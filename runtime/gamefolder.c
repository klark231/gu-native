// The game folder on a desktop (runtime/gamefolder.h). Android's GameFolderActivity does this in Java before the game
// starts; here runtime/main.c asks before the window opens. The folder chosen is kept in game_folder.txt (UTF-8) beside
// the settings, as on Android.
#ifndef __ANDROID__
#include "gamefolder.h"

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "platform.h"
#include "settings_locale.h"

#ifndef GAMELIB_ID
#define GAMELIB_ID "dev"
#endif

#define SAVED "game_folder.txt"
#define MIN_ROMFS_FILES 35000  // (1.4.0 has 35531: the margin of runtime/main.c, for dump tools that leave out a few)
static const uint8_t BUILD_ID[8] = {0xfb, 0x08, 0xf1, 0xd2, 0x0f, 0xd1, 0x20, 0x4f};  // (version 1.4.0's main)

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFDIR);
}
static bool is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && !(st.st_mode & S_IFDIR);
}
static const char *T(int lang, const char *en) { return settings_text(lang, en); }

// mhgugame.dll missing: the download never brings it (it is made from the player's own copy, the guide's PC step 2)
static const char NO_LIB[] = "mhgugame.dll is missing from that folder. The download doesn't include it, on purpose: it is the "
                             "game code, made once on this PC from your own copy with the guide's tool (step 2 of its PC "
                             "setup): python build_game.py --pc <your folder>.";
static const char NO_LIB_PC[] = "mhgugame.dll, the game code for PC, is missing from that folder: the download doesn't include "
                                "it. Run the guide's tool on it again with --pc: python build_game.py --pc <your folder>. It "
                                "keeps libmhgugame.so and only adds the PC's.";

int gamefolder_language(void) {
    // (the settings' Language when they say Spanish; before the first run, the system's)
    FILE *f = fopen("mhgurecomp.ini", "r");
    char line[256];
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "Language", 8)) {
            fclose(f);
            return strstr(line, "Spanish") ? 1 : 0;
        }
    if (f) fclose(f);
    int n = 0;
    SDL_Locale **loc = SDL_GetPreferredLocales(&n);
    bool es = loc && n > 0 && loc[0]->language && !SDL_strncasecmp(loc[0]->language, "es", 2);
    SDL_free(loc);
    return es ? 1 : 0;
}

// The executable's folder (out: "" when unknown)
static void exe_dir(char *out, size_t size) {
    out[0] = 0;
    if (!plat_exe_path(out, size)) { out[0] = 0; return; }
    char *s = strrchr(out, '\\'), *s2 = strrchr(out, '/');
    if (s2 > s) s = s2;
    if (s) *s = 0; else out[0] = 0;
}

void gamefolder_problem(const char *dir, int lang, char *why, size_t size) {
    char exefs[1200], romfs[1200], p[1400];
    why[0] = 0;
    snprintf(exefs, sizeof exefs, "%s/exefs", dir);
    snprintf(romfs, sizeof romfs, "%s/romfs", dir);
    bool e = is_dir(exefs), r = is_dir(romfs);
    if (!e && !r) {
        snprintf(why, size, "%s", T(lang, "exefs and romfs are not in that folder. Choose the folder that holds them."));
        return;
    }
    if (!e) { snprintf(why, size, "%s", T(lang, "The exefs folder is missing, beside romfs.")); return; }
    if (!r) { snprintf(why, size, "%s", T(lang, "The romfs folder is missing, beside exefs.")); return; }
    snprintf(p, sizeof p, "%s/main", exefs);
    FILE *f = fopen(p, "rb");
    if (!f) { snprintf(why, size, "%s", T(lang, "The file main is missing from exefs.")); return; }
    uint8_t hdr[0x48] = {0};
    bool ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, "NSO0", 4) && !memcmp(hdr + 0x40, BUILD_ID, 8);
    fclose(f);
    if (!ok) { snprintf(why, size, "%s", T(lang, "This exefs is not version 1.4.0. Use the one from the 1.4.0 update.")); return; }
    int files = plat_count_files(romfs);
    if (files < MIN_ROMFS_FILES) {
        snprintf(why, size, T(lang, "romfs is incomplete: it has %d files where there should be 35,531. Use the base game with "
                                    "the 1.4.0 update applied."), files);
        return;
    }
    snprintf(p, sizeof p, "%s/nativeNX/system/app_shader/AppShaderPackage.mfx", romfs);
    if (!is_file(p)) {
        snprintf(why, size, "%s", T(lang, "nativeNX/system/app_shader/AppShaderPackage.mfx is missing from romfs. Use the base "
                                          "game with the 1.4.0 update applied."));
        return;
    }
    // the game code for PC, made by the same tool that makes the phone's (tools/build_game.py), beside it; where
    // runtime/gamelib.c looks: MHGU_GAME_LIB (tests), the game folder, then the executable's folder (a build folder)
    if (getenv("MHGU_GAME_LIB")) return;
    char lib_dir[1100];
    snprintf(lib_dir, sizeof lib_dir, "%s", dir);
    snprintf(p, sizeof p, "%s/mhgugame.dll", lib_dir);
    if (!is_file(p)) {
        exe_dir(lib_dir, sizeof lib_dir);
        snprintf(p, sizeof p, "%s/mhgugame.dll", lib_dir);
    }
    if (!lib_dir[0] || !is_file(p)) {
        snprintf(p, sizeof p, "%s/libmhgugame.so", dir);
        snprintf(why, size, "%s", T(lang, is_file(p) ? NO_LIB_PC : NO_LIB));
        return;
    }
    char id[64] = "";
    snprintf(p, sizeof p, "%s/mhgugame.id", lib_dir);
    if ((f = fopen(p, "r"))) {
        if (!fgets(id, sizeof id, f)) id[0] = 0;
        fclose(f);
        id[strcspn(id, "\r\n ")] = 0;
    }
    if (strcmp(id, GAMELIB_ID))
        snprintf(why, size, "%s", T(lang, "This mhgugame.dll is from another version. Make it again with this version's tool."));
}

// ---- the system's folder dialog (SDL3: on Windows it runs on a thread of its own and calls back from there)
static SDL_AtomicInt g_picked_done;
static char g_picked[1100];

static void SDLCALL on_picked(void *ud, const char *const *list, int filter) {
    (void)ud, (void)filter;
    g_picked[0] = 0;
    if (list && list[0]) snprintf(g_picked, sizeof g_picked, "%s", list[0]);
    SDL_SetAtomicInt(&g_picked_done, 1);
}

static bool pick_folder(const char *start, char *utf8, size_t size) {
    SDL_SetAtomicInt(&g_picked_done, 0);
    SDL_ShowOpenFolderDialog(on_picked, NULL, NULL, start && *start ? start : NULL, false);
    while (!SDL_GetAtomicInt(&g_picked_done)) {
        SDL_PumpEvents();
        SDL_Delay(15);
    }
    if (!g_picked[0]) return false;
    snprintf(utf8, size, "%s", g_picked);
    return true;
}

// true: choose a folder; false: leave
static bool ask(int lang, const char *text) {
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, T(lang, "Choose folder")},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, T(lang, "Exit")},
    };
    const SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, NULL, "MHGU", text, 2, buttons, NULL};
    int hit = 0;
    return SDL_ShowMessageBox(&box, &hit) && hit == 1;
}

static bool read_saved(char *utf8, size_t size) {
    FILE *f = fopen(SAVED, "rb");
    if (!f) return false;
    bool ok = fgets(utf8, (int)size, f) != NULL;
    fclose(f);
    if (ok) utf8[strcspn(utf8, "\r\n")] = 0;
    return ok && utf8[0];
}

static void write_saved(const char *utf8) {
    FILE *f = fopen(SAVED, "wb");
    if (f) fprintf(f, "%s\n", utf8), fclose(f);
}

bool gamefolder_find(char *out, size_t size) {
#ifdef _WIN32
    // (mhgugame.dll imports the runtime from MHGU.exe by that name: under another one it can't load, or it loads a second,
    // uninitialised copy of the program from beside it)
    char me[1100];
    if (plat_exe_path(me, sizeof me)) {
        const char *b = strrchr(me, '\\'), *b2 = strrchr(me, '/');
        if (b2 > b) b = b2;
        if (SDL_strcasecmp(b ? b + 1 : me, "MHGU.exe")) {
            plat_error_box(T(gamefolder_language(), "This program must be called MHGU.exe: the game code (mhgugame.dll) finds "
                                                    "it by that name. Rename it back to MHGU.exe."));
            return false;
        }
    }
#endif
    const char *env = getenv("MHGU_DATA_DIR");
    if (env && *env) return snprintf(out, size, "%s", env) < (int)size;  // (as given: runtime/main.c checks it)
    int lang = gamefolder_language();
    char why[700] = "", saved[1100] = "", native[1100];
    // the folders it can be in already: the one chosen before, data beside the executable, the executable's own
    char cand[3][1100];
    int n = 0;
    if (read_saved(saved, sizeof saved) && plat_path_from_utf8(saved, cand[n], sizeof cand[n])) n++;
    char exe[1100];
    exe_dir(exe, sizeof exe);
    if (exe[0]) {
        snprintf(cand[n++], sizeof cand[0], "%s/data", exe);
        snprintf(cand[n++], sizeof cand[0], "%s", exe);
    }
    for (int i = 0; i < n; i++) {
        char e[1200], r[1200];
        snprintf(e, sizeof e, "%s/exefs", cand[i]);
        snprintf(r, sizeof r, "%s/romfs", cand[i]);
        if (!is_dir(e) && !is_dir(r)) continue;  // (not the game's folder)
        gamefolder_problem(cand[i], lang, why, sizeof why);
        if (!why[0]) return snprintf(out, size, "%s", cand[i]) < (int)size;
        break;  // (the player's folder, with something wrong: say what, and let them choose again)
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {  // (dialogs and message boxes)
        plat_error_box(why[0] ? why : "The game files were not found.");
        return false;
    }
    // the game folder screen, in the settings menu's look (runtime/launcher.cpp); the system's boxes below only if it
    // can't make its window
    int chosen = launcher_choose_folder(why, native, sizeof native);
    if (chosen == 1) return snprintf(out, size, "%s", native) < (int)size;
    if (chosen == 0) return false;
    char text[1400];
    if (why[0])
        snprintf(text, sizeof text, "%s\n\n%s", why, T(lang, "Choose the folder with your game: exefs, romfs and the game code "
                                                             "for PC (mhgugame.dll), which the guide's tool makes."));
    else
        snprintf(text, sizeof text, "%s", T(lang, "Choose the folder with your game: exefs, romfs and the game code for PC "
                                                  "(mhgugame.dll), which the guide's tool makes."));
    if (!ask(lang, text)) return false;
    for (;;) {
        char picked[1100], utf8[1100];
        if (!pick_folder(saved, picked, sizeof picked)) return false;
        snprintf(saved, sizeof saved, "%s", picked);
        if (gamefolder_check_picked(picked, lang, utf8, sizeof utf8, native, sizeof native, why, sizeof why)) {
            gamefolder_remember(utf8);
            return snprintf(out, size, "%s", native) < (int)size;
        }
        snprintf(text, sizeof text, "%s", why);
        if (!ask(lang, text)) return false;
    }
}

bool gamefolder_check_picked(const char *picked, int lang, char *utf8, size_t utf8_size, char *native, size_t native_size,
                             char *why, size_t why_size) {
    // (or one level off, as on Android: its data folder, or the folder above)
    char ways[3][1100];
    snprintf(ways[0], sizeof ways[0], "%s", picked);
    snprintf(ways[1], sizeof ways[1], "%s/data", picked);
    snprintf(ways[2], sizeof ways[2], "%s", picked);
    char *s = strrchr(ways[2], '\\'), *s2 = strrchr(ways[2], '/');
    if (s2 > s) s = s2;
    if (s && s != ways[2]) *s = 0;
    int use = 0;
    for (int i = 0; i < 3; i++) {
        char e[1200], r[1200];
        if (!plat_path_from_utf8(ways[i], native, native_size)) continue;
        snprintf(e, sizeof e, "%s/exefs", native);
        snprintf(r, sizeof r, "%s/romfs", native);
        if (is_dir(e) && is_dir(r)) { use = i; break; }
    }
    if (!plat_path_from_utf8(ways[use], native, native_size)) {
        snprintf(why, why_size, "%s", T(lang, "That folder can't be read. Choose another one."));
        return false;
    }
    gamefolder_problem(native, lang, why, why_size);
    snprintf(utf8, utf8_size, "%s", ways[use]);
    return !why[0];
}

bool gamefolder_lib_missing(const char *why, int lang) {
    return why && (!strcmp(why, T(lang, NO_LIB)) || !strcmp(why, T(lang, NO_LIB_PC)));
}

void gamefolder_remember(const char *utf8) { write_saved(utf8); }

void gamefolder_saved(char *utf8, size_t size) {
    if (!read_saved(utf8, size)) utf8[0] = 0;
}
#endif
