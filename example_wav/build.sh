#!/bin/bash
# Build script for WAV playback demo

set -euo pipefail

SDL_DIR="../../bowling/3rdparty/SDL"

echo "Building WAV playback demo..."

# Compile - no ymfm needed! Pure WAV playback with SDL2 audio
g++ -std=c++17 -O2 \
    -I"$SDL_DIR/include" \
    -I.. \
    ../xfm_wavplay.cpp \
    wavplay_demo.cpp \
    -o wavplay_demo \
    $(pkg-config --cflags --libs sdl2 2>/dev/null || echo "-I/opt/homebrew/include -L/opt/homebrew/lib -lSDL2") \
    -lm

echo "Build complete!"
echo ""
echo "Running demo..."
echo ""

# Run demo (uses WAVs from exporter)
./wavplay_demo ../exporter/exported

echo ""
echo "Demo finished!"
