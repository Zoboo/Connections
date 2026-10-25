#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifndef CONNECTIONS_PROFILE
#define CONNECTIONS_PROFILE 1
#endif
#if CONNECTIONS_PROFILE
void platform_profile_init(void);
bool platform_profile_is_enabled(void);
void platform_profile_exit(void);
void platform_profile_frame_begin(void);
void platform_profile_update_end(void);
void platform_profile_render_end(void);
void platform_profile_frame_end(void);
void platform_profile_draw(void);
void platform_profile_worker_sample(uint32_t microseconds);
void platform_profile_set_scenario(const char *name);
#else
static inline void platform_profile_init(void) {}
static inline bool platform_profile_is_enabled(void) { return false; }
static inline bool platform_profile_is_enabled(void) { return false; }
static inline void platform_profile_exit(void) {}
static inline void platform_profile_frame_begin(void) {}
static inline void platform_profile_update_end(void) {}
static inline void platform_profile_render_end(void) {}
static inline void platform_profile_frame_end(void) {}
static inline void platform_profile_draw(void) {}
static inline void platform_profile_worker_sample(uint32_t us) { (void)us; }
static inline void platform_profile_set_scenario(const char *name) { (void)name; }
#endif
