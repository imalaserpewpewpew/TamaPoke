// Waveshare 1.43: charging inferred from the battery voltage (BatRiseDetector, batcurve.h)
// and the deep-sleep policy (deepSleepDue, power.h).
#include "framework.h"
#include "../batcurve.h"
#include "../power.h"

// feeds the same voltage every 2 s for `ms`; returns the detector state at the end
static bool feed(BatRiseDetector &d, int mv, uint32_t &t, uint32_t ms) {
  bool r = d.on;
  for (uint32_t end = t + ms; t < end; t += 2000) r = d.sample(mv, t);
  return r;
}

TEST(batrise, steady_discharge_is_not_charging) {
  BatRiseDetector d;
  uint32_t t = 0;
  for (int mv = 4100; mv > 3600; mv -= 2) CHECK(!d.sample(mv, t += 2000));
}

TEST(batrise, screen_dimming_bounce_is_not_charging) {
  BatRiseDetector d;
  uint32_t t = 0;
  CHECK(!feed(d, 3800, t, 60000));
  CHECK(!feed(d, 3840, t, 60000));  // lighter load: the cell gives back ~40 mV
  CHECK(!feed(d, 3800, t, 60000));
}

TEST(batrise, plugging_a_charger_is_seen_within_seconds) {
  BatRiseDetector d;
  uint32_t t = 0;
  CHECK(!feed(d, 3750, t, 60000));
  CHECK(feed(d, 3920, t, 10000));  // +170 mV on charge
  CHECK(d.on);
}

TEST(batrise, still_charging_while_the_voltage_climbs_and_off_after_unplugging) {
  BatRiseDetector d;
  uint32_t t = 0;
  feed(d, 3750, t, 60000);
  feed(d, 3920, t, 10000);
  for (int mv = 3920; mv <= 4180; mv += 5) CHECK(d.sample(mv, t += 2000));  // CC/CV climb
  CHECK(feed(d, 4200, t, 600000));   // full and terminated: stays "on" (no drop)
  CHECK(!feed(d, 4110, t, 10000));   // unplugged: the voltage sags under load
}

TEST(batrise, a_slow_drift_up_over_minutes_does_not_latch) {
  BatRiseDetector d;
  uint32_t t = 0;
  // +60 mV spread over 10 min (cell recovering after a heavy session): under the threshold
  for (int mv = 3700; mv <= 3760; mv += 1) CHECK(!d.sample(mv, t += 10000));
}

TEST(batrise, zero_readings_are_ignored) {
  BatRiseDetector d;
  uint32_t t = 0;
  feed(d, 3800, t, 20000);
  CHECK(!d.sample(0, t += 2000));
  CHECK_EQ(d.smoothMv(), 3800);
}

static PowerCtx sleepCtx(uint32_t idle, bool usb, bool game) {
  PowerCtx c = {};
  c.idleMs = idle;
  c.usb = usb;
  c.gameOpen = game;
  c.batPct = 70;
  return c;
}

TEST(batrise, deep_sleep_only_after_the_idle_time_on_battery_and_outside_games) {
  CHECK(!deepSleepDue(sleepCtx(POWER_DIM2_MS, false, false)));          // screen nearly off: not yet
  CHECK(!deepSleepDue(sleepCtx(POWER_DEEPSLEEP_MS, false, false)));     // exactly at the limit: not yet
  CHECK(deepSleepDue(sleepCtx(POWER_DEEPSLEEP_MS + 1, false, false)));
  CHECK(!deepSleepDue(sleepCtx(POWER_DEEPSLEEP_MS + 1, true, false)));  // USB host: stay up
  CHECK(!deepSleepDue(sleepCtx(POWER_DEEPSLEEP_MS + 1, false, true)));  // minigame open
  CHECK(POWER_DEEPSLEEP_MS > POWER_DIM2_MS);  // the screen always dims first
}
