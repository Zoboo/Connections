#include "multiplayer_screen.h"
#include "multiplayer/multiplayer.h"
#include "words_screen.h"
#include "core/config.h"
#include "platform/platform_save.h"
#include "platform/platform_player_profile.h"
#include "platform/platform_graphics.h"
#include "rendering/sdl_renderer.h"
#include "rendering/drawing.h"
#include "rendering/colors.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef enum { WIFI_CHOICE, SESSION } EntryState;
static EntryState entry;
static bool use_local, active_puzzle;
static MpSnapshot snapshot;
static MpRules rules;
static unsigned selection, scroll, local_score, puzzle_index;
static Uint32 last_nav, next_snapshot;
static char name[MP_NAME_LENGTH + 1], ui_error[96];
static SDL_Thread *preparer;
static SDL_atomic_t prepared;
static ConnectionsPuzzle *library;
static int library_count, library_entries;
static uint64_t prepared_match;
static bool count_published;
static unsigned lobby_focus;
static unsigned device_solved_count;
static bool solved_count_published;
static Image winner_crown;
static bool winner_crown_loaded;
static bool appearance_published;
static struct { uint64_t id; uint32_t revision; Image icon; } avatars[MP_MAX_PLAYERS];

// Textures are created only when a complete, changed avatar arrives, never in draw().
static void cache_avatars(void) {
    for (unsigned i=0;i<snapshot.player_count;i++) {
        const MpPlayer *player=&snapshot.players[i];
        if (avatars[i].id==player->id && avatars[i].revision==player->icon_revision && (!player->icon_revision || avatars[i].icon.texture)) continue;
        image_free(&avatars[i].icon);
        avatars[i].id=player->id;
        avatars[i].revision=player->icon_revision;
        if (!player->icon_revision) continue;
        SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormatFrom((void *)player->icon_rgba,
            MP_ICON_SIZE,MP_ICON_SIZE,32,MP_ICON_SIZE*4,SDL_PIXELFORMAT_RGBA32);
        if (!surface) continue;
        avatars[i].icon.texture=SDL_CreateTextureFromSurface(sdl_renderer_get(),surface);
        SDL_FreeSurface(surface);
        avatars[i].icon.width=avatars[i].icon.height=MP_ICON_SIZE;
    }
}
static void player_avatar(unsigned index,int x,int y,int size) {
    const Image *icon=&avatars[index].icon;
    if (snapshot.players[index].id==snapshot.own_id) icon=&platform_player_profile_get()->icon;
    if (!icon->texture) icon=platform_player_profile_fallback_icon();
    drawing_image_scaled(icon,x,y,size,size);
}
static void player_name_text(const char *text,int x,int y,int scale,uint32_t color) {
    // Preserve the exact profile color; the outline keeps pale colors readable.
    drawing_text(text,x-1,y,scale,COLOR_BLACK);
    drawing_text(text,x+1,y,scale,COLOR_BLACK);
    drawing_text(text,x,y-1,scale,COLOR_BLACK);
    drawing_text(text,x,y+1,scale,COLOR_BLACK);
    drawing_text(text,x,y,scale,color);
}

const char *multiplayer_screen_profile_scenario(void) {
    switch (snapshot.state) {
        case MP_DISCOVERING: return "discovery";
        case MP_CONNECTING: return "connecting";
        case MP_LOBBY: return "lobby";
        case MP_PREPARING: return "preparing";
        case MP_STARTING: return "countdown";
        case MP_PLAYING: return "match";
        case MP_FINALIZING: return "finalizing";
        case MP_RESULTS: return "winner";
        default: return "multiplayer";
    }
}

// The preparation worker owns its outputs until the release/acquire atomic fires.
static int prepare_library(void *unused) {
    (void)unused;
    GameSave *save = malloc(sizeof(*save));
    device_solved_count = 0;
    if (save) {
        if (platform_save_load(save) && save->completed_puzzle_count > 0)
            device_solved_count = (unsigned)save->completed_puzzle_count;
        free(save);
    }
    bool ok = connections_puzzle_prepare_race(connections_puzzle_data_path(), &library, &library_count, &library_entries);
    if(!ok) {free(library);library=NULL;library_count=library_entries=0;}
    SDL_AtomicSet(&prepared, ok ? 1 : -1);
    return 0;
}
static void release_library(void) {
    if (preparer) { SDL_WaitThread(preparer, NULL); preparer = NULL; }
    free(library); library = NULL; library_count = 0;
}
static bool keyboard(const char *title, char *text, size_t size) {
    SwkbdConfig config;
    if (R_FAILED(swkbdCreate(&config, 0))) { strcpy(ui_error, "Keyboard unavailable"); return false; }
    swkbdConfigMakePresetDefault(&config);
    swkbdConfigSetHeaderText(&config, title);
    swkbdConfigSetInitialText(&config, text);
    swkbdConfigSetStringLenMin(&config, 1);
    swkbdConfigSetStringLenMax(&config, size - 1);
    Result result = swkbdShow(&config, text, size);
    swkbdClose(&config);
    return R_SUCCEEDED(result);
}
static void begin_wifi(bool host) {
    char ip[16] = "192.168.1.50", port[6] = "42000";
    if ((!host && !keyboard("IP ADDRESS", ip, sizeof(ip))) || !keyboard("PORT", port, sizeof(port))) return;
    char *end;
    unsigned long number = strtoul(port, &end, 10);
    if (!port[0] || *end || number < 1 || number > 65535) { strcpy(ui_error, "Invalid port (1 - 65535)"); return; }
    if (!mp_begin_wifi(host, ip, (uint16_t)number, name)) { strcpy(ui_error, "Unable to queue connection"); return; }
    entry = SESSION; ui_error[0] = 0;
}
static bool touched(PlatformInput in, int x, int y, int w, int h) {
    return in.touch && in.touch_x >= x && in.touch_x < x+w && in.touch_y >= y && in.touch_y < y+h;
}
static void button(const char *label, int y, bool selected) {
    if (platform_graphics_modern()) {
        drawing_round_rect(410,y,460,58,29,selected?COLOR_BLACK:COLOR_WHITE);
        drawing_round_rect_outline(410,y,460,58,29,2,COLOR_BLACK);
        if (selected) drawing_round_rect_outline(406,y-4,468,66,33,2,0xFFDC7328);
        drawing_text(label,640-drawing_text_width(label,3)/2,y+16,3,selected?COLOR_WHITE:COLOR_BLACK);
        return;
    }
    drawing_rect(410, y, 460, 58, COLOR_BLACK);
    if (selected) drawing_rect_outline(406, y-4, 468, 66, 4, COLOR_WHITE);
    drawing_text(label, 640-drawing_text_width(label, 3)/2, y+16, 3, COLOR_WHITE);
}
void multiplayer_screen_select_transport(bool local) { use_local = local; }
void multiplayer_screen_init(void) {
    memset(&snapshot, 0, sizeof(snapshot));
    rules = (MpRules){MP_TIMED, 300, 10};
    selection = scroll = local_score = puzzle_index = 0;
    last_nav = next_snapshot = 0;
    if (!preparer || SDL_AtomicGet(&prepared)) { release_library(); prepared_match = 0; }
    active_puzzle = false; ui_error[0] = 0;
    count_published = false; lobby_focus = 0;
    mp_set_library(NULL,0,0);
    solved_count_published = false;
    if (!winner_crown_loaded)
        winner_crown_loaded = image_load_png("romfs:/images/crown.jpg", &winner_crown);
    if (!preparer) {
        SDL_AtomicSet(&prepared, 0);
        preparer = SDL_CreateThreadWithStackSize(prepare_library, "Race puzzles", 256*1024, NULL);
        if (!preparer) strcpy(ui_error, "Unable to prepare local puzzle library");
    }
    const LocalPlayerProfile *profile = platform_player_profile_get();
    snprintf(name, sizeof(name), "%s", profile->nickname);
    appearance_published = false;
    if (use_local) {
        entry = SESSION;
        if (!mp_begin_local(name)) strcpy(ui_error, "Local discovery unavailable");
    } else entry = WIFI_CHOICE;
}
static uint64_t elapsed_us(void) {
    uint64_t now = SDL_GetPerformanceCounter();
    return snapshot.start_ticks && now >= snapshot.start_ticks ?
        (now-snapshot.start_ticks)*1000000 / SDL_GetPerformanceFrequency() : 0;
}
static void advance_puzzle(void) {
    if (!library_count) return;
    ConnectionsPuzzle next;
    if(!mp_race_puzzle(snapshot.match_id,puzzle_index,&next)) {strcpy(ui_error,"Local race puzzle unavailable");return;}
    ++puzzle_index;
    words_screen_open_race_puzzle(&next);
    active_puzzle = true;
}

static unsigned target_limit(void) {
    unsigned limit = MP_MAX_PUZZLES;
    bool found = false;
    for (unsigned i = 0; i < snapshot.player_count; ++i) {
        if (!snapshot.players[i].connected) continue;
        unsigned count = snapshot.players[i].puzzle_count;
        if (!count) return 0;
        if (count < limit) limit = count;
        found = true;
    }
    return found ? limit : 0;
}

static void edit_rule_value(void) {
    char value[16], *end;
    if (rules.mode == MP_TIMED) {
        snprintf(value, sizeof(value), "%lu:%02lu", (unsigned long)(rules.duration_seconds/60),
                 (unsigned long)(rules.duration_seconds%60));
        if (!keyboard("TIME LIMIT (M:SS, 0:30 - 60:00)", value, sizeof(value))) return;
        char *separator = strchr(value, ':');
        if (!separator) { strcpy(ui_error, "Enter time as M:SS"); return; }
        *separator = 0;
        unsigned long minutes = strtoul(value, &end, 10);
        if (!value[0] || *end) { strcpy(ui_error, "Invalid time limit"); return; }
        unsigned long seconds = strtoul(separator+1, &end, 10);
        if (!separator[1] || *end || minutes > 60 || seconds > 59 ||
            minutes*60+seconds < 30 || minutes*60+seconds > 3600) {
            strcpy(ui_error, "Time limit must be 0:30 - 60:00"); return;
        }
        rules.duration_seconds = minutes*60+seconds;
    } else {
        unsigned limit = target_limit();
        if (!limit) limit=MP_MAX_PUZZLES;
        char title[80];
        snprintf(title, sizeof(title), "FIRST TO (1 - %u puzzles)", limit);
        snprintf(value, sizeof(value), "%lu", (unsigned long)rules.target_score);
        if (!keyboard(title, value, sizeof(value))) return;
        unsigned long amount = strtoul(value, &end, 10);
        if (!value[0] || *end || amount < 1 || amount > limit) {
            snprintf(ui_error, sizeof(ui_error), "Choose 1 - %u puzzles", limit); return;
        }
        rules.target_score = amount;
    }
    if (!mp_set_rules(rules)) strcpy(ui_error, "Unable to queue rule change");
    else ui_error[0] = 0;
}
ScreenResult multiplayer_screen_update(PlatformInput input) {
    if (input.b) { mp_stop(); return (ScreenResult){SCREEN_MAIN_MENU}; }
    Uint32 now = SDL_GetTicks();
    bool refresh = (Sint32)(now-next_snapshot) >= 0;
    // Consume published changes at 20 Hz, never poll sockets or discovery here.
    if (refresh) {
        next_snapshot = now + 50;
        if (mp_read_snapshot(&snapshot)) {
            if (snapshot.state == MP_LOBBY) {rules = snapshot.rules;active_puzzle=false;}
            if (snapshot.state==MP_LOBBY && target_limit() &&
                (!strcmp(ui_error,"Waiting for every player's puzzle count") ||
                 !strcmp(ui_error,"Preparing local puzzles..."))) ui_error[0]=0;
            cache_avatars();
            if(snapshot.error[0])snprintf(ui_error,sizeof(ui_error),"%s",snapshot.error);
        }
    }
    if (preparer && SDL_AtomicGet(&prepared)) {
        SDL_WaitThread(preparer,NULL); preparer=NULL;
        if (SDL_AtomicGet(&prepared) < 0) strcpy(ui_error, "No usable local puzzles");
    }
    if (refresh && !count_published && !preparer)
        count_published = mp_set_library(library,(unsigned)library_count,(unsigned)library_entries);
    if (refresh && !solved_count_published && !preparer)
        solved_count_published = mp_set_solved_count(device_solved_count);
    if (refresh && !appearance_published && entry==SESSION) {
        const LocalPlayerProfile *profile=platform_player_profile_get();
        appearance_published=mp_set_appearance(profile->color_id,profile->color,profile->avatar_rgba);
    }
    if (entry == WIFI_CHOICE) {
        if ((input.up || input.down) && now-last_nav >= 180) { selection ^= 1; last_nav = now; }
        if (touched(input,410,300,460,58)) { selection=0; input.a=true; }
        if (touched(input,410,390,460,58)) { selection=1; input.a=true; }
        if (input.a) begin_wifi(selection == 0);
        return (ScreenResult){SCREEN_MULTIPLAYER};
    }
    if (snapshot.match_id && prepared_match != snapshot.match_id && (snapshot.state == MP_PREPARING || snapshot.state == MP_STARTING || snapshot.state == MP_PLAYING)) {
        if (!count_published || preparer) return (ScreenResult){SCREEN_MULTIPLAYER};
        ConnectionsPuzzle first;
        bool usable=mp_race_puzzle(snapshot.match_id,0,&first);
        if (snapshot.state==MP_PREPARING && !mp_ready(usable)) return (ScreenResult){SCREEN_MULTIPLAYER};
        if (!usable) {
            strcpy(ui_error,"Unable to prepare local race sequence");
            return (ScreenResult){SCREEN_MULTIPLAYER};
        }
        prepared_match=snapshot.match_id;local_score=puzzle_index=0;active_puzzle=false;ui_error[0]=0;
    }
    if (snapshot.state == MP_PLAYING) {
        if (input.select && snapshot.player_count > 4) scroll = (scroll+1) % (snapshot.player_count-3);
        uint64_t elapsed = elapsed_us();
        bool expired = snapshot.rules.mode == MP_TIMED && elapsed >= (uint64_t)snapshot.rules.duration_seconds*1000000;
        bool target_done = snapshot.rules.mode == MP_TARGET_SCORE && local_score >= snapshot.rules.target_score;
        if (!active_puzzle && !expired && !target_done) advance_puzzle();
        if (active_puzzle && !expired && !target_done) {
            ScreenResult result = words_screen_update(input);
            if (result.next_screen == SCREEN_MAIN_MENU) { mp_stop(); return result; }
            if (words_screen_take_race_completed()) {
                elapsed = elapsed_us();
                if (snapshot.rules.mode != MP_TIMED || elapsed < (uint64_t)snapshot.rules.duration_seconds*1000000) {
                    ++local_score;
                    if (!mp_complete(local_score, elapsed)) strcpy(ui_error,"Score queue full; session ending");
                    advance_puzzle();
                }
            }
            // Exhausted attempts advance without awarding a completion.
            if (words_screen_take_race_failed()) advance_puzzle();
        }
    } else if (snapshot.state == MP_RESULTS || snapshot.state == MP_FINALIZING) {
        if(snapshot.is_host && snapshot.state==MP_RESULTS &&
           (input.start || touched(input,410,640,460,38)))mp_return_lobby();
        if ((input.up || input.down) && now-last_nav >= 180) {
            if (input.down && scroll+4 < snapshot.player_count) ++scroll;
            if (input.up && scroll) --scroll;
            last_nav=now;
        }
    } else if (snapshot.state == MP_LOBBY) {
        if ((input.up || input.down) && now-last_nav >= 180) {
            if (input.down && scroll+4 < snapshot.player_count) ++scroll;
            if (input.up && scroll) --scroll;
            last_nav=now;
            if (snapshot.is_host) {
                if (input.down) lobby_focus = lobby_focus < 2 ? 2 : 3;
                else lobby_focus = lobby_focus == 3 ? 2 : (rules.mode == MP_TIMED ? 0 : 1);
            }
        }
        if (snapshot.is_host) {
            if ((input.left || input.right) && now-last_nav >= 180) {
                lobby_focus = input.left ? 0 : 1; last_nav=now;
            }
            if (touched(input,350,390,280,58)) { lobby_focus=0; input.a=true; }
            if (touched(input,650,390,280,58)) { lobby_focus=1; input.a=true; }
            if (touched(input,410,475,460,58)) { lobby_focus=2; input.a=true; }
            if (touched(input,410,575,460,58)) { lobby_focus=3; input.a=true; }
            if (input.select) { lobby_focus=rules.mode==MP_TIMED?1:0; input.a=true; }
            if (input.a && lobby_focus < 2) {
                rules.mode = lobby_focus == 0 ? MP_TIMED : MP_TARGET_SCORE;
                unsigned cap = target_limit();
                if (cap && rules.target_score > cap) rules.target_score = cap;
                mp_set_rules(rules);
                edit_rule_value();
            } else if (input.a && lobby_focus == 2) edit_rule_value();
            if (input.start || (input.a && lobby_focus == 3)) {
                if (target_limit()) mp_prepare_match();
                else if (preparer) strcpy(ui_error, "Preparing local puzzles...");
                else if (!library_count) strcpy(ui_error, "No usable local puzzles");
                else strcpy(ui_error, "Waiting for every player's puzzle count");
            }
        }
    }
    return (ScreenResult){SCREEN_MULTIPLAYER};
}
static void player_list(int x,int y,bool scores) {
    drawing_text("PLAYERS",x,y,3,COLOR_BLACK);
    drawing_rect(x,y+26, drawing_text_width("PLAYERS",3),3,COLOR_BLACK);
    if (!scores) drawing_text("SOLVED",860,y,3,COLOR_BLACK);
    for(unsigned i=scroll;i<snapshot.player_count && i<scroll+4;i++) {
        char label[80];
        int row_y=y+44+(i-scroll)*(scores?105:44);
        snprintf(label,sizeof(label),"%.*s%s",scores?12:24,snapshot.players[i].name,
                 scores || snapshot.players[i].connected?"":" (LEFT)");
        player_avatar(i,x,row_y,scores?32:30);
        player_name_text(label,x+38,row_y,scores?2:3,snapshot.players[i].profile_color);
        snprintf(label,sizeof(label),"%lu",(unsigned long)(scores?snapshot.players[i].score:snapshot.players[i].solved_count));
        drawing_text(label,scores?x:860,scores?row_y+30:row_y,scores?4:3,COLOR_BLACK);
        if(scores && !snapshot.players[i].connected) drawing_text("LEFT",x+145,row_y+40,1,COLOR_BLACK);
    }
}
static void mode_button(const char *label, int x, MpMode mode, unsigned focus) {
    bool selected = rules.mode == mode;
    if (platform_graphics_modern()) {
        drawing_round_rect(x,390,280,58,29,selected?COLOR_BLACK:COLOR_WHITE);
        drawing_round_rect_outline(x,390,280,58,29,2,COLOR_BLACK);
        if (snapshot.is_host && lobby_focus==focus)
            drawing_round_rect_outline(x-4,386,288,66,33,2,0xFFDC7328);
        drawing_text(label,x+(280-drawing_text_width(label,3))/2,406,3,selected?COLOR_WHITE:COLOR_BLACK);
        return;
    }
    drawing_rect(x,390,280,58,selected?COLOR_BLACK:COLOR_WHITE);
    drawing_rect_outline(x,390,280,58,3,COLOR_BLACK);
    if (snapshot.is_host && lobby_focus == focus)
        drawing_rect_outline(x-4,386,288,66,4,0xFFD9739B);
    drawing_text(label,x+(280-drawing_text_width(label,3))/2,406,3,selected?COLOR_WHITE:COLOR_BLACK);
}
void multiplayer_screen_draw(void) {
    if (active_puzzle && (snapshot.state==MP_PLAYING || snapshot.state==MP_FINALIZING)) {
        words_screen_draw(); player_list(20,80,true);
        char timer[48]; uint64_t seconds=elapsed_us()/1000000;
        if(snapshot.rules.mode==MP_TIMED) {
            seconds=seconds<snapshot.rules.duration_seconds?snapshot.rules.duration_seconds-seconds:0;
            snprintf(timer,sizeof(timer),"LEFT %02llu:%02llu",(unsigned long long)(seconds/60),(unsigned long long)(seconds%60));
        } else snprintf(timer,sizeof(timer),"FIRST TO %lu",(unsigned long)snapshot.rules.target_score);
        drawing_text(timer,20,570,2,COLOR_BLACK);
        if(snapshot.state==MP_FINALIZING) drawing_text("FINALIZING...",20,610,1,COLOR_BLACK);
    } else {
        drawing_rect(0,0,SCREEN_WIDTH,SCREEN_HEIGHT,COLOR_MENU_BG);
        const char *heading=snapshot.state==MP_RESULTS?"CONGRATULATIONS!":snapshot.state==MP_LOBBY?"PLAYER LOBBY":"MULTIPLAYER";
        drawing_text(heading,640-drawing_text_width(heading,5)/2,50,5,COLOR_BLACK);
        if(entry==WIFI_CHOICE) { button("HOST",300,selection==0); button("JOIN",390,selection==1); }
        else if(snapshot.state==MP_LOBBY) {
            if (platform_graphics_modern()) {
                drawing_round_rect(250,130,780,240,6,COLOR_WHITE);
                drawing_round_rect_outline(250,130,780,240,6,1,0xFFE6EFEF);
            } else drawing_rect_outline(250,130,780,240,3,COLOR_BLACK);
            player_list(275,150,false);
            char rule[64];
            mode_button("TIME LIMIT",350,MP_TIMED,0);
            mode_button("FIRST TO",650,MP_TARGET_SCORE,1);
            if(rules.mode==MP_TIMED) snprintf(rule,sizeof(rule),"%lu:%02lu",(unsigned long)(rules.duration_seconds/60),(unsigned long)(rules.duration_seconds%60));
            else snprintf(rule,sizeof(rule),"%lu PUZZLES",(unsigned long)rules.target_score);
            button(rule,475,snapshot.is_host && lobby_focus==2);
            if(snapshot.is_host) {
                button("START RACE",575,lobby_focus==3);
            } else drawing_text("Waiting for host...",430,590,3,COLOR_BLACK);
        } else if(snapshot.state==MP_RESULTS) {
            int pulse=(SDL_GetTicks()%1800); pulse=pulse<=900?pulse:1800-pulse;
            int size=160+pulse*24/900;
            if(winner_crown_loaded) drawing_image_scaled(&winner_crown,640-size/2,200-size/2,size,size);
            unsigned winner_count=0,winner=0;
            for(unsigned i=0;i<snapshot.player_count;i++) if(snapshot.winner_mask&(1u<<i)) {
                ++winner_count; winner=i;
            }
            char headline[64];
            if(winner_count==1) snprintf(headline,sizeof(headline),"%.32s WINS!",snapshot.players[winner].name);
            else snprintf(headline,sizeof(headline),"%s",winner_count?"TIED WINNERS!":"NO WINNER");
            int headline_x=640-drawing_text_width(headline,4)/2;
            if(winner_count==1) {
                player_avatar(winner,headline_x-54,299,42);
                player_name_text(headline,headline_x,305,4,snapshot.players[winner].profile_color);
            } else drawing_text(headline,headline_x,305,4,COLOR_BLACK);
            for(unsigned i=scroll;i<snapshot.player_count && i<scroll+4;i++) {
                int y=380+(i-scroll)*72;
                bool won=(snapshot.winner_mask&(1u<<i))!=0;
                if (platform_graphics_modern()) drawing_round_rect(170,y-8,940,64,6,won?COLOR_UI_YELLOW:COLOR_WHITE);
                else drawing_rect(170,y-8,940,64,won?COLOR_WHITE:COLOR_MENU_BG);
                char label[80]; snprintf(label,sizeof(label),"%.24s%s",snapshot.players[i].name,snapshot.players[i].connected?"":" (LEFT)");
                player_avatar(i,190,y-3,48);
                player_name_text(label,250,y+8,3,snapshot.players[i].profile_color);
                snprintf(label,sizeof(label),"%lu",(unsigned long)snapshot.players[i].score);
                drawing_text(label,1060-drawing_text_width(label,6),y,6,COLOR_BLACK);
            }
            if(snapshot.is_host) {
                if (platform_graphics_modern()) drawing_round_rect(410,640,460,38,19,COLOR_BLACK);
                else drawing_rect(410,640,460,38,COLOR_BLACK);
                drawing_text("START: RETURN TO LOBBY",450,650,2,COLOR_WHITE);
            }
        } else {
            const char *status=snapshot.state==MP_DISCOVERING?"Discovering nearby Switches...":snapshot.state==MP_CONNECTING?"Connecting...":snapshot.state==MP_PREPARING?"Preparing puzzles / waiting for READY...":snapshot.state==MP_STARTING?"Get ready...":snapshot.state==MP_ERROR?snapshot.error:"Starting network...";
            drawing_text(status,180,330,2,COLOR_BLACK);
        }
    }
    if(ui_error[0]) drawing_text(ui_error,40,630,2,COLOR_BLACK);
    drawing_text("B: Cancel / Return to menu",28,690,2,COLOR_BLACK);
}
void multiplayer_screen_exit(void) {
    mp_stop(); active_puzzle=false; words_screen_exit();
    mp_set_library(NULL,0,0);
    // Cancellation never waits for the network. Preparation only joins once finished.
    if(!preparer || SDL_AtomicGet(&prepared)) release_library();
}
void multiplayer_screen_shutdown(void) {
    image_free(&winner_crown); winner_crown_loaded=false;
    release_library();
    for (unsigned i=0;i<MP_MAX_PLAYERS;i++) image_free(&avatars[i].icon);
}
