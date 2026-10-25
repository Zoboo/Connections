#pragma once

#include <stdbool.h>

void platform_graphics_init(void);
void platform_graphics_shutdown(void);
bool platform_graphics_modern(void);
bool platform_graphics_set_modern(bool modern);
