#pragma once

#include "screen.h"

void create_screen_init(void);
ScreenResult create_screen_update(PlatformInput input);
void create_screen_draw(void);
void create_screen_exit(void);
