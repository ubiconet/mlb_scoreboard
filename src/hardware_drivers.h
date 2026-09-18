#pragma once

#include <Arduino.h>

// Pass this instead of a real score to leave a matrix dark (no game active).
static const int MAX7219_SCORE_BLANK = -1;

void initHardwareDrivers();
void setCountLeds(uint8_t balls, uint8_t strikes, uint8_t outs);
void setMax7219Scores(int awayScore, int homeScore);
// Shows or clears local 24-hour time: hour on home matrix and minutes on away matrix.
void updateMax7219Clock(bool enabled);
void invalidateMax7219Clock();

// TEST ONLY: cycles each ball/strike/out LED on for 2s, one at a time, forever.
void runCountLedTestLoop();

// TEST ONLY: shows "H" on the home matrix and "A" on the away matrix at boot.
void runMax7219BootTest();
