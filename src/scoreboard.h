#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "mlb_snapshot.h"

// Render-side accessors for the snapshots filled by the core-0 MLB data task.
// Each function copies the current snapshot into `out` only if the generation
// counter has advanced since the caller's last sample, so the renderer can
// detect fresh data without locks. Returns true when a fresh payload was
// delivered.
bool takeLinescoreSnapshot(LinescoreSnapshot& out, uint32_t& lastGen);
bool takePlaySnapshot(PlaySnapshot& out, uint32_t& lastGen);
bool takeScheduleSnapshot(ScheduleSnapshot& out, uint32_t& lastGen);

// Current live-game tracker (set by main loop; read by data task). 0 means
// "no game followed / waiting mode".
int  getActiveGamePk();
void setActiveGamePk(int gamePk);

// NewsStory layout used by both the renderer and the core-0 task. Kept here
// so the renderer owns the visible slot count (MAX_NEWS_STORIES) and the data
// task publishes into those slots under the news-stories extern in scoreboard.cpp.
struct NewsStory {
  char headline[120];
  char description[220];
};

// One-time splash screen shown at boot, before network/game state is known.
void renderBootSplash();

// Renders the full MLB linescore UI on the ST7789 TFT and updates hardware LEDs / MAX7219 matrices
void renderLinescore(const LinescoreSnapshot& linescore);

// Shows a completed plate-appearance result for a short time, then restores the live panel.
void showAtBatResult(const char* batterName, const char* description);
void updateAtBatResultDisplay();
void resetAtBatResultDisplay();

// Renders the waiting state and clears scores/count LEDs until a preferred team is live.
void renderWaiting(JsonObjectConst upcomingSchedule,
				   JsonObjectConst standings,
				   const int preferredTeamIds[3]);

// Writes the expected TFT, LED matrix, and discrete LED state to Serial.
void logLiveDisplayState(const LinescoreSnapshot& linescore, int gamePk);
void logWaitingDisplayState(const int preferredTeamIds[3]);

// Rotates the scrolling ticker in the lower TFT section (On Deck / league scores)
void rotateCarousel();

// ---- Firmware self-update UI (driven by ota_update.cpp on core 0) ----
enum class OtaStage : uint8_t {
  NONE = 0,
  DOWNLOADING,   // binary is streaming to flash (progress 0..100)
  REBOOTING,     // image verified, device about to restart
  FAILED,        // download/flash failed; normal operation resumes
};
void publishOtaStage(OtaStage stage, int progressPct);  // core-0 updater
OtaStage getOtaStage();
int getOtaProgress();
void setOtaTargetVersion(const char* version);
// Draws the "updating — do not turn off" screen while an update is in
// progress. Returns true when the OTA UI owns this frame so the caller
// (loop()) should skip normal rendering.
bool handleOtaUpdateScreen();

// Apply the latest snapshot of "other live games" to the ticker data used by
// drawBottomPanel. Excluded gamePk keeps the tracked game off the list.
void updateOtherGames(const ScheduleSnapshot& schedule, int excludeGamePk);

// Apply the latest news payload to the carousel's slide list.
void updateNewsStories(JsonObjectConst newsRoot);

// Constant shared with scoreboard.cpp so the data task's cache allocation
// (gCachedNews) doesn't exceed the renderer slot count.
static const size_t MAX_NEWS_STORIES = 10;

// Data-task → renderer setter. The renderer owns the news slot storage inside
// its anonymous namespace; this API exposes only a structured update so the
// data task can publish fresh stories without naming the internals.
struct NewsSlotUpdate {
  size_t index;
  char   headline[120];
  char   description[220];
};
void publishNewsStory(const NewsSlotUpdate& slot);
void setNewsStoryCount(size_t count);   // also resets the index to 0
size_t getNewsStoryCount();
size_t getNewsStoryIndex();
void   advanceNewsStoryIndex();
const NewsStory& getNewsStory(size_t index);
// Schedule JSON document used by the renderer (findNextGame) to populate the
// upcoming-games carousel. The data task on core 0 republishes this whenever
// a fresh schedule fetch completes. Kept as a global so the render path
// doesn't need to keep a JsonDocument around itself.
void publishUpcomingScheduleJson(JsonObjectConst src);

// Read accessor: returns the most recently published schedule JSON as a
// JsonObjectConst. Returns a null JsonObjectConst when the data task
// hasn't published one yet (e.g. right after boot, before the first
// successful schedule fetch). Used by main.cpp's renderWaiting() path.
JsonObjectConst getUpcomingScheduleJson();

// Diagnostic helpers for the "no upcoming games" screen. They report what
// the data task has actually published so we can tell at a glance whether
// the schedule JSON is empty, the document is fresh, etc.
uint32_t getUpcomingSchedulePublishedAt();   // millis() of last publish, 0 = never
size_t    getUpcomingScheduleDocSize();       // bytes allocated in gUpcomingScheduleDoc
uint32_t getScheduleFetchAttempts();          // # times fetchScheduleSnapshot was called
uint32_t getScheduleFetchSuccesses();         // # times it returned true
void        bumpScheduleFetchAttempt();       // data task calls on each fetchSchedule entry
void        bumpScheduleFetchSuccess();       // data task calls when fetchSchedule returns true
const char* getScheduleLastError();           // short tag for the most recent failure
void        setScheduleLastError(const char* err); // data task calls on each fetch failure
const char* getScheduleLastUrl();              // short tag of the last URL attempted (for diag)
void        setScheduleLastUrl(const char* url);
int         getScheduleLastHttpCode();         // last HTTP response code (-1 = never)
void        setScheduleLastHttpCode(int code);  // data task sets after each fetch
uint32_t    getScheduleLastFetchAt();          // millis() of last fetch attempt
void        setScheduleLastFetchAt(uint32_t ms);