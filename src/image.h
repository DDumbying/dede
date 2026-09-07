#ifndef IMAGE_H_
#define IMAGE_H_

#define GLEW_STATIC
#include <GL/glew.h>

#define GL_GLEXT_PROTOTYPES
#include <SDL2/SDL_opengl.h>

#include "./common.h"

typedef struct {
    GLuint texture;
    int width;
    int height;
} Image_Texture;

// Loads `path` (any format SDL2_image supports) into a new GL_RGBA
// texture. Returns nonzero and leaves *img zeroed on failure (missing
// file, decode error) - callers must treat a zeroed texture (texture
// == 0) as "no image available" and skip drawing it, not as fatal.
Errno image_texture_load(Image_Texture *img, const char *path);

#endif // IMAGE_H_
