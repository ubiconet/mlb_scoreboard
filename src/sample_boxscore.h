#pragma once

static const char SAMPLE_BOXSCORE_JSON[] = R"json({
  "id": 2025030416,
  "gameDate": "2026-06-14",
  "gameState": "OFF",
  "periodDescriptor": { "number": 3, "periodType": "REG" },
  "clock": { "timeRemaining": "00:00", "running": false },
  "awayTeam": {
    "abbrev": "CAR", "placeName": { "default": "Carolina" },
    "commonName": { "default": "Hurricanes" }, "score": 3, "sog": 23
  },
  "homeTeam": {
    "abbrev": "VGK", "placeName": { "default": "Vegas" },
    "commonName": { "default": "Golden Knights" }, "score": 0, "sog": 22
  },
  "playerByGameStats": {
    "awayTeam": { "forwards": [
      { "name": { "default": "N. Ehlers" }, "goals": 1, "assists": 0, "points": 1 },
      { "name": { "default": "J. Blake" }, "goals": 1, "assists": 1, "points": 2 },
      { "name": { "default": "T. Hall" }, "goals": 1, "assists": 0, "points": 1 }
    ], "defense": [
      { "name": { "default": "J. Slavin" }, "goals": 0, "assists": 1, "points": 1 }
    ] },
    "homeTeam": { "forwards": [], "defense": [] }
  }
})json";