#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "config.h"
#include "common/hal/tft_panel.h"
#include "common/ui/boot_splash.h"
#include "common/ui/gfx.h"

void renderBootSplash(int logoWidth, int logoHeight, const uint16_t* logoPixels) {
  // The splash's canvas() call is the FIRST allocation of the render canvas,
  // deliberately: the pristine pre-Wi-Fi heap always fits the 153.6 KB block
  // (post-association fragmentation may not — measured maxAlloc 33 KB after
  // the feeds start). This is safe now that the boot OTA check runs over
  // plain HTTP: nothing at boot needs the canvas freed. Only install-time
  // TLS parks the display (see common/comms/ota_update.cpp).
  GFXcanvas16& canvas = tftPanel.canvas();
  canvas.fillScreen(COLOR_BG);

  int x = (320 - logoWidth) / 2;
  int y = (240 - logoHeight) / 2;
  for (int py = 0; py < logoHeight; py++) {
    for (int px = 0; px < logoWidth; px++) {
      uint16_t color = pgm_read_word(logoPixels + (py * logoWidth) + px);
      canvas.drawPixel(x + px, y + py, color);
    }
  }

  canvas.setTextColor(COLOR_GOLD);
  canvas.setTextSize(1);
  drawCenteredText(canvas, FIRMWARE_VERSION, 160, 226);

  tftPanel.pushFull();
}
