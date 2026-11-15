#pragma once

#include <Arduino.h>

// Power monitor — the LiPo + boost supply's defense AND gauge (battery
// constants in the sport's sport_config.h).
//
// Boot gate (this board's device behavior — the NHL board has no gate):
//   voltage:  a divider on BATTERY_SENSE_PIN is read at power-on and
//             at/below BATTERY_LOW_VOLTS the board flashes the warning and
//             sleeps immediately. While asleep it silently re-checks every
//             BATTERY_RECHECK_MS and resumes on its own once the charger
//             lifts the pack past BATTERY_RESUME_VOLTS.
//   backstop: with no divider wired, an NVS boot journal counts rapid bad
//             boots (brownout resets / sessions that never reached
//             BATTERY_HEALTHY_BOOT_MS of uptime); a streak of
//             BATTERY_BAD_BOOT_STREAK trips the same path. Recovery then
//             needs a power cycle after charging.
//
// Runtime gauge (NHL-board feature, v2.75): serviceBatteryMonitor()
// samples the divider every BATTERY_MONITOR_INTERVAL_MS on the core-0
// data task and feeds a smoothed estimate read by the on-screen battery
// indicator and the portal's diagnostics page. It needs the divider on an
// ADC1 pin — ADC2 is claimed by Wi-Fi, so a divider there (the old GPIO 17
// placeholder) can only ever serve the boot gate. batteryGaugeAvailable()
// reports whether the configured pin supports the runtime gauge.

namespace power {

enum class BootDecision {
  NORMAL,    // proceed with the boot
  RESLEEP,   // timer-wake recheck found the pack still low: stay asleep
  SHUTDOWN,  // low battery: flash the warning and shut down
};

// Runs at the very top of setup(), before the display or Wi-Fi come up.
// Cheap and deliberately first: during a low-battery boot storm some cycles
// die within seconds, and the journal/battery reading must happen even then.
BootDecision earlyBootCheck();

// Enter (or re-enter) the low-battery deep sleep. Never returns.
void sleepNow();

// Flash the low-battery warning on the TFT, blank the matrices and count
// LEDs, then deep-sleep. Never returns. The display must already be
// initialized (call after sport::setup()).
void showLowBatteryAndSleep();

// Call every loop() pass: once the session passes BATTERY_HEALTHY_BOOT_MS
// of uptime it marks the boot journal healthy, so a later normal power-off
// doesn't count toward the bad-boot streak.
void noteHealthyUptime();

// ---- Runtime gauge ----

// True when the configured divider pin can be sampled while Wi-Fi runs
// (ADC1). False hides the indicator and the diagnostics readout.
bool batteryGaugeAvailable();

// Sample the pack and update the runtime gauge (paced; no-op on boards
// without a gauge-capable divider). Call from the core-0 data task loop.
void serviceBatteryMonitor();

// Immediate sample that bypasses the pacing interval, for the diagnostics
// board test (runs on the network task like the paced monitor). Returns
// the raw volts — 0.0 when the board has no gauge-capable divider.
float readBatteryNow();

// Last runtime sample. 0.0 when unknown (no divider / no sample yet).
float batteryVolts();
// Coarse charge level for the on-screen gauge: 0..4 quarter segments, or
// 255 when unknown — callers hide the indicator then, so boards without
// the divider never show a gauge.
uint8_t batteryQuarters();
// Estimated remaining charge, 0..100 percent, linear over the smoothed
// pack voltage; 255 = unknown.
uint8_t batteryPercent();

}  // namespace power
