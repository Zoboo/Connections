#pragma once

#include "platform/platform.h"

typedef enum {
    SCREEN_MAIN_MENU,
    SCREEN_WORDS,
    SCREEN_CUSTOM,
    SCREEN_CREATE,
    SCREEN_HISTORY,
    SCREEN_MULTIPLAYER,
    SCREEN_EXIT
} ScreenId;

typedef struct {
    ScreenId next_screen;
} ScreenResult;
