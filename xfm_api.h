
/**
 * =============================================================================
 * xfm_api.h — eggsfm — Modular FM Synthesis Library (C99)
 * =============================================================================
 *
 * A modular FM audio system for games and demos.
 * Each module represents ONE FM chip instance.
 *
 * Supported chip families (extensible):
 *   • OPN  (YM2612 / YM3438)
 *   • OPM  (YM2151)
 *   • OPQ  (YM??? extended variants)
 *   • OPL2 / OPL3
 *
 * -----------------------------------------------------------------------------
 * 🧠 CORE MODEL
 * -----------------------------------------------------------------------------
 *
 * • Each module has:
 *     - One chip type
 *
 * • Assets:
 *     - Furnace patterns (parsed and played in real-time)
 *
 * -----------------------------------------------------------------------------
 */

#ifndef XFM_H
#define XFM_H

#include <stdint.h>
#include <stdbool.h>

#include "ymfm.h"
#include "ymfm_opn.h"

#ifdef __cplusplus
extern "C" {
#endif
/**
 * =============================================================================
 * FM PATCH DATA STRUCTURES
 * =============================================================================
 *
 * These are low-level register-style patch definitions.
 * They are NOT runtime voice objects.
 *
 * Each patch maps closely to real chip parameters.
 * The engine simply forwards them to the correct backend.
 *
 * -----------------------------------------------------------------------------
 * IMPORTANT DESIGN RULE
 * -----------------------------------------------------------------------------
 *
 * • These structs are CHIP-SPECIFIC
 * • They are NOT interchangeable
 * • Engine does NOT reinterpret between formats
 *
 * You MUST pass correct xfm_chip_type when assigning patches.
 */

/* =============================================================================
 * OPN (YM2612 / YM3438 style)
 * ============================================================================= */

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

/**
 * Song switch timing options.
 */
typedef enum {
    FM_SONG_SWITCH_NOW = 0,     /* Switch immediately */
    FM_SONG_SWITCH_ROW,         /* Switch at next row */
    FM_SONG_SWITCH_LOOP          /* Switch at next loop point */
} xfm_song_switch_timing;


// Pending song change
struct XfmPendingSong {
    int                 song_id;
    xfm_song_switch_timing timing;
    bool                pending;
};

// =============================================================================
// YM2612 Chip Wrapper (same as old EggsFMChip, standalone)
// =============================================================================

typedef struct xfm_patch_opn_operator
{
    int8_t  DT;     /* detune (-3..+3) */
    uint8_t MUL;    /* frequency multiplier */
    uint8_t TL;     /* total level */
    uint8_t RS;     /* rate scale */
    uint8_t AR;     /* attack rate */
    uint8_t AM;     /* amplitude modulation enable */
    uint8_t DR;     /* decay rate */
    uint8_t SR;     /* sustain rate */
    uint8_t SL;     /* sustain level */
    uint8_t RR;     /* release rate */
    uint8_t SSG;    /* SSG-EG (OPN specific) */
} xfm_patch_opn_operator;

typedef struct xfm_patch_opn
{
    uint8_t ALG;     /* algorithm */
    uint8_t FB;      /* feedback */
    uint8_t AMS;     /* AM sensitivity (0-3) */
    uint8_t FMS;     /* FM sensitivity (0-7) */

    xfm_patch_opn_operator op[4]; /* 4 operators */
} xfm_patch_opn;

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
    ymfm::ym3438 chip;
    int acc_err;  // Bresenham resampling accumulator

    XfmChipOpn() : chip(intf), acc_err(0) { chip.reset(); }

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

    void reset_resample_accum() {
        acc_err = 0;
    }

    void reset_chip() {
        chip.reset();
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
        ymfm::ym3438::output_data out;
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
        
        for (int i = 0; i < samples; i++) {
            int16_t L, R;
            do {
                generate(&L, &R);
                this->acc_err += sample_rate;
            } while (this->acc_err < REF_RATE);
            this->acc_err -= REF_RATE;
            stream[i * 2 + 0] = L;
            stream[i * 2 + 1] = R;
        }
    }
};

/* =============================================================================
 * CHIP TYPES
 * ============================================================================= */

typedef enum {
    XFM_CHIP_YM2612 = 0,  /* OPN2 — original, authentic Sega sound */
    XFM_CHIP_YM3438,      /* OPN2C — CMOS, cleaner */
    XFM_CHIP_OPM,         /* YM2151 */
    XFM_CHIP_OPQ,         /* extended */
    XFM_CHIP_OPL2,
    XFM_CHIP_OPL3
} xfm_chip_type;

struct xfm_module {
    // Configuration
    int             sample_rate;
    int             buffer_frames;
    xfm_chip_type    chip_type;
    float           volume;

    // OPN chip instance (now uses YM3438 variant for accurate DAC behavior)
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


/* =============================================================================
 * OPM (YM2151 style)
 * ============================================================================= */

typedef struct xfm_patch_opm_operator
{
    uint8_t dt1;    /* coarse detune */
    uint8_t dt2;    /* fine detune */
    uint8_t mul;

    uint8_t tl;

    uint8_t ks;     /* key scale */
    uint8_t ar;
    uint8_t dr;
    uint8_t sr;
    uint8_t rr;
    uint8_t sl;

    uint8_t ssg;    /* SSG EG (OPM variant) */
} xfm_patch_opm_operator;

typedef struct xfm_patch_opm
{
    uint8_t alg;
    uint8_t fb;

    uint8_t pan;     /* stereo placement */
    uint8_t lfo_freq;
    uint8_t lfo_wave;

    xfm_patch_opm_operator op[4];
} xfm_patch_opm;

/* =============================================================================
 * OPL2 / OPL3 (YM3812 / YMF262 style)
 * ============================================================================= */

typedef struct xfm_patch_opl_operator
{
    uint8_t am;     /* amplitude modulation */
    uint8_t vib;    /* vibrato */
    uint8_t eg;     /* envelope generator type */
    uint8_t ksr;    /* key scaling rate */
    uint8_t mul;
    uint8_t wave;   /* waveform select (0-7) */

    uint8_t ksl;    /* key scale level */
    uint8_t tl;

    uint8_t ar;
    uint8_t dr;
    uint8_t sl;
    uint8_t rr;
} xfm_patch_opl_operator;

typedef struct xfm_patch_opl
{
    uint8_t alg;   /* connection type */
    uint8_t fb;
    bool is4op;    /* 4-operator mode (OPL3) */

    uint8_t waveform; /* OPL2/OPL3 waveform select (global for 2-op, per-op for 4-op) */

    xfm_patch_opl_operator op[4]; /* OPL2 = 2 ops, OPL3 4-op = 4 ops */
} xfm_patch_opl;

/* =============================================================================
 * TYPES
 * ============================================================================= */

typedef struct xfm_module xfm_module;

typedef int xfm_song_id;
typedef int xfm_sfx_id;
typedef int xfm_voice_id;
typedef int xfm_patch_id;

#define FM_VOICE_INVALID (-1)

/* =============================================================================
 * MODULE LIFETIME
 * ============================================================================= */

/**
 * Create module.
 *
 * @param sample_rate   output sample rate
 * @param buffer_frames audio buffer size
 * @param chip_type     FM chip family
 */
xfm_module* xfm_module_create(
    int sample_rate,
    int buffer_frames,
    xfm_chip_type chip_type
);

/**
 * Destroy module.
 */
void xfm_module_destroy(xfm_module* m);

/**
 * Set master volume (0.0 – 1.0).
 */
void xfm_module_set_volume(xfm_module* m, float volume);

/**
 * Enable / configure LFO.
 *
 * Interpretation of freq depends on chip type.
 */
void xfm_module_set_lfo(xfm_module* m, bool enable, int freq);

/**
 * Reload all patches and reset voice patch tracking.
 * Call after editing patches to force reload on next note.
 */
void xfm_module_reload_patches(xfm_module* m);

/**
 * @brief Reset module state for clean export
 *
 * Call this before exporting SFX to ensure no state leakage
 * from previous exports. Resets voices, SFX state, song state,
 * and the Bresenham resampling accumulator.
 *
 * @param m Module instance
 */
void xfm_module_reset_state(xfm_module* m);

/* =============================================================================
 * PATCH SYSTEM
 * ============================================================================= */

/**
 * Generic patch setter.
 *
 * The data pointer must match the chip type:
 *
 *   XFM_CHIP_YM2612/YM3438 → YM2612-style struct
 *   XFM_CHIP_OPM  → YM2151-style struct
 *   XFM_CHIP_OPL2 → OPL2 struct
 *   etc.
 *
 * The engine does NOT reinterpret formats.
 */
void xfm_patch_set(
    xfm_module* m,
    xfm_patch_id patch_id,
    const void* patch_data,
    int patch_size,
    xfm_chip_type patch_type
);

/* =============================================================================
 * SONG DECLARATION
 * ============================================================================= */

/**
 * Declare a song pattern.
 * 
 * Pattern format is multi-channel Furnace format:
 *   First line: number of rows
 *   Each row: channel1|channel2|...|channelN
 *   Each channel: note(3) + instrument(2) + volume(2) [+ effects]
 * 
 * Example (2 channels):
 *   "8\n"
 *   "C-4007F....|E-4017F....\n"
 *   "...........|...........\n"
 *   ...
 *
 * @param m module
 * @param id Song ID (1-15) to assign
 * @param pattern_text Furnace pattern text
 * @param tick_rate ticks per second (e.g. 60)
 * @param speed ticks per row (e.g. 6 = 100ms/row at 60Hz)
 * @return Song ID or -1 on error
 */
xfm_song_id xfm_song_declare(
    xfm_module* m,
    xfm_song_id id,
    const char* pattern_text,
    int tick_rate,
    int speed
);

/* =============================================================================
 * SONG CONTROL
 * ============================================================================= */

/**
 * Start playing a song.
 *
 * @param m module
 * @param id Song ID from xfm_song_declare
 * @param loop true = loop indefinitely, false = play once
 */
void xfm_song_play(xfm_module* m, xfm_song_id id, bool loop);

/**
 * Schedule a song change.
 *
 * @param m module
 * @param id Song ID to switch to
 * @param timing When to switch (NOW, STEP, or LOOP)
 */
void xfm_song_schedule(xfm_module* m, xfm_song_id id, xfm_song_switch_timing timing);

/**
 * Get current song row.
 *
 * @param m module
 * @return Current row index (0-based)
 */
int xfm_song_get_row(xfm_module* m);

/**
 * Get total rows in a song.
 *
 * @param m module
 * @param id Song ID
 * @return Total number of rows
 */
int xfm_song_get_total_rows(xfm_module* m, xfm_song_id id);

/* =============================================================================
 * SFX DECLARATION
 * ============================================================================= */

/**
 * Declare an SFX pattern.
 * 
 * Pattern format is single-channel Furnace format:
 *   First line: number of rows
 *   Each row: note(3) + instrument(2) + volume(2)
 * 
 * Example:
 *   "6\n"
 *   "C-4007F\n"
 *   "E-4007F\n"
 *   "G-4007F\n"
 *   "C-5007F\n"
 *   "OFF....\n"
 *   ".......\n"
 *
 * @param m module
 * @param id SFX ID (0-255) to assign
 * @param pattern_text Furnace pattern text
 * @param tick_rate ticks per second (e.g. 60)
 * @param speed ticks per row (e.g. 3 = 50ms/row)
 * @return SFX ID or -1 on error
 */
xfm_sfx_id xfm_sfx_declare(
    xfm_module* m,
    xfm_sfx_id id,
    const char* pattern_text,
    int tick_rate,
    int speed
);

/* =============================================================================
 * SFX CONTROL
 * ============================================================================= */

/**
 * Play SFX.
 *
 * Uses voice stealing:
 *   - Prefers free voices
 *   - Steals lowest priority voice if all busy
 *   - If equal priority, steals oldest
 *
 * Priority scale:
 *   0 = piano/manual notes (lowest)
 *   1-2 = ambient SFX
 *   3-4 = common gameplay
 *   5-6 = significant events
 *   7-9 = critical/must-hear
 *
 * @param m module
 * @param id SFX ID from xfm_sfx_declare
 * @param priority 0-9 (higher = harder to steal)
 * @return voice ID or FM_VOICE_INVALID
 */
xfm_voice_id xfm_sfx_play(
    xfm_module* m,
    xfm_sfx_id id,
    int priority
);

/**
 * Trigger note.
 */
xfm_voice_id xfm_note_on(
    xfm_module* m,
    int midi_note,
    xfm_patch_id patch_id,
    int velocity
);

/**
 * Release note.
 */
void xfm_note_off(xfm_module* m, xfm_voice_id v);

/* =============================================================================
 * AUDIO OUTPUT
 * ============================================================================= */

/**
 * @brief Mix audio from module into output buffer.
 * 
 * This is the main audio generation function. Call this from your audio callback
 * to generate audio samples from the module.
 * 
 * ─────────────────────────────────────────────────────────────────────────────
 * 📊 CALL RATE & TIMING
 * ─────────────────────────────────────────────────────────────────────────────
 * 
 * Call Frequency: From SDL audio callback (~172 Hz for 256-frame buffer @ 44100Hz)
 * 
 * Processing Flow:
 *   1. update_song(m, frames)  - Advances song by 'frames' samples
 *   2. update_sfx(m, frames)   - Advances all active SFX by 'frames' samples
 *   3. chip->generate_buffer() - Generates FM synthesis output
 * 
 * ─────────────────────────────────────────────────────────────────────────────
 * 📦 PARAMETERS
 * ─────────────────────────────────────────────────────────────────────────────
 * 
 * @param m
 *   - Pointer to xfm_module
 *   - Contains: patches, active song, active SFX, voice pool, chip instance
 *   - Thread: Must be thread-safe if called from audio thread
 * 
 * @param stream
 *   - Output buffer for audio samples
 *   - Format: Interleaved stereo int16_t [-32768, +32767]
 *   - Layout: [L0, R0, L1, R1, L2, R2, ...]
 *   - Size: Must hold at least (frames × 2) int16_t values
 *   - Ownership: Caller allocates, callee fills
 * 
 * @param frames
 *   - Number of stereo frames to generate
 *   - 1 frame = 1 left sample + 1 right sample = 4 bytes
 *   - Typical values: 64, 128, 256, 512 (power of 2 for efficiency)
 *   - Calculation: frames = buffer_bytes / 4
 * 
 * ─────────────────────────────────────────────────────────────────────────────
 * 🔄 INTERNAL PROCESSING
 * ─────────────────────────────────────────────────────────────────────────────
 * 
 * 1. SONG PROCESSING (update_song)
 *    - Advances song position by 'frames' samples
 *    - Checks for new notes at row boundaries
 *    - Triggers key_on() for new notes
 *    - Triggers key_off() for released notes
 *    - Updates per-channel volume (if using volume scaling)
 * 
 * 2. SFX PROCESSING (update_sfx)
 *    - Advances each active SFX by 'frames' samples
 *    - Processes row-by-row with dynamic gap timing
 *    - Triggers key_on() at appropriate sample
 *    - Triggers key_off() at end of SFX or on OFF rows
 *    - Manages voice stealing based on priority
 * 
 * 3. FM SYNTHESIS (chip->generate_buffer)
 *    - Reads note/frequency data from active voices
 *    - Applies patch parameters (ALG, FB, AMS, FMS, envelopes, etc.)
 *    - Generates stereo output via YMFM emulator
 *    - Writes to stream buffer
 * 
 * ─────────────────────────────────────────────────────────────────────────────
 * ⏱️  TIMING & LATENCY
 * ─────────────────────────────────────────────────────────────────────────────
 * 
 * Sample Rate: Typically 44100 Hz (defined in xfm_module_create)
 * Frame Time:  frames / sample_rate seconds
 *   - 256 frames @ 44100 Hz = 5.8 ms
 *   - 512 frames @ 44100 Hz = 11.6 ms
 * 
 * Note Trigger Timing:
 *   - Song: Notes trigger at row boundaries (speed × tick_rate)
 *   - SFX: Notes trigger after dynamic gap (distance-based)
 *   - Gap formula: gap_samples = sample_rate / (tick_rate × speed)
 * 
 * ─────────────────────────────────────────────────────────────────────────────
 * ⚠️  THREAD SAFETY
 * ─────────────────────────────────────────────────────────────────────────────
 * 
 * This function is typically called from the SDL audio thread.
 * 
 * Safe operations:
 *   - Reading patch data (patches are immutable after loading)
 *   - Reading song/SFX patterns (immutable after declaration)
 *   - Voice state updates (internal locking)
 * 
 * Unsafe without locking:
 *   - Modifying patches while playing
 *   - Declaring new songs/SFX during playback
 *   - Changing module configuration
 * 
 * Use SDL_LockAudioDevice() if modifying state from main thread.
 * 
 * ─────────────────────────────────────────────────────────────────────────────
 * 📈 PERFORMANCE
 * ─────────────────────────────────────────────────────────────────────────────
 * 
 * Typical CPU usage per call (256 frames, 6 voices active):
 *   - update_song:    ~50-100 μs
 *   - update_sfx:     ~50-100 μs  
 *   - generate_buffer: ~200-400 μs (YMFM synthesis)
 *   - Total:          ~300-600 μs per callback
 *   - Budget:         5800 μs (5.8 ms)
 *   - Headroom:       ~90% CPU idle time
 * 
 * Optimization tips:
 *   - Use xfm_mix_song() and xfm_mix_sfx() for dedicated modules
 *   - Keep voice count low (6 voices per module is typical)
 *   - Avoid allocations in audio path
 */
void xfm_mix(xfm_module* m, int16_t* stream, int frames);

/**
 * @brief Mix audio from music module (song only, no SFX).
 * 
 * Optimized version for modules that only play songs.
 * Skips SFX processing for ~50-100 μs savings per callback.
 * 
 * @param m Module (should have song declared, no SFX needed)
 * @param stream Output buffer (interleaved stereo int16_t)
 * @param frames Number of stereo frames to generate
 * 
 * @see xfm_mix() for full documentation
 */
void xfm_mix_song(xfm_module* m, int16_t* stream, int frames);

/**
 * @brief Mix audio from SFX module (SFX only, no song).
 * 
 * Optimized version for modules that only play SFX.
 * Skips song processing for ~50-100 μs savings per callback.
 * 
 * @param m Module (should have SFX declared, no song needed)
 * @param stream Output buffer (interleaved stereo int16_t)
 * @param frames Number of stereo frames to generate
 * 
 * @see xfm_mix() for full documentation
 */
void xfm_mix_sfx(xfm_module* m, int16_t* stream, int frames);

/**
 * @brief Set automatic note-off delay for song and SFX playback.
 *
 * When a new note starts on a channel that's still playing a previous note,
 * the previous note is automatically keyed off with a configurable delay.
 * This gives the previous note time to play before the release envelope starts.
 *
 * Applies to both song channels and SFX voices.
 *
 * @param m Module instance
 * @param delay Delay as fraction of row (0.0 = immediate, 1.0 = full row)
 *              Default is 0.3 (note plays for 30% of row, releases for 70%)
 *
 * @note Values near 1.0 may cause note bleeding into next note
 * @note Values near 0.0 give maximum release time but shorter note duration
 */
void xfm_set_auto_off_delay(xfm_module* m, float delay);

/**
 * @brief Get current automatic note-off delay setting.
 *
 * @param m Module instance
 * @return Current delay value (0.0 to 1.0)
 */
float xfm_get_auto_off_delay(xfm_module* m);

#ifdef __cplusplus
}
#endif

#endif /* XFM_H */