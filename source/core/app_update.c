#include "app_update.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static uint32_t read32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t read64(const unsigned char *p) {
    return read32(p) | (uint64_t)read32(p + 4) << 32;
}

static bool version_parts(const char *text, unsigned parts[3]) {
    if (!text || !*text)
        return false;
    for (int i = 0; i < 3; i++) {
        unsigned value = 0;
        if (*text < '0' || *text > '9')
            return false;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + (unsigned)(*text++ - '0');
            if (value > 999999)
                return false;
        }
        parts[i] = value;
        if (i < 2 && *text++ != '.')
            return false;
    }
    return *text == '\0';
}

bool app_update_version_compare(const char *left, const char *right, int *order) {
    unsigned a[3], b[3];
    if (!version_parts(left, a) || !version_parts(right, b))
        return false;
    *order = 0;
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) {
            *order = a[i] > b[i] ? 1 : -1;
            break;
        }
    }
    return true;
}

bool app_update_parse_header(const unsigned char *header, uint64_t size, AppUpdateInfo *info) {
    memset(info, 0, sizeof(*info));
    if (size < APP_UPDATE_HEADER_SIZE + APP_UPDATE_ASSET_SIZE || size > APP_UPDATE_MAX_SIZE ||
        memcmp(header + 16, "NRO0", 4) != 0)
        return false;
    uint32_t code_size = read32(header + 24);
    if (code_size < APP_UPDATE_HEADER_SIZE || code_size > size - APP_UPDATE_ASSET_SIZE || (code_size & 0xFFF))
        return false;
    uint64_t previous_end = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t offset = read32(header + 32 + i * 8);
        uint32_t length = read32(header + 36 + i * 8);
        if ((offset & 0xFFF) || (length & 0xFFF) || (uint64_t)offset + length > code_size || offset < previous_end)
            return false;
        if (i == 0 && (offset != 0 || length < APP_UPDATE_HEADER_SIZE))
            return false;
        previous_end = (uint64_t)offset + length;
    }
    info->file_size = size;
    info->asset_offset = code_size;
    memcpy(info->build_id, header + 64, sizeof(info->build_id));
    return true;
}

bool app_update_parse_assets(const unsigned char *assets, AppUpdateInfo *info) {
    if (memcmp(assets, "ASET", 4) != 0 || read32(assets + 4) != 0)
        return false;
    uint64_t available = info->file_size - info->asset_offset;
    uint64_t previous_end = APP_UPDATE_ASSET_SIZE;
    for (int i = 0; i < 3; i++) {
        uint64_t offset = read64(assets + 8 + i * 16);
        uint64_t size = read64(assets + 16 + i * 16);
        if (!size && i == 0)
            continue;
        if (offset < previous_end || offset > available || size > available - offset || !size)
            return false;
        previous_end = offset + size;
        if (i == 1) {
            if (size != APP_UPDATE_NACP_SIZE)
                return false;
            info->nacp_offset = info->asset_offset + offset;
        }
        if (i == 2)
            info->romfs_offset = info->asset_offset + offset;
    }
    return true;
}

bool app_update_parse_nacp(const unsigned char *nacp, AppUpdateInfo *info) {
    // Reject HTML, unrelated homebrew, missing NACP, and unverifiable versions.
    if (memcmp(nacp, "Connections\0", 12) != 0 || !memchr(nacp + 0x3060, 0, 16))
        return false;
    memcpy(info->version, nacp + 0x3060, 16);
    int order;
    return app_update_version_compare(info->version, info->version, &order);
}

bool app_update_read_file(const char *path, AppUpdateInfo *info) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    unsigned char header[APP_UPDATE_HEADER_SIZE], assets[APP_UPDATE_ASSET_SIZE], nacp[APP_UPDATE_NACP_SIZE];
    bool ok = false;
    if (fseek(file, 0, SEEK_END) != 0)
        goto done;
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0 || fread(header, 1, sizeof(header), file) != sizeof(header) ||
        !app_update_parse_header(header, (uint64_t)size, info))
        goto done;
    if (fseek(file, (long)info->asset_offset, SEEK_SET) != 0 || fread(assets, 1, sizeof(assets), file) != sizeof(assets) ||
        !app_update_parse_assets(assets, info))
        goto done;
    if (fseek(file, (long)info->nacp_offset, SEEK_SET) != 0 || fread(nacp, 1, sizeof(nacp), file) != sizeof(nacp))
        goto done;
    ok = app_update_parse_nacp(nacp, info);
done:
    fclose(file);
    return ok;
}

bool app_update_launch_path(const char *argument, char *path, size_t capacity) {
    if (!argument || !capacity)
        return false;
    const char *relative = argument;
    if (strncmp(relative, "sdmc:", 5) == 0)
        relative += 5;
    size_t length = strlen(relative);
    if (*relative != '/' || length < 5 || strcmp(relative + length - 4, ".nro") != 0 ||
        length + 6 > capacity || length + 16 >= APP_UPDATE_PATH_SIZE ||
        strchr(relative, ':') || strchr(relative, '\\') || strchr(relative, '"'))
        return false;
    for (const char *p = relative; *p; p++) {
        if ((unsigned char)*p < 0x20 || (*p == '/' && (p[1] == '/' ||
            (p[1] == '.' && (p[2] == '/' || (p[2] == '.' && p[3] == '/'))))))
            return false;
    }
    snprintf(path, capacity, "sdmc:%s", relative);
    return true;
}

bool app_update_replace(const char *staged, const char *current, const char *backup) {
    if (remove(backup) != 0 && errno != ENOENT)
        return false;
    if (rename(current, backup) != 0)
        return false;
    if (rename(staged, current) == 0)
        return true;
    rename(backup, current);
    return false;
}
