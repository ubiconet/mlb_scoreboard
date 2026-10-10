#pragma once

#include <Arduino.h>

// Single source of truth for team identity: the id->abbrev mapping used by
// the renderer's team cards and logo cache. The setup-portal dropdown list
// and default preferred teams that used to live here were removed along
// with team selection (v2.62) — the board now follows the first live game
// of the day, no preferences.

struct MlbTeam {
  int id;
  const char* abbrev;   // canonical 3-letter code (logos, live screen)
};

extern const MlbTeam MLB_TEAM_TABLE[];
extern const size_t MLB_TEAM_TABLE_COUNT;

// Canonical abbreviation for a team id, or `fallback` (defaults to "MLB")
// when the id is unknown and no fallback string is supplied.
const char* mlbTeamAbbrev(int teamId, const char* fallbackStr = nullptr);
