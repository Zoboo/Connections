#pragma once

#include <stdbool.h>
#include <SDL2/SDL.h>

typedef struct {
    int width;
    int height;
    SDL_Texture *texture;
} Image;

bool image_load_png(const char *path, Image *out_image);
void image_free(Image *image);
