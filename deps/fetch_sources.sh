#!/bin/bash
# Downloads the pinned sources of the native dependencies into deps/src (what the Windows setup does by hand), checking
# each against its SHA-256: SDL3-3.4.18, ffmpeg (9.0.2) and shaderc (2026.4) with glslang, SPIRV-Tools and SPIRV-Headers.
# Uso: bash deps/fetch_sources.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/src"
TMP="$HERE/tmp/dl"
mkdir -p "$SRC" "$TMP"

# get <name> <url> <sha256> <destination folder>
get() {
  local f="$TMP/$1"
  if [ ! -f "$f" ]; then curl -fL --retry 3 -o "$f.part" "$2" && mv "$f.part" "$f"; fi
  echo "$3  $f" | sha256sum -c - >/dev/null || { echo "fetch_sources: $1 does not match its SHA-256"; rm -f "$f"; exit 1; }
  if [ ! -d "$4" ]; then
    mkdir -p "$4.tmp" && tar -xf "$f" -C "$4.tmp" --strip-components=1 && mv "$4.tmp" "$4"
  fi
  echo "ok: $1"
}

SPIRV=1.4.363.0
get SDL3-3.4.18.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz \
    9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3 "$SRC/SDL3-3.4.18"
get ffmpeg-9.0.2.tar.xz https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
    8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e "$SRC/ffmpeg"
get shaderc-2026.4.tar.gz https://github.com/google/shaderc/archive/refs/tags/v2026.4.tar.gz \
    f06ce5bcca94e5df7f34e115743597d0ad2e13c5fe9213c67dc8c76031241947 "$SRC/shaderc"
get glslang-16.6.0.tar.gz https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz \
    9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2 "$SRC/shaderc/third_party/glslang"
get SPIRV-Tools-$SPIRV.tar.gz https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/vulkan-sdk-$SPIRV.tar.gz \
    e6c83a215538fbfd265ccf398d3877985055df1273dbed73912bd05307664688 "$SRC/shaderc/third_party/spirv-tools"
get SPIRV-Headers-$SPIRV.tar.gz https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-$SPIRV.tar.gz \
    5b3a0d099f5c7649e29c0938476c462e71d66ad89a0bde42fa1ba3eb476956c9 "$SRC/shaderc/third_party/spirv-headers"
