#pragma once
// =============================================================================
// ingamefm.h  — single include for the ingamefm header-only library
//
// Pull in both modules:
//   ingamefm_patchlib.h  — YM2612Patch struct + IngameFMChip register-programming wrapper
//   ingamefm_player.h    — IngameFMPlayer (parser + SDL playback engine)
//
// Dependencies (must be on your include path):
//   ymfm.h / ymfm_opn.h  (ymfm chip emulation library)
//   SDL.h                 (SDL 2)
//
// Usage:
//   #define FURNACE_SDL_IMPLEMENTATION  (in exactly ONE .cpp file)
//   #include "ingamefm.h"
//
// Because the library is fully header-only all definitions live in the headers.
// The FURNACE_SDL_IMPLEMENTATION guard is reserved for future non-inline code.
// =============================================================================

#include "ingamefm_patchlib.h"
#include "ingamefm_player.h"
#include "ingamefm_patches.h"
