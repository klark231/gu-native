# XeSS-SR headers (Intel XeSS SDK)

What the Windows build uses for the optional XeSS-SR upscaler (`[Upscaler] Upscaler = XeSS`,
`runtime/nvn/vulkan/xess.c`).

## In this folder

- `inc/xess/xess.h`, `inc/xess/xess_vk.h`, `inc/xess/xess_debug.h` and `inc/MIT.txt`: the SDK's public headers from
  its main branch at commit `de0fb9c1c510661c571164e1418ceca8101dab69` (2026-09-09, "Update license to MIT for public
  headers only"), unchanged, byte for byte (CRLF line ends, as upstream). MIT License, Copyright (c) 2026 Intel
  Corporation: `inc/MIT.txt`. Apart from their comments they declare the same API as the 3.0.2 release's headers.
  - https://github.com/intel/xess/tree/de0fb9c1c510661c571164e1418ceca8101dab69/inc
  - https://github.com/intel/xess/commit/de0fb9c1c510661c571164e1418ceca8101dab69

## Not in this folder, nor in the repository

`libxess.dll`, the XeSS runtime. It is never linked: `xess.c` loads it at run time from `MHGU.exe`'s folder, and when it
is missing, or can't run on that PC, the game uses FSR 3.1. It is shipped unmodified (its Authenticode signature
intact), never patched or hooked, as its license requires.

- Release: XeSS SDK 3.0.2 (tag `v3.0.2`, 2026-07-24), https://github.com/intel/xess/releases/tag/v3.0.2
- Download: https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip
  - `XeSS_SDK_3.0.2.zip`: 76,839,749 bytes, SHA-256 `88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990`
  - its `bin/libxess.dll`: file version 2.0.2.68, 77,795,704 bytes, SHA-256
    `251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7`
- `bash deps/build_deps.sh win64 xess` downloads the zip (or takes a copy: `XESS_ZIP=<path>`), checks both SHA-256
  values and puts `libxess.dll`, the SDK's `LICENSE.txt` and its `third-party-programs.txt` in `deps/out/win64/xess`.
  `native/build_windows.sh` copies the DLL beside `MHGU.exe` and writes `licenses/XeSS.txt` from the other two.
- License of the DLL: the Intel Simplified Software License (Version October 2022), copyright line "Intel(R) Xe
  Super Sampling (XeSS) SDK: Copyright (C) 2025 Intel Corporation". It allows redistributing the binary unmodified,
  with that notice and its terms in the distribution. It refers to `third-party-programs.txt` for the third-party
  software in the SDK.
- What it needs on the player's PC: the Microsoft Visual C++ Redistributable (x64) 14.40.33810 or later (`msvcp140.dll`,
  `vcruntime140.dll`, `vcruntime140_1.dll`), and a GPU and driver its own queries accept: Intel Iris Xe or Arc, or
  another vendor's card with `shaderStorageImageWriteWithoutFormat` and `VK_EXT_mutable_descriptor_type` (DP4a for
  speed: AMD RX 6000, NVIDIA RTX 3000 or newer).

XeSS frame generation (XeSS-FG) and XeLL are Direct3D 12 only: there is nothing of them here.
