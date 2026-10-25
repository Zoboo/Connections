#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <SDL2/SDL_atomic.h>

/* Worker-only: installs this application's validated native package, never arbitrary titles. */
bool platform_nsp_install(const char *path, uint32_t expected_version,
                          SDL_atomic_t *cancel, SDL_atomic_t *progress, char *error, size_t error_capacity);
