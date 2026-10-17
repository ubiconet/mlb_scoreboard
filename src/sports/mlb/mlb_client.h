#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Fetch today's schedule JSON from statsapi.mlb.com (live-game detection +
// the around-the-league ticker). A null/empty date asks the server for
// "today" so the device never needs its own clock.
bool fetchMlbSchedule(const char* dateStr, JsonDocument& doc);

// Range form (inclusive): yesterday+today in one fetch for the standby
// screen's recent-results window. Null/empty strings degrade to "today".
bool fetchMlbScheduleRange(const char* startDate, const char* endDate,
                           JsonDocument& doc);

// Fetch lightweight linescore JSON for a specific game
bool fetchMlbLinescore(int gamePk, JsonDocument& doc);

// Fetch the most recent completed plate appearance for the active game.
bool fetchMlbLatestPlay(int gamePk, JsonDocument& doc);

// Fetch AL+NL division standings (one record set per division). season=0 lets
// the API default to the current season.
bool fetchMlbStandings(JsonDocument& doc, int season = 0);
bool fetchEspnMlbNews(JsonDocument& doc, int limit = 3);

// True when a specific scheduled game is final.
bool isMlbGameFinal(JsonObjectConst scheduleRoot, int gamePk);
