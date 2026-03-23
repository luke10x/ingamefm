
/**
 * =============================================================================
 * xfm_api.h — eggsfm — Modular FM Synthesis Library (C99)
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
 * -----------------------------------------------------------------------------
 * 🧠 CORE MODEL
 * -----------------------------------------------------------------------------
 *
 * • Each module has:
 *     - One chip type
 *
 * • Assets:
 *     - Furnace patterns (parsed and played in real-time)
 *
 * -----------------------------------------------------------------------------
 */

#ifndef XFM_H
#define XFM_H

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
 * You MUST pass correct xfm_chip_type when assigning patches.
 */

/* =============================================================================
 * OPN (YM2612 / YM3438 style)
 * ============================================================================= */

typedef struct xfm_patch_opn_operator
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
} xfm_patch_opn_operator;

typedef struct xfm_patch_opn
{
    uint8_t ALG;     /* algorithm */
    uint8_t FB;      /* feedback */
    uint8_t AMS;     /* AM sensitivity (0-3) */
    uint8_t FMS;     /* FM sensitivity (0-7) */

    xfm_patch_opn_operator op[4]; /* 4 operators */
} xfm_patch_opn;

/* =============================================================================
 * OPM (YM2151 style)
 * ============================================================================= */

typedef struct xfm_patch_opm_operator
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
} xfm_patch_opm_operator;

typedef struct xfm_patch_opm
{
    uint8_t alg;
    uint8_t fb;

    uint8_t pan;     /* stereo placement */
    uint8_t lfo_freq;
    uint8_t lfo_wave;

    xfm_patch_opm_operator op[4];
} xfm_patch_opm;

/* =============================================================================
 * OPL2 / OPL3 (YM3812 / YMF262 style)
 * ============================================================================= */

typedef struct xfm_patch_opl_operator
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
} xfm_patch_opl_operator;

typedef struct xfm_patch_opl
{
    uint8_t alg;   /* connection type */
    uint8_t fb;

    uint8_t waveform; /* OPL2/OPL3 waveform select */

    xfm_patch_opl_operator op[2]; /* OPL2 = 2 ops, OPL3 may map 2+2 externally */
} xfm_patch_opl;

/* =============================================================================
 * TYPES
 * ============================================================================= */

typedef struct xfm_module xfm_module;

typedef int xfm_song_id;
typedef int xfm_sfx_id;
typedef int xfm_voice_id;
typedef int xfm_patch_id;

#define FM_VOICE_INVALID (-1)

/* =============================================================================
 * CHIP TYPES
 * ============================================================================= */

typedef enum {
    XFM_CHIP_YM2612 = 0,  /* OPN2 — original, authentic Sega sound */
    XFM_CHIP_YM3438,      /* OPN2C — CMOS, cleaner */
    XFM_CHIP_OPM,         /* YM2151 */
    XFM_CHIP_OPQ,         /* extended */
    XFM_CHIP_OPL2,
    XFM_CHIP_OPL3
} xfm_chip_type;

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
xfm_module* xfm_module_create(
    int sample_rate,
    int buffer_frames,
    xfm_chip_type chip_type
);

/**
 * Destroy module.
 */
void xfm_module_destroy(xfm_module* m);

/**
 * Set master volume (0.0 – 1.0).
 */
void xfm_module_set_volume(xfm_module* m, float volume);

/**
 * Enable / configure LFO.
 *
 * Interpretation of freq depends on chip type.
 */
void xfm_module_set_lfo(xfm_module* m, bool enable, int freq);

/* =============================================================================
 * PATCH SYSTEM
 * ============================================================================= */

/**
 * Generic patch setter.
 *
 * The data pointer must match the chip type:
 *
 *   XFM_CHIP_YM2612/YM3438 → YM2612-style struct
 *   XFM_CHIP_OPM  → YM2151-style struct
 *   XFM_CHIP_OPL2 → OPL2 struct
 *   etc.
 *
 * The engine does NOT reinterpret formats.
 */
void xfm_patch_set(
    xfm_module* m,
    xfm_patch_id patch_id,
    const void* patch_data,
    int patch_size,
    xfm_chip_type patch_type
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
xfm_song_id xfm_song_declare(
    xfm_module* m,
    xfm_song_id id,
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
 * @param id Song ID from xfm_song_declare
 * @param loop true = loop indefinitely, false = play once
 */
void xfm_song_play(xfm_module* m, xfm_song_id id, bool loop);

/**
 * Song switch timing options.
 */
typedef enum {
    FM_SONG_SWITCH_NOW = 0,     /* Switch immediately */
    FM_SONG_SWITCH_STEP,         /* Switch at next row */
    FM_SONG_SWITCH_LOOP          /* Switch at next loop point */
} xfm_song_switch_timing;

/**
 * Schedule a song change.
 *
 * @param m module
 * @param id Song ID to switch to
 * @param timing When to switch (NOW, STEP, or LOOP)
 */
void xfm_song_schedule(xfm_module* m, xfm_song_id id, xfm_song_switch_timing timing);

/**
 * Get current song row.
 *
 * @param m module
 * @return Current row index (0-based)
 */
int xfm_song_get_row(xfm_module* m);

/**
 * Get total rows in a song.
 *
 * @param m module
 * @param id Song ID
 * @return Total number of rows
 */
int xfm_song_get_total_rows(xfm_module* m, xfm_song_id id);

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
xfm_sfx_id xfm_sfx_declare(
    xfm_module* m,
    xfm_sfx_id id,
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
 * @param id SFX ID from xfm_sfx_declare
 * @param priority 0-9 (higher = harder to steal)
 * @return voice ID or FM_VOICE_INVALID
 */
xfm_voice_id xfm_sfx_play(
    xfm_module* m,
    xfm_sfx_id id,
    int priority
);

/**
 * Trigger note.
 */
xfm_voice_id xfm_note_on(
    xfm_module* m,
    int midi_note,
    xfm_patch_id patch_id,
    int velocity
);

/**
 * Release note.
 */
void xfm_note_off(xfm_module* m, xfm_voice_id v);

/* =============================================================================
 * AUDIO OUTPUT
 * ============================================================================= */

/**
 * Mix audio from module into buffer.
 * Processes both song and SFX if active.
 *
 * @param m module
 * @param stream interleaved stereo (int16_t)
 * @param frames number of frames (L+R pairs)
 */
void xfm_mix(xfm_module* m, int16_t* stream, int frames);

/**
 * Mix audio from music module (song only).
 * Use this for dedicated music modules.
 *
 * @param m module
 * @param stream interleaved stereo (int16_t)
 * @param frames number of frames (L+R pairs)
 */
void xfm_mix_song(xfm_module* m, int16_t* stream, int frames);

/**
 * Mix audio from SFX module (SFX only).
 * Use this for dedicated SFX modules.
 *
 * @param m module
 * @param stream interleaved stereo (int16_t)
 * @param frames number of frames (L+R pairs)
 */
void xfm_mix_sfx(xfm_module* m, int16_t* stream, int frames);

#ifdef __cplusplus
}
#endif

#endif /* XFM_H */