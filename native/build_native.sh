#!/bin/bash
# Compila libmain.so (runtime) y, si existe generated/, libmhgugame.so (código del juego) para Android.
# Uso: bash build_native.sh [abi] [Release|RelWithDebInfo]
set -e
ABI=${1:-arm64-v8a}
TYPE=${2:-Release}
API=28
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
SRC="$ROOT"
. "$ROOT/deps/env.sh"  # SDK, NDK, CMAKE, NINJA, HOST_TAG (Windows Git Bash or Linux)
OUT="$HERE/build/$ABI"
m() { cygpath -m "$1"; }

# las mismas ids que el Makefile: ambos lados de la interfaz runtime <-> código del juego deben coincidir
GENFLAGS="android-$ABI -O2 -nossp -fno-strict-aliasing -ffp-contract=off -fno-math-errno"
cd "$SRC"
# (sin \r: el mismo id con cualquier fin de línea; tools/build_game.py lo calcula igual)
GAMELIB_ID=$( (cat tools/gen.py tools/lifter.py tools/neon.py tools/nso.py tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
               runtime/cpu.h runtime/vfp.h runtime/neon_rt.h runtime/gamelib.h | tr -d '\r'; echo "$GENFLAGS") | sha256sum | cut -c1-16)
SHADER_ID=$(cat runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h | tr -d '\r' | sha256sum | cut -c1-16)
VERSION="$(cat "$ROOT/VERSION" 2>/dev/null || echo dev)"
cd "$HERE"
mkdir -p "$HERE/build"  # native/build/ is gitignored, so on a fresh checkout it does not exist yet and the "> $OUT.configure.log" redirect fails

"$CMAKE" -S "$(m "$HERE")" -B "$(m "$OUT")" -G Ninja -DCMAKE_MAKE_PROGRAM="$(m "$NINJA")" \
  -DCMAKE_TOOLCHAIN_FILE="$(m "$NDK/build/cmake/android.toolchain.cmake")" -DANDROID_ABI=$ABI \
  -DANDROID_PLATFORM=android-$API -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=$TYPE \
  -DMHGU_SRC="$(m "$SRC")" -DDEPS="$(m "$ROOT/deps/out/$ABI")" -DGEN="$(m "$SRC/generated")" \
  -DGAMELIB_ID=$GAMELIB_ID -DSHADER_ID=$SHADER_ID -DMHGU_VERSION="$VERSION" -DMHGU_DEV=${MHGU_DEV:-OFF} > "$OUT.configure.log" 2>&1 \
  || { tail -30 "$OUT.configure.log"; exit 1; }
"$CMAKE" --build "$(m "$OUT")" --parallel ${JOBS:-12} 2>&1 | grep -E "error|warning: implicit|FAILED|^ninja: build stopped" | head -60
"$CMAKE" --build "$(m "$OUT")" --parallel ${JOBS:-12} > "$OUT.build.log" 2>&1 || { tail -40 "$OUT.build.log"; exit 1; }

# a jniLibs del proyecto Gradle
J="$ROOT/android/app/src/main/jniLibs/$ABI"
mkdir -p "$J"
mkdir -p "$ROOT/android/app/src/main/assets"
printf %s "$GAMELIB_ID" > "$ROOT/android/app/src/main/assets/gamelib_id.txt"  # (GameFolderActivity: which game code fits)
cp "$OUT/libmain.so" "$OUT/libmhguffmpeg.so" "$ROOT/deps/out/$ABI/sdl3/lib/libSDL3.so" "$J/"
cp "$NDK/toolchains/llvm/prebuilt/$HOST_TAG/sysroot/usr/lib/$( [ $ABI = arm64-v8a ] && echo aarch64 || echo x86_64)-linux-android/libc++_shared.so" "$J/"
# (a driver of the player's own: runtime/platform/android_gpu.c; these are found in the app's nativeLibraryDir)
[ "$ABI" = arm64-v8a ] && cp "$OUT"/adrenotools/src/hook/lib*_hook.so "$OUT/adrenotools/src/hook/libhook_impl.so" "$J/"
[ -f "$OUT/libmhgugame.so" ] && cp "$OUT/libmhgugame.so" "$J/"
echo "native $ABI: ok (GAMELIB_ID $GAMELIB_ID)"
ls -la "$J"
