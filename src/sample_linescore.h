#pragma once

static const char SAMPLE_LINESCORE_JSON[] = R"json({
  "copyright": "Copyright 2026 MLB Advanced Media, L.P.",
  "currentInning": 3,
  "currentInningOrdinal": "3rd",
  "inningState": "Bottom",
  "isTopInning": false,
  "scheduledInnings": 9,
  "innings": [
    { "num": 1, "ordinalNum": "1st", "home": { "runs": 0, "hits": 1, "errors": 0 }, "away": { "runs": 0, "hits": 0, "errors": 0 } },
    { "num": 2, "ordinalNum": "2nd", "home": { "runs": 0, "hits": 0, "errors": 0 }, "away": { "runs": 0, "hits": 0, "errors": 0 } },
    { "num": 3, "ordinalNum": "3rd", "home": { "runs": 1, "hits": 2, "errors": 0 }, "away": { "runs": 0, "hits": 0, "errors": 0 } }
  ],
  "teams": {
    "away": { "team": { "id": 144, "name": "Atlanta Braves", "abbreviation": "ATL" }, "runs": 0, "hits": 0, "errors": 0, "leftOnBase": 1 },
    "home": { "team": { "id": 143, "name": "Philadelphia Phillies", "abbreviation": "PHI" }, "runs": 1, "hits": 3, "errors": 0, "leftOnBase": 3 }
  },
  "defense": {
    "pitcher": { "id": 656550, "fullName": "Grant Holmes" },
    "catcher": { "id": 686948, "fullName": "Drake Baldwin" },
    "team": { "id": 144, "name": "Atlanta Braves", "abbreviation": "ATL" }
  },
  "offense": {
    "batter": { "id": 607208, "fullName": "Trea Turner" },
    "onDeck": { "id": 547180, "fullName": "Bryce Harper" },
    "inHole": { "id": 650333, "fullName": "Luis Arraez" },
    "pitcher": { "id": 666200, "fullName": "Jesús Luzardo" },
    "team": { "id": 143, "name": "Philadelphia Phillies", "abbreviation": "PHI" }
  },
  "balls": 2,
  "strikes": 1,
  "outs": 1
})json";
