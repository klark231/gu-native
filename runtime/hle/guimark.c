// The game's markers over a point of the world on the HUD (a villager's speech bubble, the quest icon over whoever gives
// or departs on the quest, and the other markers of that kind: main 0x7c65e8 .. 0x81ef50) are shown only while that
// point is on the screen. Their test, main 0x218dcc, takes the point as the projections at 0x7511a4 .. 0x751698 give
// it: in the HUD's 1280x720 canvas (the view's pixels x 1280 / its width, x 720 / its height). It compares it with the
// view's viewport, its +0x70 width and +0x74 height, which are pixels. On the console that viewport is 1280x720 or
// 1920x1080, never smaller than the canvas, so the test spanned the whole canvas. With a smaller 3D picture it spans only
// the canvas's top left part, as many units as the picture has pixels, and a marker past it was hidden: at 26 % of a
// 1600x720 screen (416x186) the bubbles and quest icons in the middle of the screen, below 50 % of a 720-line one those
// lower than a quarter of the way down, at 100 % of a 960x720 one those in the right quarter (#188, Discord). It came
// back for a few frames when the camera moved them up into that corner (sitting down to eat).
// Here the game's test is done as it was, with the viewport never smaller than the canvas it is compared with: as the
// console in handheld mode below 1280x720, unchanged at or above it. The test's other callers pass canvas points too.
#include <stdint.h>

#include "../hle.h"

#define FN_ON_SCREEN 0x218dccu  // r0 the view, r1 the point (x, y, z, w floats), r2 0: x may pass the right edge by s0;
                                // s0, s1 the margins left / above -> r0 1 when the point is on the screen
#define VIEW_VP_W    0x70u      // the view's viewport, pixels (int)
#define VIEW_VP_H    0x74u
#define CANVAS_W     1280       // the HUD's canvas (the projections' 1280.0 / 720.0)
#define CANVAS_H     720

static float f_of(uint32_t u) {
    union { uint32_t u; float f; } v = {u};
    return v.f;
}

// The game's instructions, in C (NaN compares as its vcmp + condition codes do: GE and LS false, HI true)
static void on_screen(Cpu *c) {
    int32_t iw = (int32_t)LD32(c->r[0] + VIEW_VP_W), ih = (int32_t)LD32(c->r[0] + VIEW_VP_H);
    float w = (float)(iw > CANVAS_W ? iw : CANVAS_W), h = (float)(ih > CANVAS_H ? ih : CANVAS_H);
    float mx = c->v.f[0], my = c->v.f[1];
    uint32_t p = c->r[1];
    float pw = f_of(LD32(p + 12)), x, y;
    c->r[0] = 0;
    if (pw <= 0.0f) return;  // (behind the camera)
    x = f_of(LD32(p));
    if (!c->r[2]) w += mx;
    if (x >= w || x <= -mx) return;
    y = f_of(LD32(p + 4));
    if (y >= h) return;
    if (!(y <= -my)) c->r[0] = 1;
}

void guimark_init(void) {
    fnhook_set(FN_ON_SCREEN, hle_register("guimark:on_screen", on_screen));
}
