// Persistent device configuration.
//
// Holds the runtime-configurable settings (a list of saved Wi-Fi credential
// profiles plus regulatory country) in a single struct loaded from the last
// flash sector at boot and written back on demand. If flash holds no valid
// config (first boot, or a corrupted/old layout), the compile-time defaults from
// wifi_config.h are used.
//
// The bridge runs no IP of its own, so there is no addressing to configure --
// only what is needed to join the access point.

#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>

// Bumped from the v1 single-credential layout (0x50574946 "PWIF"): a stored v1
// record now fails this magic check and falls back to defaults. No migration --
// after upgrading, the device boots unprovisioned and is reconfigured once.
#define CONFIG_MAGIC 0x32465750u // "PWF2" (little-endian), marks a valid v2 record
#define CONFIG_SSID_MAX 33 // 32 chars + NUL
#define CONFIG_PASS_MAX 64 // 63 chars + NUL
#define CONFIG_PROFILE_MAX 8 // saved credential sets (bounded by the flash sector)
#define CONFIG_ACTIVE_NONE 0xFFu // cfg->active when no profile is selected

// One saved network's credentials.
typedef struct {
  char ssid[CONFIG_SSID_MAX];
  char password[CONFIG_PASS_MAX]; // WPA2-PSK passphrase ("" = open network)
} wifi_profile_t;

// The full configuration record, kept in the last flash sector. A change to this
// layout just invalidates any record already in flash (the CRC/magic no longer
// match), and the compile-time defaults are used until the next SAVE -- there is
// no migration, by design.
typedef struct {
  uint32_t magic; // CONFIG_MAGIC

  uint8_t profile_count;  // number of valid entries in profiles[] (0..CONFIG_PROFILE_MAX)
  uint8_t active;         // index of the profile in use, or CONFIG_ACTIVE_NONE
  uint8_t debug_enabled;  // 1 = stream diagnostics on the second serial port
  uint8_t _pad;           // keep the following fields aligned
  uint32_t country;       // CYW43 country code (e.g. CYW43_COUNTRY_WORLDWIDE)
  wifi_profile_t profiles[CONFIG_PROFILE_MAX];

  uint32_t crc32; // CRC-32 over every preceding byte of this struct; MUST be last
} config_t;

// Fill cfg with the compile-time defaults (from wifi_config.h).
void config_defaults(config_t *cfg);

// Load cfg from flash. Falls back to config_defaults() if the stored record is
// absent, has the wrong magic, or fails its CRC.
void config_load(config_t *cfg);

// Compute the CRC and write cfg to the last flash sector. Returns false if the
// flash operation did not complete. Infrequent by design (a config change).
bool config_save(config_t *cfg);

// --- active-profile accessors (never NULL; "" when no profile is selected) ----

const char *config_active_ssid(const config_t *cfg);
const char *config_active_pass(const config_t *cfg);

// --- profile list operations --------------------------------------------------

// Index of the profile whose SSID matches, or -1 if none.
int config_find_profile(const config_t *cfg, const char *ssid);

// Append a profile {ssid, ""} and return its index, or -1 if the list is full.
int config_add_profile(config_t *cfg, const char *ssid);

// Remove profile i, shifting later entries down and fixing up `active`.
void config_del_profile(config_t *cfg, int i);

#endif // CONFIG_H
