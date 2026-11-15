#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "mlb_snapshot.h"

// Renderer-internal shared state — defined in mlb_renderer_linescore.cpp,
// used by both renderer TUs (linescore + waiting) and nowhere else. The
// pre-split anonymous namespace couldn't cross file boundaries, so the
// cross-file pieces live in this named namespace instead. Do NOT include
// from outside the renderer.

namespace mlb_render {

// Latest linescore snapshot from core 0 + whether a live game owns the
// screen (set false by renderWaiting, true by renderLinescore).
extern LinescoreSnapshot currentLinescore;
extern bool hasCurrentLiveGame;

// Division standings JSON (fetched but only rendered by the parked #if 0
// standings feature; kept wired for re-enable).
extern JsonObjectConst currentStandings;

// Live-game ticker slide index (On Deck / Around-the-League rotation) and
// the shared carousel dwell clock.
extern uint8_t tickerSlide;
extern uint32_t lastCarouselTime;

// Snapshot of other in-progress games, copied out of the schedule JSON so it
// stays valid across schedule re-fetches. 3 games per ticker slide.
struct OtherGameInfo {
  int  awayTeamId;
  int  homeTeamId;
  char awayAbbrev[8];
  char homeAbbrev[8];
  int  awayScore;
  int  homeScore;
  char inningState[8];   // Top / Bottom / Middle / End
  char inningOrdinal[8]; // e.g. "7th"
};
const size_t MAX_OTHER_GAMES = 9;
extern OtherGameInfo otherGames[MAX_OTHER_GAMES];
extern size_t otherGameCount;

// 1 + otherGameCount: On Deck slide plus one league game per slide.
size_t tickerSlideCount();

// Drops the live frame's dirty-rect cache so the next renderLinescore()
// repaints AND pushes everything. Call whenever another screen drew into
// the shared canvas (waiting idle pages, the at-bat card) — with the
// steady-state band push (v2.85) a stale tracker would otherwise leave
// the other screen's pixels standing above the bottom panel.
void invalidateLiveFrame();

}  // namespace mlb_render
