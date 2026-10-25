#pragma once

#include <switch.h>
#include <stdint.h>
#include "rendering/image.h"
#include "profile_colors.h"

#define PLAYER_PROFILE_AVATAR_SIZE 32
#define PLAYER_PROFILE_AVATAR_BYTES (PLAYER_PROFILE_AVATAR_SIZE * PLAYER_PROFILE_AVATAR_SIZE * 4)

typedef struct {
    AccountUid uid;
    char nickname[33];
    Image icon;
    uint8_t color_id;
    uint32_t color; // Renderer color: 0xAABBGGRR.
    bool nickname_available;
    bool icon_available;
    uint8_t avatar_rgba[PLAYER_PROFILE_AVATAR_BYTES];
} LocalPlayerProfile;

// Call once after renderer initialization; Account access and JPEG decoding are cached.
void platform_player_profile_init(void);
const LocalPlayerProfile *platform_player_profile_get(void);
void platform_player_profile_shutdown(void);
const Image *platform_player_profile_fallback_icon(void);
