// The game's add-on content (DLC): each one the player has, as a folder of its own in dlc/ (beside cheats/ and mods/;
// MHGU_DLC_DIR overrides), holding what that DLC's RomFS holds (the game reads a DLC.dlcp from each: main's strings
// "aoc%d", "abs:%s:\%s" and "DLC.dlcp"; its menu Download takes them into the save). A folder's name says which one
// it is: the DLC's title id (16 hex digits: the game's base 0100770008DD8000 + 0x1000 + its index), or its index
// alone; a folder named otherwise takes the next free index. aoc:u lists the indices (services/misc.c) and
// OpenDataStorageByDataId opens one's image (services/fs.c), built from its folder like the game's (romfs.c).
// Nothing here has been seen working with a real DLC: none was at hand. Each step is logged.
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../hle.h"
#include "../romfs.h"
#include "dlc.h"

#define DLC_MAX 256

static struct {
    uint32_t index;
    char path[1024];
    RomfsImage *image;  // (made the first time the game mounts it)
} g_dlc[DLC_MAX];
static int g_ndlc = -1;

// MHGU_DLC_DIR, else the game folder's dlc/ when there is one (where a player can put them on a phone), else the app's
static bool is_dir(const char *p);
static const char *dlc_dir(void) {
    const char *e = getenv("MHGU_DLC_DIR");
    if (e && *e) return e;
    extern const char *g_game_dir;  // (main.c)
    static char in_game[1100];
    if (!in_game[0] && g_game_dir) snprintf(in_game, sizeof in_game, "%s/dlc", g_game_dir);
    return in_game[0] && is_dir(in_game) ? in_game : "dlc";
}

static bool is_dir(const char *p) {
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
}

static bool taken(uint32_t index) {
    for (int i = 0; i < g_ndlc; i++)
        if (g_dlc[i].index == index) return true;
    return false;
}

static int by_index(const void *a, const void *b) {
    uint32_t x = ((const uint32_t *)a)[0], y = ((const uint32_t *)b)[0];
    return x < y ? -1 : x > y;
}

// The index a folder's name says, or 0
static uint32_t index_of(const char *name) {
    size_t n = strlen(name);
    if (n == 16) {
        bool hex = true;
        for (size_t i = 0; i < n; i++) hex = hex && isxdigit((unsigned char)name[i]);
        unsigned long long tid = hex ? strtoull(name, NULL, 16) : 0;
        if (tid > DLC_BASE_ID && tid <= DLC_BASE_ID + 0xfff) return (uint32_t)(tid - DLC_BASE_ID);
        return 0;
    }
    bool digits = n > 0 && n <= 4;
    for (size_t i = 0; i < n; i++) digits = digits && isdigit((unsigned char)name[i]);
    unsigned long v = digits ? strtoul(name, NULL, 10) : 0;
    return v >= 1 && v <= 0xfff ? (uint32_t)v : 0;
}

static void scan(void) {
    if (g_ndlc >= 0) return;
    g_ndlc = 0;
    DIR *d = opendir(dlc_dir());
    if (!d) { LOG("[dlc] no DLC (no %s folder)\n", dlc_dir()); return; }
    char later[DLC_MAX][256];
    int nlater = 0;
    struct dirent *e;
    while ((e = readdir(d)) && g_ndlc < DLC_MAX) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dlc_dir(), e->d_name);
        if (!is_dir(path)) continue;
        uint32_t idx = index_of(e->d_name);
        if (!idx || taken(idx)) {
            if (nlater < DLC_MAX) snprintf(later[nlater++], sizeof later[0], "%s", e->d_name);
            continue;
        }
        g_dlc[g_ndlc].index = idx;
        snprintf(g_dlc[g_ndlc].path, sizeof g_dlc[0].path, "%s", path);
        g_ndlc++;
    }
    closedir(d);
    uint32_t next = 1;
    for (int i = 0; i < nlater && g_ndlc < DLC_MAX; i++) {
        while (taken(next) && next < 0xfff) next++;
        g_dlc[g_ndlc].index = next;
        snprintf(g_dlc[g_ndlc].path, sizeof g_dlc[0].path, "%s/%s", dlc_dir(), later[i]);
        g_ndlc++;
    }
    qsort(g_dlc, (size_t)g_ndlc, sizeof g_dlc[0], by_index);
    LOG("[dlc] %d DLC in %s\n", g_ndlc, dlc_dir());
    for (int i = 0; i < g_ndlc; i++) {
        char dlcp[1100];
        snprintf(dlcp, sizeof dlcp, "%s/DLC.dlcp", g_dlc[i].path);
        LOG("[dlc]   index %u: %s%s\n", g_dlc[i].index, g_dlc[i].path, is_dir(g_dlc[i].path) && access(dlcp, 0) == 0 ? "" : " (no DLC.dlcp in it)");
    }
}

int dlc_count(void) {
    scan();
    return g_ndlc;
}

int dlc_list(uint32_t offset, uint32_t *out, int max) {
    scan();
    int n = 0;
    for (int i = (int)offset; i < g_ndlc && n < max; i++) out[n++] = g_dlc[i].index;
    return n;
}

RomfsImage *dlc_image(uint64_t data_id) {
    scan();
    if (data_id <= DLC_BASE_ID || data_id > DLC_BASE_ID + 0xfff) return NULL;
    uint32_t idx = (uint32_t)(data_id - DLC_BASE_ID);
    for (int i = 0; i < g_ndlc; i++) {
        if (g_dlc[i].index != idx) continue;
        if (!g_dlc[i].image) g_dlc[i].image = romfs_image_open(g_dlc[i].path);
        LOG("[dlc] the game opens DLC %u (%s): %s\n", idx, g_dlc[i].path, g_dlc[i].image ? "ok" : "its folder can't be read");
        return g_dlc[i].image;
    }
    LOG("[dlc] the game opens DLC %u: not here\n", idx);
    return NULL;
}
