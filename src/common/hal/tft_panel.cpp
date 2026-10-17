#include "tft_panel.h"

// Single instance, driven from the Arduino loop core only.
TftPanel tftPanel;

void TftPanel::begin(int csPin, int dcPin, int mosiPin, int sclkPin,
                     int resetPin, int nativeWidth, int nativeHeight,
                     int rotation) {
  if (tft_ == nullptr) {
    tft_ = new Adafruit_ST7789(csPin, dcPin, mosiPin, sclkPin, resetPin);
  }
  tft_->init(nativeWidth, nativeHeight);
  tft_->setRotation(rotation);
  // Logical (post-rotation) frame size the canvas and push calls use.
  w_ = (rotation % 2 == 1) ? nativeHeight : nativeWidth;
  h_ = (rotation % 2 == 1) ? nativeWidth : nativeHeight;
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

Adafruit_ST7789& TftPanel::raw() {
  static Adafruit_ST7789* tft = tft_;  // begin() must have run
  return *tft;
}
