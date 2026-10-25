#include "core/app_update.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rename_count;
static int test_rename(const char *from, const char *to) {
    const char *fail = getenv("TEST_FAIL_RENAME");
    const char *fail_from = getenv("TEST_FAIL_RENAME_FROM");
    rename_count++;
    if ((fail && rename_count == atoi(fail)) || (fail_from && rename_count >= atoi(fail_from))) {
        errno = EACCES;
        return -1;
    }
    return rename(from, to);
}
#define rename test_rename
#include "core/app_update.c"
#undef rename

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    if (strcmp(argv[1], "version") == 0 && argc == 4) {
        int order;
        if (!app_update_version_compare(argv[2], argv[3], &order)) return 1;
        printf("%d\n", order);
        return 0;
    }
    if (strcmp(argv[1], "file") == 0) {
        AppUpdateInfo info;
        if (!app_update_read_file(argv[2], &info)) return 1;
        printf("%s\n", info.version);
        return 0;
    }
    if (strcmp(argv[1], "path") == 0) {
        char path[APP_UPDATE_PATH_SIZE];
        if (!app_update_launch_path(argv[2], path, sizeof(path))) return 1;
        printf("%s\n", path);
        return 0;
    }
    if (strcmp(argv[1], "replace") == 0 && argc == 5)
        return app_update_replace(argv[2], argv[3], argv[4]) ? 0 : 1;
    return 2;
}
