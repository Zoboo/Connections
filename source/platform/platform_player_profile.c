#include "platform_player_profile.h"
#include "core/version.h"
#include "rendering/sdl_renderer.h"
#include <SDL2/SDL_image.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PROFILE_IMAGE_MAX (128 * 1024)
static LocalPlayerProfile local_profile;
static Image fallback_icon;
static bool initialized;
static SDL_Thread *logger;
static char diagnostics[4096];
static size_t diagnostics_length;

static void diagnostic(const char *format, ...) {
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    SDL_Log("[Account] %s", line);
    size_t remaining = sizeof(diagnostics) - diagnostics_length;
    if (remaining > 1) {
        int length = snprintf(diagnostics + diagnostics_length, remaining, "%s\n", line);
        if (length > 0) diagnostics_length += (size_t)length < remaining ? (size_t)length : remaining - 1;
    }
}

static int write_diagnostics(void *unused) {
    (void)unused;
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/Connections", 0777);
    FILE *file = fopen("sdmc:/switch/Connections/account.log", "ab");
    if (file) { fputs(diagnostics, file); fclose(file); }
    else SDL_Log("[Account] Unable to open account.log");
    return 0;
}

static bool selected_uid(const char *api, Result result, AccountUid uid) {
    bool valid = R_SUCCEEDED(result) && accountUidIsValid(&uid);
    diagnostic("%s: Result=0x%08lX valid=%s UID=%016llX:%016llX", api,
        (unsigned long)result, valid ? "YES" : "NO",
        (unsigned long long)uid.uid[0], (unsigned long long)uid.uid[1]);
    return valid;
}

static void fallback_avatar(void) {
    for (int y = 0; y < PLAYER_PROFILE_AVATAR_SIZE; ++y) {
        for (int x = 0; x < PLAYER_PROFILE_AVATAR_SIZE; ++x) {
            int dx = x - 16, dy = y - 11;
            bool silhouette = dx * dx + dy * dy <= 36 ||
                (y >= 20 && y <= 29 && dx * dx + (y - 29) * (y - 29) <= 121);
            uint8_t *pixel = &local_profile.avatar_rgba[(y * PLAYER_PROFILE_AVATAR_SIZE + x) * 4];
            pixel[0] = silhouette ? 245 : 70;
            pixel[1] = silhouette ? 245 : 70;
            pixel[2] = silhouette ? 245 : 70;
            pixel[3] = 255;
        }
    }
}

static void load_icon(AccountProfile *profile) {
    u32 size = 0, actual = 0;
    Result result = accountProfileGetImageSize(profile, &size);
    diagnostic("accountProfileGetImageSize: Result=0x%08lX size=%lu", (unsigned long)result, (unsigned long)size);
    if (R_FAILED(result) || size < 3 || size > PROFILE_IMAGE_MAX) return;
    unsigned char *bytes = malloc(size);
    if (!bytes) { diagnostic("Icon allocation failed"); return; }
    result = accountProfileLoadImage(profile, bytes, size, &actual);
    diagnostic("accountProfileLoadImage: Result=0x%08lX size=%lu", (unsigned long)result, (unsigned long)actual);
    if (R_SUCCEEDED(result) && actual >= 3 && actual <= size && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        diagnostic("Icon format: JPEG");
        SDL_RWops *stream = SDL_RWFromConstMem(bytes, (int)actual);
        SDL_Surface *decoded = stream ? IMG_Load_RW(stream, 1) : NULL;
        if (decoded && decoded->w > 0 && decoded->h > 0 && decoded->w <= 1024 && decoded->h <= 1024) {
            uint32_t indexed_color = local_profile.color;
            local_profile.color = platform_player_profile_resolve_color(local_profile.color_id, decoded);
            diagnostic("Icon background comparison: %s", local_profile.color == indexed_color ?
                "indexed mapping retained" : "observed palette background overrides inferred index");
            SDL_Surface *thumbnail = SDL_CreateRGBSurfaceWithFormatFrom(local_profile.avatar_rgba,
                PLAYER_PROFILE_AVATAR_SIZE, PLAYER_PROFILE_AVATAR_SIZE, 32,
                PLAYER_PROFILE_AVATAR_SIZE * 4, SDL_PIXELFORMAT_RGBA32);
            if (thumbnail) {
                SDL_SetSurfaceBlendMode(decoded, SDL_BLENDMODE_NONE);
                if (SDL_BlitScaled(decoded, NULL, thumbnail, NULL) == 0) local_profile.icon_available = true;
                SDL_FreeSurface(thumbnail);
            }
            if (local_profile.icon_available) {
                local_profile.icon.texture = SDL_CreateTextureFromSurface(sdl_renderer_get(), decoded);
                if (local_profile.icon.texture) {
                    local_profile.icon.width = decoded->w;
                    local_profile.icon.height = decoded->h;
                }
            }
        }
        if (decoded) SDL_FreeSurface(decoded);
        if (!local_profile.icon_available) diagnostic("Icon decoding failed: %s", IMG_GetError());
    } else if (R_SUCCEEDED(result)) diagnostic("Unsupported or invalid Account icon data");
    free(bytes);
}

void platform_player_profile_init(void) {
    if (initialized) return;
    initialized = true;
    memset(&local_profile, 0, sizeof(local_profile));
    strcpy(local_profile.nickname, "Player");
    local_profile.color_id = 255;
    local_profile.color = platform_player_profile_color(local_profile.color_id);
    fallback_avatar();
    SDL_Surface *fallback = SDL_CreateRGBSurfaceWithFormatFrom(local_profile.avatar_rgba,
        PLAYER_PROFILE_AVATAR_SIZE, PLAYER_PROFILE_AVATAR_SIZE, 32,
        PLAYER_PROFILE_AVATAR_SIZE * 4, SDL_PIXELFORMAT_RGBA32);
    if (fallback) {
        fallback_icon.texture = SDL_CreateTextureFromSurface(sdl_renderer_get(), fallback);
        SDL_FreeSurface(fallback);
        if (fallback_icon.texture) fallback_icon.width = fallback_icon.height = PLAYER_PROFILE_AVATAR_SIZE;
    }
    diagnostic("Connections version=" CONNECTIONS_APP_VERSION);
    diagnostic("AppletType=%d executable=%s", (int)appletGetAppletType(),
        envIsNso() ? "NSO (installed NSP application)" : "NRO (homebrew loader)");
    Result result = accountInitialize(AccountServiceType_Application);
    diagnostic("accountInitialize(Application): Result=0x%08lX", (unsigned long)result);
    bool selected = false;
    if (R_SUCCEEDED(result)) {
        s32 count = 0;
        Result count_result = accountGetUserCount(&count);
        diagnostic("accountGetUserCount: Result=0x%08lX count=%ld", (unsigned long)count_result, (long)count);
        AccountUid uid = {0};
        result = accountGetPreselectedUser(&uid);
        selected = selected_uid("accountGetPreselectedUser", result, uid);
        if (!selected) {
            memset(&uid, 0, sizeof(uid));
            result = accountTrySelectUserWithoutInteraction(&uid, false);
            selected = selected_uid("accountTrySelectUserWithoutInteraction(false)", result, uid);
        }
        if (!selected) {
            memset(&uid, 0, sizeof(uid));
            result = accountGetLastOpenedUser(&uid);
            selected = selected_uid("accountGetLastOpenedUser", result, uid);
        }
        if (selected) {
            local_profile.uid = uid;
            AccountProfile profile = {0};
            result = accountGetProfile(&profile, uid);
            diagnostic("accountGetProfile: Result=0x%08lX", (unsigned long)result);
            if (R_SUCCEEDED(result)) {
                AccountProfileBase base = {0};
                AccountUserData data = {0};
                result = accountProfileGet(&profile, &data, &base);
                diagnostic("accountProfileGet: Result=0x%08lX", (unsigned long)result);
                if (R_SUCCEEDED(result)) {
                    local_profile.color_id = data.iconBackgroundColorID;
                    local_profile.color = platform_player_profile_color(local_profile.color_id);
                    size_t length = strnlen(base.nickname, sizeof(base.nickname));
                    if (length) {
                        memcpy(local_profile.nickname, base.nickname, length);
                        local_profile.nickname[length] = 0;
                        local_profile.nickname_available = true;
                    }
                }
                load_icon(&profile);
                accountProfileClose(&profile);
            }
        } else {
            AccountUid users[ACC_USER_LIST_SIZE] = {0};
            s32 total = 0;
            result = accountListAllUsers(users, ACC_USER_LIST_SIZE, &total);
            diagnostic("accountListAllUsers (diagnostic only): Result=0x%08lX count=%ld", (unsigned long)result, (long)total);
            // Never pick an arbitrary profile from this list.
        }
        accountExit();
    }
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(local_profile.avatar_rgba,
        PLAYER_PROFILE_AVATAR_SIZE, PLAYER_PROFILE_AVATAR_SIZE, 32,
        PLAYER_PROFILE_AVATAR_SIZE * 4, SDL_PIXELFORMAT_RGBA32);
    if (surface && !local_profile.icon.texture) {
        local_profile.icon.texture = SDL_CreateTextureFromSurface(sdl_renderer_get(), surface);
        if (local_profile.icon.texture) local_profile.icon.width = local_profile.icon.height = PLAYER_PROFILE_AVATAR_SIZE;
        else diagnostic("Icon texture creation failed: %s", SDL_GetError());
    }
    if (surface) SDL_FreeSurface(surface);
    diagnostic("Selected user UID valid: %s", selected ? "YES" : "NO");
    diagnostic("Nickname retrieval: %s", local_profile.nickname_available ? "SUCCESS" : "FAILURE");
    diagnostic("Nickname: %s", local_profile.nickname);
    diagnostic("Icon retrieval: %s", local_profile.icon_available ? "SUCCESS" : "FAILURE (fallback icon)");
    diagnostic("Background color ID: %u", (unsigned)local_profile.color_id);
    diagnostic("Resolved RGBA: %u,%u,%u,%u", (unsigned)(local_profile.color & 255),
        (unsigned)((local_profile.color >> 8) & 255), (unsigned)((local_profile.color >> 16) & 255),
        (unsigned)(local_profile.color >> 24));
    logger = SDL_CreateThread(write_diagnostics, "Account diagnostics", NULL);
    if (!logger) diagnostic("Unable to start Account log worker: %s", SDL_GetError());
}

const LocalPlayerProfile *platform_player_profile_get(void) { return &local_profile; }
const Image *platform_player_profile_fallback_icon(void) { return &fallback_icon; }

void platform_player_profile_shutdown(void) {
    if (logger) { SDL_WaitThread(logger, NULL); logger = NULL; }
    image_free(&local_profile.icon);
    image_free(&fallback_icon);
    initialized = false;
    diagnostics_length = 0;
    diagnostics[0] = 0;
}
