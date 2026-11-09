// The log's writer (Windows). A line logged (rt.h LOG, rt_logf) is formatted by the thread that logs and copied into a
// 1 MB ring; a thread of its own, below normal priority, writes what is in the ring with one write. Before, every line
// was an fprintf to the unbuffered stderr, which the C runtime writes to a file or a pipe a character or a few at a
// time (measured: about 150 us a line, against 4 for one write of it), and any of those writes can wait for the disk:
// in a hunt with the log on a hard disk, lines took up to 28 ms on the game's update thread and 6 to 9 ms on the
// window's (MHGU_HITCHLOG). Now a line costs the thread that logs a few microseconds (log), and the disk's waits are
// the writer's (log-write). A full ring drops lines rather than wait, and the log says how many and where. FATAL, a
// crash and exit write out what is queued first (rt_log_flush), and every line after that goes straight out.
// MHGU_LOG_ASYNC=0: each line written by the thread that logs (for comparisons).
// Elsewhere stderr is used directly, as before (Android: a pipe that a thread of its own reads, platform/android.c).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "hitchlog.h"
#include "logwriter.h"
#include "platform.h"

#ifndef _WIN32
int rt_vlogf(const char *fmt, va_list ap) { return vfprintf(stderr, fmt, ap); }
int rt_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stderr, fmt, ap);
    va_end(ap);
    return r;
}
void rt_log_write(const char *s, size_t n) { fwrite(s, 1, n, stderr); }
void rt_log_start(void) {}
void rt_log_flush(void) { fflush(stderr); }
#else
#include <windows.h>
#include <io.h>
#include <process.h>
#include <stdatomic.h>
#include <string.h>

#define RING (1u << 20)
static char g_ring[RING];
// Bytes ever queued and ever written: the ring holds [tail, head), at position % RING. The threads that queue take
// g_put among themselves, and only read the tail; whoever writes the ring out (the writer, or rt_log_flush) holds
// g_out, and only reads the head. So the writer, a low priority thread, never holds a lock the game's threads wait for.
static _Atomic uint64_t g_head, g_tail;
static SRWLOCK g_put = SRWLOCK_INIT, g_out = SRWLOCK_INIT;
static uint64_t g_lost, g_lost_bytes;  // lines dropped (the ring full) since the last one that got in (under g_put)
static _Atomic int g_mode;             // 0 lines go straight out (not started), 1 queued, 2 straight out (flushed for good)
static _Atomic int g_idle;             // the writer waits for g_wake
static HANDLE g_wake, g_dest, g_writer;  // (an auto-reset event; stderr's handle; the writer)
static DWORD g_writer_id;

// [from, to) of the ring to stderr, by its holder of g_out
static void write_out(uint64_t from, uint64_t to) {
    uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
    for (uint64_t at = from; at < to;) {
        uint32_t o = (uint32_t)(at % RING);
        DWORD n = (DWORD)(to - at < RING - o ? to - at : RING - o), done = 0;
        if (!WriteFile(g_dest, g_ring + o, n, &done, NULL) || !done) break;  // (a pipe nobody reads any more: dropped)
        at += done;
    }
    if (HITCH_ON) hitch_add(HC_LOG_WRITE, plat_now_ns() - t0, to - from);  // ((debug) MHGU_HITCHLOG)
}

static unsigned __stdcall writer(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BACKGROUND);
    for (;;) {
        uint64_t t = atomic_load_explicit(&g_tail, memory_order_relaxed);
        if (atomic_load(&g_head) == t) {
            atomic_store(&g_idle, 1);  // (then the head again: a line queued in between is not missed, see put)
            if (atomic_load(&g_head) == t) WaitForSingleObject(g_wake, INFINITE);
            atomic_store(&g_idle, 0);
            continue;
        }
        AcquireSRWLockExclusive(&g_out);
        uint64_t from = atomic_load_explicit(&g_tail, memory_order_relaxed), to = atomic_load(&g_head);
        if (to != from) write_out(from, to);
        atomic_store_explicit(&g_tail, to, memory_order_release);
        ReleaseSRWLockExclusive(&g_out);
    }
    return 0;
}

static void copy_in(uint64_t at, const char *s, size_t n) {
    uint32_t o = (uint32_t)(at % RING);
    size_t first = n < RING - o ? n : RING - o;
    memcpy(g_ring + o, s, first);
    memcpy(g_ring, s + first, n - first);
}

static int lost_line(char *b, size_t size) {
    return snprintf(b, size, "[log] %llu lines (%llu KB) left out here: the log was writing slower than lines came\n",
                    (unsigned long long)g_lost, (unsigned long long)(g_lost_bytes + 1023) / 1024);
}

static void put(const char *s, size_t n) {
    char note[128];
    AcquireSRWLockExclusive(&g_put);
    uint64_t h = atomic_load_explicit(&g_head, memory_order_relaxed);
    uint64_t room = RING - (h - atomic_load_explicit(&g_tail, memory_order_acquire));
    size_t k = g_lost && n <= room ? (size_t)lost_line(note, sizeof note) : 0;  // (formatted only when it may fit)
    if (n + k > room) {
        g_lost++, g_lost_bytes += n;
        ReleaseSRWLockExclusive(&g_put);
        if (HITCH_ON) hitch_add(HC_LOG_DROP, 0, n);
        return;
    }
    if (k) copy_in(h, note, k), h += k, g_lost = g_lost_bytes = 0;
    copy_in(h, s, n);
    atomic_store(&g_head, h + n);
    ReleaseSRWLockExclusive(&g_put);
    if (atomic_exchange(&g_idle, 0)) SetEvent(g_wake);
}

static void emit(const char *s, size_t n) {
    if (atomic_load_explicit(&g_mode, memory_order_acquire) == 1) put(s, n);
    else fwrite(s, 1, n, stderr);
}

int rt_vlogf(const char *fmt, va_list ap) {
    uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
    char buf[2048], *p = buf;
    va_list again;
    va_copy(again, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n >= (int)sizeof buf) {  // (a long one: a buffer of its own, else its start)
        p = malloc((size_t)n + 1);
        if (p) vsnprintf(p, (size_t)n + 1, fmt, again);
        else p = buf, n = (int)sizeof buf - 1;
    }
    va_end(again);
    if (n > 0) emit(p, (size_t)n);
    if (p != buf) free(p);
    if (HITCH_ON) hitch_add(HC_LOG, plat_now_ns() - t0, 0);  // ((debug) MHGU_HITCHLOG: the logging thread's own time)
    return n;
}

int rt_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = rt_vlogf(fmt, ap);
    va_end(ap);
    return r;
}

void rt_log_write(const char *s, size_t n) {
    if (n) emit(s, n);
}

void rt_log_start(void) {
    const char *e = getenv("MHGU_LOG_ASYNC");
    if (e && *e == '0') return;
    int fd = _fileno(stderr);
    HANDLE h = fd >= 0 ? (HANDLE)_get_osfhandle(fd) : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE || !h || h == (HANDLE)(intptr_t)-2) return;  // (no log anywhere; -2: a GUI program's)
    g_dest = h;
    if (!(g_wake = CreateEventW(NULL, FALSE, FALSE, NULL))) return;
    unsigned id = 0;
    uintptr_t th = _beginthreadex(NULL, 0, writer, NULL, 0, &id);
    if (!th) return;
    g_writer = (HANDLE)th, g_writer_id = id;
    int off = 0;
    if (atomic_compare_exchange_strong(&g_mode, &off, 1)) atexit(rt_log_flush);  // (else flushed already: lines go out)
}

void rt_log_flush(void) {
    if (!atomic_exchange(&g_mode, 2)) { fflush(stderr); return; }  // (never queued)
    // The write in progress ends first: the writer is raised to finish it. It may never end (a pipe nobody reads) or be
    // this thread's (a crash while writing): then on without it after 2 s, or at once.
    bool held = false;
    if (GetCurrentThreadId() != g_writer_id) {
        SetThreadPriority(g_writer, THREAD_PRIORITY_HIGHEST);
        for (int i = 0; i < 2000 && !(held = TryAcquireSRWLockExclusive(&g_out)); i++) Sleep(1);
    }
    uint64_t from = atomic_load_explicit(&g_tail, memory_order_relaxed), to = atomic_load(&g_head);
    if (to != from) write_out(from, to);
    atomic_store_explicit(&g_tail, to, memory_order_release);
    if (held) ReleaseSRWLockExclusive(&g_out);
    bool put_held = false;  // (lines dropped at the end, with no line after them to say so)
    for (int i = 0; i < 100 && !(put_held = TryAcquireSRWLockExclusive(&g_put)); i++) Sleep(1);
    if (put_held) {
        char note[128];
        int k = g_lost ? lost_line(note, sizeof note) : 0;
        g_lost = g_lost_bytes = 0;
        ReleaseSRWLockExclusive(&g_put);
        if (k > 0) fwrite(note, 1, (size_t)k, stderr);
    }
    fflush(stderr);
}
#endif
