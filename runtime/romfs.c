// Virtual RomFS image (romfs.h). Layout as a real image: a 0x50-byte header, file data from 0x200 (each file
// 16-byte aligned), then the directory hash table, directory entries, file hash table and file entries. The game's
// RomFS code walks the tables itself, finding entries by hash, so their order only has to be consistent.
#include "romfs.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "arcmerge.h"
#include "rt.h"

#define EMPTY 0xFFFFFFFFu
#define DATA_START 0x200u

typedef struct Dir Dir;
typedef struct {
    char *name, *path;  // path: the host file
    char *orig;         // the game's own file, once a mod has replaced it
    uint64_t size, data_off;
    uint32_t off;
    Dir *parent;
    char *rpath;        // path inside the RomFS
} File;
struct Dir {
    char *name;
    Dir *parent;
    Dir **dirs;
    File **files;
    int ndirs, nfiles, cdirs, cfiles;
    uint32_t off;
};

static int by_name_dir(const void *a, const void *b) { return strcmp((*(Dir *const *)a)->name, (*(Dir *const *)b)->name); }
static int by_name_file(const void *a, const void *b) { return strcmp((*(File *const *)a)->name, (*(File *const *)b)->name); }

static void add_dir(Dir *d, Dir *c) {
    if (d->ndirs == d->cdirs) d->dirs = realloc(d->dirs, sizeof *d->dirs * (d->cdirs = d->cdirs ? d->cdirs * 2 : 8));
    d->dirs[d->ndirs++] = c;
    c->parent = d;
}
static void add_file(Dir *d, File *f) {
    if (d->nfiles == d->cfiles) d->files = realloc(d->files, sizeof *d->files * (d->cfiles = d->cfiles ? d->cfiles * 2 : 16));
    d->files[d->nfiles++] = f;
    f->parent = d;
}

static Dir *scan(const char *path, const char *name, int depth) {
    if (depth > 32) return NULL;
    DIR *dd = opendir(path);
    if (!dd) return NULL;
    Dir *dir = calloc(1, sizeof *dir);
    dir->name = strdup(name);
    struct dirent *e;
    while ((e = readdir(dd))) {
        if (e->d_name[0] == '.') continue;
        size_t n = strlen(path) + strlen(e->d_name) + 2;
        char *p = malloc(n);
        snprintf(p, n, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) { free(p); continue; }
        if (S_ISDIR(st.st_mode)) {
            Dir *c = scan(p, e->d_name, depth + 1);
            free(p);
            if (c) add_dir(dir, c);
        } else if (S_ISREG(st.st_mode)) {
            File *f = calloc(1, sizeof *f);
            f->name = strdup(e->d_name), f->path = p, f->size = (uint64_t)st.st_size;
            add_file(dir, f);
        } else {
            free(p);
        }
    }
    closedir(dd);
    return dir;
}

// overlay `src` onto `dst`: its files replace or join dst's, its folders merge. Names match in any case (a mod's
// nativenx/ is the game's nativeNX/: Windows doesn't tell them apart either); the game's own spelling stays.
static int merge(Dir *dst, Dir *src) {
    int changed = 0;
    for (int i = 0; i < src->nfiles; i++) {
        File *f = src->files[i];
        int k = 0;
        while (k < dst->nfiles && strcasecmp(dst->files[k]->name, f->name)) k++;
        if (k < dst->nfiles) {
            File *d = dst->files[k];
            size_t nl = strlen(f->name);
            char merged[1200];
            if (d->orig && nl > 4 && !strcasecmp(f->name + nl - 4, ".arc") &&
                arc_merge(d->orig, d->path, f->path, merged, sizeof merged)) {  // (two mods' archives: both's changes)
                struct stat st;
                if (!stat(merged, &st)) {
                    free(d->path);
                    d->path = strdup(merged);
                    d->size = (uint64_t)st.st_size;
                    changed++;
                    continue;
                }
            }
            if (!d->orig) d->orig = strdup(d->path);
            free(d->path);
            d->path = strdup(f->path);
            d->size = f->size;
        } else {
            File *n = calloc(1, sizeof *n);
            n->name = strdup(f->name), n->path = strdup(f->path), n->size = f->size;
            add_file(dst, n);
        }
        changed++;
    }
    for (int i = 0; i < src->ndirs; i++) {
        Dir *s = src->dirs[i];
        int k = 0;
        while (k < dst->ndirs && strcasecmp(dst->dirs[k]->name, s->name)) k++;
        if (k == dst->ndirs) {
            Dir *n = calloc(1, sizeof *n);
            n->name = strdup(s->name);
            add_dir(dst, n);
        }
        changed += merge(dst->dirs[k], s);
    }
    return changed;
}

static void sort_tree(Dir *d) {
    if (d->ndirs) qsort(d->dirs, d->ndirs, sizeof *d->dirs, by_name_dir);
    if (d->nfiles) qsort(d->files, d->nfiles, sizeof *d->files, by_name_file);
    for (int i = 0; i < d->ndirs; i++) sort_tree(d->dirs[i]);
}

typedef struct { Dir **dirs; File **files; int nd, nf, cd, cf; } Lists;
static void flatten(Dir *d, Lists *l) {
    for (int i = 0; i < d->nfiles; i++) {
        if (l->nf == l->cf) l->files = realloc(l->files, sizeof *l->files * (l->cf = l->cf ? l->cf * 2 : 256));
        l->files[l->nf++] = d->files[i];
    }
    for (int i = 0; i < d->ndirs; i++) {
        if (l->nd == l->cd) l->dirs = realloc(l->dirs, sizeof *l->dirs * (l->cd = l->cd ? l->cd * 2 : 64));
        l->dirs[l->nd++] = d->dirs[i];
    }
    for (int i = 0; i < d->ndirs; i++) flatten(d->dirs[i], l);
}

static uint32_t name_hash(uint32_t parent, const char *name) {
    uint32_t h = parent ^ 123456789u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) h = ((h >> 5) | (h << 27)) ^ *p;
    return h;
}
static uint32_t bucket_count(uint32_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    uint32_t c = n;
    while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0) c++;
    return c;
}
static uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }
static uint64_t align16(uint64_t x) { return (x + 15) & ~15ull; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

// ------------------------------------------------------------------ the image
// One image: the game's (romfs_*), or a DLC's (romfs_image_*: services/dlc.c)
struct RomfsImage {
    uint8_t hdr[DATA_START];
    uint8_t *tables;          // dir hash, dir meta, file hash, file meta (contiguous)
    uint64_t tables_off, tables_size, size;
    File **files;             // by data offset
    int nfiles;
};
static RomfsImage g_main;

static char *rel_path(File *f) {
    char *parts[40];
    int n = 0;
    for (Dir *d = f->parent; d && d->parent && n < 40; d = d->parent) parts[n++] = d->name;
    size_t len = strlen(f->name) + 1;
    for (int i = 0; i < n; i++) len += strlen(parts[i]) + 1;
    char *s = malloc(len);
    s[0] = 0;
    for (int i = n - 1; i >= 0; i--) { strcat(s, parts[i]); strcat(s, "/"); }
    strcat(s, f->name);
    return s;
}

static double g_short_gb;  // (the game's image, when it looks cut short: its size, and its empty files)
static int g_short_empty;

static bool build(RomfsImage *im, const char *base_dir, const char *const *overlays, int noverlays, bool game) {
    Dir *root = scan(base_dir, "", 0);
    if (!root) { LOG("[romfs] cannot read %s\n", base_dir); return false; }
    for (int i = 0; i < noverlays; i++) {
        Dir *o = scan(overlays[i], "", 0);
        if (!o) continue;
        int n = merge(root, o);
        LOG("[romfs] mod %s: %d files\n", overlays[i], n);
    }
    sort_tree(root);
    Lists l = {0};
    l.dirs = malloc(sizeof *l.dirs * (l.cd = 64));
    l.dirs[l.nd++] = root;
    flatten(root, &l);

    uint32_t dsize = 0, fsize = 0;
    for (int i = 0; i < l.nd; i++) l.dirs[i]->off = dsize, dsize += 0x18 + align4((uint32_t)strlen(l.dirs[i]->name));
    for (int i = 0; i < l.nf; i++) l.files[i]->off = fsize, fsize += 0x20 + align4((uint32_t)strlen(l.files[i]->name));
    uint64_t data = 0;
    for (int i = 0; i < l.nf; i++) {
        data = align16(data);
        l.files[i]->data_off = data;
        l.files[i]->rpath = rel_path(l.files[i]);
        data += l.files[i]->size;
    }
    uint32_t nbd = bucket_count((uint32_t)l.nd), nbf = bucket_count((uint32_t)l.nf);
    uint64_t dhash_off = (DATA_START + data + 3) & ~3ull;
    uint64_t dmeta_off = dhash_off + 4ull * nbd, fhash_off = dmeta_off + dsize, fmeta_off = fhash_off + 4ull * nbf;
    im->tables_off = dhash_off;
    im->tables_size = fmeta_off + fsize - dhash_off;
    im->tables = calloc(1, im->tables_size);
    uint32_t *dbuckets = (uint32_t *)im->tables, *fbuckets = (uint32_t *)(im->tables + (fhash_off - dhash_off));
    uint8_t *dtab = im->tables + (dmeta_off - dhash_off), *ftab = im->tables + (fmeta_off - dhash_off);
    for (uint32_t i = 0; i < nbd; i++) dbuckets[i] = EMPTY;
    for (uint32_t i = 0; i < nbf; i++) fbuckets[i] = EMPTY;
    for (int i = 0; i < l.nd; i++) {
        Dir *d = l.dirs[i];
        uint8_t *e = dtab + d->off;
        uint32_t parent = d->parent ? d->parent->off : 0, nl = (uint32_t)strlen(d->name), sib = EMPTY;
        if (d->parent)
            for (int k = 0; k + 1 < d->parent->ndirs; k++)
                if (d->parent->dirs[k] == d) sib = d->parent->dirs[k + 1]->off;
        uint32_t h = name_hash(parent, d->name) % nbd;
        put32(e, parent);
        put32(e + 4, sib);
        put32(e + 8, d->ndirs ? d->dirs[0]->off : EMPTY);
        put32(e + 12, d->nfiles ? d->files[0]->off : EMPTY);
        put32(e + 16, dbuckets[h]);
        put32(e + 20, nl);
        memcpy(e + 0x18, d->name, nl);
        dbuckets[h] = d->off;
    }
    for (int i = 0; i < l.nf; i++) {
        File *f = l.files[i];
        uint8_t *e = ftab + f->off;
        uint32_t nl = (uint32_t)strlen(f->name), sib = EMPTY;
        for (int k = 0; k + 1 < f->parent->nfiles; k++)
            if (f->parent->files[k] == f) sib = f->parent->files[k + 1]->off;
        uint32_t h = name_hash(f->parent->off, f->name) % nbf;
        put32(e, f->parent->off);
        put32(e + 4, sib);
        put64(e + 8, f->data_off);
        put64(e + 16, f->size);
        put32(e + 24, fbuckets[h]);
        put32(e + 28, nl);
        memcpy(e + 0x20, f->name, nl);
        fbuckets[h] = f->off;
    }
    uint64_t hv[10] = {0x50, dhash_off, 4ull * nbd, dmeta_off, dsize, fhash_off, 4ull * nbf, fmeta_off, fsize, DATA_START};
    memcpy(im->hdr, hv, sizeof hv);
    im->size = fmeta_off + fsize;
    im->files = l.files;  // (flatten order is data order)
    im->nfiles = l.nf;
    free(l.dirs);
    LOG("[romfs] %d files, %d folders, image %.1f GB\n", l.nf, l.nd, im->size / 1e9);
    if (!game) return true;
    // (version 1.4.0's is 12.3 GB, and none of its files is empty. A copy cut short, with every file there but some of
    // them empty or short, starts and then fails in ways that look like the port's: a black picture, a call to nothing,
    // the game closing in one place (#106). The player is told once the game shows: romfs_incomplete, overlay.cpp)
    int empty = 0;
    for (int i = 0; i < l.nf; i++)
        if (!l.files[i]->size && !strncmp(l.files[i]->path, base_dir, strlen(base_dir)) && empty++ < 3)  // (a mod's may be empty)
            LOG("[romfs] WARNING: empty file %s\n", l.files[i]->rpath);
    if (im->size < 12000000000ull) LOG("[romfs] WARNING: smaller than the game's 12.3 GB: some files are probably incomplete\n");
    if (empty) LOG("[romfs] WARNING: %d empty file(s): the copy is incomplete\n", empty);
    if (im->size < 12000000000ull || empty) g_short_gb = im->size / 1e9, g_short_empty = empty;
    const char *t = getenv("MHGU_ROMFS_SHORT");  // (test: the notice as if the copy were that many GB, e.g. 11.5)
    if (t && atof(t) > 0) g_short_gb = atof(t);
    return true;
}

bool romfs_incomplete(double *gb, int *empty) {
    if (gb) *gb = g_short_gb;
    if (empty) *empty = g_short_empty;
    return g_short_gb > 0;
}

bool romfs_open(const char *base_dir, const char *const *overlays, int noverlays) {
    return build(&g_main, base_dir, overlays, noverlays, true);
}

RomfsImage *romfs_image_open(const char *dir) {
    RomfsImage *im = calloc(1, sizeof *im);
    if (!build(im, dir, NULL, 0, false)) { free(im); return NULL; }
    return im;
}

uint64_t romfs_size(void) { return g_main.size; }
uint64_t romfs_image_size(const RomfsImage *im) { return im->size; }

// ---- host file descriptors (a small cache: the image has tens of thousands of files)
#define FDC 64
static struct { File *f; int fd; uint64_t used; } fdc[FDC];
static uint64_t fdc_clock;
static pthread_mutex_t fdc_lock = PTHREAD_MUTEX_INITIALIZER;

static void fdc_take(void) {
    if (HITCH_ON) hitch_mutex_lock(&fdc_lock, HC_FILE_LOCK);  // ((debug) MHGU_HITCHLOG: the waits, opens and reads timed)
    else pthread_mutex_lock(&fdc_lock);
}
static int fdc_find(File *f, int *oldest) {  // (fdc_lock held) f's slot, or -1 and the least used one
    int slot = -1, lru = 0;
    for (int i = 0; i < FDC; i++) {
        if (fdc[i].f == f) { slot = i; break; }
        if (fdc[i].used < fdc[lru].used) lru = i;
    }
    if (oldest) *oldest = lru;
    return slot;
}

static int64_t read_file(File *f, uint8_t *dst, uint64_t off, uint64_t n) {
    fdc_take();
    int oldest, slot = fdc_find(f, NULL), fd;
    if (slot >= 0) {
        fdc[slot].used = ++fdc_clock;
        fd = fdc[slot].fd;
        pthread_mutex_unlock(&fdc_lock);
    } else {
        // (opened with the lock let go: on a hard disk an open takes 20-30 ms, and every other file read - another game
        // thread's, the music's - waited that long for it; MHGU_HITCHLOG saw one wait 61 ms)
        pthread_mutex_unlock(&fdc_lock);
        uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
        int mine = open(f->path, O_RDONLY | O_BINARY);
        if (mine < 0) LOG("[romfs] cannot open %s\n", f->path);
        if (HITCH_ON) hitch_add_label(HC_FILE_OPEN, plat_now_ns() - t0, 0, f->rpath, -1);
        fdc_take();
        slot = fdc_find(f, &oldest);
        if (slot >= 0) {  // (another thread opened it meanwhile: its descriptor, ours closed)
            fdc[slot].used = ++fdc_clock;
            fd = fdc[slot].fd;
            pthread_mutex_unlock(&fdc_lock);
            if (mine >= 0) close(mine);
        } else {
            slot = oldest;
            if (fdc[slot].f && fdc[slot].fd >= 0) close(fdc[slot].fd);
            fdc[slot].f = f, fdc[slot].fd = mine, fdc[slot].used = ++fdc_clock;
            fd = mine;
            pthread_mutex_unlock(&fdc_lock);  // (pread doesn't move the file position: safe while others use it)
        }
    }
    if (fd < 0) return -1;
    if (!HITCH_ON) return plat_pread(fd, dst, n, (off_t)off);  // (dst: an IPC buffer in guest memory, platform.h)
    uint64_t t0 = plat_now_ns();
    int64_t r = plat_pread(fd, dst, n, (off_t)off);
    hitch_add_label(HC_FILE_READ, plat_now_ns() - t0, n, f->rpath, -1);
    return r;
}

static int file_at(const RomfsImage *im, uint64_t rel) {  // index of the file whose padded data range holds rel, or -1
    int lo = 0, hi = im->nfiles - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (im->files[mid]->data_off <= rel) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return best;
}

const char *romfs_file_at(uint64_t off) {
    if (off < DATA_START) return NULL;
    int i = file_at(&g_main, off - DATA_START);
    return i >= 0 && off - DATA_START < g_main.files[i]->data_off + g_main.files[i]->size ? g_main.files[i]->rpath : NULL;
}

const char *romfs_host_path(const char *rpath) {
    while (*rpath == '/') rpath++;
    for (int i = 0; i < g_main.nfiles; i++)  // (rare: movies)
        if (!strcasecmp(g_main.files[i]->rpath, rpath)) return g_main.files[i]->path;
    return NULL;
}

uint64_t romfs_image_read(const RomfsImage *im, uint8_t *dst, uint64_t off, uint64_t size) {
    if (off >= im->size) return 0;
    if (off + size > im->size) size = im->size - off;
    uint64_t done = 0;
    while (done < size) {
        uint64_t o = off + done, n = size - done;
        if (o < DATA_START) {  // header
            if (n > DATA_START - o) n = DATA_START - o;
            memcpy(dst + done, im->hdr + o, n);
        } else if (o >= im->tables_off) {  // tables
            memcpy(dst + done, im->tables + (o - im->tables_off), n);
        } else {  // file data (and the padding between files)
            if (o + n > im->tables_off) n = im->tables_off - o;
            uint64_t rel = o - DATA_START;
            int i = file_at(im, rel);
            File *f = i >= 0 ? im->files[i] : NULL;
            if (f && rel < f->data_off + f->size) {
                uint64_t in = rel - f->data_off, avail = f->size - in;
                if (n > avail) n = avail;
                int64_t r = read_file(f, dst + done, in, n);
                if (r < 0) r = 0;
                if ((uint64_t)r < n) memset(dst + done + r, 0, n - r);
            } else {  // padding up to the next file
                uint64_t next = i + 1 < im->nfiles ? im->files[i + 1]->data_off : im->tables_off - DATA_START;
                if (n > next - rel) n = next - rel;
                memset(dst + done, 0, n);
            }
        }
        done += n;
    }
    return done;
}

uint64_t romfs_read(uint8_t *dst, uint64_t off, uint64_t size) { return romfs_image_read(&g_main, dst, off, size); }
