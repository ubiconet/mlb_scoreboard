#pragma once

#include <Arduino.h>   // uint32_t & friends — config headers are self-sufficient

// ============================================================================
// MLB scoreboard — sport + device profile.
// ============================================================================
// This file (named sport_config.h in every sport folder) is what the generic
// framework in src/common/ composes in via the include path each PlatformIO
// env sets up. It carries everything that changes between sports AND between
// physical builds: display pins/geometry, branding strings, feed poll
// cadences, and UI layout constants.
//
// Starting a new sport from this template: copy src/sports/mlb to
// src/sports/<sport>, edit this file, and point the env's -I flag and
// build_src_filter at the new folder (see README "new sport" checklist).

// ---- 2.0-inch ST7789V TFT (320x240, software SPI) --------------------------
// The 5-arg software-SPI constructor is deliberate: on ESP32-S3 the 3-arg
// hardware-SPI variant binds SPI.begin() to the variant's default VSPI pins
// instead of these, leaving a blank screen. See main.cpp's display comment.
static const int TFT_SCLK_PIN = 13;
static const int TFT_MOSI_PIN = 12;
static const int TFT_CS_PIN = 9;
static const int TFT_DC_PIN = 10;
static const int TFT_RESET_PIN = 11;
static const int TFT_BACKLIGHT_PIN = -1;
static const int TFT_NATIVE_WIDTH = 240;
static const int TFT_NATIVE_HEIGHT = 320;

// ---- MAX7219 8x8 LED Matrix displays (2 cascaded modules: 0=Away, 1=Home) --
static const int MAX7219_DIN_PIN = 14;
static const int MAX7219_CLK_PIN = 8;
static const int MAX7219_CS_PIN = 16;

// ---- Discrete Count LEDs (GPIO pins; balls/strikes/outs) --------------------
static const int BALL_3_PIN = 1;
static const int BALL_2_PIN = 2;
static const int BALL_1_PIN = 3;
static const int STRIKE_2_PIN = 4;
static const int STRIKE_1_PIN = 5;
static const int OUT_2_PIN = 6;
static const int OUT_1_PIN = 7;

// ---- Branding (AP network name, hostname, portal title) ---------------------
static const char* NETWORK_AP_SSID = "BASEBALL_SCOREBOARD";
static const char* NETWORK_HOSTNAME = "baseball-scoreboard";

// ---- Battery power (LiPo + boost converter supply) --------------------------
// The board runs off a 3.7 V LiPo through a 5 V boost converter. When the
// pack runs low the boost rail collapses under load and the board brown-out
// cycles on/off with no indication. The power gate (common/app/power_monitor,
// driven from main.cpp's boot sequence) handles it two ways:
//   * BATTERY_SENSE_ENABLED = false (no divider wired yet): a backstop
//     counts rapid bad boots (brownout resets / boots that died before
//     BATTERY_HEALTHY_BOOT_MS of uptime); BATTERY_BAD_BOOT_STREAK trips in
//     a row trigger the low-battery screen + deep sleep. Recovery needs a
//     power cycle after charging.
//   * BATTERY_SENSE_ENABLED = true (divider wired, see below): the pack
//     voltage is read at boot — the ONLY time ADC2 is free, Wi-Fi claims
//     it afterwards — and at/below BATTERY_LOW_VOLTS the board shows the
//     low-battery screen and deep-sleeps immediately (no cycling first).
//     While asleep it silently re-checks every BATTERY_RECHECK_MS and
//     resumes on its own once the charger lifts the pack past
//     BATTERY_RESUME_VOLTS.
// Divider wiring: BAT+ (after the switch / boost input +) --[470k]--
// BATTERY_SENSE_PIN --[470k]-- GND, with 100 nF across the lower resistor.
static const bool BATTERY_SENSE_ENABLED = false;   // flip once divider wired
static const int BATTERY_SENSE_PIN = 17;           // ADC2_CH6, free on this build
static const float BATTERY_SENSE_DIVIDER = 2.0f;   // (R1 + R2) / R2
static const float BATTERY_LOW_VOLTS = 3.40f;      // boot reading that trips shutdown
static const float BATTERY_RESUME_VOLTS = 3.90f;   // re-check resume (charger present)
static const uint32_t BATTERY_RECHECK_MS = 900000; // silent re-check while asleep (15 min)
static const uint8_t BATTERY_BAD_BOOT_STREAK = 3;  // bad boots before the backstop trips
static const uint32_t BATTERY_HEALTHY_BOOT_MS = 90000; // uptime that marks a boot healthy

// ---- UI theme (RGB565) -------------------------------------------------------
// Shared by the sport renderer AND the generic boot/OTA screens (they include
// config.h). A new sport overrides these to re-skin the whole UI.
static const uint16_t COLOR_BG = 0x0821;        // Dark slate
static const uint16_t COLOR_CARD = 0x18A5;      // Card background
static const uint16_t COLOR_GOLD = 0xFD20;      // Gold accent
static const uint16_t COLOR_YELLOW = 0xFFE0;    // Count / runner yellow
static const uint16_t COLOR_MUTED = 0x7BEF;     // Gray text/icon
static const uint16_t COLOR_LED_RED = 0xF800;   // Bright red for LED dot display
static const uint16_t COLOR_LED_OFF = 0x2100;   // Dark unlit LED dot background

// ---- MLB feed polling + UI timing -------------------------------------------
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
static const uint32_t MLB_AT_BAT_RESULT_DISPLAY_MS = 5000; // Full-screen result card duration
static const uint32_t MLB_CAROUSEL_ROTATE_MS = 5000;      // Rotate live-game stat ticker every 5s
