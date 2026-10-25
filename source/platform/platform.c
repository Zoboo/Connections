#include "platform.h"
#include "platform_save.h"
#include <switch.h>

static PadState pad;
static bool was_touching = false;

void platform_init(void) {
    romfsInit();
    platform_save_init();
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
}

void platform_update(void) {
    padUpdate(&pad);
}

void platform_exit(void) {
    platform_save_exit();
    romfsExit();
}

PlatformInput platform_get_input(void) {
    u64 held = padGetButtons(&pad);
    u64 down = padGetButtonsDown(&pad);
    HidTouchScreenState touch_state = {0};
    bool is_touching = false;

    PlatformInput input = {0};

    input.a      = down & HidNpadButton_A;
    input.b      = down & HidNpadButton_B;
    input.start  = down & HidNpadButton_Plus;
    input.select = down & HidNpadButton_Minus;
    input.up     = held & HidNpadButton_Up;
    input.down   = held & HidNpadButton_Down;
    input.left   = held & HidNpadButton_Left;
    input.right  = held & HidNpadButton_Right;

    if (hidGetTouchScreenStates(&touch_state, 1) > 0 && touch_state.count > 0) {
        is_touching = true;
        input.touch_down = true;
        input.touch_x = touch_state.touches[0].x;
        input.touch_y = touch_state.touches[0].y;

        if (!was_touching) {
            input.touch = true;
        }
    }

    was_touching = is_touching;

    return input;
}
