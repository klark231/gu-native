// Whether the local hunter has the weapon out (the touch controls: running with the thumb past the stick's rim is R,
// which with the weapon out is some weapons' guard: runtime/touch.cpp runs only sheathed).
// main v1.4.0: the game's own question is a virtual of the hunter, slot 94, FN_DRAWN (0x4a2444): bit 0 of the byte
// +0x2749. The hunter has a class per weapon (0x1949a88..0x19515ec, on 0x19491bc) and each of them has that same
// function in that slot, so the slot tells a hunter of any weapon, and nothing else needs to know which one it is.
// The local player's slot (sGameControl *0x1a8ca48 +0x30 its number; sPlayer *0x1a85b58 +0x1c the 4 slots) has held
// the hunter, or on some hunts an object of another class (vtable 0x198bf48, no place in the world) that points to the
// hunter at +0x15b0 or +0x3010: both seen on a Galaxy S25. No fixed address or layout of a session is kept here.
// Which weapon: the classes are uPlayerQuest00 .. 15, in the order of the game's weapon numbers (no 05; 15 the Prowler):
// 04 the heavy bowgun, 06 the light one, 10 the bow. Checked on a Motorola g06 with a heavy bowgun: 0x194bdc4.
#include "../hle.h"
#include "mousecam.h"

#define G_SPLAYER     0x1a85b58u
#define G_GAMECONTROL 0x1a8ca48u
#define VT_DRAWN      94u          // the slot of the vtable
#define FN_DRAWN      0x4a2444u
#define PL_FLAGS      0x2749u      // (bit 0: the weapon is out, drawing included)
#define PL_ACT_ST     0x2506u      // the current action (u16 act_st, act_no: hle/movelog.c)
#define PL_ACT_NO     0x250au
#define VT_HEAVY      0x194bdc4u   // those classes' vtables
#define VT_LIGHT      0x194c694u
#define VT_BOW        0x194e9e0u
#define VT_PROWLER    0x19515ecu   // (uPlayerQuest15: the last of the sixteen vtables with FN_DRAWN in slot 94, the base
                                   // class first; a Prowler is a hunter to the rest of this file)
#define LINK_A        0x15b0u
#define LINK_B        0x3010u
#define MAIN_LO       0x00200000u  // (main's image: a vtable is in it)
#define MAIN_HI       0x02347000u

static bool sane(uint32_t a) { return a >= 0x08000000u && a < 0xF0000000u && !(a & 3); }

static bool is_hunter(uint32_t p) {
    if (!sane(p)) return false;
    uint32_t vt = LD32(p);
    return vt >= MAIN_LO && vt < MAIN_HI && !(vt & 3) && LD32(vt + 4 * VT_DRAWN) == FN_DRAWN;
}

// The local hunter, 0 if there is none now (a menu, a town's loading, a Prowler)
uint32_t weapon_hunter(void) {
    uint32_t gc = LD32(G_GAMECONTROL), sp = LD32(G_SPLAYER);
    if (!sane(gc) || !sane(sp)) return 0;
    uint32_t slot = LD32(gc + 0x30);
    if (slot > 3) return 0;
    uint32_t pl = LD32(sp + 0x1c + 4u * slot), h = 0;
    if (!sane(pl)) return 0;
    if (is_hunter(pl)) h = pl;
    else if (is_hunter(LD32(pl + LINK_A))) h = LD32(pl + LINK_A);
    else if (is_hunter(LD32(pl + LINK_B))) h = LD32(pl + LINK_B);
    static uint32_t seen_pl = 1, seen_h = 1;
    if ((pl != seen_pl || h != seen_h) && ENV("MHGU_TOUCH_LOG"))
        LOG("[weapon] player slot %08x (vtable %08x): hunter %08x\n", pl, LD32(pl), h);
    seen_pl = pl, seen_h = h;
    return h;
}

// The game's option Bow Controls (sGameControl +0x6c, the options copied there from the save): 0 Type 1, X draws and R
// enters the aiming mode; 1 Type 2, R draws and X enters it (X then stays in it until pressed again). Measured on a
// Motorola g06 by writing it: the next press of X went from the aiming mode to a shot.
int weapon_bow_type(void) {
    uint32_t gc = LD32(G_GAMECONTROL);
    return sane(gc) ? LD8(gc + 0x6c) == 1 : 0;
}

// 1 the weapon is out, 0 sheathed, -1 not known
int weapon_drawn(void) {
    uint32_t h = weapon_hunter();
    return h ? LD8(h + PL_FLAGS) & 1 : -1;
}

// WEAPON_*: of a hunter (0: none, or another weapon)
int weapon_ranged(uint32_t h) {
    uint32_t vt = h ? LD32(h) : 0;
    return vt == VT_BOW ? WEAPON_BOW : vt == VT_HEAVY || vt == VT_LIGHT ? WEAPON_BOWGUN : 0;
}

// the hunter is in the common actions of aiming with a gun or a bow (R held: 0001:045 going in, 046 held, 048 back from
// walking; 0002:016 walking so). 046 stays the hunter's action after the aim or the scope is over, standing, until the
// next action (an RG406V with a heavy bowgun, 2026-11-10: the hunt camera's handler ran again, the view was the hunt's,
// for minutes): standing, it counts only while the gun's or the bow's own camera is up, the hunt camera's stopped. No
// action of a quick aim, the scope (046 with the weapon's command table 43), a shot (0010:005 then 124; an empty
// magazine's 0010:004), a reload (0010:001) or crouching fire (0010:026, 028, 029, 033; 053 its dodge) had bit 15 set
// there
bool weapon_aiming(uint32_t h) {
    if (!h || !weapon_ranged(h)) return false;
    uint32_t st = LD16(h + PL_ACT_ST), no = LD16(h + PL_ACT_NO);
    return (st == 1 && no >= 45 && no <= 48 && !mousecam_hunt_camera()) || (st == 2 && no == 16);
}

// the hunter in an aim's own actions, whatever camera is up: 0.1.8's weapon_aiming, which its locked shot went by (the
// one standing still keeps it: runtime/touch.cpp touch_lock_shot)
bool weapon_aim_action(uint32_t h) {
    if (!h || !weapon_ranged(h)) return false;
    uint32_t st = LD16(h + PL_ACT_ST), no = LD16(h + PL_ACT_NO);
    return (st == 1 && no >= 45 && no <= 48) || (st == 2 && no == 16);
}

// Aiming a bow or a bowgun, for the gyro (runtime/host_input.c gyro_update): a gun's quick aim and its scope (their own
// camera up: weapon_aiming), its crouching fire (0010:026, 028, 029, 033); a bow's aiming mode (0010:012 a while: a shot
// from the stance passes through it for a moment on its way back) and a shot taken from it (its draw 0010:002 and its
// shot 001, back to 012 after). Seen on a Moto g06 and an RG406V, 2026-11-10
bool weapon_gyro_aiming(uint32_t h, uint64_t now_ns) {
    static uint64_t in_012, aim_ns;  // (0010:012 since then; the aiming mode last seen)
    int ranged = weapon_ranged(h);
    if (!ranged || !(LD8(h + PL_FLAGS) & 1)) return false;
    uint32_t st = LD16(h + PL_ACT_ST), no = LD16(h + PL_ACT_NO);
    if (ranged == WEAPON_BOWGUN) return weapon_aiming(h) || (st == 0x10 && (no == 26 || no == 28 || no == 29 || no == 33));
    bool mode = st == 0x10 && no == 12;
    if (!mode) in_012 = 0;
    else if (!in_012) in_012 = now_ns;
    if (mode && now_ns - in_012 >= 150000000ull) aim_ns = now_ns;
    return (mode && now_ns - in_012 >= 150000000ull) || (st == 0x10 && (no == 1 || no == 2 || no == 12) && now_ns - aim_ns < 1500000000ull);
}

// the local player is a Prowler (a Palico played): its item bar holds Support Moves, chosen with L + Y / A
bool weapon_prowler(uint32_t h) { return h && LD32(h) == VT_PROWLER; }

// the hunter's current action, st << 16 | no (the log of a shot that found no aim it knew)
unsigned weapon_action(uint32_t h) { return h ? (unsigned)LD16(h + PL_ACT_ST) << 16 | LD16(h + PL_ACT_NO) : 0; }

// the hunter is in a move of the weapon's own (a shot, a reload), not a roll or a stagger
bool weapon_moving(uint32_t h) { return h && (LD16(h + PL_ACT_ST) & 0x7fff) == 0x10; }

// A bow or a bowgun out in a normal stance, standing or walking: in no move of the weapon's own (a shot, a reload,
// crouching fire, a style's move), not aiming, and the hunt camera's handler still running (a gun aimed, a bow drawn,
// the scope have a camera of their own: an aim of a style's that weapon_aiming doesn't know stops it too). Only a press
// from here is a locked shot's (runtime/touch.cpp touch_lock_shot, hle/mousecam.c mousecam_lock_shot)
bool weapon_stance(uint32_t h) {
    return weapon_ranged(h) && (LD8(h + PL_FLAGS) & 1) && !weapon_moving(h) && !weapon_aiming(h) && mousecam_hunt_camera();
}

// The local player's place in the world, x and z (its slot's object, as the control file's plpos: a town's walker too,
// which is no hunter of weapon_hunter's), false if there is none
bool player_place(float *x, float *z) {
    uint32_t gc = LD32(G_GAMECONTROL), sp = LD32(G_SPLAYER);
    if (!sane(gc) || !sane(sp)) return false;
    uint32_t slot = LD32(gc + 0x30), pl = slot <= 3 ? LD32(sp + 0x1c + 4u * slot) : 0;
    if (!sane(pl)) return false;
    float px = LDF(pl + 0xa0), pz = LDF(pl + 0xa8);
    if (!(px > -1e7f && px < 1e7f && pz > -1e7f && pz < 1e7f)) return false;  // (not a number either)
    *x = px, *z = pz;
    return true;
}
