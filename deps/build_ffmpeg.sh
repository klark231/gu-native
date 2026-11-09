#!/bin/bash
# FFmpeg mínimo (MP4 + H.264 + AAC, como el Makefile del port) estático para Android y Windows x64.
# Uso: bash build_ffmpeg.sh [abi]   (arm64-v8a por defecto; x86_64 para el emulador; win64 para Windows x64)
# Variables: DEPS_SRC (fuentes, por defecto deps/src), LLVM_MINGW (llvm-mingw o su bin, win64), JOBS (make -j, 12)
set -e
mkdir -p "$(dirname "$0")/tmp"; export TMPDIR="$(cd "$(dirname "$0")" && pwd)/tmp" TEMP= TMP=
ABI=${1:-arm64-v8a}
API=29
case $ABI in
  arm64-v8a) ARCH=aarch64; TRIPLE=aarch64-linux-android ;;
  x86_64)    ARCH=x86_64;  TRIPLE=x86_64-linux-android ;;
  win64)     ARCH=x86_64;  TRIPLE=x86_64-w64-mingw32 ;;
  *) echo "abi desconocida: $ABI"; exit 1 ;;
esac
. "$(cd "$(dirname "$0")" && pwd)/env.sh"  # SDK, NDK, HOST_TAG, EXE (Windows Git Bash or Linux)
TCB="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/out/$ABI/ffmpeg"
BUILD="$HERE/out/$ABI/ffmpeg-build"
[ -f "$OUT/.built" ] && { echo "ffmpeg $ABI: ya está"; exit 0; }
SRC=$(cd "${DEPS_SRC:-$HERE/src}" && pwd)  # (despues de lo anterior: ya compilado, no hacen falta las fuentes)
rm -rf "$BUILD" && mkdir -p "$BUILD" "$OUT"
if [ $ABI = win64 ]; then
  # Windows x64 con llvm-mingw (clang + UCRT), sin nasm; hilos de Windows (si faltan, configure falla)
  LLVM_MINGW=${LLVM_MINGW:-E:/MH4U_Android/build/game_vanilla/toolchain/llvm-mingw-20241217-ucrt-x86_64/bin}
  [ -f "$LLVM_MINGW/bin/x86_64-w64-mingw32-clang.exe" ] && LLVM_MINGW="$LLVM_MINGW/bin"  # (su carpeta en vez de bin: lo mismo)
  export PATH="$(cygpath -u "$LLVM_MINGW"):$NDK/prebuilt/$HOST_TAG/bin:$PATH"
  TOS=mingw32; XCC=$TRIPLE-clang.exe; XCXX=$TRIPLE-clang++.exe; EXTRA=(--enable-w32threads)
else
  export PATH="$TCB/bin:$NDK/prebuilt/$HOST_TAG/bin:$PATH"
  TOS=android; XCC="clang$EXE --target=$TRIPLE$API"; XCXX="clang++$EXE --target=$TRIPLE$API"
  EXTRA=(--sysroot="$(cygpath -m "$TCB/sysroot")" --enable-pic)
fi
cd "$BUILD"
"$(cygpath -m "$SRC/ffmpeg")/configure" --prefix="$(cygpath -m "$OUT")" \
  --enable-cross-compile --target-os=$TOS --arch=$ARCH \
  --cc="$XCC" --cxx="$XCXX" \
  --host-cc="$XCC" --ar=llvm-ar$EXE --nm=llvm-nm$EXE --ranlib=llvm-ranlib$EXE --strip=llvm-strip$EXE \
  "${EXTRA[@]}" \
  --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
  --disable-debug --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
  --enable-static --disable-shared --enable-protocol=file --enable-demuxer=mov \
  --enable-parser=h264,aac --enable-decoder=h264,aac \
  --extra-cflags="-ffile-prefix-map=$(cygpath -m "$SRC/ffmpeg")=ffmpeg" \
  $( [ $ARCH = x86_64 ] && echo --disable-x86asm ) > configure.log 2>&1 || { tail -30 configure.log; tail -40 ffbuild/config.log; exit 1; }
sed -i 's|^#define FFMPEG_CONFIGURATION .*|#define FFMPEG_CONFIGURATION ""|' config.h  # (no paths of this machine in the library)
# (make de Windows: rutas E:/ en vez de /e/, con la unidad de las fuentes)
if [ "$HOST_OS" = windows ]; then
  U=$(echo "$SRC" | cut -d/ -f2)
  sed -i "s|/$U/|${U^^}:/|g" Makefile ffbuild/config.mak
fi
export TMPDIR="$(cygpath -m "$TMPDIR")" TMP="$(cygpath -m "$TMPDIR")" TEMP="$(cygpath -m "$TMPDIR")"
# (sin "make install": el make de Windows corta las órdenes de más de ~8 KB y la de los headers de libavutil
#  quedaba con el destino cortado, copiándolos en la carpeta de las fuentes; se instalan las librerías y los .pc)
make -j${JOBS:-12} install-libs install-libavutil-pkgconfig install-libavcodec-pkgconfig install-libavformat-pkgconfig \
  > build.log 2>&1 || { tail -40 build.log; exit 1; }
# (la instalaci�n de headers falla con el make de Windows: se copian)
for l in libavutil libavcodec libavformat; do
  mkdir -p "$OUT/include/$l"; cp "$SRC/ffmpeg/$l/"*.h "$OUT/include/$l/"; cp "$BUILD/$l/"*.h "$OUT/include/$l/" 2>/dev/null || true
done
touch "$OUT/.built"
echo "ffmpeg $ABI: ok"
