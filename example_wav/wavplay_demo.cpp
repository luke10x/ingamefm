// =============================================================================
// wavplay_demo.cpp — WAV Playback Demo (pre-rendered alternative)
// =============================================================================
//
// Demonstrates using xfm_wavplay API to play pre-rendered WAV files
// instead of real-time synthesis. Much lower CPU usage!
//
// Usage: ./wavplay_demo [path_to_exported_wavs]
// Default: ./exporter/exported/
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>

#include <SDL.h>
#include "xfm_wavplay.h"

// Global for signal handler
static xfm_wav_module* g_module = nullptr;
static SDL_AudioDeviceID g_audio_dev = 0;
static bool g_running = true;

void signal_handler(int) {
    g_running = false;
}

// SDL audio callback
void audio_callback(void* userdata, Uint8* stream, int len)
{
    (void)userdata;
    int16_t* buffer = (int16_t*)stream;
    int frames = len / 4;  // len bytes / (2 channels * 2 bytes)
    
    // Mix audio into buffer
    xfm_wav_mix(g_module, buffer, frames);
}

int main(int argc, char** argv)
{
    // Determine WAV directory
    const char* wav_dir = "./exporter/exported";
    if (argc > 1) {
        wav_dir = argv[1];
    }
    
    printf("eggsfm WAV Playback Demo\n");
    printf("========================\n");
    printf("WAV directory: %s\n\n", wav_dir);
    
    // Initialize SDL audio
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    
    SDL_AudioSpec desired = {};
    desired.freq = 44100;
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 256;
    desired.callback = audio_callback;
    
    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);
    if (g_audio_dev == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    
    printf("Audio device opened: 44100 Hz, stereo, 16-bit\n\n");
    
    // Create WAV playback module
    // Same sample rate as exported WAVs (44100 Hz)
    g_module = xfm_wav_module_create(44100, 256);
    if (!g_module) {
        fprintf(stderr, "Failed to create WAV module\n");
        SDL_CloseAudioDevice(g_audio_dev);
        SDL_Quit();
        return 1;
    }
    
    // Load pre-rendered WAV files
    // These would be created by the exporter tool
    char filename[512];
    
    printf("Loading songs...\n");
    snprintf(filename, sizeof(filename), "%s/song_1.wav", wav_dir);
    if (xfm_wav_load_file(g_module, XFM_WAV_SONG, 1, filename) == 0) {
        printf("  Loaded song 1\n");
    }
    
    snprintf(filename, sizeof(filename), "%s/song_2.wav", wav_dir);
    if (xfm_wav_load_file(g_module, XFM_WAV_SONG, 2, filename) == 0) {
        printf("  Loaded song 2\n");
    }
    
    printf("\nLoading SFX...\n");
    const char* sfx_names[] = { "jump", "coin", "alarm", "fanfare" };
    for (int i = 0; i < 4; i++) {
        snprintf(filename, sizeof(filename), "%s/sfx_%s.wav", wav_dir, sfx_names[i]);
        if (xfm_wav_load_file(g_module, XFM_WAV_SFX, i, filename) == 0) {
            printf("  Loaded SFX %s\n", sfx_names[i]);
        }
    }
    
    printf("\n");
    
    // Set up signal handler for clean exit
    signal(SIGINT, signal_handler);
    
    // Start audio playback
    SDL_PauseAudioDevice(g_audio_dev, 0);
    
    // Play song 1 with looping
    printf("Playing song 1 (looping)...\n");
    printf("Press Ctrl+C to stop\n\n");
    xfm_wav_song_play(g_module, 1, true);
    
    // Main loop - just wait for user to stop
    int iterations = 0;
    int max_iterations = 1000;  // Limit for demo (~10 seconds at 100Hz)
    
    while (g_running && iterations < max_iterations) {
        SDL_Delay(10);  // 100 Hz
        iterations++;
        
        // Demo: play SFX periodically
        if (iterations == 200) {
            printf("Playing SFX: jump\n");
            xfm_wav_sfx_play(g_module, 0, 5);
        }
        if (iterations == 400) {
            printf("Playing SFX: coin\n");
            xfm_wav_sfx_play(g_module, 1, 5);
        }
        if (iterations == 600) {
            printf("Playing SFX: alarm\n");
            xfm_wav_sfx_play(g_module, 2, 5);
        }
        if (iterations == 800) {
            printf("Switching to song 2...\n");
            xfm_wav_song_schedule(g_module, 2, XFM_WAV_SWITCH_ROW);
        }
    }
    
    // Cleanup
    printf("\nStopping playback...\n");
    SDL_PauseAudioDevice(g_audio_dev, 1);
    xfm_wav_sfx_stop_all(g_module);
    xfm_wav_module_destroy(g_module);
    g_module = nullptr;
    
    SDL_CloseAudioDevice(g_audio_dev);
    SDL_Quit();
    
    printf("Done!\n");
    
    return 0;
}
