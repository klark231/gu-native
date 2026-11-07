// Touch controls (phones): a floating stick on the left, the camera turned by dragging anywhere else, and the game's
// buttons in the Monster Hunter style around the right thumb (runtime/touch.cpp)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// host main thread (runtime/host_input.c)
bool touch_event(const SDL_Event *e);  // a finger event: true when it was the controls'
void touch_input(bool pad_connected, bool typing, uint64_t *buttons, int *lx, int *ly, float *cam_dx, float *cam_dy);
// game thread (runtime/overlay.cpp): drawn over the game, under the port's menus
bool touch_visible(void);
bool touch_drawn(void);  // (something of them is on the screen: the settings button alone, when they are hidden)
void touch_draw(float display_w, float display_h);
void touch_edit_begin(void);  // the layout editor (settings menu: Controls), once the menu is closed
void touch_edit_cancel(void);  // (the settings menu's unsaved-changes dialog cancelled: it stays open)
void touch_lock_toggle(void);  // the lock-on (a controller's right stick click in a hunt: runtime/host_input.c)
// the lock-on's L presses, given every button pressed now and whether the camera is being turned (host_input.c)
uint64_t touch_lock_buttons(uint64_t buttons, bool cam_input);
void touch_reset(void);  // every finger gone (the app went to the background: runtime/ui.c)
// while shown, the HUD moves out of the thumbs' way: the status to the left edge, the item bar to the bottom middle,
// the Hunter Arts and the target camera to the top right (nvn/vulkan/exec.c)
extern volatile int g_touch_hud;
// Where those moved HUD parts are drawn, so a finger on them works them: a tap on the item bar uses the item and a
// swipe turns it, a tap on a Hunter Art's panel performs it, a tap on the target camera's icon turns the lock-on on
// or off. The render thread passes each moved part's place (the game's position, and where it went on the 1280x720
// canvas that covers the screen: nvn/vulkan/exec.c cbuf_info), and closes each presented frame (device.c).
enum { TZ_ITEM, TZ_ARTS, TZ_TARGET, TZ_COUNT };
// where the player put them in the layout editor: an offset from their default place, in the game's 16:9 canvas
// units (1280 x 720); the render thread adds it (nvn/vulkan/exec.c)
extern volatile float g_touch_zone_dx[TZ_COUNT], g_touch_zone_dy[TZ_COUNT];
void touch_hud_piece(int zone, float game_x, float game_y, float canvas_x, float canvas_y);
void touch_hud_frame(void);
void touch_hud_menu(void);  // (render thread) a menu window of the game's was drawn this frame: the menu layout

#ifdef __cplusplus
}
#endif
