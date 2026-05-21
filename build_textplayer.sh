#!/bin/bash
# Build script for xfm_textplayer (native SDL2 build)

set -e

echo "Building xfm_textplayer..."

# Compiler flags
CXX="${CXX:-g++}"

# Use pkg-config to get SDL2 flags
SDL_CFLAGS=$(pkg-config --cflags sdl2 2>/dev/null || echo "-I/usr/include/SDL2")
SDL_LIBS=$(pkg-config --libs sdl2 2>/dev/null || echo "-lSDL2")

# YMFM paths - adjust these based on your setup
YMFM_PATH="${YMFM_PATH:-../../my-ym2612-plugin/build/_deps/ymfm-src/src}"
YMFM_SOURCES=""

# Check if YMFM sources exist
if [ -d "$YMFM_PATH" ]; then
    echo "Using YMFM from: $YMFM_PATH"
    # Only include OPN (YM2612) support for textplayer
    YMFM_SOURCES="$YMFM_PATH/ymfm_misc.cpp $YMFM_PATH/ymfm_adpcm.cpp $YMFM_PATH/ymfm_ssg.cpp $YMFM_PATH/ymfm_opn.cpp"
    YMFM_INCLUDE="-I$YMFM_PATH"
else
    echo "YMFM not found at $YMFM_PATH, trying alternative locations..."
    # Try common locations
    for path in "../ymfm/src" "../../ymfm/src" "/usr/include/ymfm"; do
        if [ -d "$path" ]; then
            YMFM_PATH="$path"
            YMFM_SOURCES="$YMFM_PATH/ymfm_misc.cpp $YMFM_PATH/ymfm_adpcm.cpp $YMFM_PATH/ymfm_ssg.cpp $YMFM_PATH/ymfm_opn.cpp"
            YMFM_INCLUDE="-I$YMFM_PATH"
            echo "Found YMFM at: $YMFM_PATH"
            break
        fi
    done
fi

CXXFLAGS="-std=c++17 -Wall -Wextra -O2 -I. $SDL_CFLAGS $YMFM_INCLUDE"
LDFLAGS="$SDL_LIBS -lm"

# Source files
SOURCES="xfm_textplayer.cpp xfm_impl.cpp xfm_export.cpp $YMFM_SOURCES"

# Output
OUTPUT="xfm_textplayer"

# Build
echo "Compiling..."
$CXX $CXXFLAGS $SOURCES -o $OUTPUT $LDFLAGS

echo ""
echo "Build complete: ./$OUTPUT"
echo ""
echo "Usage: ./$OUTPUT <song.txt> <ticks_per_second> <ticks_per_row> [--from-marker] [-v]"
echo "Example: ./$OUTPUT song.txt 60 6"
echo "Example range: ./$OUTPUT song.txt 60 6 --from-marker -v"
