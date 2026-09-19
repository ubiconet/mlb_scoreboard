#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <ArduinoJson.h>
#include <time.h>

#include "config.h"
#include "hardware_drivers.h"
#include "network.h"
#include "scoreboard.h"
#include "mlb_client.h"
#include "mlb_snapshot.h"

// Reverted to the 5-arg software-SPI constructor that bit-bangs SCK/MOSI
// directly on the panel pins. We tried the 3-arg hardware-SPI variant which
// should be faster, but initSPI() calls SPI.begin() with no args internally —
// and on ESP32-S3 the no-arg form binds the SPI peripheral to the variant's
// default VSPI pins (SCK=8, MOSI=11) instead of our panel pins (13/12/9).
// Result: blank screen because every SPI write went out on the wrong pins.
// Software SPI is slower per byte but the constructor pins we pass ARE the
// ones the library bit-bangs, so it's been proven to work on this hardware.
Adafruit_ST7789 display(TFT_CS_PIN, TFT_DC_PIN, TFT_MOSI_PIN, TFT_SCLK_PIN,
                         TFT_RESET_PIN);

// No JSON docs live on the render core anymore. The data task on core 0
// parses the linescore / playByPlay / schedule / news payloads and publishes
// compact POD snapshots that loop() just copies out.
JsonDocument emptyDoc;  // empty stand-in for the few legacy JsonObjectConst args

// Last-rendered state, used to detect transitions between live/waiting and to
// drive the postgame grace window without re-reading snapshots.
uint32_t postgameGraceStartedAt = 0;
int  lastAtBatIndex       = -1;
bool atBatBaselineReady   = false;
bool timeSyncRequested    = false;

enum class ScoreboardState {
  WAITING,
  LIVE_GAME
};
ScoreboardState scoreboardState = ScoreboardState::WAITING;

// "Last seen" generation counters for the snapshots we consume.
static uint32_t sLastLinescoreGen = 0;
static uint32_t sLastPlayGen      = 0;
static uint32_t sLastScheduleGen  = 0;
static uint32_t sLastScheduleRenderAt = 0;

// The data task can't decide on its own which game the user wants to follow
// (that depends on the preferred-team list, which lives in NVS on core 1).
// We make that decision here, set activeGamePk via setActiveGamePk(), and the
// data task reads it on the next iteration. Until core-0 publishes its first
// schedule snapshot we stay in WAITING (the data task publishes a valid (but
// possibly empty) schedule within ~5 s).

// Pick the gamePk of the highest-priority preferred team currently live,
// straight from the other-games snapshot. This must not depend on any
// previously-fetched linescore: the data task only polls a linescore once a
// gamePk is selected here, so deriving the selection FROM a linescore can
// never bootstrap out of waiting mode (the bug this replaces).
int selectLiveGamePkForTeams(const ScheduleSnapshot& sch,
                             const int preferredTeams[3]) {
  for (int p = 0; p < 3; ++p) {
    int teamId = preferredTeams[p];
    if (teamId == 0) continue;
    for (size_t i = 0; i < sch.otherCount; ++i) {
      int a = atoi(sch.others[i].awayAbbrev);
      int h = atoi(sch.others[i].homeAbbrev);
      if (a == teamId || h == teamId) {
        return sch.others[i].gamePk;
      }
    }
  }
  return 0;
}

void logPendingLiveDisplayState() {
  DBG_PRINTF(
    "[DISPLAY] state=LIVE_GAME gamePk=%d | TFT/matrices/count LEDs=waiting "
    "for immediate linescore refresh\n", getActiveGamePk());
}

bool getScheduleRange(char startDate[11], char endDate[11]) {
  time_t now = time(nullptr);
  if (now < 1704067200) { // 2024-01-01: NTP has not set the clock yet.
    return false;
  }

  tm startLocal = {};
  localtime_r(&now, &startLocal);
  time_t endTime = now + (13 * 24 * 60 * 60);
  tm endLocal = {};
  localtime_r(&endTime, &endLocal);

  return strftime(startDate, 11, "%Y-%m-%d", &startLocal) > 0 &&
         strftime(endDate, 11, "%Y-%m-%d", &endLocal) > 0;
}

void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  // Native USB CDC enumeration grace period
  uint32_t start = millis();
  while (!Serial && (millis() - start < 3000)) {
    delay(10);
  }

  // Initialize hardware drivers (Discrete Count LEDs & MAX7219 matrices)
  initHardwareDrivers();

  // TEST ONLY: shows H (home) / A (away) on the score matrices; leave enabled during hardware validation.
  //runMax7219BootTest();

  // Initialize TFT Display over software SPI. The 5-arg Adafruit_ST7789
  // constructor bit-bangs SCK/MOSI on the pins we pass directly; no SPI bus
  // init is needed.
  display.init(TFT_NATIVE_WIDTH, TFT_NATIVE_HEIGHT);
  display.setRotation(1);

    // TEST ONLY: cycles ball/strike/out LEDs one at a time; leave enabled during hardware validation.
  runCountLedTestLoop();

  renderBootSplash();
  delay(BOOT_SPLASH_HOLD_MS);

  startNetworkServices();
  startNetworkTask();
  startMlbDataTask();  // core-0 linescore/play/schedule/news fetches

  // Boot banner — visible over Serial whenever MLB_DEBUG=1 so a freshly
  // uploaded firmware can be confirmed at a glance. Two lines: one short,
  // one with everything useful for diagnosing which build is on the box.
  Serial.println();
  Serial.printf("[BOOT] FW=%s ESPN_TTL=%lu min build=%s %s\n",
                FIRMWARE_VERSION,
                (unsigned long)(MLB_NEWS_CACHE_TTL_MS / 60000UL),
                __DATE__, __TIME__);
  Serial.printf("[BOOT] heap_free=%u heap_min=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap());
  Serial.println("[MAIN] MLB Scoreboard Ready");
}

void loop() {
  handleNetworkDisplay();
  updateAtBatResultDisplay();

  // Firmware-update UI owns the whole screen while an OTA download runs;
  // everything else pauses until it finishes (reboot) or fails.
  if (handleOtaUpdateScreen()) {
    return;
  }

  rotateCarousel();

  if (consumeScoreboardRelease()) {
    int preferredTeams[3];
    getPreferredTeamIds(preferredTeams);
    scoreboardState = ScoreboardState::WAITING;
    setActiveGamePk(0);
    postgameGraceStartedAt = 0;
    JsonObjectConst sched = getUpcomingScheduleJson();
    renderWaiting(sched, sched, preferredTeams);
    logWaitingDisplayState(preferredTeams);
  }

  if (!isOnline()) {
    return;
  }

  uint32_t now = millis();
  int preferredTeams[3];
  getPreferredTeamIds(preferredTeams);

  if (!timeSyncRequested) {
    configTzTime(LOCAL_TIMEZONE, "pool.ntp.org", "time.nist.gov");
    timeSyncRequested = true;
    DBG_PRINTF("[TIME] NTP sync requested; timezone=%s\n", LOCAL_TIMEZONE);
  }

  // Tick the scoreboard clock once per loop when idle (no live game).
  if (scoreboardState == ScoreboardState::WAITING) {
    updateMax7219Clock(isClockDisplayEnabled());
  }

  // ---- Consume new linescore snapshot from core 0 ----
  LinescoreSnapshot ls{};
  bool newLinescore = takeLinescoreSnapshot(ls, sLastLinescoreGen);

  // ---- Consume new play-by-play snapshot ----
  PlaySnapshot play{};
  bool newPlay = takePlaySnapshot(play, sLastPlayGen);

  // ---- Consume new schedule snapshot ----
  ScheduleSnapshot sch{};
  bool newSchedule = takeScheduleSnapshot(sch, sLastScheduleGen);

  // ---- Decide which game (if any) the renderer should follow ----
  //
  // The data task on core 0 polls linescore for whatever gamePk we tell it.
  // We pick the highest-priority preferred team that has a live game on
  // today's slate. The "other games" list inside ScheduleSnapshot carries
  // the away/home team ids; when one matches a preferred team we know there's
  // a live game for them. We then ask core 0 to switch to whichever
  // gamePk the data task is currently fetching (the linescore snapshot's
  // gamePk) — if that happens to be the same game.
  if (newSchedule && sch.valid) {
    // Select the followed game directly from the live-games snapshot.
    int newGamePk = selectLiveGamePkForTeams(sch, preferredTeams);

    updateOtherGames(sch, newGamePk);
    bool gameChanged = newGamePk != getActiveGamePk();
    if (newGamePk > 0) {
      setActiveGamePk(newGamePk);
      scoreboardState = ScoreboardState::LIVE_GAME;
      postgameGraceStartedAt = 0;
      if (gameChanged) {
        lastAtBatIndex = -1;
        atBatBaselineReady = false;
        resetAtBatResultDisplay();
      }
    }
  }

  // Render based on state.
  if (scoreboardState == ScoreboardState::WAITING) {
    // First entry to WAITING (or schedule refresh): draw the waiting page once.
    if (sLastScheduleRenderAt == 0 || newSchedule) {
      sLastScheduleRenderAt = now;
      JsonObjectConst sched = getUpcomingScheduleJson();
      renderWaiting(sched, sched, preferredTeams);
      logWaitingDisplayState(preferredTeams);
    }
  } else {
    // LIVE_GAME: each fresh linescore drives the live screen; each fresh
    // play snapshot may fire the at-bat result card.
    if (newLinescore && ls.valid && !isAtBatResultVisible()) {
      renderLinescore(ls);
      logLiveDisplayState(ls, ls.gamePk);
    }
    if (newPlay && play.valid) {
      if (atBatBaselineReady && play.atBatIndex > lastAtBatIndex &&
          play.description[0] != '\0') {
        showAtBatResult(play.batterName, play.event, play.description);
      }
      if (play.atBatIndex >= 0) {
        lastAtBatIndex = play.atBatIndex;
        atBatBaselineReady = true;
      }
    }
  }
}

