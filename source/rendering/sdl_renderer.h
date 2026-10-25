#pragma once

#include <SDL2/SDL.h>
#include <stdbool.h>

bool sdl_renderer_init(void);
void sdl_renderer_begin_frame(void);
void sdl_renderer_end_frame(void);
void sdl_renderer_shutdown(void);

SDL_Renderer *sdl_renderer_get(void);