# MHGU for Android: project notes

A native Android port of Monster Hunter Generations Ultimate 1.4.0, built on MHGURecomp: the game's 32-bit ARM code is translated
to C, and a host runtime reimplements the Switch's services and its NVN graphics API on Vulkan.

This file has two uses: helping someone set the game up, and continuing the project.

## Rules

- This project never provides game files. Don't obtain, link to or explain how to get ROMs or keys, and don't explain how to dump a
  console. The person brings their own `exefs` and `romfs`.
- Never commit or publish `exefs`, `romfs`, `generated/`, `libmhgugame.so`, `libmhgugame.id`, saves or signing keys. Never put game
  code in an APK or a release: `libmhgugame.so` is made from the game, so each person makes their own with `tools/build_game.py`.
- Keep `LICENSE` (the project's own license from 0.1.7: no redistribution without the author's written permission) and
  `THIRD_PARTY_NOTICES` (MHGURecomp's MIT license and the other libraries' licenses require it).
- Don't lower someone's graphics or audio settings without asking.
- Any new menu, notice or touch control must match the game's style: bronze, gold and wine-dark, with no default Android look.

## Helping someone set it up

Follow `README.md`. In short:

1. Check that a folder holds `exefs/main` and a `romfs` with 35,531 files, from version 1.4.0.
2. Run `python tools/build_game.py <that folder>`. It needs Python 3 on Windows or Linux, installs `lz4`, and downloads NDK r27 to
   `~/.mhgu-tool` if there is none (`--ndk <path>` uses another). It leaves `libmhgugame.so` and `libmhgugame.id` in the folder.
3. Copy the folder to the device, anywhere except `Android/data`. With adb: `adb push <folder> /sdcard/MHGU`.
4. Install the APK from Releases. For Samsung's Game Booster: `adb install -r -i com.mhgu.recomp <apk>`.
5. The person opens the app, grants file access and chooses the folder.

To diagnose a device: `adb logcat -s MHGU`, or `Android/data/com.mhgu.recomp/files/log.txt`.

## Layout

- `runtime/`: the host runtime. `touch.cpp` touch controls and editor, `settings_panel.cpp` the menu, `nvn/vulkan/` the renderer,
  `hle/` game hooks, `platform/android*.c` Android, `config.c` settings and first-run defaults.
- `tools/`: `gen.py`, `lifter.py`, `neon.py`, `nso.py` translate the game; `build_game.py` is the person's tool.
- `android/`: the app. `GameFolderActivity` is the entry screen, `GameActivity` runs the game, `SaveImportActivity` imports a save
  (it is only applied at the next start, before the game runs, so the game can't save over it).
- `native/`: CMake and `build_native.sh`. `deps/`: scripts for SDL3, shaderc and FFmpeg.

## Building

Windows, in Git Bash, with the Android SDK, NDK 27.0.12077973, CMake 3.22.1 and Android Studio's JBR.

1. Put the sources in `deps/src/`: `SDL3-3.4.18`, `ffmpeg` (9.0.2) and `shaderc` (2026.4, with glslang 16.6.0 and SPIRV-Tools and
   SPIRV-Headers 1.4.363.0 in its `third_party`). Then run `bash deps/build_deps.sh arm64-v8a` and `bash deps/build_ffmpeg.sh arm64-v8a`.
2. `bash native/build_native.sh arm64-v8a` builds the runtime. It first builds `crates/mhgu-crossplay` (Rust: crossplay with Ryujinx, Yuzu-family rooms and Citra-family rooms), so it needs rustup with the `aarch64-linux-android` target (`x86_64-linux-android` for that ABI) and, the first time, network access for cargo. With `MHGU_DEV=1` in front, a test build.
3. `cd android && ./gradlew assembleRelease` makes four APKs (`app-standard-release.apk`, `app-antutu-release.apk`, `app-ludashi-release.apk`, `app-alt-release.apk`; one with `assembleStandardRelease` etc.). The other three have the package names `com.antutu.ABenchMark`, `com.ludashi.benchmark` and `com.mhgu.recomp.alt` and the labels "MHGU Antutu", "MHGU Ludashi" and "MHGU Alt" (Alt: a second install beside the standard one): the code is the same (namespace `com.mhgu.recomp`), but each package has its own `Android/data/<package>/files` (settings, log, saves) and none installs over the real benchmark. It signs only if `android/keystore.properties` and `mhgu.keystore` exist.

The public APK leaves `libmhgugame.so` out. `-Pwithgame` keeps it for your own tests: never publish that APK.

`GAMELIB_ID` is a hash of the generator and of the headers listed in `tools/build_game.py`, `runtime/gamelib.h` and
`hle_symbols.txt` included. If any of them changes, every player must run the tool again, and the app says so. `runtime/*.c` can change
without that: new hooks use `fnhook_set`, new declarations go in another header. Check the id before releasing.

## Building for Windows

In Git Bash, with llvm-mingw 20241217 (UCRT) and the Android SDK's CMake 3.22.1 and Ninja. No MSYS2 needed.

1. The same sources, plus `Vulkan-Headers` (tag vulkan-sdk-1.4.363.0), in `deps/src/` or a folder named by `DEPS_SRC`. Then
   `bash deps/build_deps.sh win64` and `bash deps/build_ffmpeg.sh win64`; `LLVM_MINGW=<its folder or its bin folder>` if it isn't in the scripts' default place.
2. `bash native/build_windows.sh` builds `MHGU.exe` and `mhguffmpeg.dll`, and leaves them with `SDL3.dll` in `native/build/win64/MHGU`.
3. `python tools/build_game.py <folder with exefs and romfs>`, the players' same command, translates once and leaves both
   `libmhgugame.so` (+ `.id`) and `mhgugame.dll` (+ `mhgugame.id`) in that folder; one already up to date is kept, so a player who made the
   phone's only gets the PC's added. `--android` / `--pc` make one only, `--force` both again. It finds or downloads the NDK and
   llvm-mingw (`--ndk`, `--llvm-mingw`). The Android library it makes is the same, byte for byte, as before the PC part existed.
4. `MHGU.exe` finds the folder by itself (`runtime/gamefolder.c`): the one chosen before (`game_folder.txt` beside the settings), `data`
   beside the executable, or its own folder; else it asks with the system's folder dialog, with the checks and messages of Android's
   `GameFolderActivity` (in English or Spanish), and then loads `mhgugame.dll` from that folder.

- `mhgugame.dll` imports the runtime's functions from the executable by name, so it only loads into `MHGU.exe`: don't rename it. The
  tool needs no copy of it: a first link lists what nothing else provides, and `llvm-dlltool` makes the import library from that. The
  variables among them are reached through clang's `.refptr` pointers and go in as `DATA`; a variable that went in as code would be read
  through a jump thunk.
- The game code is built for x86-64-v2 (SSE4.2: any 64-bit processor from 2009 on): `runtime/gamelib.c` refuses an older one
  with a message. The game has no fused multiply-add, and x86-64-v3 measured no faster; never `-march=native`.
- The renderer's `vk*` are volk's pointers here too, from the Vulkan loader SDL opened for the window (`nvn/vulkan/device.c`).
- The Android app's own screens are `runtime/pc_actions.c` here: Game folder, Import / Export save, Log folder, Add cheats, Add a mod
  use SDL3's file dialogs (they answer on their own thread; `settings_panel_request_view` opens the menu's confirmation, `overlay_notice`
  tells the result), and Licenses is a page of the menu reading `licenses/` beside the executable. An imported save waits in
  `save_import/` beside the settings and is put in place at the next start, before the game runs, the current one copied to
  `save_backups/<date>` beside the save folder. `pick folder|save|export|cheats|mod <path>` in the control file answers a dialog.
- Add a mod takes the mod's folder or its `.zip` (`runtime/unzip.c`, the project's own reader: stored and Deflate, ZIP64; every path
  is checked before anything is written). The Cheats and Mods tabs remove a cheat file (its cheats stop at once: `cheats_drop_file`) or
  a mod. A mod the game loaded at its start is only hidden (`mods/_remove.txt`, read by `mods_installed`) and deleted at the next start
  (`pc_apply_mod_changes`), since the game reads its files while it runs; one added again while in use waits as `mods/.update-<name>`.
  Deleting never goes through a link (a junction, a symbolic link: `is_link`): the link alone goes, and what it points to stays.
- `native/build_windows.sh` also puts `pipelines.list`, `licenses/` (with llvm-mingw's runtime, linked into MHGU.exe) and
  `extras/comer_rapido.txt` (the optional cheats, copied into `cheats/` and turned off at the first start) in the program's folder.
- The NDK's Windows `make` cuts command lines at about 8 KB: `deps/build_ffmpeg.sh` installs the libraries without `make install`,
  whose header step was cut and wrote into `deps/src`.
- Test runs: `MHGU_HOME` (settings, caches, log, `shots/`), `MHGU_DATA_DIR` (the game folder), `MHGU_SAVE_DIR` (saves: otherwise
  `%APPDATA%\MHGURecomp\save`), `MHGU_CONTROL` (the control file above), `MHGU_WINDOW=1280x720`, `MHGU_FPS=1` (frame rate in the
  log), `MHGU_FPS_LIMIT=240`, `MHGU_VSYNC=0`, `MHGU_QUICKHUNT=101`. `sdlquit` in the control file closes it once `[Application]
  ConfirmExit = Off`. `MHGU_HIDDEN=1` still draws (shots, `gpuprof`) without showing a window: measured that way, nothing caps the
  frame rate but the limit, where a visible window on the desktop stopped at the monitor's refresh rate. `sdlkey F1` opens the menu
  and `mouse <x> <y> click` presses its rows and tabs.
- Measured on an RX 6800 XT (Ryzen 5 3600XT), a 1920x1080 window, no limit and no VSync: 200 to 237 fps in menus, about 205 in the
  village, 140 to 165 in a Jurassic Frontier quest (the monitor's refresh, see MHGU_HIDDEN above). Hidden, 3D at 3440x1440: the 240
  limit in that quest, the GPU's frame 1.8 to 2.3 ms, of which the window image's pass (the picture copied in, the overlay) 0.2 to
  0.6 ms. AMD's Windows driver has no mailbox present mode: VSync on is FIFO.
- Below 100 % RenderScale a desktop always uses `fsr.c`'s plain scaling (`config.c`), Low-end mode or not: the 3D picture small,
  the HUD, the text and the game's last draw at the window's size (Borderless with a smaller Resolution: the screen's, as FSR),
  without the phone's caps. It finds the start of the HUD as FSR does, so Bloom on is the usual case (checked at 50 % and 33 %,
  with Bloom on and off). The player's MSAA (the 3D's passes, before the HUD) and FXAA stay; menus' 3D models drawn after the
  start of the HUD come out at the window's size with no anti-aliasing, as with FSR (`models_aa`; the 3D resolution's help says
  so); SmoothGradients keeps H wide (`vk_fsr_input`). A desktop's Low-end mode: the menu sets RenderScale for a 3D picture about
  720 lines tall at the Resolution of the moment (`config_lowend_scale`: 66 % at 1080p, 50 % at 1440p, 33 % at 4K) and Bloom
  off, and MSAA (F7 too) and FSR are off; FXAA, shadows, texture filtering, the frame rate and reverb stay. Toaster: about 540
  lines, 30 FPS, no shadows, FXAA or anisotropic filtering (trilinear stays: NEAREST mips showed as steps on the floors).
  Leaving either, the menu also takes back an older PC build's phone values (40 %, Toaster 25 %, 30 FPS, no shadows, Toaster's
  reverb off), which its files still have. Android keeps a phone's values.
- The log is written by a thread of its own (`runtime/logwriter.c`): a line costs the thread that logs a few microseconds. Before,
  each was an `fprintf` to the unbuffered stderr, which the C runtime writes to a file a character at a time (~150 us a line), and
  with the log on a hard disk lines took up to 28 ms on the game's threads. A full 1 MB queue drops lines and the log says so; FATAL,
  a crash and exit write the queue out first (`rt_log_flush`). `MHGU_LOG_ASYNC=0`: each line written by the thread that logs.
- Thread priorities (`runtime/platform/windows.c`): guest threads above normal up to Horizon priority 43, normal from 44 on (never
  under normal, where the runtime's own normal threads kept a lock's holder off the CPU; never highest), the update and presenting
  threads, the vsync tick and the audio mixer at least above normal, background work below normal (`plat_thread_priority`). A thread
  waiting on a guest mutex lends the owner its level until the owner lets it go (`kernel.c` `prio_lend`), as Horizon does; the hitch
  log's `prio-lend` counts it. The process opts out of power throttling (EcoQoS and ignored timer resolution). `MHGU_PRIO=0`: all
  normal; `MHGU_PRIO_LEND=0`: nothing lent; `MHGU_QOS=0`: throttling left to Windows. The log's `[plat] thread priorities` line says which.
- Guest kernel waits (`runtime/kernel.c` `kwait`): each guest thread sleeps on an auto-reset event of its own, a timed wait until its
  deadline on the thread's high-resolution timer (`plat_event_wait_until`). winpthreads' timed waits end on the system's 15.6 ms tick:
  a 1 ms guest wait took 15 ms (median), a 16 ms one 31; now 1.1-1.3 and 16.3, p99 about 0.6 ms late (the timer's 0.5 ms tick here).
  A wait that times out as it is woken counts as woken, as in Horizon. `MHGU_KWAIT_PTHREAD=1`: the condition variables as before; the
  log's `[kernel] guest waits` line says which. Android and Linux: the condition variables, unchanged.

## Testing on a device

A test build (`MHGU_DEV=1`) reads two files in `Android/data/com.mhgu.recomp/files`; a public build reads neither:

- `env.txt`: `NAME=value` lines. `MHGU_QUICKHUNT=101` goes straight into a quest, `MHGU_VK_STATS=1` logs statistics,
  `MHGU_TOUCH_LOG=1` logs fingers. `MHGU_HOME=<folder>` moves the settings and the log, but not the saves: those are always
  `Android/data/com.mhgu.recomp/files/MHGURecomp/save`, so copy them first. `MHGU_CAMLOG=1` logs the hunt camera and the monsters,
  `MHGU_LOG_MOVES=1` the hunter's actions, `MHGU_LOG_NET=1` the network, `MHGU_PAD_DEBUG=1` what SDL reads from a controller,
  `MHGU_FORCE_NOLSE=1` the adapted game code on any CPU, `MHGU_BCN_LOG=1` the time big BC textures take.
- `control.txt`: appended commands, read while the game runs. `A 8`, `DOWN 8`, `type <text>`, `textkey enter`,
  `memsave <hex address> <hex size> <file>`, `peek`, `poke`, `gpuprof` for one frame's time per render pass, `hudtrace` for one
  frame's HUD layers, and `crash` for the error screen. Create `control.txt` from the shell with `chmod 666`, or the app can't read it.

What the app writes to `cache/` under `Android/data` can't be deleted over adb, so a test folder under `MHGU_HOME` stays until the app removes it.

Empty `control.txt` before starting: old commands replay. `3D resolution` needs a restart: until then the menu says so and the old
one is still running.

**Read frame rates from the on-screen counter.** `[vk] 2s waits` counts submits, and a frame takes two of them.

Android killing the game shows in `adb logcat` as `lowmemorykiller: Kill 'com.mhgu.recomp'`.

## What real devices taught us

- Mali has no BC textures, so they are decoded on the CPU and take 4 to 8 times the memory. BC4 and BC5 go to R8 and RG8, and under
  5 GB of RAM textures with mipmaps lose their largest level (`mip_skip`, `nvn/vulkan/textures.c`). Without this a 4 GB phone was killed.
- On a Mali, more than half of a hunt's frame is some 30 full-screen passes, about 0.15 ms each whatever their size. Low-end mode skips
  the bloom and depth of field chain (`lowend_skipped`, `nvn/vulkan/exec.c`), and the two draws that add it to the picture must go too.
  A render target that is never drawn gets uploaded from guest memory when sampled: that garbage, added over the picture, is coloured noise.
  So skipped targets are marked with `renderer_mark_gpu_owned`. Below 100 % it also keeps the HUD at the screen's size through
  `fsr.c`'s plain scaling path, without FSR. `MHGU_VK11=1` was tested with it, only by forcing that path on a newer driver.
- FXAA leaves a 2 px green line at the edges on Mali and costs about a fifth of the frame rate, so it is off by default on Android.
  The cause is unknown.
- A Vulkan 1.1 driver (seen: Mali-G57 MC2, 1.1.177) has no timeline semaphores: each submission gets a fence and one thread waits for them
  (`nvn/vulkan/exec.c`). It also gets SPIR-V 1.3 and no FSR. `MHGU_VK11=1` in `env.txt` forces that path on a newer driver.
- That same Mali has no `shaderInt64`: with `maxwell_no_int64` the shaders do their 64-bit math in 32 bits (`nvn/maxwell/translate.c`).
  `MHGU_VK_NO_INT64=1` forces it. A forced path only shows that the path works: a real old driver can still differ.
- A driver can make more window images than it was asked for. Never assume the count: `VK_SC_MAX`, `nvn/vulkan/device.c`.
  `MHGU_VK_IMAGES=<n>` asks for that many.
- Samsung ignores the manifest's game category. Only the one set by the installer counts: see `GameFolderActivity.declareGame`.
- On Android, a `SUBOPTIMAL` result from present must not recreate the swapchain: doing so cost two thirds of the frame rate.
- The hunter has one class per weapon: never compare against one vtable. Virtual 94 tells a hunter of any weapon and whether the
  weapon is out (`runtime/hle/weapon.c`). The `sPlayer` slot can hold another object that points to the hunter.
- Don't trust an offset measured in one session: check it in another one.
- The game code is built for ARMv8.2, and its 2564 `casal` / `swpal` are illegal on an ARMv8.0 CPU (Cortex-A53, A73 alone): SIGILL at the
  first critical section. `runtime/gamelib_lse.c` writes a private copy of the library where each jumps to an exclusive load / store
  sequence in an added segment. `MHGU_FORCE_NOLSE=1` tests it; no device here lacks LSE, so only logs of those phones confirm it.
- A LAN search has to wait: on Wi-Fi a broadcast waits at the router for a sleeping device's next beacon, and 400 ms found nothing. It
  now listens 1.5 s and asks every 0.5 s, on each interface's own broadcast address too, with a Wi-Fi multicast lock held.
- `touch_hud_menu` decides the touch layout from the HUD's layers. Layer 97 is the game's menus, but aiming a heavy bowgun draws there too
  (`weapon_aiming`), and the multiplayer menu and the hub form are on layer 100, as stretched panels, which the HUD's parts there are not.
- The hunter: heading at +0x14a0, in the camera's units, (x, z) = (sin, cos); written, the hunter turns at once. Place at +0xa0, zone at
  +0x1054, which monsters have too. The classes are `uPlayerQuest00..15` in weapon order, no 05: 04 and 06 the bowguns, 10 the bow.
- In towns the game's camera does not turn, with any stick: it isn't a controller problem.
- Heat: `GameActivity.pickRefreshRate` takes the lowest screen mode that fits the FPS limit, never under 60, because forcing the fastest one
  shows nothing more. `efficiency_follow` (`android_perf.c`, API 35) lets the system use efficient cores while frames use under
  half the target, and stops at once when they near it. `MHGU_ADPF_EFFICIENT=0` turns it off. A phone that isn't charging gives real watts:
  `/sys/class/power_supply/battery/current_now` and `voltage_now`. Measured in a hunt, standing: the FPS limit is what counts. Moto g06 at 60 FPS
  3.37 W, at 30 FPS 2.70 W; S25 Ultra at 120 (106 real) 3.48 W, at 60 2.33 to 2.73 W. 60 Hz against a faster screen showed no gain beyond the noise on either.
  A Galaxy denies those files: use `dumpsys battery` (Charge counter, in steps of 4.9 mAh); its `dumpsys thermalservice` repeats a stale value.
- `MHGU_FS_NEW=1` logs each fragment program the first time it draws; `MHGU_FS_WATCH=<hash>` saves the frames where one draws. A hit draws
  `cc207b43683737c8`, a 17-sample radial blur. The device's `shots` folder keeps old files: go by the names in the log, not by what's there.
- Started with the phone locked or upright, the display says portrait: the 3D picture came out 720x576 with black at the sides. `config.c` now
  swaps the two. Start test runs with the screen on and unlocked anyway: `am start` on a locked phone doesn't show the game.
- The game's Bow Controls (`sGameControl` +0x6c): Type 1, X draws and R enters the aiming mode; Type 2 the other way round. Read `weapon_bow_type`, or a
  bow's layout presses the wrong button. Letting go of a button that doesn't draw does nothing, which looks like a broken bow.
- `HN_WeaponControlsMsg_eng.gmd`, `HN_HunterTipsMsg_eng.gmd` and `StartMenuMsg_eng.gmd` in `romfs/nativeNX/eng/table` are the game's own manual: read them
  before guessing what a button does. L + X and L + B change the ammo, and the game takes L only after about 300 ms.
- The aim's pitch: hunter +0x14b8 (a float, -100 down to 100 up) and +0x2714 (16-bit units, 45 degrees up, 30 down). A shot that isn't aimed ignores them, so a
  locked shot from a bowgun goes through quick aim (`touch.cpp`, `assist_*`). With a gun aimed or a bow drawn the hunt camera's handler does not run: per-update
  work goes in the item bar's hook (`hle/itembar.c`).
- A menu can't be told from a window over the hunt: no flag in the hunter or the game's globals changes, and the hunt camera runs under menus too. `game_menu`
  treats a window as a menu when the hunter isn't being played or a button that opens one was pressed just before.
- The renderer's `vk*` are volk's pointers (`third_party/volk`, force-included by `native/CMakeLists.txt`), filled from `runtime/platform/android_gpu.c`: the
  system's `libvulkan.so`, or the player's driver through `third_party/libadrenotools`. `volkLoadDevice` is not used, so calls go through the same loader
  entry points as before. A trial file sends a driver that never drew back to the system's. The hook libraries must stay extracted (`extractNativeLibs`).
- Custom drivers, tested on an Adreno 830 with a marked copy of its own library, not yet with Turnip. A library that can't be opened doesn't stop the game:
  `hook_impl` falls back to the system's, and so does one named `vulkan.adreno.so` (the system's, already loaded); `gpu_driver_check` notices
  both (below). `AdrenoVK: Driver Path` in logcat tells which one runs. A driver that dies before drawing goes through the trial file.
- Adreno 630 on Android 10 (driver 1.1.87): SIGSEGV in `vulkan.sdm845.so+1dd318`, from `vkCmdClearAttachments`. Drivers under 1.1.128 clear with draws
  (`clears_drawn`, `MHGU_CLEAR_FORCE=1` tests it anywhere). Adreno with Vulkan 1.1: below 720 lines and without MSAA the 3D picture repeats in a grid, so
  `msaa_floor` keeps 2x unless `[Performance] AdrenoFix = Off` (an Adreno 610 on 1.1.128 looked dark and flickered with it). Its cause is unknown.
- A romfs under 12.3 GB with all 35,531 files is an incomplete copy: a black picture, then `no function at 0`. The log warns.
- `Config` gets its defaults by position: `CONFIG_DEFAULT_VALUES` in `config.c`. A new field in `config.h` needs its value there, in its
  place, or every later field moves (two added once turned most touch buttons off).
- The shadow maps are one 1024x3072 depth atlas (three cascades), the first pass of a hunt's frame: 47 draws, 2.8 ms of a Moto g06's 18.6.
  The game clears it with three full-size draws, not a clear. `[Performance] Shadows = Off` clears it once and skips its pass (`shadow_skipped`).
- The chat's log is a layer 97 window at the bottom left (x 30..605, y 547..708 of the 1280x720 canvas) that stays seconds after a line: it
  must not count as a menu (`touch_hud_menu`), or the menu pad shows over the hunt.
- A model's skeleton: +0x490 the joint count, +0x494 the array, 0xa0 bytes each, world position at +0x40 (w 1.0), parent at +0x91 (0xff the
  root). The root is the pelvis. Locked shots aim there (`mousecam.c body_center`).
- `camera_fovy` compares the camera's aspect with the screen's with a tolerance: a small 3D picture's rounding made them differ at 33%.
- DLC: the game asks `aoc:u` only from its main menu's Download, which needs its online services and stops with an error without them,
  even with `MHGU_FAKE_NET=1`. `services/dlc.c`, the RomFS images and the importer are in place; the menu row shows with `MHGU_DLC=1`.
- Cheats, mods and DLC are read from the game folder's `cheats/`, `mods/` and `dlc/` too (a file manager reaches it, not `Android/data`).
  `ImportActivity` copies there; imported cheats start off (`cheats/_off.txt` in the app's folder).
- Every minute the log has a `[mem]` line: memory map entries, resident memory and VMA's blocks. Games on PowerVR closed after 5 to 15
  minutes out of map entries; there VMA is told Vulkan 1.0 so images share its blocks (`MHGU_VMA_DEDICATED=1`: the old way).
- A player's driver is checked once the device is made (`gpu_driver_check`): no library from its folder loaded means the system's runs,
  and the choice moves to `failed`. `DriverActivity` renames a library named as the system's (`vulkan.adreno.so`) to `vulkan.player.so`.
- The Khronos validation layer: put `libVkLayer_khronos_validation.so` (Android binaries of Vulkan-ValidationLayers) in `jniLibs` for a
  test build only, and `MHGU_VK_VALIDATION=1`. Never in a public APK.
- `minSdk` is 28 (Android 9). `./gradlew lintRelease` lists calls newer than that (`NewApi`); `native/build_native.sh` builds for API 28.
- Built for API 28, every `__thread` variable is emulated TLS: a call to `__emutls_get_address` and a `pthread_getspecific` per access.
  `rt_dispatch`'s cache was one, on every indirect jump of the game (a tenth of a game thread on a Moto g06): it is now per `Cpu`, through a
  pointer in `Cpu._pad` (`dispatch.c`, bytes the game's code never touches; `cpu.h` itself must not change). Keep `__thread` out of hot paths.
- Profiling: a test APK with `<profileable android:shell="true"/>` (put it in, build, take it out), NDK `simpleperf record --app` on the
  device, `binary_cache_builder.py -lib native/build/arm64-v8a`. `--call-graph fp` stops at the Mali driver; `dwarf` gets through. The
  stripped Mali library shows its own code as `stdlibp_neon_f32x16_to_f16x16`: that is the driver recording draws, not a conversion.
- The audio renderer had 70 effect slots of 1.7 KB, searched for every order number on every mix: over half the audio thread. `fx_order`
  lists them once after an update (`services/audio.c`). `getenv` walks the whole environment: never call it per frame, cache it.
- `gpuprof` shows the window image's pass (the game's picture copied in, the overlay) as its last line. The game's own passes all load
  their attachments; one whose first commands clear an attachment whole begins with `LOAD_OP_CLEAR` and those values instead
  (`noload_clears`, `render_pass`), and the clears it already did are skipped (`pass_skip_clear`). Not `DONT_CARE` plus the clears: an
  Adreno 830 then drew the scene black but for some tiles. Anbernic (Mali-G57) GPU frame -6 %, Adreno 830 -0.7 %. `MHGU_VK_NOLOAD=0`: off.
- Descriptor sets: `stage_set` reuses one written earlier in the same submission when every write is the same (about a quarter of the
  sets), and draw skips binding the same two again (`SS_SETS`; any other graphics bind forgets them). Zeros, the identity rescale table,
  the alpha reference and each stage's driver constants are copied once per submission (`shared_*`, `zero_ubo`), or every set would be
  new; a storage buffer of zeros is shared only by shaders without `uses_gmem_store`. `MHGU_VK_SETREUSE=0`, `MHGU_VK_SHARED=0`: off.
- Texture uploads (`nvn/vulkan/textures.c`): a texture's first upload is hashed and copied into staging memory by three worker
  threads in bands of about 1 MB, and the submission waits for them (`vk_upload_jobs_finish`): nothing draws early or half uploaded.
  The list's own CPU-side writes (ClearBuffer, CopyBufferToBuffer, UpdateUniformBuffer, ReportCounter) run at once, so one over
  memory a queued band has yet to read waits for the bands first (`vk_upload_jobs_guard`, `tex-guard` in the hitch log): a draw
  before the write sees the old contents, as on the console's GPU. What each texture's queued bands read is kept until they are
  done (`up_range`); a write over none of it waits for nothing.
  Under 256 KB, and formats decoded on the CPU (BCn on a Mali, ASTC), stay on the render thread. `tegra.c` copies a GOB whole (into
  staging memory 5 to 9 times the old speed). On a desktop the staging ring's next 32 MB chunk is made ahead on its own thread: made on
  the spot it cost 34 ms inside a frame. `MHGU_TEX_ASYNC=0`, `MHGU_VK_SPARE_CHUNK=0`: off. A texture that changes on 120 frames in a
  row is named once in the log (`[tex] changed and uploaded ...`).
- A Galaxy (Samsung) refuses every simpleperf event for apps (`cpu-clock`, `task-clock`, `cpu-cycles`): profile the CPU on another
  phone. Its frame is CPU-bound: the GPU span in `gpuprof` includes the gaps where the GPU waits for the render thread.
- `[Performance] LowEnd` is Off, On or Toaster (2). On a phone (a desktop's: Building for Windows) everything that tests `low_end`
  treats Toaster as On; Toaster adds a HUD at most 540 lines tall and smaller BC textures, and the menu sets 25 %, 30 FPS, Shadows,
  Bloom and Reverb off. `[Performance] Bloom` (the bloom and depth of field chain, `lowend_skipped`; not with real FSR: the plain
  scaling sets `g_fsr_on` too, so test `g_config.fsr`) and `[Audio] Reverb` are switches for everyone. A file with LowEnd on and no
  Bloom key gets Bloom off, as before.
- Test runs can override the device's settings without touching its ini: `MHGU_LOWEND=0|1|2`, `MHGU_SCALE`, `MHGU_SHADOWS`, `MHGU_BLOOM`,
  `MHGU_FPS_LIMIT`, `MHGU_NO_REVERB=1`. A phone on its charger reads charging current: unplug it to measure watts. At 30 FPS the GPU runs at
  lower clocks, so `gpuprof` times are longer: compare frame rates or watts, not milliseconds, across FPS limits.
- Internet hubs (`net/ldn.c`, the `server/` hub server): the Network page shows Hub server, Matchmaking key and Check hub server on
  Android too; empty Server is `LDN_DEFAULT_SERVER`, two public servers (a list of up to `MAX_HUBS` = 8, separated by spaces or commas). Hosting lists the hub on every server under link versions 2 and 3 (`LINK_VERSIONS`, one thread each); searching asks every server for each version (and the `public-phone` room without a key), and a found hub remembers its server, version and room for joining. With no Key a hub is listed in the public room under v2 and v3 and in the `public-phone` room under v3 (3 listings a server: the server limits hubs per IP to 4 by default), so PCs and phones of both builds see each other. A host accepts hellos and LAN probes of either version; `discover` tries each version on `Search hosts` addresses.
- `[Touch] Layout` = Traditional (`touch.cpp`, `trad()`): the hunt's separate-attack controls (`C_HEAVY` A, `C_LIGHT` X, `C_DODGE` B, `C_SHEATHE` Y, R, L, ZR, ZL, +, -, d-pad, stick) as plain lettered discs, always: no menu layout, no menu pad, no ranged one (`game` and `ranged` are forced off in `touch_input`). Its own file, `touch_layout_traditional.txt`, and its own defaults; `from_right` puts L and ZL at the left and + at the right there. Switching in the settings applies at once (`switch_scheme`).
- While a menu of the game's is open the touch layout doesn't change: the menu pad shows besides (`C_MPAD`, `[Touch] ShowMenuPad`,
  saved as `menu_pad` in touch_layout.txt) and presses the d-pad, its page tabs above it (`C_ML` / `C_MR`: the pair the game's option
  turns pages with, `menu_pages_zl`), the other pair above them with `ShowMenuZL` / `ShowMenuZR` (off by default) and a small Minus
  below it while the hunt's is hidden (the item box opens its other sections with it). The game reads what the pad alone presses
  only while a menu takes input: a menu's cursor (`cCursor`, `cBoxCursor`, a quantity, a yes / no: hle/mousemenu.c, hooked on both
  platforms) ran in the game's last update, or the hunter isn't played (`menu_takes_input`, gated in hle/input.c as the game reads
  the controller). A hunt's menu closing runs no cursor while its windows still fade out, and the hunter takes commands again from
  the next update on (an X there drew the weapon): a d-pad press then is a Hunter Art. The stick keeps walking (the pause menu, the
  pouch and the town's item box don't read the stick); it moves the cursor where the hunter isn't played (the title, the main menu)
  and in a town's menu once the player hasn't moved for 300 ms of tilt (the item box, a shop). Test with `finger` and `touchedit`
  (MHGU_CONTROL); `menutrace` shows each pad update's buttons and how many updates ago a cursor ran.
- The HUD's parts (`runtime/hud_parts.h`: the item bar, the Hunter Arts, the target camera, the clock, health and stamina, the
  sharpness and gauges, the party, the minimap) are found by layer and place in `cbuf_info` (`nvn/vulkan/exec.c`; the status, layer
  103, split by region, a piece in none going with the nearest part: `hudtrace` says which part and "nearest") and placed by
  `hud_place_*`, which the touch editor uses for its estimates too. Two arrangements, saved as `hud_<key>` and `pc_hud_<key>` in
  touch_layout.txt (offset, size, opacity, hidden; files with only offset and size read the same): `HUD_TOUCH` while the touch
  controls show with MoveHUD On, `HUD_PLAIN` otherwise (a controller, a keyboard, MoveHUD Off); the touch editor edits either
  (its HUD switch), a desktop's Move the HUD all of the plain one's (the same frames and names: draw_part_frames, part_frame). Left at their defaults the parts land exactly where
  they always did: keep it so. A menu of the game's is told by `touch_game_menu`, not by a layer 97 window (the chat's log, a
  bubble, an aim, the Quest Complete stamp are drawn there too): with one open a part takes only pieces at places it had in the
  last 1.5 s, and a moved part on its window isn't drawn. The hunt's pause menu and pouch draw only the status, before their
  windows; the minimap has no scissor.
- Bugs that only show on some devices are common here. Test on a real phone, and on a device without Snapdragon.
- Citra Room crossplay (`[Multiplayer] Crossplay = Citra Room`, `crates/mhgu-crossplay/src/citra_bridge.rs`): the game is one 3DS in an
  Azahar / Citra room. MHXX players' networks come from their beacons (`uds.rs`) and show in the game's search; joining one is a UDS
  join, and hosting sends beacons so MHXX players can join. Pia packets are converted as the AzaharPlus MHGU hub bridge does (`pia.rs`),
  with each UDS node at 169.254.x.n (x from the host's MAC) and our friend code seed made for our IP. The room routes by MAC, so each
  phone is one console: several MHGU players are several phones in the room. Hosting in Citra Room mode is also our normal host
  (LAN, hub server): one network, `citra_sync_locked` tells the bridge our own stations, MHXX players take the slots those don't hold, and
  `host_route_ex` relays between both sides. If the room drops, its players leave and the hub goes on.
- `Crossplay = All Rooms` (XP_ALL): a hosted hub is on our own host, in the Citra room and in the Yuzu room at once. Each room is a
  door (`citra_bridge.rs`, `yuzu_door.rs`): ldn.c keeps the slots (`L.door_of`), tells each door the hub without its players
  (`door_sync_locked`, from `push_info_locked`) and takes back only its players (`door_merge_locked`). `host_route_ex` relays between
  all of them. Yuzu room players see the others at 192.168.250.k, which only a room that sends unknown IPs to its host passes on:
  the room this app runs (RoomHost) does. Joining is still one network at a time; the search lists all of them.
- The Network tab: local play, an Internet hubs submenu, the Crossplay mode, Room nickname (`RoomName`, both room families; a taken one gets
  -2..-5 in that room), and a submenu per room family the mode uses (`xp_uses`: Ryujinx, Yuzu Room, Citra Room), each row showing
  `ldn_room_state`. Settings save through `config_save` (temp file, fsync, rename); touch layouts the same way (`save_layout`). The nickname is `[Multiplayer] RoomName`. Not yet seen on
  a device: the log's `[citra]` lines tell what it did.
- The touch Layout (`[Touch] Layout = Adaptive / Traditional`, `AutoRun`, `TradShow*`) lives in `config.h` / `config_store.c`; `touch.cpp`
  needs all three, or it does not compile.
- Languages: `[Application] Language = Auto` (the default) follows the phone (`settings_system_language`, SDL's locale, refreshed on
  `SDL_EVENT_LOCALE_CHANGED`): zh is Simplified Chinese, es Spanish, anything else English. Spanish is in `settings_locale.h`; Chinese is
  `runtime/settings_locale_zh.c`, made by `tools/gen_locale_zh.py` from `tools/language_zh.tsv` and `language_zh_extra.tsv` (edit those,
  run it again). Once Chinese is shown the overlay merges a CJK font from the system's (`cjk_font_if_needed`). The Java screens go
  through `Zh.t` (exact strings, then known pieces). A new menu string needs a line in `language_zh_extra.tsv` and the Spanish table.
