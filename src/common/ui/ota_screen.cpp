#include <Arduino.h>

#include <Adafruit_GFX.h>

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

void renderOtaUpdateScreen() {
  GFXcanvas16& canvas = tftPanel.canvas();
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

  tftPanel.pushFull();
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
    // ~150 KB it holds. The panel keeps showing the last pushed frame
    // (splash / status page) — no redraws until the check finishes.
    if (!gDisplayParked) {
      tftPanel.releaseCanvas();
      gDisplayParked = true;
    }
    return true;
  }
  // Full-screen redraw on stage change (~1.2 s per push on the software
  // SPI); progress ticks only repaint the bar/text window so the download
  // UI stays responsive without monopolizing the render loop.
  static OtaStage sLastDrawnStage = OtaStage::NONE;
  static int sLastDrawnPct = -1;
  if (gOtaStage != sLastDrawnStage) {
    sLastDrawnStage = gOtaStage;
    sLastDrawnPct = -1;
    renderOtaUpdateScreen();  // canvas re-allocates lazily here (post-park)
    return true;
  }
  if (gOtaStage == OtaStage::DOWNLOADING && gOtaProgress != sLastDrawnPct) {
    sLastDrawnPct = gOtaProgress;
    GFXcanvas16& canvas = tftPanel.canvas();
    canvas.fillRect(20, 136, 280, 68, COLOR_BG);
    canvas.drawRoundRect(20, 140, 280, 22, 4, COLOR_MUTED);
    int fillW = (gOtaProgress > 100 ? 100 : gOtaProgress) * 276 / 100;
    if (fillW > 0) canvas.fillRoundRect(22, 142, fillW, 18, 3, COLOR_LED_RED);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", gOtaProgress);
    canvas.setTextColor(COLOR_GOLD);
    canvas.setTextSize(2);
    drawCenteredText(canvas, pct, 160, 176);
    // Row-by-row for the same stride reason as the ticker window.
    tftPanel.pushRows(20, 136, 280, 68);
  }
  return true;
}
