// The mods folder for the Mods tab of the settings menu (modorder.h).
#include "modorder.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "arcmerge.h"

extern const char *g_game_dir;  // (main.c)

static bool is_dir(const char *p) {
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
}

// MHGU_MODS_DIR, else the game folder's mods/ when there is one (a phone: a file manager reaches it, and not the app's
// own folder in Android/data), else the app's mods/
const char *mods_dir(void) {
    const char *e = getenv("MHGU_MODS_DIR");
    if (e && *e) return e;
    static char in_game[1100];
    if (!in_game[0] && g_game_dir) snprintf(in_game, sizeof in_game, "%s/mods", g_game_dir);
    return in_game[0] && is_dir(in_game) ? in_game : "mods";
}

// The folder named `want` (any case) in dir or up to `depth` levels below it (shallowest first), into out.
bool mods_find_dir(const char *dir, const char *want, int depth, char *out, size_t n) {
    for (int level = 0; level <= depth; level++) {
        // breadth first: walk the tree to `level` and look there
        struct { char path[1024]; int lv; } stack[64];
        int sp = 0;
        snprintf(stack[sp].path, sizeof stack[sp].path, "%s", dir), stack[sp++].lv = 0;
        while (sp) {
            sp--;
            char cur[1024];
            int lv = stack[sp].lv;
            snprintf(cur, sizeof cur, "%s", stack[sp].path);
            DIR *d = opendir(cur);
            if (!d) continue;
            struct dirent *e;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                char p[1100];
                snprintf(p, sizeof p, "%s/%s", cur, e->d_name);
                if (!is_dir(p)) continue;
                if (lv == level && !strcasecmp(e->d_name, want)) {
                    closedir(d);
                    snprintf(out, n, "%s", p);
                    return true;
                }
                if (lv < level && sp < 64) snprintf(stack[sp].path, sizeof stack[sp].path, "%s", p), stack[sp++].lv = lv + 1;
            }
            closedir(d);
        }
    }
    return false;
}


bool mods_romfs_of(const char *name, char *out, size_t n) {
    char mod[1100];
    snprintf(mod, sizeof mod, "%s/%s", mods_dir(), name);
    if (mods_find_dir(mod, "romfs", 3, out, n)) return true;
    if (!mods_find_dir(mod, "nativeNX", 3, out, n)) return false;  // the files without their romfs/ folder
    *strrchr(out, '/') = 0;
    return true;
}

// ------------------------------------------------------------------ load order
static int cmp_names(const void *a, const void *b) { return strcasecmp(((const ModEntry *)a)->name, ((const ModEntry *)b)->name); }

#ifndef __ANDROID__
// The mods a desktop's settings menu removed (runtime/pc_actions.c): mods/_remove.txt, a folder's name per line, read
// whole (NULL: none). Left out until their folders are gone, which may wait for the next start.
static char *removed_list(void) {
    char path[1100];
    snprintf(path, sizeof path, "%s/_remove.txt", mods_dir());
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = len >= 0 ? malloc((size_t)len + 1) : NULL;
    size_t got = s ? fread(s, 1, (size_t)len, f) : 0;
    fclose(f);
    if (s) s[got] = 0;
    return s;
}

static bool removed(const char *list, const char *name) {
    size_t n = strlen(name);
    for (const char *p = list; p && *p;) {
        const char *e = strchr(p, '\n'), *s = p;
        size_t l = e ? (size_t)(e - p) : strlen(p);
        while (l && (*s == ' ' || *s == '\t')) s++, l--;
        while (l && s[l - 1] == '\r') l--;
        if (l == n && !memcmp(s, name, n)) return true;
        p = e ? e + 1 : NULL;
    }
    return false;
}
#endif

int mods_installed(ModEntry *out, int max) {
    int n = 0;
#ifndef __ANDROID__
    char *gone = removed_list();
#endif
    DIR *d = opendir(mods_dir());
    if (d) {
        struct dirent *e;
        while (n < max && (e = readdir(d))) {
            char path[1100];
            if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof out->name) continue;
#ifndef __ANDROID__
            if (removed(gone, e->d_name)) continue;
#endif
            snprintf(path, sizeof path, "%s/%s", mods_dir(), e->d_name);
            if (!is_dir(path)) continue;
            snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name);
            out[n++].on = false;
        }
        closedir(d);
    }
#ifndef __ANDROID__
    free(gone);
#endif
    qsort(out, (size_t)n, sizeof *out, cmp_names);

    char path[1100];
    snprintf(path, sizeof path, "%s/load_order.txt", mods_dir());
    FILE *f = fopen(path, "r");
    if (!f) {  // no list: all load by name, except the folders starting with '_'
        ModEntry *by_state = calloc((size_t)n + 1, sizeof *by_state);
        int k = 0;
        for (int pass = 0; pass < 2 && by_state; pass++)
            for (int i = 0; i < n; i++)
                if ((out[i].name[0] != '_') == !pass) by_state[k] = out[i], by_state[k++].on = !pass;
        if (by_state) memcpy(out, by_state, sizeof *out * (size_t)n);
        free(by_state);
        return n;
    }
    ModEntry *ordered = calloc((size_t)n + 1, sizeof *ordered);
    int k = 0;
    bool ours = false;  // a list the Mods tab wrote: "# off: name" keeps a mod's place, and a folder it doesn't
                        // mention is a new mod, which loads
    char line[512];
    for (int ln = 0; ordered && fgets(line, sizeof line, f); ln++) {
        line[strcspn(line, "\r\n")] = 0;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!ln && !strncmp(s, MODS_LIST_MARK, strlen(MODS_LIST_MARK))) ours = true;
        bool on = true;
        if (ours && !strncmp(s, "# off: ", 7)) s += 7, on = false;
        else if (!*s || *s == '#') continue;
        for (int i = 0; i < n; i++)
            if (out[i].name[0] && !strcmp(out[i].name, s)) {
                ordered[k] = out[i];
                ordered[k++].on = on;
                out[i].name[0] = 0;  // (taken)
                break;
            }
    }
    fclose(f);
    for (int i = 0; i < n && ordered; i++)  // the folders the list doesn't mention, by name: new mods load
        if (out[i].name[0]) {
            ordered[k] = out[i];
            ordered[k++].on = ours;
        }
    if (ordered) memcpy(out, ordered, sizeof *out * (size_t)n);
    free(ordered);
    return n;
}

bool mods_save_order(const ModEntry *e, int n, char *err, size_t errsz) {
    char path[1200], tmp[1300];
    snprintf(path, sizeof path, "%s/load_order.txt", mods_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        snprintf(err, errsz, "Could not write mods/load_order.txt");
        return false;
    }
    fputs(MODS_LIST_MARK " of the settings menu (also fine to edit by hand).\n"
          "# One mod folder per line, first to last: a file in a later mod wins.\n"
          "# A folder added later, that is neither listed nor marked off, loads after the listed ones.\n",
          f);
    for (int i = 0; i < n; i++)  // (a mod that is off keeps its place, for when it is turned on again)
        fprintf(f, "%s%s\n", e[i].on ? "" : "# off: ", e[i].name);
    bool ok = fclose(f) == 0;
#ifdef _WIN32
    if (ok) ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;  // (Windows' rename won't replace a file)
#else
    if (ok) ok = rename(tmp, path) == 0;
#endif
    if (!ok) {
        remove(tmp);
        snprintf(err, errsz, "Could not write mods/load_order.txt");
    }
    return ok;
}

// ------------------------------------------------------------------ conflicts
typedef struct {
    char *rel, *path;  // rel: lower case, inside the RomFS
    long long size;
    int mod;
} Item;
typedef struct {
    Item *v;
    int n, cap;
} Items;

static void walk(Items *it, int mod, const char *root, const char *rel, int depth) {
    if (depth > 32) return;
    char dir[1400];
    snprintf(dir, sizeof dir, "%s%s%s", root, *rel ? "/" : "", rel);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char r[1400], p[1500];
        snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        snprintf(p, sizeof p, "%s/%s", root, r);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            walk(it, mod, root, r, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            if (it->n == it->cap) it->v = realloc(it->v, sizeof *it->v * (size_t)(it->cap = it->cap ? it->cap * 2 : 1024));
            for (char *c = r; *c; c++) *c = (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
            it->v[it->n++] = (Item){strdup(r), strdup(p), (long long)st.st_size, mod};
        }
    }
    closedir(d);
}

static int cmp_item(const void *a, const void *b) {
    const Item *x = a, *y = b;
    int c = strcmp(x->rel, y->rel);
    return c ? c : x->mod - y->mod;
}

// The game's own file for `rel` (any case), into out
static bool game_file(const char *rel, char *out, size_t n) {
    char base[1100];
    snprintf(base, sizeof base, "%s/romfs", g_game_dir ? g_game_dir : "game");
    char cur[1500];
    snprintf(cur, sizeof cur, "%s", base);
    char part[1400];
    snprintf(part, sizeof part, "%s", rel);
    for (char *s = strtok(part, "/"); s; s = strtok(NULL, "/")) {
        DIR *d = opendir(cur);
        if (!d) return false;
        struct dirent *e;
        bool found = false;
        while ((e = readdir(d)))
            if (!strcasecmp(e->d_name, s)) {
                size_t l = strlen(cur);
                snprintf(cur + l, sizeof cur - l, "/%s", e->d_name);
                found = true;
                break;
            }
        closedir(d);
        if (!found) return false;
    }
    snprintf(out, n, "%s", cur);
    return true;
}

static bool same_file(const Item *a, const Item *b) {
    if (a->size != b->size) return false;
    FILE *x = fopen(a->path, "rb"), *y = fopen(b->path, "rb");
    bool same = x && y;
    char bx[65536], by[65536];
    while (same) {
        size_t nx = fread(bx, 1, sizeof bx, x), ny = fread(by, 1, sizeof by, y);
        if (nx != ny || memcmp(bx, by, nx)) same = false;
        if (nx < sizeof bx) break;
    }
    if (x) fclose(x);
    if (y) fclose(y);
    return same;
}

static ModConflict *pair(ModScan *s, int *cap, int a, int b) {
    for (int i = 0; i < s->nconflicts; i++)
        if (s->conflicts[i].a == a && s->conflicts[i].b == b) return &s->conflicts[i];
    if (s->nconflicts == *cap) s->conflicts = realloc(s->conflicts, sizeof *s->conflicts * (size_t)(*cap = *cap ? *cap * 2 : 16));
    ModConflict *c = &s->conflicts[s->nconflicts++];
    memset(c, 0, sizeof *c);
    c->a = a, c->b = b;
    return c;
}

void modscan_run(const char (*names)[128], int n, ModScan *out) {
    memset(out, 0, sizeof *out);
    if (n > MODS_MAX) n = MODS_MAX;
    out->nmods = n;
    Items it = {0};
    for (int i = 0; i < n; i++) {
        char root[1100];
        int before = it.n;
        if (mods_romfs_of(names[i], root, sizeof root)) walk(&it, i, root, "", 0);
        out->files[i] = it.n - before;
    }
    qsort(it.v, (size_t)it.n, sizeof *it.v, cmp_item);
    int cap = 0;
    for (int i = 0; i < it.n;) {
        int j = i;
        while (j < it.n && !strcmp(it.v[j].rel, it.v[i].rel)) j++;
        size_t len = strlen(it.v[i].rel);
        bool arc = len > 4 && !strcmp(it.v[i].rel + len - 4, ".arc");
        char orig[1500];
        bool have_orig = j - i > 1 && arc && game_file(it.v[i].rel, orig, sizeof orig);
        for (int x = i; x < j; x++)
            for (int y = x + 1; y < j; y++) {
                if (it.v[x].mod == it.v[y].mod) continue;  // (the same mod's folder twice)
                int ov = -1;
                char entry[80] = "";
                if (have_orig) ov = arc_overlap(orig, it.v[x].path, it.v[y].path, entry, sizeof entry);
                if (ov == 0) {
                    ModConflict *c = pair(out, &cap, it.v[x].mod, it.v[y].mod);
                    c->merged++;
                    if (!c->example[0]) snprintf(c->example, sizeof c->example, "%s", it.v[x].rel);
                    continue;
                }
                if (ov < 0 && same_file(&it.v[x], &it.v[y])) continue;
                ModConflict *c = pair(out, &cap, it.v[x].mod, it.v[y].mod);
                c->replaced++;
                if (c->replaced == 1) snprintf(c->example, sizeof c->example, "%s", it.v[x].rel);
            }
        i = j;
    }
    for (int i = 0; i < it.n; i++) free(it.v[i].rel), free(it.v[i].path);
    free(it.v);
}

void modscan_free(ModScan *s) {
    free(s->conflicts);
    s->conflicts = NULL;
    s->nconflicts = 0;
}
