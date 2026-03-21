// =============================================================================
// demo7.cpp — ingamefm: configurable audio system init/teardown (ImGui + Emscripten)
//
// Features:
//   - Init / teardown sound system from ImGui controls
//   - Choose sample rate (22050 / 44100 / 48000) and SDL buffer size
//   - 5-channel song plays on start
//   - 4 SFX buttons (JUMP, COIN, ALARM, FANFARE)
//   - Music / SFX volume sliders
//   - Aurora background shader
//
// Build — see BUILD COMMANDS at bottom of file.
// =============================================================================

// =============================================================================
// 1. INCLUDES
// =============================================================================

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#  include <emscripten/html5.h>
#  include <GLES3/gl3.h>
#else
#  include <SDL_opengles2.h>
#endif

#include <SDL.h>
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>

#include "ingamefm.h"
#include "ingamefm_patch_editor.h"

// =============================================================================
// 2. GLSL
// =============================================================================

#define GLSL_VERSION "#version 300 es\n"

// =============================================================================
// 2. BOWLING APP PATCHES
// These are the same patches used in the bowling game.
// =============================================================================

static constexpr YM2612Patch PATCH_00 =
{
    .ALG = 2, .FB = 5, .AMS = 0, .FMS = 0,
    .op = {
        { .DT = 1,  .MUL = 3, .TL = 38, .RS = 0, .AR = 12, .AM = 0, .DR = 7,  .SR = 11, .SL = 4, .RR = 6,  .SSG = 0 },
        { .DT = -1, .MUL = 1, .TL = 38, .RS = 0, .AR = 17, .AM = 0, .DR = 5,  .SR = 2,  .SL = 2, .RR = 1,  .SSG = 0 },
        { .DT = 1,  .MUL = 2, .TL = 5,  .RS = 0, .AR = 11, .AM = 0, .DR = 13, .SR = 11, .SL = 5, .RR = 13, .SSG = 0 },
        { .DT = -1, .MUL = 1, .TL = 0,  .RS = 0, .AR = 31, .AM = 0, .DR = 9,  .SR = 15, .SL = 5, .RR = 8,  .SSG = 3 }
    }
};

static constexpr YM2612Patch PATCH_01 =
{
    .ALG = 4, .FB = 6, .AMS = 0, .FMS = 0,
    .op = {
        { .DT = 0, .MUL = 3, .TL = 35, .RS = 0, .AR = 13, .AM = 0, .DR = 1,  .SR = 25, .SL = 2, .RR = 0, .SSG = 0 },
        { .DT = 0, .MUL = 1, .TL = 20, .RS = 0, .AR = 17, .AM = 0, .DR = 10, .SR = 8,  .SL = 2, .RR = 7, .SSG = 0 },
        { .DT = 0, .MUL = 1, .TL = 11, .RS = 0, .AR = 8,  .AM = 0, .DR = 4,  .SR = 23, .SL = 7, .RR = 1, .SSG = 0 },
        { .DT = 0, .MUL = 1, .TL = 14, .RS = 0, .AR = 25, .AM = 0, .DR = 0,  .SR = 10, .SL = 0, .RR = 9, .SSG = 0 }
    }
};

// =============================================================================
// 3. AURORA SHADERS
// =============================================================================

static const char* AURORA_VERT =
    GLSL_VERSION
    R"(
    precision highp float;
    layout(location = 0) in vec3 aPos;
    layout(location = 1) in vec2 aTexCoord;
    out vec2 TexCoord;
    void main() {
        TexCoord    = aTexCoord;
        gl_Position = vec4(aPos, 1.0);
    }
    )";

static const char* AURORA_FRAG =
    GLSL_VERSION
    R"(
    precision highp float;
    in  vec2 TexCoord;
    out vec4 FragColor;
    uniform float uYaw;
    uniform float uPitch;
    uniform float uTime;

    vec3 mod289v3(vec3 x) { return x - floor(x * (1.0/289.0)) * 289.0; }
    vec2 mod289v2(vec2 x) { return x - floor(x * (1.0/289.0)) * 289.0; }
    vec3 permute3(vec3 x) { return mod289v3(((x * 34.0) + 1.0) * x); }
    float snoise(vec2 v) {
        const vec4 C = vec4(0.211324865405187,  0.366025403784439,
                           -0.577350269189626,  0.024390243902439);
        vec2 i  = floor(v + dot(v, C.yy));
        vec2 x0 = v - i + dot(i, C.xx);
        vec2 i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
        vec4 x12 = x0.xyxy + C.xxzz;
        x12.xy -= i1;
        i = mod289v2(i);
        vec3 p = permute3(permute3(i.y + vec3(0.0,i1.y,1.0))
                          + i.x  + vec3(0.0,i1.x,1.0));
        vec3 m = max(0.5 - vec3(dot(x0,x0),
                                dot(x12.xy,x12.xy),
                                dot(x12.zw,x12.zw)), 0.0);
        m = m*m*m*m;
        vec3 x  = 2.0*fract(p*C.www) - 1.0;
        vec3 h  = abs(x) - 0.5;
        vec3 ox = floor(x + 0.5);
        vec3 a0 = x - ox;
        m *= 1.79284291400159 - 0.85373472095314*(a0*a0 + h*h);
        vec3 g;
        g.x  = a0.x *x0.x  + h.x *x0.y;
        g.yz = a0.yz*x12.xz + h.yz*x12.yw;
        return 130.0*dot(m,g);
    }
    void main() {
        float yawNorm     = uYaw / 3.14159;
        float yawOffset   = yawNorm * abs(yawNorm);
        float pitchNorm   = (uPitch + 1.5708) / 3.14159;
        float pitchOffset = (pitchNorm - 0.5) * 4.0;
        float timeOffset  = uTime * 0.0005;
        vec2 uv = TexCoord + vec2(yawOffset, pitchOffset + timeOffset);
        float n1 = snoise(uv * 3.0)  * 0.5;
        float n2 = snoise(uv * 7.0  + vec2(uTime*0.01, 0.0)) * 0.3;
        float n3 = snoise(uv * 15.0 + vec2(uTime*0.02, 0.0)) * 0.2;
        float intensity = clamp(n1+n2+n3, 0.0, 1.0);
        vec3 col1 = vec3(sin(TexCoord.x+TexCoord.y+uTime*0.001), 0.2, 0.3);
        vec3 col2 = vec3(0.9, sin(TexCoord.y+uTime*0.0005), 0.5);
        vec3 color = mix(col1, col2, intensity);
        FragColor = vec4(color, pitchNorm);
    }
    )";

// =============================================================================
// 4. SONG
//
// 5 channels, tick_rate=60, speed=6 -> 100ms/row, 64 rows = 6.4s loop
// No "org.tildearrow.furnace" header — parser handles this.
// Column format: note(3)+inst(2)+vol(2)+effect_dots(4) = 11 chars each.
//
// Instruments:
//   00 = PATCH_00  (ch0 bass/lead)
//   01 = PATCH_01  (ch2-4 chord voices)
//   02 = PATCH_HIHAT  (ch1 hi-hat, very quiet)
// =============================================================================

static const char* SONG =
"64\n"
"C-3007F....|C#30266....|C-3017F....|E-3017F....|G-3017F....\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#402......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#402......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#402......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"G-300......|C#302......|G-301......|B-301......|D-301......\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#402......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#402......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#302......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"A-300......|C#402......|A-301......|C-301......|E-301......\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#402......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#402......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#402......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"F-300......|C#302......|F-301......|A-301......|C-301......\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#402......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#402......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#402......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
;

// =============================================================================
// 4b. SONG 2 — drum/percussion loop
//
// 4 channels, tick_rate=60, speed=4 -> ~66ms/row, 16 rows = ~1.06s loop
// Instruments:
//   20 = PATCH_KICK
//   21 = PATCH_SNARE
//   22 = PATCH_HIHAT
//   23 = PATCH_CLANG
// =============================================================================

static const char* SONG2 =
"16\n"
"C-3207F....|...........|C-3227F....|...........\n"
"...........|...........|...........|...........\n"
"...........|C-3217F....|...........|C-3237F....\n"
"...........|...........|...........|...........\n"
"C-3207F....|...........|C-3227F....|...........\n"
"...........|...........|...........|...........\n"
"...........|C-3217F....|...........|...........\n"
"...........|...........|C-3227F....|...........\n"
"C-3207F....|...........|...........|C-3237F....\n"
"...........|...........|C-3227F....|...........\n"
"...........|C-3217F....|...........|...........\n"
"...........|...........|...........|...........\n"
"C-3207F....|...........|C-3227F....|...........\n"
"...........|...........|...........|...........\n"
"...........|C-3217F....|...........|C-3237F....\n"
"...........|...........|C-3227F....|...........\n"
;

static constexpr int SONG_ID_1 = 1;
static constexpr int SONG_ID_2 = 2;

// =============================================================================
// 5. SFX PATTERNS
// =============================================================================

// =============================================================================
// 5. SFX PATTERNS
// tick_rate=60, speed=3 → 50ms per row
// inst 00 = PATCH_00 (punchy bass/lead), inst 01 = PATCH_01 (bright lead)
// =============================================================================

// JUMP — rising three-note sweep, snappy
static const char* SFX_JUMP =
"6\n"
"C-4007F\n"
"E-4007F\n"
"G-4007F\n"
"C-5007F\n"
"OFF....\n"
".......\n";

// COIN — bright high two-note ping
static const char* SFX_COIN =
"5\n"
"E-5017F\n"
"A-5017F\n"
"E-6017F\n"
"OFF....\n"
".......\n";

// ALARM — urgent repeating two-tone pulse
static const char* SFX_ALARM =
"8\n"
"A-4007F\n"
"E-4007F\n"
"A-4007F\n"
"E-4007F\n"
"A-4007F\n"
"E-4007F\n"
"OFF....\n"
".......\n";

// FANFARE — triumphant ascending arpeggio with held final note
static const char* SFX_FANFARE =
"12\n"
"C-4017F\n"
"E-4017F\n"
"G-4017F\n"
"C-5017F\n"
"E-5017F\n"
"G-5017F\n"
"C-6017F\n"
".......\n"
".......\n"
".......\n"
"OFF....\n"
".......\n";

static constexpr int SFX_ID_JUMP    = 0;
static constexpr int SFX_ID_COIN    = 1;
static constexpr int SFX_ID_ALARM   = 2;
static constexpr int SFX_ID_FANFARE = 3;

// =============================================================================
// 6. GL HELPERS
// =============================================================================

static GLuint compileShader(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if(!ok) {
        char log[1024]; glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader error:\n%s\n", log); exit(1);
    }
    return s;
}

static GLuint createProgram(const char* vert, const char* frag)
{
    GLuint vs=compileShader(GL_VERTEX_SHADER,vert);
    GLuint fs=compileShader(GL_FRAGMENT_SHADER,frag);
    GLuint p=glCreateProgram();
    glAttachShader(p,vs); glAttachShader(p,fs);
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

// =============================================================================
// 7. AURORA RENDERER
// =============================================================================

struct AuroraRenderer
{
    GLuint program=0, vao=0;
    float  time=3.0f;

    void init() {
        program = createProgram(AURORA_VERT, AURORA_FRAG);
        static const GLfloat verts[] = {
            -1.f,-1.f,1.000f, 0.f,0.f,
             1.f,-1.f,0.998f, 1.f,0.f,
            -1.f, 1.f,0.998f, 0.f,1.f,
             1.f, 1.f,0.998f, 1.f,1.f,
        };
        static const GLuint idx[] = {0,1,2,1,3,2};
        GLuint vbo,ebo;
        glGenVertexArrays(1,&vao); glGenBuffers(1,&vbo); glGenBuffers(1,&ebo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER,vbo);
        glBufferData(GL_ARRAY_BUFFER,sizeof(verts),verts,GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(idx),idx,GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,5*sizeof(GLfloat),(void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,5*sizeof(GLfloat),(void*)(3*sizeof(GLfloat)));
        glBindVertexArray(0);
    }

    void render(float dt) {
        time += dt;
        float yaw   = sinf(time*0.07f)*1.2f;
        float pitch = sinf(time*0.04f)*0.8f-0.4f;
        glUseProgram(program);
        glUniform1f(glGetUniformLocation(program,"uTime"), time);
        glUniform1f(glGetUniformLocation(program,"uYaw"),  yaw);
        glUniform1f(glGetUniformLocation(program,"uPitch"),pitch);
        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,0);
        glBindVertexArray(0);
    }
};

// =============================================================================
// 8. IMGUI MODULE
// =============================================================================

struct ModImgui
{
    ImGuiContext* ctx=nullptr;
    void init(SDL_Window* w, SDL_GLContext g) {
        IMGUI_CHECKVERSION();
        ctx=ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        ImGui_ImplSDL2_InitForOpenGL(w,g);
        ImGui_ImplOpenGL3_Init("#version 300 es");
    }
    void shutdown() {
        ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext(ctx); ctx=nullptr;
    }
    void processEvent(const SDL_Event& e) const { ImGui_ImplSDL2_ProcessEvent(&e); }
    void newFrame() const {
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
    }
    void render() const {
        ImGui::Render(); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
};

// =============================================================================
// 9. SOUND SYSTEM
// =============================================================================

struct SfxInfo { int id; const char* name; const char* pattern; int tick; int speed; int dur; int pri; };
static const SfxInfo SFX_LIST[] = {
    { SFX_ID_JUMP,    "JUMP",    SFX_JUMP,    60, 3, 10, 4 },  // 6 rows + 4 tail
    { SFX_ID_COIN,    "COIN",    SFX_COIN,    60, 3,  9, 3 },  // 5 rows + 4 tail
    { SFX_ID_ALARM,   "ALARM",   SFX_ALARM,   60, 3, 12, 5 },  // 8 rows + 4 tail
    { SFX_ID_FANFARE, "FANFARE", SFX_FANFARE, 60, 3, 16, 6 },  // 12 rows + 4 tail
};
static constexpr int SFX_COUNT = 4;

struct SoundSystem
{
    IngameFMPlayer    player;
    SDL_AudioDeviceID dev     = 0;
    bool              running = false;
    bool              isLive  = true;
    std::string       lastError;

    // Which song is currently active
    int               currentSongId = SONG_ID_1;

    // Cached rate — survives teardown
    int               cachedRate = 0;

    // Per-song cache progress (main thread only)
    int               song1RowsDone = 0;
    int               song2RowsDone = 0;
    bool              song1Cached   = false;
    bool              song2Cached   = false;

    // Settings
    int              sampleRate   = 44100;
    int              bufferFrames = 256;
    IngameFMChipType chipType     = IngameFMChipType::YM3438;

    // ── Cache state queries ───────────────────────────────────────────────────
    bool isCapturePending()  const { return running && isLive && player.is_capture_pending(); }
    bool isCaching()         const { return running && isLive && player.is_capturing(); }
    bool isCaptureSession()  const { return running && isLive && player.is_capture_session(); }

    // Active recording progress (for the currently-recording song)
    int  captureRowsDone()  const { return player.capture_song_rows_done(); }
    bool captureComplete()  const { return player.is_song_captured(); }

    // SFX cache
    int  sfxRowsDone(int id)  const { return player.capture_sfx_rows_done(id); }
    int  sfxRowsTotal(int id) const { return player.capture_sfx_total_rows(id); }
    bool sfxCached(int id)    const {
        int t = sfxRowsTotal(id);
        return t > 0 && sfxRowsDone(id) >= t;
    }

    bool allCached() const {
        if(!song1Cached) return false;
        if(!song2Cached) return false;
        for(int i = 0; i < SFX_COUNT; i++)
            if(!sfxCached(SFX_LIST[i].id)) return false;
        return true;
    }

    void onCacheComplete() {
        if(allCached() && cachedRate == 0) cachedRate = sampleRate;
    }

    // Called every frame from main thread — syncs per-song capture progress
    void tickCacheProgress() {
        if(!running || !isLive) return;
        int  rows = captureRowsDone();
        bool done = captureComplete();
        if(currentSongId == SONG_ID_1) {
            song1RowsDone = rows;
            if(done) song1Cached = true;
        } else {
            song2RowsDone = rows;
            if(done) song2Cached = true;
        }
    }

    // ── Helpers ───────────────────────────────────────────────────────────────
    void loadPatches() {
        // Song 1 instruments
        player.add_patch(0x00, PATCH_00);
        player.add_patch(0x01, PATCH_01);
        player.add_patch(0x02, PATCH_HIHAT);
        // Song 2 instruments
        player.add_patch(0x20, PATCH_KICK);
        player.add_patch(0x21, PATCH_SNARE);
        player.add_patch(0x22, PATCH_HIHAT);
        player.add_patch(0x23, PATCH_CLANG);
        // SFX instruments
        player.add_patch(0x11, PATCH_HIHAT);
        player.add_patch(0x12, PATCH_CLANG);
        player.add_patch(0x14, PATCH_ELECTRIC_BASS);
        player.sfx_set_voices(3);
    }

    void defineSounds() {
        player.song_define(SONG_ID_1, SONG,  60, 6);
        player.song_define(SONG_ID_2, SONG2, 60, 4);
        for(int i = 0; i < SFX_COUNT; i++)
            player.sfx_define(SFX_LIST[i].id, SFX_LIST[i].pattern,
                              SFX_LIST[i].tick, SFX_LIST[i].speed);
    }

    bool openDevice() {
        int bf = bufferFrames < 256 ? 256 : bufferFrames;
        int p = 256; while(p < bf && p < 4096) p <<= 1; bf = p;
        SDL_AudioSpec desired{};
        desired.freq     = sampleRate;
        desired.format   = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples  = static_cast<Uint16>(bf);
        desired.callback = IngameFMPlayer::s_audio_callback;
        desired.userdata = &player;
        SDL_AudioSpec obtained{};
        dev = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        if(dev == 0) { lastError = std::string("SDL_OpenAudioDevice: ") + SDL_GetError(); return false; }
        if(obtained.freq != sampleRate)
            printf("[demo7] Warning: SDL gave %d Hz (requested %d)\n", obtained.freq, sampleRate);
        return true;
    }

    // ── Start live ────────────────────────────────────────────────────────────
    bool startLive() {
        if(running) teardown();
        lastError.clear();
        currentSongId = SONG_ID_1;
        // Preserve cache if any songs are already recorded — user must
        // explicitly clear it. keep_cache=true skips song_cache_/sfx_cache_ wipe.
        bool hasCachedData = song1Cached || song2Cached;
        player.reset(hasCachedData);
        player.set_sample_rate(sampleRate);
        player.set_chip_type(chipType);
        player.set_build_cache(false);
        player.set_play_cache(false);
        loadPatches();
        try { defineSounds(); }
        catch(const std::exception& e) { lastError = e.what(); return false; }
        if(!openDevice()) return false;
        player.song_select(SONG_ID_1, true);
        player.start(dev, true);
        SDL_PauseAudioDevice(dev, 0);
        running = true; isLive = true;
        printf("[demo7] Live: sr=%d buf=%d\n", sampleRate, bufferFrames);
        return true;
    }

    // ── Song switching (call from main thread — uses audio lock internally) ───
    void changeSong(int id, bool now) {
        if(!running || !isLive) return;
        SongChangeWhen when = now ? SongChangeWhen::NOW : SongChangeWhen::AT_PATTERN_END;
        SDL_LockAudioDevice(dev);
        player.change_song(id, when);
        SDL_UnlockAudioDevice(dev);
        currentSongId = id;
    }

    // ── Recording control ─────────────────────────────────────────────────────
    void startCapture() {
        if(!running || !isLive) return;
        SDL_LockAudioDevice(dev);
        player.request_capture();
        SDL_UnlockAudioDevice(dev);
        printf("[demo7] Capture requested (pending next loop)\n");
    }

    void cancelCapture() {
        if(!running) return;
        SDL_LockAudioDevice(dev);
        player.cancel_capture();
        SDL_UnlockAudioDevice(dev);
        printf("[demo7] Capture cancelled\n");
    }

    // ── Start cached ──────────────────────────────────────────────────────────
    bool startCached() {
        if(cachedRate == 0) { lastError = "No cache recorded yet."; return false; }
        if(sampleRate != cachedRate) {
            lastError = "Rate must be " + std::to_string(cachedRate) + " Hz to use cache.";
            return false;
        }
        if(running) {
            if(dev) { player.stop(dev); SDL_CloseAudioDevice(dev); dev=0; }
            running = false;
        }
        lastError.clear();
        player.set_play_cache(true);
        player.set_sample_rate(sampleRate);
        player.set_chip_type(chipType);
        if(!openDevice()) return false;
        player.song_select(SONG_ID_1, true);
        currentSongId = SONG_ID_1;
        player.start(dev, true);
        SDL_PauseAudioDevice(dev, 0);
        running = true; isLive = false;
        printf("[demo7] Cached: sr=%d buf=%d\n", sampleRate, bufferFrames);
        return true;
    }

    // ── Teardown ──────────────────────────────────────────────────────────────
    void teardown() {
        if(!running) return;
        if(dev) { player.stop(dev); SDL_CloseAudioDevice(dev); dev=0; }
        player.set_play_cache(false);
        player.cancel_capture();
        running = false;
        printf("[demo7] Teardown\n");
    }

    void sfx_play(int id, int priority, int duration) {
        if(!running || !dev) return;
        SDL_LockAudioDevice(dev);
        player.sfx_play(id, priority, duration);
        SDL_UnlockAudioDevice(dev);
    }
};

// =============================================================================
// 10. APPLICATION STATE
// =============================================================================

struct AppState
{
    SDL_Window*   window  = nullptr;
    SDL_GLContext glCtx   = nullptr;
    int           winW    = 720;
    int           winH    = 520;

    AuroraRenderer aurora;
    ModImgui       imgui;
    SoundSystem    sound;

    float musicVol = 1.0f;
    float sfxVol   = 1.0f;

    Uint32 lastTick    = 0;
    float  fpsSmooth   = 0.0f;
    // Display values — updated at most once per 500ms
    float  displayFps  = 0.0f;
    float  displayMs   = 0.0f;
    Uint32 lastDisplay = 0;
    bool   running   = true;

    // UI state
    int  selectedRate = 2;   // index into RATES[]
    int  selectedBuf  = 1;   // index into BUFS[]
    int  selectedChip = 0;   // index into CHIPS[]
    bool showError    = false;

    // Patch editors — one per song instrument
    OPNPatchEditor editors[7];    // [0]=PATCH_00 [1]=PATCH_01 [2]=PATCH_HIHAT  [3]=PATCH_KICK [4]=PATCH_SNARE [5]=PATCH_HIHAT2 [6]=PATCH_CLANG
    bool editorsInited = false;
};

static AppState g_app;

static const int  RATES[]     = { 11025, 22050, 44100, 48000 };
static const char* RATE_LBLS[]= { "11025 Hz", "22050 Hz", "44100 Hz", "48000 Hz" };
static const int  BUFS[]      = { 256, 512, 1024, 2048 };
static const char* BUF_LBLS[] = { "256", "512", "1024", "2048" };

// Cleanest first
static const IngameFMChipType CHIPS[]  = { IngameFMChipType::YM3438, IngameFMChipType::YM2612 };
static const char* CHIP_LBLS[]         = { "YM3438 — Clean", "YM2612 — Authentic Sega" };

// =============================================================================
// 11. PANEL
// =============================================================================

static void drawPanel(AppState& app)
{
    ImGuiIO& io = ImGui::GetIO();
    const float panelW = 400.f;
    const float panelH = (float)app.winH - 40.f;
    ImGui::SetNextWindowPos(
        ImVec2((io.DisplaySize.x-panelW)*0.5f,(io.DisplaySize.y-panelH)*0.5f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("ingamefm demo7", nullptr,
        ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|
        ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoScrollbar);

    SoundSystem& s   = app.sound;
    const bool running   = s.running;
    const bool isLive    = running && s.isLive;
    const bool isCached  = running && !s.isLive;
    const bool stopped   = !running;
    const bool capPend   = s.isCapturePending();
    const bool capActive = s.isCaching();
    const bool allCached = s.allCached();

    static const char* LFO_LBLS[] = {"Off","3.82 Hz","5.33 Hz","5.77 Hz",
                                      "6.11 Hz","6.60 Hz","9.23 Hz","46.1 Hz","69.2 Hz"};

    // =========================================================================
    // SECTION 1 — HARDWARE
    // Chip, sample rate, buffer size. Locked while engine is running.
    // =========================================================================
    ImGui::SeparatorText("Hardware");
    ImGui::BeginDisabled(running);
    {
        ImGui::Text("Chip"); ImGui::SameLine(); ImGui::SetNextItemWidth(-1.f);
        if(ImGui::BeginCombo("##chip", CHIP_LBLS[app.selectedChip])) {
            for(int i=0;i<2;i++) {
                bool sel=(i==app.selectedChip);
                if(ImGui::Selectable(CHIP_LBLS[i],sel)) app.selectedChip=i;
                if(sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::Text("Sample rate"); ImGui::SameLine(); ImGui::SetNextItemWidth(110.f);
        if(ImGui::BeginCombo("##rate", RATE_LBLS[app.selectedRate])) {
            for(int i=0;i<4;i++) {
                bool sel=(i==app.selectedRate);
                if(ImGui::Selectable(RATE_LBLS[i],sel)) app.selectedRate=i;
                if(sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::Text("Buffer"); ImGui::SameLine(); ImGui::SetNextItemWidth(-1.f);
        if(ImGui::BeginCombo("##buf", BUF_LBLS[app.selectedBuf])) {
            for(int i=0;i<4;i++) {
                bool sel=(i==app.selectedBuf);
                if(ImGui::Selectable(BUF_LBLS[i],sel)) app.selectedBuf=i;
                if(sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::EndDisabled();

    // =========================================================================
    // SECTION 2 — PLAYBACK MODE
    // Shows current engine state; start/stop controls.
    // =========================================================================
    ImGui::SeparatorText("Playback");
    ImGui::Spacing();

    if(stopped) {
        // ── Stopped ──────────────────────────────────────────────────────────
        ImGui::TextDisabled("Engine stopped.");
        ImGui::Spacing();

        // Live Synthesis
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f,0.35f,0.15f,1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f,0.5f, 0.25f,1.f));
        if(ImGui::Button("Start — Live Synthesis", ImVec2(-1,0))) {
            s.sampleRate   = RATES[app.selectedRate];
            s.bufferFrames = BUFS[app.selectedBuf];
            s.chipType     = CHIPS[app.selectedChip];
            app.showError  = false;
            if(s.startLive()) {
                app.musicVol=1.f; app.sfxVol=1.f;
                s.player.set_music_volume(1.f); s.player.set_sfx_volume(1.f);
            } else app.showError=true;
        }
        ImGui::PopStyleColor(2);

        // Cached Playback — only when cache exists
        {
            ImGui::BeginDisabled(!allCached);
            char lbl[64];
            if(allCached && s.cachedRate>0)
                snprintf(lbl,sizeof(lbl),"Start — Cached Playback  (%d Hz)", s.cachedRate);
            else
                snprintf(lbl,sizeof(lbl),"Start — Cached Playback  (no cache)");
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f,0.25f,0.4f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f,0.4f, 0.6f,1.f));
            if(ImGui::Button(lbl, ImVec2(-1,0))) {
                s.sampleRate   = s.cachedRate;
                s.bufferFrames = BUFS[app.selectedBuf];
                s.chipType     = CHIPS[app.selectedChip];
                app.showError  = false;
                for(int i=0;i<4;i++) if(RATES[i]==s.cachedRate){app.selectedRate=i;break;}
                if(!s.startCached()) app.showError=true;
                else {
                    app.musicVol=1.f; app.sfxVol=1.f;
                    s.player.set_music_volume(1.f); s.player.set_sfx_volume(1.f);
                }
            }
            ImGui::PopStyleColor(2);
            ImGui::EndDisabled();
            if(allCached && s.cachedRate>0 && RATES[app.selectedRate]!=s.cachedRate) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f,0.8f,0.2f,1.f));
                ImGui::TextWrapped("  Cache recorded at %d Hz — rate will be forced.", s.cachedRate);
                ImGui::PopStyleColor();
            }
        }

        if(app.showError && !s.lastError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f,0.3f,0.3f,1.f));
            ImGui::TextWrapped("%s", s.lastError.c_str());
            ImGui::PopStyleColor();
        }

    } else {
        // ── Running ───────────────────────────────────────────────────────────
        // Mode badge
        if(isLive) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f,0.75f,0.2f,1.f));
            ImGui::Text("●  LIVE SYNTHESIS");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f,0.9f,0.4f,1.f));
            ImGui::Text("●  CACHED PLAYBACK");
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s  %d Hz  buf %d  row %d/%d",
            CHIP_LBLS[app.selectedChip],
            s.sampleRate, s.bufferFrames,
            s.player.get_current_row(), s.player.get_song_length());
        ImGui::Spacing();

        // Stop button
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.4f,0.08f,0.08f,1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f,0.12f,0.12f,1.f));
        if(ImGui::Button("Stop Engine", ImVec2(-1,0))) s.teardown();
        ImGui::PopStyleColor(2);
    }

    // =========================================================================
    // SECTION 3 — SONGS
    // Song switching + per-song cache progress.
    // =========================================================================
    ImGui::SeparatorText("Songs");
    {
        char overlay[72];

        // Song 1
        {
            bool isCur  = (s.currentSongId == SONG_ID_1);
            int  done   = isCur ? s.captureRowsDone() : s.song1RowsDone;
            bool cached = s.song1Cached;
            if(cached) done = 64;
            float frac = (float)done / 64.f;
            if(frac > 1.f) frac = 1.f;
            const char* status = cached              ? "cached"
                               : (capActive && isCur) ? "recording..."
                               : (capPend   && isCur) ? "waiting..."
                               : "";
            snprintf(overlay, sizeof(overlay), "Song 1  %d/64  %s", done, status);
            ImVec4 col = cached                      ? ImVec4(0.2f,0.7f,0.2f,1.f)
                       : (capPend   && isCur)        ? ImVec4(0.35f,0.35f,0.35f,1.f)
                       : (capActive && isCur)        ? ImVec4(0.7f,0.5f,0.1f,1.f)
                       :                               ImVec4(0.45f,0.4f,0.1f,1.f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
            ImGui::ProgressBar(frac, ImVec2(-1,0), overlay);
            ImGui::PopStyleColor();
        }
        // Song 2
        {
            bool isCur  = (s.currentSongId == SONG_ID_2);
            int  done   = isCur ? s.captureRowsDone() : s.song2RowsDone;
            bool cached = s.song2Cached;
            if(cached) done = 16;
            float frac = (float)done / 16.f;
            if(frac > 1.f) frac = 1.f;
            const char* status = cached              ? "cached"
                               : (capActive && isCur) ? "recording..."
                               : (capPend   && isCur) ? "waiting..."
                               : "";
            snprintf(overlay, sizeof(overlay), "Song 2  %d/16  %s", done, status);
            ImVec4 col = cached                      ? ImVec4(0.2f,0.7f,0.2f,1.f)
                       : (capPend   && isCur)        ? ImVec4(0.35f,0.35f,0.35f,1.f)
                       : (capActive && isCur)        ? ImVec4(0.7f,0.5f,0.1f,1.f)
                       :                               ImVec4(0.45f,0.4f,0.1f,1.f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
            ImGui::ProgressBar(frac, ImVec2(-1,0), overlay);
            ImGui::PopStyleColor();
        }

        // Song switching — only in live mode
        if(isLive) {
            ImGui::Spacing();
            // Show which song is current
            if(s.currentSongId == SONG_ID_1) {
                ImGui::TextDisabled("Playing: Song 1");
            } else {
                ImGui::TextDisabled("Playing: Song 2");
            }
            // Switch buttons — one row each for the other song
            float hw = (panelW - 40.f) * 0.5f;
            if(s.currentSongId != SONG_ID_1) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.2f,0.3f,0.45f,1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f,0.45f,0.65f,1.f));
                if(ImGui::Button("Song 1 — Now",         ImVec2(hw,0))) s.changeSong(SONG_ID_1, true);
                ImGui::SameLine();
                if(ImGui::Button("Song 1 — At Loop End", ImVec2(-1,0))) s.changeSong(SONG_ID_1, false);
                ImGui::PopStyleColor(2);
            }
            if(s.currentSongId != SONG_ID_2) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.2f,0.3f,0.45f,1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f,0.45f,0.65f,1.f));
                if(ImGui::Button("Song 2 — Now",         ImVec2(hw,0))) s.changeSong(SONG_ID_2, true);
                ImGui::SameLine();
                if(ImGui::Button("Song 2 — At Loop End", ImVec2(-1,0))) s.changeSong(SONG_ID_2, false);
                ImGui::PopStyleColor(2);
            }
        }
    }

    // =========================================================================
    // SECTION 4 — CACHE
    // SFX bars + record controls.
    // =========================================================================
    ImGui::SeparatorText("Cache");
    {
        char overlay[72];
        bool sessionOn = s.isCaptureSession();

        // SFX bars
        for(int i = 0; i < SFX_COUNT; i++) {
            const SfxInfo& info = SFX_LIST[i];
            int done=s.sfxRowsDone(info.id), total=s.sfxRowsTotal(info.id);
            bool ok=s.sfxCached(info.id);
            float frac=(total>0)?(float)done/total:0.f;
            const char* status=ok?"done":sessionOn?"trigger to record":capPend?"waiting...":"";
            snprintf(overlay,sizeof(overlay),"%-8s  %d/%d  %s",info.name,done,total>0?total:0,status);
            ImVec4 col=ok?ImVec4(0.2f,0.7f,0.2f,1.f):(capPend||(sessionOn&&!ok))?ImVec4(0.35f,0.35f,0.35f,1.f):ImVec4(0.45f,0.4f,0.1f,1.f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,col);
            ImGui::ProgressBar(frac,ImVec2(-1,0),overlay);
            ImGui::PopStyleColor();
        }
        if(allCached && s.cachedRate>0)
            ImGui::TextColored(ImVec4(0.3f,1.f,0.3f,1.f),"Cache complete  —  %d Hz", s.cachedRate);
        else if(!allCached && !sessionOn && !capPend)
            ImGui::TextDisabled("No cache recorded");
    }

    // Record controls — only in Live mode
    if(isLive) {
        ImGui::Spacing();
        bool curCached = (s.currentSongId == SONG_ID_1) ? s.song1Cached : s.song2Cached;
        if(!curCached) {
            bool isPending = s.isCapturePending();
            if(!capActive && !isPending) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.45f,0.15f,0.0f,1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f,0.28f,0.0f,1.f));
                if(ImGui::Button("Record Current Song + SFX", ImVec2(-1,0))) s.startCapture();
                ImGui::PopStyleColor(2);
            } else {
                const char* lbl = isPending ? "Stop Recording  (waiting for next loop...)" : "Stop Recording";
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.5f,0.05f,0.05f,1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f,0.1f, 0.1f,1.f));
                if(ImGui::Button(lbl, ImVec2(-1,0))) s.cancelCapture();
                ImGui::PopStyleColor(2);
            }
        } else {
            // Current song is cached — offer to clear it so user can re-record
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f,0.22f,0.22f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f,0.35f,0.35f,1.f));
            if(ImGui::Button("Clear Cache for Current Song", ImVec2(-1,0))) {
                if(s.currentSongId == SONG_ID_1) { s.song1Cached = false; s.song1RowsDone = 0; }
                else                              { s.song2Cached = false; s.song2RowsDone = 0; }
                // Cache rate is no longer valid since not all songs are cached
                s.cachedRate = 0;
                // Clear the audio data for this song from the player
                SDL_LockAudioDevice(s.dev);
                s.player.cancel_capture();
                SDL_UnlockAudioDevice(s.dev);
            }
            ImGui::PopStyleColor(2);
        }
        if(!allCached)
            ImGui::TextDisabled("Switch songs to record each one.");
    }

    // =========================================================================
    // SECTION 5 — MIXER  (only while running)
    // Volume sliders + LFO for both chips.
    // =========================================================================
    if(running) {
        ImGui::SeparatorText("Mixer");

        // Volume
        ImGui::Text("Volume"); ImGui::SameLine();
        int mv=(int)(app.musicVol*100.f+0.5f);
        ImGui::SetNextItemWidth(130.f);
        if(ImGui::SliderInt("Song##vol",&mv,0,100,"%d%%")) {
            app.musicVol=mv/100.f; s.player.set_music_volume(app.musicVol);
        }
        ImGui::SameLine();
        int sv=(int)(app.sfxVol*100.f+0.5f);
        ImGui::SetNextItemWidth(130.f);
        if(ImGui::SliderInt("SFX##vol",&sv,0,100,"%d%%")) {
            app.sfxVol=sv/100.f; s.player.set_sfx_volume(app.sfxVol);
        }

        // LFO
        ImGui::Text("LFO"); ImGui::SameLine();
        {
            bool en=s.player.get_music_lfo_enable();
            int  fr=en?s.player.get_music_lfo_freq()+1:0;
            ImGui::Text("Song"); ImGui::SameLine(); ImGui::SetNextItemWidth(90.f);
            if(ImGui::BeginCombo("##mlfo",LFO_LBLS[fr])) {
                for(int i=0;i<9;i++){bool sel=(i==fr);if(ImGui::Selectable(LFO_LBLS[i],sel)){SDL_LockAudioDevice(s.dev);s.player.set_music_lfo(i>0,i>0?i-1:0);SDL_UnlockAudioDevice(s.dev);}if(sel)ImGui::SetItemDefaultFocus();}
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine();
        {
            bool en=s.player.get_sfx_lfo_enable();
            int  fr=en?s.player.get_sfx_lfo_freq()+1:0;
            ImGui::Text("SFX"); ImGui::SameLine(); ImGui::SetNextItemWidth(90.f);
            if(ImGui::BeginCombo("##slfo",LFO_LBLS[fr])) {
                for(int i=0;i<9;i++){bool sel=(i==fr);if(ImGui::Selectable(LFO_LBLS[i],sel)){SDL_LockAudioDevice(s.dev);s.player.set_sfx_lfo(i>0,i>0?i-1:0);SDL_UnlockAudioDevice(s.dev);}if(sel)ImGui::SetItemDefaultFocus();}
                ImGui::EndCombo();
            }
        }

        // =========================================================================
        // SECTION 6 — SOUND EFFECTS  (only while running)
        // =========================================================================
        ImGui::SeparatorText("Sound Effects");
        struct SfxBtn{int id;int pri;int dur;const char* label;ImVec4 col;};
        static const SfxBtn btns[]={
            {SFX_ID_JUMP,   4,10,"[q] JUMP",   ImVec4(0.2f,0.5f,0.8f,1.f)},
            {SFX_ID_COIN,   3, 9,"[w] COIN",   ImVec4(0.8f,0.7f,0.1f,1.f)},
            {SFX_ID_ALARM,  5,12,"[e] ALARM",  ImVec4(0.8f,0.4f,0.1f,1.f)},
            {SFX_ID_FANFARE,6,16,"[r] FANFARE",ImVec4(0.6f,0.1f,0.6f,1.f)},
        };
        const float bw=(panelW-40.f)/2.f;
        for(int i=0;i<4;i++){
            if(i%2!=0)ImGui::SameLine();
            const SfxBtn& b=btns[i];
            ImVec4 dim(b.col.x*.35f,b.col.y*.35f,b.col.z*.35f,.9f);
            ImVec4 hov(b.col.x*.6f, b.col.y*.6f, b.col.z*.6f,1.f);
            ImGui::PushStyleColor(ImGuiCol_Button,dim);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,b.col);
            if(ImGui::Button(b.label,ImVec2(bw,36.f)))s.sfx_play(b.id,b.pri,b.dur);
            ImGui::PopStyleColor(3);
        }
    }

    // =========================================================================
    // SECTION 7 — INSTRUMENTS  (hidden in Cached Playback mode)
    // Patch editors open as floating windows.
    // =========================================================================
    bool canEdit = stopped || isLive;
    if(!canEdit) app.editorsInited = false;
    if(canEdit) {
        // Patch name, patch ID, initial patch data
        static const char* PATCH_NAMES[] = {
            "PATCH_00", "PATCH_01", "PATCH_HIHAT",          // Song 1
            "PATCH_KICK","PATCH_SNARE","PATCH_HIHAT","PATCH_CLANG" // Song 2
        };
        static const int PATCH_IDS[] = {
            0x00, 0x01, 0x02,
            0x20, 0x21, 0x22, 0x23
        };
        if(!app.editorsInited) {
            app.editors[0].init("PATCH_00",   PATCH_00,   false,0,0);
            app.editors[1].init("PATCH_01",   PATCH_01,   false,0,0);
            app.editors[2].init("PATCH_HIHAT",PATCH_HIHAT,false,0,0);
            app.editors[3].init("PATCH_KICK", PATCH_KICK, false,0,0);
            app.editors[4].init("PATCH_SNARE",PATCH_SNARE,false,0,0);
            app.editors[5].init("PATCH_HIHAT",PATCH_HIHAT,false,0,0);
            app.editors[6].init("PATCH_CLANG",PATCH_CLANG,false,0,0);
            app.editorsInited=true;
        }
        ImGui::SeparatorText("Instruments");
        // Song 1 row — 3 buttons
        ImGui::TextDisabled("Song 1:");
        float edBtnW3 = (panelW-40.f)/3.f;
        for(int i=0;i<3;i++){
            if(i>0)ImGui::SameLine();
            OPNPatchEditor& ed=app.editors[i];
            ImVec4 c=ed.open?ImVec4(0.3f,0.5f,0.7f,1.f):ImVec4(0.18f,0.28f,0.38f,1.f);
            ImGui::PushStyleColor(ImGuiCol_Button,c);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.4f,0.6f,0.8f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f,0.7f,0.9f,1.f));
            char lbl[32]; snprintf(lbl,sizeof(lbl),"%s##ed%d",PATCH_NAMES[i],i);
            if(ImGui::Button(lbl,ImVec2(edBtnW3,0)))ed.open=!ed.open;
            ImGui::PopStyleColor(3);
        }
        // Song 2 row — 4 buttons
        ImGui::TextDisabled("Song 2:");
        float edBtnW4 = (panelW-40.f)/4.f;
        for(int i=3;i<7;i++){
            if(i>3)ImGui::SameLine();
            OPNPatchEditor& ed=app.editors[i];
            ImVec4 c=ed.open?ImVec4(0.3f,0.5f,0.7f,1.f):ImVec4(0.18f,0.28f,0.38f,1.f);
            ImGui::PushStyleColor(ImGuiCol_Button,c);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.4f,0.6f,0.8f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f,0.7f,0.9f,1.f));
            char lbl[32]; snprintf(lbl,sizeof(lbl),"%s##ed%d",PATCH_NAMES[i],i);
            if(ImGui::Button(lbl,ImVec2(edBtnW4,0)))ed.open=!ed.open;
            ImGui::PopStyleColor(3);
        }
        (void)PATCH_IDS; // used in popup section below
    }

    // ── Footer ────────────────────────────────────────────────────────────────
    ImGui::Separator();
    ImGui::TextDisabled("%.0f fps  %.2f ms", app.displayFps, app.displayMs);
    ImGui::End();
    // ── Patch editor popup windows (drawn outside main panel) ─────────────────
    if(canEdit) {
        static const char* PATCH_NAMES[] = {
            "PATCH_00", "PATCH_01", "PATCH_HIHAT",
            "PATCH_KICK","PATCH_SNARE","PATCH_HIHAT","PATCH_CLANG"
        };
        static const int PATCH_IDS[] = {
            0x00, 0x01, 0x02,
            0x20, 0x21, 0x22, 0x23
        };
        for(int i=0;i<7;i++) {
            OPNPatchEditor& ed = app.editors[i];
            char wndTitle[64]; snprintf(wndTitle, sizeof(wndTitle), "OPN Editor — %s###opned%d", PATCH_NAMES[i], i);
            if(ed.drawWindow(wndTitle, ImVec2(500, 640))) {
                if(s.running && s.isLive) {
                    SDL_LockAudioDevice(s.dev);
                    s.player.add_patch(PATCH_IDS[i], ed.patch, ed.block, ed.lfoEnable, (uint8_t)ed.lfoFreq);
                    SDL_UnlockAudioDevice(s.dev);
                }
            }
        }
    }
}
// =============================================================================
// 12. MAIN TICK
// =============================================================================

static void mainTick()
{
    AppState& app = g_app;

    Uint32 now = SDL_GetTicks();
    float  dt  = (app.lastTick==0) ? 0.016f : (now-app.lastTick)*0.001f;
    app.lastTick = now;
    if(dt>0.1f) dt=0.1f;
    float fps = (dt>0.0001f) ? (1.f/dt) : 9999.f;
    app.fpsSmooth = (app.fpsSmooth<1.f) ? fps : app.fpsSmooth*0.9f+fps*0.1f;
    if(now - app.lastDisplay >= 500) {
        app.displayFps  = app.fpsSmooth;
        app.displayMs   = (app.fpsSmooth > 0.f) ? (1000.f/app.fpsSmooth) : 0.f;
        app.lastDisplay = now;
    }

    // Sync per-song cache progress and check completion
    app.sound.tickCacheProgress();
    if(app.sound.allCached()) app.sound.onCacheComplete();

    SDL_Event e;
    while(SDL_PollEvent(&e)) {
        app.imgui.processEvent(e);
        if(e.type==SDL_QUIT) {
            app.running=false;
#ifdef __EMSCRIPTEN__
            emscripten_cancel_main_loop();
#endif
        }
        if(e.type==SDL_KEYDOWN && !e.key.repeat) {
            switch(e.key.keysym.sym) {
                case SDLK_ESCAPE:
                    app.running=false;
#ifdef __EMSCRIPTEN__
                    emscripten_cancel_main_loop();
#endif
                    break;
                case SDLK_q: app.sound.sfx_play(SFX_ID_JUMP,    4, 10); break;
                case SDLK_w: app.sound.sfx_play(SFX_ID_COIN,    3,  8); break;
                case SDLK_e: app.sound.sfx_play(SFX_ID_ALARM,   5, 12); break;
                case SDLK_r: app.sound.sfx_play(SFX_ID_FANFARE, 6, 20); break;
                default: break;
            }
        }
        if(e.type==SDL_WINDOWEVENT &&
           e.window.event==SDL_WINDOWEVENT_RESIZED) {
            app.winW=e.window.data1; app.winH=e.window.data2;
            glViewport(0,0,app.winW,app.winH);
        }
    }

    glClearColor(0.f,0.f,0.05f,1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    app.aurora.render(dt);

    app.imgui.newFrame();
    drawPanel(app);
    app.imgui.render();

    SDL_GL_SwapWindow(app.window);
}

// =============================================================================
// 13. MAIN
// =============================================================================

int main(int /*argc*/, char** /*argv*/)
{
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "1");

    AppState& app = g_app;
    app.window = SDL_CreateWindow(
        "ingamefm demo7 — configurable audio",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        app.winW, app.winH,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    if(!app.window) { fprintf(stderr,"SDL_CreateWindow: %s\n",SDL_GetError()); return 1; }

    app.glCtx = SDL_GL_CreateContext(app.window);
    if(!app.glCtx) { fprintf(stderr,"SDL_GL_CreateContext: %s\n",SDL_GetError()); return 1; }

    if(SDL_GL_SetSwapInterval(-1)!=0)
        if(SDL_GL_SetSwapInterval(1)!=0)
            SDL_GL_SetSwapInterval(0);

    glViewport(0,0,app.winW,app.winH);

    app.aurora.init();
    app.imgui.init(app.window, app.glCtx);

    printf("=== ingamefm demo7 ===\n");
    printf("Click 'Init Sound System' to start audio.\n");
    printf("Keys: q=JUMP  w=COIN  e=ALARM  r=FANFARE  Esc=quit\n\n");

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(mainTick, 0, 1);
#else
    while(app.running) mainTick();
    app.sound.teardown();
    app.imgui.shutdown();
    SDL_GL_DeleteContext(app.glCtx);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
#endif

    return 0;
}

// =============================================================================
// BUILD COMMANDS
// =============================================================================
//
// IMGUI=../imgui
// YMFM=../my-ym2612-plugin/build/_deps/ymfm-src/src
// SDL_INC=../bowling/3rdparty/SDL/include
// SDL_LIB=../bowling/build/macos/usr/lib/libSDL2.a
//
// ── NATIVE (macOS) ────────────────────────────────────────────────────────────
//
//   g++ -std=c++17 -O2 \
//       -I../bowling/build/macos/sdl2/include/ -I$SDL_INC \
//       -I$YMFM -I$IMGUI \
//       $SDL_LIB \
//       $YMFM/ymfm_misc.cpp $YMFM/ymfm_adpcm.cpp \
//       $YMFM/ymfm_ssg.cpp  $YMFM/ymfm_opn.cpp \
//       $IMGUI/imgui.cpp $IMGUI/imgui_draw.cpp \
//       $IMGUI/imgui_tables.cpp $IMGUI/imgui_widgets.cpp \
//       $IMGUI/backends/imgui_impl_sdl2.cpp \
//       $IMGUI/backends/imgui_impl_opengl3.cpp \
//       demo7.cpp \
//       -framework Cocoa -framework IOKit -framework CoreVideo \
//       -framework CoreAudio -framework AudioToolbox \
//       -framework ForceFeedback -framework Carbon \
//       -framework Metal -framework GameController -framework CoreHaptics \
//       -lobjc -o demo7 && ./demo7
//
// ── EMSCRIPTEN ────────────────────────────────────────────────────────────────
//
//   em++ -std=c++17 -O2 \
//       -I$YMFM -I$IMGUI \
//       $YMFM/ymfm_misc.cpp $YMFM/ymfm_adpcm.cpp \
//       $YMFM/ymfm_ssg.cpp  $YMFM/ymfm_opn.cpp \
//       $IMGUI/imgui.cpp $IMGUI/imgui_draw.cpp \
//       $IMGUI/imgui_tables.cpp $IMGUI/imgui_widgets.cpp \
//       $IMGUI/backends/imgui_impl_sdl2.cpp \
//       $IMGUI/backends/imgui_impl_opengl3.cpp \
//       demo7.cpp \
//       -s USE_SDL=2 -s FULL_ES3=1 \
//       -s MIN_WEBGL_VERSION=2 -s MAX_WEBGL_VERSION=2 \
//       -s ALLOW_MEMORY_GROWTH=1 -s ASYNCIFY \
//       --shell-file shell4.html \
//       -o demo7.html
//
//   python3 -m http.server  →  http://localhost:8000/demo7.html
// =============================================================================
