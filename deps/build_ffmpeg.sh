#!/bin/bash
# FFmpeg mínimo (MP4 + H.264 + AAC, como el Makefile del port) estático para Android.
# Uso: bash build_ffmpeg.sh [abi]
set -e
mkdir -p "$(dirname "$0")/tmp"; export TMPDIR="$(cd "$(dirname "$0")" && pwd)/tmp" TEMP= TMP=
ABI=${1:-arm64-v8a}
API=29
case $ABI in
  arm64-v8a) ARCH=aarch64; TRIPLE=aarch64-linux-android ;;
  x86_64)    ARCH=x86_64;  TRIPLE=x86_64-linux-android ;;
esac
SDK=$(cygpath -u "$LOCALAPPDATA/Android/Sdk")
NDK="$SDK/ndk/27.0.12077973"
TCB="$NDK/toolchains/llvm/prebuilt/windows-x86_64"
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/out/$ABI/ffmpeg"
BUILD="$HERE/out/$ABI/ffmpeg-build"
[ -f "$OUT/.built" ] && { echo "ffmpeg $ABI: ya está"; exit 0; }
rm -rf "$BUILD" && mkdir -p "$BUILD" "$OUT"
export PATH="$TCB/bin:$NDK/prebuilt/windows-x86_64/bin:$PATH"
cd "$BUILD"
"$(cygpath -m "$HERE/src/ffmpeg")/configure" --prefix="$(cygpath -m "$OUT")" \
  --enable-cross-compile --target-os=android --arch=$ARCH \
  --cc="clang.exe --target=$TRIPLE$API" --cxx="clang++.exe --target=$TRIPLE$API" \
  --host-cc="clang.exe --target=$TRIPLE$API" --ar=llvm-ar.exe --nm=llvm-nm.exe --ranlib=llvm-ranlib.exe --strip=llvm-strip.exe \
  --sysroot="$(cygpath -m "$TCB/sysroot")" --enable-pic \
  --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
  --disable-debug --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
  --enable-static --disable-shared --enable-protocol=file --enable-demuxer=mov \
  --enable-parser=h264,aac --enable-decoder=h264,aac \
  --extra-cflags="-ffile-prefix-map=$(cygpath -m "$HERE/src/ffmpeg")=ffmpeg" \
  $( [ $ARCH = x86_64 ] && echo --disable-x86asm ) > configure.log 2>&1 || { tail -30 configure.log; tail -40 ffbuild/config.log; exit 1; }
sed -i 's|^#define FFMPEG_CONFIGURATION .*|#define FFMPEG_CONFIGURATION ""|' config.h  # (no paths of this machine in the library)
# (make de Windows: rutas E:/ en vez de /e/)
sed -i "s|/e/|E:/|g" Makefile ffbuild/config.mak
export TMPDIR="$(cygpath -m "$TMPDIR")" TMP="$(cygpath -m "$TMPDIR")" TEMP="$(cygpath -m "$TMPDIR")"
make -j12 install > build.log 2>&1 || { tail -40 build.log; exit 1; }
# (la instalaci�n de headers falla con el make de Windows: se copian)
for l in libavutil libavcodec libavformat; do
  mkdir -p "$OUT/include/$l"; cp "$HERE/src/ffmpeg/$l/"*.h "$OUT/include/$l/"; cp "$BUILD/$l/"*.h "$OUT/include/$l/" 2>/dev/null || true
done
touch "$OUT/.built"
echo "ffmpeg $ABI: ok"
