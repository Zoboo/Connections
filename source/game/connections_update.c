#include "connections_update.h"
#include "connections_puzzle.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static bool remove_if_present(const char *path) {
    return remove(path) == 0 || errno == ENOENT;
}

ConnectionsUpdateResult connections_update_install(const char *staged, const char *current,
                                                   const char *backup, const char *bundled) {
    ConnectionsDataInfo incoming, previous;
    if (!connections_puzzle_validate_file(staged, &incoming))
        return CONNECTIONS_UPDATE_INVALID;

    bool current_valid = connections_puzzle_validate_file(current, &previous);
    bool have_previous = current_valid || connections_puzzle_validate_file(backup, &previous) ||
                         connections_puzzle_validate_file(bundled, &previous);
    // A valid but shortened archive is also unsafe for saved puzzle IDs.
    if (have_previous && (incoming.puzzle_count < previous.puzzle_count ||
                          incoming.playable_count < previous.playable_count ||
                          strcmp(incoming.latest_date, previous.latest_date) < 0))
        return CONNECTIONS_UPDATE_OLDER;

    if (current_valid) {
        // At every step either current or backup still holds the old database.
        // Do not rely on rename() replacing an existing file on the Switch SD.
        if (!remove_if_present(backup) || rename(current, backup) != 0)
            return CONNECTIONS_UPDATE_STORAGE_ERROR;
    } else if (!remove_if_present(current)) {
        return CONNECTIONS_UPDATE_STORAGE_ERROR;
    }

    if (rename(staged, current) != 0) {
        if (current_valid)
            rename(backup, current); // If rollback fails, the loader uses backup.
        return CONNECTIONS_UPDATE_STORAGE_ERROR;
    }
    return CONNECTIONS_UPDATE_INSTALLED;
}
