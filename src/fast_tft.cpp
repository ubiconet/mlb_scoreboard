#include "fast_tft.h"

#include "config.h"
#include "soc/gpio_struct.h"

namespace {

constexpr uint32_t MASK_SCK = 1UL << TFT_SCLK_PIN;
constexpr uint32_t MASK_MOSI = 1UL << TFT_MOSI_PIN;
constexpr uint32_t MASK_CS = 1UL << TFT_CS_PIN;
constexpr uint32_t MASK_DC = 1UL << TFT_DC_PIN;

inline void busClockLow() { GPIO.out_w1tc = MASK_SCK; }
inline void busClockHigh() { GPIO.out_w1ts = MASK_SCK; }
inline void busCsActive() { GPIO.out_w1tc = MASK_CS; }
inline void busCsIdle() { GPIO.out_w1ts = MASK_CS; }
inline void busDcCommand() { GPIO.out_w1tc = MASK_DC; }
inline void busDcData() { GPIO.out_w1ts = MASK_DC; }

// SPI mode 0, MSB first: data changes while the clock is low, the panel
// latches on the rising edge.
inline void busWriteByte(uint8_t b) {
  for (int8_t i = 7; i >= 0; --i) {
    busClockLow();
    if (b & (1 << i)) {
      GPIO.out_w1ts = MASK_MOSI;
    } else {
      GPIO.out_w1tc = MASK_MOSI;
    }
    busClockHigh();
  }
}

void busWriteCommand(uint8_t cmd) {
  busDcCommand();
  busWriteByte(cmd);
  busDcData();
}

}  // namespace

void fastWriteWindow(int x, int y, int w, int h, const uint16_t* pixels) {
  busCsActive();

  // Column address set (rotated-space x, panel initialized rotation 1).
  busWriteCommand(0x2A);
  busWriteByte(0);
  busWriteByte(x);
  busWriteByte(0);
  busWriteByte(x + w - 1);
  // Row address set (rotated-space y).
  busWriteCommand(0x2B);
  busWriteByte(0);
  busWriteByte(y);
  busWriteByte(0);
  busWriteByte(y + h - 1);
  // Memory write, row-major in rotated space (MADCTL handles the mapping).
  busWriteCommand(0x2C);
  const uint16_t* p = pixels;
  for (int n = w * h; n > 0; --n, ++p) {
    busWriteByte((uint8_t)(*p >> 8));
    busWriteByte((uint8_t)(*p & 0xFF));
  }

  busClockLow();  // leave the bus in the Adafruit idle state
  busCsIdle();
}
