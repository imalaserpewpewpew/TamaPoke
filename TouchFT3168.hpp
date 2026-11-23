#pragma once
// FocalTech FT3168 touch (Waveshare ESP32-S3-Touch-AMOLED-1.43), polled over I2C.
//
// Same small interface the sketch uses for the GT911 (TouchDrvGT911.hpp):
// begin(Wire, addr, sda, scl), getPoint(&x, &y, n), setMaxCoordinates/setMirrorXY.
// Register use copied from Waveshare's own driver (examples/arduino/examples/
// 07_LVGL_Test/FT3168.cpp): 0x00 <- 0 puts the chip in normal mode, 0x02 holds the
// number of touches and 0x03..0x06 XH, XL, YH, YL of the first one (12 bits each).
// SensorLib's FocalTech driver is not used because it refuses chips whose ID isn't
// on its list, and the FT3168's ID is not confirmed there.
//
// INT and RESET are not wired on this board, so the chip is never put into deep
// sleep (0xA5 <- 3): without a reset line nothing could wake it again. It drops
// into its own low-power monitor mode when idle.
#include <Arduino.h>
#include <Wire.h>

class TouchFT3168 {
public:
  bool begin(TwoWire &wire, uint8_t addr, int8_t sda = -1, int8_t scl = -1) {
    (void)sda;
    (void)scl;  // Wire is already started on the shared bus in setup()
    _wire = &wire;
    _addr = addr;
    _wire->beginTransmission(_addr);
    if (_wire->endTransmission() != 0) return false;
    return writeReg(0x00, 0x00);  // device mode: normal (Waveshare Touch_Init)
  }

  void setMaxCoordinates(uint16_t x, uint16_t y) { _xMax = x; _yMax = y; }
  void setMirrorXY(bool mirrorX, bool mirrorY) { _mirrorX = mirrorX; _mirrorY = mirrorY; }
  void setSwapXY(bool swap) { _swap = swap; }

  // Returns the number of touches reported (0 or 1 here) and the first point.
  int8_t getPoint(int16_t *x, int16_t *y, uint8_t point = 1) {
    (void)point;
    if (!_wire) return 0;
    uint8_t b[5];
    if (!readRegs(0x02, b, sizeof(b))) return 0;
    uint8_t n = b[0] & 0x0F;
    if (n == 0 || n > 2) return 0;  // 0x0F etc. = no valid report
    int16_t px = (int16_t)(((b[1] & 0x0F) << 8) | b[2]);
    int16_t py = (int16_t)(((b[3] & 0x0F) << 8) | b[4]);
    if (_swap) { int16_t t = px; px = py; py = t; }
    if (px >= (int16_t)_xMax) px = _xMax - 1;  // Waveshare clamps edge reads the same way
    if (py >= (int16_t)_yMax) py = _yMax - 1;
    if (_mirrorX) px = _xMax - 1 - px;
    if (_mirrorY) py = _yMax - 1 - py;
    if (x) *x = px;
    if (y) *y = py;
    return 1;
  }

private:
  bool writeReg(uint8_t reg, uint8_t val) {
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->write(val);
    return _wire->endTransmission() == 0;
  }
  bool readRegs(uint8_t reg, uint8_t *buf, uint8_t len) {
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    if (_wire->endTransmission(false) != 0) return false;
    if (_wire->requestFrom(_addr, len) != len) return false;
    for (uint8_t i = 0; i < len; i++) buf[i] = _wire->read();
    return true;
  }

  TwoWire *_wire = nullptr;
  uint8_t _addr = 0x38;
  uint16_t _xMax = 466, _yMax = 466;
  bool _mirrorX = false, _mirrorY = false, _swap = false;
};
