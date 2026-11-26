#pragma once
// TamaDroid UI icons (16x16, same character palette as species.h).
#include "droid.h"
#if DROID_PREVIEW

// power cell (replaces the red berry / food button); yellow charge
static const char* const SPR_ICON_CELL_Y[16] = {
  "................",
  "......kkkk......",
  "......kssk......",
  "....kkkkkkkk....",
  "....kwssssSk....",
  "....kwssssSk....",
  "....kkkkkkkk....",
  "....kfyyyyYk....",
  "....kfyyyyYk....",
  "....kfyyyyYk....",
  "....kfyyyyYk....",
  "....kkkkkkkk....",
  "....kwssssSk....",
  "....kkkkkkkk....",
  "................",
  "................",
};
static const char* const SPR_ICON_CELL_B[16] = {
  "................",
  "......kkkk......",
  "......kssk......",
  "....kkkkkkkk....",
  "....kwssssSk....",
  "....kwssssSk....",
  "....kkkkkkkk....",
  "....kwbbbbBk....",
  "....kwbbbbBk....",
  "....kwbbbbBk....",
  "....kwbbbbBk....",
  "....kkkkkkkk....",
  "....kwssssSk....",
  "....kkkkkkkk....",
  "................",
  "................",
};
static const char* const SPR_ICON_CELL_G[16] = {
  "................",
  "......kkkk......",
  "......kssk......",
  "....kkkkkkkk....",
  "....kwssssSk....",
  "....kwssssSk....",
  "....kkkkkkkk....",
  "....ktggggGk....",
  "....ktggggGk....",
  "....ktggggGk....",
  "....ktggggGk....",
  "....kkkkkkkk....",
  "....kwssssSk....",
  "....kkkkkkkk....",
  "................",
  "................",
};

// training remote (replaces the Poke Ball / play button)
static const char* const SPR_ICON_REMOTE[16] = {
  "................",
  ".....kkkkkk.....",
  "...kkwwsssskk...",
  "..kswwsssssSSk..",
  "..kswsskksssSk..",
  ".kssskrrksssSSk.",
  ".ksssskksssssSk.",
  ".kSSSSSSSSSSSSk.",
  ".kkkkkkkkkkkkkk.",
  ".ksssssskkssSSk.",
  ".kssssskrrksSSk.",
  "..kssssskkssSk..",
  "..kSsssssssSSk..",
  "...kkSSSSSSkk...",
  ".....kkkkkk.....",
  "................",
};

// oil drop (replaces the bath bubbles and the candy)
static const char* const SPR_ICON_OIL[16] = {
  "................",
  "................",
  ".......kk.......",
  "......kook......",
  "......kook......",
  ".....kwoook.....",
  ".....kwoooOk....",
  "....kwooooOOk...",
  "....kwooooOOk...",
  "....koooooOOk...",
  "....kooooOOOk...",
  ".....kooOOOk....",
  "......kkkkk.....",
  "................",
  "................",
  "................",
};


// oil puddle with a dropped bolt (replaces the poop: droids leak, they do not poop)
static const char* const SPR_OIL_PUDDLE[32] = {
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "......................kkkk......",
  ".....................kswSSk.....",
  ".....................kssSSk.....",
  "......................kkkk......",
  "...........kkkkkkkkk............",
  "........kkkMMMkkkkkkkkk.........",
  ".....kkkkMwwMkkkkkkkkkkkkk......",
  "...kkkkkkkMMkkkkkkkMMMkkkkkk....",
  "...kkkkkkkkkkkkkkkkkkkkkkkkkk...",
  ".....kkkkkkkkkkkkkkkkkkkkkk.....",
  "........kkkkkkkkkkkkkk..........",
  "................................",
  "................................",
  "................................",
};

// 8x8 icons for the need bars (drawn at 2x, same height as the old text labels)
static const char* const SPR_BAR_CHARGE[8] = {
  "........",
  "wwwwwww.",
  "wffffYw.",
  "wffffYww",
  "wffffYww",
  "wffffYw.",
  "wwwwwww.",
  "........",
};
static const char* const SPR_BAR_MORALE[8] = {
  "........",
  ".rr..rr.",
  "rwrrrrrR",
  "rrrrrrrR",
  ".rrrrrR.",
  "..rrrR..",
  "...rR...",
  "........",
};
static const char* const SPR_BAR_ENERGY[8] = {  // gear: system energy
  "...ss...",
  ".s.ws.s.",
  "..swss..",
  "sws..sss",
  "sss..sss",
  "..ssss..",
  ".s.ss.s.",
  "...ss...",
};
static const char* const SPR_BAR_MAINT[8] = {
  "...o....",
  "...o....",
  "..ooo...",
  ".owooo..",
  ".owooO..",
  ".ooooO..",
  "..oOO...",
  "........",
};

// power / standby symbol (replaces the moon on the rest button: droids power down, they do not sleep)
static const char* const SPR_ICON_STANDBY[16] = {
  "................",
  "................",
  ".......BB.......",
  "....B..BB..B....",
  "...BB..BB..BB...",
  "..BB...BB...BB..",
  "..B....BB....B..",
  ".BB....BB....BB.",
  ".BB..........BB.",
  ".BB..........BB.",
  "..B..........B..",
  "..BB........BB..",
  "...BB......BB...",
  "....BBBBBBBB....",
  "................",
  "................",
};

// cargo crate (cargo lift game)
static const char* const SPR_ICON_CRATE[16] = {
  "................",
  "................",
  "..kkkkkkkkkkkk..",
  "..kCyyyyyyyyCk..",
  "..kcCccccccCck..",
  "..kccCccccCcck..",
  "..kcccCccCccck..",
  "..kccccCCcccck..",
  "..kccccCCcccck..",
  "..kcccCccCccck..",
  "..kccCccccCcck..",
  "..kcCccccccCck..",
  "..kCccccccccCk..",
  "..kkkkkkkkkkkk..",
  "................",
  "................",
};
// circuit chip with 4 lights (circuit repair game)
static const char* const SPR_ICON_CHIP[16] = {
  "................",
  "................",
  "......s.s.s.....",
  "......s.s.s.....",
  "....kkkkkkkk....",
  "....kSSSSSSk....",
  "..sskSrrSyykss..",
  "....kSrrSyyk....",
  "..sskSSSSSSkss..",
  "....kSggSbbk....",
  "..sskSggSbbkss..",
  "....kkkkkkkk....",
  "......s.s.s.....",
  "......s.s.s.....",
  "................",
  "................",
};
// canyon run (coming soon)
static const char* const SPR_ICON_CANYON[16] = {
  "................",
  "................",
  "................",
  "................",
  "................",
  "................",
  "................",
  ".......ww.......",
  ".....owwwwo.....",
  ".......ww.......",
  "....C......C....",
  "...cC......Cc...",
  ".cccC......Cccc.",
  "ccccC......Ccccc",
  "ccccC......Ccccc",
  "kkkkkkkkkkkkkkkk",
};

#define DROID_BIOME_TATOOINE 6
#endif
