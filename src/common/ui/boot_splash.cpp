#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "config.h"
#include "common/hal/tft_panel.h"
#include "common/ui/boot_splash.h"
#include "common/ui/gfx.h"

void renderBootSplash(int logoWidth, int logoHeight, const uint16_t* logoPixels) {
  // Drawn DIRECTLY to the panel, deliberately before the render canvas
  // exists: the boot OTA check needs the ~150 KB the canvas would hold,
  // and once freed a fragmented heap may never fit the canvas back. The
  // splash must not be the thing that allocates it.
  Adafruit_ST7789& tft = tftPanel.raw();
  tft.fillScreen(COLOR_BG);

  int x = (320 - logoWidth) / 2;
  int y = (240 - logoHeight) / 2;
  uint16_t row[320];
  for (int py = 0; py < logoHeight; py++) {
    for (int px = 0; px < logoWidth; px++) {
      row[px] = pgm_read_word(logoPixels + (py * logoWidth) + px);
    }
    tft.drawRGBBitmap(x, y + py, row, logoWidth, 1);
  }

  tft.setTextColor(COLOR_GOLD);
  tft.setTextSize(1);
  char version[24];
  snprintf(version, sizeof(version), " %s ", FIRMWARE_VERSION);
  drawCenteredText(tft, version, 160, 226);
}
