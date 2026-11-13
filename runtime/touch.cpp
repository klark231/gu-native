// Touch controls for phones, in the Monster Hunter style: bronze-rimmed dark discs with cream glyphs and the game's
// button letter on a small badge.
//   - left thumb: a stick fixed at its place (the knob follows the thumb inside the rim); a light tilt walks, a full
//     one runs ([Touch] StickRun: the thumb well past the rim holds R, the dash, with the weapon sheathed); lifted inside
//     the small circle with a padlock that shows above the stick (lit while the thumb is on it), the run locks (Call of
//     Duty's sprint lock) and goes on forward until the stick or the circle is touched again, the weapon comes out or a
//     menu opens. With StickRun Off a Run button above the attacks runs while held
//   - right thumb: the attack pill (the heavy attack A on its upper half, the light one X on the lower, both on its
//     seam), R guard / dash beside its lower end, B dodge below it, ZR, the target (the game's change target, by its
//     Control Type: hle/mousecam.c), L centre the camera; Plus (the game's menu), the map (the left stick's click: the
//     minimap) and the port's settings at the left edge
//   - the lock-on, as in the newer games and the MH4U port: the camera keeps turning to the monster the game's Target
//     Cam has chosen (the target button moves it to the other), else the one it followed, else the nearest
//     (hle/mousecam.c). The padlock by L turns it on and off with a tap, and so does a long press on L; a controller's
//     right stick click too (runtime/host_input.c; held, in the Control Type where that click changes the target). L is
//     ringed while it is on, the ring pulsing while the camera follows. A turn by hand or by the game (L) pauses it
//   - while a menu of the game's is open (the pause menu, the item pouch, a shop, a yes / no: its windows are drawn
//     on layer 97, nvn/vulkan/exec.c touch_hud_menu), nothing of the layout changes: a small pad shows besides, the
//     menu pad, which presses the d-pad (the game's menus move their cursor with it, a step and then its own repeat),
//     with the page tabs above it (L and R, or ZL and ZR by the game's option) and a small Minus below it. The stick
//     keeps walking where the game lets the hunter walk (it moves the cursor where the hunter isn't played: the title,
//     the main menu); A, B and the rest stay what they are. A drag anywhere else moves the cursor, a step every so far
//   - the game's own HUD, moved out of the thumbs' way (nvn/vulkan/exec.c): a tap on the item bar (bottom middle)
//     uses the item, a swipe on it turns the bar, a long press holds it open with its neighbouring items (as L held,
//     with no L: hle/itembar.c); a tap on a Hunter Art's panel (top right) performs it. The target camera's icons take
//     no taps (a drag there turns the camera); with the controls hidden (a controller, a keyboard) a lit padlock by them
//     shows while the lock-on is on
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

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "imgui.h"

#include "config.h"
extern "C" {
uint64_t plat_now_ns(void);
extern volatile uint64_t g_town_ns;  // (hle/quickhunt.c: a town's walking-around update)
extern volatile uint64_t g_present_count;  // (frames presented: the log's lines of the menu pad)
int weapon_drawn(void);  // (hle/weapon.c: 1 the weapon out, 0 sheathed, -1 not known)
#include "hle/input.h"
#include "hle/itembar.h"
#include "hle/mousecam.h"
#include "hle/mousemenu.h"  // (menu_pages_zl: the pair the game turns its pages with; menu_cursor_age: a menu takes input)
SDL_Window *ui_window(void);
}
#include "haptics.h"  // (a short tick on the phone when a control is pressed)
#include "logwriter.h"  // (rt_logf: Windows queues the line, the log's own thread writes it)
#include "overlay.h"
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
    C_ITEML, C_ITEMR,                                                                      // (the item bar's arrows)
    C_RUNLOCK,                                                                             // (the sprint lock's circle)
    C_STICK,                                                                               // (laid out: touch_layout.txt)
    C_ML, C_MR,          // (the menu pad's: its page tabs above it, as a controller's shoulders, placed from it;
    C_MZL, C_MZR,        // the other pair above them, when [Touch] ShowMenuZL / ZR say so; Minus below it, with the
    C_MMINUS,            // hunt's own Minus hidden)
    C_COUNT
};
const int C_LAID = C_STICK + 1;
enum Icon { I_ATTACK, I_DODGE, I_SHIELD, I_BURST, I_TARGET_SELECT, I_CAMERA, I_MENU, I_MAP, I_GEAR, I_SHEATHE, I_RUN, I_STICK, I_DPAD,
            I_LETTER, I_MINUS, I_COMBO, I_SHOT, I_BOW, I_RELOAD, I_SCOPE, I_CROUCH, I_COATING, I_MELEE, I_GATHER, I_UP, I_DOWN,
            I_LEFT, I_RIGHT };
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
    {"dodge", "Evade  B", I_DODGE, "B", B(BTN_B), true, .10f, .12f, .056f},
    {"guard", "Guard / dash  R", I_SHIELD, "R", B(BTN_R), true, .505f, .095f, .058f},
    {"zr", "Special  ZR", I_BURST, "ZR", B(BTN_ZR), true, .075f, .68f, .045f},
    {"target", "Target  ZL", I_TARGET_SELECT, "ZL", B(BTN_ZL), true, .19f, .70f, .045f},
    {"camera", "Camera / lock-on  L", I_CAMERA, "L", B(BTN_L), true, .31f, .64f, .045f},
    {"menu", "Menu  +", I_MENU, "+", B(BTN_PLUS), false, .065f, .55f, .046f},
    {"map", "Left stick click", I_MAP, "L3", B(BTN_LSTICK), false, .065f, .43f, .046f},  // (the minimap on / off; the whole map:
                                                                                  // the game's menu, Y)
    {"settings", "Settings", I_GEAR, "", 0, false, .065f, .67f, .040f},
    // sheathe: Y with the weapon out (sheathed, Y uses the item: the item bar's tap); left of the pill's upper half
    {"sheathe", "Sheathe  Y", I_SHEATHE, "Y", B(BTN_Y), true, .44f, .44f, .046f},
    // run: R while held, with the weapon sheathed ([Touch] StickRun Off: the stick only walks then), above the attacks at
    // the edge (clear of the menu pad, which the editor shows with it; R3's place, off by default). Its key changed with
    // its side (a left one, never shown, was in every layout saved before: those lines are dropped)
    {"run_button", "Run  R", I_RUN, "R", 0, true, .075f, .56f, .05f},
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
    {"rdodge", "Evade  B", I_DODGE, "B", B(BTN_B), true, .085f, .10f, .056f},
    {"rsheathe", "Sheathe  Y", I_SHEATHE, "Y", B(BTN_Y), true, .175f, .47f, .046f},
    {"ammo_up", "Next ammo", I_UP, "", 0, true, .56f, .30f, .05f},
    {"ammo_down", "Previous ammo", I_DOWN, "", 0, true, .56f, .13f, .05f},
    // the menu pad, under the right thumb: above the attack pill's upper end (A) and right of Y, its tabs above it and
    // Minus at its lower left clear of both, from the screen's right edge like them (on a phone wider than 16:9 with the
    // HUD in the middle, mostly in the strip right of the game's windows). Its key changed with that place (it was
    // placed from the HUD's right edge, in the middle of the screen): a layout saved before keeps it at the default
    {"menupad", "Menu pad", I_DPAD, "", 0, true, .21f, .55f, .08f},
    // the item bar's arrows ([Touch] ShowItemArrows), one each side of the bar: these places only mark them as not placed
    // by the player (they follow the bar then, or show by its usual place while none is known: arrow_follow), as the MH4U
    // port's; the editor moves them as the other controls, and their places are kept in touch_layout.txt
    {"item_left", "Item bar: previous", I_LEFT, "", 0, true, .64f, .085f, .052f},
    {"item_right", "Item bar: next", I_RIGHT, "", 0, true, .565f, .085f, .052f},
    // the sprint lock's circle ([Touch] StickRun): this place only marks it as not placed by the player (it is then
    // LOCK_AT above the stick, wherever the stick is: lock_spot); placed in the editor it stays where it was put. Its
    // size: LOCK_R of the stick's radius, times its own
    {"run_lock", "Sprint lock", I_RUN, "", 0, false, .29f, .546f, .0483f},
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
    case C_ITEML: case C_ITEMR: return TB_ITEM_ARROWS;
    case C_RUNLOCK: return TB_STICK_RUN;
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
    // (the run button: with StickRun Off, in a hunt and in towns, either layout, never in a menu - R turns its pages;
    // what a hunt can do without is off until asked for: [Touch] Show*)
    if (i == C_RUN) return !menu && !config_live_touch_show(TB_STICK_RUN);
    if (i == C_ITEML || i == C_ITEMR) return false;  // (drawn and pressed with the item bar, where it shows: touch_draw)
    if (i == C_RUNLOCK) return false;                // (drawn and armed with the stick: control, on_lock_circle)
    // a menu of the game's open: the menu pad besides, nothing else changes (Plus shows whatever ShowPlus says: the
    // game's menus use it, as the way out of the pause menu)
    if (i == C_MPAD) return menu && mpad_shown();
    if (of_mpad(i)) return menu && mpad_shown() && mpad_member(i);
    if (menu && i == C_MENU) return true;
    if (is_ranged(i) ? !ranged_layout() : ranged_layout() && (i == C_ATTACK || i == C_HEAVY || i == C_LIGHT || i == C_CORNER ||
                                                              i == C_R || i == C_DODGE || i == C_SHEATHE))
        return false;
    if (is_combo(i)) return config_live_touch_combo(i - C_COMBO) != 0;
    // (ZL in a town, whatever the settings say: it is the way to the multiplayer menu; in a hunt it is the target button,
    // ShowZL. It stays while a menu opened in the town is open, whether the town's update runs under it or not)
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
    if (i == C_ITEML || i == C_ITEMR) return config_live_touch_show(TB_ITEM_ARROWS) != 0;
    if (i == C_RUNLOCK) return config_live_touch_show(TB_STICK_RUN) != 0;
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
// Running with the stick ([Touch] StickRun), in the stick base's radius r, as before: the thumb past RUN_ON runs (R held:
// the dash, with the weapon sheathed) and lets go under RUN_OFF. And Call of Duty Mobile's sprint lock: a small circle
// LOCK_AT above the stick (LOCK_EDGE below the screen's top at least), shown as the thumb pushes up past the rim. The thumb
// on it (within LOCK_GRAB of its middle) arms it, lit; slid out again it is only the rim's run, so passing over it does
// nothing. Lifted there, the run locks and goes on forward (the stick held full up: where the camera looks, the right
// thumb steering) until the stick or the circle is touched again, or a run can't go on (run_end). The same in the MH4U
// port's touch.cpp
const float RUN_ON = 1.6f, RUN_OFF = 1.45f, LOCK_AT = 2.4f, LOCK_R = .42f, LOCK_GRAB = .55f, LOCK_EDGE = .02f;
const float LOCK_UP = .7f;  // (the thumb's way within about 45 degrees of straight up: the circle shows)
const uint64_t RUN_X_WAIT = 400 * 1000000ull;  // (X pressed: no run's R until this long after: touch_input)
// The lock circle's middle, for a stick at (bx, by) of radius r on a screen h high
ImVec2 lock_spot(float bx, float by, float r, float h) { return ImVec2(bx, std::max(by - LOCK_AT * r, LOCK_R * r + LOCK_EDGE * h)); }
const float CAMERA_COUNTS = 1150.0f; // mouse counts for a drag the screen's height (hle/mousecam.c: 0.08 degrees each)
const uint64_t MS = 1000000ull;

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
// The layout editor's panel: its middle where the player dragged it (fractions of the screen's width from the left and
// of its height from the top; below 0 its usual place, a little above the middle) and its size (EPANEL_MIN to 100 %).
// Kept in the file however the editor closes, Save or Cancel (editor_panel in touch_layout.txt: the MH4U port's the same)
struct EditPanel {
    float x = -1.0f, y = -1.0f, k = 1.0f;
    bool operator!=(const EditPanel &o) const { return x != o.x || y != o.y || k != o.k; }
};
const float EPANEL_MIN = .6f;
// (its line in the file, save_layout's and save_panel's, under this note)
const char *const EPANEL_NOTE = "# the layout editor's panel: its middle (the screen's width from the left, its height from "
                                "the top; -1 -1 its usual place), size\n";
void epanel_line(char out[80], const EditPanel &ep) { snprintf(out, 80, "editor_panel %.4f %.4f %.2f\n", ep.x, ep.y, ep.k); }
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
#define LAYOUT_FILE layout_file()

bool load_layout(Layout *l, EditPanel *ep) {
    *l = defaults(), *ep = EditPanel{};
    FILE *f = fopen(LAYOUT_FILE, "r");
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
        bool usual = x < 0 && y < 0, placed = x >= 0 && x <= 1 && y >= 0 && y <= 1;
        if (!strcmp(key, "editor_panel") && (usual || placed) && s >= EPANEL_MIN && s <= 1.0f)
            ep->x = usual ? -1.0f : x, ep->y = usual ? -1.0f : y, ep->k = s;
    }
    fclose(f);
    return true;
}

void save_layout(const Layout &l, const EditPanel &ep) {
    FILE *f = fopen(LAYOUT_FILE, "w");
    if (!f) { rt_logf("[touch] could not save %s\n", LAYOUT_FILE); return; }
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
    char line[80];
    epanel_line(line, ep);
    fprintf(f, "%s%s", EPANEL_NOTE, line);
    fclose(f);
    rt_logf("[touch] layout saved (%s)\n", LAYOUT_FILE);
}
// The panel's line alone into the file, the rest of it as it is: Cancel keeps where the panel was put but not the
// controls' or the HUD's places, so that a player who never saved a layout still has none (a file with the panel only:
// the loader takes the defaults for the rest, a later version's new ones too)
void save_panel(const EditPanel &ep) {
    std::string was, now;
    if (FILE *f = fopen(LAYOUT_FILE, "r")) {
        char buf[512];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) was.append(buf, n);
        fclose(f);
    }
    char line[80], key[32];
    epanel_line(line, ep);
    bool put = false;
    for (size_t at = 0, end; at < was.size(); at = end) {
        end = was.find('\n', at);
        end = end == std::string::npos ? was.size() : end + 1;
        std::string l = was.substr(at, end - at);
        if (sscanf(l.c_str(), "%31s", key) != 1 || strcmp(key, "editor_panel")) now += l;
        else if (!put) now += line, put = true;  // (in its place; a second one dropped)
    }
    if (!put) {  // (none yet: at the end, under its note)
        if (!now.empty() && now.back() != '\n') now += '\n';
        now += EPANEL_NOTE, now += line;
    }
    FILE *f = fopen(LAYOUT_FILE, "w");
    if (!f) { rt_logf("[touch] could not save %s\n", LAYOUT_FILE); return; }
    fputs(now.c_str(), f);
    fclose(f);
    rt_logf("[touch] layout editor: its panel saved alone (%s%s)\n", LAYOUT_FILE, was.empty() ? ", the rest the defaults" : "");
}

struct Geo {
    ImVec2 c;
    float r;
};
float mpad_above(void) {  // (how far the menu pad's group reaches above it: the other pair of tabs shown, higher)
    return config_live_touch_show(TB_MENU_ZL) || config_live_touch_show(TB_MENU_ZR) ? MPAD_ABOVE2 : MPAD_ABOVE;
}
// an arrow of the item bar's, or the sprint lock's circle, the player has placed (else it follows the bar, the stick)
bool arrow_placed(const Layout &l, int i) { return fabsf(l.x[i] - DEFS[i].x) > 1e-4f || fabsf(l.y[i] - DEFS[i].y) > 1e-4f; }
Geo arrow_follow(const Layout &l, int i, float w, float h);
Geo place(const Layout &l, int i, float w, float h) {
    const Def &d = DEFS[i];
    if ((i == C_ITEML || i == C_ITEMR) && !arrow_placed(l, i)) return arrow_follow(l, i, w, h);
    if (i == C_RUNLOCK && !arrow_placed(l, i)) {  // (not placed: above the stick, with it)
        Geo s = place(l, C_STICK, w, h);
        return {lock_spot(s.c.x, s.c.y, s.r, h), LOCK_R * s.r * l.scale[i]};
    }
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

// A Hunter Art's row (top to bottom: the game's art 1, 2, 3): what performs it, by the player's own controls in the
// game's Options (hle/mousemenu.c game_control_type), as the game shows beside each art. Type 1 the d-pad up, right, down
// (a d-pad with that arm marked); Type 2 R + A + B (all three at once: the game takes them in a short window), ZL, ZR;
// Type 3 ZL with X, A, B ("at the same time"); Type 4 the right stick up, right, down. R Stick Controls swaps the right
// stick and the d-pad, L/R Button Controls L / R and ZL / ZR. (A tap pressed Type 1's d-pad whatever the type: with Type
// 3 it only turned the camera, #125)
const int ART_DIR[3] = {4, 1, 8};  // (dpad_part's bits: up, right, down)
struct ArtPress {
    uint64_t buttons;  // pressed together
    int stick;         // or the right stick tilted all the way that way (dpad_part's bits)
};
uint64_t lr_swapped(uint64_t b) {  // (L/R Button Controls Type 2: L <-> ZL, R <-> ZR)
    const uint64_t SHOULDERS = B(BTN_L) | B(BTN_R) | B(BTN_ZL) | B(BTN_ZR);
    return (b & ~SHOULDERS) | (b & B(BTN_L) ? B(BTN_ZL) : 0) | (b & B(BTN_ZL) ? B(BTN_L) : 0) |
           (b & B(BTN_R) ? B(BTN_ZR) : 0) | (b & B(BTN_ZR) ? B(BTN_R) : 0);
}
ArtPress art_press(int row, int type, bool rstick_swap, bool lr_swap) {
    if (row < 0 || row > 2) return {0, 0};
    if (type == 0 || type == 3) {  // (Type 1 the d-pad, Type 4 the right stick: the option swaps them)
        if ((type == 3) != rstick_swap) return {0, ART_DIR[row]};
        return {dpad_buttons(ART_DIR[row]), 0};
    }
    static const uint64_t TYPE2[3] = {B(BTN_R) | B(BTN_A) | B(BTN_B), B(BTN_ZL), B(BTN_ZR)};
    static const uint64_t TYPE3[3] = {B(BTN_ZL) | B(BTN_X), B(BTN_ZL) | B(BTN_A), B(BTN_ZL) | B(BTN_B)};
    uint64_t b = (type == 1 ? TYPE2 : TYPE3)[row];
    return {lr_swap ? lr_swapped(b) : b, 0};
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
        if (!editor && i == C_HDPAD && in_mode(C_MPAD, menu)) continue;  // (the menu pad in its place: under_mpad)
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
    // (between the two attack buttons: both, as the pill's seam; only where they are: in the ranged layout the seam's
    // place, under the fire button in a layout where the two sit round it, pressed X + A for a tap on the shot - crouching
    // fire, no locked shot: the owner's Galaxy, 2026-11-10)
    if (part && !menu && g_style.load() == STYLE_SEPARATE && in_mode(C_LIGHT, menu) && in_mode(C_HEAVY, menu)) {
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
// (while the game is held - the settings menu, the layout editor, playing alone: overlay.cpp - its last picture stays on
// the screen, and what it drew counts as just drawn: the editor's frames stay live)
std::atomic<uint64_t> g_held_ns;
std::atomic<uint64_t> g_part_moved_ns, g_step_ns;  // (a part of the HUD moved in the editor, the game's last step: touch_editor_step)
std::atomic<float> g_aim_pitch_deg;  // (the aim's slide up or down, degrees not yet given to the aim: touch_aim_pitch)
uint64_t hud_now(void) {
    uint64_t held = g_held_ns.load();
    return held ? held : SDL_GetTicksNS();
}
bool stale(const Zones &z) { return hud_now() - z.ns > stale_limit(); }
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
// ...but a window whose cursor runs is a menu, however it came: the full pouch's after a gathering (it comes as the
// gathering ends, longer than WINDOW_PRESS after the A on a mining spot), or the pause menu opened while a bubble was up.
// Taken for a bubble, they had no menu pad and their cursor could not be moved (#74, #107). Only a menu runs one (a list,
// a box, a quantity, a choice of two: hle/mousemenu.c)
// Without a window, a while without the hunter played is a menu (the title, the main menu, a talk in a town), but never
// with a hunter in a quest (weapon_hunter): a ballista's or a cannon's own aim runs no hunt camera, and four seconds on
// one made a menu of it - the stick turned into the d-pad, a drag into a cursor's steps, and nothing aimed (#169). A
// cutscene there reads no stick either way.
// A bow or a gun out in a quest: its aims and the scope draw on that layer, and what they draw outlasts them (the action
// stays 0001:046 after an aim or the scope, with the hunt camera back, which weapon_aiming no longer counts since #177),
// while the lock-on's target button (ZL) is pressed all the time. There only Plus or Minus (the pause menu, the map) make
// a window a menu, and a cursor running one only after them: a heavy bowgun's hunt went to the menu layout, its shot
// taken by the menu pad, no locked shot, no item bar arrows, no crouching fire
std::atomic<uint64_t> g_menu_press_ns;  // (SDL_GetTicksNS) when Plus or Minus was last pressed
bool game_menu(uint32_t hunter) {
    uint64_t m = g_menu_ns.load(), p = g_play_ns.load(), now = plat_now_ns(), ticks = SDL_GetTicksNS();
    if (m && (ticks - m < stale_limit() || g_frames_since_menu.load() < 3)) {
        bool gun = hunter && weapon_ranged(hunter) && weapon_drawn() == 1;
        bool pressed = ticks - (gun ? g_menu_press_ns.load() : g_press_ns.load()) < WINDOW_PRESS;
        if (!g_window_open) g_window_asked = !p || now - p > PLAY_NOW || pressed;
        if (!g_window_asked && menu_cursor_age() <= 1 && (!gun || pressed)) {
            g_window_asked = true;
            static int dbg = -1;
            if (dbg < 0) dbg = getenv("MHGU_TOUCH_LOG") != nullptr;
            if (dbg) rt_logf("[touch] a window that came by itself runs a menu's cursor: a menu\n");
        }
        g_window_open = true;
        if (g_window_asked) return true;
    } else {
        g_window_open = false;
    }
    return (!p || now - p > PLAY_HOLD) && !hunter;
}
// A bow or a gun aimed in a way weapon_aiming doesn't know (a style's aim): the weapon out, its own camera up (the hunt
// camera's handler stopped) and the player's A, R or X down just now (touch_lock_shot, every input's buttons). Not a
// cutscene, a carve or a window that comes with the weapon out: the hunt camera stops there too, without those (#177)
std::atomic<uint64_t> g_ranged_press_ns;  // (plat_now_ns: the last update with A, R or X down and a bow or a gun out)
bool ranged_aim(uint32_t hunter) {
    uint64_t p = g_ranged_press_ns.load();
    return p && plat_now_ns() - p < 1500 * 1000000ull && weapon_ranged(hunter) && weapon_drawn() == 1 && !mousecam_hunt_camera();
}
// (a bow drawn or a gun aimed has a camera of its own: the hunt camera's handler doesn't run then)
void play_seen(uint32_t hunter) {  // (the SDL main thread, each input update)
    uint64_t now = plat_now_ns(), town = g_town_ns;
    if (mousecam_active() || (town && now - town < 400 * 1000000ull) || weapon_moving(hunter) || weapon_aiming(hunter) ||
        ranged_aim(hunter))
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
// A model shown in a window of the game's (the smithy's weapon, the item box's hunter: touch_hud_preview), which the game
// turns with the sticks, as frames presented since the last one that drew it
std::atomic<int> g_frames_since_preview{1000};
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
int art_row_at(const Zones &z, float w, float h, float y) {  // (the row under a finger at y, or the nearest one; -1 none)
    float rows[3][2];
    int nr = arts_rows(z, w, h, rows), row = -1;
    float best = 1e30f;
    for (int i = 0; i < nr; i++) {
        float d = y < rows[i][0] ? rows[i][0] - y : y > rows[i][1] ? y - rows[i][1] : 0;
        if (d < best) best = d, row = i;
    }
    return row;
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
    *live = same && hud_now() - sn.ns <= stale_limit();
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
enum Kind { F_NONE, F_BUTTON, F_STICK, F_CAMERA, F_MENU, F_ITEM, F_ARTS, F_EDIT };
struct Finger {
    SDL_FingerID id;
    Kind kind;
    int ctl, part;       // the control (F_BUTTON, F_EDIT), the pill's part
    float x, y, x0, y0;  // pixels: now, where it landed
    uint64_t t0;
    float swipe, swipe_y;  // the item bar, a menu's cursor: pixels swiped since the last step
    bool moved, done;
    uint64_t repeat_ns;    // an ammo arrow held: its next step then
    uint64_t btn;          // a control of the ranged layout: what it pressed when it landed (the weapon may go out under it)
    int sent;              // the menu pad's: the direction it presses now (dpad_part's; a tab, Minus: 1 pressed), which
                           // follows part as press_ok lets a press start
    uint64_t keep;         // a thumb from Guard slid onto the attacks ([Touch] GuardSlide): R kept with them
    bool on_bar;           // ...or one on Guard slid onto the item bar: R + Y while it is there
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
    uint64_t bow_leave, bow_leave_until;  // a bow's aim held and let go: its button pressed again to leave the aiming
                                          // mode once the hunter is in it (touch_input), until then at most
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
    bool run;                // running: R held (the stick's locked run, or the Run button held)
    bool run_lock;           // the stick's run locked ([Touch] StickRun): the thumb lifted on the lock circle, the
    bool run_free;           // stick stays pushed full forward (both: a lock is only ever a lifted thumb's)
    bool run_armed;          // the thumb landed, or came back inside the rim since the last lock ended (it may lock)
    bool run_over;           // the thumb on the lock circle, past the rim, a lock possible: lifted there it locks (lit)
    bool run_can;            // a lock could start now (StickRun, sheathed, no menu, the hunter played: the circle shows)
    bool rim;                // the thumb past the rim runs (RUN_ON, down to RUN_OFF)
    uint64_t run_x_until;    // an attack that draws the weapon (X) was pressed: no run's R until then (#151)
    float fx, fy;            // the stick's thumb where it is (the knob stops at the rim)
    bool lock;               // the lock-on is on (hle/mousecam.c turns the hunt camera to the monster)
    uint64_t hunt_ns;        // when the hunt camera last ran (away 10 s: the lock-on goes off)
    uint64_t item_flash, art_flash;
    bool bar_open;           // a long press on the item bar holds it open (hle/itembar.c itembar_hold)...
    SDL_FingerID bar_finger; // ...that finger's
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
    uint64_t assist_t0;                  // (when its R began: it waits for the aim from then)
    uint64_t combo_t0;                   // (its press of A, held back for the game's two-button window: 0 none)
    uint64_t keep_r;                     // (it ended by B, X or Y: its R kept down until then, so it is no tap: the scope)
    uint64_t keep_buttons, replay_until; // (...those B, X, Y, held back meanwhile and pressed for the game until then)
    bool assist_wait, fired;             // it waits for the aim; the fire button was down
    bool assist_off;                     // a dodge, a reload or a sheathe pressed since the fire button: no aim until it is up
    bool still_wait;                     // the aim waited for is a shot's standing still: 0.1.8's (touch_lock_shot)
    uint64_t still_until;                // (a shot standing still: the hunter turned to the monster until then, hle/mousecam.c)
    uint64_t help_r0;                    // (the help's R pressed since then: never let go as a tap, touch_lock_shot)
    uint64_t menu_seen_ns;               // (a window taken for a menu in a hunt since then: MENU_SETTLE)
    uint64_t shot_others, others_ns;     // (B, X, Y at the last locked-shot update: their presses; the last one's time)
    int drawn_known, weapon_known;       // the last weapon_drawn that was known (0 / 1), the last weapon in hand...
    uint64_t hunter_ns;                  // ...and when the hunter was last found (SDL ticks)
    bool prowler;                        // the player is a Prowler (its item bar: Support Moves), at the last touch_input
    int rs_dir;                          // a Hunter Art on the right stick (Type 4): its direction (dpad_part's bits)...
    uint64_t rs_until;                   // ...tilted until then (touch_right_stick)
    bool preview;                        // a model in a menu of the game's turns with the sticks: they are the game's there
    float w, h;              // the screen, pixels
    Layout layout, saved;
    bool loaded, touched, auto_wait;
    bool menu;  // a menu of the game's is open (game_menu, at the last touch_input)  // (auto_wait: a desktop's Auto, waiting for the first finger)
    bool edit, edit_request;  // the layout editor
    int edit_prof;            // ...the HUD it shows and edits (HUD_*: the switch)
    int sel;
    bool pinch;
    float pinch_d0, pinch_s0;
    EditPanel epanel, epanel_open;  // the editor's panel: where it is and how big, and as the editor opened
    int rep_btn;                    // ...its - or + that sized it last (-1 none), when, and where it was then
    uint64_t rep_ns;
    Rect rep_at;
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
std::atomic<bool> g_lock_engaged; // (the lock-on is on, whatever else: hle/mousecam.c keeps its monster while it is)
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
// (the lock-on waits so long after a turn by hand or an L, as the MH4U port's; a press on L so long turns it on or off)
const uint64_t LOCK_AFTER_TURN = 600 * MS, LOCK_HOLD = 600 * MS, MIN_PRESS = 70 * MS;
// (the aim's tap: let go within this, the game's own tap of R, under its 14 frames at 60 FPS with an update to spare)
const uint64_t AIM_TAP = 180 * MS;
const uint64_t BAR_HOLD = 400 * MS;  // (a finger kept this long on the item bar, unmoved, holds it open: as the MH4U port)

// The Run button at its default place in a layout of the corner style (saved before the button was there): inside the
// band's hollow, where next_style puts it, when the band would cover it (both are placed from the same corner: any
// screen's shape tells)
void run_clear_of_band(Layout &l) {
    if (l.style != STYLE_CORNER || l.x[C_RUN] != DEFS[C_RUN].x || l.y[C_RUN] != DEFS[C_RUN].y) return;
    Geo band = place(l, C_CORNER, 2.0f, 1.0f), o = place(l, C_RUN, 2.0f, 1.0f);
    if (band_distance(band, o.c.x, o.c.y, nullptr) < band.r * BAND_T + o.r) l.x[C_RUN] = .325f, l.y[C_RUN] = .455f;
}

void ensure_loaded(State &s) {
    if (s.loaded) return;
    s.loaded = true, s.sel = -1;
    g_scheme = config_live_touch_scheme();
    bool had = load_layout(&s.layout, &s.epanel);
    run_clear_of_band(s.layout);
    s.saved = s.layout;
    publish_hud(s.layout);
    g_style = s.layout.style;
    rt_logf("[touch] layout: %s\n", had ? LAYOUT_FILE : "the defaults");
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
void l_step(State &s, uint64_t button) {
    uint64_t now = SDL_GetTicksNS();
    uint64_t at = now < s.ammo_hold ? std::max(now + 20 * MS, s.ammo_ready) : now + 380 * MS;
    pulse(s, button, 80 * MS, at - now);
    s.ammo_ready = at + 170 * MS;
    s.ammo_hold = at + 320 * MS;
}
void ammo_step(State &s, int dir) { l_step(s, dir > 0 ? B(BTN_X) : B(BTN_B)); }
// A step of a Prowler's Support Moves, which its item bar shows: L held, Y the one to the left, A the one to the right (the
// game's L + Y / A). The hunter's item bar is turned directly instead (hle/itembar.c), which a Prowler's is not (#126)
void support_step(State &s, int dir) { l_step(s, dir > 0 ? B(BTN_A) : B(BTN_Y)); }
// The item bar turned a step by the arrows or a swipe (dir < 0 left): the hunter's items, a Prowler's Support Moves
void item_step(State &s, int dir, uint64_t now) {
    if (s.prowler) support_step(s, dir);
    else itembar_scroll(dir);
    s.item_dir = dir, s.item_flash = now + 200 * MS;
    tap_due();
}
// A tap on a Hunter Art's row: what the player's controls perform it with (art_press), pressed as a tap
void art_tap(State &s, int row, uint64_t now) {
    int type = game_control_type();
    bool rswap = game_rstick_swapped(), lrswap = game_lr_swapped();
    ArtPress p = art_press(row, type, rswap, lrswap);
    if (p.stick) s.rs_dir = p.stick, s.rs_until = now + 120 * MS;
    else if (p.buttons) pulse(s, p.buttons, 120 * MS);
    s.art_row = row, s.art_flash = now + 250 * MS;
    tap_due();
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("MHGU_TOUCH_LOG") != nullptr;
    if (dbg)
        rt_logf("[touch] Hunter Art %d: Control Type %d%s%s, %s %llx\n", row + 1, type + 1, rswap ? ", R Stick swapped" : "",
                lrswap ? ", L/R swapped" : "", p.stick ? "the right stick" : "buttons",
                (unsigned long long)(p.stick ? (uint64_t)p.stick : p.buttons));
}
// The item bar's arrows ([Touch] ShowItemArrows): beside the bar, wherever it has been moved to
// The item bar's arrows, one each side of the bar at its middle: ITEM_ARROW screen heights in radius, smaller where the
// screen's edge leaves less room beside the bar (both the same size, at least ITEM_ARROW_MIN). The MH4U port's touch.cpp
// places its own by the same rule
const float ITEM_ARROW = .052f, ITEM_ARROW_MIN = .03f;
Geo item_arrow(const Rect &item, int d, float w, float h, float k = 1.0f) {
    float room = std::min(item.x0, w - item.x1) / 2.2f;  // (a radius and a little gap each side)
    float r = std::max(std::min(ITEM_ARROW * k * h, room), ITEM_ARROW_MIN * h);
    float x = d < 0 ? item.x0 - r * 1.15f : item.x1 + r * 1.15f;
    return {ImVec2(std::clamp(x, r, std::max(r, w - r)), (item.y0 + item.y1) * .5f), r};
}
// The item bar its arrows go by while the player hasn't placed them: the hunt's; in the layout editor, the game paused
// under it, the bar as last drawn, moved and sized as the editor has its box since (part_frame's way)
Rect arrow_bar(float w, float h) {
    Rect r = zone_rect(zones(), HP_ITEMS, w, h);
    int prof = g_hud_edit_prof;
    if (prof < 0 || g_edit_set.load() < 0 || !r.ok) return r;
    Seen sn;
    {
        std::lock_guard<std::mutex> l(g_zone_lock);
        sn = g_seen[HP_ITEMS];
    }
    if (!sn.ns || sn.prof != prof) return r;
    float e[4];
    hud_box(HP_ITEMS, prof, g_hud_arts_top, e);
    float kx = (e[2] - e[0]) / std::max(.001f, sn.e[2] - sn.e[0]), ky = (e[3] - e[1]) / std::max(.001f, sn.e[3] - sn.e[1]);
    Pic p = picture(w, h);
    auto mx = [&](float x) { return p.x0 + (e[0] + ((x - p.x0) * 1280.0f / p.w - sn.e[0]) * kx) * p.w / 1280.0f; };
    auto my = [&](float y) { return p.y0 + (e[1] + ((y - p.y0) * 720.0f / p.h - sn.e[1]) * ky) * p.h / 720.0f; };
    return {mx(r.x0), my(r.y0), mx(r.x1), my(r.y1), true};
}
// An arrow not placed by the player: beside the bar (its size the layout's, as a placed one's), or while no bar is known
// where they are beside the bar's usual place, at the bottom in the middle
Geo arrow_follow(const Layout &l, int i, float w, float h) {
    Rect bar = arrow_bar(w, h);
    if (bar.ok) return item_arrow(bar, i == C_ITEML ? -1 : 1, w, h, l.scale[i]);
    return {ImVec2(w * .5f + (i == C_ITEML ? -.34f : .34f) * h, h * .85f), ITEM_ARROW * l.scale[i] * h};
}
// (the editor: an arrow that follows the bar, or the sprint lock's circle that follows the stick, dragged, keeps the place
// it had as its own, then moves from there)
void pin_arrow(State &s, int i) {
    Geo g = place(s.layout, i, s.w, s.h);
    s.layout.x[i] = (from_right(i) ? s.w - g.c.x : g.c.x) / s.h, s.layout.y[i] = (s.h - g.c.y) / s.h;
    s.layout.scale[i] = std::clamp(g.r / (DEFS[i].r * s.h), .5f, 2.2f);
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
    // (inside the rim: pushed from here into the lock circle, it may lock; every finger event, not each update, so that a
    // quick flick counts)
    if (d < r) s.run_armed = true;
    if (d > r) x = s.bx + dx / d * r, y = s.by + dy / d * r;
    s.kx = x, s.ky = y;
}

// The stick's locked run ends (why: for the log); it locks again only for a thumb come back inside the rim, or a new one
void run_end(State &s, const char *why) {
    s.run_over = false;
    if (!s.run_lock) return;
    s.run_lock = s.run_free = s.run_armed = false;
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("MHGU_TOUCH_LOG") != nullptr;
    if (dbg) rt_logf("[touch] run let go (%s)\n", why);
}
// The thumb at (x, y) is on the lock circle, past the rim (a stick placed high has the circle clamped near it)
bool on_lock_circle(const State &s, float x, float y) {
    float r = DEFS[C_STICK].r * s.layout.scale[C_STICK] * s.h;
    Geo lg = place(s.layout, C_RUNLOCK, s.w, s.h);  // (where it is: above the stick, or where the editor put it)
    return hypotf(x - s.bx, y - s.by) > r && hypotf(x - lg.c.x, y - lg.c.y) < LOCK_GRAB / LOCK_R * lg.r;
}
// A thumb from Guard keeps R as it slides ([Touch] GuardSlide): with the weapon out, no menu of the game's open
bool guard_slide(const State &s) { return config_live_touch_show(TB_GUARD_SLIDE) && s.drawn == 1 && !s.menu; }
void guard_log(const char *what) {  // (MHGU_TOUCH_LOG)
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("MHGU_TOUCH_LOG") != nullptr;
    if (dbg) rt_logf("[touch] guard kept: %s\n", what);
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
    uint64_t b = f.ctl == C_ATTACK || f.ctl == C_CORNER ? PILL_BUTTONS[f.part]
               : of_mpad(f.ctl) ? mpad_buttons(f.ctl, f.sent)
               : is_dpad(f.ctl) ? dpad_buttons(f.part)
               : is_combo(f.ctl) ? (uint64_t)config_live_touch_combo(f.ctl - C_COMBO)
               : f.ctl == C_LIGHT && f.part ? PILL_BUTTONS[2]
               : f.btn || f.ctl == C_ZL ? f.btn  // (the target and camera buttons: what they pressed as they landed, target_buttons
                                                 // (0 nothing) and camera_buttons)
                       : DEFS[f.ctl].buttons;
    return b | f.keep | (f.on_bar ? B(BTN_Y) : 0);  // (a guard kept as it slid: [Touch] GuardSlide)
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
    run_end(s, "every finger let go of");
    s.bar_open = false;
}

// The lock-on on or off (the padlock by L, a long press on L, a controller's right stick click), as the settings allow
void lock_set(State &s, bool on, const char *by) {
    on = on && config_live_lock_on();
    if (on != s.lock) rt_logf("[touch] lock-on %s (%s)\n", on ? "on" : "off", by);
    s.lock = on, s.hunt_ns = SDL_GetTicksNS();
    g_lock_engaged = on;
}

// The padlock by L (the lock-on's, as the MH4U port's): where it is, its size; false where it doesn't show (L hidden, a
// menu of the game's open, the lock-on turned off in the settings)
bool padlock_place(const State &s, float w, float h, ImVec2 *c, float *size) {
    if (s.menu || s.edit || !config_live_lock_on() || !in_mode(C_L, false)) return false;
    Geo g = place(s.layout, C_L, w, h);
    *c = ImVec2(g.c.x - g.r * .72f, g.c.y - g.r * .62f), *size = std::max(g.r * .42f, .018f * h);
    return true;
}
bool padlock_hit(const State &s, float x, float y) {
    ImVec2 c;
    float size;
    return padlock_place(s, s.w, s.h, &c, &size) && hypotf(x - c.x, y - c.y) < size * 1.5f;
}

// The target button (ZL on the layout): in a hunt the game's change target, as the player's Control Type has it
// (hle/mousecam.c: ZL in Type 1, the d-pad in Type 2, the right stick's click in Type 3; the lock-on follows the
// monster it chooses); in a town ZL, the way to the multiplayer menu. 0: nothing a touch button can press
bool target_in_town(void) { return plat_now_ns() - g_town_ns < 500 * MS || g_menu_town.load(); }
uint64_t target_buttons(void) {
    if (target_in_town()) return B(BTN_ZL);
    return mousecam_target_buttons();
}
// The camera button (L on the layout) and a double tap on the camera's part: the game's camera centring, L, or ZL in a hunt
// with the game's L/R Button Controls swapped (L is then Type 1's change target, the target button's: both changed the
// monster, and nothing centred the camera); in a town L, beside the target button's ZL. The lock-on's pause for a turn by
// the game goes by the same button (touch_lock_buttons)
uint64_t camera_buttons(void) { return !target_in_town() && game_lr_swapped() ? B(BTN_ZL) : B(BTN_L); }
// and its badge: the button it presses (a d-pad's direction: an arrow, target_badge)
const char *target_letter(void) {
    uint64_t b = target_buttons();
    return b & B(BTN_L) ? "L" : b & B(BTN_RSTICK) ? "R3" : b & B(BTN_LSTICK) ? "L3" : b & B(BTN_DOWN) ? "v"
         : b & B(BTN_RIGHT) ? ">" : b & B(BTN_LEFT) ? "<" : b ? "ZL" : "";
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
// a part to move it, pinch (a second finger anywhere) or the panel's - / + to size it. The panel itself goes where its
// title strip is dragged (a grip at each end of it), and is as small as EPANEL_MIN: picked there, a pinch or its - / +
// size it, so that it leaves free what is being placed
enum { E_SMALLER, E_BIGGER, E_HIDE, E_RESET, E_SET, E_STYLE, E_PROF, E_DEFAULTS, E_CANCEL, E_SAVE, E_COUNT };
const int SEL_PANEL = -2;  // (s.sel / a finger's ctl: the editor's panel, picked by its title strip)
const uint64_t PANEL_REPEAT = 700 * MS;  // (taps on its - or + this close together: one button's, State::rep_btn)
bool grabbed(int ctl) { return ctl >= 0 || ctl == SEL_PANEL; }  // (a finger's ctl: something it moves)
void panel_dims(float w, float h, float k, float *pw, float *ph) { *pw = std::min(w * .94f, 1.9f * h) * k, *ph = .28f * h * k; }
// The panel, its buttons and its title strip (grab: where a finger picks and drags it), on the screen whole; its measure
// (the screen's height at 100 %, smaller with it)
float editor_panel(const EditPanel &ep, float w, float h, Rect *panel, Rect btn[E_COUNT], Rect *grab = nullptr) {
    float k = std::clamp(ep.k, EPANEL_MIN, 1.0f), u = k * h, pw, ph;
    panel_dims(w, h, k, &pw, &ph);
    float x0 = ep.x < 0 ? (w - pw) * .5f : ep.x * w - pw * .5f, y0 = ep.y < 0 ? (h - ph) * .4f : ep.y * h - ph * .5f;
    x0 = std::clamp(x0, 0.0f, std::max(0.0f, w - pw)), y0 = std::clamp(y0, 0.0f, std::max(0.0f, h - ph));
    *panel = {x0, y0, x0 + pw, y0 + ph, true};
    float gap = .012f * u, bh = .082f * u, r1 = y0 + .1f * u, r2 = r1 + bh + gap, bw = (pw - gap * 5) / 4;
    if (grab) *grab = {x0, y0, x0 + pw, r1 - gap * .5f, true};
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
    return u;
}
// The panel dragged by (dx, dy) pixels, kept on the screen whole
void panel_move(State &s, float dx, float dy) {
    Rect p, b[E_COUNT];
    editor_panel(s.epanel, s.w, s.h, &p, b);
    float hw = (p.x1 - p.x0) * .5f, hh = (p.y1 - p.y0) * .5f;
    s.epanel.x = std::clamp((p.x0 + p.x1) * .5f + dx, hw, std::max(hw, s.w - hw)) / s.w;
    s.epanel.y = std::clamp((p.y0 + p.y1) * .5f + dy, hh, std::max(hh, s.h - hh)) / s.h;
}
// The panel sized (whole percents, EPANEL_MIN to 100 %) about a point of it that stays where it is: the - or + under the
// finger (a few taps in a row stay on it), its middle for a pinch
void panel_resize(State &s, float to, float ax, float ay) {
    Rect p, b[E_COUNT];
    editor_panel(s.epanel, s.w, s.h, &p, b);
    float fx = (ax - p.x0) / std::max(1.0f, p.x1 - p.x0), fy = (ay - p.y0) / std::max(1.0f, p.y1 - p.y0), pw, ph;
    s.epanel.k = std::clamp(roundf(to * 100.0f) / 100.0f, EPANEL_MIN, 1.0f);
    panel_dims(s.w, s.h, s.epanel.k, &pw, &ph);
    s.epanel.x = (ax + (.5f - fx) * pw) / s.w, s.epanel.y = (ay + (.5f - fy) * ph) / s.h;
    panel_move(s, 0, 0);
}

// [Touch] Layout changed: the other layout and its file; the fingers on what it lacks let go
void switch_scheme(State &s, int scheme) {
    g_scheme = scheme;
    bool had = load_layout(&s.layout, &s.epanel);
    run_clear_of_band(s.layout);
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
    s.saved = s.layout, s.epanel_open = s.epanel, s.rep_btn = -1;
    drop_fingers(s);
    rt_logf("[touch] layout editor\n");
}
void edit_end(State &s, bool keep) {
    if (keep) save_layout(s.layout, s.epanel), s.saved = s.layout;
    else s.layout = s.saved;
    if (s.epanel != s.epanel_open) {  // (the panel stays where it was put, and as big, Save or Cancel: Cancel its line alone)
        if (!keep) save_panel(s.epanel);
        rt_logf("[touch] layout editor: its panel at %.2f %.2f, %d%%\n", s.epanel.x, s.epanel.y, (int)lroundf(s.epanel.k * 100));
    }
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
    static const struct { int c; float x, y; } MOVED[3] = {{C_DODGE, .33f, .33f}, {C_SHEATHE, .5f, .5f}, {C_RUN, .325f, .455f}};
    for (const auto &m : MOVED) {
        Layout before = s.saved.style == STYLE_CORNER ? defaults() : s.saved;
        s.layout.x[m.c] = before.x[m.c], s.layout.y[m.c] = before.y[m.c];
        if (style != STYLE_CORNER) continue;
        Geo band = place(s.layout, C_CORNER, s.w, s.h), o = place(s.layout, m.c, s.w, s.h);
        if (band_distance(band, o.c.x, o.c.y, nullptr) < band.r * BAND_T + o.r)
            s.layout.x[m.c] = m.x, s.layout.y[m.c] = m.y;
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
    g_part_moved_ns = SDL_GetTicksNS();  // (the game draws it there: touch_editor_step)
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
    g_part_moved_ns = SDL_GetTicksNS();  // (the game draws it there: touch_editor_step)
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
// The touch layout editor shows and edits the touch controls with their HUD; with MoveHUD Off there is one HUD for all,
// the one the touch controls play over, so they show and move with it (they were left out: only the HUD moved, #189)
static bool edit_controls(const State &s) { return s.edit_prof == HUD_TOUCH || !config_live_touch_hud(); }

void edit_button(State &s, int i) {
    int part = s.sel >= ZONE_SEL ? s.sel - ZONE_SEL : -1, c = s.sel >= 0 && s.sel < C_COUNT ? s.sel : -1;
    bool touch = edit_controls(s);
    switch (i) {
    case E_SMALLER:
    case E_BIGGER: {
        float by = i == E_BIGGER ? .1f : -.1f;
        if (part >= 0) part_size(s, part, s.layout.hud[s.edit_prof][part].k + by);
        else if (c >= 0) s.layout.scale[c] += by, clamp_place(s, c);
        else if (s.sel == SEL_PANEL) {  // (about the button pressed: it stays under the finger)
            Rect p, b[E_COUNT];
            editor_panel(s.epanel, s.w, s.h, &p, b);
            panel_resize(s, s.epanel.k + by, (b[i].x0 + b[i].x1) * .5f, (b[i].y0 + b[i].y1) * .5f);
        }
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
        else if (s.sel == SEL_PANEL) s.epanel = EditPanel{};  // (the panel: its usual place and size)
        break;
    case E_SET:
        if (!touch || trad()) break;  // (Traditional has no ranged layout)
        g_edit_set = g_edit_set.load() == 1 ? 0 : 1;
        if (part < 0 && s.sel != SEL_PANEL) s.sel = -1;
        break;
    case E_STYLE:
        if (touch && !ranged_layout() && !trad()) next_style(s);
        break;
    case E_PROF:
        s.edit_prof = touch ? HUD_PLAIN : HUD_TOUCH, g_hud_edit_prof = s.edit_prof;
        if (part < 0 && s.sel != SEL_PANEL) s.sel = -1;  // (a control: none in the HUD without them)
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
        Rect panel, btn[E_COUNT], grab;
        editor_panel(s.epanel, s.w, s.h, &panel, btn, &grab);
        // (taps in a row where the panel's - or + was press it again, though the panel it sizes slid at the screen's edge
        // and put another button there)
        uint64_t now = SDL_GetTicksNS();
        int again = s.rep_btn >= 0 && now - s.rep_ns < PANEL_REPEAT && s.rep_at.in(x, y) ? s.rep_btn : -1;
        for (int i = 0; i < E_COUNT; i++)
            if (i == again || (again < 0 && btn[i].in(x, y))) {
                f->kind = F_MENU, f->ctl = i;  // (held until it lifts: nothing under it moves; the button it lights)
                bool sizing = s.sel == SEL_PANEL && (i == E_SMALLER || i == E_BIGGER);
                edit_button(s, i);
                if (sizing) s.rep_btn = i, s.rep_ns = now, s.rep_at = again < 0 ? btn[i] : s.rep_at;
                else s.rep_btn = -1;
                if (!s.edit) f->kind = F_NONE;
                return;
            }
        s.rep_btn = -1;
        Finger *drag = nullptr;
        for (Finger &g : s.f)
            if (&g != f && g.kind == F_EDIT && grabbed(g.ctl)) drag = &g;
        if (drag) {  // a second finger: pinch what the first one drags, a control, a part or the panel
            s.pinch = true, s.pinch_d0 = std::max(1.0f, hypotf(x - drag->x, y - drag->y));
            s.pinch_s0 = drag->ctl == SEL_PANEL ? s.epanel.k
                       : drag->ctl >= ZONE_SEL ? s.layout.hud[s.edit_prof][drag->ctl - ZONE_SEL].k : s.layout.scale[drag->ctl];
            f->kind = F_EDIT;
            return;
        }
        // the panel's title strip: the panel, picked and dragged, over what is under it. A little past its edges (not into
        // the buttons below it) it is the panel only where nothing else is: a control or a part there stays the one picked
        auto strip = [&](float m) { return x >= grab.x0 - m && x <= grab.x1 + m && y >= grab.y0 - m && y <= grab.y1; };
        if (strip(0.0f)) {
            f->kind = F_EDIT, f->ctl = SEL_PANEL;
            s.sel = SEL_PANEL;
            return;
        }
        // a control (with the touch controls' HUD), else a part of the game's HUD, else the panel's face (nothing moves),
        // else the panel by its title strip's edge
        int c = edit_controls(s) ? hit(s.layout, x, y, s.w, s.h, nullptr, 1.5f, true, false, true) : -1;
        if (c < 0) {
            int k = part_hit(s, x, y);
            if (k >= 0) c = ZONE_SEL + k;
        }
        if (c < 0 && panel.in(x, y)) { f->kind = F_MENU; return; }
        if (c < 0 && strip(.02f * s.h)) c = SEL_PANEL;
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
            int c = a && grabbed(a->ctl) ? a->ctl : b && grabbed(b->ctl) ? b->ctl : -1;
            if (a && b && grabbed(c)) {
                float by = hypotf(a->x - b->x, a->y - b->y) / s.pinch_d0;
                if (c == SEL_PANEL) {  // (about its middle)
                    Rect p, bt[E_COUNT];
                    editor_panel(s.epanel, s.w, s.h, &p, bt);
                    panel_resize(s, s.pinch_s0 * by, (p.x0 + p.x1) * .5f, (p.y0 + p.y1) * .5f);
                } else if (c >= ZONE_SEL) part_size(s, c - ZONE_SEL, s.pinch_s0 * by);
                else s.layout.scale[c] = s.pinch_s0 * by, clamp_place(s, c);
            }
        } else if (f->ctl == SEL_PANEL) {
            panel_move(s, dx, dy);
        } else if (f->ctl >= ZONE_SEL) {
            part_move(s, f->ctl - ZONE_SEL, dx, dy);
        } else if (f->ctl >= 0) {
            if ((f->ctl == C_ITEML || f->ctl == C_ITEMR || f->ctl == C_RUNLOCK) && !arrow_placed(s.layout, f->ctl)) pin_arrow(s, f->ctl);
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
    if (keep) save_layout(s.layout, s.epanel), s.saved = s.layout;
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

uint64_t aim_hold_ns(void);  // (the game's R held this long is never a tap: touch_lock_shot's)
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
        if (!g_visible && !s.auto_wait) {
            int part = 0;
            if (g_gear_only && hit(s.layout, x, y, s.w, s.h, &part, 1.22f, false, s.menu) == C_SETTINGS) {
                f->kind = F_BUTTON, f->ctl = C_SETTINGS, f->part = part;  // (a tap opens the menu, as when they show)
                tap_due();
                return true;
            }
            // (a controller in the hands, a handheld's own: a tap on a Hunter Art's panel still performs it, without
            // letting go of the stick to reach the d-pad, #48)
            if (g_gear_only && !s.menu) {
                Zones z = zones();
                if (zone_rect(z, HP_ARTS, s.w, s.h).in(x, y)) {
                    f->kind = F_ARTS, f->part = art_row_at(z, s.w, s.h, y);
                    return true;
                }
            }
            f->kind = F_CAMERA;  // (hidden: a drag still turns the camera)
            return true;
        }
        Zones z = zones();
        // (in a menu of the game's the HUD's parts take no finger: a finger there is a drag that moves the cursor)
        Rect none = {0, 0, 0, 0, false}, item = zone_rect(z, HP_ITEMS, s.w, s.h), arts = s.menu ? none : zone_rect(z, HP_ARTS, s.w, s.h);
        // the padlock by L: a tap turns the lock-on on or off (the finger is no control's then: its moves and lift go
        // nowhere)
        if (padlock_hit(s, x, y)) {
            lock_set(s, !s.lock, "the padlock");
            tap_due();
            return true;
        }
        // the sprint lock's circle, lit while the run goes on by itself: a tap there ends it, as touching the stick does
        // (the finger is no control's then)
        if (s.run_lock && !s.stick_on && on_lock_circle(s, x, y)) {
            run_end(s, "the lock circle touched");
            tap_due();
            return true;
        }
        int part = 0, c = hit(s.layout, x, y, s.w, s.h, &part, 1.22f, false, s.menu);
        if ((c == C_R || c == C_DODGE || c == C_CORNER || c == C_RSECOND || c == C_AMMO_UP || c == C_AMMO_DOWN) && item.in(x, y))
            c = -1;  // (narrow screens: their default places, and B's in layouts saved before, reach the item bar)
        int arrow = 0;
        if (c < 0 && item.ok && !s.menu && config_live_touch_show(TB_ITEM_ARROWS))
            for (int d = -1; d <= 1; d += 2) {
                Geo g = place(s.layout, d < 0 ? C_ITEML : C_ITEMR, s.w, s.h);
                if (hypotf(x - g.c.x, y - g.c.y) < g.r * 1.2f) arrow = d;
            }
        if (c >= 0) {
            if (c == C_RAIM) s.bow_leave = 0;
            f->kind = F_BUTTON, f->ctl = c, f->part = part;
            f->btn = is_ranged(c) ? ranged_buttons(c, s.drawn, s.menu) : c == C_ZL ? target_buttons() : c == C_L ? camera_buttons() : 0;
            if ((c == C_AMMO_UP || c == C_AMMO_DOWN) && !s.menu)
                ammo_step(s, c == C_AMMO_UP ? 1 : -1), f->repeat_ns = now + 550 * MS;  // (held: more steps, touch_input)
            if (!of_mpad(c)) tap_due();  // (the menu pad's group: as it presses, touch_input)
        } else if (s.menu && item.in(x, y)) {
            f->kind = F_CAMERA;  // (the item bar in a menu: a drag on it moves the cursor, as anywhere else)
        } else if (arrow) {
            f->kind = F_MENU;  // (nothing more of it until it lifts)
            item_step(s, arrow, now);
        } else if (item.in(x, y)) {
            f->kind = F_ITEM;
        } else if (arts.in(x, y)) {
            // (the row under the finger, or the nearest one; performed on the tap, when it lifts: a drag turns the camera)
            f->kind = F_ARTS, f->part = art_row_at(z, s.w, s.h, y);
        } else if (!s.stick_on && in_mode(C_STICK, s.menu) && in_stick_zone(s, x, y, item)) {
            f->kind = F_STICK;
            Geo g = place(s.layout, C_STICK, s.w, s.h);
            run_end(s, "the stick touched again");  // (a run going on by itself stops: this thumb drives now)
            s.stick_on = true, s.bx = g.c.x, s.by = g.c.y;
            stick_to(s, x, y);
            // (a new thumb may lock from wherever it landed, slid up into the circle again - not one that landed in it,
            // which a stick placed high has near)
            Geo lg = place(s.layout, C_RUNLOCK, s.w, s.h);
            if (hypotf(x - lg.c.x, y - lg.c.y) >= LOCK_GRAB / LOCK_R * lg.r) s.run_armed = true;
        } else {
            f->kind = F_CAMERA;
        }
        recompute_held(s);
        static int dbg = -1;  // (MHGU_TOUCH_LOG=1: what each finger landed on)
        if (dbg < 0) dbg = getenv("MHGU_TOUCH_LOG") != nullptr;
        if (dbg)
            rt_logf("[touch] finger %.0f,%.0f of %.0fx%.0f: kind %d control %d part %d, buttons %llx; zones %d%d%d%s\n",
                    x, y, s.w, s.h, f->kind, f->ctl, f->part, (unsigned long long)s.held, item.ok, arts.ok,
                    zone_rect(z, HP_TARGET, s.w, s.h).ok,
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
        // a drag moves the cursor, following the finger, a step every so far (with a model shown, the smithy's or the item
        // box's, it turns the camera as anywhere else: the right stick, which turns the model there)
        if (f->kind == F_CAMERA && s.menu && !s.preview) {
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
        } else if (f->kind == F_BUTTON && f->ctl == C_RAIM && f->moved && !f->done && !s.menu) {
            // (a slide on the aim: across as a drag of the view; up and down the aim's own pitch, turned as the view's
            // drag would turn it - the game's quick aim took the stick's up and down only now and then: the owner's
            // Galaxy, "mantener R con distancia no apunta en vertical"; hle/mousecam.c touch_aim_pitch)
            s.cam_dx += dx, s.cam_ns = now;
            g_aim_pitch_deg = g_aim_pitch_deg.load() - dy / s.h * CAMERA_COUNTS * .08f * config_live_touch_camera();
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
            while (!f->done && f->swipe >= step) item_step(s, 1, now), f->swipe -= step;
            while (!f->done && f->swipe <= -step) item_step(s, -1, now), f->swipe += step;
        } else if (f->kind == F_BUTTON && is_dpad(f->ctl)) {  // the d-pad: the direction the thumb is in now
            int part = dpad_part(place(s.layout, f->ctl, s.w, s.h), x, y, f->part);
            if (part != f->part) f->part = part, recompute_held(s), tap_due();
        } else if (f->kind == F_BUTTON && f->ctl == C_MPAD) {  // the menu pad: the same (pressed by touch_input, as a
            f->part = mpad_part(place(s.layout, C_MPAD, s.w, s.h), x, y, f->part);  // press may start: press_ok)
        } else if (f->kind == F_BUTTON && (f->ctl == C_ATTACK || f->ctl == C_CORNER)) {  // sliding along it: another part
            int part = 0, c = hit(s.layout, x, y, s.w, s.h, &part, 1.22f, false, s.menu);
            if (c == f->ctl && part != f->part) {
                // (the corner's guard onto its attacks: R kept, [Touch] GuardSlide; back onto the guard, the guard alone)
                if (f->ctl == C_CORNER && (f->part == 3 || f->keep) && part != 3 && guard_slide(s)) f->keep = B(BTN_R);
                if (part == 3) f->keep = 0;
                f->part = part, f->on_bar = false, recompute_held(s), tap_due();
            } else if (c == C_R && f->keep) {  // (a guard kept, back onto Guard: Guard again)
                f->ctl = C_R, f->part = 0, f->keep = 0, recompute_held(s), tap_due();
            }
        } else if (f->kind == F_BUTTON && (f->ctl == C_HEAVY || f->ctl == C_LIGHT)) {  // from one attack to the other
            int part = 0, c = hit(s.layout, x, y, s.w, s.h, &part, 1.22f, false, s.menu);
            if ((c == C_HEAVY || c == C_LIGHT) && (c != f->ctl || part != f->part)) f->ctl = c, f->part = part, recompute_held(s), tap_due();
            else if (c == C_R && f->keep) f->ctl = C_R, f->part = 0, f->keep = 0, recompute_held(s), tap_due();
        } else if (f->kind == F_BUTTON && (f->ctl == C_DODGE || f->ctl == C_R || f->ctl == C_RUN)) {  // a thumb sliding onto
            int part = 0, c = hit(s.layout, x, y, s.w, s.h, &part, 1.22f, false, s.menu);           // the attacks
            if (c == C_ATTACK || c == C_CORNER || c == C_HEAVY || c == C_LIGHT) {
                // (from Guard with the weapon out and [Touch] GuardSlide: R kept, R + X, R + A; from Run: the attack
                // alone, a draw attack, the run's R let go; from Dodge: the attack)
                f->keep = f->ctl == C_R && guard_slide(s) ? B(BTN_R) : 0;
                f->ctl = c, f->part = part, f->on_bar = false, recompute_held(s);
                if (f->keep) tap_due(), guard_log("R with the attacks");
            }
        }
        // (a thumb on Guard - the corner's too - slid onto the item bar with GuardSlide: R + Y while it is there)
        if (f->kind == F_BUTTON && !f->done && (f->ctl == C_R || (f->ctl == C_CORNER && f->part == 3))) {
            bool bar = guard_slide(s) && zone_rect(zones(), HP_ITEMS, s.w, s.h).in(x, y);
            if (bar != f->on_bar) f->on_bar = bar, recompute_held(s), tap_due(), guard_log(bar ? "R + Y on the item bar" : "off the item bar");
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
        while (!f->done && f->swipe >= step * .6f) item_step(s, 1, now), f->swipe -= step;
        while (!f->done && f->swipe <= -step * .6f) item_step(s, -1, now), f->swipe += step;
    }
    bool tap = !f->moved && now - f->t0 < 300 * MS && e->type == SDL_EVENT_FINGER_UP;
    bool cancel = e->type == SDL_EVENT_FINGER_CANCELED;
    // The aim with a gun out (its R): a tap, let go within AIM_TAP and not slid, is the game's tap of R, the scope; held
    // longer or slid it is the quick aim, held while the thumb is (its slide turns the aim as a drag of the view does),
    // and its R stays down until the game can't take it for a tap (aim_hold_ns): the aim comes up and ends, no scope.
    // A bow's: its button enters the aiming mode, which stays until it is pressed again (0010:012, the game's Bow
    // Controls say which button: f->btn): a tap enters it; held or slid, the thumb aims and letting go leaves it
    uint64_t held_ns = now - f->t0;
    bool aim_hold = f->kind == F_BUTTON && f->ctl == C_RAIM && !s.menu && !cancel && s.drawn == 1 && (f->moved || held_ns >= AIM_TAP);
    if (aim_hold && g_ranged.load() == WEAPON_BOWGUN && (f->btn & B(BTN_R)) && held_ns < aim_hold_ns())
        pulse(s, B(BTN_R), aim_hold_ns() - held_ns);
    if (aim_hold && g_ranged.load() == WEAPON_BOW && f->btn) s.bow_leave = f->btn, s.bow_leave_until = now + 1500 * MS;
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
    if (f->kind == F_STICK) {
        s.stick_on = false;
        // The thumb lifted on the lock circle, armed (it came from inside the rim or landed elsewhere, and a run can
        // lock): the run locks and goes on by itself, the stick held full forward. Not when the system took the finger,
        // nor for a thumb that only passed over the circle
        bool over = s.run_can && s.run_armed && on_lock_circle(s, x, y);  // (where it lifted)
        s.run_over = false;
        if (!cancel && over && s.run_can && !s.run_lock) {
            s.run_lock = s.run_free = true;
            tap_due();
            static int dbg = -1;
            if (dbg < 0) dbg = getenv("MHGU_TOUCH_LOG") != nullptr;
            if (dbg) rt_logf("[touch] run locked (the thumb lifted on the lock circle)\n");
        }
    }
    if (f->kind == F_EDIT) s.pinch = false;
    if (f->kind == F_ITEM && s.bar_open && f->id == s.bar_finger) s.bar_open = false;  // (closed: no item used)
    if (f->kind == F_ITEM && tap && !f->done && fabsf(f->swipe) < .02f * s.h && fabsf(f->swipe_y) < .02f * s.h)
        pulse(s, B(BTN_Y), 80 * MS), tap_due();  // a tap: the item (Y)
    if (f->kind == F_ARTS && tap && f->part >= 0) art_tap(s, f->part, now);  // a tap on a Hunter Art's panel: that art
    // the settings button: a tap opens the port's menu (a long press opened the editor: touch_input)
    if (f->kind == F_BUTTON && f->ctl == C_SETTINGS && !f->done && e->type == SDL_EVENT_FINGER_UP) overlay_toggle_settings();
    if (s.menu) s.tap_ns = 0;  // (in the game's menus L turns pages: no double tap there)
    else if (f->kind == F_CAMERA && tap) {  // a double tap on the camera's part centres it (L)
        if (now - s.tap_ns < 350 * MS && hypotf(x - s.tap_x, y - s.tap_y) < .08f * s.h) pulse(s, camera_buttons(), 60 * MS), s.tap_ns = 0;
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
    bool game = !trad() && game_menu(hunter) && !weapon_aiming(hunter) && !ranged_aim(hunter);
    static int dbg = -1;  // (MHGU_TOUCH_LOG: the menu, the hunter played or not, the menu pad's presses, as they change)
    if (dbg < 0) dbg = getenv("MHGU_TOUCH_LOG") != nullptr;
    // (a bow's aim held and let go: out of the aiming mode once the hunter is back in it - a shot taken meanwhile ends
    // there -; no more once it has left it by itself, a dodge or a sheathe)
    if (s.bow_leave) {
        unsigned act = weapon_action(hunter);
        if (now >= s.bow_leave_until || act >> 16 != 0x10 || s.drawn != 1) {
            s.bow_leave = 0;
        } else if (act == (0x10u << 16 | 12)) {
            pulse(s, s.bow_leave, MIN_PRESS);
            if (dbg) rt_logf("[touch] the bow's aim let go: out of the aiming mode\n");
            s.bow_leave = 0;
        }
    }
    // (a menu opened in a town, the item box, a shop, the smithy, its windows drawn: no Hunter Art comes of the d-pad
    // there, so what the menu pad presses passes whether a cursor of the game's ran or not - the item box's sorting moves
    // none of those, and the pad pressed nothing there, #174)
    bool town_menu = s.menu && g_menu_town.load() && g_frames_since_menu.load() <= 2;
    bool play = playable(), ok = press_ok() || town_menu;
    menu_set_free(!play || town_menu);  // (the game's thread lets the menu pad's buttons through then too: hle/input.c)
    {
        static int was = -1;
        if (dbg && (int)play != was) {
            unsigned act = weapon_action(hunter);
            rt_logf("[touch] hunter %s (menu %d; action %04x:%03u, hunt camera %d)\n", play ? "played" : "not played",
                    (int)game, act >> 16, act & 0xffff, (int)mousecam_active());
        }
        was = play;
        static uint64_t said_ns;  // (a cursor of a menu's running in the field, no menu window: there should be none)
        if (dbg && play && !game && menu_cursor_age() == 0 && now - said_ns > 2000 * MS)
            rt_logf("[touch] a menu's cursor runs with no menu window open\n"), said_ns = now;
    }
    // The hunter: found now or a moment ago (the weapon in hand and whether it is out: the ranged layout and what its
    // buttons press). A moment without one changed the layout under the fingers, and the fire button pressed X - with the
    // gun out a reload (#89)
    if (hunter) {
        int d = weapon_drawn();
        s.hunter_ns = now, s.weapon_known = weapon_ranged(hunter);
        if (d >= 0) s.drawn_known = d;
    }
    bool hunter_lately = hunter || now - s.hunter_ns < 1500 * MS;
    s.weapon = hunter_lately ? s.weapon_known : 0;
    int ranged = !trad() && config_live_touch_show(TB_RANGED) ? s.weapon : 0;
    s.drawn = hunter_lately ? s.drawn_known : -1;
    s.prowler = weapon_prowler(hunter);
    g_bow_type = weapon_bow_type();
    if (ranged != g_ranged.load()) {  // (a weapon in hand or gone: the other layout, under the same fingers)
        g_ranged = ranged;
        for (Finger &f : s.f)
            if (f.kind == F_BUTTON && !in_mode(f.ctl, s.menu)) f.done = true;
        recompute_held(s);
    }
    // (a window taken for a menu while a quest's hunter is played - an aim's or the scope's, a message - is no menu until
    // it has lasted MENU_SETTLE: for a frame the layout went to the menu's and back, and the lock-on's shot let go of its
    // R, the scope; the owner's Galaxy in the Arena, 2026-11-10)
    const uint64_t MENU_SETTLE = 200 * MS;
    if (game && !s.menu && hunter) {
        if (!s.menu_seen_ns) s.menu_seen_ns = now;
        if (now - s.menu_seen_ns < MENU_SETTLE) game = false;
    } else if (!game) {
        s.menu_seen_ns = 0;
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
    {  // a model shown in a town's menu (the smithy's weapon, the item box's hunter): the game turns it with the sticks,
       // so the stick and the drags are the game's there, as a controller's, and the menu pad moves the cursor (#172)
        bool preview = s.menu && g_menu_town.load() && g_frames_since_preview.load() <= 3;
        if (preview != s.preview && dbg) rt_logf("[touch] a model in the menu: %s\n", preview ? "the sticks turn it" : "gone");
        s.preview = preview;
    }
    // The menu pad's fingers: what each asks for is pressed once a press may start (press_ok), and what one presses
    // stays down while the menu takes input and the finger is on it (a long update neither lets it go nor presses it
    // again: no step too many, no repeat begun again); the game repeats a direction held. It lets go once no menu's
    // cursor has run for MPAD_LET_GO updates, the hunter played: a thumb kept on it as the menu closes is no d-pad held
    // in the field (a Hunter Art). A click of the vibrator as it presses something new, not as it is touched
    s.pad_held = 0;
    uint32_t age = menu_cursor_age();
    bool gone = play && !town_menu && age >= MPAD_LET_GO;  // (the hunter not played, a town: no Art can come of it)
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
    // a long press on the settings button opens the editor; on L, it turns the lock-on on or off (L lets go: the press
    // was the lock-on's; the game had its L as it came)
    for (Finger &f : s.f) {
        if (f.kind != F_BUTTON || f.done) continue;
        if (f.ctl == C_SETTINGS && now - f.t0 > 700 * MS && !menu) {
            f.done = true;
            edit_begin(s);
            break;
        }
        if (f.ctl == C_L && !s.menu && !menu && config_live_lock_on() && now - f.t0 > LOCK_HOLD) {
            f.done = true;
            lock_set(s, !s.lock, "L held");
            tap_due();
            recompute_held(s);
            break;
        }
    }
    // an ammo arrow held: another step every so often (a gun with many kinds of ammo, #89)
    for (Finger &f : s.f)
        if (f.kind == F_BUTTON && !f.done && (f.ctl == C_AMMO_UP || f.ctl == C_AMMO_DOWN) && !s.menu && f.repeat_ns &&
            now >= f.repeat_ns)
            ammo_step(s, f.ctl == C_AMMO_UP ? 1 : -1), f.repeat_ns = now + 320 * MS, tap_due();
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
    // A long press on the item bar holds it open, its neighbouring items shown as while L is held, with nothing pressed
    // (L would turn the camera): a swipe meanwhile turns it, lifting closes it and uses no item (#109, as the MH4U port's)
    for (Finger &f : s.f)
        if (f.kind == F_ITEM && !f.done && !f.moved && !s.bar_open && now - f.t0 > BAR_HOLD && !s.menu) {
            s.bar_open = true, s.bar_finger = f.id;
            tap_due();
            if (dbg) rt_logf("[touch] item bar held open\n");
        }
    if (s.bar_open && (menu || s.menu || !visible)) s.bar_open = false;  // (a menu over it: closed)
    itembar_hold(s.bar_open);
    // The stick's locked run ends once a run can't go on: StickRun turned off, the port's menu or one of the game's open,
    // the controls hidden or in the editor, the weapon out, the hunter not played (a cutscene, a cart, the quest's end);
    // and as the stick or the lock circle is touched again (touch_event)
    bool stick_run = trad() ? config_live_touch_auto_run() != 0  // (Traditional: the thumb past the rim runs only with [Touch] AutoRun; else the R button)
                            : config_live_touch_show(TB_STICK_RUN) != 0;
    s.run_can = stick_run && s.drawn != 1 && !s.menu && play && visible && !s.edit && !menu;
    if (s.run_lock && !s.run_can)
        run_end(s, !stick_run ? "StickRun off" : menu ? "the settings menu" : !visible || s.edit ? "the controls hidden or edited"
                 : s.menu ? "a menu of the game's" : s.drawn == 1 ? "the weapon out" : "the hunter not played");
    s.run = false;
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
    // (with a model shown there, never: the game turns it with the stick, s.preview)
    if (s.menu && g_menu_town.load() && !s.stick_cursor && !s.preview) {
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
    if (s.menu && (!play || s.stick_cursor) && s.stick_menu_ok && !s.preview) {
        if (stick_dir && stick_dir != s.stick_part) tap_due();
        s.stick_part = stick_dir;
        *buttons |= dpad_buttons(stick_dir);
        return;
    }
    s.stick_part = 0;
    bool moving = false;
    if (s.run_lock && s.h > 0) {  // (the sprint lock, the thumb out past the rim or lifted: the stick held full forward,
        *ly += 32767;              // where the camera looks: the right thumb steers)
        moving = true;
    } else if (s.stick_on && s.h > 0) {
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
    // Running, R held: the dash with the weapon sheathed (never with it out: R is some weapons' guard, and not in a menu:
    // R turns its pages). [Touch] StickRun: the thumb past the rim, as before (the rim), and the sprint lock: the thumb
    // lifted on the lock circle above the stick, come from inside the rim while a lock can start, locks the run with a
    // click; it goes on forward (above) until run_end. StickRun Off: the Run button, while held. No run's R with X pressed
    // or just after: X draws the weapon, and R + X + A is another move (the Great Sword's guard from the sheath came out of
    // a run and a draw attack: #151)
    int drawn = s.drawn;
    static int drawn_was = -2;
    if (drawn != drawn_was) {
        if (dbg) rt_logf("[touch] weapon %s\n", drawn == 1 ? "out" : drawn == 0 ? "sheathed" : "(not known)");
        drawn_was = drawn;
    }
    if (s.stick_on && s.h > 0) {
        float r = DEFS[C_STICK].r * s.layout.scale[C_STICK] * s.h, d = hypotf(s.fx - s.bx, s.fy - s.by);
        // (the rim, as before: well past it, so that a thumb at the edge in a fight still walks; r is the stick's size
        // in the layout: a bigger stick, farther)
        s.rim = stick_run && moving && drawn != 1 && !s.menu && d > r * (s.rim ? RUN_OFF : RUN_ON);
        // (the thumb on the circle, armed: lit, and lifted there it locks, as the finger lifts; slid off it, nothing)
        bool in_lock = on_lock_circle(s, s.fx, s.fy);
        if (!s.run_can && in_lock) s.run_armed = false;  // (there while it couldn't lock - the weapon out as it was
                                                         // sheathed: not until it has come back inside the rim)
        bool over = s.run_can && s.run_armed && in_lock;
        if (dbg && over != s.run_over) rt_logf("[touch] run lock %s\n", over ? "armed (the thumb on its circle)" : "no longer armed");
        s.run_over = over;
    } else {
        s.rim = s.run_over = false;
    }
    bool run_held = false;  // (the Run button)
    for (const Finger &f : s.f) run_held |= f.kind == F_BUTTON && !f.done && f.ctl == C_RUN;
    bool want = s.run_lock || s.rim || (run_held && !stick_run && drawn != 1 && !s.menu);
    if (*buttons & B(BTN_X)) s.run_x_until = now + RUN_X_WAIT;
    s.run = want && now >= s.run_x_until;
    if (s.run) *buttons |= B(BTN_R);
    static bool said_x, said_run;  // (the log: once a change, what held R said as it lets go too)
    static const char *said_why = "";
    if (dbg && want && !s.run && !said_x) rt_logf("[touch] run: no R while X draws the weapon\n");
    said_x = want && !s.run;
    if (s.run) said_why = s.run_lock ? "the lock" : s.rim ? "the stick's rim" : "the Run button";
    if (dbg && s.run != said_run) rt_logf("[touch] run: R %s (%s)\n", s.run ? "held" : "let go", said_why);
    said_run = s.run;
}

// (MHGU_PERF_INPUT_WAKE) What touch_input does by the clock, between events: a finger down (the long presses, the
// stick's run, an ammo arrow held, the menu pad), a timed press (a tap's, an art's, the camera's double tap), the run
// held back after X, the locked run, the lock-on (its follow comes back LOCK_AFTER_TURN after a turn), a locked shot,
// a Hunter Art on the right stick, the item bar held open, the editor. While any is on the window loop polls every 3 ms
// as always
extern "C" bool touch_busy(void) {
    std::lock_guard<std::mutex> l(g_lock);
    const State &s = g_s;
    uint64_t now = SDL_GetTicksNS();
    for (const Finger &f : s.f)
        if (f.kind != F_NONE) return true;
    for (const Pulse &p : s.pulse)
        if (p.until > now) return true;
    if (s.stick_on || s.run || s.run_lock || s.run_free || s.lock || s.bar_open || s.edit || s.edit_request || s.pinch)
        return true;
    if (s.held || s.tapped || s.pad_held || s.cam_dx != 0 || s.cam_dy != 0 || g_tap_due) return true;
    if (now < s.ammo_hold || now < s.ammo_ready || now < s.run_x_until || now < s.lit_until || now < s.rs_until ||
        now < s.item_flash || now < s.art_flash || now - s.tap_ns < 350 * MS)
        return true;
    if (s.combo_t0 || s.assist_wait || now < s.assist_until || now < s.assist_fire || s.keep_r || now < s.replay_until)
        return true;
#ifndef __ANDROID__
    if (s.hud_edit || s.hud_request || g_hud_hold) return true;
#endif
    return false;
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

extern "C" void touch_game_held(bool on) { g_held_ns = on ? SDL_GetTicksNS() : 0; }

// (overlay.cpp, the game held for the editor: a part of the HUD dragged or sized since the last step - the item bar
// above all, which the touch controls go by -, the game takes a step at most every EDIT_STEP so that it draws it where
// it goes; the picture held stayed where the part was, and it looked as if it no longer moved: the owner's Galaxy)
// (hle/mousecam.c, each update of the hunter: the degrees the aim's slide turns the aim up by, taken)
extern "C" float touch_aim_pitch(void) { return g_aim_pitch_deg.exchange(0.0f); }

extern "C" bool touch_editor_step(void) {
    const uint64_t EDIT_STEP = 100 * MS, now = SDL_GetTicksNS();
    if (g_part_moved_ns.load() <= g_step_ns.load() || now - g_step_ns.load() < EDIT_STEP) return false;
    g_step_ns = now;
    return true;
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
    lock_set(g_s, !g_s.lock, "the right stick's click");
}

// The lock-on: the hunt camera turns to its monster each update (hle/mousecam.c), while the player is not turning it (a
// finger, the right stick, the mouse: cam_input) and for LOCK_AFTER_TURN after, nor pressing L (the game's own turn:
// behind the hunter, or to the Target Cam's monster; the game's turns are seen there too). Off by itself once the hunt
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
        if (buttons & ~was & (B(BTN_PLUS) | B(BTN_MINUS))) g_menu_press_ns = now;  // (a bow or a gun out: game_menu)
        was = buttons;
    }
    bool hunt = mousecam_active() || s.weapon;  // (a gun aimed has a camera of its own)
    if (hunt) s.hunt_ns = now;
    if (s.lock && now - s.hunt_ns > 10000 * MS) lock_set(s, false, "no hunt camera");
    static bool l_was;
    bool l_now = (buttons & camera_buttons()) != 0;  // (the game's camera centring: L, or ZL with L and ZL swapped)
    if (cam_input || (l_now && !l_was)) s.cam_ns = now;
    l_was = l_now;
    if (s.lock && !config_live_lock_on()) lock_set(s, false, "the settings");
    bool on = s.lock && !s.edit && hunt && !overlay_menu_open() && !s.menu;
    g_lock_shots = on;
    g_lock_follow = on && now - s.cam_ns > LOCK_AFTER_TURN;
    g_lock_mark = s.lock && !s.edit && !s.menu;
    g_lock_engaged = s.lock;
    return 0;  // (no L presses: the hunt camera follows the monster itself, hle/mousecam.c)
}

// With the lock-on on, a bowgun's shot from a normal stance aims first: only an aimed shot takes the monster's height
// (hle/mousecam.c). A press of A in the stance (weapon_stance: the gun out, standing or walking, in no move of its own,
// not aiming) is held back for the game's own two-button window (hle/combowin.c: 67 ms, or two updates with
// ComboInputWindow Strict): X joining it is crouching fire, B or Y another move, and they pass together as pressed. Then R
// aims (held: the quick aim) and A fires once the aim is up; a press while the hunter is still in that aim fires at once,
// and after a shot (the game leaves the aim then) the next press from the stance aims anew.
// Everything else is the player's and passes untouched: their own R (the quick aim, the scope), crouching fire, a reload, a
// dodge, a sheathe, the styles' moves (Valor's slide, its sheathing stance), and any press out of the stance (#132, #177),
// with L, ZL or ZR (the item bar, Control Type 3's Hunter Arts), with no monster to aim at (mousecam_lock_aimable) or
// while the camera is turned by hand (the follow paused: no shot is turned then).
// On every input's fire button (the touch controls', a key, a mouse button, a controller's: host_input.c gives them all,
// once each update). The aim is known by the hunter's actions (weapon_aiming) or by the hunt camera's handler stopping (a
// gun aimed has a camera of its own: so any style's aim is seen); a moment of R without either fires anyway: R is the aim
extern "C" uint64_t combowin_ns(void);         // (hle/combowin.c: how long the game waits for a second button now)
extern "C" uint64_t combowin_update_ns(void);  // (...and one of its updates)
const uint64_t SHOT_AIM = 1300 * MS;  // (a locked shot's aim stays this long after its last press)
const uint64_t SHOT_A = 110 * MS;     // (its A, pressed for the game this long)
// The game's R: let go within about 250 ms of its own time it is a tap, the scope; held longer, the quick aim, whose
// camera then comes up (the hunt camera's stops). Meanwhile (0001:045) the game takes no A, B, X or Y. Measured on the
// RG406V with a heavy bowgun: at 60 FPS 14 frames of R the scope, 15 the aim; at 30 FPS 8 frames the scope, 9 the aim
uint64_t aim_hold_ns(void) { return 250 * MS + 3 * combowin_update_ns(); }  // (R held this long: never the scope)
// (no aim seen this long after its R began: it fires anyway. The aim was seen after 375 ms at 60 FPS)
uint64_t shot_late_ns(void) { return std::max<uint64_t>(600 * MS, 250 * MS + 10 * combowin_update_ns()); }
std::atomic<bool> g_assist_waiting, g_player_aims;  // (touch_lock_assist_waiting, touch_lock_player_aims)
std::atomic<uint64_t> g_still_until;                // (touch_lock_still)
// Standing still (still: no stick from any input), a press of A alone - not with B, X, Y or a shoulder, not in crouching
// fire (0010:026, 028, 029, 033; 053 its dodge) - is the locked shot as 0.1.8 had it, the owner's call after trying the
// stance's on a Galaxy (2026-11-10: standing, it didn't shoot at the monster as before): no stance, no monster to wait for,
// and the hunter turned to the monster from the press to the shot (hle/mousecam.c, touch_lock_still). In an aim's actions
// already (weapon_aim_action, 0.1.8's weapon_aiming: 0001:046 stays after an aim, for minutes) A passes at once, as it did
// then. Else R at once (let go through the game's two-button window first if the help's R was still down: a new press for
// the game), A held back until the aim is up, then A for SHOT_A; R stays SHOT_AIM after the press. Unlike 0.1.8 the aim
// counts only once it is past going in (0001:045): there the game drops A, and 0.1.8's first shot from the stance never
// left (measured on a Moto g06 with a heavy bowgun, 2026-11-10). The player's own R, and B, X or Y joining, end it as
// they end the stance's
bool crouch_fire(unsigned act) {
    unsigned st = act >> 16, no = act & 0xffff;
    return st == 0x10 && (no == 26 || no == 28 || no == 29 || no == 33 || no == 53);
}
extern "C" uint64_t touch_lock_shot(uint64_t buttons, bool still) {
    std::lock_guard<std::mutex> l(g_lock);
    State &s = g_s;
    uint64_t now = SDL_GetTicksNS();
    const uint64_t in = buttons;  // (the player's own, from every input)
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("MHGU_TOUCH_LOG") != nullptr;
    bool player_aims = false;
    if (s.weapon && s.drawn == 1 && (buttons & (B(BTN_A) | B(BTN_R) | B(BTN_X)))) g_ranged_press_ns = plat_now_ns();
    if (s.weapon == WEAPON_BOWGUN && s.drawn == 1 && g_lock_shots.load()) {
        uint32_t hunter = weapon_hunter();
        unsigned act = weapon_action(hunter);
        bool fire = (buttons & B(BTN_A)) != 0, press = fire && !s.fired;
        s.fired = fire;
        player_aims = (buttons & B(BTN_R)) != 0;  // (the player's own: nothing of the help's is in yet)
        uint64_t others = buttons & (B(BTN_B) | B(BTN_X) | B(BTN_Y)), joined = others & ~s.shot_others;
        s.shot_others = others;
        if (joined) s.others_ns = now;
        // (L, ZL or ZR with it: L + A turns the item bar, ZL + A is a Hunter Art with Control Type 3 - L + A with the L / R
        // Button Controls swapped -: held at the press or joining it in the window, no locked shot)
        uint64_t mods = buttons & (B(BTN_L) | B(BTN_ZL) | B(BTN_ZR));
        uint64_t window = combowin_ns();
        bool helping = s.combo_t0 || s.assist_wait || now < s.assist_until;
        if (player_aims && helping) {  // (the player's R: the aim is theirs, and the press held back fires with it)
            if (s.combo_t0 || s.assist_wait) s.assist_fire = now + SHOT_A;
            s.combo_t0 = s.assist_until = s.keep_r = 0, s.assist_wait = false;
            if (dbg) rt_logf("[touch] a locked shot ended: the player aims\n");
        } else if (s.combo_t0 && mods) {  // (a shoulder joined the press in the window: it passes with it, as pressed)
            if (!fire) s.assist_fire = now + SHOT_A;
            s.combo_t0 = 0, s.assist_off = true;
            if (dbg) rt_logf("[touch] a locked shot ended by L, ZL or ZR, its A passed with it\n");
        } else if (joined && helping) {
            // A dodge, a reload, crouching fire, a sheathe: the help ends, and none begins until A is let go. In the game's
            // window, before any R, the press held back passes with them as pressed (X + A crouching fire; a tap already let
            // go gets its A back for X). Once its R began, R stays down until it can't be a tap of R (aim_hold_ns: the
            // scope), and B, X or Y wait meanwhile (the game takes none while its aim starts) and reach it then as a press
            // of their own, a tap let go meanwhile too; A not with them: R + A + B is a Hunter Art with Control Type 2 (#89)
            uint64_t r_min = s.assist_t0 + aim_hold_ns();
            s.assist_fire = s.combo_t0 && !fire && (joined & B(BTN_X)) ? now + SHOT_A : 0;
            if (!s.combo_t0 && now < s.assist_until && now < r_min) s.keep_r = r_min;
            if (dbg)
                rt_logf("[touch] a locked shot ended by %s (action %04x:%03u)%s\n", joined & B(BTN_X) ? "X" : joined & B(BTN_B) ? "B" : "Y",
                        act >> 16, act & 0xffff, s.combo_t0 ? ", its A passed with it" : now < s.keep_r ? ", its R kept a moment" : "");
            s.combo_t0 = s.assist_until = 0, s.assist_wait = false, s.assist_off = true;
        }
        if (now < s.keep_r) s.keep_buttons |= joined;  // (B, X, Y pressed while the help's R finishes: they wait)
        if (!fire && !s.combo_t0) s.assist_off = false;
        if (press && !player_aims && !s.assist_off) {
            if (s.assist_wait) {
                // (another press while it waits for the aim: it fires with the first; the wait isn't begun again)
            } else if (now < s.assist_until && weapon_aiming(hunter)) {
                s.assist_until = now + SHOT_AIM;  // (the hunter in its aim: this one passes at once, and the aim stays)
                if (now < s.still_until) s.still_until = now + SHOT_AIM;
            } else if (!s.combo_t0 && still && !others && !mods && !crouch_fire(act)) {
                s.still_until = now + SHOT_AIM;
                bool in_aim = weapon_aim_action(hunter) && act != (1u << 16 | 45);
                if (in_aim) {
                } else if (now < s.assist_until) {  // (its R let go through the window first: a new press of R for the game)
                    s.combo_t0 = now, s.assist_until = 0, s.still_wait = true;
                } else {
                    s.assist_wait = s.still_wait = true, s.assist_t0 = now, s.assist_until = now + SHOT_AIM;
                }
                if (dbg) rt_logf("[touch] a locked shot standing still (action %04x:%03u)%s\n", act >> 16, act & 0xffff,
                                 in_aim ? ": in an aim's actions, it fires now" : "");
            } else if (!s.combo_t0) {
                bool stance = weapon_stance(hunter), alone = !others && !mods && now - s.others_ns >= window && !s.keep_r &&
                                                             now >= s.replay_until;
                // (and a monster to aim at, the camera not turned by hand just now - the mouse, a finger, the right stick, L:
                // hle/mousecam.c turns no shot then -: else the help would only hold the shot back)
                bool target = mousecam_lock_aimable() && g_lock_follow.load();
                if (stance && alone && target) {
                    // (after a shot the game leaves the aim, R held or not: the next press from the stance aims anew, its
                    // R let go through the window first, so the game takes it as a new press of R; held that long, its
                    // letting go is no tap of R)
                    s.combo_t0 = now, s.assist_until = 0;
                    if (dbg) rt_logf("[touch] a locked shot from the stance (action %04x:%03u)\n", act >> 16, act & 0xffff);
                } else if (dbg) {
                    rt_logf("[touch] fire passed: %s (action %04x:%03u)\n",
                            !alone ? "another button with it" : weapon_aiming(hunter) ? "aiming" : weapon_moving(hunter) ? "a weapon move"
                            : !mousecam_hunt_camera() ? "no hunt camera (an aim, crouching fire)"
                            : !stance ? "not in the stance" : !g_lock_follow.load() ? "the camera turned by hand" : "no monster to aim at",
                            act >> 16, act & 0xffff);
                }
            }
        }
        // the game's two-button window over, nothing joined: the aim begins, if the hunter is still in the stance
        if (s.combo_t0 && now - s.combo_t0 >= window) {
            if (weapon_stance(hunter) || s.still_wait) {
                s.assist_wait = true, s.assist_t0 = now, s.assist_until = now + SHOT_AIM;
            } else {
                if (!fire) s.assist_fire = now + SHOT_A;  // (the press passes as it came)
                s.assist_off = fire;
            }
            s.combo_t0 = 0;
        }
        if (s.assist_wait) {
            // The aim seen: the hunter's aiming actions, or the hunt camera stopped since R began (the gun's own camera: a
            // style's aim too), but not while it is still going in (0001:045, about 22 updates at 60 FPS): the game drops an
            // A there early on, and the camera stops some updates before the aim's own action (0001:046) comes. Measured on
            // the RG406V with a heavy bowgun, 2026-11-10: the press, its window and the aim, about 440 ms to the shot
            bool going_in = act == (1u << 16 | 45);
            bool aimed = !going_in && (weapon_aiming(hunter) || (now > s.assist_t0 && !mousecam_hunt_camera()));
            bool late = now - s.assist_t0 >= shot_late_ns();
            if (s.still_wait) aimed = !going_in && weapon_aim_action(hunter);  // (0.1.8's aim, not going in: A is dropped there)
            if (aimed || late) {
                s.assist_wait = false, s.assist_fire = now + SHOT_A;
                if (dbg) rt_logf("[touch] a locked shot: aim %s after %llu ms (action %04x:%03u)\n", aimed ? "up" : "not seen",
                                 (unsigned long long)((now - s.assist_t0) / MS), act >> 16, act & 0xffff);
                if (!aimed) {
                    static unsigned said[8];
                    unsigned k = 0;
                    while (k < 8 && said[k] && said[k] != act) k++;
                    if (k < 8 && !said[k]) {
                        said[k] = act;
                        rt_logf("[touch] a locked shot: no aim seen (action %04x:%03u), fired with R held\n",
                                act >> 16, act & 0xffff);
                    }
                }
            }
        }
        if (!s.assist_wait && !s.combo_t0) s.still_wait = false;
        if (player_aims || (joined && !s.assist_wait && now >= s.assist_until)) s.still_until = 0;
        if (s.combo_t0 || s.assist_wait) buttons &= ~B(BTN_A);  // (held back: the window, then the aim)
        if (now < s.assist_until) buttons |= B(BTN_R);
        if (s.keep_r && now >= s.keep_r) s.keep_r = 0, s.replay_until = now + SHOT_A;  // (R lets go, the presses come)
        if (now < s.keep_r) buttons = (buttons & ~(B(BTN_A) | B(BTN_B) | B(BTN_X) | B(BTN_Y))) | B(BTN_R);
        else if (now < s.replay_until) buttons |= s.keep_buttons;
        else s.keep_buttons = 0;
        if (now < s.assist_fire && !(buttons & B(BTN_B))) buttons |= B(BTN_A);
    } else {
        s.assist_wait = s.fired = s.assist_off = s.still_wait = false, s.assist_until = s.combo_t0 = s.keep_r = s.assist_fire = 0;
        s.keep_buttons = s.replay_until = s.still_until = 0;
    }
    // The help's R, however it ended - the lock-on off for a moment, the layout gone to a menu for a frame, the gun put
    // away -, stays down until the game can't take it for a tap of R, which opens the scope (the owner's Galaxy in the
    // Arena, 2026-11-10: "a veces cuando disparo se pone la mira grande sola")
    bool help_r = (buttons & B(BTN_R)) && !(in & B(BTN_R));
    if (help_r) {
        if (!s.help_r0) s.help_r0 = now;
    } else if (s.help_r0) {
        if (!(in & B(BTN_R)) && now < s.help_r0 + aim_hold_ns()) buttons |= B(BTN_R);
        else s.help_r0 = 0;
    }
    g_assist_until = s.assist_until;
    g_still_until = s.still_until;
    g_assist_waiting = s.assist_wait;
    g_player_aims = player_aims;
    return buttons;
}

// (the hunt camera's thread, hle/mousecam.c) the lock-on is on and the player is not turning the camera
extern "C" bool touch_lock_active(void) { return g_lock_follow.load(); }
extern "C" bool touch_lock_on(void) { return g_lock_shots.load(); }
extern "C" bool touch_lock_engaged(void) { return g_lock_engaged.load(); }
extern "C" bool touch_lock_assist(void) { return g_lock_shots.load() && SDL_GetTicksNS() < g_assist_until.load(); }
extern "C" bool touch_lock_assist_waiting(void) { return touch_lock_assist() && g_assist_waiting.load(); }
extern "C" bool touch_lock_player_aims(void) { return g_lock_shots.load() && g_player_aims.load(); }
extern "C" bool touch_lock_still(void) { return g_lock_shots.load() && SDL_GetTicksNS() < g_still_until.load(); }

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
bool g_build_preview;  // (render thread) a model in a window in this frame
extern "C" void touch_hud_preview(void) { g_build_preview = true; }
std::atomic<uint64_t> g_test_window_until;  // (tests: fakewindow) a menu's window seen in every frame until then
extern "C" void touch_test_window(int ms) {
    g_test_window_until = SDL_GetTicksNS() + (uint64_t)(ms > 0 ? ms : 0) * MS;
    rt_logf("[touch] (test) a window of the game's menus for %d ms\n", ms);
}

extern "C" void touch_item_step(int dir) {
    if (!dir) return;
    TapAfterUnlock tap_after;
    std::lock_guard<std::mutex> l(g_lock);
    State &s = g_s;
    uint64_t now = SDL_GetTicksNS();
    item_step(s, dir > 0 ? 1 : -1, now);
    g_tap_due = false;  // (a key or the wheel: no click of the phone's vibrator)
}

// A Hunter Art performed with the right stick (Control Type 4, or Type 1 with R Stick Controls swapped): the stick
// tilted all the way that way for a moment, on the right stick's value (host_input.c, after touch_input)
extern "C" void touch_right_stick(int *rx, int *ry) {
    std::lock_guard<std::mutex> l(g_lock);
    const State &s = g_s;
    if (!s.rs_dir || SDL_GetTicksNS() >= s.rs_until) return;
    *rx = s.rs_dir & 1 ? 32767 : s.rs_dir & 2 ? -32767 : 0;
    *ry = s.rs_dir & 4 ? 32767 : s.rs_dir & 8 ? -32767 : 0;
}

extern "C" void touch_hud_frame(void) {
    if (SDL_GetTicksNS() < g_test_window_until.load()) g_build_menu = true;
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
    if (g_build_preview) g_frames_since_preview = 0;
    else if (g_frames_since_preview.load() < 1000) g_frames_since_preview = g_frames_since_preview.load() + 1;
    g_build_preview = false;
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

// the lock-on with the touch controls hidden (a controller, a keyboard: no L to show it by): a lit padlock on a dark
// disc by the target camera's icons (their zone), pulsing while the camera follows the monster. It takes no taps
void lock_mark(ImDrawList *dl, const Rect &target, float a, float h, uint64_t now) {
    float glow = mousecam_lock_following() ? .75f + .25f * sinf(now / 1e9f * 4) : .6f;
    ImVec2 pc(target.x0 - .02f * h, target.y1 - .02f * h);
    dl->AddCircleFilled(pc, .022f * h, fade(DEEP, a * .85f), 20);
    dl->AddCircle(pc, .022f * h, fade(GOLD, a), 20, .003f * h);
    padlock(dl, pc, .026f * h, true, fade(GLOW, a * glow), fade(DEEP, a));
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
// the same badge with a d-pad's direction on it ('v' down, '<' left, '>' right): a button of the d-pad, as the game's
// own prompts draw it
void badge_arrow(ImDrawList *dl, ImVec2 b, float br, char dir, float a) {
    dl->AddCircleFilled(b, br, fade(IM_COL32(18, 12, 9, 255), a), 20);
    dl->AddCircle(b, br, fade(GOLD, a), 20, br * .14f);
    float k = br * .5f;
    ImVec2 d = dir == '<' ? ImVec2(-1, 0) : dir == '>' ? ImVec2(1, 0) : ImVec2(0, 1), n(-d.y, d.x);
    dl->AddTriangleFilled(ImVec2(b.x + d.x * k, b.y + d.y * k), ImVec2(b.x - d.x * k * .55f + n.x * k * .8f, b.y - d.y * k * .55f + n.y * k * .8f),
                          ImVec2(b.x - d.x * k * .55f - n.x * k * .8f, b.y - d.y * k * .55f - n.y * k * .8f), fade(INK, a));
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

// The sprint lock's circle above the stick ([Touch] StickRun): a small disc with a padlock, open, faded in (a) as the thumb
// pushes up toward it; armed (the thumb on it: lifted there it locks), lit, the padlock still open; locked, lit, the
// padlock closed, a gold ring round it. The same in the MH4U port's touch.cpp
void lock_circle(ImDrawList *dl, ImVec2 c, float cr, float a, bool armed, bool locked) {
    if (a <= 0.0f) return;
    disc(dl, c, cr, armed || locked, a);
    padlock(dl, ImVec2(c.x, c.y - cr * .08f), cr * .74f, locked, fade(armed || locked ? GLOW : INK, a), fade(DEEP, a));
    if (locked) dl->AddCircle(c, cr * 1.18f, fade(GLOW, a * (.6f + .3f * sinf(SDL_GetTicksNS() / 1e9f * 4))), 32, cr * .1f);
}

// an arrow of the item bar's: a disc with an arrowhead pointing its way, lit as it turns the bar
void item_arrow_disc(ImDrawList *dl, Geo g, int d, bool lit, float a) {
    disc(dl, g.c, g.r, lit, a);
    float ch = g.r * .42f;
    dl->AddTriangleFilled(ImVec2(g.c.x + d * ch, g.c.y), ImVec2(g.c.x - d * ch * .6f, g.c.y - ch), ImVec2(g.c.x - d * ch * .6f, g.c.y + ch),
                          fade(lit ? GLOW : INK, a));
}
void control(ImDrawList *dl, ImFont *font, const Layout &l, int i, float w, float h, const State &s, float a) {
    Geo g = place(l, i, w, h);
    if (i == C_ITEML || i == C_ITEMR) {  // (the editor's: in a hunt they go with the item bar)
        item_arrow_disc(dl, g, i == C_ITEML ? -1 : 1, false, a);
        return;
    }
    if (i == C_RUNLOCK) {  // (the editor's: in a hunt it shows as the thumb pushes toward it, with the stick)
        lock_circle(dl, g.c, g.r, a * .8f, false, false);
        return;
    }
    if (i == C_STICK) {
        if ((s.stick_on || s.run_free) && !s.edit) {
            // (a locked run going on by itself: the knob held at the rim, forward)
            ImVec2 base(s.bx, s.by), knob = s.stick_on ? ImVec2(s.kx, s.ky) : ImVec2(s.bx, s.by - g.r);
            stick(dl, base, knob, g.r, a, true);
            // the sprint lock's circle: faded in as the thumb pushes up past the rim while a lock could start; lit with
            // the thumb on it (armed) and locked
            // (toward the circle, wherever the editor put it: above the stick unless moved)
            float v = s.run_lock || s.run_over ? 1.0f : 0.0f;
            Geo lg = place(l, C_RUNLOCK, w, h);
            if (!s.run_lock && !s.run_over && s.run_can && s.stick_on) {
                float ux = lg.c.x - s.bx, uy = lg.c.y - s.by, ud = std::max(1.0f, hypotf(ux, uy));
                float dx = s.fx - s.bx, dy = s.fy - s.by, d = hypotf(dx, dy), up = d > 1.0f ? (dx * ux + dy * uy) / (d * ud) : 0.0f;
                float reach = std::max(1.0f, ud - lg.r - .9f * g.r);
                v = std::clamp((d - .9f * g.r) / reach, 0.0f, 1.0f) * std::clamp((up - LOCK_UP) / .15f, 0.0f, 1.0f);
            }
            lock_circle(dl, lg.c, lg.r, a * v, s.run_over, s.run_lock);
            if (s.run) {  // (running: a gold ring round the rim, and streaks behind the knob)
                dl->AddCircle(base, g.r * 1.12f, fade(GLOW, a * .85f), 48, g.r * .07f);
                float dx = knob.x - s.bx, dy = knob.y - s.by, n = std::max(1.0f, hypotf(dx, dy));
                dx /= n, dy /= n;
                for (int i = -1; i <= 1; i++) {
                    ImVec2 o(knob.x - dy * i * g.r * .22f, knob.y + dx * i * g.r * .22f);
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
            if (f.keep & B(BTN_R)) mask |= i == C_R ? 1 : i == C_CORNER && f.ctl == C_CORNER ? 4 : 0;  // (a guard kept)
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
    if (i == C_RUN && s.drawn == 1 && !s.edit) a *= .45f, mask = 0;  // (the weapon out: dimmed, it presses nothing)
    disc(dl, g.c, r, mask != 0, a);
    if (i == C_RUN && s.run) dl->AddCircle(g.c, g.r * 1.16f, fade(GLOW, a * (.6f + .3f * sinf(SDL_GetTicksNS() / 1e9f * 4))), 40, g.r * .09f);
    // (the lock-on, as the MH4U port's: a ring round L while it is on, pulsing while the camera follows the monster)
    if (i == C_L && s.lock && !s.edit)
        dl->AddCircle(g.c, g.r * 1.16f, fade(GLOW, a * (mousecam_lock_following() ? .6f + .3f * sinf(SDL_GetTicksNS() / 1e9f * 4) : .45f)),
                      40, g.r * .09f);
    Icon ic = DEFS[i].icon;
    char letter[8];
    snprintf(letter, sizeof letter, "%s", i == C_ZL && !s.edit ? target_letter()
                                          : i == C_L && !s.edit && camera_buttons() == B(BTN_ZL) ? "ZL" : DEFS[i].letter);
    if (is_ranged(i)) ranged_face(i, s.edit ? 1 : s.drawn, &ic, letter, sizeof letter);
    icon(dl, ic, g.c, i == C_RFIRE ? r * .94f : r, fade(mask ? GLOW : INK, a), fade(DEEP, a));
    float br = std::max(r * (i == C_RFIRE ? .22f : .3f), .021f * h);  // (the large one's as large as the others')
    ImVec2 bc(g.c.x + r * .7f, g.c.y + r * .7f);
    if (i == C_ZL && strchr("v<>", letter[0]) && letter[0]) badge_arrow(dl, bc, br, letter[0], a);  // (Type 2: the d-pad's)
    else if (i != C_MMINUS) badge(dl, font, bc, br, letter, a);  // (the small Minus: its glyph)
    ImVec2 pc;
    float ps;
    if (i == C_L && padlock_place(s, w, h, &pc, &ps))  // (the padlock by L: a tap turns the lock-on; closed and lit while on)
        padlock(dl, pc, ps, s.lock, fade(s.lock ? GLOW : INK, a), fade(DEEP, a));
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

// A button's word on its plate: on one line, smaller where it is too long; where that would make it much smaller (a small
// panel, a narrow screen), on two lines split at the space nearest its middle, when those come out larger (never after a
// one-letter word: "Diseño:" over "a distancia", not "Diseño: a" over "distancia")
void button_text(ImDrawList *dl, ImFont *font, const Rect &b, float fs, ImU32 col, const char *t) {
    float room = (b.x1 - b.x0) * .92f, wide = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x, one = wide > room ? fs * room / wide : fs;
    ImVec2 c((b.x0 + b.x1) * .5f, (b.y0 + b.y1) * .5f);
    int n = (int)strlen(t), cut = -1;
    for (int k = 2; k < n - 1; k++)
        if (t[k] == ' ' && t[k - 2] != ' ' && (cut < 0 || abs(2 * k - n) < abs(2 * cut - n))) cut = k;
    if (one < fs * .8f && cut > 0 && n < 64) {
        char first[64];
        snprintf(first, sizeof first, "%.*s", cut, t);
        const char *second = t + cut + 1;
        float w2 = std::max(font->CalcTextSizeA(fs, FLT_MAX, 0, first).x, font->CalcTextSizeA(fs, FLT_MAX, 0, second).x);
        float two = std::min(std::min(fs, fs * room / std::max(1.0f, w2)), (b.y1 - b.y0) * .44f);
        if (two > one * 1.1f) {
            text_center(dl, font, two, ImVec2(c.x, c.y - two * .52f), col, first);
            text_center(dl, font, two, ImVec2(c.x, c.y + two * .52f), col, second);
            return;
        }
    }
    text_center(dl, font, one, c, col, t);
}

// A grip of the editor's panel: four arrowheads round a cross on a dark plate with a gold edge (its title strip drags it)
void grip(ImDrawList *dl, const Rect &r, ImU32 col) {
    ImVec2 c((r.x0 + r.x1) * .5f, (r.y0 + r.y1) * .5f);
    float s = std::min(r.x1 - r.x0, r.y1 - r.y0) * .5f, a = s * .74f, hw = s * .25f, t = s * .12f;
    dl->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), fade(DEEP, .9f), s * .25f);
    dl->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), fade(col, .75f), s * .25f, 0, s * .08f);
    dl->AddLine(ImVec2(c.x - a * .6f, c.y), ImVec2(c.x + a * .6f, c.y), col, t);
    dl->AddLine(ImVec2(c.x, c.y - a * .6f), ImVec2(c.x, c.y + a * .6f), col, t);
    for (int q = 0; q < 4; q++) {
        ImVec2 d = q == 0 ? ImVec2(1, 0) : q == 1 ? ImVec2(-1, 0) : q == 2 ? ImVec2(0, 1) : ImVec2(0, -1), n(-d.y, d.x);
        ImVec2 tip(c.x + d.x * a, c.y + d.y * a), base(c.x + d.x * (a - hw * 1.6f), c.y + d.y * (a - hw * 1.6f));
        dl->AddTriangleFilled(tip, ImVec2(base.x + n.x * hw, base.y + n.y * hw), ImVec2(base.x - n.x * hw, base.y - n.y * hw), col);
    }
}
// The panel's grips, at both ends of its title strip (grab), in its measure u
void panel_grips(const Rect &grab, float u, Rect g[2]) {
    float gw = .07f * u, m = .012f * u;
    g[0] = {grab.x0 + m, grab.y0 + m * 1.4f, grab.x0 + m + gw, grab.y1 - m * .5f, true};
    g[1] = {grab.x1 - m - gw, g[0].y0, grab.x1 - m, g[0].y1, true};
}

// The editor: the game's HUD parts framed where they are (where they would be when they aren't drawn: outside a hunt,
// hidden), named below them on a dark plate, the picked one pulsing with its size; the touch controls over them, with
// their names (with the touch controls' HUD); a part and a control over each other both red; the panel, and a line under
// it for what the player should know (the item bar can't be hidden with the touch controls, a hidden part a tap no
// longer works, the frames outside a hunt)
void draw_editor(ImDrawList *dl, ImFont *font, const State &s, float w, float h) {
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, 96));
    bool touch = edit_controls(s);
    const HudVals *hud = s.layout.hud[s.edit_prof];
    Rect panel, btn[E_COUNT], grab;
    float u = editor_panel(s.epanel, w, h, &panel, btn, &grab);
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
    float line = .004f * u;
    plate(dl, panel, line, 1.0f, false);
    // (the panel picked: pulsing round it, its grips lit, its size by the right one)
    bool picked = s.sel == SEL_PANEL;
    if (picked) {
        float pulse_a = .65f + .35f * sinf(SDL_GetTicksNS() / 1e9f * 6), m = .004f * h;
        dl->AddRect(ImVec2(panel.x0 - m, panel.y0 - m), ImVec2(panel.x1 + m, panel.y1 + m), fade(GLOW, pulse_a), 0, 0, .006f * h);
    }
    Rect grips[2];
    panel_grips(grab, u, grips);
    for (const Rect &g : grips) grip(dl, g, picked ? GLOW : GOLD);
    ImFont *title = ImGui::GetIO().Fonts->Fonts.Size > 1 ? ImGui::GetIO().Fonts->Fonts[1] : font;  // (the serif: overlay.cpp)
    text_center(dl, title, .034f * u, ImVec2((panel.x0 + panel.x1) * .5f, panel.y0 + .045f * u), GLOW, T("Controls and HUD"));
    if (picked) {
        char pct[16];
        snprintf(pct, sizeof pct, "%d%%", (int)lroundf(std::clamp(s.epanel.k, EPANEL_MIN, 1.0f) * 100));
        float pf = .03f * u, pw = font->CalcTextSizeA(pf, FLT_MAX, 0, pct).x;
        text_center(dl, font, pf, ImVec2(grips[1].x0 - .012f * u - pw * .5f, panel.y0 + .045f * u), GLOW, pct);
    }
    const char *say = T(picked              ? "The panel: drag its title to move it  -  pinch or use - / + for its size"
                        : s.sel == C_MPAD   ? "Shows while a menu of the game's is open - drag to move, pinch or use - / + for its size"
                        : part >= 0 && hud[part].hidden ? "Hidden: Show brings it back"
                        : s.sel >= 0        ? "Drag to move  -  pinch or use - / + for its size"
                                            : "Tap a control or a part of the game's screen to pick it, drag it to move it");
    float ss = .022f * u, room = grips[1].x0 - grips[0].x1 - .024f * u, wide = font->CalcTextSizeA(ss, FLT_MAX, 0, say).x;
    if (wide > room) ss *= room / wide;  // (a narrow screen, a long sentence: on one line, smaller)
    text_center(dl, font, ss, ImVec2((panel.x0 + panel.x1) * .5f, panel.y0 + .078f * u), INK, say);
    static const char *const STYLE_LABEL[STYLE_COUNT] = {"Attacks: pill", "Attacks: buttons", "Attacks: corner"};
    const char *const LABEL[E_COUNT] = {"-", "+", part >= 0 && hud[part].hidden ? "Show" : "Hide", "Reset",
                                        trad() ? "Layout: traditional" : ranged_layout() ? "Layout: ranged" : "Layout: melee",
                                        trad() ? "A B X Y" : STYLE_LABEL[s.layout.style],
                                        touch ? "HUD: touch" : "HUD: controller", "Defaults", "Cancel", "Save"};
    for (int i = 0; i < E_COUNT; i++) {
        if (!btn[i].ok) continue;
        bool off = ((i == E_SMALLER || i == E_BIGGER || i == E_RESET) && s.sel < 0 && !picked) ||
                   (i == E_HIDE && (part < 0 || !hud_hide_ok(s.edit_prof, part))) || ((i == E_SET || i == E_STYLE) && !touch) ||
                   (i == E_STYLE && ranged_layout()) || ((i == E_STYLE || i == E_SET) && trad());
        bool lit = false;
        for (const Finger &f : s.f)
            if (f.kind == F_MENU && f.ctl == i) lit = true;
        plate(dl, btn[i], line * .8f, off ? .45f : 1.0f, lit && !off);
        button_text(dl, font, btn[i], (i < E_HIDE ? .05f : .028f) * u, fade(i == E_SAVE ? GLOW : INK, off ? .45f : 1.0f), T(LABEL[i]));
    }
    // (what the player should know: about the part picked first, else a hidden part a tap no longer works, else why the
    // frames show outside a hunt)
    const char *note = nullptr;
    int hidden_tap = touch && hud[HP_ARTS].hidden ? HP_ARTS : -1;
    if (touch && part == HP_ITEMS) note = "The item bar picks and uses items with the touch controls: it can't be hidden here";
    else if (touch && part == HP_ARTS && hud[part].hidden) hidden_tap = part;
    if (!note && hidden_tap == HP_ARTS) note = "Hidden: no tap performs the Hunter Arts - the game's own buttons for them do";
    if (!note && !any_live && s.sel < 0) note = "Outside a hunt the frames show where each part goes";
    if (note) {
        const char *t = T(note);
        float fs = .022f * h, tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x, maxw = std::max((panel.x1 - panel.x0) * .96f, w * .6f);
        if (tw > maxw) fs *= maxw / tw, tw = maxw;
        ImVec2 c((panel.x0 + panel.x1) * .5f, panel.y1 + fs * 1.1f);  // (under the panel, above it at the bottom edge)
        if (c.y + fs * .75f > h) c.y = panel.y0 - fs * 1.1f;
        c.x = std::clamp(c.x, tw * .5f + fs * .5f, std::max(tw * .5f + fs * .5f, w - tw * .5f - fs * .5f));
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

// A hunt button under the menu pad's group (its tabs reach ZR, ZL and L at their usual places): it fades out as the pad
// comes in and back as it goes, as in the MH4U port (its under_mpad); there the pad's group takes the finger first (hit).
// Never the attacks nor B (a menu's confirm and back). The hunt's d-pad (ShowDpad) wherever it is: the menu pad takes its
// place, so it goes as the pad comes in and comes back as it goes, and no finger takes it meanwhile (hit; 4u-native #25)
bool under_mpad(const Layout &l, int i, float w, float h) {
    if (of_mpad(i) || i == C_SETTINGS || i == C_ATTACK || i == C_CORNER || i == C_HEAVY || i == C_LIGHT || i == C_DODGE)
        return false;
    if (i == C_HDPAD) return true;
    Geo g = place(l, i, w, h);
    for (int m : {C_ML, C_MR, C_MZL, C_MZR, C_MMINUS}) {
        if (!mpad_member(m)) continue;
        Geo t = place(l, m, w, h);
        float hw = is_tab(m) ? t.r * TAB_W : t.r, hh = is_tab(m) ? t.r * TAB_H : t.r;
        if (fabsf(g.c.x - t.c.x) < hw + g.r && fabsf(g.c.y - t.c.y) < hh + g.r) return true;
    }
    Geo p = place(l, C_MPAD, w, h);
    return hypotf(g.c.x - p.c.x, g.c.y - p.c.y) < p.r + g.r;
}

// (MHGU_PERF_OVERLAY_REUSE, runtime/overlay.cpp) Everything touch_draw's picture is made of, for one frame: the controls'
// state (its clocks that draw nothing cleared, those that do as what they draw), what shows and where (in_mode, the HUD
// parts' places), the settings (their generation), the weapon's layout, the game's buttons named on the badges, the
// screen. Two frames with the same key draw the same lists. false: something moves by itself (a finger down, the stick, a
// run, the lock-on's pulse, the menu pad coming or going, the editor): no frame stands for the next
struct DrawKey {
    State s;
    float w, h;
    int flags, ranged, bow_type, style, edit_set, config_gen, nrows;
    float pad_a, mpad_out, rows[3][2], parts[3][5];  // (parts: the target camera's, the item bar's, the arts' zone_rect)
    uint64_t camera, modes;
    char target[8];
};
static_assert(C_COUNT <= 64, "DrawKey.modes: a bit per control");
bool g_reuse_on;      // (touch_reuse_begin: the overlay reuses frames, touch_draw keeps the key of each)
DrawKey g_drawn;      // (game thread) the key of the frame touch_draw drew last...
bool g_drawn_ok;      // ...and whether it may stand for another
bool draw_key(float w, float h, DrawKey *k) {
    memset(k, 0, sizeof *k);
    {
        std::lock_guard<std::mutex> l(g_lock);
        memcpy(&k->s, &g_s, sizeof k->s);
    }
    State &s = k->s;
    uint64_t now = SDL_GetTicksNS();
    bool lit = s.lit_until && now < s.lit_until, item_lit = now < s.item_flash, art_lit = now < s.art_flash;
    float untouched = s.last_ns ? (now - s.last_ns) / 1e9f : 99.0f;  // (as touch_draw: dimmer after 5 s untouched)
    bool idle = untouched > 5.0f;
    k->pad_a = s.menu ? std::min(1.0f, (float)(now - s.menu_on_ns) / (float)MPAD_IN) * mpad_fade(now) : 0.0f;
    k->mpad_out = s.menu ? mpad_fade(now) : 0.0f;
    if (s.edit || s.edit_request || s.stick_on || s.run || s.run_lock || s.run_free || s.lock || g_lock_mark)
        return false;
#ifndef __ANDROID__
    if (s.hud_edit || g_hud_edit) return false;
#endif
    for (const Finger &f : s.f)
        if (f.kind != F_NONE) return false;
    if ((k->pad_a > 0 && k->pad_a < 1) || (k->mpad_out > 0 && k->mpad_out < 1)) return false;
    // (clocks: those that draw something as what they draw; the others, which move while nothing changes, cleared)
    s.last_ns = s.menu_on_ns = s.item_flash = s.art_flash = s.lit_until = 0;
    s.hunt_ns = s.hunter_ns = s.cam_ns = s.tap_ns = s.step_ns = s.others_ns = s.stuck_ns = 0;
    k->flags = (int)g_visible | (int)g_gear_only << 1 | (int)overlay_menu_open() << 2 | (int)lit << 3 | (int)item_lit << 4 |
               (int)art_lit << 5 | (int)idle << 6 | (int)menu_pages_zl() << 7;
    k->w = w, k->h = h;
    k->ranged = g_ranged.load(), k->bow_type = g_bow_type.load(), k->style = g_style.load(), k->edit_set = g_edit_set.load();
    k->config_gen = config_live_generation();
    k->camera = camera_buttons();
    snprintf(k->target, sizeof k->target, "%s", target_letter());
    for (int i = 0; i < C_COUNT; i++)
        if (in_mode(i, s.menu)) k->modes |= 1ull << i;
    Zones z = zones();
    const int parts[3] = {HP_TARGET, HP_ITEMS, HP_ARTS};
    for (int i = 0; i < 3; i++) {
        Rect r = zone_rect(z, parts[i], w, h);
        if (!r.ok) continue;
        float *v = k->parts[i];
        v[0] = r.x0, v[1] = r.y0, v[2] = r.x1, v[3] = r.y1, v[4] = 1;
    }
    k->nrows = arts_rows(z, w, h, k->rows);
    return true;
}

}  // namespace

// (MHGU_PERF_OVERLAY_REUSE, runtime/overlay.cpp: the game thread, once at the start) touch_draw keeps each frame's key
extern "C" void touch_reuse_begin(void) { g_reuse_on = true; }
// (the game thread, before a frame is built) the controls would draw the very same lists as in the frame drawn last
extern "C" bool touch_draw_unchanged(float w, float h) {
    if (!g_drawn_ok) return false;
    DrawKey k;
    return draw_key(w, h, &k) && !memcmp(&k, &g_drawn, sizeof k);
}

extern "C" void touch_draw(float w, float h) {
    if (g_reuse_on) g_drawn_ok = draw_key(w, h, &g_drawn);  // (MHGU_PERF_OVERLAY_REUSE: what this frame is made of)
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
        s.bx *= kx, s.kx *= kx, s.by *= ky, s.ky *= ky, s.fx *= kx, s.fy *= ky;
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
        if (g_lock_mark && target.ok) lock_mark(dl, target, 1.0f, h, now);
        Rect arts = zone_rect(z, HP_ARTS, w, h);  // (a Hunter Art tapped: its row lit a moment, as with them shown)
        float rows[3][2];
        if (g_gear_only && arts.ok && now < s.art_flash && s.art_row < arts_rows(z, w, h, rows))
            dl->AddRect(ImVec2(arts.x0, rows[s.art_row][0]), ImVec2(arts.x1, rows[s.art_row][1]), fade(GLOW, a), .01f * h, 0, .005f * h);
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
    // (in a menu of the game's the player's buttons stay, at half strength so its window reads under them, as in the
    // MH4U port; the menu pad's group over them at full)
    for (int i = 0; i < C_COUNT; i++) {
        if (!in_mode(i, s.menu)) continue;
        float k = of_mpad(i) ? pad_a : s.menu ? .5f : 1.0f;
        if (!of_mpad(i) && pad_a > 0 && under_mpad(s.layout, i, w, h)) k *= 1 - pad_a;
        if (k > .01f) control(dl, font, s.layout, i, w, h, s, a * k);
    }
    // the HUD parts that answer a finger: the item bar's turn, the art performed (the item bar's arrows as
    // in a hunt while a menu is open too: nothing changes as one opens, though they turn nothing then)
    Rect item = zone_rect(z, HP_ITEMS, w, h), arts = zone_rect(z, HP_ARTS, w, h);
    if (item.ok && config_live_touch_show(TB_ITEM_ARROWS)) {
        for (int d = -1; d <= 1; d += 2) {
            bool lit = now < s.item_flash && s.item_dir == d;
            item_arrow_disc(dl, place(s.layout, d < 0 ? C_ITEML : C_ITEMR, w, h), d, lit, a);
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
}
