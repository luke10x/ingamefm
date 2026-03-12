# ingamefm — header-only FM song playback library

Plays [Furnace tracker](https://github.com/tildearrow/furnace) songs in SDL programs using the **ymfm** YM2612 chip emulator.

---

## Files

| File | Purpose |
|---|---|
| `ingamefm_patchlib.h` | YM2612Patch/YM2612Operator structs + IngameFMChip register-programming wrapper |
| `ingamefm_player.h` | Furnace pattern text parser + SDL audio-driven playback engine |
| `ingamefm.h` | Convenience header — includes both modules |
| `demo.cpp` | Demo program playing a slap-bass riff |

---

## Dependencies

- **ymfm** — <https://github.com/aaronsgiles/ymfm> (ymfm.h, ymfm_opn.h, ymfm_opn.cpp)
- **SDL 2** — audio only; link with `$(sdl2-config --libs)`
- C++17 compiler

---

## Build (Linux)

```bash
g++ -std=c++17 -O2 demo.cpp \
    -I/path/to/ymfm/src \
    -I/path/to/SDL2/include \
    /path/to/ymfm/src/ymfm_opn.cpp \
    $(sdl2-config --libs) \
    -o ingamefm_demo

./ingamefm_demo
```

---

## Quick start

```cpp
#include "ingamefm.h"

// 1. Define patches
static constexpr YM2612Patch MY_BASS = {
    .ALG = 4, .FB = 5, .AMS = 0, .FMS = 0,
    .op = {
        { .DT=3,.MUL=1,.TL=34,.RS=0,.AR=31,.AM=0,.DR=10,.SR=6,.SL=4,.RR=7,.SSG=0 },
        { .DT=0,.MUL=2,.TL=18,.RS=1,.AR=25,.AM=0,.DR=12,.SR=5,.SL=5,.RR=6,.SSG=0 },
        { .DT=0,.MUL=1,.TL= 0,.RS=0,.AR=31,.AM=0,.DR= 6,.SR=3,.SL=6,.RR=5,.SSG=0 },
        { .DT=0,.MUL=1,.TL= 0,.RS=0,.AR=31,.AM=0,.DR= 7,.SR=2,.SL=5,.RR=5,.SSG=0 },
    }
};

// 2. Copy pattern text from Furnace (Edit → Copy Pattern Data)
static const char* SONG = R"(
org.tildearrow.furnace - Pattern Data (8)
8
E-2007F....|...........|
G-2007F....|...........|
A-2007F....|...........|
OFF........|...........|
E-2007F....|...........|
G-2007F....|...........|
A-2007F....|...........|
OFF........|...........|
)";

int main(int, char**) {
    SDL_Init(SDL_INIT_AUDIO);

    IngameFMPlayer player;
    player.set_song(SONG, /*tick_rate=*/60, /*speed=*/4);
    player.add_patch(0, MY_BASS);   // instrument id 0x00 → MY_BASS
    player.play();                  // blocks until finished

    SDL_Quit();
}
```

---

## Pattern format

Furnace patterns are copied via **Edit → Copy Pattern Data**.  The text looks like:

```
org.tildearrow.furnace - Pattern Data (N)
<row_count>
<note><inst><vol><effects>|<note><inst><vol><effects>|...
...
```

### Column layout (per channel)

| Chars | Field | Example |
|---|---|---|
| 3 | Note name | `C-4`, `F#2`, `OFF`, `...` |
| 2 | Instrument (hex) | `0B`, `..` |
| 2 | Volume (hex) | `7F`, `..` |
| 3n | Effects (ignored) | `EC0`, `...` |

- `...` in any field means "inherit from previous row".
- `OFF` triggers key-off on the channel.
- Channels remember the last instrument and volume until a new value is specified.
- Effects are parsed and silently ignored.

---

## Tempo

```
seconds per row = speed / tick_rate
```

Furnace's **Speed** setting is ticks-per-row; **Tick Rate** is ticks per second.  Typical values: `tick_rate=60, speed=6` → 10 rows/second.

---

## Channel limit

`IngameFMPlayer` drives **3 YM2612 channels** (port 0, channels 0-2).  Channels beyond index 2 in the pattern are parsed but produce no sound.  Extend `MAX_CHANNELS` to 6 and add port-1 register writes to `ingamefm_patchlib.h` if you need the full 6-channel YM2612.

---

## Error handling

`parse_ingamefm_song()` throws `std::runtime_error` with a message identifying the line number and what was expected vs found.  Both `set_song()` and `play()` propagate these exceptions.

---

## Patch format reference

```cpp
struct YM2612Operator {
    int DT;  // Detune:      -3..+3 (mapped to 0-7 for hardware)
    int MUL; // Multiplier:  0-15
    int TL;  // Total Level: 0-127  (0 = loudest)
    int RS;  // Rate Scaling: 0-3
    int AR;  // Attack Rate:  0-31
    int AM;  // AM Enable:    0-1
    int DR;  // Decay Rate:   0-31
    int SR;  // Sustain Rate: 0-31
    int SL;  // Sustain Level:0-15
    int RR;  // Release Rate: 0-15
    int SSG; // SSG-EG:       0=off, 1-8=modes 0-7
};

struct YM2612Patch {
    int ALG; // Algorithm:       0-7
    int FB;  // Feedback:        0-7
    int AMS; // AM Sensitivity:  0-3
    int FMS; // FM Sensitivity:  0-7
    YM2612Operator op[4]; // OP1, OP2, OP3, OP4
};
```
