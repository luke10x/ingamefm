#pragma once
// =============================================================================
// ingamefm_patches.h  — part of ingamefm (header-only)
//
// Built-in YM2612 patch catalogue.  Include this header and pass any patch
// to IngameFMPlayer::add_patch() or IngameFMChip::load_patch().
//
// Each patch is accompanied by:
//   _BLOCK       — recommended octave offset for set_frequency()
//   _LFO_ENABLE  — 1 = enable chip LFO before playing this patch
//   _LFO_FREQ    — LFO frequency index 0-7 (used when _LFO_ENABLE = 1)
// =============================================================================

#include "ingamefm_patchlib.h"

// =============================================================================
// Bass instruments
// =============================================================================

static constexpr YM2612Patch PATCH_SLAP_BASS =
{
    .ALG = 4, .FB = 5, .AMS = 2, .FMS = 3,
    .op =
    {
        { .DT=3, .MUL=1, .TL=34, .RS=0, .AR=31, .AM=0, .DR=10, .SR=6, .SL=4, .RR=7, .SSG=0 },
        { .DT=0, .MUL=2, .TL=18, .RS=1, .AR=25, .AM=0, .DR=12, .SR=5, .SL=5, .RR=6, .SSG=0 },
        { .DT=0, .MUL=1, .TL= 0, .RS=0, .AR=31, .AM=0, .DR= 6, .SR=3, .SL=6, .RR=5, .SSG=0 },
        { .DT=0, .MUL=1, .TL= 0, .RS=0, .AR=31, .AM=0, .DR= 7, .SR=2, .SL=5, .RR=5, .SSG=0 }
    }
};
static constexpr int PATCH_SLAP_BASS_BLOCK      = 0;
static constexpr int PATCH_SLAP_BASS_LFO_ENABLE = 0;
static constexpr int PATCH_SLAP_BASS_LFO_FREQ   = 0;

static constexpr YM2612Patch PATCH_SYNTH_BASS =
{
    .ALG = 5, .FB = 7, .AMS = 0, .FMS = 4,
    .op =
    {
        { .DT=0, .MUL=1, .TL=20, .RS=0, .AR=31, .AM=0, .DR=15, .SR=7, .SL=3, .RR=8, .SSG=0 },
        { .DT=0, .MUL=1, .TL=15, .RS=0, .AR=28, .AM=0, .DR=12, .SR=6, .SL=4, .RR=7, .SSG=0 },
        { .DT=0, .MUL=0, .TL= 0, .RS=0, .AR=31, .AM=0, .DR= 0, .SR=0, .SL=0, .RR=0, .SSG=0 },
        { .DT=0, .MUL=0, .TL= 0, .RS=0, .AR=31, .AM=0, .DR= 0, .SR=0, .SL=0, .RR=0, .SSG=0 }
    }
};
static constexpr int PATCH_SYNTH_BASS_BLOCK      = 0;
static constexpr int PATCH_SYNTH_BASS_LFO_ENABLE = 1;
static constexpr int PATCH_SYNTH_BASS_LFO_FREQ   = 3;

static constexpr YM2612Patch PATCH_ELECTRIC_BASS =
{
    .ALG = 4, .FB = 6, .AMS = 1, .FMS = 2,
    .op =
    {
        { .DT=2, .MUL=1, .TL=28, .RS=0, .AR=31, .AM=0, .DR=12, .SR=5, .SL=4, .RR=6, .SSG=0 },
        { .DT=0, .MUL=1, .TL=22, .RS=0, .AR=26, .AM=0, .DR=10, .SR=4, .SL=5, .RR=5, .SSG=0 },
        { .DT=0, .MUL=1, .TL= 0, .RS=0, .AR=31, .AM=0, .DR= 5, .SR=3, .SL=6, .RR=4, .SSG=0 },
        { .DT=0, .MUL=0, .TL= 0, .RS=0, .AR=31, .AM=0, .DR= 0, .SR=0, .SL=0, .RR=0, .SSG=0 }
    }
};
static constexpr int PATCH_ELECTRIC_BASS_BLOCK      = 0;
static constexpr int PATCH_ELECTRIC_BASS_LFO_ENABLE = 0;
static constexpr int PATCH_ELECTRIC_BASS_LFO_FREQ   = 0;

static constexpr YM2612Patch PATCH_ACOUSTIC_BASS =
{
    .ALG = 2, .FB = 3, .AMS = 0, .FMS = 1,
    .op =
    {
        { .DT=1, .MUL=1, .TL=24, .RS=0, .AR=30, .AM=0, .DR= 8, .SR=4, .SL=3, .RR=5, .SSG=0 },
        { .DT=0, .MUL=2, .TL=16, .RS=1, .AR=24, .AM=0, .DR=10, .SR=5, .SL=4, .RR=6, .SSG=0 },
        { .DT=0, .MUL=1, .TL=12, .RS=0, .AR=28, .AM=0, .DR= 6, .SR=3, .SL=5, .RR=4, .SSG=0 },
        { .DT=0, .MUL=0, .TL= 0, .RS=0, .AR=31, .AM=0, .DR= 0, .SR=0, .SL=0, .RR=0, .SSG=0 }
    }
};
static constexpr int PATCH_ACOUSTIC_BASS_BLOCK      = 0;
static constexpr int PATCH_ACOUSTIC_BASS_LFO_ENABLE = 0;
static constexpr int PATCH_ACOUSTIC_BASS_LFO_FREQ   = 0;

// =============================================================================
// Lead / melody instruments
// =============================================================================

static constexpr YM2612Patch PATCH_GUITAR =
{
    .ALG = 3, .FB = 7, .AMS = 0, .FMS = 0,
    .op =
    {
        { .DT= 3, .MUL=15, .TL=61, .RS=0, .AR=11, .AM=0, .DR= 0, .SR=0, .SL=10, .RR=0, .SSG=0 },
        { .DT= 3, .MUL= 1, .TL= 4, .RS=0, .AR=21, .AM=0, .DR=18, .SR=0, .SL= 2, .RR=4, .SSG=0 },
        { .DT=-2, .MUL= 7, .TL=19, .RS=0, .AR=31, .AM=0, .DR=31, .SR=0, .SL=15, .RR=9, .SSG=1 },
        { .DT= 0, .MUL= 2, .TL= 6, .RS=0, .AR=21, .AM=0, .DR= 5, .SR=0, .SL= 1, .RR=5, .SSG=0 }
    }
};
static constexpr int PATCH_GUITAR_BLOCK      = 0;
static constexpr int PATCH_GUITAR_LFO_ENABLE = 0;
static constexpr int PATCH_GUITAR_LFO_FREQ   = 5;

static constexpr YM2612Patch PATCH_SUPERSAW =
{
    .ALG = 7, .FB = 6, .AMS = 0, .FMS = 0,
    .op =
    {
        { .DT=-2, .MUL=1, .TL= 8, .RS=0, .AR=31, .AM=0, .DR=10, .SR=0, .SL=0, .RR=6, .SSG=0 },
        { .DT= 0, .MUL=1, .TL=10, .RS=0, .AR=31, .AM=0, .DR=10, .SR=0, .SL=0, .RR=6, .SSG=0 },
        { .DT= 2, .MUL=1, .TL= 8, .RS=0, .AR=31, .AM=0, .DR=10, .SR=0, .SL=0, .RR=6, .SSG=0 },
        { .DT= 0, .MUL=2, .TL=18, .RS=0, .AR=31, .AM=0, .DR=12, .SR=0, .SL=0, .RR=6, .SSG=0 }
    }
};
static constexpr int PATCH_SUPERSAW_BLOCK      = 0;
static constexpr int PATCH_SUPERSAW_LFO_ENABLE = 0;
static constexpr int PATCH_SUPERSAW_LFO_FREQ   = 0;

static constexpr YM2612Patch PATCH_FLUTE =
{
    .ALG = 4, .FB = 5, .AMS = 0, .FMS = 0,
    .op =
    {
        { .DT=0, .MUL=1, .TL=63, .RS=0, .AR=31, .AM=0, .DR= 5, .SR=0, .SL= 1, .RR=10, .SSG=0 },
        { .DT=3, .MUL=1, .TL= 0, .RS=0, .AR=31, .AM=0, .DR=16, .SR=0, .SL= 1, .RR=10, .SSG=0 },
        { .DT=0, .MUL=1, .TL=63, .RS=0, .AR=31, .AM=0, .DR= 5, .SR=0, .SL= 1, .RR=10, .SSG=0 },
        { .DT=0, .MUL=1, .TL= 0, .RS=0, .AR= 0, .AM=0, .DR= 5, .SR=0, .SL= 1, .RR=10, .SSG=0 }
    }
};
static constexpr int PATCH_FLUTE_BLOCK      = 0;
static constexpr int PATCH_FLUTE_LFO_ENABLE = 0;
static constexpr int PATCH_FLUTE_LFO_FREQ   = 0;

// =============================================================================
// Percussion
// =============================================================================

// FM Kick drum — low sine thump with fast pitch drop via high modulator MUL.
// Play at a low note (e.g. C1 / MIDI 24) for best results.
static constexpr YM2612Patch PATCH_KICK =
{
    // ALG 0: OP1->OP2->OP3->OP4 full chain — deep modulation for pitch transient
    .ALG = 0, .FB = 7, .AMS = 0, .FMS = 0,
    .op =
    {
        // OP1: very fast decay modulator — produces the initial click/pitch drop
        { .DT=0, .MUL=1, .TL= 0, .RS=3, .AR=31, .AM=0, .DR=31, .SR=0, .SL=15, .RR=15, .SSG=0 },
        // OP2: secondary modulator shaping the body
        { .DT=0, .MUL=1, .TL=16, .RS=2, .AR=31, .AM=0, .DR=20, .SR=0, .SL=15, .RR=10, .SSG=0 },
        // OP3: body modulator
        { .DT=0, .MUL=1, .TL=20, .RS=1, .AR=31, .AM=0, .DR=18, .SR=0, .SL=15, .RR= 8, .SSG=0 },
        // OP4: carrier — the audible thump with medium-fast release
        { .DT=0, .MUL=1, .TL= 0, .RS=0, .AR=31, .AM=0, .DR=14, .SR=0, .SL=15, .RR= 8, .SSG=0 }
    }
};
static constexpr int PATCH_KICK_BLOCK      = 0;
static constexpr int PATCH_KICK_LFO_ENABLE = 0;
static constexpr int PATCH_KICK_LFO_FREQ   = 0;

// FM Snare drum — noise-like bright crack via heavy cross-modulation.
// Play at a mid note (e.g. A2 / MIDI 45) for best results.
static constexpr YM2612Patch PATCH_SNARE =
{
    // ALG 4: (OP1->OP2) + (OP3->OP4) — two modulated carriers, creates snare buzz
    .ALG = 4, .FB = 7, .AMS = 0, .FMS = 0,
    .op =
    {
        // OP1: high-ratio modulator — produces noisy inharmonic component
        { .DT=3, .MUL=15, .TL= 0, .RS=3, .AR=31, .AM=0, .DR=31, .SR=0, .SL=15, .RR=15, .SSG=0 },
        // OP2: carrier 1 — bright crack transient, very fast decay
        { .DT=0, .MUL= 3, .TL= 0, .RS=2, .AR=31, .AM=0, .DR=28, .SR=0, .SL=15, .RR=14, .SSG=0 },
        // OP3: second modulator — adds buzzy mid-frequency body
        { .DT=2, .MUL= 7, .TL=10, .RS=1, .AR=31, .AM=0, .DR=24, .SR=0, .SL=15, .RR=12, .SSG=0 },
        // OP4: carrier 2 — body of the snare, slightly longer tail
        { .DT=0, .MUL= 1, .TL= 2, .RS=0, .AR=31, .AM=0, .DR=22, .SR=0, .SL=15, .RR=10, .SSG=0 }
    }
};
static constexpr int PATCH_SNARE_BLOCK      = 0;
static constexpr int PATCH_SNARE_LFO_ENABLE = 0;
static constexpr int PATCH_SNARE_LFO_FREQ   = 0;

// =============================================================================
// Patch catalogue — iterate all patches at runtime if needed
// =============================================================================

struct PatchEntry
{
    const char*        name;
    const YM2612Patch* patch;
    int                block;
    int                lfoEnable;
    int                lfoFreq;
};

static const PatchEntry PATCH_CATALOGUE[] =
{
    { "Slap Bass",     &PATCH_SLAP_BASS,     PATCH_SLAP_BASS_BLOCK,     PATCH_SLAP_BASS_LFO_ENABLE,     PATCH_SLAP_BASS_LFO_FREQ     },
    { "Synth Bass",    &PATCH_SYNTH_BASS,    PATCH_SYNTH_BASS_BLOCK,    PATCH_SYNTH_BASS_LFO_ENABLE,    PATCH_SYNTH_BASS_LFO_FREQ    },
    { "Electric Bass", &PATCH_ELECTRIC_BASS, PATCH_ELECTRIC_BASS_BLOCK, PATCH_ELECTRIC_BASS_LFO_ENABLE, PATCH_ELECTRIC_BASS_LFO_FREQ },
    { "Acoustic Bass", &PATCH_ACOUSTIC_BASS, PATCH_ACOUSTIC_BASS_BLOCK, PATCH_ACOUSTIC_BASS_LFO_ENABLE, PATCH_ACOUSTIC_BASS_LFO_FREQ },
    { "Guitar",        &PATCH_GUITAR,        PATCH_GUITAR_BLOCK,        PATCH_GUITAR_LFO_ENABLE,        PATCH_GUITAR_LFO_FREQ        },
    { "Supersaw",      &PATCH_SUPERSAW,      PATCH_SUPERSAW_BLOCK,      PATCH_SUPERSAW_LFO_ENABLE,      PATCH_SUPERSAW_LFO_FREQ      },
    { "Flute",         &PATCH_FLUTE,         PATCH_FLUTE_BLOCK,         PATCH_FLUTE_LFO_ENABLE,         PATCH_FLUTE_LFO_FREQ         },
    { "Kick",          &PATCH_KICK,          PATCH_KICK_BLOCK,          PATCH_KICK_LFO_ENABLE,          PATCH_KICK_LFO_FREQ          },
    { "Snare",         &PATCH_SNARE,         PATCH_SNARE_BLOCK,         PATCH_SNARE_LFO_ENABLE,         PATCH_SNARE_LFO_FREQ         },
};
static constexpr int PATCH_CATALOGUE_SIZE = static_cast<int>(sizeof(PATCH_CATALOGUE) / sizeof(PATCH_CATALOGUE[0]));
