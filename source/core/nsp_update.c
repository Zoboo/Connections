#include "nsp_update.h"
#include "app_update.h"
#include "libs/cjson/cJSON.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const unsigned char *p) {
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

bool nsp_update_index(const unsigned char *data, size_t length, uint64_t total,
                      uint64_t *offset, size_t *size) {
    if (length < 16 || memcmp(data, "PFS0", 4) || total > NSP_UPDATE_MAX_SIZE)
        return false;
    uint32_t count = le32(data + 4), strings = le32(data + 8);
    if (!count || count > 16 || !strings || strings > 1024)
        return false;
    size_t base = 16 + (size_t)count * 24 + strings;
    if (base > length || base > total)
        return false;
    const unsigned char *names = data + 16 + count * 24;
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *entry = data + 16 + i * 24;
        uint32_t name = le32(entry + 16);
        uint64_t start = le64(entry), bytes = le64(entry + 8);
        if (name >= strings || !memchr(names + name, 0, strings - name) ||
            start > total - base || bytes > total - base - start)
            return false;
        if (!strcmp((const char *)names + name, "connections-update.json")) {
            if (found || !bytes || bytes >= NSP_UPDATE_MANIFEST_LIMIT)
                return false;
            *offset = base + start;
            *size = (size_t)bytes;
            found = true;
        }
    }
    return found;
}

bool nsp_update_manifest(const unsigned char *data, size_t length,
                         char *version, size_t capacity, uint32_t *native_version) {
    if (!length || length >= NSP_UPDATE_MANIFEST_LIMIT || memchr(data, 0, length))
        return false;
    char text[NSP_UPDATE_MANIFEST_LIMIT];
    memcpy(text, data, length);
    text[length] = 0;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithOpts(text, &end, true);
    if (!root) return false;
    cJSON *format = cJSON_GetObjectItemCaseSensitive(root, "format");
    cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title_id");
    cJSON *release = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *native = cJSON_GetObjectItemCaseSensitive(root, "native_version");
    int order;
    bool valid = end == text + length && cJSON_IsNumber(format) && format->valuedouble == 1 &&
        cJSON_IsString(title) && !strcmp(title->valuestring, "0100C04E4E530000") &&
        cJSON_IsString(release) && strlen(release->valuestring) < capacity &&
        app_update_version_compare(release->valuestring, release->valuestring, &order) &&
        cJSON_IsNumber(native) && native->valuedouble >= 0 && native->valuedouble <= UINT32_MAX &&
        floor(native->valuedouble) == native->valuedouble;
    if (valid) {
        unsigned major = 0, minor = 0, patch = 0;
        valid = sscanf(release->valuestring, "%u.%u.%u", &major, &minor, &patch) == 3 &&
            major <= 255 && minor <= 255 && patch <= 255 &&
            ((uint32_t)native->valuedouble >> 8) == (major << 16 | minor << 8 | patch);
    }
    if (valid) {
        strcpy(version, release->valuestring);
        *native_version = (uint32_t)native->valuedouble;
    }
    cJSON_Delete(root);
    return valid;
}
