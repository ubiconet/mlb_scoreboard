#pragma once

// Serial
static const uint32_t SERIAL_BAUD_RATE = 115200;
static const char* FIRMWARE_VERSION = "v2.24";

// Compile-time debug log gate. Set to 0 in production builds to drop the
// per-tick [DISPLAY]/[API CALL] printf noise (a Serial.printf at 115200 baud
// stalls the loop ~25 ms — measurable against the 5s poll cadence). Enable by
// uncommenting or passing -DMLB_DEBUG=1 in build_flags during development.
#ifndef MLB_DEBUG
#define MLB_DEBUG 1   // temporarily enabled to diagnose "no upcoming games"
#endif
#if MLB_DEBUG
#define DBG_PRINTF(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#else
#define DBG_PRINTF(fmt, ...) do {} while (0)
#endif

// How long the boot splash (MLB SCOREBOARD logo) stays up before network setup begins.
static const uint32_t BOOT_SPLASH_HOLD_MS = 15000;

// 2.0-inch ST7789V TFT (320x240, SPI)
static const int TFT_SCLK_PIN = 13;
static const int TFT_MOSI_PIN = 12;
static const int TFT_CS_PIN = 9;
static const int TFT_DC_PIN = 10;
static const int TFT_RESET_PIN = 11;
static const int TFT_BACKLIGHT_PIN = -1;
static const int TFT_NATIVE_WIDTH = 240;
static const int TFT_NATIVE_HEIGHT = 320;

// MAX7219 8x8 LED Matrix displays (2 cascaded modules: 0=Away, 1=Home)
static const int MAX7219_DIN_PIN = 14;
static const int MAX7219_CLK_PIN = 8;
static const int MAX7219_CS_PIN = 16;

// Discrete Count LEDs (GPIO pins)
static const int BALL_3_PIN = 1;
static const int BALL_2_PIN = 2;
static const int BALL_1_PIN = 3;
static const int STRIKE_2_PIN = 4;
static const int STRIKE_1_PIN = 5;
static const int OUT_2_PIN = 6;
static const int OUT_1_PIN = 7;

// Network fallback access point and portal login.
static const char* NETWORK_AP_SSID = "MLB_SCOREBOARD";
static const char* NETWORK_AP_PASSWORD = "score1234";
static const char* NETWORK_PORTAL_PASSWORD = "score";
static const char* NETWORK_HOSTNAME = "mlb-scoreboard";
// Network (Arduino) OTA: used by `pio run -t upload --upload-port <ip>` during development.
static const char* NETWORK_OTA_PASSWORD = "score1234";
// Connectivity probe: any HTTP response from this anchor means the uplink works.
static const char* NETWORK_PROBE_ANCHOR_URL = "http://connectivitycheck.gstatic.com/generate_204";
static const uint32_t NETWORK_CONNECT_AND_PROBE_TIMEOUT_MS = 30000; // assoc + internet probe budget
static const uint32_t NETWORK_PROBE_INTERVAL_MS = 5000; 
static const uint32_t NETWORK_RECONNECT_GRACE_MS = 15000; // sustained drop before re-provisioning
static const uint32_t NETWORK_RECONNECT_RETRY_MS = 5000;
static const uint32_t NETWORK_DEBUG_INTERVAL_MS = 10000;
static const uint32_t NETWORK_SETUP_SCREEN_MS = 15000;    // grace after going online before scoreboard
static const uint32_t NETWORK_SCAN_REFRESH_MS = 30000;    // portal scan-list cache age
static const uint32_t NETWORK_PROVISIONING_RETRY_MS = 60000; // retry saved Wi-Fi after this long in AP mode
// Minimum time the Wi-Fi setup/connecting screen stays up on the very first boot connect,
// so it isn't just a flash when a saved network connects almost instantly. Only applies
// once at boot; later reconnects after a drop are not artificially delayed.
static const uint32_t NETWORK_FIRST_CONNECT_MIN_MS = 15000;

// POSIX timezone used to convert MLB's UTC gameDate values to the local clock.
// Change this for the location where the scoreboard is installed.
static const char* LOCAL_TIMEZONE = "EST5EDT,M3.2.0,M11.1.0";

// MLB Live Polling
static const uint32_t MLB_LIVE_POLL_INTERVAL_MS = 5000;  // 5 second live linescore tick
static const uint32_t MLB_SCHEDULE_POLL_INTERVAL_MS = 60000; // Detect followed-game start/end within 1 min
static const uint32_t MLB_SCHEDULE_RETRY_MS = 15000;   // Base retry while the last schedule fetch failed (flaky Wi-Fi)
static const uint32_t MLB_RETRY_BACKOFF_MAX_MS = 120000; // Exponential backoff cap for failed fetch retries.
// After the first couple of connections following boot, new TLS connections
// start failing instantly (start_ssl_client: -1) with the radio still
// associated and heap healthy — consistent with the AP's flood protection
// and/or leaked lwIP PCBs from failed handshakes. Backing off exponentially
// (per ADR-0002) instead of retrying every 15 s lets those windows expire.
static const uint32_t MLB_NTP_READY_RETRY_MS = 5000;     // short retry while awaiting first time sync
static const uint32_t MLB_POSTGAME_GRACE_MS = 300000;    // Keep final followed game visible for 5 min
static const uint32_t MLB_AT_BAT_RESULT_DISPLAY_MS = 4000; // Full-screen result card duration
static const uint32_t MLB_CAROUSEL_ROTATE_MS = 5000;      // Rotate live-game stat ticker every 5s
static const uint32_t MLB_UPCOMING_GAMES_ROTATE_MS = 5000;  // Show each upcoming-game card for 5s
// News ticker pacing. The software-SPI band push is the hard limit: the
// measured per-frame cost of repainting the 320x36 text band is ~180 ms
// (see the [TICKER] serial line), i.e. ~5.5 fps, regardless of FRAME_MS.
// The smoothest motion at that frame rate steps about ONE character cell
// (24 px for size-4 text) per frame, so the rate is set to
// 24 px / 0.18 s ~= 144 px/s. Larger values make multi-character jumps;
// smaller ones waste frame time. The scroll offset itself is derived from
// wall-clock time in rotateCarousel() so loop jitter can't double-step.
static const uint32_t MLB_NEWS_TICKER_FRAME_MS = 25;   // min ms between ticker frames
static const int MLB_NEWS_TICKER_PX_PER_SEC = 144;     // ~1 char cell per 180 ms frame
static const uint32_t MLB_NEWS_CACHE_TTL_MS = 30UL * 60UL * 1000UL; // Refresh ESPN news every 30 min
static const uint32_t MLB_NEWS_RETRY_MS     = 60UL * 1000UL;       // Retry failed news fetch every 60s until first success. Keep this gentle: ESPN's edge starts rejecting TLS handshakes (fatal alerts) from clients that retry every few seconds.

// Firmware self-update. `pio run -t deploy` writes the binary + manifest to
// releases/ in the GitHub repo; the device polls the manifest after boot
// and flashes itself when the version differs from FIRMWARE_VERSION.
// raw.githubusercontent.com only serves HTTPS — the one TLS connection the
// firmware still makes (feeds themselves run over plain HTTP; see
// mlb_client.cpp). It runs alone on core 0 with the statsapi session
// closed, so the ~45 KB TLS context fits.
static const char* OTA_MANIFEST_URL =
    "https://raw.githubusercontent.com/ubiconet/mlb_scoreboard/main/"
    "releases/manifest.json";
static const uint32_t OTA_CHECK_DELAY_MS = 90000; // let Wi-Fi, feeds, and clock settle first
static const uint32_t OTA_CHECK_INTERVAL_MS = 12UL * 60UL * 60UL * 1000UL; // recheck
static const uint32_t OTA_CHECK_RETRY_MS = 30UL * 60UL * 1000UL;  // retry failed checks
static const uint32_t OTA_DOWNLOAD_STALL_MS = 30000;  // abort a download with no progress

