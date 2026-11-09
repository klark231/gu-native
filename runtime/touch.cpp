// Touch controls for phones, in the Monster Hunter style: bronze-rimmed dark discs with cream glyphs and the game's
// button letter on a small badge.
//   - left thumb: a stick fixed at its place (the knob follows the thumb inside the rim); a light tilt walks, a full
//     one runs
//   - right thumb: the attack pill (the heavy attack A on its upper half, the light one X on the lower, both on its
//     seam), R guard / dash beside its lower end, B dodge below it, ZR, ZL target, L centre the camera (a long press:
//     the lock-on); Plus (the game's menu), the map (the left stick's click: the minimap) and the port's settings at the
//     left edge
//   - while a menu of the game's is open (the pause menu, the item pouch, a shop, a yes / no: its windows are drawn
//     on layer 97, nvn/vulkan/exec.c touch_hud_menu), nothing of the layout changes: a small pad shows besides, the
//     menu pad, which presses the d-pad (the game's menus move their cursor with it, a step and then its own repeat),
//     with the page tabs above it (L and R, or ZL and ZR by the game's option) and a small Minus below it. The stick
//     keeps walking where the game lets the hunter walk (it moves the cursor where the hunter isn't played: the title,
//     the main menu); A, B and the rest stay what they are. A drag anywhere else moves the cursor, a step every so far
//   - the game's own HUD, moved out of the thumbs' way (nvn/vulkan/exec.c): a tap on the item bar (bottom middle)
//     uses the item, a swipe on it turns the bar; a tap on a Hunter Art's panel (top right) performs it; a tap on the
//     target camera's icon turns the lock-on on or off (the camera keeps turning to the target, as in the newer games;
//     a controller's right stick click does the same, runtime/host_input.c). A padlock by the icon, lit while it is on;
//     with the controls hidden (a controller, a keyboard) it still shows while the lock-on is on, by the icon unmoved
//   - with a bow or a bowgun in hand, in a hunt, a layout of their own: a large button under the thumb that fires
//     (a drag on it, held, aims: the right thumb can't be on the screen and on a button at once), a smaller one that
//     reloads (a bow: its melee attack), aim, crouching fire (a bow: its coating), dodge, sheathe, and arrows for the
//     ammo; a swipe up or down on the item bar changes the ammo too. Laid out on its own in the editor
//   - anywhere else: dragging turns the camera by the angle moved, as in a phone game (the hunt camera's own angles,
//     runtime/hle/mousecam.c; elsewhere as the right stick); a double tap centres it (L)
// Every control can be moved and resized (the layout editor: a long press on the settings button, or the settings
// menu's Controls page), kept in touch_layout.txt beside mhgurecomp.ini.
// The same editor moves, sizes and hides each part of the game's HUD (runtime/hud_parts.h: the item bar, the Hunter Arts,
// the target camera, the clock, health and stamina, the sharpness and gauges, the party, the minimap), in the HUD of
// the touch controls or, with its switch, the one without them (a controller), live on the game.
// On a desktop the HUD editor (the settings menu's Display page: Move the HUD) moves, sizes and hides the same eight
// parts with the mouse, in the HUD shown with the touch controls hidden (a keyboard, a controller: HUD_PLAIN, the touch
// editor's HUD: controller), the touch editor's frames, names and estimates; kept in the same file.
// Fingers come from the host's main thread (touch_event, touch_input: runtime/host_input.c); the game thread draws
// (touch_draw, from runtime/overlay.cpp's frame); the render thread says where the moved HUD parts are
// (touch_hud_piece / touch_hud_frame). While the port's settings menu is open, fingers are its mouse.
// Shown while no controller is connected ([Touch] Controls = Auto), always (On) or never (Off).
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX  // (std::min / std::max below)
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "imgui.h"

#include "config.h"
extern "C" {
uint64_t plat_now_ns(void);
extern volatile uint64_t g_town_ns;  // (hle/quickhunt.c: a town's walking-around update)
extern volatile uint64_t g_present_count;  // (frames presented: the log's lines of the menu pad)
int weapon_drawn(void);  // (hle/weapon.c: 1 the weapon out, 0 sheathed, -1 not known)
// (net/ldn.c: the Join Hub screen's lists of hubs, chosen by a tap on a number as by ZL / ZR)
bool ldn_hub_browsing(void);
int ldn_hub_lists(void);
bool ldn_hub_list_goto(int n);
bool ldn_hub_list_step(int dir);
int ldn_hub_list_chosen(void);
void ldn_hub_list_range(int *first, int *last, int *total);
#include "hle/input.h"
#include "hle/itembar.h"
#include "hle/mousecam.h"
#include "hle/mousemenu.h"  // (menu_pages_zl: the pair the game turns its pages with; menu_cursor_age: a menu takes input)
SDL_Window *ui_window(void);
}
#include "haptics.h"  // (a short tick on the phone when a control is pressed)
#include "logwriter.h"  // (rt_logf: Windows queues the line, the log's own thread writes it)
#include "overlay.h"
#include "settings_panel.h"  // (settings_panel_theme: the hub list bar's colours)
#include "settings_locale.h"
#include "touch.h"

volatile int g_touch_hud;
// (runtime/hud_parts.h: the HUD's parts in both arrangements, for the render thread; sizes and opacities start at 1, so
// that nothing is drawn at no size before the layout is read)
#define HUD_ONES {1, 1, 1, 1, 1, 1, 1, 1}
static_assert(HP_COUNT == 8, "HUD_ONES: one 1 for each part");
volatile float g_hud_dx[HUD_PROFILES][HP_COUNT], g_hud_dy[HUD_PROFILES][HP_COUNT];
volatile float g_hud_k[HUD_PROFILES][HP_COUNT] = {HUD_ONES, HUD_ONES}, g_hud_alpha[HUD_PROFILES][HP_COUNT] = {HUD_ONES, HUD_ONES};
volatile int g_hud_hidden[HUD_PROFILES][HP_COUNT];
volatile float g_hud_shift_dx[HP_COUNT], g_hud_shift_dy[HP_COUNT];
volatile int g_hud_edit_prof = -1;
volatile int g_hud_prof_now;
volatile float g_hud_arts_top = HUD_ARTS_TOP_GAME;

namespace {

const float PI = 3.14159265f;
#define B(x) (1ull << (x))

// ------------------------------------------------------------------ the controls and their default places
enum Ctl {
    C_ATTACK, C_DODGE, C_R, C_ZR, C_ZL, C_L, C_MENU, C_MAP, C_SETTINGS, C_SHEATHE, C_RUN, C_HEAVY, C_LIGHT, C_CORNER,
    C_HMINUS, C_R3, C_HDPAD, C_COMBO, C_COMBO_END = C_COMBO + TOUCH_COMBOS - 1,
    C_RFIRE, C_RSECOND, C_RAIM, C_RSPECIAL, C_RDODGE, C_RSHEATHE, C_AMMO_UP, C_AMMO_DOWN,  // (a bow's or a bowgun's)
    C_MPAD,                                                                                // (the menu pad, in menus)
    C_STICK,                                                                               // (laid out: touch_layout.txt)
    C_ML, C_MR,          // (the menu pad's: its page tabs above it, as a controller's shoulders, placed from it;
    C_MZL, C_MZR,        // the other pair above them, when [Touch] ShowMenuZL / ZR say so; Minus below it, with the
    C_MMINUS,            // hunt's own Minus hidden)
    C_COUNT
};
const int C_LAID = C_STICK + 1;
enum Icon { I_ATTACK, I_DODGE, I_SHIELD, I_BURST, I_TARGET_SELECT, I_CAMERA, I_MENU, I_MAP, I_GEAR, I_SHEATHE, I_RUN, I_STICK, I_DPAD,
            I_LETTER, I_MINUS, I_COMBO, I_SHOT, I_BOW, I_RELOAD, I_SCOPE, I_CROUCH, I_COATING, I_MELEE, I_GATHER, I_UP, I_DOWN };
// The menu pad (C_MPAD), in its base's radius: a thumb within MPAD_GRAB is its, before anything else (a tap on its
// edge is a step that way, never a tab); a direction comes on past MPAD_ON and stays down to MPAD_OFF
const float MPAD_GRAB = 1.35f, MPAD_ON = .35f, MPAD_OFF = .25f;
// its group, in screen heights at its size 1, from its centre (y up): the tabs' centres (the page pair, then the other
// pair above it), Minus's; how far the group reaches each side, below and above (the other pair shown: higher), so
// that it stays on the screen whole. The tabs' boxes (TAB_W x TAB_H, a thumb's slop more) stay outside the grab ring
const float MPAD_TAB_X = .085f, MPAD_TAB_Y = .165f, MPAD_TAB2_Y = .25f, MPAD_MINUS_X = -.125f, MPAD_MINUS_Y = -.075f;
const float MPAD_HALF_W = .155f, MPAD_BELOW = .105f, MPAD_ABOVE = .199f, MPAD_ABOVE2 = .284f;

struct Def {
    const char *key, *name;  // in touch_layout.txt; in the layout editor
    Icon icon;
    const char *letter;      // the game's button, on the badge
    uint64_t buttons;
    bool right;              // placed from the screen's right edge (else from the left)
    float x, y, r;           // the default centre (screen heights from that edge and from the bottom) and radius (the
                             // attack pill: half its thickness; the stick: its base)
};
// The attack pill lies along the right thumb's arc (rising to the right, 40 degrees), both halves as far from the
// screen's corner; R (guard) by its lower end, beside the light attack; B (dodge) below it, in the corner; the small
// buttons on an arc above it; the menus in a column at the left edge, below the status
const Def DEFS[C_COUNT] = {
    {"attack", "Attack  A / X", I_ATTACK, "", 0, true, .255f, .27f, .085f},
    {"dodge", "Dodge  B", I_DODGE, "B", B(BTN_B), true, .10f, .12f, .056f},
    {"guard", "Guard  R", I_SHIELD, "R", B(BTN_R), true, .505f, .095f, .058f},
    {"zr", "Special  ZR", I_BURST, "ZR", B(BTN_ZR), true, .075f, .68f, .045f},
    {"target", "Target  ZL", I_TARGET_SELECT, "ZL", B(BTN_ZL), true, .19f, .70f, .045f},
    {"camera", "Camera  L", I_CAMERA, "L", B(BTN_L), true, .31f, .64f, .045f},
    {"menu", "Menu  +", I_MENU, "+", B(BTN_PLUS), false, .065f, .55f, .046f},
    {"map", "Left stick click", I_MAP, "L3", B(BTN_LSTICK), false, .065f, .43f, .046f},  // (the minimap on / off; the whole map:
                                                                                  // the game's menu, Y)
    {"settings", "Settings", I_GEAR, "", 0, false, .065f, .67f, .040f},
    // sheathe: Y with the weapon out (sheathed, Y uses the item: the item bar's tap); left of the pill's upper half
    {"sheathe", "Sheathe  Y", I_SHEATHE, "Y", B(BTN_Y), true, .44f, .44f, .046f},
    {"run", "Run", I_RUN, "", 0, false, .29f, .52f, .044f},  // (not shown: the stick's rim runs; kept for the file's order)
    // the attack buttons as three of their own (STYLE_SEPARATE), and as one band round the screen's corner (STYLE_CORNER:
    // x, y the band's outer edge from the screen's edges, r the radius of its middle line at the bend)
    {"heavy", "Heavy attack  A", I_ATTACK, "A", B(BTN_A), true, .16f, .33f, .078f},
    {"light", "Light attack  X", I_ATTACK, "X", B(BTN_X), true, .34f, .17f, .078f},
    {"corner", "Attack and guard", I_ATTACK, "", 0, true, .03f, .03f, .26f},
    // off by default, like ZL, ZR, L and L3 ([Touch] Show*): the game's buttons a hunt can do without
    {"minus_hunt", "Minus  -", I_MINUS, "-", B(BTN_MINUS), false, .18f, .67f, .046f},
    {"r3", "Right stick click", I_LETTER, "R3", B(BTN_RSTICK), true, .075f, .56f, .045f},
    {"dpad_hunt", "D-pad", I_DPAD, "", 0, false, .33f, .60f, .105f},
    // the player's own ([Touch] Combo1..): what they press and their label are in the settings
    {"combo1", "", I_COMBO, "", 0, true, .60f, .30f, .052f},
    {"combo2", "", I_COMBO, "", 0, true, .60f, .44f, .052f},
    {"combo3", "", I_COMBO, "", 0, true, .60f, .58f, .052f},
    {"combo4", "", I_COMBO, "", 0, true, .73f, .30f, .052f},
    {"combo5", "", I_COMBO, "", 0, true, .73f, .44f, .052f},
    {"combo6", "", I_COMBO, "", 0, true, .73f, .58f, .052f},
    // a bow's or a bowgun's (their buttons: ranged_buttons): the shot large under the thumb, the rest in an arc round it,
    // the dodge in the corner, the ammo arrows beside the item bar
    {"fire", "Fire / shoot", I_SHOT, "", 0, true, .215f, .26f, .115f},
    {"second", "Reload / melee", I_RELOAD, "", 0, true, .365f, .13f, .07f},
    {"aim", "Aim", I_SCOPE, "", 0, true, .415f, .29f, .07f},
    {"rspecial", "Crouching fire / coating", I_CROUCH, "", 0, true, .33f, .445f, .058f},
    {"rdodge", "Dodge  B", I_DODGE, "B", B(BTN_B), true, .085f, .10f, .056f},
    {"rsheathe", "Sheathe  Y", I_SHEATHE, "Y", B(BTN_Y), true, .175f, .47f, .046f},
    {"ammo_up", "Next ammo", I_UP, "", 0, true, .56f, .30f, .05f},
    {"ammo_down", "Previous ammo", I_DOWN, "", 0, true, .56f, .13f, .05f},
    // the menu pad, under the right thumb: above the attack pill's upper end (A) and right of Y, its tabs above it and
    // Minus at its lower left clear of both, from the screen's right edge like them (on a phone wider than 16:9 with the
    // HUD in the middle, mostly in the strip right of the game's windows). Its key changed with that place (it was
    // placed from the HUD's right edge, in the middle of the screen): a layout saved before keeps it at the default
    {"menupad", "Menu pad", I_DPAD, "", 0, true, .21f, .55f, .08f},
    {"stick", "Movement", I_STICK, "", 0, false, .29f, .27f, .115f},
    // the menu pad's (x, y unused: place(); what the tabs press: the game's option, mpad_buttons)
    {"ml", "L", I_LETTER, "L", B(BTN_L), true, 0, 0, .040f},  // (the shoulders: tabs in the item box, the shops)
    {"mr", "R", I_LETTER, "R", B(BTN_R), true, 0, 0, .040f},
    {"mzl", "ZL", I_LETTER, "ZL", B(BTN_ZL), true, 0, 0, .040f},  // (sections and more information: the item box, the smithy)
    {"mzr", "ZR", I_LETTER, "ZR", B(BTN_ZR), true, 0, 0, .040f},
    {"mminus", "Minus  -", I_MINUS, "-", B(BTN_MINUS), true, 0, 0, .030f},  // (the item box: its other sections)
};
// which layout a control belongs to: the hunt's (the pill, B, the map, the stick), the menus' (the d-pad, the diamond,
// Minus), or both
enum { STYLE_PILL, STYLE_SEPARATE, STYLE_CORNER, STYLE_COUNT };  // how the attacks and the guard are laid out
std::atomic<int> g_style{STYLE_PILL};                            // (Layout.style, for every thread)
std::atomic<int> g_scheme{TS_ADAPTIVE};  // [Touch] Layout (config_live_touch_scheme), as the layout in use has it
// Traditional: an emulator's controls, the same always - A B X Y (the attack buttons, dodge and sheathe), L R ZL ZR, + -, the
// d-pad and the stick - whatever the game or the weapon is doing: no menu layout, no ranged one. Its own layout file.
bool trad() { return g_scheme.load() == TS_TRADITIONAL; }
std::atomic<int> g_ranged{0};     // WEAPON_* of the hunter while the layout for it applies (touch_input), else 0
std::atomic<int> g_bow_type{0};   // the game's Bow Controls: 0 X draws, 1 R draws
std::atomic<int> g_edit_set{-1};  // the editor's: which layout it shows, 0 the usual one, 1 the ranged one; -1 not open
bool ranged_layout() {
    int set = g_edit_set.load();
    return set >= 0 ? set == 1 : g_ranged.load() != 0;
}
bool is_ranged(int i) { return i >= C_RFIRE && i <= C_AMMO_DOWN; }
// Traditional puts L and ZL at the left edge and Plus at the right one, where the controller has them
bool from_right(int i) {
    if (trad() && (i == C_L || i == C_ZL)) return false;
    if (trad() && i == C_MENU) return true;
    return DEFS[i].right;
}
// What a control shows in Traditional: the controller's letter on a plain disc
bool plain_letter(int i) {
    return trad() && (i == C_HEAVY || i == C_LIGHT || i == C_DODGE || i == C_SHEATHE || i == C_R || i == C_ZR || i == C_ZL ||
                      i == C_L || i == C_MENU || i == C_HMINUS || i == C_MAP);
}
const char *letter_of(int i) {
    switch (i) {
    case C_HEAVY: return "A";
    case C_LIGHT: return "X";
    case C_DODGE: return "B";
    case C_SHEATHE: return "Y";
    default: return DEFS[i].letter;
    }
}
const char *name_of(int i) {
    if (!trad()) return DEFS[i].name;
    switch (i) {
    case C_HEAVY: return "A";
    case C_LIGHT: return "X";
    case C_DODGE: return "B";
    case C_SHEATHE: return "Y";
    case C_R: return "R";
    case C_ZR: return "ZR";
    case C_ZL: return "ZL";
    case C_L: return "L";
    default: return DEFS[i].name;
    }
}
int show_setting(int i) {  // the [Touch] Show* a control follows (the settings button has none: it is the way back to them)
    switch (i) {
    case C_ATTACK: case C_HEAVY: case C_LIGHT: case C_CORNER: return TB_ATTACKS;
    case C_DODGE: case C_RDODGE: return TB_DODGE;
    case C_RFIRE: return TB_FIRE;
    case C_RSECOND: return TB_SECOND;
    case C_RAIM: return TB_AIM;
    case C_RSPECIAL: return TB_RSPECIAL;
    case C_AMMO_UP: case C_AMMO_DOWN: return TB_AMMO;
    case C_RSHEATHE: return TB_SHEATHE;
    case C_R: return TB_GUARD;
    case C_SHEATHE: return TB_SHEATHE;
    case C_MENU: return TB_PLUS;
    case C_HMINUS: return TB_MINUS;
    case C_HDPAD: return TB_DPAD;
    case C_L: return TB_L;
    case C_ZL: return TB_ZL;
    case C_ZR: return TB_ZR;
    case C_MZL: return TB_MENU_ZL;
    case C_MZR: return TB_MENU_ZR;
    case C_MPAD: case C_ML: case C_MR: case C_MMINUS: return TB_MENU_PAD;
    case C_MAP: return TB_L3;
    case C_R3: return TB_R3;
    case C_STICK: return TB_STICK;
    default: return -1;
    }
}
// the [Touch] TradShow* a Traditional control follows (each on by default: the layout is an emulator's, all of it)
int trad_setting(int i) {
    switch (i) {
    case C_HEAVY: case C_LIGHT: return TB_ATTACKS;
    case C_DODGE: return TB_DODGE;
    case C_SHEATHE: return TB_SHEATHE;
    case C_R: return TB_GUARD;
    case C_MENU: return TB_PLUS;
    case C_HMINUS: return TB_MINUS;
    case C_HDPAD: return TB_DPAD;
    case C_L: return TB_L;
    case C_ZL: return TB_ZL;
    case C_ZR: return TB_ZR;
    case C_MAP: return TB_L3;
    case C_R3: return TB_R3;
    default: return TB_STICK;
    }
}
bool is_combo(int i) { return i >= C_COMBO && i <= C_COMBO_END; }
bool is_tab(int i) { return i == C_ML || i == C_MR || i == C_MZL || i == C_MZR; }  // (the menu pad's shoulders, as tabs)
bool is_dpad(int i) { return i == C_HDPAD; }
bool of_mpad(int i) { return i == C_MPAD || is_tab(i) || i == C_MMINUS; }  // (the menu pad's group)
bool mpad_shown(void);  // (a menu window drawn lately, the pad's fade not over: below, with the menu's test)
bool mpad_ready(void);  // (and it has come in enough to take a finger)
bool mpad_member(int i) {  // (which of its tabs show with the menu pad: the page pair; the other pair asked for only;
    if (i == C_ML || i == C_MR) return true;  // Minus while the hunt's own is hidden)
    if (i == C_MZL || i == C_MZR) return config_live_touch_show(show_setting(i)) != 0;
    return i == C_MMINUS && !config_live_touch_show(TB_MINUS);
}
std::atomic<bool> g_menu_town;  // (the menu opened in a town: its ZL stays while the menu is open, touch_input)
bool in_mode(int i, bool menu) {
    if (trad()) {  // (the same ones in a menu and out of it; L3 and R3 only when [Touch] ShowL3 / ShowR3 say so)
        if (is_combo(i)) return config_live_touch_combo(i - C_COMBO) != 0;
        switch (i) {
        case C_SETTINGS: return true;
        case C_HEAVY: case C_LIGHT: case C_DODGE: case C_SHEATHE: case C_R: case C_ZR: case C_ZL: case C_L: case C_MENU:
        case C_HMINUS: case C_HDPAD: case C_MAP: case C_R3: case C_STICK: return config_live_touch_show_trad(trad_setting(i)) != 0;
        default: return false;
        }
    }
    // (no run button: the thumb past the stick's rim runs; what a hunt can do without is off until asked for: [Touch] Show*)
    if (i == C_RUN) return false;
    // a menu of the game's open: the menu pad besides, nothing else changes (Plus shows whatever ShowPlus says: the
    // game's menus use it, as the way out of the pause menu)
    if (i == C_MPAD) return menu && mpad_shown();
    if (of_mpad(i)) return menu && mpad_shown() && mpad_member(i);
    if (menu && i == C_MENU) return true;
    if (is_ranged(i) ? !ranged_layout() : ranged_layout() && (i == C_ATTACK || i == C_HEAVY || i == C_LIGHT || i == C_CORNER ||
                                                              i == C_R || i == C_DODGE || i == C_SHEATHE))
        return false;
    if (is_combo(i)) return config_live_touch_combo(i - C_COMBO) != 0;
    // (ZL in a town, whatever the settings say: it is the way to the multiplayer menu, and a hunt's ZL stays off; it
    // stays while a menu opened in the town is open, whether the town's update runs under it or not)
    if (i == C_ZL && (plat_now_ns() - g_town_ns < 500 * 1000000ull || (menu && g_menu_town.load()))) return true;
    int show = show_setting(i);
    if (show >= 0 && !config_live_touch_show(show)) return false;
    int style = g_style.load();
    if (i == C_ATTACK) return style == STYLE_PILL;
    if (i == C_HEAVY || i == C_LIGHT) return style == STYLE_SEPARATE;
    if (i == C_CORNER) return style == STYLE_CORNER;
    if (i == C_R) return style != STYLE_CORNER;
    return true;
}
// in the layout editor: what shows in a hunt or in a menu (Plus with ShowPlus off too, and the menu pad: placed there,
// whether a menu is open or not; its tabs and Minus go with it)
bool in_editor(int i) {
    if (i == C_MPAD) return config_live_touch_show(TB_MENU_PAD);
    return i < C_LAID && (in_mode(i, false) || in_mode(i, true));
}
const ImVec2 PILL_U(.766f, -.643f);  // the pill's axis on the screen (y down): up and to the right
const float PILL_HALF = 1.9f;        // its half length, in its radii
const float PILL_SEAM = .17f;        // the seam's half width (both buttons), in half lengths
const float TAB_W = 1.75f, TAB_H = .85f;  // the menu's L and R tabs: half their width and height, in their radius
const uint64_t PILL_BUTTONS[4] = {B(BTN_A), B(BTN_X), B(BTN_X) | B(BTN_A), B(BTN_R)};  // the upper half (heavy), the
                                                                 // lower (light), the seam (both); the corner's guard
// The corner band, from its left end along the bottom, round the bend, up the right edge; in its bend's radius r:
const float BAND_T = .365f;     // its half thickness
const float BAND_LEFT = 1.15f;  // the straight part along the bottom
const float BAND_UP = .96f;     // the straight part up the side
const float BAND_LEN = BAND_LEFT + PI * .5f + BAND_UP;
// where its buttons end, in parts of its length: the guard, the light attack, the seam (both attacks), the heavy attack
const float BAND_GUARD = .30f, BAND_SEAM0 = .63f, BAND_SEAM1 = .69f;
const float KNOB = .45f;             // the stick's knob, in its base's radius
const float CAMERA_COUNTS = 1150.0f; // mouse counts for a drag the screen's height (hle/mousecam.c: 0.08 degrees each)
const uint64_t MS = 1000000ull;
// a Hunter Art's row: its button
// (the game's default controls: the d-pad - its icon beside each art is a d-pad with that arm marked: up, right, down)
const uint64_t ART_FACE[3] = {B(BTN_UP), B(BTN_RIGHT), B(BTN_DOWN)};

// What a control of the ranged layout presses. Out, a bowgun: A fires, X reloads, R aims (a tap the scope, held the quick
// aim), X + A crouching fire; a bow: the game's Bow Controls say which of X and R draws (held, let go it shoots) and which
// enters the aiming mode; A its melee attack, X + A its coating. Sheathed the large button brings the weapon out (X: R
// would run) and the smaller one does what A does then: gather, carve, talk. In a menu of the game's the same ones
// whatever is in hand: the shot confirms (A), the smaller one is X, aim R; crouching fire and the ammo do nothing
uint64_t ranged_buttons(int c, int drawn, bool menu = false) {
    if (menu) {
        switch (c) {
        case C_RFIRE: return B(BTN_A);
        case C_RSECOND: return B(BTN_X);
        case C_RAIM: return B(BTN_R);
        case C_RSPECIAL: case C_AMMO_UP: case C_AMMO_DOWN: return 0;
        default: return DEFS[c].buttons;
        }
    }
    bool bow = g_ranged.load() == WEAPON_BOW, type2 = g_bow_type.load() != 0, out = drawn == 1;
    switch (c) {
    case C_RFIRE: return !out ? B(BTN_X) : !bow ? B(BTN_A) : type2 ? B(BTN_R) : B(BTN_X);
    case C_RSECOND: return !out || bow ? B(BTN_A) : B(BTN_X);
    case C_RAIM: return !out || (bow && type2) ? B(BTN_X) : B(BTN_R);
    case C_RSPECIAL: return B(BTN_X) | B(BTN_A);
    default: return DEFS[c].buttons;
    }
}

// A part of the game's HUD in one arrangement (runtime/hud_parts.h): its offset from its default place (canvas units),
// its size about its middle, its opacity (1: kept for when it is offered), hidden
struct HudVals {
    float dx, dy, k, alpha;
    bool hidden;
};
struct Layout {
    float x[C_COUNT], y[C_COUNT], scale[C_COUNT];
    int style;                               // STYLE_*
    HudVals hud[HUD_PROFILES][HP_COUNT];     // (HUD_TOUCH: hud_<key> in the file; HUD_PLAIN: pc_hud_<key>)
};
const char *const HUD_NAME[HP_COUNT] = {"Item bar", "Hunter Arts", "Target camera", "Clock", "Health and stamina",
                                        "Sharpness and gauges", "Party", "Map"};
HudVals hud_default(void) { return {0.0f, 0.0f, 1.0f, 1.0f, false}; }
// (the item bar with the touch controls: a finger picks and uses the items on it, and swipes it: never hidden there)
bool hud_hide_ok(int prof, int part) { return !(prof == HUD_TOUCH && part == HP_ITEMS); }
Layout defaults(void) {
    Layout l = {};
    for (int i = 0; i < C_COUNT; i++) l.x[i] = DEFS[i].x, l.y[i] = DEFS[i].y, l.scale[i] = 1.0f;  // (the pad's group: unused)
    for (auto &prof : l.hud)
        for (HudVals &v : prof) v = hud_default();
    if (trad()) {  // (the layout in use is the traditional one: the diamond right, the shoulders at the top corners, + and - at
                   // the top middle, the d-pad above the stick; x, y: screen heights from the edge it is placed from, from the bottom)
        l.style = STYLE_SEPARATE;
        struct { int c; float x, y, k; } T[] = {
            {C_HEAVY, .105f, .30f, .75f}, {C_DODGE, .225f, .175f, 1.04f}, {C_LIGHT, .225f, .425f, .75f},  // A right, B below, X above
            {C_SHEATHE, .345f, .30f, 1.26f},                                                               // Y left
            {C_R, .09f, .80f, 1.0f}, {C_ZR, .09f, .93f, 1.0f}, {C_L, .09f, .80f, 1.0f}, {C_ZL, .09f, .93f, 1.0f},
            {C_MENU, .60f, .93f, 1.0f}, {C_HMINUS, .60f, .93f, 1.0f}, {C_HDPAD, .30f, .60f, .95f},
            {C_MAP, .09f, .14f, 1.0f}, {C_R3, .09f, .14f, 1.0f}, {C_SETTINGS, .065f, .67f, 1.0f}, {C_STICK, .29f, .27f, 1.0f},
        };
        for (const auto &t : T) l.x[t.c] = t.x, l.y[t.c] = t.y, l.scale[t.c] = t.k;
    }
    return l;
}
void publish_hud(const Layout &l) {  // (to the render thread)
    for (int p = 0; p < HUD_PROFILES; p++)
        for (int k = 0; k < HP_COUNT; k++) {
            const HudVals &v = l.hud[p][k];
            g_hud_dx[p][k] = v.dx, g_hud_dy[p][k] = v.dy, g_hud_k[p][k] = v.k, g_hud_alpha[p][k] = v.alpha;
            g_hud_hidden[p][k] = v.hidden;
        }
}
const char *const LAYOUT_FILES[2] = {"touch_layout.txt", "touch_layout_traditional.txt"};  // (the working folder: the app's, beside mhgurecomp.ini)
const char *layout_file() { return LAYOUT_FILES[trad() ? 1 : 0]; }

bool load_layout(Layout *l) {
    *l = defaults();
    FILE *f = fopen(layout_file(), "r");
    if (!f) return false;
    char line[160], key[32];
    float x, y, s, alpha;
    int hidden;
    while (fgets(line, sizeof line, f)) {
        // (a HUD part's line: offset, size, then opacity and hidden, which files older than them lack: hud_<key> dx dy 1
        // with the touch controls, pc_hud_<key> dx dy size without)
        int n = sscanf(line, "%31s %f %f %f %f %d", key, &x, &y, &s, &alpha, &hidden);
        if (n < 4) continue;
        for (int i = 0; i < C_LAID; i++)
            if (!strcmp(key, DEFS[i].key) && x > 0 && x < 4 && y > 0 && y < 1 && s >= .4f && s <= 2.5f)
                l->x[i] = x, l->y[i] = y, l->scale[i] = s;
        for (int p = 0; p < HUD_PROFILES; p++)
            for (int k = 0; k < HP_COUNT; k++) {
                char name[32];
                snprintf(name, sizeof name, "%shud_%s", p == HUD_PLAIN ? "pc_" : "", HUD_KEY[k]);
                if (strcmp(key, name) || fabsf(x) > HUD_DX_MAX || fabsf(y) > HUD_DY_MAX || s < HUD_SIZE_MIN || s > HUD_SIZE_MAX) continue;
                HudVals &v = l->hud[p][k];
                v.dx = x, v.dy = y, v.k = s;
                v.alpha = n >= 5 && alpha >= .2f && alpha <= 1.0f ? alpha : 1.0f;
                v.hidden = n >= 6 && hidden == 1 && hud_hide_ok(p, k);
            }
        if (!strcmp(key, "attack_style") && x >= 0 && x < STYLE_COUNT && !trad()) l->style = (int)x;
    }
    fclose(f);
    return true;
}

void save_layout(const Layout &l) {
    // (to a temporary file, on the disk, then over the old one: the app closed or killed while saving keeps the old layout)
    char temp[128];
    snprintf(temp, sizeof temp, "%s.tmp", layout_file());
    FILE *f = fopen(temp, "w");
    if (!f) { rt_logf("[touch] could not save %s\n", layout_file()); return; }
    fprintf(f, "# touch controls: name, centre (screen heights from its edge and the bottom), size\n");
    for (int i = 0; i < C_LAID; i++) fprintf(f, "%s %.4f %.4f %.3f\n", DEFS[i].key, l.x[i], l.y[i], l.scale[i]);
    fprintf(f, "# the game's HUD parts with the touch controls: name, offset from their place (the game's 1280x720 units), size, "
               "opacity, hidden\n");
    for (int k = 0; k < HP_COUNT; k++) {
        const HudVals &v = l.hud[HUD_TOUCH][k];
        fprintf(f, "hud_%s %.1f %.1f %.2f %.2f %d\n", HUD_KEY[k], v.dx, v.dy, v.k, v.alpha, (int)v.hidden);
    }
    fprintf(f, "attack_style %d 0 1\n", l.style);
    fprintf(f, "# the game's HUD parts without them (a controller, a keyboard): the same\n");
    for (int k = 0; k < HP_COUNT; k++) {
        const HudVals &v = l.hud[HUD_PLAIN][k];
        fprintf(f, "pc_hud_%s %.1f %.1f %.2f %.2f %d\n", HUD_KEY[k], v.dx, v.dy, v.k, v.alpha, (int)v.hidden);
    }
    bool ok = fflush(f) == 0 && !ferror(f);
#ifdef _WIN32
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && fsync(fileno(f)) == 0;
#endif
    ok = fclose(f) == 0 && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(temp, layout_file(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    ok = ok && rename(temp, layout_file()) == 0;
#endif
    if (!ok) { remove(temp); rt_logf("[touch] could not save %s (the old layout is kept)\n", layout_file()); return; }
    rt_logf("[touch] layout saved (%s)\n", layout_file());
}

struct Geo {
    ImVec2 c;
    float r;
};
float mpad_above(void) {  // (how far the menu pad's group reaches above it: the other pair of tabs shown, higher)
    return config_live_touch_show(TB_MENU_ZL) || config_live_touch_show(TB_MENU_ZR) ? MPAD_ABOVE2 : MPAD_ABOVE;
}
Geo place(const Layout &l, int i, float w, float h) {
    const Def &d = DEFS[i];
    if (i == C_CORNER) {  // (c: the centre of its bend; a larger band stays against the edges)
        float r = d.r * l.scale[i] * h, in = r * (1 + BAND_T);
        return {ImVec2(w - l.x[i] * h - in, h - l.y[i] * h - in), r};
    }
    if (i == C_MPAD) {  // (its whole group on the screen: the tabs above it, Minus below)
        float k = l.scale[i], hw = MPAD_HALF_W * k * h, up = mpad_above() * k * h, down = MPAD_BELOW * k * h;
        float x = w - l.x[i] * h, y = h - l.y[i] * h;
        return {ImVec2(std::clamp(x, hw, std::max(hw, w - hw)), std::clamp(y, up, std::max(up, h - down))), d.r * k * h};
    }
    if (of_mpad(i)) {  // above the menu pad, as a controller's shoulders: the page pair left and right, the other pair
        Geo g = place(l, C_MPAD, w, h);  // above them; Minus below it, at the left. Outside its grab ring at any size
        float k = l.scale[C_MPAD];
        int n = i == C_ML ? 0 : i == C_MR ? 1 : i == C_MZL ? 2 : i == C_MZR ? 3 : 4;
        static const float ox[5] = {-MPAD_TAB_X, MPAD_TAB_X, -MPAD_TAB_X, MPAD_TAB_X, MPAD_MINUS_X},
                           oy[5] = {MPAD_TAB_Y, MPAD_TAB_Y, MPAD_TAB2_Y, MPAD_TAB2_Y, MPAD_MINUS_Y};
        return {ImVec2(g.c.x + ox[n] * k * h, g.c.y - oy[n] * k * h), d.r * k * h};
    }
    return {ImVec2(from_right(i) ? w - l.x[i] * h : l.x[i] * h, h - l.y[i] * h), d.r * l.scale[i] * h};
}

// the d-pad's direction under a finger, from its centre: one at a time (a menu's cursor goes one way: a thumb between
// two arms pressing both makes it jump), and the one held stays until the thumb is clearly nearer another
const uint64_t DPAD_UP = B(BTN_UP), DPAD_DOWN = B(BTN_DOWN), DPAD_LEFT = B(BTN_LEFT), DPAD_RIGHT = B(BTN_RIGHT);
int dpad_part(Geo g, float x, float y, int held = 0, float on = .17f, float off = .12f) {
    float dx = x - g.c.x, dy = y - g.c.y, ax = fabsf(dx), ay = fabsf(dy);
    if (hypotf(dx, dy) < g.r * (held ? off : on)) return 0;  // (the middle: none)
    const float STICKY = 1.35f;
    bool across = held & 3 ? ax * STICKY >= ay : held & 12 ? ax >= ay * STICKY : ax >= ay;
    return across ? (dx > 0 ? 1 : 2) : (dy < 0 ? 4 : 8);  // (right, left; up, down)
}
// the menu pad's: a wider middle (a thumb resting on it presses nothing until it leans), the same arms
int mpad_part(Geo g, float x, float y, int held = 0) { return dpad_part(g, x, y, held, MPAD_ON, MPAD_OFF); }
uint64_t dpad_buttons(int part) {
    return (part & 1 ? DPAD_RIGHT : 0) | (part & 2 ? DPAD_LEFT : 0) | (part & 4 ? DPAD_UP : 0) | (part & 8 ? DPAD_DOWN : 0);
}

// The band's middle line: the point s along it (0 its left end, BAND_LEN * r its top), off it by e (> 0 toward the outside)
ImVec2 band_point(Geo g, float s, float e) {
    float r = g.r, left = BAND_LEFT * r, bend = PI * .5f * r;
    if (s <= left) return ImVec2(g.c.x - left + s, g.c.y + r + e);
    if (s >= left + bend) return ImVec2(g.c.x + r + e, g.c.y - (s - left - bend));
    float an = PI * .5f - (s - left) / r;
    return ImVec2(g.c.x + cosf(an) * (r + e), g.c.y + sinf(an) * (r + e));
}
// How far a point is from the middle line, and how far along it (ends included: their round caps)
float band_distance(Geo g, float x, float y, float *along) {
    float r = g.r, left = BAND_LEFT * r, bend = PI * .5f * r, up = BAND_UP * r, px = x - g.c.x, py = y - g.c.y, s, d;
    if (px <= 0 && py >= 0) {  // (the straight part along the bottom)
        float t = std::clamp(px + left, 0.0f, left);
        s = t, d = hypotf(px + left - t, py - r);
    } else if (px >= 0 && py <= 0) {  // (the one up the side)
        float t = std::clamp(-py, 0.0f, up);
        s = left + bend + t, d = hypotf(px - r, -py - t);
    } else if (px > 0 && py > 0) {  // (the bend)
        float an = atan2f(py, px);
        s = left + (PI * .5f - an) * r, d = fabsf(hypotf(px, py) - r);
    } else {  // (inside the band's open side: the nearer end of the two straight parts)
        float a = hypotf(px + std::min(left, -px), py - r), b = hypotf(px - r, py + std::min(up, -py));
        s = a < b ? left + std::max(px, -left) : left + bend + std::min(up, -py), d = std::min(a, b);
    }
    if (along) *along = s;
    return d;
}
int band_part(Geo g, float x, float y) {  // PILL_BUTTONS' index
    float s = 0;
    band_distance(g, x, y, &s);
    s /= g.r * BAND_LEN;
    return s < BAND_GUARD ? 3 : s < BAND_SEAM0 ? 1 : s < BAND_SEAM1 ? 2 : 0;
}

float pill_distance(Geo g, float x, float y) {  // from the pill's axis
    float half = g.r * PILL_HALF, px = x - g.c.x, py = y - g.c.y;
    float t = std::clamp(px * PILL_U.x + py * PILL_U.y, -half, half);
    float qx = px - PILL_U.x * t, qy = py - PILL_U.y * t;
    return sqrtf(qx * qx + qy * qy);
}
int pill_part(Geo g, float x, float y) {  // 0 the upper half (A, heavy), 1 the lower (X, light), 2 the seam (both)
    float t = ((x - g.c.x) * PILL_U.x + (y - g.c.y) * PILL_U.y) / (g.r * PILL_HALF);
    return t > PILL_SEAM ? 0 : t < -PILL_SEAM ? 1 : 2;
}

// the point between the separate attack buttons that presses both (not when they are moved far apart)
bool separate_seam(const Layout &l, float w, float h, ImVec2 *at, float *size) {
    if (trad()) return false;  // (A and X are two buttons, as on a controller: nothing presses both between them)
    Geo a = place(l, C_HEAVY, w, h), b = place(l, C_LIGHT, w, h);
    float gap = hypotf(a.c.x - b.c.x, a.c.y - b.c.y) - a.r - b.r;
    if (gap > a.r * 2) return false;
    *at = ImVec2((a.c.x + b.c.x) * .5f, (a.c.y + b.c.y) * .5f);
    *size = a.r * .3f;
    return true;
}

// the control under a finger (a little larger than its art: a thumb is wider), and the attack pill's part. In a menu of
// the game's the menu pad first: a thumb within its ring is its (the tabs and the rest after), and no seam presses two
// attacks at once (no menu wants X + A); in the editor, what it shows (in_editor)
int hit(const Layout &l, float x, float y, float w, float h, int *part, float slop = 1.22f, bool stick = false,
        bool menu = false, bool editor = false) {
    bool pad_ready = !menu || editor || mpad_ready();  // (just opened: not yet)
    if (menu && !editor && pad_ready && in_mode(C_MPAD, true)) {
        Geo g = place(l, C_MPAD, w, h);
        if (hypotf(x - g.c.x, y - g.c.y) < g.r * MPAD_GRAB) {
            if (part) *part = mpad_part(g, x, y);
            return C_MPAD;
        }
    }
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < C_COUNT; i++) {
        if (!(editor ? in_editor(i) : in_mode(i, menu)) || (i == C_STICK && !stick) || (of_mpad(i) && !pad_ready)) continue;
        Geo g = place(l, i, w, h);
        float d = i == C_ATTACK ? pill_distance(g, x, y) / (g.r * (slop - .07f))
                : i == C_CORNER ? band_distance(g, x, y, nullptr) / (g.r * BAND_T * (slop - .07f))
                : is_tab(i) ? std::max(fabsf(x - g.c.x) / (g.r * TAB_W * slop), fabsf(y - g.c.y) / (g.r * TAB_H * slop))
                                : hypotf(x - g.c.x, y - g.c.y) / (g.r * slop);
        if (d < 1 && of_mpad(i) && !editor) d *= .01f;  // (the menu pad's tabs over a button placed under them: theirs)
        if (d < 1 && d < bd) bd = d, best = i;
    }
    if (editor && in_editor(C_MPAD))  // (the editor: a tab or Minus picks the menu pad, which they go with)
        for (int i = C_ML; i <= C_MMINUS; i++) {
            Geo g = place(l, i, w, h);
            float d = is_tab(i) ? std::max(fabsf(x - g.c.x) / (g.r * TAB_W * slop), fabsf(y - g.c.y) / (g.r * TAB_H * slop))
                                : hypotf(x - g.c.x, y - g.c.y) / (g.r * slop);
            if (mpad_member(i) && d < 1 && d < bd) bd = d, best = C_MPAD;
        }
    if (best == C_ATTACK && part) {
        Geo g = place(l, C_ATTACK, w, h);
        *part = !menu ? pill_part(g, x, y) : (x - g.c.x) * PILL_U.x + (y - g.c.y) * PILL_U.y > 0 ? 0 : 1;  // (a menu: no seam)
    }
    if (best == C_CORNER && part) {
        Geo g = place(l, C_CORNER, w, h);
        *part = band_part(g, x, y);
        if (menu && *part == 2) {  // (the seam: the nearer attack)
            float s = 0;
            band_distance(g, x, y, &s);
            *part = s / (g.r * BAND_LEN) < (BAND_SEAM0 + BAND_SEAM1) * .5f ? 1 : 0;
        }
    }
    if (part && !menu && g_style.load() == STYLE_SEPARATE) {  // (between the two attack buttons: both, as the pill's seam)
        ImVec2 at;
        float size;
        if (separate_seam(l, w, h, &at, &size) && hypotf(x - at.x, y - at.y) < size * 1.5f) best = C_LIGHT, *part = 1;
    }
    if (best >= 0 && is_dpad(best) && part) *part = dpad_part(place(l, best, w, h), x, y);
    return best;
}

// ------------------------------------------------------------------ the moved HUD parts (render thread -> others)
// Each part as the last frame that drew any drew it: how many pieces, their places on the canvas (all of them, and those
// a finger works: the item bar without its ammo list), its size, hidden; the Hunter Arts' pieces (the game's y and the
// canvas', for their rows)
struct Zones {
    int n[HP_COUNT], nt[HP_COUNT];
    float x0[HP_COUNT], y0[HP_COUNT], x1[HP_COUNT], y1[HP_COUNT];
    float tx0[HP_COUNT], ty0[HP_COUNT], tx1[HP_COUNT], ty1[HP_COUNT];
    float k[HP_COUNT];
    bool hidden[HP_COUNT];
    int na;
    float agy[64], acy[64];
    uint64_t ns;
};
std::mutex g_zone_lock;
Zones g_zones, g_build;  // (g_build: the render thread's alone)
// Each part as last drawn this session, on the canvas: its frame (its pieces' places and HUD_PAD; the minimap: its box),
// where its default box went then (hud_box), in which arrangement and when: the editor frames a part from it, moved and
// sized as the box is since (immediately as it is dragged, and where it would be when it isn't drawn)
struct Seen {
    float r[4], e[4];
    int prof;
    uint64_t ns;
};
Seen g_seen[HP_COUNT];  // (under g_zone_lock)
bool g_build_menu;       // (render thread) a menu window of the game's in this frame
std::atomic<uint64_t> g_menu_ns;  // when one was last drawn

// The parts are gone (a menu over the hunt, a cutscene) once no frame has drawn them for three frames' time (at
// least 250 ms, at most 1.5 s: a slow frame now and then must not take them away under a finger)
std::atomic<uint64_t> g_frame_ns{16 * 1000000ull};  // the time between presented frames, smoothed
uint64_t stale_limit(void) { return std::clamp<uint64_t>(3 * g_frame_ns.load(), 250 * 1000000ull, 1500 * 1000000ull); }
bool stale(const Zones &z) { return SDL_GetTicksNS() - z.ns > stale_limit(); }
// Menu detection: a menu of the game's is open while one of its windows is drawn (just now: within stale_limit, or with
// no frame presented since - a long update in a menu, a model loading for 300 ms on a slow phone, keeps it open), and
// while the hunter is not being played (the title, the main menu, the character creation, a cutscene: no hunt camera and
// no town walked for a few seconds - a few, so carving or a short animation in a hunt, when the camera handler does not
// run, is no menu). Then the menu pad shows; nothing else changes.
// A window on that layer that comes up by itself while the hunter is being played, a chat bubble, is no menu: the game's
// menus open at a press. Nothing shows which of the two it is: not the hunter, not the game's globals, and the hunt camera
// runs under a menu too. A real one taken for a bubble can still be answered: A and B are where they always are
std::atomic<uint64_t> g_play_ns;   // (plat_now_ns) when the hunter was last played
std::atomic<uint32_t> g_play_upd;  // (menu_updates) and in which of the game's updates
std::atomic<uint64_t> g_press_ns;  // (SDL_GetTicksNS) when a button that can open a menu was last pressed
bool g_window_open, g_window_asked;
const uint64_t PLAY_HOLD = 4000 * 1000000ull, WINDOW_PRESS = 1500 * 1000000ull, PLAY_NOW = 400 * 1000000ull;
std::atomic<int> g_frames_since_menu{1000};  // (touch_hud_frame: 0 when the frame presented had a menu window)
std::atomic<uint64_t> g_menu_gone_ns;        // (SDL ticks) when the second frame without one came (0: one since)
bool game_menu(void) {
    uint64_t m = g_menu_ns.load(), p = g_play_ns.load(), now = plat_now_ns(), ticks = SDL_GetTicksNS();
    if (m && (ticks - m < stale_limit() || g_frames_since_menu.load() < 3)) {
        if (!g_window_open) g_window_asked = !p || now - p > PLAY_NOW || ticks - g_press_ns.load() < WINDOW_PRESS;
        g_window_open = true;
        if (g_window_asked) return true;
    } else {
        g_window_open = false;
    }
    return !p || now - p > PLAY_HOLD;
}
// (a bow drawn or a gun aimed has a camera of its own: the hunt camera's handler doesn't run then)
void play_seen(uint32_t hunter) {  // (the SDL main thread, each input update)
    uint64_t now = plat_now_ns(), town = g_town_ns;
    if (mousecam_active() || (town && now - town < 400 * 1000000ull) || weapon_moving(hunter) || weapon_aiming(hunter))
        g_play_ns = now, g_play_upd = menu_updates();
}
// The hunter is being played just now: the hunt camera, a town walked (both run under the game's menus too: the pause
// menu, the pouch, a town's item box), the weapon. Not: the title, the main menu, a cutscene, a talk. (Within PLAY_NOW,
// or within the game's last three updates for up to 2 s: a long update, a stall, is no time without the hunter)
bool playable(void) {
    uint64_t p = g_play_ns.load(), t = plat_now_ns() - p;
    return p && (t < PLAY_NOW || (t < 2000 * 1000000ull && menu_updates() - g_play_upd.load() < 3));
}
// The menu pad's presses are the d-pad's - in the field a Hunter Art. A press may start only while a menu takes input: a
// cursor of its (a list, a box, a quantity, a choice of two) ran in the game's last update (counted, not timed: a long
// update in a menu keeps it; hle/mousemenu.c menu_cursor_age), or with the hunter not played (the title, a talk: no Art
// can come of it). Not on the menu's windows: a hunt's menu closing still draws them for 100 ms and more, running no
// cursor, and the hunter may be played again by then. What the pad presses lets go once no cursor has run for
// MPAD_LET_GO updates (the hunter played): a thumb left on it as the menu closes is no d-pad held in the field
bool press_ok(void) { return menu_cursor_age() <= 1 || !playable(); }
const uint32_t MPAD_LET_GO = 2;  // (updates without a menu's cursor: touch_input)
// The pad comes in as the menu opens (MPAD_IN) and goes as its windows go: from the later of the second frame without
// one and the last frame with one and MPAD_HOLD (a submenu's first frames without a window: measured one frame, 30 ms),
// in MPAD_OUT; gone within 180 ms of the last window (a frame's time to draw it included), not when the menu mode ends
// (stale_limit, 250 ms and more). It stays while a menu's cursor runs, windows or not (a model shown alone)
const uint64_t MPAD_IN = 120 * 1000000ull, MPAD_HOLD = 90 * 1000000ull, MPAD_OUT = 80 * 1000000ull;
// Not on the title, the main menu, the character's choice and making (the front end: its backdrop drawn just now): their
// lists lie across the middle of the screen, under it, and nobody walks there - the stick moves their cursor (with the
// stick hidden, ShowStick Off, the pad shows there too: nothing else on the screen would move it)
std::atomic<int> g_frames_since_front{1000};  // (touch_hud_frame: 0 when the frame presented had the front end's backdrop)
float mpad_fade(uint64_t ticks) {  // (1 while a menu window shows, down to 0 after it is gone)
    if (!config_live_touch_show(TB_MENU_PAD)) return 0.0f;
    if (g_frames_since_front.load() <= 2 && config_live_touch_show(TB_STICK)) return 0.0f;
    uint64_t m = g_menu_ns.load();
    if (!m) return 0.0f;
    if (g_frames_since_menu.load() <= 1 || menu_cursor_age() <= 1) return 1.0f;
    uint64_t start = std::max(g_menu_gone_ns.load(), m + MPAD_HOLD);
    if (ticks <= start) return 1.0f;
    return std::max(0.0f, 1.0f - (float)(ticks - start) / (float)MPAD_OUT);
}
bool mpad_shown(void) { return mpad_fade(SDL_GetTicksNS()) > 0.0f; }
// A finger takes the pad once it has come in a little (MPAD_TAKE after the menu opened): not one that landed there as it
// opened, when nothing showed yet
const uint64_t MPAD_TAKE = 30 * 1000000ull;
std::atomic<uint64_t> g_menu_on_ns;  // (SDL ticks: s.menu_on_ns, for hit)
bool mpad_ready(void) { return SDL_GetTicksNS() - g_menu_on_ns.load() >= MPAD_TAKE; }

// The game's picture in the window: as wide as it fits in its own shape (g_render_aspect), centred (device.c
// backend_present), with bars when the shapes differ; the HUD's 1280x720 canvas covers the picture
struct Pic {
    float x0, y0, w, h;
};
Pic picture(float w, float h) {
    float a = g_render_aspect > 0 ? g_render_aspect : w / h, pw = w, ph = w / a;
    if (ph > h) ph = h, pw = h * a;
    return {(w - pw) * .5f, (h - ph) * .5f, pw, ph};
}

struct Rect {
    float x0, y0, x1, y1;
    bool ok;
    bool in(float x, float y) const { return ok && x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};
// A part where it was drawn last, in display pixels: its pieces' places and how far its art reaches past them (HUD_PAD,
// sized with it), a little more for a finger. For a finger (touch), the pieces a finger works and not while hidden; for
// a frame, all of them
Rect zone_rect(const Zones &z, int k, float w, float h, bool touch = true) {
    Rect r = {1e30f, 1e30f, -1e30f, -1e30f, false};
    if (!(touch ? z.nt[k] : z.n[k]) || stale(z) || (touch && z.hidden[k])) return r;
    if (touch) r.x0 = z.tx0[k], r.y0 = z.ty0[k], r.x1 = z.tx1[k], r.y1 = z.ty1[k];
    else r.x0 = z.x0[k], r.y0 = z.y0[k], r.x1 = z.x1[k], r.y1 = z.y1[k];
    float s = hud_squeeze(), m = z.k[k];  // (a part sized: its art reaches as much farther)
    Pic p = picture(w, h);
    r.x0 = p.x0 + (r.x0 - (HUD_PAD[k][0] * m + HUD_SLOP) * s) * p.w / 1280.0f, r.x1 = p.x0 + (r.x1 + (HUD_PAD[k][2] * m + HUD_SLOP) * s) * p.w / 1280.0f;
    r.y0 = p.y0 + (r.y0 - HUD_PAD[k][1] * m - HUD_SLOP) * p.h / 720.0f, r.y1 = p.y0 + (r.y1 + HUD_PAD[k][3] * m + HUD_SLOP) * p.h / 720.0f;
    r.ok = true;
    return r;
}
// the Hunter Arts' rows (their pieces grouped by the game's y), top to bottom: screen y ranges
int arts_rows(const Zones &z, float w, float h, float rows[3][2]) {
    int k = HP_ARTS, n = z.na, idx[64];
    if (!n || !z.nt[k] || stale(z)) return 0;
    for (int i = 0; i < n; i++) idx[i] = i;
    std::sort(idx, idx + n, [&](int a, int b) { return z.agy[a] < z.agy[b]; });
    int nr = 0;
    float last = -1e9f;
    for (int j = 0; j < n; j++) {
        int i = idx[j];
        if (z.agy[i] - last > 22.0f) {
            if (nr == 3) break;
            rows[nr][0] = rows[nr][1] = z.acy[i], nr++;
        }
        rows[nr - 1][0] = std::min(rows[nr - 1][0], z.acy[i]), rows[nr - 1][1] = std::max(rows[nr - 1][1], z.acy[i]);
        last = z.agy[i];
    }
    Pic p = picture(w, h);
    float m = z.k[k];
    for (int i = 0; i < nr; i++)
        rows[i][0] = p.y0 + (rows[i][0] - HUD_PAD[k][1] * m - 4) * p.h / 720.0f, rows[i][1] = p.y0 + (rows[i][1] + HUD_PAD[k][3] * m + 4) * p.h / 720.0f;
    return nr;
}
Zones zones(void) {
    std::lock_guard<std::mutex> l(g_zone_lock);
    return g_zones;
}
// A part's frame in the editor, in display pixels, a little larger than its art: where it is drawn now (live), or where
// it would be (as last drawn, moved and sized as its box is since; never drawn this session: its box placed). With at:
// where it would be with those values (the factory's: the editor's warnings)
Rect part_frame(int k, int prof, float w, float h, bool *live, const float *at = nullptr) {
    Seen sn;
    {
        std::lock_guard<std::mutex> l(g_zone_lock);
        sn = g_seen[k];
    }
    float e[4], r[4];
    if (at) hud_box_with(k, prof, g_hud_arts_top, at[0], at[1], at[2], e);
    else hud_box(k, prof, g_hud_arts_top, e);
    memcpy(r, e, sizeof r);
    bool same = sn.ns && sn.prof == prof;
    if (same && !HUD_FRAME_BOX(k)) {
        float kx = (e[2] - e[0]) / std::max(.001f, sn.e[2] - sn.e[0]), ky = (e[3] - e[1]) / std::max(.001f, sn.e[3] - sn.e[1]);
        r[0] = e[0] + (sn.r[0] - sn.e[0]) * kx, r[2] = e[0] + (sn.r[2] - sn.e[0]) * kx;
        r[1] = e[1] + (sn.r[1] - sn.e[1]) * ky, r[3] = e[1] + (sn.r[3] - sn.e[1]) * ky;
    }
    *live = same && SDL_GetTicksNS() - sn.ns <= stale_limit();
    float s = hud_squeeze();
    Pic p = picture(w, h);
    return {p.x0 + (r[0] - HUD_SLOP * s) * p.w / 1280.0f, p.y0 + (r[1] - HUD_SLOP) * p.h / 720.0f,
            p.x0 + (r[2] + HUD_SLOP * s) * p.w / 1280.0f, p.y0 + (r[3] + HUD_SLOP) * p.h / 720.0f, true};
}
bool target_seen(void) {  // (the target camera's icon was drawn just now: the lock-on's padlock goes by it)
    std::lock_guard<std::mutex> l(g_zone_lock);
    return g_zones.nt[HP_TARGET] && !stale(g_zones);
}

// ------------------------------------------------------------------ fingers (host main thread)
enum Kind { F_NONE, F_BUTTON, F_STICK, F_CAMERA, F_MENU, F_ITEM, F_ARTS, F_TARGET, F_EDIT };
struct Finger {
    SDL_FingerID id;
    Kind kind;
    int ctl, part;       // the control (F_BUTTON, F_EDIT), the pill's part
    float x, y, x0, y0;  // pixels: now, where it landed
    uint64_t t0;
    float swipe, swipe_y;  // the item bar, a menu's cursor: pixels swiped since the last step
    bool moved, done;
    uint64_t btn;          // a control of the ranged layout: what it pressed when it landed (the weapon may go out under it)
    int sent;              // the menu pad's: the direction it presses now (dpad_part's; a tab, Minus: 1 pressed), which
                           // follows part as press_ok lets a press start
};
struct Pulse {
    uint64_t buttons, from, until;  // (held from .. until: a sequence starts some later)
    bool menu;                      // the menu's (the menu pad's, a drag's step): dropped as the menu closes...
    bool fresh;                     // ...and dropped at its first update unless a press may start then (press_ok)
    bool checked;
    int ctl, part;                  // a tap of the menu pad's group: which one, its direction (-1: no such tap)
};
struct State {
    Finger f[10];
    bool stick_on;
    float bx, by, kx, ky;    // the stick's base and knob, pixels
    float cam_dx, cam_dy;    // pixels dragged since the last touch_input
    uint64_t held;           // buttons held by fingers
    uint64_t tapped;         // and those held at any moment since the game last read them (touch_input): a quick tap
                             // between two reads still reaches the game, as mashing to break free of a grab needs (#150)
    uint64_t last_ns;        // the last touch
    uint64_t cam_ns;         // the last camera drag
    uint64_t tap_ns;         // the last short tap on the camera's part (a second one soon after centres it)
    uint64_t step_ns;        // when a drag may move a menu's cursor again
    float tap_x, tap_y;
    Pulse pulse[6];
    bool run;                // the thumb is past the stick's rim: running (R held)
    uint64_t run_idle;
    float fx, fy;            // the stick's thumb where it is (the knob stops at the rim)
    bool lock;               // the lock-on: L is pressed every LOCK_EVERY while the hunt camera turns
    uint64_t lock_next, lock_until, hunt_ns;
    uint64_t item_flash, art_flash;
    int item_dir, art_row;
    int drawn;               // weapon_drawn, at the last touch_input
    int weapon;              // WEAPON_* in hand, whatever the layout, at the last touch_input (the lock-on's)
    int stick_part;          // where the hunter isn't played, in a menu: the direction the stick is pressing (dpad_part)
    bool stick_menu_ok;      // ...which it may: the thumb landed or came back to the middle since the hunter was played
    bool stick_cursor;       // a town's menu where the hunter doesn't walk: the stick moves the cursor there too
    uint64_t stuck_ns;       // ...found by the stick tilted since then and the player's place not changed (0: not tilted)
    float stuck_x, stuck_z;
    uint64_t pad_held;       // the menu pad's buttons down (its fingers' sent)
    uint64_t menu_on_ns;     // when the menu opened (the pad comes in)
    int lit_ctl, lit_part;   // a quick tap of the menu pad's group pressing now (lit until lit_until)
    uint64_t lit_until;
    uint64_t ammo_ready, ammo_hold;  // the next press of the ammo may start then; L held for it until then
    uint64_t assist_until, assist_fire;  // a locked shot: R held so far (the aim), A until then
    uint64_t assist_t0;                  // (when it began to wait for the aim)
    bool assist_wait, fired;             // it waits for the aim; the fire button was down
    float w, h;              // the screen, pixels
    Layout layout, saved;
    bool loaded, touched, auto_wait;
    bool menu;  // a menu of the game's is open (game_menu, at the last touch_input)  // (auto_wait: a desktop's Auto, waiting for the first finger)
    bool edit, edit_request;  // the layout editor
    int edit_prof;            // ...the HUD it shows and edits (HUD_*: the switch)
    int sel;
    bool pinch;
    float pinch_d0, pinch_s0;
#ifndef __ANDROID__
    // The HUD editor: open, asked for; the panel's button held down, the part being dragged (HP_*; -1 none); the
    // pointer (window pixels); the drag's start: the pointer, the part's offset and its frame; the finger that is the
    // pointer on a touch screen, and one it let go of (the editor closed or the port's menu came over it while it was
    // down: the rest of it is nobody's, and no touch for the touch controls)
    bool hud_edit, hud_request;
    int hud_btn, hud_drag;
    float hud_px, hud_py, hud_x0, hud_y0, hud_ox, hud_oy;
    Rect hud_fr;
    bool hud_fingered, hud_lifting;
    SDL_FingerID hud_finger, hud_lift;
#endif
};
std::mutex g_lock;
State g_s;
std::atomic<bool> g_lock_follow;  // (touch_lock_buttons -> the hunt camera: follow the monster now)
std::atomic<bool> g_lock_shots;   // (the lock-on is on, in a hunt: hle/mousecam.c turns a bow's or a gun's shots)
std::atomic<bool> g_lock_mark;    // (the lock-on is on, no menu of the game's open: its padlock shows, the controls
                                  // hidden too; touch_lock_buttons -> touch_drawn)
std::atomic<bool> g_game_menu;    // (s.menu, for the keys: touch_game_menu)
uint64_t g_menu_buttons;          // (the SDL main thread's: what the menu pad presses, touch_menu_buttons)
std::atomic<uint64_t> g_assist_until;  // (a locked shot holds R for its aim until then: SDL_GetTicksNS)
// a click on the phone's vibrator, asked for under g_lock and made once it is released (a JNI / binder call: the
// game thread's touch_draw waits on that lock); the SDL main thread's alone
bool g_tap_due;
void tap_due(void) { g_tap_due = true; }
struct TapAfterUnlock {
    ~TapAfterUnlock() {
        if (g_tap_due) g_tap_due = false, haptics_tap();
    }
};
std::atomic<bool> g_visible;
std::atomic<bool> g_gear_only;  // (the controls hidden: only the settings button shows, and answers)
#ifndef __ANDROID__
std::atomic<bool> g_hud_edit;   // (s.hud_edit, for the game thread: touch_drawn, touch_draw)
std::atomic<bool> g_hud_hold;   // (the left button that closed it is still down: the rest of that press is its own)
std::atomic<bool> g_hud_asked;  // (s.hud_edit, s.hud_request or g_hud_hold: touch_hud_editing, the mouse kept free)
void hud_asked(const State &s) { g_hud_asked = s.hud_edit || s.hud_request || g_hud_hold; }
std::atomic<bool> g_hud_pad;    // (a controller is read: its B, which cancels the editor, shows on Cancel once used)
#endif
const uint64_t LOCK_EVERY = 250 * MS, LOCK_PRESS = 50 * MS, LOCK_AFTER_DRAG = 400 * MS, MIN_PRESS = 70 * MS;

void ensure_loaded(State &s) {
    if (s.loaded) return;
    s.loaded = true, s.sel = -1;
    g_scheme = config_live_touch_scheme();
    bool had = load_layout(&s.layout);
    s.saved = s.layout;
    publish_hud(s.layout);
    g_style = s.layout.style;
    rt_logf("[touch] layout: %s\n", had ? layout_file() : "the defaults");
}

// (menu: the menu's own, dropped as it closes; fresh: a press that starts, which press_ok must let through; ctl, part:
// a tap of the menu pad's group, lit while it presses)
void pulse(State &s, uint64_t buttons, uint64_t ns, uint64_t after = 0, bool menu = false, bool fresh = false, int ctl = -1,
           int part = 0) {
    uint64_t now = SDL_GetTicksNS();
    for (Pulse &p : s.pulse)
        if (p.until <= now) {
            p.buttons = buttons, p.from = now + after, p.until = now + after + ns;
            p.menu = menu, p.fresh = fresh, p.checked = false, p.ctl = ctl, p.part = part;
            return;
        }
}
// A step of the ammo or the coating: L held, X the next one, B the one before, and L let go confirms it (the game's own).
// The game takes L only after a while: X 220 ms after it reloaded or did nothing, 310 ms after it changed the ammo (a
// Motorola g06). More steps while L is held need no wait
void ammo_step(State &s, int dir) {
    uint64_t now = SDL_GetTicksNS();
    uint64_t at = now < s.ammo_hold ? std::max(now + 20 * MS, s.ammo_ready) : now + 380 * MS;
    pulse(s, dir > 0 ? B(BTN_X) : B(BTN_B), 80 * MS, at - now);
    s.ammo_ready = at + 170 * MS;
    s.ammo_hold = at + 320 * MS;
}
// The item bar's arrows ([Touch] ShowItemArrows): beside the bar, wherever it has been moved to
Geo item_arrow(const Rect &item, int d, float h) {
    float r = .052f * h;
    return {ImVec2(d < 0 ? item.x0 - r * 1.15f : item.x1 + r * 1.15f, (item.y0 + item.y1) * .5f), r};
}

// The stick is fixed at its place (the layout's: the editor moves it); a thumb on its base or a little past its rim
// takes it, not on the item bar
bool in_stick_zone(const State &s, float x, float y, const Rect &item) {
    Geo g = place(s.layout, C_STICK, s.w, s.h);
    if (item.in(x, y)) return false;
    return hypotf(x - g.c.x, y - g.c.y) < std::max(1.45f * g.r, .17f * s.h);
}

void stick_to(State &s, float x, float y) {  // (the knob under the thumb, kept inside the rim)
    s.fx = x, s.fy = y;
    float dx = x - s.bx, dy = y - s.by, d = sqrtf(dx * dx + dy * dy);
    float r = DEFS[C_STICK].r * s.layout.scale[C_STICK] * s.h;
    if (d > r) x = s.bx + dx / d * r, y = s.by + dy / d * r;
    s.kx = x, s.ky = y;
}

Finger *finger(State &s, SDL_FingerID id) {
    for (Finger &f : s.f)
        if (f.kind != F_NONE && f.id == id) return &f;
    return nullptr;
}

// What one of the menu pad's group presses, given what it presses now (sent): its direction; a page tab the pair the
// game turns its pages with (its option: L / R, or ZL / ZR), the other tabs the other pair; Minus
uint64_t mpad_buttons(int ctl, int sent) {
    if (!sent) return 0;
    if (ctl == C_MPAD) return dpad_buttons(sent);
    if (ctl == C_MMINUS) return B(BTN_MINUS);
    bool z = menu_pages_zl() == (ctl == C_ML || ctl == C_MR), left = ctl == C_ML || ctl == C_MZL;
    return left ? B(z ? BTN_ZL : BTN_L) : B(z ? BTN_ZR : BTN_R);
}
const char *mpad_label(int ctl) {  // (a tab: what it presses)
    uint64_t b = mpad_buttons(ctl, 1);
    return b & B(BTN_ZL) ? "ZL" : b & B(BTN_ZR) ? "ZR" : b & B(BTN_L) ? "L" : b & B(BTN_R) ? "R" : "-";
}
int mpad_want(const Finger &f) { return f.ctl == C_MPAD ? f.part : 1; }  // (what its finger asks for now)

uint64_t finger_buttons(const Finger &f) {
    return f.ctl == C_ATTACK || f.ctl == C_CORNER ? PILL_BUTTONS[f.part]
         : of_mpad(f.ctl) ? mpad_buttons(f.ctl, f.sent)
         : is_dpad(f.ctl) ? dpad_buttons(f.part)
         : is_combo(f.ctl) ? (uint64_t)config_live_touch_combo(f.ctl - C_COMBO)
         : f.ctl == C_LIGHT && f.part ? PILL_BUTTONS[2]
         : f.btn ? f.btn
                 : DEFS[f.ctl].buttons;
}

void recompute_held(State &s) {  // (the menu pad's go by s.pad_held: touch_input, as a press may start)
    s.held = 0;
    for (Finger &f : s.f)
        if (f.kind == F_BUTTON && !f.done && !of_mpad(f.ctl)) s.held |= finger_buttons(f);
    s.tapped |= s.held;
}

// Every finger let go of, nothing held or waiting (the app to the background, an editor opened): no old direction of
// the menu pad's reaches a menu afterwards
void drop_fingers(State &s) {
    for (Finger &f : s.f) f.kind = F_NONE;
    for (Pulse &p : s.pulse) p.until = 0;
    s.stick_on = false, s.held = s.pad_held = s.tapped = 0, s.stick_part = 0, s.pinch = false;
}

// the port's settings menu takes the fingers as its mouse (overlay.cpp)
void to_menu(const SDL_Event *e) {
    SDL_Window *win = ui_window();
    int pw = 0, ph = 0;
    if (win) SDL_GetWindowSize(win, &pw, &ph);
    SDL_Event m;
    SDL_zero(m);
    float x = e->tfinger.x * pw, y = e->tfinger.y * ph;
    m.motion.type = SDL_EVENT_MOUSE_MOTION, m.motion.x = x, m.motion.y = y;
    overlay_event(&m);
    if (e->type == SDL_EVENT_FINGER_CANCELED) {  // (taken by the system: released away from everything, no click)
        m.motion.x = m.motion.y = -10000.0f;
        overlay_event(&m);
        x = y = -10000.0f;
    }
    if (e->type != SDL_EVENT_FINGER_MOTION) {
        SDL_zero(m);
        bool down = e->type == SDL_EVENT_FINGER_DOWN;
        m.button.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        m.button.button = SDL_BUTTON_LEFT, m.button.down = down, m.button.clicks = 1;
        m.button.x = x, m.button.y = y;
        overlay_event(&m);
    }
}

// ---- the layout editor: the touch controls and the game's HUD. A panel in two rows: what is picked (smaller, bigger,
// hide or show a part, back to its default) and the rest (the melee or ranged layout, the attacks' style, which HUD it
// shows - the touch controls' or, while MoveHUD is On, the one without them -, defaults, cancel, save). Drag a control or
// a part to move it, pinch (a second finger anywhere) or the panel's - / + to size it
enum { E_SMALLER, E_BIGGER, E_HIDE, E_RESET, E_SET, E_STYLE, E_PROF, E_DEFAULTS, E_CANCEL, E_SAVE, E_COUNT };
void editor_panel(float w, float h, Rect *panel, Rect btn[E_COUNT]) {
    float pw = std::min(w * .94f, 1.9f * h), ph = .28f * h, x0 = (w - pw) * .5f, y0 = (h - ph) * .4f;
    *panel = {x0, y0, x0 + pw, y0 + ph, true};
    float gap = .012f * h, bh = .082f * h, r1 = y0 + .1f * h, r2 = r1 + bh + gap, bw = (pw - gap * 5) / 4;
    for (int i = 0; i < E_SET; i++) btn[i] = {x0 + gap + i * (bw + gap), r1, x0 + gap + i * (bw + gap) + bw, r1 + bh, true};
    bool two = config_live_touch_hud() != 0;  // (MoveHUD Off: one HUD for all, no switch)
    int n = two ? E_COUNT - E_SET : E_COUNT - E_SET - 1, j = 0;
    bw = (pw - gap * (n + 1)) / n;
    for (int i = E_SET; i < E_COUNT; i++) {
        btn[i] = {0, 0, 0, 0, false};
        if (i == E_PROF && !two) continue;
        btn[i] = {x0 + gap + j * (bw + gap), r2, x0 + gap + j * (bw + gap) + bw, r2 + bh, true};
        j++;
    }
}

// [Touch] Layout changed: the other layout and its file; the fingers on what it lacks let go
void switch_scheme(State &s, int scheme) {
    g_scheme = scheme;
    bool had = load_layout(&s.layout);
    s.saved = s.layout;
    publish_hud(s.layout);
    g_style = s.layout.style;
    s.sel = -1;
    if (scheme == TS_TRADITIONAL) g_ranged = 0, s.menu = false;
    for (Finger &f : s.f)
        if (f.kind == F_BUTTON && !in_mode(f.ctl, s.menu)) f.done = true;
    recompute_held(s);
    rt_logf("[touch] layout %s: %s\n", scheme == TS_TRADITIONAL ? "Traditional" : "Adaptive", had ? layout_file() : "the defaults");
}

void edit_begin(State &s) {
    s.edit = true, s.edit_request = false, s.sel = -1, s.pinch = false;
    g_edit_set = !trad() && g_ranged.load() != 0;  // (the layout in use; the panel's button changes it)
    s.edit_prof = config_live_touch_hud() ? HUD_TOUCH : HUD_PLAIN;  // (the HUD in use with the touch controls)
    g_hud_edit_prof = s.edit_prof;
    s.saved = s.layout;
    drop_fingers(s);
    rt_logf("[touch] layout editor\n");
}
void edit_end(State &s, bool keep) {
    if (keep) save_layout(s.layout), s.saved = s.layout;
    else s.layout = s.saved;
    publish_hud(s.layout);
    g_style = s.layout.style;
    g_edit_set = -1;
    g_hud_edit_prof = -1;
    s.edit = false, s.sel = -1, s.pinch = false;
}
// The next way of laying out the attacks. What the corner band would cover goes inside its bend; leaving it, back to
// where it was when the editor opened
void next_style(State &s) {
    int style = (s.layout.style + 1) % STYLE_COUNT;
    s.layout.style = style, g_style = style;
    static const struct { int c; float at; } MOVED[2] = {{C_DODGE, .33f}, {C_SHEATHE, .5f}};
    for (const auto &m : MOVED) {
        Layout before = s.saved.style == STYLE_CORNER ? defaults() : s.saved;
        s.layout.x[m.c] = before.x[m.c], s.layout.y[m.c] = before.y[m.c];
        if (style != STYLE_CORNER) continue;
        Geo band = place(s.layout, C_CORNER, s.w, s.h), o = place(s.layout, m.c, s.w, s.h);
        if (band_distance(band, o.c.x, o.c.y, nullptr) < band.r * BAND_T + o.r)
            s.layout.x[m.c] = s.layout.y[m.c] = m.at;
    }
    if (s.sel >= 0 && s.sel < C_COUNT && !in_editor(s.sel)) s.sel = -1;
}
const int ZONE_SEL = 100;  // (s.sel / a finger's ctl: a HUD part, ZONE_SEL + its HP_*)

void clamp_place(State &s, int i) {
    float wmax = s.w / s.h;
    s.layout.scale[i] = std::clamp(s.layout.scale[i], i == C_CORNER ? .7f : .5f, i == C_CORNER ? 1.3f : 2.2f);
    if (i == C_MPAD) {  // (its whole group on the screen: the tabs above it, Minus below, as place() draws it)
        float k = s.layout.scale[i], hw = MPAD_HALF_W * k;
        s.layout.x[i] = std::clamp(s.layout.x[i], hw, std::max(hw, wmax - hw));
        s.layout.y[i] = std::clamp(s.layout.y[i], MPAD_BELOW * k, std::max(MPAD_BELOW * k, 1.0f - mpad_above() * k));
        return;
    }
    s.layout.x[i] = std::clamp(s.layout.x[i], .03f, wmax - .03f);
    s.layout.y[i] = std::clamp(s.layout.y[i], .03f, .97f);
}

// The part under a finger in the editor: of the frames within a little of it, the smallest (the target camera over the
// minimap); -1 none
int part_hit(const State &s, float x, float y) {
    int best = -1;
    float area = 1e30f, m = .012f * s.h;
    for (int k = 0; k < HP_COUNT; k++) {
        bool live;
        Rect r = part_frame(k, s.edit_prof, s.w, s.h, &live);
        if (x < r.x0 - m || x > r.x1 + m || y < r.y0 - m || y > r.y1 + m) continue;
        float a = (r.x1 - r.x0) * (r.y1 - r.y0);
        if (a < area) area = a, best = k;
    }
    return best;
}
// A part dragged by (dx, dy) pixels, 1:1 on any shape of picture (in canvas units through the squeezes: hud_parts.h), its
// frame kept on the picture (one already past an edge may come back)
void part_move(State &s, int k, float dx, float dy) {
    HudVals &v = s.layout.hud[s.edit_prof][k];
    Pic p = picture(s.w, s.h);
    bool live;
    Rect r = part_frame(k, s.edit_prof, s.w, s.h, &live);
    dx = std::clamp(dx, std::min(0.0f, p.x0 - r.x0), std::max(0.0f, p.x0 + p.w - r.x1));
    dy = std::clamp(dy, std::min(0.0f, p.y0 - r.y0), std::max(0.0f, p.y0 + p.h - r.y1));
    v.dx = std::clamp(v.dx + dx * 1280.0f / p.w / hud_squeeze(), -HUD_DX_MAX, HUD_DX_MAX);
    v.dy = std::clamp(v.dy + dy * 720.0f / p.h / hud_vsqueeze(), -HUD_DY_MAX, HUD_DY_MAX);
    publish_hud(s.layout);
}
// A part sized (whole percents: back at 100 it is the game's size), and moved back onto the picture if it now leaves it
void part_size(State &s, int k, float to) {
    HudVals &v = s.layout.hud[s.edit_prof][k];
    v.k = std::clamp(roundf(to * 100.0f) / 100.0f, HUD_SIZE_MIN, HUD_SIZE_MAX);
    publish_hud(s.layout);
    Pic p = picture(s.w, s.h);
    bool live;
    Rect r = part_frame(k, s.edit_prof, s.w, s.h, &live);
    float dx = r.x1 - r.x0 > p.w ? 0 : r.x0 < p.x0 ? p.x0 - r.x0 : r.x1 > p.x0 + p.w ? p.x0 + p.w - r.x1 : 0;
    float dy = r.y1 - r.y0 > p.h ? 0 : r.y0 < p.y0 ? p.y0 - r.y0 : r.y1 > p.y0 + p.h ? p.y0 + p.h - r.y1 : 0;
    if (dx != 0.0f || dy != 0.0f) part_move(s, k, dx, dy);
}
// A part picked up: an offset the render thread had to move to keep its middle on the picture (saved on another shape of
// window, or with another HUD anchor) is taken in, so that it stays where it shows (the shift is spent: the next frame
// tells the new one)
void part_pickup(State &s, int k) {
    if (g_hud_prof_now != s.edit_prof) return;
    HudVals &v = s.layout.hud[s.edit_prof][k];
    float fx = g_hud_shift_dx[k], fy = g_hud_shift_dy[k];
    bool custom = v.dx != 0.0f || v.dy != 0.0f || v.k != 1.0f || v.hidden;
    if (!custom || !std::isfinite(fx) || !std::isfinite(fy) || (fx == 0.0f && fy == 0.0f)) return;
    v.dx = std::clamp(v.dx + fx, -HUD_DX_MAX, HUD_DX_MAX), v.dy = std::clamp(v.dy + fy, -HUD_DY_MAX, HUD_DY_MAX);
    g_hud_shift_dx[k] = 0.0f, g_hud_shift_dy[k] = 0.0f;
    publish_hud(s.layout);
}
// The panel's buttons. Reset: what is picked, back to its default; Defaults: the touch controls (while their HUD shows)
// and the HUD shown, the other HUD kept; the HUD switch: the touch controls' or the one without them, the game showing
// it live (g_hud_edit_prof)
void edit_button(State &s, int i) {
    int part = s.sel >= ZONE_SEL ? s.sel - ZONE_SEL : -1, c = s.sel >= 0 && s.sel < C_COUNT ? s.sel : -1;
    bool touch = s.edit_prof == HUD_TOUCH;
    switch (i) {
    case E_SMALLER:
    case E_BIGGER: {
        float by = i == E_BIGGER ? .1f : -.1f;
        if (part >= 0) part_size(s, part, s.layout.hud[s.edit_prof][part].k + by);
        else if (c >= 0) s.layout.scale[c] += by, clamp_place(s, c);
        break;
    }
    case E_HIDE:  // (not the item bar with the touch controls: hud_hide_ok)
        if (part >= 0 && hud_hide_ok(s.edit_prof, part)) {
            s.layout.hud[s.edit_prof][part].hidden = !s.layout.hud[s.edit_prof][part].hidden;
            publish_hud(s.layout);
        }
        break;
    case E_RESET:
        if (part >= 0) s.layout.hud[s.edit_prof][part] = hud_default(), publish_hud(s.layout);
        else if (c >= 0) {  // (the layout in use's own default: Traditional's differs)
            Layout d = defaults();
            s.layout.x[c] = d.x[c], s.layout.y[c] = d.y[c], s.layout.scale[c] = d.scale[c];
        }
        break;
    case E_SET:
        if (!touch || trad()) break;  // (Traditional has no ranged layout)
        g_edit_set = g_edit_set.load() == 1 ? 0 : 1;
        if (part < 0) s.sel = -1;
        break;
    case E_STYLE:
        if (touch && !ranged_layout() && !trad()) next_style(s);
        break;
    case E_PROF:
        s.edit_prof = touch ? HUD_PLAIN : HUD_TOUCH, g_hud_edit_prof = s.edit_prof;
        if (part < 0) s.sel = -1;  // (a control: none in the HUD without them)
        rt_logf("[touch] layout editor: the HUD %s the touch controls\n", touch ? "without" : "with");
        break;
    case E_DEFAULTS: {
        Layout was = s.layout;
        if (touch) s.layout = defaults();
        for (int p = 0; p < HUD_PROFILES; p++)
            for (int k = 0; k < HP_COUNT; k++) s.layout.hud[p][k] = p == s.edit_prof ? hud_default() : was.hud[p][k];
        publish_hud(s.layout);
        g_style = s.layout.style;
        break;
    }
    default:
        edit_end(s, i == E_SAVE);
    }
}

void edit_event(State &s, const SDL_Event *e, float x, float y) {
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        Finger *f = nullptr;
        for (Finger &g : s.f)
            if (g.kind == F_NONE) { f = &g; break; }
        if (!f) return;
        *f = {};
        f->id = e->tfinger.fingerID, f->x = f->x0 = x, f->y = f->y0 = y, f->ctl = -1;
        Rect panel, btn[E_COUNT];
        editor_panel(s.w, s.h, &panel, btn);
        for (int i = 0; i < E_COUNT; i++)
            if (btn[i].in(x, y)) {
                f->kind = F_MENU;  // (held until it lifts: nothing under it moves)
                edit_button(s, i);
                if (!s.edit) f->kind = F_NONE;
                return;
            }
        Finger *drag = nullptr;
        for (Finger &g : s.f)
            if (&g != f && g.kind == F_EDIT && g.ctl >= 0) drag = &g;
        if (drag) {  // a second finger: pinch what the first one drags, a control or a part
            s.pinch = true, s.pinch_d0 = std::max(1.0f, hypotf(x - drag->x, y - drag->y));
            s.pinch_s0 = drag->ctl >= ZONE_SEL ? s.layout.hud[s.edit_prof][drag->ctl - ZONE_SEL].k : s.layout.scale[drag->ctl];
            f->kind = F_EDIT;
            return;
        }
        // a control (with the touch controls' HUD), else a part of the game's HUD, else the panel's face (nothing moves)
        int c = s.edit_prof == HUD_TOUCH ? hit(s.layout, x, y, s.w, s.h, nullptr, 1.5f, true, false, true) : -1;
        if (c < 0) {
            int k = part_hit(s, x, y);
            if (k >= 0) c = ZONE_SEL + k;
        }
        if (c < 0 && panel.in(x, y)) { f->kind = F_MENU; return; }
        f->kind = F_EDIT, f->ctl = c;
        s.sel = c;
        if (c >= ZONE_SEL) part_pickup(s, c - ZONE_SEL);
        return;
    }
    Finger *f = finger(s, e->tfinger.fingerID);
    if (!f) return;
    if (e->type == SDL_EVENT_FINGER_MOTION) {
        float dx = x - f->x, dy = y - f->y;
        f->x = x, f->y = y;
        if (f->kind != F_EDIT) return;
        if (s.pinch) {
            Finger *a = nullptr, *b = nullptr;
            for (Finger &g : s.f)
                if (g.kind == F_EDIT) (a ? b : a) = &g;
            int c = a && a->ctl >= 0 ? a->ctl : b && b->ctl >= 0 ? b->ctl : -1;
            if (a && b && c >= 0) {
                float by = hypotf(a->x - b->x, a->y - b->y) / s.pinch_d0;
                if (c >= ZONE_SEL) part_size(s, c - ZONE_SEL, s.pinch_s0 * by);
                else s.layout.scale[c] = s.pinch_s0 * by, clamp_place(s, c);
            }
        } else if (f->ctl >= ZONE_SEL) {
            part_move(s, f->ctl - ZONE_SEL, dx, dy);
        } else if (f->ctl >= 0) {
            s.layout.x[f->ctl] += (from_right(f->ctl) ? -dx : dx) / s.h;
            s.layout.y[f->ctl] -= dy / s.h;
            clamp_place(s, f->ctl);
        }
        return;
    }
    if (f->kind == F_EDIT) s.pinch = false;
    f->kind = F_NONE;
}

#ifndef __ANDROID__
// ---- the HUD editor (a desktop's, with the mouse): every part of the HUD the touch layout editor offers (the clock,
// health and stamina, the sharpness and gauges, the party, the minimap, the target camera, the item bar, the Hunter
// Arts), in the HUD shown with the touch controls hidden (HUD_PLAIN: a keyboard, a controller; the touch editor's
// HUD: controller), from where the game puts them. Framed and named as in the touch editor, where they are drawn or
// where they would be (outside a hunt, hidden: part_frame); a part dragged moves, the wheel over it sizes it. A panel
// in one row: what is picked (- / +, hide or show, back to its default), then defaults (every part), cancel and save;
// Esc cancels
enum { H_SMALLER, H_BIGGER, H_HIDE, H_RESET, H_DEFAULTS, H_CANCEL, H_SAVE, H_COUNT };
void hud_panel(float w, float h, Rect *panel, Rect btn[H_COUNT]) {
    float pw = std::min(w * .94f, 1.75f * h), ph = .2f * h, x0 = (w - pw) * .5f, y0 = (h - ph) * .4f;
    *panel = {x0, y0, x0 + pw, y0 + ph, true};
    // (the picked part's four, then the rest a little apart)
    float gap = .015f * h, split = 2.0f * gap, bw = (pw - gap * (H_COUNT + 1) - split) / H_COUNT;
    float by0 = y0 + ph * .5f, by1 = y0 + ph - gap;
    for (int i = 0; i < H_COUNT; i++) {
        float bx = x0 + gap + i * (bw + gap) + (i >= H_DEFAULTS ? split : 0.0f);
        btn[i] = {bx, by0, bx + bw, by1, true};
    }
}
// The part under the pointer (window pixels, on a w x h window): of the frames within a little of it, the smallest (the
// target camera over the minimap), as the touch editor's part_hit; none over the panel's buttons
int hud_over(float x, float y, float w, float h) {
    Rect panel, btn[H_COUNT];
    hud_panel(w, h, &panel, btn);
    for (int i = 0; i < H_COUNT; i++)
        if (btn[i].in(x, y)) return -1;
    int best = -1;
    float area = 1e30f, m = .012f * h;
    for (int k = 0; k < HP_COUNT; k++) {
        bool live;
        Rect r = part_frame(k, HUD_PLAIN, w, h, &live);
        if (x < r.x0 - m || x > r.x1 + m || y < r.y0 - m || y > r.y1 + m) continue;
        float a = (r.x1 - r.x0) * (r.y1 - r.y0);
        if (a < area) area = a, best = k;
    }
    return best;
}
void hud_cursor(bool move) {  // (the pointer over a part: the system's four arrows)
    static SDL_Cursor *arrows;
    static bool on;
    if (move == on) return;
    if (move && !arrows) arrows = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE);
    on = move;
    SDL_SetCursor(move && arrows ? arrows : SDL_GetDefaultCursor());
}
// The finger working it let go of (the editor closed, or the port's menu came over it, while it was down)
void hud_let_go(State &s) {
    if (s.hud_fingered) s.hud_lifting = true, s.hud_lift = s.hud_finger, s.hud_fingered = false;
}
void hud_hold_end(State &s) {  // (the press that closed it is up: the mouse is the game's again)
    g_hud_hold = false;
    hud_asked(s);
}
void hud_edit_begin(State &s) {
    s.hud_request = false;
    if (s.hud_edit || s.edit) {  // (open already, or the touch layout editor is: one at a time, nothing of it lost)
        hud_asked(s);
        return;
    }
    s.hud_edit = true, s.sel = -1, s.hud_btn = s.hud_drag = -1, s.hud_fingered = false;
    // (the HUD without the touch controls, shown live as it is edited: part_size and part_pickup go by edit_prof)
    s.edit_prof = HUD_PLAIN, g_hud_edit_prof = HUD_PLAIN;
    s.saved = s.layout;
    drop_fingers(s);
    s.hud_px = s.hud_py = -1e9f;  // (where the pointer is: the button under it lights at once)
    SDL_Window *win = ui_window();
    int ww = 0, wh = 0, pw = 0, ph = 0;
    float mx = 0, my = 0;
    if (win && SDL_GetWindowSize(win, &ww, &wh) && SDL_GetWindowSizeInPixels(win, &pw, &ph) && ww > 0 && wh > 0) {
        SDL_GetMouseState(&mx, &my);
        s.w = (float)pw, s.h = (float)ph, s.hud_px = mx * pw / ww, s.hud_py = my * ph / wh;
    }
    g_hud_edit = true;
    hud_asked(s);
    rt_logf("[touch] HUD editor\n");
}
void hud_edit_end(State &s, bool keep) {
    if (keep) save_layout(s.layout), s.saved = s.layout;
    else s.layout = s.saved;
    publish_hud(s.layout);
    g_hud_edit_prof = -1;
    s.hud_edit = false, s.sel = -1, s.hud_btn = s.hud_drag = -1;
    hud_let_go(s);  // (the finger that tapped Save or Cancel: its lift is no touch on the game)
    // (the left button that pressed Save or Cancel, or held through Esc: the rest of that press stays the editor's, no
    // touch for Forge's plugins and no capture of the mouse until it is up)
    g_hud_hold = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
    g_hud_edit = false;
    hud_asked(s);
    hud_cursor(false);
}
// The panel's buttons: - / + (10% a click), Hide / Show and Reset for the picked part; Defaults puts every part back
// where the game has it, at its size, shown (the touch controls' HUD kept); Cancel, Save
void hud_button(State &s, int i) {
    int k = s.sel >= ZONE_SEL ? s.sel - ZONE_SEL : -1;
    HudVals *v = k >= 0 ? &s.layout.hud[HUD_PLAIN][k] : nullptr;
    switch (i) {
    case H_SMALLER:
    case H_BIGGER:
        if (v) part_size(s, k, v->k + (i == H_BIGGER ? .1f : -.1f));
        break;
    case H_HIDE:
        if (v) v->hidden = !v->hidden, publish_hud(s.layout);
        break;
    case H_RESET:
        if (v) *v = hud_default(), publish_hud(s.layout);
        break;
    case H_DEFAULTS:
        for (HudVals &d : s.layout.hud[HUD_PLAIN]) d = hud_default();
        publish_hud(s.layout);
        break;
    default:
        hud_edit_end(s, i == H_SAVE);
    }
}
void hud_press(State &s, float x, float y) {
    Rect panel, btn[H_COUNT];
    hud_panel(s.w, s.h, &panel, btn);
    s.hud_btn = s.hud_drag = -1;
    for (int i = 0; i < H_COUNT; i++)
        if (btn[i].in(x, y)) {
            s.hud_btn = i;  // (lit while held)
            hud_button(s, i);
            return;
        }
    int k = hud_over(x, y, s.w, s.h);
    if (k < 0 && panel.in(x, y)) return;  // (the panel's face: nothing under it moves)
    s.sel = k >= 0 ? ZONE_SEL + k : -1;
    if (k < 0) return;
    // (a part the render thread keeps on the picture, its offset from another shape of window or HUD anchor: picked
    // up where it shows, and saved so)
    part_pickup(s, k);
    HudVals &v = s.layout.hud[HUD_PLAIN][k];
    bool live;
    s.hud_drag = k, s.hud_x0 = x, s.hud_y0 = y, s.hud_ox = v.dx, s.hud_oy = v.dy;
    s.hud_fr = part_frame(k, HUD_PLAIN, s.w, s.h, &live);
}
// The pointer moved: the part being dragged follows it from where it was picked up, in the canvas' units as in the
// touch layout editor, its frame kept on the picture (one already past an edge may come back; one wider or taller
// than the picture, sized up: its middle)
void hud_move(State &s, float x, float y) {
    s.hud_px = x, s.hud_py = y;
    if (s.hud_drag < 0) {
        hud_cursor(s.hud_btn < 0 && hud_over(x, y, s.w, s.h) >= 0);
        return;
    }
    int k = s.hud_drag;
    Pic p = picture(s.w, s.h);
    Rect r = s.hud_fr;
    if (r.x1 - r.x0 > p.w) r.x0 = r.x1 = (r.x0 + r.x1) * .5f;
    if (r.y1 - r.y0 > p.h) r.y0 = r.y1 = (r.y0 + r.y1) * .5f;
    float dx = std::clamp(x - s.hud_x0, std::min(0.0f, p.x0 - r.x0), std::max(0.0f, p.x0 + p.w - r.x1));
    float dy = std::clamp(y - s.hud_y0, std::min(0.0f, p.y0 - r.y0), std::max(0.0f, p.y0 + p.h - r.y1));
    HudVals &v = s.layout.hud[HUD_PLAIN][k];
    v.dx = std::clamp(s.hud_ox + dx * 1280.0f / p.w / hud_squeeze(), -HUD_DX_MAX, HUD_DX_MAX);
    v.dy = std::clamp(s.hud_oy + dy * 720.0f / p.h / hud_vsqueeze(), -HUD_DY_MAX, HUD_DY_MAX);
    publish_hud(s.layout);
}
void hud_release(State &s) { s.hud_btn = s.hud_drag = -1; }
void hud_wheel(State &s, float x, float y, int notches) {  // (over a part, or else the picked one: 5% a notch)
    if (!notches || s.hud_drag >= 0) return;  // (not while one is dragged: its frame at the drag's start holds)
    int k = hud_over(x, y, s.w, s.h);
    if (k < 0 && s.sel >= ZONE_SEL) k = s.sel - ZONE_SEL;
    if (k < 0) return;
    s.sel = ZONE_SEL + k;
    part_size(s, k, s.layout.hud[HUD_PLAIN][k].k + notches * .05f);
}
// A touch screen: its first finger is the pointer, while the editor is open and the port's menu is not over it
void hud_finger(State &s, const SDL_Event *e, float x, float y) {
    SDL_FingerID id = e->tfinger.fingerID;
    bool up = e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED;
    if (s.hud_lifting && id == s.hud_lift) {  // (one it let go of: nothing more of it)
        if (up) s.hud_lifting = false;
        return;
    }
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        if (s.hud_fingered || !s.hud_edit || overlay_menu_open()) return;
        s.hud_fingered = true, s.hud_finger = id;
        s.hud_px = x, s.hud_py = y;
        hud_press(s, x, y);
        return;
    }
    if (!s.hud_fingered || id != s.hud_finger) return;
    if (!s.hud_edit || overlay_menu_open()) {  // (closed, or under the menu now: let go of)
        hud_let_go(s), hud_release(s);
        if (up) s.hud_lifting = false;
        return;
    }
    hud_move(s, x, y);
    if (up) s.hud_fingered = false, hud_release(s), s.hud_px = s.hud_py = -1e9f;
}
#endif

}  // namespace

// ------------------------------------------------------------------ the Join Hub screen's list buttons
// A floating bar low in the middle of the picture: "<  1  2  3  >" with "hubs #25 to 48 of 55" over it. A tap on a number chooses that list,
// the arrows the one before / after; the search follows by itself (runtime/host_input.c presses Retry a moment later).
// (Sized and placed by the picture, not by the game's table, so it fits every window shape; colours: [Application] Theme.)
bool hub_strip_on() { return ldn_hub_browsing() && ldn_hub_lists() > 1; }
int hub_strip_count() { return std::min(ldn_hub_lists(), 8) + 2; }  // (the arrows and the numbers)
// With more than 8 lists (up to 28 for 500 hubs) the 8 numbers are a window that follows the chosen list.
int hub_strip_first() { return std::clamp(ldn_hub_list_chosen() - 3, 0, std::max(0, ldn_hub_lists() - 8)); }
// Sized by the game's picture (picture(): its height, and centred in it, whatever shape the window is), not by the
// game's table, so it sits the same on 16:9, 21:9, 4:3 ... It floats low in the middle, clear of the thumbs' buttons
// and the game's text box, with a line under the table's reach saying which hubs the list holds.
struct HubGeo {
    float px, py, pw, ph;  // the bar
    float bh, bw, pitch, bx, by, gap;  // a button, the pitch between them, the first one
    float u;               // the picture's height (scaled down when the bar would be too wide)
    float cy, ch;          // the caption's middle and height
};
HubGeo hub_geo(float w, float h) {
    HubGeo g;
    Pic p = picture(w, h);
    int cnt = hub_strip_count();
    float u = p.h, bw = .072f * u, gap = .014f * u, pad = .022f * u;
    float inner = cnt * bw + (cnt - 1) * gap, full = inner + 2 * pad;
    float k = std::min(1.0f, .56f * p.w / full);  // (a narrow picture: smaller)
    u *= k, bw *= k, gap *= k, pad *= k, inner *= k, full *= k;
    g.u = u, g.bw = bw, g.bh = bw, g.gap = gap, g.pitch = bw + gap;
    g.pw = full, g.ph = bw + 2 * pad;
    g.px = p.x0 + (p.w - full) * .5f;
    g.py = p.y0 + .735f * p.h;
    g.bx = g.px + pad, g.by = g.py + pad;
    g.ch = .062f * u;
    g.cy = g.py - .014f * u - g.ch * .5f;
    return g;
}
void hub_strip_rect(int i, float w, float h, ImVec2 *a, ImVec2 *b) {
    HubGeo g = hub_geo(w, h);
    a->x = g.bx + i * g.pitch, a->y = g.by;
    b->x = a->x + g.bw, b->y = a->y + g.bh;
}
int hub_strip_hit(float x, float y, float w, float h) {  // the button under a finger, or -1
    if (!hub_strip_on()) return -1;
    HubGeo g = hub_geo(w, h);
    for (int i = 0; i < hub_strip_count(); i++) {
        ImVec2 a, b;
        hub_strip_rect(i, w, h, &a, &b);
        float mx = g.gap * .5f, my = g.bh * .3f;  // (slop round each: a thumb on a phone; half the gap, so none overlap)
        if (x >= a.x - mx && x <= b.x + mx && y >= a.y - my && y <= b.y + my) return i;
    }
    return -1;
}
void hub_strip_press(int i) {
    int n = hub_strip_count() - 2;
    if (i == 0) ldn_hub_list_step(-1);
    else if (i == n + 1) ldn_hub_list_step(1);
    else ldn_hub_list_goto(hub_strip_first() + i - 1);
}
extern "C" bool touch_event(const SDL_Event *e) {
    if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION && e->type != SDL_EVENT_FINGER_UP &&
        e->type != SDL_EVENT_FINGER_CANCELED)
        return false;
    TapAfterUnlock tap_after;  // (declared first: it runs after the lock is released)
    std::lock_guard<std::mutex> l(g_lock);
    State &s = g_s;
    ensure_loaded(s);
    uint64_t now = SDL_GetTicksNS();
#ifndef __ANDROID__
    // The HUD editor's own fingers, and the rest of one it let go of (the tap that closed it, the port's menu opened
    // over it), are no touch for the touch controls: a desktop's wait for a finger on the game itself
    SDL_FingerID fid = e->tfinger.fingerID;
    if (e->type == SDL_EVENT_FINGER_DOWN) {  // (a new touch: an id it kept, whose lift never came, is free again)
        if (s.hud_lifting && fid == s.hud_lift) s.hud_lifting = false;
        if (s.hud_fingered && fid == s.hud_finger) s.hud_fingered = false, hud_release(s);
    }
    bool hud_own = (s.hud_edit && !overlay_menu_open()) || (s.hud_fingered && fid == s.hud_finger) ||
                   (s.hud_lifting && fid == s.hud_lift);
    s.last_ns = now, s.touched = s.touched || !hud_own;
#else
    s.last_ns = now, s.touched = true;
#endif
    SDL_Window *win = ui_window();
    int pw = 0, ph = 0;
    if (win) SDL_GetWindowSizeInPixels(win, &pw, &ph);
    if (pw <= 0 || ph <= 0) return true;
    s.w = (float)pw, s.h = (float)ph;
    float x = e->tfinger.x * s.w, y = e->tfinger.y * s.h;
    if (s.edit && !overlay_menu_open()) { edit_event(s, e, x, y); return true; }
#ifndef __ANDROID__
    if (hud_own) { hud_finger(s, e, x, y); return true; }
#endif
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        Finger *f = nullptr;
        for (Finger &g : s.f)
            if (g.kind == F_NONE) { f = &g; break; }
        if (!f) return true;
        *f = {};
        f->id = e->tfinger.fingerID, f->x = f->x0 = x, f->y = f->y0 = y, f->t0 = now;
        if (overlay_menu_open()) {
            f->kind = F_MENU;
            bool other = false;
            for (Finger &g : s.f)
                if (&g != f && g.kind == F_MENU && g.part) other = true;
            f->part = !other;  // (the menu's mouse: the first finger only)
            if (f->part) to_menu(e);
            return true;
        }
        if (g_visible) {  // (the Join Hub screen's list buttons)
            int hb = hub_strip_hit(x, y, s.w, s.h);
            if (hb >= 0) {
                hub_strip_press(hb);
                f->kind = F_MENU;  // (nothing more of it until it lifts)
                tap_due();
                return true;
            }
        }
        if (!g_visible && !s.auto_wait) {
            int part = 0;
            if (g_gear_only && hit(s.layout, x, y, s.w, s.h, &part, 1.22f, false, s.menu) == C_SETTINGS) {
                f->kind = F_BUTTON, f->ctl = C_SETTINGS, f->part = part;  // (a tap opens the menu, as when they show)
                tap_due();
                return true;
            }
            f->kind = F_CAMERA;  // (hidden: a drag still turns the camera)
            return true;
        }
        Zones z = zones();
        // (in a menu of the game's the HUD's parts take no finger: a finger there is a drag that moves the cursor)
        Rect none = {0, 0, 0, 0, false}, item = zone_rect(z, HP_ITEMS, s.w, s.h),
             arts = s.menu ? none : zone_rect(z, HP_ARTS, s.w, s.h), target = s.menu ? none : zone_rect(z, HP_TARGET, s.w, s.h);
        int part = 0, c = hit(s.layout, x, y, s.w, s.h, &part, 1.22f, false, s.menu);
        if ((c == C_R || c == C_DODGE || c == C_CORNER || c == C_RSECOND || c == C_AMMO_UP || c == C_AMMO_DOWN) && item.in(x, y))
            c = -1;  // (narrow screens: their default places, and B's in layouts saved before, reach the item bar)
        int arrow = 0;
        if (c < 0 && item.ok && !s.menu && config_live_touch_show(TB_ITEM_ARROWS))
            for (int d = -1; d <= 1; d += 2) {
                Geo g = item_arrow(item, d, s.h);
                if (hypotf(x - g.c.x, y - g.c.y) < g.r * 1.2f) arrow = d;
            }
        if (c >= 0) {
            f->kind = F_BUTTON, f->ctl = c, f->part = part;
            f->btn = is_ranged(c) ? ranged_buttons(c, s.drawn, s.menu) : 0;
            if ((c == C_AMMO_UP || c == C_AMMO_DOWN) && !s.menu) ammo_step(s, c == C_AMMO_UP ? 1 : -1);
            if (!of_mpad(c)) tap_due();  // (the menu pad's group: as it presses, touch_input)
        } else if (s.menu && item.in(x, y)) {
            f->kind = F_CAMERA;  // (the item bar in a menu: a drag on it moves the cursor, as anywhere else)
        } else if (arrow) {
            f->kind = F_MENU;  // (nothing more of it until it lifts)
            itembar_scroll(arrow), s.item_dir = arrow, s.item_flash = now + 200 * MS, tap_due();
        } else if (item.in(x, y)) {
            f->kind = F_ITEM;
        } else if (target.in(x, y)) {
            f->kind = F_TARGET;
        } else if (arts.in(x, y)) {
            float rows[3][2];
            int nr = arts_rows(z, s.w, s.h, rows), row = -1;
            float best = 1e30f;  // (the row under the finger, or the nearest one)
            for (int i = 0; i < nr; i++) {
                float d = y < rows[i][0] ? rows[i][0] - y : y > rows[i][1] ? y - rows[i][1] : 0;
                if (d < best) best = d, row = i;
            }
            f->kind = F_ARTS, f->part = row;  // (performed on the tap, when it lifts: a drag turns the camera)
        } else if (!s.stick_on && in_mode(C_STICK, s.menu) && in_stick_zone(s, x, y, item)) {
            f->kind = F_STICK;
            Geo g = place(s.layout, C_STICK, s.w, s.h);
            s.stick_on = true, s.bx = g.c.x, s.by = g.c.y;
            stick_to(s, x, y);
        } else {
            f->kind = F_CAMERA;
        }
        recompute_held(s);
        static int dbg = -1;  // (MHGU_TOUCH_LOG=1: what each finger landed on)
        if (dbg < 0) dbg = getenv("MHGU_TOUCH_LOG") != nullptr;
        if (dbg)
            rt_logf("[touch] finger %.0f,%.0f of %.0fx%.0f: kind %d control %d part %d, buttons %llx; zones %d%d%d%s\n",
                    x, y, s.w, s.h, f->kind, f->ctl, f->part, (unsigned long long)s.held, item.ok, arts.ok, target.ok,
                    s.menu ? " (menu)" : "");
        return true;
    }
    Finger *f = finger(s, e->tfinger.fingerID);
    if (!f) return true;
    if (f->kind == F_MENU) {
        if (overlay_menu_open() && f->part) to_menu(e);
        if (e->type != SDL_EVENT_FINGER_MOTION) f->kind = F_NONE;
        return true;
    }
    if (e->type == SDL_EVENT_FINGER_MOTION) {
        float dx = x - f->x, dy = y - f->y;
        if (hypotf(x - f->x0, y - f->y0) > .025f * s.h) f->moved = true;
        if (f->kind == F_ARTS && f->moved) f->kind = F_CAMERA, dx = x - f->x0, dy = y - f->y0;  // (a drag: the camera)
        if (f->kind == F_CAMERA && s.menu) {  // a drag moves the cursor, following the finger, a step every so far
            f->swipe += dx, f->swipe_y += dy;
            float ax = fabsf(f->swipe), ay = fabsf(f->swipe_y), step = .075f * s.h;
            if (now >= s.step_ns && std::max(ax, ay) >= step) {
                bool across = ax > ay;  // (a step: a press that starts, as the menu pad's; felt as it presses: touch_input)
                pulse(s, across ? (f->swipe > 0 ? DPAD_RIGHT : DPAD_LEFT) : (f->swipe_y > 0 ? DPAD_DOWN : DPAD_UP), 70 * MS, 0,
                      true, true);
                s.step_ns = now + 130 * MS;  // (time to let go between steps: the game reads a press, not a hold)
                f->swipe = across ? f->swipe - copysignf(step, f->swipe) : 0;
                f->swipe_y = across ? 0 : f->swipe_y - copysignf(step, f->swipe_y);
            }
        } else if (f->kind == F_CAMERA || (f->kind == F_BUTTON && f->ctl == C_RFIRE && f->moved && !f->done && !s.menu)) {
            s.cam_dx += dx, s.cam_dy += dy, s.cam_ns = now;  // (a drag on the shot aims while it is held)
        } else if (f->kind == F_STICK) stick_to(s, x, y);
        else if (f->kind == F_ITEM) {  // a swipe turns the bar, an item every so far; up or down with a gun: the ammo
            f->swipe += dx, f->swipe_y += dy;
            float step = .055f * s.h;
            if (g_ranged.load() && !f->done && fabsf(f->swipe_y) >= step * 1.3f && fabsf(f->swipe_y) > fabsf(f->swipe)) {
                ammo_step(s, f->swipe_y < 0 ? 1 : -1), tap_due();
                f->done = true;  // (one step a swipe: each takes a moment, and two would look like none)
            }
            while (!f->done && f->swipe >= step) itembar_scroll(1), f->swipe -= step, s.item_dir = 1, s.item_flash = now + 200 * MS, tap_due();
            while (!f->done && f->swipe <= -step) itembar_scroll(-1), f->swipe += step, s.item_dir = -1, s.item_flash = now + 200 * MS, tap_due();
        } else if (f->kind == F_BUTTON && is_dpad(f->ctl)) {  // the d-pad: the direction the thumb is in now
            int part = dpad_part(place(s.layout, f->ctl, s.w, s.h), x, y, f->part);
            if (part != f->part) f->part = part, recompute_held(s), tap_due();
        } else if (f->kind == F_BUTTON && f->ctl == C_MPAD) {  // the menu pad: the same (pressed by touch_input, as a
            f->part = mpad_part(place(s.layout, C_MPAD, s.w, s.h), x, y, f->part);  // press may start: press_ok)
        } else if (f->kind == F_BUTTON && (f->ctl == C_ATTACK || f->ctl == C_CORNER)) {  // sliding along it: another part
            int part = 0, c = hit(s.layout, x, y, s.w, s.h, &part, 1.22f, false, s.menu);
            if (c == f->ctl && part != f->part) f->part = part, recompute_held(s), tap_due();
        } else if (f->kind == F_BUTTON && (f->ctl == C_HEAVY || f->ctl == C_LIGHT)) {  // from one attack to the other
            int part = 0, c = hit(s.layout, x, y, s.w, s.h, &part, 1.22f, false, s.menu);
            if ((c == C_HEAVY || c == C_LIGHT) && (c != f->ctl || part != f->part)) f->ctl = c, f->part = part, recompute_held(s), tap_due();
        } else if (f->kind == F_BUTTON && (f->ctl == C_DODGE || f->ctl == C_R)) {  // a thumb sliding onto the attacks
            int part = 0, c = hit(s.layout, x, y, s.w, s.h, &part, 1.22f, false, s.menu);
            if (c == C_ATTACK || c == C_CORNER || c == C_HEAVY || c == C_LIGHT) f->ctl = c, f->part = part, recompute_held(s);
        }
        f->x = x, f->y = y;
        return true;
    }
    // up / cancelled. (Where the finger lifted counts too: a quick flick can come with no motion between its down and
    // its up, and on the item bar that was a tap: the item used instead of the bar turned)
    if (hypotf(x - f->x0, y - f->y0) > .025f * s.h) f->moved = true;
    if (f->kind == F_ITEM && e->type == SDL_EVENT_FINGER_UP && !f->done) {
        f->swipe += x - f->x, f->swipe_y += y - f->y;
        float step = .055f * s.h;
        if (g_ranged.load() && fabsf(f->swipe_y) >= step * 1.3f && fabsf(f->swipe_y) > fabsf(f->swipe)) {
            ammo_step(s, f->swipe_y < 0 ? 1 : -1), tap_due();
            f->done = true;
        }
        // (the last bit of a swipe: an item more once it is most of a step, as the finger lifting ends it)
        while (!f->done && f->swipe >= step * .6f) itembar_scroll(1), f->swipe -= step, s.item_dir = 1, s.item_flash = now + 200 * MS, tap_due();
        while (!f->done && f->swipe <= -step * .6f) itembar_scroll(-1), f->swipe += step, s.item_dir = -1, s.item_flash = now + 200 * MS, tap_due();
    }
    bool tap = !f->moved && now - f->t0 < 300 * MS && e->type == SDL_EVENT_FINGER_UP;
    bool cancel = e->type == SDL_EVENT_FINGER_CANCELED;
    if (f->kind == F_BUTTON && !f->done && !cancel && now - f->t0 < MIN_PRESS) {  // (a quick tap is held long enough for the game,
        if (of_mpad(f->ctl)) {  // which reads the buttons at its own update rate; the menu pad's the menu's, and a press
            int want = mpad_want(*f);  // not started yet starts only as press_ok lets it)
            uint64_t b = mpad_buttons(f->ctl, want);
            if (b) pulse(s, b, MIN_PRESS - (now - f->t0), 0, true, f->sent != want, f->ctl, want);
        } else {
            uint64_t b = finger_buttons(*f);
            if (b) pulse(s, b, MIN_PRESS - (now - f->t0));
        }
    }
    if (f->kind == F_STICK) s.stick_on = false;
    if (f->kind == F_EDIT) s.pinch = false;
    if (f->kind == F_ITEM && tap && !f->done && fabsf(f->swipe) < .02f * s.h && fabsf(f->swipe_y) < .02f * s.h)
        pulse(s, B(BTN_Y), 80 * MS), tap_due();  // a tap: the item (Y)
    if (f->kind == F_ARTS && tap && f->part >= 0)  // a tap on a Hunter Art's panel: its d-pad direction
        pulse(s, ART_FACE[f->part], 120 * MS),  // (its d-pad direction)
            s.art_row = f->part, s.art_flash = now + 250 * MS, tap_due();
    if (f->kind == F_TARGET && tap) {
        tap_due();
        if (!config_live_lock_on()) {  // (the original game's: L turns the camera to the target, once)
            pulse(s, B(BTN_L), 60 * MS);
        } else {
            s.lock = !s.lock, s.lock_next = s.lock_until = 0, s.hunt_ns = now;
            rt_logf("[touch] lock-on %s\n", s.lock ? "on" : "off");
        }
    }
    // the settings button: a tap opens the port's menu (a long press opened the editor: touch_input)
    if (f->kind == F_BUTTON && f->ctl == C_SETTINGS && !f->done && e->type == SDL_EVENT_FINGER_UP) overlay_toggle_settings();
    if (s.menu) s.tap_ns = 0;  // (in the game's menus L turns pages: no double tap there)
    else if (f->kind == F_CAMERA && tap) {  // a double tap on the camera's part centres it (L)
        if (now - s.tap_ns < 350 * MS && hypotf(x - s.tap_x, y - s.tap_y) < .08f * s.h) pulse(s, B(BTN_L), 60 * MS), s.tap_ns = 0;
        else s.tap_ns = now, s.tap_x = x, s.tap_y = y;
    }
    f->kind = F_NONE;
    recompute_held(s);
    return true;
}

extern "C" void touch_input(bool pad_connected, bool typing, uint64_t *buttons, int *lx, int *ly, float *cam_dx,
                            float *cam_dy) {
    TapAfterUnlock tap_after;  // (declared first: it runs after the lock is released)
    std::lock_guard<std::mutex> l(g_lock);
    State &s = g_s;
    ensure_loaded(s);
    uint64_t now = SDL_GetTicksNS();
    g_menu_buttons = 0;
    bool menu = overlay_menu_open();
    uint32_t hunter = weapon_hunter();
    play_seen(hunter);
    // (a bow or a gun being aimed draws on the layer of the game's menus, and it is the hunt's controls that it needs)
    if (!s.edit && config_live_touch_scheme() != g_scheme.load()) switch_scheme(s, config_live_touch_scheme());
    // (Traditional: no menu layout, no menu pad and no ranged one, whatever the game is doing)
    bool game = !trad() && game_menu() && !weapon_aiming(hunter);
    static int dbg = -1;  // (MHGU_TOUCH_LOG: the menu, the hunter played or not, the menu pad's presses, as they change)
    if (dbg < 0) dbg = getenv("MHGU_TOUCH_LOG") != nullptr;
    bool play = playable(), ok = press_ok();
    menu_set_free(!play);  // (the game's thread lets the menu pad's buttons through then too: hle/input.c)
    {
        static int was = -1;
        if (dbg && (int)play != was) rt_logf("[touch] hunter %s (menu %d)\n", play ? "played" : "not played", (int)game);
        was = play;
    }
    s.weapon = weapon_ranged(hunter);
    int ranged = !trad() && config_live_touch_show(TB_RANGED) ? s.weapon : 0;
    s.drawn = weapon_drawn();
    g_bow_type = weapon_bow_type();
    if (ranged != g_ranged.load()) {  // (a weapon in hand or gone: the other layout, under the same fingers)
        g_ranged = ranged;
        for (Finger &f : s.f)
            if (f.kind == F_BUTTON && !in_mode(f.ctl, s.menu)) f.done = true;
        recompute_held(s);
    }
    if (game != s.menu) {  // (a menu of the game's opened or closed: the menu pad comes or goes, nothing else changes;
        s.menu = game;     // fingers on its group let go, its presses and steps waiting are dropped)
        g_game_menu = game;
        if (dbg) rt_logf("[touch] game menu %s (last seen %.0f ms ago, limit %.0f)\n", game ? "open" : "closed",
                         (now - g_menu_ns.load()) / 1e6, stale_limit() / 1e6);
        if (game) s.menu_on_ns = now, g_menu_on_ns = now, g_menu_town = plat_now_ns() - g_town_ns < 500 * MS;
        s.stick_cursor = false, s.stuck_ns = 0;
        for (Finger &f : s.f)
            if (f.kind == F_BUTTON && !in_mode(f.ctl, game)) f.done = true;
        if (!game) {
            for (Pulse &p : s.pulse)
                if (p.menu) p.until = 0;
            s.pad_held = 0, g_menu_town = false;
        }
        recompute_held(s);
    }
    // The menu pad's fingers: what each asks for is pressed once a press may start (press_ok), and what one presses
    // stays down while the menu takes input and the finger is on it (a long update neither lets it go nor presses it
    // again: no step too many, no repeat begun again); the game repeats a direction held. It lets go once no menu's
    // cursor has run for MPAD_LET_GO updates, the hunter played: a thumb kept on it as the menu closes is no d-pad held
    // in the field (a Hunter Art). A click of the vibrator as it presses something new, not as it is touched
    s.pad_held = 0;
    uint32_t age = menu_cursor_age();
    bool gone = play && age >= MPAD_LET_GO;  // (the hunter not played: no Art can come of it)
    static int said_want = -1, said_sent = -1, said_ctl = -1;  // (the log: once a change, not every update it waits)
    bool any = false;
    for (Finger &f : s.f) {
        if (f.kind != F_BUTTON || f.done || !of_mpad(f.ctl)) continue;
        any = true;
        int asked = mpad_want(f), want = gone ? 0 : asked;
        if (want != f.sent) {
            int was = f.sent;
            f.sent = ok ? want : 0;
            if (f.sent && f.sent != was) tap_due();
        }
        if (dbg && (asked || said_want > 0) && (asked != said_want || f.sent != said_sent || f.ctl != said_ctl))
            rt_logf("[touch] menu pad %d: asks %d, presses %d (frame %llu, a menu's cursor %u updates ago)%s\n", f.ctl, asked,
                    f.sent, (unsigned long long)g_present_count, age,
                    asked && !f.sent ? " (held back: no menu's cursor in the last update)" : "");
        said_want = asked, said_sent = f.sent, said_ctl = f.ctl;
        s.pad_held |= mpad_buttons(f.ctl, f.sent);
    }
    if (!any) said_want = said_sent = said_ctl = -1;
    // the editor, asked for from the settings menu (which closes itself: settings_panel.cpp): once it has closed
#ifndef __ANDROID__
    if (s.edit_request && s.hud_edit) s.edit_request = false;  // (the HUD editor open: one at a time)
#endif
    if (s.edit_request && !menu) edit_begin(s);
#ifndef __ANDROID__
    if (s.hud_request && !menu) hud_edit_begin(s);  // (the HUD editor: the same way)
    if (s.hud_edit && menu) {  // (the port's menu over it: the usual pointer, and a finger or a drag under way let go)
        hud_cursor(false);
        hud_let_go(s), hud_release(s);
    }
    // (the press that closed it is up, wherever its button-up went)
    if (g_hud_hold && !s.hud_edit && !(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK)) hud_hold_end(s);
#endif
    // a long press on the settings button opens the editor; on L, it turns the lock-on on or off
    for (Finger &f : s.f) {
        if (f.kind != F_BUTTON || f.done) continue;
        if (f.ctl == C_SETTINGS && now - f.t0 > 700 * MS && !menu) {
            f.done = true;
            edit_begin(s);
            break;
        }
        // (no long press on L any more: the lock-on is the target icon's alone)
    }
    int mode = config_live_touch_controls();
#ifdef __ANDROID__
    bool screen = true;
#else
    bool screen = s.touched;  // (a desktop: Auto shows them once a finger has touched the screen)
#endif
    bool visible = s.edit || (!typing && (mode == TOUCH_ON || (mode == TOUCH_AUTO && !pad_connected && screen)));
    s.auto_wait = !s.edit && !typing && mode == TOUCH_AUTO && !pad_connected && !screen;
#ifndef __ANDROID__
    if (s.hud_edit) visible = s.auto_wait = false;  // (the HUD editor: the HUD as it is with them hidden, nothing of theirs)
#endif
    g_visible = visible;
    // (hidden, by a controller or by Off: the settings button alone stays, or a phone without a controller had no way
    // back to the menu but clearing the app's data)
    g_gear_only = !visible && !typing && screen;
#ifndef __ANDROID__
    if (s.hud_edit) g_gear_only = false;
#endif
    {
        static int was = -1;
        if (was != (int)visible) {
            rt_logf("[touch] controls %s (setting %d, controller %d, typing %d, editor %d)\n",
                    visible ? "shown" : "hidden", mode, (int)pad_connected, (int)typing, (int)s.edit);
            was = visible;
        }
    }
    g_touch_hud = visible && config_live_touch_hud();
    // (while a menu is open the fingers are its mouse: nothing of theirs reaches the game or the menu's controller)
    if (menu) {
        s.cam_dx = s.cam_dy = 0;
        return;
    }
    for (Pulse &p : s.pulse) {
        if (p.until <= now || now < p.from) continue;
        if (p.menu && gone) {  // (the menu takes no input any more: the rest of a tap of the pad's lets go too)
            if (dbg && (!p.checked || p.ctl >= 0))
                rt_logf("[touch] a menu press %llx dropped (frame %llu): no menu's cursor for %u updates\n",
                        (unsigned long long)p.buttons, (unsigned long long)g_present_count, age);
            p.until = 0;
            continue;
        }
        if (p.fresh && !p.checked) {  // (a press that starts, the menu pad's or a drag's step: now, or never)
            p.checked = true;
            if (!ok) {
                if (dbg)
                    rt_logf("[touch] a menu press %llx dropped (frame %llu): no menu's cursor in the last update\n",
                            (unsigned long long)p.buttons, (unsigned long long)g_present_count);
                p.until = 0;
                continue;
            }
            tap_due();
        }
        if (p.ctl >= 0) s.lit_ctl = p.ctl, s.lit_part = p.part, s.lit_until = p.until;  // (lit while it presses)
        if (p.menu) g_menu_buttons |= p.buttons;  // (the game reads them only while a menu takes input: host_input.c)
        else *buttons |= p.buttons;
    }
    if (now < s.ammo_hold) *buttons |= B(BTN_L);
    if (s.h > 0 && !s.edit) {
        float k = CAMERA_COUNTS * config_live_touch_camera() / s.h;
        *cam_dx += s.cam_dx * k, *cam_dy += s.cam_dy * k;
    }
    s.cam_dx = s.cam_dy = 0;
    if (!visible || s.edit) return;
    *buttons |= s.held | s.tapped;
    s.tapped = 0;
    if (s.menu) g_menu_buttons |= s.pad_held;
    // (a press that can open a menu, and a locked shot's aim: on every input's buttons, touch_lock_buttons and
    // touch_lock_shot)
    // In a menu where the hunter isn't played (the title, the main menu, a talk: nothing walks there) the stick moves the
    // cursor as before: the d-pad's direction, one at a time, held while it is tilted; only for a thumb that landed or
    // came back to the middle since the hunter was played, or one tilted as such a menu opened would move it at once.
    // Elsewhere, menu or not, it walks (the pause menu, the pouch: the game's menus there don't read the stick)
    // ...but for a menu opened in a town where the stick tilted moves the player nowhere for STUCK_NS (the item box, a
    // shop, the smithy, the quest counter: the town goes on under them, the hunter only turns): from then until it
    // closes the stick moves its cursor there too. Never in a hunt: a d-pad there is a Hunter Art
    float sr = DEFS[C_STICK].r * s.layout.scale[C_STICK] * s.h;
    int stick_dir = s.stick_on && s.h > 0 ? dpad_part({ImVec2(s.bx, s.by), sr * 1.7f}, s.kx, s.ky, s.stick_part) : 0;
    if (s.menu && g_menu_town.load() && !s.stick_cursor) {
        const uint64_t STUCK_NS = 300 * MS;
        float px, pz;
        if (stick_dir && player_place(&px, &pz)) {
            if (!s.stuck_ns || hypotf(px - s.stuck_x, pz - s.stuck_z) > 2.0f) s.stuck_ns = now, s.stuck_x = px, s.stuck_z = pz;
            else if (now - s.stuck_ns >= STUCK_NS) {
                s.stick_cursor = s.stick_menu_ok = true;
                if (dbg) rt_logf("[touch] the player walks nowhere in this menu: the stick moves its cursor\n");
            }
        } else {
            s.stuck_ns = 0;
        }
    }
    if (!stick_dir) s.stick_menu_ok = true;
    else if (play && !s.stick_cursor) s.stick_menu_ok = false;
    if (s.menu && (!play || s.stick_cursor) && s.stick_menu_ok) {
        if (stick_dir && stick_dir != s.stick_part) tap_due();
        s.stick_part = stick_dir;
        *buttons |= dpad_buttons(stick_dir);
        return;
    }
    s.stick_part = 0;
    bool moving = false;
    if (s.stick_on && s.h > 0) {
        float r = DEFS[C_STICK].r * s.layout.scale[C_STICK] * s.h;
        float dx = (s.kx - s.bx) / r, dy = -(s.ky - s.by) / r;  // (screen y is down, the stick's up)
        float m = sqrtf(dx * dx + dy * dy);
        // a tiny dead zone (4%), and the first bit of tilt already past the game's own dead zone (about a quarter):
        // the hunter moves from the first millimetre, faster the farther; full at 90% of the rim
        if (m > .04f) {
            float o = .28f + .72f * std::min(1.0f, (m - .04f) / .86f), t = o / m;
            *lx += (int)(dx * t * 32767), *ly += (int)(dy * t * 32767);
            moving = m > .3f;
        }
    }
    // the thumb past the stick's rim runs: R held (the dash, weapon sheathed; drawn, some weapons guard with it), with
    // a little hysteresis so the edge does not flicker
    s.run = false;
    // (only sheathed: with the weapon out, R is some weapons' guard - the thumb at the edge in a fight walks)
    int drawn = weapon_drawn();
    static int drawn_was = -2;
    if (drawn != drawn_was) {
        if (dbg) rt_logf("[touch] weapon %s\n", drawn == 1 ? "out" : drawn == 0 ? "sheathed" : "(not known)");
        drawn_was = drawn;
    }
    if (s.stick_on && moving && s.h > 0 && drawn != 1 && !s.menu && (!trad() || config_live_touch_auto_run())) {  // (not in a menu: R turns its pages; Traditional with [Touch] AutoRun off: the R button runs)
        float r = DEFS[C_STICK].r * s.layout.scale[C_STICK] * s.h, d = hypotf(s.fx - s.bx, s.fy - s.by);
        static bool was;
        was = d > r * (was ? 1.45f : 1.6f);  // (well past the rim: a thumb at the edge in a fight still walks;
                                             // r is the stick's size in the layout: a bigger stick, farther)
        s.run = was;
        if (s.run) *buttons |= B(BTN_R);
    }
}

extern "C" bool touch_visible(void) { return g_visible; }
extern "C" uint64_t touch_menu_buttons(void) { return g_menu_buttons; }
#ifndef __ANDROID__
extern "C" bool touch_drawn(void) { return g_visible || g_gear_only || (g_lock_mark && target_seen()) || g_hud_edit; }
#else
extern "C" bool touch_drawn(void) { return g_visible || g_gear_only || (g_lock_mark && target_seen()); }
#endif
extern "C" bool touch_game_menu(void) { return g_game_menu.load(); }

extern "C" void touch_edit_begin(void) {
    std::lock_guard<std::mutex> l(g_lock);
    g_s.edit_request = true;
}

extern "C" bool touch_editor_open(void) {
    std::lock_guard<std::mutex> l(g_lock);
#ifndef __ANDROID__
    return g_s.edit || g_s.hud_edit;
#else
    return g_s.edit;
#endif
}

extern "C" void touch_editor_cancel(void) {  // (a controller's B: a player with a controller alone gets out)
    std::lock_guard<std::mutex> l(g_lock);
    if (g_s.edit) edit_end(g_s, false), drop_fingers(g_s);
#ifndef __ANDROID__
    else if (g_s.hud_edit) hud_edit_end(g_s, false);
#endif
}

extern "C" void touch_edit_cancel(void) {  // (the settings menu stayed open: "Cancel" in its unsaved-changes dialog)
    std::lock_guard<std::mutex> l(g_lock);
    g_s.edit_request = false;
#ifndef __ANDROID__
    g_s.hud_request = false;  // (the HUD editor's too)
    hud_asked(g_s);
#endif
}

#ifndef __ANDROID__
extern "C" void touch_hud_edit_begin(void) {
    std::lock_guard<std::mutex> l(g_lock);
    g_s.hud_request = true;
    hud_asked(g_s);
}

extern "C" bool touch_hud_editing(void) { return g_hud_asked.load(); }

extern "C" void touch_hud_pad(bool read) { g_hud_pad = read; }

// The mouse while the HUD editor is open (not under the port's menu: the menu has it then): a left click presses the
// panel's button or picks up the part under it, a drag moves that part, the wheel sizes one; any other button does
// nothing (no capture, no press of the game's), and Esc cancels the editor
extern "C" bool touch_edit_mouse(const SDL_Event *e) {
    // (the press that closed it, Save or Cancel or Esc in the middle of a drag: the rest of it is the editor's still,
    // until its button is up or a motion shows it up)
    if (g_hud_hold.load() && !g_hud_edit.load() &&
        (e->type == SDL_EVENT_MOUSE_MOTION || (e->type == SDL_EVENT_MOUSE_BUTTON_UP && e->button.button == SDL_BUTTON_LEFT))) {
        if (e->type == SDL_EVENT_MOUSE_BUTTON_UP || !(e->motion.state & SDL_BUTTON_LMASK)) {
            std::lock_guard<std::mutex> l(g_lock);
            hud_hold_end(g_s);
        }
        return true;
    }
    if (!g_hud_edit.load() || overlay_menu_open()) return false;
    bool esc = e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_ESCAPE;
    if (!esc && e->type != SDL_EVENT_MOUSE_MOTION && e->type != SDL_EVENT_MOUSE_BUTTON_DOWN &&
        e->type != SDL_EVENT_MOUSE_BUTTON_UP && e->type != SDL_EVENT_MOUSE_WHEEL)
        return false;
    std::lock_guard<std::mutex> l(g_lock);
    State &s = g_s;
    if (!s.hud_edit) return false;
    if (esc) {
        if (!e->key.repeat) hud_edit_end(s, false);
        return true;
    }
    SDL_Window *win = ui_window();  // (the mouse's place in the window's points; the editor's in its pixels)
    int ww = 0, wh = 0, pw = 0, ph = 0;
    if (!win || !SDL_GetWindowSize(win, &ww, &wh) || !SDL_GetWindowSizeInPixels(win, &pw, &ph) || ww <= 0 || wh <= 0 ||
        pw <= 0 || ph <= 0)
        return true;
    s.w = (float)pw, s.h = (float)ph;
    float kx = s.w / ww, ky = s.h / wh;
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (!(e->motion.state & SDL_BUTTON_LMASK)) hud_release(s);  // (let go away from the window: no up came)
        hud_move(s, e->motion.x * kx, e->motion.y * ky);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button == SDL_BUTTON_LEFT) hud_press(s, e->button.x * kx, e->button.y * ky);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button == SDL_BUTTON_LEFT) hud_release(s);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        hud_wheel(s, e->wheel.mouse_x * kx, e->wheel.mouse_y * ky,
                  e->wheel.integer_y * (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1));
        break;
    default:
        break;
    }
    return true;
}
#endif

extern "C" void touch_lock_toggle(void) {
    std::lock_guard<std::mutex> l(g_lock);
    g_s.lock = !g_s.lock, g_s.lock_next = g_s.lock_until = 0, g_s.hunt_ns = SDL_GetTicksNS();
    rt_logf("[touch] lock-on %s (controller)\n", g_s.lock ? "on" : "off");
}

// The lock-on: the camera turned to the target again and again (L tapped: the game's target camera; with no target,
// behind the hunter), while the hunt camera turns and the player is neither turning it (a finger, the right stick, the
// mouse: cam_input) nor pressing another button (L held with A / Y turns the item bar). Off by itself once the hunt
// camera has not run for 10 s (the hunt over). It waits while a menu of the game's is open (game_menu): so that a menu
// is told from a window that came by itself whatever the player plays with, a press that can open one is noted here,
// from every input's buttons (the touch controls', the keys', a controller's)
extern "C" uint64_t touch_lock_buttons(uint64_t buttons, bool cam_input) {
    std::lock_guard<std::mutex> l(g_lock);
    State &s = g_s;
    uint64_t now = SDL_GetTicksNS();
    if (!overlay_menu_open()) {  // (the port's menu takes the presses while it is open)
        static uint64_t was;
        // (with a gun out A fires, and the scope's window comes back after each shot)
        const uint64_t OPENS = (s.weapon && s.drawn == 1 ? 0 : B(BTN_A) | B(BTN_Y)) | B(BTN_PLUS) | B(BTN_MINUS) | B(BTN_ZL) | B(BTN_ZR);
        if (buttons & ~was & OPENS) g_press_ns = now;
        was = buttons;
    }
    bool hunt = mousecam_active() || s.weapon;  // (a gun aimed has a camera of its own)
    if (hunt) s.hunt_ns = now;
    if (s.lock && now - s.hunt_ns > 10000 * MS) {
        s.lock = false;
        rt_logf("[touch] lock-on off (no hunt camera)\n");
    }
    if (cam_input) s.cam_ns = now;
    const uint64_t others = B(BTN_A) | B(BTN_B) | B(BTN_X) | B(BTN_Y) | B(BTN_R) | B(BTN_ZR) | B(BTN_ZL) | B(BTN_L);
    (void)others;
    if (s.lock && !config_live_lock_on()) s.lock = false;  // (turned off in the settings)
    bool on = s.lock && !s.edit && hunt && !overlay_menu_open() && !s.menu;
    g_lock_shots = on;
    g_lock_follow = on && now - s.cam_ns > LOCK_AFTER_DRAG;
    g_lock_mark = s.lock && !s.edit && !s.menu;
    return 0;  // (no L presses: the hunt camera follows the monster itself, hle/mousecam.c)
}

// With the lock-on on, a gun's shot aims first: only an aimed shot takes the monster's height (hle/mousecam.c). Its aim
// stays a moment, for the next shot; the scope, opened by hand, is left alone. On every input's fire button (the touch
// controls', a key, a mouse button, a controller's: host_input.c gives them all, once each update)
// The aim is known by the hunter's actions (weapon_aiming): some styles' aims are other actions, and a shot that
// waited for them never left (seen: a light and a heavy bowgun). So after a moment of R it fires anyway: R is the aim
extern "C" uint64_t touch_lock_shot(uint64_t buttons) {
    std::lock_guard<std::mutex> l(g_lock);
    State &s = g_s;
    uint64_t now = SDL_GetTicksNS();
    if (s.weapon == WEAPON_BOWGUN && s.drawn == 1 && g_lock_shots.load()) {
        uint32_t hunter = weapon_hunter();
        bool fire = (buttons & B(BTN_A)) != 0, aiming = weapon_aiming(hunter);
        if (fire && !s.fired && !aiming) s.assist_wait = true, s.assist_t0 = now;
        if (fire && !aiming) s.assist_until = now + 1300 * MS;
        s.fired = fire;
        if (now < s.assist_until) buttons |= B(BTN_R);
        if (s.assist_wait) {
            buttons &= ~B(BTN_A);
            bool late = now - s.assist_t0 >= 450 * MS;
            if (aiming || late) {
                s.assist_wait = false, s.assist_fire = now + 110 * MS;
                if (!aiming) {
                    static unsigned said[8];
                    unsigned act = weapon_action(hunter), k = 0;
                    while (k < 8 && said[k] && said[k] != act) k++;
                    if (k < 8 && !said[k]) {
                        said[k] = act;
                        rt_logf("[touch] a locked shot: no aim seen (action %04x:%03u), fired with R held\n",
                                act >> 16, act & 0xffff);
                    }
                }
            } else if (now >= s.assist_until) {
                s.assist_wait = false;
            }
        }
        if (now < s.assist_fire) buttons |= B(BTN_A);
    } else {
        s.assist_wait = s.fired = false, s.assist_until = 0;
    }
    g_assist_until = s.assist_until;
    return buttons;
}

// (the hunt camera's thread, hle/mousecam.c) the lock-on is on and the player is not turning the camera
extern "C" bool touch_lock_active(void) { return g_lock_follow.load(); }
extern "C" bool touch_lock_on(void) { return g_lock_shots.load(); }
extern "C" bool touch_lock_assist(void) { return g_lock_shots.load() && SDL_GetTicksNS() < g_assist_until.load(); }

// The app goes to the background (Android): every finger is gone (their ups will not come)
extern "C" void touch_reset(void) {
    std::lock_guard<std::mutex> l(g_lock);
    State &s = g_s;
    drop_fingers(s);
    s.cam_dx = s.cam_dy = 0;
}

extern "C" void touch_hud_piece(int part, float game_x, float game_y, float canvas_x, float canvas_y, float k, int flags) {
    (void)game_x;
    if (part < 0 || part >= HP_COUNT) return;
    Zones &b = g_build;
    if (!b.n[part]++) b.x0[part] = b.x1[part] = canvas_x, b.y0[part] = b.y1[part] = canvas_y, b.k[part] = k, b.hidden[part] = false;
    b.x0[part] = std::min(b.x0[part], canvas_x), b.x1[part] = std::max(b.x1[part], canvas_x);
    b.y0[part] = std::min(b.y0[part], canvas_y), b.y1[part] = std::max(b.y1[part], canvas_y);
    b.hidden[part] |= (flags & 2) != 0;
    if (!(flags & 1)) return;
    if (!b.nt[part]++) b.tx0[part] = b.tx1[part] = canvas_x, b.ty0[part] = b.ty1[part] = canvas_y;
    b.tx0[part] = std::min(b.tx0[part], canvas_x), b.tx1[part] = std::max(b.tx1[part], canvas_x);
    b.ty0[part] = std::min(b.ty0[part], canvas_y), b.ty1[part] = std::max(b.ty1[part], canvas_y);
    if (part == HP_ARTS && b.na < 64) b.agy[b.na] = game_y, b.acy[b.na] = canvas_y, b.na++;
}

extern "C" void touch_hud_menu(void) { g_build_menu = true; }
bool g_build_front;  // (render thread) the front end's backdrop in this frame
extern "C" void touch_hud_front(void) { g_build_front = true; }

extern "C" void touch_hud_frame(void) {
    {  // (MHGU_TOUCH_LOG: the frames without a menu window between two with one, the gaps inside a menu)
        static int dbg = -1, gap;
        static uint64_t last_with;
        if (dbg < 0) dbg = getenv("MHGU_TOUCH_LOG") != nullptr;
        uint64_t t = SDL_GetTicksNS();
        if (g_build_menu) {
            if (dbg && gap > 0 && t - last_with < 2000 * MS)
                rt_logf("[touch] menu window back after %d frames without (%.0f ms)\n", gap, (t - last_with) / 1e6);
            gap = 0, last_with = t;
        } else {
            gap++;
        }
    }
    if (g_build_front) g_frames_since_front = 0;
    else if (g_frames_since_front.load() < 1000) g_frames_since_front = g_frames_since_front.load() + 1;
    g_build_front = false;
    // (the menu pad: frames presented since the last one with a menu window, and when the second without one came)
    if (g_build_menu) {
        g_menu_ns = SDL_GetTicksNS();
        g_frames_since_menu = 0, g_menu_gone_ns = 0;
    } else {
        int n = g_frames_since_menu.load();
        if (n < 1000) g_frames_since_menu = ++n;
        if (n == 2) g_menu_gone_ns = SDL_GetTicksNS();
    }
    g_build_menu = false;
    // (a frame drawn without those parts keeps the last ones: they age out after 300 ms, zone_rect, when the HUD is
    // gone; a frame now and then without them must not leave a finger on the camera instead)
    bool any = false;
    for (int k = 0; k < HP_COUNT; k++) any |= g_build.nt[k] != 0;
    static uint64_t last;
    uint64_t t = SDL_GetTicksNS();
    if (last && t - last < 2000 * MS) g_frame_ns = (g_frame_ns * 7 + (t - last)) / 8;
    last = t;
    static int dbg = -1, frames, with;  // (MHGU_TOUCH_LOG: how many presented frames had them, every 2 s)
    static uint64_t since;
    if (dbg < 0) dbg = getenv("MHGU_TOUCH_LOG") != nullptr;
    if (dbg) {
        uint64_t now = SDL_GetTicksNS();
        frames++, with += any;
        if (now - since > 2000 * MS) {
            rt_logf("[touch] hud parts in %d of %d frames (item %d arts %d target %d)\n", with, frames, g_build.nt[HP_ITEMS],
                    g_build.nt[HP_ARTS], g_build.nt[HP_TARGET]);
            since = now, frames = with = 0;
        }
    }
    {
        std::lock_guard<std::mutex> l(g_zone_lock);
        if (any) g_zones = g_build, g_zones.ns = SDL_GetTicksNS();
        int prof = g_hud_prof_now;
        float s = hud_squeeze(), arts_top = g_hud_arts_top;
        for (int k = 0; k < HP_COUNT; k++) {  // (the editor's frames: every part drawn, a finger on it or not)
            if (!g_build.n[k]) continue;
            Seen &sn = g_seen[k];
            float m = g_build.k[k];
            hud_box(k, prof, arts_top, sn.e);
            if (HUD_FRAME_BOX(k)) memcpy(sn.r, sn.e, sizeof sn.r);
            else
                sn.r[0] = g_build.x0[k] - HUD_PAD[k][0] * m * s, sn.r[1] = g_build.y0[k] - HUD_PAD[k][1] * m,
                sn.r[2] = g_build.x1[k] + HUD_PAD[k][2] * m * s, sn.r[3] = g_build.y1[k] + HUD_PAD[k][3] * m;
            sn.prof = prof, sn.ns = SDL_GetTicksNS();
        }
    }
    memset(g_build.n, 0, sizeof g_build.n), memset(g_build.nt, 0, sizeof g_build.nt), g_build.na = 0;
}

extern "C" bool touch_hud_zone(int part, float w, float h, float rect[4]) {
    if (part < 0 || part >= HP_COUNT) return false;
    Rect r = zone_rect(zones(), part, w, h);
    if (r.ok) rect[0] = r.x0, rect[1] = r.y0, rect[2] = r.x1, rect[3] = r.y1;
    return r.ok;
}

extern "C" bool touch_hud_part_rect(int part, float w, float h, float rect[4]) {
    int prof = g_hud_prof_now;
    if (part < 0 || part >= HP_COUNT || prof < 0 || prof >= HUD_PROFILES || g_hud_hidden[prof][part]) return false;
    bool live;
    Rect r = part_frame(part, prof, w, h, &live);
    if (live) rect[0] = r.x0, rect[1] = r.y0, rect[2] = r.x1, rect[3] = r.y1;
    return live;
}

// ------------------------------------------------------------------ drawing (game thread)
namespace {

const ImU32 INK = IM_COL32(244, 232, 200, 255), GOLD = IM_COL32(222, 186, 96, 255), BRONZE = IM_COL32(112, 76, 34, 255),
            DEEP = IM_COL32(28, 20, 15, 255), GLOW = IM_COL32(255, 214, 92, 255), RED = IM_COL32(176, 34, 30, 255),
            MOSS = IM_COL32(80, 142, 54, 255), WINE = IM_COL32(50, 25, 24, 255);

ImU32 fade(ImU32 c, float a) {
    return (c & 0x00ffffffu) | ((ImU32)std::clamp(((c >> 24) & 0xff) * a, 0.0f, 255.0f) << 24);
}
ImVec2 P(ImVec2 c, float s, float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); }
ImVec2 add(ImVec2 a, ImVec2 b, float k) { return ImVec2(a.x + b.x * k, a.y + b.y * k); }

// The hub list bar's colours follow the settings menu's theme ([Application] Theme)
struct HubColors {
    ImU32 panel, edge, button, lit, lit_edge, lit_ink, ink, accent;
};
HubColors hub_colors() {
    switch (settings_panel_theme()) {
    case THEME_BLUE:
        return {IM_COL32(6, 10, 17, 238), IM_COL32(70, 140, 170, 255), IM_COL32(20, 32, 48, 255), IM_COL32(111, 216, 255, 255),
                IM_COL32(190, 240, 255, 255), IM_COL32(6, 14, 24, 255), IM_COL32(244, 247, 252, 255), IM_COL32(111, 216, 255, 255)};
    case THEME_GOLD:
        return {IM_COL32(34, 25, 19, 240), IM_COL32(150, 112, 60, 255), IM_COL32(56, 41, 30, 255), IM_COL32(213, 162, 79, 255),
                IM_COL32(255, 222, 150, 255), IM_COL32(30, 20, 12, 255), IM_COL32(242, 216, 170, 255), IM_COL32(213, 162, 79, 255)};
    default:  // Hunter: the game's own wine, gold and bronze
        return {IM_COL32(74, 24, 22, 242), IM_COL32(150, 104, 52, 255), IM_COL32(40, 14, 14, 255), GOLD, GLOW, DEEP, INK, GOLD};
    }
}
void hub_strip_draw(ImDrawList *dl, ImFont *font, float w, float h, float a) {
    if (!hub_strip_on()) return;
    HubColors c = hub_colors();
    HubGeo g = hub_geo(w, h);
    int cnt = hub_strip_count(), chosen = ldn_hub_list_chosen();
    float fs = g.bh * .5f, r = g.bh * .2f, line = std::max(1.0f, g.bh * .045f);
    ImVec2 p0(g.px, g.py), p1(g.px + g.pw, g.py + g.ph);
    dl->AddRectFilled(p0, p1, fade(c.panel, a), r * 1.3f);
    dl->AddRect(p0, p1, fade(c.edge, a * .9f), r * 1.3f, 0, line);
    for (int i = 0; i < cnt; i++) {
        ImVec2 p, q;
        hub_strip_rect(i, w, h, &p, &q);
        bool arrow = i == 0 || i == cnt - 1, lit = !arrow && hub_strip_first() + i - 1 == chosen;
        dl->AddRectFilled(p, q, fade(lit ? c.lit : c.button, a), r);
        dl->AddRect(p, q, fade(lit ? c.lit_edge : c.edge, a), r, 0, line);
        ImVec2 m((p.x + q.x) * .5f, (p.y + q.y) * .5f);
        ImU32 ink = fade(lit ? c.lit_ink : c.ink, a);
        if (arrow) {
            float d = i == 0 ? -1.0f : 1.0f, ch = g.bh * .2f;
            ImVec2 tip(m.x + d * ch * .7f, m.y), t1(m.x - d * ch * .5f, m.y - ch), t2(m.x - d * ch * .5f, m.y + ch);
            dl->AddLine(t1, tip, ink, g.bh * .08f), dl->AddLine(t2, tip, ink, g.bh * .08f);
        } else {
            char t[8];
            snprintf(t, sizeof t, "%d", hub_strip_first() + i);
            ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0, t);
            dl->AddText(font, fs, ImVec2(m.x - sz.x * .5f, m.y - sz.y * .5f), ink, t);
        }
    }
    // which hubs the chosen list holds, in a small chip over the bar
    int first = 0, last = 0, total = 0;
    ldn_hub_list_range(&first, &last, &total);
    char t[64];
    snprintf(t, sizeof t, "hubs #%d to %d of %d", first, last, total);
    float cs = g.ch * .62f;
    ImVec2 sz = font->CalcTextSizeA(cs, FLT_MAX, 0, t);
    float cw = sz.x + g.ch * 1.0f, cx = g.px + g.pw * .5f;
    ImVec2 c0(cx - cw * .5f, g.cy - g.ch * .5f), c1(cx + cw * .5f, g.cy + g.ch * .5f);
    dl->AddRectFilled(c0, c1, fade(c.panel, a), g.ch * .5f);
    dl->AddRect(c0, c1, fade(c.edge, a * .9f), g.ch * .5f, 0, line);
    dl->AddText(font, cs, ImVec2(cx - sz.x * .5f, g.cy - sz.y * .5f), fade(c.ink, a), t);
}

void sword(ImDrawList *dl, ImVec2 c, float s, float dx, float dy, ImU32 col) {
    float n = sqrtf(dx * dx + dy * dy);
    dx /= n, dy /= n;
    float px = -dy, py = dx;
    ImVec2 b0(c.x - dx * .25f * s, c.y - dy * .25f * s), tip(c.x + dx * .9f * s, c.y + dy * .9f * s);
    ImVec2 sh(tip.x - dx * .22f * s, tip.y - dy * .22f * s);
    float w = .12f * s;
    ImVec2 blade[5] = {ImVec2(b0.x + px * w, b0.y + py * w), ImVec2(sh.x + px * w, sh.y + py * w), tip,
                       ImVec2(sh.x - px * w, sh.y - py * w), ImVec2(b0.x - px * w, b0.y - py * w)};
    dl->AddConvexPolyFilled(blade, 5, col);
    dl->AddLine(ImVec2(b0.x + px * .4f * s, b0.y + py * .4f * s), ImVec2(b0.x - px * .4f * s, b0.y - py * .4f * s), col,
                .14f * s);
    ImVec2 hilt(b0.x - dx * .42f * s, b0.y - dy * .42f * s);
    dl->AddLine(b0, hilt, col, .12f * s);
    dl->AddCircleFilled(ImVec2(hilt.x - dx * .06f * s, hilt.y - dy * .06f * s), .11f * s, col);
}

// the heavy attack: a great sword, point up (a broad blade with its fuller, a wide guard, the grip and pommel)
// a sword along u, its point toward +u, centred on c: the blade (and its fuller), the guard across, the grip, the pommel
void sword_along(ImDrawList *dl, ImVec2 c, ImVec2 u, float len, float bw, float guard, ImU32 col, ImU32 deep, bool fuller) {
    ImVec2 n(-u.y, u.x), base = add(c, u, -.18f * len), tip = add(c, u, .62f * len), sh = add(tip, u, -bw * 1.1f);
    ImVec2 blade[5] = {add(base, n, bw * .5f), add(sh, n, bw * .5f), tip, add(sh, n, -bw * .5f), add(base, n, -bw * .5f)};
    dl->AddConvexPolyFilled(blade, 5, col);
    if (fuller) dl->AddLine(add(base, u, bw * .3f), add(sh, u, -bw * .2f), deep, bw * .22f);
    dl->AddLine(add(base, n, guard * .5f), add(base, n, -guard * .5f), col, bw * .55f);
    ImVec2 grip = add(base, u, -.2f * len);
    dl->AddLine(base, grip, col, bw * .42f);
    dl->AddCircleFilled(add(grip, u, -bw * .2f), bw * .38f, col, 16);
}

void heavy_blade(ImDrawList *dl, ImVec2 c, float s, ImU32 col, ImU32 deep) {
    ImVec2 b[5] = {P(c, s, -.25f, .28f), P(c, s, -.25f, -.5f), P(c, s, 0, -.9f), P(c, s, .25f, -.5f), P(c, s, .25f, .28f)};
    dl->AddConvexPolyFilled(b, 5, col);
    dl->AddLine(P(c, s, 0, .18f), P(c, s, 0, -.55f), deep, .075f * s);         // (the fuller)
    dl->AddRectFilled(P(c, s, -.5f, .28f), P(c, s, .5f, .42f), col, .04f * s);  // (the guard)
    dl->AddRectFilled(P(c, s, -.075f, .42f), P(c, s, .075f, .76f), col);       // (the grip)
    dl->AddCircleFilled(P(c, s, 0, .83f), .105f * s, col, 16);                 // (the pommel)
}

// a crescent: the trail of a slash (thickest in its middle, tapering to points), from angle a0 to a1 round c
void crescent(ImDrawList *dl, ImVec2 c, float r, float width, float a0, float a1, ImU32 col) {
    const int N = 18;
    ImVec2 pts[2 * (N + 1)];
    for (int i = 0; i <= N; i++) {
        float t = (float)i / N, a = a0 + (a1 - a0) * t, ri = r - width * sinf(PI * t);
        pts[i] = ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r);
        pts[2 * N + 1 - i] = ImVec2(c.x + cosf(a) * ri, c.y + sinf(a) * ri);
    }
    dl->AddConcavePolyFilled(pts, 2 * (N + 1), col);
}

// the light attack: a slim blade pointing up and right, quick: three speed streaks along it on the side it comes from
void light_blade(ImDrawList *dl, ImVec2 c, float s, ImU32 col) {
    const float dx = .7071f, dy = -.7071f, nx = -.7071f, ny = -.7071f;  // (the blade's way; the side behind it)
    ImVec2 m = P(c, s, -nx * .16f, -ny * .16f);
    static const float off[3] = {.36f, .56f, .76f}, len[3] = {.62f, .46f, .3f}, alpha[3] = {.85f, .6f, .38f};
    for (int i = 0; i < 3; i++) {
        ImVec2 o(m.x + nx * off[i] * s + dx * .06f * s, m.y + ny * off[i] * s + dy * .06f * s);
        dl->AddLine(ImVec2(o.x - dx * len[i] * s, o.y - dy * len[i] * s), ImVec2(o.x + dx * len[i] * s * .7f, o.y + dy * len[i] * s * .7f),
                    fade(col, alpha[i]), .085f * s);
    }
    sword(dl, m, s * .9f, 1, -1, col);
}

void padlock(ImDrawList *dl, ImVec2 c, float s, bool closed, ImU32 col, ImU32 deep) {
    float sx = closed ? 0 : .28f;  // (open: the shackle swung up and to the right)
    dl->PathArcTo(P(c, s, sx, -.18f), .34f * s, PI, 2 * PI, 16);
    dl->PathStroke(col, 0, .13f * s);
    dl->AddLine(P(c, s, sx + .34f, -.18f), P(c, s, sx + .34f, closed ? .02f : -.1f), col, .13f * s);
    dl->AddLine(P(c, s, sx - .34f, -.18f), P(c, s, sx - .34f, .02f), col, .13f * s);
    dl->AddRectFilled(P(c, s, -.5f, -.02f), P(c, s, .5f, .72f), col, .1f * s);
    dl->AddCircleFilled(P(c, s, 0, .3f), .11f * s, deep, 12);
    dl->AddLine(P(c, s, 0, .3f), P(c, s, 0, .52f), deep, .08f * s);
}

// the lock-on: a padlock by the target camera's icon (its zone), lit (and a ring round the icon) while on
void lock_mark(ImDrawList *dl, const Rect &target, bool on, float a, float h, uint64_t now) {
    ImVec2 c((target.x0 + target.x1) * .5f, (target.y0 + target.y1) * .5f);
    float rr = std::max(target.x1 - target.x0, target.y1 - target.y0) * .52f;
    if (on) dl->AddCircle(c, rr, fade(GLOW, a * (.6f + .3f * sinf(now / 1e9f * 4))), 40, .005f * h);
    ImVec2 pc(target.x0 - .02f * h, target.y1 - .02f * h);
    dl->AddCircleFilled(pc, .022f * h, fade(DEEP, a * .85f), 20);
    dl->AddCircle(pc, .022f * h, fade(GOLD, a), 20, .003f * h);
    padlock(dl, pc, .026f * h, on, fade(on ? GLOW : INK, a), fade(DEEP, a));
}

void icon(ImDrawList *dl, Icon ic, ImVec2 c, float r, ImU32 col, ImU32 deep) {
    float s = r * .62f;
    switch (ic) {
    case I_DODGE: {  // a roll: a curled arrow with motion lines
        float ar = .5f * s;
        dl->PathArcTo(ImVec2(c.x + .08f * s, c.y), ar, PI * .85f, PI * 2.35f, 24);
        dl->PathStroke(col, 0, .16f * s);
        float a = PI * 2.35f;
        ImVec2 e(c.x + .08f * s + cosf(a) * ar, c.y + sinf(a) * ar);
        dl->AddTriangleFilled(ImVec2(e.x - .26f * s, e.y - .16f * s), ImVec2(e.x + .12f * s, e.y - .3f * s),
                              ImVec2(e.x + .08f * s, e.y + .12f * s), col);
        for (int i = 0; i < 3; i++)
            dl->AddLine(P(c, s, -.95f, -.25f + .25f * i), P(c, s, -.62f, -.25f + .25f * i), col, .09f * s);
        break;
    }
    case I_SHIELD: {
        ImVec2 sh[5] = {P(c, s, -.55f, -.62f), P(c, s, .55f, -.62f), P(c, s, .55f, .05f), P(c, s, 0, .7f), P(c, s, -.55f, .05f)};
        dl->AddConvexPolyFilled(sh, 5, col);
        dl->AddLine(P(c, s, 0, -.45f), P(c, s, 0, .45f), deep, .1f * s);
        dl->AddLine(P(c, s, -.36f, -.12f), P(c, s, .36f, -.12f), deep, .1f * s);
        break;
    }
    case I_BURST: {  // a special move: an eight-point burst
        ImVec2 pts[16];
        for (int i = 0; i < 16; i++) {
            float a = i * PI / 8 - PI / 2, rr = (i % 2 ? .34f : .82f) * s;
            pts[i] = ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr);
        }
        for (int i = 0; i < 16; i += 2) {
            dl->AddTriangleFilled(c, pts[(i + 15) % 16], pts[i], col);
            dl->AddTriangleFilled(c, pts[i], pts[i + 1], col);
        }
        dl->AddCircleFilled(c, .2f * s, deep, 12);
        break;
    }
    case I_TARGET_SELECT: {  // the next target: a reticle between two arrows
        dl->AddCircle(c, .38f * s, col, 20, .11f * s);
        dl->AddCircleFilled(c, .1f * s, col);
        for (int i = 0; i < 4; i++) {
            float a = i * PI * .5f;
            dl->AddLine(ImVec2(c.x + cosf(a) * .26f * s, c.y + sinf(a) * .26f * s),
                        ImVec2(c.x + cosf(a) * .52f * s, c.y + sinf(a) * .52f * s), col, .1f * s);
        }
        for (int d = -1; d <= 1; d += 2)
            dl->AddTriangleFilled(P(c, s, .95f * d, 0), P(c, s, .68f * d, -.24f), P(c, s, .68f * d, .24f), col);
        break;
    }
    case I_CAMERA: {  // centre the camera: a camera
        dl->AddRectFilled(P(c, s, -.66f, -.36f), P(c, s, .66f, .44f), col, .12f * s);
        dl->AddRectFilled(P(c, s, -.24f, -.54f), P(c, s, .24f, -.3f), col, .06f * s);
        dl->AddCircleFilled(P(c, s, 0, .05f), .27f * s, deep, 18);
        dl->AddCircleFilled(P(c, s, 0, .05f), .15f * s, col, 14);
        break;
    }
    case I_MENU:
        for (int i = -1; i <= 1; i++) dl->AddRectFilled(P(c, s, -.55f, i * .38f - .08f), P(c, s, .55f, i * .38f + .08f), col, .05f * s);
        break;
    case I_MAP: {
        ImVec2 m[6] = {P(c, s, -.7f, -.45f), P(c, s, -.24f, -.6f), P(c, s, .24f, -.45f), P(c, s, .7f, -.6f),
                       P(c, s, .7f, .5f), P(c, s, -.7f, .62f)};
        dl->AddConvexPolyFilled(m, 6, col);
        dl->AddLine(P(c, s, -.24f, -.6f), P(c, s, -.24f, .52f), deep, .07f * s);
        dl->AddLine(P(c, s, .24f, -.45f), P(c, s, .24f, .62f), deep, .07f * s);
        dl->AddCircleFilled(P(c, s, .45f, -.05f), .1f * s, RED);
        break;
    }
    case I_MINUS:
        dl->AddRectFilled(P(c, s, -.58f, -.12f), P(c, s, .58f, .12f), col, .06f * s);
        break;
    case I_RUN: {  // a running boot with speed lines
        ImVec2 boot[6] = {P(c, s, -.12f, -.62f), P(c, s, .22f, -.62f), P(c, s, .22f, .2f), P(c, s, .62f, .32f),
                          P(c, s, .62f, .56f), P(c, s, -.18f, .56f)};
        dl->AddConvexPolyFilled(boot, 3, col);
        dl->AddRectFilled(P(c, s, -.18f, -.62f), P(c, s, .22f, .56f), col, .06f * s);
        dl->AddRectFilled(P(c, s, -.18f, .22f), P(c, s, .62f, .56f), col, .12f * s);
        for (int i = 0; i < 3; i++)
            dl->AddLine(P(c, s, -.88f, -.36f + .3f * i), P(c, s, -.36f, -.36f + .3f * i), col, .1f * s);
        break;
    }
    case I_SHEATHE: {  // a sword sliding down into its scabbard
        ImVec2 sc[4] = {P(c, s, -.22f, -.08f), P(c, s, .22f, -.08f), P(c, s, .16f, .82f), P(c, s, -.16f, .82f)};
        dl->AddConvexPolyFilled(sc, 4, col);  // (the scabbard)
        dl->AddLine(P(c, s, -.3f, .12f), P(c, s, .3f, .12f), deep, .07f * s);  // (its band)
        dl->AddRectFilled(P(c, s, -.42f, -.26f), P(c, s, .42f, -.14f), col, .04f * s);  // (the guard, at its mouth)
        dl->AddRectFilled(P(c, s, -.07f, -.62f), P(c, s, .07f, -.26f), col);  // (the grip)
        dl->AddCircleFilled(P(c, s, 0, -.68f), .1f * s, col, 14);  // (the pommel)
        for (int d = -1; d <= 1; d += 2)  // (down arrows beside it: in it goes)
            dl->AddTriangleFilled(P(c, s, .62f * d, .38f), P(c, s, .44f * d, .1f), P(c, s, .8f * d, .1f), col);
        break;
    }
    case I_SHOT: {  // a shell flying up and to the right, its trail behind it
        ImVec2 u(.7071f, -.7071f), n(.7071f, .7071f), b = add(c, u, -.12f * s), m = add(c, u, .3f * s), t = add(c, u, .78f * s);
        float w = .27f * s;
        ImVec2 body[5] = {add(b, n, w), add(m, n, w), t, add(m, n, -w), add(b, n, -w)};
        dl->AddConvexPolyFilled(body, 5, col);
        dl->AddLine(add(add(b, u, .17f * s), n, w), add(add(b, u, .17f * s), n, -w), deep, .07f * s);  // (its band)
        for (int k = -1; k <= 1; k++) {
            ImVec2 o = add(add(c, u, -.34f * s), n, k * .24f * s);
            dl->AddLine(o, add(o, u, -(k ? .34f : .54f) * s), fade(col, k ? .7f : 1.0f), .09f * s);
        }
        break;
    }
    case I_BOW: {  // a bow drawn, up and to the right, the arrow on its string
        const float at = -PI * .25f;
        ImVec2 u(.7071f, -.7071f), n(.7071f, .7071f), o = add(c, u, -.5f * s), nock = add(c, u, -.42f * s);
        float r0 = .95f * s, sweep = 1.0f;
        dl->PathArcTo(o, r0, at - sweep, at + sweep, 22);
        dl->PathStroke(col, 0, .13f * s);
        ImVec2 e0(o.x + cosf(at - sweep) * r0, o.y + sinf(at - sweep) * r0), e1(o.x + cosf(at + sweep) * r0, o.y + sinf(at + sweep) * r0);
        dl->AddLine(e0, nock, col, .05f * s);
        dl->AddLine(nock, e1, col, .05f * s);
        ImVec2 tip = add(c, u, .92f * s), sh = add(c, u, .56f * s);
        dl->AddLine(add(c, u, -.62f * s), sh, col, .08f * s);
        dl->AddTriangleFilled(tip, add(sh, n, .17f * s), add(sh, n, -.17f * s), col);
        dl->AddLine(add(add(c, u, -.62f * s), n, .13f * s), add(c, u, -.42f * s), col, .06f * s);  // (its fletching)
        dl->AddLine(add(add(c, u, -.62f * s), n, -.13f * s), add(c, u, -.42f * s), col, .06f * s);
        break;
    }
    case I_RELOAD: {  // a shell in a turning arrow
        float ar = .68f * s, a1 = PI * 1.55f;
        dl->PathArcTo(c, ar, -PI * .35f, a1, 28);
        dl->PathStroke(col, 0, .13f * s);
        ImVec2 e(c.x + cosf(a1) * ar, c.y + sinf(a1) * ar);
        dl->AddTriangleFilled(ImVec2(e.x + .34f * s, e.y + .02f * s), ImVec2(e.x - .1f * s, e.y - .3f * s), ImVec2(e.x - .1f * s, e.y + .3f * s), col);
        ImVec2 sh[5] = {P(c, s, -.17f, .34f), P(c, s, -.17f, -.1f), P(c, s, 0, -.38f), P(c, s, .17f, -.1f), P(c, s, .17f, .34f)};
        dl->AddConvexPolyFilled(sh, 5, col);
        break;
    }
    case I_SCOPE: {  // the view through a scope
        dl->AddCircle(c, .66f * s, col, 28, .13f * s);
        dl->AddCircleFilled(c, .09f * s, col, 12);
        for (int i = 0; i < 4; i++) {
            float a = i * PI * .5f;
            dl->AddLine(ImVec2(c.x + cosf(a) * .28f * s, c.y + sinf(a) * .28f * s), ImVec2(c.x + cosf(a) * .94f * s, c.y + sinf(a) * .94f * s), col, .1f * s);
        }
        break;
    }
    case I_CROUCH: {  // down on one knee: two chevrons over the ground
        for (int k = 0; k < 2; k++) {
            float y = -.5f + k * .42f;
            ImVec2 v[3] = {P(c, s, -.6f, y), P(c, s, 0, y + .42f), P(c, s, .6f, y)};
            dl->AddPolyline(v, 3, col, 0, .17f * s);
        }
        dl->AddRectFilled(P(c, s, -.7f, .5f), P(c, s, .7f, .63f), col, .05f * s);
        break;
    }
    case I_COATING: {  // a flask
        dl->AddRectFilled(P(c, s, -.17f, -.72f), P(c, s, .17f, -.2f), col, .03f * s);
        dl->AddRectFilled(P(c, s, -.27f, -.8f), P(c, s, .27f, -.66f), col, .04f * s);
        dl->AddCircleFilled(P(c, s, 0, .22f), .55f * s, col, 24);
        dl->AddCircleFilled(P(c, s, 0, .22f), .36f * s, deep, 20);
        dl->AddCircleFilled(P(c, s, 0, .3f), .24f * s, col, 16);
        break;
    }
    case I_MELEE:
        sword(dl, P(c, s, -.08f, .08f), s * 1.05f, 1, -1, col);
        break;
    case I_GATHER: {  // an open hand
        for (int i = 0; i < 4; i++) {
            float x = -.42f + i * .28f, top = i == 0 || i == 3 ? -.42f : -.66f;
            dl->AddLine(P(c, s, x, .1f), P(c, s, x, top), col, .2f * s);
            dl->AddCircleFilled(P(c, s, x, top), .1f * s, col, 10);
        }
        dl->AddLine(P(c, s, -.5f, .3f), P(c, s, -.84f, -.06f), col, .2f * s);
        dl->AddRectFilled(P(c, s, -.52f, 0), P(c, s, .52f, .66f), col, .18f * s);
        break;
    }
    case I_UP: case I_DOWN: {
        float d = ic == I_UP ? -1.0f : 1.0f;
        dl->AddTriangleFilled(P(c, s, 0, d * .78f), P(c, s, -.72f, -d * .3f), P(c, s, .72f, -d * .3f), col);
        dl->AddRectFilled(P(c, s, -.5f, d > 0 ? -.72f : .44f), P(c, s, .5f, d > 0 ? -.5f : .66f), fade(col, .7f), .04f * s);
        break;
    }
    case I_GEAR:
        dl->AddCircleFilled(c, .5f * s, col, 24);
        for (int i = 0; i < 8; i++) {
            float a = i * PI * .25f;
            dl->AddLine(ImVec2(c.x + cosf(a) * .4f * s, c.y + sinf(a) * .4f * s),
                        ImVec2(c.x + cosf(a) * .72f * s, c.y + sinf(a) * .72f * s), col, .24f * s);
        }
        dl->AddCircleFilled(c, .22f * s, deep, 16);
        break;
    default:
        break;
    }
}

// a disc: a soft shadow, the bronze ring with a gold rule and four studs, the dark face; pressed, it lights up
void disc(ImDrawList *dl, ImVec2 c, float r, bool pressed, float a) {
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * .07f), r * 1.05f, fade(IM_COL32(0, 0, 0, 120), a), 40);
    dl->AddCircleFilled(c, r, fade(BRONZE, a * .95f), 40);
    dl->AddCircle(c, r * .91f, fade(GOLD, a), 40, r * .06f);
    dl->AddCircleFilled(c, r * .84f, fade(pressed ? IM_COL32(92, 62, 22, 255) : DEEP, a * (pressed ? .92f : .72f)), 40);
    dl->AddCircleFilled(ImVec2(c.x, c.y - r * .2f), r * .6f, fade(IM_COL32(255, 240, 210, 255), a * .05f), 32);
    for (int i = 0; i < 4; i++) {  // (studs on the ring)
        float an = PI * .25f + i * PI * .5f, sr = r * .955f;
        ImVec2 p(c.x + cosf(an) * sr, c.y + sinf(an) * sr);
        float d = r * .07f;
        ImVec2 q[4] = {ImVec2(p.x, p.y - d), ImVec2(p.x + d, p.y), ImVec2(p.x, p.y + d), ImVec2(p.x - d, p.y)};
        dl->AddConvexPolyFilled(q, 4, fade(GOLD, a));
    }
    if (pressed) {
        dl->AddCircle(c, r * 1.1f, fade(GLOW, a * .7f), 40, r * .08f);
        dl->AddCircle(c, r * 1.2f, fade(GLOW, a * .25f), 40, r * .06f);
    }
}

void badge(ImDrawList *dl, ImFont *font, ImVec2 b, float br, const char *letter, float a) {
    if (!letter[0]) return;
    dl->AddCircleFilled(b, br, fade(IM_COL32(18, 12, 9, 255), a), 20);
    dl->AddCircle(b, br, fade(GOLD, a), 20, br * .14f);
    float fs = br * (strlen(letter) > 1 ? 1.0f : 1.3f);
    ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, letter);
    dl->AddText(font, fs, ImVec2(b.x - ts.x * .5f, b.y - ts.y * .52f), fade(INK, a), letter);
}

// the attack pill's outline (a capsule along PILL_U), as the current path
void capsule_path(ImDrawList *dl, ImVec2 c, float half, float r) {
    float an = atan2f(PILL_U.y, PILL_U.x);
    dl->PathArcTo(add(c, PILL_U, half), r, an - PI / 2, an + PI / 2, 20);
    dl->PathArcTo(add(c, PILL_U, -half), r, an + PI / 2, an + 3 * PI / 2, 20);
}
// one half (0 upper, 1 lower) of its face, from the seam out, as the current path
void half_path(ImDrawList *dl, ImVec2 c, float half, float r, int which, float seam) {
    float an = atan2f(PILL_U.y, PILL_U.x), d = which ? -1.0f : 1.0f;
    ImVec2 n(-PILL_U.y, PILL_U.x), s0 = add(c, PILL_U, d * seam);
    dl->PathLineTo(add(s0, n, which ? r : -r));
    dl->PathArcTo(add(c, PILL_U, d * half), r, an + (which ? PI / 2 : -PI / 2), an + (which ? 3 * PI / 2 : PI / 2), 20);
    dl->PathLineTo(add(s0, n, which ? -r : r));
}

void pill_body(ImDrawList *dl, Geo g, int pressed_mask, float a) {
    float r = g.r, half = r * PILL_HALF, seam = half * PILL_SEAM;
    ImVec2 c = g.c, n(-PILL_U.y, PILL_U.x);
    capsule_path(dl, ImVec2(c.x, c.y + r * .07f), half, r * 1.05f);
    dl->PathFillConvex(fade(IM_COL32(0, 0, 0, 120), a));
    capsule_path(dl, c, half, r);
    dl->PathFillConvex(fade(BRONZE, a * .95f));
    capsule_path(dl, c, half, r * .91f);
    dl->PathStroke(fade(GOLD, a), ImDrawFlags_Closed, r * .06f);
    for (int h = 0; h < 2; h++) {
        bool on = pressed_mask & (1 << h);
        half_path(dl, c, half, r * .84f, h, seam);
        dl->PathFillConvex(fade(on ? IM_COL32(92, 62, 22, 255) : DEEP, a * (on ? .92f : .72f)));
    }
    // the seam: both buttons; a gold rule each side and a stud in the middle
    bool both = (pressed_mask & 3) == 3;
    ImVec2 q[4] = {add(add(c, PILL_U, seam), n, r * .84f), add(add(c, PILL_U, seam), n, -r * .84f),
                   add(add(c, PILL_U, -seam), n, -r * .84f), add(add(c, PILL_U, -seam), n, r * .84f)};
    dl->AddConvexPolyFilled(q, 4, fade(both ? IM_COL32(120, 84, 28, 255) : IM_COL32(52, 36, 22, 255), a * .85f));
    dl->AddLine(add(add(c, PILL_U, seam), n, r * .86f), add(add(c, PILL_U, seam), n, -r * .86f), fade(GOLD, a), r * .045f);
    dl->AddLine(add(add(c, PILL_U, -seam), n, r * .86f), add(add(c, PILL_U, -seam), n, -r * .86f), fade(GOLD, a), r * .045f);
    float d = r * .13f;
    ImVec2 st[4] = {ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y)};
    dl->AddConvexPolyFilled(st, 4, fade(both ? GLOW : GOLD, a));
    if (pressed_mask & 3) {
        capsule_path(dl, c, half, r * 1.1f);
        dl->PathStroke(fade(GLOW, a * .6f), ImDrawFlags_Closed, r * .08f);
    }
}

void pill_letters(ImDrawList *dl, ImFont *font, Geo g, const char *upper, const char *lower, float a) {
    float r = g.r, fs = r * .38f;
    ImVec2 n(-PILL_U.y, PILL_U.x);
    for (int h = 0; h < 2; h++) {
        const char *t = h ? lower : upper;
        float sgn = h ? -1.0f : 1.0f;
        ImVec2 at = add(add(g.c, PILL_U, sgn * r * 2.3f), n, sgn * r * .5f), ts = font->CalcTextSizeA(fs, FLT_MAX, 0, t);
        dl->AddText(font, fs, ImVec2(at.x - ts.x * .5f + fs * .05f, at.y - ts.y * .5f + fs * .06f), fade(IM_COL32(0, 0, 0, 255), a * .8f), t);
        dl->AddText(font, fs, ImVec2(at.x - ts.x * .5f, at.y - ts.y * .5f), fade(GOLD, a), t);
    }
}

void pill(ImDrawList *dl, ImFont *font, Geo g, int pressed_mask, float a) {
    float r = g.r;
    ImVec2 c = g.c;
    pill_body(dl, g, pressed_mask, a);
    // the icons in the middle of each half (between the seam and the round end: 1.5 radii from the centre), along the
    // pill: a great sword for the heavy attack on the upper half, a small sword for the light one on the lower; the
    // game's letter engraved by the outer end of each half, on the side away from the guard
    ImU32 cu = fade(pressed_mask & 1 ? GLOW : INK, a), cl = fade(pressed_mask & 2 ? GLOW : INK, a);
    sword_along(dl, add(c, PILL_U, r * 1.5f), PILL_U, r * 2.05f, r * .46f, r * .95f, cu, fade(DEEP, a), true);
    sword_along(dl, add(c, PILL_U, -r * 1.5f), PILL_U, r * 1.25f, r * .24f, r * .6f, cl, fade(DEEP, a), false);
    pill_letters(dl, font, g, "A", "X", a);
}

// L and R: tabs, as the game's own pages, an arrow to their side
void tab(ImDrawList *dl, ImFont *font, Geo g, bool left, const char *t, bool pressed, float a) {
    float w = g.r * TAB_W, h = g.r * TAB_H, rd = h * .55f;
    ImVec2 p0(g.c.x - w, g.c.y - h), p1(g.c.x + w, g.c.y + h), in(g.r * .13f, g.r * .13f);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + g.r * .08f), ImVec2(p1.x, p1.y + g.r * .1f), fade(IM_COL32(0, 0, 0, 120), a), rd);
    dl->AddRectFilled(p0, p1, fade(BRONZE, a * .95f), rd);
    dl->AddRect(ImVec2(p0.x + in.x * .45f, p0.y + in.y * .45f), ImVec2(p1.x - in.x * .45f, p1.y - in.y * .45f), fade(pressed ? GLOW : GOLD, a), rd * .85f, 0, g.r * .06f);
    dl->AddRectFilled(ImVec2(p0.x + in.x, p0.y + in.y), ImVec2(p1.x - in.x, p1.y - in.y),
                      fade(pressed ? IM_COL32(92, 62, 22, 255) : DEEP, a * (pressed ? .92f : .72f)), rd * .7f);
    float d = left ? -1.0f : 1.0f, ax = g.c.x + d * w * .62f, ch = g.r * .34f;
    dl->AddTriangleFilled(ImVec2(ax + d * ch * .8f, g.c.y), ImVec2(ax - d * ch * .3f, g.c.y - ch), ImVec2(ax - d * ch * .3f, g.c.y + ch),
                          fade(pressed ? GLOW : GOLD, a));
    float fs = g.r * (t[1] ? .8f : .95f);  // (ZL / ZR: two letters in the same tab)
    ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, t), at(g.c.x - d * g.r * .28f - ts.x * .5f, g.c.y - ts.y * .52f);
    dl->AddText(font, fs, ImVec2(at.x + fs * .04f, at.y + fs * .05f), fade(IM_COL32(0, 0, 0, 255), a * .7f), t);
    dl->AddText(font, fs, at, fade(pressed ? GLOW : INK, a), t);
}

// The band's outline between two points along it, f off its middle line both ways, as the current path; a round end
// where it is one of the band's own
void band_path(ImDrawList *dl, Geo g, float s0, float s1, float f, bool cap0, bool cap1) {
    const int N = 44;
    for (int i = 0; i <= N; i++) { ImVec2 p = band_point(g, s0 + (s1 - s0) * i / N, f); dl->PathLineTo(p); }
    if (cap1) dl->PathArcTo(band_point(g, s1, 0), f, 0, -PI, 14);
    for (int i = N; i >= 0; i--) { ImVec2 p = band_point(g, s0 + (s1 - s0) * i / N, -f); dl->PathLineTo(p); }
    if (cap0) dl->PathArcTo(band_point(g, s0, 0), f, -PI * .5f, -PI * 1.5f, 14);
}
// Filled as a strip of quads (no triangulation of a shape so bent, which took in its inside; no edge smoothing between
// them, which would show as lines) with round ends
void band_fill(ImDrawList *dl, Geo g, float s0, float s1, float f, ImU32 col, bool cap0, bool cap1) {
    const int N = 44;
    ImDrawListFlags keep = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (int i = 0; i < N; i++) {
        float a = s0 + (s1 - s0) * i / N, b = s0 + (s1 - s0) * (i + 1) / N;
        dl->AddQuadFilled(band_point(g, a, f), band_point(g, b, f), band_point(g, b, -f), band_point(g, a, -f), col);
    }
    if (cap1) { dl->PathArcTo(band_point(g, s1, 0), f, 0, -PI, 16); dl->PathFillConvex(col); }
    if (cap0) { dl->PathArcTo(band_point(g, s0, 0), f, -PI * .5f, -PI * 1.5f, 16); dl->PathFillConvex(col); }
    dl->Flags = keep;
}
ImVec2 band_along(Geo g, float s) {  // its direction at s
    ImVec2 a = band_point(g, s - g.r * .02f, 0), b = band_point(g, s + g.r * .02f, 0);
    float dx = b.x - a.x, dy = b.y - a.y, n = std::max(1e-3f, hypotf(dx, dy));
    return ImVec2(dx / n, dy / n);
}
// All three in one band round the corner: the guard along the bottom, the light attack at the bend, the heavy one up the
// side, the seam between the attacks pressing both; bits of mask: 1 heavy, 2 light, 4 guard
void corner_band(ImDrawList *dl, ImFont *font, Geo g, int mask, float a) {
    float t = g.r * BAND_T, len = g.r * BAND_LEN, s0 = len * BAND_GUARD, s1 = len * BAND_SEAM0, s2 = len * BAND_SEAM1, e = .84f * t;
    ImU32 lit = IM_COL32(92, 62, 22, 255);
    band_fill(dl, {ImVec2(g.c.x, g.c.y + t * .08f), g.r}, 0, len, t * 1.04f, fade(IM_COL32(0, 0, 0, 120), a), true, true);
    band_fill(dl, g, 0, len, t, fade(BRONZE, a * .95f), true, true);
    band_path(dl, g, 0, len, t, true, true);
    dl->PathStroke(fade(BRONZE, a * .95f), ImDrawFlags_Closed, 1.2f);  // (its outer edges smooth)
    band_path(dl, g, 0, len, t * .91f, true, true);
    dl->PathStroke(fade(GOLD, a), ImDrawFlags_Closed, t * .06f);
    auto face = [&](float from, float to, bool c0, bool c1, bool on) {
        band_fill(dl, g, from, to, e, fade(on ? lit : DEEP, a * (on ? .92f : .72f)), c0, c1);
    };
    face(0, s0, true, false, mask & 4);
    face(s0, s1, false, false, mask & 2);
    face(s2, len, false, true, mask & 1);
    bool both = (mask & 3) == 3;
    band_fill(dl, g, s1, s2, e, fade(both ? IM_COL32(120, 84, 28, 255) : IM_COL32(52, 36, 22, 255), a * .85f), false, false);
    for (float at : {s0, s1, s2}) dl->AddLine(band_point(g, at, t * .86f), band_point(g, at, -t * .86f), fade(GOLD, a), t * .045f);
    ImVec2 m = band_point(g, (s1 + s2) * .5f, 0);
    float d = t * .13f;
    ImVec2 st[4] = {ImVec2(m.x, m.y - d), ImVec2(m.x + d, m.y), ImVec2(m.x, m.y + d), ImVec2(m.x - d, m.y)};
    dl->AddConvexPolyFilled(st, 4, fade(both ? GLOW : GOLD, a));
    if (mask) {
        band_path(dl, g, 0, len, t * 1.1f, true, true);
        dl->PathStroke(fade(GLOW, a * .6f), ImDrawFlags_Closed, t * .08f);
    }
    ImU32 ch = fade(mask & 1 ? GLOW : INK, a), cl = fade(mask & 2 ? GLOW : INK, a), cg = fade(mask & 4 ? GLOW : INK, a);
    float sh = (s2 + len) * .5f, sl = (s0 + s1) * .5f, sg = s0 * .52f;
    sword_along(dl, band_point(g, sh, 0), band_along(g, sh), t * 2.05f, t * .46f, t * .95f, ch, fade(DEEP, a), true);
    sword_along(dl, band_point(g, sl, 0), band_along(g, sl), t * 1.25f, t * .24f, t * .6f, cl, fade(DEEP, a), false);
    icon(dl, I_SHIELD, band_point(g, sg, 0), t * .95f, cg, fade(DEEP, a));
    float fs = t * .38f;
    const struct { const char *t; ImVec2 at; } letters[3] = {
        {"A", band_point(g, sh + t * 1.0f, -t * .52f)}, {"X", band_point(g, sl - t * .95f, -t * .52f)}, {"R", band_point(g, sg - t * .95f, -t * .52f)}};
    for (const auto &l : letters) {
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, l.t);
        dl->AddText(font, fs, ImVec2(l.at.x - ts.x * .5f + fs * .05f, l.at.y - ts.y * .5f + fs * .06f), fade(IM_COL32(0, 0, 0, 255), a * .8f), l.t);
        dl->AddText(font, fs, ImVec2(l.at.x - ts.x * .5f, l.at.y - ts.y * .5f), fade(GOLD, a), l.t);
    }
}

// the d-pad's directions: right, left, up, down (the part's bits)
const float DPAD_DIR[4][2] = {{1, 0}, {-1, 0}, {0, -1}, {0, 1}};
// the direction pressed: its quarter of the well lit, and a gold arc outside the rim, which shows round the thumb
void dpad_lit(ImDrawList *dl, ImVec2 c, float r, int part, float a) {
    for (int i = 0; i < 4; i++) {
        if (!(part & (1 << i))) continue;
        float an = atan2f(DPAD_DIR[i][1], DPAD_DIR[i][0]);
        dl->PathLineTo(c);
        dl->PathArcTo(c, r * .84f, an - PI * .25f, an + PI * .25f, 14);
        dl->PathFillConvex(fade(IM_COL32(92, 62, 22, 255), a * .9f));
        dl->PathArcTo(c, r * 1.1f, an - PI * .23f, an + PI * .23f, 14);
        dl->PathStroke(fade(GLOW, a * .85f), ImDrawFlags_None, r * .07f);
        dl->PathArcTo(c, r * 1.2f, an - PI * .18f, an + PI * .18f, 12);
        dl->PathStroke(fade(GLOW, a * .3f), ImDrawFlags_None, r * .05f);
    }
}
// the d-pad: a round bronze well like the other buttons, a long cross on it with a cream arrow near each tip
void dpad(ImDrawList *dl, Geo g, int part, float a) {
    ImVec2 c = g.c;
    float r = g.r, hw = r * .3f, len = r * .8f, edge = r * .045f;  // (an arm's half width and length, its bronze edge)
    const auto &dir = DPAD_DIR;
    disc(dl, c, r, false, a);
    auto lit = [&](int i) { return (part & (1 << i)) != 0; };
    dpad_lit(dl, c, r, part, a);
    // (the arms out from a square in the middle, one piece: drawn from the centre they would overlap there, and show)
    auto arm = [&](const float *d, float from, float to, float w, ImU32 col) {
        ImVec2 p0(c.x + d[0] * from - (d[1] != 0 ? w : 0), c.y + d[1] * from - (d[0] != 0 ? w : 0));
        ImVec2 p1(c.x + d[0] * to + (d[1] != 0 ? w : 0), c.y + d[1] * to + (d[0] != 0 ? w : 0));
        ImDrawFlags tip = d[0] > 0 ? ImDrawFlags_RoundCornersRight : d[0] < 0 ? ImDrawFlags_RoundCornersLeft
                        : d[1] < 0 ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersBottom;
        dl->AddRectFilled(ImVec2(std::min(p0.x, p1.x), std::min(p0.y, p1.y)), ImVec2(std::max(p0.x, p1.x), std::max(p0.y, p1.y)), col, w * .3f, tip);
    };
    const ImU32 face = fade(IM_COL32(60, 43, 28, 255), a * .95f), pressed = fade(IM_COL32(128, 88, 32, 255), a);
    float out = hw + edge;
    for (int i = 0; i < 4; i++) arm(dir[i], out, len + edge, out, fade(BRONZE, a));
    dl->AddRectFilled(ImVec2(c.x - out, c.y - out), ImVec2(c.x + out, c.y + out), fade(BRONZE, a));
    for (int i = 0; i < 4; i++) arm(dir[i], hw, len, hw, lit(i) ? pressed : face);
    dl->AddRectFilled(ImVec2(c.x - hw, c.y - hw), ImVec2(c.x + hw, c.y + hw), face);
    dl->AddCircleFilled(c, hw * .68f, fade(DEEP, a), 20);
    float d = hw * .36f;
    ImVec2 q[4] = {ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y)};
    dl->AddConvexPolyFilled(q, 4, fade(GOLD, a));
    for (int i = 0; i < 4; i++) {  // (the arrows; pressed, a little farther out)
        float dx = dir[i][0], dy = dir[i][1], px = -dy, py = dx, s = hw * .62f, at = r * (lit(i) ? .75f : .72f);
        ImVec2 tip(c.x + dx * at, c.y + dy * at), b0(tip.x - dx * s * 1.25f + px * s, tip.y - dy * s * 1.25f + py * s),
            b1(tip.x - dx * s * 1.25f - px * s, tip.y - dy * s * 1.25f - py * s);
        dl->AddTriangleFilled(tip, b0, b1, fade(lit(i) ? GLOW : INK, a));
    }
}

void stick(ImDrawList *dl, ImVec2 base, ImVec2 knob, float r, float a, bool on) {
    float kr = r * KNOB;
    dl->AddCircleFilled(base, r, fade(IM_COL32(10, 7, 5, 255), a * (on ? .42f : .26f)), 48);
    dl->AddCircle(base, r, fade(BRONZE, a), 48, r * .07f);
    dl->AddCircle(base, r * .955f, fade(GOLD, a * .85f), 48, r * .025f);
    for (int i = 0; i < 8; i++) {  // (eight notches: the directions)
        float an = i * PI * .25f;
        ImVec2 o(base.x + cosf(an) * r * .82f, base.y + sinf(an) * r * .82f);
        float d = r * (i % 2 ? .035f : .06f);
        ImVec2 q[4] = {ImVec2(o.x + cosf(an) * d * 1.6f, o.y + sinf(an) * d * 1.6f), ImVec2(o.x - sinf(an) * d, o.y + cosf(an) * d),
                       ImVec2(o.x - cosf(an) * d * 1.6f, o.y - sinf(an) * d * 1.6f), ImVec2(o.x + sinf(an) * d, o.y - cosf(an) * d)};
        dl->AddConvexPolyFilled(q, 4, fade(GOLD, a * .8f));
    }
    disc(dl, knob, kr, on, a);
    float d = kr * .32f;
    ImVec2 q[4] = {ImVec2(knob.x, knob.y - d), ImVec2(knob.x + d, knob.y), ImVec2(knob.x, knob.y + d), ImVec2(knob.x - d, knob.y)};
    dl->AddConvexPolyFilled(q, 4, fade(GOLD, a));
}

// The menu pad: the stick's base, smaller, with four arrows for its notches (it is the d-pad); the direction pressed
// lit as on the d-pad, the knob under the thumb (kept inside the rim)
void menu_pad(ImDrawList *dl, Geo g, ImVec2 knob, int part, float a, bool on) {
    ImVec2 c = g.c;
    float r = g.r;
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * .07f), r * 1.05f, fade(IM_COL32(0, 0, 0, 120), a), 40);
    dl->AddCircleFilled(c, r, fade(IM_COL32(10, 7, 5, 255), a * (on ? .55f : .42f)), 48);
    dpad_lit(dl, c, r, part, a);
    dl->AddCircle(c, r, fade(BRONZE, a), 48, r * .08f);
    dl->AddCircle(c, r * .95f, fade(GOLD, a * .85f), 48, r * .03f);
    for (int i = 0; i < 4; i++) {  // (the arrows, pointing out near the rim; pressed, lit and a little farther out)
        bool lit = (part & (1 << i)) != 0;
        float dx = DPAD_DIR[i][0], dy = DPAD_DIR[i][1], px = -dy, py = dx, s = r * .14f, at = r * (lit ? .86f : .82f);
        ImVec2 tip(c.x + dx * at, c.y + dy * at), b0(tip.x - dx * s * 1.3f + px * s, tip.y - dy * s * 1.3f + py * s),
            b1(tip.x - dx * s * 1.3f - px * s, tip.y - dy * s * 1.3f - py * s);
        dl->AddTriangleFilled(tip, b0, b1, fade(lit ? GLOW : GOLD, a * (lit ? 1.0f : .85f)));
    }
    float kr = r * KNOB;
    disc(dl, knob, kr, part != 0, a);
    float d = kr * .32f;
    ImVec2 q[4] = {ImVec2(knob.x, knob.y - d), ImVec2(knob.x + d, knob.y), ImVec2(knob.x, knob.y + d), ImVec2(knob.x - d, knob.y)};
    dl->AddConvexPolyFilled(q, 4, fade(GOLD, a));
}

void combo_label(int i, char *out, size_t size) { config_combo_text(config_live_touch_combo(i - C_COMBO), out, size); }

// What a control of the ranged layout shows now: the weapon in hand, the game's Bow Controls, out or sheathed (the editor,
// with no such weapon in hand, shows a bowgun's, out)
void ranged_face(int i, int drawn, Icon *ic, char *letter, size_t size) {
    bool bow = g_ranged.load() == WEAPON_BOW, out = drawn == 1;
    uint64_t b = ranged_buttons(i, drawn);
    *ic = i == C_RFIRE ? (bow ? I_BOW : I_SHOT)
        : i == C_RSECOND ? (!out ? I_GATHER : bow ? I_MELEE : I_RELOAD)
        : i == C_RSPECIAL ? (bow ? I_COATING : I_CROUCH)
                          : DEFS[i].icon;
    snprintf(letter, size, "%s", i == C_AMMO_UP || i == C_AMMO_DOWN ? "" : (b & B(BTN_X)) && (b & B(BTN_A)) ? "X+A" : b & B(BTN_A) ? "A"
                                  : b & B(BTN_X) ? "X" : b & B(BTN_R) ? "R" : DEFS[i].letter);
}

void control(ImDrawList *dl, ImFont *font, const Layout &l, int i, float w, float h, const State &s, float a) {
    Geo g = place(l, i, w, h);
    if (i == C_STICK) {
        if (s.stick_on && !s.edit) {
            stick(dl, ImVec2(s.bx, s.by), ImVec2(s.kx, s.ky), g.r, a, true);
            if (s.run) {  // (running: a gold ring round the rim, and streaks behind the knob)
                dl->AddCircle(ImVec2(s.bx, s.by), g.r * 1.12f, fade(GLOW, a * .85f), 48, g.r * .07f);
                float dx = s.kx - s.bx, dy = s.ky - s.by, n = std::max(1.0f, hypotf(dx, dy));
                dx /= n, dy /= n;
                for (int i = -1; i <= 1; i++) {
                    ImVec2 o(s.kx - dy * i * g.r * .22f, s.ky + dx * i * g.r * .22f);
                    dl->AddLine(ImVec2(o.x - dx * g.r * .55f, o.y - dy * g.r * .55f), ImVec2(o.x - dx * g.r * .95f, o.y - dy * g.r * .95f),
                                fade(GLOW, a * (i ? .5f : .8f)), g.r * .06f);
                }
            }
        }
        else stick(dl, g.c, g.c, g.r, a * .8f, false);
        return;
    }
    int mask = 0;
    ImVec2 thumb = g.c;  // (the menu pad's knob: under its finger, inside the rim)
    for (const Finger &f : s.f)
        if (f.kind == F_BUTTON && !f.done) {
            if (f.ctl == i) mask |= i == C_ATTACK ? (f.part == 2 ? 3 : 1 << f.part)
                                  : i == C_CORNER ? (f.part == 2 ? 3 : f.part == 3 ? 4 : 1 << f.part)
                                  : i == C_MPAD ? f.sent  // (the menu pad's group: what it presses, not what is asked)
                                  : of_mpad(i) ? f.sent != 0
                                  : is_dpad(i) ? f.part : 1;
            if (i == C_MPAD && f.ctl == i) {
                float dx = f.x - g.c.x, dy = f.y - g.c.y, d = hypotf(dx, dy), m = g.r * (1 - KNOB * .5f);
                thumb = d > m ? ImVec2(g.c.x + dx / d * m, g.c.y + dy / d * m) : ImVec2(f.x, f.y);
            }
            if (i == C_HEAVY && f.ctl == C_LIGHT && f.part) mask |= 1;  // (the seam between them: both lit)
        }
    if (of_mpad(i) && s.lit_ctl == i && SDL_GetTicksNS() < s.lit_until) mask |= i == C_MPAD ? s.lit_part : 1;  // (a tap)
    if (i == C_ATTACK) { pill(dl, font, g, mask, a); return; }
    if (i == C_MPAD) { menu_pad(dl, g, thumb, mask, a, thumb.x != g.c.x || thumb.y != g.c.y); return; }
    if (is_tab(i)) { tab(dl, font, g, i == C_ML || i == C_MZL, mpad_label(i), mask != 0, a); return; }
    if (i == C_CORNER) { corner_band(dl, font, g, mask, a); return; }
    if ((i == C_HEAVY || i == C_LIGHT) && !plain_letter(i)) {  // (a great sword, a small one, as on the pill)
        float r = mask ? g.r * .95f : g.r;
        disc(dl, g.c, r, mask != 0, a);
        ImU32 col = fade(mask ? GLOW : INK, a);
        if (i == C_HEAVY) sword_along(dl, ImVec2(g.c.x - PILL_U.x * r * .08f, g.c.y - PILL_U.y * r * .08f), PILL_U, r * 1.55f, r * .36f, r * .72f, col, fade(DEEP, a), true);
        else sword_along(dl, ImVec2(g.c.x - PILL_U.x * r * .05f, g.c.y - PILL_U.y * r * .05f), PILL_U, r * 1.2f, r * .2f, r * .5f, col, fade(DEEP, a), false);
        badge(dl, font, ImVec2(g.c.x + r * .7f, g.c.y + r * .7f), r * .3f, DEFS[i].letter, a);
        ImVec2 at;
        float size;
        if (i == C_LIGHT && separate_seam(l, w, h, &at, &size)) {  // (between the two: both)
            bool both = false;
            for (const Finger &f : s.f) both |= f.kind == F_BUTTON && !f.done && f.ctl == C_LIGHT && f.part;
            float d = size * .55f;
            ImVec2 q[4] = {ImVec2(at.x, at.y - d), ImVec2(at.x + d, at.y), ImVec2(at.x, at.y + d), ImVec2(at.x - d, at.y)};
            dl->AddConvexPolyFilled(q, 4, fade(both ? GLOW : GOLD, a));
        }
        return;
    }
    if (is_dpad(i)) { dpad(dl, g, mask, a); return; }
    if (DEFS[i].icon == I_LETTER || DEFS[i].icon == I_COMBO || plain_letter(i)) {  // (the diamond: the game's letter, large, as on the Switch)
        char label[32];
        if (is_combo(i)) combo_label(i, label, sizeof label);
        else snprintf(label, sizeof label, "%s", letter_of(i));
        float r = mask ? g.r * .95f : g.r, fs = r * 1.05f;
        disc(dl, g.c, r, mask != 0, a);
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, label);
        if (ts.x > r * 1.55f) fs *= r * 1.55f / ts.x, ts = font->CalcTextSizeA(fs, FLT_MAX, 0, label);  // (inside the disc)
        dl->AddText(font, fs, ImVec2(g.c.x - ts.x * .5f + fs * .04f, g.c.y - ts.y * .5f + fs * .05f), fade(IM_COL32(0, 0, 0, 255), a * .7f), label);
        dl->AddText(font, fs, ImVec2(g.c.x - ts.x * .5f, g.c.y - ts.y * .52f), fade(mask ? GLOW : INK, a), label);
        return;
    }
    float r = mask ? g.r * .95f : g.r;
    disc(dl, g.c, r, mask != 0, a);
    if (i == C_RUN && s.run) dl->AddCircle(g.c, g.r * 1.16f, fade(GLOW, a * (.6f + .3f * sinf(SDL_GetTicksNS() / 1e9f * 4))), 40, g.r * .09f);
    if (i == C_L && s.lock) dl->AddCircle(g.c, g.r * 1.16f, fade(GLOW, a * (.6f + .3f * sinf(SDL_GetTicksNS() / 1e9f * 4))), 40, g.r * .09f);
    Icon ic = DEFS[i].icon;
    char letter[8];
    snprintf(letter, sizeof letter, "%s", DEFS[i].letter);
    if (is_ranged(i)) ranged_face(i, s.edit ? 1 : s.drawn, &ic, letter, sizeof letter);
    icon(dl, ic, g.c, i == C_RFIRE ? r * .94f : r, fade(mask ? GLOW : INK, a), fade(DEEP, a));
    float br = std::max(r * (i == C_RFIRE ? .22f : .3f), .021f * h);  // (the large one's as large as the others')
    if (i != C_MMINUS) badge(dl, font, ImVec2(g.c.x + r * .7f, g.c.y + r * .7f), br, letter, a);  // (the small Minus: its glyph)
}

// a plate in the game's window style: wine-dark, a black edge, red and green rules top and bottom
void plate(ImDrawList *dl, Rect r, float line, float a, bool lit) {
    dl->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), fade(lit ? IM_COL32(96, 52, 30, 255) : WINE, a * .94f));
    dl->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), fade(IM_COL32(0, 0, 0, 255), a), 0, 0, line);
    dl->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y0 + line), fade(RED, a));
    dl->AddRectFilled(ImVec2(r.x0, r.y0 + line), ImVec2(r.x1, r.y0 + line * 1.7f), fade(MOSS, a));
    dl->AddRectFilled(ImVec2(r.x0, r.y1 - line * 1.7f), ImVec2(r.x1, r.y1 - line), fade(MOSS, a));
    dl->AddRectFilled(ImVec2(r.x0, r.y1 - line), ImVec2(r.x1, r.y1), fade(RED, a));
}

void text_center(ImDrawList *dl, ImFont *font, float size, ImVec2 c, ImU32 col, const char *t, float w = 0) {
    ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, t);
    if (w > 0) c.x = std::clamp(c.x, ts.x * .5f + size * .3f, w - ts.x * .5f - size * .3f);  // (inside the screen)
    dl->AddText(font, size, ImVec2(c.x - ts.x * .5f + size * .05f, c.y - ts.y * .5f + size * .06f), IM_COL32(0, 0, 0, 200), t);
    dl->AddText(font, size, ImVec2(c.x - ts.x * .5f, c.y - ts.y * .5f), col, t);
}

void dashed_rect(ImDrawList *dl, Rect r, ImU32 col, float t) {
    float seg = t * 5;
    for (float x = r.x0; x < r.x1; x += seg * 2) {
        dl->AddLine(ImVec2(x, r.y0), ImVec2(std::min(x + seg, r.x1), r.y0), col, t);
        dl->AddLine(ImVec2(x, r.y1), ImVec2(std::min(x + seg, r.x1), r.y1), col, t);
    }
    for (float y = r.y0; y < r.y1; y += seg * 2) {
        dl->AddLine(ImVec2(r.x0, y), ImVec2(r.x0, std::min(y + seg, r.y1)), col, t);
        dl->AddLine(ImVec2(r.x1, y), ImVec2(r.x1, std::min(y + seg, r.y1)), col, t);
    }
}

int g_language;  // the settings menu's (touch_draw, each frame): the editor's texts in it
const char *T(const char *english) { return settings_text(g_language, english); }

// A control drawn by the editor: the layout's, or one of the menu pad's tabs or its Minus, which go with it
bool editor_shows(int i) {
    if (i < C_LAID) return in_editor(i);
    return of_mpad(i) && in_editor(C_MPAD) && mpad_member(i);
}
// The control's place, a thumb's slop more, touches a rectangle (a part's frame: the editor's warning)
bool control_on(const Layout &l, int i, const Rect &r, float w, float h) {
    Geo g = place(l, i, w, h);
    if (i == C_ATTACK || i == C_CORNER || is_tab(i)) {  // (thin, tilted, bent or square: points across the frame)
        for (int a = 0; a <= 6; a++)
            for (int b = 0; b <= 6; b++) {
                float x = r.x0 + (r.x1 - r.x0) * a / 6, y = r.y0 + (r.y1 - r.y0) * b / 6;
                float d = i == C_ATTACK ? pill_distance(g, x, y) / (g.r * 1.15f)
                        : i == C_CORNER ? band_distance(g, x, y, nullptr) / (g.r * BAND_T * 1.15f)
                                        : std::max(fabsf(x - g.c.x) / (g.r * TAB_W * 1.22f), fabsf(y - g.c.y) / (g.r * TAB_H * 1.22f));
                if (d < 1) return true;
            }
        return r.in(g.c.x, g.c.y);
    }
    float x = std::clamp(g.c.x, r.x0, r.x1), y = std::clamp(g.c.y, r.y0, r.y1);
    return hypotf(x - g.c.x, y - g.c.y) < g.r * (i == C_MPAD ? MPAD_GRAB : 1.22f);
}

// The HUD's parts in an editor (the touch layout editor's, a desktop's HUD editor): each framed where it is (where it
// would be when it isn't drawn: outside a hunt, hidden), gold, grey while hidden, red where the touch editor finds it
// on a control (red_part), the picked one (pick, HP_*; -1 none) pulsing, the one under a desktop's pointer (over)
// brighter; each named beside its frame on a dark plate, with how it is (hidden, not on the screen) and the picked
// one's size
void draw_part_frames(ImDrawList *dl, ImFont *font, const HudVals *hud, int pick, int over, const Rect fr[HP_COUNT],
                      const bool live[HP_COUNT], const bool red_part[HP_COUNT], const Rect &panel, float w, float h) {
    float pulse_t = SDL_GetTicksNS() / 1e9f;
    for (int k = 0; k < HP_COUNT; k++) {  // (the frames first: the names go over all of them)
        bool sel = k == pick;
        Rect f = fr[k];
        ImU32 col = red_part[k] ? RED : hud[k].hidden ? IM_COL32(150, 146, 140, 220) : !live[k] ? IM_COL32(222, 186, 96, 150) : IM_COL32(222, 186, 96, 210);
        if (k == over && !red_part[k]) col = hud[k].hidden ? IM_COL32(196, 190, 182, 240) : GOLD;
        if (sel) col = red_part[k] ? fade(RED, .65f + .35f * sinf(pulse_t * 6)) : fade(GLOW, .65f + .35f * sinf(pulse_t * 6));
        if (live[k] && !hud[k].hidden) dl->AddRectFilled(ImVec2(f.x0, f.y0), ImVec2(f.x1, f.y1), sel ? IM_COL32(255, 214, 92, 40) : IM_COL32(222, 186, 96, 22));
        dashed_rect(dl, f, col, (sel || k == over ? .005f : live[k] ? .0035f : .0025f) * h);
    }
    // The names: each touching its own frame (below it, above it, inside its top left, beside it, inside its other
    // corners), never on another part's frame, another name or the panel; the small frames first (the clock, the
    // sharpness), which have the fewest places. Where every place is taken, the one that covers least
    Rect used[HP_COUNT + 1];
    int nused = 0;
    used[nused++] = panel;
    int order[HP_COUNT];
    for (int k = 0; k < HP_COUNT; k++) order[k] = k;
    std::sort(order, order + HP_COUNT, [&](int a, int b) {
        return (fr[a].x1 - fr[a].x0) * (fr[a].y1 - fr[a].y0) < (fr[b].x1 - fr[b].x0) * (fr[b].y1 - fr[b].y0);
    });
    for (int n = 0; n < HP_COUNT; n++) {
        int k = order[n];
        bool sel = k == pick;
        Rect f = fr[k];
        // (its name, how it is - hidden, not on the screen - and the picked one's size; where that long a name has no
        // clear place, the name alone, which the frame's look and the line under the panel explain)
        char t[96], name[64];
        snprintf(name, sizeof name, "%s", T(HUD_NAME[k]));
        if (hud[k].hidden) snprintf(t, sizeof t, T("%s (hidden)"), name);
        else if (!live[k]) snprintf(t, sizeof t, T("%s (not on the screen)"), name);
        else snprintf(t, sizeof t, "%s", name);
        if (sel) {
            snprintf(t + strlen(t), sizeof t - strlen(t), "   %d%%", (int)lroundf(hud[k].k * 100));
            snprintf(name + strlen(name), sizeof name - strlen(name), "   %d%%", (int)lroundf(hud[k].k * 100));
        }
        float fs = .022f * h, th = fs * 1.24f, m = fs * .1f;
        auto cover = [](const Rect &a, const Rect &b) {  // (how much of two rectangles lies on both)
            float x = std::min(a.x1, b.x1) - std::max(a.x0, b.x0), y = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
            return x > 1.0f && y > 1.0f ? x * y : 0.0f;
        };
        // The places, as the plate's middle: below and above, slid along the frame as far as it still touches it, its
        // middle first; inside the top left; to the right and left, at the frame's top, middle and bottom; the other
        // inside corners. The first that covers nothing, else the one that covers least (another name, another part's
        // frame, much more the panel); off the screen never
        auto find = [&](const char *text, ImVec2 *at, float *plate_w) {
            float pw = font->CalcTextSizeA(fs, FLT_MAX, 0, text).x + fs * .7f;
            ImVec2 cand[40];
            int nc = 0;
            float lo = f.x0 - pw * .5f + std::min(pw, f.x1 - f.x0) * .3f, hi = f.x1 + pw * .5f - std::min(pw, f.x1 - f.x0) * .3f;
            static const float SLIDE[9] = {.5f, .375f, .625f, .25f, .75f, .125f, .875f, 0.0f, 1.0f};
            for (float y : {f.y1 + th * .5f, f.y0 - th * .5f})
                for (float t01 : SLIDE) cand[nc++] = ImVec2(lo + (hi - lo) * t01, y);
            cand[nc++] = ImVec2(f.x0 + m + pw * .5f, f.y0 + m + th * .5f);
            for (float x : {f.x1 + pw * .5f, f.x0 - pw * .5f})
                for (float y : {f.y0 + th * .5f, (f.y0 + f.y1) * .5f, f.y1 - th * .5f}) cand[nc++] = ImVec2(x, y);
            cand[nc++] = ImVec2(f.x0 + m + pw * .5f, f.y1 - m - th * .5f);
            cand[nc++] = ImVec2(f.x1 - m - pw * .5f, f.y0 + m + th * .5f);
            cand[nc++] = ImVec2(f.x1 - m - pw * .5f, f.y1 - m - th * .5f);
            float best = 1e30f;
            *at = cand[0], *plate_w = pw;
            for (int j = 0; j < nc && best > 0.0f; j++) {
                Rect r = {cand[j].x - pw * .5f, cand[j].y - th * .5f, cand[j].x + pw * .5f, cand[j].y + th * .5f, true};
                if (r.x0 < 0 || r.x1 > w || r.y0 < 0 || r.y1 > h) continue;
                float cost = cover(r, panel) * 1000.0f;
                for (int u = 1; u < nused; u++) cost += cover(r, used[u]) * 10.0f;
                for (int o = 0; o < HP_COUNT; o++)
                    if (o != k) cost += cover(r, fr[o]);
                if (cost < best) best = cost, *at = cand[j];
            }
            if (best >= 1e30f)
                at->x = std::clamp(at->x, pw * .5f, std::max(pw * .5f, w - pw * .5f)), at->y = std::clamp(at->y, th * .5f, h - th * .5f);
            return best;
        };
        ImVec2 c, c2;
        float pw, pw2, cost = find(t, &c, &pw);
        if (cost > 0.0f && strcmp(t, name) && find(name, &c2, &pw2) < cost) c = c2, pw = pw2, snprintf(t, sizeof t, "%s", name);
        Rect plate_r = {c.x - pw * .5f, c.y - th * .5f, c.x + pw * .5f, c.y + th * .5f, true};
        dl->AddRectFilled(ImVec2(plate_r.x0, plate_r.y0), ImVec2(plate_r.x1, plate_r.y1), IM_COL32(28, 20, 15, 190), fs * .3f);
        text_center(dl, font, fs, c, sel ? GLOW : hud[k].hidden || !live[k] ? fade(INK, .75f) : INK, t, w);
        if (nused < HP_COUNT + 1) used[nused++] = plate_r;
    }
}

// The editor: the game's HUD parts framed where they are (where they would be when they aren't drawn: outside a hunt,
// hidden), named below them on a dark plate, the picked one pulsing with its size; the touch controls over them, with
// their names (with the touch controls' HUD); a part and a control over each other both red; the panel, and a line under
// it for what the player should know (the item bar can't be hidden with the touch controls, a hidden part a tap no
// longer works, the frames outside a hunt)
void draw_editor(ImDrawList *dl, ImFont *font, const State &s, float w, float h) {
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, 96));
    bool touch = s.edit_prof == HUD_TOUCH;
    const HudVals *hud = s.layout.hud[s.edit_prof];
    Rect panel, btn[E_COUNT];
    editor_panel(w, h, &panel, btn);
    Rect fr[HP_COUNT];
    bool live[HP_COUNT], any_live = false, red_part[HP_COUNT] = {}, red_ctl[C_COUNT] = {};
    for (int k = 0; k < HP_COUNT; k++) fr[k] = part_frame(k, s.edit_prof, w, h, &live[k]), any_live |= live[k];
    // (a part shown on a control's place, where it isn't with the factory's places of both: a fresh layout warns of
    // nothing, the player's own move or size does)
    Layout factory = defaults();
    const float FACTORY[3] = {0.0f, 0.0f, 1.0f};
    for (int k = 0; k < HP_COUNT && touch; k++) {
        if (hud[k].hidden) continue;
        bool unused;
        Rect f0 = part_frame(k, s.edit_prof, w, h, &unused, FACTORY);
        for (int i = 0; i < C_COUNT; i++)
            if (editor_shows(i) && control_on(s.layout, i, fr[k], w, h) && !control_on(factory, i, f0, w, h))
                red_part[k] = red_ctl[i == C_MPAD || of_mpad(i) ? C_MPAD : i] = true;
    }
    draw_part_frames(dl, font, hud, s.sel >= ZONE_SEL ? s.sel - ZONE_SEL : -1, -1, fr, live, red_part, panel, w, h);
    for (int i = 0; i < C_LAID && touch; i++) {
        if (!in_editor(i)) continue;  // (the controls no longer there stay in the file's order only)
        control(dl, font, s.layout, i, w, h, s, 1.0f);
        if (i == C_MPAD)  // (its tabs and Minus with it, where it puts them)
            for (int j = C_ML; j <= C_MMINUS; j++)
                if (mpad_member(j)) control(dl, font, s.layout, j, w, h, s, 1.0f);
        Geo g = place(s.layout, i, w, h);
        float rr = i == C_ATTACK ? g.r * PILL_HALF * .75f + g.r : i == C_MPAD ? MPAD_BELOW * s.layout.scale[i] * h : g.r;
        if (red_ctl[i]) {  // (a part of the HUD on it: red round it)
            if (i == C_ATTACK) {
                capsule_path(dl, g.c, g.r * PILL_HALF, g.r * 1.3f);
                dl->PathStroke(fade(RED, .9f), ImDrawFlags_Closed, .005f * h);
            } else if (i == C_CORNER) {
                band_path(dl, g, 0, g.r * BAND_LEN, g.r * BAND_T * 1.3f, true, true);
                dl->PathStroke(fade(RED, .9f), ImDrawFlags_Closed, .005f * h);
            } else {
                dl->AddCircle(g.c, g.r * (i == C_MPAD ? MPAD_GRAB : 1.32f), fade(RED, .9f), 48, .005f * h);
            }
        }
        if (i == s.sel) {
            float t = SDL_GetTicksNS() / 1e9f, pulse_a = .65f + .35f * sinf(t * 6);
            if (i == C_ATTACK) {
                capsule_path(dl, g.c, g.r * PILL_HALF, g.r * 1.22f);
                dl->PathStroke(fade(GLOW, pulse_a), ImDrawFlags_Closed, .006f * h);
            } else if (i == C_CORNER) {
                band_path(dl, g, 0, g.r * BAND_LEN, g.r * BAND_T * 1.2f, true, true);
                dl->PathStroke(fade(GLOW, pulse_a), ImDrawFlags_Closed, .006f * h);
            } else {
                dl->AddCircle(g.c, g.r * 1.25f, fade(GLOW, pulse_a), 48, .006f * h);
            }
        }
        if (i == C_CORNER) {  // (its name above its upright part, its size beside it)
            ImVec2 top = band_point(g, g.r * BAND_LEN, 0);
            float ny = top.y - g.r * BAND_T - .03f * h;
            text_center(dl, font, .024f * h, ImVec2(top.x - g.r * .22f, ny), i == s.sel ? GLOW : INK, T(name_of(i)), w);
            if (i == s.sel) {
                char pct[16];
                snprintf(pct, sizeof pct, "%d%%", (int)lroundf(s.layout.scale[i] * 100));
                text_center(dl, font, .03f * h, ImVec2(top.x - g.r * .22f, ny - .035f * h), GLOW, pct, w);
            }
            continue;
        }
        // its name below it, or above it near the bottom edge, or beside it in a column at an edge (the control below
        // would be under it); the selected one's size on the other side
        char own[32];
        if (is_combo(i)) combo_label(i, own, sizeof own);
        const char *t = is_combo(i) ? own : T(name_of(i));
        float fs = .024f * h;
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, t);
        float side = from_right(i) ? -1.0f : 1.0f, tx = g.c.x + side * (g.r * 1.3f + ts.x * .5f);
        bool beside = s.layout.x[i] < .12f && i != C_ATTACK && i != C_STICK && i != C_MPAD;
        for (int j = 0; j < C_LAID && beside; j++) {  // (not over another control: then below or above it)
            if (j == i || !in_editor(j)) continue;
            Geo o = place(s.layout, j, w, h);
            if (j == C_CORNER) continue;
            if (j == C_ATTACK) {  // (a thin tilted capsule: the label's corners, edges and middle against its axis)
                for (int k = 0; k < 9; k++)
                    if (pill_distance(o, tx + (k % 3 - 1) * ts.x * .5f, g.c.y + (k / 3 - 1) * fs * .6f) < o.r) beside = false;
                continue;
            }
            if (fabsf(o.c.x - tx) < ts.x * .5f + o.r && fabsf(o.c.y - g.c.y) < fs * .6f + o.r) beside = false;
        }
        if (beside) {
            text_center(dl, font, fs, ImVec2(tx, g.c.y), i == s.sel ? GLOW : INK, t, w);
            if (i == s.sel) {
                char pct[16];
                snprintf(pct, sizeof pct, "%d%%", (int)lroundf(s.layout.scale[i] * 100));
                text_center(dl, font, .03f * h, ImVec2(tx, g.c.y + .035f * h), GLOW, pct, w);
            }
            continue;
        }
        bool above = g.c.y + rr + .05f * h > h;
        float ny = above ? g.c.y - rr - .03f * h : g.c.y + rr + .03f * h, py = above ? g.c.y + rr + .035f * h : g.c.y - rr - .035f * h;
        if (py > h - .02f * h || py < .02f * h) py = g.c.y;
        text_center(dl, font, .024f * h, ImVec2(g.c.x, ny), i == s.sel ? GLOW : INK, t, w);
        if (i == s.sel) {
            char pct[16];
            snprintf(pct, sizeof pct, "%d%%", (int)lroundf(s.layout.scale[i] * 100));
            text_center(dl, font, .03f * h, ImVec2(g.c.x, py), GLOW, pct, w);
        }
    }
    int part = s.sel >= ZONE_SEL ? s.sel - ZONE_SEL : -1;
    float line = .004f * h;
    plate(dl, panel, line, 1.0f, false);
    text_center(dl, font, .034f * h, ImVec2((panel.x0 + panel.x1) * .5f, panel.y0 + .045f * h), GLOW, T("Controls and HUD"));
    const char *say = T(s.sel == C_MPAD     ? "Shows while a menu of the game's is open - drag to move, pinch or use - / + for its size"
                        : part >= 0 && hud[part].hidden ? "Hidden: Show brings it back"
                        : s.sel >= 0        ? "Drag to move  -  pinch or use - / + for its size"
                                            : "Tap a control or a part of the game's screen to pick it, drag it to move it");
    float ss = .022f * h, room = (panel.x1 - panel.x0) * .94f, wide = font->CalcTextSizeA(ss, FLT_MAX, 0, say).x;
    if (wide > room) ss *= room / wide;  // (a narrow screen, a long sentence: on one line, smaller)
    text_center(dl, font, ss, ImVec2((panel.x0 + panel.x1) * .5f, panel.y0 + .078f * h), INK, say);
    static const char *const STYLE_LABEL[STYLE_COUNT] = {"Attacks: pill", "Attacks: buttons", "Attacks: corner"};
    const char *const LABEL[E_COUNT] = {"-", "+", part >= 0 && hud[part].hidden ? "Show" : "Hide", "Reset",
                                        trad() ? "Layout: traditional" : ranged_layout() ? "Layout: ranged" : "Layout: melee",
                                        trad() ? "A B X Y" : STYLE_LABEL[s.layout.style],
                                        touch ? "HUD: touch" : "HUD: controller", "Defaults", "Cancel", "Save"};
    for (int i = 0; i < E_COUNT; i++) {
        if (!btn[i].ok) continue;
        bool off = ((i == E_SMALLER || i == E_BIGGER || i == E_RESET) && s.sel < 0) ||
                   (i == E_HIDE && (part < 0 || !hud_hide_ok(s.edit_prof, part))) || ((i == E_SET || i == E_STYLE) && !touch) ||
                   (i == E_STYLE && ranged_layout()) || ((i == E_STYLE || i == E_SET) && trad());
        bool lit = false;
        for (const Finger &f : s.f)
            if (f.kind == F_MENU && btn[i].in(f.x, f.y)) lit = true;
        plate(dl, btn[i], line * .8f, off ? .45f : 1.0f, lit && !off);
        float fs = (i < E_HIDE ? .05f : .028f) * h, room = (btn[i].x1 - btn[i].x0) * .92f, wide = font->CalcTextSizeA(fs, FLT_MAX, 0, T(LABEL[i])).x;
        if (wide > room) fs *= room / wide;  // (a narrow screen, a long word: on one line, smaller)
        text_center(dl, font, fs, ImVec2((btn[i].x0 + btn[i].x1) * .5f, (btn[i].y0 + btn[i].y1) * .5f),
                    fade(i == E_SAVE ? GLOW : INK, off ? .45f : 1.0f), T(LABEL[i]));
    }
    // (what the player should know: about the part picked first, else a hidden part a tap no longer works, else why the
    // frames show outside a hunt)
    const char *note = nullptr;
    int hidden_tap = touch && hud[HP_ARTS].hidden ? HP_ARTS : touch && hud[HP_TARGET].hidden ? HP_TARGET : -1;
    if (touch && part == HP_ITEMS) note = "The item bar picks and uses items with the touch controls: it can't be hidden here";
    else if (touch && (part == HP_ARTS || part == HP_TARGET) && hud[part].hidden) hidden_tap = part;
    if (!note && hidden_tap == HP_ARTS) note = "Hidden: no tap performs the Hunter Arts - the D-pad button or a controller does";
    else if (!note && hidden_tap == HP_TARGET)
        note = "Hidden: no tap turns the lock-on on or off - the Right stick click button or a controller does";
    if (!note && !any_live && s.sel < 0) note = "Outside a hunt the frames show where each part goes";
    if (note) {
        const char *t = T(note);
        float fs = .022f * h, tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x, maxw = (panel.x1 - panel.x0) * .96f;
        if (tw > maxw) fs *= maxw / tw, tw = maxw;
        ImVec2 c((panel.x0 + panel.x1) * .5f, panel.y1 + fs * 1.1f);
        dl->AddRectFilled(ImVec2(c.x - tw * .5f - fs * .5f, c.y - fs * .75f), ImVec2(c.x + tw * .5f + fs * .5f, c.y + fs * .75f),
                          IM_COL32(28, 20, 15, 215), fs * .3f);
        text_center(dl, font, fs, c, GOLD, t);
    }
}

#ifndef __ANDROID__
// The game's B as the controller in use marks it ([Controls] ButtonIcons; FaceButtons: overlay_face_position), on the
// touch controls' badge: on the HUD editor's Cancel, which it presses too
void pad_b_badge(ImDrawList *dl, ImFont *font, ImVec2 c, float r, int icons) {
    int at = overlay_face_position(1) & 3;  // (0 the bottom button, 1 the right one, 2 the left, 3 the top)
    if (icons != 2) {
        static const char *const XBOX[4] = {"A", "B", "X", "Y"}, *const SWITCH[4] = {"B", "A", "Y", "X"};
        badge(dl, font, c, r, (icons == 1 ? SWITCH : XBOX)[at], 1.0f);
        return;
    }
    dl->AddCircleFilled(c, r, IM_COL32(18, 12, 9, 255), 20);  // (PlayStation's: its shapes)
    dl->AddCircle(c, r, GOLD, 20, r * .14f);
    float q = r * .45f, t = r * .16f;
    if (at == 0) {
        dl->AddLine(ImVec2(c.x - q, c.y - q), ImVec2(c.x + q, c.y + q), INK, t);
        dl->AddLine(ImVec2(c.x - q, c.y + q), ImVec2(c.x + q, c.y - q), INK, t);
    } else if (at == 1) {
        dl->AddCircle(c, q, INK, 20, t);
    } else if (at == 2) {
        dl->AddRect(ImVec2(c.x - q, c.y - q), ImVec2(c.x + q, c.y + q), INK, 0, 0, t);
    } else {
        dl->AddTriangle(ImVec2(c.x, c.y - q), ImVec2(c.x + q, c.y + q * .75f), ImVec2(c.x - q, c.y + q * .75f), INK, t);
    }
}
// A key of the keyboard in the badge's look, as tall as its disc (r: half its height) and as wide as its name, its middle
// at c; and its width
float key_badge_width(ImFont *font, float r, const char *name) {
    return std::max(r * 2.0f, font->CalcTextSizeA(r * 1.15f, FLT_MAX, 0, name).x + r * 1.1f);
}
void key_badge(ImDrawList *dl, ImFont *font, ImVec2 c, float r, const char *name) {
    float w = key_badge_width(font, r, name), k = r * .45f;
    ImVec2 a(c.x - w * .5f, c.y - r), b(c.x + w * .5f, c.y + r);
    dl->AddRectFilled(a, b, IM_COL32(18, 12, 9, 255), k);
    dl->AddRect(a, b, GOLD, k, 0, r * .14f);
    ImVec2 ts = font->CalcTextSizeA(r * 1.15f, FLT_MAX, 0, name);
    dl->AddText(font, r * 1.15f, ImVec2(c.x - ts.x * .5f, c.y - ts.y * .52f), INK, name);
}

// The HUD editor: every part framed and named as in the touch layout editor (draw_part_frames: where it is drawn, or
// where it would be), the one under the pointer brighter, the picked one pulsing with its size; the panel (icons:
// [Controls] ButtonIcons), and a line under it for what the player should know (a hidden part, the frames outside a
// hunt)
void draw_hud_editor(ImDrawList *dl, ImFont *font, const State &s, float w, float h, int icons) {
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, 96));
    const HudVals *hud = s.layout.hud[HUD_PLAIN];
    Rect panel, btn[H_COUNT];
    hud_panel(w, h, &panel, btn);
    Rect fr[HP_COUNT];
    bool live[HP_COUNT], any_live = false, red[HP_COUNT] = {};
    for (int k = 0; k < HP_COUNT; k++) fr[k] = part_frame(k, HUD_PLAIN, w, h, &live[k]), any_live |= live[k];
    int part = s.sel >= ZONE_SEL ? s.sel - ZONE_SEL : -1;
    int over = s.hud_drag < 0 && s.hud_btn < 0 ? hud_over(s.hud_px, s.hud_py, w, h) : -1;
    draw_part_frames(dl, font, hud, part, over, fr, live, red, panel, w, h);
    float line = .004f * h, mid = (panel.x0 + panel.x1) * .5f;
    plate(dl, panel, line, 1.0f, false);
    text_center(dl, font, .034f * h, ImVec2(mid, panel.y0 + .045f * h), GLOW, T("Move the HUD"));
    const char *say = T(part >= 0 && hud[part].hidden ? "Hidden: Show brings it back"
                        : part >= 0                   ? "Drag to move  -  the wheel or - / + for its size"
                                                      : "Click a part to pick it, drag it to move it");
    float ss = .022f * h, room = (panel.x1 - panel.x0) * .94f, wide = font->CalcTextSizeA(ss, FLT_MAX, 0, say).x;
    if (wide > room) ss *= room / wide;  // (a narrow window, a long sentence: on one line, smaller)
    text_center(dl, font, ss, ImVec2(mid, panel.y0 + .078f * h), INK, say);
    const char *const LABEL[H_COUNT] = {"-", "+", part >= 0 && hud[part].hidden ? "Show" : "Hide", "Reset", "Defaults", "Cancel", "Save"};
    // (on Cancel what cancels it too: a controller's B when a controller is read and was the last thing used
    // (host_input.c), else Esc)
    bool pad_b = g_hud_pad && overlay_last_input() == USED_PAD;
    for (int i = 0; i < H_COUNT; i++) {
        if (!btn[i].ok) continue;
        bool off = i <= H_RESET && part < 0;  // (what is picked: nothing yet)
        bool lit = !off && (s.hud_btn == i || (s.hud_btn < 0 && s.hud_drag < 0 && btn[i].in(s.hud_px, s.hud_py)));
        plate(dl, btn[i], line * .8f, off ? .45f : 1.0f, lit);
        float br = i == H_CANCEL ? std::min(.022f * h, (btn[i].y1 - btn[i].y0) * .3f) : 0.0f;
        float bw = br > 0 ? pad_b ? br * 2 : key_badge_width(font, br, "Esc") : 0.0f;  // (the badge's width)
        float bs = (i < 2 ? .05f : .028f) * h, broom = (btn[i].x1 - btn[i].x0) * .92f - bw * 1.1f,
              bwide = font->CalcTextSizeA(bs, FLT_MAX, 0, T(LABEL[i])).x;
        if (bwide > broom) bs *= broom / bwide;
        text_center(dl, font, bs, ImVec2((btn[i].x0 + btn[i].x1) * .5f - bw * .55f, (btn[i].y0 + btn[i].y1) * .5f),
                    fade(i == H_SAVE ? GLOW : INK, off ? .45f : 1.0f), T(LABEL[i]));
        ImVec2 at(btn[i].x1 - br * .6f - bw * .5f, (btn[i].y0 + btn[i].y1) * .5f);
        if (br > 0 && pad_b) pad_b_badge(dl, font, at, br, icons);
        else if (br > 0) key_badge(dl, font, at, br, "Esc");
    }
    // (what the player should know: no part on the screen - outside a hunt, a menu over it -, where the frames are)
    if (!any_live && part < 0) {
        const char *t = T("Outside a hunt the frames show where each part goes");
        float fs = .022f * h, tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x, maxw = (panel.x1 - panel.x0) * .96f;
        if (tw > maxw) fs *= maxw / tw, tw = maxw;
        ImVec2 c(mid, panel.y1 + fs * 1.1f);
        dl->AddRectFilled(ImVec2(c.x - tw * .5f - fs * .5f, c.y - fs * .75f), ImVec2(c.x + tw * .5f + fs * .5f, c.y + fs * .75f),
                          IM_COL32(28, 20, 15, 215), fs * .3f);
        text_center(dl, font, fs, c, GOLD, t);
    }
}
#endif

}  // namespace

extern "C" void touch_draw(float w, float h) {
#ifndef __ANDROID__
    if ((!g_visible && !g_gear_only && !g_lock_mark && !g_hud_edit) || overlay_menu_open()) return;
#else
    if ((!g_visible && !g_gear_only && !g_lock_mark) || overlay_menu_open()) return;
#endif
    State s;
    {
        std::lock_guard<std::mutex> l(g_lock);
        s = g_s;
    }
    if (!s.loaded) return;
    if (s.w > 0 && (s.w != w || s.h != h)) {  // (the fingers' pixels are the window's; the picture's may differ)
        float kx = w / s.w, ky = h / s.h;
        s.bx *= kx, s.kx *= kx, s.by *= ky, s.ky *= ky;
        for (Finger &f : s.f) f.x *= kx, f.y *= ky;
#ifndef __ANDROID__
        s.hud_px *= kx, s.hud_py *= ky;
#endif
    }
    ImFont *font = ImGui::GetFont();
#ifndef __ANDROID__
    if (s.hud_edit) {
        Config c = config_editable();
        g_language = c.language;
        draw_hud_editor(ImGui::GetForegroundDrawList(), font, s, w, h, c.button_icons);
        return;
    }
#endif
    if (s.edit) {
        g_language = config_editable().language;
        draw_editor(ImGui::GetForegroundDrawList(), font, s, w, h);
        return;
    }
    uint64_t now = SDL_GetTicksNS();
    float idle = s.last_ns ? (now - s.last_ns) / 1e9f : 99.0f;
    float a = config_live_touch_opacity() * (idle > 5.0f ? .65f : 1.0f);  // (dimmer after a while untouched)
    ImDrawList *dl = ImGui::GetBackgroundDrawList();  // (under the port's menus and notices)
    Zones z = zones();
    Rect target = zone_rect(z, HP_TARGET, w, h);
    if (!g_visible) {  // (hidden: the settings button alone, a little fainter; while the lock-on is on its padlock, opaque
                       // as the HUD it goes by)
        if (g_gear_only) control(dl, font, s.layout, C_SETTINGS, w, h, s, a * .75f);
        if (g_lock_mark && target.ok) lock_mark(dl, target, true, 1.0f, h, now);
        return;
    }
    // (the menu pad's group, over the rest: in as the menu opens, out as its windows go)
    float pad_a = s.menu ? std::min(1.0f, (float)(now - s.menu_on_ns) / (float)MPAD_IN) * mpad_fade(now) : 0.0f;
    {
        static int dbg = -1;  // (MHGU_TOUCH_LOG: when the menu pad began to go and when it was gone, after the last window)
        static float was;
        if (dbg < 0) dbg = getenv("MHGU_TOUCH_LOG") != nullptr;
        float out = s.menu ? mpad_fade(now) : 0.0f;
        uint64_t m = g_menu_ns.load();
        if (dbg && m && was >= 1.0f && out < 1.0f) rt_logf("[touch] menu pad going, %.0f ms after the last menu window\n", (now - m) / 1e6);
        if (dbg && m && was > 0.0f && out <= 0.0f) rt_logf("[touch] menu pad gone, %.0f ms after the last menu window\n", (now - m) / 1e6);
        was = out;
    }
    for (int i = 0; i < C_COUNT; i++)
        if (in_mode(i, s.menu)) control(dl, font, s.layout, i, w, h, s, of_mpad(i) ? a * pad_a : a);
    hub_strip_draw(dl, font, w, h, 1.0f);  // (after every control, so over them, and solid: a button never shows through it)
    // the HUD parts that answer a finger: the item bar's turn, the art performed, the lock-on (the item bar's arrows as
    // in a hunt while a menu is open too: nothing changes as one opens, though they turn nothing then)
    Rect item = zone_rect(z, HP_ITEMS, w, h), arts = zone_rect(z, HP_ARTS, w, h);
    if (item.ok && config_live_touch_show(TB_ITEM_ARROWS)) {
        for (int d = -1; d <= 1; d += 2) {
            bool lit = now < s.item_flash && s.item_dir == d;
            Geo g = item_arrow(item, d, h);
            disc(dl, g.c, g.r, lit, a);
            float ch = g.r * .42f;
            dl->AddTriangleFilled(ImVec2(g.c.x + d * ch, g.c.y), ImVec2(g.c.x - d * ch * .6f, g.c.y - ch), ImVec2(g.c.x - d * ch * .6f, g.c.y + ch),
                                  fade(lit ? GLOW : INK, a));
        }
    } else if (item.ok) {
        float cy = (item.y0 + item.y1) * .5f, ch = .03f * h;
        for (int d = -1; d <= 1; d += 2) {
            bool lit = now < s.item_flash && s.item_dir == d;
            float x = d < 0 ? item.x0 - .012f * h : item.x1 + .012f * h;
            dl->AddTriangleFilled(ImVec2(x + d * ch * .55f, cy), ImVec2(x - d * ch * .45f, cy - ch), ImVec2(x - d * ch * .45f, cy + ch),
                                  fade(lit ? GLOW : GOLD, a * (lit ? 1.0f : .55f)));
        }
    }
    if (arts.ok && now < s.art_flash) {
        float rows[3][2];
        int nr = arts_rows(z, w, h, rows);
        if (s.art_row < nr)
            dl->AddRect(ImVec2(arts.x0, rows[s.art_row][0]), ImVec2(arts.x1, rows[s.art_row][1]), fade(GLOW, a), .01f * h, 0, .005f * h);
    }
    if (target.ok && config_live_lock_on()) lock_mark(dl, target, s.lock, a, h, now);  // (off: the padlock open, to tap)
}
