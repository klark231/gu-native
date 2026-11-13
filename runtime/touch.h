// Touch controls (phones): a floating stick on the left, the camera turned by dragging anywhere else, and the game's
// buttons in the Monster Hunter style around the right thumb (runtime/touch.cpp)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#include "hud_parts.h"  // (the HUD's parts: HP_*, where they go)

#ifdef __cplusplus
extern "C" {
#endif

// host main thread (runtime/host_input.c)
bool touch_event(const SDL_Event *e);  // a finger event: true when it was the controls'
void touch_input(bool pad_connected, bool typing, uint64_t *buttons, int *lx, int *ly, float *cam_dx, float *cam_dy);
// after touch_input: a Hunter Art tapped that the player's controls perform with the right stick (Control Type 4, or R
// Stick Controls swapped) sets the right stick for a moment
void touch_right_stick(int *rx, int *ry);
// the item bar turned a step (a desktop's wheel, ItemLeft / ItemRight; dir < 0 left): a hunter's items, a Prowler's
// Support Moves (L + Y / A)
void touch_item_step(int dir);
// (MHGU_PERF_INPUT_WAKE, runtime/ui.c) a finger down, or something of theirs going on by the clock (a timed press, the
// locked run, the lock-on, a locked shot, the item bar held open, the editor): the window loop keeps its pace
bool touch_busy(void);
// game thread (runtime/overlay.cpp): drawn over the game, under the port's menus
bool touch_visible(void);
// (something of them is on the screen: hidden, the settings button alone, or the lock-on's padlock by the target camera's
// icon while it is on, whatever the player plays with; or the HUD editor)
bool touch_drawn(void);
void touch_draw(float display_w, float display_h);
// (MHGU_PERF_OVERLAY_REUSE, runtime/overlay.cpp) touch_draw keeps what each frame is made of from now on; and whether the
// controls would draw the very same lists as in the frame drawn last (nothing of theirs moving by itself)
void touch_reuse_begin(void);
bool touch_draw_unchanged(float display_w, float display_h);
void touch_edit_begin(void);  // the layout editor (settings menu: Controls), once the menu is closed
void touch_edit_cancel(void);  // (the settings menu's unsaved-changes dialog cancelled: it stays open)
// an editor is open, the touch layout's or a desktop's HUD editor: a controller's B cancels it (host_input.c)
bool touch_editor_open(void);
// the game is held (the settings menu or the layout editor open, playing alone: overlay.cpp): the HUD it drew last stays
void touch_game_held(bool on);
// the game held for the layout editor: a part of the HUD dragged since its last step, it takes another (overlay.cpp)
bool touch_editor_step(void);
void touch_editor_cancel(void);
#ifndef __ANDROID__
// The HUD editor (a desktop's, worked with the mouse): every part of the HUD (runtime/hud_parts.h) moved, sized and
// hidden from where the game puts them with the touch controls hidden (settings menu: Display, Move the HUD), once the
// menu is closed. Kept in touch_layout.txt (pc_hud_*: HUD_PLAIN, which the touch layout editor edits too)
void touch_hud_edit_begin(void);
// open, asked for, or the press that closed it still down: the mouse is its own, free and shown (runtime/host_input.c)
bool touch_hud_editing(void);
void touch_hud_pad(bool read);     // a controller is read, any (host_input.c): its B shows on Cancel once it is used
// host main thread, while it is open: a mouse event, or Esc (cancel); true when it took it (fingers: touch_event)
bool touch_edit_mouse(const SDL_Event *e);
#endif
void touch_lock_toggle(void);  // the lock-on (a controller's right stick click in a hunt: runtime/host_input.c)
bool touch_lock_engaged(void);  // the lock-on is on, menus and pauses aside (hle/mousecam.c keeps its monster meanwhile)
// the lock-on, each update, given every button pressed now (of every input: a press that can open one of the game's
// menus counts for the menu check) and whether the camera is being turned (host_input.c); no buttons of its own: 0
uint64_t touch_lock_buttons(uint64_t buttons, bool cam_input);
// a locked shot, after touch_lock_buttons: given every button pressed now, the ones the game gets (with the lock-on on,
// a bowgun's shot is aimed first: R held, the fire button held back until the aim; host_input.c)
uint64_t touch_lock_shot(uint64_t buttons, bool still);  // (still: no stick from any input)
// one of the game's menus is open (the item box, a shop, the title...: their cursor moves with the d-pad), as the touch
// controls tell it for their menu pad, whether they show or not (updated by touch_input, each update)
bool touch_game_menu(void);
// the buttons the menu pad presses now (its fingers, its taps, a drag's steps in a menu), from the last touch_input, not
// in its buttons: host_input.c stages those it alone presses (input_stage_menu_only), which the game reads only while a
// menu takes input
uint64_t touch_menu_buttons(void);
void touch_reset(void);  // every finger gone (the app went to the background: runtime/ui.c)
void touch_test_window(int ms);  // (tests, the control file's fakewindow) a menu's window seen for that long, no press
// while shown ([Touch] MoveHUD On), the HUD's arrangement is the touch controls' (HUD_TOUCH): out of the thumbs' way by
// default, the status to the left edge, the item bar to the bottom middle, the Hunter Arts and the target camera to
// the top right (nvn/vulkan/exec.c); else the plain one (HUD_PLAIN: a controller's, a keyboard's)
extern volatile int g_touch_hud;
// Where the HUD's parts (HP_*) are drawn, so a finger on them works them and the editors frame them: a tap on the item
// bar uses the item and a swipe turns it, a tap on a Hunter Art's panel performs it (the target camera's icons take no
// taps). The render thread passes each piece of a part (the game's position, where it went on the 1280x720 canvas that
// covers the screen, the part's size, and flags: 1 a finger works it, 2 hidden: no finger; nvn/vulkan/exec.c cbuf_info),
// and closes each presented frame (device.c). Not moved (the controls hidden), the parts are passed where they are: the
// lock-on's padlock goes by the target camera's icons then.
void touch_hud_piece(int part, float game_x, float game_y, float canvas_x, float canvas_y, float k, int flags);
void touch_hud_frame(void);
void touch_hud_menu(void);  // (render thread) a menu window of the game's was drawn this frame: the menu pad, the
                            // lock-on waits
void touch_hud_front(void);  // (render thread) and it is the front end's (the title, the main menu): no menu pad there
// (render thread) a model drawn in a window of the game's this frame (the smithy's weapon, the item box's hunter: a
// depth-tested draw in a viewport smaller than the picture), which the game turns with the sticks
void touch_hud_preview(void);
// (any thread) where a part (HP_*) was drawn last, in display pixels, its art and a margin: x0, y0, x1, y1. Moved or
// not, the touch controls shown or not; false while it isn't drawn (no hunt, a menu over it, a cutscene)
bool touch_hud_zone(int part, float display_w, float display_h, float rect[4]);
// (any thread) a part's frame as last drawn, a finger on it or not (the minimap, the status), in display pixels, while
// it is drawn and not hidden: x0, y0, x1, y1
bool touch_hud_part_rect(int part, float display_w, float display_h, float rect[4]);

#ifdef __cplusplus
}
#endif
