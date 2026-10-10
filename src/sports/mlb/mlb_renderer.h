#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "mlb_snapshot.h"

// Baseball renderer API (split across mlb_renderer_linescore.cpp and
// mlb_renderer_waiting.cpp). Cross-core data plumbing (snapshot channels,
// active gamePk, fetch diagnostics) lives in mlb_state.h; boot/OTA screens
// live in common/ui.

// ---- Renderers ----

// One-time splash screen shown at boot, before network/game state is known.
void renderBootSplash();

// Renders the full live-game UI on the ST7789 TFT and updates hardware
// LEDs / MAX7219 matrices.
void renderLinescore(const LinescoreSnapshot& linescore);

// Shows a completed plate-appearance result for a short time, then restores the live panel.
void showAtBatResult(const char* batterName, const char* event,
                     const char* description);
void updateAtBatResultDisplay();
void resetAtBatResultDisplay();
// True while the full-screen at-bat result card owns the display.
bool isAtBatResultVisible();

// Renders the waiting state (no live game): a simple standby screen on the
// TFT with schedule-fetch status, count LEDs dark, and the score matrices
// released to the idle clock. The v2.62 removal of team selection also
// removed the upcoming-games carousel and news slides that used to live
// here.
void renderWaiting();

// Writes the current TFT, LED matrix, and discrete LED state to Serial.
void logLiveDisplayState(const LinescoreSnapshot& linescore, int gamePk);
void logWaitingDisplayState();

// Rotates the live-game ticker in the lower TFT section (On Deck / league scores)
void rotateCarousel();

// Apply the latest snapshot of "other live games" to the ticker data used by
// drawBottomPanel. Excluded gamePk keeps the tracked game off the list.
void updateOtherGames(const ScheduleSnapshot& schedule, int excludeGamePk);

// ---- Manual mode (portal "Manual Mode" page owns the values) ----
// TFT shows the boot splash screen; the score matrices and count LEDs
// follow the portal-set values. Value-cached like the live screen.
// The splash artwork is passed in by the caller (like renderBootSplash)
// so this file never includes boot_logo.h — the logo array lives in the
// one TU that defines it (mlb_app.cpp) and must not be emitted twice.
void renderManual(int logoWidth, int logoHeight, const uint16_t* logoPixels);
// Drops the manual screen's cache so the next renderManual() repaints
// everything (called on manual-mode entry).
void invalidateManualScreen();
// Full repaint of the cached live frame with the dirty-rect tracker
// reset — used when manual mode hands the display back. No-op without a
// cached live frame.
void forceLiveRepaint();
