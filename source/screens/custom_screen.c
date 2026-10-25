#include "custom_screen.h"

#include "core/config.h"
#include "platform/platform_audio.h"
#include "platform/platform_save.h"
#include "platform/platform_graphics.h"
#include "platform/platform_transfer.h"
#include "rendering/colors.h"
#include "rendering/drawing.h"
#include "words_screen.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#define CUSTOM_ROW_X 130
#define CUSTOM_ROW_Y 110
#define CUSTOM_ROW_WIDTH 1020
#define CUSTOM_ROW_HEIGHT 82
#define CUSTOM_ROW_GAP 18
#define CUSTOM_VISIBLE_ROWS 5
#define CUSTOM_NAV_DELAY_MS 160
#define CUSTOM_OUTLINE_THICKNESS 4
#define CUSTOM_MODAL_X 330
#define CUSTOM_MODAL_Y 220
#define CUSTOM_MODAL_WIDTH 620
#define CUSTOM_MODAL_HEIGHT 260
#define CUSTOM_MODAL_BUTTON_WIDTH 160
#define CUSTOM_MODAL_BUTTON_HEIGHT 64
#define CUSTOM_MODAL_BUTTON_GAP 26
#define CUSTOM_ACTION_BUTTON_WIDTH 110
#define CUSTOM_ACTION_BUTTON_GAP 10
#define CUSTOM_RECEIVER_ROW_X 390
#define CUSTOM_RECEIVER_ROW_Y 288
#define CUSTOM_RECEIVER_ROW_WIDTH 500
#define CUSTOM_RECEIVER_ROW_HEIGHT 28
#define CUSTOM_RECEIVER_VISIBLE_ROWS 3
#define CUSTOM_LEVEL_YELLOW 0xFF6DDFF9
#define CUSTOM_LEVEL_GREEN  0xFF5AC3A0
#define CUSTOM_LEVEL_BLUE   0xFFEFC4B0
#define CUSTOM_LEVEL_PURPLE 0xFFC581BA
#define CUSTOM_LONG_PRESS_MS 700
#define CUSTOM_CREATE_X 28
#define CUSTOM_CREATE_Y 22
#define CUSTOM_CREATE_WIDTH 180
#define CUSTOM_CREATE_HEIGHT 58

typedef enum {
    CUSTOM_MODAL_NONE,
    CUSTOM_MODAL_ACTIONS,
    CUSTOM_MODAL_DELETE,
    CUSTOM_MODAL_TRANSFER
} CustomModalType;

typedef enum {
    CUSTOM_TRANSFER_SEND,
    CUSTOM_TRANSFER_RECEIVE
} CustomTransferMode;

static GameSave save_data;
static int selected_entry = 0;
static int scroll_offset = 0;
static int modal_selected = 0;
static Uint32 last_nav_time = 0;
static CustomModalType modal_type = CUSTOM_MODAL_NONE;
static bool can_press_a = false;
static bool a_holding = false;
static bool a_long_handled = false;
static Uint32 a_hold_start = 0;
static bool touch_tracking = false;
static bool touch_long_handled = false;
static int touch_entry = -1;
static Uint32 touch_hold_start = 0;
static CustomTransferMode transfer_mode = CUSTOM_TRANSFER_SEND;
static int selected_receiver = 0;
static bool receiving_in_background = false;
static bool create_selected = false;
static bool create_confirm_armed = false;

static int text_width(const char *text, int scale) {
    return drawing_text_width(text, scale);
}

static bool point_in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static u32 level_color(int level) {
    if (level == 0)
        return CUSTOM_LEVEL_YELLOW;

    if (level == 1)
        return CUSTOM_LEVEL_GREEN;

    if (level == 2)
        return CUSTOM_LEVEL_BLUE;

    return CUSTOM_LEVEL_PURPLE;
}

static void keep_selected_visible(void) {
    if (selected_entry < scroll_offset)
        scroll_offset = selected_entry;

    if (selected_entry >= scroll_offset + CUSTOM_VISIBLE_ROWS)
        scroll_offset = selected_entry - CUSTOM_VISIBLE_ROWS + 1;
}

static int visible_count(void) {
    int visible = save_data.custom_puzzle_count - scroll_offset;
    if (visible > CUSTOM_VISIBLE_ROWS)
        visible = CUSTOM_VISIBLE_ROWS;

    return visible;
}

static int touched_entry_index(int px, int py) {
    int visible = visible_count();

    for (int i = 0; i < visible; i++) {
        int y = CUSTOM_ROW_Y + i * (CUSTOM_ROW_HEIGHT + CUSTOM_ROW_GAP);
        if (point_in_rect(px, py, CUSTOM_ROW_X, y, CUSTOM_ROW_WIDTH, CUSTOM_ROW_HEIGHT))
            return scroll_offset + i;
    }

    return -1;
}

static int modal_button_x(int button_index) {
    int button_count = 2;
    int button_width = CUSTOM_MODAL_BUTTON_WIDTH;
    int button_gap = CUSTOM_MODAL_BUTTON_GAP;

    if (modal_type == CUSTOM_MODAL_ACTIONS) {
        button_count = 5;
        button_width = CUSTOM_ACTION_BUTTON_WIDTH;
        button_gap = CUSTOM_ACTION_BUTTON_GAP;
    } else if (modal_type == CUSTOM_MODAL_TRANSFER) {
        button_count = 1;
    }

    int total_width = button_width * button_count + button_gap * (button_count - 1);
    return CUSTOM_MODAL_X + (CUSTOM_MODAL_WIDTH - total_width) / 2 +
           button_index * (button_width + button_gap);
}

static int touched_modal_button(int px, int py) {
    int y = CUSTOM_MODAL_Y + 160;
    int button_count = 2;
    int button_width = CUSTOM_MODAL_BUTTON_WIDTH;

    if (modal_type == CUSTOM_MODAL_ACTIONS) {
        button_count = 5;
        button_width = CUSTOM_ACTION_BUTTON_WIDTH;
    } else if (modal_type == CUSTOM_MODAL_TRANSFER) {
        button_count = 1;
    }

    for (int i = 0; i < button_count; i++) {
        if (point_in_rect(px, py, modal_button_x(i), y, button_width, CUSTOM_MODAL_BUTTON_HEIGHT))
            return i;
    }

    return -1;
}

static int touched_receiver_index(int px, int py) {
    int count = platform_transfer_receiver_count();
    if (count > CUSTOM_RECEIVER_VISIBLE_ROWS)
        count = CUSTOM_RECEIVER_VISIBLE_ROWS;

    for (int i = 0; i < count; i++) {
        int y = CUSTOM_RECEIVER_ROW_Y + i * (CUSTOM_RECEIVER_ROW_HEIGHT + 8);
        if (point_in_rect(px, py, CUSTOM_RECEIVER_ROW_X, y, CUSTOM_RECEIVER_ROW_WIDTH, CUSTOM_RECEIVER_ROW_HEIGHT))
            return i;
    }

    return -1;
}

static ScreenResult play_selected_custom(void) {
    if (selected_entry < 0 || selected_entry >= save_data.custom_puzzle_count)
        return (ScreenResult){ SCREEN_CUSTOM };

    words_screen_open_custom_puzzle(&save_data.custom_puzzles[selected_entry]);
    return (ScreenResult){ SCREEN_WORDS };
}

static void delete_selected_custom(void) {
    if (selected_entry < 0 || selected_entry >= save_data.custom_puzzle_count)
        return;

    for (int i = selected_entry; i < save_data.custom_puzzle_count - 1; i++)
        save_data.custom_puzzles[i] = save_data.custom_puzzles[i + 1];

    save_data.custom_puzzle_count--;

    if (selected_entry >= save_data.custom_puzzle_count)
        selected_entry = save_data.custom_puzzle_count - 1;

    if (selected_entry < 0)
        selected_entry = 0;

    keep_selected_visible();
    platform_save_write(&save_data);
}

static const char *custom_puzzle_name(int index) {
    if (index >= 0 &&
        index < save_data.custom_puzzle_count &&
        save_data.custom_puzzles[index].name[0] != '\0') {
        return save_data.custom_puzzles[index].name;
    }

    return NULL;
}

static void default_custom_puzzle_name(int index, char *out, size_t out_size) {
    const char *name = custom_puzzle_name(index);
    if (name) {
        strncpy(out, name, out_size);
        out[out_size - 1] = '\0';
        return;
    }

    snprintf(out, out_size, "CUSTOM PUZZLE %d", index + 1);
}

static void rename_selected_custom(void) {
    if (selected_entry < 0 || selected_entry >= save_data.custom_puzzle_count)
        return;

    SwkbdConfig keyboard;
    char output[GAME_SAVE_CUSTOM_TEXT_LENGTH] = {0};
    char current_name[GAME_SAVE_CUSTOM_TEXT_LENGTH];
    Result rc = swkbdCreate(&keyboard, 0);
    if (R_FAILED(rc))
        return;

    default_custom_puzzle_name(selected_entry, current_name, sizeof(current_name));

    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, "Rename Custom Puzzle");
    swkbdConfigSetGuideText(&keyboard, "Enter puzzle name");
    swkbdConfigSetOkButtonText(&keyboard, "OK");
    swkbdConfigSetStringLenMin(&keyboard, 1);
    swkbdConfigSetStringLenMax(&keyboard, GAME_SAVE_CUSTOM_TEXT_LENGTH - 1);
    swkbdConfigSetInitialText(&keyboard, current_name);

    rc = swkbdShow(&keyboard, output, sizeof(output));
    if (R_SUCCEEDED(rc) && output[0] != '\0') {
        strncpy(save_data.custom_puzzles[selected_entry].name, output, GAME_SAVE_CUSTOM_TEXT_LENGTH);
        save_data.custom_puzzles[selected_entry].name[GAME_SAVE_CUSTOM_TEXT_LENGTH - 1] = '\0';
        platform_save_write(&save_data);
    }

    swkbdClose(&keyboard);
}

static void send_selected_custom(void) {
    if (selected_entry < 0 || selected_entry >= save_data.custom_puzzle_count)
        return;

    platform_transfer_scan_receivers_for_custom_puzzle(&save_data.custom_puzzles[selected_entry]);
    transfer_mode = CUSTOM_TRANSFER_SEND;
    selected_receiver = 0;
    modal_type = CUSTOM_MODAL_TRANSFER;
}

static void receive_custom(void) {
    platform_transfer_start_receiving();
    transfer_mode = CUSTOM_TRANSFER_RECEIVE;
    receiving_in_background = false;
    modal_selected = 0;
    modal_type = CUSTOM_MODAL_TRANSFER;
}

static void import_received_custom_if_ready(void) {
    GameSaveCustomPuzzle received;

    if (!platform_transfer_take_received_custom_puzzle(&received))
        return;

    if (platform_save_add_custom_puzzle(&save_data, &received)) {
        platform_save_write(&save_data);
        selected_entry = save_data.custom_puzzle_count - 1;
        keep_selected_visible();
        receiving_in_background = false;
    } else {
        receiving_in_background = false;
    }

    platform_transfer_cancel();
    modal_type = CUSTOM_MODAL_NONE;
}

static void open_selected_modal(void) {
    if (save_data.custom_puzzle_count <= 0)
        return;

    modal_selected = 0;
    modal_type = CUSTOM_MODAL_ACTIONS;
}

static void open_delete_modal(void) {
    if (save_data.custom_puzzle_count <= 0)
        return;

    modal_selected = 1;
    modal_type = CUSTOM_MODAL_DELETE;
}

void custom_screen_init(void) {
    if (!platform_save_load(&save_data)) {
        platform_save_defaults(&save_data);
        platform_save_write(&save_data);
    }

    selected_entry = 0;
    create_selected = save_data.custom_puzzle_count <= 0;
    create_confirm_armed = false;
    scroll_offset = 0;
    modal_selected = 0;
    last_nav_time = 0;
    modal_type = CUSTOM_MODAL_NONE;
    can_press_a = false;
    a_holding = false;
    a_long_handled = false;
    a_hold_start = 0;
    touch_tracking = false;
    touch_long_handled = false;
    touch_entry = -1;
    touch_hold_start = 0;
    transfer_mode = CUSTOM_TRANSFER_SEND;
    platform_transfer_cancel();
}

ScreenResult custom_screen_update(PlatformInput input) {
    platform_transfer_update();
    import_received_custom_if_ready();

    int receiver_count = platform_transfer_receiver_count();
    if (selected_receiver >= receiver_count && receiver_count > 0)
        selected_receiver = receiver_count - 1;
    if (selected_receiver < 0)
        selected_receiver = 0;

    if (input.b) {
        if (modal_type != CUSTOM_MODAL_NONE) {
            if (modal_type == CUSTOM_MODAL_TRANSFER)
                platform_transfer_cancel();

            modal_type = CUSTOM_MODAL_NONE;
            can_press_a = false;
            return (ScreenResult){ SCREEN_CUSTOM };
        }

        return (ScreenResult){ SCREEN_MAIN_MENU };
    }

    Uint32 now = SDL_GetTicks();
    if (!input.a)
        create_confirm_armed = true;

    if (modal_type == CUSTOM_MODAL_NONE &&
        ((input.touch && point_in_rect(input.touch_x, input.touch_y, CUSTOM_CREATE_X, CUSTOM_CREATE_Y,
                                      CUSTOM_CREATE_WIDTH, CUSTOM_CREATE_HEIGHT)) ||
         (input.a && create_selected && create_confirm_armed))) {
        platform_audio_play_pop();
        return (ScreenResult){ SCREEN_CREATE };
    }

    if (modal_type != CUSTOM_MODAL_NONE && !input.a)
        can_press_a = true;

    if (input.touch) {
        if (modal_type != CUSTOM_MODAL_NONE) {
            if (modal_type == CUSTOM_MODAL_TRANSFER &&
                platform_transfer_state() == PLATFORM_TRANSFER_RECEIVING) {
                modal_type = CUSTOM_MODAL_NONE;
                receiving_in_background = true;
                can_press_a = false;
                return (ScreenResult){ SCREEN_CUSTOM };
            }

            int button = touched_modal_button(input.touch_x, input.touch_y);
            int receiver = modal_type == CUSTOM_MODAL_TRANSFER &&
                           platform_transfer_state() == PLATFORM_TRANSFER_SCANNING
                               ? touched_receiver_index(input.touch_x, input.touch_y)
                               : -1;

            if (receiver >= 0) {
                platform_audio_play_pop();
                selected_receiver = receiver;
                platform_transfer_send_to_receiver(selected_receiver);
                can_press_a = false;
            } else if (button >= 0) {
                platform_audio_play_pop();
                modal_selected = button;
                CustomModalType activated_modal = modal_type;
                modal_type = CUSTOM_MODAL_NONE;

                if (activated_modal == CUSTOM_MODAL_ACTIONS) {
                    if (modal_selected == 0)
                        return play_selected_custom();

                    if (modal_selected == 1)
                        send_selected_custom();

                    if (modal_selected == 2)
                        receive_custom();

                    if (modal_selected == 3)
                        open_delete_modal();

                    if (modal_selected == 4)
                        rename_selected_custom();
                } else if (activated_modal == CUSTOM_MODAL_DELETE && modal_selected == 0) {
                    delete_selected_custom();
                } else if (activated_modal == CUSTOM_MODAL_TRANSFER) {
                    if (platform_transfer_state() == PLATFORM_TRANSFER_RECEIVING) {
                        receiving_in_background = true;
                        modal_type = CUSTOM_MODAL_NONE;
                    } else {
                        platform_transfer_cancel();
                    }
                }
            }
        } else {
            int entry = touched_entry_index(input.touch_x, input.touch_y);
            if (entry >= 0) {
                platform_audio_play_pop();
                create_selected = false;
                selected_entry = entry;
                touch_tracking = true;
                touch_long_handled = false;
                touch_entry = entry;
                touch_hold_start = now;
            }
        }

        can_press_a = false;
    }

    if (modal_type == CUSTOM_MODAL_NONE && touch_tracking) {
        if (input.touch_down) {
            if (!touch_long_handled && now - touch_hold_start >= CUSTOM_LONG_PRESS_MS) {
                selected_entry = touch_entry;
                open_delete_modal();
                touch_long_handled = true;
            }
        } else {
            if (!touch_long_handled) {
                selected_entry = touch_entry;
                open_selected_modal();
            }

            touch_tracking = false;
            touch_entry = -1;
        }
    }

    if (now - last_nav_time >= CUSTOM_NAV_DELAY_MS) {
        if (modal_type != CUSTOM_MODAL_NONE) {
            int button_count = modal_type == CUSTOM_MODAL_ACTIONS ? 5 : 2;

            if (modal_type == CUSTOM_MODAL_TRANSFER)
                button_count = 1;

            if (modal_type == CUSTOM_MODAL_TRANSFER &&
                platform_transfer_state() == PLATFORM_TRANSFER_SCANNING &&
                receiver_count > 0) {
                if (input.down && selected_receiver < receiver_count - 1) {
                    selected_receiver++;
                    platform_audio_play_pop();
                    last_nav_time = now;
                } else if (input.up && selected_receiver > 0) {
                    selected_receiver--;
                    platform_audio_play_pop();
                    last_nav_time = now;
                }
            } else if (input.left && modal_selected > 0) {
                modal_selected--;
                platform_audio_play_pop();
                last_nav_time = now;
            } else if (input.right && modal_selected < button_count - 1) {
                modal_selected++;
                platform_audio_play_pop();
                last_nav_time = now;
            }
        } else {
            if (input.down && create_selected) {
                create_selected = false;
                platform_audio_play_pop();
                last_nav_time = now;
            } else if (input.up && selected_entry == 0 && !create_selected) {
                create_selected = true;
                platform_audio_play_pop();
                last_nav_time = now;
            } else if (input.down && selected_entry < save_data.custom_puzzle_count - 1) {
                selected_entry++;
                keep_selected_visible();
                platform_audio_play_pop();
                last_nav_time = now;
            } else if (input.up && selected_entry > 0 && !create_selected) {
                selected_entry--;
                keep_selected_visible();
                platform_audio_play_pop();
                last_nav_time = now;
            }
        }
    }

    if (modal_type != CUSTOM_MODAL_NONE && input.a && can_press_a) {
        platform_audio_play_pop();
        can_press_a = false;

        if (modal_type == CUSTOM_MODAL_TRANSFER &&
            platform_transfer_state() == PLATFORM_TRANSFER_SCANNING &&
            receiver_count > 0) {
            platform_transfer_send_to_receiver(selected_receiver);
            return (ScreenResult){ SCREEN_CUSTOM };
        }

        if (modal_type == CUSTOM_MODAL_TRANSFER &&
            platform_transfer_state() == PLATFORM_TRANSFER_RECEIVING) {
            receiving_in_background = true;
            modal_type = CUSTOM_MODAL_NONE;
            return (ScreenResult){ SCREEN_CUSTOM };
        }

        CustomModalType activated_modal = modal_type;
        modal_type = CUSTOM_MODAL_NONE;

        if (activated_modal == CUSTOM_MODAL_ACTIONS) {
            if (modal_selected == 0)
                return play_selected_custom();

            if (modal_selected == 1)
                send_selected_custom();

            if (modal_selected == 2)
                receive_custom();

            if (modal_selected == 3)
                open_delete_modal();

            if (modal_selected == 4)
                rename_selected_custom();
        } else if (activated_modal == CUSTOM_MODAL_DELETE && modal_selected == 0) {
            delete_selected_custom();
        } else if (activated_modal == CUSTOM_MODAL_TRANSFER) {
            platform_transfer_cancel();
        }
    }

    if (modal_type == CUSTOM_MODAL_NONE && !create_selected) {
        if (input.a && save_data.custom_puzzle_count <= 0) {
            platform_audio_play_pop();
            receive_custom();
            can_press_a = false;
        } else if (input.a && !a_holding && save_data.custom_puzzle_count > 0) {
            platform_audio_play_pop();
            a_holding = true;
            a_long_handled = false;
            a_hold_start = now;
        }

        if (input.a && a_holding && !a_long_handled && now - a_hold_start >= CUSTOM_LONG_PRESS_MS) {
            open_delete_modal();
            a_long_handled = true;
        }

        if (!input.a && a_holding) {
            if (!a_long_handled)
                open_selected_modal();

            a_holding = false;
        }
    }

    return (ScreenResult){ SCREEN_CUSTOM };
}

static void draw_centered_text(const char *text, int x, int y, int w, int h, int scale, u32 color) {
    int width = text_width(text, scale);
    drawing_text(text, x + (w - width) / 2, y + (h - 7 * scale) / 2, scale, color);
}

static void build_preview_line(const GameSaveCustomPuzzle *puzzle, char *out, size_t out_size) {
    out[0] = '\0';

    for (int group = 0; group < GAME_SAVE_CUSTOM_GROUPS; group++) {
        if (group > 0)
            strncat(out, " / ", out_size - strlen(out) - 1);

        strncat(out, puzzle->group_names[group], out_size - strlen(out) - 1);
    }
}

static void draw_entry(int entry_index, int y) {
    const GameSaveCustomPuzzle *puzzle = &save_data.custom_puzzles[entry_index];
    char title[64];
    char preview[GAME_SAVE_CUSTOM_GROUPS * GAME_SAVE_CUSTOM_TEXT_LENGTH];
    bool selected = entry_index == selected_entry && !create_selected;

    default_custom_puzzle_name(entry_index, title, sizeof(title));
    build_preview_line(puzzle, preview, sizeof(preview));

    bool modern = platform_graphics_modern();
    if (modern)
        drawing_round_rect(CUSTOM_ROW_X, y, CUSTOM_ROW_WIDTH, CUSTOM_ROW_HEIGHT, 6, 0xFFE6EFEF);
    else
        drawing_rect(CUSTOM_ROW_X, y, CUSTOM_ROW_WIDTH, CUSTOM_ROW_HEIGHT, COLOR_WHITE);

    for (int group = 0; group < GAME_SAVE_CUSTOM_GROUPS; group++) {
        int swatch_x = CUSTOM_ROW_X + 16 + group * 30;
        if (modern)
            drawing_round_rect(swatch_x, y + 16, 22, 22, 4, level_color(puzzle->group_levels[group]));
        else
            drawing_rect(swatch_x, y + 16, 22, 22, level_color(puzzle->group_levels[group]));
    }

    drawing_text(title, CUSTOM_ROW_X + 160, y + 14, 2, COLOR_BLACK);
    drawing_text(preview, CUSTOM_ROW_X + 160, y + 48, 1, COLOR_BLACK);

    if (modern) {
        if (selected && modal_type == CUSTOM_MODAL_NONE)
            drawing_round_rect_outline(CUSTOM_ROW_X, y, CUSTOM_ROW_WIDTH, CUSTOM_ROW_HEIGHT,
                                       6, 2, 0xFFDC7328);
    } else if (modal_type == CUSTOM_MODAL_NONE) {
        drawing_rect_outline(
            CUSTOM_ROW_X,
            y,
            CUSTOM_ROW_WIDTH,
            CUSTOM_ROW_HEIGHT,
            selected ? CUSTOM_OUTLINE_THICKNESS : 2,
            selected ? COLOR_BLACK : COLOR_GRAY
        );
    } else {
        drawing_rect_outline(CUSTOM_ROW_X, y, CUSTOM_ROW_WIDTH, CUSTOM_ROW_HEIGHT, 2, COLOR_GRAY);
    }
}

static void draw_modal_button(const char *label, int index) {
    int x = modal_button_x(index);
    int y = CUSTOM_MODAL_Y + 160;
    int width = modal_type == CUSTOM_MODAL_ACTIONS ? CUSTOM_ACTION_BUTTON_WIDTH : CUSTOM_MODAL_BUTTON_WIDTH;
    bool selected = modal_selected == index;
    u32 fill_color = selected ? COLOR_BLACK : COLOR_WHITE;
    u32 text_color = selected ? COLOR_WHITE : COLOR_BLACK;
    int outline = selected ? CUSTOM_OUTLINE_THICKNESS : 2;

    if (platform_graphics_modern()) {
        drawing_round_rect(x, y, width, CUSTOM_MODAL_BUTTON_HEIGHT,
                           CUSTOM_MODAL_BUTTON_HEIGHT / 2, fill_color);
        drawing_round_rect_outline(x, y, width, CUSTOM_MODAL_BUTTON_HEIGHT,
                                   CUSTOM_MODAL_BUTTON_HEIGHT / 2, 2, COLOR_BLACK);
        if (selected)
            drawing_round_rect_outline(x - 4, y - 4, width + 8, CUSTOM_MODAL_BUTTON_HEIGHT + 8,
                                       CUSTOM_MODAL_BUTTON_HEIGHT / 2 + 4, 2, 0xFFDC7328);
        draw_centered_text(label, x, y, width, CUSTOM_MODAL_BUTTON_HEIGHT, 2, text_color);
        return;
    }

    drawing_rect(x, y, width, CUSTOM_MODAL_BUTTON_HEIGHT, fill_color);
    drawing_rect_outline(
        x,
        y,
        width,
        CUSTOM_MODAL_BUTTON_HEIGHT,
        outline,
        COLOR_BLACK
    );
    draw_centered_text(label, x, y, width, CUSTOM_MODAL_BUTTON_HEIGHT, 2, text_color);
}

static void draw_modal_panel(void) {
    if (platform_graphics_modern()) {
        drawing_round_rect(CUSTOM_MODAL_X, CUSTOM_MODAL_Y, CUSTOM_MODAL_WIDTH, CUSTOM_MODAL_HEIGHT, 6, COLOR_WHITE);
        drawing_round_rect_outline(CUSTOM_MODAL_X, CUSTOM_MODAL_Y, CUSTOM_MODAL_WIDTH, CUSTOM_MODAL_HEIGHT, 6, 2, COLOR_BLACK);
    } else {
        drawing_rect(CUSTOM_MODAL_X, CUSTOM_MODAL_Y, CUSTOM_MODAL_WIDTH, CUSTOM_MODAL_HEIGHT, COLOR_WHITE);
        drawing_rect_outline(CUSTOM_MODAL_X, CUSTOM_MODAL_Y, CUSTOM_MODAL_WIDTH, CUSTOM_MODAL_HEIGHT, 4, COLOR_BLACK);
    }
}

static void draw_action_modal(void) {
    char title[GAME_SAVE_CUSTOM_TEXT_LENGTH];

    default_custom_puzzle_name(selected_entry, title, sizeof(title));

    draw_modal_panel();
    draw_centered_text(title, CUSTOM_MODAL_X, CUSTOM_MODAL_Y + 70, CUSTOM_MODAL_WIDTH, 40, 2, COLOR_BLACK);
    draw_modal_button("PLAY", 0);
    draw_modal_button("SEND", 1);
    draw_modal_button("RECEIVE", 2);
    draw_modal_button("DELETE", 3);
    draw_modal_button("RENAME", 4);
}

static void draw_delete_modal(void) {
    draw_modal_panel();
    draw_centered_text("ARE YOU SURE YOU WANT", CUSTOM_MODAL_X, CUSTOM_MODAL_Y + 42, CUSTOM_MODAL_WIDTH, 34, 2, COLOR_BLACK);
    draw_centered_text("TO DELETE THIS PUZZLE", CUSTOM_MODAL_X, CUSTOM_MODAL_Y + 82, CUSTOM_MODAL_WIDTH, 34, 2, COLOR_BLACK);
    draw_modal_button("YES", 0);
    draw_modal_button("NO", 1);
}

static void draw_transfer_modal(void) {
    char seconds_text[64];
    PlatformTransferState state = platform_transfer_state();
    const char *active_label = transfer_mode == CUSTOM_TRANSFER_RECEIVE ? "RECEIVING" : "SENDING";
    const char *complete_label = transfer_mode == CUSTOM_TRANSFER_RECEIVE ? "PUZZLE RECEIVED" : "SEND TEST COMPLETE";
    const char *failed_label = transfer_mode == CUSTOM_TRANSFER_RECEIVE ? "RECEIVE FAILED" : "SEND TEST FAILED";
    int receiver_count = platform_transfer_receiver_count();

    draw_modal_panel();

    if (state == PLATFORM_TRANSFER_SCANNING) {
        draw_centered_text("SELECT RECEIVER", CUSTOM_MODAL_X, CUSTOM_MODAL_Y + 28, CUSTOM_MODAL_WIDTH, 30, 2, COLOR_BLACK);
        draw_centered_text(platform_transfer_status(), CUSTOM_MODAL_X + 20, CUSTOM_MODAL_Y + 66, CUSTOM_MODAL_WIDTH - 40, 24, 1, COLOR_BLACK);

        if (receiver_count <= 0) {
            draw_centered_text("NO RECEIVERS FOUND", CUSTOM_MODAL_X, CUSTOM_MODAL_Y + 116, CUSTOM_MODAL_WIDTH, 24, 2, COLOR_BLACK);
        } else {
            int visible = receiver_count;
            if (visible > CUSTOM_RECEIVER_VISIBLE_ROWS)
                visible = CUSTOM_RECEIVER_VISIBLE_ROWS;

            for (int i = 0; i < visible; i++) {
                int y = CUSTOM_RECEIVER_ROW_Y + i * (CUSTOM_RECEIVER_ROW_HEIGHT + 8);
                bool selected = i == selected_receiver;
                if (platform_graphics_modern()) {
                    drawing_round_rect(CUSTOM_RECEIVER_ROW_X, y, CUSTOM_RECEIVER_ROW_WIDTH, CUSTOM_RECEIVER_ROW_HEIGHT,
                                       6, selected ? COLOR_BLACK : 0xFFE6EFEF);
                    if (selected)
                        drawing_round_rect_outline(CUSTOM_RECEIVER_ROW_X - 3, y - 3,
                                                   CUSTOM_RECEIVER_ROW_WIDTH + 6, CUSTOM_RECEIVER_ROW_HEIGHT + 6,
                                                   9, 2, 0xFFDC7328);
                } else {
                    drawing_rect(CUSTOM_RECEIVER_ROW_X, y, CUSTOM_RECEIVER_ROW_WIDTH, CUSTOM_RECEIVER_ROW_HEIGHT, selected ? COLOR_BLACK : COLOR_WHITE);
                    drawing_rect_outline(CUSTOM_RECEIVER_ROW_X, y, CUSTOM_RECEIVER_ROW_WIDTH, CUSTOM_RECEIVER_ROW_HEIGHT, selected ? 3 : 2, COLOR_BLACK);
                }
                draw_centered_text(
                    platform_transfer_receiver_name(i),
                    CUSTOM_RECEIVER_ROW_X,
                    y,
                    CUSTOM_RECEIVER_ROW_WIDTH,
                    CUSTOM_RECEIVER_ROW_HEIGHT,
                    1,
                    selected ? COLOR_WHITE : COLOR_BLACK
                );
            }
        }

        draw_modal_button("CANCEL", 0);
        return;
    }

    if (state == PLATFORM_TRANSFER_SENDING || state == PLATFORM_TRANSFER_RECEIVING) {
        draw_centered_text(active_label, CUSTOM_MODAL_X, CUSTOM_MODAL_Y + 42, CUSTOM_MODAL_WIDTH, 34, 3, COLOR_BLACK);
        if (state == PLATFORM_TRANSFER_RECEIVING)
            snprintf(
                seconds_text,
                sizeof(seconds_text),
                "PRESS OK TO HIDE"
            );
        else
            snprintf(seconds_text, sizeof(seconds_text), "%d SECONDS LEFT", platform_transfer_seconds_remaining());
    } else if (state == PLATFORM_TRANSFER_DONE) {
        draw_centered_text(complete_label, CUSTOM_MODAL_X, CUSTOM_MODAL_Y + 42, CUSTOM_MODAL_WIDTH, 34, 2, COLOR_BLACK);
        snprintf(seconds_text, sizeof(seconds_text), transfer_mode == CUSTOM_TRANSFER_RECEIVE ? "ADDED TO CUSTOM LIST" : "SEND WINDOW CLOSED");
    } else if (state == PLATFORM_TRANSFER_ERROR) {
        draw_centered_text(failed_label, CUSTOM_MODAL_X, CUSTOM_MODAL_Y + 42, CUSTOM_MODAL_WIDTH, 34, 2, COLOR_BLACK);
        snprintf(seconds_text, sizeof(seconds_text), "CHECK HARDWARE AND LDN ACCESS");
    } else {
        draw_centered_text("TRANSFER", CUSTOM_MODAL_X, CUSTOM_MODAL_Y + 42, CUSTOM_MODAL_WIDTH, 34, 3, COLOR_BLACK);
        snprintf(seconds_text, sizeof(seconds_text), "IDLE");
    }

    draw_centered_text(platform_transfer_status(), CUSTOM_MODAL_X + 20, CUSTOM_MODAL_Y + 92, CUSTOM_MODAL_WIDTH - 40, 28, 1, COLOR_BLACK);
    draw_centered_text(seconds_text, CUSTOM_MODAL_X + 20, CUSTOM_MODAL_Y + 122, CUSTOM_MODAL_WIDTH - 40, 28, 1, COLOR_BLACK);

    draw_modal_button(state == PLATFORM_TRANSFER_SENDING ? "CANCEL" : "OK", 0);
}

void custom_screen_draw(void) {
    drawing_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_MENU_BG);

    int title_width = text_width("Custom Puzzles", 3);
    drawing_text("Custom Puzzles", (SCREEN_WIDTH - title_width) / 2, 36, 3, COLOR_BLACK);

    bool create_highlight = create_selected && modal_type == CUSTOM_MODAL_NONE;
    if (platform_graphics_modern()) {
        drawing_round_rect(CUSTOM_CREATE_X, CUSTOM_CREATE_Y, CUSTOM_CREATE_WIDTH, CUSTOM_CREATE_HEIGHT,
                           CUSTOM_CREATE_HEIGHT / 2, create_highlight ? COLOR_BLACK : COLOR_WHITE);
        drawing_round_rect_outline(CUSTOM_CREATE_X, CUSTOM_CREATE_Y, CUSTOM_CREATE_WIDTH, CUSTOM_CREATE_HEIGHT,
                                   CUSTOM_CREATE_HEIGHT / 2, 2, COLOR_BLACK);
        if (create_highlight)
            drawing_round_rect_outline(CUSTOM_CREATE_X - 4, CUSTOM_CREATE_Y - 4, CUSTOM_CREATE_WIDTH + 8,
                                       CUSTOM_CREATE_HEIGHT + 8, CUSTOM_CREATE_HEIGHT / 2 + 4, 2, 0xFFDC7328);
    } else {
    drawing_rect(CUSTOM_CREATE_X, CUSTOM_CREATE_Y, CUSTOM_CREATE_WIDTH, CUSTOM_CREATE_HEIGHT,
                 create_highlight ? COLOR_BLACK : COLOR_WHITE);
    drawing_rect_outline(CUSTOM_CREATE_X, CUSTOM_CREATE_Y, CUSTOM_CREATE_WIDTH, CUSTOM_CREATE_HEIGHT,
                         create_highlight ? CUSTOM_OUTLINE_THICKNESS : 2, COLOR_BLACK);
    }
    draw_centered_text("CREATE", CUSTOM_CREATE_X, CUSTOM_CREATE_Y, CUSTOM_CREATE_WIDTH, CUSTOM_CREATE_HEIGHT,
                       3, create_highlight ? COLOR_WHITE : COLOR_BLACK);

    if (save_data.custom_puzzle_count <= 0) {
        draw_centered_text("NO CUSTOM PUZZLES YET", 0, 310, SCREEN_WIDTH, 40, 3, COLOR_BLACK);
        draw_centered_text("PRESS A TO RECEIVE ONE", 0, 360, SCREEN_WIDTH, 28, 2, COLOR_BLACK);

        if (modal_type == CUSTOM_MODAL_TRANSFER)
            draw_transfer_modal();

        return;
    }

    int visible = visible_count();
    for (int i = 0; i < visible; i++) {
        int entry_index = scroll_offset + i;
        int y = CUSTOM_ROW_Y + i * (CUSTOM_ROW_HEIGHT + CUSTOM_ROW_GAP);
        draw_entry(entry_index, y);
    }

    char count_text[64];
    if (platform_transfer_state() == PLATFORM_TRANSFER_RECEIVING) {
        snprintf(
            count_text,
            sizeof(count_text),
            "RECEIVE %s",
            receiving_in_background ? "ON" : "OPEN"
        );
    } else {
        snprintf(count_text, sizeof(count_text), "%d CUSTOM PUZZLES", save_data.custom_puzzle_count);
    }
    drawing_text(count_text, 20, 690, 1, COLOR_BLACK);

    if (modal_type == CUSTOM_MODAL_ACTIONS)
        draw_action_modal();
    else if (modal_type == CUSTOM_MODAL_DELETE)
        draw_delete_modal();
    else if (modal_type == CUSTOM_MODAL_TRANSFER)
        draw_transfer_modal();
}

void custom_screen_exit(void) {
    platform_transfer_cancel();
}
