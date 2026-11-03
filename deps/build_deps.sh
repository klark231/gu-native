#!/bin/bash
# Compila las dependencias nativas para Android (arm64-v8a): SDL3 (shared), shaderc (static), FFmpeg (static).
# Uso: bash build_deps.sh [abi]   (abi por defecto arm64-v8a; x86_64 para el emulador)
set -e
ABI=${1:-arm64-v8a}
API=29
. "$(cd "$(dirname "$0")" && pwd)/env.sh"  # SDK, NDK, CMAKE, NINJA (Windows Git Bash or Linux)
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/src"
OUT="$HERE/out/$ABI"
mkdir -p "$OUT"
TC="$(cygpath -m "$NDK/build/cmake/android.toolchain.cmake")"
COMMON=(-G Ninja -DCMAKE_MAKE_PROGRAM="$(cygpath -m "$NINJA")" -DCMAKE_TOOLCHAIN_FILE="$TC" -DANDROID_ABI=$ABI
        -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON)

what=${2:-all}

if [[ $what == all || $what == sdl ]] && [ ! -f "$OUT/sdl3/.built" ]; then
  echo "== SDL3"
  "$CMAKE" -S "$(cygpath -m "$SRC/SDL3-3.4.18")" -B "$(cygpath -m "$OUT/sdl3/build")" "${COMMON[@]}" \
    -DCMAKE_INSTALL_PREFIX="$(cygpath -m "$OUT/sdl3")" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384 > "$OUT/sdl3.configure.log" 2>&1 || { tail -30 "$OUT/sdl3.configure.log"; exit 1; }
  "$CMAKE" --build "$(cygpath -m "$OUT/sdl3/build")" --target install --parallel 12 > "$OUT/sdl3.build.log" 2>&1 || { tail -30 "$OUT/sdl3.build.log"; exit 1; }
  touch "$OUT/sdl3/.built"
fi

if [[ $what == all || $what == shaderc ]] && [ ! -f "$OUT/shaderc/.built" ]; then
  echo "== shaderc"
  "$CMAKE" -S "$(cygpath -m "$SRC/shaderc")" -B "$(cygpath -m "$OUT/shaderc/build")" "${COMMON[@]}" \
    -DANDROID_STL=c++_static -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
    -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON \
    -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF > "$OUT/shaderc.configure.log" 2>&1 || { tail -30 "$OUT/shaderc.configure.log"; exit 1; }
  "$CMAKE" --build "$(cygpath -m "$OUT/shaderc/build")" --target shaderc_combined --parallel 12 > "$OUT/shaderc.build.log" 2>&1 || { tail -30 "$OUT/shaderc.build.log"; exit 1; }
  mkdir -p "$OUT/shaderc/lib" "$OUT/shaderc/include"
  cp "$OUT/shaderc/build/libshaderc/libshaderc_combined.a" "$OUT/shaderc/lib/"
  cp -r "$SRC/shaderc/libshaderc/include/shaderc" "$OUT/shaderc/include/"
  touch "$OUT/shaderc/.built"
fi
echo "deps $ABI: ok"
