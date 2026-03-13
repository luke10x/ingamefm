// =============================================================================
// demo4.cpp — ingamefm YM2612 patch editor
//
// Aurora background + ImGui patch editor panel.
//
// Audio:
//   Channel 0 — long pad: C4, E4, A4, F4  (instrument 0x00)
//   Channel 1 — short staccato: same notes, offset rhythm  (instrument 0x00)
//   Both channels always use the instrument currently selected in the editor.
//   Selecting a different instrument or moving any slider takes effect live.
//
// Editor:
//   Dropdown — selects which catalogue patch is loaded as instrument 0x00
//   Global row — ALG, FB, AMS, FMS sliders
//   4 operator columns — all 11 params per operator
//   FPS counter shown inside the window
//
// No interactive keyboard/mouse notes. Esc to quit.
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
#include <string>

#include "ingamefm.h"
#include "ingamefm_patch_serializer.h"

// =============================================================================
// 2. GLSL
// =============================================================================

#define GLSL_VERSION "#version 300 es\n"

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
// 16 rows, tick_rate=60, speed=12  →  200ms/row
//
// Ch0 (inst 00) — long pad, one note every 4 rows: C4, E4, A4, F4
// Ch1 (inst 00) — short staccato, same notes offset by 2 rows, OFF after 1 row
//
// Both channels use instrument 0x00 so editing a single patch affects both.
// Volume 7F throughout — volume scaling can be explored via TL sliders.
//
// Column format: note(3) inst(2) vol(2)  =  7 chars
// =============================================================================

static const char* SONG =
"org.tildearrow.furnace - Pattern Data (16)\n"
"16\n"
/* row  0 */ "C-4007F|.......|" "\n"   // pad: C4 on
/* row  1 */ ".......|.......|" "\n"
/* row  2 */ ".......|C-4017F|" "\n"   // staccato: C4 on  (inst 01)
/* row  3 */ ".......|OFF....|" "\n"   //            C4 off
/* row  4 */ "E-4007F|.......|" "\n"   // pad: E4
/* row  5 */ ".......|.......|" "\n"
/* row  6 */ ".......|E-4017F|" "\n"   // staccato: E4
/* row  7 */ ".......|OFF....|" "\n"
/* row  8 */ "A-4007F|.......|" "\n"   // pad: A4
/* row  9 */ ".......|.......|" "\n"
/* row 10 */ ".......|A-4017F|" "\n"   // staccato: A4
/* row 11 */ ".......|OFF....|" "\n"
/* row 12 */ "F-4007F|.......|" "\n"   // pad: F4
/* row 13 */ ".......|.......|" "\n"
/* row 14 */ ".......|F-4017F|" "\n"   // staccato: F4
/* row 15 */ "OFF....|OFF....|" "\n";  // both off before loop

// =============================================================================
// 5. GL HELPERS
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
        fprintf(stderr, "Shader %s error:\n%s\n",
                type == GL_VERTEX_SHADER ? "vert" : "frag", log);
        exit(1);
    }
    return s;
}

static GLuint createProgram(const char* vert, const char* frag)
{
    GLuint vs = compileShader(GL_VERTEX_SHADER,   vert);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, frag);
    GLuint p  = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        fprintf(stderr, "Program link error:\n%s\n", log);
        exit(1);
    }
    return p;
}

// =============================================================================
// 6. AURORA RENDERER
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
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              5*sizeof(GLfloat), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                              5*sizeof(GLfloat), (void*)(3*sizeof(GLfloat)));
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
// 7. IMGUI MODULE  — stock dark style, no customisation
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
        ImGui::StyleColorsDark();   // stock — nothing more
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

    void processEvent(const SDL_Event& e) const
    {
        ImGui_ImplSDL2_ProcessEvent(&e);
    }

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
// 8. PATCH EDITOR STATE
//
// Holds one mutable working copy per catalogue patch.
// selectedIdx  — which patch is currently loaded as instrument 0x00
// =============================================================================

static constexpr int NUM_PATCHES = PATCH_CATALOGUE_SIZE;

// Name used when serializing — can be anything, parser ignores it
static constexpr const char* SERIALIZE_NAME = "MY_PATCH";

struct PatchEditorState
{
    YM2612Patch editPatches[NUM_PATCHES];
    int         selectedIdx = 0;
    bool        lfoEnable   = false;
    int         lfoFreq     = 0;   // 0-7

    // ── Tab state ────────────────────────────────────────────────────────────
    // 0 = Designer  1 = Code
    int  activeTab = 0;

    // ── Code editor ──────────────────────────────────────────────────────────
    static constexpr int CODE_BUF_SIZE = 8192;
    char codeBuf[CODE_BUF_SIZE] = {};

    // Snapshot of patch+lfo at the moment Code tab was entered (or last Apply).
    // "Restore" reverts to this.
    YM2612Patch savedPatch;
    bool        savedLfoEnable = false;
    int         savedLfoFreq   = 0;
    bool        hasSaved       = false; // false until first Code tab visit

    // Error state — cleared on successful apply
    bool codeHasError  = false;
    char codeError[256] = {};
    int  codeErrLine   = 0;
    int  codeErrCol    = 0;

    void init()
    {
        for (int i = 0; i < NUM_PATCHES; i++)
            editPatches[i] = *PATCH_CATALOGUE[i].patch;
    }

    YM2612Patch& current() { return editPatches[selectedIdx]; }

    // Regenerate codeBuf from current patch state.
    void refreshCodeBuf()
    {
        std::string s = IngameFMSerializer::serialize(
            current(), SERIALIZE_NAME,
            0, lfoEnable ? 1 : 0, lfoFreq);
        snprintf(codeBuf, CODE_BUF_SIZE, "%s", s.c_str());
        codeHasError = false;
        codeError[0] = '\0';
        codeErrLine  = 0;
        codeErrCol   = 0;
    }

    // Try to parse codeBuf into patch+lfo. Returns true on success.
    // On success: current patch and lfo fields are updated, savedPatch is snapped.
    // On failure: codeHasError/codeError/codeErrLine/codeErrCol are set.
    bool tryApplyCode()
    {
        YM2612Patch outPatch{};
        int  outBlock = 0, outLfoEn = 0, outLfoFreq = 0;
        std::string err;
        int errLine = 0, errCol = 0;

        bool ok = IngameFMSerializer::parse(
            std::string(codeBuf),
            outPatch, outBlock, outLfoEn, outLfoFreq,
            err, errLine, errCol);

        if (ok)
        {
            current()  = outPatch;
            lfoEnable  = (outLfoEn != 0);
            lfoFreq    = outLfoFreq;
            // Snap saved state
            savedPatch     = current();
            savedLfoEnable = lfoEnable;
            savedLfoFreq   = lfoFreq;
            hasSaved       = true;
            codeHasError   = false;
            codeError[0]   = '\0';
        }
        else
        {
            codeHasError = true;
            snprintf(codeError, sizeof(codeError), "%s", err.c_str());
            codeErrLine  = errLine;
            codeErrCol   = errCol;
        }
        return ok;
    }

    // Restore code buffer + patch to last saved snapshot, clear error.
    void restoreToSaved()
    {
        if (!hasSaved) return;
        current()  = savedPatch;
        lfoEnable  = savedLfoEnable;
        lfoFreq    = savedLfoFreq;
        refreshCodeBuf();   // regenerate from restored patch, clears error
        // re-snap so codeBuf matches saved exactly
        savedPatch     = current();
        savedLfoEnable = lfoEnable;
        savedLfoFreq   = lfoFreq;
    }
};

// =============================================================================
// 9. APPLICATION STATE
// =============================================================================

struct AppState
{
    // Window / GL
    SDL_Window*   window = nullptr;
    SDL_GLContext glCtx  = nullptr;
    int           winW   = 1024;
    int           winH   = 640;

    // Subsystems
    AuroraRenderer   aurora;
    ModImgui         imgui;
    PatchEditorState editor;

    // Audio
    SDL_AudioDeviceID audioDev = 0;
    IngameFMPlayer    player;

    // Mute state — one flag per channel
    bool muted[2] = { false, false };

    // Timing
    Uint32 lastTick  = 0;
    float  fpsSmooth = 0.0f;

    bool running = true;
};

static AppState g_app;

// =============================================================================
// 10. PATCH SYNC HELPERS
//
// pushPatch  — called on every slider/instrument change.
//              Always updates the editor's real patch.
//              Respects current mute state when deciding what the player sees.
//
// applyMute  — called when a mute checkbox toggles.
//              Re-syncs both the player map and the live chip registers.
//
// Mute strategy:
//   The player's audio callback calls commit_keyon() at every note trigger,
//   which calls chip->load_patch(patches_[instId], ch).  So the only reliable
//   way to keep a channel silent is to make patches_[instId] itself silent for
//   that channel's instrument — otherwise the real patch gets re-loaded on the
//   next note and the mute breaks.
//
//   We keep the real patch in editor.editPatches[] (UI always shows true values).
//   The player's slot 0x00 gets either the real patch or a TL=127 copy depending
//   on the per-channel mute flag.  Because both channels share instrument 0x00,
//   we need per-channel control: channel 0 uses instrument 0x00, channel 1 uses
//   instrument 0x01.  That way each can independently carry a real or silent patch.
//
//   initAudio() registers the same real patch under both 0x00 and 0x01.
//   pushPatch / applyMute keep them in sync with the mute state.
// =============================================================================

// Returns a copy of patch p with all operator TLs set to 127 (full attenuation).
static YM2612Patch makeSilent(const YM2612Patch& p)
{
    YM2612Patch s = p;
    for (int i = 0; i < 4; i++) s.op[i].TL = 127;
    return s;
}

// Sync instrument slots 0x00 (ch0) and 0x01 (ch1) to the player, and update
// the live chip registers for both channels.  Must be called under audio lock.
static void syncPatches_locked(AppState& app)
{
    const YM2612Patch& real   = app.editor.current();
    YM2612Patch        silent = makeSilent(real);

    // LFO — global chip register, apply every sync
    app.player.chip()->enable_lfo(app.editor.lfoEnable,
                                  (uint8_t)app.editor.lfoFreq);

    // Player map: ch0 → inst 0x00, ch1 → inst 0x01
    app.player.add_patch(0x00, app.muted[0] ? silent : real);
    app.player.add_patch(0x01, app.muted[1] ? silent : real);

    // Chip registers: update sounding voices immediately
    app.player.chip()->load_patch(app.muted[0] ? silent : real, 0);
    app.player.chip()->load_patch(app.muted[1] ? silent : real, 1);
}

static void pushPatch(AppState& app)
{
    SDL_LockAudioDevice(app.audioDev);
    syncPatches_locked(app);
    SDL_UnlockAudioDevice(app.audioDev);
}

// Same as pushPatch — kept as a named alias for call-site clarity.
static void applyMute(AppState& app) { pushPatch(app); }

// =============================================================================
// 11. VISUAL INDICATORS
// =============================================================================

// Operator colors (one per op, matching classic FM editor convention)
static const ImU32 OP_COLORS[4] = {
    IM_COL32(100, 200, 255, 255),  // OP1 — cyan-blue
    IM_COL32(120, 255, 140, 255),  // OP2 — green
    IM_COL32(255, 210,  80, 255),  // OP3 — amber
    IM_COL32(255, 110, 110, 255),  // OP4 — red
};

// Draw the algorithm topology diagram into a region starting at `p0` of size `sz`.
// Uses ImDrawList directly. Ops are small filled squares labeled 1-4.
// Carrier ops have an arrow going to an "out" marker on the right.
static void drawAlgoIndicator(ImDrawList* dl, ImVec2 p0, ImVec2 sz, int algo)
{
    float bx = p0.x, by = p0.y, uw = sz.x, uh = sz.y;
    const float opSz = 10.0f;   // half-size of op square
    const ImU32 lineCol = IM_COL32(180,180,180,200);
    const ImU32 outCol  = IM_COL32(100,255,160,255);
    const float th = 1.5f;

    // Draw a modulation line from (x1,y1) to (x2,y2)
    auto ln = [&](float x1, float y1, float x2, float y2){
        dl->AddLine(ImVec2(x1,y1), ImVec2(x2,y2), lineCol, th);
    };
    // Draw output line + filled dot
    auto out = [&](float x1, float y1, float x2, float /*y2*/){
        dl->AddLine(ImVec2(x1,y1), ImVec2(x2,y1), outCol, th);
        dl->AddCircleFilled(ImVec2(x2,y1), 3.5f, outCol);
    };
    // Draw one op box + number label
    auto drawOp = [&](float cx, float cy, int num, bool isFirstMod = false){
        ImU32 col = OP_COLORS[num-1];
        ImU32 bg  = IM_COL32(30,30,40,220);
        dl->AddRectFilled(ImVec2(cx-opSz,cy-opSz), ImVec2(cx+opSz,cy+opSz), bg);
        dl->AddRect      (ImVec2(cx-opSz,cy-opSz), ImVec2(cx+opSz,cy+opSz),
                         isFirstMod ? IM_COL32(255,255,255,180) : col, 2.f, 0, 1.5f);
        char buf[2] = { (char)('0'+num), 0 };
        // Centre the single digit
        ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(cx - ts.x*0.5f, cy - ts.y*0.5f), col, buf);
    };

    switch (algo) {
    case 0: {
        float sp=uw/4.8f, y=by+uh/2,
              x1=bx+sp*.5f, x2=bx+sp*1.5f, x3=bx+sp*2.5f, x4=bx+sp*3.5f, x5=bx+sp*4.2f;
        ln(x1+opSz,y, x2-opSz,y); ln(x2+opSz,y, x3-opSz,y); ln(x3+opSz,y, x4-opSz,y);
        out(x4+opSz,y, x5+opSz,y);
        drawOp(x1,y,1,true); drawOp(x2,y,2); drawOp(x3,y,3); drawOp(x4,y,4); break; }
    case 1: {
        float x1=bx+uw*.20f, x3=bx+uw*.40f, x4=bx+uw*.60f, x5=bx+uw*0.75f,
              y1=by+uh*.3f, y2=by+uh*.7f, ym=by+uh*.5f;
        ln(x1+opSz,y1, x3-opSz,ym); ln(x1+opSz,y2, x3-opSz,ym);
        ln(x3+opSz,ym, x4-opSz,ym); out(x4+opSz,ym, x5+opSz,ym);
        drawOp(x1,y1,1,true); drawOp(x1,y2,2); drawOp(x3,ym,3); drawOp(x4,ym,4); break; }
    case 2: {
        float x1=bx+uw*.20f, x3=bx+uw*.40f, x4=bx+uw*.60f, x5=bx+uw*.75f,
              y1=by+uh*.3f, y2=by+uh*.7f, ym=by+uh*.5f;
        ln(x1+opSz,y1, x3+opSz,y1); ln(x3+opSz,y1, x4-opSz,ym);
        ln(x1+opSz,y2, x3-opSz,y2); ln(x3+opSz,y2, x4-opSz,ym);
        out(x4+opSz,ym, x5+opSz,ym);
        drawOp(x1,y1,1,true); drawOp(x1,y2,2); drawOp(x3,y2,3); drawOp(x4,ym,4); break; }
    case 3: {
        float x1=bx+uw*.2f, x2=bx+uw*.4f, x4=bx+uw*.6f, x5=bx+uw*.75f,
              y1=by+uh*.3f, y3=by+uh*.7f, ym=by+uh*.5f;
        ln(x1+opSz,y1, x2-opSz,y1); ln(x2+opSz,y1, x4-opSz,ym);
        ln(x1+opSz,y3, x2+opSz,y3); ln(x2+opSz,y3, x4-opSz,ym);
        out(x4+opSz,ym, x5+opSz,ym);
        drawOp(x1,y1,1,true); drawOp(x2,y1,2); drawOp(x1,y3,3); drawOp(x4,ym,4); break; }
    case 4: {
        float x1=bx+uw*.2f, x2=bx+uw*.5f, x3=bx+uw*.75f,
              y1=by+uh*.33f, ym=by+uh*.5f, y2=by+uh*.66f;
        ln(x1+opSz,y1, x2-opSz,y1); out(x2+opSz,y1, x3+opSz,ym);
        ln(x1+opSz,y2, x2-opSz,y2); out(x2+opSz,y2, x3+opSz,ym);
        drawOp(x1,y1,1,true); drawOp(x2,y1,2); drawOp(x1,y2,3); drawOp(x2,y2,4); break; }
    case 5: {
        float x1=bx+uw*.25f, x2=bx+uw*.5f, x3=bx+uw*.75f,
              y1=by+uh*.25f, y2=by+uh*.5f, y3=by+uh*.75f;
        ln(x1+opSz,y2, x2-opSz,y1); out(x2+opSz,y1, x3+opSz,y2);
        ln(x1+opSz,y2, x2-opSz,y2); out(x2+opSz,y2, x3+opSz,y2);
        ln(x1+opSz,y2, x2-opSz,y3); out(x2+opSz,y3, x3+opSz,y2);
        drawOp(x1,y2,1,true); drawOp(x2,y1,2); drawOp(x2,y2,3); drawOp(x2,y3,4); break; }
    case 6: {
        float x1=bx+uw*.25f, x2=bx+uw*.5f, x3=bx+uw*.75f,
              y1=by+uh*.25f, y2=by+uh*.5f, y3=by+uh*.75f;
        ln(x1+opSz,y1, x2-opSz,y1); out(x2+opSz,y1, x3+opSz,y2);
        out(x2+opSz,y2, x3+opSz,y2); out(x2+opSz,y3, x3+opSz,y2);
        drawOp(x1,y1,1,true); drawOp(x2,y1,2); drawOp(x2,y2,3); drawOp(x2,y3,4); break; }
    case 7: {
        float sp=uw/5.f, y=by+uh*.4f, yOut=by+uh*.75f, xOut=bx+uw*.5f;
        float x1=bx+sp, x2=bx+sp*2, x3=bx+sp*3, x4=bx+sp*4;
        dl->AddLine(ImVec2(x1,y+opSz), ImVec2(xOut,yOut), lineCol, th);
        dl->AddLine(ImVec2(x2,y+opSz), ImVec2(xOut,yOut), lineCol, th);
        dl->AddLine(ImVec2(x3,y+opSz), ImVec2(xOut,yOut), lineCol, th);
        dl->AddLine(ImVec2(x4,y+opSz), ImVec2(xOut,yOut), lineCol, th);
        dl->AddCircleFilled(ImVec2(xOut,yOut), 4.f, outCol);
        drawOp(x1,y,1,true); drawOp(x2,y,2); drawOp(x3,y,3); drawOp(x4,y,4); break; }
    default: break;
    }
}

// Draw SSG-EG waveform indicator into region (p0, sz).
// ssg: 0=off, 1-8=modes 0-7 (matching our field convention).
// Waveforms translated from JUCE path code — same shapes.
static void drawSsgIndicator(ImDrawList* dl, ImVec2 p0, ImVec2 sz, int ssg, ImU32 col)
{
    float x1 = p0.x + 2, x2 = p0.x + sz.x - 2;
    float yBot = p0.y + sz.y - 2, yTop = p0.y + 2;
    float w = x2 - x1;

    if (ssg == 0) {
        // Off — draw a dim dash
        float ym = p0.y + sz.y * 0.5f;
        dl->AddLine(ImVec2(x1, ym), ImVec2(x2, ym), IM_COL32(80,80,80,200), 1.5f);
        return;
    }

    int mode = ssg - 1;   // convert to 0-7
    float sw = w / 4.f;   // segment width

    // Build polyline points
    ImVec2 pts[16];
    int    npts = 0;
    auto P = [&](float x, float y){ pts[npts++] = ImVec2(x, y); };

    switch (mode) {
    case 0: // sawtooth down repeat
        P(x1,yBot); P(x1,yTop); P(x1+sw,yBot); P(x1+sw,yTop);
        P(x1+sw*2,yBot); P(x1+sw*2,yTop); P(x1+sw*3,yBot); P(x1+sw*3,yTop);
        P(x1+sw*4,yBot); break;
    case 1: // single down
        P(x1,yBot); P(x1,yTop); P(x1+sw,yBot); P(x1+sw*4,yBot); break;
    case 2: // down-up alternating
        P(x1,yBot); P(x1,yTop); P(x1+sw,yBot);
        P(x1+sw*2,yTop); P(x1+sw*3,yBot); P(x1+sw*4,yTop); break;
    case 3: // down then hold high
        P(x1,yBot); P(x1,yTop); P(x1+sw,yBot); P(x1+sw,yTop); P(x1+sw*4,yTop); break;
    case 4: // sawtooth up repeat
        P(x1,yBot); P(x1+sw,yTop); P(x1+sw,yBot); P(x1+sw*2,yTop);
        P(x1+sw*2,yBot); P(x1+sw*3,yTop); P(x1+sw*3,yBot); P(x1+sw*4,yTop); break;
    case 5: // single up then hold
        P(x1,yBot); P(x1+sw,yTop); P(x1+sw*4,yTop); break;
    case 6: // up-down alternating
        P(x1,yBot); P(x1+sw,yTop); P(x1+sw*2,yBot);
        P(x1+sw*3,yTop); P(x1+sw*4,yBot); break;
    case 7: // up then hold low
        P(x1,yBot); P(x1+sw,yTop); P(x1+sw,yBot); P(x1+sw*4,yBot); break;
    }

    if (npts >= 2)
        dl->AddPolyline(pts, npts, col, 0, 1.5f);
}

// Draw ADSR envelope visualizer into region (p0, sz) for one operator.
static void drawEnvelopeIndicator(ImDrawList* dl, ImVec2 p0, ImVec2 sz,
                                  const YM2612Operator& o, ImU32 col)
{
    float x0 = p0.x + 1, y0 = p0.y + 1;
    float w  = sz.x - 2, h = sz.y - 2;
    float yTop = y0 + 2, yBot = y0 + h - 2;

    // Dim background
    dl->AddRectFilled(p0, ImVec2(p0.x+sz.x, p0.y+sz.y), IM_COL32(20,20,30,180));
    dl->AddRect(p0, ImVec2(p0.x+sz.x, p0.y+sz.y),
                (col & 0x00FFFFFFu) | IM_COL32(0,0,0,80));

    // Rate→width: high rate = near-vertical = narrow segment.
    // maxRate must match the hardware range (31 for AR/DR/SR, 15 for RR).
    auto rateW = [&](int rate, int maxRate, float base) -> float {
        float r = rate / (float)maxRate;
        float curve = r*r*r;
        return base * (1.f - curve * 0.95f);
    };

    float sl = 1.f - o.SL / 15.f,  // SL 0=loud→top, 15=silent→bottom
          sr = o.SR / 31.f;

    float ySL   = yTop + (yBot - yTop) * (1.f - sl);
    float wAtk  = rateW(o.AR, 31, w * 0.18f);
    float wDec  = rateW(o.DR, 31, w * 0.18f);
    float wSus  = (w * 0.40f) * (1.f - sr) + 3.f * sr;
    float wRel  = rateW(o.RR, 15, w * 0.28f);

    // Envelope line
    float cx = x0;
    ImVec2 env[6];
    env[0] = ImVec2(cx, yBot);   cx += wAtk;
    env[1] = ImVec2(cx, yTop);   cx += wDec;
    env[2] = ImVec2(cx, ySL);    cx += wSus;
    env[3] = ImVec2(cx, ySL);
    env[4] = ImVec2(cx + wRel, yBot);

    // Filled area under envelope
    ImVec2 fill[8];
    int fi = 0;
    for (int i = 0; i < 5; i++) fill[fi++] = env[i];
    fill[fi++] = ImVec2(env[4].x, yBot);
    fill[fi++] = ImVec2(x0, yBot);
    ImU32 fillCol = (col & 0x00FFFFFF) | IM_COL32(0,0,0,40);
    dl->AddConvexPolyFilled(fill, fi, fillCol);

    // Envelope outline
    dl->AddPolyline(env, 5, col, 0, 1.5f);

    // Sustain level dashed line
    ImU32 dashCol = (col & 0x00FFFFFF) | IM_COL32(0,0,0,70);
    float xd = x0;
    while (xd < x0 + w) {
        dl->AddLine(ImVec2(xd, ySL), ImVec2(xd+3.f, ySL), dashCol, 1.f);
        xd += 6.f;
    }

    // "EG" label
    dl->AddText(ImVec2(x0+2, y0+1), IM_COL32(120,120,120,180), "EG");
}

// =============================================================================
// 12. IMGUI PATCH EDITOR
// =============================================================================

static bool drawPatchEditor(AppState& app, float fps)
{
    bool changed = false;
    PatchEditorState& ed = app.editor;

    ImGui::SetNextWindowPos( ImVec2(12.0f, 12.0f), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(700.0f, (float)app.winH - 24.0f), ImGuiCond_Once);

    if (!ImGui::Begin("YM2612 Patch Editor"))
    {
        ImGui::End();
        return false;
    }

    // ── FPS ──────────────────────────────────────────────────────────────────
    ImGui::Text("%.0f FPS", fps);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Instrument dropdown (always visible) ─────────────────────────────────
    ImGui::Text("Instrument");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);

    if (ImGui::BeginCombo("##inst", PATCH_CATALOGUE[ed.selectedIdx].name))
    {
        for (int i = 0; i < NUM_PATCHES; i++)
        {
            bool sel = (i == ed.selectedIdx);
            if (ImGui::Selectable(PATCH_CATALOGUE[i].name, sel))
            { ed.selectedIdx = i; changed = true; }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();

    // ── Tab bar ───────────────────────────────────────────────────────────────
    // We manage activeTab manually so we can block the Designer tab when the
    // code editor has unparsed/errored content.

    // Render tab buttons manually so we can intercept clicks.
    const ImVec4 tabActive   = ImGui::GetStyleColorVec4(ImGuiCol_TabActive);
    const ImVec4 tabInactive = ImGui::GetStyleColorVec4(ImGuiCol_Tab);

    auto drawTab = [&](const char* label, int idx) -> bool
    {
        bool isActive = (ed.activeTab == idx);
        ImGui::PushStyleColor(ImGuiCol_Button,
            isActive ? tabActive : tabInactive);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            ImGui::GetStyleColorVec4(ImGuiCol_TabHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            ImGui::GetStyleColorVec4(ImGuiCol_TabActive));
        bool clicked = ImGui::Button(label);
        ImGui::PopStyleColor(3);
        return clicked;
    };

    bool wantDesigner = drawTab("Designer", 0);
    ImGui::SameLine();
    bool wantCode     = drawTab("Code",     1);
    ImGui::Separator();
    ImGui::Spacing();

    // ── Tab switch logic ──────────────────────────────────────────────────────
    if (wantDesigner && ed.activeTab == 1)
    {
        // Attempt to leave Code tab → must parse successfully
        if (ed.tryApplyCode())
        {
            ed.activeTab = 0;
            changed = true;   // patch may have changed
        }
        // If parse failed, codeHasError is set and we stay on Code tab
    }
    if (wantCode && ed.activeTab == 0)
    {
        // Enter Code tab: regenerate buffer, snapshot saved state
        ed.savedPatch     = ed.current();
        ed.savedLfoEnable = ed.lfoEnable;
        ed.savedLfoFreq   = ed.lfoFreq;
        ed.hasSaved       = true;
        ed.refreshCodeBuf();
        ed.activeTab      = 1;
    }

    // ── DESIGNER TAB ─────────────────────────────────────────────────────────
    if (ed.activeTab == 0)
    {
        YM2612Patch& p = ed.current();
        ImGui::Text("Global");
        ImGui::Spacing();

        {
            float avail   = ImGui::GetContentRegionAvail().x;
            float sp      = ImGui::GetStyle().ItemSpacing.x;
            float labelW  = ImGui::CalcTextSize("FMS").x + sp;
            float sliderW = (avail - labelW*4.f - sp*3.f) / 4.f;
            if (sliderW < 30.f) sliderW = 30.f;

            ImGui::Text("ALG"); ImGui::SameLine();
            ImGui::SetNextItemWidth(sliderW);
            if (ImGui::SliderInt("##ALGg", &p.ALG, 0, 7)) changed = true;

            ImGui::SameLine();
            ImGui::Text("FB"); ImGui::SameLine();
            ImGui::SetNextItemWidth(sliderW);
            if (ImGui::SliderInt("##FBg",  &p.FB,  0, 7)) changed = true;

            ImGui::SameLine();
            ImGui::Text("AMS"); ImGui::SameLine();
            ImGui::SetNextItemWidth(sliderW);
            if (ImGui::SliderInt("##AMSg", &p.AMS, 0, 3)) changed = true;

            ImGui::SameLine();
            ImGui::Text("FMS"); ImGui::SameLine();
            ImGui::SetNextItemWidth(sliderW);
            if (ImGui::SliderInt("##FMSg", &p.FMS, 0, 7)) changed = true;
        }

        // ALG diagram
        {
            ImVec2 cSize(ImGui::GetContentRegionAvail().x, 72.f);
            ImVec2 cPos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##algcanvas", cSize);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(cPos, ImVec2(cPos.x+cSize.x, cPos.y+cSize.y),
                              IM_COL32(20,20,30,180), 4.f);
            char algBuf[16];
            snprintf(algBuf, sizeof(algBuf), "ALG %d", p.ALG);
            dl->AddText(ImVec2(cPos.x+4, cPos.y+3), IM_COL32(180,180,180,200), algBuf);
            drawAlgoIndicator(dl, ImVec2(cPos.x, cPos.y+14.f),
                              ImVec2(cSize.x, cSize.y-14.f), p.ALG);
        }

        ImGui::Spacing();

        // LFO
        {
            static const char* LFO_LABELS[] = {
                "3.82 Hz","5.33 Hz","5.77 Hz","6.11 Hz",
                "6.60 Hz","9.23 Hz","46.11 Hz","69.22 Hz"
            };
            if (ImGui::Checkbox("LFO##lfoEn", &ed.lfoEnable)) changed = true;
            ImGui::SameLine();
            ImGui::BeginDisabled(!ed.lfoEnable);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::BeginCombo("##lfoFreq", LFO_LABELS[ed.lfoFreq]))
            {
                for (int i = 0; i < 8; i++) {
                    bool sel = (i == ed.lfoFreq);
                    if (ImGui::Selectable(LFO_LABELS[i], sel))
                    { ed.lfoFreq = i; changed = true; }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Operator columns
        ImGui::Text("Operators");
        ImGui::Spacing();

        if (ImGui::BeginTable("##ops", 4,
                ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableSetupColumn("OP 1");
            ImGui::TableSetupColumn("OP 2");
            ImGui::TableSetupColumn("OP 3");
            ImGui::TableSetupColumn("OP 4");
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();

#define OPS(lbl, fld, lo, hi) \
    ImGui::Text(lbl); ImGui::SameLine(); \
    ImGui::SetNextItemWidth(-1); \
    snprintf(s, sizeof(s), "##%s%d", lbl, op); \
    if (ImGui::SliderInt(s, &o.fld, lo, hi)) changed = true;

            for (int op = 0; op < 4; op++)
            {
                ImGui::TableSetColumnIndex(op);
                YM2612Operator& o = p.op[op];
                char s[16];
                ImU32 opCol = OP_COLORS[op];

                // Envelope visualizer
                {
                    float envH = 42.f;
                    float envW = ImGui::GetContentRegionAvail().x;
                    ImVec2 envPos = ImGui::GetCursorScreenPos();
                    snprintf(s, sizeof(s), "##env%d", op);
                    ImGui::InvisibleButton(s, ImVec2(envW, envH));
                    drawEnvelopeIndicator(ImGui::GetWindowDrawList(),
                                          envPos, ImVec2(envW, envH), o, opCol);
                }

                OPS("TL",  TL,   0, 127)

                // SSG indicator
                OPS("SSG", SSG,  0,   8)
                {
                    float ssgH = 18.f;
                    float ssgW = ImGui::GetContentRegionAvail().x;
                    ImVec2 ssgPos = ImGui::GetCursorScreenPos();
                    snprintf(s, sizeof(s), "##ssg%d", op);
                    ImGui::InvisibleButton(s, ImVec2(ssgW, ssgH));
                    drawSsgIndicator(ImGui::GetWindowDrawList(),
                                     ssgPos, ImVec2(ssgW, ssgH), o.SSG, opCol);
                }

                OPS("AR",  AR,   0,  31)
                OPS("DR",  DR,   0,  31)
                OPS("SR",  SR,   0,  31)
                OPS("SL",  SL,   0,  15)
                OPS("RR",  RR,   0,  15)
                OPS("MUL", MUL,  0,  15)
                OPS("DT",  DT,  -3,   3)
                OPS("RS",  RS,   0,   3)
                OPS("AM",  AM,   0,   1)
            }
#undef OPS
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Mute
        bool muteChanged = false;
        muteChanged |= ImGui::Checkbox("Mute Ch0 (pad)",      &app.muted[0]);
        ImGui::SameLine();
        muteChanged |= ImGui::Checkbox("Mute Ch1 (staccato)", &app.muted[1]);
        if (muteChanged) changed = true;

        ImGui::Spacing();
        ImGui::TextDisabled("Ch0: pad (inst 0x00)  |  Ch1: staccato (inst 0x01)  |  editor controls both");
    }

    // ── CODE TAB ─────────────────────────────────────────────────────────────
    if (ed.activeTab == 1)
    {
        // Reserve space: leave room at bottom for buttons + error line
        const float bottomH = ImGui::GetFrameHeightWithSpacing() * 2.f + 8.f;
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float  textH = avail.y - bottomH;
        if (textH < 60.f) textH = 60.f;

        // Highlight error line inside the text box via a draw-list overlay.
        // We do this by drawing a tinted rect behind the matching line before
        // InputTextMultiline renders (using the window draw list, drawn after).
        ImVec2 textPos = ImGui::GetCursorScreenPos();

        ImGui::InputTextMultiline(
            "##code",
            ed.codeBuf, PatchEditorState::CODE_BUF_SIZE,
            ImVec2(avail.x, textH),
            ImGuiInputTextFlags_AllowTabInput);

        // If there is an error, tint the error line red inside the text box
        if (ed.codeHasError && ed.codeErrLine > 0)
        {
            // Approximate line height from font size
            float lineH = ImGui::GetTextLineHeight();
            float lineY = textPos.y + (ed.codeErrLine - 1) * lineH;
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(textPos.x, lineY),
                ImVec2(textPos.x + avail.x, lineY + lineH),
                IM_COL32(200, 40, 40, 60));
        }

        ImGui::Spacing();

        // ── Buttons ───────────────────────────────────────────────────────────
        // [Apply & Close] attempts parse, switches to Designer if ok.
        // [Restore]       reverts to saved snapshot, switches to Designer.
        float btnW = (avail.x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

        if (ImGui::Button("Apply & Close", ImVec2(btnW, 0)))
        {
            if (ed.tryApplyCode())
            {
                ed.activeTab = 0;
                changed = true;
            }
            // else error is shown below
        }

        ImGui::SameLine();

        // Restore is only available if we have a saved snapshot
        ImGui::BeginDisabled(!ed.hasSaved);
        if (ImGui::Button("Restore", ImVec2(btnW, 0)))
        {
            ed.restoreToSaved();
            ed.activeTab = 0;
            changed = true;
        }
        ImGui::EndDisabled();

        // ── Error display ─────────────────────────────────────────────────────
        if (ed.codeHasError)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 90, 90, 255));
            if (ed.codeErrLine > 0)
            {
                char loc[64];
                snprintf(loc, sizeof(loc), "Line %d col %d: ", ed.codeErrLine, ed.codeErrCol);
                ImGui::TextUnformatted(loc);
                ImGui::SameLine();
            }
            ImGui::TextWrapped("%s", ed.codeError);
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::TextDisabled("Edit C++ patch code, then Apply & Close — or Restore to last save.");
        }
    }

    ImGui::End();
    return changed;
}

// =============================================================================
// 13. PER-FRAME TICK
// =============================================================================

static void mainTick()
{
    AppState& app = g_app;

    // ── Timing ───────────────────────────────────────────────────────────────
    Uint32 now = SDL_GetTicks();
    float  dt  = (app.lastTick == 0) ? 0.016f
                                     : (now - app.lastTick) * 0.001f;
    app.lastTick = now;
    if (dt > 0.1f) dt = 0.1f;

    float fps    = (dt > 0.0001f) ? (1.0f / dt) : 9999.0f;
    app.fpsSmooth = (app.fpsSmooth < 1.0f)
                  ? fps
                  : app.fpsSmooth * 0.9f + fps * 0.1f;

    // ── Events ───────────────────────────────────────────────────────────────
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

        if (e.type == SDL_KEYDOWN && !e.key.repeat &&
            e.key.keysym.sym == SDLK_ESCAPE)
        {
            app.running = false;
#ifdef __EMSCRIPTEN__
            emscripten_cancel_main_loop();
#endif
        }

        if (e.type == SDL_WINDOWEVENT &&
            e.window.event == SDL_WINDOWEVENT_RESIZED)
        {
            app.winW = e.window.data1;
            app.winH = e.window.data2;
            glViewport(0, 0, app.winW, app.winH);
        }
    }

    // ── Aurora ───────────────────────────────────────────────────────────────
    glClearColor(0.0f, 0.0f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    app.aurora.render(dt);

    // ── ImGui ────────────────────────────────────────────────────────────────
    app.imgui.newFrame();
    bool changed = drawPatchEditor(app, app.fpsSmooth);
    app.imgui.render();

    // Push patch to player + chip immediately if anything changed
    if (changed)
        pushPatch(app);

    SDL_GL_SwapWindow(app.window);
}

// =============================================================================
// 13. INIT
// =============================================================================

static bool initVideo(AppState& app)
{
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "1");

    app.window = SDL_CreateWindow(
        "ingamefm patch editor",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        app.winW, app.winH,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN
    );
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
        app.player.set_song(SONG, /*tick_rate=*/60, /*speed=*/12);
        // Ch0 uses inst 0x00, ch1 uses inst 0x01 — both start as the same patch.
        // syncPatches_locked() keeps them in sync with the mute state at runtime.
        app.player.add_patch(0x00, app.editor.current());
        app.player.add_patch(0x01, app.editor.current());
    } catch (const std::exception& ex) {
        fprintf(stderr, "Song parse error: %s\n", ex.what());
        return false;
    }

    SDL_AudioSpec desired{};
    desired.freq     = IngameFMPlayer::SAMPLE_RATE;
    desired.format   = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples  = 512;
    desired.callback = IngameFMPlayer::s_audio_callback;
    desired.userdata = &app.player;

    SDL_AudioSpec obtained{};
    app.audioDev = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (app.audioDev == 0) { fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError()); return false; }

    app.player.start(app.audioDev, /*loop=*/true);
    SDL_PauseAudioDevice(app.audioDev, 0);
    return true;
}

// =============================================================================
// 14. MAIN
// =============================================================================

int main(int /*argc*/, char** /*argv*/)
{
    AppState& app = g_app;

    if (!initVideo(app)) return 1;

    app.editor.init();   // must be before initAudio (uses editor.current())

    if (!initAudio(app)) return 1;

    app.aurora.init();
    app.imgui.init(app.window, app.glCtx);

    printf("=== ingamefm patch editor ===\n");
    printf("Looping C-E-A-F pad + staccato.  Edit sliders to change sound live.\n");
    printf("Esc to quit.\n\n");

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
// Assumes imgui/ is a sibling directory containing imgui.h, imgui.cpp,
// imgui_draw.cpp, imgui_tables.cpp, imgui_widgets.cpp, and backends/.
//
// ── NATIVE (macOS) ───────────────────────────────────────────────────────────
//
//   IMGUI=../imgui
//   YMFM=../my-ym2612-plugin/build/_deps/ymfm-src/src
//   SDL_INC=../bowling/3rdparty/SDL/include
//   SDL_LIB=../bowling/build/macos/usr/lib/libSDL2.a
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
//       demo4.cpp \
//       -framework Cocoa -framework IOKit -framework CoreVideo \
//       -framework CoreAudio -framework AudioToolbox \
//       -framework ForceFeedback -framework Carbon \
//       -framework Metal -framework GameController -framework CoreHaptics \
//       -lobjc -o demo4 && ./demo4
//
// ── EMSCRIPTEN ───────────────────────────────────────────────────────────────
//
//   IMGUI=../imgui
//   YMFM=../my-ym2612-plugin/build/_deps/ymfm-src/src
//
//   em++ -std=c++17 -O2 \
//       -I$YMFM -I$IMGUI \
//       $YMFM/ymfm_misc.cpp $YMFM/ymfm_adpcm.cpp \
//       $YMFM/ymfm_ssg.cpp  $YMFM/ymfm_opn.cpp \
//       $IMGUI/imgui.cpp $IMGUI/imgui_draw.cpp \
//       $IMGUI/imgui_tables.cpp $IMGUI/imgui_widgets.cpp \
//       $IMGUI/backends/imgui_impl_sdl2.cpp \
//       $IMGUI/backends/imgui_impl_opengl3.cpp \
//       demo4.cpp \
//       -s USE_SDL=2 \
//       -s FULL_ES3=1 \
//       -s MIN_WEBGL_VERSION=2 \
//       -s MAX_WEBGL_VERSION=2 \
//       -s ALLOW_MEMORY_GROWTH=1 \
//       -s ASYNCIFY \
//       --shell-file shell4.html \
//       -o demo4.html
//
//   python3 -m http.server  →  http://localhost:8000/demo4.html
// =============================================================================
