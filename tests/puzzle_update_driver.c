#include "game/connections_puzzle.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Inject rename failures into the production installer to test rollback and
// the backup left behind when both promotion and rollback fail.
static int rename_count;
static int test_rename(const char *from, const char *to) {
    const char *fail = getenv("TEST_FAIL_RENAME");
    const char *fail_all = getenv("TEST_FAIL_RENAME_FROM");
    rename_count++;
    if ((fail && rename_count == atoi(fail)) || (fail_all && rename_count >= atoi(fail_all))) {
        errno = EACCES;
        return -1;
    }
    return rename(from, to);
}
#define rename test_rename
#include "game/connections_update.c"
#undef rename

int main(int argc, char **argv) {
    if (argc < 3)
        return 2;
    ConnectionsPuzzle puzzle;
    ConnectionsDataInfo info;
    if (strcmp(argv[1], "validate") == 0) {
        if (!connections_puzzle_validate_file(argv[2], &info))
            return 1;
        printf("%d %d %s\n", info.puzzle_count, info.playable_count, info.latest_date);
        return 0;
    }
    if (strcmp(argv[1], "by-id") == 0 && argc == 4) {
        if (!connections_puzzle_load_by_id(argv[2], atoi(argv[3]), &puzzle, false))
            return 1;
        printf("%d %s %s %s\n", puzzle.id, puzzle.date, puzzle.words[0], puzzle.group_names[0]);
        return 0;
    }
    if (strcmp(argv[1], "random") == 0) {
        if (!connections_puzzle_load_random(argv[2], &puzzle))
            return 1;
        printf("%d\n", puzzle.id);
        return 0;
    }
    if (strcmp(argv[1], "install") == 0 && argc == 6)
        return connections_update_install(argv[2], argv[3], argv[4], argv[5]);
    return 2;
}
