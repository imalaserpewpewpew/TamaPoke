#pragma once
// Waveshare ESP32-S3-Touch-AMOLED-1.43 helpers (BOARD_AMOLED_143 only): panel
// controller detection, panel power, and deep sleep with shake-to-wake through
// the QMI8658 accelerometer. See PORTING_143.md.
#include <Arduino.h>
#include "pin_config.h"

#if defined(BOARD_AMOLED_143)

// Wake-on-motion threshold in mg (QMI8658 WoM, 1 mg steps). 200 mg is SensorLib's
// default: a shake or a pick-up wakes it, typing next to it on the desk doesn't.
#ifndef SLEEP_WOM_MG
#define SLEEP_WOM_MG 200
#endif

// Call first in setup(): releases the panel-power hold left by deep sleep and
// turns the panel power (OLED_EN) on. Returns true if this boot is a wake from
// deep sleep (shake).
bool board143Boot();

// Reads the display controller ID (register 0xDA) by bit-banging the QSPI pins
// as plain GPIO, exactly as Waveshare's read_lcd_id_bsp.c does. Must run BEFORE
// gfx->begin() (it resets the panel and takes the pins). 0x86 = SH8601.
uint8_t board143ReadPanelId();

// QMI8658 on the shared I2C bus (Wire must already be started). Without it the
// board never deep-sleeps, because nothing could wake it again.
bool board143ImuBegin();
bool board143CanDeepSleep();

// Arms wake-on-motion, holds the panel power off and enters deep sleep. The
// caller saves the game and turns the panel off first. Returns false (and stays
// awake) only if the IMU could not be armed; otherwise it does not return.
bool board143DeepSleep();

#endif
