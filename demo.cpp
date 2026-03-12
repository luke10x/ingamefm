// =============================================================================
// demo.cpp  — ingamefm library demonstration
//
// Plays a short FM bass riff encoded as a furnace pattern.
//
// Build (Linux example):
//   g++ -std=c++17 -O2 demo.cpp \
//       -I/path/to/ymfm/src \
//       -I/path/to/SDL2/include \
//       /path/to/ymfm/src/ymfm_opn.cpp \
//       $(sdl2-config --libs) \
//       -o ingamefm_demo
// =============================================================================

#include "ingamefm.h"
#include <cstdio>

// =============================================================================
// Built-in patch: Slap Bass
// =============================================================================

static constexpr YM2612Patch PATCH_SLAP_BASS =
{
    .ALG = 4,
    .FB  = 5,
    .AMS = 2,
    .FMS = 3,

    .op =
    {
        { .DT = 3, .MUL = 1, .TL = 34, .RS = 0, .AR = 31, .AM = 0, .DR = 10, .SR = 6, .SL = 4, .RR = 7, .SSG = 0 },
        { .DT = 0, .MUL = 2, .TL = 18, .RS = 1, .AR = 25, .AM = 0, .DR = 12, .SR = 5, .SL = 5, .RR = 6, .SSG = 0 },
        { .DT = 0, .MUL = 1, .TL =  0, .RS = 0, .AR = 31, .AM = 0, .DR =  6, .SR = 3, .SL = 6, .RR = 5, .SSG = 0 },
        { .DT = 0, .MUL = 1, .TL =  0, .RS = 0, .AR = 31, .AM = 0, .DR =  7, .SR = 2, .SL = 5, .RR = 5, .SSG = 0 }
    }
};

// =============================================================================
// Demo song — furnace pattern text
//
// Format: header line, row count, then rows with | separated channels.
// Three channels, 16 rows.
// Instrument 0x00 = PATCH_SLAP_BASS (loaded below).
// Volume 0x7F = loudest.
// =============================================================================

static const char* DEMO_SONG =
R"(org.tildearrow.furnace - Pattern Data (16)
16
E-2007F....|...........|...........|
...........|...........|...........|
G-2007F....|...........|...........|
...........|...........|...........|
A-2007F....|...........|...........|
...........|...........|...........|
G-2007F....|...........|...........|
...........|...........|...........|
E-2007F....|...........|...........|
...........|...........|...........|
D-2007F....|...........|...........|
...........|...........|...........|
C-2007F....|...........|...........|
...........|...........|...........|
OFF........|...........|...........|
...........|...........|...........|
)";

// =============================================================================
// main
// =============================================================================

int main(int /*argc*/, char** /*argv*/)
{
    if (SDL_Init(SDL_INIT_AUDIO) != 0)
    {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    std::printf("=== ingamefm demo ===\n");
    std::printf("Playing demo song (slap bass riff)...\n");

    try
    {
        IngameFMPlayer player;

        // tick_rate = 60 Hz, speed = 4 ticks per row
        // → each row lasts 4/60 s ≈ 66.7 ms
        player.set_song(DEMO_SONG, /*tick_rate=*/60, /*speed=*/4);

        // Register instrument 0 → PATCH_SLAP_BASS
        player.add_patch(0, PATCH_SLAP_BASS);

        // Play (blocks until song ends)
        player.play();
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "Error: %s\n", e.what());
        SDL_Quit();
        return 1;
    }

    std::printf("Playback finished.\n");
    SDL_Quit();
    return 0;
}
