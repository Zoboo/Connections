#pragma once

#include "screen.h"
#include "platform/platform.h"

void screen_manager_init(void);
void screen_manager_update(PlatformInput input);
void screen_manager_draw(void);
void screen_manager_exit(void);

bool screen_manager_should_exit(void);
const char *screen_manager_profile_scenario(void);
