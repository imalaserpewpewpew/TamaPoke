// TamaPoke - tamagotchi pixel art inspirado en la gen 1
// para Waveshare ESP32-S3-Touch-AMOLED-1.75 (por defecto)
// y, como soporte experimental, la ESP32-S3-Touch-AMOLED-1.75C (-DBOARD_AMOLED_175C=1)
//
// Librerias (Library Manager o repo de Waveshare):
//   - "GFX Library for Arduino" (moononournation), con soporte CO5300 QSPI
//   - "SensorLib" (Lewis He), driver tactil CST9217
//
// Original 1.75: ESP32S3 Dev Module | Flash 16MB | PSRAM OPI | CDC On Boot
//                Partition 16M Flash (3MB APP/9MB FATFS)
// 1.75C:         ESP32S3 Dev Module | Flash 32MB | PSRAM OPI | CDC On Boot
//                -DBOARD_AMOLED_175C=1 + boards/175c/partitions.csv
//                (tools/build_175c.sh). See PORTING.md and board.h.
// SpotPear 1.28 AI Ball: same as the original 1.75 + -DBOARD_SPOTPEAR_BALL128
//                (experimental; 480 canvas shrunk 2:1 to the 240 panel). See board.h.
//
// Los sprites y la tabla de especies se generan con tools/sprites.py (emit).

#include <Arduino.h>
#include <Wire.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"  // pines + capacidades de placa (pin_config.h / board.h); antes que el driver de tactil
#if defined(TOUCH_FT3168)
#include "TouchFT3168.hpp"  // Waveshare 1.43: polled FT3168
#elif TOUCH_POLLING
#include "TouchDrvGT911.hpp"
#else
#include "TouchDrvCSTXXX.hpp"
#endif
// I2C bus and pins of the touch chip: the shared Wire bus unless the board
// gives the touch its own (SpotPear 1.28 AI Ball: Wire1 on 11/7)
#ifndef TOUCH_WIRE
#define TOUCH_WIRE Wire
#endif
#ifndef TP_SDA
#define TP_SDA IIC_SDA
#define TP_SCL IIC_SCL
#endif
#include <U8g2lib.h>  // fuentes CJK (japones); ver applyLangFont()
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "species.h"
#include "dex.h"
#include "droid_ui.h"
#if DROID_PREVIEW
#include "game_sprites.h"
#endif
#include "pet.h"
#include "sdmon.h"
#include "rtcbat.h"
#include "i18n.h"
#include "audio.h"
#include "blit.h"
#include "power.h"
#include "bootbutton.h"
#include "screenlock.h"
#include "trainingcapture.h"
#include "board143.h"
#include <Preferences.h>

// Version del firmware. Subir este numero en cada release (y manifest.json para
// el instalador web). Se muestra en la pantalla de ajustes y por serie al arrancar.
#define FW_VERSION "1.22"

#if defined(BOARD_GUITION_4848S040)
// ST7701S: la secuencia de init viaja por SPI de 3 hilos y los pixels por el
// bus paralelo RGB (DMA). Cableado y porches copiados literalmente de la
// propia config de Arduino_GFX para este modulo (ESP32_4848S040_86BOX_GUITION
// en examples/PDQgraphicstest/Arduino_GFX_dev_device.h); init st7701_type9,
// la probada en el 4848S040.
Arduino_DataBus *bus = new Arduino_SWSPI(
    GFX_NOT_DEFINED /* DC */, LCD_CS, LCD_SCLK, LCD_MOSI, GFX_NOT_DEFINED /* MISO */);
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    RGB_DE, RGB_VSYNC, RGB_HSYNC, RGB_PCLK,
    RGB_R0, RGB_R1, RGB_R2, RGB_R3, RGB_R4,
    RGB_G0, RGB_G1, RGB_G2, RGB_G3, RGB_G4, RGB_G5,
    RGB_B0, RGB_B1, RGB_B2, RGB_B3, RGB_B4,
    1 /*hsync pol*/, 10 /*front*/, 8 /*pulso*/, 50 /*back*/,
    1 /*vsync pol*/, 10 /*front*/, 8 /*pulso*/, 20 /*back*/,
    0 /*pclk activo flanco pos*/, RGB_PCLK_HZ /*prefer_speed*/, false /*big endian*/);
// rotation 0 = orientacion nativa del modulo (cuadrada, tactil alineado); si
// el montaje fisico gira la pantalla, ajustar aqui y el swapXY del GT911
Arduino_RGB_Display *panel = new Arduino_RGB_Display(
    LCD_WIDTH, LCD_HEIGHT, rgbpanel, 0 /*rotation*/, true /*auto flush*/,
    bus, GFX_NOT_DEFINED /*RST*/, st7701_type9_init_operations, sizeof(st7701_type9_init_operations));
// retroiluminacion del IPS: pin 38, PWM (el CO5300 la lleva interna)
void displaySetBrightness(uint8_t v) {
  static bool attached = false;
  if (!attached) {
    ledcAttach(LCD_BL, 5000, 8);
    attached = true;
  }
  ledcWrite(LCD_BL, v);
}
#elif defined(BOARD_SPOTPEAR_BALL128)
// SpotPear 1.28 AI Ball: GC9A01A 240x240 over SPI. The UI still draws into the
// 480x480 canvas below; Arduino_Canvas_Half::flush() averages every 2x2 block
// into one panel pixel (halfscale.h) and streams the 240x240 frame.
#include "halfscale.h"
Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, GFX_NOT_DEFINED /* MISO */);
Arduino_GC9A01 *panel = new Arduino_GC9A01(bus, LCD_RESET, 0 /*rotation*/, true /*ips*/, PANEL_WIDTH, PANEL_HEIGHT);
// backlight is active LOW: duty 255 = off
void displaySetBrightness(uint8_t v) {
  static bool attached = false;
  if (!attached) {
    ledcAttach(LCD_BL, 5000, 8);
    attached = true;
  }
  ledcWrite(LCD_BL, 255 - v);
}
#define HALF_BAND 8  // panel rows per SPI burst (240 % 8 == 0); buffer stays in internal RAM
class Arduino_Canvas_Half : public Arduino_Canvas {
public:
  using Arduino_Canvas::Arduino_Canvas;
  void flush(bool force_flush = false) override {
    if (!_framebuffer) return;
    static uint16_t band[PANEL_WIDTH * HALF_BAND];
    static uint32_t frames = 0, totalUs = 0;
    uint32_t t0 = micros();
    const uint16_t *src = _framebuffer;
    panel->startWrite();  // one SPI transaction for the whole frame
    panel->writeAddrWindow(0, 0, PANEL_WIDTH, PANEL_HEIGHT);
    for (int y = 0; y < PANEL_HEIGHT; y += HALF_BAND) {
      for (int k = 0; k < HALF_BAND; k++, src += 2 * WIDTH)
        halfscaleRows565(src, src + WIDTH, band + k * PANEL_WIDTH, PANEL_WIDTH);
      bus->writePixels(band, PANEL_WIDTH * HALF_BAND);
    }
    panel->endWrite();
    // report the cost once, averaged over the first 30 frames (for the fps note)
    if (frames < 30) {
      totalUs += micros() - t0;
      if (++frames == 30)
        Serial.printf("flush 480->240: %lu us/frame avg over 30 frames (SPI %d MHz)\n",
                      (unsigned long)(totalUs / 30), DISPLAY_BEGIN_HZ / 1000000);
    }
  }
};
#define CANVAS_CLASS Arduino_Canvas_Half
#elif defined(BOARD_AMOLED_143)
// Waveshare 1.43: Waveshare ships this board with either an SH8601 or a CO5300
// (both 466x466 over QSPI). Both drivers are built here (their constructors don't
// touch the hardware); setup() reads the controller ID and uses the one fitted.
// The CO5300 needs the same 6 px column offset as on the 1.75 (Waveshare's flush
// adds 6 to x for it); the SH8601 needs none.
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panelCo5300 = new Arduino_CO5300(
  bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
Arduino_SH8601 *panelSh8601 = new Arduino_SH8601(
  bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT);
Arduino_OLED *panel = panelCo5300;  // replaced in setup() once the ID is known
int32_t panelHz = DISPLAY_HZ_CO5300;
void displaySetBrightness(uint8_t v) { panel->setBrightness(v); }
#define GFX_CREATED_IN_SETUP 1  // the canvas needs the panel picked at boot
#else
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(
  bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
void displaySetBrightness(uint8_t v) { panel->setBrightness(v); }
#endif
// Framebuffer completo en PSRAM: dibujamos todo y hacemos flush() (sin parpadeo)
#ifndef CANVAS_CLASS
#define CANVAS_CLASS Arduino_Canvas
#endif
#if GFX_CREATED_IN_SETUP
Arduino_Canvas *gfx = nullptr;  // created in setup() (see BOARD_AMOLED_143 above)
#else
Arduino_Canvas *gfx = new CANVAS_CLASS(LCD_WIDTH, LCD_HEIGHT, panel);
#endif

#if defined(TOUCH_FT3168)
TouchFT3168 touch;
#elif TOUCH_POLLING
TouchDrvGT911 touch;
#elif defined(BOARD_SPOTPEAR_BALL128)
TouchDrvCST816 touch;
#else
TouchDrvCST92xx touch;
#endif
// fuente CJK activa: de momento siempre false, la carga llegara con el
// soporte de japones/coreano/chino (ver PR #13)
bool gCjkFont = false;
Pet pet;

// sprite animado de la SD para la especie actual (si existe el archivo)
SdMon mon;          // sprite B/N (respaldo y minijuego si no hay PMD)
PmdMon pmd;         // sprite PMD multi-accion (pantalla principal)
PmdMon evoPmd;      // forma anterior, solo durante el parpadeo de evolucion
int16_t monFor = -2;
bool monShinyFor = false;

// comportamiento del bicho en pantalla
struct {
  uint8_t mode = 0;     // 0 idle, 1 paseo, 2 gesto one-shot
  uint8_t act = PMD_IDLE;
  uint32_t t0 = 0;      // inicio de la animacion en curso
  uint32_t until = 0;   // fin del estado actual
  float x = LCD_WIDTH / 2, targetX = LCD_WIDTH / 2;
} beh;
uint32_t walkLast = 0;  // instante del ultimo paso del paseo (0 = ninguno aun)
#define PET_GROUND 304  // linea de suelo de la mascota
PmdMon galleryPmd;  // sprite grande de la vista detalle de la galeria (PMD/TPK2, legal)

// galeria pokedex
bool galleryOpen = false;
bool galleryDirty = false;
int galleryPage = 0;        // 10 paginas de 16
int16_t galleryDetail = 0;  // dex en vista detalle, 0 = rejilla

bool screenOff = false;       // original 1.75: single PWR; 1.75C: triple-press lock
uint8_t userDim = 0;          // nivel de brillo de ajustes (power.h): 0 alto, 1 medio, 2 bajo
bool cardOpen = false;        // ficha del bicho (deslizar vertical)
bool kbOpen = false;          // teclado para renombrar al bicho
char nameBuf[12] = "";
uint8_t nameLen = 0;
uint8_t cardPage = 0;         // 0 perfil, 1 stats+medallas
bool clockOpen = false;       // pantalla de ajuste de hora (deslizar abajo)
int clockH = 12, clockM = 0;  // hora en edicion

// escena de bano: espuma sobre el bicho y limpieza al reventar
uint32_t bathUntil = 0;
bool bathPending = false;
struct { int16_t x, y; uint8_t r, ph; } bubbles[14];
uint32_t feedMenuUntil = 0;   // selector de comida abierto hasta este millis

// minijuego "toques": mantener la pokeball en el aire
bool gameOpen = false;
uint32_t gameOverUntil = 0;
float ballX, ballY, ballVX, ballVY, gamePetX;
uint32_t lastGameStep = 0;  // ultima llamada a stepGame(): fisica por tiempo real, no por frame
uint16_t gameScore;  // 16 bits: con uint8_t volvia a 0 al pasar de 255 (#48)
uint8_t gameMisses;
float hitX, hitY;             // ultimo golpe (anillo de impacto)
uint32_t hitTime = 0;
bool gameNewHi = false;

// saco de entrenamiento (entrena la fuerza)
bool sackOpen = false;
uint32_t sackUntil = 0, sackOverUntil = 0;
uint16_t sackHits = 0;
float sackShake = 0;
uint8_t sackGain = 0;
bool sackNewHi = false;

#if DROID_PREVIEW
// TamaDroid: game menu, blaster training (replaces the ball game) and circuit repair
uint32_t gameMenuUntil = 0;
float remoteX = 233;            // training remote position (it drifts along the top)
bool boltLive = false, boltBack = false;
uint32_t nextBoltAt = 0;
uint32_t blShakeUntil = 0, blHurtUntil = 0, blFlashUntil = 0, blRemoteHitUntil = 0;
uint8_t blCombo = 0;
uint32_t cargoTapAt = 0;
uint32_t circShortUntil = 0, circOkUntil = 0, circShakeUntil = 0;
struct Spark { float x, y, vx, vy; uint32_t until; uint16_t col; };
Spark blSparks[18];
bool circOpen = false;
uint8_t circSeq[40];
uint8_t circLen = 0, circPos = 0, circPhase = 0;  // phase 0 watch, 1 repeat, 2 result
uint32_t circT0 = 0, circOverUntil = 0, circFlashUntil = 0;
int8_t circFlash = -1;
bool circNewHi = false;
void startCircuit();
void circuitTap(int16_t x, int16_t y);
void renderCircuit();
bool misPickOpen = false;
uint8_t misSel = 0;
void renderMissionPick();
void missionPickTap(int16_t x, int16_t y);
void renderMissionReport();
void drawAway();
void drawCrate(uint8_t taps);
static void drawIdx(const IdxSprite &sp, int x, int y, int s, bool dim);
static void blPanel(int x, int y, int w, int h);
#endif

// las 9 especies con sprite propio en flash (respaldo sin SD): dex -> indice
int flashIdxForDex(int16_t dex) {
  static const int8_t IDX[10] = { -1, 3, 4, 5, 0, 1, 2, 6, 7, 8 };
  return (dex >= 1 && dex <= 9) ? IDX[dex] : -1;
}

#define CX (LCD_WIDTH / 2)  // centro de la pantalla redonda
#define CY (LCD_HEIGHT / 2)
// desplazamiento horizontal de la UI respecto al diseno original de 466 px (centro 233):
// 0 en Waveshare, 7 en 480x480. Se suma a toda x literal de cajas, botones y zonas tactiles
#define UI_DX (CX - 233)
#define PET_CY 202  // centro vertical del sprite

static const uint16_t INK_K = 0x18C4;  // spriteColor('k')

// botones de icono siguiendo el arco inferior de la pantalla redonda
// (los exteriores van mas altos para no salirse del circulo)
struct Btn {
  int16_t cx, cy;
  const char *const *icon;
};
Btn buttons[4] = {
#if DROID_PREVIEW
  { 140 + UI_DX, 390, SPR_ICON_CELL_Y },  // charge (comer)
  { 202 + UI_DX, 404, SPR_ICON_REMOTE },  // training remote (jugar)
  { 264 + UI_DX, 404, SPR_ICON_STANDBY }, // standby / power down (luz)
  { 326 + UI_DX, 390, SPR_ICON_OIL },     // oil bath (bano)
#else
  { 140 + UI_DX, 390, SPR_ICON_FOOD },   // comer
  { 202 + UI_DX, 404, SPR_ICON_PLAY },   // jugar
  { 264 + UI_DX, 404, SPR_ICON_LIGHT },  // luz
  { 326 + UI_DX, 390, SPR_ICON_CLEAN },  // bano
#endif
};
#define BTN_HALF 26  // boton de 52x52
#define BTN_HIT 36   // radio tactil (un poco mas generoso)

// grietas del huevo (pixeles 'k' sobre el sprite)
static const uint8_t CRACK1[][2] = { {15,8},{16,9},{15,10} };
static const uint8_t CRACK2[][2] = { {11,13},{12,14},{11,15},{20,12},{19,13},{20,14} };
// estrellas del modo noche
static const uint16_t STARS[][2] = { {120,140},{330,120},{370,210},{95,230},{280,90},{160,95} };

bool wasPressed = false;
// eleccion de inicial (primera partida): Bulbasaur / Charmander / Squirtle, 3 filas
static const int16_t STARTER_DEX[3] = { 1, 4, 7 };
#define STARTER_ROW_Y 110
#define STARTER_ROW_H 70
#define STARTER_ROW_GAP 8
// boton-CTA de evolucion (centrado, mitad de pantalla)
#define EVO_BTN_W 256
#define EVO_BTN_H 64
#define EVO_BTN_X (CX - EVO_BTN_W / 2)
#define EVO_BTN_Y 172
// boton-CTA de despedida (mas ancho: lleva el nombre + frase)
#define FAR_BTN_W 408
#define FAR_BTN_H 58
#define FAR_BTN_X (CX - FAR_BTN_W / 2)
#define FAR_BTN_Y 176
// el CST9217 avisa por el pin INT cuando hay datos tactiles; lo usamos para no
// leer el bus I2C mientras el chip esta dormido (esa lectura se colgaba ~1s)
volatile bool gTouchIrq = false;
#if BOARD_AMOLED_175C
static portMUX_TYPE touchIrqMux = portMUX_INITIALIZER_UNLOCKED;
void IRAM_ATTR touchIsr() {
  portENTER_CRITICAL_ISR(&touchIrqMux);
  gTouchIrq = true;
  portEXIT_CRITICAL_ISR(&touchIrqMux);
}
static bool takeTouchIrq() {
  portENTER_CRITICAL(&touchIrqMux);
  bool pending = gTouchIrq;
  gTouchIrq = false;
  portEXIT_CRITICAL(&touchIrqMux);
  return pending;
}
static TrainingCapture trainingCapture;
static bool trainingCaptureReady = false;
static bool trainingNeedsRelease = false;
#else
void IRAM_ATTR touchIsr() { gTouchIrq = true; }
#endif
uint32_t lastRender = 0;
uint32_t sdEpoch = 0;  // +1 cada vez que se recarga la SD (las miniaturas cambian)
// proteccion del AMOLED: atenuado por inactividad
uint32_t lastInteract = 0;
uint8_t dimStage = 0;        // 0 despierto, 1 atenuado (90s), 2 casi apagado (5min)
bool swallowGesture = false; // el toque que despierta no acciona nada
uint32_t holdStart = 0;     // pulsacion larga sobre el bicho
uint32_t confirmUntil = 0;  // dialogo "soltar?" activo hasta este millis
uint8_t choiceKind = 0;     // dialogo de decision: 0 ninguno, 1 evolucion, 2 despedida
uint32_t choiceUntil = 0;   // se cierra solo a este millis
int16_t tX0, tY0, tXl, tYl; // gesto en curso (inicio y ultima posicion)
uint32_t tStart = 0;
bool holdFired = false;

// BOOT controls and small overlay; all UI/pet changes stay on the loop task.
#if BOARD_AMOLED_175C
BootButton bootButton;
PwrButton pwrButton;
ScreenLock screenLock;
static bool panelSleeping = false;
static bool touchNeedsRelease = false;
#endif
static uint8_t panelBrightness = 255;  // ultimo brillo enviado al panel (255 = forzar el primero)
#if BOARD_AMOLED_175C
static char controlToast[48] = "";
static uint32_t controlToastUntil = 0;
static bool controlToastVisible = false;
#endif

bool screenIsLocked() {
#if BOARD_AMOLED_175C
  return screenLock.locked();
#else
  return false;
#endif
}

#if BOARD_AMOLED_175C
void showControlToast(const char *text, uint32_t duration = 1200) {
  if (screenIsLocked()) return;  // sync/pet/serial feedback cannot wake a pocketed device
  snprintf(controlToast, sizeof(controlToast), "%s", text);
  controlToastUntil = millis() + duration;
  controlToastVisible = true;
  galleryDirty = true;
  screenOff = false;
  wakeUI();
}

// Every renderer flushes through this so hints work on settings/gallery too.
void flushUI() {
#if BOARD_AMOLED_175C
  if (screenLock.blank(millis())) return;  // no QSPI frames after the lock hint
#endif
  bool lockHint = screenIsLocked();
  const char *hint = lockHint ? "Locked" : controlToast;
  if (lockHint || (controlToastVisible && (int32_t)(controlToastUntil - millis()) > 0)) {
    gfx->fillRoundRect(43, 272, 380, 56, 10, RGB565_BLACK);
    gfx->drawRoundRect(43, 272, 380, 56, 10, RGB565_WHITE);
    gfx->setTextColor(RGB565_WHITE);
    setSize(2);
    setCur(centerX(hint, 2), 284);
    printT(hint);
  }
  gfx->flush();
#if BOARD_AMOLED_175C
  screenLock.frameShown(millis());
#endif
}
#else
// Original board: no overlays, the flush is exactly the old one.
inline void flushUI() { gfx->flush(); }
#endif

#if BOARD_AMOLED_175C
// BOOT double / long press. Free in the board port; a build with time sync
// hooks these up to resync and Bluetooth pairing.
void bootDoublePress() { Serial.println("BOOT double: no action in this build"); }
void bootLongPress() { Serial.println("BOOT long: no action in this build"); }
#endif

void pollBootButton() {
#if BOARD_AMOLED_175C
  if (screenLock.locked()) return;
  BootButton::Action action = bootButton.update(digitalRead(BOOT_BUTTON_PIN) == LOW, millis());
  if (bootButton.active()) { screenOff = false; wakeUI(); }
  if (action == BootButton::SINGLE) {
    audioSetEnabled(!audioEnabled());  // existing NVS tamapoke/snd setting
    Serial.printf("BOOT single: sound %s (saved)\n", audioEnabled() ? "on" : "muted");
    showControlToast(audioEnabled() ? "Sound on" : "Muted");
    if (audioEnabled()) sfxPlay(SFX_TAP);
  } else if (action == BootButton::DOUBLE) {
    bootDoublePress();
  } else if (action == BootButton::LONG) {
    bootLongPress();
  }
#endif
}

// Poll every loop (not the old 250 ms PMU poll, which misses quick presses).
void pollPwrButton() {
#if BOARD_AMOLED_175C
  if (!pwrButton.update(digitalRead(PWR_SENSE_PIN) == PWR_SENSE_PRESSED, millis())) return;
  bool locked = screenLock.toggle();
  // Discard both a partly-entered BOOT gesture and a finger already on the glass.
  bootButton.begin(digitalRead(BOOT_BUTTON_PIN) == LOW, millis());
  wasPressed = false;
  trainingCapture.reset(true);  // discard hits/exit holds across a lock transition
  trainingNeedsRelease = false;
  holdStart = 0;
  holdFired = false;
  swallowGesture = true;
  gTouchIrq = false;
  controlToastVisible = false;
  if (locked) {
    detachInterrupt(digitalPinToInterrupt(TP_INT));
    screenOff = false;  // render only the 600 ms hint, then blank via powerCtx()
    galleryDirty = true;
    lastInteract = millis();
    Serial.println("PWR triple: Locked");
  } else {
    if (panelSleeping) {
      panel->displayOn();  // CO5300 DISPON + SLPOUT, library's 120 ms delays
      panelSleeping = false;
    }
    screenOff = false;
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), touchIsr, FALLING);
    touchNeedsRelease = true;
    gTouchIrq = true;  // one initial sample: swallow any finger held through unlock
    showControlToast("Unlocked");
    Serial.println("PWR triple: Unlocked");
  }
#else
  // Keep the original board's PEK short-press behaviour unchanged.
  static uint32_t lastPwr = 0;
  uint32_t now = millis();
  if (now - lastPwr > 250) {
    lastPwr = now;
    if (pwrShortPressed()) {
      screenOff = !screenOff;
      if (!screenOff) wakeUI();
    }
  }
#endif
}

void updateScreenLock(uint32_t now) {
#if BOARD_AMOLED_175C
  // Leave time for the preceding canvas QSPI DMA to finish before sending commands.
  if (screenLock.blank(now) && !panelSleeping && now - lastRender >= 100) {
    panel->setBrightness(0);
    panelBrightness = 0;
    panel->displayOff();  // CO5300 DISPOFF (0x28), SLPIN (0x10); NOT shared DCDC1
    panelSleeping = true;
    Serial.println("Screen lock: CO5300 off/asleep; touch and BOOT ignored");
  }
#endif
}

void setup() {
#if defined(BOARD_AMOLED_143)
  // panel power on (and released from the deep-sleep hold) before anything else
  bool wokeFromSleep = board143Boot();
#endif
#if defined(BOARD_SPOTPEAR_BALL128)
  // Hold the power latch first: on battery the board switches off without it.
  pinMode(POWER_LATCH_PIN, OUTPUT);
  digitalWrite(POWER_LATCH_PIN, HIGH);
  // Backlight off (active LOW) until the first frame and displaySetBrightness()
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
#endif
  Serial.setRxBufferSize(8192);  // la transferencia a SD llega en bloques de 2 KB
  Serial.begin(115200);
  // CRITICO: sin esto, Serial.print BLOQUEA el juego cuando no hay un
  // monitor serie abierto en el host (el bufer TX del USB CDC se llena
  // y nadie lo vacia) -> con timeout 0 los mensajes se descartan
  Serial.setTxTimeoutMs(0);
  Serial.printf("TamaPoke fw v%s (%s)\n", FW_VERSION, BOARD_NAME);
#if defined(BOARD_AMOLED_143)
  // 2 = woke from the IMU (ext0); 0 = normal power-on/reset
  Serial.printf("Boot: wake cause=%d reset reason=%d\n", (int)esp_sleep_get_wakeup_cause(),
                (int)esp_reset_reason());
#endif
  loadLang();  // idioma guardado (ES por defecto)
  Wire.begin(IIC_SDA, IIC_SCL);
  // CST9217 (tactil), AXP2101 (PMU) y PCF85063 (RTC) comparten este bus I2C.
  // Red de seguridad para PMU/RTC (SensorLib NO respeta este timeout en el
  // tactil; el cuelgue del tactil dormido se resuelve gateando por INT, ver
  // handleTouch).
  Wire.setTimeOut(50);
  // Ese es el timeout del bus. SensorLib lee con requestFrom() + readBytes(), y
  // readBytes() usa el de Stream (setTimeout, con "o" minuscula), 1000 ms por
  // defecto: si el chip dormido devuelve menos bytes, el bucle se congelaba 1 s
  // (#46). requestFrom() es sincrono, asi que los bytes ya estan o no llegaran.
  Wire.setTimeout(20);
  TOUCH_WIRE.setTimeout(20);  // la SpotPear lleva el tactil en Wire1

#if HAS_PMU
  // CRITICO: encender la alimentacion del panel (BLDO1=OLED VDD 3.3V) ANTES de
  // inicializar el display. Si el PMU se reseteo (drenaje total), este rail
  // queda OFF y la pantalla se ve negra aunque el resto de la placa funcione.
  pmuEnablePanel();
#endif

#if defined(BOARD_AMOLED_143)
  if (wokeFromSleep) Serial.println("Woke from deep sleep (shake)");
  {
    uint8_t id = board143ReadPanelId();  // before gfx->begin(): bit-bangs the QSPI pins
    bool sh = (id == LCD_ID_SH8601);
    panel = sh ? (Arduino_OLED *)panelSh8601 : (Arduino_OLED *)panelCo5300;
    panelHz = sh ? DISPLAY_HZ_SH8601 : DISPLAY_HZ_CO5300;
    Serial.printf("Panel ID 0x%02X: %s at %ld MHz\n", id, sh ? "SH8601" : "CO5300", (long)(panelHz / 1000000));
  }
  gfx = new CANVAS_CLASS(LCD_WIDTH, LCD_HEIGHT, panel);
  if (!gfx->begin(panelHz)) Serial.println("gfx->begin() fallo");
#else
  // QSPI a 80MHz en la Waveshare (por defecto 40): el flush del framebuffer es
  // el cuello de botella del fps (~56ms a 40MHz). El panel RGB usa el
  // prefer_speed del rgbpanel (RGB_PCLK_HZ). Si mostrara basura, bajarlo.
  if (!gfx->begin(DISPLAY_BEGIN_HZ)) Serial.println("gfx->begin() fallo");
#endif
  {  // nivel de brillo guardado (ajustes)
    Preferences p;
    p.begin("tamapoke", true);
    userDim = p.getUChar("bri", 0) % BRIGHT_LEVELS;
    p.end();
  }
  displaySetBrightness(180);
  applyLangFont();  // fuente del idioma guardado (clasica salvo CJK)

#if TOUCH_POLLING
  // GT911: el modulo no saca INT ni RESET (piernas a nivel fijo), se sondea por
  // I2C desde handleTouch. begin() solo comprueba presencia y lee la resolucion.
  bool touchOk = false;
  for (int i = 0; i < 3 && !touchOk; i++) {  // a veces falla al primer intento
    touchOk = touch.begin(TOUCH_WIRE, TOUCH_ADDR, TP_SDA, TP_SCL);
    if (!touchOk) delay(150);
  }
#if defined(TOUCH_FT3168)
  // FT3168 reports panel pixels, upright with the panel at rotation 0 (Waveshare's
  // LVGL demo maps them 1:1): no mirror or swap
  if (!touchOk) Serial.println("FT3168 not found");
#else
  if (!touchOk) Serial.println("GT911 no detectado");
#endif
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  // orientacion nativa del modulo; si el montaje gira el tactil, ajustar
  // (setMirrorXY / setSwapXY) tras probar en la placa
  touch.setMirrorXY(false, false);
#else
  touch.setPins(TP_RESET, TP_INT);
  bool touchOk = false;
  for (int i = 0; i < 3 && !touchOk; i++) {  // a veces falla al primer intento
    touchOk = touch.begin(TOUCH_WIRE, TOUCH_ADDR, TP_SDA, TP_SCL);
    if (!touchOk) delay(150);
  }
#if defined(BOARD_SPOTPEAR_BALL128)
  if (!touchOk) Serial.println("CST816 no detectado");
  // CST816 reports panel pixels (0..239); handleTouch() scales them to the 480 canvas
  touch.setMaxCoordinates(PANEL_WIDTH, PANEL_HEIGHT);
  touch.setMirrorXY(false, false);
  touch.setSwapXY(false);
#else
  if (!touchOk) Serial.println("CST9217 no detectado");
  // begin() deja el chip en modo comando (lee la identidad y no sale);
  // hace falta un reset por hardware para que vuelva a reportar toques
  touch.reset();
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setMirrorXY(true, true);  // panel montado 180 deg; same on official 1.75C widgets demo
#endif
  // INT activo-bajo: salta cuando hay datos. Gatea las lecturas I2C (ver loop)
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), touchIsr, FALLING);
#endif

  pet.begin();
  sdBegin();  // debe ir despues de gfx->begin(): en Guition el SD comparte SCK/MOSI con la pantalla
  thumbs.load();

  // reloj real: aplica el tiempo que estuvo apagado
  rtcBegin();
  batBegin();
  pwrSetup();
  uint32_t e = rtcEpoch();
  if (e == 0) {
    // Sin pila de respaldo, el PCF85063 pierde la hora al cortar la
    // alimentacion. Sembrar siempre la fecha fija haria RETROCEDER el tiempo de
    // juego respecto a lo ya guardado, y eso rompe la racha, la edad y la hora
    // de la escena. Se siembra con la ultima hora vista, si es posterior, para
    // que el tiempo no vaya nunca hacia atras. Los minutos apagado se pierden
    // igual (sin RTC no hay forma de saberlos), pero nada se descuadra.
    uint32_t seed = 1767225600UL;             // RTC virgen de verdad
    uint32_t seen = pet.savedEpoch();         // usa la NVS que pet ya tiene abierta
    if (seen > seed) seed = seen;
    rtcSetEpoch(seed);
    e = rtcEpoch();
    Serial.printf("RTC sin hora: sembrado en %u%s\n", seed,
                  seen > 1767225600UL ? " (desde la ultima hora guardada)" : "");
  }
  pet.syncClock(e);

  audioBegin();  // ES8311 + I2S + amplificador (suena un jingle de arranque)
#if defined(BOARD_AMOLED_143)
  board143ImuBegin();  // QMI8658: shake-to-wake for deep sleep
#endif

#if BOARD_AMOLED_175C
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  bootButton.begin(digitalRead(BOOT_BUTTON_PIN) == LOW, millis());
  pwrButton.begin(digitalRead(PWR_SENSE_PIN) == PWR_SENSE_PRESSED, millis());
  Serial.println("PWR GPIO3 active high: triple=lock/unlock gap=350ms tap<600ms debounce=25ms; hold 4s=hardware off");
  Serial.println("BOOT GPIO0 ready: debounce=25ms single=mute");
  trainingCaptureReady = touchOk && trainingCapture.begin(readTrainingTouch);
  if (!trainingCaptureReady) Serial.println("Strength training: touch capture unavailable");
#endif
  lastInteract = millis();
}

// carga/descarga el sprite de SD cuando cambia la especie
void ensureMon() {
  if (pet.speciesId == monFor && monShinyFor == pet.shiny && !sdDirty) return;
  // los ficheros recien recibidos pueden incluir un thumbs.bin nuevo, que se
  // cargaba solo en setup(): sin esto no se veia en la galeria hasta reiniciar
  if (sdDirty) { thumbs.unload(); thumbs.load(); sdEpoch++; }
  sdDirty = false;
  monFor = pet.speciesId;
  monShinyFor = pet.shiny;
  mon.unload();
  pmd.unload();
  beh.x = beh.targetX = LCD_WIDTH / 2;
  beh.mode = 0;
  beh.until = 0;
  if (pet.speciesId >= 1 && pet.speciesId <= DEX_COUNT) {
    pmd.load(pet.speciesId, pet.shiny);          // principal: PMD
    if (!pmd.loaded) mon.load(pet.speciesId, pet.shiny);  // respaldo: B/N
  }
}

#if HAS_DEEP_SLEEP
// Saves, turns the panel off and deep-sleeps (board143.cpp). A shake wakes the
// board, which boots normally; the RTC kept the time, so the pet catches up the
// same way it does after a power-off. Only returns if the IMU could not be armed.
void deepSleepNow(const char *why) {
  if (!board143CanDeepSleep()) return;  // no IMU: nothing could wake it again
  static uint32_t lastTry = 0;           // if arming failed, retry once a minute, not every pass
  bool manual = strcmp(why, "serial") == 0 || strcmp(why, "menu") == 0;  // asked for: always try
  if (!manual && lastTry && millis() - lastTry < 60000) return;
  lastTry = millis() ? millis() : 1;
  Serial.printf("Going to deep sleep (%s)\n", why);
  pet.flushSave();
  // let the previous frame's QSPI DMA finish before sending panel commands
  uint32_t since = millis() - lastRender;
  if (since < 100) delay(100 - since);
  displaySetBrightness(0);
  panel->displayOff();
  board143DeepSleep();  // does not return when it works
  panel->displayOn();   // arming failed: stay up; updateBrightness() restores the level
  panelBrightness = 0;
}
#endif

void loop() {
  uint32_t now = millis();
  pet.update(now);

  // el amplificador sigue al estado de sueno (la llamada sale sola si no cambia).
  // Aqui cubre todas las vias: el boton de luz, el sueno nocturno y el que llega
  // aplicado desde la progresion offline.
  audioSetSleeping(pet.sleeping);

  // avisa con un sonido cuando el bicho pasa a estar listo para evolucionar
  // (incluye el caso de cumplir al despertar). canEvolveNow es false durmiendo.
  static bool wasEvoReady = false;
  bool evoReady = pet.wantEvolveButton();
  if (evoReady && !wasEvoReady) sfxPlay(SFX_MEDAL);
  wasEvoReady = evoReady;
  // aviso sombrio cuando el bicho esta a punto de escaparse por abandono
  static bool wasRunReady = false;
  bool runReady = pet.canRunawayNow();
  if (runReady && !wasRunReady) sfxPlay(SFX_DENY);
  wasRunReady = runReady;

#if BOARD_AMOLED_175C
  pollPwrButton();  // lock input before handling touch/BOOT in this pass
#endif
  handleTouch();
  handleSerial();
#if BOARD_AMOLED_175C
  pollBootButton();
#endif
  ensureMon();
#if BOARD_AMOLED_175C
  updateScreenLock(now);
  now = millis();  // panel sleep/wake commands include controller settling delays
#else
  pollPwrButton();  // original board: same PEK short press as before
#endif

  updateBrightness(now);

  // bateria al limite seguro (sin USB): se guarda la partida y se apaga el PMU antes de que la
  // celda baje mas; el RTC conserva la hora
  if (batShouldShutdown() && !usbPresent()) {
    pet.flushSave();
    displaySetBrightness(0);
#if HAS_DEEP_SLEEP
    deepSleepNow("battery low");  // no PMU to cut power: deep sleep instead (a shake wakes it)
#endif
    batPowerOff();
  }

  // vuelca el autoguardado periodico SOLO con la pantalla atenuada/apagada o
  // durmiendo: la escritura a NVS congela ~1s ambos cores (caché de flash off),
  // y aqui no hay animacion que se corte ni dedo esperando respuesta. Con 90s
  // de inactividad la pantalla ya atenua, asi que se vuelca enseguida; el uso
  // activo persiste igual por los guardados de cada accion (comer/jugar/...).
  bool lockBlank = false;
#if BOARD_AMOLED_175C
  lockBlank = screenLock.blank(millis());
#endif
  if (pet.savePending() && (screenOff || lockBlank || dimStage >= 1 || pet.sleeping)) {
    pet.flushSave();
  }

  // anota la hora real cada 30 s (se persiste en cada save del juego)
  static uint32_t lastClock = 0;
  if (now - lastClock > 30000) {
    lastClock = now;
    uint32_t e = rtcEpoch();
    if (e) pet.lastSeenEpoch = e;
  }

  // latido de salud cada 5 min (para el soak test; se descarta si no hay monitor)
  static uint32_t lastHealth = 0;
  if (now - lastHealth > 300000) {
    lastHealth = now;
#if BOARD_AMOLED_175C
    Serial.printf("HEALTH up=%lus heap=%u min=%u bat=%d%% mv=%d chg=%d usb=%d dim=%u off=%d locked=%d\n",
                  (unsigned long)(now / 1000), ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                  batPercent(), batMillivolts(), batCharging() ? 1 : 0,
                  usbPresent() ? 1 : 0, dimStage, screenOff ? 1 : 0, screenIsLocked() ? 1 : 0);
#else
    Serial.printf("HEALTH up=%lus heap=%u min=%u bat=%d%% mv=%d chg=%d usb=%d dim=%u off=%d\n",
                  (unsigned long)(now / 1000), ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                  batPercent(), batMillivolts(), batCharging() ? 1 : 0,
                  usbPresent() ? 1 : 0, dimStage, screenOff ? 1 : 0);
#endif
  }

  // Ritmo de dibujo (power.h): 10 fps con el bicho en uso, menos cuanto mas rato sin tocar,
  // 85 ms en juego/saco (margen seguro para que el redibujado no pise el envio DMA del frame
  // anterior: a 40-65 ms solapaba y causaba flashes negros).
  // Con la pantalla apagada no se dibuja: a brillo 0 no se ve nada, pero el redibujado y el
  // flush DMA del framebuffer del lienzo seguian corriendo a 10 fps, y medido son 70 ms de
  // trabajo por cada 100 (46 de ellos solo el volcado). Cada render repinta la escena
  // entera, asi que no queda nada a medias; y al quedarse lastRender congelado, el primer
  // frame tras despertar sale en el acto.
  PowerCtx pc = powerCtx(millis());
#if HAS_DEEP_SLEEP
  if (deepSleepDue(pc)) deepSleepNow("idle");  // power.h: 10 min untouched, on battery
#endif
  audioSetIdle(!amplifierWanted(pc));  // el amplificador consume en reposo
  batSetPollMs(powerCacheMs(pc));
  uint32_t interval = frameIntervalMs(pc);
  bool drewFrame = false;
  if (interval && now - lastRender >= interval) {
    lastRender = now;
    drewFrame = render();
#if BOARD_AMOLED_175C
    // renderSack's capture worker has joined: only the loop changes hits/UI.
    if (sackOpen && !screenIsLocked()) drainTrainingTouch();
#endif
  }
  // cede la CPU en los pases que no dibujan: sin esto el loop gira a pleno rendimiento
  // entre frames; 1 ms no se nota en el tactil (se lee cada 20 ms) y el frame no se retrasa
  if (!drewFrame) idleWait(pc);
}

// estado de consumo actual (ver power.h)
PowerCtx powerCtx(uint32_t now) {
  PowerCtx c;
  c.idleMs = idleSince(now, lastInteract);
  c.screenOff = screenOff;
#if BOARD_AMOLED_175C
  c.screenOff = c.screenOff || screenLock.blank(now);
#endif
  c.sleeping = pet.sleeping;
#if DROID_PREVIEW
  c.gameOpen = gameOpen || sackOpen || circOpen;
#else
  c.gameOpen = gameOpen || sackOpen;
#endif
  c.usb = usbPresent();
  c.charging = batCharging();
  c.batPct = batPercent();
  c.userDim = userDim;
  return c;
}

// el usuario ha interactuado: vuelve el ritmo normal de dibujo (sin esperar al proximo
// frame de un ritmo lento) y el amplificador se enciende antes de que suene nada
void wakeUI() {
  if (screenIsLocked()) return;
  uint32_t now = millis();
  uint32_t iv = frameIntervalMs(powerCtx(now));
  bool slow = !iv || iv > 100;  // pantalla apagada o ritmo lento: se dibuja ya, sin esperar al frame
  lastInteract = now;
  // el frame forzado respeta los 85 ms entre redibujados (si no, solapa el DMA del anterior)
  if (slow) lastRender = (now - lastRender >= 85) ? 0 : lastRender - 15;
  audioSetIdle(false);
}

// brillo segun sueno + inactividad (proteccion del AMOLED) y bateria
void updateBrightness(uint32_t now) {
#if BOARD_AMOLED_175C
  if (panelSleeping) return;  // brightness/auto-dim must never wake a sleeping panel
#endif
  // los eventos visibles despiertan la pantalla solos
  if (!screenOff && (pet.evolving() || pet.ceremony || pet.eating() || pet.showHeart())) {
    if (dimStageFor(idleSince(now, lastInteract)) != DIM_AWAKE) wakeUI();
    else lastInteract = now;
  }
  PowerCtx c = powerCtx(now);
  dimStage = dimStageFor(c.idleMs);
  uint8_t target = brightnessFor(c);
  if (target != panelBrightness) {
    panelBrightness = target;
    displaySetBrightness(target);
  }
}

// Pausa de un pase del bucle que no ha dibujado. Con la pantalla apagada, o encendida pero casi
// apagada (5 min sin tocar), y sin USB se duerme la CPU (light sleep) hasta el proximo frame o
// 200 ms: despierta por el temporizador o por un toque (el INT del tactil baja). Con USB no, para
// no perder el puerto serie de la tarjeta SD: se mira sin cache, por si se acaba de enchufar.
void idleWait(PowerCtx c) {
#if !TOUCH_POLLING
  // no sleep during a gesture/radio operation or while QSPI DMA is in flight
  bool locked = screenIsLocked();
  // con la pantalla apagada por PWR el tactil no despierta la CPU (#43)
  bool touchWake = !locked && !screenOff;
  bool controlsBusy = false;
#if BOARD_AMOLED_175C
  // PWR must wake at its FIRST edge, then stay awake through
  // debounce/releases/the next-click window.
  controlsBusy = pwrButton.active() || (!locked && bootButton.active());
#endif
  if (!controlsBusy && lightSleepAllowed(c) && millis() - lastRender >= 100 && !audioBusy()) {
    c.usb = usbPresentNow();
    if (!lightSleepAllowed(c)) { delay(idleDelayMs(c)); return; }
    if (touchWake && digitalRead(TP_INT) == LOW) { gTouchIrq = true; delay(1); return; }
#if BOARD_AMOLED_175C
    if (digitalRead(PWR_SENSE_PIN) == PWR_SENSE_PRESSED ||
        (!locked && digitalRead(BOOT_BUTTON_PIN) == LOW)) { delay(1); return; }
#endif
    uint32_t ms = POWER_SLEEP_MS;
    uint32_t iv = frameIntervalMs(c);
    if (iv) {
      uint32_t due = lastRender + iv - millis();
      if ((int32_t)due < (int32_t)ms) ms = (int32_t)due > 1 ? due : 1;
    }
    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000);
    if (touchWake) gpio_wakeup_enable((gpio_num_t)TP_INT, GPIO_INTR_LOW_LEVEL);
#if BOARD_AMOLED_175C
    gpio_wakeup_enable((gpio_num_t)PWR_SENSE_PIN, GPIO_INTR_HIGH_LEVEL);
    if (!locked) gpio_wakeup_enable((gpio_num_t)BOOT_BUTTON_PIN, GPIO_INTR_LOW_LEVEL);
#endif
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();
#if BOARD_AMOLED_175C
    gpio_wakeup_disable((gpio_num_t)PWR_SENSE_PIN);
    if (!locked) gpio_wakeup_disable((gpio_num_t)BOOT_BUTTON_PIN);
#endif
    if (touchWake) {
      gpio_wakeup_disable((gpio_num_t)TP_INT);
      gpio_set_intr_type((gpio_num_t)TP_INT, GPIO_INTR_NEGEDGE);
      if (digitalRead(TP_INT) == LOW) gTouchIrq = true;
    }
    return;
  }
#endif
#if defined(BOARD_AMOLED_143)
  // No touch INT on this board, so light sleep is timer-only and the touch is
  // polled after each wake. Capped at 100 ms so a quick tap (~100-150 ms) on the
  // nearly-off screen isn't missed between two polls. Same conditions as the
  // other boards: screen nearly off and no USB host (keeps the serial port).
  if (lightSleepAllowed(c) && millis() - lastRender >= 100) {
    c.usb = usbPresentNow();
    if (lightSleepAllowed(c)) {
      uint32_t ms = 100;
      uint32_t iv = frameIntervalMs(c);
      if (iv) {
        uint32_t due = lastRender + iv - millis();
        if ((int32_t)due < (int32_t)ms) ms = (int32_t)due > 1 ? due : 1;
      }
      esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000);
      esp_light_sleep_start();
      return;
    }
  }
#endif
  delay(idleDelayMs(c));
}

// ---------- consola serie (provision de SD + depuracion) ----------

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
#if BOARD_AMOLED_175C
  if (line == "LOCK?") {
    Serial.printf("LOCK locked=%d panel_sleep=%d pwr=%d\n", screenLock.locked() ? 1 : 0,
                  panelSleeping ? 1 : 0,
                  digitalRead(PWR_SENSE_PIN) == PWR_SENSE_PRESSED ? 1 : 0);
    return;
  }
#endif
#if defined(BOARD_AMOLED_143)
  if (line == "BAT?") {  // battery estimate (no PMU on this board; see rtcbat.cpp)
    Serial.printf("BAT mv=%d pct=%d chg=%d usb=%d imu=%d\n", batMillivolts(), batPercent(),
                  batCharging() ? 1 : 0, usbPresent() ? 1 : 0, board143CanDeepSleep() ? 1 : 0);
    return;
  }
  if (line == "SLEEPNOW") {  // test deep sleep right away (even with USB); shake to wake
    Serial.println("DONE");
    deepSleepNow("serial");
    return;
  }
#endif
  if (sdSerialCommand(line)) return;

  if (line == "HATCH") {
    pet.eggTap(); pet.eggTap(); pet.eggTap();
    Serial.println("DONE");
  } else if (line.startsWith("SPEC ")) {
    int n = line.substring(5).toInt();
    if (n >= 1 && n <= DEX_COUNT) {
      pet.prevSpeciesId = pet.speciesId;
      pet.speciesId = n;
      Serial.printf("especie #%d %s\n", n, DEX_TBL[n].name);
    }
    Serial.println("DONE");
  } else if (line.startsWith("LVL ")) {
    pet.ageMinutes = (uint32_t)line.substring(4).toInt() * MINUTES_PER_LEVEL;
    Serial.println("DONE");
  } else if (line.startsWith("TIME ")) {
    uint32_t e = (uint32_t)line.substring(5).toInt();
    rtcSetEpoch(e);
    pet.setClock(e);
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line.startsWith("RTCSET ")) {  // solo RTC (simular apagados en pruebas)
    rtcSetEpoch((uint32_t)line.substring(7).toInt());
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "TIME") {
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "GAL") {
    galleryOpen = !galleryOpen;
    galleryDetail = 0;
    galleryDirty = true;
    if (!galleryOpen) galleryPmd.unload();
    Serial.println("DONE");
  } else if (line == "EGGS") {
    // simula 20 tiradas de huevo (no cambia el estado del juego)
    for (int i = 0; i < 20; i++) {
      int16_t d = pet.pickEggSpecies();
      Serial.printf("%d:%s(r%u) ", d, DEX_TBL[d].name, DEX_TBL[d].rarity);
    }
    Serial.println();
    Serial.println("DONE");
  } else if (line == "SHINY") {  // alterna shiny del actual (pruebas)
    pet.shiny = !pet.shiny;
    Serial.printf("shiny=%d\n", pet.shiny);
    Serial.println("DONE");
  } else if (line.startsWith("NICK ")) {
    pet.rename(line.substring(5).c_str());
    Serial.printf("nick=%s\n", pet.nick);
    Serial.println("DONE");
  } else if (line == "CAREDAY") {  // simula un dia nuevo cuidado (pruebas)
    pet.setClock(pet.lastSeenEpoch + 86400);
    pet.caress();
    Serial.printf("streak=%u bond=%u medals=0x%X\n", pet.streak, pet.bond, pet.medals);
    Serial.println("DONE");
  } else if (line == "BYE") {
    pet.startFarewell();
    Serial.println("DONE");
  } else if (line == "RUN") {
    pet.startRunaway();
    Serial.println("DONE");
  } else if (line == "BEEP") {
    sfxPlay(SFX_HATCH);  // prueba de audio
    Serial.println("DONE");
  } else if (line == "ABANDON") {
    pet.dbgRunawayReady();  // fuerza el estado "lista para escaparse" (test del boton)
    Serial.println("DONE");
  } else if (line == "WIPE") {
    pet.factoryReset();     // borra NVS y reinicia -> partida nueva (eleccion de inicial)
    Serial.println("DONE");
    delay(100);
    ESP.restart();
  } else if (line == "REG") {
    Serial.printf("pokedex %u/151:", pet.registeredCount());
    for (int i = 1; i <= 151; i++)
      if (pet.isRegistered(i)) Serial.printf(" %d", i);
    Serial.println();
    Serial.println("DONE");
#if BOARD_AMOLED_175C
  } else if (line == "TRAINTRACE ON" || line == "TRAINTRACE OFF" || line == "TRAINTRACE?") {
    if (line != "TRAINTRACE?") trainingCapture.setTracing(line == "TRAINTRACE ON");
    Serial.printf("TRAINTRACE %s poll=20ms trace_drops=%lu kind=0:error,1:contact,2:release,3:empty,4:cover\n",
                  trainingCapture.tracing() ? "ON" : "OFF", (unsigned long)trainingCapture.traceDropped());
    Serial.println("DONE");
#endif
  } else if (line == "HEALTH") {
    Serial.printf("up=%lus heap=%u min=%u sd=%d mon=%d\n",
                  (unsigned long)(millis() / 1000), ESP.getFreeHeap(),
                  ESP.getMinFreeHeap(), sdReady, pmd.loaded || mon.loaded);
    Serial.println("DONE");
  } else if (line == "STATS") {
    Serial.printf("spec=%d nv=%u com=%u fel=%u ene=%u lim=%u desc=%u sd=%d mon=%d bat=%d usb=%d rtc=%u\n",
                  pet.speciesId, pet.level(), pet.fullness, pet.joy, pet.energy,
                  pet.hygiene, pet.careMistakes, sdReady, mon.loaded,
                  batPercent(), usbPresent(), rtcEpoch());
    Serial.printf("peso=%u fue=%u def=%u vel=%u genes=%u/%u/%u tr=%u/%u/%u baya=%d\n",
                  pet.weight, pet.atkStat(), pet.defStat(), pet.speStat(),
                  pet.geneAtk, pet.geneDef, pet.geneSpe,
                  pet.trAtk, pet.trDef, pet.trSpe, pet.berryKnown);
    Serial.printf("shiny=%d streak=%u/%u bond=%u medals=0x%X(%u) nick=%s\n",
                  pet.shiny, pet.streak, pet.bestStreak, pet.bond, pet.medals,
                  pet.totalMedals, pet.nick);
    Serial.println("DONE");
  }
}

// ---------- entrada tactil ----------

bool inPetZone(int16_t x, int16_t y) {
  return x > 110 + UI_DX && x < 356 + UI_DX && y > 95 && y < 310;
}

#if BOARD_AMOLED_175C
// Retain the legacy getPoint() EMPTY -> no-contact behaviour and 20 ms
// cadence. The 5 ms/strict-release experiment merged taps on real hardware.
// Wire errors are still errors; no I2C or serial printing inside the ISR.
TrainingReport readTrainingTouch() {
  bool irq = takeTouchIrq();
  if (!irq && !trainingCapture.down() && digitalRead(TP_INT) != LOW) return {};
  TrainingReport failed;
  failed.attempted = true; failed.irq = irq;
  Wire.beginTransmission(TOUCH_ADDR);
  if (Wire.endTransmission() != 0) return failed;
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0xD0); Wire.write(0x00);
  if (Wire.endTransmission() != 0) return failed;
  uint8_t b[15] = {};
  size_t n = Wire.requestFrom((uint8_t)TOUCH_ADDR, (size_t)sizeof(b));
  size_t got = Wire.readBytes(b, n > sizeof(b) ? sizeof(b) : n);
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0xD0); Wire.write(0x00); Wire.write(0xAB);
  bool ackOk = Wire.endTransmission() == 0;
  TrainingReport report = decodeTrainingReport(b, got);
  report.irq = irq;
  if (!ackOk || got != sizeof(b)) { report.valid = false; report.kind = TrainingReport::ERROR; }
  return report;
}

// Optional bounded raw trace. Default OFF. Only the loop prints, after the
// worker has joined. Serial TX already has timeout zero; logging cannot block.
void drainTrainingTrace() {
  TrainingTrace trace;
  while (trainingCapture.nextTrace(trace)) {
    const auto &r = trace.report;
    Serial.printf("TRAINRAW ms=%lu irq=%u len=%u kind=%u press=%u xy=%d,%d action=%u raw=",
                  (unsigned long)trace.at, r.irq, r.bytes, (unsigned)r.kind,
                  r.pressed, r.x, r.y, (unsigned)trace.action);
    for (uint8_t byte : r.raw) Serial.printf("%02X", byte);
    Serial.println();
  }
}

void leaveSack() {
  bool held = trainingCapture.down();
  sackOpen = false;
  trainingCapture.reset(held);
  wasPressed = false;
  trainingNeedsRelease = held;
  touchNeedsRelease = held;  // never leak an exit hold into the pet/menus
  gTouchIrq = true;
}

void drainTrainingTouch() {
  drainTrainingTrace();
  TrainingEvent event;
  while (trainingCapture.next(event)) {
    wakeUI();
    if (event.action == TrainingTouch::EXIT && !sackOverUntil &&
        trainingBeforeDeadline(sackUntil, event.at)) { leaveSack(); return; }
    // Use capture time, not render/drain time: don't lose a last-moment tap,
    // or credit a tap that happened after the ten-second deadline.
    if (event.action == TrainingTouch::HIT && !sackOverUntil &&
        trainingBeforeDeadline(sackUntil, event.at)) {
      ++sackHits;
      sackShake = 16;
    }
  }
  wasPressed = trainingCapture.down();
}
#endif

// el toque se resuelve al LEVANTAR el dedo para distinguir tap de deslizar
void handleTouch() {
  if (screenIsLocked()) return;  // no touch I2C transactions or actions while locked
#if BOARD_AMOLED_175C
  if (trainingNeedsRelease && trainingCaptureReady) {
    // Swallow the exit contact until the legacy reader reports no point.
    trainingCapture.poll();
    wasPressed = trainingCapture.down();
    if (!trainingCapture.waitingRelease()) {
      trainingNeedsRelease = false; touchNeedsRelease = false; wasPressed = false;
    }
    return;
  }
  if (sackOpen) {
    trainingCapture.poll();  // legacy 20 ms; also serviced during QSPI rendering
    if (!trainingCapture.waitingRelease()) touchNeedsRelease = false;
    drainTrainingTouch();
    return;
  }
#endif
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll < 20) return;  // 50 Hz le sobra a un dedo
  lastPoll = millis();
  // Pantalla apagada con PWR (1.75 original): solo PWR la enciende. Un roce en
  // el bolsillo la encendia y gastaba la bateria (#43). El atenuado normal por
  // inactividad (dimStage) si se despierta tocando.
  if (screenOff) {
    gTouchIrq = false;
    wasPressed = false;
    return;
  }
  // solo tocamos el bus si el chip aviso por INT o si el dedo sigue abajo (hay
  // que detectar el levantamiento). Leer el CST9217 dormido se colgaba ~1s y
  // congelaba el loop entero; SensorLib no respeta el timeout de Wire.
#if TOUCH_POLLING
  // GT911: sin linea INT no hay aviso; se sondea cada pase (arriba ya esta
  // estrangulado a 50 Hz y sin toque la lectura es 1 byte por I2C)
  gTouchIrq = true;
#endif
#if BOARD_AMOLED_175C
  if (!takeTouchIrq() && !wasPressed) return;
#else
  if (!gTouchIrq && !wasPressed) return;
  gTouchIrq = false;
#endif
  // Un ciclo de solo direccion antes de leer. Sin esto, getPoint() se colgaba
  // exactamente 1000 ms (el timeout por defecto del driver I2C, que SensorLib no
  // acota) y congelaba el loop entero: es lo que se percibia como "el minijuego
  // se congela 2-3 segundos al tocar la bola" (issue #16), porque ahi los toques
  // son rapidos y seguidos.
  //
  // Medido en placa, jugando lo mismo (~470 lecturas, marcador ~50):
  //   sin esta linea: 5 parones de 1000 ms en 60 s
  //   con ella:       0
  // El contador de rechazos salio 0 en ambos casos, asi que NO funciona
  // saltandose lecturas cuando el chip no contesta: lo que hace es despertarlo,
  // para que la lectura siguiente no se encuentre el CST9217 dormido.
  TOUCH_WIRE.beginTransmission(TOUCH_ADDR);
  if (TOUCH_WIRE.endTransmission() != 0) return;  // no responde: se reintenta en 20 ms
  int16_t x, y;
  bool pressed = touch.getPoint(&x, &y, 1) > 0;
#if defined(BOARD_SPOTPEAR_BALL128)
  // panel pixel (0..239, edge reads clamped) -> 480 canvas the UI is laid out in
  x = constrain(x, 0, PANEL_WIDTH - 1) * 2;
  y = constrain(y, 0, PANEL_HEIGHT - 1) * 2;
#endif
#if BOARD_AMOLED_175C
  if (touchNeedsRelease) {
    wasPressed = pressed;
    if (!pressed) touchNeedsRelease = false;
    return;
  }
#endif

#if !BOARD_AMOLED_175C
  // Original board: retain its existing touch/exit policy.
  if (sackOpen) {
    if (pressed && !wasPressed) {
      wakeUI();
      if (y < 72) sackOpen = false;  // tocar arriba = abandonar
      else sackTap();
    }
    wasPressed = pressed;
    return;
  }
#endif

  if (pressed && !wasPressed) {  // empieza el gesto
    tX0 = tXl = x;
    tY0 = tYl = y;
    tStart = millis();
    holdFired = false;
    swallowGesture = (dimStage > 0) || screenOff;  // si estaba a oscuras, solo despierta
    screenOff = false;
    wakeUI();
  } else if (pressed) {  // sigue apoyado
    tXl = x;
    tYl = y;
    // pulsacion larga sin moverse sobre el bicho -> dialogo de soltar
    if (!holdFired && !swallowGesture && !galleryOpen && !cardOpen && !kbOpen && !clockOpen && millis() - tStart > 3000 &&
        abs(tXl - tX0) < 30 && abs(tYl - tY0) < 30 && inPetZone(tX0, tY0) &&
        !pet.isEgg() && !confirmUntil && !pet.ceremony) {
      confirmUntil = millis() + 10000;
      holdFired = true;
    }
  } else if (wasPressed) {  // levanta el dedo: resolver gesto
    wakeUI();
    int dx = tXl - tX0, dy = tYl - tY0;
    uint32_t dt = millis() - tStart;
    if (!holdFired && !swallowGesture) {
      if (abs(dx) > 80 && abs(dy) < 70 && dt < 800) onSwipe(dx > 0 ? 1 : -1);
      else if (abs(dy) > 80 && abs(dx) < 70 && dt < 800) onSwipeV(dy > 0 ? 1 : -1);
      else if (dt < 1500 && abs(dx) < 40 && abs(dy) < 40) onTap(tX0, tY0);
    }
  }
  wasPressed = pressed;
}

// deslizar vertical: abre/cierra la ficha del bicho
void openClock();  // prototipo

void onSwipeV(int dir) {
  if (pet.awaitingStarter()) return;  // bloqueado durante la eleccion de inicial
  if (gameOpen || galleryOpen || kbOpen || sackOpen || pet.ceremony) return;
#if DROID_PREVIEW
  if (circOpen || misPickOpen) return;
#endif
  if (clockOpen) { clockOpen = false; return; }
  if (cardOpen) {
    if (dir < 0) cardOpen = false;  // arriba cierra la ficha
    return;
  }
  if (dir > 0) {                    // deslizar abajo: ajustar hora
    if (!confirmUntil && !feedMenuUntil) openClock();
  } else if (!pet.isEgg() && !confirmUntil && !feedMenuUntil) {
    cardOpen = true;                // deslizar arriba: ficha
    cardPage = 0;
  }
}

// deslizar: dir +1 = hacia la derecha
void onSwipe(int dir) {
  if (pet.awaitingStarter()) return;  // bloqueado durante la eleccion de inicial
  if (gameOpen || kbOpen || clockOpen) return;
#if DROID_PREVIEW
  if (circOpen || misPickOpen) return;
#endif
  if (cardOpen) {  // dentro de la ficha: cambiar entre las 4 paginas
    int p = (int)cardPage + (dir > 0 ? -1 : 1);  // izquierda avanza
    cardPage = p < 0 ? 0 : (p > 3 ? 3 : p);
    return;
  }
  if (!galleryOpen) {
    if (!pet.ceremony && !confirmUntil) {
      galleryOpen = true;
      galleryPage = 0;
      galleryDetail = 0;
      galleryDirty = true;
    }
    return;
  }
  if (galleryDetail) {  // en detalle: volver a la rejilla
    galleryDetail = 0;
    galleryPmd.unload();
    galleryDirty = true;
    return;
  }
  int np = galleryPage - dir;  // deslizar a la izquierda avanza pagina
  if (np < 0) {                // retroceder desde la primera = salir
    galleryOpen = false;
    galleryPmd.unload();
    return;
  }
  if (np > 9) np = 9;
  if (np != galleryPage) {
    galleryPage = np;
    galleryDirty = true;
  }
}

void onTap(int16_t x, int16_t y) {
  // Serial.printf("TOUCH %d %d\n", x, y);  // diagnostico (silenciado: satura el log)
  if (pet.awaitingStarter()) {  // primera partida: elegir inicial
    for (int i = 0; i < 3; i++) {
      int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
      if (x >= 70 + UI_DX && x <= 396 + UI_DX && y >= ry && y <= ry + STARTER_ROW_H) {
        pet.chooseStarter(STARTER_DEX[i]);
        sfxPlay(SFX_TAP);
        break;
      }
    }
    return;
  }
  if (galleryOpen) {
    galleryTap(x, y);
    return;
  }
  if (kbOpen) {
    keyboardTap(x, y);
    return;
  }
  if (clockOpen) {
    clockTap(x, y);
    return;
  }
  if (pet.ceremony) return;  // durante la despedida no hay botones
  if (cardOpen) {
    if (cardPage == 0 && y < 84) openKeyboard();  // tocar el nombre = renombrar
    else if (cardPage == 1 && y >= 300 && y <= 340 && x >= 96 + UI_DX && x <= 370 + UI_DX) {
      cardOpen = false;            // boton ENTRENAR FUERZA
      startSack();
    } else {
      cardOpen = false;
    }
    return;
  }
  if (gameOpen) {
    gameTap(x, y);
    return;
  }
#if DROID_PREVIEW
  if (circOpen) {
    circuitTap(x, y);
    return;
  }
  if (misPickOpen) {
    missionPickTap(x, y);
    return;
  }
  if (pet.repUnseen && !pet.onMission()) {  // tap dismisses the mission report
    pet.seenReport();
    return;
  }
#endif
  if (choiceKind) {          // dialogo de decision: boton accion (arriba) / mantener (abajo)
    bool b1 = (x >= 93 + UI_DX && x <= 373 + UI_DX && y >= 206 && y <= 258);  // accion
    bool b2 = (x >= 93 + UI_DX && x <= 373 + UI_DX && y >= 268 && y <= 320);  // mantener / quedaros
    if (choiceKind == 1) {                 // evolucion
      if (b1) { int16_t old = pet.speciesId; pet.evolve(); evoPmd.load(old, pet.shiny); }
      else if (b2) pet.declineEvolve();
    } else if (choiceKind == 2) {          // despedida
      if (b1) pet.startFarewell();
      else if (b2) pet.declineFarewell();
    }
    choiceKind = 0;
    return;
  }
  if (confirmUntil) {        // dialogo "soltar?": SI / NO
    if (timeLeft(confirmUntil) && x >= 118 + UI_DX && x <= 218 + UI_DX && y >= 252 && y <= 304) {
      pet.release();
    }
    confirmUntil = 0;
    return;
  }
  if (feedMenuUntil) {       // selector de comida
    if (timeLeft(feedMenuUntil) && y >= 288 && y <= 352 && x >= 101 + UI_DX && x < 365 + UI_DX) {
#if DROID_PREVIEW
      if (x < CX + UI_DX) pet.feedCell();  // left: power cell, right: premium oil
      else pet.feedCandy();
#else
      int item = (x - 101 - UI_DX) / 66;
      if (item == 3) pet.feedCandy();
      else pet.feedBerry(item);
#endif
      sfxPlay(SFX_EAT);
    }
    feedMenuUntil = 0;
    return;
  }
#if DROID_PREVIEW
  if (gameMenuUntil) {       // game menu: blaster / cargo / circuit / canyon (soon)
    if (timeLeft(gameMenuUntil) && y >= 288 && y <= 352 && x >= 101 + UI_DX && x < 365 + UI_DX) {
      int item = (x - 101 - UI_DX) / 66;
      if (item == 0) startGame();
      else if (item == 1) startSack();
      else if (item == 2) startCircuit();
      else if (item == 3) { misPickOpen = true; misSel = pet.onMission() ? (uint8_t)pet.missionJob : 0; }
    }
    gameMenuUntil = 0;
    return;
  }
#endif
  if (pet.isEgg()) {
    pet.eggTap();
    sfxPlay(SFX_TAP);
    return;
  }
  // boton de evolucion: abre el dialogo evolucionar/mantener
  if (pet.wantEvolveButton() && x >= EVO_BTN_X && x <= EVO_BTN_X + EVO_BTN_W &&
      y >= EVO_BTN_Y && y <= EVO_BTN_Y + EVO_BTN_H) {
    choiceKind = 1; choiceUntil = millis() + 12000;
    return;
  }
  // botones de final (mismo recuadro): escapada directa; despedida abre dialogo
  if (x >= FAR_BTN_X && x <= FAR_BTN_X + FAR_BTN_W &&
      y >= FAR_BTN_Y && y <= FAR_BTN_Y + FAR_BTN_H) {
    if (pet.canRunawayNow()) { pet.startRunaway(); return; }
    if (pet.wantFarewellButton()) { choiceKind = 2; choiceUntil = millis() + 12000; return; }
  }
  for (int i = 0; i < 4; i++) {
    int dx = x - buttons[i].cx, dy = y - buttons[i].cy;
    if (dx * dx + dy * dy <= BTN_HIT * BTN_HIT) {
      Serial.printf("BTN %d\n", i);
      sfxPlay(SFX_TAP);
      if (i == 0) {
        if (!pet.sleeping) feedMenuUntil = millis() + 6000;
      } else if (i == 1) {
#if DROID_PREVIEW
        if (!pet.sleeping) gameMenuUntil = millis() + 6000;  // pick a game
#else
        startGame();
#endif
      } else if (i == 2) {
        pet.toggleLight();
      } else {
        startBath();
      }
      return;
    }
  }
  // tocar al bicho = caricia
  if (inPetZone(x, y)) {
    Serial.println("PET");
    pet.caress();
    if (!pet.sleeping) sfxPlay(SFX_HEART);
  }
}

// ---------- render ----------

bool gNight = false;  // noche real (por hora) o durmiendo: lo fija render()
uint16_t inkColor() { return gNight ? UI_INK_NIGHT : UI_INK; }

// ---------- escena de fondo: bioma del tipo + hora real del RTC ----------

#define C565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define HORIZON (LCD_HEIGHT / 2 - 1)  // linea donde el cielo se encuentra con el suelo

uint16_t lerp565(uint16_t a, uint16_t b, int i, int n) {
  if (n <= 0) return a;
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)((((ar + (br - ar) * i / n) << 11)) |
                    (((ag + (bg - ag) * i / n) << 5)) | (ab + (bb - ab) * i / n));
}

// hora del dia 0-23 (de la hora real cacheada cada 30s; 13 si no hay reloj)
int sceneHour() {
  uint32_t e = pet.lastSeenEpoch;
  return e ? (int)((e / 3600) % 24) : 13;
}

// suelo de cada bioma de dia (de noche se mezcla hacia el azul nocturno)
static const uint16_t BIOME_SOIL[6] = {
  C565(0x7e, 0xc0, 0x7f),  // 0 pradera
  C565(0xdc, 0xca, 0x94),  // 1 playa (arena)
  C565(0x4f, 0x8a, 0x55),  // 2 bosque
  C565(0x8a, 0x55, 0x44),  // 3 volcan
  C565(0xa8, 0x90, 0x6a),  // 4 montana
  C565(0xe6, 0xee, 0xf5),  // 5 nieve
};

void drawClouds(uint32_t now, uint16_t col) {
  for (int k = 0; k < 2; k++) {
    int cx = (int)((now / 50 + k * 250) % 560) - 40;
    int cy = 70 + k * 34;
    gfx->fillCircle(cx, cy, 16, col);
    gfx->fillCircle(cx + 18, cy + 3, 13, col);
    gfx->fillCircle(cx - 15, cy + 4, 12, col);
  }
}

#if DROID_PREVIEW
// half-ellipse dome sitting on baseY, lit from the left
static void tatooineDome(int cx, int baseY, int rx, int ry, uint16_t col, uint16_t shade) {
  for (int dy = 0; dy <= ry; dy++) {
    float f = 1.0f - (float)(dy * dy) / (float)(ry * ry);
    int w = (int)(rx * sqrtf(f > 0 ? f : 0));
    if (w <= 0) continue;
    gfx->fillRect(cx - w, baseY - dy, 2 * w, 1, col);
    int sw = w / 3;  // shaded right third
    gfx->fillRect(cx + w - sw, baseY - dy, sw, 1, shade);
  }
}

// moisture vaporator: tripod legs, a ringed column, cap and antenna. sizePct scales it.
static void drawVaporator(int vx, int baseY, int sizePct, bool night) {
  uint16_t body = C565(0x9c, 0x98, 0x92), hi = C565(0xd8, 0xd4, 0xcc), dark = C565(0x5e, 0x5a, 0x56);
  if (night) {
    uint16_t nt = C565(0x16, 0x1c, 0x30);
    body = lerp565(body, nt, 9, 16); hi = lerp565(hi, nt, 9, 16); dark = lerp565(dark, nt, 9, 16);
  }
  auto S = [&](int v) { return v * sizePct / 100; };
  // tripod legs
  gfx->fillTriangle(vx - S(14), baseY, vx - S(9), baseY, vx - S(2), baseY - S(16), dark);
  gfx->fillTriangle(vx + S(14), baseY, vx + S(9), baseY, vx + S(2), baseY - S(16), dark);
  // column with a highlight down the left side
  gfx->fillRect(vx - S(4), baseY - S(62), S(9) | 1, S(50), body);
  gfx->fillRect(vx - S(3), baseY - S(60), S(2) > 0 ? S(2) : 1, S(44), hi);
  // condenser rings
  gfx->fillRect(vx - S(8), baseY - S(52), S(17), S(5) > 0 ? S(5) : 1, dark);
  gfx->fillRect(vx - S(7), baseY - S(34), S(15), S(4) > 0 ? S(4) : 1, dark);
  gfx->fillRect(vx - S(6), baseY - S(16), S(13), S(4) > 0 ? S(4) : 1, body);
  // cap and antenna
  gfx->fillRect(vx - S(6), baseY - S(66), S(13), S(4) > 0 ? S(4) : 1, dark);
  gfx->fillRect(vx, baseY - S(78), 1, S(12), dark);
  gfx->fillRect(vx - S(4), baseY - S(74), S(9), 2, dark);
}
#endif

void drawScene(uint8_t biome, uint32_t now, bool night) {
  int h = sceneHour();
  uint16_t top, bot;
#if DROID_PREVIEW
  biome = DROID_BIOME_TATOOINE;  // TamaDroid: desert planet with twin suns
  if (night)            { top = C565(0x10, 0x10, 0x2a); bot = C565(0x2c, 0x22, 0x40); }
  else if (h < 8)       { top = C565(0xe0, 0x8a, 0x6a); bot = C565(0xf6, 0xc8, 0x8a); }
  else if (h < 18)      { top = C565(0xa8, 0xc8, 0xd8); bot = C565(0xf2, 0xe0, 0xb8); }
  else                  { top = C565(0xc8, 0x60, 0x40); bot = C565(0xf2, 0xa8, 0x58); }
#else
  if (night)            { top = C565(0x0c, 0x12, 0x24); bot = C565(0x1e, 0x26, 0x46); }
  else if (h < 8)       { top = C565(0xd1, 0x6a, 0x86); bot = C565(0xf3, 0xb8, 0x7c); }  // amanecer
  else if (h < 18)      { top = C565(0x8f, 0xc8, 0xea); bot = C565(0xdc, 0xee, 0xe6); }  // dia
  else                  { top = C565(0xc7, 0x5a, 0x4a); bot = C565(0xf0, 0xae, 0x64); }  // atardecer
#endif

  // cielo en bandas
  for (int y = 0; y < HORIZON; y += 8)
    gfx->fillRect(0, y, LCD_WIDTH, 8, lerp565(top, bot, y, HORIZON));

  // sol o luna
#if DROID_PREVIEW
  // TamaDroid: kept below the name/status lines (the 1.43 crops the top of the canvas)
  if (night) {
    gfx->fillCircle(352, 150, 16, C565(0xe8, 0xee, 0xf5));
    gfx->fillCircle(359, 146, 14, lerp565(top, bot, 150, HORIZON));  // creciente
    gfx->fillCircle(130, 160, 6, C565(0xd8, 0xcc, 0xc0));            // segunda luna
    for (auto &st : STARS) if (st[1] > 110) gfx->fillRect(st[0], st[1], 4, 4, UI_WHITE);
  } else {
    // twin suns: a big pale one and a smaller orange one
    gfx->fillCircle(330, 150, 18, C565(0xff, 0xf0, 0xc0));
    gfx->fillCircle(372, 168, 11, C565(0xff, 0xb8, 0x60));
  }
#else
  if (night) {
    gfx->fillCircle(360, 78, 24, C565(0xe8, 0xee, 0xf5));
    gfx->fillCircle(370, 72, 22, lerp565(top, bot, 78, HORIZON));  // creciente
    for (auto &st : STARS) gfx->fillRect(st[0], st[1], 4, 4, UI_WHITE);
  } else if (h < 18) {
    gfx->fillCircle(360, 84, 26, h < 8 ? C565(0xff, 0xd9, 0x8a) : C565(0xff, 0xe7, 0x9f));
    drawClouds(now, C565(0xff, 0xff, 0xff));
  } else {
    gfx->fillCircle(CX, HORIZON - 6, 34, C565(0xff, 0xf1, 0xc8));  // sol poniente
  }
#endif

  // mar de la playa: una franja de agua sobre la arena
  uint16_t soil = BIOME_SOIL[biome < 6 ? biome : 0];
#if DROID_PREVIEW
  if (biome == DROID_BIOME_TATOOINE) soil = C565(0xe2, 0xbc, 0x82);  // sand
#endif
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  if (biome == 1) {
    uint16_t sea = night ? C565(0x1c, 0x34, 0x52) : C565(0x4f, 0x96, 0xc4);
    gfx->fillRect(0, HORIZON - 26, LCD_WIDTH, 26, sea);
    for (int i = 0; i < 3; i++) {
      int wy = HORIZON - 22 + i * 7;
      uint16_t fc = night ? C565(0x3a, 0x58, 0x78) : C565(0xbf, 0xe6, 0xf5);
      gfx->fillRect(60 + ((now / 60 + i * 30) % 60), wy, 26, 2, fc);
      gfx->fillRect(300 - ((now / 60 + i * 20) % 60), wy, 26, 2, fc);
    }
  }

  // suelo
  gfx->fillRect(0, HORIZON, LCD_WIDTH, LCD_HEIGHT - HORIZON, soil);
  uint16_t hill = lerp565(soil, night ? C565(0x0c, 0x12, 0x24) : C565(0xff, 0xff, 0xff), 3, 16);
  gfx->fillRoundRect(-60, HORIZON - 14, 586, 60, 30, hill);

  // detalles del bioma
  uint16_t dk = lerp565(soil, C565(0x10, 0x18, 0x20), night ? 11 : 7, 16);
  if (biome == 2) {  // bosque: coniferas en silueta
    for (int tx : { 60, 150, 360, 416 }) {
      gfx->fillTriangle(tx, HORIZON - 46, tx - 16, HORIZON, tx + 16, HORIZON, dk);
      gfx->fillTriangle(tx, HORIZON - 60, tx - 12, HORIZON - 28, tx + 12, HORIZON - 28, dk);
    }
  } else if (biome == 3) {  // volcan: rocas y brasas
    gfx->fillTriangle(70, HORIZON, 40, HORIZON + 30, 100, HORIZON + 30, dk);
    gfx->fillTriangle(400, HORIZON + 4, 372, HORIZON + 30, 430, HORIZON + 30, dk);
    if (!night)
      for (int e = 0; e < 4; e++)
        gfx->fillRect(120 + e * 70, HORIZON + 8 + (e % 2) * 6, 4, 4, C565(0xff, 0x9b, 0x3a));
  } else if (biome == 4) {  // montana: cumbres al fondo
    gfx->fillTriangle(140, HORIZON - 50, 60, HORIZON, 220, HORIZON, dk);
    gfx->fillTriangle(330, HORIZON - 38, 250, HORIZON, 410, HORIZON, dk);
  } else if (biome == 5 && !night) {  // nieve: copos cayendo
    for (int f = 0; f < 10; f++) {
      int fx = (f * 53 + now / 40) % LCD_WIDTH;
      int fy = (f * 90 + now / 18) % HORIZON;
      gfx->fillRect(fx, fy, 3, 3, UI_WHITE);
    }
#if DROID_PREVIEW
  } else if (biome == DROID_BIOME_TATOOINE) {  // dunes, a homestead and moisture vaporators
    uint16_t dune = lerp565(soil, C565(0xff, 0xf4, 0xd8), night ? 2 : 6, 16);
    gfx->fillEllipse(120, HORIZON + 26, 130, 16, dune);
    gfx->fillEllipse(360, HORIZON + 40, 150, 18, dune);
    uint16_t nightTint = C565(0x16, 0x1c, 0x30);
    // homestead: a big adobe dome with a doorway and a small dome beside it
    uint16_t adobe = C565(0xec, 0xd8, 0xb0), adobeSh = C565(0xc9, 0xab, 0x80), door = C565(0x5a, 0x40, 0x30);
    if (night) { adobe = lerp565(adobe, nightTint, 9, 16); adobeSh = lerp565(adobeSh, nightTint, 9, 16); door = lerp565(door, nightTint, 8, 16); }
    uint16_t edge = lerp565(adobeSh, C565(0x40, 0x30, 0x24), 7, 16);  // outline so the domes read against the sand
    gfx->fillEllipse(382, HORIZON + 3, 52, 4, lerp565(soil, C565(0x40, 0x30, 0x24), 4, 16));  // ground shadow
    tatooineDome(372, HORIZON + 2, 36, 26, edge, edge);
    tatooineDome(372, HORIZON + 2, 34, 24, adobe, adobeSh);
    tatooineDome(418, HORIZON + 2, 18, 14, edge, edge);
    tatooineDome(418, HORIZON + 2, 16, 12, adobe, adobeSh);
    gfx->fillRoundRect(364, HORIZON - 14, 16, 16, 7, door);   // arched doorway
    gfx->fillRect(364, HORIZON - 6, 16, 8, door);
    gfx->fillRect(338, HORIZON - 1, 70, 3, adobeSh);           // footing
    // moisture vaporators: a big one and a smaller one further away
    drawVaporator(62, HORIZON + 2, 100, night);
    drawVaporator(126, HORIZON - 2, 65, night);
#endif
  } else if (biome == 0) {  // pradera: matas de hierba
    for (int gx : { 80, 175, 300, 395 })
      for (int b = -1; b <= 1; b++)
        gfx->fillRect(gx + b * 5, HORIZON + 6, 2, 8 + (b == 0 ? 4 : 0), dk);
  }
}

// primera partida: elige inicial entre Bulbasaur / Charmander / Squirtle
// fondo de los menus a pantalla completa: disco en el AMOLED redondo, rectangulo en panel cuadrado
static inline void fillBackdrop(uint16_t colour) {
#if PANEL_ROUND
  gfx->fillCircle(CX, CY, 231, colour);
#else
  gfx->fillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, colour);
#endif
}

void renderStarterSelect() {
  gfx->fillScreen(RGB565_BLACK);
  fillBackdrop(UI_BG_DAY);
  const char *t = T(S_CHOOSE_STARTER);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(centerX(t, 2), 68);
  printT(t);
  for (int i = 0; i < 3; i++) {
    int16_t d = STARTER_DEX[i];
    const DexEntry &de = DEX_TBL[d];
    int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
    gfx->fillRoundRect(70 + UI_DX, ry, 326, STARTER_ROW_H, 14, lerp565(de.accent, UI_WHITE, 6, 8));
    gfx->drawRoundRect(70 + UI_DX, ry, 326, STARTER_ROW_H, 14, de.accent);
    const uint8_t *th = thumbs.get(d);     // miniatura del inicial (si la SD esta lista)
    if (th) drawThumb(th, 76 + UI_DX, ry - 5, 3, false);
    gfx->setTextColor(UI_INK);
    setSize(3);
    setCur(178 + UI_DX, ry + 24);
    printT(dexName(d));
  }
  flushUI();
}

// ---------- texto: tamano y cursor (preparado para fuentes CJK) ----------
// La fuente clasica ancla el cursor en la ESQUINA SUPERIOR del texto, y las 74
// coordenadas Y del codigo estan escritas asi. Las fuentes U8g2 lo anclan en la
// LINEA BASE (drawChar hace curY = y - baseline*size), asi que al cambiar de
// fuente todo el texto subiria. setCur() compensa esa diferencia en un solo
// sitio, para que la Y siga significando "arriba" con cualquier fuente.
//
// setSize() existe solo porque la libreria tiene textsize_x protegido y setCur()
// necesita saber la escala activa para multiplicar el ascenso.
//
// Con la fuente clasica gFontAscent es 0, asi que setCur(x,y) es exactamente
// setCur(x,y): esto no mueve un pixel en los seis idiomas actuales.
uint8_t gTextSize = 1;
int gFontAscent = 0;  // px del borde superior a la linea base, 0 = fuente clasica

// --- ajuste de la fuente CJK (los dos valores van juntos) ---
// La fuente clasica mide 8 px de alto a escala 1. Si la CJK mide mas, hay que
// dividir la escala para que los tamanos cuadren... pero dividir ADELGAZA el
// trazo: a escala 1 la unifont pinta lineas de 1 px mientras el latino a escala
// 2 las pinta de 2, y el japones se ve tenue aunque mida igual.
//
// Por eso conviene una fuente base pequena Y EN NEGRITA (_b_), que permita usar
// la MISMA escala que el latino: asi el grosor coincide y no hay que dividir.
//
// Se probo una fuente realmente negrita (b10_b) y NO sirve a este tamano: los
// trazos se juntan y los kana pierden detalle, ilegibles incluso para un lector
// nativo. Se vuelve a la unifont, que es la mas legible, y el trazo fino se
// compensa en printT() repintando 1 px desplazado (pseudo-negrita).
// japanese3 y no japanese1: los subconjuntos 1 y 2 traen kana y kanji pero
// NINGUNA puntuacion CJK, y Arduino_GFX no dibuja nada cuando le falta el
// glifo (ni avanza el cursor), asi que los 25 signos ！ y ？ de las cadenas
// desaparecian. Es la misma familia unifont: cabecera de metricas identica y
// superconjunto estricto, comprobado glifo a glifo sobre los 226 codepoints
// que usa el firmware. Cuesta 102 KB mas de flash y no mueve un pixel.
// El coreano necesita una fuente con hangul, y CJK_FONT era un solo #define.
// En vez de dar por hecho que un subconjunto sirve para las dos escrituras, la
// fuente se elige por idioma. korean2 y no korean1: el coreano de aqui usa 296
// silabas distintas y todas estan en KS X 1001, el conjunto de 2350 que es el
// tamano medido para korean2; korean1 trae 478. La comprobacion mira la
// pertenencia a KS X 1001, no recorre la tabla de la fuente, asi que se apoya
// en que el conjunto de korean2 sea ese; el ASCII se da por presente.
// Sin comprobar en placa: si CJK_SIZE_DIV 2 le sienta a korean2 igual que a la
// japonesa. El ascenso si se mide en tiempo de ejecucion en applyLangFont().
#define CJK_FONT_JA u8g2_font_unifont_t_japanese3
#define CJK_FONT_KO u8g2_font_unifont_t_korean2
#define CJK_SIZE_DIV 2

void setSize(uint8_t n) {
  // gTextSize guarda la escala REALMENTE aplicada, no la pedida: setCur()
  // multiplica el ascenso por ella y tiene que cuadrar con lo que se pinta.
  gTextSize = gCjkFont ? (n >= CJK_SIZE_DIV ? n / CJK_SIZE_DIV : 1) : n;
  gfx->setTextSize(gTextSize);
}

void setCur(int x, int y) {
  gfx->setCursor(x, y + gFontAscent * gTextSize);
}

// Fija la fuente del idioma activo. Es el UNICO sitio que la toca: el resto del
// codigo dibuja igual para todos los idiomas gracias a textW()/setCur().
//
// El ascenso se mide en vez de codificarlo: getTextBounds() devuelve y1 como
// desplazamiento del borde superior respecto al cursor, negativo con las fuentes
// U8g2 (que anclan en la linea base). Asi setCur() puede seguir tratando la Y
// como "arriba" sea cual sea la fuente, sin constantes magicas por fuente.
void applyLangFont() {
  gCjkFont = LANG_IS_CJK(gLang);
  if (!gCjkFont) {
    gfx->setFont();            // 5x7 clasica, CP437: la Y ya es el borde superior
    gfx->setUTF8Print(false);
    gFontAscent = 0;
    return;
  }
  gfx->setFont((gLang == LANG_KO) ? CJK_FONT_KO : CJK_FONT_JA);
  gfx->setUTF8Print(true);     // las cadenas CJK son UTF-8 multibyte
  int16_t x1, y1;
  uint16_t w, h;
  uint8_t antes = gTextSize;
  gfx->setTextSize(1);
  gfx->getTextBounds("A", 0, 0, &x1, &y1, &w, &h);
  gFontAscent = -y1;           // y1 negativo: subir desde la linea base
  gfx->setTextSize(antes);
}

// ---------- medida de texto (preparado para fuentes CJK) ----------
// La fuente clasica de Arduino_GFX avanza 6 px por caracter a tamano 1, y la UI
// centraba con strlen(s)*6*n a pelo. Eso solo vale para un byte por caracter y
// ancho fijo: con una fuente U8g2 para japones/coreano/chino, ni strlen() cuenta
// caracteres (UTF-8 es multibyte) ni todos miden lo mismo. Centralizarlo aqui es
// lo que permitira anadir esos idiomas sin tocar cada punto de dibujado.
//
// Para la fuente clasica devuelve EXACTAMENTE la misma cuenta que habia antes,
// asi que este cambio no mueve un pixel en los seis idiomas actuales.
uint16_t textW(const char *s, uint8_t size) {
  if (!gCjkFont) return (uint16_t)strlen(s) * 6 * size;
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

// x del cursor para dejar el texto centrado en CX
int centerX(const char *s, uint8_t size) { return CX - textW(s, size) / 2; }

// Imprime respetando la fuente activa. Con la CJK repinta el texto desplazado
// 1 px: la unifont a escala reducida deja trazos de 1 px y se ve tenue, y una
// fuente de verdad negrita a este tamano junta los trazos y estropea los kana.
// Repintar engorda el trazo sin deformar el glifo. Probado en placa por
// usakomint, que comparo las dos opciones en japones.
// El teclado de apodos es solo alfabeto (se deja asi a proposito), de modo que
// aqui nunca hay CJK y no hace falta engordar nada.
void printT(char c) { gfx->print(c); }

void printT(const char *s) {
  if (gCjkFont) {
    int16_t x = gfx->getCursorX(), y = gfx->getCursorY();
    gfx->print(s);
    gfx->setCursor(x + 1, y);
  }
  gfx->print(s);
}

// Pantallas que solo cambian con un toque (o con un dato que cambia muy de vez en cuando):
// mientras su "clave" no cambie no se repintan ni se vuelcan por QSPI.
static bool staticScreen() {
  if (pet.awaitingStarter()) return true;
  if (galleryOpen || gameOpen || sackOpen) return false;  // la galeria gestiona su propio repintado
#if DROID_PREVIEW
  if (circOpen || misPickOpen || pet.repUnseen) return false;
#endif
  if (kbOpen || clockOpen) return true;
  return cardOpen && cardPage != 0;  // la pagina 0 tiene el retrato animado
}
static uint32_t mixKey(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t screenKey() {
  uint32_t h = 2166136261u;
  h = mixKey(h, pet.awaitingStarter() ? 1 : kbOpen ? 2 : clockOpen ? 3 : 4 + cardPage);
  h = mixKey(h, sdEpoch);
  if (kbOpen) {
    h = mixKey(h, nameLen);
    for (int i = 0; i < nameLen; i++) h = mixKey(h, (uint8_t)nameBuf[i]);
  } else if (clockOpen) {
    h = mixKey(h, clockH * 60 + clockM);
    h = mixKey(h, audioEnabled());
    h = mixKey(h, audioVolume());
    h = mixKey(h, userDim);
    h = mixKey(h, gLang);
  } else if (cardOpen) {
    for (uint32_t v : { (uint32_t)pet.speciesId, (uint32_t)pet.shiny, (uint32_t)pet.ageMinutes, (uint32_t)pet.streak,
                        (uint32_t)pet.bestStreak, (uint32_t)pet.bond, (uint32_t)pet.medals, (uint32_t)pet.weight,
                        (uint32_t)pet.trAtk, (uint32_t)pet.trDef, (uint32_t)pet.trSpe, (uint32_t)pet.geneAtk,
                        (uint32_t)pet.geneDef, (uint32_t)pet.geneSpe, (uint32_t)pet.careMistakes,
                        (uint32_t)pet.fullness, (uint32_t)pet.joy, (uint32_t)pet.energy, (uint32_t)pet.hygiene,
                        (uint32_t)pet.berryKnown, (uint32_t)pet.strHi, (uint32_t)gLang })
      h = mixKey(h, v);
    for (const char *c = pet.nick; *c; c++) h = mixKey(h, (uint8_t)*c);
  }
  return h;
}
bool screenKeyValid = false;  // (el renderer del host lo invalida entre escenas)
uint32_t lastScreenKey = 0;

// Dibuja un frame y devuelve true si lo ha volcado al panel.
bool render() {
#if BOARD_AMOLED_175C
  if (controlToastVisible || screenIsLocked()) {
    screenKeyValid = false;
    galleryDirty = true;
    // Also invalidate on the first frame after expiry to erase the overlay.
    if ((int32_t)(controlToastUntil - millis()) <= 0) controlToastVisible = false;
  }
#endif
  if (staticScreen()) {
    uint32_t k = screenKey();
    if (screenKeyValid && k == lastScreenKey) return false;
    lastScreenKey = k;
    screenKeyValid = true;
  } else {
    screenKeyValid = false;
  }
  if (pet.awaitingStarter()) {  // primera partida: elegir inicial (prioridad total)
    renderStarterSelect();
    return true;
  }
  if (galleryOpen) {
    renderGallery();
    return true;
  }
  if (gameOpen) {
    renderGame();
    return true;
  }
  if (sackOpen) {
    renderSack();
    return true;
  }
#if DROID_PREVIEW
  if (circOpen) {
    renderCircuit();
    return true;
  }
  if (misPickOpen) {
    renderMissionPick();
    return true;
  }
  if (pet.repUnseen && !pet.onMission() && !pet.isEgg()) {
    renderMissionReport();
    return true;
  }
#endif
  if (kbOpen) {
    renderKeyboard();
    return true;
  }
  if (clockOpen) {
    renderClock();
    return true;
  }
  if (cardOpen) {
    renderCard();
    return true;
  }
  int h = sceneHour();
  gNight = pet.sleeping || h < 6 || h >= 20;
  // drawScene cubre la pantalla completa (LCD_WIDTH x LCD_HEIGHT): sin fillScreen(NEGRO) previo para
  // que un flush DMA solapado nunca capture negro a medias (anti-parpadeo)
  drawScene(pet.isEgg() ? 0 : DEX_TBL[pet.speciesId].biome, millis(), gNight);

  if (pet.ceremony) {
    const DexEntry &d = DEX_TBL[pet.speciesId];
    const char *msg = (pet.ceremony == CER_FAREWELL) ? T(S_FAREWELL)
                      : (pet.ceremony == CER_RUNAWAY) ? T(S_RUNAWAY)
                                                      : T(S_GOODBYE);
    drawHeader(dexName(pet.speciesId), d.accent, msg);
    drawCeremony();
    flushUI();
    return true;
  }

  if (pet.isEgg()) {
    drawHeader(T(S_EGG_HDR), inkColor(), eggMsg());
#if DROID_PREVIEW
    drawCrate(pet.eggCracks());
#else
    int s = 5, x = CX - 16 * s, y = PET_CY - 16 * s;
    drawMap(SPR_EGG, SPRITE_H, x, y, s, false);
    if (pet.eggCracks() >= 1)
      for (auto &c : CRACK1) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (pet.eggCracks() >= 2)
      for (auto &c : CRACK2) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
#endif
    if (pet.eggRarity() >= R_RARO) {
      const char *rar = (pet.eggRarity() == R_LEGENDARIO) ? T(S_EGG_LEGEND) : T(S_EGG_RARE);
      gfx->setTextColor(pet.eggRarity() == R_LEGENDARIO ? UI_BAR_WARN : 0x4C98);
      setSize(2);
      setCur(centerX(rar, 2), 316);
      printT(rar);
    }
    char reg[24];
    snprintf(reg, sizeof(reg), T(S_POKEDEX_FMT), pet.registeredCount());
    gfx->fillRect(0, 312, LCD_WIDTH, LCD_HEIGHT - 312, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    gfx->setTextColor(inkColor());
    setSize(2);
    setCur(centerX(reg, 2), 348);
    printT(reg);
  } else {
    const DexEntry &d = DEX_TBL[pet.speciesId];
    char name[28];
    const char *base = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
    snprintf(name, sizeof(name), T(S_NAME_FMT), pet.shiny ? "*" : "", base, pet.level());
    drawHeader(name, gNight ? UI_INK_NIGHT : d.accent, statusMsg());
    drawStreakBadge();
#if DROID_PREVIEW
    if (pet.onMission()) drawAway();
    else
#endif
    {
      drawPet();
      drawBath();
      drawPoops();
    }
    // panel inferior: base limpia para barras y botones sobre el paisaje
    gfx->fillRect(0, 312, LCD_WIDTH, LCD_HEIGHT - 312, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    drawBars();
    drawButtons();
    drawCelebration();
    if (pet.wantEvolveButton()) drawEvolveButton();        // CTA rojo: evolucionar
    else if (pet.canRunawayNow()) drawRunawayButton();     // CTA sombrio: escapada (abandono)
    else if (pet.wantFarewellButton()) drawFarewellButton();  // CTA dorado: despedida
  }

  if (pet.sleeping) {
    gfx->setTextColor(UI_INK_NIGHT);
#if DROID_PREVIEW
    drawMap(SPR_ICON_STANDBY, 16, 312 + UI_DX, 112, 3, false);  // standby, not snoring
#else
    setSize(3);
    setCur(320 + UI_DX, 130);
    printT("Zz");
#endif
  }

  // selector de comida
  if (feedMenuUntil) {
    if (!timeLeft(feedMenuUntil)) {
      feedMenuUntil = 0;
    } else {
      gfx->fillRoundRect(101 + UI_DX, 288, 264, 64, 14, UI_WHITE);
      gfx->drawRoundRect(101 + UI_DX, 288, 264, 64, 14, inkColor());
#if DROID_PREVIEW
      drawMap(SPR_ICON_CELL_Y, 16, 143 + UI_DX, 296, 3, false);   // power cell
      gfx->drawLine(CX + UI_DX, 298, CX + UI_DX, 342, UI_TRACK);   // divider
      drawMap(SPR_ICON_OIL, 16, 275 + UI_DX, 296, 3, false);       // premium oil treat
#else
      drawMap(SPR_ICON_FOOD, 16, 110 + UI_DX, 296, 3, false);
      drawMap(SPR_ICON_BERRY_B, 16, 176 + UI_DX, 296, 3, false);
      drawMap(SPR_ICON_BERRY_G, 16, 242 + UI_DX, 296, 3, false);
      drawMap(SPR_ICON_CANDY, 16, 308 + UI_DX, 296, 3, false);
#endif
    }
  }

#if DROID_PREVIEW
  // game menu (same panel as the food menu)
  if (gameMenuUntil) {
    if (!timeLeft(gameMenuUntil)) {
      gameMenuUntil = 0;
    } else {
      gfx->fillRoundRect(101 + UI_DX, 288, 264, 64, 14, UI_WHITE);
      gfx->drawRoundRect(101 + UI_DX, 288, 264, 64, 14, inkColor());
      drawMap(SPR_ICON_REMOTE, 16, 110 + UI_DX, 296, 3, false);
      drawMap(SPR_ICON_CRATE, 16, 176 + UI_DX, 296, 3, false);
      drawMap(SPR_ICON_CHIP, 16, 242 + UI_DX, 296, 3, false);
      drawIdx(GS_MISSION, 308 + UI_DX, 296, 3, false);  // missions
    }
  }
#endif

  // dialogo "soltar?" (pulsacion larga sobre el bicho)
  if (confirmUntil) {
    if (!timeLeft(confirmUntil)) {
      confirmUntil = 0;
    } else {
      gfx->fillRoundRect(94 + UI_DX, 168, 278, 152, 16, UI_WHITE);
      gfx->drawRoundRect(94 + UI_DX, 168, 278, 152, 16, UI_INK);
      // 48 y no 28: el buffer se dimensiono cuando toda cadena era de un byte
      // por caracter. Con una fila UTF-8, "%s 놓아줄까요?" mas el nombre pasa
      // de 28 y snprintf corta a mitad de una secuencia de 3 bytes, que la
      // fuente ya no sabe dibujar.
      char q[48];
      snprintf(q, sizeof(q), T(S_RELEASE_FMT), dexName(pet.speciesId));
      gfx->setTextColor(UI_INK);
      setSize(2);
      setCur(centerX(q, 2), 196);
      printT(q);
      gfx->fillRoundRect(118 + UI_DX, 252, 100, 52, 12, UI_BAR_OK);
      gfx->setTextColor(UI_WHITE);
      setCur(118 + UI_DX + (100 - textW(T(S_YES), 2)) / 2, 270);
      printT(T(S_YES));
      gfx->fillRoundRect(248 + UI_DX, 252, 100, 52, 12, UI_BAR_BAD);
      setCur(248 + UI_DX + (100 - textW(T(S_NO), 2)) / 2, 270);
      printT(T(S_NO));
    }
  }

  // dialogo de decision (evolucionar/mantener, despedirse/quedaros)
  if (choiceKind) {
    if (!timeLeft(choiceUntil)) choiceKind = 0;
    else drawChoiceDialog();
  }

  flushUI();
  return true;
}

// ---------- minijuego: toques con la pokeball ----------

void startGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
#if DROID_PREVIEW
  if (pet.onMission()) return;
#endif
  gameOpen = true;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  hitTime = 0;
  gamePetX = CX;
  lastGameStep = millis();
#if DROID_PREVIEW
  blCombo = 0;
  blShakeUntil = blHurtUntil = blFlashUntil = blRemoteHitUntil = 0;
  for (auto &s : blSparks) s.until = 0;
#endif
  respawnBall();
}

#if DROID_PREVIEW
// TamaDroid blaster training: the training remote hovers over the homestead and fires
// bolts at the droid. Tap a bolt to send it back; it bursts on the remote. Three bolts
// that get through end the round. Same scoring/record/reward as the original ball game.
#define BL_GROUND 392     // droid's feet
#define BL_HIT_Y 330      // a bolt reaching this height has hit the droid
#define BL_REMOTE_Y 150

static void blSpawnSparks(float x, float y, int n, uint16_t c1, uint16_t c2) {
  uint32_t now = millis();
  for (int i = 0, made = 0; i < 18 && made < n; i++) {
    if ((int32_t)(blSparks[i].until - now) > 0) continue;
    float ang = random(628) / 100.0f, sp = 2.0f + random(40) / 10.0f;
    blSparks[i] = { x, y, cosf(ang) * sp, sinf(ang) * sp - 1.0f, (uint32_t)(now + 260 + random(200)), (made & 1) ? c1 : c2 };
    made++;
  }
}

void fireBolt() {
  ballX = remoteX;
  ballY = BL_REMOTE_Y + 22;
  float tx = gamePetX + (float)(random(41) - 20), ty = BL_HIT_Y + 20;
  float sp = 3.6f + gameScore * 0.12f;
  if (sp > 8.5f) sp = 8.5f;
  float dx = tx - ballX, dy = ty - ballY;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 1) len = 1;
  ballVX = dx / len * sp;
  ballVY = dy / len * sp;
  boltLive = true;
  boltBack = false;
}

void respawnBall() {  // schedules the next bolt
  boltLive = false;
  uint32_t wait = 800 - (gameScore * 15 > 450 ? 450 : gameScore * 15);
  nextBoltAt = millis() + wait;
}

#define GAME_EXIT_Y 40  // franja de arriba que sale del minijuego

void gameTap(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  float dx = ballX - x, dy = ballY - y;
  bool hit = boltLive && !boltBack && dx * dx + dy * dy < 66 * 66;
  if (!hit && y < GAME_EXIT_Y) {
    gameOpen = false;
    return;
  }
  if (hit) {  // deflected: send it back at the remote
    gameScore++;
    if (blCombo < 99) blCombo++;
    sfxPlay(SFX_PLAY);
    boltBack = true;
    float tx = remoteX - ballX, ty = BL_REMOTE_Y - ballY;
    float len = sqrtf(tx * tx + ty * ty);
    if (len < 1) len = 1;
    float sp = 9.0f;
    ballVX = tx / len * sp;
    ballVY = ty / len * sp;
    hitX = ballX;
    hitY = ballY;
    hitTime = millis();
    blFlashUntil = millis() + 160;  // droid's dome light blinks
    blSpawnSparks(ballX, ballY, 10, C565(0xff, 0xf0, 0xa0), C565(0x90, 0xd8, 0xff));
  }
}

void stepGame() {
  uint32_t now = millis();
  float k = lastGameStep ? (now - lastGameStep) / 85.0f : 1.0f;
  if (k > 3.0f) k = 3.0f;
  lastGameStep = now;
  remoteX = CX + 118.0f * sinf(now * 0.0011f + gameScore * 0.35f);
  if (!boltLive) {
    if ((int32_t)(now - nextBoltAt) >= 0) fireBolt();
  } else {
    ballX += ballVX * k;
    ballY += ballVY * k;
    if (boltBack) {
      float dx = ballX - remoteX, dy = ballY - BL_REMOTE_Y;
      if (dx * dx + dy * dy < 26 * 26 || ballY < 40) {  // bursts on the remote
        blSpawnSparks(ballX, ballY, 12, C565(0xff, 0xd0, 0x60), C565(0xff, 0xff, 0xff));
        blRemoteHitUntil = now + 220;
        respawnBall();
      }
    } else if (ballY >= BL_HIT_Y) {  // hit the droid
      blCombo = 0;
      blShakeUntil = now + 260;
      blHurtUntil = now + 450;
      blSpawnSparks(ballX, ballY, 8, C565(0xff, 0x60, 0x40), C565(0xff, 0xc0, 0x40));
      sfxPlay(SFX_TAP);
      if (++gameMisses >= 3) {
        boltLive = false;
        gameNewHi = (gameScore > pet.gameHi);
        pet.playResult(gameScore);
        sfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
        gameOverUntil = millis() + 4000;
      } else {
        respawnBall();
      }
    }
  }
  for (auto &s : blSparks) {
    if ((int32_t)(s.until - now) <= 0) continue;
    s.x += s.vx * k; s.y += s.vy * k; s.vy += 0.35f * k;
  }
  // the droid shuffles along under the remote
  float chase = (remoteX - gamePetX) * 0.05f;
  if (chase > 4) chase = 4;
  if (chase < -4) chase = -4;
  gamePetX += chase * k;
}

// indexed sprite (game_sprites.h) at integer scale, run-length filled
static void drawIdx(const IdxSprite &sp, int x, int y, int s, bool dim) {
  for (int r = 0; r < sp.h; r++) {
    const uint8_t *row = sp.idx + r * sp.w;
    int c = 0;
    while (c < sp.w) {
      uint8_t k = row[c];
      int c0 = c;
      while (c < sp.w && row[c] == k) c++;
      if (k == 0xFF) continue;
      uint16_t col = dim ? UI_TRACK : sp.pal[k];
      gfx->fillRect(x + c0 * s, y + r * s, (c - c0) * s, s, col);
    }
  }
}

static void blPanel(int x, int y, int w, int h) {
  gfx->fillRoundRect(x, y, w, h, 12, C565(0x1e, 0x26, 0x46));
  gfx->drawRoundRect(x, y, w, h, 12, UI_WHITE);
  gfx->drawRoundRect(x + 2, y + 2, w - 4, h - 4, 10, C565(0x4a, 0x58, 0x80));
}

void drawScene(uint8_t biome, uint32_t now, bool night);  // defined above

void renderBlaster() {
  uint32_t now = millis();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  drawScene(0, now, night);  // Tatooine homestead backdrop

  if (gameOverUntil) {
    if (!timeLeft(gameOverUntil)) { gameOpen = false; return; }
    blPanel(CX - 130, 128, 260, 176);
    gfx->setTextColor(C565(0xa8, 0xb8, 0xe0));
    setSize(2);
    setCur(centerX("BOLTS DEFLECTED", 2), 146);
    printT("BOLTS DEFLECTED");
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", gameScore);
    gfx->setTextColor(UI_WHITE);
    setSize(6);
    setCur(centerX(buf, 6), 176);
    printT(buf);
    setSize(2);
    if (gameNewHi && gameScore > 0) {
      gfx->setTextColor(UI_BAR_WARN);
      setCur(centerX(T(S_NEW_RECORD), 2), 238);
      printT(T(S_NEW_RECORD));
    } else {
      char rec[20];
      snprintf(rec, sizeof(rec), T(S_RECORD_FMT), pet.gameHi);
      gfx->setTextColor(C565(0xa8, 0xb8, 0xe0));
      setCur(centerX(rec, 2), 238);
      printT(rec);
    }
    const char *msg = gameScore >= 10 ? T(S_GREAT_JOY) : T(S_PLUS_JOY);
    gfx->setTextColor(UI_BAR_OK);
    setCur(centerX(msg, 2), 270);
    printT(msg);
    flushUI();
    return;
  }

  stepGame();
  int shx = 0, shy = 0;
  if (timeLeft(blShakeUntil)) { shx = (int)random(9) - 4; shy = (int)random(5) - 2; }

  // droid: hurt shake, otherwise shuffling under the remote
  if (pmd.loaded) {
    uint8_t act = PMD_IDLE;
    if (timeLeft(blHurtUntil) && pmd.has(PMD_HURT)) act = PMD_HURT;
    else if (remoteX > gamePetX + 6 && pmd.has(PMD_WALKR)) act = PMD_WALKR;
    else if (remoteX < gamePetX - 6 && pmd.has(PMD_WALKL)) act = PMD_WALKL;
    drawPmdAct(act, (int)gamePetX + shx, BL_GROUND + shy, now, true, false, 3);
  }

  // training remote: bobs, its firing port glows just before a shot, flashes when hit
  int ry = BL_REMOTE_Y + (int)(4 * sinf(now * 0.006f));
  bool charging = !boltLive && (int32_t)(nextBoltAt - now) < 300;
  drawIdx(charging ? GS_REMOTE_CHARGE : GS_REMOTE, (int)remoteX - GS_REMOTE.w + shx, ry - GS_REMOTE.h + shy, 2, false);
  if (timeLeft(blRemoteHitUntil))
    gfx->drawCircle((int)remoteX + shx, ry + shy, 30 + (int)(220 - (blRemoteHitUntil - now)) / 20, C565(0xff, 0xe0, 0x80));

  // bolt: dark glow, bright body, white core, drawn along its direction of travel
  if (boltLive) {
    float sp = sqrtf(ballVX * ballVX + ballVY * ballVY);
    float ux = sp > 0 ? ballVX / sp : 0, uy = sp > 0 ? ballVY / sp : 1;
    uint16_t outer = boltBack ? C565(0x20, 0x60, 0xa0) : C565(0x90, 0x10, 0x10);
    uint16_t body = boltBack ? C565(0x70, 0xc8, 0xff) : C565(0xff, 0x40, 0x30);
    uint16_t core = boltBack ? C565(0xe0, 0xf6, 0xff) : C565(0xff, 0xe0, 0xd8);
    int bx = (int)ballX + shx, by = (int)ballY + shy;
    for (int i = 0; i <= 6; i++) gfx->fillCircle(bx - (int)(ux * i * 4), by - (int)(uy * i * 4), 6, outer);
    for (int i = 0; i <= 6; i++) gfx->fillCircle(bx - (int)(ux * i * 4), by - (int)(uy * i * 4), 4, body);
    for (int i = 0; i <= 5; i++) gfx->fillCircle(bx - (int)(ux * i * 4), by - (int)(uy * i * 4), 2, core);
  }

  // impact ring where a bolt was deflected
  uint32_t ht = now - hitTime;
  if (hitTime && ht < 240) {
    int rad = 14 + (int)(ht / 8);
    gfx->drawCircle((int)hitX + shx, (int)hitY + shy, rad, C565(0xff, 0xf0, 0xb0));
    gfx->drawCircle((int)hitX + shx, (int)hitY + shy, rad + 1, C565(0xff, 0xd0, 0x70));
  }
  for (auto &s : blSparks)
    if ((int32_t)(s.until - now) > 0) gfx->fillRect((int)s.x + shx, (int)s.y + shy, 3, 3, s.col);

  // red edge flash when the droid takes a hit
  if (timeLeft(blHurtUntil) && (blHurtUntil - now) > 250)
    for (int r = 0; r < 6; r++) gfx->drawCircle(CX, CY, 230 - r, C565(0xe0, 0x30, 0x30));

  // HUD: framed score panel, droid-dome lives under it, combo badge
  blPanel(CX - 62, 24, 124, 50);
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", gameScore);
  gfx->setTextColor(UI_WHITE);
  setSize(4);
  setCur(centerX(buf, 4), 33);
  printT(buf);
  for (int i = 0; i < 3; i++)
    drawIdx(GS_LIFE, CX - 46 + i * 32, 80, 2, i >= 3 - gameMisses);
  if (blCombo >= 3) {
    char cb[8];
    snprintf(cb, sizeof(cb), "x%u", blCombo);
    gfx->setTextColor(UI_BAR_WARN);
    setSize(2);
    setCur(CX + 70, 42);
    printT(cb);
  }
  flushUI();
}

#else
void respawnBall() {
  ballX = 150 + UI_DX + random(166);
  ballY = 96;
  float sp = 1.6f + gameScore * 0.05f;  // mas viva segun avanzas
  if (sp > 4.0f) sp = 4.0f;
  ballVX = random(2) ? sp : -sp;
  ballVY = 0;
}

#define GAME_EXIT_Y 40  // franja de arriba que sale del minijuego (el marcador empieza en y = 30)

void gameTap(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  float dx = ballX - x, dy = ballY - y;
  bool hitBall = dx * dx + dy * dy < 74 * 74;
  // tocar el borde de arriba = abandonar sin premio. Solo si el toque no va a la bola y por
  // encima del marcador: con y < 72 la franja llegaba al texto del record y a la bola, que
  // sale a y = 96, y al ir a darle arriba se salia del juego (#39)
  if (!hitBall && y < GAME_EXIT_Y) {
    gameOpen = false;
    return;
  }
  if (hitBall) {  // toque a la bola!
    gameScore++;
    sfxPlay(SFX_PLAY);
    // golpe mas suave: impulso moderado que crece poco a poco con la puntuacion
    float lift = 6.6f + (gameScore > 16 ? 3.5f : gameScore * 0.22f);
    ballVY = -lift;
    ballVX += dx * 0.12f;
    if (ballVX > 6.5f) ballVX = 6.5f;
    if (ballVX < -6.5f) ballVX = -6.5f;
    hitX = ballX;
    hitY = ballY;
    hitTime = millis();
  }
}

void stepGame() {
  uint32_t now = millis();
  // paso relativo a 85 ms, el periodo de frame con el que se afinaron estas
  // constantes (ver el planificador de render en loop()): sin esto la fisica
  // avanzaba "un paso por frame" y el juego iba mas rapido o mas lento segun
  // lo que tardara en dibujarse el frame anterior, que depende del tamano del
  // sprite cargado. Con un Charizard la bola caia mas despacio que con un
  // Diglett, asi que el record no era comparable entre especies.
  float k = lastGameStep ? (now - lastGameStep) / 85.0f : 1.0f;
  if (k > 3.0f) k = 3.0f;  // frame anormalmente tardio: no dar un salto enorme
  lastGameStep = now;

  float grav = 0.40f + gameScore * 0.013f;  // cae un poco mas rapido cada vez
  if (grav > 0.80f) grav = 0.80f;
  ballVY += grav * k;
  ballX += ballVX * k;
  ballY += ballVY * k;
  // rebote en la pared circular
  float dx = ballX - CX, dy = ballY - CY;
  float d = sqrtf(dx * dx + dy * dy);
  if (d > 205) {
    float nx = dx / d, ny = dy / d;
    float dot = ballVX * nx + ballVY * ny;
    if (dot > 0) {
      ballVX = (ballVX - 2 * dot * nx) * 0.85f;
      ballVY = (ballVY - 2 * dot * ny) * 0.85f;
    }
    ballX = CX + nx * 205;
    ballY = CY + ny * 205;
  }
  if (ballY > 384) {  // al suelo
    if (++gameMisses >= 3) {
      gameNewHi = (gameScore > pet.gameHi);
      pet.playResult(gameScore);  // actualiza el record y da felicidad
      sfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
      gameOverUntil = millis() + 4000;
    } else {
      respawnBall();
    }
  }
  // el bicho la sigue por abajo
  float chase = (ballX - gamePetX) * 0.12f;
  if (chase > 7) chase = 7;
  if (chase < -7) chase = -7;
  gamePetX += chase * k;
}

#endif

// ---------- saco de entrenamiento (entrena la fuerza) ----------

void startSack() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
#if DROID_PREVIEW
  if (pet.onMission()) return;
#endif
#if BOARD_AMOLED_175C
  if (!trainingCaptureReady) { showControlToast("Touch capture unavailable"); return; }
  trainingCapture.reset();
#endif
  sackOpen = true;
  sackUntil = millis() + 10000;
  sackOverUntil = 0;
  sackHits = 0;
  sackShake = 0;
  sackNewHi = false;
}

void sackTap() {
  if (!timeLeft(sackUntil)) return;  // ya termino el tiempo
  sackHits++;
  sackShake = 16;  // sacude el saco
#if DROID_PREVIEW
  cargoTapAt = millis();
#endif
}

void drawGameScene();  // prototipo (definida mas abajo)
#if DROID_PREVIEW
// ---------- TamaDroid: cargo lift (polished; replaces the punching bag look) ----------
// The droid works a winch; every tap hauls the crate up the gantry crane for 10 s.
#define CG_GROUND 392
#define CG_CRATE_X 290

static int cargoLift(uint16_t hits) { int v = hits * 5; return v > 196 ? 196 : v; }

static void drawCrane(uint16_t ink) {
  uint16_t steel = C565(0x8a, 0x8e, 0x98), steelHi = C565(0xc8, 0xcc, 0xd4), steelDk = C565(0x50, 0x54, 0x60);
  for (int px : { 232, 342 }) {                       // posts with a highlight edge
    gfx->fillRect(px, 104, 8, CG_GROUND - 104, steel);
    gfx->fillRect(px, 104, 2, CG_GROUND - 104, steelHi);
    gfx->fillRect(px + 6, 104, 2, CG_GROUND - 104, steelDk);
    gfx->drawRect(px - 1, 103, 10, CG_GROUND - 102, ink);
  }
  for (int y = 112; y + 56 <= CG_GROUND - 6; y += 56) {  // cross bracing on the back, inside the posts
    gfx->drawLine(240, y, 342, y + 56, steelDk);
    gfx->drawLine(342, y, 240, y + 56, steelDk);
  }
  gfx->fillRect(222, 96, 134, 10, steel);             // top beam
  gfx->fillRect(222, 96, 134, 2, steelHi);
  gfx->drawRect(221, 95, 136, 12, ink);
  gfx->fillRect(226, CG_GROUND - 4, 126, 6, steelDk); // footing
}

static void drawWinch(int cx, int baseY, uint16_t hits, uint16_t ink) {
  uint16_t body = C565(0xd0, 0x8a, 0x2c), bodyDk = C565(0x96, 0x5c, 0x1a);
  gfx->fillRoundRect(cx - 20, baseY - 34, 40, 34, 4, body);
  gfx->fillRect(cx - 20, baseY - 12, 40, 12, bodyDk);
  gfx->drawRoundRect(cx - 20, baseY - 34, 40, 34, 4, ink);
  gfx->fillCircle(cx, baseY - 24, 9, C565(0x5a, 0x5e, 0x6a));   // drum
  gfx->drawCircle(cx, baseY - 24, 9, ink);
  float a = hits * 1.1f;                                         // crank turns with each tap
  int hx = cx + (int)(14 * cosf(a)), hy = baseY - 24 + (int)(14 * sinf(a));
  gfx->drawLine(cx, baseY - 24, hx, hy, ink);
  gfx->fillCircle(hx, hy, 3, C565(0xe0, 0x40, 0x30));
}

static void drawTimerRing(uint32_t left, uint32_t total) {
  const int N = 60;
  int on = (int)((uint64_t)left * N / total);
  for (int i = 0; i < N; i++) {
    float ang = -1.5708f + i * 6.2832f / N;
    int x = CX + (int)(220 * cosf(ang)), y = CY + (int)(220 * sinf(ang));
    uint16_t c = UI_TRACK;
    if (i < on) c = on > N / 2 ? UI_BAR_OK : (on > N / 5 ? UI_BAR_WARN : UI_BAR_BAD);
    gfx->fillCircle(x, y, 3, c);
  }
}

void renderCargo() {
  uint32_t now = millis();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = UI_INK;
  drawScene(0, now, night);

  if (sackOverUntil) {
    if (!timeLeft(sackOverUntil)) { sackOpen = false; return; }
    blPanel(CX - 130, 128, 260, 176);
    gfx->setTextColor(C565(0xa8, 0xb8, 0xe0));
    setSize(2);
    setCur(centerX("WINCH PULLS", 2), 146);
    printT("WINCH PULLS");
    char b[8];
    snprintf(b, sizeof(b), "%u", sackHits);
    gfx->setTextColor(UI_WHITE);
    setSize(6);
    setCur(centerX(b, 6), 176);
    printT(b);
    setSize(2);
    char g[18];
    snprintf(g, sizeof(g), T(S_STR_GAIN_FMT), sackGain);
    gfx->setTextColor(UI_BAR_OK);
    setCur(centerX(g, 2), 238);
    printT(g);
    if (sackNewHi && sackHits > 0) {
      gfx->setTextColor(UI_BAR_WARN);
      setCur(centerX(T(S_NEW_RECORD), 2), 268);
      printT(T(S_NEW_RECORD));
    } else {
      char r[18];
      snprintf(r, sizeof(r), T(S_RECORD_FMT), pet.strHi);
      gfx->setTextColor(C565(0xa8, 0xb8, 0xe0));
      setCur(centerX(r, 2), 268);
      printT(r);
    }
    flushUI();
    return;
  }

  if (!timeLeft(sackUntil)) {  // time's up: apply the training
    sackNewHi = (sackHits > pet.strHi);
    sackGain = pet.trainStrength(sackHits);
    sfxPlay(sackNewHi ? SFX_MEDAL : SFX_PLAY);
    sackOverUntil = now + 3500;
    blSpawnSparks(CG_CRATE_X, CG_GROUND - 64 - cargoLift(sackHits), 14, C565(0xff, 0xe0, 0x80), UI_WHITE);
    flushUI();
    return;
  }

  // physics-ish: the crate eases towards its target height and jolts on each pull
  sackShake *= 0.80f;
  int jolt = (int)(sackShake * 0.25f * sinf(now * 0.09f));
  int lift = cargoLift(sackHits);
  int crateTop = CG_GROUND - 64 - lift + jolt;

  drawCrane(ink);
  // best-so-far marker
  if (pet.strHi > 0) {
    int by = CG_GROUND - 64 - cargoLift(pet.strHi);
    for (int x = 244; x < 340; x += 10) gfx->fillRect(x, by, 6, 2, UI_BAR_WARN);
    gfx->setTextColor(UI_BAR_WARN);
    setSize(1);
    setCur(350, by - 4);
    printT("BEST");
  }
  // pulley and cables
  drawIdx(GS_PULLEY, CG_CRATE_X - 16, 104, 2, false);
  gfx->drawLine(CG_CRATE_X, 128, CG_CRATE_X, crateTop - 6, ink);
  gfx->drawLine(CG_CRATE_X - 14, 116, 196, CG_GROUND - 24, ink);
  gfx->fillRect(CG_CRATE_X - 6, crateTop - 8, 12, 8, C565(0x50, 0x54, 0x60));  // hook block
  // crate (dust when it leaves the ground)
  drawIdx(GS_CRATE, CG_CRATE_X - GS_CRATE.w, crateTop, 2, false);
  if (sackHits == 1 && now - cargoTapAt < 60)
    blSpawnSparks(CG_CRATE_X, CG_GROUND - 2, 10, C565(0xe8, 0xd0, 0xa0), C565(0xc8, 0xa8, 0x78));
  // winch + droid cranking it
  drawWinch(196, CG_GROUND, sackHits, ink);
  if (pmd.loaded) {
    bool working = now - cargoTapAt < 260;
    uint8_t act = working && pmd.has(PMD_WALKR) ? PMD_WALKR : PMD_IDLE;
    drawPmdAct(act, 128, CG_GROUND, now, true, false, 3);
  }
  if (now - cargoTapAt < 40) blSpawnSparks(196, CG_GROUND - 24, 3, C565(0xff, 0xd0, 0x60), UI_WHITE);
  for (auto &s : blSparks) {
    if ((int32_t)(s.until - now) <= 0) continue;
    s.x += s.vx * 0.6f; s.y += s.vy * 0.6f; s.vy += 0.25f;
    gfx->fillRect((int)s.x, (int)s.y, 3, 3, s.col);
  }

  // HUD: count panel, hint, and a ring of dots counting down the 10 s
  blPanel(CX - 62, 24, 124, 50);
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", sackHits);
  gfx->setTextColor(UI_WHITE);
  setSize(4);
  setCur(centerX(buf, 4), 33);
  printT(buf);
  if (sackHits < 3) {
    gfx->setTextColor(ink);
    setSize(2);
    setCur(centerX(T(S_HIT_FAST), 2), 80);
    printT(T(S_HIT_FAST));
  }
  drawTimerRing(sackUntil - now, 10000);
  flushUI();
}
#endif


void renderSack() {
#if DROID_PREVIEW
  renderCargo();
  return;
#endif
  uint32_t now = millis();
  drawGameScene();  // fondo del habitat
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  // pantalla de resultado
  if (sackOverUntil) {
    if (!timeLeft(sackOverUntil)) {
#if BOARD_AMOLED_175C
      leaveSack();
#else
      sackOpen = false;
#endif
      return;
    }
    char b[20];
    snprintf(b, sizeof(b), T(S_HITS_FMT), sackHits);
    gfx->setTextColor(ink);
    setSize(4);
    setCur(centerX(b, 4), 150);
    printT(b);
    char g[18];
    snprintf(g, sizeof(g), T(S_STR_GAIN_FMT), sackGain);
    gfx->setTextColor(UI_BAR_BAD);
    setSize(3);
    setCur(centerX(g, 3), 210);
    printT(g);
    setSize(2);
    if (sackNewHi && sackHits > 0) {
      gfx->setTextColor(UI_BAR_WARN);
      setCur(centerX(T(S_NEW_RECORD), 2), 256);
      printT(T(S_NEW_RECORD));
    } else {
      char r[18];
      snprintf(r, sizeof(r), T(S_RECORD_FMT), pet.strHi);
      gfx->setTextColor(ink);
      setCur(centerX(r, 2), 256);
      printT(r);
    }
    flushUI();
    return;
  }

  // se acabaron los 10 s: aplicar entrenamiento
  if (!timeLeft(sackUntil)) {
    sackNewHi = (sackHits > pet.strHi);
    sackGain = pet.trainStrength(sackHits);
#if BOARD_AMOLED_175C
    Serial.printf("Strength training: hits=%u input_drops=%lu poll=20ms exit_hold=1000ms\n",
                  sackHits, (unsigned long)trainingCapture.dropped());
#endif
    sfxPlay(sackNewHi ? SFX_MEDAL : SFX_PLAY);
    sackOverUntil = now + 3500;
    flushUI();
    return;
  }

#if BOARD_AMOLED_175C
  // Everything below is I2C-free. Snapshot policy state before starting the
  // worker; RAII joins it on return, including the blocking QSPI flush.
  uint32_t exitHeld = trainingCapture.exitHeldMs(now);
  TrainingCapture::Frame captureFrame(trainingCapture, !screenIsLocked());
#endif
  // aporreo activo
  sackShake *= 0.84f;
  int off = (int)(sackShake * sinf(now * 0.05f));
  int sx = CX + off, top = 86, sy = 150;
#if DROID_PREVIEW
  // cargo lift: every tap hauls the crate a little higher on its cable
  (void)top; (void)sy;
  int lift = sackHits * 4 > 128 ? 128 : sackHits * 4;
  int cy = 194 - lift;                                   // crate top (bottom stays above the counter)
  uint16_t wood = C565(0xb0, 0x7a, 0x45), woodDk = C565(0x7e, 0x55, 0x30);
  gfx->fillRect(CX - 60, 50, 120, 6, ink);               // gantry beam
  gfx->fillRect(sx - 1, 56, 3, cy - 56, ink);            // cable
  gfx->fillRect(sx - 8, cy - 8, 17, 8, ink);             // hook block
  gfx->fillRect(sx - 42, cy, 84, 60, wood);              // crate
  gfx->drawRect(sx - 42, cy, 84, 60, ink);
  gfx->drawRect(sx - 41, cy + 1, 82, 58, woodDk);
  for (int i = 0; i < 4; i++) {                          // X bracing
    gfx->drawLine(sx - 38 + i, cy + 4, sx + 38 - 3 + i, cy + 56, woodDk);
    gfx->drawLine(sx + 38 - i, cy + 4, sx - 38 + 3 - i, cy + 56, woodDk);
  }
#else
  gfx->fillRect(CX - 3, 56, 6, top - 56, ink);          // gancho/cuerda
  gfx->fillRect(sx - 4, top - 30, 8, 34, ink);          // cadena
  gfx->fillRoundRect(sx - 42, top, 84, 150, 26, C565(0xb5, 0x3a, 0x3a));  // saco
  gfx->fillRoundRect(sx - 42, top, 84, 22, 18, C565(0x7e, 0x28, 0x28));   // tapa
  gfx->drawRoundRect(sx - 42, top, 84, 150, 26, ink);
  gfx->fillRect(sx - 42, top + 70, 84, 4, C565(0x7e, 0x28, 0x28));        // costura
#endif

  // contador de golpes
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", sackHits);
  gfx->setTextColor(ink);
  setSize(6);
  setCur(centerX(buf, 6), 268);
  printT(buf);

  setSize(2);
  setCur(centerX(T(S_HIT_FAST), 2), 322);
  printT(T(S_HIT_FAST));

  // barra de tiempo
  uint32_t left = sackUntil - now;
  int bw = 280, fw = (int)((uint32_t)bw * left / 10000);
  gfx->fillRoundRect(CX - bw / 2, 350, bw, 16, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 350, fw, 16, 5, UI_BAR_OK);
#if BOARD_AMOLED_175C
  // Visible, separated from the bag/count/time bar, fully inside the circle.
  const int ex = TrainingTouch::EXIT_X, ey = TrainingTouch::EXIT_Y;
  const int ew = TrainingTouch::EXIT_W, eh = TrainingTouch::EXIT_H;
  gfx->fillRoundRect(ex, ey, ew, eh, 8, UI_TRACK);
  int progress = (int)(ew * (exitHeld > 1000 ? 1000 : exitHeld) / 1000);
  if (progress > 2) gfx->fillRoundRect(ex, ey, progress, eh, 8, UI_BAR_WARN);
  gfx->drawRoundRect(ex, ey, ew, eh, 8, ink);
  gfx->setTextColor(ink);
  setSize(2);
  setCur(centerX(T(S_HOLD_EXIT), 2), ey + 16);
  printT(T(S_HOLD_EXIT));
#endif

  flushUI();
}

// fondo del minijuego: hatibat del bicho (cielo por hora + suelo del bioma)
void drawGameScene() {
  int hh = sceneHour();
  bool night = hh < 6 || hh >= 20;
  uint16_t top, bot;
  if (night)       { top = C565(0x0c, 0x12, 0x24); bot = C565(0x1e, 0x26, 0x46); }
  else if (hh < 8) { top = C565(0xd1, 0x6a, 0x86); bot = C565(0xf3, 0xb8, 0x7c); }
  else if (hh < 18){ top = C565(0x8f, 0xc8, 0xea); bot = C565(0xdc, 0xee, 0xe6); }
  else             { top = C565(0xc7, 0x5a, 0x4a); bot = C565(0xf0, 0xae, 0x64); }
  int hor = 376;
  for (int y = 0; y < hor; y += 8)
    gfx->fillRect(0, y, LCD_WIDTH, 8, lerp565(top, bot, y, hor));
  if (night)
    for (auto &st : STARS) gfx->fillRect(st[0], st[1], 4, 4, UI_WHITE);
  uint8_t bio = pet.isEgg() ? 0 : DEX_TBL[pet.speciesId].biome;
  uint16_t soil = BIOME_SOIL[bio < 6 ? bio : 0];
#if DROID_PREVIEW
  soil = C565(0xe2, 0xbc, 0x82);  // Tatooine sand, like the main scene
#endif
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  gfx->fillRect(0, hor, LCD_WIDTH, LCD_HEIGHT - hor, soil);
}

void renderGame() {
#if DROID_PREVIEW
  renderBlaster();
  return;
#endif
  // sin fillScreen(NEGRO): drawGameScene cubre la pantalla completa (LCD_WIDTH x LCD_HEIGHT). Si el
  // DMA del flush anterior aun lee el buffer, vera contenido valido (no negro
  // a medio pintar), que era el parpadeo a 25 fps.
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  if (gameOverUntil) {
    drawGameScene();
    if (!timeLeft(gameOverUntil)) {
      gameOpen = false;
      return;
    }
    char buf[22];
    snprintf(buf, sizeof(buf), T(S_SCORE_FMT), gameScore);
    gfx->setTextColor(ink);
    setSize(4);
    setCur(centerX(buf, 4), 160);
    printT(buf);
    setSize(2);
    if (gameNewHi && gameScore > 0) {
      gfx->setTextColor(UI_BAR_WARN);
      setCur(centerX(T(S_NEW_RECORD), 2), 214);
      printT(T(S_NEW_RECORD));
    } else {
      char rec[20];
      snprintf(rec, sizeof(rec), T(S_RECORD_FMT), pet.gameHi);
      gfx->setTextColor(ink);
      setCur(centerX(rec, 2), 214);
      printT(rec);
    }
    const char *msg = gameScore >= 10 ? T(S_GREAT_JOY) : T(S_PLUS_JOY);
    gfx->setTextColor(ink);
    setCur(centerX(msg, 2), 250);
    printT(msg);
    flushUI();
    return;
  }

  drawGameScene();
  stepGame();

  // marcador, record y vidas
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", gameScore);
  gfx->setTextColor(ink);
  setSize(4);
  setCur(centerX(buf, 4), 30);
  printT(buf);
  // 24 y no 12: en japones "きろく %u" son 10 bytes antes de la cifra y el
  // record se quedaba en su primer digito ("きろく 2" con 219). Las tallas de
  // los buffers de texto las vigila TestBuffersDeTexto en test/test_tools.py.
  char rec[24];
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.gameHi);
  setSize(2);
  setCur(centerX(rec, 2), 76);
  printT(rec);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + UI_DX + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + UI_DX + i * 28, 104, 6, UI_TRACK);
  }

  if (pmd.loaded) {
    uint8_t act = (ballX > gamePetX + 4) ? PMD_WALKR : (ballX < gamePetX - 4) ? PMD_WALKL : PMD_IDLE;
    if (!pmd.has(act)) act = PMD_IDLE;
    drawPmdAct(act, (int)gamePetX, 394, millis(), true, false, 3);
  } else if (mon.loaded) {
    int s = (mon.h * 2 > 130) ? 1 : 2;
    int w = mon.w * s, h = mon.h * s;
    uint16_t fm = mon.frameMs ? mon.frameMs : 100;
    uint16_t fi = (millis() / fm) % mon.frames;
    const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
    int px = (int)gamePetX - w / 2, py = 394 - h;
    for (int r = 0; r < mon.h; r++) {
      const uint8_t *row = fr + r * mon.w;
      forRuns(mon.w, [&](int c) { return indexedKey(row[c], false); },
              [&](int c0, int len, int k) { gfx->fillRect(px + c0 * s, py + r * s, len * s, s, mon.pal[k]); });
    }
  }

  // anillo de impacto que se expande y desvanece (feedback suave del golpe)
  uint32_t ht = millis() - hitTime;
  if (hitTime && ht < 260) {
    int rad = 22 + (int)(ht / 6);
    gfx->drawCircle((int)hitX, (int)hitY, rad, C565(0xff, 0xe7, 0x9f));
    gfx->drawCircle((int)hitX, (int)hitY, rad - 2, C565(0xff, 0xd9, 0x8a));
  }

  // la pokeball
#if DROID_PREVIEW
  // training remote + its bolt (glowing head with a short tail)
  drawMap(SPR_ICON_REMOTE, 16, (int)remoteX - 24, 108 - 24, 3, false);
  if (boltLive) {
    float sp = sqrtf(ballVX * ballVX + ballVY * ballVY);
    float ux = sp > 0 ? ballVX / sp : 0, uy = sp > 0 ? ballVY / sp : 1;
    uint16_t glow = boltBack ? C565(0x80, 0xd0, 0xff) : C565(0xff, 0x50, 0x40);
    for (int i = 4; i >= 1; i--)
      gfx->fillCircle((int)(ballX - ux * i * 7), (int)(ballY - uy * i * 7), 6 - i, glow);
    gfx->fillCircle((int)ballX, (int)ballY, 7, glow);
    gfx->fillCircle((int)ballX, (int)ballY, 3, UI_WHITE);
  }
#else
  drawMap(SPR_ICON_PLAY, 16, (int)ballX - 24, (int)ballY - 24, 3, false);
#endif

  flushUI();
}

#if DROID_PREVIEW
// ---------- TamaDroid: circuit repair (memory game) ----------
// An open access panel: four lamps on a circuit board flash a sequence; repeat it.
// Each round adds one step. A wrong lamp shorts the board.
static const int16_t CIRC_PX[4] = { 0, 98, 0, -98 };   // up, right, down, left (offsets from centre)
static const int16_t CIRC_PY[4] = { -84, 0, 84, 0 };
#define CIRC_R 30
static const uint16_t CIRC_GLOW[4] = { C565(0xff, 0x58, 0x48), C565(0xff, 0xde, 0x58), C565(0x58, 0xe8, 0x78), C565(0x68, 0xb8, 0xff) };

static uint16_t circStepMs() { int v = 480 - circLen * 15; return v < 220 ? 220 : v; }

static void circAddStep(uint32_t delayMs) {
  if (circLen < sizeof(circSeq)) circSeq[circLen++] = random(4);
  circPos = 0;
  circPhase = 0;
  circT0 = millis() + delayMs;
}

void startCircuit() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony || pet.onMission()) return;
  circOpen = true;
  circLen = 0;
  circNewHi = false;
  circOverUntil = 0;
  circShortUntil = circOkUntil = circShakeUntil = 0;
  circFlash = -1;
  for (auto &s : blSparks) s.until = 0;
  circAddStep(800);
}

static void circGameOver() {
  uint16_t rounds = circLen ? circLen - 1 : 0;
  circNewHi = rounds > pet.circHi;
  pet.circuitResult(rounds);
  sfxPlay(circNewHi && rounds > 0 ? SFX_MEDAL : SFX_LEVEL);
  circPhase = 2;
  uint32_t now = millis();
  circShortUntil = now + 900;   // show the short circuit before the results panel
  circShakeUntil = now + 350;
  circOverUntil = now + 4500;
  blSpawnSparks(CX, CY, 16, C565(0xff, 0xe0, 0x60), C565(0xff, 0x60, 0x40));
}

void circuitTap(int16_t x, int16_t y) {
  if (circPhase == 2) return;
  if (y < GAME_EXIT_Y) { circOpen = false; return; }
  if (circPhase != 1) return;  // still showing the sequence
  int pad = -1;
  for (int i = 0; i < 4; i++) {
    int dx = x - (CX + CIRC_PX[i]), dy = y - (CY + CIRC_PY[i]);
    if (dx * dx + dy * dy <= (CIRC_R + 18) * (CIRC_R + 18)) { pad = i; break; }
  }
  if (pad < 0) return;
  circFlash = pad;
  circFlashUntil = millis() + 300;
  if (pad != circSeq[circPos]) { circGameOver(); return; }
  sfxPlay(SFX_TAP);
  blSpawnSparks(CX + CIRC_PX[pad], CY + CIRC_PY[pad], 6, CIRC_GLOW[pad], UI_WHITE);
  if (++circPos >= circLen) {
    circOkUntil = millis() + 450;
    if (circLen >= sizeof(circSeq)) circGameOver();  // finished every step
    else circAddStep(900);
  }
}

static void drawCircuitBoard(int ox, int oy, int lit, bool shorted) {
  uint16_t plate = C565(0x5a, 0x60, 0x6c), plateHi = C565(0x8a, 0x90, 0x9c), plateDk = C565(0x3a, 0x3e, 0x48);
  uint16_t pcb = shorted ? C565(0x5a, 0x24, 0x20) : C565(0x1c, 0x52, 0x30);
  uint16_t copper = C565(0xc0, 0x9a, 0x48), copperDk = C565(0x80, 0x64, 0x2c);
  // access panel plate with rivets
  gfx->fillRoundRect(ox - 172, oy - 146, 344, 292, 26, plateDk);
  gfx->fillRoundRect(ox - 170, oy - 144, 340, 288, 24, plate);
  gfx->drawRoundRect(ox - 170, oy - 144, 340, 288, 24, plateHi);
  for (int sx : { -150, 150 })
    for (int sy : { -124, 124 }) {
      gfx->fillCircle(ox + sx, oy + sy, 5, plateDk);
      gfx->fillCircle(ox + sx - 1, oy + sy - 1, 3, plateHi);
    }
  // circuit board
  gfx->fillRoundRect(ox - 142, oy - 118, 284, 236, 14, pcb);
  gfx->drawRoundRect(ox - 142, oy - 118, 284, 236, 14, copperDk);
  // decorative traces + pads in the corners
  const int16_t deco[][4] = { { -128, -100, -70, -100 }, { -70, -100, -70, -60 }, { 128, -100, 70, -100 }, { 70, -100, 70, -60 },
                             { -128, 100, -70, 100 }, { -70, 100, -70, 60 }, { 128, 100, 70, 100 }, { 70, 100, 70, 60 } };
  for (auto &d : deco) gfx->drawLine(ox + d[0], oy + d[1], ox + d[2], oy + d[3], copperDk);
  for (int sx : { -70, 70 })
    for (int sy : { -60, 60 }) { gfx->fillCircle(ox + sx, oy + sy, 4, copper); gfx->fillCircle(ox + sx, oy + sy, 1, pcb); }
  // main traces: chip to each lamp, doubled for weight; the lit one glows
  for (int i = 0; i < 4; i++) {
    uint16_t c = (i == lit) ? CIRC_GLOW[i] : copper;
    int lx = ox + CIRC_PX[i], ly = oy + CIRC_PY[i];
    if (CIRC_PX[i] == 0) { gfx->fillRect(lx - 2, (ly < oy ? ly : oy), 5, abs(ly - oy), c); }
    else { gfx->fillRect((lx < ox ? lx : ox), ly - 2, abs(lx - ox), 5, c); }
  }
  // lamps with a bloom when lit
  for (int i = 0; i < 4; i++) {
    int lx = ox + CIRC_PX[i], ly = oy + CIRC_PY[i];
    if (i == lit) {
      gfx->fillCircle(lx, ly, CIRC_R + 14, lerp565(pcb, CIRC_GLOW[i], 5, 16));
      gfx->fillCircle(lx, ly, CIRC_R + 8, lerp565(pcb, CIRC_GLOW[i], 9, 16));
    }
    const IdxSprite &sp = *GS_LAMPS[i][i == lit ? 1 : 0];
    drawIdx(sp, lx - sp.w, ly - sp.h, 2, false);
  }
  // centre chip with the round number
  drawIdx(GS_CHIP, ox - GS_CHIP.w, oy - GS_CHIP.h, 2, false);
  char n[6];
  snprintf(n, sizeof(n), "%u", circLen);
  gfx->setTextColor(UI_WHITE);
  setSize(3);
  setCur(ox - textW(n, 3) / 2, oy - 11);
  printT(n);
}

void renderCircuit() {
  uint32_t now = millis();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  drawScene(0, now, night);

  if (circPhase == 2 && !timeLeft(circShortUntil)) {
    if (!timeLeft(circOverUntil)) { circOpen = false; return; }
    blPanel(CX - 130, 128, 260, 176);
    gfx->setTextColor(C565(0xa8, 0xb8, 0xe0));
    setSize(2);
    setCur(centerX("ROUNDS REPAIRED", 2), 146);
    printT("ROUNDS REPAIRED");
    char b[8];
    snprintf(b, sizeof(b), "%u", circLen ? circLen - 1 : 0);
    gfx->setTextColor(UI_WHITE);
    setSize(6);
    setCur(centerX(b, 6), 176);
    printT(b);
    setSize(2);
    if (circNewHi && circLen > 1) {
      gfx->setTextColor(UI_BAR_WARN);
      setCur(centerX(T(S_NEW_RECORD), 2), 238);
      printT(T(S_NEW_RECORD));
    } else {
      char r[20];
      snprintf(r, sizeof(r), T(S_RECORD_FMT), pet.circHi);
      gfx->setTextColor(C565(0xa8, 0xb8, 0xe0));
      setCur(centerX(r, 2), 238);
      printT(r);
    }
    gfx->setTextColor(UI_BAR_OK);
    setCur(centerX("Circuits patched!", 2), 270);
    printT("Circuits patched!");
    flushUI();
    return;
  }

  // which lamp is on right now
  int lit = -1;
  if (circPhase == 0) {
    int32_t el = (int32_t)(now - circT0);
    if (el >= 0) {
      uint32_t slot = circStepMs() + 160;
      uint32_t idx = (uint32_t)el / slot;
      if (idx >= circLen) { circPhase = 1; circPos = 0; }
      else if ((uint32_t)el % slot < circStepMs()) lit = circSeq[idx];
    }
  }
  // the tapped lamp stays lit for its flash even after the last press of a round,
  // while the next sequence is waiting to start
  if (lit < 0 && circFlash >= 0 && timeLeft(circFlashUntil)) lit = circFlash;

  bool shorted = circPhase == 2;  // still inside the short-circuit window
  int ox = CX, oy = CY + 16;
  if (timeLeft(circShakeUntil)) { ox += (int)random(11) - 5; oy += (int)random(7) - 3; }
  drawCircuitBoard(ox, oy, shorted ? -1 : lit, shorted);

  // round complete: green ring round the chip
  if (timeLeft(circOkUntil))
    for (int r = 0; r < 3; r++) gfx->drawCircle(ox, oy, 36 + r, UI_BAR_OK);

  for (auto &s : blSparks) {
    if ((int32_t)(s.until - now) <= 0) continue;
    s.x += s.vx * 0.6f; s.y += s.vy * 0.6f; s.vy += 0.2f;
    gfx->fillRect((int)s.x, (int)s.y, 3, 3, s.col);
  }

  // prompt panel with a status LED, and step LEDs along the bottom of the plate
  blPanel(CX - 84, 24, 168, 46);
  const char *p = shorted ? "SHORT!" : (circPhase == 0 ? "WATCH" : "REPEAT");
  uint16_t led = shorted ? UI_BAR_BAD : (circPhase == 0 ? UI_BAR_WARN : UI_BAR_OK);
  gfx->fillCircle(CX - 62, 47, 6, led);
  gfx->setTextColor(UI_WHITE);
  setSize(3);
  setCur(CX - 46, 36);
  printT(p);
  int shown = circLen > 12 ? 12 : circLen;
  for (int i = 0; i < shown; i++) {
    int x = CX - (shown - 1) * 9 + i * 18;
    bool done = circPhase == 1 && i < circPos;
    gfx->fillCircle(x, oy + 132, 4, done ? UI_BAR_OK : C565(0x30, 0x34, 0x3c));
  }
  flushUI();
}
#endif

#if DROID_PREVIEW
// ---------- TamaDroid: salvage missions (picker, away sign, report) ----------
static const char *const MIS_NAME[MISSION_COUNT] = {
  "DUNE SCOUTING", "MOISTURE HAUL", "VAPORATOR REPAIR", "JAWA TRADE RUN", "TUSKEN TERRITORY", "SANDCRAWLER SALVAGE" };
static const char *const MIS_RISK[MISSION_COUNT] = { "LOW RISK", "LOW RISK", "MED RISK", "MED RISK", "HIGH RISK", "HIGH RISK" };
static const char *const PART_NAME[PART_COUNT] = { "SENSOR", "SERVO", "PLATING", "COUPLING", "HOLO-PROJ" };
static const char *const MIS_TITLE[4] = { "GREAT RUN!", "JOB DONE", "ROUGH TRIP", "FAILED" };
// [job][outcome][line]; '|' breaks the line
static const char *const MIS_LOG[MISSION_COUNT][4][3] = {
  { { "Mapped three dunes,|found a buried crate!", "Outran a sandstorm|with time to spare.", "Spotted a krayt trail|and logged it." },
    { "Scouted the ridge|beeping all the way.", "Found an old sensor|half buried in sand.", "Charted a safe path|for the farmers." },
    { "Got lost in the dunes|for a while.", "Sand in every joint.|Every. Joint.", "Tumbled down a dune|but kept going." },
    { "Sandstorm. Came back|scrubbed and sulking.", "Stuck in a sand pit|until a Jawa helped.", "Chased off by a womp|rat. Embarrassing." } },
  { { "Hauled twice the load.|The farmer tipped!", "Carried every canister|without a scratch.", "Fastest haul on the|homestead this season." },
    { "Delivered the water|right on time.", "Heavy cargo, steady|wheels. Job done.", "The farmer gave it|a friendly pat." },
    { "Dropped a canister.|Mostly saved it.", "Wheels overheated|halfway there.", "Took the long way|around a rock." },
    { "Tipped the whole load|into the sand.", "Servo seized. Had to|be towed home.", "Rolled into a ditch.|Water everywhere." } },
  { { "Fixed the vaporator|AND tuned two more.", "Rewired it perfectly.|It hums now.", "Found a spare coupling|in the housing!" },
    { "Vaporator fixed.|Moisture flowing.", "Swapped a fried coil.|Good as new.", "Patched a leak in|the condenser." },
    { "Fixed it after a|few small zaps.", "The repair held...|barely.", "Got stuck up the|tower for an hour." },
    { "Short circuit! Came|back smoking a bit.", "Made it worse.|Much worse.", "Zapped by the|condenser. Ouch." } },
  { { "Out-haggled the Jawas!|Utinni indeed.", "Traded junk for|real treasure.", "The Jawas threw in|a bonus part!" },
    { "Fair trade with|the Jawas today.", "Came back with parts|and a strange smell.", "Swapped scrap for|a working servo." },
    { "Jawas drove a hard|bargain. Barely even.", "Nearly got sold|by mistake.", "Lost its way back|from the sandcrawler." },
    { "Jawas took the parts|and tried to keep it.", "Ion blast from a Jawa.|Rebooted twice.", "Traded everything|for a broken lamp." } },
  { { "Slipped past the|Tuskens unseen!", "Salvaged a wreck in|Tusken land. Bold.", "Dodged a gaffi stick|and grabbed the loot." },
    { "Got in, got the|plating, got out.", "The Tuskens spotted it|too late.", "Sneaked through the|canyon. Nicely done." },
    { "Tuskens gave chase.|Escaped with dents.", "Hid in a crevice|for hours.", "A bantha sat on it.|Briefly." },
    { "Tuskens took a swing|at it. Ran home.", "Chased off by a|Tusken raiding party.", "Came back dented|and empty-handed." } },
  { { "Raided the crawler's|scrap bins. Jackpot!", "Found a holo-projector|under the junk!", "Best haul ever.|The Jawas never knew." },
    { "Picked through the|crawler's junk pile.", "Good salvage, only|small scratches.", "Came back loaded|with parts." },
    { "Got locked in the|cargo hold for a bit.", "Nearly got melted|down. Close one.", "Found mostly junk.|Some good bits." },
    { "Captured by Jawas!|Escaped, barely.", "Fell down the|ore smelter chute.", "Caught by the crawler|crew. Ran for it." } },
};

// prints "a|b" as two centred lines
static void printTwoLines(const char *s, int y, uint8_t size, uint16_t col) {
  char a[40], b[40];
  const char *bar = strchr(s, '|');
  if (bar) {
    size_t n = (size_t)(bar - s) < sizeof(a) - 1 ? (size_t)(bar - s) : sizeof(a) - 1;
    memcpy(a, s, n); a[n] = 0;
    snprintf(b, sizeof(b), "%s", bar + 1);
  } else { snprintf(a, sizeof(a), "%s", s); b[0] = 0; }
  gfx->setTextColor(col);
  setSize(size);
  setCur(centerX(a, size), y);
  printT(a);
  if (b[0]) { setCur(centerX(b, size), y + 10 * size); printT(b); }
}

static const char *misUses(uint8_t job) {
  const MissionDef &m = MISSIONS[job];
  if (m.wAtk && m.wDef && m.wSpe) return "USES ALL STATS";
  if (m.wDef && m.wSpe) return "USES ARM + SPD";
  if (m.wAtk && m.wSpe) return "USES STR + SPD";
  if (m.wAtk) return "USES STR";
  if (m.wDef) return "USES ARM";
  return "USES SPD";
}

// salvage crate (replaces the egg): rattles, dents with each tap, the lid springs open
void drawCrate(uint8_t taps) {
  uint32_t now = millis();
  int s = 4, w = GS_CRATE.w * s, h = GS_CRATE.h * s;
  int jig = (int)(taps * 1.5f * sinf(now * 0.02f));          // rattles more as it gives way
  if ((now / 1700) % 2 == 0 && (now % 1700) < 240) jig += (int)(3 * sinf(now * 0.08f));  // idle shiver
  int x = CX - w / 2 + jig, y = PET_GROUND - h;
  uint16_t ink = UI_INK, glow = C565(0xff, 0xe0, 0x80);
  gfx->fillRoundRect(CX - w / 2 - 6, PET_GROUND - 4, w + 12, 10, 5, lerp565(C565(0xe2, 0xbc, 0x82), ink, 4, 16));  // shadow
  if (taps >= 2) {  // lid ajar: light spills out of the gap
    gfx->fillRect(x + 8, y - 6, w - 16, 8, glow);
    for (int i = 0; i < 5; i++) gfx->drawLine(x + 20 + i * 30, y - 8, x + 10 + i * 34, y - 30 - (i % 2) * 10, glow);
  }
  drawIdx(GS_CRATE, x, y + (taps >= 2 ? 6 : 0), s, false);
  if (taps >= 1) {  // dents and a popped strap
    int yy = y + (taps >= 2 ? 6 : 0);
    gfx->drawLine(x + 22, yy + 30, x + 40, yy + 52, ink);
    gfx->drawLine(x + 40, yy + 52, x + 30, yy + 70, ink);
    gfx->drawLine(x + w - 30, yy + 18, x + w - 44, yy + 40, ink);
    gfx->fillRect(x + w - 50, yy - 4, 14, 6, C565(0x96, 0x9a, 0xa6));
  }
  if (taps >= 2) {  // lid prised up on the right
    uint16_t lid = C565(0x96, 0x70, 0x46);
    int lx0 = x - 4, ly0 = y + 4, lx1 = x + w + 6, ly1 = y - 20;
    gfx->fillTriangle(lx0, ly0, lx1, ly1, lx1, ly1 + 10, lid);
    gfx->fillTriangle(lx0, ly0, lx1, ly1 + 10, lx0, ly0 + 10, lid);
    gfx->drawLine(lx0, ly0, lx1, ly1, ink);
    gfx->drawLine(lx0, ly0 + 10, lx1, ly1 + 10, ink);
    gfx->drawLine(lx1, ly1, lx1, ly1 + 10, ink);
  }
}

void drawAway() {
  // wooden sign where the droid usually stands, tracks leading off over the dunes
  bool night = gNight;
  uint16_t wood = night ? C565(0x5a, 0x44, 0x30) : C565(0xa8, 0x7c, 0x4c);
  uint16_t woodDk = night ? C565(0x3a, 0x2c, 0x20) : C565(0x72, 0x52, 0x30);
  for (int i = 0; i < 9; i++) {  // tracks
    int x = CX + 30 + i * 22, y = 300 - i * 6;
    gfx->fillRect(x, y, 6, 3, woodDk);
    gfx->fillRect(x + 4, y + 6, 6, 3, woodDk);
  }
  gfx->fillRect(CX - 4, 250, 8, 62, woodDk);  // post
  gfx->fillRoundRect(CX - 110, 190, 220, 66, 8, wood);
  gfx->drawRoundRect(CX - 110, 190, 220, 66, 8, woodDk);
  gfx->drawRoundRect(CX - 108, 192, 216, 62, 7, woodDk);
  for (int nx : { CX - 100, CX + 96 }) gfx->fillRect(nx, 218, 4, 4, woodDk);  // nails
  // the header already says "Out on a mission": the sign just names the job and the time
  static const char *const SIGN[MISSION_COUNT] = {
    "DUNE SCOUT", "MOISTURE HAUL", "VAPORATOR", "JAWA TRADE", "TUSKEN LAND", "SANDCRAWLER" };
  uint8_t job = (uint8_t)pet.missionJob;
  uint16_t cream = C565(0xff, 0xf4, 0xd8);
  gfx->setTextColor(cream);
  setSize(2);
  setCur(centerX(SIGN[job], 2), 202);
  printT(SIGN[job]);
  char b[20];
  if (pet.missionLeft <= 1) snprintf(b, sizeof(b), "BACK ANY MINUTE");
  else snprintf(b, sizeof(b), "BACK IN %u MIN", pet.missionLeft);
  gfx->fillRect(CX - 80, 224, 160, 2, woodDk);  // divider
  setCur(centerX(b, 2), 232);
  printT(b);
}

void renderMissionPick() {
  uint32_t now = millis();
  drawScene(0, now, sceneHour() < 6 || sceneHour() >= 20);
  bool away = pet.onMission();
  uint8_t j = away ? (uint8_t)pet.missionJob : misSel;
  const MissionDef &m = MISSIONS[j];

  blPanel(CX - 84, 24, 168, 44);
  gfx->setTextColor(UI_WHITE);
  setSize(3);
  setCur(centerX("MISSIONS", 3), 35);
  printT("MISSIONS");

  blPanel(CX - 132, 82, 264, 238);
  gfx->setTextColor(UI_WHITE);
  setSize(2);
  setCur(centerX(MIS_NAME[j], 2), 96);
  printT(MIS_NAME[j]);
  char b[32];
  snprintf(b, sizeof(b), "%u MIN  %s", m.minutes, MIS_RISK[j]);
  uint16_t riskCol = j >= 4 ? UI_BAR_BAD : (j >= 2 ? UI_BAR_WARN : UI_BAR_OK);
  gfx->setTextColor(riskCol);
  setCur(centerX(b, 2), 122);
  printT(b);
  gfx->setTextColor(C565(0xa8, 0xb8, 0xe0));
  setCur(centerX(misUses(j), 2), 148);
  printT(misUses(j));

  // odds, or why it can't go
  const char *odds;
  uint16_t oc;
  if (away) { snprintf(b, sizeof(b), "BACK IN %u MIN", pet.missionLeft); odds = b; oc = UI_BAR_WARN; }
  else if (pet.level() < m.minLevel) { snprintf(b, sizeof(b), "UNLOCKS AT LV.%u", m.minLevel); odds = b; oc = UI_TRACK; }
  else if (pet.sleeping) { odds = "POWERED DOWN"; oc = UI_TRACK; }
  else if (pet.energy < 30 || pet.hygiene < 30) { odds = "NEEDS SYS+OIL 30"; oc = UI_BAR_BAD; }
  else {
    int margin = (int)pet.missionScore(j, 20) - (int)m.difficulty;
    odds = margin >= 25 ? "ODDS: GREAT" : margin >= 5 ? "ODDS: GOOD" : margin >= -15 ? "ODDS: FAIR" : "ODDS: RISKY";
    oc = margin >= 5 ? UI_BAR_OK : margin >= -15 ? UI_BAR_WARN : UI_BAR_BAD;
  }
  gfx->setTextColor(oc);
  setSize(2);
  setCur(centerX(odds, 2), 176);
  printT(odds);

  // what it can find
  gfx->setTextColor(C565(0xa8, 0xb8, 0xe0));
  setSize(1);
  setCur(CX - 96, 214);
  printT("FINDS");
  drawIdx(*GS_PARTS[m.partA], CX - 50, 204, 2, false);
  if (m.partB != m.partA) drawIdx(*GS_PARTS[m.partB], CX - 10, 204, 2, false);
  if (m.holoChance) drawIdx(GS_PART_HOLO, CX + 30, 204, 2, false);

  // parts bin
  gfx->drawLine(CX - 110, 248, CX + 110, 248, C565(0x4a, 0x58, 0x80));
  setSize(1);
  setCur(CX - 110, 256);
  printT("PARTS BIN");
  for (int i = 0; i < PART_COUNT; i++) {
    int x = CX - 112 + i * 46;
    drawIdx(*GS_PARTS[i], x, 270, 1, pet.parts[i] == 0);
    snprintf(b, sizeof(b), "%u", pet.parts[i]);
    gfx->setTextColor(UI_WHITE);
    setSize(2);
    setCur(x + 18, 272);
    printT(b);
  }

  // arrows + send button
  if (!away) {
    for (int s : { -1, 1 }) {
      int ax = CX + s * 172;
      gfx->fillCircle(ax, 200, 20, C565(0x1e, 0x26, 0x46));
      gfx->drawCircle(ax, 200, 20, UI_WHITE);
      gfx->setTextColor(UI_WHITE);
      setSize(3);
      setCur(ax - 8, 189);
      printT(s < 0 ? "<" : ">");
    }
  }
  bool can = !away && pet.canStartMission(j);
  uint16_t bc = away ? C565(0x4a, 0x58, 0x80) : can ? UI_BAR_OK : UI_TRACK;
  gfx->fillRoundRect(CX - 80, 334, 160, 44, 12, bc);
  gfx->drawRoundRect(CX - 80, 334, 160, 44, 12, UI_WHITE);
  const char *bl = away ? "OK" : "SEND";
  gfx->setTextColor(UI_WHITE);
  setSize(3);
  setCur(centerX(bl, 3), 345);
  printT(bl);
  flushUI();
}

void missionPickTap(int16_t x, int16_t y) {
  bool away = pet.onMission();
  if (y < GAME_EXIT_Y) { misPickOpen = false; return; }
  if (y >= 330 && y <= 384 && x >= CX - 84 && x <= CX + 84) {
    if (away) { misPickOpen = false; return; }
    if (pet.startMission(misSel)) { sfxPlay(SFX_PLAY); misPickOpen = false; }
    return;
  }
  if (away) return;
  if (x < CX - 132) misSel = (misSel + MISSION_COUNT - 1) % MISSION_COUNT;
  else if (x > CX + 132) misSel = (misSel + 1) % MISSION_COUNT;
}

void renderMissionReport() {
  uint32_t now = millis();
  drawScene(0, now, sceneHour() < 6 || sceneHour() >= 20);
  uint8_t j = pet.repJob >= 0 ? (uint8_t)pet.repJob : 0;
  uint8_t o = pet.repOutcome < 4 ? pet.repOutcome : 3;
  blPanel(CX - 150, 70, 300, 290);
  uint16_t tc = o == MIS_GREAT ? UI_BAR_WARN : o == MIS_SUCCESS ? UI_BAR_OK : o == MIS_ROUGH ? C565(0xa8, 0xb8, 0xe0) : UI_BAR_BAD;
  gfx->setTextColor(tc);
  setSize(3);
  setCur(centerX(MIS_TITLE[o], 3), 88);
  printT(MIS_TITLE[o]);
  gfx->setTextColor(C565(0xa8, 0xb8, 0xe0));
  setSize(2);
  setCur(centerX(MIS_NAME[j], 2), 124);
  printT(MIS_NAME[j]);
  printTwoLines(MIS_LOG[j][o][pet.repLine % 3], 160, 2, UI_WHITE);
  gfx->drawLine(CX - 120, 214, CX + 120, 214, C565(0x4a, 0x58, 0x80));
  int found = (pet.repParts[0] < PART_COUNT) + (pet.repParts[1] < PART_COUNT);
  if (!found) {
    gfx->setTextColor(UI_TRACK);
    setSize(2);
    setCur(centerX("NO PARTS FOUND", 2), 250);
    printT("NO PARTS FOUND");
  } else {
    int idx = 0;
    for (uint8_t p : pet.repParts) {
      if (p >= PART_COUNT) continue;
      int cx = found == 1 ? CX : CX - 64 + idx * 128;
      drawIdx(*GS_PARTS[p], cx - 16, 228, 2, false);
      gfx->setTextColor(UI_WHITE);
      setSize(1);
      setCur(cx - textW(PART_NAME[p], 1) / 2, 266);
      printT(PART_NAME[p]);
      idx++;
    }
  }
  gfx->setTextColor(C565(0x80, 0x90, 0xb8));
  setSize(1);
  setCur(centerX("tap to continue", 1), 330);
  printT("tap to continue");
  flushUI();
}
#endif

// ---------- ficha del bicho (deslizar vertical) ----------

// x donde arrancan las barras de la ficha. Estaba fijo en 150, que daba de sobra
// para etiquetas latinas de 3 caracteres pero no para las japonesas, mas anchas:
// こうげき se metia dentro de la barra. Se calcula a partir de la etiqueta mas
// larga para que las cuatro barras sigan alineadas en cualquier idioma, con el
// valor original como suelo (asi en los idiomas latinos no cambia nada).
// Requiere tener ya puesto el tamano de texto 2, porque textW() lo necesita.
static int statBarX() {
  const StrId ids[] = { S_STAT_ATK, S_STAT_DEF, S_STAT_SPE, S_STAT_WGT, S_VIN };
  int ancho = 0;
  for (StrId id : ids) {
    int w = textW(T(id), 2);
    if (w > ancho) ancho = w;
  }
  int x = 96 + UI_DX + ancho + 12;   // 12 px de aire entre etiqueta y barra
  return x < 150 + UI_DX ? 150 + UI_DX : x;
}

void drawCardStat(int y, const char *label, uint16_t val, uint16_t maxBar, uint16_t color) {
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(96 + UI_DX, y);
  printT(label);
  char num[8];
  snprintf(num, sizeof(num), "%u", val);
  setCur(330 + UI_DX, y);
  printT(num);
  int bx = statBarX();
  int bw = 310 + UI_DX - bx;   // la barra siempre acaba en 310, dejando aire hasta el numero
  int fw = (int)val * bw / maxBar;
  if (fw > bw) fw = bw;
  gfx->fillRoundRect(bx, y + 2, bw, 11, 3, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(bx, y + 2, fw, 11, 3, color);
}

// ---------- ajuste de hora en pantalla (deslizar abajo) ----------
// El usuario pone su hora LOCAL a ojo; el firmware la usa tal cual, asi que
// no hay que gestionar zona horaria. Preserva el dia (no rompe racha/edad).

void openClock() {
  uint32_t e = pet.lastSeenEpoch ? pet.lastSeenEpoch : rtcEpoch();
  clockH = (e / 3600) % 24;
  clockM = (e / 60) % 60;
  clockOpen = true;
}

void applyClock() {
  uint32_t base = pet.lastSeenEpoch ? pet.lastSeenEpoch : rtcEpoch();
  uint32_t e = (base / 86400) * 86400 + (uint32_t)clockH * 3600 + (uint32_t)clockM * 60;
  rtcSetEpoch(e);
  pet.setClock(e);
  clockOpen = false;
}

void drawClockBtn(int x, int y, const char *l) {
  gfx->fillRoundRect(x, y, 58, 58, 12, UI_WHITE);
  gfx->drawRoundRect(x, y, 58, 58, 12, UI_INK);
  gfx->setTextColor(UI_INK);
  setSize(4);
  setCur(x + 17, y + 15);
  printT(l);
}

// pildoras de idioma centradas en y; rellena la activa
#define LANG_PILL_Y 296
#define LANG_PILL_H 30
#define LANG_PILL_X (336 + UI_DX)          // pildora de idioma (cicla LANG_COUNT al tocar)
#define LANG_PILL_W 96
#define SND_PILL_X (34 + UI_DX)          // pildora de sonido (cicla alto, medio, bajo y apagado)
#define SND_PILL_W 96
#define POWER_PILL (HAS_DEEP_SLEEP && !HAS_AUDIO)  // left pill = power off instead of sound
#define BRI_PILL_X (183 + UI_DX)         // pildora de brillo, centrada (cicla los niveles al tocar)
#define BRI_PILL_W 100
static const char *const LANG_CODES[LANG_COUNT] = { "ES", "EN", "FR", "DE", "IT", "PT", "JA", "KO", "NL" };

// tres barras crecientes (brillo, volumen); las `lit` primeras van rellenas
static void drawLevelBars(int x, int lit) {
  for (int i = 0; i < 3; i++) {
    int bh = 8 + i * 5, bx = x + i * 14, by = LANG_PILL_Y + LANG_PILL_H - 6 - bh;
    if (i < lit) gfx->fillRect(bx, by, 9, bh, UI_BAR_OK);
    gfx->drawRect(bx, by, 9, bh, UI_INK);
  }
}

void renderClock() {
  gfx->fillScreen(RGB565_BLACK);
  fillBackdrop(UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(centerX(T(S_SET_TIME), 3), 44);
  printT(T(S_SET_TIME));

  char t[8];
  snprintf(t, sizeof(t), "%02d:%02d", clockH, clockM);
  setSize(7);
  setCur(CX - 105, 108);
  printT(t);

  drawClockBtn(104 + UI_DX, 190, "-");  // hora -
  drawClockBtn(170 + UI_DX, 190, "+");  // hora +
  drawClockBtn(252 + UI_DX, 190, "-");  // min -
  drawClockBtn(318 + UI_DX, 190, "+");  // min +
  setSize(2);
  gfx->setTextColor(UI_TRACK);
  setCur(120 + UI_DX, 256);
  printT(T(S_HOUR));
  setCur(276 + UI_DX, 256);
  printT(T(S_MIN));

#if POWER_PILL
  // no audio on this board: the left pill is "power off" (deep sleep, shake to wake)
  gfx->fillRoundRect(SND_PILL_X, LANG_PILL_Y, SND_PILL_W, LANG_PILL_H, 8, UI_WHITE);
  gfx->drawRoundRect(SND_PILL_X, LANG_PILL_Y, SND_PILL_W, LANG_PILL_H, 8, UI_INK);
  {
    int px = SND_PILL_X + 22, py = LANG_PILL_Y + LANG_PILL_H / 2 + 1;
    for (int r = 8; r <= 9; r++)            // power symbol: ring open at the top...
      for (int a = 40; a <= 320; a += 4) {
        float t = (a - 90) * DEG_TO_RAD;
        gfx->drawPixel(px + (int)roundf(r * cosf(t)), py + (int)roundf(r * sinf(t)), UI_BAR_BAD);
      }
    gfx->fillRect(px - 1, py - 11, 3, 10, UI_BAR_BAD);  // ...and the bar through the gap
    gfx->setTextColor(UI_INK);
    setSize(2);
    setCur(SND_PILL_X + 40, LANG_PILL_Y + 8);
    printT("OFF");
  }
#else
  // sonido (izquierda de la fila): un altavoz y tres barras con el volumen, o una X si esta apagado
  gfx->fillRoundRect(SND_PILL_X, LANG_PILL_Y, SND_PILL_W, LANG_PILL_H, 8, UI_WHITE);
  gfx->drawRoundRect(SND_PILL_X, LANG_PILL_Y, SND_PILL_W, LANG_PILL_H, 8, UI_INK);
  {
    int sx = SND_PILL_X + 18, sy = LANG_PILL_Y + LANG_PILL_H / 2;
    gfx->fillRect(sx, sy - 4, 5, 8, UI_INK);                           // cuerpo del altavoz
    gfx->fillTriangle(sx + 4, sy, sx + 12, sy - 9, sx + 12, sy + 9, UI_INK);  // bocina
    if (audioEnabled()) {
      drawLevelBars(SND_PILL_X + 46, VOL_LEVELS - audioVolume());
    } else {
      int xx = SND_PILL_X + 58;
      for (int d = -1; d <= 1; d++) {  // X de 3 px de grueso
        gfx->drawLine(xx - 7 + d, sy - 7, xx + 7 + d, sy + 7, UI_BAR_BAD);
        gfx->drawLine(xx - 7 + d, sy + 7, xx + 7 + d, sy - 7, UI_BAR_BAD);
      }
    }
  }
#endif

  // nivel de brillo (centro de la fila): un sol y tres barras, las encendidas son el nivel
  gfx->fillRoundRect(BRI_PILL_X, LANG_PILL_Y, BRI_PILL_W, LANG_PILL_H, 8, UI_WHITE);
  gfx->drawRoundRect(BRI_PILL_X, LANG_PILL_Y, BRI_PILL_W, LANG_PILL_H, 8, UI_INK);
  {
    int sx = BRI_PILL_X + 26, sy = LANG_PILL_Y + LANG_PILL_H / 2;
    gfx->fillCircle(sx, sy, 5, UI_INK);
    static const int8_t RAY[8][2] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1} };
    for (int i = 0; i < 8; i++) {
      int k = i < 4 ? 8 : 6, e = i < 4 ? 11 : 9;  // las diagonales algo mas cortas
      gfx->drawLine(sx + RAY[i][0] * k, sy + RAY[i][1] * k, sx + RAY[i][0] * e, sy + RAY[i][1] * e, UI_INK);
    }
    drawLevelBars(BRI_PILL_X + 52, BRIGHT_LEVELS - userDim);
  }

  // selector de idioma: una pildora que cicla los 6 idiomas al tocar
  gfx->fillRoundRect(LANG_PILL_X, LANG_PILL_Y, LANG_PILL_W, LANG_PILL_H, 8, UI_WHITE);
  gfx->drawRoundRect(LANG_PILL_X, LANG_PILL_Y, LANG_PILL_W, LANG_PILL_H, 8, UI_INK);
  char lp[10];
  snprintf(lp, sizeof(lp), "%s >", LANG_CODES[gLang]);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(LANG_PILL_X + (LANG_PILL_W - textW(lp, 2)) / 2, LANG_PILL_Y + 8);
  printT(lp);

  gfx->fillRoundRect(133 + UI_DX, 340, 200, 48, 14, UI_BAR_OK);
  gfx->setTextColor(UI_BG_DAY);
  setSize(3);
  setCur(CX - 18, 352);
  printT("OK");

  gfx->setTextColor(UI_TRACK);
  setSize(2);
  setCur(centerX(T(S_CLOCK_CANCEL), 2), 410);
  printT(T(S_CLOCK_CANCEL));

  // version del firmware (discreta, abajo del todo)
  char ver[20];
  snprintf(ver, sizeof(ver), "TamaPoke v%s", FW_VERSION);
  setSize(1);
  setCur(centerX(ver, 1), 436);
  printT(ver);
  flushUI();
}

void clockTap(int16_t x, int16_t y) {
  if (y >= 190 && y <= 248) {  // fila de botones +/-
    if (x >= 104 + UI_DX && x < 162 + UI_DX) clockH = (clockH + 23) % 24;
    else if (x >= 170 + UI_DX && x < 228 + UI_DX) clockH = (clockH + 1) % 24;
    else if (x >= 252 + UI_DX && x < 310 + UI_DX) clockM = (clockM + 59) % 60;
    else if (x >= 318 + UI_DX && x < 376 + UI_DX) clockM = (clockM + 1) % 60;
    return;
  }
  if (y >= LANG_PILL_Y && y <= LANG_PILL_Y + LANG_PILL_H) {
#if POWER_PILL
    if (x >= SND_PILL_X && x < SND_PILL_X + SND_PILL_W) {  // power off: deep sleep, shake to wake
      clockOpen = false;  // wake up on the pet screen, not in settings
      deepSleepNow("menu");
      return;             // only reached if sleep could not be armed
    }
#endif
    if (x >= SND_PILL_X && x < SND_PILL_X + SND_PILL_W) {  // cicla sonido: alto, medio, bajo, apagado
      if (!audioEnabled()) {
        audioSetVolume(0);
        audioSetEnabled(true);
      } else if (audioVolume() + 1 < VOL_LEVELS) {
        audioSetVolume(audioVolume() + 1);
      } else {
        audioSetEnabled(false);
      }
      if (audioEnabled()) sfxPlay(SFX_TAP);    // se oye el nivel nuevo
      return;
    }
    if (x >= BRI_PILL_X && x < BRI_PILL_X + BRI_PILL_W) {  // cicla brillo: alto, medio, bajo
      userDim = (userDim + 1) % BRIGHT_LEVELS;
      Preferences p;
      p.begin("tamapoke", false);
      p.putUChar("bri", userDim);
      p.end();
      sfxPlay(SFX_TAP);
      return;
    }
    if (x >= LANG_PILL_X && x < LANG_PILL_X + LANG_PILL_W) {  // cicla idioma
      setLang((Lang)((gLang + 1) % LANG_COUNT));
      applyLangFont();  // la fuente cambia con el idioma
      sfxPlay(SFX_TAP);
      return;
    }
  }
  if (y >= 340 && y <= 388 && x >= 133 + UI_DX && x <= 333 + UI_DX) { applyClock(); return; }
}

// llama + numero de racha arriba a la izquierda
void drawStreakBadge() {
  if (pet.streak < 1) return;
  int x = 26, y = 16;
  gfx->fillTriangle(x + 8, y, x + 1, y + 17, x + 15, y + 17, UI_BAR_BAD);
  gfx->fillTriangle(x + 8, y + 7, x + 4, y + 17, x + 12, y + 17, UI_BAR_WARN);
  char s[6];
  snprintf(s, sizeof(s), "%u", pet.streak);
  gfx->setTextColor(inkColor());
  setSize(2);
  setCur(x + 22, y + 2);
  printT(s);
}

// banner temporal: medalla nueva o hito de racha
void drawCelebration() {
  const char *l1 = nullptr, *l2 = nullptr;
  // 32 y no 20: "%u にちれんぞく！" ya pasa de 20 con una sola cifra, y snprintf
  // cortaba a mitad de un caracter de 3 bytes.
  char buf[32];
  if (pet.showMedal()) {
    for (int i = 0; i < MED_COUNT; i++)
      if (pet.newMedal & (1 << i)) { l2 = medalName(i); break; }
    l1 = T(S_MEDAL_BANNER);
  } else if (pet.showMilestone()) {
    snprintf(buf, sizeof(buf), T(S_STREAK_DAYS_FMT), pet.streak);
    l1 = T(S_GREAT);
    l2 = buf;
  }
  if (!l1) return;
  gfx->fillRoundRect(73 + UI_DX, 150, 320, 96, 16, UI_BAR_WARN);
  gfx->drawRoundRect(73 + UI_DX, 150, 320, 96, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(centerX(l1, 3), 176);
  printT(l1);
  setSize(2);
  setCur(centerX(l2, 2), 212);
  printT(l2);
}

// medallas en la ficha: badge con etiqueta, color si conseguida
void drawMedalBadge(int x, int y, int i) {
  bool got = pet.hasMedal(1 << i);
  gfx->fillRoundRect(x, y, 100, 24, 6, got ? UI_BAR_OK : UI_TRACK);
  if (!got) gfx->drawRoundRect(x, y, 100, 24, 6, UI_TRACK);
  gfx->setTextColor(got ? UI_BG_DAY : 0x9492);
  setSize(2);
  setCur(x + (100 - textW(medalLabel(i), 2)) / 2, y + 5);
  printT(medalLabel(i));
}

// pagina 0: perfil (retrato grande, identidad, racha, vinculo, baya)
void renderCardProfile() {
  const DexEntry &d = DEX_TBL[pet.speciesId];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  char head[32];
  snprintf(head, sizeof(head), T(S_NAME_FMT), pet.shiny ? "*" : "", nm, pet.level());
  gfx->setTextColor(d.accent);
  // auto-encoge: a tamano 3 los nombres largos no caben en la franja estrecha de
  // arriba de la pantalla redonda, asi que se cortaban por el borde. El ancho se
  // MIDE con textW(): strlen() cuenta bytes, y en una fila UTF-8 no hay un byte
  // por caracter ni todos miden lo mismo. Con la fuente clasica textW() devuelve
  // strlen()*6*size, o sea exactamente la cuenta anterior: no mueve un pixel en
  // los seis idiomas latinos (11 caracteres a tamano 3 son esos 198 px).
  setSize(3);
  int hts = (textW(head, 3) <= 198) ? 3 : 2;
  setSize(hts);
  setCur(centerX(head, hts), hts == 3 ? 34 : 40);
  printT(head);
  if (pet.nick[0]) {  // especie real bajo el apodo
    const char *sp = dexName(pet.speciesId);
    char par[32];
    snprintf(par, sizeof(par), "(%s)", sp);
    gfx->setTextColor(UI_TRACK);
    setSize(2);
    setCur(centerX(par, 2), 64);   // medido, no contado en bytes
    printT(par);
  }

  // retrato grande animado
  if (pmd.loaded) drawPmdAct(PMD_IDLE, CX, 206, millis(), true, false, 4);

  // racha con llama
  int sx = 138 + UI_DX, sy = 224;
  gfx->fillTriangle(sx + 8, sy, sx + 1, sy + 18, sx + 15, sy + 18, UI_BAR_BAD);
  gfx->fillTriangle(sx + 8, sy + 7, sx + 4, sy + 18, sx + 12, sy + 18, UI_BAR_WARN);
  char rl[40];  // en japones pierde cifras con 30 a partir de 100 dias
  snprintf(rl, sizeof(rl), T(S_STREAK_FMT), pet.streak, pet.bestStreak);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(sx + 24, sy + 2);
  printT(rl);

  drawCardStat(258, T(S_VIN), pet.bond, 100, C565(0xd4, 0x52, 0x7e));

  char info[48];
#if DROID_PREVIEW
  snprintf(info, sizeof(info), "MISSIONS %u   AGE %lud", pet.missionsDone,
           (unsigned long)(pet.ageMinutes / 1440));
#else
  const char *berry = !pet.berryKnown ? T(S_BERRY_UNK)
                      : pet.lovesBerry(0) ? T(S_BERRY_RED)
                      : pet.lovesBerry(1) ? T(S_BERRY_BLUE)
                                          : T(S_BERRY_GREEN);
  snprintf(info, sizeof(info), T(S_INFO_FMT), berry,
           (unsigned long)(pet.ageMinutes / 1440));
#endif
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(centerX(info, 2), 296);
  printT(info);

  gfx->setTextColor(UI_TRACK);
  setCur(centerX(T(S_RENAME_HINT), 2), 332);
  printT(T(S_RENAME_HINT));
}

// pagina 1: combate (4 barras + boton de entrenar)
void renderCardStats() {
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(centerX(T(S_BATTLE), 3), 48);
  printT(T(S_BATTLE));

  drawCardStat(118, T(S_STAT_ATK), pet.atkStat(), 260, UI_BAR_BAD);
  drawCardStat(160, T(S_STAT_DEF), pet.defStat(), 260, 0x4C98);
  drawCardStat(202, T(S_STAT_SPE), pet.speStat(), 260, UI_BAR_WARN);
  drawCardStat(244, T(S_STAT_WGT), pet.weight, 100, 0xB3C8);

  // boton: saco de entrenamiento de fuerza
  gfx->fillRoundRect(96 + UI_DX, 300, 274, 40, 12, UI_BAR_BAD);
  gfx->setTextColor(UI_BG_DAY);
  setSize(2);
  setCur(centerX(T(S_TRAIN_STR), 2), 311);
  printT(T(S_TRAIN_STR));
}

// pagina 2: medallas con etiqueta descriptiva
void renderCardMedals() {
  int got = 0;
  for (int i = 0; i < MED_COUNT; i++)
    if (pet.hasMedal(1 << i)) got++;
  char head[24];
  snprintf(head, sizeof(head), T(S_MEDALS_FMT), got, MED_COUNT);
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(centerX(head, 3), 48);
  printT(head);

  for (int i = 0; i < MED_COUNT; i++) {
    int x = 28 + UI_DX + (i % 2) * 206, y = 104 + (i / 2) * 54;
    bool g = pet.hasMedal(1 << i);
    gfx->fillRoundRect(x, y, 196, 44, 10, g ? UI_BAR_OK : UI_TRACK);
    if (g) {  // marca de conseguida
      gfx->fillCircle(x + 22, y + 22, 11, UI_BG_DAY);
      gfx->setTextColor(UI_BAR_OK);
      setSize(2);
      setCur(x + 16, y + 13);
      printT("v");
    }
    gfx->setTextColor(g ? UI_BG_DAY : 0x8410);
    setSize(2);
    setCur(x + 44, y + 14);
    printT(medalDesc(i));
  }
}

// pagina 3: progreso (nivel, evolucion, descuidos) — saca a la luz mecanicas
// que antes eran invisibles (cuanto falta para subir/evolucionar y por que)
void renderCardProgress() {
  const DexEntry &d = DEX_TBL[pet.speciesId];
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(centerX(T(S_PROGRESS), 3), 44);
  printT(T(S_PROGRESS));

  // nivel grande
  char lv[10];
  snprintf(lv, sizeof(lv), T(S_LVL_FMT), pet.level());
  setSize(5);
  setCur(centerX(lv, 5), 86);
  printT(lv);

  // barra de progreso al siguiente nivel (1 nivel = 60 min de juego)
  uint8_t into = pet.ageMinutes % MINUTES_PER_LEVEL;
  int bx = 93 + UI_DX, bw = 280, by = 158, bh = 22;
  gfx->fillRoundRect(bx, by, bw, bh, 6, UI_TRACK);
  int fw = (bw - 4) * into / MINUTES_PER_LEVEL;
  if (fw > 0) gfx->fillRoundRect(bx + 2, by + 2, fw, bh - 4, 5, UI_BAR_OK);
  char nx[32];
  snprintf(nx, sizeof(nx), T(S_NEXT_LVL_FMT), MINUTES_PER_LEVEL - into, pet.level() + 1);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(centerX(nx, 2), by + 32);
  printT(nx);

  // estado de evolucion
  gfx->setTextColor(UI_TRACK);
  setCur(centerX(T(S_EVO_LABEL), 2), 230);
  printT(T(S_EVO_LABEL));
  char evoBuf[32];
  const char *evo;
  uint16_t evoCol = UI_INK;
  if (d.evolvesTo == 0) {
    evo = T(S_FINAL_FORM);
  } else {
    int needed = d.evolveLevel + pet.careMistakes;
    if (pet.level() >= needed) {
      if (pet.lowestStat() >= 40) { evo = T(S_EVO_READY); evoCol = UI_BAR_OK; }
      else { evo = T(S_EVO_BLOCKED); evoCol = UI_BAR_BAD; }
    } else {
      snprintf(evoBuf, sizeof(evoBuf), T(S_EVO_IN_FMT), needed - pet.level());
      evo = evoBuf;
    }
  }
  gfx->setTextColor(evoCol);
  setCur(centerX(evo, 2), 256);
  printT(evo);

  // descuidos (retrasan la evolucion)
  char ms[24];
  snprintf(ms, sizeof(ms), T(S_MISTAKES_FMT), pet.careMistakes);
  gfx->setTextColor(pet.careMistakes > 0 ? UI_BAR_BAD : UI_INK);
  setCur(centerX(ms, 2), 312);
  printT(ms);
}

void renderCard() {
  gfx->fillScreen(RGB565_BLACK);
  fillBackdrop(UI_BG_DAY);
  if (cardPage == 0) renderCardProfile();
  else if (cardPage == 1) renderCardStats();
  else if (cardPage == 2) renderCardMedals();
  else renderCardProgress();

  // indicador de 4 paginas + ayuda
  for (int i = 0; i < 4; i++) {
    if (i == cardPage) gfx->fillCircle(194 + UI_DX + i * 26, 374, 5, UI_INK);
    else gfx->drawCircle(194 + UI_DX + i * 26, 374, 4, UI_INK);
  }
  gfx->setTextColor(UI_TRACK);
  setSize(2);
  setCur(centerX(T(S_BACK), 2), 398);
  printT(T(S_BACK));
  flushUI();
}

// ---------- teclado para renombrar ----------

static const char KB_KEYS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-";  // 28 + DEL + OK = 30
#define KB_COLS 6
#define KB_X (40 + UI_DX)
#define KB_Y 150
#define KB_W 64
#define KB_H 52

void openKeyboard() {
  kbOpen = true;
  strncpy(nameBuf, pet.nick, sizeof(nameBuf) - 1);
  nameBuf[sizeof(nameBuf) - 1] = 0;
  nameLen = strlen(nameBuf);
}

void renderKeyboard() {
  gfx->fillScreen(RGB565_BLACK);
  fillBackdrop(UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(centerX(T(S_NAME), 2), 56);
  printT(T(S_NAME));
  // buffer actual
  gfx->fillRoundRect(83 + UI_DX, 84, 300, 40, 8, UI_WHITE);
  gfx->drawRoundRect(83 + UI_DX, 84, 300, 40, 8, UI_INK);
  setSize(3);
  setCur(95 + UI_DX, 94);
  printT(nameLen ? nameBuf : "_");

  for (int i = 0; i < 30; i++) {
    int x = KB_X + (i % KB_COLS) * KB_W, y = KB_Y + (i / KB_COLS) * KB_H;
    bool special = (i >= 28);
    gfx->fillRoundRect(x, y, KB_W - 6, KB_H - 6, 6, special ? UI_BAR_WARN : UI_WHITE);
    gfx->drawRoundRect(x, y, KB_W - 6, KB_H - 6, 6, UI_INK);
    gfx->setTextColor(UI_INK);
    setSize(2);
    if (i < 28) {
      setCur(x + KB_W / 2 - 9, y + KB_H / 2 - 10);
      printT(KB_KEYS[i]);
    } else {
      const char *lab = (i == 28) ? "<-" : "OK";
      setCur(x + KB_W / 2 - 15, y + KB_H / 2 - 10);
      printT(lab);
    }
  }
  flushUI();
}

void keyboardTap(int16_t x, int16_t y) {
  // filtra ANTES de dividir: con x/y menores que KB_X/KB_Y la resta da negativo
  // y la division entera trunca hacia cero (no hacia -inf), asi que col/row
  // saldria 0 en vez de negativo y el filtro de abajo no lo detectaria
  if (x < KB_X || y < KB_Y) return;
  int col = (x - KB_X) / KB_W, row = (y - KB_Y) / KB_H;
  if (col >= KB_COLS || row >= 5) return;
  int i = row * KB_COLS + col;
  if (i >= 30) return;
  if (i == 28) {  // borrar
    if (nameLen) nameBuf[--nameLen] = 0;
  } else if (i == 29) {  // OK
    pet.rename(nameBuf);
    kbOpen = false;
  } else if (nameLen < sizeof(nameBuf) - 1) {
    nameBuf[nameLen++] = KB_KEYS[i];
    nameBuf[nameLen] = 0;
  }
}

// ---------- galeria pokedex ----------

#define GAL_X (73 + UI_DX)
#define GAL_Y 84
#define GAL_CELL 80

// dibuja una miniatura centrada en su celda; sil=true la pinta en tinta
void drawThumb(const uint8_t *b, int x, int y, int s, bool sil) {
  uint8_t w = b[0], h = b[1], n = b[2];
  const uint8_t *pal = b + 3;
  const uint8_t *d = pal + n * 2;
  int ox = x + (GAL_CELL - w * s) / 2;
  int oy = y + (GAL_CELL - h * s) / 2;
  for (int r = 0; r < h; r++) {
    const uint8_t *row = d + r * w;
    forRuns(w, [&](int c) { return indexedKey(row[c], sil); },
            [&](int c0, int len, int k) {
              uint16_t col = sil ? INK_K : (uint16_t)(pal[k * 2] | (pal[k * 2 + 1] << 8));
              gfx->fillRect(ox + c0 * s, oy + r * s, len * s, s, col);
            });
  }
}

void renderGallery() {
  if (galleryDetail) {  // vista detalle: se redibuja siempre (animada)
    gfx->fillScreen(RGB565_BLACK);
    fillBackdrop(UI_BG_DAY);
    const DexEntry &d = DEX_TBL[galleryDetail];
    bool reg = pet.isRegistered(galleryDetail);
    char head[32];
    snprintf(head, sizeof(head), "N.%03d %s%s", galleryDetail,
             pet.isShinyRegistered(galleryDetail) ? "*" : "", reg ? dexName(galleryDetail) : "???");
    gfx->setTextColor(reg ? d.accent : UI_INK);
    // auto-encoge nombres largos (no caben a t3), midiendo en vez de contar
    // bytes; 13 caracteres a tamano 3 son los 234 px de antes.
    setSize(3);
    int gts = (textW(head, 3) <= 234) ? 3 : 2;
    setSize(gts);
    setCur(centerX(head, gts), gts == 3 ? 56 : 60);
    printT(head);
    if (galleryPmd.loaded) {
      // animado y a color si esta registrado; silueta estatica si no (estilo "?")
      drawPmdActM(galleryPmd, PMD_IDLE, CX, 300, reg ? millis() : 0, true, !reg, 6);
    } else {
      const uint8_t *t = thumbs.get(galleryDetail);
      if (t) drawThumb(t, CX - GAL_CELL, 135, 4, !reg);
    }
    gfx->setTextColor(UI_INK);
    setSize(2);
    setCur(centerX(T(S_DETAIL_BACK), 2), 408);
    printT(T(S_DETAIL_BACK));
    flushUI();
    return;
  }

  if (!galleryDirty) return;  // la rejilla es estatica
  galleryDirty = false;

  gfx->fillScreen(RGB565_BLACK);
  fillBackdrop(UI_BG_DAY);
  char head[24];
  snprintf(head, sizeof(head), T(S_POKEDEX_FMT), pet.registeredCount());
  gfx->setTextColor(UI_INK);
  setSize(3);
  setCur(centerX(head, 3), 36);
  printT(head);

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      int16_t dex = galleryPage * 16 + r * 4 + c + 1;
      if (dex > 151) break;
      int x = GAL_X + c * GAL_CELL, y = GAL_Y + r * GAL_CELL;
      const uint8_t *t = thumbs.get(dex);
      if (t) {
        drawThumb(t, x, y, 2, !pet.isRegistered(dex));
        if (pet.isShinyRegistered(dex)) {
          gfx->setTextColor(UI_BAR_WARN);
          setSize(2);
          setCur(x + 62, y + 4);
          printT("*");
        }
      } else {
        char num[6];
        snprintf(num, sizeof(num), "%d", dex);
        gfx->setTextColor(UI_TRACK);
        setSize(2);
        setCur(x + 24, y + 32);
        printT(num);
      }
    }
  }
  // puntos de pagina
  for (int i = 0; i < 10; i++) {
    if (i == galleryPage) gfx->fillCircle(170 + UI_DX + i * 14, 436, 4, UI_INK);
    else gfx->drawCircle(170 + UI_DX + i * 14, 436, 3, UI_INK);
  }
  flushUI();
}

void galleryTap(int16_t x, int16_t y) {
  if (galleryDetail) {  // volver a la rejilla
    galleryDetail = 0;
    galleryPmd.unload();
    galleryDirty = true;
    return;
  }
  if (y < 72) {  // tocar la cabecera = salir
    galleryOpen = false;
    galleryPmd.unload();
    return;
  }
  // mismo motivo que en el teclado: filtrar antes de dividir
  if (x < GAL_X || y < GAL_Y) return;
  int c = (x - GAL_X) / GAL_CELL, r = (y - GAL_Y) / GAL_CELL;
  if (c > 3 || r > 3) return;
  int16_t dex = galleryPage * 16 + r * 4 + c + 1;
  if (dex > 151) return;
  galleryDetail = dex;
  galleryPmd.load(dex, pet.isShinyRegistered(dex));
}

void drawBattery() {
  int pc = batPercent();
  if (pc < 0) return;  // sin bateria conectada
  int x = CX - 14, y = 12, w = 24, h = 11;
  bool charging = batCharging();
  uint16_t col = charging ? UI_BAR_OK
                 : (pc >= 40) ? inkColor()
                 : (pc >= 15) ? UI_BAR_WARN
                              : UI_BAR_BAD;
  gfx->drawRoundRect(x, y, w, h, 2, col);
  gfx->fillRect(x + w, y + 3, 3, 5, col);  // borne
  if (charging) {
    // rayo de carga (zigzag) en vez de la barra de nivel
    uint16_t bolt = C565(0xff, 0xd9, 0x4a);
    int bx = x + w / 2;
    gfx->fillTriangle(bx + 3, y + 1, bx - 4, y + 6, bx + 1, y + 6, bolt);
    gfx->fillTriangle(bx - 1, y + 5, bx + 4, y + 5, bx - 3, y + 10, bolt);
  } else {
    int fw = (w - 4) * pc / 100;
    if (fw > 0) gfx->fillRect(x + 2, y + 2, fw, h - 4, col);
  }
}

void drawHeader(const char *name, uint16_t nameColor, const char *msg) {
  drawBattery();
  gfx->setTextColor(nameColor);
  setSize(3);
  setCur(centerX(name, 3), 52);
  printT(name);
  gfx->setTextColor(inkColor());
  setSize(2);
  setCur(centerX(msg, 2), 90);
  printT(msg);
}

// animacion de la ceremonia (10s): despedida = reverencia con corazones y se
// aleja caminando; escapada = se asusta y sale corriendo. Sustituye al idle.
void drawCeremony() {
  if (!pmd.loaded) { drawPet(); return; }  // respaldo si no hay sprite PMD
  uint32_t now = millis();
  float t = pet.ceremonyT();               // 0..1 a lo largo de los 10s
  bool panic = (pet.ceremony == CER_RUNAWAY);
  int x = CX, y = PET_GROUND;
  uint8_t act = PMD_IDLE;

  if (panic) {
    // final triste: penumbra azulada + lluvia
    for (int i = 0; i < 46; i++) {
      int rx = (i * 47 + now / 3) % LCD_WIDTH;
      int ry = (i * 91 + now / 2) % (LCD_HEIGHT + 4);
      gfx->drawLine(rx, ry, rx - 3, ry + 12, C565(0x6a, 0x84, 0xb0));
    }
    bool fade = false;
    if (t < 0.30f) {                       // cabizbajo, temblando
      act = pmd.has(PMD_HURT) ? PMD_HURT : PMD_IDLE;
      x = CX + (int)(4 * sinf(now * 0.04f));
    } else {                               // se aleja despacio y se desvanece
      act = pmd.has(PMD_WALKL) ? PMD_WALKL : PMD_IDLE;
      x = CX - (int)(((t - 0.30f) / 0.70f) * (CX + 120));
      fade = (t > 0.6f) && ((now / 160) % 2 == 0);  // parpadea hacia la silueta
    }
    drawPmdAct(act, x, y, now, true, fade, 5);  // fade=silueta: se difumina al irse
    // lagrima cayendo del bicho
    if (t < 0.55f) {
      int ty = y - 150 + (int)((now / 6) % 40);
      gfx->fillRect(x + 6, ty, 3, 6, C565(0x9a, 0xc4, 0xe8));
    }
    return;
  }

  // despedida epica: halo dorado pulsante + chispas y corazones que ascienden
  int gcy = PET_GROUND - 96;
  for (int k = 0; k < 4; k++) {
    int r = 60 + k * 34 + (int)(10 * sinf(now * 0.02f));
    gfx->drawCircle(CX, gcy, r, C565(0xff, 0xdf, 0x8a));
  }
  for (int i = 0; i < 16; i++) {
    int px = (i * 71 + 28) % LCD_WIDTH;
    int py = 410 - (int)((now / 8 + i * 70) % 360);   // suben y reaparecen abajo
    if (py < 30) continue;
    if (i % 4 == 0) drawMap(SPR_HEART, 32, px - 8, py - 8, 1, false);  // corazoncito
    else gfx->fillRect(px, py, 4, 4, (i % 2) ? C565(0xff, 0xe7, 0x9f) : C565(0xff, 0x9a, 0xc0));
  }

  if (t < 0.45f) {                         // reverencia / pose de despedida
    act = pmd.has(PMD_POSE) ? PMD_POSE : (pmd.has(PMD_NOD) ? PMD_NOD : PMD_IDLE);
  } else {                                 // se aleja por la derecha
    act = pmd.has(PMD_WALKR) ? PMD_WALKR : PMD_IDLE;
    x = CX + (int)(((t - 0.45f) / 0.55f) * (CX + 140));
  }
  drawPmdAct(act, x, y, now, true, false, 5);
  if (pet.showHeart())                     // corazon grande siguiendo al bicho
    drawMap(SPR_HEART, 32, x + 50, y - 190, 2, false);
}

// dialogo de decision (2 botones apilados): evolucionar/mantener o despedirse/quedaros
void drawChoiceDialog() {
  const char *q, *o1, *o2;
  uint16_t c1, c2, t1, t2;
  if (choiceKind == 1) {  // evolucion
    q = T(S_EVO_Q); o1 = T(S_EVO_TAP); o2 = T(S_EVO_KEEP);
    c1 = UI_BAR_BAD; t1 = UI_WHITE; c2 = UI_TRACK; t2 = UI_INK;
  } else {                // despedida
    q = T(S_FAR_Q); o1 = T(S_FAR_GO); o2 = T(S_FAR_STAY);
    c1 = UI_BAR_WARN; t1 = UI_INK; c2 = UI_BAR_OK; t2 = UI_WHITE;
  }
  gfx->fillRoundRect(73 + UI_DX, 156, 320, 188, 16, UI_WHITE);
  gfx->drawRoundRect(73 + UI_DX, 156, 320, 188, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(centerX(q, 2), 176);
  printT(q);
  gfx->fillRoundRect(93 + UI_DX, 206, 280, 52, 12, c1);     // boton accion
  gfx->setTextColor(t1);
  setCur(centerX(o1, 2), 224);
  printT(o1);
  gfx->fillRoundRect(93 + UI_DX, 268, 280, 52, 12, c2);     // boton mantener/quedaros
  gfx->setTextColor(t2);
  setCur(centerX(o2, 2), 286);
  printT(o2);
}

// boton-CTA rojo y grande para evolucionar (pulsa para llamar la atencion)
void drawEvolveButton() {
  uint32_t now = millis();
  int p = (int)(5 * sinf(now * 0.006f));  // late: -5..5
  int x = EVO_BTN_X - p, y = EVO_BTN_Y - p, w = EVO_BTN_W + 2 * p, h = EVO_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 18, UI_BAR_BAD);
  gfx->drawRoundRect(x, y, w, h, 18, UI_WHITE);
  gfx->drawRoundRect(x + 2, y + 2, w - 4, h - 4, 16, UI_WHITE);
  gfx->setTextColor(UI_WHITE);
  setSize(3);
  const char *t = T(S_EVO_TAP);
  setCur(centerX(t, 3), y + h / 2 - 11);
  printT(t);
}

// boton-CTA dorado de despedida: "<nombre> quiere decirte algo..."
void drawFarewellButton() {
  uint32_t now = millis();
  int p = (int)(4 * sinf(now * 0.005f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, UI_BAR_WARN);
  gfx->drawRoundRect(x, y, w, h, 16, UI_INK);
  char buf[52];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  snprintf(buf, sizeof(buf), T(S_FAREWELL_BTN), nm);
  gfx->setTextColor(UI_INK);
  setSize(2);
  setCur(centerX(buf, 2), y + h / 2 - 8);
  printT(buf);
}

// boton-CTA sombrio de escapada por abandono: "<nombre> se siente abandonado..."
// (final triste: azul-gris oscuro, latido lento y apagado)
void drawRunawayButton() {
  uint32_t now = millis();
  int p = (int)(3 * sinf(now * 0.003f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, C565(0x3a, 0x44, 0x5a));
  gfx->drawRoundRect(x, y, w, h, 16, C565(0x70, 0x80, 0x98));
  char buf[52];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  snprintf(buf, sizeof(buf), T(S_RUNAWAY_BTN), nm);
  gfx->setTextColor(C565(0xc8, 0xd2, 0xe0));
  setSize(2);
  setCur(centerX(buf, 2), y + h / 2 - 8);
  printT(buf);
}

// animacion epica de evolucion: halo radial + rayos giratorios + parpadeo del
// sprite acelerando + chispas que salen disparadas + fogonazo final
void drawEvolveFX(uint32_t now) {
  float t = pet.evolveT();          // 0..1
  int cx = CX, cy = PET_GROUND - 96;

  // halo radial que crece y pulsa
  int halo = 36 + (int)(t * 150) + (int)(8 * sinf(now * 0.02f));
  for (int k = 0; k < 4; k++) {
    int r = halo - k * 7;
    if (r > 0) gfx->drawCircle(cx, cy, r, UI_WHITE);
  }
  // rayos giratorios desde el centro del bicho
  float base = now * 0.004f;
  for (int i = 0; i < 12; i++) {
    float a = base + i * (float)(PI / 6);
    int len = 90 + (int)(70 * (0.5f + 0.5f * sinf(now * 0.012f + i)));
    gfx->drawLine(cx, cy, cx + (int)(cosf(a) * len), cy + (int)(sinf(a) * len), UI_WHITE);
  }
  // parpadeo entre la forma ANTERIOR y la NUEVA (siluetas), acelerando; al
  // final (t>0.9) se queda fija en la nueva para el fogonazo de revelado
  int period = 60 + (int)(220 * (1.0f - t));
  bool showOld = t < 0.9f && evoPmd.loaded && ((now / period) % 2) == 0;
  if (showOld) drawPmdActM(evoPmd, PMD_IDLE, cx, PET_GROUND, 0, true, true, 5);
  else drawPmdAct(PMD_IDLE, cx, PET_GROUND, 0, true, true, 5);
  // chispas que salen disparadas
  for (int i = 0; i < 10; i++) {
    float a = i * (float)(PI / 5) + t * 4.0f;
    int d = (int)((now / 14 + i * 33) % 200);
    int sx = cx + (int)(cosf(a) * d), sy = cy + (int)(sinf(a) * d);
    gfx->fillRect(sx - 2, sy - 2, 5, 5, (i & 1) ? C565(0xff, 0xe0, 0x70) : UI_WHITE);
  }
  // fogonazo final antes de revelar la forma nueva
  if (t > 0.9f) gfx->fillCircle(cx, cy, (int)(300 * (t - 0.9f) / 0.1f), UI_WHITE);
}

void drawPet() {
  if (pmd.loaded) {
    drawPetPMD();
    return;
  }
  if (mon.loaded) {
    drawPetSD();
    return;
  }
  int fi = flashIdxForDex(pet.speciesId);
  if (fi < 0) {
    // sin SD y sin sprite de flash: aviso claro de que faltan sprites
    gfx->setTextColor(inkColor());
    setSize(6);
    setCur(CX - 18, PET_CY - 80);
    printT("?");
    setSize(2);
    const char *l1 = T(S_NO_SPRITES);
    setCur(centerX(l1, 2), PET_CY - 4);
    printT(l1);
    const char *l2 = T(S_LOAD_SPRITES);
    setCur(centerX(l2, 2), PET_CY + 20);
    printT(l2);
    return;
  }
  const Species &sp = SPECIES[fi];
  int s = sp.scale;
  int x = CX - 16 * s;
  int y = PET_CY - 16 * s;

  // animacion de evolucion: alterna la silueta de la forma anterior y la nueva
  if (pet.evolving()) {
    bool flash = (millis() / 300) % 2;
    int16_t showDex = (flash && pet.prevSpeciesId >= 0) ? pet.prevSpeciesId : pet.speciesId;
    int sfi = flashIdxForDex(showDex);
    if (sfi >= 0) {
      const Species &show = SPECIES[sfi];
      drawMap(show.sprite, SPRITE_H, CX - 16 * show.scale, PET_CY - 16 * show.scale, show.scale, flash);
    }
    return;
  }

  PetMood m = pet.mood();
  if (m == MOOD_HAPPY && (millis() / 500) % 2) y -= 6;  // saltito

  drawMap(sp.sprite, SPRITE_H, x, y, s, false);

  // expresiones superpuestas usando las anclas de la especie
  bool blink = (millis() % 3500 < 300);
  if (m == MOOD_SLEEPING || blink) {
    overlayEye(sp, x, y, s, sp.eyeColL);
    overlayEye(sp, x, y, s, sp.eyeColR);
  }
  if (m == MOOD_EATING) overlayMouth(sp, x, y, s, true);
  else if (m == MOOD_SAD) overlayMouth(sp, x, y, s, false);

  if (pet.showHeart()) drawMap(SPR_HEART, 32, x + 20 * s, y - 2 * s, 2, false);
}

// ---------- escena de bano ----------

void startBath() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony || bathUntil) return;
  bathUntil = millis() + 3000;
  bathPending = true;
  int cx = (int)beh.x;
  for (auto &b : bubbles) {
    b.x = cx - 70 + random(140);
    b.y = PET_GROUND - random(150);
    b.r = 8 + random(16);
    b.ph = random(64);
  }
}

void drawBath() {
  uint32_t now = millis();
  if (!timeLeft(bathUntil)) {
    bathUntil = 0;
    if (bathPending) {
      bathPending = false;
      pet.clean();
      // pose de alegria al quedar limpio
      if (pmd.has(PMD_POSE)) {
        beh.mode = 2;
        beh.act = PMD_POSE;
        beh.t0 = now;
        beh.until = now + pmdActTotalMs(pmd.acts[PMD_POSE]) * 2;
      }
    }
    return;
  }
  uint32_t left = bathUntil - now;
  if (left > 800) {
#if DROID_PREVIEW
    // oil bath: amber drops run down over the droid and pool at its feet
    uint16_t oil = C565(0xd8, 0x8a, 0x2a), oilHi = C565(0xff, 0xd0, 0x80), oilDk = C565(0x8a, 0x52, 0x18);
    uint32_t el = 3000 - left;
    for (auto &b : bubbles) {
      int bx = b.x + (b.ph % 5) - 2;
      int by = (b.y - 70) + (int)((el + b.ph * 37) % 900) / 5;  // falling, staggered
      int r = 3 + b.r / 6;
      if (by > PET_GROUND - 8) continue;                         // landed in the puddle
      gfx->fillCircle(bx, by, r, oil);
      gfx->fillRect(bx - 1, by - r - 4, 3, 5, oil);              // drip tail
      gfx->fillRect(bx - r / 2, by - r / 2, 2, 2, oilHi);        // shine
    }
    int pw = 30 + (int)(el / 40);                                 // puddle grows
    if (pw > 100) pw = 100;
    int px = (int)beh.x;
    gfx->fillRoundRect(px - pw, PET_GROUND - 6, pw * 2, 10, 5, oilDk);
    gfx->fillRoundRect(px - pw + 6, PET_GROUND - 5, pw, 3, 2, oil);
#else
    // espuma: pompas meciendose y subiendo poco a poco
    float t = now / 220.0f;
    for (auto &b : bubbles) {
      int bx = b.x + (int)(sinf(t + b.ph) * 6);
      int by = b.y - (int)((3000 - left) / 90);
      gfx->fillCircle(bx, by, b.r, UI_WHITE);
      gfx->drawCircle(bx, by, b.r, 0x7E3D);
      gfx->fillCircle(bx - b.r / 3, by - b.r / 3, b.r / 4, UI_BG_DAY);
    }
#endif
  } else {
    // las pompas revientan: destellos
    for (int i = 0; i < 8; i++) {
      auto &b = bubbles[i];
      int sx = b.x + (i % 3) * 6 - 6, sy = b.y - 18;
      uint16_t col = (i % 2) ? UI_BAR_WARN : UI_WHITE;
      gfx->fillRect(sx - 6, sy - 1, 13, 3, col);
      gfx->fillRect(sx - 1, sy - 6, 3, 13, col);
    }
  }
}

// ---------- mascota PMD: comportamiento ----------

uint32_t pmdActTotalMs(const PmdAct &a) {
  uint32_t t = 0;
  for (uint8_t i = 0; i < a.frames; i++) t += a.ms[i];
  return t ? t : 100;
}

uint8_t pmdFrameAt(const PmdAct &a, uint32_t t, bool loop) {
  uint32_t total = pmdActTotalMs(a);
  if (!loop && t >= total) return a.frames - 1;
  t %= total;
  uint8_t i = 0;
  // guarda: nunca mas vueltas que frames, por si un .bin corrupto trae ms[]=0
  // pese al saneo de la carga (defensa en profundidad)
  for (uint8_t guard = 0; guard < a.frames && t >= a.ms[i]; guard++) {
    t -= a.ms[i];
    i = (i + 1) % a.frames;
  }
  return i;
}

// dibuja una accion anclada por la base (centro-x, suelo) y devuelve su escala
// dibuja una accion de un PmdMon concreto (m); drawPmdAct usa el global pmd
void drawPmdActM(PmdMon &m, uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS) {
  const PmdAct &a = m.acts[actId];
  if (!a.frames) return;
  uint8_t sBase = m.acts[PMD_IDLE].h ? 170 / m.acts[PMD_IDLE].h : 5;
  if (sBase < 2) sBase = 2;
  if (sBase > maxS) sBase = maxS;
  uint8_t s = sBase;
  while (s > 2 && a.h * s > 250) s--;  // acciones con frame grande (ataque)
  uint8_t fi = pmdFrameAt(a, t, loop);
  const uint8_t *fr = a.data + (uint32_t)fi * a.w * a.h;
  // anclar por los pies (a.base), no por el alto del lienzo: asi las acciones
  // con padding distinto (Hurt, Eat...) quedan todas a la misma altura de suelo
  int x0 = cx - a.w * s / 2, y0 = groundY - (a.base ? a.base : a.h) * s;
  for (int r = 0; r < a.h; r++) {
    const uint8_t *row = fr + r * a.w;
    forRuns(a.w, [&](int c) { return indexedKey(row[c], sil); },
            [&](int c0, int len, int k) { gfx->fillRect(x0 + c0 * s, y0 + r * s, len * s, s, sil ? INK_K : m.pal[k]); });
  }
}
void drawPmdAct(uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS) {
  drawPmdActM(pmd, actId, cx, groundY, t, loop, sil, maxS);
}

// elige el siguiente capricho del bicho cuando esta contento
void behNext() {
  uint32_t now = millis();
  beh.t0 = now;
  int r = random(100);
  if (r < 35 && (pmd.has(PMD_WALKL) || pmd.has(PMD_WALKR))) {
    beh.mode = 1;  // paseo
    beh.targetX = 150 + UI_DX + random(176);
    beh.until = now + 15000;
  } else if (r < 60) {
    // gesto aleatorio entre los disponibles
    // (Hop fuera: salta demasiado alto; Sit fuera: mira hacia atras)
    static const uint8_t flair[] = { PMD_POSE, PMD_NOD, PMD_BREATH };
    uint8_t pick[3], n = 0;
    for (uint8_t f : flair)
      if (pmd.has(f)) pick[n++] = f;
    if (n) {
      beh.mode = 2;
      beh.act = pick[random(n)];
      beh.until = now + pmdActTotalMs(pmd.acts[beh.act]);
      return;
    }
    beh.mode = 0;
    beh.until = now + 2000 + random(3000);
  } else {
    beh.mode = 0;  // mirar al frente
    beh.until = now + 2000 + random(3000);
  }
}

void drawPetPMD() {
  uint32_t now = millis();

  if (pet.evolving()) {
    drawEvolveFX(now);
    return;
  }
  if (evoPmd.loaded) evoPmd.unload();  // termino la evolucion: libera la forma anterior

  PetMood m = pet.mood();
  uint8_t act;
  bool loop = true;
  if (m == MOOD_SLEEPING && pmd.has(PMD_SLEEP)) {
    act = PMD_SLEEP;
    beh.mode = 0;
  } else if (m == MOOD_EATING && pmd.has(PMD_EAT)) {
    act = PMD_EAT;
    beh.t0 = 0;
  } else if (m == MOOD_SAD && pmd.has(PMD_HURT)) {
    act = PMD_HURT;
  } else {
    // contento: el planificador decide (idle / paseo / gesto)
    if (now > beh.until) behNext();
    if (beh.mode != 1) walkLast = 0;  // al volver a caminar no se cuenta el tiempo parado
    if (beh.mode == 1) {
      float d = beh.targetX - beh.x;
      if (fabsf(d) < 4) {
        behNext();
        act = PMD_IDLE;
      } else {
        // 30 px/s de diseno (3 px por frame a 10 fps), midiendo el tiempo real: con menos frames
        // por segundo camina igual de rapido
        uint32_t dt = walkLast ? now - walkLast : 100;
        uint32_t cap = frameIntervalMs(powerCtx(now)) + 100;  // un hueco mayor (otra pantalla de por medio) no es un paso largo
        if (dt > cap) dt = cap;
        walkLast = now;
        float step = 30.0f * dt / 1000.0f;
        if (step > fabsf(d)) step = fabsf(d);  // un frame lento no sobrepasa el destino
        beh.x += d > 0 ? step : -step;
        act = (d > 0) ? PMD_WALKR : PMD_WALKL;
      }
    } else {
      act = (beh.mode == 2) ? beh.act : PMD_IDLE;
      loop = false;
    }
    if (!pmd.has(act)) act = PMD_IDLE;
  }

  drawPmdAct(act, (int)beh.x, PET_GROUND, now - beh.t0, loop || act == PMD_IDLE, false, 5);

  if (pet.showHeart()) drawMap(SPR_HEART, 32, (int)beh.x + 50, PET_GROUND - 190, 2, false);
}

// sprite animado desde la SD: zoom entero por pixel, frames a su ritmo
void drawPetSD() {
  int s = mon.scale;
  int w = mon.w * s, h = mon.h * s;
  int x = CX - w / 2;
  int y = PET_CY - h / 2;

  bool sil = false;
  if (pet.evolving()) {
    sil = (millis() / 300) % 2;
  } else if (pet.mood() == MOOD_HAPPY && (millis() / 500) % 2) {
    y -= 6;  // saltito
  }

  uint16_t fm = mon.frameMs ? mon.frameMs : 100;
  uint16_t fi = pet.sleeping ? 0 : (millis() / fm) % mon.frames;
  const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
  for (int r = 0; r < mon.h; r++) {
    const uint8_t *row = fr + r * mon.w;
    forRuns(mon.w, [&](int c) { return indexedKey(row[c], sil); },
            [&](int c0, int len, int k) { gfx->fillRect(x + c0 * s, y + r * s, len * s, s, sil ? INK_K : mon.pal[k]); });
  }

  // emotes en vez de expresiones (los sprites importados no tienen anclas)
  if (pet.showHeart()) drawMap(SPR_HEART, 32, x + w - 30, y - 50, 2, false);
}

// ojo cerrado: borra el ojo 3x4 y dibuja el parpado
void overlayEye(const Species &sp, int x, int y, int s, int col) {
  gfx->fillRect(x + col * s, y + sp.eyeRow * s, 3 * s, 4 * s, sp.bodyColor);
  gfx->fillRect(x + col * s, y + (sp.eyeRow + 2) * s, 3 * s, s, INK_K);
}

// borra la sonrisa base y pinta boca abierta (comer) o ceno (triste)
void overlayMouth(const Species &sp, int x, int y, int s, bool open) {
  int mc = sp.mouthCol, mr = sp.mouthRow;
  gfx->fillRect(x + (mc - 3) * s, y + mr * s, 7 * s, 2 * s, sp.bodyColor);
  if (open) {
    gfx->fillRect(x + (mc - 2) * s, y + mr * s, 5 * s, 2 * s, INK_K);
  } else {
    gfx->fillRect(x + (mc - 2) * s, y + mr * s, 5 * s, s, INK_K);
    gfx->fillRect(x + (mc - 3) * s, y + (mr + 1) * s, s, s, INK_K);
    gfx->fillRect(x + (mc + 3) * s, y + (mr + 1) * s, s, s, INK_K);
  }
}

void drawPoops() {
  for (int i = 0; i < pet.poops; i++) {
#if DROID_PREVIEW
    drawMap(SPR_OIL_PUDDLE, 32, 36 + i * 46, 244, 2, false);
#else
    drawMap(SPR_POOP, 32, 36 + i * 46, 244, 2, false);
#endif
  }
}

#if DROID_PREVIEW
// TamaDroid: an icon in front of each bar instead of a text label
void drawIconBar(int x, int y, const char *const *icon, uint8_t val) {
  drawMap(icon, 8, x, y, 2, false);  // 16x16 px, same height as the bar
  const int bx = x + 24, bw = 118, bh = 15;
  uint16_t fill = (val >= 50) ? UI_BAR_OK : (val >= 25) ? UI_BAR_WARN : UI_BAR_BAD;
  gfx->fillRoundRect(bx, y, bw, bh, 4, UI_TRACK);
  int fw = (bw - 4) * val / 100;
  if (fw > 0) gfx->fillRoundRect(bx + 2, y + 2, fw, bh - 4, 3, fill);
}

// set DROID_BAR_ICONS to 1 for icons in front of the bars instead of text labels
#ifndef DROID_BAR_ICONS
#define DROID_BAR_ICONS 0
#endif
void drawBar(int x, int y, const char *label, uint8_t val);
void drawBars() {
#if DROID_BAR_ICONS
  drawIconBar(78 + UI_DX, 318, SPR_BAR_CHARGE, pet.fullness);
  drawIconBar(244 + UI_DX, 318, SPR_BAR_MORALE, pet.joy);
  drawIconBar(78 + UI_DX, 346, SPR_BAR_ENERGY, pet.energy);
  drawIconBar(244 + UI_DX, 346, SPR_BAR_MAINT, pet.hygiene);
#else
  drawBar(78 + UI_DX, 318, T(S_BAR_FOOD), pet.fullness);
  drawBar(244 + UI_DX, 318, T(S_BAR_JOY), pet.joy);
  drawBar(78 + UI_DX, 346, T(S_BAR_ENE), pet.energy);
  drawBar(244 + UI_DX, 346, T(S_BAR_HYG), pet.hygiene);
#endif
}
#else
void drawBars() {
  drawBar(78 + UI_DX, 318, T(S_BAR_FOOD), pet.fullness);
  drawBar(244 + UI_DX, 318, T(S_BAR_JOY), pet.joy);
  drawBar(78 + UI_DX, 346, T(S_BAR_ENE), pet.energy);
  drawBar(244 + UI_DX, 346, T(S_BAR_HYG), pet.hygiene);
}
#endif

// Separacion entre la etiqueta y su barra en la fila de necesidades. Estaba fija
// en 48 px, justo lo que ocupan 4 letras latinas a escala 2, sin margen. Las
// etiquetas japonesas son mas anchas (ごきげん son 4 kana, ~64 px) y se metian
// dentro de la barra. Se deriva de la etiqueta mas larga, con el 48 de suelo
// para que en los idiomas latinos no cambie nada.
// Requiere el tamano de texto 2 ya puesto, porque textW() lo necesita.
static int barLabelGap() {
  const StrId ids[] = { S_BAR_FOOD, S_BAR_JOY, S_BAR_ENE, S_BAR_HYG };
  int ancho = 0;
  for (StrId id : ids) {
    int w = textW(T(id), 2);
    if (w > ancho) ancho = w;
  }
  // por debajo del umbral original se deja EXACTAMENTE 48, para no mover nada en
  // los idiomas latinos; por encima se anade aire para que la etiqueta respire
  return ancho <= 48 ? 48 : ancho + 8;
}

void drawBar(int x, int y, const char *label, uint8_t val) {
  gfx->setTextColor(inkColor());
  setSize(2);
  setCur(x, y);
  printT(label);
  int gap = barLabelGap();
  // las dos columnas empiezan en 78 y 244: la barra de la primera tiene que
  // acabar antes de la etiqueta de la segunda, y ambas se pintan igual de anchas
  int bw = 232 - (78 + gap);
  if (bw > 100) bw = 100;   // con etiquetas latinas sale 100, como estaba
  int bx = x + gap, bh = 15;
  uint16_t fill = (val >= 50) ? UI_BAR_OK : (val >= 25) ? UI_BAR_WARN : UI_BAR_BAD;
  gfx->fillRoundRect(bx, y, bw, bh, 4, UI_TRACK);
  int fw = (bw - 4) * val / 100;
  if (fw > 0) gfx->fillRoundRect(bx + 2, y + 2, fw, bh - 4, 3, fill);
}

void drawButtons() {
  for (int i = 0; i < 4; i++) {
    bool off = pet.sleeping && i != 2;  // durmiendo solo funciona LUZ
    int bx = buttons[i].cx - BTN_HALF, by = buttons[i].cy - BTN_HALF;
    // durmiendo, con suelo de brillo (Guition) LUZ conserva su relleno claro: es el unico modo de despertar
    if (!pet.sleeping || (i == 2 && BRIGHTNESS_FLOOR > 0)) gfx->fillRoundRect(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, 14, UI_WHITE);
    gfx->drawRoundRect(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, 14, inkColor());
    if (!off) drawMap(buttons[i].icon, 16, buttons[i].cx - 16, buttons[i].cy - 16, 2, false);
  }
}

const char *eggMsg() {
  switch (pet.eggCracks()) {
    case 0: return T(S_EGG_TOUCH);
    case 1: return T(S_EGG_MOVES);
    default: return T(S_EGG_ALMOST);
  }
}

const char *statusMsg() {
#if DROID_PREVIEW
  if (pet.onMission()) return "Out on a mission";
#endif
  if (pet.evolving()) return T(S_EVOLVING);
  if (bathUntil) return "Splish splash!";  // onomatopeya universal
  if (pet.sleeping) return "Zzz...";
  if (pet.eating()) return T(S_EATING);
  if (pet.showHeart()) return T(S_LIKES);
  if (pet.fullness < 25) return T(S_HUNGRY);
  if (pet.hygiene < 25) return T(S_NEEDS_BATH);
  if (pet.energy < 25) return T(S_EXHAUSTED);
  if (pet.joy < 25) return T(S_SAD);
  if (pet.weight > 60) return T(S_CHUBBY);
  if (pet.shiny && pet.ageMinutes < 15) return T(S_IS_SHINY);
  return T(S_HAPPY);
}

// dibuja un mapa de n x n pixeles escalado; silhouette=true lo pinta en tinta
void drawMap(const char *const *map, int n, int x, int y, int s, bool silhouette) {
  for (int r = 0; r < n; r++) {
    const char *row = map[r];
    forRuns(n, [&](int c) { return charKey(row[c], silhouette); },
            [&](int c0, int len, int k) { gfx->fillRect(x + c0 * s, y + r * s, len * s, s, silhouette ? INK_K : spriteColor((char)k)); });
  }
}
