#!/bin/bash
# Compila MHGU.exe (runtime) y mhguffmpeg.dll para Windows x64 con llvm-mingw, y arma native/build/win64/MHGU (lo que se
# entrega: el ejecutable y sus DLL, nunca código del juego; mhgugame.dll lo arma cada jugador con tools/build_game.py).
# Uso: bash build_windows.sh [Release|RelWithDebInfo]
#   Dependencias en deps/out/win64 (bash deps/build_deps.sh win64; bash deps/build_ffmpeg.sh win64).
#   LLVM_MINGW=<carpeta de llvm-mingw, o su bin> si no está en la ruta de abajo. GEN=<carpeta con main/ y sdk/>: también mhgugame.dll
#   (solo para pruebas propias, como -Pwithgame en Android: nunca publicarla).
set -e
TYPE=${1:-Release}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
SDK=$(cygpath -u "$LOCALAPPDATA/Android/Sdk")
CMAKE=${CMAKE:-"$SDK/cmake/3.22.1/bin/cmake.exe"}
NINJA=${NINJA:-"$SDK/cmake/3.22.1/bin/ninja.exe"}
LLVM_MINGW=${LLVM_MINGW:-E:/MH4U_Android/build/game_vanilla/toolchain/llvm-mingw-20241217-ucrt-x86_64}
[ -f "$LLVM_MINGW/x86_64-w64-mingw32-clang.exe" ] && LLVM_MINGW=$(dirname "$LLVM_MINGW")  # (its bin folder given: the same)
DEPS=${DEPS:-"$ROOT/deps/out/win64"}  # (otra copia de trabajo: DEPS=<las de esta>)
OUT="$HERE/build/win64"
m() { cygpath -m "$1"; }

# las mismas ids que tools/build_game.py --pc: ambos lados de la interfaz runtime <-> código del juego deben coincidir
GENFLAGS="windows-x86_64 -O2 -march=x86-64-v2 -fno-strict-aliasing -ffp-contract=off -fno-math-errno"
cd "$ROOT"
# (sin \r: el mismo id con cualquier fin de línea)
GAMELIB_ID=$( (cat tools/gen.py tools/lifter.py tools/neon.py tools/nso.py tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
               runtime/cpu.h runtime/vfp.h runtime/neon_rt.h runtime/gamelib.h | tr -d '\r'; echo "$GENFLAGS") | sha256sum | cut -c1-16)
SHADER_ID=$(cat runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h | tr -d '\r' | sha256sum | cut -c1-16)
VERSION="$(cat "$ROOT/VERSION" 2>/dev/null || echo dev)"
cd "$HERE"
export PATH="$(cygpath -u "$LLVM_MINGW")/bin:$PATH"
mkdir -p "$OUT"
# (the compiler's own files named llvm-mingw/..., not by where it sits on this computer: libc++'s headers put their paths in
# the program's assertion messages, and MHGU.exe carried this computer's folder names over a hundred times)
PREFIX_MAP="-ffile-prefix-map=$(m "$LLVM_MINGW")=llvm-mingw"

"$CMAKE" -S "$(m "$HERE")" -B "$(m "$OUT")" -G Ninja -DCMAKE_MAKE_PROGRAM="$(m "$NINJA")" \
  -DCMAKE_C_FLAGS="$PREFIX_MAP" -DCMAKE_CXX_FLAGS="$PREFIX_MAP" \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang.exe -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++.exe \
  -DCMAKE_ASM_COMPILER=x86_64-w64-mingw32-clang.exe -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres.exe \
  -DCMAKE_AR="$(m "$LLVM_MINGW")/bin/llvm-ar.exe" -DCMAKE_RANLIB="$(m "$LLVM_MINGW")/bin/llvm-ranlib.exe" \
  -DCMAKE_BUILD_TYPE=$TYPE -DMHGU_SRC="$(m "$ROOT")" -DDEPS="$(m "$DEPS")" -DGEN="${GEN:+$(m "$GEN")}" \
  -DGAMELIB_ID=$GAMELIB_ID -DSHADER_ID=$SHADER_ID -DMHGU_VERSION="$VERSION" -DMHGU_DEV=${MHGU_DEV:-OFF} > "$OUT.configure.log" 2>&1 \
  || { tail -30 "$OUT.configure.log"; exit 1; }
"$CMAKE" --build "$(m "$OUT")" --parallel ${JOBS:-10} > "$OUT.build.log" 2>&1 \
  || { grep -E "error|FAILED" "$OUT.build.log" | head -60; tail -20 "$OUT.build.log"; exit 1; }

# la carpeta del programa: MHGU.exe y sus DLL (no la de Vulkan: es la del driver)
DIST="$OUT/MHGU"
A="$ROOT/android/app/src/main/assets"
mkdir -p "$DIST/extras"
cp "$OUT/MHGU.exe" "$OUT/mhguffmpeg.dll" "$DEPS/sdl3/bin/SDL3.dll" "$DIST/"
# XeSS-SR's libxess.dll (the XeSS SDK 3.0.2's, from deps/build_deps.sh win64 xess), when deps has it with its license:
# loaded at run time by runtime/nvn/vulkan/xess.c (without it the game uses FSR 3.1). Shipped byte for byte as Intel
# signed it: one that differs is left out
XESS_DLL_SHA=251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7
XS="$DEPS/xess"
rm -f "$DIST/libxess.dll"
if [ -f "$XS/libxess.dll" ] && [ -f "$XS/LICENSE.txt" ] && [ -f "$XS/third-party-programs.txt" ]; then
  if [ "$(sha256sum "$XS/libxess.dll" | cut -d' ' -f1)" = "$XESS_DLL_SHA" ]; then
    cp "$XS/libxess.dll" "$DIST/"
  else
    echo "windows: $XS/libxess.dll is not the XeSS SDK 3.0.2's (SHA-256): not shipped, the game will use FSR 3.1"
  fi
else
  echo "windows: no libxess.dll in $XS (bash deps/build_deps.sh win64 xess): XeSS-SR not shipped, the game will use FSR 3.1"
fi
printf %s "$GAMELIB_ID" > "$DIST/gamelib_id.txt"  # (qué código del juego le sirve: tools/build_game.py)
# los pipelines grabados en las zonas comunes (hashes y estados, nada del juego): el primer arranque los arma de fondo
cp "$A/pipelines.list" "$DIST/"
# los trucos opcionales del port: el primer arranque los copia a cheats/, apagados (runtime/pc_actions.c)
cp "$A/cheats/comer_rapido.txt" "$DIST/extras/"
# el aviso legal y las licencias (Créditos y licencias → Licencias las muestra, el aviso primero): las de Android con lo
# propio de Windows, y las del runtime de llvm-mingw que MHGU.exe, SDL3.dll y mhguffmpeg.dll llevan adentro
# (THIRD_PARTY_NOTICES is the same file for both: what only one of them contains says "Android only" / "PC only";
# Android.txt, libadrenotools.txt and liblinkernsbypass.txt are the APK's alone and are not copied)
L="$DIST/licenses"
rm -rf "$L" && mkdir -p "$L"
# (the legal notice names the Android app: a first paragraph says it covers this version for Windows too; LICENSE
# names both, MHGU for Android and MHGU for PC, and goes as it is)
{ echo "This notice also covers MHGU for PC (MHGU.exe), the same project's version for Windows. On PC, the game code library each player builds is mhgugame.dll."
  echo; echo "Este aviso también cubre MHGU para PC (MHGU.exe), la versión para Windows del mismo proyecto. En PC, la librería de código que arma cada jugador es mhgugame.dll."
  echo; cat "$A/licenses/LEGAL.txt"; } > "$L/LEGAL.txt"
cp "$ROOT/LICENSE" "$L/LICENSE.txt"
cp "$ROOT/THIRD_PARTY_NOTICES" "$L/THIRD_PARTY_NOTICES.txt"
cp "$A/licenses/shaderc.txt" "$A/licenses/volk.txt" "$L/"
# (SDL3.txt: SDL's zlib license, then yuv2rgb's BSD license and the others SDL3.dll needs, as the APK's)
sed 's/(libSDL3\.so)/(SDL3.dll, beside MHGU.exe)/' "$A/licenses/SDL3.txt" > "$L/SDL3.txt"
# FFmpeg (LGPL 2.1+): how mhguffmpeg.dll was made, for anyone to make their own and put it in its place. FFCONF is
# deps/build_ffmpeg.sh's win64 configure without this machine's paths and tools: keep the two in step (checked below
# against the configure the libraries in DEPS were made with)
FFCONF="--enable-cross-compile --target-os=mingw32 --arch=x86_64 --enable-w32threads --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect --disable-debug --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample --enable-static --disable-shared --enable-protocol=file --enable-demuxer=mov --enable-parser=h264,aac --enable-decoder=h264,aac --disable-x86asm"
FFLOG="$DEPS/ffmpeg-build/ffbuild/config.log"
if [ -f "$FFLOG" ]; then
  FFUSED=" $(head -1 "$FFLOG" | tr -d "'\r" | cut -d' ' -f3-) "
  for o in $FFCONF; do
    case "$FFUSED" in *" $o "*) ;; *) echo "windows: FFmpeg in $DEPS was not configured with $o, which licenses/FFmpeg.txt says (deps/build_ffmpeg.sh)" ;; esac
  done
  for o in $FFUSED; do
    case "$o" in --prefix=*|--cc=*|--cxx=*|--host-cc=*|--ar=*|--nm=*|--ranlib=*|--strip=*|--sysroot=*|--extra-cflags=*) continue ;; esac
    case " $FFCONF " in *" $o "*) ;; *) echo "windows: FFmpeg in $DEPS was configured with $o too, which licenses/FFmpeg.txt leaves out (FFCONF)" ;; esac
  done
fi
# (each heading on one line: the menu's page wraps it to its width)
{ echo "This program uses FFmpeg 9.0.2 (libavformat, libavcodec, libavutil; Copyright (c) 2000-2026 the FFmpeg developers) under the GNU Lesser General Public License 2.1 or later (below). On Windows it is the separate library mhguffmpeg.dll beside MHGU.exe, which you can replace with your own build of FFmpeg 9.0.2: MHGU.exe calls only FFmpeg's public functions in it, by name. Its source code is the release ffmpeg-9.0.2.tar.xz, unchanged: it is attached to each release of this project that ships the PC zip (https://github.com/khorne-hub/gu-native/releases), and FFmpeg publishes it at https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz. It was configured with: $FFCONF, and built with llvm-mingw 20241217 (UCRT); its three static libraries are linked whole into the one DLL, every function exported (deps/build_ffmpeg.sh and native/CMakeLists.txt in this project's source, github.com/khorne-hub/gu-native)."
  echo; sed '1,/^$/d' "$A/licenses/FFmpeg.txt"; } > "$L/FFmpeg.txt"
MW="$(cygpath -u "$LLVM_MINGW")"
{ echo "MHGU.exe contains llvm-mingw's libc++, libc++abi, libunwind and compiler-rt, and SDL3.dll and mhguffmpeg.dll compiler-rt's builtins, linked statically, under the Apache License 2.0 with LLVM Exceptions (below)."
  echo; cat "$MW/LICENSE.TXT"; } > "$L/LLVM.txt"
{ echo "MHGU.exe, SDL3.dll and mhguffmpeg.dll contain the mingw-w64 runtime, and MHGU.exe winpthreads too, linked statically, under their licenses (below)."; echo
  cat "$MW/x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt"; echo
  cat "$MW/x86_64-w64-mingw32/share/mingw32/COPYING.winpthreads.txt"; } > "$L/MinGW-w64.txt"
# (what the folder holds and where each license is, in English and Spanish; one paragraph a file: the menu's page
# joins the lines of a paragraph)
{ echo "MHGU for PC: what is in this folder, and where each license is."
  echo; echo "MHGU.exe: the program, under the project's license (LICENSE.txt). THIRD_PARTY_NOTICES.txt lists the third-party code in it; shaderc and its parts (shaderc.txt) and llvm-mingw's runtime (LLVM.txt, MinGW-w64.txt) are linked into it."
  echo; echo "SDL3.dll: SDL 3.4.18 (SDL3.txt)."
  echo; echo "mhguffmpeg.dll: FFmpeg 9.0.2, under the GNU LGPL 2.1 or later. You can replace it with your own build (FFmpeg.txt)."
  if [ -f "$DIST/libxess.dll" ]; then
    echo; echo "libxess.dll: Intel's XeSS-SR runtime, unmodified, as Intel released it (XeSS.txt). It needs the Microsoft Visual C++ Redistributable (x64), which is not included."
  fi
  echo; echo "gamelib_id.txt, pipelines.list and extras: the project's own files."
  echo; echo "Nothing of the game is included: each player makes the game code library, mhgugame.dll, from their own copy of the game. The Vulkan loader is the graphics driver's."
  echo; echo "Monster Hunter is a trademark of the Great C. This project is not affiliated with, endorsed by or sponsored by the Great C or the Great N."
  echo; echo "----------------------------------------"
  echo; echo "MHGU para PC: qué hay en esta carpeta y dónde está cada licencia."
  echo; echo "MHGU.exe: el programa, con la licencia del proyecto (LICENSE.txt). THIRD_PARTY_NOTICES.txt lista el código de terceros que lleva dentro; shaderc y sus partes (shaderc.txt) y el runtime de llvm-mingw (LLVM.txt, MinGW-w64.txt) van enlazados en él."
  echo; echo "SDL3.dll: SDL 3.4.18 (SDL3.txt)."
  echo; echo "mhguffmpeg.dll: FFmpeg 9.0.2, con la licencia GNU LGPL 2.1 o posterior. Puedes reemplazarla por una compilada por ti (FFmpeg.txt)."
  if [ -f "$DIST/libxess.dll" ]; then
    echo; echo "libxess.dll: el XeSS-SR de Intel, sin modificar, tal como Intel lo publica (XeSS.txt). Necesita el Microsoft Visual C++ Redistributable (x64), que no viene incluido."
  fi
  echo; echo "gamelib_id.txt, pipelines.list y extras: archivos propios del proyecto."
  echo; echo "No incluye nada del juego: cada jugador arma la librería de código del juego, mhgugame.dll, con su propia copia del juego. El cargador de Vulkan es el del controlador (driver) de la tarjeta gráfica."
  echo; echo "Monster Hunter es una marca de la Gran C. Este proyecto no está afiliado, respaldado ni patrocinado por la Gran C ni la Gran N."
} > "$L/Windows.txt"
# (XeSS-SR: the license libxess.dll comes with, its copyright line in it, then the SDK's third-party notices)
if [ -f "$DIST/libxess.dll" ]; then
  { echo "libxess.dll, beside MHGU.exe, is the XeSS SDK 3.0.2's XeSS-SR runtime, shipped unmodified and loaded only when Upscaler is XeSS-SR, under the Intel Simplified Software License (below). The headers the program is built with are the SDK's, under the MIT License (THIRD_PARTY_NOTICES). The SDK's notices for third-party software follow the license."
    echo; tr -d '\r' < "$XS/LICENSE.txt"; echo; echo; tr -d '\r' < "$XS/third-party-programs.txt"; } > "$L/XeSS.txt"
fi
echo "windows: ok (GAMELIB_ID $GAMELIB_ID)"
ls -la "$DIST"
# PACK=1: the release's zip - the program's own files, by name (a folder the program ran in has its settings and logs too;
# nothing of the game) - and its SHA-256 beside it: the one each release publishes, for players to check theirs with
# certutil -hashfile (README, Official downloads)
if [ "${PACK:-0}" = 1 ]; then
  Z="MHGU-PC-$VERSION.zip"
  rm -f "$OUT/$Z" "$OUT/$Z.sha256.txt"
  # (the menu and the README offer XeSS-SR: a zip without its DLL only on purpose, XESS_OPTIONAL=1)
  if [ ! -f "$DIST/libxess.dll" ] && [ "${XESS_OPTIONAL:-0}" != 1 ]; then
    echo "zip: not made, libxess.dll is not beside MHGU.exe (see above): bash deps/build_deps.sh win64 xess, or XESS_OPTIONAL=1 for a zip without XeSS-SR"
    exit 1
  fi
  XZ=""; [ -f "$DIST/libxess.dll" ] && XZ=MHGU/libxess.dll  # (XeSS-SR, when this build has it)
  (cd "$OUT" && "$(cygpath -u "$SYSTEMROOT")/System32/tar.exe" -a -c -f "$Z" MHGU/MHGU.exe MHGU/mhguffmpeg.dll MHGU/SDL3.dll \
     $XZ MHGU/gamelib_id.txt MHGU/pipelines.list MHGU/extras MHGU/licenses) || { echo "zip: failed"; exit 1; }
  (cd "$OUT" && sha256sum "$Z" > "$Z.sha256.txt")
  echo "zip: $OUT/$Z"; echo "SHA-256: $(cut -d' ' -f1 "$OUT/$Z.sha256.txt")"
fi
