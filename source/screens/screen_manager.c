#include "screen_manager.h"

#include "custom_screen.h"
#include "create_screen.h"
#include "history_screen.h"
#include "main_menu_screen.h"
#include "words_screen.h"
#include "multiplayer_screen.h"

static ScreenId current_screen = SCREEN_MAIN_MENU;
static bool should_exit = false;

static void enter_screen(ScreenId screen) {
    if (screen == SCREEN_MULTIPLAYER) {
        multiplayer_screen_init();
    } else if (screen == SCREEN_MAIN_MENU) {
        main_menu_screen_init();
    } else if (screen == SCREEN_WORDS) {
        words_screen_init();
    } else if (screen == SCREEN_CUSTOM) {
        custom_screen_init();
    } else if (screen == SCREEN_CREATE) {
        create_screen_init();
    } else if (screen == SCREEN_HISTORY) {
        history_screen_init();
    }
}

static void exit_screen(ScreenId screen) {
    if (screen == SCREEN_MULTIPLAYER) {
        multiplayer_screen_exit();
    } else if (screen == SCREEN_MAIN_MENU) {
        main_menu_screen_exit();
    } else if (screen == SCREEN_WORDS) {
        words_screen_exit();
    } else if (screen == SCREEN_CUSTOM) {
        custom_screen_exit();
    } else if (screen == SCREEN_CREATE) {
        create_screen_exit();
    } else if (screen == SCREEN_HISTORY) {
        history_screen_exit();
    }
}

void screen_manager_init(void) {
    current_screen = SCREEN_MAIN_MENU;
    should_exit = false;

    enter_screen(current_screen);
}

void screen_manager_update(PlatformInput input) {
    ScreenResult result;

    if (current_screen == SCREEN_MULTIPLAYER) {
        result = multiplayer_screen_update(input);
    } else if (current_screen == SCREEN_MAIN_MENU) {
        result = main_menu_screen_update(input);
    } else if (current_screen == SCREEN_WORDS) {
        result = words_screen_update(input);
    } else if (current_screen == SCREEN_CUSTOM) {
        result = custom_screen_update(input);
    } else if (current_screen == SCREEN_CREATE) {
        result = create_screen_update(input);
    } else if (current_screen == SCREEN_HISTORY) {
        result = history_screen_update(input);
    } else {
        result = (ScreenResult){ SCREEN_EXIT };
    }

    if (result.next_screen == SCREEN_EXIT) {
        should_exit = true;
        return;
    }

    if (result.next_screen != current_screen) {
        exit_screen(current_screen);
        current_screen = result.next_screen;
        enter_screen(current_screen);
    }
}

void screen_manager_draw(void) {
    if (current_screen == SCREEN_MULTIPLAYER) {
        multiplayer_screen_draw();
    } else if (current_screen == SCREEN_MAIN_MENU) {
        main_menu_screen_draw();
    } else if (current_screen == SCREEN_WORDS) {
        words_screen_draw();
    } else if (current_screen == SCREEN_CUSTOM) {
        custom_screen_draw();
    } else if (current_screen == SCREEN_CREATE) {
        create_screen_draw();
    } else if (current_screen == SCREEN_HISTORY) {
        history_screen_draw();
    }
}

bool screen_manager_should_exit(void) {
    return should_exit;
}

void screen_manager_exit(void) {
    exit_screen(current_screen);
}

const char *screen_manager_profile_scenario(void) {
    switch (current_screen) {
        case SCREEN_MAIN_MENU: return "menu";
        case SCREEN_WORDS: return "puzzle";
        case SCREEN_CUSTOM: return "custom";
        case SCREEN_CREATE: return "create";
        case SCREEN_HISTORY: return "history";
        case SCREEN_MULTIPLAYER: return multiplayer_screen_profile_scenario();
        default: return "transition";
    }
}
