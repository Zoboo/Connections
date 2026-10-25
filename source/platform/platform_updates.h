#pragma once
#include <stdbool.h>

typedef enum {
    UPDATES_IDLE, UPDATES_CHECKING_APP, UPDATES_APP_AVAILABLE,
    UPDATES_DOWNLOADING_APP, UPDATES_CHECKING_PUZZLES, UPDATES_PUZZLES_AVAILABLE,
    UPDATES_FINISHED, UPDATES_RESTART_READY
} PlatformUpdateState;

void platform_updates_init(int argc, char **argv);
void platform_updates_start(void);
void platform_updates_start_launch(void);
unsigned platform_updates_revision(void);
bool platform_updates_poll(void);
void platform_updates_accept(void);
void platform_updates_skip(void);
void platform_updates_cancel(void);
void platform_updates_exit(void);
PlatformUpdateState platform_updates_state(void);
bool platform_updates_busy(void);
bool platform_updates_is_nsp_offer(void);
const char *platform_updates_available_version(void);
bool platform_updates_native_restart_pending(void);
const char *platform_updates_status(void);
// -1 means the server has not supplied a transfer size yet; otherwise 0..100.
int platform_updates_progress(void);
