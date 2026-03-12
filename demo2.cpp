// =============================================================================
// demo2.cpp  — ingamefm demo: looping background song + keyboard SFX
//
// - Opens an SDL window with an event loop (Esc or window-close to quit)
// - Plays a two-channel bass/chord riff on YM2612 channels 0 and 1, looping
// - Channel 2 is reserved for live keyboard input (lead synth)
//   Z S X D C V G B H N J M  →  C4 C#4 D4 D#4 E4 F4 F#4 G4 G#4 A4 A#4 B4
//
// Build (same flags as demo.cpp, just swap the source file):
//   g++ -std=c++17 -O2 demo2.cpp \
//       -I../bowling/build/macos/sdl2/include/ \
//       -I../bowling/3rdparty/SDL/include \
//       -I../my-ym2612-plugin/build/_deps/ymfm-src/src/ \
//       ../bowling/build/macos/usr/lib/libSDL2.a \
//       ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_misc.cpp \
//       ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_adpcm.cpp \
//       ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_ssg.cpp \
//       ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_opn.cpp \
//       -framework Cocoa -framework IOKit -framework CoreVideo -framework CoreAudio \
//       -framework AudioToolbox -framework ForceFeedback -framework Carbon \
//       -framework Metal -framework GameController -framework CoreHaptics \
//       -lobjc -o demo2
// =============================================================================

#include "ingamefm.h"
#include <cstdio>

// =============================================================================
// Patches
// =============================================================================

// Background: Slap Bass (channels 0-1)
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

// Lead: bright FM bell/lead patch on channel 2 (keyboard-triggered)
static constexpr YM2612Patch PATCH_LEAD =
{
    .ALG = 5,
    .FB  = 3,
    .AMS = 0,
    .FMS = 0,
    .op =
    {
        { .DT = 1, .MUL = 1, .TL = 20, .RS = 1, .AR = 31, .AM = 0, .DR = 12, .SR = 4, .SL = 3, .RR = 8, .SSG = 0 },
        { .DT = 0, .MUL = 2, .TL =  0, .RS = 0, .AR = 31, .AM = 0, .DR =  8, .SR = 3, .SL = 4, .RR = 6, .SSG = 0 },
        { .DT = 2, .MUL = 1, .TL = 16, .RS = 0, .AR = 31, .AM = 0, .DR = 10, .SR = 3, .SL = 3, .RR = 7, .SSG = 0 },
        { .DT = 0, .MUL = 1, .TL =  0, .RS = 0, .AR = 31, .AM = 0, .DR =  8, .SR = 2, .SL = 3, .RR = 6, .SSG = 0 }
    }
};

// =============================================================================
// Background song — two channels (ch 0 = bass, ch 1 = counter-melody)
// 16 rows, tick_rate=60, speed=6 → ~10 rows/sec → 1.6 sec loop
// Channel column width: note(3) + inst(2) + vol(2) = 7 chars, no effects
// =============================================================================

static const char* SONG =
R"(org.tildearrow.furnace - Pattern Data (16)
16
E-2007F|C-4017F|
.......|.......|
G-2007F|E-4017F|
.......|.......|
A-2007F|G-4017F|
.......|.......|
G-2007F|E-4017F|
.......|.......|
E-2007F|C-4017F|
.......|.......|
D-2007F|A-3017F|
.......|.......|
C-2007F|G-3017F|
.......|.......|
D-2007F|A-3017F|
.......|.......|
)";

// =============================================================================
// Keyboard → MIDI note mapping  (same layout as original patchtest)
// =============================================================================

static int key_to_midi(SDL_Keycode key)
{
    switch (key)
    {
        case SDLK_z: return 60;  // C4
        case SDLK_s: return 61;  // C#4
        case SDLK_x: return 62;  // D4
        case SDLK_d: return 63;  // D#4
        case SDLK_c: return 64;  // E4
        case SDLK_v: return 65;  // F4
        case SDLK_g: return 66;  // F#4
        case SDLK_b: return 67;  // G4
        case SDLK_h: return 68;  // G#4
        case SDLK_n: return 69;  // A4
        case SDLK_j: return 70;  // A#4
        case SDLK_m: return 71;  // B4
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
        "ingamefm demo2 — Z-M to play lead, Esc to quit",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        480, 160,
        SDL_WINDOW_SHOWN
    );

    if (!window)
    {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // -------------------------------------------------------------------------
    // Set up IngameFMPlayer for the background song
    // -------------------------------------------------------------------------

    IngameFMPlayer player;

    try
    {
        player.set_song(SONG, /*tick_rate=*/60, /*speed=*/6);
        player.add_patch(0, PATCH_SLAP_BASS);  // instrument 0x00
        player.add_patch(1, PATCH_LEAD);        // instrument 0x01 (ch 1 counter-melody)
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "Song error: %s\n", e.what());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // -------------------------------------------------------------------------
    // Open a single shared SDL audio device
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

    // start() creates the chip and resets state — must be called first.
    // Audio is still paused so no lock needed yet.
    player.start(dev, /*loop=*/true);

    // Now chip() is valid. Load lead patch onto channel 2 before unpausing.
    player.chip()->load_patch(PATCH_LEAD, 2);

    SDL_PauseAudioDevice(dev, 0);

    std::printf("=== ingamefm demo2 ===\n");
    std::printf("Background song looping on channels 0-1.\n");
    std::printf("Play lead on channel 2: Z S X D C V G B H N J M (C4-B4)\n");
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

                        // Lock audio device while writing to the chip
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
                int midi = key_to_midi(e.key.keysym.sym);
                if (midi >= 0)
                {
                    SDL_LockAudioDevice(dev);
                    player.chip()->key_off(2);
                    SDL_UnlockAudioDevice(dev);
                }
            }
        }

        SDL_Delay(1);
    }

    // -------------------------------------------------------------------------
    // Cleanup
    // -------------------------------------------------------------------------

    SDL_CloseAudioDevice(dev);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
