#include "connections_puzzle.h"
#include "libs/cjson/cJSON.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONNECTIONS_MAX_PUZZLES 20000

static char *read_file(const char *path, size_t *out_size) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    char *text = NULL;
    if (fseek(file, 0, SEEK_END) != 0)
        goto done;
    long size = ftell(file);
    if (size <= 0 || size > CONNECTIONS_MAX_FILE_SIZE || fseek(file, 0, SEEK_SET) != 0)
        goto done;
    text = malloc((size_t)size + 1);
    if (text && fread(text, 1, (size_t)size, file) != (size_t)size) {
        free(text);
        text = NULL;
    }
    if (text) {
        text[size] = '\0';
        *out_size = (size_t)size;
    }
done:
    fclose(file);
    return text;
}

// cJSON uses NUL-terminated strings. Reject embedded NULs so a field such as
// "id\u0000other" cannot masquerade as "id".
static bool safe_strings(const char *text, size_t size) {
    bool in_string = false;
    for (size_t i = 0; i < size; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (!ch)
            return false;
        if (ch == '"')
            in_string = !in_string;
        else if (in_string && ch < 0x20)
            return false;
        else if (in_string && ch == '\\') {
            if (++i >= size)
                return false;
            if (text[i] == 'u' && size - i >= 5 && memcmp(text + i, "u0000", 5) == 0)
                return false;
        } else if (!in_string && ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') {
            return false;
        }
    }
    return !in_string;
}

static const cJSON *field(const cJSON *object, const char *name) {
    const cJSON *found = NULL;
    const cJSON *item;
    if (!cJSON_IsObject(object))
        return NULL;
    cJSON_ArrayForEach(item, object) {
        if (strcmp(item->string, name) == 0) {
            if (found)
                return NULL;
            found = item;
        }
    }
    return found;
}

static bool integer(const cJSON *item, int minimum, int maximum, int *out) {
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= minimum && item->valuedouble <= maximum))
        return false;
    int value = (int)item->valuedouble;
    if (item->valuedouble != value)
        return false;
    *out = value;
    return true;
}

static bool valid_date(const char *date) {
    if (strlen(date) != 10 || date[4] != '-' || date[7] != '-')
        return false;
    for (int i = 0; i < 10; i++) {
        if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9'))
            return false;
    }
    int year, month, day;
    if (sscanf(date, "%d-%d-%d", &year, &month, &day) != 3 || year < 1 || month < 1 || month > 12)
        return false;
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int limit = days[month - 1];
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
        limit++;
    return day >= 1 && day <= limit;
}

static void copy_label(char *out, size_t capacity, const char *text) {
    size_t length = strlen(text);
    if (length >= capacity) {
        length = capacity - 1;
        while (length > 0 && ((unsigned char)text[length] & 0xC0) == 0x80)
            length--;
    }
    memcpy(out, text, length);
    out[length] = '\0';
}

static bool parse_puzzle(const cJSON *record, ConnectionsPuzzle *puzzle, bool *playable) {
    memset(puzzle, 0, sizeof(*puzzle));
    *playable = true;
    const cJSON *date = field(record, "date");
    const cJSON *answers = field(record, "answers");
    if (!integer(field(record, "id"), 1, INT_MAX, &puzzle->id) ||
        !cJSON_IsString(date) || !valid_date(date->valuestring) ||
        !cJSON_IsArray(answers) || cJSON_GetArraySize(answers) != CONNECTIONS_GROUP_COUNT)
        return false;
    strcpy(puzzle->date, date->valuestring);
    unsigned levels = 0;
    for (int group = 0; group < CONNECTIONS_GROUP_COUNT; group++) {
        const cJSON *answer = cJSON_GetArrayItem(answers, group);
        const cJSON *name = field(answer, "group");
        const cJSON *members = field(answer, "members");
        if (!integer(field(answer, "level"), -1, 3, &puzzle->group_levels[group]) ||
            !cJSON_IsString(name) || !name->valuestring[0] ||
            !cJSON_IsArray(members) || cJSON_GetArraySize(members) != CONNECTIONS_GROUP_SIZE)
            return false;
        int level = puzzle->group_levels[group];
        // -1 is the upstream's existing "unknown difficulty" convention.
        if (level >= 0) {
            if (levels & (1u << level))
                return false;
            levels |= 1u << level;
        }
        copy_label(puzzle->group_names[group], CONNECTIONS_GROUP_NAME_MAX_LENGTH, name->valuestring);
        for (int member = 0; member < CONNECTIONS_GROUP_SIZE; member++) {
            const cJSON *word = cJSON_GetArrayItem(members, member);
            if (!cJSON_IsString(word) || strlen(word->valuestring) >= CONNECTIONS_WORD_MAX_LENGTH)
                return false;
            // The archive includes a symbol-only April Fools puzzle with empty
            // strings. Keep the archive compatible, but never offer that puzzle.
            if (!word->valuestring[0])
                *playable = false;
            for (int i = 0; i < puzzle->word_count; i++) {
                if (strcmp(puzzle->words[i], word->valuestring) == 0)
                    *playable = false;
            }
            strcpy(puzzle->answers[group][member], word->valuestring);
            strcpy(puzzle->words[puzzle->word_count], word->valuestring);
            puzzle->group_ids[puzzle->word_count++] = group;
        }
    }
    return true;
}

static int compare_ids(const void *a, const void *b) {
    int left = *(const int *)a, right = *(const int *)b;
    return (left > right) - (left < right);
}

static cJSON *load_database(const char *path, ConnectionsDataInfo *info) {
    size_t size = 0;
    char *text = read_file(path, &size);
    if (!text)
        return NULL;
    const char *end = NULL;
    cJSON *root = safe_strings(text, size) ? cJSON_ParseWithLengthOpts(text, size + 1, &end, 1) : NULL;
    bool complete = root && end == text + size;
    free(text);
    int count = cJSON_IsArray(root) ? cJSON_GetArraySize(root) : 0;
    if (!complete || count < 1 || count > CONNECTIONS_MAX_PUZZLES) {
        cJSON_Delete(root);
        return NULL;
    }
    int *ids = malloc((size_t)count * sizeof(*ids));
    if (!ids) {
        cJSON_Delete(root);
        return NULL;
    }
    memset(info, 0, sizeof(*info));
    const cJSON *item;
    cJSON_ArrayForEach(item, root) {
        ConnectionsPuzzle puzzle;
        bool playable;
        if (!parse_puzzle(item, &puzzle, &playable))
            goto invalid;
        ids[info->puzzle_count++] = puzzle.id;
        if (playable) {
            info->playable_count++;
            if (strcmp(puzzle.date, info->latest_date) > 0)
                strcpy(info->latest_date, puzzle.date);
        }
    }
    qsort(ids, (size_t)count, sizeof(*ids), compare_ids);
    for (int i = 1; i < count; i++) {
        if (ids[i] == ids[i - 1])
            goto invalid;
    }
    if (!info->playable_count)
        goto invalid;
    free(ids);
    return root;
invalid:
    free(ids);
    cJSON_Delete(root);
    return NULL;
}

bool connections_puzzle_validate_file(const char *path, ConnectionsDataInfo *out_info) {
    ConnectionsDataInfo info;
    cJSON *root = load_database(path, &info);
    if (!root)
        return false;
    cJSON_Delete(root);
    if (out_info)
        *out_info = info;
    return true;
}

static void seed_random(void) {
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = true;
    }
}

static void shuffle_words(ConnectionsPuzzle *puzzle) {
    seed_random();
    for (int i = puzzle->word_count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        if (i == j) continue;
        char temp[CONNECTIONS_WORD_MAX_LENGTH];
        int temp_group_id = puzzle->group_ids[i];
        strcpy(temp, puzzle->words[i]);
        strcpy(puzzle->words[i], puzzle->words[j]);
        strcpy(puzzle->words[j], temp);
        puzzle->group_ids[i] = puzzle->group_ids[j];
        puzzle->group_ids[j] = temp_group_id;
    }
}

bool connections_puzzle_prepare_race(const char *path, ConnectionsPuzzle **out_puzzles, int *out_count, int *out_entries) {
    *out_entries = 0;
    *out_puzzles = NULL;
    *out_count = 0;
    ConnectionsDataInfo info;
    cJSON *root = load_database(path, &info);
    if (!root) return false;
    ConnectionsPuzzle *puzzles = malloc((size_t)info.playable_count * sizeof(*puzzles));
    if (!puzzles) { cJSON_Delete(root); return false; }
    const cJSON *item;
    int count = 0;
    cJSON_ArrayForEach(item, root) {
        ConnectionsPuzzle parsed;
        bool playable;
        if (parse_puzzle(item, &parsed, &playable) && playable) {
            puzzles[count++] = parsed;
        }
    }
    cJSON_Delete(root);
    *out_entries = info.puzzle_count;
    *out_puzzles = puzzles;
    *out_count = count;
    return count > 0;
}

bool connections_puzzle_load_random(const char *path, ConnectionsPuzzle *out_puzzle) {
    if (!out_puzzle)
        return false;
    ConnectionsDataInfo info;
    cJSON *root = load_database(path, &info);
    if (!root)
        return false;
    seed_random();
    int index = rand() % info.playable_count;
    const cJSON *item;
    bool loaded = false;
    cJSON_ArrayForEach(item, root) {
        bool playable;
        parse_puzzle(item, out_puzzle, &playable);
        if (playable && index-- == 0) {
            shuffle_words(out_puzzle);
            loaded = true;
            break;
        }
    }
    cJSON_Delete(root);
    return loaded;
}

bool connections_puzzle_load_by_id(const char *path, int puzzle_id, ConnectionsPuzzle *out_puzzle, bool shuffle) {
    if (!out_puzzle)
        return false;
    ConnectionsDataInfo info;
    cJSON *root = load_database(path, &info);
    if (!root)
        return false;
    const cJSON *item;
    bool loaded = false;
    cJSON_ArrayForEach(item, root) {
        if (field(item, "id")->valueint != puzzle_id)
            continue;
        bool playable;
        loaded = parse_puzzle(item, out_puzzle, &playable) && playable;
        if (loaded && shuffle)
            shuffle_words(out_puzzle);
        break;
    }
    cJSON_Delete(root);
    return loaded;
}

bool connections_puzzle_latest_date(const char *path, char *out_date, size_t out_size) {
    if (!out_date || !out_size)
        return false;
    out_date[0] = '\0';
    ConnectionsDataInfo info;
    if (!connections_puzzle_validate_file(path, &info))
        return false;
    snprintf(out_date, out_size, "%s", info.latest_date);
    return true;
}

const char *connections_puzzle_data_path(void) {
    if (connections_puzzle_validate_file(CONNECTIONS_SD_PATH, NULL))
        return CONNECTIONS_SD_PATH;
    if (connections_puzzle_validate_file(CONNECTIONS_BACKUP_PATH, NULL))
        return CONNECTIONS_BACKUP_PATH;
    return CONNECTIONS_ROMFS_PATH;
}
