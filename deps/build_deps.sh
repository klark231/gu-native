#!/bin/bash
# Compila las dependencias nativas para Android (arm64-v8a) y PC (win64): SDL3 (shared), shaderc (static); FFmpeg (static)
# en build_ffmpeg.sh.
# Uso: bash build_deps.sh [abi] [all|sdl|shaderc|vulkan|xess]   (abi por defecto arm64-v8a; x86_64 para el emulador; win64
# para Windows x64, que suma los encabezados de Vulkan y la libxess.dll de XeSS-SR)
# Variables: DEPS_SRC (fuentes, por defecto deps/src), DEPS_OUT (dónde quedan, por defecto deps/out/<abi>), LLVM_MINGW
# (llvm-mingw o su bin, para win64), JOBS (procesos, 12), XESS_ZIP (win64: XeSS_SDK_3.0.2.zip ya bajado, en vez de bajarlo).
set -e
ABI=${1:-arm64-v8a}
API=29
. "$(cd "$(dirname "$0")" && pwd)/env.sh"  # SDK, NDK, CMAKE, NINJA (Windows Git Bash or Linux)
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cygpath -u "${DEPS_SRC:-$HERE/src}")
OUT=$(cygpath -u "${DEPS_OUT:-$HERE/out/$ABI}")
JOBS=${JOBS:-12}
mkdir -p "$OUT"
if [ "$ABI" = win64 ]; then
  # Windows x64 con llvm-mingw (clang + libc++, UCRT), sin archivo de toolchain
  LLVM_MINGW=$(cygpath -u "${LLVM_MINGW:-E:/MH4U_Android/build/game_vanilla/toolchain/llvm-mingw-20241217-ucrt-x86_64/bin}")
  [ -f "$LLVM_MINGW/bin/x86_64-w64-mingw32-clang.exe" ] && LLVM_MINGW="$LLVM_MINGW/bin"  # (su carpeta en vez de bin: lo mismo)
  export PATH="$LLVM_MINGW:$PATH"
  MW="$(cygpath -m "$LLVM_MINGW")"
  COMMON=(-G Ninja -DCMAKE_MAKE_PROGRAM="$(cygpath -m "$NINJA")" -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=AMD64
          -DCMAKE_C_COMPILER="$MW/x86_64-w64-mingw32-clang.exe" -DCMAKE_CXX_COMPILER="$MW/x86_64-w64-mingw32-clang++.exe"
          -DCMAKE_RC_COMPILER="$MW/x86_64-w64-mingw32-windres.exe" -DCMAKE_AR="$MW/llvm-ar.exe"
          -DCMAKE_RANLIB="$MW/llvm-ranlib.exe" -DCMAKE_BUILD_TYPE=Release)
  # (SDL3 es C: que la DLL no dependa de libc++.dll ni de libunwind.dll)
  SDL_OPTS=(-DCMAKE_SHARED_LINKER_FLAGS="-static-libgcc -static-libstdc++")
  SHADERC_OPTS=()
else
  TC="$(cygpath -m "$NDK/build/cmake/android.toolchain.cmake")"
  COMMON=(-G Ninja -DCMAKE_MAKE_PROGRAM="$(cygpath -m "$NINJA")" -DCMAKE_TOOLCHAIN_FILE="$TC" -DANDROID_ABI=$ABI
          -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
  SDL_OPTS=(-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384)
  SHADERC_OPTS=(-DANDROID_STL=c++_static)
fi

what=${2:-all}

if [[ $what == all || $what == sdl ]] && [ ! -f "$OUT/sdl3/.built" ]; then
  echo "== SDL3"
  "$CMAKE" -S "$(cygpath -m "$SRC/SDL3-3.4.18")" -B "$(cygpath -m "$OUT/sdl3/build")" "${COMMON[@]}" \
    -DCMAKE_INSTALL_PREFIX="$(cygpath -m "$OUT/sdl3")" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF "${SDL_OPTS[@]}" > "$OUT/sdl3.configure.log" 2>&1 || { tail -30 "$OUT/sdl3.configure.log"; exit 1; }
  "$CMAKE" --build "$(cygpath -m "$OUT/sdl3/build")" --target install --parallel $JOBS > "$OUT/sdl3.build.log" 2>&1 || { tail -30 "$OUT/sdl3.build.log"; exit 1; }
  touch "$OUT/sdl3/.built"
fi

if [[ $what == all || $what == shaderc ]] && [ ! -f "$OUT/shaderc/.built" ]; then
  echo "== shaderc"
  "$CMAKE" -S "$(cygpath -m "$SRC/shaderc")" -B "$(cygpath -m "$OUT/shaderc/build")" "${COMMON[@]}" \
    "${SHADERC_OPTS[@]}" -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
    -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON \
    -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF > "$OUT/shaderc.configure.log" 2>&1 || { tail -30 "$OUT/shaderc.configure.log"; exit 1; }
  "$CMAKE" --build "$(cygpath -m "$OUT/shaderc/build")" --target shaderc_combined --parallel $JOBS > "$OUT/shaderc.build.log" 2>&1 || { tail -30 "$OUT/shaderc.build.log"; exit 1; }
  mkdir -p "$OUT/shaderc/lib" "$OUT/shaderc/include"
  cp "$OUT/shaderc/build/libshaderc/libshaderc_combined.a" "$OUT/shaderc/lib/"
  cp -r "$SRC/shaderc/libshaderc/include/shaderc" "$OUT/shaderc/include/"
  touch "$OUT/shaderc/.built"
fi

# Windows: los encabezados de Vulkan (en Android vienen con el NDK), de Vulkan-Headers en DEPS_SRC: la etiqueta
# vulkan-sdk-1.4.363.0, la del SDK de SPIRV-Tools y SPIRV-Headers (FSR 4 usa VK_EXT_shader_float8, que el del NDK no tiene)
if [ "$ABI" = win64 ] && [[ $what == all || $what == vulkan ]] && [ ! -f "$OUT/vulkan/.built" ]; then
  echo "== Vulkan-Headers"
  [ -f "$SRC/Vulkan-Headers/include/vulkan/vulkan_core.h" ] || { echo "falta $SRC/Vulkan-Headers (vulkan-sdk-1.4.363.0)"; exit 1; }
  rm -rf "$OUT/vulkan" && mkdir -p "$OUT/vulkan/include"
  cp -r "$SRC/Vulkan-Headers/include/vulkan" "$SRC/Vulkan-Headers/include/vk_video" "$OUT/vulkan/include/"
  touch "$OUT/vulkan/.built"
fi
# Windows: la libxess.dll de XeSS-SR (el escalador opcional, runtime/nvn/vulkan/xess.c), sin modificar, con su licencia y
# su third-party-programs.txt, del zip del XeSS SDK 3.0.2 (third_party/xess/README.md). Nunca va al repositorio:
# build_windows.sh la pone junto a MHGU.exe y arma licenses/XeSS.txt
XESS_URL=https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip
XESS_ZIP_SHA=88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990
XESS_DLL_SHA=251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7
if [ "$ABI" = win64 ] && [[ $what == all || $what == xess ]] && [ ! -f "$OUT/xess/.built" ]; then
  echo "== XeSS SDK 3.0.2 (libxess.dll)"
  TMP="$HERE/tmp/xess"
  rm -rf "$TMP" && mkdir -p "$TMP"
  if [ -n "$XESS_ZIP" ]; then
    Z=$(cygpath -u "$XESS_ZIP")
  else
    Z="$TMP/XeSS_SDK_3.0.2.zip"
    curl -fL --retry 3 -o "$Z" "$XESS_URL" || { echo "no se pudo bajar $XESS_URL"; exit 1; }
  fi
  [ "$(sha256sum "$Z" | cut -d' ' -f1)" = "$XESS_ZIP_SHA" ] || { echo "$Z no es el zip del XeSS SDK 3.0.2 (SHA-256 distinto)"; exit 1; }
  unzip -q -o "$Z" bin/libxess.dll LICENSE.txt third-party-programs.txt -d "$TMP/sdk" || { echo "$Z: no se pudo extraer"; exit 1; }
  [ "$(sha256sum "$TMP/sdk/bin/libxess.dll" | cut -d' ' -f1)" = "$XESS_DLL_SHA" ] || { echo "libxess.dll: SHA-256 distinto"; exit 1; }
  rm -rf "$OUT/xess" && mkdir -p "$OUT/xess"
  cp "$TMP/sdk/bin/libxess.dll" "$TMP/sdk/LICENSE.txt" "$TMP/sdk/third-party-programs.txt" "$OUT/xess/"
  rm -rf "$TMP"
  touch "$OUT/xess/.built"
fi
echo "deps $ABI: ok"
