#pragma once
// TamaDroid preview build: astromech = dex 1-3, BB unit = 4-6, Gonk = 7-9;
// each salvage crate holds one of the three at random. Its three restoration stages
// reuse the existing evolution chain 1 -> 2 -> 3 (levels 16 and 32).
// Set DROID_PREVIEW=0 to get the original TamaPoke behaviour (the host tests do).
#ifndef DROID_PREVIEW
#define DROID_PREVIEW 1
#endif
#define DROID_FIRST_DEX 1  // astromech: dex 1-3
#define DROID_BB_DEX 4     // BB unit: dex 4-6
#define DROID_GONK_DEX 7   // Gonk power droid: dex 7-9
