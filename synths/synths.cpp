// =============================================================================
// synths.cpp — Multi-chip FM Synthesizer Test Bed
// 3 synths (OPN, OPL, OPM), each with its own patch
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

#include "../xfm_api.h"
#include "../xfm_synth_editors.h"

// =============================================================================
// GLSL — Aurora Shader
// =============================================================================

#define GLSL_VERSION "#version 300 es\n"

static const char* AURORA_VERT = GLSL_VERSION R"(
precision highp float;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
void main() { TexCoord = aTexCoord; gl_Position = vec4(aPos, 1.0); }
)";

static const char* AURORA_FRAG = GLSL_VERSION R"(
precision highp float;
in vec2 TexCoord; out vec4 FragColor;
uniform float uYaw; uniform float uPitch; uniform float uTime;
vec3 mod289v3(vec3 x) { return x - floor(x * (1.0/289.0)) * 289.0; }
vec2 mod289v2(vec2 x) { return x - floor(x * (1.0/289.0)) * 289.0; }
vec3 permute3(vec3 x) { return mod289v3(((x * 34.0) + 1.0) * x); }
float snoise(vec2 v) {
    const vec4 C = vec4(0.211324865405187, 0.366025403784439, -0.577350269189626, 0.024390243902439);
    vec2 i = floor(v + dot(v, C.yy));
    vec2 x0 = v - i + dot(i, C.xx);
    vec2 i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec4 x12 = x0.xyxy + C.xxzz; x12.xy -= i1;
    i = mod289v2(i);
    vec3 p = permute3(permute3(i.y + vec3(0.0,i1.y,1.0)) + i.x + vec3(0.0,i1.x,1.0));
    vec3 m = max(0.5 - vec3(dot(x0,x0), dot(x12.xy,x12.xy), dot(x12.zw,x12.zw)), 0.0);
    m = m*m*m*m;
    vec3 x = 2.0*fract(p*C.www) - 1.0;
    vec3 h = abs(x) - 0.5; vec3 ox = floor(x + 0.5); vec3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314*(a0*a0 + h*h);
    vec3 g; g.x = a0.x*x0.x + h.x*x0.y; g.yz = a0.yz*x12.xz + h.yz*x12.yw;
    return 130.0*dot(m,g);
}
void main() {
    float yawNorm = uYaw / 3.14159, yawOffset = yawNorm * abs(yawNorm);
    float pitchNorm = (uPitch + 1.5708) / 3.14159, pitchOffset = (pitchNorm - 0.5) * 4.0;
    float timeOffset = uTime * 0.0005;
    vec2 uv = TexCoord + vec2(yawOffset, pitchOffset + timeOffset);
    float n1 = snoise(uv * 3.0) * 0.5, n2 = snoise(uv * 7.0 + vec2(uTime*0.01, 0.0)) * 0.3;
    float n3 = snoise(uv * 15.0 + vec2(uTime*0.02, 0.0)) * 0.2;
    float intensity = clamp(n1+n2+n3, 0.0, 1.0);
    vec3 col1 = vec3(sin(TexCoord.x+TexCoord.y+uTime*0.001), 0.2, 0.3);
    vec3 col2 = vec3(0.9, sin(TexCoord.y+uTime*0.0005), 0.5);
    vec3 color = mix(col1, col2, intensity);
    FragColor = vec4(color, pitchNorm);
}
)";

// =============================================================================
// OpenGL Helpers
// =============================================================================

static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof(log), nullptr, log); fprintf(stderr, "Shader error:\n%s\n", log); exit(1); }
    return s;
}

static GLuint createProgram(const char* vert, const char* frag) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vert);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, frag);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs); glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

// =============================================================================
// Aurora Renderer
// =============================================================================

struct AuroraRenderer {
    GLuint program = 0, vao = 0;
    float time = 3.0f;

    void init() {
        program = createProgram(AURORA_VERT, AURORA_FRAG);
        static const GLfloat verts[] = {
            -1.f, -1.f, 1.000f, 0.f, 0.f,  1.f, -1.f, 0.998f, 1.f, 0.f,
            -1.f, 1.f, 0.998f, 0.f, 1.f,  1.f, 1.f, 0.998f, 1.f, 1.f
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
        float yaw = sinf(time * 0.07f) * 1.2f;
        float pitch = sinf(time * 0.04f) * 0.8f - 0.4f;
        glUseProgram(program);
        glUniform1f(glGetUniformLocation(program, "uTime"), time);
        glUniform1f(glGetUniformLocation(program, "uYaw"), yaw);
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
// ImGui Module
// =============================================================================

struct ModImgui {
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

    void processEvent(const SDL_Event& e) { ImGui_ImplSDL2_ProcessEvent(&e); }
    void newFrame() { ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame(); }
    void render() { ImGui::Render(); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); }
};

// =============================================================================
// Synth Types
// =============================================================================

enum SynthType { SYNTH_OPN = 0, SYNTH_OPL, SYNTH_OPM, SYNTH_COUNT };

// =============================================================================
// Application State
// =============================================================================

struct AppState {
    SDL_Window* window = nullptr;
    SDL_GLContext glCtx = nullptr;
    int winW = 900, winH = 600;

    AuroraRenderer aurora;
    ModImgui imgui;

    // Sound system
    xfm_module* synth_module = nullptr;
    SDL_AudioDeviceID audio_dev = 0;
    bool sound_running = false;

    // Three synths, each with its own patch
    struct {
        char name[64];
        xfm_patch_opn opn;
        xfm_patch_opl opl;
        xfm_patch_opm opm;
    } synths[SYNTH_COUNT];

    int selected_synth = SYNTH_OPN;  // Which synth is currently selected

    // Editors
    synth_edit::OPNEditor opn_editor;
    synth_edit::OPL3Editor opl_editor;
    synth_edit::OPMEditor opm_editor;
    bool show_editor = false;

    // Piano state
    int  pianoHeldNote   = -1;
    bool pianoKeyHeld[12] = {};
    int  pianoVoice[12]  = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};

    Uint32 lastTick = 0;
    float fpsSmooth = 0.0f, displayFps = 0.0f, displayMs = 0.0f;
    Uint32 lastDisplay = 0;

    bool running = true;
};

static AppState g_app;

// =============================================================================
// Audio Callback
// =============================================================================
static void sdl_audio_callback(void* userdata, Uint8* stream, int len)
{
    // Cast userdata back to our AppState
    AppState* app = static_cast<AppState*>(userdata);
    
    // Cast stream to 16-bit signed integers (AUDIO_S16SYS format)
    int16_t* buffer = reinterpret_cast<int16_t*>(stream);
    
    // Calculate number of stereo frames
    // len is in bytes, each stereo frame = 4 bytes (2 channels × 2 bytes)
    int frames = len / 4;
    
    // Example: len = 1024 bytes → frames = 256 stereo frames
    
    // Clear buffer first (prevents garbage/noise)
    std::memset(buffer, 0, len);
    
    // Mix music module (song only - more efficient!)
    // xfm_mix_song generates 'frames' stereo samples into buffer
    if (app->synth_module) {
        xfm_mix_sfx(app->synth_module, buffer, frames);
    }
}

// =============================================================================
// Sound System
// =============================================================================

static void apply_current_patch(AppState& app) {
    if (!app.synth_module) return;
    
    // Always use YM2612 chip - module handles all chip types internally
    int idx = app.selected_synth;
    if (idx == SYNTH_OPN) {
        xfm_patch_set(app.synth_module, 0, &app.synths[idx].opn, sizeof(app.synths[idx].opn), XFM_CHIP_YM2612);
    } else if (idx == SYNTH_OPL) {
        xfm_patch_set(app.synth_module, 0, &app.synths[idx].opl, sizeof(app.synths[idx].opl), XFM_CHIP_YM2612);
    } else if (idx == SYNTH_OPM) {
        xfm_patch_set(app.synth_module, 0, &app.synths[idx].opm, sizeof(app.synths[idx].opm), XFM_CHIP_YM2612);
    }
}

static bool start_sound_system(AppState& app) {
    if (app.sound_running) return true;

    app.synth_module = xfm_module_create(44100, 256, XFM_CHIP_YM2612);
    if (!app.synth_module) { fprintf(stderr, "Failed to create synth module\n"); return false; }

    // Apply current synth's patch
    apply_current_patch(app);

    SDL_AudioSpec desired{};
    desired.freq = 44100; desired.format = AUDIO_S16SYS; desired.channels = 2;
    desired.samples = 256; desired.callback = sdl_audio_callback; desired.userdata = &app;

    SDL_AudioSpec obtained{};
    app.audio_dev = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (app.audio_dev == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        xfm_module_destroy(app.synth_module); app.synth_module = nullptr; return false;
    }

    SDL_PauseAudioDevice(app.audio_dev, 0);
    app.sound_running = true;
    printf("[synths] Sound started: %d Hz, 256 samples\n", obtained.freq);
    return true;
}

static void stop_sound_system(AppState& app) {
    if (!app.sound_running) return;
    
    // Release all active piano voices first
    for (int k = 0; k < 12; k++) {
        if (app.pianoVoice[k] >= 0 && app.synth_module) {
            xfm_note_off(app.synth_module, app.pianoVoice[k]);
            app.pianoVoice[k] = -1;
        }
    }
    
    if (app.audio_dev) { SDL_CloseAudioDevice(app.audio_dev); app.audio_dev = 0; }
    if (app.synth_module) { xfm_module_destroy(app.synth_module); app.synth_module = nullptr; }
    app.sound_running = false;
    printf("[synths] Sound stopped\n");
}

static void switch_synth(AppState& app, int newSynth) {
    if (newSynth < 0 || newSynth >= SYNTH_COUNT) return;
    app.selected_synth = newSynth;
    
    // Reload module with new synth's patch
    if (app.synth_module) {
        xfm_module_destroy(app.synth_module);
        app.synth_module = xfm_module_create(44100, 256, XFM_CHIP_YM2612);
        apply_current_patch(app);
    }
}

// =============================================================================
// Piano Keyboard
// =============================================================================

static const SDL_Keycode PIANO_KEYS[12] = {
    SDLK_z, SDLK_s, SDLK_x, SDLK_d, SDLK_c, SDLK_v, SDLK_g, SDLK_b, SDLK_h, SDLK_n, SDLK_j, SDLK_m
};

static const int WHITE_TO_SEMI[7] = { 0, 2, 4, 5, 7, 9, 11 };
static const int PIANO_BASE_NOTE = 60;

static void drawPiano(AppState& app) {
    if (!app.sound_running) return;

    ImGuiIO& io = ImGui::GetIO();
    const float winW = io.DisplaySize.x, winH = io.DisplaySize.y;
    const float pianoH = 130.f, pianoY = winH - pianoH - 10.f;

    ImGui::SetNextWindowPos(ImVec2(0, pianoY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(winW, pianoH + 10.f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::Begin("##piano", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);

    // Synth selector with Edit button
    static const char* SYNTH_NAMES[] = { "OPN (YM2612)", "OPL (YM3812)", "OPM (YM2151)" };
    ImGui::Text("Synth:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(180.f);
    if (ImGui::BeginCombo("##synth", SYNTH_NAMES[app.selected_synth])) {
        for (int i = 0; i < SYNTH_COUNT; i++) {
            bool sel = (i == app.selected_synth);
            if (ImGui::Selectable(SYNTH_NAMES[i], sel)) {
                switch_synth(app, i);
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Edit")) {
        app.show_editor = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("  %s  |  Z-M = piano", app.synths[app.selected_synth].name);

    // Draw keyboard
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cp = ImGui::GetCursorScreenPos();

    const float totalW = winW - 20.f;
    const int NUM_WHITE = 7;
    const float wkW = totalW / NUM_WHITE, wkH = 80.f;
    const float bkW = wkW * 0.6f, bkH = wkH * 0.58f;
    const float kX = cp.x, kY = cp.y;

    // White keys
    for (int w = 0; w < NUM_WHITE; w++) {
        int semi = WHITE_TO_SEMI[w];
        bool held = app.pianoKeyHeld[semi];
        float x0 = kX + w * wkW, y0 = kY;
        ImU32 col = held ? IM_COL32(180, 220, 255, 255) : IM_COL32(240, 240, 240, 255);
        ImU32 bdr = held ? IM_COL32(20, 80, 160, 255) : IM_COL32(80, 80, 80, 255);
        dl->AddRectFilled(ImVec2(x0 + 1, y0), ImVec2(x0 + wkW - 1, y0 + wkH), col, 3.f);
        dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + wkW, y0 + wkH), bdr, 3.f);
        static const char WK_CHAR[7] = {'Z','X','C','V','B','N','M'};
        char lbl[4]; snprintf(lbl, sizeof(lbl), "%c", WK_CHAR[w]);
        ImU32 lblCol = held ? IM_COL32(20, 80, 160, 255) : IM_COL32(100, 100, 100, 255);
        dl->AddText(ImVec2(x0 + wkW * 0.5f - 4.f, y0 + wkH - 16.f), lblCol, lbl);
    }

    // Black keys
    static const int BLACK_AFTER_WHITE[5] = { 0, 1, 3, 4, 5 };
    static const int BLACK_SEMI[5] = { 1, 3, 6, 8, 10 };
    static const char BK_CHAR[5] = {'S','D','G','H','J'};
    for (int b = 0; b < 5; b++) {
        int semi = BLACK_SEMI[b];
        bool held = app.pianoKeyHeld[semi];
        int wAfter = BLACK_AFTER_WHITE[b];
        float x0 = kX + (wAfter + 1) * wkW - bkW * 0.5f, y0 = kY;
        ImU32 col = held ? IM_COL32(60, 130, 220, 255) : IM_COL32(30, 30, 30, 255);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bkW, y0 + bkH), col, 2.f);
        dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + bkW, y0 + bkH), IM_COL32(0, 0, 0, 255), 2.f);
        char lbl[4]; snprintf(lbl, sizeof(lbl), "%c", BK_CHAR[b]);
        ImU32 lblCol = held ? IM_COL32(180, 220, 255, 255) : IM_COL32(160, 160, 160, 255);
        dl->AddText(ImVec2(x0 + bkW * 0.5f - 4.f, y0 + bkH - 16.f), lblCol, lbl);
    }

    // Mouse interaction
    ImVec2 mpos = ImGui::GetMousePos();
    bool mclick = ImGui::IsMouseClicked(0);
    bool mrel = ImGui::IsMouseReleased(0);

    if (mpos.y >= kY && mpos.y < kY + wkH) {
        int clickedNote = -1;
        if (mpos.y < kY + bkH) {
            for (int b = 0; b < 5; b++) {
                int wAfter = BLACK_AFTER_WHITE[b];
                float x0 = kX + (wAfter + 1) * wkW - bkW * 0.5f;
                if (mpos.x >= x0 && mpos.x < x0 + bkW) { clickedNote = BLACK_SEMI[b]; break; }
            }
        }
        if (clickedNote < 0) {
            int w = (int)((mpos.x - kX) / wkW);
            if (w >= 0 && w < NUM_WHITE) clickedNote = WHITE_TO_SEMI[w];
        }

        if (clickedNote >= 0 && mclick) {
            int midiNote = PIANO_BASE_NOTE + clickedNote;
            if (app.sound_running && app.synth_module) {
                xfm_voice_id voice = xfm_note_on(app.synth_module, midiNote, 0, 0);
                app.pianoVoice[clickedNote] = voice;
            }
            app.pianoHeldNote = midiNote;
        }
    }

    if (mrel && app.pianoHeldNote >= 0) {
        int heldSemi = app.pianoHeldNote - PIANO_BASE_NOTE;
        bool kbHeld = (heldSemi >= 0 && heldSemi < 12) ? app.pianoKeyHeld[heldSemi] : false;
        if (!kbHeld) {
            if (app.sound_running && app.synth_module && app.pianoVoice[heldSemi] >= 0) {
                xfm_note_off(app.synth_module, app.pianoVoice[heldSemi]);
                app.pianoVoice[heldSemi] = -1;
            }
            app.pianoHeldNote = -1;
        }
    }

    ImGui::End();
}

// =============================================================================
// Main Panel
// =============================================================================

static void drawPanel(AppState& app) {
    ImGuiIO& io = ImGui::GetIO();
    const float panelW = 420.f, panelH = (float)app.winH - 40.f;

    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - panelW) * 0.5f, (io.DisplaySize.y - panelH) * 0.5f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);

    ImGui::Begin("eggsfm synths", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);

    // Synth info
    ImGui::SeparatorText("Synths");
    static const char* SYNTH_NAMES[] = { "OPN (YM2612)", "OPL (YM3812)", "OPM (YM2151)" };
    ImGui::Text("Selected: %s", SYNTH_NAMES[app.selected_synth]);
    ImGui::Text("Patch: %s", app.synths[app.selected_synth].name);
    ImGui::Spacing();

    // Playback section
    ImGui::SeparatorText("Playback");
    ImGui::Spacing();

    if (!app.sound_running) {
        ImGui::TextDisabled("Synth engine stopped.");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.15f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.5f, 0.25f, 1.f));
        if (ImGui::Button("Start Sound System", ImVec2(-1, 0))) {
            if (!start_sound_system(app)) fprintf(stderr, "Failed to start sound system\n");
        }
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.9f, 0.4f, 1.f));
        ImGui::Text("●  RUNNING");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.08f, 0.08f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.6f, 0.12f, 0.12f, 1.f));
        if (ImGui::Button("Stop Sound System", ImVec2(-1, 0))) {
            stop_sound_system(app);
            app.pianoHeldNote = -1;
            for (int k = 0; k < 12; k++) { app.pianoKeyHeld[k] = false; app.pianoVoice[k] = -1; }
        }
        ImGui::PopStyleColor(2);
    }

    // Footer
    ImGui::Separator();
    ImGui::TextDisabled("%.0f fps  %.2f ms", app.displayFps, app.displayMs);
    ImGui::End();

    // Patch editor window
    if (app.show_editor) {
        char title[128];
        snprintf(title, sizeof(title), "Edit: %s (%s)", 
            app.synths[app.selected_synth].name, SYNTH_NAMES[app.selected_synth]);

        if (app.selected_synth == SYNTH_OPN) {
            if (!app.opn_editor.open) {
                app.opn_editor.open = true;
                app.opn_editor.init(app.synths[app.selected_synth].name, app.synths[app.selected_synth].opn);
            }
            app.opn_editor.drawWindow(title, ImVec2(520, 680));
            // Real-time update: copy editor patch to synth on every frame
            app.synths[app.selected_synth].opn = app.opn_editor.patch;
            if (!app.opn_editor.open) app.show_editor = false;
        } else if (app.selected_synth == SYNTH_OPL) {
            if (!app.opl_editor.open) {
                app.opl_editor.open = true;
                app.opl_editor.init(app.synths[app.selected_synth].name, app.synths[app.selected_synth].opl, false);
            }
            app.opl_editor.drawWindow(title, ImVec2(520, 680));
            // Real-time update
            app.synths[app.selected_synth].opl = app.opl_editor.patch;
            app.synths[app.selected_synth].opl.is4op = app.opl_editor.is4op;
            if (!app.opl_editor.open) app.show_editor = false;
        } else if (app.selected_synth == SYNTH_OPM) {
            if (!app.opm_editor.open) {
                app.opm_editor.open = true;
                app.opm_editor.init(app.synths[app.selected_synth].name, app.synths[app.selected_synth].opm);
            }
            app.opm_editor.drawWindow(title, ImVec2(520, 680));
            // Real-time update
            app.synths[app.selected_synth].opm = app.opm_editor.patch;
            if (!app.opm_editor.open) app.show_editor = false;
        }

        // Apply patch to chip every frame while editor is open (real-time)
        if (app.sound_running) {
            apply_current_patch(app);
            xfm_module_reload_patches(app.synth_module);  // Force reload on next note
        }
    }
}

// =============================================================================
// Main Tick
// =============================================================================

static void mainTick() {
    AppState& app = g_app;

    Uint32 now = SDL_GetTicks();
    float dt = (app.lastTick == 0) ? 0.016f : (now - app.lastTick) * 0.001f;
    app.lastTick = now;
    if (dt > 0.1f) dt = 0.1f;

    float fps = (dt > 0.0001f) ? (1.f / dt) : 9999.f;
    app.fpsSmooth = (app.fpsSmooth < 1.f) ? fps : app.fpsSmooth * 0.9f + fps * 0.1f;
    if (now - app.lastDisplay >= 500) {
        app.displayFps = app.fpsSmooth;
        app.displayMs = (app.fpsSmooth > 0.f) ? (1000.f / app.fpsSmooth) : 0.f;
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
            return;
        }
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            if (e.key.keysym.sym == SDLK_ESCAPE) {
                app.running = false;
#ifdef __EMSCRIPTEN__
                emscripten_cancel_main_loop();
#endif
                return;
            }
            // Piano keys
            for (int k = 0; k < 12; k++) {
                if (e.key.keysym.sym == PIANO_KEYS[k]) {
                    app.pianoKeyHeld[k] = true;
                    int midiNote = PIANO_BASE_NOTE + k;
                    if (app.sound_running && app.synth_module) {
                        xfm_voice_id voice = xfm_note_on(app.synth_module, midiNote, 0, 0);
                        app.pianoVoice[k] = voice;
                    }
                    break;
                }
            }
        }
        if (e.type == SDL_KEYUP) {
            for (int k = 0; k < 12; k++) {
                if (e.key.keysym.sym == PIANO_KEYS[k]) {
                    app.pianoKeyHeld[k] = false;
                    if (app.sound_running && app.synth_module && app.pianoVoice[k] >= 0) {
                        xfm_note_off(app.synth_module, app.pianoVoice[k]);
                        app.pianoVoice[k] = -1;
                    }
                    break;
                }
            }
        }
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_RESIZED) {
            app.winW = e.window.data1; app.winH = e.window.data2;
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
// Main
// =============================================================================

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError()); return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "1");

    AppState& app = g_app;
    app.window = SDL_CreateWindow("eggsfm synths — 3-Chip FM Test Bed",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, app.winW, app.winH,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);

    if (!app.window) { fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

    app.glCtx = SDL_GL_CreateContext(app.window);
    if (!app.glCtx) { fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError()); SDL_DestroyWindow(app.window); SDL_Quit(); return 1; }

    if (SDL_GL_SetSwapInterval(-1) != 0) if (SDL_GL_SetSwapInterval(1) != 0) SDL_GL_SetSwapInterval(0);
    glViewport(0, 0, app.winW, app.winH);

    app.aurora.init();
    app.imgui.init(app.window, app.glCtx);

    // Initialize piano state
    app.pianoHeldNote = -1;
    for (int k = 0; k < 12; k++) { app.pianoKeyHeld[k] = false; app.pianoVoice[k] = -1; }

    // Initialize 3 synths with default patches
    // OPN
    snprintf(app.synths[SYNTH_OPN].name, sizeof(app.synths[SYNTH_OPN].name), "INIT_OPN");
    app.synths[SYNTH_OPN].opn.ALG = 0; app.synths[SYNTH_OPN].opn.FB = 3; app.synths[SYNTH_OPN].opn.AMS = 0; app.synths[SYNTH_OPN].opn.FMS = 0;
    for (int i = 0; i < 4; i++) {
        app.synths[SYNTH_OPN].opn.op[i].DT = 0; app.synths[SYNTH_OPN].opn.op[i].MUL = 1;
        app.synths[SYNTH_OPN].opn.op[i].TL = (i < 2) ? 50 : 20;
        app.synths[SYNTH_OPN].opn.op[i].RS = 0; app.synths[SYNTH_OPN].opn.op[i].AR = 31; app.synths[SYNTH_OPN].opn.op[i].AM = 0;
        app.synths[SYNTH_OPN].opn.op[i].DR = 10; app.synths[SYNTH_OPN].opn.op[i].SR = 10;
        app.synths[SYNTH_OPN].opn.op[i].SL = 10; app.synths[SYNTH_OPN].opn.op[i].RR = 8; app.synths[SYNTH_OPN].opn.op[i].SSG = 0;
    }

    // OPL
    snprintf(app.synths[SYNTH_OPL].name, sizeof(app.synths[SYNTH_OPL].name), "INIT_OPL");
    app.synths[SYNTH_OPL].opl.alg = 0; app.synths[SYNTH_OPL].opl.fb = 3; app.synths[SYNTH_OPL].opl.is4op = false; app.synths[SYNTH_OPL].opl.waveform = 0;
    app.synths[SYNTH_OPL].opl.op[0].am = 0; app.synths[SYNTH_OPL].opl.op[0].vib = 0; app.synths[SYNTH_OPL].opl.op[0].eg = 0; app.synths[SYNTH_OPL].opl.op[0].wave = 0;
    app.synths[SYNTH_OPL].opl.op[0].ksr = 0; app.synths[SYNTH_OPL].opl.op[0].mul = 1; app.synths[SYNTH_OPL].opl.op[0].ksl = 0;
    app.synths[SYNTH_OPL].opl.op[0].tl = 50; app.synths[SYNTH_OPL].opl.op[0].ar = 15; app.synths[SYNTH_OPL].opl.op[0].dr = 10;
    app.synths[SYNTH_OPL].opl.op[0].sl = 10; app.synths[SYNTH_OPL].opl.op[0].rr = 8;
    app.synths[SYNTH_OPL].opl.op[1].am = 0; app.synths[SYNTH_OPL].opl.op[1].vib = 0; app.synths[SYNTH_OPL].opl.op[1].eg = 0; app.synths[SYNTH_OPL].opl.op[1].wave = 0;
    app.synths[SYNTH_OPL].opl.op[1].ksr = 0; app.synths[SYNTH_OPL].opl.op[1].mul = 1; app.synths[SYNTH_OPL].opl.op[1].ksl = 0;
    app.synths[SYNTH_OPL].opl.op[1].tl = 20; app.synths[SYNTH_OPL].opl.op[1].ar = 15; app.synths[SYNTH_OPL].opl.op[1].dr = 10;
    app.synths[SYNTH_OPL].opl.op[1].sl = 10; app.synths[SYNTH_OPL].opl.op[1].rr = 8;
    // Initialize OP3/OP4 for 4-op mode
    app.synths[SYNTH_OPL].opl.op[2] = app.synths[SYNTH_OPL].opl.op[0];
    app.synths[SYNTH_OPL].opl.op[3] = app.synths[SYNTH_OPL].opl.op[1];

    // OPM
    snprintf(app.synths[SYNTH_OPM].name, sizeof(app.synths[SYNTH_OPM].name), "INIT_OPM");
    app.synths[SYNTH_OPM].opm.alg = 0; app.synths[SYNTH_OPM].opm.fb = 3; app.synths[SYNTH_OPM].opm.pan = 3;
    app.synths[SYNTH_OPM].opm.lfo_freq = 0; app.synths[SYNTH_OPM].opm.lfo_wave = 0;
    for (int i = 0; i < 4; i++) {
        app.synths[SYNTH_OPM].opm.op[i].dt1 = 0; app.synths[SYNTH_OPM].opm.op[i].dt2 = 0; app.synths[SYNTH_OPM].opm.op[i].mul = 1;
        app.synths[SYNTH_OPM].opm.op[i].tl = (i < 2) ? 50 : 20;
        app.synths[SYNTH_OPM].opm.op[i].ks = 0;
        app.synths[SYNTH_OPM].opm.op[i].ar = 31; app.synths[SYNTH_OPM].opm.op[i].dr = 10;
        app.synths[SYNTH_OPM].opm.op[i].sr = 10; app.synths[SYNTH_OPM].opm.op[i].sl = 10;
        app.synths[SYNTH_OPM].opm.op[i].rr = 8; app.synths[SYNTH_OPM].opm.op[i].ssg = 0;
    }

    printf("=== eggsfm synths ===\n");
    printf("3 synths: OPN, OPL, OPM — each with its own patch\n");
    printf("Click 'Start Sound System' to enable audio.\n");
    printf("Keys: Z-M = piano  |  Esc = quit\n\n");

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(mainTick, 0, 1);
#else
    while (app.running) mainTick();
#endif

    stop_sound_system(app);
    app.aurora.shutdown();
    app.imgui.shutdown();
    SDL_GL_DeleteContext(app.glCtx);
    SDL_DestroyWindow(app.window);
    SDL_Quit();

    return 0;
}
