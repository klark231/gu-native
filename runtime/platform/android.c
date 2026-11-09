// Android host layer (runtime/platform.h): linux.c adapted to bionic on arm64.
//
// - The app's files live in MHGU_HOME (the activity sets it: the app's external files folder); plat_exe_path
//   answers a path inside it, so everything the runtime keeps "beside the executable" (data/, mods/, cache/,
//   pipelines.list, log.txt) lands there. The game code library comes from MHGU_GAME_LIB (the APK's lib folder).
// - Faults: arm64's ucontext (pc, sp, x30; the ESR record tells writes from reads).
// - No execinfo: backtraces through the unwinder. The log is also mirrored to logcat (tag MHGU).
//
// The guest window is reserved PROT_NONE up front and pages are backed on their first touch by the SIGSEGV handler
// below (64 KB at a time), exactly as windows.c does with MEM_RESERVE + MEM_COMMIT.
//
// Differences from Windows worth knowing:
// - No GetWriteWatch: plat_written_pages() makes the pages it is asked about read-only and the fault handler notes the
//   writes ("page write tracking" below). The kernel can't take those faults: a read() into a watched page fails with
//   EFAULT, so file reads into guest memory go through plat_pread and host writes are announced with plat_commit.
// - Signal handlers are process-wide, so one SIGSEGV handler does both jobs: back a guest page, or report the fault.
//   It is installed by plat_reserve_guest(), i.e. from main.c's constructor, before any guest code can run.
// - No console to keep or free: with stderr not a terminal nobody is watching, so the log goes to log.txt.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <android/api-level.h>
#include <android/log.h>
#include <jni.h>
#include <sys/system_properties.h>
#include <SDL3/SDL_messagebox.h>
#include "../settings_locale.h"
#include <unwind.h>
#include <pthread.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <asm/sigcontext.h>
#include <time.h>
#include <unistd.h>

#include "../platform.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// ---- guest address space
static uint64_t g_lo, g_hi;
#define COMMIT_CHUNK 0x10000ull
#define MAX_CHUNKS ((0x100000000ull + COMMIT_CHUNK - 1) / COMMIT_CHUNK)

// Per-chunk state, so the fault handler can tell "not backed yet" (back it and resume) from a real fault in backed
// memory (report it) without a lock: a signal handler may not take one, and several guest threads faulting into the
// same fresh chunk is the normal case. 0 not backed, 1 somebody is backing it, 2 backed.
enum { CHUNK_FREE = 0, CHUNK_BUSY, CHUNK_READY };
static _Atomic unsigned char g_chunk[MAX_CHUNKS];

static bool chunk_back(uint64_t base) {
    void *p = mmap((void *)(uintptr_t)base, COMMIT_CHUNK, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    return p == (void *)(uintptr_t)base;
}

// The faulting address is inside the window and its chunk is not backed: back it. True = resume the instruction.
static bool lazy_commit(uint64_t a) {
    if (a < g_lo || a >= g_hi) return false;
    uint64_t idx = (a - g_lo) / COMMIT_CHUNK;
    if (idx >= MAX_CHUNKS) return false;
    unsigned char expect = CHUNK_FREE;
    if (atomic_compare_exchange_strong(&g_chunk[idx], &expect, CHUNK_BUSY)) {
        bool ok = chunk_back(g_lo + idx * COMMIT_CHUNK);
        atomic_store(&g_chunk[idx], ok ? CHUNK_READY : CHUNK_FREE);
        return ok;
    }
    while (atomic_load(&g_chunk[idx]) == CHUNK_BUSY) atomic_thread_fence(memory_order_acquire);
    return atomic_load(&g_chunk[idx]) == CHUNK_READY;  // (whoever won it backed the page: resume)
}

// ---- page write tracking (plat_written_pages: the vertex cache and the texture checks in runtime/nvn/vulkan)
// Windows has GetWriteWatch. Here a query makes the blocks it reports read-only ("armed"); the first write to one faults
// into fault_handler, which makes it writable again and marks it written, and the next query over it reports it and arms
// it again. Blocks of 16 KB rather than 4 KB pages: a quarter of the faults, mprotect calls and map entries, and the same
// on a 16 KB-page kernel. Still reported as 4 KB pages (textures.c wants that granularity); a block a query covers only
// in part keeps its other pages' news for the queries that cover them (wt_pend), as GetWriteWatch resets only the range
// asked about.
//   untracked  never asked about (writable): all written, for all anyone knows
//   armed      read-only, not written since it was last reported
//   dirty      written since (writable again): reported, then armed again
//   hot        dirty at query after query (a dynamic buffer, a busy variable beside a vertex buffer): left writable and
//              reported every time, so its writers don't fault every frame; watched again after about a second
//              (staggered by block, a few dozen per query: not a burst of faults), twice as long each time it is hot
//              again straight away (up to ~8 s)
//   busy       changing: only the thread that made it busy changes its protection (a query arming it; a fault or a
//              host write making it writable)
//   off        plat_protect owns its protection: always reported, never armed
// Only blocks in backed chunks are armed (lazy_commit maps a chunk read-write over whatever was there), and only in the
// ranges the renderer asks about (heap textures and vertex buffers: not the stacks, TLS or the HLE heap). An armed
// block no query has covered for ~10 s (a buffer gone, a texture no longer drawn) is unwatched: writable, untracked.
// The kernel doesn't take these faults: a read() into an armed page fails with EFAULT (or stops short). File reads into
// guest memory go through plat_pread, and plat_commit disarms the buffers the host writes next (IPC buffers).
// Each read-only run can cost two of the process's vm.max_map_count entries (it splits a mapping): arming stops at a
// budget (the runs as last counted, + those armed and split since), and if mprotect runs out anyway (ENOMEM) where a
// write needs its page the tracking ends: every backed chunk writable again, every query "all written" (as without
// tracking: the renderer compares contents).
// MHGU_NO_WRITE_WATCH (any value): off. MHGU_WT_STATS or MHGU_VK_STATS: the counts in the log every 2 s.
#define WT_BLOCK 0x4000ull
#define WT_PAGES ((unsigned)(WT_BLOCK / PLAT_PAGE))
#define WT_ALL ((1u << WT_PAGES) - 1)
#define WT_BLOCKS (0x100000000ull / WT_BLOCK)
#define WT_STRIKES 4      // dirty at this many queries in a row, each within WT_COOL of the last: hot
#define WT_COOL 48        // in ticks of ~2 ms (plat_now_ns >> 21): ~100 ms
#define WT_REARM 512      // ticks a hot block stays unwatched: ~1 s, + 2 per (block number & 255) (staggered: up to ~1 s
                          // more), doubled each time it is hot again straight after a re-arm (WT_AGAIN)...
#define WT_BACKOFF 3      // ...this many times at most (~8.6 s)
#define WT_AGAIN 0x80     // (wt_back: re-armed from hot, not cool since)
#define WT_REARM_MAX 64   // hot blocks re-armed by one query at most (the others at the next ones)
#define WT_IDLE 10        // in ~1.07 s (plat_now_ns >> 30): an armed block no query has covered for longer is unwatched
enum { WT_UNTRACKED = 0, WT_ARMED, WT_DIRTY, WT_HOT, WT_BUSY, WT_OFF };
static _Atomic unsigned char wt_state[WT_BLOCKS];
static _Atomic unsigned char wt_pend[WT_BLOCKS];  // armed: its pages written before it was armed, not reported yet
static unsigned char wt_strikes[WT_BLOCKS];       // (these four, the run counts and the bounds: queries only - the
static uint16_t wt_time[WT_BLOCKS];               //  renderer, under its lock)
static unsigned char wt_back[WT_BLOCKS];          // (hot: the re-arm period's doublings, | WT_AGAIN)
static unsigned char wt_seen[WT_BLOCKS];          // (the last query over it, in ~1.07 s: WT_IDLE)
static _Atomic int g_wt = -1;                     // 1 on, 0 off, -1 not decided yet (the first query decides)
static _Atomic int g_wt_full, g_wt_dead;          // full: arming failed (out of map entries), none since; dead: ended
static int wt_runs, wt_max_runs;                  // read-only runs (as last counted, + those armed since) / budget
static _Atomic int wt_splits;                     // armed blocks made writable since that count (each may split a run)
static uint64_t wt_lo_b = ~0ull, wt_hi_b;         // blocks ever queried: [lo, hi)
static _Atomic uint64_t wt_n_query, wt_n_armed, wt_n_fault, wt_n_host, wt_n_efault, wt_n_idle;

static void wt_spin(int i) {
    if (i > 64) sched_yield();
    else atomic_thread_fence(memory_order_acquire);
}

// Out of map entries where a write needed its block writable: every backed chunk read-write again (that only joins
// mappings: it can't run out itself), and from now on every query answers "all written". Signal-safe.
static void wt_give_up(void) {
    atomic_store(&g_wt_dead, 1);
    uint64_t n = (g_hi - g_lo) / COMMIT_CHUNK;
    if (n > MAX_CHUNKS) n = MAX_CHUNKS;
    for (uint64_t c = 0; c < n;) {
        uint64_t e = c;
        while (e < n && atomic_load(&g_chunk[e]) == CHUNK_READY) e++;
        if (e > c) mprotect((void *)(uintptr_t)(g_lo + c * COMMIT_CHUNK), (e - c) * COMMIT_CHUNK, PROT_READ | PROT_WRITE);
        c = e + 1;
    }
}

// Blocks [rb, re) read-write, one mprotect (the caller has them busy). Signal-safe.
static void wt_rw(uint64_t rb, uint64_t re) {
    void *p = (void *)(uintptr_t)(g_lo + rb * WT_BLOCK);
    if (mprotect(p, (re - rb) * WT_BLOCK, PROT_READ | PROT_WRITE) != 0) {  // (ENOMEM: splitting the mapping needs an entry)
        wt_give_up();
        mprotect(p, (re - rb) * WT_BLOCK, PROT_READ | PROT_WRITE);
    }
}

// Block b writable again. 1: it was armed (now dirty: a write is on its way); 2 (heal): it was dirty or hot and is made
// writable all the same - a fault there raced whoever disarmed it, or hit a run whose arming failed halfway (mprotect
// may change part of a range before it fails); 0: none of those, not ours. Signal-safe: atomics, mprotect, sched_yield.
static int wt_unprotect(uint64_t b, bool heal) {
    for (int i = 0;; i++) {
        unsigned char s = atomic_load(&wt_state[b]);
        if (s == WT_BUSY) { wt_spin(i); continue; }
        if (s != WT_ARMED && (!heal || (s != WT_DIRTY && s != WT_HOT))) return 0;
        if (!atomic_compare_exchange_weak(&wt_state[b], &s, WT_BUSY)) continue;
        wt_rw(b, b + 1);
        if (s == WT_ARMED) atomic_fetch_add_explicit(&wt_splits, 1, memory_order_relaxed);  // (its run split, maybe)
        atomic_store(&wt_state[b], s == WT_ARMED ? WT_DIRTY : s);
        return s == WT_ARMED ? 1 : 2;
    }
}

static bool wt_backed(uint64_t off) {  // (off: from g_lo, inside the window) its chunk is backed
    return atomic_load(&g_chunk[off / COMMIT_CHUNK]) == CHUNK_READY;
}

// A SIGSEGV at host address a: a write to a block a query armed? True: it is writable now, resume the instruction.
static bool wt_fault(uint64_t a) {
    if (atomic_load_explicit(&g_wt, memory_order_relaxed) <= 0 || a < g_lo || a >= g_hi) return false;
    uint64_t off = a - g_lo, b = off / WT_BLOCK, next = (b + 1) * WT_BLOCK;
    // (a chunk not backed yet is lazy_commit's: none of it is armed, and a heal of the next block below would resume a
    // fault only backing this chunk ends, forever)
    if (!wt_backed(off)) return false;
    int r = wt_unprotect(b, true), armed = r == 1;
    // (an access across a block's end may be reported at its first byte: the read-only part is then the next block -
    // if its chunk is backed, else nothing of it is armed)
    if (off % WT_BLOCK >= WT_BLOCK - 64 && g_lo + next < g_hi && wt_backed(next)) {
        int r2 = wt_unprotect(b + 1, true);
        armed += r2 == 1;
        r |= r2;
    }
    if (armed) atomic_fetch_add_explicit(&wt_n_fault, (uint64_t)armed, memory_order_relaxed);
    return r != 0;
}

// The blocks host [addr, addr+size) touches inside the window: [*b0, *b1).
static bool wt_range(uint64_t addr, uint64_t size, uint64_t *b0, uint64_t *b1) {
    uint64_t end = addr + size;
    if (!size || end <= g_lo || addr >= g_hi) return false;
    if (addr < g_lo) addr = g_lo;
    if (end > g_hi) end = g_hi;
    *b0 = (addr - g_lo) / WT_BLOCK;
    *b1 = (end - g_lo + WT_BLOCK - 1) / WT_BLOCK;
    return true;
}

// Blocks [b0, b1): the armed ones writable again - claimed busy, then one mprotect per run of them, as arming does - and
// then `to`; the others `to` as well if all, else left as they are. The number that were armed.
static uint64_t wt_open(uint64_t b0, uint64_t b1, unsigned char to, bool all) {
    uint64_t rb = b0, re = b0, n = 0;  // claimed, still read-only: [rb, re)
    for (uint64_t b = b0; b <= b1; b++) {
        bool claim = false;
        for (int i = 0; b < b1; i++) {  // (busy: another thread's for a moment; claims go up, so none waits on ours)
            unsigned char s = atomic_load(&wt_state[b]);
            if (s == WT_BUSY) { wt_spin(i); continue; }
            if (s != WT_ARMED && (!all || s == to)) break;
            if (atomic_compare_exchange_weak(&wt_state[b], &s, s == WT_ARMED ? WT_BUSY : to)) { claim = s == WT_ARMED; break; }
        }
        if (claim) {
            if (re == rb) rb = b;
            re = b + 1;
        } else if (re > rb) {
            wt_rw(rb, re);
            atomic_fetch_add_explicit(&wt_splits, 1, memory_order_relaxed);  // (a run split, maybe)
            for (uint64_t k = rb; k < re; k++) atomic_store(&wt_state[k], to);
            n += re - rb;
            rb = re;
        }
    }
    return n;
}

// The host writes [addr, addr+size) next, maybe through the kernel (which can't take the fault): the armed blocks there
// writable again, as written.
static void wt_disarm(uint64_t addr, uint64_t size) {
    uint64_t b0, b1;
    if (atomic_load(&g_wt) <= 0 || !wt_range(addr, size, &b0, &b1)) return;
    uint64_t n = wt_open(b0, b1, WT_DIRTY, false);
    if (n) atomic_fetch_add_explicit(&wt_n_host, n, memory_order_relaxed);
}

// plat_protect: those blocks' protection is the caller's now. Armed ones writable first: the caller may change only
// part of a block, and a write to the rest of it would fault with nobody to take it.
static void wt_forget(uint64_t addr, uint64_t size) {
    uint64_t b0, b1;
    if (wt_range(addr, size, &b0, &b1)) wt_open(b0, b1, WT_OFF, true);
}

static unsigned wt_in(uint64_t base, uint64_t lo, uint64_t hi) {  // the pages of the block at base inside [lo, hi)
    unsigned m = 0;
    for (unsigned p = 0; p < WT_PAGES; p++)
        if (base + p * PLAT_PAGE >= lo && base + (p + 1) * PLAT_PAGE <= hi) m |= 1u << p;
    return m;
}

// A query found block b dirty. True: it has been at query after query, and is hot now (left writable).
static bool wt_heat(uint64_t b, uint16_t t) {
    uint16_t d = (uint16_t)(t - wt_time[b]);
    if (!wt_strikes[b] || d > WT_COOL) wt_strikes[b] = 1, wt_time[b] = t, wt_back[b] = 0;  // (the first write in a while)
    else if (d >= 2 && wt_strikes[b] < WT_STRIKES) wt_strikes[b]++, wt_time[b] = t;  // (queries within ~4 ms: once)
    unsigned char e = WT_DIRTY;
    if (wt_strikes[b] < WT_STRIKES || !atomic_compare_exchange_strong(&wt_state[b], &e, WT_HOT)) return false;
    unsigned char k = wt_back[b] & ~WT_AGAIN;  // (hot again straight after its re-arm: left alone twice as long)
    wt_back[b] = (unsigned char)((wt_back[b] & WT_AGAIN) && k < WT_BACKOFF ? k + 1 : k);
    return true;
}

static unsigned wt_rearm_ticks(uint64_t b) {  // how long hot block b stays unwatched (staggered: not all at once)
    return ((unsigned)WT_REARM << (wt_back[b] & ~WT_AGAIN)) + (unsigned)(b & 255) * 2;
}

// Blocks [rb, re), busy (claimed by a query over host [lo, hi)): read-only together, armed. Their pages outside the query
// stay pending - written, for all anyone knows - until a query over them reports them.
static void wt_arm(uint64_t rb, uint64_t re, uint64_t lo, uint64_t hi) {
    bool dead = atomic_load(&g_wt_dead);
    bool ok = !dead && mprotect((void *)(uintptr_t)(g_lo + rb * WT_BLOCK), (re - rb) * WT_BLOCK, PROT_READ) == 0;
    int err = errno;  // (logged below, once the blocks are no longer busy: a fault on one waits for that)
    for (uint64_t b = rb; b < re; b++) {
        if (ok) atomic_store(&wt_pend[b], (unsigned char)(WT_ALL & ~wt_in(g_lo + b * WT_BLOCK, lo, hi)));
        atomic_store(&wt_state[b], ok ? WT_ARMED : WT_DIRTY);  // (failed: any part made read-only anyway heals on a fault)
    }
    if (ok) {
        wt_runs++;
        atomic_fetch_add_explicit(&wt_n_armed, re - rb, memory_order_relaxed);
    } else if (!dead && !atomic_exchange(&g_wt_full, 1)) {  // (ENOMEM: out of map entries; anything else: no use retrying)
        fprintf(stderr, "[plat] write tracking: mprotect failed (%s) at ~%d read-only runs: no more blocks watched\n",
                strerror(err), wt_runs);
    }
}

static void wt_init(void) {
    int on = 0, mmc = 65530;
    FILE *f = fopen("/proc/sys/vm/max_map_count", "r");
    if (f) {
        if (fscanf(f, "%d", &mmc) != 1) mmc = 65530;
        fclose(f);
    }
    // (two map entries per read-only run at worst; 25000 left for the rest: the GPU driver maps every allocation)
    wt_max_runs = (mmc - 25000) / 2;
    long pg = sysconf(_SC_PAGESIZE);
    if (getenv("MHGU_NO_WRITE_WATCH"))
        fprintf(stderr, "[plat] page write tracking off (MHGU_NO_WRITE_WATCH)\n");
    else if (pg <= 0 || (uint64_t)pg > WT_BLOCK || !g_lo || (g_lo & (WT_BLOCK - 1)) || g_hi - g_lo > WT_BLOCKS * WT_BLOCK)
        fprintf(stderr, "[plat] page write tracking off (page size %ld)\n", pg);
    else if (wt_max_runs < 1024)
        fprintf(stderr, "[plat] page write tracking off (vm.max_map_count %d)\n", mmc);
    else {
        on = 1;
        fprintf(stderr, "[plat] page write tracking on: 16 KB blocks, up to %d read-only runs (vm.max_map_count %d)\n",
                wt_max_runs, mmc);
    }
    atomic_store(&g_wt, on);
}

static void wt_unwatch(uint64_t rb, uint64_t re) {  // (busy, armed, idle: writable and untracked again)
    wt_rw(rb, re);
    for (uint64_t b = rb; b < re; b++) atomic_store(&wt_state[b], WT_UNTRACKED);
    atomic_fetch_add_explicit(&wt_n_idle, re - rb, memory_order_relaxed);
}

// Now and then (from a query): the armed blocks no query has covered for WT_IDLE unwatched, the read-only runs counted
// afresh (faults split them, arming joins them; in between each new run and each split counts one), and with
// MHGU_WT_STATS / MHGU_VK_STATS the counts in the log every 2 s.
static void wt_tick(uint64_t now) {
    static uint64_t next, next_log;
    static int stats = -1, said_dead;
    if (now < next) return;
    next = now + 250000000ull;
    if (stats < 0) stats = getenv("MHGU_WT_STATS") || getenv("MHGU_VK_STATS");
    atomic_store(&wt_splits, 0);  // (before the count: a split while it runs is counted twice at worst, never missed)
    unsigned char g = (unsigned char)(now >> 30);
    int runs = 0, armed = 0, hot = 0, dirty = 0;
    bool ro = false;
    uint64_t ub = 0, ue = 0;  // idle armed blocks claimed (busy) to be unwatched: [ub, ue)
    for (uint64_t b = wt_lo_b; b < wt_hi_b; b++) {
        unsigned char s = atomic_load_explicit(&wt_state[b], memory_order_relaxed), e = WT_ARMED;
        if (s == WT_ARMED && (unsigned char)(g - wt_seen[b]) > WT_IDLE &&
            atomic_compare_exchange_strong(&wt_state[b], &e, WT_BUSY)) {
            if (ue == ub) ub = b;
            ue = b + 1;
            s = WT_UNTRACKED;  // (counted as it will be)
        } else if (ue > ub) {
            wt_unwatch(ub, ue);
            ub = ue;
        }
        bool r = s == WT_ARMED || s == WT_BUSY;
        runs += r && !ro;
        ro = r;
        armed += s == WT_ARMED;
        hot += s == WT_HOT;
        dirty += s == WT_DIRTY;
    }
    if (ue > ub) wt_unwatch(ub, ue);
    wt_runs = runs;
    if (atomic_load(&g_wt_dead) && !said_dead) {
        said_dead = 1;
        fprintf(stderr, "[plat] write tracking: out of map entries (vm.max_map_count) where a write needed its page: "
                        "stopped, every page counts as written from now on\n");
    }
    if (!stats || now < next_log) return;
    next_log = now + 2000000000ull;
    fprintf(stderr, "[plat] 2s write tracking: %llu queries, %llu blocks armed, %llu write faults, %llu host writes, "
                    "%llu unwatched (idle), %llu EFAULT retries | %d armed in %d runs (budget %d), %d hot, %d dirty%s%s\n",
            (unsigned long long)atomic_exchange(&wt_n_query, 0), (unsigned long long)atomic_exchange(&wt_n_armed, 0),
            (unsigned long long)atomic_exchange(&wt_n_fault, 0), (unsigned long long)atomic_exchange(&wt_n_host, 0),
            (unsigned long long)atomic_exchange(&wt_n_idle, 0), (unsigned long long)atomic_exchange(&wt_n_efault, 0),
            armed, runs, wt_max_runs, hot, dirty,
            atomic_load(&g_wt_full) ? ", arming stopped (mprotect failed)" : "",
            atomic_load(&g_wt_dead) ? ", STOPPED" : "");
}

void (*plat_touch_hook)(uint64_t guest_addr, int write);
static void fault_handler(int sig, siginfo_t *si, void *uc);

bool plat_reserve_guest(uint64_t base, uint64_t size) {
    void *p = mmap((void *)(uintptr_t)base, size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (p != (void *)(uintptr_t)base) return false;
    g_lo = base;
    g_hi = base + size;
    struct sigaction sa = {0};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    return true;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t a = addr & ~(COMMIT_CHUNK - 1), e = (addr + size + COMMIT_CHUNK - 1) & ~(COMMIT_CHUNK - 1);
    for (uint64_t c = a; c < e && c < g_hi; c += COMMIT_CHUNK) lazy_commit(c);
    if (e > g_hi)
        fprintf(stderr, "[plat] commit %llx+%llx past the guest window\n", (unsigned long long)addr,
                (unsigned long long)size);
    wt_disarm(addr, size);  // (the host writes there next, maybe through the kernel: no read-only pages)
}

// Reports what GetWriteWatch would, in 4 KB pages, and arms what it reports (see "page write tracking" above).
int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, wt_init);
    if (atomic_load(&g_wt) <= 0 || addr < g_lo || addr > g_hi || size > g_hi - addr || max <= 0) return -1;
    *page = PLAT_PAGE;
    uint64_t now = plat_now_ns();
    wt_tick(now);
    if (!size) return 0;
    uint64_t lo = addr & ~(PLAT_PAGE - 1), hi = (addr + size + PLAT_PAGE - 1) & ~(PLAT_PAGE - 1);
    int n = 0;
    if (atomic_load(&g_wt_dead)) {  // (the tracking ended: all of it written)
        for (uint64_t p = lo; p < hi && n < max; p += PLAT_PAGE) pages[n++] = p;
        return n;
    }
    atomic_fetch_add_explicit(&wt_n_query, 1, memory_order_relaxed);
    uint16_t t = (uint16_t)(now >> 21);
    unsigned char g = (unsigned char)(now >> 30);
    uint64_t b0 = (lo - g_lo) / WT_BLOCK, b1 = (hi - g_lo + WT_BLOCK - 1) / WT_BLOCK;
    if (b0 < wt_lo_b) wt_lo_b = b0;
    if (b1 > wt_hi_b) wt_hi_b = b1;
    uint64_t rb = 0, re = 0;  // blocks claimed for arming (busy) and not read-only yet: [rb, re), one mprotect
    int rearmed = 0;          // hot blocks re-armed (WT_REARM_MAX)
    bool over = false;        // arming held back by the map budget
    for (uint64_t b = b0; b < b1; b++) {
        uint64_t base = g_lo + b * WT_BLOCK;
        unsigned in = base >= lo && base + WT_BLOCK <= hi ? WT_ALL : wt_in(base, lo, hi), rep = 0;
        unsigned char s = WT_UNTRACKED;
        bool arm = false;
        if (atomic_load(&g_chunk[(base - g_lo) / COMMIT_CHUNK]) == CHUNK_READY) {  // (else never backed: zeros, unwritten)
            wt_seen[b] = g;
            s = atomic_load(&wt_state[b]);
            switch (s) {
            case WT_ARMED:  // (only what was written before it was armed and no query has reported yet)
                rep = atomic_load(&wt_pend[b]) & in;
                if (rep) atomic_fetch_and(&wt_pend[b], (unsigned char)~rep);
                break;
            case WT_UNTRACKED:
                rep = in, arm = true;
                wt_strikes[b] = 0, wt_time[b] = t, wt_back[b] = 0;
                break;
            case WT_DIRTY: rep = in, arm = !wt_heat(b, t); break;
            case WT_HOT:  // (its time is up: watched again, and hot again at its next dirty query)
                rep = in;
                arm = (uint16_t)(t - wt_time[b]) >= wt_rearm_ticks(b) && rearmed < WT_REARM_MAX;
                break;
            default: rep = in; break;  // (busy, off: written, for all anyone knows)
            }
        }
        for (unsigned p = 0; p < WT_PAGES; p++)  // (a full array: the caller takes the whole range as written)
            if (rep >> p & 1 && n < max) pages[n++] = base + p * PLAT_PAGE;
        bool claim = false;
        if (arm && !atomic_load(&g_wt_full)) {
            if (re > rb || wt_runs + atomic_load_explicit(&wt_splits, memory_order_relaxed) < wt_max_runs) {
                unsigned char e = s;
                claim = atomic_compare_exchange_strong(&wt_state[b], &e, WT_BUSY);
            } else {
                over = true;
            }
        }
        if (claim && s == WT_HOT) rearmed++, wt_strikes[b] = WT_STRIKES - 1, wt_time[b] = t, wt_back[b] |= WT_AGAIN;
        if (claim) {
            if (re == rb) rb = b;
            re = b + 1;
        } else if (re > rb) {
            wt_arm(rb, re, lo, hi);
            rb = re;
        }
    }
    if (re > rb) wt_arm(rb, re, lo, hi);
    static bool said_over;
    if (over && !said_over) {  // (here: no block busy)
        said_over = true;
        fprintf(stderr, "[plat] write tracking: budget of %d read-only runs (vm.max_map_count) reached: more blocks are "
                        "armed only as others are written or go idle\n", wt_max_runs);
    }
    return n;
}

// pread for buffers that may be guest memory: an armed page there makes the kernel fail (EFAULT) or stop short where a
// store would just fault, so the range is disarmed first and again on EFAULT (a query may arm it meanwhile), and a short
// read goes on (only the end of the file stops it). Refused again and again: through a buffer of ours, whose copy into
// guest memory faults like any store - only into guest memory: anywhere else EFAULT is a bad pointer, the caller's.
ssize_t plat_pread(int fd, void *buf, size_t n, off_t off) {
    uint64_t a = (uint64_t)(uintptr_t)buf;
    // (not decided yet, -1: the careful way, the first query may be arming right now)
    if (!atomic_load(&g_wt) || a >= g_hi || a + n <= g_lo) return pread(fd, buf, n, off);
    size_t done = 0;
    for (int refused = 0; done < n;) {
        wt_disarm(a + done, n - done);
        ssize_t r = pread(fd, (char *)buf + done, n - done, off + (off_t)done);
        if (r > 0) { done += (size_t)r; refused = 0; continue; }
        if (r == 0) break;
        if (errno == EINTR) continue;
        if (errno != EFAULT) return done ? (ssize_t)done : -1;
        atomic_fetch_add_explicit(&wt_n_efault, 1, memory_order_relaxed);
        if (++refused < 8) continue;
        char tmp[4096];
        uint64_t d = a + done;
        size_t len = n - done < sizeof tmp ? n - done : sizeof tmp;
        if (d < g_lo || d >= g_hi) { errno = EFAULT; return done ? (ssize_t)done : -1; }
        if (len > g_hi - d) len = (size_t)(g_hi - d);  // (up to the window's end: past it, not ours)
        r = pread(fd, tmp, len, off + (off_t)done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return done ? (ssize_t)done : r;
        memcpy((char *)buf + done, tmp, (size_t)r);
        done += (size_t)r;
        refused = 0;
    }
    return (ssize_t)done;
}

bool plat_committed(uint64_t addr, uint64_t *end) {
    if (!g_lo || addr < g_lo || addr >= g_hi) { *end = addr + PLAT_PAGE; return false; }
    uint64_t idx = (addr - g_lo) / COMMIT_CHUNK;
    if (atomic_load(&g_chunk[idx]) != CHUNK_READY) {
        *end = g_lo + (idx + 1) * COMMIT_CHUNK;
        return false;
    }
    uint64_t n = idx;
    while (n + 1 < MAX_CHUNKS && n - idx < 4096 && atomic_load(&g_chunk[n + 1]) == CHUNK_READY) n++;
    *end = g_lo + (n + 1) * COMMIT_CHUNK;
    return true;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    if (addr >= g_lo && addr < g_hi) {
        plat_commit(addr, size);
        wt_forget(addr, size);
    }
    return mprotect((void *)(uintptr_t)addr, size, p) == 0 ? 0 : -1;
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    // A 1 us timer slack (50 us by default) ends the sleep close to t, so the spin at the end can be short: spinning
    // keeps a big core awake, which costs battery and thermal headroom on a phone.
    static __thread bool slack_set;
    if (!slack_set) { prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0); slack_set = true; }
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 50000) { sched_yield(); continue; }  // (the last bit: spinning beats a syscall)
        // Asleep until 50 us before t (an absolute time on plat_now_ns's clock). (It was given the time left as if it
        // were absolute: long past, so it returned at once and every sleep was a busy loop of syscalls.)
        uint64_t wake = t - 50000;
        struct timespec due = {(time_t)(wake / 1000000000ull), (long)(wake % 1000000000ull)};
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, NULL);  // (EINTR: the loop re-reads the clock)
    }
}

// ---- process
void plat_init(void) {
    // send() on a dropped Local Play connection gives EPIPE here instead of killing the process: Windows reports
    // WSAEPIPE to the caller, which is what nn::socket expects.
    signal(SIGPIPE, SIG_IGN);
}

unsigned long plat_process_id(void) { return (unsigned long)getpid(); }
bool plat_wait_process(unsigned long pid) {
    struct timespec pause = {0, 10000000};
    for (int i = 0; i < 3000; ++i) {
        if (kill((pid_t)pid, 0) < 0) return errno == ESRCH;
        nanosleep(&pause, NULL);
    }
    return false;
}

bool plat_exe_path(char *buf, size_t size) {
    const char *home = getenv("MHGU_HOME");
    if (!home || !*home) return false;
    return snprintf(buf, size, "%s/mhgurecomp", home) < (int)size;
}

// Started on its own: run in the executable's folder (MHGU_HOME: in that folder instead); with nobody watching stderr (a launcher, a .desktop entry, `&`)
// the log goes to log.txt and plat_fatal_notice then points the player at it.
static bool g_own_log;

static void *log_tee(void *arg) {
    struct { int fd; FILE *log; } *t = arg;
    char buf[4096], line[4096];
    size_t n = 0;
    for (;;) {
        ssize_t r = read(t->fd, buf, sizeof buf);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
        fwrite(buf, 1, (size_t)r, t->log);
        fflush(t->log);
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n' || n == sizeof line - 1) {
                line[n] = 0;
                __android_log_write(ANDROID_LOG_INFO, "MHGU", line);
                n = 0;
                if (buf[i] == '\n') continue;
            }
            line[n++] = buf[i];
        }
    }
    return NULL;
}

void plat_standalone(void) {
    char exe[PATH_MAX];
    const char *home = getenv("MHGU_HOME");
    if (home && *home) {
        mkdir(home, 0755);
        if (chdir(home) != 0) { /* the folder we are in will do */ }
    } else if (plat_exe_path(exe, sizeof exe)) {
        char *s = strrchr(exe, '/');
        if (s) { *s = 0; if (chdir(exe) != 0) { /* the folder we are in will do */ } }
    }
    // log.txt in the home folder, and every line to logcat as well (adb logcat -s MHGU): stderr goes to a pipe a
    // thread reads, writing each line to both.
    int fds[2];
    rename("log.txt", "log_prev.txt");  // (kept: after a crash the share screen sends both)
    FILE *log = fopen("log.txt", "w");
    if (log && pipe(fds) == 0) {
        setvbuf(log, NULL, _IOLBF, 0);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        static struct { int fd; FILE *log; } tee;
        tee.fd = fds[0], tee.log = log;
        pthread_t th;
        pthread_create(&th, NULL, log_tee, &tee);
        pthread_detach(th);
        setvbuf(stderr, NULL, _IONBF, 0);
        g_own_log = true;
    } else if (log) {
        fclose(log);
        if (freopen("log.txt", "w", stderr)) { setvbuf(stderr, NULL, _IONBF, 0); g_own_log = true; }
    }
}

// $XDG_DATA_HOME/MHGURecomp, else ~/.local/share/MHGURecomp (docs/STATUS.md "Save data", runtime/platform.h).
static bool mkdir_p(const char *path) {
    char buf[PATH_MAX];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *s = buf + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(buf, 0777) != 0 && errno != EEXIST) return false;
        *s = '/';
    }
    return mkdir(buf, 0777) == 0 || errno == EEXIST;
}

bool plat_user_dir(char *buf, size_t size) {
    const char *base = getenv("XDG_DATA_HOME");
    char fallback[PATH_MAX];
    if (!base || !*base) {
        const char *home = getenv("HOME");
        if (!home || !*home) return false;
        snprintf(fallback, sizeof fallback, "%s/.local/share", home);
        base = fallback;
    }
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/MHGURecomp", base) >= (int)sizeof dir) return false;
    if (!mkdir_p(dir)) return false;
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    snprintf(buf, size, "%s", dir);
    return true;
}

static int count_files(const char *dir, int depth) {
    DIR *dd = depth > 32 ? NULL : opendir(dir);
    if (!dd) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dd))) {
        if (e->d_name[0] == '.') continue;
        char sub[PATH_MAX];
        if (snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name) >= (int)sizeof sub) continue;
        int type = e->d_type;
        if (type == DT_UNKNOWN || type == DT_LNK) {  // (file systems without types; links: what they point at)
            struct stat st;
            type = stat(sub, &st) != 0 ? DT_UNKNOWN : S_ISDIR(st.st_mode) ? DT_DIR : S_ISREG(st.st_mode) ? DT_REG : DT_UNKNOWN;
        }
        if (type == DT_DIR) n += count_files(sub, depth + 1);
        else if (type == DT_REG) n++;
    }
    closedir(dd);
    return n;
}
int plat_count_files(const char *dir) { return count_files(dir, 0); }
// (paths are UTF-8 here already)
bool plat_path_from_utf8(const char *utf8, char *out, size_t size) { return snprintf(out, size, "%s", utf8) < (int)size; }
bool plat_path_to_utf8(const char *native, char *out, size_t size) { return snprintf(out, size, "%s", native) < (int)size; }

void plat_error_box(const char *msg) {
    fprintf(stderr, "\n*** %s\n", msg);
    __android_log_write(ANDROID_LOG_ERROR, "MHGU", msg);
    // SDL's message box (an Android dialog over the activity), looked up at run time: the platform layer does not link
    // to SDL. In the game's window colours: wine-dark, cream text, a bronze button.
    typedef bool (*Box)(const SDL_MessageBoxData *, int *);
    Box box = (Box)dlsym(RTLD_DEFAULT, "SDL_ShowMessageBox");
    if (!box) return;
    static const SDL_MessageBoxColorScheme look = {{{38, 18, 14}, {240, 228, 200}, {196, 150, 70}, {62, 38, 22}, {112, 76, 28}}};
    const SDL_MessageBoxButtonData ok = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "OK"};
    const SDL_MessageBoxData data = {SDL_MESSAGEBOX_ERROR, NULL, "MHGU", msg, 1, &ok, &look};
    int pressed;
    box(&data, &pressed);
}

// The game's error screen, in a process of its own (LogShareActivity): this one is about to end, and sharing the log
// must outlive it. SDL looked up at run time, as in plat_error_box.
static bool crash_screen(void) {
    typedef void *(*Get)(void);
    Get get_env = (Get)dlsym(RTLD_DEFAULT, "SDL_GetAndroidJNIEnv"), get_act = (Get)dlsym(RTLD_DEFAULT, "SDL_GetAndroidActivity");
    JNIEnv *env = get_env ? (JNIEnv *)get_env() : NULL;
    jobject act = env && get_act ? (jobject)get_act() : NULL;
    if (!act) return false;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetStaticMethodID(env, cls, "crashReport", "()V") : NULL;
    if (m) (*env)->CallStaticVoidMethod(env, cls, m);
    bool ok = m && !(*env)->ExceptionCheck(env);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
    return ok;
}

int plat_thread_create(void *(*fn)(void *), void *arg, size_t stack) {
    pthread_attr_t attr;
    pthread_t th;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);  // (mapped, not committed: pages come as the stack grows)
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);  // (nobody joins it: its stack goes when it ends)
    int err = pthread_create(&th, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return err;
}

void plat_fatal_notice(void) {
    int config_language(void);
    if (!g_own_log) return;
    const char *msg = settings_text(config_language(), "The game stopped because of an error. The details are in log.txt, "
                                                       "in Android/data/com.mhgu.recomp/files.");
    if (crash_screen()) {
        fprintf(stderr, "\n*** %s\n", msg);
        usleep(300000);  // (the log's last lines on their way to the file: stderr is a pipe, log_tee)
        return;
    }
    plat_error_box(msg);
}

// Which phone, for a log someone else reads (not everyone who sends one knows its chip)
void plat_log_device(void) {
    char maker[PROP_VALUE_MAX] = "", model[PROP_VALUE_MAX] = "", soc[PROP_VALUE_MAX] = "", hw[PROP_VALUE_MAX] = "", ver[PROP_VALUE_MAX] = "";
    __system_property_get("ro.product.manufacturer", maker);
    __system_property_get("ro.product.model", model);
    __system_property_get("ro.soc.model", soc);
    __system_property_get("ro.hardware", hw);
    __system_property_get("ro.build.version.release", ver);
    extern bool gamelib_cpu_lse(void);
    fprintf(stderr, "[device] %s %s, Android %s (API %d), chip %s (%s), %ld CPUs, %lld MB, LSE atomics %s\n", maker, model, ver,
            android_get_device_api_level(), soc[0] ? soc : "?", hw, sysconf(_SC_NPROCESSORS_CONF),
            (long long)sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE) >> 20, gamelib_cpu_lse() ? "yes" : "no");
}

// ---- diagnostics
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    static __thread char name[PATH_MAX], exe[PATH_MAX];
    Dl_info di;
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_fname) return NULL;
    if (!exe[0] && !plat_exe_path(exe, sizeof exe)) snprintf(exe, sizeof exe, "-");
    if (!strcmp(di.dli_fname, exe)) return NULL;  // (the caller prints "mhgurecomp+<off>" for the executable itself)
    const char *slash = strrchr(di.dli_fname, '/');
    snprintf(name, sizeof name, "%s", slash ? slash + 1 : di.dli_fname);
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_fbase;
    return name;
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

typedef struct { void **bt; int n, max; } BtState;
static _Unwind_Reason_Code bt_step(struct _Unwind_Context *ctx, void *arg) {
    BtState *s = arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && s->n < s->max) s->bt[s->n++] = (void *)pc;
    return s->n < s->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}

void plat_print_backtrace(void) {
    void *bt[64];
    BtState st = {bt, 0, 64};
    _Unwind_Backtrace(bt_step, &st);
    int n = st.n;
    Dl_info self;
    uint64_t exe = dladdr((void *)(uintptr_t)&plat_print_backtrace, &self) ? (uint64_t)(uintptr_t)self.dli_fbase : 0;
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        const char *s = rt_host_fn_name(pc, &off);
        if (!s) s = plat_symbol(pc, &off);
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, s, (unsigned long long)off);
        else fprintf(stderr, "  #%-2d %016llx mhgurecomp+%llx\n", i, (unsigned long long)pc,
                     (unsigned long long)(exe ? pc - exe : 0));
    }
}

static const char *sig_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    default: return "signal";
    }
}

// 1 write, 0 read, -1 unknown
#if defined(__aarch64__)
// (the ESR record the kernel puts in the signal frame: data abort, WnR is bit 6)
static int fault_is_write(ucontext_t *ctx) {
    struct _aarch64_ctx *h = (struct _aarch64_ctx *)(void *)ctx->uc_mcontext.__reserved;
    for (int i = 0; i < 64 && h->magic; i++) {
        if (h->magic == ESR_MAGIC) return (((struct esr_context *)h)->esr >> 6) & 1;
        if (!h->size) break;
        h = (struct _aarch64_ctx *)((char *)h + h->size);
    }
    return -1;
}
#define FAULT_PC(ctx) ((uint64_t)(ctx)->uc_mcontext.pc)
#define FAULT_LR(ctx) ((uint64_t)(ctx)->uc_mcontext.regs[30])
#else  // x86_64 (the emulator)
static int fault_is_write(ucontext_t *ctx) { return (ctx->uc_mcontext.gregs[REG_ERR] & 2) != 0; }
#define FAULT_PC(ctx) ((uint64_t)(ctx)->uc_mcontext.gregs[REG_RIP])
static uint64_t fault_lr(ucontext_t *ctx) {
    uint64_t sp = (uint64_t)ctx->uc_mcontext.gregs[REG_RSP], end = 0;
    return plat_committed(sp, &end) && end - sp >= 8 ? *(const uint64_t *)(uintptr_t)sp : 0;
}
#define FAULT_LR(ctx) fault_lr(ctx)
#endif

static void fault_handler(int sig, siginfo_t *si, void *uc) {
    ucontext_t *ctx = (ucontext_t *)uc;
    uint64_t fa = si && si->si_addr ? (uint64_t)(uintptr_t)si->si_addr : 0;
    int saved_errno = errno;  // (the interrupted code may be between a call and its errno check)
    // A write to a block the renderer watches. Before lazy_commit: that one resumes any fault in a backed chunk, and a
    // read-only page would fault again forever.
    if (sig == SIGSEGV && wt_fault(fa)) { errno = saved_errno; return; }
    if ((sig == SIGSEGV || sig == SIGBUS) && lazy_commit(fa)) {  // (a guest page first touched: back it, resume)
        if (plat_touch_hook) plat_touch_hook(fa - g_lo, fault_is_write(ctx));
        errno = saved_errno;
        return;
    }
    uint64_t pc = FAULT_PC(ctx);
    uint64_t lr = FAULT_LR(ctx);
    char what[80];
    snprintf(what, sizeof what, "%s (code %d, addr %llx)", sig_name(sig), si ? si->si_code : 0,
             (unsigned long long)fa);
    rt_fault_report(what, fa, pc, lr);
    _exit(139);  // (rt_fault_report does not return)
}

void plat_crash_handler_install(void) {
    // A guard page under an exhausted stack faults with no room left to run a handler, so run it on its own stack.
    // Signal stacks are per thread: this covers the calling thread, guest threads keep theirs.
    // Signal stacks are per thread, and guest threads fault into fresh pages all the time: each gets its own (bionic's
    // default one is small for the fault report).
    static __thread void *altstack;
    stack_t old;
    if (!altstack && sigaltstack(NULL, &old) == 0 && !(old.ss_flags & SS_ONSTACK)) {
        size_t size = 256 * 1024;
        void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            stack_t ss = {.ss_sp = p, .ss_size = size};
            if (sigaltstack(&ss, NULL) == 0) altstack = p;
            else munmap(p, size);
        }
    }
    struct sigaction sa = {0};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}
