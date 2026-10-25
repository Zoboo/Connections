#pragma once

#include <stdbool.h>

bool platform_audio_init(void);
void platform_audio_exit(void);
void platform_audio_play_bloop(void);
void platform_audio_play_pop(void);
