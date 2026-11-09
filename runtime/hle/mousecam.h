// Mouse look on the hunt camera's own angles (runtime/hle/mousecam.c)
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { WEAPON_BOW = 1, WEAPON_BOWGUN };  // (runtime/hle/weapon.c)
uint32_t weapon_hunter(void);
int weapon_drawn(void);  // 1 the weapon out (being drawn too), 0 sheathed, -1 not known (no hunter)
int weapon_ranged(uint32_t hunter);
bool weapon_aiming(uint32_t hunter);
bool weapon_moving(uint32_t hunter);
bool player_place(float *x, float *z);  // the local player's place (x, z), a town's walker too
unsigned weapon_action(uint32_t hunter);  // st << 16 | no
int weapon_bow_type(void);
void mousecam_lock_shot(void);         // (each update of the hunter, hle/itembar.c)

void mousecam_init(void);              // hooks the quest camera's stick handler
bool mousecam_active(void);            // the hunt camera takes the mouse now (its stick handler ran just now)
void mousecam_add(float dx, float dy);  // counts the mouse moved (x right, y down; any thread)
