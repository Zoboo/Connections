#pragma once

#include <stdbool.h>
#include <stddef.h>

#define CONNECTIONS_WORD_COUNT 16
#define CONNECTIONS_GROUP_COUNT 4
#define CONNECTIONS_GROUP_SIZE 4
#define CONNECTIONS_WORD_MAX_LENGTH 32
#define CONNECTIONS_GROUP_NAME_MAX_LENGTH 64

#define CONNECTIONS_MAX_FILE_SIZE (8 * 1024 * 1024)
#define CONNECTIONS_SD_DIR "sdmc:/switch/Connections"
#define CONNECTIONS_SD_PATH CONNECTIONS_SD_DIR "/connections.json"
#define CONNECTIONS_BACKUP_PATH CONNECTIONS_SD_PATH ".bak"
#define CONNECTIONS_TEMP_PATH CONNECTIONS_SD_PATH ".tmp"
#define CONNECTIONS_ROMFS_PATH "romfs:/connections.json"

typedef struct {
    int puzzle_count;
    int playable_count;
    char latest_date[16];
} ConnectionsDataInfo;

typedef struct {
    int id;
    char date[16];
    char words[CONNECTIONS_WORD_COUNT][CONNECTIONS_WORD_MAX_LENGTH];
    char answers[CONNECTIONS_GROUP_COUNT][CONNECTIONS_GROUP_SIZE][CONNECTIONS_WORD_MAX_LENGTH];
    char group_names[CONNECTIONS_GROUP_COUNT][CONNECTIONS_GROUP_NAME_MAX_LENGTH];
    int group_levels[CONNECTIONS_GROUP_COUNT];
    int group_ids[CONNECTIONS_WORD_COUNT];
    int word_count;
} ConnectionsPuzzle;

bool connections_puzzle_load_random(const char *path, ConnectionsPuzzle *out_puzzle);
bool connections_puzzle_load_by_id(const char *path, int puzzle_id, ConnectionsPuzzle *out_puzzle, bool shuffle);
bool connections_puzzle_latest_date(const char *path, char *out_date, size_t out_size);
const char *connections_puzzle_data_path(void);
// Validate every record using the same parser used for gameplay.
bool connections_puzzle_validate_file(const char *path, ConnectionsDataInfo *out_info);
// Caller owns playable records in file order; entry count includes archival records.
// Multiplayer randomizes a separate in-memory index sequence.
bool connections_puzzle_prepare_race(const char *path, ConnectionsPuzzle **out_puzzles, int *out_count, int *out_entries);
