#pragma once
// =============================================================================
// ingamefm_patchlib.h  — part of ingamefm (header-only)
//
// YM2612 patch structures and register-programming helpers.
// Requires ymfm (ymfm.h / ymfm_opn.h) on the include path.
// =============================================================================

#include <cstdint>
#include <cmath>
#include <memory>
#include "ymfm.h"
#include "ymfm_opn.h"

// -----------------------------------------------------------------------------
// Patch data structures
// -----------------------------------------------------------------------------

struct YM2612Operator
{
    int DT;   // Detune  (-3..+3, mapped to hw 0-7)
    int MUL;  // Multiplier
    int TL;   // Total Level
    int RS;   // Rate Scaling
    int AR;   // Attack Rate
    int AM;   // AM Enable
    int DR;   // Decay Rate
    int SR;   // Sustain Rate
    int SL;   // Sustain Level
    int RR;   // Release Rate
    int SSG;  // SSG-EG  (0=off, 1-8=enabled modes 0-7)
};

struct YM2612Patch
{
    int ALG;  // Algorithm
    int FB;   // Feedback
    int AMS;  // AM Sensitivity
    int FMS;  // FM Sensitivity

    YM2612Operator op[4];
};

// -----------------------------------------------------------------------------
// ymfm interface shim (minimal)
// -----------------------------------------------------------------------------

class IngameFMYMInterface : public ymfm::ymfm_interface
{
public:
    void ymfm_set_timer(uint32_t, int32_t) override {}
    void ymfm_set_busy_end(uint32_t)       override {}
    bool ymfm_is_busy()                    override { return false; }
};

// -----------------------------------------------------------------------------
// IngameFMChip  — thin wrapper around ymfm::ym2612
// -----------------------------------------------------------------------------

// Supported OPN2-family chip types (all share the same 6-ch FM register layout)
enum class IngameFMChipType {
    YM2612,  // OPN2  — Sega Mega Drive, authentic dirty DAC
    YM3438,  // OPN2C — CMOS, clean
};

// Forward-declare the template; specializations below
template<IngameFMChipType TYPE = IngameFMChipType::YM2612>
class IngameFMChipImpl;

// ── YM2612 ──────────────────────────────────────────────────────────────────
template<> class IngameFMChipImpl<IngameFMChipType::YM2612>
{
public:
    static constexpr uint32_t YM_CLOCK = 7670453;
    IngameFMYMInterface intf;
    ymfm::ym2612 chip;
    IngameFMChipImpl() : chip(intf) { chip.reset(); }
    void raw_write(uint8_t addr, uint8_t data) { chip.write(addr, data); }
    void raw_generate(ymfm::ym2612::output_data& out) { chip.generate(&out, 1); }
    void reset() { chip.reset(); }
};

// ── YM3438 ──────────────────────────────────────────────────────────────────
template<> class IngameFMChipImpl<IngameFMChipType::YM3438>
{
public:
    static constexpr uint32_t YM_CLOCK = 7670453;
    IngameFMYMInterface intf;
    ymfm::ym3438 chip;
    IngameFMChipImpl() : chip(intf) { chip.reset(); }
    void raw_write(uint8_t addr, uint8_t data) { chip.write(addr, data); }
    void raw_generate(ymfm::ym3438::output_data& out) { chip.generate(&out, 1); }
    void reset() { chip.reset(); }
};

// IngameFMChip — runtime-selectable chip type wrapper.
// Only the selected chip is allocated — no wasted init cost.
class IngameFMChip
{
public:
    static constexpr uint32_t YM_CLOCK = 7670453;

    IngameFMChipType chip_type = IngameFMChipType::YM3438;

    // Only one of these is non-null at a time
    std::unique_ptr<IngameFMChipImpl<IngameFMChipType::YM2612>> chip2612;
    std::unique_ptr<IngameFMChipImpl<IngameFMChipType::YM3438>> chip3438;

    IngameFMChip() { allocate_chip(chip_type); }

    void set_chip_type(IngameFMChipType t) {
        chip_type = t;
        chip2612.reset();
        chip3438.reset();
        allocate_chip(t);
    }

    void reset_chip() {
        switch(chip_type) {
            case IngameFMChipType::YM2612: if(chip2612) chip2612->reset(); break;
            case IngameFMChipType::YM3438: if(chip3438) chip3438->reset(); break;
        }
    }

    void write(uint8_t port, uint8_t reg, uint8_t val) {
        uint8_t addr = port * 2;
        switch(chip_type) {
            case IngameFMChipType::YM2612:
                chip2612->chip.write(addr,   reg);
                chip2612->chip.write(addr+1, val); break;
            case IngameFMChipType::YM3438:
                chip3438->chip.write(addr,   reg);
                chip3438->chip.write(addr+1, val); break;
        }
    }

    void do_generate(int16_t* L, int16_t* R) {
        int32_t l=0, r=0;
        switch(chip_type) {
            case IngameFMChipType::YM2612: {
                ymfm::ym2612::output_data out;
                chip2612->chip.generate(&out, 1);
                l=out.data[0]; r=out.data[1]; break;
            }
            case IngameFMChipType::YM3438: {
                ymfm::ym3438::output_data out;
                chip3438->chip.generate(&out, 1);
                l=out.data[0]; r=out.data[1]; break;
            }
        }
        *L = static_cast<int16_t>(std::max(-32768, std::min(32767, l)));
        *R = static_cast<int16_t>(std::max(-32768, std::min(32767, r)));
    }

private:
    void allocate_chip(IngameFMChipType t) {
        switch(t) {
            case IngameFMChipType::YM2612: chip2612 = std::make_unique<IngameFMChipImpl<IngameFMChipType::YM2612>>(); break;
            case IngameFMChipType::YM3438: chip3438 = std::make_unique<IngameFMChipImpl<IngameFMChipType::YM3438>>(); break;
        }
    }
public:

    void load_patch(const YM2612Patch& p, int ch)
    {
        // YM2612 slot order in registers: OP1, OP3, OP2, OP4
        // Patch array order:              OP1, OP2, OP3, OP4
        const int slotMap[4] = { 0, 2, 1, 3 };

        const uint8_t port = (ch >= 3) ? 1 : 0;
        const int     hwch = ch % 3;

        for (int patchOp = 0; patchOp < 4; patchOp++)
        {
            int hwSlot     = slotMap[patchOp];
            const auto& op = p.op[patchOp];

            uint8_t dt_hw = static_cast<uint8_t>((op.DT + 3) & 0x07);
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

    void enable_lfo(bool enable, uint8_t freq = 0)
    {
        write(0, 0x22, enable ? (0x08 | (freq & 0x07)) : 0x00);
    }

    // Set frequency for channel ch (0-5).
    // octaveOffset: additional octave shift (from patch BLOCK field).
    // sample_rate: accepted for API compatibility but not used — pitch is
    // always computed with fref=chip.sample_rate() to match generate()'s REF_RATE.
    //
    // generate() calls chip.generate() at chip's native rate regardless of SDL rate.
    // So chip's effective output rate is always its native rate, and FNUM must be
    // computed against that fixed reference: FNUM = hz * 2^(21-B) / chip_native_rate.
    // This makes pitch identical at any output sample rate.
    void set_frequency(int ch, double hz, int octaveOffset = 0, int /*sample_rate*/ = 0)
    {
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

    void key_on(int ch)
    {
        write(0, 0x28, 0xF0 | ((ch >= 3) ? 0x04 : 0x00) | (ch % 3));
    }

    void key_off(int ch)
    {
        write(0, 0x28, ((ch >= 3) ? 0x04 : 0x00) | (ch % 3));
    }

    // Generate 'samples' stereo frames for an SDL device at sample_rate Hz.
    //
    // All patches in ingamefm are designed for ymfm running at chip's native rate.
    // To keep envelope timing consistent at any SDL sample rate, we use a Bresenham
    // accumulator to call chip.generate() at chip's native rate regardless of output rate.
    //
    // At output = chip native rate: 1 chip call/output sample.
    // At lower rates: multiple chip calls/output sample → same EG advances/sec.
    // At higher rates: fractional chip calls/output → same EG advances/sec.
    void generate(int16_t* stream, int samples, int sample_rate = 44100)
    {
        // REF_RATE = chip's native sample rate at YM_CLOCK
        // YM2612: internal clock = master / 6, sample rate = internal / 24 = master / 144
        static constexpr int REF_RATE = static_cast<int>(YM_CLOCK / 144);  // ~53267 Hz
        for (int i = 0; i < samples; i++)
        {
            int16_t L, R;
            do {
                do_generate(&L, &R);
                acc_err_ += sample_rate;
            } while (acc_err_ < REF_RATE);
            acc_err_ -= REF_RATE;
            stream[i * 2 + 0] = L;
            stream[i * 2 + 1] = R;
        }
    }

private:
    int acc_err_ = 0;

public:

    static double midi_to_hz(int midiNote)
    {
        return 440.0 * std::pow(2.0, (midiNote - 69) / 12.0);
    }
};
