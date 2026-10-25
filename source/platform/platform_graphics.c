#include "platform_graphics.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static bool modern_graphics = true;
static const char settings_path[] = "sdmc:/switch/Connections/graphics.cfg";

void platform_graphics_init(void) {
    char value[16] = {0};
    FILE *file = fopen(settings_path, "rb");
    modern_graphics = true;
    if (!file) return;
    if (fgets(value, sizeof(value), file))
        modern_graphics = strncmp(value, "legacy", 6) != 0;
    fclose(file);
}

void platform_graphics_shutdown(void) {
}

bool platform_graphics_modern(void) {
    return modern_graphics;
}

bool platform_graphics_set_modern(bool modern) {
    char temporary[128];
    modern_graphics = modern;
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/Connections", 0777);
    snprintf(temporary, sizeof(temporary), "%s.tmp", settings_path);
    FILE *file = fopen(temporary, "wb");
    if (!file) return false;
    bool written = fputs(modern ? "modern\n" : "legacy\n", file) >= 0;
    if (fclose(file) != 0) written = false;
    if (!written || rename(temporary, settings_path) != 0) {
        remove(temporary);
        return false;
    }
    return true;
}
