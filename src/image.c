// NOTE: only build.sh wires up the SDL2_image dependency this needs.
// build_msvc.bat/build_msys2_mingw64.sh don't vendor/link SDL2_image yet,
// so the splash screen's logo (the only current user of this file) won't
// build there until someone adds it - a known, documented gap, not an
// oversight.

#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include "./image.h"

Errno image_texture_load(Image_Texture *img, const char *path)
{
    memset(img, 0, sizeof(*img));

    SDL_Surface *surface = IMG_Load(path);
    if (surface == NULL) {
        fprintf(stderr, "WARNING: could not load image %s: %s\n", path, IMG_GetError());
        return 1;
    }

    SDL_Surface *converted = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(surface);
    if (converted == NULL) {
        fprintf(stderr, "WARNING: could not convert image %s: %s\n", path, SDL_GetError());
        return 1;
    }

    glGenTextures(1, &img->texture);
    glBindTexture(GL_TEXTURE_2D, img->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        converted->w,
        converted->h,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        converted->pixels);

    img->width = converted->w;
    img->height = converted->h;
    SDL_FreeSurface(converted);
    return 0;
}
