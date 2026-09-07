#ifndef FREE_GLYPH_H_
#define FREE_GLYPH_H_

#include <stdlib.h>
#include <stdint.h>
#include "./la.h"

#define GLEW_STATIC
#include <GL/glew.h>

#define GL_GLEXT_PROTOTYPES
#include <SDL2/SDL_opengl.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "simple_renderer.h"

#define FREE_GLYPH_FONT_SIZE 64

// https://en.wikibooks.org/wiki/OpenGL_Programming/Modern_OpenGL_Tutorial_Text_Rendering_02

typedef struct {
    float ax; // advance.x
    float ay; // advance.y

    float bw; // bitmap.width;
    float bh; // bitmap.rows;

    float bl; // bitmap_left;
    float bt; // bitmap_top;

    float tx; // x offset of glyph in texture coordinates
    float ty; // y offset of glyph in texture coordinates
} Glyph_Metric;

typedef struct {
    uint32_t codepoint;
    Glyph_Metric metric;
} Glyph_Entry;

typedef struct {
    Glyph_Entry *items;
    size_t count;
    size_t capacity;
} Glyph_Entries;

// Fixed size for the whole session - glyphs are packed into it on demand
// (see free_glyph_atlas_glyph) rather than pre-sized for a known glyph
// set, since UTF-8 text can reference any of Unicode's ~1.1M codepoints.
// Single-channel (GL_RED) at this size is 4MB - trivial, and comfortably
// larger than any realistic editing session's distinct-glyph count.
#define ATLAS_TEXTURE_SIZE 2048

typedef struct {
    FT_Face face;
    FT_UInt atlas_width;
    FT_UInt atlas_height;
    GLuint glyphs_texture;
    Glyph_Entries glyphs;

    // Shelf-packing cursor: glyphs are placed left to right on the
    // current "shelf" (row), wrapping to a new shelf below once a glyph
    // no longer fits - rows can be uneven height, but widths within a
    // row waste nothing (unlike a fixed-cell grid, which would waste a
    // lot of space since glyph widths vary far more than heights at a
    // single font size).
    FT_UInt pen_x, pen_y, row_height;
} Free_Glyph_Atlas;

void free_glyph_atlas_init(Free_Glyph_Atlas *atlas, FT_Face face);

// Returns the glyph metric for `codepoint`, loading and packing it into
// the atlas texture on first use. Always returns a valid metric - an
// unrenderable/unloadable codepoint (missing from the font, or the atlas
// texture is exhausted) falls back to the '?' glyph rather than failing.
// Returned by value (not a pointer) since `glyphs` can reallocate on a
// later call - every caller in this codebase already copies Glyph_Metric
// out immediately rather than holding a pointer to it, so this keeps
// that same discipline enforced by the type itself.
Glyph_Metric free_glyph_atlas_glyph(Free_Glyph_Atlas *atlas, uint32_t codepoint);

float free_glyph_atlas_cursor_pos(Free_Glyph_Atlas *atlas, const char *text, size_t text_size, Vec2f pos, size_t col);
void free_glyph_atlas_measure_line_sized(Free_Glyph_Atlas *atlas, const char *text, size_t text_size, Vec2f *pos);
void free_glyph_atlas_render_line_sized(Free_Glyph_Atlas *atlas, Simple_Renderer *sr, const char *text, size_t text_size, Vec2f *pos, Vec4f color);

#endif // FREE_GLYPH_H_
