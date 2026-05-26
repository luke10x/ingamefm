// =============================================================================
// xfm_impl.cpp — Implementation of the new eggsfm API (C99 interface)
// =============================================================================
//
// Architecture:
//   - Client creates two xfm_module instances: one for music, one for SFX
//   - Each module owns its own YM2612 chip instance
//   - Audio mixing is done by client calling xfm_mix() from SDL callback
//   - Patches are chip-specific (OPN for now, OPM/OPL extensible later)
//
// =============================================================================

#include "xfm_api.h"
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <new>
#include <cctype>


static constexpr int SONG_PITCH_SLIDE_NONE = -1000000;
static constexpr int SONG_VOLUME_SLIDE_NONE = -1000000;
static constexpr int SONG_NOTE_SLIDE_NONE = -1000000;
static constexpr double XFM_PI = 3.14159265358979323846;


// Gap calculation helpers - scale by sample rate for consistent timing
// Base gap is ~5.7ms at 44100Hz, scaled proportionally for other rates
static inline int get_min_gap_samples(int sample_rate)
{
    return (250 * sample_rate) / 44100;
}

// Hard mute threshold - if we're this close to keyon and RR hasn't released, force mute
// ~3ms at 44100Hz - last resort to prevent note bleed
static inline int get_hard_mute_threshold(int sample_rate)
{
    return (200 * sample_rate) / 44100;
}

static inline int get_dynamic_gap(int sample_rate, int rows_until_next, int samples_per_row)
{
    if (rows_until_next <= 0) return get_min_gap_samples(sample_rate);
    
    // Gap = min_gap + (rows * 0.4 * samples_per_row)
    // This gives ~40% of the time for release, 60% for note sustain
    int base_gap = get_min_gap_samples(sample_rate);
    int dynamic_gap = base_gap + (rows_until_next * samples_per_row * 4 / 10);
    int max_gap = (250 * sample_rate) / 44100;
    
    return std::min(max_gap, dynamic_gap);
}

// =============================================================================
// MODULE LIFETIME
// =============================================================================

xfm_module* xfm_module_create(int sample_rate, int buffer_frames, xfm_chip_type chip_type)
{
    // For now, only YM2612 and YM3438 are supported
    if (chip_type != XFM_CHIP_YM2612 && chip_type != XFM_CHIP_YM3438) {
        // Future: allocate appropriate chip type
        // case XFM_CHIP_OPM: chip = new XfmChipOpm(); break;
        // case XFM_CHIP_OPL2: chip = new XfmChipOpl2(); break;
        return nullptr;
    }

    xfm_module* m = new (std::nothrow) xfm_module;
    if (!m) return nullptr;

    m->sample_rate   = sample_rate;
    m->buffer_frames = buffer_frames;
    m->chip_type     = chip_type;
    m->volume        = 1.0f;
    m->chip          = new (std::nothrow) XfmChipOpn;
    m->lfo_enable    = false;
    m->lfo_freq      = 0;
    m->voice_age_counter = 0;
    m->auto_off_delay = 0.3f;  // Default: 30% of row before key-off

    if (!m->chip) {
        delete m;
        return nullptr;
    }

    // Initialize patch and channel state
    std::memset(m->patches, 0, sizeof(m->patches));
    std::memset(m->patch_present, 0, sizeof(m->patch_present));
    std::memset(m->macros, 0, sizeof(m->macros));
    std::memset(m->macro_present, 0, sizeof(m->macro_present));
    for (int patch = 0; patch < 256; patch++) {
        for (int target = 0; target < XFM_MACRO_TARGET_COUNT; target++) {
            m->patch_macros[patch][target] = -1;
        }
    }
    std::memset(m->current_patch, -1, sizeof(m->current_patch));
    std::memset(m->channel_active, 0, sizeof(m->channel_active));
    std::memset(m->live_patches, 0, sizeof(m->live_patches));
    std::memset(m->live_patch_valid, 0, sizeof(m->live_patch_valid));
    for (int i = 0; i < 6; i++) {
        m->live_patch_id[i] = -1;
        m->live_op_mask[i] = 0x0F;
    }

    // Initialize voice pool
    for (int i = 0; i < 6; i++) {
        m->voices[i].midi_note = -1;
        m->voices[i].patch_id = -1;
        m->voices[i].active = false;
        m->voices[i].age = 0;
        m->voices[i].priority = 0;
        m->voices[i].sfx_id = -1;
    }

    // Initialize SFX patterns
    std::memset(m->sfx_patterns, 0, sizeof(m->sfx_patterns));
    std::memset(m->sfx_present, 0, sizeof(m->sfx_present));
    
    // Initialize active SFX tracking
    for (int i = 0; i < 6; i++) {
        m->active_sfx[i].sfx_id = -1;
        m->active_sfx[i].priority = 0;
        m->active_sfx[i].voice_idx = -1;
        m->active_sfx[i].current_row = 0;
        m->active_sfx[i].sample_in_row = get_min_gap_samples(m->sample_rate);
        m->active_sfx[i].rows_remaining = 0;
        m->active_sfx[i].last_patch_id = -1;
        m->active_sfx[i].pending_has_note = false;
        m->active_sfx[i].pending_note = -1;
        m->active_sfx[i].pending_patch_id = -1;
        m->active_sfx[i].pending_gap = get_min_gap_samples(m->sample_rate);
        m->active_sfx[i].auto_off_scheduled = false;
        m->active_sfx[i].auto_off_at_sample = 0;
        m->active_sfx[i].legato_enabled = false;
        m->active_sfx[i].portamento_active = false;
        m->active_sfx[i].portamento_target_note = -1;
        m->active_sfx[i].current_hz = 0.0;
        m->active_sfx[i].target_hz = 0.0;
        m->active_sfx[i].portamento_step_hz = 0.0;
        m->active_sfx[i].live_patch_valid = false;
        m->active_sfx[i].active = false;
    }

    // Initialize song patterns
    std::memset(m->song_patterns, 0, sizeof(m->song_patterns));
    std::memset(m->song_present, 0, sizeof(m->song_present));

    // Initialize active song
    m->active_song.song_id = 0;
    m->active_song.current_row = 0;
    m->active_song.sample_in_row = 0;
    m->active_song.rows_remaining = 0;
    m->active_song.loop_start_row = 0;
    m->active_song.loop_end_row = -1;
    m->active_song.active = false;
    m->active_song.loop = false;
    for (int ch = 0; ch < 6; ch++) {
        m->active_song.channels[ch].current_patch = -1;
        m->active_song.channels[ch].current_volume = 127;
        m->active_song.channels[ch].current_volume_f = 127.0;
        m->active_song.channels[ch].current_volume_f = 127.0;
        m->active_song.channels[ch].pending_has_note = false;
        m->active_song.channels[ch].pending_is_off = false;
        m->active_song.channels[ch].wait_for_next_row = false;
        m->active_song.channels[ch].legato_enabled = false;
        m->active_song.channels[ch].pitch_slide_speed = 0;
        m->active_song.channels[ch].portamento_active = false;
        m->active_song.channels[ch].portamento_speed = 0;
        m->active_song.channels[ch].portamento_target_note = -1;
        m->active_song.channels[ch].current_hz = 0.0;
        m->active_song.channels[ch].target_hz = 0.0;
        m->active_song.channels[ch].portamento_step_hz = 0.0;
        m->active_song.channels[ch].volume_slide_speed = 0;
        m->active_song.channels[ch].note_slide_active = false;
        m->active_song.channels[ch].note_slide_target_hz = 0.0;
        m->active_song.channels[ch].note_slide_step_hz = 0.0;
        m->active_song.channels[ch].fine_pitch_cents = 0;
        m->active_song.channels[ch].vibrato_speed = 0;
        m->active_song.channels[ch].vibrato_depth = 0;
        m->active_song.channels[ch].vibrato_phase = 0.0;
        m->active_song.channels[ch].tremolo_speed = 0;
        m->active_song.channels[ch].tremolo_depth = 0;
        m->active_song.channels[ch].tremolo_phase = 0.0;
        m->active_song.channels[ch].envelope_hard_reset = false;
        m->active_song.channels[ch].base_note = -1;
        m->active_song.channels[ch].arp_offset = 0;
        m->active_song.channels[ch].sample_in_tick = 0;
        m->active_song.channels[ch].macro_disabled_mask = 0;
        for (int target = 0; target < XFM_MACRO_TARGET_COUNT; target++) {
            m->active_song.channels[ch].macro_states[target].macro_id = -1;
            m->active_song.channels[ch].macro_states[target].pos = 0;
            m->active_song.channels[ch].macro_states[target].active = false;
            m->active_song.channels[ch].macro_states[target].released = false;
        }
        m->active_song.channels[ch].live_patch_valid = false;
    }

    // Initialize pending song
    m->pending_song.song_id = 0;
    m->pending_song.timing = FM_SONG_SWITCH_NOW;
    m->pending_song.pending = false;

    return m;
}

void xfm_module_destroy(xfm_module* m)
{
    if (!m) return;
    delete m->chip;
    delete m;
}

void xfm_module_set_volume(xfm_module* m, float volume)
{
    if (!m) return;
    m->volume = std::max(0.0f, std::min(1.0f, volume));
}

void xfm_module_set_lfo(xfm_module* m, bool enable, int freq)
{
    if (!m || !m->chip) return;
    m->lfo_enable = enable;
    m->lfo_freq = freq & 7;
    m->chip->enable_lfo(enable, static_cast<uint8_t>(m->lfo_freq));
}

void xfm_module_reload_patches(xfm_module* m)
{
    if (!m || !m->chip) return;
    // Reset current_patch tracking so patches reload on next note
    for (int i = 0; i < 6; i++) {
        m->current_patch[i] = -1;
        m->live_patch_valid[i] = false;
        m->live_patch_id[i] = -1;
        m->live_op_mask[i] = 0x0F;
    }
}

// Reset module state for clean export (call before each SFX export)
void xfm_module_reset_state(xfm_module* m)
{
    if (!m || !m->chip) return;
    
    // CRITICAL: Reset YM2612 chip internal state (phase accumulators, envelopes, etc.)
    // This prevents state leakage from previous exports that corrupts audio
    m->chip->reset_chip();
    
    // Key off all voice and clear state
    for (int i = 0; i < 6; i++) {
        m->chip->key_off(i);
        m->voices[i].active = false;
        m->voices[i].midi_note = -1;
        m->voices[i].patch_id = -1;
        m->voices[i].priority = 0;
        m->voices[i].sfx_id = -1;
        m->voices[i].age = 0;
        m->channel_active[i] = false;
        m->current_patch[i] = -1;
        m->live_patch_valid[i] = false;
        m->live_patch_id[i] = -1;
        m->live_op_mask[i] = 0x0F;
        m->active_sfx[i].active = false;
        m->active_sfx[i].sfx_id = -1;
        m->active_sfx[i].voice_idx = -1;
        m->active_sfx[i].current_row = 0;
        m->active_sfx[i].sample_in_row = 0;
        m->active_sfx[i].rows_remaining = 0;
        m->active_sfx[i].pending_has_note = false;
        m->active_sfx[i].pending_note = -1;
        m->active_sfx[i].pending_patch_id = -1;
        m->active_sfx[i].auto_off_scheduled = false;
        m->active_sfx[i].legato_enabled = false;
        m->active_sfx[i].portamento_active = false;
        m->active_sfx[i].portamento_target_note = -1;
        m->active_sfx[i].current_hz = 0.0;
        m->active_sfx[i].target_hz = 0.0;
        m->active_sfx[i].portamento_step_hz = 0.0;
        m->active_sfx[i].live_patch_valid = false;
    }
    
    // Reset song state
    m->active_song.active = false;
    m->active_song.song_id = 0;
    m->active_song.sample_in_row = 0;
    m->active_song.current_row = 0;
    m->active_song.rows_remaining = 0;
    for (int ch = 0; ch < 6; ch++) {
        m->active_song.channels[ch].envelope_hard_reset = false;
        m->active_song.channels[ch].base_note = -1;
        m->active_song.channels[ch].arp_offset = 0;
        m->active_song.channels[ch].sample_in_tick = 0;
        m->active_song.channels[ch].macro_disabled_mask = 0;
        for (int target = 0; target < XFM_MACRO_TARGET_COUNT; target++) {
            m->active_song.channels[ch].macro_states[target].macro_id = -1;
            m->active_song.channels[ch].macro_states[target].pos = 0;
            m->active_song.channels[ch].macro_states[target].active = false;
            m->active_song.channels[ch].macro_states[target].released = false;
        }
        m->active_song.channels[ch].live_patch_valid = false;
    }
}

// =============================================================================
// PATCH SYSTEM
// =============================================================================

void xfm_patch_set(xfm_module* m, xfm_patch_id patch_id, const void* patch_data,
                  int patch_size, xfm_chip_type patch_type)
{
    if (!m || !patch_data || patch_id < 0 || patch_id > 255) return;

    // Only YM2612/YM3438 patches supported for now
    if (patch_type != XFM_CHIP_YM2612 && patch_type != XFM_CHIP_YM3438) return;
    if (m->chip_type != XFM_CHIP_YM2612 && m->chip_type != XFM_CHIP_YM3438) return;

    // Validate size
    if (patch_size != sizeof(xfm_patch_opn)) return;

    m->patches[patch_id] = *static_cast<const xfm_patch_opn*>(patch_data);
    m->patch_present[patch_id] = true;
}

int xfm_macro_parse(XfmMacro* out, uint8_t target, const char* sequence)
{
    if (!out || !sequence) return 0;
    if (target <= XFM_MACRO_NONE || target >= XFM_MACRO_TARGET_COUNT) return 0;

    XfmMacro macro = {};
    macro.target = target;
    macro.loop_start = 0;
    macro.release_start = 0xFF;
    macro.has_loop = false;

    const char* p = sequence;
    while (*p) {
        while (*p && (std::isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;

        if (*p == '|') {
            macro.loop_start = macro.length;
            macro.has_loop = true;
            p++;
            continue;
        }

        char* end = nullptr;
        long value = std::strtol(p, &end, 0);
        if (end == p) return 0;
        p = end;

        int repeat = 1;
        if (*p == '*') {
            p++;
            char* repeat_end = nullptr;
            long parsed_repeat = std::strtol(p, &repeat_end, 10);
            if (repeat_end == p || parsed_repeat <= 0 || parsed_repeat > XFM_MAX_MACRO_VALUES) return 0;
            repeat = (int)parsed_repeat;
            p = repeat_end;
        }

        for (int i = 0; i < repeat; i++) {
            if (macro.length >= XFM_MAX_MACRO_VALUES) return 0;
            macro.values[macro.length++] = (int16_t)value;
        }
    }

    if (macro.length == 0) return 0;
    if (macro.has_loop && macro.loop_start >= macro.length) return 0;

    *out = macro;
    return 1;
}

xfm_macro_id xfm_macro_set(xfm_module* m, xfm_macro_id id, const XfmMacro* macro)
{
    if (!m || !macro) return -1;
    if (id < 0 || id >= XFM_MAX_MACROS) return -1;
    if (macro->target <= XFM_MACRO_NONE || macro->target >= XFM_MACRO_TARGET_COUNT) return -1;
    if (macro->length == 0 || macro->length > XFM_MAX_MACRO_VALUES) return -1;
    if (macro->has_loop && macro->loop_start >= macro->length) return -1;

    m->macros[id] = *macro;
    m->macro_present[id] = true;
    return id;
}

void xfm_patch_macro_set(xfm_module* m, xfm_patch_id patch_id,
                         uint8_t target, xfm_macro_id macro_id)
{
    if (!m) return;
    if (patch_id < 0 || patch_id > 255) return;
    if (target <= XFM_MACRO_NONE || target >= XFM_MACRO_TARGET_COUNT) return;
    if (macro_id < 0 || macro_id >= XFM_MAX_MACROS || !m->macro_present[macro_id]) return;
    if (m->macros[macro_id].target != target) return;
    m->patch_macros[patch_id][target] = macro_id;
}

void xfm_patch_macro_clear(xfm_module* m, xfm_patch_id patch_id, uint8_t target)
{
    if (!m) return;
    if (patch_id < 0 || patch_id > 255) return;
    if (target == XFM_MACRO_NONE) {
        for (int t = 0; t < XFM_MACRO_TARGET_COUNT; t++) m->patch_macros[patch_id][t] = -1;
        return;
    }
    if (target >= XFM_MACRO_TARGET_COUNT) return;
    m->patch_macros[patch_id][target] = -1;
}


// =============================================================================
// SFX PATTERN PARSER
// =============================================================================

static int parse_note_field(const char* nc) {
    if (nc[0]=='.' && nc[1]=='.' && nc[2]=='.') return -1;  // no note
    if (nc[0]=='O' && nc[1]=='F' && nc[2]=='F') return -2;  // note off
    if (nc[0]=='R' && nc[1]=='E' && nc[2]=='L') return -3;  // release
    if (nc[0]=='=' && nc[1]=='=' && nc[2]=='=') return -4;  // hard cut
    
    int semitone = -1;
    switch (nc[0]) {
        case 'C': semitone = 0;  break;
        case 'D': semitone = 2;  break;
        case 'E': semitone = 4;  break;
        case 'F': semitone = 5;  break;
        case 'G': semitone = 7;  break;
        case 'A': semitone = 9;  break;
        case 'B': semitone = 11; break;
        default: return -1;
    }
    
    if (nc[1] == '#') semitone++;
    else if (nc[1] != '-') return -1;
    
    if (nc[2] < '0' || nc[2] > '9') return -1;
    
    // Furnace octave: C-4 = MIDI 60
    return 12 + (nc[2] - '0') * 12 + semitone;
}

static int parse_hex2(const char* p) {
    if (p[0] == '.' && p[1] == '.') return -1;
    
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    
    int h0 = hex(p[0]);
    int h1 = hex(p[1]);
    if (h0 < 0 || h1 < 0) return -1;
    return h0 * 16 + h1;
}

static double note_to_hz(int midi_note)
{
    return 440.0 * std::pow(2.0, (midi_note - 69) / 12.0);
}

static bool song_is_carrier(uint8_t alg, int op);

xfm_sfx_id xfm_sfx_declare(xfm_module* m, xfm_sfx_id id, const char* pattern_text, int tick_rate, int speed)
{
    if (!m || !pattern_text || tick_rate <= 0 || speed <= 0) return -1;
    if (id < 0 || id > 255) return -1;
    
    // Parse the pattern text (Furnace format, single channel)
    // First line: number of rows
    const char* p = pattern_text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    
    int num_rows = 0;
    while (*p >= '0' && *p <= '9') {
        num_rows = num_rows * 10 + (*p - '0');
        p++;
    }

    // Allow up to 65536 rows (16-bit range) for long songs
    if (num_rows <= 0 || num_rows > 65536) return -1;
    
    // Skip to first row data
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
    
    // Allocate and parse rows
    XfmSfxPattern& pat = m->sfx_patterns[id];
    pat.num_rows = num_rows;
    pat.tick_rate = tick_rate;
    pat.speed = speed;
    pat.samples_per_row = (int)((double)m->sample_rate / tick_rate * speed);
    
    // Free existing rows if any
    if (pat.rows) {
        delete[] pat.rows;
    }
    pat.rows = new XfmSfxEvent[num_rows];
    
    int last_inst = -1;
    
    for (int row = 0; row < num_rows; row++) {
        XfmSfxEvent& ev = pat.rows[row];
        ev.note = -1;
        ev.patch_id = -1;
        
        if (!*p || *p == '\0') break;
        
        // Parse row: "C-4007F" or "......." or "OFF...."
        // Format: note(3) + inst(2) + vol(2) [+ effects]
        char note_str[4] = {0};
        char inst_str[3] = {0};
        
        // Copy note (3 chars)
        for (int i = 0; i < 3 && *p && *p != '|' && *p != '\n'; i++) {
            note_str[i] = *p++;
        }
        // Copy instrument (2 chars)
        for (int i = 0; i < 2 && *p && *p != '|' && *p != '\n'; i++) {
            inst_str[i] = *p++;
        }
        // Skip rest of channel and move to next line
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        
        // Parse note
        if (note_str[0] && note_str[0] != '.') {
            ev.note = parse_note_field(note_str);
        } else {
            ev.note = -1;  // inherit
        }
        
        // Parse instrument
        if (inst_str[0] && inst_str[0] != '.') {
            ev.patch_id = parse_hex2(inst_str);
            if (ev.patch_id >= 0) last_inst = ev.patch_id;
        } else {
            ev.patch_id = -1;  // inherit
        }
        
        // Resolve instrument inheritance only. A "..." note field is a rest/hold:
        // it must not retrigger the previous note every row.
        if (ev.patch_id == -1) ev.patch_id = last_inst;
    }
    
    m->sfx_present[id] = true;
    return id;
}

// =============================================================================
// SFX PLAYBACK WITH VOICE STEALING
// =============================================================================

// Find the next row with a note in an SFX pattern
static int find_next_sfx_note_row(XfmSfxPattern& pat, int start_row)
{
    for (int row = start_row + 1; row < pat.num_rows; row++) {
        if (pat.rows[row].note >= 0) {
            return row;
        }
    }
    return -1;  // No more notes
}

// Process a row for an SFX voice (sets up pending note)
// Implements automatic note-off delay for clean transitions
static void sfx_process_row(xfm_module* m, int voice_idx, int row_idx)
{
    XfmActiveSfx* sfx = nullptr;
    for (int slot = 0; slot < 6; slot++) {
        if (m->active_sfx[slot].voice_idx == voice_idx) {
            sfx = &m->active_sfx[slot];
            break;
        }
    }
    if (!sfx) return;

    XfmSfxPattern& pat = m->sfx_patterns[sfx->sfx_id];
    if (row_idx >= pat.num_rows) return;

    XfmSfxEvent& ev = pat.rows[row_idx];

    // Update last known patch
    if (ev.patch_id >= 0) {
        sfx->last_patch_id = ev.patch_id;
    }

    // Clear auto-off scheduling from previous row
    sfx->auto_off_scheduled = false;

    // Handle note
    sfx->pending_has_note = false;
    if (ev.note == -2 || ev.note == -3) {
        // OFF/REL note - key off and let the envelope release.
        m->chip->key_off(voice_idx);
        m->channel_active[voice_idx] = false;
    } else if (ev.note == -4) {
        // === hard cut - immediate silence, no release tail.
        m->chip->hard_mute(voice_idx);
        m->channel_active[voice_idx] = false;
    } else if (ev.note >= 0) {
        // New note - check if voice is still playing from previous row
        bool was_playing = m->channel_active[voice_idx];
        
        // Schedule auto key-off at configured delay position
        if (was_playing) {
            sfx->auto_off_scheduled = true;
            sfx->auto_off_at_sample = (int)(pat.samples_per_row * m->auto_off_delay);
            if (sfx->auto_off_at_sample < 1) sfx->auto_off_at_sample = 1;
        }

        if (!was_playing) {
            sfx->pending_gap = 0;
        } else {
            // Look ahead to find next note
            int next_note_row = find_next_sfx_note_row(pat, row_idx);
            if (next_note_row >= 0) {
                // Calculate distance to next note in samples
                int rows_until_next = next_note_row - row_idx;
                // Dynamic gap for RR release
                int gap = get_min_gap_samples(m->sample_rate) + (rows_until_next * pat.samples_per_row * 4 / 10);
                int max_gap = (250 * m->sample_rate) / 44100;
                sfx->pending_gap = std::min(max_gap, gap);
            } else {
                // No next note - use default gap
                sfx->pending_gap = get_min_gap_samples(m->sample_rate);
            }
        }

        // Mark for key-on after gap
        sfx->pending_has_note = true;
        sfx->pending_note = ev.note;
        sfx->pending_patch_id = (ev.patch_id >= 0) ? ev.patch_id : sfx->last_patch_id;
    }
}

// Commit pending note (after gap samples)
static void sfx_commit_keyon(xfm_module* m, int voice_idx, int current_gap)
{
    // Find the active SFX for this voice
    for (int slot = 0; slot < 6; slot++) {
        XfmActiveSfx& sfx = m->active_sfx[slot];
        if (!sfx.active || sfx.voice_idx != voice_idx) continue;

        if (!sfx.pending_has_note) return;
        if (sfx.pending_patch_id < 0) return;
        if (!m->patch_present[sfx.pending_patch_id]) return;
        
        // If gap is 0, key on immediately (fast passage)
        if (sfx.pending_gap == 0) {
            m->chip->load_patch(m->patches[sfx.pending_patch_id], voice_idx);
            m->current_patch[voice_idx] = sfx.pending_patch_id;
            // Re-apply LFO settings after loading patch
            m->chip->enable_lfo(m->lfo_enable, static_cast<uint8_t>(m->lfo_freq));
            double hz = 440.0 * std::pow(2.0, (sfx.pending_note - 69) / 12.0);
            m->chip->set_frequency(voice_idx, hz, 0);
            m->chip->key_on(voice_idx);
            sfx.current_hz = hz;
            sfx.target_hz = hz;
            sfx.portamento_active = false;
            m->voices[voice_idx].midi_note = sfx.pending_note;
            m->channel_active[voice_idx] = true;
            sfx.pending_has_note = false;
            return;
        }

        // Wait until gap has fully elapsed before keying on
        if (current_gap < sfx.pending_gap) {
            return;
        }

        // Load patch and key on
        m->chip->load_patch(m->patches[sfx.pending_patch_id], voice_idx);
        m->current_patch[voice_idx] = sfx.pending_patch_id;
        // Re-apply LFO settings after loading patch
        m->chip->enable_lfo(m->lfo_enable, static_cast<uint8_t>(m->lfo_freq));
        double hz = 440.0 * std::pow(2.0, (sfx.pending_note - 69) / 12.0);
        m->chip->set_frequency(voice_idx, hz, 0);
        m->chip->key_on(voice_idx);
        sfx.current_hz = hz;
        sfx.target_hz = hz;
        sfx.portamento_active = false;
        m->voices[voice_idx].midi_note = sfx.pending_note;
        m->channel_active[voice_idx] = true;
        sfx.pending_has_note = false;
        sfx.pending_gap = get_min_gap_samples(m->sample_rate);
        return;
    }
}

xfm_voice_id xfm_sfx_play(xfm_module* m, xfm_sfx_id id, int priority)
{
    if (!m || !m->chip) return FM_VOICE_INVALID;
    if (id < 0 || id > 255) return FM_VOICE_INVALID;
    if (!m->sfx_present[id]) return FM_VOICE_INVALID;
    
    XfmSfxPattern& pat = m->sfx_patterns[id];
    if (!pat.rows || pat.num_rows <= 0) return FM_VOICE_INVALID;
    
    // Find a voice to use for this SFX
    // Priority rules:
    // 1. Prefer free voices
    // 2. Steal lowest priority voice (including piano which is 0)
    // 3. If equal priority, steal oldest
    
    int best_voice = -1;
    int best_slot = -1;  // active_sfx slot to clear
    int best_priority = priority + 1;  // Start higher than our priority
    int best_age = 0;
    bool found_free = false;
    
    // First pass: look for any free voice
    for (int i = 0; i < 6; i++) {
        if (!m->voices[i].active) {
            best_voice = i;
            best_slot = -1;
            found_free = true;
            break;  // Take the first free voice
        }
    }
    
    // Second pass: if no free voice, find one to steal
    if (!found_free) {
        for (int i = 0; i < 6; i++) {
            // Check if we can steal this voice
            if (m->voices[i].priority < best_priority ||
                (m->voices[i].priority == best_priority && m->voices[i].age > best_age)) {
                best_voice = i;
                best_priority = m->voices[i].priority;
                best_age = m->voices[i].age;
                // Find the active_sfx slot for this voice
                for (int slot = 0; slot < 6; slot++) {
                    if (m->active_sfx[slot].active && m->active_sfx[slot].voice_idx == i) {
                        best_slot = slot;
                        break;
                    }
                }
            }
        }
    }
    
    if (best_voice < 0) return FM_VOICE_INVALID;  // Should not happen
    
    // Clear the old SFX slot if we're stealing
    if (best_slot >= 0) {
        m->active_sfx[best_slot].active = false;
        m->active_sfx[best_slot].voice_idx = -1;
        m->active_sfx[best_slot].sfx_id = -1;
    }
    
    // Key off the voice immediately
    m->chip->key_off(best_voice);
    
    // Find a free active_sfx slot
    int slot = -1;
    for (int i = 0; i < 6; i++) {
        if (!m->active_sfx[i].active) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) return FM_VOICE_INVALID;  // No free SFX slots (should not happen)
    
    // Initialize the SFX
    XfmActiveSfx& sfx = m->active_sfx[slot];
    sfx.sfx_id = id;
    sfx.priority = priority;
    sfx.voice_idx = best_voice;
    sfx.current_row = 0;
    sfx.sample_in_row = get_min_gap_samples(m->sample_rate);  // Start after gap
    sfx.rows_remaining = pat.num_rows;
    sfx.last_patch_id = -1;
    sfx.pending_has_note = false;
    sfx.pending_note = -1;
    sfx.pending_patch_id = -1;
    sfx.active = true;
    sfx.auto_off_scheduled = false;
    sfx.auto_off_at_sample = 0;
    sfx.legato_enabled = false;
    sfx.portamento_active = false;
    sfx.portamento_target_note = -1;
    sfx.current_hz = 0.0;
    sfx.target_hz = 0.0;
    sfx.portamento_step_hz = 0.0;
    sfx.live_patch_valid = false;

    // Update voice state
    m->voices[best_voice].active = true;
    m->voices[best_voice].priority = priority;
    m->voices[best_voice].sfx_id = id;
    m->voices[best_voice].age = ++m->voice_age_counter;
    m->voices[best_voice].midi_note = -1;
    m->voices[best_voice].patch_id = -1;
    m->channel_active[best_voice] = false;

    // Initialize pending gap
    sfx.pending_gap = get_min_gap_samples(m->sample_rate);

    // Process first row immediately (sets up pending note)
    sfx_process_row(m, best_voice, 0);

    // Commit key-on immediately (we're already past the gap)
    sfx_commit_keyon(m, best_voice, sfx.sample_in_row);

    return best_voice;
}

void xfm_sfx_stop(xfm_module* m, xfm_voice_id voice)
{
    if (!m || !m->chip) return;
    if (voice < 0 || voice >= 6) return;

    m->chip->key_off(voice);
    m->channel_active[voice] = false;
    m->voices[voice].active = false;
    m->voices[voice].midi_note = -1;
    m->voices[voice].patch_id = -1;
    m->voices[voice].priority = 0;
    m->voices[voice].sfx_id = -1;
    m->current_patch[voice] = -1;
    m->live_patch_valid[voice] = false;
    m->live_patch_id[voice] = -1;
    m->live_op_mask[voice] = 0x0F;

    for (int slot = 0; slot < 6; slot++) {
        XfmActiveSfx& sfx = m->active_sfx[slot];
        if (sfx.voice_idx != voice) continue;

        sfx.active = false;
        sfx.sfx_id = -1;
        sfx.voice_idx = -1;
        sfx.current_row = 0;
        sfx.sample_in_row = 0;
        sfx.rows_remaining = 0;
        sfx.last_patch_id = -1;
        sfx.pending_has_note = false;
        sfx.pending_note = -1;
        sfx.pending_patch_id = -1;
        sfx.pending_gap = get_min_gap_samples(m->sample_rate);
        sfx.auto_off_scheduled = false;
        sfx.auto_off_at_sample = 0;
        sfx.portamento_active = false;
        sfx.portamento_target_note = -1;
        sfx.current_hz = 0.0;
        sfx.target_hz = 0.0;
        sfx.portamento_step_hz = 0.0;
        sfx.live_patch_valid = false;
    }
}

void xfm_sfx_stop_all(xfm_module* m)
{
    if (!m) return;
    for (int voice = 0; voice < 6; voice++) {
        xfm_sfx_stop(m, voice);
    }
}

// Internal: Update active SFX voices (called from xfm_mix each frame)
// Advances SFX by 1 sample. Call this in a loop for each sample in the buffer.
static void update_sfx_voice(xfm_module* m, int slot)
{
    XfmActiveSfx& sfx = m->active_sfx[slot];
    if (!sfx.active) return;
    if (sfx.rows_remaining <= 0) return;
    if (sfx.voice_idx < 0 || sfx.voice_idx >= 6) return;

    XfmSfxPattern& pat = m->sfx_patterns[sfx.sfx_id];
    int voice = sfx.voice_idx;

    // Advance sample counter
    sfx.sample_in_row++;

    // Check for scheduled auto key-off
    if (sfx.auto_off_scheduled && sfx.sample_in_row == sfx.auto_off_at_sample) {
        m->chip->key_off(voice);
        m->channel_active[voice] = false;
        sfx.auto_off_scheduled = false;
    }

    // Check if we've crossed the gap boundary - commit key-on
    if (sfx.pending_has_note) {
        sfx_commit_keyon(m, voice, sfx.sample_in_row);
    }

    // Check if we've reached end of row
    if (sfx.sample_in_row >= pat.samples_per_row) {
        sfx.sample_in_row = 0;
        sfx.rows_remaining--;
        sfx.current_row++;

        // SFX finished?
        if (sfx.rows_remaining <= 0) {
            m->chip->key_off(voice);
            m->voices[voice].active = false;
            m->voices[voice].priority = 0;
            m->voices[voice].sfx_id = -1;
            m->voices[voice].midi_note = -1;
            m->voices[voice].patch_id = -1;
            m->channel_active[voice] = false;
            sfx.active = false;
            sfx.voice_idx = -1;
            sfx.sfx_id = -1;
            return;
        }
        
        // Process next row if within pattern
        if (sfx.current_row < pat.num_rows) {
            sfx_process_row(m, voice, sfx.current_row);
        }
    }
}

// Update all SFX voices by a given number of samples
/**
 * @brief Advance all active SFX and trigger notes.
 * 
 * Called every audio callback to advance all active SFX.
 * 
 * Call Rate: ~172 Hz (for 256 frames @ 44100 Hz)
 * From: xfm_mix() or xfm_mix_sfx()
 * Thread: SDL audio thread
 * 
 * Processing:
 *   For each sample:
 *     For each SFX slot (0-5):
 *       → update_sfx_voice() - Process one SFX
 * 
 * Note Triggering:
 *   - Dynamic gap timing (gap scales with interval distance)
 *   - key_on() when pending_gap reaches 0
 *   - key_off() on "OFF" rows or SFX end
 * 
 * @param m Module instance
 * @param num_samples Number of samples to advance (= frames from callback)
 */
static void update_sfx(xfm_module* m, int num_samples)
{
    for (int i = 0; i < num_samples; i++) {
        for (int slot = 0; slot < 6; slot++) {
            update_sfx_voice(m, slot);
        }
    }
}

static void finish_sfx_voice(xfm_module* m, int slot)
{
    XfmActiveSfx& sfx = m->active_sfx[slot];
    int voice = sfx.voice_idx;
    if (voice >= 0 && voice < 6) {
        m->chip->key_off(voice);
        m->voices[voice].active = false;
        m->voices[voice].priority = 0;
        m->voices[voice].sfx_id = -1;
        m->voices[voice].midi_note = -1;
        m->voices[voice].patch_id = -1;
        m->channel_active[voice] = false;
    }
    sfx.active = false;
    sfx.voice_idx = -1;
    sfx.sfx_id = -1;
}

static bool process_sfx_events_now(xfm_module* m)
{
    bool changed = false;
    for (int slot = 0; slot < 6; slot++) {
        XfmActiveSfx& sfx = m->active_sfx[slot];
        if (!sfx.active || sfx.rows_remaining <= 0) continue;
        if (sfx.voice_idx < 0 || sfx.voice_idx >= 6) continue;

        XfmSfxPattern& pat = m->sfx_patterns[sfx.sfx_id];
        int voice = sfx.voice_idx;

        if (sfx.auto_off_scheduled && sfx.sample_in_row >= sfx.auto_off_at_sample) {
            m->chip->key_off(voice);
            m->channel_active[voice] = false;
            sfx.auto_off_scheduled = false;
            changed = true;
        }

        if (sfx.pending_has_note && sfx.sample_in_row >= sfx.pending_gap) {
            sfx_commit_keyon(m, voice, sfx.sample_in_row);
            changed = true;
        }

        while (sfx.active && sfx.sample_in_row >= pat.samples_per_row) {
            sfx.sample_in_row -= pat.samples_per_row;
            sfx.rows_remaining--;
            sfx.current_row++;
            changed = true;

            if (sfx.rows_remaining <= 0) {
                finish_sfx_voice(m, slot);
                break;
            }

            if (sfx.current_row < pat.num_rows) {
                sfx_process_row(m, voice, sfx.current_row);
            }
        }
    }
    return changed;
}

static int next_sfx_event_delta(xfm_module* m, int max_frames)
{
    int next = max_frames;
    for (int slot = 0; slot < 6; slot++) {
        XfmActiveSfx& sfx = m->active_sfx[slot];
        if (!sfx.active || sfx.rows_remaining <= 0) continue;
        if (sfx.voice_idx < 0 || sfx.voice_idx >= 6) continue;

        XfmSfxPattern& pat = m->sfx_patterns[sfx.sfx_id];
        if (sfx.pending_has_note) {
            next = std::min(next, std::max(0, sfx.pending_gap - sfx.sample_in_row));
        }
        if (sfx.auto_off_scheduled) {
            next = std::min(next, std::max(0, sfx.auto_off_at_sample - sfx.sample_in_row));
        }
        next = std::min(next, std::max(0, pat.samples_per_row - sfx.sample_in_row));
    }
    return next;
}

static void advance_sfx_time(xfm_module* m, int frames)
{
    for (int slot = 0; slot < 6; slot++) {
        XfmActiveSfx& sfx = m->active_sfx[slot];
        if (sfx.active) sfx.sample_in_row += frames;
    }
}

// =============================================================================
// SONG PATTERN PARSER (multi-channel)
// =============================================================================

xfm_song_id xfm_song_declare(xfm_module* m, xfm_song_id id, const char* pattern_text,
                           int tick_rate, int speed)
{
    if (!m || !pattern_text || tick_rate <= 0 || speed <= 0) return -1;
    if (id < 1 || id > 15) return -1;  // Reserve IDs 1-15 for songs
    
    // Parse the pattern text (Furnace multi-channel format)
    const char* p = pattern_text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    
    // First line: number of rows
    int num_rows = 0;
    while (*p >= '0' && *p <= '9') {
        num_rows = num_rows * 10 + (*p - '0');
        p++;
    }

    // Allow up to 65536 rows (16-bit range) for long songs
    if (num_rows <= 0 || num_rows > 65536) return -1;
    
    // Skip to first row data
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
    
    // Parse first row to determine number of channels
    const char* row_start = p;
    int num_channels = 1;
    while (*p && *p != '\n') {
        if (*p == '|') num_channels++;
        p++;
    }
    
    // Limit to 6 channels (OPN max)
    if (num_channels > 6) num_channels = 6;
    
    // Reset position to start of first row
    p = row_start;
    
    // Allocate and parse song
    XfmSongPattern& song = m->song_patterns[id];

    // Free existing rows if any
    if (song.rows) {
        for (int r = 0; r < song.num_rows; r++) {
            if (song.rows[r]) delete[] song.rows[r];
        }
        delete[] song.rows;
        song.rows = nullptr;
    }

    song.num_rows = num_rows;
    song.num_channels = num_channels;
    song.tick_rate = tick_rate;
    song.speed = speed;
    song.samples_per_row = (int)((double)m->sample_rate / tick_rate * speed);

    song.rows = new XfmSongEvent*[num_rows];
    for (int r = 0; r < num_rows; r++) {
        song.rows[r] = new XfmSongEvent[num_channels];
        for (int ch = 0; ch < num_channels; ch++) {
            song.rows[r][ch].note = -1;
            song.rows[r][ch].patch_id = -1;
            song.rows[r][ch].volume = -1;
            song.rows[r][ch].legato = -1;
            song.rows[r][ch].pitch_slide = SONG_PITCH_SLIDE_NONE;
            song.rows[r][ch].portamento = -1;
            song.rows[r][ch].vibrato = -1;
            song.rows[r][ch].tremolo = -1;
            song.rows[r][ch].volume_slide = SONG_VOLUME_SLIDE_NONE;
            song.rows[r][ch].note_slide = SONG_NOTE_SLIDE_NONE;
            song.rows[r][ch].fine_pitch = -1;
            song.rows[r][ch].hard_reset = -1;
            song.rows[r][ch].macro_enable = -1;
            song.rows[r][ch].macro_disable = -1;
            song.rows[r][ch].opn_effect_count = 0;
        }
    }

    // Parse each row - p is already at start of first data row
    int last_note[6] = {-1, -1, -1, -1, -1, -1};
    int last_inst[6] = {-1, -1, -1, -1, -1, -1};
    int last_vol[6] = {127, 127, 127, 127, 127, 127};
    
    for (int row = 0; row < num_rows; row++) {
        if (!*p || *p == '\0') break;
        
        for (int ch = 0; ch < num_channels; ch++) {
            XfmSongEvent& ev = song.rows[row][ch];
            ev.note = -1;       // -1 = no note (keep playing)
            ev.patch_id = -1;   // -1 = inherit instrument
            ev.volume = -1;     // -1 = inherit volume
            ev.legato = -1;     // -1 = no change
            ev.pitch_slide = SONG_PITCH_SLIDE_NONE; // no change
            ev.portamento = -1; // -1 = no change
            ev.vibrato = -1;    // -1 = no change
            ev.tremolo = -1;    // -1 = no change
            ev.volume_slide = SONG_VOLUME_SLIDE_NONE; // no change
            ev.note_slide = SONG_NOTE_SLIDE_NONE;     // no change
            ev.fine_pitch = -1;  // -1 = no change
            ev.hard_reset = -1;   // -1 = no change
            ev.macro_enable = -1;  // -1 = no change
            ev.macro_disable = -1; // -1 = no change
            ev.opn_effect_count = 0;
            
            if (!*p || *p == '\n') break;
            
            // Parse channel: note(3) + inst(2) + vol(2) + dots(4) = 11 chars typical
            char note_str[4] = {0};
            char inst_str[3] = {0};
            char vol_str[3] = {0};
            
            // Copy note (3 chars)
            for (int i = 0; i < 3 && *p && *p != '|' && *p != '\n'; i++) {
                note_str[i] = *p++;
            }
            // Copy instrument (2 chars)
            for (int i = 0; i < 2 && *p && *p != '|' && *p != '\n'; i++) {
                inst_str[i] = *p++;
            }
            // Copy volume (2 chars)
            for (int i = 0; i < 2 && *p && *p != '|' && *p != '\n'; i++) {
                vol_str[i] = *p++;
            }
            // Be forgiving of effect-only cells written with one missing volume
            // dot, e.g. "......0300" instead of the canonical ".......0300".
            if (vol_str[0] == '.' && vol_str[1] && vol_str[1] != '.') {
                p--;
                vol_str[1] = '.';
            }
            // Parse effect cells. Furnace-style cells are 4 chars: effect + value.
            while (*p && *p != '|' && *p != '\n') {
                char fx[5] = {0};
                int fx_len = 0;
                while (fx_len < 4 && *p && *p != '|' && *p != '\n') {
                    fx[fx_len++] = *p++;
                }

                if (fx_len == 4 && !(fx[0] == '.' && fx[1] == '.' && fx[2] == '.' && fx[3] == '.')) {
                    int value = parse_hex2(fx + 2);
                    if ((fx[0] == '0') && (fx[1] == '1') && value >= 0) {
                        ev.pitch_slide = value;
                    } else if ((fx[0] == '0') && (fx[1] == '2') && value >= 0) {
                        ev.pitch_slide = -value;
                    } else if ((fx[0] == '0') && (fx[1] == '3') && value >= 0) {
                        ev.portamento = value;
                    } else if ((fx[0] == '0') && (fx[1] == '4') && value >= 0) {
                        ev.vibrato = value;
                    } else if ((fx[0] == '0') && (fx[1] == '7') && value >= 0) {
                        ev.tremolo = value;
                    } else if ((fx[0] == '0') && (fx[1] == 'A' || fx[1] == 'a') && value >= 0) {
                        int up = (value >> 4) & 0x0F;
                        int down = value & 0x0F;
                        ev.volume_slide = up - down;
                    } else if ((fx[0] == 'E' || fx[0] == 'e') && (fx[1] == '1') && value >= 0) {
                        ev.note_slide = value;
                    } else if ((fx[0] == 'E' || fx[0] == 'e') && (fx[1] == '2') && value >= 0) {
                        ev.note_slide = -value;
                    } else if ((fx[0] == 'E' || fx[0] == 'e') && (fx[1] == '5') && value >= 0) {
                        ev.fine_pitch = value;
                    } else if ((fx[0] == 'E' || fx[0] == 'e') && (fx[1] == 'A' || fx[1] == 'a') && value >= 0) {
                        ev.legato = value != 0 ? 1 : 0;
                    } else if ((fx[0] == 'F' || fx[0] == 'f') && fx[1] == '5' && value >= 0) {
                        ev.macro_disable = value;
                    } else if ((fx[0] == 'F' || fx[0] == 'f') && fx[1] == '6' && value >= 0) {
                        ev.macro_enable = value;
                    } else if (value >= 0) {
                        auto hex = [](char c) -> int {
                            if (c >= '0' && c <= '9') return c - '0';
                            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                            return -1;
                        };
                        int hi = hex(fx[0]);
                        int lo = hex(fx[1]);
                        if (hi >= 0 && lo >= 0) {
                            int code = (hi << 4) | lo;
                            bool is_opn_effect =
                                code == 0x10 || code == 0x11 ||
                                (code >= 0x12 && code <= 0x16) ||
                                (code >= 0x19 && code <= 0x1D) ||
                                code == 0x30 ||
                                (code >= 0x50 && code <= 0x5F) ||
                                (code >= 0x60 && code <= 0x63);

                            if (code == 0x30) {
                                ev.hard_reset = value != 0 ? 1 : 0;
                            }
                            if (is_opn_effect && ev.opn_effect_count < 16) {
                                ev.opn_effects[ev.opn_effect_count].code = static_cast<uint8_t>(code);
                                ev.opn_effects[ev.opn_effect_count].value = static_cast<uint8_t>(value);
                                ev.opn_effect_count++;
                            }
                        }
                    }
                }
            }
            
            // Parse note - only if not "..."
            if (note_str[0] && note_str[0] != '.') {
                ev.note = parse_note_field(note_str);
                if (ev.note >= 0) last_note[ch] = ev.note;
                else if (ev.note <= -2) last_note[ch] = ev.note;
            }
            // If note_str is "...", ev.note stays -1 (no new note)
            
            // Parse instrument - only if not ".."
            if (inst_str[0] && inst_str[0] != '.') {
                ev.patch_id = parse_hex2(inst_str);
                if (ev.patch_id >= 0) last_inst[ch] = ev.patch_id;
            }
            // If inst_str is "..", ev.patch_id stays -1 (inherit)
            
            // Parse volume - only if not ".."
            if (vol_str[0] && vol_str[0] != '.') {
                ev.volume = parse_hex2(vol_str);
                if (ev.volume >= 0) last_vol[ch] = ev.volume;
            }
            // If vol_str is "..", ev.volume stays -1 (inherit)
            
            // Skip '|' separator
            if (*p == '|') p++;
        }
        
        // Skip to next line
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    
    m->song_present[id] = true;
    return id;
}

// Find the next row with a note on a given channel
static int find_next_note_row(XfmSongPattern& pat, int start_row, int ch)
{
    for (int row = start_row + 1; row < pat.num_rows; row++) {
        if (pat.rows[row][ch].note >= 0) {
            return row;
        }
    }
    return -1;  // No more notes
}

static void song_load_channel_patch(xfm_module* m, int ch, int patch_id, int volume)
{
    if (!m || !m->chip) return;
    if (patch_id < 0 || !m->patch_present[patch_id]) return;

    if (!m->live_patch_valid[ch] || m->live_patch_id[ch] != patch_id) {
        m->live_patches[ch] = m->patches[patch_id];
        m->live_patch_valid[ch] = true;
        m->live_patch_id[ch] = patch_id;
        m->live_op_mask[ch] = 0x0F;
    }
    m->active_song.channels[ch].live_patch_valid = true;
    m->current_patch[ch] = patch_id;
    xfm_patch_opn patch = m->live_patches[ch];
    int tl_add = ((0x7F - volume) * 127) / 0x7F;
    for (int op = 0; op < 4; op++) {
        if (song_is_carrier(patch.ALG, op)) {
            patch.op[op].TL = std::min(127, (int)patch.op[op].TL + tl_add);
        }
    }

    m->chip->load_patch(patch, ch);
    m->chip->enable_lfo(m->lfo_enable, static_cast<uint8_t>(m->lfo_freq));
}

static bool song_is_carrier(uint8_t alg, int op)
{
    switch (alg) {
        case 0: case 1: case 2: case 3: return op == 3;
        case 4: return op == 1 || op == 3;
        case 5: case 6: return op == 1 || op == 2 || op == 3;
        case 7: return true;
        default: return op == 3;
    }
}

static void song_write_channel_tremolo(xfm_module* m, int ch, int attenuation)
{
    if (!m || !m->chip) return;
    int patch_id = m->current_patch[ch];
    if (patch_id < 0 || !m->patch_present[patch_id]) return;

    const xfm_patch_opn& patch = m->live_patch_valid[ch] ? m->live_patches[ch] : m->patches[patch_id];
    int volume = m->active_song.channels[ch].current_volume;
    int tl_add = ((0x7F - volume) * 127) / 0x7F;
    const int slotMap[4] = {0, 2, 1, 3};
    uint8_t port = (ch >= 3) ? 1 : 0;
    int hwch = ch % 3;

    for (int op = 0; op < 4; op++) {
        if (!song_is_carrier(patch.ALG, op)) continue;
        if ((m->live_op_mask[ch] & (1 << op)) == 0) {
            int hwSlot = slotMap[op];
            m->chip->write(port, 0x40 + hwSlot * 4 + hwch, 0x7F);
            continue;
        }
        int hwSlot = slotMap[op];
        int tl = std::min(127, (int)patch.op[op].TL + tl_add + attenuation);
        m->chip->write(port, 0x40 + hwSlot * 4 + hwch, (uint8_t)(tl & 0x7F));
    }
}

static void song_write_opn_operator(xfm_module* m, int ch, int op)
{
    if (!m || !m->chip || ch < 0 || ch >= 6 || op < 0 || op >= 4) return;
    if (!m->live_patch_valid[ch]) return;

    const int slotMap[4] = {0, 2, 1, 3};
    const uint8_t port = (ch >= 3) ? 1 : 0;
    const int hwch = ch % 3;
    const int hwSlot = slotMap[op];
    const xfm_patch_opn_operator& o = m->live_patches[ch].op[op];

    uint8_t dt_hw = XfmChipOpn::dt_to_hw(o.DT);
    m->chip->write(port, 0x30 + hwSlot * 4 + hwch, (dt_hw << 4) | (o.MUL & 0x0F));
    int tl = o.TL & 0x7F;
    if ((m->live_op_mask[ch] & (1 << op)) == 0) {
        tl = 127;
    } else if (song_is_carrier(m->live_patches[ch].ALG, op)) {
        int volume = m->active_song.channels[ch].current_volume;
        tl = std::min(127, tl + ((0x7F - volume) * 127) / 0x7F);
    }
    m->chip->write(port, 0x40 + hwSlot * 4 + hwch, tl & 0x7F);
    m->chip->write(port, 0x50 + hwSlot * 4 + hwch, ((o.RS & 0x03) << 6) | (o.AR & 0x1F));
    m->chip->write(port, 0x60 + hwSlot * 4 + hwch, ((o.AM & 0x01) << 7) | (o.DR & 0x1F));
    m->chip->write(port, 0x70 + hwSlot * 4 + hwch, o.SR & 0x1F);
    m->chip->write(port, 0x80 + hwSlot * 4 + hwch, ((o.SL & 0x0F) << 4) | (o.RR & 0x0F));
    uint8_t ssg_hw = (o.SSG > 0) ? (0x08 | ((o.SSG - 1) & 0x07)) : 0;
    m->chip->write(port, 0x90 + hwSlot * 4 + hwch, ssg_hw);
}

static void song_write_opn_channel_regs(xfm_module* m, int ch)
{
    if (!m || !m->chip || ch < 0 || ch >= 6 || !m->live_patch_valid[ch]) return;
    const uint8_t port = (ch >= 3) ? 1 : 0;
    const int hwch = ch % 3;
    const xfm_patch_opn& patch = m->live_patches[ch];
    m->chip->write(port, 0xB0 + hwch, ((patch.FB & 0x07) << 3) | (patch.ALG & 0x07));
    m->chip->write(port, 0xB4 + hwch, 0xC0 | ((patch.AMS & 0x03) << 4) | (patch.FMS & 0x07));
}

static void song_set_opn_operator_tl(xfm_module* m, int ch, int op, int value);
static void song_write_channel_frequency(xfm_module* m, int ch, const XfmSongChannel& ch_state);

static uint32_t song_macro_target_mask(int target)
{
    if (target <= XFM_MACRO_NONE || target >= XFM_MACRO_TARGET_COUNT) return 0;
    return 1u << target;
}

static void song_apply_macro_value(xfm_module* m, int ch, int target, int value)
{
    if (!m || ch < 0 || ch >= 6) return;
    XfmSongChannel& ch_state = m->active_song.channels[ch];

    if (target >= XFM_MACRO_TL1 && target <= XFM_MACRO_TL4) {
        song_set_opn_operator_tl(m, ch, target - XFM_MACRO_TL1, value);
        return;
    }
    if (target >= XFM_MACRO_MUL1 && target <= XFM_MACRO_MUL4) {
        int op = target - XFM_MACRO_MUL1;
        if (!m->live_patch_valid[ch]) return;
        m->live_patches[ch].op[op].MUL = static_cast<uint8_t>(std::min(15, std::max(0, value)));
        song_write_opn_operator(m, ch, op);
        return;
    }
    if (target >= XFM_MACRO_DT1 && target <= XFM_MACRO_DT4) {
        int op = target - XFM_MACRO_DT1;
        if (!m->live_patch_valid[ch]) return;
        m->live_patches[ch].op[op].DT = static_cast<int8_t>(std::min(3, std::max(-3, value)));
        song_write_opn_operator(m, ch, op);
        return;
    }
    if (target == XFM_MACRO_FB) {
        if (!m->live_patch_valid[ch]) return;
        m->live_patches[ch].FB = static_cast<uint8_t>(std::min(7, std::max(0, value)));
        song_write_opn_channel_regs(m, ch);
        return;
    }
    if (target == XFM_MACRO_ARP) {
        ch_state.arp_offset = value;
        if (m->channel_active[ch]) {
            song_write_channel_frequency(m, ch, ch_state);
        }
    }
}

static void song_start_patch_macros(xfm_module* m, int ch, int patch_id)
{
    if (!m || ch < 0 || ch >= 6 || patch_id < 0 || patch_id > 255) return;
    XfmSongChannel& ch_state = m->active_song.channels[ch];

    ch_state.arp_offset = 0;
    ch_state.sample_in_tick = 0;
    for (int target = 0; target < XFM_MACRO_TARGET_COUNT; target++) {
        XfmMacroState& state = ch_state.macro_states[target];
        state.macro_id = -1;
        state.pos = 0;
        state.active = false;
        state.released = false;
    }

    for (int target = 1; target < XFM_MACRO_TARGET_COUNT; target++) {
        int macro_id = m->patch_macros[patch_id][target];
        if (macro_id < 0 || macro_id >= XFM_MAX_MACROS || !m->macro_present[macro_id]) continue;
        if ((ch_state.macro_disabled_mask & song_macro_target_mask(target)) != 0) continue;

        XfmMacroState& state = ch_state.macro_states[target];
        state.macro_id = macro_id;
        state.pos = 0;
        state.active = true;
        state.released = false;
        song_apply_macro_value(m, ch, target, m->macros[macro_id].values[0]);
    }
}

static void song_set_macro_enabled(xfm_module* m, int ch, int target, bool enabled)
{
    if (!m || ch < 0 || ch >= 6) return;
    XfmSongChannel& ch_state = m->active_song.channels[ch];

    int begin = target == 0 ? 1 : target;
    int end = target == 0 ? XFM_MACRO_TARGET_COUNT - 1 : target;
    if (begin <= 0 || end >= XFM_MACRO_TARGET_COUNT) return;

    for (int t = begin; t <= end; t++) {
        uint32_t mask = song_macro_target_mask(t);
        XfmMacroState& state = ch_state.macro_states[t];
        if (!enabled) {
            ch_state.macro_disabled_mask |= mask;
            state.active = false;
            state.released = false;
            continue;
        }

        ch_state.macro_disabled_mask &= ~mask;
        int patch_id = ch_state.current_patch;
        if (patch_id < 0 || patch_id > 255) continue;
        int macro_id = m->patch_macros[patch_id][t];
        if (macro_id < 0 || macro_id >= XFM_MAX_MACROS || !m->macro_present[macro_id]) continue;
        state.macro_id = macro_id;
        state.pos = 0;
        state.active = true;
        state.released = false;
        song_apply_macro_value(m, ch, t, m->macros[macro_id].values[0]);
    }
}

static void song_advance_macros(xfm_module* m, int frames)
{
    if (!m || !m->active_song.active || frames <= 0) return;
    if (m->active_song.song_id <= 0 || m->active_song.song_id > 15) return;
    if (!m->song_present[m->active_song.song_id]) return;

    XfmSongPattern& pat = m->song_patterns[m->active_song.song_id];
    int samples_per_tick = std::max(1, m->sample_rate / std::max(1, pat.tick_rate));

    for (int ch = 0; ch < pat.num_channels && ch < 6; ch++) {
        XfmSongChannel& ch_state = m->active_song.channels[ch];
        ch_state.sample_in_tick += frames;

        while (ch_state.sample_in_tick >= samples_per_tick) {
            ch_state.sample_in_tick -= samples_per_tick;

            for (int target = 1; target < XFM_MACRO_TARGET_COUNT; target++) {
                XfmMacroState& state = ch_state.macro_states[target];
                if (!state.active) continue;
                if (state.macro_id < 0 || state.macro_id >= XFM_MAX_MACROS) continue;
                if (!m->macro_present[state.macro_id]) continue;

                const XfmMacro& macro = m->macros[state.macro_id];
                if (macro.length == 0) continue;

                int next_pos = state.pos + 1;
                int loop_end = macro.length;
                if (!state.released && macro.release_start != 0xFF && macro.release_start <= macro.length) {
                    loop_end = std::max(1, (int)macro.release_start);
                }
                if (!state.released && macro.has_loop && next_pos >= loop_end) {
                    next_pos = macro.loop_start;
                } else if (next_pos >= macro.length) {
                    if (!state.released && macro.has_loop) next_pos = macro.loop_start;
                    else {
                        state.active = false;
                        continue;
                    }
                }
                state.pos = static_cast<uint8_t>(next_pos);
                song_apply_macro_value(m, ch, target, macro.values[state.pos]);
            }
        }
    }
}

static void song_release_macros(xfm_module* m, int ch)
{
    if (!m || ch < 0 || ch >= 6) return;
    XfmSongChannel& ch_state = m->active_song.channels[ch];

    for (int target = 1; target < XFM_MACRO_TARGET_COUNT; target++) {
        XfmMacroState& state = ch_state.macro_states[target];
        if (!state.active) continue;
        if (state.macro_id < 0 || state.macro_id >= XFM_MAX_MACROS) continue;
        if (!m->macro_present[state.macro_id]) continue;

        const XfmMacro& macro = m->macros[state.macro_id];
        state.released = true;
        if (macro.release_start == 0xFF || macro.release_start >= macro.length) continue;

        state.pos = macro.release_start;
        song_apply_macro_value(m, ch, target, macro.values[state.pos]);
    }
}

static void song_stop_active_macros(xfm_module* m, int ch)
{
    if (!m || ch < 0 || ch >= 6) return;
    XfmSongChannel& ch_state = m->active_song.channels[ch];

    for (int target = 0; target < XFM_MACRO_TARGET_COUNT; target++) {
        XfmMacroState& state = ch_state.macro_states[target];
        state.active = false;
        state.released = false;
    }
    ch_state.arp_offset = 0;
}

static int song_opn_dt_from_furnace(int value)
{
    static const int map[8] = {-3, -2, -1, 0, 1, 2, 3, 0};
    return map[value & 7];
}

static void song_apply_opn_to_ops(xfm_module* m, int ch, int op_selector,
                                  void (*apply)(xfm_patch_opn_operator&, int), int value)
{
    if (!m || ch < 0 || ch >= 6 || !m->live_patch_valid[ch]) return;
    if (op_selector == 0) {
        for (int op = 0; op < 4; op++) {
            apply(m->live_patches[ch].op[op], value);
            song_write_opn_operator(m, ch, op);
        }
        return;
    }
    if (op_selector >= 1 && op_selector <= 4) {
        int op = op_selector - 1;
        apply(m->live_patches[ch].op[op], value);
        song_write_opn_operator(m, ch, op);
    }
}

static void song_set_opn_operator_tl(xfm_module* m, int ch, int op, int value)
{
    if (!m || ch < 0 || ch >= 6 || op < 0 || op >= 4 || !m->live_patch_valid[ch]) return;
    m->live_patches[ch].op[op].TL = static_cast<uint8_t>(std::min(127, std::max(0, value)));
    if (song_is_carrier(m->live_patches[ch].ALG, op)) song_write_channel_tremolo(m, ch, 0);
    else song_write_opn_operator(m, ch, op);
}

static void song_apply_opn_effect(xfm_module* m, int ch, const XfmSongOpnEffect& effect)
{
    if (!m || !m->chip || ch < 0 || ch >= 6) return;

    int patch_id = m->active_song.channels[ch].current_patch;
    if (!m->live_patch_valid[ch] && patch_id >= 0 && m->patch_present[patch_id]) {
        m->live_patches[ch] = m->patches[patch_id];
        m->live_patch_valid[ch] = true;
        m->live_patch_id[ch] = patch_id;
        m->active_song.channels[ch].live_patch_valid = true;
    }
    if (!m->live_patch_valid[ch]) return;

    int code = effect.code;
    int value = effect.value;
    int x = (value >> 4) & 0x0F;
    int y = value & 0x0F;

    if (code == 0x10) {
        xfm_module_set_lfo(m, x != 0, y);
        return;
    }
    if (code == 0x11) {
        m->live_patches[ch].FB = static_cast<uint8_t>(value & 0x07);
        song_write_opn_channel_regs(m, ch);
        return;
    }
    if (code >= 0x12 && code <= 0x15) {
        song_set_opn_operator_tl(m, ch, code - 0x12, value);
        return;
    }
    if (code == 0x16) {
        if (x >= 1 && x <= 4) {
            int op = x - 1;
            m->live_patches[ch].op[op].MUL = static_cast<uint8_t>(y & 0x0F);
            song_write_opn_operator(m, ch, op);
        }
        return;
    }
    if (code >= 0x19 && code <= 0x1D) {
        int target = (code == 0x19) ? 0 : (code - 0x19);
        song_apply_opn_to_ops(m, ch, target, [](xfm_patch_opn_operator& op, int v) {
            op.AR = static_cast<uint8_t>(std::min(31, std::max(0, v)));
        }, value);
        return;
    }
    if (code == 0x50) {
        song_apply_opn_to_ops(m, ch, x, [](xfm_patch_opn_operator& op, int v) {
            op.AM = static_cast<uint8_t>(v ? 1 : 0);
        }, y);
        return;
    }
    if (code == 0x51) {
        song_apply_opn_to_ops(m, ch, x, [](xfm_patch_opn_operator& op, int v) {
            op.SL = static_cast<uint8_t>(std::min(15, std::max(0, v)));
        }, y);
        return;
    }
    if (code == 0x52) {
        song_apply_opn_to_ops(m, ch, x, [](xfm_patch_opn_operator& op, int v) {
            op.RR = static_cast<uint8_t>(std::min(15, std::max(0, v)));
        }, y);
        return;
    }
    if (code == 0x53) {
        song_apply_opn_to_ops(m, ch, x, [](xfm_patch_opn_operator& op, int v) {
            op.DT = static_cast<int8_t>(song_opn_dt_from_furnace(v));
        }, y);
        return;
    }
    if (code == 0x54) {
        song_apply_opn_to_ops(m, ch, x, [](xfm_patch_opn_operator& op, int v) {
            op.RS = static_cast<uint8_t>(std::min(3, std::max(0, v)));
        }, y);
        return;
    }
    if (code == 0x55) {
        song_apply_opn_to_ops(m, ch, x, [](xfm_patch_opn_operator& op, int v) {
            op.SSG = static_cast<uint8_t>((v >= 0 && v <= 7) ? (v + 1) : 0);
        }, y);
        return;
    }
    if (code >= 0x56 && code <= 0x5A) {
        int target = (code == 0x56) ? 0 : (code - 0x56);
        song_apply_opn_to_ops(m, ch, target, [](xfm_patch_opn_operator& op, int v) {
            op.DR = static_cast<uint8_t>(std::min(31, std::max(0, v)));
        }, value);
        return;
    }
    if (code >= 0x5B && code <= 0x5F) {
        int target = (code == 0x5B) ? 0 : (code - 0x5B);
        song_apply_opn_to_ops(m, ch, target, [](xfm_patch_opn_operator& op, int v) {
            op.SR = static_cast<uint8_t>(std::min(31, std::max(0, v)));
        }, value);
        return;
    }
    if (code == 0x60) {
        if (x == 0) {
            m->live_op_mask[ch] = static_cast<uint8_t>(y & 0x0F);
            for (int op = 0; op < 4; op++) song_write_opn_operator(m, ch, op);
        } else if (x >= 1 && x <= 4) {
            int op = x - 1;
            if (y) m->live_op_mask[ch] |= static_cast<uint8_t>(1 << op);
            else m->live_op_mask[ch] &= static_cast<uint8_t>(~(1 << op));
            song_write_opn_operator(m, ch, op);
        }
        return;
    }
    if (code == 0x61) {
        m->live_patches[ch].ALG = static_cast<uint8_t>(value & 0x07);
        song_write_opn_channel_regs(m, ch);
        song_write_channel_tremolo(m, ch, 0);
        return;
    }
    if (code == 0x62) {
        m->live_patches[ch].FMS = static_cast<uint8_t>(value & 0x07);
        song_write_opn_channel_regs(m, ch);
        return;
    }
    if (code == 0x63) {
        m->live_patches[ch].AMS = static_cast<uint8_t>(value & 0x03);
        song_write_opn_channel_regs(m, ch);
    }
}

static double song_channel_effective_hz(const XfmSongChannel& ch_state)
{
    double hz = ch_state.current_hz;
    if (hz <= 0.0) return hz;

    if (ch_state.arp_offset != 0) {
        hz *= std::pow(2.0, ch_state.arp_offset / 12.0);
    }
    if (ch_state.fine_pitch_cents != 0) {
        hz *= std::pow(2.0, ch_state.fine_pitch_cents / 1200.0);
    }
    if (ch_state.vibrato_depth > 0) {
        double cents = std::sin(ch_state.vibrato_phase) * (ch_state.vibrato_depth * 7.0);
        hz *= std::pow(2.0, cents / 1200.0);
    }
    return hz;
}

static void song_write_channel_frequency(xfm_module* m, int ch, const XfmSongChannel& ch_state)
{
    double hz = song_channel_effective_hz(ch_state);
    if (hz > 0.0) {
        m->chip->set_frequency(ch, hz, 0);
    }
}

static void song_key_on_channel(xfm_module* m, int ch, XfmSongChannel& ch_state)
{
    if (ch_state.pending_patch < 0) return;
    if (!m->patch_present[ch_state.pending_patch]) return;

    song_load_channel_patch(m, ch, ch_state.pending_patch, ch_state.current_volume);
    ch_state.base_note = ch_state.pending_note;
    song_start_patch_macros(m, ch, ch_state.pending_patch);
    double hz = note_to_hz(ch_state.base_note);
    ch_state.current_hz = hz;
    ch_state.target_hz = hz;
    ch_state.portamento_active = false;
    ch_state.note_slide_active = false;
    song_write_channel_frequency(m, ch, ch_state);
    m->chip->key_on(ch);
    m->channel_active[ch] = true;
    ch_state.pending_has_note = false;
    ch_state.wait_for_next_row = false;
    ch_state.pending_gap = get_min_gap_samples(m->sample_rate);
}

static void song_start_portamento(xfm_module* m, XfmSongPattern& pat, int ch,
                                  XfmSongChannel& ch_state, int target_note)
{
    double target_hz = note_to_hz(target_note);
    if (!m->channel_active[ch] || ch_state.current_hz <= 0.0 || ch_state.portamento_speed <= 0) {
        ch_state.pending_has_note = true;
        ch_state.pending_note = target_note;
        ch_state.pending_patch = ch_state.current_patch;
        ch_state.pending_volume = ch_state.current_volume;
        ch_state.pending_gap = 0;
        ch_state.wait_for_next_row = false;
        return;
    }

    if (ch_state.current_patch >= 0 && m->current_patch[ch] != ch_state.current_patch) {
        song_load_channel_patch(m, ch, ch_state.current_patch, ch_state.current_volume);
    }

    int duration = std::max(1, (pat.samples_per_row * 16) / std::max(1, ch_state.portamento_speed));
    ch_state.target_hz = target_hz;
    ch_state.portamento_target_note = target_note;
    ch_state.portamento_step_hz = (target_hz - ch_state.current_hz) / duration;
    ch_state.portamento_active = true;
    ch_state.pending_has_note = false;
    ch_state.wait_for_next_row = false;
}

static void song_start_note_slide(XfmSongPattern& pat, XfmSongChannel& ch_state, int packed)
{
    if (packed == 0 || ch_state.current_hz <= 0.0) {
        ch_state.note_slide_active = false;
        return;
    }

    int value = std::abs(packed);
    int speed = (value >> 4) & 0x0F;
    int semitones = value & 0x0F;
    if (semitones <= 0) {
        ch_state.note_slide_active = false;
        return;
    }

    double direction = packed > 0 ? 1.0 : -1.0;
    double target_hz = ch_state.current_hz * std::pow(2.0, direction * semitones / 12.0);
    int duration = std::max(1, (pat.samples_per_row * 16) / std::max(1, speed));
    ch_state.note_slide_target_hz = target_hz;
    ch_state.note_slide_step_hz = (target_hz - ch_state.current_hz) / duration;
    ch_state.note_slide_active = true;
    ch_state.portamento_active = false;
}

static bool song_apply_legato_note(xfm_module* m, int ch, XfmSongChannel& ch_state, int note)
{
    if (!m->channel_active[ch] || ch_state.current_hz <= 0.0) return false;

    if (ch_state.current_patch >= 0 && m->current_patch[ch] != ch_state.current_patch) {
        song_load_channel_patch(m, ch, ch_state.current_patch, ch_state.current_volume);
    }

    double hz = note_to_hz(note);
    ch_state.base_note = note;
    ch_state.current_hz = hz;
    ch_state.target_hz = hz;
    ch_state.portamento_active = false;
    ch_state.note_slide_active = false;
    ch_state.portamento_target_note = note;
    song_write_channel_frequency(m, ch, ch_state);
    ch_state.pending_has_note = false;
    ch_state.wait_for_next_row = false;
    return true;
}

// Process a row for the active song - called at END of previous row
// Sets up pending notes. Key-off of previous notes happens at sample 1 of new row.
static void song_process_row(xfm_module* m, int row_idx)
{
    XfmActiveSong& song = m->active_song;
    if (song.song_id <= 0 || song.song_id > 15) return;
    if (!m->song_present[song.song_id]) return;

    XfmSongPattern& pat = m->song_patterns[song.song_id];
    if (row_idx >= pat.num_rows) return;

    // Process each channel
    for (int ch = 0; ch < pat.num_channels && ch < 6; ch++) {
        XfmSongEvent& ev = pat.rows[row_idx][ch];
        XfmSongChannel& ch_state = song.channels[ch];

        // Update instrument and volume if specified
        if (ev.patch_id >= 0) {
            ch_state.current_patch = ev.patch_id;
            m->live_patch_valid[ch] = false;
            m->live_patch_id[ch] = -1;
            m->live_op_mask[ch] = 0x0F;
            ch_state.live_patch_valid = false;
        }
        if (ev.volume >= 0) {
            ch_state.current_volume = ev.volume;
            ch_state.current_volume_f = ev.volume;
            if (m->channel_active[ch]) {
                song_write_channel_tremolo(m, ch, 0);
            }
        }
        if (ev.legato >= 0) ch_state.legato_enabled = ev.legato != 0;
        if (ev.pitch_slide != SONG_PITCH_SLIDE_NONE) {
            ch_state.pitch_slide_speed = ev.pitch_slide;
            ch_state.portamento_active = false;
            ch_state.note_slide_active = false;
        }
        if (ev.portamento >= 0) {
            ch_state.portamento_speed = ev.portamento;
            ch_state.pitch_slide_speed = 0;
            ch_state.note_slide_active = false;
            if (ev.portamento == 0) {
                ch_state.portamento_active = false;
            }
        }
        if (ev.vibrato >= 0) {
            ch_state.vibrato_speed = (ev.vibrato >> 4) & 0x0F;
            ch_state.vibrato_depth = ev.vibrato & 0x0F;
            if (ev.vibrato == 0) {
                ch_state.vibrato_phase = 0.0;
            }
        }
        if (ev.tremolo >= 0) {
            ch_state.tremolo_speed = (ev.tremolo >> 4) & 0x0F;
            ch_state.tremolo_depth = ev.tremolo & 0x0F;
            if (ev.tremolo == 0) {
                ch_state.tremolo_phase = 0.0;
                song_write_channel_tremolo(m, ch, 0);
            }
        }
        if (ev.volume_slide != SONG_VOLUME_SLIDE_NONE) {
            ch_state.volume_slide_speed = ev.volume_slide;
        }
        if (ev.note_slide != SONG_NOTE_SLIDE_NONE) {
            song_start_note_slide(pat, ch_state, ev.note_slide);
            ch_state.pitch_slide_speed = 0;
        }
        if (ev.fine_pitch >= 0) {
            ch_state.fine_pitch_cents = ev.fine_pitch - 0x80;
            if (m->channel_active[ch]) {
                song_write_channel_frequency(m, ch, ch_state);
            }
        }
        if (ev.hard_reset >= 0) {
            ch_state.envelope_hard_reset = ev.hard_reset != 0;
        }
        if (ev.macro_disable >= 0) {
            song_set_macro_enabled(m, ch, ev.macro_disable, false);
        }
        if (ev.macro_enable >= 0) {
            song_set_macro_enabled(m, ch, ev.macro_enable, true);
        }
        for (int i = 0; i < ev.opn_effect_count; i++) {
            song_apply_opn_effect(m, ch, ev.opn_effects[i]);
        }

        if (ev.note == -2 || ev.note == -3 || ev.note == -4) {
            // OFF/REL release the envelope; === hard-cuts the channel.
            if (m->channel_active[ch]) {
                if (ev.note == -4) m->chip->hard_mute(ch);
                else m->chip->key_off(ch);
                m->channel_active[ch] = false;
            }
            ch_state.pending_has_note = false;
            ch_state.pending_is_off = false;
            ch_state.pending_gap = get_min_gap_samples(m->sample_rate);
            ch_state.portamento_active = false;
            ch_state.pitch_slide_speed = 0;
            ch_state.volume_slide_speed = 0;
            ch_state.note_slide_active = false;
            ch_state.arp_offset = 0;
            ch_state.vibrato_speed = 0;
            ch_state.vibrato_depth = 0;
            ch_state.tremolo_speed = 0;
            ch_state.tremolo_depth = 0;
            if (ev.note == -3) song_release_macros(m, ch);
            else song_stop_active_macros(m, ch);
        } else if (ev.note >= 0) {
            if (ch_state.portamento_speed > 0 && m->channel_active[ch]) {
                song_start_portamento(m, pat, ch, ch_state, ev.note);
                continue;
            }

            if (ch_state.legato_enabled && song_apply_legato_note(m, ch, ch_state, ev.note)) {
                continue;
            }

            // New note on this channel
            // Check if channel is still playing from previous row
            bool was_playing = m->channel_active[ch];
            
            // Key off happens at sample 1 of this row in update_song()
            // The new note will wait until end of this row (simulates manual OFF row)
            ch_state.wait_for_next_row = was_playing;

            if (!was_playing) {
                ch_state.pending_gap = 0;
            } else {
                // Look ahead to find next note on this channel
                int next_note_row = find_next_note_row(pat, row_idx, ch);

                // Calculate gap for clean release
                if (next_note_row >= 0) {
                    int rows_until_next = next_note_row - row_idx;
                    ch_state.pending_gap = get_dynamic_gap(m->sample_rate, rows_until_next, pat.samples_per_row);
                } else {
                    ch_state.pending_gap = get_min_gap_samples(m->sample_rate);
                }
            }

            // Mark for key-on after gap (or at end of row if waiting)
            ch_state.pending_has_note = true;
            ch_state.pending_note = ev.note;
            ch_state.pending_patch = ch_state.current_patch;
            ch_state.pending_volume = ch_state.current_volume;
        } else {
            // ev.note == -1: no new note, keep previous state
        }
    }
}

static void song_reset_active_channels(xfm_module* m)
{
    if (!m || !m->chip) return;
    for (int ch = 0; ch < 6; ch++) {
        m->chip->key_off(ch);
        m->channel_active[ch] = false;
        m->current_patch[ch] = -1;
        m->live_patch_valid[ch] = false;
        m->live_patch_id[ch] = -1;
        m->live_op_mask[ch] = 0x0F;

        XfmSongChannel& ch_state = m->active_song.channels[ch];
        ch_state.current_patch = -1;
        ch_state.current_volume = 127;
        ch_state.current_volume_f = 127.0;
        ch_state.pending_has_note = false;
        ch_state.pending_is_off = false;
        ch_state.pending_gap = get_min_gap_samples(m->sample_rate);
        ch_state.wait_for_next_row = false;
        ch_state.legato_enabled = false;
        ch_state.pitch_slide_speed = 0;
        ch_state.portamento_active = false;
        ch_state.portamento_speed = 0;
        ch_state.portamento_target_note = -1;
        ch_state.current_hz = 0.0;
        ch_state.target_hz = 0.0;
        ch_state.portamento_step_hz = 0.0;
        ch_state.volume_slide_speed = 0;
        ch_state.note_slide_active = false;
        ch_state.note_slide_target_hz = 0.0;
        ch_state.note_slide_step_hz = 0.0;
        ch_state.fine_pitch_cents = 0;
        ch_state.vibrato_speed = 0;
        ch_state.vibrato_depth = 0;
        ch_state.vibrato_phase = 0.0;
        ch_state.tremolo_speed = 0;
        ch_state.tremolo_depth = 0;
        ch_state.tremolo_phase = 0.0;
        ch_state.envelope_hard_reset = false;
        ch_state.base_note = -1;
        ch_state.arp_offset = 0;
        ch_state.sample_in_tick = 0;
        ch_state.macro_disabled_mask = 0;
        for (int target = 0; target < XFM_MACRO_TARGET_COUNT; target++) {
            ch_state.macro_states[target].macro_id = -1;
            ch_state.macro_states[target].pos = 0;
            ch_state.macro_states[target].active = false;
            ch_state.macro_states[target].released = false;
        }
        ch_state.live_patch_valid = false;
    }
}

static void song_jump_to_row(xfm_module* m, int row)
{
    if (!m || !m->active_song.active) return;
    int song_id = m->active_song.song_id;
    if (song_id <= 0 || song_id > 15 || !m->song_present[song_id]) return;
    XfmSongPattern& pat = m->song_patterns[song_id];
    row = std::max(0, std::min(row, pat.num_rows - 1));
    song_reset_active_channels(m);
    m->active_song.current_row = row;
    m->active_song.sample_in_row = 0;
    m->active_song.rows_remaining =
        std::max(1, m->active_song.loop_end_row - m->active_song.loop_start_row + 1);
    song_process_row(m, row);
}

// Commit pending notes for active song (after gap samples)
// If wait_for_next_row is set, keyon happens at end of row instead
static void song_commit_keyon(xfm_module* m, int current_gap)
{
    XfmActiveSong& song = m->active_song;
    if (song.song_id <= 0 || song.song_id > 15) return;

    for (int ch = 0; ch < 6; ch++) {
        XfmSongChannel& ch_state = song.channels[ch];
        if (!ch_state.pending_has_note) continue;
        if (ch_state.pending_patch < 0) continue;
        if (!m->patch_present[ch_state.pending_patch]) continue;
        
        // If waiting for next row, skip keyon here (happens at row boundary)
        if (ch_state.wait_for_next_row) {
            continue;
        }

        // Wait until gap has fully elapsed before keying on
        // This ensures previous note's release envelope completes
        if (current_gap < ch_state.pending_gap) {
            continue;
        }

        song_key_on_channel(m, ch, ch_state);
    }
}

// Start a song
void xfm_song_play(xfm_module* m, xfm_song_id id, bool loop)
{
    if (!m || !m->chip) return;
    if (id <= 0 || id > 15) return;
    if (!m->song_present[id]) return;

    XfmSongPattern& pat = m->song_patterns[id];

    // Stop current song and reset all channels
    for (int ch = 0; ch < 6; ch++) {
        m->chip->key_off(ch);
        m->channel_active[ch] = false;
        m->current_patch[ch] = -1;
        m->live_patch_valid[ch] = false;
        m->live_patch_id[ch] = -1;
        m->live_op_mask[ch] = 0x0F;
    }

    // Initialize active song - start at sample 0
    m->active_song.song_id = id;
    m->active_song.current_row = 0;
    m->active_song.sample_in_row = 0;
    m->active_song.rows_remaining = pat.num_rows;
    m->active_song.loop_start_row = 0;
    m->active_song.loop_end_row = pat.num_rows - 1;
    m->active_song.active = true;
    m->active_song.loop = loop;

    // Initialize channels completely
    for (int ch = 0; ch < 6; ch++) {
        m->active_song.channels[ch].current_patch = -1;
        m->active_song.channels[ch].current_volume = 127;
        m->active_song.channels[ch].pending_has_note = false;
        m->active_song.channels[ch].pending_is_off = false;
        m->active_song.channels[ch].pending_gap = get_min_gap_samples(m->sample_rate);
        m->active_song.channels[ch].wait_for_next_row = false;
        m->active_song.channels[ch].legato_enabled = false;
        m->active_song.channels[ch].pitch_slide_speed = 0;
        m->active_song.channels[ch].portamento_active = false;
        m->active_song.channels[ch].portamento_speed = 0;
        m->active_song.channels[ch].portamento_target_note = -1;
        m->active_song.channels[ch].current_hz = 0.0;
        m->active_song.channels[ch].target_hz = 0.0;
        m->active_song.channels[ch].portamento_step_hz = 0.0;
        m->active_song.channels[ch].volume_slide_speed = 0;
        m->active_song.channels[ch].note_slide_active = false;
        m->active_song.channels[ch].note_slide_target_hz = 0.0;
        m->active_song.channels[ch].note_slide_step_hz = 0.0;
        m->active_song.channels[ch].fine_pitch_cents = 0;
        m->active_song.channels[ch].vibrato_speed = 0;
        m->active_song.channels[ch].vibrato_depth = 0;
        m->active_song.channels[ch].vibrato_phase = 0.0;
        m->active_song.channels[ch].tremolo_speed = 0;
        m->active_song.channels[ch].tremolo_depth = 0;
        m->active_song.channels[ch].tremolo_phase = 0.0;
        m->active_song.channels[ch].envelope_hard_reset = false;
        m->active_song.channels[ch].base_note = -1;
        m->active_song.channels[ch].arp_offset = 0;
        m->active_song.channels[ch].sample_in_tick = 0;
        m->active_song.channels[ch].macro_disabled_mask = 0;
        for (int target = 0; target < XFM_MACRO_TARGET_COUNT; target++) {
            m->active_song.channels[ch].macro_states[target].macro_id = -1;
            m->active_song.channels[ch].macro_states[target].pos = 0;
            m->active_song.channels[ch].macro_states[target].active = false;
            m->active_song.channels[ch].macro_states[target].released = false;
        }
        m->active_song.channels[ch].live_patch_valid = false;
    }

    // Process first row (sets up pending notes - will be triggered in update_song)
    song_process_row(m, 0);
}

void xfm_song_set_loop_range(xfm_module* m, int loop_start, int loop_end)
{
    if (!m || !m->active_song.active) return;
    int song_id = m->active_song.song_id;
    if (song_id <= 0 || song_id > 15 || !m->song_present[song_id]) return;

    XfmSongPattern& pat = m->song_patterns[song_id];
    int start = std::max(0, std::min(loop_start, loop_end));
    int end = std::min(pat.num_rows - 1, std::max(loop_start, loop_end));
    m->active_song.loop_start_row = start;
    m->active_song.loop_end_row = end;
    m->active_song.rows_remaining = std::max(1, end - start + 1);

    if (m->active_song.current_row < start || m->active_song.current_row > end) {
        song_jump_to_row(m, start);
    }
}

// Schedule song change
void xfm_song_schedule(xfm_module* m, xfm_song_id id, xfm_song_switch_timing timing)
{
    if (!m || id <= 0 || id > 15) return;
    if (!m->song_present[id]) return;
    
    m->pending_song.song_id = id;
    m->pending_song.timing = timing;
    m->pending_song.pending = true;
}

// Get current row
int xfm_song_get_row(xfm_module* m)
{
    if (!m || !m->active_song.active) return 0;
    return m->active_song.current_row;
}

// Get total rows
int xfm_song_get_total_rows(xfm_module* m, xfm_song_id id)
{
    if (!m || id <= 0 || id > 15) return 0;
    if (!m->song_present[id]) return 0;
    return m->song_patterns[id].num_rows;
}

// Set automatic note-off delay
void xfm_set_auto_off_delay(xfm_module* m, float delay)
{
    if (!m) return;
    // Clamp to valid range [0.0, 1.0]
    m->auto_off_delay = std::max(0.0f, std::min(1.0f, delay));
}

// Get automatic note-off delay
float xfm_get_auto_off_delay(xfm_module* m)
{
    return m->auto_off_delay;
}

// =============================================================================
// SONG PROCESSING
// =============================================================================

/**
 * @brief Advance song position and trigger notes.
 * 
 * This function is called every audio callback to advance the active song
 * by the number of samples in the output buffer.
 * 
 * ─────────────────────────────────────────────────────────────────────────────
 * 📊 CALL RATE
 * ─────────────────────────────────────────────────────────────────────────────
 * 
 * Called: Once per audio callback (~172 Hz for 256 frames @ 44100 Hz)
 * From:   xfm_mix() or xfm_mix_song()
 * Thread: SDL audio thread
 * 
 * ─────────────────────────────────────────────────────────────────────────────
 * 🔄 PROCESSING FLOW
 * ─────────────────────────────────────────────────────────────────────────────
 * 
 * For each sample in num_samples:
 * 
 *   1. Check for pending song change (FM_SONG_SWITCH_NOW)
 *      - Immediate switch, no waiting
 * 
 *   2. Increment sample_in_row counter
 *      - Tracks position within current row
 *      - Range: 0 to samples_per_row-1
 * 
 *   3. song_commit_keyon() - Trigger pending notes
 *      - Checks if gap has elapsed for each channel
 *      - Calls chip->load_patch() for new notes
 *      - Calls chip->set_frequency() for pitch
 *      - Calls chip->key_on() to start envelope
 *
 *   4. Check end of row
 *      - If sample_in_row >= samples_per_row:
 *        a. Reset sample_in_row = 0
 *        b. Increment current_row
 *        c. Check for pending song change (FM_SONG_SWITCH_ROW)
 *        d. Check end of song (loop or stop)
 *        e. song_process_row() - Parse next row pattern
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * ⏱️  NOTE TRIGGER TIMING
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * Song Row Structure:
 *   ┌──────────────────────────────────────────┐
 *   │ Row N     │ Gap │ Note │ Sustain │ Gap   │ → Row N+1
 *   └──────────────────────────────────────────┘
 *              ↑              ↑
 *         key_on()      key_off() or next note
 * 
 * Gap Timing:
 *   - Gap provides clean transitions between notes
 *   - Prevents clicking/popping
 *   - Duration: samples_per_row / num_rows per row
 * 
 * Key-On Event:
 *   - Triggered at END of gap (when sample_in_row reaches commit point)
 *   - Loads patch into voice
 *   - Sets frequency from note value
 *   - Starts envelope (AR → DR → SR/SL)
 * 
 * Key-Off Event:
 *   - Triggered by "OFF" row entry
 *   - Or when new note starts on same channel
 *   - Starts release phase (RR)
 * 
 * ─────────────────────────────────────────────────────────────────────────────
 * 📦 STATE VARIABLES (XfmActiveSong)
 * ─────────────────────────────────────────────────────────────────────────────
 * 
 * @param song_id       Current song ID (1-15, 0 = no song)
 * @param active        Song is playing
 * @param loop          Loop at end of song
 * @param current_row   Current row index (0 to num_rows-1)
 * @param sample_in_row Sample position within row (0 to samples_per_row-1)
 * @param rows_remaining Rows remaining in song
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * @param m Module instance
 * @param num_samples Number of samples to advance ( = frames from audio callback)
 */
static void update_song(xfm_module* m, int num_samples)
{
    XfmActiveSong& song = m->active_song;
    if (!song.active) return;
    if (song.song_id <= 0 || song.song_id > 15) return;
    if (!m->song_present[song.song_id]) return;

    XfmSongPattern& pat = m->song_patterns[song.song_id];

    for (int i = 0; i < num_samples; i++) {
        // Check for pending song change with NOW timing
        if (m->pending_song.pending && m->pending_song.timing == FM_SONG_SWITCH_NOW) {
            xfm_song_play(m, m->pending_song.song_id, song.loop);
            m->pending_song.pending = false;
            continue;
        }

        song.sample_in_row++;

        // At configured position in row, key off active notes that have a new note pending
        // This gives previous note time to play before release envelope starts
        // Default delay is 0.3 (30% of row continues playing, 70% for release)
        int off_sample = (int)(pat.samples_per_row * m->auto_off_delay);
        if (off_sample < 1) off_sample = 1;  // Ensure at least sample 1
        if (song.sample_in_row == off_sample) {
            for (int ch = 0; ch < 6; ch++) {
                XfmSongChannel& ch_state = song.channels[ch];
                // Key off if channel is active AND there's a new note waiting
                if (m->channel_active[ch] && ch_state.pending_has_note) {
                    if (ch_state.envelope_hard_reset) m->chip->hard_mute(ch);
                    else m->chip->key_off(ch);
                    m->channel_active[ch] = false;
                }
            }
        }

        // At END of gap, commit key-on for pending notes
        // Pass current gap (sample_in_row) to check per-channel gaps
        song_commit_keyon(m, song.sample_in_row);

        // End of row
        if (song.sample_in_row >= pat.samples_per_row) {
            song.sample_in_row = 0;
            song.current_row++;
            
            // Key on notes that were waiting for next row
            // This happens AFTER the full row of release
            for (int ch = 0; ch < 6; ch++) {
                XfmSongChannel& ch_state = song.channels[ch];
                if (ch_state.pending_has_note && ch_state.wait_for_next_row) {
                    song_key_on_channel(m, ch, ch_state);
                }
            }

            // Check for pending song change with ROW timing
            if (m->pending_song.pending && m->pending_song.timing == FM_SONG_SWITCH_ROW) {
                xfm_song_play(m, m->pending_song.song_id, song.loop);
                m->pending_song.pending = false;
                continue;
            }

            // End of song
            int loopStart = std::max(0, std::min(song.loop_start_row, pat.num_rows - 1));
            int loopEnd = song.loop_end_row >= 0 ? std::min(song.loop_end_row, pat.num_rows - 1)
                                                 : pat.num_rows - 1;
            if (loopEnd < loopStart) loopEnd = loopStart;
            if (song.current_row > loopEnd || song.current_row >= pat.num_rows) {
                if (song.loop) {
                    song.current_row = loopStart;
                    song.rows_remaining = std::max(1, loopEnd - loopStart + 1);
                } else {
                    // Stop song
                    for (int ch = 0; ch < 6; ch++) {
                        m->chip->key_off(ch);
                        m->channel_active[ch] = false;
                    }
                    song.active = false;
                    song.song_id = 0;
                    m->pending_song.pending = false;
                    return;
                }
            }

            // Process next row (sets up pending notes)
            if (song.current_row < pat.num_rows) {
                song_process_row(m, song.current_row);
            }
        }
    }
}

static void song_commit_waiting_keyons(xfm_module* m)
{
    for (int ch = 0; ch < 6; ch++) {
        XfmSongChannel& ch_state = m->active_song.channels[ch];
        if (!ch_state.pending_has_note || !ch_state.wait_for_next_row) continue;
        if (ch_state.pending_patch < 0) continue;
        if (!m->patch_present[ch_state.pending_patch]) continue;

        song_key_on_channel(m, ch, ch_state);
    }
}

static bool process_song_events_now(xfm_module* m)
{
    XfmActiveSong& song = m->active_song;
    if (!song.active) return false;
    if (song.song_id <= 0 || song.song_id > 15) return false;
    if (!m->song_present[song.song_id]) return false;

    if (m->pending_song.pending && m->pending_song.timing == FM_SONG_SWITCH_NOW) {
        xfm_song_play(m, m->pending_song.song_id, song.loop);
        m->pending_song.pending = false;
        return true;
    }

    XfmSongPattern& pat = m->song_patterns[song.song_id];
    bool changed = false;

    int off_sample = (int)(pat.samples_per_row * m->auto_off_delay);
    if (off_sample < 1) off_sample = 1;
    if (song.sample_in_row == off_sample) {
        for (int ch = 0; ch < 6; ch++) {
            XfmSongChannel& ch_state = song.channels[ch];
            if (m->channel_active[ch] && ch_state.pending_has_note) {
                if (ch_state.envelope_hard_reset) m->chip->hard_mute(ch);
                else m->chip->key_off(ch);
                m->channel_active[ch] = false;
                changed = true;
            }
        }
    }

    song_commit_keyon(m, song.sample_in_row);

    while (song.active && song.sample_in_row >= pat.samples_per_row) {
        song.sample_in_row -= pat.samples_per_row;
        song.current_row++;
        changed = true;

        song_commit_waiting_keyons(m);

        if (m->pending_song.pending && m->pending_song.timing == FM_SONG_SWITCH_ROW) {
            xfm_song_play(m, m->pending_song.song_id, song.loop);
            m->pending_song.pending = false;
            return true;
        }

        int loopStart = std::max(0, std::min(song.loop_start_row, pat.num_rows - 1));
        int loopEnd = song.loop_end_row >= 0 ? std::min(song.loop_end_row, pat.num_rows - 1)
                                             : pat.num_rows - 1;
        if (loopEnd < loopStart) loopEnd = loopStart;
        if (song.current_row > loopEnd || song.current_row >= pat.num_rows) {
            if (song.loop) {
                song.current_row = loopStart;
                song.rows_remaining = std::max(1, loopEnd - loopStart + 1);
            } else {
                for (int ch = 0; ch < 6; ch++) {
                    m->chip->key_off(ch);
                    m->channel_active[ch] = false;
                }
                song.active = false;
                song.song_id = 0;
                m->pending_song.pending = false;
                return true;
            }
        }

        if (song.current_row < pat.num_rows) {
            song_process_row(m, song.current_row);
        }
    }

    return changed;
}

static int next_song_event_delta(xfm_module* m, int max_frames)
{
    XfmActiveSong& song = m->active_song;
    if (!song.active) return max_frames;
    if (song.song_id <= 0 || song.song_id > 15) return max_frames;
    if (!m->song_present[song.song_id]) return max_frames;
    if (m->pending_song.pending && m->pending_song.timing == FM_SONG_SWITCH_NOW) return 0;

    XfmSongPattern& pat = m->song_patterns[song.song_id];
    int next = std::min(max_frames, std::max(0, pat.samples_per_row - song.sample_in_row));

    int off_sample = (int)(pat.samples_per_row * m->auto_off_delay);
    if (off_sample < 1) off_sample = 1;
    if (song.sample_in_row <= off_sample) {
        for (int ch = 0; ch < 6; ch++) {
            if (m->channel_active[ch] && song.channels[ch].pending_has_note) {
                next = std::min(next, std::max(0, off_sample - song.sample_in_row));
                break;
            }
        }
    }

    for (int ch = 0; ch < 6; ch++) {
        XfmSongChannel& ch_state = song.channels[ch];
        if (ch_state.pitch_slide_speed != 0) {
            next = std::min(next, 64);
        }
        if (ch_state.volume_slide_speed != 0) {
            next = std::min(next, 64);
        }
        if (ch_state.portamento_active) {
            next = std::min(next, 64);
        }
        if (ch_state.note_slide_active) {
            next = std::min(next, 64);
        }
        if (ch_state.vibrato_depth > 0 || ch_state.tremolo_depth > 0) {
            next = std::min(next, 64);
        }
        bool has_active_macro = false;
        for (int target = 1; target < XFM_MACRO_TARGET_COUNT; target++) {
            if (ch_state.macro_states[target].active) {
                has_active_macro = true;
                break;
            }
        }
        if (has_active_macro) {
            int samples_per_tick = std::max(1, m->sample_rate / std::max(1, pat.tick_rate));
            next = std::min(next, std::max(0, samples_per_tick - ch_state.sample_in_tick));
        }
        if (ch_state.pending_has_note && !ch_state.wait_for_next_row) {
            next = std::min(next, std::max(0, ch_state.pending_gap - song.sample_in_row));
        }
    }
    return next;
}

static void advance_song_portamento(xfm_module* m, int frames)
{
    if (!m || !m->chip || !m->active_song.active || frames <= 0) return;
    if (m->active_song.song_id <= 0 || m->active_song.song_id > 15) return;
    if (!m->song_present[m->active_song.song_id]) return;

    XfmSongPattern& pat = m->song_patterns[m->active_song.song_id];

    for (int ch = 0; ch < 6; ch++) {
        XfmSongChannel& ch_state = m->active_song.channels[ch];
        if (ch_state.pitch_slide_speed != 0 && m->channel_active[ch] && ch_state.current_hz > 0.0) {
            double cents_per_second = std::abs(ch_state.pitch_slide_speed) * 25.0;
            double cents = cents_per_second * (double)frames / (double)m->sample_rate;
            if (ch_state.pitch_slide_speed < 0) cents = -cents;
            ch_state.current_hz *= std::pow(2.0, cents / 1200.0);
            ch_state.target_hz = ch_state.current_hz;
        }

        bool volume_changed = false;
        if (ch_state.volume_slide_speed != 0 && m->channel_active[ch]) {
            double delta = (double)ch_state.volume_slide_speed * (double)frames / (double)std::max(1, pat.samples_per_row);
            ch_state.current_volume_f = std::max(0.0, std::min(127.0, ch_state.current_volume_f + delta));
            int new_volume = (int)std::round(ch_state.current_volume_f);
            if (new_volume != ch_state.current_volume) {
                ch_state.current_volume = new_volume;
                volume_changed = true;
            }
        }

        if (ch_state.portamento_active && m->channel_active[ch]) {
            double next_hz = ch_state.current_hz + ch_state.portamento_step_hz * frames;
            bool reached = (ch_state.portamento_step_hz >= 0.0 && next_hz >= ch_state.target_hz) ||
                           (ch_state.portamento_step_hz < 0.0 && next_hz <= ch_state.target_hz);
            if (reached || std::abs(ch_state.portamento_step_hz) < 0.000001) {
                next_hz = ch_state.target_hz;
                ch_state.portamento_active = false;
            }

            if (next_hz > 0.0) {
                ch_state.current_hz = next_hz;
            }
        }

        if (ch_state.note_slide_active && m->channel_active[ch]) {
            double next_hz = ch_state.current_hz + ch_state.note_slide_step_hz * frames;
            bool reached = (ch_state.note_slide_step_hz >= 0.0 && next_hz >= ch_state.note_slide_target_hz) ||
                           (ch_state.note_slide_step_hz < 0.0 && next_hz <= ch_state.note_slide_target_hz);
            if (reached || std::abs(ch_state.note_slide_step_hz) < 0.000001) {
                next_hz = ch_state.note_slide_target_hz;
                ch_state.note_slide_active = false;
            }

            if (next_hz > 0.0) {
                ch_state.current_hz = next_hz;
            }
        }

        if (m->channel_active[ch] && ch_state.current_hz > 0.0) {
            if (ch_state.vibrato_depth > 0) {
                double rate_hz = 0.8 + ch_state.vibrato_speed * 0.75;
                ch_state.vibrato_phase += (2.0 * XFM_PI * rate_hz * frames) / (double)m->sample_rate;
                if (ch_state.vibrato_phase > 2.0 * XFM_PI) {
                    ch_state.vibrato_phase = std::fmod(ch_state.vibrato_phase, 2.0 * XFM_PI);
                }
            }
            song_write_channel_frequency(m, ch, ch_state);
        }

        if (m->channel_active[ch] && ch_state.tremolo_depth > 0) {
            double rate_hz = 0.8 + ch_state.tremolo_speed * 0.75;
            ch_state.tremolo_phase += (2.0 * XFM_PI * rate_hz * frames) / (double)m->sample_rate;
            if (ch_state.tremolo_phase > 2.0 * XFM_PI) {
                ch_state.tremolo_phase = std::fmod(ch_state.tremolo_phase, 2.0 * XFM_PI);
            }
            double bipolar = (std::sin(ch_state.tremolo_phase) + 1.0) * 0.5;
            int attenuation = (int)std::round(bipolar * ch_state.tremolo_depth * 4.0);
            song_write_channel_tremolo(m, ch, attenuation);
        } else if (volume_changed) {
            song_write_channel_tremolo(m, ch, 0);
        }
    }
}

static void advance_song_time(xfm_module* m, int frames)
{
    if (m->active_song.active) {
        song_advance_macros(m, frames);
        advance_song_portamento(m, frames);
        m->active_song.sample_in_row += frames;
    }
}

// =============================================================================
// NOTE TRIGGERING (polyphonic with voice stealing)
// =============================================================================

// Find a free voice, or steal the oldest one if all are busy
static int allocate_voice(xfm_module* m)
{
    // Look for a free voice first
    for (int i = 0; i < 6; i++) {
        if (!m->voices[i].active) {
            return i;
        }
    }
    
    // All voices busy - steal the oldest one (highest age)
    int oldest = 0;
    int oldest_age = m->voices[0].age;
    for (int i = 1; i < 6; i++) {
        if (m->voices[i].age > oldest_age) {
            oldest = i;
            oldest_age = m->voices[i].age;
        }
    }
    return oldest;
}

xfm_voice_id xfm_note_on(xfm_module* m, int midi_note, xfm_patch_id patch_id, int velocity)
{
    if (!m || !m->chip) return FM_VOICE_INVALID;

    // Validate patch
    if (patch_id < 0 || patch_id > 255) return FM_VOICE_INVALID;
    if (!m->patch_present[patch_id]) return FM_VOICE_INVALID;

    // Allocate a voice (with stealing if necessary)
    int voice_idx = allocate_voice(m);
    
    // If stealing an active voice, key it off first
    if (m->voices[voice_idx].active) {
        m->chip->key_off(voice_idx);
    }

    // Load patch if different from current
    if (m->current_patch[voice_idx] != patch_id) {
        m->chip->load_patch(m->patches[patch_id], voice_idx);
        m->current_patch[voice_idx] = patch_id;
        // Re-apply LFO settings after loading patch
        m->chip->enable_lfo(m->lfo_enable, static_cast<uint8_t>(m->lfo_freq));
    }

    // Calculate frequency from MIDI note (A4 = 69 = 440 Hz)
    double hz = 440.0 * std::pow(2.0, (midi_note - 69) / 12.0);

    // Set frequency and key on
    (void)velocity; // Future: use for volume
    m->chip->set_frequency(voice_idx, hz, 0);
    m->chip->key_on(voice_idx);
    
    // Update voice state
    m->voices[voice_idx].midi_note = midi_note;
    m->voices[voice_idx].patch_id = patch_id;
    m->voices[voice_idx].active = true;
    m->voices[voice_idx].age = ++m->voice_age_counter;
    m->voices[voice_idx].priority = 0;  // Piano has lowest priority
    m->voices[voice_idx].sfx_id = -1;
    m->channel_active[voice_idx] = true;

    // Return voice ID
    return voice_idx;
}

void xfm_note_off(xfm_module* m, xfm_voice_id v)
{
    if (!m || !m->chip) return;
    if (v < 0 || v > 5) return;

    // Key off the voice
    m->chip->key_off(v);
    m->voices[v].active = false;
    m->voices[v].midi_note = -1;
    m->channel_active[v] = false;
}

// =============================================================================
// AUDIO OUTPUT IMPLEMENTATION
// =============================================================================

/**
 * @brief Generate audio samples from music module (song only).
 * 
 * Optimized version that skips SFX processing.
 * Use for modules dedicated to background music.
 * 
 * @param m Module instance (should have active song)
 * @param stream Output buffer (interleaved stereo int16_t)
 * @param frames Number of stereo frames to generate
 */
void xfm_mix_song(xfm_module* m, int16_t* stream, int frames)
{
    if (!m || !m->chip || m->volume < 0.01f) {
        std::memset(stream, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    int offset = 0;
    while (offset < frames) {
        for (int guard = 0; guard < 16 && process_song_events_now(m); guard++) {}

        int chunk = next_song_event_delta(m, frames - offset);
        if (chunk <= 0) chunk = 1;
        if (chunk > frames - offset) chunk = frames - offset;

        m->chip->generate_buffer(stream + offset * 2, chunk, m->sample_rate);
        advance_song_time(m, chunk);
        offset += chunk;
    }

    for (int guard = 0; guard < 16 && process_song_events_now(m); guard++) {}

    // // Apply volume
    float vol = m->volume;
    if (vol < 1.0f) {
        for (int i = 0; i < frames * 2; i++) {
            stream[i] = static_cast<int16_t>(stream[i] * vol);
        }
    }
}

/**
 * @brief Generate audio samples from SFX module (SFX only).
 * 
 * Optimized version that skips song processing.
 * Use for modules dedicated to sound effects.
 * 
 * @param m Module instance (should have active SFX)
 * @param stream Output buffer (interleaved stereo int16_t)
 * @param frames Number of stereo frames to generate
 */
void xfm_mix_sfx(xfm_module* m, int16_t* stream, int frames)
{
    if (!m || !m->chip || m->volume < 0.01f) {
        std::memset(stream, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    int offset = 0;
    while (offset < frames) {
        for (int guard = 0; guard < 16 && process_sfx_events_now(m); guard++) {}

        int chunk = next_sfx_event_delta(m, frames - offset);
        if (chunk <= 0) chunk = 1;
        if (chunk > frames - offset) chunk = frames - offset;

        m->chip->generate_buffer(stream + offset * 2, chunk, m->sample_rate);
        advance_sfx_time(m, chunk);
        offset += chunk;
    }

    for (int guard = 0; guard < 16 && process_sfx_events_now(m); guard++) {}

    // Apply volume
    float vol = m->volume;
    if (vol < 1.0f) {
        for (int i = 0; i < frames * 2; i++) {
            stream[i] = static_cast<int16_t>(stream[i] * vol);
        }
    }
}
