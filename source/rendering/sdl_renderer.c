#include "sdl_renderer.h"
#include <SDL2/SDL_image.h>
#include "drawing.h"
#include "platform/platform_graphics.h"

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static bool image_initialized = false;

bool sdl_renderer_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        return false;

    if ((IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) & (IMG_INIT_PNG | IMG_INIT_JPG)) != (IMG_INIT_PNG | IMG_INIT_JPG)) {
        SDL_Quit();
        return false;
    }
    image_initialized = true;

    window = SDL_CreateWindow("Connections", 0, 0, 1280, 720, 0);
    if (!window) {
        IMG_Quit();
        SDL_Quit();
        image_initialized = false;
        return false;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        SDL_DestroyWindow(window);
        IMG_Quit();
        SDL_Quit();
        window = NULL;
        image_initialized = false;
        return false;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_RenderSetLogicalSize(renderer, 1280, 720);
    platform_graphics_init();
    drawing_init();

    return true;
}

void sdl_renderer_begin_frame(void) {
    SDL_SetRenderDrawColor(renderer, 20, 20, 40, 255);
    SDL_RenderClear(renderer);
}

void sdl_renderer_end_frame(void) {
    SDL_RenderPresent(renderer);
}

SDL_Renderer *sdl_renderer_get(void) {
    return renderer;
}

void sdl_renderer_shutdown(void) {
    drawing_shutdown();
    platform_graphics_shutdown();
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);

    renderer = NULL;
    window = NULL;

    if (image_initialized) {
        IMG_Quit();
        image_initialized = false;
    }

    SDL_Quit();
}
