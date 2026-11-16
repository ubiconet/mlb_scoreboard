#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "audio_clips.h"
#include "boot_logo.h"
#include "common/app/sport_api.h"
#include "common/comms/network_service.h"
#include "common/data/time_util.h"
#include "common/hal/audio.h"
#include "common/hal/count_leds.h"
#include "common/hal/led_matrix.h"
#include "common/hal/tft_panel.h"
#include "common/ui/boot_splash.h"
#include "common/ui/gfx.h"
#include "mlb_renderer.h"
#include "mlb_snapshot.h"
#include "mlb_state.h"
#include "mlb_teams.h"

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

// Count-LED pin order for initCountLeds() and the display self-test chase
// (counter A = balls, B = strikes, C = outs, least-significant first).
const int COUNT_LED_PINS[7] = {BALL_1_PIN, BALL_2_PIN, BALL_3_PIN,
                               STRIKE_1_PIN, STRIKE_2_PIN, OUT_1_PIN,
                               OUT_2_PIN};

// The data task can't decide on its own which game the board should follow
// (that depends on the preferred-team list, which lives in NVS on core 1).
// We make that decision here, set activeGamePk via setActiveGamePk(), and
// the data task reads it on the next iteration. Until core-0 publishes its
// first schedule snapshot we stay in WAITING (the data task publishes a
// valid (but possibly empty) schedule within ~5 s).

// Pick the gamePk of the highest-priority preferred team currently live,
// straight from the other-games snapshot (which lists live games in
// start-time order and stores team-id digits in the abbrev fields). With
// no preferred team live, fall back to the first live game on the slate
// so the board always shows live baseball. This must not depend on any
// previously-fetched linescore: the data task only polls a linescore once
// a gamePk is selected here, so deriving the selection FROM a linescore
// can never bootstrap out of waiting mode.
int selectLiveGamePk(const ScheduleSnapshot& sch,
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
  return sch.otherCount > 0 ? sch.others[0].gamePk : 0;
}

// ---- Score sounds (MAX98357 I2S amp) ----
// Five selectable clips, id order matching the portal's sound picker
// (soundOptions() below): two crowd cheers, the bat crack, the two
// combined, and a ballpark-organ fanfare. All synthesized in-repo by
// tools/gen_score_sounds.py — no licensed audio ships with the firmware.
struct ScoreClip {
  const int16_t* pcm;
  size_t samples;
  int rate;
};
const ScoreClip kScoreClips[] = {
    {CHEER_PCM, CHEER_SAMPLES, CHEER_RATE},
    {ROAR_PCM, ROAR_SAMPLES, ROAR_RATE},
    {CRACK_PCM, CRACK_SAMPLES, CRACK_RATE},
    {CRACK_CHEER_PCM, CRACK_CHEER_SAMPLES, CRACK_CHEER_RATE},
    {ORGAN_PCM, ORGAN_SAMPLES, ORGAN_RATE},
};
const int kScoreClipCount =
    (int)(sizeof(kScoreClips) / sizeof(kScoreClips[0]));

// Plays the portal-selected sound (NVS "sound_id"; crowd cheer when unset
// or unrecognized). The isAudioEnabled() gate lives here so every caller —
// a preferred team's live run, a demo run, a portal preview — obeys the
// setting.
void playScoreSound() {
  if (!isAudioEnabled()) {
    return;
  }
  int id = getSelectedSoundId();
  if (id < 0 || id >= kScoreClipCount) {
    id = 0;
  }
  startClip(kScoreClips[id].pcm, kScoreClips[id].samples,
            kScoreClips[id].rate);
}

// ---- Live score-sound trigger ----
// A run by one of the user's own preferred teams plays the selected sound
// (the board follows preferred teams by default, but the fallback "first
// live game on the slate" may involve none of them — those stay silent,
// exactly like the NHL board's goal horn). Baselines reset whenever the
// followed game changes or a portal display override releases, so sounds
// missed while the demo/manual screen was up never replay in a burst.
int sSoundBaselinePk = 0;
int sSoundPrevHome = -1;
int sSoundPrevAway = -1;

void resetScoreSoundBaseline() {
  sSoundBaselinePk = 0;
  sSoundPrevHome = -1;
  sSoundPrevAway = -1;
}

void maybePlayScoreSound(const LinescoreSnapshot& ls) {
  if (ls.gamePk != sSoundBaselinePk) {
    sSoundBaselinePk = ls.gamePk;
    sSoundPrevHome = ls.homeScore;
    sSoundPrevAway = ls.awayScore;
    return;
  }
  bool homeScored = sSoundPrevHome >= 0 && ls.homeScore > sSoundPrevHome;
  bool awayScored = sSoundPrevAway >= 0 && ls.awayScore > sSoundPrevAway;
  sSoundPrevHome = ls.homeScore;
  sSoundPrevAway = ls.awayScore;
  if (!homeScored && !awayScored) return;
  // Resolve team ids the same way the renderer does when the snapshot
  // only carries offense/defense ids.
  int awayId = ls.awayTeamId ? ls.awayTeamId
                             : (ls.isTopInning ? ls.offenseTeamId
                                               : ls.defenseTeamId);
  int homeId = ls.homeTeamId ? ls.homeTeamId
                             : (ls.isTopInning ? ls.defenseTeamId
                                               : ls.offenseTeamId);
  int preferred[3];
  getPreferredTeamIds(preferred);
  for (int i = 0; i < 3; ++i) {
    int teamId = preferred[i];
    if (teamId == 0) continue;
    if ((homeScored && homeId == teamId) ||
        (awayScored && awayId == teamId)) {
      DBG_PRINTF("[AUDIO] preferred team scored: %d-%d\n", ls.awayScore,
                 ls.homeScore);
      playScoreSound();
      return;
    }
  }
}

// ---- Game demo (portal diagnostics page) ----
// A scripted live game — Blue Jays (home) vs Dodgers (away) — rendered
// through the real live renderer, so every output (TFT linescore, score
// matrices, count LEDs, at-bat result cards) is exercised exactly as it
// would be during a real game. One plate appearance resolves every 6 s
// from a fixed outcome table, so the demo is deterministic and loops
// forever through the batting order (extra innings past the 9th are fine
// for a demo; the inning digits render up to 99).
struct DemoState {
  int inning;
  bool top;            // true = away (LAD) bats
  int outs;
  int balls, strikes;
  bool first, second, third;
  int awayScore, homeScore;
  int paIndex;         // plate appearances completed since demo start
};
DemoState sDemo{};
LinescoreSnapshot sDemoGame{};
uint32_t sDemoStartedAtMs = 0;
uint32_t sDemoLastSecond = UINT32_MAX;

const int DEMO_TOR_ID = 141;  // Toronto Blue Jays (home)
const int DEMO_LAD_ID = 119;  // Los Angeles Dodgers (away)
const char* const DEMO_TOR_BATTERS[4] = {
    "V. GUERRERO JR.", "A. BICHETTE", "G. SPRINGER", "A. KIRK"};
const char* const DEMO_LAD_BATTERS[4] = {
    "S. OHTANI", "M. BETTS", "F. FREEMAN", "T. HERNANDEZ"};
// Pitchers/catchers are named by the team they belong to; the fielding
// side's pair is shown while the other team bats.
const char* const DEMO_TOR_PITCHER = "K. GAUSMAN";
const char* const DEMO_LAD_PITCHER = "Y. YAMAMOTO";
const char* const DEMO_TOR_CATCHER = "A. KIRK";
const char* const DEMO_LAD_CATCHER = "W. SMITH";

enum DemoOutcome : uint8_t {
  OUT_STRIKEOUT,
  OUT_FLY,
  OUT_GROUND,
  HIT_SINGLE,
  HIT_DOUBLE,
  HOME_RUN,
  WALK,
};
const DemoOutcome DEMO_SCRIPT[] = {
    OUT_STRIKEOUT, HIT_SINGLE,  OUT_FLY,     HOME_RUN,
    OUT_GROUND,    HIT_DOUBLE,  WALK,        OUT_FLY,
    HIT_SINGLE,    OUT_STRIKEOUT, HOME_RUN,  OUT_GROUND};
const size_t DEMO_SCRIPT_LEN =
    sizeof(DEMO_SCRIPT) / sizeof(DEMO_SCRIPT[0]);
const char* demoOutcomeName(DemoOutcome o) {
  switch (o) {
    case OUT_STRIKEOUT: return "Strikeout";
    case OUT_FLY:       return "Flyout";
    case OUT_GROUND:    return "Groundout";
    case HIT_SINGLE:    return "Single";
    case HIT_DOUBLE:    return "Double";
    case HOME_RUN:      return "Home Run";
    case WALK:          return "Walk";
  }
  return "";
}

// Existing runners move up `bases` (1 = single, 2 = double); the batter
// takes the base a hit of that size earns. Returns runs scored.
int advanceDemoRunners(int bases) {
  int runs = 0;
  bool nf = false, ns = false, nt = false;
  if (sDemo.first)  { if (bases == 1) ns = true; else nt = true; }
  if (sDemo.second) { if (bases == 1) nt = true; else ++runs; }
  if (sDemo.third)  { ++runs; }
  if (bases == 1) nf = true; else ns = true;
  sDemo.first = nf;
  sDemo.second = ns;
  sDemo.third = nt;
  return runs;
}

void buildDemoSnapshot();  // defined below; used by the PA resolver

// One plate appearance resolves: apply the scripted outcome, fire the
// full-screen at-bat result card, and flip the half-inning on 3 outs.
void resolveDemoPlateAppearance() {
  bool homeBatting = !sDemo.top;
  const char* const* lineup =
      homeBatting ? DEMO_TOR_BATTERS : DEMO_LAD_BATTERS;
  const char* batter = lineup[sDemo.paIndex % 4];
  const char* pitcher = homeBatting ? DEMO_LAD_PITCHER : DEMO_TOR_PITCHER;
  DemoOutcome outcome = DEMO_SCRIPT[sDemo.paIndex % DEMO_SCRIPT_LEN];
  sDemo.paIndex++;

  int runs = 0;
  char description[160];
  switch (outcome) {
    case OUT_STRIKEOUT:
      sDemo.outs++;
      snprintf(description, sizeof(description),
               "%s strikes out swinging against %s.", batter, pitcher);
      break;
    case OUT_FLY:
      sDemo.outs++;
      snprintf(description, sizeof(description),
               "%s flies out to center field.", batter);
      break;
    case OUT_GROUND:
      sDemo.outs++;
      snprintf(description, sizeof(description),
               "%s grounds out to shortstop.", batter);
      break;
    case HIT_SINGLE:
      runs = advanceDemoRunners(1);
      snprintf(description, sizeof(description),
               "%s lines a single to left field.%s", batter,
               runs > 0 ? " Runners come around to score." : "");
      break;
    case HIT_DOUBLE:
      runs = advanceDemoRunners(2);
      snprintf(description, sizeof(description),
               "%s rips a double into the gap.%s", batter,
               runs > 0 ? " Runners come around to score." : "");
      break;
    case HOME_RUN:
      runs = 1 + (sDemo.first ? 1 : 0) + (sDemo.second ? 1 : 0) +
             (sDemo.third ? 1 : 0);
      sDemo.first = sDemo.second = sDemo.third = false;
      snprintf(description, sizeof(description),
               "%s launches a home run! %d run%s score.", batter, runs,
               runs == 1 ? "" : "s");
      break;
    case WALK: {
      bool forcedRun = false;
      if (sDemo.first) {
        if (sDemo.second) {
          if (sDemo.third) forcedRun = true;
          sDemo.third = true;
        }
        sDemo.second = true;
      }
      sDemo.first = true;
      runs = forcedRun ? 1 : 0;
      snprintf(description, sizeof(description),
               "%s works a walk off %s.%s", batter, pitcher,
               forcedRun ? " A run is forced in." : "");
      break;
    }
  }
  if (runs > 0) {
    if (homeBatting) sDemo.homeScore += runs;
    else             sDemo.awayScore += runs;
    playScoreSound();  // demo runs sound for either side
  }
  sDemo.balls = 0;
  sDemo.strikes = 0;

  if (sDemo.outs >= 3) {
    sDemo.outs = 0;
    sDemo.first = sDemo.second = sDemo.third = false;
    if (sDemo.top) {
      sDemo.top = false;  // bottom half
    } else {
      sDemo.top = true;   // next inning
      ++sDemo.inning;
    }
  }

  buildDemoSnapshot();
  showAtBatResult(batter, demoOutcomeName(outcome), description);
}

// The demo's LinescoreSnapshot: the same POD the live data task publishes,
// so the real renderer (TFT + matrices + count LEDs) drives the demo.
void buildDemoSnapshot() {
  sDemoGame = LinescoreSnapshot{};
  sDemoGame.valid = true;
  sDemoGame.gamePk = -1;  // never matches a real followed game
  sDemoGame.awayTeamId = DEMO_LAD_ID;
  sDemoGame.homeTeamId = DEMO_TOR_ID;
  sDemoGame.awayScore = sDemo.awayScore;
  sDemoGame.homeScore = sDemo.homeScore;
  sDemoGame.currentInning = sDemo.inning;
  sDemoGame.inningState = sDemo.top ? 0 : 1;
  sDemoGame.isTopInning = sDemo.top;
  sDemoGame.balls = sDemo.balls;
  sDemoGame.strikes = sDemo.strikes;
  sDemoGame.outs = sDemo.outs;
  bool homeBatting = !sDemo.top;
  sDemoGame.offenseTeamId = homeBatting ? DEMO_TOR_ID : DEMO_LAD_ID;
  sDemoGame.defenseTeamId = homeBatting ? DEMO_LAD_ID : DEMO_TOR_ID;
  sDemoGame.offenseFirst = sDemo.first;
  sDemoGame.offenseSecond = sDemo.second;
  sDemoGame.offenseThird = sDemo.third;
  const char* const* lineup =
      homeBatting ? DEMO_TOR_BATTERS : DEMO_LAD_BATTERS;
  strlcpy(sDemoGame.batter, lineup[sDemo.paIndex % 4],
          sizeof(sDemoGame.batter));
  strlcpy(sDemoGame.onDeck, lineup[(sDemo.paIndex + 1) % 4],
          sizeof(sDemoGame.onDeck));
  strlcpy(sDemoGame.inHole, lineup[(sDemo.paIndex + 2) % 4],
          sizeof(sDemoGame.inHole));
  strlcpy(sDemoGame.pitcher,
          homeBatting ? DEMO_LAD_PITCHER : DEMO_TOR_PITCHER,
          sizeof(sDemoGame.pitcher));
  strlcpy(sDemoGame.catcher,
          homeBatting ? DEMO_LAD_CATCHER : DEMO_TOR_CATCHER,
          sizeof(sDemoGame.catcher));
}

void startGameDemo(uint32_t nowMs) {
  sDemo = DemoState{};
  sDemo.inning = 6;
  sDemo.top = false;     // bottom of the 6th, Blue Jays batting
  sDemo.outs = 1;
  sDemo.first = true;    // runner on first
  sDemo.awayScore = 2;   // Dodgers
  sDemo.homeScore = 3;   // Blue Jays
  sDemoStartedAtMs = nowMs;
  sDemoLastSecond = 0;
  buildDemoSnapshot();
  // gamePk -1 never matches the cached live frame, so this is a full
  // repaint by construction.
  renderLinescore(sDemoGame);
  DBG_PRINTF("[DEMO] game demo started: TOR vs LAD, bottom %d\n",
             sDemo.inning);
}

// Advances the demo at most once per second: the count builds through the
// plate appearance, the 6th tick resolves it (at-bat result card), and the
// fresh snapshot drives the real live renderer. While the result card owns
// the screen the linescore render is skipped — same rule as the live flow.
bool advanceGameDemo(uint32_t nowMs) {
  uint32_t elapsedSec = (nowMs - sDemoStartedAtMs) / 1000;
  if (elapsedSec == sDemoLastSecond) return false;
  sDemoLastSecond = elapsedSec;

  // Let an in-flight result card expire; it repaints the (cached) linescore.
  updateAtBatResultDisplay();

  int phase = elapsedSec % 6;  // 0..5 within the current plate appearance
  static const uint8_t BALLS[6]   = {0, 0, 1, 1, 2, 0};
  static const uint8_t STRIKES[6] = {0, 1, 1, 2, 2, 0};
  if (phase < 5) {
    sDemo.balls = BALLS[phase];
    sDemo.strikes = STRIKES[phase];
    buildDemoSnapshot();
  } else {
    resolveDemoPlateAppearance();  // fires the result card too
  }
  if (!isAtBatResultVisible()) {
    renderLinescore(sDemoGame);
  }
  return true;
}

// Repaint whatever the scoreboard was showing before a portal display
// override (game demo / manual mode) took the screen.
void restoreAfterPortalOverride() {
  resetAtBatResultDisplay();
  resetScoreSoundBaseline();
  if (scoreboardState == ScoreboardState::LIVE_GAME) {
    forceLiveRepaint();
  } else {
    renderWaiting(sHasCurrentSchedule ? &sCurrentSchedule : nullptr,
                  &sCurrentNews, &sCurrentStandings);
    invalidateMax7219Clock();
  }
}

// Portal-triggered display test: one cycle through every display, a 2 s
// hold with everything on, then back to whatever was showing. Runs on the
// render core (blocking ~8 s — nothing renders during the cycle by
// definition; feeds keep publishing on core 0 and land right after).
void runDisplayTestCycle() {
  Serial.println("[TEST] display test cycle (portal)");

  // Screen banner
  GFXcanvas16& c = tftPanel.canvas();
  c.fillScreen(COLOR_BG);
  c.setTextColor(COLOR_GOLD);
  c.setTextSize(4);
  drawCenteredText(c, "DISPLAY", 160, 78);
  drawCenteredText(c, "TEST", 160, 118);
  tftPanel.pushFull();

  // Matrices: all-on, one module at a time (1 = away, 2 = home)
  for (int pos = 1; pos <= 2; ++pos) {
    setMax7219PositionTest(pos, true);
    delay(1200);
    setMax7219PositionTest(pos, false);
    delay(150);
  }
  // Then the digit pattern
  setMax7219Scores(2, 3);
  delay(1200);

  // Count LEDs: chase one lamp at a time, then all on
  for (int i = 0; i < 7; ++i) {
    for (int k = 0; k < 7; ++k) {
      digitalWrite(COUNT_LED_PINS[k], k == i ? HIGH : LOW);
    }
    delay(250);
  }
  for (int k = 0; k < 7; ++k) digitalWrite(COUNT_LED_PINS[k], HIGH);

  // HOLD: everything on for 2 s
  setMax7219DisplayTestAll(true);
  delay(2000);

  // Restore whatever was showing before the cycle (a running demo
  // repaints through the demo renderer, live games through the cached
  // linescore, waiting mode through the standby screen).
  setMax7219DisplayTestAll(false);
  setCountLeds(0, 0, 0);
  if (isGameDemoMode()) {
    renderLinescore(sDemoGame);
  } else if (scoreboardState == ScoreboardState::LIVE_GAME) {
    forceLiveRepaint();
  } else {
    renderWaiting(sHasCurrentSchedule ? &sCurrentSchedule : nullptr,
                  &sCurrentNews, &sCurrentStandings);
    invalidateMax7219Clock();
  }
  Serial.println("[TEST] display test done — restored");
}

}  // namespace

namespace sport {

const char* name() { return "Baseball Scoreboard"; }

NetworkBranding branding() {
  return NetworkBranding{name(), NETWORK_AP_SSID, NETWORK_HOSTNAME,
                         "Score Sounds"};
}

const NetworkTeamOption* teamOptions(size_t& count) {
  return mlbTeamOptions(count);
}

const int* defaultPreferredTeams() { return MLB_DEFAULT_PREFERRED_TEAMS; }

// Portal score-sound picker: id order matches kScoreClips above.
const NetworkSoundOption* soundOptions(size_t& count) {
  static const NetworkSoundOption options[] = {
      {0, "Crowd Cheer"},
      {1, "Stadium Roar"},
      {2, "Bat Crack"},
      {3, "Crack + Cheer"},
      {4, "Ballpark Organ"},
  };
  count = sizeof(options) / sizeof(options[0]);
  return options;
}

int defaultSoundId() { return 0; }

void setup() {
  // Initialize hardware: discrete count LEDs + MAX7219 matrices, then the
  // TFT panel over software SPI (bit-bangs SCK/MOSI on exactly the pins
  // passed in — see common/hal/tft_panel.h for why the hardware-SPI variant
  // must not be used on this board).
  initCountLeds(COUNT_LED_PINS);
  initLedMatrix(MAX7219_DIN_PIN, MAX7219_CLK_PIN, MAX7219_CS_PIN);
  initAudio(I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DIN_PIN);  // score-sound amp

  // TEST ONLY: shows H (home) / A (away) on the score matrices; leave enabled during hardware validation.
  //runMax7219BootTest();

  tftPanel.begin(TFT_CS_PIN, TFT_DC_PIN, TFT_MOSI_PIN, TFT_SCLK_PIN,
                 TFT_RESET_PIN, TFT_NATIVE_WIDTH, TFT_NATIVE_HEIGHT,
                 TFT_ROTATION, TFT_BACKLIGHT_PIN, TFT_MISO_PIN,
                 TFT_OUTPUT_ROTATION_DEG);

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

void tick(const SportTickContext& ctx) {
  // ---- Panel orientation picker (diagnostics page, bench tool) ----
  // Modes A-F: the driver-rotation x transpose combos; applied live, then
  // the waiting screen repaints so the result is judgeable by eye.
  int panelMode = consumePanelOrientationRequest();
  if (panelMode >= 0) {
    switch (panelMode) {
      case 0: tftPanel.setOutputMode(1, 0);   break;  // A: direct, rot 1
      case 1: tftPanel.setOutputMode(3, 0);   break;  // B: direct, rot 3
      case 2: tftPanel.setOutputMode(0, -90); break;  // C: transpose -90
      case 3: tftPanel.setOutputMode(0, 90);  break;  // D: transpose +90
      case 4: tftPanel.setOutputMode(2, -90); break;  // E: rot 2, -90
      case 5: tftPanel.setOutputMode(2, 90); break;   // F: rot 2, +90
      case 6: tftPanel.setRawOutputMode(0x28, -90); break;  // G: MV|BGR
      case 7: tftPanel.setRawOutputMode(0x28, 90); break;   // H: MV|BGR
      case 8: tftPanel.setRawOutputMode(0xE8, -90); break;  // I: MY|MV|BGR
      default: tftPanel.setRawOutputMode(0xE8, 90); break;  // J: MY|MV|BGR
    }
    // Announce the mode: full-bleed color + label, so a photo of the
    // flash shows WHICH mode is active and exactly how far its FULL-frame
    // write reaches across the glass (bench instrumentation, v2.90).
    {
      static const char* const kLetters[] = {"A", "B", "C", "D", "E",
                                             "F", "G", "H", "I", "J"};
      GFXcanvas16& c = tftPanel.canvas();
      c.fillScreen(COLOR_CARD);
      c.setTextColor(COLOR_GOLD);
      c.setTextSize(5);
      char label[10];
      snprintf(label, sizeof(label), "MODE %s", kLetters[panelMode]);
      drawCenteredText(c, label, 160, 108);
      tftPanel.pushFull();
      delay(1400);
    }
    invalidateManualScreen();
    resetAtBatResultDisplay();
    renderWaiting(sHasCurrentSchedule ? &sCurrentSchedule : nullptr,
                  &sCurrentNews, &sCurrentStandings);
    return;
  }

  // ---- Portal display self-test (diagnostics page) ----
  if (consumeDisplayTestRequest()) {
    runDisplayTestCycle();
    return;  // screen restored; next tick resumes normal flow
  }

  // Portal audio test: true = a request arrived; id -1 means it didn't
  // name a sound (diag "Play Test Sound", manual-mode sound button), so
  // play the saved selection; otherwise the picker's Preview id.
  int testSoundId = -1;
  if (consumeAudioTestRequest(testSoundId)) {
    if (isAudioEnabled()) {
      if (testSoundId >= 0 && testSoundId < kScoreClipCount) {
        DBG_PRINTF("[AUDIO] sound preview id %d\n", testSoundId);
        startClip(kScoreClips[testSoundId].pcm,
                  kScoreClips[testSoundId].samples,
                  kScoreClips[testSoundId].rate);
      } else {
        DBG_PRINTF("[AUDIO] score sound test\n");
        playScoreSound();
      }
    } else {
      DBG_PRINTF("[AUDIO] disabled in settings - test skipped\n");
    }
  }

  // ---- Game demo (diagnostics page; owns display + hardware) ----
  static bool sInManual = false;
  static bool sInDemo = false;
  if (isGameDemoMode()) {
    if (!sInDemo) {
      sInDemo = true;
      sInManual = false;
      startGameDemo(ctx.nowMs);
    } else {
      advanceGameDemo(ctx.nowMs);
    }
    return;
  }
  if (sInDemo) {
    sInDemo = false;
    // Demo mode handed the display back — repaint whatever was showing.
    restoreAfterPortalOverride();
  }

  // ---- Manual mode (portal page owns the values) ----
  if (isManualMode()) {
    if (!sInManual) {
      sInManual = true;
      invalidateManualScreen();
    }
    renderManual();
    return;
  }
  if (sInManual) {
    sInManual = false;
    // Manual mode handed the display back — repaint whatever was showing.
    restoreAfterPortalOverride();
  }

  updateAtBatResultDisplay();
  rotateCarousel();

  if (consumeScoreboardRelease()) {
    scoreboardState = ScoreboardState::WAITING;
    setActiveGamePk(0);
    postgameGraceStartedAt = 0;
    resetScoreSoundBaseline();
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
  // tell it. Selection follows the preferred teams in priority order (see
  // selectLiveGamePk), falling back to the first live game on the slate;
  // the "other games" list feeds the live ticker.
  if (newSchedule && sch.valid) {
    sCurrentSchedule = sch;
    sHasCurrentSchedule = true;
    int preferredTeams[3];
    getPreferredTeamIds(preferredTeams);
    int newGamePk = selectLiveGamePk(sch, preferredTeams);

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
    // play snapshot may fire the at-bat result card. A run by one of the
    // preferred teams fires the score sound first (even while a result
    // card holds the screen — the card would otherwise eat the moment).
    if (newLinescore && ls.valid) {
      maybePlayScoreSound(ls);
    }
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

String feedDiagnosticsJson() {
  // Portal-side feed diagnostics (pulled into /status and /diag by the
  // network service's status hook — see main.cpp). Everything here is
  // already-cached state: no radio calls, no allocation beyond the
  // returned String.
  uint32_t ago = (getScheduleLastFetchAt() == 0)
                     ? 0
                     : (millis() - getScheduleLastFetchAt()) / 1000;
  char clock[24] = "unset";
  if (timeIsSynced()) {
    time_t t = time(nullptr);
    tm lt = {};
    localtime_r(&t, &lt);
    strftime(clock, sizeof(clock), "%Y-%m-%d %H:%M", &lt);
  }
  String j = "\"schedAttempts\":";
  j += (unsigned)getScheduleFetchAttempts();
  j += ",\"schedOk\":";
  j += (unsigned)getScheduleFetchSuccesses();
  j += ",\"schedErr\":\"";
  j += getScheduleLastError();
  j += "\",\"schedCode\":";
  j += getScheduleLastHttpCode();
  j += ",\"schedAgeSec\":";
  j += (unsigned)ago;
  j += ",\"activeGamePk\":";
  j += getActiveGamePk();
  j += ",\"rssi\":";
  j += WiFi.RSSI();
  j += ",\"clock\":\"";
  j += clock;
  j += "\",\"freeHeap\":";
  j += (unsigned)ESP.getFreeHeap();
  return j;
}

}  // namespace sport
