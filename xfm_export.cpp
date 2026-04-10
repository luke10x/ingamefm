// =============================================================================
// xfm_export.cpp — WAV Export Implementation
// =============================================================================

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdint>

// Include implementation first to access internal structures
#include "xfm_api.h"
#include "xfm_export.h"
// #include "xfm_impl.cpp"

// =============================================================================
// Minimal WAV Writer (no external dependencies)
// =============================================================================

static void write_le16(uint8_t* buf, uint16_t val) {
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
}

static void write_le32(uint8_t* buf, uint32_t val) {
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
    buf[2] = (val >> 16) & 0xFF;
    buf[3] = (val >> 24) & 0xFF;
}

static void write_wav_header(uint8_t* header, int sample_rate, int data_size)
{
    int num_channels = 2;  // Stereo
    int bits_per_sample = 16;
    int byte_rate = sample_rate * num_channels * bits_per_sample / 8;
    int block_align = num_channels * bits_per_sample / 8;
    int file_size = 36 + data_size;  // 44 byte header - 8 = 36

    memset(header, 0, 44);

    // RIFF header
    memcpy(header + 0, "RIFF", 4);
    write_le32(header + 4, file_size);
    memcpy(header + 8, "WAVE", 4);

    // fmt chunk
    memcpy(header + 12, "fmt ", 4);
    write_le32(header + 16, 16);  // Chunk size (16 for PCM)
    write_le16(header + 20, 1);   // Audio format (1 = PCM)
    write_le16(header + 22, num_channels);
    write_le32(header + 24, sample_rate);
    write_le32(header + 28, byte_rate);
    write_le16(header + 32, block_align);
    write_le16(header + 34, bits_per_sample);

    // data chunk
    memcpy(header + 36, "data", 4);
    write_le32(header + 40, data_size);
}

static int write_wav_file(const char* filename, int sample_rate, int num_samples, const int16_t* data)
{
    int data_size = num_samples * 2 * sizeof(int16_t);  // stereo

    // Allocate header (44 bytes)
    uint8_t header[44];
    write_wav_header(header, sample_rate, data_size);

    // Write file
    FILE* f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Failed to open %s for writing\n", filename);
        return -1;
    }

    // Write header
    if (fwrite(header, 1, 44, f) != 44) {
        fprintf(stderr, "Failed to write WAV header\n");
        fclose(f);
        return -1;
    }

    // Write audio data
    if (fwrite(data, 1, data_size, f) != (size_t)data_size) {
        fprintf(stderr, "Failed to write WAV data\n");
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

static void* create_wav_buffer(int sample_rate, int num_samples, const int16_t* data, int* outSize)
{
    int data_size = num_samples * 2 * sizeof(int16_t);  // stereo
    int total_size = 44 + data_size;

    uint8_t* buffer = (uint8_t*)malloc(total_size);
    if (!buffer) {
        fprintf(stderr, "Failed to allocate %d bytes for WAV buffer\n", total_size);
        return NULL;
    }

    write_wav_header(buffer, sample_rate, data_size);
    memcpy(buffer + 44, data, data_size);

    if (outSize) *outSize = total_size;
    return buffer;
}

// =============================================================================
// WAV FILE TO MEMORY
// =============================================================================

extern "C" void* xfm_wav_file_to_memory(const char* filename, int* outSize)
{
    if (!filename) return NULL;

    FILE* f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "xfm_wav_file_to_memory: Cannot open %s\n", filename);
        return NULL;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fprintf(stderr, "xfm_wav_file_to_memory: Empty or invalid file %s\n", filename);
        fclose(f);
        return NULL;
    }

    // Allocate buffer
    uint8_t* buffer = (uint8_t*)malloc(file_size);
    if (!buffer) {
        fprintf(stderr, "xfm_wav_file_to_memory: Failed to allocate %ld bytes\n", file_size);
        fclose(f);
        return NULL;
    }

    // Read file
    if (fread(buffer, 1, file_size, f) != (size_t)file_size) {
        fprintf(stderr, "xfm_wav_file_to_memory: Failed to read %s\n", filename);
        free(buffer);
        fclose(f);
        return NULL;
    }

    fclose(f);

    if (outSize) *outSize = (int)file_size;
    return buffer;
}

// =============================================================================
// SONG EXPORT
// =============================================================================

// Internal: render song to buffer, returns total samples rendered
// Matches reference exporter: renders with loop=true, trims silence, adds trailing silence
//
// CRITICAL: Do NOT call xfm_module_reset_state() here!
// The module is already fresh when passed in (created per-song in adaptive_audio.h).
// Calling reset_state AFTER xfm_song_declare() clears active_song state and keys off voices,
// causing gaps in the audio timeline. The song declaration sets up pattern data that
// must remain intact for proper rendering.
static int render_song_to_buffer(xfm_module* m, xfm_song_id song_id, int16_t* buffer, int max_samples)
{
    if (!m || !buffer || max_samples <= 0) return 0;
    if (song_id <= 0 || song_id > 15) return 0;

    int num_rows = xfm_song_get_total_rows(m, song_id);
    if (num_rows <= 0) return 0;

    int samples_per_row = m->song_patterns[song_id].samples_per_row;

    // Module is already fresh (created per-song in adaptive_audio.h)
    // Do NOT reset state here - it would clear chip state after song declaration

    // Play song with loop=true (matches reference exporter and real-time behavior)
    xfm_song_play(m, song_id, true);

    // Render in chunks - reference exporter uses 180 second max
    int frames_per_chunk = m->buffer_frames;
    int samples_rendered = 0;
    int max_render_samples = m->sample_rate * 180;  // 3 minute hard limit (matches reference)
    if (max_render_samples > max_samples) max_render_samples = max_samples;

    while (samples_rendered < max_render_samples) {
        int frames_to_render = frames_per_chunk;
        if (samples_rendered + frames_to_render > max_render_samples) {
            frames_to_render = max_render_samples - samples_rendered;
        }

        xfm_mix_song(m, buffer + (samples_rendered * 2), frames_to_render);
        samples_rendered += frames_to_render;
    }

    // Trim trailing silence (keep last 2 seconds of non-silent audio)
    int last_non_silent = samples_rendered - 1;
    while (last_non_silent > 0) {
        int idx = last_non_silent * 2;
        if (buffer[idx] > 100 || buffer[idx] < -100 ||
            buffer[idx + 1] > 100 || buffer[idx + 1] < -100) {
            break;
        }
        last_non_silent--;
    }
    samples_rendered = last_non_silent + 1;

    // Add 2 seconds of trailing silence (matches reference exporter)
    int trailing_silence = m->sample_rate * 2;
    int final_size = samples_rendered + trailing_silence;
    if (final_size > max_samples) final_size = max_samples;
    
    // Zero out trailing silence
    for (int i = samples_rendered * 2; i < final_size * 2; i++) {
        buffer[i] = 0;
    }

    xfm_song_play(m, 0, false);
    return final_size;
}

extern "C" int xfm_export_song(xfm_module* m, xfm_song_id song_id, const char* filename)
{
    if (!m || !filename) return -1;
    if (song_id <= 0 || song_id > 15) return -1;

    int num_rows = xfm_song_get_total_rows(m, song_id);
    if (num_rows <= 0) {
        fprintf(stderr, "xfm_export_song: Song %d not found or empty\n", song_id);
        return -1;
    }

    int samples_per_row = m->song_patterns[song_id].samples_per_row;
    int total_rows = num_rows + 2;
    int total_samples = total_rows * samples_per_row;

    int16_t* buffer = (int16_t*)malloc(total_samples * 2 * sizeof(int16_t));
    if (!buffer) {
        fprintf(stderr, "xfm_export_song: Failed to allocate buffer\n");
        return -1;
    }
    memset(buffer, 0, total_samples * 2 * sizeof(int16_t));

    int samples_rendered = render_song_to_buffer(m, song_id, buffer, total_samples);
    if (samples_rendered <= 0) {
        free(buffer);
        fprintf(stderr, "xfm_export_song: Failed to render song %d\n", song_id);
        return -1;
    }

    int result = write_wav_file(filename, m->sample_rate, samples_rendered, buffer);
    free(buffer);

    if (result < 0) return -1;

    printf("Exported song %d to %s (%d samples, %d Hz, %.2f seconds)\n",
           song_id, filename, samples_rendered, m->sample_rate,
           (float)samples_rendered / m->sample_rate);

    return 0;
}

extern "C" void* xfm_export_song_to_memory(xfm_module* m, xfm_song_id song_id, int* outSize)
{
    if (!m || !outSize) return NULL;
    if (song_id <= 0 || song_id > 15) return NULL;

    int num_rows = xfm_song_get_total_rows(m, song_id);
    if (num_rows <= 0) {
        fprintf(stderr, "xfm_export_song_to_memory: Song %d not found or empty\n", song_id);
        return NULL;
    }

    int samples_per_row = m->song_patterns[song_id].samples_per_row;
    int total_rows = num_rows + 2;
    int total_samples = total_rows * samples_per_row;

    int16_t* buffer = (int16_t*)malloc(total_samples * 2 * sizeof(int16_t));
    if (!buffer) {
        fprintf(stderr, "xfm_export_song_to_memory: Failed to allocate buffer\n");
        return NULL;
    }
    memset(buffer, 0, total_samples * 2 * sizeof(int16_t));

    int samples_rendered = render_song_to_buffer(m, song_id, buffer, total_samples);
    if (samples_rendered <= 0) {
        free(buffer);
        fprintf(stderr, "xfm_export_song_to_memory: Failed to render song %d\n", song_id);
        return NULL;
    }

    void* wav_buffer = create_wav_buffer(m->sample_rate, samples_rendered, buffer, outSize);
    free(buffer);

    if (wav_buffer) {
        printf("Exported song %d to memory (%d bytes, %d samples, %d Hz, %.2f seconds)\n",
               song_id, *outSize, samples_rendered, m->sample_rate,
               (float)samples_rendered / m->sample_rate);
    }

    return wav_buffer;
}

// =============================================================================
// SFX EXPORT
// =============================================================================

// Internal: render SFX to buffer, returns total samples rendered
//
// CRITICAL: Do NOT call xfm_module_reset_state() here!
// The module state is managed by the caller (adaptive_audio.h resets before each SFX export).
// Calling reset_state AFTER xfm_sfx_declare() clears the SFX pattern state and voice allocation,
// resulting in silent or corrupted output.
static int render_sfx_to_buffer(xfm_module* m, int sfx_id, int16_t* buffer, int max_samples)
{
    if (!m || !buffer || max_samples <= 0) return 0;
    if (sfx_id < 0 || sfx_id > 255) return 0;
    if (!m->sfx_present[sfx_id]) return 0;

    int num_rows = m->sfx_patterns[sfx_id].num_rows;
    int samples_per_row = m->sfx_patterns[sfx_id].samples_per_row;
    int total_rows = num_rows + 4;  // Extra rows for release
    int total_samples = total_rows * samples_per_row;
    if (total_samples > max_samples) total_samples = max_samples;

    printf("[SFX Export] SFX %d: %d rows, %d samples/row, total=%d samples, auto_off=%.2f\n",
           sfx_id, num_rows, samples_per_row, total_samples, m->auto_off_delay);

    // Module is already fresh (reset before each SFX in adaptive_audio.h)
    // Do NOT reset state here - it would clear chip state after SFX declaration

    // Directly set up SFX on voice 0 without going through xfm_sfx_play()
    // This avoids voice stealing, priority logic, and state corruption
    int voice = 0;
    m->voices[voice].active = true;
    m->voices[voice].sfx_id = sfx_id;
    m->voices[voice].priority = 0;
    m->voices[voice].age = ++m->voice_age_counter;
    m->voices[voice].midi_note = -1;
    m->voices[voice].patch_id = -1;
    m->channel_active[voice] = false;
    
    m->active_sfx[voice].sfx_id = sfx_id;
    m->active_sfx[voice].priority = 0;
    m->active_sfx[voice].voice_idx = voice;
    m->active_sfx[voice].current_row = 0;
    m->active_sfx[voice].sample_in_row = 0;
    m->active_sfx[voice].rows_remaining = num_rows;
    m->active_sfx[voice].last_patch_id = -1;
    m->active_sfx[voice].pending_has_note = false;
    m->active_sfx[voice].pending_note = -1;
    m->active_sfx[voice].pending_patch_id = -1;
    m->active_sfx[voice].pending_gap = 0;  // No gap for export - render immediately
    m->active_sfx[voice].active = true;
    m->active_sfx[voice].auto_off_scheduled = false;
    m->active_sfx[voice].auto_off_at_sample = 0;

    // Process first row immediately to set up pending note
    // (sfx_process_row is internal to xfm_impl.cpp, we need to call it via xfm_mix_sfx)
    
    // Render in chunks
    int frames_per_chunk = m->buffer_frames;
    int samples_rendered = 0;

    while (samples_rendered < total_samples) {
        int frames_to_render = frames_per_chunk;
        if (samples_rendered + frames_to_render > total_samples) {
            frames_to_render = total_samples - samples_rendered;
        }

        xfm_mix_sfx(m, buffer + (samples_rendered * 2), frames_to_render);
        samples_rendered += frames_to_render;

        // Don't break early - render full buffer including release tail
    }

    printf("[SFX Export] SFX %d: rendered %d samples (%.2f ms)\n", 
           sfx_id, samples_rendered, (float)samples_rendered / m->sample_rate * 1000.0f);
    return samples_rendered;
}

extern "C" int xfm_export_sfx(xfm_module* m, int sfx_id, const char* filename)
{
    if (!m || !filename) return -1;
    if (sfx_id < 0 || sfx_id > 255) return -1;
    if (!m->sfx_present[sfx_id]) {
        fprintf(stderr, "xfm_export_sfx: SFX %d not found\n", sfx_id);
        return -1;
    }

    int num_rows = m->sfx_patterns[sfx_id].num_rows;
    int samples_per_row = m->sfx_patterns[sfx_id].samples_per_row;
    int total_rows = num_rows + 4;
    int total_samples = total_rows * samples_per_row;

    int16_t* buffer = (int16_t*)malloc(total_samples * 2 * sizeof(int16_t));
    if (!buffer) {
        fprintf(stderr, "xfm_export_sfx: Failed to allocate buffer\n");
        return -1;
    }
    memset(buffer, 0, total_samples * 2 * sizeof(int16_t));

    int samples_rendered = render_sfx_to_buffer(m, sfx_id, buffer, total_samples);
    if (samples_rendered <= 0) {
        free(buffer);
        fprintf(stderr, "xfm_export_sfx: Failed to render SFX %d\n", sfx_id);
        return -1;
    }

    int result = write_wav_file(filename, m->sample_rate, samples_rendered, buffer);
    free(buffer);

    if (result < 0) return -1;

    printf("Exported SFX %d to %s (%d samples, %d Hz, %.2f seconds)\n",
           sfx_id, filename, samples_rendered, m->sample_rate,
           (float)samples_rendered / m->sample_rate);

    return 0;
}

extern "C" void* xfm_export_sfx_to_memory(xfm_module* m, int sfx_id, int* outSize)
{
    if (!m || !outSize) return NULL;
    if (sfx_id < 0 || sfx_id > 255) return NULL;
    if (!m->sfx_present[sfx_id]) {
        fprintf(stderr, "xfm_export_sfx_to_memory: SFX %d not found\n", sfx_id);
        return NULL;
    }

    int num_rows = m->sfx_patterns[sfx_id].num_rows;
    int samples_per_row = m->sfx_patterns[sfx_id].samples_per_row;
    int total_rows = num_rows + 4;
    int total_samples = total_rows * samples_per_row;

    int16_t* buffer = (int16_t*)malloc(total_samples * 2 * sizeof(int16_t));
    if (!buffer) {
        fprintf(stderr, "xfm_export_sfx_to_memory: Failed to allocate buffer\n");
        return NULL;
    }
    memset(buffer, 0, total_samples * 2 * sizeof(int16_t));

    int samples_rendered = render_sfx_to_buffer(m, sfx_id, buffer, total_samples);
    if (samples_rendered <= 0) {
        free(buffer);
        fprintf(stderr, "xfm_export_sfx_to_memory: Failed to render SFX %d\n", sfx_id);
        return NULL;
    }

    void* wav_buffer = create_wav_buffer(m->sample_rate, samples_rendered, buffer, outSize);
    free(buffer);

    if (wav_buffer) {
        printf("Exported SFX %d to memory (%d bytes, %d samples, %d Hz, %.2f seconds)\n",
               sfx_id, *outSize, samples_rendered, m->sample_rate,
               (float)samples_rendered / m->sample_rate);
    }

    return wav_buffer;
}

// =============================================================================
// YIELDABLE EXPORT IMPLEMENTATION
// =============================================================================

// --- Song Export ---

int xfm_export_song_begin(xfm_export_song_state* state, xfm_module* m, xfm_song_id song_id, int samples_per_chunk)
{
    if (!state || !m) return -1;
    if (song_id <= 0 || song_id > 15) return -1;

    int num_rows = xfm_song_get_total_rows(m, song_id);
    if (num_rows <= 0) return -1;

    int samples_per_row = m->song_patterns[song_id].samples_per_row;
    int total_rows = num_rows + 2;
    int total_samples = total_rows * samples_per_row;

    // Allocate intermediate render buffer
    int16_t* buffer = (int16_t*)malloc(total_samples * 2 * sizeof(int16_t));
    if (!buffer) {
        fprintf(stderr, "xfm_export_song_begin: Failed to allocate buffer\n");
        return -1;
    }
    memset(buffer, 0, total_samples * 2 * sizeof(int16_t));

    // Start song playback
    xfm_song_play(m, song_id, true);

    // Initialize state
    state->module = m;
    state->song_id = song_id;
    state->render_buffer = buffer;
    state->max_samples = total_samples;
    state->samples_per_chunk = samples_per_chunk;
    state->samples_rendered = 0;
    state->total_samples = total_samples;
    state->phase = 0;  // rendering
    state->last_non_silent = 0;
    state->done = false;
    state->failed = false;
    state->progress = 0.0f;
    state->wav_buffer = NULL;
    state->max_render_samples = m->sample_rate * 180;  // 3 minute hard limit
    if (state->max_render_samples > total_samples) state->max_render_samples = total_samples;
    state->wav_size = 0;

    return 0;
}

int xfm_export_song_step(xfm_export_song_state* state)
{
    if (!state || !state->module || state->done || state->failed) return -1;

    if (state->phase == 0) {
        // Rendering phase: render up to samples_per_chunk samples per call,
        // but internally use buffer_frames chunks to match original exporter behavior
        int frames_per_chunk = state->module->buffer_frames;
        int target_frames = state->samples_per_chunk;
        int rendered_this_step = 0;

        while (rendered_this_step < target_frames) {
            int frames_to_render = frames_per_chunk;
            int remaining = state->max_render_samples - state->samples_rendered;
            int target_remaining = target_frames - rendered_this_step;
            if (frames_to_render > remaining) frames_to_render = remaining;
            if (frames_to_render > target_remaining) frames_to_render = target_remaining;
            if (frames_to_render <= 0) {
                // All done rendering, move to trimming
                state->phase = 1;
                goto do_trim;
            }

            xfm_mix_song(state->module, state->render_buffer + (state->samples_rendered * 2), frames_to_render);
            state->samples_rendered += frames_to_render;
            rendered_this_step += frames_to_render;
        }

        if (state->samples_rendered >= state->max_render_samples) {
            state->phase = 1;
        }

do_trim:
        if (state->phase == 1) {
            // Trimming phase: find last non-silent sample
            state->last_non_silent = state->samples_rendered - 1;
            while (state->last_non_silent > 0) {
                int idx = state->last_non_silent * 2;
                if (state->render_buffer[idx] > 100 || state->render_buffer[idx] < -100 ||
                    state->render_buffer[idx + 1] > 100 || state->render_buffer[idx + 1] < -100) {
                    break;
                }
                state->last_non_silent--;
            }
            state->phase = 2;
        }

        if (state->phase == 2) {
            // Trailing silence phase: add 2 seconds of silence
            int trailing_silence = state->module->sample_rate * 2;
            int final_size = state->last_non_silent + 1 + trailing_silence;
            if (final_size > state->max_samples) final_size = state->max_samples;

            for (int i = (state->last_non_silent + 1) * 2; i < final_size * 2; i++) {
                state->render_buffer[i] = 0;
            }
            state->samples_rendered = final_size;

            xfm_song_play(state->module, 0, false);

            state->done = true;
            state->progress = 1.0f;
        }

        state->progress = (float)state->samples_rendered / state->max_render_samples * 0.8f;
        return 0;
    }

    if (state->phase == 1) {
        state->last_non_silent = state->samples_rendered - 1;
        while (state->last_non_silent > 0) {
            int idx = state->last_non_silent * 2;
            if (state->render_buffer[idx] > 100 || state->render_buffer[idx] < -100 ||
                state->render_buffer[idx + 1] > 100 || state->render_buffer[idx + 1] < -100) {
                break;
            }
            state->last_non_silent--;
        }
        state->phase = 2;
        return 0;
    }

    if (state->phase == 2) {
        int trailing_silence = state->module->sample_rate * 2;
        int final_size = state->last_non_silent + 1 + trailing_silence;
        if (final_size > state->max_samples) final_size = state->max_samples;

        for (int i = (state->last_non_silent + 1) * 2; i < final_size * 2; i++) {
            state->render_buffer[i] = 0;
        }
        state->samples_rendered = final_size;

        xfm_song_play(state->module, 0, false);

        state->done = true;
        state->progress = 1.0f;
        return 0;
    }

    return -1;
}
void* xfm_export_song_finalize(xfm_export_song_state* state, int* outSize)
{
    if (!state || !state->done) return NULL;

    void* wav_buffer = create_wav_buffer(state->module->sample_rate, state->samples_rendered, state->render_buffer, outSize);
    if (wav_buffer) {
        state->wav_buffer = wav_buffer;
        state->wav_size = *outSize;
        printf("Exported song %d to memory (%d bytes, %d samples, %d Hz, %.2f seconds)\n",
               state->song_id, *outSize, state->samples_rendered, state->module->sample_rate,
               (float)state->samples_rendered / state->module->sample_rate);
    }
    return wav_buffer;
}

void xfm_export_song_cleanup(xfm_export_song_state* state)
{
    if (!state) return;
    if (state->render_buffer) {
        free(state->render_buffer);
        state->render_buffer = NULL;
    }
    // Note: wav_buffer is NOT freed here - caller owns it after finalize
}

// --- SFX Export ---

int xfm_export_sfx_begin(xfm_export_sfx_state* state, xfm_module* m, int sfx_id, int samples_per_chunk)
{
    if (!state || !m) return -1;
    if (sfx_id < 0 || sfx_id > 255) return -1;
    if (!m->sfx_present[sfx_id]) return -1;

    int num_rows = m->sfx_patterns[sfx_id].num_rows;
    int samples_per_row = m->sfx_patterns[sfx_id].samples_per_row;
    int total_rows = num_rows + 4;  // Extra rows for release
    int total_samples = total_rows * samples_per_row;

    // Allocate intermediate render buffer
    int16_t* buffer = (int16_t*)malloc(total_samples * 2 * sizeof(int16_t));
    if (!buffer) {
        fprintf(stderr, "xfm_export_sfx_begin: Failed to allocate buffer\n");
        return -1;
    }
    memset(buffer, 0, total_samples * 2 * sizeof(int16_t));

    // Set up SFX on voice 0 (matches render_sfx_to_buffer)
    int voice = 0;
    m->voices[voice].active = true;
    m->voices[voice].sfx_id = sfx_id;
    m->voices[voice].priority = 0;
    m->voices[voice].age = ++m->voice_age_counter;
    m->voices[voice].midi_note = -1;
    m->voices[voice].patch_id = -1;
    m->channel_active[voice] = false;

    m->active_sfx[voice].sfx_id = sfx_id;
    m->active_sfx[voice].priority = 0;
    m->active_sfx[voice].voice_idx = voice;
    m->active_sfx[voice].current_row = 0;
    m->active_sfx[voice].sample_in_row = 0;
    m->active_sfx[voice].rows_remaining = num_rows;
    m->active_sfx[voice].last_patch_id = -1;
    m->active_sfx[voice].pending_has_note = false;
    m->active_sfx[voice].pending_note = -1;
    m->active_sfx[voice].pending_patch_id = -1;
    m->active_sfx[voice].pending_gap = 0;
    m->active_sfx[voice].active = true;
    m->active_sfx[voice].auto_off_scheduled = false;
    m->active_sfx[voice].auto_off_at_sample = 0;

    // Initialize state
    state->module = m;
    state->sfx_id = sfx_id;
    state->render_buffer = buffer;
    state->max_samples = total_samples;
    state->samples_per_chunk = samples_per_chunk;
    state->samples_rendered = 0;
    state->total_samples = total_samples;
    state->done = false;
    state->failed = false;
    state->progress = 0.0f;
    state->wav_buffer = NULL;
    state->wav_size = 0;

    return 0;
}

int xfm_export_sfx_step(xfm_export_sfx_state* state)
{
    if (!state || !state->module || state->done || state->failed) return -1;

    // Render up to samples_per_chunk samples per call,
    // but internally use buffer_frames chunks to match original exporter behavior
    int frames_per_chunk = state->module->buffer_frames;
    int target_frames = state->samples_per_chunk;
    int rendered_this_step = 0;

    while (rendered_this_step < target_frames) {
        int frames_to_render = frames_per_chunk;
        int remaining = state->total_samples - state->samples_rendered;
        int target_remaining = target_frames - rendered_this_step;
        if (frames_to_render > remaining) frames_to_render = remaining;
        if (frames_to_render > target_remaining) frames_to_render = target_remaining;
        if (frames_to_render <= 0) {
            state->done = true;
            state->progress = 1.0f;
            return 0;
        }

        xfm_mix_sfx(state->module, state->render_buffer + (state->samples_rendered * 2), frames_to_render);
        state->samples_rendered += frames_to_render;
        rendered_this_step += frames_to_render;
    }

    state->progress = (float)state->samples_rendered / state->total_samples;

    if (state->samples_rendered >= state->total_samples) {
        state->done = true;
        state->progress = 1.0f;
    }

    return 0;
}
void* xfm_export_sfx_finalize(xfm_export_sfx_state* state, int* outSize)
{
    if (!state || !state->done) return NULL;

    void* wav_buffer = create_wav_buffer(state->module->sample_rate, state->samples_rendered, state->render_buffer, outSize);
    if (wav_buffer) {
        state->wav_buffer = wav_buffer;
        state->wav_size = *outSize;
        printf("Exported SFX %d to memory (%d bytes, %d samples, %d Hz, %.2f seconds)\n",
               state->sfx_id, *outSize, state->samples_rendered, state->module->sample_rate,
               (float)state->samples_rendered / state->module->sample_rate);
    }
    return wav_buffer;
}

void xfm_export_sfx_cleanup(xfm_export_sfx_state* state)
{
    if (!state) return;
    if (state->render_buffer) {
        free(state->render_buffer);
        state->render_buffer = NULL;
    }
}
