// Windows' own performance figures for the readout's PC lines ([Performance] ShowStats: runtime/perfstats.cpp), sampled
// about once a second on a thread of their own while it shows (runtime/platform/win_stats.c)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One pass's figures; NaN: not known (not measured yet, or this system or driver does not give it)
typedef struct {
    // the GPU the game runs on
    float gpu_temp;                 // its temperature, degrees C
    float gpu_use;                  // its busiest engine's use by every process, percent (Task Manager's GPU figure)
    float gpu_ghz;                  // its 3D engine's clock
    float vram_gb, vram_budget_gb;  // this process's video memory, and the budget the system gives it
    // the CPU
    float cpu_use;                  // this process's share of all the logical processors, percent
    float cpu_ghz;                  // the processors' real clock
    float ram_gb;                   // this process's working set
    float game_ms, render_ms;       // CPU time per presented frame of the game's update thread and of the presenting one
    float game_busy, render_busy;   // their share of a core (near 1: that thread holds the frame rate back)
} WinStats;

// host main thread (perfstats_poll), each loop pass: 0 none (the sampler idles), 1 the GPU's temperature, 2 everything
void winstats_level(int level);
// game thread (perfstats_draw): the last snapshot's number (0: none, or none for some seconds), and the snapshot into
// *out when that number is not have. Never waits for the sampler: while it is writing, have comes back.
uint32_t winstats_get(WinStats *out, uint32_t have);

#ifdef __cplusplus
}
#endif
