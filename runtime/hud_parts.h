// The parts of the game's HUD the player moves, sizes and hides (runtime/touch.cpp's editors), placed by the render
// thread (nvn/vulkan/exec.c cbuf_info) and by the editors where they estimate a part: both go through hud_place, so a
// part dragged follows the finger 1:1 on any shape of picture. C, for both
#pragma once
#include <stdbool.h>

#include "config.h"  // (g_render_aspect, g_ui_anchor_aspect)

#ifdef __cplusplus
extern "C" {
#endif

// The parts (their keys in touch_layout.txt: hud_<key>, pc_hud_<key>): the item bar (and its ammo or coating list), the
// Hunter Arts, the target camera, the clock, health and stamina, the sharpness and the gauges below them, the party's
// plates (the hunter's own first), the minimap. The status (layer 103 above the item bar) is split by place: measured
// with a Great Sword and two Felynes; a piece in none of the four's regions goes with the nearest (nvn/vulkan/exec.c)
enum { HP_ITEMS, HP_ARTS, HP_TARGET, HP_CLOCK, HP_BARS, HP_GAUGES, HP_PARTY, HP_MAP, HP_COUNT };
// Two arrangements of them: with the touch controls on the screen ([Touch] MoveHUD On), out of the thumbs' way by
// default, and without them (a controller, a keyboard; with MoveHUD Off the touch controls too), the game's by default
enum { HUD_TOUCH, HUD_PLAIN, HUD_PROFILES };

// Each part in each arrangement, from runtime/touch.cpp (the layout; the render thread reads them every piece, each
// field on its own: a change caught halfway lasts a frame): its offset from its default place in the game's 16:9
// canvas units (1280 x 720), its size about its middle (1: the game's), its opacity (1: not offered yet), hidden
extern volatile float g_hud_dx[HUD_PROFILES][HP_COUNT], g_hud_dy[HUD_PROFILES][HP_COUNT], g_hud_k[HUD_PROFILES][HP_COUNT];
extern volatile float g_hud_alpha[HUD_PROFILES][HP_COUNT];
extern volatile int g_hud_hidden[HUD_PROFILES][HP_COUNT];
// how far the render thread moved each part to keep its middle on the picture (an offset saved on another shape of
// picture, or with another HUD anchor), in the offset's units: an editor takes it in as the part is picked up
extern volatile float g_hud_shift_dx[HP_COUNT], g_hud_shift_dy[HP_COUNT];
// the arrangement an editor shows while it is open (HUD_*), -1 none: the one in use (the touch controls shown or not)
extern volatile int g_hud_edit_prof;
// (the render thread's) the arrangement it drew the last frame with, and the Hunter Arts' top row then (the game's y):
// the editors' estimates
extern volatile int g_hud_prof_now;
extern volatile float g_hud_arts_top;

static const char *const HUD_KEY[HP_COUNT] = {"items", "arts", "target", "clock", "bars", "gauges", "party", "map"};
// Measured on the 16:9 canvas, in the game's units, against shots (a piece's place is its own anchor point, not its
// middle: nothing here is a piece's place plus or minus half its size):
//   HUD_MID  the middle a part is sized about
//   HUD_BOX  where its art is by default (x0, y0, x1, y1): the editors' estimate where it isn't on the screen
//   HUD_PAD  how far its art reaches past its pieces' places (left, top, right, bottom), sized with it
// The minimap is framed by its box alone (HUD_FRAME_BOX): its picture is one piece at the box's middle (1114, 225, in
// both of its zooms), and its icons, which move over it, would widen a frame of their places past it
static const float HUD_MID[HP_COUNT][2] = {{1115.0f, 663.0f}, {833.0f, 663.0f}, {1200.0f, 56.0f}, {56.0f, 56.0f},
                                           {431.0f, 25.0f}, {153.0f, 48.0f}, {113.0f, 175.0f}, {1114.0f, 225.0f}};
static const float HUD_BOX[HP_COUNT][4] = {{992, 600, 1238, 712}, {709, 618, 957, 707}, {1170, 16, 1230, 95},
                                           {10, 15, 102, 98},     {100, 13, 763, 38},   {108, 36, 199, 60},
                                           {16, 104, 210, 247},   {986, 97, 1242, 353}};
static const float HUD_PAD[HP_COUNT][4] = {{61, 17, 85, 23}, {17, 13, 115, 11}, {30, 26, 30, 13}, {1, 1, 39, 43},
                                           {2, 7, 21, 5},    {2, 0, 83, 22},    {1, 1, 110, 9},   {0, 0, 0, 0}};
#define HUD_FRAME_BOX(part) ((part) == HP_MAP)
#define HUD_SLOP 10.0f  // (and a little more all round: a finger, a frame drawn round it)
// The touch controls' arrangement (HUD_TOUCH's defaults): the item bar to the bottom middle (x 992..1238 -> 517..763),
// the Hunter Arts to the top right (x 709..957 -> 1020..1268, from the screen's right edge; the top row's first piece
// at y HUD_ARTS_TOP: two rows, 631 -> 41), the target camera left of them (x 1200 -> 950), the status to the left edge
#define HUD_ITEM_BAR_DX (-475.0f)
#define HUD_ARTS_DX 311.0f
#define HUD_ARTS_TOP 41.0f
#define HUD_ARTS_TOP_GAME 631.0f  // (the game's y of the top row with two: the estimate's)
#define HUD_TARGET_DX (-250.0f)
// how far an offset may go, in canvas units: across a whole 32:9 picture and more, where the HUD is squeezed and a unit is
// smaller (the editors keep a part's middle on the picture anyway); the sizes
#define HUD_DX_MAX 8000.0f
#define HUD_DY_MAX 3000.0f
#define HUD_SIZE_MIN 0.5f
#define HUD_SIZE_MAX 2.0f

// The HUD keeps the 16:9 shape it is laid out in: on a wider picture its parts are narrowed (squeeze) and moved toward
// their side, on a narrower one (16:10) flattened (vsqueeze) and moved toward the top or the bottom
static inline float hud_squeeze(void) {
    float a = g_render_aspect;
    return a > 16.0f / 9.0f ? (16.0f / 9.0f) / a : 1.0f;
}
static inline float hud_vsqueeze(void) {
    float a = g_render_aspect;
    return a > 0 && a < 16.0f / 9.0f ? a / (16.0f / 9.0f) : 1.0f;
}

// Where a part goes, in one arrangement: hud_place_begin once, hud_place_piece for each of its pieces (the game's place
// -> the canvas). The default place is the game's anchored as exec.c anchors its layer (the touch controls' arrangement
// moves the part first), then the offset, the middle kept on the picture, the size about it
typedef struct {
    int part, prof;
    float s, sy, left, right;  // (the squeezes; the HUD's area: its left and right edge on the canvas)
    float arts_top;            // (the Hunter Arts' top row, the game's y: the touch arrangement takes it to HUD_ARTS_TOP)
    float dx, dy, k;
    bool hidden, custom;       // (custom: anything not the arrangement's default)
    float shift_x, shift_y;    // (what the last piece's middle was moved by, in the offset's units)
} HudPlace;
// (with the values given: hud_place_begin's are the layout's, as published)
static inline void hud_place_begin_with(HudPlace *p, int part, int prof, float arts_top, float dx, float dy, float k, bool hidden) {
    float ra = g_render_aspect > 0 ? g_render_aspect : 16.0f / 9.0f;
    float aa = g_ui_anchor_aspect > 0 ? g_ui_anchor_aspect : 16.0f / 9.0f;
    if (aa > ra) aa = ra;
    p->part = part, p->prof = prof;
    p->s = hud_squeeze(), p->sy = hud_vsqueeze();
    p->left = 640.0f * (1.0f - aa / ra), p->right = 1280.0f - p->left;
    p->arts_top = arts_top;
    p->dx = dx, p->dy = dy, p->k = k, p->hidden = hidden;
    p->custom = p->dx != 0.0f || p->dy != 0.0f || p->k != 1.0f || p->hidden;
    p->shift_x = p->shift_y = 0.0f;
}
static inline void hud_place_begin(HudPlace *p, int part, int prof, float arts_top) {
    hud_place_begin_with(p, part, prof, arts_top, g_hud_dx[prof][part], g_hud_dy[prof][part], g_hud_k[prof][part],
                         g_hud_hidden[prof][part] != 0);
}
static inline bool hud_custom(int part, int prof) {  // (anything not the arrangement's default)
    return g_hud_dx[prof][part] != 0.0f || g_hud_dy[prof][part] != 0.0f || g_hud_k[prof][part] != 1.0f || g_hud_hidden[prof][part];
}
// (its default place and offset, before the middle and the size; the same sums as the render thread's before parts
// were sized, so a part left alone lands where it always did)
static inline void hud_place_base(const HudPlace *p, float gx, float gy, float *x, float *y) {
    float s = p->s, sy = p->sy, dx = p->dx, dy = p->dy;
    bool touch = p->prof == HUD_TOUCH;
    switch (p->part) {
    case HP_ITEMS:  // (anchored right and bottom; the touch arrangement: the middle)
        if (touch) *x = 640.0f + (gx - 640.0f) * s, *x += (HUD_ITEM_BAR_DX + dx) * s;
        else *x = p->right - (1280.0f - gx) * s, *x += dx * s;
        *y = 720.0f - (720.0f - gy) * sy, *y += dy * sy;
        break;
    case HP_ARTS:  // (right and bottom; the touch arrangement: from the screen's right edge, the top)
        if (touch) *x = 1280.0f - (1280.0f - (gx + HUD_ARTS_DX + dx)) * s, *y = (gy - p->arts_top + HUD_ARTS_TOP) * sy;
        else *x = p->right - (1280.0f - gx) * s, *x += dx * s, *y = 720.0f - (720.0f - gy) * sy;
        *y += dy * sy;
        break;
    case HP_TARGET:  // (right and top; the touch arrangement: from the screen's right edge)
        if (touch) *x = 1280.0f - (1280.0f - (gx + HUD_TARGET_DX + dx)) * s;
        else *x = p->right - (1280.0f - gx) * s, *x += dx * s;
        *y = gy * sy, *y += dy * sy;
        break;
    case HP_MAP:  // (right and top)
        *x = p->right - (1280.0f - gx) * s, *x += dx * s;
        *y = gy * sy, *y += dy * sy;
        break;
    default:  // the status's (left and top; the touch arrangement: at the screen's left edge)
        *x = touch ? gx * s : p->left + gx * s, *x += dx * s;
        *y = gy * sy, *y += dy * sy;
        break;
    }
}
// a piece: where it goes on the canvas (the part's middle kept on the picture: every piece gets the same shift, so the
// part stays one)
static inline void hud_place_piece(HudPlace *p, float gx, float gy, float *x, float *y) {
    hud_place_base(p, gx, gy, x, y);
    float cx = *x + (HUD_MID[p->part][0] - gx) * p->s, cy = *y + (HUD_MID[p->part][1] - gy) * p->sy;
    float shift_x = cx < 0.0f ? -cx : cx > 1280.0f ? 1280.0f - cx : 0.0f;
    float shift_y = cy < 0.0f ? -cy : cy > 720.0f ? 720.0f - cy : 0.0f;
    *x += shift_x, *y += shift_y, cx += shift_x, cy += shift_y;
    p->shift_x = p->s > 0.0f ? shift_x / p->s : 0.0f, p->shift_y = p->sy > 0.0f ? shift_y / p->sy : 0.0f;
    if (p->k != 1.0f) *x = cx + (*x - cx) * p->k, *y = cy + (*y - cy) * p->k;
}

// A part's default box (HUD_BOX) where hud_place puts it now, on the canvas: x0, y0, x1, y1 (hud_box_with: as it would
// be with that offset and size)
static inline void hud_box_with(int part, int prof, float arts_top, float dx, float dy, float k, float out[4]) {
    HudPlace p;
    hud_place_begin_with(&p, part, prof, arts_top, dx, dy, k, false);
    hud_place_piece(&p, HUD_BOX[part][0], HUD_BOX[part][1], &out[0], &out[1]);
    hud_place_piece(&p, HUD_BOX[part][2], HUD_BOX[part][3], &out[2], &out[3]);
}
static inline void hud_box(int part, int prof, float arts_top, float out[4]) {
    hud_box_with(part, prof, arts_top, g_hud_dx[prof][part], g_hud_dy[prof][part], g_hud_k[prof][part], out);
}

#ifdef __cplusplus
}
#endif
