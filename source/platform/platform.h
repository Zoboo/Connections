#pragma once

#include <switch.h>
#include <stdbool.h>

typedef struct {
    bool a;
    bool b;
    bool start;
    bool select;
    bool up;
    bool down;
    bool left;
    bool right;
    bool touch;
    bool touch_down;
    int touch_x;
    int touch_y;
} PlatformInput;

void platform_init(void);
void platform_update(void);
void platform_exit(void);

PlatformInput platform_get_input(void);
