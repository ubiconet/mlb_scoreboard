#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include "config.h"
#include "common/comms/http_fetcher.h"
#include "common/comms/network_service.h"
#include "common/comms/ota_update.h"
#include "common/data/time_util.h"
#include "mlb_client.h"
#include "mlb_snapshot.h"
#include "mlb_state.h"

namespace {

// JSON workspace on core 0. Each API routine clears it before beginning a
// subsequent HTTPS request, so a previous response does not crowd TLS buffers.
JsonDocument mlbDoc;

const char* inningStateCode(const char* state) {
  if (strcmp(state, "Top") == 0)    return "Top";
  if (strcmp(state, "Bottom") == 0) return "Bot";
  if (strcmp(state, "Middle") == 0) return "Mid";
  if (strcmp(state, "End") == 0)    return "End";
  return "";
}

uint8_t classifyInningState(const char* state) {
  if (strcmp(state, "Top") == 0)    return 0;
  if (strcmp(state, "Bottom") == 0) return 1;
  if (strcmp(state, "Middle") == 0) return 2;
  if (strcmp(state, "End") == 0)    return 3;
  return 4;
}

void copyLinescoreSnapshot(int gamePk, JsonObjectConst src, LinescoreSnapshot& dst) {
  memset(&dst, 0, sizeof(dst));
  dst.gamePk         = gamePk;
  dst.awayScore      = src["teams"]["away"]["runs"]      | 0;
  dst.homeScore      = src["teams"]["home"]["runs"]      | 0;
  dst.balls          = src["balls"]                      | 0;
  dst.strikes        = src["strikes"]                    | 0;
  dst.outs           = src["outs"]                       | 0;
  dst.currentInning  = src["currentInning"]              | 0;
  const char* innState = src["inningState"] | "";
  dst.inningState    = classifyInningState(innState);
  dst.isTopInning    = (src["isTopInning"] | (strcmp(innState, "Top") == 0)) ? true : false;
  dst.offenseFirst   = !src["offense"]["first"].isNull();
  dst.offenseSecond  = !src["offense"]["second"].isNull();
  dst.offenseThird   = !src["offense"]["third"].isNull();
  dst.offenseTeamId  = src["offense"]["team"]["id"] | 0;
  dst.defenseTeamId  = src["defense"]["team"]["id"] | 0;
  // teams.away.team.id / teams.home.team.id are the most reliable ids
  // available from the linescore endpoint; offense/defense.team.id are
  // fallbacks if the teams block didn't include them.
  dst.awayTeamId     = src["teams"]["away"]["team"]["id"] | 0;
  dst.homeTeamId     = src["teams"]["home"]["team"]["id"] | 0;
  if (dst.awayTeamId == 0) {
    dst.awayTeamId = dst.isTopInning ? dst.offenseTeamId : dst.defenseTeamId;
  }
  if (dst.homeTeamId == 0) {
    dst.homeTeamId = dst.isTopInning ? dst.defenseTeamId : dst.offenseTeamId;
  }

  strlcpy(dst.batter,
          src["offense"]["batter"]["fullName"] | "",
          sizeof(dst.batter));
  strlcpy(dst.pitcher,
          src["defense"]["pitcher"]["fullName"] | "",
          sizeof(dst.pitcher));
  strlcpy(dst.onDeck,
          src["offense"]["onDeck"]["fullName"] | "",
          sizeof(dst.onDeck));
  strlcpy(dst.inHole,
          src["offense"]["inHole"]["fullName"] | "",
          sizeof(dst.inHole));
  strlcpy(dst.catcher,
          src["defense"]["catcher"]["fullName"] | "",
          sizeof(dst.catcher));
  dst.valid = true;
}

void copyPlaySnapshot(JsonObjectConst src, PlaySnapshot& dst) {
  memset(&dst, 0, sizeof(dst));
  JsonArrayConst plays = src["allPlays"].as<JsonArrayConst>();
  if (plays.isNull() || plays.size() == 0) {
    dst.valid = false;
    return;
  }
  // allPlays ends with the IN-PROGRESS at-bat (empty result fields, new
  // batter). Snapshot the latest COMPLETED play instead: its atBatIndex
  // only advances when a result actually exists, which is what the
  // renderer's card trigger keys on — the in-progress entry used to
  // consume the index early and suppress (or blank) the result card.
  JsonObjectConst latest;
  for (size_t i = plays.size(); i-- > 0;) {
    JsonObjectConst play = plays[i];
    if (play["about"]["isComplete"] | false) {
      latest = play;
      break;
    }
  }
  if (latest.isNull()) {
    dst.valid = false;  // game just started, nothing completed yet
    return;
  }
  dst.atBatIndex  = latest["about"]["atBatIndex"] | -1;
  dst.batterId    = latest["matchup"]["batter"]["id"] | 0;
  strlcpy(dst.batterName,
          latest["matchup"]["batter"]["fullName"] | "",
          sizeof(dst.batterName));
  strlcpy(dst.event,
          latest["result"]["event"] | "",
          sizeof(dst.event));
  strlcpy(dst.description,
          latest["result"]["description"] | "",
          sizeof(dst.description));
  dst.valid = true;
}

void copySchedule(JsonObjectConst scheduleRoot, ScheduleSnapshot& dst) {
  memset(&dst, 0, sizeof(dst));
  JsonArrayConst dates = scheduleRoot["dates"].as<JsonArrayConst>();
  if (dates.isNull() || dates.size() == 0) return;

  // Finals from the (possibly multi-day) window, keeping the NEWEST 3:
  // dates[] arrives oldest-first, so shift older ones out as later ones
  // arrive; the standby screen then shows them newest-first.
  RecentGameLite finals[3];
  uint8_t finalCount = 0;

  for (JsonObjectConst date : dates) {
    for (JsonObjectConst game : date["games"].as<JsonArrayConst>()) {
      const char* state = game["status"]["abstractGameState"] | "";
      if (strcmp(state, "Live") == 0 && dst.otherCount < 9) {
        OtherGameLite& slot = dst.others[dst.otherCount++];
        slot.gamePk      = game["gamePk"] | 0;
        int awayTeamId = game["teams"]["away"]["team"]["id"] | 0;
        int homeTeamId = game["teams"]["home"]["team"]["id"] | 0;
        // Store team-id digits in the abbrev fields as a placeholder; the
        // renderer's updateOtherGames() resolves the canonical abbreviation.
        snprintf(slot.awayAbbrev, sizeof(slot.awayAbbrev), "%d", awayTeamId);
        snprintf(slot.homeAbbrev, sizeof(slot.homeAbbrev), "%d", homeTeamId);
        slot.awayScore = game["teams"]["away"]["score"] | 0;
        slot.homeScore = game["teams"]["home"]["score"] | 0;
        strlcpy(slot.inningState,
                inningStateCode(game["linescore"]["inningState"] | ""),
                sizeof(slot.inningState));
        strlcpy(slot.inningOrdinal,
                game["linescore"]["currentInningOrdinal"] | "",
                sizeof(slot.inningOrdinal));
      } else if (!dst.upcoming.valid && strcmp(state, "Preview") == 0) {
        dst.upcoming.gamePk = game["gamePk"] | 0;
        dst.upcoming.awayTeamId = game["teams"]["away"]["team"]["id"] | 0;
        dst.upcoming.homeTeamId = game["teams"]["home"]["team"]["id"] | 0;
        strlcpy(dst.upcoming.gameDate, game["gameDate"] | "",
                sizeof(dst.upcoming.gameDate));
        dst.upcoming.valid = dst.upcoming.gamePk > 0;
      } else if (strcmp(state, "Final") == 0) {
        RecentGameLite final{};
        final.awayTeamId = game["teams"]["away"]["team"]["id"] | 0;
        final.homeTeamId = game["teams"]["home"]["team"]["id"] | 0;
        final.awayScore = game["teams"]["away"]["score"] | 0;
        final.homeScore = game["teams"]["home"]["score"] | 0;
        if (finalCount < 3) {
          finals[finalCount++] = final;
        } else {  // shift older out, append the newer
          finals[0] = finals[1];
          finals[1] = finals[2];
          finals[2] = final;
        }
      }
    }
  }
  // Newest-first: the most recent final at the top of the results page.
  for (uint8_t i = 0; i < finalCount; ++i) {
    dst.recent[i] = finals[finalCount - 1 - i];
  }
  dst.recentCount = finalCount;
  dst.valid = true;
}

bool fetchLinescore(int gamePk, LinescoreSnapshot& out) {
  if (!fetchMlbLinescore(gamePk, mlbDoc)) return false;
  copyLinescoreSnapshot(gamePk, mlbDoc.as<JsonObjectConst>(), out);
  mlbDoc.clear();
  return out.valid;
}

bool fetchLatestPlay(int gamePk, PlaySnapshot& out) {
  if (!fetchMlbLatestPlay(gamePk, mlbDoc)) return false;
  copyPlaySnapshot(mlbDoc.as<JsonObjectConst>(), out);
  mlbDoc.clear();
  return out.valid;
}

bool fetchScheduleSnapshot(ScheduleSnapshot& out) {
  // Bump the attempt counter immediately so the renderer can see the data
  // task is alive even when every fetch fails.
  bumpScheduleFetchAttempt();
  // Yesterday+today (a few KB per day with hydrate=linescore) for
  // live-game detection, the around-the-league ticker, the next scheduled
  // matchup, AND the recent-results page — mid-afternoon there are often
  // no finals yet today, so yesterday's games fill the page. A null date
  // (clock not yet NTP-synced) asks the server for "today" alone. The
  // endpoint includes every game type, including postseason; do not add a
  // regular-season gameType filter here.
  char startDate[11] = "";
  char endDate[11] = "";
  time_t now = time(nullptr);
  if (timeIsSynced()) {
    tm today = {};
    localtime_r(&now, &today);
    strftime(endDate, sizeof(endDate), "%Y-%m-%d", &today);
    tm yesterday = {};
    time_t dayAgo = now - 86400;
    localtime_r(&dayAgo, &yesterday);
    strftime(startDate, sizeof(startDate), "%Y-%m-%d", &yesterday);
  }
  if (!fetchMlbScheduleRange(startDate[0] != '\0' ? startDate : nullptr,
                            endDate[0] != '\0' ? endDate : nullptr, mlbDoc)) {
    setScheduleLastError("today_fetch_failed");
    return false;
  }
  copySchedule(mlbDoc.as<JsonObjectConst>(), out);
  mlbDoc.clear();

  bumpScheduleFetchSuccess();
  setScheduleLastError("ok");
  DBG_PRINTF("[SCHED] fetched OK on attempt %u\n",
             (unsigned)getScheduleFetchSuccesses());
  return out.valid;
}

void fetchIdleContentIfDue() {
  // "Next due" timestamps: success schedules the next fetch 30 minutes out,
  // while a failure (or an empty parse) retries after 3 minutes — a
  // transient network wobble must not leave the news page on
  // "Loading..." for half an hour.
  static uint32_t newsNextDueAt = 0;
  static uint32_t standingsNextDueAt = 0;
  const uint32_t now = millis();
  if (getActiveGamePk() != 0) return;

  const uint32_t OK_INTERVAL = 30UL * 60UL * 1000UL;
  const uint32_t RETRY_INTERVAL = 3UL * 60UL * 1000UL;

  if ((int32_t)(now - newsNextDueAt) >= 0) {
    bool haveNews = false;
    if (fetchEspnMlbNews(mlbDoc)) {
      NewsSnapshot news{};
      for (JsonObjectConst article : mlbDoc["articles"].as<JsonArrayConst>()) {
        if (news.count >= 3) break;
        const char* headline = article["headline"] | "";
        if (headline[0] == '\0') continue;
        strlcpy(news.headline[news.count++], headline,
                sizeof(news.headline[0]));
      }
      news.valid = news.count > 0;
      if (news.valid) {
        mlb_data::publishNews(news);
        haveNews = true;
      }
    }
    mlbDoc.clear();
    newsNextDueAt = now + (haveNews ? OK_INTERVAL : RETRY_INTERVAL);
  }

  if ((int32_t)(now - standingsNextDueAt) >= 0) {
    bool haveStandings = false;
    http_fetch::closeSession();
    if (fetchMlbStandings(mlbDoc)) {
      StandingsSnapshot standings{};
      for (JsonObjectConst record : mlbDoc["records"].as<JsonArrayConst>()) {
        if (standings.divisionCount >= 6) break;
        uint8_t division = standings.divisionCount++;
        strlcpy(standings.division[division],
                record["division"]["name"] | "Division",
                sizeof(standings.division[division]));
        for (JsonObjectConst team : record["teamRecords"].as<JsonArrayConst>()) {
          if (standings.count[division] >= 5) break;
          StandingsRow& row =
              standings.rows[division][standings.count[division]++];
          row.teamId = team["team"]["id"] | 0;
          row.wins = team["wins"] | 0;
          row.losses = team["losses"] | 0;
        }
      }
      standings.valid = standings.divisionCount > 0;
      if (standings.valid) {
        mlb_data::publishStandings(standings);
        haveStandings = true;
      }
    }
    mlbDoc.clear();
    standingsNextDueAt = now + (haveStandings ? OK_INTERVAL : RETRY_INTERVAL);
  }
}

void mlbDataTaskLoop(void*) {
  // Schedule goes first in each tick: it is the primary display feed, and at
  // boot we want it to win the race for the first (often only) healthy TLS
  // connection window.
  static uint32_t lastScheduleAt = 0;
  static bool lastScheduleOk = true;
  static uint32_t scheduleRetryInterval = MLB_SCHEDULE_RETRY_MS;

  for (;;) {
    // Yield to the network task. We only do work when online.
    static uint32_t sOnlineSince = 0;
    if (isOnline()) {
      if (sOnlineSince == 0) sOnlineSince = millis();
    } else {
      sOnlineSince = 0;
    }
    if (sOnlineSince != 0) {
      uint32_t onlineFor = millis() - sOnlineSince;
      // Firmware self-update check (manifest + OTA download). While a
      // download is running, hold off the feed fetches entirely: the TLS
      // download needs the heap and airtime to itself.
      if (otaCheckRequested()) {
        // Free the JSON pool and response buffer so a portal-triggered
        // mid-session TLS check gets the largest contiguous heap we can
        // offer (see the note at the top of ota_update.cpp).
        mlbDoc.clear();
        mlbDoc.shrinkToFit();
        http_fetch::releaseBodyBuffer();
      }
      serviceOtaUpdates(onlineFor);
      if (otaUpdateInProgress()) {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      // Portal priority mode: someone is actively using the setup pages —
      // pause all feed activity (and drop the statsapi session) so the web
      // server gets the core and the radio airtime to itself. Cached data
      // keeps the display going; feeds resume when the portal goes idle.
      if (portalEngaged()) {
        http_fetch::closeSession();
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }

      // The first OTA check runs before any feed fetch: its TLS handshake
      // needs the pristine boot heap (see otaBootGateReached).
      if (!otaBootGateReached(onlineFor)) {
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }

      uint32_t now = millis();

      // Schedule: refresh ~once a minute after a successful fetch. While the
      // last fetch failed, retry after scheduleRetryInterval, which doubles
      // per consecutive failure (capped) — hammering a network that is
      // refusing TLS connections only prolongs the lockout.
      bool firstEver = (lastScheduleAt == 0);
      bool due       = (now - lastScheduleAt >= MLB_SCHEDULE_POLL_INTERVAL_MS);
      bool retryDue  = (!lastScheduleOk &&
                        now - lastScheduleAt >= scheduleRetryInterval);
      if (firstEver || due || retryDue) {
        lastScheduleAt = now;
        ScheduleSnapshot sch;
        if (fetchScheduleSnapshot(sch)) {
          mlb_data::publishSchedule(sch);
        }
        // Judge success at the fetch level (lastError == "ok"), not by
        // out.valid — a day with no games parses fine but leaves no live
        // games to publish, and that shouldn't trigger fast retries.
        lastScheduleOk = (strcmp(getScheduleLastError(), "ok") == 0);
        if (lastScheduleOk) {
          scheduleRetryInterval = MLB_SCHEDULE_RETRY_MS;
        } else {
          scheduleRetryInterval =
              (scheduleRetryInterval * 2 > MLB_RETRY_BACKOFF_MAX_MS)
                  ? MLB_RETRY_BACKOFF_MAX_MS
                  : scheduleRetryInterval * 2;
        }
      }

      int activeGamePk = getActiveGamePk();
      fetchIdleContentIfDue();
      static uint32_t lastLivePollAt = 0;
      if (activeGamePk > 0 && now - lastLivePollAt >= MLB_LIVE_POLL_INTERVAL_MS) {
        lastLivePollAt = now;
        LinescoreSnapshot ls;
        if (fetchLinescore(activeGamePk, ls)) {
          mlb_data::publishLinescore(ls);
        }
        PlaySnapshot play;
        if (fetchLatestPlay(activeGamePk, play)) {
          mlb_data::publishPlay(play);
        }
      }

      // NOTE: the statsapi keep-alive session is deliberately left OPEN
      // between ticks. The install's network path refuses new TCP flows
      // from this device after a burst of connections, so reusing one
      // session across refreshes (and across the 5 s live polls during a
      // game) is what keeps the feeds alive. The OTA updater closes it
      // before its own connection; the next statsapi fetch reopens it.
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

} // namespace

void startMlbDataTask() {
  xTaskCreatePinnedToCore(mlbDataTaskLoop, "MlbData", 12288,
                          nullptr, 1, nullptr, 0);
}
