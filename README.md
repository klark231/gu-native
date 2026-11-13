# MHGU for Android and PC

Play your own copy of Monster Hunter Generations Ultimate natively: on Android, with touch controls made for phones, and on
Windows PCs, with keyboard and mouse or a controller. It is not an emulator: the game's code is translated to run directly on
your device. Both use the same game folder, which you make once from your own copy.

**This project includes no game code, game files, ROMs or keys. You need your own copy of the game.**

Made with Claude Code (Opus 5.5). Many of the decisions were my own, because AI isn't ready to have the final say on something like this.

[Lee esta guía en español](README.es.md)

The newest version is on [Releases](../../releases/latest): the APK for Android and `MHGU-PC-<version>.zip` for PC.
Talk with other players and ask for help in the community's [Discord](https://discord.gg/sk5xEjEMwF).

## Contents

- [Android](#android): [What it does](#what-it-does), [How well does it run?](#how-well-does-it-run),
  [What you need](#what-you-need), [Setup](#setup), [Saves](#saves), [Cheats and mods](#cheats-and-mods),
  [Multiplayer](#multiplayer), [Where things are kept](#where-things-are-kept), [Update](#update-to-a-new-version),
  [Uninstall](#uninstall-it), [Samsung: Game Booster](#samsung-game-booster), [If something goes wrong](#if-something-goes-wrong)
- [PC (Windows)](#pc-windows): [What it does](#what-it-does-1), [How well does it run?](#how-well-does-it-run-1),
  [What you need](#what-you-need-1), [Setup](#setup-1), [Controls](#controls), [Graphics and frame rate](#graphics-and-frame-rate),
  [Saves](#saves-1), [Cheats and mods](#cheats-and-mods-1), [Multiplayer](#multiplayer-1),
  [Where things are kept](#where-things-are-kept-1), [Update](#update-to-a-new-version-1), [Uninstall](#uninstall-it-1),
  [If something goes wrong](#if-something-goes-wrong-1)
- [Legal](#legal), [Credits](#credits), [Official downloads](#official-downloads), [License](#license),
  [For developers](#for-developers)

## Android

### What it does

#### Touch controls for the hunt

- **Movement:** a fixed pad that doesn't follow your finger. Drag past its edge to run; this turns off while your weapon is out, so you don't run off in the middle of a fight. Slide up onto the small circle with a padlock that shows above the pad, which lights up, and lift your thumb there: the run locks and the hunter keeps running forward, and you steer with the camera, until you touch the pad or the circle again, draw your weapon or open a menu. To run only while you hold a button instead: under the gear, **Controls**, **Buttons on the screen**, turn off **Run with the stick**, and a **Run** button shows above the attacks
- **Camera:** drag anywhere on the screen to turn it, as in a phone game. A double tap centers it
- **Monster lock-on:** tap the padlock by **L**, or hold **L**, and the camera keeps turning to the monster until you do it again, as in the newer Monster Hunter games and the MH4U port. It follows the monster the game's Target Cam has chosen, or the nearest one if none is chosen; the target button (**ZL**, or whatever your **Control Type** uses) moves it to the other monster. With the game's **L/R Button Controls** swapped, the target button and **L**'s button swap as the game does: their badges show what each presses. Turning the camera yourself, or pressing **L**, only pauses it for a moment, and it only follows a monster in your zone. You can turn it off under the gear, **Controls**, **Lock-on (hold L)**
- **A layout of their own for bows and bowguns:** with one in hand, in a hunt, the controls change by themselves: a large fire button under your thumb, reload, the scope with a tap or quick aim if you hold it, crouching fire, and arrows for the ammo (hold one to keep changing). Drag on the fire button while you hold it to aim. It follows the game's Bow Controls option and has its own place in the editor
- **Change ammo with a swipe:** besides the arrows, swipe up or down on the item bar. The game's list now shows whole, with each ammo's name
- **Shots with a monster locked on:** a shot from a normal stance goes toward the middle of its body, even if it is higher or lower than you. To raise the shot, a bowgun goes into quick aim for a moment. Your own aim, crouching fire and the styles' moves are left to you, and without the lock you aim as always
- **Items:** the bar sits at the bottom center. Tap it to use the item and swipe it to change the item; a quick swipe no longer uses it by mistake. Hold your finger on it to see the items before and after the selected one, as the game shows them while L is held: swipe meanwhile to turn it, and lift to close it without using anything. Arrows on each side of it turn it too (to hide them: under the gear, **Controls**, **Buttons on the screen**, **Item bar arrows**). As a Prowler, the arrows and the swipe change the Support Move
- **Hunter Arts:** tap each one's panel to use it. It presses what the game's **Control Type** option uses for that art (types 1 to 4, and **L/R Button Controls** and **R Stick Controls** too), and it works with a controller connected as well
- **Attacks:** three layouts to choose from: a diagonally split pill, separate buttons, or a U-shaped band in the corner of the screen. With **Keep guarding while sliding** on (under **Buttons on the screen**), a thumb that slides from guard onto the attacks keeps guarding, for moves like a lance's counter
- **Other buttons:** dodge, guard and sheathe, each with its own icon

#### Menus and comfort

- **Menus made for your thumb:** nothing of the layout changes when a menu of the game's opens: a small pad shows and moves the cursor, with the page buttons above it. The movement stick keeps walking (where the hunter can't walk, as at a town's item box or a shop, it moves the cursor; where the game shows a model, as at the smithy, the stick and dragging turn it, as on a controller), A and B stay where they are, and dragging on the screen moves the cursor too. The pad is moved and resized in the editor like the other buttons. To hide it: under the gear, **Controls**, **Buttons on the screen**, **Menu pad**
- Your phone's keyboard works for typing your character's name, and in the settings menu's text with **Phone keyboard** (Enter accepts; Back hides the keyboard and keeps what you typed); the menu's own keys stay for a controller
- The settings menu works by touch and matches the game's look
- **Faster drinking and eating:** two optional cheats, off by default, under the gear, **Cheats**. They speed up the animation x1.7 or x2.5, but they don't let you walk while drinking
- **Your own cheats and mods:** see [Cheats and mods](#cheats-and-mods)
- Import your save from an emulator, another device or the PC game, and export it to take it there: see [Saves](#saves)
- The game pauses by itself when you leave the app, and while the settings menu is open if you play alone
- Vibration on button taps, and the game's rumble on your phone, with or without a controller

#### Customization

- Press and hold the gear to open the editor (or **Controls**, **Customize controls and HUD**): move and resize each button, and move, resize or hide each part of the game's HUD: the clock, health and stamina, the sharpness and gauges, the party, the map, the target camera, the item bar and the Hunter Arts. Outside a hunt, frames show where each part goes, each with its name. A part you put over a button turns red, and so does the button; the default layout shows no red. The item bar can't be hidden there: it is how you pick and use items with your fingers
- The editor's panel moves out of the way of what you are placing: drag it by its title (the grips at its ends). Tap its title and - / + or a pinch make it smaller, down to 60 %. It stays where you leave it, and as big
- A part you moved stays where you put it: a chat line, a bow's or a bowgun's aim and the Quest Complete stamp no longer send it back to the game's place, and it doesn't jump when a menu of the game's opens. A part that would sit over that menu's window hides until the menu closes
- The game's HUD is rearranged to stay clear of your thumbs (**Move the HUD for touch**). Turned off, the HUD is the same as with a controller. The editor's **HUD: controller** button arranges the HUD you get with a controller, without connecting one, and a controller's B closes the editor without saving
- Adjustable control opacity and camera speed
- **Buttons on the screen:** under the gear, **Controls**, **Buttons on the screen**, choose which ones show. You can add L, ZL, ZR, -, the d-pad and the stick clicks, which are hidden by default (the d-pad gives way to the menu pad while a menu of the game's is open). In town ZL always shows, because it opens the multiplayer menu. In the game's menus you can add ZL and ZR too (**ZL in menus**, **ZR in menus**), which some menus use to change sections or show more information
- **Combined buttons:** on that same page, make up to 6 buttons of your own that press several at once, such as A+X. Place them with the editor, and you can also put them on a controller button

#### Display and performance

- Up to 120 fps on powerful phones, with a configurable frame rate limit
- Fills the whole screen, from ultra-wide phones to 4:3 handhelds
- Adjustable 3D resolution, down to 25%, to gain performance on modest devices
- **Shadows:** turn them off for more speed, under the gear, **Graphics & Audio**, **Shadows**. On a Moto g06 it cut the GPU's work per frame by about 15% in a hunt
- **Low-end mode:** for weak phones. It turns off bloom, depth of field and shadows, uses lighter textures and keeps the HUD sharp over a smaller 3D picture
- **Toaster mode:** the third choice of Low-end mode, for the weakest phones and for the battery: 25% 3D resolution, 30 fps, no reverb or grass, and a lighter HUD and textures still. On a Moto g06 it held 30 fps and used about a third less power than Low-end mode at 60 fps
- **Bloom and depth of field** and **Reverb** (gear, **Graphics & Audio**): switches of their own. Off, the GPU or the processor does less
- **Grass** (gear, **Graphics & Audio**): off, the tall grass and foliage of the maps are left out, faster where there is a lot of it (the Marshland's area 8)
- Optional FPS and temperature counter
- Uses less memory on devices with Mali GPUs and on phones with little RAM
- The screen follows your FPS limit: at 60 fps it runs at 60 Hz even if it can do 120
- **If your phone runs hot, lower the FPS limit.** Nothing helps more: going from 120 to 60 used a quarter to a third less power on a Galaxy S25 Ultra, and going from 60 to 30 a fifth less on a Moto g06
- On Mali GPUs textures are decoded on several cores, to reduce stutter
- **Your own GPU driver:** Adreno only, at your own risk. Under the gear, **Graphics & Audio**, **GPU driver**, add a .zip with `meta.json` and the library. By default the system's is used, and if one fails at start or doesn't load, the app goes back to it by itself and says so
- **Adreno fix:** on an Adreno with a Vulkan 1.1 driver, at a low 3D resolution, the app keeps 2x MSAA so the picture doesn't repeat in a grid. If it looks dark or flickers, or for more speed, set **Adreno fix** to **Off** under the gear, **Graphics & Audio**

#### With a controller

- The touch controls hide as soon as you connect one and come back when you disconnect it, with no restart
- The gear stays on the screen, so you can always open the settings. You can also press **+** and **-** together
- Clicking the right stick locks onto the monster (with the game's **Control Type 3**, whose click changes the target, hold it instead). While the lock is on, a padlock shows by the target icon, and a bowgun's shots aim at the monster's body, as with the touch controls
- **Any controller:** if the game doesn't read yours well, go to the gear, **Controls**, **Map a controller**, and press each button once. It's kept for that model. If a controller can't be read until it is mapped, the app tells you when you connect it

### How well does it run?

Measured with the on-screen counter. In the Low-end mode column the 3D picture is at 40% and the FPS limit was raised, except for the 5-minute session, which used the mode's 30 fps limit:

| Device | Chip | GPU | RAM | Normal | Low-end mode |
|---|---|---|---|---|---|
| Galaxy S25 Ultra | Snapdragon 8 Elite | Adreno 830 | 12 GB | 120 fps | 120 fps in the village |
| Anbernic RG406V | Unisoc T820 | Mali-G57 | 8 GB | 48 fps at full resolution, 60 fps at 50% | 60 fps |
| moto g06 power | MediaTek MT6769 | Mali-G52 MC2 | 4 GB | 16 fps at full resolution, up to 28 fps at 50% | 45 fps; a steady 30 fps over a 5-minute session |

On a phone with 4 GB of RAM, close your other apps first: Android can close the game when memory runs out. For more speed,
turn on Low-end mode: go to the gear button, **Graphics & Audio**, **Low-end mode**, save, and restart the game. It sets the 3D
resolution to 40% and the limit to 30 fps, and you can change both. It costs some sharpness: the game looks softer.

### What you need

- **An Android device:** Android 9 or newer (Android 9 is experimental: it couldn't be tested on one), 64-bit, with Vulkan 1.2. Almost any device from the last few years. Vulkan 1.1 works experimentally. On processors from before 2018 the app adapts the game code the first time it starts, which takes a few seconds.
- **About 13 GB free** on the device.
- **A Windows or Linux PC,** only for setup, with about 10 GB free.
- **Your own copy of the game,** updated to version 1.4.0.

### Setup

It takes three steps, and you only do them once.

#### Step 1: Get your game files

You need two folders from your own game: `exefs` and `romfs`. They must include the 1.4.0 update.

If your copy is in the Ryujinx emulator on your PC:

1. Make sure the 1.4.0 update is applied to the game.
2. Right-click the game, open **Extract Data**, and extract **ExeFS** and then **RomFS** into one empty folder.

Keep both in one folder, for example `C:\MHGU`:

```
MHGU
├─ exefs
└─ romfs
```

This project does not explain how to dump a game from your console, and does not provide game files, keys or links to them.

#### Step 2: Create the game code

The app has no game code in it, so you create yours from your own copy. It runs on your PC and sends nothing anywhere.

1. Install [Python](https://www.python.org/downloads/). On Windows, tick **Add python.exe to PATH** in the installer.
2. On this page, click the green **Code** button, then **Download ZIP**, and unzip it.
3. Open the unzipped folder, then its `tools` folder.
4. Windows: click the address bar of that window, type `cmd` and press Enter. Linux: open a terminal in that folder.
5. Type this, with your own folder, and press Enter:

   ```
   python build_game.py --android C:\MHGU
   ```

The first time, it downloads Google's Android compiler (about 1.8 GB). Then it works on its own: about 6 minutes on a fast PC,
longer on a slower one. When it says **Done**, your folder has two new files:

```
MHGU
├─ exefs
├─ romfs
├─ libmhgugame.so
└─ libmhgugame.id
```

**These files are made from your game: keep them for yourself and don't share them.**

Do you also play on PC? Leave out `--android`: one run makes the game code for both, and also adds `mhgugame.dll` and
`mhgugame.id`. It then downloads llvm-mingw too, the compiler for the PC game (about 150 MB), and takes a few minutes more.
Running it again keeps what is already made and up to date.

#### Step 3: Install it

1. Copy the whole `MHGU` folder to your device, for example with a USB cable. It can go anywhere in its storage or on an SD card,
   except inside `Android/data`.
2. On your device, download the APK from [Releases](../../releases/latest) and open it. Android may ask you to allow installing
   apps from your browser.
3. Open **MHGU**, tap **Give permission and choose folder**, and turn on file access when Android asks. The game only reads that folder.
4. Find your `MHGU` folder (it shows a ✓) and tap **Use this folder**. The app has its own folder browser; if you'd rather use Android's, tap **Android's picker**.

The app checks the folder, tells you exactly what is missing if something is wrong, and starts the game.

Your saves and settings are kept separately, in `Android/data/com.mhgu.recomp/files`. To choose another folder later: gear
button, **Menu Options**, **Game folder**.

### Saves

#### Import your save

You can bring your save from an emulator, another device or the PC game:

1. Find your save's file named `system`. In Ryujinx: right-click the game, **Open User Save Directory**. In yuzu: right-click the game,
   **Open Save Data Location**. From the PC game: export it there (see [Saves on PC](#saves-1)).
2. Copy that file to your device, for example to Downloads.
3. In the game, go to the gear, **Menu Options**, **Import save**, and choose the file.

All the characters on the device are replaced and the game restarts. Your previous save is kept in
`Android/data/com.mhgu.recomp/files/MHGURecomp/save_backups`.

#### Export your save

To take your save to an emulator, another device or the PC game:

1. Save in the game. Then go to the gear, **Menu Options**, **Export save**, and choose a folder.
2. A folder named `MHGU_save_...` is made there with the files `system` and `system_backup`. Move it to your PC.
3. In Ryujinx: right-click the game, **Open User Save Directory**. In yuzu: right-click the game, **Open Save Data Location**.
4. Keep a copy of what is there, and copy both files in.

#### Move a save between Android and PC

The save is the same on both, so you can take it either way:

- **From the phone to the PC:** on the phone, **Export save** and copy the `MHGU_save_...` folder to the PC. In the PC game,
  **F1**, **Menu Options**, **Import save**, and choose the `system` file in that folder.
- **From the PC to the phone:** in the PC game, **F1**, **Menu Options**, **Export save**, and copy the `system` file to the
  phone. On the phone, **Import save** and choose it.

Importing replaces all the characters on the device it goes to, and the save it replaces is kept in a copy, as above.

#### Choose where your save is kept

The save is in the app's own folder unless you choose another one, for example a folder that a sync app (Syncthing,
FolderSync and the like) shares with your other devices. Go to the gear, **Menu Options**, **Save folder**, and choose the
folder:

- Your save goes in a folder named `MHGU save` inside the one you choose, so one shared folder can hold MHGU's and MH4U for
  Android's (`MH4U save`). A folder that already holds a save, such as one **Export save** made, is used as it is.
- Your save is copied there before anything changes. If the folder already has a save, you choose which one stays; the
  other is kept in a copy, in `save_backups` in the app's own folder.
- The game restarts to use the folder: save first. **Use the game's own folder**, on the same screen, goes back.
- On two devices, hunt on one at a time, and let the sync app finish before you open the game on the other.

When the game starts it checks the folder. If it can't reach it, if it is empty but had a save, if the sync app kept two
versions of the save, or if another device has the game open, it says so and lets you choose what to do. If the folder
can't be reached at a save, that save stays on the device, and the next start asks which one to keep.

### Cheats and mods

- **Cheats:** `.txt` files in Atmosphere's format, for version 1.4.0 (its build id is `FB08F1D20FD1204F`). Under the gear, **Cheats**, **Add cheats**, pick the file, and turn each one on in that same page. They start off.
- **Mods:** a mod's folder with `romfs` or `exefs` in it, or a `.zip` of that folder. Under the gear, **Mods**, **Add a mod**. Then order them and turn them on in that page.
- You can also copy them yourself into your game folder, into `cheats` and `mods` folders beside `exefs` and `romfs`. The game reads them at start.
- Cheats and mods that change the game's code work, but that part of the game runs slower. Use cheats online only if everyone in the hub agrees.

### Multiplayer

#### Local play

Two devices on the same Wi-Fi can play together:

1. Both go to town, tap **ZL** and choose **Connect**.
2. One player (the host) chooses **Create Hub** and confirms with **X**.
3. The other chooses **Search Hubs** and picks that hub.
4. If the hub doesn't show up: under the gear, **Multiplayer**, **Search hosts**, enter the host's IP and search again.

Creating a hub, finding it and joining it were tested on a Motorola and an Anbernic, but not a full quest yet.

#### Over the internet

The app comes with a hub server already set, `hunterhubid-gu-hub.duckdns.org`, which players of the community run (not this
project). Everyone on the same server finds the others' hubs, with no ports to open.

1. Play as in [Local play](#local-play): one creates the hub, the others search for it. Hubs from the internet show up next
   to the ones on your Wi-Fi.
2. To play only with your friends: under the gear, **Multiplayer**, everyone enters the same **Matchmaking key**. Only
   players with that key see your hubs.
3. **Hub server** picks another server: **Custom...** to type one (a name or IP, and `:port` if it isn't 39510; the small
   program in the `server` folder runs one, see `server/README.md`), or **Off** for your local network only. **Check hub
   server** says whether it answers and how many hubs it lists.
4. The servers and keys you type and save stay in their lists, the last one first (up to 24), to pick them again; the **×**
   beside one takes it out.

The first time you play online a notice says which server the game connects to: like any server on the internet, it sees
your IP address.

Without a server: put every device on the same VPN (for example Tailscale or ZeroTier) and enter the host's VPN IP under **Search hosts**.

Phones and PCs with 0.1.6 or later meet in the same hubs, through the server or on the same Wi-Fi. Tested in 0.1.9: two
phones in one hub through the default server with a matchmaking key, and a PC joining a phone's hub through the server and
on the same Wi-Fi; a full quest together not yet.

#### Monster HP and Village quests in the Hub

Adjustable monster HP (gear, **Gameplay**) and Village quests in the Hub (gear, **Multiplayer**) come from MHGURecomp, the project this
one is based on. They haven't been fully tested on Android yet.

### Where things are kept

| What | Where |
|---|---|
| Settings, the touch layout and the HUD's, controller mappings, the log | `Android/data/com.mhgu.recomp/files`: `mhgurecomp.ini`, `touch_layout.txt`, `gamepad_mappings.txt`, `log.txt` (the session before: `log_prev.txt`) |
| Saves | `Android/data/com.mhgu.recomp/files/MHGURecomp/save`, or the folder you chose in **Save folder**. The save an import replaces goes to `save_backups`, in `MHGURecomp` |
| Cheats and mods you add | Your game folder's `cheats` and `mods` |

### Update to a new version

1. Download the new APK from [Releases](../../releases/latest) and open it: it installs over the app you have. Your saves,
   settings, touch layout and game folder stay.
2. If the new version needs new game code, the app says "This libmhgugame.so is from another version" when it starts. Do
   step 2 again with the new version's tool (this page's ZIP again, or **Source code (zip)** on its release) and copy the
   two new files to your folder on the device. From 0.1.6 to 0.1.7 the game code stays the same.

### Uninstall it

- Export your save first if you want to keep it: see [Export your save](#export-your-save).
- Uninstall the app as any other. Android may offer to keep its data (your saves and settings); if you don't keep it, it is
  deleted with the app.
- Your game folder is yours: keep it for the PC game, or delete it. The cheats and mods you added are in it.
- On the PC you used for setup, the tool keeps the compilers it downloaded in `.mhgu-tool`, in your user folder. Delete it if
  you won't make the game code again. If the lz4 Python package was missing, the tool installed it (`pip uninstall lz4`
  removes it); and Python stays, if you installed it for this.

### Samsung: Game Booster

Samsung's performance panel only appears for apps it recognizes as games, and an APK installed by hand doesn't count. To get
it, install the APK once from a PC with [adb](https://developer.android.com/tools/adb):

```
adb install -r -i com.mhgu.recomp MHGU-0.1.7.apk
```

Use the name of the APK you downloaded. This is optional: the game runs the same without it. To at least see it in Gaming Hub: **My games**, **Library**, the three
dots, **Add games**.

### If something goes wrong

| Problem | What to do |
|---|---|
| `python` is not recognized | Install Python again, tick **Add python.exe to PATH**, and open a new terminal |
| "Failed to build installable wheels" for lz4 | The tool from 0.1.9 on reads the game code without the lz4 package when your Python can't install it: download it again |
| "there is no exefs/main" | The path must be the folder that holds `exefs` and `romfs` |
| "This exefs is not version 1.4.0" | Use the `exefs` with the 1.4.0 update |
| "romfs is incomplete" | Use the base game's `romfs` with the update applied. It has 35,531 files |
| "libmhgugame.so is missing" | Do step 2, then copy the two new files to the folder on your device |
| "This libmhgugame.so is from another version" | A new app version needs it again: repeat step 2 with the new download |
| The game files screen says "EACCES" or "open failed" | Another app (a file manager, for example through Shizuku) made the app's folder in `Android/data`. From 0.1.9 the app takes it back, or keeps its files in its private folder and says so |
| The game closes by itself | Close other apps and lower the 3D resolution. It's usually a lack of memory |
| Black picture with only the touch controls, or it closes when you press A | The copy of `romfs` to your device didn't finish. Copy it again and check that it takes up 12.3 GB |
| The game says **Game files incomplete** | Some files of `romfs` were cut short while copying, and the game can close or draw black where it reads them. Copy `romfs` again and check that it takes up 12.3 GB |
| It closes after a few minutes of play | Update to 0.1.6 or newer: it uses video memory differently on PowerVR GPUs. If it still happens, share the log: it notes the memory every minute |
| On a weak phone it runs slowly | Turn on Low-end mode, turn off **Shadows**, and try a 3D resolution of 33% or 25%. Still slow: Low-end mode, **Toaster** |

If it still fails, ask in the [Discord](https://discord.gg/sk5xEjEMwF) or open an [issue](../../issues) with your device model and the log. If the game closes because of an error, a screen lets you
share it; if not, it's under the gear, **Menu Options**, **Share log**. If the screen stays black and you can't reach the gear, a long
press on the app's icon offers **Share log** too. Don't attach game files.

## PC (Windows)

The same game folder plays on a Windows PC: `MHGU.exe` runs your game natively.

The download has the program only: the game code (`mhgugame.dll` and `mhgugame.id`) is made once from your own copy, in
[Setup step 2](#step-2-create-the-game-code-1).

### What it does

#### Keyboard and mouse

- **Plays with keyboard and mouse alone:** WASD moves, the mouse turns the camera, left click attacks (X) and F is the action
  button (A): it talks, opens, gathers and picks up. The full list is in [Controls](#controls)
- **The right click is a second attack (A), only with the weapon out:** it never talks to people, opens or gathers by mistake
- **Every key can be changed,** up to two per button, mouse buttons included: **F1**, **Controls**, **Keybinds**. The camera
  and your combined buttons can go on keys too
- **Monster lock-on:** a tap on Alt (R3) keeps the camera on the monster, and a padlock shows by the target icon. A shot
  from a normal stance then aims at the monster's body; your own aim, crouching fire and the styles' moves are left to you.
  It can be turned off: **F1**, **Controls**, **Lock-on**
- **Type with your keyboard:** your character's name, and a chat line in a hub with **/**, in a quest too
- **A touch screen:** on a PC with one, the phone's touch controls show when you touch it (**Controls**, **Touch controls**),
  and **Customize controls and HUD** places them

#### Menus and comfort

- **The mouse in the game's menus, as in the newer games:** in the item box, the pouch, the pause menu, the equipment box and
  other lists, and while someone talks to you or the game asks something, point and click. Left click is A, right click is
  B, the wheel scrolls lists and turns pages, and the side buttons turn pages. See [Controls](#controls)
- **WASD moves the cursor in the game's menus,** in town too: the item box, a shop, the quest counter and its board
- **The settings menu (F1)** works with the mouse, the keyboard or a controller, in English or Spanish. Esc, B or a click
  outside closes each window
- **Faster drinking and eating:** the same two optional cheats as on the phone, off, under **F1**, **Cheats**
- **Your own cheats and mods:** see [Cheats and mods](#cheats-and-mods-1)
- Import your save from an emulator or the Android app, and export it to take it there: see [Saves](#saves-1)
- The game stops while its window is minimized, and goes on where it was when you bring it back (**F1**, **Menu Options**,
  **Pause in the background**)

#### Customization

- **Move the HUD:** **F1**, **Display**, **Move the HUD**, and drag any part with the mouse: the clock, health and stamina, the
  sharpness and gauges, the party, the map, the target camera, the item bar and the Hunter Arts. The wheel changes a part's
  size, and **Hide** takes it off the screen. The same HUD is used with a keyboard and with a controller. How: see
  [Controls](#controls)
- A part you moved stays where you put it: a chat line, an aim and the Quest Complete stamp no longer send it back, and it
  doesn't jump when a menu of the game's opens. A part that would sit over that menu's window hides until the menu closes
- **On a wide screen,** **HUD anchor** (**F1**, **Display**) lays the HUD out over the middle 16:9, 21:9, 32:9 or the whole
  width
- **Combined buttons:** up to 6 buttons of your own that press several at once, such as A+X, on a key (**Keybinds**) or a
  controller button (**Controls**, **Combined buttons**)

#### Display and performance

- Your screen's resolution, in a borderless full screen by default, or in a window of any size. **F11** or **Alt+Enter**
  switches between them. The picture takes the window's shape at once, from 16:10 to ultra-wide
- Up to 240 fps, or no limit. At any frame rate the game keeps its normal speed
- **3D resolution** down to 25%, with the HUD and the text always sharp
- **Low-end mode** and **Toaster** for integrated graphics and weak PCs
- **FSR 3.1, XeSS-SR or FSR 4** to rebuild a smaller 3D picture; MSAA up to 8x, FXAA and **Smooth gradients**
- **Shadows**, **Bloom and depth of field** and **Reverb**: switches of their own
- **Draw distance** for the people in towns and hubs, and **Field of view**
- **GPU:** on a PC with two graphics cards, choose the one the game runs on (by default a dedicated one if there is one)
- **Show FPS and PC stats:** the frame rate, the GPU's temperature, and with **Full** the GPU's and the CPU's use
- The first start prepares all the game's shaders while the logos play, so fewer things show black the first time
- With the game on a hard disk, the game code is read ahead at start, for fewer stutters

More in [Graphics and frame rate](#graphics-and-frame-rate).

#### With a controller

- Connect it and play. **F1**, or **+** and **-** together, opens the settings
- Clicking the right stick locks onto the monster
- **Any controller:** if the game doesn't read yours well, go to **F1**, **Controls**, **Map a controller**, and press each
  button once. It's kept for that model
- The settings menu can show the buttons in the style of your controller (**Controls**, **Button icons**)

#### Discord

If the Discord app is open on the same PC, your profile there shows what you're doing: where you are, the monsters you hunt,
the map and area, how many hunters are with you, your weapon, and your hunter's name and HR. It is on by default. To turn it
off: **F1**, **Multiplayer**, **Discord Rich Presence**. To leave out your name and HR: **Show hunter name and HR**. The game only
talks to the Discord app on your PC.

### How well does it run?

Measured on the developer's PC:

| PC | Where | Settings | Frame rate |
|---|---|---|---|
| Ryzen 5 3600XT, Radeon RX 6800 XT | A hunt | 1600x900, no limit | 282 to 336 fps |
| Same | The village | 1920x1080 window, no limit, VSync off | about 205 fps |
| Same | A quest | 3D picture at 3440x1440, 240 fps limit | 240 fps, the limit |

It hasn't been measured on integrated graphics or a weak PC yet. If you try one, an [issue](../../issues) with your
processor, graphics card and frame rate helps.

### What you need

- **Windows 10 or 11,** 64-bit.
- **A graphics card with Vulkan 1.2,** with its driver up to date: Intel integrated graphics from 6th-generation Core (2015)
  on, NVIDIA GeForce GTX 600 or newer, or AMD Radeon HD 7700, R7, R9 or newer.
- **A 64-bit processor with SSE4.2:** Intel Core i3, i5, i7 (2009 on) or later, AMD FX, A-series from 2012, or Ryzen. Older
  ones lack it, such as an Intel Core 2 or an Atom from before 2013, an AMD Phenom II or Athlon II, or AMD's A- and
  E-series from 2011, and the game says so when it starts.
- **8 GB of RAM** recommended. 4 GB should work with nothing else open, with slower loading, but it hasn't been tested.
- **Free space:** about 13 GB for your game folder, at least 3 GB more while the tool works, and about half a GB for the
  program and its shaders.
- **Your own copy of the game,** updated to version 1.4.0.

### Setup

It takes three steps, and you only do them once.

#### Step 1: Get your game files

You need two folders from your own game: `exefs` and `romfs`. They must include the 1.4.0 update.

If your copy is in the Ryujinx emulator on your PC:

1. Make sure the 1.4.0 update is applied to the game.
2. Right-click the game, open **Extract Data**, and extract **ExeFS** and then **RomFS** into one empty folder.

Keep both in one folder, for example `C:\MHGU`:

```
MHGU
├─ exefs
└─ romfs
```

This project does not explain how to dump a game from your console, and does not provide game files, keys or links to them.

#### Step 2: Create the game code

The program has no game code in it, so you create yours from your own copy. It runs on your PC and sends nothing anywhere.

1. Install [Python](https://www.python.org/downloads/). Tick **Add python.exe to PATH** in the installer.
2. On this page, click the green **Code** button, then **Download ZIP**, and unzip it.
3. Open the unzipped folder, then its `tools` folder.
4. Click the address bar of that window, type `cmd` and press Enter.
5. Type this, with your own folder, and press Enter:

   ```
   python build_game.py --pc C:\MHGU
   ```

The first time, it downloads llvm-mingw, the compiler for the PC game (about 150 MB). Then it works on its own: about 10
minutes on a fast PC, and up to an hour on one with 2 cores. When it says **Done**, your folder has two new files:

```
MHGU
├─ exefs
├─ romfs
├─ mhgugame.dll
└─ mhgugame.id
```

**These files are made from your game: keep them for yourself and don't share them.**

Do you also play on Android? Leave out `--pc`: one run makes the game code for both, and also adds `libmhgugame.so` and
`libmhgugame.id`. It then downloads Google's Android compiler too (about 1.8 GB). If you made the phone's before, running it
again keeps those and only adds the PC's.

#### Step 3: Install it

There is no installer: the program is a folder.

1. Download `MHGU-PC-<version>.zip` from [Releases](../../releases/latest) and unzip all of it into a folder of your own,
   such as `C:\Games` (opened from inside the zip, `MHGU.exe` doesn't start). Not into Program Files: the game keeps its
   settings, caches and log in its own folder, and Windows doesn't let it write there. Keep the name `MHGU.exe`: the game
   code looks for the program by that name.
2. Open `MHGU.exe`. Windows may say it doesn't know the program, because it isn't signed: **More info**, **Run anyway**. With
   Smart App Control on, Windows blocks it instead: see [If something goes wrong](#if-something-goes-wrong-1).
3. The first time, a screen asks for your game folder: **Choose folder**, and pick your `MHGU` folder. The game checks it, tells
   you exactly what is missing if something is wrong, and starts. It remembers the folder.

If `MHGU.exe` is inside your `MHGU` folder, beside `exefs` and `romfs`, the game finds it without asking. The first start
prepares the game's shaders in the background while the logos play; they take about 340 MB in the `cache` folder. A notice
then says that **F1** opens the settings. To choose another folder later: **F1**, **Menu Options**, **Game folder**; the game
uses it from its next start.

The zip holds one folder with the program's files and nothing of the game:

- `MHGU.exe`, and the libraries it uses: `SDL3.dll`, `mhguffmpeg.dll` (the game's videos) and `libxess.dll` (XeSS-SR)
- `gamelib_id.txt`: the id of the game code this version needs. Your folder's `mhgugame.id` must say the same
- `pipelines.list`: graphics states the first start prepares
- `extras`: the optional cheats for faster drinking and eating, which the first start copies, off, into the `cheats` folder
  beside `MHGU.exe`
- `licenses`: the legal notice and the licenses, also in **F1**, **Credits & Licenses**, **Licenses**

### Controls

- **Keyboard and mouse:** WASD moves (in the game's menus, such as the item box, a shop or the quest counter, it moves the
  cursor), left Shift runs (R), left Ctrl walks. Left click or R is X, F is A (talk, open, gather, pick up, confirm), right
  click is A too but only with the weapon out (an attack: it never talks, opens or gathers), Space B, E or the middle button
  Y (sheathe, use an item), Q is L, Tab and T are ZL and ZR, Enter is +, Backspace −, the arrows are the d-pad, M the
  minimap (L3), a tap on Alt the lock-on (R3) and I, J, K, L turn the camera. The mouse wheel turns the item bar. Click the
  window to turn the camera with the mouse; Esc lets the mouse go. Every key can be changed: **F1**, **Controls**,
  **Keybinds**.
- **The mouse in the game's menus:** in the item box, the pouch, the pause menu, the equipment box and other lists, and
  while someone talks to you or the game asks something, the pointer shows and works as in the newer games: point at a row
  or a box to select it (its details show), left click is A (it confirms what you point at, or what is selected, and goes
  on with a talk), right click is B (back; in menus and talks it is not A, F still is), the wheel scrolls lists and turns
  the box's pages (in a quantity it adds or takes one, in a yes / no question it picks the answer, which in most of them
  you can also point at and click; in the others confirm with F), and the side buttons turn pages. Out of the menus and
  talks the mouse turns the camera as before. In those menus and talks a left click is A, not X: where a menu asks for X,
  press R. To turn it off: **F1**, **Controls**, **Mouse in the game's menus**.
- **Controller:** connect it and play; + and − together open the settings. If the game doesn't read yours well: **Controls**,
  **Map a controller**.
- **F1** opens the settings, **F11** or **Alt+Enter** switches between full screen and a window, **F12** saves a screenshot in
  `shots`, **F7** tries the MSAA levels without saving them, and **/** opens the chat in a hub (on a Spanish keyboard, the **-**
  key; you can change it in **Keybinds**).
- **The HUD:** open **F1**, **Display**, **Move the HUD** and drag any part of it with the mouse: the clock, health and
  stamina, the sharpness and gauges, the party, the map, the target camera, the item bar and the Hunter Arts. The wheel over
  one (or **-** / **+**) changes its size, **Hide** takes it off the screen, **Reset** puts the picked one back and
  **Defaults** all of them. Outside a hunt, frames show where each part goes. **Save** keeps it; Esc cancels.

### Graphics and frame rate

On a laptop with integrated graphics, or on a 4K screen, lower **3D resolution** under **F1**, **Display** (the HUD and the
text stay sharp), or turn on **Low-end mode** under **Graphics & Audio**: the 3D picture goes to about 720 lines (66% at 1080p,
50% at 1440p, 33% at 4K), and bloom, depth of field, MSAA and FSR turn off. Its third choice, **Toaster**, goes to about 540
lines, 30 fps and no shadows or FXAA. Both take effect when the game restarts.

**Frame rate limit**, under **Graphics & Audio**, goes from 30 to 240, or **Unlimited** (60 by default). Unlimited needs
**VSync** off: with VSync on, the monitor's refresh rate is the most. On a PC that can't hold 60, 30 is steadier. There too,
**Show FPS and PC stats**: **Basic** shows the frame rate, the frame time, the longest frame and the GPU's temperature, and
**Full** adds a line on the GPU (use, clock, video memory) and one on the CPU. Windows doesn't let a program read the CPU's
temperature.

With **FSR** on, the **Upscaler** row there also offers XeSS-SR. It needs Intel Iris Xe or Arc graphics, or a card with DP4a
such as an AMD RX 6000 or NVIDIA RTX 3000 or newer (where it costs more than FSR 3.1), and the Microsoft Visual C++
Redistributable (x64). Without them the game uses FSR 3.1, and the row says why. **FSR 4**, in that list too, is for AMD
Radeon RX 9000 cards: it is untested and needs AMD's own `amd_fidelityfx_vk.dll` beside `MHGU.exe`, which isn't included.
Get it only from AMD, never from another site. Without it, FSR 3.1 runs. FSR has a cost of its own: for speed on a weak GPU,
lower the 3D resolution or use Low-end mode instead.

Under **Display**: **Window mode** (Borderless, Fullscreen or Windowed), **Screen resolution**, **Field of view**, **HUD anchor**
for wide screens, and **Fill the screen**, which lets the game's videos and the menus' backdrops fill a screen wider than 16:9
instead of showing bars at the sides. Under **Graphics & Audio**: **GPU**, **MSAA**, **FXAA**, **Draw distance**, **Shadows**,
**Bloom and depth of field**, **Smooth gradients**, the volume, **Reverb** and **Mute in background**.

### Saves

From **F1**, **Menu Options**:

- **Import save:** choose a save's `system` file. From Ryujinx: right-click the game, **Open User Save Directory**; from yuzu:
  right-click the game, **Open Save Data Location**; from the phone: export it there (see
  [Move a save between Android and PC](#move-a-save-between-android-and-pc)). The characters here are replaced, the game
  restarts with it, and the save it replaces is kept in `%APPDATA%\MHGURecomp\save_backups`.
- **Export save:** save in the game first, then choose a folder, and a `MHGU_save_...` folder with `system` and
  `system_backup` is made in it. To take it to an emulator, keep a copy of what is in its save folder and copy both files in.
  To take it to the phone, copy `system` to it and use **Import save** there.

### Cheats and mods

- **Cheats:** `.txt` files in Atmosphere's format, for version 1.4.0 (its build id is `FB08F1D20FD1204F`). **F1**, **Cheats**,
  **Add cheats**, and pick the files. They start off: turn each one on in that page and press **Save**. Turning them on or
  off needs no restart.
- **Mods:** a mod's folder with `romfs` or `exefs` in it, or a `.zip` of that folder. **F1**, **Mods**, **Add a mod**. Then
  order them and turn them on in that page: when two change the same file, the one lower in the list wins.
- They go in your game folder's `cheats` and `mods`, where you can also put them yourself, and the game restarts to read
  them. Each cheat file and each mod has its own **Remove**: a cheat file's cheats stop at once, and a mod the game is using
  goes when it restarts.
- The optional cheats for faster drinking and eating are there too, off.
- Cheats and mods that change the game's code work, but that part of the game runs slower. Use cheats online only if
  everyone in the hub agrees.

### Multiplayer

Local play and hubs over the internet work as on the phone, with the same code and the same settings, under **F1**,
**Multiplayer**: see [Local play](#local-play) and [Over the internet](#over-the-internet). In town, press ZL (Tab) and choose
**Connect**.

- **Create Hub** is confirmed with X: R on a keyboard.
- The first time you create a hub, Windows Firewall asks whether MHGU may use the network. Allow it, or nobody can find or
  join your hub. Only the host needs this. If you said no: Windows Security, **Firewall & network protection**, **Allow an
  app through firewall**.
- The search asks every network the PC is on. Over a VPN, such as Tailscale or ZeroTier, you can also enter the host's IP
  under **Search hosts** and restart the game.
- A PC and a phone play in the same hub (0.1.6 or later on both).
- Adjustable monster HP (**Gameplay**, with HP for single quests under **Per-quest overrides**) and Village quests in the Hub
  (**Multiplayer**) come from MHGURecomp.

Tested on PC: joining a phone's hub through the hub server and on the same Wi-Fi. Not yet: creating a hub on the PC (it
needs the firewall's permission) and a full quest.

### Where things are kept

| What | Where |
|---|---|
| Settings, the folder you chose, controller mappings, the HUD's layout | Beside `MHGU.exe`: `mhgurecomp.ini` (and the settings menu's own `forge_imgui.ini`), `game_folder.txt`, `gamepad_mappings.txt`, `touch_layout.txt` |
| Shader caches, screenshots, the log | Beside `MHGU.exe`: `cache`, `shots`, `log.txt` (the session before: `log_prev.txt`) |
| The included cheats, and which cheats are off | Beside `MHGU.exe`: `cheats` |
| Saves | `%APPDATA%\MHGURecomp\save`. A save you import waits in `save_import`, beside `MHGU.exe`, until the game restarts, and the save it replaces goes to `save_backups` in `%APPDATA%\MHGURecomp` |
| Cheats and mods you add | Your game folder's `cheats` and `mods` |

To open `%APPDATA%`, type it in File Explorer's address bar and press Enter.

### Update to a new version

1. Unzip the new version and copy its files over the old ones. Your settings, the folder you chose and the HUD's layout stay,
   and your saves stay in any case.
2. If the new version needs new game code, the game says "This mhgugame.dll is from another version" when it starts. Do step 2
   again with the new version's tool (this page's ZIP again, or **Source code (zip)** on its release) and open the game again.

### Uninstall it

- Delete the program's folder, the one with `MHGU.exe`. Its settings, caches, screenshots, log and included cheats go with
  it.
- Your saves stay in `%APPDATA%\MHGURecomp`. Delete that folder too only if you want them gone, and keep a copy first.
- Your game folder is yours: keep it for the phone, or delete it. The cheats and mods you added are in it.
- The tool keeps the compilers it downloaded in `.mhgu-tool`, in your user folder. Delete it if you won't make the game code
  again. If the lz4 Python package was missing, the tool installed it (`pip uninstall lz4` removes it); and Python stays, if
  you installed it for this.
- Nothing else is installed: there was no installer, and the game keeps no settings in the registry. If you allowed MHGU
  through Windows Firewall, that rule stays until you remove it in Windows Security.

### If something goes wrong

| Problem | What to do |
|---|---|
| `python` is not recognized | Install Python again, tick **Add python.exe to PATH**, and open a new terminal |
| "there is no exefs/main" | The path must be the folder that holds `exefs` and `romfs` |
| "This exefs is not version 1.4.0" | Use the `exefs` with the 1.4.0 update |
| "romfs is incomplete" | Use the base game's `romfs` with the update applied. It has 35,531 files |
| "SDL3.dll was not found" | Unzip the whole zip into a folder first: `MHGU.exe` doesn't start from inside the zip |
| Smart App Control blocked it | It blocks programs that aren't signed, with no **Run anyway**, and `MHGU.exe` isn't signed: it only runs with Smart App Control off (Windows Security, **App & browser control**, **Smart App Control settings**). Think it over first: on many versions of Windows it can't be turned back on without reinstalling Windows |
| "mhgugame.dll is missing" | The download doesn't include it: do [Setup step 2](#step-2-create-the-game-code-1) (`python build_game.py --pc <your folder>`) and choose that folder |
| "This mhgugame.dll is from another version" | A new PC version needs it again: repeat step 2 with the new download |
| "This processor lacks instructions the game code needs" | The PC game needs a processor with SSE4.2: see [What you need](#what-you-need-1) |
| "This program must be called MHGU.exe" | Rename it back to `MHGU.exe` |
| "This PC's graphics has no Vulkan driver" | Install the latest driver from your graphics card's maker. The oldest graphics have none: see [What you need](#what-you-need-1) |
| "Windows ran out of memory" | Close other programs, or let Windows manage the page file |
| It asks for the game folder at every start | Windows doesn't let it write in its folder: move it out of Program Files |
| It closes at the start, or the window stays black | Update your graphics card's driver |
| The mouse doesn't turn the camera | Click the window first. Esc lets the mouse go |
| The **Upscaler** row says FSR 3.1 is in use | The upscaler you chose can't run on this PC, and the row's help says why. For XeSS-SR it is often the missing Microsoft Visual C++ Redistributable (x64) |
| It runs slowly | Lower **3D resolution** or turn on **Low-end mode**; still slow, **Toaster**. On a laptop with two graphics cards, check that **GPU** shows the dedicated one |

The log is in **F1**, **Menu Options**, **Log folder** (`log.txt`; if the game closed and you opened it again, that session's
is `log_prev.txt`): attach it to an [issue](../../issues) with your PC's processor and graphics card. Don't attach game files. You can also ask in the [Discord](https://discord.gg/sk5xEjEMwF).

## Legal

This project is not affiliated with, endorsed by or sponsored by the Great C or the Great N. Monster Hunter is a trademark of the Great C.
Switch is a trademark of the Great N.

This repository and its releases contain no game code, game data, ROMs or keys. Use it only with a copy of the game you own.
Requests for game files, keys or ready-made `libmhgugame.so` or `mhgugame.dll` files will be closed.

Rights holders: please open an issue to get in touch.

## Credits

Based on the open-source MHGURecomp project (MIT license) and the libraries listed in
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES). Their licenses, with the legal notice, are also in the app: gear button, **Credits & Licenses**, **Licenses**. On PC: **F1**, **Credits & Licenses**, **Licenses**, and the `licenses` folder beside `MHGU.exe`.

From the same author: [MH4U for Android](https://github.com/khorne-hub/4u-native), Monster Hunter 4 Ultimate recompiled for
Android the same way.

## Official downloads

MHGU for Android is free, and so is its PC version. The only official downloads are the links khorne-hub gives, starting with
this repository's [Releases](https://github.com/khorne-hub/gu-native/releases). If you got it anywhere else, paid for it, or
your copy came with ads, it is not the official one. In the app, the GitHub button at the top of the settings menu, beside
**Save**, opens this page in your browser, and the Discord button beside it the community's [Discord](https://discord.gg/sk5xEjEMwF).

Every official APK is signed with the same key. Its certificate's SHA-256 fingerprint is:

```
D7:2F:6F:F1:55:E0:31:2C:9C:74:86:C5:30:64:60:38:E4:24:94:C0:AD:7C:D6:5F:4B:19:FF:5D:0F:32:B2:97
```

To check an APK: `apksigner verify --print-certs MHGU-<version>.apk` (Android SDK build-tools). A copy signed with another
key can't be installed over the official app either. From 0.1.9 the app checks it too: a copy signed with another key says
so once when it starts.

The app also tells you when a new version is out. At most once a day it asks GitHub for this repository's latest release
(nothing else is sent, not even the phone's model), and at the next start it offers to open the release page. It never
downloads or installs anything by itself. Gear, **Menu Options**, **Check for updates** turns it off.

The PC version has no such signature: each release lists the SHA-256 of its zip. To check yours on Windows, open a terminal
in its folder and run `certutil -hashfile <file> SHA256`; the result must match the release's.

## License

From version 0.1.7 on, MHGU for Android and MHGU for PC are under the project's own [license](LICENSE): you can download the APK
and the PC zip from this page, install them on your devices, read the code and build it for yourself, but redistributing the app
or its code anywhere else, under any name, needs my written permission first (open an issue to ask). Versions 0.1.6 and earlier
stay under the MIT license they were published with, and the third-party parts keep their own licenses. FFmpeg, which plays the
game's videos, is under the GNU LGPL 2.1: each release from 0.1.7 on carries its source, and you may modify MHGU for your own use
and reverse engineer it to debug those changes.

## For developers

See [CLAUDE.md](CLAUDE.md) for how the project is organized, built and tested, and for step-by-step setup notes.
