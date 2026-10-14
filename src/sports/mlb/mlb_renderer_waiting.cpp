#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "config.h"
#include "common/hal/count_leds.h"
#include "common/hal/led_matrix.h"
#include "common/hal/tft_panel.h"
#include "common/data/time_util.h"
#include "common/ui/gfx.h"
#include "mlb_renderer.h"
#include "mlb_renderer_internal.h"
#include "mlb_state.h"
#include "mlb_teams.h"

namespace {
ScheduleSnapshot sSchedule{};
NewsSnapshot sNews{};
StandingsSnapshot sStandings{};
uint8_t sIdlePage = 0;
uint32_t sIdlePageAt = 0;

uint8_t idlePageCount() {
  return 3 + sStandings.divisionCount;
}

void drawIdlePage() {
  GFXcanvas16& canvas = tftPanel.canvas();
  canvas.fillScreen(COLOR_BG);
  canvas.setTextColor(COLOR_GOLD);
  canvas.setTextSize(3);

  if (sIdlePage == 0) {
    drawCenteredText(canvas, "No Live Games", 160, 50);
    if (sSchedule.upcoming.valid) {
      const UpcomingGameLite& next = sSchedule.upcoming;
      char matchup[32];
      snprintf(matchup, sizeof(matchup), "NEXT: %s @ %s",
               mlbTeamAbbrev(next.awayTeamId), mlbTeamAbbrev(next.homeTeamId));
      canvas.setTextColor(ST77XX_WHITE);
      canvas.setTextSize(2);
      drawCenteredText(canvas, matchup, 160, 102);
      time_t gameTime;
      tm localTime = {};
      char startTime[32] = "Start time TBD";
      if (isoDateToEpoch(next.gameDate, gameTime) &&
          localtime_r(&gameTime, &localTime) != nullptr) {
        strftime(startTime, sizeof(startTime), "Starts %a %I:%M %p", &localTime);
        if (startTime[11] == '0') {
          memmove(startTime + 11, startTime + 12, strlen(startTime + 12) + 1);
        }
      }
      canvas.setTextColor(COLOR_MUTED);
      drawCenteredText(canvas, startTime, 160, 132);
    } else {
      canvas.setTextColor(COLOR_MUTED);
      canvas.setTextSize(2);
      drawCenteredText(canvas, "Check back for the next game", 160, 112);
    }
  } else if (sIdlePage == 1) {
    drawCenteredText(canvas, "Recent Results", 160, 28);
    canvas.setTextSize(2);
    if (sSchedule.recentCount == 0) {
      canvas.setTextColor(COLOR_MUTED);
      drawCenteredText(canvas, "No completed games today", 160, 112);
    }
    for (size_t i = 0; i < sSchedule.recentCount; ++i) {
      const RecentGameLite& game = sSchedule.recent[i];
      char line[40];
      snprintf(line, sizeof(line), "%s %d  %d %s",
               mlbTeamAbbrev(game.awayTeamId), game.awayScore, game.homeScore,
               mlbTeamAbbrev(game.homeTeamId));
      canvas.setTextColor(ST77XX_WHITE);
      drawCenteredText(canvas, line, 160, 72 + static_cast<int>(i) * 38);
    }
  } else if (sIdlePage == 2) {
    drawCenteredText(canvas, "Baseball News", 160, 28);
    canvas.setTextSize(2);
    if (sNews.count == 0) {
      canvas.setTextColor(COLOR_MUTED);
      drawCenteredText(canvas, "Loading latest headlines...", 160, 112);
    } else {
      canvas.setTextColor(ST77XX_WHITE);
      drawCenteredText(canvas, sNews.headline[0], 160, 82);
      if (sNews.count > 1) {
        canvas.setTextColor(COLOR_MUTED);
        canvas.setTextSize(1);
        drawCenteredText(canvas, sNews.headline[1], 160, 142);
      }
      if (sNews.count > 2) {
        drawCenteredText(canvas, sNews.headline[2], 160, 172);
      }
    }
  } else {
    uint8_t division = sIdlePage - 3;
    if (division >= sStandings.divisionCount) {
      sIdlePage = 0;
      drawIdlePage();
      return;
    }
    drawCenteredText(canvas, sStandings.division[division], 160, 24);
    canvas.setTextColor(COLOR_MUTED);
    canvas.setTextSize(1);
    canvas.setCursor(30, 48);
    canvas.print("TEAM");
    canvas.setCursor(210, 48);
    canvas.print("W");
    canvas.setCursor(260, 48);
    canvas.print("L");
    for (uint8_t row = 0; row < sStandings.count[division]; ++row) {
      const StandingsRow& team = sStandings.rows[division][row];
      canvas.setTextColor(ST77XX_WHITE);
      canvas.setTextSize(2);
      canvas.setCursor(30, 64 + row * 30);
      canvas.print(mlbTeamAbbrev(team.teamId));
      canvas.setCursor(210, 64 + row * 30);
      canvas.print(team.wins);
      canvas.setCursor(260, 64 + row * 30);
      canvas.print(team.losses);
    }
  }
  tftPanel.pushFull();
}
}  // namespace

void renderWaiting(const ScheduleSnapshot* schedule, const NewsSnapshot* news,
                   const StandingsSnapshot* standings) {
  mlb_render::hasCurrentLiveGame = false;
  mlb_render::otherGameCount = 0;
  mlb_render::tickerSlide = 0;
  mlb_render::lastCarouselTime = millis();

  setCountLeds(0, 0, 0);
  // Release the matrices to the idle clock (sport::tick drives it); force
  // a repaint so stale game scores can't linger through waiting mode.
  invalidateMax7219Clock();

  if (schedule != nullptr) sSchedule = *schedule;
  if (news != nullptr) sNews = *news;
  if (standings != nullptr) sStandings = *standings;
  drawIdlePage();
}

void logWaitingDisplayState() {
  DBG_PRINTF(
    "[DISPLAY] state=WAITING | TFT=standby | matrices: idle clock | count "
    "LEDs: balls=0 strikes=0 outs=0 | sched att=%lu ok=%lu err=%s\n",
    (unsigned long)getScheduleFetchAttempts(),
    (unsigned long)getScheduleFetchSuccesses(), getScheduleLastError());
}

void rotateCarousel() {
  if (!mlb_render::hasCurrentLiveGame || !mlb_render::currentLinescore.valid) {
    if (millis() - sIdlePageAt >= MLB_CAROUSEL_ROTATE_MS) {
      sIdlePage = (sIdlePage + 1) % idlePageCount();
      sIdlePageAt = millis();
      drawIdlePage();
    }
    return;
  }
  if (millis() - mlb_render::lastCarouselTime >= MLB_CAROUSEL_ROTATE_MS) {
    mlb_render::tickerSlide =
        (mlb_render::tickerSlide + 1) % mlb_render::tickerSlideCount();
    renderLinescore(mlb_render::currentLinescore);  // re-uses dirty-rect path
    mlb_render::lastCarouselTime = millis();
  }
}
