// [Performance] Grass: the game's grass system, left out while it is Off (the Marshland's area 8 slowed weak phones:
// #85, #121; #159).
//
// The grass fields of the maps are one unit class, uGrass2 (a cUnit, 0x9dd0 bytes, vtable 0x198ade0), one per area
// that has any (its rGrass2, stage\<map>\<area>\etc\<area>-grass00, and the rGrass2Setting files beside it: 22 areas
// of 10 maps, the Marshland's (m07) areas 1, 2 and 8 among them). A cUnit's vtable +0x30 draws it into one of the
// scene's passes (r0 the unit, r1 the pass: its +0x16a mask against the unit's +0x10); uGrass2's (main 0xb13490)
// draws all of its grass, the shadow and depth passes included. Off, it is not run: no grass is drawn, nothing else
// of the map changes, and it applies at once. (Its +0x24, 0xb0d938, only sorts its clusters: skipped, the grass still
// drew. Its move, +0x18, hands it to sGrass. The game has an older uGrass with rGrass too: no file of the game uses
// one. The game never draws its grass with alpha to coverage, which [Performance] Grass first left out.)
// The weeds, reeds and lotus of an area's ground (textures named zassou, syokubutu, shida, hasu...) are part of the
// area's own model (uStage: terrain, rocks and trees in the same model, a few draws of it) and stay: only a list of
// texture names per map could tell them, and the gathering points' herbs are named the same way (m07d-item_kusa00).
//   MHGU_GRASS_LOG=1  (debug) each grass unit the first time it draws, with its rGrass2
#include <stdbool.h>
#include <string.h>

#include "../config.h"
#include "../hle.h"

#define FN_GRASS2_RENDER 0xb13490u  // uGrass2's draw into a pass (vtable +0x30): r0 = the unit, r1 = the pass
#define GRASS2_RES       0xd8u      // uGrass2: its rGrass2 (a cResource: the path at +0x08)
#define RES_PATH         0x08u

static bool g_log;

static void log_unit(uint32_t u) {
    static uint32_t seen[16];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == u) return;
    if (nseen == 16) return;
    seen[nseen++] = u;
    uint32_t res = LD32(u + GRASS2_RES);
    char path[64] = "";
    if (res >= 0x1000u && kernel_mapped(res + RES_PATH, sizeof path)) {
        memcpy(path, G2H(res + RES_PATH), sizeof path - 1);
        for (int i = 0; path[i]; i++)
            if ((unsigned char)path[i] < 0x20 || (unsigned char)path[i] > 0x7e) path[i] = 0;
    }
    LOG("[grass] uGrass2 0x%08x (%s): %s\n", u, path, config_live_grass() ? "drawn" : "left out");
}

static void grass2_render(Cpu *c) {
    if (g_log) log_unit(c->r[0]);
    if (config_live_grass()) fnhook_call_original(c, FN_GRASS2_RENDER);
}

void grass_init(void) {
    const char *e = getenv("MHGU_GRASS_LOG");
    g_log = e && *e == '1';
    bool hooked = fnhook_set(FN_GRASS2_RENDER, hle_register("grass:uGrass2::draw", grass2_render));
    if (g_log) LOG("[grass] uGrass2's draw (0x%08x) %s; Grass %s\n", FN_GRASS2_RENDER, hooked ? "hooked" : "NOT hooked",
                   config_live_grass() ? "On" : "Off");
}
