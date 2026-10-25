#include "history_screen.h"

#include "core/config.h"
#include "game/connections_puzzle.h"
#include "platform/platform_audio.h"
#include "platform/platform_save.h"
#include "platform/platform_graphics.h"
#include "rendering/colors.h"
#include "rendering/drawing.h"
#include "words_screen.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#define HISTORY_ROW_HEIGHT 118
#define HISTORY_START_Y 92
#define HISTORY_LEFT_X 40
#define HISTORY_WORDS_X 340
#define HISTORY_VISIBLE_ROWS 4
#define HISTORY_NAV_DELAY_MS 160
#define HISTORY_ENTRY_X 24
#define HISTORY_ENTRY_WIDTH 1232
#define HISTORY_ENTRY_HEIGHT 116
#define HISTORY_ENTRY_OUTLINE 4
#define HISTORY_SEARCH_BUTTON_X 1030
#define HISTORY_SEARCH_BUTTON_Y 18
#define HISTORY_SEARCH_BUTTON_WIDTH 190
#define HISTORY_SEARCH_BUTTON_HEIGHT 44
#define HISTORY_SEARCH_OUTLINE 3

typedef struct {
    int puzzle_id;
    int best_time_seconds;
    char published_date[GAME_SAVE_DATE_LENGTH];
    char completed_date[GAME_SAVE_DATE_LENGTH];
    char words[CONNECTIONS_WORD_COUNT][CONNECTIONS_WORD_MAX_LENGTH];
} HistoryEntry;

static GameSave save_data;
static HistoryEntry entries[GAME_SAVE_MAX_COMPLETED_PUZZLES];
static int loaded_count = 0;
static int next_save_index = -1;
static int scroll_offset = 0;
static int selected_entry = 0;
static Uint32 last_nav_time = 0;
static bool loading = false;
static bool can_press_a = false;
static bool search_active = false;
static bool search_button_selected = false;
static char search_filter[5] = "";

static int text_width(const char *text, int scale) {
    return drawing_text_width(text, scale);
}

static void draw_centered_text(const char *text, int x, int y, int w, int h, int scale, u32 color) {
    int width = text_width(text, scale);
    drawing_text(text, x + (w - width) / 2, y + (h - 7 * scale) / 2, scale, color);
}

static void copy_date(char *dest, size_t dest_size, const char *source) {
    size_t length = strlen(source);
    if (length >= dest_size)
        length = dest_size - 1;

    memcpy(dest, source, length);
    dest[length] = '\0';
}

static void format_time(int total_seconds, char *out, size_t out_size) {
    if (total_seconds <= 0) {
        snprintf(out, out_size, "--:--");
        return;
    }

    int hours = total_seconds / 3600;
    int minutes = (total_seconds / 60) % 60;
    int seconds = total_seconds % 60;

    if (hours > 0) {
        snprintf(out, out_size, "%d:%02d:%02d", hours, minutes, seconds);
    } else {
        snprintf(out, out_size, "%d:%02d", minutes, seconds);
    }
}

static bool history_entry_matches_filter(const HistoryEntry *entry) {
    if (!search_active)
        return true;

    return entry->published_date[5] == search_filter[0] &&
           entry->published_date[6] == search_filter[1] &&
           entry->published_date[8] == search_filter[2] &&
           entry->published_date[9] == search_filter[3];
}

static int filtered_entry_count(void) {
    int count = 0;

    for (int i = 0; i < loaded_count; i++) {
        if (history_entry_matches_filter(&entries[i]))
            count++;
    }

    return count;
}

static int filtered_entry_index_at(int position) {
    int current_position = 0;

    for (int i = 0; i < loaded_count; i++) {
        if (!history_entry_matches_filter(&entries[i]))
            continue;

        if (current_position == position)
            return i;

        current_position++;
    }

    return -1;
}

static int filtered_position_of_entry(int entry_index) {
    int current_position = 0;

    if (entry_index < 0 || entry_index >= loaded_count)
        return -1;

    for (int i = 0; i < loaded_count; i++) {
        if (!history_entry_matches_filter(&entries[i]))
            continue;

        if (i == entry_index)
            return current_position;

        current_position++;
    }

    return -1;
}

static void copy_loaded_puzzle(const ConnectionsPuzzle *puzzle, const char *completed_date, int best_time_seconds) {
    if (loaded_count >= GAME_SAVE_MAX_COMPLETED_PUZZLES)
        return;

    HistoryEntry *entry = &entries[loaded_count++];
    entry->puzzle_id = puzzle->id;
    entry->best_time_seconds = best_time_seconds;
    copy_date(entry->published_date, sizeof(entry->published_date), puzzle->date);
    copy_date(entry->completed_date, sizeof(entry->completed_date), completed_date);

    for (int i = 0; i < CONNECTIONS_WORD_COUNT; i++) {
        strncpy(entry->words[i], puzzle->words[i], CONNECTIONS_WORD_MAX_LENGTH);
        entry->words[i][CONNECTIONS_WORD_MAX_LENGTH - 1] = '\0';
    }
}

static void load_next_history_entry(void) {
    if (!loading)
        return;

    if (next_save_index < 0) {
        loading = false;
        return;
    }

    ConnectionsPuzzle puzzle;
    int puzzle_id = save_data.completed_puzzle_ids[next_save_index];

    if (connections_puzzle_load_by_id(connections_puzzle_data_path(), puzzle_id, &puzzle, true)) {
        copy_loaded_puzzle(
            &puzzle,
            save_data.completed_puzzle_dates[next_save_index],
            save_data.completed_puzzle_best_times[next_save_index]
        );
    }

    next_save_index--;
}

static bool point_in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static int touched_entry_index(int px, int py) {
    int visible = filtered_entry_count() - scroll_offset;
    if (visible > HISTORY_VISIBLE_ROWS)
        visible = HISTORY_VISIBLE_ROWS;

    for (int i = 0; i < visible; i++) {
        int y = HISTORY_START_Y + i * HISTORY_ROW_HEIGHT - 12;
        if (point_in_rect(px, py, HISTORY_ENTRY_X, y, HISTORY_ENTRY_WIDTH, HISTORY_ENTRY_HEIGHT))
            return filtered_entry_index_at(scroll_offset + i);
    }

    return -1;
}

static bool point_in_search_button(int px, int py) {
    return point_in_rect(
        px,
        py,
        HISTORY_SEARCH_BUTTON_X,
        HISTORY_SEARCH_BUTTON_Y,
        HISTORY_SEARCH_BUTTON_WIDTH,
        HISTORY_SEARCH_BUTTON_HEIGHT
    );
}

static void keep_selected_visible(void) {
    int selected_position = filtered_position_of_entry(selected_entry);

    if (selected_position < 0)
        return;

    if (selected_position < scroll_offset)
        scroll_offset = selected_position;

    if (selected_position >= scroll_offset + HISTORY_VISIBLE_ROWS)
        scroll_offset = selected_position - HISTORY_VISIBLE_ROWS + 1;
}

static void select_first_filtered_entry(void) {
    selected_entry = filtered_entry_index_at(0);
    scroll_offset = 0;
}

static bool valid_search_text(const char *text) {
    for (int i = 0; i < 4; i++) {
        if (text[i] < '0' || text[i] > '9')
            return false;
    }

    return text[4] == '\0';
}

static void apply_search_filter(const char *text) {
    strncpy(search_filter, text, sizeof(search_filter));
    search_filter[sizeof(search_filter) - 1] = '\0';
    search_active = true;
    search_button_selected = false;
    select_first_filtered_entry();

    if (selected_entry < 0)
        search_button_selected = true;
}

static void open_search_keyboard(void) {
    SwkbdConfig keyboard;
    char output[8] = {0};
    Result rc = swkbdCreate(&keyboard, 0);
    if (R_FAILED(rc))
        return;

    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetType(&keyboard, SwkbdType_NumPad);
    swkbdConfigSetHeaderText(&keyboard, "Search Date");
    swkbdConfigSetGuideText(&keyboard, "Enter MMDD");
    swkbdConfigSetOkButtonText(&keyboard, "SEARCH");
    swkbdConfigSetStringLenMin(&keyboard, 4);
    swkbdConfigSetStringLenMax(&keyboard, 4);
    if (search_active)
        swkbdConfigSetInitialText(&keyboard, search_filter);

    rc = swkbdShow(&keyboard, output, sizeof(output));
    if (R_SUCCEEDED(rc) && valid_search_text(output))
        apply_search_filter(output);

    swkbdClose(&keyboard);
}

void history_screen_init(void) {
    if (!platform_save_load(&save_data))
        platform_save_defaults(&save_data);

    loaded_count = 0;
    next_save_index = save_data.completed_puzzle_count - 1;
    scroll_offset = 0;
    selected_entry = 0;
    last_nav_time = 0;
    loading = true;
    can_press_a = false;
    search_active = false;
    search_button_selected = false;
    search_filter[0] = '\0';
}

ScreenResult history_screen_update(PlatformInput input) {
    if (input.b)
        return (ScreenResult){ SCREEN_MAIN_MENU };

    load_next_history_entry();

    int filtered_count = filtered_entry_count();
    if (filtered_count > 0 &&
        (selected_entry < 0 || filtered_position_of_entry(selected_entry) < 0)) {
        select_first_filtered_entry();
    } else if (filtered_count <= 0) {
        selected_entry = -1;
        scroll_offset = 0;
    }

    if (!input.a)
        can_press_a = true;

    if (input.touch) {
        if (point_in_search_button(input.touch_x, input.touch_y)) {
            platform_audio_play_pop();
            search_button_selected = true;
            open_search_keyboard();
            can_press_a = false;
            return (ScreenResult){ SCREEN_HISTORY };
        }

        int entry_index = touched_entry_index(input.touch_x, input.touch_y);
        if (entry_index >= 0 && entry_index < loaded_count) {
            platform_audio_play_pop();
            selected_entry = entry_index;
            search_button_selected = false;
            words_screen_open_puzzle_id(entries[selected_entry].puzzle_id);
            return (ScreenResult){ SCREEN_WORDS };
        }
    }

    if (input.a && can_press_a && search_button_selected) {
        platform_audio_play_pop();
        can_press_a = false;
        open_search_keyboard();
        return (ScreenResult){ SCREEN_HISTORY };
    }

    if (input.a && can_press_a && selected_entry >= 0 && selected_entry < loaded_count) {
        platform_audio_play_pop();
        can_press_a = false;
        words_screen_open_puzzle_id(entries[selected_entry].puzzle_id);
        return (ScreenResult){ SCREEN_WORDS };
    }

    Uint32 now = SDL_GetTicks();
    if (now - last_nav_time >= HISTORY_NAV_DELAY_MS) {
        int selected_position = filtered_position_of_entry(selected_entry);

        if (search_button_selected) {
            if (input.down && filtered_count > 0) {
                search_button_selected = false;
                select_first_filtered_entry();
                platform_audio_play_pop();
                last_nav_time = now;
            }
        } else if (filtered_count > 0 && selected_position >= 0) {
            if (input.down && selected_position < filtered_count - 1) {
                selected_entry = filtered_entry_index_at(selected_position + 1);
                keep_selected_visible();
                platform_audio_play_pop();
                last_nav_time = now;
            } else if (input.up) {
                if (selected_position > 0) {
                    selected_entry = filtered_entry_index_at(selected_position - 1);
                    keep_selected_visible();
                } else {
                    search_button_selected = true;
                }

                platform_audio_play_pop();
                last_nav_time = now;
            }
        } else if (input.up || input.down) {
            search_button_selected = true;
            platform_audio_play_pop();
            last_nav_time = now;
        }
    }

    return (ScreenResult){ SCREEN_HISTORY };
}

static void draw_words(const HistoryEntry *entry, int x, int y) {
    for (int row = 0; row < 4; row++) {
        char line[CONNECTIONS_GROUP_SIZE * CONNECTIONS_WORD_MAX_LENGTH];
        line[0] = '\0';

        for (int col = 0; col < 4; col++) {
            int index = row * 4 + col;
            if (col > 0)
                strncat(line, ", ", sizeof(line) - strlen(line) - 1);
            strncat(line, entry->words[index], sizeof(line) - strlen(line) - 1);
        }

        drawing_text(line, x, y + row * 18, 1, COLOR_BLACK);
    }
}

static void draw_entry(const HistoryEntry *entry, int y, bool selected) {
    if (platform_graphics_modern()) {
        drawing_round_rect(HISTORY_ENTRY_X, y - 12, HISTORY_ENTRY_WIDTH, HISTORY_ENTRY_HEIGHT,
                           6, 0xFFE6EFEF);
        if (selected)
            drawing_round_rect_outline(HISTORY_ENTRY_X, y - 12, HISTORY_ENTRY_WIDTH,
                                       HISTORY_ENTRY_HEIGHT, 6, 2, 0xFFDC7328);
    } else if (selected) {
        drawing_rect_outline(
            HISTORY_ENTRY_X,
            y - 12,
            HISTORY_ENTRY_WIDTH,
            HISTORY_ENTRY_HEIGHT,
            HISTORY_ENTRY_OUTLINE,
            COLOR_WHITE
        );
        drawing_rect_outline(
            HISTORY_ENTRY_X + HISTORY_ENTRY_OUTLINE,
            y - 12 + HISTORY_ENTRY_OUTLINE,
            HISTORY_ENTRY_WIDTH - HISTORY_ENTRY_OUTLINE * 2,
            HISTORY_ENTRY_HEIGHT - HISTORY_ENTRY_OUTLINE * 2,
            2,
            COLOR_BLACK
        );
    }

    drawing_text("Published", HISTORY_LEFT_X, y, 1, COLOR_BLACK);
    drawing_text(entry->published_date, HISTORY_LEFT_X, y + 18, 1, COLOR_BLACK);
    drawing_text("Completed", HISTORY_LEFT_X, y + 42, 1, COLOR_BLACK);
    drawing_text(entry->completed_date, HISTORY_LEFT_X, y + 60, 1, COLOR_BLACK);
    char best_time[16];
    format_time(entry->best_time_seconds, best_time, sizeof(best_time));
    drawing_text("Best Time", HISTORY_LEFT_X, y + 78, 1, COLOR_BLACK);
    drawing_text(best_time, HISTORY_LEFT_X + 106, y + 78, 1, COLOR_BLACK);
    draw_words(entry, HISTORY_WORDS_X, y);
}

static void draw_search_button(void) {
    char label[32];
    u32 fill_color = search_button_selected ? COLOR_BLACK : COLOR_MENU_BG;
    u32 text_color = search_button_selected ? COLOR_WHITE : COLOR_BLACK;
    int outline = search_button_selected ? HISTORY_SEARCH_OUTLINE + 1 : HISTORY_SEARCH_OUTLINE;

    if (search_active) {
        snprintf(
            label,
            sizeof(label),
            "%c%c/%c%c",
            search_filter[0],
            search_filter[1],
            search_filter[2],
            search_filter[3]
        );
    } else {
        snprintf(label, sizeof(label), "SEARCH");
    }

    if (platform_graphics_modern()) {
        drawing_round_rect(HISTORY_SEARCH_BUTTON_X, HISTORY_SEARCH_BUTTON_Y,
                           HISTORY_SEARCH_BUTTON_WIDTH, HISTORY_SEARCH_BUTTON_HEIGHT,
                           HISTORY_SEARCH_BUTTON_HEIGHT / 2, fill_color);
        drawing_round_rect_outline(HISTORY_SEARCH_BUTTON_X, HISTORY_SEARCH_BUTTON_Y,
                                   HISTORY_SEARCH_BUTTON_WIDTH, HISTORY_SEARCH_BUTTON_HEIGHT,
                                   HISTORY_SEARCH_BUTTON_HEIGHT / 2, 2, COLOR_BLACK);
        if (search_button_selected)
            drawing_round_rect_outline(HISTORY_SEARCH_BUTTON_X - 4, HISTORY_SEARCH_BUTTON_Y - 4,
                                       HISTORY_SEARCH_BUTTON_WIDTH + 8, HISTORY_SEARCH_BUTTON_HEIGHT + 8,
                                       HISTORY_SEARCH_BUTTON_HEIGHT / 2 + 4, 2, 0xFFDC7328);
    } else {
    drawing_rect(
        HISTORY_SEARCH_BUTTON_X,
        HISTORY_SEARCH_BUTTON_Y,
        HISTORY_SEARCH_BUTTON_WIDTH,
        HISTORY_SEARCH_BUTTON_HEIGHT,
        fill_color
    );
    drawing_rect_outline(
        HISTORY_SEARCH_BUTTON_X,
        HISTORY_SEARCH_BUTTON_Y,
        HISTORY_SEARCH_BUTTON_WIDTH,
        HISTORY_SEARCH_BUTTON_HEIGHT,
        outline,
        COLOR_BLACK
    );
    }
    draw_centered_text(
        label,
        HISTORY_SEARCH_BUTTON_X,
        HISTORY_SEARCH_BUTTON_Y,
        HISTORY_SEARCH_BUTTON_WIDTH,
        HISTORY_SEARCH_BUTTON_HEIGHT,
        2,
        text_color
    );
}

void history_screen_draw(void) {
    drawing_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_MENU_BG);

    int title_width = text_width("History", 3);
    drawing_text("History", (SCREEN_WIDTH - title_width) / 2, 18, 3, COLOR_BLACK);
    draw_search_button();

    char save_status[96];
    int filtered_count = filtered_entry_count();
    if (search_active) {
        snprintf(
            save_status,
            sizeof(save_status),
            "Save %s Records %d Loaded %d Matches %d",
            platform_save_backend_name(),
            save_data.completed_puzzle_count,
            loaded_count,
            filtered_count
        );
    } else {
        snprintf(
            save_status,
            sizeof(save_status),
            "Save %s Records %d Loaded %d",
            platform_save_backend_name(),
            save_data.completed_puzzle_count,
            loaded_count
        );
    }
    drawing_text(save_status, 20, 690, 1, COLOR_BLACK);

    if (loaded_count == 0 && !loading) {
        drawing_text("No completed puzzles yet", 430, 320, 2, COLOR_BLACK);
        return;
    }

    if (search_active && filtered_count == 0 && !loading) {
        drawing_text("No completed puzzles found", 400, 320, 2, COLOR_BLACK);
        return;
    }

    int visible = filtered_count - scroll_offset;
    if (visible > HISTORY_VISIBLE_ROWS)
        visible = HISTORY_VISIBLE_ROWS;

    for (int i = 0; i < visible; i++) {
        int entry_index = filtered_entry_index_at(scroll_offset + i);
        if (entry_index < 0)
            continue;

        int y = HISTORY_START_Y + i * HISTORY_ROW_HEIGHT;
        draw_entry(&entries[entry_index], y, entry_index == selected_entry);
    }

    if (loading)
        drawing_text("Loading...", 1070, 670, 1, COLOR_BLACK);
}

void history_screen_exit(void) {}
