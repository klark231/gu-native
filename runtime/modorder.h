// The mods folder as the Mods tab of the settings menu sees it (settings_panel.cpp): which mods are on, in what order,
// and which of them change the same game files. The game itself reads the same state in mods.c at start.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *mods_dir(void);
// The folder named `want` (any case) in dir or up to `depth` levels below it, shallowest first
bool mods_find_dir(const char *dir, const char *want, int depth, char *out, size_t n);
// The folder of mod `name` to layer over the RomFS (its romfs/, else its bare nativeNX/'s parent)
bool mods_romfs_of(const char *name, char *out, size_t n);

#define MODS_MAX 256
#define MODS_LIST_MARK "# Written by the Mods tab"  // first line of a load_order.txt the Mods tab saved
typedef struct {
    char name[128];
    bool on;
} ModEntry;
// Every mod folder, in load order (the ones a list leaves out after it, by name). A desktop leaves out those its menu
// removed (mods/_remove.txt, runtime/pc_actions.c) until their folders are gone.
int mods_installed(ModEntry *out, int max);
// Did the game load the mod `name` at its start (mods.c)? It reads that mod's files while it runs.
bool mods_loaded(const char *name);
// Writes mods/load_order.txt for the list, in this order (mods that are off as "# off: name"); false with `err`
// set when it can't
bool mods_save_order(const ModEntry *e, int n, char *err, size_t errsz);

// What a mod pair changes in common
#define MODSCAN_FIELD 160
typedef struct {
    int a, b;                 // indexes of two mods in the list given to modscan_run (a < b)
    int replaced, merged;     // files only one of the two can win; archives whose entries both can keep
    char example[MODSCAN_FIELD];
} ModConflict;
typedef struct {
    int nmods, nconflicts;
    int files[MODS_MAX];      // files each mod puts over the game's
    ModConflict *conflicts;   // (free()d by modscan_free)
} ModScan;
// Compares the mods (their order in `names` doesn't matter). Reads files: run it off the render thread.
void modscan_run(const char (*names)[128], int n, ModScan *out);
void modscan_free(ModScan *s);

#ifdef __cplusplus
}
#endif
