// =============================================================================
// simple_demo.cpp — eggsfm: Minimal SDL2 demo with keyboard input
// =============================================================================

#include "xfm_api.h"
#include <SDL.h>
#include <cstdio>

// Kick drum patch
static const xfm_patch_opn KICK = {
    .ALG = 0, .FB = 7, .LFO = 0,
    .op = {
        { .DT = 0, .MUL = 1, .TL =  0, .RS = 3, .AR = 31, .AM = 0, .DR = 31, .SR =  0, .SL = 15, .RR = 15, .SSG = 0 },
        { .DT = 0, .MUL = 1, .TL = 16, .RS = 2, .AR = 31, .AM = 0, .DR = 20, .SR =  0, .SL = 15, .RR = 10, .SSG = 0 },
        { .DT = 0, .MUL = 1, .TL = 20, .RS = 1, .AR = 31, .AM = 0, .DR = 18, .SR =  0, .SL = 15, .RR =  8, .SSG = 0 },
        { .DT = 0, .MUL = 1, .TL =  0, .RS = 0, .AR = 31, .AM = 0, .DR = 14, .SR =  0, .SL = 15, .RR =  8, .SSG = 0 }
    }
};

// Snare drum patch
static const xfm_patch_opn SNARE = {
    .ALG = 4, .FB = 7, .LFO = 0,
    .op = {
        { .DT = 3, .MUL = 15, .TL =  0, .RS = 3, .AR = 31, .AM = 0, .DR = 31, .SR =  0, .SL = 15, .RR = 15, .SSG = 0 },
        { .DT = 0, .MUL =  3, .TL =  0, .RS = 2, .AR = 31, .AM = 0, .DR = 28, .SR =  0, .SL = 15, .RR = 14, .SSG = 0 },
        { .DT = 2, .MUL =  7, .TL = 10, .RS = 1, .AR = 31, .AM = 0, .DR = 24, .SR =  0, .SL = 15, .RR = 12, .SSG = 0 },
        { .DT = 0, .MUL =  1, .TL =  2, .RS = 0, .AR = 31, .AM = 0, .DR = 22, .SR =  0, .SL = 15, .RR = 10, .SSG = 0 }
    }
};

// Hi-hat patch
static const xfm_patch_opn HIHAT = {
    .ALG = 7, .FB = 7, .LFO = 0,
    .op = {
        { .DT = 3, .MUL = 13, .TL =  8, .RS = 3, .AR = 31, .AM = 0, .DR = 31, .SR =  0, .SL = 15, .RR = 15, .SSG = 0 },
        { .DT = 2, .MUL = 11, .TL = 12, .RS = 3, .AR = 31, .AM = 0, .DR = 31, .SR =  0, .SL = 15, .RR = 15, .SSG = 0 },
        { .DT = 1, .MUL =  7, .TL = 16, .RS = 3, .AR = 31, .AM = 0, .DR = 30, .SR =  0, .SL = 15, .RR = 14, .SSG = 0 },
        { .DT = 0, .MUL = 15, .TL = 20, .RS = 3, .AR = 31, .AM = 0, .DR = 29, .SR =  0, .SL = 15, .RR = 13, .SSG = 0 }
    }
};

// SFX patterns
static const char* SFX_KICK =
"4\n"
"C-1007F\n"
"OFF....\n"
".......\n"
".......\n";

static const char* SFX_SNARE =
"4\n"
"A-2017F\n"
"OFF....\n"
".......\n"
".......\n";

static const char* SFX_HIHAT =
"2\n"
"C-5027F\n"
"OFF....\n";

// Audio callback
static void audio_callback(void* userdata, Uint8* stream, int len) {
    xfm_mix((xfm_module*)userdata, (int16_t*)stream, len / 4);
}

int main(int argc, char** argv) {
    // Initialize SDL video, audio and events
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Create a simple window
    SDL_Window* window = SDL_CreateWindow(
        "eggsfm Demo — SPACE/K/L=drums, Z-M=piano, ESC=quit",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        480, 120,
        SDL_WINDOW_SHOWN
    );
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Create audio device
    SDL_AudioSpec desired = {0};
    desired.freq     = 44100;
    desired.format   = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples  = 256;
    desired.callback = audio_callback;
    desired.userdata = NULL;

    SDL_AudioDeviceID dev;
    SDL_AudioSpec obtained;

    // Create eggsfm SFX module
    xfm_module* xm = xfm_module_create(44100, 256, XFM_CHIP_YM2612);
    if (!xm) {
        std::fprintf(stderr, "xfm_module_create failed\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Load patches
    xfm_patch_set(xm, 0, &KICK, sizeof(KICK), XFM_CHIP_YM2612);
    xfm_patch_set(xm, 1, &SNARE, sizeof(SNARE), XFM_CHIP_YM2612);
    xfm_patch_set(xm, 2, &HIHAT, sizeof(HIHAT), XFM_CHIP_YM2612);

    // Declare SFX
    xfm_sfx_declare(xm, 0, SFX_KICK, 60, 3);
    xfm_sfx_declare(xm, 1, SFX_SNARE, 60, 3);
    xfm_sfx_declare(xm, 2, SFX_HIHAT, 60, 3);

    // Set audio callback userdata and start audio
    desired.userdata = xm;
    dev = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    if (dev == 0) {
        std::fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        xfm_module_destroy(xm);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Start audio
    SDL_PauseAudioDevice(dev, 0);

    std::printf("eggsfm Demo\n");
    std::printf("===========\n");
    std::printf("Drums: SPACE=Kick, K=Snare, L=Hi-hat\n");
    std::printf("Piano: Z S X D C V G B H N J M (C4-B4)\n");
    std::printf("ESC to quit\n\n");

    // Main event loop
    int running = 1;
    SDL_Event e;
    while (running) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = 0;
            }
            if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        running = 0;
                        break;
                    // Drums
                    case SDLK_SPACE:
                        xfm_sfx_play(xm, 0, 5);
                        std::printf("Kick!\n");
                        break;
                    case SDLK_k:
                        xfm_sfx_play(xm, 1, 5);
                        std::printf("Snare!\n");
                        break;
                    case SDLK_l:
                        xfm_sfx_play(xm, 2, 5);
                        std::printf("Hi-hat!\n");
                        break;
                    // Piano (Z-M keys)
                    case SDLK_z: xfm_note_on(xm, 60, 0, 0); std::printf("C4\n"); break;
                    case SDLK_s: xfm_note_on(xm, 61, 0, 0); std::printf("C#4\n"); break;
                    case SDLK_x: xfm_note_on(xm, 62, 0, 0); std::printf("D4\n"); break;
                    case SDLK_d: xfm_note_on(xm, 63, 0, 0); std::printf("D#4\n"); break;
                    case SDLK_c: xfm_note_on(xm, 64, 0, 0); std::printf("E4\n"); break;
                    case SDLK_v: xfm_note_on(xm, 65, 0, 0); std::printf("F4\n"); break;
                    case SDLK_g: xfm_note_on(xm, 66, 0, 0); std::printf("F#4\n"); break;
                    case SDLK_b: xfm_note_on(xm, 67, 0, 0); std::printf("G4\n"); break;
                    case SDLK_h: xfm_note_on(xm, 68, 0, 0); std::printf("G#4\n"); break;
                    case SDLK_n: xfm_note_on(xm, 69, 0, 0); std::printf("A4\n"); break;
                    case SDLK_j: xfm_note_on(xm, 70, 0, 0); std::printf("A#4\n"); break;
                    case SDLK_m: xfm_note_on(xm, 71, 0, 0); std::printf("B4\n"); break;
                }
            }
        }
        SDL_Delay(1);
    }

    // Cleanup
    xfm_module_destroy(xm);
    SDL_CloseAudioDevice(dev);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
