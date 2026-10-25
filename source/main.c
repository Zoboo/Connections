#include <switch.h>
#include <stdio.h>

#include "platform/platform.h"
#include "rendering/colors.h"
#include "screens/screen_manager.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include "platform/platform_audio.h"
#include "platform/platform_updates.h"
#include "rendering/sdl_renderer.h"
#include "multiplayer/multiplayer.h"
#include "screens/multiplayer_screen.h"
#include "platform/platform_profile.h"
#include "platform/platform_player_profile.h"

int main(int argc, char* argv[]) {
    platform_init();

    if (!sdl_renderer_init())
        return 1;

    platform_audio_init();
    platform_profile_init();
    platform_player_profile_init();

    platform_updates_init(argc, argv);
    mp_init();
    screen_manager_init();
    platform_updates_start_launch();

    // This static board game needs only 30 Hz UI updates. Network I/O remains
    // on its own worker; pace the main thread even with a software renderer.
    const Uint64 frame_ticks = SDL_GetPerformanceFrequency() / 30;
    while (appletMainLoop()) {
        Uint64 frame_started = SDL_GetPerformanceCounter();
#if CONNECTIONS_PROFILE
        if (platform_profile_is_enabled()) platform_profile_set_scenario(screen_manager_profile_scenario());
#endif
        platform_profile_frame_begin();
        platform_update();

        PlatformInput input = platform_get_input();

        platform_updates_poll();

        screen_manager_update(input);
        platform_profile_update_end();

        if (screen_manager_should_exit()) {
            break;
        }

        sdl_renderer_begin_frame();
        screen_manager_draw();
        platform_profile_draw();
        platform_profile_render_end();
        sdl_renderer_end_frame();
        Uint64 elapsed = SDL_GetPerformanceCounter() - frame_started;
        if (elapsed < frame_ticks) {
            Uint64 remaining_ms = (frame_ticks - elapsed) * 1000 / SDL_GetPerformanceFrequency();
            if (remaining_ms) SDL_Delay((Uint32)remaining_ms);
        }
        platform_profile_frame_end();
    }

    platform_updates_exit();

    screen_manager_exit();
    mp_shutdown();
    multiplayer_screen_shutdown();
    platform_player_profile_shutdown();
    platform_profile_exit();
    platform_audio_exit();
    sdl_renderer_shutdown();

    platform_exit();

    if (platform_updates_native_restart_pending()) {
        Result result = appletRestartProgram(NULL, 0);
        // Successful relaunch never returns; retain diagnostics if Horizon refuses it.
        FILE *log = fopen("sdmc:/switch/Connections/update.log", "a");
        if (log) { fprintf(log,"appletRestartProgram Result=0x%08lX\n",(unsigned long)result); fclose(log); }
        SDL_Log("appletRestartProgram Result=0x%08lX",(unsigned long)result);
    }

    return 0;
}
