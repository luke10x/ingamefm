/**
 * =============================================================================
 * xfm_wavplay.h — WAV Playback for eggsfm (pre-rendered alternative)
 * =============================================================================
 *
 * Mirror API for xfm_api.h that plays pre-rendered WAV files instead of
 * real-time synthesis. Useful for clients that cannot afford real-time CPU cost.
 *
 * Features:
 *   - Load WAV files into memory
 *   - Play songs/SFX from WAV data
 *   - Same API pattern as xfm_api.h for easy switching
 *   - Multiple concurrent SFX voices
 *   - Song looping support
 *
 * Usage:
 *   xfm_wav_module* m = xfm_wav_module_create(44100, 256);
 *   
 *   // Load pre-rendered WAV files
 *   xfm_wav_load_file(m, XFM_WAV_SONG, 1, "song_1.wav");
 *   xfm_wav_load_file(m, XFM_WAV_SFX, 0, "sfx_jump.wav");
 *   
 *   // Play (same API as synthesis)
 *   xfm_wav_song_play(m, 1, true);
 *   xfm_wav_sfx_play(m, 0, 5);
 *   
 *   // Mix audio (drop-in replacement for xfm_mix_song/sfx)
 *   xfm_wav_mix_song(m, buffer, frames);
 *   xfm_wav_mix_sfx(m, buffer, frames);
 */

#ifndef XFM_WAVPLAY_H
#define XFM_WAVPLAY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to WAV playback module
 */
typedef struct xfm_wav_module xfm_wav_module;

/**
 * @brief WAV content type
 */
typedef enum {
    XFM_WAV_SONG = 0,  // Song pattern
    XFM_WAV_SFX  = 1   // Sound effect
} xfm_wav_type;

/**
 * @brief Voice ID for SFX playback
 */
typedef int xfm_wav_voice_id;

/**
 * @brief Song switch timing
 */
typedef enum {
    XFM_WAV_SWITCH_NOW = 0,   // Switch immediately
    XFM_WAV_SWITCH_STEP = 1,  // Switch at next row
    XFM_WAV_SWITCH_LOOP = 2   // Switch at next loop point
} xfm_wav_switch_timing;

/**
 * @brief Create WAV playback module
 *
 * @param sample_rate Output sample rate (e.g., 44100)
 * @param buffer_frames Buffer size in frames (e.g., 256)
 * @return Module instance, or NULL on error
 */
xfm_wav_module* xfm_wav_module_create(int sample_rate, int buffer_frames);

/**
 * @brief Destroy WAV playback module
 *
 * @param m Module instance
 */
void xfm_wav_module_destroy(xfm_wav_module* m);

/**
 * @brief Load WAV data from file
 *
 * @param m Module instance
 * @param type Content type (song or SFX)
 * @param id Content ID (1-15 for songs, 0-255 for SFX)
 * @param filename Path to WAV file
 * @return 0 on success, -1 on error
 */
int xfm_wav_load_file(xfm_wav_module* m, xfm_wav_type type, int id, const char* filename);

/**
 * @brief Load WAV data from memory buffer
 *
 * @param m Module instance
 * @param type Content type (song or SFX)
 * @param id Content ID (1-15 for songs, 0-255 for SFX)
 * @param data Pointer to WAV data in memory
 * @param size Size of WAV data in bytes
 * @param copy_data If true, copy data internally; if false, use provided pointer
 * @return 0 on success, -1 on error
 */
int xfm_wav_load_memory(xfm_wav_module* m, xfm_wav_type type, int id, 
                        const void* data, int size, bool copy_data);

/**
 * @brief Unload WAV data
 *
 * @param m Module instance
 * @param type Content type (song or SFX)
 * @param id Content ID to unload
 */
void xfm_wav_unload(xfm_wav_module* m, xfm_wav_type type, int id);

/**
 * @brief Play a song
 *
 * @param m Module instance
 * @param song_id Song ID to play (1-15)
 * @param loop If true, loop at end of song
 */
void xfm_wav_song_play(xfm_wav_module* m, int song_id, bool loop);

/**
 * @brief Schedule song change
 *
 * @param m Module instance
 * @param song_id Song ID to switch to (1-15)
 * @param timing When to switch (NOW, STEP, or LOOP)
 */
void xfm_wav_song_schedule(xfm_wav_module* m, int song_id, xfm_wav_switch_timing timing);

/**
 * @brief Play a sound effect
 *
 * @param m Module instance
 * @param sfx_id SFX ID to play (0-255)
 * @param priority Priority level (higher = less likely to be interrupted)
 * @return Voice ID, or -1 if no voices available
 */
xfm_wav_voice_id xfm_wav_sfx_play(xfm_wav_module* m, int sfx_id, int priority);

/**
 * @brief Stop a sound effect
 *
 * @param m Module instance
 * @param voice Voice ID from xfm_wav_sfx_play()
 */
void xfm_wav_sfx_stop(xfm_wav_module* m, xfm_wav_voice_id voice);

/**
 * @brief Stop all sound effects
 *
 * @param m Module instance
 */
void xfm_wav_sfx_stop_all(xfm_wav_module* m);

/**
 * @brief Get current song row (for UI display)
 *
 * @param m Module instance
 * @return Current row index, or 0 if no song playing
 */
int xfm_wav_song_get_row(xfm_wav_module* m);

/**
 * @brief Get total rows in a song (from WAV metadata or estimate)
 *
 * @param m Module instance
 * @param song_id Song ID
 * @return Total rows, or 0 if not found
 */
int xfm_wav_song_get_total_rows(xfm_wav_module* m, int song_id);

/**
 * @brief Check if a song is currently playing
 *
 * @param m Module instance
 * @return true if song is active
 */
bool xfm_wav_song_is_playing(xfm_wav_module* m);

/**
 * @brief Mix audio from song module (song only, no SFX)
 *
 * @param m Module instance
 * @param stream Output buffer (interleaved stereo int16_t)
 * @param frames Number of stereo frames to generate
 */
void xfm_wav_mix_song(xfm_wav_module* m, int16_t* stream, int frames);

/**
 * @brief Mix audio from SFX module (SFX only, no song)
 *
 * @param m Module instance
 * @param stream Output buffer (interleaved stereo int16_t)
 * @param frames Number of stereo frames to generate
 */
void xfm_wav_mix_sfx(xfm_wav_module* m, int16_t* stream, int frames);

/**
 * @brief Mix audio from both song and SFX
 *
 * @param m Module instance
 * @param stream Output buffer (interleaved stereo int16_t)
 * @param frames Number of stereo frames to generate
 */
void xfm_wav_mix(xfm_wav_module* m, int16_t* stream, int frames);

#ifdef __cplusplus
}
#endif

#endif /* XFM_WAVPLAY_H */
