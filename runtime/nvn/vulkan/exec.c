// Executes recorded NVN command lists with Vulkan 1.2. A lean port of LAS Recomp's runtime/nvn/vulkan/exec.c (same
// command interpretation, tables and shader contract) for the 1.2 baseline: VkRenderPass objects + framebuffers instead
// of dynamic rendering, all fixed-function state in the pipeline, v1 barriers. Guest memory is copied (vertex / index /
// uniform ranges, once per submission) rather than imported: the 32-bit guest's heap is 2.5 GB and lazily committed.
//   MHGU_SYNC_SHADERS=1   build shaders / pipelines on first use, blocking (deterministic runs; default: on worker
//                         threads, draws needing them wait a bounded time, then are skipped until ready)
//   MHGU_PIPE_WAIT_MS=n   per-frame budget for those waits (default 6, a desktop's a quarter of the frame interval up
//                         to 6; 0: skip at once)
//   MHGU_PIPE_WAIT_NEW=1  (desktop) wait for builds no cache has as well (a pipeline first made at a draw, a program
//                         being translated): by default their draws are skipped at once, no budget would get them
//   MHGU_COMPILE_WORKERS=n  threads for SPIR-V / pipeline builds (default 3; a desktop's half its logical processors,
//                         3 to 6, on Windows below the game's threads' priority)
//   MHGU_NO_PROGRAM_CACHE=1  don't keep translated programs / pipeline states in cache/ for the next run to prebuild
//   MHGU_SAVE_GLSL=1      translated shaders -> build/glsl/<code address>_<stage>.glsl
//   MHGU_VK_STATS=1       draw / pass / skip counts in the log every 2 s (and shader / pipeline build stats)
//   MHGU_VK_STAGING=<n>   desktop: staging chunks made at start-up (default 4; see "transient memory ring")
//   MHGU_VK_VC_SAFETY=<n> the vertex cache's safety net: 0 off (Windows' default), 1 on, 2 checked (see "vertex cache")
//   MHGU_PERF_VCNET=1     that net's turn every 30 s, 512 KB a frame, instead of every 2 s, 2 MB (see "vertex cache")
//   MHGU_SKIP_LOG=1       every draw skipped for a building program / pipeline: its shaders and colour target
//   MHGU_NO_PREWARM=1     don't translate the game's programs ahead of their first use (see "prewarm"; the ini's
//                         [Performance] ShaderPrewarm = Off does the same)
#ifdef __ANDROID__
#include <sys/resource.h>  // (setpriority: the shader workers below the game)
#include <unistd.h>
#endif
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>  // ftruncate (mingw pulls it in through another header; POSIX does not)

#include "../../kernel.h"
#include "fsr.h"
#include "shaders.h"
#include "vk.h"
#include "../../touch.h"
#ifndef __ANDROID__
#include "../../hle/mousemenu.h"
#endif
#ifdef _WIN32
#include <SDL3/SDL_cpuinfo.h>  // (compile_workers: no sysconf)
#endif
// The hunt's HUD parts (runtime/hud_parts.h: the item bar, the Hunter Arts, the target camera), where the player put them
// in the editors (runtime/touch.cpp) and, with the touch controls on the screen ([Touch] MoveHUD), out of the thumbs'
// way by default, on the game's 1280x720 canvas (cbuf_info): the status (clock, health, stamina, sharpness, name:
// layer 103 above y 500) to the screen's left edge, the item bar (layer 103 from y 500: x 992..1238 with its label) to
// the bottom middle, where a tap uses the item and a swipe turns the bar, the Hunter Arts panels (layer 100, x 709..957
// with their button icons, y 618..708: two rows; a third above them) to the top right, the target camera's icons (layer
// 100 at x 1200, y 42..82) left of them

static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_snapshots;
static uint64_t stat_a2c;  // (MHGU_VK_STATS) draws with alpha to coverage (none in the Marshland's grass at 1x or 4x: the grass
                           // is the game's uGrass2, which [Performance] Grass leaves out in runtime/hle/grass.c)
// (MHGU_VK_STATS) time spent: translating command lists, waiting for the GPU, guest memory copied per submission
uint64_t stat_exec_ns, stat_wait_ns, stat_copy_bytes, stat_upload_ns;
// (MHGU_VK_STATS) of those bytes: the vertex cache's whole ranges (new, or written all over), its written pages, its
// safety net's slices, and the ranges copied for one submission (not cached: uniforms, small or untracked ranges)
static uint64_t stat_vc_whole_bytes, stat_vc_pages_bytes, stat_vc_net_bytes, stat_gcopy_bytes;
static _Atomic int stat_compiles;
// (MHGU_VK_STATS) shader / pipeline builds: skips by cause, frames with skips, the waits, build times per job kind
// (translate, GLSL -> SPIR-V + modules, pipeline), request -> ready latency, the slowest frame (exec thread's view)
static uint64_t stat_skip_prog, stat_skip_pipe, stat_skip_frames, stat_waited_ns, stat_waits, stat_rescued, stat_frame_max;
// The render passes' power switches (measured in 0.1.9's perf pass and on by default, FMTLIST aside; its MHGU_PERF_<NAME> set
// in the environment, env.txt on a phone; read once, in vk_exec_init, and named in the log when on). CLAUDE.md lists them.
// The same struct holds the translator's per-draw switches (perf_init, below).
static struct {
    bool getenv, drvtab, texscan, textime, pipememo, lazyidx, idxcopy, dynubo;  // (the per-draw switches: perf_init)
    int rodepth;       // MHGU_PERF_RODEPTH=1: a draw sampling the pass's own depth target, without writing depth or stencil,
                       // reads it where it is (the pass goes on with it read-only) instead of a copy of it (resolve_feedback)
    int shadow_clear;  // MHGU_PERF_SHADOW_CLEAR=1: the shadow atlas's pass begins cleared, not read in, once its clearing draws
                       // are seen to overwrite all of it (at_*); 2 also leaves out those draws where they would write
                       // that value over itself
    int fxaa_copydraw; // MHGU_PERF_FXAA_COPYDRAW=1: FXAA Off, the game's FXAA pass's copy of its input drawn as the first draw
                       // of the pass that draws into its target next (fill_draw), once checked to give the copy's bytes
} perf;
static uint64_t stat_ro_passes, stat_ro_left, stat_atlas_clear, stat_atlas_skipped, stat_fills;  // (MHGU_VK_STATS)
static int perf_env(const char *name, int def) {  // (def: unset; measured on in 0.1.9's perf pass)
    const char *e = getenv(name);
    int v = e ? atoi(e) : def;
    if (v) LOG("[perf] %s=%d\n", name, v);
    return v;
}
bool vk_gpu_adreno(void);  // (device.c)
// (this draw: the game's alpha to coverage done as an alpha test at 0.5, one sample on an Adreno: the draw, #70)
static bool a2c_as_test;
static _Atomic uint64_t stat_pw_new, stat_pw_known, stat_pw_left;  // prewarm: translated, already cached, jobs queued
static _Atomic uint64_t stat_job_n[3], stat_job_ns[3], stat_job_max[3], stat_lat_n, stat_lat_ns, stat_lat_max;
static void atomic_max(_Atomic uint64_t *m, uint64_t v) {
    uint64_t o = atomic_load(m);
    while (v > o && !atomic_compare_exchange_weak(m, &o, v)) {}
}
static void job_stat(int kind, uint64_t t0) {
    uint64_t d = plat_now_ns() - t0;
    atomic_fetch_add(&stat_job_n[kind], 1);
    atomic_fetch_add(&stat_job_ns[kind], d);
    atomic_max(&stat_job_max[kind], d);
    if (HITCH_ON) hitch_add(HC_JOB_XLATE + kind, d, 0);  // ((debug) MHGU_HITCHLOG: translate, program, pipeline)
}

// ---- (test) the translator's per-draw work, each cut behind a switch of its own, read once at start (vk_exec_init,
// which logs them): MHGU_PERF_<NAME>=0|1, on unless set to 0. MHGU_PERF_DRAWS=0 turns off all but DYNUBO at once
// (a switch of its own still wins). None changes what is drawn: the same commands, the same data.
//   GETENV    debug switches read once, not by getenv at every depth clamp command / buffer-to-texture copy
//   DRVTAB    the driver constants' storage buffer table (256 bytes) compared before the 2 KB block is zeroed and filled
//   TEXSCAN   a draw's textures looked up once for the movie, MSAA-resolve and feedback checks (each walked them again);
//             the MSAA check is skipped until some pass has rendered multisampled
//   TEXTIME   a texture bind that has nothing to upload isn't timed (textures.c vk_tex: two clock reads each)
//   PIPEMEMO  a draw with the same pipeline key as the last one takes its pipeline without hashing the key and probing
//   LAZYIDX   a draw's largest index found only when a vertex stream's copy needs it (rarely: bound ranges up to 4 MB
//             are copied whole, the vertex cache's not at all)
//   IDXCOPY   16 / 32-bit indices copied once per submission per range (the same mesh drawn in the shadow, depth, mask
//             and main passes shares the copy, as uniform buffers do)
//   DYNUBO    the uniform buffers bound with dynamic offsets (VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC), the game's per
//             object ones first, up to the GPU's maxDescriptorSetUniformBuffersDynamic a program: a buffer's place in the
//             staging ring is then no longer part of a descriptor set, and stage_set reuses far more of them. Changes
//             the programs' pipeline layouts: the driver's pipeline cache is cold once (every pipeline built again)
// (their fields are in perf, above, with the render passes' switches)
static bool perf_dump_verbose, perf_tex_log;  // (GETENV: MHGU_DUMP_VERBOSE, MHGU_TEX_LOG)
static uint32_t perf_dyn_limit;               // (DYNUBO: dynamic uniform buffers a pipeline layout may hold)
bool vk_perf_textime;                         // (textures.c vk_tex)
static uint64_t stat_pipememo, stat_idx_bytes, stat_idx_shared, stat_lazy_scans, stat_dyn_binds;  // (MHGU_VK_STATS)
// MHGU_PERF_CHECK=1 (a test of the switches, slower): each cut that is on also does the old work and compares, and the
// [perf] 2s line counts what differed (always 0 if they are exact): the driver constants' bytes, a draw's textures, the
// memoised pipeline, the indices' shared copy against guest memory, the vertex data a lazy largest index sizes
static bool perf_check;
static uint64_t stat_check_n, stat_check_bad;
static void check_bad(const char *what) {
    stat_check_bad++;
    static int n;
    if (n++ < 20) LOG("[perf] check: %s differs from the old way\n", what);
}
static bool perf_switch(const char *name, bool group) {
    char k[48];
    snprintf(k, sizeof k, "MHGU_PERF_%s", name);
    const char *e = getenv(k);
    return e ? atoi(e) != 0 : group;
}
static void perf_init(void) {
    const char *g = getenv("MHGU_PERF_DRAWS");
    bool all = g ? atoi(g) != 0 : true;  // (unset: on, measured in 0.1.9's perf pass)
    perf.getenv = perf_switch("GETENV", all), perf.drvtab = perf_switch("DRVTAB", all);
    perf.texscan = perf_switch("TEXSCAN", all), perf.textime = perf_switch("TEXTIME", all);
    perf.pipememo = perf_switch("PIPEMEMO", all), perf.lazyidx = perf_switch("LAZYIDX", all);
    perf.idxcopy = perf_switch("IDXCOPY", all), perf.dynubo = perf_switch("DYNUBO", g_vk.api12);  // (a Vulkan 1.1 driver: off unless set, device.c)
    perf_check = perf_switch("CHECK", false);
    perf_dump_verbose = getenv("MHGU_DUMP_VERBOSE") != NULL, perf_tex_log = getenv("MHGU_TEX_LOG") != NULL;
    vk_perf_textime = perf.textime;
    uint32_t lim = g_vk.props.limits.maxDescriptorSetUniformBuffersDynamic;
    perf_dyn_limit = lim > 32 ? 32 : lim;
    if (perf.dynubo && !perf_dyn_limit) perf.dynubo = false;
    LOG("[perf] draws: getenv %d drvtab %d texscan %d textime %d pipememo %d lazyidx %d idxcopy %d dynubo %d (a pipeline layout's "
        "dynamic uniform buffers: up to %u)\n", perf.getenv, perf.drvtab, perf.texscan, perf.textime, perf.pipememo, perf.lazyidx,
        perf.idxcopy, perf.dynubo, lim);
}

// The renderer's own pipelines (resolves, masked clears, FXAA), made on the render thread the first time they are needed
static VkResult sync_pipeline(const VkGraphicsPipelineCreateInfo *pi, VkPipeline *p) {
    uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, pi, NULL, p);
    if (HITCH_ON) hitch_add(HC_PIPE_SYNC, plat_now_ns() - t0, 0);
    return r;
}

// ================================================================== submissions
// A submission = optional upload command buffer + the main one, signalling the timeline with its number.
#define SUBS 8
typedef struct {
    VkCommandBuffer main, upload;
    bool main_begun, upload_begun;
    uint64_t seq;
    VkDescriptorPool pools[32];
    int npools, cur_pool;
} Sub;
static Sub subs[SUBS];
static int cur_sub;
static uint64_t submit_seq;  // last submitted
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static bool in_pass;
static void ss_forget(void);

// ---- device lost: what each submission held, for the log when the driver loses the device (a GPU fault or hang
// in work submitted earlier shows at a later call: every submission not known finished is listed). Per slot of the
// ring, a few bytes per pass and per program: render thread only, read once at the error.
#define LOST_PASSES 32
#define LOST_PROGS 48
typedef struct { uint16_t w, h; int16_t color, depth; uint8_t ncolor, samples; uint32_t draws; } LostPass;
typedef struct {
    uint64_t seq;
    int npass, nprog;
    uint32_t passes, draws, drawn_clears, copies, depth_copies;  // (all of them, also past the arrays)
    LostPass pass[LOST_PASSES];
    uint32_t prog[LOST_PROGS][2];  // (vertex and fragment code addresses)
} LostRec;
static LostRec lost_rec[SUBS];
static void lost_reset(int slot, uint64_t seq) { memset(&lost_rec[slot], 0, sizeof lost_rec[slot]); lost_rec[slot].seq = seq; }
static void lost_pass(uint32_t w, uint32_t h, VkFormat c0, uint32_t ncolor, VkFormat depth, uint32_t samples) {
    LostRec *r = &lost_rec[cur_sub];
    r->passes++;
    if (r->npass < LOST_PASSES)
        r->pass[r->npass++] = (LostPass){(uint16_t)w, (uint16_t)h, (int16_t)c0, (int16_t)depth, (uint8_t)ncolor, (uint8_t)samples, 0};
}
static void lost_draw(void) {
    LostRec *r = &lost_rec[cur_sub];
    r->draws++;
    if (r->npass) r->pass[r->npass - 1].draws++;
}
static void lost_prog(uint64_t vs, uint64_t fs) {
    LostRec *r = &lost_rec[cur_sub];
    for (int i = r->nprog - 1; i >= 0; i--)
        if (r->prog[i][0] == (uint32_t)vs && r->prog[i][1] == (uint32_t)fs) return;
    if (r->nprog < LOST_PROGS) r->prog[r->nprog][0] = (uint32_t)vs, r->prog[r->nprog][1] = (uint32_t)fs, r->nprog++;
}
static void lost_report(const char *where);
volatile int g_fake_lost;  // (test) MHGU_CONTROL "devicelost": the next submission with a pass says the device was lost

// Depth (and stencil) copies: whether the GPU can copy that format at all (textures.c depth_format: a driver may lack it,
// and a copy it can't do is an invalid command that may lose the device), and each kind named once in the log where the
// game first does it, counted for the device-lost report (#160: few drivers are tested with them)
static bool depth_copy_ok(VkFormat f) {
    const VkFormatFeatureFlags x = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (vk_format_features(f) & x) == x;
}
static void depth_copy_note(const char *kind, int sw, int sh, int dw, int dh, VkFormat f) {
    static struct { const char *kind; int sw, dw; VkFormat f; } seen[8];
    static int nseen;
    int i = 0;
    while (i < nseen && !(seen[i].kind == kind && seen[i].sw == sw && seen[i].dw == dw && seen[i].f == f)) i++;
    if (i == nseen && nseen < 8) {
        seen[nseen].kind = kind, seen[nseen].sw = sw, seen[nseen].dw = dw, seen[nseen].f = f, nseen++;
        LOG("[vk] depth %s %dx%d -> %dx%d (format %d)\n", kind, sw, sh, dw, dh, f);
    }
    lost_rec[cur_sub].depth_copies++;
}

// Without timeline semaphores (a Vulkan 1.1 driver) each submission signals a fence, one per slot of the ring: sub()
// waits for a slot's old submission, so its fence is free again by then. A fence can't be reset while another thread
// waits for it, and any thread may wait for a submission here: so this one thread waits for them all, in order, resets
// each and only then says it is done.
static VkFence seq_fence[SUBS];
static pthread_mutex_t fence_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fence_cond = PTHREAD_COND_INITIALIZER;
static uint64_t fence_queued;        // the last submission with a fence (under fence_lock)
static _Atomic uint64_t fence_done;  // the last one finished, its fence ready (written under fence_lock)

static void *fence_thread(void *arg) {
    (void)arg;
    plat_thread_name("fence");
    for (uint64_t seq = 1;; seq++) {
        pthread_mutex_lock(&fence_lock);
        while (fence_queued < seq) pthread_cond_wait(&fence_cond, &fence_lock);
        pthread_mutex_unlock(&fence_lock);
        VkFence f = seq_fence[seq % SUBS];
        VkResult r;
        while ((r = vkWaitForFences(g_vk.dev, 1, &f, VK_TRUE, 10ull * 1000000000ull)) == VK_TIMEOUT)
            LOG("[vk] GPU did not finish submission %llu within 10 s (device lost?)\n", (unsigned long long)seq);
        if (r != VK_SUCCESS) {
            if (vk_exiting()) return NULL;
            if (r == VK_ERROR_DEVICE_LOST) lost_report("vkWaitForFences");
            FATAL("vkWaitForFences: %d (device lost)\n", r);
        }
        VK_CHECK(vkResetFences(g_vk.dev, 1, &f));
        pthread_mutex_lock(&fence_lock);
        atomic_store(&fence_done, seq);
        pthread_cond_broadcast(&fence_cond);
        pthread_mutex_unlock(&fence_lock);
    }
}

static void fence_init(void) {
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (int i = 0; i < SUBS; i++) VK_CHECK(vkCreateFence(g_vk.dev, &fi, NULL, &seq_fence[i]));
    pthread_t t;
    if (pthread_create(&t, NULL, fence_thread, NULL)) FATAL("the fence thread could not start\n");
    pthread_detach(t);
}

static VkResult fence_wait(uint64_t seq) {  // (as vkWaitSemaphores: up to 10 s)
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 10;
    int e = 0;
    pthread_mutex_lock(&fence_lock);
    while (atomic_load(&fence_done) < seq && e != ETIMEDOUT) e = pthread_cond_timedwait(&fence_cond, &fence_lock, &until);
    pthread_mutex_unlock(&fence_lock);
    return e == ETIMEDOUT ? VK_TIMEOUT : VK_SUCCESS;
}

uint64_t vk_completed_seq(void) {
    if (!g_vk.has_timeline) return atomic_load(&fence_done);
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v);
    return v;
}

// (MHGU_VK_STATS) the GPU waits by site: 0 other, 1 the submission ring, 2 nvnQueueFinish, 3 nvnSyncWait,
// 4 renderer_flush, 5 the present slot, 6 a readback; a wait for everything submitted ("drain") counted apart
enum { W_OTHER, W_RING, W_FINISH, W_SYNC, W_FLUSH, W_PRESENT, W_READBACK, W_SITES };
static _Atomic uint64_t stat_w_n[W_SITES], stat_w_ns[W_SITES], stat_w_drain[W_SITES], stat_submits;
static __thread int wait_site;
void vk_wait_site(int site) { wait_site = site; }  // (the next vk_wait_seq's site, this thread)

void vk_wait_seq(uint64_t seq) {
    if (!seq || vk_exiting()) return;  // (exit in progress: never reach the driver from a late guest thread)
    int site = wait_site;
    wait_site = W_OTHER;
    if (vk_completed_seq() >= seq) return;  // (done already: no wait)
    uint64_t t0_ = plat_now_ns();
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    VkResult r = g_vk.has_timeline ? vkWaitSemaphores(g_vk.dev, &wi, 10ull * 1000000000ull) : fence_wait(seq);
    uint64_t d_ = plat_now_ns() - t0_;
    stat_wait_ns += d_;
    if (site >= 0 && site < W_SITES) {
        atomic_fetch_add(&stat_w_n[site], 1), atomic_fetch_add(&stat_w_ns[site], d_);
        if (seq >= submit_seq) atomic_fetch_add(&stat_w_drain[site], 1);
        if (HITCH_ON) hitch_add(HC_GPU_OTHER + site, d_, 0);  // ((debug) MHGU_HITCHLOG: the sites in the same order)
    }
    if (r == VK_TIMEOUT) LOG("[vk] GPU did not finish submission %llu within 10 s (device lost?)\n", (unsigned long long)seq);
    else if (r != VK_SUCCESS) {
        if (r == VK_ERROR_DEVICE_LOST) lost_report("vkWaitSemaphores");
        FATAL("vkWaitSemaphores: %d (device lost)\n", r);
    }
}

uint64_t vk_pending_seq(void) { return submit_seq + 1; }

static Sub *sub(void) {
    Sub *s = &subs[cur_sub];
    if (s->seq != submit_seq + 1) {  // slot reused: its previous submission must be done
        vk_wait_site(W_RING);
        vk_wait_seq(s->seq);
        uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
        for (int i = 0; i < s->npools; i++) vkResetDescriptorPool(g_vk.dev, s->pools[i], 0);
        if (HITCH_ON && s->npools) hitch_add(HC_DESC_RESET, plat_now_ns() - t0, (uint64_t)s->npools);
        s->cur_pool = 0;
        s->seq = submit_seq + 1;
        lost_reset(cur_sub, s->seq);
        s->main_begun = s->upload_begun = false;
    }
    return s;
}

static void begin_cb(VkCommandBuffer *cb, bool *begun) {
    if (*begun) return;
    if (!*cb) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, cb));
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(*cb, &bi));
    *begun = true;
}

VkCommandBuffer vk_cmd(void) {
    Sub *s = sub();
    if (!s->main_begun) {
        begin_cb(&s->main, &s->main_begun);
        ss_forget();
    }
    return s->main;
}

VkCommandBuffer vk_upload_cmd(void) {
    Sub *s = sub();
    if (!s->upload_begun) {
        begin_cb(&s->upload, &s->upload_begun);
        // after everything submitted before it: these copies overwrite what earlier work may still read
        vk_full_barrier(s->upload);
    }
    return s->upload;
}

void vk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_ALL_WRITES, .dstAccessMask = VK_ACCESS_ALL};
    vkCmdPipelineBarrier(cb, VK_STAGES_ALL, VK_STAGES_ALL, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void guest_copy_new_submission(void);
static void staging_trim(void);
static void vc_trim(void);

// Submit the pending work (if any). wait / signal: binary semaphores for presentation (may be null).
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal) {
    vk_end_pass();
    Sub *s = sub();
    if (!s->main_begun && !s->upload_begun && !wait && !signal) return;
    VkCommandBuffer cbs[2];
    uint32_t n = 0;
    if (s->upload_begun) {
        vk_full_barrier(s->upload);  // uploads complete before anything in the main buffer reads them
        VK_CHECK(vkEndCommandBuffer(s->upload));
        cbs[n++] = s->upload;
    }
    vk_cmd();  // (an empty main buffer still carries the semaphores)
    VK_CHECK(vkEndCommandBuffer(s->main));
    cbs[n++] = s->main;
    VkSemaphore sig[2] = {g_vk.timeline, signal};
    uint64_t sig_val[2] = {submit_seq + 1, 0};
    uint64_t wait_val = 0;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                        .waitSemaphoreValueCount = wait ? 1 : 0, .pWaitSemaphoreValues = &wait_val,
                                        .signalSemaphoreValueCount = signal ? 2 : 1, .pSignalSemaphoreValues = sig_val};
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts, .waitSemaphoreCount = wait ? 1 : 0,
                       .pWaitSemaphores = &wait, .pWaitDstStageMask = &wait_stage, .commandBufferCount = n,
                       .pCommandBuffers = cbs, .signalSemaphoreCount = signal ? 2 : 1, .pSignalSemaphores = sig};
    VkFence fence = VK_NULL_HANDLE;
    if (!g_vk.has_timeline) {
        si.pNext = NULL;
        si.signalSemaphoreCount = signal ? 1 : 0, si.pSignalSemaphores = &signal;
        fence = seq_fence[(submit_seq + 1) % SUBS];
    }
    vk_upload_jobs_finish();  // (the upload buffer's copies read staging memory the texture workers may still be writing)
    uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
    VkResult sr = vkQueueSubmit(g_vk.queue, 1, &si, fence);
    if (g_fake_lost && sr == VK_SUCCESS && s->main_begun && lost_rec[cur_sub].passes) sr = VK_ERROR_DEVICE_LOST;  // (test: "devicelost")
    if (sr != VK_SUCCESS) {
        if (sr == VK_ERROR_DEVICE_LOST) lost_report("vkQueueSubmit");
        FATAL("vkQueueSubmit(g_vk.queue, 1, &si, fence) failed: %d (%s:%d)\n", sr, __FILE__, __LINE__);
    }
    if (HITCH_ON) hitch_add(HC_SUBMIT, plat_now_ns() - t0, 0);
    if (fence) {
        pthread_mutex_lock(&fence_lock);
        fence_queued = submit_seq + 1;
        pthread_cond_broadcast(&fence_cond);
        pthread_mutex_unlock(&fence_lock);
    }
    atomic_fetch_add(&stat_submits, 1);
    pthread_mutex_lock(&seq_lock);
    submit_seq++;
    pthread_mutex_unlock(&seq_lock);
    cur_sub = (cur_sub + 1) % SUBS;
    guest_copy_new_submission();
    vk_collect_dead();
    staging_trim();
    vc_trim();
}

static void renderer_submit(void) { vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE); }

#ifdef __ANDROID__
// (device lost) the GPU driver's own lines in this process's logcat (an app reads its own): Adreno's say a page fault
// from a hang, a Mali's the same
static void lost_logcat(void) {
    FILE *f = popen("logcat -d -v brief -t 800 2>/dev/null", "r");
    if (!f) return;
    char line[512];
    int n = 0;
    static const char *const keys[] = {"Adreno", "adreno", "GSL", "kgsl", "KGSL", "vulkan", "Vulkan", "mali", "Mali", "gralloc", "GPU"};
    while (fgets(line, sizeof line, f) && n < 60) {
        if (strstr(line, "/MHGU") || strstr(line, "/SDL")) continue;  // (the game's own log: already in it)
        bool hit = false;
        for (size_t i = 0; i < sizeof keys / sizeof keys[0] && !hit; i++) hit = strstr(line, keys[i]) != NULL;
        if (!hit) continue;
        line[strcspn(line, "\r\n")] = 0;
        LOG("[vk]   logcat: %.300s\n", line);
        n++;
    }
    pclose(f);
    if (!n) LOG("[vk]   logcat: no line of the GPU driver's\n");
}
#endif

// (device lost, once) every submission not known finished, and the last LOST_FINISHED that are: its passes (size, first
// colour format and how many, depth format, samples, draws), the programs it drew with, the clears drawn and the copies;
// and the driver's own words. The finished ones too because a driver may say the one that faulted finished: the Adreno 618's
// of Android 10 (1.1.87, #160, #191) retires a batch that faulted, its fence signalled, and refuses the next submission
// (KGSL's EPROTO: "the context faulted before"), so 0.1.9.1's report named only that next one, an empty submission.
#define LOST_FINISHED 4
static void lost_report(const char *where) {
    static _Atomic int once;
    if (atomic_exchange(&once, 1)) return;
    uint64_t done = g_vk.has_timeline ? 0 : atomic_load(&fence_done), last = submit_seq + 1;
    if (g_vk.has_timeline && vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &done) != VK_SUCCESS) done = 0;
    LOG("[vk] device lost (%s): submission %llu was being submitted, %llu known finished; the last finished and the work since:\n",
        where, (unsigned long long)last, (unsigned long long)done);
    uint64_t first = last >= SUBS ? last - SUBS + 1 : 1;
    if (done >= LOST_FINISHED && done + 1 - LOST_FINISHED > first) first = done + 1 - LOST_FINISHED;
    for (uint64_t q = first; q <= last; q++) {
        const LostRec *r = NULL;
        for (int i = 0; i < SUBS; i++)
            if (lost_rec[i].seq == q) r = &lost_rec[i];
        if (!r) continue;
        LOG("[vk]  submission %llu%s: %u passes, %u draws, %u clears drawn, %u copies (%u of depth)\n", (unsigned long long)q,
            q <= done ? " (finished)" : "", r->passes, r->draws, r->drawn_clears, r->copies, r->depth_copies);
        for (int i = 0; i < r->npass; i++) {
            const LostPass *p = &r->pass[i];
            LOG("[vk]   pass %d: %ux%u, colour %d x%u, depth %d, %ux, %u draws\n", i, p->w, p->h, p->color, p->ncolor, p->depth,
                p->samples, p->draws);
        }
        for (int i0 = 0; i0 < r->nprog; i0 += 12) {  // (12 a line)
            char progs[12 * 20 + 1];
            int k = 0;
            for (int i = i0; i < r->nprog && i < i0 + 12; i++)
                k += snprintf(progs + k, sizeof progs - k, " %x/%x", r->prog[i][0], r->prog[i][1]);
            LOG("[vk]   programs (vertex/fragment code):%s\n", progs);
        }
    }
#ifdef __ANDROID__
    lost_logcat();
#endif
}

void renderer_flush(void) {
    if (vk_exiting()) return;
    vk_lock();
    renderer_submit();
    uint64_t s = submit_seq;
    vk_unlock();
    vk_wait_site(W_FLUSH);
    vk_wait_seq(s);
}

void renderer_sync(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    vk_wait_site(W_FINISH);
    vk_wait_seq(s);
}

uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t q = submit_seq + (subs[cur_sub].main_begun || subs[cur_sub].upload_begun ? 1 : 0);
    pthread_mutex_unlock(&seq_lock);
    return q;
}

void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    vk_wait_site(W_SYNC);
    vk_wait_seq(seq > s ? s : seq);  // not submitted yet (other thread): everything submitted so far
}

VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    Sub *s = sub();
    for (;;) {
        if (s->cur_pool == s->npools) {
            if (s->npools == 32) FATAL("descriptor pools exhausted\n");
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384},
                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 512}, {VK_DESCRIPTOR_TYPE_SAMPLER, 512},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 512},
                                            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 16384}};  // (MHGU_PERF_DYNUBO only)
            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4096,
                                             .poolSizeCount = perf.dynubo ? 7 : 6, .pPoolSizes = sizes};
            uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
            VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &s->pools[s->npools++]));
            if (HITCH_ON) hitch_add(HC_DESC_POOL, plat_now_ns() - t0, 0);
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = s->pools[s->cur_pool], .descriptorSetCount = 1, .pSetLayouts = &layout};
        VkDescriptorSet set;
        if (vkAllocateDescriptorSets(g_vk.dev, &ai, &set) == VK_SUCCESS) return set;
        s->cur_pool++;
    }
}

// ================================================================== transient memory ring
// 32 MB host-visible chunks; a chunk is reused once every submission that allocated from it has completed.
// Desktop: the first ones are made at start-up (vk_exec_init), so a burst of uploads in play doesn't make one: a new
// chunk took 31-40 ms on an RX 6800 XT (its memory zeroed and mapped; render thread, vk_lock held: MHGU_HITCHLOG's
// staging-chunk), and a hunt had made 3.
//   MHGU_VK_STAGING=<n>   chunks made at start-up (default 4; 0: none, each made when needed)
#define CHUNK_SIZE (32ull << 20)
#define CHUNKS_AHEAD 4
typedef struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; uint64_t size, used, seq, picked_ns; } Chunk;
static Chunk *chunks;
static int nchunks, active = -1;
static _Atomic uint64_t staging_bytes;  // (the [mem] line: the chunks' bytes and count)
static _Atomic int staging_n;

static VkResult make_chunk(Chunk *c, uint64_t size) {
    memset(c, 0, sizeof *c);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
    VkResult r = vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info);
    if (HITCH_ON) hitch_add(HC_CHUNK, plat_now_ns() - t0, size);  // ((debug) MHGU_HITCHLOG: on the thread that made it)
    if (r == VK_SUCCESS) c->ptr = info.pMappedData, c->size = size;
    return r;
}

#ifndef __ANDROID__
// The ring's next chunk is made ahead, on a thread of its own. A new chunk is a vkAllocateMemory of some 30 ms (the
// driver zeroes and pins its 32 MB: 8000 page faults), which a frame paid whenever a burst of first-time textures took
// the last free chunk (measured: a 1.3 MB texture upload "took" 34.6 ms). Each time the ring takes the spare, the next
// one is made. Phones still grow on the spot: there a chunk made ahead is 32 MB held until a burst needs it.
// MHGU_VK_SPARE_CHUNK=0: no spare (for comparisons).
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool want, ready, thread;
    Chunk c;
} spare = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER};

static void *spare_thread(void *unused) {
    (void)unused;
    plat_thread_name("spare-chunk");
    pthread_mutex_lock(&spare.mu);
    for (;;) {
        while (!spare.want) pthread_cond_wait(&spare.cv, &spare.mu);
        pthread_mutex_unlock(&spare.mu);
        Chunk c;
        bool ok = !vk_exiting() && make_chunk(&c, CHUNK_SIZE) == VK_SUCCESS;
        pthread_mutex_lock(&spare.mu);
        spare.want = false;
        if (ok) spare.c = c, spare.ready = true;
    }
    return NULL;
}

// (render thread) The spare, if it is ready; the next one is asked for either way.
static bool take_spare(Chunk *c) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("MHGU_VK_SPARE_CHUNK"); on = !e || atoi(e) != 0; }
    if (!on) return false;
    pthread_mutex_lock(&spare.mu);
    bool got = spare.ready;
    if (got) *c = spare.c, spare.ready = false;
    if (!spare.want) {
        if (!spare.thread) {
            pthread_t th;
            spare.thread = !pthread_create(&th, NULL, spare_thread, NULL);
            if (spare.thread) pthread_detach(th);
        }
        spare.want = spare.thread;
        pthread_cond_signal(&spare.cv);
    }
    pthread_mutex_unlock(&spare.mu);
    return got;
}
#endif

static int new_chunk(uint64_t size, bool must) {  // (!must: -1 when the driver refuses)
    Chunk c;
#ifndef __ANDROID__
    if (size != CHUNK_SIZE || !take_spare(&c))
#endif
    {
        VkResult r = make_chunk(&c, size);
        if (r != VK_SUCCESS) {
            if (must) VK_CHECK(r);
            return -1;
        }
    }
    chunks = realloc(chunks, (nchunks + 1) * sizeof *chunks);
    chunks[nchunks] = c;
    atomic_fetch_add_explicit(&staging_bytes, c.size, memory_order_relaxed);
    atomic_fetch_add_explicit(&staging_n, 1, memory_order_relaxed);
    return nchunks++;
}

#ifndef __ANDROID__
static void chunks_ahead(void) {
    const char *e = getenv("MHGU_VK_STAGING");
    int want = e ? atoi(e) : CHUNKS_AHEAD;
    while (nchunks < want && nchunks < 16 && new_chunk(CHUNK_SIZE, false) >= 0) {}
    if (!nchunks) return;
    VmaAllocationInfo info;
    vmaGetAllocationInfo(g_vk.vma, chunks[0].mem, &info);
    const VkPhysicalDeviceMemoryProperties *mp;
    vmaGetMemoryProperties(g_vk.vma, &mp);
    LOG("[vk] staging: %d chunks of %llu MB made up front (memory type %u, flags %#x)\n", nchunks,
        (unsigned long long)(CHUNK_SIZE >> 20), info.memoryType, mp->memoryTypes[info.memoryType].propertyFlags);
}
#endif

VkSpan vk_transient(uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    uint64_t pending = vk_pending_seq();
    if (active >= 0) {
        Chunk *c = &chunks[active];
        uint64_t off = (c->used + align - 1) & ~(align - 1);
        if (off + size <= c->size) {
            c->used = off + size;
            c->seq = pending;
            return (VkSpan){c->buf, off, c->ptr + off};
        }
    }
    uint64_t done = vk_completed_seq();
    int pick = -1;
    for (int i = 0; i < nchunks; i++)
        if (i != active && chunks[i].seq <= done && chunks[i].size >= size) { pick = i; break; }
    if (pick < 0) pick = new_chunk(size > CHUNK_SIZE ? size : CHUNK_SIZE, true);
    active = pick;
    chunks[pick].used = size;
    chunks[pick].seq = pending;
    chunks[pick].picked_ns = plat_now_ns();
    return (VkSpan){chunks[pick].buf, 0, chunks[pick].ptr};
}

// Phones: a chunk the ring hasn't taken for STAGING_IDLE_NS goes back to the system. The ring takes the first free chunk
// in order, so the ones it needs every frame stay, and those a burst made (an area's textures, decoded on a Mali to 4 to
// 8 times their size) don't stay for good (#59, #156: a 2.8 GB phone closed by Android). One is always kept.
// MHGU_VK_STAGING_TRIM=0: kept for good, as before.
#define STAGING_IDLE_NS 8000000000ull

#ifdef __ANDROID__
// (phones, the trims below) the system's memory nearly gone: under 350 MB available (MemAvailable, read at most every 2 s;
// Android 14 and newer no longer tell an app in the foreground that memory runs low, onTrimMemory's RUNNING levels).
// Then what is idle goes after 2 s, not 8 and 20. MHGU_LOWMEM_MB=<n>: that threshold.
static bool memory_short(uint64_t now) {
    static uint64_t next, limit_kb;
    static bool shrt, said;
    if (!limit_kb) {
        const char *e = getenv("MHGU_LOWMEM_MB");
        limit_kb = (uint64_t)(e && atoi(e) > 0 ? atoi(e) : 350) << 10;
    }
    if (now < next) return shrt;
    next = now + 2000000000ull;
    FILE *f = fopen("/proc/meminfo", "re");
    if (!f) return shrt = false;
    char line[128];
    long kb = -1;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "MemAvailable:", 13)) { kb = atol(line + 13); break; }
    fclose(f);
    shrt = kb >= 0 && (uint64_t)kb < limit_kb;
    if (shrt && !said) LOG("[vk] memory short (%ld MB available): idle staging and vertex cache memory goes at once\n", kb >> 10);
    said |= shrt;
    return shrt;
}
#endif

static void staging_trim(void) {
#ifdef __ANDROID__
    static uint64_t next;
    static int on = -1;
    if (on < 0) { const char *e = getenv("MHGU_VK_STAGING_TRIM"); on = !e || atoi(e) != 0; }
    uint64_t now = plat_now_ns();
    if (!on || now < next || nchunks <= 1) return;
    next = now + 1000000000ull;
    uint64_t done = vk_completed_seq(), freed = 0, idle = memory_short(now) ? 2000000000ull : STAGING_IDLE_NS;
    int n = 0;
    for (int i = nchunks - 1; i >= 0 && nchunks > 1; i--) {
        Chunk *c = &chunks[i];
        if (i == active || c->seq > done || now - c->picked_ns < idle) continue;
        freed += c->size, n++;
        atomic_fetch_sub_explicit(&staging_bytes, c->size, memory_order_relaxed);
        atomic_fetch_sub_explicit(&staging_n, 1, memory_order_relaxed);
        vmaDestroyBuffer(g_vk.vma, c->buf, c->mem);  // (its submissions are done: nothing reads it)
        int last = nchunks - 1;
        if (i != last) {
            chunks[i] = chunks[last];
            if (active == last) active = i;
        }
        nchunks--;
    }
    static int nlog;
    if (n && nlog++ < 6) LOG("[vk] staging: %d idle chunk(s) given back (%llu MB), %d kept\n", n, (unsigned long long)(freed >> 20), nchunks);
#endif
}

VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = vk_transient(size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    return s;
}

// GPU -> CPU copies: one grow-only host-cached buffer (every user waits for the GPU and reads before the next one).
VkSpan vk_readback(uint64_t size) {
    static VkBuffer buf;
    static VmaAllocation mem;
    static uint8_t *ptr;
    static uint64_t cap;
    if (size > cap) {
        if (buf) { vk_wait_seq(submit_seq); vmaDestroyBuffer(g_vk.vma, buf, mem); }
        cap = (size + (16 << 20) - 1) & ~((16ull << 20) - 1);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info));
        ptr = info.pMappedData;
    }
    return (VkSpan){buf, 0, ptr};
}

// Guest memory copied once per submission: draws binding the same range share the copy.
#define GCOPY_TAB 4096
typedef struct { uint64_t addr, size, gen; VkSpan span; } GCopy;
static GCopy gcopy[GCOPY_TAB];
static uint64_t gcopy_gen = 1;  // bumped per submission and when guest memory is written mid-list
static void guest_copy_new_submission(void) { gcopy_gen++; }
static VkSpan guest_copy_in(GCopy *tab, uint32_t mask, uint64_t addr, uint64_t size, uint64_t *bytes) {
    uint64_t h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & mask;
    if (tab[slot].gen == gcopy_gen && tab[slot].addr == addr && tab[slot].size == size) return tab[slot].span;
    VkSpan s = vk_transient_copy(G2H((uint32_t)addr), size, 256);
    *bytes += size;
    tab[slot].addr = addr; tab[slot].size = size; tab[slot].gen = gcopy_gen; tab[slot].span = s;
    return s;
}
static VkSpan guest_copy(uint64_t addr, uint64_t size) {
    uint64_t before = stat_copy_bytes;
    VkSpan s = guest_copy_in(gcopy, GCOPY_TAB - 1, addr, size, &stat_copy_bytes);
    stat_gcopy_bytes += stat_copy_bytes - before;
    return s;
}
// (MHGU_PERF_IDXCOPY) a draw's indices: a table of their own, so they don't push the uniform buffers' copies out
#define GCOPY_IDX_TAB 1024
static GCopy gcopy_idx[GCOPY_IDX_TAB];
static VkSpan guest_copy_indices(uint64_t addr, uint64_t size) {
    uint64_t before = stat_idx_bytes;
    VkSpan s = guest_copy_in(gcopy_idx, GCOPY_IDX_TAB - 1, addr, size, &stat_idx_bytes);
    if (stat_idx_bytes == before) stat_idx_shared++;
    return s;
}

static bool guest_range_ok(uint64_t addr, uint64_t size) { return addr && addr < GUEST_SPACE && size <= GUEST_SPACE - addr; }

// ---- vertex cache: device-local copies of bound vertex ranges, kept across submissions and refreshed only when the
// CPU wrote their pages since (page write tracking, textures.c vk_pages_changed). The copy goes into the submission's
// upload command buffer, which starts with a full barrier (earlier submissions' reads are done) and completes before
// its draws. Without tracking for a range (outside the heap): not cached. MHGU_VK_VCACHE=0: off.
// Safety net (as textures.c's periodic full hash): a write the tracking missed would leave a copy stale for good, so an
// entry in use is copied again every 2-4 s (staggered by its slot), a slice of VC_SLICE bytes at a time and VC_SLICE
// bytes a frame at most: a big entry takes several frames, never one. Off on Windows, where GetWriteWatch is kept by
// the memory manager from the page tables: it sees every write into the guest window whoever makes it (the game's code,
// the runtime's own copies, the kernel's on ReadFile), and nothing maps those pages a second time (kernel.c's
// MapMemory and shared memory copy into the window). Android's tracking is mprotect's, which the kernel's own writes
// and its unwatched hot blocks get past (platform/android.c).
//   MHGU_VK_VC_SAFETY=0|1|2   off / on (any platform) / on and checked: a slice whose bytes changed since its last copy
//                             with no write reported there is logged (a write the tracking missed), with counts (a
//                             test: every copy is hashed too)
//   MHGU_PERF_VCNET=1         the net's turn every VC_NET_NS (30-32 s) instead of 2-4 s, VC_NET_SLICE (512 KB) a slice
//                             and a frame instead of VC_SLICE: what it copies again, about a fifteenth (it only covers
//                             a write the tracking missed, which =2 has not seen)
#define VC_TAB 4096
// Its budget: 384 MB; on a phone with 4 GB or less 192, and 128 at 3 GB or less (a 2.8 GB phone was closed by Android:
// #59, #156). Phones also drop entries no draw used for VC_IDLE_NS (vc_trim): the vertex data of areas left behind
// stayed until the budget was full. MHGU_VK_VC_MB=<n>: that budget.
static uint64_t vc_budget(void) {
    static uint64_t b;
    if (!b) {
        const char *e = getenv("MHGU_VK_VC_MB");
        uint64_t mb = 384;
#ifdef __ANDROID__
        uint64_t ram = vk_ram_mb();
        mb = ram <= 3200 ? 128 : ram <= 4400 ? 192 : 384;
#endif
        if (e && atoi(e) > 0) mb = (uint64_t)atoi(e);
        b = mb << 20;
        if (mb != 384) LOG("[vk] vertex cache: %llu MB at most\n", (unsigned long long)mb);
    }
    return b;
}
#define VC_BUDGET vc_budget()
// The budget is what the cache keeps, not what the frames may need: entries go least recently used first, and when the
// least recently used one was drawn from in the last VC_HOT_FRAMES frames, all of them were, and dropping it only means
// copying it whole again a moment later. With 0.1.9's 128 MB a town's menus dropped and copied ranges again all the
// time: the Palico info (the town and the menu's models, 173 MB) on a Moto g06 taken for a 2.8 GB phone, 1.4 GB a
// second, the translating thread 13 -> 35 ms a frame, 30 -> 16-26 FPS; on a player's 2.8 GB phone the item box at 12 FPS,
// the smithy and the canteen at 19-20. So the cache then goes over the budget, up to vc_ceiling (a phone's: twice it, 384 MB at
// most); past that a draw copies what it reads (guest_copy) and the entries in use stay. vc_trim brings it back under
// the budget once they are no longer drawn from.
#define VC_HOT_FRAMES 4
static uint64_t vc_ceiling(void) {
    uint64_t b = VC_BUDGET;
#ifdef __ANDROID__
    uint64_t top = 384ull << 20;
    return b >= top ? b : b * 2 < top ? b * 2 : top;
#else  // (a desktop keeps 384 MB: vc_trim, which brings it back, is a phone's)
    return b;
#endif
}
#define VC_IDLE_NS 20000000000ull
#define VC_FULL_NS 2000000000ull
#ifdef __ANDROID__
#define VC_SLICE (2ull << 20)
#else
#define VC_SLICE (1ull << 20)
#endif
#define VC_NET_NS 30000000000ull
#define VC_NET_SLICE (512ull << 10)
static uint64_t vc_full_ns = VC_FULL_NS, vc_slice = VC_SLICE;  // (the net's period and slice: MHGU_PERF_VCNET, vc_safety)
typedef struct {
    uint64_t addr, size, used_seq, checked_seq, full_t;  // (full_t: when its last full copy or safety pass ended)
    uint64_t used_frame;                                 // (the frame a draw last read it from: VC_HOT_FRAMES)
    uint32_t gen, full_off;                              // (full_off: the safety pass's next slice)
    uint64_t *sums;                                      // (checked: each slice's hash as last copied, 0 unknown)
    VkBuffer buf;
    VmaAllocation mem;
} VcEnt;
static VcEnt vc_tab[VC_TAB];
static uint64_t vc_bytes;
static _Atomic uint64_t vc_bytes_seen;  // (vc_bytes for the [mem] line's thread)
static uint64_t vc_frame = ~0ull, vc_t, vc_full_left;  // (the frame's number, its time, the safety net's bytes left)
uint64_t stat_vc_hits, stat_vc_refresh;
static uint64_t stat_vc_full;  // (MHGU_VK_STATS) draws whose range didn't fit under vc_ceiling (copied, guest_copy)
static bool vc_hot(const VcEnt *e) { return e->used_frame + VC_HOT_FRAMES > vk_frame_counter(); }

// (read once, by vcache's first call: the net's period and slice too)
static int vc_safety(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MHGU_VK_VC_SAFETY");
#ifdef _WIN32
        on = e ? atoi(e) : 0;
#else
        on = e ? atoi(e) : 1;
#endif
        const char *p = getenv("MHGU_PERF_VCNET");
        if (on && (p ? atoi(p) == 1 : true)) {  // (unset: on)
            vc_full_ns = VC_NET_NS, vc_slice = VC_NET_SLICE;
            LOG("[vk] vertex cache: the safety net every %llu s, %llu KB a frame (MHGU_PERF_VCNET)\n",
                (unsigned long long)(vc_full_ns / 1000000000ull), (unsigned long long)(vc_slice >> 10));
        }
    }
    return on;
}

static void vc_new_frame(void) {
    uint64_t f = vk_frame_counter();
    if (f != vc_frame) vc_frame = f, vc_t = plat_now_ns(), vc_full_left = vc_slice;
}

static bool vc_full_due(VcEnt *e) {  // (the safety net: e's turn to be copied again)
    vc_new_frame();
    return vc_t - e->full_t >= vc_full_ns + (uint64_t)((e - vc_tab) & 255) * 8000000ull;
}

// (MHGU_VK_VC_SAFETY=2) a slice's hash, never 0
static uint64_t vc_sum(const uint8_t *p, uint64_t n) {
    uint64_t h[4] = {0x9E3779B97F4A7C15ull ^ n, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull}, v[4];
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        memcpy(v, p + i, 32);
        for (int k = 0; k < 4; k++) h[k] = (h[k] ^ v[k]) * 0x100000001B3ull, h[k] ^= h[k] >> 29;
    }
    uint64_t r = h[0] ^ (h[1] * 0x9E3779B97F4A7C15ull) ^ (h[2] * 0xC2B2AE3D27D4EB4Full) ^ (h[3] * 0x165667B19E3779F9ull);
    for (; i < n; i++) r = (r ^ p[i]) * 0x100000001B3ull;
    return r | 1;
}

static bool vc_sums_alloc(VcEnt *e) {
    return e->sums || (e->sums = calloc((e->size + vc_slice - 1) / vc_slice, sizeof *e->sums));
}

// (MHGU_VK_VC_SAFETY=2) bytes [lo, hi) of e were just copied from guest memory: the slices inside get their hash, one
// copied in part becomes unknown (its other bytes are from an earlier copy)
static void vc_sums_copied(VcEnt *e, uint64_t lo, uint64_t hi) {
    if (!vc_sums_alloc(e)) return;
    const uint8_t *g = (const uint8_t *)G2H((uint32_t)e->addr);
    for (uint64_t k = lo / vc_slice; k * vc_slice < hi; k++) {
        uint64_t o = k * vc_slice, n = e->size - o < vc_slice ? e->size - o : vc_slice;
        e->sums[k] = lo <= o && o + n <= hi ? vc_sum(g + o, n) : 0;
    }
}

// (MHGU_VK_VC_SAFETY=2) the slice of e at off (n bytes at g), about to be copied again: changed since its last copy
// with the tracking reporting no write there since e's check (asked once more now: a write may have come after it)
static void vc_check(VcEnt *e, uint64_t off, uint64_t n, const uint8_t *g) {
    static uint64_t compared, changed, missed;
    uint64_t k = off / vc_slice, h = vc_sum(g, n);
    if (!vc_sums_alloc(e)) return;
    if (e->sums[k]) {
        compared++;
        if (e->sums[k] != h) {
            changed++;
            uint32_t now;
            if (vk_pages_changed(e->addr + off, n, e->gen, &now) == 0 && ++missed <= 20)
                LOG("[vk] vertex cache: %#llx+%llu KB changed with no write reported there (missed by the write tracking)\n",
                    (unsigned long long)(e->addr + off), (unsigned long long)(n >> 10));
        }
        static uint64_t said_t;  // (the counts every 2000 slices, and a minute after the last time at most)
        if (compared % 2000 == 0 || vc_t - said_t >= 60000000000ull) {
            said_t = vc_t;
            LOG("[vk] vertex cache check: %llu slices compared, %llu changed, %llu of those missed by the write tracking\n",
                (unsigned long long)compared, (unsigned long long)changed, (unsigned long long)missed);
        }
    }
    e->sums[k] = h;
}

// The safety net's next slice of e (at e->full_off): its bytes, if the frame's budget has room for them, else 0
static uint64_t vc_slice_next(VcEnt *e) {
    uint64_t n = e->size - e->full_off < vc_slice ? e->size - e->full_off : vc_slice;
    return n <= vc_full_left ? n : 0;
}

static void vc_slice_copy(VcEnt *e, uint64_t n) {
    uint64_t off = e->full_off;
    vc_full_left -= n;
    const uint8_t *g = (const uint8_t *)G2H((uint32_t)e->addr) + off;
    uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
    if (vc_safety() == 2) vc_check(e, off, n, g);
    VkSpan s = vk_transient_copy(g, n, 16);
    VkBufferCopy rg = {s.off, off, n};
    vkCmdCopyBuffer(vk_upload_cmd(), s.buf, e->buf, 1, &rg);
    if (HITCH_ON) hitch_add(HC_VC_SAFETY, plat_now_ns() - t0, n);
    stat_copy_bytes += n, stat_vc_net_bytes += n;
    e->full_off = off + n < e->size ? (uint32_t)(off + n) : 0;
    if (!e->full_off) e->full_t = vc_t;  // (the pass is over: the next in 2-4 s)
}

// (device.c vk_mem_summary, the [mem] line, from the presenting thread outside vk_lock) the staging ring's chunks and
// the vertex cache's bytes
void vk_exec_mem(uint64_t *staging, int *nstaging, uint64_t *vc) {
    *staging = atomic_load_explicit(&staging_bytes, memory_order_relaxed);
    *nstaging = atomic_load_explicit(&staging_n, memory_order_relaxed);
    *vc = atomic_load_explicit(&vc_bytes_seen, memory_order_relaxed);
}

static void vc_drop(VcEnt *e);

// (phones, every 2 s from vk_submit_extra) entries no submission used in the last VC_IDLE_NS go: their buffers are
// destroyed once the submissions that used them are done (vk_defer_buffer)
static void vc_trim(void) {
#ifdef __ANDROID__
    static uint64_t next, hist_t[16], hist_seq[16];
    static int nh, on = -1;
    if (on < 0) { const char *e = getenv("MHGU_VK_VC_TRIM"); on = !e || atoi(e) != 0; }
    uint64_t now = plat_now_ns();
    if (!on || now < next) return;
    next = now + 2000000000ull;
    // (the submission that was next VC_IDLE_NS ago: samples every 2 s; entries last used before it are idle)
    hist_t[nh % 16] = now, hist_seq[nh % 16] = vk_pending_seq(), nh++;
    uint64_t before = 0, idle = memory_short(now) ? 2000000000ull : VC_IDLE_NS;
    for (int i = 0; i < 16 && i < nh; i++)
        if (now - hist_t[i] >= idle && hist_seq[i] > before) before = hist_seq[i];
    uint64_t freed = 0;
    int n = 0;
    if (before && vc_bytes)
        for (int i = 0; i < VC_TAB; i++)
            if (vc_tab[i].buf && vc_tab[i].used_seq < before) freed += vc_tab[i].size, n++, vc_drop(&vc_tab[i]);
    static int nlog;
    if (n && nlog++ < 6) LOG("[vk] vertex cache: %d idle range(s) dropped (%llu MB), %llu MB kept\n", n, (unsigned long long)(freed >> 20),
                             (unsigned long long)(vc_bytes >> 20));
    // (over the budget, VC_HOT_FRAMES: back under it, least recently used first, with what the frames no longer draw from)
    uint64_t over_freed = 0;
    while (vc_bytes > VC_BUDGET) {
        int lru = -1;
        for (int i = 0; i < VC_TAB; i++)
            if (vc_tab[i].buf && !vc_hot(&vc_tab[i]) && (lru < 0 || vc_tab[i].used_seq < vc_tab[lru].used_seq)) lru = i;
        if (lru < 0) break;
        over_freed += vc_tab[lru].size;
        vc_drop(&vc_tab[lru]);
    }
    static int nover;
    if (over_freed && nover++ < 6)
        LOG("[vk] vertex cache over its %llu MB: %llu KB no longer drawn from dropped, %llu MB kept%s\n",
            (unsigned long long)(VC_BUDGET >> 20), (unsigned long long)(over_freed >> 10), (unsigned long long)(vc_bytes >> 20),
            vc_bytes > VC_BUDGET ? " (the rest still drawn from)" : "");
#endif
}

static void vc_drop(VcEnt *e) {
    vk_defer_buffer(e->buf, e->mem);
    vc_bytes -= e->size;
    atomic_store_explicit(&vc_bytes_seen, vc_bytes, memory_order_relaxed);
    free(e->sums);
    memset(e, 0, sizeof *e);
}

static bool vcache(uint64_t addr, uint64_t size, VkBuffer *buf) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("MHGU_VK_VCACHE"); on = !e || atoi(e) != 0; (void)vc_safety(); }
    if (!on || size < 4096 || size > (32u << 20)) return false;
    uint64_t pend = vk_pending_seq();
    uint64_t h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VC_TAB - 1), free_slot = ~0u, old = ~0u;
    VcEnt *e = NULL;
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VC_TAB - 1)) {
        VcEnt *c = &vc_tab[slot];
        if (c->buf && c->addr == addr && c->size == size) { e = c; break; }
        if (!c->buf && free_slot == ~0u) free_slot = slot;
        if (c->buf && c->used_seq < pend && (old == ~0u || c->used_seq < vc_tab[old].used_seq)) old = slot;
    }
    bool copy = false;
    uint32_t since = 0;       // refresh: only pages written after this generation
    uint64_t so = 0, sn = 0;  // the safety net's slice copied below: its offset and bytes (none: 0)
    if (!e) {
        uint32_t now = 0;
        if (vk_pages_changed(addr, size, 0, &now) < 0) return false;  // (no tracking here: per-submission copies)
        if (free_slot == ~0u) {
            if (old == ~0u || vc_hot(&vc_tab[old])) return false;
            vc_drop(&vc_tab[old]);
            free_slot = old;
        }
        // the least recently used go, unless the last frames drew from it, and so from all of them (VC_HOT_FRAMES): then
        // over the budget up to the ceiling. (hot_frame: the frame that found so; all stay in use till it ends)
        static uint64_t hot_frame = ~0ull;
        uint64_t frame = vk_frame_counter();
        while (vc_bytes + size > VC_BUDGET) {
            if (hot_frame != frame) {
                int lru = -1;
                for (int i = 0; i < VC_TAB; i++)
                    if (vc_tab[i].buf && vc_tab[i].used_seq < pend && (lru < 0 || vc_tab[i].used_seq < vc_tab[lru].used_seq)) lru = i;
                if (lru < 0) return false;
                if (!vc_hot(&vc_tab[lru])) { vc_drop(&vc_tab[lru]); continue; }
                hot_frame = frame;
            }
            static bool said_over, said_full;
            if (vc_bytes + size > vc_ceiling()) {
                stat_vc_full++;
                if (!said_full) said_full = true, LOG("[vk] vertex cache: at %llu MB, all of it drawn from: a draw past that copies what it reads\n",
                                                      (unsigned long long)(vc_ceiling() >> 20));
                return false;
            }
            if (!said_over) said_over = true, LOG("[vk] vertex cache: the frames draw from more than its %llu MB: kept over it while they do, up to %llu MB\n",
                                                  (unsigned long long)(VC_BUDGET >> 20), (unsigned long long)(vc_ceiling() >> 20));
            break;
        }
        e = &vc_tab[free_slot];
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
        if (vk_create_buffer(&bi, &e->buf, &e->mem) != VK_SUCCESS) { memset(e, 0, sizeof *e); return false; }
        if (HITCH_ON) hitch_add(HC_VC_NEW, plat_now_ns() - t0, size);
        e->addr = addr; e->size = size; e->gen = now; e->checked_seq = pend;
        vc_bytes += size;
        atomic_store_explicit(&vc_bytes_seen, vc_bytes, memory_order_relaxed);
        copy = true;
    } else if (e->checked_seq != pend) {  // first use in this submission: written since the last copy?
        uint32_t now = 0;
        int ch = vk_pages_changed(addr, size, e->gen, &now);
        e->checked_seq = pend;
        if (ch != 0) { copy = true; since = ch > 0 ? e->gen : 0; e->gen = now; }
        if ((since || !copy) && vc_safety() && vc_full_due(e)) {  // (not a full copy already: the safety net's turn)
            sn = vc_slice_next(e);
            so = sn ? e->full_off : 0;
        }
    }
    if (copy && since) {  // the pages written since the last refresh, as runs (none inside the slice: it has them)
        extern uint32_t vk_page_gen(uint64_t addr);
        VkBufferCopy rg[64];
        int n = 0;
        uint64_t first = addr & ~4095ull;
        for (uint64_t p = first; p < addr + size; p += 4096) {
            if (vk_page_gen(p) <= since) continue;
            uint64_t lo = p < addr ? addr : p, hi = p + 4096 > addr + size ? addr + size : p + 4096;
            if (sn && lo - addr < so + sn && hi - addr > so) {
                if (e->sums) e->sums[so / vc_slice] = 0;  // (written, as reported: nothing to check there)
                if (lo - addr < so) hi = addr + so;
                else if (hi - addr > so + sn) lo = addr + so + sn;
                else continue;
            }
            if (n && rg[n - 1].dstOffset + rg[n - 1].size == lo - addr) { rg[n - 1].size += hi - lo; continue; }
            if (n == 64) { n = -1; break; }
            rg[n++] = (VkBufferCopy){lo - addr, lo - addr, hi - lo};  // (srcOffset fixed up below)
        }
        if (n < 0) since = 0;  // (scattered: the whole range)
        else {
            for (int k = 0; k < n; k++) {
                VkSpan s = vk_transient_copy((uint8_t *)G2H((uint32_t)addr) + rg[k].dstOffset, rg[k].size, 16);
                rg[k].srcOffset = s.off;
                vkCmdCopyBuffer(vk_upload_cmd(), s.buf, e->buf, 1, &rg[k]);
                stat_copy_bytes += rg[k].size, stat_vc_pages_bytes += rg[k].size;
                if (vc_safety() == 2) vc_sums_copied(e, rg[k].dstOffset, rg[k].dstOffset + rg[k].size);
            }
            stat_vc_refresh++;
        }
    }
    if (copy && !since) {
        uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
        VkSpan s = vk_transient_copy(G2H((uint32_t)addr), size, 16);
        if (HITCH_ON) hitch_add(HC_VC_COPY, plat_now_ns() - t0, size);
        VkBufferCopy rg = {s.off, 0, size};
        vkCmdCopyBuffer(vk_upload_cmd(), s.buf, e->buf, 1, &rg);
        stat_copy_bytes += size, stat_vc_whole_bytes += size;
        stat_vc_refresh++;
        if (vc_safety() == 2) vc_sums_copied(e, 0, size);
        vc_new_frame();
        e->full_t = vc_t, e->full_off = 0;  // (a safety pass under way starts again)
    } else if (sn) {
        vc_slice_copy(e, sn);
        stat_vc_refresh++;
    } else {
        stat_vc_hits++;
    }
    e->used_seq = pend;
    e->used_frame = vk_frame_counter();
    *buf = e->buf;
    return true;
}

// ================================================================== worker threads (shader / pipeline builds)
// Two lists per queue: jobs a draw is (or may be) waiting for, and the start-up prewarm's (low: taken only when the
// first list is empty).
typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
typedef struct {
    Job *head, *tail, *lhead, *ltail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool started;
    int threads;  // (0: compile_workers)
    // (the log's "caught up" line: when the builds queued at the start are done; 1 watching, 2 still being queued)
    int busy, watch;
    uint64_t watch_t0, watch_jobs;
} JobQueue;
static JobQueue compile_q = {NULL, NULL, NULL, NULL, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false, 0};
static JobQueue translate_q = {NULL, NULL, NULL, NULL, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false, 1};

// The compile workers (SPIR-V, modules, pipelines): 3. A desktop has cores to spare: half its logical processors, 3 to
// 6, so a burst of new pipelines ends sooner; on Windows below the game's threads' priority, so they don't compete.
// A PowerVR: one, its driver compiling one thing at a time anyway (device.c one_compiler_install).
static int compile_workers(void) {
    const char *e = getenv("MHGU_COMPILE_WORKERS");
    if (e && atoi(e) > 0) return atoi(e) < 16 ? atoi(e) : 16;
    if (vk_gpu_powervr()) return 1;
#ifdef __ANDROID__
    return 3;
#else
#ifdef _WIN32
    long n = SDL_GetNumLogicalCPUCores() / 2;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN) / 2;
#endif
    return n < 3 ? 3 : n > 6 ? 6 : (int)n;
#endif
}

// (under q->lock) the builds queued at the start all done (progcache_open): logged once, with how long they took
static void queue_caught_up(JobQueue *q) {
    if (q->watch != 1 || q->head || q->lhead || q->busy) return;
    q->watch = 0;
    LOG("[shader] the builds queued at the start are done: %llu in %.1f s\n", (unsigned long long)q->watch_jobs,
        (plat_now_ns() - q->watch_t0) / 1e9);
}

static void *worker(void *a) {
    JobQueue *q = a;
    {
        static _Atomic int nc, nt;
        char name[16];
        snprintf(name, sizeof name, q == &compile_q ? "shader-%d" : "prewarm-%d",
                 atomic_fetch_add(q == &compile_q ? &nc : &nt, 1));
        plat_thread_name(name);
    }
#ifdef _WIN32
    if (q == &compile_q) plat_thread_below_normal();
#elif defined(__ANDROID__)
    // (below the game too: the start's thousands of shaders kept every big core busy, the phone hot in its first minutes
    // even on a Snapdragon 8 Gen 5, #91, #113; at nice 10 the scheduler gives them the efficient cores first)
    if (q == &compile_q) setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
#endif
    for (;;) {
        pthread_mutex_lock(&q->lock);
        while (!q->head && !q->lhead) pthread_cond_wait(&q->cond, &q->lock);
        Job *j;
        if (q->head) {
            j = q->head;
            q->head = j->next;
            if (!q->head) q->tail = NULL;
        } else {
            j = q->lhead;
            q->lhead = j->next;
            if (!q->lhead) q->ltail = NULL;
        }
        q->busy++;
        pthread_mutex_unlock(&q->lock);
        j->fn(j->arg);
        free(j);
        pthread_mutex_lock(&q->lock);
        q->busy--;
        if (q->watch) q->watch_jobs++, queue_caught_up(q);
        pthread_mutex_unlock(&q->lock);
    }
    return NULL;
}

// (not for a PowerVR any more: built on the render thread, its start built every saved program and pipeline before the
// first picture, a black screen for minutes on a Helio G35, #157; it gets one worker and one compile at a time instead)
static bool sync_shaders(void) {
    static int v = -1;
    if (v < 0) {
        v = getenv("MHGU_SYNC_SHADERS") != NULL;
        if (v) LOG("[vk] shaders and pipelines built on one thread, when first needed\n");
    }
    return v;
}

static void enqueue(JobQueue *q, bool low, void (*fn)(void *), void *arg) {
    if (sync_shaders()) { fn(arg); return; }
    pthread_mutex_lock(&q->lock);
    if (!q->started) {
        q->started = true;
        int n = q->threads ? q->threads : compile_workers();
        for (int i = 0; i < n; i++) { pthread_t t; pthread_create(&t, NULL, worker, q); }
    }
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    Job **head = low ? &q->lhead : &q->head, **tail = low ? &q->ltail : &q->tail;
    if (*tail) (*tail)->next = j; else *head = j;
    *tail = j;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

// The translator isn't re-entrant: one thread translates, the compiles run on a few workers.
static pthread_mutex_t translate_lock = PTHREAD_MUTEX_INITIALIZER;
static void run_translate(void (*fn)(void *), void *arg) { enqueue(&translate_q, false, fn, arg); }
static void run_job(void (*fn)(void *), void *arg) { enqueue(&compile_q, false, fn, arg); }
void vk_run_job(void (*fn)(void *), void *arg) { run_job(fn, arg); }  // (fsr.c: its capture shaders / pipelines)

// A queued job a draw is waiting for goes to the front of its queue (the queues are FIFO, and an area load queues a
// few hundred builds at once), from the prewarm's list too.
static void promote(JobQueue *q, void *arg) {
    pthread_mutex_lock(&q->lock);
    for (int low = 0; low < 2; low++) {
        Job **head = low ? &q->lhead : &q->head, **tail = low ? &q->ltail : &q->tail;
        for (Job *j = *head, *prev = NULL; j; prev = j, j = j->next) {
            if (j->arg != arg) continue;
            if (!low && !prev) goto done;  // (already first)
            if (prev) prev->next = j->next; else *head = j->next;
            if (*tail == j) *tail = prev;
            j->next = q->head;
            q->head = j;
            if (!q->tail) q->tail = j;
            goto done;
        }
    }
done:
    pthread_mutex_unlock(&q->lock);
}

// Build states (0 building, 1 ready, 2 failed) change under ready_lock so a draw can wait for one. Windows: the draw
// waits on ready_event instead, which set_state sets while one waits (winpthreads' timed waits end on the system's
// 15.6 ms tick whatever is asked: a frame's 6 ms budget took up to 22.5 ms).
static pthread_mutex_t ready_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready_cond = PTHREAD_COND_INITIALIZER;
#ifdef _WIN32
static void *ready_event;  // (vk_exec_init)
static _Atomic int ready_waiters;
#endif
static void set_state(_Atomic int *state, int v, uint64_t t_req) {
    if (t_req) {
        uint64_t d = plat_now_ns() - t_req;
        atomic_fetch_add(&stat_lat_n, 1);
        atomic_fetch_add(&stat_lat_ns, d);
        atomic_max(&stat_lat_max, d);
        if (HITCH_ON) hitch_add(HC_BUILD_READY, d, 0);
    }
    pthread_mutex_lock(&ready_lock);
    atomic_store(state, v);
    pthread_cond_broadcast(&ready_cond);
    pthread_mutex_unlock(&ready_lock);
#ifdef _WIN32
    if (atomic_load(&ready_waiters)) plat_event_set(ready_event);
#endif
}

// Until state's build is done or deadline. 1 ms slices: a program's GLSL compile is queued once its translation is
// done, and goes to the front too.
static void ready_wait(_Atomic int *state, void *job, uint64_t t0, uint64_t deadline) {
#ifdef _WIN32
    if (ready_event) {
        atomic_fetch_add(&ready_waiters, 1);
        for (uint64_t now = t0; !atomic_load(state) && now < deadline; now = plat_now_ns()) {
            plat_event_wait_until(ready_event, now + 1000000 < deadline ? now + 1000000 : deadline);
            if (!atomic_load(state)) promote(&compile_q, job);
        }
        atomic_fetch_sub(&ready_waiters, 1);
        return;
    }
#endif
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);  // (the condition variable's clock)
    int64_t rt_off = (int64_t)rt.tv_sec * 1000000000ll + rt.tv_nsec - (int64_t)t0;
    pthread_mutex_lock(&ready_lock);
    for (uint64_t now = t0; !atomic_load(state) && now < deadline; now = plat_now_ns()) {
        uint64_t until = now + 1000000 < deadline ? now + 1000000 : deadline;
        int64_t abs_ns = (int64_t)until + rt_off;
        struct timespec ts = {(time_t)(abs_ns / 1000000000ll), (long)(abs_ns % 1000000000ll)};
        pthread_cond_timedwait(&ready_cond, &ready_lock, &ts);
        if (!atomic_load(state)) {
            pthread_mutex_unlock(&ready_lock);
            promote(&compile_q, job);
            pthread_mutex_lock(&ready_lock);
        }
    }
    pthread_mutex_unlock(&ready_lock);
}

#ifndef __ANDROID__
// The frames' interval: the median of the last 16 (frame_dt_note, at each frame's first command list)
static uint32_t frame_dt[16];
static unsigned frame_dt_n;
static void frame_dt_note(void) {
    static uint64_t f0, t0;
    uint64_t f = vk_frame_counter();
    if (f == f0) return;
    uint64_t now = plat_now_ns();
    if (t0 && f > f0) {
        uint64_t dt = (now - t0) / (f - f0);
        frame_dt[frame_dt_n++ % 16] = dt < 100000000 ? (uint32_t)dt : 100000000;  // (a pause, a load: 0.1 s)
    }
    f0 = f, t0 = now;
}
static uint64_t frame_dt_median(void) {
    unsigned n = frame_dt_n < 16 ? frame_dt_n : 16;
    uint32_t v[16];
    for (unsigned i = 0; i < n; i++) {  // (sorted as they are put in)
        unsigned j = i;
        for (; j && v[j - 1] > frame_dt[i]; j--) v[j] = v[j - 1];
        v[j] = frame_dt[i];
    }
    return n ? v[n / 2] : 0;
}
static bool wait_new_builds(void) {
    static int v = -1;
    if (v < 0) v = getenv("MHGU_PIPE_WAIT_NEW") != NULL;
    return v;
}
#endif

// A frame's budget for draws waiting for builds: MHGU_PIPE_WAIT_MS, else 6 ms; a desktop's a quarter of the frame
// interval up to that (at 160 Hz 1.6 ms: a 6.25 ms frame that waits 6 more misses its refresh, the picture repeats).
static int64_t frame_wait_budget(void) {
    static int64_t fixed = -2;  // (-1: MHGU_PIPE_WAIT_MS not set)
    if (fixed == -2) {
        const char *e = getenv("MHGU_PIPE_WAIT_MS");
        fixed = !e ? -1 : atoi(e) > 0 ? atoi(e) * 1000000ll : 0;
    }
    if (fixed >= 0) return fixed;
#ifdef __ANDROID__
    return 6000000;
#else
    uint64_t dt = frame_dt_median();
    return dt && dt / 4 < 6000000 ? (int64_t)(dt / 4) : 6000000;
#endif
}

// (Windows) a wake-up comes some 0.1-0.5 ms after its time: the waits end that much early, so a frame's stay within
// its budget
#ifdef _WIN32
#define WAIT_SLACK_NS 200000ll
#else
#define WAIT_SLACK_NS 0ll
#endif

// A draw whose program / pipeline is still building waits for it, up to the budget per frame in all, rather than
// being skipped at once: a build from the caches takes about a millisecond (SPIR-V from cache/spirv, the driver's
// pipeline cache), so the effect appears in its first frame instead of a few frames later. cached: such a build (a
// program loading from the program cache, a pipeline of pipelines.bin / pipelines.list). A desktop skips the others at
// once: a pipeline first made at a draw is the driver's whole compile (40-70 ms measured on a PC) and a program being
// translated more, so a wait would cost every frame its budget until then for nothing. (MHGU_PIPE_WAIT_NEW=1: they wait
// too.) at_least_ns: a draw that mustn't be skipped waits that long whatever is left of the budget.
//
// The quest photo (Quest Rewards / Reward Box screen): at the clear the game copies the scene into its photo buffer
// once and never again, so a draw skipped then leaves the photo as whatever the buffer's memory held: black (the R9),
// or other textures' data (RADV, Steam Deck). The game's request (0xa6f97c, hooked in tools/gen.py) opens a window of
// PHOTO_FRAMES frames in which every draw waits for its build, up to PHOTO_WAIT_NS: the capture and the scene it copies.
#define PHOTO_FRAMES 8
#define PHOTO_WAIT_NS 2000000000ll
static _Atomic uint64_t photo_until;
void backend_photo_requested(void) {
    uint64_t f = vk_frame_counter();
    if (atomic_exchange(&photo_until, f + PHOTO_FRAMES) < f)
        LOG("[vk] quest photo requested (frame %llu): draws wait for their shaders until it is taken\n", (unsigned long long)f);
}
static int64_t wait_budget_ns;
static uint64_t wait_frame = ~0ull;
static bool await_build(_Atomic int *state, void *job, int64_t at_least_ns, bool cached) {
    int s = atomic_load(state);
    if (s) return s == 1;
    promote(&translate_q, job);
    promote(&compile_q, job);
    uint64_t f = vk_frame_counter();
    uint64_t photo = atomic_load(&photo_until);
    if (photo && f <= photo && at_least_ns < PHOTO_WAIT_NS) at_least_ns = PHOTO_WAIT_NS;
    if (f != wait_frame) wait_frame = f, wait_budget_ns = frame_wait_budget();
#ifndef __ANDROID__
    if (!cached && !at_least_ns && !wait_new_builds()) {
        if (HITCH_ON) hitch_add(HC_BUILD_COLD, 0, 0);
        return false;
    }
#else
    (void)cached;
#endif
    int64_t allowed = wait_budget_ns > at_least_ns ? wait_budget_ns : at_least_ns;
    if (allowed <= WAIT_SLACK_NS) {
        if (HITCH_ON) hitch_add(HC_BUILD_SKIP, 0, 0);  // ((debug) MHGU_HITCHLOG: the frame's budget is spent)
        return false;
    }
    uint64_t t0 = plat_now_ns();
    ready_wait(state, job, t0, t0 + (uint64_t)(allowed - WAIT_SLACK_NS));
    s = atomic_load(state);
    uint64_t d = plat_now_ns() - t0;
    wait_budget_ns -= (int64_t)d;
    stat_waited_ns += d;
    stat_waits++;
    if (s == 1) stat_rescued++;
    if (HITCH_ON) hitch_add(at_least_ns ? HC_BUILD_LONG : HC_BUILD_WAIT, d, s != 1);
    return s == 1;
}

// ================================================================== state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

static struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart, depth_clamp;
    uint32_t restart_index;
    float alpha_ref;
    uint64_t copy_row_stride, copy_image_stride;
} st;

// A draw skipped because its program / pipeline is still building. MHGU_SKIP_LOG=1 logs each one with its shaders and
// colour target 0 (which pass lost it; a one-off pass, like the quest photo, stays wrong).
static void skip_draw(uint64_t *cause) {
    static uint64_t last = ~0ull;
    static int log = -1;
    uint64_t f = vk_frame_counter();
    if (f != last) last = f, stat_skip_frames++;
    stat_skipped++;
    (*cause)++;
    if (log < 0) log = getenv("MHGU_SKIP_LOG") != NULL;
    if (log && st.program) {
        NvnTexture *t = st.rt.ncolor ? st.rt.color[0] : NULL;
        LOG("[skip] frame %llu %s vs %llx fs %llx | rt0 %p %ux%u f%#x\n", (unsigned long long)f,
            cause == &stat_skip_prog ? "program" : "pipeline", (unsigned long long)st.program->stages[0].code_addr,
            (unsigned long long)st.program->stages[1].code_addr, (void *)t, t ? t->width : 0, t ? t->height : 0, t ? t->format : 0);
    }
}

uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);

static void st_defaults(void) {
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

void vk_exec_init(void) {
    perf_init();
    st_defaults();
    // (a Vulkan 1.1 driver: off unless set, as the translator's VSTRIM, device.c)
    perf.rodepth = perf_env("MHGU_PERF_RODEPTH", g_vk.api12 ? 1 : 0);
    perf.shadow_clear = perf_env("MHGU_PERF_SHADOW_CLEAR", g_vk.api12 ? 2 : 0);
    perf.fxaa_copydraw = perf_env("MHGU_PERF_FXAA_COPYDRAW", g_vk.api12 ? 1 : 0);
    if (!g_vk.api12) LOG("[perf] a Vulkan 1.1 driver: VSTRIM, DYNUBO, RODEPTH, SHADOW_CLEAR, FXAA_COPYDRAW and HUDSTART_FUSE off unless set\n");
    if (!g_vk.has_timeline) fence_init();
#ifdef _WIN32
    ready_event = plat_event_new();  // (NULL: the draws wait on winpthreads' condition variable)
#endif
#ifndef __ANDROID__
    chunks_ahead();
#endif
}

// ================================================================== render passes and framebuffers
static pthread_mutex_t rp_lock = PTHREAD_MUTEX_INITIALIZER;  // (pipeline workers ask for render passes too)

// noload: attachments (bit i colour i, bit 8 depth / stencil) the pass begins by clearing (LOAD_OP_CLEAR, the values
// given when it begins) instead of reading their old contents in, because its first commands clear them whole
// (begin_pass). A tile-based GPU (every phone's) otherwise reads each attachment from memory into its tiles at the start
// of a pass. Not DONT_CARE with the clears after it: an Adreno 830 then drew the scene black but for some tiles (its
// early depth test, set up at the pass's start, seems not to take a clear inside it). Load operations don't change a
// render pass's compatibility, so pipelines and framebuffers are made with the variant that loads everything (noload 0).
// RP_RO_DEPTH in noload: the depth / stencil target is read-only in the pass (MHGU_PERF_RODEPTH, resolve_feedback): its
// layout there DEPTH_STENCIL_READ_ONLY_OPTIMAL, from and back to GENERAL around it (layouts don't change compatibility
// either), loaded, never cleared as the pass begins.
#define RP_RO_DEPTH (1u << 9)
static VkRenderPass render_pass(const VkFormat *color, uint32_t ncolor, VkFormat depth, uint32_t samples, uint32_t noload) {
    static struct { VkFormat c[8], d; uint32_t n, s, nl; VkRenderPass rp; } tab[512];
    static int ntab;
    pthread_mutex_lock(&rp_lock);
    for (int i = 0; i < ntab; i++)
        if (tab[i].n == ncolor && tab[i].d == depth && tab[i].s == samples && tab[i].nl == noload &&
            !memcmp(tab[i].c, color, ncolor * sizeof *color)) {
            VkRenderPass rp = tab[i].rp;
            pthread_mutex_unlock(&rp_lock);
            return rp;
        }
    VkAttachmentDescription a[9];
    VkAttachmentReference cref[8], dref;
    uint32_t na = 0;
    // (MSAA: the multisampled images stay in their attachment layouts: textures.c vk_tex_ms)
    VkImageLayout cl = samples > 1 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    VkImageLayout dl = samples > 1 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    for (uint32_t i = 0; i < ncolor; i++) {
        if (color[i] == VK_FORMAT_UNDEFINED) { cref[i] = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL}; continue; }
        a[na] = (VkAttachmentDescription){.format = color[i], .samples = (VkSampleCountFlagBits)samples,
                                          .loadOp = (noload >> i) & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                          .initialLayout = cl, .finalLayout = cl};
        cref[i] = (VkAttachmentReference){na++, cl};
    }
    if (depth != VK_FORMAT_UNDEFINED) {
        bool ro = (noload & RP_RO_DEPTH) && samples == 1;
        VkAttachmentLoadOp dload = (noload >> 8) & 1 && !ro ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a[na] = (VkAttachmentDescription){.format = depth, .samples = (VkSampleCountFlagBits)samples,
                                          .loadOp = dload, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .stencilLoadOp = dload, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
                                          .initialLayout = dl, .finalLayout = dl};
        dref = (VkAttachmentReference){na++, ro ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : dl};
    }
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = ncolor,
                               .pColorAttachments = cref, .pDepthStencilAttachment = depth != VK_FORMAT_UNDEFINED ? &dref : NULL};
    // (the full barriers around passes order them; these keep the implicit external dependencies broad)
    VkSubpassDependency dep[2] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_STAGES_ALL, VK_STAGES_ALL & ~(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT),
         VK_ACCESS_ALL_WRITES, VK_ACCESS_ALL, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_STAGES_ALL & ~(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT), VK_STAGES_ALL,
         VK_ACCESS_ALL_WRITES, VK_ACCESS_ALL, 0}};
    VkRenderPassCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = na, .pAttachments = a,
                                 .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = dep};
    VkRenderPass rp;
    VK_CHECK(vkCreateRenderPass(g_vk.dev, &ci, NULL, &rp));
    if (ntab < 512) {
        memcpy(tab[ntab].c, color, ncolor * sizeof *color);
        tab[ntab].n = ncolor; tab[ntab].d = depth; tab[ntab].s = samples; tab[ntab].nl = noload; tab[ntab].rp = rp;
        ntab++;
    }
    pthread_mutex_unlock(&rp_lock);
    return rp;
}

VkRenderPass vk_render_pass(const VkFormat *color, uint32_t ncolor, VkFormat depth, uint32_t samples) {
    return render_pass(color, ncolor, depth, samples, 0);
}

#define FB_TAB 1024
typedef struct { VkRenderPass rp; VkImageView v[9]; uint32_t n, w, h; VkFramebuffer fb; } FbEnt;
static FbEnt fb_tab[FB_TAB];

VkFramebuffer vk_framebuffer(VkRenderPass rp, const VkImageView *views, uint32_t n, uint32_t w, uint32_t h) {
    uint64_t hh = (uint64_t)(uintptr_t)rp * 0x9E3779B97F4A7C15ull ^ ((uint64_t)w << 32 | h);
    for (uint32_t i = 0; i < n; i++) hh = (hh ^ (uint64_t)(uintptr_t)views[i]) * 0xC2B2AE3D27D4EB4Full;
    uint32_t slot = (uint32_t)(hh ^ (hh >> 32)) & (FB_TAB - 1);
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (FB_TAB - 1)) {
        FbEnt *e = &fb_tab[slot];
        if (e->fb && e->rp == rp && e->n == n && e->w == w && e->h == h && !memcmp(e->v, views, n * sizeof *views)) return e->fb;
        if (!e->fb) break;
    }
    VkFramebufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = n,
                                  .pAttachments = views, .width = w, .height = h, .layers = 1};
    VkFramebuffer fb;
    VK_CHECK(vkCreateFramebuffer(g_vk.dev, &ci, NULL, &fb));
    FbEnt *e = &fb_tab[slot];
    if (e->fb) vk_defer_framebuffer(e->fb);  // (evicted)
    e->rp = rp; e->n = n; e->w = w; e->h = h; e->fb = fb;
    memcpy(e->v, views, n * sizeof *views);
    return fb;
}

void vk_framebuffers_forget_view(VkImageView v) {
    for (int i = 0; i < FB_TAB; i++) {
        FbEnt *e = &fb_tab[i];
        if (!e->fb) continue;
        for (uint32_t k = 0; k < e->n; k++)
            if (e->v[k] == v) { vk_defer_framebuffer(e->fb); e->fb = VK_NULL_HANDLE; break; }
    }
}

// ================================================================== passes
static VkFormat pass_color[8], pass_depth;
static uint32_t pass_ncolor, pass_samples = 1, pass_w, pass_h;
static bool ui_relax_scissor;  // the draw being set up moved a HUD piece out from under the game's scissor
static int ui_scissor_v = -1;  // 16:10: the draw being set up is a HUD piece anchored so (0 top, 1 centre, 2 bottom),
                               // and its scissor moves with it
static bool ui_scissor_mid;    // wider than 16:9: the draw being set up is a piece kept in the middle 16:9 (the Meownster
                               // Hunters' map), and its scissor narrows about the middle with it
static bool ui_menu_pass;  // this pass drew a layer 97 menu before (the windows of layer 100 then sit beside Top-anchored ones)
static bool ui_window_pass;  // and one of a menu's windows (not the chat's log or the Quest Complete stamp, drawn there too)
static uint64_t ui_window_frame;  // the frame (g_present_count) that drew one of a menu's windows last, 0 none yet
// The HUD quads this pass drew last (16:9 canvas place, vertical anchor 0 top / 1 centre / 2 bottom, horizontal 0 left /
// 1 right / 2 centre): a model preview follows the window drawn over it (ui_preview_map)
#define UI_RECENT 256
static struct { float x, y; uint8_t vanchor, hanchor; } ui_recent[UI_RECENT];
static uint32_t ui_nrecent;  // (quads recorded this pass; the slot is the count modulo UI_RECENT)
static bool ui_loading_pass;  // this pass draws the loading screen (its backdrop, layer 110, came first)
static bool ui_title_pass;  // this pass draws the title screen (its layout, layer 108 at the canvas centre, came first)
static bool ui_frontend_pass;  // this pass draws a front-end screen (Main Menu, Character Select: its 16:9 backdrop, layer
                               // 250 at the canvas centre, came first)
static bool ui_stamp_pass;  // this pass draws the Quest Complete stamp (its borders, layer 97 at x 640, y 53 / 687, came first)
static bool ui_results_pass, ui_results_last;  // this pass / the last pass with HUD in it draws the quest results screen
static bool ui_meownster_pass;  // this pass draws the Meownster Hunters return screen (its map frame, layer 105, came first)
// A menu window of the game's drawn in this frame: the touch controls' menu detection (runtime/touch.cpp: the menu pad
// shows), and on a desktop the mouse in the game's menus, which it keeps for a moment after the menu's cursor stops
// (runtime/hle/mousemenu.c)
static void ui_menu_drawn(void) {
    touch_hud_menu();
#ifndef __ANDROID__
    mousemenu_menu();
#endif
}
volatile int g_hudtrace_active;  // (debug) MHGU_CONTROL "hudtrace": log every HUD quad of the next frame and its anchor (device.c)

// The hunt's HUD with a menu of the game's open. Its windows (layer 97) are not what tells: the chat's log, a chat
// bubble, a bow's or a bowgun's aim and the Quest Complete stamp are drawn there too, while the hunter is played, and
// the parts drawn after them went back to the game's places (they jumped while a line of chat showed). A menu is open
// as the touch controls tell it (touch_game_menu: a window that came at a press, or the hunter not played), read once
// a pass. With none, every piece that falls in a part's region is the part's, and its place (layer, the game's x, y)
// is kept, with the time. With one, a piece is the part's only at a place the part had in the last HUD_SEEN_NS (each
// such piece keeps it), so that the menu's own pieces (its button hints at the bottom right, the Guild Card's plate)
// stay where the game puts them while the part stays where the player has it. Measured: the hunt's pause menu and
// pouch draw the status first, then their windows, and no item bar, Hunter Arts, target camera or minimap at all
static bool ui_menu_open;  // (touch_game_menu, at the pass's start)
#define HUD_SEEN 48
#define HUD_SEEN_PARTS HP_COUNT
#define HUD_SEEN_NS (1500 * 1000000ull)
static struct { float z, x, y; uint64_t ns; } hud_seen[HUD_SEEN_PARTS][HUD_SEEN];
static uint32_t hud_nseen[HUD_SEEN_PARTS];
// Which part of the status (layer 103 above the item bar, not its ammo list) a piece is, by its place on the 16:9 canvas
// (measured with a Great Sword and two Felynes: the clock's six pieces at the top left; the health and stamina bars' at
// x 102 and their ends, y 20..33; the sharpness's at 110, 38 and 116, 32; the plates' from y 105). Other weapons' gauges,
// the buffs' icons, other hunters' plates: by the same regions (below the bars: the gauges), and a piece in none of them
// goes with the part whose box is nearest (*nearest: hudtrace says so), so that nothing is left behind as its neighbours
// move
static int hud_status_part(float x, float y, bool *nearest) {
    *nearest = false;
    if (x < 98.0f && y < 100.0f) return HP_CLOCK;
    if ((fabsf(x - 116.0f) <= 1.0f && fabsf(y - 32.0f) <= 1.0f) || (x >= 104.0f && y >= 36.0f && y < 100.0f)) return HP_GAUGES;
    if (x >= 98.0f && y < 36.0f) return HP_BARS;
    if (x < 420.0f && y >= 100.0f) return HP_PARTY;
    *nearest = true;
    int best = HP_CLOCK;
    float bd = 1e30f;
    for (int p = HP_CLOCK; p <= HP_PARTY; p++) {
        const float *b = HUD_BOX[p];
        float dx = x < b[0] ? b[0] - x : x > b[2] ? x - b[2] : 0.0f, dy = y < b[1] ? b[1] - y : y > b[3] ? y - b[3] : 0.0f;
        if (dx * dx + dy * dy < bd) bd = dx * dx + dy * dy, best = p;
    }
    return best;
}
// The target camera's pieces (layer 100): one large monster's icon at x 1200 (y 42 and 57) with the change-target
// hint under it (y 82); two monsters' icons side by side at x 1170 and 1230 (y 42), the hint still at 1200 (measured on
// a Motorola g06 in the Arena, 2026-11-09: only the one at 1200 was taken, and the two icons stayed under the Hunter Arts
// the touch arrangement puts there, #79). Not the shoutout list's header on the same layer (y 79..80, x 1132..1228)
static bool target_piece(float x, float y) {
    if (fabsf(x - 1200.0f) < 1.0f) return y < 100.0f;
    return (fabsf(x - 1170.0f) < 1.0f || fabsf(x - 1230.0f) < 1.0f) && y < 70.0f;
}

static bool hud_accept(int part, float z, float x, float y) {
    uint64_t now = plat_now_ns();
    for (int i = 0; i < HUD_SEEN; i++) {
        if (hud_seen[part][i].z != z || fabsf(hud_seen[part][i].x - x) > 2.0f || fabsf(hud_seen[part][i].y - y) > 2.0f) continue;
        if (!ui_menu_open || now - hud_seen[part][i].ns < HUD_SEEN_NS) {
            hud_seen[part][i].ns = now;
            return true;
        }
        break;
    }
    if (ui_menu_open) return false;
    int i = (int)(hud_nseen[part]++ % HUD_SEEN);
    hud_seen[part][i].z = z, hud_seen[part][i].x = x, hud_seen[part][i].y = y, hud_seen[part][i].ns = now;
    return true;
}
// ...and a part moved from the game's place whose frame (last frame's, on the canvas) lies on a menu window is not drawn
// while the menu is open: it doesn't jump, and never covers the menu. A window: the pieces of layer 97 / 98 near each
// other (their places, a margin more), this frame's and last frame's
#define HUD_WIN 16
typedef struct { float x0, y0, x1, y1; } HudBox;
static HudBox hud_win[2][HUD_WIN];  // ([0] this frame's, [1] last frame's)
static int hud_nwin[2];
static HudBox hud_part_box[2][HUD_SEEN_PARTS];  // (the moved parts' pieces on the canvas: this frame's, last frame's)
static uint64_t hud_frame, hud_hide_frame[HUD_SEEN_PARTS];
static bool hud_hide[HUD_SEEN_PARTS];
static void hud_new_frame(void) {
    if (g_present_count == hud_frame) return;
    hud_frame = g_present_count;
    memcpy(hud_win[1], hud_win[0], sizeof hud_win[0]), hud_nwin[1] = hud_nwin[0], hud_nwin[0] = 0;
    memcpy(hud_part_box[1], hud_part_box[0], sizeof hud_part_box[0]);
    for (int p = 0; p < HUD_SEEN_PARTS; p++) hud_part_box[0][p] = (HudBox){1e9f, 1e9f, -1e9f, -1e9f};
}
static void hud_window_piece(float x, float y) {
    const float NEAR = 48.0f;
    for (int i = 0; i < hud_nwin[0]; i++) {
        HudBox *b = &hud_win[0][i];
        if (x < b->x0 - NEAR || x > b->x1 + NEAR || y < b->y0 - NEAR || y > b->y1 + NEAR) continue;
        b->x0 = fminf(b->x0, x), b->y0 = fminf(b->y0, y), b->x1 = fmaxf(b->x1, x), b->y1 = fmaxf(b->y1, y);
        return;
    }
    if (hud_nwin[0] < HUD_WIN) hud_win[0][hud_nwin[0]++] = (HudBox){x, y, x, y};
}
static void hud_part_piece(int part, float x, float y) {  // (where a moved part's piece went, for next frame's test)
    HudBox *b = &hud_part_box[0][part];
    b->x0 = fminf(b->x0, x), b->y0 = fminf(b->y0, y), b->x1 = fmaxf(b->x1, x), b->y1 = fmaxf(b->y1, y);
}
static bool hud_on_window(int part, const float pad[4]) {  // (decided once a frame: all of the part or none of it)
    if (!ui_menu_open) return false;
    if (hud_hide_frame[part] == hud_frame) return hud_hide[part];
    hud_hide_frame[part] = hud_frame, hud_hide[part] = false;
    HudBox p = hud_part_box[1][part];
    if (p.x1 < p.x0) return false;
    p.x0 -= pad[0], p.y0 -= pad[1], p.x1 += pad[2], p.y1 += pad[3];
    const float M = 10.0f;  // (a window's edge pieces sit on its frame: the art reaches a little past them)
    for (int f = 0; f < 2; f++)
        for (int i = 0; i < hud_nwin[f]; i++) {
            const HudBox *w = &hud_win[f][i];
            if (p.x0 < w->x1 + M && p.x1 > w->x0 - M && p.y0 < w->y1 + M && p.y1 > w->y0 - M) hud_hide[part] = true;
        }
    if (hud_hide[part] && g_hudtrace_active) LOG("[hud] part %d on a menu window: not drawn while the menu is open\n", part);
    return hud_hide[part];
}

// 16:10: a window is drawn as pieces (body, edges, corners, then its text), each placed by its own centre, so one rule
// could send its top edge up and its bottom edge down (the chat window, the shoutout list, the Guild Card greeting).
// The pieces drawn one after another on one layer, near each other, are a group; when their anchors disagree the whole
// group takes one, from where it sits: by the nearer edge (top or bottom) or, with about as much room above as below,
// the centre. Their matrices are still in transient memory (not submitted before the pass ends), so the group is
// settled when it is complete: at the next piece that doesn't belong to it, a 3D draw (a model preview inside the
// window follows its anchor) or the end of the pass.
#define UI_GROUP_MAX 512
static struct {
    int n, predict;  // predict: the anchor this group got last frame (ui_group_known), -1 if not seen
    float z, x0, y0, xmin, xmax, ymin, ymax;
    uint8_t anchors;  // (bit per vertical anchor among its pieces)
    struct { float *m, pos_y; uint8_t vanchor; uint32_t recent; int piece; } p[UI_GROUP_MAX];
} ui_group;
static int ui_piece = -1;  // (a desktop's: the piece cbuf_info just told the mouse menus, runtime/hle/mousemenu.c)
// Each group's final anchor, by its first piece (layer and place): the HUD is drawn the same way every frame, so the
// scissors of a group's pieces, recorded before the group is complete, take the anchor it got last frame
#define UI_KNOWN 64
static struct { float z, x, y; int v; } ui_group_known[UI_KNOWN];
static uint32_t ui_nknown;
static int ui_group_lookup(float z, float x, float y) {
    for (int i = 0; i < UI_KNOWN; i++)
        if (ui_group_known[i].z == z && ui_group_known[i].x == x && ui_group_known[i].y == y) return ui_group_known[i].v;
    return -1;
}
static void ui_group_remember(int v) {
    for (int i = 0; i < UI_KNOWN; i++)
        if (ui_group_known[i].z == ui_group.z && ui_group_known[i].x == ui_group.x0 && ui_group_known[i].y == ui_group.y0) {
            ui_group_known[i].v = v;
            return;
        }
    int i = (int)(ui_nknown++ % UI_KNOWN);
    ui_group_known[i].z = ui_group.z, ui_group_known[i].x = ui_group.x0, ui_group_known[i].y = ui_group.y0, ui_group_known[i].v = v;
}
// The anchor the group's pieces so far get: their own when they agree, else one from where the group sits (-1: agree)
static int ui_group_vanchor(void) {
    bool agree = true;
    for (int i = 1; i < ui_group.n; i++) agree &= ui_group.p[i].vanchor == ui_group.p[0].vanchor;
    if (agree) return -1;
    float above = ui_group.ymin, below = 720.0f - ui_group.ymax;  // (piece centres: the edges and corners lie on the frame)
    return above < 0.5f * below ? 0 : below < 0.5f * above ? 2 : 1;
}
static void ui_group_end(void) {
    int n = ui_group.n;
    if (!n) return;
    int v = n < 2 ? -1 : ui_group_vanchor();
    ui_group_remember(v >= 0 ? v : ui_group.p[0].vanchor);
    ui_group.n = 0;
    if (v < 0) return;
    float sy = g_render_aspect / (16.0f / 9.0f);
    for (int i = 0; i < n; i++) {
        float y = ui_group.p[i].pos_y;
        ui_group.p[i].m[13] = v == 0 ? y * sy : v == 2 ? 720.0f - (720.0f - y) * sy : 360.0f + (y - 360.0f) * sy;
        if (ui_nrecent - ui_group.p[i].recent <= UI_RECENT) ui_recent[ui_group.p[i].recent % UI_RECENT].vanchor = (uint8_t)v;
#ifndef __ANDROID__
        mousemenu_piece_y(ui_group.p[i].piece, ui_group.p[i].m[13]);
#endif
    }
    if (g_hudtrace_active) LOG("[hud] group z %.1f: %d pieces, y %.1f..%.1f -> %c\n", ui_group.z, n, ui_group.ymin, ui_group.ymax, "TCB"[v]);
}
static void ui_group_add(float *m, float pos_z, float pos_x, float pos_y, int vanchor) {
    // Not near: more than 400 above or below (a frame drawn as its corners only, the help windows' 336 high, is one group;
    // the target cam at y 42..82 and the Hunter Arts prompts below y 600 are two), or a piece of another anchor beside the
    // group both sideways and vertically (the Guild Card's page counter, top right, next to its page; a window's own
    // pieces line up with the rest of it on one side: its other corner, its bottom edge)
    const float margin = 24.0f;
    bool apart = !(ui_group.anchors & (1u << vanchor)) && (pos_x < ui_group.xmin - margin || pos_x > ui_group.xmax + margin) &&
                 (pos_y < ui_group.ymin - margin || pos_y > ui_group.ymax + margin);
    if (ui_group.n && (fabsf(pos_z - ui_group.z) > 0.5f || pos_y < ui_group.ymin - 400.0f || pos_y > ui_group.ymax + 400.0f ||
                       apart || ui_group.n == UI_GROUP_MAX))
        ui_group_end();
    if (!ui_group.n) {
        ui_group.z = pos_z, ui_group.x0 = ui_group.xmin = ui_group.xmax = pos_x, ui_group.y0 = ui_group.ymin = ui_group.ymax = pos_y;
        ui_group.anchors = 0;
        ui_group.predict = ui_group_lookup(pos_z, pos_x, pos_y);
    }
    ui_group.xmin = fminf(ui_group.xmin, pos_x), ui_group.xmax = fmaxf(ui_group.xmax, pos_x);
    ui_group.ymin = fminf(ui_group.ymin, pos_y), ui_group.ymax = fmaxf(ui_group.ymax, pos_y);
    ui_group.anchors |= (uint8_t)(1u << vanchor);
    ui_group.p[ui_group.n].m = m, ui_group.p[ui_group.n].pos_y = pos_y, ui_group.p[ui_group.n].vanchor = (uint8_t)vanchor;
    ui_group.p[ui_group.n].recent = ui_nrecent - 1;  // (cbuf_info recorded it just before)
    ui_group.p[ui_group.n].piece = ui_piece;
    ui_group.n++;
}
static VkRenderPass pass_rp;
static uint32_t pass_noload;  // the attachments the pass began by clearing (render_pass, begin_pass)
static uint32_t pass_skip_clear;  // of them, those nothing has written yet: their leading clears are done (clear)
static uint64_t stat_noload;  // (MHGU_VK_STATS) passes begun so
// (MHGU_PERF_RODEPTH) the pass began with its depth / stencil target read-only (RP_RO_DEPTH); ro_want: the draw being set
// up samples that target where it is (resolve_feedback), so the pass it draws in must be such a pass
static bool pass_ro_depth, ro_want;
static bool noload_on(void) {  // MHGU_VK_NOLOAD=0: every pass reads its attachments in (as before)
    static int on = -1;
    if (on < 0) { const char *e = getenv("MHGU_VK_NOLOAD"); on = !e || atoi(e) != 0; }
    return on;
}
static bool preview_mapped(void);
static bool clears_drawn(void);
extern volatile int g_shadow_scissor_mode, g_clear_draw;
static float pass_sx = 1.0f, pass_sy = 1.0f;  // FSR: the pass renders into output-size stand-ins (fsr.c): viewport, scissor x

// Redundant-state filter: what draw() last set in the current command buffer (forgotten whenever something else may
// have recorded state: a new command buffer, a new pass, the clear pipeline).
enum { SS_PIPE = 1 << 0, SS_STEN = 1 << 1, SS_BC = 1 << 2, SS_DB = 1 << 3, SS_VP = 1 << 4, SS_SC = 1 << 5, SS_IB = 1 << 6,
       SS_SETS = 1 << 7 };
static struct {
    uint32_t known, vb_known;
    VkPipeline pipe;
    uint32_t sten[2][3];  // compare mask, write mask, reference per face
    float bc[4], db[3];
    VkViewport vp;
    VkRect2D sc;
    VkBuffer ib; VkDeviceSize io; VkIndexType it;
    VkBuffer vb[17]; VkDeviceSize vo[17];
    VkPipelineLayout sets_layout; VkDescriptorSet sets[2];  // (draw's two sets; any other bind forgets them: SS_SETS)
    uint32_t doff[34];  // (and their dynamic offsets: MHGU_PERF_DYNUBO; a layout fixes how many)
} ss;
static void ss_forget(void) { ss.known = 0; ss.vb_known = 0; }
static inline bool ss_need(uint32_t bit, bool same) {
    if ((ss.known & bit) && same) return false;
    ss.known |= bit;
    return true;
}

// ---- (debug) pass dumps: MHGU_CONTROL "dump" saves colour target 0 of every render pass of the next frame as
// shots/pass_NNN_<format>_<w>x<h>.png (RGBA8 / BGRA8 / RGBA16F / RGBA16 / B10G11R11; HDR values clamped to [0, 1] and also
// listed as the max in the log line).
volatile int g_dump_active;  // (device.c: set for one frame)
static char dump_draw_info[4096];  // the pass's last draw: program and fragment textures
static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }
int g_dump_no, g_dump_frame;  // (pass number, frame of a multi-frame dump)
static const char *dump_prefix(void) { static char p[32]; snprintf(p, sizeof p, g_dump_frame ? "shots/f%d_pass_" : "shots/pass_", g_dump_frame); return p; }
static uint64_t pass_draws_at;

static float half_to_float(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}
static float uf11(uint32_t v, int mbits) {  // unsigned small float: 5-bit exponent, mbits mantissa
    uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (e == 0) return ldexpf((float)m, -14 - mbits);
    if (e == 31) return m ? NAN : INFINITY;
    return ldexpf((float)(m | (1u << mbits)), (int)e - 15 - mbits);
}

// (the pass's own colour target 0: st.rt already names the next pass's targets when this one ends)
static NvnTexture *dump_tex, *dump_depth;
static uint32_t dump_lv, dump_ly;

static void dump_pass(void) {
    NvnTexture *t = dump_tex;
    uint64_t draws = stat_draws - pass_draws_at;
    if (t && t->backend) vk_ms_resolve(t->backend);  // (MSAA: resolved for the dump)
    if (dump_depth && dump_depth->backend) vk_ms_resolve(dump_depth->backend);
    if ((!t || !t->backend) && dump_depth && dump_depth->backend && ((TexBackend *)dump_depth->backend)->format == VK_FORMAT_D32_SFLOAT) {
        // depth-only pass (shadow maps): depth as grey, stretched over its own min..max
        TexBackend *b = dump_depth->backend;
        uint32_t w = b->width, h = b->height;
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);
        VkSpan out = vk_readback((uint64_t)w * h * 4);
        VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
        renderer_submit();
        vk_wait_seq(submit_seq);
        const float *d = out.ptr;
        float lo = INFINITY, hi = -INFINITY;
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) if (d[i] < 1.0f) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
        uint8_t *px = malloc((size_t)w * h * 4);
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) {
            float v = hi > lo ? (d[i] - lo) / (hi - lo) : 0;
            uint8_t g = d[i] >= 1.0f ? 255 : (uint8_t)(fminf(fmaxf(v, 0), 1) * 254.0f);
            px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = g; px[i * 4 + 3] = 255;
        }
        char path[128];
        mkdir("shots", 0755);
        snprintf(path, sizeof path, "%s%03d_depth_%ux%u.png", dump_prefix(), g_dump_no, w, h);
        vk_save_png(path, px, w, h);
        free(px);
        snprintf(path, sizeof path, "%s%03d_depth.raw", dump_prefix(), g_dump_no);  // (float32, w x h)
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(d, 4, (size_t)w * h, f); fclose(f); }
        LOG("[dump] pass %03d: depth only, %llu draws, range %g..%g | %s\n", g_dump_no++, (unsigned long long)draws, lo, hi, dump_draw_info);
        dump_draw_info[0] = 0;
        return;
    }
    if (!t || !t->backend) { LOG("[dump] pass %03d: no colour 0 (%llu draws)\n", g_dump_no++, (unsigned long long)draws); return; }
    TexBackend *b = t->backend;
    uint32_t lv = dump_lv, ly = dump_ly;
    uint32_t w = level_extent(b->width, lv), h = level_extent(b->height, lv), bpp;
    switch (b->format) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: bpp = 4; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: bpp = 8; break;
    default:
        LOG("[dump] pass %03d: format %d %ux%u (not dumped, %llu draws)\n", g_dump_no++, b->format, w, h, (unsigned long long)draws);
        return;
    }
    if (b->samples > 1) { LOG("[dump] pass %03d: multisampled (%llu draws)\n", g_dump_no++, (unsigned long long)draws); return; }
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * bpp);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_submit();
    vk_wait_seq(submit_seq);
    uint8_t *px = malloc((size_t)w * h * 4), *ax = malloc((size_t)w * h * 4);
    float maxv = 0;
    for (uint64_t i = 0; i < (uint64_t)w * h; i++) {
        float f[4] = {0, 0, 0, 1};
        const uint8_t *s = (const uint8_t *)out.ptr + i * bpp;
        if (b->format == VK_FORMAT_R16G16B16A16_SFLOAT) {
            const uint16_t *q = (const uint16_t *)s;
            for (int k = 0; k < 4; k++) f[k] = half_to_float(q[k]);
        } else if (b->format == VK_FORMAT_R16G16B16A16_UNORM) {
            const uint16_t *q = (const uint16_t *)s;
            for (int k = 0; k < 4; k++) f[k] = q[k] / 65535.0f;
        } else if (b->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
            uint32_t v;
            memcpy(&v, s, 4);
            f[0] = uf11(v & 0x7FF, 6); f[1] = uf11((v >> 11) & 0x7FF, 6); f[2] = uf11(v >> 22, 5);
        } else {
            bool bgr = b->format == VK_FORMAT_B8G8R8A8_UNORM || b->format == VK_FORMAT_B8G8R8A8_SRGB;
            for (int k = 0; k < 4; k++) f[k] = s[bgr && k < 3 ? 2 - k : k] / 255.0f;
        }
        for (int k = 0; k < 3; k++) {
            if (f[k] != f[k]) f[k] = 1e9f;  // NaN: shown white, counted as huge
            if (f[k] > maxv) maxv = f[k];
            px[i * 4 + k] = (uint8_t)(fminf(fmaxf(f[k], 0), 1) * 255.0f + 0.5f);
        }
        px[i * 4 + 3] = 255;
        uint8_t a = (uint8_t)(fminf(fmaxf(f[3], 0), 1) * 255.0f + 0.5f);
        ax[i * 4] = ax[i * 4 + 1] = ax[i * 4 + 2] = a;
        ax[i * 4 + 3] = 255;
    }
    char path[128];
    mkdir("shots", 0755);
    snprintf(path, sizeof path, "%s%03d_f%d_%ux%u.png", dump_prefix(), g_dump_no, b->format, w, h);
    vk_save_png(path, px, w, h);
    snprintf(path, sizeof path, "%s%03d_alpha.png", dump_prefix(), g_dump_no);
    vk_save_png(path, ax, w, h);
    free(ax);
    LOG("[dump] pass %03d: %llu draws, nvn fmt %#x, max value %g | %s\n", g_dump_no, (unsigned long long)draws, t->format, maxv, dump_draw_info);
    dump_draw_info[0] = 0;
    g_dump_no++;
    free(px);
}

// ================================================================== (debug) GPU profile: MHGU_CONTROL "gpuprof"
// GPU timestamps between the passes (and MSAA resolves) of the next frame, logged in milliseconds by vk_gpuprof_frame.
volatile int g_gpuprof_request;
static int gp_active, gp_n, gp_pass;
static VkQueryPool gp_pool;
static char gp_label[256][64];

static void gp_mark(const char *label) {
    if (!gp_active || gp_n >= 256) return;
    vkCmdWriteTimestamp(vk_cmd(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gp_pool, (uint32_t)gp_n);
    snprintf(gp_label[gp_n++], sizeof gp_label[0], "%s", label);
}

void vk_gpuprof_mark(const char *label) { gp_mark(label); }  // (device.c: the window image's pass)

void vk_gpuprof_frame(void) {  // (device.c, after each presented frame's submission)
    if (gp_active) {
        gp_active = 0;
        uint64_t t[256];
        if (gp_n > 1 && vkGetQueryPoolResults(g_vk.dev, gp_pool, 0, (uint32_t)gp_n, sizeof t, t, 8,
                                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
            double per = g_vk.props.limits.timestampPeriod / 1e6;
            bool ok = t[gp_n - 1] > t[0];
            for (int i = 1; i < gp_n; i++) ok = ok && t[i] >= t[i - 1];
            if (!ok) LOG("[gpuprof] no usable timestamps this frame: ask again\n");
            for (int i = 1; ok && i < gp_n; i++) LOG("[gpuprof] %7.3f ms  %s\n", (t[i] - t[i - 1]) * per, gp_label[i]);
            if (ok) LOG("[gpuprof] %7.3f ms  frame\n", (t[gp_n - 1] - t[0]) * per);
        }
    }
    if (g_gpuprof_request) {
        g_gpuprof_request = 0;
        if (!gp_pool) {
            VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                        .queryCount = 256};
            VK_CHECK(vkCreateQueryPool(g_vk.dev, &qi, NULL, &gp_pool));
        }
        vkCmdResetQueryPool(vk_cmd(), gp_pool, 0, 256);
        gp_active = 1, gp_n = 0, gp_pass = 0;
        gp_mark("start");
    }
}

static bool pass_msaa;  // (MSAA, below) the pass renders through multisampled images (resolved when next read)
static bool ms_ever;     // (MHGU_PERF_TEXSCAN) some pass has: until then no texture has a resolve owed (ms_dirty)
static TexBackend *pass_att[9];
static uint32_t pass_natt;
// (MSAA) colour target 0 is a picture 3D models are drawn into (ms_expand). Its picture is copied into the samples
// only for what tests depth (the models, and the depth clears before them): the menu's 2D draws before the first model
// stay without MSAA; the ones after it go into the samples too, shaded per sample (else the seams between their quads
// show), so the frame doesn't switch between the two. draw / clear set ms_want_depth before a pass begins;
// pass_want_depth: what the pass began for
static bool pass_models, pass_want_depth, ms_want_depth;

void vk_end_pass(void) {
    if (!in_pass) return;
    ui_group_end();
    if (ui_nrecent) ui_results_last = ui_results_pass;
    vkCmdEndRenderPass(vk_cmd());
    in_pass = false;
    if (gp_active) {
        char l[64];
        snprintf(l, sizeof l, "pass %d %ux%u%s, %llu draws%s%s", gp_pass++, pass_w, pass_h, pass_msaa ? " MSAA" : "",
                 (unsigned long long)(stat_draws - pass_draws_at), pass_noload ? ", cleared as it began" : "",
                 pass_ro_depth ? ", depth read-only" : "");
        gp_mark(l);
    }
    if (pass_msaa) for (uint32_t k = 0; k < pass_natt; k++) pass_att[k]->ms_dirty = true, ms_ever = true;
    pass_msaa = false;
    if (g_dump_active) dump_pass();
}


// ================================================================== MSAA
// With MSAA on (settings), the 3D scene's passes (those whose depth target has the 3D resolution) render into
// multisampled companions of their targets (TexBackend.ms). A companion is used only while it holds its target's
// contents (ms_valid): written last by such a pass, or cleared whole at the start of this one. Any other write to the
// target (a pass without MSAA, a copy) makes it stale, and a pass with a stale companion that isn't cleared first
// renders without MSAA. The target itself is brought up to date (resolved: vk_ms_resolve) only when something reads
// it - a draw or dispatch sampling it, a copy, a pass without MSAA, presentation - so the scene's passes in a row
// share one resolve: on the R9 270X a resolve is the expensive part of MSAA. (Menus: 3D models drawn over the finished
// picture, ms_expand.)
static uint64_t cur_rec;  // guest address of the command being executed (exec_list)
static int rt_kind(uint32_t f);

// Attachments of the pass about to begin that the commands from cur_rec on clear whole before anything else (bit i:
// colour target i, bit 8: depth, and stencil too where there is stencil).
static uint32_t leading_clears(uint32_t w, uint32_t h, bool stencil) {
    if (!cur_rec) return 0;
    int32_t sc[4];
    memcpy(sc, st.scissor[0], sizeof sc);
    uint32_t got = 0;
    uint64_t p = cur_rec;
    for (int n = 0; n < 64; n++) {
        const CmdRec *r = (const CmdRec *)G2H((uint32_t)p);
        bool full = sc[0] <= 0 && sc[1] <= 0 && (int64_t)sc[0] + sc[2] >= w && (int64_t)sc[1] + sc[3] >= h;
        switch (r->op) {
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_SET_SCISSORS: if (r->x[0] == 0 && r->x[1]) memcpy(sc, r->extra, sizeof sc); break;
        case CMD_CLEAR_COLOR: if (full && (r->x[2] & 0xF) == 0xF && r->x[0] < 8) got |= 1u << r->x[0]; break;
        case CMD_CLEAR_DEPTH_STENCIL: if (full && r->x[0] && (!stencil || (r->x[2] & 0xFF) == 0xFF)) got |= 1u << 8; break;
        case CMD_UPDATE_UNIFORM_BUFFER: case CMD_REPORT_COUNTER: break;
        default:
            if (r->op >= CMD_BIND_PROGRAM && r->op <= CMD_SET_DEPTH_CLAMP && r->op != CMD_SET_RENDER_TARGETS) break;
            return got;  // (a draw, a copy, other targets...)
        }
        p += r->size;
    }
    return got;
}

// vk_ms_resolve: a target brought up to date from its multisampled image. Colour: vkCmdResolveImage (the samples
// averaged). Depth / stencil: sample 0, by a shader from a sampleable copy (the image itself must stay unsampleable to
// keep its compression; Vulkan 1.2 render pass depth resolves were no faster). Measured on the R9 270X, 1080p, 4x:
// ~1.6 ms per colour target, ~9 ms for the depth / stencil target (mostly the driver decompressing it), so the scene's
// passes share one resolve per target (see above).
static VkDescriptorSetLayout rs_set_layout;
static VkPipelineLayout rs_layout;

// kind 1 depth, 2 depth + stencil; into a multisampled target of `samples` (ms_expand): 3 colour, 4 depth, 5 depth +
// stencil; 6 FXAA over menus' 3D models, colour without alpha (models_fxaa_end)
static VkPipeline resolve_pipeline(VkRenderPass rp, int kind, uint32_t samples) {
    static const char *frag[] = {NULL, "resolve.frag.depth", "resolve.frag.depth.s", "expand.frag", "expand.frag.depth",
                                 "expand.frag.depth.s", "fxaa.frag.models"};
    static struct { VkRenderPass rp; int kind; VkPipeline p; } tab[32];
    static int n;
    for (int i = 0; i < n; i++) if (tab[i].rp == rp && tab[i].kind == kind) return tab[i].p;
    if (!rs_layout) {
        VkDescriptorSetLayoutBinding b[2] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2,
                                              .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &rs_set_layout));
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &rs_set_layout};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &rs_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("resolve.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(frag[kind]),
         .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
                                            .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)samples};
    VkStencilOpState so = {VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xFF, 0xFF, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = kind != 3 && kind != 6, .depthWriteEnable = kind != 3 && kind != 6,
                                                .depthCompareOp = VK_COMPARE_OP_ALWAYS,
                                                .stencilTestEnable = kind == 2 || kind == 4 || kind == 5, .front = so, .back = so};
    VkPipelineColorBlendAttachmentState all = {.colorWriteMask = kind == 6 ? 0x7 : 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = kind == 3 || kind == 6, .pAttachments = &all};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
                                           .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vs, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                       .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
                                       .layout = rs_layout, .renderPass = rp};
    VkPipeline p = VK_NULL_HANDLE;
    if (sync_pipeline(&pi, &p) != VK_SUCCESS) LOG("[vk] resolve pipeline failed\n");
    if (n < 32) tab[n].rp = rp, tab[n].kind = kind, tab[n++].p = p;
    return p;
}

void vk_ms_resolve(TexBackend *b) {
    if (!b || !b->ms_dirty) return;
    b->ms_dirty = false;
    TexBackend *m = b->ms;
    if (!m) return;
    if (in_pass) { vk_end_pass(); st.rt_dirty = true; }
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    bool depth = b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkImageLayout att = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_ALL_WRITES,
                               .dstAccessMask = VK_ACCESS_ALL, .oldLayout = att, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = m->image, .subresourceRange = {m->aspect, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_STAGES_ALL, VK_STAGES_ALL, 0, 0, NULL, 0, NULL, 1, &ib);
    TexBackend *r = NULL;
    if (!depth) {  // colour: the GPU's own resolve (averages the samples)
        VkImageResolve rv = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                             .extent = {b->width, b->height, 1}};
        vkCmdResolveImage(cb, m->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
    } else if (depth_copy_ok(m->format) && (r = vk_tex_ms_readable(m))) {  // depth / stencil: copied where a shader can read it (below)
        depth_copy_note("resolve copy", (int)b->width, (int)b->height, (int)b->width, (int)b->height, m->format);
        VkImageCopy c = {.srcSubresource = {m->aspect, 0, 0, 1}, .dstSubresource = {m->aspect, 0, 0, 1},
                         .extent = {b->width, b->height, 1}};
        vkCmdCopyImage(cb, m->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ib.newLayout = att;
    ib.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_STAGES_ALL, VK_STAGES_ALL, 0, 0, NULL, 0, NULL, 1, &ib);
    if (r) {  // depth (and stencil) of sample 0 written by a shader
        vk_full_barrier(cb);
        static const VkComponentMapping id = {0};
        VkFormat f = b->ms_fmt, none = VK_FORMAT_UNDEFINED;
        bool sten = (r->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && g_vk.stencil_export;
        VkRenderPass rp = vk_render_pass(&none, 0, f, 1);
        VkPipeline p = resolve_pipeline(rp, sten ? 2 : 1, 1);
        VkImageView dst = vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, b->aspect);
        if (!p || !dst) return;
        VkDescriptorSet set = vk_alloc_set(rs_set_layout);
        VkDescriptorImageInfo ii[2] = {
            {vk_default_sampler(false),
             vk_image_view(r, VK_IMAGE_VIEW_TYPE_2D, r->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_DEPTH_BIT), VK_IMAGE_LAYOUT_GENERAL},
            {vk_default_sampler(false),
             sten ? vk_image_view(r, VK_IMAGE_VIEW_TYPE_2D, r->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_STENCIL_BIT) : VK_NULL_HANDLE,
             VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0]},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[1]}};
        vkUpdateDescriptorSets(g_vk.dev, sten ? 2 : 1, w, 0, NULL);
        VkRect2D area = {{0, 0}, {b->width, b->height}};
        VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                    .framebuffer = vk_framebuffer(rp, &dst, 1, b->width, b->height), .renderArea = area};
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, rs_layout, 0, 1, &set, 0, NULL);
        ss.known &= ~SS_SETS;  // (not draw's sets any more)
        VkViewport vp = {0, 0, (float)b->width, (float)b->height, 0, 1};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &area);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        ss_forget();
    }
    gp_mark("  resolve");
}

// A pass that draws 3D models over a picture it did not clear: menus draw the hunter and Palicoes (Character Select,
// Guild Card) straight into the finished picture, after the game's FXAA and outside the 3D scene's passes, so they
// had no anti-aliasing. Such a target (TexBackend.ms_models: depth-writing draws went into it lately, the first ones
// without MSAA) gets its picture copied into every sample of its multisampled image m as the frame's first pass of its
// depth-testing draws begins (pass_models), which then renders with MSAA like the scene's passes. Its depth / stencil
// target too, unless the pass clears it whole first (the smithy's preview draws over depth it doesn't clear). f: the
// format the pass renders a colour target as. false: not possible (the pass goes without).
static bool ms_expand(VkCommandBuffer cb, TexBackend *b, TexBackend *m, VkFormat f) {
    static const VkComponentMapping id = {0};
    bool depth = b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    // (without stencil export, as vk_ms_resolve: depth only, the stencil set to 0)
    bool sten = depth && (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && g_vk.stencil_export;
    VkFormat none = VK_FORMAT_UNDEFINED;
    VkRenderPass rp = depth ? vk_render_pass(&none, 0, m->format, m->samples) : vk_render_pass(&f, 1, none, m->samples);
    VkPipeline p = resolve_pipeline(rp, !depth ? 3 : sten ? 5 : 4, m->samples);
    VkImageView src = vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, depth ? b->format : f, 0, 1, 0, 1, id,
                                    depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView src_s = sten ? vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, b->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_STENCIL_BIT)
                             : VK_NULL_HANDLE;
    VkImageView dst = vk_image_view(m, VK_IMAGE_VIEW_TYPE_2D, depth ? m->format : f, 0, 1, 0, 1, id,
                                    depth ? m->aspect : VK_IMAGE_ASPECT_COLOR_BIT);
    if (!p || !src || !dst || (sten && !src_s)) return false;  // (kind 4: the stencil test writes reference 0)
    VkDescriptorSet set = vk_alloc_set(rs_set_layout);
    VkDescriptorImageInfo ii[2] = {{vk_default_sampler(false), src, VK_IMAGE_LAYOUT_GENERAL},
                                   {vk_default_sampler(false), src_s, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[1]}};
    vkUpdateDescriptorSets(g_vk.dev, sten ? 2 : 1, w, 0, NULL);
    VkRect2D area = {{0, 0}, {b->width, b->height}};
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                .framebuffer = vk_framebuffer(rp, &dst, 1, b->width, b->height), .renderArea = area};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, rs_layout, 0, 1, &set, 0, NULL);
    ss.known &= ~SS_SETS;  // (not draw's sets any more)
    VkViewport vp = {0, 0, (float)b->width, (float)b->height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &area);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    vk_full_barrier(cb);
    ss_forget();
    gp_mark(depth ? "  MSAA: depth into the samples (3D models)" : "  MSAA: the picture into the samples (3D models over it)");
    static bool logged;
    if (!logged) logged = true, LOG("[vk] MSAA %ux for 3D models drawn over a finished %ux%u picture (menus)\n", m->samples,
                                    b->width, b->height);
    return true;
}
// The anti-aliasing such models get: the player's (models_aa), though every other anti-aliasing has run before they
// are drawn. MSAA on: they are drawn through its samples (ms_expand; the 2D draws after them need per-sample shading,
// sampleRateShading). FXAA on: FXAA 3.11 over them (models_fxaa_end). FSR never sees them (it runs at its start of
// the HUD, before them): none, as with anti-aliasing off. full_picture: a picture they are drawn over: the render
// size, once the game's FXAA pass drew this frame (before it, the 3D scene's passes, which keep their own rules).
enum { MODELS_AA_NONE, MODELS_AA_MSAA, MODELS_AA_FXAA };
static uint64_t aa_pass_frame;  // the frame the game's FXAA pass last drew in (fxaa_skipped)
static int models_aa(void) {
    if (g_fsr_on) return MODELS_AA_NONE;
    if (g_vk.msaa > 1) return g_vk.sample_shading ? MODELS_AA_MSAA : MODELS_AA_NONE;
    return config_live_fxaa() ? MODELS_AA_FXAA : MODELS_AA_NONE;
}
static bool full_picture(uint32_t w, uint32_t h) {
    return models_aa() != MODELS_AA_NONE && aa_pass_frame == g_present_count && w == g_render_size[0] &&
           h == g_render_size[1];
}

// FXAA over such models (MODELS_AA_FXAA): the picture is copied before each run of their depth-testing draws (and
// depth clears) and again after it, then FXAA 3.11 is drawn into it from the second copy, only where the models
// changed it and next to that ("fxaa.frag.models"), so the 2D draws around them stay sharp.
static void snapshots_invalidate(NvnTexture *t);
static NvnTexture *mf_tex;        // the picture of the run of models open (NULL none)
static uint64_t mf_frame;         // the frame it opened in
static TexBackend mf_before, mf_after;
static bool picture_copy(TexBackend *b, TexBackend *d) {  // d: a copy of b (made as b's size and format)
    VkCommandBuffer cb = vk_cmd();
    if (d->image && (d->width != b->width || d->height != b->height || d->format != b->format)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                                .extent = {b->width, b->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        if (vk_create_image(&ci, &d->image, &d->mem) != VK_SUCCESS) return d->image = VK_NULL_HANDLE, false;
        d->format = b->format; d->width = b->width; d->height = b->height; d->depth = d->layers = d->levels = d->samples = 1;
        d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(cb, d->image, d->aspect);
    }
    vk_full_barrier(cb);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {b->width, b->height, 1}};
    vkCmdCopyImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    vk_full_barrier(cb);
    return true;
}
static void models_fxaa_begin(NvnTexture *t) {
    vk_end_pass();
    st.rt_dirty = true;
    if (picture_copy(t->backend, &mf_before)) mf_tex = t, mf_frame = g_present_count;
}
static void models_fxaa_end(void) {
    NvnTexture *t = mf_tex;
    TexBackend *b = t->backend;
    mf_tex = NULL;
    vk_end_pass();
    st.rt_dirty = true;
    if (!picture_copy(b, &mf_after)) return;
    static const VkComponentMapping id = {0};
    VkFormat f = b->format;
    VkRenderPass rp = vk_render_pass(&f, 1, VK_FORMAT_UNDEFINED, 1);
    VkPipeline p = resolve_pipeline(rp, 6, 1);
    VkImageView dst = vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView after = vk_image_view(&mf_after, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView before = vk_image_view(&mf_before, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!p || !dst || !after || !before) return;
    VkCommandBuffer cb = vk_cmd();
    VkDescriptorSet set = vk_alloc_set(rs_set_layout);
    VkDescriptorImageInfo ii[2] = {{vk_default_sampler(true), after, VK_IMAGE_LAYOUT_GENERAL},
                                   {vk_default_sampler(false), before, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[1]}};
    vkUpdateDescriptorSets(g_vk.dev, 2, w, 0, NULL);
    VkRect2D area = {{0, 0}, {b->width, b->height}};
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                .framebuffer = vk_framebuffer(rp, &dst, 1, b->width, b->height), .renderArea = area};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, rs_layout, 0, 1, &set, 0, NULL);
    ss.known &= ~SS_SETS;  // (not draw's sets any more)
    VkViewport vp = {0, 0, (float)b->width, (float)b->height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &area);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    vk_full_barrier(cb);
    ss_forget();
    snapshots_invalidate(t);
    gp_mark("  FXAA over 3D models (menus)");
    static bool logged;
    if (!logged) logged = true, LOG("[vk] FXAA for 3D models drawn over a finished %ux%u picture (menus)\n", b->width, b->height);
}
// Before a draw or clear: models (it tests or clears depth) into a picture they are drawn over open a run, anything
// else ends it. A run still open at the frame's end is dropped (the picture is presented without it).
static void models_fxaa_track(bool models) {
    if (models_aa() != MODELS_AA_FXAA) { mf_tex = NULL; return; }
    if (mf_tex && mf_frame != g_present_count) mf_tex = NULL;
    NvnTexture *t = st.rt.ncolor ? st.rt.color[0] : NULL;
    TexBackend *b = t ? t->backend : NULL;
    models = models && b && b->ms_models && b->ms_models + 2 >= g_present_count && full_picture(b->width, b->height);
    if (mf_tex && !(models && t == mf_tex)) models_fxaa_end();
    if (!mf_tex && models) models_fxaa_begin(t);
}

// (MHGU_PERF_TEXSCAN) the draw being set up's textures, its vertex and fragment programs', looked up once (draw_textures):
// nothing between draw's start and its end changes what is bound
static NvnTexture *shader_texture_object(int stage, ShaderTexture *t);
static NvnTexture *dtex[2][64];
static ShaderInfo *dtex_si[2];
static void draw_textures(ShaderInfo *vi, ShaderInfo *fi) {
    ShaderInfo *s[2] = {vi, fi};
    for (int sg = 0; sg < 2; sg++) {
        dtex_si[sg] = s[sg];
        for (int i = 0; i < s[sg]->ntex && i < 64; i++) dtex[sg][i] = shader_texture_object(sg, &s[sg]->tex[i]);
    }
}
static inline NvnTexture *draw_texture(int stage, ShaderInfo *si, int i) {
    if (!(stage < 2 && dtex_si[stage] == si && i < 64)) return shader_texture_object(stage, &si->tex[i]);
    if (perf_check && (stat_check_n++, dtex[stage][i] != shader_texture_object(stage, &si->tex[i]))) check_bad("a draw's texture");
    return dtex[stage][i];
}

static uint32_t perf_noload(uint32_t w, uint32_t h, TexBackend *const *att, const int *att_index, uint32_t n, VkClearValue cv[9]);
static TexBackend *depth_clear_pending;
static void depth_clear_now(void);
static void at_written(void);
// (MHGU_PERF_HUDSTART_FUSE & 2, MHGU_PERF_FXAA_COPYDRAW) dst to be filled from src - kind 0 scaled (H' from H), 1 copied (the
// FXAA pass's target from its input) - not done yet
static struct { TexBackend *dst, *src; int kind; } fill;
static bool fill_used;
static int fill_index = -1;                    // the colour target the pass beginning draws it into (begin_pass)
static void fill_now(void);
static void fill_copy_flush(void);
bool vk_color_fill_later(TexBackend *dst, TexBackend *src, int kind);
void vk_color_fill_check(TexBackend *dst, TexBackend *src, int kind);
static void fill_draw(VkCommandBuffer cb, TexBackend *src, VkRenderPass rp, uint32_t index, uint32_t ncolor, VkFormat depth,
                      uint32_t samples, uint32_t w, uint32_t h, int kind);

// Before a draw / dispatch: the textures it samples up to date (attachments of the pass: resolve_feedback).
static void fill_sampled_stage(int stage, ShaderInfo *si) {
    for (int i = 0; i < si->ntex && fill.dst; i++) {
        NvnTexture *t = shader_texture_object(stage, &si->tex[i]);
        if (t && g_fsr_on) t = vk_fsr_map(t, false);
        if (t && t->backend == fill.dst) fill_now();
    }
}
static void ms_resolve_sampled(int stage, ShaderInfo *si) {
    if (perf.texscan && !ms_ever) return;
    for (int i = 0; i < si->ntex; i++) {
        NvnTexture *t = draw_texture(stage, si, i);
        if (t && t->backend && ((TexBackend *)t->backend)->ms_dirty) vk_ms_resolve(t->backend);
    }
}

// The attachments the commands from cur_rec on clear whole before anything else, as leading_clears, but stricter: only
// clears, scissors and jumps on the way, so the state clear() will see (viewport, depth state: the preview mapping) is
// the state begin_pass sees now.
// cv[bit]: the value each is cleared to last (the colour's 16 bytes as the clear has them: float, int or uint).
static uint32_t noload_clears(uint32_t w, uint32_t h, bool stencil, VkClearValue cv[9]) {
    if (!cur_rec) return 0;
    int32_t sc[4];
    memcpy(sc, st.scissor[0], sizeof sc);
    uint32_t got = 0;
    uint64_t p = cur_rec;
    for (int n = 0; n < 16; n++) {
        const CmdRec *r = (const CmdRec *)G2H((uint32_t)p);
        bool full = sc[0] <= 0 && sc[1] <= 0 && (int64_t)sc[0] + sc[2] >= w && (int64_t)sc[1] + sc[3] >= h;
        switch (r->op) {
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_SET_SCISSORS: if (r->x[0] == 0 && r->x[1]) memcpy(sc, r->extra, sizeof sc); break;
        case CMD_CLEAR_COLOR:
            if (full && (r->x[2] & 0xF) == 0xF && r->x[0] < 8) got |= 1u << r->x[0], memcpy(&cv[r->x[0]].color, r->extra, 16);
            break;
        case CMD_CLEAR_DEPTH_STENCIL:
            if (full && r->x[0] && (!stencil || (r->x[2] & 0xFF) == 0xFF))
                got |= 1u << 8, cv[8].depthStencil = (VkClearDepthStencilValue){r->f[0], (uint32_t)r->x[1]};
            break;
        default: return got;
        }
        p += r->size;
    }
    return got;
}

static void begin_pass(void) {
    if (in_pass && !st.rt_dirty) return;
    vk_end_pass();
    st.rt_dirty = false;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // passes, copies and dispatches see each other's results
    VkImageView views[9];
    TexBackend *att[9];  // per view: its image, the format it's rendered as, colour index (-1 depth)
    VkFormat att_fmt[9];
    int att_index[9];
    uint32_t msaa = g_vk.msaa;
    bool ms_ok = msaa > 1;  // (every attachment one whole level 0 / layer 0, not an integer format)
    uint32_t nviews = 0, minw = UINT32_MAX, minh = UINT32_MAX;
    pass_ncolor = (uint32_t)st.rt.ncolor;
    pass_samples = 1;
    pass_sx = pass_sy = 1.0f;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        pass_color[i] = VK_FORMAT_UNDEFINED;
        if (!t) continue;
        vk_rt_widen(t, vk_wide_rt_on() && !vk_fsr_input(t));  // (the game's own target: FSR 3 / 4 take no RGBA16 UNORM)
        if (g_fsr_on) {  // FSR: from the start of the HUD on, an output-size stand-in (fsr.c)
            NvnTexture *s = vk_fsr_map(t, false);
            if (s != t) pass_sx = (float)s->width / (float)t->width, pass_sy = (float)s->height / (float)t->height, t = s;
        }
        renderer_mark_gpu_owned(t);
        VkImageView v = vk_view(t, &st.rt.cview[i], st.rt.has_cview[i], true, TEXTYPE_2D);
        TexBackend *b = t->backend;
        if (!v) continue;
        pass_color[i] = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4) && vk_format(t->format) == b->format
                      ? vk_format(st.rt.cview[i].format) : b->format;
        att[nviews] = b, att_fmt[nviews] = pass_color[i], att_index[nviews] = i;
        views[nviews++] = v;
        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        uint32_t ly = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 2) ? st.rt.cview[i].min_layer : 0;
        ms_ok = ms_ok && !lv && !ly && b->samples == 1 && !rt_kind(t->format);
        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
        if (ew < minw) minw = ew;
        if (eh < minh) minh = eh;
        pass_samples = b->samples;
    }
    pass_depth = VK_FORMAT_UNDEFINED;
    NvnTexture *dt = st.rt.depth && pass_sx != 1.0f ? vk_fsr_map(st.rt.depth, true) : st.rt.depth;  // (FSR: as above)
    if (dt) {
        renderer_mark_gpu_owned(dt);
        VkImageView v = vk_view(dt, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);
        TexBackend *b = dt->backend;
        if (v && renderer_format_is_depth(dt->format) && b->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
            pass_depth = b->format;
            att[nviews] = b, att_fmt[nviews] = b->format, att_index[nviews] = -1;
            views[nviews++] = v;
            uint32_t lv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
            uint32_t ly = st.rt.has_dview && (st.rt.dview.setmask & 2) ? st.rt.dview.min_layer : 0;
            ms_ok = ms_ok && !lv && !ly && b->samples == 1;
            uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
            if (ew < minw) minw = ew;
            if (eh < minh) minh = eh;
            if (!pass_ncolor) pass_samples = b->samples;
        }
    }
    if (minw == UINT32_MAX) minw = minh = 16;
    pass_w = minw; pass_h = minh;
    if (g_hudtrace_active) {  // (debug, "hudtrace") each pass's targets, the stand-ins and the area rendered
        char tg[400];
        int n = 0;
        for (int i = 0; i < st.rt.ncolor && n < 300; i++)
            if (st.rt.color[i]) n += snprintf(tg + n, sizeof tg - n, " c%d %ux%u", i, st.rt.color[i]->width, st.rt.color[i]->height);
        if (st.rt.depth) n += snprintf(tg + n, sizeof tg - n, " d %ux%u%s", st.rt.depth->width, st.rt.depth->height,
                                       dt != st.rt.depth ? " (stand-in)" : "");
        LOG("[hud] pass%s -> area %ux%u, sx %.3f sy %.3f\n", tg, minw, minh, pass_sx, pass_sy);
    }
    ui_loading_pass = false;
    ui_title_pass = false;
    ui_frontend_pass = false;
    ui_stamp_pass = false;
    ui_results_pass = false;
    ui_meownster_pass = false;
    ui_menu_pass = false;
    ui_window_pass = false;
    ui_menu_open = touch_game_menu();
    ui_nrecent = 0;
    pass_msaa = false;
    // (MSAA: a colour target 3D models were drawn into lately (ms_expand), while they are; once they are not, it is like
    // any other target again)
    pass_models = false;
    pass_want_depth = ms_want_depth;
    for (uint32_t k = 0; k < nviews; k++) {
        TexBackend *b = att[k];
        if (att_index[k] < 0 || !b->ms_models) continue;
        if (b->ms_models + 2 >= g_present_count) {
            pass_models |= att_index[k] == 0 && models_aa() == MODELS_AA_MSAA && full_picture(minw, minh);
            continue;
        }
        b->ms_models = 0;
        vk_ms_resolve(b);
        b->ms_valid = false;
    }
    if (ms_ok && pass_depth != VK_FORMAT_UNDEFINED && minw == g_render_size[0] && minh == g_render_size[1]) {
        bool stencil = pass_depth == VK_FORMAT_D24_UNORM_S8_UINT || pass_depth == VK_FORMAT_D32_SFLOAT_S8_UINT;
        uint32_t cleared = leading_clears(minw, minh, stencil);
        bool ok = true, expand[9] = {false};
        for (uint32_t k = 0; k < nviews && ok; k++) {
            TexBackend *b = att[k];
            bool clr = (cleared >> (att_index[k] < 0 ? 8 : att_index[k])) & 1;
            bool valid = b->ms && b->ms_valid && b->ms->samples == msaa;
            // (its picture, and the depth the models test against, into the samples first, as the models begin)
            expand[k] = pass_models && pass_want_depth && !clr && !valid && (att_index[k] < 0 || b->ms_models);
            ok = b->width == minw && b->height == minh && (clr || valid || expand[k]);
        }
        VkImageView msv[9];
        VkFormat ms_depth_fmt = pass_depth;
        for (uint32_t k = 0; k < nviews && ok; k++) {
            TexBackend *m = vk_tex_ms(att[k], msaa);
            ok = m && (msv[k] = vk_image_view(m, VK_IMAGE_VIEW_TYPE_2D, att_index[k] < 0 ? m->format : att_fmt[k], 0, 1, 0, 1,
                                              (VkComponentMapping){0}, m->aspect));
            if (ok && att_index[k] < 0) ms_depth_fmt = m->format;
        }
        for (uint32_t k = 0; k < nviews && ok; k++)
            if (expand[k]) ok = ms_expand(cb, att[k], att[k]->ms, att_fmt[k]);
        if (ok) {  // rendered into the multisampled images (resolved at the end: vk_end_pass)
            memcpy(views, msv, nviews * sizeof *views);
            memcpy(pass_att, att, nviews * sizeof *att);
            pass_natt = nviews;
            for (uint32_t k = 0; k < nviews; k++) att[k]->ms_fmt = att_fmt[k];
            pass_samples = msaa;
            pass_depth = ms_depth_fmt;
            pass_msaa = true;
        }
    }
    for (uint32_t k = 0; k < nviews; k++) {
        // (pass_models, its 2D draws before the models: they don't use the depth / stencil target, left as it is (a
        // resolve of it is slow) and its multisampled image valid)
        if (!pass_msaa && pass_models && !pass_want_depth && att_index[k] < 0) continue;
        if (!pass_msaa) vk_ms_resolve(att[k]);  // (rendered into directly: up to date first)
        att[k]->ms_valid = pass_msaa;
    }
    pass_rp = vk_render_pass(pass_color, pass_ncolor, pass_depth, pass_samples);
    if (!nviews) {  // nothing to render into: a pass without attachments still needs a framebuffer size
        static VkImageView none;
        (void)none;
    }
    VkFramebuffer fb = vk_framebuffer(pass_rp, views, nviews, minw, minh);
    // Attachments the pass's first commands clear whole are not read in (render_pass): the same pixels, without a
    // tile-based GPU reading the old ones from memory first. Only where clear() will clear the whole pass: nothing
    // moves its rectangle (a HUD window's preview mapping, the debug shadow scissor modes).
    // (the clears themselves still run after it, to the same values)
    pass_noload = 0;
    VkClearValue cv[9] = {0}, cvals[9] = {0};
    uint32_t ncv = 0;
    if (noload_on() && !g_shadow_scissor_mode && !preview_mapped() && !clears_drawn() && g_clear_draw <= 0) {  // (drawn clears: a pipeline that may fail)
        bool stencil = pass_depth == VK_FORMAT_D24_UNORM_S8_UINT || pass_depth == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                       pass_depth == VK_FORMAT_S8_UINT;
        uint32_t cleared = noload_clears(minw, minh, stencil, cv);
        for (uint32_t k = 0; k < nviews; k++) {  // (views are in the render pass's attachment order: colours, then depth)
            uint32_t bit = att_index[k] < 0 ? 8 : (uint32_t)att_index[k];
            if ((cleared >> bit) & 1) pass_noload |= 1u << bit;
            cvals[k] = cv[bit];
        }
        if (pass_noload) ncv = nviews;
    }
    pass_skip_clear = pass_noload;  // (the game's own clears only: what perf_noload adds below is no clear of the game's)
    // (MHGU_PERF_RODEPTH) the depth target read-only: a draw samples it where it is (resolve_feedback)
    bool ro = ro_want && pass_depth != VK_FORMAT_UNDEFINED && !pass_msaa && pass_samples == 1 && dt == st.rt.depth;
    if (ro) {
        pass_noload &= ~(1u << 8);  // (read-only: loaded, never cleared as it begins; no leading depth clear before a draw)
        pass_skip_clear &= ~(1u << 8);
        stat_ro_passes++;
        for (uint32_t k = 0; k < nviews && depth_clear_pending; k++)
            if (att[k] == depth_clear_pending) depth_clear_now();
        for (uint32_t k = 0; k < nviews && fill.dst; k++)
            if (att[k] == fill.dst || fill.kind == 1) fill_now();  // (a copy: before any other pass)
    } else {  // (MHGU_PERF_*: more attachments begun cleared, where what the pass draws first covers them whole anyway)
        VkClearValue pv[9];
        uint32_t more = perf_noload(minw, minh, att, att_index, nviews, pv);
        for (uint32_t k = 0; k < nviews; k++) {
            uint32_t bit = att_index[k] < 0 ? 8 : (uint32_t)att_index[k];
            if (((more & ~pass_noload) >> bit) & 1) pass_noload |= 1u << bit, cvals[k] = pv[bit];
        }
        if (pass_noload) ncv = nviews;
    }
    pass_ro_depth = ro;
    uint32_t rp_bits = pass_noload | (ro ? RP_RO_DEPTH : 0);
    VkRenderPass begin_rp = rp_bits ? render_pass(pass_color, pass_ncolor, pass_depth, pass_samples, rp_bits) : pass_rp;
    if (pass_noload) stat_noload++;
    gp_mark("  (between passes)");
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = begin_rp, .framebuffer = fb,
                                .renderArea = {{0, 0}, {minw, minh}}, .clearValueCount = ncv, .pClearValues = ncv ? cvals : NULL};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    ss_forget();
    in_pass = true;
    if (fill_index >= 0) {  // (H scaled into H', or the FXAA pass's copy, as the pass's first draw)
        fill_draw(cb, fill.src, pass_rp, (uint32_t)fill_index, pass_ncolor, pass_depth, pass_samples, minw, minh, fill.kind);
        fill.dst = fill.src = NULL, fill_index = -1;
        stat_fills++;
    }
    pass_draws_at = stat_draws;
    lost_pass(minw, minh, pass_ncolor ? pass_color[0] : VK_FORMAT_UNDEFINED, pass_ncolor, pass_depth, pass_samples);
    if (g_dump_active) {  // (debug) the pass's targets: texture object, size, NVN format
        char tg[400];
        int n = 0;
        for (int i = 0; i < st.rt.ncolor && n < 300; i++)
            if (st.rt.color[i]) n += snprintf(tg + n, sizeof tg - n, " c%d %p %ux%u f%#x", i, (void *)st.rt.color[i],
                                              st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format);
        if (st.rt.depth) snprintf(tg + n, sizeof tg - n, " d %p %ux%u f%#x", (void *)st.rt.depth, st.rt.depth->width,
                                  st.rt.depth->height, st.rt.depth->format);
        else tg[n] = 0;
        LOG("[dump] begin pass %03d:%s%s\n", g_dump_no, tg, pass_msaa ? " (MSAA)" : "");
    }
    dump_tex = st.rt.ncolor ? st.rt.color[0] : NULL;
    dump_depth = st.rt.depth;
    dump_lv = st.rt.has_cview[0] && (st.rt.cview[0].setmask & 1) ? st.rt.cview[0].base_level : 0;
    dump_ly = st.rt.has_cview[0] && (st.rt.cview[0].setmask & 2) ? st.rt.cview[0].min_layer : 0;
    stat_passes++;
}

static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    if (pass_sx != 1.0f) *w = (int)(*w * pass_sx + 0.5f), *h = (int)(*h * pass_sy + 0.5f);  // (FSR: output-size stand-ins)
}

volatile int g_shadow_scissor_mode;  // (debug, F8 in host_input.c) depth-only passes: 0 as set, 1 none, 2 mirrored in the viewport

// The pass's size as the guest sees it: its viewport and scissor (FSR: the output-size stand-ins scaled back by pass_s*)
static void pass_guest_size(float *w, float *h) { *w = roundf((float)pass_w / pass_sx), *h = roundf((float)pass_h / pass_sy); }

// A 3D model shown inside a HUD window (the Smithy's weapon / armor preview, the equipment screens) is drawn through a
// viewport and scissor the game sized for its 1280x720 canvas, spread over the whole picture. The window itself is moved
// and squeezed by cbuf_info (the same anchoring), so the model would stay where the window was before it moved and spill
// past its frame: on a picture wider than 16:9 sideways, on a narrower one (16:10) up or down. x' = sx + x * kx (and
// likewise y) puts the rectangle where the window went: sideways left / right / centre by where it is, vertically as the
// window's frame (the last HUD quad drawn over the viewport, ui_recent) was anchored. The scissor follows it exactly; the
// viewport keeps its size and only its centre moves (the game's projection is made for that shape; squeezing it squeezes
// the model).
// Returns false when the draw is not such a rectangle.
typedef struct { float sx, kx, sy, ky; } PreviewMap;
// (begin_pass, the attachments not read in: could ui_preview_map move a clear's rectangle now? Its own checks before it
// finds the window, without its side effects; true when unsure)
static bool preview_mapped(void) {
    float pw, ph;
    pass_guest_size(&pw, &ph);
    if (fabsf(g_render_aspect - 16.0f / 9.0f) <= 0.01f || !st.ds[0] || !pass_w || ph != (float)g_render_size[1]) return false;
    float w = st.viewport[0][2], h = fabsf(st.viewport[0][3]);
    return !(w < 8.0f || w > 0.9f * pw || h > 0.98f * ph);
}
// The same draw at any shape of picture, while a menu of the game's is open: a model shown in its window (the smithy's
// weapon, the item box's hunter), which the game turns with the sticks (runtime/touch.cpp: the touch stick and drags are
// the game's sticks then, #172)
static void preview_tell(void) {
    if (!ui_menu_open || !st.ds[0] || !pass_w) return;
    float pw, ph;
    pass_guest_size(&pw, &ph);
    float w = st.viewport[0][2], h = fabsf(st.viewport[0][3]);
    if (ph == (float)g_render_size[1] && !(w < 8.0f || w > 0.9f * pw || h > 0.98f * ph)) touch_hud_preview();
}
static bool ui_preview_map(PreviewMap *pm) {
    float asp = g_render_aspect, pw, ph;
    pass_guest_size(&pw, &ph);
    if (fabsf(asp - 16.0f / 9.0f) <= 0.01f || !st.ds[0] || !pass_w || ph != (float)g_render_size[1]) return false;
    ui_group_end();  // (the window drawn before it is complete: its anchor is the one to follow)
    const float *vp = st.viewport[0];
    float w = vp[2], h = fabsf(vp[3]);
    if (w < 8.0f || w > 0.9f * pw || h > 0.98f * ph) return false;  // (the scene itself: the whole picture)
    *pm = (PreviewMap){0, 1, 0, 1};
    // The window it is drawn in: the last quad over the viewport (inside it sideways, not below it). The last quad of all
    // can be a button prompt under the window ("Rotate" under the Palico Scout preview), anchored apart from it.
    int vanchor = ui_nrecent ? ui_recent[(ui_nrecent - 1) % UI_RECENT].vanchor : 1, hanchor = -1;
    {
        float x0 = vp[0] / pw * 1280.0f, x1 = (vp[0] + w) / pw * 1280.0f, y1 = (fminf(vp[1], vp[1] + vp[3]) + h) / ph * 720.0f;
        for (uint32_t i = ui_nrecent; i > 0 && ui_nrecent - i < UI_RECENT; i--) {
            uint32_t k = (i - 1) % UI_RECENT;
            if (ui_recent[k].x >= x0 && ui_recent[k].x <= x1 && ui_recent[k].y <= y1) {
                vanchor = ui_recent[k].vanchor, hanchor = ui_recent[k].hanchor;
                break;
            }
        }
    }
    if (asp > 16.0f / 9.0f) {
        float s = (16.0f / 9.0f) / asp;
        float ui = ui_frontend_pass ? 16.0f / 9.0f : g_ui_anchor_aspect;  // (front-end screens: as their HUD, cbuf_info)
        float anchor = ui < asp ? ui : asp;
        float left_px = 640.0f * (1.0f - anchor / asp) * (pw / 1280.0f);
        float cx = (vp[0] + w * 0.5f) / pw * 1280.0f;
        // sideways as its window went (the smithy's parchment, layer 100, keeps the middle 16:9 with the menu: by its own
        // place the model went Left, out of it), or by where it is when no window is over it
        if (hanchor < 0) hanchor = cx <= 430.0f ? 0 : cx >= 650.0f ? 1 : 2;
        pm->kx = s;
        if (hanchor == 0) pm->sx = left_px;
        else if (hanchor == 1) pm->sx = pw - left_px - pw * s;
        else pm->sx = pw * 0.5f * (1.0f - s);
    } else {
        float sy = asp / (16.0f / 9.0f);
        pm->ky = sy;
        if (vanchor == 0) pm->sy = 0;
        else if (vanchor == 2) pm->sy = ph * (1.0f - sy);
        else pm->sy = ph * 0.5f * (1.0f - sy);
    }
    if (g_hudtrace_active)
        LOG("[hud] preview vp %.0f,%.0f %.0fx%.0f sc %d,%d %dx%d (pass %.0fx%.0f) -> sx %.1f kx %.3f sy %.1f ky %.3f\n", vp[0], vp[1], w, h,
            st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], pw, ph, pm->sx, pm->kx, pm->sy, pm->ky);
    return true;
}

static VkRect2D scissor_rect(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return (VkRect2D){{0, 0}, {pass_w, pass_h}};
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    if (ui_relax_scissor) { x = y = 0; sw = w; sh = h; }
    if (ui_scissor_v >= 0 && !ui_relax_scissor) {  // (16:10: a HUD piece's scissor moves with it, unless it is the whole pass)
        float pw, ph;
        pass_guest_size(&pw, &ph);
        if (x > 0 || y > 0 || sw < (int)pw || sh < (int)ph) {
            float sy = g_render_aspect / (16.0f / 9.0f), y0 = (float)y, y1 = (float)(y + sh);
            float off = ui_scissor_v == 0 ? 0.0f : ui_scissor_v == 2 ? ph * (1.0f - sy) : ph * 0.5f * (1.0f - sy);
            y = (int)floorf(off + y0 * sy);
            sh = (int)ceilf(off + y1 * sy) - y;
        }
    }
    if (ui_scissor_mid && !ui_relax_scissor && g_render_aspect > 16.0f / 9.0f) {
        float pw, ph;
        pass_guest_size(&pw, &ph);
        if (x > 0 || sw < (int)pw) {
            float k = (16.0f / 9.0f) / g_render_aspect, cx = pw * 0.5f;
            float x0 = cx + ((float)x - cx) * k, x1 = cx + ((float)(x + sw) - cx) * k;
            x = (int)floorf(x0);
            sw = (int)ceilf(x1) - x;
        }
    }
    {
        PreviewMap pm;
        if (!ui_relax_scissor && ui_preview_map(&pm)) {
            float x0 = pm.sx + x * pm.kx, x1 = pm.sx + (x + sw) * pm.kx, y0 = pm.sy + y * pm.ky, y1 = pm.sy + (y + sh) * pm.ky;
            x = (int)lroundf(x0);
            sw = (int)lroundf(x1 - x0);
            y = (int)lroundf(y0);
            sh = (int)lroundf(y1 - y0);
        }
    }
    if (!st.rt.ncolor && g_shadow_scissor_mode == 1) { x = (int)st.viewport[0][0]; y = (int)st.viewport[0][1]; sw = (int)st.viewport[0][2]; sh = (int)st.viewport[0][3]; }
    if (!st.rt.ncolor && g_shadow_scissor_mode == 2) y = 2 * (int)st.viewport[0][1] + (int)st.viewport[0][3] - y - sh;
    // FSR: the guest's rectangle (render size) on the output-size stand-in. Not a relaxed one: that is the whole pass
    // already, in the stand-in's size (pass_size). Scaled again it covered pass_sx of it, which is below 1 with Toaster's
    // HUD (at most 540 lines) under a taller 3D picture: the item bar, drawn through it, was cut off past 75 % of the
    // screen at 66 % on a 1080-line one
    if (pass_sx != 1.0f && !ui_relax_scissor) {
        double x1 = floor(x * (double)pass_sx), y1 = floor(y * (double)pass_sy);
        double x2 = ceil(((double)x + sw) * pass_sx), y2 = ceil(((double)y + sh) * pass_sy);
        x = (int)x1, y = (int)y1;
        sw = (int)fmin(x2 - x1, 1 << 30), sh = (int)fmin(y2 - y1, 1 << 30);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (w > (int)pass_w) w = (int)pass_w;
    if (h > (int)pass_h) h = (int)pass_h;
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w || sw > w) sw = w - x;
    if (y + sh > h || sh > h) sh = h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};
}

static void apply_scissor(VkCommandBuffer cb) {
    VkRect2D r = scissor_rect();
    if (!ss_need(SS_SC, !memcmp(&r, &ss.sc, sizeof r))) return;
    ss.sc = r;
    vkCmdSetScissor(cb, 0, 1, &r);
}

// The translator's clip space is Metal's (y up in NDC, origin top-left): a negative-height viewport.
static void set_viewport(VkCommandBuffer cb, float x, float y, float w, float h, float zn, float zf) {
    if (w <= 0) w = 1;
    if (h == 0) h = 1;
    VkViewport vp = {x, y + h, w, -h, fminf(fmaxf(zn, 0), 1), fminf(fmaxf(zf, 0), 1)};
    if (!ss_need(SS_VP, !memcmp(&vp, &ss.vp, sizeof vp))) return;
    ss.vp = vp;
    vkCmdSetViewport(cb, 0, 1, &vp);
}

// ================================================================== fixed-function tables
static const VkDynamicState DYN[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                     VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                     VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
#define NDYN (sizeof DYN / sizeof DYN[0])

static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

static VkFormat vertex_format(uint32_t f) {
    static const VkFormat tab[0x31] = {
        [0x01] = VK_FORMAT_R8_UNORM, [0x02] = VK_FORMAT_R8_SNORM, [0x03] = VK_FORMAT_R8_UINT, [0x04] = VK_FORMAT_R8_SINT,
        [0x05] = VK_FORMAT_R16_SFLOAT, [0x06] = VK_FORMAT_R16_UNORM, [0x07] = VK_FORMAT_R16_SNORM, [0x08] = VK_FORMAT_R16_UINT,
        [0x09] = VK_FORMAT_R16_SINT, [0x0A] = VK_FORMAT_R32_SFLOAT, [0x0B] = VK_FORMAT_R32_UINT, [0x0C] = VK_FORMAT_R32_SINT,
        [0x0D] = VK_FORMAT_R8G8_UNORM, [0x0E] = VK_FORMAT_R8G8_SNORM, [0x0F] = VK_FORMAT_R8G8_UINT, [0x10] = VK_FORMAT_R8G8_SINT,
        [0x11] = VK_FORMAT_R16G16_SFLOAT, [0x12] = VK_FORMAT_R16G16_UNORM, [0x13] = VK_FORMAT_R16G16_SNORM,
        [0x14] = VK_FORMAT_R16G16_UINT, [0x15] = VK_FORMAT_R16G16_SINT, [0x16] = VK_FORMAT_R32G32_SFLOAT,
        [0x17] = VK_FORMAT_R32G32_UINT, [0x18] = VK_FORMAT_R32G32_SINT, [0x19] = VK_FORMAT_R8G8B8_UNORM,
        [0x1A] = VK_FORMAT_R8G8B8_SNORM, [0x1B] = VK_FORMAT_R8G8B8_UINT, [0x1C] = VK_FORMAT_R8G8B8_SINT,
        [0x1D] = VK_FORMAT_R16G16B16_SFLOAT, [0x1E] = VK_FORMAT_R16G16B16_UNORM, [0x1F] = VK_FORMAT_R16G16B16_SNORM,
        [0x20] = VK_FORMAT_R16G16B16_UINT, [0x21] = VK_FORMAT_R16G16B16_SINT, [0x22] = VK_FORMAT_R32G32B32_SFLOAT,
        [0x23] = VK_FORMAT_R32G32B32_UINT, [0x24] = VK_FORMAT_R32G32B32_SINT, [0x25] = VK_FORMAT_R8G8B8A8_UNORM,
        [0x26] = VK_FORMAT_R8G8B8A8_SNORM, [0x27] = VK_FORMAT_R8G8B8A8_UINT, [0x28] = VK_FORMAT_R8G8B8A8_SINT,
        [0x29] = VK_FORMAT_R16G16B16A16_SFLOAT, [0x2A] = VK_FORMAT_R16G16B16A16_UNORM, [0x2B] = VK_FORMAT_R16G16B16A16_SNORM,
        [0x2C] = VK_FORMAT_R16G16B16A16_UINT, [0x2D] = VK_FORMAT_R16G16B16A16_SINT, [0x2E] = VK_FORMAT_R32G32B32A32_SFLOAT,
        [0x2F] = VK_FORMAT_R32G32B32A32_UINT, [0x30] = VK_FORMAT_R32G32B32A32_SINT};
    if (f < 0x31 && tab[f]) return tab[f];
    switch (f) {
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x51: return VK_FORMAT_R8_UINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

// 3-component 8/16-bit vertex formats aren't universally supported: read 4 components instead.
static VkFormat vertex_format_wide(VkFormat f);
static VkFormat vertex_format_supported(VkFormat f) {
    static uint8_t known[256];  // 1 ok, 2 not
    uint32_t i = f < 256 ? f : 0;
    if (!known[i]) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
        known[i] = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
        // (once per format, so a log tells whether a GPU takes this path: nothing makes the 4th component 1 there, #180;
        // a format without a 4-component twin, such as B10G11R11, is read as R32G32B32A32_SFLOAT: its data misread)
        if (known[i] == 2) {
            VkFormat w = vertex_format_wide(f);
            LOG("[vk] vertex format %d not supported: read as %d (%s)\n", (int)f, (int)w,
                w == VK_FORMAT_R32G32B32A32_SFLOAT ? "a stand-in: its data misread" : "4 components, the 4th from the next bytes");
        }
    }
    if (known[i] == 1) return f;
    return vertex_format_wide(f);
}

static VkFormat vertex_format_wide(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[vk] unknown blend factor %#x\n", f);
        return VK_BLEND_FACTOR_ONE;
    }
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_fn(uint8_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp stencil_op(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}

// ================================================================== clears
static VkPipelineLayout pc_layout(void) {  // push constants only (clears)
    static VkPipelineLayout l;
    if (!l) {
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    }
    return l;
}

// A masked clear (some channels / stencil bits only): a fullscreen triangle with the masks in the pipeline.
static VkPipeline clear_pipeline(int index, int kind, uint32_t mask, bool depth, bool stencil, uint32_t smask) {
    static struct { uint64_t key; VkPipeline pipe; } tab[256];
    uint64_t key = 0xcbf29ce484222325ull;
    uint64_t parts[] = {(uint64_t)index, (uint64_t)kind, mask, depth, stencil, smask, (uint64_t)(uintptr_t)pass_rp, pass_ncolor};
    key = hash_words(key, parts, sizeof parts);
    if (!key) key = 1;
    uint32_t slot = (uint32_t)key & 255;
    for (int p = 0; p < 256; p++, slot = (slot + 1) & 255) {
        if (tab[slot].key == key) return tab[slot].pipe;
        if (!tab[slot].key) break;
    }
    char fsname[32];
    if (index >= 0) snprintf(fsname, sizeof fsname, "clear.frag.%c%d", kind == 1 ? 'i' : kind == 2 ? 'u' : 'f', index);
    else snprintf(fsname, sizeof fsname, "clear.frag.none");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("clear.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fsname), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)pass_samples};
    VkStencilOpState so = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xFF, smask, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = depth, .depthWriteEnable = depth, .depthCompareOp = VK_COMPARE_OP_ALWAYS,
                                                .stencilTestEnable = stencil, .front = so, .back = so};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < pass_ncolor; i++)
        if ((int)i == index) cba[i].colorWriteMask = mask & 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 3, .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pc_layout(), .renderPass = pass_rp};
    VkPipeline p = VK_NULL_HANDLE;
    if (sync_pipeline(&pi, &p) != VK_SUCCESS) LOG("[vk] clear pipeline failed\n");
    tab[slot].key = key;
    tab[slot].pipe = p;
    return p;
}

volatile int g_clear_draw = -1;  // colour clears as draws (MHGU_CLEAR_DRAW, F9 toggles; host_input.c)

// The Adreno 630's driver of Android 10 (1.1.87, vulkan.sdm845.so) dies inside vkCmdClearAttachments, at the first clear of
// the game (two reports; a null at +0x1dd318): on drivers that old a clear is a draw. MHGU_CLEAR_FORCE=1 tries it anywhere
static bool clears_drawn(void) {
    static int on = -1;
    if (on < 0) {
        on = getenv("MHGU_CLEAR_FORCE") || (vk_gpu_adreno() && g_vk.props.apiVersion < VK_MAKE_VERSION(1, 1, 128));
        if (on) LOG("[vk] clears as draws: vkCmdClearAttachments is not used on this driver\n");
    }
    return on;
}

static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask) {
    if (depth || stencil) {  // (MSAA, a picture 3D models are drawn into: their depth clear into the samples, pass_models)
        ms_want_depth = true;
        if (in_pass && pass_models && !pass_msaa && !pass_want_depth) st.rt_dirty = true;
    }
    models_fxaa_track(depth || stencil);  // (FXAA: the same, models_fxaa_end)
    if ((depth || stencil) && in_pass && pass_ro_depth) st.rt_dirty = true, stat_ro_left++;  // (a read-only target: a new pass)
    ro_want = false;
    begin_pass();
    if (depth || stencil) at_written();  // (MHGU_PERF_SHADOW_CLEAR: the atlas written by other than a draw)
    if (g_dump_active) LOG("[dump]   clear %d mask %#x depth %d stencil %d (after %llu draws)\n", index, mask, depth, stencil,
                           (unsigned long long)(stat_draws - pass_draws_at));
    VkCommandBuffer cb = vk_cmd();
    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    if (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED)) return;
    bool has_stencil = pass_depth == VK_FORMAT_D24_UNORM_S8_UINT || pass_depth == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                       pass_depth == VK_FORMAT_S8_UINT;
    bool has_depth = pass_depth != VK_FORMAT_UNDEFINED && pass_depth != VK_FORMAT_S8_UINT;
    depth = depth && has_depth;
    stencil = stencil && has_stencil;
    if (index < 0 && !depth && !stencil) return;
    // (the pass began by clearing this attachment, begin_pass: a clear of the whole of it before anything else wrote
    // there is done already, to the value the pass began with, its last such clear. Done again it was a second write of
    // the whole attachment, 2% of an Adreno 830's frame)
    bool whole = rect.offset.x == 0 && rect.offset.y == 0 && rect.extent.width >= pass_w && rect.extent.height >= pass_h;
    if (index >= 0 && (pass_skip_clear >> index) & 1) {
        if (whole && (mask & 0xF) == 0xF) return;
        pass_skip_clear &= ~(1u << index);
    } else if (index < 0 && (pass_skip_clear >> 8) & 1) {
        if (whole && depth && (!has_stencil || (stencil && (smask & 0xFF) == 0xFF))) return;
        pass_skip_clear &= ~(1u << 8);
    }
    if (g_clear_draw < 0) { const char *e = getenv("MHGU_CLEAR_DRAW"); g_clear_draw = e && atoi(e); }
    bool simple = (index < 0 || ((mask & 0xF) == 0xF && !g_clear_draw)) && (!stencil || (smask & 0xFF) == 0xFF) && !clears_drawn();
    if (simple) {
        VkClearAttachment a[2];
        uint32_t n = 0;
        if (index >= 0) {
            a[n] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)index};
            memcpy(a[n].clearValue.color.float32, color, 16);
            n++;
        }
        VkImageAspectFlags dsa = (depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (dsa) a[n++] = (VkClearAttachment){.aspectMask = dsa, .clearValue.depthStencil = {dval, sval}};
        VkClearRect cr = {rect, 0, 1};
        if (n) vkCmdClearAttachments(cb, n, a, 1, &cr);
        return;
    }
    VkPipeline p = clear_pipeline(index, kind, mask, depth, stencil, smask & 0xFF);
    if (!p) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    ss_forget();  // (its own pipeline and state: the next draw sets everything again)
    if (stencil) vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sval);
    int w, h;
    pass_size(&w, &h);
    VkViewport vp = {0, 0, (float)(w ? w : (int)pass_w), (float)(h ? h : (int)pass_h), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &rect);
    float pc[8] = {0};
    if (color) memcpy(pc, color, 16);
    pc[4] = dval;
    vkCmdPushConstants(cb, pc_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    lost_rec[cur_sub].drawn_clears++;
}

// ================================================================== programs
#define MAX_BIND 96
typedef struct {
    ShaderInfo *vi, *fi;
    uint64_t code[2], cmax[2];
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    _Atomic int state;  // 0 building, 1 ready, 2 failed, 3 dormant (in programs.bin, not built: prog_want)
    uint64_t t_req;     // (stats) when first needed
    uint64_t chash[2], ck;  // program cache: hashes of the vertex / fragment code, the lookup key
    long src_off;           // program cache: where its sources are in programs.src (when src_saved)
    uint32_t vlen, flen;
    bool src_saved;
    bool wanted;            // (under ready_lock) a draw needs it: a prewarm build goes on to make its modules
    bool loaded;            // (render thread) built from the program cache (programs.src, cache/spirv): a draw may wait
                            // for it a little, and its ShaderInfo (programs.bin's, or the prewarm's) keys its pipelines
                            // before it is built (pipe_ahead)
    struct Pipe *prewarm;   // pipelines of earlier runs to build once this is (pipeline cache)
    FsrCapProg *fsrcap;     // FSR object motion: the capture variant of its vertex shader (fsr.c)
    uint32_t dyn_mask[2];   // (MHGU_PERF_DYNUBO) each set's uniform buffers bound with a dynamic offset (bit: binding)
    uint8_t ndyn[2];        // (and how many)
} Prog;
// The draw being set up (draw(): what perf_noload sees as the first command of a pass begun for it)
static const CmdRec *cur_draw;
static bool cur_draw_indexed;
static Prog *cur_draw_pr;
// exec_list's place at each depth of command lists called from others (at_look reads ahead through them)
static uint64_t exec_pos[9];
static uint64_t exec_call_i[9];
static int exec_top;
static void prewarm_pipelines(Prog *p);

// (MHGU_PERF_DYNUBO) Which of a program's uniform buffers (the ones cbuf_info fills: bindings 0..16) get a dynamic offset,
// up to perf_dyn_limit in its pipeline layout: the game's (3..16: a new copy for most draws, the per-object ones) of the
// vertex then the fragment stage, then the compiler constants (1: a new copy whenever the list wrote guest memory), then
// the driver constants (0: one copy shared by most draws). The rest keep their place in the set, as before.
static void dyn_pick(Prog *p) {
    uint32_t left = perf_dyn_limit;
    p->dyn_mask[0] = p->dyn_mask[1] = 0;
    for (int pass = 0; pass < 3; pass++)
        for (uint32_t set = 0; set < 2; set++)
            for (int i = 0; i < p->nbinds && left; i++) {
                const VkBindingInfo *b = &p->binds[i];
                if (b->set != set || b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->binding >= 17) continue;
                if (pass == 0 ? b->binding < 3 : b->binding != (pass == 1 ? 1u : 0u)) continue;
                if (p->dyn_mask[set] >> b->binding & 1) continue;
                p->dyn_mask[set] |= 1u << b->binding, left--;
            }
    p->ndyn[0] = (uint8_t)__builtin_popcount(p->dyn_mask[0]), p->ndyn[1] = (uint8_t)__builtin_popcount(p->dyn_mask[1]);
    static _Atomic int said;
    if (atomic_fetch_add(&said, 1) < 4) {  // (a few: how a GPU's limit splits them)
        int all = 0;
        for (int i = 0; i < p->nbinds; i++)
            all += p->binds[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && p->binds[i].binding < 17 && p->binds[i].set < 2;
        LOG("[perf] dynamic uniform buffers: program %llx/%llx, %d of its %d (vertex %#x, fragment %#x)\n",
            (unsigned long long)p->code[0], (unsigned long long)p->code[1], p->ndyn[0] + p->ndyn[1], all, p->dyn_mask[0],
            p->dyn_mask[1]);
    }
}

static VkPipelineLayout make_layout(Prog *p, int nsets, VkShaderStageFlags s0, VkShaderStageFlags s1) {
    const VkBindingInfo *binds = p->binds;
    VkBindingInfo dyn[MAX_BIND];
    if (p->dyn_mask[0] | p->dyn_mask[1]) {  // (MHGU_PERF_DYNUBO: those buffers' type in the layouts)
        memcpy(dyn, p->binds, (size_t)p->nbinds * sizeof *dyn);
        for (int i = 0; i < p->nbinds; i++)
            if (dyn[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && dyn[i].set < 2 && dyn[i].binding < 17 &&
                (p->dyn_mask[dyn[i].set] >> dyn[i].binding & 1))
                dyn[i].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        binds = dyn;
    }
    p->set_layout[0] = vk_set_layout(binds, p->nbinds, 0, s0);
    if (nsets > 1) p->set_layout[1] = vk_set_layout(binds, p->nbinds, 1, s1);
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)nsets,
                                     .pSetLayouts = p->set_layout};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

static void save_glsl(const char *src, uint64_t addr, const char *suffix) {
    if (!getenv("MHGU_SAVE_GLSL")) return;
    mkdir("build", 0755);
    mkdir("build/glsl", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/glsl/%llx_%s.glsl", (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "w");
    if (f) { fputs(src, f); fclose(f); }
}

static void compile_program(void *arg) {
    Prog *p = arg;
    uint64_t t0 = plat_now_ns();
    size_t nv = 0, nf = 0;
    uint32_t *sv = vk_glsl_to_spirv(p->vi->msl, STAGE_VERTEX, "vs", &nv);
    const char *fsrc = p->fi->msl;
    char *over = NULL;
    if (getenv("MHGU_GLSL_OVERRIDE")) {  // (debug) glsl_override/<code address>_fs.glsl replaces the translated shader
        char path[96];
        snprintf(path, sizeof path, "glsl_override/%llx_fs.glsl", (unsigned long long)p->code[1]);
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            over = calloc(1, (size_t)n + 1);
            if (fread(over, 1, (size_t)n, f) == (size_t)n) fsrc = over;
            fclose(f);
            LOG("[shader] using %s\n", path);
        }
    }
    uint32_t *sf = sv ? vk_glsl_to_spirv(fsrc, STAGE_FRAGMENT, "fs", &nf) : NULL;
    free(over);
    if (!sv || !sf) { free(sv); free(sf); set_state(&p->state, 2, p->t_req); prewarm_pipelines(p); return; }
    p->nbinds = vk_reflect(sv, nv, p->binds, MAX_BIND);
    p->nbinds += vk_reflect(sf, nf, p->binds + p->nbinds, MAX_BIND - p->nbinds);
    p->vs = vk_module(sv, nv);
    p->fs = vk_module(sf, nf);
    free(sv);
    free(sf);
    if (perf.dynubo) dyn_pick(p);
    p->layout = make_layout(p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    if (g_fsr_objvel) p->fsrcap = vk_fsr_capprog_new(p->vi->msl);  // (FSR: its vertex shader's source, for the capture)
    free(p->vi->msl);  // (not needed any more: the source of every program ever seen would add up)
    free(p->fi->msl);
    p->vi->msl = p->fi->msl = NULL;
    atomic_fetch_add(&stat_compiles, 1);
    job_stat(1, t0);
    set_state(&p->state, p->vs && p->fs ? 1 : 2, p->t_req);
    prewarm_pipelines(p);
}

static void progcache_store(Prog *p);

// Translates p and records it in programs.bin; false: failed (state 2). quiet: the prewarm's (not logged: a program
// no draw uses may hit translator gaps no one sees)
static _Atomic uint64_t stat_pw_failed;
static bool translate_core(Prog *p, bool quiet) {
    uint64_t t0 = plat_now_ns();
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    pthread_mutex_lock(&translate_lock);
    p->vi = maxwell_translate((const uint8_t *)G2H((uint32_t)p->code[0]), p->cmax[0], STAGE_VERTEX, &p->vkey);
    if (p->vi->msl) {
        p->fkey.vs_out_mask = p->vi->out_mask;
        p->fi = maxwell_translate((const uint8_t *)G2H((uint32_t)p->code[1]), p->cmax[1], STAGE_FRAGMENT, &p->fkey);
    }
    pthread_mutex_unlock(&translate_lock);
    job_stat(0, t0);
    if (!p->vi->msl || !p->fi->msl) {
        if (quiet) stat_pw_failed++;
        else if (!p->vi->msl) LOG("[shader] VS translate failed: %s (code %llx)\n", p->vi->error, (unsigned long long)p->code[0]);
        else LOG("[shader] FS translate failed: %s (code %llx)\n", p->fi->error, (unsigned long long)p->code[1]);
        set_state(&p->state, 2, p->t_req);
        prewarm_pipelines(p);  // (a pipeline list's, waiting for it: failed too)
        return false;
    }
    if (!quiet && (p->vi->uses_gmem || p->fi->uses_gmem)) {
        static int n;
        if (n++ < 10) LOG("[shader] program %llx/%llx reads global memory (not available: zeros)\n",
                          (unsigned long long)p->code[0], (unsigned long long)p->code[1]);
    }
    if (maxwell_vs_trim) maxwell_trim_outputs(p->vi, p->fi->attr_mask);  // (what the fragment shader reads)
    save_glsl(p->vi->msl, p->code[0], "vs");
    char fsuf[16] = "fs";
    if (p->fkey.alpha_func)  // (alpha-test variants)
        snprintf(fsuf, sizeof fsuf, "fs_at%d%s", p->fkey.alpha_func, p->fkey.alpha_coverage ? "c" : "");
    save_glsl(p->fi->msl, p->code[1], fsuf);
    progcache_store(p);
    return true;
}

static void translate_program(void *arg) {
    Prog *p = arg;
    if (translate_core(p, false)) run_job(compile_program, p);
}

// ---- program cache: cache/programs.bin keeps every translated program (reflection, keyed by a hash of the guest code
// the translator reads and the shader keys; the sources in programs.src), and the next run builds the ones draws used
// on the workers at its first draw, as Ryujinx's shader cache does: an area's programs are then ready when it is
// entered (translation is serial and was most of the wait); the prewarm's (below) wait dormant until a draw needs one.
// The files are tied to the translator (the Makefile's SHADER_ID, a hash of its sources, and CACHE_FORMAT below), so
// they outlive updates that leave the translator alone, as Ryujinx's cache does; they are appended to as programs are
// translated (a killed run keeps them). MHGU_NO_PROGRAM_CACHE=1: off
// (also off with the translator debug options MHGU_SAVE_GLSL / MHGU_SAVE_MSL / MHGU_NO_DISCARD).
#define PROG_TAB 16384
// The records are an index read whole at start-up; the sources (~27 KB a program, read back only when a dormant program
// is built) are kept apart in programs.src.
typedef struct {
    uint32_t magic, size;  // size: sizeof (ProgRec)
    uint64_t ck, chash[2], code[2], cmax[2];
    ShaderKey vkey, fkey;
    ShaderInfo vi, fi;  // (msl: NULL)
    uint64_t src_off;   // its sources in programs.src: vlen + flen bytes there
    uint32_t vlen, flen;
} ProgRec;
#define PROGREC_MAGIC 0x31475250u  // "PRG1"
#define PROGCACHE_PATH "cache/programs.bin"
#define PROGSRC_PATH "cache/programs.src"
static FILE *progcache_f, *progsrc_f;
static bool progcache_on;
typedef struct { char magic[8]; uint64_t stamp; } CacheHeader;
#define CACHE_FORMAT 1  // bump when what a programs.bin / pipelines.bin record means changes here (ProgRec, PipeKey)
static void pipecache_init(const CacheHeader *hdr);
static void plist_load(void);
static void plist_export_finish(void);
static uint64_t stat_prog_cached;
static struct { uint64_t ck; Prog *prog; } progcache_tab[PROG_TAB];

// The bytes maxwell_translate reads: NVN header + SPH, and the code up to its end (found the same way).
static uint64_t code_hash(uint64_t addr, uint64_t cmax, int stage) {
    const uint8_t *blob = (const uint8_t *)G2H((uint32_t)addr);
    size_t hdr = stage == STAGE_COMPUTE ? 0 : 0x80;
    if (cmax <= hdr) return 0;
    size_t n = (cmax - hdr) / 8, i;
    if (n > 65536) n = 65536;
    bool end = false;
    for (i = 0; i < n; i++) {
        uint64_t w;
        memcpy(&w, blob + hdr + i * 8, 8);
        if (i % 4 == 0) continue;
        int32_t off = (int32_t)((uint32_t)(w >> 20) << 8) >> 8;
        if ((w & 0xfff0000000000000ull) == 0xe240000000000000ull && off == -8 && end) { i++; break; }  // bra self
        if (w == 0 && i > 4) { i++; break; }
        end = true;
    }
    return hash_words(0x84222325cbf29ce4ull ^ i, blob, hdr + i * 8);
}

static uint64_t prog_content_key(const uint64_t chash[2], const ShaderKey *vkey, const ShaderKey *fkey) {
    uint64_t h = hash_words(0xcbf29ce484222325ull, chash, 16);
    h = hash_words(h, vkey, sizeof *vkey);
    h = hash_words(h, fkey->rt_kind, sizeof fkey->rt_kind);
    h = hash_words(h, &fkey->alpha_func, 1);
    if (fkey->alpha_coverage) h = hash_words(h, &fkey->alpha_coverage, 1);  // (MSAA variant)
    return h ? h : 1;
}

static Prog **progcache_slot(uint64_t ck, bool insert) {
    uint32_t slot = (uint32_t)(ck ^ (ck >> 31)) & (PROG_TAB - 1);
    for (int n = 0; n < PROG_TAB; n++, slot = (slot + 1) & (PROG_TAB - 1)) {
        if (progcache_tab[slot].ck == ck) return &progcache_tab[slot].prog;
        if (!progcache_tab[slot].ck) {
            if (!insert) return NULL;
            progcache_tab[slot].ck = ck;
            return &progcache_tab[slot].prog;
        }
    }
    return NULL;
}

// progcache_tab is shared by the render thread (draws) and the translate thread (the prewarm): under progcache_lock
// once progcache_init has run (progcache_ready; the prewarm waits for it).
static pthread_mutex_t progcache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t progcache_cond = PTHREAD_COND_INITIALIZER;
static bool progcache_ready;
static FILE *progcache_rf;  // reads dormant programs' sources back from programs.src (load_program; under progcache_rlock)
static pthread_mutex_t progcache_rlock = PTHREAD_MUTEX_INITIALIZER;

// A dormant program's sources, read back from programs.bin, then built (its SPIR-V is in cache/spirv: file reads).
static void load_program(void *arg) {
    Prog *p = arg;
    char *vs = malloc(p->vlen + 1), *fs = malloc(p->flen + 1);
    pthread_mutex_lock(&progcache_rlock);
    bool ok = progcache_rf && !fseek(progcache_rf, p->src_off, SEEK_SET) && fread(vs, 1, p->vlen, progcache_rf) == p->vlen &&
              fread(fs, 1, p->flen, progcache_rf) == p->flen;
    pthread_mutex_unlock(&progcache_rlock);
    if (!ok) { free(vs); free(fs); set_state(&p->state, 2, p->t_req); prewarm_pipelines(p); return; }
    vs[p->vlen] = fs[p->flen] = 0;
    p->vi->msl = vs;
    p->fi->msl = fs;
    compile_program(p);
}

extern bool maxwell_no_int64;  // (nvn/maxwell/translate.c)
static void progcache_open(void) {
    if (getenv("MHGU_NO_PROGRAM_CACHE") || getenv("MHGU_SAVE_GLSL") || getenv("MHGU_SAVE_MSL") || getenv("MHGU_NO_DISCARD"))
        return;
    CacheHeader hdr = {"MHGUPRG", sizeof(ProgRec)}, fh;
    hdr.stamp = hash_words(hdr.stamp, (uint64_t[]){CACHE_FORMAT}, 8);
    // (the sources depend on 64-bit integers: translated with them, a program asks for a capability a driver without
    // them must not be given - the validation layer said so after a switch from one driver to another)
    hdr.stamp = hash_words(hdr.stamp, (uint64_t[]){maxwell_no_int64 ? 0x6e6f3634ull : 0x69363400ull}, 8);
    // (the translator's performance switches give other sources: a run with other ones translates anew)
    uint64_t sw = (maxwell_nminmax ? 1u : 0u) | (maxwell_ipa_fold ? 2u : 0u) | (maxwell_vs_trim ? 4u : 0u);
    if (sw) hdr.stamp = hash_words(hdr.stamp, &sw, 8);
#ifdef SHADER_ID
    hdr.stamp = hash_words(hdr.stamp, SHADER_ID, sizeof SHADER_ID - 1);
#else  // (built without the Makefile's id: tied to this executable, size + time stamp)
    char exe[1024];
    struct stat es;
    if (!plat_exe_path(exe, sizeof exe) || stat(exe, &es)) return;
    hdr.stamp = hash_words(hdr.stamp, (uint64_t[]){(uint64_t)es.st_size, (uint64_t)es.st_mtime}, 16);
#endif
    mkdir("cache", 0755);
    uint64_t t0 = plat_now_ns();
    int n = 0, used = 0;
    long good = 0;
    FILE *f = fopen(PROGCACHE_PATH, "r+b"), *sf = fopen(PROGSRC_PATH, "r+b");
    if (f && sf && fread(&fh, sizeof fh, 1, f) == 1 && !memcmp(&fh, &hdr, sizeof hdr)) {
        good = (long)sizeof hdr;
        fseek(sf, 0, SEEK_END);
        uint64_t send = (uint64_t)ftell(sf);
        ProgRec r;
        while (fread(&r, sizeof r, 1, f) == 1) {
            if (r.magic != PROGREC_MAGIC || r.size != sizeof r || r.vlen > (16u << 20) || r.flen > (16u << 20)) break;
            if (r.src_off + r.vlen + r.flen > send) break;  // (its sources cut short by a killed run)
            good = ftell(f);
            long src = (long)r.src_off;
            Prog **slot = progcache_slot(r.ck, true);
            if (!slot || *slot) continue;  // (a duplicate: translated twice in a run)
            Prog *p = calloc(1, sizeof *p);
            p->vi = malloc(sizeof *p->vi);
            p->fi = malloc(sizeof *p->fi);
            *p->vi = r.vi;
            *p->fi = r.fi;
            p->vi->msl = p->fi->msl = NULL;
            p->src_off = src;
            p->src_saved = true;
            p->vlen = r.vlen;
            p->flen = r.flen;
            memcpy(p->code, r.code, sizeof p->code);
            memcpy(p->cmax, r.cmax, sizeof p->cmax);
            memcpy(p->chash, r.chash, sizeof p->chash);
            p->ck = r.ck;
            p->vkey = r.vkey;
            p->fkey = r.fkey;
            atomic_store(&p->state, 3);
            *slot = p;
            n++;
        }
        fseek(f, good, SEEK_SET);
        if (ftruncate(fileno(f), good)) {}  // (a record cut short by a killed run)
    } else {
        if (f) fclose(f);
        if (sf) fclose(sf);
        f = fopen(PROGCACHE_PATH, "wb");
        sf = fopen(PROGSRC_PATH, "wb");
        if (f) fwrite(&hdr, sizeof hdr, 1, f);
    }
    if (!f || !sf) { if (f) fclose(f); if (sf) fclose(sf); return; }
    fflush(f);
    fseek(sf, 0, SEEK_END);
    progcache_f = f;
    progsrc_f = sf;
    progcache_rf = fopen(PROGSRC_PATH, "rb");
    progcache_on = true;
    pipecache_init(&hdr);  // (attaches the recorded pipelines to their programs before these are built)
    plist_load();          // (and the shipped list's whose programs are in programs.bin)
    plist_export_finish();
    // Programs a draw has used (each has recorded pipelines) are built now; the others (the prewarm's) stay dormant
    // until a draw wants one (prog_want)
    pthread_mutex_lock(&compile_q.lock);
    compile_q.watch = 2, compile_q.watch_t0 = t0, compile_q.watch_jobs = 0;
    pthread_mutex_unlock(&compile_q.lock);
    for (int i = 0; i < PROG_TAB; i++) {
        Prog *p = progcache_tab[i].prog;
        if (!p || !p->prewarm) continue;
        atomic_store(&p->state, 0);
        p->loaded = true;
        run_job(load_program, p);
        used++;
    }
    pthread_mutex_lock(&compile_q.lock);
    compile_q.watch = used && !sync_shaders() ? 1 : 0;
    queue_caught_up(&compile_q);
    pthread_mutex_unlock(&compile_q.lock);
    LOG("[shader] program cache: %d programs (%d used before: building on the workers), read in %.0f ms\n", n, used,
        (plat_now_ns() - t0) / 1e6);
}

static void progcache_init(void) {
    static bool done;
    if (done) return;
    done = true;
    progcache_open();
    pthread_mutex_lock(&progcache_lock);
    progcache_ready = true;
    pthread_cond_broadcast(&progcache_cond);
    pthread_mutex_unlock(&progcache_lock);
}

// (translate thread only: the one writer)
static void progcache_store(Prog *p) {
    if (!progcache_f) return;
    ProgRec r;
    memset(&r, 0, sizeof r);
    r.magic = PROGREC_MAGIC;
    r.ck = p->ck;
    memcpy(r.chash, p->chash, sizeof r.chash);
    memcpy(r.code, p->code, sizeof r.code);
    memcpy(r.cmax, p->cmax, sizeof r.cmax);
    r.vkey = p->vkey;
    r.fkey = p->fkey;
    r.vi = *p->vi;
    r.fi = *p->fi;
    r.vi.msl = r.fi.msl = NULL;
    r.vlen = (uint32_t)strlen(p->vi->msl);
    r.flen = (uint32_t)strlen(p->fi->msl);
    r.size = (uint32_t)sizeof r;
    long at = ftell(progsrc_f);
    if (at < 0 || fwrite(p->vi->msl, 1, r.vlen, progsrc_f) != r.vlen || fwrite(p->fi->msl, 1, r.flen, progsrc_f) != r.flen) return;
    fflush(progsrc_f);  // (the sources before the record that points at them)
    r.src_off = (uint64_t)at;
    if (fwrite(&r, sizeof r, 1, progcache_f) == 1) {
        p->src_off = at;
        p->src_saved = true;
        p->vlen = r.vlen;
        p->flen = r.flen;
    }
    fflush(progcache_f);
}

// A draw needs p (found in the program cache): a dormant one is loaded on the workers; one the prewarm is still
// building goes on to make its modules.
static void prog_want(Prog *p) {
    bool load = false;
    pthread_mutex_lock(&ready_lock);
    p->wanted = true;
    if (atomic_load(&p->state) == 3) { atomic_store(&p->state, 0); load = true; }
    pthread_mutex_unlock(&ready_lock);
    if (!atomic_load(&p->state)) p->t_req = plat_now_ns();
    if (load && HITCH_ON) hitch_add(HC_PROG_LOAD, 0, 0);
    if (load) p->loaded = true, run_job(load_program, p);
}

// ---- prewarm: the game makes all its programs at start-up (nvnProgramSetShaders, ~6300 vertex / fragment pairs
// before its first frame). Each one not in programs.bin yet is translated with the keys most draws use (float vertex
// inputs and colour targets, no alpha test) on the translate thread and its SPIR-V compiled into cache/spirv on the
// workers, behind every build a draw asked for (the queues' low lists), then left dormant: when a draw first needs it
// its modules come from the SPIR-V cache in about a millisecond, within the per-frame wait. So a program is ready
// the first time it is drawn even on a first run once the prewarm has got to it (it starts at the first frame; a
// draw needing one it has not reached yet is skipped, as before, never waited for past the budget). Variants with
// other keys (integer vertex inputs: skinned models; alpha-tested materials) are still made on first use.
// MHGU_NO_PREWARM=1: off.
typedef struct { uint64_t code[2], cmax[2]; } PrewarmReq;
static void plist_seen(const uint64_t code[2], const uint64_t cmax[2], const uint64_t chash[2]);  // (pipelines.list)
static uint64_t pw_t0;
static _Atomic uint64_t pw_total_new, pw_total_known;

static void prewarm_done(void) {
    if (atomic_fetch_sub(&stat_pw_left, 1) != 1) return;
    LOG("[shader] prewarm: %llu programs translated, %llu already cached, %llu failed, %.1f s after the first one\n",
        (unsigned long long)atomic_load(&pw_total_new), (unsigned long long)atomic_load(&pw_total_known),
        (unsigned long long)atomic_load(&stat_pw_failed), (plat_now_ns() - pw_t0) / 1e9);
}

// (workers, low list) the SPIR-V into cache/spirv; then the sources go and the program is dormant, unless a draw
// already wants it.
static void prewarm_compile(void *arg) {
    Prog *p = arg;
    size_t n;
    free(vk_glsl_to_spirv(p->vi->msl, STAGE_VERTEX, "vs", &n));
    free(vk_glsl_to_spirv(p->fi->msl, STAGE_FRAGMENT, "fs", &n));
    pthread_mutex_lock(&ready_lock);
    bool wanted = p->wanted || !p->src_saved;
    if (!wanted) {
        free(p->vi->msl);
        free(p->fi->msl);
        p->vi->msl = p->fi->msl = NULL;
        atomic_store(&p->state, 3);
    }
    pthread_mutex_unlock(&ready_lock);
    if (wanted) compile_program(p);
    prewarm_done();
}

// (translate thread, low list)
static void prewarm_translate(void *arg) {
    PrewarmReq *r = arg;
    pthread_mutex_lock(&progcache_lock);
    while (!progcache_ready) pthread_cond_wait(&progcache_cond, &progcache_lock);
    pthread_mutex_unlock(&progcache_lock);
    Prog *p = NULL;
    if (progcache_on) {
        ShaderKey vkey = {0}, fkey = {0};  // (as draw() makes them)
        vkey.glsl = fkey.glsl = 1;
        vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
        vkey.depth_minus_one = nvn_depth_mode() == 0;
        uint64_t chash[2];
        for (int i = 0; i < 2; i++) chash[i] = code_hash(r->code[i], r->cmax[i], i);
        uint64_t ck = prog_content_key(chash, &vkey, &fkey);
        plist_seen(r->code, r->cmax, chash);  // (the pipeline list's variants of this code: made on the render thread)
        pthread_mutex_lock(&progcache_lock);
        Prog **c = progcache_slot(ck, true);
        if (c && !*c) {
            p = calloc(1, sizeof *p);
            memcpy(p->code, r->code, sizeof p->code);
            memcpy(p->cmax, r->cmax, sizeof p->cmax);
            memcpy(p->chash, chash, sizeof chash);
            p->ck = ck;
            p->vkey = vkey;
            p->fkey = fkey;
            *c = p;
        }
        pthread_mutex_unlock(&progcache_lock);
    }
    free(r);
    if (!p) { atomic_fetch_add(&stat_pw_known, 1); atomic_fetch_add(&pw_total_known, 1); prewarm_done(); return; }
    atomic_fetch_add(&stat_pw_new, 1);
    atomic_fetch_add(&pw_total_new, 1);
    if (translate_core(p, true)) enqueue(&compile_q, true, prewarm_compile, p);
    else prewarm_done();
}

// (game thread) nvnProgramSetShaders
void backend_program_created(const NvnProgram *np) {
    static int on = -1;
    // (not on PowerVR: its shader compiler crashed building many pipelines at once, so a phone that played once closed at
    // every later start, the saved pipelines being built on the workers (issues #73, #99; the MH4U port saw the same).
    // Its one worker has the saved programs and pipelines to build first)
    if (on < 0) on = g_config.shader_prewarm && !getenv("MHGU_NO_PREWARM") && !sync_shaders() && !vk_gpu_powervr();
    if (!on || !(np->stage_mask & 1) || !(np->stage_mask & 2)) return;
    PrewarmReq *r = malloc(sizeof *r);
    for (int i = 0; i < 2; i++) {
        r->code[i] = np->stages[i].code_addr;
        r->cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
    }
    if (atomic_fetch_add(&stat_pw_left, 1) == 0 && !pw_t0) pw_t0 = plat_now_ns();
    enqueue(&translate_q, true, prewarm_translate, r);
}

static struct { uint64_t key; Prog *prog; } prog_tab[PROG_TAB];

static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    if (!(np->stage_mask & 1) || !(np->stage_mask & 2)) return NULL;
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2; i++) h = hash_words(h, &np->stages[i].code_addr, 8);
    h = hash_words(h, vkey, sizeof *vkey);
    h = hash_words(h, fkey->rt_kind, sizeof fkey->rt_kind);
    h = hash_words(h, &fkey->alpha_func, 1);
    if (fkey->alpha_coverage) h = hash_words(h, &fkey->alpha_coverage, 1);  // (MSAA variant)
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    while (prog_tab[slot].key && prog_tab[slot].key != h) slot = (slot + 1) & (PROG_TAB - 1);
    if (prog_tab[slot].key) return prog_tab[slot].prog;
    progcache_init();
    uint64_t code[2], cmax[2], chash[2] = {0, 0}, ck = 0;
    for (int i = 0; i < 2; i++) {
        code[i] = np->stages[i].code_addr;
        cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
        if (progcache_on) chash[i] = code_hash(code[i], cmax[i], i);
    }
    Prog *p = NULL;
    if (progcache_on) {
        ck = prog_content_key(chash, vkey, fkey);
        pthread_mutex_lock(&progcache_lock);
        Prog **c = progcache_slot(ck, true);
        Prog *q = c ? *c : NULL;
        if (c && !q) *c = p = calloc(1, sizeof *p);  // (an identical program at another address is found by its contents)
        pthread_mutex_unlock(&progcache_lock);
        if (q) {  // (translated in an earlier run or by the prewarm: dormant, building, or built)
            prog_tab[slot].key = h;
            prog_tab[slot].prog = q;
            stat_prog_cached++;
            prog_want(q);
            return q;
        }
    }
    if (!p) p = calloc(1, sizeof *p);
    prog_tab[slot].key = h;
    prog_tab[slot].prog = p;
    memcpy(p->code, code, sizeof code);
    memcpy(p->cmax, cmax, sizeof cmax);
    memcpy(p->chash, chash, sizeof chash);
    p->ck = ck;
    p->vkey = *vkey;
    p->fkey = *fkey;
    p->wanted = true;
    p->t_req = plat_now_ns();
    if (HITCH_ON) hitch_add(HC_PROG_NEW, 0, 0);
    run_translate(translate_program, p);
    return p;
}

// ================================================================== graphics pipelines
// Everything a pipeline bakes in (on the 1.2 baseline: all fixed-function state but viewport, scissor, depth bias,
// blend constants and stencil masks / reference), captured by value so it can be built on a worker thread.
#define ZERO_BINDING 16
typedef struct {
    Prog *prog;
    VkRenderPass rp;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint32_t write_mask[8];
    uint8_t blend_on, a2c, topology, poly_fill;
    uint8_t cull, front_ccw, prim_restart, depth_bias, depth_clamp;
    uint8_t depth_test, depth_write, depth_op, stencil_test;
    uint8_t sten_ops[2][4];  // per face: fail, pass, depth fail, compare
    uint8_t sample_shading;  // (MSAA) every sample shaded (exec.c pass_models)
} PipeKey;

typedef struct Pipe {
    PipeKey k;
    VkPipeline pipe;
    _Atomic int state;  // 0 building, 1 ready, 2 failed
    bool cold;          // (render thread) first made at a draw (pipe_record): no cache has it, the driver compiles it all
    uint64_t t_req;
    struct Pipe *next;  // (Prog.prewarm)
} Pipe;
#define PIPE_TAB 8192
static struct { uint64_t key; Pipe *pipe; } pipe_tab[PIPE_TAB];

static Pipe *pipe_slot(const PipeKey *k, bool *created) {
    uint64_t ph = hash_words(0xcbf29ce484222325ull, k, sizeof *k);
    if (!ph) ph = 1;
    uint32_t pslot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
    for (int n = 0; pipe_tab[pslot].key && pipe_tab[pslot].key != ph; n++, pslot = (pslot + 1) & (PIPE_TAB - 1))
        if (n == PIPE_TAB) return NULL;
    *created = !pipe_tab[pslot].pipe;
    if (*created) {
        Pipe *pp = calloc(1, sizeof *pp);
        pp->k = *k;
        pipe_tab[pslot].key = ph;
        pipe_tab[pslot].pipe = pp;
    }
    return pipe_tab[pslot].pipe;
}

// ---- pipeline cache (with the program cache): cache/pipelines.bin records each pipeline's state, its program by
// content key and its render pass by formats; the next run builds them as soon as their (cached) program is built.
// An area's first frame can need over a hundred new pipelines at once: more than the wait budget, even from the
// Vulkan pipeline cache.
typedef struct {
    uint32_t magic, pad;
    uint64_t ck;
    VkFormat color[8], depth;
    PipeKey k;  // (prog, rp: NULL)
} PipeRec;
#define PIPEREC_MAGIC 0x31504950u  // "PIP1"
#define PIPECACHE_PATH "cache/pipelines.bin"
static FILE *pipecache_f;

static void build_pipeline(void *arg);
static void prewarm_pipelines(Prog *p) {
    pthread_mutex_lock(&ready_lock);  // (the render thread adds to the list too: plist_attach)
    Pipe *q = p->prewarm;
    p->prewarm = NULL;
    pthread_mutex_unlock(&ready_lock);
    for (Pipe *next; q; q = next) {
        next = q->next;
        if (atomic_load(&p->state) == 1) run_job(build_pipeline, q);
        else set_state(&q->state, 2, 0);
    }
}
// (render thread) pp built once p is, or now if it is already
static void pipe_attach(Prog *p, Pipe *pp) {
    pthread_mutex_lock(&ready_lock);
    int s = atomic_load(&p->state);
    if (s != 1 && s != 2) pp->next = p->prewarm, p->prewarm = pp;
    pthread_mutex_unlock(&ready_lock);
    if (s == 1) run_job(build_pipeline, pp);
    else if (s == 2) set_state(&pp->state, 2, 0);
}
static void plist_export_add(const Prog *p, const PipeRec *r);

static void pipecache_init(const CacheHeader *hdr) {
    CacheHeader fh;
    int n = 0;
    long good = 0;
    FILE *f = fopen(PIPECACHE_PATH, "r+b");
    if (f && fread(&fh, sizeof fh, 1, f) == 1 && !memcmp(fh.magic, "MHGUPRG", 8) &&
        fh.stamp == hash_words(hdr->stamp, (uint64_t[]){sizeof(PipeRec)}, 8)) {
        good = (long)sizeof fh;
        PipeRec r;
        while (fread(&r, sizeof r, 1, f) == 1 && r.magic == PIPEREC_MAGIC && r.k.ncolor <= 8) {
            good = ftell(f);
            Prog **c = progcache_slot(r.ck, false);
            if (!c || !*c) continue;
            plist_export_add(*c, &r);
            r.k.prog = *c;
            r.k.rp = vk_render_pass(r.color, r.k.ncolor, r.depth, r.k.samples);
            bool created;
            Pipe *pp = pipe_slot(&r.k, &created);
            if (!pp || !created) continue;
            pp->next = (*c)->prewarm;
            (*c)->prewarm = pp;
            n++;
        }
        fseek(f, good, SEEK_SET);
        if (ftruncate(fileno(f), good)) {}
    } else {
        if (f) fclose(f);
        f = fopen(PIPECACHE_PATH, "wb");
        fh = *hdr;
        fh.stamp = hash_words(hdr->stamp, (uint64_t[]){sizeof(PipeRec)}, 8);
        if (f) fwrite(&fh, sizeof fh, 1, f);
    }
    pipecache_f = f;
    if (n) LOG("[vk] pipeline cache: %d pipelines to build\n", n);
}

static void pipecache_store(const Pipe *pp) {
    if (!pipecache_f || !pp->k.prog->ck) return;
    PipeRec r;
    memset(&r, 0, sizeof r);
    r.magic = PIPEREC_MAGIC;
    r.ck = pp->k.prog->ck;
    memcpy(r.color, pass_color, sizeof r.color);
    r.depth = pass_depth;
    r.k = pp->k;
    r.k.prog = NULL;
    r.k.rp = VK_NULL_HANDLE;
    fwrite(&r, sizeof r, 1, pipecache_f);
    fflush(pipecache_f);
}

// ---- shipped pipeline list: pipelines.list beside the executable, the pipelines recorded while playing the common
// areas, so a first run builds them in the background before an area needs them (cache/pipelines.bin only knows the
// ones this computer drew). Unlike pipelines.bin it outlives translator changes and other GPUs: a record names its
// program by the game's own shader code (code_hash) and shader keys, not by a translated program, and the formats
// the GPU decides are made again here (DEPTH24_STENCIL8's: D32S8 on AMD; vertex formats it lacks: widened). A record
// whose program is in programs.bin is attached at start-up like pipelines.bin's; the others wait until the prewarm
// meets their shader code (plist_seen, translate thread), then the render thread (plist_drain) makes the program
// variant and its pipelines. Recording: MHGU_PIPELIST_EXPORT=<file> writes the list (pipelines.bin's records and the
// shipped list's, without duplicates) at the first draw. MHGU_PIPELIST=<file> reads another list; MHGU_NO_PIPELIST=1:
// none.
typedef struct {
    uint32_t magic, pad;
    uint64_t chash[2];
    ShaderKey vkey, fkey;
    VkFormat color[8], depth;
    PipeKey k;  // (prog, rp: NULL)
} ListRec;
typedef struct { char magic[8]; uint32_t key_size, pipe_size, rec_size, count; } ListHeader;
#define LISTREC_MAGIC 0x314c5050u  // "PPL1"
static ListRec *plist;
static int plist_n, plist_left;
static uint8_t *plist_done;
static struct { uint64_t code[2], cmax[2], chash[2]; } plist_q[256];  // shader code the prewarm met (plist_seen)
static int plist_qn;
static _Atomic int plist_qpending;
static pthread_mutex_t plist_lock = PTHREAD_MUTEX_INITIALIZER;

// (translate thread, prewarm) the game made a program with this code: the render thread looks for list records
static void plist_seen(const uint64_t code[2], const uint64_t cmax[2], const uint64_t chash[2]) {
    if (!plist_left) return;
    bool any = false;
    for (int i = 0; i < plist_n && !any; i++)
        any = !plist_done[i] && plist[i].chash[0] == chash[0] && plist[i].chash[1] == chash[1];
    if (!any) return;
    for (;;) {
        pthread_mutex_lock(&plist_lock);
        if (plist_qn < (int)(sizeof plist_q / sizeof *plist_q)) {
            memcpy(plist_q[plist_qn].code, code, 16);
            memcpy(plist_q[plist_qn].cmax, cmax, 16);
            memcpy(plist_q[plist_qn].chash, chash, 16);
            plist_qn++;
            atomic_store(&plist_qpending, 1);
            pthread_mutex_unlock(&plist_lock);
            return;
        }
        pthread_mutex_unlock(&plist_lock);
        struct timespec ts = {0, 2000000};  // (full: the render thread empties it at its next command list)
        nanosleep(&ts, NULL);
    }
}

// (render thread) r's pipeline for program p, built once p is (as pipelines.bin's)
static int plist_attach(Prog *p, const ListRec *r) {
    PipeKey k = r->k;
    k.prog = p;
    for (int i = 0; i < 32; i++)
        if (k.attr_mask & (1u << i)) k.attr_fmt[i] = vertex_format_supported(k.attr_fmt[i]);
    VkFormat depth = r->depth;
    if (depth == VK_FORMAT_D24_UNORM_S8_UINT || depth == VK_FORMAT_D32_SFLOAT_S8_UINT) depth = vk_depth24_stencil8();
    k.rp = vk_render_pass(r->color, k.ncolor, depth, k.samples);
    bool created;
    Pipe *pp = pipe_slot(&k, &created);
    if (!pp || !created) return 0;
    pipe_attach(p, pp);
    return 1;
}

static void plist_load(void) {
    if (getenv("MHGU_NO_PIPELIST")) return;
    char path[1100];
    const char *e = getenv("MHGU_PIPELIST");
    if (e && *e) {
        snprintf(path, sizeof path, "%s", e);
    } else {
        char exe[1024];
        if (!plat_exe_path(exe, sizeof exe)) return;
        char *s = strrchr(exe, '/'), *bs = strrchr(exe, '\\');
        if (bs > s) s = bs;
        if (s) s[1] = 0; else exe[0] = 0;
        snprintf(path, sizeof path, "%spipelines.list", exe);
    }
    FILE *f = fopen(path, "rb");
    if (!f) return;
    ListHeader h;
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, "MHGUPL1", 8) || h.key_size != sizeof(ShaderKey) ||
        h.pipe_size != sizeof(PipeKey) || h.rec_size != sizeof(ListRec) || h.count > 1000000) {
        LOG("[vk] %s: not a pipeline list of this version, not used\n", path);
        fclose(f);
        return;
    }
    plist = calloc(h.count ? h.count : 1, sizeof *plist);
    plist_done = calloc(h.count ? h.count : 1, 1);
    plist_n = plist ? (int)fread(plist, sizeof *plist, h.count, f) : 0;
    fclose(f);
    int now = 0, built = 0;
    for (int i = 0; i < plist_n; i++) {
        if (plist[i].magic != LISTREC_MAGIC || plist[i].k.ncolor > 8) { plist_done[i] = 1; continue; }
        uint64_t ck = prog_content_key(plist[i].chash, &plist[i].vkey, &plist[i].fkey);
        Prog **c = progcache_slot(ck, false);
        if (!c || !*c) continue;  // (its program waits for the prewarm: plist_seen)
        plist_done[i] = 1;
        now++;
        built += plist_attach(*c, &plist[i]);
    }
    for (int i = 0; i < plist_n; i++) plist_left += !plist_done[i];
    LOG("[vk] pipeline list %s: %d pipelines, %d new to build now, %d when the prewarm meets their programs\n", path,
        plist_n, built, plist_left);
    (void)now;
}

// (render thread) the programs of the records whose shader code the prewarm met, then their pipelines
static void plist_drain(void) {
    if (!atomic_load(&plist_qpending)) return;
    pthread_mutex_lock(&plist_lock);
    int n = plist_qn;
    static struct { uint64_t code[2], cmax[2], chash[2]; } q[256];
    memcpy(q, plist_q, sizeof *q * (size_t)n);
    plist_qn = 0;
    atomic_store(&plist_qpending, 0);
    pthread_mutex_unlock(&plist_lock);
    for (int j = 0; j < n; j++)
        for (int i = 0; i < plist_n; i++) {
            ListRec *r = &plist[i];
            if (plist_done[i] || r->chash[0] != q[j].chash[0] || r->chash[1] != q[j].chash[1]) continue;
            plist_done[i] = 1;
            plist_left--;
            uint64_t ck = prog_content_key(r->chash, &r->vkey, &r->fkey);
            Prog *p = NULL, *have = NULL;
            pthread_mutex_lock(&progcache_lock);
            Prog **c = progcache_slot(ck, true);
            if (c) { have = *c; if (!have) *c = p = calloc(1, sizeof *p); }
            pthread_mutex_unlock(&progcache_lock);
            if (have) {
                prog_want(have);  // (a dormant one is loaded)
                plist_attach(have, r);
                continue;
            }
            if (!p) continue;
            memcpy(p->code, q[j].code, sizeof p->code);
            memcpy(p->cmax, q[j].cmax, sizeof p->cmax);
            memcpy(p->chash, r->chash, sizeof p->chash);
            p->ck = ck;
            p->vkey = r->vkey;
            p->fkey = r->fkey;
            p->wanted = true;
            plist_attach(p, r);  // (before it is translated: its pipelines follow it)
            run_translate(translate_program, p);
        }
    if (!plist_left) LOG("[vk] pipeline list: every program met\n");
}

// MHGU_PIPELIST_EXPORT: pipelines.bin's records (as pipecache_init reads them) and the shipped list's
static FILE *plist_ex;
static uint64_t *plist_ex_seen;
static int plist_ex_n, plist_ex_cap;
static void plist_export_rec(const ListRec *r) {
    uint64_t h = hash_words(0xcbf29ce484222325ull, r, sizeof *r);
    for (int i = 0; i < plist_ex_n; i++)
        if (plist_ex_seen[i] == h) return;
    if (plist_ex_n == plist_ex_cap) plist_ex_seen = realloc(plist_ex_seen, sizeof *plist_ex_seen * (size_t)(plist_ex_cap = plist_ex_cap ? plist_ex_cap * 2 : 4096));
    plist_ex_seen[plist_ex_n++] = h;
    fwrite(r, sizeof *r, 1, plist_ex);
}
static void plist_export_add(const Prog *p, const PipeRec *pr) {
    static int tried;
    if (!tried) {
        tried = 1;
        const char *e = getenv("MHGU_PIPELIST_EXPORT");
        if (e && *e && (plist_ex = fopen(e, "wb"))) {
            ListHeader h = {"MHGUPL1", sizeof(ShaderKey), sizeof(PipeKey), sizeof(ListRec), 0};
            fwrite(&h, sizeof h, 1, plist_ex);
        }
    }
    if (!plist_ex || !p) return;
    ListRec r;
    memset(&r, 0, sizeof r);
    r.magic = LISTREC_MAGIC;
    memcpy(r.chash, p->chash, sizeof r.chash);
    r.vkey = p->vkey;
    r.fkey = p->fkey;
    memcpy(r.color, pr->color, sizeof r.color);
    r.depth = pr->depth;
    r.k = pr->k;
    r.k.prog = NULL;
    r.k.rp = VK_NULL_HANDLE;
    plist_export_rec(&r);
}
static void plist_export_finish(void) {
    plist_export_add(NULL, NULL);  // (opens the file when pipelines.bin had nothing)
    if (!plist_ex) return;
    for (int i = 0; i < plist_n; i++)
        if (plist[i].magic == LISTREC_MAGIC) plist_export_rec(&plist[i]);
    ListHeader h = {"MHGUPL1", sizeof(ShaderKey), sizeof(PipeKey), sizeof(ListRec), (uint32_t)plist_ex_n};
    fseek(plist_ex, 0, SEEK_SET);
    fwrite(&h, sizeof h, 1, plist_ex);
    fclose(plist_ex);
    plist_ex = NULL;
    LOG("[vk] pipeline list exported: %s (%d pipelines)\n", getenv("MHGU_PIPELIST_EXPORT"), plist_ex_n);
}

static void build_pipeline(void *arg) {
    Pipe *pp = arg;
    uint64_t t0 = plat_now_ns();
    PipeKey *k = &pp->k;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = k->prog->vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = k->prog->fs, .pName = "main"}};
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = (VkPrimitiveTopology)k->topology, .primitiveRestartEnable = k->prim_restart};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE,
                                                 .cullMode = k->cull == 1 ? VK_CULL_MODE_FRONT_BIT : k->cull == 2 ? VK_CULL_MODE_BACK_BIT
                                                           : k->cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE,
                                                 .frontFace = k->front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
                                                 // (and the device's: a key from pipelines.list, of a run with another
                                                 // driver, may ask for it where it is off - the validation layer's error)
                                                 .depthClampEnable = k->depth_clamp && g_vk.depth_clamp,
                                                 .depthBiasEnable = k->depth_bias, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c,
                                               .sampleShadingEnable = k->sample_shading, .minSampleShading = 1};
    VkStencilOpState so[2];
    for (int f = 0; f < 2; f++)
        so[f] = (VkStencilOpState){(VkStencilOp)k->sten_ops[f][0], (VkStencilOp)k->sten_ops[f][1], (VkStencilOp)k->sten_ops[f][2],
                                   (VkCompareOp)k->sten_ops[f][3], 0xFF, 0xFF, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = k->depth_test, .depthWriteEnable = k->depth_write,
                                                .depthCompareOp = (VkCompareOp)k->depth_op, .stencilTestEnable = k->stencil_test,
                                                .front = so[0], .back = so[1]};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = k->prog->layout, .renderPass = k->rp};
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &pp->pipe);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline creation failed: %d\n", r);
    }
    atomic_fetch_add(&stat_compiles, 1);
    job_stat(2, t0);
    set_state(&pp->state, r == VK_SUCCESS ? 1 : 2, pp->t_req);
}

// ================================================================== feedback: sampling a texture that is an attachment
// (a feedback loop in Vulkan): the draw samples a copy instead, made when the attachment has changed since.
typedef struct { NvnTexture *key; TexBackend copy; bool valid; } Snap;
static Snap snaps[16];
static int nsnaps;
static NvnTexture *feedback_tex[4];
static VkImageView feedback_view[4];
static bool feedback_ro[4];  // (MHGU_PERF_RODEPTH) sampled where it is: the pass's depth target, read-only in the pass
static int nfeedback;

static NvnTexture *shader_texture_object(int stage, ShaderTexture *t) {
    int slot = ((int)t->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return ((NvnTexEntry *)st.tpool->entries)[tic].tex;
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

static void snapshots_invalidate(NvnTexture *t) {
    for (int i = 0; i < nsnaps; i++) if (!t || snaps[i].key == t) snaps[i].valid = false;
}

// (MHGU_PERF_RODEPTH) t, the pass's depth / stencil target, can be sampled where it is by the draw being set up: the draw
// writes neither depth nor stencil (so the pass can go on with the target read-only: draw / clear end such a pass for a
// write), and nothing else stands in for it (no MSAA image, no FSR stand-in, one level and layer)
static bool ro_depth_ok(NvnTexture *t) {
    if (!perf.rodepth || t != st.rt.depth || g_vk.msaa > 1 || pass_models || !renderer_format_is_depth(t->format)) return false;
    TexBackend *b = t->backend;
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || b->levels != 1 || b->layers != 1 || b->samples != 1 || b->ms)
        return false;
    if (g_fsr_on && (vk_fsr_map(t, true) != t || vk_fsr_map(t, false) != t)) return false;
    if (st.rt.has_dview && (st.rt.dview.setmask & 3) && (st.rt.dview.base_level || st.rt.dview.min_layer)) return false;
    return !(st.ds[0] && st.ds[1]) && !st.ds[3];  // (no depth write; no stencil test, whose operations may write)
}
// The draw being set up writes the depth / stencil target (it can't be drawn in a read-only pass)
static bool draw_writes_ds(void) { return (st.ds[0] && st.ds[1]) || st.ds[3]; }

static void resolve_feedback(Prog *pr) {
    nfeedback = 0;
    ro_want = false;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = draw_texture(sg, infos[sg], i);
            if (!is_attachment(t)) continue;
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
            if (have || nfeedback == 4) continue;
            feedback_ro[nfeedback] = false;
            if (ro_depth_ok(t)) {  // (MHGU_PERF_RODEPTH: no copy; the pass goes on, or begins again, with it read-only)
                feedback_tex[nfeedback] = t, feedback_view[nfeedback] = VK_NULL_HANDLE, feedback_ro[nfeedback++] = true;
                ro_want = true;
                if (!(in_pass && pass_ro_depth)) st.rt_dirty = true;
                continue;
            }
            TexBackend *src = vk_tex(g_fsr_on ? vk_fsr_map(t, false) : t, true);  // (FSR: an output-size stand-in)
            Snap *s = NULL;
            for (int k = 0; k < nsnaps; k++) if (snaps[k].key == t) s = &snaps[k];
            if (!s) {
                if (nsnaps == 16) { static int rr; s = &snaps[rr++ & 15]; } else s = &snaps[nsnaps++];
                if (s->key != t) s->valid = false;
                s->key = t;
            }
            TexBackend *d = &s->copy;
            if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format || d->samples != src->samples)) {
                vk_views_forget(d);
                vk_defer_image(d->image, d->mem);
                d->image = VK_NULL_HANDLE;
                s->valid = false;
            }
            VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
            if (!d->image) {
                VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                        .extent = {src->width, src->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                        .samples = (VkSampleCountFlagBits)src->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                        .usage = vk_usage_supported(src->format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)};
                VK_CHECK(vk_create_image(&ci, &d->image, &d->mem));
                d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
                d->samples = src->samples; d->aspect = src->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
                vk_end_pass();
                vk_image_init_layout(vk_cmd(), d->image, d->aspect);
            }
            bool sdepth = src->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
            if (!s->valid && sdepth && !depth_copy_ok(src->format)) {  // (a GPU that can't copy that depth format: left as it was)
                static int nlog;
                if (nlog++ < 4) LOG("[vk] depth copy %d (a texture drawn into while read) left out: the GPU can't do it\n", src->format);
                s->valid = true;
            }
            if (!s->valid) {
                vk_end_pass();
                vk_ms_resolve(src);
                VkCommandBuffer cb = vk_cmd();
                vk_full_barrier(cb);
                VkImageCopy rg = {.srcSubresource = {src->aspect, 0, 0, 1}, .dstSubresource = {src->aspect, 0, 0, 1},
                                  .extent = {src->width, src->height, 1}};
                vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
                if (sdepth) depth_copy_note("snapshot", (int)src->width, (int)src->height, (int)src->width, (int)src->height, src->format);
                s->valid = true;
                st.rt_dirty = true;
                stat_snapshots++;
            }
            static const VkComponentMapping id = {0};
            feedback_tex[nfeedback] = t;
            feedback_view[nfeedback++] = vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
        }
}

// ================================================================== resource binding
static VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size) {
    uint64_t max = g_vk.props.limits.maxUniformBufferRange;
    return (VkDescriptorBufferInfo){s.buf, s.off, size > max ? max : size};
}

static VkDescriptorBufferInfo zero_buffer(uint64_t size) {
    static const uint8_t zeros[0x10000];
    if (size > sizeof zeros) size = sizeof zeros;
    VkSpan s = vk_transient_copy(zeros, size, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, size};
}

// Per submission, copied once and shared by its draws (a shader never writes a uniform buffer), so their descriptor sets
// come out the same (stage_set reuses those): zeros, the identity resolution scale, the last alpha reference, each
// stage's last driver constants. A storage buffer of zeros stays the draw's own (a shader may write it).
static uint64_t shared_seq;
static VkSpan shared_zeros, shared_rscale, shared_aref;
static float shared_aref_v;
static bool shared_have_rscale, shared_have_aref;
static bool shared_on(void) {  // MHGU_VK_SHARED=0: a copy of its own for every draw, as before
    static int on = -1;
    if (on < 0) { const char *e = getenv("MHGU_VK_SHARED"); on = !e || atoi(e) != 0; }
    return on;
}
static void shared_new_submission(void) {
    uint64_t seq = vk_pending_seq();
    if (shared_seq == seq) return;
    shared_seq = seq;
    shared_zeros.buf = VK_NULL_HANDLE;
    shared_have_rscale = shared_have_aref = false;
}
static VkDescriptorBufferInfo zero_ubo(uint64_t size) {
    static const uint8_t zeros[0x10000];
    if (size > sizeof zeros) size = sizeof zeros;
    if (!shared_on()) return zero_buffer(size);
    shared_new_submission();
    if (!shared_zeros.buf) shared_zeros = vk_transient_copy(zeros, sizeof zeros, 256);
    return (VkDescriptorBufferInfo){shared_zeros.buf, shared_zeros.off, size};
}

// Constant buffer k of a stage: 0 the driver's (storage buffer table, compute group counts), 1 the compiler constants
// stored after the shader code, 3.. the game's bound uniform buffers. Copied into transient memory.
static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    if (k == 0 && perf.drvtab && stage != 5) {  // (MHGU_PERF_DRVTAB: the same bytes as below, the 2 KB zeroed only for a new copy)
        uint32_t tab[64];  // (the storage buffer table: 16 entries of {address, 0, size, 0})
        for (int i = 0; i < 16; i++)
            tab[4 * i] = (uint32_t)st.sbuf[stage][i].addr, tab[4 * i + 1] = 0, tab[4 * i + 2] = (uint32_t)st.sbuf[stage][i].size,
            tab[4 * i + 3] = 0;
        uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
        uint64_t size = si->cbuf_size[0] > 0x800 ? 0x800 : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        static struct { uint64_t seq, size; uint32_t tab[64]; VkSpan span; } last[2];
        bool share = (stage == 0 || stage == 1) && shared_on();
        uint32_t old[0x800 / 4];
        if (perf_check) {  // (as below)
            memset(old, 0, sizeof old);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &old[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
        }
        if (share) {
            shared_new_submission();
            if (last[stage].seq == shared_seq && last[stage].size == size && !memcmp(last[stage].tab, tab, 256)) {
                if (perf_check && (stat_check_n++, memcmp(last[stage].span.ptr, old, size))) check_bad("driver constants (shared)");
                return ubo_span(last[stage].span, size);
            }
        }
        uint32_t drv[0x800 / 4];  // (only the bytes copied zeroed; the table may lie past them, as below)
        memset(drv, 0, size);
        memcpy(&drv[base / 4], tab, 256);
        VkSpan sp = vk_transient_copy(drv, size, 256);
        if (perf_check && (stat_check_n++, memcmp(sp.ptr, old, size))) check_bad("driver constants");
        if (share) last[stage].seq = shared_seq, last[stage].size = size, memcpy(last[stage].tab, tab, 256), last[stage].span = sp;
        return ubo_span(sp, size);
    }
    if (k == 0) {
        uint32_t drv[0x800 / 4] = {0};
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                u[0] = (uint32_t)st.ubuf[5][i].addr; u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[2] = (uint32_t)st.sbuf[5][i].size;
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
        }
        uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (size < 0x500 && stage == 5) size = 0x500;
        if ((stage == 0 || stage == 1) && shared_on()) {  // (only the storage buffer table: most draws have the same one)
            static struct { uint64_t seq, size; uint32_t tab[64]; VkSpan span; } last[2];
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            shared_new_submission();
            if (last[stage].seq == shared_seq && last[stage].size == size && !memcmp(last[stage].tab, &drv[base / 4], 256))
                return ubo_span(last[stage].span, size);
            last[stage].seq = shared_seq, last[stage].size = size;
            memcpy(last[stage].tab, &drv[base / 4], 256);
            last[stage].span = vk_transient_copy(drv, size, 256);
            return ubo_span(last[stage].span, size);
        }
        return ubo_span(vk_transient_copy(drv, size, 256), size);
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        uint64_t size = ss->const_size ? ss->const_size : 16;
        if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
        if (!guest_range_ok(ss->code_addr + ss->const_offset, size)) return zero_ubo(size);
        return ubo_span(guest_copy(ss->code_addr + ss->const_offset, size), size);
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if (!r.addr) return zero_ubo(size);
    if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
    if (size > r.size && r.size) size = r.size;
    if (size < 16) size = 16;
    if (!guest_range_ok(r.addr, size)) return zero_ubo(size);
    if (stage == 0 && k == 3) ui_relax_scissor = false, ui_scissor_v = -1, ui_scissor_mid = false;
    if (stage == 0 && k == 3 && !st.ds[0] && (pass_h == g_render_size[1] || pass_sy != 1.0f)) {  // (FSR: stand-ins)
        // (touch controls: their HUD parts at 16:9 too. At 16:9 without them nothing moves but a part the player moved,
        // sized or hid (HudPlace.custom), and the touch layer is still told where the hunt's parts are and when a menu is
        // open: the lock-on's padlock and its menu check, the editors, touch.cpp)
        bool hud_moves = fabsf(g_render_aspect - 16.0f / 9.0f) > 0.01f || touch_visible();
        // (the parts' arrangement: an editor's while it shows one, else the touch controls' or the plain one)
        int prof = g_hud_edit_prof >= 0 ? g_hud_edit_prof : g_touch_hud ? HUD_TOUCH : HUD_PLAIN;
        g_hud_prof_now = prof;
        if (size >= 64) {
            float m[16];
            memcpy(m, G2H((uint32_t)r.addr), 64);
            float pos_x = m[12], pos_y = m[13], pos_z = m[14];

            // Ignore fullscreen overlays (e.g. screen fade / full backdrop)
            bool is_fullscreen = (fabsf(pos_x - 640.0f) < 1.0f && fabsf(pos_y - 360.0f) < 1.0f &&
                                  (fabsf(m[0]) >= 1200.0f || fabsf(m[5]) >= 700.0f)) ||
                                 (fabsf(pos_x) <= 1.0f && fabsf(pos_y) <= 1.0f);

            if (is_fullscreen && g_hudtrace_active)
                LOG("[hud] z %.1f pos %.1f,%.1f m %.2f %.2f %.2f %.2f: whole screen, kept\n", pos_z, pos_x, pos_y, m[0], m[1], m[4], m[5]);
            if (!is_fullscreen) {
                // The HUD keeps the 16:9 shape it is laid out in: on a wider picture its parts are narrowed (s) and moved
                // toward their side, on a narrower one (16:10) flattened (sy) and moved toward the top or bottom.
                float s = g_render_aspect > 16.0f / 9.0f ? (16.0f / 9.0f) / g_render_aspect : 1.0f;
                float sy = g_render_aspect < 16.0f / 9.0f ? g_render_aspect / (16.0f / 9.0f) : 1.0f;
                // Layer 110 is the loading screen's scrolling backdrop: strips at y 168 / 433 / 570 that must stay one picture.
                // The generic rule anchored each strip to a different edge and flattened it, leaving gaps between them.
                // They keep their height (as on a stretched 16:9 canvas), so nothing moves vertically.
                bool loading_strip = false;
                if (fabsf(pos_z - 110.0f) < 0.5f) {
                    sy = 1.0f;
                    // (only the strips: the lock-on markers and other effects use layer 110 too, mid-hunt, and made
                    // everything after them center-anchored, so the HUD jumped down)
                    loading_strip = fabsf(pos_y - 168.0f) < 0.3f || fabsf(pos_y - 433.0f) < 0.3f || fabsf(pos_y - 570.0f) < 0.3f;
                    if (loading_strip) ui_loading_pass = true;
                    static int dbg = -1;  // (MHGU_TOUCH_LOG: a strip taken mid-hunt would put the whole HUD at the game's places)
                    if (dbg < 0) dbg = getenv("MHGU_TOUCH_LOG") != NULL;
                    static uint64_t said;
                    if (dbg && loading_strip && plat_now_ns() - said > 2000000000ull)
                        LOG("[hud] a loading strip: z 110 at %.1f,%.1f m %.2f %.2f\n", pos_x, pos_y, m[0], m[5]), said = plat_now_ns();
                }
                // Front-end screens keep their backdrop 16:9: the HUD stays on it as with the 16:9 anchor. Edges / 21:9 / 32:9
                // took their centred menus (layer 100, x 640, anchored Right) and the help box (96) off it, to the sides.
                bool backdrop = fabsf(pos_z - 250.0f) < 0.5f && fabsf(pos_x - 640.0f) < 0.5f && fabsf(pos_y - 360.0f) < 0.5f;
                if (backdrop) {
                    ui_frontend_pass = true;
                    // (the front-end's screens: the title, the main menu, the character select and creation are menus
                    // too, driven with the d-pad and A / B; the touch controls' stick moves their cursor, and no menu
                    // pad goes over their lists)
                    ui_menu_drawn();
                    touch_hud_front();
                }
                // [Display] FillScreen: the backdrop itself covers the wider screen, in its shape (cut above and below)
                bool backdrop_fill = backdrop && g_config.fill_screen && s < 1.0f;
                bool backdrop_tall = backdrop && g_config.fill_screen && sy < 1.0f;  // (narrower: as high as the screen, cut at the sides)
                float anchor_aspect = ui_frontend_pass ? 16.0f / 9.0f : g_ui_anchor_aspect;  // (config.c: 16:9 .. the screen's)
                if (anchor_aspect > g_render_aspect) anchor_aspect = g_render_aspect;
                float a_ratio = anchor_aspect / g_render_aspect;
                float anchor_left = 640.0f * (1.0f - a_ratio);
                float anchor_right = 1280.0f - anchor_left;

                bool is_3d_overlay = (pos_z > 180.0f);
                float new_pos_x = pos_x, new_pos_y = pos_y;
                // the hunt's HUD (not the results, a loading screen, the title; under a menu of the game's, only at its
                // places: hud_accept): its parts (HP_*) where the player put them, the touch controls' arrangement by
                // default while they show; the item bar, the Hunter Arts and the target camera answer a finger
                // (runtime/touch.cpp), moved or not
                bool hunt_hud = !ui_results_pass && !ui_results_last && !ui_loading_pass && !ui_title_pass && !ui_meownster_pass;
                int part = -1;              // (HP_*)
                bool part_nearest = false;  // (a piece of the status in none of its parts' regions: the nearest's)
                bool part_touch = false;    // (a finger works it there: the item bar itself, not its ammo list)
                bool part_custom = false;   // (moved, sized or hidden by the player)
                float part_k = 1.0f;        // (its size)
                bool window_piece = false;  // (a menu window's, layer 97 / 98: hud_on_window)
                bool hud_hidden = false;    // (a part hidden, or on a menu's window: not drawn)
                bool meownster_mid = false; // (the Meownster Hunters' map and what is on it: kept in the middle 16:9)
                bool window_before = false; // (a left window of layer 100 drawn before the menu's own: hudtrace says so)
                hud_new_frame();
                bool grouped = false;
                int vgroup = 1;

                if (is_3d_overlay && g_hudtrace_active)
                    LOG("[hud] z %.1f pos %.1f,%.1f m %.2f %.2f %.2f %.2f: 3D overlay, not moved\n", pos_z, pos_x, pos_y, m[0], m[1], m[4], m[5]);
                if (!is_3d_overlay) {
                    enum { ANCHOR_LEFT, ANCHOR_RIGHT, ANCHOR_CENTER } anchor = ANCHOR_CENTER;
                    // vertically one anchor per layer (or per part of one), so a window drawn in pieces stays together
                    enum { V_TOP, V_BOTTOM, V_CENTER } vanchor = V_CENTER;

                    // Title screen: its layout (layer 108) is placed at the canvas centre and carries the copyright line at
                    // its bottom (y ~698), which the centre anchor moved up into "PRESS A" (layer 100, y 653 / 671, bottom
                    // anchor). Both scale about the centre, so they keep their spacing.
                    if (fabsf(pos_z - 108.0f) < 0.5f && fabsf(pos_x - 640.0f) < 0.5f && fabsf(pos_y - 360.0f) < 0.5f) ui_title_pass = true;
                    if (ui_title_pass && fabsf(pos_z - 100.0f) < 0.5f) {
                        anchor = ANCHOR_CENTER;
                        vanchor = V_CENTER;
                    } else if (ui_loading_pass && pos_z >= 94.0f && pos_z < 109.5f) {
                        // Loading screen: the quest panel's headers (layer 100), its text (101) and the title (105) are one
                        // picture. The generic rules moved them by different amounts (bottom, center, top) and the
                        // "Summary" header landed on the text. All of it scales about the center instead.
                        anchor = ANCHOR_CENTER;
                        vanchor = V_CENTER;
                    } else if (pos_z < 95.0f) {
                        // 94.0: Central notifications & banners ("Time limit: 50 min.")
                        // 60.0: the Guild Card's name plate (top right, under its page counter): by which third it is in
                        anchor = ANCHOR_CENTER;
                        vanchor = pos_y < 240.0f ? V_TOP : pos_y > 480.0f ? V_BOTTOM : V_CENTER;
                    } else if (fabsf(pos_z - 96.0f) < 0.5f) {
                        // 96.0: Dialogue & Help text box (bottom left), and the button guides of the village / Hub (top
                        // left and right)
                        anchor = pos_x >= 650.0f ? ANCHOR_RIGHT : ANCHOR_LEFT;
                        vanchor = pos_y < 360.0f ? V_TOP : V_BOTTOM;
                        // (the dialogue box keeps the middle 16:9: at 21:9 or Edges it went under the touch stick, #77, #101)
                        if (pos_y >= 360.0f && pos_x < 650.0f) anchor = ANCHOR_CENTER;
                    } else if (fabsf(pos_z - 97.0f) < 0.5f || fabsf(pos_z - 98.0f) < 0.5f) {
                        // 97.0: Menus and Submenus
                        // 98.0: text drawn over them (the Qty. label of the quantity box): it used the generic rule, which
                        // centered it 36 px below its top-anchored box
                        // Left window (Quest Info: x <= 430) -> Left anchor
                        // Submenu & Pause Menu (Combine, Items, Menu 1/2: x > 430) -> Right anchor
                        // The game lays its windows out for 16:9: they keep the middle 16:9 at any HUD anchor (split to
                        // the edges at 21:9 / Edges, they went under the touch controls: #77, #101); the HUD's parts
                        // on the other layers still go to the edges
                        anchor = ANCHOR_CENTER;
                        // The Quest Complete banner's lower border slides in from y 739: it belongs to the bottom edge.
                        vanchor = pos_y > 650.0f ? V_BOTTOM : V_TOP;
                        // The Quest Complete stamp is one picture, after its borders (x 640, by the top and bottom edges; the
                        // lower one slides in from y 739). Right / Left by place split it on a wide picture, its tilted
                        // "QUEST" (x -17) left of the rest.
                        if (fabsf(pos_z - 97.0f) < 0.5f && fabsf(pos_x - 640.0f) < 0.5f && (pos_y < 60.0f || pos_y > 680.0f))
                            ui_stamp_pass = true;
                        window_piece = !ui_stamp_pass;
                        if (fabsf(pos_z - 97.0f) < 0.5f) {
                            ui_menu_pass = true;
                            // (a menu of the game's is open: the touch controls' menu detection, the menu pad; not the
                            // field's Quest Complete banner, drawn on this layer too, nor the chat's log: a box at the
                            // bottom left, x 30..605, y 547..708 (y 323.. while the shoutouts are open beside it), which
                            // stays a few seconds after a line is sent or comes, and kept a menu believed open under the
                            // player. Measured on a Motorola g06, a village)
                            bool chat_log = pos_x < 620.0f && pos_y >= 540.0f;
                            if (!ui_stamp_pass && !chat_log)
                                ui_window_pass = true, ui_window_frame = g_present_count, ui_menu_drawn();
                        }
                        if (ui_stamp_pass) anchor = ANCHOR_CENTER;
                    } else if (fabsf(pos_z - 100.0f) < 0.5f) {
                        // (a window on this layer, the multiplayer menu or the hub's form: a stretched panel, which the
                        // HUD's parts here never are, in a hunt: the touch controls' menu detection)
                        if (fabsf(m[0]) > 2.0f && fabsf(m[5]) > 2.0f) ui_menu_drawn();
                        // 100.0: Hunter Arts HUD and bottom right prompts (y > 600) -> Bottom anchor
                        // Target cam selector (right edge, y 42..82: x > 1000, y < 360) -> Top anchor, so it stays by the minimap.
                        // (Windows centered on this layer, x 640, must stay one piece: they keep the Bottom anchor.)
                        anchor = ANCHOR_RIGHT;
                        // The black bar at the top of a cinematic (y 0, its twin at y 720 is Bottom) must stay at the top edge, or the scene shows above it.
                        // The left windows (Request Log info panel, quest card: x < 630, y < 560) are drawn here too, beside the Top-anchored menus
                        // of layer 97: one anchor for the whole window, or it splits. Without a menu in the pass (the results screen, whose
                        // labels on the left and numbers on the right are one table) they stay Bottom like the rest of the layer.
                        // Some come before the menu's windows in the frame (the smithy's parchment and its model, the quest
                        // counter's quest card: drawn under the windows beside them): a window drawn in this frame or the
                        // last two tells as well, or at 21:9 / Edges they went Right, over the window beside them
                        bool window_lately = ui_window_pass || (ui_window_frame && g_present_count - ui_window_frame <= 2);
                        vanchor = ((pos_x > 1000.0f && pos_y < 360.0f) || pos_y < 10.0f ||
                                   ((ui_menu_pass || window_lately) && pos_x < 630.0f && pos_y < 560.0f)) ? V_TOP : V_BOTTOM;
                        // and sideways those left windows keep the middle 16:9 with the menu's own (layer 97, since 0.1.8):
                        // Right, at 21:9 / Edges they slid over the window beside them (the canteen's dish and its Meal
                        // Effects over the Felyne skills, #167). Not a cinematic's bar, nor the Meownster screen's frame
                        if (window_lately && !ui_meownster_pass && pos_x < 630.0f && pos_y >= 10.0f && pos_y < 560.0f)
                            anchor = ANCHOR_CENTER, window_before = !ui_window_pass;
                        // Touch controls (runtime/touch.cpp): the hunt's Hunter Arts panels (x 726..842, y 560..720: up
                        // to three rows) to the top right, the target camera's icons (target_piece) left of them
                        // (not a stretched panel: the multiplayer menu's, the hub form's)
                        bool flat = !(fabsf(m[0]) > 2.0f && fabsf(m[5]) > 2.0f);
                        if (hunt_hud && flat && pos_x >= 700.0f && pos_x <= 900.0f && pos_y > 560.0f &&
                            hud_accept(HP_ARTS, pos_z, pos_x, pos_y)) {
                            part = HP_ARTS, part_touch = true;
                            if (prof == HUD_TOUCH) vanchor = V_TOP;
                        } else if (hunt_hud && target_piece(pos_x, pos_y) && hud_accept(HP_TARGET, pos_z, pos_x, pos_y)) {
                            part = HP_TARGET, part_touch = true;
                        }
                    } else if (fabsf(pos_z - 103.0f) < 0.5f) {
                        // 103.0: In-hunt Status HUD & Item Selector
                        // Status HUD (Clock, HP bar, Stamina, gauges, palico status: y < 500) -> Left anchor
                        // Item Selector wheel (y >= 500) -> Right anchor, and the ammo or coating list that opens above it
                        // (x 970..1242, y 392..464, L held), which must stay with its plates below
                        if (pos_y < 500.0f && !(pos_x > 900.0f && pos_y > 330.0f)) {
                            anchor = ANCHOR_LEFT;
                            vanchor = V_TOP;
                            // (its parts: the clock, health and stamina, the sharpness and the gauges, the plates; with
                            // the touch controls at the screen's left edge by default)
                            if (hunt_hud) {
                                int p = hud_status_part(pos_x, pos_y, &part_nearest);
                                if (hud_accept(p, pos_z, pos_x, pos_y)) part = p;
                            }
                        } else {
                            anchor = ANCHOR_RIGHT;
                            vanchor = V_BOTTOM;
                            bool bar = hunt_hud && hud_accept(HP_ITEMS, pos_z, pos_x, pos_y);
                            if (bar) part = HP_ITEMS, part_touch = pos_y >= 500.0f;
                            // the item bar is drawn through a scissor sized for the original layout; it cut the moved bar
                            // (and the ammo list above it)
                            ui_relax_scissor = s < 1.0f || sy < 1.0f || (bar && (prof == HUD_TOUCH || hud_custom(HP_ITEMS, prof)));
                            // Touch controls on the screen (runtime/touch.cpp): the bar goes to the bottom middle, out of
                            // the right thumb's buttons, where a tap on it uses the item and a swipe turns it
                            if (prof == HUD_TOUCH && bar) anchor = ANCHOR_CENTER;
                        }
                    } else if (fabsf(pos_z - 105.0f) < 0.5f) {
                        // 105.0: Minimap & Target Cam
                        anchor = ANCHOR_RIGHT;
                        vanchor = V_TOP;
                        // the minimap: its picture (one piece at 1114, 225 in both of its zooms; not drawn under the hunt's
                        // menus) and the icons over it, in its box. Under a menu its icons, which move, go with the map
                        // while its picture is at its place
                        static uint64_t map_frame;
                        if (hunt_hud && pos_x >= 975.0f && pos_x <= 1255.0f && pos_y >= 92.0f && pos_y <= 365.0f) {
                            bool picture = fabsf(pos_x - 1114.0f) < 3.0f && fabsf(pos_y - 225.0f) < 3.0f;
                            if (picture ? hud_accept(HP_MAP, pos_z, pos_x, pos_y) : !ui_menu_open || map_frame == g_present_count) {
                                part = HP_MAP;
                                if (picture) map_frame = g_present_count;
                            }
                        }
                    } else {
                        // General fallback for other layers
                        if (pos_x <= 430.0f) anchor = ANCHOR_LEFT;
                        else if (pos_x >= 650.0f) anchor = ANCHOR_RIGHT;
                        else anchor = ANCHOR_CENTER;
                        vanchor = pos_y < 240.0f ? V_TOP : pos_y > 480.0f ? V_BOTTOM : V_CENTER;
                        // 99.0: the results screen's rule between its rows (y 266) is drawn here, beside rows of layer 100 that sit
                        // Bottom: no center band, or the line cuts through the numbers.
                        if (fabsf(pos_z - 99.0f) < 0.5f && vanchor == V_CENTER) vanchor = V_BOTTOM;
                    }

                    // Quest results screen: its header and page counter at the top, the Time Left bar at the bottom (layer
                    // 99, its ends at x 80 / 1180, y 643: what marks the screen), and in the middle the Quest Complete
                    // stamp, the reward table, the item lists' parchment (101), its lines (99) and rows (100). The layers'
                    // rules sent those different ways (the rows top, the parchment centre: the first row sat above the
                    // parchment, the header went down). The bar is drawn after the stamp, so the last pass also tells.
                    if (fabsf(pos_z - 99.0f) < 0.5f && fabsf(pos_y - 643.0f) < 0.5f && (fabsf(pos_x - 80.0f) < 0.5f || fabsf(pos_x - 1180.0f) < 0.5f))
                        ui_results_pass = true;
                    // Sideways the middle is one table, centred (layer 100's Right took it right of the stamp it sits on, with
                    // Edges / 21:9), and the header and page counter go to their side.
                    if ((ui_results_pass || ui_results_last) && pos_z > 98.5f && pos_z < 101.5f) {
                        vanchor = pos_y < 60.0f ? V_TOP : pos_y > 600.0f ? V_BOTTOM : V_CENTER;
                        if (vanchor == V_CENTER) anchor = ANCHOR_CENTER;
                        else if (vanchor == V_TOP) anchor = pos_x <= 430.0f ? ANCHOR_LEFT : pos_x >= 650.0f ? ANCHOR_RIGHT : ANCHOR_CENTER;
                    }
                    // Meownster Hunters return screen: a map in a black frame at the top (the frame on layers 105 and 100,
                    // its corner at 215, 0; the map 105; its icons 102 / 103, bubbles 98), the Palicoes' tokens and the
                    // reward list over it (100), the Reward List header and the prompts at the bottom. Layer 100 went to the
                    // bottom with its rule: the tokens sat too low on the map, the lead Palico below the frame, and the
                    // frame's lower edge slid away from it. All of it but the header and prompts (below y 600) goes top.
                    // Sideways it is one picture too, in the middle 16:9 (the launch screen as well, with its range circles on
                    // the map): by layer the frame and the map went Right and the circles and icons Left or by their place, so
                    // at 21:9 and Edges the circles slid off the map, out of its frame, cut by the map's scissor left where the
                    // game laid it out (#143). Its scissor narrows about the middle with it (ui_scissor_mid)
                    if (fabsf(pos_z - 105.0f) < 0.5f && pos_x == 215.0f && pos_y == 0.0f) ui_meownster_pass = true;
                    if (ui_meownster_pass && pos_z > 97.5f && pos_z < 105.5f) {
                        vanchor = pos_y > 600.0f ? V_BOTTOM : V_TOP;
                        if (pos_y <= 600.0f) anchor = ANCHOR_CENTER, meownster_mid = true;
                    }
                    // The loading backdrop's tiles (x -30, 549, 1128...: one every 579, past 1280) are one picture sideways
                    // too, from the screen's left edge: by place they went Left / Centre / Right, three copies with gaps
                    // between them with Edges / 21:9; centred (or Center), nothing covered the left of the first tile.
                    if (loading_strip) {
                        new_pos_x = pos_x * s;
                    } else if (anchor == ANCHOR_LEFT) {
                        new_pos_x = anchor_left + pos_x * s;
                    } else if (anchor == ANCHOR_RIGHT) {
                        new_pos_x = anchor_right - (1280.0f - pos_x) * s;
                    } else {
                        new_pos_x = 640.0f + (pos_x - 640.0f) * s;
                    }
                    int v = vanchor == V_TOP ? 0 : vanchor == V_BOTTOM ? 2 : 1;
                    ui_recent[ui_nrecent % UI_RECENT].x = pos_x, ui_recent[ui_nrecent % UI_RECENT].y = pos_y;
                    ui_recent[ui_nrecent % UI_RECENT].vanchor = (uint8_t)v;
                    ui_recent[ui_nrecent % UI_RECENT].hanchor = (uint8_t)anchor;
                    ui_nrecent++;
                    if (vanchor == V_TOP) new_pos_y = pos_y * sy;
                    else if (vanchor == V_BOTTOM) new_pos_y = 720.0f - (720.0f - pos_y) * sy;
                    else new_pos_y = 360.0f + (pos_y - 360.0f) * sy;
                    if (part >= 0) {  // a part: where the player has it (runtime/hud_parts.h)
                        // (the Hunter Arts' top row, which the touch arrangement takes to the top: its first piece's y,
                        // last frame's or this one's so far, whichever is higher; two rows by default)
                        static uint64_t arts_frame;
                        static float arts_top = HUD_ARTS_TOP_GAME, arts_top_now = 1e9f;
                        if (part == HP_ARTS) {
                            if (g_present_count != arts_frame) {
                                if (arts_top_now < 1e9f) arts_top = arts_top_now;
                                arts_top_now = 1e9f, arts_frame = g_present_count;
                            }
                            if (pos_y < arts_top_now) arts_top_now = pos_y;
                        }
                        HudPlace hp;
                        hud_place_begin(&hp, part, prof, fminf(arts_top, arts_top_now));
                        if (part == HP_ARTS) g_hud_arts_top = fminf(arts_top, arts_top_now);
                        part_custom = hp.custom;
                        if (hud_moves || part_custom) {  // (at 16:9 without the touch controls, left alone: the game's)
                            hud_place_piece(&hp, pos_x, pos_y, &new_pos_x, &new_pos_y);
                            g_hud_shift_dx[part] = hp.shift_x, g_hud_shift_dy[part] = hp.shift_y;
                            part_k = hp.k, hud_hidden = hp.hidden;
                        }
                    }
                    if (!hud_moves && !part_custom) new_pos_x = pos_x, new_pos_y = pos_y;  // (told only: drawn where the game put it)
                    if (window_piece) hud_window_piece(new_pos_x, new_pos_y);
                    {  // (a part moved from the game's place: where it went, and not drawn while it lies on a menu's window)
                        // (the touch arrangement moves all but the minimap)
                        int p = part >= 0 && ((prof == HUD_TOUCH && part != HP_MAP) || part_custom) ? part : -1;
                        if (p >= 0) {
                            const float *pp = HUD_PAD[p];
                            float pad[4] = {pp[0] * s * part_k, pp[1] * sy * part_k, pp[2] * s * part_k, pp[3] * sy * part_k};
                            hud_part_piece(p, new_pos_x, new_pos_y);
                            if (hud_on_window(p, pad)) hud_hidden = true;
                        }
                    }
#ifndef __ANDROID__
                    // (the menus' layers: the mouse's hit tests map the pointer back to the game's canvas through where
                    // their pieces went, runtime/hle/mousemenu.c)
                    ui_piece = -1;
                    if (pos_z > 93.5f && pos_z < 101.5f) {
                        bool moved = hud_moves || part_custom;
                        float kx = moved ? s * part_k : 1.0f, ky = moved ? sy * part_k : 1.0f;
                        ui_piece = mousemenu_piece(pos_x, pos_y, pos_z, new_pos_x, new_pos_y, kx, ky);
                    }
#endif
                    // (where each part's pieces went, its size, whether a finger works it, hidden: runtime/touch.cpp)
                    if (part >= 0) touch_hud_piece(part, pos_x, pos_y, new_pos_x, new_pos_y, part_k, (part_touch ? 1 : 0) | (hud_hidden ? 2 : 0));
                    // One anchor per window (ui_group_end), on a picture narrower than 16:9. Not for the hunt's status HUD
                    // and item bar (103), the minimap (105), the loading screen, or the bars that belong to an edge (a
                    // cinematic's at y 0 / 720, the Quest Complete border sliding in from y 739).
                    grouped = sy < 1.0f && !(ui_loading_pass && pos_z >= 94.0f && pos_z < 109.5f) && fabsf(pos_z - 103.0f) >= 0.5f &&
                              fabsf(pos_z - 105.0f) >= 0.5f && pos_y >= 10.0f && pos_y <= 712.0f &&
                              !(part >= 0 && ((prof == HUD_TOUCH && (part == HP_ARTS || part == HP_TARGET)) || part_custom));
                    vgroup = v;
                    if (g_hudtrace_active)
                        LOG("[hud] z %.1f pos %.1f,%.1f m %.2f %.2f %.2f %.2f -> %c%c %.1f,%.1f%s%s sc %d,%d %dx%d%s%s%s%s\n", pos_z, pos_x, pos_y, m[0], m[1],
                            m[4], m[5], "LRC"[anchor], "TBC"[vanchor], new_pos_x, new_pos_y, ui_menu_pass ? " menu" : "",
                            window_before ? " (before the menu's windows)" : "", st.scissor[0][0],
                            st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], part >= 0 ? " part " : "", part >= 0 ? HUD_KEY[part] : "",
                            part >= 0 && part_nearest ? " (nearest)" : "", hud_hidden ? " hidden" : "");
                }
                if (!hud_moves && !part_custom) {  // (the touch layer told; the piece, its scissor and its matrix stay the game's)
                    ui_relax_scissor = false;
                    return ubo_span(guest_copy(r.addr, size), size);
                }

                if (backdrop_fill) {
                    float k = 1.0f / s;
                    m[1] *= k; m[5] *= k; m[9] *= k;
                } else if (backdrop_tall) {
                    float k = 1.0f / sy;
                    m[0] *= k; m[4] *= k; m[8] *= k;
                } else {
                    m[0] *= s; m[4] *= s; m[8] *= s;
                }
                if (!backdrop_tall) { m[1] *= sy; m[5] *= sy; m[9] *= sy; }
                if (part_k != 1.0f) { m[0] *= part_k; m[1] *= part_k; m[4] *= part_k; m[5] *= part_k; m[8] *= part_k; m[9] *= part_k; }
                if (hud_hidden) m[0] = m[1] = m[4] = m[5] = m[8] = m[9] = 0.0f;  // (no area: nothing drawn)
                m[12] = new_pos_x;
                m[13] = new_pos_y;

                VkSpan sp = vk_transient_copy(G2H((uint32_t)r.addr), size, 256);  // (all of it, the matrix changed)
                memcpy(sp.ptr, m, 64);
                if (grouped) ui_group_add((float *)sp.ptr, pos_z, pos_x, pos_y, vgroup);
                // The game clips some windows (a list, the help windows' body) with a scissor laid out for the 16:9 window:
                // it moves with the piece, as its group was anchored last frame (the group is not complete yet), or for a
                // group not seen before as the pieces so far are.
                if (sy < 1.0f && !is_3d_overlay) {
                    int gv = !grouped ? -1 : ui_group.predict >= 0 ? ui_group.predict : ui_group_vanchor();
                    ui_scissor_v = gv >= 0 ? gv : vgroup;
                }
                if (s < 1.0f && meownster_mid) ui_scissor_mid = true;
                return ubo_span(sp, size);
            }
        }
    }
    return ubo_span(guest_copy(r.addr, size), size);
}

static int32_t fsr_cap_base;  // (FSR capture draw: its FSR_CAP_BASE_BINDING value, stage_set)

static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out) {
    ShaderTexture *stx = &si->tex[i];
    int slot = ((int)stx->handle_offset - 0x20) / 8;  // 64-bit handles from offset 0x20
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler smp = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
    if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        if (e->tex) {
            view = vk_view(g_fsr_on ? vk_fsr_map(e->tex, false) : e->tex, &e->view, e->has_view, false, stx->type);  // (FSR)
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k] && (stx->type == TEXTYPE_2D || stx->type == TEXTYPE_2D_ARRAY)) {
                    if (stx->type == TEXTYPE_2D_ARRAY) view = vk_dummy_view(TEXTYPE_2D_ARRAY);  // (as a copy's has always been)
                    else if (!feedback_ro[k]) view = feedback_view[k];
                    else if (pass_ro_depth && view) layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;  // (the target
                    else {                                                             // itself, in the pass's layout for it)
                        static int nlog;
                        if (nlog++ < 4) LOG("[vk] read-only depth: the pass is not read-only, a black texture instead\n");
                        view = vk_dummy_view(TEXTYPE_2D);
                    }
                }
        }
    }
    if (h && st.spool && tsc < (uint32_t)st.spool->count) smp = vk_sampler(&((NvnSamplerDesc *)st.spool->entries)[tsc]);
    if (!view) view = vk_dummy_view(stx->type);
    if (!smp) smp = vk_default_sampler(true);
    *out = (VkDescriptorImageInfo){smp, view, layout};
}

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
// A set already written in this submission with the same contents is used again: allocating and writing one is driver
// work on every draw (most of the render thread's time inside the driver, on a Mali and an Adreno alike), and draws in a
// row often bind the same textures and buffers. The key is the layout and every write (binding, type, buffer range or
// sampler and view), compared in full. MHGU_VK_SETREUSE=0: a new set for every draw, as before.
#define SET_CACHE 2048
typedef struct { VkDescriptorSetLayout layout; uint32_t binding, type; uint64_t a, b, c; } SetWrite;
typedef struct { uint64_t seq, hash; VkDescriptorSetLayout layout; VkDescriptorSet ds; int n; SetWrite *w; } SetEnt;
static SetEnt set_cache[SET_CACHE];
static SetWrite *set_store;  // (each submission's writes, for the full compare: SET_CACHE * MAX_BIND, reused)
uint64_t stat_set_reused, stat_set_new;
static bool set_reuse_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("MHGU_VK_SETREUSE"); on = !e || atoi(e) != 0; }
    return on;
}

// dyn_mask (MHGU_PERF_DYNUBO): the uniform buffers the layout has with a dynamic offset (bit: binding), written at offset 0
// with their offsets in the ring put in dyn_off, in binding order (vkCmdBindDescriptorSets' order within a set)
static VkDescriptorSet stage_set(int nbinds, const VkBindingInfo *binds, VkDescriptorSetLayout layout, uint32_t set, int stage,
                                 ShaderInfo *si, uint32_t gx, uint32_t gy, uint32_t gz, uint32_t dyn_mask, uint32_t *dyn_off) {
    VkDescriptorSet ds = VK_NULL_HANDLE;
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    int n = 0;
    for (int i = 0; i < nbinds; i++) {
        const VkBindingInfo *b = &binds[i];
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            if (ti < si->ntex) texture_info(stage, si, ti, &ii[n]);
            else ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL};
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding < 17) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            w[n].pBufferInfo = &bi[n];
            if (dyn_mask >> b->binding & 1) {
                dyn_off[__builtin_popcount(dyn_mask & ((1u << b->binding) - 1))] = (uint32_t)bi[n].offset;
                bi[n].offset = 0;
                w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            }
        } else if (b->binding == FSR_CAP_OUT_BINDING && b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {  // (FSR capture)
            bi[n] = vk_fsr_cap_buffer();
            w[n].pBufferInfo = &bi[n];
        } else if (b->binding == FSR_CAP_BASE_BINDING && b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            int32_t base[4] = {fsr_cap_base, 0, 0, 0};
            bi[n] = ubo_span(vk_transient_copy(base, sizeof base, 256), sizeof base);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding == 18) {  // rscale: no scaling here
            float rs[67][4];
            shared_new_submission();
            if (!shared_have_rscale || !shared_on()) {
                for (int k = 0; k < 67; k++) rs[k][0] = rs[k][1] = 1, rs[k][2] = rs[k][3] = 0;
                memset(rs[66], 0xFF, 4);  // [66].x: the front-face mask's all-ones word (translate.c)
                shared_rscale = vk_transient_copy(rs, sizeof rs, 256), shared_have_rscale = true;
            }
            bi[n] = ubo_span(shared_rscale, sizeof rs);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding == ALPHA_REF_BUFFER_INDEX) {
            const float aref = a2c_as_test ? 0.5f : st.alpha_ref;
            float ref[4] = {aref, 0, 0, 0};
            shared_new_submission();
            if (!shared_have_aref || !shared_on() || memcmp(&shared_aref_v, &aref, sizeof shared_aref_v))
                shared_aref = vk_transient_copy(ref, sizeof ref, 256), shared_aref_v = aref, shared_have_aref = true;
            bi[n] = ubo_span(shared_aref, sizeof ref);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {  // tint (19), gmap (20): zeros
            bi[n] = zero_ubo(0x200);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {  // global memory: not available (zeros)
            // (a shader that never writes it, uses_gmem_store: the shared zeros, as a uniform buffer's)
            bi[n] = si && !si->uses_gmem_store ? zero_ubo(0x1000) : zero_buffer(0x1000);
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;
        }
        n++;
    }
    // (the same set again? Not with a storage buffer the shader writes: that one is the draw's own)
    bool reuse = set_reuse_on() && n <= MAX_BIND;
    SetWrite key[MAX_BIND];
    uint64_t h = (uint64_t)(uintptr_t)layout * 0x9E3779B97F4A7C15ull ^ (uint64_t)n;
    for (int i = 0; reuse && i < n; i++) {
        SetWrite *k = &key[i];
        k->layout = layout, k->binding = w[i].dstBinding, k->type = (uint32_t)w[i].descriptorType;
        if (w[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && (!si || si->uses_gmem_store)) { reuse = false; break; }
        if (w[i].pImageInfo) k->a = (uint64_t)(uintptr_t)w[i].pImageInfo->sampler, k->b = (uint64_t)(uintptr_t)w[i].pImageInfo->imageView,
                             k->c = (uint64_t)w[i].pImageInfo->imageLayout;
        else k->a = (uint64_t)(uintptr_t)w[i].pBufferInfo->buffer, k->b = w[i].pBufferInfo->offset, k->c = w[i].pBufferInfo->range;
        uint64_t v[4] = {(uint64_t)k->binding << 32 | k->type, k->a, k->b, k->c};
        for (int j = 0; j < 4; j++) h = (h ^ v[j]) * 0xC2B2AE3D27D4EB4Full, h ^= h >> 29;
    }
    uint64_t seq = vk_pending_seq();
    SetEnt *e = NULL;
    if (reuse) {
        if (!set_store) set_store = calloc((size_t)SET_CACHE * MAX_BIND, sizeof *set_store);
        e = &set_cache[(h ^ (h >> 32)) & (SET_CACHE - 1)];
        if (e->seq == seq && e->hash == h && e->layout == layout && e->n == n && !memcmp(e->w, key, (size_t)n * sizeof *key)) {
            stat_set_reused++;
            return e->ds;
        }
    }
    ds = vk_alloc_set(layout);
    for (int i = 0; i < n; i++) w[i].dstSet = ds;
    if (n) vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    stat_set_new++;
    if (e) {  // (the slot's last set, of this submission or an older one, is replaced: it stays valid where it was bound)
        e->seq = seq, e->hash = h, e->layout = layout, e->ds = ds, e->n = n;
        e->w = set_store + (e - set_cache) * MAX_BIND;
        memcpy(e->w, key, (size_t)n * sizeof *key);
    }
    return ds;
}

// ================================================================== compute
typedef struct { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; } CsProg;

static void build_compute(void *arg) {
    CsProg *cp = arg;
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(cp->si->msl, STAGE_COMPUTE, "cs", &n);
    if (!spv) { set_state(&cp->p.state, 2, 0); return; }
    cp->p.nbinds = vk_reflect(spv, n, cp->p.binds, MAX_BIND);
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    cp->p.layout = make_layout(&cp->p, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec = {3, me, 12, cp->local};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                .pSpecializationInfo = &spec},
                                      .layout = cp->p.layout};
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &cp->pipe);
    atomic_fetch_add(&stat_compiles, 1);
    set_state(&cp->p.state, r == VK_SUCCESS ? 1 : 2, 0);
    if (r != VK_SUCCESS) LOG("[compute] pipeline failed: %d\n", r);
}

static struct { uint64_t addr; CsProg *cp; } g_cs_tab[256];
static int g_cs_n;

static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *s5 = &np->stages[5];
    if (!s5->code_addr) return NULL;
    for (int i = 0; i < g_cs_n; i++) if (g_cs_tab[i].addr == s5->code_addr) return g_cs_tab[i].cp;
    CsProg *cp = calloc(1, sizeof *cp);
    if (g_cs_n < 256) { g_cs_tab[g_cs_n].addr = s5->code_addr; g_cs_tab[g_cs_n++].cp = cp; }
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    ShaderKey k = {0};
    k.glsl = 1;
    uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
    pthread_mutex_lock(&translate_lock);
    cp->si = maxwell_translate((const uint8_t *)G2H((uint32_t)s5->code_addr), s5->const_offset ? s5->const_offset : 0x10000, STAGE_COMPUTE, &k);
    pthread_mutex_unlock(&translate_lock);
    if (HITCH_ON) hitch_add(HC_CS_NEW, plat_now_ns() - t0, 0);  // (the translate thread's translation waited for too)
    cp->p.vi = cp->si;
    if (!cp->si->msl) {
        LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, (unsigned long long)s5->code_addr);
        set_state(&cp->p.state, 2, 0);
        return cp;
    }
    if (cp->si->uses_gmem) LOG("[compute] %llx uses global memory (not available: zeros)\n", (unsigned long long)s5->code_addr);
    for (int i = 0; i < 3; i++) {
        cp->local[i] = s5->control ? *(const uint32_t *)(s5->control + 0x748 + 4 * i) : 1;
        if (!cp->local[i]) cp->local[i] = 1;
    }
    save_glsl(cp->si->msl, s5->code_addr, "cs");
    run_job(build_compute, cp);
    return cp;
}

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    if (g_hudtrace_active) LOG("[hud] dispatch %u x %u x %u\n", gx, gy, gz);  // (debug, "hudtrace")
    snapshots_invalidate(NULL);
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = compute_program(st.program);
    if (!cp) { stat_skipped++; return; }
    // (cached: after its first run its SPIR-V and pipeline come from the caches)
    if (!await_build(&cp->p.state, cp, 0, true)) { skip_draw(&stat_skip_pipe); return; }
    vk_end_pass();
    st.rt_dirty = true;
    ms_resolve_sampled(5, cp->si);
    if (fill.dst) fill_sampled_stage(5, cp->si);
    fill_copy_flush();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    nfeedback = 0;
    VkDescriptorSet ds = stage_set(cp->p.nbinds, cp->p.binds, cp->p.set_layout[0], 0, 5, cp->si, gx, gy, gz, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, gx, gy, gz);
    stat_dispatches++;
}

// ================================================================== draws
// The largest of count 16- or 32-bit indices, the primitive restart index aside
static uint32_t index_max(const void *src, uint32_t isz, uint32_t count, uint32_t restart) {
    uint32_t m = 0;
    if (isz == 2) { const uint16_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > m && s[i] != (uint16_t)restart) m = s[i]; }
    else { const uint32_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > m && s[i] != restart) m = s[i]; }
    return m;
}

// Quads / fans / polygons as a 32-bit triangle index list.
static uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first, uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    return out;
}

static bool topology(uint32_t mode, VkPrimitiveTopology *t) {
    switch (mode) {
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;
    default: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return false;
    }
}

// The fragment code's content hash: the same in every run and area (its address is not).
static uint64_t prog_fs_hash(Prog *p) {
    if (!p->chash[1]) p->chash[1] = code_hash(p->code[1], p->cmax[1], 1);
    return p->chash[1];
}

// FXAA 3.11 (FXAAMode = 3_11): the game's FXAA pass draws shaders.c "fxaa.frag" instead, from the same input into the
// same target. False: not possible, the game's own pass draws.
static VkDescriptorSetLayout fxaa_set_layout;
static VkPipelineLayout fxaa_layout;

static VkPipeline fxaa_pipeline(VkRenderPass rp) {
    static struct { VkRenderPass rp; VkPipeline p; } tab[8];
    static int n;
    for (int i = 0; i < n; i++) if (tab[i].rp == rp) return tab[i].p;
    if (!fxaa_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &fxaa_set_layout));
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &fxaa_set_layout};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &fxaa_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("resolve.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("fxaa.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
                                            .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState att = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &att};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
                                           .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vs, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = fxaa_layout, .renderPass = rp};
    VkPipeline p = VK_NULL_HANDLE;
    if (sync_pipeline(&pi, &p) != VK_SUCCESS) LOG("[vk] FXAA pipeline failed\n");
    if (n < 8) tab[n].rp = rp, tab[n++].p = p;
    return p;
}

static bool fxaa_311(NvnTexture *src) {
    NvnTexture *dt = st.rt.color[0];
    TexBackend *s = vk_tex(src, false), *d = vk_tex(dt, true);
    if (s->samples != 1 || d->samples != 1) return false;
    vk_end_pass();
    st.rt_dirty = true;
    vk_ms_resolve(s);
    vk_ms_resolve(d);
    bool has = st.rt.has_cview[0];
    VkImageView sv = vk_view(src, NULL, false, false, TEXTYPE_2D), dv = vk_view(dt, &st.rt.cview[0], has, true, TEXTYPE_2D);
    VkFormat f = has && (st.rt.cview[0].setmask & 4) && vk_format(dt->format) == d->format ? vk_format(st.rt.cview[0].format)
                                                                                           : d->format;
    VkRenderPass rp = vk_render_pass(&f, 1, VK_FORMAT_UNDEFINED, 1);
    VkPipeline p = sv && dv ? fxaa_pipeline(rp) : VK_NULL_HANDLE;
    if (!p) return false;
    d->ms_valid = false;
    renderer_mark_gpu_owned(dt);
    uint32_t lv = has && (st.rt.cview[0].setmask & 1) ? st.rt.cview[0].base_level : 0;
    uint32_t w = level_extent(d->width, lv), h = level_extent(d->height, lv);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkDescriptorSet set = vk_alloc_set(fxaa_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), sv, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    VkRect2D area = {{0, 0}, {w, h}};
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                .framebuffer = vk_framebuffer(rp, &dv, 1, w, h), .renderArea = area};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaa_layout, 0, 1, &set, 0, NULL);
    ss.known &= ~SS_SETS;  // (not draw's sets any more)
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &area);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    ss_forget();
    stat_draws++;
    return true;
}

// FXAA off (settings): the game's FXAA pass (recognised by its shader) copies its input instead; FXAAMode = 3_11 draws
// FXAA 3.11 there (fxaa_311).
#define FXAA_FS_HASH 0x8530ded68f4407dfull
static void copy_texture(const TexRef *src, const TexRef *dst, uint64_t flags);
static bool fxaa_skipped(Prog *pr) {
    if (prog_fs_hash(pr) == FXAA_FS_HASH) aa_pass_frame = g_present_count;  // (menus: full_picture)
    bool fxaa_311_on = config_live_fxaa() && config_live_fxaa_mode() == FXAA_311;
    if ((config_live_fxaa() && !fxaa_311_on) || prog_fs_hash(pr) != FXAA_FS_HASH || pr->fi->ntex != 1 || !st.rt.ncolor ||
        !st.rt.color[0])
        return false;
    NvnTexture *src = shader_texture_object(1, &pr->fi->tex[0]);
    if (!src || src == st.rt.color[0]) return false;
    if (fxaa_311_on) return fxaa_311(src);
    TexRef s = {.tex = (uint64_t)(uintptr_t)src}, d = {.tex = (uint64_t)(uintptr_t)st.rt.color[0]};
    if (st.rt.has_cview[0]) d.view = st.rt.cview[0], d.has_view = 1;
    // (MHGU_PERF_FXAA_COPYDRAW: the whole picture, one level, the same size and format as its input: drawn in the next pass)
    TexBackend *sb = src->backend, *db = st.rt.color[0]->backend;
    bool whole = perf.fxaa_copydraw && sb && db && !(d.has_view && (d.view.setmask & 3) && (d.view.base_level || d.view.min_layer)) &&
                 !g_fsr_on && sb->width == db->width && sb->height == db->height && sb->format == db->format && sb->levels == 1 &&
                 db->levels == 1 && sb->layers == 1 && db->layers == 1 && !sb->mip_skip && !db->mip_skip;
    if (whole) {
        vk_end_pass();
        st.rt_dirty = true;
        vk_ms_resolve(sb);
        vk_ms_resolve(db);
        db->ms_valid = false;
        renderer_mark_gpu_owned(st.rt.color[0]);
        snapshots_invalidate(NULL);
    }
    if (!whole || !vk_color_fill_later(db, sb, 1)) {
        copy_texture(&s, &d, 0);
        if (whole) vk_color_fill_check(db, sb, 1);
    }
    stat_draws++;
    return true;
}

// [Performance] LowEnd: what a weak GPU is spared. On a Mali more than half of a hunt's frame goes to some 30 full-screen
// passes, and each costs about 0.15 ms whatever its size, so a lower resolution alone doesn't get rid of them.
// The bloom and depth of field chain goes: from the picture's copy at half size to the last pass at a size below the
// picture's, and the two draws that add its results to the picture. Its targets are marked as the GPU's: never drawn,
// they would be uploaded from the guest's memory when sampled, and that garbage over the picture is coloured noise.
static NvnTexture *lowend_seen[32];  // (debug: fs_new) the targets skipped this frame
static uint64_t lowend_seen_frame;
static int lowend_nseen;
static void lowend_note_skipped(NvnTexture *t) {
    if (lowend_seen_frame != g_present_count) lowend_seen_frame = g_present_count, lowend_nseen = 0;
    for (int i = 0; i < lowend_nseen; i++)
        if (lowend_seen[i] == t) return;
    if (lowend_nseen < 32) lowend_seen[lowend_nseen++] = t;
}
static bool lowend_was_skipped(NvnTexture *t) {
    if (lowend_seen_frame != g_present_count) return false;
    for (int i = 0; i < lowend_nseen; i++)
        if (lowend_seen[i] == t) return true;
    return false;
}
#define BLOOM_FIRST_FS_HASH 0x6be2f1217dae4cbfull  // the picture to half size
#define BLOOM_ADD_FS_HASH 0x414c7689cdfe5a68ull    // one result into the picture
#define BLOOM_GLOW_FS_HASH 0xb505f15693063946ull   // the other, at the start of the HUD
static bool bloom_open;
static uint64_t bloom_frame;

static bool small_target(void) {
    NvnTexture *t = st.rt.ncolor ? st.rt.color[0] : NULL;
    return t && t->width < g_render_size[0];
}

// [Performance] Bloom Off (Low-end mode sets it so): the bloom and depth of field chain is left out. Not with FSR's
// upscaling, whose start of the HUD is found from these post passes (fsr.c); Low-end mode's plain scaling (g_fsr_on
// too, without g_config.fsr) has always gone with it.
static bool lowend_skipped(Prog *pr) {
    if (config_live_bloom() || g_config.fsr) { bloom_open = false; return false; }
    uint64_t h = prog_fs_hash(pr);
    if (h == BLOOM_FIRST_FS_HASH) bloom_open = true, bloom_frame = g_present_count;
    if (bloom_open && (bloom_frame != g_present_count || !small_target())) bloom_open = false;
    if (bloom_open) {
        renderer_mark_gpu_owned(st.rt.color[0]);
        lowend_note_skipped(st.rt.color[0]);
    }
    return bloom_open || h == BLOOM_ADD_FS_HASH || h == BLOOM_GLOW_FS_HASH;
}

// (debug) MHGU_FS_NEW=1: a fragment program the first time it draws, what it reads and where it draws, so one that
// only draws now and then (a hit's effect) stands out; a low-end frame's draw that reads a target it skipped, too.
// MHGU_FS_WATCH=<hash>: when that program draws, the next frames as pictures and a dump of every pass.
static void fs_new(Prog *pr, uint32_t count) {
    static int on = -1;
    if (on < 0) on = getenv("MHGU_FS_NEW") != NULL;
    if (!on) return;
    extern bool mousecam_active(void);
    extern volatile int g_dump_request;
    static uint64_t seen[2048], in_hunt;
    static int nseen;
    uint64_t h = prog_fs_hash(pr), now = plat_now_ns();
    static uint64_t watch, watch_next;
    static int watch_left = 4;
    if (!watch && getenv("MHGU_FS_WATCH")) watch = strtoull(getenv("MHGU_FS_WATCH"), NULL, 16);
    if (h == watch && watch_left && now >= watch_next) {  // (its frames, and every pass of the next one)
        extern char g_burst_name[200];
        extern int g_burst_left, g_burst_index;
        watch_left--, watch_next = now + 3000000000ull;
        snprintf(g_burst_name, sizeof g_burst_name, "hit%d", watch_left);
        g_burst_index = 0, g_burst_left = 10, g_dump_request = 1;
        LOG("[fsnew] frame %llu: watched program, %d more\n", (unsigned long long)g_present_count, watch_left);
    }
    if (!mousecam_active()) in_hunt = 0;
    else if (!in_hunt) in_hunt = now;
    bool reads_skipped = false;
    char list[220];
    int n = 0;
    list[0] = 0;
    for (int i = 0; i < pr->fi->ntex && n < 180; i++) {
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        bool skipped = lowend_was_skipped(t);
        reads_skipped |= skipped;
        n += snprintf(list + n, sizeof list - (size_t)n, " %ux%u%s", t ? t->width : 0, t ? t->height : 0, skipped ? "(skipped)" : "");
    }
    NvnTexture *to = st.rt.ncolor ? st.rt.color[0] : NULL;
    if (reads_skipped) {
        static uint64_t said[64];
        static int nsaid;
        bool known = false;
        for (int i = 0; i < nsaid; i++) known |= said[i] == h;
        if (!known && nsaid < 64) {
            said[nsaid++] = h;
            LOG("[lowend] fs %016llx (%u vertices, into %ux%u) reads what was skipped:%s\n", (unsigned long long)h, count,
                to ? to->width : 0, to ? to->height : 0, list);
        }
    }
    for (int i = 0; i < nseen; i++)
        if (seen[i] == h) return;
    if (nseen == 2048) return;
    seen[nseen++] = h;
    LOG("[fsnew] frame %llu fs %016llx %u vertices, into %ux%u, reads%s%s\n", (unsigned long long)g_present_count, (unsigned long long)h,
        count, to ? to->width : 0, to ? to->height : 0, list, in_hunt ? " (in a hunt)" : "");
}

// A movie frame: the game draws the decoder's NV12 planes (hle/movie.c), an R8 picture and its half-size RG8 colour.
// Its picture (NULL: not a movie frame).
static NvnTexture *movie_picture(Prog *pr) {
    NvnTexture *y = NULL, *uv = NULL;
    for (int i = 0; i < pr->fi->ntex; i++) {
        NvnTexture *t = draw_texture(1, pr->fi, i);
        if (t && t->format == 0x1) y = t;        // R8
        else if (t && t->format == 0xd) uv = t;  // RG8
    }
    if (!y || !uv || uv->width * 2 != y->width || uv->height * 2 != y->height || !y->height) return NULL;
    // The game writes each frame into one of a few texture pairs it takes turns with. Until a texture was seen to
    // change, its contents were checked only every 8th frame (textures.c upload_texture): for the first second of a
    // movie, some frames showed a pair's old contents, black or a dark Y under the new colour (black flickers).
    renderer_mark_dynamic(y);
    renderer_mark_dynamic(uv);
    return y;
}

// A movie frame keeps the movie's shape. The game draws it over the whole target, which on a picture of another shape
// (16:10, 21:9) stretched it: drawn instead in the largest rectangle of the movie's shape, centred, the pass's clear
// showing as bars.
static bool movie_viewport(NvnTexture *y, const float *vp, float out[4]) {
    float pw, ph;
    pass_guest_size(&pw, &ph);
    if (vp[0] != 0 || vp[1] != 0 || vp[2] != pw || vp[3] != ph || !pass_h) return false;
    float movie = (float)y->width / (float)y->height, screen = vp[2] / vp[3];
    if (fabsf(movie / screen - 1.0f) < 0.01f) return false;
    float w = vp[2], h = vp[3];
    if (screen > movie && g_config.fill_screen) h = w / movie;  // ([Display] FillScreen: as wide as the screen, cut above and below)
    else if (screen > movie) w = h * movie;
    else h = w / movie;
    out[0] = (vp[2] - w) * 0.5f, out[1] = (vp[3] - h) * 0.5f, out[2] = w, out[3] = h;
    return true;
}

// [Performance] Shadows Off: nothing drawn into the shadow maps (a pass with only a depth target, not the 3D picture's
// size: the game's is one 1024x3072 atlas, three cascades, the first pass of a hunt's frame - 47 draws, 2.8 ms of a
// Motorola g06's 18.6). The game clears it with three full-size draws, not a clear (2.3 ms of that pass on its own; all
// skipped, the whole scene was in shadow). So the atlas is cleared once to the far depth here, and then nothing more is
// drawn into it: its pass never even begins. Every lookup finds nothing in the way: no shadow anywhere.
static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask);
// Which atlases are cleared is kept in each one's own TexBackend (shadow_off_gen, shadow_off_image), so every atlas the game
// makes is cleared once, however many: a list of the first 8 kept the 9th and later - new ones after some areas or a
// quest, the village again - uncleared, their garbage putting the whole scene in shadow, dark (#165, as #88, #105). A new
// atlas has a new TexBackend (0: not cleared), even where its NvnTexture's memory, or its image's handle, was another's.
// And the same atlas again when its image was made anew. Shadows turned On then Off again: all of them cleared again.
static bool shadow_skipped(void) {
    static uint32_t off_gen = 1;
    static bool was_on = true;
    if (config_live_shadows()) {
        was_on = true;
        return false;
    }
    if (was_on) was_on = false, off_gen++;
    if (st.rt.ncolor) return false;
    NvnTexture *d = st.rt.depth;
    if (!d || (d->width < 512 && d->height < 512) || (d->width == g_render_size[0] && d->height == g_render_size[1])) return false;
    TexBackend *b = d->backend;
    if (!b || b->shadow_off_gen != off_gen || b->shadow_off_image != b->image) {
        bool relaxed = ui_relax_scissor;  // (the whole atlas, not the cascade the scissor is set for)
        ui_relax_scissor = true;
        clear(-1, 0, NULL, 0, true, 1.0f, false, 0, 0);  // (makes its image, the first time)
        ui_relax_scissor = relaxed;
        if ((b = d->backend)) {
            b->shadow_off_gen = off_gen, b->shadow_off_image = b->image;
            LOG("[vk] shadows off: the %ux%u shadow atlas cleared once, its draws left out\n", d->width, d->height);
        }
    }
    renderer_mark_gpu_owned(d);  // (not to be uploaded from the guest's memory when sampled)
    return true;
}

// ---- (MHGU_PERF_SHADOW_CLEAR) Shadows On: the atlas's pass reads all of last frame's atlas back in as it begins (12.6 MB),
// though the game then writes all of it again: each cascade's casters come after a draw that writes one depth over the
// whole cascade (a quad, depth test Always with write: most of the Moto g06's 2.8 ms pass, with that read). Once that is
// seen on the GPU, the pass begins cleared to that depth instead (=1): an atlas's first frame begins it filled with a
// marker depth, and each such draw's rectangle is read back after it - every texel the same value, never the marker, and
// the rectangles together the whole atlas. Then only in a frame whose commands, read ahead from the pass's first draw,
// have those draws again (the same program over the same rectangles, in order), and each is compared again as it comes:
// its vertices, indices, constants and state those of the draw that was checked. One that differs turns it off for that
// atlas (logged). =2 also leaves such a draw out where nothing was drawn over its rectangle since the pass began cleared:
// it would write that value over itself.
enum { AT_NONE, AT_VERIFY, AT_WAIT, AT_OK, AT_OFF };
#define AT_SLOTS 4
#define AT_MARKER 0x3f7fffffu  // (the float just under 1.0: what an atlas being checked begins filled with)
static struct {
    TexBackend *b;  // the atlas this is about (its image: a new one is checked again)
    VkImage image;
    int state, nslots;
    int32_t rect[AT_SLOTS][4];  // the clearing draws' rectangles (x, y, w, h), in order
    uint64_t sig[AT_SLOTS], prog[AT_SLOTS];  // their signatures (at_sig), vertex programs
    uint64_t part[AT_SLOTS][6];               // (the signature's pieces: which one differs, in the log)
    uint32_t value;  // the depth they write (its bits)
    // (AT_VERIFY, AT_WAIT) where the rectangles are read back, and the submission that copies them there
    VkBuffer buf;
    VmaAllocation mem;
    void *ptr;
    uint64_t cap, off, seq, slot_off[AT_SLOTS];
    int pending;  // (AT_VERIFY) a clearing draw being set up: its slot (-1 none); one that doesn't draw spoils the check
    bool spoiled;
    int tries;    // checks spoiled so (a pipeline still building): tried again in a later frame, 8 times at most
    // the frame's pass
    uint64_t frame;
    bool open;   // it began cleared to the value (AT_OK) or filled with the marker (AT_VERIFY)
    int next;    // the clearing draws met in it
    bool dirty_all;          // (=2) drawn into since it began: all of it, or the 32x32 tiles marked (at_mark)
    uint32_t dirty[128][4];  // (a row of tiles a row here: 128 x 128 tiles, an atlas up to 4096 x 4096)
} at = {.pending = -1};

static float f_of(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static TexBackend *at_target(void) {  // the pass's target is a shadow atlas this can do (as shadow_skipped tells one)
    if (!perf.shadow_clear || st.rt.ncolor || !st.rt.depth || g_shadow_scissor_mode || !config_live_shadows()) return NULL;
    NvnTexture *d = st.rt.depth;
    if ((d->width < 512 && d->height < 512) || (d->width == g_render_size[0] && d->height == g_render_size[1])) return NULL;
    if (st.rt.has_dview && (st.rt.dview.setmask & 3) && (st.rt.dview.base_level || st.rt.dview.min_layer)) return NULL;
    TexBackend *b = d->backend;
    if (!b || !b->image || b->format != VK_FORMAT_D32_SFLOAT || b->samples != 1 || b->ms || b->levels != 1 || b->layers != 1 ||
        !depth_copy_ok(b->format))
        return NULL;
    return b;
}

// The draw's rectangle: its viewport, whole texels inside the target and inside its scissor. false: not such a rectangle
static bool at_draw_rect(const TexBackend *b, int32_t r[4]) {
    const float *vp = st.viewport[0];
    const int32_t *sc = st.scissor[0];
    int32_t x = (int32_t)vp[0], y = (int32_t)vp[1], w = (int32_t)vp[2], h = (int32_t)vp[3];
    if ((float)x != vp[0] || (float)y != vp[1] || (float)w != vp[2] || (float)h != vp[3] || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        (uint32_t)(x + w) > b->width || (uint32_t)(y + h) > b->height)
        return false;
    if (sc[0] > x || sc[1] > y || (int64_t)sc[0] + sc[2] < (int64_t)x + w || (int64_t)sc[1] + sc[3] < (int64_t)y + h) return false;
    r[0] = x, r[1] = y, r[2] = w, r[3] = h;
    return true;
}

// Where any draw can write: its viewport within its scissor, rounded out
static void at_bounds(int32_t r[4]) {
    const float *vp = st.viewport[0];
    const int32_t *sc = st.scissor[0];
    double x0 = fmax(floor(vp[0]), sc[0]), y0 = fmax(floor(vp[1]), sc[1]);
    double x1 = fmin(ceil((double)vp[0] + vp[2]), (double)sc[0] + sc[2]), y1 = fmin(ceil((double)vp[1] + vp[3]), (double)sc[1] + sc[3]);
    x0 = fmax(x0, 0), y0 = fmax(y0, 0);
    if (x1 > 65536) x1 = 65536;
    if (y1 > 65536) y1 = 65536;
    r[0] = (int32_t)x0, r[1] = (int32_t)y0, r[2] = x1 > x0 ? (int32_t)(x1 - x0) : 0, r[3] = y1 > y0 ? (int32_t)(y1 - y0) : 0;
}

static bool rects_meet(const int32_t a[4], const int32_t b[4]) {
    return a[2] > 0 && a[3] > 0 && b[2] > 0 && b[3] > 0 && a[0] < b[0] + b[2] && b[0] < a[0] + a[2] && a[1] < b[1] + b[3] &&
           b[1] < a[1] + a[3];
}

// A draw that writes one depth over a rectangle whatever was there: depth test Always with write, no alpha test or coverage,
// no discard, depth written or texture read, a few vertices, one instance (the atlas has no stencil: its test does nothing)
static bool at_clear_like(const TexBackend *b, const Prog *pr, const CmdRec *r, bool indexed, int32_t rect[4]) {
    if (!r || !pr || !(st.ds[0] && st.ds[1]) || compare_fn(st.ds[2]) != VK_COMPARE_OP_ALWAYS) return false;
    if (st.ms[0xC] || (st.color_state[2] >= 1 && st.color_state[2] <= 7)) return false;
    if (pr->fi->uses_discard || pr->fi->writes_depth || pr->vi->ntex || pr->fi->ntex || pr->vi->uses_gmem || pr->fi->uses_gmem)
        return false;
    uint32_t inst = (uint32_t)(indexed ? r->x[6] : r->x[4]);
    if ((uint32_t)r->x[2] > 6 || inst > 1) return false;
    return at_draw_rect(b, rect);
}

// What decides what such a draw writes: its programs, vertices (and indices), constants and state. 0: can't tell.
// part (when given): the same in AT_PARTS pieces - state, indices, vertices, storage buffer table, compiler constants,
// uniform buffers - so a log can say which one differs
#define AT_PARTS 6
static uint64_t at_sig(const CmdRec *r, bool indexed, const Prog *pr, uint64_t *part) {
    uint64_t q[AT_PARTS] = {0};
    uint64_t h = hash_words(0x5ad0, &st.program->stages[0].code_addr, 8);
    h = hash_words(h, &st.program->stages[1].code_addr, 8);
    h = hash_words(h, &r->op, 4), h = hash_words(h, r->x, sizeof r->x);
    h = hash_words(h, st.viewport[0], 16), h = hash_words(h, st.scissor[0], 16), h = hash_words(h, st.depth_range[0], 8);
    h = hash_words(h, st.ds, 8), h = hash_words(h, st.poly, 4), h = hash_words(h, st.poly_offset, sizeof st.poly_offset);
    h = hash_words(h, &st.depth_clamp, sizeof st.depth_clamp), h = hash_words(h, st.color_state, 4), h = hash_words(h, st.ms, 0x18);
    h = hash_words(h, &st.alpha_ref, sizeof st.alpha_ref);
    h = hash_words(h, &st.prim_restart, sizeof st.prim_restart), h = hash_words(h, &st.restart_index, 4);
    h = hash_words(h, &st.nattrib, sizeof st.nattrib), h = hash_words(h, st.attrib, sizeof st.attrib);
    h = hash_words(h, &st.nstream, sizeof st.nstream), h = hash_words(h, st.stream, sizeof st.stream);
    q[0] = h;
    uint32_t count = (uint32_t)r->x[2];
    uint64_t lo, hi;  // (the vertices read)
    if (!indexed) {
        lo = (uint32_t)r->x[1], hi = lo + count;
    } else {
        uint32_t itype = (uint32_t)r->x[1], isz = itype == 0 ? 1 : itype == 1 ? 2 : 4;
        if (!count || count > 64 || !guest_range_ok(r->x[3], (uint64_t)count * isz)) return 0;
        const uint8_t *ip = G2H((uint32_t)r->x[3]);
        h = hash_words(h, ip, (size_t)count * isz);
        q[1] = hash_words(0, ip, (size_t)count * isz);
        uint32_t mn = UINT32_MAX, mx = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = isz == 1 ? ip[i] : isz == 2 ? ((const uint16_t *)ip)[i] : ((const uint32_t *)ip)[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        int64_t base = (int32_t)r->x[4];
        if ((int64_t)mn + base < 0) return 0;
        lo = (uint64_t)((int64_t)mn + base), hi = (uint64_t)((int64_t)mx + base) + 1;
    }
    // (each vertex's bytes, its stride's: every attribute is read inside it; a stream without a stride, its one element)
    for (int i = 0; i < st.nattrib; i++) {
        uint32_t a = st.attrib[i], fmt = a & 0xFF, s = (a >> 8) & 0x1F, off = a >> 13;
        if (!(pr->vi->attr_mask & (1u << i)) || !fmt || s >= 16 || s >= (uint32_t)st.nstream) continue;
        uint64_t stride = st.stream[s][0], from = stride ? stride * lo : off, to = stride ? stride * hi : off + 16;
        if (stride && st.stream[s][1]) return 0;  // (read per instance: not such a draw)
        Range v = st.vbuf[s];
        if (v.size && to > v.size) to = v.size;
        if (!v.addr || to <= from || !guest_range_ok(v.addr + from, to - from)) return 0;
        h = hash_words(h, G2H((uint32_t)(v.addr + from)), (size_t)(to - from));
        q[2] = hash_words(q[2], G2H((uint32_t)(v.addr + from)), (size_t)(to - from));
    }
    const ShaderInfo *si[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++) {
        const NvnShaderStage *ss = &st.program->stages[sg];
        if (si[sg]->cbuf_mask & 1) h = hash_words(h, st.sbuf[sg], sizeof st.sbuf[sg]), q[3] = hash_words(q[3], st.sbuf[sg], sizeof st.sbuf[sg]);
        if (si[sg]->cbuf_mask & 2) {
            uint64_t size = ss->const_size ? ss->const_size : 16;
            if (size < si[sg]->cbuf_size[1]) size = si[sg]->cbuf_size[1];
            if (guest_range_ok(ss->code_addr + ss->const_offset, size)) {
                h = hash_words(h, G2H((uint32_t)(ss->code_addr + ss->const_offset)), size);
                q[4] = hash_words(q[4], G2H((uint32_t)(ss->code_addr + ss->const_offset)), size);
            }
        }
        for (int k = 3; k < 21; k++) {
            if (!(si[sg]->cbuf_mask & (1u << k))) continue;
            Range u = st.ubuf[sg][k - 3];
            uint64_t size = si[sg]->cbuf_size[k] ? si[sg]->cbuf_size[k] : 256;
            if ((si[sg]->cbuf_dynamic & (1u << k)) && u.size) size = u.size > 0x10000 ? 0x10000 : u.size;
            if (size > u.size && u.size) size = u.size;
            h = hash_words(h, &k, sizeof k), q[5] = hash_words(q[5], &k, sizeof k);
            if (u.addr && guest_range_ok(u.addr, size))
                h = hash_words(h, G2H((uint32_t)u.addr), size), q[5] = hash_words(q[5], G2H((uint32_t)u.addr), size);
        }
    }
    if (part) memcpy(part, q, sizeof q);
    return h ? h : 1;
}

static void at_free(void) {  // (once the submissions that may copy into it are done)
    if (!at.buf) return;
    vk_defer_buffer(at.buf, at.mem);
    at.buf = VK_NULL_HANDLE, at.ptr = NULL, at.cap = 0;
}

static void at_reset(TexBackend *b) {
    at_free();
    memset(&at, 0, sizeof at);
    at.pending = -1;
    at.b = b, at.image = b->image;
}

// (AT_WAIT, its copies done) every texel of each rectangle the same value, not the marker, and the rectangles the atlas
static void at_check(void) {
    vmaInvalidateAllocation(g_vk.vma, at.mem, 0, VK_WHOLE_SIZE);
    const uint32_t *p = at.ptr;
    uint32_t v = p[0];
    uint64_t area = 0, bad = UINT64_MAX;
    bool ok = at.nslots > 0 && v != AT_MARKER;
    for (int s = 0; ok && s < at.nslots; s++) {
        const uint32_t *q = p + at.slot_off[s] / 4;
        uint64_t n = (uint64_t)at.rect[s][2] * (uint32_t)at.rect[s][3];
        for (uint64_t i = 0; i < n && bad == UINT64_MAX; i++)
            if (q[i] != v) bad = i;
        if (bad != UINT64_MAX) {
            LOG("[perf] shadow atlas %ux%u: clearing draw %d left texel (%llu, %llu) of its rectangle %d,%d %dx%d at %08x, not %08x: "
                "the atlas is read in as before\n", at.b->width, at.b->height, s, (unsigned long long)(bad % (uint32_t)at.rect[s][2]),
                (unsigned long long)(bad / (uint32_t)at.rect[s][2]), at.rect[s][0], at.rect[s][1], at.rect[s][2], at.rect[s][3], q[bad], v);
            ok = false;
        }
        area += n;
        for (int t = 0; t < s; t++) ok = ok && !rects_meet(at.rect[s], at.rect[t]);
    }
    if (ok && area != (uint64_t)at.b->width * at.b->height) {
        LOG("[perf] shadow atlas %ux%u: its %d clearing draws cover %llu of its %llu texels: the atlas is read in as before\n",
            at.b->width, at.b->height, at.nslots, (unsigned long long)area, (unsigned long long)at.b->width * at.b->height);
        ok = false;
    }
    at_free();
    at.state = ok ? AT_OK : AT_OFF;
    at.value = v;
    if (ok)
        LOG("[perf] shadow atlas %ux%u: its %d clearing draws write %g (%08x) over all of it: its pass begins cleared to that%s\n",
            at.b->width, at.b->height, at.nslots, f_of(v), v, perf.shadow_clear >= 2 ? ", and they are left out where it is there" : "");
}

// The atlas's pass of this frame is over (another pass begins)
static void at_pass_end(void) {
    if (!at.open) return;
    at.open = false;
    if (at.state == AT_VERIFY) {
        if (at.pending >= 0) at.spoiled = true;  // (its last clearing draw didn't draw)
        at.pending = -1;
        if (at.spoiled || !at.next) {  // (a draw not drawn yet - its pipeline still building -: again in a later frame)
            at_free();
            at.state = at.next && ++at.tries < 8 ? AT_NONE : AT_OFF;
            if (at.state == AT_OFF) LOG("[perf] shadow atlas: its clearing draws could not be checked: read in as before\n");
            return;
        }
        at.nslots = at.next;
        at.seq = vk_pending_seq();
        at.state = AT_WAIT;
    } else if (at.state == AT_OK && at.next != at.nslots) {
        LOG("[perf] shadow atlas: a frame with %d of its %d clearing draws: read in as before from now on\n", at.next, at.nslots);
        at.state = AT_OFF;
    }
}

// (AT_OK) the commands after the pass's first draw have its other clearing draws, as checked: the same vertex program over
// the same rectangle, in order, before anything ends the pass (render targets, copies, dispatches)
typedef struct { uint8_t ds[8]; float vp[4]; uint64_t prog; int found; bool stop, bad; } AtLook;
static void at_look_list(uint64_t p, int depth, AtLook *l) {
    for (int n = 0; n < 200000 && !l->stop; n++) {
        const CmdRec *r = (const CmdRec *)G2H((uint32_t)p);
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL:  // (as exec_list: lists called from 8 deep are not run)
            for (uint64_t i = 0; i < r->x[0] && depth < 8 && !l->stop; i++) at_look_list(((const uint64_t *)r->extra)[i], depth + 1, l);
            break;
        case CMD_BIND_PROGRAM: {
            const NvnProgram *np = (const NvnProgram *)(uintptr_t)r->x[0];
            l->prog = np ? np->stages[0].code_addr : 0;
            break;
        }
        case CMD_BIND_DEPTH_STENCIL: memcpy(l->ds, r->extra, 8); break;
        case CMD_SET_VIEWPORTS: if (r->x[0] == 0 && r->x[1]) memcpy(l->vp, r->extra, 16); break;
        case CMD_SET_RENDER_TARGETS:
            if (r->x[0] || ((const RtRec *)r->extra)->depth != (uint64_t)(uintptr_t)st.rt.depth) { l->stop = true; return; }
            break;
        case CMD_DRAW_ARRAYS: case CMD_DRAW_ELEMENTS:
            if (l->ds[0] && l->ds[1] && compare_fn(l->ds[2]) == VK_COMPARE_OP_ALWAYS) {
                int s = 1 + l->found++;
                if (s >= at.nslots || l->prog != at.prog[s] || l->vp[0] != (float)at.rect[s][0] || l->vp[1] != (float)at.rect[s][1] ||
                    l->vp[2] != (float)at.rect[s][2] || l->vp[3] != (float)at.rect[s][3]) {
                    l->stop = l->bad = true;
                    return;
                }
            }
            break;
        case CMD_DRAW_ARRAYS_INDIRECT: case CMD_DRAW_ELEMENTS_INDIRECT: case CMD_DISPATCH: case CMD_DISPATCH_INDIRECT:
        case CMD_COPY_BUFFER_TO_TEXTURE: case CMD_COPY_TEXTURE_TO_BUFFER: case CMD_COPY_TEXTURE_TO_TEXTURE: case CMD_DOWNSAMPLE:
        case CMD_CLEAR_TEXTURE: case CMD_CLEAR_DEPTH_STENCIL: case CMD_DRAW_TEXTURE: case CMD_RESOLVE_DEPTH:
            l->stop = true;
            return;
        default: break;
        }
        p += r->size;
    }
}
static bool at_look(void) {
    AtLook l = {.prog = st.program->stages[0].code_addr};
    memcpy(l.ds, st.ds, 8), memcpy(l.vp, st.viewport[0], 16);
    // (from the record after the current one, then what follows it in each list that called this one)
    for (int d = exec_top; d >= 0 && !l.stop; d--) {
        const CmdRec *r = (const CmdRec *)G2H((uint32_t)exec_pos[d]);
        if (d < exec_top) {  // (the list's CALL: its next lists first)
            for (uint64_t i = exec_call_i[d] + 1; i < r->x[0] && !l.stop; i++) at_look_list(((const uint64_t *)r->extra)[i], d + 1, &l);
            if (l.stop) break;
        }
        at_look_list(exec_pos[d] + r->size, d, &l);
    }
    return !l.bad && l.found == at.nslots - 1;
}

// (begin_pass) the atlas's pass begins cleared: to the marker when checking it, to the value once checked
static uint32_t at_noload(uint32_t w, uint32_t h, TexBackend *const *att, const int *att_index, uint32_t n, VkClearValue cv[9]) {
    TexBackend *b = at_target();
    if (!b) { at_pass_end(); return 0; }
    if (b != at.b || b->image != at.image) at_pass_end(), at_reset(b);
    if (at.open && at.frame != g_present_count) at_pass_end();  // (last frame's)
    if (at.open) {  // (begun again in the frame: read in, what was drawn kept; nothing is left out after that)
        at.dirty_all = true;
        return 0;
    }
    if (at.state == AT_WAIT && vk_completed_seq() >= at.seq) at_check();
    int32_t rect[4];
    if (n != 1 || att[0] != b || att_index[0] >= 0 || w != b->width || h != b->height || pass_msaa ||
        !at_clear_like(b, cur_draw_pr, cur_draw, cur_draw_indexed, rect))
        return 0;
    if (at.state == AT_NONE) {  // (checked in this frame: its rectangles read back after each clearing draw)
        uint64_t size = (uint64_t)b->width * b->height * 4;
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &at.buf, &at.mem, &info) != VK_SUCCESS) { at.state = AT_OFF; return 0; }
        at.ptr = info.pMappedData, at.cap = size, at.off = 0, at.seq = 0, at.spoiled = false;
        at.state = AT_VERIFY, at.open = true, at.frame = g_present_count, at.next = 0, at.pending = -1;
        at.dirty_all = false, memset(at.dirty, 0, sizeof at.dirty);
        cv[8].depthStencil = (VkClearDepthStencilValue){f_of(AT_MARKER), 0};
        LOG("[perf] shadow atlas %ux%u: checking its clearing draws\n", b->width, b->height);
        return 1u << 8;
    }
    if (at.state != AT_OK || memcmp(rect, at.rect[0], sizeof rect) || at_sig(cur_draw, cur_draw_indexed, cur_draw_pr, NULL) != at.sig[0] ||
        !at_look())
        return 0;
    at.open = true, at.frame = g_present_count, at.next = 0;
    at.dirty_all = false, memset(at.dirty, 0, sizeof at.dirty);
    cv[8].depthStencil = (VkClearDepthStencilValue){f_of(at.value), 0};
    stat_atlas_clear++;
    return 1u << 8;
}

// (=2) the 32x32 tiles a rectangle touches: marked (set) or whether any is marked
static bool at_mark(const int32_t r[4], bool set) {
    if (r[2] <= 0 || r[3] <= 0) return false;
    if (r[0] + r[2] > 4096 || r[1] + r[3] > 4096) {  // (past what the tiles cover: all of it)
        if (set) at.dirty_all = true;
        return true;
    }
    int x0 = r[0] >> 5, x1 = (r[0] + r[2] - 1) >> 5, y0 = r[1] >> 5, y1 = (r[1] + r[3] - 1) >> 5;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            uint32_t *w = &at.dirty[y][x >> 5], bit = 1u << (x & 31);
            if (set) *w |= bit;
            else if (*w & bit) return true;
        }
    return false;
}

// (draw, in the pass begun for it) a draw into the atlas's pass. true: left out (=2)
static bool at_draw(const CmdRec *r, bool indexed, Prog *pr) {
    if (at.pending >= 0) at.spoiled = true, at.pending = -1;  // (the clearing draw before didn't draw)
    if (!at.open || !in_pass || st.rt.depth == NULL || st.rt.depth->backend != at.b || st.rt.ncolor) return false;
    int32_t rect[4];
    if (at_clear_like(at.b, pr, r, indexed, rect)) {
        int s = at.next++;
        if (at.state == AT_VERIFY) {  // (read back after it: at_drawn)
            uint64_t bytes = (uint64_t)rect[2] * (uint32_t)rect[3] * 4;
            uint64_t sig = s < AT_SLOTS ? at_sig(r, indexed, pr, at.part[s]) : 0;
            if (s >= AT_SLOTS || at.off + bytes > at.cap || !sig) { at.spoiled = true, at.tries = 8; return false; }
            memcpy(at.rect[s], rect, sizeof rect);
            at.sig[s] = sig, at.prog[s] = st.program->stages[0].code_addr, at.slot_off[s] = at.off;
            at.off += bytes;
            at.pending = s;
            return false;
        }
        if (at.state != AT_OK) return false;
        uint64_t part[AT_PARTS] = {0};
        if (s >= at.nslots || memcmp(rect, at.rect[s], sizeof rect) || at_sig(r, indexed, pr, part) != at.sig[s]) {
            char w[80] = "";
            static const char *const pn[AT_PARTS] = {" state", " indices", " vertices", " storage buffers", " constants", " uniforms"};
            for (int i = 0; s < at.nslots && i < AT_PARTS; i++)
                if (part[i] != at.part[s][i]) strncat(w, pn[i], sizeof w - strlen(w) - 1);
            LOG("[perf] shadow atlas: clearing draw %d differs from the one checked (%s): read in as before from now on\n", s,
                s >= at.nslots ? "one more" : memcmp(rect, at.rect[s], sizeof rect) ? "rectangle" : w);
            at.state = AT_OFF;
            return false;
        }
        if (perf.shadow_clear < 2 || at.dirty_all || at_mark(rect, false)) return false;
        stat_atlas_skipped++;
        return true;
    }
    if (at.dirty_all) return false;
    int32_t bnd[4];
    at_bounds(bnd);
    at_mark(bnd, true);
    return false;
}

// ---- (MHGU_PERF_HUDSTART_FUSE, fsr.c hud_start) the output-size depth stand-in is cleared to far as the first pass that
// renders into it begins (LOAD_OP_CLEAR, the same values), where that pass covers it whole, instead of by a clear of its
// own before the HUD's pass, which then read it back in. Anything else first: the clear as before (depth_clear_now).
void vk_depth_clear_later(TexBackend *b) { depth_clear_pending = b; }
void vk_depth_clear_forget(TexBackend *b) {
    if (depth_clear_pending == b) depth_clear_pending = NULL;
}
static void depth_clear_now(void) {  // (outside a pass)
    TexBackend *b = depth_clear_pending;
    depth_clear_pending = NULL;
    VkClearDepthStencilValue far = {1.0f, 0};
    VkImageSubresourceRange rg = {b->aspect, 0, 1, 0, 1};
    VkCommandBuffer cb = vk_cmd();
    vkCmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &far, 1, &rg);
    vk_full_barrier(cb);
}

// (begin_pass, outside the pass, its attachments known) the attachments the MHGU_PERF_* switches begin cleared (bit i
// colour i, bit 8 depth / stencil) and their values: only where the pass's first draws write all of them anyway
static uint32_t perf_noload(uint32_t w, uint32_t h, TexBackend *const *att, const int *att_index, uint32_t n, VkClearValue cv[9]) {
    uint32_t got = 0;
    for (uint32_t k = 0; k < n && depth_clear_pending; k++) {
        if (att[k] != depth_clear_pending || att_index[k] >= 0) continue;
        if (w == att[k]->width && h == att[k]->height && !pass_msaa) {
            depth_clear_pending = NULL;
            cv[8].depthStencil = (VkClearDepthStencilValue){1.0f, 0};
            got |= 1u << 8;
        } else depth_clear_now();
    }
    for (uint32_t k = 0; k < n && fill.dst; k++)  // (its source an attachment too: the draw would sample it in the pass)
        if (att[k] == fill.src) fill_now();
    bool has_fill = false;
    for (uint32_t k = 0; k < n && fill.dst; k++) has_fill |= att[k] == fill.dst;
    if (fill.dst && fill.kind == 1 && !has_fill) fill_now();  // (a copy: done before any other pass, which may draw into its input)
    for (uint32_t k = 0; k < n && fill.dst; k++) {
        if (att[k] != fill.dst) continue;
        int i = att_index[k];
        if (i >= 0 && ((pass_noload >> i) & 1)) {  // (the game clears it whole as the pass begins: nothing to scale into it)
            fill.dst = fill.src = NULL;
        } else if (i >= 0 && w == att[k]->width && h == att[k]->height && !pass_msaa && pass_samples == 1 &&
                   pass_color[i] == att[k]->format) {
            fill_index = i;
            cv[i].color = (VkClearColorValue){{0, 0, 0, 0}};
            got |= 1u << i;
        } else fill_now();
    }
    if (perf.shadow_clear) got |= at_noload(w, h, att, att_index, n, cv);
    return got;
}

// ---- (MHGU_PERF_HUDSTART_FUSE & 2, fsr.c hud_start) Plain scaling: the 3D picture H is scaled into its output-size stand-in
// H' as the first draw of the HUD's pass (fill_draw: a full-screen triangle sampling H bilinearly at each pixel's centre,
// the pass begun cleared under it) instead of by a blit before it, which that pass then read back in. Only once the two
// were compared on this GPU: the first frame scaled so also draws into a picture of its own, and both are read back
// (fill_check): every byte the same, or the blit stays. Anything else that reads H' first gets the blit (fill_now).
// (MHGU_PERF_FXAA_COPYDRAW, fxaa_skipped) The same for FXAA Off's copy of the FXAA pass's input into its target (kind 1:
// each texel fetched), drawn first in the very next pass when that pass draws into the target (the game's next draw, with
// Bloom On, adds the bloom there), compared once with vkCmdCopyImage; any other pass or copy gets the copy first.
enum { FILL_UNKNOWN, FILL_CHECKING, FILL_OK, FILL_OFF };
static struct {
    int state;
    uint32_t sw, sh, dw, dh;  // (the sizes and format it was checked with)
    VkFormat f;
    TexBackend scratch;
    VkBuffer buf;
    VmaAllocation mem;
    void *ptr;
    uint64_t seq;
} fcs[2];
static VkDescriptorSetLayout fill_set_layout;
static VkPipelineLayout fill_layout;

static VkPipeline fill_pipeline(VkRenderPass rp, uint32_t index, uint32_t ncolor, VkFormat depth, uint32_t samples, int kind) {
    static struct { VkRenderPass rp; uint32_t index; int kind; VkPipeline p; } tab[16];
    static int n;
    for (int i = 0; i < n; i++) if (tab[i].rp == rp && tab[i].index == index && tab[i].kind == kind) return tab[i].p;
    if (!fill_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &fill_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &fill_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &fill_layout));
    }
    char fs[32];
    snprintf(fs, sizeof fs, kind ? "copy.frag.%u" : "fill.frag.%u", index);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("resolve.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fs), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
                                            .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att[8] = {{0}};
    for (uint32_t i = 0; i < ncolor && i < 8; i++) att[i].colorWriteMask = i == index ? 0xF : 0;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = ncolor, .pAttachments = att};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
                                           .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vs, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                       .pDepthStencilState = depth != VK_FORMAT_UNDEFINED ? &ds : NULL, .pColorBlendState = &cb,
                                       .pDynamicState = &dy, .layout = fill_layout, .renderPass = rp};
    VkPipeline p = VK_NULL_HANDLE;
    if (sync_pipeline(&pi, &p) != VK_SUCCESS) LOG("[vk] scaling pipeline failed\n");
    if (n < 16) tab[n].rp = rp, tab[n].index = index, tab[n].kind = kind, tab[n++].p = p;
    return p;
}

// (inside a pass of rp's kind, w x h) src scaled over colour target index, as vkCmdBlitImage LINEAR would (kind 1: copied,
// as vkCmdCopyImage would)
static void fill_draw(VkCommandBuffer cb, TexBackend *src, VkRenderPass rp, uint32_t index, uint32_t ncolor, VkFormat depth,
                      uint32_t samples, uint32_t w, uint32_t h, int kind) {
    static const VkComponentMapping id = {0};
    VkPipeline p = fill_pipeline(rp, index, ncolor, depth, samples, kind);
    VkImageView v = vk_image_view(src, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!p || !v) return;
    VkDescriptorSet set = vk_alloc_set(fill_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), v, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, fill_layout, 0, 1, &set, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D area = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &area);
    float pc[4] = {1.0f / (float)w, 1.0f / (float)h, 0, 0};
    vkCmdPushConstants(cb, fill_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    ss_forget();  // (its own pipeline, set and state: the next draw sets them all again)
}

// The blit, as fsr.c scale_to_stand does it (outside a pass); kind 1: the copy, as copy_texture does it
static void fill_blit(TexBackend *s, TexBackend *d, int kind) {
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    if (kind) {
        VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .extent = {d->width, d->height, 1}};
        vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    VkImageBlit r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)s->width, (int32_t)s->height, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)d->width, (int32_t)d->height, 1}}};
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r, VK_FILTER_LINEAR);
    vk_full_barrier(cb);
}

static void fill_now(void) {  // (H' read before a pass drew it in: the blit, now)
    if (!fill.dst) return;
    if (in_pass) vk_end_pass(), st.rt_dirty = true;  // (from begin_pass, no pass is open: none ends)
    fill_blit(fill.src, fill.dst, fill.kind);
    fill.dst = fill.src = NULL;
}
void vk_color_fill_flush(TexBackend *b) { if (fill.dst && (!b || b == fill.dst)) fill_now(); }
static void fill_copy_flush(void) { if (fill.dst && fill.kind == 1) fill_now(); }  // (a copy, clear or dispatch first)
void vk_color_fill_forget(TexBackend *b) { if (fill.dst == b || fill.src == b) fill.dst = fill.src = NULL; }

// (FILL_CHECKING, its copies done) the blit's (or copy's) picture and the draw's: the same bytes
static void fill_check(int kind) {
#define fc fcs[kind]
    vmaInvalidateAllocation(g_vk.vma, fc.mem, 0, VK_WHOLE_SIZE);
    const uint8_t *a = fc.ptr, *b = a + (size_t)fc.dw * fc.dh * 4;
    uint64_t n = (uint64_t)fc.dw * fc.dh * 4, ndiff = 0;
    int mx = 0;
    for (uint64_t i = 0; i < n; i++)
        if (a[i] != b[i]) {
            ndiff++;
            int d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
            if (d > mx) mx = d;
        }
    vk_defer_buffer(fc.buf, fc.mem);
    fc.buf = VK_NULL_HANDLE;
    vk_views_forget(&fc.scratch);
    vk_defer_image(fc.scratch.image, fc.scratch.mem);
    fc.scratch.image = VK_NULL_HANDLE;
    fc.state = ndiff ? FILL_OFF : FILL_OK;
    const char *what = kind ? "FXAA Off's copy" : "HUD start";
    LOG("[perf] %s %ux%u -> %ux%u: the draw %s the %s%s\n", what, fc.sw, fc.sh, fc.dw, fc.dh, ndiff ? "differs from" : "gives the same bytes as",
        kind ? "copy" : "blit", ndiff ? ": that stays" : ": drawn in the next pass from now on");
    if (ndiff) LOG("[perf] %s: %llu of %llu bytes differ, by %d at most\n", what, (unsigned long long)ndiff, (unsigned long long)n, mx);
#undef fc
}

// (fsr.c hud_start, a frame scaled without FSR) true: H' is filled from H in the first pass that draws into it (fill_draw);
// false: the caller blits, and then calls vk_color_fill_check on the first such frame
bool vk_color_fill_later(TexBackend *dst, TexBackend *src, int kind) {
    fill_used = true;
#define fc fcs[kind]
    if (fc.state == FILL_CHECKING && vk_completed_seq() >= fc.seq) fill_check(kind);
    if (fc.state != FILL_OK || g_vk.msaa > 1 || src->width != fc.sw || src->height != fc.sh || dst->width != fc.dw ||
        dst->height != fc.dh || dst->format != fc.f || src->format != fc.f)
        return false;
#undef fc
    if (fill.dst) fill_now();  // (one not drawn yet: as it would have been)
    fill.dst = dst, fill.src = src, fill.kind = kind;
    return true;
}

// (after the blit, the first time) the scaling draw into a picture of its own, and both read back for fill_check
void vk_color_fill_check(TexBackend *dst, TexBackend *src, int kind) {
#define fc fcs[kind]
    if (fc.state != FILL_UNKNOWN || g_vk.msaa > 1 || src->format != dst->format || src->samples != 1 || dst->samples != 1 ||
        (dst->format != VK_FORMAT_R8G8B8A8_UNORM && dst->format != VK_FORMAT_R8G8B8A8_SRGB && dst->format != VK_FORMAT_B8G8R8A8_UNORM))
        return;
    fc.state = FILL_OFF;  // (until it is made)
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = dst->format,
                            .extent = {dst->width, dst->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    if (vk_create_image(&ci, &fc.scratch.image, &fc.scratch.mem) != VK_SUCCESS) return;
    TexBackend *t = &fc.scratch;
    t->format = dst->format, t->width = dst->width, t->height = dst->height;
    t->depth = t->layers = t->levels = t->samples = 1, t->aspect = VK_IMAGE_ASPECT_COLOR_BIT, t->base_type = VK_IMAGE_VIEW_TYPE_2D;
    uint64_t one = (uint64_t)dst->width * dst->height * 4;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = one * 2, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &fc.buf, &fc.mem, &info) != VK_SUCCESS) {
        vk_defer_image(t->image, t->mem), t->image = VK_NULL_HANDLE;
        return;
    }
    fc.ptr = info.pMappedData;
    vk_end_pass();
    st.rt_dirty = true;
    VkCommandBuffer cb = vk_cmd();
    vk_image_init_layout(cb, t->image, t->aspect);
    vk_full_barrier(cb);
    static const VkComponentMapping id = {0};
    VkFormat f = dst->format;
    VkRenderPass rp = vk_render_pass(&f, 1, VK_FORMAT_UNDEFINED, 1);
    VkImageView v = vk_image_view(t, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                .framebuffer = vk_framebuffer(rp, &v, 1, t->width, t->height), .renderArea = {{0, 0}, {t->width, t->height}}};
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    fill_draw(cb, src, rp, 0, 1, VK_FORMAT_UNDEFINED, 1, t->width, t->height, kind);
    vkCmdEndRenderPass(cb);
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = 0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {t->width, t->height, 1}};
    vkCmdCopyImageToBuffer(cb, dst->image, VK_IMAGE_LAYOUT_GENERAL, fc.buf, 1, &c);
    c.bufferOffset = one;
    vkCmdCopyImageToBuffer(cb, t->image, VK_IMAGE_LAYOUT_GENERAL, fc.buf, 1, &c);
    vk_full_barrier(cb);
    fc.sw = src->width, fc.sh = src->height, fc.dw = dst->width, fc.dh = dst->height, fc.f = f;
    fc.seq = vk_pending_seq();
    fc.state = FILL_CHECKING;
    LOG("[perf] %s %ux%u -> %ux%u: the draw compared with the %s\n", kind ? "FXAA Off's copy" : "HUD start", fc.sw, fc.sh,
        fc.dw, fc.dh, kind ? "copy" : "blit");
#undef fc
}

// (clear) the pass's depth cleared: in the atlas's pass, nothing more is left out in it
static void at_written(void) {
    if (at.open) at.dirty_all = true;
}

// (draw, after recording it) a clearing draw being checked: its rectangle read back
static void at_drawn(void) {
    int s = at.pending;
    if (s < 0) return;
    at.pending = -1;
    vk_end_pass();
    st.rt_dirty = true;  // (the next draw begins the pass again, reading the atlas in)
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = at.slot_off[s], .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                           .imageOffset = {at.rect[s][0], at.rect[s][1], 0},
                           .imageExtent = {(uint32_t)at.rect[s][2], (uint32_t)at.rect[s][3], 1}};
    vkCmdCopyImageToBuffer(cb, at.b->image, VK_IMAGE_LAYOUT_GENERAL, at.buf, 1, &c);
    vk_full_barrier(cb);
}

// The pipeline a draw of pr needs in the pass open now, with the state set now
static PipeKey pipe_key(Prog *pr, uint32_t mode, bool native, VkPrimitiveTopology topo, bool flip_y) {
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.prog = pr;
    k.rp = pass_rp;
    k.ncolor = pass_ncolor;
    k.samples = pass_samples;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        k.attr_mask |= 1u << i;
        uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= st.nattrib || fmt == 0 || stream >= 16) {  // unbound attribute: (0, 0, 0, 1) from a constant stream
            k.attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
            k.attr_bind[i] = ZERO_BINDING;
            k.used_streams |= 1u << ZERO_BINDING;
            continue;
        }
        k.attr_fmt[i] = vertex_format_supported(vertex_format(fmt));
        k.attr_off[i] = off;
        k.attr_bind[i] = stream;
        k.used_streams |= 1u << stream;
    }
    for (int s = 0; s < 16; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        k.stride[s] = s < st.nstream ? st.stream[s][0] : 0;
        k.divisor[s] = s < st.nstream && k.stride[s] ? st.stream[s][1] : 0;
        if (k.divisor[s] > 1) {  // (an element every N instances: VK_EXT_vertex_attribute_divisor, else every instance)
            static uint32_t told;  // (each divisor up to 31 once: which the game uses, a driver's divisor or not, #171)
            if (k.divisor[s] < 32 && !(told & (1u << k.divisor[s])))
                told |= 1u << k.divisor[s], LOG("[vk] a stream read once every %u instances (%s)\n", k.divisor[s],
                                                g_vk.divisor ? "the driver's divisor" : "no divisor here: read every instance");
            if (!g_vk.divisor) k.divisor[s] = 1;
        }
    }
    for (uint32_t i = 0; i < k.ncolor && i < 8; i++) {
        if (!st.rt.color[i] || pass_color[i] == VK_FORMAT_UNDEFINED) continue;
        // targets the fragment shader doesn't write keep their contents (undefined in Vulkan otherwise): masked
        k.write_mask[i] = ((pr->fi->out_mask >> (i * 4)) & 0xF) ? (st.channel_mask >> (i * 4)) & 0xF : 0;
        if (((st.color_state[0] >> i) & 1) && !rt_kind(st.rt.color[i]->format)) {
            k.blend_on |= (uint8_t)(1u << i);
            memcpy(k.blend[i], st.blend[i], 8);
        }
    }
    {  // (debug) MHGU_FORCE_WMASK=<fragment code address>: that shader writes all channels of target 0 (override shaders)
        static uint64_t fw = ~0ull;
        if (fw == ~0ull) { const char *e = getenv("MHGU_FORCE_WMASK"); fw = e ? strtoull(e, NULL, 16) : 0; }
        if (fw && st.program->stages[1].code_addr == fw && k.ncolor) k.write_mask[0] = 0xF;
    }
    k.a2c = st.ms[0xC] != 0 && !a2c_as_test;
    k.topology = (uint8_t)(native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    k.poly_fill = st.poly[2] != 1;
    k.cull = st.poly[0];
    bool ccw = st.poly[1] == 1;
    k.front_ccw = ccw != flip_y;
    k.prim_restart = st.prim_restart && native && (mode == 3 || mode == 5);
    k.depth_clamp = st.depth_clamp && g_vk.depth_clamp;  // (shadow casters beyond the light's near / far planes)
    k.depth_bias = (st.poly[3] & 4) != 0;
    bool has_depth = pass_depth != VK_FORMAT_UNDEFINED && pass_depth != VK_FORMAT_S8_UINT;
    bool has_stencil = pass_depth == VK_FORMAT_D24_UNORM_S8_UINT || pass_depth == VK_FORMAT_D32_SFLOAT_S8_UINT || pass_depth == VK_FORMAT_S8_UINT;
    k.depth_test = st.ds[0] && has_depth;
    k.depth_write = k.depth_test && st.ds[1];
    k.depth_op = (uint8_t)(k.depth_test ? compare_fn(st.ds[2]) : VK_COMPARE_OP_ALWAYS);
    k.stencil_test = st.ds[3] && has_stencil;
    k.sample_shading = pass_msaa && pass_models && !k.depth_test;  // (the menu's 2D draws after the models: pass_models)
    if (k.stencil_test) {
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        for (int f = 0; f < 2; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            k.sten_ops[f][0] = (uint8_t)stencil_op(o & 0xF);
            k.sten_ops[f][1] = (uint8_t)stencil_op((o >> 8) & 0xF);
            k.sten_ops[f][2] = (uint8_t)stencil_op((o >> 4) & 0xF);
            k.sten_ops[f][3] = (uint8_t)compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF);
        }
    }
    return k;
}

// A pipeline first made at a draw: recorded in pipelines.bin for the next run's start; no cache has it now (cold)
static void pipe_record(Pipe *pp) {
    pp->t_req = plat_now_ns();
    pp->cold = true;
    pipecache_store(pp);
    if (HITCH_ON) hitch_add(HC_PIPE_NEW, plat_now_ns() - pp->t_req, 0);  // (the time: its record written)
}

// A draw skipped for its program, still loading from the program cache: its pipeline is keyed now (the program's
// ShaderInfo is known) and attached to the program as pipelines.bin's are, so its build starts as soon as the program
// is built rather than at a draw a frame or more later. Only where the key is the one that draw will need: the pass it
// draws into is open and stays so (no MSAA pass to begin for 3D models), no alpha test as sample coverage (another
// program).
static void pipe_ahead(Prog *pr, uint32_t mode, bool flip_y, int af) {
    if (atomic_load(&pr->state) == 2 || !in_pass || st.rt_dirty) return;
    if ((pass_models && !pass_msaa && st.ds[0] && st.rt.depth && !pass_want_depth) ||
        (pass_samples > 1 && (af == 2 || af == 4 || af == 5 || af == 7)))
        return;
    VkPrimitiveTopology topo;
    bool native = topology(mode, &topo);
    if (!native && mode != 6 && mode != 7 && mode != 9) return;
    PipeKey k = pipe_key(pr, mode, native, topo, flip_y);
    bool created;
    Pipe *pp = pipe_slot(&k, &created);
    if (!pp || !created) return;
    pipe_record(pp);
    pipe_attach(pr, pp);
}

// The polygon offsets of a pass with only a depth target (the shadow atlas's casters), each one once, the first 8: with the
// depth format and its depth clamp, so a log shows what a GPU is given where its shadows differ (#180). f: factor, units, clamp.
static void depth_bias_tell(const float f[3], bool depth_clamp) {
    static float seen[8][3];
    static int n;
    if (n >= 8) return;
    for (int i = 0; i < n; i++) if (!memcmp(seen[i], f, sizeof seen[i])) return;
    memcpy(seen[n++], f, sizeof seen[0]);
    LOG("[vk] depth-only pass %ux%u, format %d: polygon offset factor %g units %g clamp %g, depth clamp %s\n", pass_w, pass_h,
        (int)pass_depth, f[0], f[1], f[2], depth_clamp ? "on" : "off");
}

static void draw(CmdRec *r, bool indexed) {
    dtex_si[0] = dtex_si[1] = NULL;  // (MHGU_PERF_TEXSCAN: the last draw's textures are no longer this one's)
    if (!st.program) { stat_skipped++; return; }
    if (shadow_skipped()) { stat_skipped++; return; }
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(st.rt.color[i]->format);
    if (st.color_state[2] >= 1 && st.color_state[2] <= 7) fkey.alpha_func = st.color_state[2];  // (NVN alpha test)
    Prog *pr = get_program(st.program, &vkey, &fkey);
    if (!pr) { stat_skipped++; return; }
    if (!await_build(&pr->state, pr, 0, pr->loaded)) {
        if (pr->loaded) pipe_ahead(pr, (uint32_t)r->x[0], vkey.flip_y, fkey.alpha_func);
        skip_draw(&stat_skip_prog);
        return;
    }
    fs_new(pr, (uint32_t)r->x[2]);
    if (fxaa_skipped(pr) || lowend_skipped(pr)) return;
    if (perf.texscan) draw_textures(pr->vi, pr->fi);
    NvnTexture *movie = movie_picture(pr);
    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    bool native = topology(mode, &topo);
    if (!native && mode != 6 && mode != 7 && mode != 9) { stat_skipped++; return; }
    if (g_fsr_on) {  // FSR: the frame's structure; at the start of the HUD FSR runs before this draw (fsr.c)
        FsrDraw fd = {.color0 = st.rt.ncolor ? st.rt.color[0] : NULL, .depth = st.rt.depth, .count = (uint32_t)r->x[2],
                      .depth_write = st.ds[0] && st.ds[1], .depth_test = st.ds[0] && st.ds[2] != 8,  // (8: Always)
                      .vubuf = &st.ubuf[0][0].addr, .nvubuf = 18,
                      .vp_w = st.viewport[0][2], .vp_h = st.viewport[0][3]};
        for (int i = 0; i < pr->fi->ntex && fd.ntex < 16; i++) fd.tex[fd.ntex++] = draw_texture(1, pr->fi, i);
        vk_fsr_draw(&fd);
    }

    ms_want_depth = st.ds[0] && st.rt.depth;  // (MSAA, a picture 3D models are drawn into: pass_models)
    if (in_pass && pass_models && !pass_msaa && ms_want_depth && !pass_want_depth) st.rt_dirty = true;
    models_fxaa_track(ms_want_depth);  // (FXAA: the same, models_fxaa_end)
    if (st.rt_dirty) vk_end_pass();  // (MSAA: the ending pass marks its targets for resolving before they are read)
    ms_resolve_sampled(0, pr->vi);
    ms_resolve_sampled(1, pr->fi);
    if (fill.dst) fill_sampled_stage(0, pr->vi), fill_sampled_stage(1, pr->fi);  // (MHGU_PERF_HUDSTART_FUSE: H' read first)
    resolve_feedback(pr);
    if (st.rt.depth && st.ds[0] && st.ds[1]) snapshots_invalidate(st.rt.depth);
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    if (in_pass && pass_ro_depth && !ro_want && draw_writes_ds()) st.rt_dirty = true, stat_ro_left++;  // (read-only no more)
    cur_draw = r, cur_draw_indexed = indexed, cur_draw_pr = pr;  // (perf_noload: the pass's first draw)
    begin_pass();
    cur_draw = NULL;
    ro_want = false;
    pass_skip_clear = 0;  // (drawn into: later clears are clears again)
    if (perf.shadow_clear && at_draw(r, indexed, pr)) return;  // (=2: a clearing draw over what the atlas began with)
    VkCommandBuffer cb = vk_cmd();
    if (st.ms[0xC]) stat_a2c++;
    int af = fkey.alpha_func;
    // One sample on an Adreno: the game's alpha to coverage became nothing at all for partly transparent texels (the
    // tall grass missing on an Adreno 640, #70; Mali keeps those at alpha 0.5 and up): done as an alpha test at 0.5
    a2c_as_test = false;
    if (pass_samples <= 1 && st.ms[0xC] && af == 0 && vk_gpu_adreno()) {
        a2c_as_test = true;
        fkey.alpha_func = 7;
        pr = get_program(st.program, &vkey, &fkey);
        if (!pr) { stat_skipped++; return; }
        if (!await_build(&pr->state, pr, 0, pr->loaded)) { skip_draw(&stat_skip_prog); return; }
    }
    if (pass_samples > 1 && (af == 2 || af == 4 || af == 5 || af == 7)) {  // MSAA: the alpha test as sample coverage
        fkey.alpha_coverage = (uint8_t)pass_samples;
        pr = get_program(st.program, &vkey, &fkey);
        if (!pr) { stat_skipped++; return; }
        if (!await_build(&pr->state, pr, 0, pr->loaded)) { skip_draw(&stat_skip_prog); return; }
    }

    // ---- pipeline
    PipeKey k = pipe_key(pr, mode, native, topo, vkey.flip_y);
    // (3D models drawn over the finished picture, without MSAA: from the next frame on, for as long as they are drawn,
    // the player's anti-aliasing for them: ms_expand, models_fxaa_end)
    if (k.depth_write) {
        TexBackend *c0 = st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->backend : NULL;
        if (c0 && (!pass_msaa || c0->ms_models) && full_picture(pass_w, pass_h)) c0->ms_models = g_present_count;
    }
    bool created = false;
    Pipe *pp;
    // (MHGU_PERF_PIPEMEMO) the same key as the last draw's: the same entry of pipe_tab, which only ever gains entries
    static PipeKey memo_k;
    static Pipe *memo_pp;
    if (perf.pipememo && memo_pp && !memcmp(&memo_k, &k, sizeof k)) {
        pp = memo_pp, stat_pipememo++;
        if (perf_check) {
            bool c;
            stat_check_n++;
            if (pipe_slot(&k, &c) != pp || c) check_bad("the memoised pipeline");
        }
    } else {
        pp = pipe_slot(&k, &created);
        if (perf.pipememo && pp) memo_k = k, memo_pp = pp;
    }
    if (!pp) { stat_skipped++; return; }
    if (created) {
        pipe_record(pp);
        run_job(build_pipeline, pp);
    }
    // A movie frame is the whole picture: skipped, the screen showed the pass's black clear for a few frames at the
    // start of the first movie after an update (its pipeline not in cache/pipelines.bin yet, ~2 frames to build). It
    // waits for its pipeline, so the picture before it stays up that long instead.
    if (!await_build(&pp->state, pp, movie ? 2000000000ll : 0, !pp->cold)) { skip_draw(&stat_skip_pipe); return; }
    if (ss_need(SS_PIPE, ss.pipe == pp->pipe)) {
        ss.pipe = pp->pipe;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pipe);
        lost_prog(st.program->stages[0].code_addr, st.program->stages[1].code_addr);
    }

    // ---- dynamic state
    if (k.stencil_test) {
        for (int f = 0; f < 2; f++) {
            uint32_t v[3] = {st.stencil_vmask[f] & 0xFF, st.stencil_mask[f] & 0xFF, st.stencil_ref[f]};
            if (!ss_need(SS_STEN << (f * 8), !memcmp(v, ss.sten[f], sizeof v))) continue;
            memcpy(ss.sten[f], v, sizeof v);
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            vkCmdSetStencilCompareMask(cb, face, v[0]);
            vkCmdSetStencilWriteMask(cb, face, v[1]);
            vkCmdSetStencilReference(cb, face, v[2]);
        }
    }
    if (ss_need(SS_BC, !memcmp(ss.bc, st.blend_color, sizeof ss.bc))) { memcpy(ss.bc, st.blend_color, sizeof ss.bc); vkCmdSetBlendConstants(cb, st.blend_color); }
    {
        float f[3] = {0, 0, 0};
        if (k.depth_bias) memcpy(f, st.poly_offset, sizeof f);
        if (k.depth_bias && !pass_ncolor) depth_bias_tell(f, k.depth_clamp);
        if (ss_need(SS_DB, !memcmp(ss.db, f, sizeof f))) { memcpy(ss.db, f, sizeof f); vkCmdSetDepthBias(cb, f[1], f[2], f[0]); }
    }
    float *vp = st.viewport[0], movie_vp[4], v[4];
    PreviewMap pm;
    if (movie && movie_viewport(movie, vp, movie_vp)) vp = movie_vp;
    else preview_tell();
    memcpy(v, vp, sizeof v);
    if (vp != movie_vp && ui_preview_map(&pm)) {  // (a model preview: the viewport keeps its size, its centre moves)
        v[0] = pm.sx + (vp[0] + vp[2] * 0.5f) * pm.kx - vp[2] * 0.5f;
        v[1] = pm.sy + (vp[1] + vp[3] * 0.5f) * pm.ky - vp[3] * 0.5f;
    }
    set_viewport(cb, v[0] * pass_sx, v[1] * pass_sy, v[2] * pass_sx, v[3] * pass_sy, st.depth_range[0][0],
                 st.depth_range[0][1]);  // (FSR: pass_s* scale onto output-size stand-ins, else 1)
    apply_scissor(cb);

    // ---- descriptor sets
    uint32_t doff[34];  // (MHGU_PERF_DYNUBO: the dynamic offsets, set 0's then set 1's)
    int nd = pr->ndyn[0] + pr->ndyn[1];
    VkDescriptorSet sets[2] = {stage_set(pr->nbinds, pr->binds, pr->set_layout[0], 0, 0, pr->vi, 0, 0, 0, pr->dyn_mask[0], doff),
                               stage_set(pr->nbinds, pr->binds, pr->set_layout[1], 1, 1, pr->fi, 0, 0, 0, pr->dyn_mask[1],
                                         doff + pr->ndyn[0])};
    if (ss_need(SS_SETS, ss.sets_layout == pr->layout && !memcmp(ss.sets, sets, sizeof sets) &&  // (the same ones: kept)
                             (!nd || !memcmp(ss.doff, doff, (size_t)nd * sizeof *doff)))) {
        ss.sets_layout = pr->layout, memcpy(ss.sets, sets, sizeof sets);
        if (nd) memcpy(ss.doff, doff, (size_t)nd * sizeof *doff), stat_dyn_binds++;
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, sets, (uint32_t)nd, nd ? doff : NULL);
    }
    if (ui_relax_scissor || ui_scissor_v >= 0 || ui_scissor_mid) {  // (set while the constants above were built: for this
        apply_scissor(cb);                         // draw only; the next draw and any clear before it get the game's scissor again)
        ui_relax_scissor = false, ui_scissor_v = -1, ui_scissor_mid = false;
    }
    // (debug, "hudtrace") each draw: its fragment program, viewport and scissor as set and as the game gave them, depth
    // test and write, vertices, and the textures it samples (size / NVN format)
    if (g_hudtrace_active) {
        char tl[200];
        int tn = 0;
        tl[0] = 0;
        for (int i = 0; i < pr->fi->ntex && tn < 160; i++) {
            NvnTexture *tx = shader_texture_object(1, &pr->fi->tex[i]);
            if (tx) tn += snprintf(tl + tn, sizeof tl - tn, " t%ux%u/%#x", tx->width, tx->height, tx->format);
        }
        LOG("[hud]   draw fs %016llx vp %.1f,%.1f %.1fx%.1f (game %.1f,%.1f %.1fx%.1f) sc %d,%d %ux%u (game %d,%d %dx%d) ds %d%d n %u "
            "%s\n", (unsigned long long)prog_fs_hash(pr),
            ss.vp.x, ss.vp.y, ss.vp.width, ss.vp.height, vp[0], vp[1], vp[2], vp[3], ss.sc.offset.x, ss.sc.offset.y,
            ss.sc.extent.width, ss.sc.extent.height, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3],
            (int)st.ds[0], (int)st.ds[1], (uint32_t)r->x[2], tl);
    }

    // ---- indices (copied; the largest index bounds the vertex data copied below)
    uint32_t first = 0, count = (uint32_t)r->x[2], inst, base_inst, max_index = 0;
    int32_t base_vertex = 0;
    const void *lazy_src = NULL;  // (MHGU_PERF_LAZYIDX) 16 / 32-bit indices whose largest is still to find
    uint32_t lazy_isz = 0, lazy_restart = 0;
    VkSpan ib = {0};
    VkIndexType itype_vk = VK_INDEX_TYPE_UINT32;
    uint32_t icount = 0;
    bool use_index = indexed || !native;
    if (!indexed) {
        first = (uint32_t)r->x[1]; base_inst = (uint32_t)r->x[3]; inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        max_index = first + count;
        if (!native) {
            uint32_t *e = expand_indices(mode, count, NULL, 0, first, &icount);
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
    } else {
        uint32_t itype = (uint32_t)r->x[1];
        uint64_t iaddr = r->x[3];
        base_vertex = (int32_t)r->x[4];
        base_inst = (uint32_t)r->x[5];
        inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        uint32_t isz = itype == 0 ? 1 : itype == 1 ? 2 : 4;
        if (!guest_range_ok(iaddr, (uint64_t)count * isz)) { stat_skipped++; return; }
        const void *src = G2H((uint32_t)iaddr);
        uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu;
        if (native && itype != 0) {
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            icount = count;
            ib = perf.idxcopy ? guest_copy_indices(iaddr, (uint64_t)count * isz + 4) : vk_transient_copy(src, (uint64_t)count * isz + 4, 4);
            if (perf_check && perf.idxcopy && (stat_check_n++, memcmp(ib.ptr, src, (size_t)count * isz + 4))) check_bad("indices");
            if (perf.lazyidx) lazy_src = src, lazy_isz = isz, lazy_restart = restart;  // (found below if a stream needs it)
            else max_index = index_max(src, isz, count, restart);
        } else if (native) {  // 8-bit indices: widened to 16
            ib = vk_transient((uint64_t)count * 2 + 4, 4);
            uint16_t *w = ib.ptr;
            const uint8_t *s = src;
            for (uint32_t i = 0; i < count; i++) { w[i] = s[i] == 0xFF && st.prim_restart ? 0xFFFF : s[i]; if (s[i] > max_index && w[i] != 0xFFFF) max_index = s[i]; }
            itype_vk = VK_INDEX_TYPE_UINT16;
            icount = count;
        } else {
            uint32_t *e = expand_indices(mode, count, src, (int)itype, 0, &icount);
            for (uint32_t i = 0; i < icount; i++) if (e[i] > max_index) max_index = e[i];
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
        max_index += 1;
        if (base_vertex > 0) max_index += (uint32_t)base_vertex;
    }
    uint32_t chk_max = max_index;  // (MHGU_PERF_CHECK: the largest index as the old way finds it)
    if (perf_check && lazy_src) {
        chk_max = index_max(lazy_src, lazy_isz, count, lazy_restart) + 1;
        if (base_vertex > 0) chk_max += (uint32_t)base_vertex;
    }
    if (use_index && ss_need(SS_IB, ss.ib == ib.buf && ss.io == ib.off && ss.it == itype_vk)) {
        ss.ib = ib.buf; ss.io = ib.off; ss.it = itype_vk;
        vkCmdBindIndexBuffer(cb, ib.buf, ib.off, itype_vk);
    }

    // ---- vertex streams: the bound range (once per submission) when small, else what this draw can read
    for (int s = 0; s < 17; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        VkSpan sp;
        if (s == ZERO_BINDING) {
            static const float zero[4] = {0, 0, 0, 1};
            sp = vk_transient_copy(zero, 16, 16);
        } else {
            Range v = st.vbuf[s];
            uint64_t stride = k.stride[s], need;
            if (!stride) need = 256;
            else if (k.divisor[s]) need = ((uint64_t)base_inst + inst + k.divisor[s] - 1) / k.divisor[s] * stride + 256;
            else if (lazy_src && v.size && v.size <= (4u << 20)) need = v.size;  // (what the line below makes of any index)
            else {
                if (lazy_src) {  // (as the copy above would have found it)
                    max_index = index_max(lazy_src, lazy_isz, count, lazy_restart) + 1;
                    if (base_vertex > 0) max_index += (uint32_t)base_vertex;
                    lazy_src = NULL, stat_lazy_scans++;
                }
                need = (uint64_t)max_index * stride + 256;
            }
            if (v.size && (need > v.size || v.size <= (4u << 20))) need = v.size;
            if (perf_check && perf.lazyidx && stride && !k.divisor[s]) {  // (the old way's)
                uint64_t old = (uint64_t)chk_max * stride + 256;
                if (v.size && (old > v.size || v.size <= (4u << 20))) old = v.size;
                stat_check_n++;
                if (old != need) check_bad("a vertex stream's copy size");
            }
            VkBuffer cached;
            if (!v.addr || !guest_range_ok(v.addr, need)) { static const uint8_t zero[256]; sp = vk_transient_copy(zero, 256, 16); }
            else if (v.size && guest_range_ok(v.addr, v.size) && vcache(v.addr, v.size, &cached)) sp = (VkSpan){cached, 0, NULL};
            else sp = guest_copy(v.addr, need ? need : 16);
        }
        VkDeviceSize off = sp.off;
        if ((ss.vb_known >> s & 1) && ss.vb[s] == sp.buf && ss.vo[s] == off) continue;
        ss.vb[s] = sp.buf; ss.vo[s] = off; ss.vb_known |= 1u << s;
        vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &sp.buf, &off);
    }

    if (g_dump_active) {
        int n = snprintf(dump_draw_info, sizeof dump_draw_info, "vs %llx fs %llx vp %.0f,%.0f %.0fx%.0f tex:",
                         (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
                         vp[0], vp[1], vp[2], vp[3]);
        for (int i = 0; i < pr->fi->ntex && n < 3600; i++) {
            NvnTexture *tx = shader_texture_object(1, &pr->fi->tex[i]);
            n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n, " [%d:%s%ux%u f%#x lv%u @%p]", i,
                          tx && tx == st.rt.color[0] ? "SELF " : "", tx ? tx->width : 0, tx ? tx->height : 0, tx ? tx->format : 0,
                          tx ? tx->levels : 0, (void *)tx);
            if (tx && tx->backend && getenv("MHGU_DUMP_VERBOSE")) {  // image mip count, sampler filter / LOD clamp
                ShaderTexture *stx = &pr->fi->tex[i];
                int slot = ((int)stx->handle_offset - 0x20) / 8;
                uint64_t h = (slot >= 0 && slot < 32) ? st.tex[1][slot] : 0;
                uint32_t tsc = (uint32_t)(h >> 20) & 0xFFF;
                NvnSamplerDesc *sd = st.spool && tsc < (uint32_t)st.spool->count ? &((NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;
                n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n, "{img lv %u, min %u lod %g..%g wrap %u,%u aniso %g border %g,%g,%g,%g}",
                              ((TexBackend *)tx->backend)->levels, sd ? sd->min_filter : 99, sd ? sd->lod_min : 0, sd ? sd->lod_max : 0,
                              sd ? sd->wrap[0] : 99, sd ? sd->wrap[1] : 99, sd ? sd->max_aniso : 0, sd ? sd->border[0] : 0,
                              sd ? sd->border[1] : 0, sd ? sd->border[2] : 0, sd ? sd->border[3] : 0);
            }
        }
        if (getenv("MHGU_DUMP_VERBOSE") && n < 3600) {
            n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n,
                          "\n    rt @%p wmask %x blend %x mode %u count %u cull %u a0 fmt %d off %u stride %u",
                          (void *)st.rt.color[0], k.write_mask[0], k.blend_on, mode, count, k.cull, k.attr_fmt[0],
                          k.attr_off[0], k.stride[k.attr_bind[0]]);
            n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n, " | depth %p fmt %d %ux%u test %d write %d op %d stencil %d | ncolor %u fmts %d,%d,%d samples %u a2c %d alphatest %d"
                          " fill %d restart %d ccw %d",
                          (void *)st.rt.depth, pass_depth, pass_w, pass_h, k.depth_test, k.depth_write, k.depth_op, k.stencil_test,
                          k.ncolor, pass_color[0], pass_ncolor > 1 ? pass_color[1] : 0, pass_ncolor > 2 ? pass_color[2] : 0, k.samples,
                          k.a2c, fkey.alpha_func, k.poly_fill, k.prim_restart, k.front_ccw);
            {  // compiler constants (cbuf 1) stored after the fragment code
                NvnShaderStage *fs = &st.program->stages[1];
                if (fs->code_addr && guest_range_ok(fs->code_addr + fs->const_offset, 16)) {
                    const float *f = (const float *)G2H((uint32_t)(fs->code_addr + fs->const_offset));
                    n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n, " cb1@+%x size %x (%g %g %g %g | %g %g %g %g)",
                                  (unsigned)fs->const_offset, (unsigned)fs->const_size, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
                }
            }
            for (int u = 0; u < 3 && n < 3600; u++) {  // the fragment stage's first uniform buffers
                Range ub = st.ubuf[1][u];
                if (!ub.addr || !guest_range_ok(ub.addr, 64)) continue;
                const float *f = (const float *)G2H((uint32_t)ub.addr);
                n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n, " cb%d@%llx(", u + 3, (unsigned long long)ub.addr);
                for (int q = 0; q < 16 && n < 3900; q++) n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n, q ? " %g" : "%g", f[q]);
                n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n, ")");
            }
            Range v = st.vbuf[k.attr_bind[0] < 16 ? k.attr_bind[0] : 0];
            if (v.addr && guest_range_ok(v.addr + k.attr_off[0], 64) && k.attr_fmt[0] == VK_FORMAT_R32G32B32A32_SFLOAT)
                for (uint32_t q = 0; q < 4 && n < 3800; q++) {
                    const float *f = (const float *)G2H((uint32_t)(v.addr + k.attr_off[0] + (uint64_t)(first + q) * k.stride[k.attr_bind[0]]));
                    n += snprintf(dump_draw_info + n, sizeof dump_draw_info - n, " (%.3g %.3g %.3g %.3g)", f[0], f[1], f[2], f[3]);
                }
        }
    }
    if (g_dump_active && getenv("MHGU_DUMP_DRAWS"))  // (debug) every draw's program and depth / stencil use
        LOG("[dump]   cdraw vs %llx fs %llx (code hash %016llx) count %u inst %u mode %u | z test %d write %d op %d stencil %d\n",
            (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
            (unsigned long long)prog_fs_hash(pr), count, inst, mode, k.depth_test, k.depth_write, k.depth_op, k.stencil_test);
    if (g_dump_active && getenv("MHGU_DUMP_DRAWS")) {
        for (int i = 0; i < pr->fi->ntex; i++) {
            NvnTexture *tx = shader_texture_object(1, &pr->fi->tex[i]);
            LOG("[dump]     tex %d: %ux%u f%#x @%p\n", i, tx ? tx->width : 0, tx ? tx->height : 0, tx ? tx->format : 0, (void *)tx);
        }
        LOG("[dump]     target @%p blend %x wmask %x func %02x %02x %02x %02x\n", st.rt.ncolor ? (void *)st.rt.color[0] : NULL, k.blend_on, k.write_mask[0],
            ((uint8_t *)k.blend[0])[0], ((uint8_t *)k.blend[0])[1], ((uint8_t *)k.blend[0])[2], ((uint8_t *)k.blend[0])[3]);
    }
    if (g_dump_active && !st.rt.ncolor && getenv("MHGU_DUMP_VERBOSE")) {  // (debug) depth-only passes: every draw
        Range v0 = st.vbuf[k.attr_bind[0] < 16 ? k.attr_bind[0] : 0];
        // (with its polygon offset - factor, units - and how its alpha cuts it: alpha to coverage, the alpha test's function)
        LOG("[dump]   draw vs %llx vp %.0f,%.0f %.0fx%.0f sc %d,%d %dx%d count %u inst %u vb %llx clamp %d bias %g %g a2c %d"
            " atest %d\n",
            (unsigned long long)st.program->stages[0].code_addr, vp[0], vp[1], vp[2], vp[3], st.scissor[0][0], st.scissor[0][1],
            st.scissor[0][2], st.scissor[0][3], count, inst, (unsigned long long)v0.addr, k.depth_clamp,
            k.depth_bias ? st.poly_offset[0] : 0.0f, k.depth_bias ? st.poly_offset[1] : 0.0f, k.a2c, fkey.alpha_func);
    }
    {  // (debug) MHGU_CB_WATCH=<fragment code address>: that shader's first fragment uniform buffer, logged every 30 draws
        static uint64_t watch = ~0ull, nwatch;
        if (watch == ~0ull) { const char *e = getenv("MHGU_CB_WATCH"); watch = e ? strtoull(e, NULL, 16) : 0; }
        if (watch && st.program->stages[1].code_addr == watch && nwatch++ % 30 == 0) {
            Range ub = st.ubuf[1][0];
            if (ub.addr && guest_range_ok(ub.addr, 32)) {
                const float *f = (const float *)G2H((uint32_t)ub.addr);
                LOG("[cbwatch] %llx: %g %g %g %g | %g %g %g %g\n", (unsigned long long)ub.addr, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
            }
        }
    }
    if (!use_index) vkCmdDraw(cb, count, inst, first, base_inst);
    else vkCmdDrawIndexed(cb, icount, inst, 0, base_vertex, base_inst);
    stat_draws++;
    lost_draw();
    if (g_fsr_objvel && pr->fsrcap && (k.depth_write || (k.depth_test && pr->vi->cbuf_dynamic)) && inst == 1 && pass_samples == 1) {
        // FSR object motion: the same draw again through the capture variant of its vertex shader (rasterizer
        // discard), which stores each vertex's clip position for the velocity pass (fsr.c)
        FsrCapture fc = {.color0 = st.rt.ncolor ? st.rt.color[0] : NULL, .depth = st.rt.depth, .vs_code = st.program->stages[0].code_addr,
                         .used_streams = k.used_streams, .iaddr = indexed ? r->x[3] : 0, .mode = mode, .count = count,
                         .first = first, .base_vertex = base_vertex, .idx = use_index ? ib.ptr : NULL, .itype = itype_vk,
                         .gidx = indexed && native && r->x[1] != 0 ? G2H((uint32_t)r->x[3]) : NULL,
                         .icount = icount, .attr_mask = k.attr_mask, .topology = k.topology, .restart = k.prim_restart,
                         .rp = pass_rp, .depth_write = k.depth_write, .skinned = pr->vi->cbuf_dynamic != 0,
                         .vubuf = &st.ubuf[0][0].addr};
        for (int s = 0; s < 16; s++) fc.vbuf[s] = (k.used_streams >> s & 1) ? st.vbuf[s].addr : 0;
        memcpy(fc.attr_fmt, k.attr_fmt, sizeof fc.attr_fmt), memcpy(fc.attr_off, k.attr_off, sizeof fc.attr_off);
        memcpy(fc.attr_bind, k.attr_bind, sizeof fc.attr_bind), memcpy(fc.stride, k.stride, sizeof fc.stride);
        memcpy(fc.divisor, k.divisor, sizeof fc.divisor);
        VkPipeline cpipe;
        if (vk_fsr_capture(&fc, pr->fsrcap, &cpipe, &fsr_cap_base)) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, cpipe);
            ss.known &= SS_IB;  // (a pipeline without dynamic state: the next draw sets it all again)
            VkDescriptorSet s0 = stage_set(pr->fsrcap->nbinds, pr->fsrcap->binds, pr->fsrcap->set0, 0, 0, pr->vi, 0, 0, 0, 0, NULL);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->fsrcap->layout, 0, 1, &s0, 0, NULL);
            ss.known &= ~SS_SETS;  // (not draw's sets any more)
            if (!use_index) vkCmdDraw(cb, count, 1, first, base_inst);
            else vkCmdDrawIndexed(cb, icount, 1, 0, base_vertex, base_inst);
        }
    }
    if (at.pending >= 0) at_drawn();  // (MHGU_PERF_SHADOW_CLEAR: a clearing draw being checked, read back)
}

// ================================================================== texture copies
static void copy_region_dims(NvnTexture *t, const TexRef *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    if (!*fi || !(*fi)->bpb) return;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRef *dst) {
    snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    vk_end_pass();
    st.rt_dirty = true;
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row = 0, img = 0;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    if (perf.getenv ? perf_tex_log : getenv("MHGU_TEX_LOG") != NULL) {
        static int nlog;
        if (nlog++ < 40) LOG("[copy] buffer %llx -> tex fmt %#x %ux%u region %d,%d %dx%d lv %d row %llu img %llu (strides set %llu/%llu)\n",
                             (unsigned long long)src, t->format, t->width, t->height, rg[0], rg[1], rg[3], rg[4], level,
                             (unsigned long long)row, (unsigned long long)img, (unsigned long long)st.copy_row_stride,
                             (unsigned long long)st.copy_image_stride);
    }
    TexBackend *b = vk_tex(t, false);  // prior CPU contents uploaded first
    vk_ms_resolve(b);
    b->ms_valid = false;
    renderer_mark_gpu_owned(t);
    if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) || b->astc_bw || b->bcn) return;
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint32_t out_bpb = b->wide ? 8 : fi->bpb == 3 ? 4 : fi->bpb;  // (wide: RGBA8 texels widened to RGBA16)
    uint32_t nz = (uint32_t)rg[5];
    if (!guest_range_ok(src, img * nz)) return;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t out_row = (uint64_t)wb * out_bpb;
    VkSpan s = vk_transient(out_row * hb * nz, 16);
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < hb; y++) {
            const uint8_t *sr = (const uint8_t *)G2H((uint32_t)(src + (uint64_t)z * img + (uint64_t)y * row));
            uint8_t *dr = (uint8_t *)s.ptr + ((uint64_t)z * hb + y) * out_row;
            if (out_bpb == fi->bpb) memcpy(dr, sr, out_row);
            else if (b->wide) for (uint32_t x = 0; x < wb * 4; x++) ((uint16_t *)dr)[x] = (uint16_t)(sr[x] * 257);
            else for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
        }
    VkBufferImageCopy c = {.bufferOffset = s.off, .bufferRowLength = wb * fi->bw, .bufferImageHeight = hb * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

static void copy_texture_to_buffer(const TexRef *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst) return;
    vk_end_pass();
    st.rt_dirty = true;
    if (t->backend) vk_ms_resolve(t->backend);
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row = 0, img = 0;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (g_hudtrace_active)  // (debug, "hudtrace")
        LOG("[hud] copy to buffer: tex fmt %#x %ux%u region %d,%d,%d %dx%dx%d lv %d row %llu img %llu -> %llx\n", t->format,
            t->width, t->height, rg[0], rg[1], rg[2], rg[3], rg[4], rg[5], level, (unsigned long long)row,
            (unsigned long long)img, (unsigned long long)dst);
    if (!fi || !fi->bpb || fi->bpb == 3 || row % fi->bpb) return;
    TexBackend *b = vk_tex(t, false);
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t nz = (uint32_t)rg[5];
    uint64_t total = img * nz;
    if (!guest_range_ok(dst, total)) return;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkSpan out = vk_readback(b->wide ? total * 2 : total);  // (wide: RGBA16 read back, narrowed to the guest's RGBA8)
    VkBufferImageCopy c = {.bufferOffset = out.off, .bufferRowLength = (uint32_t)(row / fi->bpb) * fi->bw,
                           .bufferImageHeight = (uint32_t)(img / row) * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_submit();  // the CPU may read the result right after the fence
    vk_wait_site(W_READBACK);
    vk_wait_seq(submit_seq);
    if (b->wide) {
        uint8_t *d = (uint8_t *)G2H((uint32_t)dst);
        for (uint64_t i = 0; i < total; i++) d[i] = (uint8_t)((((const uint16_t *)out.ptr)[i] + 128) / 257);
    } else memcpy(G2H((uint32_t)dst), out.ptr, total);
}

static void copy_texture(const TexRef *src, const TexRef *dst, uint64_t flags) {
    snapshots_invalidate(NULL);
    fill_copy_flush();  // (MHGU_PERF_FXAA_COPYDRAW: its copy, done first)
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    if (g_dump_active) LOG("[dump] copy %p -> %p\n", (void *)st_, (void *)dt);
    if (g_hudtrace_active)  // (debug, "hudtrace")
        LOG("[hud] copy tex fmt %#x %ux%u (%d,%d %dx%d) -> fmt %#x %ux%u (%d,%d %dx%d) flags %llx\n", st_->format, st_->width,
            st_->height, src->region[0], src->region[1], src->region[3], src->region[4], dt->format, dt->width, dt->height,
            dst->region[0], dst->region[1], dst->region[3], dst->region[4], (unsigned long long)flags);
    vk_end_pass();
    st.rt_dirty = true;
    TexBackend *s = vk_tex(st_, false), *d = vk_tex(dt, true);
    vk_ms_resolve(s);
    vk_ms_resolve(d);
    d->ms_valid = false;
    renderer_mark_gpu_owned(dt);
    const int32_t *sr = src->region, *dr = dst->region;
    int sx0 = sr[0], sy0 = sr[1], dx0 = dr[0], dy0 = dr[1];
    int sw = sr[3] ? sr[3] : (int)s->width, sh = sr[4] ? sr[4] : (int)s->height;
    int dw = dr[3] ? dr[3] : (int)d->width, dh = dr[4] ? dr[4] : (int)d->height;
    uint32_t slv = src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0;
    uint32_t dlv = dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0;
    uint32_t sly = src->has_view && (src->view.setmask & 2) ? src->view.min_layer : 0;
    uint32_t dly = dst->has_view && (dst->view.setmask & 2) ? dst->view.min_layer : 0;
    if (slv >= s->levels || dlv >= d->levels || sly >= s->layers || dly >= d->layers) return;
    if (s->mip_skip || d->mip_skip) return;  // (textures only sampled: not copied by the game)
    {   // regions stay inside their levels (out-of-bounds copies / blits are undefined in Vulkan)
        int slw = (int)level_extent(s->width, slv), slh = (int)level_extent(s->height, slv);
        int dlw = (int)level_extent(d->width, dlv), dlh = (int)level_extent(d->height, dlv);
        if (!sr[3]) sw = slw - sx0;
        if (!sr[4]) sh = slh - sy0;
        if (!dr[3]) dw = dlw - dx0;
        if (!dr[4]) dh = dlh - dy0;
        if (sx0 + sw > slw) sw = slw - sx0;
        if (sy0 + sh > slh) sh = slh - sy0;
        if (dx0 + dw > dlw) dw = dlw - dx0;
        if (dy0 + dh > dlh) dh = dlh - dy0;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    }
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    lost_rec[cur_sub].copies++;
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        return;
    }
    if (s->samples != d->samples) return;
    {   // (a depth format the GPU can't copy or blit, textures.c depth_format: left out, never an invalid command)
        bool ds = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        bool same = sw == dw && sh == dh && s->format == d->format;
        VkFormatFeatureFlags fs = ds ? vk_format_features(s->format) : 0, fd = ds ? vk_format_features(d->format) : 0;
        if (ds && !(same ? (fs & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) && (fd & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)
                         : (fs & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (fd & VK_FORMAT_FEATURE_BLIT_DST_BIT))) {
            static int nlog;
            if (nlog++ < 4)
                LOG("[vk] depth %s %d -> %d left out: the GPU can't do it with that format\n", same ? "copy" : "blit", s->format, d->format);
            return;
        }
        if (ds) depth_copy_note(same ? "copy" : "blit", sw, sh, dw, dh, s->format);
    }
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// ================================================================== presentation helper (device.c)
VkRenderPass vk_window_pass(void);
int vk_window_turn(void);
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

static VkPipeline present_pipeline(void) {
    static VkPipeline p;
    static VkRenderPass made_for;
    VkRenderPass rp = vk_window_pass();
    if (p && made_for == rp) return p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("present.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = present_layout, .renderPass = rp};
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    made_for = rp;
    return p;
}

// Scanout shows the bytes the game wrote: sRGB images are read through a UNORM view (no decode).
static VkImageView raw_view(TexBackend *b) {
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    static const VkComponentMapping id = {0};
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// Inside the window image's render pass (device.c): the game image into (vx, vy, vw, vh).
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    VkPipeline p = present_pipeline();
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(b), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    ss.known &= ~SS_SETS;  // (not draw's sets any more)
    VkViewport vp = {vx, vy, vw, vh, 0, 1};
    // (the window image drawn turned for the panel, device.c MHGU_PERF_PREROTATE: the rectangle turned with it, and the
    // shader samples the picture turned back)
    int turn = vk_window_turn();
    float W = (float)g_vk.sc_extent.width, H = (float)g_vk.sc_extent.height;
    if (turn == 1) vp = (VkViewport){H - vy - vh, vx, vh, vw, 0, 1};
    else if (turn == 3) vp = (VkViewport){vy, W - vx - vw, vh, vw, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    float pv[8] = {nvn_window_origin() == 0 ? 1.0f : 0.0f, (float)turn, 0, 0, 0, 0, 1, 1};  // (flip, turn, crop origin / size)
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ================================================================== executor
void nvn_sync_set_seq(uint64_t guest, uint64_t seq);

static void exec_list(uint64_t handle, int depth) {
    uint64_t p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)G2H((uint32_t)p);
        void *x = r->extra;
        cur_rec = p;
        exec_pos[depth] = p, exec_top = depth;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) exec_call_i[depth] = i, exec_list(h[i], depth + 1);
            exec_top = depth;
            break;
        }
        case CMD_BIND_PROGRAM: st.program = (NvnProgram *)(uintptr_t)r->x[0]; break;
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: memcpy(st.color_state, x, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE: memcpy(st.ms, x, 0x18); break;
        case CMD_BIND_POLYGON: memcpy(st.poly, x, 4); break;
        case CMD_BIND_VERTEX_ATTRIB:
            st.nattrib = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.attrib, x, st.nattrib * 4);
            break;
        case CMD_BIND_VERTEX_STREAM:
            st.nstream = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.stream, x, st.nstream * 8);
            break;
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            const RtRec *rt = x;
            RtState prev = st.rt;  // the same targets again: keep the pass
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = (int)r->x[0] > 8 ? 8 : (int)r->x[0];
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;
            if (memcmp(&prev, &st.rt, sizeof prev)) st.rt_dirty = true;
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(st.poly_offset, r->f, sizeof st.poly_offset); break;
        case CMD_SET_ALPHA_REF: st.alpha_ref = r->f[0]; break;
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_DEPTH_CLAMP:
            st.depth_clamp = r->x[0] != 0;
            if (perf.getenv ? perf_dump_verbose : getenv("MHGU_DUMP_VERBOSE") != NULL) {
                static int n, last = -1;
                if (st.depth_clamp != last && n++ < 20) LOG("[vk] depth clamp -> %d\n", st.depth_clamp);
                last = st.depth_clamp;
            }
            break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            snapshots_invalidate(NULL);
            int index = (int)r->x[0];
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            break;
        }
        case CMD_CLEAR_TEXTURE: {  // a render-target clear of the texture alone
            const ClearTexRec *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snapshots_invalidate(NULL);
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            vk_end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            vk_end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            snapshots_invalidate(NULL);
            if (st.rt.depth) {
                bool stencil = renderer_format_has_stencil(st.rt.depth->format);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], stencil && r->x[2] != 0, (uint32_t)r->x[1], (uint32_t)r->x[2]);
            }
            break;
        case CMD_CLEAR_BUFFER: {  // (address, size, 32-bit value): CPU-side, in order with the list
            uint64_t a = r->x[0], n = r->x[1] & ~3ull;
            uint32_t v = (uint32_t)r->x[2];
            if (guest_range_ok(a, n)) {
                vk_upload_jobs_guard(a, n, "ClearBuffer");  // (texture bands queued before it read what was there first)
                for (uint64_t i = 0; i < n; i += 4) ST32((uint32_t)(a + i), v);
            }
            gcopy_gen++;
            break;
        }
        case CMD_DISPATCH: dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]); break;
        case CMD_DRAW_ARRAYS: draw(r, false); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            if (guest_range_ok(r->x[0] + r->x[2], r->x[3])) {
                vk_upload_jobs_guard(r->x[0] + r->x[2], r->x[3], "UpdateUniformBuffer");
                memcpy(G2H((uint32_t)(r->x[0] + r->x[2])), x, r->x[3]);
            }
            gcopy_gen++;  // (copies shared within the submission are stale now)
            break;
        case CMD_FENCE_SYNC:
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            break;
        case CMD_REPORT_COUNTER: {  // (type, address): {0, timestamp in GPU ticks (614.4 MHz)}
            {
                static uint32_t seen;
                uint32_t ty = (uint32_t)r->x[0];
                if (ty < 32 && !(seen & (1u << ty))) { seen |= 1u << ty; LOG("[nvn] ReportCounter type %u\n", ty); }
            }
            if (!r->x[1] || !guest_range_ok(r->x[1], 16)) break;
            uint64_t now = plat_now_ns();
            uint64_t ticks = (uint64_t)((unsigned __int128)now * 384 / 625);
            vk_upload_jobs_guard(r->x[1], 16, "ReportCounter");
            ST64((uint32_t)r->x[1], 0);
            ST64((uint32_t)r->x[1] + 8, ticks);
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: { const TexRef *t = x; copy_texture(&t[0], &t[1], r->x[6]); break; }
        case CMD_DOWNSAMPLE: { const TexRef *t = x; copy_texture(&t[0], &t[1], 1); break; }
        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER:  // (src, dst, size): guest memory only the CPU writes here
            if (guest_range_ok(r->x[0], r->x[2]) && guest_range_ok(r->x[1], r->x[2])) {
                vk_upload_jobs_guard(r->x[1], r->x[2], "CopyBufferToBuffer");
                memmove(G2H((uint32_t)r->x[1]), G2H((uint32_t)r->x[0]), r->x[2]);
            }
            gcopy_gen++;
            break;
        default: break;
        }
        p += r->size;
    }
}

void backend_execute(uint64_t handle) {
#ifdef __ANDROID__
    perf_hint_thread(PERF_SUBMIT);  // (the translating thread, for the performance hint session: platform/android_perf.c)
#endif
    vk_lock();
#ifndef __ANDROID__
    frame_dt_note();  // (the frame interval: the wait budget's)
#endif
    plist_drain();  // (the pipeline list's programs the prewarm met)
    uint64_t t0 = plat_now_ns();
    exec_list(handle, 0);
    stat_exec_ns += plat_now_ns() - t0;
    if (HITCH_ON) hitch_add(HC_EXEC, plat_now_ns() - t0, 0);
    renderer_submit();
    vk_unlock();
    static int stats = -1;
    if (stats < 0) stats = getenv("MHGU_VK_STATS") != NULL;
    static uint64_t last, frame, frame_t;
    uint64_t now = plat_now_ns();
    if (vk_frame_counter() != frame) {  // (the first submission of each frame: the longest gap is a stutter)
        if (frame_t && now - frame_t > stat_frame_max) stat_frame_max = now - frame_t;
        frame = vk_frame_counter(), frame_t = now;
    }
    if (stats && now - last > 2000000000ull) {
        last = now;
        LOG("[vk] 2s: vcache %llu hits %llu copies | %llu draws, %llu skipped, %llu alpha to coverage, %llu passes (%llu cleared as they began), %llu sets (%llu reused), %llu dispatches, %llu snapshots, %d compiles | exec %.0f ms (uploads %.0f) gpu wait %.0f ms, copied %.1f MB\n",
            (unsigned long long)stat_vc_hits, (unsigned long long)stat_vc_refresh,
            (unsigned long long)stat_draws, (unsigned long long)stat_skipped, (unsigned long long)stat_a2c,
            (unsigned long long)stat_passes, (unsigned long long)stat_noload,
            (unsigned long long)(stat_set_new + stat_set_reused), (unsigned long long)stat_set_reused,
            (unsigned long long)stat_dispatches, (unsigned long long)stat_snapshots, atomic_exchange(&stat_compiles, 0),
            stat_exec_ns / 1e6, stat_upload_ns / 1e6, stat_wait_ns / 1e6, stat_copy_bytes / 1048576.0);
        extern uint64_t stat_tex_hashed, stat_tex_net, stat_tex_untracked, stat_tex_changed, stat_tex_streamed, stat_tex_samples;
        LOG("[vk] 2s copied: vertex cache %.1f MB whole, %.1f MB written pages, %.1f MB safety net (%.0f MB held, %llu draws past "
            "it) | %.1f MB for one submission"
            " | textures hashed whole %.1f MB (no write reported, the safety net: %.1f; untracked %.1f; changed %.1f),"
            " copied at once %.1f MB, %llu samplings\n",
            stat_vc_whole_bytes / 1048576.0, stat_vc_pages_bytes / 1048576.0, stat_vc_net_bytes / 1048576.0,
            atomic_load_explicit(&vc_bytes_seen, memory_order_relaxed) / 1048576.0, (unsigned long long)stat_vc_full,
            stat_gcopy_bytes / 1048576.0, stat_tex_hashed / 1048576.0, stat_tex_net / 1048576.0, stat_tex_untracked / 1048576.0,
            stat_tex_changed / 1048576.0, stat_tex_streamed / 1048576.0, (unsigned long long)stat_tex_samples);
        stat_vc_whole_bytes = stat_vc_pages_bytes = stat_vc_net_bytes = stat_gcopy_bytes = stat_vc_full = 0;
        stat_tex_hashed = stat_tex_net = stat_tex_untracked = stat_tex_changed = stat_tex_streamed = stat_tex_samples = 0;
        {
            static const char *const site[W_SITES] = {"other", "ring", "finish", "syncwait", "flush", "present", "readback"};
            char line[600];
            int o = snprintf(line, sizeof line, "[vk] 2s waits: %llu submits |", (unsigned long long)atomic_exchange(&stat_submits, 0));
            for (int i = 0; i < W_SITES; i++) {
                uint64_t n = atomic_exchange(&stat_w_n[i], 0), ns = atomic_exchange(&stat_w_ns[i], 0), dr = atomic_exchange(&stat_w_drain[i], 0);
                if (n && o < (int)sizeof line - 60)
                    o += snprintf(line + o, sizeof line - o, " %s %llu x %.0f ms (%llu drains)", site[i], (unsigned long long)n, ns / 1e6,
                                  (unsigned long long)dr);
            }
            LOG("%s\n", line);
        }
        if (stat_skipped || stat_waits || stat_prog_cached || atomic_load(&stat_job_n[0]) || atomic_load(&stat_job_n[2])) {
            double ms[3], mx[3];
            uint64_t n[3];
            for (int i = 0; i < 3; i++) {
                n[i] = atomic_exchange(&stat_job_n[i], 0);
                ms[i] = n[i] ? atomic_exchange(&stat_job_ns[i], 0) / 1e6 / n[i] : 0;
                mx[i] = atomic_exchange(&stat_job_max[i], 0) / 1e6;
            }
            uint64_t ln = atomic_exchange(&stat_lat_n, 0);
            double lat = ln ? atomic_exchange(&stat_lat_ns, 0) / 1e6 / ln : 0, lmax = atomic_exchange(&stat_lat_max, 0) / 1e6;
            LOG("[vk] 2s builds: %llu programs from the cache | skipped %llu (program %llu, pipeline %llu) in %llu frames | waited %.0f ms in %llu waits, %llu "
                "ready in time | translate %llu x %.1f ms (max %.0f), spirv %llu x %.1f (max %.0f), pipeline %llu x %.1f "
                "(max %.0f) | needed -> ready %.0f ms avg, %.0f max | slowest frame %.1f ms | prewarm %llu new, %llu cached, %llu queued\n",
                (unsigned long long)stat_prog_cached, (unsigned long long)stat_skipped, (unsigned long long)stat_skip_prog,
                (unsigned long long)stat_skip_pipe, (unsigned long long)stat_skip_frames, stat_waited_ns / 1e6, (unsigned long long)stat_waits,
                (unsigned long long)stat_rescued, (unsigned long long)n[0], ms[0], mx[0], (unsigned long long)n[1], ms[1],
                mx[1], (unsigned long long)n[2], ms[2], mx[2], lat, lmax, stat_frame_max / 1e6,
                (unsigned long long)atomic_exchange(&stat_pw_new, 0), (unsigned long long)atomic_exchange(&stat_pw_known, 0),
                (unsigned long long)atomic_load(&stat_pw_left));
        }
        if (perf.rodepth || perf.shadow_clear || perf.fxaa_copydraw || fill_used)
            LOG("[perf] 2s: %llu passes with depth read-only (%llu ended for a depth or stencil write) | shadow atlas: %llu passes "
                "begun cleared, %llu clearing draws left out | %llu scalings or copies drawn in the next pass\n",
                (unsigned long long)stat_ro_passes, (unsigned long long)stat_ro_left, (unsigned long long)stat_atlas_clear,
                (unsigned long long)stat_atlas_skipped, (unsigned long long)stat_fills);
        stat_ro_passes = stat_ro_left = stat_atlas_clear = stat_atlas_skipped = stat_fills = 0;
        stat_prog_cached = stat_skip_prog = stat_skip_pipe = stat_skip_frames = stat_waited_ns = stat_waits = stat_rescued = stat_frame_max = 0;
        stat_draws = stat_skipped = stat_passes = stat_noload = stat_dispatches = stat_snapshots = stat_a2c = 0;
        stat_set_new = stat_set_reused = 0;
        stat_exec_ns = stat_wait_ns = stat_copy_bytes = stat_upload_ns = stat_vc_hits = stat_vc_refresh = 0;
        if (perf_check)
            LOG("[perf] 2s check: %llu compared, %llu differed\n", (unsigned long long)stat_check_n, (unsigned long long)stat_check_bad);
        stat_check_n = 0;
        if (perf.pipememo || perf.idxcopy || perf.lazyidx || perf.dynubo)  // (MHGU_PERF_*: what the switches on did)
            LOG("[perf] 2s: %llu draws took the last draw's pipeline, indices copied %.1f MB (%llu draws shared a copy), the "
                "largest index found %llu times, %llu binds with dynamic offsets\n", (unsigned long long)stat_pipememo,
                stat_idx_bytes / 1048576.0, (unsigned long long)stat_idx_shared, (unsigned long long)stat_lazy_scans,
                (unsigned long long)stat_dyn_binds);
        stat_pipememo = stat_idx_bytes = stat_idx_shared = stat_lazy_scans = stat_dyn_binds = 0;
    }
}
