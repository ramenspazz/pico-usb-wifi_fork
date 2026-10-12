// pico-usb-wifi: a driverless USB Wi-Fi adapter.
//
// The Pico is a transparent layer-2 bridge between a USB host and a Wi-Fi
// network. The host's USB interface adopts the Wi-Fi station's MAC address, so
// there is one MAC and one IP identity end to end -- the host's. The Pico runs
// no IP of its own; it associates to the access point and shuttles Ethernet
// frames between USB and Wi-Fi (usb_network.c). Provisioning and status are on
// the CDC-ACM serial console (serial_console.c).

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <hardware/watchdog.h>
#include <lwip/netif.h>
#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>

#include "config.h"
#include "config_proto.h"
#include "debug_console.h"
#include "serial_console.h"
#include "usb_network.h"
#include "wifi_scan.h"

// Watchdog timeout. If the main loop stops feeding the watchdog for this long --
// because the firmware hard-faulted, locked up, or deadlocked in a driver -- the
// chip resets and USB re-enumerates instead of needing a physical replug. It
// comfortably exceeds any legitimate main-loop pause (a flash SAVE is a few
// milliseconds; association is asynchronous).
#define WDT_TIMEOUT_MS 4000u

// Crash telemetry that survives a watchdog reboot. The C runtime does not clear
// .uninitialized_data, so on a watchdog warm-reset (but not a cold power-on)
// these fields retain the values last written before the firmware hung. The
// magic word distinguishes a warm reboot from a cold power-on (random SRAM).
#define CRASHLOG_MAGIC 0x50574631u // "PWF1"
typedef struct {
  uint32_t magic;
  // Bridge-counter snapshot, refreshed continuously; on a hang these hold the
  // last values from just before the loop stopped.
  uint32_t host_to_wifi, wifi_to_host, txdrop, rxdrop, reflected, poolfail, ring_max;
  uint32_t boots; // boots since the last cold power-on
  uint32_t hangs; // watchdog-timeout recoveries since the last cold power-on
  uint32_t faults;       // hard-fault recoveries since the last cold power-on
  uint32_t fault_pc;     // address of the most recent hard fault
  uint32_t fault_lr;     // link register at the most recent hard fault
  uint32_t fault_pending; // set by the fault handler, consumed on the next boot
} crashlog_t;
static crashlog_t crashlog __attribute__((section(".uninitialized_data.crashlog")));

// Hard-fault handler. A crash under load otherwise just hangs until the watchdog
// times out, telling us nothing about where. This records the faulting PC/LR
// into the crashlog (which survives the reset) and reboots immediately, so the
// fault is pinned to an address we can map with addr2line. Overrides the
// pico-sdk weak isr_hardfault. If the firmware merely deadlocks (no fault) this
// never runs and only the timeout watchdog recovers -- which is itself a clue.
void __attribute__((used)) fault_capture(uint32_t pc, uint32_t lr) {
  crashlog.magic = CRASHLOG_MAGIC;
  crashlog.fault_pc = pc;
  crashlog.fault_lr = lr;
  crashlog.fault_pending = 1;
  watchdog_reboot(0, 0, 0); // immediate reset, boot from flash
  while (1) tight_loop_contents();
}

void __attribute__((naked, used)) isr_hardfault(void) {
  __asm volatile(
      "movs r0, #4         \n" // select the stack the fault frame is on
      "mov  r1, lr         \n" // (EXC_RETURN bit 2: 0 = MSP, 1 = PSP)
      "tst  r0, r1         \n"
      "beq  1f             \n"
      "mrs  r0, psp        \n"
      "b    2f             \n"
      "1:  mrs r0, msp     \n"
      "2:  ldr r1, [r0, #20] \n" // stacked LR  -> r1 (arg1)
      "    ldr r0, [r0, #24] \n" // stacked PC  -> r0 (arg0)
      "    ldr r2, =fault_capture \n"
      "    bx  r2          \n"
      "    .ltorg          \n");
}

// Tell the station to deliver all multicast frames, not just groups it has
// joined. The bridge runs no IP stack of its own, so it never joins anything;
// without this the firmware would drop the multicast that IPv6 Neighbor
// Discovery (router advertisements, DAD, address resolution) depends on, and
// the host's IPv6 would not work through the bridge. Re-applied on each
// association, since the setting does not survive a re-join.
static void wifi_enable_allmulti(void) {
  uint8_t buf[16] = {0};
  strcpy((char *)buf, "allmulti"); // iovar name, NUL-terminated at buf[8]
  uint32_t on = 1;
  memcpy(buf + 9, &on, sizeof(on)); // value follows the name+NUL
  // cyw43_ioctl encodes the command in cmd>>1 and SET in the low bit; WLC_SET_VAR
  // is 263. cyw43_ioctl does its own thread locking (not an lwIP call).
  cyw43_ioctl(&cyw43_state, (263u << 1) | 1u, 13, buf, CYW43_ITF_STA);
}

// Auth for the active profile. A password-protected profile uses WPA2/WPA3
// transition mode: the driver installs both the WPA2 PMK and the WPA3 SAE
// password, so the station joins either a WPA2 or a WPA3 access point. An empty
// password means an open network.
static uint32_t active_auth(const config_t *cfg) {
  return config_active_pass(cfg)[0] ? CYW43_AUTH_WPA3_WPA2_AES_PSK : CYW43_AUTH_OPEN;
}

// Apply Wi-Fi config immediately: drop any association and re-join with the
// active profile's credentials. Registered with the control protocol so a SET
// takes effect without a reboot. (A country change persists but fully applies
// only on the next boot, when cyw43 is initialised with it.)
static void apply_wifi(const config_t *cfg) {
  cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
  const char *ssid = config_active_ssid(cfg);
  if (ssid[0]) {
    cyw43_arch_wifi_connect_async(ssid, config_active_pass(cfg), active_auth(cfg));
  }
}

static void mac_to_str(const uint8_t mac[6], char out[18]) {
  static const char hex[] = "0123456789ABCDEF";
  int c = 0;
  for (int i = 0; i < 6; i++) {
    out[c++] = hex[(mac[i] >> 4) & 0xf];
    out[c++] = hex[mac[i] & 0xf];
    if (i < 5) out[c++] = ':';
  }
  out[c] = '\0';
}

// Free RAM: the heap region (whose top is the stack limit) minus what malloc
// currently holds out. Shown on the debug console as headroom for tuning the
// pbuf pool and buffers. __StackLimit/__bss_end__ come from the pico-sdk linker.
static uint32_t free_ram(void) {
  extern char __StackLimit, __bss_end__;
  struct mallinfo mi = mallinfo();
  return (uint32_t)(&__StackLimit - &__bss_end__) - (uint32_t)mi.uordblks;
}

// Human-readable cyw43 station link status for the debug console. This is what
// distinguishes the stuck-association causes -- a wrong password or a WPA3-only
// AP shows "badauth", an absent AP shows "nonet" -- which otherwise present only
// as a never-ending "associating".
static const char *link_reason(int s) {
  switch (s) {
    case CYW43_LINK_DOWN: return "down";
    case CYW43_LINK_JOIN: return "join";
    case CYW43_LINK_NOIP: return "noip";
    case CYW43_LINK_UP: return "up";
    case CYW43_LINK_FAIL: return "fail";
    case CYW43_LINK_NONET: return "nonet";
    case CYW43_LINK_BADAUTH: return "badauth";
    default: return "?";
  }
}

int main(void) {
  stdio_uart_init();

  config_t cfg;
  config_load(&cfg);

  // If the watchdog just rebooted us out of a hang, the crashlog still holds the
  // pre-hang counters (see crashlog_t). Snapshot them for a one-time report, then
  // refresh the boot/hang tallies. A cold power-on leaves SRAM indeterminate,
  // caught by the magic word.
  bool warm = watchdog_caused_reboot() && crashlog.magic == CRASHLOG_MAGIC;
  crashlog_t pre = crashlog; // snapshot before mutating the tallies
  bool fault_recovered = warm && pre.fault_pending;       // a hard fault was caught
  bool hang_recovered = warm && !pre.fault_pending;       // timeout, no fault = deadlock
  if (crashlog.magic != CRASHLOG_MAGIC) {
    memset(&crashlog, 0, sizeof(crashlog));
    crashlog.magic = CRASHLOG_MAGIC;
  }
  crashlog.boots++;
  if (fault_recovered) crashlog.faults++;
  if (hang_recovered) crashlog.hangs++;
  crashlog.fault_pending = 0; // consumed

  // cyw43_arch_init also initialises lwIP (the cyw43 station netif lives there);
  // the USB side is a raw bridge and uses no lwIP interface of its own.
  if (cyw43_arch_init_with_country(cfg.country)) {
    printf("cyw43_arch_init failed\n");
    return -1;
  }
  cyw43_arch_enable_sta_mode();
  struct netif *sta = &cyw43_state.netif[CYW43_ITF_STA];

  // The host adopts the Wi-Fi station's MAC so the bridge can forward frames
  // unmodified: one MAC, one association, one IP identity (the host's).
  uint8_t smac[6];
  cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, smac);
  char macstr[18];
  mac_to_str(smac, macstr);
  printf("Wi-Fi station MAC %s; host adopts it\n", macstr);
  usb_network_set_host_mac(smac);

  if (!usb_network_init()) {
    printf("failed to start usb network\n");
    return -1;
  }
  usb_network_attach_wifi(sta); // intercept station RX -> USB

  serial_console_init(&cfg);
  config_proto_set_apply(apply_wifi); // SET/RESTORE re-associate immediately

  // Associate (non-blocking) so USB stays serviced during the join. The bridge
  // drops host frames until the link is up; the host's DHCP simply retries.
  if (config_active_ssid(&cfg)[0] == '\0') {
    printf("no Wi-Fi SSID configured; provision over the serial console\n");
  } else {
    printf("associating to '%s' ...\n", config_active_ssid(&cfg));
    cyw43_arch_wifi_connect_async(config_active_ssid(&cfg), config_active_pass(&cfg),
                                  active_auth(&cfg));
  }

  printf("setup complete, bridging\n");

  // Start the watchdog now that initialisation (which can outlast the timeout)
  // is done; the loop below pets it every iteration.
  watchdog_enable(WDT_TIMEOUT_MS, true);

  int key = 0;
  uint32_t last_led = 0;
  uint32_t last_join = 0;
  uint32_t last_dbg = 0;
  bool was_up = false;
  bool led_on = false; // last LED state, so breathing only writes the SPI LED on a change
  bool hang_reported = false;
  while ((key != 's') && (key != 'S')) {
    watchdog_update();      // a hung loop stops petting the watchdog -> auto-reset
    usb_network_update();   // shuttle frames both directions
    serial_console_task();  // CDC-ACM management console
    debug_console_task();   // CDC-ACM debug console (drain input)
    usb_network_set_debug(cfg.debug_enabled); // gate per-frame packet logging

    // One-time notice that the watchdog recovered us, with the counters from
    // just before the crash. A fault also carries the faulting PC/LR. Best-effort
    // (the stats line below repeats faults/faultpc durably).
    if ((fault_recovered || hang_recovered) && !hang_reported && usb_network_is_up()) {
      hang_reported = true;
      if (fault_recovered) {
        debug_printf("RECOVERED from HARD FAULT pc=0x%08lx lr=0x%08lx (fault #%lu, boot #%lu). "
                     "Pre: ->wifi=%lu ->host=%lu txdrop=%lu rxdrop=%lu refl=%lu poolfail=%lu ringmax=%lu\n",
                     (unsigned long)pre.fault_pc, (unsigned long)pre.fault_lr,
                     (unsigned long)crashlog.faults, (unsigned long)crashlog.boots,
                     (unsigned long)pre.host_to_wifi, (unsigned long)pre.wifi_to_host,
                     (unsigned long)pre.txdrop, (unsigned long)pre.rxdrop,
                     (unsigned long)pre.reflected, (unsigned long)pre.poolfail,
                     (unsigned long)pre.ring_max);
        printf("RECOVERED from HARD FAULT pc=0x%08lx lr=0x%08lx\n",
               (unsigned long)pre.fault_pc, (unsigned long)pre.fault_lr);
      } else {
        debug_printf("RECOVERED from hang (watchdog timeout, no fault; hang #%lu, boot #%lu). "
                     "Pre: ->wifi=%lu ->host=%lu txdrop=%lu rxdrop=%lu refl=%lu poolfail=%lu ringmax=%lu\n",
                     (unsigned long)crashlog.hangs, (unsigned long)crashlog.boots,
                     (unsigned long)pre.host_to_wifi, (unsigned long)pre.wifi_to_host,
                     (unsigned long)pre.txdrop, (unsigned long)pre.rxdrop,
                     (unsigned long)pre.reflected, (unsigned long)pre.poolfail,
                     (unsigned long)pre.ring_max);
        printf("RECOVERED from hang via watchdog (hang #%lu)\n", (unsigned long)crashlog.hangs);
      }
    }

    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Periodic bridge stats on the debug console, only when enabled.
    if (cfg.debug_enabled && (now - last_dbg >= 2000u)) {
      last_dbg = now;
      usb_bridge_stats_t s;
      usb_network_get_stats(&s);
      uint8_t ip4[4];
      char ips[16] = "none";
      if (usb_network_host_ipv4(ip4)) {
        snprintf(ips, sizeof(ips), "%u.%u.%u.%u", ip4[0], ip4[1], ip4[2], ip4[3]);
      }
      // "up" is the netif link (association), since the IP-unaware
      // cyw43_wifi_link_status reports JOIN even once fully associated; its value
      // is meaningful only as the failure reason while not yet associated.
      const char *link = netif_is_link_up(sta)
                             ? "up"
                             : link_reason(cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA));
      debug_printf("stats: ->wifi=%lu ->host=%lu txdrop=%lu rxdrop=%lu refl=%lu "
                   "poolfail=%lu ringpk=%lu link=%s hangs=%lu faults=%lu faultpc=0x%08lx "
                   "freeram=%lu host=%s\n",
                   (unsigned long)s.host_to_wifi, (unsigned long)s.wifi_to_host,
                   (unsigned long)s.txdrop, (unsigned long)s.rxdrop,
                   (unsigned long)s.reflected, (unsigned long)s.poolfail,
                   (unsigned long)usb_network_ring_recent_reset(), link,
                   (unsigned long)crashlog.hangs,
                   (unsigned long)crashlog.faults, (unsigned long)crashlog.fault_pc,
                   (unsigned long)free_ram(), ips);
    }

    // LED:
    //   solid              = associated (bridging)
    //   slow blink (1 Hz)  = configured, associating
    //   fast blink (5 Hz)  = no Wi-Fi configured -> provision it
    //   double-flash       = live (continuous) scan, disassociated
    //   off                = USB not ready
    // The double-flash is evaluated every loop iteration (the SPI-driven LED is
    // written only on a state change) so its short pulses are crisp; the steady
    // states are re-evaluated at ~20 Hz.
    if (config_proto_contscan_active()) {
      // Two short flashes ~150 ms apart, then a ~1.7 s pause -- a heartbeat.
      uint32_t t = now % 2000u;
      bool on = usb_network_is_up() && ((t < 70u) || (t >= 220u && t < 290u));
      if (on != led_on) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
        led_on = on;
      }
    } else if (now - last_led >= 50u) {
      last_led = now;

      // Refresh the crash snapshot so a watchdog reboot can report the state
      // from just before a hang (see crashlog_t).
      usb_bridge_stats_t cs;
      usb_network_get_stats(&cs);
      crashlog.host_to_wifi = cs.host_to_wifi;
      crashlog.wifi_to_host = cs.wifi_to_host;
      crashlog.txdrop = cs.txdrop;
      crashlog.rxdrop = cs.rxdrop;
      crashlog.reflected = cs.reflected;
      crashlog.poolfail = cs.poolfail;
      crashlog.ring_max = cs.ring_max;

      bool usb_ok = usb_network_is_up();
      // "Associated" is the netif link, not cyw43_tcpip_link_status: the latter
      // only reaches LINK_UP once the station has an IP, and the station never
      // takes one (the host holds the address).
      bool wifi_link = netif_is_link_up(sta);
      bool on;
      if (!usb_ok) {
        on = false;
      } else if (wifi_link) {
        on = true;
      } else if (config_active_ssid(&cfg)[0] == '\0') {
        on = (now % 200u) < 100u;
      } else {
        on = (now % 1000u) < 500u;
      }
      cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
      led_on = on; // keep in sync for the breathing branch's change-detection

      // Announce association edges on the debug console; re-issue connect if
      // dropped. The management console stays quiet except in reply to commands.
      if (wifi_link && !was_up) {
        wifi_enable_allmulti(); // needed for the host's IPv6 to bridge
        if (cfg.debug_enabled) debug_printf("associated to %s\n", config_active_ssid(&cfg));
      } else if (!wifi_link && was_up) {
        if (cfg.debug_enabled) debug_printf("Wi-Fi link down\n");
      }
      was_up = wifi_link;
      // Re-issue the join if the link is down -- but not while a scan is running,
      // or the new join would abort the scan (and the scan already cleared a
      // stuck association before starting).
      if (!wifi_link && config_active_ssid(&cfg)[0] && !wifi_scan_in_progress() &&
          (now - last_join >= 10000u)) {
        last_join = now;
        cyw43_arch_wifi_connect_async(config_active_ssid(&cfg), config_active_pass(&cfg),
                                      active_auth(&cfg));
      }
    }

    key = getchar_timeout_us(0);
  }

  printf("shutting down\n");
  usb_network_deinit();
  cyw43_arch_deinit();
  return 0;
}
