#!/bin/sh
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS="$ROOT/vita-deps"
WORK=$(mktemp -d)
JOBS=$(getconf _NPROCESSORS_ONLN)

# vitaGL first: SDL's configure looks for vitaGL.h and silently builds without
# VIDEO_VITA_VGL when it is missing, which leaves SDL_GL_CreateContext failing.
git clone https://github.com/Rinnegatamante/vitaGL "$WORK/vitaGL"
git -C "$WORK/vitaGL" checkout 92888a7e0b015d6017d0febef922e05dafa83fd7
sed -i.orig 's/define NUM_DISPLAY_LISTS 512/define NUM_DISPLAY_LISTS 65536/' "$WORK/vitaGL/source/display_lists.c"
sed -i.orig 's/void resetDlists()/void reset_dlists()/' "$WORK/vitaGL/source/display_lists.c"
make -C "$WORK/vitaGL" HAVE_DISPLAY_LISTS=1 NO_DEBUG=1 -j"$JOBS"
mkdir -p "$DEPS/lib" "$DEPS/include"
cp "$WORK/vitaGL/libvitaGL.a" "$DEPS/lib/"
cp "$WORK/vitaGL/source/vitaGL.h" "$DEPS/include/"

git clone -b vitagl https://github.com/Northfear/SDL "$WORK/SDL"
git -C "$WORK/SDL" checkout f99bb638f7eb256fa812e9da15d4e295a170640c
cmake -S "$WORK/SDL" -B "$WORK/SDL/build" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-I$DEPS/include" \
    -DVIDEO_VITA_VGL=ON \
    -DSDL_SHARED=OFF \
    -DSDL_TEST=OFF \
    -DCMAKE_INSTALL_PREFIX="$DEPS"
grep -q '^#define SDL_VIDEO_VITA_VGL 1' "$WORK/SDL/build/include-config-release/SDL_config.h" || {
    echo "SDL configured without vitaGL support" >&2
    exit 1
}
cmake --build "$WORK/SDL/build" -j"$JOBS"
cmake --install "$WORK/SDL/build"

rm -rf "$WORK"
