// Presents timed to the display's vsyncs (Android, VK_GOOGLE_display_timing), for a game held to an FPS limit that is a
// whole fraction of the refresh rate (30 on 60 Hz; 40 or 30 on 120 Hz). Queued as soon as drawn, its frames landed on
// either side of a vsync as their times varied, and were shown for 16 and 50 ms in turn instead of 33: an Anbernic
// RG406V at Low-end 50 % and 30 FPS had 7.7 % of its intervals a vsync short or long, 1% low 20 fps, p99 50 ms. The rule
// is MH4U for Android's (app_native/present_pacing.cpp, EGL there; its research/pacing.md section 5):
//  - such a limit presents FIFO, so that no picture replaces one still waiting for its time (MAILBOX would);
//  - a new frame's slot is the last one's plus the limit's vsyncs (n), or, if that is sooner than the margin from now,
//    the first vsync that is not (that frame is shown a vsync late, once); after a hitch (a frame more than a period
//    late) the cadence starts again a vsync after it, so the frames after it don't keep the hitch's lateness;
//  - the margin is what SurfaceFlinger needs: the display's presentation deadline (Display.getPresentationDeadlineNanos,
//    GameActivity.presentationDeadlineNs) and 4 ms for the window's own pass, within half a period and three periods
//    (two periods if it is unknown); a frame shown a vsync later than the others a second time within 30 s raises it by
//    2 ms for good (up to three periods);
//  - frames that keep coming a whole period early (the game's clock a little ahead of the display's: its limiter and
//    vsync event run on the phone's own timers) move the slots a period earlier once they have for some seconds (2 s,
//    doubled each time it happens again within 10 s, up to 32 s), or they would wait longer and longer for their slots;
//  - the time given is half a period before the slot's vsync: SurfaceFlinger shows a buffer at the first vsync not
//    before its time, so the grid may be off by up to that much.
// The display's vsyncs are the times past presents reached the screen (vkGetPastPresentationTimingGOOGLE, read every half
// second: each finished present costs Android's loader one call to SurfaceFlinger, so not every frame), and its period
// is measured from them: vkGetRefreshCycleDurationGOOGLE (asked every 2 s) said 16.68 ms on an Anbernic RG406V whose
// vsyncs come every 16.667, and slots chained on it drifted half a period in 15 s, a frame shown a vsync late each time.
// So every slot is also put on the grid of the newest vsync seen. A picture presented again (the settings menu holding the game)
// takes the latest slot. Nothing changes at the refresh rate itself, at a limit that is no whole fraction of it, or with
// VSync Off or MHGU_VK_PRESENT / MHGU_VSYNC given.
//   MHGU_PERF_PRESENT_TIME=1   on (read once at start; the default; 0 the old way)
//   MHGU_PRESENT_MARGIN=<us>   a fixed margin instead (tests)
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"

static struct {
    int on;  // -1: the switch not read yet
    bool ext;  // VK_GOOGLE_display_timing enabled on the device
    PFN_vkGetRefreshCycleDurationGOOGLE refresh;
    PFN_vkGetPastPresentationTimingGOOGLE past;
    VkSwapchainKHR sc;  // the swapchain the times below belong to
    int64_t asked, period_at;   // the period vkGetRefreshCycleDurationGOOGLE gives (ns; 0: not known), and when it was asked
    int64_t period;             // the one in use: measured from the vsyncs seen once they span 2 s, else the one asked
    int64_t ref;                // the vsync the period is measured from
    int64_t vsync, past_at;     // a vsync's time (a past present's), and when past presents were read
    uint32_t id;                // the latest presentID given (never 0)
    struct { uint32_t id; int64_t slot, period; } sent[32];  // the presents given a time lately
    int64_t margin, margin_period, margin_at, last_late;
    int64_t after[32];  // the latest frames' (shown - slot)
    int n_after;
    int64_t last_slot, last_frame_slot, run_since, moved_at, window;  // the cadence
    bool anew;
    int told_n;
    int64_t told_period, told_at;
    int told_lines;
} pp = {.on = -1, .window = 2000000000};

static bool switch_on(void) {
    if (pp.on < 0) {
        const char *e = getenv("MHGU_PERF_PRESENT_TIME");
        pp.on = e ? *e == '1' : 1;  // (unset: on)
    }
    return pp.on;
}

// (create_device) the device extension, if the switch is on and the system's loader has it (Android's has since 7.0)
bool vk_pacing_device_ext(const VkExtensionProperties *ep, uint32_t ne) {
    if (!switch_on()) return false;
    for (uint32_t i = 0; i < ne; i++)
        if (!strcmp(ep[i].extensionName, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME)) return pp.ext = true;
    LOG("[vk] present timing: the system has no %s\n", VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
    return false;
}

// (create_device, once the device is made)
void vk_pacing_device(void) {
    if (!pp.ext) return;
    pp.refresh = (PFN_vkGetRefreshCycleDurationGOOGLE)vkGetDeviceProcAddr(g_vk.dev, "vkGetRefreshCycleDurationGOOGLE");
    pp.past = (PFN_vkGetPastPresentationTimingGOOGLE)vkGetDeviceProcAddr(g_vk.dev, "vkGetPastPresentationTimingGOOGLE");
    if (!pp.refresh || !pp.past) pp.ext = false;
    LOG("[vk] present timing: %s (MHGU_PERF_PRESENT_TIME)\n", pp.ext ? "on, at a whole fraction of the refresh rate" : "its functions are missing: off");
}

static void ask_period(int64_t now) {
    VkRefreshCycleDurationGOOGLE rc = {0};
    pp.period_at = now;
    if (!pp.sc || pp.sc != g_vk.swapchain) return;  // (that swapchain is gone: the app in the background)
    if (pp.refresh(g_vk.dev, pp.sc, &rc) != VK_SUCCESS || rc.refreshDuration < 2000000 || rc.refreshDuration > 50000000)
        return;  // (between 20 and 500 Hz)
    int64_t a = (int64_t)rc.refreshDuration;
    if (!pp.asked || llabs(a - pp.asked) > pp.asked / 100) pp.period = a, pp.ref = 0;  // (another rate: measured anew)
    pp.asked = a;
}

// The grid's vsync nearest t (the newest vsync seen and the period)
static int64_t on_grid(int64_t t) {
    int64_t d = t - pp.vsync, p = pp.period;
    return pp.vsync + (d >= 0 ? (d + p / 2) / p : -((-d + p / 2) / p)) * p;
}

// (create_swapchain) a swapchain was made: the display's period asked again, the cadence anew
void vk_pacing_swapchain(void) {
    if (!pp.ext) return;
    pp.sc = g_vk.swapchain;
    pp.vsync = 0, pp.past_at = 0, pp.n_after = 0, pp.asked = 0;
    pp.last_slot = pp.last_frame_slot = pp.run_since = pp.moved_at = 0, pp.anew = false;
    memset(pp.sent, 0, sizeof pp.sent);
    if (pp.sc) ask_period(plat_now_ns());
}

// The vsyncs a game frame takes at the FPS limit: n >= 2 when the limit is a whole fraction of the refresh rate (2 %
// either way), else 1
static int cadence(void) {
    if (pp.period <= 0) return 1;
    double refresh = 1e9 / (double)pp.period, cap = config_live_fps();
    if (cap <= 0 || cap >= refresh * 0.98) return 1;
    int k = (int)(refresh / cap + 0.5);
    if (k < 2) return 1;
    double d = refresh / k - cap;
    return (d < 0 ? -d : d) <= 0.02 * cap ? k : 1;
}

// Should the swapchain present FIFO with times (the switch on, the limit a whole fraction of the display's rate)?
// (device.c present_mode, and frame_begin: a change makes the swapchain again, at a menu's change of the limit or when
// the display changes its rate)
bool vk_pacing_fifo(void) {
    if (!pp.ext) return false;
    int64_t now = plat_now_ns();
    if (pp.sc && now - pp.period_at >= 2000000000) ask_period(now);
    return cadence() >= 2;
}

static int64_t deadline_ns(void) {  // (GameActivity.presentationDeadlineNs; 0: unknown)
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!act) return 0;
    jlong r = 0;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetStaticMethodID(env, cls, "presentationDeadlineNs", "()J") : NULL;
    if (m && !(*env)->ExceptionCheck(env)) r = (*env)->CallStaticLongMethod(env, cls, m);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env), r = 0;
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
    return (int64_t)r;
}

// When the presents given a time reached the screen: the newest is the vsync grid's phase; one shown a vsync later than the
// others a second time within 30 s raises the margin
static void read_past(int64_t now) {
    pp.past_at = now;
    VkPastPresentationTimingGOOGLE t[16];
    uint32_t n = 16;
    VkResult r = pp.past(g_vk.dev, pp.sc, &n, t);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return;
    for (uint32_t i = 0; i < n; i++) {
        int64_t shown = (int64_t)t[i].actualPresentTime;
        if (shown <= 0 || shown > now || now - shown > 2000000000) continue;  // (not shown, or not a time)
        if (shown > pp.vsync) pp.vsync = shown;
        if (pp.asked > 0 && (!pp.ref || shown < pp.ref)) {
            pp.ref = shown;
        } else if (pp.asked > 0 && shown - pp.ref >= 2000000000) {  // (the period: the vsyncs between the two, counted)
            int64_t k = (shown - pp.ref + pp.asked / 2) / pp.asked, m = k > 0 ? (shown - pp.ref + k / 2) / k : 0;
            if (m && llabs(m - pp.asked) <= pp.asked / 100) {
                if (llabs(m - pp.period) > 20000) LOG("[vk] present timing: the display's vsyncs come every %.4f ms\n", m / 1e6);
                pp.period = m;
            } else {
                pp.ref = shown;  // (not that rate: measured anew)
            }
        }
        for (int k = 0; k < 32; k++) {
            if (pp.sent[k].id != t[i].presentID) continue;
            int64_t slot = pp.sent[k].slot, per = pp.sent[k].period;
            pp.sent[k].id = 0;
            // The time a present reports is its slot's vsync plus what the display adds: an Anbernic RG406V reports every
            // frame a whole period after its slot, steady on the screen all the same. So late is against the least of
            // the last 32 frames' (shown - slot): a buffer later than that by more than half a period. Not in a margin's
            // first 10 s (a load's time), and only the second within 30 s, as a GPU's hitch alone makes one buffer late
            // whatever the margin.
            pp.after[pp.n_after++ % 32] = shown - slot;
            int64_t base = INT64_MAX;
            for (int a = 0; a < (pp.n_after < 32 ? pp.n_after : 32); a++) base = pp.after[a] < base ? pp.after[a] : base;
            bool late = pp.n_after >= 8 && shown - slot > base + per / 2;
            if (pp.n_after == 32) LOG("[vk] present timing: the display reports frames %.1f ms after their slots\n", base / 1e6);
            bool again = late && pp.last_late && now - pp.last_late < 30000000000ll;
            if (late) pp.last_late = now;
            if (again && now - pp.margin_at > 10000000000ll && pp.margin && pp.margin < 3 * per) {
                pp.last_late = 0;
                pp.margin = pp.margin + 2000000 < 3 * per ? pp.margin + 2000000 : 3 * per;
                LOG("[vk] present timing: a frame %.1f ms late, the margin is now %.1f ms\n", (shown - slot - base) / 1e6, pp.margin / 1e6);
            }
            break;
        }
    }
}

// Before vkQueuePresentKHR on a swapchain made for it (vk_pacing_fifo): the present's time, chained into pi. new_frame:
// a game frame (false: a picture presented again).
void vk_pacing_present(VkPresentInfoKHR *pi, bool new_frame) {
    static VkPresentTimeGOOGLE pt;
    static VkPresentTimesInfoGOOGLE pti = {.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE, .swapchainCount = 1, .pTimes = &pt};
    if (!pp.ext || !pp.sc || pp.sc != g_vk.swapchain) return;
    int64_t now = plat_now_ns();
    if (now - pp.past_at >= 500000000) read_past(now);
    if (now - pp.period_at >= 2000000000) ask_period(now);
    int64_t period = pp.period;
    if (!++pp.id) pp.id = 1;
    pt.presentID = pp.id, pt.desiredPresentTime = 0;  // (0: no time; the present's timing is still recorded)
    pti.pNext = pi->pNext, pi->pNext = &pti;
    int n = cadence();
    static const char *margin_env = (const char *)1;
    if (margin_env == (const char *)1) margin_env = getenv("MHGU_PRESENT_MARGIN");
    if (period > 0 && (!pp.margin || llabs(period - pp.margin_period) > period / 20)) {  // (worked out again at a new rate)
        int64_t d = deadline_ns(), m = margin_env ? atoll(margin_env) * 1000 : d > 0 ? d + 4000000 : 2 * period;
        if (!margin_env) m = m < period / 2 ? period / 2 : m > 3 * period ? 3 * period : m;
        pp.margin = m, pp.margin_period = period, pp.margin_at = now;
        LOG("[vk] present timing: the display's vsync every %.2f ms, presentation deadline %.1f ms, margin %.1f ms\n",
            period / 1e6, d / 1e6, m / 1e6);
    }
    if ((n != pp.told_n || llabs(period - pp.told_period) > period / 20) && now - pp.told_at > 5000000000ll && pp.told_lines < 40) {
        pp.told_n = n, pp.told_period = period, pp.told_at = now, pp.told_lines++;
        LOG("[vk] present timing: %d vsync(s) a game frame at %.0f FPS\n", n, (double)config_live_fps());
    }
    if (n < 2 || pp.vsync <= 0 || period <= 0) {  // (not a whole fraction, or the grid not known yet: shown when ready)
        pp.last_slot = pp.last_frame_slot = pp.run_since = 0, pp.anew = false;
        return;
    }
    int64_t t = now + pp.margin;  // the first vsync of the grid at or after now + margin
    int64_t earliest = on_grid(t);
    if (earliest < t) earliest += period;
    if (pp.last_slot && pp.last_slot < now - 4 * period * n)  // (nothing presented for a while: a new start)
        pp.last_slot = pp.last_frame_slot = pp.run_since = pp.moved_at = 0, pp.anew = false;
    if (pp.last_slot) pp.last_slot = on_grid(pp.last_slot);  // (the slots taken, on the newest grid)
    if (pp.last_frame_slot) pp.last_frame_slot = on_grid(pp.last_frame_slot);
    int64_t slot = earliest > pp.last_slot ? earliest : pp.last_slot;
    if (new_frame) {
        int64_t want = pp.last_frame_slot + (pp.anew ? period : n * period);
        bool late = pp.last_frame_slot && want < earliest;
        pp.anew = late && earliest - want > period;
        if (!pp.last_frame_slot || want < earliest) {
            if (pp.moved_at && now - pp.moved_at < 10000000000ll)  // (moved earlier too soon)
                pp.window = pp.window * 2 < 32000000000ll ? pp.window * 2 : 32000000000ll;
            pp.moved_at = 0, pp.run_since = 0;
        } else {
            if (want > slot) slot = want;
            if (want - earliest >= period) {  // (a whole period early)
                if (!pp.run_since) pp.run_since = now;
                if (now - pp.run_since >= pp.window && slot == want) {
                    slot -= period;  // (still not before `earliest`)
                    pp.moved_at = now, pp.run_since = 0;
                }
            } else {
                pp.run_since = 0;
            }
            if (pp.last_slot > slot) slot = pp.last_slot;
        }
        pp.last_frame_slot = slot;
    }
    pp.last_slot = slot;
    pt.desiredPresentTime = (uint64_t)(slot - period / 2);
    for (int k = 0; k < 32; k++)  // (the oldest entry makes room)
        if (!pp.sent[k].id || (uint32_t)(pp.id - pp.sent[k].id) >= 32) {
            pp.sent[k].id = pp.id, pp.sent[k].slot = slot, pp.sent[k].period = period;
            break;
        }
}
#else
typedef int present_pacing_unused;  // (the desktop: VSync's own present modes)
#endif
