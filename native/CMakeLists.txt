# MHGU Recomp para Android: el runtime (libmain.so, lo que carga SDLActivity) y el código del juego recompilado
# (libmhgugame.so, generado desde el dump propio con `make gen`), equivalente al Makefile del port.
#
#   cmake -DMHGU_SRC=<MHGURecomp> -DDEPS=<deps/out/<abi>> -DGAMELIB_ID=... -DSHADER_ID=... -DMHGU_VERSION=...
cmake_minimum_required(VERSION 3.22)
project(mhgurecomp_android C CXX ASM)

set(S ${MHGU_SRC})
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

if(ANDROID_ABI STREQUAL "arm64-v8a")
  # ARMv8.2: LSE atomics (the guest's exclusives are compare-and-swap), fp16, dotprod; any phone since ~2018
  set(GENARCH -march=armv8.2-a)
else()
  set(GENARCH "")
endif()

# ---------------------------------------------------------------- runtime (libmain.so)
file(GLOB RT_SRC ${S}/runtime/*.c ${S}/runtime/hle/*.c ${S}/runtime/services/*.c ${S}/runtime/net/*.c
                 ${S}/runtime/nvn/vulkan/*.c)
list(APPEND RT_SRC
  ${S}/runtime/nvn/objects.c ${S}/runtime/nvn/cmdbuf.c ${S}/runtime/nvn/nvn_args.c ${S}/runtime/nvn/tegra.c
  ${S}/runtime/nvn/formats.c ${S}/runtime/nvn/backend_null.c ${S}/runtime/nvn/astc.c ${S}/runtime/nvn/bcn.c ${S}/runtime/nvn/maxwell/translate.c
  ${S}/runtime/platform/android.c ${S}/runtime/platform/android_gpu.c ${S}/runtime/platform/android_perf.c
  ${S}/third_party/volk/volk.c
  ${S}/updater/sha256.c)

set(IMGUI ${S}/third_party/cimgui)
set(IMGUI_SRC ${IMGUI}/cimgui.cpp ${IMGUI}/imgui/imgui.cpp ${IMGUI}/imgui/imgui_draw.cpp ${IMGUI}/imgui/imgui_tables.cpp
              ${IMGUI}/imgui/imgui_widgets.cpp ${IMGUI}/imgui/imgui_demo.cpp ${IMGUI}/imgui/backends/imgui_impl_vulkan.cpp)

set(FSR ${S}/third_party/fsr-vulkan)
set(FFX ${FSR}/upstream/ffx-1.1.4/sdk)
set(FSR_C   ${FSR}/src/ffx_vk_portable.c ${FSR}/src/ffx_fsr3_glsl.c)
set(FSR_CXX ${FSR}/src/ffx_vk_fsr3_portable.cpp ${FSR}/src/ffx_fsr3_shaderblobs.cpp
            ${FFX}/src/shared/ffx_assert.cpp ${FFX}/src/shared/ffx_message.cpp ${FFX}/src/shared/ffx_object_management.cpp
            ${FFX}/src/shared/ffx_breadcrumbs_list.cpp ${FFX}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
            ${FFX}/src/backends/vk/ffx_vk.cpp ${FFX}/src/backends/shared/ffx_shader_blobs.cpp)

add_library(main SHARED ${RT_SRC} ${S}/runtime/overlay.cpp ${S}/runtime/settings_panel.cpp ${S}/runtime/touch.cpp
                        ${S}/runtime/perfstats.cpp ${IMGUI_SRC} ${S}/runtime/nvn/vulkan/vma.cpp ${FSR_C} ${FSR_CXX})

set(LIB_INC ${DEPS}/sdl3/include ${DEPS}/shaderc/include ${DEPS}/ffmpeg/include)
target_include_directories(main PRIVATE ${S}/runtime ${S}/third_party/vk_mem_alloc ${IMGUI} ${IMGUI}/imgui
                           ${FSR}/include ${S}/third_party/ffx-api ${LIB_INC})
target_compile_definitions(main PRIVATE GAMELIB_ID="${GAMELIB_ID}" SHADER_ID="${SHADER_ID}" MHGU_VERSION="${MHGU_VERSION}"
                           FSR4_DRIVER_PATCH=0 CIMGUI_NO_EXPORT)
target_compile_options(main PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -g -fno-strict-aliasing -Wall -Wno-unused-function
                       -Wno-unused-variable -Wno-unused-but-set-variable> -ffile-prefix-map=${S}=mhgu)
if(MHGU_DEV)  # (test builds: files/env.txt, files/control.txt; a public build reads neither)
  target_compile_definitions(main PRIVATE MHGU_DEV=1)
endif()
# Dear ImGui / FSR: as the Makefile's IMGUIFLAGS / FSRFLAGS
set_source_files_properties(${IMGUI_SRC} ${S}/runtime/overlay.cpp ${S}/runtime/settings_panel.cpp ${S}/runtime/touch.cpp
  ${S}/runtime/perfstats.cpp PROPERTIES
  COMPILE_OPTIONS "-O2;-fvisibility=hidden;-w")
set_source_files_properties(${S}/runtime/nvn/vulkan/vma.cpp PROPERTIES COMPILE_OPTIONS "-O2;-w")
set_source_files_properties(${FSR_C} PROPERTIES COMPILE_OPTIONS "-O2;-w;-std=c11")
set_source_files_properties(${FSR_CXX} PROPERTIES COMPILE_OPTIONS
  "-O2;-w;-std=c++17;-include;${FSR}/src/ffx_1_1_4_portability.hpp")
set(FSR_DEFS FFX_VK_PORTABLE_HAS_FSR3_UPSCALER=1 FFX_FSR3UPSCALER=1 DISABLE_FFX_DEBUG_BREAK=1)
set_property(SOURCE ${FSR_C} ${FSR_CXX} APPEND PROPERTY COMPILE_DEFINITIONS ${FSR_DEFS})
set_property(SOURCE ${FSR_C} ${FSR_CXX} APPEND PROPERTY INCLUDE_DIRECTORIES
  ${FSR}/include ${FFX}/include ${FFX}/src/shared ${FFX}/src/components ${FFX}/src/backends/shared
  ${FFX}/src/backends/shared/blob_accessors)
# (ffx_fsr3_glsl.c embeds the GLSL with .incbin: absolute paths here)
set_property(SOURCE ${FSR}/src/ffx_fsr3_glsl.c APPEND PROPERTY COMPILE_DEFINITIONS
  FFX_GPU_DIR="${FFX}/include/FidelityFX/gpu" FFX_PASS_DIR="${FFX}/src/backends/vk/shaders/fsr3upscaler")

add_library(SDL3 SHARED IMPORTED)
set_target_properties(SDL3 PROPERTIES IMPORTED_LOCATION ${DEPS}/sdl3/lib/libSDL3.so)
# FFmpeg (LGPL) as a library of its own beside the runtime, not inside it: anyone can replace it with their own build
# (-Bsymbolic: as FFmpeg's own shared build, for its assembly)
add_library(mhguffmpeg SHARED ${CMAKE_CURRENT_LIST_DIR}/ffmpeg_shared.c)
target_link_options(mhguffmpeg PRIVATE -Wl,--whole-archive ${DEPS}/ffmpeg/lib/libavformat.a ${DEPS}/ffmpeg/lib/libavcodec.a
  ${DEPS}/ffmpeg/lib/libavutil.a -Wl,--no-whole-archive -Wl,-Bsymbolic -Wl,-z,max-page-size=16384)
target_link_libraries(mhguffmpeg PRIVATE m)
set_target_properties(mhguffmpeg PROPERTIES LINK_DEPENDS  # (they are link options: a new build of them must relink it)
  "${DEPS}/ffmpeg/lib/libavformat.a;${DEPS}/ffmpeg/lib/libavcodec.a;${DEPS}/ffmpeg/lib/libavutil.a")
# (the vk* of everything in it are volk's pointers, filled by runtime/platform/android_gpu.c: the system's driver, or the
# player's own through libadrenotools)
set(ADRENOTOOLS_GEN OFF)
add_subdirectory(${S}/third_party/libadrenotools adrenotools)
target_compile_definitions(main PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_ANDROID_KHR VMA_STATIC_VULKAN_FUNCTIONS=1
                           VMA_DYNAMIC_VULKAN_FUNCTIONS=0)
target_include_directories(main PRIVATE ${S}/third_party/volk)
target_compile_options(main PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-include ${S}/third_party/volk/volk.h>")
# MHGU_XP_LIB: crates/mhgu-crossplay (Rust, built by build_native.sh): Ryujinx and Yuzu-room crossplay
target_link_libraries(main PRIVATE SDL3 mhguffmpeg adrenotools ${MHGU_XP_LIB}
  ${DEPS}/shaderc/lib/libshaderc_combined.a android log dl m z)
# The game code library binds the runtime's functions (rt_dispatch, the NEON helpers...) by name: all exported.
# The static libraries stay inside.
target_link_options(main PRIVATE -Wl,--exclude-libs,ALL -Wl,--export-dynamic -Wl,-z,nostart-stop-gc)

# ---------------------------------------------------------------- game code (libmhgugame.so)
if(EXISTS ${GEN}/main)
  file(GLOB GEN_SRC ${GEN}/main/*.c ${GEN}/sdk/*.c)
  add_library(mhgugame SHARED ${GEN_SRC} ${S}/tools/gamelib_entry.c)
  target_include_directories(mhgugame PRIVATE ${S}/runtime)
  # (exactly the Makefile's GENFLAGS, but for this CPU; the same string goes into GAMELIB_ID)
  target_compile_options(mhgugame PRIVATE -O2 -fno-strict-aliasing -ffp-contract=off -fno-math-errno -Wno-return-type
    -Wno-unused-label -Wno-parentheses-equality -Wno-unused-value -fvisibility=hidden ${GENARCH} -w
    -fno-stack-protector -U_FORTIFY_SOURCE -g0 -fno-plt)  # (the NDK adds stack checks and debug info: not for this code)
  set_property(SOURCE ${S}/tools/gamelib_entry.c APPEND PROPERTY COMPILE_DEFINITIONS GAMELIB_ID="${GAMELIB_ID}")
  target_link_libraries(mhgugame PRIVATE main)
  # the recompiled code jumps far: no range limits from the linker's thunks matter, but keep sections apart
  target_link_options(mhgugame PRIVATE -Wl,-z,max-page-size=16384)
endif()
target_link_options(main PRIVATE -Wl,-z,max-page-size=16384)
