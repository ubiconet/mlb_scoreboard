#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "config.h"
#include "mlb_client.h"
#include "scoreboard.h"

namespace {
// All endpoints are fetched over plain HTTP on purpose. The payloads are
// public sports data (scores, headlines), the previous TLS usage was already
// validation-free (setInsecure), and a single TLS session pinned ~45 KB of
// heap — more than the device has to spare alongside the JSON documents —
// which is what originally killed the schedule/news parses. Plain HTTP also
// avoids the handshake allocations (RSA/BIGNUM) that failed on the
// fragmented heap. Both hosts serve identical bodies over http://.
//
// Shared statsapi session: one WiFiClient + one HTTPClient reused across the
// fetches of a single data-task tick via HTTP/1.1 keep-alive, and kept open
// between ticks (the install's network path refuses new TCP flows from this
// device after a burst of them). fetchEspnMlbNews drops it before its own
// connection so the two never overlap.
WiFiClient* sApiClient = nullptr;
HTTPClient sApiHttp;
bool sApiConnected = false;

void resetMlbApiSession() {
  sApiHttp.end();
  if (sApiClient != nullptr) sApiClient->stop();
  sApiConnected = false;
}

// Performs one GET on the shared session. Retries once on a fresh
// connection (covers a keep-alive socket the server silently dropped).
int statsApiGet(const String& url, uint32_t timeoutMs) {
  if (sApiClient == nullptr) {
    sApiClient = new WiFiClient();
  }
  for (int attempt = 0; attempt < 2; attempt++) {
    sApiHttp.setReuse(true);
    sApiHttp.setTimeout(timeoutMs);
    if (!sApiHttp.begin(*sApiClient, url)) return -1;
    int code = sApiHttp.GET();
    if (code >= 0 || attempt == 1) {
      sApiConnected = (code > 0);
      return code;
    }
    // code < 0: stale keep-alive or transient — rebuild the session once.
    resetMlbApiSession();
  }
  return -1;
}

// Reusable response-body buffer. An open TLS session pins ~45 KB of heap
// (mbedTLS in/out record buffers), and the device only has ~50 KB free once
// Wi-Fi + the display canvas are up, so parsing a large JSON straight off
// the live stream overflowed (IncompleteInput / esp-sha allocation
// failures). Instead the body is read into this buffer first; the payloads
// are kept small (teamId-filtered schedule, linescore) so the subsequent
// parse fits alongside the open session. Capacity is kept between fetches
// so the same heap block is reused instead of churned.
String gResponseBody;

// Presents the buffered body to ArduinoJson as a Stream so parsed strings
// are COPIED into the JsonDocument's own pool (in-memory inputs would be
// zero-copy linked into gResponseBody, which the next fetch overwrites).
class MemoryReadStream : public Stream {
 public:
  MemoryReadStream(const char* data, size_t len)
      : data_(data), len_(len), pos_(0) {}
  int available() override { return (int)(len_ - pos_); }
  int read() override { return pos_ < len_ ? (unsigned char)data_[pos_++] : -1; }
  int peek() override { return pos_ < len_ ? (unsigned char)data_[pos_] : -1; }
  size_t write(uint8_t) override { return 0; }
 private:
  const char* data_;
  size_t len_;
  size_t pos_;
};

// Reads the response body into gResponseBody with an explicit deadline.
// HTTPClient::getString() uses an available()-polling loop that bails out
// with a silent empty/partial String as soon as connected() blips false —
// which happens routinely on this install's lossy Wi-Fi while body packets
// are still in flight. This reader waits out the gaps instead, and reads
// EXACTLY Content-Length bytes so the keep-alive connection stays clean for
// the next request on the shared session.
static bool readResponseBody(HTTPClient& http, uint32_t timeoutMs) {
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) return false;
  int remaining = http.getSize();  // Content-Length, or -1 when absent
  uint32_t deadline = millis() + timeoutMs;
  uint8_t buf[512];
  gResponseBody = "";
  while ((int)gResponseBody.length() < 0x10000 && millis() < deadline) {
    size_t avail = stream->available();
    if (avail == 0) {
      if (remaining == 0) break;  // known size fully read
      if (remaining < 0 && !stream->connected()) break;  // close-delimited end
      delay(2);
      continue;
    }
    size_t toRead = sizeof(buf);
    if (toRead > avail) toRead = avail;
    if (remaining > 0 && toRead > (size_t)remaining) toRead = (size_t)remaining;
    size_t got = stream->readBytes(buf, toRead);
    if (got == 0) continue;
    gResponseBody.concat((const char*)buf, (unsigned int)got);
    if (remaining > 0) remaining -= (int)got;
    if (remaining == 0) break;
  }
  return (remaining == 0) ||
         (remaining < 0 && gResponseBody.length() > 0);
}

// Buffers the response of an already-successful GET and parses it. Call only
// when http.GET() returned HTTP_CODE_OK. Pass a filter to restrict what is
// kept, or nullptr to parse the whole body. The connection is left open on
// success so the shared keep-alive session can serve the next request;
// callers end the session via resetMlbApiSession() on any failure path.
DeserializationError parseBufferedResponse(HTTPClient& http, JsonDocument& doc,
                                           JsonDocument* filter) {
  int contentLength = http.getSize();
  bool readOk = readResponseBody(http, 8000);
  DBG_PRINTF("[MLB] body %u bytes (content-length %d%s)\n",
             (unsigned)gResponseBody.length(), contentLength,
             readOk ? "" : ", read incomplete");
  if (!readOk) {
    return DeserializationError(DeserializationError::IncompleteInput);
  }
  MemoryReadStream stream(gResponseBody.c_str(), gResponseBody.length());
  if (filter != nullptr) {
    return deserializeJson(doc, stream, DeserializationOption::Filter(*filter));
  }
  return deserializeJson(doc, stream);
}

void buildScheduleFilter(JsonDocument& filter) {
  filter["dates"][0]["date"] = true;
  filter["dates"][0]["games"][0]["gamePk"] = true;
  filter["dates"][0]["games"][0]["gameDate"] = true;
  filter["dates"][0]["games"][0]["status"]["abstractGameState"] = true;
  filter["dates"][0]["games"][0]["teams"]["away"]["team"]["id"] = true;
  filter["dates"][0]["games"][0]["teams"]["away"]["team"]["name"] = true;
  filter["dates"][0]["games"][0]["teams"]["away"]["score"] = true;
  filter["dates"][0]["games"][0]["teams"]["home"]["team"]["id"] = true;
  filter["dates"][0]["games"][0]["teams"]["home"]["team"]["name"] = true;
  filter["dates"][0]["games"][0]["teams"]["home"]["score"] = true;
  filter["dates"][0]["games"][0]["linescore"]["inningState"] = true;
  filter["dates"][0]["games"][0]["linescore"]["currentInningOrdinal"] = true;
}
} // namespace

// Uniform debug line for every outbound API call: endpoint name + HTTP response code.
// Gated by MLB_DEBUG so the live game loop doesn't stall ~25 ms per printf on
// every linescore poll. maxAlloc is the largest contiguous block the heap can
// still satisfy — freeHeap alone hides fragmentation, and the TLS handshake
// allocations fail against maxAlloc, not freeHeap.
void logApiCall(const char* endpointName, int httpCode) {
  DBG_PRINTF("[API CALL] %s -> HTTP %d | freeHeap=%u maxAlloc=%u\n",
             endpointName, httpCode, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

// Drops the shared statsapi keep-alive session (used by the OTA updater,
// which needs the heap to itself for its TLS download connection).
void closeMlbApiSession() {
  resetMlbApiSession();
}

void releaseMlbBuffers() {
  gResponseBody = String();
}

bool fetchMlbSchedule(const char* dateStr, JsonDocument& doc) {
  // A null/empty date asks the server for today's slate (US game-day), so the
  // device never needs its own clock. hydrate+fields pulls per-game inning
  // state for the other-live-games ticker while keeping the payload ~3 KB.
  String url = "http://statsapi.mlb.com/api/v1/schedule?sportId=1"
               "&hydrate=linescore"
               "&fields=dates,date,games,gamePk,status,abstractGameState,detailedState,"
               "gameDate,"
               "linescore,currentInningOrdinal,inningState,isTopInning,"
               "teams,away,team,id,name,score,home,team,id,name,score";
  if (dateStr != nullptr && dateStr[0] != '\0') {
    url += "&date=";
    url += dateStr;
  }

  setScheduleLastUrl(url.c_str());
  setScheduleLastFetchAt(millis());

  doc.clear();
  int httpCode = statsApiGet(url, 10000);
  logApiCall("schedule", httpCode);
  setScheduleLastHttpCode(httpCode);

  if (httpCode == HTTP_CODE_OK) {
    JsonDocument filter;
    buildScheduleFilter(filter);
    DeserializationError error = parseBufferedResponse(sApiHttp, doc, &filter);
    if (!error) {
      return true;
    } else {
      DBG_PRINTF("[MLB] Schedule JSON parse error: %s\n", error.c_str());
      resetMlbApiSession();
      return false;
    }
  }
  DBG_PRINTF("[MLB] Schedule HTTP error: %d\n", httpCode);
  resetMlbApiSession();
  return false;
}

bool fetchMlbScheduleRange(const char* startDate, const char* endDate,
                           JsonDocument& doc, const int teamIds[3]) {
  HTTPClient http;
  String url = "http://statsapi.mlb.com/api/v1/schedule?sportId=1"
               "&fields=dates,date,games,gamePk,gameDate,status,abstractGameState,detailedState,"
               "teams,away,team,id,name,score,home,team,id,name,score"
               "&startDate=";
  url += startDate;
  url += "&endDate=";
  url += endDate;
  // Restrict to the preferred teams server-side: a 3-day full-slate body is
  // ~13 KB, which cannot be buffered alongside the ~45 KB an open TLS session
  // pins on this heap; three teams over 3 days is ~2 KB and fits easily.
  // findNextGame() in the renderer only ever looks these teams up anyway.
  String teamIdParam;
  for (int i = 0; i < 3; i++) {
    if (teamIds == nullptr || teamIds[i] == 0) continue;
    if (teamIdParam.length() > 0) teamIdParam += ',';
    teamIdParam += String(teamIds[i]);
  }
  if (teamIdParam.length() > 0) {
    url += "&teamId=";
    url += teamIdParam;
  }

  setScheduleLastUrl(url.c_str());
  setScheduleLastFetchAt(millis());

  doc.clear();
  int httpCode = statsApiGet(url, 10000);
  logApiCall("schedule_range", httpCode);
  setScheduleLastHttpCode(httpCode);

  if (httpCode == HTTP_CODE_OK) {
    JsonDocument filter;
    buildScheduleFilter(filter);
    DeserializationError error = parseBufferedResponse(sApiHttp, doc, &filter);
    if (!error) {
      return true;
    }
    DBG_PRINTF("[MLB] Schedule range JSON parse error: %s\n", error.c_str());
    resetMlbApiSession();
    return false;
  }
  DBG_PRINTF("[MLB] Schedule range HTTP error: %d\n", httpCode);
  resetMlbApiSession();
  return false;
}

bool fetchMlbLinescore(int gamePk, JsonDocument& doc) {
  String url = "http://statsapi.mlb.com/api/v1/game/";
  url += String(gamePk);
  url += "/linescore";

  // statsApiGet() already retries once on a fresh connection; two rounds
  // total is enough and keeps the per-poll latency bounded.
  for (int attempt = 1; attempt <= 2; attempt++) {
    int httpCode = statsApiGet(url, 4000);
    logApiCall("linescore", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      doc.clear();
      // Linescore bodies are tiny (~1.5 KB): read them via the exact-length
      // body reader so the keep-alive connection stays clean, then parse
      // the whole thing (no filter).
      DeserializationError error = parseBufferedResponse(sApiHttp, doc, nullptr);
      if (!error) {
        return true;
      }
      DBG_PRINTF("[MLB] Linescore JSON parse error (attempt %d/2): %s\n",
                    attempt, error.c_str());
      resetMlbApiSession();
    } else {
      DBG_PRINTF("[MLB] Linescore HTTP error (attempt %d/2): %d\n",
                 attempt, httpCode);
      resetMlbApiSession();
    }
  }

  DBG_PRINTF("[MLB] Linescore failed after 2 attempts\n");
  return false;
}

bool fetchMlbLatestPlay(int gamePk, JsonDocument& doc) {
  String url = "http://statsapi.mlb.com/api/v1/game/";
  url += String(gamePk);
  url += "/playByPlay?fields=allPlays,about,atBatIndex,isComplete,"
         "result,event,description,matchup,batter,fullName";

  int httpCode = statsApiGet(url, 5000);
  logApiCall("play_by_play", httpCode);

  if (httpCode == HTTP_CODE_OK) {
    doc.clear();
    JsonDocument filter;
    filter["allPlays"][0]["about"]["atBatIndex"] = true;
    filter["allPlays"][0]["about"]["isComplete"] = true;
    filter["allPlays"][0]["result"]["event"] = true;
    filter["allPlays"][0]["result"]["description"] = true;
    filter["allPlays"][0]["matchup"]["batter"]["fullName"] = true;

    DeserializationError error =
        parseBufferedResponse(sApiHttp, doc, &filter);
    if (!error) {
      return true;
    }
    DBG_PRINTF("[MLB] Play-by-play JSON parse error: %s\n", error.c_str());
    resetMlbApiSession();
    return false;
  }
  DBG_PRINTF("[MLB] Play-by-play HTTP error: %d\n", httpCode);
  resetMlbApiSession();
  return false;
}

bool fetchMlbStandings(JsonDocument& doc, int season) {
  HTTPClient http;
  String url = "http://statsapi.mlb.com/api/v1/standings?leagueId=103,104"
               "&standingsTypes=regularSeason"
               "&fields=records,division,id,teamRecords,team,id,name,"
               "divisionRank,gamesBack,wins,losses,clinched,wildCardGamesBack";
  if (season > 0) {
    url += "&season=";
    url += String(season);
  }

  WiFiClient client;  // plain HTTP — see the note at the top of this file
  http.begin(client, url);
  http.setTimeout(12000);
  int httpCode = http.GET();
  logApiCall("standings", httpCode);

  if (httpCode == HTTP_CODE_OK) {
    // The API's "fields" filter can't tell our outer "records" array apart from the
    // (much larger) per-team "records" stats blob that shares the same key name, so
    // every team's split/division/league/expected records come through regardless.
    // Filter client-side so only what we render is kept in memory.
    JsonDocument filter;
    filter["records"][0]["division"]["id"] = true;
    filter["records"][0]["teamRecords"][0]["team"]["id"] = true;
    filter["records"][0]["teamRecords"][0]["team"]["name"] = true;
    filter["records"][0]["teamRecords"][0]["wins"] = true;
    filter["records"][0]["teamRecords"][0]["losses"] = true;
    filter["records"][0]["teamRecords"][0]["divisionRank"] = true;
    filter["records"][0]["teamRecords"][0]["gamesBack"] = true;
    filter["records"][0]["teamRecords"][0]["clinched"] = true;
    filter["records"][0]["teamRecords"][0]["wildCardGamesBack"] = true;

    DeserializationError error =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (!error) {
      size_t divisionCount = doc["records"].as<JsonArrayConst>().size();
      DBG_PRINTF("[MLB] Standings parsed OK: %u divisions\n",
                    static_cast<unsigned int>(divisionCount));
      return true;
    }
    DBG_PRINTF("[MLB] Standings JSON parse error: %s\n", error.c_str());
    return false;
  }
  DBG_PRINTF("[MLB] Standings HTTP error: %d\n", httpCode);
  http.end();
  return false;
}

bool fetchEspnMlbNews(JsonDocument& doc, int limit) {
  String url = "http://site.api.espn.com/apis/site/v2/sports/baseball/mlb/news?limit=";
  url += String(limit);

  doc.clear();
  doc.shrinkToFit();
  gResponseBody = String();
  // Drop the statsapi keep-alive session so the two connections never
  // overlap; the next statsapi fetch reopens its session transparently.
  resetMlbApiSession();

  int httpCode = -1;
  for (int attempt = 0; attempt < 2 && httpCode < 0; attempt++) {
    HTTPClient http;
    WiFiClient client;  // plain HTTP — see the note at the top of this file
    http.begin(client, url);
    http.setTimeout(8000);
    httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      // ESPN's article objects are heavy (categories/athletes/links/images),
      // so filter client-side to just headline + description to keep this
      // parseable on-device. This stays a streaming parse on purpose: the
      // unfiltered body is far larger than we could ever buffer, and the
      // filtered doc (~4 KB) is trivial against a TLS-free heap.
      JsonDocument filter;
      filter["articles"][0]["headline"] = true;
      filter["articles"][0]["description"] = true;

      DeserializationError error =
          deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
      http.end();
      logApiCall("espn_mlb_news", httpCode);
      if (!error) {
        size_t articleCount = doc["articles"].as<JsonArrayConst>().size();
        DBG_PRINTF("[ESPN] News parsed OK: %u articles\n",
                   static_cast<unsigned int>(articleCount));
        return true;
      }
      DBG_PRINTF("[ESPN] News JSON parse error: %s\n", error.c_str());
      return false;
    }
    // Non-OK code: log and (if negative = transport failure) retry once on
    // a completely fresh client — fragmentation and router moods are both
    // transient.
    logApiCall("espn_mlb_news", httpCode);
    DBG_PRINTF("[ESPN] News HTTP error (attempt %d/2): %d\n", attempt + 1, httpCode);
    http.end();
  }
  return false;
}

int selectGamePkForTeams(JsonObjectConst scheduleRoot, const int preferredTeamIds[3]) {
  JsonArrayConst dates = scheduleRoot["dates"].as<JsonArrayConst>();
  if (dates.isNull() || dates.size() == 0) return 0;

  // Search for games involving preferred teams in order of priority (p1 -> p2 -> p3)
  for (int p = 0; p < 3; p++) {
    int targetTeam = preferredTeamIds[p];
    if (targetTeam == 0) continue;

    for (JsonObjectConst date : dates) {
      for (JsonObjectConst game : date["games"].as<JsonArrayConst>()) {
        int awayTeamId = game["teams"]["away"]["team"]["id"] | 0;
        int homeTeamId = game["teams"]["home"]["team"]["id"] | 0;

        if (awayTeamId == targetTeam || homeTeamId == targetTeam) {
          const char* state = game["status"]["abstractGameState"] | "";
          if (strcmp(state, "Live") == 0) {
            return game["gamePk"] | 0;
          }
        }
      }
    }
  }

  return 0;
}

bool isMlbGameFinal(JsonObjectConst scheduleRoot, int gamePk) {
  if (gamePk <= 0) return false;

  for (JsonObjectConst date : scheduleRoot["dates"].as<JsonArrayConst>()) {
    for (JsonObjectConst game : date["games"].as<JsonArrayConst>()) {
      if ((game["gamePk"] | 0) != gamePk) continue;
      const char* state = game["status"]["abstractGameState"] | "";
      return strcmp(state, "Final") == 0;
    }
  }
  return false;
}
