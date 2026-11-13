// Host controls -> the Switch controller the game reads (runtime/hle/input.c).
//
// Keyboard and mouse: the keys and mouse buttons of the settings' [Keybinds] (runtime/config.c; by default WASD left
//   stick, Left Shift held: half tilt, walking; left click = X, right click = A, Space = B, R / middle click = Y, Q = L,
//   E = R, Tab = ZL, T = ZR, Enter = Plus, Backspace = Minus, arrow keys = d-pad, Ctrl = left stick click, Alt = right
//   stick click, I / K / J / L = the right stick all the way up / down / left / right: the camera; Combo1Key.. a combined
//   button's buttons, [Touch] Combo1..), mouse movement the camera (in a hunt its angles directly, elsewhere the right
//   stick; click in the window to capture the mouse, Esc releases it), wheel up / down = item bar left / right (ItemLeft
//   / ItemRight: in a hunt, one item per press or notch without holding L, runtime/hle/itembar.c). In the game's menus
//   and dialogs ([Controls] MouseMenus: a talk, a message, a yes / no) the mouse is free and shown and points and clicks
//   instead: left click A (on the row or cell under it, else on what has the focus), right click B, the wheel scrolls,
//   the side buttons turn pages (runtime/hle/mousemenu.c). On a desktop A is F and the right click, and a mouse button
//   on A presses it out of the game's menus and dialogs only with the weapon out, an attack (mouse_a_ok). On a desktop a
//   modifier key on a button (Ctrl, Alt) presses it when tapped alone, not in a shortcut such as Alt+Tab (mod_spent).
//   While the game shows a text entry (names, chat) the keyboard types instead
//   (runtime/textinput.c). / ([Controls] ChatKey) opens a line of chat, Enter sends it, also in a quest (runtime/hle/chat.c).
//   F11 switches windowed <-> full screen, F12 saves a screenshot, Shift+F12 the next 8
//   frames (exact rendered pixels, run\shots\), F7 tries the MSAA levels; in a test build (MHGU_DEV) F10 dumps every
//   render pass of the next 2 frames (debug, slow), F9 toggles how colour clears are done and F8 the shadow pass's
//   scissors (debug).
// Gamepad (SDL): face buttons by position as on a Switch controller (the bottom one is B), or by their labels
//   (settings FaceButtons, runtime/config.c; MHGU_PAD_POSITIONAL=0|1 overrides).
//   Every controller is open and the one a button was last pressed on is read (a Steam Deck with a controller plugged
//   in has two). SDL calls other devices a gamepad too (a Steam Controller's puck, an analog keyboard's mouse
//   interface): MHGU_PAD=<part of the name> keeps only yours, and the log lists every one it finds.
// Scripted input (tests): MHGU_INPUT="frame:BUTTON:frames,..." holds BUTTON from that presented frame for that many
//   frames. BUTTON: A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LSTICK RSTICK, or STICK_UP/DOWN/LEFT/RIGHT and
//   RSTICK_UP/DOWN/LEFT/RIGHT (full tilt).
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "config.h"
#include "menu_confirm.h"
#include "forge.h"
#include "haptics.h"
#include "hle/chat.h"
#include "hle/input.h"
#include "hle/itembar.h"
#include "hle/mousecam.h"
#include "hle/mousemenu.h"
#include "overlay.h"
#include "padmap.h"
#include "rt.h"
#include "textinput.h"
#include "touch.h"

#define B(x) (1ull << (x))

static bool keys[SDL_SCANCODE_COUNT];
static SDL_Gamepad *gyro_pad;  // (the controller whose gyroscope is read while aiming: gyro_update)
static SDL_Gamepad *pad;  // the controller read: of those open, the one a button was last pressed on
#define MAX_PADS 8
static struct { SDL_Gamepad *pad; uint64_t opened_ns; bool real; } pads[MAX_PADS];  // (real: hides the touch controls)
static int npads;

// [Keybinds] (config.h BIND_*): the Switch button each presses (-1: a stick's directions, walking; -2: the item bar,
// turned directly; -3: a combined button's buttons)
static const int bind_buttons[BINDS] = {
    [BIND_MOVE_UP] = -1, [BIND_MOVE_DOWN] = -1, [BIND_MOVE_LEFT] = -1, [BIND_MOVE_RIGHT] = -1, [BIND_WALK] = -1,
    [BIND_CAM_UP] = -1, [BIND_CAM_DOWN] = -1, [BIND_CAM_LEFT] = -1, [BIND_CAM_RIGHT] = -1,
    [BIND_A] = BTN_A, [BIND_B] = BTN_B, [BIND_X] = BTN_X, [BIND_Y] = BTN_Y, [BIND_L] = BTN_L, [BIND_R] = BTN_R,
    [BIND_ZL] = BTN_ZL, [BIND_ZR] = BTN_ZR, [BIND_PLUS] = BTN_PLUS, [BIND_MINUS] = BTN_MINUS, [BIND_DPAD_UP] = BTN_UP,
    [BIND_DPAD_DOWN] = BTN_DOWN, [BIND_DPAD_LEFT] = BTN_LEFT, [BIND_DPAD_RIGHT] = BTN_RIGHT, [BIND_LSTICK] = BTN_LSTICK,
    [BIND_RSTICK] = BTN_RSTICK, [BIND_ITEM_LEFT] = -2, [BIND_ITEM_RIGHT] = -2,
    [BIND_COMBO1] = -3, [BIND_COMBO2] = -3, [BIND_COMBO3] = -3, [BIND_COMBO4] = -3, [BIND_COMBO5] = -3, [BIND_COMBO6] = -3,
};
static uint32_t mouse_held;  // the mouse buttons held (bit: SDL_BUTTON_*), while captured
// (the game's menus: the buttons pressed while a menu had the mouse, or held as one took it, are the menu's until let
// go, whatever is open then: hle/mousemenu.c)
static uint32_t menu_held;
// A wheel notch on a keybind other than the item bar's is a short press of it (WHEEL_PRESS_NS: long enough for the
// game to see at 30 updates a second; notches closer together than that are one press)
#define WHEEL_PRESS_NS 70000000ull
static uint64_t wheel_until[2];  // up, down: pressed until then
// A modifier key (SDL's eight in a row, Left Ctrl to Right GUI: Ctrl, Shift, Alt, the Windows key) on a keybind that
// presses something presses it only when tapped: let go with no other key or mouse button pressed meanwhile, and then
// for as long as a wheel notch. Pressed with another key it is a shortcut and presses nothing: Alt is the right stick's
// click by default, and Alt+Tab and Alt+Enter turned the lock-on on and off. Shift is no shortcut key here and is held
// as any key: it is R by default (running, held while moving). On a stick's direction (Walk, moving, the camera) every
// key is held. (A desktop's: a phone's keyboard as before.)
#define MOD_KEYS 8
static bool mod_held[MOD_KEYS], mod_spent[MOD_KEYS];  // down now; another key or button went down since it did
static uint64_t mod_until[MOD_KEYS];                  // tapped: pressed until then
static bool is_mod(int code) { return code >= SDL_SCANCODE_LCTRL && code <= SDL_SCANCODE_RGUI; }
static bool is_tap_mod(int code) { return is_mod(code) && code != SDL_SCANCODE_LSHIFT && code != SDL_SCANCODE_RSHIFT; }
// A mouse button (or a wheel notch) on A presses it, out of the game's menus and dialogs, only with the weapon out (a
// desktop's: mouse_a_ok, each update). By default A is F and the right click: with the right click talking, opening the
// item box, gathering and picking up, and in the menus (MouseMenus) going back, the same button did two opposite things.
// Now those are F's alone and the right click attacks. (Every ini, an older one's "A = Mouse Right, F" too.)
static bool mouse_a = true;
static bool is_mouse_code(int code) { return code > KEY_MOUSE && code <= KEY_WHEEL_DOWN; }
static bool bind_held(int bind, bool wheel) {
    uint64_t now = plat_now_ns();
#ifdef __ANDROID__
    bool tap = false;
#else
    bool tap = bind_buttons[bind] != -1;  // (presses something: a modifier is a tap there)
#endif
    for (int k = 0; k < BIND_KEYS; k++) {
        int code = config_live_bind(bind, k);
        if (bind == BIND_A && !mouse_a && is_mouse_code(code)) continue;
        if (tap && is_tap_mod(code)) {
            if (now < mod_until[code - SDL_SCANCODE_LCTRL]) return true;
            continue;
        }
        if (code > 0 && code < SDL_SCANCODE_COUNT && keys[code]) return true;
        if (code > KEY_MOUSE && code <= KEY_MOUSE + SDL_BUTTON_X2 && (mouse_held >> (code - KEY_MOUSE) & 1)) return true;
        if (wheel && (code == KEY_WHEEL_UP || code == KEY_WHEEL_DOWN) && now < wheel_until[code - KEY_WHEEL_UP])
            return true;
    }
    return false;
}
// The wheel turned n notches (> 0 up): the item bar's keybinds turn it a notch each, others are pressed briefly
static void wheel_turn(int n) {
    int code = n > 0 ? KEY_WHEEL_UP : KEY_WHEEL_DOWN;
    for (int i = 0; i < BINDS; i++)
        for (int k = 0; k < BIND_KEYS; k++) {
            if (config_live_bind(i, k) != code) continue;
            if (i == BIND_ITEM_LEFT || i == BIND_ITEM_RIGHT)  // (a Prowler's: its Support Moves, runtime/touch.cpp)
                for (int k2 = 0; k2 < abs(n) && k2 < 4; k2++) touch_item_step(i == BIND_ITEM_LEFT ? -1 : 1);
            else wheel_until[code - KEY_WHEEL_UP] = plat_now_ns() + WHEEL_PRESS_NS;
            break;
        }
}

// ---- mouse: captured (relative mode) after a click in the window, Esc lets it go. In a hunt its movement turns the
// camera itself, a fixed angle per count (runtime/hle/mousecam.c). Elsewhere it is the right stick: the speed it moves
// at, smoothed over a few milliseconds, as the stick's tilt (full tilt at MOUSE_FULL_SPEED counts a second, times the
// settings' MouseSensitivity). Its buttons press what [Keybinds] says. While the HUD editor is open (a desktop's:
// runtime/touch.cpp) the mouse is its own instead: free and shown, its clicks and wheel the editor's, Esc cancels it.
#define MOUSE_FULL_SPEED 1500.0f
#define MOUSE_SMOOTH_S 0.025f  // (the averaging time: mice report at 125-8000 Hz, the game reads at its frame rate)
// The camera ignores the right stick below about 39% tilt (any direction: a round dead zone; measured 2026-10-23, 38%
// still, 40% turning slowly, then faster up to full tilt): the mouse's tilt starts there, so slow movements turn too.
#define MOUSE_DEADZONE 0.40f
static bool mouse_grab;       // the player's choice: clicked in (true) or Esc / focus lost (false)
static bool mouse_captured;   // relative mode on now (grabbed, and no Forge menu open)
static bool mouse_no_capture; // relative mode refused (WSLg, some Wayland compositors): clicks are buttons, no mouse look
static float mouse_dx, mouse_dy;  // counts moved since the last update
static uint64_t touch_cam_ns;  // when a finger last turned the camera (runtime/touch.cpp)
// (tests: the control file's "mousemove <x> <y> <ms>", a mouse moving at that many counts a second, captured or not)
static float test_vx, test_vy;
static uint64_t test_until;
static float gyro_test_r, gyro_test_u;  // (control "gyro <right> <up> <ms>": a turn of that many degrees a second)
static uint64_t gyro_test_until;

// ---- scripted input
typedef struct { uint64_t start, len; uint64_t buttons; int stick, x, y; } ScriptStep;
#define MAX_STEPS 4096
static ScriptStep script[MAX_STEPS];
static int nscript = -1;
static uint64_t script_end;  // (the presented frame the last step ends at: host_input_busy)
static bool out_busy;         // (the last update gave the game a button or a stick: host_input_busy)

static const char *const button_names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                           "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};

// One step: BUTTON (or STICK_<dir> / RSTICK_<dir>) held from presented frame `start` for `len` frames.
static bool add_step(const char *name, uint64_t start, uint64_t len) {
    ScriptStep st = {start, len, 0, -1, 0, 0};
    const char *dir = NULL;
    if (!strncmp(name, "STICK_", 6)) st.stick = 0, dir = name + 6;
    else if (!strncmp(name, "RSTICK_", 7)) st.stick = 1, dir = name + 7;
    if (dir) {
        // UP, DOWN, LEFT, RIGHT or a diagonal (UPLEFT, DOWNRIGHT...)
        int dx = strstr(dir, "LEFT") ? -1 : strstr(dir, "RIGHT") ? 1 : 0, dy = strstr(dir, "DOWN") ? -1 : strstr(dir, "UP") ? 1 : 0;
        int m = dx && dy ? 23170 : 32767;  // (diagonals: full deflection, not beyond the circle)
        st.x = dx * m, st.y = dy * m;
    } else {
        int b = -1;
        for (int i = 0; i < 16; i++) if (!strcmp(name, button_names[i])) b = i;
        if (b < 0) return false;
        st.buttons = B(b);
    }
    if (nscript == MAX_STEPS) {  // (control file sessions: drop the steps that are over)
        int k = 0;
        for (int i = 0; i < nscript; i++) if (script[i].start + script[i].len > g_present_count) script[k++] = script[i];
        nscript = k;
        if (nscript == MAX_STEPS) return false;
    }
    script[nscript++] = st;
    if (st.start + st.len > script_end) script_end = st.start + st.len;
    return true;
}

// Press BUTTON (as in MHGU_INPUT) from the next presented frame for `frames` frames. UI thread only (with
// host_input_update), like the control file.
bool host_input_press(const char *button, unsigned frames) {
    return add_step(button, g_present_count + 1, frames);
}

static void script_parse(void) {
    nscript = 0;
    const char *s = getenv("MHGU_INPUT");
    if (!s) return;
    char *copy = strdup(s), *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char name[32];
        unsigned long long start, len;
        if (sscanf(tok, "%llu:%31[^:]:%llu", &start, name, &len) != 3 || !add_step(name, start, len))
            LOG("[input] bad MHGU_INPUT step '%s'\n", tok);
    }
    free(copy);
    LOG("[input] scripted input: %d steps\n", nscript);
}

// MHGU_CONTROL=<file>: commands appended to the file while the game runs (one per line), for driving it from outside:
//   <BUTTON> [frames] [delay]  press now, or that many presented frames from now (default 8 frames); STICK_UP 30 etc.
//                          as in MHGU_INPUT. Lines written together start from the same frame: a timed sequence.
//   shot <name>            save the next presented frame as shots/<name>.png
//   burst <n> <name>       save the next n presented frames as shots/<name>_<i>.png
//   dump [n]               save colour target 0 (or the depth of a depth-only pass) of every render pass of the next
//                          frame (shots/pass_*.png), or of the next n frames (shots/f<i>_pass_*.png for i >= 1)
//   hudtrace               log every HUD quad of the next frame: its layer, place, and where 16:10 / ultrawide moved it;
//                          and each pass (targets, stand-ins, area), draw (program, viewport, scissor, textures), copy and dispatch
//   frame                  log the current frame number
//   type <text>            type into the game's text entry (runtime/textinput.c); textkey enter|escape|backspace
//   mousemove <x> <y> <ms> mouse look as if the mouse moved at that many counts a second (y down) for that long
//   wheel <notches>        the mouse wheel turned that far (> 0 up; by default the item bar to the left)
//   mousedown / mouseup <x> <y>, mousedrag <x> <y>, mousewheel <x> <y> <notches>: the left button held / let go at
//                          (x, y) in the window, the pointer moved there with it held, the wheel turned over it
//   mouse <x> <y> click|right|middle|x1|x2: the pointer to (x, y), that button pressed and let go there
//   menutrace [updates]    log the menus' cursors, where their rows and cells are, and the pointer (hle/mousemenu.c)
//   plpos                  log the local player's place and heading (a scripted walk in a town)
//   menupress <hex>        those buttons in the menus' pressed word for one update, a menu open or not (2000: A;
//                          a test of what reads it: hle/mousemenu.c)
//   menustall <ms>         the game's next update waits that long (a slow update in a menu: hle/mousemenu.c)
//   hudedit                the HUD editor opens, as Move the HUD does (a desktop's: runtime/touch.cpp)
//   finger <n> down|move|up|cancel <x> <y> [ms]: a finger on the screen (n: which one, 0..15; x, y: parts of the
//                          window's width and height), now or that many ms from now (tests: the touch controls)
//   touchedit              the touch layout editor opens, as Customize controls and HUD does (runtime/touch.cpp)
//   fakewindow <ms>        the touch controls see a window of the game's menus drawn for that long (a chat bubble: a
//                          window that comes by itself; runtime/touch.cpp)
char g_shot_request[256];  // (runtime/nvn/vulkan/device.c saves it)
// (tests) the fingers' events waiting for their time: pushed to SDL's queue then, as a touch screen's
static struct {
    uint64_t due;
    SDL_Event e;
} finger_wait[32];
static void finger_due(void) {
    uint64_t now = plat_now_ns();
    for (int i = 0; i < 32; i++)
        if (finger_wait[i].due && now >= finger_wait[i].due) {
            SDL_Event e = finger_wait[i].e;
            e.tfinger.timestamp = SDL_GetTicksNS();
            finger_wait[i].due = 0;
            SDL_PushEvent(&e);
        }
}
static void finger_command(const char *line) {
    int n = -1, ms = 0;
    char what[16] = "";
    float x = 0, y = 0;
    if (sscanf(line, "%*s %d %15s %f %f %d", &n, what, &x, &y, &ms) < 4 || n < 0 || n > 15) {
        LOG("[control] finger: bad line %s", line);
        return;
    }
    Uint32 type = !strcmp(what, "down") ? SDL_EVENT_FINGER_DOWN : !strcmp(what, "move") ? SDL_EVENT_FINGER_MOTION
                : !strcmp(what, "up") ? SDL_EVENT_FINGER_UP : !strcmp(what, "cancel") ? SDL_EVENT_FINGER_CANCELED : 0;
    if (!type) { LOG("[control] finger: down, move, up or cancel, not '%s'\n", what); return; }
    SDL_Event e = {0};
    e.tfinger.type = type, e.tfinger.touchID = 1, e.tfinger.fingerID = 1000 + n;
    e.tfinger.x = x, e.tfinger.y = y, e.tfinger.pressure = type == SDL_EVENT_FINGER_UP ? 0.0f : 1.0f;
    for (int i = 0; i < 32; i++)
        if (!finger_wait[i].due) {
            finger_wait[i].due = plat_now_ns() + (uint64_t)(ms > 0 ? ms : 0) * 1000000ull;
            finger_wait[i].e = e;
            return;
        }
    LOG("[control] finger: too many waiting\n");
}
char g_burst_name[200];
int g_burst_left, g_burst_index;
volatile int g_dump_request;
volatile int g_hudtrace_request;  // (debug) "hudtrace": nvn/vulkan/exec.c logs the next frame's HUD quads
static void control_poll(void) {
    static const char *path = (const char *)1;
    static long done;
    static uint64_t last_ns;
    if (path == (const char *)1) path = getenv("MHGU_CONTROL");
    if (!path) return;
    uint64_t now = plat_now_ns();
    if (now - last_ns < 50000000ull) return;
    last_ns = now;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < done) done = 0;  // (file replaced)
    if (size > done) {
        fseek(f, done, SEEK_SET);
        char line[300];
        while (ftell(f) < size && fgets(line, sizeof line, f)) {
            if (!strchr(line, '\n') && ftell(f) >= size) break;  // (partial last line: next time)
            done = ftell(f);
            char cmd[32] = "", arg[256] = "", arg2[200] = "";
            if (sscanf(line, "%31s %255s %199s", cmd, arg, arg2) < 1) continue;
            if (!strcmp(cmd, "crash")) raise(SIGILL);  // (tests: the error screen, runtime/platform/android.c)
            if (!strcmp(cmd, "msaa")) { void vk_set_msaa(uint32_t); vk_set_msaa((uint32_t)atoi(arg)); continue; }
#ifdef __ANDROID__
            if (!strcmp(cmd, "prerotate")) {  // prerotate 0|1: the window image drawn turned for the panel or not, the
                extern int g_prerotate_test;  // swapchain made again (tests: nvn/vulkan/device.c MHGU_PERF_PREROTATE, the
                g_prerotate_test = atoi(arg) ? 1 : 0;  // same held frame both ways)
                continue;
            }
#endif
            if (!strcmp(cmd, "gpuprof")) { extern volatile int g_gpuprof_request; g_gpuprof_request = 1; continue; }
            if (!strcmp(cmd, "devicelost")) { extern volatile int g_fake_lost; g_fake_lost = 1; continue; }  // (tests: exec.c lost_report)
            if (!strcmp(cmd, "hudtrace")) { g_hudtrace_request = 1; continue; }
            if (!strcmp(cmd, "menutrace")) { mousemenu_trace(atoi(arg)); continue; }  // (hle/mousemenu.c)
            if (!strcmp(cmd, "menupress")) { mousemenu_test_press((uint32_t)strtoul(arg, NULL, 16)); continue; }
            if (!strcmp(cmd, "menustall")) { mousemenu_test_stall(atoi(arg)); continue; }
            if (!strcmp(cmd, "dump")) { g_dump_request = atoi(arg) > 0 ? atoi(arg) : 1; continue; }
            if (!strcmp(cmd, "memsave")) {  // memsave <guest hex address> <hex bytes> <file>: raw guest memory to a file
                char path[200] = "";
                sscanf(line, "%*s %*s %*s %199s", path);
                uint32_t a = (uint32_t)strtoul(arg, NULL, 16), n = (uint32_t)strtoul(arg2, NULL, 16);
                FILE *o = path[0] && n <= (16u << 20) ? fopen(path, "wb") : NULL;
                if (o) { fwrite(G2H(a), 1, n, o); fclose(o); }
                LOG("[memsave] %08x+%x -> %s%s\n", a, n, path, o ? "" : " (failed)");
                continue;
            }
            if (!strcmp(cmd, "testhook")) {  // testhook <hex address>: a Forge plugin's kind of hook there (forge.c)
                forge_test_hook((uint32_t)strtoul(arg, NULL, 16));
                continue;
            }
            if (!strcmp(cmd, "interpwatch")) {  // interpwatch <guest hex addr> <file of hex entries>: run those game
                // functions (and all game code they call) in the interpreter, logging what changes the word at addr
                void interp_watch(uint32_t, int);
                void interp_add_range(uint32_t, uint32_t);
                int dispatch_interpret(uint32_t, uint32_t);
                FILE *fl = fopen(arg2, "r");
                char fn[32];
                int n = 0;
                interp_add_range(0x200000, 0x1800000);
                while (fl && fgets(fn, sizeof fn, fl)) {
                    uint32_t e = (uint32_t)strtoul(fn, NULL, 16);
                    if (e && dispatch_interpret(e, e + 4) > 0) n++;
                }
                if (fl) fclose(fl);
                interp_watch((uint32_t)strtoul(arg, NULL, 16), 40);
                LOG("[watch] %s: %d functions interpreted\n", arg, n);
                continue;
            }
            if (!strcmp(cmd, "interpread")) {  // interpread <lo hex>:<hi hex> <file of hex entries>: as interpwatch, but
                // logging the instructions that read the range (once each)
                void interp_read_watch(uint32_t, uint32_t);
                void interp_add_range(uint32_t, uint32_t);
                int dispatch_interpret(uint32_t, uint32_t);
                FILE *fl = fopen(arg2, "r");
                char fn[32];
                int n = 0;
                interp_add_range(0x200000, 0x1800000);
                while (fl && fgets(fn, sizeof fn, fl)) {
                    uint32_t e = (uint32_t)strtoul(fn, NULL, 16);
                    int r = e ? dispatch_interpret(e, e + 4) : -2;
                    if (r > 0) n++;
                    else {
                        extern GuestFn dispatch_lookup(uint32_t);
                        LOG("[readwatch] %x: not interpreted (%d, lookup %p)\n", e, r, (void *)dispatch_lookup(e));
                    }
                }
                if (!fl) LOG("[readwatch] can't open %s\n", arg2);
                if (fl) fclose(fl);
                const char *colon = strchr(arg, ':');
                interp_read_watch((uint32_t)strtoul(arg, NULL, 16), colon ? (uint32_t)strtoul(colon + 1, NULL, 16) : 0);
                LOG("[readwatch] %s: %d functions interpreted\n", arg, n);
                continue;
            }
            if (!strcmp(cmd, "emact")) {  // emact <group> <index> [mode]: start a monster action (hle/emtrace.c)
                void emtrace_request(uint32_t, uint32_t, uint32_t);
                char arg3[16] = "";
                sscanf(line, "%*s %*s %*s %15s", arg3);
                emtrace_request((uint32_t)atoi(arg), (uint32_t)atoi(arg2), (uint32_t)atoi(arg3));
                continue;
            }
            if (!strcmp(cmd, "emsweep")) {  // emsweep <group> <first> <last>: each monster action in turn (emtrace.c)
                void emtrace_sweep(uint32_t, uint32_t, uint32_t);
                char arg3[16] = "";
                sscanf(line, "%*s %*s %*s %15s", arg3);
                emtrace_sweep((uint32_t)atoi(arg), (uint32_t)atoi(arg2), (uint32_t)atoi(arg3));
                continue;
            }
            if (!strcmp(cmd, "plact") || !strcmp(cmd, "plsweep")) {  // plact <act_st hex> <act_no> [param] /
                // plsweep <act_st hex> <first> <last>: start player actions (MHGU_LOG_MOVES, hle/movelog.c)
                void movelog_request(uint32_t, uint32_t, uint32_t), movelog_sweep(uint32_t, uint32_t, uint32_t);
                char arg3[16] = "";
                sscanf(line, "%*s %*s %*s %15s", arg3);
                (cmd[2] == 'a' ? movelog_request : movelog_sweep)((uint32_t)strtoul(arg, NULL, 16), (uint32_t)atoi(arg2),
                                                                  (uint32_t)atoi(arg3));
                continue;
            }
            if (!strcmp(cmd, "plsnap")) {  // plsnap [updates]: the player's memory every update (MHGU_PLSNAP, movelog.c)
                void movelog_snap(int);
                movelog_snap(atoi(arg));
                continue;
            }
            if (!strcmp(cmd, "plpos")) {  // plpos: log the local player's place and heading (tests: a scripted walk in a
                // town, frame by frame with MHGU_FPS_LIMIT=60: the house's item box; writing the place does not move it)
                uint32_t gc = LD32(0x1a8ca48u), sp = LD32(0x1a85b58u);  // (sGameControl, sPlayer: hle/weapon.c)
                uint32_t slot = gc ? LD32(gc + 0x30) : 9, pl = sp && slot < 4 ? LD32(sp + 0x1c + 4u * slot) : 0;
                if (pl < 0x08000000u || (pl & 3)) { LOG("[plpos] no player\n"); continue; }
                LOG("[plpos] player %08x (vt %08x) at %.1f %.1f %.1f heading %u\n", pl, LD32(pl), LDF(pl + 0xa0), LDF(pl + 0xa4),
                    LDF(pl + 0xa8), LD32(pl + 0x14a0) & 0xffff);
                continue;
            }
            if (!strcmp(cmd, "poke")) {  // poke <guest hex address> <hex word>: write guest memory (reverse engineering)
                uint32_t a = (uint32_t)strtoul(arg, NULL, 16) & ~3u, v = (uint32_t)strtoul(arg2, NULL, 16);
                LOG("[poke] %08x: %08x -> %08x\n", a, LD32(a), v);
                ST32(a, v);
                continue;
            }
            if (!strcmp(cmd, "fntrace")) {  // fntrace <lo hex> <hi hex> [n] [r0 lo hex] [r0 hi hex]: log the next n calls of
                                            // guest functions there (only those with r0 in [r0 lo, r0 hi) when given)
                void fntrace_set(uint32_t, uint32_t, int, uint32_t, uint32_t);
                char arg3[16] = "", arg4[16] = "", arg5[16] = "";
                sscanf(line, "%*s %*s %*s %15s %15s %15s", arg3, arg4, arg5);
                fntrace_set((uint32_t)strtoul(arg, NULL, 16), (uint32_t)strtoul(arg2, NULL, 16), arg3[0] ? atoi(arg3) : 200,
                            (uint32_t)strtoul(arg4, NULL, 16), (uint32_t)strtoul(arg5, NULL, 16));
                continue;
            }
            if (!strcmp(cmd, "fnnew")) {  // fnnew <lo hex> <hi hex> [learn ms]: log first calls only (dispatch.c)
                void fnnew_set(uint32_t, uint32_t, int);
                char arg3[16] = "";
                sscanf(line, "%*s %*s %*s %15s", arg3);
                fnnew_set((uint32_t)strtoul(arg, NULL, 16), (uint32_t)strtoul(arg2, NULL, 16), atoi(arg3));
                continue;
            }
            if (!strcmp(cmd, "find")) {  // find <hex bytes>: log where they are in committed guest memory (first 32)
                uint8_t pat[100];
                int np = 0;
                for (const char *q = arg; q[0] && q[1] && np < 100; q += 2) {
                    char hx[3] = {q[0], q[1], 0};
                    pat[np++] = (uint8_t)strtoul(hx, NULL, 16);
                }
                int hits = 0;
                for (uint64_t a = 0x1000; np && a < GUEST_SPACE && hits < 32;) {
                    uint64_t end;
                    bool ok = plat_committed(GUEST_BASE + a, &end);
                    end -= GUEST_BASE;
                    if (end > GUEST_SPACE) end = GUEST_SPACE;
                    if (ok && end - a >= (uint64_t)np) {
                        const uint8_t *m = G2H((uint32_t)a);
                        for (uint64_t i = 0; i + np <= end - a && hits < 32; i++)
                            if (m[i] == pat[0] && !memcmp(m + i, pat, np)) LOG("[find] %08llx\n", (unsigned long long)(a + i)), hits++;
                    }
                    a = end;
                }
                LOG("[find] %d hit(s)\n", hits);
                continue;
            }
            if (!strcmp(cmd, "peek")) {  // peek <guest hex address> [words]: log guest memory (reverse engineering)
                uint32_t a = (uint32_t)strtoul(arg, NULL, 16) & ~3u;
                int nw = arg2[0] ? atoi(arg2) : 8;
                for (int k = 0; k < nw && k < 256; k += 4) {
                    char line[96];
                    int len = snprintf(line, sizeof line, "[peek] %08x:", a + 4 * k);
                    for (int j = k; j < nw && j < k + 4; j++) len += snprintf(line + len, sizeof line - len, " %08x", LD32(a + 4 * j));
                    LOG("%s\n", line);
                }
                continue;
            }
            if (!strcmp(cmd, "burst")) {
                snprintf(g_burst_name, sizeof g_burst_name, "%s", arg2[0] ? arg2 : "burst");
                g_burst_index = 0;
                g_burst_left = atoi(arg) > 0 ? atoi(arg) : 4;
                continue;
            }
            if (!strcmp(cmd, "type")) {
                char *t = line + strspn(line, " \t") + 4;
                t += strspn(t, " \t");
                t[strcspn(t, "\r\n")] = 0;
                textinput_type(t);
                continue;
            }
            if (!strcmp(cmd, "sdlquit")) { // Same path as the window close button / Alt+F4 (runtime tests).
                SDL_Event event = {0}; event.type = SDL_EVENT_QUIT; SDL_PushEvent(&event);
                continue;
            }
#ifndef __ANDROID__
            if (!strcmp(cmd, "pick")) {  // pick folder|save|export|cheats|mod <path>: a desktop dialog's answer (pc_actions.c)
                void pc_test_pick(const char *what, const char *path);
                pc_test_pick(arg, arg2);
                continue;
            }
#endif
            if (!strcmp(cmd, "sdlkey") || !strcmp(cmd, "sdlkeydown") || !strcmp(cmd, "sdlkeyup")) {
                // sdlkey [ctrl+|alt+|shift+]<SDL key name>: a key pressed and released, with that modifier held around it
                // (Insert: Forge's menu; "sdlkey Left Alt": Alt alone, a tap). sdlkeydown / sdlkeyup: only pressed or
                // only let go, to hold a key ("sdlkeydown I", later "sdlkeyup I": the camera turns meanwhile)
                static const struct { const char *prefix; SDL_Scancode sc; SDL_Keycode key; SDL_Keymod mod; } mods[] = {
                    {"ctrl+", SDL_SCANCODE_LCTRL, SDLK_LCTRL, SDL_KMOD_LCTRL}, {"alt+", SDL_SCANCODE_LALT, SDLK_LALT, SDL_KMOD_LALT},
                    {"shift+", SDL_SCANCODE_LSHIFT, SDLK_LSHIFT, SDL_KMOD_LSHIFT}};
                bool press = cmd[6] != 'u', release = cmd[6] != 'd';
                char *text = line + strspn(line, " \t") + strlen(cmd), *name;  // (the rest of the line: names have spaces)
                text += strspn(text, " \t");
                text[strcspn(text, "\r\n")] = 0;
                int m = -1;
                for (int i = 0; i < 3; i++)
                    if (!SDL_strncasecmp(text, mods[i].prefix, strlen(mods[i].prefix))) m = i;
                name = text + (m >= 0 ? strlen(mods[m].prefix) : 0);
                SDL_Scancode sc = SDL_GetScancodeFromName(name);
                SDL_Event e = {0};
                e.key.type = SDL_EVENT_KEY_DOWN, e.key.down = true;
                if (press && m >= 0) {  // (the modifier first, as a keyboard sends it)
                    e.key.scancode = mods[m].sc, e.key.key = mods[m].key, e.key.mod = mods[m].mod;
                    SDL_PushEvent(&e);
                }
                e.key.scancode = sc, e.key.key = SDL_GetKeyFromScancode(sc, 0, false), e.key.mod = m >= 0 ? mods[m].mod : 0;
                if (press) SDL_PushEvent(&e);
                e.key.type = SDL_EVENT_KEY_UP, e.key.down = false;
                if (release) SDL_PushEvent(&e);
                if (release && m >= 0) {  // (then the modifier's own release: none left)
                    e.key.scancode = mods[m].sc, e.key.key = mods[m].key, e.key.mod = 0;
                    SDL_PushEvent(&e);
                }
                LOG("[control] key %s%s (%d)\n", text, !release ? " down" : !press ? " up" : "", (int)sc);
                continue;
            }
            if (!strcmp(cmd, "vpad")) {  // vpad: attach an SDL virtual controller (tests: picking the controller)
                static SDL_Joystick *vpads[4];
                static int nv;
                if (!strcmp(arg, "") && nv < 4) {
                    SDL_VirtualJoystickDesc d;
                    SDL_INIT_INTERFACE(&d);
                    d.type = SDL_JOYSTICK_TYPE_GAMEPAD, d.naxes = SDL_GAMEPAD_AXIS_COUNT, d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
                    d.name = nv ? "Virtual pad 2" : "Virtual pad 1";
                    d.vendor_id = 0x1234, d.product_id = (Uint16)(nv + 1);  // (SDL names it by its mapping: one per id)
                    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
                    vpads[nv] = id ? SDL_OpenJoystick(id) : NULL;
                    LOG("[control] vpad %d: %s\n", nv, vpads[nv] ? "attached" : SDL_GetError());
                    nv++;
                } else {  // vpad <n> <SDL button number> <0|1>: that button of virtual controller n up or down
                    int n = atoi(arg), down = 0, btn = atoi(arg2);
                    sscanf(line, "%*s %*s %*s %d", &down);
                    if (n >= 0 && n < nv && vpads[n]) SDL_SetJoystickVirtualButton(vpads[n], btn, down != 0);
                }
                continue;
            }
            if (!strcmp(cmd, "sdltext")) {  // sdltext <text>: typed text, as the keyboard sends it (not the game's entry)
                static char text[256];
                snprintf(text, sizeof text, "%s", arg);
                SDL_Event e = {0};
                e.text.type = SDL_EVENT_TEXT_INPUT, e.text.text = text;
                SDL_PushEvent(&e);
                continue;
            }
            if (!strcmp(cmd, "mouse")) {  // mouse <x> <y> [click|right|middle|x1|x2]: the pointer to (x, y), clicked there
                char arg3[16] = "";
                sscanf(line, "%*s %*s %*s %15s", arg3);
                SDL_Event e = {0};
                e.motion.type = SDL_EVENT_MOUSE_MOTION, e.motion.x = (float)atof(arg), e.motion.y = (float)atof(arg2);
                static float last_x = -1, last_y;  // (moved from there, as a mouse's motion says: input_seen)
                e.motion.xrel = last_x < 0 ? 10 : e.motion.x - last_x, e.motion.yrel = last_x < 0 ? 0 : e.motion.y - last_y;
                last_x = e.motion.x, last_y = e.motion.y;
                SDL_PushEvent(&e);
                int button = !strcmp(arg3, "click") ? SDL_BUTTON_LEFT : !strcmp(arg3, "right") ? SDL_BUTTON_RIGHT
                           : !strcmp(arg3, "middle") ? SDL_BUTTON_MIDDLE : !strcmp(arg3, "x1") ? SDL_BUTTON_X1
                           : !strcmp(arg3, "x2") ? SDL_BUTTON_X2 : 0;
                if (button) {
                    SDL_Event b = {0};
                    b.button.type = SDL_EVENT_MOUSE_BUTTON_DOWN, b.button.button = (Uint8)button, b.button.down = true;
                    b.button.x = e.motion.x, b.button.y = e.motion.y, b.button.clicks = 1;
                    SDL_PushEvent(&b);
                    b.button.type = SDL_EVENT_MOUSE_BUTTON_UP, b.button.down = false;
                    SDL_PushEvent(&b);
                }
                continue;
            }
            // mousedown / mouseup <x> <y>: the left button held / let go there; mousedrag <x> <y>: the pointer moved
            // there with it held; mousewheel <x> <y> <notches>: the wheel turned over (x, y) (tests: the HUD editor)
            if (!strcmp(cmd, "mousedown") || !strcmp(cmd, "mouseup")) {
                SDL_Event b = {0};
                bool down = !strcmp(cmd, "mousedown");
                b.button.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                b.button.button = SDL_BUTTON_LEFT, b.button.down = down, b.button.clicks = 1;
                b.button.x = (float)atof(arg), b.button.y = (float)atof(arg2);
                SDL_PushEvent(&b);
                continue;
            }
            if (!strcmp(cmd, "mousedrag")) {
                SDL_Event e = {0};
                e.motion.type = SDL_EVENT_MOUSE_MOTION, e.motion.state = SDL_BUTTON_LMASK;
                e.motion.x = (float)atof(arg), e.motion.y = (float)atof(arg2);
                SDL_PushEvent(&e);
                continue;
            }
            if (!strcmp(cmd, "mousewheel")) {
                char arg3[16] = "";
                sscanf(line, "%*s %*s %*s %15s", arg3);
                SDL_Event w = {0};
                w.wheel.type = SDL_EVENT_MOUSE_WHEEL, w.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
                w.wheel.mouse_x = (float)atof(arg), w.wheel.mouse_y = (float)atof(arg2);
                w.wheel.integer_y = atoi(arg3), w.wheel.y = (float)atoi(arg3);
                SDL_PushEvent(&w);
                continue;
            }
#ifndef __ANDROID__
            if (!strcmp(cmd, "hudedit")) {  // hudedit: the HUD editor opens, as Move the HUD does from the menu (tests)
                touch_hud_edit_begin();
                continue;
            }
#endif
            if (!strcmp(cmd, "finger")) { finger_command(line); continue; }
            if (!strcmp(cmd, "touchedit")) { touch_edit_begin(); continue; }  // (as from the settings menu: once it is closed)
            if (!strcmp(cmd, "fakewindow")) { touch_test_window(atoi(arg)); continue; }  // fakewindow <ms> (runtime/touch.cpp)
            if (!strcmp(cmd, "mousemove")) {  // mousemove <x> <y> <ms>: mouse look at that speed (counts a second)
                char arg3[16] = "";
                sscanf(line, "%*s %*s %*s %15s", arg3);
                test_vx = (float)atof(arg), test_vy = (float)atof(arg2);
                test_until = plat_now_ns() + (uint64_t)atoi(arg3) * 1000000ull;
                continue;
            }
            if (!strcmp(cmd, "gyro")) {  // gyro <right> <up> <ms>: the gyroscope turned that fast (degrees a second)
                char arg3[16] = "";
                sscanf(line, "%*s %*s %*s %15s", arg3);
                gyro_test_r = (float)atof(arg), gyro_test_u = (float)atof(arg2);
                gyro_test_until = plat_now_ns() + (uint64_t)atoi(arg3) * 1000000ull;
                continue;
            }
            if (!strcmp(cmd, "wheel")) {  // wheel <notches>: the mouse wheel turned (< 0 down), captured or not
                if (atoi(arg)) wheel_turn(atoi(arg));
                continue;
            }
            if (!strcmp(cmd, "textkey")) {
                textinput_key(!strcmp(arg, "enter") ? TI_ENTER : !strcmp(arg, "escape") ? TI_CANCEL : TI_NONE);
                continue;
            }
            if (!strcmp(cmd, "shot")) {
                snprintf(g_shot_request, sizeof g_shot_request, "shots/%s.png", arg[0] ? arg : "control");
            } else if (!strcmp(cmd, "frame")) {
                LOG("[control] frame %llu\n", (unsigned long long)g_present_count);
            } else {
                unsigned long n = arg[0] ? strtoul(arg, NULL, 10) : 8, delay = strtoul(arg2, NULL, 10);
                uint64_t at = g_present_count + 1 + delay;
                if (!add_step(cmd, at, n ? n : 8)) LOG("[control] unknown command '%s'\n", cmd);
                else LOG("[control] %s %lu at frame %llu\n", cmd, n ? n : 8, (unsigned long long)at);
            }
        }
    }
    fclose(f);
}
// ---- events
// Which controller the game reads. Every one is open, and a button pressed on one makes it the one read: the first
// one SDL listed was not always the one in the player's hands (a Steam Deck's own controls and a controller plugged
// into it, or Steam Input's virtual controller beside the real one). SDL calls other devices a gamepad as well (a
// Steam Controller's puck, an analog keyboard's mouse interface), which would read as stuck buttons and sticks: so
// every device and every change is logged, and MHGU_PAD=<part of the name, case-insensitive> says which one to use.
static bool pad_matches(SDL_JoystickID id) {
    const char *sel = getenv("MHGU_PAD");
    if (!sel || !*sel) return true;
    unsigned vid = 0, pid = 0;  // "28de:1142" matches the ids, anything else part of the name
    if (sscanf(sel, "%4x:%4x", &vid, &pid) == 2)
        return SDL_GetGamepadVendorForID(id) == vid && SDL_GetGamepadProductForID(id) == pid;
    const char *name = SDL_GetGamepadNameForID(id);
    return name && SDL_strcasestr(name, sel);
}

static void log_pad(SDL_JoystickID id, const char *tag) {
    const char *name = SDL_GetGamepadNameForID(id);
    LOG("[input] %s: %s (%04x:%04x), SDL mapping %s\n", tag, name ? name : "?", SDL_GetGamepadVendorForID(id),
        SDL_GetGamepadProductForID(id), SDL_GetGamepadMappingForID(id) ? "known" : "NONE (SDL guesses the buttons)");
}

static int find_pad(SDL_JoystickID id) {
    for (int i = 0; i < npads; i++)
        if (SDL_GetGamepadID(pads[i].pad) == id) return i;
    return -1;
}

// A controller in the player's hands hides the touch controls (runtime/touch.cpp). Android calls other devices a
// gamepad too: the emulator's keyboard ("qwerty2"), virtual and uinput devices, a phone's GPIO keys, keypads, and the
// media keys of a USB or Bluetooth device ("... Consumer Control"; seen: a BSP-D8).
static bool pad_is_real(SDL_Gamepad *p) {
    static const char *const not_pads[] = {"qwerty", "virtual", "uinput", "gpio", "keypad", "consumer control"};
    const char *name = SDL_GetGamepadName(p);
    if (!name) return true;  // (no name: SDL still maps it as a gamepad)
    for (unsigned i = 0; i < sizeof not_pads / sizeof *not_pads; i++)
        if (SDL_strcasestr(name, not_pads[i])) return false;
    return true;
}

static bool real_pad_connected(void) {  // (from the open pads every call: follows ADDED / REMOVED live)
    for (int i = 0; i < npads; i++)
        if (pads[i].real) return true;
    return false;
}
static SDL_AtomicInt real_pad_seen;  // (host_input_real_pad: real_pad_connected at the last update, for other threads)
bool host_input_real_pad(void) { return SDL_GetAtomicInt(&real_pad_seen) != 0; }

static void open_pad(SDL_JoystickID id) {
    if (find_pad(id) >= 0) return;
    if (!pad_matches(id)) { log_pad(id, "gamepad skipped (MHGU_PAD)"); return; }
    if (npads == MAX_PADS) { log_pad(id, "gamepad skipped (too many)"); return; }
    SDL_Gamepad *p = SDL_OpenGamepad(id);
    if (!p) { LOG("[input] gamepad %u failed to open: %s\n", (unsigned)id, SDL_GetError()); return; }
    pads[npads].pad = p, pads[npads].real = pad_is_real(p) || padmap_custom(id), pads[npads++].opened_ns = plat_now_ns();
    if (!pad) pad = p, log_pad(id, "gamepad in use");
    else log_pad(id, "gamepad open too (a button pressed on it makes it the one in use)");
    const char *name = SDL_GetGamepadName(p);
    if (pads[npads - 1].real) LOG("[input] %s counts as a controller: touch controls hidden\n", name ? name : "?");
    else LOG("[input] %s: not a controller for the touch controls\n", name ? name : "?");
}

// A button pressed on another open controller: that one is read from now on. Not in the first half second after it
// opened (a device whose buttons read as held sends their presses then, and would take over at startup).
static void pad_pressed(SDL_JoystickID id) {
    int i = find_pad(id);
    if (i < 0 || pads[i].pad == pad || plat_now_ns() - pads[i].opened_ns < 500000000ull) return;
    pad = pads[i].pad;
    log_pad(id, "gamepad in use (a button pressed on it)");
}

static void close_pad(SDL_JoystickID id) {
    int i = find_pad(id);
    if (i < 0) return;
    bool was = pads[i].pad == pad;
    const char *name = SDL_GetGamepadName(pads[i].pad);  // (its id is gone already: log_pad would find nothing)
    LOG("[input] gamepad removed%s: %s\n", was ? " (it was in use)" : "", name ? name : "?");
    bool real = pads[i].real;
    if (pads[i].pad == gyro_pad) gyro_pad = NULL;  // (its gyro was read: gone with it)
    SDL_CloseGamepad(pads[i].pad);
    pads[i] = pads[--npads];
    if (!real) LOG("[input] (it was not a controller for the touch controls)\n");
    else if (real_pad_connected()) LOG("[input] it counted as a controller: touch controls stay hidden (another one)\n");
    else LOG("[input] it counted as a controller: touch controls shown again\n");
    if (was) {
        pad = npads ? pads[0].pad : NULL;
        if (pad) log_pad(SDL_GetGamepadID(pad), "gamepad in use");
    }
}

// A joystick SDL has no gamepad mapping for (seen: generic Bluetooth pads that only report BUTTON_1 .. 16) is not read
// at all until the player maps it, and nothing said so: many said their controller "isn't read". So it is logged, and
// a notice says where to map it (Controls, Map a controller: runtime/padmap.c), once per controller and session
static void joystick_seen(SDL_JoystickID id) {
    if (SDL_IsGamepad(id)) return;
    static SDL_JoystickID told[16];
    static int ntold;
    for (int i = 0; i < ntold; i++)
        if (told[i] == id) return;
    if (ntold < 16) told[ntold++] = id;
    const char *name = SDL_GetJoystickNameForID(id);
    LOG("[input] joystick %s (%04x:%04x) has no gamepad mapping: not read until it is mapped (Controls, Map a controller)\n",
        name ? name : "?", SDL_GetJoystickVendorForID(id), SDL_GetJoystickProductForID(id));
    if (name && (SDL_strcasestr(name, "consumer control") || SDL_strcasestr(name, "virtual") || SDL_strcasestr(name, "uinput")))
        return;  // (not a controller in the player's hands: pad_is_real)
    bool es = config_language() == 1;
    char text[200];
    snprintf(text, sizeof text,
             es ? "%s: para usarlo, configúralo en el engranaje, Controles, Mapear un mando."
                : "%s: to use it, set it up under the gear, Controls, Map a controller.",
             name ? name : (es ? "Un mando" : "A controller"));
    overlay_notice(es ? "Mando sin configurar" : "Controller not set up", text);
}

static void pick_pad(void) {  // over every controller SDL sees now: those MHGU_PAD allows (all without it), first in use
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n; i++) open_pad(ids[i]);
    SDL_free(ids);
    ids = SDL_GetJoysticks(&n);
    for (int i = 0; i < n; i++) joystick_seen(ids[i]);
    SDL_free(ids);
}

// While a line of chat is typed the pointer shows, and dragging the box moves it (kept in chat_box.txt)
static bool chat_mouse(const SDL_Event *e) {
    static bool dragging;
    static float last_x, last_y;
    if (!chat_active()) {
        dragging = false;
        return false;
    }
    int w = 0, h = 0;
    SDL_Window *win = ui_window();
    if (win) SDL_GetWindowSize(win, &w, &h);
    if (w <= 0 || h <= 0) return false;
    switch (e->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button == SDL_BUTTON_LEFT && chat_in_box(e->button.x / w, e->button.y / h))
            dragging = true, last_x = e->button.x, last_y = e->button.y;
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        if (dragging) {
            chat_move((e->motion.x - last_x) / w, (e->motion.y - last_y) / h);
            last_x = e->motion.x, last_y = e->motion.y;
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (dragging && e->button.button == SDL_BUTTON_LEFT) {
            dragging = false;
            chat_save_position();
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        return true;
    }
    return false;
}

// ---- the game's menus and dialogs ([Controls] MouseMenus, runtime/hle/mousemenu.c): the mouse free and shown, its
// place given on the game's 1280x720 canvas, which covers the picture (the picture as wide as fits in its own shape,
// centred: device.c backend_present, touch.cpp picture()); the window's points, not its pixels (a scaled desktop)
static bool menu_mouse(void) {
    return mousemenu_active() && !overlay_menu_open() && !chat_active() && !textinput_active()
#ifndef __ANDROID__
           && !touch_hud_editing()
#endif
        ;
}
static bool canvas_point(float x, float y, float *cx, float *cy) {
    SDL_Window *w = ui_window();
    int ww = 0, wh = 0;
    if (!w || !SDL_GetWindowSize(w, &ww, &wh) || ww <= 0 || wh <= 0) return false;
    float a = g_render_aspect > 0 ? g_render_aspect : (float)ww / (float)wh, pw = (float)ww, ph = (float)ww / a;
    if (ph > (float)wh) ph = (float)wh, pw = (float)wh * a;
    *cx = (x - ((float)ww - pw) * 0.5f) * 1280.0f / pw, *cy = (y - ((float)wh - ph) * 0.5f) * 720.0f / ph;
    return true;
}
// The pointer hidden in a menu or a dialog until the mouse moves: a key or a controller was used there, or a controller
// last as it opened (a key outside one keeps it shown: the keyboard goes with the mouse; a controller hides it for the
// next one too), or as it closed a moment ago (a load between two windows: as it was)
static bool menu_pointer_hidden;
static uint64_t menu_left_ns;  // when the game's menus last let the mouse go
#define MENU_GAP_NS 3000000000ull
// The pointer in a menu: bronze and gold, outlined in wine-dark, as the game's own menus (a scaled desktop: bigger)
static SDL_Cursor *menu_cursor(void) {
    static SDL_Cursor *cur;
    static float made_for;
    SDL_Window *w = ui_window();
    float k = w ? SDL_GetWindowDisplayScale(w) : 1.0f;
    if (!(k >= 1.0f && k <= 4.0f)) k = 1.0f;
    if (made_for == k) return cur;  // (made for this scale; NULL if that failed: the system's arrow)
    made_for = k;
    int n = (int)(24.0f * k + 0.5f);
    SDL_Surface *s = SDL_CreateSurface(n, n, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return NULL;
    // the classic arrow: tip at (0, 0), its point down the left edge, the notch at its middle (units of n / 24)
    static const float px[] = {1.0f, 1.0f, 5.2f, 8.3f, 12.0f, 8.9f, 14.2f};
    static const float py[] = {1.0f, 18.5f, 14.6f, 21.8f, 20.2f, 13.4f, 13.4f};
    const int np = 7;
    float u = (float)n / 24.0f;
    uint32_t *pix = (uint32_t *)s->pixels;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float fx = ((float)x + 0.5f) / u, fy = ((float)y + 0.5f) / u;
            bool in = false;  // (inside the arrow: crossings)
            float edge = 1e9f;  // (and how far from its outline)
            for (int i = 0, j = np - 1; i < np; j = i++) {
                if ((py[i] > fy) != (py[j] > fy) && fx < (px[j] - px[i]) * (fy - py[i]) / (py[j] - py[i]) + px[i]) in = !in;
                float ex = px[j] - px[i], ey = py[j] - py[i], t = ((fx - px[i]) * ex + (fy - py[i]) * ey) / (ex * ex + ey * ey);
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                float dx = fx - (px[i] + t * ex), dy = fy - (py[i] + t * ey), d = sqrtf(dx * dx + dy * dy);
                if (d < edge) edge = d;
            }
            uint32_t c = 0;
            if (in && edge > 1.0f) {  // gold above, bronze below, a lighter edge inside the outline
                float g = fy / 21.8f;
                int r = (int)(250 - 70 * g), gg = (int)(214 - 100 * g), b = (int)(96 - 60 * g);
                if (edge < 1.7f) r = r + 5 > 255 ? 255 : r + 5, gg = gg + 25 > 255 ? 255 : gg + 25, b += 30;
                c = 0xff000000u | (uint32_t)r << 16 | (uint32_t)gg << 8 | (uint32_t)b;
            } else if (in || edge <= 1.0f) {
                c = 0xff000000u | 0x3c << 16 | 0x0e << 8 | 0x12;  // (wine-dark)
            }
            pix[y * (s->pitch / 4) + x] = c;
        }
    SDL_Cursor *made = SDL_CreateColorCursor(s, (int)(u + 0.5f), (int)(u + 0.5f));
    SDL_DestroySurface(s);
    LOG("[input] the menus' pointer, %d px: %s\n", n, made ? "made" : SDL_GetError());
    if (!made) return cur;
    if (cur && SDL_GetCursor() != cur) SDL_DestroyCursor(cur);  // (the one shown now stays until replaced)
    cur = made;
    return cur;
}

// Every key and mouse button going down, whoever takes it (the menus too: Alt+F1 is a shortcut as well): it spends the
// modifiers held, which then press nothing when let go (mod_until)
static void mod_track(const SDL_Event *e) {
    bool key = (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) || e->type == SDL_EVENT_KEY_UP;
    if ((key && e->key.down) || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_WHEEL)
        for (int i = 0; i < MOD_KEYS; i++)
            if (mod_held[i]) mod_spent[i] = true;
    if (!key || !is_mod(e->key.scancode)) return;
    int i = e->key.scancode - SDL_SCANCODE_LCTRL;
    mod_held[i] = e->key.down;
    if (e->key.down) mod_spent[i] = false;
}

// What the player is using, for the button hints of the port's menus (overlay_last_input): a key, a mouse button or the
// wheel, the mouse moved a few points (SDL's own motion when it warps the pointer has none: no relative movement), a
// controller's button or a stick or trigger pushed past half way (on a controller open for half a second: as for
// pad_pressed, one whose buttons read as held at first is not the player's), a finger
static void input_seen(const SDL_Event *e) {
    static float moved;  // (the mouse's travel, in the window's points, since something else was used)
    static uint64_t moved_ns;
    int kind = -1;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: kind = USED_KEYS; break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: if (e->button.which != SDL_TOUCH_MOUSEID) kind = USED_MOUSE; break;
    case SDL_EVENT_MOUSE_WHEEL: if (e->wheel.which != SDL_TOUCH_MOUSEID) kind = USED_MOUSE; break;
    case SDL_EVENT_MOUSE_MOTION: {
        if (e->motion.which == SDL_TOUCH_MOUSEID || overlay_last_input() == USED_MOUSE) break;
        uint64_t now = plat_now_ns();
        if (now - moved_ns > 300000000ull) moved = 0;  // (a nudge a while ago doesn't add up with one now)
        moved_ns = now;
        moved += fabsf(e->motion.xrel) + fabsf(e->motion.yrel);
        if (moved >= 8) kind = USED_MOUSE;
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        bool button = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        int i = find_pad(button ? e->gbutton.which : e->gaxis.which);
        if (i < 0 || plat_now_ns() - pads[i].opened_ns < 500000000ull) break;
        if (button || e->gaxis.value > 16384 || e->gaxis.value < -16384) kind = USED_PAD;
        break;
    }
    case SDL_EVENT_FINGER_DOWN: kind = USED_TOUCH; break;
    default: break;
    }
    if (kind < 0) return;
    moved = 0;
    overlay_input_seen(kind);
}

void host_input_event(const SDL_Event *e) {
    input_seen(e);
    mod_track(e);
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) pad_pressed(e->gbutton.which);
    // (a key or a controller: no row is pointed at until the mouse moves; in a menu or a dialog the pointer hides too,
    // and a controller's hides it for the next one as well)
    bool key_down = e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat;
    bool pad_down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                    (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (e->gaxis.value > 16000 || e->gaxis.value < -16000));
    if (key_down || pad_down) {
        mousemenu_hide();
        if (pad_down || menu_mouse()) menu_pointer_hidden = true;
    }
    if (e->type == SDL_EVENT_FINGER_UP && textinput_active()) textinput_tap();  // (the screen keyboard, if hidden)
    if (touch_event(e)) return;  // (fingers: the touch controls, or the settings menu's mouse: runtime/touch.cpp)
    if (overlay_event(e)) return;  // (Forge's menu open: keyboard and mouse are its)
#ifndef __ANDROID__
    if (touch_edit_mouse(e)) return;  // (the HUD editor open: the mouse moves the HUD's parts, Esc cancels it)
#endif
    if (chat_mouse(e)) return;  // (typing a line of chat: the mouse drags its box)
    if (textinput_event(e)) return;  // (typing into a text entry: not controller buttons)
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        // (a modifier let go that the game saw go down, with nothing pressed meanwhile: a tap. The release SDL sends for
        // every key held as the window loses the focus, Alt+Tab's, comes right before FOCUS_LOST, which undoes it)
        if (e->type == SDL_EVENT_KEY_UP && is_mod(e->key.scancode) && keys[e->key.scancode] &&
            !mod_spent[e->key.scancode - SDL_SCANCODE_LCTRL])
            mod_until[e->key.scancode - SDL_SCANCODE_LCTRL] = plat_now_ns() + WHEEL_PRESS_NS;
        if (e->key.scancode < SDL_SCANCODE_COUNT) keys[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // F12: screenshot, Shift+F12: 8 consecutive frames (run\shots\), F11: windowed <-> full screen
            static int nshot;
            uint64_t t = (uint64_t)time(NULL);
            if ((e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) && (e->key.mod & SDL_KMOD_ALT)) {
                keys[e->key.scancode] = false;  // (Alt+Enter: windowed <-> full screen, not a button press)
                keys[SDL_SCANCODE_LALT] = keys[SDL_SCANCODE_RALT] = false;
                ui_toggle_fullscreen();
            } else if ((int)e->key.scancode == config_live_chat_key() && config_live_chat_key()) {
                keys[e->key.scancode] = false;
                chat_open();  // (a line to type, sent as chat: runtime/hle/chat.c)
            } else if (e->key.scancode == SDL_SCANCODE_ESCAPE) {
                if (mouse_grab) LOG("[input] mouse released\n");
                mouse_grab = false;
            } else if (e->key.scancode == SDL_SCANCODE_F12 && !(e->key.mod & SDL_KMOD_SHIFT))
                snprintf(g_shot_request, sizeof g_shot_request, "shots/shot_%llu_%d.png", (unsigned long long)t, nshot++);
            else if (e->key.scancode == SDL_SCANCODE_F11) {
                ui_toggle_fullscreen();
            } else if (e->key.scancode == SDL_SCANCODE_F12) {
                snprintf(g_burst_name, sizeof g_burst_name, "burst_%llu_%d", (unsigned long long)t, nshot++);
                g_burst_index = 0;
                g_burst_left = 8;
            } else if (e->key.scancode == SDL_SCANCODE_F7) {  // MSAA off / 2x / 4x / 8x (to compare; the settings set it)
                void vk_cycle_msaa(void);
                vk_cycle_msaa();
#ifdef MHGU_DEV  // (the debug keys, F8 to F10: test builds only. The control file's "dump" works in any build)
            } else if (e->key.scancode == SDL_SCANCODE_F10) {
                g_dump_request = 2;  // (debug) every render pass of the next 2 frames, as "dump 2"
            } else if (e->key.scancode == SDL_SCANCODE_F8) {  // (debug) shadow-pass scissors: as set / none / mirrored
                extern volatile int g_shadow_scissor_mode;
                g_shadow_scissor_mode = (g_shadow_scissor_mode + 1) % 3;
                LOG("[input] F8: shadow scissor mode %d\n", g_shadow_scissor_mode);
            } else if (e->key.scancode == SDL_SCANCODE_F9) {  // (debug) colour clears: vkCmdClearAttachments <-> draws
                extern volatile int g_clear_draw;
                g_clear_draw = g_clear_draw > 0 ? 0 : 1;
                LOG("[input] F9: colour clears as %s\n", g_clear_draw ? "draws" : "vkCmdClearAttachments");
#endif
            }
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (menu_mouse() && !mouse_captured) {  // (a menu: where it points)
            float cx, cy;
            if (canvas_point(e->motion.x, e->motion.y, &cx, &cy)) mousemenu_pointer(cx, cy, true);
            menu_pointer_hidden = false;
        } else if (mouse_captured) {
            mouse_dx += e->motion.xrel, mouse_dy += e->motion.yrel;
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL: {  // (what [Keybinds] says: by default the item bar; in a menu, the menu's)
        int n = e->wheel.integer_y * (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1);
        float cx, cy;
        if (n && menu_mouse() && canvas_point(e->wheel.mouse_x, e->wheel.mouse_y, &cx, &cy)) mousemenu_wheel(n, cx, cy);
        else if (mouse_captured && n) wheel_turn(n);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
        int btn = e->button.button;
        if (btn < 1 || btn > SDL_BUTTON_X2) break;
        if (!e->button.down) {
            mouse_held &= ~(1u << btn);
            menu_held &= ~(1u << btn);
        } else if (menu_mouse()) {  // (a menu: a click there, never one of the game's buttons, nor a capture)
            float cx, cy;
            menu_held |= 1u << btn;
            menu_pointer_hidden = false;
            if (canvas_point(e->button.x, e->button.y, &cx, &cy)) mousemenu_pointer(cx, cy, true), mousemenu_click(btn, cx, cy);
        } else if (!mouse_captured && !mouse_no_capture) {  // (the click that captures the mouse is not a button press)
            if (!mouse_grab) LOG("[input] capturing the mouse\n");
            mouse_grab = true;
        } else {
            mouse_held |= 1u << btn;
        }
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        memset(keys, 0, sizeof keys);
        memset(mod_until, 0, sizeof mod_until);  // (Alt+Tab: its Alt was no tap)
        memset(mod_held, 0, sizeof mod_held);
        mouse_held = menu_held = 0, mouse_grab = false;
        config_window_focus(false);  // ([Audio] MuteInBackground)
        if (config_editable().mute_in_background) LOG("[input] window in the background: sound muted\n");
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        config_window_focus(true);
        if (config_editable().mute_in_background) LOG("[input] window focused: sound back\n");
        break;
#ifndef __ANDROID__
    // ([Application] PauseInBackground: on a desktop the background is a minimized window; Android: runtime/ui.c's
    // lifecycle watch)
    case SDL_EVENT_WINDOW_MINIMIZED: {
        void vi_set_paused(bool on);  // (runtime/hle/vi.c)
        if (config_live_pause_background()) vi_set_paused(true);
        break;
    }
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED: {
        void vi_set_paused(bool on);
        vi_set_paused(false);
        break;
    }
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:  // (the window resized, full screen or back: the picture takes its shape)
        config_window_shape(e->window.data1, e->window.data2);
        break;
#endif
    case SDL_EVENT_GAMEPAD_ADDED:
        open_pad(e->gdevice.which);
        break;
    case SDL_EVENT_JOYSTICK_ADDED:  // (also sent for gamepads, before their GAMEPAD_ADDED: joystick_seen skips those)
        joystick_seen(e->jdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        close_pad(e->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMAPPED: {  // (the player mapped it: runtime/padmap.c)
        int i = find_pad(e->gdevice.which);
        if (i >= 0) pads[i].real |= padmap_custom(e->gdevice.which);
        break;
    }
    }
}

static int32_t clamp_axis(int v) { return v < -32767 ? -32767 : v > 32767 ? 32767 : v; }

// Capture or release the mouse as wanted, and give its movement to the hunt camera or the right stick.
// ---- Aiming with the gyro ([Controls] GyroAim): while a bow or a bowgun aims (weapon_gyro_aiming; not a locked shot's
// own aim), the phone's gyroscope - a controller's, if the one read has one - turns the aim as a drag does, added to it:
// the angle turned since the aim began, times GyroSensitivity, in the mouse's counts (the hunt camera's GYRO_COUNT_DEG
// each; with a gun's or a bow's own camera up, mouse_update's stick). The sensor is open only while aiming. The
// phone's axes go by the display's quarter turns from the device's natural orientation (a phone's portrait, an Anbernic
// RG406V's landscape). The MH4U port's the same (app_native/controls.cpp)
const float GYRO_COUNT_DEG = 0.08f;   // (hle/mousecam.c: the hunt camera's turn for a count)
const float GYRO_DEADBAND = 0.01f;    // (radians a second: the gyroscope's own drift, at rest)
static SDL_Sensor *gyro_dev;
static bool gyro_on;
static int gyro_turns;
static uint64_t gyro_ns;
bool touch_lock_assist(void);  // (runtime/touch.cpp: a locked shot's own aim)
static int display_quarter(SDL_DisplayOrientation o) {  // (the device turned this many quarters counterclockwise)
    return o == SDL_ORIENTATION_LANDSCAPE ? 1 : o == SDL_ORIENTATION_PORTRAIT_FLIPPED ? 2 : o == SDL_ORIENTATION_LANDSCAPE_FLIPPED ? 3 : 0;
}
static void gyro_off(void) {
    if (gyro_dev) SDL_CloseSensor(gyro_dev), gyro_dev = NULL;
    if (gyro_pad) SDL_SetGamepadSensorEnabled(gyro_pad, SDL_SENSOR_GYRO, false), gyro_pad = NULL;
    if (gyro_on) LOG("[gyro] off\n");
    gyro_on = false, gyro_ns = 0;
}
static void gyro_update(void) {
    uint64_t now = plat_now_ns();
    if (!config_live_gyro_aim() || !weapon_gyro_aiming(weapon_hunter(), now) || touch_lock_assist()) {
        if (gyro_on) gyro_off();
        return;
    }
    if (!gyro_on) {  // (the aim begins: where it is now is the middle, the phone's pose with it)
        gyro_on = true, gyro_ns = 0;
        SDL_Gamepad *p = pad && SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) ? pad : NULL;
        if (p && SDL_SetGamepadSensorEnabled(p, SDL_SENSOR_GYRO, true)) gyro_pad = p;
        int n = 0;
        SDL_SensorID *ids = gyro_pad ? NULL : SDL_GetSensors(&n);
        for (int i = 0; ids && i < n && !gyro_dev; i++)
            if (SDL_GetSensorTypeForID(ids[i]) == SDL_SENSOR_GYRO) gyro_dev = SDL_OpenSensor(ids[i]);
        SDL_free(ids);
        SDL_DisplayID d = SDL_GetPrimaryDisplay();
        gyro_turns = (display_quarter(SDL_GetCurrentDisplayOrientation(d)) - display_quarter(SDL_GetNaturalDisplayOrientation(d)) + 4) % 4;
        LOG("[gyro] on: %s (the display %d quarter turns from the device's own orientation)\n",
            gyro_pad ? SDL_GetGamepadName(gyro_pad) : gyro_dev ? SDL_GetSensorName(gyro_dev) : "no gyroscope", gyro_turns);
    }
    float g[3] = {0, 0, 0}, right = 0, up = 0;  // (radians a second: the aim's turn right, up)
    if (gyro_pad && SDL_GetGamepadSensorData(gyro_pad, SDL_SENSOR_GYRO, g, 3)) {
        right = -g[1], up = g[0];  // (SDL's: a controller held before you, x right, y up, z toward you)
    } else if (gyro_dev && SDL_GetSensorData(gyro_dev, g, 3)) {
        // (the device's natural axes: x right, y up, z out of the screen; the screen's up and right in them, by quarter turn)
        static const float ux[4] = {0, 1, 0, -1}, uy[4] = {1, 0, -1, 0}, rx[4] = {1, 0, -1, 0}, ry[4] = {0, -1, 0, 1};
        int k = gyro_turns;
        right = -(g[0] * ux[k] + g[1] * uy[k]), up = g[0] * rx[k] + g[1] * ry[k];
    }
    if (fabsf(right) < GYRO_DEADBAND) right = 0;
    if (fabsf(up) < GYRO_DEADBAND) up = 0;
    if (now < gyro_test_until) right = gyro_test_r * 0.0174533f, up = gyro_test_u * 0.0174533f;
    float dt = gyro_ns ? (float)(now - gyro_ns) * 1e-9f : 0.0f;
    gyro_ns = now;
    if (dt > 0.1f) dt = 0.1f;
    float k = config_live_gyro_sensitivity() * 57.29578f / GYRO_COUNT_DEG * dt;  // (radians -> degrees -> counts)
    if ((right != 0 || up != 0) && k > 0) mouse_dx += right * k, mouse_dy -= up * k, touch_cam_ns = now;
}

static void mouse_update(int *rx, int *ry) {
    // (Forge's menu, the chat box, a text the game asks for: the pointer is theirs)
    bool want = mouse_grab && !overlay_menu_open() && !chat_active() && !textinput_active();
#ifndef __ANDROID__
    if (touch_hud_editing()) want = false;  // (and the HUD editor, from the moment it is asked for: runtime/touch.cpp)
#endif
    bool menu = menu_mouse();
    if (menu) want = false;  // (and the game's menus: hle/mousemenu.c)
    mousemenu_block(overlay_menu_open() || chat_active() || textinput_active()
#ifndef __ANDROID__
                    || touch_hud_editing()
#endif
    );
    SDL_Window *w = ui_window();
    if (want != mouse_captured && w) {
        if (SDL_SetWindowRelativeMouseMode(w, want) || !want) {
            mouse_captured = want;
        } else {
            LOG("[input] mouse capture failed: %s (no mouse look; clicks are buttons)\n", SDL_GetError());
            mouse_grab = false, mouse_no_capture = true;
        }
        mouse_dx = mouse_dy = 0;
    }
    // (the game's menus and dialogs: the pointer free, in the game's colours, shown as one opens unless a controller
    // was used last, and until a key or a controller is used in it; the buttons held as a menu takes it are its own
    // until let go. Its place as it comes back is no movement: no row is pointed at until the mouse moves)
    static bool menu_was, menu_shown, menu_showing;
    if (menu != menu_was && w) {
        menu_was = menu;
        uint64_t now = plat_now_ns();
        if (menu) {
            int last = overlay_last_input();
            if (!menu_left_ns || now - menu_left_ns >= MENU_GAP_NS) menu_pointer_hidden = false;
            if (last == USED_PAD || last == USED_TOUCH) menu_pointer_hidden = true;
        } else {
            menu_left_ns = now;
        }
        LOG("[input] %s%s\n", menu ? "a menu of the game's has the mouse: pointer free" : "the game's menus let the mouse go",
            !menu ? "" : menu_pointer_hidden ? " (hidden until the mouse moves)" : " and shown");
        if (menu) {
            menu_held |= mouse_held, mouse_held = 0;
            float x = 0, y = 0, cx, cy;
            SDL_GetMouseState(&x, &y);
            if (canvas_point(x, y, &cx, &cy)) mousemenu_pointer(cx, cy, false);
        } else if (menu_shown) {  // (back as it was: the system's arrow, hidden in full screen, shown in a window)
            SDL_Cursor *mine = menu_cursor();
            if (mine && SDL_GetCursor() == mine) SDL_SetCursor(SDL_GetDefaultCursor());
            bool theirs = overlay_menu_open() || chat_active() || textinput_active()
#ifndef __ANDROID__
                          || touch_hud_editing()
#endif
                ;
            if (!theirs && (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN)) SDL_HideCursor();
            else if (!SDL_CursorVisible()) SDL_ShowCursor();
            menu_shown = false;
        }
    }
    if (menu && w) {
        bool show = !menu_pointer_hidden;
        if (show != menu_showing || !menu_shown) LOG("[input] the menus' pointer %s\n", show ? "shown" : "hidden");
        menu_showing = show;
        if (show) {
            SDL_Cursor *mine = menu_cursor();
            if (mine && SDL_GetCursor() != mine) SDL_SetCursor(mine);
            if (!SDL_CursorVisible()) SDL_ShowCursor();
        } else if (SDL_CursorVisible()) {
            SDL_HideCursor();
        }
        menu_shown = true;
    }
    static bool chat_pointer;  // (full screen hides the pointer: shown while the chat box can be dragged)
    if (chat_active() != chat_pointer && w) {
        chat_pointer = chat_active();
        if (chat_pointer) SDL_ShowCursor();
        else if (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) SDL_HideCursor();
    }
#ifndef __ANDROID__
    // (the HUD editor's too, full screen as well: shown again whatever hid it as the menu before it closed, and hidden
    // again once it closes, unless something else still has it)
    static bool hud_pointer;
    bool hud = touch_hud_editing() && !overlay_menu_open();
    if (hud && !SDL_CursorVisible()) SDL_ShowCursor();
    else if (!hud && hud_pointer && w && (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) && !overlay_menu_open() &&
             !chat_active())
        SDL_HideCursor();
    hud_pointer = hud;
#endif
    static uint64_t last;
    static float vx, vy;  // the speed, smoothed: counts a second
    uint64_t now = plat_now_ns();
    float dt = last ? (float)(now - last) * 1e-9f : 0.0f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    if (now < test_until) mouse_dx += test_vx * dt, mouse_dy += test_vy * dt;
    if (mousecam_active()) {  // (the hunt camera turns by the counts themselves)
        if (mouse_dx != 0 || mouse_dy != 0) mousecam_add(mouse_dx, mouse_dy);
        mouse_dx = mouse_dy = vx = vy = 0;
        return;
    }
    if (dt > 0.0f) {
        float a = 1.0f - expf(-dt / MOUSE_SMOOTH_S);
        vx += (mouse_dx / dt - vx) * a, vy += (mouse_dy / dt - vy) * a;
    }
    mouse_dx = mouse_dy = 0;
    if (!mouse_captured && now >= test_until && now - touch_cam_ns > 150000000ull) vx = vy = 0;
    float s = config_live_mouse_sensitivity() / MOUSE_FULL_SPEED;
    float x = vx * s, y = -vy * s;  // (the mouse's y is down, the stick's up)
    float m = sqrtf(x * x + y * y);
    if (m < 0.02f) return;
    float t = (MOUSE_DEADZONE + (1.0f - MOUSE_DEADZONE) * (m > 1.0f ? 1.0f : m)) / m;
    *rx += (int)(x * t * 32767.0f), *ry += (int)(y * t * 32767.0f);
}

// The game's rumble (input_rumble) to the controller: on a change, at most every 16 ms (the game sends a value every
// 5 ms, and a DualSense over Bluetooth takes a packet per call), renewed while it lasts, stopped at once. And to the
// phone's own vibrator, with or without a controller ([Controls] PhoneRumble, runtime/haptics.c).
static void rumble_update(void) {
    static uint32_t sent, peak;
    static uint64_t sent_ns, on_ns;
    static SDL_Gamepad *sent_pad;
    uint32_t v = input_rumble();
    haptics_rumble(v);
    uint64_t now = plat_now_ns();
    if (v && !on_ns) on_ns = now, peak = 0;  // (the log: each rumble's length and strongest motor values)
    if (v) peak = ((v >> 16) > (peak >> 16) ? v & 0xFFFF0000u : peak & 0xFFFF0000u) | ((v & 0xFFFF) > (peak & 0xFFFF) ? v & 0xFFFF : peak & 0xFFFF);
    if (!v && on_ns) {
        LOG("[input] rumble %.0f ms, low %u high %u%s\n", (now - on_ns) / 1e6, peak >> 16, peak & 0xFFFF, pad ? "" : " (no controller)");
        on_ns = 0;
    }
    if (!pad) { sent_pad = NULL; return; }
    if (pad != sent_pad) sent = 0, sent_pad = pad;
    bool change = v != sent && (v == 0 || now - sent_ns >= 16000000ull), renew = v && now - sent_ns >= 100000000ull;
    if (!change && !renew) return;
    SDL_RumbleGamepad(pad, (Uint16)(v >> 16), (Uint16)v, v ? 200 : 0);
    sent = v, sent_ns = now;
}

#ifndef __ANDROID__
// A mouse button on A presses it now (mouse_a): with the weapon out (being drawn too), in a menu or a dialog of the
// game's (MouseMenus Off: there the right click confirms as before; On: the clicks are the menu's anyway), or with no
// hunter, no hunt camera and no town (the title, the main menu, a cutscene: their A). Not in a town nor with the weapon
// sheathed (-1, no hunter: a Prowler, as sheathed). A left and a right click together with it sheathed: the left one
// (X) draws it, and the right one, held, presses A as soon as it is out, a frame or two later - the next move's A, never
// one with the weapon sheathed, which could talk, gather or pick up
extern volatile uint64_t g_town_ns;  // (hle/quickhunt.c: a town's walking-around update)
static bool mouse_a_ok(void) {
    if (weapon_drawn() == 1 || mousemenu_open()) return true;
    uint64_t town = g_town_ns;
    return !weapon_hunter() && !mousecam_active() && !(town && plat_now_ns() - town < 1000000000ull);
}
#endif

void host_input_update(void) {
    if (nscript < 0) script_parse(), padmap_init();
    padmap_poll();
    rumble_update();
    finger_due();  // (tests: fingers waiting for their time, every update: their ms count)
    control_poll();
    quickhunt_poll();  // (debug, MHGU_QUICKHUNT: runtime/hle/quickhunt.c)
    textinput_update();
    uint64_t b = 0;
    int lx = 0, ly = 0, rx = 0, ry = 0;
    static bool typing;
    if (textinput_active() != typing) {
        typing = !typing;
        memset(keys, 0, sizeof keys);  // (keys held when the entry opened are typing now, or released)
    }

#ifndef __ANDROID__
    mouse_a = mouse_a_ok();
#endif
    for (int i = 0; i < BINDS; i++)
        if (bind_buttons[i] >= 0 && bind_held(i, true)) b |= B(bind_buttons[i]);
    for (int i = 0; i < TOUCH_COMBOS; i++)  // (a combined button's key: its buttons, as its button on the screen)
        if (bind_held(BIND_COMBO1 + i, true)) b |= config_live_touch_combo(i);
    static bool item_held[2];  // ItemLeft / ItemRight's keys and buttons: a press turns the item bar once
    for (int i = 0; i < 2; i++) {  // (their wheel notches turn it in wheel_turn)
        bool held = bind_held(BIND_ITEM_LEFT + i, false);
        if (held && !item_held[i]) touch_item_step(i ? 1 : -1);  // (a Prowler's: its Support Moves, runtime/touch.cpp)
        item_held[i] = held;
    }
    int kx = bind_held(BIND_MOVE_RIGHT, true) - bind_held(BIND_MOVE_LEFT, true);
    int ky = bind_held(BIND_MOVE_UP, true) - bind_held(BIND_MOVE_DOWN, true);
#ifndef __ANDROID__
    // (in the game's menus - the item box, the shops, the title - the move keys are the d-pad, which moves their cursor
    // a step at a time where the left stick does not, as the touch controls' stick there: runtime/touch.cpp game_menu).
    // Not left / right while a list that only the stick moves sideways runs (hle/mousemenu.c menu_stick_sideways: a
    // menu's tabs, which a controller's stick turns and its d-pad does not): the stick's there, the arrow keys the d-pad.
    // In a town's menu (the item box, a shop, the quest counter and its board: the town goes on under them, the hunter
    // only turns) the d-pad and the left stick both, as before the d-pad (the quest board's lists, which the stick moves
    // where the d-pad doesn't), as long as the hunter goes nowhere with them (the pause menu, the pouch: the game's menus
    // there don't read the stick and the hunter walks - from then the d-pad alone until the menu closes). And a list's
    // cursor running in a town while the picture shows no menu window, or a menu or a dialog the game's updates say is
    // open (hle/mousemenu.c mousemenu_open: the quest counter's talk and its board): the d-pad too. Each change in the log
    static bool was_menu, walks;
    static float mx, mz;
    static int said = -1;
    bool in_menu = touch_game_menu();
    uint64_t town_ns = g_town_ns;
    bool town = town_ns && plat_now_ns() - town_ns < 500000000ull;
    bool cursor = !in_menu && town && (menu_cursor_age() <= 2 || mousemenu_open());
    if (in_menu != was_menu) {  // (a menu opened or closed: where the hunter stood as it opened)
        was_menu = in_menu;
        walks = in_menu && !player_place(&mx, &mz);
    }
    if (kx || ky) {
        float px, pz;
        if (in_menu && town && !walks && player_place(&px, &pz) && hypotf(px - mx, pz - mz) > 2.0f) walks = true;
        bool both = cursor || (in_menu && town && !walks);  // the d-pad and the stick
        bool side = kx && in_menu && menu_stick_sideways();  // left / right the stick's alone
        int st = (int)in_menu | (int)cursor << 1 | (int)town << 2 | (int)both << 3 | (int)side << 4 | (int)walks << 5;
        if (st != said) {
            if (in_menu || cursor)
                LOG("[input] the move keys in %s%s: the d-pad%s%s (a list's cursor %u updates ago)\n",
                    in_menu ? "a menu" : "a list's cursor or a dialog (no menu window)", town ? " in a town" : "",
                    both ? " and the left stick" : walks ? " alone (the hunter walked with the stick)" : " alone",
                    side ? ", left / right the stick's alone" : "", menu_cursor_age());
            else if (said > 0)
                LOG("[input] the move keys: the left stick (a list's cursor %u updates ago)\n", menu_cursor_age());
            said = st;
        }
        if (in_menu || cursor) {
            if (kx && !side) b |= B(kx > 0 ? BTN_RIGHT : BTN_LEFT);
            if (ky) b |= B(ky > 0 ? BTN_UP : BTN_DOWN);
            if (!both) {
                if (!side) kx = 0;
                ky = 0;
            }
        }
    }
#endif
    int k = bind_held(BIND_WALK, true) ? 16000 : 32767;
    if (kx && ky) k = k * 7071 / 10000;  // diagonals: same length as straight
    lx += kx * k, ly += ky * k;
    int cx = bind_held(BIND_CAM_RIGHT, true) - bind_held(BIND_CAM_LEFT, true);  // (the camera's keys: the right stick
    int cy = bind_held(BIND_CAM_UP, true) - bind_held(BIND_CAM_DOWN, true);     // all the way, the hunt camera's turn)
    int ck = cx && cy ? 23170 : 32767;
    rx += cx * ck, ry += cy * ck;
    float tdx = 0, tdy = 0;  // (the touch controls: buttons, the stick, and a drag that turns the camera as the mouse)
    bool real_pad = real_pad_connected();
    SDL_SetAtomicInt(&real_pad_seen, real_pad);
    touch_input(real_pad, typing, &b, &lx, &ly, &tdx, &tdy);  // (a real controller open hides them)
    touch_right_stick(&rx, &ry);  // (a Hunter Art tapped that the game's controls put on the right stick)
    if (tdx != 0 || tdy != 0) mouse_dx += tdx, mouse_dy += tdy, touch_cam_ns = plat_now_ns();
    gyro_update();  // (aiming a bow or a bowgun: the gyroscope's turn, as a drag)
    bool mouse_moved = mouse_dx != 0 || mouse_dy != 0;  // (the mouse or a finger turning the camera: the lock-on waits)
    mouse_update(&rx, &ry);

    uint64_t before_pad = b;  // (what the controller adds: an editor's B and +, below)
    if (pad) {
        // combined buttons on the controller's own ([Touch] Combo*Pad): that button is theirs now, not what it was
        static const SDL_GamepadButton pad_of[PADB_COUNT] = {
            SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_BACK,
            SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_MISC1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,
            SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, SDL_GAMEPAD_BUTTON_TOUCHPAD};
        uint64_t taken = 0;
        for (int i = 0; i < TOUCH_COMBOS; i++) {
            int on = config_live_touch_combo_pad(i);
            uint32_t combo = config_live_touch_combo(i);
            if (on <= PADB_NONE || on >= PADB_COUNT || !combo) continue;
            taken |= 1ull << pad_of[on];
            if (SDL_GetGamepadButton(pad, pad_of[on])) b |= combo;
        }
        static const struct { SDL_GamepadButton sdl; int btn; } pad_buttons[] = {
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, BTN_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, BTN_R},
            {SDL_GAMEPAD_BUTTON_START, BTN_PLUS}, {SDL_GAMEPAD_BUTTON_BACK, BTN_MINUS},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, BTN_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, BTN_RIGHT},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, BTN_LSTICK}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, BTN_RSTICK},
        };
        for (unsigned i = 0; i < sizeof pad_buttons / sizeof *pad_buttons; i++)
            if (!(taken >> pad_buttons[i].sdl & 1) && SDL_GetGamepadButton(pad, pad_buttons[i].sdl)) b |= B(pad_buttons[i].btn);
        // face buttons: SDL names them by position (south, east, west, north). Settings FaceButtons: Position = the
        // Switch layout; Label = the letter on the button (PlayStation symbols as an Xbox pad's letters at that place)
        static const SDL_GamepadButton face[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                                  SDL_GAMEPAD_BUTTON_NORTH};
        static const int switch_at[4] = {BTN_B, BTN_A, BTN_Y, BTN_X}, xbox_at[4] = {BTN_A, BTN_B, BTN_X, BTN_Y};
        static int old_face_mode = -1;
        static bool wait_face_release;
        int face_mode = config_live_face_buttons();
        if (old_face_mode >= 0 && old_face_mode != face_mode) wait_face_release = true;
        old_face_mode = face_mode;
        bool held_face = false;
        int menu_positions[4] = {1, 0, 3, 2};
        for (int i = 0; i < 4; i++) {
            int btn = switch_at[i];
            if (face_mode == FACE_LABEL && !padmap_custom(SDL_GetGamepadID(pad))) {  // (a mapped one: by position, it was set so)
                switch (SDL_GetGamepadButtonLabel(pad, face[i])) {
                case SDL_GAMEPAD_BUTTON_LABEL_A: btn = BTN_A; break;
                case SDL_GAMEPAD_BUTTON_LABEL_B: btn = BTN_B; break;
                case SDL_GAMEPAD_BUTTON_LABEL_X: btn = BTN_X; break;
                case SDL_GAMEPAD_BUTTON_LABEL_Y: btn = BTN_Y; break;
                default: btn = xbox_at[i]; break;
                }
            }
            int semantic = btn == BTN_A ? 0 : btn == BTN_B ? 1 : btn == BTN_X ? 2 : 3;
            menu_positions[semantic] = i;
            bool pressed = SDL_GetGamepadButton(pad, face[i]);
            held_face |= pressed;
            if (pressed && !wait_face_release) b |= B(btn);
        }
        if (!held_face) wait_face_release = false;
        overlay_face_positions(menu_positions);
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000) b |= B(BTN_ZL);
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000) b |= B(BTN_ZR);
        // sticks: SDL's y is down; a small dead zone against drift
        int ax[4] = {SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY),
                     SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY)};
        for (int i = 0; i < 4; i++)
            if (ax[i] > -3000 && ax[i] < 3000) ax[i] = 0;
        lx += ax[0], ly += ax[1], rx += ax[2], ry += ax[3];
        static const char *dbg;  // (MHGU_PAD_DEBUG=1) every raw SDL controller change: finds a button that presses itself
        if (!dbg) dbg = getenv("MHGU_PAD_DEBUG") ? "1" : "";
        if (*dbg) {
            static uint64_t pbtn;
            static int pax[6];
            uint64_t cb = 0;
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
                if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)i)) cb |= 1ull << i;
            int dax[6];
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++) dax[i] = SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)i);
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
                if ((cb ^ pbtn) & (1ull << i))
                    LOG("[paddebug] %s %s\n", SDL_GetGamepadStringForButton((SDL_GamepadButton)i),
                        (cb >> i) & 1 ? "down" : "up");
            pbtn = cb;
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++)
                if (dax[i] != pax[i] && (dax[i] > 500 || dax[i] < -500 || pax[i] > 500 || pax[i] < -500))
                    LOG("[paddebug] axis %s %d\n", SDL_GetGamepadStringForAxis((SDL_GamepadAxis)i), dax[i]);
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++) pax[i] = dax[i];
        }
    } else {
        static bool scanned;  // (controllers present at startup send no ADDED event to a loop that started after them)
        if (!scanned) scanned = true, pick_pad();
    }
    uint64_t pad_b = b & ~before_pad;  // (the controller's own: its B cancels an editor, below)

    uint64_t frame = g_present_count;
    for (int i = 0; i < nscript; i++) {
        const ScriptStep *st = &script[i];
        if (frame < st->start || frame >= st->start + st->len) continue;
        b |= st->buttons;
        if (st->stick == 0) lx = st->x, ly = st->y;
        if (st->stick == 1) rx = st->x, ry = st->y;
    }

    // The right stick's click in a hunt turns the lock-on on or off (the camera kept on the monster, as in the newer
    // games: runtime/touch.cpp); elsewhere it is the game's. With the game's Control Type 3, whose click changes the
    // target (hle/mousecam.c mousecam_target_buttons), the click stays the game's and holding it turns the lock-on
    static bool rstick_was;
    static uint64_t rstick_hold;  // (Control Type 3: when the click held turns the lock-on; 0 none)
    bool rstick = (b & B(BTN_RSTICK)) && mousecam_active() && config_live_lock_on();
    bool r3_targets = rstick && (mousecam_target_buttons() & B(BTN_RSTICK)) != 0;
    if (rstick && !rstick_was) {
        if (r3_targets) rstick_hold = plat_now_ns() + 600000000ull;  // (as the touch controls' long press on L)
        else touch_lock_toggle(), rstick_hold = 0;
    }
    if (!rstick) rstick_hold = 0;
    if (rstick_hold && plat_now_ns() >= rstick_hold) touch_lock_toggle(), rstick_hold = 0;
    rstick_was = rstick;
    if (rstick && !r3_targets) b &= ~B(BTN_RSTICK);
    b |= touch_lock_buttons(b, mouse_moved || (int64_t)rx * rx + (int64_t)ry * ry > 12000LL * 12000);  // (round: as the game's)
    // (with the lock-on on, a bowgun's shot is aimed first, whatever fires it: runtime/touch.cpp; standing still, no stick
    // past a quarter, as 0.1.8 aimed it)
    b = touch_lock_shot(b, (int64_t)lx * lx + (int64_t)ly * ly < 8192LL * 8192);
    // (the touch controls' menu pad: what it alone presses the game reads only while a menu takes input, hle/input.c)
    uint64_t menu_only = touch_menu_buttons() & ~b;
    b |= menu_only;

    // A controller can reach host settings without needing a keyboard.
    static bool settings_chord;
    bool chord = (b & (B(BTN_PLUS) | B(BTN_MINUS))) == (B(BTN_PLUS) | B(BTN_MINUS));
    if (chord && !settings_chord && !typing) overlay_toggle_settings();
    if (chord && !typing) settings_chord = true;
    if (settings_chord) {
        // Swallow releases as well: releasing one half must not activate the game's menu.
        if (!(b & (B(BTN_PLUS) | B(BTN_MINUS)))) settings_chord = false;
        b &= ~(B(BTN_PLUS) | B(BTN_MINUS));
    }
    static bool menu, menu_face_release;
    if (overlay_menu_open() != menu) {
        if (menu) menu_face_release = true;
        menu = !menu;
        memset(keys, 0, sizeof keys);  // (keys held as it opened went to the menu: nothing stays pressed)
        mouse_held = 0;
    }
    if (padmap_active()) b = 0, lx = ly = rx = ry = 0;  // (being mapped: its presses are the mapping's)
    if (menu) {  // Forge's menu has the controller (as in Forge: A activates, B cancels, stick and d-pad move)
        OverlayPad p = {b & B(BTN_A), b & B(BTN_B), b & B(BTN_X), b & B(BTN_Y), b & B(BTN_UP), b & B(BTN_DOWN),
                        b & B(BTN_LEFT), b & B(BTN_RIGHT), b & B(BTN_L), b & B(BTN_R), b & B(BTN_PLUS),
                        b & B(BTN_MINUS), clamp_axis(lx) / 32767.0f, clamp_axis(ly) / 32767.0f};
        overlay_pad(&p);
        b = 0, lx = ly = rx = ry = 0;
    }

    if (!menu && menu_face_release) {
        const uint64_t face_mask = B(BTN_A) | B(BTN_B) | B(BTN_X) | B(BTN_Y);
        b = menu_release_buttons(b, face_mask, &menu_face_release);
    }
    static uint64_t was;
    uint64_t down = b & ~was;
    was = b;
    // Plus (Start) confirms, B cancels the entry; the game keeps the rest of the controller. Dropping it whole read as a
    // lock-up: with no keyboard on screen (the game's own inline keyboard needs nativeNX/arc/swkbd_eu.arc, which is
    // missing), a pad-only player had no way out - B only reopened the entry. A line of chat (runtime/hle/chat.c)
    // leaves them to the game: it is typed on the keyboard, and B mid-hunt is a dodge, not losing the line.
    if (typing && !chat_active()) {
        if (down & B(BTN_PLUS)) textinput_key(TI_ENTER);
        else if (down & B(BTN_B)) textinput_key(TI_CANCEL);
        b &= ~(B(BTN_PLUS) | B(BTN_B));
    }
    // An editor open (runtime/touch.cpp: the touch layout's, worked with fingers; on a desktop the HUD's, worked with the
    // mouse, Esc cancels it): a controller's B cancels it too, or a player with a controller alone had no way out. Not +:
    // Start reads as confirm, the panel's own + sizes what is picked, and Start + Back opens the settings. The game gets
    // neither B nor + while it is open, nor one still held as it closes, however it closed, until let go (the keyboard's
    // keys for them are held back too: Esc is the keyboard's there)
    static uint64_t pad_was, ed_held;
    static bool ed_was;
    uint64_t pad_down = pad_b & ~pad_was;
    pad_was = pad_b;
#ifndef __ANDROID__
    touch_hud_pad(pad != NULL);  // (a controller read: its B shows on the HUD editor's Cancel)
#endif
    ed_held &= b;
    bool ed = touch_editor_open() && !menu;
    if (ed && (pad_down & B(BTN_B))) touch_editor_cancel(), ed = false;
    if (ed) b &= ~(B(BTN_B) | B(BTN_PLUS));
    else if (ed_was) ed_held |= b & (B(BTN_B) | B(BTN_PLUS));
    ed_was = ed;
    b &= ~ed_held;
    input_stage_menu_only(menu_only & b);
    input_set(b, clamp_axis(lx), clamp_axis(ly), clamp_axis(rx), clamp_axis(ry));
    out_busy = b || lx || ly || rx || ry || rstick_hold;
}

// (MHGU_PERF_INPUT_WAKE, runtime/ui.c) Something that goes on by the clock, not by an event: then the window loop keeps its
// 3 ms pace. A button or a stick given to the game at the last update (a touch press, a locked run, a key, a scripted
// step), a controller SDL reads only as the loop polls (HIDAPI's: a device path; a virtual one: bus 0. Android's own, a
// handheld's built-in buttons too, have neither and come as events from the system's input thread: SDL gives them all
// the Bluetooth bus), a scripted press or a test finger waiting, the game's rumble, the settings menu, a text being typed,
// a controller being mapped, or the touch controls' own timers (touch_busy). Otherwise nothing changes until an event, a
// frame or a rumble wakes the loop (ui_loop_wake).
int host_input_busy(void) {  // (which, for the log: HIB_*, 0 none)
    if (out_busy) return HIB_OUTPUT;
    if (padmap_active()) return HIB_PADMAP;
    if (g_present_count < script_end || plat_now_ns() < test_until) return HIB_SCRIPT;
    for (int i = 0; i < npads; i++) {
        SDL_Joystick *j = SDL_GetGamepadJoystick(pads[i].pad);
        SDL_GUID g = j ? SDL_GetJoystickGUID(j) : (SDL_GUID){{0}};
        const char *path = SDL_GetGamepadPath(pads[i].pad);
        bool events = j && !path && g.data[0] == 0x05 && !g.data[1];  // (SDL_HARDWARE_BUS_BLUETOOTH, SDL_sysjoystick.h)
        static SDL_Gamepad *said;
        if (said != pads[i].pad) {
            LOG("[perf] input loop: %s %s (bus %02x%02x, %s)\n", SDL_GetGamepadName(pads[i].pad),
                events ? "sends events: the loop waits between them" : "is read by polling: 3 ms", g.data[1], g.data[0],
                path ? path : "no path");
            said = pads[i].pad;
        }
        if (!events) return HIB_PAD;
    }
    for (int i = 0; i < 32; i++)
        if (finger_wait[i].due) return HIB_SCRIPT;
    if (input_rumble()) return HIB_RUMBLE;
    if (overlay_menu_open() || textinput_active() || chat_active()) return HIB_MENU;
    return touch_busy() ? HIB_TOUCH : 0;
}
