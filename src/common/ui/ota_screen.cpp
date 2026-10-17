#include <Arduino.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "config.h"
#include "common/hal/tft_panel.h"
#include "common/ui/gfx.h"
#include "common/ui/ota_screen.h"

namespace {
volatile OtaStage gOtaStage = OtaStage::NONE;
volatile int gOtaProgress = 0;
char gOtaTargetVersion[16] = "";
// Set by the render loop when it has parked for stage CHECKING (rendering
// stopped, canvas freed); cleared again on stage NONE.
volatile bool gDisplayParked = false;

// The update screen draws DIRECTLY to the panel — the second sanctioned
// direct-draw screen after the AP provisioning page. During an install the
// display is parked (canvas freed so the TLS download fits the heap), so the
// canvas cannot be trusted to exist here; the panel can.
void drawOtaProgressBar(Adafruit_ST7789& tft) {
  tft.drawRoundRect(20, 140, 280, 22, 4, COLOR_MUTED);
  int fillW = (gOtaProgress > 100 ? 100 : gOtaProgress) * 276 / 100;
  if (fillW > 0) tft.fillRoundRect(22, 142, fillW, 18, 3, COLOR_LED_RED);
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", gOtaProgress);
  tft.setTextColor(COLOR_GOLD);
  tft.setTextSize(2);
  drawCenteredText(tft, pct, 160, 176);
}

void renderOtaUpdateScreen() {
  Adafruit_ST7789& tft = tftPanel.raw();
  tft.fillScreen(COLOR_BG);

  tft.setTextColor(COLOR_GOLD);
  tft.setTextSize(2);
  drawCenteredText(tft, "FIRMWARE UPDATE", 160, 30);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  if (gOtaTargetVersion[0] != '\0') {
    char line[40];
    snprintf(line, sizeof(line), "Installing %s (from %s)",
             gOtaTargetVersion, FIRMWARE_VERSION);
    drawCenteredText(tft, line, 160, 60);
  }

  if (gOtaStage == OtaStage::CHECKING) {
    drawCenteredText(tft, "Preparing to download -", 160, 92);
    drawCenteredText(tft, "please do not turn off the power", 160, 104);
  } else if (gOtaStage == OtaStage::DOWNLOADING) {
    drawCenteredText(tft, "Downloading - please do not", 160, 92);
    drawCenteredText(tft, "turn off the power", 160, 104);
    drawOtaProgressBar(tft);
  } else if (gOtaStage == OtaStage::REBOOTING) {
    drawCenteredText(tft, "Update installed", 160, 92);
    drawCenteredText(tft, "Rebooting...", 160, 104);
  } else {  // FAILED
    tft.setTextColor(COLOR_MUTED);
    drawCenteredText(tft, "Update failed - check network", 160, 92);
    drawCenteredText(tft, "Normal operation continues", 160, 104);
  }
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

bool otaDisplayParked() { return gDisplayParked; }

bool handleOtaUpdateScreen() {
  if (gOtaStage == OtaStage::NONE) {
    gDisplayParked = false;
    return false;
  }
  if (gOtaStage == OtaStage::CHECKING) {
    // Park: free the render canvas so the updater's TLS handshake gets the
    // ~150 KB it holds, then paint the update frame DIRECTLY (the canvas is
    // gone; the panel is not). The frame tells the user an update has begun
    // before the progress bar has any bytes to show.
    if (!gDisplayParked) {
      tftPanel.releaseCanvas();
      gDisplayParked = true;
      renderOtaUpdateScreen();
    }
    return true;
  }
  // Full-screen redraw on stage change; progress ticks only repaint the
  // bar/percentage window so the download UI stays responsive without
  // monopolizing the render loop (software SPI).
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
    Adafruit_ST7789& tft = tftPanel.raw();
    tft.fillRect(20, 136, 280, 68, COLOR_BG);
    drawOtaProgressBar(tft);
  }
  return true;
}
