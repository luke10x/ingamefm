#!/bin/bash 
g++ -std=c++17 \
    -I../bowling/build/macos/sdl2/include/ \
    -I../bowling/3rdparty/SDL/include \
    -I../my-ym2612-plugin/build/_deps/ymfm-src/src/ \
    ../bowling/build/macos/usr/lib/libSDL2.a \
    ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_misc.cpp \
    ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_adpcm.cpp \
    ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_ssg.cpp \
    ../my-ym2612-plugin/build/_deps/ymfm-src/src/ymfm_opn.cpp \
    demo.cpp \
    -std=c++17 \
    -framework Cocoa -framework IOKit -framework CoreVideo -framework CoreAudio -framework AudioToolbox \
    -framework ForceFeedback -framework Carbon -framework Metal -framework GameController -framework CoreHaptics \
    -lobjc -o demo && ./demo 