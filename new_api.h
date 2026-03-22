
/**
 * =============================================================================
 * ingamefm.h — Modular FM Synthesis & Cached Playback Library (C99)
 * =============================================================================
 *
 * A modular FM audio system for games and demos.
 * Each module represents ONE FM chip instance.
 *
 * Supported chip families (extensible):
 *   • OPN  (YM2612 / YM3438)
 *   • OPM  (YM2151)
 *   • OPQ  (YM??? extended variants)
 *   • OPL2 / OPL3
 *
 * The API abstracts scheduling and playback, while chip-specific details are
 * handled through patch structures and chip configuration.
 *
 * -----------------------------------------------------------------------------
 * 🧠 CORE MODEL
 * -----------------------------------------------------------------------------
 *
 * • Each module has:
 *     - One chip type
 *     - One scheduler mode (SONG or SFX)
 *     - One playback mode (SYNTH / RECORD / CACHE)
 *
 * • Assets:
 *     - SYNTH (Furnace patterns)
 *     - CACHE (PCM buffers, client-owned)
 *
 * • RECORD mode:
 *     - Synth plays normally
 *     - Cache buffers are filled progressively
 *
 * • CACHE mode:
 *     - No chip required
 *     - Unlimited SFX playback
 *     - No note triggering / chip control
 *
 * -----------------------------------------------------------------------------
 * 📦 CACHE FORMAT (VARIABLE ROW LENGTHS)
 * -----------------------------------------------------------------------------
 *
 * A "row" = one tracker step.
 *
 * Due to fractional timing:
 *   sample_rate / (ticks * speed) is NOT integer.
 *
 * Therefore:
 *   • Each row may contain a VARIABLE number of samples
 *   • Cache playback MUST follow row boundaries exactly
 *   • This ensures perfect sync with SYNTH mode
 *
 * -----------------------------------------------------------------------------
 */

#ifndef INGAMEFM_H
#define INGAMEFM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
/**
 * =============================================================================
 * FM PATCH DATA STRUCTURES
 * =============================================================================
 *
 * These are low-level register-style patch definitions.
 * They are NOT runtime voice objects.
 *
 * Each patch maps closely to real chip parameters.
 * The engine simply forwards them to the correct backend.
 *
 * -----------------------------------------------------------------------------
 * IMPORTANT DESIGN RULE
 * -----------------------------------------------------------------------------
 *
 * • These structs are CHIP-SPECIFIC
 * • They are NOT interchangeable
 * • Engine does NOT reinterpret between formats
 *
 * You MUST pass correct fm_chip_type when assigning patches.
 */

/* =============================================================================
 * OPN (YM2612 / YM3438 style)
 * ============================================================================= */

typedef struct fm_patch_opn_operator
{
    int8_t  DT;     /* detune (-3..+3) */
    uint8_t MUL;    /* frequency multiplier */
    uint8_t TL;     /* total level */
    uint8_t RS;     /* rate scale */
    uint8_t AR;     /* attack rate */
    uint8_t AM;     /* amplitude modulation enable */
    uint8_t DR;     /* decay rate */
    uint8_t SR;     /* sustain rate */
    uint8_t SL;     /* sustain level */
    uint8_t RR;     /* release rate */
    uint8_t SSG;    /* SSG-EG (OPN specific) */
} fm_patch_opn_operator;

typedef struct fm_patch_opn
{
    uint8_t ALG;     /* algorithm */
    uint8_t FB;      /* feedback */
    uint8_t LFO;     /* LFO enable / sensitivity */

    fm_patch_opn_operator op[4]; /* 4 operators */
} fm_patch_opn;

/* =============================================================================
 * OPM (YM2151 style)
 * ============================================================================= */

typedef struct fm_patch_opm_operator
{
    uint8_t dt1;    /* coarse detune */
    uint8_t dt2;    /* fine detune */
    uint8_t mul;

    uint8_t tl;

    uint8_t ks;     /* key scale */
    uint8_t ar;
    uint8_t dr;
    uint8_t sr;
    uint8_t rr;
    uint8_t sl;

    uint8_t ssg;    /* SSG EG (OPM variant) */
} fm_patch_opm_operator;

typedef struct fm_patch_opm
{
    uint8_t alg;
    uint8_t fb;

    uint8_t pan;     /* stereo placement */
    uint8_t lfo_freq;
    uint8_t lfo_wave;

    fm_patch_opm_operator op[4];
} fm_patch_opm;

/* =============================================================================
 * OPL2 / OPL3 (YM3812 / YMF262 style)
 * ============================================================================= */

typedef struct fm_patch_opl_operator
{
    uint8_t am;     /* amplitude modulation */
    uint8_t vib;    /* vibrato */
    uint8_t eg;     /* envelope generator type */
    uint8_t ksr;    /* key scaling rate */
    uint8_t mul;

    uint8_t ksl;    /* key scale level */
    uint8_t tl;

    uint8_t ar;
    uint8_t dr;
    uint8_t sl;
    uint8_t rr;
} fm_patch_opl_operator;

typedef struct fm_patch_opl
{
    uint8_t alg;   /* connection type */
    uint8_t fb;

    uint8_t waveform; /* OPL2/OPL3 waveform select */

    fm_patch_opl_operator op[2]; /* OPL2 = 2 ops, OPL3 may map 2+2 externally */
} fm_patch_opl;

/* =============================================================================
 * TYPES
 * ============================================================================= */

typedef struct fm_module fm_module;

typedef int fm_song_id;
typedef int fm_sfx_id;
typedef int fm_voice_id;
typedef int fm_patch_id;

#define FM_VOICE_INVALID (-1)

/* =============================================================================
 * CHIP TYPES
 * ============================================================================= */

typedef enum {
    FM_CHIP_YM2612 = 0,  /* OPN2 — original, authentic Sega sound */
    FM_CHIP_YM3438,      /* OPN2C — CMOS, cleaner */
    FM_CHIP_OPM,         /* YM2151 */
    FM_CHIP_OPQ,         /* extended */
    FM_CHIP_OPL2,
    FM_CHIP_OPL3
} fm_chip_type;

/* =============================================================================
 * PLAYBACK & SCHEDULING
 * ============================================================================= */

typedef enum {
    FM_MODE_SYNTH = 0,
    FM_MODE_RECORD,
    FM_MODE_CACHE
} fm_mode;

typedef enum {
    FM_SCHED_SONG = 0,
    FM_SCHED_SFX
} fm_sched_mode;

/* =============================================================================
 * MODULE LIFETIME
 * ============================================================================= */

/**
 * Create module.
 *
 * @param sample_rate   output sample rate
 * @param buffer_frames audio buffer size
 * @param chip_type     FM chip family
 */
fm_module* fm_module_create(
    int sample_rate,
    int buffer_frames,
    fm_chip_type chip_type
);

/**
 * Destroy module.
 */
void fm_module_destroy(fm_module* m);

/**
 * Set playback mode.
 *
 * RULE:
 *   FM_MODE_CACHE is allowed ONLY if all declared assets are cached.
 */
bool fm_module_set_mode(fm_module* m, fm_mode mode);

/**
 * Set scheduler mode (init-time only).
 */
void fm_module_set_scheduler(fm_module* m, fm_sched_mode mode);

/**
 * Set master volume (0.0 – 1.0).
 */
void fm_module_set_volume(fm_module* m, float volume);

/**
 * Set chip clock (Hz).
 *
 * Needed for accurate frequency generation.
 */
void fm_module_set_clock(fm_module* m, int clock_hz);

/**
 * Enable / configure LFO.
 *
 * Interpretation of freq depends on chip type.
 */
void fm_module_set_lfo(fm_module* m, bool enable, int freq);

/* =============================================================================
 * PATCH SYSTEM
 * ============================================================================= */

/**
 * Generic patch setter.
 *
 * The data pointer must match the chip type:
 *
 *   FM_CHIP_YM2612/YM3438 → YM2612-style struct
 *   FM_CHIP_OPM  → YM2151-style struct
 *   FM_CHIP_OPL2 → OPL2 struct
 *   etc.
 *
 * The engine does NOT reinterpret formats.
 */
void fm_patch_set(
    fm_module* m,
    fm_patch_id patch_id,
    const void* patch_data,
    int patch_size,
    fm_chip_type patch_type
);

/**
 * Assign patch to song channel.
 */
void fm_patch_set_song_channel(
    fm_module* m,
    fm_song_id song,
    int channel,
    fm_patch_id patch_id
);

/**
 * Assign default patch to SFX.
 */
void fm_patch_set_sfx(
    fm_module* m,
    fm_sfx_id sfx,
    fm_patch_id patch_id
);

/* =============================================================================
 * SONG DECLARATION
 * ============================================================================= */

/**
 * Declare a song pattern.
 * 
 * Pattern format is multi-channel Furnace format:
 *   First line: number of rows
 *   Each row: channel1|channel2|...|channelN
 *   Each channel: note(3) + instrument(2) + volume(2) [+ effects]
 * 
 * Example (2 channels):
 *   "8\n"
 *   "C-4007F....|E-4017F....\n"
 *   "...........|...........\n"
 *   ...
 *
 * @param m module
 * @param id Song ID (1-15) to assign
 * @param pattern_text Furnace pattern text
 * @param tick_rate ticks per second (e.g. 60)
 * @param speed ticks per row (e.g. 6 = 100ms/row at 60Hz)
 * @return Song ID or -1 on error
 */
fm_song_id fm_song_declare(
    fm_module* m,
    fm_song_id id,
    const char* pattern_text,
    int tick_rate,
    int speed
);

/* =============================================================================
 * SONG CONTROL
 * ============================================================================= */

/**
 * Start playing a song.
 *
 * @param m module
 * @param id Song ID from fm_song_declare
 * @param loop true = loop indefinitely, false = play once
 */
void fm_song_play(fm_module* m, fm_song_id id, bool loop);

/**
 * Song switch timing options.
 */
typedef enum {
    FM_SONG_SWITCH_NOW = 0,     /* Switch immediately */
    FM_SONG_SWITCH_STEP,         /* Switch at next row */
    FM_SONG_SWITCH_LOOP          /* Switch at next loop point */
} fm_song_switch_timing;

/**
 * Schedule a song change.
 *
 * @param m module
 * @param id Song ID to switch to
 * @param timing When to switch (NOW, STEP, or LOOP)
 */
void fm_song_schedule(fm_module* m, fm_song_id id, fm_song_switch_timing timing);

/**
 * Get current song row.
 *
 * @param m module
 * @return Current row index (0-based)
 */
int fm_song_get_row(fm_module* m);

/**
 * Get total rows in a song.
 *
 * @param m module
 * @param id Song ID
 * @return Total number of rows
 */
int fm_song_get_total_rows(fm_module* m, fm_song_id id);

/* =============================================================================
 * SFX DECLARATION
 * ============================================================================= */

/**
 * Declare an SFX pattern.
 * 
 * Pattern format is single-channel Furnace format:
 *   First line: number of rows
 *   Each row: note(3) + instrument(2) + volume(2)
 * 
 * Example:
 *   "6\n"
 *   "C-4007F\n"
 *   "E-4007F\n"
 *   "G-4007F\n"
 *   "C-5007F\n"
 *   "OFF....\n"
 *   ".......\n"
 *
 * @param m module
 * @param id SFX ID (0-255) to assign
 * @param pattern_text Furnace pattern text
 * @param tick_rate ticks per second (e.g. 60)
 * @param speed ticks per row (e.g. 3 = 50ms/row)
 * @return SFX ID or -1 on error
 */
fm_sfx_id fm_sfx_declare(
    fm_module* m,
    fm_sfx_id id,
    const char* pattern_text,
    int tick_rate,
    int speed
);

/* =============================================================================
 * SFX CONTROL
 * ============================================================================= */

/**
 * Play SFX.
 *
 * Uses voice stealing:
 *   - Prefers free voices
 *   - Steals lowest priority voice if all busy
 *   - If equal priority, steals oldest
 *
 * Priority scale:
 *   0 = piano/manual notes (lowest)
 *   1-2 = ambient SFX
 *   3-4 = common gameplay
 *   5-6 = significant events
 *   7-9 = critical/must-hear
 *
 * @param m module
 * @param id SFX ID from fm_sfx_declare
 * @param priority 0-9 (higher = harder to steal)
 * @return voice ID or FM_VOICE_INVALID
 */
fm_voice_id fm_sfx_play(
    fm_module* m,
    fm_sfx_id id,
    int priority
);

/**
 * Trigger note (SYNTH only).
 *
 * Returns FM_VOICE_INVALID in CACHE mode.
 */
fm_voice_id fm_note_on(
    fm_module* m,
    int midi_note,
    fm_patch_id patch_id,
    int velocity
);

/**
 * Release note.
 */
void fm_note_off(fm_module* m, fm_voice_id v);

/* =============================================================================
 * AUDIO OUTPUT
 * ============================================================================= */

/**
 * Mix audio into buffer.
 *
 * @param stream interleaved stereo (int16_t)
 * @param frames number of frames (L+R pairs)
 */
void fm_mix(fm_module* m, int16_t* stream, int frames);

#ifdef __cplusplus
}
#endif

#endif /* INGAMEFM_H */