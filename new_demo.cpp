// =============================================================================
// new_demo.cpp — ingamefm: new API demo (SDL + Aurora shader)
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

    // Piano state
    int  pianoInstrument = 0;   // index into piano instrument list
    int  pianoHeldNote   = -1;  // MIDI note currently held, -1 if none
    bool pianoKeyHeld[12] = {}; // which of the 12 keys are pressed

    Uint32 lastTick    = 0;
    float  fpsSmooth   = 0.0f;
    float  displayFps  = 0.0f;
    float  displayMs   = 0.0f;
    Uint32 lastDisplay = 0;

    bool running = true;
};

static AppState g_app;

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

    ImGui::Begin("ingamefm new_demo", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);

    // =========================================================================
    // SECTION 1 — HARDWARE
    // =========================================================================
    ImGui::SeparatorText("Hardware");
    ImGui::Text("Chip: YM2612 / YM3438");
    ImGui::Text("Sample rate: 44100 Hz");
    ImGui::Text("Buffer: 256 samples");

    // =========================================================================
    // SECTION 2 — PLAYBACK
    // =========================================================================
    ImGui::SeparatorText("Playback");
    ImGui::Spacing();
    ImGui::TextDisabled("Engine stopped.");
    ImGui::Spacing();
    ImGui::TextDisabled("New API integration pending...");

    // =========================================================================
    // SECTION 3 — SONGS
    // =========================================================================
    ImGui::SeparatorText("Songs");
    ImGui::TextDisabled("No songs loaded.");

    // =========================================================================
    // SECTION 4 — CACHE
    // =========================================================================
    ImGui::SeparatorText("Cache");
    ImGui::TextDisabled("No cache recorded.");

    // =========================================================================
    // SECTION 5 — MIXER
    // =========================================================================
    ImGui::SeparatorText("Mixer");
    ImGui::TextDisabled("Mixer controls pending...");

    // =========================================================================
    // SECTION 6 — SOUND EFFECTS
    // =========================================================================
    ImGui::SeparatorText("Sound Effects");
    ImGui::TextDisabled("SFX controls pending...");

    // =========================================================================
    // SECTION 7 — INSTRUMENTS
    // =========================================================================
    ImGui::SeparatorText("Instruments");
    ImGui::TextDisabled("Instrument editors pending...");

    // =========================================================================
    // FOOTER — FPS
    // =========================================================================
    ImGui::Separator();
    ImGui::TextDisabled("%.0f fps  %.2f ms", app.displayFps, app.displayMs);

    ImGui::End();
}

// =============================================================================
// PIANO KEYBOARD
// =============================================================================

static void drawPiano(AppState& app)
{
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
    ImGui::TextDisabled("  Z S X D C V G B H N J M");

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
            // Visual feedback only for now - audio will be added with new API
            app.pianoHeldNote = midiNote;
        }
    }

    if (mrel && app.pianoHeldNote >= 0) {
        int heldSemi = app.pianoHeldNote - PIANO_BASE_NOTE;
        bool kbHeld = (heldSemi >= 0 && heldSemi < 12) ? app.pianoKeyHeld[heldSemi] : false;
        if (!kbHeld) {
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
            // Piano keys — visual feedback only (audio will be added with new API)
            for (int k = 0; k < 12; k++) {
                if (e.key.keysym.sym == PIANO_KEYS[k]) {
                    app.pianoKeyHeld[k] = true;
                    app.pianoHeldNote = PIANO_BASE_NOTE + k;
                    break;
                }
            }
        }
        if (e.type == SDL_KEYUP) {
            SDL_Keycode sym = e.key.keysym.sym;
            for (int k = 0; k < 12; k++) {
                if (sym == PIANO_KEYS[k]) {
                    app.pianoKeyHeld[k] = false;
                    // Release only if this was the held note and no other key is down
                    if (app.pianoHeldNote == PIANO_BASE_NOTE + k) {
                        bool anyHeld = false;
                        for (int j = 0; j < 12; j++) {
                            if (app.pianoKeyHeld[j]) { anyHeld = true; break; }
                        }
                        if (!anyHeld) {
                            app.pianoHeldNote = -1;
                        } else {
                            // Switch to the other held key
                            for (int j = 0; j < 12; j++) {
                                if (app.pianoKeyHeld[j]) {
                                    app.pianoHeldNote = PIANO_BASE_NOTE + j;
                                    break;
                                }
                            }
                        }
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
        "ingamefm new_demo — new API",
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

    printf("=== ingamefm new_demo ===\n");
    printf("Aurora shader running.\n");
    printf("Esc to quit.\n\n");

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(mainTick, 0, 1);
#else
    while (app.running) {
        mainTick();
    }
#endif

    app.aurora.shutdown();
    app.imgui.shutdown();
    SDL_GL_DeleteContext(app.glCtx);
    SDL_DestroyWindow(app.window);
    SDL_Quit();

    return 0;
}
