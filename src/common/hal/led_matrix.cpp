#include <Arduino.h>
#include <string.h>
#include <time.h>

#include "common/data/time_util.h"
#include "led_matrix.h"

namespace {
// Pins, captured from initLedMatrix() — the driver never reads config itself.
int DIN_PIN = -1;
int CLK_PIN = -1;
int CS_PIN = -1;

// MAX7219 Register Addresses
const uint8_t MAX7219_REG_NOOP = 0x00;
const uint8_t MAX7219_REG_DIGIT0 = 0x01;
const uint8_t MAX7219_REG_DECODEMODE = 0x09;
const uint8_t MAX7219_REG_INTENSITY = 0x0A;
const uint8_t MAX7219_REG_SCANLIMIT = 0x0B;
const uint8_t MAX7219_REG_SHUTDOWN = 0x0C;
const uint8_t MAX7219_REG_DISPLAYTEST = 0x0F;

// 3x5 font digits for 2-digit scores (0-9)
const uint8_t FONT_3X5[10][5] = {
  {0b111, 0b101, 0b101, 0b101, 0b111}, // 0
  {0b010, 0b110, 0b010, 0b010, 0b111}, // 1
  {0b111, 0b001, 0b111, 0b100, 0b111}, // 2
  {0b111, 0b001, 0b111, 0b001, 0b111}, // 3
  {0b101, 0b101, 0b111, 0b001, 0b001}, // 4
  {0b111, 0b100, 0b111, 0b001, 0b111}, // 5
  {0b111, 0b100, 0b111, 0b101, 0b111}, // 6
  {0b111, 0b001, 0b010, 0b010, 0b010}, // 7
  {0b111, 0b101, 0b111, 0b101, 0b111}, // 8
  {0b111, 0b101, 0b111, 0b001, 0b111}  // 9
};

// Bold 6x8 font digits for 1-digit values (0-9). Strokes are 2 pixels wide
// (and the horizontal bars 2 rows thick) so single-digit scores and clock
// digits read chunky and far more visible than the thin 5x7 it replaced;
// two digits cannot fit this weight in 8 columns, so values >= 10 keep the
// 3x5 pair layout above.
const uint8_t FONT_6X8_BOLD[10][8] = {
  {0b011110, 0b110011, 0b110011, 0b110011, 0b110011, 0b110011, 0b110011, 0b011110}, // 0
  {0b001100, 0b011100, 0b111100, 0b001100, 0b001100, 0b001100, 0b001100, 0b111111}, // 1
  {0b011110, 0b110011, 0b000011, 0b000110, 0b001100, 0b011000, 0b110000, 0b111111}, // 2
  {0b011110, 0b110011, 0b000011, 0b001110, 0b001110, 0b000011, 0b110011, 0b011110}, // 3
  {0b000011, 0b001111, 0b011011, 0b110011, 0b111111, 0b000011, 0b000011, 0b000011}, // 4
  {0b111111, 0b110000, 0b110000, 0b111110, 0b000011, 0b000011, 0b110011, 0b011110}, // 5
  {0b011110, 0b110011, 0b110000, 0b111110, 0b110011, 0b110011, 0b110011, 0b011110}, // 6
  {0b111111, 0b110011, 0b000011, 0b000110, 0b001100, 0b001100, 0b001100, 0b001100}, // 7
  {0b011110, 0b110011, 0b110011, 0b011110, 0b011110, 0b110011, 0b110011, 0b011110}, // 8
  {0b011110, 0b110011, 0b110011, 0b011111, 0b000011, 0b000011, 0b110011, 0b011110}  // 9
};

// 5x7 font letters used only for the boot-time matrix test (H = home, A = away)
const uint8_t LETTER_H_5X7[7] = {
  0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001
};
const uint8_t LETTER_A_5X7[7] = {
  0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001
};

int lastClockMinute = -1;
bool clockShown = false;
const uint8_t MATRIX_GAME_INTENSITY = 0x05;
const uint8_t MATRIX_CLOCK_INTENSITY = 0x01;
uint8_t matrixIntensity = MATRIX_GAME_INTENSITY;

void max7219ShiftByte(uint8_t data) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(CLK_PIN, LOW);
    digitalWrite(DIN_PIN, (data & (1 << i)) ? HIGH : LOW);
    digitalWrite(CLK_PIN, HIGH);
  }
}

// Send (reg, data) command pair to 2 cascaded MAX7219 devices: dev1 (Home) then dev0 (Away)
void max7219Send(uint8_t reg1, uint8_t data1, uint8_t reg0, uint8_t data0) {
  digitalWrite(CS_PIN, LOW);
  max7219ShiftByte(reg1);
  max7219ShiftByte(data1);
  max7219ShiftByte(reg0);
  max7219ShiftByte(data0);
  digitalWrite(CS_PIN, HIGH);
}

void max7219SendAll(uint8_t reg, uint8_t data) {
  max7219Send(reg, data, reg, data);
}

void setMatrixIntensity(uint8_t intensity) {
  if (matrixIntensity == intensity) return;
  max7219SendAll(MAX7219_REG_INTENSITY, intensity);
  matrixIntensity = intensity;
}

// Bit c of row r ("column c") lives at bit position (7-c) of rows[r].
uint8_t getMatrixBit(const uint8_t rows[8], int r, int c) {
  return (rows[r] >> (7 - c)) & 0x01;
}

void setMatrixBit(uint8_t rows[8], int r, int c, uint8_t value) {
  if (value) {
    rows[r] |= static_cast<uint8_t>(1 << (7 - c));
  } else {
    rows[r] &= static_cast<uint8_t>(~(1 << (7 - c)));
  }
}

// Physical mounting corrections, from the v2.76 rewire bench test:
// BOTH matrices mount upright (home verified v2.77, away verified v2.78
// after trying 180/CCW/CW corrections) — no rotation is applied to
// either chain's glyph frames.

// Convert integer score (0-99) into 8 row bytes for an 8x8 matrix. A negative
// score (MAX7219_SCORE_BLANK) leaves the matrix dark for "no active game".
// Single-digit values use the bold 2-px-stroke 6x8 glyph; values >= 10 fall
// back to two thin 3x5 digits side by side (a bold pair cannot fit 8 cols).
void scoreToMatrixRows(int score, bool useLargeFont, uint8_t rows[8]) {
  for (int i = 0; i < 8; i++) rows[i] = 0;
  if (score < 0) return;
  if (score > 99) score = 99;

  if (useLargeFont) {
    for (int r = 0; r < 8; r++) {
      rows[r] = (FONT_6X8_BOLD[score][r] & 0x3F) << 1; // Center 6 bits in 8 cols
    }
  } else {
    // Two 3x5 digits side by side: tens at cols 7..5, ones at cols 3..1.
    int tens = score / 10;
    int ones = score % 10;
    for (int r = 0; r < 5; r++) {
      uint8_t tensBits = FONT_3X5[tens][r] & 0x07;
      uint8_t onesBits = FONT_3X5[ones][r] & 0x07;
      rows[r + 1] = (tensBits << 4) | onesBits;
    }
  }
}

void writeMatrixRows(uint8_t awayRows[8], uint8_t homeRows[8]) {
  // Both modules mount upright — glyph rows go out unrotated.
  for (uint8_t row = 0; row < 8; row++) {
    uint8_t reg = MAX7219_REG_DIGIT0 + row;
    max7219Send(reg, homeRows[row], reg, awayRows[row]);
  }
}
} // namespace

void initLedMatrix(int dinPin, int clkPin, int csPin) {
  DIN_PIN = dinPin;
  CLK_PIN = clkPin;
  CS_PIN = csPin;

  pinMode(DIN_PIN, OUTPUT);
  pinMode(CLK_PIN, OUTPUT);
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);

  // Initialize MAX7219 registers
  max7219SendAll(MAX7219_REG_SHUTDOWN, 0x01);    // Normal operation
  max7219SendAll(MAX7219_REG_DECODEMODE, 0x00);  // Raw matrix mode
  max7219SendAll(MAX7219_REG_SCANLIMIT, 0x07);   // Scan all 8 digits
  max7219SendAll(MAX7219_REG_INTENSITY, MATRIX_GAME_INTENSITY);
  max7219SendAll(MAX7219_REG_DISPLAYTEST, 0x00); // Test off

  setMax7219Scores(MAX7219_SCORE_BLANK, MAX7219_SCORE_BLANK);
  Serial.println("[HW] MAX7219 matrix driver initialized");
}

void setMax7219Scores(int awayScore, int homeScore) {
  setMatrixIntensity(MATRIX_GAME_INTENSITY);
  uint8_t awayRows[8];
  uint8_t homeRows[8];

  const bool useLargeFont = awayScore >= 0 && homeScore >= 0 &&
                            awayScore < 10 && homeScore < 10;
  scoreToMatrixRows(awayScore, useLargeFont, awayRows);
  scoreToMatrixRows(homeScore, useLargeFont, homeRows);
  writeMatrixRows(awayRows, homeRows);
}

// Display-test register helpers (the portal's screen self-test): all LEDs
// on ONE matrix (1 = away, 2 = home) or on both at once, bypassing digit
// data. The display-test mode overrides the intensity and scan registers,
// so the lit pattern is full-brightness — exactly what a wiring check
// wants. Restoring writes digit data again (callers repaint), so only the
// `on` transitions touch the register here.
void setMax7219PositionTest(int position, bool on) {
  const uint8_t value = on ? 0x01 : 0x00;
  const uint8_t away = (position == 1) ? value : 0x00;
  const uint8_t home = (position == 2) ? value : 0x00;
  max7219Send(MAX7219_REG_DISPLAYTEST, home, MAX7219_REG_DISPLAYTEST, away);
}

void setMax7219DisplayTestAll(bool on) {
  max7219SendAll(MAX7219_REG_DISPLAYTEST, on ? 0x01 : 0x00);
}

void updateMax7219Clock(bool enabled) {
  if (!enabled) {
    if (clockShown) {
      setMax7219Scores(MAX7219_SCORE_BLANK, MAX7219_SCORE_BLANK);
      clockShown = false;
    }
    return;
  }

  time_t now = time(nullptr);
  if (!timeIsSynced()) return; // Wait until NTP has established the local clock.

  tm localTime = {};
  localtime_r(&now, &localTime);
  if (localTime.tm_min == lastClockMinute) return;

  uint8_t awayRows[8];
  uint8_t homeRows[8];
  int hour = localTime.tm_hour % 12;
  if (hour == 0) hour = 12;
  // Clock mode always uses the compact two-digit glyph on both matrices.
  // This preserves the leading zero for minutes (for example, 9:07).
  scoreToMatrixRows(localTime.tm_min, false, awayRows);
  scoreToMatrixRows(hour, false, homeRows);
  setMatrixIntensity(MATRIX_CLOCK_INTENSITY);
  writeMatrixRows(awayRows, homeRows);
  lastClockMinute = localTime.tm_min;
  clockShown = true;
}

void invalidateMax7219Clock() {
  lastClockMinute = -1;
  // Treat active-game content as clock-owned so a disabled idle clock clears it.
  clockShown = true;
}

void runMax7219BootTest() {
  uint8_t homeRows[8] = {0};
  uint8_t awayRows[8] = {0};
  for (int row = 0; row < 7; row++) {
    homeRows[row] = (LETTER_H_5X7[row] & 0x1F) << 1; // Center 5 bits in 8 cols
    awayRows[row] = (LETTER_A_5X7[row] & 0x1F) << 1;
  }

  for (uint8_t row = 0; row < 8; row++) {
    uint8_t reg = MAX7219_REG_DIGIT0 + row;
    max7219Send(reg, homeRows[row], reg, awayRows[row]);
  }

  Serial.println("[HW TEST] MAX7219 boot test: H=home, A=away");
  delay(2000);
  setMax7219Scores(MAX7219_SCORE_BLANK, MAX7219_SCORE_BLANK);
}
