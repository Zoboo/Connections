#include "profile_colors.h"
#include <SDL2/SDL.h>
#include <limits.h>

/* Local profile-background palette, hue groups from light to dark.
 * Palette recorded by the original asset contributor:
 * https://www.spriters-resource.com/nintendo_switch/systembios/asset/93824/page-2/
 * Ordered palette: https://lospec.com/palette-list/nintendo-switch-profile-icon-background-colors
 * Zero-based IDs in this published order are inferred, not firmware-verified.
 * The Account API does not document this index mapping. Keep it centralized
 * so firmware-specific corrections do not change gameplay or networking.
 */
static const uint32_t profile_rgb[] = {
    0xFFCCCC, 0xFF9999, 0xFF6969, 0xFF0000, 0xC80A0A, 0x6F1010,
    0xFFE3CC, 0xFFC399, 0xFFB066, 0xFF8400, 0xC17F3A, 0x6E4722,
    0xFFFF99, 0xFFFF00, 0xFFE600, 0xFFB600, 0xBFA645, 0x7E6C0F,
    0xF0FF99, 0xE6FF31, 0xD3F025, 0xC6D600, 0xA1B026, 0x67730F,
    0xD4F9C7, 0xB4F49C, 0x86E66C, 0x34D70D, 0x4FA31D, 0x247519,
    0xBFEFE1, 0x91E3CE, 0x65E6C3, 0x00DBA1, 0x2EAC8B, 0x0F7358,
    0xC0E4F0, 0x91DCF2, 0x5ED6EB, 0x13BEED, 0x3496C1, 0x165E72,
    0xAED5F7, 0x80BDFF, 0x479BFF, 0x314FFF, 0x0051BD, 0x1C3D68,
    0xE4CCFF, 0xD2A8FF, 0xB66BFF, 0x8C19FF, 0x711DBD, 0x451278,
    0xFFCCF9, 0xFFA6E4, 0xFF73D4, 0xFF19DF, 0xBF0ABC, 0x691579,
    0xF5F5F5, 0xD9D9D9, 0xB3B3B3, 0x808080, 0x474B55, 0x282828,
    0xEBF0F7, 0xC3CAD9, 0x99A0B3, 0x666C80, 0x3C4459, 0x131D45
};

uint32_t platform_player_profile_color(uint8_t id) {
    if (id >= sizeof(profile_rgb) / sizeof(profile_rgb[0]))
        return PROFILE_COLOR_FALLBACK;
    uint32_t rgb = profile_rgb[id];
    return UINT32_C(0xFF000000) | ((rgb & 0xFF) << 16) |
           (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}

uint32_t platform_player_profile_resolve_color(uint8_t id, const SDL_Surface *decoded) {
    uint32_t indexed = platform_player_profile_color(id);
    if (id >= sizeof(profile_rgb) / sizeof(profile_rgb[0])) return PROFILE_COLOR_FALLBACK;
    if (id >= sizeof(profile_rgb) / sizeof(profile_rgb[0]) ||
        !decoded || decoded->w < 8 || decoded->h < 8) return indexed;
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat((SDL_Surface *)decoded, SDL_PIXELFORMAT_RGBA32, 0);
    if (!rgba) return indexed;
    unsigned votes[sizeof(profile_rgb) / sizeof(profile_rgb[0])] = {0};
    for (int corner = 0; corner < 4; ++corner) {
        int x = (corner & 1) ? rgba->w - 4 : 1;
        int y = (corner & 2) ? rgba->h - 4 : 1;
        unsigned channels[3] = {0};
        for (int dy = 0; dy < 3; ++dy) for (int dx = 0; dx < 3; ++dx) {
            const uint8_t *pixel = (const uint8_t *)rgba->pixels + (y + dy) * rgba->pitch + (x + dx) * 4;
            for (int channel = 0; channel < 3; ++channel) channels[channel] += pixel[channel];
        }
        unsigned best = 0, distance = UINT_MAX;
        for (unsigned i = 0; i < sizeof(profile_rgb) / sizeof(profile_rgb[0]); ++i) {
            unsigned error = 0;
            for (int channel = 0; channel < 3; ++channel) {
                int difference = (int)(channels[channel] / 9) - (int)((profile_rgb[i] >> (16 - channel * 8)) & 255);
                error += (unsigned)(difference * difference);
            }
            if (error < distance) { distance = error; best = i; }
        }
        if (distance < 300) ++votes[best];
    }
    SDL_FreeSurface(rgba);
    for (unsigned i = 0; i < sizeof(profile_rgb) / sizeof(profile_rgb[0]); ++i) {
        if (votes[i] >= 3) {
            uint32_t observed = platform_player_profile_color((uint8_t)i);
            SDL_Log("[Account] Icon background evidence: palette=%u matches indexed color=%s", i,
                observed == indexed ? "YES" : "NO (using observed palette color)");
            return observed;
        }
    }
    SDL_Log("[Account] Icon background evidence ambiguous; using centralized color ID mapping");
    return indexed;
}
