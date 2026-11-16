#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

// Exposes SPITFT's protected address-window control so a frame can be
// written RAW onto the glass's full native grid with any MADCTL byte —
// for panels whose native geometry doesn't match the library's rotation
// table (the MLB board's landscape-native 320x240 ILI9341, v2.88).
class RawWindowILI9341 : public Adafruit_ILI9341 {
 public:
  using Adafruit_ILI9341::Adafruit_ILI9341;
  void writeRawFrame(uint16_t* pixels, uint16_t w, uint16_t h,
                     uint8_t madctl) {
    // MUST run inside a transaction: setAddrWindow/writePixels assume the
    // caller opened one (startWrite drops CS); without it every byte goes
    // out with CS high and the panel ignores the whole frame (v2.92).
    startWrite();
    sendCommand(ILI9341_MADCTL, &madctl, 1);
    setAddrWindow(0, 0, w - 1, h - 1);
    writePixels(pixels, (uint32_t)w * h, true, false);
    endWrite();
  }
};

// TftPanel — owns the TFT panel plus the one shared full-screen render
// canvas every screen draws into. All rendering rasterizes onto the canvas;
// exactly one push call then moves pixels to the panel, so each screen has
// a single choke point instead of scattered direct panel writes.
//
// Pins and geometry are passed to begin() by the sport/app layer (they live
// in the sport's sport_config.h) — this file stays build-agnostic.
//
// Sideways-mounted glass (outputRotationDeg != 0): when the panel module is
// physically installed rotated a quarter-turn (e.g. its header edge on the
// right), no driver rotation can present the pixel grid upright — the grid
// itself is turned. In that mode the driver runs in its portrait rotation
// and every push TRANSPOSES the landscape canvas 90 degrees through a
// PSRAM mirror, so all UI code keeps drawing ordinary 320x240 landscape.
class TftPanel {
 public:
  // Construct + init the panel over software SPI. The 5-pin form is
  // deliberate: on ESP32-S3 the hardware-SPI Adafruit constructors end up
  // bound to the variant's default VSPI pins instead of the panel pins,
  // leaving a blank screen — software SPI bit-bangs exactly the pins we
  // pass. Do NOT switch to the 3-arg constructor (see repo memory /
  // AGENTS.md). rotation 1 = landscape 320x240 with (0,0) top-left.
  // backlightPin >= 0 (active HIGH) is driven HIGH once the panel comes up
  // clean, so the blank/init never shows lit; setBacklight() controls it
  // afterwards (e.g. dark before a deep sleep). -1 = no firmware control.
  // misoPin feeds the ILI9341 soft-SPI constructor (write-only display;
  // -1 when uncommitted). outputRotationDeg: 0 drives the panel directly
  // with `rotation`; ±90 enables the sideways-glass transpose mode above
  // (sign picks the direction — flip it if the image lands 180° off).
  void begin(int csPin, int dcPin, int mosiPin, int sclkPin, int resetPin,
             int nativeWidth, int nativeHeight, int rotation,
             int backlightPin = -1, int misoPin = -1,
             int outputRotationDeg = 0);

  // Backlight control for boards whose LED line is wired (no-op when the
  // panel was begun without one).
  void setBacklight(bool on);

  // LIVE output-mode switch (the diagnostics page's orientation picker):
  // driverRotation 0-3 as the Adafruit lib numbers them, transposeDeg one
  // of 0 / 90 / -90 (the rotate-at-push mode). Repaint the screen after
  // calling — the previous frame's pixels are reinterpreted, not moved.
  void setOutputMode(int driverRotation, int transposeDeg);

  // Hard-resets the panel (reset pin + full re-init) and clears any raw
  // override — the picker applies this before every mode so register
  // experiments can never leave the controller wedged.
  void resetPanel();

  // RAW output mode (picker modes G-J): writes the transposed frame onto
  // the glass's FULL native grid (320 cols x 240 rows) with an explicit
  // MADCTL byte, bypassing the library's rotation geometry — for the
  // landscape-native panel, whose portrait rotations only address 240 of
  // the 320 columns (the "not quite wide enough" modes E/F). transposeDeg
  // is +90 or -90 (frame handedness). Repaint afterwards.
  void setRawOutputMode(uint8_t madctl, int transposeDeg);

  // Double-buffer canvas (153.6 KB at 320x240), lazily allocated ONCE on
  // first use — never from the render loop's hot path. If the heap is
  // starved at boot, falls back to a 1x1 stub so the loop paints something
  // instead of crashing on a null deref.
  GFXcanvas16& canvas();

  // Frees the canvas buffer (~150 KB). For the OTA updater only: its TLS
  // handshake cannot fit the heap alongside the canvas, so the render core
  // parks (stops rendering) and calls this; the next canvas() call lazily
  // re-allocates. Never call while any draw is in flight.
  void releaseCanvas();

  // Push the whole canvas to the panel in a single call.
  void pushFull();

  // Push a full-width horizontal band (canvas rows y..y+h-1) in one call —
  // valid because a full-width band is contiguous in the canvas buffer.
  void pushBand(int y, int h);

  // Push an arbitrary sub-rectangle row by row. drawRGBBitmap expects a
  // packed w-wide bitmap, but a window into the wider canvas straddles row
  // strides, so each row is pushed as one contiguous w-pixel run.
  void pushRows(int x, int y, int w, int h);

  // Pushes an external 320x240 landscape canvas through the same
  // output-rotation path — for screens that draw while the shared canvas
  // is parked (the OTA update screen).
  void pushExternal(const GFXcanvas16& c);

  // Direct panel access for one-off maintenance only (the low-battery
  // SLPIN command). NEVER for drawing: direct draws bypass the output
  // rotation and land sideways on a transposed panel.
  Adafruit_ILI9341& raw();

  // Controller register readback (needs SDO/MISO wired) — identifies the
  // actual panel silicon (RDDID 0x04 / RDDST 0x09 / alt-ID 0xD3).
  uint8_t readCommand8(uint8_t reg, uint8_t index = 0) {
    return tft_->readcommand8(reg, index);
  }

  int width() const { return w_; }
  int height() const { return h_; }

 private:
  // Transposes src (a w_ x h_ landscape frame) into the panel-portrait
  // mirror; rows [rowY, rowY+rowH) when rowH > 0 push just that stripe
  // (packed via scratch_). Returns false when the transpose buffers are
  // unavailable — callers fall back to the direct path.
  bool pushRotated(const uint16_t* src, int rowY, int rowH);

  RawWindowILI9341* tft_ = nullptr;  // raw subclass; base API still applies
  uint8_t rawMadctl_ = 0;       // nonzero while the raw output mode owns the panel
  int resetPin_ = -1;
  int w_ = 0;
  int h_ = 0;
  int backlightPin_ = -1;
  int outputRotDeg_ = 0;          // 0 = direct; ±90 = transpose mode
  uint16_t* mirror_ = nullptr;    // panel-portrait frame (PSRAM), transpose mode
  uint16_t* scratch_ = nullptr;   // packed stripe buffer (PSRAM), transpose mode
  int pw_ = 0;                    // panel-portrait width  (e.g. 240)
  int ph_ = 0;                    // panel-portrait height (e.g. 320)
};

extern TftPanel tftPanel;
