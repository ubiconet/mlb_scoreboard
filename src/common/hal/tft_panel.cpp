#include "config.h"
#include "tft_panel.h"

// Single instance, driven from the Arduino loop core only.
TftPanel tftPanel;

void TftPanel::begin(int csPin, int dcPin, int mosiPin, int sclkPin,
                     int resetPin, int nativeWidth, int nativeHeight,
                     int rotation, int backlightPin, int misoPin) {
  if (tft_ == nullptr) {
    // Soft-SPI form, same reasoning as the ST7789 boards before it (see
    // tft_panel.h): the hardware-SPI constructor binds to the variant's
    // default VSPI pins instead of ours.
    tft_ = new Adafruit_ILI9341(csPin, dcPin, mosiPin, sclkPin, resetPin,
                                misoPin);
  }
  backlightPin_ = backlightPin;
  if (backlightPin_ >= 0) {
    // Claim the line but stay dark until the panel holds a clean frame.
    pinMode(backlightPin_, OUTPUT);
    digitalWrite(backlightPin_, LOW);
  }
  (void)nativeWidth;
  (void)nativeHeight;  // ILI9341 is a fixed 240x320 panel — begin() inits it
  tft_->begin();
  tft_->setRotation(rotation);
  // IPS panels on these modules ship with inverted polarity (colors look
  // like a photo negative without this); TFT_INVERT_COLORS flips it back.
  tft_->invertDisplay(TFT_INVERT_COLORS);
  // Logical (post-rotation) frame size the canvas and push calls use.
  w_ = (rotation % 2 == 1) ? nativeHeight : nativeWidth;
  h_ = (rotation % 2 == 1) ? nativeWidth : nativeHeight;
  // begin() ends by switching the display on with RAM still undefined —
  // the classic boot "snow". Blank the panel behind a display-off so the
  // fill itself is never visible either; the panel comes up clean black
  // and the first real frame (the boot splash) lands on it. (ILI9341 has
  // no enableDisplay() — DISPOFF/DISPON are the direct commands.)
  tft_->sendCommand(ILI9341_DISPOFF);
  tft_->fillScreen(0x0000);
  tft_->sendCommand(ILI9341_DISPON);
  if (backlightPin_ >= 0) {
    digitalWrite(backlightPin_, HIGH);
  }
}

void TftPanel::setBacklight(bool on) {
  if (backlightPin_ < 0) return;
  digitalWrite(backlightPin_, on ? HIGH : LOW);
}

// File-scope so canvas() and releaseCanvas() share the one pointer.
GFXcanvas16* sCanvasPtr = nullptr;

GFXcanvas16& TftPanel::canvas() {
  // Allocation comment preserved from the original getCanvas(): if the heap
  // is starved at boot (large news sprite + JSON docs all live alongside
  // this), new may return nullptr; fall back to a tiny 1x1 canvas so the
  // loop at least paints *something* instead of crashing.
  if (sCanvasPtr == nullptr) {
    // Pre-check the largest contiguous block: GFXcanvas16's constructor
    // mallocs the pixel buffer internally and a failure there is silent —
    // the object comes back with a null buffer and the first draw faults
    // (observed as a StoreProhibited panic after the OTA park released
    // and re-allocated the canvas). Returning the stub WITHOUT caching it
    // retries the allocation on every later draw, so the display
    // self-heals once the heap recovers.
    const size_t needed = (size_t)w_ * h_ * 2;
    if (ESP.getMaxAllocHeap() < needed) {
      static GFXcanvas16 stub(1, 1);
      return stub;
    }
    sCanvasPtr = new GFXcanvas16(w_, h_);
    if (sCanvasPtr == nullptr) {
      static GFXcanvas16 stub(1, 1);
      return stub;
    }
  }
  return *sCanvasPtr;
}

void TftPanel::releaseCanvas() {
  delete sCanvasPtr;
  sCanvasPtr = nullptr;
}

void TftPanel::pushFull() {
  // Skip while the canvas is un-allocatable (stub active): pushing would
  // read w*h pixels out of the 1x1 stub buffer.
  if (tft_ == nullptr || sCanvasPtr == nullptr) return;
  GFXcanvas16& c = canvas();
  tft_->drawRGBBitmap(0, 0, c.getBuffer(), w_, h_);
}

void TftPanel::pushBand(int y, int h) {
  if (tft_ == nullptr || sCanvasPtr == nullptr) return;
  GFXcanvas16& c = canvas();
  tft_->drawRGBBitmap(0, y, c.getBuffer() + (size_t)y * w_, w_, h);
}

void TftPanel::pushRows(int x, int y, int w, int h) {
  if (tft_ == nullptr || sCanvasPtr == nullptr) return;
  GFXcanvas16& c = canvas();
  for (int row = 0; row < h; ++row) {
    tft_->drawRGBBitmap(x, y + row,
                        c.getBuffer() + (size_t)(y + row) * w_ + x, w, 1);
  }
}

Adafruit_ILI9341& TftPanel::raw() {
  return *tft_;  // begin() must have run
}
