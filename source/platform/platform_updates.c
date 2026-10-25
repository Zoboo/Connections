#include "platform_updates.h"
#include "multiplayer/multiplayer.h"
#include "core/app_update.h"
#include "core/version.h"
#include "core/nsp_update.h"
#include "platform_nsp_install.h"
#include "game/connections_puzzle.h"
#include "game/connections_update.h"
#include <switch.h>
#include <SDL2/SDL.h>
#include <curl/curl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define APP_URL "https://github.com/Zoboo/Connections/releases/latest/download/Connections.nro"
#define NSP_URL "https://github.com/Zoboo/Connections/releases/latest/download/Connections.nsp"
#define PUZZLE_URL "https://raw.githubusercontent.com/Eyefyre/NYT-Connections-Answers/main/connections.json"
#define NSP_TEMP "sdmc:/switch/Connections/connections-update.nsp.tmp"

typedef enum { JOB_NSP_CHECK, JOB_APP_CHECK, JOB_APP_DOWNLOAD, JOB_NSP_DOWNLOAD, JOB_PUZZLES } UpdateJob;
typedef enum { FETCH_OK, FETCH_OFFLINE, FETCH_FAILED, FETCH_STORAGE, FETCH_CANCELLED } FetchResult;
typedef struct {
    FILE *file;
    unsigned char *memory;
    size_t bytes, limit;
    bool write_failed, has_range;
    unsigned long long first, last, total;
} Download;

static PlatformUpdateState state;
static SDL_Thread *worker;
static SDL_atomic_t finished, cancelled;
static SDL_atomic_t progress_percent;
static UpdateJob job;
static FetchResult worker_result;
static char nsp_version[32];
static uint32_t nsp_native_version;
static bool nsp_offered, nsp_check_failed;
static AppUpdateInfo remote_info;
static AppUpdateInfo offered_info;
static ConnectionsDataInfo puzzle_info;
static char status[128];
static char nro_path[APP_UPDATE_PATH_SIZE];
static char nro_temp[APP_UPDATE_PATH_SIZE];
static char nro_backup[APP_UPDATE_PATH_SIZE];
static bool restart_supported, app_check_failed, app_update_skipped, puzzles_pending;
static bool launch_check;
static unsigned revision;
static PlatformUpdateState deferred_state;
static char deferred_status[128];
static bool native_restart_pending;
static char native_install_error[128];

static void finish(const char *message) {
    snprintf(status, sizeof(status), "%s", message);
    state = UPDATES_FINISHED;
}

static size_t receive_data(char *data, size_t size, size_t count, void *userdata) {
    Download *output = userdata;
    if (SDL_AtomicGet(&cancelled) || (size && count > SIZE_MAX / size))
        return 0;
    size_t bytes = size * count;
    if (bytes > output->limit - output->bytes)
        return 0;
    if (output->file) {
        if (fwrite(data, 1, bytes, output->file) != bytes) {
            output->write_failed = true;
            return 0;
        }
    } else {
        memcpy(output->memory + output->bytes, data, bytes);
    }
    output->bytes += bytes;
    return bytes;
}

static size_t receive_header(char *data, size_t size, size_t count, void *userdata) {
    if (size && count > SIZE_MAX / size)
        return 0;
    size_t bytes = size * count;
    Download *output = userdata;
    if (bytes >= 5 && strncmp(data, "HTTP/", 5) == 0)
        output->has_range = false;
    if (bytes < 160) {
        char line[160];
        memcpy(line, data, bytes);
        line[bytes] = 0;
        // HTTP header names are case insensitive.
        if (SDL_strncasecmp(line, "Content-Range:", 14) == 0)
            output->has_range = sscanf(line + 14, " bytes %llu-%llu/%llu",
                &output->first, &output->last, &output->total) == 3;
    }
    return bytes;
}

static int progress(void *unused, curl_off_t total, curl_off_t now, curl_off_t up_total, curl_off_t up_now) {
    (void)unused; (void)up_total; (void)up_now;
    if (total > 0 && now >= 0) {
        int maximum = job == JOB_NSP_DOWNLOAD ? 80 : 100;
        int percent = now >= total ? maximum : (int)((double)now / (double)total * maximum);
        SDL_AtomicSet(&progress_percent, percent);
    }
    return SDL_AtomicGet(&cancelled) != 0;
}

static FetchResult fetch(CURL *curl, const char *url, Download *output, const char *range) {
    curl_easy_reset(curl);
#define OPTION(key, value) do { if (curl_easy_setopt(curl, key, value) != CURLE_OK) return FETCH_FAILED; } while (0)
    OPTION(CURLOPT_URL, url);
    OPTION(CURLOPT_USERAGENT, "Connections-Switch/" CONNECTIONS_APP_VERSION);
    OPTION(CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
    OPTION(CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
    OPTION(CURLOPT_FOLLOWLOCATION, 1L);
    OPTION(CURLOPT_MAXREDIRS, 5L);
    OPTION(CURLOPT_FAILONERROR, 1L);
    OPTION(CURLOPT_SSL_VERIFYPEER, 1L);
    OPTION(CURLOPT_SSL_VERIFYHOST, 2L);
    OPTION(CURLOPT_CONNECTTIMEOUT, 10L);
    OPTION(CURLOPT_TIMEOUT, range ? 20L : 120L);
    OPTION(CURLOPT_LOW_SPEED_LIMIT, 1024L);
    OPTION(CURLOPT_LOW_SPEED_TIME, 15L);
    OPTION(CURLOPT_NOSIGNAL, 1L);
    OPTION(CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)output->limit);
    OPTION(CURLOPT_WRITEFUNCTION, receive_data);
    OPTION(CURLOPT_WRITEDATA, output);
    OPTION(CURLOPT_HEADERFUNCTION, receive_header);
    OPTION(CURLOPT_HEADERDATA, output);
    OPTION(CURLOPT_NOPROGRESS, 0L);
    OPTION(CURLOPT_XFERINFOFUNCTION, progress);
    OPTION(CURLOPT_ACCEPT_ENCODING, "identity");
    if (range)
        OPTION(CURLOPT_RANGE, range);
#undef OPTION
    CURLcode result = curl_easy_perform(curl);
    long response = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
    if (SDL_AtomicGet(&cancelled))
        return FETCH_CANCELLED;
    if (output->write_failed)
        return FETCH_STORAGE;
    return result == CURLE_OK && response == (range ? 206 : 200) && output->bytes ? FETCH_OK : FETCH_FAILED;
}

static bool read_range(CURL *curl, const char *url, uint64_t maximum, uint64_t offset, size_t size, unsigned char *buffer, uint64_t *total) {
    char range[64];
    snprintf(range, sizeof(range), "%llu-%llu", (unsigned long long)offset, (unsigned long long)(offset + size - 1));
    Download output = {.memory = buffer, .limit = size};
    if (fetch(curl, url, &output, range) != FETCH_OK || output.bytes != size || !output.has_range ||
        output.first != offset || output.last != offset + size - 1 || output.total > maximum ||
        output.total <= output.last || (*total && *total != output.total))
        return false;
    *total = output.total;
    return true;
}

static bool check_remote_nro(CURL *curl) {
    unsigned char header[APP_UPDATE_HEADER_SIZE], assets[APP_UPDATE_ASSET_SIZE], nacp[APP_UPDATE_NACP_SIZE];
    uint64_t total = 0;
    return read_range(curl, APP_URL, APP_UPDATE_MAX_SIZE, 0, sizeof(header), header, &total) &&
        app_update_parse_header(header, total, &remote_info) &&
        read_range(curl, APP_URL, APP_UPDATE_MAX_SIZE, remote_info.asset_offset, sizeof(assets), assets, &total) &&
        app_update_parse_assets(assets, &remote_info) &&
        read_range(curl, APP_URL, APP_UPDATE_MAX_SIZE, remote_info.nacp_offset, sizeof(nacp), nacp, &total) &&
        app_update_parse_nacp(nacp, &remote_info);
}

static bool check_remote_nsp(CURL *curl) {
    unsigned char index[NSP_UPDATE_INDEX_LIMIT], manifest[NSP_UPDATE_MANIFEST_LIMIT];
    uint64_t total = 0, offset = 0;
    size_t size = 0;
    return read_range(curl, NSP_URL, NSP_UPDATE_MAX_SIZE, 0, sizeof(index), index, &total) &&
        nsp_update_index(index, sizeof(index), total, &offset, &size) &&
        read_range(curl, NSP_URL, NSP_UPDATE_MAX_SIZE, offset, size, manifest, &total) &&
        nsp_update_manifest(manifest, size, nsp_version, sizeof(nsp_version), &nsp_native_version);
}

static bool ensure_directory(const char *path) {
    if (mkdir(path, 0777) == 0)
        return true;
    struct stat info;
    return errno == EEXIST && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static FetchResult download_file(CURL *curl, const char *url, const char *path, size_t limit) {
    Download output = {.limit = limit};
    output.file = fopen(path, "wb");
    if (!output.file)
        return FETCH_STORAGE;
    FetchResult result = fetch(curl, url, &output, NULL);
    bool saved = fflush(output.file) == 0;
    if (saved && fsync(fileno(output.file)) != 0)
        saved = false;
    if (fclose(output.file) != 0)
        saved = false;
    if (!saved)
        result = FETCH_STORAGE;
    if (result != FETCH_OK)
        remove(path);
    return result;
}

static int network_worker(void *unused) {
    (void)unused;
    FetchResult outcome = FETCH_FAILED;
    bool sockets_ready = false, curl_ready = false;
    CURL *curl = NULL;
    Result rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) {
        outcome = FETCH_OFFLINE;
        goto done;
    }
    NifmInternetConnectionType type;
    NifmInternetConnectionStatus connection;
    u32 strength;
    rc = nifmGetInternetConnectionStatus(&type, &strength, &connection);
    nifmExit();
    if (R_FAILED(rc) || connection != NifmInternetConnectionStatus_Connected) {
        outcome = FETCH_OFFLINE;
        goto done;
    }
    if (SDL_AtomicGet(&cancelled)) {
        outcome = FETCH_CANCELLED;
        goto done;
    }
    if (!mp_socket_acquire())
        goto done;
    sockets_ready = true;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        goto done;
    curl_ready = true;
    curl = curl_easy_init();
    if (!curl)
        goto done;
    if (job == JOB_NSP_CHECK) {
        outcome = check_remote_nsp(curl) ? FETCH_OK : FETCH_FAILED;
    } else if (job == JOB_APP_CHECK) {
        outcome = check_remote_nro(curl) ? FETCH_OK : FETCH_FAILED;
    } else if (job == JOB_APP_DOWNLOAD) {
        outcome = download_file(curl, APP_URL, nro_temp, APP_UPDATE_MAX_SIZE);
    } else if (job == JOB_NSP_DOWNLOAD) {
        if (!ensure_directory("sdmc:/switch") || !ensure_directory("sdmc:/switch/Connections")) {
            outcome = FETCH_STORAGE;
        } else {
            outcome = download_file(curl, NSP_URL, NSP_TEMP, NSP_UPDATE_MAX_SIZE);
            if (outcome == FETCH_OK) {
                native_restart_pending = platform_nsp_install(NSP_TEMP,nsp_native_version,&cancelled,&progress_percent,
                                                              native_install_error,sizeof(native_install_error));
                if (!native_restart_pending) outcome = FETCH_FAILED;
            }
            remove(NSP_TEMP);
        }
    } else if (!ensure_directory("sdmc:/switch") || !ensure_directory(CONNECTIONS_SD_DIR)) {
        outcome = FETCH_STORAGE;
    } else {
        outcome = download_file(curl, PUZZLE_URL, CONNECTIONS_TEMP_PATH, CONNECTIONS_MAX_FILE_SIZE);
    }
done:
    if (curl)
        curl_easy_cleanup(curl);
    if (curl_ready)
        curl_global_cleanup();
    if (sockets_ready)
        mp_socket_release();
    worker_result = !native_restart_pending && SDL_AtomicGet(&cancelled) ? FETCH_CANCELLED : outcome;
    SDL_AtomicSet(&finished, 1);
    return 0;
}

static void begin(UpdateJob next) {
    job = next;
    SDL_AtomicSet(&cancelled, 0);
    SDL_AtomicSet(&finished, 0);
    SDL_AtomicSet(&progress_percent, -1);
    state = (next == JOB_APP_CHECK || next == JOB_NSP_CHECK) ? UPDATES_CHECKING_APP :
            (next == JOB_APP_DOWNLOAD || next == JOB_NSP_DOWNLOAD) ? UPDATES_DOWNLOADING_APP : UPDATES_CHECKING_PUZZLES;
    snprintf(status, sizeof(status), "%s", next == JOB_NSP_CHECK ? "Checking NSP version..." : next == JOB_APP_CHECK ? "Checking app version..." :
        (next == JOB_APP_DOWNLOAD || next == JOB_NSP_DOWNLOAD) ? "Downloading app update..." : "Checking for new puzzles...");
    worker = SDL_CreateThreadWithStackSize(network_worker, "Updates", 256 * 1024, NULL);
    if (!worker)
        finish("Could not start update check. Try again.");
}

void platform_updates_init(int argc, char **argv) {
    AppUpdateInfo installed;
    int order;
    restart_supported = argc > 0 && app_update_launch_path(argv[0], nro_path, sizeof(nro_path)) &&
        envHasNextLoad() && app_update_read_file(nro_path, &installed) &&
        app_update_version_compare(installed.version, CONNECTIONS_APP_VERSION, &order) && order == 0;
    if (nro_path[0]) {
        // app_update_launch_path reserves room for both suffixes.
        strcpy(nro_temp, nro_path);
        strcat(nro_temp, ".update.tmp");
        strcpy(nro_backup, nro_path);
        strcat(nro_backup, ".bak");
    }
    state = UPDATES_IDLE;
}

void platform_updates_start(void) {
    if (worker || state == UPDATES_APP_AVAILABLE || state == UPDATES_PUZZLES_AVAILABLE)
        return;
    app_check_failed = false;
    app_update_skipped = false;
    puzzles_pending = false;
    nsp_offered = false;
    nsp_check_failed = false;
    launch_check = false;
    begin(JOB_NSP_CHECK);
}

void platform_updates_start_launch(void) {
    platform_updates_start();
    launch_check = true;
}

static void dismiss_nsp_offer(void) {
    nsp_offered = false;
    if (launch_check) {
        state = deferred_state;
        snprintf(status, sizeof(status), "%s", deferred_status);
        ++revision;
    } else {
        begin(JOB_APP_CHECK);
    }
}

static void finish_app_download(void) {
    AppUpdateInfo incoming, current;
    int order;
    if (!app_update_read_file(nro_temp, &incoming) || incoming.file_size != offered_info.file_size ||
        memcmp(incoming.build_id, offered_info.build_id, sizeof(incoming.build_id)) != 0 ||
        strcmp(incoming.version, offered_info.version) != 0 ||
        !app_update_version_compare(incoming.version, CONNECTIONS_APP_VERSION, &order) || order <= 0 ||
        !app_update_read_file(nro_path, &current) || strcmp(current.version, CONNECTIONS_APP_VERSION) != 0) {
        remove(nro_temp);
        finish("Invalid app download. Existing app kept.");
        return;
    }

    // ROMFS keeps the running NRO open. Close it before renaming its SD file.
    romfsExit();
    if (!app_update_replace(nro_temp, nro_path, nro_backup)) {
        remove(nro_temp);
        if (R_FAILED(romfsInit()))
            romfsMountFromFsdev(nro_backup, current.romfs_offset, "romfs");
        finish("Install failed. Previous app kept in place or .bak.");
        return;
    }
    char arguments[APP_UPDATE_PATH_SIZE + 4];
    snprintf(arguments, sizeof(arguments), "\"%s\"", nro_path);
    if (R_SUCCEEDED(envSetNextLoad(nro_path, arguments))) {
        state = UPDATES_RESTART_READY;
        snprintf(status, sizeof(status), "Restarting updated app...");
    } else {
        // Installation is valid even if a loader unexpectedly rejects restart.
        romfsInit();
        finish("App installed. Quit and reopen it from hbmenu.");
    }
}

static bool files_equal(const char *left, const char *right) {
    FILE *a = fopen(left, "rb"), *b = fopen(right, "rb");
    bool equal = false;
    if (a && b) {
        unsigned char first[4096], second[4096];
        for (;;) {
            size_t na = fread(first, 1, sizeof(first), a);
            size_t nb = fread(second, 1, sizeof(second), b);
            if (na != nb || memcmp(first, second, na) != 0 || ferror(a) || ferror(b))
                break;
            if (na < sizeof(first)) {
                equal = true;
                break;
            }
        }
    }
    if (a) fclose(a);
    if (b) fclose(b);
    return equal;
}

bool platform_updates_poll(void) {
    if (!worker || !SDL_AtomicGet(&finished))
        return false;
    SDL_WaitThread(worker, NULL);
    worker = NULL;
    if (job == JOB_NSP_DOWNLOAD) {
        nsp_offered = false;
        if (native_restart_pending) state = UPDATES_RESTART_READY;
        else {
            puzzles_pending = launch_check && deferred_state == UPDATES_PUZZLES_AVAILABLE;
            finish(native_install_error[0] ? native_install_error : worker_result == FETCH_CANCELLED ?
                   "Update cancelled." : "Version update failed.");
        }
    } else if (worker_result == FETCH_CANCELLED) {
        if (job == JOB_APP_DOWNLOAD) remove(nro_temp);
        if (job == JOB_PUZZLES) remove(CONNECTIONS_TEMP_PATH);
        finish("Update check cancelled. Existing files kept.");
    } else if (worker_result == FETCH_OFFLINE && job != JOB_NSP_CHECK) {
        finish("No internet connection. Existing files kept.");
    } else if (job == JOB_NSP_CHECK) {
        int order = 0;
        nsp_check_failed = worker_result != FETCH_OK ||
            !app_update_version_compare(nsp_version, CONNECTIONS_APP_VERSION, &order);
        unsigned major = 0, minor = 0, patch = 0;
        sscanf(CONNECTIONS_APP_VERSION, "%u.%u.%u", &major, &minor, &patch);
        uint32_t current_native = major << 24 | minor << 16 | patch << 8 | CONNECTIONS_PACKAGE_REVISION;
        if (!nsp_check_failed && (order > 0 || (order == 0 && nsp_native_version > current_native))) {
            nsp_offered = true;
            if (!launch_check) {
                state = UPDATES_APP_AVAILABLE;
                snprintf(status, sizeof(status), "NSP version %s is available.", nsp_version);
            }
            else begin(JOB_PUZZLES);
        } else {
            begin(launch_check ? JOB_PUZZLES : JOB_APP_CHECK);
        }
    } else if (job == JOB_APP_CHECK) {
        int order = 0;
        app_check_failed = worker_result != FETCH_OK ||
            !app_update_version_compare(remote_info.version, CONNECTIONS_APP_VERSION, &order);
        if (!app_check_failed && order > 0) {
            offered_info = remote_info;
            state = UPDATES_APP_AVAILABLE;
            snprintf(status, sizeof(status), "App version %s is available.", offered_info.version);
        } else {
            begin(JOB_PUZZLES);
        }
    } else if (worker_result != FETCH_OK) {
        puzzles_pending = job == JOB_APP_DOWNLOAD;
        finish(worker_result == FETCH_STORAGE ? "Could not save download. Existing files kept." :
                                               "Update download failed. Existing files kept.");
    } else if (job == JOB_APP_DOWNLOAD) {
        finish_app_download();
        puzzles_pending = state != UPDATES_RESTART_READY;
    } else {
        // All cJSON parsing and file promotion run on the UI thread. Background
        // checks can safely continue while a game or custom screen is open.
        ConnectionsDataInfo current;
        if (!connections_puzzle_validate_file(CONNECTIONS_TEMP_PATH, &puzzle_info)) {
            remove(CONNECTIONS_TEMP_PATH);
            finish("Unsupported puzzle file. Existing puzzles kept.");
        } else {
            const char *current_path = connections_puzzle_data_path();
            bool have_current = connections_puzzle_validate_file(current_path, &current);
            bool older = have_current && (puzzle_info.puzzle_count < current.puzzle_count ||
                puzzle_info.playable_count < current.playable_count || strcmp(puzzle_info.latest_date, current.latest_date) < 0);
            // Also offer upstream corrections to existing puzzles. A matching
            // date/count alone does not mean the archive is unchanged.
            bool newer = !have_current || !files_equal(CONNECTIONS_TEMP_PATH, current_path);
            if (!older && newer) {
                state = UPDATES_PUZZLES_AVAILABLE;
                snprintf(status, sizeof(status), "Puzzle update through %s.", puzzle_info.latest_date);
            } else {
                remove(CONNECTIONS_TEMP_PATH);
                finish(older ? "Older puzzle archive ignored." : app_check_failed ?
                    "Puzzles are current. App check unavailable." : app_update_skipped ?
                    "Puzzles current. App update postponed." : nsp_check_failed ? "Puzzles current. NSP check unavailable." : "App and puzzles are up to date.");
            }
        }
    }
    if (launch_check && nsp_offered && !worker) {
        deferred_state = state;
        snprintf(deferred_status, sizeof(deferred_status), "%s", status);
        state = UPDATES_APP_AVAILABLE;
        snprintf(status, sizeof(status), "NSP version %s is available.", nsp_version);
    }
    ++revision;
    return true;
}

void platform_updates_accept(void) {
    if (state == UPDATES_APP_AVAILABLE) {
        if (nsp_offered) {
            if (!envIsNso()) {
                nsp_offered = false;
                finish("Native update requires launch from HOME.");
                ++revision;
                return;
            }
            native_install_error[0] = 0;
            begin(JOB_NSP_DOWNLOAD);
            return;
        }
        if (!restart_supported) {
            puzzles_pending = true;
            app_update_skipped = true;
            finish("Launch this NRO from hbmenu to install updates.");
            return;
        }
        begin(JOB_APP_DOWNLOAD);
    } else if (state == UPDATES_PUZZLES_AVAILABLE) {
        ConnectionsUpdateResult result = connections_update_install(CONNECTIONS_TEMP_PATH,
            CONNECTIONS_SD_PATH, CONNECTIONS_BACKUP_PATH, CONNECTIONS_ROMFS_PATH);
        remove(CONNECTIONS_TEMP_PATH);
        finish(result == CONNECTIONS_UPDATE_INSTALLED ? "Puzzles updated successfully." :
            result == CONNECTIONS_UPDATE_INVALID ? "Unsupported puzzle file. Existing puzzles kept." :
            result == CONNECTIONS_UPDATE_OLDER ? "Older puzzle archive ignored." : "Could not install puzzles. Previous data kept.");
    }
}

void platform_updates_skip(void) {
    if (state == UPDATES_FINISHED && puzzles_pending) {
        puzzles_pending = false;
        app_update_skipped = true;
        begin(JOB_PUZZLES);
    } else if (state == UPDATES_APP_AVAILABLE && nsp_offered) {
        dismiss_nsp_offer();
    } else if (state == UPDATES_APP_AVAILABLE) {
        app_update_skipped = true;
        begin(JOB_PUZZLES);
    } else if (state == UPDATES_PUZZLES_AVAILABLE) {
        remove(CONNECTIONS_TEMP_PATH);
        finish("Puzzle update skipped.");
    }
}

void platform_updates_cancel(void) {
    SDL_AtomicSet(&cancelled, 1);
}

void platform_updates_exit(void) {
    if (worker) {
        platform_updates_cancel();
        SDL_WaitThread(worker, NULL);
        worker = NULL;
    }
    if (nro_temp[0]) remove(nro_temp);
    remove(NSP_TEMP);
    remove(CONNECTIONS_TEMP_PATH);
}

PlatformUpdateState platform_updates_state(void) { return state; }
unsigned platform_updates_revision(void) { return revision; }
bool platform_updates_busy(void) { return worker != NULL; }
bool platform_updates_is_nsp_offer(void) { return nsp_offered && (state == UPDATES_APP_AVAILABLE || state == UPDATES_DOWNLOADING_APP); }
const char *platform_updates_available_version(void) { return nsp_offered ? nsp_version : offered_info.version; }
bool platform_updates_native_restart_pending(void) { return native_restart_pending; }
const char *platform_updates_status(void) { return status; }
int platform_updates_progress(void) { return SDL_AtomicGet(&progress_percent); }
