// Runtime configuration control protocol, transport-agnostic. See config_proto.h.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h> // strtol
#include <string.h>
#include <strings.h>

#include <lwip/netif.h>
#include <pico/cyw43_arch.h> // CYW43_COUNTRY(...) / cyw43_state

#include "config.h"
#include "config_proto.h"
#include "usb_network.h" // snooped host address for the status dump
#include "wifi_scan.h"

#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev" // overridden at build time from the git tag
#endif

static inline void out(const cfg_io_t *io, const char *s) { io->write(io->ctx, s); }

static config_apply_fn g_apply; // re-applies Wi-Fi config live (registered by main)

void config_proto_set_apply(config_apply_fn cb) { g_apply = cb; }

// Which menu the console is in. MODE_SCAN is entered by the SCAN command and
// left by BACK or a successful JOIN. MODE_CONTSCAN is the live (continuous) scan,
// entered by LIVE and left by any keypress.
static enum { MODE_MAIN, MODE_SCAN, MODE_CONTSCAN } g_mode;

// A single `scan` unions several passive passes -- one pass catches only a subset
// of the beacons present -- accumulating into the results before the list shows.
#define SCAN_PASSES 4
static uint8_t scan_rounds; // passes remaining in a single-scan session, 0 = idle

void config_proto_reset(void) {
  g_mode = MODE_MAIN;
  scan_rounds = 0;
  wifi_scan_stop(); // drop any scan latch so association can resume
}

// --- helpers ------------------------------------------------------------------

static uint32_t parse_country(const char *v) {
  if (strcasecmp(v, "WORLDWIDE") == 0) {
    return CYW43_COUNTRY_WORLDWIDE;
  }
  if (strlen(v) >= 2) {
    return CYW43_COUNTRY(toupper((unsigned char)v[0]), toupper((unsigned char)v[1]), 0);
  }
  return CYW43_COUNTRY_WORLDWIDE;
}

static void fmt_ip6(char *out16, const uint8_t ip[16]) {
  char *p = out16;
  for (int i = 0; i < 8; i++) {
    uint16_t g = (uint16_t)((ip[2 * i] << 8) | ip[2 * i + 1]);
    p += sprintf(p, "%x%s", g, i < 7 ? ":" : "");
  }
}

// Format the regulatory country, guarding against the unset/zero case whose
// bytes would be NUL (which truncates the output string).
static void fmt_country(char *o, size_t n, uint32_t country) {
  char a = (char)(country & 0xff), b = (char)((country >> 8) & 0xff);
  if (a >= 0x20 && a < 0x7f && b >= 0x20 && b < 0x7f) {
    snprintf(o, n, "%c%c", a, b);
  } else {
    snprintf(o, n, "(unset)");
  }
}

// Copy an SSID for display, replacing non-printable bytes with '?', so a name
// with control bytes or an exotic encoding cannot corrupt the terminal. The
// numbered lists exist precisely so such networks are still selectable.
static void fmt_ssid(char *o, size_t n, const char *ssid) {
  size_t i = 0;
  for (; ssid[i] && i + 1 < n; i++) {
    unsigned char c = (unsigned char)ssid[i];
    o[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
  }
  o[i] = '\0';
}

// Parse a 1-based list index argument into a 0-based index in [0,count), or -1.
static int parse_index(const char *s, int count) {
  char *end;
  long v = strtol(s, &end, 10);
  if (end == s || *end != '\0' || v < 1 || v > count) {
    return -1;
  }
  return (int)v - 1;
}

// --- state dump (printed after every main-menu command) -----------------------

void config_proto_dump(const cfg_io_t *io, const config_t *cfg) {
  bool assoc = netif_is_link_up(&cyw43_state.netif[CYW43_ITF_STA]);
  char line[80], cc[8], ssid[CONFIG_SSID_MAX];

  out(io, "\n-- pico-usb-wifi " FW_VERSION " --\n");
  if (cfg->active < cfg->profile_count) {
    snprintf(line, sizeof(line), "    profiles:   %u saved (active: %u)\n",
             cfg->profile_count, (unsigned)(cfg->active + 1));
  } else {
    snprintf(line, sizeof(line), "    profiles:   %u saved (active: none)\n", cfg->profile_count);
  }
  out(io, line);

  const char *as = config_active_ssid(cfg);
  fmt_ssid(ssid, sizeof(ssid), as);
  snprintf(line, sizeof(line), "    ssid:       %s\n", as[0] ? ssid : "(unset)");
  out(io, line);
  out(io, config_active_pass(cfg)[0] ? "    pass:       set\n" : "    pass:       unset\n");
  fmt_country(cc, sizeof(cc), cfg->country);
  snprintf(line, sizeof(line), "    country:    %s\n", cc);
  out(io, line);
  out(io, cfg->debug_enabled ? "    debug:      on\n" : "    debug:      off\n");
  out(io, assoc ? "    status:     associated (host holds the IP)\n"
                : "    status:     associating\n");

  uint8_t ip4[4];
  if (usb_network_host_ipv4(ip4)) {
    snprintf(line, sizeof(line), "    host IPv4:  %u.%u.%u.%u\n", ip4[0], ip4[1], ip4[2], ip4[3]);
    out(io, line);
  } else {
    out(io, "    host IPv4:  (not seen yet)\n");
  }
  uint8_t ip6[16];
  if (usb_network_host_ipv6(ip6)) {
    char b[44];
    fmt_ip6(b, ip6);
    snprintf(line, sizeof(line), "    host IPv6:  %s\n", b);
    out(io, line);
  }
}

// --- list views ---------------------------------------------------------------

static void list_profiles(const cfg_io_t *io, const config_t *cfg) {
  char line[96], ssid[CONFIG_SSID_MAX];
  if (cfg->profile_count == 0) {
    out(io, "    (no saved profiles -- set ssid/pass or scan to add one)\n");
    return;
  }
  snprintf(line, sizeof(line), "    profiles (%u/%u):\n", cfg->profile_count, CONFIG_PROFILE_MAX);
  out(io, line);
  for (uint8_t i = 0; i < cfg->profile_count; i++) {
    fmt_ssid(ssid, sizeof(ssid), cfg->profiles[i].ssid);
    snprintf(line, sizeof(line), "      %u%c %s%s\n", i + 1,
             i == cfg->active ? '*' : ' ', ssid,
             cfg->profiles[i].password[0] ? "" : "  (open)");
    out(io, line);
  }
  out(io, "    (* = active)\n");
}

static void list_scan(const cfg_io_t *io) {
  char line[96], ssid[CONFIG_SSID_MAX];
  uint8_t n = wifi_scan_count();
  if (n == 0) {
    out(io, "    (no networks found -- scan to try again)\n");
  } else {
    snprintf(line, sizeof(line), "    networks (%u):\n", n);
    out(io, line);
    for (uint8_t i = 0; i < n; i++) {
      const wifi_scan_entry_t *e = wifi_scan_get(i);
      fmt_ssid(ssid, sizeof(ssid), e->ssid);
      snprintf(line, sizeof(line),
               "      %2u  %-20s  %4d dBm  %02x:%02x:%02x:%02x:%02x:%02x\n", i + 1,
               ssid, e->rssi, e->bssid[0], e->bssid[1], e->bssid[2], e->bssid[3],
               e->bssid[4], e->bssid[5]);
      out(io, line);
    }
  }
}

// Continuous-scan line: signal, channel, and BSSID (MAC) alongside the SSID --
// enough to identify each access point as the stream scrolls. No index; the
// numbered list for joining is reprinted when the stream stops.
static void stream_scan(const cfg_io_t *io) {
  char line[96], ssid[CONFIG_SSID_MAX];
  for (uint8_t i = 0; i < wifi_scan_count(); i++) {
    const wifi_scan_entry_t *e = wifi_scan_get(i);
    fmt_ssid(ssid, sizeof(ssid), e->ssid);
    snprintf(line, sizeof(line),
             "  %-20s  %4d dBm  ch %2u  %02x:%02x:%02x:%02x:%02x:%02x\n", ssid,
             e->rssi, e->channel, e->bssid[0], e->bssid[1], e->bssid[2],
             e->bssid[3], e->bssid[4], e->bssid[5]);
    out(io, line);
  }
}

static void start_scan(const cfg_io_t *io) {
  if (wifi_scan_start()) {
    g_mode = MODE_SCAN;
    scan_rounds = SCAN_PASSES;
    out(io, "[*] scanning"); // a dot is appended as each pass completes
  } else {
    out(io, "[!] scan failed to start\n");
  }
}

// --- SET ----------------------------------------------------------------------

enum { SET_ERR, SET_WIFI, SET_OTHER }; // error / changed Wi-Fi / changed other

static int cmd_set(const cfg_io_t *io, char *args, config_t *cfg) {
  char *val = strchr(args, ' ');
  if (val == NULL) {
    out(io, "ERR usage: set <ssid|pass|country|debug> <value>\n");
    return SET_ERR;
  }
  *val++ = '\0'; // args is now the key, val the remainder of the line

  if (strcasecmp(args, "ssid") == 0) {
    // Edit the active profile's SSID, or create the first profile if none.
    if (cfg->active < cfg->profile_count) {
      strncpy(cfg->profiles[cfg->active].ssid, val, CONFIG_SSID_MAX - 1);
      cfg->profiles[cfg->active].ssid[CONFIG_SSID_MAX - 1] = '\0';
    } else {
      int i = config_add_profile(cfg, val);
      if (i < 0) {
        out(io, "ERR profile list full -- del one first\n");
        return SET_ERR;
      }
      cfg->active = (uint8_t)i;
    }
    return SET_WIFI;
  } else if (strcasecmp(args, "pass") == 0) {
    if (cfg->active >= cfg->profile_count) {
      out(io, "ERR no active profile -- set ssid first\n");
      return SET_ERR;
    }
    strncpy(cfg->profiles[cfg->active].password, val, CONFIG_PASS_MAX - 1);
    cfg->profiles[cfg->active].password[CONFIG_PASS_MAX - 1] = '\0';
    return SET_WIFI;
  } else if (strcasecmp(args, "country") == 0) {
    cfg->country = parse_country(val);
    return SET_WIFI;
  } else if (strcasecmp(args, "debug") == 0) {
    if (strcasecmp(val, "on") == 0) {
      cfg->debug_enabled = 1;
      return SET_OTHER;
    } else if (strcasecmp(val, "off") == 0) {
      cfg->debug_enabled = 0;
      return SET_OTHER;
    }
    out(io, "ERR usage: set debug <on|off>\n");
    return SET_ERR;
  }
  out(io, "ERR unknown key (ssid|pass|country|debug)\n");
  return SET_ERR;
}

// --- menu handlers ------------------------------------------------------------

static void handle_main(const cfg_io_t *io, char *cmd, char *args, config_t *cfg) {
  if (strcasecmp(cmd, "SET") == 0 && args) {
    int r = cmd_set(io, args, cfg);
    if (r == SET_ERR) {
      return; // error already printed
    }
    if (r == SET_WIFI && g_apply) {
      out(io, "[*] applying -- re-associating\n");
      g_apply(cfg); // re-associate with the new credentials immediately
    } else if (r == SET_OTHER) {
      out(io, "[*] updated\n");
    }
    config_proto_dump(io, cfg);
  } else if (strcasecmp(cmd, "LIST") == 0) {
    list_profiles(io, cfg);
  } else if (strcasecmp(cmd, "USE") == 0 && args) {
    int i = parse_index(args, cfg->profile_count);
    if (i < 0) {
      out(io, "ERR usage: use <n> (see list)\n");
      return;
    }
    cfg->active = (uint8_t)i;
    if (g_apply) {
      out(io, "[*] applying -- re-associating\n");
      g_apply(cfg);
    }
    config_proto_dump(io, cfg);
  } else if (strcasecmp(cmd, "DEL") == 0 && args) {
    int i = parse_index(args, cfg->profile_count);
    if (i < 0) {
      out(io, "ERR usage: del <n> (see list)\n");
      return;
    }
    config_del_profile(cfg, i);
    out(io, "[*] deleted (save to persist)\n");
    if (g_apply) {
      g_apply(cfg); // active profile may have changed
    }
    config_proto_dump(io, cfg);
  } else if (strcasecmp(cmd, "SCAN") == 0) {
    start_scan(io);
  } else if (strcasecmp(cmd, "SAVE") == 0) {
    out(io, config_save(cfg) ? "[*] saved to flash\n" : "[!] save failed\n");
    config_proto_dump(io, cfg);
  } else if (strcasecmp(cmd, "RESTORE") == 0) {
    config_load(cfg); // discard unsaved edits: reload the saved (or default) record
    out(io, "[*] restored saved settings\n");
    if (g_apply) {
      g_apply(cfg);
    }
    config_proto_dump(io, cfg);
  } else {
    out(io, "[!] commands: set <ssid|pass|country|debug> <val> | list | use <n> | "
            "del <n> | scan | save | restore\n");
  }
}

static void handle_scan(const cfg_io_t *io, char *cmd, char *args, config_t *cfg) {
  if (strcasecmp(cmd, "BACK") == 0) {
    g_mode = MODE_MAIN;
    config_proto_dump(io, cfg);
  } else if (strcasecmp(cmd, "SCAN") == 0) {
    start_scan(io); // re-run the scan
  } else if (strcasecmp(cmd, "LIVE") == 0) {
    // Continuous scan: stream access points until any key is pressed, staying
    // disassociated for the duration.
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    if (wifi_scan_start()) {
      g_mode = MODE_CONTSCAN;
      out(io, "[*] live scan -- press any key to stop\n");
    } else {
      out(io, "[!] scan failed to start\n");
    }
  } else if (strcasecmp(cmd, "JOIN") == 0 && args) {
    int i = parse_index(args, wifi_scan_count());
    if (i < 0) {
      out(io, "ERR usage: join <n> (see the list)\n");
      return;
    }
    const wifi_scan_entry_t *e = wifi_scan_get((uint8_t)i);
    int p = config_find_profile(cfg, e->ssid); // reuse an existing profile if any
    if (p < 0) {
      p = config_add_profile(cfg, e->ssid);
      if (p < 0) {
        out(io, "ERR profile list full -- back then del one\n");
        return;
      }
    }
    cfg->active = (uint8_t)p;
    g_mode = MODE_MAIN;
    out(io, "[*] staged -- set pass <password> then save\n");
    if (g_apply) {
      g_apply(cfg); // open networks associate now; secured wait for the password
    }
    config_proto_dump(io, cfg);
  } else {
    out(io, "[!] scan: back | join <n> | scan | live\n");
  }
}

// --- line handling ------------------------------------------------------------

void config_proto_handle_line(const cfg_io_t *io, char *line, config_t *cfg) {
  size_t n = strlen(line);
  while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) {
    line[--n] = '\0';
  }
  if (n == 0) { // bare Enter reprints the current view
    if (g_mode == MODE_SCAN) {
      list_scan(io);
    } else {
      config_proto_dump(io, cfg);
    }
    return;
  }

  char *args = strchr(line, ' ');
  if (args != NULL) {
    *args++ = '\0'; // line is the command word, args the remainder
  }

  if (g_mode == MODE_SCAN) {
    handle_scan(io, line, args, cfg);
  } else {
    handle_main(io, line, args, cfg);
  }
}

bool config_proto_scanning(void) {
  return wifi_scan_in_progress() || g_mode == MODE_CONTSCAN || scan_rounds > 0;
}

void config_proto_poll(const cfg_io_t *io, config_t *cfg) {
  (void)cfg;
  if (g_mode == MODE_CONTSCAN) {
    if (wifi_scan_poll()) {
      stream_scan(io); // a pass finished: print it...
    }
    if (!wifi_scan_in_progress()) {
      wifi_scan_start(); // ...and keep a fresh pass running
    }
    return;
  }

  // Single scan: union SCAN_PASSES passive passes before showing the list.
  bool done = wifi_scan_poll(); // also clears a stray latch if scan_rounds == 0
  if (scan_rounds == 0) {
    return;
  }
  if (done && --scan_rounds > 0) {
    out(io, "."); // a pass finished, more to go
  }
  if (scan_rounds > 0) {
    if (!wifi_scan_in_progress()) {
      wifi_scan_again(); // next accumulating pass
    }
  } else if (g_mode == MODE_SCAN) {
    out(io, "\n");
    list_scan(io);
    out(io, config_proto_prompt());
  }
}

const char *config_proto_prompt(void) {
  if (g_mode == MODE_CONTSCAN) {
    return ""; // streaming; any key stops it, so no prompt
  }
  if (g_mode == MODE_SCAN) {
    // Same shape as the main prompt: the verbs in parentheses before the '#'.
    return wifi_scan_in_progress() ? "" : "(back|join|scan|live) # ";
  }
  return "(set|scan|list|use|save) # ";
}

bool config_proto_contscan_active(void) { return g_mode == MODE_CONTSCAN; }

void config_proto_contscan_stop(const cfg_io_t *io, config_t *cfg) {
  (void)cfg;
  wifi_scan_stop();   // abandon the in-flight pass: no further auto-print
  scan_rounds = 0;
  g_mode = MODE_SCAN; // back to the scan submenu with the last results
  out(io, "\n[*] stopped\n");
  list_scan(io);
}
