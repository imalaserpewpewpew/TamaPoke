// TamaDroid salvage missions (pet.cpp).
#include "framework.h"
#include "shim/Arduino.h"
#include "../pet.h"
#include "../dex.h"

static void advanceM(Pet &p, uint32_t minutes) {
  for (uint32_t i = 0; i < minutes; i++) {
    mockAdvanceMillis(PET_TICK_MS);
    p.update(millis());
  }
}

static void hatched(Pet &p) {
  mockNvsReset();
  mockSetMillis(0);
  mockClearForcedRandom();
  p.begin();
  if (p.awaitingStarter()) p.chooseStarter(1);
  p.eggTap(); p.eggTap(); p.eggTap();
}

TEST(mission, needs_sys_and_oil_and_awake) {
  Pet p; hatched(p);
  p.energy = 20; p.hygiene = 80;
  CHECK(!p.canStartMission(0));
  p.energy = 80; p.hygiene = 20;
  CHECK(!p.canStartMission(0));
  p.hygiene = 80;
  CHECK(p.canStartMission(0));
  p.sleeping = true;
  CHECK(!p.canStartMission(0));
}

TEST(mission, locked_jobs_need_level) {
  Pet p; hatched(p);
  p.energy = p.hygiene = 90;
  CHECK(p.level() < MISSIONS[5].minLevel);
  CHECK(!p.canStartMission(5));
  CHECK(!p.startMission(5));
  CHECK(!p.onMission());
}

TEST(mission, away_then_comes_back_with_a_report) {
  Pet p; hatched(p);
  p.energy = p.hygiene = 90;
  CHECK(p.startMission(0));
  CHECK(p.onMission());
  CHECK(!p.canStartMission(1));  // one at a time
  CHECK_EQ(p.missionLeft, (uint16_t)MISSIONS[0].minutes);
  advanceM(p, MISSIONS[0].minutes - 1);
  CHECK(p.onMission());
  advanceM(p, 1);
  CHECK(!p.onMission());
  CHECK(p.repUnseen);
  CHECK_EQ(p.repJob, (int8_t)0);
  p.seenReport();
  CHECK(!p.repUnseen);
}

TEST(mission, needs_barely_drop_and_no_slipups_while_away) {
  Pet p; hatched(p);
  p.energy = p.hygiene = 90; p.fullness = 30; p.joy = 30;
  uint8_t mistakes = p.careMistakes;
  CHECK(p.startMission(4 < MISSION_COUNT && p.level() >= MISSIONS[4].minLevel ? 4 : 1));
  advanceM(p, 18);
  CHECK(p.fullness >= 25);   // floor while away
  CHECK(p.joy >= 25);
  CHECK_EQ(p.careMistakes, mistakes);
}

TEST(mission, great_run_gives_two_parts_and_training) {
  Pet p; hatched(p);
  p.energy = p.hygiene = 90;
  uint8_t spe = p.trSpe;
  p.startMission(0);              // dune scouting: SPD, easy
  mockForceRandom(40);            // best luck
  advanceM(p, MISSIONS[0].minutes);
  mockClearForcedRandom();
  CHECK_EQ(p.repOutcome, (uint8_t)MIS_GREAT);
  CHECK_EQ(p.repParts[0], (uint8_t)PART_SENSOR);
  CHECK(p.repParts[1] != 255);
  CHECK_EQ(p.parts[PART_SENSOR], (uint8_t)1);
  CHECK(p.trSpe >= spe + 2);
}

TEST(mission, hard_job_fails_for_a_fresh_droid) {
  Pet p; hatched(p);
  p.energy = p.hygiene = 90;
  // force the job (bypassing the level lock) to check the roll itself
  p.missionJob = 5; p.missionLeft = 1;
  mockForceRandom(0);             // worst luck
  advanceM(p, 1);
  mockClearForcedRandom();
  CHECK(p.repOutcome == MIS_FAILED || p.repOutcome == MIS_ROUGH);
  CHECK(p.hygiene <= 60);         // came back grimy
}

TEST(mission, carbon_buildup_lowers_the_score) {
  Pet p; hatched(p);
  uint8_t lean = p.missionScore(0, 20);
  p.weight = 80;
  CHECK_EQ(p.missionScore(0, 20), (uint8_t)(lean - 20));
}

TEST(mission, survives_a_reboot_and_runs_offline) {
  Pet p; hatched(p);
  p.energy = p.hygiene = 90;
  p.setClock(1000000);
  p.startMission(1);   // saves
  Pet q;
  q.begin();
  CHECK(q.onMission());
  CHECK_EQ(q.missionJob, (int8_t)1);
  q.syncClock(1000000 + 60UL * (MISSIONS[1].minutes + 5));  // off for longer than the job
  CHECK(!q.onMission());
  CHECK(q.repUnseen);
}

TEST(mission, new_droid_keeps_the_parts_bin) {
  Pet p; hatched(p);
  p.parts[PART_SERVO] = 3;
  p.missionJob = 2; p.missionLeft = 10;
  p.newEgg();
  CHECK(!p.onMission());
  CHECK_EQ(p.parts[PART_SERVO], (uint8_t)3);
}

TEST(mission, successful_runs_are_counted_per_droid) {
  Pet p; hatched(p);
  p.energy = p.hygiene = 90;
  p.startMission(0);
  mockForceRandom(40);
  advanceM(p, MISSIONS[0].minutes);
  mockClearForcedRandom();
  CHECK_EQ(p.missionsDone, (uint16_t)1);
  p.newEgg();
  CHECK_EQ(p.missionsDone, (uint16_t)0);
}

TEST(feed, power_cell_charges_without_carbon) {
  Pet p; hatched(p);
  p.fullness = 40;
  uint8_t w = p.weight;
  p.feedCell();
  CHECK_EQ(p.fullness, (uint8_t)70);
  CHECK_EQ(p.weight, w);
  p.feedCandy();                 // premium oil: the treat adds carbon
  CHECK(p.weight > w);
}
