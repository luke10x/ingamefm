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

    // Channel state (6 channels for OPN)
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
    // For now, only OPN is supported. OPM/OPL can be added later.
    if (chip_type != FM_CHIP_OPN) {
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

    if (!m->chip) {
        delete m;
        return nullptr;
    }

    // Initialize patch and channel state
    std::memset(m->patches, 0, sizeof(m->patches));
    std::memset(m->patch_present, 0, sizeof(m->patch_present));
    std::memset(m->current_patch, -1, sizeof(m->current_patch));
    std::memset(m->channel_active, 0, sizeof(m->channel_active));

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

    // Only OPN patches supported for now
    if (patch_type != FM_CHIP_OPN) return;
    if (m->chip_type != FM_CHIP_OPN) return;

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

fm_voice_id fm_sfx_play(fm_module* m, fm_sfx_id id, int priority)
{
    (void)m;
    (void)id;
    (void)priority;
    return FM_VOICE_INVALID;
}

// =============================================================================
// NOTE TRIGGERING (what we need for piano)
// =============================================================================

fm_voice_id fm_note_on(fm_module* m, int midi_note, fm_patch_id patch_id, int velocity)
{
    if (!m || !m->chip) return FM_VOICE_INVALID;
    if (m->mode == FM_MODE_CACHE) return FM_VOICE_INVALID;

    // Validate patch
    if (patch_id < 0 || patch_id > 255) return FM_VOICE_INVALID;
    if (!m->patch_present[patch_id]) return FM_VOICE_INVALID;

    // For now, use channel 0 as the "piano voice"
    // Future: implement proper voice allocation
    int channel = 0;

    // Load patch if different
    if (m->current_patch[channel] != patch_id) {
        m->chip->load_patch(m->patches[patch_id], channel);
        m->current_patch[channel] = patch_id;
    }

    // Calculate frequency from MIDI note (A4 = 69 = 440 Hz)
    double hz = 440.0 * std::pow(2.0, (midi_note - 69) / 12.0);

    // Set frequency and key on
    (void)velocity; // Future: use for volume
    m->chip->set_frequency(channel, hz, 0);
    m->chip->key_on(channel);
    m->channel_active[channel] = true;

    // Return channel as voice ID
    return channel;
}

void fm_note_off(fm_module* m, fm_voice_id v)
{
    if (!m || !m->chip) return;
    if (v < 0 || v > 5) return;

    m->chip->key_off(v);
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
