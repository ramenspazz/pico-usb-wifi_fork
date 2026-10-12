// USB <-> Wi-Fi transparent layer-2 bridge. See usb_network.h.

/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Peter Lawrence
 * Copyright (c) 2024 Matthew Bennett
 *
 * influenced by lrndis https://github.com/fetisov/lrndis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <string.h>

#include <hardware/sync.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>
#include <tusb.h>

#include "debug_console.h"
#include "usb_network.h"

// The MAC reported to the host for its interface. Set to the Wi-Fi station MAC
// (usb_network_set_host_mac) so host and station share one identity and the
// bridge can forward frames unmodified.
uint8_t tud_network_mac_address[6];

// The Wi-Fi station netif we bridge to/from, and its original input handler
// (unused once intercepted, but kept for clarity/restore).
static struct netif *wifi_netif;

// A single host->Wi-Fi frame staged by tud_network_recv_cb() for the main loop.
static struct pbuf *received_frame;

// Set when tud_network_recv_cb() had to drop a datagram (pbuf pool exhausted).
// The main loop must still call tud_network_recv_renew() to re-arm the NCM OUT
// endpoint; otherwise the host->device direction wedges and the host logs
// "NETDEV WATCHDOG: transmit queue timed out".
static bool recv_stalled;

// Bridged-frame counters (see usb_network.h).
static volatile uint32_t cnt_host_to_wifi;
static volatile uint32_t cnt_wifi_to_host;
static volatile uint32_t cnt_txdrop;    // host -> Wi-Fi dropped (not associated)
static volatile uint32_t cnt_rxdrop;    // Wi-Fi -> host dropped (queue full)
static volatile uint32_t cnt_reflected; // Wi-Fi -> host dropped (host's own frame)
static volatile uint32_t cnt_poolfail;  // host -> Wi-Fi dropped (pbuf pool exhausted)
static volatile uint32_t cnt_ring_max;    // all-time high-water mark of the USB-TX ring depth
static volatile uint32_t cnt_ring_recent; // ring high-water mark since the last report (windowed)

static volatile bool dbg_log; // per-frame logging to the debug console

void usb_network_set_debug(bool on) { dbg_log = on; }

// Per-frame W>H/H>W logging floods the debug console under load (thousands of
// lines per second) and drowns the periodic stats line. Compile-time off by
// default; build with -DTRACE_FRAMES=1 to re-enable per-frame tracing.
#ifndef TRACE_FRAMES
#define TRACE_FRAMES 0
#endif

// One-line packet summary on the debug console. Called only from the main loop
// (debug_printf touches TinyUSB), so the Wi-Fi->host side logs at drain time.
static void log_pkt(const char *dir, const uint8_t *f, uint16_t len) {
  if (!TRACE_FRAMES || !dbg_log || len < 14) return;
  uint16_t eth = (uint16_t)((f[12] << 8) | f[13]);
  if (eth == 0x0800 && len >= 34) { // IPv4
    debug_printf("%s %4u IPv4 %u.%u.%u.%u > %u.%u.%u.%u proto=%u\n", dir, len,
                 f[26], f[27], f[28], f[29], f[30], f[31], f[32], f[33], f[23]);
  } else if (eth == 0x86dd && len >= 54) { // IPv6 (next header at 20)
    debug_printf("%s %4u IPv6 nexthdr=%u\n", dir, len, f[20]);
  } else if (eth == 0x0806) { // ARP
    debug_printf("%s %4u ARP\n", dir, len);
  } else {
    debug_printf("%s %4u ethertype=0x%04x\n", dir, len, eth);
  }
}

// Host address, snooped passively from outbound frames (the Pico holds no IP).
static volatile bool host_ip4_valid;
static volatile uint8_t host_ip4[4];
static volatile bool host_ip6_valid;
static volatile uint8_t host_ip6[16];

// Inspect a host->Wi-Fi frame and record the source address it advertises, so
// the console can display the single IP the host holds. Best-effort, untagged
// Ethernet only.
static void snoop_host_addr(const uint8_t *f, uint16_t len) {
  if (len < 14) return;
  uint16_t eth = (uint16_t)((f[12] << 8) | f[13]);
  if (eth == 0x0806 && len >= 28) { // ARP: sender protocol (IPv4) address at 28
    if (f[28] | f[29] | f[30] | f[31]) {
      for (int i = 0; i < 4; i++) host_ip4[i] = f[28 + i];
      host_ip4_valid = true;
    }
  } else if (eth == 0x0800 && len >= 30) { // IPv4: source address at 26
    if (f[26] | f[27] | f[28] | f[29]) {
      for (int i = 0; i < 4; i++) host_ip4[i] = f[26 + i];
      host_ip4_valid = true;
    }
  } else if (eth == 0x86dd && len >= 38) { // IPv6: source address at 22
    if ((f[22] & 0xe0) == 0x20) { // 2000::/3 global unicast (skip link-local/mcast)
      for (int i = 0; i < 16; i++) host_ip6[i] = f[22 + i];
      host_ip6_valid = true;
    }
  }
}

// Deferred USB-TX queue. Wi-Fi frames are received in the background (cyw43)
// context, but TinyUSB must only be touched from the main loop, so the Wi-Fi RX
// path just pushes frames here and usb_tx_drain() (main loop) transmits them.
#define USB_TX_QUEUE_LEN 32 // deeper ring absorbs Wi-Fi RX bursts the FS USB side cannot drain instantly
static struct pbuf *usb_tx_queue[USB_TX_QUEUE_LEN];
static volatile uint8_t usb_tx_head = 0; // producer index (push)
static volatile uint8_t usb_tx_tail = 0; // consumer index (pop, main loop only)

static bool usb_tx_push(struct pbuf *q) {
  uint32_t save = save_and_disable_interrupts();
  uint8_t next = (uint8_t)((usb_tx_head + 1) % USB_TX_QUEUE_LEN);
  bool ok = (next != usb_tx_tail);
  if (ok) {
    usb_tx_queue[usb_tx_head] = q;
    usb_tx_head = next;
    uint8_t depth = (uint8_t)((usb_tx_head + USB_TX_QUEUE_LEN - usb_tx_tail) % USB_TX_QUEUE_LEN);
    if (depth > cnt_ring_max) cnt_ring_max = depth;
    if (depth > cnt_ring_recent) cnt_ring_recent = depth;
  }
  restore_interrupts(save);
  return ok;
}

// drain queued Wi-Fi frames to the host over USB -- MAIN LOOP ONLY
static void usb_tx_drain(void) {
  while (usb_tx_tail != usb_tx_head) {
    struct pbuf *q = usb_tx_queue[usb_tx_tail];
    if (!tud_ready() || !tud_network_can_xmit(q->tot_len)) {
      break; // USB busy; try again next iteration
    }
    log_pkt("W>H", (const uint8_t *)q->payload, q->tot_len);
    tud_network_xmit(q, 0);
    uint32_t save = save_and_disable_interrupts();
    usb_tx_tail = (uint8_t)((usb_tx_tail + 1) % USB_TX_QUEUE_LEN);
    restore_interrupts(save);
    cyw43_arch_lwip_begin(); // pbuf pool is shared with the background Wi-Fi context
    pbuf_free(q);
    cyw43_arch_lwip_end();
  }
}

// --- Wi-Fi -> host -----------------------------------------------------------

// Replaces the station netif's input handler. cyw43 calls this (in the
// background context) for every received Ethernet frame, having already copied
// it into an independent PBUF_POOL pbuf. We take ownership (return ERR_OK so
// cyw43 does not free it) and queue it for the main loop to send to the host.
static err_t bridge_wifi_input(struct pbuf *p, struct netif *inp) {
  (void)inp;
  // Drop frames the host itself transmitted that the AP flooded back: their
  // source MAC is our station MAC, which the host shares. A bridge must not
  // echo a station's own frames back to it (removes multicast self-reflection).
  if (p->len >= 12 && memcmp((const uint8_t *)p->payload + 6, tud_network_mac_address, 6) == 0) {
    cnt_reflected++;
    pbuf_free(p);
    return ERR_OK;
  }
  if (usb_tx_push(p)) {
    cnt_wifi_to_host++;
  } else {
    cnt_rxdrop++;
    pbuf_free(p); // ring full: drop (we own it; cyw43 will not free on ERR_OK)
  }
  return ERR_OK;
}

// --- host -> Wi-Fi -----------------------------------------------------------

static void bridge_to_wifi(struct pbuf *p) {
  if (wifi_netif == NULL || !netif_is_link_up(wifi_netif)) {
    cnt_txdrop++;
    return; // not associated yet; drop
  }
  cyw43_arch_lwip_begin();
  cyw43_send_ethernet(&cyw43_state, CYW43_ITF_STA, p->tot_len, p, true);
  cyw43_arch_lwip_end();
  cnt_host_to_wifi++;
}

// --- TinyUSB network callbacks -----------------------------------------------

void tud_network_init_cb(void) {
  if (received_frame) {
    cyw43_arch_lwip_begin(); // pbuf pool is shared with the background Wi-Fi context
    pbuf_free(received_frame);
    cyw43_arch_lwip_end();
    received_frame = NULL;
  }
}

bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
  if (received_frame) {
    return false; // previous frame not yet forwarded; ask TinyUSB to hold off
  }
  if (size) {
    // The pbuf pool is shared with the background Wi-Fi context; all lwIP calls
    // outside an lwIP callback must hold the cyw43 lock or the free list corrupts.
    cyw43_arch_lwip_begin();
    struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
    cyw43_arch_lwip_end();
    if (p) {
      memcpy(p->payload, src, size);
      received_frame = p; // forwarded onto Wi-Fi by usb_network_update()
    } else {
      recv_stalled = true; // pool exhausted: drop, but still re-arm RX below
      cnt_poolfail++;
    }
  }
  return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
  struct pbuf *p = (struct pbuf *)ref;
  (void)arg;
  return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

// --- public api --------------------------------------------------------------

void usb_network_set_host_mac(const uint8_t mac[6]) {
  memcpy(tud_network_mac_address, mac, 6);
}

bool usb_network_init(void) {
  if (!tud_init(PICO_TUD_RHPORT)) {
    printf("usb_network: tud_init fail\n");
    return false;
  }
  return true;
}

void usb_network_attach_wifi(struct netif *sta) {
  wifi_netif = sta;
  // Intercept the station's received frames: cyw43 delivers each RX frame via
  // netif->input, which we redirect to the bridge instead of lwIP.
  cyw43_arch_lwip_begin();
  sta->input = bridge_wifi_input;
  cyw43_arch_lwip_end();
}

void usb_network_update(void) {
  tud_task();
  usb_tx_drain(); // Wi-Fi -> host

  bool renew = false;
  if (received_frame) { // host -> Wi-Fi
    snoop_host_addr((const uint8_t *)received_frame->payload, received_frame->tot_len);
    log_pkt("H>W", (const uint8_t *)received_frame->payload, received_frame->tot_len);
    bridge_to_wifi(received_frame);
    cyw43_arch_lwip_begin(); // pbuf pool is shared with the background Wi-Fi context
    pbuf_free(received_frame);
    cyw43_arch_lwip_end();
    received_frame = NULL;
    renew = true;
  }
  if (recv_stalled) { // a datagram was dropped; the OUT endpoint still needs re-arming
    recv_stalled = false;
    renew = true;
  }
  // Re-arm the NCM OUT endpoint after every consumed or dropped datagram.
  // Skipping it on the dropped path leaves the endpoint un-armed and wedges the
  // host->device direction (NETDEV WATCHDOG: transmit queue timed out).
  if (renew) tud_network_recv_renew();
}

void usb_network_get_stats(usb_bridge_stats_t *s) {
  s->host_to_wifi = cnt_host_to_wifi;
  s->wifi_to_host = cnt_wifi_to_host;
  s->txdrop = cnt_txdrop;
  s->rxdrop = cnt_rxdrop;
  s->reflected = cnt_reflected;
  s->poolfail = cnt_poolfail;
  s->ring_max = cnt_ring_max;
}

uint32_t usb_network_ring_recent_reset(void) {
  // Read and clear the windowed high-water mark so the next caller sees only the
  // peak depth reached since now -- the all-time cnt_ring_max latches at the
  // ceiling and stops being informative, this stays a live "temperature".
  uint32_t save = save_and_disable_interrupts();
  uint32_t v = cnt_ring_recent;
  cnt_ring_recent = 0;
  restore_interrupts(save);
  return v;
}

bool usb_network_host_ipv4(uint8_t ip[4]) {
  if (!host_ip4_valid) return false;
  for (int i = 0; i < 4; i++) ip[i] = host_ip4[i];
  return true;
}

bool usb_network_host_ipv6(uint8_t ip[16]) {
  if (!host_ip6_valid) return false;
  for (int i = 0; i < 16; i++) ip[i] = host_ip6[i];
  return true;
}

bool usb_network_is_up(void) {
  return tud_ready();
}

void usb_network_deinit(void) {
  tud_deinit(PICO_TUD_RHPORT);
}
