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
// Provides register writes, patch loading, frequency setting, key on/off,
// and stereo sample generation.
// -----------------------------------------------------------------------------

class IngameFMChip
{
public:
    static constexpr uint32_t YM_CLOCK = 7670453;

    IngameFMYMInterface intf;
    ymfm::ym2612       chip;

    IngameFMChip() : chip(intf) { chip.reset(); }

    // Write one register (port 0 only — channels 0-2)
    void write(uint8_t port, uint8_t reg, uint8_t val)
    {
        chip.write(port * 2 + 0, reg);
        chip.write(port * 2 + 1, val);
    }

    // Load a YM2612Patch into hardware channel ch (0-2 on port 0)
    void load_patch(const YM2612Patch& p, int ch)
    {
        // YM2612 slot order in registers: OP1, OP3, OP2, OP4
        // Patch array order:              OP1, OP2, OP3, OP4
        // Mapping: patchOp → hwSlot
        const int slotMap[4] = { 0, 2, 1, 3 };

        for (int patchOp = 0; patchOp < 4; patchOp++)
        {
            int hwSlot       = slotMap[patchOp];
            const auto& op   = p.op[patchOp];

            // 0x30: DT | MUL
            uint8_t dt_hw = static_cast<uint8_t>((op.DT + 3) & 0x07);
            write(0, 0x30 + hwSlot * 4 + ch, (dt_hw << 4) | (op.MUL & 0x0F));

            // 0x40: TL
            write(0, 0x40 + hwSlot * 4 + ch, op.TL & 0x7F);

            // 0x50: RS | AR
            write(0, 0x50 + hwSlot * 4 + ch, ((op.RS & 0x03) << 6) | (op.AR & 0x1F));

            // 0x60: AM | DR
            write(0, 0x60 + hwSlot * 4 + ch, ((op.AM & 0x01) << 7) | (op.DR & 0x1F));

            // 0x70: SR
            write(0, 0x70 + hwSlot * 4 + ch, op.SR & 0x1F);

            // 0x80: SL | RR
            write(0, 0x80 + hwSlot * 4 + ch, ((op.SL & 0x0F) << 4) | (op.RR & 0x0F));

            // 0x90: SSG-EG
            uint8_t ssg_hw = 0;
            if (op.SSG > 0)
                ssg_hw = 0x08 | ((op.SSG - 1) & 0x07);
            write(0, 0x90 + hwSlot * 4 + ch, ssg_hw);
        }

        // 0xB0: FB | ALG
        write(0, 0xB0 + ch, ((p.FB & 0x07) << 3) | (p.ALG & 0x07));

        // 0xB4: L/R | AMS | FMS
        write(0, 0xB4 + ch, 0xC0 | ((p.AMS & 0x03) << 4) | (p.FMS & 0x07));
    }

    // Enable/disable LFO
    void enable_lfo(bool enable, uint8_t freq = 0)
    {
        write(0, 0x22, enable ? (0x08 | (freq & 0x07)) : 0x00);
    }

    // Set frequency for channel ch given a frequency in Hz
    // octaveOffset shifts the frequency by N octaves (can be negative)
    void set_frequency(int ch, double hz, int octaveOffset = 0)
    {
        hz *= std::pow(2.0, static_cast<double>(octaveOffset));

        const double fref = static_cast<double>(YM_CLOCK) / 144.0;
        int block = 4;
        double fn = hz * static_cast<double>(1 << (20 - block)) / fref;

        while (fn > 0x7FF && block < 7) { block++; fn /= 2.0; }
        while (fn < 0x200 && block > 0) { block--; fn *= 2.0; }

        auto fnum = static_cast<uint16_t>(std::min(0x7FF, std::max(0, static_cast<int>(fn))));

        write(0, 0xA4 + ch, ((block & 7) << 3) | ((fnum >> 8) & 0x07));
        write(0, 0xA0 + ch, fnum & 0xFF);
    }

    void key_on(int ch)
    {
        // 0x28: all 4 slots on
        write(0, 0x28, 0xF0 | (ch & 0x07));
    }

    void key_off(int ch)
    {
        write(0, 0x28, ch & 0x07);
    }

    // Generate 'samples' stereo frames into a 16-bit interleaved buffer
    void generate(int16_t* stream, int samples)
    {
        for (int i = 0; i < samples; i++)
        {
            ymfm::ym2612::output_data out;
            chip.generate(&out, 1);
            stream[i * 2 + 0] = out.data[0];
            stream[i * 2 + 1] = out.data[1];
        }
    }

    // Utility: MIDI note → Hz  (A4 = MIDI 69 = 440 Hz)
    static double midi_to_hz(int midiNote)
    {
        return 440.0 * std::pow(2.0, (midiNote - 69) / 12.0);
    }

    // Utility: furnace note name + octave → MIDI note number
    // noteName: 0=C,1=C#,2=D,3=D#,4=E,5=F,6=F#,7=G,8=G#,9=A,10=A#,11=B
    static int note_to_midi(int noteName, int octave)
    {
        // MIDI: C0 = 12, so C4 = 60
        return 12 + octave * 12 + noteName;
    }
};
