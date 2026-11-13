// Guest address -> recompiled function lookup, and the host (HLE) function registry.
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gamelib.h"
#include "hle.h"
#include "interp.h"
#include "kernel.h"

typedef struct { const uint32_t *addrs; const GuestFn *fns; uint32_t count, lo, hi; } FnTable;
static FnTable tables[2];

// HLE functions live at fake addresses HLE_BASE + 16 * i
#define MAX_HLE 8192
typedef struct { const char *name; HostFn fn; } HleSlot;
static HleSlot hle_slots[MAX_HLE];
static int hle_count;
static pthread_mutex_t hle_lock = PTHREAD_MUTEX_INITIALIZER;

// Overrides of recompiled guest functions (by address)
#define MAX_OVERRIDE 1024
static struct { uint32_t addr; HostFn fn; const char *name; } overrides[MAX_OVERRIDE];
static int override_count;

// Host implementations registered by name (HLE_FUNC)
#if defined(__linux__)
extern const HleImplEntry __start_hle_impls[], __stop_hle_impls[];
#else
// PE/COFF: the linker sorts .hle$a < .hle$m (entries, hle.h) < .hle$z by the text after '$'.
__attribute__((section(".hle$a"), used)) static const HleImplEntry __start_hle_impls[1];
__attribute__((section(".hle$z"), used)) static const HleImplEntry __stop_hle_impls[1];
#endif

HostFn hle_lookup_impl(const char *name) {
    const char *skip = getenv("MHGU_HLE_SKIP");  // debugging: disable HLE for names containing this
    if (skip && *skip && strstr(name, skip)) return NULL;
    const HleImplEntry *start = __start_hle_impls, *stop = __stop_hle_impls;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    for (const HleImplEntry *p = start; p < stop; p++)
        if (p->name && !strcmp(p->name, name)) return p->fn;
    return NULL;
}

uint32_t hle_register(const char *name, HostFn fn) {
    pthread_mutex_lock(&hle_lock);
    for (int i = 0; i < hle_count; i++)
        if (!strcmp(hle_slots[i].name, name)) { pthread_mutex_unlock(&hle_lock); return HLE_BASE + 16u * i; }
    if (hle_count == MAX_HLE) FATAL("too many HLE stubs\n");
    hle_slots[hle_count].name = strdup(name);
    hle_slots[hle_count].fn = fn;
    uint32_t a = HLE_BASE + 16u * hle_count++;
    pthread_mutex_unlock(&hle_lock);
    return a;
}

void hle_override(uint32_t addr, HostFn fn, const char *name) {
    if (override_count == MAX_OVERRIDE) FATAL("too many overrides\n");
    overrides[override_count].addr = addr;
    overrides[override_count].fn = fn;
    overrides[override_count].name = name;
    override_count++;
}

static __thread uint32_t unimpl_pc;
static int unimpl_counts[MAX_HLE];
static void hle_unimplemented(Cpu *c) {
    int i = (int)((unimpl_pc - HLE_BASE) / 16);
    if (unimpl_counts[i]++ < 3)
        LOG("[hle] UNIMPLEMENTED %s (r0=%x r1=%x r2=%x lr=%x) -> returning 0\n", hle_slots[i].name, c->r[0], c->r[1],
            c->r[2], c->r[14]);
    c->r[0] = 0;
    c->r[1] = 0;
}

uint32_t guest_sym(const char *name) {
    static struct { const char *name; uint32_t addr; } cache[64];
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    for (int i = 0; i < 64 && cache[i].name; i++)
        if (cache[i].name == name) { uint32_t a = cache[i].addr; pthread_mutex_unlock(&m); return a; }
    pthread_mutex_unlock(&m);
    uint32_t a = loader_find_symbol(name);
    if (!a) FATAL("guest symbol %s not found\n", name);
    pthread_mutex_lock(&m);
    for (int i = 0; i < 64; i++) if (!cache[i].name) { cache[i].name = name; cache[i].addr = a; break; }
    pthread_mutex_unlock(&m);
    return a;
}

static uint32_t hle_next = HLE_HEAP;
uint32_t hle_alloc(uint32_t size) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    uint32_t a = hle_next;
    hle_next += (size + 63) & ~63u;
    if (hle_next > HLE_HEAP + HLE_HEAP_SIZE) FATAL("HLE heap exhausted\n");
    pthread_mutex_unlock(&m);
    plat_commit(GUEST_BASE + a, size);
    memset(G2H(a), 0, size);
    return a;
}

// The perf pass's MHGU_PERF_DISPATCH=1 (0 or unset: the old way), read once: rt_dispatch's misses (dispatch_slow2,
// below). MHGU_PERF_DISPATCH_STATS=1: each thread's misses in the log every 10 s, either way. (Read at the first
// guest thread's cache, which kernel_init may make before dispatch_init.)
static int env_on(const char *name) {
    const char *e = getenv(name);
    return e && *e && strcmp(e, "0") ? 1 : 0;
}
static int dispatch_v2(void) {
    static int v = -1;
    if (v < 0) v = env_on("MHGU_PERF_DISPATCH");
    return v;
}
static int dispatch_stats(void) {
    static int v = -1;
    if (v < 0) v = env_on("MHGU_PERF_DISPATCH_STATS");
    return v;
}

// (MHGU_PERF_DISPATCH) A table's first function at or after each 4 KB page of its range: a lookup searches its page's
// functions only, not all of them (the same lower bound, so the same function).
static uint32_t *page_first[2];

static void page_index_build(int t) {
    FnTable *ft = &tables[t];
    if (!ft->count) return;
    uint64_t lo_page = ft->lo >> 12, pages = (ft->hi >> 12) - lo_page + 2;  // (+1: the page after the last one's end)
    uint32_t *first = malloc(pages * sizeof *first), i = 0;
    if (!first) return;
    for (uint64_t p = 0; p < pages; p++) {
        uint64_t start = (lo_page + p) << 12;
        while (i < ft->count && ft->addrs[i] < start) i++;
        first[p] = i;
    }
    page_first[t] = first;
}

void dispatch_init(void) {
    const GameLibTables *g = gamelib_tables();
    uint32_t nm = *g->main_count, ns = *g->sdk_count;
    tables[0] = (FnTable){g->main_addrs, g->main_fns, nm, g->main_addrs[0], g->main_addrs[nm - 1]};
    tables[1] = (FnTable){g->sdk_addrs, g->sdk_fns, ns, g->sdk_addrs[0], g->sdk_addrs[ns - 1]};
    if (dispatch_v2()) {
        page_index_build(0), page_index_build(1);
        LOG("[dispatch] perf: two ways, HLE functions cached, lookups by page%s\n", dispatch_stats() ? " (stats on)" : "");
    }
}

// dispatch_lookup by page_first (MHGU_PERF_DISPATCH): the same function, fewer steps
static GuestFn dispatch_lookup_paged(uint32_t addr) {
    for (int t = 0; t < 2; t++) {
        FnTable *ft = &tables[t];
        if (addr < ft->lo || addr > ft->hi) continue;
        uint32_t lo = 0, hi = ft->count;
        if (page_first[t]) {
            uint32_t p = (addr >> 12) - (ft->lo >> 12);
            lo = page_first[t][p], hi = page_first[t][p + 1];
        }
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (ft->addrs[mid] < addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo < ft->count && ft->addrs[lo] == addr) return ft->fns[lo];
    }
    return NULL;
}

GuestFn dispatch_lookup(uint32_t addr) {
    for (int t = 0; t < 2; t++) {
        FnTable *ft = &tables[t];
        if (addr < ft->lo || addr > ft->hi) continue;
        uint32_t lo = 0, hi = ft->count;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (ft->addrs[mid] < addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo < ft->count && ft->addrs[lo] == addr) return ft->fns[lo];
    }
    return NULL;
}

uint32_t dispatch_function_at(uint32_t addr, uint32_t *next) {
    FnTable *ft = &tables[addr >= SDK_BASE];
    *next = 0;
    if (!ft->count || addr < ft->lo) return 0;
    uint32_t lo = 0, hi = ft->count;  // the first entry above addr
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (ft->addrs[mid] <= addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < ft->count) *next = ft->addrs[lo];
    return ft->addrs[lo - 1];
}

// ---- functions run in the interpreter (rt.h)
// The entry gets a hook to a stub `ldr pc, [pc, #-4]; .word entry` in interpreted memory, so a call runs the stub
// and goes on at the entry in the interpreter (its code is an interpreted range); a hook already there stays (its
// original, fnhook_call_original, is then interpreted).
#define MAX_INTERP_FNS 512
static uint32_t interp_fns[MAX_INTERP_FNS], interp_stubs;
static volatile int n_interp_fns;
static pthread_mutex_t interp_fn_lock = PTHREAD_MUTEX_INITIALIZER;

static bool interpreted(uint32_t addr) {
    int n = __atomic_load_n(&n_interp_fns, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (interp_fns[i] == addr) return true;
    return false;
}

int dispatch_interpret(uint32_t entry, uint32_t end) {
    pthread_mutex_lock(&interp_fn_lock);
    int ok = interpreted(entry) ? 0 : -1;
    if (ok < 0 && n_interp_fns < MAX_INTERP_FNS && dispatch_lookup(entry)) {
        if (!interp_stubs) {
            interp_stubs = hle_alloc(MAX_INTERP_FNS * 8);
            interp_add_range(interp_stubs, interp_stubs + MAX_INTERP_FNS * 8);
        }
        uint32_t stub = interp_stubs + n_interp_fns * 8;
        ST32(stub, 0xE51FF004u);
        ST32(stub + 4, entry);
        interp_add_range(entry, end);
        interp_fns[n_interp_fns] = entry;
        __atomic_store_n(&n_interp_fns, n_interp_fns + 1, __ATOMIC_RELEASE);
        if (!fnhook_get(entry)) fnhook_set(entry, stub);
        ok = 1;
    }
    pthread_mutex_unlock(&interp_fn_lock);
    return ok;
}

// ---- the original first words of hooked entries (rt.h)
#define MAX_KEPT 1024
static struct { uint32_t addr, w[2]; } kept[MAX_KEPT];
static volatile int nkept;
static pthread_mutex_t kept_lock = PTHREAD_MUTEX_INITIALIZER;

void fnhook_keep_entry(uint32_t addr, uint32_t w0, uint32_t w1) {
    pthread_mutex_lock(&kept_lock);
    int n = nkept;
    bool have = false;
    for (int i = 0; i < n; i++) have |= kept[i].addr == addr;  // (hooked again: the first words stay the original)
    if (!have && n < MAX_KEPT) {
        kept[n].addr = addr, kept[n].w[0] = w0, kept[n].w[1] = w1;
        __atomic_store_n(&nkept, n + 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&kept_lock);
}

bool fnhook_entry_word(uint32_t pc, uint32_t *w) {
    int n = __atomic_load_n(&nkept, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (pc - kept[i].addr < 8) {
            *w = kept[i].w[(pc - kept[i].addr) / 4];
            return true;
        }
    return false;
}

// ---- function hooks (rt.h)
uint8_t g_fnhook[HLE_BASE >> 5];
#define MAX_HOOKS 1024
static struct { uint32_t addr, detour; } hooks[MAX_HOOKS];
static struct { uint32_t at, addr; bool original; } tramps[MAX_HOOKS];
static int nhooks, ntramps;
static pthread_mutex_t hook_lock = PTHREAD_MUTEX_INITIALIZER;
// The next entry of this function runs it (the hook's original). Kept in the Cpu's spare bytes, not in a __thread
// variable (emulated TLS on Android 9): each guest thread has its own Cpu.
#define CPU_BYPASS 8
static inline uint32_t hook_bypass(const Cpu *c) { uint32_t v; memcpy(&v, c->_pad + CPU_BYPASS, 4); return v; }
static inline void set_hook_bypass(Cpu *c, uint32_t v) { memcpy(c->_pad + CPU_BYPASS, &v, 4); }

bool fnhook_set(uint32_t addr, uint32_t detour) {
    if (addr >= HLE_BASE || !dispatch_lookup(addr)) {
        LOG("[hook] %08x is not the start of a recompiled function: not hooked\n", addr);
        return false;
    }
    pthread_mutex_lock(&hook_lock);
    int i = 0;
    while (i < nhooks && hooks[i].addr != addr) i++;
    if (i == nhooks) {
        if (nhooks == MAX_HOOKS) {
            pthread_mutex_unlock(&hook_lock);
            LOG("[hook] too many function hooks (%d): %08x is not hooked\n", MAX_HOOKS, addr);
            return false;
        }
        hooks[i].addr = addr;
        __atomic_store_n(&nhooks, nhooks + 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&hooks[i].detour, detour, __ATOMIC_RELEASE);
    uint8_t bit = (uint8_t)(1u << ((addr >> 2) & 7));
    if (detour) __atomic_fetch_or(&g_fnhook[addr >> 5], bit, __ATOMIC_RELEASE);
    else __atomic_fetch_and(&g_fnhook[addr >> 5], (uint8_t)~bit, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&hook_lock);
    return true;
}

uint32_t fnhook_get(uint32_t addr) {
    int n = __atomic_load_n(&nhooks, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (hooks[i].addr == addr) return __atomic_load_n(&hooks[i].detour, __ATOMIC_ACQUIRE);
    return 0;
}

int fnhook_enter(Cpu *c, uint32_t addr) {
    if (hook_bypass(c) == addr) {
        set_hook_bypass(c, 0);
        return 0;
    }
    uint32_t detour = fnhook_get(addr);
    if (!detour) return 0;
    c->pc = detour;  // (in the function's place: the detour returns to the function's caller)
    rt_dispatch(c);
    return 1;
}

void fnhook_call_original(Cpu *c, uint32_t addr) {
    if (interpreted(addr)) {  // (its code was changed: dispatch_interpret)
        c->pc = addr;
        interp_run(c);
        return;
    }
    GuestFn f = dispatch_lookup(addr);
    if (!f) FATAL("hook original: no function at %x\n", addr);
    set_hook_bypass(c, addr);
    f(c);
    set_hook_bypass(c, 0);
}

bool fnhook_add_trampoline(uint32_t at, uint32_t addr, bool original) {
    pthread_mutex_lock(&hook_lock);
    if (ntramps == MAX_HOOKS) {
        pthread_mutex_unlock(&hook_lock);
        return false;
    }
    tramps[ntramps].at = at;
    tramps[ntramps].addr = addr;
    tramps[ntramps].original = original;
    __atomic_store_n(&ntramps, ntramps + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&hook_lock);
    return true;
}

// Room for a hook at addr (and a trampoline for it): the tables are fixed, a hook that does not fit fails cleanly.
bool fnhook_room(uint32_t addr, bool tramp) {
    bool ok = true;
    pthread_mutex_lock(&hook_lock);
    int i = 0;
    while (i < nhooks && hooks[i].addr != addr) i++;
    if (i == nhooks && nhooks == MAX_HOOKS) ok = false;
    if (tramp && ntramps == MAX_HOOKS) ok = false;
    pthread_mutex_unlock(&hook_lock);
    pthread_mutex_lock(&kept_lock);
    bool have = false;
    for (int k = 0; k < nkept; k++) have |= kept[k].addr == addr;
    if (!have && nkept == MAX_KEPT) ok = false;
    pthread_mutex_unlock(&kept_lock);
    return ok;
}

static void trampoline(Cpu *c) {
    int n = __atomic_load_n(&ntramps, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        if (tramps[i].at != c->pc) continue;
        if (tramps[i].original) {
            fnhook_call_original(c, tramps[i].addr);
            return;
        }
        c->pc = tramps[i].addr;
        __attribute__((musttail)) return rt_dispatch(c);
    }
    FATAL("no function at %x (lr=%x): not a hook trampoline\n", c->pc, c->r[14]);
}

static void interp_entry(Cpu *c) { interp_run(c); }

// Small direct-mapped cache in front of the binary search, one per guest thread. Its Cpu's spare bytes (_pad, which
// the game's code never touches) point to it: built for Android 9, a __thread variable is emulated TLS, a call and a
// pthread_getspecific on every indirect jump (about a tenth of a game thread's time on a Moto g06). The thread-local
// cache is only the fallback for a Cpu that has none.
#define DCACHE 4096
typedef struct { uint32_t addr; GuestFn fn; } DEntry;
#define CPU_DCACHE 0  // _pad offset: this thread's cache (DEntry *); CPU_BYPASS (above) is at 8
static __thread DEntry dcache_tls[DCACHE];
static __attribute__((noinline)) DEntry *dc_tls(void) { return dcache_tls; }
static inline DEntry *dc_of(Cpu *c) {
    DEntry *dc;
    memcpy(&dc, c->_pad + CPU_DCACHE, sizeof dc);
    return __builtin_expect(dc != NULL, 1) ? dc : dc_tls();
}

// (MHGU_PERF_DISPATCH) A thread's cache has a second way, DCACHE more entries right after the first, and its miss
// counts after that (MHGU_PERF_DISPATCH_STATS). The thread-local fallback has the first way only.
typedef struct { uint64_t t0; uint32_t slow, way1, hle, lookups; } DStats;
#define DC_WAYS2 (2 * DCACHE * sizeof(DEntry))
static inline DStats *dc_stats(DEntry *dc) { return (DStats *)((char *)dc + DC_WAYS2); }

void dispatch_thread_init(Cpu *c) {
    DEntry *dc = dispatch_v2() || dispatch_stats() ? calloc(1, DC_WAYS2 + sizeof(DStats)) : calloc(DCACHE, sizeof *dc);
    memcpy(c->_pad + CPU_DCACHE, &dc, sizeof dc);
}
void dispatch_thread_done(Cpu *c) {  // (its thread has left the guest's code for good: kernel.c thread_main)
    DEntry *dc, *none = NULL;
    memcpy(&dc, c->_pad + CPU_DCACHE, sizeof dc);
    memcpy(c->_pad + CPU_DCACHE, &none, sizeof none);
    free(dc);
}

static GuestFn dispatch_slow2(Cpu *c, DEntry *dcache);
static void dstats_count(Cpu *c, int what);

static GuestFn dispatch_slow(Cpu *c, DEntry *dcache) {
    if (dispatch_v2()) return dispatch_slow2(c, dcache);
    uint32_t pc = c->pc;
    if (dispatch_stats()) dstats_count(c, pc >= HLE_BASE && pc < HLE_BASE + HLE_SIZE ? 2 : 3);
    if (pc >= HLE_BASE && pc < HLE_BASE + HLE_SIZE) {
        rt_unwind_point();
        int i = (int)((pc - HLE_BASE) / 16);
        if (i < hle_count) {
            if (hle_slots[i].fn) return hle_slots[i].fn;
            unimpl_pc = pc;
            return hle_unimplemented;
        }
    }
    for (int i = 0; i < override_count; i++)
        if (overrides[i].addr == pc) return overrides[i].fn;
    if (pc & 1) FATAL("jump to Thumb code at %x (lr=%x)\n", pc, c->r[14]);
    GuestFn f = dispatch_lookup(pc);
    if (!f && interp_owns(pc)) f = interp_entry;
    if (!f && pc >= FORGE_TRAMP && pc < FORGE_STUBS) f = trampoline;
    if (!f) {
        uint32_t off;
        const char *s = loader_symbol_at(c->r[14], &off);
        FATAL("no function at %x (lr=%x %s+%x)\n", pc, c->r[14], s ? s : "?", off);
    }
    unsigned h = (pc >> 2) & (DCACHE - 1);
    dcache[h].addr = pc;
    dcache[h].fn = f;
    return f;
}

// The second way's index: a hash of the address, so addresses that share a slot of the first way (16 KB apart) mostly
// don't share one here
static inline unsigned way1_index(uint32_t pc) { return ((pc >> 2) * 2654435761u) >> (32 - 12); }
_Static_assert(DCACHE == 1 << 12, "way1_index");

// (MHGU_PERF_DISPATCH_STATS) what: 1 found in the second way, 2 an HLE function, 3 looked up
static void dstats_count(Cpu *c, int what) {
    DEntry *own;
    memcpy(&own, c->_pad + CPU_DCACHE, sizeof own);
    if (!own) return;  // (the thread-local fallback: no counts)
    DStats *st = dc_stats(own);
    st->slow++;
    if (what == 1) st->way1++;
    else if (what == 2) st->hle++;
    else if (what == 3) st->lookups++;
    if (st->slow & 1023) return;
    uint64_t now = plat_now_ns();
    if (!st->t0) st->t0 = now;
    if (now - st->t0 < 10000000000ull) return;
    LOG("[dispatch] t%x: %u misses in %.1f s (%u in the second way, %u HLE, %u looked up)\n", kernel_current_thread_handle(),
        st->slow, (now - st->t0) / 1e9, st->way1, st->hle, st->lookups);
    st->t0 = now;
    st->slow = st->way1 = st->hle = st->lookups = 0;
}

// rt_dispatch's miss with MHGU_PERF_DISPATCH: rt_dispatch reads the first way only, as before; a miss here
// - looks in the second way (way1_index); found, it goes to the first way and the first way's entry to the second;
// - caches HLE functions too (the old way called this for every one: every NVN call of the render thread). They pass
//   rt_unwind_point here only, which does nothing in this runtime: nothing sets g_rt_unwind (a reboot that sets it
//   would have to drop them from every cache);
// - finds a recompiled function by its page's functions only (dispatch_lookup_paged);
// - keeps the first way's old entry in the second way.
// Each address gets the same function as the old way; overrides are still looked up each time (there are none).
static GuestFn dispatch_slow2(Cpu *c, DEntry *dcache) {
    uint32_t pc = c->pc;
    DEntry *own;
    memcpy(&own, c->_pad + CPU_DCACHE, sizeof own);
    DEntry *w1 = own && own == dcache ? own + DCACHE : NULL;  // (NULL: the thread-local fallback, one way)
    unsigned h = (pc >> 2) & (DCACHE - 1), h1 = way1_index(pc);
    if (w1 && w1[h1].addr == pc && w1[h1].fn) {
        DEntry hit = w1[h1], old = dcache[h];
        w1[h1] = (DEntry){0, NULL};
        if (old.fn) w1[way1_index(old.addr)] = old;
        dcache[h] = hit;
        if (dispatch_stats()) dstats_count(c, 1);
        return hit.fn;
    }
    GuestFn f = NULL;
    if (pc >= HLE_BASE && pc < HLE_BASE + HLE_SIZE) {
        rt_unwind_point();
        int i = (int)((pc - HLE_BASE) / 16);
        if (i < hle_count) {
            if (!hle_slots[i].fn) {
                unimpl_pc = pc;
                return hle_unimplemented;
            }
            f = hle_slots[i].fn;
        }
    }
    if (dispatch_stats()) dstats_count(c, f ? 2 : 3);
    if (!f) {
        for (int i = 0; i < override_count; i++)
            if (overrides[i].addr == pc) return overrides[i].fn;
        if (pc & 1) FATAL("jump to Thumb code at %x (lr=%x)\n", pc, c->r[14]);
        f = dispatch_lookup_paged(pc);
        if (!f && interp_owns(pc)) f = interp_entry;
        if (!f && pc >= FORGE_TRAMP && pc < FORGE_STUBS) f = trampoline;
        if (!f) {
            uint32_t off;
            const char *s = loader_symbol_at(c->r[14], &off);
            FATAL("no function at %x (lr=%x %s+%x)\n", pc, c->r[14], s ? s : "?", off);
        }
    }
    if (w1 && dcache[h].fn) w1[way1_index(dcache[h].addr)] = dcache[h];
    dcache[h].addr = pc;
    dcache[h].fn = f;
    return f;
}

// Always tail-calls, so chains of indirect jumps don't grow the host stack.
void rt_dispatch(Cpu *c) {
    DEntry *dcache = dc_of(c);
    unsigned h = (c->pc >> 2) & (DCACHE - 1);
    // (an empty slot holds address 0: a jump to guest address 0 must not "hit" it)
    GuestFn f = dcache[h].addr == c->pc && dcache[h].fn ? dcache[h].fn : dispatch_slow(c, dcache);
    __attribute__((musttail)) return f(c);
}

void guest_call(Cpu *c, uint32_t addr) {
    uint32_t lr = c->r[14];
    c->r[14] = 0;
    c->pc = addr;
    rt_dispatch(c);
    c->r[14] = lr;
}

// Host pc -> recompiled guest function containing it ("f_<addr>"), for crash reports.
static int cmp_host(const void *a, const void *b) {
    uint64_t x = ((const uint64_t *)a)[0], y = ((const uint64_t *)b)[0];
    return x < y ? -1 : x > y;
}
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) {
    static uint64_t (*byhost)[2];
    static uint64_t n;
    static __thread char name[32];
    if (!tables[0].count) return NULL;
    if (!byhost) {
        uint64_t total = tables[0].count + tables[1].count, k = 0;
        uint64_t (*v)[2] = malloc(total * sizeof *v);
        if (!v) return NULL;
        for (int t = 0; t < 2; t++)
            for (uint32_t i = 0; i < tables[t].count; i++) {
                v[k][0] = (uint64_t)(uintptr_t)tables[t].fns[i];
                v[k++][1] = tables[t].addrs[i];
            }
        qsort(v, k, sizeof *v, cmp_host);
        n = k;
        byhost = v;
    }
    uint64_t lo = 0, hi = n;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (byhost[mid][0] <= pc) lo = mid + 1;
        else hi = mid;
    }
    if (!lo || pc - byhost[lo - 1][0] > 0x100000) return NULL;
    snprintf(name, sizeof name, "f_%llx", (unsigned long long)byhost[lo - 1][1]);
    if (offset) *offset = pc - byhost[lo - 1][0];
    return name;
}

void rt_fatal_trace(void) {
    fprintf(stderr, "host backtrace (guest functions are f_<addr>):\n");
    plat_print_backtrace();
#ifdef __ANDROID__
    void perf_mem_report(void);  // (runtime/platform/android_perf.c: map entries and memory now)
    perf_mem_report();
#endif
    Cpu *c = kernel_current_cpu();
    if (c) {
        uint32_t off = 0;
        const char *s = loader_symbol_at(c->r[14], &off);
        fprintf(stderr, "guest: sp=%08x lr=%08x (%s+%x) pc=%08x\n", c->r[13], c->r[14], s ? s : "?", off, c->pc);
        for (int i = 0; i < 16; i += 4)
            fprintf(stderr, "  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n", i, c->r[i], i + 1, c->r[i + 1], i + 2,
                    c->r[i + 2], i + 3, c->r[i + 3]);
    }
    fflush(stderr);
    plat_fatal_notice();
    signal(SIGABRT, SIG_DFL);
    abort();
}

void rt_udf(Cpu *c, uint32_t pc) {
    (void)c;
    FATAL("undefined instruction / trap at %x\n", pc);
}

void rt_thumb(Cpu *c, uint32_t pc) {
    (void)c;
    FATAL("BLX to Thumb code at %x\n", pc);
}

void rt_bkpt(Cpu *c, uint32_t imm, uint32_t pc) {
    (void)c;
    FATAL("bkpt #%u at %x\n", imm, pc);
}

void rt_fault_report(const char *what, uint64_t fa, uint64_t pc, uint64_t lr) {
    uint64_t po = 0, lo = 0;
    const char *ps = rt_host_fn_name(pc, &po), *ls = rt_host_fn_name(lr, &lo);
    if (!ps) ps = plat_symbol(pc, &po);
    if (!ls) ls = plat_symbol(lr, &lo);
    fprintf(stderr, "\n*** %s accessing %#llx", what, (unsigned long long)fa);
    if (fa >= GUEST_BASE && fa < GUEST_BASE + GUEST_SPACE) fprintf(stderr, " (guest %08llx)", (unsigned long long)(fa - GUEST_BASE));
    fprintf(stderr, "\n    host pc %llx (%s+%llx)\n    host lr %llx (%s)\n", (unsigned long long)pc, ps ? ps : "?",
            (unsigned long long)po, (unsigned long long)lr, ls ? ls : "?");
    rt_fatal_trace();
}

void crash_handler_install(void) { plat_crash_handler_install(); }

// ---- function-entry profiler (MHGU_FNPROF=1): counts calls per guest function, prints the top ones every 5 s.
// MHGU_FNTRACE=lo-hi[:n] (hex) instead logs calls of functions in [lo, hi) (the first n, default 2000) with the
// caller and the calling thread.
volatile int g_fnprof;
static uint32_t trace_lo, trace_hi;
static int trace_left = -1;
static bool new_mode;  // (fnnew, below)
void fnprof_init(void) {
    const char *t = getenv("MHGU_FNTRACE");
    if (t) {
        trace_left = 2000;
        if (sscanf(t, "%x-%x:%d", &trace_lo, &trace_hi, &trace_left) >= 2) g_fnprof = 1;
    }
    if (getenv("MHGU_FNPROF")) g_fnprof = 1, trace_left = -1;
}
// The control file's "fntrace <lo> <hi> [n] [r0 lo] [r0 hi]": start tracing calls of [lo, hi) in a running game (n = 0:
// stop); with an r0 range, only calls whose first argument is in it (the methods of one object).
static uint32_t trace_r0_lo, trace_r0_hi;
void fntrace_set(uint32_t lo, uint32_t hi, int n, uint32_t r0_lo, uint32_t r0_hi) {
    trace_lo = lo, trace_hi = hi, trace_left = n, new_mode = false;
    trace_r0_lo = r0_lo, trace_r0_hi = r0_hi;
    g_fnprof = n > 0;
}
// "fnnew <lo> <hi> [learn ms]": functions in [lo, hi) called during the first learn ms (default 3000) are only noted;
// after that, the first call of every other function there is logged once. Finds what an event (a menu choice, a
// scene change) runs for the first time without the per-frame noise of fntrace. "fnnew 0 0" stops.
static uint64_t new_learn_until;
static uint32_t new_seen[1 << 16];
void fnnew_set(uint32_t lo, uint32_t hi, int learn_ms) {
    memset(new_seen, 0, sizeof new_seen);
    trace_lo = lo, trace_hi = hi, trace_left = 0, new_mode = hi > lo;
    new_learn_until = plat_now_ns() + (uint64_t)(learn_ms > 0 ? learn_ms : 3000) * 1000000;
    g_fnprof = new_mode;
}
static void fnnew_hit(uint32_t addr, uint32_t lr, uint32_t r0) {
    if (addr < trace_lo || addr >= trace_hi) return;
    enum { N = 1 << 16 };
    uint32_t h = (addr >> 2) * 2654435761u >> 16;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t k = (h + i) & (N - 1);
        if (new_seen[k] == addr) return;
        if (!new_seen[k]) {
            new_seen[k] = addr;  // (threads may race here; a function logged twice is harmless)
            if (plat_now_ns() >= new_learn_until)
                LOG("[fnnew] t%x %08x from %08x r0=%08x\n", kernel_current_thread_handle(), addr, lr, r0);
            return;
        }
    }
}
void fnprof_hit(uint32_t addr, uint32_t lr, uint32_t r0) {
    if (new_mode) { fnnew_hit(addr, lr, r0); return; }
    if (trace_left >= 0) {
        if (addr >= trace_lo && addr < trace_hi && trace_left > 0 &&
            (trace_r0_hi <= trace_r0_lo || (r0 >= trace_r0_lo && r0 < trace_r0_hi))) {
            trace_left--;
            LOG("[fntrace] t%x %08x from %08x r0=%08x\n", kernel_current_thread_handle(), addr, lr, r0);
        }
        return;
    }
    enum { N = 1 << 16 };
    static uint32_t keys[N];
    static uint64_t counts[N];
    static uint64_t last;
    uint32_t h = (addr >> 2) * 2654435761u >> 16;
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t k = (h + i) & (N - 1);
        if (keys[k] == addr) { counts[k]++; break; }
        if (!keys[k]) { keys[k] = addr; counts[k] = 1; break; }
    }
    uint64_t now = plat_now_ns();
    if (now - last > 5000000000ull) {
        last = now;
        for (int j = 0; j < 20; j++) {
            uint64_t best = 0;
            int bi = -1;
            for (int k = 0; k < N; k++) if (counts[k] > best) { best = counts[k]; bi = k; }
            if (bi < 0) break;
            uint32_t off;
            const char *s = loader_symbol_at(keys[bi], &off);
            LOG("[fnprof] %08x %10llu %s\n", keys[bi], (unsigned long long)best, s ? s : "");
            counts[bi] = 0;
        }
    }
}
