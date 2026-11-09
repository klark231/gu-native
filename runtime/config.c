// Player settings (config.h): mhgurecomp.ini in the game folder (next to the executable, or MHGU_HOME; with a game
// folder argument, the current folder). A missing file is written with the defaults, so
// deleting it resets everything; keys that are missing or not understood keep their defaults (the log says which).
//   MHGU_CONFIG=<path>   another settings file
//   MHGU_WINDOW=WxH      windowed at that size, MHGU_RENDER=WxH  3D resolution, MHGU_MSAA=1|2|4|8, MHGU_FXAA=0|1,
//                        MHGU_RT16=0|1 (SmoothGradients), MHGU_PAD_POSITIONAL=0|1 (test runs; override the file)
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL3/SDL.h>

#include "config.h"
#include "settings_locale.h"
#include "rt.h"

#include "keymap.h"

// (by BIND_*: one left out has no key, as the combined buttons'. A PC's, for playing on the keyboard alone too: Left
// Shift is R, running while moving, E is Y, sheathing and using items, F is A, talking, opening, gathering and confirming
// in the game's menus (where the move keys are the d-pad: host_input.c), first so that A's name is F wherever one is
// shown; a right click is A too, but outside the game's menus and dialogs only with the weapon out, an attack
// (host_input.c mouse_a_ok). R is X too (in the game's menus a left click is A, runtime/hle/mousemenu.c, and some ask
// for X: Create Hub), M is the left stick's click, the minimap, and Left Ctrl walks; a phone's keyboard as before.
// [Keybinds] in DEFAULT_INI says the same)
#ifdef __ANDROID__
#define DEFAULT_WALK_KEY {SDL_SCANCODE_LSHIFT}
#define DEFAULT_A_KEYS {KEY_MOUSE + SDL_BUTTON_RIGHT}
#define DEFAULT_X_KEYS {KEY_MOUSE + SDL_BUTTON_LEFT}
#define DEFAULT_Y_KEYS {SDL_SCANCODE_R, KEY_MOUSE + SDL_BUTTON_MIDDLE}
#define DEFAULT_R_KEYS {SDL_SCANCODE_E}
#define DEFAULT_L3_KEYS {SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL}
#else
#define DEFAULT_WALK_KEY {SDL_SCANCODE_LCTRL}
#define DEFAULT_A_KEYS {SDL_SCANCODE_F, KEY_MOUSE + SDL_BUTTON_RIGHT}
#define DEFAULT_X_KEYS {KEY_MOUSE + SDL_BUTTON_LEFT, SDL_SCANCODE_R}
#define DEFAULT_Y_KEYS {SDL_SCANCODE_E, KEY_MOUSE + SDL_BUTTON_MIDDLE}
#define DEFAULT_R_KEYS {SDL_SCANCODE_LSHIFT}
#define DEFAULT_L3_KEYS {SDL_SCANCODE_M, SDL_SCANCODE_RCTRL}
#endif
#define DEFAULT_BINDS {[BIND_MOVE_UP] = {SDL_SCANCODE_W}, [BIND_MOVE_DOWN] = {SDL_SCANCODE_S},                         \
    [BIND_MOVE_LEFT] = {SDL_SCANCODE_A}, [BIND_MOVE_RIGHT] = {SDL_SCANCODE_D}, [BIND_WALK] = DEFAULT_WALK_KEY,         \
    [BIND_CAM_UP] = {SDL_SCANCODE_I}, [BIND_CAM_DOWN] = {SDL_SCANCODE_K}, [BIND_CAM_LEFT] = {SDL_SCANCODE_J},          \
    [BIND_CAM_RIGHT] = {SDL_SCANCODE_L}, [BIND_A] = DEFAULT_A_KEYS, [BIND_B] = {SDL_SCANCODE_SPACE},                   \
    [BIND_X] = DEFAULT_X_KEYS, [BIND_Y] = DEFAULT_Y_KEYS,                                                              \
    [BIND_L] = {SDL_SCANCODE_Q}, [BIND_R] = DEFAULT_R_KEYS, [BIND_ZL] = {SDL_SCANCODE_TAB},                           \
    [BIND_ZR] = {SDL_SCANCODE_T}, [BIND_PLUS] = {SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER},                        \
    [BIND_MINUS] = {SDL_SCANCODE_BACKSPACE}, [BIND_DPAD_UP] = {SDL_SCANCODE_UP}, [BIND_DPAD_DOWN] = {SDL_SCANCODE_DOWN}, \
    [BIND_DPAD_LEFT] = {SDL_SCANCODE_LEFT}, [BIND_DPAD_RIGHT] = {SDL_SCANCODE_RIGHT},                                 \
    [BIND_LSTICK] = DEFAULT_L3_KEYS, [BIND_RSTICK] = {SDL_SCANCODE_LALT, SDL_SCANCODE_RALT},                          \
    [BIND_ITEM_LEFT] = {KEY_WHEEL_UP}, [BIND_ITEM_RIGHT] = {KEY_WHEEL_DOWN}}
_Static_assert(BIND_COMBO6 - BIND_COMBO1 + 1 == TOUCH_COMBOS, "a keybind for each combined button");
// (the screen's own size, full screen, on a phone and on a PC: the game at the display's resolution, not a smaller
// picture made bigger)
#define CONFIG_DEFAULT_SIZE 0, 0
#ifdef __ANDROID__
#define CONFIG_DEFAULT_FXAA 0  // (as the new mhgurecomp.ini says: the first run, before that file is read, too)
#else
#define CONFIG_DEFAULT_FXAA 1
#endif
#define CONFIG_DEFAULT_VALUES DISPLAY_BORDERLESS, CONFIG_DEFAULT_SIZE, 0, 0, 60, 1, 1, CONFIG_DEFAULT_FXAA, FACE_POSITION, 39500, "", 0, 0, 1, 1, {{1, 1, 1, 1}, 2}, 1.0f, 0.0f, 1.0f, UI_CENTER, 1, 100, 0, MENU_LANG_AUTO, 100, MENU_CONFIRM_ORIGINAL, 1, 0, FSR_NATIVE, 0.8f, 1, 0.9f, 1, FSR_UP_3, UPDATES_ASK, 1, 1.0f, 0, COMBO_LENIENT, "", "", "/", FXAA_ORIGINAL, 0, DEFAULT_BINDS, THEME_HUNTER, 1, 1, 1, TOUCH_AUTO, 1, 1.0f, 0.8f, 100, 1, 1, 0, 1, 0, 1, \
    1, 0, 1, 1, TOUCH_SHOW_DEFAULT, 0, {{0}}, {PADB_NONE}, "Auto", 0, 1, 1, \
    .touch_scheme = TS_ADAPTIVE, .touch_show_trad = TOUCH_SHOW_TRAD_DEFAULT, .touch_auto_run = 0, \
    .crossplay = XP_OFF, .ryu_servers = "", .ryu_host = 0, .ryu_port = 30456, .ryu_password = "", \
    .room_server = "", .room_host = 0, .room_port = 24872, .room_name = "MHGU", .room_password = "", \
    .web_api_url = WEB_API_DEFAULT_URL, \
    .citra_web_api_url = CITRA_WEB_API_DEFAULT_URL, .citra_web_api_url_2 = CITRA_WEB_API_DEFAULT_URL_2
// (in Config's order, config.h: a new field there needs its value here, in its place - shadows, adreno_fix, bloom, reverb before
// touch_show: without them the touch buttons' defaults moved two places and most buttons were off; after touch_show the
// thank-you not shown yet, then the combined buttons, none, gpu, the menu's hint not seen yet, and the mouse in the game's
// menus. hub_full_list is the 0 after the empty ldn_join text. The Traditional layout's and crossplay's fields come last and
// are set by name)
Config g_config = {CONFIG_DEFAULT_VALUES};
Config config_defaults(void) { Config c = {CONFIG_DEFAULT_VALUES}; return c; }
uint32_t g_render_size[2] = {1920, 1080};
float g_render_aspect = 16.0f / 9.0f;
float g_fov_scale = 1.0f;
uint32_t g_ui_size[2] = {1280, 720};
float g_ui_anchor_aspect = 16.0f / 9.0f;
uint32_t g_fsr_output[2];
float g_fsr_lod_bias;
const char *g_fsr4_fallback;
const char *g_xess_fallback;

static const char DEFAULT_INI[] =
    "; MHGU Recomp settings. Delete this file to reset to default settings, the file is regenerated.\n"
    "\n"
    "[Display]\n"
    "; Fullscreen, Windowed, Borderless\n"
    "Mode = Borderless\n"
    "\n"
    "; Resolution (WxH), or Desktop: the screen's own\n"
    "Resolution = Desktop\n"
    "\n"
    "; Rendering resolution (WxH)\n"
    "; Higher = sharper (for supersampling)\n"
    "; Default: Auto\n"
    "RenderResolution = Auto\n"
    "\n"
    "; With RenderResolution Auto: the 3D picture's resolution, in percent of the screen's (lower = faster, softer)\n"
#ifndef __ANDROID__  // (config_load: below 100, fsr.c's plain scaling with the HUD at the screen's size)
    "; Below 100 the HUD and the text stay sharp: they are drawn at the screen's full resolution over the 3D picture\n"
#endif
    "; Between 25 and 100\n"
    "; Default: 100\n"
    "RenderScale = 100\n"
    "\n"
    "; Set to 16:9 to preserve the game's native aspect ratio\n"
    "; Default: Auto\n"
    "AspectRatio = Auto\n"
    "\n"
    "; Field of view\n"
    "; Between 0.5 and 2\n"
    "; Default: 1.0\n"
    "FOV = 1.0\n"
    "\n"
#ifdef __ANDROID__
    "; At resolutions wider than 16:9, determines where the HUD goes\n"
    "; Center, Edges\n"
#else  // (all four of the menu's, its 16:9 being Center; the menu's own help names them as it shows them)
    "; On a picture wider than 16:9, how wide the HUD is laid out: Center (the middle 16:9), 21:9, 32:9 or Edges (the\n"
    "; whole width)\n"
#endif
    "; Default: Center\n"
    "UIAnchor = Center\n"
    "\n"
    "; On a screen wider than 16:9, movies and the menus' backdrops fill it (a little is cut above and below) instead\n"
    "; of showing bars at the sides\n"
    "; Default: On\n"
    "FillScreen = On\n"
    "\n"
    "; The graphics card the game runs on: Auto (a dedicated card if there is one), or a card's name as the log's\n"
    "; [vk] gpu lines show it\n"
    "; Default: Auto\n"
    "GPU = Auto\n"
    "\n"
    "[Anti-aliasing]\n"
    "; Multi-Sample Anti Aliasing, cycle modes with F7 key\n"
    "; Off, 2x, 4x, 8x\n"
    "; Default: Off\n"
    "MSAA = Off\n"
    "\n"
    "; GU's default FXAA\n"
    "; Default: On\n"
#ifdef __ANDROID__  // (off on phones: a fifth of the frame rate on a Mali, and a green line at the screen's edges there)
    "FXAA = Off\n"
#else
    "FXAA = On\n"
#endif
    "\n"
    "; Which FXAA: Original is GU's own, 3_11 is FXAA 3.11, which keeps textures sharper\n"
    "; Original, 3_11\n"
    "; Default: Original\n"
    "FXAAMode = Original\n"
    "\n"
    "[Upscaler]\n"
    "; FSR temporal upscaling\n"
#ifndef __ANDROID__  // (measured on an RX 6800 XT: FSR 3.1 Quality 3.7 ms a frame, without it and with FXAA 1.8 to 2.3)
    "; FSR costs GPU time of its own: about 2 ms a frame on a fast graphics card, more on integrated graphics. For\n"
    "; speed on a weak GPU, lower RenderScale or use LowEnd instead\n"
#endif
    "; Default: Off\n"
    "FSR = Off\n"
    "\n"
    "; Which upscaler to use (FSR3 is FSR 3.1)\n"
#ifdef _WIN32  // (XeSS-SR: libxess.dll beside MHGU.exe, nvn/vulkan/xess.c)
    "; XeSS is XeSS-SR, for Intel Iris Xe or Arc graphics, or another card with DP4a (RX 6000, RTX 3000 or newer), where\n"
    "; it costs more than FSR 3.1. It needs the Visual C++ Redistributable (x64). Where it can't run, FSR 3.1 is used\n"
    "; FSR3, FSR4, XeSS\n"
#else
    "; FSR3, FSR4\n"
#endif
    "; Default: FSR3\n"
    "Upscaler = FSR3\n"
    "\n"
    "; FSR quality\n"
#ifndef __ANDROID__  // (config_load: FSRQuality's ratio; NativeAA renders at the output's size)
    "; NativeAA draws the 3D picture at full size: it only improves the picture, and costs more than FSR Off. Quality,\n"
    "; Balanced, Performance and UltraPerformance draw it at 67%, 59%, 50% and 33% and rebuild it\n"
#endif
    "; NativeAA, Quality, Balanced, Performance, UltraPerformance\n"
    "; Default: NativeAA\n"
    "FSRQuality = NativeAA\n"
    "\n"
    "; FSR's sharpening filter strength\n"
    "; Between 0 to 1\n"
    "; Default: 0.8\n"
    "Sharpness = 0.8\n"
    "\n"
    "; FSR Object Motion, reduces smearing\n"
    "; Default: On\n"
    "ObjectMotion = On\n"
    "\n"
    "; Reactive anti-ghosting\n"
    "; Between 0 and 1\n"
    "; Default: 0.9\n"
    "Reactive = 0.9\n"
    "\n"
    "; Animated surface textures skip FSR's frame history to avoid smearing\n"
    "; Default: On\n"
    "AnimSurf = On\n"
    "\n"
    "[Graphics]\n"
    "; How far away people in the villages and the Hubs are still drawn: 1 is the game's distance, 2 twice as far,\n"
    "; 0.5 half as far, 0 no limit\n"
    "; Between 0 and 10\n"
    "; Default: 1.0\n"
    "DrawDistance = 1.0\n"
    "\n"
    "; Renders the picture with 16 bits per colour instead of 8, so dark skies and soft glows show no colour bands.\n"
    "; Costs GPU time: about 1 ms a frame at 1600x1000, more at higher resolutions\n"
    "; Default: Off\n"
    "SmoothGradients = Off\n"
    "\n"
    "[Performance]\n"
#ifdef __ANDROID__
    "; Framerate cap\n"
    "; Between 30 and 240\n"
#else
    "; Framerate cap. Unlimited: as many frames as the PC can draw, with VSync Off (with VSync On, the monitor's\n"
    "; refresh rate is the most)\n"
    "; Between 30 and 240, or Unlimited (a number up to 1000 is taken too)\n"
#endif
    "; Default: 60\n"
    "FPS = 60\n"
    "\n"
    "; VSync\n"
    "VSync = On\n"
    "\n"
    "; Translate all of the game's shaders in the background at startup, so fewer things are drawn black the first time\n"
    "; they appear. The first start does this in about 20 seconds while the logos play; it needs about 340 MB in the cache folder.\n"
    "; Default: On\n"
    "ShaderPrewarm = On\n"
    "\n"
#ifdef __ANDROID__
    "; Off, On or Toaster. On, for a weak phone: no bloom or depth of field, cheaper and smaller textures, and the HUD stays\n"
    "; sharp over a smaller 3D picture. Toaster: all of that and more, for speed and the battery before looks: the HUD at\n"
    "; most 540 lines tall and lighter textures still. It does not change the resolution, the frame rate, Shadows, Bloom or\n"
    "; Reverb by itself: the menu's switch does that\n"
#else  // (a PC's GPU takes the game's BC textures as they are: textures.c makes smaller only those it decodes, a Mali's)
    "; Off, On or Toaster, for a weak GPU such as integrated graphics. On: no MSAA or FSR (FXAA, texture filtering and\n"
    "; the rest stay as set). Toaster, for the weakest: no FXAA either, and cheaper texture filtering. The menu's switch\n"
    "; also sets RenderScale for a 3D picture about 720 lines tall at the Resolution of the moment (Toaster: 540) and\n"
    "; Bloom off, and with Toaster 30 FPS and Shadows off; written here by hand, it changes none of those\n"
#endif
    "; Default: Off\n"
    "LowEnd = Off\n"
    "\n"
    "; The shadows of the hunters, monsters and scenery. Off: none, a little faster on a weak GPU\n"
    "; Default: On\n"
    "Shadows = On\n"
    "\n"
    "; The glow around lights and the blur of what is far away (bloom and depth of field). Off: faster on a weak GPU\n"
    "; Default: On\n"
    "Bloom = On\n"
    "\n"
    "; An Adreno GPU with a Vulkan 1.1 driver can show the 3D picture repeated in a grid below 720 lines without MSAA.\n"
    "; Auto keeps 2x MSAA there; Off leaves MSAA as set (faster, and some of those drivers look better without it)\n"
    "; Auto, Off\n"
    "AdrenoFix = Auto\n"
    "\n"
#ifdef __ANDROID__
    "; Shows the frame rate, the frame time and the device's temperature in a corner of the screen\n"
#else  // (a desktop: Off, On or Full, runtime/perfstats.cpp; "On" as before, for files of older versions)
    "; Shows the frame rate and the frame time at the top of the screen. On: with the GPU's temperature. Full: also a\n"
    "; line on the GPU (use, temperature, clock, video memory) and one on the CPU (the game's use and memory, the time\n"
    "; its game and render threads take per frame, the CPU's clock). The CPU's temperature is not shown: Windows lets\n"
    "; no program read it without installing a driver\n"
    "; Off, On, Full\n"
#endif
    "; Default: Off\n"
    "ShowStats = Off\n"
    "\n"
    "[Controls]\n"
    "; Controller face button layout. Set to \"Label\" to reverse A/B and X/Y on XBox controllers to match the button prompts (not recommended)\n"
    "; Position, Label\n"
    "; Default: Position\n"
    "FaceButtons = Position\n"
    "\n"
    "; Confirm button in the settings menu (F1, or Minus + Plus on a controller). The game's own menus are unchanged.\n"
    "; Original, South (bottom button), East (right button)\n"
    "; Default: Original\n"
    "MenuConfirm = Original\n"
    "\n"
    "; Controller input is also registered if the window is out of focus\n"
    "; Default: On\n"
    "BackgroundInput = On\n"
    "\n"
    "; How long the game waits for a second button after the first, for inputs like X+A. Lenient: as long as at 30 fps\n"
    "; (on the Switch) at any frame rate. Strict: a fixed number of frames, so the higher the frame rate, the closer\n"
    "; together both buttons have to be pressed\n"
    "; Lenient, Strict\n"
    "; Default: Lenient\n"
    "ComboInputWindow = Lenient\n"
    "\n"
    "; The key that opens a line of chat, anywhere (in a quest too): a key's name as the settings menu shows it, or\n"
    "; nothing for none\n"
    "; Default: /\n"
    "ChatKey = /\n"
    "\n"
    "; The game's rumble also on the phone's own vibrator (with or without a controller)\n"
    "; Default: On\n"
    "PhoneRumble = On\n"
    "\n"
    "; The lock-on, as in the newer games: the target icon or the right stick's click keep the camera on the monster,\n"
    "; and a bow's or a bowgun's shots go to it. Off: the icon turns the camera to it once, as in the original game\n"
    "; Default: On\n"
    "LockOn = On\n"
#ifndef __ANDROID__  // (runtime/hle/mousemenu.c)
    "\n"
    "; In the game's menus (the item box, the pouch, the pause menu) and while someone talks to you or the game asks\n"
    "; something, the mouse works as in the newer games: the pointer shows, the cursor goes where it points, left click\n"
    "; is A (it confirms, or goes on with a talk), right click is B (back), the wheel scrolls lists and turns pages, and\n"
    "; the side buttons turn pages too. Off: the mouse turns the camera there too, and its buttons press what Keybinds says\n"
    "; Default: On\n"
    "MouseMenus = On\n"
#endif
    "\n"
    "[Mouse]\n"
    "; Mouse sensitivity\n"
    "; Default: 1.0\n"
    "MouseSensitivity = 1.0\n"
    "\n"
    "[Touch]\n"
#ifdef __ANDROID__
    "; The controls on the screen (a phone): Auto shows them while no controller is connected\n"
#else  // (runtime/touch.cpp: a desktop's Auto waits for a finger on the screen)
    "; The controls on a touch screen: Auto shows them once a finger touches it, while no controller is connected\n"
#endif
    "; Auto, On, Off\n"
    "; Default: Auto\n"
    "Controls = Auto\n"
    "\n"
    "; Adaptive: the buttons change with what you do (the menu's, a bow's or a bowgun's). Traditional: as an emulator's, the same\n"
    "; buttons always (A B X Y, L R ZL ZR, + -, the d-pad), with a layout of their own in touch_layout_traditional.txt\n"
    "; Adaptive, Traditional\n"
    "; Default: Adaptive\n"
    "Layout = Adaptive\n"
    "\n"
    "; On: with the touch controls the HUD has an arrangement of its own, out of your thumbs' way. Off: the same HUD as\n"
    "; without them\n"
    "; Default: On\n"
    "MoveHUD = On\n"
    "\n"
    "; Traditional layout only: the thumb past the stick's rim runs (holds R) while the weapon is sheathed. Off: only the R button\n"
    "; runs, so the stick just walks. (The Adaptive layout always runs with the stick.)\n"
    "; Default: Off\n"
    "AutoRun = Off\n"
    "\n"
    "; Which of the game's buttons are on the screen in a hunt. A hunt needs none of those off by default\n"
    "ShowAttacks = On\n"
    "ShowDodge = On\n"
    "ShowGuard = On\n"
    "ShowSheathe = On\n"
    "ShowPlus = On\n"
    "ShowMinus = Off\n"
    "ShowDpad = Off\n"
    "ShowL = Off\n"
    "ShowZL = Off\n"
    "ShowZR = Off\n"
    "ShowL3 = Off\n"
    "ShowR3 = Off\n"
    "ShowStick = On\n"
    "\n"
    "; While a menu of the game's is open, a small pad shows that moves its cursor, with the page buttons above it (L and R,\n"
    "; or ZL and ZR if the game's options turn pages with them). The movement stick keeps walking and the other buttons stay\n"
    "; where they are; where the hunter can't walk (a town's item box, a shop) it moves the cursor too. If the pad covers a\n"
    "; combined button, move it with Customize controls and HUD.\n"
    "; Default: On\n"
    "ShowMenuPad = On\n"
    "\n"
    "; The same for the Traditional layout: every one of its buttons is on by default\n"
    "; Default: On\n"
    "TradShowAttacks = On\n"
    "TradShowDodge = On\n"
    "TradShowGuard = On\n"
    "TradShowSheathe = On\n"
    "TradShowPlus = On\n"
    "TradShowMinus = On\n"
    "TradShowDpad = On\n"
    "TradShowL = On\n"
    "TradShowZL = On\n"
    "TradShowZR = On\n"
    "TradShowL3 = On\n"
    "TradShowR3 = On\n"
    "TradShowStick = On\n"
    "\n"
    "; With a bow or a bowgun in hand, a layout of their own in a hunt, and which of its buttons show\n"
    "ShowRanged = On\n"
    "ShowFire = On\n"
    "ShowSecond = On\n"
    "ShowAim = On\n"
    "ShowRangedSpecial = On\n"
    "ShowAmmo = On\n"
    "\n"
    "; Arrows beside the item bar. A swipe on the bar turns it too\n"
    "; Default: Off\n"
    "ShowItemArrows = Off\n"
    "\n"
    "; Buttons of your own, each pressing several of the game's together: A+X, ZL+R, L+Up...\n"
    "; A B X Y L R ZL ZR Plus Minus Up Down Left Right L3 R3, joined by +\n"
    "; ComboNPad: a controller's button that does the same: None, L3, R3, Minus, Guide, Misc, Paddle1 to Paddle4, Touchpad\n"
    "Combo1 =\n"
    "Combo1Pad = None\n"
    "Combo2 =\n"
    "Combo2Pad = None\n"
    "Combo3 =\n"
    "Combo3Pad = None\n"
    "Combo4 =\n"
    "Combo4Pad = None\n"
    "Combo5 =\n"
    "Combo5Pad = None\n"
    "Combo6 =\n"
    "Combo6Pad = None\n"
    "\n"
    "; How far a drag turns the camera (1: about 90 degrees for a drag the screen's height)\n"
    "; Between 0.2 and 5\n"
    "; Default: 1.0\n"
    "CameraSpeed = 1.0\n"
    "\n"
    "; How solid the controls are drawn\n"
    "; Between 0.2 and 1\n"
    "; Default: 0.8\n"
    "Opacity = 0.8\n"
    "\n"
    "; A short vibration when a touch control is pressed\n"
    "; Default: On\n"
    "Vibration = On\n"
    "\n"
    "[Keybinds]\n"
    "; The keys or mouse buttons for each Switch button, for moving (the left stick), for the camera (the right stick)\n"
    "; and for turning the item bar (ItemLeft / ItemRight: one item per press, in a hunt, without holding L).\n"
    "; Combo1Key to Combo6Key press the buttons of [Touch] Combo1 to Combo6, the combined buttons, while held.\n"
    "; Up to two each, separated by a comma, or None.\n"
    "; Mouse buttons: Mouse Left, Mouse Right, Mouse Middle, Mouse 4, Mouse 5, Wheel Up, Wheel Down.\n"
#ifndef __ANDROID__  // (runtime/host_input.c mod_spent: a phone's keyboard as before)
    "; Ctrl, Alt and the Windows key press a button when tapped alone: with another key they are a shortcut\n"
    "; (Alt+Tab, Alt+Enter) and press nothing. Shift is held as any key. For moving and the camera they all work as held.\n"
    "; A mouse button on A presses it out of the game's menus and talks only with the weapon out, an attack: talking,\n"
    "; opening, gathering and picking up are a key's (F).\n"
#endif
    "; Esc, Insert, F1 and F7 to F12 are the recomp's own keys.\n"
    "MoveUp = W\n"
    "MoveDown = S\n"
    "MoveLeft = A\n"
    "MoveRight = D\n"
#ifdef __ANDROID__  // (a phone's keyboard as before; a PC's: Shift runs, E sheathes and uses items, Ctrl walks)
    "Walk = Left Shift\n"
#else
    "Walk = Left Ctrl\n"
#endif
    "CameraUp = I\n"
    "CameraDown = K\n"
    "CameraLeft = J\n"
    "CameraRight = L\n"
#ifdef __ANDROID__
    "A = Mouse Right\n"
#else
    "A = F, Mouse Right\n"
#endif
    "B = Space\n"
#ifdef __ANDROID__
    "X = Mouse Left\n"
    "Y = R, Mouse Middle\n"
    "L = Q\n"
    "R = E\n"
#else
    "X = Mouse Left, R\n"
    "Y = E, Mouse Middle\n"
    "L = Q\n"
    "R = Left Shift\n"
#endif
    "ZL = Tab\n"
    "ZR = T\n"
    "Plus = Return, Keypad Enter\n"
    "Minus = Backspace\n"
    "DpadUp = Up\n"
    "DpadDown = Down\n"
    "DpadLeft = Left\n"
    "DpadRight = Right\n"
#ifdef __ANDROID__
    "LeftStickClick = Left Ctrl, Right Ctrl\n"
#else
    "LeftStickClick = M, Right Ctrl\n"
#endif
    "RightStickClick = Left Alt, Right Alt\n"
    "ItemLeft = Wheel Up\n"
    "ItemRight = Wheel Down\n"
    "Combo1Key = None\n"
    "Combo2Key = None\n"
    "Combo3Key = None\n"
    "Combo4Key = None\n"
    "Combo5Key = None\n"
    "Combo6Key = None\n"
    "\n"
    "[Multiplayer]\n"
    "; Which port to host/join hubs on (TCP and UDP)\n"
    "; Default: 39500\n"
    "Port = 39500\n"
    "\n"
    "; IP Address of the host to join. Use IP:Port if it doesn't match the port above.\n"
    "; Can be multiple IP addresses, comma-separated\n"
    "; Default:\n"
    "Join =\n"
    "\n"
    "; Hub server for playing over the internet without port forwarding: hubs you create are listed there, and\n"
    "; Search Hubs shows its hubs next to the LAN's. host or host:port (default port 39510).\n"
    "; Several servers can be named, separated by commas or spaces (up to 8): your hubs are listed on all of them,\n"
    "; and Search Hubs shows the hubs of all of them. A name starting with - is kept but switched off.\n"
    "; Hubs are listed for both link versions (2: this port, 3: the MHGURecomp PC build), so both find them.\n"
    "; Empty: the default (hunterhubid-gu-hub.duckdns.org and morerokk-mhgurecomp-ldnserver.duckdns.org). Off: no internet hubs.\n"
    "; Default:\n"
    "Server =\n"
    "\n"
    "; Matchmaking key: hubs are only seen by players with the same key, so friends who all enter the same key\n"
    "; only see each other's hubs. Empty: public, your hubs are seen by (and you see) everyone without a key.\n"
    "; Default:\n"
    "Key =\n"
    "\n"
    "; Crossplay mode: Off keeps native hubs only; Ryujinx uses RyuLdn servers; Yuzu Room uses Yuzu-family rooms (Eden, Suyu,\n"
    "; Yuzu, Citron); Mixed uses both at once; Citra Room plays with Monster Hunter XX players in Azahar / Citra rooms; All Rooms\n"
    "; hosts one hub on the LAN / hub server, in the Yuzu room and in the Citra room at once (this device relays between them;\n"
    "; the Yuzu room works best run here, RoomHost). Everyone must run the same game version (1.4.0). Small monster sync and HP\n"
    "; scaling are port features: they are off in crossplay hubs.\n"
    "; Default: Off\n"
    "Crossplay = Off\n"
    "\n"
    "; RyuLdn server addresses, host[:port], comma separated. Default port: 30456. Empty: none (RyuHost can run one here).\n"
    "; Default:\n"
    "RyuServers =\n"
    "\n"
    "; Run a RyuLdn server on this device for Ryujinx players. Others use this device's address and RyuPort.\n"
    "; Internet play needs port forwarding or a shared VPN.\n"
    "; Default: Off\n"
    "RyuHost = Off\n"
    "\n"
    "; TCP port of the RyuLdn server this device runs (1 to 65535).\n"
    "; Default: 30456\n"
    "RyuPort = 30456\n"
    "\n"
    "; RyuLdn lobby passphrase. Empty: public. A private lobby is Ryujinx- and eight hexadecimal digits, the same on every client.\n"
    "; Default:\n"
    "RyuPassword =\n"
    "\n"
    "; Yuzu-family room server, host[:port] (Eden, Suyu, Yuzu, Citron). Default port: 24872. Empty: no remote room.\n"
    "; The Yuzu Room Browser in the Network tab fills this in from the rooms the Yuzu (Eden) Web API lists.\n"
    "; Default:\n"
    "RoomServer =\n"
    "\n"
    "; Run a Yuzu-family room on this device. Others use this device's address and RoomPort.\n"
    "; Internet play needs port forwarding or a shared VPN.\n"
    "; Default: Off\n"
    "RoomHost = Off\n"
    "\n"
    "; Port of the room this device runs (1 to 65535).\n"
    "; Default: 24872\n"
    "RoomPort = 24872\n"
    "\n"
    "; Your nickname in a room, and the name of the room this device runs.\n"
    "; Default: MHGU\n"
    "RoomName = MHGU\n"
    "\n"
    "; Room password. Empty: open. Everyone in the room uses the same one.\n"
    "; Default:\n"
    "RoomPassword =\n"
    "\n"
    "; The Yuzu (Eden) Web API the Yuzu Room Browser asks for the list of public rooms (a host, or http:// / https:// and a host).\n"
    "; Eden and Suyu both come from Yuzu and share this room list.\n"
    "; Default: api.ynet-fun.xyz\n"
    "WebApiUrl = api.ynet-fun.xyz\n"
    "\n"
    "; The room the Room Browser joined last, kept so it is pinned at the top of the list. The browser writes these two.\n"
    "; Default:\n"
    "LastRoom =\n"
    "LastRoomName =\n"
    "\n"
    "; Citra-family room server (Azahar, Citra), host[:port]. Default port: 24872. Empty: none. Crossplay = Citra Room plays\n"
    "; Monster Hunter XX (3DS) players in it. The Citra Room Browser in the Network tab fills this in.\n"
    "; Default:\n"
    "CitraRoomServer =\n"
    "\n"
    "; The Citra room's password. Empty: open.\n"
    "; Default:\n"
    "CitraRoomPassword =\n"
    "\n"
    "; The Citra Web API the Citra Room Browser asks for the public rooms (a host, or http:// / https:// and a host), and a\n"
    "; second one whose rooms are added to the list. Empty: off. The defaults are AzaharPlus's.\n"
    "; Default: http://88.198.47.46:5000 and http://3ds.mgn.pub:3000\n"
    "CitraWebApiUrl = http://88.198.47.46:5000\n"
    "CitraWebApiUrl2 = http://3ds.mgn.pub:3000\n"
    "\n"
    "; The token sent with a join request to a Citra room (an Azahar network token, or a room JWT). Empty: join anonymously,\n"
    "; which public rooms accept.\n"
    "; Default:\n"
    "CitraToken =\n"
    "\n"
    "; The Citra room the Citra Room Browser joined last, pinned at the top of the list. The browser writes these two.\n"
    "; Default:\n"
    "LastCitraRoom =\n"
    "LastCitraRoomName =\n"
    "\n"
    "; Sync small monsters between players in multiplayer.\n"
    "; (Currently off by default, pending a bug investigation where small monsters may refuse to attack players)\n"
    "; Default: Off\n"
    "SyncSmallMonsters = Off\n"
    "\n"
    "; Join Hub search: a hub server answers each search with only part of its hubs, so the list grows as you press Retry.\n"
    "; On: each search asks every hub server several times at once and shows nearly all the hubs straight away.\n"
    "; Off: one answer per search; press Retry to see more. (A little more network use when On.)\n"
    "; Default: Off\n"
    "FullHubList = Off\n"
    "\n"
    "[Monster HP]\n"
    "; Playercount-specific HP multipliers for monsters in Hub quests.\n"
    "; Only affects large monster health.\n"
    "; When joining a hub, the host's scaling settings are always used.\n"
    "; Default: 1.0\n"
    "1Player = 1.0\n"
    "2Players = 1.0\n"
    "3Players = 1.0\n"
    "4Players = 1.0\n"
    "\n"
    "; (Optional: quest-specific HP multipliers go here. Comma-separated per playercount, 1 line per quest. Example:)\n"
    "; Quest10107 = 1.0, 1.0, 1.0, 1.0\n"
    "\n"
    "[Village Quests]\n"
    "; Press L/R in the Hub while selecting a quest to swap to the Village Quest list instead, letting you play Village quests in multiplayer.\n"
    "; Default: On\n"
    "VillageQuestsInHub = On\n"
    "\n"
    "; When playing a Village Quest from the Hub, first multiply the monster HP by this amount.\n"
    "; This amount is then scaled again by the [Monster HP] settings.\n"
    "; Default: 2.0\n"
    "HubHP = 2.0\n"
    "\n"
    "[Discord]\n"
    "; Show what you're doing in the game on your Discord profile (Discord must be running on this computer):\n"
    "; where you are, the quest's monsters and how many hunters are with you\n"
    "; Default: On\n"
    "RichPresence = On\n"
    "\n"
    "; Show your hunter's name and Hunter Rank there too\n"
    "; Default: On\n"
    "ShowHunter = On\n"
    "\n"
    "[Bonus Content]\n"
    "; If enabled, the free online downloadable content (Event Quests, item packs, Palicoes) are automatically copied from the provided savegame inside the `bonus/` folder into your own savegame.\n"
    "; This only happens if your savegame hasn't received any downloadable content yet.\n"
    "; Default: On\n"
    "BonusContent = On\n"
    "\n"
    "[Audio]\n"
    "; Volume of the whole game\n"
    "; Between 0 (mute) and 100\n"
    "; Default: 100\n"
    "Volume = 100\n"
    "\n"
    "; Mute the game while its window is in the background (not focused)\n"
    "; Default: Off\n"
    "MuteInBackground = Off\n"
    "\n"
#ifdef __ANDROID__
    "; The echo of caves and rooms (the game's reverb). Off: the sound is dry, and a weak phone's processor does less\n"
#else
    "; The echo of caves and rooms (the game's reverb). Off: the sound is dry, and a weak processor does less\n"
#endif
    "; Default: On\n"
    "Reverb = On\n"
    "\n"
    "[Application]\n"
    "; The game stops while the app is in the background (a phone: another app or the home screen; a PC: its window\n"
    "; minimized), so it uses no battery there; it goes on where it was when you come back\n"
    "; Default: On\n"
    "PauseInBackground = On\n"
    "\n"
    "; Ask before the game window closes (closing does not save the game)\n"
    "; Default: On\n"
    "ConfirmExit = On\n"
    "\n"
    "; Language of the settings menu (F1, or Minus + Plus on a controller). Auto: the phone's (Chinese, Spanish, else English)\n"
    "; Auto, English, Spanish, Chinese\n"
    "; Default: Auto\n"
    "Language = Auto\n"
    "\n"
    "; Button icons in the settings menu\n"
    "; Xbox, Switch, PlayStation\n"
    "; Default: Xbox\n"
    "ButtonIcons = Xbox\n"
    "\n"
    "; Colours of the settings menu\n"
    "; Gold, Blue, Hunter\n"
    "; Default: Hunter\n"
    "Theme = Hunter\n"
    "\n"
    "; Size of the settings menu, in percent\n"
    "; Between 50 and 200\n"
    "; Default: 100\n"
    "MenuScale = 100\n"
    "\n"
    "; The thank-you shown once, after the credits, the first time the game opens (the settings menu's Credits & Licenses\n"
    "; shows it again). On once it has been closed\n"
    "; Default: Off\n"
    "ThanksShown = Off\n"
    "\n"
    "; Not used: MHGU has no updater, and starting the game never looks for or installs a new version (the setting was\n"
    "; for the original project's updater).\n"
    "; Ask, Auto, Off\n"
    "; Default: Ask\n"
    "Updates = Ask\n"
#ifndef __ANDROID__  // (runtime/overlay.cpp menu_hint; settings_panel.cpp turns it On)
    "\n"
    "; The notice at the start that says F1 (or Minus + Plus on a controller) opens the settings menu shows until the menu\n"
    "; has been opened once, which turns this On\n"
    "; Default: Off\n"
    "MenuHintSeen = Off\n"
#endif
    ;

void config_path(char *buf, size_t size) {
    const char *e = getenv("MHGU_CONFIG");
    if (e && *e) { snprintf(buf, size, "%s", e); return; }
    snprintf(buf, size, "mhgurecomp.ini");  // (started on its own the game runs in its folder, or MHGU_HOME: main.c)
}

// lower case, spaces and '_' / '-' removed: "Borderless Window" == "borderlesswindow"
static void squash(char *s) {
    char *d = s;
    for (; *s; s++) if (!isspace((unsigned char)*s) && *s != '_' && *s != '-') *d++ = (char)tolower((unsigned char)*s);
    *d = 0;
}

static bool parse_size(const char *v, int *w, int *h) {
    int a, b;
    char x;
    if (sscanf(v, "%d %c %d", &a, &x, &b) != 3 || (x != 'x' && x != 'X' && x != '*')) return false;
    if (a < 320 || b < 180 || a > 16384 || b > 16384) return false;
    *w = a, *h = b;
    return true;
}

// [Monster HP] factors: "0.6", "0.6x", or a comma separated list of up to four (squashed: no spaces). The count read,
// 0 when one is not a number in HP_SCALE_MIN..HP_SCALE_MAX.
static int parse_factors(const char *v, float f[4]) {
    int n = 0;
    while (*v && n < 4) {
        char *end;
        float x = strtof(v, &end);
        if (end == v || !(x >= HP_SCALE_MIN && x <= HP_SCALE_MAX)) return 0;
        f[n++] = x;
        v = end + (*end == 'x');
        if (*v == ',') v++;
        else if (*v) return 0;
    }
    return *v ? 0 : n;
}

// The vertical field of view (radians) a camera of this aspect gets (tools/gen.py PATCHES: the three callers of the
// perspective matrix that load a camera's). One made for the screen (its 16:9 aspect replaced with g_render_aspect)
// gets the FOV factor and, on a picture narrower than 16:9 (16:10), the height that keeps 16:9's width (Vert+): more
// above and below instead of less to the sides. Other cameras keep their own.
float camera_fovy(float fovy, float aspect) {
    // (near enough: a small 3D picture's size is rounded to whole pixels, and at 33% its shape came out a little off the
    // screen's - the FOV setting did nothing there, and did at 50%)
    if (!(fabsf(aspect / g_render_aspect - 1.0f) < 0.006f)) return fovy;
    fovy *= config_live_fov();
    if (aspect < 16.0f / 9.0f) fovy = 2.0f * atanf(tanf(fovy * 0.5f) * (16.0f / 9.0f) / aspect);
    return fovy;
}

// (a desktop) The window resized, or went full screen or back (host_input.c, SDL's pixel size): with AspectRatio = Auto
// the picture takes the window's shape at once, as config_load gives it at the start (16:10 at the narrowest; near 16:9
// is 16:9). The 3D picture keeps the size it was made at: the game's cameras take the new shape (gen.py PATCHES read
// g_render_aspect at each projection), the HUD is placed for it (nvn/vulkan/exec.c, in fractions of the picture) and the
// window shows the picture in that shape, stretched to it (nvn/vulkan/device.c backend_present).
void config_window_shape(int w, int h) {
#ifndef __ANDROID__
    if (w <= 0 || h <= 0 || g_config.aspect_ratio > 0) return;
    float a = (float)w / (float)h;
    if (a < 1.6f) a = 1.6f;
    if (fabsf(a - 16.0f / 9.0f) < 0.01f) a = 16.0f / 9.0f;  // (1366x768 and the like)
    if (fabsf(a - g_render_aspect) < 0.001f) return;
    __atomic_store(&g_render_aspect, &a, __ATOMIC_RELAXED);  // (read by the game's thread and the renderer's)
    LOG("[config] window %dx%d: the picture's shape is %.3f now\n", w, h, (double)a);
#else
    (void)w, (void)h;
#endif
}

// Town NPCs (uLobbyNpc: villages, Hubs) fade in within a distance from the game's table (2500 units, 1800 for some)
// and out beyond it; tools/gen.py PATCHES hands its square here. [Graphics] DrawDistance scales it, 0 is no limit.
float npc_fade_dist2(float d2) {
    float m = config_live_draw_distance();
    return m > 0 ? d2 * m * m : FLT_MAX;
}

// A picture shape: "Auto" (0), "21:9" / "21/9", or a number (0.5 .. 10).
static bool parse_aspect(const char *v, float *out) {
    float n, d, a;
    char sep, *end;
    if (!strcasecmp(v, "auto")) { *out = 0; return true; }
    if (sscanf(v, "%f %c %f", &n, &sep, &d) == 3 && (sep == ':' || sep == '/') && n > 0 && d > 0) {
        a = n / d;
    } else {
        a = strtof(v, &end);
        if (end == v || *end) return false;
    }
    if (a < 0.5f || a > 10.0f) return false;
    *out = a;
    return true;
}

// ---- the settings of the file (config.h ConfigSetting): one row each, in the file's order
// config_validate's messages (settings_locale.h has them in Spanish; tests/test_settings_locale.py checks)
#define ERR_SIZE "Resolution must be Desktop/Auto or 320x180 to 16384x16384."
#define ERR_ASPECT "Aspect ratio must be Auto or between 0.5 and 10."
#define ERR_VIEW "FOV must be 0.5-2.0 and mouse sensitivity 0.1-10."
#define ERR_MISC "Invalid frame cap, anti-aliasing, controller, HUD or volume setting."
#define ERR_TOGGLE "Invalid On/Off value."
#define ERR_FSR "Invalid FSR quality, provider, sharpness or reactive mask."
#define ERR_NET "Invalid port or Join address list (no newlines, ';' or '#')."
#define ERR_SERVER "Invalid hub server or matchmaking key (no newlines, ';' or '#')."
#define ERR_XP "Invalid crossplay mode, port, server, room name, password or Web API URL (no newlines, ';' or '#')."
#define ERR_HP "HP factors must be 0.05-20."
#define ERR_VILLAGE "Invalid village HP or quest override count."
#define ERR_CONFIRM "Invalid menu confirmation policy."
#define ERR_SCALE "Invalid interface scale."
#define ERR_ICONS "Invalid button icon layout."
#define ERR_LANGUAGE "Invalid menu language."
#define ERR_UPDATES "Invalid update setting."
#define ERR_DRAW "Draw distance must be 0-10 (0: no limit)."
#define ERR_KEYS "Invalid keybind."
#define F(x) offsetof(Config, x)
#define ON_OFF(section, key, field, restart) {section, key, SET_BOOL, F(field), 0, 0, 0, NULL, NULL, restart, ERR_TOGGLE}
#define KEYS(key, bind) {"Keybinds", key, SET_KEYS, F(binds[bind]), 0, 0, 0, NULL, NULL, false, ERR_KEYS}
static const int anchor_values[] = {UI_CENTER, UI_21_9, UI_32_9, UI_EDGES}, msaa_values[] = {1, 2, 4, 8};
const ConfigSetting g_settings[] = {
    {"Display", "Mode", SET_CHOICE, F(display_mode), 0, 0, 0,
     "Windowed|window,Fullscreen|exclusive|exclusivefullscreen,Borderless|borderless*", NULL, true, ERR_SIZE},
    {"Display", "Resolution", SET_SIZE, F(width), F(height), 0, 0, "Desktop|auto", NULL, true, ERR_SIZE},
    {"Display", "RenderResolution", SET_SIZE, F(render_w), F(render_h), 0, 0, "Auto", NULL, true, ERR_SIZE},
    {"Display", "RenderScale", SET_INT, F(render_scale), 0, 25, 100, NULL, NULL, true, ERR_SIZE},
    {"Display", "AspectRatio", SET_ASPECT, F(aspect_ratio), 0, 0.5f, 10.0f, NULL, NULL, true, ERR_ASPECT},
    {"Display", "FOV", SET_FLOAT, F(fov), 0, 0.5f, 2.0f, NULL, NULL, false, ERR_VIEW},
    {"Display", "UIAnchor", SET_CHOICE, F(ui_anchor), 0, 0, 0, "Center|1|16:9,21:9|2,32:9|3,Edges", anchor_values, true,
     ERR_MISC},
    ON_OFF("Display", "FillScreen", fill_screen, true),
    {"Display", "GPU", SET_TEXT, F(gpu), sizeof g_config.gpu, 0, 0, NULL, NULL, true, ERR_MISC},
    {"Anti-aliasing", "MSAA", SET_CHOICE, F(msaa), 0, 0, 0, "Off|none|0|1|1x,2x|2,4x|4,8x|8", msaa_values, false, ERR_MISC},
    ON_OFF("Anti-aliasing", "FXAA", fxaa, false),
    {"Anti-aliasing", "FXAAMode", SET_CHOICE, F(fxaa_mode), 0, 0, 0, "Original,3_11", NULL, false, ERR_MISC},
    ON_OFF("Upscaler", "FSR", fsr, true),
    {"Upscaler", "Upscaler", SET_CHOICE, F(fsr_upscaler), 0, 0, 0, "FSR3|fsr3.1|3,FSR4|4,XeSS|xesssr", NULL, true, ERR_FSR},
    {"Upscaler", "FSRQuality", SET_CHOICE, F(fsr_quality), 0, 0, 0,
     "NativeAA|native|aa,Quality,Balanced,Performance,UltraPerformance|ultraperf", NULL, true, ERR_FSR},
    {"Upscaler", "Sharpness", SET_FLOAT, F(fsr_sharpness), 0, 0.0f, 1.0f, NULL, NULL, true, ERR_FSR},
    ON_OFF("Upscaler", "ObjectMotion", fsr_objmotion, true),
    {"Upscaler", "Reactive", SET_FLOAT, F(fsr_reactive), 0, 0.0f, 1.0f, NULL, NULL, true, ERR_FSR},
    ON_OFF("Upscaler", "AnimSurf", fsr_animsurf, true),
    {"Graphics", "DrawDistance", SET_FLOAT, F(draw_distance), 0, 0.0f, 10.0f, NULL, NULL, false, ERR_DRAW},
    ON_OFF("Graphics", "SmoothGradients", smooth_gradients, false),
    {"Performance", "FPS|FrameRate", SET_INT, F(fps), 0, 30, FPS_MAX, NULL, NULL, false, ERR_MISC},
    ON_OFF("Performance", "VSync", vsync, false),
    ON_OFF("Performance", "ShaderPrewarm", shader_prewarm, true),
    {"Performance", "LowEnd", SET_CHOICE, F(low_end), 0, 0, 0, "Off|false|no|0,On|true|yes|1,Toaster|tostadora|2", NULL, true,
     ERR_MISC},
    ON_OFF("Performance", "Shadows", shadows, false),
    ON_OFF("Performance", "Grass", grass, false),
    ON_OFF("Performance", "Bloom", bloom, false),
    {"Performance", "AdrenoFix", SET_CHOICE, F(adreno_fix), 0, 0, 0, "Auto,Off", NULL, true, ERR_MISC},
#ifdef __ANDROID__
    ON_OFF("Performance", "ShowStats", show_stats, false),
#else  // (a desktop: On is the menu's Basic, as an older file has it; Full adds the GPU's and the CPU's lines)
    {"Performance", "ShowStats", SET_CHOICE, F(show_stats), 0, 0, 0, "Off|false|no|0,On|true|yes|1|basic,Full|2", NULL,
     false, ERR_MISC},
#endif
    {"Controls", "FaceButtons", SET_CHOICE, F(face_buttons), 0, 0, 0, "Position|positional,Label|labels|xbox",
     NULL, false, ERR_MISC},
    {"Controls", "MenuConfirm", SET_CHOICE, F(menu_confirm), 0, 0, 0, "Original,South,East", NULL, false, ERR_CONFIRM},
    ON_OFF("Controls", "BackgroundInput", background_input, false),
    {"Controls", "ComboInputWindow", SET_CHOICE, F(combo_window), 0, 0, 0, "Lenient,Strict", NULL, false, ERR_MISC},
    {"Controls", "ChatKey", SET_TEXT, F(chat_key), sizeof g_config.chat_key, 0, 0, NULL, NULL, false, ERR_MISC},
    {"Mouse", "MouseSensitivity", SET_FLOAT, F(mouse_sensitivity), 0, 0.1f, 10.0f, NULL, NULL, false, ERR_VIEW},
    ON_OFF("Controls", "PhoneRumble", phone_rumble, false),
    ON_OFF("Controls", "LockOn", lock_on, false),
#ifndef __ANDROID__
    ON_OFF("Controls", "MouseMenus", mouse_menus, false),
#endif
    {"Touch", "Controls", SET_CHOICE, F(touch_controls), 0, 0, 0, "Auto,On,Off", NULL, false, ERR_MISC},
    {"Touch", "Layout", SET_CHOICE, F(touch_scheme), 0, 0, 0, "Adaptive|adaptive|0,Traditional|tradicional|1", NULL, false, ERR_MISC},
    ON_OFF("Touch", "MoveHUD", touch_hud, false),
    ON_OFF("Touch", "AutoRun", touch_auto_run, false),
    ON_OFF("Touch", "ShowAttacks", touch_show[TB_ATTACKS], false),
    ON_OFF("Touch", "ShowDodge", touch_show[TB_DODGE], false),
    ON_OFF("Touch", "ShowGuard", touch_show[TB_GUARD], false),
    ON_OFF("Touch", "ShowSheathe", touch_show[TB_SHEATHE], false),
    ON_OFF("Touch", "ShowPlus", touch_show[TB_PLUS], false),
    ON_OFF("Touch", "ShowMinus", touch_show[TB_MINUS], false),
    ON_OFF("Touch", "ShowDpad", touch_show[TB_DPAD], false),
    ON_OFF("Touch", "ShowL", touch_show[TB_L], false),
    ON_OFF("Touch", "ShowZL", touch_show[TB_ZL], false),
    ON_OFF("Touch", "ShowZR", touch_show[TB_ZR], false),
    ON_OFF("Touch", "ShowMenuZL", touch_show[TB_MENU_ZL], false),
    ON_OFF("Touch", "ShowMenuZR", touch_show[TB_MENU_ZR], false),
    ON_OFF("Touch", "ShowMenuPad", touch_show[TB_MENU_PAD], false),
    ON_OFF("Touch", "ShowL3", touch_show[TB_L3], false),
    ON_OFF("Touch", "ShowR3", touch_show[TB_R3], false),
    ON_OFF("Touch", "ShowStick", touch_show[TB_STICK], false),
    ON_OFF("Touch", "TradShowAttacks", touch_show_trad[TB_ATTACKS], false),
    ON_OFF("Touch", "TradShowDodge", touch_show_trad[TB_DODGE], false),
    ON_OFF("Touch", "TradShowGuard", touch_show_trad[TB_GUARD], false),
    ON_OFF("Touch", "TradShowSheathe", touch_show_trad[TB_SHEATHE], false),
    ON_OFF("Touch", "TradShowPlus", touch_show_trad[TB_PLUS], false),
    ON_OFF("Touch", "TradShowMinus", touch_show_trad[TB_MINUS], false),
    ON_OFF("Touch", "TradShowDpad", touch_show_trad[TB_DPAD], false),
    ON_OFF("Touch", "TradShowL", touch_show_trad[TB_L], false),
    ON_OFF("Touch", "TradShowZL", touch_show_trad[TB_ZL], false),
    ON_OFF("Touch", "TradShowZR", touch_show_trad[TB_ZR], false),
    ON_OFF("Touch", "TradShowL3", touch_show_trad[TB_L3], false),
    ON_OFF("Touch", "TradShowR3", touch_show_trad[TB_R3], false),
    ON_OFF("Touch", "TradShowStick", touch_show_trad[TB_STICK], false),
    ON_OFF("Touch", "ShowRanged", touch_show[TB_RANGED], false),
    ON_OFF("Touch", "ShowFire", touch_show[TB_FIRE], false),
    ON_OFF("Touch", "ShowSecond", touch_show[TB_SECOND], false),
    ON_OFF("Touch", "ShowAim", touch_show[TB_AIM], false),
    ON_OFF("Touch", "ShowRangedSpecial", touch_show[TB_RSPECIAL], false),
    ON_OFF("Touch", "ShowAmmo", touch_show[TB_AMMO], false),
    ON_OFF("Touch", "ShowItemArrows", touch_show[TB_ITEM_ARROWS], false),
#define COMBO(n) {"Touch", "Combo" #n, SET_TEXT, F(touch_combo[n - 1]), sizeof g_config.touch_combo[0], 0, 0, NULL, NULL, false, ERR_MISC}, \
    {"Touch", "Combo" #n "Pad", SET_CHOICE, F(touch_combo_pad[n - 1]), 0, 0, 0, \
     "None,L3,R3,Minus,Guide,Misc,Paddle1,Paddle2,Paddle3,Paddle4,Touchpad", NULL, false, ERR_MISC}
    COMBO(1), COMBO(2), COMBO(3), COMBO(4), COMBO(5), COMBO(6),
    {"Touch", "CameraSpeed", SET_FLOAT, F(touch_camera), 0, 0.2f, 5.0f, NULL, NULL, false, ERR_VIEW},
    {"Touch", "Opacity", SET_FLOAT, F(touch_opacity), 0, 0.2f, 1.0f, NULL, NULL, false, ERR_VIEW},
    ON_OFF("Touch", "Vibration", touch_vibration, false),
    KEYS("MoveUp", BIND_MOVE_UP), KEYS("MoveDown", BIND_MOVE_DOWN), KEYS("MoveLeft", BIND_MOVE_LEFT),
    KEYS("MoveRight", BIND_MOVE_RIGHT), KEYS("Walk", BIND_WALK), KEYS("CameraUp", BIND_CAM_UP),
    KEYS("CameraDown", BIND_CAM_DOWN), KEYS("CameraLeft", BIND_CAM_LEFT), KEYS("CameraRight", BIND_CAM_RIGHT),
    KEYS("A", BIND_A), KEYS("B", BIND_B), KEYS("X", BIND_X),
    KEYS("Y", BIND_Y), KEYS("L", BIND_L), KEYS("R", BIND_R), KEYS("ZL", BIND_ZL), KEYS("ZR", BIND_ZR),
    KEYS("Plus", BIND_PLUS), KEYS("Minus", BIND_MINUS), KEYS("DpadUp", BIND_DPAD_UP), KEYS("DpadDown", BIND_DPAD_DOWN),
    KEYS("DpadLeft", BIND_DPAD_LEFT), KEYS("DpadRight", BIND_DPAD_RIGHT), KEYS("LeftStickClick", BIND_LSTICK),
    KEYS("RightStickClick", BIND_RSTICK), KEYS("ItemLeft", BIND_ITEM_LEFT), KEYS("ItemRight", BIND_ITEM_RIGHT),
    // (not "Combo1": [Touch] has that key, and a key names one setting whatever its section)
    KEYS("Combo1Key", BIND_COMBO1), KEYS("Combo2Key", BIND_COMBO2), KEYS("Combo3Key", BIND_COMBO3),
    KEYS("Combo4Key", BIND_COMBO4), KEYS("Combo5Key", BIND_COMBO5), KEYS("Combo6Key", BIND_COMBO6),
    {"Multiplayer", "Port", SET_INT, F(ldn_port), 0, 1, 65535, NULL, NULL, true, ERR_NET},
    {"Multiplayer", "Join", SET_TEXT, F(ldn_join), sizeof g_config.ldn_join, 0, 0, NULL, NULL, true, ERR_NET},
    {"Multiplayer", "Server", SET_TEXT, F(ldn_server), sizeof g_config.ldn_server, 0, 0, NULL, NULL, false, ERR_SERVER},
    {"Multiplayer", "Key", SET_TEXT, F(ldn_key), sizeof g_config.ldn_key, 0, 0, NULL, NULL, false, ERR_SERVER},
    {"Multiplayer", "Crossplay", SET_CHOICE, F(crossplay), 0, 0, 0, "Off,Ryujinx,Yuzu Room|Room|Yuzu|Eden|Suyu,Mixed,Citra Room|Citra|Azahar,All Rooms|All|Rooms", NULL, false, ERR_XP},
    {"Multiplayer", "RyuServers", SET_TEXT, F(ryu_servers), sizeof g_config.ryu_servers, 0, 0, NULL, NULL, false, ERR_XP},
    ON_OFF("Multiplayer", "RyuHost", ryu_host, false),
    {"Multiplayer", "RyuPort", SET_INT, F(ryu_port), 0, 1, 65535, NULL, NULL, false, ERR_XP},
    {"Multiplayer", "RyuPassword", SET_TEXT, F(ryu_password), sizeof g_config.ryu_password, 0, 0, NULL, NULL, false, ERR_XP},
    {"Multiplayer", "RoomServer", SET_TEXT, F(room_server), sizeof g_config.room_server, 0, 0, NULL, NULL, false, ERR_XP},
    ON_OFF("Multiplayer", "RoomHost", room_host, false),
    {"Multiplayer", "RoomPort", SET_INT, F(room_port), 0, 1, 65535, NULL, NULL, false, ERR_XP},
    {"Multiplayer", "RoomName", SET_TEXT, F(room_name), sizeof g_config.room_name, 0, 0, NULL, NULL, false, ERR_XP},
    {"Multiplayer", "RoomPassword", SET_TEXT, F(room_password), sizeof g_config.room_password, 0, 0, NULL, NULL, false, ERR_XP},
    {"Multiplayer", "WebApiUrl", SET_TEXT, F(web_api_url), sizeof g_config.web_api_url, 0, 0, NULL, NULL, false, ERR_XP},
    {"Multiplayer", "LastRoom", SET_TEXT, F(last_room), sizeof g_config.last_room, 0, 0, NULL, NULL, false, ERR_XP},
    {"Multiplayer", "LastRoomName", SET_TEXT, F(last_room_name), sizeof g_config.last_room_name, 0, 0, NULL, NULL, false, ERR_XP},
    {"Multiplayer", "CitraRoomServer", SET_TEXT, F(citra_room_server), sizeof g_config.citra_room_server, 0, 0, NULL, NULL, false, ERR_XP},
    {"Multiplayer", "CitraRoomPassword", SET_TEXT, F(citra_room_password), sizeof g_config.citra_room_password, 0, 0, NULL, NULL, false, ERR_XP},
    {"Multiplayer", "CitraWebApiUrl", SET_TEXT, F(citra_web_api_url), sizeof g_config.citra_web_api_url, 0, 0, NULL, NULL, false, ERR_XP},
    {"Multiplayer", "CitraWebApiUrl2", SET_TEXT, F(citra_web_api_url_2), sizeof g_config.citra_web_api_url_2, 0, 0, NULL, NULL, false, ERR_XP},
    {"Multiplayer", "CitraToken", SET_TEXT, F(citra_token), sizeof g_config.citra_token, 0, 0, NULL, NULL, false, ERR_XP},
    {"Multiplayer", "LastCitraRoom", SET_TEXT, F(last_citra_room), sizeof g_config.last_citra_room, 0, 0, NULL, NULL, false, ERR_XP},
    {"Multiplayer", "LastCitraRoomName", SET_TEXT, F(last_citra_room_name), sizeof g_config.last_citra_room_name, 0, 0, NULL, NULL, false, ERR_XP},
    ON_OFF("Multiplayer", "SyncSmallMonsters", sync_small_monsters, true),
    ON_OFF("Multiplayer", "FullHubList", hub_full_list, false),
    {"Monster HP", "1Player|1Players", SET_FACTOR, F(hp.scale[0]), 0, HP_SCALE_MIN, HP_SCALE_MAX, NULL, NULL, true, ERR_HP},
    {"Monster HP", "2Players|2Player", SET_FACTOR, F(hp.scale[1]), 0, HP_SCALE_MIN, HP_SCALE_MAX, NULL, NULL, true, ERR_HP},
    {"Monster HP", "3Players|3Player", SET_FACTOR, F(hp.scale[2]), 0, HP_SCALE_MIN, HP_SCALE_MAX, NULL, NULL, true, ERR_HP},
    {"Monster HP", "4Players|4Player", SET_FACTOR, F(hp.scale[3]), 0, HP_SCALE_MIN, HP_SCALE_MAX, NULL, NULL, true, ERR_HP},
    ON_OFF("Village Quests", "VillageQuestsInHub", village_in_hub, true),
    {"Village Quests", "HubHP", SET_FACTOR, F(hp.village), 0, HP_SCALE_MIN, HP_SCALE_MAX, NULL, NULL, true, ERR_VILLAGE},
    ON_OFF("Discord", "RichPresence", discord, false),
    ON_OFF("Discord", "ShowHunter", discord_hunter, false),
    ON_OFF("Bonus Content", "BonusContent", bonus_content, true),
    {"Audio", "Volume", SET_INT, F(volume), 0, 0, 100, NULL, NULL, false, ERR_MISC},
    ON_OFF("Audio", "MuteInBackground", mute_in_background, false),
    ON_OFF("Audio", "Reverb", reverb, false),
    ON_OFF("Application", "PauseInBackground", pause_background, false),
    ON_OFF("Application", "ConfirmExit", confirm_exit, false),
    {"Application", "Language", SET_CHOICE, F(language), 0, 0, 0, "English|en,Spanish|es|Español,Chinese|zh|中文|Simplified Chinese,Auto|System|Phone", NULL, false, ERR_LANGUAGE},
    {"Application", "ButtonIcons", SET_CHOICE, F(button_icons), 0, 0, 0, "Xbox,Switch,PlayStation", NULL, false, ERR_ICONS},
    {"Application", "Theme", SET_CHOICE, F(menu_theme), 0, 0, 0, "Gold,Blue,Hunter", NULL, false, ERR_MISC},
    {"Application", "MenuScale", SET_INT, F(menu_scale), 0, 50, 200, NULL, NULL, false, ERR_SCALE},
    ON_OFF("Application", "ThanksShown", thanks_shown, false),  // (written as the thank-you closes: settings_panel.cpp)
    {"Application", "Updates", SET_CHOICE, F(updates), 0, 0, 0, "Ask,Auto|automatic,Off|never", NULL, false, ERR_UPDATES},
#ifndef __ANDROID__  // (a phone's settings button is always on the screen: no notice, no key)
    ON_OFF("Application", "MenuHintSeen", menu_hint_seen, false),
#endif
};
#undef ON_OFF
#undef KEYS
const int g_nsettings = (int)(sizeof g_settings / sizeof g_settings[0]);

const ConfigSetting *config_bind_setting(int bind) {
    for (int i = 0; i < g_nsettings; i++)
        if (g_settings[i].kind == SET_KEYS && g_settings[i].field == F(binds[bind])) return &g_settings[i];
    return NULL;
}
#undef F

// [Keybinds] names: SDL's for a key, but its own for the ones whose name has a ',' (the list's separator) or a
// ';' / '#' (a comment in the file), and for the mouse buttons and wheel
static const struct { int code; const char *name; } key_names[] = {
    {SDL_SCANCODE_COMMA, "Comma"}, {SDL_SCANCODE_KP_COMMA, "Keypad Comma"}, {SDL_SCANCODE_SEMICOLON, "Semicolon"},
    {SDL_SCANCODE_KP_HASH, "Keypad Hash"}, {KEY_MOUSE + SDL_BUTTON_LEFT, "Mouse Left"},
    {KEY_MOUSE + SDL_BUTTON_RIGHT, "Mouse Right"}, {KEY_MOUSE + SDL_BUTTON_MIDDLE, "Mouse Middle"},
    {KEY_MOUSE + SDL_BUTTON_X1, "Mouse 4"}, {KEY_MOUSE + SDL_BUTTON_X2, "Mouse 5"}, {KEY_WHEEL_UP, "Wheel Up"},
    {KEY_WHEEL_DOWN, "Wheel Down"},
};
const char *config_key_name(int code) {
    for (size_t i = 0; i < sizeof key_names / sizeof *key_names; i++)
        if (key_names[i].code == code) return key_names[i].name;
    const char *n = code > 0 && code < SDL_SCANCODE_COUNT ? SDL_GetScancodeName((SDL_Scancode)code) : "";
    return n && !strpbrk(n, ",;#") ? n : "";
}
int config_key_code(const char *name) {
    if (!*name) return 0;
    for (size_t i = 0; i < sizeof key_names / sizeof *key_names; i++)
        if (!strcasecmp(key_names[i].name, name)) return key_names[i].code;
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    return sc != SDL_SCANCODE_UNKNOWN && *config_key_name((int)sc) ? (int)sc : 0;
}
// One of a [Keybinds] row's keys: a key or button with a name, not one the recomp keeps for itself (keymap.h)
static bool bind_key_valid(int code) { return !code || (*config_key_name(code) && !key_reserved(code)); }

// Is name (squashed) one of the '|'-separated spellings in list (from list, up to its end or a ',')? With *index, the
// number of the ','-separated item it matched.
static bool spelled(const char *list, const char *name, int *index) {
    int item = 0;
    for (const char *p = list; *p;) {
        size_t n = strcspn(p, "|,");
        char one[64];
        snprintf(one, sizeof one, "%.*s", (int)n, p);
        squash(one);
        size_t len = strlen(one);
        bool prefix = len && one[len - 1] == '*';
        if (prefix ? !strncmp(name, one, len - 1) : !strcmp(name, one)) {
            if (index) *index = item;
            return true;
        }
        p += n;
        if (*p == ',') item++;
        if (*p) p++;
    }
    return false;
}

const ConfigSetting *config_setting(const char *key) {
    char k[64];
    snprintf(k, sizeof k, "%s", key);
    squash(k);
    for (int i = 0; i < g_nsettings; i++)
        if (spelled(g_settings[i].key, k, NULL)) return &g_settings[i];
    return NULL;
}

// The first spelling of a choice (as the file writes it)
static void choice_name(const char *list, int index, char *buf, size_t size) {
    const char *p = list;
    for (int i = 0; i < index && p; i++) if ((p = strchr(p, ','))) p++;
    if (!p) p = list;
    snprintf(buf, size, "%.*s", (int)strcspn(p, "|,"), p);
}

static int *field_int(const Config *c, size_t at) { return (int *)((char *)c + at); }
static float *field_float(const ConfigSetting *s, const Config *c) { return (float *)((char *)c + s->field); }
static int choice_value(const ConfigSetting *s, int index) { return s->values ? s->values[index] : index; }
static int nchoices(const ConfigSetting *s) {
    int n = 1;
    for (const char *p = s->choices; *p; p++) n += *p == ',';
    return n;
}

static bool parse_bool(const char *v, int *out) {
    if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) *out = 1;
    else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) *out = 0;
    else return false;
    return true;
}

bool config_parse(const ConfigSetting *s, const char *value, Config *c) {
    char v[256], *end;
    snprintf(v, sizeof v, "%s", value);
    squash(v);
    int index;
    float f[4];
    switch (s->kind) {
    case SET_BOOL: return parse_bool(v, field_int(c, s->field));
    case SET_INT: {
#if FPS_MAX == FPS_UNLIMITED  // (a desktop's FPS = Unlimited, as config_format writes it)
        if (s->field == offsetof(Config, fps) && !strcasecmp(v, "Unlimited")) {
            *field_int(c, s->field) = FPS_UNLIMITED;
            return true;
        }
#endif
        double d = strtod(v, &end);
        if (end == v || *end || !(d >= s->min && d <= s->max)) return false;
        *field_int(c, s->field) = (int)d;
        return true;
    }
    case SET_FLOAT: {
        float x = strtof(v, &end);
        if (end == v || *end || !(x >= s->min && x <= s->max)) return false;
        *field_float(s, c) = x;
        return true;
    }
    case SET_FACTOR:
        if (parse_factors(v, f) != 1) return false;
        *field_float(s, c) = f[0];
        return true;
    case SET_CHOICE:
        if (!spelled(s->choices, v, &index)) return false;
        *field_int(c, s->field) = choice_value(s, index);
        return true;
    case SET_SIZE:
        if (spelled(s->choices, v, NULL)) {
            *field_int(c, s->field) = *field_int(c, s->field2) = 0;
            return true;
        }
        return parse_size(v, field_int(c, s->field), field_int(c, s->field2));
    case SET_ASPECT: return parse_aspect(v, field_float(s, c));
    case SET_TEXT:  // (as written: host names keep their dashes)
        snprintf((char *)c + s->field, s->field2, "%s", value);
        return true;
    case SET_KEYS: {  // names separated by ',' ("Return, Keypad Enter"), or None
        int keys[BIND_KEYS] = {0}, n = 0;
        if (strcmp(v, "none") && *v)
            for (const char *p = value;; p++) {
                size_t len = strcspn(p, ",");
                char one[64];
                snprintf(one, sizeof one, "%.*s", (int)len, p);
                char *a = one, *b = one + strlen(one);
                while (isspace((unsigned char)*a)) a++;
                while (b > a && isspace((unsigned char)b[-1])) *--b = 0;
                int code = config_key_code(a);
                if (!code || !bind_key_valid(code) || n == BIND_KEYS) return false;
                if (!n || keys[0] != code) keys[n++] = code;
                p += len;
                if (!*p) break;
            }
        memcpy(field_int(c, s->field), keys, sizeof keys);
        return true;
    }
    }
    return false;
}

void config_format(const ConfigSetting *s, const Config *c, char *buf, size_t size) {
    int iv = s->kind == SET_FLOAT || s->kind == SET_FACTOR || s->kind == SET_ASPECT || s->kind == SET_TEXT
                 ? 0 : *field_int(c, s->field);
    float fv = s->kind == SET_FLOAT || s->kind == SET_FACTOR || s->kind == SET_ASPECT ? *field_float(s, c) : 0;
    switch (s->kind) {
    case SET_BOOL: snprintf(buf, size, "%s", iv ? "On" : "Off"); break;
    case SET_INT:
#if FPS_MAX == FPS_UNLIMITED
        if (s->field == offsetof(Config, fps) && iv >= FPS_UNLIMITED) {
            snprintf(buf, size, "Unlimited");
            break;
        }
#endif
        snprintf(buf, size, "%d", iv);
        break;
    case SET_FLOAT:
    case SET_FACTOR: snprintf(buf, size, "%g", (double)fv); break;
    case SET_CHOICE:
        for (int i = 0, n = nchoices(s); i < n; i++)
            if (choice_value(s, i) == iv) { choice_name(s->choices, i, buf, size); return; }
        choice_name(s->choices, 0, buf, size);
        break;
    case SET_SIZE:
        if (iv) snprintf(buf, size, "%dx%d", iv, *field_int(c, s->field2));
        else choice_name(s->choices, 0, buf, size);
        break;
    case SET_ASPECT:
        if (fv == 0) snprintf(buf, size, "Auto");
        else snprintf(buf, size, "%g", (double)fv);
        break;
    case SET_TEXT: snprintf(buf, size, "%s", (const char *)c + s->field); break;
    case SET_KEYS: {
        const int *k = field_int(c, s->field);
        size_t n = 0;
        if (size) *buf = 0;
        for (int i = 0; i < BIND_KEYS; i++)
            if (k[i] && n < size) n += (size_t)snprintf(buf + n, size - n, "%s%s", n ? ", " : "", config_key_name(k[i]));
        if (!n) snprintf(buf, size, "None");
        break;
    }
    }
}

bool config_valid(const ConfigSetting *s, const Config *c) {
    switch (s->kind) {
    case SET_BOOL: {
        int v = *field_int(c, s->field);
        return v == 0 || v == 1;
    }
    case SET_INT: {
        int v = *field_int(c, s->field);
        return v >= s->min && v <= s->max;
    }
    case SET_FLOAT:
    case SET_FACTOR: {
        float v = *field_float(s, c);
        return isfinite(v) && v >= s->min && v <= s->max;
    }
    case SET_CHOICE:
        for (int i = 0, n = nchoices(s); i < n; i++)
            if (choice_value(s, i) == *field_int(c, s->field)) return true;
        return false;
    case SET_SIZE: {
        int w = *field_int(c, s->field), h = *field_int(c, s->field2);
        return (!w && !h) || (w >= 320 && w <= 16384 && h >= 180 && h <= 16384);
    }
    case SET_ASPECT: {
        float v = *field_float(s, c);
        return isfinite(v) && (v == 0 || (v >= s->min && v <= s->max));
    }
    case SET_TEXT: {  // (one line, and no comment character: the file would read it back cut short)
        const char *t = (const char *)c + s->field;
        return memchr(t, 0, s->field2) && !strpbrk(t, "\r\n;#");
    }
    case SET_KEYS: {
        const int *k = field_int(c, s->field);
        for (int i = 0; i < BIND_KEYS; i++)
            if (!bind_key_valid(k[i])) return false;
        return true;
    }
    }
    return false;
}

bool config_equal(const ConfigSetting *s, const Config *a, const Config *b) {
    switch (s->kind) {
    case SET_FLOAT:
    case SET_FACTOR:
    case SET_ASPECT: return *field_float(s, a) == *field_float(s, b);
    case SET_SIZE:
        return *field_int(a, s->field) == *field_int(b, s->field) &&
               *field_int(a, s->field2) == *field_int(b, s->field2);
    case SET_TEXT: return !strcmp((const char *)a + s->field, (const char *)b + s->field);
    case SET_KEYS: return !memcmp(field_int(a, s->field), field_int(b, s->field), sizeof(int) * BIND_KEYS);
    default: return *field_int(a, s->field) == *field_int(b, s->field);
    }
}

bool config_restart_key(const char *key) {
    const ConfigSetting *s = config_setting(key);
    return s && s->restart;
}

bool config_range(const char *key, float *min, float *max) {
    const ConfigSetting *s = config_setting(key);
    if (!s || (s->kind != SET_INT && s->kind != SET_FLOAT && s->kind != SET_FACTOR && s->kind != SET_ASPECT)) return false;
    *min = s->min, *max = s->max;
    return true;
}

// What DEFAULT_INI says of a setting: the comment lines above its key (keys right below one another, 1Player..4Players,
// share them). A line wrapped in the file is joined to the one before (that one long, not ending a sentence);
// "Default: x" goes last, left out when x is empty.
bool config_help(const char *key, char *buf, size_t size) {
    const char *p = DEFAULT_INI, *block = NULL;  // block: the comment lines that go with the next key
    bool after_key = false;
    size_t klen = strlen(key);
    while (*p) {
        const char *end = strchr(p, '\n');
        if (!end) end = p + strlen(p);
        if (*p == ';') {
            if (!block || after_key) block = p;
            after_key = false;
        } else if (p == end || *p == '[') {
            block = NULL, after_key = false;
        } else {
            after_key = true;
            if (block && (size_t)(end - p) > klen && !strncmp(p, key, klen) && (p[klen] == ' ' || p[klen] == '=')) break;
        }
        p = *end ? end + 1 : end;
    }
    if (!*p || !block || !size) return false;
    size_t n = 0;
    char dflt[128] = "", prev[256] = "";
    *buf = 0;
    for (const char *l = block; *l == ';';) {
        const char *end = strchr(l, '\n');
        char line[256];
        snprintf(line, sizeof line, "%.*s", (int)(end - l - 1), l + 1);
        const char *t = line + (*line == ' ');
        if (!strncmp(t, "Default:", 8)) {
            snprintf(dflt, sizeof dflt, "%s", t + 8 + (t[8] == ' '));
        } else {
            size_t pl = strlen(prev);
            bool list = strchr(t, ',') != NULL;  // (the values it takes, "Off, 2x, 4x": a line of its own)
            for (const char *c = t; *c && list; c++) list = *c != ' ' || c[-1] == ',';
            bool join = pl >= 80 && !strchr(".!?:)", prev[pl - 1]) && strncmp(t, "Between ", 8) && !list;
            if (n) n += (size_t)snprintf(buf + n, n < size ? size - n : 0, "%s", join ? " " : "\n");
            n += (size_t)snprintf(buf + n, n < size ? size - n : 0, "%s", t);
            snprintf(prev, sizeof prev, "%s", line);
        }
        l = end + 1;
    }
    if (*dflt) n += (size_t)snprintf(buf + n, n < size ? size - n : 0, "%sDefault: %s", n ? "\n" : "", dflt);
    return n > 0;
}

static int desktop_w, desktop_h;
static bool render_auto = true;
static bool g_shadows_set, g_bloom_set;  // (the file says Shadows / Bloom: an older one doesn't, and LowEnd then takes them off)

static void set_key(const char *key, char *val, int line) {
    char k[64], v[256];
    snprintf(k, sizeof k, "%s", key);
    snprintf(v, sizeof v, "%s", val);
    squash(k);
    squash(v);
    if (!strncmp(k, "quest", 5) && isdigit((unsigned char)k[5])) {  // [Monster HP] Quest<number> = factors
        HpScale *h = &g_config.hp;
        char *end;
        unsigned long q = strtoul(k + 5, &end, 10);
        float f[4];
        int n = parse_factors(v, f);
        if (*end || !q || (n != 1 && n != 4)) {
            LOG("[config] line %d: \"%s\" is not a value %s takes; using the default\n", line, val, key);
            return;
        }
        int i = 0;
        while (i < h->quests && h->quest[i].quest != q) i++;
        if (i == HP_QUESTS_MAX) { LOG("[config] line %d: more than %d quests set apart\n", line, HP_QUESTS_MAX); return; }
        h->quest[i].quest = (uint32_t)q;
        for (int j = 0; j < 4; j++) h->quest[i].scale[j] = f[n == 4 ? j : 0];
        if (i == h->quests) h->quests++;
        return;
    }
    const ConfigSetting *s = config_setting(key);
    if (!s) { LOG("[config] line %d: unknown setting \"%s\" (ignored)\n", line, key); return; }
    if (s->field == offsetof(Config, shadows)) g_shadows_set = true;
    if (s->field == offsetof(Config, bloom)) g_bloom_set = true;
    if (!config_parse(s, val, &g_config))
        LOG("[config] line %d: \"%s\" is not a value %s takes; using the default\n", line, val, key);
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

// The largest size inside w x h matching the given aspect ratio, even numbers.
static void fit_aspect(int w, int h, float aspect, int *rw, int *rh) {
    if (aspect <= 0.0f) aspect = 16.0f / 9.0f;
    if ((float)w / (float)h > aspect)
        w = (int)((float)h * aspect + 0.5f);
    else
        h = (int)((float)w / aspect + 0.5f);
    *rw = w & ~1, *rh = h & ~1;
}

#ifndef __ANDROID__
// (a desktop) What the menu's Low-end mode (1) or Toaster (LOW_END_TOASTER) sets RenderScale to: the 3D picture about
// LOW_END_LINES or TOASTER_LINES tall whatever the screen - Low-end 66% at 1080p, 50% at 1440p, 33% at 4K, 93% at 768p;
// Toaster 50%, 37%, 25%, 70% - never under 33% (Toaster: 25%) nor over 100%. Of the height config_load fits the 3D
// picture to: Resolution's (Desktop: the desktop's; Borderless at most the desktop's), in the picture's shape.
int config_lowend_scale(const Config *c, int mode) {
    int dw = desktop_w > 0 ? desktop_w : 1920, dh = desktop_h > 0 ? desktop_h : 1080;
    bool own = c->width > 0 && c->height > 0;
    int tw = own ? c->width : dw, th = own ? c->height : dh;
    if (c->display_mode == DISPLAY_BORDERLESS) tw = tw < dw ? tw : dw, th = th < dh ? th : dh;
    float aspect = c->aspect_ratio > 0 ? c->aspect_ratio : (float)tw / (float)th;
    if (aspect < 1.6f - 0.005f) aspect = 1.6f;  // (config_load's narrowest on a desktop)
    if (fabsf(aspect - 16.0f / 9.0f) < 0.01f) aspect = 16.0f / 9.0f;
    int w, h;
    fit_aspect(tw, th, aspect, &w, &h);
    bool toaster = mode == LOW_END_TOASTER;
    int least = toaster ? 25 : 33, s = h > 0 ? (toaster ? TOASTER_LINES : LOW_END_LINES) * 100 / h : 100;
    return s < least ? least : s > 100 ? 100 : s;
}
#endif

// Sections of DEFAULT_INI the file lacks (written by an older build) are added with their defaults at its end; the
// player's own settings stay as they are.
static void append_missing_sections(const char *path, const char *seen) {
    FILE *o = NULL;
    for (const char *p = strstr(DEFAULT_INI, "\n["); p;) {
        const char *start = p + 1, *end = strstr(start, "\n["), *close = strchr(start, ']');
        p = end;
        char name[64], key[70];
        snprintf(name, sizeof name, "%.*s", (int)(close - start - 1), start + 1);
        for (char *q = name; *q; q++) *q = (char)tolower((unsigned char)*q);
        snprintf(key, sizeof key, "|%s|", name);
        if (strstr(seen, key)) continue;
        if (!o && !(o = fopen(path, "a"))) return;
        fprintf(o, "\n%.*s", (int)(end ? end - start : (long)strlen(start)), start);
        LOG("[config] added the [%s] section to %s\n", name, path);
    }
    if (o) fclose(o);
}

void config_load(void) {
    char path[1100];
    config_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) {
        FILE *o = fopen(path, "w");
        const char *ini = DEFAULT_INI;  // (Language = Auto: the menu follows the phone's language from the start)
        if (o) {
            fputs(ini, o);
            fclose(o);
            LOG("[config] wrote the default settings to %s\n", path);
        } else {
            LOG("[config] %s is missing and could not be written; using the defaults\n", path);
        }
    } else {
        char buf[512], seen[512] = "|";
        int line = 0;
        while (fgets(buf, sizeof buf, f)) {
            line++;
            char *s = trim(buf);
            if (*s == '[' && strchr(s, ']') && strlen(seen) + strlen(s) + 2 < sizeof seen) {
                for (char *q = s + 1; *q != ']'; q++) {
                    size_t n = strlen(seen);
                    seen[n] = (char)tolower((unsigned char)*q), seen[n + 1] = 0;
                }
                strcat(seen, "|");
            }
            if (!*s || *s == ';' || *s == '#' || *s == '[') continue;
            char *eq = strchr(s, '=');
            if (!eq) { LOG("[config] line %d: no '=' (ignored)\n", line); continue; }
            *eq = 0;
            char *val = trim(eq + 1), *cmt = strpbrk(val, ";#");
            if (cmt) *cmt = 0;
            set_key(trim(s), trim(val), line);
        }
        fclose(f);
        append_missing_sections(path, seen);
    }

    // (a file of an older version, with LowEnd on and no Shadows: off, as the menu's Low-end mode now sets them; a
    // desktop's Low-end mode keeps the shadows, and only Toaster turns them off)
#ifdef __ANDROID__
    if (f && !g_shadows_set && g_config.low_end) g_config.shadows = 0;
#else
    if (f && !g_shadows_set && g_config.low_end == LOW_END_TOASTER) g_config.shadows = 0;
#endif
    if (f && !g_bloom_set && g_config.low_end) g_config.bloom = 0;  // (LowEnd alone left the bloom out up to 0.1.6)
    // Keep Auto/Desktop and file values, rather than persisting temporary environment overrides.
    render_auto = !g_config.render_w;
    config_store_loaded(&g_config);
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    desktop_w = dm ? dm->w : 1920, desktop_h = dm ? dm->h : 1080;
#ifdef __ANDROID__
    // (the game is always landscape, but a phone that is locked or still upright says otherwise: the 3D picture and the
    // HUD would be laid out for that shape, narrow, with black at the sides, until the next start)
    if (desktop_h > desktop_w) {
        int upright = desktop_w;
        desktop_w = desktop_h, desktop_h = upright;
    }
#endif
    if (!g_config.width) g_config.width = desktop_w, g_config.height = desktop_h;

    const char *e = getenv("MHGU_WINDOW");
    int w, h;
    if (e && parse_size(e, &w, &h)) g_config.display_mode = DISPLAY_WINDOWED, g_config.width = w, g_config.height = h;
    if (getenv("MHGU_HIDDEN")) g_config.display_mode = DISPLAY_WINDOWED;
    if ((e = getenv("MHGU_MSAA"))) g_config.msaa = atoi(e) >= 8 ? 8 : atoi(e) >= 4 ? 4 : atoi(e) >= 2 ? 2 : 1;
    if ((e = getenv("MHGU_FXAA"))) g_config.fxaa = atoi(e) != 0;
    if ((e = getenv("MHGU_RT16"))) g_config.smooth_gradients = atoi(e) != 0;
    if ((e = getenv("MHGU_BACKGROUND_INPUT"))) g_config.background_input = atoi(e) != 0;
    if ((e = getenv("MHGU_PAD_POSITIONAL"))) g_config.face_buttons = atoi(e) ? FACE_POSITION : FACE_LABEL;
    if ((e = getenv("MHGU_LDN_PORT")) && atoi(e) > 0) g_config.ldn_port = atoi(e);
    if ((e = getenv("MHGU_SYNC_ZAKO"))) g_config.sync_small_monsters = atoi(e) != 0;
    if ((e = getenv("MHGU_LDN_JOIN"))) snprintf(g_config.ldn_join, sizeof g_config.ldn_join, "%s", e);
    if ((e = getenv("MHGU_LDN_SERVER"))) snprintf(g_config.ldn_server, sizeof g_config.ldn_server, "%s", e);
    if ((e = getenv("MHGU_LDN_KEY"))) snprintf(g_config.ldn_key, sizeof g_config.ldn_key, "%s", e);
    static const struct { const char *env, *key; } crossplay_env[] = {
        {"MHGU_CROSSPLAY", "Crossplay"}, {"MHGU_RYU_SERVERS", "RyuServers"}, {"MHGU_RYU_HOST", "RyuHost"},
        {"MHGU_RYU_PORT", "RyuPort"}, {"MHGU_RYU_PASSWORD", "RyuPassword"}, {"MHGU_ROOM_SERVER", "RoomServer"},
        {"MHGU_ROOM_HOST", "RoomHost"}, {"MHGU_ROOM_PORT", "RoomPort"}, {"MHGU_ROOM_NAME", "RoomName"},
        {"MHGU_ROOM_PASSWORD", "RoomPassword"}, {"MHGU_WEB_API", "WebApiUrl"},
        {"MHGU_CITRA_ROOM", "CitraRoomServer"}, {"MHGU_CITRA_PASSWORD", "CitraRoomPassword"}, {"MHGU_CITRA_WEB_API", "CitraWebApiUrl"},
        {"MHGU_CITRA_WEB_API_2", "CitraWebApiUrl2"}, {"MHGU_CITRA_TOKEN", "CitraToken"}
    };
    for (unsigned i = 0; i < sizeof crossplay_env / sizeof *crossplay_env; ++i) {
        const ConfigSetting *setting = config_setting(crossplay_env[i].key);
        const char *value = getenv(crossplay_env[i].env);
        if (!setting || !value) continue;
        if (setting->kind == SET_BOOL && (!strcmp(value, "0") || !strcmp(value, "1"))) value = *value == '0' ? "Off" : "On";
        if (!strcmp(crossplay_env[i].key, "Crossplay") && strlen(value) == 1 && *value >= '0' && *value <= '5') {
            static const char *const modes[] = {"Off", "Ryujinx", "Yuzu Room", "Mixed", "Citra Room", "All Rooms"};
            value = modes[*value - '0'];
        }
        if (setting->kind == SET_TEXT && strlen(value) >= setting->field2) continue;
        Config candidate = g_config;
        if (config_parse(setting, value, &candidate) && config_valid(setting, &candidate)) g_config = candidate;
    }
    if ((e = getenv("MHGU_ASPECT"))) parse_aspect(e, &g_config.aspect_ratio);
    if ((e = getenv("MHGU_FOV")) && strtof(e, NULL) >= 0.5f && strtof(e, NULL) <= 2.0f) g_config.fov = strtof(e, NULL);
    if ((e = getenv("MHGU_UI_ANCHOR"))) config_parse(config_setting("UIAnchor"), e, &g_config);
    e = getenv("MHGU_RENDER");
    if (e && parse_size(e, &w, &h)) render_auto = false, g_config.render_w = w, g_config.render_h = h;
    // (test runs, the device's own settings left as they are: MHGU_LOWEND=0|1|2 (Toaster), MHGU_SCALE=<percent>,
    // MHGU_SHADOWS=0|1, MHGU_BLOOM=0|1, MHGU_FPS_LIMIT=<fps>; not saved)
    if ((e = getenv("MHGU_LOWEND"))) g_config.low_end = atoi(e) < 0 ? 0 : atoi(e) > 2 ? 2 : atoi(e);
    if ((e = getenv("MHGU_BLOOM"))) g_config.bloom = atoi(e) != 0, config_live_test(&g_config);
    if ((e = getenv("MHGU_FPS_LIMIT")) && atoi(e) >= 30 && atoi(e) <= FPS_MAX) g_config.fps = atoi(e), config_live_test(&g_config);
    if ((e = getenv("MHGU_SCALE")) && atoi(e) >= 10 && atoi(e) <= 100) g_config.render_scale = atoi(e);
    if ((e = getenv("MHGU_SHADOWS"))) g_config.shadows = atoi(e) != 0, config_live_test(&g_config);
    if ((e = getenv("MHGU_FSR"))) g_config.fsr = atoi(e) != 0;  // (test runs: MHGU_FSR=0|1, MHGU_FSR_QUALITY=quality...)
#ifdef __ANDROID__
    if (g_config.low_end) g_config.fsr = 0, g_config.fxaa = 0, g_config.msaa = 1;  // (none of them on a weak GPU)
#else  // (a desktop's Low-end mode keeps FXAA, one pass over a small picture: only Toaster turns it off too)
    if (g_config.low_end) g_config.fsr = 0, g_config.msaa = 1;
    if (g_config.low_end == LOW_END_TOASTER) g_config.fxaa = 0;
#endif
    if ((e = getenv("MHGU_FSR_UPSCALER")))  // (4 or fsr4, xess, anything else FSR 3.1)
        g_config.fsr_upscaler = !strcmp(e, "4") || !strcasecmp(e, "fsr4") ? FSR_UP_4 : !strncasecmp(e, "xess", 4) ? FSR_UP_XESS : FSR_UP_3;
    if ((e = getenv("MHGU_FSR_QUALITY"))) config_parse(config_setting("FSRQuality"), e, &g_config);
    if ((e = getenv("MHGU_FSR_OBJMOTION"))) g_config.fsr_objmotion = atoi(e) != 0;
    if ((e = getenv("MHGU_FSR_ANIMSURF"))) g_config.fsr_animsurf = atoi(e) != 0;
    if ((e = getenv("MHGU_FSR_SHARPNESS")) && strtof(e, NULL) >= 0.0f && strtof(e, NULL) <= 1.0f) g_config.fsr_sharpness = strtof(e, NULL);
    if ((e = getenv("MHGU_FSR_REACTIVE")) && strtof(e, NULL) >= 0.0f && strtof(e, NULL) <= 1.0f) g_config.fsr_reactive = strtof(e, NULL);

    int tw = g_config.width, th = g_config.height;  // (Borderless shows the image at the desktop size)
    if (g_config.display_mode == DISPLAY_BORDERLESS) tw = tw < desktop_w ? tw : desktop_w, th = th < desktop_h ? th : desktop_h;
    // The picture's shape: the window's or AspectRatio's. The game lays its HUD out for 16:9; on a narrower picture it keeps
    // that shape, smaller, its parts at the top and bottom edges (nvn/vulkan/exec.c). Android: down to 5:4, a console's 4:3
    // screen filled. Wider than 16:9 shows more to the sides, narrower more above and below (camera_fovy, gen.py PATCHES).
    float aspect = g_config.aspect_ratio > 0 ? g_config.aspect_ratio : (float)tw / (float)th;
#ifdef __ANDROID__
    const float narrowest = 1.25f;
#else
    const float narrowest = 1.6f;
#endif
    if (aspect < narrowest - 0.005f) {
        if (g_config.aspect_ratio > 0)
            LOG("[config] AspectRatio %.3f is narrower than the HUD allows: using %.2f\n", aspect, narrowest);
        aspect = narrowest;
    }
    if (fabsf(aspect - 16.0f / 9.0f) < 0.01f) aspect = 16.0f / 9.0f;  // (1366x768 and the like)
    if (render_auto && g_config.fsr && g_config.display_mode == DISPLAY_BORDERLESS && (tw < desktop_w || th < desktop_h)) {
        // FSR, Borderless: it upscales straight to the size the picture is shown at (the screen, in the picture's shape)
        // instead of to Resolution, which the window would then stretch again (blurring what FSR rebuilt)
        fit_aspect(desktop_w, desktop_h, aspect, &w, &h);
        LOG("[config] FSR, Borderless: upscaling to %dx%d (the screen, in the picture's shape) instead of Resolution %dx%d\n",
            w, h, tw, th);
    } else if (render_auto) {
        // RenderScale: a smaller 3D picture (phones), stretched to the screen; not with FSR (FSRQuality sets that)
        int s = g_config.fsr ? 100 : g_config.render_scale;
        int least = 32000 / tw + 1 > 18000 / th + 1 ? 32000 / tw + 1 : 18000 / th + 1;  // (320x180)
        if (s < 100 && s < least) {
            s = least < 100 ? least : 100;
            LOG("[config] RenderScale %d%% would be below 320x180: using %d%%\n", g_config.render_scale, s);
        }
        fit_aspect(tw * s / 100, th * s / 100, aspect, &w, &h);
        if (s < 100) LOG("[config] RenderScale %d%%: the 3D picture at %dx%d (screen %dx%d)\n", s, w, h, tw, th);
#ifdef __ANDROID__
        if (g_config.low_end && !g_config.fsr && s < 100) {
            // (LowEnd: the HUD stays sharp over the smaller 3D picture, fsr.c vk_fsr_init: at the screen's size up to
            // 720 lines or twice the 3D's, whichever is more. At a 2712x1220 screen's whole size it drew 9 times the
            // pixels of the 3D at 33%, and a phone with an Adreno 710 was slower with LowEnd than without)
            // (Toaster: at most TOASTER_HUD_LINES, for the HUD, the game's last draw and the window's copy)
            int ow, oh, cap = g_config.low_end == LOW_END_TOASTER ? TOASTER_HUD_LINES : h * 2 > 720 ? h * 2 : 720;
            if (th > cap) fit_aspect(tw * cap / th, cap, aspect, &ow, &oh);
            else fit_aspect(tw, th, aspect, &ow, &oh);
            g_fsr_output[0] = (uint32_t)ow, g_fsr_output[1] = (uint32_t)oh;
        }
#else
        // (a desktop, any RenderScale below 100 with or without Low-end mode: the HUD and the text stay sharp over the
        // smaller 3D picture, drawn at the screen's whole size (fsr.c's plain scaling, vk_fsr_init). A PC's GPU affords
        // that size, where a phone's took the caps above; without it the whole frame, HUD and letters too, was drawn
        // small and stretched)
        // (Borderless with a Resolution smaller than the screen: at the size the picture is shown at, the screen in the
        // picture's shape, as FSR's above, instead of at Resolution's for the window to stretch again; the 3D picture
        // stays RenderScale of Resolution's)
        int ow, oh;
        bool screen = g_config.display_mode == DISPLAY_BORDERLESS && (tw < desktop_w || th < desktop_h);
        fit_aspect(screen ? desktop_w : tw, screen ? desktop_h : th, aspect, &ow, &oh);
        if (!g_config.fsr && s < 100 && (ow != w || oh != h)) {
            g_fsr_output[0] = (uint32_t)ow, g_fsr_output[1] = (uint32_t)oh;
            LOG("[config] the HUD at %dx%d over the 3D picture%s\n", ow, oh,
                screen ? " (Borderless: the screen, in the picture's shape, instead of Resolution)" : "");
        }
#endif
    } else {
        fit_aspect(g_config.render_w, g_config.render_h, aspect, &w, &h);
        if (w != g_config.render_w || h != g_config.render_h)
            LOG("[config] RenderResolution %dx%d is not the picture's shape (%.3f): using %dx%d\n", g_config.render_w,
                g_config.render_h, aspect, w, h);
    }
    if (render_auto && g_config.fsr && g_config.render_scale < 100)
        LOG("[config] RenderScale %d%% not used: FSR is on (FSRQuality sets the 3D size)\n", g_config.render_scale);
    if (g_config.fsr) {  // FSR: that size is its output; the game renders smaller (runtime/nvn/vulkan/fsr.c)
        static const float ratio[] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
        static const char *const names[] = {"NativeAA", "Quality", "Balanced", "Performance", "UltraPerformance"};
        g_fsr_output[0] = (uint32_t)w, g_fsr_output[1] = (uint32_t)h;
        float r = ratio[g_config.fsr_quality];
        fit_aspect((int)(w / r + 0.5f), (int)(h / r + 0.5f), aspect, &w, &h);
#ifdef _WIN32
        if (g_config.fsr_upscaler == FSR_UP_XESS) {
            // XeSS-SR: output / render never above the preset's ratio, where XeSS's input range ends for Ultra
            // Performance (x3.0; nvn/vulkan/xess.c). fit_aspect rounds down to even sizes, which can go past it (1600x900
            // Quality: 1066 wide, x1.5009): the next even size at or above output / ratio instead
            int mw = (int)ceil(g_fsr_output[0] / (double)r), mh = (int)ceil(g_fsr_output[1] / (double)r);
            if (w < mw) w = (mw + 1) & ~1;
            if (h < mh) h = (mh + 1) & ~1;
            LOG("[config] XeSS-SR: the 3D picture at %dx%d, x%.4f / x%.4f of %ux%u (at most x%.1f)\n", w, h,
                (double)g_fsr_output[0] / w, (double)g_fsr_output[1] / h, g_fsr_output[0], g_fsr_output[1], r);
        }
#endif
        if (g_config.msaa > 1) LOG("[config] FSR is on: MSAA off (FSR does the anti-aliasing)\n");
        if (g_config.fxaa) LOG("[config] FSR is on: FXAA off (FSR does the anti-aliasing)\n");
        g_config.msaa = 1, g_config.fxaa = 0;
        // textures are sampled for the output size (AMD's recommendation: log2(render / output)); MHGU_FSR_LODBIAS
        // overrides (A/B)
        g_fsr_lod_bias = log2f((float)w / (float)g_fsr_output[0]);
        if ((e = getenv("MHGU_FSR_LODBIAS"))) g_fsr_lod_bias = strtof(e, NULL);
        const char *up = g_config.fsr_upscaler == FSR_UP_4      ? "FSR 4 (3.1 if it cannot start)"
                       : g_config.fsr_upscaler == FSR_UP_XESS ? "XeSS-SR (FSR 3.1 if it cannot start)"
                                                              : "FSR 3.1";
        LOG("[config] %s %s (x%.1f): the 3D picture at %dx%d, upscaled to %ux%u, sharpness %.2f, texture LOD bias "
            "%.2f\n", up, names[g_config.fsr_quality], r, w, h, g_fsr_output[0], g_fsr_output[1], g_config.fsr_sharpness,
            g_fsr_lod_bias);
    }
    g_config.render_w = w, g_config.render_h = h;
    g_render_size[0] = (uint32_t)w;
    g_render_size[1] = (uint32_t)h;
    g_render_aspect = aspect;
    g_fov_scale = g_config.fov;

    // The HUD's width: 16:9 (Center), 21:9, 32:9 or the picture's (Edges), at most the picture's; the game's 2D canvas
    // gets that width (gen.py PATCHES), exec.c places the HUD's parts in it.
    float ui = g_config.ui_anchor == UI_EDGES  ? aspect
             : g_config.ui_anchor == UI_32_9 ? 32.0f / 9.0f
             : g_config.ui_anchor == UI_21_9 ? 21.0f / 9.0f
                                             : 16.0f / 9.0f;
    g_ui_anchor_aspect = ui < aspect ? ui : aspect;
    g_ui_size[1] = 720;
    g_ui_size[0] = (uint32_t)(720.0f * g_ui_anchor_aspect + 0.5f) & ~1u;
    if (g_ui_size[0] < 1280) g_ui_size[0] = 1280;

    config_runtime_init(&g_config);

    static const char *const modes[] = {"windowed", "fullscreen", "borderless"};
    LOG("[config] %s: %s %dx%d, 3D %dx%d (shape %.3f, FOV x%.2f, HUD %.3f wide), %d fps, vsync %s, MSAA %dx, FXAA %s, "
        "face buttons by %s, controller %s in the background\n", path, modes[g_config.display_mode], g_config.width,
        g_config.height, g_config.render_w, g_config.render_h, g_render_aspect, g_fov_scale, g_ui_anchor_aspect,
        g_config.fps, g_config.vsync ? "on" : "off",
        g_config.msaa, g_config.fxaa ? "on" : "off", g_config.face_buttons == FACE_LABEL ? "label" : "position",
        g_config.background_input ? "on" : "off");
    const HpScale *hp = &g_config.hp;
    LOG("[config] monster HP in Hub quests: x%.2f / %.2f / %.2f / %.2f for 1-4 hunters, %d quest(s) set apart\n",
        hp->scale[0], hp->scale[1], hp->scale[2], hp->scale[3], hp->quests);
    LOG("[config] Village quests at the Hub counter: %s (their HP x%.2f first)\n", g_config.village_in_hub ? "on" : "off",
        hp->village);
}
