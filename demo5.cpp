// =============================================================================
// demo5.cpp  — ingamefm: 6-channel music + game SFX priority system
//
// All 6 YM2612 voices filled with music. sfx_reserve(3) means ch3/ch4/ch5
// are evictable — every SFX borrows from those. Ch0/ch1/ch2 always play.
//
// Channels:
//   Ch0 — Beat      (PATCH_KICK / PATCH_SNARE)   music, sfx-evictable
//   Ch1 — Lead      (PATCH_GUITAR)               music, sfx-evictable
//   Ch2 — Bass      (PATCH_SLAP_BASS)             music, sfx-evictable
//   Ch3 — Flute     (PATCH_FLUTE)    rows 0-10   music, sfx-evictable
//   Ch4 — Synth pad (PATCH_SUPERSAW) rows 11-21  music, sfx-evictable
//   Ch5 — Elec bass (PATCH_ELECTRIC_BASS) rows 22-31  music, sfx-evictable
//
// Instrument IDs:
//   01=KICK  02=SNARE  03=GUITAR  04=SLAP_BASS
//   05=FLUTE  06=SUPERSAW  07=ELECTRIC_BASS
//
// Tempo: tick_rate=60, speed=20 -> ~333ms/row, 32 rows = ~10.7s loop
// =============================================================================

#include "ingamefm.h"
#include <cstdio>
#include <algorithm>

static const char* SONG =
"org.tildearrow.furnace - Pattern Data (32)\n"
"32\n"
// beat(ch0)  lead(ch1)  bass(ch2)  flute(ch3)  synth(ch4)  ebass(ch5)
/* r 0  */ "C-1017F|A-4037F|A-2047F|C-5057F|.......|.......\n"
/* r 1  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r 2  */ "A-2027F|.......|.......|E-5057F|.......|.......\n"
/* r 3  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r 4  */ "C-1017F|C-5037F|E-2047F|G-5057F|.......|.......\n"
/* r 5  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r 6  */ "A-2027F|.......|.......|E-5057F|.......|.......\n"
/* r 7  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r 8  */ "C-1017F|E-5037F|A-2047F|D-5057F|.......|.......\n"
/* r 9  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r10  */ "A-2027F|.......|.......|C-5057F|OFF....|.......\n"
/* r11  */ "OFF....|.......|.......|OFF....|A-3067F|.......\n"
/* r12  */ "C-1017F|D-5037F|E-2047F|.......|.......|.......\n"
/* r13  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r14  */ "A-2027F|.......|.......|.......|E-3067F|.......\n"
/* r15  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r16  */ "C-1017F|C-5037F|C-2047F|.......|.......|.......\n"
/* r17  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r18  */ "A-2027F|.......|.......|.......|G-3067F|.......\n"
/* r19  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r20  */ "C-1017F|A-4037F|G-2047F|.......|.......|.......\n"
/* r21  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r22  */ "A-2027F|.......|.......|.......|OFF....|E-3077F\n"
/* r23  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r24  */ "C-1017F|G-4037F|G-2047F|.......|.......|A-3077F\n"
/* r25  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r26  */ "A-2027F|.......|.......|.......|.......|G-3077F\n"
/* r27  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r28  */ "C-1017F|A-4037F|A-2047F|.......|.......|E-3077F\n"
/* r29  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r30  */ "A-2027F|.......|.......|.......|.......|D-3077F\n"
/* r31  */ "OFF....|.......|.......|.......|.......|OFF....\n";

// =============================================================================
// SFX patterns
// =============================================================================

static const char* SFX_DING =
"3\n" "C-3107F\n" "OFF....\n" ".......\n";
static const char* SFX_ALARM =
"6\n" "D-5117F\n" "OFF....\n" "A-4117F\n" "OFF....\n" ".......\n" ".......\n";
static const char* SFX_FANFARE =
"10\n" "C-4127F\n" "E-4127F\n" "G-4127F\n" "C-5127F\n"
"OFF....\n" ".......\n" ".......\n" ".......\n" ".......\n" ".......\n";
static const char* SFX_JUMP =
"5\n" "C-4127F\n" "G-4127F\n" "C-5127F\n" "OFF....\n" ".......\n";
static const char* SFX_COIN =
"4\n" "A-5147F\n" "E-6147F\n" "OFF....\n" ".......\n";
static const char* SFX_LEVEL_UP =
"12\n" "C-4127F\n" "E-4127F\n" "G-4127F\n" "C-5127F\n" "E-5127F\n" "G-5127F\n"
"OFF....\n" ".......\n" ".......\n" ".......\n" ".......\n" ".......\n";
static const char* SFX_DEATH =
"14\n" "A-4137F\n" ".......\n" "F-4137F\n" ".......\n" "D-4137F\n" ".......\n"
"B-3137F\n" ".......\n" "G-3137F\n" ".......\n" "OFF....\n" ".......\n" ".......\n" ".......\n";
static const char* SFX_DAMAGE =
"6\n" "A-2107F\n" "G-2107F\n" "OFF....\n" ".......\n" ".......\n" ".......\n";
static const char* SFX_ATTACK =
"4\n" "D-5117F\n" "OFF....\n" ".......\n" ".......\n";
static const char* SFX_CLIMB =
"6\n" "A-4147F\n" "OFF....\n" "A-4147F\n" "OFF....\n" ".......\n" ".......\n";
static const char* SFX_FALL_WATER =
"10\n" "A-4107F\n" "E-4107F\n" "C-4107F\n" "A-3107F\n" "E-3107F\n"
"C-3107F\n" "OFF....\n" ".......\n" ".......\n" ".......\n";
static const char* SFX_TALK =
"8\n" "E-4157F\n" "OFF....\n" "G-4157F\n" "OFF....\n" "E-4157F\n" "OFF....\n" ".......\n" ".......\n";
static const char* SFX_LAUGH =
"10\n" "C-4157F\n" "E-4157F\n" "G-4157F\n" "OFF....\n"
"C-4157F\n" "E-4157F\n" "G-4157F\n" "OFF....\n" ".......\n" ".......\n";
static const char* SFX_SCREAM =
"12\n" "C-3127F\n" "G-3127F\n" "C-4127F\n" "G-4127F\n" "C-5127F\n" "G-5127F\n"
"C-6127F\n" ".......\n" ".......\n" "OFF....\n" ".......\n" ".......\n";
static const char* SFX_CRY =
"14\n" "G-4157F\n" ".......\n" ".......\n" "E-4157F\n" ".......\n" ".......\n"
"D-4157F\n" ".......\n" ".......\n" "B-3157F\n" ".......\n" ".......\n" "OFF....\n" ".......\n";
static const char* SFX_LAUNCH =
"8\n" "C-2117F\n" "OFF....\n" "C-3117F\n" "G-3117F\n" "C-4117F\n" "G-4117F\n" "OFF....\n" ".......\n";
static const char* SFX_WARP =
"12\n" "C-2137F\n" "C-3137F\n" "C-4137F\n" "C-5137F\n" "C-6137F\n" ".......\n"
"C-5137F\n" "C-4137F\n" "C-3137F\n" "C-2137F\n" "OFF....\n" ".......\n";
static const char* SFX_SLIDE =
"8\n" "A-5147F\n" "G-5147F\n" "E-5147F\n" "D-5147F\n" "C-5147F\n" "A-4147F\n" "OFF....\n" ".......\n";
static const char* SFX_SUBMERSE =
"12\n" "A-3137F\n" ".......\n" "G-3137F\n" ".......\n" "E-3137F\n" ".......\n"
"C-3137F\n" ".......\n" "A-2137F\n" ".......\n" "OFF....\n" ".......\n";

struct SfxEntry { int id; int priority; int duration; SDL_Keycode key; const char* pattern; const char* name; };
static const SfxEntry SFX_TABLE[] =
{
    {  0, 1,  6,  SDLK_1, SFX_DING,       "DING      " },
    {  1, 3, 12,  SDLK_2, SFX_ALARM,      "ALARM     " },
    {  2, 5, 20,  SDLK_3, SFX_FANFARE,    "FANFARE   " },
    {  3, 4, 10,  SDLK_q, SFX_JUMP,       "JUMP      " },
    {  4, 3,  8,  SDLK_w, SFX_COIN,       "COIN      " },
    {  5, 6, 24,  SDLK_e, SFX_LEVEL_UP,   "LEVEL UP  " },
    {  6, 8, 28,  SDLK_r, SFX_DEATH,      "DEATH     " },
    {  7, 5, 12,  SDLK_t, SFX_DAMAGE,     "DAMAGE    " },
    {  8, 4,  8,  SDLK_y, SFX_ATTACK,     "ATTACK    " },
    {  9, 2, 12,  SDLK_u, SFX_CLIMB,      "CLIMB     " },
    { 10, 3, 20,  SDLK_a, SFX_FALL_WATER, "FALL/WATER" },
    { 11, 2, 16,  SDLK_s, SFX_TALK,       "TALK      " },
    { 12, 3, 20,  SDLK_d, SFX_LAUGH,      "LAUGH     " },
    { 13, 6, 24,  SDLK_f, SFX_SCREAM,     "SCREAM    " },
    { 14, 3, 28,  SDLK_g, SFX_CRY,        "CRY       " },
    { 15, 5, 16,  SDLK_h, SFX_LAUNCH,     "LAUNCH    " },
    { 16, 5, 24,  SDLK_j, SFX_WARP,       "WARP      " },
    { 17, 3, 16,  SDLK_k, SFX_SLIDE,      "SLIDE     " },
    { 18, 3, 24,  SDLK_l, SFX_SUBMERSE,   "SUBMERSE  " },
};
static constexpr int SFX_COUNT = 19;

static float g_music_vol = 1.0f;
static float g_sfx_vol   = 1.0f;
static constexpr float VOL_STEP = 0.1f;

int main(int /*argc*/, char** /*argv*/)
{
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
    { std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError()); return 1; }

    SDL_Window* window = SDL_CreateWindow(
        "ingamefm demo5  |  1-3 q-u a-l = SFX  |  Up/Dn=music  Lt/Rt=sfx  |  Esc",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 540, 100, SDL_WINDOW_SHOWN);
    if (!window) { SDL_Quit(); return 1; }

    IngameFMPlayer player;
    try
    {
        player.set_song(SONG, 60, 20);

        // Music patches
        player.add_patch(0x01, PATCH_KICK);
        player.add_patch(0x02, PATCH_SNARE);
        player.add_patch(0x03, PATCH_GUITAR);
        player.add_patch(0x04, PATCH_SLAP_BASS);
        player.add_patch(0x05, PATCH_FLUTE);
        player.add_patch(0x06, PATCH_SUPERSAW);
        player.add_patch(0x07, PATCH_ELECTRIC_BASS);

        // ch3, ch4, ch5 are SFX-evictable (ch0/1/2 always play)
        player.sfx_reserve(3);

        // SFX patches
        player.add_patch(0x10, PATCH_SLAP_BASS);
        player.add_patch(0x11, PATCH_CLANG);
        player.add_patch(0x12, PATCH_GUITAR);
        player.add_patch(0x13, PATCH_SYNTH_BASS);
        player.add_patch(0x14, PATCH_HIHAT);
        player.add_patch(0x15, PATCH_ELECTRIC_BASS);

        for (int i = 0; i < SFX_COUNT; i++)
            player.sfx_define(SFX_TABLE[i].id, SFX_TABLE[i].pattern, 60, 3);
    }
    catch (const std::exception& ex)
    { std::fprintf(stderr, "Setup error: %s\n", ex.what()); SDL_DestroyWindow(window); SDL_Quit(); return 1; }

    SDL_AudioSpec desired{};
    desired.freq = IngameFMPlayer::SAMPLE_RATE; desired.format = AUDIO_S16SYS;
    desired.channels = 2; desired.samples = 128;
    desired.callback = IngameFMPlayer::s_audio_callback; desired.userdata = &player;
    SDL_AudioSpec obtained{};
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (dev == 0)
    { std::fprintf(stderr, "SDL_OpenAudioDevice failed\n"); SDL_DestroyWindow(window); SDL_Quit(); return 1; }

    player.start(dev, true);
    SDL_PauseAudioDevice(dev, 0);

    std::printf("=== ingamefm demo5: 6-channel music + SFX ===\n\n");
    std::printf("  Ch0 Beat      KICK/SNARE     always plays\n");
    std::printf("  Ch1 Lead      GUITAR         always plays\n");
    std::printf("  Ch2 Bass      SLAP BASS      always plays\n");
    std::printf("  Ch3 Flute     FLUTE          rows 0-10  (sfx-evictable)\n");
    std::printf("  Ch4 Synth     SUPERSAW       rows 11-21 (sfx-evictable)\n");
    std::printf("  Ch5 E.Bass    ELECTRIC BASS  rows 22-31 (sfx-evictable)\n\n");
    std::printf("  SFX borrow from ch3/ch4/ch5 by priority:\n");
    for (int i = 0; i < SFX_COUNT; i++) {
        char kn[2] = {'?', 0}; SDL_Keycode k = SFX_TABLE[i].key;
        if (k >= SDLK_a && k <= SDLK_z)      kn[0] = (char)(k - SDLK_a + 'a');
        else if (k >= SDLK_0 && k <= SDLK_9) kn[0] = (char)(k - SDLK_0 + '0');
        std::printf("    %s  p%d  %s\n", kn, SFX_TABLE[i].priority, SFX_TABLE[i].name);
    }
    std::printf("\n  Up/Down = music vol    Left/Right = SFX vol\n");
  std::printf("  Music: 100%%   SFX: 100%%\n\n");
  std::printf("  Esc to quit.\n\n");

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { running = false; break; }
            if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                if (e.key.keysym.sym == SDLK_ESCAPE) { running = false; continue; }
                // Volume controls
                if (e.key.keysym.sym == SDLK_UP) {
                    g_music_vol = std::min(1.0f, g_music_vol + VOL_STEP);
                    player.set_music_volume(g_music_vol);
                    std::printf("  Music: %.0f%%\n", g_music_vol * 100.f); continue;
                }
                if (e.key.keysym.sym == SDLK_DOWN) {
                    g_music_vol = std::max(0.0f, g_music_vol - VOL_STEP);
                    player.set_music_volume(g_music_vol);
                    std::printf("  Music: %.0f%%\n", g_music_vol * 100.f); continue;
                }
                if (e.key.keysym.sym == SDLK_RIGHT) {
                    g_sfx_vol = std::min(1.0f, g_sfx_vol + VOL_STEP);
                    player.set_sfx_volume(g_sfx_vol);
                    std::printf("  SFX:   %.0f%%\n", g_sfx_vol * 100.f); continue;
                }
                if (e.key.keysym.sym == SDLK_LEFT) {
                    g_sfx_vol = std::max(0.0f, g_sfx_vol - VOL_STEP);
                    player.set_sfx_volume(g_sfx_vol);
                    std::printf("  SFX:   %.0f%%\n", g_sfx_vol * 100.f); continue;
                }
                for (int i = 0; i < SFX_COUNT; i++) {
                    if (e.key.keysym.sym == SFX_TABLE[i].key) {
                        SDL_LockAudioDevice(dev);
                        player.sfx_play(SFX_TABLE[i].id, SFX_TABLE[i].priority, SFX_TABLE[i].duration);
                        SDL_UnlockAudioDevice(dev);
                        std::printf("  >> %s (p%d)\n", SFX_TABLE[i].name, SFX_TABLE[i].priority);
                        break;
                    }
                }
            }
        }
        SDL_WaitEventTimeout(nullptr, 1);
    }

    SDL_CloseAudioDevice(dev); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
