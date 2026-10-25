#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NSP_UPDATE_INDEX_LIMIT 4096
#define NSP_UPDATE_MANIFEST_LIMIT 512
#define NSP_UPDATE_MAX_SIZE (512ULL * 1024 * 1024)

bool nsp_update_index(const unsigned char *data, size_t length, uint64_t total,
                      uint64_t *offset, size_t *size);
bool nsp_update_manifest(const unsigned char *data, size_t length,
                         char *version, size_t capacity, uint32_t *native_version);
