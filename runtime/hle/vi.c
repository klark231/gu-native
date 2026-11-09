// nn::vi (display / layer) and nv:: (graphics driver init), implemented on the host.
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../config.h"
#include "../kernel.h"

static uint32_t vsync_r, vsync_w;
static KEvent *vsync_event;
uint32_t vi_vsync_handle(void) { return vsync_r; }
volatile uint64_t g_vsync_count;
uint32_t g_native_window;

// Frame rate. The engine paces itself (frame limiter at main 0xda5d00: sleeps until a frame period has passed, then
// derives its frame delta, which game logic scales by): the target is a float at +0x3c of its main system object
// (pointer in the global at 0x2326764, the same field theboy181's 60fps patch sets). The game puts 30.0 there at
// start and resets it at some transitions, so every presented frame writes the chosen rate back (the 90 / 120 fps
// cheat codes write this same field). The frame delta is in 60 fps frames, measured: walking speed is the same at 30,
// 60 and ~135 fps. The settings' FPS (mhgurecomp.ini: 30..240, default 60); MHGU_FRAMERATE supplies the initial override. Above the
// monitor's refresh rate the settings' VSync must be Off (immediate present): FIFO / mailbox wait for the display.
#define FPS_OBJ_GLOBAL 0x2326764u
static float framerate(void) { return config_live_fps(); }
void vi_apply_framerate(void) {  // (also per presented frame: vi_frame_presented, below)
    float want = framerate();
    uint32_t obj = LD32(FPS_OBJ_GLOBAL);
    if (obj < HEAP_BASE || obj >= HEAP_BASE + HEAP_MAX) return;  // (not created yet)
    float cur = LDF(obj + 0x3c);
    if (cur != want) STF(obj + 0x3c, want);
}

// ---- the app in the background (Android, [Application] PauseInBackground: runtime/ui.c)
// The game stops: its update thread waits at its next frame (vi_pause_game, from runtime/overlay.cpp's frame), and
// once it does - or a moment after the app left, if it never gets there (a long load) - the presenting, vsync and
// audio threads wait too (vi_pause_wait). While the update thread waits the guest's clock (rt_ticks) stands still:
// the engine's limiter (main 0xda5824) takes its frame step from that clock, so the first frame back is an ordinary
// frame's step, not the whole pause (capped at 16 frames by the engine: a quarter-second jump).
#define PAUSE_GRACE_NS 500000000ull
static pthread_mutex_t pause_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_cond = PTHREAD_COND_INITIALIZER;
static volatile int pause_wanted, pause_parked;  // the app is in the background; the update thread waits
static volatile uint64_t pause_since;            // when it left (plat_now_ns)
// The guest's clock: plat_now_ns less the time it stood still (clock_held), or clock_frozen while it stands (0: it
// runs). clock_gen is odd while they change (only the update thread changes them, under pause_lock), so a reader
// never mixes a value from before the change with one from after it.
static volatile uint32_t clock_gen;
static volatile uint64_t clock_frozen, clock_held;

uint64_t vi_guest_ns(void) {  // (runtime/kernel.c rt_ticks)
    for (;;) {
        uint32_t g = __atomic_load_n(&clock_gen, __ATOMIC_ACQUIRE);
        uint64_t f = __atomic_load_n(&clock_frozen, __ATOMIC_RELAXED), held = __atomic_load_n(&clock_held, __ATOMIC_RELAXED);
        uint64_t t = f ? f : plat_now_ns() - held;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (!(g & 1) && __atomic_load_n(&clock_gen, __ATOMIC_RELAXED) == g) return t;
    }
}

static void clock_set(uint64_t frozen, uint64_t held) {  // (under pause_lock)
    uint32_t g = __atomic_load_n(&clock_gen, __ATOMIC_RELAXED);
    __atomic_store_n(&clock_gen, g + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&clock_frozen, frozen, __ATOMIC_RELAXED);
    __atomic_store_n(&clock_held, held, __ATOMIC_RELAXED);
    __atomic_store_n(&clock_gen, g + 2, __ATOMIC_RELEASE);
}

// (host main thread: runtime/ui.c's lifecycle watch) the app went to the background, or came back
void vi_set_paused(bool on) {
    pthread_mutex_lock(&pause_lock);
    if (on && !pause_wanted) {
        __atomic_store_n(&pause_since, plat_now_ns(), __ATOMIC_RELAXED);
        LOG("[app] paused in the background\n");
    } else if (!on && pause_wanted) {
        LOG("[app] resumed\n");
    }
    __atomic_store_n(&pause_wanted, on ? 1 : 0, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&pause_cond);
    pthread_mutex_unlock(&pause_lock);
}

// Should the presenting / vsync / audio threads stop now? (cheap while the app is in front: one load)
bool vi_paused(void) {
    if (!__atomic_load_n(&pause_wanted, __ATOMIC_ACQUIRE)) return false;
    return __atomic_load_n(&pause_parked, __ATOMIC_ACQUIRE) ||
           plat_now_ns() - __atomic_load_n(&pause_since, __ATOMIC_RELAXED) >= PAUSE_GRACE_NS;
}

// Those threads: wait here while they should stop, until the app is back. (Called holding no lock another thread needs.)
void vi_pause_wait(void) {
    if (!vi_paused()) return;
    pthread_mutex_lock(&pause_lock);
    while (pause_wanted) pthread_cond_wait(&pause_cond, &pause_lock);
    pthread_mutex_unlock(&pause_lock);
}

// The game's update thread, once a frame (runtime/overlay.cpp overlay_frame): wait here while the app is in the
// background, the guest's clock held meanwhile.
void vi_pause_game(void) {
#if defined(__ANDROID__) || defined(_WIN32)
    perf_hint_thread(PERF_GAME);  // (the update thread, for the performance hint session: runtime/platform/android_perf.c;
                                  // Windows: the readout's game thread, platform/win_stats.c)
#endif
    if (!__atomic_load_n(&pause_wanted, __ATOMIC_ACQUIRE)) return;
#if defined(__ANDROID__) && !defined(MHGU_NULL_BACKEND)
    // (Android may end a background app with SIGKILL, no atexit: the pipeline cache is kept first, as the renderer does
    // when it presents in the background; it may not present again before this, nvn/vulkan/device.c)
    extern void vk_background_save(void);
    vk_background_save();
#endif
    pthread_mutex_lock(&pause_lock);
    if (!pause_wanted) { pthread_mutex_unlock(&pause_lock); return; }
    uint64_t t0 = plat_now_ns(), frozen = t0 - __atomic_load_n(&clock_held, __ATOMIC_RELAXED);
    if (!frozen) frozen = 1;  // (0 means running)
    clock_set(frozen, __atomic_load_n(&clock_held, __ATOMIC_RELAXED));
    __atomic_store_n(&pause_parked, 1, __ATOMIC_RELEASE);
    while (pause_wanted) pthread_cond_wait(&pause_cond, &pause_lock);
    uint64_t t1 = plat_now_ns();
    clock_set(0, t1 - frozen);  // (the clock goes on from where it stood)
    __atomic_store_n(&pause_parked, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&pause_lock);
    LOG("[app] game clock held %.1f s\n", (t1 - t0) / 1e9);
}

// (debug) MHGU_STEPLOG=1: every 5 s, the frames presented a second and the game's frame steps (+0x68 of the frame rate
// object, in 60 fps frames) added up over them: the game keeps real time while that is 60 a second ("game time 1.00x"),
// at any frame rate. For checking that a frame rate above 240 ("Unlimited" on a desktop) runs the game no faster.
static void step_log(void) {
    static int on = -1;
    static uint64_t t0;
    static double sum;
    static unsigned frames;
    if (on < 0) {
        const char *e = getenv("MHGU_STEPLOG");
        on = e && *e == '1';
    }
    if (!on) return;
    uint32_t obj = LD32(FPS_OBJ_GLOBAL);
    if (obj < HEAP_BASE || obj >= HEAP_BASE + HEAP_MAX) return;
    uint64_t now = plat_now_ns();
    if (!t0 || vi_paused()) { t0 = now, sum = 0, frames = 0; return; }
    sum += LDF(obj + 0x68), frames++;
    if (now - t0 >= 5000000000ull) {
        double s = (now - t0) / 1e9;
        LOG("[fps] %.0f fps (limit %.0f), game time %.2fx real (%.1f frame steps a second)\n", frames / s,
            (double)framerate(), sum / s / 60.0, sum / s);
        t0 = now, sum = 0, frames = 0;
    }
}

void vi_frame_presented(void) {  // (runtime/nvn/vulkan/device.c, per presented frame)
    vi_apply_framerate();
    step_log();
}

// The display's vsync event: 60 Hz, or the frame rate target when that is higher (the game waits for it per frame).
static void *vsync_thread(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_HIGH);  // (Windows: above normal, a tick late is the game's frame late)
    uint64_t next = plat_now_ns();
    for (;;) {
        if (vi_paused()) {  // (the app in the background: no frames to pace; then on from now, no burst)
            vi_pause_wait();
            next = plat_now_ns();
        }
        float fr = framerate();
        const uint64_t period = (uint64_t)(1e9 / (fr > 60.0f ? fr : 60.0f));
        next += period;
        plat_sleep_until_ns(next);
        uint64_t now = plat_now_ns();
        if (HITCH_ON && now - next > 1000000) hitch_add(HC_VSYNC_LATE, now - next, 0);  // ((debug) MHGU_HITCHLOG)
        if (now > next + period * 3) next = now;  // don't burst-signal to catch up
        vi_apply_framerate();
        __atomic_add_fetch(&g_vsync_count, 1, __ATOMIC_RELAXED);
        pthread_mutex_lock(&g_klock);
        kernel_signal(vsync_event);
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}
HLE_FUNC("_ZN2nn2vi10InitializeEv", vi_initialize) { (void)c; }
HLE_FUNC("_ZN2nn2vi8FinalizeEv", vi_finalize) { (void)c; }

HLE_FUNC("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_default_display) {
    ST32(ARG(0), hle_alloc(0x100));
    RET(0);
}

HLE_FUNC("_ZN2nn2vi12ListDisplaysEPNS0_11DisplayInfoEi", vi_list_displays) {  // DisplayInfo: name[64], ...
    if (ARG(0) && (int32_t)ARG(1) > 0) {
        memset(ARGP(0), 0, 0x60);
        strcpy((char *)ARGP(0), "Default");
        RET(1);
    } else {
        RET(0);
    }
}

HLE_FUNC("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", vi_close_display) { (void)c; }

HLE_FUNC("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer) {
    ST32(ARG(0), hle_alloc(0x100));
    RET(0);
}

HLE_FUNC("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", vi_destroy_layer) { (void)c; }
HLE_FUNC("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", vi_set_layer_crop) { RET(0); }
HLE_FUNC("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", vi_set_layer_scaling) { RET(0); }

HLE_FUNC("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window) {
    if (!g_native_window) g_native_window = hle_alloc(0x100);
    ST32(ARG(0), g_native_window);
    RET(0);
}

HLE_FUNC("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_get_vsync_event) {
    if (!vsync_event) {
        vsync_event = kernel_create_event_pair(&vsync_w, &vsync_r);
        pthread_t t;
        pthread_create(&t, NULL, vsync_thread, NULL);
    }
    // nn::os::AttachReadableHandleToSystemEvent(event, handle, managed=false, clear_mode=AutoClear)
    c->r[1] = vsync_r;
    c->r[2] = 0;
    c->r[3] = 1;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    RET(0);
}

HLE_FUNC("_ZN2nv18InitializeGraphicsEPvj", nv_initialize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv16FinalizeGraphicsEv", nv_finalize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv20SetGraphicsAllocatorEPFPvjjS0_EPFvS0_S0_EPFS0_S0_jS0_ES0_", nv_set_allocator) { (void)c; }
HLE_FUNC("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvjjS0_EPFvS0_S0_EPFS0_S0_jS0_ES0_", nv_set_dt_allocator) { (void)c; }
