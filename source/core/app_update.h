#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_UPDATE_MAX_SIZE (64u * 1024u * 1024u)
#define APP_UPDATE_HEADER_SIZE 128
#define APP_UPDATE_ASSET_SIZE 56
#define APP_UPDATE_NACP_SIZE 0x4000
// hbloader's next-NRO path buffer is 512 bytes (envSetNextLoad uses strcpy).
#define APP_UPDATE_PATH_SIZE 512

typedef struct {
    uint64_t file_size;
    uint32_t asset_offset;
    uint64_t nacp_offset;
    uint64_t romfs_offset;
    unsigned char build_id[32];
    char version[16];
} AppUpdateInfo;

bool app_update_version_compare(const char *left, const char *right, int *order);
bool app_update_parse_header(const unsigned char *header, uint64_t size, AppUpdateInfo *info);
bool app_update_parse_assets(const unsigned char *assets, AppUpdateInfo *info);
bool app_update_parse_nacp(const unsigned char *nacp, AppUpdateInfo *info);
bool app_update_read_file(const char *path, AppUpdateInfo *info);
bool app_update_launch_path(const char *argument, char *path, size_t capacity);
// Preserve a recoverable backup if promotion/rollback fails.
bool app_update_replace(const char *staged, const char *current, const char *backup);
