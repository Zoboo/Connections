#include "create_screen.h"

#include "core/config.h"
#include "platform/platform_audio.h"
#include "platform/platform_save.h"
#include "platform/platform_graphics.h"
#include "rendering/colors.h"
#include "rendering/drawing.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#define CREATE_TEXT_FIELD_COUNT 5
#define CREATE_FIELD_COUNT 6
#define CREATE_FIELD_GROUP 0
#define CREATE_FIELD_DIFFICULTY 5
#define CREATE_ACTION_INDEX 6
#define CREATE_STAGE_COUNT 4
#define CREATE_NAV_DELAY_MS 160
#define CREATE_FIELD_X 330
#define CREATE_BASE_FIELD_Y 122
#define CREATE_FIELD_WIDTH 620
#define CREATE_FIELD_HEIGHT 42
#define CREATE_FIELD_GAP 10
#define CREATE_LABEL_X 190
#define CREATE_BUTTON_WIDTH 240
#define CREATE_BUTTON_HEIGHT 64
#define CREATE_BUTTON_Y 640
#define CREATE_OUTLINE_THICKNESS 4
#define CREATE_DISABLED_COLOR COLOR_GRAY
#define CREATE_MAX_INPUT_LENGTH (GAME_SAVE_CUSTOM_TEXT_LENGTH - 1)
#define CREATE_COMPLETED_ROW_X 240
#define CREATE_COMPLETED_ROW_Y 68
#define CREATE_COMPLETED_ROW_WIDTH 800
#define CREATE_COMPLETED_ROW_HEIGHT 48
#define CREATE_COMPLETED_ROW_GAP 10
#define CREATE_LEVEL_YELLOW 0xFF6DDFF9
#define CREATE_LEVEL_GREEN  0xFF5AC3A0
#define CREATE_LEVEL_BLUE   0xFFEFC4B0
#define CREATE_LEVEL_PURPLE 0xFFC581BA

static GameSaveCustomPuzzle draft;
static int current_stage = 0;
static int selected_index = 0;
static Uint32 last_nav_time = 0;
static bool can_press_a = false;

static int text_width(const char *text, int scale) {
    return drawing_text_width(text, scale);
}

static bool point_in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static const char *stage_title(void) {
    if (current_stage == 0)
        return "FIRST GROUP";

    if (current_stage == 1)
        return "SECOND GROUP";

    if (current_stage == 2)
        return "THIRD GROUP";

    return "FOURTH GROUP";
}

static const char *field_label(int field_index) {
    if (field_index == CREATE_FIELD_GROUP)
        return "GROUP";

    if (field_index == 1)
        return "#1";

    if (field_index == 2)
        return "#2";

    if (field_index == 3)
        return "#3";

    if (field_index == 4)
        return "#4";

    return "DIFFICULTY";
}

static char *field_value(int stage, int field_index) {
    if (field_index == CREATE_FIELD_GROUP)
        return draft.group_names[stage];

    return draft.words[stage][field_index - 1];
}

static const char *difficulty_name(int level) {
    if (level == 0)
        return "YELLOW";

    if (level == 1)
        return "GREEN";

    if (level == 2)
        return "BLUE";

    return "PURPLE";
}

static u32 difficulty_color(int level) {
    if (level == 0)
        return CREATE_LEVEL_YELLOW;

    if (level == 1)
        return CREATE_LEVEL_GREEN;

    if (level == 2)
        return CREATE_LEVEL_BLUE;

    return CREATE_LEVEL_PURPLE;
}

static bool stage_is_complete(void) {
    for (int i = 0; i < CREATE_TEXT_FIELD_COUNT; i++) {
        if (field_value(current_stage, i)[0] == '\0')
            return false;
    }

    return true;
}

static int form_start_y(void) {
    return CREATE_BASE_FIELD_Y + current_stage * (CREATE_COMPLETED_ROW_HEIGHT + CREATE_COMPLETED_ROW_GAP);
}

static int field_y(int field_index) {
    return form_start_y() + field_index * (CREATE_FIELD_HEIGHT + CREATE_FIELD_GAP);
}

static int touched_index(int px, int py) {
    for (int i = 0; i < CREATE_FIELD_COUNT; i++) {
        if (point_in_rect(px, py, CREATE_FIELD_X, field_y(i), CREATE_FIELD_WIDTH, CREATE_FIELD_HEIGHT))
            return i;
    }

    int button_x = (SCREEN_WIDTH - CREATE_BUTTON_WIDTH) / 2;
    if (point_in_rect(px, py, button_x, CREATE_BUTTON_Y, CREATE_BUTTON_WIDTH, CREATE_BUTTON_HEIGHT))
        return CREATE_ACTION_INDEX;

    return -1;
}

static void copy_keyboard_result(char *dest, const char *source) {
    strncpy(dest, source, GAME_SAVE_CUSTOM_TEXT_LENGTH);
    dest[GAME_SAVE_CUSTOM_TEXT_LENGTH - 1] = '\0';
}

static void cycle_difficulty(void) {
    draft.group_levels[current_stage] = (draft.group_levels[current_stage] + 1) % 4;
}

static void edit_selected_field(void) {
    if (selected_index == CREATE_FIELD_DIFFICULTY) {
        cycle_difficulty();
        return;
    }

    if (selected_index < 0 || selected_index >= CREATE_TEXT_FIELD_COUNT)
        return;

    SwkbdConfig keyboard;
    char output[GAME_SAVE_CUSTOM_TEXT_LENGTH] = {0};
    char guide[64];
    char *target = field_value(current_stage, selected_index);
    Result rc = swkbdCreate(&keyboard, 0);
    if (R_FAILED(rc))
        return;

    snprintf(guide, sizeof(guide), "Enter %s", field_label(selected_index));

    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, stage_title());
    swkbdConfigSetGuideText(&keyboard, guide);
    swkbdConfigSetOkButtonText(&keyboard, "OK");
    swkbdConfigSetStringLenMin(&keyboard, 1);
    swkbdConfigSetStringLenMax(&keyboard, CREATE_MAX_INPUT_LENGTH);

    if (target[0] != '\0')
        swkbdConfigSetInitialText(&keyboard, target);

    rc = swkbdShow(&keyboard, output, sizeof(output));
    if (R_SUCCEEDED(rc) && output[0] != '\0')
        copy_keyboard_result(target, output);

    swkbdClose(&keyboard);
}

static ScreenResult advance_or_submit(void) {
    if (!stage_is_complete())
        return (ScreenResult){ SCREEN_CREATE };

    if (current_stage < CREATE_STAGE_COUNT - 1) {
        current_stage++;
        selected_index = 0;
        return (ScreenResult){ SCREEN_CREATE };
    }

    GameSave save;
    if (!platform_save_load(&save))
        platform_save_defaults(&save);

    if (platform_save_add_custom_puzzle(&save, &draft))
        platform_save_write(&save);

    return (ScreenResult){ SCREEN_CUSTOM };
}

void create_screen_init(void) {
    memset(&draft, 0, sizeof(draft));
    for (int i = 0; i < CREATE_STAGE_COUNT; i++)
        draft.group_levels[i] = 0;

    current_stage = 0;
    selected_index = 0;
    last_nav_time = 0;
    can_press_a = false;
}

ScreenResult create_screen_update(PlatformInput input) {
    if (input.b)
        return (ScreenResult){ SCREEN_CUSTOM };

    if (!input.a)
        can_press_a = true;

    if (input.touch) {
        int touched = touched_index(input.touch_x, input.touch_y);
        if (touched >= 0) {
            platform_audio_play_pop();
            selected_index = touched;
            if (selected_index == CREATE_ACTION_INDEX)
                return advance_or_submit();

            edit_selected_field();
            can_press_a = false;
        }
    }

    Uint32 now = SDL_GetTicks();
    if ((input.up || input.down) && now - last_nav_time >= CREATE_NAV_DELAY_MS) {
        if (input.down)
            selected_index = (selected_index + 1) % (CREATE_ACTION_INDEX + 1);
        else
            selected_index = (selected_index + CREATE_ACTION_INDEX) % (CREATE_ACTION_INDEX + 1);

        platform_audio_play_pop();
        last_nav_time = now;
    }

    if (input.a && can_press_a) {
        platform_audio_play_pop();
        can_press_a = false;

        if (selected_index == CREATE_ACTION_INDEX)
            return advance_or_submit();

        edit_selected_field();
    }

    return (ScreenResult){ SCREEN_CREATE };
}

static int text_scale_to_fit(const char *text, int max_width, int max_scale, int min_scale) {
    for (int scale = max_scale; scale > min_scale; scale--) {
        if (text_width(text, scale) <= max_width)
            return scale;
    }

    return min_scale;
}

static void draw_centered_text(const char *text, int x, int y, int w, int h, int scale, u32 color) {
    int width = text_width(text, scale);
    drawing_text(text, x + (w - width) / 2, y + (h - 7 * scale) / 2, scale, color);
}

static void build_group_words_line(int group, char *out, size_t out_size) {
    out[0] = '\0';

    for (int member = 0; member < GAME_SAVE_CUSTOM_MEMBERS; member++) {
        if (member > 0)
            strncat(out, ", ", out_size - strlen(out) - 1);

        strncat(out, draft.words[group][member], out_size - strlen(out) - 1);
    }
}

static void draw_completed_group_row(int group) {
    int y = CREATE_COMPLETED_ROW_Y + group * (CREATE_COMPLETED_ROW_HEIGHT + CREATE_COMPLETED_ROW_GAP);
    char words_line[GAME_SAVE_CUSTOM_MEMBERS * GAME_SAVE_CUSTOM_TEXT_LENGTH];
    int title_scale = text_scale_to_fit(draft.group_names[group], CREATE_COMPLETED_ROW_WIDTH - 32, 2, 1);

    build_group_words_line(group, words_line, sizeof(words_line));
    int words_scale = text_scale_to_fit(words_line, CREATE_COMPLETED_ROW_WIDTH - 32, 1, 1);

    if (platform_graphics_modern())
        drawing_round_rect(CREATE_COMPLETED_ROW_X, y, CREATE_COMPLETED_ROW_WIDTH,
                           CREATE_COMPLETED_ROW_HEIGHT, 6, difficulty_color(draft.group_levels[group]));
    else drawing_rect(
        CREATE_COMPLETED_ROW_X,
        y,
        CREATE_COMPLETED_ROW_WIDTH,
        CREATE_COMPLETED_ROW_HEIGHT,
        difficulty_color(draft.group_levels[group])
    );
    draw_centered_text(draft.group_names[group], CREATE_COMPLETED_ROW_X, y + 4, CREATE_COMPLETED_ROW_WIDTH, 18, title_scale, COLOR_BLACK);
    draw_centered_text(words_line, CREATE_COMPLETED_ROW_X, y + 26, CREATE_COMPLETED_ROW_WIDTH, 14, words_scale, COLOR_BLACK);
}

static void draw_text_field(int field_index) {
    int y = field_y(field_index);
    const char *value = field_value(current_stage, field_index);
    bool selected = selected_index == field_index;
    int value_scale = text_scale_to_fit(value, CREATE_FIELD_WIDTH - 32, 3, 1);

    drawing_text(field_label(field_index), CREATE_LABEL_X, y + 12, 2, COLOR_BLACK);
    if (platform_graphics_modern()) {
        drawing_round_rect(CREATE_FIELD_X, y, CREATE_FIELD_WIDTH, CREATE_FIELD_HEIGHT, 6, 0xFFE6EFEF);
        drawing_round_rect_outline(CREATE_FIELD_X, y, CREATE_FIELD_WIDTH, CREATE_FIELD_HEIGHT,
                                   6, 2, selected ? 0xFFDC7328 : 0xFFAAA9A5);
    } else {
        drawing_rect(CREATE_FIELD_X, y, CREATE_FIELD_WIDTH, CREATE_FIELD_HEIGHT, COLOR_WHITE);
        drawing_rect_outline(
        CREATE_FIELD_X,
        y,
        CREATE_FIELD_WIDTH,
        CREATE_FIELD_HEIGHT,
        selected ? CREATE_OUTLINE_THICKNESS : 2,
        selected ? COLOR_BLACK : COLOR_GRAY
        );
    }

    if (value[0] != '\0')
        drawing_text(value, CREATE_FIELD_X + 16, y + (CREATE_FIELD_HEIGHT - 7 * value_scale) / 2, value_scale, COLOR_BLACK);
}

static void draw_difficulty_field(void) {
    int y = field_y(CREATE_FIELD_DIFFICULTY);
    bool selected = selected_index == CREATE_FIELD_DIFFICULTY;
    int level = draft.group_levels[current_stage];

    drawing_text(field_label(CREATE_FIELD_DIFFICULTY), CREATE_LABEL_X, y + 12, 2, COLOR_BLACK);
    if (platform_graphics_modern()) {
        drawing_round_rect(CREATE_FIELD_X, y, CREATE_FIELD_WIDTH, CREATE_FIELD_HEIGHT, 6, difficulty_color(level));
        drawing_round_rect_outline(CREATE_FIELD_X, y, CREATE_FIELD_WIDTH, CREATE_FIELD_HEIGHT,
                                   6, 2, selected ? 0xFFDC7328 : 0xFFAAA9A5);
    } else {
        drawing_rect(CREATE_FIELD_X, y, CREATE_FIELD_WIDTH, CREATE_FIELD_HEIGHT, difficulty_color(level));
        drawing_rect_outline(
        CREATE_FIELD_X,
        y,
        CREATE_FIELD_WIDTH,
        CREATE_FIELD_HEIGHT,
        selected ? CREATE_OUTLINE_THICKNESS : 2,
        selected ? COLOR_BLACK : COLOR_GRAY
        );
    }
    drawing_text(difficulty_name(level), CREATE_FIELD_X + 16, y + 10, 2, COLOR_BLACK);
    drawing_text("V", CREATE_FIELD_X + CREATE_FIELD_WIDTH - 34, y + 10, 2, COLOR_BLACK);

    if (selected) {
        int option_width = CREATE_FIELD_WIDTH / 4;
        int option_y = y + CREATE_FIELD_HEIGHT + 4;

        for (int i = 0; i < 4; i++) {
            int option_x = CREATE_FIELD_X + i * option_width;
            if (platform_graphics_modern()) {
                drawing_round_rect(option_x, option_y, option_width, 28, 6, difficulty_color(i));
                drawing_round_rect_outline(option_x, option_y, option_width, 28, 6, 1, COLOR_BLACK);
            } else {
                drawing_rect(option_x, option_y, option_width, 28, difficulty_color(i));
                drawing_rect_outline(option_x, option_y, option_width, 28, 1, COLOR_BLACK);
            }
            draw_centered_text(difficulty_name(i), option_x, option_y, option_width, 28, 1, COLOR_BLACK);
        }
    }
}

static void draw_action_button(void) {
    const char *label = current_stage == CREATE_STAGE_COUNT - 1 ? "SUBMIT" : "NEXT";
    bool enabled = stage_is_complete();
    bool selected = selected_index == CREATE_ACTION_INDEX;
    u32 color = enabled ? COLOR_BLACK : CREATE_DISABLED_COLOR;
    int x = (SCREEN_WIDTH - CREATE_BUTTON_WIDTH) / 2;

    if (platform_graphics_modern()) {
        if (enabled)
            drawing_round_rect(x, CREATE_BUTTON_Y, CREATE_BUTTON_WIDTH, CREATE_BUTTON_HEIGHT,
                               CREATE_BUTTON_HEIGHT / 2, COLOR_BLACK);
        else
            drawing_round_rect_outline(x, CREATE_BUTTON_Y, CREATE_BUTTON_WIDTH, CREATE_BUTTON_HEIGHT,
                                       CREATE_BUTTON_HEIGHT / 2, 2, color);
        if (selected)
            drawing_round_rect_outline(x - 4, CREATE_BUTTON_Y - 4, CREATE_BUTTON_WIDTH + 8,
                                       CREATE_BUTTON_HEIGHT + 8, CREATE_BUTTON_HEIGHT / 2 + 4,
                                       2, 0xFFDC7328);
        draw_centered_text(label, x, CREATE_BUTTON_Y, CREATE_BUTTON_WIDTH, CREATE_BUTTON_HEIGHT,
                           3, enabled ? COLOR_WHITE : color);
        return;
    }

    drawing_rect_outline(
        x,
        CREATE_BUTTON_Y,
        CREATE_BUTTON_WIDTH,
        CREATE_BUTTON_HEIGHT,
        selected ? CREATE_OUTLINE_THICKNESS : 3,
        selected ? COLOR_WHITE : color
    );
    drawing_rect_outline(
        x + CREATE_OUTLINE_THICKNESS,
        CREATE_BUTTON_Y + CREATE_OUTLINE_THICKNESS,
        CREATE_BUTTON_WIDTH - CREATE_OUTLINE_THICKNESS * 2,
        CREATE_BUTTON_HEIGHT - CREATE_OUTLINE_THICKNESS * 2,
        2,
        color
    );

    draw_centered_text(label, x, CREATE_BUTTON_Y, CREATE_BUTTON_WIDTH, CREATE_BUTTON_HEIGHT, 3, color);
}

void create_screen_draw(void) {
    drawing_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_MENU_BG);

    int title_width = text_width(stage_title(), 4);
    drawing_text(stage_title(), (SCREEN_WIDTH - title_width) / 2, 24, 4, COLOR_BLACK);

    for (int i = 0; i < current_stage; i++)
        draw_completed_group_row(i);

    for (int i = 0; i < CREATE_TEXT_FIELD_COUNT; i++)
        draw_text_field(i);

    draw_difficulty_field();

    draw_action_button();
}

void create_screen_exit(void) {}
