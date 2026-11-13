# Fork changes on top of gu-native 0.1.9.2

Base: official gu-native 0.1.9.2. This file lists everything the fork adds. Nothing here has been built or run yet:
the changes were made without the SDL / Vulkan / Android toolchain, so expect to fix compile errors on the first build.

## 1. Crossplay (Ryujinx, Yuzu-family rooms, Citra/Azahar rooms)
- Restored: `crates/` (Rust crossplay library), `Cargo.toml`, `Cargo.lock`, `runtime/net/webapi.c`,
  `runtime/net/citra_room.h`, `GameActivity.webGet` (Java side of the room browser's web requests).
- `runtime/net/ldn.c` is the fork's version (multi-server hubs + crossplay) with 0.1.9's "playing online" notice gate
  (`online_ok`) added. `net.h` has the fork's API plus `ldn_server_name`.
- `emsync.c` and `hle/hpscale.c`: nothing is synced / scaled in crossplay hubs (`ldn_crossplay()`).
- Build: `native/CMakeLists.txt` links `${MHGU_XP_LIB}`; `native/build_native.sh`, `deps/build_deps.sh`,
  `deps/build_ffmpeg.sh` are the fork's (cargo step, `deps/env.sh`). Needs the Rust toolchain + Android targets.
- Settings (ini `[Multiplayer]`): `Crossplay` (Off / Ryujinx / Yuzu Room / Mixed / Citra...), the `Ryu*`, `Room*` and
  `Citra*` keys, web API URLs, last-room entries. Menu: Multiplayer > Crossplay, with Ryujinx, Yuzu Room and Citra Room
  screens and room browsers.

## 2. Multiple hub servers
- `[Multiplayer] Server` takes up to 8 servers (commas or spaces); hubs are listed on all, searches merge all.
- Default list (`LDN_DEFAULT_SERVER`): `hunterhubid-gu-hub.duckdns.org` and `morerokk-mhgurecomp-ldnserver.duckdns.org`.
- Menu: Multiplayer > Hub servers: one row per server, change / remove / add, Paste and Copy; plus the matchmaking key,
  the server check, and **Show all hubs** (`FullHubList`).

## 3. Traditional touch layout
- `[Touch] Layout = Adaptive | Traditional`. Traditional: A B X Y, L R ZL ZR, + -, d-pad and stick always shown,
  plain disc buttons, no menu pad, no ranged layout, own layout file `touch_layout_traditional.txt`.
- `[Touch] TradShow*`: which Traditional buttons show (separate from Adaptive's `Show*`).
- `[Touch] AutoRun`: thumb past the stick's rim runs (otherwise only the R button runs).
- Editor: reset uses the layout's own defaults; the ranged-layout and attack-style buttons are disabled in Traditional.
- Files: `runtime/touch.cpp`, `config*.c/h`, `settings_panel.cpp`.

## 4. Chinese
- Menu translation: `settings_locale.h` hook, `settings_locale_zh.c`, `tools/gen_locale_zh.py`,
  `tools/language_zh*.tsv`, CJK font loading in `overlay.cpp`.
- `[Application]` Language: English, Spanish, Chinese, Auto (follows the phone; `ui.c` handles locale changes).
  First run on a Chinese phone writes `Language = Chinese`.
- Android screens: `Zh.java` + `Zh.t()` on the 0.1.9 Java screens. New 0.1.9.x strings fall back to English.

## 5. Spoofed packages
- `android/app/build.gradle`: flavors `standard`, `antutu`, `ludashi`, `alt`.
- `src/<flavor>/res/values/strings.xml` per flavor; `src/<flavor>/res/xml/shortcuts.xml` with the right
  `targetPackage` (0.1.9 hard-coded `com.mhgu.recomp`).
- Log sharing: `LogProvider` / manifest use `${applicationId}.logs` (0.1.9 hard-coded the authority).

## 6. Android keyboard
- Menu text pages open the phone's own keyboard straight away (0.1.9's "Phone keyboard" mechanism, auto-started on
  Android; Back puts it away). The fork's older `textinput.c` keyboard code is not carried over.

## 7. Ported from morerokk/MHGURecomp (master, ~0.17.0)
| Change | Files |
|---|---|
| Equipment-view crash (decoration slots clamped at 0) | `tools/gen.py` patch 0x2db778 |
| Two/three-button inputs at high FPS (Horn X+A, Lance R+X+A) | `hle/combowin.c` (+ touch timing fns kept), gen.py 0x47ca38 / 0x47cc58 |
| Monster stall timers at high FPS | `hle/emstall.c`, gen.py 0x2c4058 / 0x2c4150 |
| Urgent quests count for everyone (`[Multiplayer] ShareUrgentCredit`, On) | `hle/urgentcredit.c`, config, menu row, `main.c` / `mods.h` init |
| Longer chat (120 chars, wrapped rows in one message) | `hle/chat.c` |
| Village quests at the Hub: empty-list and Chinese-text fixes | `hle/villagehub.c`, `hle/hpscale.c` (+ part-HP scaling), `config_live_village_in_hub` |
| High render resolution: GPU pool grows with render size | `g_gpu_pool_size` in `config.c`, gen.py 0x351e40 / 0x351fa4 |
| More memory for Forge plugins (second region 0xF0100000-0xF8100000) | `rt.h`, `forge.c` |

## Not included
- From the 0.1.8 fork: Direct (hole-punching), the other `cc63198` changes (hatestep, fastload, chat splitting, ...),
  hub-list paging buttons.
- From morerokk: lag compensation (`hitsync.c`), 16:10 UI flicker fix (#333, needs `exec.c` changes), FixKerning /
  BoldText (`textlayout.c`), the keyboard+mouse overhaul, engine modules, updater, debug tools.

## Known caveats
- Untested (no build). `gen.py` patches assume game version 1.4.0.
- `ldn.c` is the riskiest file; then `settings_panel.cpp` and `touch.cpp`.
- 0.1.9's single "Server" row and recent-server list are replaced by the Hub servers screen.
