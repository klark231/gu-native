// The game's DLC from folders of the player's (services/dlc.c)
#pragma once
#include <stdint.h>

#include "../romfs.h"

#define DLC_BASE_ID 0x0100770008DD9000ull  // (aoc:u GetAddOnContentBaseId: a DLC's title id is this + its index)

int dlc_count(void);
int dlc_list(uint32_t offset, uint32_t *out, int max);  // indices from offset on; how many
RomfsImage *dlc_image(uint64_t data_id);                 // a DLC's image by its title id, or NULL
