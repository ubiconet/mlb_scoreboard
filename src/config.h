#pragma once

// ============================================================================
// Composition root + device/repo identity
// ============================================================================
// This header composes the full build configuration from three layers:
//
//   1. THIS FILE           — identity of this particular repo/device install:
//                            firmware version, timezone, self-update URLs.
//   2. common/config.h     — framework defaults shared by every scoreboard
//                            built on this template (debug gate, boot/network
//                            timing, OTA pacing).
//   3. sport_config.h      — the selected sport's profile (pins, branding,
//                            feed cadences, layout constants). Resolved from
//                            the src/sports/<sport>/ folder that each
//                            PlatformIO env puts on the include path — see
//                            the -I flag in platformio.ini.
//
// Every source file keeps including plain "config.h" and sees the union of
// all three, exactly like the pre-template single config.h did.

// ---- Firmware identity -----------------------------------------------------
// tools/release_deploy.py reads FIRMWARE_VERSION from THIS file to name the
// release binary, so the definition must stay here.
static const char* FIRMWARE_VERSION = "v2.87";

// ---- Install location (factory default) -------------------------------------
// POSIX TZ string used ONLY until the user picks a timezone in the setup
// portal (the selection is persisted in NVS key "tz" and returned by
// network_service's getTzString()). It drives every displayed time: the
// idle clock on the score matrices, upcoming-game times/countdowns, and the
// schedule-day window the feed fetch uses. Change this to pre-provision a
// board's first-boot timezone; afterwards the portal owns it.
// TEMPLATE CHECKLIST: set to the new install's zone when forking.
static const char* FACTORY_DEFAULT_TIMEZONE = "EST5EDT,M3.2.0,M11.1.0";

// ---- Firmware self-update endpoints ----------------------------------------
// `pio run -t deploy` publishes each release to BOTH sports-miniboards.com
// (the Netlify-hosted sales site's /firmware/mlb/ folder — fast CDN,
// no-cache on the manifest) and the GitHub releases/ folder (the
// archive/manual-download copy). The device polls the SITE copy (same
// flow as the NHL board, v2.84): OTA_MANIFEST_URL below and the binary
// URL inside the site manifest both point at sports-miniboards.com.
// Both connections are TLS; checks run boot-only on the data task, where
// the N8R8 board's PSRAM heap funds the handshake beside the render
// canvas. A found update parks the display (canvas freed) for the binary
// download — see common/comms/ota_update.cpp.
// TEMPLATE CHECKLIST: point these at the new repo/site when forking for
// a new sport (and update RAW_BASE + SITE_SLUG in tools/release_deploy.py).
static const char* OTA_MANIFEST_URL =
    "https://sports-miniboards.com/firmware/mlb/manifest.json";
// Displayed on the setup portal so a user doing a manual update knows
// where the current binary lives. Must stay in sync with the site
// manifest above (same folder, deployed by `pio run -t deploy`).
static const char* OTA_LATEST_BIN_URL =
    "https://sports-miniboards.com/firmware/mlb/mlb_scoreboard_latest.bin";

// ---- Support contact -------------------------------------------------------
// Linked from the Support section of the setup portal.
static const char* SUPPORT_EMAIL = "sportsminiboards@gmail.com";
static const char* SUPPORT_WEBSITE_URL = "https://sports-miniboards.com/";

#include "common/config.h"
#include "sport_config.h"
