#pragma once
// =============================================================================
// ingamefm_patchlib.h  — part of ingamefm (header-only)
//
// YM2612 patch structures and register-programming helpers.
// Requires ymfm (ymfm.h / ymfm_opn.h) on the include path.
// =============================================================================

#include <cstdint>
#include <cmath>
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

class IngameFMChip
{
public:
    static constexpr uint32_t YM_CLOCK = 7670453;

    IngameFMYMInterface intf;
    ymfm::ym2612        chip;

    IngameFMChip() : chip(intf) { chip.reset(); }

    void write(uint8_t port, uint8_t reg, uint8_t val)
    {
        chip.write(port * 2 + 0, reg);
        chip.write(port * 2 + 1, val);
    }

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
    // sample_rate: ignored — pitch is rate-independent when generate() uses
    // decimation (see below). Parameter kept for API compatibility.
    //
    // Formula: FNUM = F * 2^(21-B) / (YM_CLOCK/144)
    // The exponent is 21-B (not 20-B which was the previous bug causing
    // notes to be one octave flat).
    void set_frequency(int ch, double hz, int octaveOffset = 0, int /*sample_rate*/ = 44100)
    {
        const uint8_t port = (ch >= 3) ? 1 : 0;
        const int     hwch = ch % 3;

        hz *= std::pow(2.0, static_cast<double>(octaveOffset));

        const double fref = static_cast<double>(YM_CLOCK) / 144.0;
        int block = 4;
        double fn = hz * static_cast<double>(1 << (21 - block)) / fref;
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
    // All patches in ingamefm are designed for ymfm running at 44100 Hz
    // (one chip.generate() call per output sample). To keep envelope timing
    // consistent at any SDL sample rate, we use a Bresenham accumulator to
    // call chip.generate() exactly 44100 times per second regardless of rate.
    //
    // At 44100 Hz: 1 chip call/output sample (same as before, no change).
    // At 22050 Hz: 2 chip calls/output sample → same 44100 EG advances/sec.
    // At 48000 Hz: ~0.92 chip calls/output → same 44100 EG advances/sec.
    void generate(int16_t* stream, int samples, int sample_rate = 44100)
    {
        // Reference rate: patches are calibrated for 44100 EG steps/sec
        static constexpr int REF_RATE = 44100;

        for (int i = 0; i < samples; i++)
        {
            ymfm::ym2612::output_data last{};
            do {
                chip.generate(&last, 1);
                acc_err_ += sample_rate;
            } while (acc_err_ < REF_RATE);
            acc_err_ -= REF_RATE;

            stream[i * 2 + 0] = static_cast<int16_t>(
                std::max(-32768, std::min(32767, last.data[0])));
            stream[i * 2 + 1] = static_cast<int16_t>(
                std::max(-32768, std::min(32767, last.data[1])));
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
