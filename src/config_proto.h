// Runtime configuration control protocol, transport-agnostic.
//
// A small line protocol that reads and mutates the live config_t, applies
// changes immediately, and persists on demand. It is driven over the CDC-ACM
// management console (serial_console.c) via an output sink; the protocol never
// touches a USB endpoint directly.
//
// Up to CONFIG_PROFILE_MAX credential profiles are stored; one is "active" and
// the device associates with it. `set ssid`/`set pass` edit the active profile.
//
// Every command prints the full device state (settings + live status), so there
// is no separate "show" command. An empty line reprints it. Command words are
// case-insensitive; the console shows them in lower case.
//
// Main commands (one per line):
//   set ssid <text...>    set the active profile's SSID (applied immediately)
//   set pass <text...>    set the active profile's password, WPA2 (applied)
//   set country <CC|WORLDWIDE>   set the regulatory country
//   set debug <on|off>    stream diagnostics on the second serial port
//   list                  list the saved profiles
//   use <n>               make profile n active and re-associate
//   del <n>               delete profile n
//   scan                  scan for nearby networks (enters the scan submenu)
//   save                  persist the current settings to flash
//   restore               discard unsaved changes (reload the saved settings)
//
// Scan submenu (after scan, once results are listed):
//   join <n>              stage scanned network n as the active profile
//   scan                  scan again
//   live                  continuous scan: stream APs until any key (disassociated)
//   back                  return to the main menu

#ifndef CONFIG_PROTO_H
#define CONFIG_PROTO_H

#include "config.h"

// Output sink. write() emits a short, NUL-terminated string to the transport;
// the transport owns any buffering and flushing.
typedef struct {
  void (*write)(void *ctx, const char *s);
  void *ctx;
} cfg_io_t;

// Called after a command changes the live config, to apply it immediately (e.g.
// re-associate Wi-Fi). Registered once by the application.
typedef void (*config_apply_fn)(const config_t *cfg);
void config_proto_set_apply(config_apply_fn cb);

// Process one complete line (without its newline). The buffer is mutated in
// place (tokenised). Call under the lwIP lock.
void config_proto_handle_line(const cfg_io_t *io, char *line, config_t *cfg);

// True while a scan is in flight and config_proto_poll() must be driven. Cheap
// and lock-free, so the console can avoid taking the cyw43 lock every tick.
bool config_proto_scanning(void);

// Drive a pending asynchronous scan: when results arrive, print the numbered
// list and the scan prompt. Call once per console tick (while
// config_proto_scanning() is true), under the lwIP lock.
void config_proto_poll(const cfg_io_t *io, config_t *cfg);

// The prompt for the current menu (main vs scan); "" while a scan is running.
const char *config_proto_prompt(void);

// Return to the main menu (e.g. when a fresh terminal attaches), dropping any
// scan in progress.
void config_proto_reset(void);

// True while the live (continuous) scan is streaming. The console turns any
// keypress into a stop; main uses it for the breathing LED and to hold off
// association.
bool config_proto_contscan_active(void);

// Stop the live scan and return to the scan submenu, printing the last results.
void config_proto_contscan_stop(const cfg_io_t *io, config_t *cfg);

// Print the full device state: settings plus live association and host address.
void config_proto_dump(const cfg_io_t *io, const config_t *cfg);

#endif // CONFIG_PROTO_H
