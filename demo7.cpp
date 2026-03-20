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
// 5 channels, tick_rate=60, speed=6 -> 100ms/row, 32 rows = 3.2s loop
// No "org.tildearrow.furnace" header — parser handles this.
// Column format: note(3)+inst(2)+vol(2)+effect_dots(4) = 11 chars each.
//
// Instruments:
//   00 = PATCH_SLAP_BASS  (ch0 bass line)
//   01 = PATCH_FLUTE      (ch2-4 chord voices)
//   02 = PATCH_HIHAT      (ch1 hi-hat, very quiet)
// =============================================================================

static const char* SONG =
"32\n"
"C-6007F....|C#60206....|C-6017F....|E-6017F....|G-6017F....\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#702......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#702......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#702......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"G-600......|C#602......|G-601......|B-601......|D-601......\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#702......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#702......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|C#602......|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n"
"...........|...........|...........|...........|...........\n";

// =============================================================================
// 5. SFX PATTERNS
// =============================================================================

static const char* SFX_JUMP =
"5\n"
"C-5127F\n"
"G-5127F\n"
"C-6127F\n"
"OFF....\n"
".......\n";

static const char* SFX_COIN =
"4\n"
"A-6147F\n"
"E-7147F\n"
"OFF....\n"
".......\n";

static const char* SFX_ALARM =
"6\n"
"D-6117F\n"
"OFF....\n"
"A-5117F\n"
"OFF....\n"
".......\n"
".......\n";

static const char* SFX_FANFARE =
"10\n"
"C-5127F\n"
"E-5127F\n"
"G-5127F\n"
"C-6127F\n"
"OFF....\n"
".......\n"
".......\n"
".......\n"
".......\n"
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
//
// Encapsulates IngameFMPlayer + SDL audio device.
// init()     — opens device, loads patches, defines song/SFX, starts playback
// teardown() — stops playback, closes device, resets player
// =============================================================================

struct SoundSystem
{
    IngameFMPlayer  player;
    SDL_AudioDeviceID dev = 0;
    bool            running = false;
    std::string     lastError;

    // Settings (set before calling init)
    int sampleRate  = 44100;
    int bufferFrames = 256;  // must be power-of-2, >= 256 for Web Audio API

    bool init()
    {
        if(running) teardown();
        lastError.clear();

        // Clamp bufferFrames to power-of-two in [256..4096]
        int bf = bufferFrames;
        if(bf < 256) bf = 256;
        // Find next power of two
        int p = 256;
        while(p < bf && p < 4096) p <<= 1;
        bf = p;

        player.set_sample_rate(sampleRate);

        // Patches
        // inst 00 — bass (SLAP_BASS)
        player.add_patch(0x00, PATCH_SLAP_BASS);
        // inst 01 — flute chord voices
        player.add_patch(0x01, PATCH_FLUTE);
        // inst 02 — hi-hat (very quiet, used at vol 06 in pattern)
        player.add_patch(0x02, PATCH_HIHAT);
        // inst 10,11,12 — SFX patches
        // SFX patch IDs match what the patterns reference:
        // JUMP/FANFARE use inst 0x12 = CLANG
        // ALARM        uses inst 0x11 = HIHAT (metallic noise)
        // COIN         uses inst 0x14 = ELECTRIC_BASS (bright pluck)
        player.add_patch(0x11, PATCH_HIHAT);
        player.add_patch(0x12, PATCH_CLANG);
        player.add_patch(0x14, PATCH_ELECTRIC_BASS);

        player.sfx_set_voices(3);

        // Define song
        try {
            player.song_define(1, SONG, 60, 6);
        } catch(const std::exception& e) {
            lastError = std::string("song_define failed: ") + e.what();
            return false;
        }

        // Define SFX — speed=3 gives ~50ms/row for snappy game feel.
        // SFX patterns use inst 0x12 (CLANG) and 0x14 (HIHAT) — both registered above.
        try {
            player.sfx_define(SFX_ID_JUMP,    SFX_JUMP,    60, 3);
            player.sfx_define(SFX_ID_COIN,    SFX_COIN,    60, 3);
            player.sfx_define(SFX_ID_ALARM,   SFX_ALARM,   60, 3);
            player.sfx_define(SFX_ID_FANFARE, SFX_FANFARE, 60, 3);
        } catch(const std::exception& e) {
            lastError = std::string("sfx_define failed: ") + e.what();
            return false;
        }

        // Open SDL audio device
        SDL_AudioSpec desired{};
        desired.freq     = sampleRate;
        desired.format   = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples  = static_cast<Uint16>(bf);
        desired.callback = IngameFMPlayer::s_audio_callback;
        desired.userdata = &player;

        SDL_AudioSpec obtained{};
        dev = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        if(dev == 0) {
            lastError = std::string("SDL_OpenAudioDevice failed: ") + SDL_GetError();
            return false;
        }

        // Select and start song
        try {
            player.song_select(1, /*loop=*/true);
        } catch(const std::exception& e) {
            lastError = std::string("song_select failed: ") + e.what();
            SDL_CloseAudioDevice(dev); dev=0;
            return false;
        }

        player.start(dev, /*loop=*/true);
        SDL_PauseAudioDevice(dev, 0);

        running = true;
        printf("[demo7] Sound system started: sr=%d  buf=%d  spr=%d\n",
               sampleRate, bf, sampleRate/60*6);
        return true;
    }

    void teardown()
    {
        if(!running) return;
        if(dev) {
            player.stop(dev);
            SDL_CloseAudioDevice(dev);
            dev = 0;
        }
        player.reset();  // clears all songs, SFX, patches, chip state
        running = false;
        printf("[demo7] Sound system torn down.\n");
    }

    void sfx_play(int id, int priority, int duration)
    {
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

    Uint32 lastTick  = 0;
    float  fpsSmooth = 0.0f;
    bool   running   = true;

    // UI state
    int  selectedRate   = 1;   // index into RATES[]
    int  selectedBuf    = 1;   // index into BUFS[]
};

static AppState g_app;

static const int  RATES[]     = { 22050, 44100, 48000 };
static const char* RATE_LBLS[]= { "22050 Hz", "44100 Hz", "48000 Hz" };
static const int  BUFS[]      = { 256, 512, 1024, 2048 };
static const char* BUF_LBLS[] = { "256", "512", "1024", "2048" };

// =============================================================================
// 11. PANEL
// =============================================================================

static void drawPanel(AppState& app)
{
    ImGuiIO& io = ImGui::GetIO();
    const float panelW = 380.f;
    const float panelH = (float)app.winH - 40.f;
    ImGui::SetNextWindowPos(
        ImVec2((io.DisplaySize.x - panelW)*0.5f, (io.DisplaySize.y - panelH)*0.5f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);

    ImGui::Begin("ingamefm demo7", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse);

    // ── Audio system init/teardown ────────────────────────────────────────────
    ImGui::SeparatorText("Audio System");

    // Rate selector (disabled when running)
    ImGui::BeginDisabled(app.sound.running);
    ImGui::Text("Sample rate");  // Note: lower rates = slower envelopes
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.f);
    if(ImGui::BeginCombo("##rate", RATE_LBLS[app.selectedRate])) {
        for(int i=0;i<3;i++) {
            bool sel=(i==app.selectedRate);
            if(ImGui::Selectable(RATE_LBLS[i],sel)) app.selectedRate=i;
            if(sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Text("Buffer");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.f);
    if(ImGui::BeginCombo("##buf", BUF_LBLS[app.selectedBuf])) {
        for(int i=0;i<4;i++) {
            bool sel=(i==app.selectedBuf);
            if(ImGui::Selectable(BUF_LBLS[i],sel)) app.selectedBuf=i;
            if(sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    ImGui::Spacing();

    if(!app.sound.running) {
        if(ImGui::Button("Init Sound System", ImVec2(-1, 0))) {
            app.sound.sampleRate   = RATES[app.selectedRate];
            app.sound.bufferFrames = BUFS[app.selectedBuf];
            if(!app.sound.init()) {
                fprintf(stderr, "[demo7] Init failed: %s\n",
                        app.sound.lastError.c_str());
            } else {
                app.musicVol = 1.0f;
                app.sfxVol   = 1.0f;
                app.sound.player.set_music_volume(app.musicVol);
                app.sound.player.set_sfx_volume(app.sfxVol);
            }
        }
        if(!app.sound.lastError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f,0.3f,0.3f,1.f));
            ImGui::TextWrapped("%s", app.sound.lastError.c_str());
            ImGui::PopStyleColor();
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f,0.1f,0.1f,1.f));
        if(ImGui::Button("Teardown Sound System", ImVec2(-1, 0)))
            app.sound.teardown();
        ImGui::PopStyleColor();

        // ── Status ───────────────────────────────────────────────────────────
        ImGui::Spacing();
        ImGui::TextDisabled("sr=%d  buf=%d  row %d/%d",
            app.sound.sampleRate,
            app.sound.bufferFrames,
            app.sound.player.get_current_row(),
            app.sound.player.get_song_length());

        ImGui::Separator();

        // ── Volume sliders ────────────────────────────────────────────────────
        ImGui::SeparatorText("Volume");
        {
            int mv = static_cast<int>(app.musicVol * 100.f + 0.5f);
            ImGui::SetNextItemWidth(160.f);
            if(ImGui::SliderInt("Music##vol", &mv, 0, 100, "%d%%")) {
                app.musicVol = mv / 100.f;
                app.sound.player.set_music_volume(app.musicVol);
            }
            ImGui::SameLine();
            int sv = static_cast<int>(app.sfxVol * 100.f + 0.5f);
            ImGui::SetNextItemWidth(160.f);
            if(ImGui::SliderInt("SFX##vol", &sv, 0, 100, "%d%%")) {
                app.sfxVol = sv / 100.f;
                app.sound.player.set_sfx_volume(app.sfxVol);
            }
        }

        ImGui::Separator();

        // ── SFX buttons ───────────────────────────────────────────────────────
        ImGui::SeparatorText("Sound Effects");
        ImGui::Spacing();

        struct SfxBtn { int id; int pri; int dur; const char* label; ImVec4 col; };
        static const SfxBtn btns[] = {
            { SFX_ID_JUMP,    4, 10, "[q] JUMP\np4",    ImVec4(0.2f,0.5f,0.8f,1.f) },
            { SFX_ID_COIN,    3,  8, "[w] COIN\np3",    ImVec4(0.8f,0.7f,0.1f,1.f) },
            { SFX_ID_ALARM,   5, 12, "[e] ALARM\np5",   ImVec4(0.8f,0.4f,0.1f,1.f) },
            { SFX_ID_FANFARE, 6, 20, "[r] FANFARE\np6", ImVec4(0.6f,0.1f,0.6f,1.f) },
        };
        const float bw = (panelW - 40.f) / 2.f;
        for(int i=0;i<4;i++) {
            if(i%2 != 0) ImGui::SameLine();
            const SfxBtn& b = btns[i];
            ImVec4 dim(b.col.x*0.35f, b.col.y*0.35f, b.col.z*0.35f, 0.9f);
            ImVec4 hov(b.col.x*0.6f,  b.col.y*0.6f,  b.col.z*0.6f,  1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button,        dim);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  b.col);
            if(ImGui::Button(b.label, ImVec2(bw, 44.f)))
                app.sound.sfx_play(b.id, b.pri, b.dur);
            ImGui::PopStyleColor(3);
        }
    }

    ImGui::Separator();
    {
        float ms = (app.fpsSmooth > 0.f) ? (1000.f/app.fpsSmooth) : 0.f;
        ImGui::TextDisabled("%.0f fps  |  %.2f ms/frame", app.fpsSmooth, ms);
    }

    ImGui::End();
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
