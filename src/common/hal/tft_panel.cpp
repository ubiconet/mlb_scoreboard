#include <esp_heap_caps.h>
#include <esp32-hal-psram.h>

#include "config.h"
#include "tft_panel.h"

// Single instance, driven from the Arduino loop core only.
TftPanel tftPanel;

void TftPanel::begin(int csPin, int dcPin, int mosiPin, int sclkPin,
                     int resetPin, int nativeWidth, int nativeHeight,
                     int rotation, int backlightPin, int misoPin,
                     int outputRotationDeg) {
  if (tft_ == nullptr) {
    // Soft-SPI form, same reasoning as the ST7789 boards before it (see
    // tft_panel.h): the hardware-SPI constructor binds to the variant's
    // default VSPI pins instead of ours.
    tft_ = new RawWindowILI9341(csPin, dcPin, mosiPin, sclkPin, resetPin,
                                misoPin);
  }
  backlightPin_ = backlightPin;
  resetPin_ = resetPin;
  if (backlightPin_ >= 0) {
    // Claim the line but stay dark until the panel holds a clean frame.
    pinMode(backlightPin_, OUTPUT);
    digitalWrite(backlightPin_, LOW);
  }
  tft_->begin();

  // Logical (canvas/UI) frame size: rotation 1/3 = landscape.
  w_ = (rotation % 2 == 1) ? nativeHeight : nativeWidth;
  h_ = (rotation % 2 == 1) ? nativeWidth : nativeHeight;

  // Sideways-glass mode: the driver always runs its portrait rotation and
  // pushes transpose; the two mirror directions differ by 180 degrees, so
  // a wrong sign shows the image upside-down — flip the config constant.
  outputRotDeg_ = (outputRotationDeg == 90 || outputRotationDeg == -90)
                      ? outputRotationDeg
                      : 0;
  if (outputRotDeg_ != 0) {
    pw_ = nativeWidth;   // panel-portrait grid: 240 wide...
    ph_ = nativeHeight;  // ...320 tall
    // The transpose buffers need ~300 KB — only PSRAM has that alongside
    // the render canvas, and the Arduino core does NOT start PSRAM unless
    // BOARD_HAS_PSRAM is defined (this build follows the NHL board and
    // leaves it undefined), so bring it up here, explicitly.
    if (!psramFound()) {
      psramInit();
    }
    if (psramFound()) {
      mirror_ = (uint16_t*)heap_caps_malloc(
          (size_t)pw_ * ph_ * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
      scratch_ = (uint16_t*)heap_caps_malloc(
          (size_t)pw_ * ph_ * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    }
    if (mirror_ == nullptr || scratch_ == nullptr) {
      // PSRAM not up YET (observed at v3.03 boot: the first psramInit()
      // right at sport::setup fails, while a retry later in the session
      // succeeds). Keep the mode PENDING — ensureTransposeBuffers()
      // retries on every push until the buffers land.
      free(mirror_);
      free(scratch_);
      mirror_ = scratch_ = nullptr;
      Serial.println("[HW] PSRAM not ready at boot - transpose buffers "
                     "pending (lazy retry on every push)");
    }
  }
  if (outputRotDeg_ != 0 && mirror_ != nullptr) {
    Serial.printf("[HW] panel output rotation %d active (PSRAM %u KB)\n",
                  outputRotDeg_, (unsigned)(ESP.getPsramSize() / 1024));
  }

  if (outputRotDeg_ != 0 && TFT_RAW_MADCTL != 0) {
    // RAW boot mode (picker letter I): full landscape window per push,
    // MADCTL from config, frame flip per handedness — see pushRotated.
    tft_->setRotation(1);  // any rotation; raw writes set MADCTL themselves
    rawMadctl_ = TFT_RAW_MADCTL;
    Serial.printf("[HW] panel RAW boot mode: MADCTL 0x%02x\n", rawMadctl_);
  } else if (outputRotDeg_ != 0) {
    // Legacy transpose mode: the driver runs at the bench-verified
    // rotation (picker letter E), not the portrait default.
    tft_->setRotation(TFT_DRIVER_ROTATION);
  } else {
    tft_->setRotation(rotation);
  }

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

void TftPanel::setRawOutputMode(uint8_t madctl, int transposeDeg) {
  if (tft_ == nullptr) return;
  transposeDeg = (transposeDeg == 90) ? 90 : -90;
  // (v3.05: raw mode no longer needs the transpose buffers — it streams
  // order-flips straight from the canvas.)
  rawMadctl_ = 0;        // leave any previous raw mode first
  outputRotDeg_ = transposeDeg;
  rawMadctl_ = madctl;
  Serial.printf("[HW] panel RAW output mode: MADCTL 0x%02x, transpose %d\n",
                madctl, transposeDeg);
}

void TftPanel::resetPanel() {
  if (tft_ == nullptr) return;
  rawMadctl_ = 0;
  // Hard reset: the controller's reset line, then the full init sequence.
  // Heals any wedged state (bench v2.98: after register experiments the
  // panel held GRAM and ignored writes until this).
  if (resetPin_ >= 0) {
    pinMode(resetPin_, OUTPUT);
    digitalWrite(resetPin_, LOW);
    delay(20);
    digitalWrite(resetPin_, HIGH);
    delay(120);
  }
  tft_->begin();
  if (backlightPin_ >= 0) digitalWrite(backlightPin_, HIGH);
  Serial.println("[HW] panel hard reset");
}

void TftPanel::setOutputMode(int driverRotation, int transposeDeg) {
  if (tft_ == nullptr) return;
  driverRotation = ((driverRotation % 4) + 4) % 4;
  transposeDeg = (transposeDeg == 90 || transposeDeg == -90) ? transposeDeg : 0;
  if (transposeDeg != 0 && mirror_ == nullptr) {
    // Transpose requested for the first time since a failed begin-time
    // allocation — try to bring the buffers up now.
    if (!psramFound()) psramInit();
    if (psramFound()) {
      if (mirror_ == nullptr) {
        mirror_ = (uint16_t*)heap_caps_malloc(
            (size_t)pw_ * ph_ * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
      }
      if (scratch_ == nullptr) {
        scratch_ = (uint16_t*)heap_caps_malloc(
            (size_t)pw_ * ph_ * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
      }
    }
    if (mirror_ == nullptr || scratch_ == nullptr) {
      free(mirror_);
      free(scratch_);
      mirror_ = scratch_ = nullptr;
      transposeDeg = 0;  // degrade to direct rather than garble
    }
  }
  outputRotDeg_ = transposeDeg;
  tft_->setRotation(driverRotation);
  Serial.printf("[HW] panel output mode: driver rotation %d, transpose %d\n",
                driverRotation, outputRotDeg_);
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

// Lazy transpose-buffer bring-up (v3.04): the boot-time psramInit can
// fail while a mid-session retry succeeds; every rotated/raw push calls
// this until the buffers land. Rate-limited to one attempt per second.
bool TftPanel::ensureTransposeBuffers() {
  if (mirror_ != nullptr && scratch_ != nullptr) return true;
  static uint32_t sLastTryAt = 0;
  uint32_t now = millis();
  if (sLastTryAt != 0 && now - sLastTryAt < 1000) return false;
  sLastTryAt = now;
  if (!psramFound()) psramInit();
  if (!psramFound()) return false;
  if (mirror_ == nullptr) {
    mirror_ = (uint16_t*)heap_caps_malloc(
        (size_t)pw_ * ph_ * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  }
  if (scratch_ == nullptr) {
    scratch_ = (uint16_t*)heap_caps_malloc(
        (size_t)pw_ * ph_ * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  }
  if (mirror_ == nullptr || scratch_ == nullptr) return false;
  Serial.printf("[HW] transpose buffers up (PSRAM %u KB)\n",
                (unsigned)(ESP.getPsramSize() / 1024));
  return true;
}

bool TftPanel::pushRotated(const uint16_t* src, int rowY, int rowH) {
  if (src == nullptr) {
    return false;
  }
  if (rawMadctl_ != 0) {
    // Raw mode (v3.05) FIRST: streams straight from the source frame with
    // only ORDER transforms — vertical flip = reversed row order (the
    // 0x88/MY winning mode I), horizontal = per-row reverse (0x48/MX).
    // No mirror buffer, no PSRAM — this board's PSRAM proves unreliable
    // at boot, and the raw path never needed a copy anyway. Bands
    // refresh full-frame.
    const bool flipRows = (rawMadctl_ & 0x40) != 0;    // MX
    const bool reverseRows = (rawMadctl_ & 0x80) != 0; // MY
    tft_->writeRawFrameFlipped(src, w_, h_, rawMadctl_, reverseRows,
                               flipRows);
    return true;
  }
  if (!ensureTransposeBuffers()) {
    return false;  // caller falls back to direct output this frame
  }
  // Legacy lib-transpose path: transpose the requested logical rows into
  // the panel-portrait mirror. The two directions are proper 90°
  // rotations that differ by 180°:
  //   -90: mirror[(w_-1-x)*pw_ + y]        = src[y*w_ + x]
  //   +90: mirror[ x     *pw_ + (h_-1-y)]  = src[y*w_ + x]
  const int y0 = (rowH > 0) ? rowY : 0;
  const int y1 = (rowH > 0) ? (rowY + rowH) : h_;
  if (outputRotDeg_ < 0) {
    for (int y = y0; y < y1; ++y) {
      const uint16_t* row = src + (size_t)y * w_;
      for (int x = 0; x < w_; ++x) {
        mirror_[(size_t)(w_ - 1 - x) * pw_ + y] = row[x];
      }
    }
  } else {
    for (int y = y0; y < y1; ++y) {
      const uint16_t* row = src + (size_t)y * w_;
      for (int x = 0; x < w_; ++x) {
        mirror_[(size_t)x * pw_ + (h_ - 1 - y)] = row[x];
      }
    }
  }
  if (rowH > 0 && rowY > 0 && (rowY + rowH) < h_) {
    // Band push: logical rows [rowY, rowY+rowH) live in mirror columns
    // [rowY, rowY+rowH); pack that stripe into scratch_ (row-major,
    // width rowH) and write just it. A band touching the frame edges
    // takes the full-frame path below — cheaper than two stripes.
    for (int py = 0; py < ph_; ++py) {
      const uint16_t* mrow = mirror_ + (size_t)py * pw_ + rowY;
      uint16_t* srow = scratch_ + (size_t)py * rowH;
      for (int k = 0; k < rowH; ++k) {
        srow[k] = mrow[k];
      }
    }
    tft_->drawRGBBitmap(rowY, 0, scratch_, rowH, ph_);
  } else {
    tft_->drawRGBBitmap(0, 0, mirror_, pw_, ph_);
  }
  return true;
}

void TftPanel::pushFull() {
  // Skip while the canvas is un-allocatable (stub active): pushing would
  // read w*h pixels out of the 1x1 stub buffer.
  if (tft_ == nullptr || sCanvasPtr == nullptr) return;
  GFXcanvas16& c = *sCanvasPtr;
  if (outputRotDeg_ != 0 && pushRotated(c.getBuffer(), 0, 0)) {
    return;
  }
  tft_->drawRGBBitmap(0, 0, c.getBuffer(), w_, h_);  // direct fallback
}

void TftPanel::pushBand(int y, int h) {
  if (tft_ == nullptr || sCanvasPtr == nullptr) return;
  GFXcanvas16& c = *sCanvasPtr;
  if (outputRotDeg_ != 0 && pushRotated(c.getBuffer(), y, h)) {
    return;
  }
  tft_->drawRGBBitmap(0, y, c.getBuffer() + (size_t)y * w_, w_, h);
}

void TftPanel::pushRows(int x, int y, int w, int h) {
  if (tft_ == nullptr || sCanvasPtr == nullptr) return;
  GFXcanvas16& c = *sCanvasPtr;
  if (outputRotDeg_ != 0 && pushRotated(c.getBuffer(), y, h)) {
    // Rotated output has no cheap sub-rectangle mapping — the band path
    // refreshed the enclosing full-width band (overdraw is harmless).
    return;
  }
  for (int row = 0; row < h; ++row) {
    tft_->drawRGBBitmap(x, y + row,
                        c.getBuffer() + (size_t)(y + row) * w_ + x, w, 1);
  }
}

void TftPanel::pushExternal(const GFXcanvas16& c) {
  if (tft_ == nullptr) return;
  if (outputRotDeg_ != 0 && pushRotated(c.getBuffer(), 0, 0)) return;
  tft_->drawRGBBitmap(0, 0, c.getBuffer(), w_, h_);
}

Adafruit_ILI9341& TftPanel::raw() {
  return *tft_;  // begin() must have run
}
