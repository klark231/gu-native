#pragma once
#include <stdbool.h>
// Called only by the overlay's game-thread frame builder.
void settings_panel(bool *open);
// (game thread) the menu is closed: its state let go, nothing drawn (the overlay, before its ImGui frame begins)
void settings_panel_idle(void);
// (game thread) the credits are on the screen now or about to be: the overlay draws a frame for them
bool settings_panel_credits_up(void);
#ifdef __cplusplus
struct ImDrawData;
// After ImGui::Render, the same thread: what of the menu is fading out (a desktop's: closed, it takes no input) goes into
// the frame's draw data; while something is, settings_panel_fading says so and the overlay keeps drawing frames
void settings_panel_after_render(ImDrawData *draw_data);
bool settings_panel_fading(void);
#endif

void settings_panel_request_close(void); // Thread-safe request; consumed by the game frame.
bool settings_panel_confirming(void);
// Any thread, C too: a confirmation the menu opens at its next frame (a desktop's dialogs answer on their own thread:
// runtime/pc_actions.c)
enum { SETTINGS_VIEW_IMPORT_SAVE = 1, SETTINGS_VIEW_RESTART = 2 };
#ifdef __cplusplus
extern "C"
#endif
void settings_panel_request_view(int view);

void settings_panel_before_frame(bool open); // Own navigation keys before ImGui NewFrame.
void settings_panel_request_back(void); // F1 / chord: same one-level back route.
bool settings_panel_dirty(void); // Atomic for the host close-event handler.
int settings_panel_theme(void); // THEME_*: the one picked in the menu while it is open, else the saved one (game frame).
bool settings_panel_key(int scancode); // Choosing a key (keybind, chat): takes the key pressed (host event thread).
bool settings_panel_mouse(int button); // Choosing a keybind: takes the mouse button pressed away from the menu's rows.
bool settings_panel_wheel(int notches); // Choosing a keybind: takes the wheel turned (> 0 up) as Wheel Up / Down.
// (game frame) The thank-you is open: the overlay counts it as a menu open, so every input goes to it, none to the game.
// (settings_panel_credits_up covers it too: the overlay draws frames while it waits after the credits, shows or fades out)
bool settings_panel_modal(void);
