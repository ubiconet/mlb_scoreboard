#pragma once

#include <Arduino.h>

// Fast direct-GPIO writer for the ST7789's software-SPI bus. The Adafruit
// library's digitalWrite-based bit-bang moves ~1 Mbit/s, so repainting the
// news-ticker band takes ~180 ms — the panel visibly shows a mix of the old
// and new frame during that sweep (tearing). Writing the clock/data pins
// through the GPIO.out_w1ts/w1tc registers instead of digitalWrite is
// 10-30x faster, which makes the sweep short enough that the two frames
// overlap for less than a pixel of scroll.
//
// Only valid for the configured TFT pins (all < 32, no PSRAM banks) and the
// panel's rotation-1 landscape mapping with zero start offsets, which is
// how main.cpp initializes the display.
void fastWriteWindow(int x, int y, int w, int h, const uint16_t* pixels);
