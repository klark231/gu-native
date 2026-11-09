// The settings' actions that are screens of their own on Android (GameFolderActivity, SaveImportActivity,
// SaveExportActivity, LogShareActivity, ImportActivity), on a desktop: the system's file dialogs (SDL3), a confirmation in
// the menu where Android asks, and a notice over the game with the result (overlay_notice). runtime/ui.c calls the
// pc_* entry points on the host thread; the dialogs answer on a thread of their own. Paths here are UTF-8 (SDL's file
// functions take them so on every system); the runtime's own are in the system's spelling (plat_path_to_utf8).
#ifndef __ANDROID__
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include "app_actions.h"
#include "cheats.h"
#include "config.h"
#include "gamefolder.h"
#include "mods.h"
#include "overlay.h"
#include "platform.h"
#include "rt.h"
#include "settings_locale.h"
#include "settings_panel.h"
#include "unzip.h"

#define SAVE_SIZE 5159100  // (a save's system file, and the mark at +0x14 that every one of this game has: as Android)
static const uint8_t SAVE_MARK[4] = {0x74, 0xee, 0xb2, 0x36};
#define PENDING_DIR "save_import"  // (beside the settings: applied at the next start, before the game runs)

static int lang(void) { return settings_language(config_editable().language) == MENU_LANG_ES ? 1 : 0; }  // (Auto: the system's)
static const char *T(const char *en) { return settings_text(lang(), en); }

static void notice(const char *title, const char *text) {
    overlay_notice(T(title), text);
    LOG("[pc] %s: %s\n", title, text);
}

static bool exists(const char *utf8, SDL_PathType type) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(utf8, &info) && info.type == type;
}

static void now_stamp(char *out, size_t size) {
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    strftime(out, size, "%Y-%m-%d_%H%M%S", &tm);
}

// The game folder (UTF-8): the one chosen (game_folder.txt), else the runtime's
static void game_dir(char *out, size_t size) {
    gamefolder_saved(out, size);
    if (!out[0] && !plat_path_to_utf8(g_game_dir, out, size)) snprintf(out, size, "%s", g_game_dir);
}

// The save folder (UTF-8) as runtime/services/fs.c mounts it, and its save_0 holding system and system_backup
static void save_dir(char *out, size_t size) {
    const char *d = getenv("MHGU_SAVE_DIR");
    if (!d || !*d) d = g_save_dir;
    if (!plat_path_to_utf8(d, out, size)) snprintf(out, size, "%s", d);
}

// ---- game folder (GameFolderActivity): checked as at the start, kept for the next one
static void SDLCALL folder_picked(void *ud, const char *const *list, int filter) {
    (void)ud, (void)filter;
    if (!list || !list[0]) return;  // (cancelled; NULL: an error)
    char utf8[1100], native[1100], why[700];
    if (!gamefolder_check_picked(list[0], lang(), utf8, sizeof utf8, native, sizeof native, why, sizeof why)) {
        notice("Game folder", why);
        return;
    }
    gamefolder_remember(utf8);
    notice("Game folder", T("Done: the game uses this folder the next time it starts."));
}

void pc_choose_game_folder(void) {
    char start[1100];
    game_dir(start, sizeof start);
    SDL_ShowOpenFolderDialog(folder_picked, NULL, ui_window(), start[0] ? start : NULL, false);
}

// ---- save import (SaveImportActivity): the file is checked here, the menu asks, and it is put in place at the next
// start, before the game runs, so the game can't save over it (pc_apply_imported_save)
static char g_import[1100];
static SDL_AtomicInt g_import_ready;

static void SDLCALL save_picked(void *ud, const char *const *list, int filter) {
    (void)ud, (void)filter;
    if (!list || !list[0]) return;
    SDL_IOStream *f = SDL_IOFromFile(list[0], "rb");
    uint8_t head[0x18] = {0};
    Sint64 size = f ? SDL_GetIOSize(f) : -1;
    bool ok = f && size == SAVE_SIZE && SDL_ReadIO(f, head, sizeof head) == sizeof head && !memcmp(head + 0x14, SAVE_MARK, 4);
    if (f) SDL_CloseIO(f);
    if (!ok) {
        notice("Import save", T("That file is not a save of this game. Choose the one named system, of about 5 MB, from "
                                "your emulator's save folder."));
        return;
    }
    snprintf(g_import, sizeof g_import, "%s", list[0]);
    SDL_SetAtomicInt(&g_import_ready, 1);
    settings_panel_request_view(SETTINGS_VIEW_IMPORT_SAVE);
}

void pc_import_save(void) {
    // (it starts where the emulators keep them, when one is there: yuzu's save folder under AppData)
    char start[1100] = "";
    const char *appdata = getenv("APPDATA");
    if (appdata) {
        snprintf(start, sizeof start, "%s/yuzu/nand/user/save/0000000000000000", appdata);
        if (!exists(start, SDL_PATHTYPE_DIRECTORY)) start[0] = 0;
    }
    SDL_ShowOpenFileDialog(save_picked, NULL, ui_window(), NULL, 0, start[0] ? start : NULL, false);
}

bool pc_import_save_confirm(void) {
    if (!SDL_GetAtomicInt(&g_import_ready)) return false;
    char dst[1200];
    SDL_CreateDirectory(PENDING_DIR);
    snprintf(dst, sizeof dst, "%s/system", PENDING_DIR);
    if (!SDL_CopyFile(g_import, dst)) {
        notice("Import save", SDL_GetError());
        return false;
    }
    SDL_SetAtomicInt(&g_import_ready, 0);
    return true;
}

void pc_import_save_cancel(void) { SDL_SetAtomicInt(&g_import_ready, 0); }

void pc_apply_imported_save(void) {
    char pending[1200], dir[1100], slot[1200], to[1300];
    snprintf(pending, sizeof pending, "%s/system", PENDING_DIR);
    if (!exists(pending, SDL_PATHTYPE_FILE)) return;
    save_dir(dir, sizeof dir);
    snprintf(slot, sizeof slot, "%s/save_0", dir);
    // the current save first, in save_backups/<date> beside the save folder (as Android's MHGURecomp/save_backups)
    snprintf(to, sizeof to, "%s/system", slot);
    if (exists(to, SDL_PATHTYPE_FILE)) {
        char stamp[32], backup[1300], from[1300];
        now_stamp(stamp, sizeof stamp);
        snprintf(backup, sizeof backup, "%s/../save_backups/%s", dir, stamp);
        SDL_CreateDirectory(backup);
        for (int i = 0; i < 2; i++) {
            const char *name = i ? "system_backup" : "system";
            snprintf(from, sizeof from, "%s/%s", slot, name);
            snprintf(to, sizeof to, "%s/%s", backup, name);
            if (exists(from, SDL_PATHTYPE_FILE) && !SDL_CopyFile(from, to)) {
                LOG("[save] the current save could not be kept in %s (%s): the imported one is left for later\n", backup,
                    SDL_GetError());
                return;
            }
        }
        LOG("[save] the current save kept in %s\n", backup);
    }
    SDL_CreateDirectory(slot);
    for (int i = 0; i < 2; i++) {
        snprintf(to, sizeof to, "%s/%s", slot, i ? "system_backup" : "system");
        if (!SDL_CopyFile(pending, to)) {
            LOG("[save] the imported save could not be written to %s: %s\n", to, SDL_GetError());
            return;
        }
    }
    SDL_RemovePath(pending);
    LOG("[save] imported save put in %s\n", slot);
}

// ---- save export (SaveExportActivity): MHGU_save_<date> with system and system_backup, in the folder chosen
static void SDLCALL export_picked(void *ud, const char *const *list, int filter) {
    (void)ud, (void)filter;
    if (!list || !list[0]) return;
    char dir[1100], from[1200], out[1300], stamp[32], to[1400];
    save_dir(dir, sizeof dir);
    snprintf(from, sizeof from, "%s/save_0/system", dir);
    if (!exists(from, SDL_PATHTYPE_FILE)) {
        notice("Export save", T("There is no save yet: save in the game first."));
        return;
    }
    now_stamp(stamp, sizeof stamp);
    snprintf(out, sizeof out, "%s/MHGU_save_%s", list[0], stamp);
    if (!SDL_CreateDirectory(out)) {
        notice("Export save", SDL_GetError());
        return;
    }
    for (int i = 0; i < 2; i++) {
        const char *name = i ? "system_backup" : "system";
        snprintf(from, sizeof from, "%s/save_0/%s", dir, name);
        snprintf(to, sizeof to, "%s/%s", out, name);
        if (exists(from, SDL_PATHTYPE_FILE) && !SDL_CopyFile(from, to)) {
            notice("Export save", SDL_GetError());
            return;
        }
    }
    char text[1500];
    snprintf(text, sizeof text, T("Done: %s, with system and system_backup."), out);
    notice("Export save", text);
}

void pc_export_save(void) { SDL_ShowOpenFolderDialog(export_picked, NULL, ui_window(), NULL, false); }

// ---- the log (LogShareActivity: a PC has no sharing): its folder opened in the system's file manager
void pc_open_log_folder(void) {
    char here[1100], url[1300];
    const char *base = SDL_GetCurrentDirectory();
    snprintf(here, sizeof here, "%s", base ? base : ".");
    SDL_free((void *)base);
    for (char *c = here; *c; c++)
        if (*c == '\\') *c = '/';
    snprintf(url, sizeof url, "file:///%s", here[0] == '/' ? here + 1 : here);
    if (!SDL_OpenURL(url)) notice("Log", SDL_GetError());
    else notice("Log", T("The folder with log.txt is open. Send that file to whoever is helping you."));
}

// ---- cheats and mods (ImportActivity): copied into the game folder's cheats/ and mods/, where the game reads them at
// its start; the menu then offers to restart. Removed from the menu as Android's screen lists them, each with its Remove.
#define BUNDLED_FILE "comer_rapido.txt"  // (the port's optional cheats: pc_install_bundled_cheats)
#define BUNDLED_MARK "cheats/_bundled"   // (a '_' name: not read as cheats)
#define REMOVE_LIST "_remove.txt"        // (in the mods folder: runtime/modorder.c leaves out the mods it names)
#define STAGED ".update-"                // (mods/.update-<name>: a copy waiting for the next start, as the game uses <name>)
#define ZIP_MOST (20ull << 30)           // (a .zip that unpacks to more is not a mod: the whole game's romfs is 12.3 GB)

// A mod folder being written or deleted whole (an add; a removal's thread) holds g_folder; the small lists beside them
// (mods/_remove.txt, mods/load_order.txt, cheats/_off.txt) are changed under g_lists, taken after g_folder when both
static SDL_Mutex *g_folder, *g_lists;
static SDL_Mutex *made(SDL_Mutex **m) {
    static SDL_SpinLock spin;
    SDL_LockSpinlock(&spin);
    if (!*m) *m = SDL_CreateMutex();
    SDL_UnlockSpinlock(&spin);
    return *m;
}

static bool write_whole(const char *path, const void *data, size_t n) {  // (through path.tmp: a write cut short
    char tmp[1300];                                                     // leaves the old file)
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    SDL_IOStream *f = SDL_IOFromFile(tmp, "wb");
    bool ok = f && SDL_WriteIO(f, data, n) == n;
    if (f && !SDL_CloseIO(f)) ok = false;
    if (ok) ok = SDL_RenamePath(tmp, path);
    if (!ok) SDL_RemovePath(tmp);
    return ok;
}

// The lines of the text file `path` that drop() picks taken out, the rest as it was; with `empty_goes`, the file is
// removed once only comments and blank lines would be left. drop() gets a line without its line end.
static void drop_lines(const char *path, bool (*drop)(const char *line, size_t len, const void *ud), const void *ud,
                       bool empty_goes) {
    size_t len = 0;
    char *text = SDL_LoadFile(path, &len), *kept = text ? SDL_malloc(len + 1) : NULL;
    size_t k = 0;
    bool changed = false, content = false;
    for (size_t at = 0; kept && at < len;) {
        const char *line = text + at, *nl = memchr(line, '\n', len - at);
        size_t n = nl ? (size_t)(nl - line) + 1 : len - at, l = n, s = 0;
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) l--;
        if (drop(line, l, ud)) {
            changed = true;
        } else {
            memcpy(kept + k, line, n), k += n;
            while (s < l && (line[s] == ' ' || line[s] == '\t')) s++;
            content |= s < l && line[s] != '#';
        }
        at += n;
    }
    if (kept && changed && !(empty_goes && !content ? SDL_RemovePath(path) : write_whole(path, kept, k)))
        LOG("[pc] %s could not be written: %s\n", path, SDL_GetError());
    SDL_free(kept);
    SDL_free(text);
}

static bool same_line(const char *line, size_t l, const void *name) {  // (the line is the name, after its indent)
    while (l && (*line == ' ' || *line == '\t')) line++, l--;
    return strlen(name) == l && !memcmp(line, name, l);
}
static bool order_line(const char *line, size_t l, const void *name) {  // (load_order.txt: the name, or "# off: name")
    while (l && (*line == ' ' || *line == '\t')) line++, l--;
    if (l >= 7 && !memcmp(line, "# off: ", 7)) line += 7, l -= 7;
    return strlen(name) == l && !memcmp(line, name, l);
}
typedef struct { const char *a, *b; } Names;
static bool off_line(const char *line, size_t l, const void *ud) {  // (cheats/_off.txt: "<file>\t<cheat>" of a or b)
    const Names *f = ud;
    const char *tab = memchr(line, '\t', l);
    size_t n = tab ? (size_t)(tab - line) : 0;
    return tab && line[0] != '#' && ((f->a[0] && strlen(f->a) == n && !memcmp(line, f->a, n)) ||
                                     (f->b[0] && strlen(f->b) == n && !memcmp(line, f->b, n)));
}

static bool list_has(const char *path, const char *name) {
    size_t len = 0;
    char *text = SDL_LoadFile(path, &len);
    bool found = false;
    for (size_t at = 0; text && at < len && !found;) {
        const char *line = text + at, *nl = memchr(line, '\n', len - at);
        size_t n = nl ? (size_t)(nl - line) + 1 : len - at, l = n;
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) l--;
        found = same_line(line, l, name);
        at += n;
    }
    SDL_free(text);
    return found;
}

static bool list_add(const char *path, const char *name) {
    size_t len = 0;
    char *text = SDL_LoadFile(path, &len);
    bool fresh = !text, open_line = text && len && text[len - 1] != '\n';  // (an edited file not ending its last line)
    SDL_free(text);
    if (list_has(path, name)) return true;
    SDL_IOStream *f = SDL_IOFromFile(path, "ab");
    if (!f) return false;
    bool ok = (!fresh || SDL_IOprintf(f, "# Mods removed in the settings menu: the game leaves them out and deletes their "
                                         "folders at its next start\n")) &&
              SDL_IOprintf(f, "%s%s\n", open_line ? "\n" : "", name);
    return SDL_CloseIO(f) && ok;
}

static void safe_name(const char *in, char *out, size_t size) {
    size_t n = 0;
    for (const char *c = in; *c && n + 1 < size; c++)
        out[n++] = ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
                    strchr(" ._()[]-", *c)) ? *c : '_';
    out[n] = 0;
    if (!n || out[0] == '.' || out[0] == '_') {  // (a '_' or '.' name is skipped by the readers)
        memmove(out + 1, out, n + 1 < size ? n + 1 : size - 1);
        out[0] = 'x';
    }
}

static const char *base_name(const char *path) {
    const char *b = strrchr(path, '/'), *b2 = strrchr(path, '\\');
    if (b2 > b) b = b2;
    return b ? b + 1 : path;
}

static void SDLCALL cheats_picked(void *ud, const char *const *list, int filter) {
    (void)ud, (void)filter;
    if (!list || !list[0]) return;
    char dir[1100], to[1300], name[300], offp[1200];
    game_dir(dir, sizeof dir);
    snprintf(to, sizeof to, "%s/cheats", dir);
    SDL_CreateDirectory(to);
    // each starts off, as Android's: "<file>\t<name>" in the app's cheats/_off.txt for each [name] (masters always run)
    snprintf(offp, sizeof offp, "cheats/_off.txt");
    SDL_LockMutex(made(&g_lists));
    SDL_IOStream *off = SDL_IOFromFile(offp, "ab");
    int files = 0;
    for (int i = 0; list[i]; i++) {
        safe_name(base_name(list[i]), name, sizeof name - 4);
        if (!SDL_strcasestr(name, ".txt")) strcat(name, ".txt");
        snprintf(to, sizeof to, "%s/cheats/%s", dir, name);
        if (!SDL_CopyFile(list[i], to)) {
            notice("Add cheats", SDL_GetError());
            continue;
        }
        files++;
        size_t len = 0;
        char *text = SDL_LoadFile(list[i], &len);
        for (char *line = text; off && line && *line;) {
            char *end = strchr(line, '\n');
            if (end) *end = 0;
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            size_t l = strlen(s);
            while (l && (s[l - 1] == '\r' || s[l - 1] == ' ')) s[--l] = 0;
            if (l > 2 && s[0] == '[' && s[l - 1] == ']') {
                s[l - 1] = 0;
                SDL_IOprintf(off, "\n%s\t%s", name, s + 1);
            }
            line = end ? end + 1 : NULL;
        }
        SDL_free(text);
    }
    if (off) SDL_IOprintf(off, "\n"), SDL_CloseIO(off);
    SDL_UnlockMutex(g_lists);
    if (files) settings_panel_request_view(SETTINGS_VIEW_RESTART);
}

void pc_import_cheats(void) {
    static SDL_DialogFileFilter filters[1];  // (kept until the dialog answers)
    filters[0] = (SDL_DialogFileFilter){T("Cheats (.txt)"), "txt"};
    SDL_ShowOpenFileDialog(cheats_picked, NULL, ui_window(), filters, 1, NULL, true);
}

// The cheats folders the game reads (UTF-8): the app's, and the game folder's - the one it started with, and the one
// chosen since, which Add cheats copies into
static int cheat_dirs(char (*dirs)[1100]) {
    int n = 1;
    if (!plat_path_to_utf8(cheats_dir(), dirs[0], sizeof dirs[0])) snprintf(dirs[0], sizeof dirs[0], "%s", cheats_dir());
    if (getenv("MHGU_CHEATS_DIR")) return n;  // (as runtime/cheats.c: that folder alone)
    for (int k = 0; k < 2; k++) {
        char d[1100], path[1200];
        if (k) game_dir(d, sizeof d);
        else if (!plat_path_to_utf8(g_game_dir, d, sizeof d)) snprintf(d, sizeof d, "%s", g_game_dir);
        snprintf(path, sizeof path, "%s/cheats", d);
        bool seen = false;
        for (int i = 0; i < n; i++) seen |= !strcmp(dirs[i], path);
        if (!seen) snprintf(dirs[n++], sizeof dirs[0], "%s", path);
    }
    return n;
}

typedef struct { char (*names)[256]; int n, max; } CheatFiles;
static SDL_EnumerationResult SDLCALL cheat_file(void *ud, const char *dir, const char *name) {
    CheatFiles *c = ud;
    char path[1400];
    size_t l = strlen(name);
    if (c->n >= c->max) return SDL_ENUM_SUCCESS;
    snprintf(path, sizeof path, "%s%s", dir, name);
    if (name[0] == '_' || name[0] == '.' || l <= 4 || l >= 256 || SDL_strcasecmp(name + l - 4, ".txt") ||
        !exists(path, SDL_PATHTYPE_FILE))
        return SDL_ENUM_CONTINUE;
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->names[i], name)) return SDL_ENUM_CONTINUE;
    snprintf(c->names[c->n++], sizeof c->names[0], "%s", name);
    return c->n < c->max ? SDL_ENUM_CONTINUE : SDL_ENUM_SUCCESS;
}
static int by_name(const void *a, const void *b) { return SDL_strcasecmp(a, b); }

int pc_cheat_files(char (*names)[256], int max) {
    char dirs[3][1100];
    int nd = cheat_dirs(dirs);
    CheatFiles c = {names, 0, max};
    for (int i = 0; i < nd && c.n < max; i++)
        if (exists(dirs[i], SDL_PATHTYPE_DIRECTORY)) SDL_EnumerateDirectory(dirs[i], cheat_file, &c);
    SDL_qsort(names, (size_t)c.n, sizeof names[0], by_name);
    return c.n;
}

bool pc_remove_cheat_file(const char *file, const char *loaded, char *err, size_t errsz) {
    char dirs[3][1100], path[1400], mine[1100], shown[300];
    int nd = cheat_dirs(dirs);
    bool named = file[0] && file[0] != '.' && file[0] != '_' && !strpbrk(file, "/\\");  // (a file name alone)
    if (!plat_path_to_utf8(loaded, shown, sizeof shown)) snprintf(shown, sizeof shown, "%s", loaded);
    if (named) snprintf(shown, sizeof shown, "%s", file);
    for (int i = 0; named && i < nd; i++) {  // (any copy of it: one left in another folder would load next time)
        snprintf(path, sizeof path, "%s/%s", dirs[i], file);
        if (exists(path, SDL_PATHTYPE_FILE) && !SDL_RemovePath(path)) {
            snprintf(err, errsz, T("%s could not be deleted: %s"), shown, SDL_GetError());
            return false;
        }
    }
    // its lines out of the list of those off: by its name, and by the game's (which keeps 63 bytes of it)
    Names both = {named ? file : "", loaded};
    if (!plat_path_to_utf8(cheats_dir(), mine, sizeof mine)) snprintf(mine, sizeof mine, "%s", cheats_dir());
    snprintf(path, sizeof path, "%s/_off.txt", mine);
    SDL_LockMutex(made(&g_lists));
    drop_lines(path, off_line, &both, false);
    SDL_UnlockMutex(g_lists);
    // the port's own: its mark stays, so the next start doesn't copy it back (pc_install_bundled_cheats)
    if (named && !strcmp(file, BUNDLED_FILE) && !exists(BUNDLED_MARK, SDL_PATHTYPE_FILE)) {
        SDL_CreateDirectory("cheats");
        SDL_IOStream *m = SDL_IOFromFile(BUNDLED_MARK, "wb");
        if (m) SDL_CloseIO(m);
    }
    if (loaded[0]) cheats_drop_file(loaded);
    char text[700];
    snprintf(text, sizeof text, T("Done: %s was deleted."), shown);
    notice("Remove cheats", text);
    return true;
}

// a mod folder: romfs, exefs or nativeNX somewhere in its first levels (as Android's hasModParts)
typedef struct { int depth; bool found; } PartScan;
static SDL_EnumerationResult SDLCALL mod_part(void *ud, const char *dir, const char *name) {
    PartScan *m = ud;
    char path[1400];
    snprintf(path, sizeof path, "%s%s", dir, name);
    if (!exists(path, SDL_PATHTYPE_DIRECTORY)) return SDL_ENUM_CONTINUE;
    if (!SDL_strcasecmp(name, "romfs") || !SDL_strcasecmp(name, "exefs") || !SDL_strcasecmp(name, "nativeNX")) {
        m->found = true;
        return SDL_ENUM_SUCCESS;
    }
    if (m->depth < 3) {
        m->depth++;
        SDL_EnumerateDirectory(path, mod_part, m);
        m->depth--;
    }
    return m->found ? SDL_ENUM_SUCCESS : SDL_ENUM_CONTINUE;
}

typedef struct { const char *to; bool ok; } TreeCopy;
static SDL_EnumerationResult SDLCALL copy_entry(void *ud, const char *dir, const char *name) {
    TreeCopy *t = ud;
    char from[1400], to[1400];
    snprintf(from, sizeof from, "%s%s", dir, name);
    snprintf(to, sizeof to, "%s/%s", t->to, name);
    if (exists(from, SDL_PATHTYPE_DIRECTORY)) {
        if (!SDL_CreateDirectory(to)) return t->ok = false, SDL_ENUM_FAILURE;
        TreeCopy sub = {to, true};
        SDL_EnumerateDirectory(from, copy_entry, &sub);
        if (!sub.ok) return t->ok = false, SDL_ENUM_FAILURE;
    } else if (!SDL_CopyFile(from, to)) {
        return t->ok = false, SDL_ENUM_FAILURE;
    }
    return SDL_ENUM_CONTINUE;
}

// A link (a junction, a symbolic link) stands for a folder or file elsewhere: SDL's listing goes through it, and a mod
// linked in from an emulator's mods folder would take that folder's files along. Windows: a reparse point naming another
// path (a name surrogate); the other kinds - the cloud's placeholders, deduplicated files - are the files themselves.
// (Unsure: a link, so nothing is deleted through it.)
static bool is_link(const char *utf8) {
#ifdef _WIN32
    WCHAR w[1400];
    WIN32_FIND_DATAW f;
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, (int)SDL_arraysize(w))) return true;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    HANDLE h = FindFirstFileW(w, &f);  // (the entry itself, not followed: its reparse tag)
    if (h == INVALID_HANDLE_VALUE) return true;
    FindClose(h);
    return IsReparseTagNameSurrogate(f.dwReserved0) != 0;
#else
    struct stat st;
    return !lstat(utf8, &st) && S_ISLNK(st.st_mode);
#endif
}

// path and everything in it deleted; a link, only itself (SDL_RemovePath doesn't follow one: RemoveDirectoryW, remove())
static void remove_tree(const char *path);
static SDL_EnumerationResult SDLCALL remove_entry(void *ud, const char *dir, const char *name) {
    (void)ud;
    char path[1400];
    snprintf(path, sizeof path, "%s%s", dir, name);
    remove_tree(path);
    return SDL_ENUM_CONTINUE;
}
static void remove_tree(const char *path) {
    if (!is_link(path) && exists(path, SDL_PATHTYPE_DIRECTORY)) SDL_EnumerateDirectory(path, remove_entry, NULL);
    SDL_RemovePath(path);
}

// Where Add a mod puts it (UTF-8): the game folder's mods/, created
static void mods_target(char *out, size_t size) {
    char dir[1100];
    game_dir(dir, sizeof dir);
    snprintf(out, size, "%s/mods", dir);
    SDL_CreateDirectory(out);
}

// mods/.new, complete, becomes the mod `name`: in place of an older one of that name, or, when the game loaded that one
// at its start (it reads its files while it runs), mods/.update-<name>, which the next start puts in its place
// (pc_apply_mod_changes). Then the menu offers to restart. (g_folder held)
static void mod_install(const char *mods, const char *tmp, const char *name) {
    char to[1400], list[1300], text[700];
    snprintf(list, sizeof list, "%s/" REMOVE_LIST, mods);
    SDL_LockMutex(made(&g_lists));
    drop_lines(list, same_line, name, true);  // (one being removed, added again: it stays)
    SDL_UnlockMutex(g_lists);
    bool later = mods_loaded(name);
    snprintf(to, sizeof to, later ? "%s/" STAGED "%s" : "%s/%s", mods, name);
    remove_tree(to);
    if (!SDL_RenamePath(tmp, to)) {
        notice("Add a mod", SDL_GetError());
        remove_tree(tmp);
        return;
    }
    snprintf(text, sizeof text,
             T(later ? "Done: %s replaces the one in use when the game restarts." : "Done: %s is in the mods folder."), name);
    notice("Add a mod", text);
    settings_panel_request_view(SETTINGS_VIEW_RESTART);
}

// ---- a mod's .zip (Android's ImportActivity takes one too): every entry checked before anything is written - a path
// that would leave the mod's folder (absolute, a drive, ".."), a name Windows can't use, a password, a compression other
// than Deflate, no romfs or exefs - then unpacked into mods/.new and put in place as a copied folder is
typedef enum { ZIP_NAME_OK, ZIP_NAME_ESCAPES, ZIP_NAME_INVALID } ZipName;

static bool device_name(const char *p, size_t l) {  // (CON, NUL, COM1...: Windows opens the device, with any extension)
    static const char *const NAMES[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    size_t n = 0;
    while (n < l && p[n] != '.') n++;
    while (n && p[n - 1] == ' ') n--;
    for (size_t i = 0; i < SDL_arraysize(NAMES); i++)
        if (strlen(NAMES[i]) == n && !SDL_strncasecmp(p, NAMES[i], n)) return true;
    return n == 4 && (!SDL_strncasecmp(p, "COM", 3) || !SDL_strncasecmp(p, "LPT", 3)) && p[3] >= '0' && p[3] <= '9';
}

// An entry's path under the mod's folder, into out ('/' between its parts; "" for nothing to make)
static ZipName zip_path(const char *name, char *out, size_t size) {
    size_t n = 0;
    out[0] = 0;
    if (name[0] == '/' || (name[0] && name[1] == ':')) return ZIP_NAME_ESCAPES;  // ("/x", "//server/x", "C:x")
    for (const char *p = name; *p;) {
        const char *e = strchr(p, '/');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == 2 && p[0] == '.' && p[1] == '.') return ZIP_NAME_ESCAPES;
        if (l && !(l == 1 && p[0] == '.')) {  // ("a//b" and "./a": nothing)
            for (size_t i = 0; i < l; i++)  // (Windows: no control characters or <>:"|?*, nor a dot or space at the end)
                if ((unsigned char)p[i] < 32 || strchr("<>:\"|?*", p[i])) return ZIP_NAME_INVALID;
            if (p[l - 1] == '.' || p[l - 1] == ' ' || device_name(p, l) || n + l + 2 > size) return ZIP_NAME_INVALID;
            if (n) out[n++] = '/';
            memcpy(out + n, p, l);
            out[n += l] = 0;
        }
        p += l + (e != NULL);
    }
    return ZIP_NAME_OK;
}

// A romfs, exefs or nativeNX folder among the path's first four (runtime/modorder.c mods_find_dir: three levels down)
static bool zip_mod_part(const char *path, bool dir) {
    for (int level = 0; level <= 3 && *path; level++) {
        const char *e = strchr(path, '/');
        if (!e && !dir) return false;  // (a file's last part is the file)
        size_t l = e ? (size_t)(e - path) : strlen(path);
        if ((l == 5 && (!SDL_strncasecmp(path, "romfs", 5) || !SDL_strncasecmp(path, "exefs", 5))) ||
            (l == 8 && !SDL_strncasecmp(path, "nativeNX", 8)))
            return true;
        if (!e) return false;
        path = e + 1;
    }
    return false;
}

typedef struct { const char *name; uint64_t done, total, shown; bool first; } Unpacking;
static void unpacking(void *ud, uint64_t n) {
    Unpacking *u = ud;
    u->done += n;
    uint64_t now = SDL_GetTicksNS();
    if (!u->first && now - u->shown < 2000000000ull) return;  // (every 2 s: each notice fades in again)
    u->shown = now, u->first = false;
    char text[500];
    snprintf(text, sizeof text, T("Unpacking %s: %d %%"), u->name, (int)(u->total ? u->done * 100 / u->total : 0));
    overlay_notice(T("Add a mod"), text);
}

static void mod_zip(const char *path) {
    char rel[1100];
    const char *why = NULL;  // (English: T() says it)
    Unzip z;
    UnzipResult r = unzip_open(&z, path);
    if (r == UNZIP_READ) {
        notice("Add a mod", SDL_GetError());
        unzip_close(&z);
        return;
    }
    if (r != UNZIP_OK) why = "That file is not a .zip, or it is damaged.";
    uint64_t total = 0;
    bool part = false;
    for (int i = 0; !why && i < z.count; i++) {
        const UnzipEntry *e = &z.entries[i];
        ZipName c = zip_path(e->name, rel, sizeof rel);
        if (c == ZIP_NAME_ESCAPES) why = "That .zip has files that would go outside its folder: it was not added.";
        else if (c == ZIP_NAME_INVALID) why = "That .zip has file names Windows can't use: it was not added.";
        else if (!e->dir && (e->flags & 1)) why = "That .zip has a password: unpack it yourself and choose its folder.";
        else if (!e->dir && e->method != 0 && e->method != 8)
            why = "That .zip is compressed in a way the game can't unpack (only Deflate): unpack it yourself and choose its folder.";
        part = part || (rel[0] && zip_mod_part(rel, e->dir));
        total += e->dir ? 0 : e->size;
    }
    if (!why && !part) why = "That .zip has no romfs or exefs folder in it. Choose the mod's own .zip.";
    if (!why && total > ZIP_MOST) why = "That .zip would unpack to more than 20 GB, more than the whole game: it was not added.";
    if (why) {
        notice("Add a mod", T(why));
        unzip_close(&z);
        return;
    }
    char mods[1200], base[300], name[300], tmp[1400], out[2600], text[1400];
    mods_target(mods, sizeof mods);
    snprintf(base, sizeof base, "%s", base_name(path));  // (the mod's name: the .zip's)
    size_t bl = strlen(base);
    if (bl > 4 && !SDL_strcasecmp(base + bl - 4, ".zip")) base[bl - 4] = 0;
    safe_name(base, name, sizeof name);
    snprintf(tmp, sizeof tmp, "%s/.new", mods);
    Unpacking u = {name, 0, total, 0, true};
    unpacking(&u, 0);
    SDL_LockMutex(made(&g_folder));
    remove_tree(tmp);
    bool ok = SDL_CreateDirectory(tmp);
    if (!ok) r = UNZIP_WRITE;
    for (int i = 0; ok && i < z.count; i++) {
        const UnzipEntry *e = &z.entries[i];
        zip_path(e->name, rel, sizeof rel);
        if (!rel[0]) continue;
        snprintf(out, sizeof out, "%s/%s", tmp, rel);
        char *slash = strrchr(out, '/');
        *slash = 0;
        ok = SDL_CreateDirectory(out);  // (its folder: a .zip need not list the folders)
        *slash = '/';
        if (ok && e->dir) ok = SDL_CreateDirectory(out);
        if (!ok) r = UNZIP_WRITE;
        else if (!e->dir) ok = (r = unzip_extract(&z, i, out, unpacking, &u)) == UNZIP_OK;
    }
    if (ok) {
        mod_install(mods, tmp, name);
    } else {
        if (r == UNZIP_WRITE || r == UNZIP_READ) snprintf(text, sizeof text, T("The mod could not be unpacked: %s"), SDL_GetError());
        else snprintf(text, sizeof text, "%s", T("That file is not a .zip, or it is damaged."));
        remove_tree(tmp);
        notice("Add a mod", text);
    }
    SDL_UnlockMutex(g_folder);
    unzip_close(&z);
}

static void SDLCALL mod_picked(void *ud, const char *const *list, int filter) {
    (void)ud, (void)filter;
    if (!list || !list[0]) return;
    if (exists(list[0], SDL_PATHTYPE_FILE)) {  // (its .zip)
        mod_zip(list[0]);
        return;
    }
    PartScan scan = {0, false};
    SDL_EnumerateDirectory(list[0], mod_part, &scan);
    if (!scan.found) {
        notice("Add a mod", T("That folder has no romfs or exefs in it. Choose the mod's own folder."));
        return;
    }
    char mods[1200], name[300], tmp[1400], text[700];
    mods_target(mods, sizeof mods);
    safe_name(base_name(list[0]), name, sizeof name);
    snprintf(tmp, sizeof tmp, "%s/.new", mods);
    snprintf(text, sizeof text, T("Copying %s..."), name);
    overlay_notice(T("Add a mod"), text);
    SDL_LockMutex(made(&g_folder));
    remove_tree(tmp);
    TreeCopy t = {tmp, SDL_CreateDirectory(tmp)};
    if (t.ok) SDL_EnumerateDirectory(list[0], copy_entry, &t);
    if (t.ok) {
        mod_install(mods, tmp, name);
    } else {
        notice("Add a mod", SDL_GetError());
        remove_tree(tmp);
    }
    SDL_UnlockMutex(g_folder);
}

static SDL_AtomicInt g_mod_zip;  // (the menu's choice for the next Add a mod: the mod's .zip, else its folder)
void pc_mod_dialog(bool zip) { SDL_SetAtomicInt(&g_mod_zip, zip ? 1 : 0); }

void pc_import_mod(void) {
    if (!SDL_GetAtomicInt(&g_mod_zip)) {
        SDL_ShowOpenFolderDialog(mod_picked, NULL, ui_window(), NULL, false);
        return;
    }
    static SDL_DialogFileFilter filters[1];  // (kept until the dialog answers)
    filters[0] = (SDL_DialogFileFilter){T("Mods (.zip)"), "zip"};
    SDL_ShowOpenFileDialog(mod_picked, NULL, ui_window(), filters, 1, NULL, false);
}

// ---- removing a mod (the Mods tab): mods/_remove.txt hides it at once (mods_installed leaves out what it names), its
// lines leave load_order.txt, and its folder goes: on a thread of its own now, or at the next start when the game loaded
// it at this one (it reads the mod's files while it runs: deleted under it, a later read would get nothing)
typedef struct { char list[1300], path[1400], name[260], shown[300]; } Removal;

static int SDLCALL removal(void *ud) {
    Removal *j = ud;
    char text[1700];
    SDL_LockMutex(made(&g_folder));
    SDL_LockMutex(made(&g_lists));
    bool asked = list_has(j->list, j->name);  // (still: not added again since)
    SDL_UnlockMutex(g_lists);
    if (asked) remove_tree(j->path);
    bool gone = asked && !exists(j->path, SDL_PATHTYPE_DIRECTORY);
    if (gone) {
        SDL_LockMutex(g_lists);
        drop_lines(j->list, same_line, j->name, true);
        SDL_UnlockMutex(g_lists);
        snprintf(text, sizeof text, T("Done: %s was deleted."), j->shown);
    } else {
        snprintf(text, sizeof text, T("Part of %s could not be deleted (%s): it is tried again at the next start."),
                 j->shown, SDL_GetError());
    }
    SDL_UnlockMutex(g_folder);
    if (asked) notice("Remove a mod", text);
    free(j);
    return 0;
}

bool pc_mod_is_link(const char *name) {
    char mods[1100], name8[300], path[1400];
    if (!name[0] || strpbrk(name, "/\\") || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    if (!plat_path_to_utf8(mods_dir(), mods, sizeof mods)) snprintf(mods, sizeof mods, "%s", mods_dir());
    if (!plat_path_to_utf8(name, name8, sizeof name8)) snprintf(name8, sizeof name8, "%s", name);
    snprintf(path, sizeof path, "%s/%s", mods, name8);
    return is_link(path);
}

int pc_remove_mod(const char *name, char *err, size_t errsz) {
    char mods[1100], order[1300], text[700];
    err[0] = 0;
    if (!name[0] || strpbrk(name, "/\\") || !strcmp(name, ".") || !strcmp(name, "..")) return 0;
    Removal *j = calloc(1, sizeof *j);
    if (!j) return 0;
    if (!plat_path_to_utf8(mods_dir(), mods, sizeof mods)) snprintf(mods, sizeof mods, "%s", mods_dir());
    if (!plat_path_to_utf8(name, j->shown, sizeof j->shown)) snprintf(j->shown, sizeof j->shown, "%s", name);
    snprintf(j->list, sizeof j->list, "%s/" REMOVE_LIST, mods);
    snprintf(j->path, sizeof j->path, "%s/%s", mods, j->shown);
    snprintf(j->name, sizeof j->name, "%s", name);  // (the lists hold the runtime's spelling: mods_installed's)
    snprintf(order, sizeof order, "%s/load_order.txt", mods);
    SDL_LockMutex(made(&g_lists));
    bool ok = list_add(j->list, name);
    if (ok) drop_lines(order, order_line, name, false);
    SDL_UnlockMutex(g_lists);
    if (!ok) {
        snprintf(err, errsz, T("%s could not be deleted: %s"), j->shown, SDL_GetError());
        free(j);
        return 0;
    }
    SDL_Thread *t = mods_loaded(name) ? NULL : SDL_CreateThread(removal, "mod removal", j);
    if (t) {
        SDL_DetachThread(t);
        return 1;
    }
    snprintf(text, sizeof text, T("Done: %s is deleted when the game restarts."), j->shown);
    notice("Remove a mod", text);
    bool loaded = mods_loaded(name);
    free(j);
    return loaded ? 2 : 1;  // (not loaded, and no thread for it: hidden now, deleted at the next start all the same)
}

// ---- at the start, before the game reads its mods (runtime/main.c): the removals and replacements asked for while the
// game used those mods, and what an add cut short left
typedef struct { char names[64][300]; int n; } Staged;
static SDL_EnumerationResult SDLCALL staged_entry(void *ud, const char *dir, const char *name) {
    Staged *s = ud;
    (void)dir;
    if (!strncmp(name, STAGED, strlen(STAGED)) && name[strlen(STAGED)] && s->n < 64)
        snprintf(s->names[s->n++], sizeof s->names[0], "%s", name);
    return SDL_ENUM_CONTINUE;
}

static void apply_mod_changes(const char *mods) {
    char path[1400], stage[1400], list[1300];
    snprintf(path, sizeof path, "%s/.new", mods);
    remove_tree(path);
    snprintf(list, sizeof list, "%s/" REMOVE_LIST, mods);
    size_t len = 0;
    char *text = SDL_LoadFile(list, &len);
    for (char *line = text; line && *line;) {
        char *nl = strchr(line, '\n'), *s = line, name8[300];
        if (nl) *nl = 0;
        size_t l = strlen(s);
        while (l && (s[l - 1] == '\r' || s[l - 1] == ' ')) s[--l] = 0;
        while (*s == ' ' || *s == '\t') s++;
        line = nl ? nl + 1 : NULL;
        if (!*s || *s == '#' || strpbrk(s, "/\\") || !strcmp(s, ".") || !strcmp(s, "..")) continue;
        if (!plat_path_to_utf8(s, name8, sizeof name8)) snprintf(name8, sizeof name8, "%s", s);
        snprintf(path, sizeof path, "%s/%s", mods, name8);
        snprintf(stage, sizeof stage, "%s/" STAGED "%s", mods, name8);
        remove_tree(stage);
        remove_tree(path);
        if (exists(path, SDL_PATHTYPE_DIRECTORY)) {
            LOG("[mods] %s could not be deleted (%s): left out, and tried again at the next start\n", name8, SDL_GetError());
            continue;
        }
        drop_lines(list, same_line, s, true);
        LOG("[mods] %s: deleted, as the settings menu asked\n", name8);
    }
    SDL_free(text);
    Staged *st = calloc(1, sizeof *st);
    if (st) SDL_EnumerateDirectory(mods, staged_entry, st);
    for (int i = 0; st && i < st->n; i++) {
        const char *name = st->names[i] + strlen(STAGED);
        snprintf(stage, sizeof stage, "%s/%s", mods, st->names[i]);
        snprintf(path, sizeof path, "%s/%s", mods, name);
        remove_tree(path);
        if (SDL_RenamePath(stage, path)) LOG("[mods] %s: replaced by the copy added while it was in use\n", name);
        else LOG("[mods] %s: the copy added while it was in use could not take its place (%s)\n", name, SDL_GetError());
    }
    free(st);
}

void pc_apply_mod_changes(void) {
    // (the folders mods_dir can be: MHGU_MODS_DIR alone, else the game folder's mods/ and the app's own)
    const char *env = getenv("MHGU_MODS_DIR");
    char done[1100] = "";
    for (int k = 0; k < (env && *env ? 1 : 2); k++) {
        char native[1100], dir[1100];
        if (env && *env) snprintf(native, sizeof native, "%s", env);
        else if (!k) snprintf(native, sizeof native, "%s/mods", g_game_dir);
        else snprintf(native, sizeof native, "mods");
        if (!plat_path_to_utf8(native, dir, sizeof dir)) snprintf(dir, sizeof dir, "%s", native);
        if (!exists(dir, SDL_PATHTYPE_DIRECTORY) || !strcmp(dir, done)) continue;
        apply_mod_changes(dir);
        snprintf(done, sizeof done, "%s", dir);
    }
}

// (tests: the control file's "pick <what> <path>", as if the player had chosen path in that dialog: host_input.c)
void pc_test_pick(const char *what, const char *path) {
    const char *const list[2] = {path, NULL};
    if (!strcmp(what, "folder")) folder_picked(NULL, list, 0);
    else if (!strcmp(what, "save")) save_picked(NULL, list, 0);
    else if (!strcmp(what, "export")) export_picked(NULL, list, 0);
    else if (!strcmp(what, "cheats")) cheats_picked(NULL, list, 0);
    else if (!strcmp(what, "mod")) mod_picked(NULL, list, 0);
    else LOG("[pc] pick: no dialog called %s\n", what);
}

// ---- the port's optional cheats (comer_rapido.txt, in extras beside the executable): the first time, copied into
// cheats/ and off, as Android's GameActivity does once (deleted, renamed or removed in the menu by the player, they
// don't come back: the mark stays)
void pc_install_bundled_cheats(void) {
    if (exists(BUNDLED_MARK, SDL_PATHTYPE_FILE)) return;
    char exe[1100], src[1300];
    if (!plat_exe_path(exe, sizeof exe)) return;
    char *b = strrchr(exe, '\\'), *b2 = strrchr(exe, '/');
    if (b2 > b) b = b2;
    if (b) *b = 0;
    char dir[1200];
    if (!plat_path_to_utf8(exe, dir, sizeof dir)) return;
    snprintf(src, sizeof src, "%s/extras/" BUNDLED_FILE, dir);
    if (!exists(src, SDL_PATHTYPE_FILE)) return;  // (a build folder: none)
    SDL_CreateDirectory("cheats");
    if (!SDL_CopyFile(src, "cheats/" BUNDLED_FILE)) return;
    SDL_IOStream *off = SDL_IOFromFile("cheats/_off.txt", "ab");
    if (off) {
        SDL_IOprintf(off, "\n" BUNDLED_FILE "\tComer rapido siempre (beber x1.7, carne x1.8)\n" BUNDLED_FILE
                          "\tBeber muy rapido (x2.5)\n");
        SDL_CloseIO(off);
    }
    SDL_IOStream *m = SDL_IOFromFile(BUNDLED_MARK, "wb");
    if (m) SDL_CloseIO(m);
    LOG("[cheats] the port's optional cheats copied into cheats/, off\n");
}
#endif
