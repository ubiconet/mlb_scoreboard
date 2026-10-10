#include "mlb_teams.h"

const MlbTeam MLB_TEAM_TABLE[] = {
  {108, "LAA"},
  {109, "ARI"},
  {110, "BAL"},
  {111, "BOS"},
  {112, "CHC"},
  {113, "CIN"},
  {114, "CLE"},
  {115, "COL"},
  {116, "DET"},
  {117, "HOU"},
  {118, "KC"},
  {119, "LAD"},
  {120, "WSH"},
  {121, "NYM"},
  {133, "OAK"},
  {134, "PIT"},
  {135, "SD"},
  {136, "SEA"},
  {137, "SF"},
  {138, "STL"},
  {139, "TB"},
  {140, "TEX"},
  {141, "TOR"},
  {142, "MIN"},
  {143, "PHI"},
  {144, "ATL"},
  {145, "CWS"},
  {146, "MIA"},
  {147, "NYY"},
  {158, "MIL"},
};
const size_t MLB_TEAM_TABLE_COUNT = sizeof(MLB_TEAM_TABLE) / sizeof(MLB_TEAM_TABLE[0]);

const char* mlbTeamAbbrev(int id, const char* fallbackStr) {
  for (size_t i = 0; i < MLB_TEAM_TABLE_COUNT; ++i) {
    if (MLB_TEAM_TABLE[i].id == id) return MLB_TEAM_TABLE[i].abbrev;
  }
  return (fallbackStr && strlen(fallbackStr) > 0) ? fallbackStr : "MLB";
}
