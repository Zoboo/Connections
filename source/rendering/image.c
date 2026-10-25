#include "image.h"
#include "sdl_renderer.h"

#include <SDL2/SDL_image.h>

bool image_load_png(const char *path, Image *out_image) {
    SDL_Texture *texture = IMG_LoadTexture(sdl_renderer_get(), path);
    if (!texture)
        return false;

    int width = 0;
    int height = 0;

    if (SDL_QueryTexture(texture, NULL, NULL, &width, &height) != 0) {
        SDL_DestroyTexture(texture);
        return false;
    }

    out_image->width = width;
    out_image->height = height;
    out_image->texture = texture;

    return true;
}

void image_free(Image *image) {
    if (image->texture) {
        SDL_DestroyTexture(image->texture);
        image->texture = NULL;
    }

    image->width = 0;
    image->height = 0;
}
