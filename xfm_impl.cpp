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
    std::memset(m->current_patch, -1, sizeof(m->current_patch));
    std::memset(m->channel_active, 0, sizeof(m->channel_active));

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
    m->active_song.active = false;
    m->active_song.loop = false;
    for (int ch = 0; ch < 6; ch++) {
        m->active_song.channels[ch].current_patch = -1;
        m->active_song.channels[ch].current_volume = 127;
        m->active_song.channels[ch].pending_has_note = false;
        m->active_song.channels[ch].pending_is_off = false;
        m->active_song.channels[ch].wait_for_next_row = false;
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
    }
    
    // Reset song state
    m->active_song.active = false;
    m->active_song.song_id = 0;
    m->active_song.sample_in_row = 0;
    m->active_song.current_row = 0;
    m->active_song.rows_remaining = 0;
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


// =============================================================================
// SFX PATTERN PARSER
// =============================================================================

static int parse_note_field(const char* nc) {
    if (nc[0]=='.' && nc[1]=='.' && nc[2]=='.') return -1;  // no note
    if (nc[0]=='O' && nc[1]=='F' && nc[2]=='F') return -2;  // note off
    
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
    
    int last_note = -1;
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
            if (ev.note >= 0) last_note = ev.note;
            else if (ev.note == -2) last_note = -2;  // OFF
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
        
        // Resolve inheritance
        if (ev.note == -1) ev.note = last_note;
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
    if (ev.note == -2) {
        // OFF note - key off
        m->chip->key_off(voice_idx);
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
    song.num_rows = num_rows;
    song.num_channels = num_channels;
    song.tick_rate = tick_rate;
    song.speed = speed;
    song.samples_per_row = (int)((double)m->sample_rate / tick_rate * speed);

    // Free existing rows if any
    if (song.rows) {
        for (int r = 0; r < song.num_rows; r++) {
            if (song.rows[r]) delete[] song.rows[r];
        }
        delete[] song.rows;
    }

    song.rows = new XfmSongEvent*[num_rows];
    for (int r = 0; r < num_rows; r++) {
        song.rows[r] = new XfmSongEvent[num_channels];
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
            // Skip rest of channel
            while (*p && *p != '|' && *p != '\n') p++;
            
            // Parse note - only if not "..."
            if (note_str[0] && note_str[0] != '.') {
                ev.note = parse_note_field(note_str);
                if (ev.note >= 0) last_note[ch] = ev.note;
                else if (ev.note == -2) last_note[ch] = -2;  // OFF
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
        if (ev.patch_id >= 0) ch_state.current_patch = ev.patch_id;
        if (ev.volume >= 0) ch_state.current_volume = ev.volume;

        if (ev.note == -2) {
            // OFF note - mark for key off at sample 1
            ch_state.pending_has_note = false;
            ch_state.pending_is_off = true;
            ch_state.pending_gap = get_min_gap_samples(m->sample_rate);
        } else if (ev.note >= 0) {
            // New note on this channel
            // Check if channel is still playing from previous row
            bool was_playing = m->channel_active[ch];
            
            // Key off happens at sample 1 of this row in update_song()
            // The new note will wait until end of this row (simulates manual OFF row)
            ch_state.wait_for_next_row = was_playing;

            // Look ahead to find next note on this channel
            int next_note_row = find_next_note_row(pat, row_idx, ch);

            // Calculate gap for clean release
            if (next_note_row >= 0) {
                int rows_until_next = next_note_row - row_idx;
                ch_state.pending_gap = get_dynamic_gap(m->sample_rate, rows_until_next, pat.samples_per_row);
            } else {
                ch_state.pending_gap = get_min_gap_samples(m->sample_rate);
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

        // Gap elapsed - load patch and key on new note
        xfm_patch_opn patch = m->patches[ch_state.pending_patch];
        int tl_add = ((0x7F - ch_state.current_volume) * 127) / 0x7F;
        bool isCarrier[4] = {false, false, false, false};
        switch(patch.ALG) {
            case 0: case 1: case 2: case 3: isCarrier[3] = true; break;
            case 4: isCarrier[1] = isCarrier[3] = true; break;
            case 5: case 6: isCarrier[1] = isCarrier[2] = isCarrier[3] = true; break;
            case 7: isCarrier[0] = isCarrier[1] = isCarrier[2] = isCarrier[3] = true; break;
        }
        for(int op = 0; op < 4; op++) {
            if(isCarrier[op]) {
                patch.op[op].TL = std::min(127, (int)patch.op[op].TL + tl_add);
            }
        }

        m->chip->load_patch(patch, ch);
        m->current_patch[ch] = ch_state.pending_patch;
        // Re-apply LFO settings after loading patch
        m->chip->enable_lfo(m->lfo_enable, static_cast<uint8_t>(m->lfo_freq));
        double hz = 440.0 * std::pow(2.0, (ch_state.pending_note - 69) / 12.0);
        m->chip->set_frequency(ch, hz, 0);
        m->chip->key_on(ch);
        m->channel_active[ch] = true;
        ch_state.pending_has_note = false;
        ch_state.pending_gap = get_min_gap_samples(m->sample_rate);
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
    }

    // Initialize active song - start at sample 0
    m->active_song.song_id = id;
    m->active_song.current_row = 0;
    m->active_song.sample_in_row = 0;
    m->active_song.rows_remaining = pat.num_rows;
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
    }

    // Process first row (sets up pending notes - will be triggered in update_song)
    song_process_row(m, 0);
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
                    m->chip->key_off(ch);
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
                    // Load patch and key on
                    xfm_patch_opn patch = m->patches[ch_state.pending_patch];
                    int tl_add = ((0x7F - ch_state.current_volume) * 127) / 0x7F;
                    bool isCarrier[4] = {false, false, false, false};
                    switch(patch.ALG) {
                        case 0: case 1: case 2: case 3: isCarrier[3] = true; break;
                        case 4: isCarrier[1] = isCarrier[3] = true; break;
                        case 5: case 6: isCarrier[1] = isCarrier[2] = isCarrier[3] = true; break;
                        case 7: isCarrier[0] = isCarrier[1] = isCarrier[2] = isCarrier[3] = true; break;
                    }
                    for(int op = 0; op < 4; op++) {
                        if(isCarrier[op]) {
                            patch.op[op].TL = std::min(127, (int)patch.op[op].TL + tl_add);
                        }
                    }
                    m->chip->load_patch(patch, ch);
                    m->current_patch[ch] = ch_state.pending_patch;
                    m->chip->enable_lfo(m->lfo_enable, static_cast<uint8_t>(m->lfo_freq));
                    double hz = 440.0 * std::pow(2.0, (ch_state.pending_note - 69) / 12.0);
                    m->chip->set_frequency(ch, hz, 0);
                    m->chip->key_on(ch);
                    m->channel_active[ch] = true;
                    ch_state.pending_has_note = false;
                    ch_state.wait_for_next_row = false;
                    ch_state.pending_gap = get_min_gap_samples(m->sample_rate);
                }
            }

            // Check for pending song change with ROW timing
            if (m->pending_song.pending && m->pending_song.timing == FM_SONG_SWITCH_ROW) {
                xfm_song_play(m, m->pending_song.song_id, song.loop);
                m->pending_song.pending = false;
                continue;
            }

            // End of song
            if (song.current_row >= pat.num_rows) {
                if (song.loop) {
                    song.current_row = 0;
                    song.rows_remaining = pat.num_rows;
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

    // Update song only
    update_song(m, frames);

    // Generate from chip using Bresenham for correct pitch
    m->chip->generate_buffer(stream, frames, m->sample_rate);

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

    // Update SFX only
    update_sfx(m, frames);

    // Generate from chip using Bresenham for correct pitch
    m->chip->generate_buffer(stream, frames, m->sample_rate);

    // Apply volume
    float vol = m->volume;
    if (vol < 1.0f) {
        for (int i = 0; i < frames * 2; i++) {
            stream[i] = static_cast<int16_t>(stream[i] * vol);
        }
    }
}
