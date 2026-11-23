#pragma once
#include <stdint.h>

// Bateria de una celda LiPo/Li-ion (cargada a 4,2 V, como configura el ejemplo de Waveshare): del
// voltaje en reposo al porcentaje, y cuando apagar para no descargarla a fondo. Logica pura, con
// tests en test/test_batcurve.cpp.

#define BAT_CUTOFF_MV 3300     // por debajo de esto (en descarga) el firmware guarda y apaga
#define BAT_CUTOFF_READS 3     // lecturas seguidas por debajo (ademas del tiempo, ver BatMonitor)
#define BAT_HW_CUTOFF_MV 3100  // respaldo en el PMU (AXP2101, de 2600 a 3300 en pasos de 100)

// curva de descarga tipica de una celda LiPo, (mV, %) de mayor a menor
struct BatPoint { int16_t mv; uint8_t pct; };
static const BatPoint BAT_CURVE[] = {
  { 4200, 100 }, { 4150, 95 }, { 4110, 90 }, { 4080, 85 }, { 4020, 80 }, { 3980, 75 }, { 3950, 70 },
  { 3910, 65 }, { 3870, 60 }, { 3850, 55 }, { 3840, 50 }, { 3820, 45 }, { 3800, 40 }, { 3790, 35 },
  { 3770, 30 }, { 3750, 25 }, { 3730, 20 }, { 3710, 15 }, { 3690, 10 }, { 3610, 5 }, { BAT_CUTOFF_MV, 0 },
};

// interpolacion lineal entre puntos; 100 por encima de 4,2 V y 0 en el apagado
inline int batPercentFromMv(int mv) {
  const int n = sizeof(BAT_CURVE) / sizeof(BAT_CURVE[0]);
  if (mv >= BAT_CURVE[0].mv) return 100;
  if (mv <= BAT_CURVE[n - 1].mv) return 0;
  for (int i = 1; i < n; i++) {
    if (mv >= BAT_CURVE[i].mv) {
      int dmv = BAT_CURVE[i - 1].mv - BAT_CURVE[i].mv, dp = BAT_CURVE[i - 1].pct - BAT_CURVE[i].pct;
      return BAT_CURVE[i].pct + (mv - BAT_CURVE[i].mv) * dp / dmv;
    }
  }
  return 0;
}

// SpotPear 1.28 AI Ball (no PMU): raw 12-bit ADC on GPIO1 at 12 dB, through a
// divider whose ratio isn't documented. Table and interpolation copied from
// XiaoZhi (78/xiaozhi-esp32 boards/spotpear/sp-esp32-s3-1.28-box/power_manager.h).
struct BatRawPoint { int16_t raw; uint8_t pct; };
static const BatRawPoint BALL_BAT_RAW[] = {
  { 1980, 0 }, { 2081, 20 }, { 2163, 40 }, { 2250, 60 }, { 2340, 80 }, { 2480, 100 },
};
inline int ballBatPercentFromRaw(int raw) {
  const int n = sizeof(BALL_BAT_RAW) / sizeof(BALL_BAT_RAW[0]);
  if (raw <= BALL_BAT_RAW[0].raw) return 0;
  if (raw >= BALL_BAT_RAW[n - 1].raw) return 100;
  for (int i = 1; i < n; i++) {
    if (raw < BALL_BAT_RAW[i].raw) {
      int dr = BALL_BAT_RAW[i].raw - BALL_BAT_RAW[i - 1].raw, dp = BALL_BAT_RAW[i].pct - BALL_BAT_RAW[i - 1].pct;
      return BALL_BAT_RAW[i - 1].pct + (raw - BALL_BAT_RAW[i - 1].raw) * dp / dr;
    }
  }
  return 100;
}

// Suaviza la tension (media movil exponencial, 1/4 por lectura), hace que el porcentaje mostrado solo
// baje mientras dura la descarga (el pico de carga justo tras desenchufar, o un bache por carga, no lo
// suben ni lo hacen bailar) y decide el apagado: la tension suavizada tiene que estar bajo el limite
// durante BAT_CUTOFF_MS seguidos (y al menos BAT_CUTOFF_READS lecturas), no solo unas lecturas.
#define BAT_CUTOFF_MS 30000UL
#define BAT_BOOT_GRACE_MS 30000UL  // tras arrancar no se apaga: la primera tension aun no dice nada

struct BatMonitor {
  int smoothMv = 0;
  int pct = -1;            // ultimo porcentaje mostrado (-1 = aun sin lectura)
  uint32_t lowSince = 0;
  uint8_t lowReads = 0;

  void reset() { smoothMv = 0; pct = -1; lowSince = 0; lowReads = 0; }
  // limite superior del porcentaje al empezar la descarga (el que mostraba el PMU)
  void seed(int p) { if (pct < 0 && p >= 0) pct = p; }
  // Una lectura en descarga (sin USB ni carga). Devuelve el porcentaje, -1 si no hubo lectura.
  int sample(int mv, uint32_t now) {
    if (mv <= 0) { lowReads = 0; lowSince = 0; return -1; }
    smoothMv = smoothMv ? smoothMv + (mv - smoothMv) / 4 : mv;
    if (smoothMv < BAT_CUTOFF_MV) {
      if (!lowReads) lowSince = now ? now : 1;
      if (lowReads < 255) lowReads++;
    } else {
      lowReads = 0;
      lowSince = 0;
    }
    int c = batPercentFromMv(smoothMv);
    if (pct < 0 || c < pct) pct = c;
    return pct;
  }
  bool shutdownNeeded(uint32_t now) const {
    return now >= BAT_BOOT_GRACE_MS && lowReads >= BAT_CUTOFF_READS && now - lowSince >= BAT_CUTOFF_MS;
  }
};

// Waveshare ESP32-S3-Touch-AMOLED-1.43: no PMU, the charger's STAT pin only lights an LED and
// there is no VBUS sense, so "on charge" is inferred from the battery voltage. Plugging a
// charger in lifts a LiPo's terminal voltage by 100 mV or more within seconds; a lighter screen
// load or light sleep gives back only a few tens of mV. Unplugging drops it again by a similar
// amount. Pure logic, tests in test/test_batrise.cpp.
#define BAT_RISE_MV 80               // rise above the recent minimum that counts as "on charge"
#define BAT_FALL_MV 60               // drop below the peak since then that counts as "unplugged"
#define BAT_RISE_WINDOW_MS 180000UL  // the minimum is tracked over about 3 minutes
#define BAT_ABSENT_MV 2500           // below this the ADC sees no cell at all

struct BatRiseDetector {
  int smooth = 0, minMv = 0, peakMv = 0;
  uint32_t minT = 0;
  bool on = false;

  void reset() { smooth = minMv = peakMv = 0; minT = 0; on = false; }
  int smoothMv() const { return smooth; }
  // one reading (mV); returns true while the cell looks like it is on charge
  bool sample(int mv, uint32_t now) {
    if (mv <= 0) return on;
    smooth = smooth ? smooth + (mv - smooth) / 2 : mv;  // faster than BatMonitor: a plug-in is a step
    if (!on) {
      if (!minMv || smooth < minMv || now - minT > BAT_RISE_WINDOW_MS) { minMv = smooth; minT = now; }
      if (smooth - minMv >= BAT_RISE_MV) { on = true; peakMv = smooth; }
    } else {
      if (smooth > peakMv) peakMv = smooth;
      if (peakMv - smooth >= BAT_FALL_MV) { on = false; minMv = smooth; minT = now; }
    }
    return on;
  }
};
