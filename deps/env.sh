# Sourced by build_deps.sh, build_ffmpeg.sh and native/build_native.sh: where the Android SDK, NDK and CMake are.
# Windows (Git Bash): Android Studio's SDK in %LOCALAPPDATA%. Linux (also GitHub Actions): $ANDROID_SDK_ROOT / $ANDROID_HOME.
NDK_VERSION=27.0.12077973
if command -v cygpath >/dev/null 2>&1; then
  HOST_OS=windows; HOST_TAG=windows-x86_64; EXE=.exe
  SDK=$(cygpath -u "$LOCALAPPDATA/Android/Sdk")
  CMAKE="$SDK/cmake/3.22.1/bin/cmake.exe"
  NINJA="$SDK/cmake/3.22.1/bin/ninja.exe"
else
  HOST_OS=linux; HOST_TAG=linux-x86_64; EXE=
  # (the scripts call cygpath -m / -u on paths: here a path is already the right one)
  cygpath() { local a; for a; do :; done; printf '%s\n' "$a"; }
  SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
  CMAKE="$SDK/cmake/3.22.1/bin/cmake"; [ -x "$CMAKE" ] || CMAKE=$(command -v cmake)
  NINJA="$SDK/cmake/3.22.1/bin/ninja"; [ -x "$NINJA" ] || NINJA=$(command -v ninja)
fi
NDK="$SDK/ndk/$NDK_VERSION"  # (that exact one: the runner's own ANDROID_NDK_HOME can be another)
[ -d "$NDK" ] || { echo "env.sh: NDK $NDK_VERSION not found in $SDK (sdkmanager \"ndk;$NDK_VERSION\")"; exit 1; }
