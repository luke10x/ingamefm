// =============================================================================
// demo6.cpp — ingamefm SFX playground (Emscripten + ImGui)
//
// Aurora background + ImGui panel with one button per SFX.
// Same 6-channel song as demo5 playing underneath.
// Volume sliders for music and SFX independently.
//
// Targets:
//   Native (macOS/Linux): OpenGL ES 3 via SDL2 + ImGui
//   Web (Emscripten):     WebGL2 via SDL2 + ImGui
//
// Build commands at bottom of file.
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
#include <algorithm>

#include "ingamefm.h"

// =============================================================================
// 2. GLSL
// =============================================================================

#define GLSL_VERSION "#version 300 es\n"

// =============================================================================
// 3. AURORA SHADERS  (identical to demo4)
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
// 4. SONG  (same 6-channel song as demo5)
// =============================================================================

static const char* SONG =
"org.tildearrow.furnace - Pattern Data (32)\n"
"32\n"
/* r 0  */ "C-2017F|A-5037F|A-3047F|C-6057F|.......|.......\n"
/* r 1  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r 2  */ "A-3027F|.......|.......|E-6057F|.......|.......\n"
/* r 3  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r 4  */ "C-2017F|C-6037F|E-3047F|G-6057F|.......|.......\n"
/* r 5  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r 6  */ "A-3027F|.......|.......|E-6057F|.......|.......\n"
/* r 7  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r 8  */ "C-2017F|E-6037F|A-3047F|D-6057F|.......|.......\n"
/* r 9  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r10  */ "A-3027F|.......|.......|C-6057F|OFF....|.......\n"
/* r11  */ "OFF....|.......|.......|OFF....|A-4067F|.......\n"
/* r12  */ "C-2017F|D-6037F|E-3047F|.......|.......|.......\n"
/* r13  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r14  */ "A-3027F|.......|.......|.......|E-4067F|.......\n"
/* r15  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r16  */ "C-2017F|C-6037F|C-3047F|.......|.......|.......\n"
/* r17  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r18  */ "A-3027F|.......|.......|.......|G-4067F|.......\n"
/* r19  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r20  */ "C-2017F|A-5037F|G-3047F|.......|.......|.......\n"
/* r21  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r22  */ "A-3027F|.......|.......|.......|OFF....|E-4077F\n"
/* r23  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r24  */ "C-2017F|G-5037F|G-3047F|.......|.......|A-4077F\n"
/* r25  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r26  */ "A-3027F|.......|.......|.......|.......|G-4077F\n"
/* r27  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r28  */ "C-2017F|A-5037F|A-3047F|.......|.......|E-4077F\n"
/* r29  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r30  */ "A-3027F|.......|.......|.......|.......|D-4077F\n"
/* r31  */ "OFF....|.......|.......|.......|.......|OFF....\n";

// =============================================================================
// 5. SONG 2  — simpler, slower, contrasting feel
//
// 3 channels only (ch3-5 are silent), tick_rate=60, speed=30 -> 500ms/row
// Ch0 — Beat  (KICK/SNARE)
// Ch1 — Simple melody (GUITAR)
// Ch2 — Root bass (SLAP_BASS)
// =============================================================================

static const char* SONG2 =
"org.tildearrow.furnace - Pattern Data (16)\n"
"16\n"
/* r 0  */ "C-3017F|C-6037F|C-4047F|.......|.......|.......\n"
/* r 1  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r 2  */ "A-4027F|.......|.......|.......|.......|.......\n"
/* r 3  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r 4  */ "C-3017F|E-6037F|G-4047F|.......|.......|.......\n"
/* r 5  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r 6  */ "A-4027F|.......|.......|.......|.......|.......\n"
/* r 7  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r 8  */ "C-3017F|G-6037F|F-4047F|.......|.......|.......\n"
/* r 9  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r10  */ "A-4027F|.......|.......|.......|.......|.......\n"
/* r11  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r12  */ "C-3017F|E-6037F|C-4047F|.......|.......|.......\n"
/* r13  */ "OFF....|.......|.......|.......|.......|.......\n"
/* r14  */ "A-4027F|.......|.......|.......|.......|.......\n"
/* r15  */ "OFF....|OFF....|OFF....|.......|.......|.......\n";

static int  g_currentSong  = 0;   // 0 = SONG, 1 = SONG2
static bool g_changePending = false;

// =============================================================================
// 6. SFX TABLE
// =============================================================================

static const char* SFX_DING =
"3\n" "C-4107F\n" "OFF....\n" ".......\n";
static const char* SFX_ALARM =
"6\n" "D-6117F\n" "OFF....\n" "A-5117F\n" "OFF....\n" ".......\n" ".......\n";
static const char* SFX_FANFARE =
"10\n" "C-5127F\n" "E-5127F\n" "G-5127F\n" "C-6127F\n"
"OFF....\n" ".......\n" ".......\n" ".......\n" ".......\n" ".......\n";
static const char* SFX_JUMP =
"5\n" "C-5127F\n" "G-5127F\n" "C-6127F\n" "OFF....\n" ".......\n";
static const char* SFX_COIN =
"4\n" "A-6147F\n" "E-7147F\n" "OFF....\n" ".......\n";
static const char* SFX_LEVEL_UP =
"12\n" "C-5127F\n" "E-5127F\n" "G-5127F\n" "C-6127F\n" "E-6127F\n" "G-6127F\n"
"OFF....\n" ".......\n" ".......\n" ".......\n" ".......\n" ".......\n";
static const char* SFX_DEATH =
"14\n" "A-5137F\n" ".......\n" "F-5137F\n" ".......\n" "D-5137F\n" ".......\n"
"B-4137F\n" ".......\n" "G-4137F\n" ".......\n" "OFF....\n" ".......\n" ".......\n" ".......\n";
static const char* SFX_DAMAGE =
"6\n" "A-3107F\n" "G-3107F\n" "OFF....\n" ".......\n" ".......\n" ".......\n";
static const char* SFX_ATTACK =
"4\n" "D-6117F\n" "OFF....\n" ".......\n" ".......\n";
static const char* SFX_CLIMB =
"6\n" "A-5147F\n" "OFF....\n" "A-5147F\n" "OFF....\n" ".......\n" ".......\n";
static const char* SFX_FALL_WATER =
"10\n" "A-5107F\n" "E-5107F\n" "C-5107F\n" "A-4107F\n" "E-4107F\n"
"C-4107F\n" "OFF....\n" ".......\n" ".......\n" ".......\n";
static const char* SFX_TALK =
"8\n" "E-5157F\n" "OFF....\n" "G-5157F\n" "OFF....\n" "E-5157F\n" "OFF....\n" ".......\n" ".......\n";
static const char* SFX_LAUGH =
"10\n" "C-5157F\n" "E-5157F\n" "G-5157F\n" "OFF....\n"
"C-5157F\n" "E-5157F\n" "G-5157F\n" "OFF....\n" ".......\n" ".......\n";
static const char* SFX_SCREAM =
"12\n" "C-4127F\n" "G-4127F\n" "C-5127F\n" "G-5127F\n" "C-6127F\n" "G-6127F\n"
"C-7127F\n" ".......\n" ".......\n" "OFF....\n" ".......\n" ".......\n";
static const char* SFX_CRY =
"14\n" "G-5157F\n" ".......\n" ".......\n" "E-5157F\n" ".......\n" ".......\n"
"D-5157F\n" ".......\n" ".......\n" "B-4157F\n" ".......\n" ".......\n" "OFF....\n" ".......\n";
static const char* SFX_LAUNCH =
"8\n" "C-3117F\n" "OFF....\n" "C-4117F\n" "G-4117F\n" "C-5117F\n" "G-5117F\n" "OFF....\n" ".......\n";
static const char* SFX_WARP =
"12\n" "C-3137F\n" "C-4137F\n" "C-5137F\n" "C-6137F\n" "C-7137F\n" ".......\n"
"C-6137F\n" "C-5137F\n" "C-4137F\n" "C-3137F\n" "OFF....\n" ".......\n";
static const char* SFX_SLIDE =
"8\n" "A-6147F\n" "G-6147F\n" "E-6147F\n" "D-6147F\n" "C-6147F\n" "A-5147F\n" "OFF....\n" ".......\n";
static const char* SFX_SUBMERSE =
"12\n" "A-4137F\n" ".......\n" "G-4137F\n" ".......\n" "E-4137F\n" ".......\n"
"C-4137F\n" ".......\n" "A-3137F\n" ".......\n" "OFF....\n" ".......\n";

struct SfxEntry { int id; int priority; int duration; SDL_Keycode key; const char* pattern; const char* name; };
static const SfxEntry SFX_TABLE[] =
{
    {  0, 1,  6,  SDLK_o, SFX_DING,       "DING"       },  // was 1
    {  1, 3, 12,  SDLK_p, SFX_ALARM,      "ALARM"      },  // was 2
    {  2, 5, 20,  SDLK_l, SFX_FANFARE,    "FANFARE"    },  // was 3
    {  3, 4, 10,  SDLK_q, SFX_JUMP,       "JUMP"       },
    {  4, 3,  8,  SDLK_w, SFX_COIN,       "COIN"       },
    {  5, 6, 24,  SDLK_e, SFX_LEVEL_UP,   "LEVEL UP"   },
    {  6, 8, 28,  SDLK_r, SFX_DEATH,      "DEATH"      },
    {  7, 5, 12,  SDLK_t, SFX_DAMAGE,     "DAMAGE"     },
    {  8, 4,  8,  SDLK_y, SFX_ATTACK,     "ATTACK"     },
    {  9, 2, 12,  SDLK_u, SFX_CLIMB,      "CLIMB"      },
    { 10, 3, 20,  SDLK_a, SFX_FALL_WATER, "FALL/WATER" },
    { 11, 2, 16,  SDLK_s, SFX_TALK,       "TALK"       },
    { 12, 3, 20,  SDLK_d, SFX_LAUGH,      "LAUGH"      },
    { 13, 6, 24,  SDLK_f, SFX_SCREAM,     "SCREAM"     },
    { 14, 3, 28,  SDLK_g, SFX_CRY,        "CRY"        },
    { 15, 5, 16,  SDLK_h, SFX_LAUNCH,     "LAUNCH"     },
    { 16, 5, 24,  SDLK_j, SFX_WARP,       "WARP"       },
    { 17, 3, 16,  SDLK_k, SFX_SLIDE,      "SLIDE"      },
    { 18, 3, 24,  SDLK_SEMICOLON, SFX_SUBMERSE, "SUBMERSE" },
};
static constexpr int SFX_COUNT = 19;

// =============================================================================
// 6. GL HELPERS
// =============================================================================

static void checkGLError(const char* where)
{
    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
        fprintf(stderr, "GL error 0x%x at %s\n", err, where);
}

static GLuint compileShader(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader error:\n%s\n", log);
        exit(1);
    }
    return s;
}

static GLuint createProgram(const char* vert, const char* frag)
{
    GLuint vs = compileShader(GL_VERTEX_SHADER,  vert);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, frag);
    GLuint p  = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

// =============================================================================
// 7. AURORA RENDERER
// =============================================================================

struct AuroraRenderer
{
    GLuint program = 0;
    GLuint vao     = 0;
    float  time    = 3.0f;

    void init()
    {
        program = createProgram(AURORA_VERT, AURORA_FRAG);
        static const GLfloat verts[] = {
            -1.f,-1.f, 1.000f, 0.f,0.f,
             1.f,-1.f, 0.998f, 1.f,0.f,
            -1.f, 1.f, 0.998f, 0.f,1.f,
             1.f, 1.f, 0.998f, 1.f,1.f,
        };
        static const GLuint idx[] = {0,1,2, 1,3,2};
        GLuint vbo, ebo;
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);
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
        checkGLError("AuroraRenderer::init");
    }

    void render(float dt)
    {
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
};

// =============================================================================
// 8. IMGUI MODULE
// =============================================================================

struct ModImgui
{
    ImGuiContext* ctx = nullptr;

    void init(SDL_Window* window, SDL_GLContext glCtx)
    {
        IMGUI_CHECKVERSION();
        ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        ImGui_ImplSDL2_InitForOpenGL(window, glCtx);
        ImGui_ImplOpenGL3_Init("#version 300 es");
    }

    void shutdown()
    {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext(ctx);
        ctx = nullptr;
    }

    void processEvent(const SDL_Event& e) const { ImGui_ImplSDL2_ProcessEvent(&e); }

    void newFrame() const
    {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
    }

    void render() const
    {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
};

// =============================================================================
// 9. APPLICATION STATE
// =============================================================================

struct AppState
{
    SDL_Window*   window   = nullptr;
    SDL_GLContext glCtx    = nullptr;
    int           winW     = 800;
    int           winH     = 560;

    AuroraRenderer aurora;
    ModImgui       imgui;

    SDL_AudioDeviceID audioDev = 0;
    IngameFMPlayer    player;

    float musicVol = 1.0f;
    float sfxVol   = 1.0f;

    Uint32 lastTick  = 0;
    float  fpsSmooth = 0.0f;

    bool running = true;
};

static AppState g_app;

// =============================================================================
// 10. SFX PANEL
// =============================================================================

// Priority colour: low=grey, medium=yellow, high=red
static ImVec4 priorityColor(int p)
{
    if (p <= 2) return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    if (p <= 5) return ImVec4(1.0f, 0.85f, 0.2f, 1.0f);
    return             ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
}

static void drawSfxPanel(AppState& app)
{
    // Center the window on the viewport
    ImGuiIO& io = ImGui::GetIO();
    const float panelW = 400.f;
    const float panelH = (float)app.winH - 40.f;
    ImGui::SetNextWindowPos(
        ImVec2((io.DisplaySize.x - panelW) * 0.5f,
               (io.DisplaySize.y - panelH) * 0.5f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82f);

    ImGui::Begin("ingamefm  •  SFX playground", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse);

    // ── Song selector ────────────────────────────────────────────────────────
    ImGui::TextDisabled("6-channel music  |  sfx_set_voices(3)  |  sfx has own 3-voice pool");
    ImGui::Separator();

    {
        int row = app.player.get_current_row();
        int len = app.player.get_song_length();
        const char* songName = (g_currentSong == 0) ? "Song 1  (6ch, fast)" : "Song 2  (3ch, slow)";
        ImGui::Text("Now playing: %s   row %d / %d", songName, row, len);
        ImGui::Spacing();

        auto doChange = [&](SongChangeWhen when, int startRow) {
            int next = 1 - g_currentSong;
            const char* txt2 = (next == 0) ? SONG : SONG2;
            int tr = (next == 0) ? 60 : 60;
            int sp = (next == 0) ? 20 : 30;
            SDL_LockAudioDevice(app.audioDev);
            app.player.change_song(txt2, tr, sp, when, startRow);
            SDL_UnlockAudioDevice(app.audioDev);
            g_currentSong = next;
        };

        if (ImGui::Button("Switch NOW##sw1"))
            doChange(SongChangeWhen::NOW, 0);
        ImGui::SameLine();
        if (ImGui::Button("Switch at end##sw2"))
            doChange(SongChangeWhen::AT_PATTERN_END, 0);
        ImGui::SameLine();
        // Switch at end but seek into the new song by current row fraction
        if (ImGui::Button("Switch + match pos##sw3")) {
            int nextLen = (g_currentSong == 0) ? 16 : 32; // SONG2=16, SONG=32
            int seekRow = (len > 0) ? (row * nextLen / len) : 0;
            doChange(SongChangeWhen::AT_PATTERN_END, seekRow);
        }
    }
    ImGui::Separator();

    // ── Volume sliders ────────────────────────────────────────────────────────
    // Store 0-100 as int so the slider moves smoothly in 1% increments.
    // Convert to/from the float 0-1 range the player expects.
    ImGui::Text("Volume");
    {
        int mv = static_cast<int>(app.musicVol * 100.f + 0.5f);
        ImGui::SetNextItemWidth(165.f);
        if (ImGui::SliderInt("Music##vol", &mv, 0, 100, "%d%%"))
        {
            app.musicVol = mv / 100.f;
            app.player.set_music_volume(app.musicVol);
        }
        ImGui::SameLine();
        int sv = static_cast<int>(app.sfxVol * 100.f + 0.5f);
        ImGui::SetNextItemWidth(165.f);
        if (ImGui::SliderInt("SFX##vol", &sv, 0, 100, "%d%%"))
        {
            app.sfxVol = sv / 100.f;
            app.player.set_sfx_volume(app.sfxVol);
        }
    }

    ImGui::Separator();

    // ── SFX button grid ───────────────────────────────────────────────────────
    ImGui::Text("Sound Effects  (priority: low  medium  high)");
    ImGui::Spacing();

    const float btnW = 118.f;
    const float btnH = 38.f;
    const int   cols = 3;

    for (int i = 0; i < SFX_COUNT; i++)
    {
        if (i % cols != 0) ImGui::SameLine();

        const SfxEntry& sfx = SFX_TABLE[i];
        ImVec4 col = priorityColor(sfx.priority);

        // Derive key label string
        char keyName[8] = "?";
        SDL_Keycode k = sfx.key;
        if (k >= SDLK_a && k <= SDLK_z)
            snprintf(keyName, sizeof(keyName), "%c", (char)(k - SDLK_a + 'a'));
        else if (k == SDLK_SEMICOLON)
            snprintf(keyName, sizeof(keyName), ";");

        // Button label: key  name  priority
        char label[64];
        snprintf(label, sizeof(label), "[%s] %s\np%d##sfx%d",
                 keyName, sfx.name, sfx.priority, sfx.id);

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(col.x*0.35f, col.y*0.35f, col.z*0.35f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(col.x*0.6f,  col.y*0.6f,  col.z*0.6f,  1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(col.x,       col.y,       col.z,       1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,           ImVec4(1.f, 1.f, 1.f, 1.f));

        if (ImGui::Button(label, ImVec2(btnW, btnH)))
        {
            SDL_LockAudioDevice(app.audioDev);
            app.player.sfx_play(sfx.id, sfx.priority, sfx.duration);
            SDL_UnlockAudioDevice(app.audioDev);
        }
        ImGui::PopStyleColor(4);
    }

    ImGui::Separator();

    // ── FPS ───────────────────────────────────────────────────────────────────
    {
        float ms = (app.fpsSmooth > 0.f) ? (1000.f / app.fpsSmooth) : 0.f;
        ImGui::TextDisabled("%.0f fps  |  %.2f ms/frame", app.fpsSmooth, ms);
    }

    ImGui::End();
}

// =============================================================================
// 11. MAIN TICK
// =============================================================================

static void mainTick()
{
    AppState& app = g_app;

    Uint32 now = SDL_GetTicks();
    float  dt  = (app.lastTick == 0) ? 0.016f : (now - app.lastTick) * 0.001f;
    app.lastTick = now;
    if (dt > 0.1f) dt = 0.1f;

    float fps = (dt > 0.0001f) ? (1.0f / dt) : 9999.0f;
    app.fpsSmooth = (app.fpsSmooth < 1.0f) ? fps : app.fpsSmooth * 0.9f + fps * 0.1f;

    // Events
    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
        app.imgui.processEvent(e);

        if (e.type == SDL_QUIT)
        {
            app.running = false;
#ifdef __EMSCRIPTEN__
            emscripten_cancel_main_loop();
#endif
        }
        if (e.type == SDL_KEYDOWN && !e.key.repeat)
        {
            if (e.key.keysym.sym == SDLK_ESCAPE)
            {
                app.running = false;
#ifdef __EMSCRIPTEN__
                emscripten_cancel_main_loop();
#endif
            }
            // SFX keyboard shortcuts — fire even when ImGui has focus
            // (allows playing sounds while dragging sliders etc.)
            for (int i = 0; i < SFX_COUNT; i++)
            {
                if (e.key.keysym.sym == SFX_TABLE[i].key)
                {
                    SDL_LockAudioDevice(app.audioDev);
                    app.player.sfx_play(SFX_TABLE[i].id,
                                        SFX_TABLE[i].priority,
                                        SFX_TABLE[i].duration);
                    SDL_UnlockAudioDevice(app.audioDev);
                    break;
                }
            }
        }
        if (e.type == SDL_WINDOWEVENT &&
            e.window.event == SDL_WINDOWEVENT_RESIZED)
        {
            app.winW = e.window.data1;
            app.winH = e.window.data2;
            glViewport(0, 0, app.winW, app.winH);
        }
    }

    // Aurora background
    glClearColor(0.0f, 0.0f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    app.aurora.render(dt);

    // ImGui
    app.imgui.newFrame();
    drawSfxPanel(app);
    app.imgui.render();

    SDL_GL_SwapWindow(app.window);
}

// =============================================================================
// 12. INIT + MAIN
// =============================================================================

static bool initVideo(AppState& app)
{
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "1");

    app.window = SDL_CreateWindow(
        "ingamefm demo6 — SFX playground",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        app.winW, app.winH,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    if (!app.window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return false; }

    app.glCtx = SDL_GL_CreateContext(app.window);
    if (!app.glCtx) { fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError()); return false; }

    if (SDL_GL_SetSwapInterval(-1) != 0)
        if (SDL_GL_SetSwapInterval(1) != 0)
            SDL_GL_SetSwapInterval(0);

    glViewport(0, 0, app.winW, app.winH);
    return true;
}

static bool initAudio(AppState& app)
{
    try {
        app.player.set_song(SONG, 60, 20);

        // Music patches
        app.player.add_patch(0x01, PATCH_KICK);
        app.player.add_patch(0x02, PATCH_SNARE);
        app.player.add_patch(0x03, PATCH_GUITAR);
        app.player.add_patch(0x04, PATCH_SLAP_BASS);
        app.player.add_patch(0x05, PATCH_FLUTE);
        app.player.add_patch(0x06, PATCH_SUPERSAW);
        app.player.add_patch(0x07, PATCH_ELECTRIC_BASS);

        // ch3-5 are SFX-evictable
        app.player.sfx_set_voices(3);

        // SFX patches
        app.player.add_patch(0x10, PATCH_SLAP_BASS);
        app.player.add_patch(0x11, PATCH_CLANG);
        app.player.add_patch(0x12, PATCH_GUITAR);
        app.player.add_patch(0x13, PATCH_SYNTH_BASS);
        app.player.add_patch(0x14, PATCH_HIHAT);
        app.player.add_patch(0x15, PATCH_ELECTRIC_BASS);

        for (int i = 0; i < SFX_COUNT; i++)
            app.player.sfx_define(SFX_TABLE[i].id, SFX_TABLE[i].pattern, 60, 3);

    } catch (const std::exception& ex) {
        fprintf(stderr, "Audio setup error: %s\n", ex.what());
        return false;
    }

    SDL_AudioSpec desired{};
    desired.freq     = IngameFMPlayer::SAMPLE_RATE;
    desired.format   = AUDIO_S16SYS;
    desired.channels = 2;
#ifdef __EMSCRIPTEN__
    desired.samples  = 256;  // Web Audio API minimum
#else
    desired.samples  = 128;  // low latency on native
#endif
    desired.callback = IngameFMPlayer::s_audio_callback;
    desired.userdata = &app.player;

    SDL_AudioSpec obtained{};
    app.audioDev = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (app.audioDev == 0) { fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError()); return false; }

    app.player.start(app.audioDev, true);
    SDL_PauseAudioDevice(app.audioDev, 0);
    return true;
}

int main(int /*argc*/, char** /*argv*/)
{
    AppState& app = g_app;

    if (!initVideo(app)) return 1;
    if (!initAudio(app)) return 1;

    app.aurora.init();
    app.imgui.init(app.window, app.glCtx);

    printf("=== ingamefm demo6 — SFX playground ===\n");
    printf("Click any button to trigger that SFX.\n");
    printf("Music and SFX volumes are independent.\n\n");

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(mainTick, 0, 1);
#else
    while (app.running)
        mainTick();

    SDL_CloseAudioDevice(app.audioDev);
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
//       demo6.cpp \
//       -framework Cocoa -framework IOKit -framework CoreVideo \
//       -framework CoreAudio -framework AudioToolbox \
//       -framework ForceFeedback -framework Carbon \
//       -framework Metal -framework GameController -framework CoreHaptics \
//       -lobjc -o demo6 && ./demo6
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
//       demo6.cpp \
//       -s USE_SDL=2 \
//       -s FULL_ES3=1 \
//       -s MIN_WEBGL_VERSION=2 \
//       -s MAX_WEBGL_VERSION=2 \
//       -s ALLOW_MEMORY_GROWTH=1 \
//       -s ASYNCIFY \
//       --shell-file shell4.html \
//       -o demo6.html
//
//   python3 -m http.server  →  http://localhost:8000/demo6.html
// =============================================================================
