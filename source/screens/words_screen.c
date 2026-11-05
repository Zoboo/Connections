#include "words_screen.h"
#include "core/config.h"
#include "game/connections_puzzle.h"
#include "platform/platform_audio.h"
#include "platform/platform_save.h"
#include "platform/platform_graphics.h"
#include "rendering/drawing.h"
#include "rendering/colors.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORD_TILE_WIDTH 180
#define WORD_TILE_HEIGHT 80
#define WORD_TILE_PADDING 16
#define WORD_TILE_START_X 240
#define WORD_TILE_START_Y 70
#define WORD_TILE_STEP_X 210
#define WORD_TILE_STEP_Y 100
#define WORD_TEXT_MAX_SCALE 3
#define WORD_TEXT_MIN_SCALE 1
#define WORD_GRID_COLS 4
#define WORD_GRID_ROWS 4
#define WORD_GRID_NAV_DELAY_MS 160
#define WORD_SELECTED_BORDER_THICKNESS 5
#define WORD_SELECTED_BORDER_DARK_PINK 0xFFD9739B
#define WORD_MISTAKES_Y 495
#define WORD_ACTIONS_Y 545
#define WORD_ACTION_BUTTON_WIDTH 180
#define WORD_ACTION_BUTTON_HEIGHT 52
#define WORD_ACTION_BUTTON_GAP 24
#define WORD_ACTION_BUTTON_OUTLINE 3
#define WORD_ACTION_BUTTON_PADDING 16
#define WORD_QUIT_BUTTON_Y 660
#define WORD_TIMER_X 1060
#define WORD_TIMER_Y 92
#define WORD_TIMER_WIDTH 180
#define WORD_ACTION_TEXT_MAX_SCALE 3
#define WORD_ACTION_TEXT_MIN_SCALE 1
#define WORD_ACTION_ACTIVE_COLOR COLOR_BLACK
#define WORD_ACTION_DISABLED_COLOR COLOR_GRAY
#define WORD_MISTAKE_DOT_COLOR COLOR_DARKGRAY
#define WORD_CURSOR_SHUFFLE 16
#define WORD_CURSOR_DESELECT_ALL 17
#define WORD_CURSOR_SUBMIT 18
#define WORD_CURSOR_NEXT 19
#define WORD_CURSOR_QUIT 20
#define WORD_SUBMIT_MESSAGE_Y 625
#define WORD_SUBMIT_MESSAGE_DURATION_MS 5000
#define WORD_SUBMIT_NONE 0
#define WORD_SUBMIT_TRY_AGAIN -1
#define WORD_SUBMIT_ONE_AWAY -2
#define WORD_SHAKE_DURATION_MS 360
#define WORD_SHAKE_STEP_MS 45
#define WORD_SHAKE_OFFSET 8
#define WORD_SOLVED_PANEL_PADDING 20
#define WORD_LEVEL_YELLOW 0xFF6DDFF9
#define WORD_LEVEL_GREEN  0xFF5AC3A0
#define WORD_LEVEL_BLUE   0xFFEFC4B0
#define WORD_LEVEL_PURPLE 0xFFC581BA
#define WORD_LEVEL_RED    0xFF3D3DF2

static ConnectionsPuzzle puzzle;
static bool puzzle_loaded = false;
static int cursor_index = 0;
static bool chosen_words[CONNECTIONS_WORD_COUNT];
static int solved_group_ids[WORD_GRID_ROWS];
static int solved_group_count = 0;
static Uint32 last_nav_time = 0;
static bool can_press_a = false;
static int last_submit_result = WORD_SUBMIT_NONE;
static Uint32 last_submit_message_at = 0;
static int mistakes_remaining = 4;
static Uint32 shake_start_time = 0;
static Uint32 puzzle_started_at = 0;
static GameSave game_save;
static int pending_puzzle_id = -1;
static bool pending_custom_puzzle = false;
static bool current_puzzle_is_custom = false;
static GameSaveCustomPuzzle pending_custom_data;
static ConnectionsPuzzle pending_race_data;
static bool pending_race = false, racing = false, race_completed = false;
static bool race_failed = false;
typedef enum { WORD_PROMPT_NONE, WORD_PROMPT_QUIT, WORD_PROMPT_SOLUTIONS } WordPrompt;
static WordPrompt prompt = WORD_PROMPT_NONE;
static bool prompt_yes = false;
static Image mistake_images[8];
static bool mistake_images_loaded = false;

static void load_mistake_images(void) {
    if (mistake_images_loaded || !platform_graphics_modern())
        return;
    for (unsigned i = 0; i < sizeof(mistake_images) / sizeof(mistake_images[0]); i++) {
        char path[64];
        snprintf(path, sizeof(path), "romfs:/images/ui/mistakes-%u.png", i);
        image_load_png(path, &mistake_images[i]);
    }
    mistake_images_loaded = true;
}

static int text_width(const char *text, int scale) {
    return drawing_text_width(text, scale);
}

static bool puzzle_is_complete(void);
static bool word_is_solved(int index);

static bool cursor_is_word(void) {
    return cursor_index >= 0 && cursor_index < CONNECTIONS_WORD_COUNT;
}

static bool point_in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static bool point_in_word_tile(int px, int py, int index) {
    int col = index % WORD_GRID_COLS;
    int row = index / WORD_GRID_COLS;
    int x = WORD_TILE_START_X + col * WORD_TILE_STEP_X;
    int y = WORD_TILE_START_Y + row * WORD_TILE_STEP_Y;

    return point_in_rect(px, py, x, y, WORD_TILE_WIDTH, WORD_TILE_HEIGHT);
}

static int action_buttons_start_x(void) {
    int total_width = 3 * WORD_ACTION_BUTTON_WIDTH + 2 * WORD_ACTION_BUTTON_GAP;
    return (SCREEN_WIDTH - total_width) / 2;
}

static int quit_button_x(void) {
    return (SCREEN_WIDTH - WORD_ACTION_BUTTON_WIDTH) / 2;
}

static int touch_action_cursor(int px, int py) {
    if (point_in_rect(
            px,
            py,
            quit_button_x(),
            WORD_QUIT_BUTTON_Y,
            WORD_ACTION_BUTTON_WIDTH,
            WORD_ACTION_BUTTON_HEIGHT
        )) {
        return WORD_CURSOR_QUIT;
    }

    if (puzzle_is_complete()) {
        int next_x = (SCREEN_WIDTH - WORD_ACTION_BUTTON_WIDTH) / 2;
        if (point_in_rect(px, py, next_x, WORD_ACTIONS_Y, WORD_ACTION_BUTTON_WIDTH, WORD_ACTION_BUTTON_HEIGHT))
            return WORD_CURSOR_NEXT;

        return -1;
    }

    int x = action_buttons_start_x();

    if (point_in_rect(px, py, x, WORD_ACTIONS_Y, WORD_ACTION_BUTTON_WIDTH, WORD_ACTION_BUTTON_HEIGHT))
        return WORD_CURSOR_SHUFFLE;

    x += WORD_ACTION_BUTTON_WIDTH + WORD_ACTION_BUTTON_GAP;
    if (point_in_rect(px, py, x, WORD_ACTIONS_Y, WORD_ACTION_BUTTON_WIDTH, WORD_ACTION_BUTTON_HEIGHT))
        return WORD_CURSOR_DESELECT_ALL;

    x += WORD_ACTION_BUTTON_WIDTH + WORD_ACTION_BUTTON_GAP;
    if (point_in_rect(px, py, x, WORD_ACTIONS_Y, WORD_ACTION_BUTTON_WIDTH, WORD_ACTION_BUTTON_HEIGHT))
        return WORD_CURSOR_SUBMIT;

    return -1;
}

static int touch_cursor_index(int px, int py) {
    if (!puzzle_is_complete()) {
        for (int i = 0; i < CONNECTIONS_WORD_COUNT; i++) {
            if (!word_is_solved(i) && point_in_word_tile(px, py, i))
                return i;
        }
    }

    return touch_action_cursor(px, py);
}

static bool puzzle_is_complete(void) {
    return solved_group_count >= CONNECTIONS_GROUP_COUNT;
}

static bool word_is_solved(int index) {
    return index >= 0 && index < solved_group_count * WORD_GRID_COLS;
}

static int chosen_word_count(void) {
    int count = 0;

    for (int i = 0; i < CONNECTIONS_WORD_COUNT; i++) {
        if (chosen_words[i])
            count++;
    }

    return count;
}

static void deselect_all_words(void) {
    memset(chosen_words, 0, sizeof(chosen_words));
}

static int selected_group_id(void) {
    for (int i = 0; i < CONNECTIONS_WORD_COUNT; i++) {
        if (chosen_words[i])
            return puzzle.group_ids[i];
    }

    return -1;
}

static bool selected_words_are_group(void) {
    int group_id = -1;

    for (int i = 0; i < CONNECTIONS_WORD_COUNT; i++) {
        if (!chosen_words[i])
            continue;

        if (group_id < 0)
            group_id = puzzle.group_ids[i];
        else if (puzzle.group_ids[i] != group_id)
            return false;
    }

    return group_id >= 0;
}

static bool selected_words_are_one_away(void) {
    int group_counts[CONNECTIONS_GROUP_COUNT] = {0};

    for (int i = 0; i < CONNECTIONS_WORD_COUNT; i++) {
        if (!chosen_words[i])
            continue;

        int group_id = puzzle.group_ids[i];
        if (group_id >= 0 && group_id < CONNECTIONS_GROUP_COUNT)
            group_counts[group_id]++;
    }

    for (int group = 0; group < CONNECTIONS_GROUP_COUNT; group++) {
        if (group_counts[group] == CONNECTIONS_GROUP_SIZE - 1)
            return true;
    }

    return false;
}

static void clear_submit_message(void) {
    last_submit_result = WORD_SUBMIT_NONE;
    last_submit_message_at = 0;
}

static void show_submit_message(int result, Uint32 now) {
    last_submit_result = result;
    last_submit_message_at = now;
}

static int puzzle_elapsed_seconds(void) {
    if (puzzle_started_at == 0)
        return 0;

    return (int)((SDL_GetTicks() - puzzle_started_at) / 1000);
}

static int puzzle_completed_seconds(void) {
    int seconds = puzzle_elapsed_seconds();
    return seconds > 0 ? seconds : 1;
}

static void format_time(int total_seconds, char *out, size_t out_size) {
    int hours = total_seconds / 3600;
    int minutes = (total_seconds / 60) % 60;
    int seconds = total_seconds % 60;

    if (hours > 0) {
        snprintf(out, out_size, "%d:%02d:%02d", hours, minutes, seconds);
    } else {
        snprintf(out, out_size, "%d:%02d", minutes, seconds);
    }
}

static bool group_is_already_solved(int group_id) {
    for (int i = 0; i < solved_group_count; i++) {
        if (solved_group_ids[i] == group_id)
            return true;
    }

    return false;
}

static void copy_word_slot(
    char dest_word[CONNECTIONS_WORD_MAX_LENGTH],
    int *dest_group_id,
    const char *source_word,
    int source_group_id
) {
    strcpy(dest_word, source_word);
    *dest_group_id = source_group_id;
}

static void rebuild_word_order(void) {
    char old_words[CONNECTIONS_WORD_COUNT][CONNECTIONS_WORD_MAX_LENGTH];
    int old_group_ids[CONNECTIONS_WORD_COUNT];
    int write_index = 0;

    memcpy(old_words, puzzle.words, sizeof(old_words));
    memcpy(old_group_ids, puzzle.group_ids, sizeof(old_group_ids));

    for (int solved = 0; solved < solved_group_count; solved++) {
        int group_id = solved_group_ids[solved];

        for (int i = 0; i < CONNECTIONS_WORD_COUNT; i++) {
            if (old_group_ids[i] == group_id) {
                copy_word_slot(
                    puzzle.words[write_index],
                    &puzzle.group_ids[write_index],
                    old_words[i],
                    old_group_ids[i]
                );
                write_index++;
            }
        }
    }

    for (int i = 0; i < CONNECTIONS_WORD_COUNT; i++) {
        if (!group_is_already_solved(old_group_ids[i])) {
            copy_word_slot(
                puzzle.words[write_index],
                &puzzle.group_ids[write_index],
                old_words[i],
                old_group_ids[i]
            );
            write_index++;
        }
    }
}

static void lock_in_selected_group(void) {
    int group_id = selected_group_id();

    if (group_id < 0 || group_is_already_solved(group_id) || solved_group_count >= WORD_GRID_ROWS)
        return;

    solved_group_ids[solved_group_count] = group_id;
    solved_group_count++;

    rebuild_word_order();
    deselect_all_words();

    if (solved_group_count < WORD_GRID_ROWS)
        cursor_index = solved_group_count * WORD_GRID_COLS;
    else
        cursor_index = WORD_CURSOR_SHUFFLE;
}

static u32 color_from_rgb(Uint8 r, Uint8 g, Uint8 b) {
    return 0xFF000000 | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
}

static u32 interpolate_color(u32 from, u32 to, int amount, int max_amount) {
    int from_r = from & 0xFF;
    int from_g = (from >> 8) & 0xFF;
    int from_b = (from >> 16) & 0xFF;
    int to_r = to & 0xFF;
    int to_g = (to >> 8) & 0xFF;
    int to_b = (to >> 16) & 0xFF;

    Uint8 r = from_r + (to_r - from_r) * amount / max_amount;
    Uint8 g = from_g + (to_g - from_g) * amount / max_amount;
    Uint8 b = from_b + (to_b - from_b) * amount / max_amount;

    return color_from_rgb(r, g, b);
}

static u32 selected_border_color(void) {
    const int cycle_ms = 1400;
    int half_cycle = cycle_ms / 2;
    int time = SDL_GetTicks() % cycle_ms;
    int amount = time <= half_cycle ? time : cycle_ms - time;

    return interpolate_color(COLOR_WHITE, WORD_SELECTED_BORDER_DARK_PINK, amount, half_cycle);
}

static int text_scale_to_fit(const char *text, int max_width) {
    for (int scale = WORD_TEXT_MAX_SCALE; scale > WORD_TEXT_MIN_SCALE; scale--) {
        if (text_width(text, scale) <= max_width)
            return scale;
    }

    return WORD_TEXT_MIN_SCALE;
}

static int text_scale_to_fit_range(const char *text, int max_width, int max_scale, int min_scale) {
    for (int scale = max_scale; scale > min_scale; scale--) {
        if (text_width(text, scale) <= max_width)
            return scale;
    }

    return min_scale;
}

static int solved_panel_width(void) {
    return WORD_TILE_WIDTH * WORD_GRID_COLS + (WORD_TILE_STEP_X - WORD_TILE_WIDTH) * (WORD_GRID_COLS - 1);
}

static u32 group_color(int group_id) {
    int level = puzzle.group_levels[group_id];

    if (level == -1)
        return WORD_LEVEL_RED;

    if (level == 0)
        return WORD_LEVEL_YELLOW;

    if (level == 1)
        return WORD_LEVEL_GREEN;

    if (level == 2)
        return WORD_LEVEL_BLUE;

    return WORD_LEVEL_PURPLE;
}

static void build_group_words_line(int group_id, char *out, size_t out_size) {
    out[0] = '\0';

    for (int member = 0; member < CONNECTIONS_GROUP_SIZE; member++) {
        if (member > 0)
            strncat(out, ", ", out_size - strlen(out) - 1);

        strncat(out, puzzle.answers[group_id][member], out_size - strlen(out) - 1);
    }
}

static void draw_centered_text(const char *text, int x, int y, int width, int height, int scale, u32 color) {
    int label_width = text_width(text, scale);
    int text_x = x + (width - label_width) / 2;
    int text_y = y + (height - 7 * scale) / 2;

    drawing_text(text, text_x, text_y, scale, color);
}

static void draw_centered_heading(const char *text, int x, int y, int width, int height, int scale, u32 color) {
    if (!platform_graphics_modern()) {
        draw_centered_text(text, x, y, width, height, scale, color);
        return;
    }
    while (scale > 1 && drawing_text_bold_width(text, scale) > width - WORD_TILE_PADDING * 2)
        scale--;
    drawing_text_bold(text, x + (width - drawing_text_bold_width(text, scale)) / 2,
                      y + (height - 7 * scale) / 2, scale, color);
}

static void draw_solved_group_row(int row) {
    int group_id = solved_group_ids[row];
    int x = WORD_TILE_START_X;
    int y = WORD_TILE_START_Y + row * WORD_TILE_STEP_Y;
    int width = solved_panel_width();
    char words_line[CONNECTIONS_GROUP_SIZE * CONNECTIONS_WORD_MAX_LENGTH];
    int text_max_width = width - WORD_SOLVED_PANEL_PADDING * 2;
    int title_scale = text_scale_to_fit_range(puzzle.group_names[group_id], text_max_width, 3, 1);

    build_group_words_line(group_id, words_line, sizeof(words_line));
    int words_scale = text_scale_to_fit_range(words_line, text_max_width, 2, 1);

    if (platform_graphics_modern())
        drawing_round_rect(x, y, width, WORD_TILE_HEIGHT, 6, group_color(group_id));
    else
        drawing_rect(x, y, width, WORD_TILE_HEIGHT, group_color(group_id));
    draw_centered_heading(puzzle.group_names[group_id], x, y + 10, width, 24, title_scale, COLOR_BLACK);
    draw_centered_text(words_line, x, y + 42, width, 22, words_scale, COLOR_BLACK);
}

static void draw_action_button(const char *label, int x, int y, bool enabled, bool selected) {
    u32 color = enabled ? WORD_ACTION_ACTIVE_COLOR : WORD_ACTION_DISABLED_COLOR;
    int max_text_width = WORD_ACTION_BUTTON_WIDTH - WORD_ACTION_BUTTON_PADDING * 2;
    int scale = text_scale_to_fit_range(
        label,
        max_text_width,
        WORD_ACTION_TEXT_MAX_SCALE,
        WORD_ACTION_TEXT_MIN_SCALE
    );

    if (platform_graphics_modern()) {
        bool filled = enabled && (!strcmp(label, "Submit") || !strcmp(label, "Next"));
        u32 outline = enabled ? COLOR_BLACK : 0xFFAAA9A5;
        if (filled)
            drawing_round_rect(x, y, WORD_ACTION_BUTTON_WIDTH, WORD_ACTION_BUTTON_HEIGHT,
                               WORD_ACTION_BUTTON_HEIGHT / 2, COLOR_BLACK);
        else
            drawing_round_rect_outline(x, y, WORD_ACTION_BUTTON_WIDTH, WORD_ACTION_BUTTON_HEIGHT,
                                       WORD_ACTION_BUTTON_HEIGHT / 2, 2, outline);
        if (selected)
            drawing_round_rect_outline(x - 5, y - 5, WORD_ACTION_BUTTON_WIDTH + 10,
                                       WORD_ACTION_BUTTON_HEIGHT + 10,
                                       WORD_ACTION_BUTTON_HEIGHT / 2 + 5, 2, 0xFFDC7328);
        draw_centered_text(label, x, y, WORD_ACTION_BUTTON_WIDTH, WORD_ACTION_BUTTON_HEIGHT,
                           scale, filled ? COLOR_WHITE : outline);
        return;
    }

    drawing_rect_outline(
        x,
        y,
        WORD_ACTION_BUTTON_WIDTH,
        WORD_ACTION_BUTTON_HEIGHT,
        WORD_ACTION_BUTTON_OUTLINE,
        color
    );

    if (selected) {
        drawing_rect_outline(
            x - 5,
            y - 5,
            WORD_ACTION_BUTTON_WIDTH + 10,
            WORD_ACTION_BUTTON_HEIGHT + 10,
            WORD_SELECTED_BORDER_THICKNESS,
            selected_border_color()
        );
    }

    draw_centered_text(label, x, y, WORD_ACTION_BUTTON_WIDTH, WORD_ACTION_BUTTON_HEIGHT, scale, color);
}

static void draw_mistakes_remaining(void) {
    const char *label = "Mistakes Remaining:";
    int scale = 2;
    int label_width = text_width(label, scale);
    bool modern = platform_graphics_modern();
    int total_width = label_width + 24 + (modern ? 94 : 4 * 22);
    int x = (SCREEN_WIDTH - total_width) / 2;
    int y = WORD_MISTAKES_Y;
    int dot_x = x + label_width + 24;

    drawing_text(label, x, y, scale, modern ? 0xFF4E595A : COLOR_BLACK);

    if (modern && mistakes_remaining >= 1 && mistakes_remaining <= 4) {
        int index = (4 - mistakes_remaining) * 2 + (puzzle_is_complete() ? 1 : 0);
        if (mistake_images[index].texture) {
            drawing_image(&mistake_images[index], dot_x, y - 1);
            return;
        }
    }

    for (int i = 0; i < mistakes_remaining; i++)
        drawing_circle(dot_x + i * 22, y + 7, 8, WORD_MISTAKE_DOT_COLOR);
}

static void draw_timer(void) {
    char time_text[16];

    format_time(puzzle_elapsed_seconds(), time_text, sizeof(time_text));
    draw_centered_text("TIME", WORD_TIMER_X, WORD_TIMER_Y, WORD_TIMER_WIDTH, 22, 2, COLOR_BLACK);
    draw_centered_text(time_text, WORD_TIMER_X, WORD_TIMER_Y + 32, WORD_TIMER_WIDTH, 34, 3, COLOR_BLACK);
}

static void draw_action_buttons(void) {
    int total_width = 3 * WORD_ACTION_BUTTON_WIDTH + 2 * WORD_ACTION_BUTTON_GAP;
    int x = (SCREEN_WIDTH - total_width) / 2;
    int chosen_count = chosen_word_count();
    bool has_chosen_words = chosen_count > 0;
    bool can_submit = chosen_count == 4;

    if (puzzle_is_complete()) {
        int next_x = (SCREEN_WIDTH - WORD_ACTION_BUTTON_WIDTH) / 2;
        draw_action_button("Next", next_x, WORD_ACTIONS_Y, true, cursor_index == WORD_CURSOR_NEXT);
        draw_action_button("Quit", quit_button_x(), WORD_QUIT_BUTTON_Y, true, cursor_index == WORD_CURSOR_QUIT);
        return;
    }

    draw_action_button("Shuffle", x, WORD_ACTIONS_Y, true, cursor_index == WORD_CURSOR_SHUFFLE);
    draw_action_button(
        "Deselect All",
        x + WORD_ACTION_BUTTON_WIDTH + WORD_ACTION_BUTTON_GAP,
        WORD_ACTIONS_Y,
        has_chosen_words,
        cursor_index == WORD_CURSOR_DESELECT_ALL
    );
    draw_action_button(
        "Submit",
        x + 2 * (WORD_ACTION_BUTTON_WIDTH + WORD_ACTION_BUTTON_GAP),
        WORD_ACTIONS_Y,
        can_submit,
        cursor_index == WORD_CURSOR_SUBMIT
    );
    draw_action_button("Quit", quit_button_x(), WORD_QUIT_BUTTON_Y, true, cursor_index == WORD_CURSOR_QUIT);
}

static void draw_submit_message(void) {
    const char *message = NULL;
    u32 color = COLOR_BLACK;

    if (last_submit_result == WORD_SUBMIT_NONE || last_submit_message_at == 0)
        return;

    if (SDL_GetTicks() - last_submit_message_at >= WORD_SUBMIT_MESSAGE_DURATION_MS)
        return;

    if (last_submit_result == WORD_SUBMIT_ONE_AWAY) {
        message = "ONE AWAY";
    } else if (last_submit_result == WORD_SUBMIT_TRY_AGAIN) {
        message = "TRY AGAIN";
    }

    if (!message)
        return;

    int scale = 2;
    int width = text_width(message, scale);
    drawing_text(message, (SCREEN_WIDTH - width) / 2, WORD_SUBMIT_MESSAGE_Y, scale, color);
}

static int shake_offset_for_word(int index) {
    if (!chosen_words[index] || shake_start_time == 0)
        return 0;

    Uint32 elapsed = SDL_GetTicks() - shake_start_time;
    if (elapsed >= WORD_SHAKE_DURATION_MS)
        return 0;

    int step = (int)(elapsed / WORD_SHAKE_STEP_MS);
    if (step % 2 == 0)
        return WORD_SHAKE_OFFSET;

    return -WORD_SHAKE_OFFSET;
}

static void shuffle_current_puzzle_words(void) {
    int first_unsolved = solved_group_count * WORD_GRID_COLS;
    for (int i = puzzle.word_count - 1; i > first_unsolved; i--) {
        int j = first_unsolved + rand() % (i - first_unsolved + 1);
        if (i == j) continue;
        bool temp_chosen = chosen_words[i];
        char temp[CONNECTIONS_WORD_MAX_LENGTH];
        int temp_group_id = puzzle.group_ids[i];

        strcpy(temp, puzzle.words[i]);
        strcpy(puzzle.words[i], puzzle.words[j]);
        strcpy(puzzle.words[j], temp);

        puzzle.group_ids[i] = puzzle.group_ids[j];
        puzzle.group_ids[j] = temp_group_id;
        chosen_words[i] = chosen_words[j];
        chosen_words[j] = temp_chosen;
    }
}

static bool load_custom_puzzle(const GameSaveCustomPuzzle *custom_puzzle) {
    memset(&puzzle, 0, sizeof(puzzle));

    puzzle.id = -1;
    strcpy(puzzle.date, "CUSTOM");

    for (int group = 0; group < CONNECTIONS_GROUP_COUNT; group++) {
        strncpy(puzzle.group_names[group], custom_puzzle->group_names[group], CONNECTIONS_GROUP_NAME_MAX_LENGTH);
        puzzle.group_names[group][CONNECTIONS_GROUP_NAME_MAX_LENGTH - 1] = '\0';
        puzzle.group_levels[group] = custom_puzzle->group_levels[group];

        for (int member = 0; member < CONNECTIONS_GROUP_SIZE; member++) {
            int word_index = group * CONNECTIONS_GROUP_SIZE + member;

            strncpy(puzzle.words[word_index], custom_puzzle->words[group][member], CONNECTIONS_WORD_MAX_LENGTH);
            puzzle.words[word_index][CONNECTIONS_WORD_MAX_LENGTH - 1] = '\0';
            strncpy(puzzle.answers[group][member], custom_puzzle->words[group][member], CONNECTIONS_WORD_MAX_LENGTH);
            puzzle.answers[group][member][CONNECTIONS_WORD_MAX_LENGTH - 1] = '\0';
            puzzle.group_ids[word_index] = group;
            puzzle.word_count++;
        }
    }

    shuffle_current_puzzle_words();
    return puzzle.word_count == CONNECTIONS_WORD_COUNT;
}

void words_screen_init(void) {
    platform_audio_play_bloop();
    load_mistake_images();

    current_puzzle_is_custom = pending_custom_puzzle;

    solved_group_count = 0;
    memset(chosen_words, 0, sizeof(chosen_words));
    racing = pending_race;
    race_completed = false;
    race_failed = false;
    prompt = WORD_PROMPT_NONE;
    prompt_yes = false;
    if (pending_race) {
        puzzle = pending_race_data;
        puzzle_loaded = true;
        pending_race = false;
    } else if (pending_custom_puzzle) {
        puzzle_loaded = load_custom_puzzle(&pending_custom_data);
        pending_custom_puzzle = false;
    } else if (pending_puzzle_id >= 0) {
        puzzle_loaded = connections_puzzle_load_by_id(connections_puzzle_data_path(), pending_puzzle_id, &puzzle, true);
        pending_puzzle_id = -1;
    } else {
        puzzle_loaded = connections_puzzle_load_random(connections_puzzle_data_path(), &puzzle);
    }

    if (!racing && !platform_save_load(&game_save))
        platform_save_defaults(&game_save);

    cursor_index = 0;
    memset(chosen_words, 0, sizeof(chosen_words));
    memset(solved_group_ids, 0, sizeof(solved_group_ids));
    solved_group_count = 0;
    // Race puzzles already carry the host's shared word layout.
    last_nav_time = 0;
    can_press_a = false;
    clear_submit_message();
    mistakes_remaining = 4;
    shake_start_time = 0;
    puzzle_started_at = SDL_GetTicks();
}

void words_screen_open_puzzle_id(int puzzle_id) {
    pending_race = false;
    pending_puzzle_id = puzzle_id;
    pending_custom_puzzle = false;
}

void words_screen_open_custom_puzzle(const GameSaveCustomPuzzle *custom_puzzle) {
    if (!custom_puzzle)
        return;

    pending_custom_data = *custom_puzzle;
    pending_race = false;
    pending_custom_puzzle = true;
    pending_puzzle_id = -1;
}

static void start_next_puzzle(void) {
    words_screen_init();
}

void words_screen_open_race_puzzle(const ConnectionsPuzzle *next) {
    pending_race_data = *next;
    pending_race = true;
    pending_custom_puzzle = false;
    pending_puzzle_id = -1;
    words_screen_init();
}

bool words_screen_take_race_completed(void) {
    bool completed = race_completed;
    race_completed = false;
    return completed;
}

bool words_screen_take_race_failed(void) {
    bool failed = race_failed;
    race_failed = false;
    return failed;
}

static void open_prompt(WordPrompt next) {
    prompt = next;
    prompt_yes = false;
    can_press_a = false;
    last_nav_time = SDL_GetTicks();
}

static ScreenResult update_prompt(PlatformInput input, Uint32 now) {
    if ((input.left || input.right || input.up || input.down) &&
        now - last_nav_time >= WORD_GRID_NAV_DELAY_MS) {
        prompt_yes = !prompt_yes;
        last_nav_time = now;
    }
    bool confirm = input.a;
    if (input.touch && point_in_rect(input.touch_x, input.touch_y, 336, 370, 280, 58)) {
        prompt_yes = true;
        confirm = true;
    } else if (input.touch && point_in_rect(input.touch_x, input.touch_y, 664, 370, 280, 58)) {
        prompt_yes = false;
        confirm = true;
    }
    if (input.b) { prompt_yes = false; confirm = true; }
    if (!confirm) return (ScreenResult){ SCREEN_WORDS };

    WordPrompt answered = prompt;
    prompt = WORD_PROMPT_NONE;
    can_press_a = false;
    if (answered == WORD_PROMPT_QUIT) {
        return (ScreenResult){ prompt_yes ? SCREEN_MAIN_MENU : SCREEN_WORDS };
    }
    if (!prompt_yes) {
        if (racing) race_failed = true;
        return (ScreenResult){ racing ? SCREEN_WORDS : SCREEN_MAIN_MENU };
    }

    // Reveal through the existing solved-row renderer without awarding a solve.
    for (int group = 0; group < CONNECTIONS_GROUP_COUNT; group++) {
        if (!group_is_already_solved(group))
            solved_group_ids[solved_group_count++] = group;
    }
    rebuild_word_order();
    deselect_all_words();
    clear_submit_message();
    shake_start_time = 0;
    cursor_index = WORD_CURSOR_NEXT;
    return (ScreenResult){ SCREEN_WORDS };
}

static void draw_prompt(void) {
    if (prompt == WORD_PROMPT_NONE) return;
    drawing_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0x88000000);
    drawing_round_rect(300, 230, 680, 250, 12, COLOR_WHITE);
    const char *title = prompt == WORD_PROMPT_QUIT ? "Quit current puzzle?" : "View solutions?";
    draw_centered_text(title, 320, 275, 640, 42, 3, COLOR_BLACK);
    for (int i = 0; i < 2; i++) {
        int x = i ? 664 : 336;
        bool selected = prompt_yes == (i == 0);
        drawing_round_rect(x, 370, 280, 58, 29, selected ? COLOR_BLACK : COLOR_WHITE);
        drawing_round_rect_outline(x, 370, 280, 58, 29, 2, COLOR_BLACK);
        if (selected) drawing_round_rect_outline(x - 4, 366, 288, 66, 33, 2, 0xFFDC7328);
        draw_centered_text(i ? "No" : "Yes", x, 370, 280, 58, 3,
                           selected ? COLOR_WHITE : COLOR_BLACK);
    }
}

ScreenResult words_screen_update(PlatformInput input) {
    Uint32 now = SDL_GetTicks();
    bool activate_cursor = false;

    if (prompt != WORD_PROMPT_NONE) return update_prompt(input, now);

    if (input.touch) {
        int touched_cursor = touch_cursor_index(input.touch_x, input.touch_y);

        if (touched_cursor >= 0) {
            cursor_index = touched_cursor;
            activate_cursor = true;
        }
    }

    if (!input.touch && now - last_nav_time >= WORD_GRID_NAV_DELAY_MS) {
        bool moved = false;

        if (puzzle_is_complete()) {
            if (cursor_index != WORD_CURSOR_NEXT && cursor_index != WORD_CURSOR_QUIT) {
                cursor_index = WORD_CURSOR_NEXT;
            } else if (cursor_index == WORD_CURSOR_NEXT && input.down) {
                cursor_index = WORD_CURSOR_QUIT;
                moved = true;
            } else if (cursor_index == WORD_CURSOR_QUIT && input.up) {
                cursor_index = WORD_CURSOR_NEXT;
                moved = true;
            }
        } else if (cursor_is_word()) {
            int col = cursor_index % WORD_GRID_COLS;
            int row = cursor_index / WORD_GRID_COLS;

            if (input.left && col > 0) {
                cursor_index--;
                moved = true;
            } else if (input.right && col < WORD_GRID_COLS - 1) {
                cursor_index++;
                moved = true;
            } else if (input.up && row > 0) {
                cursor_index -= WORD_GRID_COLS;
                moved = true;
            } else if (input.down) {
                if (row < WORD_GRID_ROWS - 1) {
                    cursor_index += WORD_GRID_COLS;
                } else if (col == 0) {
                    cursor_index = WORD_CURSOR_SHUFFLE;
                } else if (col == 3) {
                    cursor_index = WORD_CURSOR_SUBMIT;
                } else {
                    cursor_index = WORD_CURSOR_DESELECT_ALL;
                }

                moved = true;
            }
        } else if (cursor_index == WORD_CURSOR_QUIT) {
            if (input.up) {
                cursor_index = WORD_CURSOR_DESELECT_ALL;
                moved = true;
            }
        } else {
            if (input.left && cursor_index > WORD_CURSOR_SHUFFLE) {
                cursor_index--;
                moved = true;
            } else if (input.right && cursor_index < WORD_CURSOR_SUBMIT) {
                cursor_index++;
                moved = true;
            } else if (input.down) {
                cursor_index = WORD_CURSOR_QUIT;
                moved = true;
            } else if (input.up) {
                if (cursor_index == WORD_CURSOR_SHUFFLE)
                    cursor_index = 12;
                else if (cursor_index == WORD_CURSOR_DESELECT_ALL)
                    cursor_index = 13;
                else
                    cursor_index = 15;

                moved = true;
            }
        }

        if (moved)
            last_nav_time = now;
    }

    if (!input.a)
        can_press_a = true;

    if (input.a && can_press_a)
        activate_cursor = true;

    if (activate_cursor) {
        if (cursor_index == WORD_CURSOR_QUIT) {
            open_prompt(WORD_PROMPT_QUIT);
        } else if (cursor_index == WORD_CURSOR_NEXT && puzzle_is_complete()) {
            if (racing) race_failed = true; // Continue after viewing a failed puzzle.
            else start_next_puzzle();
        } else if (cursor_is_word()) {
            if (chosen_words[cursor_index] || chosen_word_count() < 4) {
                if (!word_is_solved(cursor_index)) {
                    chosen_words[cursor_index] = !chosen_words[cursor_index];
                }
            }
        } else if (cursor_index == WORD_CURSOR_SHUFFLE && !puzzle_is_complete()) {
            shuffle_current_puzzle_words();
            shake_start_time = 0;
        } else if (cursor_index == WORD_CURSOR_DESELECT_ALL) {
            deselect_all_words();
        } else if (cursor_index == WORD_CURSOR_SUBMIT && chosen_word_count() == 4) {
            if (selected_words_are_group()) {
                lock_in_selected_group();
                clear_submit_message();
                if (puzzle_is_complete()) {
                    if (racing) race_completed = true;
                    if (!racing && !current_puzzle_is_custom) {
                        platform_save_mark_completed_today(&game_save, puzzle.id, puzzle_completed_seconds());
                        platform_save_write(&game_save);
                    }
                    cursor_index = WORD_CURSOR_NEXT;
                }
            } else {
                mistakes_remaining--;
                show_submit_message(
                    selected_words_are_one_away() ? WORD_SUBMIT_ONE_AWAY : WORD_SUBMIT_TRY_AGAIN,
                    now
                );
                shake_start_time = now;
                if (mistakes_remaining == 0) open_prompt(WORD_PROMPT_SOLUTIONS);
            }
        }

        can_press_a = false;
    }

    return (ScreenResult){ SCREEN_WORDS };
}

void words_screen_draw(void) {
    bool modern = platform_graphics_modern();
    drawing_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, modern ? 0xFFF8F8F8 : COLOR_MENU_BG);
    if (modern && puzzle_loaded)
        draw_centered_text("Create four groups of four!", WORD_TILE_START_X, 20,
                           solved_panel_width(), 28, 3, COLOR_BLACK);

    for (int i = 0; i < 16; i++) {
        int col = i % WORD_GRID_COLS;
        int row = i / WORD_GRID_COLS;
        int x = WORD_TILE_START_X + col * WORD_TILE_STEP_X + shake_offset_for_word(i);
        int y = WORD_TILE_START_Y + row * WORD_TILE_STEP_Y;

        bool solved = word_is_solved(i);
        if (solved) {
            if (col == 0)
                draw_solved_group_row(row);

            continue;
        }

        u32 tile_color = (chosen_words[i] || solved) ? COLOR_BLACK : COLOR_WHITE;
        u32 text_color = (chosen_words[i] || solved) ? COLOR_WHITE : COLOR_BLACK;

        if (modern) {
            tile_color = chosen_words[i] ? 0xFF4E595A : 0xFFE6EFEF;
            drawing_round_rect(x, y, WORD_TILE_WIDTH, WORD_TILE_HEIGHT, 6, tile_color);
            if (i == cursor_index)
                drawing_round_rect_outline(x - 4, y - 4, WORD_TILE_WIDTH + 8,
                                           WORD_TILE_HEIGHT + 8, 10, 2, COLOR_BLACK);
        } else {
            drawing_rect(x, y, WORD_TILE_WIDTH, WORD_TILE_HEIGHT, tile_color);
        }
        if (!modern && i == cursor_index) {
            drawing_rect_outline(
                x,
                y,
                WORD_TILE_WIDTH,
                WORD_TILE_HEIGHT,
                WORD_SELECTED_BORDER_THICKNESS,
                selected_border_color()
            );
        }

        if (puzzle_loaded && i < puzzle.word_count) {
            int max_text_width = WORD_TILE_WIDTH - WORD_TILE_PADDING * 2;
            int scale = text_scale_to_fit(puzzle.words[i], max_text_width);
            draw_centered_heading(puzzle.words[i], x, y, WORD_TILE_WIDTH, WORD_TILE_HEIGHT,
                                  scale, text_color);
        }
    }

    if (!puzzle_loaded) {
        if (current_puzzle_is_custom)
            drawing_text("FAILED TO LOAD CUSTOM PUZZLE", 360, 40, 3, modern ? COLOR_BLACK : COLOR_WHITE);
        else
            drawing_text("FAILED TO LOAD CONNECTIONS JSON", 340, 40, 3, modern ? COLOR_BLACK : COLOR_WHITE);
    }

    draw_mistakes_remaining();
    draw_timer();
    draw_action_buttons();
    draw_submit_message();
    draw_prompt();
}

void words_screen_exit(void) {
    for (unsigned i = 0; i < sizeof(mistake_images) / sizeof(mistake_images[0]); i++)
        image_free(&mistake_images[i]);
    mistake_images_loaded = false;
}
