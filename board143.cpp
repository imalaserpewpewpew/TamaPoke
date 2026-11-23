#include "board143.h"

#if defined(BOARD_AMOLED_143)
#include <Wire.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_rom_sys.h>
#include <SensorQMI8658.hpp>

static SensorQMI8658 imu;
static bool imuOk = false;

bool board143Boot() {
  // deep sleep held OLED_EN low; release it before driving the pin again
  gpio_hold_dis((gpio_num_t)LCD_EN);
  gpio_deep_sleep_hold_dis();
  pinMode(LCD_EN, OUTPUT);
  digitalWrite(LCD_EN, HIGH);  // panel power on (Waveshare factory: OLED_EN high)
  bool woke = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
  if (woke) rtc_gpio_deinit((gpio_num_t)IMU_INT_PIN);  // back to a normal GPIO
  pinMode(IMU_INT_PIN, INPUT);
  delay(10);  // let the panel rail settle before the reset in board143ReadPanelId()
  return woke;
}

// ---- panel ID: single-line SPI on CS/SCLK/SDIO0, bit-banged ------------------
// Transcribed from Waveshare's read_lcd_id_bsp.c (ESP32-S3-Touch-AMOLED-1.43,
// examples/arduino/examples/07_LVGL_Test). Read command: 0x03, 0x00, reg, 0x00,
// then 8 clocks with SDIO0 as input.
static inline void clk(int v) { digitalWrite(LCD_SCLK, v); }
static void sendByte(uint8_t d) {
  for (int i = 0; i < 8; i++) {
    digitalWrite(LCD_SDIO0, (d & 0x80) ? HIGH : LOW);
    d <<= 1;
    clk(LOW);
    clk(HIGH);
  }
}
static uint8_t readByte() {
  uint8_t d = 0;
  for (int i = 0; i < 8; i++) {
    clk(LOW);
    pinMode(LCD_SDIO0, INPUT_PULLUP);
    esp_rom_delay_us(1);
    d = (d << 1) | (digitalRead(LCD_SDIO0) ? 1 : 0);
    pinMode(LCD_SDIO0, OUTPUT);
    clk(HIGH);
    esp_rom_delay_us(1);
  }
  return d;
}

uint8_t board143ReadPanelId() {
  const int outs[] = { LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3, LCD_RESET };
  for (int p : outs) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
  digitalWrite(LCD_RESET, HIGH); delay(120);
  digitalWrite(LCD_RESET, LOW);  delay(120);
  digitalWrite(LCD_RESET, HIGH); delay(120);
  digitalWrite(LCD_CS, LOW);
  sendByte(0x03);  // read
  sendByte(0x00);
  sendByte(0xDA);  // RDID1
  sendByte(0x00);
  uint8_t id = readByte();
  digitalWrite(LCD_CS, HIGH);
  // hand the pins back untouched: Arduino_ESP32QSPI::begin() re-routes them
  for (int p : outs) pinMode(p, INPUT);
  return id;
}

// ---- IMU and deep sleep ----------------------------------------------------
bool board143ImuBegin() {
  // Waveshare's own QMI8658 example talks to 0x6B; try the other address too.
  imuOk = imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL) ||
          imu.begin(Wire, QMI8658_H_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!imuOk) Serial.println("QMI8658 not found: deep sleep disabled (nothing could wake the board)");
  return imuOk;
}

bool board143CanDeepSleep() { return imuOk; }

bool board143DeepSleep() {
  if (!imuOk) return false;
  // WoM on INT1 (the only IMU pin wired, to GPIO8), initial level high: a motion
  // event toggles it low. 128 Hz low-power accelerometer, the gyro stays off.
  // Blanking: the first 32 samples (0.25 s) after arming are ignored.
  if (!imu.configWakeOnMotion(SLEEP_WOM_MG, SensorQMI8658::ACC_ODR_LOWPOWER_128Hz,
                              SensorQMI8658::INTERRUPT_PIN_1, 1, 0x20)) {
    Serial.println("QMI8658 wake-on-motion not armed: staying awake");
    return false;
  }
  delay(20);
  int idle = digitalRead(IMU_INT_PIN);  // expected HIGH; wake on the opposite level
  Serial.printf("Deep sleep: shake to wake (INT1 idle=%d)\n", idle);
  Serial.flush();
  // panel power off for the whole sleep (GPIO42 is not an RTC pin: digital hold)
  digitalWrite(LCD_EN, LOW);
  gpio_hold_en((gpio_num_t)LCD_EN);
  gpio_deep_sleep_hold_en();
  esp_sleep_enable_ext0_wakeup((gpio_num_t)IMU_INT_PIN, idle ? 0 : 1);
  if (idle) { rtc_gpio_pullup_en((gpio_num_t)IMU_INT_PIN); rtc_gpio_pulldown_dis((gpio_num_t)IMU_INT_PIN); }
  else { rtc_gpio_pulldown_en((gpio_num_t)IMU_INT_PIN); rtc_gpio_pullup_dis((gpio_num_t)IMU_INT_PIN); }
  esp_deep_sleep_start();
  return true;  // not reached
}

#endif
