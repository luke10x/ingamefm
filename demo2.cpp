// =============================================================================
// demo2.cpp  — ingamefm: looping song with volume dynamics + keyboard guitar
//
// Channel 0 — FM Flute melody    instrument 0x00 = PATCH_FLUTE
//             Volume changes throughout: loud (7F), medium (50), quiet (30),
//             then back to loud — clearly audible dynamics.
//
// Channel 1 — FM Kick + Snare    instrument 0x01 = PATCH_KICK
//                                instrument 0x02 = PATCH_SNARE
//             Pattern: kick on beats 1 & 3, snare on beats 2 & 4.
//             Kick note = C1 (MIDI 24), Snare note = A2 (MIDI 45).
//
// Channel 2 — FM Guitar, live keyboard  (Z S X D C V G B H N J M -> C4-B4)
//
// Song: 32 rows, tick_rate=60, speed=6 -> 100ms/row -> 3.2s loop
//
// Build: same flags as demo.cpp, replace source file with demo2.cpp
// =============================================================================

#include "ingamefm.h"
#include <cstdio>

// =============================================================================
// Song
//
// Column format: note(3) + inst(2) + vol(2) = 7 chars, no effects.
//
// Instrument IDs:
//   00 = PATCH_FLUTE  (ch0 melody)
//   01 = PATCH_KICK   (ch1 percussion)
//   02 = PATCH_SNARE  (ch1 percussion)
//
// Volume map (hex):
//   7F = full      (127)
//   60 = loud-ish  ( 96)
//   50 = medium    ( 80)
//   38 = quiet     ( 56)
//   20 = very quiet ( 32)
//
// Melody structure:
//   Rows  0- 7: phrase A — full volume (7F)
//   Rows  8-15: phrase B — drops to medium (50) then quiet (38)
//   Rows 16-23: phrase A repeat — builds back to loud (60) then full (7F)
//   Rows 24-31: ending phrase — full, then fades to quiet, ends on OFF
//
// Percussion pattern (every 4 rows = one beat at this tempo):
//   Row  0: Kick  (beat 1)
//   Row  4: Snare (beat 2)
//   Row  8: Kick  (beat 3)
//   Row 12: Snare (beat 4)
//   ... repeats every 16 rows
// =============================================================================

static const char* SONG =
"org.tildearrow.furnace - Pattern Data (32)\n"
"32\n"
/* row  0 */ "E-4007F|C-1017F|\n"   // Flute E4 LOUD / Kick beat1
/* row  1 */ ".......|.......|\n"
/* row  2 */ "G-4007F|.......|\n"   // Flute G4 LOUD
/* row  3 */ ".......|.......|\n"
/* row  4 */ "A-4007F|A-2027F|\n"   // Flute A4 LOUD / Snare beat2
/* row  5 */ ".......|.......|\n"
/* row  6 */ "G-4007F|.......|\n"   // Flute G4 LOUD
/* row  7 */ ".......|.......|\n"
/* row  8 */ "E-4007F|C-1017F|\n"   // Flute E4 LOUD / Kick beat3
/* row  9 */ ".......|.......|\n"
/* row 10 */ "D-4007F|.......|\n"   // Flute D4 LOUD
/* row 11 */ ".......|.......|\n"
/* row 12 */ "C-4007F|A-2027F|\n"   // Flute C4 LOUD / Snare beat4
/* row 13 */ ".......|.......|\n"
/* row 14 */ "D-4007F|.......|\n"   // Flute D4 LOUD
/* row 15 */ ".......|.......|\n"
/* row 16 */ "E-4006F|C-1017F|\n"   // Flute E4 MEDIUM(50) / Kick
/* row 17 */ ".......|.......|\n"
/* row 18 */ "F#40060|.......|\n"   // Flute F#4 MEDIUM(50)
/* row 19 */ ".......|.......|\n"
/* row 20 */ "G-4006F|A-2027F|\n"   // Flute G4 MEDIUM / Snare
/* row 21 */ ".......|.......|\n"
/* row 22 */ "A-4006F|.......|\n"   // Flute A4 MEDIUM
/* row 23 */ ".......|.......|\n"
/* row 24 */ "G-4006F|C-1017F|\n"   // Flute G4 QUIET(3F) / Kick
/* row 25 */ ".......|.......|\n"
/* row 26 */ "E-4006F|.......|\n"   // Flute E4 QUIET
/* row 27 */ ".......|.......|\n"
/* row 28 */ "D-4006F|A-2027F|\n"   // Flute D4 VERY QUIET(2F) / Snare
/* row 29 */ ".......|.......|\n"
/* row 30 */ "C-4006F|.......|\n"   // Flute C4 back to LOUD — surprise!
/* row 31 */ "OFF....|OFF....|\n";

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
    // Set up player  (instruments 00=Flute, 01=Kick, 02=Snare)
    // -------------------------------------------------------------------------

    IngameFMPlayer player;
    try
    {
        player.set_song(SONG, /*tick_rate=*/60, /*speed=*/6);
        player.add_patch(0x00, PATCH_FLUTE);
        player.add_patch(0x01, PATCH_KICK);
        player.add_patch(0x02, PATCH_SNARE);
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

    // start() must be called before chip() to create the chip
    player.start(dev, /*loop=*/true);

    // Load guitar patch onto channel 2 (audio still paused)
    player.chip()->load_patch(PATCH_GUITAR, 2);

    SDL_PauseAudioDevice(dev, 0);

    std::printf("=== ingamefm demo2 ===\n");
    std::printf("Ch0: Flute melody  — volume: LOUD rows 0-15, MEDIUM 16-23, QUIET 24-29, back LOUD row 30\n");
    std::printf("Ch1: Kick + Snare  — kick on beats 1&3, snare on beats 2&4\n");
    std::printf("Ch2: Guitar        — Z S X D C V G B H N J M  (C4-B4)\n");
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
                        player.chip()->load_patch(PATCH_GUITAR, 2);
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
