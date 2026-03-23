// =============================================================================
// xfm_demo.cpp — eggsfm: xfm API demo (SDL + Aurora shader)
// =============================================================================

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
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
#include <algorithm>

#include "xfm_api.h"
#include "xfm_opn_editor.h"

// =============================================================================
// INSTRUMENT PATCHES (from demo7)
// =============================================================================

static constexpr xfm_patch_opn PATCH_00 =
{
    .ALG = 2, .FB = 5, .AMS = 0, .FMS = 0,
    .op = {
        { .DT = 1,  .MUL = 3, .TL = 38, .RS = 0, .AR = 12, .AM = 0, .DR = 7,  .SR = 11, .SL = 4, .RR = 6,  .SSG = 0 },
        { .DT = -1, .MUL = 1, .TL = 38, .RS = 0, .AR = 17, .AM = 0, .DR = 5,  .SR = 2,  .SL = 2, .RR = 1,  .SSG = 0 },
        { .DT = 1,  .MUL = 2, .TL = 5,  .RS = 0, .AR = 11, .AM = 0, .DR = 13, .SR = 11, .SL = 5, .RR = 13, .SSG = 0 },
        { .DT = -1, .MUL = 1, .TL = 0,  .RS = 0, .AR = 31, .AM = 0, .DR = 9,  .SR = 15, .SL = 5, .RR = 8,  .SSG = 3 }
    }
};

static constexpr xfm_patch_opn PATCH_01 =
{
    .ALG = 4, .FB = 6, .AMS = 0, .FMS = 0,
    .op = {
        { .DT = 0, .MUL = 3, .TL = 35, .RS = 0, .AR = 13, .AM = 0, .DR = 1,  .SR = 25, .SL = 2, .RR = 0, .SSG = 0 },
        { .DT = 0, .MUL = 1, .TL = 20, .RS = 0, .AR = 17, .AM = 0, .DR = 10, .SR = 8,  .SL = 2, .RR = 7, .SSG = 0 },
        { .DT = 0, .MUL = 1, .TL = 11, .RS = 0, .AR = 8,  .AM = 0, .DR = 4,  .SR = 23, .SL = 7, .RR = 1, .SSG = 0 },
        { .DT = 0, .MUL = 1, .TL = 14, .RS = 0, .AR = 25, .AM = 0, .DR = 0,  .SR = 10, .SL = 0, .RR = 9, .SSG = 0 }
    }
};

static constexpr xfm_patch_opn PATCH_HIHAT =
{
    .ALG = 7, .FB = 7, .AMS = 0, .FMS = 0,
    .op = {
        { .DT = 3, .MUL = 13, .TL =  8, .RS = 3, .AR = 31, .AM = 0, .DR = 31, .SR = 0, .SL = 15, .RR = 15, .SSG = 0 },
        { .DT = 2, .MUL = 11, .TL = 12, .RS = 3, .AR = 31, .AM = 0, .DR = 31, .SR = 0, .SL = 15, .RR = 15, .SSG = 0 },
        { .DT = 1, .MUL =  7, .TL = 16, .RS = 3, .AR = 31, .AM = 0, .DR = 30, .SR = 0, .SL = 15, .RR = 14, .SSG = 0 },
        { .DT = 0, .MUL = 15, .TL = 20, .RS = 3, .AR = 31, .AM = 0, .DR = 29, .SR = 0, .SL = 15, .RR = 13, .SSG = 0 }
    }
};

static constexpr xfm_patch_opn PATCH_KICK =
{
    .ALG = 0, .FB = 7, .AMS = 0, .FMS = 0,
    .op = {
        { .DT = 0, .MUL = 1, .TL =  0, .RS = 3, .AR = 31, .AM = 0, .DR = 31, .SR = 0, .SL = 15, .RR = 15, .SSG = 0 },
        { .DT = 0, .MUL = 1, .TL = 16, .RS = 2, .AR = 31, .AM = 0, .DR = 20, .SR = 0, .SL = 15, .RR = 10, .SSG = 0 },
        { .DT = 0, .MUL = 1, .TL = 20, .RS = 1, .AR = 31, .AM = 0, .DR = 18, .SR = 0, .SL = 15, .RR =  8, .SSG = 0 },
        { .DT = 0, .MUL = 1, .TL =  0, .RS = 0, .AR = 31, .AM = 0, .DR = 14, .SR = 0, .SL = 15, .RR =  8, .SSG = 0 }
    }
};

static constexpr xfm_patch_opn PATCH_SNARE =
{
    .ALG = 4, .FB = 7, .AMS = 0, .FMS = 0,
    .op = {
        { .DT = 3, .MUL = 15, .TL =  0, .RS = 3, .AR = 31, .AM = 0, .DR = 31, .SR = 0, .SL = 15, .RR = 15, .SSG = 0 },
        { .DT = 0, .MUL =  3, .TL =  0, .RS = 2, .AR = 31, .AM = 0, .DR = 28, .SR = 0, .SL = 15, .RR = 14, .SSG = 0 },
        { .DT = 2, .MUL =  7, .TL = 10, .RS = 1, .AR = 31, .AM = 0, .DR = 24, .SR = 0, .SL = 15, .RR = 12, .SSG = 0 },
        { .DT = 0, .MUL =  1, .TL =  2, .RS = 0, .AR = 31, .AM = 0, .DR = 22, .SR = 0, .SL = 15, .RR = 10, .SSG = 0 }
    }
};

static constexpr xfm_patch_opn PATCH_CLANG =
{
    .ALG = 3, .FB = 6, .AMS = 0, .FMS = 0,
    .op = {
        { .DT = 3, .MUL = 11, .TL =  0, .RS = 3, .AR = 31, .AM = 0, .DR = 25, .SR = 0, .SL = 15, .RR = 12, .SSG = 0 },
        { .DT = -2, .MUL = 7, .TL =  8, .RS = 2, .AR = 31, .AM = 0, .DR = 22, .SR = 0, .SL = 15, .RR = 10, .SSG = 0 },
        { .DT = 2, .MUL = 13, .TL = 12, .RS = 2, .AR = 31, .AM = 0, .DR = 20, .SR = 0, .SL = 15, .RR =  9, .SSG = 0 },
        { .DT = 0, .MUL =  1, .TL =  0, .RS = 0, .AR = 31, .AM = 0, .DR = 18, .SR = 0, .SL = 15, .RR =  8, .SSG = 0 }
    }
};

// =============================================================================
// SFX PATTERNS (from demo7)
// tick_rate=60, speed=3 → 50ms per row
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
// SONGS (from demo7)
// =============================================================================

// Song 1: 5 channels, tick_rate=60, speed=6 -> 100ms/row, 64 rows = 6.4s loop
// Instruments: 00=PATCH_00 (ch0), 01=PATCH_01 (ch2-4), 02=PATCH_HIHAT (ch1)
static const char* SONG_1 =
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

// Song 2: 4 channels, tick_rate=60, speed=4 -> ~66ms/row, 16 rows = ~1.06s loop
// Instruments: 20=KICK, 21=SNARE, 22=HIHAT, 23=CLANG
static const char* SONG_2 =
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

// Piano instrument list — name + patch id
struct PianoInstr { const char* name; int patchId; };
static const PianoInstr PIANO_INSTRS[] = {
    { "PATCH_00",   0x00 },
    { "PATCH_01",   0x01 },
    { "PATCH_HIHAT",0x02 },
    { "PATCH_KICK", 0x20 },
    { "PATCH_SNARE",0x21 },
    { "PATCH_CLANG",0x23 },
};

// =============================================================================
// GLSL
// =============================================================================

#define GLSL_VERSION "#version 300 es\n"

// =============================================================================
// AURORA SHADERS
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
// GL HELPERS
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
    GLuint vs = compileShader(GL_VERTEX_SHADER, vert);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, frag);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

// =============================================================================
// AURORA RENDERER
// =============================================================================

struct AuroraRenderer
{
    GLuint program = 0, vao = 0;
    float  time    = 3.0f;

    void init() {
        program = createProgram(AURORA_VERT, AURORA_FRAG);
        static const GLfloat verts[] = {
            -1.f, -1.f, 1.000f, 0.f, 0.f,
             1.f, -1.f, 0.998f, 1.f, 0.f,
            -1.f,  1.f, 0.998f, 0.f, 1.f,
             1.f,  1.f, 0.998f, 1.f, 1.f,
        };
        static const GLuint idx[] = {0, 1, 2, 1, 3, 2};
        GLuint vbo, ebo;
        glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo); glGenBuffers(1, &ebo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(GLfloat), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(GLfloat), (void*)(3*sizeof(GLfloat)));
        glBindVertexArray(0);
    }

    void render(float dt) {
        time += dt;
        float yaw   = sinf(time * 0.07f) * 1.2f;
        float pitch = sinf(time * 0.04f) * 0.8f - 0.4f;
        glUseProgram(program);
        glUniform1f(glGetUniformLocation(program, "uTime"),  time);
        glUniform1f(glGetUniformLocation(program, "uYaw"),   yaw);
        glUniform1f(glGetUniformLocation(program, "uPitch"), pitch);
        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

    void shutdown() {
        if (vao) glDeleteVertexArrays(1, &vao);
        if (program) glDeleteProgram(program);
    }
};

// =============================================================================
// IMGUI MODULE
// =============================================================================

struct ModImgui
{
    ImGuiContext* ctx = nullptr;

    void init(SDL_Window* w, SDL_GLContext g) {
        IMGUI_CHECKVERSION();
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        ImGui_ImplSDL2_InitForOpenGL(w, g);
        ImGui_ImplOpenGL3_Init("#version 300 es");
    }

    void shutdown() {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext(ctx);
        ctx = nullptr;
    }

    void processEvent(const SDL_Event& e) {
        ImGui_ImplSDL2_ProcessEvent(&e);
    }

    void newFrame() {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
    }

    void render() {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
};

// =============================================================================
// APPLICATION STATE
// =============================================================================

// Maps key index 0-11 (chromatic from C) to SDL keycode
// Z S X D C V G B H N J M
static const SDL_Keycode PIANO_KEYS[12] = {
    SDLK_z, SDLK_s, SDLK_x, SDLK_d, SDLK_c,
    SDLK_v, SDLK_g, SDLK_b, SDLK_h, SDLK_n, SDLK_j, SDLK_m
};

// Which of the 12 semitones are black keys
static const bool IS_BLACK[12] = {
    false, true, false, true, false,
    false, true, false, true, false, true, false
};

// For each white key index 0-6, which semitone it is
static const int WHITE_TO_SEMI[7] = { 0, 2, 4, 5, 7, 9, 11 };

// PIANO_INSTRS is defined above with patches

static const int PIANO_INSTR_COUNT = 6;
static const int PIANO_BASE_NOTE = 60; // C4

struct AppState
{
    SDL_Window*   window = nullptr;
    SDL_GLContext glCtx  = nullptr;
    int           winW   = 720;
    int           winH   = 520;

    AuroraRenderer aurora;
    ModImgui       imgui;

    // Sound system state
    xfm_module*      music_module = nullptr;  // for music/songs
    xfm_module*      sfx_module   = nullptr;  // for SFX/piano
    SDL_AudioDeviceID audio_dev  = 0;
    bool            sound_running = false;

    // Sound settings (configured before start)
    int             sample_rate   = 44100;
    int             buffer_frames = 256;
    xfm_chip_type    chip_type     = XFM_CHIP_YM2612;

    // Volume controls (0.0 - 1.0)
    float           music_volume  = 1.0f;
    float           sfx_volume    = 1.0f;

    // LFO controls
    bool            music_lfo_enable = false;
    int             music_lfo_freq   = 0;  // 0-7
    bool            sfx_lfo_enable   = false;
    int             sfx_lfo_freq     = 0;  // 0-7

    // Song state
    int             current_song_id = 0;  // 0 = none, 1 = SONG_1, 2 = SONG_2
    int             current_row     = 0;
    int             total_rows      = 0;
    bool            song_loop       = true;

    // Piano state
    int  pianoInstrument = 0;   // index into piano instrument list
    int  pianoHeldNote   = -1;  // MIDI note currently held (for mouse), -1 if none
    bool pianoKeyHeld[12] = {}; // which of the 12 keys are pressed
    int  pianoVoice[12]  = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}; // voice ID for each key

    // Patch editors (one per instrument)
    OPNPatchEditor editors[6];  // [0]=PATCH_00, [1]=PATCH_01, [2]=PATCH_HIHAT, [3]=PATCH_KICK, [4]=PATCH_SNARE, [5]=PATCH_CLANG
    bool editorsInitialized = false;

    Uint32 lastTick    = 0;
    float  fpsSmooth   = 0.0f;
    float  displayFps  = 0.0f;
    float  displayMs   = 0.0f;
    Uint32 lastDisplay = 0;

    bool running = true;
};

static AppState g_app;

// =============================================================================
// SOUND SYSTEM INIT/TEARDOWN
// =============================================================================

// SDL audio callback - mixes both modules
static void sdl_audio_callback(void* userdata, Uint8* stream, int len)
{
    AppState* app = static_cast<AppState*>(userdata);
    int16_t* buffer = reinterpret_cast<int16_t*>(stream);
    int frames = len / 4; // stereo 16-bit

    // Clear buffer first
    std::memset(buffer, 0, len);

    // Mix music module (song only - more efficient!)
    if (app->music_module) {
        xfm_mix_song(app->music_module, buffer, frames);
    }

    // Mix SFX module (SFX only - more efficient!)
    if (app->sfx_module) {
        // For additive mixing, we need to accumulate
        // For simplicity, just mix SFX for now
        int16_t* sfx_buffer = new int16_t[frames * 2];
        std::memset(sfx_buffer, 0, frames * 2 * sizeof(int16_t));
        xfm_mix_sfx(app->sfx_module, sfx_buffer, frames);

        // Simple additive mix with clipping prevention
        for (int i = 0; i < frames * 2; i++) {
            int mixed = static_cast<int>(buffer[i]) + static_cast<int>(sfx_buffer[i]);
            mixed = std::max(-32768, std::min(32767, mixed));
            buffer[i] = static_cast<int16_t>(mixed);
        }
        delete[] sfx_buffer;
    }
}

static bool start_sound_system(AppState& app)
{
    if (app.sound_running) return true;

    // Create music module (for songs - not used yet)
    app.music_module = xfm_module_create(app.sample_rate, app.buffer_frames, app.chip_type);
    if (!app.music_module) {
        std::fprintf(stderr, "Failed to create music module\n");
        return false;
    }

    // Create SFX module (for piano)
    app.sfx_module = xfm_module_create(app.sample_rate, app.buffer_frames, app.chip_type);
    if (!app.sfx_module) {
        std::fprintf(stderr, "Failed to create SFX module\n");
        xfm_module_destroy(app.music_module);
        app.music_module = nullptr;
        return false;
    }

    // Load patches into SFX module
    xfm_patch_set(app.sfx_module, 0x00, &PATCH_00, sizeof(PATCH_00), XFM_CHIP_YM2612);
    xfm_patch_set(app.sfx_module, 0x01, &PATCH_01, sizeof(PATCH_01), XFM_CHIP_YM2612);
    xfm_patch_set(app.sfx_module, 0x02, &PATCH_HIHAT, sizeof(PATCH_HIHAT), XFM_CHIP_YM2612);
    xfm_patch_set(app.sfx_module, 0x20, &PATCH_KICK, sizeof(PATCH_KICK), XFM_CHIP_YM2612);
    xfm_patch_set(app.sfx_module, 0x21, &PATCH_SNARE, sizeof(PATCH_SNARE), XFM_CHIP_YM2612);
    xfm_patch_set(app.sfx_module, 0x23, &PATCH_CLANG, sizeof(PATCH_CLANG), XFM_CHIP_YM2612);

    // Load patches into music module (for songs)
    xfm_patch_set(app.music_module, 0x00, &PATCH_00, sizeof(PATCH_00), XFM_CHIP_YM2612);
    xfm_patch_set(app.music_module, 0x01, &PATCH_01, sizeof(PATCH_01), XFM_CHIP_YM2612);
    xfm_patch_set(app.music_module, 0x02, &PATCH_HIHAT, sizeof(PATCH_HIHAT), XFM_CHIP_YM2612);
    xfm_patch_set(app.music_module, 0x20, &PATCH_KICK, sizeof(PATCH_KICK), XFM_CHIP_YM2612);
    xfm_patch_set(app.music_module, 0x21, &PATCH_SNARE, sizeof(PATCH_SNARE), XFM_CHIP_YM2612);
    xfm_patch_set(app.music_module, 0x23, &PATCH_CLANG, sizeof(PATCH_CLANG), XFM_CHIP_YM2612);

    // Declare SFX patterns
    xfm_sfx_declare(app.sfx_module, SFX_ID_JUMP,    SFX_JUMP,    60, 3);
    xfm_sfx_declare(app.sfx_module, SFX_ID_COIN,    SFX_COIN,    60, 3);
    xfm_sfx_declare(app.sfx_module, SFX_ID_ALARM,   SFX_ALARM,   60, 3);
    xfm_sfx_declare(app.sfx_module, SFX_ID_FANFARE, SFX_FANFARE, 60, 3);

    // Declare songs in music module
    xfm_song_declare(app.music_module, SONG_ID_1, SONG_1, 60, 6);
    xfm_song_declare(app.music_module, SONG_ID_2, SONG_2, 60, 4);

    // Start song 1 by default
    xfm_song_play(app.music_module, SONG_ID_1, true);
    app.current_song_id = SONG_ID_1;
    app.total_rows = xfm_song_get_total_rows(app.music_module, SONG_ID_1);

    // Open SDL audio device
    SDL_AudioSpec desired{};
    desired.freq     = app.sample_rate;
    desired.format   = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples  = static_cast<Uint16>(app.buffer_frames);
    desired.callback = sdl_audio_callback;
    desired.userdata = &app;

    SDL_AudioSpec obtained{};
    app.audio_dev = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (app.audio_dev == 0) {
        std::fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        xfm_module_destroy(app.sfx_module);
        xfm_module_destroy(app.music_module);
        app.sfx_module = nullptr;
        app.music_module = nullptr;
        return false;
    }

    std::printf("[xfm_demo] Sound started: %d Hz, %d samples\n", obtained.freq, obtained.samples);

    SDL_PauseAudioDevice(app.audio_dev, 0);
    app.sound_running = true;

    return true;
}

static void stop_sound_system(AppState& app)
{
    if (!app.sound_running) return;

    if (app.audio_dev) {
        SDL_CloseAudioDevice(app.audio_dev);
        app.audio_dev = 0;
    }

    if (app.sfx_module) {
        xfm_module_destroy(app.sfx_module);
        app.sfx_module = nullptr;
    }

    if (app.music_module) {
        xfm_module_destroy(app.music_module);
        app.music_module = nullptr;
    }

    app.sound_running = false;
    
    // Reset piano state
    app.pianoHeldNote = -1;
    for (int k = 0; k < 12; k++) {
        app.pianoKeyHeld[k] = false;
        app.pianoVoice[k] = -1;
    }
    
    std::printf("[xfm_demo] Sound stopped\n");
}

// =============================================================================
// UI PANEL
// =============================================================================

static void drawPanel(AppState& app)
{
    ImGuiIO& io = ImGui::GetIO();
    const float panelW = 400.f;
    const float panelH = (float)app.winH - 40.f;

    ImGui::SetNextWindowPos(
        ImVec2((io.DisplaySize.x - panelW) * 0.5f, (io.DisplaySize.y - panelH) * 0.5f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);

    ImGui::Begin("eggsfm xfm_demo", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);

    // =========================================================================
    // SECTION 1 — HARDWARE
    // =========================================================================
    ImGui::SeparatorText("Hardware");
    ImGui::BeginDisabled(app.sound_running);
    {
        // Chip selector
        static const char* CHIP_LBLS[] = { "YM2612 — Original Sega", "YM3438 — Clean CMOS" };
        int chipIdx = (app.chip_type == XFM_CHIP_YM3438) ? 1 : 0;
        
        ImGui::Text("Chip"); ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::BeginCombo("##chip", CHIP_LBLS[chipIdx])) {
            for (int i = 0; i < 2; i++) {
                if (ImGui::Selectable(CHIP_LBLS[i], i == chipIdx)) {
                    app.chip_type = (i == 0) ? XFM_CHIP_YM2612 : XFM_CHIP_YM3438;
                }
            }
            ImGui::EndCombo();
        }

        // Sample rate selector
        static const int RATES[]     = { 11025, 22050, 44100, 48000 };
        static const char* RATE_LBLS[] = { "11025 Hz", "22050 Hz", "44100 Hz", "48000 Hz" };
        int rateIdx = 2; // default 44100
        for (int i = 0; i < 4; i++) if (RATES[i] == app.sample_rate) rateIdx = i;

        ImGui::Text("Sample rate"); ImGui::SameLine(); ImGui::SetNextItemWidth(110.f);
        if (ImGui::BeginCombo("##rate", RATE_LBLS[rateIdx])) {
            for (int i = 0; i < 4; i++) {
                if (ImGui::Selectable(RATE_LBLS[i], i == rateIdx)) {
                    app.sample_rate = RATES[i];
                }
            }
            ImGui::EndCombo();
        }

        // Buffer size selector
        static const int BUFS[]      = { 256, 512, 1024, 2048 };
        static const char* BUF_LBLS[] = { "256", "512", "1024", "2048" };
        int bufIdx = 0; // default 256
        for (int i = 0; i < 4; i++) if (BUFS[i] == app.buffer_frames) bufIdx = i;

        ImGui::SameLine(); ImGui::Text("Buffer"); ImGui::SameLine(); ImGui::SetNextItemWidth(-1.f);
        if (ImGui::BeginCombo("##buf", BUF_LBLS[bufIdx])) {
            for (int i = 0; i < 4; i++) {
                if (ImGui::Selectable(BUF_LBLS[i], i == bufIdx)) {
                    app.buffer_frames = BUFS[i];
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::EndDisabled();

    // =========================================================================
    // SECTION 2 — PLAYBACK
    // =========================================================================
    ImGui::SeparatorText("Playback");
    ImGui::Spacing();

    if (!app.sound_running) {
        ImGui::TextDisabled("Engine stopped.");
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.35f, 0.15f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.5f,  0.25f, 1.f));
        if (ImGui::Button("Start Sound System", ImVec2(-1, 0))) {
            if (!start_sound_system(app)) {
                std::fprintf(stderr, "Failed to start sound system\n");
            }
        }
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.9f, 0.4f, 1.f));
        ImGui::Text("●  RUNNING");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("%d Hz  buf %d", app.sample_rate, app.buffer_frames);
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.4f, 0.08f, 0.08f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.12f, 0.12f, 1.f));
        if (ImGui::Button("Stop Sound System", ImVec2(-1, 0))) {
            stop_sound_system(app);
            // Reset piano state
            app.pianoHeldNote = -1;
            for (int k = 0; k < 12; k++) app.pianoKeyHeld[k] = false;
        }
        ImGui::PopStyleColor(2);
    }

    // =========================================================================
    // SECTION 3 — SONGS
    // =========================================================================
    ImGui::SeparatorText("Songs");
    
    if (app.sound_running) {
        // Current song indicator
        if (app.current_song_id == SONG_ID_1) {
            ImGui::Text("Playing: Song 1 (64 rows)");
        } else if (app.current_song_id == SONG_ID_2) {
            ImGui::Text("Playing: Song 2 (16 rows)");
        } else {
            ImGui::TextDisabled("No song playing");
        }
        
        // Row progress
        app.current_row = xfm_song_get_row(app.music_module);
        char row_label[32];
        snprintf(row_label, sizeof(row_label), "%d/%d", app.current_row, app.total_rows);
        ImGui::ProgressBar((float)app.current_row / app.total_rows, ImVec2(-1, 0), row_label);
        
        ImGui::Spacing();
        
        // Song selection buttons
        float hw = (panelW - 40.f) / 2.f;
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.2f, 0.3f, 0.45f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.45f, 0.65f, 1.f));
        
        if (app.current_song_id != SONG_ID_1) {
            if (ImGui::Button("Song 1 - Now", ImVec2(hw, 0))) {
                xfm_song_play(app.music_module, SONG_ID_1, true);
                app.current_song_id = SONG_ID_1;
                app.total_rows = xfm_song_get_total_rows(app.music_module, SONG_ID_1);
            }
            ImGui::SameLine();
            if (ImGui::Button("Song 1 - Next Row", ImVec2(-1, 0))) {
                xfm_song_schedule(app.music_module, SONG_ID_1, FM_SONG_SWITCH_STEP);
                app.current_song_id = SONG_ID_1;
                app.total_rows = xfm_song_get_total_rows(app.music_module, SONG_ID_1);
            }
        }
        
        if (app.current_song_id != SONG_ID_2) {
            if (ImGui::Button("Song 2 - Now", ImVec2(hw, 0))) {
                xfm_song_play(app.music_module, SONG_ID_2, true);
                app.current_song_id = SONG_ID_2;
                app.total_rows = xfm_song_get_total_rows(app.music_module, SONG_ID_2);
            }
            ImGui::SameLine();
            if (ImGui::Button("Song 2 - Next Row", ImVec2(-1, 0))) {
                xfm_song_schedule(app.music_module, SONG_ID_2, FM_SONG_SWITCH_STEP);
                app.current_song_id = SONG_ID_2;
                app.total_rows = xfm_song_get_total_rows(app.music_module, SONG_ID_2);
            }
        }
        
        ImGui::PopStyleColor(2);
        
        // Loop toggle
        ImGui::Spacing();
        if (ImGui::Checkbox("Loop", &app.song_loop)) {
            // Loop state is handled internally
        }
    } else {
        ImGui::TextDisabled("Start sound system to play songs.");
    }

    // =========================================================================
    // SECTION 4 — CACHE
    // =========================================================================
    ImGui::SeparatorText("Cache");
    ImGui::TextDisabled("Cache system not yet implemented.");

    // =========================================================================
    // SECTION 5 — MIXER
    // =========================================================================
    ImGui::SeparatorText("Mixer");

    if (app.sound_running) {
        // Two columns: Music | SFX
        if (ImGui::BeginTable("mixer", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            
            // Music column
            ImGui::TableNextColumn();
            ImGui::Text("Music");
            ImGui::Separator();
            
            int musicVolPct = (int)(app.music_volume * 100.f);
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::SliderInt("##musicvol", &musicVolPct, 0, 100, "Volume %d%%")) {
                app.music_volume = musicVolPct / 100.f;
                xfm_module_set_volume(app.music_module, app.music_volume);
            }
            
            static const char* LFO_LBLS[] = {
                "Off", "3.82 Hz", "5.33 Hz", "5.77 Hz", "6.11 Hz", "6.60 Hz", "9.23 Hz", "46.1 Hz", "69.2 Hz"
            };
            ImGui::SetNextItemWidth(-1.f);
            int musicLfoIdx = app.music_lfo_enable ? app.music_lfo_freq + 1 : 0;
            if (ImGui::Combo("##musiclfo", &musicLfoIdx, LFO_LBLS, 9)) {
                app.music_lfo_enable = (musicLfoIdx > 0);
                app.music_lfo_freq = musicLfoIdx - 1;
                xfm_module_set_lfo(app.music_module, app.music_lfo_enable, app.music_lfo_freq);
            }
            
            // SFX column
            ImGui::TableNextColumn();
            ImGui::Text("SFX");
            ImGui::Separator();
            
            int sfxVolPct = (int)(app.sfx_volume * 100.f);
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::SliderInt("##sfxvol", &sfxVolPct, 0, 100, "Volume %d%%")) {
                app.sfx_volume = sfxVolPct / 100.f;
                xfm_module_set_volume(app.sfx_module, app.sfx_volume);
            }
            
            ImGui::SetNextItemWidth(-1.f);
            int sfxLfoIdx = app.sfx_lfo_enable ? app.sfx_lfo_freq + 1 : 0;
            if (ImGui::Combo("##sfxlfo", &sfxLfoIdx, LFO_LBLS, 9)) {
                app.sfx_lfo_enable = (sfxLfoIdx > 0);
                app.sfx_lfo_freq = sfxLfoIdx - 1;
                xfm_module_set_lfo(app.sfx_module, app.sfx_lfo_enable, app.sfx_lfo_freq);
            }
            
            ImGui::EndTable();
        }
    } else {
        ImGui::TextDisabled("Start sound system for mixer controls.");
    }

    // =========================================================================
    // SECTION 6 — SOUND EFFECTS
    // =========================================================================
    ImGui::SeparatorText("Sound Effects");
    
    if (app.sound_running) {
        struct SfxBtn { int id; int pri; int dur; const char* label; ImVec4 col; };
        static const SfxBtn btns[] = {
            { SFX_ID_JUMP,    4, 10, "[Q] JUMP",    ImVec4(0.2f, 0.5f, 0.8f, 1.f) },
            { SFX_ID_COIN,    3,  9, "[W] COIN",    ImVec4(0.8f, 0.7f, 0.1f, 1.f) },
            { SFX_ID_ALARM,   5, 12, "[E] ALARM",   ImVec4(0.8f, 0.4f, 0.1f, 1.f) },
            { SFX_ID_FANFARE, 6, 16, "[R] FANFARE", ImVec4(0.6f, 0.1f, 0.6f, 1.f) },
        };
        
        const float bw = (panelW - 40.f) / 2.f;
        for (int i = 0; i < 4; i++) {
            if (i % 2 != 0) ImGui::SameLine();
            const SfxBtn& b = btns[i];
            ImVec4 dim(b.col.x * 0.35f, b.col.y * 0.35f, b.col.z * 0.35f, 0.9f);
            ImVec4 hov(b.col.x * 0.6f, b.col.y * 0.6f, b.col.z * 0.6f, 1.f);
            ImGui::PushStyleColor(ImGuiCol_Button, dim);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, b.col);
            if (ImGui::Button(b.label, ImVec2(bw, 36.f))) {
                xfm_sfx_play(app.sfx_module, b.id, b.pri);
            }
            ImGui::PopStyleColor(3);
        }
    } else {
        ImGui::TextDisabled("Start sound system to play SFX.");
    }

    // =========================================================================
    // SECTION 7 — INSTRUMENTS
    // =========================================================================
    ImGui::SeparatorText("Instruments");
    
    if (app.sound_running) {
        // Initialize editors on first frame
        if (!app.editorsInitialized) {
            app.editors[0].init("PATCH_00", PATCH_00);
            app.editors[1].init("PATCH_01", PATCH_01);
            app.editors[2].init("PATCH_HIHAT", PATCH_HIHAT);
            app.editors[3].init("PATCH_KICK", PATCH_KICK);
            app.editors[4].init("PATCH_SNARE", PATCH_SNARE);
            app.editors[5].init("PATCH_CLANG", PATCH_CLANG);
            app.editorsInitialized = true;
        }
        
        static const char* EDITOR_NAMES[] = {"PATCH_00", "PATCH_01", "PATCH_HIHAT", "PATCH_KICK", "PATCH_SNARE", "PATCH_CLANG"};
        static const int EDITOR_PATCH_IDS[] = {0x00, 0x01, 0x02, 0x20, 0x21, 0x23};
        
        // Song 1 row (3 instruments)
        ImGui::TextDisabled("Song 1:");
        float edBtnW3 = (panelW - 40.f) / 3.f;
        for (int i = 0; i < 3; i++) {
            if (i > 0) ImGui::SameLine();
            OPNPatchEditor& ed = app.editors[i];
            ImVec4 col = ed.open ? ImVec4(0.3f, 0.5f, 0.7f, 1.f) : ImVec4(0.18f, 0.28f, 0.38f, 1.f);
            ImGui::PushStyleColor(ImGuiCol_Button, col);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.6f, 0.8f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.7f, 0.9f, 1.f));
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "%s##ed%d", EDITOR_NAMES[i], i);
            if (ImGui::Button(lbl, ImVec2(edBtnW3, 0))) {
                ed.open = !ed.open;
            }
            ImGui::PopStyleColor(3);
        }
        
        // Song 2 row (3 instruments)
        ImGui::TextDisabled("Song 2:");
        float edBtnW4 = (panelW - 40.f) / 3.f;
        for (int i = 3; i < 6; i++) {
            if (i > 3) ImGui::SameLine();
            OPNPatchEditor& ed = app.editors[i];
            ImVec4 col = ed.open ? ImVec4(0.3f, 0.5f, 0.7f, 1.f) : ImVec4(0.18f, 0.28f, 0.38f, 1.f);
            ImGui::PushStyleColor(ImGuiCol_Button, col);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.6f, 0.8f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.7f, 0.9f, 1.f));
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "%s##ed%d", EDITOR_NAMES[i], i);
            if (ImGui::Button(lbl, ImVec2(edBtnW4, 0))) {
                ed.open = !ed.open;
            }
            ImGui::PopStyleColor(3);
        }
    } else {
        ImGui::TextDisabled("Start sound system to edit instruments.");
    }

    // =========================================================================
    // FOOTER — FPS
    // =========================================================================
    ImGui::Separator();
    ImGui::TextDisabled("%.0f fps  %.2f ms", app.displayFps, app.displayMs);

    ImGui::End();
    
    // Draw patch editor windows (outside main panel)
    if (app.sound_running && app.editorsInitialized) {
        static const char* EDITOR_NAMES[] = {"PATCH_00", "PATCH_01", "PATCH_HIHAT", "PATCH_KICK", "PATCH_SNARE", "PATCH_CLANG"};
        static const int EDITOR_PATCH_IDS[] = {0x00, 0x01, 0x02, 0x20, 0x21, 0x23};
        
        for (int i = 0; i < 6; i++) {
            OPNPatchEditor& ed = app.editors[i];
            char wndTitle[64];
            snprintf(wndTitle, sizeof(wndTitle), "OPN Editor — %s###opned%d", EDITOR_NAMES[i], i);
            
            if (ed.drawWindow(wndTitle, ImVec2(500, 640))) {
                // Patch was modified — update both music and SFX modules
                SDL_LockAudioDevice(app.audio_dev);
                xfm_patch_set(app.music_module, EDITOR_PATCH_IDS[i], &ed.patch, sizeof(ed.patch), XFM_CHIP_YM2612);
                xfm_patch_set(app.sfx_module, EDITOR_PATCH_IDS[i], &ed.patch, sizeof(ed.patch), XFM_CHIP_YM2612);
                SDL_UnlockAudioDevice(app.audio_dev);
            }
        }
    }
}

// =============================================================================
// PIANO KEYBOARD (polyphonic)
// =============================================================================

static void drawPiano(AppState& app)
{
    // Only draw piano when sound is running
    if (!app.sound_running) return;

    ImGuiIO& io = ImGui::GetIO();
    const float winW   = io.DisplaySize.x;
    const float winH   = io.DisplaySize.y;

    const float pianoH = 130.f;
    const float pianoY = winH - pianoH - 10.f;
    ImGui::SetNextWindowPos(ImVec2(0, pianoY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(winW, pianoH + 10.f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::Begin("##piano", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar);

    // Instrument selector
    ImGui::Text("Piano:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(140.f);
    if (ImGui::BeginCombo("##pinstr", PIANO_INSTRS[app.pianoInstrument].name)) {
        for (int i = 0; i < PIANO_INSTR_COUNT; i++) {
            bool sel = (i == app.pianoInstrument);
            if (ImGui::Selectable(PIANO_INSTRS[i].name, sel))
                app.pianoInstrument = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("  Z S X D C V G B H N J M  (polyphonic!)");

    // Draw the keyboard using ImDrawList
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cp = ImGui::GetCursorScreenPos();

    const float totalW   = winW - 20.f;
    const int   NUM_WHITE = 7;
    const float wkW      = totalW / NUM_WHITE;  // white key width
    const float wkH      = 80.f;
    const float bkW      = wkW * 0.6f;
    const float bkH      = wkH * 0.58f;
    const float kX       = cp.x;
    const float kY       = cp.y;

    // Black keys sit between white keys: after C, D, F, G, A
    static const int BLACK_AFTER_WHITE[5] = { 0, 1, 3, 4, 5 };
    static const int BLACK_SEMI[5]        = { 1, 3, 6, 8, 10 };

    // Draw white keys first
    for (int w = 0; w < NUM_WHITE; w++) {
        int semi = WHITE_TO_SEMI[w];
        bool held = app.pianoKeyHeld[semi];
        float x0 = kX + w * wkW;
        float y0 = kY;
        ImU32 col = held ? IM_COL32(180,220,255,255) : IM_COL32(240,240,240,255);
        ImU32 bdr = held ? IM_COL32(20,80,160,255) : IM_COL32(80,80,80,255);
        dl->AddRectFilled(ImVec2(x0+1,y0), ImVec2(x0+wkW-1,y0+wkH), col, 3.f);
        dl->AddRect(ImVec2(x0,y0), ImVec2(x0+wkW,y0+wkH), bdr, 3.f);
        // Key label at bottom
        static const char WK_CHAR[7] = {'Z','X','C','V','B','N','M'};
        char lbl[4];
        snprintf(lbl, sizeof(lbl), "%c", WK_CHAR[w]);
        ImU32 lblCol = held ? IM_COL32(20,80,160,255) : IM_COL32(100,100,100,255);
        dl->AddText(ImVec2(x0 + wkW*0.5f - 4.f, y0 + wkH - 16.f), lblCol, lbl);
    }

    // Draw black keys on top
    static const char BK_CHAR[5] = {'S','D','G','H','J'};
    for (int b = 0; b < 5; b++) {
        int semi = BLACK_SEMI[b];
        bool held = app.pianoKeyHeld[semi];
        int wAfter = BLACK_AFTER_WHITE[b];
        float x0 = kX + (wAfter + 1) * wkW - bkW * 0.5f;
        float y0 = kY;
        ImU32 col = held ? IM_COL32(60,130,220,255) : IM_COL32(30,30,30,255);
        ImU32 bdr = IM_COL32(0,0,0,255);
        dl->AddRectFilled(ImVec2(x0,y0), ImVec2(x0+bkW,y0+bkH), col, 2.f);
        dl->AddRect(ImVec2(x0,y0), ImVec2(x0+bkW,y0+bkH), bdr, 2.f);
        char lbl[4]; snprintf(lbl, sizeof(lbl), "%c", BK_CHAR[b]);
        ImU32 lblCol = held ? IM_COL32(180,220,255,255) : IM_COL32(160,160,160,255);
        dl->AddText(ImVec2(x0 + bkW*0.5f - 4.f, y0 + bkH - 16.f), lblCol, lbl);
    }

    // Mouse click on keys
    ImVec2 mpos = ImGui::GetMousePos();
    bool mclick = ImGui::IsMouseClicked(0);
    bool mrel   = ImGui::IsMouseReleased(0);

    if (mpos.y >= kY && mpos.y < kY + wkH) {
        int clickedNote = -1;
        // Check black keys first (they're on top)
        if (mpos.y < kY + bkH) {
            for (int b = 0; b < 5; b++) {
                int wAfter = BLACK_AFTER_WHITE[b];
                float x0 = kX + (wAfter + 1) * wkW - bkW * 0.5f;
                if (mpos.x >= x0 && mpos.x < x0 + bkW) {
                    clickedNote = BLACK_SEMI[b];
                    break;
                }
            }
        }
        if (clickedNote < 0) {
            int w = (int)((mpos.x - kX) / wkW);
            if (w >= 0 && w < NUM_WHITE) clickedNote = WHITE_TO_SEMI[w];
        }

        if (clickedNote >= 0 && mclick) {
            int midiNote = PIANO_BASE_NOTE + clickedNote;
            // Trigger note if sound is running
            if (app.sound_running && app.sfx_module) {
                xfm_voice_id voice = xfm_note_on(app.sfx_module, midiNote, PIANO_INSTRS[app.pianoInstrument].patchId, 0);
                app.pianoVoice[clickedNote] = voice;
            }
            app.pianoHeldNote = midiNote;
        }
    }

    if (mrel && app.pianoHeldNote >= 0) {
        int heldSemi = app.pianoHeldNote - PIANO_BASE_NOTE;
        bool kbHeld = (heldSemi >= 0 && heldSemi < 12) ? app.pianoKeyHeld[heldSemi] : false;
        if (!kbHeld) {
            // Release note if sound is running
            if (app.sound_running && app.sfx_module && app.pianoVoice[heldSemi] >= 0) {
                xfm_note_off(app.sfx_module, app.pianoVoice[heldSemi]);
                app.pianoVoice[heldSemi] = -1;
            }
            app.pianoHeldNote = -1;
        }
    }

    ImGui::End();
}

// =============================================================================
// MAIN TICK
// =============================================================================

static void mainTick()
{
    AppState& app = g_app;

    Uint32 now = SDL_GetTicks();
    float  dt  = (app.lastTick == 0) ? 0.016f : (now - app.lastTick) * 0.001f;
    app.lastTick = now;
    if (dt > 0.1f) dt = 0.1f;

    float fps = (dt > 0.0001f) ? (1.f / dt) : 9999.f;
    app.fpsSmooth = (app.fpsSmooth < 1.f) ? fps : app.fpsSmooth * 0.9f + fps * 0.1f;
    if (now - app.lastDisplay >= 500) {
        app.displayFps  = app.fpsSmooth;
        app.displayMs   = (app.fpsSmooth > 0.f) ? (1000.f / app.fpsSmooth) : 0.f;
        app.lastDisplay = now;
    }

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        app.imgui.processEvent(e);
        if (e.type == SDL_QUIT) {
            app.running = false;
#ifdef __EMSCRIPTEN__
            emscripten_cancel_main_loop();
#endif
        }
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            if (e.key.keysym.sym == SDLK_ESCAPE) {
                app.running = false;
#ifdef __EMSCRIPTEN__
                emscripten_cancel_main_loop();
#endif
            }
            // SFX hotkeys (Q, W, E, R)
            if (app.sound_running && app.sfx_module) {
                if (e.key.keysym.sym == SDLK_q) xfm_sfx_play(app.sfx_module, SFX_ID_JUMP, 4);
                if (e.key.keysym.sym == SDLK_w) xfm_sfx_play(app.sfx_module, SFX_ID_COIN, 3);
                if (e.key.keysym.sym == SDLK_e) xfm_sfx_play(app.sfx_module, SFX_ID_ALARM, 5);
                if (e.key.keysym.sym == SDLK_r) xfm_sfx_play(app.sfx_module, SFX_ID_FANFARE, 6);
            }
            // Piano keys — polyphonic note triggering
            for (int k = 0; k < 12; k++) {
                if (e.key.keysym.sym == PIANO_KEYS[k]) {
                    app.pianoKeyHeld[k] = true;
                    int midiNote = PIANO_BASE_NOTE + k;
                    if (app.sound_running && app.sfx_module) {
                        xfm_voice_id voice = xfm_note_on(app.sfx_module, midiNote, PIANO_INSTRS[app.pianoInstrument].patchId, 0);
                        app.pianoVoice[k] = voice;
                    }
                    break;
                }
            }
        }
        if (e.type == SDL_KEYUP) {
            SDL_Keycode sym = e.key.keysym.sym;
            for (int k = 0; k < 12; k++) {
                if (sym == PIANO_KEYS[k]) {
                    app.pianoKeyHeld[k] = false;
                    // Release this specific voice
                    if (app.sound_running && app.sfx_module && app.pianoVoice[k] >= 0) {
                        xfm_note_off(app.sfx_module, app.pianoVoice[k]);
                        app.pianoVoice[k] = -1;
                    }
                    break;
                }
            }
        }
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_RESIZED) {
            app.winW = e.window.data1;
            app.winH = e.window.data2;
            glViewport(0, 0, app.winW, app.winH);
        }
    }

    glClearColor(0.f, 0.f, 0.05f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    app.aurora.render(dt);

    app.imgui.newFrame();
    drawPanel(app);
    drawPiano(app);
    app.imgui.render();

    SDL_GL_SwapWindow(app.window);
}

// =============================================================================
// MAIN
// =============================================================================

int main(int /*argc*/, char** /*argv*/)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "1");

    AppState& app = g_app;
    app.window = SDL_CreateWindow(
        "eggsfm xfm_demo — xfm API",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        app.winW, app.winH,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);

    if (!app.window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    app.glCtx = SDL_GL_CreateContext(app.window);
    if (!app.glCtx) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(app.window);
        SDL_Quit();
        return 1;
    }

    if (SDL_GL_SetSwapInterval(-1) != 0)
        if (SDL_GL_SetSwapInterval(1) != 0)
            SDL_GL_SetSwapInterval(0);

    glViewport(0, 0, app.winW, app.winH);

    app.aurora.init();
    app.imgui.init(app.window, app.glCtx);

    printf("=== eggsfm xfm_demo ===\n");
    printf("Aurora shader running.\n");
    printf("Click 'Start Sound System' to enable audio.\n");
    printf("Keys: Z-M = piano  |  QWER = SFX  |  Esc = quit\n\n");

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(mainTick, 0, 1);
#else
    while (app.running) {
        mainTick();
    }
#endif

    stop_sound_system(app);
    app.aurora.shutdown();
    app.imgui.shutdown();
    SDL_GL_DeleteContext(app.glCtx);
    SDL_DestroyWindow(app.window);
    SDL_Quit();

    return 0;
}
