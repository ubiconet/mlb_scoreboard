#pragma once

#include <Arduino.h>

// Dual cascaded MAX7219 8x8 LED matrix driver (bit-banged — no SPI
// peripheral). Device 0 = Away, device 1 = Home on the MLB build; the driver
// itself only knows "away matrix / home matrix" and scores. Pins are passed
// to initLedMatrix() by the sport/app layer (from its sport_config.h).

// Pass this instead of a real score to leave a matrix dark (no game active).
static const int MAX7219_SCORE_BLANK = -1;

void initLedMatrix(int dinPin, int clkPin, int csPin);
void setMax7219Scores(int awayScore, int homeScore);
// Display-test register helpers (the portal's screen self-test): all LEDs
// on ONE matrix (position 1 = away, 2 = home) or on both at once,
// bypassing digit data at full brightness.
void setMax7219PositionTest(int position, bool on);
void setMax7219DisplayTestAll(bool on);
// Shows or clears local time: hour on the home matrix, minutes on the away
// matrix. Refreshes once per minute; waits for NTP before first display.
void updateMax7219Clock(bool enabled);
// Forces the next updateMax7219Clock(true) call to repaint even when the
// minute hasn't changed (e.g. after leaving a live game).
void invalidateMax7219Clock();

// TEST ONLY: shows "H" on the home matrix and "A" on the away matrix at boot.
void runMax7219BootTest();
