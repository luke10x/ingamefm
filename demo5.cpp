// =============================================================================
// demo5.cpp  — ingamefm: 4-channel music + game SFX priority system
//
// Channels:
//   Ch0 — Lead   (PATCH_GUITAR)     music-only
//   Ch1 — Pad    (PATCH_FLUTE)      music-only
//   Ch2 — Beat   (PATCH_HIHAT/CLANG) SFX voice  \  sfx_reserve(2)
//   Ch3 — Bass   (PATCH_SLAP_BASS)  SFX voice   /  (rightmost = highest priority target)
//
// Tempo: tick_rate=60, speed=20  ->  44100/60*20 = 14700 samples/row (~333ms)
//        32 rows  ->  ~10.7s loop
//
// Song: A minor pentatonic, slow drifting feel.
//   Lead: slow melodic phrases, 2-4 rows per note.
//   Pad:  long sustained chords, 4-8 rows each.
//   Beat: clang on beats 1&3, hihat on beats 2&4 (every 4 rows at this tempo).
//   Bass: walking root-fifth pattern.
//
// SFX — all fire immediately on their own clock (speed=3, ~50ms/row):
//   1  — DING      priority 1   short blip
//   2  — ALARM     priority 3   two-hit warning
//   3  — FANFARE   priority 5   ascending run (existing three)
//   q  — JUMP      priority 4   upward chirp
//   w  — COIN      priority 3   bright high ping
//   e  — LEVEL_UP  priority 6   5-note triumphant rise
//   r  — DEATH     priority 8   descending sad fall
//   t  — DAMAGE    priority 5   harsh buzz hit
//   y  — ATTACK    priority 4   sharp sword crack
//   u  — CLIMB     priority 2   short ratchet tick
//
// Priorities are designed so game events feel correctly weighted:
//   DEATH and LEVEL_UP dominate, routine actions (ding, coin, climb) yield easily.
//
// Keys: 1 2 3 q w e r t y u  — Esc to quit
// =============================================================================

#include "ingamefm.h"
#include <cstdio>

// =============================================================================
// Song  (4 channels)
//
// Column format: note(3) inst(2) vol(2) = 7 chars, no effects.
//
// Instrument IDs (music):
//   00 = PATCH_GUITAR    ch0 lead
//   01 = PATCH_FLUTE     ch1 pad
//   02 = PATCH_CLANG     ch2 beat — clang
//   03 = PATCH_HIHAT     ch2 beat — hihat
//   04 = PATCH_SLAP_BASS ch3 bass
//
// tick_rate=60, speed=20  ->  ~333ms/row
// 32 rows -> ~10.7s loop
// =============================================================================

static const char* SONG =
"org.tildearrow.furnace - Pattern Data (32)\n"
"32\n"
// Lead(ch0)  Pad(ch1)   Beat(ch2)  Bass(ch3)
/* r 0  */ "A-4007F|A-3017F|A-5027F|A-2047F\n"  // Am chord, clang beat1, bass root
/* r 1  */ ".......|.......|OFF....|.......\n"
/* r 2  */ ".......|.......|A-5037F|.......\n"  // hihat beat2
/* r 3  */ ".......|.......|OFF....|.......\n"
/* r 4  */ "C-5007F|.......|A-5027F|E-2047F\n"  // lead up to C5, clang beat3, bass fifth
/* r 5  */ ".......|.......|OFF....|.......\n"
/* r 6  */ ".......|.......|A-5037F|.......\n"  // hihat beat4
/* r 7  */ ".......|.......|OFF....|.......\n"
/* r 8  */ "E-5007F|E-3017F|A-5027F|A-2047F\n"  // lead E5, pad shifts, bass root
/* r 9  */ ".......|.......|OFF....|.......\n"
/* r10  */ ".......|.......|A-5037F|.......\n"
/* r11  */ ".......|.......|OFF....|.......\n"
/* r12  */ "D-5007F|.......|A-5027F|E-2047F\n"  // lead D5, bass fifth
/* r13  */ ".......|.......|OFF....|.......\n"
/* r14  */ ".......|.......|A-5037F|.......\n"
/* r15  */ ".......|.......|OFF....|.......\n"
/* r16  */ "C-5007F|C-3017F|A-5027F|C-2047F\n"  // mid section: C minor feel, bass C
/* r17  */ ".......|.......|OFF....|.......\n"
/* r18  */ ".......|.......|A-5037F|.......\n"
/* r19  */ ".......|.......|OFF....|.......\n"
/* r20  */ "A-4007F|.......|A-5027F|G-2047F\n"  // lead drops back, bass G
/* r21  */ ".......|.......|OFF....|.......\n"
/* r22  */ ".......|.......|A-5037F|.......\n"
/* r23  */ ".......|.......|OFF....|.......\n"
/* r24  */ "G-4007F|G-3017F|A-5027F|G-2047F\n"  // resolution phrase, G major
/* r25  */ ".......|.......|OFF....|.......\n"
/* r26  */ ".......|.......|A-5037F|.......\n"
/* r27  */ ".......|.......|OFF....|.......\n"
/* r28  */ "A-4007F|A-3017F|A-5027F|A-2047F\n"  // return to Am
/* r29  */ ".......|.......|OFF....|.......\n"
/* r30  */ ".......|.......|A-5037F|.......\n"
/* r31  */ ".......|.......|OFF....|.......\n";

// =============================================================================
// SFX patches (instrument IDs 0x10-0x1A)
//
//   0x10 = PATCH_SLAP_BASS   — punchy low transient (ding, damage)
//   0x11 = PATCH_CLANG       — metallic crack (alarm, attack)
//   0x12 = PATCH_GUITAR      — bright stab (fanfare, jump, level up)
//   0x13 = PATCH_SYNTH_BASS  — fat buzz (death)
//   0x14 = PATCH_HIHAT       — sharp metallic tick (coin, climb)
// =============================================================================

// SFX tick_rate=60, speed=3  ->  2205 samples/row (~50ms/row)

// 1 — DING: single punchy low blip
static const char* SFX_DING =
"3\n"
"C-3107F\n"
"OFF....\n"
".......\n";

// 2 — ALARM: two metallic hits
static const char* SFX_ALARM =
"6\n"
"D-5117F\n"
"OFF....\n"
"A-4117F\n"
"OFF....\n"
".......\n"
".......\n";

// 3 — FANFARE: four-note ascending stab
static const char* SFX_FANFARE =
"10\n"
"C-4127F\n"
"E-4127F\n"
"G-4127F\n"
"C-5127F\n"
"OFF....\n"
".......\n"
".......\n"
".......\n"
".......\n"
".......\n";

// q — JUMP: quick upward chirp (low → high, fast)
static const char* SFX_JUMP =
"5\n"
"C-4127F\n"
"G-4127F\n"
"C-5127F\n"
"OFF....\n"
".......\n";

// w — COIN: bright high ping, very short
static const char* SFX_COIN =
"4\n"
"A-5147F\n"
"E-6147F\n"
"OFF....\n"
".......\n";

// e — LEVEL_UP: triumphant 5-note rise
static const char* SFX_LEVEL_UP =
"12\n"
"C-4127F\n"
"E-4127F\n"
"G-4127F\n"
"C-5127F\n"
"E-5127F\n"
"G-5127F\n"
"OFF....\n"
".......\n"
".......\n"
".......\n"
".......\n"
".......\n";

// r — DEATH: descending sad fall (5 steps down, slow)
static const char* SFX_DEATH =
"14\n"
"A-4137F\n"
".......\n"
"F-4137F\n"
".......\n"
"D-4137F\n"
".......\n"
"B-3137F\n"
".......\n"
"G-3137F\n"
".......\n"
"OFF....\n"
".......\n"
".......\n"
".......\n";

// t — DAMAGE: harsh low buzz hit
static const char* SFX_DAMAGE =
"6\n"
"A-2107F\n"
"G-2107F\n"
"OFF....\n"
".......\n"
".......\n"
".......\n";

// y — ATTACK: short sharp crack
static const char* SFX_ATTACK =
"4\n"
"D-5117F\n"
"OFF....\n"
".......\n"
".......\n";

// u — CLIMB: short repeating ratchet tick (3 quick ticks)
static const char* SFX_CLIMB =
"6\n"
"A-4147F\n"
"OFF....\n"
"A-4147F\n"
"OFF....\n"
".......\n"
".......\n";

// =============================================================================
// SFX table
// =============================================================================

struct SfxEntry {
    int         id;
    int         priority;
    int         duration;   // SFX rows
    SDL_Keycode key;
    const char* pattern;
    const char* name;
};

// Priorities:
//   DEATH=8, LEVEL_UP=6, DAMAGE=5, FANFARE=5,
//   ATTACK=4, JUMP=4, ALARM=3, COIN=3, CLIMB=2, DING=1
static const SfxEntry SFX_TABLE[] =
{
    {  0, 1,  6,  SDLK_1, SFX_DING,     "DING    " },
    {  1, 3, 12,  SDLK_2, SFX_ALARM,    "ALARM   " },
    {  2, 5, 20,  SDLK_3, SFX_FANFARE,  "FANFARE " },
    {  3, 4, 10,  SDLK_q, SFX_JUMP,     "JUMP    " },
    {  4, 3,  8,  SDLK_w, SFX_COIN,     "COIN    " },
    {  5, 6, 24,  SDLK_e, SFX_LEVEL_UP, "LEVEL UP" },
    {  6, 8, 28,  SDLK_r, SFX_DEATH,    "DEATH   " },
    {  7, 5, 12,  SDLK_t, SFX_DAMAGE,   "DAMAGE  " },
    {  8, 4,  8,  SDLK_y, SFX_ATTACK,   "ATTACK  " },
    {  9, 2, 12,  SDLK_u, SFX_CLIMB,    "CLIMB   " },
};
static constexpr int SFX_COUNT = 10;

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
        "ingamefm demo5  |  1 2 3 q w e r t y u = SFX  |  Esc: quit",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        600, 100,
        SDL_WINDOW_SHOWN);
    if (!window)
    {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    IngameFMPlayer player;
    try
    {
        // Music: slow tempo, speed=20 -> ~333ms/row
        player.set_song(SONG, /*tick_rate=*/60, /*speed=*/20);

        // Music patches
        player.add_patch(0x00, PATCH_GUITAR);    // ch0 lead
        player.add_patch(0x01, PATCH_FLUTE);     // ch1 pad
        player.add_patch(0x02, PATCH_CLANG);     // ch2 clang
        player.add_patch(0x03, PATCH_HIHAT);     // ch2 hihat
        player.add_patch(0x04, PATCH_SLAP_BASS); // ch3 bass

        // Reserve last 2 channels (ch2, ch3) as SFX voices
        player.sfx_reserve(2);

        // SFX patches
        player.add_patch(0x10, PATCH_SLAP_BASS);
        player.add_patch(0x11, PATCH_CLANG);
        player.add_patch(0x12, PATCH_GUITAR);
        player.add_patch(0x13, PATCH_SYNTH_BASS);
        player.add_patch(0x14, PATCH_HIHAT);

        // Register all SFX (speed=3 -> ~50ms/row for tight timing)
        for (int i = 0; i < SFX_COUNT; i++)
            player.sfx_define(SFX_TABLE[i].id, SFX_TABLE[i].pattern, 60, 3);
    }
    catch (const std::exception& ex)
    {
        std::fprintf(stderr, "Setup error: %s\n", ex.what());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_AudioSpec desired{};
    desired.freq     = IngameFMPlayer::SAMPLE_RATE;
    desired.format   = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples  = 128;   // small buffer for low SFX latency (~3ms)
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

    player.start(dev, /*loop=*/true);
    SDL_PauseAudioDevice(dev, 0);

    std::printf("=== ingamefm demo5: 4-channel music + game SFX ===\n\n");
    std::printf("  Music (4 channels, ~333ms/row, ~10.7s loop):\n");
    std::printf("    Ch0 Lead  — Guitar (music-only)\n");
    std::printf("    Ch1 Pad   — Flute  (music-only)\n");
    std::printf("    Ch2 Beat  — Clang/Hihat  (SFX voice)\n");
    std::printf("    Ch3 Bass  — Slap Bass    (SFX voice)\n\n");
    std::printf("  SFX (fire immediately on own clock):\n");
    for (int i = 0; i < SFX_COUNT; i++)
    {
        char kn[2] = { '?', 0 };
        if (SFX_TABLE[i].key >= SDLK_a && SFX_TABLE[i].key <= SDLK_z)
            kn[0] = (char)(SFX_TABLE[i].key - SDLK_a + 'a');
        else if (SFX_TABLE[i].key >= SDLK_0 && SFX_TABLE[i].key <= SDLK_9)
            kn[0] = (char)(SFX_TABLE[i].key - SDLK_0 + '0');
        std::printf("    %s  p%d  %s\n", kn, SFX_TABLE[i].priority, SFX_TABLE[i].name);
    }
    std::printf("\n  Esc to quit.\n\n");

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
                    break;
                }

                for (int i = 0; i < SFX_COUNT; i++)
                {
                    if (e.key.keysym.sym == SFX_TABLE[i].key)
                    {
                        SDL_LockAudioDevice(dev);
                        player.sfx_play(SFX_TABLE[i].id,
                                        SFX_TABLE[i].priority,
                                        SFX_TABLE[i].duration);
                        SDL_UnlockAudioDevice(dev);
                        std::printf("  >> %s (p%d)\n",
                                    SFX_TABLE[i].name,
                                    SFX_TABLE[i].priority);
                        break;
                    }
                }
            }
        }
        SDL_WaitEventTimeout(nullptr, 1);
    }

    SDL_CloseAudioDevice(dev);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
