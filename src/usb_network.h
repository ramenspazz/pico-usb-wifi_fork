// USB <-> Wi-Fi transparent layer-2 bridge.
//
// The device is a CDC-NCM peripheral whose host adopts the Wi-Fi station's MAC
// address, so the Pico is a dumb L2 bridge: Ethernet frames are shuttled
// verbatim between the USB link and the Wi-Fi station, touching nothing above
// layer 2. The host runs its own IP stack and holds the single IP identity
// (IPv4 and IPv6 alike); the Pico holds no IP of its own. See net_bridge in the
// architecture notes.

#ifndef USB_NETWORK_H
#define USB_NETWORK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

struct netif;

// Set the MAC the host's interface will adopt (the Wi-Fi station MAC). Must be
// called before usb_network_init(), as the descriptor reports it at enumeration.
void usb_network_set_host_mac(const uint8_t mac[6]);

// Initialise TinyUSB in bridge mode (no lwIP interface on the USB side).
bool usb_network_init(void);

// Begin bridging: intercept the Wi-Fi station's received frames toward USB, and
// remember the station so host frames can be sent onto Wi-Fi. Call once the
// station netif exists (after cyw43_arch_enable_sta_mode()).
void usb_network_attach_wifi(struct netif *sta);

// Pump the bridge: service USB, drain Wi-Fi->host frames to USB, and forward any
// host->Wi-Fi frame. Main loop only.
void usb_network_update(void);

bool usb_network_is_up(void);
void usb_network_deinit(void);

// Bridged-frame counters, for the debug console.
typedef struct {
  uint32_t host_to_wifi; // frames forwarded host -> Wi-Fi
  uint32_t wifi_to_host; // frames forwarded Wi-Fi -> host
  uint32_t txdrop;       // host -> Wi-Fi frames dropped (not associated)
  uint32_t rxdrop;       // Wi-Fi -> host frames dropped (USB queue full)
  uint32_t reflected;    // Wi-Fi -> host frames dropped (host's own, reflected)
  uint32_t poolfail;     // host -> Wi-Fi frames dropped (pbuf pool exhausted)
  uint32_t ring_max;     // all-time high-water mark of the Wi-Fi -> host USB-TX ring
} usb_bridge_stats_t;
void usb_network_get_stats(usb_bridge_stats_t *s);

// Peak USB-TX ring depth since the previous call, then reset: a live "recent"
// gauge for the debug console, where the all-time ring_max latches at the
// ceiling. Call once per debug-stats interval.
uint32_t usb_network_ring_recent_reset(void);

// Enable per-frame packet logging on the debug console (gated by `SET debug on`).
void usb_network_set_debug(bool on);

// Address the host has been seen using, snooped passively from bridged frames
// (the Pico holds no IP itself). Each returns true and fills the buffer if an
// address has been observed. ip6 reports the host's most recent global address.
bool usb_network_host_ipv4(uint8_t ip[4]);
bool usb_network_host_ipv6(uint8_t ip[16]);

#ifdef __cplusplus
}
#endif

#endif // USB_NETWORK_H
