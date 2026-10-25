#ifndef CONNECTIONS_PROFILE_COLORS_H
#define CONNECTIONS_PROFILE_COLORS_H

#include <stdint.h>

#define PROFILE_COLOR_UNKNOWN UINT8_MAX
#define PROFILE_COLOR_FALLBACK UINT32_C(0xFF555555)

/* Returns the renderer's packed ABGR color (red in the low byte). */
uint32_t platform_player_profile_color(uint8_t id);
struct SDL_Surface;
uint32_t platform_player_profile_resolve_color(uint8_t id, const struct SDL_Surface *decoded);

#endif
