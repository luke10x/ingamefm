#!/bin/bash
set -euo pipefail
mkdir -p new_demo
em++ -std=c++17 -O2 \
    -I../bowling/3rdparty/SDL/include \
    -I../bowling/3rdparty/imgui \
    -I../my-ym2612-plugin/build/_deps/ymfm-src/src/ \
    ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_misc.cpp \
    ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_adpcm.cpp \
    ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_ssg.cpp \
    ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_opn.cpp \
    ../bowling/3rdparty/imgui/imgui.cpp \
    ../bowling/3rdparty/imgui/imgui_draw.cpp \
    ../bowling/3rdparty/imgui/imgui_tables.cpp \
    ../bowling/3rdparty/imgui/imgui_widgets.cpp \
    ../bowling/3rdparty/imgui/backends/imgui_impl_sdl2.cpp \
    ../bowling/3rdparty/imgui/backends/imgui_impl_opengl3.cpp \
    new_demo.cpp \
    -s USE_SDL=2 \
    -s FULL_ES3=1 \
    -s MIN_WEBGL_VERSION=2 \
    -s MAX_WEBGL_VERSION=2 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s ASYNCIFY \
    -s ASSERTIONS \
    --shell-file shell.html \
    -o new_demo/index.html

(cd new_demo && python3 -mhttp.server)
