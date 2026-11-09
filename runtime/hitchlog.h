// (debug) MHGU_HITCHLOG=1: slow frames and what ran during them (runtime/hitchlog.c). Off (the default), each
// measurement point costs a load and a branch that is never taken.
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int g_hitch_on;  // set once by hitch_init (main.c), before the game's threads start
#define HITCH_ON __builtin_expect(g_hitch_on != 0, 0)

// The threads whose frames are timed: each calls hitch_frame once a frame, always from that same thread.
enum {
    HT_UPDATE,  // the game's update thread (forge.c frame_update: the game's whole frame, its frame limiter included)
    HT_RENDER,  // the thread that presents (nvn/vulkan/device.c backend_present)
    HT_INPUT,   // the window's event loop (ui.c ui_run, the SDL main thread): one pass
    HT_COUNT
};

// Counters, at the measurement points named: how many, their time, and an amount (bytes unless noted). A hitch line
// shows the ones that moved during the slow frame, on any thread. (The names hitchlog.c prints are in its table.)
enum {
    HC_EXEC,          // command lists translated (exec.c backend_execute)
    HC_BUILD_WAIT,    // draws waiting for a shader / pipeline build (exec.c await_build); amount: skipped all the same
    HC_BUILD_LONG,    // the same, the long wait of a movie frame or of the quest photo's frames
    HC_BUILD_SKIP,    // draws skipped at once: the frame's wait budget was spent (exec.c await_build)
    HC_BUILD_COLD,    // draws skipped at once, not waited for: their build is a whole compile (a desktop: a pipeline
                      //   first made at a draw, a program not loading from the program cache; exec.c await_build)
    HC_BUILD_READY,   // builds a draw asked for: request -> ready (exec.c set_state)
    HC_PIPE_NEW,      // pipelines a draw needed that no cache knew (exec.c draw, pipe_ahead; the time: recording it)
    HC_PROG_NEW,      // programs a draw needed that must be translated (exec.c get_program)
    HC_PROG_LOAD,     // programs a draw needed, dormant in the program cache: loaded (exec.c prog_want)
    HC_JOB_XLATE,     // shader translations (exec.c job_stat: the translate thread)
    HC_JOB_PROGRAM,   // programs built: SPIR-V, modules, layout (workers)
    HC_JOB_PIPE,      // graphics pipelines built (workers; the driver's compile)
    HC_SPIRV_DISK,    // SPIR-V read from cache/spirv (shaders.c)
    HC_SPIRV_COMPILE, // GLSL compiled by shaderc (shaders.c; amount: the source)
    HC_PIPE_SYNC,     // the renderer's own pipelines, made on the render thread (exec.c sync_pipeline)
    HC_CS_NEW,        // compute shaders translated on the render thread (exec.c compute_program)
    HC_TEX_CREATE,    // images made for textures (textures.c tex_backend)
    HC_TEX_UPLOAD,    // textures uploaded from guest memory (textures.c vk_tex: the render thread's part)
    HC_TEX_HASH,      // textures hashed whole, no upload needed
    HC_TEX_JOB,       // uploads' bands hashed and copied into staging memory (textures.c: the workers, or the render
                      //   thread at the submission or a guard; amount: guest bytes)
    HC_TEX_WAIT,      // a submission (or a guard) waiting for the workers' last bands (textures.c vk_upload_jobs_finish)
    HC_TEX_GUARD,     // a CPU write in a command list over bands still to be copied: all of them done first (textures.c
                      //   vk_upload_jobs_guard; label: the command; its time includes those tex-job and tex-wait)
    HC_TEX_DESTROY,   // textures the game destroyed (textures.c backend_texture_destroyed)
    HC_DEAD,          // Vulkan objects destroyed once the GPU was done with them (textures.c; amount: objects)
    HC_VKALLOC,       // vkAllocateMemory, through VMA (device.c; desktop: the start-up reserve's blocks too)
    HC_VKFREE,        // vkFreeMemory, through VMA
    HC_VC_NEW,        // vertex cache: new entries (exec.c vcache)
    HC_VC_COPY,       // vertex cache: whole ranges copied (new entries, writes too scattered for a partial refresh)
    HC_VC_SAFETY,     // vertex cache: the safety net's slices, 1 MB a frame at most (Android 2 MB; Windows: off)
    HC_CHUNK,         // new staging memory chunks (exec.c new_chunk; desktop: the start-up ones too)
    HC_DESC_POOL,     // descriptor pools made (exec.c vk_alloc_set)
    HC_DESC_RESET,    // descriptor pools reset, a ring slot's at a time (exec.c sub; amount: pools)
    HC_GPU_OTHER,     // GPU waits (exec.c vk_wait_seq), by site, in exec.c's order: other,
    HC_GPU_RING,      //   the submission ring,
    HC_GPU_FINISH,    //   nvnQueueFinish,
    HC_GPU_SYNC,      //   nvnSyncWait,
    HC_GPU_FLUSH,     //   renderer_flush,
    HC_GPU_FRAME,     //   the frames in flight (before acquiring a window image),
    HC_GPU_READBACK,  //   a copy back to guest memory
    HC_SUBMIT,        // vkQueueSubmit (exec.c vk_submit_extra)
    HC_ACQUIRE,       // vkAcquireNextImageKHR (device.c frame_begin)
    HC_PRESENT,       // vkQueuePresentKHR (device.c frame_end)
    HC_SWAPCHAIN,     // swapchain made again (device.c recreate_swapchain)
    HC_VKLOCK,        // waits for the renderer's lock, when another thread held it (device.c vk_lock)
    HC_KLOCK,         // waits for the guest kernel's lock, when another thread held it (kernel.c)
    HC_PRIO_LEND,     // guest mutexes' owners raised to a waiter's priority: the time each ran so (Windows, kernel.c prio_lend)
    HC_OVERLAY_TEX,   // ImGui texture uploads: they end with vkQueueWaitIdle (overlay.cpp)
    HC_IPC,           // service requests, all of them (ipc.c; the slowest one's service and command)
    HC_FILE_READ,     // game file reads (romfs.c read_file; the slowest one's file)
    HC_FILE_OPEN,     // game files opened (romfs.c: the descriptor cache missed)
    HC_FILE_LOCK,     // waits for that cache's lock
    HC_LOG,           // log lines (rt.h LOG, rt_logf): the logging thread's time (Windows: formatting and queuing it)
    HC_LOG_WRITE,     // the log's writes to stderr, by its own thread (Windows, logwriter.c; amount: the bytes)
    HC_LOG_DROP,      // log lines dropped: its queue was full (amount: their bytes)
    HC_MEM_COMMIT,    // guest memory backed on its first touch, 64 KB each (platform/windows.c)
    HC_VSYNC_LATE,    // the guest's vsync tick woke over 1 ms late (hle/vi.c)
    HC_SELF,          // this logger's own lines
    HC_COUNT
};

void hitch_init(void);
uint64_t hitch_now(void);  // (plat_now_ns, for C++ callers)
void hitch_add(int counter, uint64_t ns, uint64_t amount);
// label (copied): what the slowest of them was (a service, a file); num: its command (HC_IPC) or KB (HC_TEX_UPLOAD)
void hitch_add_label(int counter, uint64_t ns, uint64_t amount, const char *label, int num);
// The calling thread's frame ended (step: the game's frame step, HT_UPDATE; else 0).
void hitch_frame(int thread, float step);
// (kernel.c) a guest kernel wait of the calling thread ended: kind 1 mutex, 2 condvar, 3 arbiter, 4 WaitSynchronization,
// 5 SleepThread; cpu: its Cpu (where it waited)
struct Cpu;
void hitch_kwait(uint64_t ns, int kind, const struct Cpu *cpu);
// m locked, the wait timed as counter when another thread held it
void hitch_mutex_lock(pthread_mutex_t *m, int counter);
int hitch_logf(const char *fmt, ...);  // (rt.h LOG while on: timed)

#ifdef __cplusplus
}
#endif
