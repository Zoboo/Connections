#pragma once
#include "screen.h"
void multiplayer_screen_select_transport(bool local);
void multiplayer_screen_init(void);
ScreenResult multiplayer_screen_update(PlatformInput input);
void multiplayer_screen_draw(void);
void multiplayer_screen_exit(void);
void multiplayer_screen_shutdown(void);
const char *multiplayer_screen_profile_scenario(void);
