// The mouse in the game's own menus, as in the newer games on a PC (a desktop's; nothing on Android): in a menu, and
// while the game talks or asks something (a dialog: a person's lines, a message, a yes / no), the pointer is free and
// shown, the game's cursor goes to the row or cell under it, a left click is A on that row or cell and elsewhere the
// game's A on what has the focus (a talk's next line, a message's OK, the answer picked), a right click B, the wheel
// scrolls a list (a row a notch, the page at its ends) or turns a box's page (Up / Down where no list or box is moved:
// a quantity), and the mouse's side buttons are the page buttons ([Controls] MouseMenus; runtime/host_input.c frees the
// pointer while one is open and passes the mouse here).
//
// The game has no mouse of its own (sMouse is never read), so this moves its cursors from inside. Menus read their
// buttons from the menu half of the game's pad system, sFestaPad (0x1a8c670 *, updated each frame by 0x5ddf1c): +0x6bf4
// held, +0x6bfc pressed this update, +0x6c34 repeated; Minus 0x1, Plus 0x8, Up 0x10, Right 0x20, Down 0x40, Left 0x80,
// L 0x100, R 0x200, ZL 0x400, ZR 0x800, X 0x1000, A 0x2000, B 0x4000, Y 0x8000 (the stick's directions also in bits
// 16..23). The hunter reads the other half (+0x6ba4): a press written into the menu's half swings no weapon. Two classes
// keep almost every menu's cursor: cCursor, a list (+0x08 the last move's kind: 1 / 2 a row back / forward, 4 / 8 a
// page, which the menus refill the rows by; +0x0d the row on its page, +0x0e rows a page, +0x10 the page, +0x12 pages,
// +0x14 entries, +0x18 / +0x1c the rows' and the pages' axis, an entry each of the table at 0x182d0c0 {back, forward,
// sound...}, +0x20 its sound on, +0x0c pages wrap), moved by 0x74ee98(cursor, buttons), and cBoxCursor, a grid (+0x14 /
// +0x15 column / row, +0x16 / +0x17 columns / rows, +0x18 page, +0x1a pages, +0x1c entries, +0x1e mode: 1 only the
// entries can be reached, a last row cut short; +0x10 bit 0 moves, bit 1 pages), moved by 0x7530b8(cursor, buttons).
// Each returns the button that moved it, and the menu then does what a step of the d-pad does (the item's details, the
// sound). Both are hooked: when the pointer moves over another row or cell of the cursor being moved, the update runs
// once a step with a d-pad button of its own, in the same update, the steps' sounds muted but the last, so the menu
// sees ordinary moves. A notch past a list's first or last row is the page's own step (+0x08 4 / 8: the menu fills the
// new page's rows), the row then set to the new page's last or first; notches left wait for the next update.
//
// Where the rows and cells are: the menus' drawing objects place their cursor's highlight (a GUI instance: world matrix
// at +0x10, translation +0x40, place in its parent +0x90; gui = the object's +0x24) at the row or cell times a pitch,
// each frame: cUIOItemBox (the item box and the shops' and the smithy's boxes, items or equipment: 0x7b68e4, cursor
// +0x58, instance 7, 36 a cell), cUIOItemPouch (0x7bb5ac, cursor +0x4c, instance 0x2e, pitch +0x98), cUIOListWindow's
// (0x7c56e8, cursor +0x54, instance +0x88, pitch +0x58: the item box's menus, the housekeeper's), cUIOEquipList's
// (0x789c74) and many others' through 0x77033c(object, instance, row instance, row): the pause menu; the pitch is the
// row instance's place, the cursor the one moved lately that the object keeps. A list's highlight is a polygon whose top
// left is the instance's translation and whose size is twice its scale (seen: the pouch's at 872,101, 380 x 34, as the
// renderer drew it). That is the game's 1280x720 canvas; on a wider or narrower picture the renderer moves the menus
// (nvn/vulkan/exec.c cbuf_info), so the pointer goes back to the canvas through where the menu pieces of the last frame
// went (21:9 and 16:10 tested, windowed and in full screen).
//
// A click is A one update after the cursor reached its cell (most menus test A before they move their cursor), and in
// that update no cursor is moved here (some menus test A after the cursor's update: the row clicked is the one
// confirmed). A notch or a side button that comes with a click is dropped: A goes to what was clicked. A quantity box
// (0x74fa74: the item box's, the shops') takes the wheel as Up / Down, +1 / -1, and a click anywhere as its A. A choice
// of two side by side (yes / no: 0x74fbb0, 0x74fc94, 0x74fd94) keeps the menu open and takes the wheel to move between
// the two; the pointer picks one and a click on one answers with it where their places are known: the help box's at
// the bottom left (the item box's, the shops': "sell?"), the talk window's (the same parts) and the message window's in
// the middle ("quit the game?"); where they are not known a click is no answer (the wheel picks one, F or a right
// click answer it: menutrace names its caller, to add its window). A click no cursor, quantity or choice took in its
// first two updates is the game's A (written into the menu's word as theirs, for one update), on what has the focus:
// the talk window and the message window read their buttons there too (an A written into it, "menupress 2000", went
// on to a talk's next line; B closed it). Not when it was on a row or cell of a list or a box whose cursor runs that
// did not take it (a panel without the focus: no A for the other one; a list left still under a message is no such
// panel, but under a yes / no a click there is no answer either), nor within 250 ms of the quantity, the choice or the
// message now shown (a message window that had not waited): the second half of the double click that opened it is not
// its A. Nor is it a click on a row or a cell, or anything, within 250 ms of a list or a box coming (a cursor that had
// not run in the last two updates): a double click on the pause menu's Pouch or the housekeeper's Gallery opened what
// the second click then fell on.
//
// Presses go into the pressed-this-update word only, and only while a menu or a dialog is open. A menu: from a
// cursor's update (a quantity's, a choice's) until none has run for two updates; after that a menu window still drawn
// (the renderer's test, as the touch controls': layer 97, not the Quest Complete stamp nor the chat's log) keeps it open
// a moment, a closing fade, at most 300 ms after the last cursor (a gun's aim and a chat bubble draw on that layer too).
// A dialog: while the talk window is shown (its update, 0x83d010, runs only then) or the message window waits for a
// button (+0x4c 1 or 2), and two updates more; the window objects' own state, not what is drawn, so nothing in a hunt
// keeps it (the talk window stays shown from one line to the next: no gap between them). The game's thread says
// whether one is open, so a slow update or a load in a menu does not hand the mouse back to the camera there; only
// updates stopped for 3 s end it. Outside the game's menus and dialogs, and while the port's menu, the HUD editor, the
// chat or a text entry has the mouse, nothing here acts. (The field does not read the menu's word: an A written into it
// in front of the item box with its prompt shown did not open it; the controller's A did. So nothing written here
// reaches the hunter.)
//   control file: "menutrace [updates]" logs the cursors moved, the menus' geometry, the dialogs, the yes / no's windows
//   and the pointer; "menustall <ms>" makes the game's next update that slow (debug)
#include "mousemenu.h"

#include "../hle.h"

#define G_USERINFO      0x1a97f78u  // sUserInfo *: +0x446e the pages turn with ZL / ZR, not L / R (an option)

// The game's option for the page buttons, on both platforms: the touch controls' menu pad presses and labels its page
// tabs by it (runtime/touch.cpp), the mouse's side buttons press that pair (page_bits)
bool menu_pages_zl(void) {
    uint32_t ui = LD32(G_USERINFO);
    return ui >= 0x08000000u && ui < 0xF0000000u && !(ui & 3) && LD8(ui + 0x446e) != 0;
}

#define FN_PAD_UPDATE   0x5ddf1cu  // sFestaPad::update (r0 = the pad system)
#define FN_CURSOR       0x74ee98u  // cCursor::update(cursor, buttons) -> the button that moved it
#define FN_BOX_CURSOR   0x7530b8u  // cBoxCursor::update(cursor, buttons) -> the button that moved it
#define FN_QUANTITY     0x74fa74u  // a quantity box: (s16 *value, max, pressed, pressed | repeated, out *) -> 1 A, 2 B
// a choice of two side by side, yes / no: (u8 *choice, pressed, back, forward) -> 1 yes, 2 no, 0 none yet; three
// alike (the sounds differ): the message window's, the item box's and the shops' (0x74fc94, a fifth argument: sounds on)
#define FN_YES_NO       0x74fbb0u
#define FN_YES_NO_2     0x74fc94u
#define FN_YES_NO_3     0x74fd94u

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }

// ---- a menu taking input (both platforms): the pad system's updates counted, and the last one a menu's cursor, a
// quantity or a choice of two ran in. The touch controls' menu pad presses only while one runs (runtime/touch.cpp): a
// menu closing runs none while its windows still fade out, and the hunter may be played again by then
static uint32_t g_frame;      // pad updates so far (the game thread's: menu_updates for the others)
static uint32_t g_ran_frame;  // the update a cursor (or a quantity, a choice of two) last ran in
static uint64_t g_ran_ns;     // ... and when (0: none yet)
static bool g_ran_ever;
static void counted(void) { __atomic_store_n(&g_frame, g_frame + 1, __ATOMIC_RELEASE); }
static void ran_now(void) {
    __atomic_store_n(&g_ran_frame, g_frame, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ran_ever, true, __ATOMIC_RELEASE);
    g_ran_ns = plat_now_ns();
}
static volatile bool g_in_pad;    // (the game's thread) inside the pad system's update, the count not yet moved on
static volatile bool g_menu_free;  // (menu_set_free)
uint32_t menu_updates(void) { return __atomic_load_n(&g_frame, __ATOMIC_ACQUIRE); }
uint32_t menu_cursor_age(void) {
    if (!__atomic_load_n(&g_ran_ever, __ATOMIC_ACQUIRE)) return 1000000u;
    uint32_t r = __atomic_load_n(&g_ran_frame, __ATOMIC_ACQUIRE);  // (first: the count is never behind it)
    return menu_updates() - r;
}
void menu_set_free(bool free) { g_menu_free = free; }
bool menu_takes_input(void) {
    if (g_menu_free) return true;
    // (in the pad's update, which reads the controller, the count is still the last update's: a cursor ran in it, 0;
    // read later in an update, the one before it is 1 back)
    bool in_pad = g_in_pad;
    static int said;  // (MHGU_TOUCH_LOG: where the game reads the controller, once)
    if (!said && ENV("MHGU_TOUCH_LOG")) said = 1, LOG("[menu] the controller read %s the pad's update\n", in_pad ? "in" : "outside");
    return menu_cursor_age() <= (in_pad ? 0u : 1u);
}

#ifdef __ANDROID__
// (only the count and the cursors' runs: nothing else of the mouse's)
static void pad_update(Cpu *c) {
    g_in_pad = true;
    fnhook_call_original(c, FN_PAD_UPDATE);
    g_in_pad = false;
    counted();
}
static void cursor_update(Cpu *c) {
    if (sane(c->r[0])) ran_now();
    fnhook_call_original(c, FN_CURSOR);
}
static void box_update(Cpu *c) {
    if (sane(c->r[0])) ran_now();
    fnhook_call_original(c, FN_BOX_CURSOR);
}
static void quantity(Cpu *c) {
    ran_now();
    fnhook_call_original(c, FN_QUANTITY);
}
static void yes_no_1(Cpu *c) { ran_now(), fnhook_call_original(c, FN_YES_NO); }
static void yes_no_2(Cpu *c) { ran_now(), fnhook_call_original(c, FN_YES_NO_2); }
static void yes_no_3(Cpu *c) { ran_now(), fnhook_call_original(c, FN_YES_NO_3); }
void mousemenu_init(void) {
    fnhook_set(FN_PAD_UPDATE, hle_register("mousemenu:pad_update", pad_update));
    fnhook_set(FN_CURSOR, hle_register("mousemenu:cursor_update", cursor_update));
    fnhook_set(FN_BOX_CURSOR, hle_register("mousemenu:box_update", box_update));
    fnhook_set(FN_QUANTITY, hle_register("mousemenu:quantity", quantity));
    fnhook_set(FN_YES_NO, hle_register("mousemenu:yes_no", yes_no_1));
    fnhook_set(FN_YES_NO_2, hle_register("mousemenu:yes_no_2", yes_no_2));
    fnhook_set(FN_YES_NO_3, hle_register("mousemenu:yes_no_3", yes_no_3));
}
bool mousemenu_active(void) { return false; }
bool mousemenu_open(void) { return false; }
bool menu_stick_sideways(void) { return false; }
void mousemenu_pointer(float cx, float cy, bool moved) { (void)cx, (void)cy, (void)moved; }
void mousemenu_hide(void) {}
void mousemenu_click(int button, float cx, float cy) { (void)button, (void)cx, (void)cy; }
void mousemenu_wheel(int notches, float cx, float cy) { (void)notches, (void)cx, (void)cy; }
void mousemenu_trace(int frames) { (void)frames; }
void mousemenu_test_press(uint32_t buttons) { (void)buttons; }
void mousemenu_test_stall(int ms) { (void)ms; }
void mousemenu_block(bool blocked) { (void)blocked; }
int mousemenu_piece(float gx, float gy, float gz, float dx, float dy, float kx, float ky) {
    (void)gx, (void)gy, (void)gz, (void)dx, (void)dy, (void)kx, (void)ky;
    return -1;
}
void mousemenu_piece_y(int piece, float dy) { (void)piece, (void)dy; }
void mousemenu_menu(void) {}
void mousemenu_frame(void) {}
#else
#include <math.h>
#include <pthread.h>
#include <string.h>

#include <SDL3/SDL_mouse.h>

#include "../config.h"

#define FN_SE           0x6eeb78u  // a menu sound (sound system, id): cBoxCursor's steps
#define FN_UIO_ITEMBOX  0x7b68e4u  // cUIOItemBox, each frame (vtable slot 19)
#define FN_UIO_POUCH    0x7bb5acu  // cUIOItemPouch, each frame
#define FN_LIST_CURSOR  0x7c56e8u  // cUIOListWindow: its cursor's highlight placed (slot 27)
#define FN_ROW_CURSOR   0x77033cu  // cUIObject: (object, instance, row instance, row) a highlight at row * the pitch
#define FN_EQUIP_LIST   0x789c74u  // cUIOEquipList: its cursor's (+0x4c) highlight, instance 14, rows 46 apart
// the game's dialogs, which read their A and B from the menu's word too: the talk window (cUIOTalkWindow, bottom left,
// the people's lines: 0x83d010 runs while it is shown, +0x4c 1, 3 or 4; its yes / no is 0x74fbb0 on a copy of its
// +0x58, called from 0x83d234) and the message window in the middle (cUIOWindowDialog: 0x84bc40 each frame, +0x4c 2 while
// it is shown and waits for a button, +0x50 what for: 1 / 4 a yes / no, 10 / 12 one with touch buttons, 11 an OK, else
// A; +0x5c the frames it takes no button yet, +0x6c its choice)
#define FN_TALK         0x83d010u
#define FN_DIALOG       0x84bc40u
#define LR_TALK_YES_NO  0x83d238u
#define VT_TALK         0x19774b4u
#define FN_GUI_ANIM     0x746c74u  // (gui, instance, sequence): an instance's animation (traced only)
#define G_UI_MANAGER    0x1a38bd8u  // ** the UI manager: +0x148 the base screen, its +0x34 the shared UI objects (0x85cb3c)
#define FN_GUI_SETX     0x745a40u  // (gui, instance, x): an instance's place in its parent (traced only)
#define FN_GUI_SETY     0x745ac0u
#define G_PAD           0x1a8c670u  // sFestaPad *
#define AXIS_TABLE      0x182d0c0u  // cCursor's axes: 16 bytes each, {back button, forward button, sound...}
#define VT_CCURSOR_ROWS 0x2cu       // cCursor vtable slot 11: the rows on its page now
// The help box at the bottom left (shared UI object 4: cUIOUnderExpWindow, or cUIOKaiwaExpWindow, its child) shows the
// item box's, the shops' yes / no (0x85e5bc): its gui's instance 7, its animation the left one picked (sequence 0xf4245)
// or the right one (0xf4246); the talk window its own alike. The message window (shared object 8, cUIOWindowDialog
// 0x1978e54) shows its own in instance 2 (0xf4243 / 0xf4244)
#define VT_UNDER_EXP    0x1977b48u
#define VT_KAIWA_EXP    0x1977bc0u
#define VT_DIALOG       0x1978e54u
#define SEQ_HELP_LEFT   0xf4245u
#define SEQ_HELP_RIGHT  0xf4246u
#define SEQ_DLG_LEFT    0xf4243u
#define SEQ_DLG_RIGHT   0xf4244u
#define PAD_ON          0x6bf4u
#define PAD_TRG         0x6bfcu
#define PAD_REP         0x6c34u
#define HUNTER_ON       0x6ba4u  // (the hunter's half: held, +8 pressed this update, as the menu's)

#define B_UP    0x10u
#define B_RIGHT 0x20u
#define B_DOWN  0x40u
#define B_LEFT  0x80u
#define B_DIRS  0xf0u
#define B_PAGES 0xf00u   // L R ZL ZR
#define B_STICK 0xff0000u
#define B_STICK_SIDE 0xa0000u  // the left stick's left (0x80000) and right (0x20000), as the d-pad's shifted 12 up
#define B_A     0x2000u
#define B_B     0x4000u

#define CLICK_UPDATES 2               // a click no cursor took in the first two updates that saw it: the game's A
#define CLICK_MAX_NS  1000000000ull   // (one older than this, after a long stall, is dropped)
#define PRESS_NS      300000000ull    // a right click, a page button or a notch waiting for a menu while none is open
#define SETTLE_NS     250000000ull    // a list, a box, a quantity, a choice or a message takes a click made this long
                                      // after it came (not the second half of the double click that opened it)
#define DRAWN_NS      150000000ull    // a menu's window drawn this recently keeps one open (a closing fade)...
#define KEEP_NS       300000000ull    // ...this long at most after a cursor last ran
#define STOPPED_NS    3000000000ull   // the game's updates stopped this long: no menu believed open any more
#define MAX_WAIT      4               // notches, page presses waiting at most

// ---- the pointer and the buttons (host main thread -> game thread, under g_lock)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_px, g_py;           // the pointer on the canvas, as drawn
static uint32_t g_moves;           // pointer movements so far
static bool g_hidden;              // a key or a controller was used since it last moved: no hover
// a left click waiting for its cursor (ns 0: none); seen: the pad update that first saw it (0: none yet)
static struct { float x, y; uint64_t ns; uint32_t seen; } g_click;
static uint64_t g_b_ns;            // a right click waiting (0: none)
static int g_page, g_wheel;        // side buttons (< 0 back), notches (> 0 up) waiting
static uint64_t g_page_ns, g_wheel_ns;

static volatile bool g_session_on;     // the game thread's g_session, for the host
static volatile uint64_t g_beat_ns;    // the game thread's last pad update
static volatile uint64_t g_drawn_ns;   // when a menu's window was last drawn (the render thread)
static volatile int g_trace;           // (debug) pad updates left to trace
static int g_trace_id;                 // (debug) traces so far
static volatile bool g_blocked;        // the port's menu, the HUD editor, the chat, a text entry: the mouse is theirs
static volatile uint32_t g_test_press; // (debug) menu buttons pressed at the next update, menu or not
static volatile int g_test_stall;      // (debug) ms the next update waits (a slow update, a load)

// ---- the game thread's
static uint32_t g_geo_frame;  // ... a cursor with its rows or cells known
static uint32_t g_two_frame;  // ... a choice of two (the wheel is its)
static uint64_t g_two_since;  // ... since when one has run, update after update
static uint32_t g_qty_frame;  // a quantity box: the same
static uint64_t g_qty_since;
static uint32_t g_dlg_frame;  // a dialog (the talk window shown, the message window waiting): the same
static uint64_t g_dlg_ns, g_dlg_since;
static uint32_t g_msg_frame;  // the message window waiting (its own: one a talk's line brings up comes anew)
static uint64_t g_list_since; // a list or a box came (a cursor that had not run): a click elsewhere is not its A yet
static uint32_t g_blind_frame, g_blind_lr;  // a choice of two whose answers are not known ran (and its caller)
// a quantity, a choice, a message answered: the next one comes anew, whatever the updates between them
static bool g_qty_done, g_two_done, g_dlg_done;
static uint32_t g_talk;       // the talk window shown lately (a yes / no from it is its own)
static uint32_t g_two_moves;  // the pointer movements a choice's hover has seen
static bool g_session;        // a menu open: since a cursor ran, while one runs (or for a moment a menu window is drawn);
                              // a dialog: while it is shown
static uint32_t g_injected;   // buttons written into the pressed word this update (taken out at the next)
static bool g_click_a;        // A at the next update: a click reached its cell
static bool g_a_update;       // this update's A is a click's: no cursor is moved by the mouse in it
static Cpu *g_mute;           // the steps' sounds muted, on this thread (cBoxCursor: FN_SE)

typedef struct {
    uint32_t cursor, uio;
    bool grid;
    float x0, y0;        // where the highlight is at row or cell 0, on the canvas (a cell's top left)
    float px, py;        // the pitch between columns, rows
    float w;             // a list row's width
    float ref_x, ref_y;  // where the highlight is drawn now
    float z;             // its layer
    int cols, rows;      // (its cursor's columns and rows a page: where a click is on it)
    uint32_t frame;      // the update it was seen in
    uint32_t moves;      // the pointer movements its hover has seen
} Geo;
#define MAX_GEO 16
static Geo g_geo[MAX_GEO];
#define MAX_RAN 32
static struct { uint32_t cursor, frame; } g_ran[MAX_RAN];  // the cursors moved lately (0x77033c's objects name theirs)
// A list whose left / right is the left stick's alone (its rows' or its pages' axis: cCursor's axes 6 and 7, 0x80000 /
// 0x20000, the axis table at 0x182d0c0; 8 is the d-pad's or the stick's, 2 / 3 the d-pad's alone) ran in the last two
// updates: tabs that a controller's stick turns and its d-pad does not. The move keys in a menu are the d-pad
// (host_input.c): there their left / right are the stick's again
static uint32_t g_side_frame;
static bool g_side_ever;
bool menu_stick_sideways(void) {
    if (!__atomic_load_n(&g_side_ever, __ATOMIC_ACQUIRE)) return false;
    uint32_t f = __atomic_load_n(&g_side_frame, __ATOMIC_ACQUIRE);
    return menu_updates() - f <= 2;
}

// ---- the render thread's: each menu piece of the frame being drawn, then of the last one presented
typedef struct { float gx, gy, gz, dx, dy, kx, ky; } Piece;
#define MAX_PIECES 4096
static Piece g_draw[MAX_PIECES], g_shown[MAX_PIECES];
static int g_ndraw, g_nshown;
static bool g_draw_menu;  // a menu's window among them (exec.c's test: mousemenu_menu)
static pthread_mutex_t g_piece_lock = PTHREAD_MUTEX_INITIALIZER;

int mousemenu_piece(float gx, float gy, float gz, float dx, float dy, float kx, float ky) {
    if (g_ndraw == MAX_PIECES || kx <= 0.0f || ky <= 0.0f) return -1;
    g_draw[g_ndraw] = (Piece){gx, gy, gz, dx, dy, kx, ky};
    return g_ndraw++;
}
void mousemenu_piece_y(int piece, float dy) {
    if (piece >= 0 && piece < g_ndraw) g_draw[piece].dy = dy;
}
void mousemenu_menu(void) { g_draw_menu = true; }
void mousemenu_frame(void) {
    pthread_mutex_lock(&g_piece_lock);
    memcpy(g_shown, g_draw, sizeof(Piece) * (size_t)g_ndraw);
    g_nshown = g_ndraw;
    pthread_mutex_unlock(&g_piece_lock);
    if (g_draw_menu) g_drawn_ns = plat_now_ns();
    g_ndraw = 0, g_draw_menu = false;
}

// The canvas point (x, y), as drawn, on the game's canvas: through the last frame's menu piece nearest (near_x,
// near_y) on layer z (the pieces of one window are moved alike); as it is with no pieces
static void to_game(float x, float y, float near_x, float near_y, float z, float *gx, float *gy) {
    pthread_mutex_lock(&g_piece_lock);
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < g_nshown; i++) {
        const Piece *p = &g_shown[i];
        float d = (p->gx - near_x) * (p->gx - near_x) + (p->gy - near_y) * (p->gy - near_y);
        if (fabsf(p->gz - z) > 0.5f) d += 1e6f;  // (another layer: a window over or under it)
        if (d < bd) bd = d, best = i;
    }
    Piece p = best >= 0 ? g_shown[best] : (Piece){0, 0, 0, 0, 0, 1, 1};
    pthread_mutex_unlock(&g_piece_lock);
    *gx = p.gx + (x - p.dx) / p.kx, *gy = p.gy + (y - p.dy) / p.ky;
}

// ---- GUI instances
static uint32_t gui_instance(uint32_t gui, uint32_t id) {
    if (!sane(gui)) return 0;
    uint32_t n = LD32(gui + 0x298), arr = id < n ? LD32(gui + 0xf8) : LD32(gui + 0x294);
    if (!sane(arr)) return 0;
    uint32_t inst = LD32(arr + 4 * (id < n ? id : id - n));
    return sane(inst) ? inst : 0;
}

static Geo *geo_find(uint32_t cursor) {
    for (int i = 0; i < MAX_GEO; i++)
        if (g_geo[i].cursor == cursor && g_frame - g_geo[i].frame <= 1) return &g_geo[i];
    return NULL;
}

// A drawing object placed its cursor's highlight: where its rows or cells are now. A list: x0, y0 its row 0's top
// left, a row the highlight's width and the pitch high; a grid: x0, y0 cell 0's, cells pitch apart (inset: from the
// highlight's place to its cell's top left)
static void geo_seen(uint32_t cursor, uint32_t uio, uint32_t inst_id, bool grid, float pitch, float inset) {
    if (!sane(cursor) || !sane(uio) || !(pitch > 1.0f && pitch < 400.0f)) return;
    uint32_t inst = gui_instance(LD32(uio + 0x24), inst_id);
    if (!inst) return;
    int slot = -1;
    for (int i = 0; i < MAX_GEO && slot < 0; i++)
        if (g_geo[i].cursor == cursor) slot = i;
    for (int i = 0; i < MAX_GEO && slot < 0; i++)
        if (g_frame - g_geo[i].frame > 4) slot = i;
    if (slot < 0) return;
    Geo *g = &g_geo[slot];
    if (g->cursor != cursor || g->uio != uio) memset(g, 0, sizeof *g), g->moves = g_moves;
    float tx = LDF(inst + 0x40), ty = LDF(inst + 0x44), lx = LDF(inst + 0x90), ly = LDF(inst + 0x94);
    bool stale = (LD32(inst + 0x54) & 0x10000) != 0;  // (moved since its world matrix was made: next frame)
    if (!isfinite(tx) || !isfinite(ty) || !isfinite(lx) || !isfinite(ly)) return;
    if (stale && g->cursor == cursor) {  // (keep where its rows were)
        g->frame = g_frame;
        return;
    }
    g->cursor = cursor, g->uio = uio, g->grid = grid, g->frame = g_frame;
    g->x0 = tx - lx + inset, g->y0 = ty - ly + inset, g->px = g->py = pitch;
    g->w = 2.0f * fabsf(LDF(inst + 0x10));
    if (grid) g->w = pitch;
    g->cols = grid ? LD8(cursor + 0x16) : 1, g->rows = grid ? LD8(cursor + 0x17) : LD8(cursor + 0xe);
    g->ref_x = tx, g->ref_y = ty, g->z = 100.0f + LDF(inst + 0x48);
    if (g_trace > 0)
        LOG("[menu] geo %s cursor %08x uio %08x vt %08x inst %u: row 0 at %.1f,%.1f pitch %.1f width %.1f z %.1f "
            "(world %.1f,%.1f scale %.2f,%.2f local %.1f,%.1f)\n", grid ? "grid" : "list", cursor, uio, LD32(uio), inst_id,
            g->x0, g->y0, pitch, g->w, g->z, tx, ty, LDF(inst + 0x10), LDF(inst + 0x24), lx, ly);
}

// The row or cell (column + row * columns) of g under the canvas point (x, y), as drawn; -1: none
static int cell_at(const Geo *g, float gx, float gy, int cols, int rows) {
    float fx = (gx - g->x0) / g->px, fy = (gy - g->y0) / g->py;
    if (!g->grid) {
        if (gx < g->x0 || gx > g->x0 + g->w) return -1;
        fx = 0.0f;
    }
    int i = (int)floorf(fx), j = (int)floorf(fy);
    return i < 0 || j < 0 || i >= cols || j >= rows ? -1 : i + j * cols;
}
// (only the first `valid` of them are there: a grid's last page cut short)
static int hit(const Geo *g, float x, float y, int cols, int rows, int valid) {
    float gx, gy;
    to_game(x, y, g->ref_x, g->ref_y, g->z, &gx, &gy);
    int k = cell_at(g, gx, gy, cols, rows);
    if (k < 0) return -1;
    // (again through the piece nearest that cell: a window that spans both sides of a wide picture's middle)
    float cx = g->x0 + (float)(k % cols) * g->px + 0.5f * g->px, cy = g->y0 + (float)(k / cols) * g->py + 0.5f * g->py;
    to_game(x, y, cx, cy, g->z, &gx, &gy);
    k = cell_at(g, gx, gy, cols, rows);
    return k < valid ? k : -1;
}

// ---- a cursor's update, run again as the menu calls it, with one button
static uint32_t step(Cpu *c, uint32_t fn, uint32_t cursor, uint32_t buttons, bool mute) {
    uint32_t saved[16];
    memcpy(saved, c->r, sizeof saved);
    uint8_t sound = LD8(cursor + 0x20);
    bool list = fn == FN_CURSOR;
    if (mute && list) ST8(cursor + 0x20, 0);  // (cCursor: no sound with +0x20 off)
    if (mute && !list) g_mute = c;            // (cBoxCursor: FN_SE skipped)
    c->r[0] = cursor, c->r[1] = buttons;
    fnhook_call_original(c, fn);
    uint32_t moved = c->r[0];
    if (mute && list) ST8(cursor + 0x20, sound);
    g_mute = NULL;
    memcpy(c->r, saved, sizeof saved);
    return moved;
}
static uint32_t call1(Cpu *c, uint32_t fn, uint32_t a0) {
    uint32_t saved[16];
    memcpy(saved, c->r, sizeof saved);
    c->r[0] = a0;
    guest_call(c, fn);
    uint32_t v = c->r[0];
    memcpy(c->r, saved, sizeof saved);
    return v;
}

static void page_bits(uint32_t *back, uint32_t *fwd) {  // L / R, or ZL / ZR by the game's option
    bool z = menu_pages_zl();
    *back = z ? 0x400u : 0x100u, *fwd = z ? 0x800u : 0x200u;
}
static void axis_bits(uint32_t type, uint32_t *back, uint32_t *fwd) {
    *back = *fwd = 0;
    if (type > 11) return;
    *back = LD32(AXIS_TABLE + 16 * type), *fwd = LD32(AXIS_TABLE + 16 * type + 4);
    if ((0x630u >> type) & 1) page_bits(back, fwd);  // (the L / R axes follow the option)
}

// What the mouse asks of a cursor moved now: the cell a click or a hover wants (-1: none), the notches, the pages.
// valid: the cells there are (a click past them is no click on this cursor). A notch or a side button that comes with
// a click is dropped: A goes to the cell clicked. A click made as a list or a box came is dropped (the second half of
// the double click that opened it: not on the row of the new list it falls on)
typedef struct { int target; bool click; int wheel, page; } Want;
static Want want_for(Geo *g, int cols, int rows, int valid) {
    Want w = {-1, false, 0, 0};
    pthread_mutex_lock(&g_lock);
    if (g_click.ns && g_click.ns < g_list_since + SETTLE_NS) {
        if (g_trace > 0) LOG("[menu] a click at %.1f,%.1f dropped: a list or a box that came now\n", g_click.x, g_click.y);
        g_click.ns = 0;
    }
    if (g_click.ns) {
        int k = hit(g, g_click.x, g_click.y, cols, rows, valid);
        if (k >= 0) w.target = k, w.click = true, g_click.ns = 0, g_wheel = g_page = 0;
    }
    if (!w.click) {
        if (!g_hidden && g->moves != g_moves) w.target = hit(g, g_px, g_py, cols, rows, valid);
        w.wheel = g_wheel, w.page = g_page;
        g_wheel = g_page = 0;
    }
    g->moves = g_moves;
    pthread_mutex_unlock(&g_lock);
    return w;
}
// Notches a list could not take now (a page turned: the menu fills the new page's rows first) wait for the next update
static void wheel_back(int notches) {
    if (!notches) return;
    pthread_mutex_lock(&g_lock);
    if (!g_wheel || (g_wheel > 0) == (notches > 0)) {  // (not if it was turned the other way since)
        g_wheel += notches;
        if (g_wheel > MAX_WAIT) g_wheel = MAX_WAIT;
        if (g_wheel < -MAX_WAIT) g_wheel = -MAX_WAIT;
        g_wheel_ns = plat_now_ns();
    }
    pthread_mutex_unlock(&g_lock);
}

static void trace_cursor(const char *what, uint32_t cur, uint32_t lr, uint32_t bits, uint32_t moved, const char *did) {
    if (!strcmp(what, "list"))
        LOG("[menu] cCursor %08x vt %08x lr %08x bits %08x -> %x%s: row %u rows %u page %u pages %u count %u axis %u/%u snd %u "
            "wrap %u kind %u state %u\n", cur, LD32(cur), lr, bits, moved, did, LD8(cur + 0xd), LD8(cur + 0xe), LD16(cur + 0x10),
            LD16(cur + 0x12), LD16(cur + 0x14), LD32(cur + 0x18), LD32(cur + 0x1c), LD8(cur + 0x20), LD8(cur + 0xc),
            LD32(cur + 8), LD32(cur + 4));
    else
        LOG("[menu] cBoxCursor %08x vt %08x lr %08x bits %08x -> %x%s: x %u y %u cols %u rows %u page %u pages %u total %u "
            "mode %u wrap %u flags %x state %u\n", cur, LD32(cur), lr, bits, moved, did, LD8(cur + 0x14), LD8(cur + 0x15),
            LD8(cur + 0x16), LD8(cur + 0x17), LD16(cur + 0x18), LD16(cur + 0x1a), LD16(cur + 0x1c), LD16(cur + 0x1e),
            LD8(cur + 0x20), LD8(cur + 0x10), LD32(cur + 4));
}

// The cursor's state (vtable slot 7): 2 hidden, 1 a panel without the focus (its highlight grey)
static uint32_t cursor_state(Cpu *c, uint32_t cur) {
    uint32_t vt = LD32(cur), fn = vt >= 0x00200000u && vt < 0x02347000u ? LD32(vt + 0x1c) : 0;
    return fn ? call1(c, fn, cur) : 0;
}

// A cursor ran now (0: a quantity, a choice). One that had not run in the last two updates: a list or a box comes now
// (the second half of the double click that opened it is no click on its rows: want_for, click_not_a)
static void ran(uint32_t cur) {
    ran_now();
    uint64_t now = g_ran_ns;
    if (!cur) return;
    int slot = -1, old = 0;
    for (int i = 0; i < MAX_RAN && slot < 0; i++)
        if (g_ran[i].cursor == cur) slot = i;
    if (slot >= 0 && g_frame - g_ran[slot].frame <= 2) {
        g_ran[slot].frame = g_frame;
        return;
    }
    g_list_since = now;
    if (g_trace > 0) LOG("[menu] cursor %08x vt %08x comes\n", cur, LD32(cur));
    for (int i = 1; i < MAX_RAN && slot < 0; i++)  // (else the one that ran longest ago)
        if (g_frame - g_ran[i].frame > g_frame - g_ran[old].frame) old = i;
    if (slot < 0) slot = old;
    g_ran[slot].cursor = cur, g_ran[slot].frame = g_frame;
}
static bool cursor_ran(uint32_t cur) {  // (in the last two updates)
    for (int i = 0; i < MAX_RAN; i++)
        if (g_ran[i].cursor == cur) return g_frame - g_ran[i].frame <= 2;
    return false;
}

// cCursor::update: a list
static void cursor_update(Cpu *c) {
    uint32_t cur = c->r[0], bits = c->r[1], lr = c->r[14];
    if (!sane(cur)) { fnhook_call_original(c, FN_CURSOR); return; }
    ran(cur);
    Geo *g = config_live_mouse_menus() && !g_blocked ? geo_find(cur) : NULL;
    uint32_t back, fwd, pback, pfwd;
    axis_bits(LD32(cur + 0x18), &back, &fwd);
    axis_bits(LD32(cur + 0x1c), &pback, &pfwd);
    uint32_t axes = back | fwd | pback | pfwd;
    if ((axes & B_STICK_SIDE) && !(axes & (B_LEFT | B_RIGHT)) && cursor_state(c, cur) != 2) {  // (not hidden)
        __atomic_store_n(&g_side_frame, g_frame, __ATOMIC_RELEASE);
        __atomic_store_n(&g_side_ever, true, __ATOMIC_RELEASE);
    }
    // (a list across the screen, its rows side by side, has no geometry of the kind kept here)
    bool vertical = back == B_UP && fwd == B_DOWN;
    if (!g || !vertical || g_a_update || (bits & (B_DIRS | B_PAGES | B_STICK)) || LD16(cur + 0x14) == 0 ||
        cursor_state(c, cur) != 0) {
        // (keys or a controller moved it, or it has not the focus: the pointer waits until it moves. Not in a click's A
        // update: a pointer moved since goes on to its row once the A is taken)
        if (g && !g_a_update) g->moves = g_moves;
        fnhook_call_original(c, FN_CURSOR);
        if (g_trace > 0) trace_cursor("list", cur, lr, bits, c->r[0], g_a_update && g ? " (A)" : "");
        return;
    }
    g_geo_frame = g_frame;
    uint32_t rows_fn = LD32(LD32(cur) + VT_CCURSOR_ROWS);
    int rows = (int)(call1(c, rows_fn, cur) & 0xff), row = LD8(cur + 0xd);
    Want w = want_for(g, 1, rows, rows);
    uint32_t moved = 0;
    const char *did = "";
    if (w.target >= 0 && w.target != row) {  // to the row under the pointer, a step at a time, the last one heard
        int d = w.target - row, n = d > 0 ? d : -d;
        for (int k = 0; k < n; k++) {
            int before = LD8(cur + 0xd);
            moved = step(c, FN_CURSOR, cur, d > 0 ? fwd : back, k < n - 1);
            if (!moved || LD8(cur + 0xd) != before + (d > 0 ? 1 : -1)) break;
        }
        did = w.click ? " (click)" : " (hover)";
    } else if (w.wheel) {  // a notch: a row; past the page's first or last row, the page before or after
        bool up = w.wheel > 0, wrap = LD8(cur + 0xc) != 0;
        int n = up ? w.wheel : -w.wheel, page = LD16(cur + 0x10), pages = LD16(cur + 0x12);
        int room = up ? row : rows - 1 - row;
        if (room < 0) room = 0;
        int k = n < room ? n : room;
        bool turn = n > room && pages >= 2 && pback && (wrap || (up ? page > 0 : page < pages - 1));
        for (int i = 0; i < k; i++) {
            moved = step(c, FN_CURSOR, cur, up ? back : fwd, i < k - 1 || turn);
            if (!moved) { turn = false; break; }
        }
        if (turn) {
            // (the page's own step, which the menu takes for a page turn and fills the page's rows by, and the row
            // then set: the new page's last going up, its first going down; the step keeps the row)
            uint32_t paged = step(c, FN_CURSOR, cur, up ? pback : pfwd, false);
            if (paged) {
                int now_rows = (int)(call1(c, rows_fn, cur) & 0xff);
                ST8(cur + 0xd, up && now_rows > 0 ? now_rows - 1 : 0);
                moved = paged;
                wheel_back(up ? n - k - 1 : -(n - k - 1));
            }
        }
        did = turn ? " (wheel, page)" : " (wheel)";
    } else if (w.page && pback && LD16(cur + 0x12) >= 2) {  // the side buttons: its pages' own buttons
        for (int n = w.page > 0 ? w.page : -w.page; n > 0; n--) moved = step(c, FN_CURSOR, cur, w.page > 0 ? pfwd : pback, n > 1);
        did = " (page)";
    } else {
        fnhook_call_original(c, FN_CURSOR);
        moved = c->r[0];
    }
    if (w.click) g_click_a = true;
    c->r[0] = moved;
    if (g_trace > 0) trace_cursor("list", cur, lr, bits, moved, did);
}

// cBoxCursor::update: a grid
static void box_update(Cpu *c) {
    uint32_t cur = c->r[0], bits = c->r[1], lr = c->r[14];
    if (!sane(cur)) { fnhook_call_original(c, FN_BOX_CURSOR); return; }
    ran(cur);
    Geo *g = config_live_mouse_menus() && !g_blocked ? geo_find(cur) : NULL;
    int cols = LD8(cur + 0x16), rows = LD8(cur + 0x17);
    if (!g || g_a_update || (bits & (B_DIRS | B_PAGES | B_STICK)) || !(LD8(cur + 0x10) & 1) || cols < 1 || rows < 1 ||
        cursor_state(c, cur) != 0) {
        if (g && !g_a_update) g->moves = g_moves;
        fnhook_call_original(c, FN_BOX_CURSOR);
        if (g_trace > 0) trace_cursor("grid", cur, lr, bits, c->r[0], g_a_update && g ? " (A)" : "");
        return;
    }
    g_geo_frame = g_frame;
    int x = LD8(cur + 0x14), y = LD8(cur + 0x15), page = LD16(cur + 0x18), total = LD16(cur + 0x1c);
    // (mode 1 reaches only the entries, its last page cut short: the cells past them are not there; elsewhere every
    // cell is, an empty one too)
    int per = cols * rows, valid = per;
    bool counted = LD16(cur + 0x1e) == 1 && total;
    if (counted) valid = total - page * per < 0 ? 0 : total - page * per > per ? per : total - page * per;
    Want w = want_for(g, cols, rows, valid);
    uint32_t moved = 0;
    const char *did = "";
    if (w.target >= 0 && w.target != x + y * cols) {
        int tx = w.target % cols, ty = w.target / cols;
        // (a partly filled last row has fewer columns: leave it upward first, or reach it last)
        bool row_full = !counted || page * per + (y + 1) * cols <= total;
        int order[2] = {row_full ? 0 : 1, row_full ? 1 : 0}, left = (tx > x ? tx - x : x - tx) + (ty > y ? ty - y : y - ty);
        for (int o = 0; o < 2; o++) {
            bool horiz = order[o] == 0;
            int from = horiz ? LD8(cur + 0x14) : LD8(cur + 0x15), to = horiz ? tx : ty, n = to > from ? to - from : from - to;
            uint32_t b = horiz ? (to > from ? B_RIGHT : B_LEFT) : (to > from ? B_DOWN : B_UP);
            for (int k = 0; k < n; k++) {
                int before = horiz ? LD8(cur + 0x14) : LD8(cur + 0x15);
                moved = step(c, FN_BOX_CURSOR, cur, b, --left > 0);
                int after = horiz ? LD8(cur + 0x14) : LD8(cur + 0x15);
                if (!moved || after != before + (to > from ? 1 : -1) || LD16(cur + 0x18) != page) { o = 2; break; }
            }
        }
        did = w.click ? " (click)" : " (hover)";
    } else if ((w.wheel || w.page) && (LD8(cur + 0x10) & 2) && LD16(cur + 0x1a) >= 2) {  // a notch, a side button: a page
        uint32_t back, fwd;
        page_bits(&back, &fwd);
        int d = w.page ? w.page : -w.wheel;  // (the wheel down: the next page)
        for (int n = d > 0 ? d : -d; n > 0; n--) moved = step(c, FN_BOX_CURSOR, cur, d > 0 ? fwd : back, n > 1);
        did = w.page ? " (page)" : " (wheel)";
    } else {
        fnhook_call_original(c, FN_BOX_CURSOR);
        moved = c->r[0];
    }
    if (w.click) g_click_a = true;
    c->r[0] = moved;
    if (g_trace > 0) trace_cursor("grid", cur, lr, bits, moved, did);
}

static void se(Cpu *c) {
    if (g_mute == c) return;  // (a step of a jump: only the last one is heard)
    fnhook_call_original(c, FN_SE);
}

// A click made a moment after a quantity or a choice came (a double click's second half is not its A)
static bool click_since(uint64_t since) { return g_click.ns && g_click.ns >= since + SETTLE_NS; }

// A quantity box (the item box's, a shop's...): a click anywhere confirms it, as A (the wheel's Up / Down, +1 / -1, and
// a right click's B come as presses, from the pad's update)
static void quantity(Cpu *c) {
    ran(0);
    if (g_qty_done || g_frame - g_qty_frame > 1) g_qty_since = plat_now_ns();  // (it opens now)
    g_qty_frame = g_frame, g_qty_done = false;
    pthread_mutex_lock(&g_lock);
    if (click_since(g_qty_since) && config_live_mouse_menus() && !g_blocked) c->r[2] |= B_A, g_click.ns = 0;
    pthread_mutex_unlock(&g_lock);
    if (g_trace > 0) LOG("[menu] quantity: value %d max %u pressed %x\n", (int16_t)LD16(c->r[0]), c->r[1], c->r[2]);
    fnhook_call_original(c, FN_QUANTITY);
    if (c->r[0] == 1 || c->r[0] == 2) g_qty_done = true;  // (A, B: a quantity after it is another one)
}

// The base screen's shared UI objects (0x85cb3c): 4 the help box, 5 the button guide, 8 the message window
static uint32_t shared_ui(uint32_t i) {
    uint32_t global = LD32(G_UI_MANAGER);  // (its address: static data, below the heap)
    uint32_t mgr = global ? LD32(global) : 0, base = sane(mgr) ? LD32(mgr + 0x148) : 0;
    uint32_t list = sane(base) ? LD32(base + 0x34) : 0;
    return sane(list) ? LD32(list + 4 * i) : 0;
}
// An instance's animation now (its +0xdc, the sequence's id first in what +0x94 points at); 0 none
static uint32_t gui_sequence(uint32_t inst) {
    uint32_t anim = LD32(inst + 0xdc), seq = sane(anim) ? LD32(anim + 0x94) : 0;
    return sane(seq) ? LD32(seq) : 0;
}
// The two answers of the yes / no at `choice` (lr: where the helper was called from) on the game's canvas (x0, y0, x1,
// y1 each, around each answer as its highlight is drawn) and their layer; false where none is shown. Three windows draw
// it with the same parts: the help box (the item box's, the shops' "sell?"; +0x4c bit 4: its yes / no on, 0x842a78) and
// the talk window in their gui's instance 7, the message window in its instance 2. The widget's place is between the
// two, at the top of their row: seen at 432,661 in the item box's "sell?", "Yes" 362,680 and "No" 502,680, their
// highlights 114 wide
static bool choice_boxes(uint32_t choice, uint32_t lr, float box[2][4], float *z, const char **who) {
    uint32_t w, dlg = choice - 0x6c, id = 7, left = SEQ_HELP_LEFT, right = SEQ_HELP_RIGHT;
    if (sane(dlg) && LD32(dlg) == VT_DIALOG) {
        w = dlg, id = 2, left = SEQ_DLG_LEFT, right = SEQ_DLG_RIGHT, *who = "the message window's";
    } else if (lr == LR_TALK_YES_NO) {
        w = g_talk, *who = "the talk window's";
        if (!sane(w) || LD32(w) != VT_TALK) return false;
    } else {
        w = shared_ui(4), *who = "the help box's";
        if (!sane(w) || (LD32(w) != VT_UNDER_EXP && LD32(w) != VT_KAIWA_EXP) || !(LD8(w + 0x4c) & 0x10)) return false;
    }
    uint32_t in = gui_instance(LD32(w + 0x24), id), seq = in ? gui_sequence(in) : 0;
    if (!in || !(LD32(in + 0x54) & 2) || (seq != left && seq != right)) return false;  // (hidden)
    float wx = LDF(in + 0x40), wy = LDF(in + 0x44);
    if (!isfinite(wx) || !isfinite(wy)) return false;
    static const float at[2][4] = {{-132.0f, 1.0f, -7.0f, 37.0f}, {8.0f, 1.0f, 133.0f, 37.0f}};
    for (int i = 0; i < 2; i++)
        box[i][0] = wx + at[i][0], box[i][1] = wy + at[i][1], box[i][2] = wx + at[i][2], box[i][3] = wy + at[i][3];
    *z = 100.0f + LDF(in + 0x48);
    if (!(*z > 50.0f && *z < 150.0f)) *z = 96.0f;
    if (g_trace > 0)
        LOG("[menu] %s yes / no (window %08x flags %x): sequence %x, widget at %.1f,%.1f scale %.2f,%.2f z %.1f\n", *who, w,
            LD32(w + 0x4c), seq, wx, wy, LDF(in + 0x10), LDF(in + 0x24), *z);
    return true;
}
static int panel_at(float box[2][4], float z, float x, float y) {  // (x, y) as drawn: 0, 1 or -1
    for (int i = 0; i < 2; i++) {
        float gx, gy;
        to_game(x, y, 0.5f * (box[i][0] + box[i][2]), 0.5f * (box[i][1] + box[i][3]), z, &gx, &gy);
        if (gx >= box[i][0] && gx < box[i][2] && gy >= box[i][1] && gy < box[i][3]) return i;
    }
    return -1;
}

// A choice of two side by side (yes / no): the wheel moves between them; where its two are known (the help box's, the
// talk window's, the message window's) the pointer picks one (its own back / forward press, the menu's sound) and a
// click on one answers with it: A at the next update, as a cursor's (A comes before the moves in it: A with a move would
// answer with the other one). A click elsewhere is the game's A (pad_update): the one picked. Where they are not
// known no click is its A (one on the other answer would answer with this one): the wheel, F and a right click still
// answer it
static void gui_dump(const char *what, uint32_t obj);
static void yes_no(Cpu *c, uint32_t fn) {
    uint32_t choice = c->r[0], lr = c->r[14];
    ran(0);
    bool opens = g_two_done || g_frame - g_two_frame > 1;
    g_two_frame = g_frame, g_two_done = false;
    bool live = config_live_mouse_menus() && !g_blocked, on = live && !g_a_update;
    float box[2][4], z = 0.0f;
    const char *who = "";
    bool known = live && choice_boxes(choice, lr, box, &z, &who), panels = on && known;
    if (live && !known) {
        if (g_trace > 0 && (g_frame - g_blind_frame > 1 || lr != g_blind_lr))
            LOG("[menu] a yes / no whose answers aren't known (choice %08x lr %08x): no click is its answer\n", choice, lr);
        g_blind_frame = g_frame, g_blind_lr = lr;
    }
    int now = LD8(choice) ? 1 : 0, to = -1;
    const char *did = "";
    pthread_mutex_lock(&g_lock);
    if (opens) g_two_since = plat_now_ns(), g_two_moves = g_moves;  // (it opens now: the pointer picks once it moves)
    if (panels && click_since(g_two_since)) {
        int i = panel_at(box, z, g_click.x, g_click.y);
        if (i >= 0) to = i, g_click.ns = 0, g_click_a = true, g_wheel = 0, did = " (click)";
    }
    if (panels && to < 0 && !g_hidden && g_two_moves != g_moves) {
        to = panel_at(box, z, g_px, g_py);
        if (to >= 0) did = " (hover)";
    }
    if (on && to < 0 && g_wheel) to = g_wheel > 0 ? 0 : 1, did = " (wheel)";
    if (on) g_wheel = 0, g_two_moves = g_moves;
    pthread_mutex_unlock(&g_lock);
    if (to >= 0 && to != now) c->r[1] |= to == 0 ? c->r[2] : c->r[3];  // (its own back / forward: the left / right one)
    if (g_trace > 0) {
        uint32_t dlg = choice - 0x6c, vt = sane(dlg) ? LD32(dlg) : 0;
        LOG("[menu] choice %08x (%08x) lr %08x: %d -> %d%s pressed %x (window vt %08x mode %u%s%s)\n", choice, fn, lr, now, to,
            did, c->r[1], vt, vt == VT_DIALOG ? LD32(dlg + 0x50) : 0, panels ? ", " : "", panels ? who : "");
        static int dumped;  // (debug: what draws the two, once a trace)
        if (dumped != g_trace_id) {
            dumped = g_trace_id;
            if (vt == VT_DIALOG) gui_dump("window", dlg);
            if (lr == LR_TALK_YES_NO) gui_dump("talk window", g_talk);
            gui_dump("help box", shared_ui(4));
            gui_dump("message window", shared_ui(8));
        }
    }
    fnhook_call_original(c, fn);
    if (c->r[0] == 1 || c->r[0] == 2) g_two_done = true;  // (answered: a choice after it is another one)
}
static void yes_no_1(Cpu *c) { yes_no(c, FN_YES_NO); }
static void yes_no_2(Cpu *c) { yes_no(c, FN_YES_NO_2); }
static void yes_no_3(Cpu *c) { yes_no(c, FN_YES_NO_3); }

// ---- the game's dialogs: the pointer shows while one is up (pad_update), and a click there is its A
static void dialog_ran(bool comes) {
    uint64_t now = plat_now_ns();
    if (comes || g_dlg_done || g_frame - g_dlg_frame > 1 || !g_dlg_ns) g_dlg_since = now;  // (one comes now)
    g_dlg_frame = g_frame, g_dlg_ns = now, g_dlg_done = false;
}
// The talk window, while it is shown: a person's lines, their yes / no
static void talk_update(Cpu *c) {
    uint32_t win = c->r[0];
    if (sane(win)) {
        static int dumped;  // (debug: once a trace, what it draws where)
        if (g_trace > 0 && (win != g_talk || g_frame - g_dlg_frame > 1 || dumped != g_trace_id)) {
            LOG("[menu] talk window %08x vt %08x: state %u mode %u result %u choice %u\n", win, LD32(win), LD32(win + 0x4c),
                LD32(win + 0x50), LD32(win + 0x54), LD32(win + 0x58));
            if (dumped != g_trace_id) dumped = g_trace_id, gui_dump("talk window", win);
        }
        g_talk = win;
        dialog_ran(false);
    }
    fnhook_call_original(c, FN_TALK);
}
// The message window, each frame: a dialog while it is shown and waits for a button (one that comes now has its own
// moment with no click, the talk window shown under it or not)
static void dialog_update(Cpu *c) {
    uint32_t dlg = c->r[0];
    uint32_t state = sane(dlg) ? LD32(dlg + 0x4c) : 0;
    if (state == 1 || state == 2) {
        bool comes = g_frame - g_msg_frame > 1;
        if (g_trace > 0 && comes)
            LOG("[menu] message window %08x vt %08x: state %u mode %u wait %d\n", dlg, LD32(dlg), state, LD32(dlg + 0x50),
                (int)LD32(dlg + 0x5c));
        g_msg_frame = g_frame;
        dialog_ran(comes);
    }
    fnhook_call_original(c, FN_DIALOG);
    if (state == 2 && LD32(dlg + 0x4c) == 3) g_dlg_done = true;  // (answered now: a message after it is another one)
}

// ---- the menus' drawing objects (each frame, the game thread): where their cursor's rows or cells are
static void uio_itembox(Cpu *c) {
    uint32_t uio = c->r[0];
    fnhook_call_original(c, FN_UIO_ITEMBOX);
    // (its boxes are 40 x 40 on a 36 pitch, from the highlight's place: seen 880,100, the box 882..918 x 102..138)
    if (sane(uio)) geo_seen(LD32(uio + 0x58), uio, 7, true, 36.0f, 2.0f);
}
static void uio_pouch(Cpu *c) {
    uint32_t uio = c->r[0];
    fnhook_call_original(c, FN_UIO_POUCH);
    if (sane(uio)) geo_seen(LD32(uio + 0x4c), uio, 0x2e, false, LDF(uio + 0x98), 0.0f);
}
static void list_cursor(Cpu *c) {
    uint32_t uio = c->r[0];
    fnhook_call_original(c, FN_LIST_CURSOR);
    if (sane(uio)) geo_seen(LD32(uio + 0x54), uio, LD32(uio + 0x88), false, LDF(uio + 0x58), 0.0f);
}
static void equip_list(Cpu *c) {
    uint32_t uio = c->r[0];
    fnhook_call_original(c, FN_EQUIP_LIST);
    if (sane(uio)) geo_seen(LD32(uio + 0x4c), uio, 0xe, false, 46.0f, 0.0f);
}
// (object, instance, row instance, row): the object's cursor is one of those moved lately whose row that is, kept in
// the object (the pause menu's at +0x4c)
static void row_cursor(Cpu *c) {
    uint32_t uio = c->r[0], inst = c->r[1], ref = c->r[2], row = c->r[3];
    fnhook_call_original(c, FN_ROW_CURSOR);
    if (!sane(uio)) return;
    uint32_t ref_inst = gui_instance(LD32(uio + 0x24), ref);
    if (!ref_inst) return;
    for (uint32_t off = 0x28; off < 0x100; off += 4) {
        uint32_t p = LD32(uio + off);
        if (!sane(p)) continue;
        for (int i = 0; i < MAX_RAN; i++)
            if (g_ran[i].cursor == p && g_frame - g_ran[i].frame <= 1 && LD8(p + 0xd) == row) {
                geo_seen(p, uio, inst, false, LDF(ref_inst + 0x94), 0.0f);
                return;
            }
    }
}

// ---- the pad: presses for the menu (the game thread, each frame)
// A click no cursor, quantity or choice took: the game's A on what has the focus, as the keyboard's A (a talk's next
// line, a message's OK, the answer picked). Not (the reason why; NULL: it is A) while a choice of two whose answers are
// not known runs (a click on the other one would answer with this one), when it was the second half of the double
// click that brought up the list, the box, the quantity, the choice or the message now shown (SETTLE_NS), nor on a row
// or a cell of a list or a box whose cursor runs that did not take it (a panel without the focus: not an A on the other
// one; a list left still under a message is none, under a yes / no a click there is no answer either). g_lock held
static const char *click_not_a(void) {
    uint64_t at = g_click.ns;
    if (g_blind_frame && g_frame - g_blind_frame <= 2) return "a yes / no whose answers aren't known";
    if (at < g_list_since + SETTLE_NS) return "a list or a box that came now";
    if (g_frame - g_qty_frame <= 2 && at < g_qty_since + SETTLE_NS) return "a quantity that came now";
    if (g_frame - g_two_frame <= 2 && at < g_two_since + SETTLE_NS) return "a yes / no that came now";
    if (g_frame - g_dlg_frame <= 2 && at < g_dlg_since + SETTLE_NS) return "a dialog that came now";
    bool asked = g_frame - g_two_frame <= 2;
    for (int i = 0; i < MAX_GEO; i++) {
        const Geo *g = &g_geo[i];
        if (!g->cursor || g_frame - g->frame > 2 || g->cols < 1 || g->rows < 1) continue;
        if (!asked && !cursor_ran(g->cursor)) continue;  // (left still under a message, a talk: the click is theirs)
        float gx, gy, w = g->grid ? (float)g->cols * g->px : g->w;
        to_game(g_click.x, g_click.y, g->ref_x, g->ref_y, g->z, &gx, &gy);
        if (gx >= g->x0 && gx < g->x0 + w && gy >= g->y0 && gy < g->y0 + (float)g->rows * g->py)
            return asked ? "on a list or a box under a yes / no" : "on a list or a box that did not take it";
    }
    return NULL;
}
static void pad_update(Cpu *c) {
    uint32_t pad = c->r[0];
    int stall = __atomic_exchange_n(&g_test_stall, 0, __ATOMIC_ACQ_REL);  // (debug: a slow update)
    if (stall > 0) plat_sleep_until_ns(plat_now_ns() + (uint64_t)stall * 1000000ull);
    if (g_injected && sane(pad)) ST32(pad + PAD_TRG, LD32(pad + PAD_TRG) & ~g_injected);  // (it may not run again)
    g_injected = 0;
    g_in_pad = true;
    fnhook_call_original(c, FN_PAD_UPDATE);
    g_in_pad = false;
    counted();
    uint64_t now = plat_now_ns();
    // (a menu: a cursor, a quantity or a choice ran in the last two updates; after that a menu window still drawn keeps
    // it open a moment, a closing fade, but no longer than KEEP_NS after the last of them: the Quest Complete banner,
    // a gun's aim or a chat bubble, drawn on the menus' layer too, keep nothing open. A dialog: the talk window or the
    // message window ran in the last two updates)
    bool dialog = g_dlg_ns && g_frame - g_dlg_frame <= 2;
    g_session = dialog || (g_ran_ns && (g_frame - g_ran_frame <= 2 ||
                                        (g_session && now - g_drawn_ns < DRAWN_NS && now - g_ran_ns < KEEP_NS)));
    g_session_on = g_session, g_beat_ns = now;
    bool menu = g_session;
    uint32_t want = 0;
    const char *why = "";
    if (g_click_a) want |= B_A, g_click_a = false, why = " (a click's)";
    pthread_mutex_lock(&g_lock);
    // (a click is for the cursors, quantities and choices of the first two updates that see it; then, on nothing they
    // took, it is the game's A: click_not_a)
    if (g_click.ns) {
        if (!g_click.seen) g_click.seen = g_frame;
        else if (g_frame - g_click.seen >= CLICK_UPDATES) {
            const char *no = !menu ? "no menu" : want ? "an A already" : g_blocked ? "blocked" : click_not_a();
            if (!no) want |= B_A, why = " (a click elsewhere)";
            else if (g_trace > 0) LOG("[menu] a click at %.1f,%.1f dropped: %s\n", g_click.x, g_click.y, no);
            g_click.ns = 0;
        }
        if (g_click.ns && now - g_click.ns > CLICK_MAX_NS) g_click.ns = 0;
    }
    bool a_now = want != 0;  // (a click's A: the rest waits for the next update)
    if (g_b_ns && !a_now && (menu || now - g_b_ns >= PRESS_NS)) {
        if (menu) want |= B_B;
        g_b_ns = 0;
    }
    // (the side buttons where no list or box is moved: the page buttons, L / R or ZL / ZR)
    if (g_page && !a_now && g_frame - g_geo_frame > 1 && (menu || now - g_page_ns >= PRESS_NS)) {
        uint32_t back, fwd;
        page_bits(&back, &fwd);
        if (menu) want |= g_page < 0 ? back : fwd;
        g_page += g_page < 0 ? 1 : -1;
        if (!menu) g_page = 0;
    }
    // (the wheel where no list, box or choice of two is moved: Up / Down, a notch an update; a quantity's +1 / -1)
    if (g_wheel && !a_now && g_frame - g_geo_frame > 1 && g_frame - g_two_frame > 1 && (menu || now - g_wheel_ns >= PRESS_NS)) {
        if (menu) want |= g_wheel > 0 ? B_UP : B_DOWN;
        g_wheel += g_wheel > 0 ? -1 : 1;
        if (!menu) g_wheel = 0;
    }
    if (g_blocked) g_click.ns = g_b_ns = 0, g_page = g_wheel = 0, want = 0;
    pthread_mutex_unlock(&g_lock);
    if (!config_live_mouse_menus()) want = 0;
    g_a_update = (want & B_A) != 0;
    want |= __atomic_exchange_n(&g_test_press, 0, __ATOMIC_ACQ_REL);  // (debug: who reads the menu's word)
    if (want && sane(pad)) {
        uint32_t trg = LD32(pad + PAD_TRG);
        g_injected = want & ~trg;
        ST32(pad + PAD_TRG, trg | want);
    }
    if (g_trace > 0) {
        if (sane(pad))
            LOG("[menu] --- pad update %u (frame %llu): on %08x trg %08x rep %08x%s%s injected %x%s; hunter's %08x "
                "%08x, cursor %u updates ago\n", g_frame, (unsigned long long)g_present_count, LD32(pad + PAD_ON),
                LD32(pad + PAD_TRG), LD32(pad + PAD_REP), menu ? " menu" : "", dialog ? " dialog" : "", g_injected,
                g_a_update ? why : "", LD32(pad + HUNTER_ON), LD32(pad + HUNTER_ON + 8), g_frame - g_ran_frame);
        if (--g_trace == 0) {
            LOG("[menu] trace done\n");
            fnhook_set(FN_GUI_SETX, 0), fnhook_set(FN_GUI_SETY, 0), fnhook_set(FN_GUI_ANIM, 0);
        }
    }
}

// ---- (debug) the instance setters: which objects place what, while traced
static void gui_setx(Cpu *c) {
    uint32_t gui = c->r[0], id = c->r[1], lr = c->r[14];
    float v = c->v.f[0];
    fnhook_call_original(c, FN_GUI_SETX);
    if (g_trace > 0) LOG("[menu] setx gui %08x inst %u x %.1f lr %08x\n", gui, id, v, lr);
}
static void gui_sety(Cpu *c) {
    uint32_t gui = c->r[0], id = c->r[1], lr = c->r[14];
    float v = c->v.f[0];
    fnhook_call_original(c, FN_GUI_SETY);
    if (g_trace > 0) LOG("[menu] sety gui %08x inst %u y %.1f lr %08x\n", gui, id, v, lr);
}
static void gui_anim(Cpu *c) {
    uint32_t gui = c->r[0], id = c->r[1], seq = c->r[2], lr = c->r[14];
    fnhook_call_original(c, FN_GUI_ANIM);
    if (g_trace > 0) LOG("[menu] anim gui %08x inst %u sequence %x lr %08x\n", gui, id, seq, lr);
}
// (debug) a UI object's GUI instances, where each one is: what a window draws where
static void gui_dump(const char *what, uint32_t obj) {
    uint32_t gui = sane(obj) ? LD32(obj + 0x24) : 0;
    if (!sane(gui)) return;
    uint32_t n = LD32(gui + 0x298);
    LOG("[menu] %s: object %08x vt %08x gui %08x, %u instances\n", what, obj, LD32(obj), gui, n);
    for (uint32_t id = 0; id < n + 4 && id < 48; id++) {
        uint32_t in = gui_instance(gui, id);
        if (in)
            LOG("[menu]   inst %u %08x vt %08x world %.1f,%.1f scale %.2f,%.2f local %.1f,%.1f z %.1f flags %08x sequence %x\n",
                id, in, LD32(in), LDF(in + 0x40), LDF(in + 0x44), LDF(in + 0x10), LDF(in + 0x24), LDF(in + 0x90),
                LDF(in + 0x94), LDF(in + 0x48), LD32(in + 0x54), gui_sequence(in));
    }
}
static uint32_t g_setx_detour, g_sety_detour, g_anim_detour;

void mousemenu_trace(int frames) {
    pthread_mutex_lock(&g_lock);
    LOG("[menu] trace: pointer %.1f,%.1f moves %u%s, active %d\n", g_px, g_py, g_moves, g_hidden ? " hidden" : "",
        mousemenu_active());
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_lock(&g_piece_lock);
    for (int i = 0; i < g_nshown; i++) {
        const Piece *p = &g_shown[i];
        if (p->gx != p->dx || p->gy != p->dy || p->kx != 1.0f || p->ky != 1.0f || i < 4)
            LOG("[menu] piece %d z %.1f game %.1f,%.1f drawn %.1f,%.1f k %.3f %.3f\n", i, p->gz, p->gx, p->gy, p->dx, p->dy, p->kx,
                p->ky);
    }
    LOG("[menu] %d pieces\n", g_nshown);
    pthread_mutex_unlock(&g_piece_lock);
    fnhook_set(FN_GUI_SETX, g_setx_detour), fnhook_set(FN_GUI_SETY, g_sety_detour), fnhook_set(FN_GUI_ANIM, g_anim_detour);
    g_trace_id++;
    g_trace = frames > 0 ? frames : 3;
}

// ---- the host main thread (runtime/host_input.c)
// (the game thread says whether a menu is open; only its updates stopped a long while end that)
// (MouseMenus Off too: the cursors', the quantities', the choices' and the dialogs' updates are seen all the same)
bool mousemenu_open(void) {
    uint64_t beat = g_beat_ns;
    return g_session_on && beat && plat_now_ns() - beat < STOPPED_NS;
}
bool mousemenu_active(void) { return config_live_mouse_menus() && mousemenu_open(); }
void mousemenu_pointer(float cx, float cy, bool moved) {
    pthread_mutex_lock(&g_lock);
    if (moved && (cx != g_px || cy != g_py || g_hidden)) g_moves++, g_hidden = false;
    g_px = cx, g_py = cy;
    pthread_mutex_unlock(&g_lock);
}
void mousemenu_block(bool blocked) { g_blocked = blocked; }
void mousemenu_test_press(uint32_t buttons) { __atomic_or_fetch(&g_test_press, buttons, __ATOMIC_ACQ_REL); }
void mousemenu_test_stall(int ms) { g_test_stall = ms; }
void mousemenu_hide(void) {
    pthread_mutex_lock(&g_lock);
    g_hidden = true;
    pthread_mutex_unlock(&g_lock);
}
void mousemenu_click(int button, float cx, float cy) {
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&g_lock);
    if (button == SDL_BUTTON_LEFT) g_click.x = cx, g_click.y = cy, g_click.ns = now, g_click.seen = 0, g_hidden = false;
    else if (button == SDL_BUTTON_RIGHT) g_b_ns = now;
    else if (button == SDL_BUTTON_X1 || button == SDL_BUTTON_X2) {
        int d = button == SDL_BUTTON_X1 ? -1 : 1;
        if ((g_page < 0) != (d < 0)) g_page = 0;
        if (g_page * d < MAX_WAIT) g_page += d;
        g_page_ns = now;
    }
    pthread_mutex_unlock(&g_lock);
}
void mousemenu_wheel(int notches, float cx, float cy) {
    (void)cx, (void)cy;
    if (!notches) return;
    pthread_mutex_lock(&g_lock);
    if ((g_wheel < 0) != (notches < 0)) g_wheel = 0;
    g_wheel += notches;
    if (g_wheel > MAX_WAIT) g_wheel = MAX_WAIT;
    if (g_wheel < -MAX_WAIT) g_wheel = -MAX_WAIT;
    g_wheel_ns = plat_now_ns();
    pthread_mutex_unlock(&g_lock);
}

void mousemenu_init(void) {
    fnhook_set(FN_PAD_UPDATE, hle_register("mousemenu:pad_update", pad_update));
    fnhook_set(FN_CURSOR, hle_register("mousemenu:cursor_update", cursor_update));
    fnhook_set(FN_BOX_CURSOR, hle_register("mousemenu:box_update", box_update));
    fnhook_set(FN_SE, hle_register("mousemenu:se", se));
    fnhook_set(FN_UIO_ITEMBOX, hle_register("mousemenu:uio_itembox", uio_itembox));
    fnhook_set(FN_UIO_POUCH, hle_register("mousemenu:uio_pouch", uio_pouch));
    fnhook_set(FN_LIST_CURSOR, hle_register("mousemenu:list_cursor", list_cursor));
    fnhook_set(FN_ROW_CURSOR, hle_register("mousemenu:row_cursor", row_cursor));
    fnhook_set(FN_EQUIP_LIST, hle_register("mousemenu:equip_list", equip_list));
    fnhook_set(FN_QUANTITY, hle_register("mousemenu:quantity", quantity));
    fnhook_set(FN_YES_NO, hle_register("mousemenu:yes_no", yes_no_1));
    fnhook_set(FN_YES_NO_2, hle_register("mousemenu:yes_no_2", yes_no_2));
    fnhook_set(FN_YES_NO_3, hle_register("mousemenu:yes_no_3", yes_no_3));
    fnhook_set(FN_TALK, hle_register("mousemenu:talk_update", talk_update));
    fnhook_set(FN_DIALOG, hle_register("mousemenu:dialog_update", dialog_update));
    g_setx_detour = hle_register("mousemenu:gui_setx", gui_setx);
    g_sety_detour = hle_register("mousemenu:gui_sety", gui_sety);
    g_anim_detour = hle_register("mousemenu:gui_anim", gui_anim);
}
#endif
