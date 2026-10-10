#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "config.h"
#include "common/hal/count_leds.h"
#include "common/hal/led_matrix.h"
#include "common/hal/tft_panel.h"
#include "common/ui/gfx.h"
#include "mlb_renderer.h"
#include "mlb_renderer_internal.h"
#include "mlb_state.h"

// Waiting-mode renderer (no live game): a simple standby screen with the
// schedule-fetch status, plus the hardware handoff (count LEDs dark, score
// matrices released to the idle clock). The upcoming-games carousel and
// news marquee that used to live here were removed in v2.62 along with
// team selection — the board now follows the first live game of the day
// instead of per-team upcoming games.

void renderWaiting() {
  mlb_render::hasCurrentLiveGame = false;
  mlb_render::otherGameCount = 0;
  mlb_render::tickerSlide = 0;
  mlb_render::lastCarouselTime = millis();

  setCountLeds(0, 0, 0);
  // Release the matrices to the idle clock (sport::tick drives it); force
  // a repaint so stale game scores can't linger through waiting mode.
  invalidateMax7219Clock();

  GFXcanvas16& canvas = tftPanel.canvas();
  canvas.fillScreen(COLOR_BG);

  canvas.setTextColor(COLOR_GOLD);
  canvas.setTextSize(3);
  drawCenteredText(canvas, "No Live Games", 160, 78);

  // Diagnostic lines reporting what the data task has actually published,
  // so a dead feed can be told apart from a quiet slate at a glance.
  // Kept short enough for the size-2 (2-px stroke) font: one fact per line.
  canvas.setTextColor(COLOR_MUTED);
  canvas.setTextSize(2);
  if (getScheduleFetchAttempts() == 0) {
    drawCenteredText(canvas, "waiting for first fetch", 160, 126);
  } else {
    char line[32];
    snprintf(line, sizeof(line), "att:%lu  ok:%lu",
             (unsigned long)getScheduleFetchAttempts(),
             (unsigned long)getScheduleFetchSuccesses());
    drawCenteredText(canvas, line, 160, 126);
    snprintf(line, sizeof(line), "err:%s  http:%d",
             getScheduleLastError(), getScheduleLastHttpCode());
    drawCenteredText(canvas, line, 160, 150);
  }
  drawCenteredText(canvas, "refreshes every 60 s", 160, 182);

  tftPanel.pushFull();
}

void logWaitingDisplayState() {
  DBG_PRINTF(
    "[DISPLAY] state=WAITING | TFT=standby | matrices: idle clock | count "
    "LEDs: balls=0 strikes=0 outs=0 | sched att=%lu ok=%lu err=%s\n",
    (unsigned long)getScheduleFetchAttempts(),
    (unsigned long)getScheduleFetchSuccesses(), getScheduleLastError());
}

void rotateCarousel() {
  // Waiting mode has no carousel anymore (upcoming-games carousel and news
  // removed in v2.62); only the live-game ticker rotates.
  if (!mlb_render::hasCurrentLiveGame || !mlb_render::currentLinescore.valid) {
    return;
  }
  if (millis() - mlb_render::lastCarouselTime >= MLB_CAROUSEL_ROTATE_MS) {
    mlb_render::tickerSlide =
        (mlb_render::tickerSlide + 1) % mlb_render::tickerSlideCount();
    renderLinescore(mlb_render::currentLinescore);  // re-uses dirty-rect path
    mlb_render::lastCarouselTime = millis();
  }
}
