#pragma once

#include <stdbool.h>

#define GAME_SAVE_MAX_COMPLETED_PUZZLES 2048
#define GAME_SAVE_MAX_CUSTOM_PUZZLES 64
#define GAME_SAVE_DATE_LENGTH 11
#define GAME_SAVE_CUSTOM_TEXT_LENGTH 32
#define GAME_SAVE_CUSTOM_GROUPS 4
#define GAME_SAVE_CUSTOM_MEMBERS 4

typedef struct {
    char name[GAME_SAVE_CUSTOM_TEXT_LENGTH];
    char group_names[GAME_SAVE_CUSTOM_GROUPS][GAME_SAVE_CUSTOM_TEXT_LENGTH];
    char words[GAME_SAVE_CUSTOM_GROUPS][GAME_SAVE_CUSTOM_MEMBERS][GAME_SAVE_CUSTOM_TEXT_LENGTH];
    int group_levels[GAME_SAVE_CUSTOM_GROUPS];
} GameSaveCustomPuzzle;

typedef struct {
    int completed_puzzle_ids[GAME_SAVE_MAX_COMPLETED_PUZZLES];
    char completed_puzzle_dates[GAME_SAVE_MAX_COMPLETED_PUZZLES][GAME_SAVE_DATE_LENGTH];
    int completed_puzzle_best_times[GAME_SAVE_MAX_COMPLETED_PUZZLES];
    int completed_puzzle_count;
    GameSaveCustomPuzzle custom_puzzles[GAME_SAVE_MAX_CUSTOM_PUZZLES];
    int custom_puzzle_count;
    int current_streak;
    int best_streak;
    char last_played_date[GAME_SAVE_DATE_LENGTH];
} GameSave;

bool platform_save_init(void);
void platform_save_exit(void);

void platform_save_defaults(GameSave *save);
bool platform_save_load(GameSave *save);
bool platform_save_write(const GameSave *save);
bool platform_save_add_custom_puzzle(GameSave *save, const GameSaveCustomPuzzle *custom_puzzle);

const char *platform_save_backend_name(void);
const char *platform_save_path(void);

bool platform_save_has_completed_puzzle(const GameSave *save, int puzzle_id);
bool platform_save_mark_completed_today(GameSave *save, int puzzle_id, int completed_time_seconds);
