# Waveshare ESP32-S3-Touch-AMOLED-1.43 (experimental community support)

Selected with `-DBOARD_AMOLED_143`. Everything for this board is behind
`#if defined(BOARD_AMOLED_143)` (or the capability flags it sets in `board.h`), so
the default 1.75, the 1.75C, the Guition and the SpotPear builds are unchanged.

**Status:** compile-tested and host-tested only. Written against Waveshare's own
example code and schematic for the board; not yet run on hardware.

The 1.43 has the same 466×466 round panel resolution as the 1.75, so the UI is
not rescaled: it simply looks about 18% smaller.

## How it differs from the 1.75 (verified)

Sources: [Waveshare repo](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.43)
(`examples/arduino/examples/07_LVGL_Test`, `04_SD_Card`, `01_ADC_Test`,
`examples/factory`), [schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43/ESP32-S3-Touch-AMOLED-1.43.pdf).

| Item | 1.75 | 1.43 |
|---|---|---|
| Display | CO5300, QSPI CS 12, SCLK 38, D0–3 4–7, RST 39 | **SH8601 or CO5300** (Waveshare ships both), CS **9**, SCLK **10**, D0–3 **11–14**, RST **21**, panel power **OLED_EN GPIO42** |
| Controller detection | — | Register 0xDA read at boot (bit-banged, as Waveshare's `read_lcd_id_bsp.c`): 0x86 = SH8601, else CO5300 |
| Column offset | 6 | CO5300: 6 · SH8601: 0 |
| QSPI clock | 80 MHz | CO5300: 80 MHz · SH8601: 40 MHz |
| Touch | CST9217, INT 11, RST 40 | **FT3168** at 0x38, **no INT, no RST**: polled |
| I2C | SDA 15, SCL 14 | SDA **47**, SCL **48** (touch, RTC, IMU) |
| PMU | AXP2101 | **None**. ETA6098 charger; its STAT pin only drives an LED |
| Battery gauge | AXP2101 | ADC on **GPIO4**, divider ÷3 |
| USB / charge sense | AXP2101 | **None** (see below) |
| RTC | PCF85063 | PCF85063 (same), INT on GPIO15 (unused) |
| IMU | QMI8658 (unused) | QMI8658, **INT1 → GPIO8**: used for shake-to-wake |
| microSD | SDMMC 1-bit | **SPI**: CS 38, MOSI 39, MISO 40, SCK 41, on **SPI3/HSPI** (the display holds SPI2) |
| Audio | ES8311 + amp | **None** (silent build, `HAS_AUDIO 0`) |
| Buttons | PWR via AXP | No PWR button. BOOT (GPIO0) is inside the case, unused |
| Flash / PSRAM | 16 MB / 8 MB OPI | Same: same partition scheme |

## Power

- **No power switch.** With a battery connected the board runs until the
  battery protection cuts it, so the firmware deep-sleeps instead (below).
- **Battery percentage** comes from the voltage (same LiPo curve and smoothing
  as the other boards, `batcurve.h`).
- **USB present** means a USB *host* is talking to the board
  (`HWCDC::isPlugged()`). A wall charger is not visible to the firmware; that
  only means the board may deep-sleep while it charges, which is fine because
  the ETA6098 charges in hardware either way, also while asleep.
- **Charging** is inferred from a clear voltage rise (`BatRiseDetector`): a
  charger lifts the cell by 100 mV+ within seconds, screen-load changes only by
  a few tens. While charging, the percentage is an estimate (it reads high).
- **Low battery:** at the usual 3.3 V cutoff the game saves and deep-sleeps
  (there is no PMU to cut power).

## Deep sleep and shake-to-wake

After **10 minutes without a touch** (`POWER_DEEPSLEEP_MS` in `power.h`, 5 min
after the screen has dimmed to nearly off), on battery and outside a minigame:

1. the game saves to NVS;
2. the panel gets DISPOFF + SLPIN and its power (GPIO42) is held off;
3. the QMI8658 is armed for wake-on-motion on INT1 (200 mg, 128 Hz low-power
   accelerometer, gyro off, first 0.25 s ignored);
4. the ESP32-S3 deep-sleeps with GPIO8 as the ext0 wake source.

A shake or pick-up wakes it. The board boots normally (a second or two); the
PCF85063 kept the time, so the pet catches up exactly as after a power-off.

- If the QMI8658 isn't found at boot, the board **never** deep-sleeps (nothing
  could wake it) and logs that once.
- Light sleep with the screen nearly off still runs, timer-only (no touch INT),
  in 100 ms steps so a quick tap isn't missed.
- The FT3168 is left in its own idle mode, not put to sleep: with no reset line
  nothing could wake it again. The microSD card also stays powered.
- To change the delay: `tools/build_143.sh --flags '-DPOWER_DEEPSLEEP_MS=300000UL'`.

Serial commands (115200, any terminal):

- `BAT?` → `BAT mv=3912 pct=68 chg=0 usb=1 imu=1`
- `SLEEPNOW` → deep sleep right away, even on USB (shake to wake)

## Building

### arduino-cli

```bash
tools/build_143.sh --out ./out-143
```

That compiles with `esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB`
and `-DBOARD_AMOLED_143=1`. Same core and libraries as CI (esp32 3.3.10,
GFX Library for Arduino, SensorLib, XPowersLib, U8g2).

### Arduino IDE

1. Board **ESP32S3 Dev Module**, Flash Size **16MB**, PSRAM **OPI PSRAM**,
   USB CDC On Boot **Enabled**, Partition Scheme **16M Flash (3MB APP/9MB FATFS)**.
2. Add `#define BOARD_AMOLED_143 1` as the first line of `board.h` (the IDE has
   no per-sketch compile flags). Remove it again to build for another board.

### Flashing

First flash (bootloader + partition table + app), from the build output:

```bash
esptool --chip esp32s3 --port <port> --baud 921600 write-flash 0x0 TamaPoke.ino.merged.bin
```

App-only update (keeps the pet and settings): write `TamaPoke.ino.bin` at `0x10000`.

### Sprites

Same as the original board: a FAT32 microSD card with the `tools/sdcard/mons/`
files in `/mons`, copied directly or sent over USB with `python3 tools/send_sd.py`.

## File-by-file changes

| File | Change |
|---|---|
| `board.h` | **New** `BOARD_AMOLED_143` block: pins, capabilities, `HAS_DEEP_SLEEP` |
| `pin_config.h` | Skips the 1.75 pin map for the 1.43 |
| `board143.h/.cpp` | **New.** Panel power, controller ID read, QMI8658 wake-on-motion, deep sleep entry/boot |
| `TouchFT3168.hpp` | **New.** Polled FT3168 (Waveshare's register use) |
| `TamaPoke.ino` | Panel picked at boot (canvas created in `setup()`), FT3168 polling, `deepSleepNow()`, timer-only light sleep, `BAT?` / `SLEEPNOW` |
| `rtcbat.cpp` | 1.43 battery: ADC ÷3, host-USB sense, voltage-rise charging, deep sleep at cutoff |
| `batcurve.h` | **New** `BatRiseDetector` (pure logic) |
| `power.h` | **New** `deepSleepDue()` + `POWER_DEEPSLEEP_MS` (pure logic) |
| `sdmon.cpp` | SPI host selectable (`SD_SPI_HOST`, HSPI on the 1.43); the LCD_CS pin trick is now Guition-only |
| `tools/build_143.sh` | **New.** Build script |
| `test/test_batrise.cpp` | **New.** Host tests for charge inference and the deep-sleep policy |

## Checks to do on hardware

- Which controller the board reports (`Panel ID 0x..` at boot) and that the
  image is centred (CO5300 offset 6).
- Touch orientation (expected 1:1 with the panel, as in Waveshare's demo).
- `SLEEPNOW`, then a shake: the board should come back within a couple of seconds.
- Battery reading against a multimeter (`BAT?`), and deep-sleep current.
- SH8601 at more than 40 MHz, if it's stable (faster frames).
