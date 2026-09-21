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
static const char* FIRMWARE_VERSION = "v2.59";

// ---- Install location ------------------------------------------------------
// POSIX timezone used to convert feed UTC gameDate values to the local clock.
// Change this for the location where the scoreboard is installed.
static const char* LOCAL_TIMEZONE = "EST5EDT,M3.2.0,M11.1.0";

// ---- Firmware self-update endpoints ----------------------------------------
// `pio run -t deploy` writes the binary + manifest to releases/ in this
// GitHub repo; the device polls the manifest after boot and flashes itself
// when the version is strictly newer than FIRMWARE_VERSION.
// raw.githubusercontent.com only serves HTTPS — the one TLS connection the
// firmware still makes (feeds themselves run over plain HTTP; see
// sports/mlb/mlb_client.cpp). It runs alone on core 0 with the feed session
// closed, so the ~45 KB TLS context fits.
// TEMPLATE CHECKLIST: point these at the new repo when forking for a new
// sport (and update RAW_BASE + LATEST_FILE in tools/release_deploy.py).
static const char* OTA_MANIFEST_URL =
    "https://raw.githubusercontent.com/ubiconet/mlb_scoreboard/main/"
    "releases/manifest.json";
// Displayed on the setup portal so a user doing a manual update knows
// where the current binary lives. Must stay in sync with the manifest
// URL above (same folder, deployed by `pio run -t deploy`).
static const char* OTA_LATEST_BIN_URL =
    "https://raw.githubusercontent.com/ubiconet/mlb_scoreboard/main/"
    "releases/mlb_scoreboard_latest.bin";

#include "common/config.h"
#include "sport_config.h"
