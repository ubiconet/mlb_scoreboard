// ============================================================================
// Generic scoreboard app shell.
// ============================================================================
// This file is sport-agnostic: it owns the boot sequence (splash hold,
// connecting screen, firmware-update screen, boot status page), network
// bring-up, and the one-shot NTP sync, then hands every loop pass to the
// sport implementation through the contract in common/app/sport_api.h.
// Which sport is compiled in is decided by platformio.ini
// (build_src_filter + include path pointing at src/sports/<sport>).

#include <Arduino.h>
#include <time.h>

#include "config.h"
#include "common/app/health.h"
#include "common/app/power_monitor.h"
#include "common/app/sport_api.h"
#include "common/comms/network_service.h"
#include "common/hal/count_leds.h"
#include "common/ui/boot_status.h"
#include "common/ui/connect_screen.h"
#include "common/ui/ota_screen.h"

void setup() {
  Serial.begin(SERIAL_BAUD_RATE);

  // Power gate first, before anything heavy (see power_monitor.h): the
  // battery reading and the bad-boot journal must happen even on cycles
  // that die seconds later, and the divider's ADC2 pin is only free
  // before Wi-Fi starts.
  power::BootDecision powerDecision = power::earlyBootCheck();
  if (powerDecision == power::BootDecision::RESLEEP) {
    power::sleepNow();  // silent recheck found the pack still low; no return
  }

  // Sport bring-up FIRST: hardware init with the sport's pins, boot
  // tests, and the boot splash (its logo artwork belongs to the sport).
  // The splash paints within moments of power-on — nothing in front of
  // it, not even the USB CDC grace period below.
  // (No onboard-RGB pin-low here: this board's GPIO 48 — the WS2812 data
  // line on the devkit — is driven as the OUT 2 count LED instead, and a
  // driven line keeps the onboard RGB dark anyway.)
  sport::setup();

  if (powerDecision == power::BootDecision::SHUTDOWN) {
    // Flash LOW BATTERY, blank the outputs, deep-sleep; no return.
    power::showLowBatteryAndSleep();
  }

  // Read the stored crash summary now, while only this task runs — every
  // later caller (/status on the network task) gets the cached copy.
  health::lastCrashJson();

  // No blocking hold here: network services start immediately and connect
  // behind the logo. loop() enforces the minimum splash time instead, so
  // a fast Wi-Fi handshake can't cut the logo short.
  NetworkBranding branding = sport::branding();
  size_t teamOptionCount = 0;
  size_t soundOptionCount = 0;
  startNetworkServices(branding, sport::teamOptions(teamOptionCount),
                       teamOptionCount, sport::defaultPreferredTeams(),
                       sport::soundOptions(soundOptionCount),
                       soundOptionCount, sport::defaultSoundId(),
                       sport::feedDiagnosticsJson);
  startNetworkTask();
  sport::startDataTask();  // core-0 feed fetches

  // Native USB CDC enumeration grace period — placed AFTER the splash and
  // network start (it used to sit first and delayed both by up to 3 s);
  // now it only guards the boot banner below. UART builds skip it
  // instantly (Serial is ready immediately there).
  uint32_t start = millis();
  while (!Serial && (millis() - start < 3000)) {
    delay(10);
  }

  // Boot banner — visible over Serial whenever SB_DEBUG=1 so a freshly
  // uploaded firmware can be confirmed at a glance. The reset reason tells
  // a power-on apart from a crash reboot ("panic", "task-watchdog").
  Serial.println();
  Serial.printf("[BOOT] FW=%s build=%s %s reset=%s\n",
                FIRMWARE_VERSION, __DATE__, __TIME__,
                health::resetReasonName());
  Serial.printf("[BOOT] last crash: %s\n", health::lastCrashJson().c_str());
  Serial.printf("[BOOT] heap_free=%u heap_min=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap());
}

void loop() {
  // Marks this session healthy once it has run long enough that a later
  // power-off must not count toward the power gate's bad-boot streak.
  power::noteHealthyUptime();

  // Network-down indicator: the sport's chosen LEDs (all seven count LEDs
  // on the MLB board) flash together until the connection is up — from
  // power-on, through a dropped link's reconnect, and in setup mode. The
  // portal's overrides (demo, manual mode, display tests) drive those LEDs
  // themselves, so it yields to them. Decided here every pass, ahead of
  // the splash hold, so it starts with the boot.
  setCountLedsBlink(!networkLinkUp() && !portalDisplayOverrideActive(),
                    NETWORK_DOWN_BLINK_COUNT_A, NETWORK_DOWN_BLINK_COUNT_B,
                    NETWORK_DOWN_BLINK_COUNT_C, NETWORK_DOWN_BLINK_MS);

  // Boot sequence: (1) the logo splash holds BOOT_SPLASH_HOLD_MS while
  // the network task connects Wi-Fi behind it; (2) a connection still in
  // flight after that shows the connecting screen (network name, current
  // step, time left before setup mode) — the same screen any later
  // reconnect gets; (3) an in-progress firmware update owns the screen
  // whenever it runs; (4) once the connection is made, the status/setup
  // page shows for BOOT_SETUP_PAGE_MS; (5) if the connection cannot be
  // made, the AP provisioning page ("connect to the scoreboard") shows
  // instead and holds until the portal establishes the connection — then
  // the status page runs its window. The sport UI owns every frame after
  // that; network/update/game-data work runs on core 0 throughout.
  uint32_t bootNow = millis();
  if (bootNow < BOOT_SPLASH_HOLD_MS) {
    return;
  }
  if (handleOtaUpdateScreen()) {
    return;
  }
  // Anchor the setup page to the moment the network actually came up, so
  // a slow handshake eats into the splash instead of the page.
  static uint32_t onlineSinceMs = 0;
  if (onlineSinceMs == 0 && isOnline()) {
    onlineSinceMs = bootNow;
  }

  // One-shot NTP sync the moment the network is up — before the boot
  // status page, which would otherwise hold it back while the data task
  // (core 0) is already fetching. The display timezone is a portal setting
  // (NVS "tz"); the factory default in src/config.h only covers the very
  // first boot, and a mid-session portal change applies itself via
  // setenv/tzset in the network service.
  static bool timeSyncRequested = false;
  if (!timeSyncRequested && isOnline()) {
    configTzTime(getTzString(), "pool.ntp.org", "time.nist.gov");
    timeSyncRequested = true;
    DBG_PRINTF("[TIME] NTP sync requested; timezone=%s\n", getTzString());
  }

  if (!isProvisioning() && onlineSinceMs != 0 &&
      bootNow - onlineSinceMs < BOOT_SETUP_PAGE_MS) {
    renderBootStatusPage(sport::name());
    return;
  }

  handleNetworkDisplay();

  // Connecting, no usable connection, or no favorite team picked yet
  // (teamSetupPending): the network screens own the display until that's
  // resolved — the connecting screen while an attempt runs, the setup
  // screen otherwise. Without this gate the waiting screen repaints over
  // them on every schedule refresh. ONE exception: the portal's display
  // overrides (game demo, manual mode, a pending display test) still run —
  // offline too, so new builds can be bench-tested without Wi-Fi. They get
  // the provisioning-shaped context either way, so the sport's online
  // pipeline can't paint over the network screen; it repaints once the
  // override releases. When the gate opens, consumeScoreboardRelease()
  // (inside sport::tick) repaints the scoreboard.
  bool connecting = isConnecting();
  if (connecting || isProvisioning() || teamSetupPending()) {
    static bool sOverrideOwnedDisplay = false;
    if (portalDisplayOverrideActive()) {
      sOverrideOwnedDisplay = true;
      SportTickContext ctx = {millis(), false, true};
      sport::tick(ctx);
      return;
    }
    if (sOverrideOwnedDisplay) {
      sOverrideOwnedDisplay = false;
      invalidateConnectScreen();
      requestNetworkScreenRepaint();  // back to the setup screen
    }
    if (connecting) {
      renderConnectScreen();
    }
    return;
  }

  SportTickContext ctx = {millis(), isOnline(), isProvisioning()};
  sport::tick(ctx);
}
