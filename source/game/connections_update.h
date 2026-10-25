#pragma once

typedef enum {
    CONNECTIONS_UPDATE_INSTALLED,
    CONNECTIONS_UPDATE_INVALID,
    CONNECTIONS_UPDATE_OLDER,
    CONNECTIONS_UPDATE_STORAGE_ERROR
} ConnectionsUpdateResult;

// staged, current and backup must be distinct paths on the same filesystem.
ConnectionsUpdateResult connections_update_install(const char *staged, const char *current,
                                                   const char *backup, const char *bundled);
