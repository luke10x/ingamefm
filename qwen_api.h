/**
 * =============================================================================
 * ingamefm.h — Dual-Chip FM Audio Library (C99)
 * =============================================================================
 * 
 * A high-performance FM synthesis library designed for games and demos.
 * Written in strict C99, compatible with C++ applications.
 * 
 * Architecture:
 *   - Two independent YM2612/YM3438 chips (Song Chip & SFX Chip).
 *   - Song Module: Sequenced music, caching, scheduling.
 *   - SFX Module: Polyphonic voice pool, interactive notes, SFX patterns.
 *   - Backend: SDL2 audio callback compatible.
 *   - GUI: Exposes introspection for ImGui editors (Patch, Keyboard).
 * 
 * Usage:
 *   1. Create context: fm_ctx_create()
 *   2. Configure: fm_set_sample_rate(), fm_set_chip_type()
 *   3. Load Content: fm_song_load(), fm_sfx_define(), fm_update_patch()
 *   4. Start Audio: fm_start() (handles SDL_OpenAudioDevice internally or externally)
 *   5. Mix: fm_mix() called from SDL audio callback.
 *   6. Control: fm_song_play(), fm_sfx_note_on(), etc.
 *   7. Destroy: fm_ctx_destroy()
 * 
 * =============================================================================
 */

#ifndef INGAMEFM_H
#define INGAMEFM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 1. CONSTANTS & TYPES
 * ============================================================================= */

#define FM_MAX_CHANNELS     6
#define FM_MAX_VOICES       6
#define FM_MAX_PATCHES      256
#define FM_MAX_SONGS        64
#define FM_MAX_SFX          64

typedef enum FMChipType {
    FM_CHIP_YM2612 = 0,
    FM_CHIP_YM3438 = 1
} FMChipType;

typedef enum FMSongChangeWhen {
    FM_SONG_CHANGE_NOW = 0,
    FM_SONG_CHANGE_AT_LOOP = 1
} FMSongChangeWhen;

typedef enum FMError {
    FM_OK = 0,
    FM_ERR_NO_VOICE,
    FM_ERR_INVALID_ID,
    FM_ERR_NOT_INITIALIZED,
    FM_ERR_CACHE_MISSING
} FMError;

/* =============================================================================
 * 2. YM2612 PATCH FORMAT
 *    Compatible with YM2612Patch struct in ingamefm_patchlib.h.
 * ============================================================================= */

typedef struct FMOperator {
    uint8_t DT;   /* Detune */
    uint8_t MUL;  /* Multiple */
    uint8_t TL;   /* Total Level */
    uint8_t RS;   /* Rate Scaling */
    uint8_t AR;   /* Attack Rate */
    uint8_t AM;   /* Amplitude Modulation Enable */
    uint8_t DR;   /* Decay Rate */
    uint8_t SR;   /* Sustain Rate */
    uint8_t SL;   /* Sustain Level */
    uint8_t RR;   /* Release Rate */
    uint8_t SSG;  /* SSG-EG Enable */
} FMOperator;

typedef struct FMPatch {
    uint8_t ALG;      /* Algorithm */
    uint8_t FB;       /* Feedback */
    uint8_t AMS;      /* Amplitude Modulation Sensitivity */
    uint8_t FMS;      /* Frequency Modulation Sensitivity */
    FMOperator op[4]; /* Operators 0-3 */
} FMPatch;

/* =============================================================================
 * 3. CONTEXT & INITIALIZATION
 * ============================================================================= */

typedef struct FMContext FMContext;

/**
 * Create the audio context.
 * @param sample_rate Output sample rate (e.g., 44100).
 * @param chip_type   YM2612 or YM3438 emulation core.
 * @return Opaque context pointer or NULL on failure.
 */
FMContext* fm_ctx_create(int sample_rate, FMChipType chip_type);

/**
 * Destroy the context and free resources.
 */
void fm_ctx_destroy(FMContext* ctx);

/**
 * Reset the engine state.
 * @param keep_cache If true, preserved cached song/SFX data.
 */
void fm_reset(FMContext* ctx, bool keep_cache);

/**
 * Set the sample rate (must be called before start).
 */
void fm_set_sample_rate(FMContext* ctx, int sample_rate);

/**
 * Set the chip type (must be called before start).
 */
void fm_set_chip_type(FMContext* ctx, FMChipType chip_type);

/* =============================================================================
 * 4. AUDIO MIXING & BACKEND
 * ============================================================================= */

/**
 * Main mixing function. Call this from your SDL_AudioCallback.
 * @param ctx    Audio context.
 * @param stream Output buffer (S16SYS, Stereo).
 * @param len    Length in bytes.
 */
void fm_mix(FMContext* ctx, int16_t* stream, int len);

/**
 * Helper to fill SDL_AudioSpec based on context settings.
 */
void fm_get_sdl_spec(FMContext* ctx, void* sdl_audio_spec_ptr);

/* =============================================================================
 * 5. PATCH MANAGEMENT
 *    Used by both Song and SFX modules.
 * ============================================================================= */

/**
 * Load a patch into the global patch table.
 * @param ctx   Context.
 * @param id    Patch ID (0-255).
 * @param patch Pointer to FMPatch struct.
 */
void fm_update_patch(FMContext* ctx, int id, const FMPatch* patch);

/**
 * Get a pointer to a patch for GUI editing.
 * @return Pointer to internal patch data. Do not free.
 */
FMPatch* fm_get_patch_ptr(FMContext* ctx, int id);

/* =============================================================================
 * 6. SONG MODULE (Music Chip)
 *    Dedicated chip, sequenced playback, caching.
 * ============================================================================= */

/**
 * Define a song pattern.
 * @param ctx       Context.
 * @param song_id   Unique ID.
 * @param pattern   Furnace-style pattern string.
 * @param tick_rate Tick rate (e.g., 60).
 * @param speed     Speed (rows per tick).
 */
void fm_song_load(FMContext* ctx, int song_id, const char* pattern, int tick_rate, int speed);

/**
 * Select a song to play.
 * @param ctx     Context.
 * @param song_id Song ID.
 * @param when    FM_SONG_CHANGE_NOW or FM_SONG_CHANGE_AT_LOOP.
 */
void fm_song_select(FMContext* ctx, int song_id, FMSongChangeWhen when);

/**
 * Stop song playback.
 */
void fm_song_stop(FMContext* ctx);

/**
 * Set Song Chip master volume (0.0 - 1.0).
 */
void fm_song_set_volume(FMContext* ctx, float volume);

/**
 * Set Song Chip LFO.
 * @param enable True to enable.
 * @param freq   Frequency index (0-8).
 */
void fm_song_set_lfo(FMContext* ctx, bool enable, int freq);

/**
 * Get current Song Chip LFO state (for GUI).
 */
bool fm_song_get_lfo_enable(FMContext* ctx);
int  fm_song_get_lfo_freq(FMContext* ctx);

/**
 * Get current playback row (for GUI).
 */
int fm_song_get_current_row(FMContext* ctx);

/**
 * Get total song length in rows (for GUI).
 */
int fm_song_get_length(FMContext* ctx, int song_id);

/* --- Caching API (Song Module) --- */

/**
 * Request to cache the current song and SFX to RAM.
 */
void fm_request_capture(FMContext* ctx);

/**
 * Cancel ongoing capture.
 */
void fm_cancel_capture(FMContext* ctx);

/**
 * Check if capture is pending (waiting for loop start).
 */
bool fm_is_capture_pending(FMContext* ctx);

/**
 * Check if currently capturing.
 */
bool fm_is_capturing(FMContext* ctx);

/**
 * Check if a specific song is fully cached.
 */
bool fm_is_song_captured(FMContext* ctx, int song_id);

/**
 * Get capture progress for current song (rows done).
 */
int fm_capture_song_rows_done(FMContext* ctx);

/**
 * Get capture progress for a specific SFX (rows done).
 */
int fm_capture_sfx_rows_done(FMContext* ctx, int sfx_id);

/**
 * Get total rows for a specific SFX.
 */
int fm_capture_sfx_total_rows(FMContext* ctx, int sfx_id);

/**
 * Enable playback from cache instead of live synthesis.
 */
void fm_set_play_cache(FMContext* ctx, bool enable);

/**
 * Check if all songs and SFX are cached.
 */
bool fm_all_cached(FMContext* ctx);

/* =============================================================================
 * 7. SFX MODULE (Effects Chip)
 *    Dedicated chip, voice pool, polyphonic notes.
 * ============================================================================= */

/**
 * Define an SFX pattern.
 * @param ctx       Context.
 * @param sfx_id    Unique ID.
 * @param pattern   Furnace-style pattern string (single channel).
 * @param tick_rate Tick rate.
 * @param speed     Speed.
 */
void fm_sfx_define(FMContext* ctx, int sfx_id, const char* pattern, int tick_rate, int speed);

/**
 * Play an SFX pattern on the voice pool.
 * @param ctx      Context.
 * @param sfx_id   SFX ID.
 * @param priority Priority level (higher preempts lower).
 * @param duration Hint for duration (rows).
 */
void fm_sfx_play(FMContext* ctx, int sfx_id, int priority, int duration);

/**
 * Set SFX Chip master volume (0.0 - 1.0).
 */
void fm_sfx_set_volume(FMContext* ctx, float volume);

/**
 * Set SFX Chip LFO.
 */
void fm_sfx_set_lfo(FMContext* ctx, bool enable, int freq);

/**
 * Get current SFX Chip LFO state (for GUI).
 */
bool fm_sfx_get_lfo_enable(FMContext* ctx);
int  fm_sfx_get_lfo_freq(FMContext* ctx);

/**
 * Get count of active voices (for GUI visualizer).
 */
int fm_sfx_get_active_voice_count(FMContext* ctx);

/* --- Interactive Note API (Keyboard/Piano) --- */

/**
 * Begin a note (Key Press).
 * Allocates a voice from the pool. Voice remains active until note_off.
 * @param ctx       Context.
 * @param midi_note MIDI note number (e.g., 60 for C4).
 * @param patch_id  Patch ID to use.
 * @param velocity  Velocity (0-127).
 * @return Voice ID (>0) on success, -1 if no voice available.
 */
int fm_sfx_note_on(FMContext* ctx, int midi_note, int patch_id, int velocity);

/**
 * End a note (Key Release).
 * Triggers Release envelope. Voice frees automatically after envelope ends.
 * @param ctx      Context.
 * @param voice_id Voice ID returned by fm_sfx_note_on.
 */
void fm_sfx_note_off(FMContext* ctx, int voice_id);

/* =============================================================================
 * 8. LEGACY / COMPATIBILITY
 * ============================================================================= */

/**
 * Legacy piano note on (maps to fm_sfx_note_on internally).
 * Provided for compatibility with demo7 logic.
 */
void fm_piano_note_on(FMContext* ctx, int midi_note, int patch_id, int velocity);

/**
 * Legacy piano note off (maps to fm_sfx_note_off internally).
 * Note: If using voice IDs, prefer fm_sfx_note_off.
 */
void fm_piano_note_off(FMContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* INGAMEFM_H */