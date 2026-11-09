// BC1-BC7 decoded on the CPU, for GPUs without BC texture support (phones: nvn/vulkan/textures.c, as astc.c does for
// ASTC). third_party/bcdec.h (MIT / Unlicense) decodes the blocks.
#include "bcn.h"

#include "../platform.h"
#include "../rt.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <SDL3/SDL_cpuinfo.h>  // (no sysconf)
#endif

#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#include "../../third_party/bcdec.h"

uint32_t bcn_block_bytes(int kind) {
    return kind == BCN_BC1 || kind == BCN_BC4U || kind == BCN_BC4S ? 8 : 16;
}

uint32_t bcn_texel_bytes(int kind) { return kind == BCN_BC6HS || kind == BCN_BC6HU ? 8 : 4; }

// One block into 4x4 texels of the output format (RGBA8, or RGBA16F for BC6H), rows `pitch` bytes apart.
static void block(int kind, const uint8_t *b, uint8_t *o, int pitch, uint32_t tb) {
    switch (kind) {
    case BCN_BC1: bcdec_bc1(b, o, pitch); break;
    case BCN_BC2: bcdec_bc2(b, o, pitch); break;
    case BCN_BC3: bcdec_bc3(b, o, pitch); break;
    case BCN_BC7: bcdec_bc7(b, o, pitch); break;
    case BCN_BC6HS: case BCN_BC6HU: {  // RGB halfs (the pitch in halfs) -> RGBA16F with A = 1.0
        uint16_t rgb[48];
        bcdec_bc6h_half(b, rgb, 12, kind == BCN_BC6HS);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                uint16_t *p = (uint16_t *)(void *)(o + y * pitch + x * 8);
                p[0] = rgb[y * 12 + x * 3], p[1] = rgb[y * 12 + x * 3 + 1], p[2] = rgb[y * 12 + x * 3 + 2], p[3] = 0x3C00;
            }
        break;
    }
    case BCN_BC4U: case BCN_BC4S: {  // one channel: R, then G = B = 0, A = 1 (what sampling R8 returns)
        uint8_t r[16];
        bcdec_bc4(b, r, 4, kind == BCN_BC4S);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                uint8_t *p = o + y * pitch + x * tb;
                p[0] = r[y * 4 + x];
                if (tb == 4) p[1] = 0, p[2] = 0, p[3] = kind == BCN_BC4S ? 127 : 255;
            }
        break;
    }
    case BCN_BC5U: case BCN_BC5S: {
        uint8_t rg[32];
        bcdec_bc5(b, rg, 8, kind == BCN_BC5S);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                uint8_t *p = o + y * pitch + x * tb;
                p[0] = rg[y * 8 + x * 2], p[1] = rg[y * 8 + x * 2 + 1];
                if (tb == 4) p[2] = 0, p[3] = kind == BCN_BC5S ? 127 : 255;
            }
        break;
    }
    default: break;
    }
}

// A large texture is decoded on several cores: this runs on the render thread, and on a phone a few big textures in a
// row are a hitch in the frame. Whoever is free takes the next rows, so a slow core does not hold the others up.
#define BCN_WORKERS 3         // (besides the caller)
#define BCN_ROWS 8            // block rows taken at a time
#define BCN_BIG_BLOCKS 4096u  // (256 x 256 texels and up: below that, starting the others costs more than it saves)

typedef struct {
    int kind;
    const uint8_t *blocks;
    uint8_t *pad;
    uint32_t bw, bh, bb, pitch, tb;
    atomic_uint next;
} BcnJob;

static void bcn_rows(BcnJob *j) {
    for (;;) {
        uint32_t by = atomic_fetch_add_explicit(&j->next, BCN_ROWS, memory_order_relaxed);
        if (by >= j->bh) return;
        uint32_t end = by + BCN_ROWS < j->bh ? by + BCN_ROWS : j->bh;
        for (; by < end; by++)
            for (uint32_t bx = 0; bx < j->bw; bx++)
                block(j->kind, j->blocks + ((size_t)by * j->bw + bx) * j->bb,
                      j->pad + (size_t)by * 4 * j->pitch + (size_t)bx * 4 * j->tb, (int)j->pitch, j->tb);
    }
}

static struct {
    pthread_mutex_t one, mu;  // (one: a second caller decodes on its own)
    pthread_cond_t go, done;
    BcnJob *job;
    uint64_t seq;
    int busy, started;
} P = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER};

static void *bcn_worker(void *unused) {
    (void)unused;
    uint64_t seen = 0;
    pthread_mutex_lock(&P.mu);
    for (;;) {
        while (P.seq == seen) pthread_cond_wait(&P.go, &P.mu);
        seen = P.seq;
        BcnJob *j = P.job;
        if (!j) continue;  // (over before this one woke)
        P.busy++;
        pthread_mutex_unlock(&P.mu);
        bcn_rows(j);
        pthread_mutex_lock(&P.mu);
        if (!--P.busy) pthread_cond_signal(&P.done);
    }
    return NULL;
}

static void bcn_workers_start(void) {
    if (P.started) return;
#ifdef _WIN32
    long cores = SDL_GetNumLogicalCPUCores();
#else
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    int n = cores > BCN_WORKERS + 1 ? BCN_WORKERS : cores > 1 ? (int)cores - 1 : 0;
    P.started = n > 0 ? n : -1;
    for (int i = 0; i < n; i++) {
        pthread_t th;
        if (pthread_create(&th, NULL, bcn_worker, NULL)) { if (!i) P.started = -1; break; }
        pthread_detach(th);
    }
}

void bcn_decode(int kind, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *out, uint32_t tb) {
    uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4, bb = bcn_block_bytes(kind);
    // whole blocks into a padded image, then the visible part into out (width x height, tightly packed)
    uint32_t pw = bw * 4, pitch = pw * tb;
    uint8_t *pad = malloc((size_t)pitch * bh * 4);
    if (!pad) return;
    BcnJob job = {.kind = kind, .blocks = blocks, .pad = pad, .bw = bw, .bh = bh, .bb = bb, .pitch = pitch, .tb = tb};
    atomic_init(&job.next, 0);
    static int log = -1, serial;  // (tests: MHGU_BCN_LOG=1 the time big ones take, MHGU_BCN_SERIAL=1 on one core)
    if (log < 0) log = getenv("MHGU_BCN_LOG") != NULL, serial = getenv("MHGU_BCN_SERIAL") != NULL;
    uint64_t t0 = log ? plat_now_ns() : 0;
    if (bw * bh >= BCN_BIG_BLOCKS && !serial && !pthread_mutex_trylock(&P.one)) {
        bcn_workers_start();
        if (P.started > 0) {
            pthread_mutex_lock(&P.mu);
            P.job = &job, P.seq++;
            pthread_cond_broadcast(&P.go);
            pthread_mutex_unlock(&P.mu);
        }
        bcn_rows(&job);
        if (P.started > 0) {
            pthread_mutex_lock(&P.mu);
            P.job = NULL;  // (no one else joins, and the job is this stack's: wait for those in it)
            while (P.busy) pthread_cond_wait(&P.done, &P.mu);
            pthread_mutex_unlock(&P.mu);
        }
        pthread_mutex_unlock(&P.one);
    } else {
        bcn_rows(&job);
    }
    if (log && bw * bh >= BCN_BIG_BLOCKS) {
        static uint64_t total, worst;
        static int count;
        uint64_t ns = plat_now_ns() - t0;
        total += ns, worst = ns > worst ? ns : worst;
        if (++count % 50 == 0) LOG("[bcn] %d big textures: %.1f ms in all, the worst %.1f ms\n", count, total / 1e6, worst / 1e6);
    }
    for (uint32_t y = 0; y < height; y++) memcpy(out + (size_t)y * width * tb, pad + (size_t)y * pitch, (size_t)width * tb);
    free(pad);
}
