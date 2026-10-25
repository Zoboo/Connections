#pragma once
#include "screen.h"
#include "platform/platform_save.h"
#include "game/connections_puzzle.h"

void words_screen_open_puzzle_id(int puzzle_id);
void words_screen_open_custom_puzzle(const GameSaveCustomPuzzle *custom_puzzle);
void words_screen_init(void);
ScreenResult words_screen_update(PlatformInput input);
void words_screen_draw(void);
void words_screen_exit(void);
void words_screen_open_race_puzzle(const ConnectionsPuzzle *puzzle);
bool words_screen_take_race_completed(void);
bool words_screen_take_race_failed(void);
