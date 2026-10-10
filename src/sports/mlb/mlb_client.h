#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Fetch today's schedule JSON from statsapi.mlb.com (live-game detection +
// the around-the-league ticker).
bool fetchMlbSchedule(const char* dateStr, JsonDocument& doc);

// Fetch lightweight linescore JSON for a specific game
bool fetchMlbLinescore(int gamePk, JsonDocument& doc);

// Fetch the most recent completed plate appearance for the active game.
bool fetchMlbLatestPlay(int gamePk, JsonDocument& doc);

// Fetch AL+NL division standings (one record set per division). season=0 lets
// the API default to the current season.
bool fetchMlbStandings(JsonDocument& doc, int season = 0);

// True when a specific scheduled game is final.
bool isMlbGameFinal(JsonObjectConst scheduleRoot, int gamePk);
