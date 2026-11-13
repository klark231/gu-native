// The mouse in the game's own menus (runtime/hle/mousemenu.c): a desktop's, a no-op on Android (but menu_pages_zl,
// menu_updates and menu_cursor_age)
#pragma once
#include <stdbool.h>
#include <stdint.h>

// hooks the game's pad, its two cursor classes, the menus' drawing objects and its dialogs (Android: only the pad, the
// cursors, the quantity and the choices of two, which menu_cursor_age counts by)
void mousemenu_init(void);
// (both platforms, any thread) the game's menus turn their pages with ZL / ZR, not L / R (an option of the game's)
bool menu_pages_zl(void);
// (both platforms, any thread) the player's controls in the game's Options: Control Type 0..3 (Type 1..4); L / R with ZL /
// ZR swapped (L/R Button Controls), the right stick with the d-pad (R Stick Controls)
int game_control_type(void);
bool game_lr_swapped(void);
bool game_rstick_swapped(void);
// (both platforms, any thread) the game's pad updates so far, and how many since the last one a menu's cursor (a list,
// a box), a quantity or a choice of two ran in: 0 in the last one (or the one under way), large before any. A menu
// takes the d-pad while one runs; a menu closing runs none (the touch controls' menu pad: runtime/touch.cpp)
uint32_t menu_updates(void);
uint32_t menu_cursor_age(void);
// (the game's thread, as it reads the controller: hle/input.c) a menu takes input now: a menu's cursor ran in the game's
// last update (the pad system reads the controller before the menus run), or the hunter is not played (menu_set_free)
bool menu_takes_input(void);
void menu_set_free(bool free);  // (the touch controls, each update) the hunter is not played: no d-pad is a Hunter Art

// host main thread (runtime/host_input.c)
bool mousemenu_active(void);              // a menu or a dialog of the game's has the mouse now (the setting on, one open)
bool mousemenu_open(void);                // a menu or a dialog of the game's is open now, the setting on or off
// a list whose left / right only the left stick moves, not the d-pad (cCursor's axes 6 and 7: a menu's tabs), runs now
bool menu_stick_sideways(void);
// where the pointer is, on the 1280x720 canvas the picture shows; moved: by the player (false: where it came back as
// a menu took the mouse, which points at nothing until it moves)
void mousemenu_pointer(float cx, float cy, bool moved);
void mousemenu_hide(void);                // a key or a controller was used: no hover until the mouse moves again
// a button pressed over (cx, cy): SDL_BUTTON_LEFT = A on the cell under it, else the game's A on what has the focus,
// RIGHT = B, X1 / X2 = the page buttons
void mousemenu_click(int button, float cx, float cy);
void mousemenu_wheel(int notches, float cx, float cy);  // (> 0 up)
void mousemenu_trace(int frames);         // (debug) "menutrace": the cursors, the menus' geometry, the pointer
void mousemenu_test_press(uint32_t buttons);  // (debug) "menupress": those menu buttons pressed for one update, anywhere
void mousemenu_test_stall(int ms);            // (debug) "menustall": the game's next update waits that long (a slow one)
// the port's own menu, the HUD editor, the chat or a text entry has the mouse (or not): nothing waiting reaches the game
void mousemenu_block(bool blocked);

// render thread (nvn/vulkan/exec.c, device.c): each menu piece of the frame, where the game put it on its 16:9 canvas
// and where it was drawn (moved on a wider or narrower picture), with the scale it was drawn at; the frame presented
int mousemenu_piece(float gx, float gy, float gz, float dx, float dy, float kx, float ky);
void mousemenu_piece_y(int piece, float dy);  // (16:10: its window's anchor settled once the window was complete)
void mousemenu_menu(void);   // a menu window of the game's in the frame (as the touch controls' test: touch_hud_menu)
void mousemenu_frame(void);
