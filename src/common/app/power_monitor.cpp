#include <Arduino.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "config.h"
#include "common/app/power_monitor.h"
#include "common/hal/count_leds.h"
#include "common/hal/led_matrix.h"
#include "common/hal/tft_panel.h"
#include "common/ui/low_battery_screen.h"

namespace power {
namespace {

// Survives deep sleep and soft/brownout resets while power holds; zeroed by
// a true power-on. Flags "we chose to sleep for low battery", so a timer
// wake knows to re-check silently instead of doing storm bookkeeping.
RTC_DATA_ATTR bool sBatterySleep = false;

Preferences prefs;

// --- NVS boot journal (namespace "power") ---
//   died:     1 = the previous boot started but never reached healthy
//             uptime (brownout/collapse/user power-off), 2 = deliberate
//             low-battery sleep, 0 = clean previous session.
//   badboots: consecutive bad boots (brownout reset reason, or died == 1).
constexpr const char* kPrefsNamespace = "power";
constexpr const char* kKeyDied = "died";
constexpr const char* kKeyBadBoots = "badboots";

float readBatteryVolts() {
  if (!BATTERY_SENSE_ENABLED) return 0.0f;
  // 11 dB attenuation maps the divider's 0-2.1 V output (half of the pack's
  // 3.0-4.2 V) into the ADC's ~0-3.1 V window. Median-of-5 rejects the boost
  // converter's ripple and single-sample ADC glitches.
  analogSetPinAttenuation(BATTERY_SENSE_PIN, ADC_11db);
  uint32_t samples[5];
  for (int i = 0; i < 5; ++i) {
    samples[i] = analogReadMilliVolts(BATTERY_SENSE_PIN);
    delay(2);
  }
  for (int i = 1; i < 5; ++i) {  // insertion sort, n = 5
    uint32_t key = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > key) {
      samples[j + 1] = samples[j];
      --j;
    }
    samples[j + 1] = key;
  }
  return samples[2] * BATTERY_SENSE_DIVIDER / 1000.0f;
}

void journalDied(uint8_t value) {
  prefs.begin(kPrefsNamespace, false);
  prefs.putUChar(kKeyDied, value);
  prefs.end();
}

// ---- Runtime gauge state (core-0 writer, render-core reader) ----
// Float/uint8 writes are single aligned stores, so the render core can
// sample them without a lock — same discipline as the fetch diagnostics.
float sRuntimeVolts = 0.0f;    // last runtime sample; 0 = unknown
float sSmoothedVolts = 0.0f;   // EMA of samples; 0 = none yet
uint8_t sRuntimeQuarters = 0xFF;
uint8_t sRuntimePercent = 0xFF;
uint32_t sLastMonitorAt = 0;

// State of charge is a straight voltage mapping over the pack's usable
// window (user-specified levels): BATTERY_LOW_VOLTS (3.4 V) = empty,
// BATTERY_FULL_VOLTS (4.18 V and above — the charger's termination
// voltage) = full. Voltage-only fuel gauging is inherently coarse — the
// boost converter's load sags the pack ~0.1-0.2 V (under-reading while
// running) — but the linear scale keeps the gauge predictable and
// matches the quarter steps exactly; a true gauge needs a fuel-gauge IC.
uint8_t voltsToPercent(float v) {
  float span = BATTERY_FULL_VOLTS - BATTERY_LOW_VOLTS;
  float pct = (v - BATTERY_LOW_VOLTS) / span * 100.0f + 0.5f;
  if (pct < 0.0f) return 0;
  if (pct > 100.0f) return 100;
  return (uint8_t)pct;
}

// Quarter segments bucket the SAME percent scale: 4/4 spans 75-100% (a
// completed charge holds "full" until the discharge crosses 75%,
// ~3.99 V), then 3/4 / 2/4 at the 50% / 25% lines (~3.79 / ~3.60 V),
// 1/4 down to the empty floor. An explicit threshold ladder (not a
// divided float) keeps the exact boundaries on the lit side; the icon
// and the percentage switch buckets at the same voltage.
uint8_t voltsToQuarters(float v) {
  const float seg = (BATTERY_FULL_VOLTS - BATTERY_LOW_VOLTS) / 4.0f;
  if (v >= BATTERY_FULL_VOLTS - seg) return 4;        // 75-100%: full
  if (v >= BATTERY_FULL_VOLTS - 2.0f * seg) return 3; // 50-75%
  if (v >= BATTERY_FULL_VOLTS - 3.0f * seg) return 2; // 25-50%
  if (v > BATTERY_LOW_VOLTS) return 1;                // above empty
  return 0;                                           // at/below 3.4 V
}

// Post-read state update shared by the paced monitor and the on-demand
// board-test read: plausibility guard (a 1S LiPo never sits above ~4.3 V —
// higher means the divider taps the boost output or the pin floats; hide
// the gauge rather than show a lying "full") plus the EMA that steadies
// the charge estimate across samples. batteryVolts() stays raw so the
// diagnostics page can display the real number.
void applyRuntimeSample() {
  if (sRuntimeVolts > 0.5f && sRuntimeVolts <= 4.30f) {
    // EMA across the samples steadies the charge estimate; a fast alpha
    // (0.25) still tracks a real discharge over minutes.
    sSmoothedVolts = (sSmoothedVolts <= 0.0f)
                         ? sRuntimeVolts
                         : sSmoothedVolts + 0.25f * (sRuntimeVolts - sSmoothedVolts);
    sRuntimePercent = voltsToPercent(sSmoothedVolts);
    sRuntimeQuarters = voltsToQuarters(sSmoothedVolts);
  } else {
    sSmoothedVolts = 0.0f;  // resync when a trustworthy reading returns
    sRuntimePercent = 0xFF;
    sRuntimeQuarters = 0xFF;
  }
  static uint8_t sLastLoggedQuarters = 0xFE;
  if (sRuntimeQuarters != sLastLoggedQuarters) {
    sLastLoggedQuarters = sRuntimeQuarters;
    if (sRuntimeQuarters <= 4) {
      DBG_PRINTF("[POWER] battery %.2f V -> %u%% (%u/4)\n", sRuntimeVolts,
                 (unsigned)sRuntimePercent, (unsigned)sRuntimeQuarters);
    } else {
      DBG_PRINTF("[POWER] battery %.2f V -> implausible, gauge hidden\n",
                 sRuntimeVolts);
    }
  }
}

}  // namespace

BootDecision earlyBootCheck() {
  // Timer wake from our own low-battery sleep: re-check the pack and stay
  // down silently (no display work) until the charger resumes it. With no
  // divider wired the voltage can't be verified, so the re-check always
  // keeps sleeping — recovery is a power cycle.
  bool timerWake = sBatterySleep &&
                   esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  if (timerWake) {
    if (BATTERY_SENSE_ENABLED && readBatteryVolts() >= BATTERY_RESUME_VOLTS) {
      sBatterySleep = false;  // charger present — resume a normal boot
      journalDied(0);
      return BootDecision::NORMAL;
    }
    return BootDecision::RESLEEP;
  }

  // Storm bookkeeping — journal FIRST, sense second: a cycle that dies
  // seconds later still counted toward the streak.
  prefs.begin(kPrefsNamespace, false);
  uint8_t died = prefs.getUChar(kKeyDied, 0);
  uint8_t badBoots = prefs.getUChar(kKeyBadBoots, 0);
  bool badBoot = (esp_reset_reason() == ESP_RST_BROWNOUT) || (died == 1);
  if (badBoot) {
    badBoots = (badBoots < 255) ? badBoots + 1 : badBoots;
  } else {
    badBoots = 0;
  }
  prefs.putUChar(kKeyDied, 1);  // assume short-lived until proven healthy
  prefs.putUChar(kKeyBadBoots, badBoots);
  prefs.end();

  bool low = badBoots >= BATTERY_BAD_BOOT_STREAK;
  if (BATTERY_SENSE_ENABLED) {
    float volts = readBatteryVolts();
    if (volts > 0.5f && volts <= BATTERY_LOW_VOLTS) low = true;
  }
  return low ? BootDecision::SHUTDOWN : BootDecision::NORMAL;
}

void sleepNow() {
  // Journal the deliberate sleep BEFORE anything slow: if the pack collapses
  // during the warning hold, the next boot must not count this session
  // toward the bad-boot streak.
  sBatterySleep = true;
  journalDied(2);
  esp_sleep_enable_timer_wakeup((uint64_t)BATTERY_RECHECK_MS * 1000ULL);
  esp_deep_sleep_start();  // no return
}

void showLowBatteryAndSleep() {
  float volts = readBatteryVolts();  // 0.0 when no divider is wired
  DBG_PRINTF("[POWER] low battery (%.2f V) — warning, then deep sleep\n", volts);
  renderLowBatteryWarning(volts, 2500);

  // Dark outputs: the matrices and count LEDs would otherwise keep drawing
  // (and showing stale state) through the sleep.
  setMax7219Scores(MAX7219_SCORE_BLANK, MAX7219_SCORE_BLANK);
  setCountLeds(0, 0, 0);

  // Blank the panel and put it into its low-power state — the ST7789 keeps
  // scanning GRAM (mA-range) unless told to sleep. The backlight LED line
  // (wired since v2.76) goes dark too; boards without firmware backlight
  // control keep it lit until the switch is flipped — power off after
  // seeing the warning when possible.
  GFXcanvas16& canvas = tftPanel.canvas();
  canvas.fillScreen(COLOR_BG);
  tftPanel.pushFull();
  tftPanel.setBacklight(false);
  tftPanel.raw().sendCommand(0x10);  // SLPIN

  sleepNow();
}

void noteHealthyUptime() {
  if (millis() < BATTERY_HEALTHY_BOOT_MS) return;
  static bool marked = false;
  if (marked) return;
  marked = true;
  prefs.begin(kPrefsNamespace, false);
  if (prefs.getUChar(kKeyDied, 0) == 1) prefs.putUChar(kKeyDied, 0);
  prefs.end();
}

// ---- Runtime gauge (see power_monitor.h) ----

bool batteryGaugeAvailable() {
  // ADC1 = GPIO 1-10 on ESP32-S3. ADC2 pins die with Wi-Fi, so a divider
  // there supports only the boot-time gate, never this gauge.
  return BATTERY_SENSE_ENABLED && BATTERY_SENSE_PIN >= 1 &&
         BATTERY_SENSE_PIN <= 10;
}

void serviceBatteryMonitor() {
  if (!batteryGaugeAvailable()) return;  // no Wi-Fi-safe divider
  uint32_t now = millis();
  if (sLastMonitorAt != 0 &&
      now - sLastMonitorAt < BATTERY_MONITOR_INTERVAL_MS) {
    return;
  }
  sLastMonitorAt = now;
  sRuntimeVolts = readBatteryVolts();
  applyRuntimeSample();
}

float readBatteryNow() {
  // Immediate sample for the diagnostics board test — bypasses the pacing
  // so a just-built board can be verified the moment the button is
  // pressed. Runs on the network task like the paced monitor (the
  // median-of-5 read takes ~10 ms).
  if (!batteryGaugeAvailable()) return 0.0f;
  sLastMonitorAt = millis();
  sRuntimeVolts = readBatteryVolts();
  applyRuntimeSample();
  return sRuntimeVolts;
}

float batteryVolts() { return sRuntimeVolts; }
uint8_t batteryQuarters() { return sRuntimeQuarters; }
uint8_t batteryPercent() { return sRuntimePercent; }

}  // namespace power
