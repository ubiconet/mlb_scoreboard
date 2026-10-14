#include <Arduino.h>

#include "config.h"
#include "boot_logo.h"
#include "common/app/sport_api.h"
#include "common/comms/network_service.h"
#include "common/hal/count_leds.h"
#include "common/hal/led_matrix.h"
#include "common/hal/tft_panel.h"
#include "common/ui/boot_splash.h"
#include "mlb_renderer.h"
#include "mlb_snapshot.h"
#include "mlb_state.h"

// Baseball app: the WAITING/LIVE state machine and sport:: contract
// implementation (see common/app/sport_api.h). The generic shell in
// src/main.cpp drives boot/network/update; everything here is what makes
// this build a baseball scoreboard (TFT + score matrices + count LEDs).

namespace {

enum class ScoreboardState {
  WAITING,
  LIVE_GAME
};
ScoreboardState scoreboardState = ScoreboardState::WAITING;

// Last-rendered state, used to detect transitions between live/waiting and to
// drive the postgame grace window without re-reading snapshots.
uint32_t postgameGraceStartedAt = 0;
int  lastAtBatIndex       = -1;
bool atBatBaselineReady   = false;

// "Last seen" generation counters for the snapshots we consume.
uint32_t sLastLinescoreGen = 0;
uint32_t sLastPlayGen      = 0;
uint32_t sLastScheduleGen  = 0;
uint32_t sLastNewsGen = 0;
uint32_t sLastStandingsGen = 0;
uint32_t sLastScheduleRenderAt = 0;
ScheduleSnapshot sCurrentSchedule{};
bool sHasCurrentSchedule = false;
NewsSnapshot sCurrentNews{};
StandingsSnapshot sCurrentStandings{};

// The data task can't decide on its own which game the board should follow.
// We make that decision here, set activeGamePk via setActiveGamePk(), and
// the data task reads it on the next iteration. Until core-0 publishes its
// first schedule snapshot we stay in WAITING (the data task publishes a
// valid (but possibly empty) schedule within ~5 s).

// Follow the first live game on today's slate — the schedule lists games in
// start-time order, so that is the earliest game currently in progress.
// With team selection gone (v2.62), the board simply shows live baseball:
// when the followed game ends it rolls to the next live game, or returns
// to waiting mode when none are left.
int selectLiveGamePk(const ScheduleSnapshot& sch) {
  return sch.otherCount > 0 ? sch.others[0].gamePk : 0;
}

}  // namespace

namespace sport {

const char* name() { return "Baseball Scoreboard"; }

NetworkBranding branding() {
  return NetworkBranding{name(), NETWORK_AP_SSID, NETWORK_HOSTNAME};
}

void setup() {
  // Initialize hardware: discrete count LEDs + MAX7219 matrices, then the
  // TFT panel over software SPI (bit-bangs SCK/MOSI on exactly the pins
  // passed in — see common/hal/tft_panel.h for why the hardware-SPI variant
  // must not be used on this board).
  const int countLedPins[7] = {BALL_1_PIN, BALL_2_PIN, BALL_3_PIN,
                               STRIKE_1_PIN, STRIKE_2_PIN, OUT_1_PIN,
                               OUT_2_PIN};
  initCountLeds(countLedPins);
  initLedMatrix(MAX7219_DIN_PIN, MAX7219_CLK_PIN, MAX7219_CS_PIN);

  // TEST ONLY: shows H (home) / A (away) on the score matrices; leave enabled during hardware validation.
  //runMax7219BootTest();

  tftPanel.begin(TFT_CS_PIN, TFT_DC_PIN, TFT_MOSI_PIN, TFT_SCLK_PIN,
                 TFT_RESET_PIN, TFT_NATIVE_WIDTH, TFT_NATIVE_HEIGHT, 1);

    // TEST ONLY: cycles ball/strike/out LEDs one at a time; leave enabled during hardware validation.
  runCountLedTestLoop();

  renderBootSplash(BOOT_LOGO_WIDTH, BOOT_LOGO_HEIGHT, BOOT_LOGO_MLB);
  // No blocking hold here: network services start immediately and connect
  // behind the logo. The shell's loop() enforces the minimum splash time.
  Serial.println("[MAIN] Baseball Scoreboard ready");
}

void startDataTask() {
  startMlbDataTask();  // core-0 linescore/play/schedule fetches
}

bool hasInitialData() {
  // The boot status page holds until the first schedule fetch succeeds —
  // with the waiting carousel's JSON cache gone (v2.62), fetch success is
  // the "first data published" signal.
  return getScheduleFetchSuccesses() > 0;
}

void tick(const SportTickContext& ctx) {
  // ---- Manual mode (portal page owns the values) ----
  static bool sInManual = false;
  if (isManualMode()) {
    if (!sInManual) {
      sInManual = true;
      invalidateManualScreen();
    }
    renderManual(BOOT_LOGO_WIDTH, BOOT_LOGO_HEIGHT, BOOT_LOGO_MLB);
    return;
  }
  if (sInManual) {
    sInManual = false;
    // Manual mode handed the display back — repaint whatever was showing.
    resetAtBatResultDisplay();
    if (scoreboardState == ScoreboardState::LIVE_GAME) {
      forceLiveRepaint();
    } else {
      renderWaiting(sHasCurrentSchedule ? &sCurrentSchedule : nullptr,
                    &sCurrentNews, &sCurrentStandings);
      invalidateMax7219Clock();
    }
  }

  updateAtBatResultDisplay();
  rotateCarousel();

  if (consumeScoreboardRelease()) {
    scoreboardState = ScoreboardState::WAITING;
    setActiveGamePk(0);
    postgameGraceStartedAt = 0;
    renderWaiting(sHasCurrentSchedule ? &sCurrentSchedule : nullptr,
                  &sCurrentNews, &sCurrentStandings);
    logWaitingDisplayState();
  }

  if (!ctx.isOnline) {
    return;
  }

  uint32_t now = ctx.nowMs;

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
  NewsSnapshot news{};
  bool newNews = takeNewsSnapshot(news, sLastNewsGen);
  StandingsSnapshot standings{};
  bool newStandings = takeStandingsSnapshot(standings, sLastStandingsGen);
  if (newNews && news.valid) sCurrentNews = news;
  if (newStandings && standings.valid) sCurrentStandings = standings;

  // ---- Decide which game (if any) the board should follow ----
  //
  // The data task on core 0 polls linescore/play for whatever gamePk we
  // tell it. Selection follows the first live game on the slate (see
  // selectLiveGamePk); the "other games" list feeds the live ticker.
  if (newSchedule && sch.valid) {
    sCurrentSchedule = sch;
    sHasCurrentSchedule = true;
    int newGamePk = selectLiveGamePk(sch);

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
    // First entry to WAITING (or schedule refresh): refresh the standby screen.
    if (sLastScheduleRenderAt == 0 || newSchedule || newNews || newStandings) {
      sLastScheduleRenderAt = now;
      renderWaiting(sHasCurrentSchedule ? &sCurrentSchedule : nullptr,
                    &sCurrentNews, &sCurrentStandings);
      logWaitingDisplayState();
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

}  // namespace sport
