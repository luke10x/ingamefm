// =============================================================================
// new_impl.cpp — Implementation of the new ingamefm API (C99 interface)
// =============================================================================
//
// Architecture:
//   - Client creates two fm_module instances: one for music, one for SFX
//   - Each module owns its own YM2612 chip instance
//   - Audio mixing is done by client calling fm_mix() from SDL callback
//   - Patches are chip-specific (OPN for now, OPM/OPL extensible later)
//
// =============================================================================

#include "new_api.h"
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <new>

#include "ymfm.h"
#include "ymfm_opn.h"

// =============================================================================
// YM2612 Chip Wrapper (same as old IngameFMChip, standalone)
// =============================================================================

class FmChipOpn
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

    FmChipOpn() : chip(intf) { chip.reset(); }

    void write(uint8_t port, uint8_t reg, uint8_t val) {
        uint8_t addr = port * 2;
        chip.write(addr,   reg);
        chip.write(addr+1, val);
    }

    void load_patch(const fm_patch_opn& p, int ch) {
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
        write(port, 0xB4 + hwch, 0xC0 | ((p.LFO & 0x03) << 4));
    }

    void enable_lfo(bool enable, uint8_t freq) {
        write(0, 0x22, enable ? (0x08 | (freq & 0x07)) : 0x00);
    }

    void set_frequency(int ch, double hz, int octaveOffset = 0) {
        const uint8_t port = (ch >= 3) ? 1 : 0;
        const int     hwch = ch % 3;

        hz *= std::pow(2.0, static_cast<double>(octaveOffset));

        static constexpr double FREF = 44100.0;
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

    void generate(int16_t* L, int16_t* R) {
        ymfm::ym2612::output_data out;
        chip.generate(&out, 1);
        *L = static_cast<int16_t>(std::max(-32768, std::min(32767, out.data[0])));
        *R = static_cast<int16_t>(std::max(-32768, std::min(32767, out.data[1])));
    }
};

// =============================================================================
// FM MODULE INTERNAL STRUCTURE
// =============================================================================

// Voice state for polyphonic playback
struct FmVoice {
    int     midi_note;      // current MIDI note, -1 if free
    int     patch_id;       // current patch
    bool    active;         // voice is sounding
    int     age;            // age counter for voice stealing (higher = older)
    int     priority;       // priority for SFX stealing (0 = piano, 1-9 = SFX)
    int     sfx_id;         // SFX ID if playing SFX, -1 otherwise
};

// Gap samples before key-on (lets previous note's release envelope play)
static constexpr int KEY_OFF_GAP_SAMPLES = 10;

// SFX pattern event (one row)
struct FmSfxEvent {
    int     note;           // MIDI note, -1 = none, -2 = off
    int     patch_id;       // patch/instrument ID, -1 = inherit
};

// SFX pattern
struct FmSfxPattern {
    int             num_rows;
    int             tick_rate;
    int             speed;
    int             samples_per_row;
    FmSfxEvent*     rows;     // array of num_rows events
};

// Active SFX voice tracking
struct FmActiveSfx {
    int     sfx_id;         // which SFX is playing
    int     priority;       // priority level
    int     voice_idx;      // which voice it's using
    int     current_row;    // current row in pattern
    int     sample_in_row;  // current sample within row
    int     ticks_remaining; // rows remaining (for duration tracking)
    int     last_patch_id;  // last instrument used
    bool    pending_has_note; // note waiting to be committed
    int     pending_note;   // pending MIDI note
    int     pending_patch_id; // pending patch
    bool    active;         // is currently playing
};

struct fm_module {
    // Configuration
    int             sample_rate;
    int             buffer_frames;
    fm_chip_type    chip_type;
    fm_mode         mode;
    fm_sched_mode   sched_mode;
    float           volume;

    // OPN chip instance (only for SYNTH and RECORD modes)
    FmChipOpn*      chip;

    // Patch storage (up to 256 patches)
    fm_patch_opn    patches[256];
    bool            patch_present[256];

    // Voice pool (6 voices for OPN)
    FmVoice         voices[6];
    int             voice_age_counter;

    // SFX patterns (up to 256 SFX definitions)
    FmSfxPattern    sfx_patterns[256];
    bool            sfx_present[256];

    // Active SFX tracking (up to 6 concurrent SFX, one per voice)
    FmActiveSfx     active_sfx[6];

    // Channel state tracking
    int             current_patch[6];
    bool            channel_active[6];

    // LFO state
    bool            lfo_enable;
    int             lfo_freq;

    // Note: For future OPM/OPL support, we would:
    //   - Add union { FmChipOpn* opn; FmChipOpm* opm; FmChipOpl* opl; }
    //   - Switch on chip_type in all chip operations
    //   - Add patch format validation in fm_patch_set()
};

// =============================================================================
// MODULE LIFETIME
// =============================================================================

fm_module* fm_module_create(int sample_rate, int buffer_frames, fm_chip_type chip_type)
{
    // For now, only YM2612 and YM3438 are supported
    if (chip_type != FM_CHIP_YM2612 && chip_type != FM_CHIP_YM3438) {
        // Future: allocate appropriate chip type
        // case FM_CHIP_OPM: chip = new FmChipOpm(); break;
        // case FM_CHIP_OPL2: chip = new FmChipOpl2(); break;
        return nullptr;
    }

    fm_module* m = new (std::nothrow) fm_module;
    if (!m) return nullptr;

    m->sample_rate   = sample_rate;
    m->buffer_frames = buffer_frames;
    m->chip_type     = chip_type;
    m->mode          = FM_MODE_SYNTH;
    m->sched_mode    = FM_SCHED_SONG;
    m->volume        = 1.0f;
    m->chip          = new (std::nothrow) FmChipOpn;
    m->lfo_enable    = false;
    m->lfo_freq      = 0;
    m->voice_age_counter = 0;

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
        m->active_sfx[i].sample_in_row = KEY_OFF_GAP_SAMPLES;
        m->active_sfx[i].ticks_remaining = 0;
        m->active_sfx[i].last_patch_id = -1;
        m->active_sfx[i].pending_has_note = false;
        m->active_sfx[i].pending_note = -1;
        m->active_sfx[i].pending_patch_id = -1;
        m->active_sfx[i].active = false;
    }

    return m;
}

void fm_module_destroy(fm_module* m)
{
    if (!m) return;
    delete m->chip;
    delete m;
}

bool fm_module_set_mode(fm_module* m, fm_mode mode)
{
    if (!m) return false;

    // RULE: FM_MODE_CACHE is allowed ONLY if all declared assets are cached.
    // For now, we allow it but client must ensure patches are loaded.
    if (mode == FM_MODE_CACHE) {
        // In cache mode, chip is not needed
        // Future: verify all songs/SFX are cached before allowing
    }

    m->mode = mode;
    return true;
}

void fm_module_set_scheduler(fm_module* m, fm_sched_mode mode)
{
    if (!m) return;
    m->sched_mode = mode;
}

void fm_module_set_volume(fm_module* m, float volume)
{
    if (!m) return;
    m->volume = std::max(0.0f, std::min(1.0f, volume));
}

void fm_module_set_clock(fm_module* m, int clock_hz)
{
    // For OPN, clock is fixed at 7670453 Hz
    // This could be used for future chips with different clocks
    (void)m;
    (void)clock_hz;
}

void fm_module_set_lfo(fm_module* m, bool enable, int freq)
{
    if (!m || !m->chip) return;
    m->lfo_enable = enable;
    m->lfo_freq = freq & 7;
    m->chip->enable_lfo(enable, static_cast<uint8_t>(m->lfo_freq));
}

// =============================================================================
// PATCH SYSTEM
// =============================================================================

void fm_patch_set(fm_module* m, fm_patch_id patch_id, const void* patch_data,
                  int patch_size, fm_chip_type patch_type)
{
    if (!m || !patch_data || patch_id < 0 || patch_id > 255) return;

    // Only YM2612/YM3438 patches supported for now
    if (patch_type != FM_CHIP_YM2612 && patch_type != FM_CHIP_YM3438) return;
    if (m->chip_type != FM_CHIP_YM2612 && m->chip_type != FM_CHIP_YM3438) return;

    // Validate size
    if (patch_size != sizeof(fm_patch_opn)) return;

    m->patches[patch_id] = *static_cast<const fm_patch_opn*>(patch_data);
    m->patch_present[patch_id] = true;
}

void fm_patch_set_song_channel(fm_module* m, fm_song_id song, int channel,
                               fm_patch_id patch_id)
{
    // For now, we just assign patch to channel
    // Future: this will be used by song scheduler
    if (!m || channel < 0 || channel > 5) return;
    if (patch_id >= 0 && patch_id <= 255 && m->patch_present[patch_id]) {
        m->current_patch[channel] = patch_id;
        m->chip->load_patch(m->patches[patch_id], channel);
    }
}

void fm_patch_set_sfx(fm_module* m, fm_sfx_id sfx, fm_patch_id patch_id)
{
    // SFX patches are assigned when sfx_play is called
    // This is a placeholder for future SFX system
    (void)m;
    (void)sfx;
    (void)patch_id;
}

// =============================================================================
// SONG DECLARATION (stub - future implementation)
// =============================================================================

fm_song_id fm_song_declare_source(fm_module* m, const char* pattern, int ticks, int speed)
{
    // Future: parse Furnace pattern and store
    (void)m;
    (void)pattern;
    (void)ticks;
    (void)speed;
    return 0;
}

fm_song_id fm_song_declare_cache(fm_module* m, void* buffer, int byte_capacity,
                                  int original_ticks, int original_speed, int original_steps)
{
    // Future: associate cache buffer with song
    (void)m;
    (void)buffer;
    (void)byte_capacity;
    (void)original_ticks;
    (void)original_speed;
    (void)original_steps;
    return 0;
}

void fm_bind_cache_target(fm_module* m, fm_song_id synth_song_id, fm_song_id cache_song_id)
{
    (void)m;
    (void)synth_song_id;
    (void)cache_song_id;
}

int fm_song_cached_progress(fm_module* m, fm_song_id cache_song_id)
{
    (void)m;
    (void)cache_song_id;
    return 0;
}

bool fm_song_is_cached(fm_module* m, fm_song_id cache_song_id)
{
    (void)m;
    (void)cache_song_id;
    return false;
}

void fm_song_play(fm_module* m, fm_song_id id)
{
    (void)m;
    (void)id;
}

void fm_song_schedule(fm_module* m, fm_song_id id, fm_song_switch when)
{
    (void)m;
    (void)id;
    (void)when;
}

void fm_song_schedule_silence(fm_module* m, fm_song_switch when)
{
    (void)m;
    (void)when;
}

int fm_song_get_row(fm_module* m)
{
    (void)m;
    return 0;
}

// =============================================================================
// SFX DECLARATION (stub - future implementation)
// =============================================================================

fm_sfx_id fm_sfx_declare_source(fm_module* m, const char* pattern, int ticks, int speed)
{
    (void)m;
    (void)pattern;
    (void)ticks;
    (void)speed;
    return 0;
}

fm_sfx_id fm_sfx_declare_cache(fm_module* m, void* buffer, int byte_capacity,
                                int original_ticks, int original_speed, int original_steps)
{
    (void)m;
    (void)buffer;
    (void)byte_capacity;
    (void)original_ticks;
    (void)original_speed;
    (void)original_steps;
    return 0;
}

void fm_attach_sfx_cache(fm_module* m, fm_sfx_id synth_sfx_id, fm_sfx_id cache_sfx_id)
{
    (void)m;
    (void)synth_sfx_id;
    (void)cache_sfx_id;
}

int fm_sfx_cached_progress(fm_module* m, fm_sfx_id cache_sfx_id)
{
    (void)m;
    (void)cache_sfx_id;
    return 0;
}

bool fm_sfx_is_cached(fm_module* m, fm_sfx_id cache_sfx_id)
{
    (void)m;
    (void)cache_sfx_id;
    return false;
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

fm_sfx_id fm_sfx_declare(fm_module* m, fm_sfx_id id, const char* pattern_text, int tick_rate, int speed)
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
    FmSfxPattern& pat = m->sfx_patterns[id];
    pat.num_rows = num_rows;
    pat.tick_rate = tick_rate;
    pat.speed = speed;
    pat.samples_per_row = (int)((double)m->sample_rate / tick_rate * speed);
    
    // Free existing rows if any
    if (pat.rows) {
        delete[] pat.rows;
    }
    pat.rows = new FmSfxEvent[num_rows];
    
    int last_note = -1;
    int last_inst = -1;
    
    for (int row = 0; row < num_rows; row++) {
        FmSfxEvent& ev = pat.rows[row];
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

// Process a row for an SFX voice (sets up pending note)
static void sfx_process_row(fm_module* m, int voice_idx, int row_idx)
{
    FmActiveSfx& sfx = m->active_sfx[0];  // Find the right slot
    for (int slot = 0; slot < 6; slot++) {
        if (m->active_sfx[slot].voice_idx == voice_idx) {
            sfx = m->active_sfx[slot];
            break;
        }
    }
    
    FmSfxPattern& pat = m->sfx_patterns[sfx.sfx_id];
    if (row_idx >= pat.num_rows) return;
    
    FmSfxEvent& ev = pat.rows[row_idx];
    
    // Update last known patch
    if (ev.patch_id >= 0) {
        sfx.last_patch_id = ev.patch_id;
    }
    
    // Handle note
    sfx.pending_has_note = false;
    if (ev.note == -2) {
        // OFF note - key off
        m->chip->key_off(voice_idx);
    } else if (ev.note >= 0) {
        // New note - prepare for key-on after gap
        m->chip->key_off(voice_idx);
        sfx.pending_has_note = true;
        sfx.pending_note = ev.note;
        sfx.pending_patch_id = (ev.patch_id >= 0) ? ev.patch_id : sfx.last_patch_id;
    }
}

// Commit pending note (after gap samples)
static void sfx_commit_keyon(fm_module* m, int voice_idx)
{
    // Find the active SFX for this voice
    for (int slot = 0; slot < 6; slot++) {
        FmActiveSfx& sfx = m->active_sfx[slot];
        if (!sfx.active || sfx.voice_idx != voice_idx) continue;
        
        if (!sfx.pending_has_note) return;
        if (sfx.pending_patch_id < 0) return;
        if (!m->patch_present[sfx.pending_patch_id]) return;
        
        // Load patch
        m->chip->load_patch(m->patches[sfx.pending_patch_id], voice_idx);
        m->current_patch[voice_idx] = sfx.pending_patch_id;
        
        // Set frequency and key on
        double hz = 440.0 * std::pow(2.0, (sfx.pending_note - 69) / 12.0);
        m->chip->set_frequency(voice_idx, hz, 0);
        m->chip->key_on(voice_idx);
        m->voices[voice_idx].midi_note = sfx.pending_note;
        m->channel_active[voice_idx] = true;
        
        // Clear pending
        sfx.pending_has_note = false;
        return;
    }
}

fm_voice_id fm_sfx_play(fm_module* m, fm_sfx_id id, int priority)
{
    if (!m || !m->chip) return FM_VOICE_INVALID;
    if (m->mode == FM_MODE_CACHE) return FM_VOICE_INVALID;
    if (id < 0 || id > 255) return FM_VOICE_INVALID;
    if (!m->sfx_present[id]) return FM_VOICE_INVALID;
    
    FmSfxPattern& pat = m->sfx_patterns[id];
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
    
    for (int i = 0; i < 6; i++) {
        if (!m->voices[i].active) {
            // Free voice - take it immediately
            best_voice = i;
            best_slot = -1;
            break;
        }
        
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
    FmActiveSfx& sfx = m->active_sfx[slot];
    sfx.sfx_id = id;
    sfx.priority = priority;
    sfx.voice_idx = best_voice;
    sfx.current_row = 0;
    sfx.sample_in_row = KEY_OFF_GAP_SAMPLES;  // Start after gap
    sfx.ticks_remaining = pat.num_rows;
    sfx.last_patch_id = -1;
    sfx.pending_has_note = false;
    sfx.pending_note = -1;
    sfx.pending_patch_id = -1;
    sfx.active = true;
    
    // Update voice state
    m->voices[best_voice].active = true;
    m->voices[best_voice].priority = priority;
    m->voices[best_voice].sfx_id = id;
    m->voices[best_voice].age = ++m->voice_age_counter;
    m->voices[best_voice].midi_note = -1;
    m->voices[best_voice].patch_id = -1;
    m->channel_active[best_voice] = false;
    
    // Process first row immediately (sets up pending note)
    sfx_process_row(m, best_voice, 0);
    
    // Commit key-on immediately (we're already past the gap)
    sfx_commit_keyon(m, best_voice);
    
    return best_voice;
}

// Internal: Update active SFX voices (called from fm_mix each frame)
// Advances SFX by 1 sample. Call this in a loop for each sample in the buffer.
static void update_sfx_voice(fm_module* m, int slot)
{
    FmActiveSfx& sfx = m->active_sfx[slot];
    if (!sfx.active) return;
    if (sfx.ticks_remaining <= 0) return;
    if (sfx.voice_idx < 0 || sfx.voice_idx >= 6) return;
    
    FmSfxPattern& pat = m->sfx_patterns[sfx.sfx_id];
    int voice = sfx.voice_idx;
    
    // Advance sample counter
    sfx.sample_in_row++;
    
    // Check if we've crossed the gap boundary - commit key-on
    if (sfx.sample_in_row == KEY_OFF_GAP_SAMPLES && sfx.pending_has_note) {
        sfx_commit_keyon(m, voice);
    }
    
    // Check if we've reached end of row
    if (sfx.sample_in_row >= pat.samples_per_row) {
        sfx.sample_in_row = 0;
        sfx.ticks_remaining--;
        sfx.current_row++;
        
        // SFX finished?
        if (sfx.ticks_remaining <= 0) {
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
static void update_sfx(fm_module* m, int num_samples)
{
    for (int i = 0; i < num_samples; i++) {
        for (int slot = 0; slot < 6; slot++) {
            update_sfx_voice(m, slot);
        }
    }
}

// =============================================================================
// NOTE TRIGGERING (polyphonic with voice stealing)
// =============================================================================

// Find a free voice, or steal the oldest one if all are busy
static int allocate_voice(fm_module* m)
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

fm_voice_id fm_note_on(fm_module* m, int midi_note, fm_patch_id patch_id, int velocity)
{
    if (!m || !m->chip) return FM_VOICE_INVALID;
    if (m->mode == FM_MODE_CACHE) return FM_VOICE_INVALID;

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

void fm_note_off(fm_module* m, fm_voice_id v)
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
// AUDIO OUTPUT
// =============================================================================

void fm_mix(fm_module* m, int16_t* stream, int frames)
{
    if (!m) return;

    // In CACHE mode, we would mix from cache buffer
    // For now, only SYNTH mode is implemented
    if (m->mode == FM_MODE_CACHE) {
        // Future: mix from cache buffer
        std::memset(stream, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    // In SYNTH mode, chip must exist
    if (!m->chip) {
        std::memset(stream, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    // Safety: Clean up any voices that are active but have no corresponding SFX
    for (int i = 0; i < 6; i++) {
        if (m->voices[i].active && m->voices[i].sfx_id >= 0) {
            // Check if there's a matching active_sfx entry
            bool found = false;
            for (int slot = 0; slot < 6; slot++) {
                if (m->active_sfx[slot].active && m->active_sfx[slot].voice_idx == i) {
                    found = true;
                    break;
                }
            }
            // If no matching SFX, this voice is stuck - release it
            if (!found) {
                m->chip->key_off(i);
                m->voices[i].active = false;
                m->voices[i].priority = 0;
                m->voices[i].sfx_id = -1;
                m->channel_active[i] = false;
            }
        }
    }

    // Update active SFX for each sample in the buffer
    update_sfx(m, frames);

    // SYNTH mode: generate from chip
    // Note: chip generates at 44100 Hz internally
    // We need to handle sample rate conversion if needed
    for (int i = 0; i < frames; i++) {
        int16_t L, R;
        m->chip->generate(&L, &R);

        // Apply volume
        float vol = m->volume;
        L = static_cast<int16_t>(L * vol);
        R = static_cast<int16_t>(R * vol);

        // Write to interleaved stereo buffer
        stream[i * 2 + 0] = L;
        stream[i * 2 + 1] = R;
    }
}
