// Compile with -Itests/stubs -Isource and link source/core/app_update.c.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "core/app_update.h"
#include "game/connections_puzzle.h"
#include "game/connections_update.h"

static bool test_valid_puzzles = true;
static bool test_changed_app;
static bool test_changed_puzzles;
static int test_puzzle_installs, test_app_installs, test_removes;
static ConnectionsDataInfo test_current = {1, 1, "2026-09-21"};
static ConnectionsDataInfo test_incoming = {2, 2, "2026-09-22"};

static bool test_validate(const char *path, ConnectionsDataInfo *info) {
    bool incoming = strstr(path, ".tmp") != NULL;
    if (incoming && !test_valid_puzzles) return false;
    if (info) *info = incoming ? test_incoming : test_current;
    return true;
}
static const char *test_data_path(void) { return CONNECTIONS_SD_PATH; }
static ConnectionsUpdateResult test_install(const char *a, const char *b, const char *c, const char *d) {
    (void)a; (void)b; (void)c; (void)d; test_puzzle_installs++; return CONNECTIONS_UPDATE_INSTALLED;
}
static bool test_read_app(const char *path, AppUpdateInfo *info) {
    memset(info, 0, sizeof(*info));
    info->file_size = 100000;
    strcpy(info->version, strstr(path, ".tmp") ? "1.2.0" : "1.1.0");
    if (strstr(path, ".tmp") && test_changed_app) info->build_id[0] = 1;
    return true;
}
static bool test_replace(const char *a, const char *b, const char *c) {
    (void)a; (void)b; (void)c; test_app_installs++; return true;
}
static int test_remove(const char *path) { (void)path; test_removes++; return 0; }
static int test_mkdir(const char *path, int mode) { (void)path; (void)mode; return 0; }
static FILE *test_fopen(const char *path, const char *mode) {
    (void)mode;
    bool incoming = strstr(path, ".tmp") != NULL;
    FILE *file = fopen(incoming ? "build/update-flow-incoming.tmp" : "build/update-flow-current.tmp", "w+b");
    assert(file);
    bool different = test_changed_puzzles || test_incoming.puzzle_count != test_current.puzzle_count;
    fputs(incoming && different ? "updated archive" : "current archive", file);
    rewind(file);
    return file;
}
static void test_cleanup_files(void) {
    remove("build/update-flow-incoming.tmp");
    remove("build/update-flow-current.tmp");
}

#define connections_puzzle_validate_file test_validate
#define connections_puzzle_data_path test_data_path
#define connections_update_install test_install
#define app_update_read_file test_read_app
#define app_update_replace test_replace
#define remove test_remove
#define mkdir test_mkdir
#define fopen test_fopen
#define fsync(fd) 0
#include "platform/platform_updates.c"

static void reset(void) {
    worker = NULL;
    test_nextload = true;
    test_valid_puzzles = true;
    test_changed_app = false;
    test_changed_puzzles = false;
    test_threads_started = test_puzzle_installs = test_app_installs = test_restarts = test_removes = 0;
    test_incoming = (ConnectionsDataInfo){2, 2, "2026-09-22"};
    char *args[] = {"sdmc:/switch/Connections/Connections.nro"};
    platform_updates_init(1, args);
}

static void complete(FetchResult outcome, const char *version) {
    if (version) {
        memset(&remote_info, 0, sizeof(remote_info));
        remote_info.file_size = 100000;
        strcpy(remote_info.version, version);
    }
    worker_result = outcome;
    SDL_AtomicSet(&finished, 1);
    assert(platform_updates_poll());
}

int main(void) {
    reset();
    platform_updates_start();
    assert(job == JOB_APP_CHECK && state == UPDATES_CHECKING_APP);
    complete(FETCH_OK, "1.2.0");
    assert(state == UPDATES_APP_AVAILABLE && !worker && test_threads_started == 1);
    assert(!test_app_installs && !test_puzzle_installs);
    platform_updates_skip();
    assert(job == JOB_PUZZLES && state == UPDATES_CHECKING_PUZZLES);
    complete(FETCH_OK, NULL);
    assert(state == UPDATES_PUZZLES_AVAILABLE && !test_puzzle_installs);
    platform_updates_accept();
    assert(state == UPDATES_FINISHED && test_puzzle_installs == 1);

    reset();
    platform_updates_start();
    complete(FETCH_OK, "1.1.0");
    assert(job == JOB_PUZZLES);
    test_incoming = test_current;
    complete(FETCH_OK, NULL);
    assert(state == UPDATES_FINISHED && !test_puzzle_installs && strstr(status, "up to date"));

    reset();
    platform_updates_start();
    complete(FETCH_OK, "1.1.0");
    test_incoming = test_current;
    test_changed_puzzles = true;
    complete(FETCH_OK, NULL);
    assert(state == UPDATES_PUZZLES_AVAILABLE && !test_puzzle_installs);

    reset();
    platform_updates_start();
    complete(FETCH_OK, "1.0.0");
    assert(job == JOB_PUZZLES); // Never offer a downgrade.
    complete(FETCH_OK, NULL);
    platform_updates_skip();
    assert(state == UPDATES_FINISHED && !test_puzzle_installs && test_removes > 0);

    reset();
    platform_updates_start();
    complete(FETCH_OFFLINE, NULL);
    assert(state == UPDATES_FINISHED && test_threads_started == 1 && !test_app_installs);

    reset();
    platform_updates_start();
    complete(FETCH_FAILED, NULL);
    assert(job == JOB_PUZZLES);
    test_incoming = test_current;
    complete(FETCH_OK, NULL);
    assert(strstr(status, "App check unavailable"));

    reset();
    platform_updates_start();
    complete(FETCH_OK, "1.2.0");
    platform_updates_skip();
    test_incoming = test_current;
    complete(FETCH_OK, NULL);
    assert(strstr(status, "postponed"));

    reset();
    platform_updates_start();
    complete(FETCH_OK, "1.1.0");
    test_valid_puzzles = false;
    complete(FETCH_OK, NULL);
    assert(state == UPDATES_FINISHED && !test_puzzle_installs);

    reset();
    platform_updates_start();
    complete(FETCH_OK, "1.2.0");
    platform_updates_accept();
    assert(job == JOB_APP_DOWNLOAD && !test_app_installs);
    complete(FETCH_OK, NULL);
    assert(state == UPDATES_RESTART_READY && test_app_installs == 1 && test_restarts == 1);
    assert(test_threads_started == 2); // No puzzle check before restart.

    reset();
    platform_updates_start();
    complete(FETCH_OK, "1.2.0");
    platform_updates_accept();
    test_changed_app = true;
    complete(FETCH_OK, NULL);
    assert(state == UPDATES_FINISHED && !test_app_installs && !test_restarts);

    reset();
    restart_supported = false;
    platform_updates_start();
    complete(FETCH_OK, "1.2.0");
    platform_updates_accept();
    assert(state == UPDATES_FINISHED && test_threads_started == 1 && !test_app_installs);
    platform_updates_skip();
    assert(job == JOB_PUZZLES && state == UPDATES_CHECKING_PUZZLES);

    reset();
    platform_updates_start();
    platform_updates_exit();
    assert(!worker && !test_app_installs && !test_puzzle_installs);

    reset();
    platform_updates_start();
    complete(FETCH_CANCELLED, NULL);
    assert(state == UPDATES_FINISHED && test_threads_started == 1);

    unsigned char buffer[8] = {0};
    Download output = {.memory = buffer, .limit = sizeof(buffer)};
    SDL_AtomicSet(&cancelled, 0);
    assert(receive_data("12345678", 1, 8, &output) == 8);
    assert(receive_data("9", 1, 1, &output) == 0);
    char range[] = "content-range: bytes 0-127/100000\r\n";
    receive_header(range, 1, strlen(range), &output);
    assert(output.has_range && output.first == 0 && output.last == 127 && output.total == 100000);
    char redirect[] = "HTTP/1.1 302 Found\r\n";
    receive_header(redirect, 1, strlen(redirect), &output);
    assert(!output.has_range);
    test_cleanup_files();
    puts("Update flow passed: app-first checks, consent, skip, offline, failures, restart, cancel, bounded responses.");
    return 0;
}
