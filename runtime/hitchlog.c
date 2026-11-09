// (debug) MHGU_HITCHLOG=1, read once at start-up: the frame times of three threads, each against its own running
// median (the last 128 frames): the game's update thread (its whole frame, frame limiter included), the thread that
// presents, and the window's event loop (one pass). A frame slower than max(2.5 x median, median + 6 ms) gets a line:
//   [hitch] render at 123.46 s, frame 81234: 31.4 ms (median 6.25, limit 15.6) | self: cpu 9.1, waits 20.3, other 2.0
//           | process 2.9 cores, system 64% busy (last 0.1-0.2 s), 120 page faults | exec 2 x 7.9 ms | build-wait ...
// frame is the presented frame's number (as in [skip] and [vk] fps lines). self is that thread's own time in the frame:
// cpu (its CPU time, kernel mode included), blocked (waits in the guest kernel: the game's events, condition variables,
// mutexes, sleeps; the longest one with its kind, return address and the game's callers), waits (the measured waits
// below that this thread did) and other (the rest: ready but not running, I/O or page faults outside the measured
// points). Then the whole process: CPU in cores (from the threads' times, to about a millisecond each), the system's
// CPU busy, page faults. Then every counter that moved during the frame, on any thread (hitchlog.h lists them and
// where they are measured): count x total time, the amount, and the slowest one (with its label and its thread: U
// update, R render, I input, G another game thread, O another host thread - shader workers, audio, vsync).
// At most 20 lines a second. Every 10 s: a line per thread (frames, median, 99th percentile, max, frames over 1.5 x
// median, hitches, the thread's and the process's CPU) and one with the counters' totals (and, on Windows, the
// display's refresh rate and the compositor's missed frames). MHGU_HITCHLOG_FACTOR=<x> and MHGU_HITCHLOG_EXTRA_MS=<ms>
// change the 2.5 and the 6 (1.7 and 3 also catch a single refresh repeated at 160 Hz).
#ifdef _WIN32
#define PSAPI_VERSION 2  // (GetProcessMemoryInfo from kernel32: K32GetProcessMemoryInfo)
#include <windows.h>
#include <dwmapi.h>
#include <intrin.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <time.h>
#endif
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "hitchlog.h"
#include "logwriter.h"

int g_hitch_on;
static double g_factor = 2.5;
static uint64_t g_extra_ns = 6000000;
static uint64_t g_t0;

// ---- counters
enum { A_NONE, A_BYTES, A_COUNT };  // what the amount is
enum { N_NONE, N_CMD, N_KB };       // what a label's number is
typedef struct { const char *name; uint8_t wait, amount, num; const char *word; } CInfo;
static const CInfo k_info[HC_COUNT] = {
    [HC_EXEC] = {"exec"},
    [HC_BUILD_WAIT] = {"build-wait", 1, A_COUNT, 0, "skipped"},
    [HC_BUILD_LONG] = {"build-wait-long", 1, A_COUNT, 0, "skipped"},
    [HC_BUILD_SKIP] = {"build-skip"},
    [HC_BUILD_COLD] = {"build-cold"},
    [HC_BUILD_READY] = {"build-ready"},
    [HC_PIPE_NEW] = {"pipe-new"},
    [HC_PROG_NEW] = {"prog-new"},
    [HC_PROG_LOAD] = {"prog-load"},
    [HC_JOB_XLATE] = {"job-translate"},
    [HC_JOB_PROGRAM] = {"job-program"},
    [HC_JOB_PIPE] = {"job-pipeline"},
    [HC_SPIRV_DISK] = {"spirv-disk", 0, A_BYTES},
    [HC_SPIRV_COMPILE] = {"spirv-compile", 0, A_BYTES},
    [HC_PIPE_SYNC] = {"pipe-sync"},
    [HC_CS_NEW] = {"compute-new"},
    [HC_TEX_CREATE] = {"tex-create"},
    [HC_TEX_UPLOAD] = {"tex-upload", 0, A_BYTES, N_KB},
    [HC_TEX_HASH] = {"tex-hash", 0, A_BYTES},
    [HC_TEX_JOB] = {"tex-job", 0, A_BYTES},
    [HC_TEX_WAIT] = {"tex-wait", 1},
    [HC_TEX_GUARD] = {"tex-guard"},
    [HC_TEX_DESTROY] = {"tex-destroy"},
    [HC_DEAD] = {"destroyed", 0, A_COUNT, 0, "objects"},
    [HC_VKALLOC] = {"vkalloc", 0, A_BYTES},
    [HC_VKFREE] = {"vkfree"},
    [HC_VC_NEW] = {"vcache-new", 0, A_BYTES},
    [HC_VC_COPY] = {"vcache-copy", 0, A_BYTES},
    [HC_VC_SAFETY] = {"vcache-safety", 0, A_BYTES},
    [HC_CHUNK] = {"staging-chunk", 0, A_BYTES},
    [HC_DESC_POOL] = {"desc-pool"},
    [HC_DESC_RESET] = {"desc-reset", 0, A_COUNT, 0, "pools"},
    [HC_GPU_OTHER] = {"gpu-wait", 1},
    [HC_GPU_RING] = {"gpu-ring", 1},
    [HC_GPU_FINISH] = {"gpu-finish", 1},
    [HC_GPU_SYNC] = {"gpu-syncwait", 1},
    [HC_GPU_FLUSH] = {"gpu-flush", 1},
    [HC_GPU_FRAME] = {"gpu-frame", 1},
    [HC_GPU_READBACK] = {"gpu-readback", 1},
    [HC_SUBMIT] = {"submit"},
    [HC_ACQUIRE] = {"acquire", 1},
    [HC_PRESENT] = {"present", 1},
    [HC_SWAPCHAIN] = {"swapchain"},
    [HC_VKLOCK] = {"vklock-wait", 1},
    [HC_KLOCK] = {"klock-wait", 1},
    [HC_PRIO_LEND] = {"prio-lend"},
    [HC_OVERLAY_TEX] = {"overlay-tex", 1, A_BYTES},
    [HC_IPC] = {"ipc", 0, A_NONE, N_CMD},
    [HC_FILE_READ] = {"file-read", 1, A_BYTES},
    [HC_FILE_OPEN] = {"file-open", 1},
    [HC_FILE_LOCK] = {"file-lock", 1},
    [HC_LOG] = {"log"},
    [HC_LOG_WRITE] = {"log-write", 1, A_BYTES},
    [HC_LOG_DROP] = {"log-dropped", 0, A_BYTES},
    [HC_MEM_COMMIT] = {"mem-commit", 0, A_BYTES},
    [HC_VSYNC_LATE] = {"vsync-late"},
    [HC_SELF] = {"hitchlog"},
};

static _Atomic uint64_t c_n[HC_COUNT], c_ns[HC_COUNT], c_amt[HC_COUNT];
// The slowest event of each counter: one column per timed thread (its current frame), one for the 10 s totals.
#define OBS (HT_COUNT + 1)
#define LABEL 40
static _Atomic uint64_t c_max[OBS][HC_COUNT];
static char c_lab[OBS][HC_COUNT][LABEL];
static volatile int c_labn[OBS][HC_COUNT];
static volatile char c_role[OBS][HC_COUNT];

static __thread char t_role;      // U R I (timed threads), G a game thread, O another host thread
static __thread uint64_t t_wait;  // this thread's measured waits (the counters marked wait)
static __thread uint64_t t_kw_ns; // this thread's longest guest kernel wait of its frame, what and where
static __thread int t_kw_kind;
static __thread uint32_t t_kw_lr;
static __thread char t_kw_callers[80];
extern __thread uint64_t g_blocked_ns;  // (kernel.c) time this thread spent blocked in the guest kernel

static char role(void) {
    if (!t_role) t_role = kernel_current_thread() ? 'G' : 'O';
    return t_role;
}

static void set_label(char *d, const char *s) {  // (the end of a long one: a file's name)
    size_t n = s ? strlen(s) : 0, k = 0;
    if (n > LABEL - 1) memcpy(d, "...", 3), k = 3, s += n - (LABEL - 4), n = LABEL - 4;
    if (n) memcpy(d + k, s, n);
    d[k + n] = 0;
}

uint64_t hitch_now(void) { return plat_now_ns(); }

void hitch_add_label(int c, uint64_t ns, uint64_t amount, const char *label, int num) {
    if ((unsigned)c >= HC_COUNT) return;
    atomic_fetch_add_explicit(&c_n[c], 1, memory_order_relaxed);
    if (ns) atomic_fetch_add_explicit(&c_ns[c], ns, memory_order_relaxed);
    if (amount) atomic_fetch_add_explicit(&c_amt[c], amount, memory_order_relaxed);
    if (k_info[c].wait) t_wait += ns;
    if (!ns) return;
    char r = role();
    for (int o = 0; o < OBS; o++) {
        uint64_t m = atomic_load_explicit(&c_max[o][c], memory_order_relaxed);
        while (ns > m && !atomic_compare_exchange_weak_explicit(&c_max[o][c], &m, ns, memory_order_relaxed, memory_order_relaxed)) {}
        if (ns <= m) continue;  // (another was slower)
        set_label(c_lab[o][c], label);
        c_labn[o][c] = num;
        c_role[o][c] = r;
    }
}

void hitch_add(int c, uint64_t ns, uint64_t amount) { hitch_add_label(c, ns, amount, NULL, -1); }

void hitch_mutex_lock(pthread_mutex_t *m, int counter) {
    if (!pthread_mutex_trylock(m)) return;
    uint64_t t0 = plat_now_ns();
    pthread_mutex_lock(m);
    hitch_add(counter, plat_now_ns() - t0, 0);
}

void hitch_kwait(uint64_t ns, int kind, const struct Cpu *c) {
    if (ns <= t_kw_ns) return;
    t_kw_ns = ns;
    t_kw_kind = kind;
    t_kw_lr = c ? c->r[14] : 0;
    t_kw_callers[0] = 0;
    if (c && ns >= 1000000) kernel_stack_callers(c->r[13], t_kw_callers, sizeof t_kw_callers);
}

int hitch_logf(const char *fmt, ...) {
    uint64_t t0 = plat_now_ns();
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stderr, fmt, ap);
    va_end(ap);
    hitch_add(HC_LOG, plat_now_ns() - t0, 0);
    return r;
}

// ---- clocks: the thread's CPU time, the process's, page faults (cheap enough for every frame: under a microsecond
// in all), and the system's CPU busy (GetSystemTimes costs ~30 us: sampled every 100 ms by the render thread)
typedef struct { uint64_t tcpu, pcpu, faults; } Clocks;  // tcpu: in cpu_ns's units; pcpu: ns
typedef struct { uint64_t t, busy, all; } SysTimes;      // (all: the CPUs' time; busy: less their idle time)
#ifdef _WIN32
// QueryThreadCycleTime counts time-stamp counter ticks: their rate, measured against plat_now_ns once a second has
// passed (unknown until then: no CPU times in the lines yet)
static uint64_t g_tsc0;
static _Atomic uint64_t g_tsc_khz;
static bool cpu_known(void) {
    if (atomic_load_explicit(&g_tsc_khz, memory_order_relaxed)) return true;
    uint64_t ns = plat_now_ns() - g_t0;
    if (ns < 1000000000ull) return false;
    atomic_store(&g_tsc_khz, (uint64_t)((double)(__rdtsc() - g_tsc0) / (ns / 1e6)));
    return atomic_load(&g_tsc_khz) != 0;
}
static uint64_t cpu_ns(uint64_t ticks) {
    uint64_t khz = atomic_load_explicit(&g_tsc_khz, memory_order_relaxed);
    return khz ? (uint64_t)((double)ticks * 1e6 / (double)khz) : 0;
}
static uint64_t ft64(FILETIME f) { return (uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime; }
static void clocks(Clocks *k, bool full) {
    ULONG64 c = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c);
    k->tcpu = c;
    if (!full) return;
    FILETIME t0, t1, kern, user;  // (the threads' times, charged at the clock's ticks: about a millisecond each)
    if (GetProcessTimes(GetCurrentProcess(), &t0, &t1, &kern, &user)) k->pcpu = (ft64(kern) + ft64(user)) * 100;
    PROCESS_MEMORY_COUNTERS pm;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm)) k->faults = pm.PageFaultCount;
}
static bool sys_times(SysTimes *s) {
    FILETIME idle, kern, user;  // (kernel time includes the idle time)
    if (!GetSystemTimes(&idle, &kern, &user)) return false;
    s->all = ft64(kern) + ft64(user);
    s->busy = s->all - ft64(idle);
    s->t = plat_now_ns();
    return true;
}
#else
static bool cpu_known(void) { return true; }
static uint64_t cpu_ns(uint64_t ns) { return ns; }
static uint64_t ts_ns(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void clocks(Clocks *k, bool full) {
    k->tcpu = ts_ns(CLOCK_THREAD_CPUTIME_ID);
    if (!full) return;
    k->pcpu = ts_ns(CLOCK_PROCESS_CPUTIME_ID);
    struct rusage ru;
    if (!getrusage(RUSAGE_SELF, &ru)) k->faults = (uint64_t)ru.ru_minflt + (uint64_t)ru.ru_majflt;
}
static bool sys_times(SysTimes *s) { (void)s; return false; }
#endif

// The system's samples: the last 8, 100 ms apart (taken by the render thread's frames)
static SysTimes g_sys[8];
static int g_sys_i;
static pthread_mutex_t g_sys_lock = PTHREAD_MUTEX_INITIALIZER;
static void sys_sample(uint64_t now) {
    if (now - g_sys[g_sys_i].t < 100000000ull) return;
    SysTimes s;
    if (!sys_times(&s)) return;
    pthread_mutex_lock(&g_sys_lock);
    g_sys_i = (g_sys_i + 1) % 8;
    g_sys[g_sys_i] = s;
    pthread_mutex_unlock(&g_sys_lock);
}
// The system's CPU busy (%) over the last 0.1 to 0.2 s, or over [since, now] when since is given; -1: not known
static double sys_busy(const SysTimes *since) {
    SysTimes now, from = {0};
    if (!sys_times(&now)) return -1;
    if (since) {
        from = *since;
    } else {
        pthread_mutex_lock(&g_sys_lock);
        for (int k = 0; k < 8 && !from.t; k++) {
            const SysTimes *s = &g_sys[(g_sys_i - k + 8) % 8];
            if (s->t && s->t < now.t && now.t - s->t >= 100000000ull) from = *s;
        }
        pthread_mutex_unlock(&g_sys_lock);
    }
    if (!from.t || now.all <= from.all || now.busy < from.busy) return -1;
    return 100.0 * (double)(now.busy - from.busy) / (double)(now.all - from.all);
}

// ---- output
__attribute__((format(printf, 4, 5))) static void app(char *b, int cap, int *o, const char *fmt, ...) {
    if (*o >= cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b + *o, (size_t)(cap - *o), fmt, ap);
    va_end(ap);
    if (n > 0) *o = *o + n < cap - 1 ? *o + n : cap - 1;
}
static int prec(uint64_t ns) { return ns < 1000000 ? 2 : ns < 100000000 ? 1 : 0; }
#define MS(ns) prec(ns), (ns) / 1e6
static void app_bytes(char *b, int cap, int *o, uint64_t v) {
    if (v >= 1048576) app(b, cap, o, "%.1f MB", v / 1048576.0);
    else if (v >= 1024) app(b, cap, o, "%.0f KB", v / 1024.0);
    else app(b, cap, o, "%llu B", (unsigned long long)v);
}

static void emit(const char *line) {
    uint64_t t0 = plat_now_ns();
    rt_log_write(line, strlen(line));  // (Windows: queued, logwriter.c)
    hitch_add(HC_SELF, plat_now_ns() - t0, 0);
}

static void read_counters(uint64_t *n, uint64_t *ns, uint64_t *amt) {
    for (int c = 0; c < HC_COUNT; c++) {
        n[c] = atomic_load_explicit(&c_n[c], memory_order_relaxed);
        ns[c] = atomic_load_explicit(&c_ns[c], memory_order_relaxed);
        amt[c] = atomic_load_explicit(&c_amt[c], memory_order_relaxed);
    }
}

// The counters that moved (deltas dn, dns, damt), with column obs's slowest events
static void app_counters(char *b, int cap, int *o, const uint64_t *dn, const uint64_t *dns, const uint64_t *damt, int obs) {
    for (int c = 0; c < HC_COUNT; c++) {
        if (!dn[c]) continue;
        if (*o > cap - 200) { app(b, cap, o, " | ..."); return; }
        const CInfo *k = &k_info[c];
        app(b, cap, o, " | %s %llu", k->name ? k->name : "?", (unsigned long long)dn[c]);
        if (dns[c] >= 1000) app(b, cap, o, " x %.*f ms", MS(dns[c]));
        if (k->amount == A_BYTES && damt[c]) {
            app(b, cap, o, ", ");
            app_bytes(b, cap, o, damt[c]);
        } else if (k->amount == A_COUNT && damt[c]) app(b, cap, o, ", %llu %s", (unsigned long long)damt[c], k->word);
        uint64_t mx = atomic_load_explicit(&c_max[obs][c], memory_order_relaxed);
        const char *lab = c_lab[obs][c];
        bool slow = dn[c] > 1 && mx >= 500000;
        if (!mx || (!slow && !lab[0])) continue;
        app(b, cap, o, " (");
        if (slow) app(b, cap, o, "max %.*f ms%s", MS(mx), lab[0] ? " " : "");
        if (lab[0]) app(b, cap, o, "%s", lab);
        if (k->num == N_CMD && c_labn[obs][c] >= 0) app(b, cap, o, " cmd %d", c_labn[obs][c]);
        if (k->num == N_KB && c_labn[obs][c] >= 0) app(b, cap, o, " %d KB", c_labn[obs][c]);
        app(b, cap, o, " on %c)", c_role[obs][c] ? c_role[obs][c] : '?');
    }
}

static void reset_column(int obs) {
    for (int c = 0; c < HC_COUNT; c++) {
        atomic_store_explicit(&c_max[obs][c], 0, memory_order_relaxed);
        c_lab[obs][c][0] = 0;
    }
}

// At most 20 hitch lines a second (all threads); the rest are counted for the totals line
static _Atomic uint64_t g_rl_sec, g_suppressed;
static _Atomic int g_rl_n;
static bool rate_ok(uint64_t now) {
    uint64_t sec = now / 1000000000ull, cur = atomic_load(&g_rl_sec);
    if (sec != cur && atomic_compare_exchange_strong(&g_rl_sec, &cur, sec)) atomic_store(&g_rl_n, 0);
    if (atomic_fetch_add(&g_rl_n, 1) < 20) return true;
    atomic_fetch_add(&g_suppressed, 1);
    return false;
}

// ---- the timed threads
#define RING 128
#define NBINS 1901  // frame times: 0.1 ms steps to 100 ms, 1 ms steps to 1 s, then one bin
typedef struct {
    bool started;
    uint64_t last;
    uint32_t ring[RING];  // the last frames (us), for the running median
    int ring_n, ring_i, since_med;
    uint32_t med_us;
    uint64_t s_n[HC_COUNT], s_ns[HC_COUNT], s_amt[HC_COUNT];  // at the frame's start
    Clocks s_clk;
    uint64_t s_wait, s_blocked;
    uint64_t w_t0, w_frames, w_slow, w_hitch, w_max, w_max_frame;  // the 10 s window
    Clocks w_clk;
    SysTimes w_sys;
    uint32_t hist[NBINS];
} HThread;
static HThread g_th[HT_COUNT];
static const char *const k_tname[HT_COUNT] = {"update", "render", "input"};
static const char k_trole[HT_COUNT] = {'U', 'R', 'I'};

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static uint32_t median_of(const uint32_t *v, int n) {
    uint32_t tmp[RING];
    memcpy(tmp, v, (size_t)n * sizeof *v);
    qsort(tmp, (size_t)n, sizeof *tmp, cmp_u32);
    return tmp[n / 2];
}
static int bin_of(uint64_t us) { return us < 100000 ? (int)(us / 100) : us < 1000000 ? 1000 + (int)((us - 100000) / 1000) : NBINS - 1; }
static double bin_ms(int i) { return i < 1000 ? (i + 1) * 0.1 : i < NBINS - 1 ? 100 + (i - 1000 + 1) : 1000; }
static double quantile(const HThread *t, double q) {
    uint64_t want = (uint64_t)(q * (double)t->w_frames), sum = 0;
    for (int i = 0; i < NBINS; i++)
        if ((sum += t->hist[i]) > want) return bin_ms(i);
    return bin_ms(NBINS - 1);
}

static const char *const k_kind[] = {"?", "mutex", "condvar", "arbiter", "WaitSynchronization", "sleep"};

static void hitch_line(int th, const HThread *t, uint64_t now, uint64_t dt, uint64_t limit, float step, const Clocks *k,
                       uint64_t wait, uint64_t blocked, const uint64_t *n, const uint64_t *ns, const uint64_t *amt) {
    char b[2400];
    int o = 0, cap = sizeof b;
    uint64_t med = (uint64_t)t->med_us * 1000;
    // (frame: the presented frame's number, as in the renderer's own lines: [skip], [vk] fps)
    app(b, cap, &o, "[hitch] %s at %.2f s, frame %llu: %.*f ms (median %.2f, limit %.1f)", k_tname[th], (now - g_t0) / 1e9,
        (unsigned long long)g_present_count, MS(dt), med / 1e6, limit / 1e6);
    if (th == HT_UPDATE) app(b, cap, &o, ", game step %.2f", step);
    bool known = cpu_known();
    uint64_t cpu = cpu_ns(k->tcpu - t->s_clk.tcpu), used = cpu + blocked + wait;
    app(b, cap, &o, " | self:");
    if (known) app(b, cap, &o, " cpu %.*f,", MS(cpu));
    if (blocked) {
        app(b, cap, &o, " blocked %.*f", MS(blocked));
        if (t_kw_ns >= 500000) {
            uint32_t off = 0;
            const char *s = t_kw_lr ? loader_symbol_at(t_kw_lr, &off) : NULL;
            app(b, cap, &o, " (longest %.*f ms %s, lr %x", MS(t_kw_ns), k_kind[t_kw_kind >= 0 && t_kw_kind <= 5 ? t_kw_kind : 0], t_kw_lr);
            if (s) app(b, cap, &o, " %.48s+%x", s, off);
            if (t_kw_callers[0]) app(b, cap, &o, ", callers%s", t_kw_callers);
            app(b, cap, &o, ")");
        }
        app(b, cap, &o, ",");
    }
    app(b, cap, &o, " waits %.*f", MS(wait));
    if (known) app(b, cap, &o, ", other %.*f", MS(dt > used ? dt - used : 0));
    if (th != HT_INPUT) {  // (the process's CPU from the threads' times: to about a millisecond per thread)
        double busy = sys_busy(NULL);
        app(b, cap, &o, " | process %.1f cores,", (double)(k->pcpu - t->s_clk.pcpu) / (double)dt);
        if (busy >= 0) app(b, cap, &o, " system %.0f%% busy (last 0.1-0.2 s),", busy);
        app(b, cap, &o, " %llu page faults", (unsigned long long)(k->faults - t->s_clk.faults));
    }
    uint64_t dn[HC_COUNT], dns[HC_COUNT], damt[HC_COUNT];
    for (int c = 0; c < HC_COUNT; c++) dn[c] = n[c] - t->s_n[c], dns[c] = ns[c] - t->s_ns[c], damt[c] = amt[c] - t->s_amt[c];
    app_counters(b, cap, &o, dn, dns, damt, th);
    app(b, cap, &o, "\n");
    emit(b);
}

static void summary(int th, HThread *t, uint64_t now, const Clocks *k) {
    char b[600];
    int o = 0, cap = sizeof b;
    double secs = (now - t->w_t0) / 1e9;
    app(b, cap, &o, "[hitch] 10 s %s: %llu %s (%.1f a second), median %.2f ms, p99 %.2f, max %.*f (frame %llu), %llu over "
        "1.5 x median, %llu hitches", k_tname[th], (unsigned long long)t->w_frames, th == HT_INPUT ? "passes" : "frames",
        t->w_frames / secs, quantile(t, 0.5), quantile(t, 0.99), MS(t->w_max), (unsigned long long)t->w_max_frame,
        (unsigned long long)t->w_slow, (unsigned long long)t->w_hitch);
    // (the thread's CPU over the 10 s; for the game's threads also the whole process's and the system's)
    uint64_t wall = now - t->w_t0;
    if (cpu_known()) app(b, cap, &o, " | thread busy %.0f%%", 100.0 * (double)cpu_ns(k->tcpu - t->w_clk.tcpu) / (double)wall);
    if (th != HT_INPUT) {
        double busy = t->w_sys.t ? sys_busy(&t->w_sys) : -1;
        app(b, cap, &o, ", process %.1f cores", (double)(k->pcpu - t->w_clk.pcpu) / (double)wall);
        if (busy >= 0) app(b, cap, &o, ", system %.0f%% busy", busy);
        sys_times(&t->w_sys);
    }
    app(b, cap, &o, "\n");
    emit(b);
    memset(t->hist, 0, sizeof t->hist);
    t->w_frames = t->w_slow = t->w_hitch = t->w_max = 0;
    t->w_t0 = now;
    t->w_clk = *k;
}

#ifdef _WIN32
// The display's refresh rate and the compositor's missed frames since the last time (DwmGetCompositionTimingInfo,
// from dwmapi.dll: loaded by hitch_init, not in the middle of the game)
typedef HRESULT(WINAPI *DwmTimingFn)(HWND, DWM_TIMING_INFO *);
static DwmTimingFn g_dwm_timing;
static void app_display(char *b, int cap, int *o) {
    DwmTimingFn fn = g_dwm_timing;
    static bool have;
    static ULONGLONG missed, dropped;
    DWM_TIMING_INFO ti;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = sizeof ti;
    if (!fn || fn(NULL, &ti) != S_OK) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    double hz = ti.rateRefresh.uiDenominator ? (double)ti.rateRefresh.uiNumerator / ti.rateRefresh.uiDenominator
              : ti.qpcRefreshPeriod ? (double)f.QuadPart / (double)ti.qpcRefreshPeriod : 0.0;
    app(b, cap, o, " | display %.2f Hz", hz);
    if (have) app(b, cap, o, ", compositor missed %llu, dropped %llu", (unsigned long long)(ti.cFramesMissed - missed),
                  (unsigned long long)(ti.cFramesDropped - dropped));
    missed = ti.cFramesMissed, dropped = ti.cFramesDropped, have = true;
}
#endif

// Every 10 s, by whichever timed thread gets there first: the counters' totals
static _Atomic uint64_t g_next_totals;
static uint64_t g_tot_n[HC_COUNT], g_tot_ns[HC_COUNT], g_tot_amt[HC_COUNT];
static void totals(uint64_t now) {
    uint64_t due = atomic_load(&g_next_totals);
    if (now < due || !atomic_compare_exchange_strong(&g_next_totals, &due, now + 10000000000ull)) return;
    uint64_t n[HC_COUNT], ns[HC_COUNT], amt[HC_COUNT], dn[HC_COUNT], dns[HC_COUNT], damt[HC_COUNT];
    read_counters(n, ns, amt);
    for (int c = 0; c < HC_COUNT; c++) dn[c] = n[c] - g_tot_n[c], dns[c] = ns[c] - g_tot_ns[c], damt[c] = amt[c] - g_tot_amt[c];
    static char b[6000];
    int o = 0, cap = sizeof b;
    app(b, cap, &o, "[hitch] 10 s totals at %.2f s", (now - g_t0) / 1e9);
    app_counters(b, cap, &o, dn, dns, damt, HT_COUNT);
    reset_column(HT_COUNT);
    uint64_t lost = atomic_exchange(&g_suppressed, 0);
    if (lost) app(b, cap, &o, " | %llu hitch lines not printed (over 20 a second)", (unsigned long long)lost);
#ifdef _WIN32
    app_display(b, cap, &o);
#endif
    app(b, cap, &o, "\n");
    emit(b);
    memcpy(g_tot_n, n, sizeof n), memcpy(g_tot_ns, ns, sizeof ns), memcpy(g_tot_amt, amt, sizeof amt);
}

void hitch_frame(int th, float step) {
    if ((unsigned)th >= HT_COUNT) return;
    HThread *t = &g_th[th];
    uint64_t now = plat_now_ns();
    bool full = th != HT_INPUT;  // (the process's CPU and page faults: not for the event loop's ~1000 passes a second)
    Clocks k = {0};
    clocks(&k, full);
    if (th == HT_RENDER) sys_sample(now);
    uint64_t n[HC_COUNT], ns[HC_COUNT], amt[HC_COUNT];
    read_counters(n, ns, amt);
    uint64_t wait = t_wait, blocked = g_blocked_ns;
    if (!t->started) {
        t->started = true;
        t_role = k_trole[th];
        t->w_t0 = now;
        t->w_clk = k;
        if (full) sys_times(&t->w_sys);
    } else {
        uint64_t dt = now - t->last;
        uint64_t med = (uint64_t)t->med_us * 1000, limit = (uint64_t)((double)med * g_factor);
        if (limit < med + g_extra_ns) limit = med + g_extra_ns;
        bool warm = t->ring_n >= 32;
        if (warm && dt * 2 > med * 3) t->w_slow++;
        if (warm && dt > limit) {
            t->w_hitch++;
            if (rate_ok(now)) hitch_line(th, t, now, dt, limit, step, &k, wait - t->s_wait, blocked - t->s_blocked, n, ns, amt);
        }
        t->w_frames++;
        t->hist[bin_of(dt / 1000)]++;
        if (dt > t->w_max) t->w_max = dt, t->w_max_frame = g_present_count;
        t->ring[t->ring_i] = dt / 1000 > UINT32_MAX ? UINT32_MAX : (uint32_t)(dt / 1000);
        t->ring_i = (t->ring_i + 1) % RING;
        if (t->ring_n < RING) t->ring_n++;
        if (++t->since_med >= (t->ring_n < 32 ? 1 : 16)) t->since_med = 0, t->med_us = median_of(t->ring, t->ring_n);
    }
    // the next frame starts here (the line just written counts in it: HC_SELF)
    t->last = now;
    memcpy(t->s_n, n, sizeof n), memcpy(t->s_ns, ns, sizeof ns), memcpy(t->s_amt, amt, sizeof amt);
    t->s_clk = k;
    t->s_wait = wait, t->s_blocked = blocked;
    t_kw_ns = 0;
    reset_column(th);
    if (now - t->w_t0 >= 10000000000ull) summary(th, t, now, &k);
    totals(now);
}

void hitch_init(void) {
    const char *e = getenv("MHGU_HITCHLOG");
    if (!e || !*e || *e == '0') return;
    g_t0 = plat_now_ns();
#ifdef _WIN32
    g_tsc0 = __rdtsc();
    HMODULE dwm = LoadLibraryA("dwmapi.dll");
    if (dwm) g_dwm_timing = (DwmTimingFn)(void *)GetProcAddress(dwm, "DwmGetCompositionTimingInfo");
#endif
    if ((e = getenv("MHGU_HITCHLOG_FACTOR")) && atof(e) > 1) g_factor = atof(e);
    if ((e = getenv("MHGU_HITCHLOG_EXTRA_MS")) && atof(e) >= 0) g_extra_ns = (uint64_t)(atof(e) * 1e6);
    atomic_store(&g_next_totals, g_t0 + 10000000000ull);
    rt_logf("[hitch] on: a line for each frame slower than max(%.1f x median, median + %.1f ms) of the threads update "
            "(the game's frame), render (presents) and input (window events), with that thread's own time and what ran "
            "in the process meanwhile; every 10 s a summary per thread and the counters' totals\n", g_factor, g_extra_ns / 1e6);
    g_hitch_on = 1;
}
