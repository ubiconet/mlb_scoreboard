#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <time.h>

#include "config.h"
#include "fast_tft.h"
#include "hardware_drivers.h"
#include "scoreboard.h"
#include "team_logos.h"
#include "boot_logo.h"

extern Adafruit_ST7789 display;

namespace {
const uint16_t COLOR_BG = 0x0821;        // Dark slate
const uint16_t COLOR_CARD = 0x18A5;      // Card background
const uint16_t COLOR_GOLD = 0xFD20;      // Gold accent
const uint16_t COLOR_YELLOW = 0xFFE0;    // Count / runner yellow
const uint16_t COLOR_MUTED = 0x7BEF;     // Gray text/icon
const uint16_t COLOR_BASE_EMPTY = 0x31A6;// Dark base color
const uint16_t COLOR_LED_RED = 0xF800;    // Bright red for LED dot display
const uint16_t COLOR_LED_OFF = 0x2100;    // Dark unlit LED dot background

// Latest snapshot from the core-0 data task. Treated as the source of truth
// for everything renderable; the legacy JsonObjectConst paths are gone.
LinescoreSnapshot currentLinescore{};
uint32_t currentLinescoreGen = 0;
bool hasCurrentLiveGame = false;

// Division standings are fetched but not currently rendered (see #if 0 block below); kept
// wired up so the feature can be re-enabled without re-plumbing the fetch/render path.
JsonObjectConst currentStandings;

uint8_t tickerSlide = 0;
uint32_t lastCarouselTime = 0;
bool atBatResultVisible = false;
uint32_t atBatResultStartedAt = 0;

// NewsStory slots live at file scope so the core-0 data task can publish into
// them through the `extern NewsStory newsStories[]` declaration in scoreboard.h.
NewsStory newsStories[MAX_NEWS_STORIES];
size_t    newsStoryCount = 0;
size_t    newsStoryIndex = 0;

// Upcoming-games carousel alternates a news-story slide with each game card:
// story 1, game 1, story 2, game 2, ...
enum class UpcomingSubPage : uint8_t { NEWS_STORY = 0, GAME_INFO = 1, COUNT = 2 };
UpcomingSubPage upcomingSubPage = UpcomingSubPage::NEWS_STORY;

int newsTickerX = 0;   // current scroll offset (derived from newsTickerStartedAt)
uint32_t newsTickerStartedAt = 0;  // millis() when this story's scroll began
uint32_t lastNewsTickerFrameAt = 0;

struct UpcomingGameInfo {
  int awayTeamId;
  int homeTeamId;
  char awayName[40];
  char homeName[40];
  char gameDate[32];
};
UpcomingGameInfo upcomingGames[3];
size_t upcomingGameCount = 0;
size_t upcomingGameIndex = 0;

// Double-buffer canvas allocation (153.6 KB on ESP32-S3). If the heap is
// starved at boot (large news sprite + 4 JSON docs all live alongside this),
// new may return nullptr; we then fall back to a tiny 1×1 canvas so the
// bootloader at least paints *something* instead of crashing the loop.
GFXcanvas16& getCanvas() {
  static GFXcanvas16* canvasPtr = nullptr;
  if (canvasPtr == nullptr) {
    canvasPtr = new GFXcanvas16(320, 240);
    if (canvasPtr == nullptr) {
      // Last-ditch fallback. Anything we try to draw on this will look wrong,
      // but the loop won't crash on a null deref.
      static GFXcanvas16 stub(1, 1);
      return stub;
    }
  }
  return *canvasPtr;
}

// 64x64 RGB565 team-logo cache. Reading PROGMEM logos with pgm_read_word
// 4096 times per logo per repaint was the single biggest redraw cost on the
// live screen (each repaint drew 2 logos * 2 sized versions). After first
// decode we keep the pixels in RAM so subsequent draws are flat word loads.
// Two slots are enough for the live screen (home + away); upcoming-screen
// logos fall back to PROGMEM when their slot is occupied by a different team.
struct LogoCacheSlot {
  int  teamId;
  uint16_t pixels[64 * 64];
  bool valid;
};
LogoCacheSlot gLogoCache[2] = {};

void ensureLogoCached(int teamId, const char* abbrev) {
  if (teamId == 0) return;
  for (int i = 0; i < 2; ++i) {
    if (gLogoCache[i].valid && gLogoCache[i].teamId == teamId) return;
  }
  // Pick the LRU slot (just whichever isn't currently being repainted).
  // For 2 slots, evict whichever is not the new id.
  int slot = (gLogoCache[0].valid && gLogoCache[0].teamId != teamId) ? 0 : 1;
  if (gLogoCache[slot].valid && gLogoCache[slot].teamId == teamId) return;
  // Decode from PROGMEM (or fall back to abbrev lookup if id not in switch).
  // getTeamLogo64 takes an abbreviation, so we only fill the cache when the
  // abbreviation is the canonical switch case. Otherwise leave the slot
  // invalid and drawTeamLogo64() will use its fallback badge path.
  if (abbrev == nullptr || abbrev[0] == '\0') return;
  const uint16_t* src = getTeamLogo64(abbrev);
  if (src == nullptr) return;
  gLogoCache[slot].valid  = true;
  gLogoCache[slot].teamId = teamId;
  for (size_t i = 0; i < 64 * 64; ++i) {
    gLogoCache[slot].pixels[i] = pgm_read_word(&src[i]);
  }
}

const uint16_t* getCachedLogoPixels(int teamId) {
  for (int i = 0; i < 2; ++i) {
    if (gLogoCache[i].valid && gLogoCache[i].teamId == teamId) {
      return gLogoCache[i].pixels;
    }
  }
  return nullptr;
}

// Snapshot of other in-progress games, copied out of the schedule JSON so it
// stays valid across schedule re-fetches. 3 games per ticker slide.
struct OtherGameInfo {
  int  awayTeamId;
  int  homeTeamId;
  char awayAbbrev[8];
  char homeAbbrev[8];
  int  awayScore;
  int  homeScore;
  char inningState[8];   // Top / Bottom / Middle / End
  char inningOrdinal[8]; // e.g. "7th"
};
const size_t MAX_OTHER_GAMES = 9;
OtherGameInfo otherGames[MAX_OTHER_GAMES];
size_t otherGameCount = 0;

// Tracks the most recent linescore/play inputs so the renderer can skip
// redrawing sections that haven't changed (logos, cards, inning header, base
// diamond). Rebuilt whenever any of those inputs change.
struct RenderedFrame {
  int  gamePk;
  int  awayScore;
  int  homeScore;
  int  balls;
  int  strikes;
  int  outs;
  int  currentInning;
  uint8_t inningState;
  bool isTopInning;
  int  awayTeamId;
  int  homeTeamId;
  bool offenseFirst;
  bool offenseSecond;
  bool offenseThird;
};
RenderedFrame gRendered{};
bool gHasRendered = false;

// 5x7 Dot Matrix Font table for digits 0-9
const uint8_t LED_DIGIT_5X7[10][7] = {
  {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}, // 0
  {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}, // 1
  {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}, // 2
  {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}, // 3
  {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}, // 4
  {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}, // 5
  {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}, // 6
  {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}, // 7
  {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}, // 8
  {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}  // 9
};

// 5x5 LED Dot Matrix Arrow patterns (Up ▲ and Down ▼)
const uint8_t LED_ARROW_UP[5] = {
  0b00100,
  0b01110,
  0b11111,
  0b00100,
  0b00100
};

const uint8_t LED_ARROW_DOWN[5] = {
  0b00100,
  0b00100,
  0b11111,
  0b01110,
  0b00100
};

const char* getTeamAbbrev(int id, const char* fallbackStr) {
  switch (id) {
    case 108: return "LAA";
    case 109: return "ARI";
    case 110: return "BAL";
    case 111: return "BOS";
    case 112: return "CHC";
    case 113: return "CIN";
    case 114: return "CLE";
    case 115: return "COL";
    case 116: return "DET";
    case 117: return "HOU";
    case 118: return "KC";
    case 119: return "LAD";
    case 120: return "WSH";
    case 121: return "NYM";
    case 133: return "OAK";
    case 134: return "PIT";
    case 135: return "SD";
    case 136: return "SEA";
    case 137: return "SF";
    case 138: return "STL";
    case 139: return "TB";
    case 140: return "TEX";
    case 141: return "TOR";
    case 142: return "MIN";
    case 143: return "PHI";
    case 144: return "ATL";
    case 145: return "CWS";
    case 146: return "MIA";
    case 147: return "NYY";
    case 158: return "MIL";
    default: return (fallbackStr && strlen(fallbackStr) > 0) ? fallbackStr : "MLB";
  }
}

// Prints at most maxChars characters so size-2 names fit fixed columns.
void printClipped(Adafruit_GFX& target, const char* text, size_t maxChars) {
  char buf[64];
  strlcpy(buf, text != nullptr ? text : "", sizeof(buf));
  if (strlen(buf) > maxChars) buf[maxChars] = '\0';
  target.print(buf);
}

// "Bottom" -> "Bot", "Middle" -> "Mid", etc. for compact ticker rows.
const char* shortInningState(const char* state) {
  if (state == nullptr || state[0] == '\0') return "";
  if (strcmp(state, "Top") == 0) return "Top";
  if (strcmp(state, "Bottom") == 0) return "Bot";
  if (strcmp(state, "Middle") == 0) return "Mid";
  if (strcmp(state, "End") == 0) return "End";
  return "";
}

size_t tickerSlideCount() {
  return 1 + otherGameCount; // On Deck slide + one league game per slide
}

void drawLedDigit(Adafruit_GFX& target, int startX, int startY, int digit) {
  if (digit < 0 || digit > 9) return;
  const int spacing = 8;
  for (int row = 0; row < 7; row++) {
    uint8_t line = LED_DIGIT_5X7[digit][row];
    for (int col = 0; col < 5; col++) {
      int cx = startX + (col * spacing);
      int cy = startY + (row * spacing);
      bool active = (line & (1 << (4 - col))) != 0;
      if (active) {
        target.fillCircle(cx, cy, 3, COLOR_LED_RED);
      } else {
        target.fillCircle(cx, cy, 1, COLOR_LED_OFF);
      }
    }
  }
}

void drawLedArrow(Adafruit_GFX& target, int startX, int startY, bool isTop) {
  const int spacing = 8;
  const uint8_t* pattern = isTop ? LED_ARROW_UP : LED_ARROW_DOWN;
  for (int row = 0; row < 5; row++) {
    uint8_t line = pattern[row];
    for (int col = 0; col < 5; col++) {
      int cx = startX + (col * spacing);
      int cy = startY + (row * spacing) + 8; // Align vertically with 5x7 digits
      bool active = (line & (1 << (4 - col))) != 0;
      if (active) {
        target.fillCircle(cx, cy, 3, COLOR_LED_RED);
      } else {
        target.fillCircle(cx, cy, 1, COLOR_LED_OFF);
      }
    }
  }
}

void drawInningHeader(Adafruit_GFX& target, JsonObjectConst linescore) {
  int currentInning = linescore["currentInning"] | 1;
  const char* innState = linescore["inningState"] | "Top";
  bool isTop = (linescore["isTopInning"] | (strcmp(innState, "Top") == 0));
  bool betweenHalfInnings = strcmp(innState, "Middle") == 0 ||
                            strcmp(innState, "End") == 0;

  // Render Larger Red LED Dot Matrix Inning Display (Centered Top)
  int topY = 8;
  if (currentInning < 10) {
    // Single digit (e.g. 1-9)
    int digitX = betweenHalfInnings ? 146 : 162;
    if (!betweenHalfInnings) {
      drawLedArrow(target, 118, topY, isTop);
    }
    drawLedDigit(target, digitX, topY, currentInning);
  } else {
    // Double digit (e.g. 10+)
    int digit1X = betweenHalfInnings ? 122 : 140;
    int digit2X = betweenHalfInnings ? 166 : 184;
    if (!betweenHalfInnings) {
      drawLedArrow(target, 98, topY, isTop);
    }
    drawLedDigit(target, digit1X, topY, currentInning / 10);
    drawLedDigit(target, digit2X, topY, currentInning % 10);
  }
}

// Snapshot-driven variant for the live render path. Same pixel layout as
// drawInningHeader(JsonObjectConst) but reads from the pre-parsed snapshot.
inline void drawInningHeaderFromSnapshot(Adafruit_GFX& target, const LinescoreSnapshot& ls) {
  int currentInning = ls.currentInning;
  if (currentInning <= 0) currentInning = 1;
  bool betweenHalfInnings = (ls.inningState == 2) || (ls.inningState == 3);
  bool isTop = ls.isTopInning;
  int topY = 8;
  if (currentInning < 10) {
    int digitX = betweenHalfInnings ? 146 : 162;
    if (!betweenHalfInnings) drawLedArrow(target, 118, topY, isTop);
    drawLedDigit(target, digitX, topY, currentInning);
  } else {
    int digit1X = betweenHalfInnings ? 122 : 140;
    int digit2X = betweenHalfInnings ? 166 : 184;
    if (!betweenHalfInnings) drawLedArrow(target, 98, topY, isTop);
    drawLedDigit(target, digit1X, topY, currentInning / 10);
    drawLedDigit(target, digit2X, topY, currentInning % 10);
  }
}

void drawDiamondShape(Adafruit_GFX& target, int cx, int cy, int size, uint16_t color) {
  target.fillTriangle(cx, cy - size, cx - size, cy, cx + size, cy, color);
  target.fillTriangle(cx, cy + size, cx - size, cy, cx + size, cy, color);
}

void drawBaseDiamond(Adafruit_GFX& target, int centerX, int centerY, bool b1, bool b2, bool b3) {
  const int d = 14;  // Slightly smaller distance
  const int s = 5;   // Smaller base diamond radius

  // Basepaths outline
  target.drawLine(centerX, centerY + d, centerX + d, centerY, COLOR_MUTED);  // Home to 1B
  target.drawLine(centerX + d, centerY, centerX, centerY - d, COLOR_MUTED);  // 1B to 2B
  target.drawLine(centerX, centerY - d, centerX - d, centerY, COLOR_MUTED);  // 2B to 3B
  target.drawLine(centerX - d, centerY, centerX, centerY + d, COLOR_MUTED);  // 3B to Home

  // Home plate icon (small white pentagon)
  target.fillTriangle(centerX, centerY + d + 3, centerX - 3, centerY + d, centerX + 3, centerY + d, ST77XX_WHITE);

  // 2nd Base (Top)
  drawDiamondShape(target, centerX, centerY - d, s, b2 ? COLOR_YELLOW : COLOR_BASE_EMPTY);

  // 1st Base (Right)
  drawDiamondShape(target, centerX + d, centerY, s, b1 ? COLOR_YELLOW : COLOR_BASE_EMPTY);

  // 3rd Base (Left)
  drawDiamondShape(target, centerX - d, centerY, s, b3 ? COLOR_YELLOW : COLOR_BASE_EMPTY);
}

namespace {
constexpr uint16_t TEAM_LOGO_TRANSPARENT = 0x1909;
}

void drawTeamLogo64(Adafruit_GFX& target, int x, int y, int teamId, const char* abbrev) {
  // RAM-cache hit avoids 4096 PROGMEM reads per logo per repaint.
  const uint16_t* cached = getCachedLogoPixels(teamId);
  if (cached != nullptr) {
    target.drawRGBBitmap(x, y, cached, 64, 64);
    return;
  }
  const uint16_t* logo = getTeamLogo64(abbrev);
  if (logo != nullptr) {
    // Fill cache for next time, then draw via the cached buffer.
    ensureLogoCached(teamId, abbrev);
    cached = getCachedLogoPixels(teamId);
    if (cached != nullptr) {
      target.drawRGBBitmap(x, y, cached, 64, 64);
      return;
    }
    for (int py = 0; py < 64; ++py) {
      for (int px = 0; px < 64; ++px) {
        uint16_t color = pgm_read_word(logo + (py * 64) + px);
        if (color == TEAM_LOGO_TRANSPARENT) continue;
        target.drawPixel(x + px, y + py, color);
      }
    }
  } else {
    // Fallback badge box if team logo array is missing
    target.fillRoundRect(x, y, 64, 64, 8, COLOR_CARD);
    target.drawRoundRect(x, y, 64, 64, 8, COLOR_GOLD);
    target.setTextColor(COLOR_LED_RED);
    target.setTextSize(2);
    int textLen = strlen(abbrev);
    int tx = x + 32 - (textLen * 6);
    target.setCursor(tx, y + 24);
    target.print(abbrev);
  }
}

void drawTeamLogoScaled(Adafruit_GFX& target, int x, int y, int teamId,
                        const char* abbrev, int size) {
  // Only the 64x64 unscaled form is cached; scaled draws (used on the
  // upcoming-game screen) still walk PROGMEM, but the upcoming screen is
  // much less frequent than the live one and only renders 2 logos total.
  const uint16_t* logo = getTeamLogo64(abbrev);
  if (logo == nullptr) {
    drawTeamLogo64(target, x, y, teamId, abbrev);
    return;
  }
  for (int outputY = 0; outputY < size; outputY++) {
    int sourceY = outputY * 64 / size;
    for (int outputX = 0; outputX < size; outputX++) {
      int sourceX = outputX * 64 / size;
      uint16_t color = pgm_read_word(logo + sourceY * 64 + sourceX);
      if (color == TEAM_LOGO_TRANSPARENT) continue;
      target.drawPixel(x + outputX, y + outputY, color);
    }
  }
}

bool gameDateToEpoch(const char* gameDate, time_t& outEpoch) {
  int year;
  int month;
  int day;
  int hour;
  int minute;
  if (gameDate == nullptr ||
      sscanf(gameDate, "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute) != 5) {
    return false;
  }

  tm utcTime = {};
  utcTime.tm_year = year - 1900;
  utcTime.tm_mon = month - 1;
  utcTime.tm_mday = day;
  utcTime.tm_hour = hour;
  utcTime.tm_min = minute;
  utcTime.tm_isdst = -1;

  // mktime reads local time; compare it to the UTC fields it produced to
  // calculate the timezone offset at this specific instant, including DST.
  time_t asLocalTime = mktime(&utcTime);
  if (asLocalTime == static_cast<time_t>(-1)) return false;

  tm utcFields = {};
  gmtime_r(&asLocalTime, &utcFields);
  utcFields.tm_isdst = -1;
  time_t utcFieldsAsLocal = mktime(&utcFields);
  if (utcFieldsAsLocal == static_cast<time_t>(-1)) return false;

  outEpoch = asLocalTime + (asLocalTime - utcFieldsAsLocal);
  return true;
}

bool formatGameDateLocal(const char* gameDate, char* dateOutput, size_t dateOutputSize,
                         char* timeOutput, size_t timeOutputSize) {
  time_t epoch;
  if (!gameDateToEpoch(gameDate, epoch)) return false;

  tm localTime = {};
  localtime_r(&epoch, &localTime);
  return strftime(dateOutput, dateOutputSize, "%a, %b %d", &localTime) > 0 &&
         strftime(timeOutput, timeOutputSize, "%I:%M %p", &localTime) > 0;
}

// Formats the time remaining until gameDate as "DD:HH:MM"; false if it has already started
// or the clock hasn't been synced yet (epoch below 2024-01-01 is treated as unset).
bool formatCountdown(const char* gameDate, char* output, size_t outputSize) {
  time_t epoch;
  if (!gameDateToEpoch(gameDate, epoch)) return false;

  time_t now = time(nullptr);
  if (now < 1704067200) return false; // clock not yet synced
  if (epoch <= now) return false;

  long remaining = static_cast<long>(epoch - now);
  int days = remaining / 86400;
  int hours = (remaining % 86400) / 3600;
  int minutes = (remaining % 3600) / 60;
  snprintf(output, outputSize, "%02d:%02d:%02d", days, hours, minutes);
  return true;
}

JsonObjectConst findNextGame(JsonObjectConst scheduleRoot, int teamId) {
  if (teamId == 0) return JsonObjectConst();

  for (JsonObjectConst date : scheduleRoot["dates"].as<JsonArrayConst>()) {
    for (JsonObjectConst game : date["games"].as<JsonArrayConst>()) {
      const char* state = game["status"]["abstractGameState"] | "";
      if (strcmp(state, "Final") == 0) continue;

      int awayTeamId = game["teams"]["away"]["team"]["id"] | 0;
      int homeTeamId = game["teams"]["home"]["team"]["id"] | 0;
      if (awayTeamId == teamId || homeTeamId == teamId) return game;
    }
  }
  return JsonObjectConst();
}

// Standings sub-pages are disabled for now (see matching #if 0 block near
// renderDivisionStandings) in favor of the MLB news slides; kept for easy re-enable.
#if 0
const char* divisionName(int divisionId) {
  switch (divisionId) {
    case 200: return "AL WEST";
    case 201: return "AL EAST";
    case 202: return "AL CENTRAL";
    case 203: return "NL WEST";
    case 204: return "NL EAST";
    case 205: return "NL CENTRAL";
    default: return "DIVISION";
  }
}

// Finds the division record set (all teams in that division) containing teamId.
JsonObjectConst findDivisionRecordsForTeam(JsonObjectConst standingsRoot, int teamId) {
  if (teamId == 0) return JsonObjectConst();
  for (JsonObjectConst recordSet : standingsRoot["records"].as<JsonArrayConst>()) {
    for (JsonObjectConst teamRecord : recordSet["teamRecords"].as<JsonArrayConst>()) {
      if ((teamRecord["team"]["id"] | 0) == teamId) return recordSet;
    }
  }
  return JsonObjectConst();
}
#endif

void drawTeamCard(Adafruit_GFX& target, int x, int y, const char* label,
                  int teamId, const char* abbrev) {
  target.fillRoundRect(x, y, 88, 94, 6, COLOR_CARD);
  target.drawRoundRect(x, y, 88, 94, 6, COLOR_MUTED);

  // Card Top Label (HOME / AWAY)
  target.setTextColor(COLOR_MUTED);
  target.setTextSize(1);
  target.setCursor(x + 6, y + 4);
  target.print(label);

  // 64x64 Team Logo Centered Horizontally (RAM-cached on subsequent paints)
  int logoX = x + 12; // (88 - 64) / 2 = 12
  int logoY = y + 14;
  drawTeamLogo64(target, logoX, logoY, teamId, abbrev);

  // 3-Digit Team Abbreviation Centered Below Logo (red, matching inning display)
  target.setTextColor(COLOR_LED_RED);
  target.setTextSize(2);
  int abbrevX = x + 44 - (strlen(abbrev) * 6);
  target.setCursor(abbrevX, y + 76);
  target.print(abbrev);
}

void drawCenteredText(Adafruit_GFX& target, const char* text, int centerX, int y) {
  int16_t x1;
  int16_t y1;
  uint16_t width;
  uint16_t height;
  target.getTextBounds(text, 0, y, &x1, &y1, &width, &height);
  target.setCursor(centerX - static_cast<int>(width) / 2, y);
  target.print(text);
}

void drawTeamName(Adafruit_GFX& target, const char* name, int centerX, int topY) {
  char line1[24] = {};
  char line2[24] = {};
  char words[40];
  strlcpy(words, name != nullptr ? name : "", sizeof(words));

  char* word = strtok(words, " ");
  while (word != nullptr) {
    char candidate[24];
    if (line1[0] == '\0') {
      snprintf(candidate, sizeof(candidate), "%s", word);
    } else {
      snprintf(candidate, sizeof(candidate), "%s %s", line1, word);
    }

    if (strlen(candidate) <= 18 && line2[0] == '\0') {
      strlcpy(line1, candidate, sizeof(line1));
    } else if (line2[0] == '\0') {
      strlcpy(line2, word, sizeof(line2));
    } else {
      size_t line2Length = strlen(line2);
      if (line2Length + strlen(word) + 1 < sizeof(line2)) {
        snprintf(line2 + line2Length, sizeof(line2) - line2Length,
                 "%s%s", line2Length > 0 ? " " : "", word);
      }
    }
    word = strtok(nullptr, " ");
  }

  target.setTextColor(ST77XX_WHITE);
  target.setTextSize(1);
  if (line1[0] != '\0') drawCenteredText(target, line1, centerX, topY);
  if (line2[0] != '\0') drawCenteredText(target, line2, centerX, topY + 12);
}

void drawGamePositionFooter(GFXcanvas16& canvas) {
  canvas.drawLine(22, 204, 298, 204, COLOR_CARD);
  char position[20];
  snprintf(position, sizeof(position), "GAME %u OF %u",
           static_cast<unsigned int>(upcomingGameIndex + 1),
           static_cast<unsigned int>(upcomingGameCount));
  canvas.setTextColor(COLOR_MUTED);
  canvas.setTextSize(2);
  drawCenteredText(canvas, position, 160, 216);
}

void renderUpcomingGame() {
  setCountLeds(0, 0, 0);

  GFXcanvas16& canvas = getCanvas();
  canvas.fillScreen(COLOR_BG);

  if (upcomingGameCount == 0) {
    // Diagnostic: report what the data task has actually published so we can
    // tell at a glance whether the schedule JSON is empty, hasn't been
    // fetched yet, or just doesn't include the user's preferred teams.
    uint32_t publishedAt = getUpcomingSchedulePublishedAt();
    size_t    docSize    = getUpcomingScheduleDocSize();
    JsonObjectConst sched = getUpcomingScheduleJson();
    size_t dateCount = 0;
    size_t totalGames = 0;
    for (JsonObjectConst date : sched["dates"].as<JsonArrayConst>()) {
      ++dateCount;
      JsonArrayConst games = date["games"].as<JsonArrayConst>();
      if (!games.isNull()) totalGames += games.size();
    }
    uint32_t attempts   = getScheduleFetchAttempts();
    uint32_t successes  = getScheduleFetchSuccesses();
    const char* lastErr = getScheduleLastError();
    int         httpCode= getScheduleLastHttpCode();
    const char* lastUrl = getScheduleLastUrl();
    uint32_t    fetchAt = getScheduleLastFetchAt();

    char line1[40], line2[40], line3[40], line4[40], line5[40];
    if (publishedAt == 0) {
      // The data task hasn't successfully published yet. Surface whether
      // it's been trying at all and what the last failure was.
      snprintf(line1, sizeof(line1), "No upcoming games found");
      if (attempts == 0) {
        snprintf(line2, sizeof(line2), "Data task has not run yet");
        snprintf(line3, sizeof(line3), "Waiting for first online tick");
        line4[0] = '\0';
        line5[0] = '\0';
      } else {
        uint32_t sinceFetch = (fetchAt > 0) ? (millis() - fetchAt) / 1000 : 0;
        snprintf(line2, sizeof(line2), "att:%lu ok:%lu err:%s http:%d",
                 (unsigned long)attempts, (unsigned long)successes,
                 lastErr, httpCode);
        snprintf(line3, sizeof(line3), "%lu s since last fetch", (unsigned long)sinceFetch);
        snprintf(line4, sizeof(line4), "%s", lastUrl);
        snprintf(line5, sizeof(line5), "Waiting for first success");
      }
    } else {
      uint32_t ageSec = (millis() - publishedAt) / 1000;
      snprintf(line1, sizeof(line1), "No upcoming games found");
      snprintf(line2, sizeof(line2), "att:%lu ok:%lu err:%s http:%d",
               (unsigned long)attempts, (unsigned long)successes,
               lastErr, httpCode);
      snprintf(line3, sizeof(line3), "sched:%uB %udates %ugames %lus ago",
               (unsigned)docSize, (unsigned)dateCount, (unsigned)totalGames,
               (unsigned long)ageSec);
      snprintf(line4, sizeof(line4), "%s", lastUrl);
      snprintf(line5, sizeof(line5), "Carousel refreshes every 60s");
    }
    canvas.setTextColor(ST77XX_WHITE);
    canvas.setTextSize(1);
    drawCenteredText(canvas, line1, 160, 60);
    canvas.setTextColor(COLOR_GOLD);
    drawCenteredText(canvas, line2, 160, 80);
    canvas.setTextColor(COLOR_MUTED);
    drawCenteredText(canvas, line3, 160, 100);
    drawCenteredText(canvas, line4, 160, 120);
    drawCenteredText(canvas, line5, 160, 140);
    display.drawRGBBitmap(0, 0, canvas.getBuffer(), 320, 240);
    return;
  }

  const UpcomingGameInfo& game = upcomingGames[upcomingGameIndex];
  const char* homeAbbrev = getTeamAbbrev(game.homeTeamId, nullptr);
  const char* awayAbbrev = getTeamAbbrev(game.awayTeamId, nullptr);
  char localDate[24];
  char localTime[16];
  bool hasLocalDateTime = formatGameDateLocal(game.gameDate, localDate, sizeof(localDate),
                                              localTime, sizeof(localTime));
  char dateTimeLine[48];
  if (hasLocalDateTime) {
    snprintf(dateTimeLine, sizeof(dateTimeLine), "%s - %s", localDate, localTime);
  } else {
    snprintf(dateTimeLine, sizeof(dateTimeLine), "DATE & TIME TBD");
  }

  canvas.setTextColor(COLOR_GOLD);
  canvas.setTextSize(2);
  drawCenteredText(canvas, dateTimeLine, 160, 15);

  char countdown[16];
  if (formatCountdown(game.gameDate, countdown, sizeof(countdown))) {
    char countdownLine[32];
    snprintf(countdownLine, sizeof(countdownLine), "STARTS IN %s", countdown);
    canvas.setTextColor(ST77XX_WHITE);
    canvas.setTextSize(2);
    drawCenteredText(canvas, countdownLine, 160, 48);
  }

  const int logoSize = 88;
  drawTeamLogoScaled(canvas, 20, 82, game.homeTeamId, homeAbbrev, logoSize);
  drawTeamLogoScaled(canvas, 212, 82, game.awayTeamId, awayAbbrev, logoSize);

  canvas.setTextColor(COLOR_LED_RED);
  canvas.setTextSize(2);
  drawCenteredText(canvas, "HOSTS", 160, 121);
  drawTeamName(canvas, game.homeName, 64, 176);
  drawTeamName(canvas, game.awayName, 256, 176);

  drawGamePositionFooter(canvas);

  display.drawRGBBitmap(0, 0, canvas.getBuffer(), 320, 240);
}

// Standings sub-pages are disabled for now in favor of the MLB news slides (see the
// matching #if 0 block near divisionName/findDivisionRecordsForTeam); kept for easy re-enable.
#if 0
// Small padlock glyph shown in the playoff column once a team has clinched a berth.
void drawLockIcon(Adafruit_GFX& target, int x, int y, uint16_t color) {
  target.drawFastVLine(x + 2, y, 5, color);
  target.drawFastVLine(x + 7, y, 5, color);
  target.drawFastHLine(x + 2, y, 6, color);
  target.fillRoundRect(x, y + 5, 10, 8, 2, color);
}

// Renders the division standings for highlightTeamId, one row per division team.
void renderDivisionStandings(int highlightTeamId, const char* subtitle) {
  setMax7219Scores(MAX7219_SCORE_BLANK, MAX7219_SCORE_BLANK);
  setCountLeds(0, 0, 0);

  GFXcanvas16& canvas = getCanvas();
  canvas.fillScreen(COLOR_BG);

  JsonObjectConst recordSet = findDivisionRecordsForTeam(currentStandings, highlightTeamId);
  if (recordSet.isNull()) {
    Serial.printf("[STANDINGS] No division record found for teamId=%d (doc null=%d)\n",
                  highlightTeamId, currentStandings.isNull() ? 1 : 0);
    canvas.setTextColor(ST77XX_WHITE);
    canvas.setTextSize(1);
    drawCenteredText(canvas, "Standings unavailable", 160, 104);
    canvas.setTextColor(COLOR_MUTED);
    drawCenteredText(canvas, "Standings refresh every 5 min", 160, 124);
    display.drawRGBBitmap(0, 0, canvas.getBuffer(), 320, 240);
    return;
  }

  int divisionId = recordSet["division"]["id"] | 0;
  char header[24];
  snprintf(header, sizeof(header), "%s STANDINGS", divisionName(divisionId));

  canvas.setTextColor(COLOR_GOLD);
  canvas.setTextSize(2);
  drawCenteredText(canvas, header, 160, 12);

  canvas.setTextColor(COLOR_MUTED);
  canvas.setTextSize(1);
  drawCenteredText(canvas, subtitle, 160, 34);

  // Column x-positions: rank, team (narrow), W, L, GB, PO (playoff distance / clinched lock).
  const int colRank = 18;
  const int colTeam = 44;
  const int colWins = 148;
  const int colLosses = 182;
  const int colGamesBack = 214;
  const int colPlayoff = 258;

  canvas.setTextColor(COLOR_MUTED);
  canvas.setTextSize(1);
  canvas.setCursor(colTeam, 48);
  canvas.print("TEAM");
  canvas.setCursor(colWins, 48);
  canvas.print("W");
  canvas.setCursor(colLosses, 48);
  canvas.print("L");
  canvas.setCursor(colGamesBack, 48);
  canvas.print("GB");
  canvas.setCursor(colPlayoff, 48);
  canvas.print("PO");

  int rowY = 66;
  const int rowHeight = 30;
  for (JsonObjectConst teamRecord : recordSet["teamRecords"].as<JsonArrayConst>()) {
    int teamId = teamRecord["team"]["id"] | 0;
    bool isHighlighted = (teamId == highlightTeamId);
    const char* teamName = teamRecord["team"]["name"] | "";
    const char* abbrev = getTeamAbbrev(teamId, teamName);
    int wins = teamRecord["wins"] | 0;
    int losses = teamRecord["losses"] | 0;
    const char* gamesBack = teamRecord["gamesBack"] | "-";
    const char* rank = teamRecord["divisionRank"] | "";
    bool clinched = teamRecord["clinched"] | false;
    const char* wildCardGamesBack = teamRecord["wildCardGamesBack"] | "-";

    if (isHighlighted) {
      canvas.fillRect(16, rowY - 3, 288, rowHeight - 4, COLOR_CARD);
    }

    uint16_t rowColor = isHighlighted ? COLOR_GOLD : ST77XX_WHITE;
    canvas.setTextColor(rowColor);
    canvas.setTextSize(2);

    canvas.setCursor(colRank, rowY);
    canvas.print(rank);

    canvas.setCursor(colTeam, rowY);
    canvas.print(abbrev);

    canvas.setCursor(colWins, rowY);
    canvas.print(wins);

    canvas.setCursor(colLosses, rowY);
    canvas.print(losses);

    canvas.setCursor(colGamesBack, rowY);
    canvas.print(gamesBack);

    if (clinched) {
      drawLockIcon(canvas, colPlayoff, rowY + 2, rowColor);
    } else {
      canvas.setCursor(colPlayoff, rowY);
      canvas.print(wildCardGamesBack);
    }

    rowY += rowHeight;
  }

  display.drawRGBBitmap(0, 0, canvas.getBuffer(), 320, 240);
}
#endif

// Draws left-aligned word-wrapped text and returns the y just past the last line drawn.
int drawWrappedText(Adafruit_GFX& target, const char* text, int x, int y, int maxWidth,
                     int charWidth, int lineHeight, int maxLines) {
  char buffer[256];
  strlcpy(buffer, text != nullptr ? text : "", sizeof(buffer));

  char lineBuf[64] = {};
  int linesDrawn = 0;
  char* word = strtok(buffer, " ");
  while (word != nullptr && linesDrawn < maxLines) {
    char candidate[64];
    snprintf(candidate, sizeof(candidate), "%s%s%s",
             lineBuf, lineBuf[0] != '\0' ? " " : "", word);
    if (static_cast<int>(strlen(candidate)) * charWidth > maxWidth && lineBuf[0] != '\0') {
      target.setCursor(x, y + linesDrawn * lineHeight);
      target.print(lineBuf);
      linesDrawn++;
      strlcpy(lineBuf, word, sizeof(lineBuf));
    } else {
      strlcpy(lineBuf, candidate, sizeof(lineBuf));
    }
    word = strtok(nullptr, " ");
  }
  if (lineBuf[0] != '\0' && linesDrawn < maxLines) {
    target.setCursor(x, y + linesDrawn * lineHeight);
    target.print(lineBuf);
    linesDrawn++;
  }
  return y + linesDrawn * lineHeight;
}

// The news ticker occupies canvas rows 160..239 (TFT bottom strip). Only the
// text band inside it changes from frame to frame; the geometry follows the
// text size (classic 5x7 Adafruit font scaled by NEWS_TICKER_TEXT_SIZE).
// Repainting and pushing JUST this band per frame (~2.2x less software-SPI
// traffic than the full 80-row strip) keeps the frame rate up; the scroll
// offset is time-derived (rotateCarousel), so smaller steps land smoothly.
static const int NEWS_TICKER_TEXT_SIZE = 4;                        // 24x32 px char cells
static const int NEWS_TICKER_CHAR_W    = 6 * NEWS_TICKER_TEXT_SIZE;
static const int NEWS_TICKER_TEXT_Y    = 160 + (80 - 8 * NEWS_TICKER_TEXT_SIZE) / 2;  // 184
static const int NEWS_TICKER_BAND_TOP  = NEWS_TICKER_TEXT_Y - 2;   // 182
static const int NEWS_TICKER_BAND_H    = 8 * NEWS_TICKER_TEXT_SIZE + 4;  // 36

// Painted once per news slide (not per frame): the card background for the
// whole strip plus the red hairlines, which are static while the text
// scrolls past inside the band.
void drawNewsTickerStatic() {
  GFXcanvas16& canvas = getCanvas();
  canvas.fillRect(0, 160, 320, 80, COLOR_CARD);
  canvas.drawFastHLine(0, 160, 320, COLOR_LED_RED);
  canvas.drawFastHLine(0, 239, 320, COLOR_LED_RED);
  display.drawRGBBitmap(0, 160, canvas.getBuffer() + 160 * 320, 320, 80);
}

// Draws one frame of the scrolling news ticker: repaints only the text band
// of the already-allocated canvas and pushes just that band to the TFT. An
// earlier design pre-rasterized the strip into a side GFXcanvas16 sprite
// sized (320 + textWidth) x 80, but a bare 320x80 RGB565 sprite is 51.2 KB —
// more heap than this device ever has free — so the allocation always failed
// and the news slide flashed past in ~200 ms. Drawing only the visible
// characters per frame costs no extra heap. The text is laid out on a
// virtual strip of 320 blank pixels followed by the description; the 320-px
// window offset is derived from elapsed wall time at
// MLB_NEWS_TICKER_PX_PER_SEC (see rotateCarousel), so the text enters from
// the right and exits left at an even pace.
void drawNewsTickerFrame(int offset) {
  GFXcanvas16& canvas = getCanvas();
  uint32_t startedAt = micros();

  canvas.fillRect(0, NEWS_TICKER_BAND_TOP, 320, NEWS_TICKER_BAND_H, COLOR_CARD);

  const NewsStory& story = newsStories[newsStoryIndex];
  const char* text = story.description[0] != '\0' ? story.description
                                                  : story.headline;

  canvas.setTextColor(COLOR_LED_RED);
  canvas.setTextSize(NEWS_TICKER_TEXT_SIZE);
  canvas.setTextWrap(false);
  int firstChar =
      (offset > 320) ? (offset - 320 + NEWS_TICKER_CHAR_W - 1) / NEWS_TICKER_CHAR_W
                     : 0;
  canvas.setCursor(320 + firstChar * NEWS_TICKER_CHAR_W - offset,
                   NEWS_TICKER_TEXT_Y);
  for (const char* p = text + firstChar; *p != '\0'; ++p) {
    if (canvas.getCursorX() >= 320) break;
    canvas.print(*p);
  }
  canvas.setTextWrap(true);
  // Direct-GPIO push: ~10-30x faster than drawRGBBitmap's digitalWrite
  // bit-bang, so the old/new frame overlap during the sweep stays below a
  // pixel of scroll (no visible tearing).
  fastWriteWindow(0, NEWS_TICKER_BAND_TOP, 320, NEWS_TICKER_BAND_H,
                  canvas.getBuffer() + NEWS_TICKER_BAND_TOP * 320);

  // One timing line per story (not per frame) so the effective scroll pace
  // is visible on Serial when tuning FRAME_MS / PX_PER_SEC.
  static size_t sTimedStory = SIZE_MAX;
  if (sTimedStory != newsStoryIndex) {
    sTimedStory = newsStoryIndex;
    DBG_PRINTF("[TICKER] band frame %lu us; target %d px/s (step ~%d px)\n",
               (unsigned long)(micros() - startedAt),
               MLB_NEWS_TICKER_PX_PER_SEC,
               (int)((MLB_NEWS_TICKER_PX_PER_SEC *
                      (unsigned long)(micros() - startedAt)) / 1000000));
  }
}

// Total pixel width of the current story's ticker text. Used by
// rotateCarousel() to detect when it has scrolled past.
int newsTickerTextPx() {
  const NewsStory& story = newsStories[newsStoryIndex];
  const char* text = story.description[0] != '\0' ? story.description
                                                  : story.headline;
  return (int)strlen(text) * NEWS_TICKER_CHAR_W;
}

// Renders one MLB news headline/description slide, shown between upcoming-game cards.
void renderNewsStory() {
  setCountLeds(0, 0, 0);

  GFXcanvas16& canvas = getCanvas();
  canvas.fillScreen(COLOR_BG);

  canvas.setTextColor(COLOR_GOLD);
  canvas.setTextSize(1);
  drawCenteredText(canvas, "MLB NEWS", 160, 10);

  if (newsStoryCount == 0) {
    canvas.setTextColor(ST77XX_WHITE);
    canvas.setTextSize(1);
    drawCenteredText(canvas, "No news available", 160, 104);
    canvas.setTextColor(COLOR_MUTED);
    drawCenteredText(canvas, "News refreshes every 30 min", 160, 124);
    display.drawRGBBitmap(0, 0, canvas.getBuffer(), 320, 240);
    return;
  }

  const NewsStory& story = newsStories[newsStoryIndex];

  // Headline fills the top two-thirds in bold white block text.
  canvas.setTextColor(ST77XX_WHITE);
  canvas.setTextSize(2);
  drawWrappedText(canvas, story.headline, 16, 32, 288, 12, 22, 6);

  // Paint the static strip (background + hairlines) once, then the ticker's
  // first frame into the text band, then send the headline section above.
  drawNewsTickerStatic();
  newsTickerStartedAt = millis();
  newsTickerX = 0;
  drawNewsTickerFrame(newsTickerX);
  display.drawRGBBitmap(0, 0, canvas.getBuffer(), 320, 160);
}

// Dispatches to the current upcoming-games carousel sub-page (news story or game card).
void renderUpcomingCarouselPage() {
  if (upcomingGameCount == 0) {
    renderUpcomingGame();
    return;
  }
  // Skip the news slide entirely when no stories are available, rather than showing a blank one.
  UpcomingSubPage pageToRender = upcomingSubPage;
  if (pageToRender == UpcomingSubPage::NEWS_STORY && newsStoryCount == 0) {
    pageToRender = UpcomingSubPage::GAME_INFO;
  }
  switch (pageToRender) {
    case UpcomingSubPage::NEWS_STORY:
      renderNewsStory();
      break;
    case UpcomingSubPage::GAME_INFO:
    default:
      renderUpcomingGame();
      break;
  }
}

void drawBottomPanel(Adafruit_GFX& target, const LinescoreSnapshot& ls) {
  // Expanded Bottom Section (Fills empty vertical space from Y=108 to Y=232)
  target.fillRect(8, 108, 304, 124, COLOR_CARD);
  target.drawRoundRect(8, 108, 304, 124, 6, ST77XX_WHITE);

  // Fixed row: current batter (left) and current pitcher (right)
  target.setTextSize(1);
  target.setTextColor(COLOR_GOLD);
  target.setCursor(18, 116);
  target.print("AT BAT");
  target.setCursor(166, 116);
  target.print("PITCHING");
  target.drawLine(161, 112, 161, 158, COLOR_MUTED);

  target.setTextColor(ST77XX_WHITE);
  target.setTextSize(2);
  target.setCursor(18, 132);
  printClipped(target, ls.batter[0] ? ls.batter : "Unknown Batter", 11);
  target.setCursor(166, 132);
  printClipped(target, ls.pitcher[0] ? ls.pitcher : "Unknown Pitcher", 11);

  // Scrolling detail zone below the fixed row
  target.drawLine(14, 160, 306, 160, COLOR_MUTED);
  if (tickerSlide >= tickerSlideCount()) tickerSlide = 0;

  if (tickerSlide == 0) {
    // Lineup context: on deck (large), in the hole + catcher (compact)
    target.setTextSize(1);
    target.setTextColor(COLOR_GOLD);
    target.setCursor(18, 168);
    target.print("ON DECK");

    target.setTextColor(ST77XX_WHITE);
    target.setTextSize(2);
    target.setCursor(18, 182);
    printClipped(target, ls.onDeck[0] ? ls.onDeck : "--", 23);

    char line[64];
    snprintf(line, sizeof(line), "In the hole: %s   Catcher: %s",
             ls.inHole[0] ? ls.inHole : "--",
             ls.catcher[0] ? ls.catcher : "--");
    target.setTextSize(1);
    target.setTextColor(COLOR_MUTED);
    target.setCursor(18, 208);
    printClipped(target, line, 47);
  } else {
    // Around the league: ONE other live game per slide, in large type —
    // the earlier 3-rows-of-size-1 layout was unreadable at a glance.
    target.setTextSize(1);
    target.setTextColor(COLOR_GOLD);
    target.setCursor(18, 168);
    target.print("AROUND THE LEAGUE");

    const OtherGameInfo& game = otherGames[tickerSlide - 1];

    // Inning tag on the header row, right-aligned.
    char tag[24] = "";
    const char* half = shortInningState(game.inningState);
    if (half[0] != '\0' && game.inningOrdinal[0] != '\0') {
      snprintf(tag, sizeof(tag), "%s %s", half, game.inningOrdinal);
    }
    if (tag[0] != '\0') {
      int16_t x1, y1;
      uint16_t tw, th;
      target.getTextBounds(tag, 0, 0, &x1, &y1, &tw, &th);
      target.setCursor(302 - (int)tw, 168);
      target.print(tag);
    }

    // Away row then home row: abbreviation left, score right-aligned, both
    // at textSize 3 (18 px per char cell).
    char score[8];
    target.setTextSize(3);
    target.setTextColor(ST77XX_WHITE);
    target.setCursor(18, 180);
    target.print(game.awayAbbrev);
    snprintf(score, sizeof(score), "%d", game.awayScore);
    target.setTextColor(COLOR_GOLD);
    target.setCursor(302 - (int)(strlen(score) * 18), 180);
    target.print(score);

    target.setTextColor(ST77XX_WHITE);
    target.setCursor(18, 206);
    target.print(game.homeAbbrev);
    snprintf(score, sizeof(score), "%d", game.homeScore);
    target.setTextColor(COLOR_GOLD);
    target.setCursor(302 - (int)(strlen(score) * 18), 206);
    target.print(score);
  }
}
} // namespace

// One-time splash shown at boot before Wi-Fi/game state is known: the provided
// MLB Scoreboard logo artwork (src/mlbscoreboard.png, baked into boot_logo.h).
void renderBootSplash() {
  GFXcanvas16& canvas = getCanvas();
  canvas.fillScreen(COLOR_BG);

  int x = (320 - BOOT_LOGO_WIDTH) / 2;
  int y = (240 - BOOT_LOGO_HEIGHT) / 2;
  for (int py = 0; py < BOOT_LOGO_HEIGHT; py++) {
    for (int px = 0; px < BOOT_LOGO_WIDTH; px++) {
      uint16_t color = pgm_read_word(BOOT_LOGO_MLB + (py * BOOT_LOGO_WIDTH) + px);
      canvas.drawPixel(x + px, y + py, color);
    }
  }

  canvas.setTextColor(COLOR_GOLD);
  canvas.setTextSize(1);
  drawCenteredText(canvas, FIRMWARE_VERSION, 160, 226);

  display.drawRGBBitmap(0, 0, canvas.getBuffer(), 320, 240);
}

void showAtBatResult(const char* batterName, const char* event,
                     const char* description) {
  GFXcanvas16& canvas = getCanvas();
  canvas.fillScreen(COLOR_BG);

  canvas.setTextColor(COLOR_GOLD);
  canvas.setTextSize(2);
  drawCenteredText(canvas, "AT-BAT RESULT", 160, 18);
  canvas.drawFastHLine(24, 44, 272, COLOR_GOLD);

  canvas.setTextColor(ST77XX_WHITE);
  canvas.setTextSize(2);
  drawWrappedText(canvas, batterName, 20, 62, 280, 12, 22, 2);

  // The short outcome ("Single", "Strikeout", "Home Run") is the headline
  // the card exists for; the sentence below carries the detail.
  canvas.setTextColor(COLOR_LED_RED);
  canvas.setTextSize(3);
  drawCenteredText(canvas, event[0] != '\0' ? event : "", 160, 116);

  canvas.setTextColor(ST77XX_WHITE);
  canvas.setTextSize(1);
  drawWrappedText(canvas, description, 20, 160, 280, 6, 12, 5);

  display.drawRGBBitmap(0, 0, canvas.getBuffer(), 320, 240);
  // The card clobbered the whole screen: invalidate the linescore's
  // dirty-rect tracker so the return render repaints everything. Without
  // this the tracker saw "nothing changed" and the card stayed stuck on
  // screen after its dwell expired.
  gHasRendered = false;
  atBatResultStartedAt = millis();
  atBatResultVisible = true;
  DBG_PRINTF("[DISPLAY] at-bat result: %s — %s\n", batterName,
             event[0] != '\0' ? event : description);
}

void updateAtBatResultDisplay() {
  if (!atBatResultVisible ||
      millis() - atBatResultStartedAt < MLB_AT_BAT_RESULT_DISPLAY_MS) {
    return;
  }
  atBatResultVisible = false;
  if (hasCurrentLiveGame && currentLinescore.valid) {
    renderLinescore(currentLinescore);
  }
}

void resetAtBatResultDisplay() {
  atBatResultVisible = false;
  atBatResultStartedAt = 0;
}

bool isAtBatResultVisible() { return atBatResultVisible; }

void renderLinescore(const LinescoreSnapshot& ls) {
  currentLinescore    = ls;
  hasCurrentLiveGame  = true;
  currentLinescoreGen = millis();  // local-only; not used for dirty diff

  bool betweenHalfInnings = (ls.inningState == 2) || (ls.inningState == 3);

  // Determine what's different from the last rendered frame. Static portions
  // (team cards, logos, base diamond, inning header) only repaint when one of
  // their inputs actually changes; the bottom panel + score LEDs always update.
  bool teamsChanged    = !gHasRendered ||
      ls.awayTeamId != gRendered.awayTeamId || ls.homeTeamId != gRendered.homeTeamId;
  bool scoreChanged    = !gHasRendered ||
      ls.awayScore != gRendered.awayScore || ls.homeScore != gRendered.homeScore;
  bool countChanged    = !gHasRendered ||
      ls.balls != gRendered.balls || ls.strikes != gRendered.strikes ||
      ls.outs != gRendered.outs;
  bool inningChanged   = !gHasRendered ||
      ls.currentInning != gRendered.currentInning ||
      ls.inningState != gRendered.inningState ||
      ls.isTopInning != gRendered.isTopInning;
  bool basesChanged    = !gHasRendered ||
      ls.offenseFirst != gRendered.offenseFirst ||
      ls.offenseSecond != gRendered.offenseSecond ||
      ls.offenseThird != gRendered.offenseThird;

  bool needsFullRepaint = teamsChanged || inningChanged || basesChanged;

  GFXcanvas16& canvas = getCanvas();
  if (needsFullRepaint) {
    canvas.fillScreen(COLOR_BG);
  }

  // Hardware outputs (scores + 7 count LEDs) update independently of the TFT
  // buffer so even a no-pixels-changed frame keeps the LEDs accurate.
  if (scoreChanged || teamsChanged || inningChanged) {
    setMax7219Scores(ls.awayScore, ls.homeScore);
  }
  if (countChanged || teamsChanged || inningChanged) {
    if (betweenHalfInnings) setCountLeds(0, 0, 0);
    else                    setCountLeds(ls.balls, ls.strikes, ls.outs);
  }

  // Pre-warm logo cache for the two teams shown this frame so the first
  // drawTeamCard call is the cached memcpy path.
  if (teamsChanged) {
    // We don't have abbreviations in the snapshot; getTeamAbbrev() does the
    // id->abbrev switch, so we call it twice to warm the cache with the
    // resolved abbreviation strings.
    const char* aAb = getTeamAbbrev(ls.awayTeamId, nullptr);
    const char* hAb = getTeamAbbrev(ls.homeTeamId, nullptr);
    ensureLogoCached(ls.awayTeamId, aAb);
    ensureLogoCached(ls.homeTeamId, hAb);
  }

  // Static portions (only when their inputs changed).
  if (needsFullRepaint) {
    int awayTeamId = ls.awayTeamId ? ls.awayTeamId : ls.offenseTeamId;
    int homeTeamId = ls.homeTeamId ? ls.homeTeamId : ls.defenseTeamId;
    if (ls.isTopInning) {
      // Offense is batting away in the top, home in the bottom; the snapshot
      // already encodes this via awayTeamId/homeTeamId when present.
    } else if (awayTeamId == 0) {
      awayTeamId = ls.defenseTeamId;
      homeTeamId = ls.offenseTeamId;
    }

    const char* awayAbbrev = getTeamAbbrev(awayTeamId, nullptr);
    const char* homeAbbrev = getTeamAbbrev(homeTeamId, nullptr);

    // HOME Team Card on LEFT (X=8), AWAY Team Card on RIGHT (X=224)
    drawTeamCard(canvas, 8,   6, "HOME", homeTeamId, homeAbbrev);
    drawTeamCard(canvas, 224, 6, "AWAY", awayTeamId, awayAbbrev);

    // Center Base Runners Diagram
    drawBaseDiamond(canvas, 160, 74,
                    ls.offenseFirst, ls.offenseSecond, ls.offenseThird);

    // Top Red LED Dot Matrix Inning Header
    drawInningHeaderFromSnapshot(canvas, ls);
  }

  // Bottom panel + ticker always re-render (it changes every cycle anyway).
  drawBottomPanel(canvas, ls);
  lastCarouselTime = millis();

  // Single-pass push to physical TFT display (flicker-free)
  display.drawRGBBitmap(0, 0, canvas.getBuffer(), 320, 240);

  gRendered.gamePk        = ls.gamePk;
  gRendered.awayScore     = ls.awayScore;
  gRendered.homeScore     = ls.homeScore;
  gRendered.balls         = ls.balls;
  gRendered.strikes       = ls.strikes;
  gRendered.outs          = ls.outs;
  gRendered.currentInning = ls.currentInning;
  gRendered.inningState   = ls.inningState;
  gRendered.isTopInning   = ls.isTopInning;
  gRendered.awayTeamId    = ls.awayTeamId ? ls.awayTeamId : ls.offenseTeamId;
  gRendered.homeTeamId    = ls.homeTeamId ? ls.homeTeamId : ls.defenseTeamId;
  gRendered.offenseFirst  = ls.offenseFirst;
  gRendered.offenseSecond = ls.offenseSecond;
  gRendered.offenseThird  = ls.offenseThird;
  gHasRendered = true;
}

void renderWaiting(JsonObjectConst upcomingSchedule,
                   JsonObjectConst standings,
                   const int preferredTeamIds[3]) {
  hasCurrentLiveGame = false;
  invalidateMax7219Clock();
  otherGameCount = 0;
  tickerSlide = 0;
  upcomingGameCount = 0;
  upcomingGameIndex = 0;
  upcomingSubPage = UpcomingSubPage::NEWS_STORY;
  newsTickerStartedAt = millis();
  newsTickerX = 0;
  lastNewsTickerFrameAt = 0;
  currentStandings = standings;
  int listedTeamIds[3] = {};

  for (size_t priority = 0; priority < 3; priority++) {
    int selectedTeamId = preferredTeamIds[priority];
    if (selectedTeamId == 0) continue;

    bool alreadyListed = false;
    for (size_t index = 0; index < upcomingGameCount; index++) {
      if (selectedTeamId == listedTeamIds[index]) {
        alreadyListed = true;
        break;
      }
    }
    if (alreadyListed) continue;

    JsonObjectConst game = findNextGame(upcomingSchedule, selectedTeamId);
    if (game.isNull()) continue;

    upcomingGames[upcomingGameCount].awayTeamId =
      game["teams"]["away"]["team"]["id"] | 0;
    upcomingGames[upcomingGameCount].homeTeamId =
      game["teams"]["home"]["team"]["id"] | 0;
    strlcpy(upcomingGames[upcomingGameCount].awayName,
            game["teams"]["away"]["team"]["name"] | "Away Team",
            sizeof(upcomingGames[upcomingGameCount].awayName));
    strlcpy(upcomingGames[upcomingGameCount].homeName,
            game["teams"]["home"]["team"]["name"] | "Home Team",
            sizeof(upcomingGames[upcomingGameCount].homeName));
    strlcpy(upcomingGames[upcomingGameCount].gameDate,
            game["gameDate"] | "",
            sizeof(upcomingGames[upcomingGameCount].gameDate));
    listedTeamIds[upcomingGameCount] = selectedTeamId;
    upcomingGameCount++;
  }

  lastCarouselTime = millis();
  DBG_PRINTF("[SCHED] renderer received %u upcoming card(s)\n",
             (unsigned)upcomingGameCount);
  renderUpcomingCarouselPage();
}

void logLiveDisplayState(const LinescoreSnapshot& ls, int gamePk) {
  const char* inningStateName = "Unknown";
  switch (ls.inningState) {
    case 0: inningStateName = "Top";    break;
    case 1: inningStateName = "Bottom"; break;
    case 2: inningStateName = "Middle"; break;
    case 3: inningStateName = "End";    break;
  }
  DBG_PRINTF(
    "[DISPLAY] state=LIVE_GAME gamePk=%d | TFT=live panel, %s %d, ticker=%u "
    "| matrices: home=%d away=%d | count LEDs: balls=%u strikes=%u outs=%u\n",
    gamePk, inningStateName, ls.currentInning, tickerSlide,
    ls.homeScore, ls.awayScore, ls.balls, ls.strikes, ls.outs);
}

void logWaitingDisplayState(const int preferredTeamIds[3]) {
  DBG_PRINTF(
    "[DISPLAY] state=WAITING | TFT=waiting panel | matrices: home=0 away=0 "
    "| count LEDs: balls=0 strikes=0 outs=0 | priorities=[%d,%d,%d]\n",
    preferredTeamIds[0], preferredTeamIds[1], preferredTeamIds[2]);
}

void rotateCarousel() {
  if (!hasCurrentLiveGame || !currentLinescore.valid) {
    if (upcomingGameCount == 0) return;

    if (newsStoryCount == 0) {
      if (millis() - lastCarouselTime < MLB_UPCOMING_GAMES_ROTATE_MS) return;
      upcomingGameIndex = (upcomingGameIndex + 1) % upcomingGameCount;
      upcomingSubPage = UpcomingSubPage::GAME_INFO;
      lastCarouselTime = millis();
      renderUpcomingGame();
      return;
    }

    if (upcomingSubPage == UpcomingSubPage::NEWS_STORY && newsStoryCount > 0) {
      uint32_t now = millis();
      if (now - lastNewsTickerFrameAt < MLB_NEWS_TICKER_FRAME_MS) return;
      lastNewsTickerFrameAt = now;
      // Derive the offset from wall-clock time instead of accumulating a
      // step per rendered frame: the render loop's pacing varies with the
      // rest of the UI, and a fixed step turned that jitter into visible
      // double-steps (tearing). Time-based positioning renders the correct
      // position for whenever the frame actually lands.
      newsTickerX = (int)(((uint64_t)(now - newsTickerStartedAt) *
                           MLB_NEWS_TICKER_PX_PER_SEC) / 1000);

      // The window has passed the trailing edge of the text once it has
      // advanced past the 320-px lead-in plus the text width (plus a small
      // pad so the last characters clear the left edge).
      if (newsTickerX > 320 + newsTickerTextPx() + 20) {
        upcomingSubPage = UpcomingSubPage::GAME_INFO;
        lastCarouselTime = now;
        // Actually paint the game card here: without this, the GAME_INFO
        // dwell showed the frozen final ticker frame for its whole 5 s and
        // then moved on — the upcoming-game cards never appeared.
        renderUpcomingGame();
        return;
      }
      drawNewsTickerFrame(newsTickerX);
      return;
    }

    if (millis() - lastCarouselTime < MLB_UPCOMING_GAMES_ROTATE_MS) return;
    lastCarouselTime = millis();

    uint8_t nextSubPage = static_cast<uint8_t>(upcomingSubPage) + 1;
    if (nextSubPage >= static_cast<uint8_t>(UpcomingSubPage::COUNT)) {
      nextSubPage = 0;
      upcomingGameIndex = (upcomingGameIndex + 1) % upcomingGameCount;
    }
    upcomingSubPage = static_cast<UpcomingSubPage>(nextSubPage);
    if (upcomingSubPage == UpcomingSubPage::NEWS_STORY && newsStoryCount > 0) {
      newsStoryIndex = (newsStoryIndex + 1) % newsStoryCount;
      newsTickerStartedAt = millis();
      newsTickerX = 0;
      lastNewsTickerFrameAt = 0;
    }
    renderUpcomingCarouselPage();
    return;
  }

  // Live game: rotate the On Deck / Around-the-League ticker slide.
  if (millis() - lastCarouselTime >= MLB_CAROUSEL_ROTATE_MS) {
    tickerSlide = (tickerSlide + 1) % tickerSlideCount();
    renderLinescore(currentLinescore);  // re-uses dirty-rect path
    lastCarouselTime = millis();
  }
}

void updateOtherGames(const ScheduleSnapshot& sched, int excludeGamePk) {
  otherGameCount = 0;
  for (size_t i = 0; i < sched.otherCount && otherGameCount < MAX_OTHER_GAMES; ++i) {
    const OtherGameLite& g = sched.others[i];
    // The snapshot stores team-id digits in the abbrev field as a placeholder;
    // resolve the canonical abbreviation through getTeamAbbrev() here so the
    // ticker rendering code is unchanged.
    int awayTeamId = atoi(g.awayAbbrev);
    int homeTeamId = atoi(g.homeAbbrev);
    if (awayTeamId == excludeGamePk || homeTeamId == excludeGamePk) continue;
    OtherGameInfo& info = otherGames[otherGameCount++];
    info.awayTeamId   = awayTeamId;
    info.homeTeamId   = homeTeamId;
    strlcpy(info.awayAbbrev, getTeamAbbrev(awayTeamId, nullptr), sizeof(info.awayAbbrev));
    strlcpy(info.homeAbbrev, getTeamAbbrev(homeTeamId, nullptr), sizeof(info.homeAbbrev));
    info.awayScore    = g.awayScore;
    info.homeScore    = g.homeScore;
    strlcpy(info.inningState,   g.inningState,   sizeof(info.inningState));
    strlcpy(info.inningOrdinal,  g.inningOrdinal, sizeof(info.inningOrdinal));
  }
}

void updateNewsStories(JsonObjectConst newsRoot) {
  newsStoryCount = 0;
  for (JsonObjectConst article : newsRoot["articles"].as<JsonArrayConst>()) {
    if (newsStoryCount >= MAX_NEWS_STORIES) break;
    NewsStory& story = newsStories[newsStoryCount];
    strlcpy(story.headline, article["headline"] | "", sizeof(story.headline));
    strlcpy(story.description, article["description"] | "", sizeof(story.description));
    if (story.headline[0] == '\0') continue; // Skip malformed entries
    newsStoryCount++;
  }
  if (newsStoryIndex >= newsStoryCount) {
    newsStoryIndex = 0;
  }
}

// Publisher accessors for the core-0 data task. These wrap the renderer-owned
// newsStories[] storage inside the anonymous namespace so the data task can
// refresh the news cache without naming internals.
void publishNewsStory(const NewsSlotUpdate& slot) {
  if (slot.index >= MAX_NEWS_STORIES) return;
  strlcpy(newsStories[slot.index].headline, slot.headline,
          sizeof(newsStories[slot.index].headline));
  strlcpy(newsStories[slot.index].description, slot.description,
          sizeof(newsStories[slot.index].description));
}
void setNewsStoryCount(size_t count) {
  newsStoryCount = (count > MAX_NEWS_STORIES) ? MAX_NEWS_STORIES : count;
  newsStoryIndex = 0;
}
size_t getNewsStoryCount()  { return newsStoryCount; }
size_t getNewsStoryIndex()  { return newsStoryIndex; }
void   advanceNewsStoryIndex() { newsStoryIndex = (newsStoryIndex + 1) % newsStoryCount; }
const NewsStory& getNewsStory(size_t index) { return newsStories[index]; }

// Upcoming-schedule JSON cache used by the renderer's waiting carousel.
static JsonDocument gUpcomingScheduleDoc;
static uint32_t     gUpcomingSchedulePublishedAt = 0;
void publishUpcomingScheduleJson(JsonObjectConst src) {
  if (src.isNull()) {
    gUpcomingScheduleDoc.clear();
    gUpcomingSchedulePublishedAt = millis();
    return;
  }
  gUpcomingScheduleDoc.clear();
  if (!gUpcomingScheduleDoc.set(src)) {
    DBG_PRINTF("[SCHED] republish copy failed\n");
    gUpcomingScheduleDoc.clear();
    gUpcomingSchedulePublishedAt = millis();
    return;
  }
  gUpcomingSchedulePublishedAt = millis();
  size_t dateCount = 0, gameCount = 0;
  for (JsonObjectConst date : src["dates"].as<JsonArrayConst>()) {
    ++dateCount;
    JsonArrayConst games = date["games"].as<JsonArrayConst>();
    if (!games.isNull()) gameCount += games.size();
  }
  DBG_PRINTF("[SCHED] republished: %u dates, %u games\n",
             (unsigned)dateCount, (unsigned)gameCount);
}
JsonObjectConst getUpcomingScheduleJson() {
  return gUpcomingScheduleDoc.as<JsonObjectConst>();
}
uint32_t getUpcomingSchedulePublishedAt() { return gUpcomingSchedulePublishedAt; }
size_t   getUpcomingScheduleDocSize()     { return gUpcomingScheduleDoc.size(); }

// Cross-task counters + error tag for the schedule fetch. The data task on
// core 0 increments these every time it tries (and fails) to fetch the
// schedule. Read from the renderer when populating the "no upcoming games"
// diagnostic screen so we can tell fetch failures apart from publish ones.
static volatile uint32_t gScheduleFetchAttempts  = 0;
static volatile uint32_t gScheduleFetchSuccesses = 0;
static char             gScheduleLastError[40]   = "none";
static char             gScheduleLastUrl[80]     = "none";
static volatile int     gScheduleLastHttpCode    = -1;
static volatile uint32_t gScheduleLastFetchAt    = 0;
uint32_t getScheduleFetchAttempts()  { return gScheduleFetchAttempts; }
uint32_t getScheduleFetchSuccesses() { return gScheduleFetchSuccesses; }
void bumpScheduleFetchAttempt() { ++gScheduleFetchAttempts; }
void bumpScheduleFetchSuccess() { ++gScheduleFetchSuccesses; }
const char* getScheduleLastError()   { return gScheduleLastError; }
void setScheduleLastError(const char* err) {
  if (err == nullptr) return;
  strlcpy(gScheduleLastError, err, sizeof(gScheduleLastError));
}
const char* getScheduleLastUrl() { return gScheduleLastUrl; }
void setScheduleLastUrl(const char* url) {
  if (url == nullptr) return;
  strlcpy(gScheduleLastUrl, url, sizeof(gScheduleLastUrl));
}
int getScheduleLastHttpCode() { return (int)gScheduleLastHttpCode; }
void setScheduleLastHttpCode(int code) { gScheduleLastHttpCode = (volatile int)code; }
uint32_t getScheduleLastFetchAt() { return gScheduleLastFetchAt; }
void setScheduleLastFetchAt(uint32_t ms) { gScheduleLastFetchAt = ms; }

// ============================================================================
// Public snapshot accessors + active gamePk (consumed by mlb_data_task on core 0)
// ============================================================================

int  gActiveGamePk = 0;

int getActiveGamePk() { return gActiveGamePk; }
void setActiveGamePk(int gamePk) { gActiveGamePk = gamePk; }

// ============================================================================
// Firmware self-update UI state (written by ota_update.cpp on core 0, drawn
// by handleOtaUpdateScreen() on the render loop). Only POD + a small string
// cross the boundary, same pattern as the feed snapshots.
// ============================================================================
namespace {
volatile OtaStage gOtaStage = OtaStage::NONE;
volatile int gOtaProgress = 0;
char gOtaTargetVersion[16] = "";

void renderOtaUpdateScreen() {
  GFXcanvas16& canvas = getCanvas();
  canvas.fillScreen(COLOR_BG);

  canvas.setTextColor(COLOR_GOLD);
  canvas.setTextSize(2);
  drawCenteredText(canvas, "FIRMWARE UPDATE", 160, 30);

  canvas.setTextColor(ST77XX_WHITE);
  canvas.setTextSize(1);
  if (gOtaTargetVersion[0] != '\0') {
    char line[40];
    snprintf(line, sizeof(line), "Installing %s (from %s)",
             gOtaTargetVersion, FIRMWARE_VERSION);
    drawCenteredText(canvas, line, 160, 60);
  }

  if (gOtaStage == OtaStage::DOWNLOADING) {
    drawCenteredText(canvas, "Downloading - please do not", 160, 92);
    drawCenteredText(canvas, "turn off the power", 160, 104);
    // Progress bar
    canvas.drawRoundRect(20, 140, 280, 22, 4, COLOR_MUTED);
    int fillW = (gOtaProgress > 100 ? 100 : gOtaProgress) * 276 / 100;
    if (fillW > 0) canvas.fillRoundRect(22, 142, fillW, 18, 3, COLOR_LED_RED);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", gOtaProgress);
    canvas.setTextColor(COLOR_GOLD);
    canvas.setTextSize(2);
    drawCenteredText(canvas, pct, 160, 176);
  } else if (gOtaStage == OtaStage::REBOOTING) {
    drawCenteredText(canvas, "Update installed", 160, 92);
    drawCenteredText(canvas, "Rebooting...", 160, 104);
  } else {  // FAILED
    canvas.setTextColor(COLOR_MUTED);
    drawCenteredText(canvas, "Update failed - check network", 160, 92);
    drawCenteredText(canvas, "Normal operation continues", 160, 104);
  }

  display.drawRGBBitmap(0, 0, canvas.getBuffer(), 320, 240);
}
}  // namespace

void publishOtaStage(OtaStage stage, int progressPct) {
  gOtaProgress = progressPct;
  __sync_synchronize();
  gOtaStage = stage;
}
OtaStage getOtaStage() { return gOtaStage; }
int getOtaProgress() { return gOtaProgress; }
void setOtaTargetVersion(const char* version) {
  if (version == nullptr) return;
  strlcpy(gOtaTargetVersion, version, sizeof(gOtaTargetVersion));
}

bool handleOtaUpdateScreen() {
  if (gOtaStage == OtaStage::NONE) return false;
  // Full-screen redraw on stage change (~1.2 s per push on the software
  // SPI); progress ticks only repaint the bar/text window so the download
  // UI stays responsive without monopolizing the render loop.
  static OtaStage sLastDrawnStage = OtaStage::NONE;
  static int sLastDrawnPct = -1;
  if (gOtaStage != sLastDrawnStage) {
    sLastDrawnStage = gOtaStage;
    sLastDrawnPct = -1;
    renderOtaUpdateScreen();
    return true;
  }
  if (gOtaStage == OtaStage::DOWNLOADING && gOtaProgress != sLastDrawnPct) {
    sLastDrawnPct = gOtaProgress;
    GFXcanvas16& canvas = getCanvas();
    canvas.fillRect(20, 136, 280, 68, COLOR_BG);
    canvas.drawRoundRect(20, 140, 280, 22, 4, COLOR_MUTED);
    int fillW = (gOtaProgress > 100 ? 100 : gOtaProgress) * 276 / 100;
    if (fillW > 0) canvas.fillRoundRect(22, 142, fillW, 18, 3, COLOR_LED_RED);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", gOtaProgress);
    canvas.setTextColor(COLOR_GOLD);
    canvas.setTextSize(2);
    drawCenteredText(canvas, pct, 160, 176);
    fastWriteWindow(20, 136, 280, 68,
                    canvas.getBuffer() + 136 * 320 + 20);
  }
  return true;
}

// Generations published by the core-0 task. Captured as static so each caller
// tracks its own "last seen" generation and only re-reads when there's fresh
// data. The fetch cadence (5s/60s/30min) is entirely on core 0 — the render
// loop just samples these.
namespace {
uint32_t sLastLinescoreGen = 0;
uint32_t sLastPlayGen      = 0;
uint32_t sLastScheduleGen  = 0;

// Snapshot copies live outside the anonymous namespace (file-static) so the
// data task can publish into them. Reads are single-struct atomic copies.
LinescoreSnapshot gLiveLinescore{};
PlaySnapshot      gLivePlay{};
ScheduleSnapshot  gLiveSchedule{};
uint32_t gLiveLinescoreGen = 0;
uint32_t gLivePlayGen      = 0;
uint32_t gLiveScheduleGen  = 0;
}

bool takeLinescoreSnapshot(LinescoreSnapshot& out, uint32_t& lastGen) {
  if (gLiveLinescoreGen == lastGen) return false;
  out = gLiveLinescore;
  lastGen = gLiveLinescoreGen;
  return out.valid;
}

bool takePlaySnapshot(PlaySnapshot& out, uint32_t& lastGen) {
  if (gLivePlayGen == lastGen) return false;
  out = gLivePlay;
  lastGen = gLivePlayGen;
  return out.valid;
}

bool takeScheduleSnapshot(ScheduleSnapshot& out, uint32_t& lastGen) {
  if (gLiveScheduleGen == lastGen) return false;
  out = gLiveSchedule;
  lastGen = gLiveScheduleGen;
  return out.valid;
}

// Allow mlb_data_task.cpp to publish into our snapshot structs + bump the
// generation counters. Keeps the production side decoupled from any renderer
// state.
namespace mlb_data {
void publishLinescore(const LinescoreSnapshot& s) {
  gLiveLinescore = s;
  __sync_synchronize();
  gLiveLinescoreGen++;
}
void publishPlay(const PlaySnapshot& s) {
  gLivePlay = s;
  __sync_synchronize();
  gLivePlayGen++;
}
void publishSchedule(const ScheduleSnapshot& s) {
  gLiveSchedule = s;
  __sync_synchronize();
  gLiveScheduleGen++;
}
uint32_t currentLinescoreGen() { return gLiveLinescoreGen; }
uint32_t currentPlayGen()      { return gLivePlayGen; }
uint32_t currentScheduleGen()  { return gLiveScheduleGen; }
}


