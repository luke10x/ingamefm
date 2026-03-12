// =============================================================================
// demo2.cpp  — ingamefm: looping background song + keyboard guitar
//
// Channel 0 — FM Flute melody   (instrument 0x00)
// Channel 1 — FM Walking Bass   (instrument 0x01)
// Channel 2 — FM Guitar, live keyboard (Z S X D C V G B H N J M -> C4-B4)
//
// Song: 32-row loop, tick_rate=60, speed=5 (~83ms/row, ~2.67s loop)
//
// Build: same flags as demo.cpp, replace source file with demo2.cpp
// =============================================================================

#include "ingamefm.h"
#include <cstdio>

// =============================================================================
// Patches
// =============================================================================

// --- FM Flute ---
// ALG 3: OP1->OP2->OP3->OP4 chain. Soft attack, long sustain, airy tone.
static constexpr YM2612Patch PATCH_FLUTE =
{
    .ALG = 3,
    .FB  = 1,
    .AMS = 0,
    .FMS = 0,
    .op =
    {
        { .DT=1, .MUL=1, .TL=38, .RS=0, .AR=28, .AM=0, .DR=10, .SR=5, .SL=5, .RR=6, .SSG=0 },
        { .DT=0, .MUL=2, .TL=42, .RS=0, .AR=26, .AM=0, .DR=12, .SR=4, .SL=6, .RR=5, .SSG=0 },
        { .DT=2, .MUL=1, .TL=36, .RS=0, .AR=27, .AM=0, .DR= 9, .SR=4, .SL=5, .RR=6, .SSG=0 },
        { .DT=0, .MUL=1, .TL= 0, .RS=0, .AR=31, .AM=0, .DR= 7, .SR=3, .SL=4, .RR=7, .SSG=0 }
    }
};

// --- FM Walking Bass ---
// ALG 4: (OP1->OP2) + (OP3->OP4). Fast attack, short decay, punchy low-end.
static constexpr YM2612Patch PATCH_BASS =
{
    .ALG = 4,
    .FB  = 6,
    .AMS = 0,
    .FMS = 0,
    .op =
    {
        { .DT=3, .MUL=1, .TL=32, .RS=1, .AR=31, .AM=0, .DR=12, .SR=5, .SL=4, .RR=8, .SSG=0 },
        { .DT=0, .MUL=2, .TL= 0, .RS=1, .AR=31, .AM=0, .DR=14, .SR=4, .SL=5, .RR=7, .SSG=0 },
        { .DT=0, .MUL=1, .TL=28, .RS=0, .AR=31, .AM=0, .DR=10, .SR=3, .SL=5, .RR=6, .SSG=0 },
        { .DT=0, .MUL=1, .TL= 0, .RS=0, .AR=31, .AM=0, .DR= 8, .SR=2, .SL=4, .RR=6, .SSG=0 }
    }
};

// --- FM Guitar (channel 2, keyboard-triggered) ---
// ALG 5: OP1->(OP2, OP3, OP4). Bright pluck, classic FM electric guitar tone.
static constexpr YM2612Patch PATCH_GUITAR =
{
    .ALG = 3,
    .FB  = 7,
    .AMS = 0,
    .FMS = 0,

    .op =
    {
        { .DT = 3, .MUL = 15, .TL = 61, .RS = 0, .AR = 11, .AM = 0, .DR = 0, .SR = 0, .SL = 10, .RR = 0, .SSG = 0 },
        { .DT = 3, .MUL = 1, .TL = 4, .RS = 0, .AR = 21, .AM = 0, .DR = 18, .SR = 0, .SL = 2, .RR = 4, .SSG = 0 },
        { .DT = -2, .MUL = 7, .TL = 19, .RS = 0, .AR = 31, .AM = 0, .DR = 31, .SR = 0, .SL = 15, .RR = 9, .SSG = 1 },
        { .DT = 0, .MUL = 2, .TL = 6, .RS = 0, .AR = 21, .AM = 0, .DR = 5, .SR = 0, .SL = 1, .RR = 5, .SSG = 0 }
    }
};

// =============================================================================
// Background song — 32 rows, 2 channels
//
// Ch0: Flute melody  (instrument 00)
// Ch1: Walking bass  (instrument 01)
//
// Column format: note(3) + inst(2) + vol(2) = 7 chars, no effects.
// Volume 7F = full, 5F = ~75%, 40 = ~50%.
// Note on row 26: 5F demonstrates mid-song volume change being remembered.
// =============================================================================

static const char* SONG =
"org.tildearrow.furnace - Pattern Data (32)\n"
"32\n"
"E-4007F|C-2017F|\n"
".......|.......|\n"
"G-4007F|.......|\n"
".......|E-2017F|\n"
"A-4007F|.......|\n"
".......|.......|\n"
"G-4007F|G-2017F|\n"
".......|.......|\n"
"E-4007F|C-2017F|\n"
".......|.......|\n"
"D-4007F|.......|\n"
".......|A-1017F|\n"
"C-4007F|.......|\n"
".......|.......|\n"
"D-4007F|G-1017F|\n"
".......|.......|\n"
"E-4007F|C-2017F|\n"
".......|.......|\n"
"G-4007F|.......|\n"
".......|E-2017F|\n"
"B-4007F|.......|\n"
".......|.......|\n"
"A-4007F|A-2017F|\n"
".......|.......|\n"
"G-4007F|.......|\n"
".......|G-2017F|\n"
"E-4005F|.......|\n"
".......|.......|\n"
"D-4007F|D-2017F|\n"
".......|.......|\n"
"C-4007F|.......|\n"
"OFF....|OFF....|\n";

// =============================================================================
// Keyboard -> MIDI note
// =============================================================================

static int key_to_midi(SDL_Keycode key)
{
    switch (key)
    {
        case SDLK_z: return 60;
        case SDLK_s: return 61;
        case SDLK_x: return 62;
        case SDLK_d: return 63;
        case SDLK_c: return 64;
        case SDLK_v: return 65;
        case SDLK_g: return 66;
        case SDLK_b: return 67;
        case SDLK_h: return 68;
        case SDLK_n: return 69;
        case SDLK_j: return 70;
        case SDLK_m: return 71;
    }
    return -1;
}

// =============================================================================
// main
// =============================================================================

int main(int /*argc*/, char** /*argv*/)
{
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
    {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "ingamefm demo2  |  Z-M: guitar  |  Esc: quit",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        480, 120,
        SDL_WINDOW_SHOWN
    );
    if (!window)
    {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // -------------------------------------------------------------------------
    // Set up player
    // -------------------------------------------------------------------------

    IngameFMPlayer player;
    try
    {
        player.set_song(SONG, /*tick_rate=*/60, /*speed=*/5);
        player.add_patch(0x00, PATCH_FLUTE);
        player.add_patch(0x01, PATCH_BASS);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "Song error: %s\n", e.what());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // -------------------------------------------------------------------------
    // Open shared SDL audio device
    // -------------------------------------------------------------------------

    SDL_AudioSpec desired{};
    desired.freq     = IngameFMPlayer::SAMPLE_RATE;
    desired.format   = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples  = 512;
    desired.callback = IngameFMPlayer::s_audio_callback;
    desired.userdata = &player;

    SDL_AudioSpec obtained{};
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (dev == 0)
    {
        std::fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // start() creates the chip — must happen before chip() is accessed
    player.start(dev, /*loop=*/true);

    // Guitar on channel 2 — audio still paused, no lock needed yet
    player.chip()->load_patch(PATCH_GUITAR, 2);

    SDL_PauseAudioDevice(dev, 0);

    std::printf("=== ingamefm demo2 ===\n");
    std::printf("Ch0: FM Flute melody   (looping background)\n");
    std::printf("Ch1: FM Walking Bass   (looping background)\n");
    std::printf("Ch2: FM Guitar         Z S X D C V G B H N J M  (C4-B4)\n");
    std::printf("Esc or close window to quit.\n\n");

    // -------------------------------------------------------------------------
    // SDL event loop
    // -------------------------------------------------------------------------

    bool running = true;
    while (running)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT)
                running = false;

            if (e.type == SDL_KEYDOWN && !e.key.repeat)
            {
                if (e.key.keysym.sym == SDLK_ESCAPE)
                {
                    running = false;
                }
                else
                {
                    int midi = key_to_midi(e.key.keysym.sym);
                    if (midi >= 0)
                    {
                        double hz = IngameFMChip::midi_to_hz(midi);
                        SDL_LockAudioDevice(dev);
                        player.chip()->key_off(2);
                        player.chip()->set_frequency(2, hz, 0);
                        player.chip()->key_on(2);
                        SDL_UnlockAudioDevice(dev);
                    }
                }
            }

            if (e.type == SDL_KEYUP)
            {
                if (key_to_midi(e.key.keysym.sym) >= 0)
                {
                    SDL_LockAudioDevice(dev);
                    player.chip()->key_off(2);
                    SDL_UnlockAudioDevice(dev);
                }
            }
        }

        SDL_Delay(1);
    }

    SDL_CloseAudioDevice(dev);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
