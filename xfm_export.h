/**
 * =============================================================================
 * xfm_export.h — WAV Export for eggsfm
 * =============================================================================
 *
 * Export songs and SFX patterns to WAV files.
 * Includes minimal WAV writer (no external dependencies).
 *
 * Usage:
 *   xfm_module* m = xfm_module_create(44100, 256, XFM_CHIP_YM2612);
 *   // ... add patches, declare songs/SFX ...
 *   xfm_export_song(m, song_id, "output.wav");
 *   xfm_export_sfx(m, sfx_id, "sfx.wav");
 */

#ifndef XFM_EXPORT_H
#define XFM_EXPORT_H

#include "xfm_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load a WAV file into memory as an embedded array.
 *
 * Helper function to load WAV data for use with xfm_wav_load_memory().
 * Returns malloc'd buffer that caller must free().
 *
 * @param filename Path to WAV file
 * @param outSize Receives the file size
 * @return Pointer to WAV data, or NULL on error
 */
void* xfm_wav_file_to_memory(const char* filename, int* outSize);

/**
 * @brief Export a song pattern to a WAV file.
 *
 * Renders the entire song (all rows) to a stereo WAV file.
 * The song is rendered at the module's sample rate.
 *
 * @param m Module instance (must have song declared)
 * @param song_id Song ID to export (1-15)
 * @param filename Output WAV file path
 * @return 0 on success, -1 on error
 *
 * @note File will be overwritten if it exists
 * @note Song loops are NOT applied - exports exactly num_rows
 */
int xfm_export_song(xfm_module* m, xfm_song_id song_id, const char* filename);

/**
 * @brief Export a song pattern to a WAV buffer in memory.
 *
 * Renders the entire song (all rows) to a stereo WAV buffer.
 * The song is rendered at the module's sample rate.
 *
 * @param m Module instance (must have song declared)
 * @param song_id Song ID to export (1-15)
 * @param outSize Receives the size of the WAV data in bytes
 * @return Pointer to malloc'd WAV data, or NULL on error. Caller must free().
 *
 * @note Song loops are NOT applied - exports exactly num_rows
 */
void* xfm_export_song_to_memory(xfm_module* m, xfm_song_id song_id, int* outSize);

/**
 * @brief Export an SFX pattern to a WAV file.
 *
 * Renders the entire SFX pattern to a stereo WAV file.
 * The SFX is rendered at the module's sample rate.
 *
 * @param m Module instance (must have SFX declared)
 * @param sfx_id SFX ID to export (0-255)
 * @param filename Output WAV file path
 * @return 0 on success, -1 on error
 *
 * @note File will be overwritten if it exists
 */
int xfm_export_sfx(xfm_module* m, int sfx_id, const char* filename);

/**
 * @brief Export an SFX pattern to a WAV buffer in memory.
 *
 * Renders the entire SFX pattern to a stereo WAV buffer.
 * The SFX is rendered at the module's sample rate.
 *
 * @param m Module instance (must have SFX declared)
 * @param sfx_id SFX ID to export (0-255)
 * @param outSize Receives the size of the WAV data in bytes
 * @return Pointer to malloc'd WAV data, or NULL on error. Caller must free().
 */
void* xfm_export_sfx_to_memory(xfm_module* m, int sfx_id, int* outSize);

#ifdef __cplusplus
}
#endif


// =============================================================================
// Yieldable Export API - for non-blocking export in single-threaded environments
// =============================================================================

/**
 * @brief Number of samples to render per yield step.
 * Configurable - smaller values yield more frequently but add overhead.
 * At 44100 Hz, 4410 samples = 100ms of audio per step.
 */
#ifndef XFM_EXPORT_YIELD_SAMPLES
#define XFM_EXPORT_YIELD_SAMPLES 4410
#endif

/**
 * @brief State for yieldable song export.
 * Allocate on heap or stack, pass to all yieldable export functions.
 */
typedef struct {
    // Public (read-only after begin)
    int samples_rendered;    // How many samples rendered so far
    int total_samples;       // Total samples to render
    bool done;               // True when all steps complete
    bool failed;             // True if export failed
    float progress;          // 0.0 to 1.0

    // Internal state
    xfm_module* module;
    int song_id;
    int16_t* render_buffer;      // Intermediate stereo sample buffer
    int max_samples;
    int samples_per_chunk;       // How many samples to render per step
    int phase;                   // 0=rendering, 1=trimming, 2=trailing silence
    int last_non_silent;         // For silence trimming
    int max_render_samples;      // Hard limit for rendering (sample_rate * 180)
    int wav_size;                // Output WAV buffer size
    void* wav_buffer;            // Final WAV buffer (malloc'd)
} xfm_export_song_state;

/**
 * @brief State for yieldable SFX export.
 */
typedef struct {
    // Public (read-only after begin)
    int samples_rendered;
    int total_samples;
    bool done;
    bool failed;
    float progress;

    // Internal state
    xfm_module* module;
    int sfx_id;
    int16_t* render_buffer;
    int max_samples;
    int samples_per_chunk;
    int samples_rendered_total;
    int wav_size;
    void* wav_buffer;
} xfm_export_sfx_state;

/**
 * @brief Initialize yieldable song export.
 * Allocates intermediate buffer, sets up song playback.
 * Call once before calling xfm_export_song_step().
 * 
 * @param state Export state (must be zero-initialized or fresh)
 * @param m Module instance
 * @param song_id Song ID to export
 * @param samples_per_chunk How many samples to render per step (use XFM_EXPORT_YIELD_SAMPLES)
 * @return 0 on success, -1 on error
 */
int xfm_export_song_begin(xfm_export_song_state* state, xfm_module* m, xfm_song_id song_id, int samples_per_chunk);

/**
 * @brief Render one chunk of the song.
 * Call repeatedly until state->done is true.
 * Each call renders up to samples_per_chunk samples.
 * 
 * @param state Export state
 * @return 0 while still rendering, 0 when done, -1 on error
 */
int xfm_export_song_step(xfm_export_song_state* state);

/**
 * @brief Finalize song export.
 * Creates the final WAV buffer. Call after xfm_export_song_step returns done.
 * 
 * @param state Export state
 * @param outSize Receives the size of the WAV data
 * @return Pointer to malloc'd WAV data, or NULL on error
 */
void* xfm_export_song_finalize(xfm_export_song_state* state, int* outSize);

/**
 * @brief Clean up yieldable song export resources.
 * Frees the intermediate render buffer. Call after finalize (or on error).
 */
void xfm_export_song_cleanup(xfm_export_song_state* state);

/**
 * @brief Initialize yieldable SFX export.
 */
int xfm_export_sfx_begin(xfm_export_sfx_state* state, xfm_module* m, int sfx_id, int samples_per_chunk);

/**
 * @brief Render one chunk of the SFX.
 */
int xfm_export_sfx_step(xfm_export_sfx_state* state);

/**
 * @brief Finalize SFX export.
 */
void* xfm_export_sfx_finalize(xfm_export_sfx_state* state, int* outSize);

/**
 * @brief Clean up yieldable SFX export resources.
 */
void xfm_export_sfx_cleanup(xfm_export_sfx_state* state);

#endif /* XFM_EXPORT_H */
