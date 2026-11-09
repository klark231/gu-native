// Performance readout ([Performance] ShowStats, runtime/perfstats.cpp): the frame rate, the frame time and, on a phone,
// its temperature (on a PC, the GPU's; Full: lines on the GPU and the CPU), in a small plate at the top of the screen
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void perfstats_poll(void);              // host main thread, each loop pass (runtime/ui.c): the phone's temperature, every 2 s
                                        // at most; a PC: its sampler told the level (runtime/platform/win_stats.c)
bool perfstats_visible(void);           // game thread (runtime/overlay.cpp): it is to be drawn this frame
void perfstats_draw(float w, float h);  // game thread, inside the overlay's ImGui frame (display size in pixels)
float perfstats_bottom(void);           // game thread: the plate's lower edge just drawn, in display pixels; 0: none shown

#ifdef __cplusplus
}
#endif
