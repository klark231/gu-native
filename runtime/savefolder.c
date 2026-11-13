// The save folder: runtime/savefolder.h says what each file is. Platform-neutral (Android, Windows, Linux): the
// screens that choose the folder are the platform's (Android: SaveFolderActivity; a PC: its menu).
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#include "config.h"
#include "overlay.h"
#include "platform.h"
#include "rt.h"
#include "savefolder.h"

#define CHOSEN_FILE  "save_folder.txt"
#define PENDING_FILE "save_folder_pending.txt"
#define STATE_FILE   "save_folder_state.txt"
#define MISSED_FILE  "save_folder_missed.txt"
#define DEVICE_FILE  "device.txt"
#define MARKER       ".mhgu-in-use"
#define SAVE_SIZE    5159100
#define MARK_AT      0x14
#define REFRESH_S    300  // (the in-use mark: renewed every 5 minutes; the start gate counts 15 as nobody's)

static const unsigned char save_mark[4] = {0x74, 0xee, 0xb2, 0x36};

static char g_chosen_utf8[1024];  // save_folder.txt as written
static char g_chosen[1024];       // the same, as the runtime's file functions spell it
static char g_dev_id[40], g_dev_name[128];
static bool g_marked;             // this run wrote the in-use mark
static bool g_missed_told;
// The game's own folder for the rest of this run: a mount of the save missed the chosen folder once (savefolder_root).
// Never back to the folder in the same run: the game holds the save it read, and if it read this device's copy because
// the folder wasn't there yet, a later save into the folder would put that older save over the folder's newer one. 2:
// the notice that says so is due (savefolder_tick, the host thread; set from the file system's thread)
static SDL_AtomicInt g_own;
static char g_later[300];         // a notice for when the game shows (savefolder_tick)

static bool es(void) { return config_language() == 1; }

// ------------------------------------------------------------------ small files
static bool read_line_file(const char *path, char *out, size_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fgets(out, (int)size, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\r\n")] = 0;
    return ok && out[0];
}

// key=value of a small file (the first one), trimmed
static bool read_key(const char *path, const char *key, char *out, size_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char line[1200];
    size_t k = strlen(key);
    bool found = false;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, key, k) && line[k] == '=') {
            snprintf(out, size, "%s", line + k + 1);
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

// a small file written whole: beside it first, then renamed over it
static bool write_file(const char *path, const char *text) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fputs(text, f) >= 0;
    if (fclose(f)) ok = false;
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
        ok = rename(tmp, path) == 0;
#endif
    }
    if (!ok) remove(tmp);
    return ok;
}

static void join(char *out, size_t size, const char *dir, const char *name) {
    size_t n = strlen(dir);
    snprintf(out, size, "%s%s%s", dir, n && (dir[n - 1] == '/' || dir[n - 1] == '\\') ? "" : "/", name);
}

static void mkdirs(const char *path) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++)
        if (*q == '/' || *q == '\\') { char c = *q; *q = 0; mkdir(tmp, 0755); *q = c; }
    mkdir(tmp, 0755);
}

static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// A new file can be made (and is deleted again) in dir: the folder is there and can be written.
static bool writable(const char *dir) {
    if (!is_dir(dir)) return false;
    char p[1200];
    snprintf(p, sizeof p, "%s/.mhgu-probe-%lu", dir, (unsigned long)plat_process_id());
    int fd = open(p, O_CREAT | O_EXCL | O_WRONLY | O_BINARY, 0644);
    if (fd < 0) return false;
    close(fd);
    remove(p);
    return true;
}

// ------------------------------------------------------------------ a save
static bool is_save(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size != SAVE_SIZE) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char head[MARK_AT + 4];
    bool ok = fread(head, 1, sizeof head, f) == sizeof head && !memcmp(head + MARK_AT, save_mark, 4);
    fclose(f);
    return ok;
}

int savefolder_describe(const char *dir, char *names, size_t names_size, int64_t *saved) {
    char path[1200];
    join(path, sizeof path, dir, "system");
    if (names && names_size) names[0] = 0;
    if (!is_save(path)) return 0;
    struct stat st;
    if (saved) *saved = stat(path, &st) == 0 ? (int64_t)st.st_mtime : 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    // (the three character slots: u32 at +0x34/+0x38/+0x3c, from 0x24; a hunter's name is the slot's first 32 bytes)
    for (int at = 0x34; at <= 0x3c && names; at += 4) {
        unsigned char b[4];
        char name[33] = {0};
        if (fseek(f, at, SEEK_SET) || fread(b, 1, 4, f) != 4) break;
        uint32_t slot = (uint32_t)(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24) + 0x24;
        if (slot + 32 > SAVE_SIZE || fseek(f, (long)slot, SEEK_SET) || fread(name, 1, 32, f) != 32) continue;
        name[32] = 0;
        if (!name[0]) continue;
        size_t n = strlen(names);
        snprintf(names + n, names_size - n, "%s%s", n ? ", " : "", name);
    }
    fclose(f);
    return 1;
}

// src over dst: copied beside it, checked, renamed over it
static bool copy_checked(const char *src, const char *dst) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.mhgu-copy", dst);
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(tmp, "wb");
    if (!out) { fclose(in); return false; }
    static char buf[1 << 16];
    bool ok = true;
    for (size_t n; (n = fread(buf, 1, sizeof buf, in)) > 0;)
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fflush(out) || fsync(fileno(out))) ok = false;
    if (fclose(out)) ok = false;
    struct stat a, b;
    if (ok && (stat(src, &a) != 0 || stat(tmp, &b) != 0 || a.st_size != b.st_size)) ok = false;
    if (ok && a.st_size == SAVE_SIZE && !is_save(tmp)) ok = false;
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING) != 0;
#else
        ok = rename(tmp, dst) == 0;
#endif
    }
    if (!ok) remove(tmp);
    return ok;
}

static bool copy_save(const char *from, const char *to) {
    mkdirs(to);
    static const char *const files[] = {"system", "system_backup"};
    char sys[1200];
    join(sys, sizeof sys, from, "system");
    for (int i = 0; i < 2; i++) {
        char src[1200], dst[1200];
        join(src, sizeof src, from, files[i]);
        if (access(src, F_OK)) snprintf(src, sizeof src, "%s", sys);  // (no system_backup: system takes its place)
        join(dst, sizeof dst, to, files[i]);
        if (!copy_checked(src, dst)) return false;
    }
    return true;
}

// The save in dir into the game's own save_backups/<date>-<what>
static bool backup(const char *dir, const char *what) {
    char stamp[64], keep[1200];
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &tm);
    snprintf(keep, sizeof keep, "%s/../save_backups/%s-%s", g_save_dir, stamp, what);
    mkdirs(keep);
    static const char *const files[] = {"system", "system_backup"};
    for (int i = 0; i < 2; i++) {
        char src[1200], dst[1200];
        join(src, sizeof src, dir, files[i]);
        if (access(src, F_OK)) continue;
        join(dst, sizeof dst, keep, files[i]);
        if (!copy_checked(src, dst)) return false;
    }
    LOG("[save folder] the save of %s kept in %s\n", dir, keep);
    return true;
}

// ------------------------------------------------------------------ the change, at the start
static void native_path(const char *utf8, char *out, size_t size) {
#ifdef _WIN32
    if (!plat_path_from_utf8(utf8, out, size)) snprintf(out, size, "%s", utf8);
#else
    snprintf(out, size, "%s", utf8);
#endif
}

bool savefolder_set_pending(const char *folder, bool keep_folder) {
    char text[1200];
    snprintf(text, sizeof text, "folder=%s\nkeep=%s\n", folder ? folder : "", keep_folder ? "folder" : "device");
    return write_file(PENDING_FILE, text);
}

const char *savefolder_apply_pending(void) {
    static char why[400];
    char folder_utf8[1024] = "", keep[32] = "";
    if (access(PENDING_FILE, F_OK)) return NULL;
    read_key(PENDING_FILE, "folder", folder_utf8, sizeof folder_utf8);
    read_key(PENDING_FILE, "keep", keep, sizeof keep);
    bool keep_folder = !strcmp(keep, "folder");
    char now_utf8[1024] = "", now[1024] = "", own[1100];
    read_line_file(CHOSEN_FILE, now_utf8, sizeof now_utf8);
    if (now_utf8[0]) native_path(now_utf8, now, sizeof now);
    snprintf(own, sizeof own, "%s/save_0", g_save_dir);
    const char *from = now[0] ? now : own;
    why[0] = 0;
    if (folder_utf8[0]) {
        mkdirs(folder_utf8);  // (made before its short name exists: plat_path_from_utf8 needs it there)
        char to[1024];
        native_path(folder_utf8, to, sizeof to);
        mkdirs(to);
        if (!writable(to)) {
            snprintf(why, sizeof why, es() ? "no se puede escribir en %s" : "can't write in %s", folder_utf8);
        } else if (strcmp(to, from)) {
            int there = savefolder_describe(to, NULL, 0, NULL), here = savefolder_describe(from, NULL, 0, NULL);
            bool ok = true;
            if (!keep_folder || !there) {
                if (there) ok = backup(to, "folder");
                if (ok && here) ok = copy_save(from, to);
            } else if (here) {
                ok = backup(from, "device");
            }
            if (!ok) snprintf(why, sizeof why, es() ? "no se pudo copiar la partida a %s" : "the save couldn't be copied to %s", folder_utf8);
        }
        if (!why[0] && !write_file(CHOSEN_FILE, folder_utf8))
            snprintf(why, sizeof why, "%s", es() ? "no se pudo escribir save_folder.txt" : "save_folder.txt couldn't be written");
        if (!why[0]) LOG("[save folder] now %s (%s)\n", folder_utf8, keep_folder ? "its save kept" : "this device's save copied there");
    } else if (now[0]) {
        if (keep_folder && savefolder_describe(now, NULL, 0, NULL)) {
            bool ok = !savefolder_describe(own, NULL, 0, NULL) || backup(own, "device");
            if (ok) ok = copy_save(now, own);
            if (!ok) snprintf(why, sizeof why, "%s", es() ? "no se pudo copiar la partida a la carpeta propia del juego"
                                                          : "the save couldn't be copied to the game's own folder");
        }
        if (!why[0]) {
            remove(CHOSEN_FILE);
            LOG("[save folder] back to the game's own folder\n");
        }
    }
    remove(PENDING_FILE);
    if (!why[0]) {
        remove(STATE_FILE);
        remove(MISSED_FILE);
        return NULL;
    }
    LOG("[save folder] not changed: %s\n", why);
    return why;
}

// ------------------------------------------------------------------ this device and its mark
static void device(void) {
    if (read_key(DEVICE_FILE, "id", g_dev_id, sizeof g_dev_id) && g_dev_id[0]) {
        if (!read_key(DEVICE_FILE, "name", g_dev_name, sizeof g_dev_name)) snprintf(g_dev_name, sizeof g_dev_name, "?");
        return;
    }
    uint64_t x = (uint64_t)time(NULL) ^ SDL_GetPerformanceCounter() ^ ((uint64_t)plat_process_id() << 32);
    for (int i = 0; i < 4; i++) {  // (splitmix64: the id only has to differ between devices)
        x += 0x9e3779b97f4a7c15ull;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        x ^= z ^ (z >> 31);
    }
    snprintf(g_dev_id, sizeof g_dev_id, "%016llx", (unsigned long long)x);
#ifdef __ANDROID__
    char model[PROP_VALUE_MAX] = "";
    __system_property_get("ro.product.model", model);
    snprintf(g_dev_name, sizeof g_dev_name, "%s", model[0] ? model : "Android");
#elif defined(_WIN32)
    const char *pc = getenv("COMPUTERNAME");
    snprintf(g_dev_name, sizeof g_dev_name, "%s", pc && *pc ? pc : "PC");
#else
    if (gethostname(g_dev_name, sizeof g_dev_name) != 0 || !g_dev_name[0]) snprintf(g_dev_name, sizeof g_dev_name, "PC");
#endif
    char text[256];
    snprintf(text, sizeof text, "id=%s\nname=%s\n", g_dev_id, g_dev_name);
    write_file(DEVICE_FILE, text);
}

static void mark(void) {
    if (!g_chosen[0] || !is_dir(g_chosen)) return;
    char path[1200], text[300];
    join(path, sizeof path, g_chosen, MARKER);
    snprintf(text, sizeof text, "id=%s\nname=%s\ntime=%lld\n", g_dev_id, g_dev_name, (long long)time(NULL));
    FILE *f = fopen(path, "wb");  // (in place: a sync app sees one file change, not a rename)
    if (!f) return;
    fputs(text, f);
    fclose(f);
    g_marked = true;
}

static void unmark(void) {  // (atexit: a clean exit; a killed app's mark goes stale in 15 minutes)
    if (!g_marked || !g_chosen[0]) return;
    char path[1200], id[40] = "";
    join(path, sizeof path, g_chosen, MARKER);
    if (read_key(path, "id", id, sizeof id) && !strcmp(id, g_dev_id)) remove(path);
}

void savefolder_init(void) {
    const char *failed = savefolder_apply_pending();
    if (failed) {
        snprintf(g_later, sizeof g_later, es() ? "No se cambió la carpeta de partidas: %s. Tu partida sigue donde estaba."
                                               : "The save folder wasn't changed: %s. Your save is where it was.", failed);
    }
    if (getenv("MHGU_SAVE_DIR") && *getenv("MHGU_SAVE_DIR")) return;  // (tests: their folder, whatever is chosen)
    if (!read_line_file(CHOSEN_FILE, g_chosen_utf8, sizeof g_chosen_utf8)) return;
    native_path(g_chosen_utf8, g_chosen, sizeof g_chosen);
    device();
    LOG("[save folder] %s%s\n", g_chosen_utf8, is_dir(g_chosen) ? "" : " (not there now)");
    mark();
    atexit(unmark);
}

const char *savefolder_chosen(void) { return g_chosen_utf8; }

bool savefolder_root(const char *sub, char *out, size_t size) {
    bool missed = false;
    if (g_chosen[0] && !strcmp(sub, "save_0")) {
        if (!SDL_GetAtomicInt(&g_own)) {
            if (writable(g_chosen)) {
                snprintf(out, size, "%s", g_chosen);
                return false;
            }
            LOG("[save folder] %s can't be reached: the game's own folder is used until the game closes\n", g_chosen_utf8);
            SDL_SetAtomicInt(&g_own, 2);
        }
        missed = true;
    }
    snprintf(out, size, "%s/%s", g_save_dir, sub);
    return missed;
}

void savefolder_missed(void) {
    char text[64];
    snprintf(text, sizeof text, "time=%lld\n", (long long)time(NULL));
    write_file(MISSED_FILE, text);
    LOG("[save folder] the save went to the game's own folder\n");
    if (!g_missed_told && SDL_GetAtomicInt(&g_own) != 2) {  // (2: savefolder_tick tells it)
        g_missed_told = true;
        overlay_notice(es() ? "Carpeta de partidas" : "Save folder",
                       es() ? "No se pudo llegar a la carpeta, así que esta partida quedó en este dispositivo. Cuando vuelva, "
                              "el juego te preguntará cuál conservar."
                            : "The folder couldn't be reached, so this save stayed on this device. When it is back, the game "
                              "will ask which save to keep.");
    }
}

void savefolder_tick(void) {
    static uint64_t next;
    uint64_t now = SDL_GetTicks();
    if (now < next) return;
    next = now + 1000;
    if (g_later[0] && now > 20000) {  // (after the start's first seconds: a notice lasts 7 s, the boot takes longer)
        overlay_notice(es() ? "Carpeta de partidas" : "Save folder", g_later);
        g_later[0] = 0;
    }
    if (now > 20000 && SDL_CompareAndSwapAtomicInt(&g_own, 2, 1)) {
        g_missed_told = true;  // (this one says it: not again at the save)
        overlay_notice(es() ? "Carpeta de partidas" : "Save folder",
                       es() ? "No se llega a la carpeta de partidas, así que hasta que cierres el juego se usa la partida de este "
                              "dispositivo. Si guardas, al próximo inicio el juego te preguntará cuál conservar."
                            : "The save folder can't be reached, so until the game closes it uses this device's save. If you "
                              "save, the game asks at its next start which save to keep.");
    }
    static time_t last;
    time_t t = time(NULL);
    if (g_chosen[0] && t - last >= REFRESH_S) {
        if (last && !SDL_GetAtomicInt(&g_own)) mark();  // (the folder no longer used: its mark goes stale)
        last = t;
    }
}
