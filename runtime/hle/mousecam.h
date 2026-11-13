// Mouse look on the hunt camera's own angles (runtime/hle/mousecam.c)
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { WEAPON_BOW = 1, WEAPON_BOWGUN };  // (runtime/hle/weapon.c)
uint32_t weapon_hunter(void);
int weapon_drawn(void);  // 1 the weapon out (being drawn too), 0 sheathed, -1 not known (no hunter)
int weapon_ranged(uint32_t hunter);
bool weapon_aiming(uint32_t hunter);
bool weapon_aim_action(uint32_t hunter);
bool weapon_gyro_aiming(uint32_t hunter, uint64_t now_ns);  // a bow or a bowgun aims: the gyro turns it (host_input.c)  // in an aim's actions, whatever camera is up (0.1.8's weapon_aiming)
bool weapon_moving(uint32_t hunter);
bool weapon_stance(uint32_t hunter);   // a bow or a gun out, standing or walking: no weapon move, no aim (a locked shot's)
bool weapon_prowler(uint32_t hunter);  // a Prowler: Support Moves on the item bar (L + Y / A)
bool player_place(float *x, float *z);  // the local player's place (x, z), a town's walker too
unsigned weapon_action(uint32_t hunter);  // st << 16 | no
int weapon_bow_type(void);
void mousecam_lock_shot(void);         // (each update of the hunter, hle/itembar.c)

void mousecam_init(void);              // hooks the quest camera's stick handler
bool mousecam_active(void);            // the hunt camera takes the mouse now (its stick handler ran just now)
bool mousecam_hunt_camera(void);       // its handler ran in one of the hunter's last two updates (any thread)
void mousecam_add(float dx, float dy);  // counts the mouse moved (x right, y down; any thread)
bool mousecam_lock_following(void);    // the lock-on turned the camera to its monster just now (any thread)
bool mousecam_lock_aimable(void);      // a bowgun's locked shot has a monster to aim at now (any thread)
int mousecam_control_type(void);       // the game's Control Type: 0 Type 1 .. 2 Type 3 (3: the fourth, as Type 3)
uint64_t mousecam_target_buttons(void);  // the buttons that change the target now, by the Control Type (BTN_* bits)
