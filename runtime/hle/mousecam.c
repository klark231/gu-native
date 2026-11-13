// Mouse look on the hunt camera's own angles, not through the right stick. The stick turns the camera at a speed
// (past a round dead zone of about 39% tilt, faster with more tilt), so a mouse fed into it can't make small precise
// turns; here each count moved turns the camera by a fixed angle (MOUSECAM_DEG_PER_COUNT x [Mouse]
// MouseSensitivity), as in a PC game.
//
// The quest camera (uQuestCamera, 0xb00 bytes) keeps its angles as 16-bit units (0x10000 = a full turn): +0x9e8 /
// +0x9ec the pitch / yaw it heads for, +0x9e0 / +0x9e4 where it is (smoothed toward those each update), +0x9d8 /
// +0x9dc the angles the eye is placed by (the smoothed ones, the pitch then lifted off the ground). The stick handler
// (main+0x255CC; r0 = the camera, r1 = the game's invert option: bit 0 up / down, bit 1 left / right, from options
// byte +0x64, copied to +0x97c, or +0x980's for the second camera type) adds the stick's turn to +0x9e8 / +0x9ec,
// pitch clamped to +-0x3f00 (89 degrees). The game calls it only while the player may turn the camera (not in
// cutscenes, menus, while carving...), so it is hooked: after it, the mouse's movement since the last update goes to
// both the heading and the smoothed angles (no smoothing lag), with the game's invert option.
// Where it doesn't run (villages, whose camera doesn't turn, or other cameras) host_input.c gives the mouse to the
// right stick as before (mousecam_active).
//
// The lock-on (runtime/touch.cpp: the padlock by L or a long press on L, the right stick's click: a controller's, or its
// key): in the same hook, the camera turns toward one large monster every update, eased, as in the newer games: the
// game's Target Cam's (lock_target); not while a menu of the game's is open (touch_lock_active, whatever the player
// plays with), nor for a moment after the camera was turned by hand or by the game (L). The camera's
// yaw unit u looks along (x, z) = (-sin, -cos)(u * 2 pi / 0x10000) (measured on a Galaxy S25: the eye +0x40 toward the
// target point +0x60); its pitch is positive looking up. Only a monster in the hunter's zone of the map counts (+0x1054
// of each: the game's own "(Diff Area)"), or the camera would stare at a wall. Monsters: sEnemy (0x1a51fe0) +0x1c, ten slots; each
// one's status block (+0x1428) has its place at +0x40 and a network id at +0x1b6 (a large monster's below 10000).
//
// A shot with the lock-on on goes to the monster: arrows and shells leave along the hunter's heading (hunter +0x14a0, in
// the camera's units; (x, z) = (sin, cos); written, the hunter turns at once; +0xfec and +0x2ee4 are copies of it; its
// place is at +0xa0; measured on a Motorola g06). So for a shot from a normal stance (weapon_stance: the weapon out, no
// move of its own, no aim), a bow's draw and a moment after it is let go, and a bowgun's locked shot's own aim, the heading
// is set toward the monster while the camera is not being turned by hand; the gun's camera follows the hunter's heading
// then. The player's own aim, crouching fire, reloads, dodges and the styles' moves are never turned (#132, #177).
//
// The height: aiming, the shot leaves along the aim's pitch: hunter +0x14b8 (a float, -100 down .. 100 up; +0x14bc a copy),
// from which the game makes +0x2714 (16-bit units: 100 is 45 degrees up, -100 is 30 down). A shot that isn't aimed ignores
// them, it leaves level whatever they say (a Motorola g06: a shell in the same place at 0 and at -30 degrees), so with the
// lock-on on a gun's fire button, pressed in the stance, aims first, from any input (runtime/touch.cpp touch_lock_shot).
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../config.h"
#include "../hle.h"
#include "input.h"
#include "mousecam.h"

#define FN_CAMERA_STICK 0x2255ccu  // uQuestCamera's right stick handler: r0 = the camera, r1 = the invert bits
#define QC_PITCH_NOW    0x9e0u
#define QC_YAW_NOW      0x9e4u
#define QC_PITCH_TO     0x9e8u
#define QC_YAW_TO       0x9ecu
#define PITCH_MAX       0x3f00     // (the stick handler's clamp)
#define MOUSECAM_DEG_PER_COUNT 0.08f
#define ACTIVE_NS 200000000ull     // the handler ran this recently: the mouse is the camera's

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_dx, g_dy;           // counts moved since the handler last ran (x right, y down)
static volatile uint64_t g_seen_ns;  // when the handler last ran
static volatile uint32_t g_upd;      // the local hunter's updates (mousecam_lock_shot, from the item bar's hook)...
static volatile uint32_t g_cam_upd;  // ...and their count when the handler last ran

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }

bool mousecam_active(void) {
    uint64_t seen = g_seen_ns;
    return seen && plat_now_ns() - seen < ACTIVE_NS;
}

// The handler ran in one of the hunter's last two updates, counted, not timed: a gun aimed, the scope, a bow drawn stop it
// at once, where mousecam_active stays true for ACTIVE_NS after (a locked shot's question: the hunter in a normal stance,
// and its aim up, whatever actions a style aims with)
bool mousecam_hunt_camera(void) { return mousecam_active() && g_upd - g_cam_upd <= 2; }

void mousecam_add(float dx, float dy) {
    pthread_mutex_lock(&g_lock);
    g_dx += dx, g_dy += dy;
    pthread_mutex_unlock(&g_lock);
}

static int clamp_pitch(int p) { return p < -PITCH_MAX ? -PITCH_MAX : p > PITCH_MAX ? PITCH_MAX : p; }

bool touch_lock_active(void);  // (runtime/touch.cpp: the lock-on is on and the player is not turning the camera)
bool touch_lock_on(void);      // (the lock-on is on, in a hunt, no menu open: whatever the camera is doing)
bool touch_lock_engaged(void); // (the lock-on is on, menus and all: off, the monster it followed is forgotten)
bool touch_lock_assist(void);  // (a locked shot holds R for its aim now, from any input: touch_lock_shot)
bool touch_lock_assist_waiting(void);  // (...and waits for that aim to come up)
bool touch_lock_player_aims(void);     // (a bowgun aimed with the player's own R, from any input: no locked shot)
bool touch_lock_still(void);          // (a locked shot standing still, 0.1.8's: the hunter turned to the monster)
float touch_aim_pitch(void);           // (the touch aim's slide up or down since the last update, degrees: taken)
#define LOCK_AFTER_TURN 600000000ull  // (a turn the game made: the follow waits this long, as after one by hand)
static volatile uint64_t g_turn_ns;   // the game's last turn (camera_stick)
static uint32_t g_cam_seen;
static void mouse_turn(uint32_t cam, uint32_t invert, float dx, float dy, bool fresh);
#define ZONE       0x1054u       // (u8, of a hunter and of a monster: which zone of the map it is in)
#define PL_HEADING 0x14a0u
#define PL_AIM     0x14b8u       // (float: the aim's pitch)
#define PL_AIM2    0x14bcu
#define PL_PITCH   0x2714u       // (u16: the same in the camera's units)
#define MUZZLE_Y   110.0f        // (a shot leaves this far above the hunter's feet)
#define PL_PLACE   0xa0u
#define SHOT_NS    400000000ull  // (the fire button let go: the shot may leave a little later, on a bow always)
#define G_SENEMY 0x1a51fe0u
#define LOCK_TAU 0.12f       // seconds for the camera to turn ~63% of the way to the monster (eased)
#define LOCK_RANGE 6000.0f   // (a monster farther away from the hunter than this is not followed)
#define LOCK_HEIGHT 90.0f    // (the camera aims this far above the monster's feet)
#define LOCK_NEAR0 120.0f    // closer than this (under the monster) the camera holds its heading,
#define LOCK_NEAR1 380.0f    // farther than this the full follow (eased between)
// A model's skeleton (the hunter's and every monster's, measured on a Motorola g06 in the arena): +0x490 the number of
// joints, +0x494 their array, 0xa0 bytes each (+0x00 a vtable in main's image, +0x10 the world matrix's rows, its
// translation at +0x40 with w = 1.0, +0x60 the rotation, +0x70 the scale, +0x91 the parent: 0xff the root). The root
// is the pelvis, the body's middle (a quadruped's at 190 of the 215 its back's joints reach; its feet at 0).
#define EN_JOINTS   0x490u
#define JOINT_SIZE  0xa0u
#define JOINT_POS   0x40u

// The lock-on follows one monster, as the MH4U port's: the one the game's Target Cam has chosen (the change-target
// button of the player's Control Type moves it to the next), else the one it followed already, else the nearest. A
// turn by hand only pauses the follow (touch_lock_active), and distance never changes the monster.
// The Target Cam is the hunt camera's (its update, main 0x21fec4): +0xa6c the target chosen, 0 none, n the n-th of
// sEnemy's monsters whose +0xb5f7 is 0 (the large ones: main 0x22194c); the game turns it off when that one is down (its
// status byte, +0xa7c + n, bit 8; main 0x27ffb8). +0xa86 / +0xa88 the first two's network ids (+0x1b6 of their status
// blocks), +0xa8c / +0xa8d whether they can be targeted. With +0xa8e set (a monster of several points) the target is
// the object at +0xa70 + 4 (n - 1) instead. Measured on a Motorola g06 in the Arena (quest 630, two large monsters), in
// two sessions: ZL (Type 1) took +0xa6c 0, 1, 2, 0; the game's L then turned the camera to sEnemy's first and second
// large monster; +0xa86 / +0xa88 held their ids, 10 and 11. Until the hunter has seen them the icons read Hidden and ZL
// does nothing
#define QC_TARGET     0xa6cu
#define QC_TARGET_IDS 0xa84u
#define QC_TARGET_OK  0xa8cu
#define QC_POINTS     0xa8eu
#define QC_POINT_OBJS 0xa70u
#define EN_SMALL      0xb5f7u
static volatile uint32_t g_cam;  // (the hunt camera, its handler's last: the other threads' questions about the Target Cam)
static uint32_t g_lock_en;       // the monster followed (0: none yet), the hunt camera's thread's
static volatile uint64_t g_followed_ns;  // when the camera last turned to it (mousecam_lock_following)

// Dead or captured: never followed. The game's own test (main 0x27ffb8, by which the Target Cam leaves a monster): the
// monster's state +0x73e0 is 0xb or 0xe (dead), or 0xc with +0x73e1 0xff (captured), as the MH4U port's INFO_STATE;
// or, +0xb5f7 0 and +0xb5f4 9, bit 0 of its status block's +0x1bb. And no health left: the status block's +0x370 (+0x374
// the most; seen falling with each hit in the Arena, 2026-11-09)
static bool monster_down(uint32_t en, uint32_t info) {
    if ((int32_t)LD32(info + 0x370) <= 0) return true;
    uint32_t st = LD8(en + 0x73e0);
    if (st == 0xb || st == 0xe || (st == 0xc && LD8(en + 0x73e1) == 0xff)) return true;
    return LD8(en + 0xb5f7) == 0 && LD8(en + 0xb5f4) == 9 && (LD8(info + 0x1bb) & 1);
}

// the Target Cam's monster, of sEnemy's ten slots (0: none chosen; a point that is no monster of them: none)
static uint32_t target_cam_monster(uint32_t cam, const uint32_t slot[10]) {
    uint32_t t = sane(cam) ? LD32(cam + QC_TARGET) : 0, n = 0;
    if (t < 1 || t > 3) return 0;
    uint32_t point = LD8(cam + QC_POINTS) ? LD32(cam + QC_POINT_OBJS + 4 * (t - 1)) : 0;
    for (int k = 0; k < 10; k++) {
        if (!sane(slot[k])) continue;
        if (point ? slot[k] == point : LD8(slot[k] + EN_SMALL) == 0 && ++n == t) return slot[k];
    }
    return 0;
}

// A large monster of sEnemy's that can be followed: not dead or captured; its place, and whether it is in the hunter's
// zone of the map (+0x1054 of each: the game's own "(Diff Area)", or the camera would stare at a wall)
static bool lock_monster(uint32_t en, uint32_t hunter, float out[3], bool *here) {
    uint32_t info = sane(en) ? LD32(en + 0x1428) : 0;
    if (!sane(info)) return false;
    uint32_t id = LD16(info + 0x1b6);
    if (id == 0xffff || id >= 10000 || monster_down(en, info)) return false;
    float mx = LDF(info + 0x40), my = LDF(info + 0x44), mz = LDF(info + 0x48);
    if (!isfinite(mx) || !isfinite(my) || !isfinite(mz)) return false;
    out[0] = mx, out[1] = my, out[2] = mz;
    *here = !hunter || LD8(en + ZONE) == LD8(hunter + ZONE);
    return true;
}

// The monster to follow now (see above) and its place, or false: none, or the chosen one is in another zone (the follow
// waits for it, no other is taken meanwhile), or too far. (the game thread alone: the hunt camera, the hunter's update)
static bool lock_target(float x, float y, float z, float out[3]) {
    (void)y;
    uint32_t sen = LD32(G_SENEMY);
    if (!sane(sen)) return false;
    uint32_t hunter = weapon_hunter(), slot[10];
    for (int k = 0; k < 10; k++) slot[k] = LD32(sen + 0x1c + 4u * (uint32_t)k);
    float p[3];
    bool here;
    // the Target Cam's monster, alive: that one, here or not (down, the game turns its Target Cam off: the rest)
    uint32_t chosen = target_cam_monster(g_cam, slot);
    if (chosen && lock_monster(chosen, hunter, p, &here)) {
        g_lock_en = chosen;
        float d = (p[0] - x) * (p[0] - x) + (p[2] - z) * (p[2] - z);
        if (!here || !(d < LOCK_RANGE * LOCK_RANGE)) return false;
        memcpy(out, p, sizeof p);
        return true;
    }
    // the one followed already, while it is alive and here
    for (int k = 0; g_lock_en && k < 10; k++) {
        if (slot[k] != g_lock_en) continue;
        float d;
        if (lock_monster(slot[k], hunter, p, &here) && here &&
            (d = (p[0] - x) * (p[0] - x) + (p[2] - z) * (p[2] - z)) < LOCK_RANGE * LOCK_RANGE) {
            memcpy(out, p, sizeof p);
            return true;
        }
        break;
    }
    // else the nearest here
    float best = LOCK_RANGE * LOCK_RANGE;
    uint32_t best_en = 0;
    for (int k = 0; k < 10; k++) {
        if (!lock_monster(slot[k], hunter, p, &here) || !here) continue;
        float d = (p[0] - x) * (p[0] - x) + (p[2] - z) * (p[2] - z);
        if (d < best) best = d, best_en = slot[k], memcpy(out, p, sizeof p);
    }
    if (best_en) g_lock_en = best_en;
    return best_en != 0;
}

// (any thread) the camera turned to the monster just now: the lock-on's mark pulses (runtime/touch.cpp)
bool mousecam_lock_following(void) {
    uint64_t t = g_followed_ns;
    return t && plat_now_ns() - t < 300000000ull;
}

// (any thread) a bowgun out with the lock-on on, and a monster its locked shot can aim at, at the hunter's last update:
// none (another zone, too far, none left), and a press of the fire button is the player's own shot (runtime/touch.cpp
// touch_lock_shot: holding it back for an aim with nothing to aim at only delayed it)
static volatile bool g_aimable;
bool mousecam_lock_aimable(void) { return g_aimable; }

// The game's Control Type, and with it the button that changes the target (sUserInfo *0x1a97f78: the options, copied
// there from the save; read by the Target Cam's update, main 0x21fec4): +0x446c the type, 0 Type 1 (ZL; on L with the
// L / R and ZL / ZR buttons swapped, +0x446e), 1 Type 2 (the d-pad: down turns the Target Cam on and off, right and left
// go from one monster to the other; on the right stick with it and the d-pad swapped, +0x446f), 2 Type 3 (the right
// stick's click), 3 the right stick's click with +0x43fb set, else the left stick's
#define G_USERINFO   0x1a97f78u
#define UI_SWAP_LR   0x446eu
#define UI_SWAP_RS   0x446fu
#define UI_CONTROLS  0x446cu
#define UI_TYPE4_R3  0x43fbu
int mousecam_control_type(void) {
    uint32_t ui = LD32(G_USERINFO);
    return sane(ui) && LD8(ui + UI_CONTROLS) <= 3 ? LD8(ui + UI_CONTROLS) : 0;
}

// (any thread) the button that changes the target now, as the player's Control Type has it (BTN_* bits; 0: none a touch
// button can press, Type 2 on the right stick). Type 2: down while the Target Cam is off, else right from the first
// monster to the second, left back, down (off) with only one
uint64_t mousecam_target_buttons(void) {
    uint32_t ui = LD32(G_USERINFO), cam = g_cam;
    if (!sane(ui)) return 1ull << BTN_ZL;
    switch (mousecam_control_type()) {
    case 1: {
        if (LD8(ui + UI_SWAP_RS)) return 0;
        uint32_t t = sane(cam) && mousecam_active() ? LD32(cam + QC_TARGET) : 0;
        if (t == 1 && LD8(cam + QC_TARGET_OK + 1)) return 1ull << BTN_RIGHT;
        if (t == 2 && LD8(cam + QC_TARGET_OK)) return 1ull << BTN_LEFT;
        return 1ull << BTN_DOWN;
    }
    case 2:
        return 1ull << BTN_RSTICK;
    case 3:
        return LD8(ui + UI_TYPE4_R3) ? 1ull << BTN_RSTICK : 1ull << BTN_LSTICK;
    default:
        return LD8(ui + UI_SWAP_LR) ? 1ull << BTN_L : 1ull << BTN_ZL;
    }
}

// Where a locked shot goes: the monster's body, not its feet. The root joint (the pelvis) when it is well off the
// ground, else the middle of the skeleton's box (a monster whose root is low); false: no skeleton to read (its feet
// and a fixed height then)
static bool body_center(uint32_t en, float feet_y, float out[3]) {
    if (!sane(en)) return false;
    uint32_t n = LD32(en + EN_JOINTS), arr = LD32(en + EN_JOINTS + 4);
    if (n < 3 || n > 512 || !sane(arr)) return false;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, root[3] = {0};
    bool have_root = false;
    int good = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = arr + i * JOINT_SIZE, vt = LD32(j);
        if (vt < 0x00200000u || vt >= 0x02347000u) break;  // (not a joint: the array is not what it was measured to be)
        float p[3] = {LDF(j + JOINT_POS), LDF(j + JOINT_POS + 4), LDF(j + JOINT_POS + 8)};
        if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) || LDF(j + JOINT_POS + 12) != 1.0f) continue;
        for (int k = 0; k < 3; k++) lo[k] = fminf(lo[k], p[k]), hi[k] = fmaxf(hi[k], p[k]);
        if (!have_root && LD8(j + 0x91) == 0xff) root[0] = p[0], root[1] = p[1], root[2] = p[2], have_root = true;
        good++;
    }
    if (good < 3 || hi[1] - lo[1] < 1.0f || hi[1] - feet_y > 5000.0f) return false;
    if (have_root && root[1] - lo[1] > 0.35f * (hi[1] - lo[1])) {
        out[0] = root[0], out[1] = root[1], out[2] = root[2];
    } else {
        for (int k = 0; k < 3; k++) out[k] = 0.5f * (lo[k] + hi[k]);
    }
    return true;
}

// one update of the lock-on: the camera's heading and its smoothed angles eased toward the monster
static void lock_follow(uint32_t cam, float dt) {
    float t[3];
    float hx = LDF(cam + 0x60), hy = LDF(cam + 0x64), hz = LDF(cam + 0x68);  // (the target point: by the hunter)
    if (!isfinite(hx) || !lock_target(hx, hy, hz, t)) return;
    g_followed_ns = plat_now_ns();
    float dx = t[0] - hx, dz = t[2] - hz, dist = sqrtf(dx * dx + dz * dz);
    // under the monster (its root's bearing swings with every step): the follow fades out, the camera holds
    float f = (dist - LOCK_NEAR0) / (LOCK_NEAR1 - LOCK_NEAR0);
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    float k = (1.0f - expf(-dt / LOCK_TAU)) * f * f * (3.0f - 2.0f * f);
    if (k <= 0.0f) return;
    const float U = 65536.0f / (2.0f * 3.14159265f);
    int want = (int)lrintf(atan2f(-dx, -dz) * U) & 0xffff;
    int yaw_to = (int)(LD32(cam + QC_YAW_TO) & 0xffff), dy = (int16_t)(uint16_t)(want - yaw_to);
    int step = (int)lrintf(dy * k);
    if (!step && dy && k > .05f) step = dy > 0 ? 1 : -1;
    if (step) {
        ST32(cam + QC_YAW_TO, (uint32_t)(yaw_to + step) & 0xffffu);
        ST32(cam + QC_YAW_NOW, (LD32(cam + QC_YAW_NOW) + (uint32_t)step) & 0xffffu);
    }
    // the pitch: toward the monster's body, gently and within a comfortable range (looking a little down)
    float rise = (t[1] + LOCK_HEIGHT) - hy;
    int pwant = (int)lrintf(atan2f(rise, dist) * U);
    if (pwant > 0x0c00) pwant = 0x0c00;
    if (pwant < -0x0e00) pwant = -0x0e00;
    int pto = (int16_t)LD32(cam + QC_PITCH_TO), pstep = (int)lrintf((pwant - pto) * k * 0.6f);
    if (pstep) {
        int next = clamp_pitch(pto + pstep), now_p = clamp_pitch((int16_t)LD32(cam + QC_PITCH_NOW) + pstep);
        ST32(cam + QC_PITCH_TO, (uint32_t)next & 0xffffu);
        ST32(cam + QC_PITCH_NOW, (uint32_t)now_p & 0xffffu);
    }
}

// The hunter turned to the monster, and its aim raised to it (see the top)
static void aim_at_monster(uint32_t h) {
    float x = LDF(h + PL_PLACE), y = LDF(h + PL_PLACE + 4), z = LDF(h + PL_PLACE + 8), t[3];
    if (!isfinite(x) || !lock_target(x, y, z, t)) return;
    float body[3], aim_y = t[1] + LOCK_HEIGHT;  // (no skeleton to read: a fixed height over its feet)
    if (body_center(g_lock_en, t[1], body)) t[0] = body[0], t[2] = body[2], aim_y = body[1];
    float dx = t[0] - x, dz = t[2] - z, dist = sqrtf(dx * dx + dz * dz);
    if (dist < LOCK_NEAR0) return;  // (under it: the bearing swings)
    const float U = 65536.0f / (2.0f * 3.14159265f);
    ST32(h + PL_HEADING, (uint32_t)lrintf(atan2f(dx, dz) * U) & 0xffffu);
    float rise = atan2f(aim_y - (y + MUZZLE_Y), dist);  // (radians; up is positive)
    const float UP = 45.0f * 3.14159265f / 180.0f, DOWN = 30.0f * 3.14159265f / 180.0f;
    if (rise > UP) rise = UP;
    if (rise < -DOWN) rise = -DOWN;
    float aim = 100.0f * rise / (rise >= 0.0f ? UP : DOWN);
    STF(h + PL_AIM, aim), STF(h + PL_AIM2, aim);
    ST16(h + PL_PITCH, (uint16_t)(int16_t)lrintf(rise * U));
}

// A bow's or a bowgun's shot, with the lock-on on, from a normal stance only (weapon_stance): the hunter turned to the
// monster and its aim raised to it. What the player aims, and the weapon's other moves, are left alone: the player's own
// aim (a gun's R, the scope, a bow's aiming mode), crouching fire, a reload, a dodge, a style's moves (Valor's slide and
// Sidestep Shot) - writing the heading there turned them toward the monster (#132, #177). A turn by hand meanwhile makes
// the shot the player's too. (each update of the local hunter: the hunt camera's handler doesn't run while a gun aims)
// The touch aim's slide up and down, while a gun or a bow aims: the aim's pitch itself (hunter +0x2714 and +0x14b8,
// as a locked shot writes it), within the aim's own reach, 45 degrees up and 30 down
static void aim_pitch(uint32_t h) {
    float deg = touch_aim_pitch();
    if (deg == 0.0f || !weapon_aiming(h)) return;
    const float U = 65536.0f / (2.0f * 3.14159265f), UP = 45.0f * 3.14159265f / 180.0f, DOWN = 30.0f * 3.14159265f / 180.0f;
    float rise = (int16_t)LD16(h + PL_PITCH) / U + deg * 3.14159265f / 180.0f;
    if (rise > UP) rise = UP;
    if (rise < -DOWN) rise = -DOWN;
    float aim = 100.0f * rise / (rise >= 0.0f ? UP : DOWN);
    STF(h + PL_AIM, aim), STF(h + PL_AIM2, aim);
    ST16(h + PL_PITCH, (uint16_t)(int16_t)lrintf(rise * U));
}

void mousecam_lock_shot(void) {
    g_upd++;  // (mousecam_hunt_camera)
    static bool armed, stance_was, fire_was;
    static uint64_t fired, others_was;
    uint32_t h = weapon_hunter();
    aim_pitch(h);
    int ranged = weapon_ranged(h);
    bool stance = weapon_stance(h);  // (this update's, before the press: the next update's question)
    bool was = stance_was;
    stance_was = stance;
    if (!ranged || !touch_lock_on()) {
        armed = fire_was = g_aimable = false;
        return;
    }
    if (ranged == WEAPON_BOWGUN) {
        // (a monster to aim at, for touch_lock_shot: asked as the shot asks it, the monster followed left as it was)
        float x = LDF(h + PL_PLACE), y = LDF(h + PL_PLACE + 4), z = LDF(h + PL_PLACE + 8), t[3];
        uint32_t followed = g_lock_en;
        g_aimable = isfinite(x) && lock_target(x, y, z, t);
        g_lock_en = followed;
        // Only the locked shot's own aim (touch_lock_shot: R held for it after a press of A in the stance), in its aim, its
        // shot, or before them. Not the player's own R; not during a turn by hand (the aim is theirs); not once B, X or Y
        // ended it: a dodge, a reload, crouching fire, a sheathe go where the player sends them
        armed = fire_was = false;
        // (a shot standing still: as 0.1.8 turned it, in an aim's actions, the help's aim, or the shot itself)
        if (touch_lock_still() && touch_lock_active() && !touch_lock_player_aims()) {
            if (weapon_aim_action(h) || touch_lock_assist() || weapon_moving(h)) aim_at_monster(h);
            return;
        }
        if (touch_lock_player_aims() || !touch_lock_assist() || !touch_lock_active()) return;
        if (!touch_lock_assist_waiting() && !weapon_aiming(h) && !weapon_moving(h) && mousecam_hunt_camera()) return;
        aim_at_monster(h);
        return;
    }
    // A bow: what draws it (held; let go, the arrow leaves a little later, in the shot's own action: SHOT_NS) and what
    // enters its aiming mode, by the game's Bow Controls; A its melee attack (X + A its coating), Y the sheathe, B a dodge
    g_aimable = false;
    uint64_t b, now = plat_now_ns();
    int32_t sticks[4];
    input_get(&b, sticks);
    bool type2 = weapon_bow_type() != 0;
    uint64_t draw = type2 ? 1ull << BTN_R : 1ull << BTN_X, aim_mode = type2 ? 1ull << BTN_X : 1ull << BTN_R;
    uint64_t others = b & ((1ull << BTN_A) | (1ull << BTN_B) | (1ull << BTN_Y) | aim_mode);
    // (L, ZL or ZR held with the draw: ZL + X is a Hunter Art with Control Type 3, L + X with the L / R Button Controls
    // swapped - no locked shot)
    uint64_t shoulders = b & ((1ull << BTN_L) | (1ull << BTN_ZL) | (1ull << BTN_ZR));
    bool fire = (b & draw) != 0, active = touch_lock_active();
    // a draw from the stance, or one more that chains a locked shot (pressed again in its shot's move), is a locked shot
    if (fire && !fire_was) armed = active && !shoulders && (was || (armed && weapon_moving(h)));
    fire_was = fire;
    if (fire) fired = now;
    // (another button - the aiming mode's among them -, or a turn by hand. Not weapon_aiming: what the draw itself does
    // isn't known, and a draw from the aiming mode is no stance's anyway)
    if ((others & ~others_was) || !active) armed = false;
    others_was = others;
    if (armed && !fire && now - fired >= SHOT_NS && !weapon_moving(h)) armed = false;  // (the shot over)
    if (armed && (fire || now - fired < SHOT_NS) && weapon_moving(h)) aim_at_monster(h);
}

// (debug) MHGU_CAMLOG=1: twice a second, the hunt camera (eye, target, its angles) and the large monsters' places
static void camlog(uint32_t cam) {
    static int on = -1;
    static uint64_t next;
    if (on < 0) on = getenv("MHGU_CAMLOG") != NULL;
    uint64_t now = plat_now_ns();
    if (!on || now < next || !sane(cam)) return;
    next = now + 500000000ull;
    LOG("[camlog] cam %08x eye %.1f %.1f %.1f tgt %.1f %.1f %.1f | pitch now %d to %d yaw now %d to %d | target cam %u "
        "ids %u %u ok %u %u points %u status %02x %02x %02x | type %d buttons %llx | lock %d follow %d paused %d\n",
        cam, LDF(cam + 0x40), LDF(cam + 0x44), LDF(cam + 0x48), LDF(cam + 0x60), LDF(cam + 0x64), LDF(cam + 0x68),
        (int16_t)LD32(cam + QC_PITCH_NOW), (int16_t)LD32(cam + QC_PITCH_TO), (int)(LD32(cam + QC_YAW_NOW) & 0xffff),
        (int)(LD32(cam + QC_YAW_TO) & 0xffff), LD32(cam + QC_TARGET), LD16(cam + QC_TARGET_IDS + 2), LD16(cam + QC_TARGET_IDS + 4),
        LD8(cam + QC_TARGET_OK), LD8(cam + QC_TARGET_OK + 1), LD8(cam + QC_POINTS), LD8(cam + 0xa7d), LD8(cam + 0xa7e),
        LD8(cam + 0xa7f), mousecam_control_type(), (unsigned long long)mousecam_target_buttons(), (int)touch_lock_engaged(),
        (int)touch_lock_active(), (int)(now - g_turn_ns < LOCK_AFTER_TURN));
    uint32_t sen = LD32(0x1a51fe0u);
    if (!sen) return;
    uint32_t slots[10];
    for (int k = 0; k < 10; k++) slots[k] = LD32(sen + 0x1c + 4u * (uint32_t)k);
    LOG("[camlog]   target cam's monster %08x, followed %08x\n", target_cam_monster(cam, slots), g_lock_en);
    for (int k = 0; k < 10; k++) {
        uint32_t en = LD32(sen + 0x1c + 4u * (uint32_t)k), info = en ? LD32(en + 0x1428) : 0;
        if (!info) continue;
        uint32_t id = LD16(info + 0x1b6);
        LOG("[camlog]   enemy %d %08x id %u at %.1f %.1f %.1f | zone %u | +0x370 %d +0x374 %d state %02x %02x b5f7 %u%s%s\n", k, en, id,
            LDF(info + 0x40), LDF(info + 0x44), LDF(info + 0x48), LD8(en + 0x1054), (int32_t)LD32(info + 0x370),
            (int32_t)LD32(info + 0x374), LD8(en + 0x73e0), LD8(en + 0x73e1), LD8(en + 0xb5f7), en == g_lock_en ? " (followed)" : "",
            monster_down(en, info) ? " (down)" : "");
    }
}

static void camera_stick(Cpu *c) {
    uint32_t cam = c->r[0], invert = c->r[1];
    // A turn the game made: in its handler (the right stick), or between two of its runs (L: the camera behind the hunter
    // or on the target; a swing of its own). The lock-on waits LOCK_AFTER_TURN after it, as after a turn by hand (the
    // MH4U port's): two of them pulling the camera each its way made it shake (#128)
    static uint32_t left_yaw = ~0u;  // (the heading as this hook left it, after its own turns)
    uint32_t yaw_in = sane(cam) ? LD32(cam + QC_YAW_TO) & 0xffffu : 0;
    fnhook_call_original(c, FN_CAMERA_STICK);
    camlog(cam);
    uint64_t now = plat_now_ns(), seen = g_seen_ns;
    g_seen_ns = now;
    g_cam_upd = g_upd;
    if (sane(cam)) {
        g_cam = cam;
        uint32_t yaw_out = LD32(cam + QC_YAW_TO) & 0xffffu;
        bool fresh = !seen || now - seen >= ACTIVE_NS || cam != g_cam_seen;  // (the handler wasn't running: no turn known)
        if ((!fresh && left_yaw != ~0u && yaw_in != left_yaw) || yaw_out != yaw_in) g_turn_ns = now;
        g_cam_seen = cam;
    }
    pthread_mutex_lock(&g_lock);
    float dx = g_dx, dy = g_dy;
    g_dx = g_dy = 0;
    pthread_mutex_unlock(&g_lock);
    if (!touch_lock_engaged()) g_lock_en = 0;  // (turned off: on again, it takes the Target Cam's or the nearest anew)
    if (sane(cam) && seen && now - seen < ACTIVE_NS && touch_lock_active() && now - g_turn_ns > LOCK_AFTER_TURN) {
        float dt = (now - seen) / 1e9f;
        lock_follow(cam, dt > 0.1f ? 0.1f : dt);
    }
    mouse_turn(cam, invert, dx, dy, !seen || now - seen >= ACTIVE_NS);
    left_yaw = sane(cam) ? LD32(cam + QC_YAW_TO) & 0xffffu : ~0u;
}

// the mouse's (and a finger's) movement since the last update: the camera's heading and its smoothed angles turned by it
static void mouse_turn(uint32_t cam, uint32_t invert, float dx, float dy, bool fresh) {
    static float rest_yaw, rest_pitch;  // (the fractions of a unit left over, so slow movements add up)
    if (fresh) rest_yaw = rest_pitch = 0, dx = dy = 0;  // (moved while it didn't run)
    if ((dx == 0 && dy == 0) || !sane(cam)) return;
    float s = MOUSECAM_DEG_PER_COUNT * (65536.0f / 360.0f) * config_live_mouse_sensitivity();
    float yaw = -dx * s, pitch = -dy * s;  // (right turns the yaw down, up raises the pitch: as the stick)
    if (invert & 1) pitch = -pitch;
    if (invert & 2) yaw = -yaw;
    yaw += rest_yaw, pitch += rest_pitch;
    int iy = (int)yaw, ip = (int)pitch;
    rest_yaw = yaw - (float)iy, rest_pitch = pitch - (float)ip;
    if (iy) {
        ST32(cam + QC_YAW_TO, (LD32(cam + QC_YAW_TO) + (uint32_t)iy) & 0xffffu);
        ST32(cam + QC_YAW_NOW, (LD32(cam + QC_YAW_NOW) + (uint32_t)iy) & 0xffffu);
    }
    if (ip) {
        int to = (int16_t)LD32(cam + QC_PITCH_TO), next = clamp_pitch(to + ip);
        int now_p = clamp_pitch((int16_t)LD32(cam + QC_PITCH_NOW) + next - to);
        ST32(cam + QC_PITCH_TO, (uint32_t)next & 0xffffu);
        ST32(cam + QC_PITCH_NOW, (uint32_t)now_p & 0xffffu);
        if (next != to + ip) rest_pitch = 0;  // (at the limit)
    }
}

void mousecam_init(void) {
    fnhook_set(FN_CAMERA_STICK, hle_register("mousecam:camera_stick", camera_stick));
}
