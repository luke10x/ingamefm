#!/bin/bash 

em++ -std=c++17 -O2 \
    -I../bowling/3rdparty/SDL/include \
    -I../my-ym2612-plugin/build/_deps/ymfm-src/src/ \
    ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_misc.cpp \
    ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_adpcm.cpp \
    ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_ssg.cpp \
    ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_opn.cpp \
    demo3.cpp \
    -s USE_SDL=2 \
    -s FULL_ES3=1 \
    -s MIN_WEBGL_VERSION=2 \
    -s MAX_WEBGL_VERSION=2 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s ASYNCIFY \
    --shell-file shell.html \
    -o webdemo/demo3.html
    