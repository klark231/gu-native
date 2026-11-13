// The mouse wheel on the item bar (runtime/hle/itembar.c)
#pragma once
#include <stdbool.h>

void itembar_init(void);         // hooks the player's item bar handler
void itembar_scroll(int steps);  // wheel notches: < 0 the item to the left, > 0 to the right (any thread)
void itembar_hold(bool open);    // the bar held open, its neighbouring items shown: asked for again each update (any thread)
