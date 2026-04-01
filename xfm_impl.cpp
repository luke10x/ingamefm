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

#include "ymfm.h"
#include "ymfm_opn.h"

// =============================================================================
// YM2612 Chip Wrapper (same as old EggsFMChip, standalone)
// =============================================================================

class XfmChipOpn
{
public:
    static constexpr uint32_t YM_CLOCK = 7670453;

    // Minimal ymfm interface
    class Interface : public ymfm::ymfm_interface {
    public:
        void ymfm_set_timer(uint32_t, int32_t) override {}
        void ymfm_set_busy_end(uint32_t)       override {}
        bool ymfm_is_busy()                    override { return false; }
    };

    Interface intf;
    ymfm::ym2612 chip;

    XfmChipOpn() : chip(intf) { chip.reset(); }

    void write(uint8_t port, uint8_t reg, uint8_t val) {
        uint8_t addr = port * 2;
        chip.write(addr,   reg);
        chip.write(addr+1, val);
    }

    void load_patch(const xfm_patch_opn& p, int ch) {
        // YM2612 slot order in registers: OP1, OP3, OP2, OP4
        // Patch array order:              OP1, OP2, OP3, OP4
        const int slotMap[4] = { 0, 2, 1, 3 };

        const uint8_t port = (ch >= 3) ? 1 : 0;
        const int     hwch = ch % 3;

        for (int patchOp = 0; patchOp < 4; patchOp++) {
            int hwSlot     = slotMap[patchOp];
            const auto& op = p.op[patchOp];

            uint8_t dt_hw = static_cast<uint8_t>(((int)op.DT + 3) & 0x07);
            write(port, 0x30 + hwSlot * 4 + hwch, (dt_hw << 4) | (op.MUL & 0x0F));
            write(port, 0x40 + hwSlot * 4 + hwch, op.TL & 0x7F);
            write(port, 0x50 + hwSlot * 4 + hwch, ((op.RS & 0x03) << 6) | (op.AR & 0x1F));
            write(port, 0x60 + hwSlot * 4 + hwch, ((op.AM & 0x01) << 7) | (op.DR & 0x1F));
            write(port, 0x70 + hwSlot * 4 + hwch, op.SR & 0x1F);
            write(port, 0x80 + hwSlot * 4 + hwch, ((op.SL & 0x0F) << 4) | (op.RR & 0x0F));
            uint8_t ssg_hw = (op.SSG > 0) ? (0x08 | ((op.SSG - 1) & 0x07)) : 0;
            write(port, 0x90 + hwSlot * 4 + hwch, ssg_hw);
        }
        write(port, 0xB0 + hwch, ((p.FB & 0x07) << 3) | (p.ALG & 0x07));
        write(port, 0xB4 + hwch, 0xC0 | ((p.AMS & 0x03) << 4) | (p.FMS & 0x07));
    }

    void enable_lfo(bool enable, uint8_t freq) {
        write(0, 0x22, enable ? (0x08 | (freq & 0x07)) : 0x00);
    }

    void set_frequency(int ch, double hz, int octaveOffset = 0) {
        const uint8_t port = (ch >= 3) ? 1 : 0;
        const int     hwch = ch % 3;

        hz *= std::pow(2.0, static_cast<double>(octaveOffset));

        // FREF = chip's native sample rate at YM_CLOCK
        // YM2612: internal clock = master / 6, sample rate = internal / 24 = master / 144
        static constexpr double FREF = static_cast<double>(YM_CLOCK) / 144.0;
        int block = 4;
        double fn = hz * static_cast<double>(1 << (21 - block)) / FREF;
        while (fn > 0x7FF && block < 7) { block++; fn /= 2.0; }
        while (fn < 0x200 && block > 0) { block--; fn *= 2.0; }
        auto fnum = static_cast<uint16_t>(std::min(0x7FF, std::max(0, static_cast<int>(fn))));
        write(port, 0xA4 + hwch, ((block & 7) << 3) | ((fnum >> 8) & 0x07));
        write(port, 0xA0 + hwch, fnum & 0xFF);
    }

    void key_on(int ch) {
        write(0, 0x28, 0xF0 | ((ch >= 3) ? 0x04 : 0x00) | (ch % 3));
    }

    void key_off(int ch) {
        write(0, 0x28, ((ch >= 3) ? 0x04 : 0x00) | (ch % 3));
    }
    
    // Hard mute - instantly silence channel by maxing out TL on all operators
    // This is more aggressive than key_off which still plays release envelope
    void hard_mute(int ch) {
        const uint8_t port = (ch >= 3) ? 1 : 0;
        const int hwch = ch % 3;
        // Set TL=127 (max attenuation = silence) on all 4 operators
        for (int slot = 0; slot < 4; slot++) {
            write(port, 0x40 + slot * 4 + hwch, 0x7F);
        }
        // Also key off to reset state
        key_off(ch);
    }

    // Generate one sample at 44100 Hz reference rate
    void generate(int16_t* L, int16_t* R) {
        ymfm::ym2612::output_data out;
        chip.generate(&out, 1);
        *L = static_cast<int16_t>(std::max(-32768, std::min(32767, out.data[0])));
        *R = static_cast<int16_t>(std::max(-32768, std::min(32767, out.data[1])));
    }
    
    // Generate 'samples' stereo frames at given sample_rate using Bresenham
    // to maintain correct pitch at any output rate
    void generate_buffer(int16_t* stream, int samples, int sample_rate) {

        /*
        Clocking
        The general philosophy of the emulators provided here is that they are clock-independent. Much like the actual chips, you (the consumer) control the clock; 
        the chips themselves have no idea what time it is. They just tick forward each time you ask them to.

        But what if I want to output at a "normal" rate, like 44.1kHz? Sorry, you'll have to rate convert as needed.
        */
        static int REF_RATE = chip.sample_rate(YM_CLOCK);

        static int acc_err = 0;
        
        for (int i = 0; i < samples; i++) {
            int16_t L, R;
            do {
                generate(&L, &R);
                acc_err += sample_rate;
            } while (acc_err < REF_RATE);
            acc_err -= REF_RATE;
            stream[i * 2 + 0] = L;
            stream[i * 2 + 1] = R;
        }
    }
};

// =============================================================================
// FM MODULE INTERNAL STRUCTURE
// =============================================================================

// Voice state for polyphonic playback
struct XfmVoice {
    int     midi_note;      // current MIDI note, -1 if free
    int     patch_id;       // current patch
    bool    active;         // voice is sounding
    int     age;            // age counter for voice stealing (higher = older)
    int     priority;       // priority for SFX stealing (0 = piano, 1-9 = SFX)
    int     sfx_id;         // SFX ID if playing SFX, -1 otherwise
};

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

// SFX pattern event (one row)
struct XfmSfxEvent {
    int     note;           // MIDI note, -1 = none, -2 = off
    int     patch_id;       // patch/instrument ID, -1 = inherit
};

// SFX pattern
struct XfmSfxPattern {
    int             num_rows;
    int             tick_rate;
    int             speed;
    int             samples_per_row;
    XfmSfxEvent*     rows;     // array of num_rows events
};

// Active SFX voice tracking
struct XfmActiveSfx {
    int     sfx_id;         // which SFX is playing
    int     priority;       // priority level
    int     voice_idx;      // which voice it's using
    int     current_row;    // current row in pattern
    int     sample_in_row;  // current sample within row
    int     rows_remaining; // rows remaining (for duration tracking)
    int     last_patch_id;  // last instrument used
    int     pending_gap;    // gap samples before key-on (dynamic)
    bool    pending_has_note; // note waiting to be committed
    int     pending_note;   // pending MIDI note
    int     pending_patch_id; // pending patch
    bool    active;         // is currently playing

    // Automatic note-off scheduling
    bool    auto_off_scheduled;
    int     auto_off_at_sample;
};

// Song channel event
struct XfmSongEvent {
    int     note;           // MIDI note, -1 = none, -2 = off
    int     patch_id;       // patch/instrument ID, -1 = inherit
    int     volume;         // volume 0-127, -1 = inherit
};

// Song channel state
struct XfmSongChannel {
    int             current_patch;
    int             current_volume;
    int             pending_note;
    int             pending_patch;
    int             pending_volume;
    bool            pending_has_note;
    bool            pending_is_off;
    int             pending_gap;  // Gap samples before key-on
    
    // For automatic OFF: when a new note arrives, previous note gets
    // keyed off at sample 1, and new note waits until end of row
    bool            wait_for_next_row;  // Delay keyon until next row starts
};

// Song pattern (multi-channel)
struct XfmSongPattern {
    int             num_rows;
    int             num_channels;
    int             tick_rate;
    int             speed;
    int             samples_per_row;
    XfmSongEvent**   rows;     // array of num_rows, each is array of num_channels events
};

// Active song tracking
struct XfmActiveSong {
    int             song_id;
    int             current_row;
    int             sample_in_row;
    int             rows_remaining;  // for loop tracking
    bool            active;
    bool            loop;
    XfmSongChannel   channels[6];
};

// Pending song change
struct XfmPendingSong {
    int                 song_id;
    xfm_song_switch_timing timing;
    bool                pending;
};

struct xfm_module {
    // Configuration
    int             sample_rate;
    int             buffer_frames;
    xfm_chip_type    chip_type;
    float           volume;

    // OPN chip instance
    XfmChipOpn*      chip;

    // Patch storage (up to 256 patches)
    xfm_patch_opn    patches[256];
    bool            patch_present[256];

    // Voice pool (6 voices for OPN)
    XfmVoice         voices[6];
    int             voice_age_counter;

    // SFX patterns (up to 256 SFX definitions)
    XfmSfxPattern    sfx_patterns[256];
    bool            sfx_present[256];

    // Active SFX tracking (up to 6 concurrent SFX, one per voice)
    XfmActiveSfx     active_sfx[6];

    // Song patterns (up to 16 songs)
    XfmSongPattern   song_patterns[16];
    bool            song_present[16];

    // Active song
    XfmActiveSong    active_song;
    XfmPendingSong   pending_song;

    // Channel state tracking
    int             current_patch[6];
    bool            channel_active[6];

    // LFO state
    bool            lfo_enable;
    int             lfo_freq;

    // Automatic note off delay (0.0 = immediate, 1.0 = full row)
    float           auto_off_delay;  // Default 0.3 (30% of row)

    // Note: For future OPM/OPL support, we would:
    //   - Add union { XfmChipOpn* opn; XfmChipOpm* opm; XfmChipOpl* opl; }
    //   - Switch on chip_type in all chip operations
    //   - Add patch format validation in xfm_patch_set()
};

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
    
    if (num_rows <= 0 || num_rows > 256) return -1;
    
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
    
    if (num_rows <= 0 || num_rows > 256) return -1;
    
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
