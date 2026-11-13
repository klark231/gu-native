// MHGU_PERF_LIMITER=1 (on by default; 0 the old way): the engine's frame limiter sleeps until its frame's time has nearly come,
// instead of sleeping to a millisecond or two before it and reading the clock in a loop for the rest.
//
// The limiter (main 0xda5824, in the path the port's frame rate target drives: its object's +0x40 is 0, runtime/hle/vi.c)
// takes the frame's step from the system tick. With 2 ms or more of the frame period left it sleeps (floor(ms) - 1) ms
// (nn::os::SleepThread, called at 0xda5d28); then, and at once with less than 2 ms left, it reads the tick in a loop
// (GetSystemTick, called at 0xda5d4c) until the frame's step is reached: 1 to 2 ms a frame of a big core spent on
// nothing, on the game's update thread, which also presents and is in the ADPF session (its CPU time is the work reported
// there, runtime/platform/android_perf.c: the loop's turns count as work). Here the limiter's sleep, and its first read
// of the tick in a frame where it does not sleep, sleep until the frame's time less a margin (how late sleeps wake up,
// measured, plus 0.1 ms: at least 0.2 ms), and the loop turns for that margin only, as the MH4U port's limiter_sleep does
// (0.7 ms -> 0.02 ms a frame there). The step is still reached on the game's own clock in its own loop: the frame rate and
// the game's time are as before, only the loop's turns are fewer. The time left is the limiter's own sum, from its
// registers at the call (s16 the step so far, s20 the frame's step, s22 the steps a second): its sleep is taken over only
// when the milliseconds that sum gives are the ones the game asked for, and never for more than those plus 2 ms.
//   MHGU_LIMITER_LOG=1  (debug) every 5 s: the frames, how long they slept and how long the loop turned after (the switch
//                       off: the game's own sleep and loop), how late the sleeps woke up, the other tick reads
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hitchlog.h"
#include "../hle.h"
#include "../mods.h"

#define FN_SLEEP 0x15efdb8u  // nn::os::SleepThread(TimeSpan: ns in r0:r1), main's import stub
#define FN_TICK  0xa3ec90u   // GetSystemTick() -> r0:r1 (the game's wrapper of nn::os::GetSystemTick)
#define LR_START 0xda587cu   // the limiter's first tick read: a frame's limiter starts
#define LR_SLEEP 0xda5d2cu   // its sleep, with 2 ms or more left
#define LR_SPIN  0xda5d50u   // its loop's tick read
#define MIN_SLEEP_NS 300000ull  // (less than this left past the margin: the loop does it, as before)

extern __thread uint64_t g_blocked_ns;  // (runtime/kernel.c: the time a thread spent blocked, as its SleepThread counts it)

static bool g_on, g_log;
// The frame in the limiter: only the thread at LR_START touches these (the game's update thread)
static struct {
    Cpu *cpu;                // the thread whose limiter runs (others' tick reads go straight through)
    bool slept;              // this frame slept (the game or here): the loop's first turn has the step from before it
    uint64_t start, wake;    // when the limiter started; when its sleep ended (0: none)
    uint64_t spin0, spin1;   // the loop's first and last tick reads (0: none)
    uint64_t slept_ns;       // its sleep
    bool ours;               // that sleep was this file's
    uint64_t late, late_hi;  // how late sleeps woke up: a slow average; the latest that sinks back slowly (1/64 a sleep)
} L = {.late = 300000, .late_hi = 300000};
static struct {
    uint64_t t0, frames, sleeps, ours, slept, spin, spin_max, late, late_n, over, over_ns;
    _Atomic uint64_t others;
} S;

// How much sooner than the frame's time a sleep ends: the loop turns for it. Past the latest wake-ups seen lately, not
// only their average: a sleep that ends after the frame's time makes the frame late, and the next one starts from there
// (a desktop's timer, waking 0.2 ms late on average and more now and then, made 144 FPS 141 with the average alone)
static uint64_t margin(void) {
    uint64_t a = L.late + 100000, b = L.late_hi + 50000, m = a > b ? a : b;
    return m < 200000 ? 200000 : m > 2000000 ? 2000000 : m;
}

// The time left in the frame, as the limiter works it out (0xda5cf8..0xda5d10): (1000 / s22) * (s20 - s16) ms
static float left_ms(const Cpu *c) {
    float k = 1000.0f / c->v.f[22], d = c->v.f[20] - c->v.f[16];
    return k * d;
}

// (deadline: the frame's time, as the limiter's sum gives it)
static void sleep_until(Cpu *c, uint64_t until, uint64_t deadline) {
    uint64_t t0 = plat_now_ns();
    plat_sleep_until_ns(until);
    uint64_t t1 = plat_now_ns(), late = t1 > until ? t1 - until : 0;
    if (late > 3000000) late = 3000000;
    L.late = (L.late * 15 + late) / 16;
    L.late_hi = late > L.late_hi ? late : L.late_hi - L.late_hi / 64;
    L.wake = t1, L.slept_ns = t1 - t0, L.ours = true;
    g_blocked_ns += t1 - t0;  // (as the kernel's SleepThread)
    if (HITCH_ON) hitch_kwait(t1 - t0, 5, c);
    if (g_log) {
        S.late += late, S.late_n++;
        if (t1 > deadline) S.over++, S.over_ns += t1 - deadline;
    }
}

// (MHGU_LIMITER_LOG) the frame that ended: its sleep and the loop's turns after it
static void frame_done(void) {
    uint64_t now = plat_now_ns();
    if (!S.t0) S.t0 = now;
    if (L.start) {
        S.frames++;
        if (L.wake) S.sleeps++, S.slept += L.slept_ns, S.ours += L.ours;
        uint64_t from = L.wake ? L.wake : L.spin0, spin = L.spin1 > from && from ? L.spin1 - from : 0;
        S.spin += spin;
        if (spin > S.spin_max) S.spin_max = spin;
    }
    if (now - S.t0 < 5000000000ull || !S.frames) return;
    double s = (now - S.t0) / 1e9;
    LOG("[limiter] %.1f s: %llu frames, %llu slept (%llu here) %.2f ms on average, then the loop turned %.3f ms on average "
        "(longest %.3f); sleeps woke %.0f us late on average, %llu past the frame's time (%.0f us on average), margin %.0f us; "
        "%.0f other tick reads a frame (%s)\n",
        s, (unsigned long long)S.frames, (unsigned long long)S.sleeps, (unsigned long long)S.ours,
        S.sleeps ? S.slept / 1e6 / S.sleeps : 0.0, S.spin / 1e6 / S.frames, S.spin_max / 1e6,
        S.late_n ? S.late / 1e3 / S.late_n : 0.0, (unsigned long long)S.over, S.over ? S.over_ns / 1e3 / S.over : 0.0,
        margin() / 1e3, (double)atomic_load(&S.others) / S.frames,
        g_on ? "MHGU_PERF_LIMITER on" : "off: the game's own");
    memset(&S, 0, sizeof S);  // (others: an atomic of 8 bytes, zero is zero)
    S.t0 = now;
}

static void tick_hook(Cpu *c) {
    uint32_t lr = c->r[14];
    if (lr == LR_START) {  // (a frame's limiter: its state starts over)
        if (g_log) frame_done();
        __atomic_store_n(&L.cpu, c, __ATOMIC_RELAXED);
        L.slept = L.ours = false, L.start = plat_now_ns(), L.wake = L.spin0 = L.spin1 = L.slept_ns = 0;
    } else if (lr == LR_SPIN && c == __atomic_load_n(&L.cpu, __ATOMIC_RELAXED)) {
        uint64_t now = plat_now_ns();
        if (!L.spin0) L.spin0 = now;
        L.spin1 = now;
        if (g_on && !L.slept) {
            // (no sleep yet this frame: under 2 ms left. Once, here: after it each turn's step is a fresh one, and short
            // of the margin)
            L.slept = true;
            float ms = left_ms(c);
            uint64_t left = ms > 0.0f && ms < 2.0f ? (uint64_t)((double)ms * 1e6) : 0, m = margin();
            if (left > m + MIN_SLEEP_NS) sleep_until(c, now + left - m, now + left);
        }
    } else if (g_log) {
        atomic_fetch_add_explicit(&S.others, 1, memory_order_relaxed);
    }
    fnhook_call_original(c, FN_TICK);
}

static void sleep_hook(Cpu *c) {
    if (c->r[14] != LR_SLEEP || c != __atomic_load_n(&L.cpu, __ATOMIC_RELAXED)) {
        fnhook_call_original(c, FN_SLEEP);
        return;
    }
    L.slept = true;  // (the loop's first turn reads the step from before the sleep: nothing is decided on it)
    uint64_t asked = (uint64_t)c->r[1] << 32 | c->r[0];
    float ms = left_ms(c);
    if (g_on && ms >= 2.0f && ms < 1e6f && (uint64_t)((uint32_t)ms - 1u) * 1000000u == asked) {
        uint64_t now = plat_now_ns(), left = (uint64_t)((double)ms * 1e6), m = margin(), want = left > m ? left - m : 0;
        if (want > asked + 2000000) want = asked + 2000000;
        if (want < asked) want = asked;  // (never shorter than the game's own: a margin over 1 ms)
        sleep_until(c, now + want, now + left);
        return;
    }
    uint64_t t0 = plat_now_ns();  // (the game's own sleep: the switch off, or a sum that is not this one's)
    fnhook_call_original(c, FN_SLEEP);
    L.wake = plat_now_ns(), L.slept_ns = L.wake - t0, L.ours = false;
    // (MHGU_LIMITER_LOG, the switch off: how often the game's own sleep ends past the frame's time, to set against this
    // file's)
    if (g_log && ms >= 2.0f && ms < 1e6f && L.wake > t0 + (uint64_t)((double)ms * 1e6))
        S.over++, S.over_ns += L.wake - t0 - (uint64_t)((double)ms * 1e6);
}

void limiter_init(void) {
    const char *e = getenv("MHGU_PERF_LIMITER");
    g_on = e ? atoi(e) > 0 : true;  // (unset: on)
    e = getenv("MHGU_LIMITER_LOG");
    g_log = e && *e == '1';
    if (!g_on && !g_log) return;  // (nothing hooked: the game's limiter as it is)
    bool ok = fnhook_set(FN_TICK, hle_register("limiter:GetSystemTick", tick_hook)) &&
              fnhook_set(FN_SLEEP, hle_register("limiter:SleepThread", sleep_hook));
    if (!ok) {
        fnhook_set(FN_TICK, 0);
        g_on = g_log = false;
    }
    LOG("[limiter] %s\n", !ok   ? "the frame limiter's calls could not be hooked: it runs as it is"
                          : g_on ? "MHGU_PERF_LIMITER: the frame limiter sleeps until its frame's time, less a margin"
                                 : "watched (MHGU_LIMITER_LOG), as it is");
}
