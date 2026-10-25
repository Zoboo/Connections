#include "platform_save.h"

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SAVE_MOUNT_NAME "save"
#define SAVE_PATH "save:/save.json"
#define SD_SAVE_DIR "sdmc:/switch/Connections"
#define SD_SAVE_PATH SD_SAVE_DIR "/save.json"
#define SAVE_DATA_SIZE 0x20000
#define SAVE_JOURNAL_SIZE 0x20000
#define FORCE_SD_SAVE_FOR_DEV 1

static bool save_mounted = false;
static bool sd_save_fallback = false;

static u64 current_application_id(void) {
    u64 application_id = FS_SAVEDATA_CURRENT_APPLICATIONID;
    appletGetCurrentApplicationId(&application_id);
    return application_id;
}

static bool get_user(AccountUid *uid) {
    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc))
        return false;

    rc = accountGetPreselectedUser(uid);
    if (R_FAILED(rc))
        rc = accountTrySelectUserWithoutInteraction(uid, false);

    accountExit();

    return R_SUCCEEDED(rc);
}

static bool create_save_data(AccountUid uid) {
    FsSaveDataAttribute attr = {0};
    FsSaveDataCreationInfo creation_info = {0};
    FsSaveDataMetaInfo meta = {0};
    u64 application_id = current_application_id();

    attr.application_id = application_id;
    attr.uid = uid;
    attr.save_data_type = FsSaveDataType_Account;
    attr.save_data_rank = FsSaveDataRank_Primary;

    creation_info.save_data_size = SAVE_DATA_SIZE;
    creation_info.journal_size = SAVE_JOURNAL_SIZE;
    creation_info.available_size = SAVE_DATA_SIZE;
    creation_info.owner_id = application_id;
    creation_info.save_data_space_id = FsSaveDataSpaceId_User;

    meta.type = FsSaveDataMetaType_None;
    meta.size = 0;

    return R_SUCCEEDED(fsCreateSaveDataFileSystem(&attr, &creation_info, &meta));
}

bool platform_save_init(void) {
    AccountUid uid = {0};
    u64 application_id = current_application_id();

    save_mounted = false;
    sd_save_fallback = false;

#if FORCE_SD_SAVE_FOR_DEV
    mkdir("sdmc:/switch", 0777);
    mkdir(SD_SAVE_DIR, 0777);
    sd_save_fallback = true;
    return true;
#endif

    if (!get_user(&uid)) {
        mkdir("sdmc:/switch", 0777);
        mkdir(SD_SAVE_DIR, 0777);
        sd_save_fallback = true;
        return true;
    }

    Result rc = fsdevMountSaveData(SAVE_MOUNT_NAME, application_id, uid);
    if (R_FAILED(rc)) {
        create_save_data(uid);
        rc = fsdevMountSaveData(SAVE_MOUNT_NAME, application_id, uid);
    }

    save_mounted = R_SUCCEEDED(rc);
    if (!save_mounted) {
        mkdir("sdmc:/switch", 0777);
        mkdir(SD_SAVE_DIR, 0777);
        sd_save_fallback = true;
    }

    return save_mounted || sd_save_fallback;
}

void platform_save_exit(void) {
    if (save_mounted) {
        fsdevUnmountDevice(SAVE_MOUNT_NAME);
        save_mounted = false;
    }

    sd_save_fallback = false;
}

void platform_save_defaults(GameSave *save) {
    memset(save, 0, sizeof(*save));
}

const char *platform_save_backend_name(void) {
    if (save_mounted)
        return "Official";

    if (sd_save_fallback)
        return "SD";

    return "None";
}

const char *platform_save_path(void) {
    return save_mounted ? SAVE_PATH : SD_SAVE_PATH;
}

static bool read_text_file(const char *path, char **out_text) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size <= 0) {
        fclose(file);
        return false;
    }

    char *text = malloc((size_t)size + 1);
    if (!text) {
        fclose(file);
        return false;
    }

    size_t read = fread(text, 1, (size_t)size, file);
    fclose(file);

    if (read != (size_t)size) {
        free(text);
        return false;
    }

    text[size] = '\0';
    *out_text = text;
    return true;
}

static void parse_int_field(const char *text, const char *field, int *out_value) {
    const char *cursor = strstr(text, field);
    if (cursor)
        sscanf(cursor + strlen(field), " : %d", out_value);
}

static void parse_int_field_between(const char *start, const char *end, const char *field, int *out_value) {
    const char *cursor = strstr(start, field);
    if (cursor && cursor < end)
        sscanf(cursor + strlen(field), " : %d", out_value);
}

static void parse_string_field(const char *text, const char *field, char *out, size_t out_size) {
    const char *cursor = strstr(text, field);
    if (!cursor)
        return;

    cursor = strchr(cursor, ':');
    if (!cursor)
        return;

    cursor = strchr(cursor, '"');
    if (!cursor)
        return;

    cursor++;

    size_t length = 0;
    while (*cursor && *cursor != '"' && length + 1 < out_size) {
        out[length++] = *cursor++;
    }

    out[length] = '\0';
}

static bool parse_next_json_string(const char **cursor, const char *end, char *out, size_t out_size) {
    const char *start = strchr(*cursor, '"');
    if (!start || start >= end)
        return false;

    start++;

    size_t length = 0;
    while (*start && *start != '"' && start < end && length + 1 < out_size) {
        if (*start == '\\' && start[1] != '\0')
            start++;

        out[length++] = *start++;
    }

    out[length] = '\0';

    const char *string_end = strchr(start, '"');
    if (!string_end || string_end >= end)
        return false;

    *cursor = string_end + 1;
    return true;
}

static void parse_completed_puzzles(const char *text, GameSave *save) {
    const char *objects = strstr(text, "\"completedPuzzles\"");
    const char *cursor = objects;
    if (cursor) {
        const char *array_start = strchr(cursor, '[');
        const char *array_end = array_start ? strchr(array_start, ']') : NULL;
        cursor = array_start;

        while (cursor &&
               array_end &&
               (cursor = strstr(cursor, "\"id\"")) != NULL &&
               cursor < array_end &&
               save->completed_puzzle_count < GAME_SAVE_MAX_COMPLETED_PUZZLES) {
            const char *next_completed_date = strstr(cursor, "\"completedDate\"");
            const char *object_end = strchr(cursor, '}');
            if (!next_completed_date || next_completed_date > array_end)
                break;

            if (!object_end || object_end > array_end)
                object_end = array_end;

            int index = save->completed_puzzle_count;
            parse_int_field(cursor, "\"id\"", &save->completed_puzzle_ids[index]);
            parse_string_field(
                cursor,
                "\"completedDate\"",
                save->completed_puzzle_dates[index],
                sizeof(save->completed_puzzle_dates[index])
            );
            parse_int_field_between(
                cursor,
                object_end,
                "\"bestTime\"",
                &save->completed_puzzle_best_times[index]
            );

            save->completed_puzzle_count++;
            cursor += strlen("\"id\"");
        }

        return;
    }

    cursor = strstr(text, "\"completedPuzzleIds\"");
    if (!cursor)
        return;

    cursor = strchr(cursor, '[');
    if (!cursor)
        return;

    cursor++;

    while (*cursor && *cursor != ']' && save->completed_puzzle_count < GAME_SAVE_MAX_COMPLETED_PUZZLES) {
        char *end = NULL;
        long value = strtol(cursor, &end, 10);

        if (end == cursor) {
            cursor++;
            continue;
        }

        save->completed_puzzle_ids[save->completed_puzzle_count] = (int)value;
        parse_string_field(
            text,
            "\"lastPlayedDate\"",
            save->completed_puzzle_dates[save->completed_puzzle_count],
            sizeof(save->completed_puzzle_dates[save->completed_puzzle_count])
        );
        save->completed_puzzle_count++;
        cursor = end;
    }
}

static void parse_custom_puzzles(const char *text, GameSave *save) {
    const char *array = strstr(text, "\"customPuzzles\"");
    if (!array)
        return;

    const char *cursor = strchr(array, '[');
    const char *array_end = cursor ? strstr(cursor, "],\n  \"currentStreak\"") : NULL;
    if (!cursor || !array_end)
        return;

    while ((cursor = strstr(cursor, "\"groups\"")) != NULL &&
           cursor < array_end &&
           save->custom_puzzle_count < GAME_SAVE_MAX_CUSTOM_PUZZLES) {
        const char *next_puzzle = strstr(cursor + strlen("\"groups\""), "\"groups\"");
        const char *puzzle_end = next_puzzle && next_puzzle < array_end ? next_puzzle : array_end;
        const char *puzzle_start = cursor;
        int custom_index = save->custom_puzzle_count;
        bool loaded = true;

        while (puzzle_start > array && *puzzle_start != '{')
            puzzle_start--;

        const char *name_field = strstr(puzzle_start, "\"name\"");
        if (name_field && name_field < cursor) {
            parse_string_field(
                name_field,
                "\"name\"",
                save->custom_puzzles[custom_index].name,
                sizeof(save->custom_puzzles[custom_index].name)
            );
        }

        for (int group = 0; group < GAME_SAVE_CUSTOM_GROUPS; group++) {
            const char *group_field = strstr(cursor, "\"group\"");
            const char *level_field = strstr(cursor, "\"level\"");
            const char *members_field = strstr(cursor, "\"members\"");
            if (!group_field || !members_field || group_field >= puzzle_end || members_field >= puzzle_end) {
                loaded = false;
                break;
            }

            if (level_field && level_field < members_field)
                parse_int_field(level_field, "\"level\"", &save->custom_puzzles[custom_index].group_levels[group]);
            else
                save->custom_puzzles[custom_index].group_levels[group] = 0;

            const char *name_cursor = strchr(group_field, ':');
            if (!name_cursor ||
                !parse_next_json_string(
                    &name_cursor,
                    puzzle_end,
                    save->custom_puzzles[custom_index].group_names[group],
                    GAME_SAVE_CUSTOM_TEXT_LENGTH
                )) {
                loaded = false;
                break;
            }

            const char *member_cursor = strchr(members_field, '[');
            if (!member_cursor) {
                loaded = false;
                break;
            }

            for (int member = 0; member < GAME_SAVE_CUSTOM_MEMBERS; member++) {
                if (!parse_next_json_string(
                        &member_cursor,
                        puzzle_end,
                        save->custom_puzzles[custom_index].words[group][member],
                        GAME_SAVE_CUSTOM_TEXT_LENGTH
                    )) {
                    loaded = false;
                    break;
                }
            }

            if (!loaded)
                break;

            cursor = members_field + strlen("\"members\"");
        }

        if (loaded)
            save->custom_puzzle_count++;

        cursor = puzzle_end;
    }
}

bool platform_save_load(GameSave *save) {
    char *text = NULL;

    platform_save_defaults(save);

    if (!save_mounted && !sd_save_fallback)
        return false;

    const char *path = platform_save_path();
    if (!read_text_file(path, &text))
        return false;

    parse_completed_puzzles(text, save);
    parse_custom_puzzles(text, save);
    parse_int_field(text, "\"currentStreak\"", &save->current_streak);
    parse_int_field(text, "\"bestStreak\"", &save->best_streak);
    parse_string_field(text, "\"lastPlayedDate\"", save->last_played_date, sizeof(save->last_played_date));

    free(text);
    return true;
}

static void write_json_string(FILE *file, const char *text) {
    fputc('"', file);

    for (int i = 0; text[i] != '\0'; i++) {
        if (text[i] == '"' || text[i] == '\\')
            fputc('\\', file);

        fputc(text[i], file);
    }

    fputc('"', file);
}

bool platform_save_write(const GameSave *save) {
    if (!save_mounted && !sd_save_fallback)
        return false;

    const char *path = platform_save_path();
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;

    fprintf(file, "{\n");
    fprintf(file, "  \"completedPuzzleIds\": [");
    for (int i = 0; i < save->completed_puzzle_count; i++) {
        if (i > 0)
            fprintf(file, ", ");
        fprintf(file, "%d", save->completed_puzzle_ids[i]);
    }
    fprintf(file, "],\n");
    fprintf(file, "  \"completedPuzzles\": [\n");
    for (int i = 0; i < save->completed_puzzle_count; i++) {
        fprintf(
            file,
            "    { \"id\": %d, \"completedDate\": \"%s\", \"bestTime\": %d }%s\n",
            save->completed_puzzle_ids[i],
            save->completed_puzzle_dates[i],
            save->completed_puzzle_best_times[i],
            i == save->completed_puzzle_count - 1 ? "" : ","
        );
    }
    fprintf(file, "  ],\n");
    fprintf(file, "  \"customPuzzles\": [\n");
    for (int puzzle_index = 0; puzzle_index < save->custom_puzzle_count; puzzle_index++) {
        const GameSaveCustomPuzzle *custom = &save->custom_puzzles[puzzle_index];

        fprintf(file, "    { \"name\": ");
        write_json_string(file, custom->name);
        fprintf(file, ", \"groups\": [\n");
        for (int group = 0; group < GAME_SAVE_CUSTOM_GROUPS; group++) {
            fprintf(file, "      { \"group\": ");
            write_json_string(file, custom->group_names[group]);
            fprintf(file, ", \"level\": %d, \"members\": [", custom->group_levels[group]);

            for (int member = 0; member < GAME_SAVE_CUSTOM_MEMBERS; member++) {
                if (member > 0)
                    fprintf(file, ", ");

                write_json_string(file, custom->words[group][member]);
            }

            fprintf(
                file,
                "] }%s\n",
                group == GAME_SAVE_CUSTOM_GROUPS - 1 ? "" : ","
            );
        }

        fprintf(
            file,
            "    ] }%s\n",
            puzzle_index == save->custom_puzzle_count - 1 ? "" : ","
        );
    }
    fprintf(file, "  ],\n");
    fprintf(file, "  \"currentStreak\": %d,\n", save->current_streak);
    fprintf(file, "  \"bestStreak\": %d,\n", save->best_streak);
    fprintf(file, "  \"lastPlayedDate\": \"%s\"\n", save->last_played_date);
    fprintf(file, "}\n");

    fclose(file);
    if (save_mounted)
        fsdevCommitDevice(SAVE_MOUNT_NAME);

    return true;
}

bool platform_save_add_custom_puzzle(GameSave *save, const GameSaveCustomPuzzle *custom_puzzle) {
    if (save->custom_puzzle_count >= GAME_SAVE_MAX_CUSTOM_PUZZLES)
        return false;

    save->custom_puzzles[save->custom_puzzle_count] = *custom_puzzle;
    save->custom_puzzle_count++;
    return true;
}

bool platform_save_has_completed_puzzle(const GameSave *save, int puzzle_id) {
    for (int i = 0; i < save->completed_puzzle_count; i++) {
        if (save->completed_puzzle_ids[i] == puzzle_id)
            return true;
    }

    return false;
}

static int completed_puzzle_index(const GameSave *save, int puzzle_id) {
    for (int i = 0; i < save->completed_puzzle_count; i++) {
        if (save->completed_puzzle_ids[i] == puzzle_id)
            return i;
    }

    return -1;
}

static void today_string(char *out, size_t out_size) {
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);

    if (!tm_now) {
        strncpy(out, "1970-01-01", out_size);
        out[out_size - 1] = '\0';
        return;
    }

    strftime(out, out_size, "%Y-%m-%d", tm_now);
}

static bool parse_date(const char *date, struct tm *out_tm) {
    int year = 0;
    int month = 0;
    int day = 0;

    if (sscanf(date, "%d-%d-%d", &year, &month, &day) != 3)
        return false;

    memset(out_tm, 0, sizeof(*out_tm));
    out_tm->tm_year = year - 1900;
    out_tm->tm_mon = month - 1;
    out_tm->tm_mday = day;
    out_tm->tm_isdst = -1;
    return true;
}

static int days_between(const char *from_date, const char *to_date) {
    struct tm from_tm;
    struct tm to_tm;

    if (!parse_date(from_date, &from_tm) || !parse_date(to_date, &to_tm))
        return 9999;

    time_t from_time = mktime(&from_tm);
    time_t to_time = mktime(&to_tm);

    if (from_time == (time_t)-1 || to_time == (time_t)-1)
        return 9999;

    return (int)((to_time - from_time) / (60 * 60 * 24));
}

bool platform_save_mark_completed_today(GameSave *save, int puzzle_id, int completed_time_seconds) {
    char today[GAME_SAVE_DATE_LENGTH];

    today_string(today, sizeof(today));

    int completed_index = completed_puzzle_index(save, puzzle_id);
    bool newly_completed = completed_index < 0;
    if (newly_completed && save->completed_puzzle_count < GAME_SAVE_MAX_COMPLETED_PUZZLES) {
        completed_index = save->completed_puzzle_count;
        save->completed_puzzle_ids[completed_index] = puzzle_id;
        strncpy(save->completed_puzzle_dates[completed_index], today, GAME_SAVE_DATE_LENGTH);
        save->completed_puzzle_dates[completed_index][GAME_SAVE_DATE_LENGTH - 1] = '\0';
        save->completed_puzzle_count++;
    }

    if (completed_index >= 0 &&
        completed_time_seconds > 0 &&
        (save->completed_puzzle_best_times[completed_index] <= 0 ||
         completed_time_seconds < save->completed_puzzle_best_times[completed_index])) {
        save->completed_puzzle_best_times[completed_index] = completed_time_seconds;
    }

    if (strcmp(save->last_played_date, today) != 0) {
        int gap = days_between(save->last_played_date, today);

        if (gap == 1)
            save->current_streak++;
        else
            save->current_streak = 1;

        if (save->current_streak > save->best_streak)
            save->best_streak = save->current_streak;

        strncpy(save->last_played_date, today, sizeof(save->last_played_date));
        save->last_played_date[sizeof(save->last_played_date) - 1] = '\0';
    } else if (save->current_streak <= 0) {
        save->current_streak = 1;
        if (save->best_streak < 1)
            save->best_streak = 1;
    }

    return newly_completed;
}
