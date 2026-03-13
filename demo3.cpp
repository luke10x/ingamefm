// =============================================================================
// demo3.cpp — ingamefm aurora demo
//
// Aurora borealis rendered with OpenGL ES 3 / WebGL2 as a fullscreen shader.
// FM audio: looping flute melody + kick/snare on channels 0-1.
// Channel 2: FM Guitar triggered by keyboard (Z-M = C4-B4)
//             or mouse click (random note, key-off on release).
//
// FPS / frame-time stats are accumulated and printed every 5 seconds.
//
// Targets:
//   Native (macOS/Linux): OpenGL ES 3 via SDL2
//   Web (Emscripten):     WebGL2 via SDL2 + emscripten_set_main_loop
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
#include <cstdio>
#include <cmath>
#include <cstring>

#include "ingamefm.h"

// =============================================================================
// 2. GLSL VERSION STRING  (#version 300 es works for ES3 and WebGL2)
// =============================================================================

#define GLSL_VERSION "#version 300 es\n"

// =============================================================================
// 3. SONG
//    Ch0 = PATCH_FLUTE  (inst 00)   melody with volume dynamics
//    Ch1 = PATCH_KICK   (inst 01) / PATCH_SNARE (inst 02)
//    Ch2 = PATCH_GUITAR — keyboard + mouse, not in song
// =============================================================================

static const char* SONG =
"org.tildearrow.furnace - Pattern Data (32)\n"
"32\n"
"E-4007F|C-1017F|\n"
".......|.......|\n"
"G-4007F|.......|\n"
".......|.......|\n"
"A-4007F|A-2027F|\n"
".......|.......|\n"
"G-4007F|.......|\n"
".......|.......|\n"
"E-4007F|C-1017F|\n"
".......|.......|\n"
"D-4007F|.......|\n"
".......|.......|\n"
"C-4007F|A-2027F|\n"
".......|.......|\n"
"D-4007F|.......|\n"
".......|.......|\n"
"E-4005F|C-1017F|\n"
".......|.......|\n"
"F#40050|.......|\n"
".......|.......|\n"
"G-4005F|A-2027F|\n"
".......|.......|\n"
"A-4005F|.......|\n"
".......|.......|\n"
"G-4003F|C-1017F|\n"
".......|.......|\n"
"E-4003F|.......|\n"
".......|.......|\n"
"D-4002F|A-2027F|\n"
".......|.......|\n"
"C-4007F|.......|\n"
"OFF....|OFF....|\n";

// =============================================================================
// 4. AURORA SHADERS
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
        const vec4 C = vec4(0.211324865405187,
                             0.366025403784439,
                            -0.577350269189626,
                             0.024390243902439);
        vec2 i  = floor(v + dot(v, C.yy));
        vec2 x0 = v - i + dot(i, C.xx);
        vec2 i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
        vec4 x12 = x0.xyxy + C.xxzz;
        x12.xy -= i1;
        i = mod289v2(i);
        vec3 p = permute3(
                     permute3(i.y + vec3(0.0, i1.y, 1.0))
                     + i.x + vec3(0.0, i1.x, 1.0));
        vec3 m = max(0.5 - vec3(dot(x0,x0),
                                dot(x12.xy, x12.xy),
                                dot(x12.zw, x12.zw)), 0.0);
        m = m * m * m * m;
        vec3 x  = 2.0 * fract(p * C.www) - 1.0;
        vec3 h  = abs(x) - 0.5;
        vec3 ox = floor(x + 0.5);
        vec3 a0 = x - ox;
        m *= 1.79284291400159 - 0.85373472095314 * (a0*a0 + h*h);
        vec3 g;
        g.x  = a0.x  * x0.x   + h.x  * x0.y;
        g.yz = a0.yz * x12.xz + h.yz * x12.yw;
        return 130.0 * dot(m, g);
    }

    void main() {
        float yawNorm    = uYaw / 3.14159;
        float yawOffset  = yawNorm * abs(yawNorm);
        float pitchNorm  = (uPitch + 1.5708) / 3.14159;
        float pitchOffset = (pitchNorm - 0.5) * 4.0;
        float timeOffset  = uTime * 0.0005;
        vec2 uv = TexCoord + vec2(yawOffset, pitchOffset + timeOffset);
        float n1 = snoise(uv * 3.0) * 0.5;
        float n2 = snoise(uv * 7.0  + vec2(uTime * 0.01,  0.0)) * 0.3;
        float n3 = snoise(uv * 15.0 + vec2(uTime * 0.02,  0.0)) * 0.2;
        float intensity = clamp(n1 + n2 + n3, 0.0, 1.0);
        vec3 col1 = vec3(sin(TexCoord.x + TexCoord.y + uTime * 0.001), 0.2, 0.3);
        vec3 col2 = vec3(0.9, sin(TexCoord.y + uTime * 0.0005), 0.5);
        vec3 color = mix(col1, col2, intensity);
        float opacity = pitchNorm;
        FragColor = vec4(color, opacity);
    }
    )";

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
        fprintf(stderr, "Shader compile error (%s):\n%s\n",
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
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
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
    float  yaw     = 0.0f;
    float  pitch   = 0.0f;

    void init()
    {
        program = createProgram(AURORA_VERT, AURORA_FRAG);

        static const GLfloat verts[] = {
            -1.f, -1.f,  1.000f,  0.f, 0.f,
             1.f, -1.f,  0.998f,  1.f, 0.f,
            -1.f,  1.f,  0.998f,  0.f, 1.f,
             1.f,  1.f,  0.998f,  1.f, 1.f,
        };
        static const GLuint idx[] = { 0,1,2, 1,3,2 };

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
                              5 * sizeof(GLfloat), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                              5 * sizeof(GLfloat), (void*)(3 * sizeof(GLfloat)));
        glBindVertexArray(0);
        checkGLError("AuroraRenderer::init");
    }

    void render(float deltaTime)
    {
        time  += deltaTime;
        yaw    = sinf(time * 0.07f) * 1.2f;
        pitch  = sinf(time * 0.04f) * 0.8f - 0.4f;

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
// 7. PERF COUNTER  — accumulates frame times, prints summary every 5 seconds
// =============================================================================

struct PerfCounter
{
    float accumSec   = 0.0f;   // wall time accumulated in current window
    float accumDt    = 0.0f;   // sum of all dt in current window
    float minDt      = 1e9f;
    float maxDt      = 0.0f;
    int   frameCount = 0;

    static constexpr float PRINT_INTERVAL = 5.0f;

    void update(float dt)
    {
        accumSec   += dt;
        accumDt    += dt;
        frameCount++;
        if (dt < minDt) minDt = dt;
        if (dt > maxDt) maxDt = dt;

        if (accumSec >= PRINT_INTERVAL)
        {
            float avgDt  = accumDt / frameCount;
            float avgFps = 1.0f / avgDt;
            float minFps = 1.0f / maxDt;   // worst fps = longest frame
            float maxFps = 1.0f / minDt;   // best fps  = shortest frame

            printf("[perf] over %.1fs — "
                   "avg %.2f ms (%.1f fps)  "
                   "min %.2f ms (%.1f fps)  "
                   "max %.2f ms (%.1f fps)  "
                   "frames %d\n",
                   accumSec,
                   avgDt * 1000.f, avgFps,
                   minDt * 1000.f, maxFps,
                   maxDt * 1000.f, minFps,
                   frameCount);

            // Reset for next window
            accumSec   = 0.0f;
            accumDt    = 0.0f;
            minDt      = 1e9f;
            maxDt      = 0.0f;
            frameCount = 0;
        }
    }
};

// =============================================================================
// 8. APPLICATION STATE
// =============================================================================

struct AppState
{
    // Window / GL
    SDL_Window*     window  = nullptr;
    SDL_GLContext   glCtx   = nullptr;
    int             winW    = 800;
    int             winH    = 480;

    // Renderer
    AuroraRenderer  aurora;

    // Audio
    SDL_AudioDeviceID  audioDev = 0;
    IngameFMPlayer     player;

    // Timing
    Uint32       lastTick = 0;
    PerfCounter  perf;

    // Mouse guitar state: which MIDI note is currently held by mouse (-1 = none)
    int  mouseMidiNote = -1;

    bool running = true;
};

static AppState g_app;

// =============================================================================
// 9. KEYBOARD -> MIDI
// =============================================================================

static int keyToMidi(SDL_Keycode k)
{
    switch (k) {
        case SDLK_z: return 60;  case SDLK_s: return 61;
        case SDLK_x: return 62;  case SDLK_d: return 63;
        case SDLK_c: return 64;  case SDLK_v: return 65;
        case SDLK_g: return 66;  case SDLK_b: return 67;
        case SDLK_h: return 68;  case SDLK_n: return 69;
        case SDLK_j: return 70;  case SDLK_m: return 71;
    }
    return -1;
}

// Pick a random guitar MIDI note in the range C3-B4 (48-71) each mouse press.
// Uses a simple LCG seeded from SDL_GetTicks so it varies each run.
static int randomGuitarNote()
{
    static Uint32 seed = 0;
    if (seed == 0) seed = SDL_GetTicks() | 1u;
    seed = seed * 1664525u + 1013904223u;   // Knuth LCG
    // Range: MIDI 48 (C3) – 71 (B4), 24 notes
    return 48 + (int)(seed >> 16) % 24;
}

// =============================================================================
// 10. PER-FRAME TICK
// =============================================================================

static void mainTick()
{
    AppState& app = g_app;

    // -------------------------------------------------------------------------
    // Timing
    // -------------------------------------------------------------------------
    Uint32 now = SDL_GetTicks();
    float  dt  = (app.lastTick == 0) ? 0.016f
                                     : (now - app.lastTick) * 0.001f;
    app.lastTick = now;

    // Clamp dt to avoid spiral-of-death on tab switches / hitches
    if (dt > 0.1f) dt = 0.1f;

    app.perf.update(dt);

    // -------------------------------------------------------------------------
    // Events
    // -------------------------------------------------------------------------
    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
        // ---- quit ----
        if (e.type == SDL_QUIT)
        {
            app.running = false;
#ifdef __EMSCRIPTEN__
            emscripten_cancel_main_loop();
#endif
        }

        // ---- keyboard ----
        if (e.type == SDL_KEYDOWN && !e.key.repeat)
        {
            if (e.key.keysym.sym == SDLK_ESCAPE)
            {
                app.running = false;
#ifdef __EMSCRIPTEN__
                emscripten_cancel_main_loop();
#endif
            }
            int midi = keyToMidi(e.key.keysym.sym);
            if (midi >= 0)
            {
                double hz = IngameFMChip::midi_to_hz(midi);
                SDL_LockAudioDevice(app.audioDev);
                app.player.chip()->key_off(2);
                app.player.chip()->load_patch(PATCH_GUITAR, 2);
                app.player.chip()->set_frequency(2, hz, 0);
                app.player.chip()->key_on(2);
                SDL_UnlockAudioDevice(app.audioDev);
            }
        }

        if (e.type == SDL_KEYUP)
        {
            if (keyToMidi(e.key.keysym.sym) >= 0)
            {
                SDL_LockAudioDevice(app.audioDev);
                app.player.chip()->key_off(2);
                SDL_UnlockAudioDevice(app.audioDev);
            }
        }

        // ---- mouse — random note on press, key-off on release ----
        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT)
        {
            app.mouseMidiNote = randomGuitarNote();
            double hz = IngameFMChip::midi_to_hz(app.mouseMidiNote);
            SDL_LockAudioDevice(app.audioDev);
            app.player.chip()->key_off(2);
            app.player.chip()->load_patch(PATCH_GUITAR, 2);
            app.player.chip()->set_frequency(2, hz, 0);
            app.player.chip()->key_on(2);
            SDL_UnlockAudioDevice(app.audioDev);
        }

        if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT)
        {
            if (app.mouseMidiNote >= 0)
            {
                SDL_LockAudioDevice(app.audioDev);
                app.player.chip()->key_off(2);
                SDL_UnlockAudioDevice(app.audioDev);
                app.mouseMidiNote = -1;
            }
        }

        // ---- window resize ----
        if (e.type == SDL_WINDOWEVENT &&
            e.window.event == SDL_WINDOWEVENT_RESIZED)
        {
            app.winW = e.window.data1;
            app.winH = e.window.data2;
            glViewport(0, 0, app.winW, app.winH);
        }
    }

    // -------------------------------------------------------------------------
    // Render
    // -------------------------------------------------------------------------
    glClearColor(0.0f, 0.0f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    app.aurora.render(dt);

    SDL_GL_SwapWindow(app.window);
}

// =============================================================================
// 11. INIT
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
        "ingamefm aurora  |  Z-M / click: guitar  |  Esc: quit",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        app.winW, app.winH,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN
    );
    if (!app.window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    app.glCtx = SDL_GL_CreateContext(app.window);
    if (!app.glCtx) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }

    if (SDL_GL_SetSwapInterval(-1) != 0)
        if (SDL_GL_SetSwapInterval(1) != 0)
            SDL_GL_SetSwapInterval(0);

    glViewport(0, 0, app.winW, app.winH);
    return true;
}

static bool initAudio(AppState& app)
{
    try {
        app.player.set_song(SONG, 60, 6);
        app.player.add_patch(0x00, PATCH_FLUTE);
        app.player.add_patch(0x01, PATCH_KICK);
        app.player.add_patch(0x02, PATCH_SNARE);
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
    if (app.audioDev == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }

    app.player.start(app.audioDev, true);
    app.player.chip()->load_patch(PATCH_GUITAR, 2);
    SDL_PauseAudioDevice(app.audioDev, 0);
    return true;
}

// =============================================================================
// 12. MAIN
// =============================================================================

int main(int /*argc*/, char** /*argv*/)
{
    AppState& app = g_app;

    if (!initVideo(app)) return 1;
    if (!initAudio(app)) return 1;

    app.aurora.init();

    printf("=== ingamefm aurora demo ===\n");
    printf("Ch0: Flute melody   (looping, volume dynamics)\n");
    printf("Ch1: Kick + Snare\n");
    printf("Ch2: Guitar — keyboard Z-M (C4-B4), or click anywhere (random note)\n");
    printf("Perf stats printed every 5 seconds.\n");
    printf("Esc or close to quit.\n\n");

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(mainTick, 0, 1);
#else
    while (app.running)
        mainTick();

    SDL_CloseAudioDevice(app.audioDev);
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
// ── NATIVE (macOS) ───────────────────────────────────────────────────────────
//
//   g++ -std=c++17 -O2 \
//       -I../bowling/build/macos/sdl2/include/ \
//       -I../bowling/3rdparty/SDL/include \
//       -I../my-ym2612-plugin/build/_deps/ymfm-src/src/ \
//       ../bowling/build/macos/usr/lib/libSDL2.a \
//       ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_misc.cpp \
//       ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_adpcm.cpp \
//       ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_ssg.cpp \
//       ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_opn.cpp \
//       demo3.cpp \
//       -framework Cocoa -framework IOKit -framework CoreVideo \
//       -framework CoreAudio -framework AudioToolbox \
//       -framework ForceFeedback -framework Carbon \
//       -framework Metal -framework GameController -framework CoreHaptics \
//       -lobjc -o demo3 && ./demo3
//
// ── EMSCRIPTEN ───────────────────────────────────────────────────────────────
//
//   em++ -std=c++17 -O2 \
//       -I../bowling/3rdparty/SDL/include \
//       -I../my-ym2612-plugin/build/_deps/ymfm-src/src/ \
//       ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_misc.cpp \
//       ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_adpcm.cpp \
//       ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_ssg.cpp \
//       ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_opn.cpp \
//       demo3.cpp \
//       -s USE_SDL=2 \
//       -s FULL_ES3=1 \
//       -s MIN_WEBGL_VERSION=2 \
//       -s MAX_WEBGL_VERSION=2 \
//       -s ALLOW_MEMORY_GROWTH=1 \
//       -s ASYNCIFY \
//       --shell-file shell.html \
//       -o demo3.html
//
//   python3 -m http.server   →   http://localhost:8000/demo3.html
// =============================================================================
