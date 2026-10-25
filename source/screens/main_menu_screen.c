#include "main_menu_screen.h"
#include "multiplayer_screen.h"

#include "core/config.h"

#include "game/connections_puzzle.h"
#include "platform/platform_audio.h"
#include "platform/platform_updates.h"
#include "platform/platform_graphics.h"
#include "core/version.h"
#include "rendering/image.h"
#include "rendering/drawing.h"
#include "rendering/colors.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

#define CONNECTIONS_BANNER "romfs:/connections-banner.png"
#define HISTORY_BUTTON_IMAGE "romfs:/history.png"
#define DOWNLOAD_BUTTON_IMAGE "romfs:/download.png"

#define MENU_TOP_Y         80
#define MENU_BANNER_GAP    40
#define MENU_BUTTON_GAP    24
#define MENU_BUTTON_WIDTH  300
#define MENU_BUTTON_HEIGHT 72
#define MENU_BUTTON_COLOR  COLOR_BLACK
#define MENU_BUTTON_TEXT_COLOR COLOR_WHITE
#define MENU_SELECTED_BORDER_THICKNESS 5
#define MENU_SELECTED_BORDER_DARK_PINK 0xFFD9739B
#define MENU_BANNER_PULSE_MS 1800
#define MENU_BANNER_PULSE_PIXELS 10
#define MENU_NAV_DELAY_MS 180
#define MENU_HOVER_GROW_PIXELS 8
#define MENU_HISTORY_SIZE 72
#define MENU_HISTORY_MARGIN 28
#define MENU_DOWNLOAD_SIZE 72
#define MENU_DOWNLOAD_MARGIN 28
#define MENU_MODAL_X 300
#define MENU_MODAL_Y 230
#define MENU_MODAL_WIDTH 680
#define MENU_MODAL_HEIGHT 250

typedef struct {
    const char *label;
    int x;
    int y;
    int width;
    int height;
} MenuButton;

static Image banner_image;
static Image history_image;
static Image download_image;
static bool banner_loaded = false;
static bool history_loaded = false;
static bool download_loaded = false;
static MenuButton play_button;
static MenuButton custom_button;
static MenuButton multiplayer_button;
static MenuButton history_button;
static MenuButton download_button;
static bool multiplayer_modal_open = false;
static bool local_selected = true;
static bool multiplayer_confirm_armed = false;
static const MenuButton local_button = { "LOCAL", MENU_MODAL_X + 36, MENU_MODAL_Y + 120, 280, 64 };
static const MenuButton wifi_button = { "WIFI", MENU_MODAL_X + 364, MENU_MODAL_Y + 120, 280, 64 };
static const MenuButton update_yes = { "INSTALL", MENU_MODAL_X + 36, MENU_MODAL_Y + 178, 280, 48 };
static const MenuButton update_no = { "LATER", MENU_MODAL_X + 364, MENU_MODAL_Y + 178, 280, 48 };
static unsigned update_revision;
static bool update_feedback = false;
static bool accept_selected = false;
static int selected_button = 0;
static Uint32 last_nav_time = 0;
static bool update_modal_open = false;
static char latest_puzzle_label[64] = "";
static bool options_open;
static bool options_modern;
static bool options_save_failed;
static const MenuButton options_button = { "OPTIONS", 1080, 20, 172, 48 };
static const MenuButton legacy_button = { "LEGACY", MENU_MODAL_X + 36, MENU_MODAL_Y + 120, 280, 48 };
static const MenuButton modern_button = { "MODERN", MENU_MODAL_X + 364, MENU_MODAL_Y + 120, 280, 48 };

static void center_button(MenuButton *button, int y) {
    button->x = (SCREEN_WIDTH - button->width) / 2;
    button->y = y;
}

static void layout_menu(void) {
    int button_y = MENU_TOP_Y + MENU_BANNER_GAP;

    if (banner_loaded)
        button_y += banner_image.height;

    center_button(&play_button, button_y);
    center_button(&multiplayer_button, play_button.y + play_button.height + MENU_BUTTON_GAP);
    center_button(&custom_button, multiplayer_button.y + multiplayer_button.height + MENU_BUTTON_GAP);

    history_button.x = SCREEN_WIDTH - history_button.width - MENU_HISTORY_MARGIN;
    history_button.y = SCREEN_HEIGHT - history_button.height - MENU_HISTORY_MARGIN;

    download_button.x = MENU_DOWNLOAD_MARGIN;
    download_button.y = SCREEN_HEIGHT - download_button.height - MENU_DOWNLOAD_MARGIN;
}

static int text_width(const char *text, int scale) {
    return drawing_text_width(text, scale);
}

static bool point_in_button(int x, int y, const MenuButton *button) {
    return x >= button->x &&
           x < button->x + button->width &&
           y >= button->y &&
           y < button->y + button->height;
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

    return interpolate_color(COLOR_WHITE, MENU_SELECTED_BORDER_DARK_PINK, amount, half_cycle);
}

static int banner_pulse_amount(void) {
    int half_cycle = MENU_BANNER_PULSE_MS / 2;
    int time = SDL_GetTicks() % MENU_BANNER_PULSE_MS;
    int amount = time <= half_cycle ? time : MENU_BANNER_PULSE_MS - time;

    return amount * MENU_BANNER_PULSE_PIXELS / half_cycle;
}

static const char *month_name(int month) {
    static const char *months[] = {
        "January",
        "February",
        "March",
        "April",
        "May",
        "June",
        "July",
        "August",
        "September",
        "October",
        "November",
        "December"
    };

    if (month < 1 || month > 12)
        return "";

    return months[month - 1];
}

static void load_latest_puzzle_label(void) {
    char latest_date[16];
    int year = 0;
    int month = 0;
    int day = 0;

    latest_puzzle_label[0] = '\0';

    if (!connections_puzzle_latest_date(connections_puzzle_data_path(), latest_date, sizeof(latest_date)))
        return;

    if (sscanf(latest_date, "%d-%d-%d", &year, &month, &day) != 3)
        return;

    snprintf(
        latest_puzzle_label,
        sizeof(latest_puzzle_label),
        "Latest Puzzle: %s %d, %d",
        month_name(month),
        day,
        year
    );
}

static void draw_button(const MenuButton *button, bool selected) {
    int scale = 4;
    int label_width = text_width(button->label, scale);
    int grow = selected ? MENU_HOVER_GROW_PIXELS : 0;
    int x = button->x - grow;
    int y = button->y - grow;
    int width = button->width + grow * 2;
    int height = button->height + grow * 2;
    int text_x = x + (width - label_width) / 2;
    int text_y = y + (height - 7 * scale) / 2;

    if (platform_graphics_modern()) {
        x=button->x; y=button->y; width=button->width; height=button->height;
        bool strong=selected;
        drawing_round_rect(x,y,width,height,height/2,strong?COLOR_BLACK:COLOR_WHITE);
        drawing_round_rect_outline(x,y,width,height,height/2,2,COLOR_BLACK);
        if (selected) drawing_round_rect_outline(x-4,y-4,width+8,height+8,height/2+4,2,0xFFDC7328);
        int label_scale=button->height<=48?3:4;
        drawing_text(button->label,x+(width-text_width(button->label,label_scale))/2,
            y+(height-7*label_scale)/2,label_scale,strong?COLOR_WHITE:COLOR_BLACK);
        return;
    }

    drawing_rect(x, y, width, height, MENU_BUTTON_COLOR);
    if (selected) {
        drawing_rect_outline(
            x,
            y,
            width,
            height,
            MENU_SELECTED_BORDER_THICKNESS,
            selected_border_color()
        );
    }
    drawing_text(button->label, text_x, text_y, scale, MENU_BUTTON_TEXT_COLOR);
}

static void draw_image_button(const MenuButton *button, const Image *image, bool loaded, bool selected) {
    int grow = selected ? MENU_HOVER_GROW_PIXELS : 0;
    int x = button->x - grow;
    int y = button->y - grow;
    int width = button->width + grow * 2;
    int height = button->height + grow * 2;

    if (loaded)
        drawing_image_scaled(image, x, y, width, height);
    else
        draw_button(button, false);

    if (selected) {
        drawing_rect_outline(
            x,
            y,
            width,
            height,
            MENU_SELECTED_BORDER_THICKNESS,
            selected_border_color()
        );
    }
}

void main_menu_screen_init(void) {
    banner_loaded = image_load_png(CONNECTIONS_BANNER, &banner_image);
    history_loaded = image_load_png(HISTORY_BUTTON_IMAGE, &history_image);
    download_loaded = image_load_png(DOWNLOAD_BUTTON_IMAGE, &download_image);

    play_button = (MenuButton){ "PLAY", 0, 0, MENU_BUTTON_WIDTH, MENU_BUTTON_HEIGHT };
    custom_button = (MenuButton){ "CUSTOM", 0, 0, MENU_BUTTON_WIDTH, MENU_BUTTON_HEIGHT };
    multiplayer_button = (MenuButton){ "MULTIPLAYER", 0, 0, MENU_BUTTON_WIDTH, MENU_BUTTON_HEIGHT };
    history_button = (MenuButton){
        "HISTORY",
        0,
        0,
        MENU_HISTORY_SIZE,
        MENU_HISTORY_SIZE
    };
    download_button = (MenuButton){
        "DOWNLOAD",
        0,
        0,
        MENU_DOWNLOAD_SIZE,
        MENU_DOWNLOAD_SIZE
    };
    multiplayer_modal_open = false;
    local_selected = true;
    selected_button = 0;
    update_modal_open = platform_updates_state() == UPDATES_APP_AVAILABLE ||
                        platform_updates_state() == UPDATES_PUZZLES_AVAILABLE;
    accept_selected = false;
    last_nav_time = 0;
    load_latest_puzzle_label();

    layout_menu();
}

ScreenResult main_menu_screen_update(PlatformInput input) {
    if (options_open) {
        Uint32 now=SDL_GetTicks();
        if ((input.left || input.right || input.up || input.down) && now-last_nav_time>=MENU_NAV_DELAY_MS) {
            options_modern=!options_modern;
            last_nav_time=now;
            platform_audio_play_pop();
        }
        bool apply=input.a;
        if (input.touch && point_in_button(input.touch_x,input.touch_y,&legacy_button)) {
            options_modern=false; apply=true;
        } else if (input.touch && point_in_button(input.touch_x,input.touch_y,&modern_button)) {
            options_modern=true; apply=true;
        }
        if (apply) {
            options_save_failed=!platform_graphics_set_modern(options_modern);
            platform_audio_play_pop();
        }
        if (input.b || input.select) options_open=false;
        return (ScreenResult){SCREEN_MAIN_MENU};
    }
    if (update_revision != platform_updates_revision()) {
        update_revision = platform_updates_revision();
        PlatformUpdateState state = platform_updates_state();
        if (state == UPDATES_RESTART_READY)
            return (ScreenResult){ SCREEN_EXIT };
        if (state == UPDATES_APP_AVAILABLE || state == UPDATES_PUZZLES_AVAILABLE) {
            update_modal_open = true;
            accept_selected = false;
        } else if (state == UPDATES_FINISHED) {
            update_modal_open = update_feedback;
            load_latest_puzzle_label();
        }
        // A press intended for the menu must not confirm a newly opened prompt.
        return (ScreenResult){ SCREEN_MAIN_MENU };
    }

    if (update_modal_open) {
        PlatformUpdateState state = platform_updates_state();
        if (platform_updates_busy()) {
            if (input.b || (input.touch && point_in_button(input.touch_x, input.touch_y, &update_no)))
                platform_updates_cancel();
            return (ScreenResult){ SCREEN_MAIN_MENU };
        }
        if (state == UPDATES_APP_AVAILABLE || state == UPDATES_PUZZLES_AVAILABLE) {
            Uint32 now = SDL_GetTicks();
            if ((input.left || input.right || input.up || input.down) && now - last_nav_time >= MENU_NAV_DELAY_MS) {
                accept_selected = !accept_selected;
                last_nav_time = now;
                platform_audio_play_pop();
            }
            bool accept = input.a && accept_selected;
            bool skip = input.b || (input.a && !accept_selected);
            if (input.touch) {
                accept = point_in_button(input.touch_x, input.touch_y, &update_yes);
                skip = point_in_button(input.touch_x, input.touch_y, &update_no);
            }
            if (accept) {
                platform_audio_play_pop();
                update_feedback = true;
                platform_updates_accept();
                if (platform_updates_state() == UPDATES_FINISHED)
                    load_latest_puzzle_label();
            } else if (skip) {
                platform_audio_play_pop();
                platform_updates_skip();
                update_modal_open = update_feedback;
            }
        } else if (input.a || input.b || input.touch) {
            platform_audio_play_pop();
            platform_updates_skip();
            update_modal_open = false;
            update_feedback = false;
        }
        return (ScreenResult){ SCREEN_MAIN_MENU };
    }

    if (multiplayer_modal_open) {
        Uint32 now = SDL_GetTicks();
        if (!input.a)
            multiplayer_confirm_armed = true;
        if (input.b) {
            multiplayer_modal_open = false;
            platform_audio_play_pop();
            return (ScreenResult){ SCREEN_MAIN_MENU };
        }
        if ((input.left || input.right || input.up || input.down) && now - last_nav_time >= MENU_NAV_DELAY_MS) {
            local_selected = !local_selected;
            last_nav_time = now;
            platform_audio_play_pop();
        }
        bool activate = input.a && multiplayer_confirm_armed;
        if (input.touch) {
            if (point_in_button(input.touch_x, input.touch_y, &local_button)) {
                local_selected = true;
                activate = true;
            } else if (point_in_button(input.touch_x, input.touch_y, &wifi_button)) {
                local_selected = false;
                activate = true;
            }
        }
        if (activate) {
            platform_audio_play_pop();
            multiplayer_screen_select_transport(local_selected);
            return (ScreenResult){ SCREEN_MULTIPLAYER };
        }
        return (ScreenResult){ SCREEN_MAIN_MENU };
    }

    if (input.touch) {
        const MenuButton *buttons[] = { &play_button, &multiplayer_button, &custom_button,
                                        &history_button, &download_button, &options_button };
        for (int i = 0; i < 6; i++) {
            if (point_in_button(input.touch_x, input.touch_y, buttons[i])) {
                selected_button = i;
                input.a = true;
                break;
            }
        }
    }

    Uint32 now = SDL_GetTicks();
    if ((input.up || input.down) && now - last_nav_time >= MENU_NAV_DELAY_MS) {
        selected_button = (selected_button + (input.down ? 1 : 5)) % 6;
        platform_audio_play_pop();
        last_nav_time = now;
    }

    if (input.select || (input.a && selected_button==5)) {
        options_open=true;
        options_modern=platform_graphics_modern();
        options_save_failed=false;
        return (ScreenResult){SCREEN_MAIN_MENU};
    }
    if (input.a) {
        platform_audio_play_pop();
        if (selected_button == 0)
            return (ScreenResult){ SCREEN_WORDS };
        if (selected_button == 1) {
            multiplayer_modal_open = true;
            local_selected = true;
            multiplayer_confirm_armed = false;
            return (ScreenResult){ SCREEN_MAIN_MENU };
        }
        if (selected_button == 2)
            return (ScreenResult){ SCREEN_CUSTOM };
        if (selected_button == 3)
            return (ScreenResult){ SCREEN_HISTORY };

        update_feedback = true;
        update_modal_open = true;
        accept_selected = false;
        platform_updates_start();
    }
    return (ScreenResult){ SCREEN_MAIN_MENU };
}

void main_menu_screen_draw(void) {
    drawing_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_MENU_BG);

    if (banner_loaded) {
        int pulse = banner_pulse_amount();
        int banner_width = banner_image.width + pulse * 2;
        int banner_height = banner_image.height + pulse * 2;
        int banner_x = (SCREEN_WIDTH - banner_width) / 2;
        int banner_y = MENU_TOP_Y - pulse;

        drawing_image_scaled(&banner_image, banner_x, banner_y, banner_width, banner_height);
    } else {
        drawing_text("Connections", (SCREEN_WIDTH - text_width("Connections", 4)) / 2, MENU_TOP_Y, 4, COLOR_BLACK);
    }

    draw_button(&play_button, selected_button == 0);
    draw_button(&multiplayer_button, selected_button == 1);
    draw_button(&custom_button, selected_button == 2);
    draw_image_button(&history_button, &history_image, history_loaded, selected_button == 3);
    draw_image_button(&download_button, &download_image, download_loaded, selected_button == 4);
    draw_button(&options_button,selected_button==5);

    if (latest_puzzle_label[0] != '\0') {
        int scale = 2;
        int label_width = text_width(latest_puzzle_label, scale);
        drawing_text(
            latest_puzzle_label,
            (SCREEN_WIDTH - label_width) / 2,
            SCREEN_HEIGHT - 42,
            scale,
            COLOR_BLACK
        );
    }

    drawing_text("v" CONNECTIONS_APP_VERSION, 28, 28, 1, COLOR_BLACK);
    if (platform_updates_busy() && !update_modal_open)
        drawing_text(platform_updates_status(), 28, 52, 1, COLOR_BLACK);

    if (options_open) {
        drawing_rect(0,0,SCREEN_WIDTH,SCREEN_HEIGHT,0x66000000);
        if (platform_graphics_modern()) drawing_round_rect(MENU_MODAL_X,MENU_MODAL_Y,MENU_MODAL_WIDTH,MENU_MODAL_HEIGHT,12,COLOR_WHITE);
        else drawing_rect(MENU_MODAL_X,MENU_MODAL_Y,MENU_MODAL_WIDTH,MENU_MODAL_HEIGHT,COLOR_WHITE);
        drawing_text("GRAPHICS",640-text_width("GRAPHICS",3)/2,MENU_MODAL_Y+32,3,COLOR_BLACK);
        draw_button(&legacy_button,!options_modern);
        draw_button(&modern_button,options_modern);
        drawing_text(options_save_failed?"Could not save preference":"A: Apply   B: Back",MENU_MODAL_X+36,MENU_MODAL_Y+210,2,COLOR_BLACK);
    }
    if (multiplayer_modal_open && !update_modal_open && !options_open) {
        if (platform_graphics_modern()) {
            drawing_rect(0,0,SCREEN_WIDTH,SCREEN_HEIGHT,0x66000000);
            drawing_round_rect(MENU_MODAL_X,MENU_MODAL_Y,MENU_MODAL_WIDTH,MENU_MODAL_HEIGHT,12,COLOR_WHITE);
        } else {
        drawing_rect(MENU_MODAL_X, MENU_MODAL_Y, MENU_MODAL_WIDTH, MENU_MODAL_HEIGHT, COLOR_WHITE);
        drawing_rect_outline(MENU_MODAL_X, MENU_MODAL_Y, MENU_MODAL_WIDTH, MENU_MODAL_HEIGHT, 4, COLOR_BLACK);
        }
        int width = text_width("MULTIPLAYER", 3);
        drawing_text("MULTIPLAYER", MENU_MODAL_X + (MENU_MODAL_WIDTH - width) / 2, MENU_MODAL_Y + 32, 3, COLOR_BLACK);
        draw_button(&local_button, local_selected);
        draw_button(&wifi_button, !local_selected);
        drawing_text("B: Back", MENU_MODAL_X + 36, MENU_MODAL_Y + 214, 2, COLOR_BLACK);
    }

    if (update_modal_open && !options_open) {
        if (platform_graphics_modern()) {
            drawing_rect(0,0,SCREEN_WIDTH,SCREEN_HEIGHT,0x66000000);
            drawing_round_rect(MENU_MODAL_X,MENU_MODAL_Y,MENU_MODAL_WIDTH,MENU_MODAL_HEIGHT,12,COLOR_WHITE);
        } else {
        drawing_rect(MENU_MODAL_X, MENU_MODAL_Y, MENU_MODAL_WIDTH, MENU_MODAL_HEIGHT, COLOR_WHITE);
        drawing_rect_outline(MENU_MODAL_X, MENU_MODAL_Y, MENU_MODAL_WIDTH, MENU_MODAL_HEIGHT, 4, COLOR_BLACK);
        }

        PlatformUpdateState state = platform_updates_state();
        bool prompt = state == UPDATES_APP_AVAILABLE || state == UPDATES_PUZZLES_AVAILABLE;
        bool nsp_offer = platform_updates_is_nsp_offer();
        const char *title = nsp_offer ? "VERSION UPDATE" : state == UPDATES_APP_AVAILABLE || state == UPDATES_DOWNLOADING_APP ?
                            "APP UPDATE" : state == UPDATES_PUZZLES_AVAILABLE ? "NEW PUZZLES" : "UPDATES";
        int title_scale = 3;
        int title_width = text_width(title, title_scale);
        drawing_text(title, MENU_MODAL_X + (MENU_MODAL_WIDTH - title_width) / 2, MENU_MODAL_Y + 28, title_scale, COLOR_BLACK);
        if (nsp_offer) {
            char available[80];
            snprintf(available,sizeof(available),"VERSION %s IS AVAILABLE.",platform_updates_available_version());
            drawing_text(available,640-text_width(available,2)/2,MENU_MODAL_Y+86,2,COLOR_BLACK);
        } else drawing_text(platform_updates_status(), MENU_MODAL_X + 24, MENU_MODAL_Y + 86, 2, COLOR_BLACK);
        if (platform_updates_busy()) {
            int percent = platform_updates_progress();
            int x = MENU_MODAL_X + 36, y = MENU_MODAL_Y + 132, width = MENU_MODAL_WIDTH - 72;
            drawing_rect_outline(x,y,width,22,2,COLOR_BLACK);
            if (percent >= 0) {
                drawing_rect(x+4,y+4,(width-8)*percent/100,14,COLOR_BLACK);
            } else {
                // Indeterminate until Content-Length arrives; animation uses only local time.
                int offset = (SDL_GetTicks()/8) % (2*(width-88));
                if (offset > width-88) offset = 2*(width-88)-offset;
                drawing_rect(x+4+offset,y+4,80,14,COLOR_BLACK);
            }
        }
        if (prompt || (nsp_offer && platform_updates_busy())) {
            if (!nsp_offer) drawing_text(state == UPDATES_APP_AVAILABLE ? "Download, install and restart the app?" :
                         "Install the updated puzzle list?", MENU_MODAL_X + 36, MENU_MODAL_Y + 128, 2, COLOR_BLACK);
            MenuButton accept = update_yes;
            MenuButton later = update_no;
            if (nsp_offer) { accept.label = "UPDATE"; later.label = "LATER"; }
            draw_button(&accept, accept_selected);
            draw_button(&later, !accept_selected);
        } else if (platform_updates_busy()) {
            MenuButton cancel = update_no;
            cancel.label = "CANCEL";
            draw_button(&cancel, false);
            drawing_text("B: Cancel", MENU_MODAL_X + 36, MENU_MODAL_Y + 192, 2, COLOR_BLACK);
        } else {
            drawing_text("A / B / Tap: Close", MENU_MODAL_X + 36, MENU_MODAL_Y + 192, 2, COLOR_BLACK);
        }
    }
}

void main_menu_screen_exit(void) {
    if (banner_loaded) {
        image_free(&banner_image);
        banner_loaded = false;
    }

    if (history_loaded) {
        image_free(&history_image);
        history_loaded = false;
    }

    if (download_loaded) {
        image_free(&download_image);
        download_loaded = false;
    }
}
