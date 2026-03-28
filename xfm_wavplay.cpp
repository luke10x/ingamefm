// =============================================================================
// xfm_wavplay.cpp — WAV Playback Implementation (pre-rendered alternative)
// =============================================================================

#include "xfm_wavplay.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <vector>

// =============================================================================
// WAV File Parser
// =============================================================================

struct WavInfo {
    int sample_rate;
    int num_channels;
    int bits_per_sample;
    int num_samples;
    int data_offset;  // Offset to data in original buffer
    const uint8_t* raw_data;  // Raw buffer pointer
};

static bool parse_wav_header(const uint8_t* data, int size, WavInfo& info)
{
    if (size < 44) return false;
    
    // Check RIFF header
    if (memcmp(data, "RIFF", 4) != 0) return false;
    if (memcmp(data + 8, "WAVE", 4) != 0) return false;
    
    // Find fmt chunk
    int pos = 12;
    int fmt_pos = 0;  // Position of fmt chunk data
    int fmt_size = 0;
    while (pos + 8 <= size) {
        uint32_t chunk_size = data[pos + 4] | (data[pos + 5] << 8) | 
                              (data[pos + 6] << 16) | (data[pos + 7] << 24);
        if (memcmp(data + pos, "fmt ", 4) == 0) {
            fmt_size = (int)chunk_size;
            fmt_pos = pos + 8;  // Position of fmt data
            break;
        }
        pos += 8 + (int)chunk_size;
        if (chunk_size & 1) pos++;  // Word alignment
        if (pos > size) return false;
    }
    
    if (fmt_size < 16) return false;
    
    // Parse fmt chunk (little-endian) - check bounds for each access
    pos = fmt_pos;
    if (pos + 16 > size) return false;
    
    // WAV fmt chunk structure:
    // 0-1: wFormatTag, 2-3: wChannels, 4-7: dwSamplesPerSec, 8-11: dwAvgBytesPerSec
    // 12-13: wBlockAlign, 14-15: wBitsPerSample
    info.num_channels = data[pos + 2] | (data[pos + 3] << 8);
    info.sample_rate = data[pos + 4] | (data[pos + 5] << 8) | 
                       (data[pos + 6] << 16) | (data[pos + 7] << 24);
    info.bits_per_sample = data[pos + 14] | (data[pos + 15] << 8);
    
    // Continue searching for data chunk from after fmt chunk
    pos = fmt_pos + fmt_size;
    if (fmt_size & 1) pos++;  // Word alignment
    
    // Find data chunk
    while (pos + 8 <= size) {
        uint32_t chunk_size = data[pos + 4] | (data[pos + 5] << 8) | 
                              (data[pos + 6] << 16) | (data[pos + 7] << 24);
        if (memcmp(data + pos, "data", 4) == 0) {
            if (pos + 8 > size) return false;
            info.num_samples = (int)(chunk_size / (info.num_channels * info.bits_per_sample / 8));
            info.data_offset = pos + 8;
            info.raw_data = data;
            return true;
        }
        pos += 8 + (int)chunk_size;
        if (chunk_size & 1) pos++;  // Word alignment
    }
    
    return false;
}

// =============================================================================
// Internal Structures
// =============================================================================

struct XfmWavContent {
    int16_t* data;          // Audio data (interleaved stereo)
    int num_samples;        // Total samples
    int sample_rate;        // Sample rate
    bool owned;             // Do we own the data?
};

struct XfmWavActiveSfx {
    int sfx_id;             // Which SFX is playing
    int priority;           // Priority level
    int sample_pos;         // Current sample position
    bool active;            // Is currently playing
};

struct XfmWavActiveSong {
    int song_id;            // Current song ID (0 = none)
    int sample_pos;         // Current sample position
    bool active;            // Is currently playing
    bool loop;              // Loop at end
    int pending_song_id;    // Pending song change
    xfm_wav_switch_timing pending_timing;
    bool pending_set;       // Is pending change set?
};

struct xfm_wav_module {
    // Configuration
    int sample_rate;
    int buffer_frames;
    
    // Song WAVs (1-15)
    XfmWavContent songs[16];
    bool song_present[16];
    
    // SFX WAVs (0-255)
    XfmWavContent* sfx;     // Array of 256
    bool* sfx_present;      // Array of 256
    
    // Active song
    XfmWavActiveSong active_song;
    
    // Active SFX voices (up to 6 concurrent)
    XfmWavActiveSfx active_sfx[6];
};

// =============================================================================
// Module Lifetime
// =============================================================================

xfm_wav_module* xfm_wav_module_create(int sample_rate, int buffer_frames)
{
    xfm_wav_module* m = new (std::nothrow) xfm_wav_module;
    if (!m) return nullptr;
    
    m->sample_rate = sample_rate;
    m->buffer_frames = buffer_frames;
    
    // Initialize songs
    std::memset(m->songs, 0, sizeof(m->songs));
    std::memset(m->song_present, 0, sizeof(m->song_present));
    
    // Initialize SFX
    m->sfx = new (std::nothrow) XfmWavContent[256];
    m->sfx_present = new (std::nothrow) bool[256];
    if (!m->sfx || !m->sfx_present) {
        delete[] m->sfx;
        delete[] m->sfx_present;
        delete m;
        return nullptr;
    }
    std::memset(m->sfx, 0, sizeof(XfmWavContent) * 256);
    std::memset(m->sfx_present, 0, sizeof(bool) * 256);
    
    // Initialize active song
    m->active_song.song_id = 0;
    m->active_song.sample_pos = 0;
    m->active_song.active = false;
    m->active_song.loop = false;
    m->active_song.pending_song_id = 0;
    m->active_song.pending_timing = XFM_WAV_SWITCH_NOW;
    m->active_song.pending_set = false;
    
    // Initialize SFX voices
    for (int i = 0; i < 6; i++) {
        m->active_sfx[i].sfx_id = -1;
        m->active_sfx[i].priority = 0;
        m->active_sfx[i].sample_pos = 0;
        m->active_sfx[i].active = false;
    }
    
    return m;
}

void xfm_wav_module_destroy(xfm_wav_module* m)
{
    if (!m) return;
    
    // Unload all songs
    for (int i = 1; i <= 15; i++) {
        xfm_wav_unload(m, XFM_WAV_SONG, i);
    }
    
    // Unload all SFX
    for (int i = 0; i < 256; i++) {
        xfm_wav_unload(m, XFM_WAV_SFX, i);
    }
    
    delete[] m->sfx;
    delete[] m->sfx_present;
    delete m;
}

// =============================================================================
// WAV Loading
// =============================================================================

int xfm_wav_load_file(xfm_wav_module* m, xfm_wav_type type, int id, const char* filename)
{
    if (!m || !filename) return -1;
    
    // Read file using vector for safety
    FILE* f = fopen(filename, "rb");
    if (!f) return -1;
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size > 100 * 1024 * 1024) {
        fclose(f);
        return -1;
    }
    
    // Use vector with no initialization for speed
    std::vector<uint8_t> data;
    data.resize(file_size);
    
    size_t read_size = fread(data.data(), 1, file_size, f);
    fclose(f);
    
    if (read_size != (size_t)file_size) return -1;
    
    // Load from memory
    return xfm_wav_load_memory(m, type, id, data.data(), (int)file_size, true);
}

int xfm_wav_load_memory(xfm_wav_module* m, xfm_wav_type type, int id,
                        const void* data, int size, bool /*copy_data*/)
{
    if (!m || !data || size < 44) return -1;
    
    // Validate ID range
    if (type == XFM_WAV_SONG && (id < 1 || id > 15)) return -1;
    if (type == XFM_WAV_SFX && (id < 0 || id > 255)) return -1;
    
    // Parse WAV header
    WavInfo info;
    memset(&info, 0, sizeof(info));
    if (!parse_wav_header((const uint8_t*)data, size, info)) {
        return -1;
    }
    
    // Check format (we need 16-bit stereo)
    if (info.bits_per_sample != 16) {
        fprintf(stderr, "xfm_wav_load_memory: Only 16-bit WAV supported (got %d)\n", 
                info.bits_per_sample);
        return -1;
    }
    
    if (info.num_channels != 2) {
        fprintf(stderr, "xfm_wav_load_memory: Only stereo WAV supported (got %d channels)\n",
                info.num_channels);
        return -1;
    }
    
    // Get target content array
    XfmWavContent* content = nullptr;
    if (type == XFM_WAV_SONG) {
        content = &m->songs[id];
    } else {
        content = &m->sfx[id];
    }
    
    // Unload existing content
    if (content->data && content->owned) {
        free(content->data);
    }
    
    // Always copy the audio data (not the header)
    int16_t* new_data = (int16_t*)malloc(info.num_samples * 2 * sizeof(int16_t));
    if (!new_data) return -1;
    
    // Copy byte-by-byte to avoid alignment issues
    const uint8_t* src = info.raw_data + info.data_offset;
    for (int i = 0; i < info.num_samples * 2; i++) {
        new_data[i] = (int16_t)(src[i * 2] | (src[i * 2 + 1] << 8));
    }
    
    // Store content
    content->data = new_data;
    content->num_samples = info.num_samples;
    content->sample_rate = info.sample_rate;
    content->owned = true;
    
    // Mark as present
    if (type == XFM_WAV_SONG) {
        m->song_present[id] = true;
    } else {
        m->sfx_present[id] = true;
    }
    
    return 0;
}

void xfm_wav_unload(xfm_wav_module* m, xfm_wav_type type, int id)
{
    if (!m) return;
    
    XfmWavContent* content = nullptr;
    if (type == XFM_WAV_SONG) {
        if (id < 1 || id > 15) return;
        content = &m->songs[id];
        m->song_present[id] = false;
    } else {
        if (id < 0 || id > 255) return;
        content = &m->sfx[id];
        m->sfx_present[id] = false;
    }
    
    if (content->data && content->owned) {
        free(content->data);
    }
    
    content->data = nullptr;
    content->num_samples = 0;
    content->sample_rate = 0;
    content->owned = false;
}

// =============================================================================
// Song Playback
// =============================================================================

void xfm_wav_song_play(xfm_wav_module* m, int song_id, bool loop)
{
    if (!m || song_id < 1 || song_id > 15) return;
    if (!m->song_present[song_id]) return;
    
    m->active_song.song_id = song_id;
    m->active_song.sample_pos = 0;
    m->active_song.active = true;
    m->active_song.loop = loop;
    m->active_song.pending_set = false;
}

void xfm_wav_song_schedule(xfm_wav_module* m, int song_id, xfm_wav_switch_timing timing)
{
    if (!m || song_id < 1 || song_id > 15) return;
    if (!m->song_present[song_id]) return;
    
    m->active_song.pending_song_id = song_id;
    m->active_song.pending_timing = timing;
    m->active_song.pending_set = true;
}

int xfm_wav_song_get_row(xfm_wav_module* m)
{
    if (!m || !m->active_song.active) return 0;
    
    // Estimate row from sample position
    // This is approximate since we don't have row data in WAV
    XfmWavContent& song = m->songs[m->active_song.song_id];
    if (song.num_samples == 0) return 0;
    
    // Assume ~100ms per row (typical for speed=6, tick_rate=60)
    int samples_per_row = m->sample_rate / 10;
    return m->active_song.sample_pos / samples_per_row;
}

int xfm_wav_song_get_total_rows(xfm_wav_module* m, int song_id)
{
    if (!m || song_id < 1 || song_id > 15) return 0;
    if (!m->song_present[song_id]) return 0;
    
    // Estimate from duration
    XfmWavContent& song = m->songs[song_id];
    int samples_per_row = m->sample_rate / 10;  // ~100ms per row
    return song.num_samples / samples_per_row;
}

bool xfm_wav_song_is_playing(xfm_wav_module* m)
{
    return m && m->active_song.active;
}

// =============================================================================
// SFX Playback
// =============================================================================

static int allocate_wav_sfx_voice(xfm_wav_module* m, int priority)
{
    // Look for free voice
    for (int i = 0; i < 6; i++) {
        if (!m->active_sfx[i].active) {
            return i;
        }
    }
    
    // All busy - steal lowest priority
    int lowest = 0;
    int lowest_priority = m->active_sfx[0].priority;
    for (int i = 1; i < 6; i++) {
        if (m->active_sfx[i].priority < lowest_priority) {
            lowest = i;
            lowest_priority = m->active_sfx[i].priority;
        }
    }
    
    return lowest;
}

xfm_wav_voice_id xfm_wav_sfx_play(xfm_wav_module* m, int sfx_id, int priority)
{
    if (!m || sfx_id < 0 || sfx_id > 255) return -1;
    if (!m->sfx_present[sfx_id]) return -1;
    
    int voice = allocate_wav_sfx_voice(m, priority);
    if (voice < 0) return -1;
    
    XfmWavActiveSfx& sfx = m->active_sfx[voice];
    sfx.sfx_id = sfx_id;
    sfx.priority = priority;
    sfx.sample_pos = 0;
    sfx.active = true;
    
    return voice;
}

void xfm_wav_sfx_stop(xfm_wav_module* m, xfm_wav_voice_id voice)
{
    if (!m || voice < 0 || voice >= 6) return;
    m->active_sfx[voice].active = false;
}

void xfm_wav_sfx_stop_all(xfm_wav_module* m)
{
    if (!m) return;
    for (int i = 0; i < 6; i++) {
        m->active_sfx[i].active = false;
    }
}

// =============================================================================
// Audio Mixing
// =============================================================================

void xfm_wav_mix_song(xfm_wav_module* m, int16_t* stream, int frames)
{
    if (!m || !stream) return;

    // Clear buffer
    std::memset(stream, 0, frames * 2 * sizeof(int16_t));

    if (!m->active_song.active) return;

    XfmWavContent& song = m->songs[m->active_song.song_id];
    if (!song.data) return;

    int sample_pos = m->active_song.sample_pos;

    for (int i = 0; i < frames; i++) {
        if (sample_pos >= song.num_samples) {
            // End of song
            if (m->active_song.loop) {
                sample_pos = 0;
            } else {
                m->active_song.active = false;
                // Fill rest with silence
                std::memset(stream + (i * 2), 0, (frames - i) * 2 * sizeof(int16_t));
                break;
            }
        }

        // Copy sample
        stream[i * 2 + 0] = song.data[sample_pos * 2 + 0];
        stream[i * 2 + 1] = song.data[sample_pos * 2 + 1];
        sample_pos++;
    }

    m->active_song.sample_pos = sample_pos;

    // Check for pending song change (STEP timing)
    if (m->active_song.pending_set &&
        m->active_song.pending_timing == XFM_WAV_SWITCH_STEP) {
        m->active_song.song_id = m->active_song.pending_song_id;
        m->active_song.sample_pos = 0;
        m->active_song.pending_set = false;
    }
}

void xfm_wav_mix_sfx(xfm_wav_module* m, int16_t* stream, int frames)
{
    if (!m || !stream) return;

    // Don't clear buffer - song audio is already there!
    // We mix SFX on top of existing audio

    // Mix all active SFX
    for (int v = 0; v < 6; v++) {
        XfmWavActiveSfx& sfx = m->active_sfx[v];
        if (!sfx.active) continue;

        XfmWavContent& content = m->sfx[sfx.sfx_id];
        if (!content.data) {
            sfx.active = false;
            continue;
        }

        int sample_pos = sfx.sample_pos;

        for (int i = 0; i < frames; i++) {
            if (sample_pos >= content.num_samples) {
                // SFX finished
                sfx.active = false;
                break;
            }

            // Mix sample (with simple clipping)
            int mixed_l = stream[i * 2 + 0] + content.data[sample_pos * 2 + 0];
            int mixed_r = stream[i * 2 + 1] + content.data[sample_pos * 2 + 1];

            stream[i * 2 + 0] = (int16_t)std::max(-32768, std::min(32767, mixed_l));
            stream[i * 2 + 1] = (int16_t)std::max(-32768, std::min(32767, mixed_r));

            sample_pos++;
        }

        sfx.sample_pos = sample_pos;
    }
}

void xfm_wav_mix(xfm_wav_module* m, int16_t* stream, int frames)
{
    if (!m || !stream) return;
    
    // Mix song
    xfm_wav_mix_song(m, stream, frames);
    
    // Mix SFX on top
    xfm_wav_mix_sfx(m, stream, frames);
}
