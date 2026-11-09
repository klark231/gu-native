// Windows' own performance figures for the readout ([Performance] ShowStats: runtime/perfstats.cpp, win_stats.h), with
// no admin rights, no driver and no DLL of ours. They are read once a second on a thread of their own while the readout
// shows; the game's threads only copy the last snapshot, and never wait for this one.
//   - the GPU the Vulkan device runs on (its LUID: nvn/vulkan/device.c), through D3DKMT (gdi32): its temperature (the
//     adapter's perf data, WDDM 2.4), its 3D engine's clock (that node's perf data: about 0.8 ms a call on AMD's driver)
//     and this process's video memory against the budget the system gives it. The perf data's Power is left out: its
//     unit depends on the vendor (watts on AMD's, where Microsoft documents tenths of a percent). A value a driver
//     leaves at 0 (an integrated GPU's temperature) is not shown.
//   - its use: PDH's GPU Engine counters of that LUID, what Task Manager shows (the busiest engine, with every process's
//     work on it). PDH's first counter in a process costs about 0.7 s of this thread's CPU, its own start-up (the
//     others' frames go on meanwhile): pdh.dll is loaded here, and only for Full.
//   - the CPU: this process's share of all the logical processors, its working set, the processors' real clock (PDH: %
//     Processor Performance of the Processor Frequency), and the CPU time a presented frame takes on the game's update
//     thread and on the presenting one (perf_hint_thread names them). QueryProcessCycleTime and QueryThreadCycleTime
//     count TSC ticks, so against rdtsc they give the cores, or the share of a core, used.
// The CPU's temperature is not here: Windows gives it to no program without a kernel driver of its own.
// Every minute while it runs, a [perf] line in the log.
#include <windows.h>
#include <intrin.h>
#include <math.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../logwriter.h"
#include "../platform.h"
#include "win_stats.h"

#define LOG(...) rt_logf(__VA_ARGS__)
#define PERIOD_MS 1000   // a pass a second
#define STALE_MS 5000    // a snapshot older than this is not shown (the sampler held up in a driver's call)
#define RETRY_MS 10000   // the adapter is opened again after a failure (a GPU reset, a driver update) at most this often
#define GB (1024.0 * 1024 * 1024)

bool vk_device_luid(uint8_t luid[8]);      // (nvn/vulkan/device.c)
extern volatile uint64_t g_present_count;  // (runtime/ui.c: the frames presented)

// ---- D3DKMT, declared from Microsoft's documentation of d3dkmthk.h (MinGW has no such header), as fsr4.c does
typedef UINT KmtHandle;
typedef struct { LUID luid; KmtHandle adapter; } KmtOpen;                          // D3DKMT_OPENADAPTERFROMLUID
typedef struct { KmtHandle adapter; } KmtClose;                                    // D3DKMT_CLOSEADAPTER
typedef struct { KmtHandle adapter; UINT type; void *data; UINT size; } KmtQuery;  // D3DKMT_QUERYADAPTERINFO
typedef struct {  // D3DKMT_ADAPTER_PERFDATA
    UINT32 physical;
    ULONGLONG mem_hz, mem_hz_max, mem_hz_max_oc, mem_bytes, pcie_bytes;
    ULONG fan_rpm, power, temperature;  // (temperature: tenths of a degree C)
    UCHAR power_state_override;
} KmtAdapterPerf;
typedef struct {  // D3DKMT_NODE_PERFDATA
    UINT32 node, physical;
    ULONGLONG hz, hz_max, hz_max_oc;
    ULONG mv, mv_max, mv_max_oc;
    ULONGLONG max_transition_latency;
} KmtNodePerf;
#pragma pack(push, 1)  // (D3DKMT_NODEMETADATA is in a part of d3dkmdt.h packed to 1 byte: 78 bytes; 80 is refused)
typedef struct {
    UINT node;         // NodeOrdinalAndAdapterIndex: the node in the low 16 bits
    UINT engine_type;  // DXGK_ENGINE_TYPE: 1 is 3D
    WCHAR name[32];
    UINT32 flags;
    BOOLEAN gpu_mmu, io_mmu;
} KmtNodeMeta;
#pragma pack(pop)
typedef struct {  // D3DKMT_QUERYVIDEOMEMORYINFO
    HANDLE process;      // (NULL: this one)
    KmtHandle adapter;
    UINT segment_group;  // 0: local, the card's own memory
    UINT64 budget, usage, reservation, available;
    UINT physical;
} KmtVideoMemory;
_Static_assert(sizeof(KmtAdapterPerf) == 64 && sizeof(KmtNodePerf) == 56 && sizeof(KmtNodeMeta) == 78 &&
               sizeof(KmtVideoMemory) == 56, "D3DKMT's structures");
enum { KMT_NODEMETADATA = 25, KMT_NODEPERFDATA = 61, KMT_ADAPTERPERFDATA = 62 };  // KMTQAITYPE_*
static LONG (WINAPI *kmt_open)(KmtOpen *);
static LONG (WINAPI *kmt_close)(KmtClose *);
static LONG (WINAPI *kmt_query)(KmtQuery *);
static LONG (WINAPI *kmt_vidmem)(KmtVideoMemory *);

// ---- PDH, looked up in pdh.dll when Full first asks for it
static struct {
    __typeof__(PdhOpenQueryW) *open;
    __typeof__(PdhAddEnglishCounterW) *add;  // (English names: localized ones differ on a Spanish Windows)
    __typeof__(PdhCollectQueryData) *collect;
    __typeof__(PdhGetFormattedCounterValue) *value;
    __typeof__(PdhGetFormattedCounterArrayW) *array;
    __typeof__(PdhCloseQuery) *close;
} pdh;

// ---- the threads the readout times (perf_hint_thread, platform.h): the latest of each role. They are the frame's own,
// so each is raised above normal too the first time it says so (platform/windows.c plat_thread_priority)
static SRWLOCK role_lock = SRWLOCK_INIT;
static HANDLE role_thread[2];  // PERF_GAME, PERF_RENDER (THREAD_QUERY_LIMITED_INFORMATION)
static uint32_t role_gen[2];   // changed with the thread
static DWORD role_tid[2];

void perf_hint_thread(int role) {
    if (role != PERF_GAME && role != PERF_RENDER) return;
    DWORD tid = GetCurrentThreadId();
    if (__atomic_load_n(&role_tid[role], __ATOMIC_RELAXED) == tid) return;  // (every frame after the first)
    if (plat_thread_priority(PLAT_PRIO_HIGH))
        LOG("[plat] thread %lu, the game's %s, at least above normal priority\n", (unsigned long)tid,
            role == PERF_GAME ? "update thread" : "presenting thread");
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid), old;
    AcquireSRWLockExclusive(&role_lock);
    old = role_thread[role];
    role_thread[role] = h;
    role_gen[role]++;
    __atomic_store_n(&role_tid[role], tid, __ATOMIC_RELAXED);
    ReleaseSRWLockExclusive(&role_lock);
    if (old) CloseHandle(old);
}

void perf_hint_frame(void) { perf_hint_thread(PERF_RENDER); }

// ---- the snapshot the game's thread copies
static SRWLOCK snap_lock = SRWLOCK_INIT;
static WinStats snap;
static uint32_t snap_seq;  // (under snap_lock)
static uint32_t pub_seq;   // the last published one's number, 0: none
static uint64_t pub_ms;    // when (GetTickCount64)

uint32_t winstats_get(WinStats *out, uint32_t have) {
    uint32_t seq = __atomic_load_n(&pub_seq, __ATOMIC_ACQUIRE);
    uint64_t at = __atomic_load_n(&pub_ms, __ATOMIC_RELAXED), now = GetTickCount64();
    if (!seq || (now > at && now - at > STALE_MS)) return 0;
    if (seq == have || !TryAcquireSRWLockShared(&snap_lock)) return have;
    *out = snap;
    seq = snap_seq;
    ReleaseSRWLockShared(&snap_lock);
    return seq;
}

static void publish(const WinStats *s) {
    static uint32_t n;
    AcquireSRWLockExclusive(&snap_lock);
    snap = *s;
    snap_seq = ++n ? n : ++n;  // (never 0)
    uint32_t seq = snap_seq;
    ReleaseSRWLockExclusive(&snap_lock);
    __atomic_store_n(&pub_ms, GetTickCount64(), __ATOMIC_RELAXED);
    __atomic_store_n(&pub_seq, seq, __ATOMIC_RELEASE);
}

// ---- the sampler's own state (its thread only)
static int want;     // the level asked for (winstats_level)
static HANDLE wake;  // set when it changes
static KmtHandle adapter;
static LUID luid;
static UINT node3d;        // the adapter's first 3D engine
static unsigned worked;    // the queries that have answered on this adapter handle (bits: perf data, node, memory)
static uint64_t retry_at;  // no new try to open the adapter before this (GetTickCount64)
static PDH_HQUERY query;
static PDH_HCOUNTER c_gpu, c_perf, c_freq;
static bool pdh_broken, gpu_counter_broken;
static PDH_FMT_COUNTERVALUE_ITEM_W *items;  // (the GPU's engine instances, kept between passes)
static DWORD items_size;
// the last Full pass's readings, which the next one's rates are taken against
static bool base_ok, base_cpu_ok, base_thread_ok[2];
static uint64_t base_qpc, base_cpu, base_tsc, base_frames, base_cycles[2];
static uint32_t base_gen[2];

static bool pdh_valid(DWORD status) { return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA; }
static uint64_t tsc_now(void) {
#if defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#else
    return 0;  // (no thread figures)
#endif
}

static LONG kmt(UINT type, void *data, UINT size) {
    KmtQuery q = {adapter, type, data, size};
    return kmt_query(&q);
}

static void close_adapter(void) {
    if (adapter) {
        KmtClose c = {adapter};
        kmt_close(&c);
    }
    adapter = 0, worked = 0;
}

// The Vulkan device's adapter, by its LUID (a virtual display's adapter can repeat the GPU's perf data under its own,
// with no engines), and its first 3D engine. false: not now (no device yet: the next pass tries again)
static bool open_adapter(void) {
    static bool loaded;
    static int told;
    if (!loaded) {
        loaded = true;
        HMODULE gdi = LoadLibraryW(L"gdi32.dll");  // (a known DLL: always the system's own)
        if (gdi) {
            kmt_open = (void *)GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid");
            kmt_close = (void *)GetProcAddress(gdi, "D3DKMTCloseAdapter");
            kmt_query = (void *)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
            kmt_vidmem = (void *)GetProcAddress(gdi, "D3DKMTQueryVideoMemoryInfo");
        }
        if (!kmt_open || !kmt_close || !kmt_query) LOG("[perf] gdi32 has no D3DKMT: no GPU figures in the readout\n");
    }
    uint8_t id[8];
    if (!kmt_open || !kmt_close || !kmt_query || !vk_device_luid(id)) return false;
    KmtOpen o = {0};
    memcpy(&o.luid, id, sizeof o.luid);
    LONG st = kmt_open(&o);
    if (st) {
        if (told++ < 3)
            LOG("[perf] the GPU's adapter (LUID %08lx:%08lx) could not be opened (0x%08lx): no GPU figures for now\n",
                (unsigned long)o.luid.HighPart, (unsigned long)o.luid.LowPart, (unsigned long)st);
        retry_at = GetTickCount64() + RETRY_MS;
        return false;
    }
    adapter = o.adapter, luid = o.luid, node3d = 0;
    for (UINT n = 0; n < 64; n++) {
        KmtNodeMeta m = {.node = n};
        if (kmt(KMT_NODEMETADATA, &m, sizeof m)) break;
        if (m.engine_type == 1) {
            node3d = n;
            break;
        }
    }
    if (told++ < 3)
        LOG("[perf] the readout's GPU figures: adapter LUID %08lx:%08lx, 3D engine node %u\n", (unsigned long)luid.HighPart,
            (unsigned long)luid.LowPart, node3d);
    return true;
}

// The GPU's figures (Basic: the temperature alone). A query that answered before and fails three passes in a row (a
// driver may refuse one now and then): the adapter is gone (a GPU reset, a driver update), opened again in a while.
static void gpu_pass(WinStats *s, int level) {
    static int streak;
    bool lost = false;
    KmtAdapterPerf pd = {0};
    if (!kmt(KMT_ADAPTERPERFDATA, &pd, sizeof pd)) {
        worked |= 1;
        if (pd.temperature) s->gpu_temp = pd.temperature / 10.0f;
    } else {
        lost |= (worked & 1) != 0;
    }
    if (level >= 2) {
        KmtNodePerf np = {.node = node3d};
        if (!kmt(KMT_NODEPERFDATA, &np, sizeof np)) {
            worked |= 2;
            if (np.hz) s->gpu_ghz = (float)(np.hz / 1e9);
        } else {
            lost |= (worked & 2) != 0;
        }
        KmtVideoMemory vm = {.adapter = adapter};
        if (kmt_vidmem && !kmt_vidmem(&vm)) {
            worked |= 4;
            if (vm.budget) s->vram_gb = (float)(vm.usage / GB), s->vram_budget_gb = (float)(vm.budget / GB);
        } else {
            lost |= (worked & 4) != 0;
        }
    }
    if (!lost) {
        streak = 0;
    } else if (++streak >= 3) {
        LOG("[perf] the GPU's adapter stopped answering: it is opened again in %d s\n", RETRY_MS / 1000);
        close_adapter();
        retry_at = GetTickCount64() + RETRY_MS;
        streak = 0;
    }
}

static void pdh_stop(void) {
    if (query) pdh.close(query);
    query = NULL, c_gpu = c_perf = c_freq = NULL;
    free(items);
    items = NULL, items_size = 0;
}

// The GPU's engines, of the adapter's LUID only (about 400 instances of 500 here), once it is open
static void pdh_add_gpu(void) {
    if (c_gpu || !adapter || gpu_counter_broken) return;
    wchar_t path[128];
    swprintf(path, 128, L"\\GPU Engine(*luid_0x%08lX_0x%08lX*)\\Utilization Percentage", (unsigned long)luid.HighPart,
             (unsigned long)luid.LowPart);
    if (pdh.add(query, path, 0, &c_gpu) != ERROR_SUCCESS) {
        c_gpu = NULL, gpu_counter_broken = true;
        LOG("[perf] PDH has no GPU Engine counters: no GPU use in the readout\n");
    }
}

// PDH's query: the processors' clock and the GPU's engines (or those once the adapter opens: pdh_pass). The first one
// in the process takes about 0.7 s (PDH's own start-up).
static void pdh_start(void) {
    static bool loaded;
    if (!loaded) {
        loaded = true;
        HMODULE dll = LoadLibraryExW(L"pdh.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (dll) {
            pdh.open = (void *)GetProcAddress(dll, "PdhOpenQueryW");
            pdh.add = (void *)GetProcAddress(dll, "PdhAddEnglishCounterW");
            pdh.collect = (void *)GetProcAddress(dll, "PdhCollectQueryData");
            pdh.value = (void *)GetProcAddress(dll, "PdhGetFormattedCounterValue");
            pdh.array = (void *)GetProcAddress(dll, "PdhGetFormattedCounterArrayW");
            pdh.close = (void *)GetProcAddress(dll, "PdhCloseQuery");
        }
        pdh_broken = !pdh.open || !pdh.add || !pdh.collect || !pdh.value || !pdh.array || !pdh.close;
        if (pdh_broken) LOG("[perf] pdh.dll is not usable: no GPU use or CPU clock in the readout\n");
    }
    if (pdh_broken) return;
    uint64_t t0 = GetTickCount64();
    if (pdh.open(NULL, 0, &query) != ERROR_SUCCESS) {
        query = NULL, pdh_broken = true;
        LOG("[perf] PDH gave no query: no GPU use or CPU clock in the readout\n");
        return;
    }
    if (pdh.add(query, L"\\Processor Information(_Total)\\% Processor Performance", 0, &c_perf) != ERROR_SUCCESS ||
        pdh.add(query, L"\\Processor Information(_Total)\\Processor Frequency", 0, &c_freq) != ERROR_SUCCESS)
        c_perf = c_freq = NULL;
    pdh_add_gpu();
    pdh.collect(query);  // (rates take two collections: the next pass has them)
    static bool told;
    if (!told)
        LOG("[perf] PDH ready in %llu ms (the processors' clock: %s; the GPU's engines: %s)\n",
            (unsigned long long)(GetTickCount64() - t0), c_perf ? "yes" : "no", c_gpu ? "yes" : "not yet");
    told = true;
}

// The adapter's busiest engine (Task Manager's GPU figure): each engine's use summed over the processes using it
static float busiest_engine(void) {
    const DWORD fmt = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
    DWORD size = items_size, n = 0;
    PDH_STATUS st = pdh.array(c_gpu, fmt, &size, &n, items);
    if (st == (PDH_STATUS)PDH_MORE_DATA) {  // (the size asked again with none: a short buffer's answer is not reliable)
        size = 0;
        if (pdh.array(c_gpu, fmt, &size, &n, NULL) != (PDH_STATUS)PDH_MORE_DATA) return NAN;
        free(items);
        items_size = size + 16384;  // (room for processes that start meanwhile)
        items = malloc(items_size);
        if (!items) {
            items_size = 0;
            return NAN;
        }
        size = items_size;
        st = pdh.array(c_gpu, fmt, &size, &n, items);
    }
    if (st != ERROR_SUCCESS) return NAN;
    // "pid_<pid>_luid_0x<high>_0x<low>_phys_<n>_eng_<n>_engtype_<type>": by physical adapter and engine
    static double use[4][64];
    memset(use, 0, sizeof use);
    for (DWORD i = 0; i < n; i++) {
        if (!pdh_valid(items[i].FmtValue.CStatus)) continue;
        const wchar_t *p = wcsstr(items[i].szName, L"_phys_"), *e = wcsstr(items[i].szName, L"_eng_");
        unsigned long pi = p ? wcstoul(p + 6, NULL, 10) : 0, ei = e ? wcstoul(e + 5, NULL, 10) : 64;
        if (pi < 4 && ei < 64) use[pi][ei] += items[i].FmtValue.doubleValue;
    }
    double best = 0;
    for (int p = 0; p < 4; p++)
        for (int e = 0; e < 64; e++)
            if (use[p][e] > best) best = use[p][e];
    return (float)(best < 100 ? best : 100);
}

static void pdh_pass(WinStats *s) {
    pdh_add_gpu();
    if (pdh.collect(query) != ERROR_SUCCESS) return;
    if (c_gpu) s->gpu_use = busiest_engine();
    PDH_FMT_COUNTERVALUE perf, freq;
    if (c_perf && pdh.value(c_perf, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, NULL, &perf) == ERROR_SUCCESS &&
        pdh.value(c_freq, PDH_FMT_DOUBLE, NULL, &freq) == ERROR_SUCCESS && pdh_valid(perf.CStatus) &&
        pdh_valid(freq.CStatus) && perf.doubleValue > 0 && freq.doubleValue > 0)
        s->cpu_ghz = (float)(perf.doubleValue / 100 * freq.doubleValue / 1000);
}

// This process's CPU and memory, and its game and render threads' time per frame, since the last Full pass
static void process_pass(WinStats *s) {
    static DWORD ncpu;
    if (!ncpu) ncpu = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    PROCESS_MEMORY_COUNTERS m = {.cb = sizeof m};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &m, sizeof m)) s->ram_gb = (float)(m.WorkingSetSize / GB);
    LARGE_INTEGER qf, qc;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&qc);
    uint64_t frames = g_present_count, tsc = tsc_now();
    // (the process's cycles rather than GetProcessTimes, which charges whole timer ticks to whoever runs at the tick:
    // threads that wake, work and wait between two ticks went missing, and a second read 0% or 28%)
    ULONG64 cpu = 0;
    bool cpu_ok = tsc && QueryProcessCycleTime(GetCurrentProcess(), &cpu);
    ULONG64 cycles[2] = {0, 0};
    uint32_t gen[2];
    bool thread_ok[2];
    AcquireSRWLockShared(&role_lock);
    for (int r = 0; r < 2; r++) {
        gen[r] = role_gen[r];
        thread_ok[r] = tsc && role_thread[r] && QueryThreadCycleTime(role_thread[r], &cycles[r]);
    }
    ReleaseSRWLockShared(&role_lock);
    if (base_ok && tsc > base_tsc) {
        double wall = (double)(qc.QuadPart - (LONGLONG)base_qpc) / (double)qf.QuadPart;
        uint64_t df = frames - base_frames;
        if (cpu_ok && base_cpu_ok && ncpu) {
            double use = (double)(cpu - base_cpu) / (double)(tsc - base_tsc) / ncpu * 100;
            s->cpu_use = (float)(use < 100 ? use : 100);
        }
        for (int r = 0; r < 2; r++) {
            if (!thread_ok[r] || !base_thread_ok[r] || gen[r] != base_gen[r]) continue;
            double busy = (double)(cycles[r] - base_cycles[r]) / (double)(tsc - base_tsc);
            if (busy > 1.05) continue;  // (not TSC ticks after all: left out)
            if (busy > 1) busy = 1;
            *(r == PERF_GAME ? &s->game_busy : &s->render_busy) = (float)busy;
            if (df) *(r == PERF_GAME ? &s->game_ms : &s->render_ms) = (float)(busy * wall * 1e3 / (double)df);
        }
    }
    base_ok = true, base_cpu_ok = cpu_ok;
    base_qpc = (uint64_t)qc.QuadPart, base_cpu = cpu, base_tsc = tsc, base_frames = frames;
    for (int r = 0; r < 2; r++) base_thread_ok[r] = thread_ok[r], base_gen[r] = gen[r], base_cycles[r] = cycles[r];
}

// A [perf] line every minute while the readout shows (the first some seconds in, once the rates are known)
static uint64_t next_log;
static void item(char *b, size_t size, const char *fmt, ...) {  // (appended to b, after a comma when b has some)
    size_t n = strlen(b);
    if (n && n + 2 < size) memcpy(b + n, ", ", 3), n += 2;
    va_list ap;
    va_start(ap, fmt);
    if (n < size) vsnprintf(b + n, size - n, fmt, ap);
    va_end(ap);
}
static void log_pass(const WinStats *s, int level) {
    uint64_t now = GetTickCount64();
    if (now < next_log) return;
    next_log = now + 60000;
    char gpu[160] = "", cpu[224] = "";
    if (isfinite(s->gpu_temp)) item(gpu, sizeof gpu, "%.0f C", s->gpu_temp);
    if (level >= 2) {
        if (isfinite(s->gpu_use)) item(gpu, sizeof gpu, "%.0f%% use", s->gpu_use);
        if (isfinite(s->gpu_ghz)) item(gpu, sizeof gpu, "%.2f GHz", s->gpu_ghz);
        if (isfinite(s->vram_gb)) item(gpu, sizeof gpu, "video memory %.2f of %.1f GB", s->vram_gb, s->vram_budget_gb);
        if (isfinite(s->cpu_use)) item(cpu, sizeof cpu, "%.1f%%", s->cpu_use);
        if (isfinite(s->cpu_ghz)) item(cpu, sizeof cpu, "%.2f GHz", s->cpu_ghz);
        if (isfinite(s->ram_gb)) item(cpu, sizeof cpu, "working set %.2f GB", s->ram_gb);
        if (isfinite(s->game_ms))
            item(cpu, sizeof cpu, "game thread %.2f ms a frame (%.0f%% busy)", s->game_ms, s->game_busy * 100);
        if (isfinite(s->render_ms))
            item(cpu, sizeof cpu, "render thread %.2f ms a frame (%.0f%% busy)", s->render_ms, s->render_busy * 100);
    }
    if (level >= 2) LOG("[perf] GPU: %s; CPU: %s\n", *gpu ? gpu : "-", *cpu ? cpu : "-");
    else if (*gpu) LOG("[perf] GPU: %s\n", gpu);
}

static DWORD WINAPI sampler(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    static const WinStats UNKNOWN = {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};
    _Static_assert(sizeof(WinStats) == 12 * sizeof(float), "every figure unknown to begin with");
    int was = 0;
    for (;;) {
        int level = __atomic_load_n(&want, __ATOMIC_ACQUIRE);
        if (level != was) {  // (the rates start over; what the new level doesn't need is let go)
            base_ok = false;
            if (level < 2) pdh_stop();
            if (!level) {
                close_adapter();
                __atomic_store_n(&pub_seq, 0, __ATOMIC_RELEASE);
            }
            next_log = GetTickCount64() + 10000;
            was = level;
        }
        if (!level) {
            WaitForSingleObject(wake, INFINITE);
            continue;
        }
        WinStats s = UNKNOWN;
        if (!adapter && GetTickCount64() >= retry_at) open_adapter();
        if (adapter) gpu_pass(&s, level);
        if (level >= 2) {
            if (!query && !pdh_broken) pdh_start();  // (its rates come with the next pass)
            else if (query) pdh_pass(&s);
            process_pass(&s);
        }
        publish(&s);
        log_pass(&s, level);
        WaitForSingleObject(wake, PERIOD_MS);  // (a new level wakes it at once)
    }
    return 0;
}

void winstats_level(int level) {
    static int last;  // (the host main thread's only)
    static HANDLE thread;
    if (level < 0 || level > 2) level = level > 0 ? 2 : 0;
    if (level == last) return;
    last = level;
    __atomic_store_n(&want, level, __ATOMIC_RELEASE);
    if (thread) {
        SetEvent(wake);  // (acted on at once, not at the end of the second)
    } else if (level) {  // (started the first time the readout is on, and reads the level itself; it idles while off)
        if (!wake) wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (wake) thread = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
        if (!thread) LOG("[perf] the readout's sampler could not start (%lu): no PC figures\n", GetLastError());
    }
}
