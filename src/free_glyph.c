#include <assert.h>
#include <stdbool.h>
#include "./free_glyph.h"
#include "./common.h"

void free_glyph_atlas_init(Free_Glyph_Atlas *atlas, FT_Face face)
{
    atlas->face = face;
    atlas->atlas_width = ATLAS_TEXTURE_SIZE;
    atlas->atlas_height = ATLAS_TEXTURE_SIZE;

    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &atlas->glyphs_texture);
    glBindTexture(GL_TEXTURE_2D, atlas->glyphs_texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RED,
        (GLsizei) atlas->atlas_width,
        (GLsizei) atlas->atlas_height,
        0,
        GL_RED,
        GL_UNSIGNED_BYTE,
        NULL);

    // Pre-warm printable ASCII (including '?', the fallback glyph every
    // on-demand miss degrades to) so the common case has zero first-paint
    // latency; every other codepoint loads the first time it's actually used.
    for (int i = 32; i < 128; ++i) {
        free_glyph_atlas_glyph(atlas, (uint32_t) i);
    }
}

Glyph_Metric free_glyph_atlas_glyph(Free_Glyph_Atlas *atlas, uint32_t codepoint)
{
    for (size_t i = 0; i < atlas->glyphs.count; ++i) {
        if (atlas->glyphs.items[i].codepoint == codepoint) {
            return atlas->glyphs.items[i].metric;
        }
    }

    // Any failure below (font is missing this codepoint, or the atlas
    // texture is exhausted) degrades to the '?' glyph instead of crashing
    // or corrupting the texture - '?' itself failing is the only case
    // that can't recurse further, so that returns an empty metric instead.
    if (FT_Load_Char(atlas->face, codepoint, FT_LOAD_RENDER) ||
            FT_Render_Glyph(atlas->face->glyph, FT_RENDER_MODE_NORMAL)) {
        if (codepoint == '?') {
            Glyph_Metric empty = {0};
            return empty;
        }
        return free_glyph_atlas_glyph(atlas, '?');
    }

    FT_GlyphSlot glyph = atlas->face->glyph;
    FT_UInt bw = glyph->bitmap.width;
    FT_UInt bh = glyph->bitmap.rows;

    if (atlas->pen_x + bw > atlas->atlas_width) {
        atlas->pen_x = 0;
        atlas->pen_y += atlas->row_height;
        atlas->row_height = 0;
    }
    if (atlas->pen_y + bh > atlas->atlas_height) {
        if (codepoint == '?') {
            Glyph_Metric empty = {0};
            return empty;
        }
        return free_glyph_atlas_glyph(atlas, '?');
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        (GLint) atlas->pen_x,
        (GLint) atlas->pen_y,
        (GLsizei) bw,
        (GLsizei) bh,
        GL_RED,
        GL_UNSIGNED_BYTE,
        glyph->bitmap.buffer);

    Glyph_Entry entry = {0};
    entry.codepoint = codepoint;
    entry.metric.ax = glyph->advance.x >> 6;
    entry.metric.ay = glyph->advance.y >> 6;
    entry.metric.bw = (float) bw;
    entry.metric.bh = (float) bh;
    entry.metric.bl = (float) glyph->bitmap_left;
    entry.metric.bt = (float) glyph->bitmap_top;
    entry.metric.tx = (float) atlas->pen_x / (float) atlas->atlas_width;
    entry.metric.ty = (float) atlas->pen_y / (float) atlas->atlas_height;
    da_append(&atlas->glyphs, entry);

    atlas->pen_x += bw;
    if (bh > atlas->row_height) atlas->row_height = bh;

    return entry.metric;
}

float free_glyph_atlas_cursor_pos(Free_Glyph_Atlas *atlas, const char *text, size_t text_size, Vec2f pos, size_t col)
{
    size_t i = 0;
    while (i < text_size) {
        if (i == col) {
            return pos.x;
        }

        uint32_t cp;
        i += utf8_decode(text + i, text_size - i, &cp);

        Glyph_Metric metric = free_glyph_atlas_glyph(atlas, cp);
        pos.x += metric.ax;
        pos.y += metric.ay;
    }

    return pos.x;
}

void free_glyph_atlas_measure_line_sized(Free_Glyph_Atlas *atlas, const char *text, size_t text_size, Vec2f *pos)
{
    size_t i = 0;
    while (i < text_size) {
        uint32_t cp;
        i += utf8_decode(text + i, text_size - i, &cp);

        Glyph_Metric metric = free_glyph_atlas_glyph(atlas, cp);
        pos->x += metric.ax;
        pos->y += metric.ay;
    }
}

void free_glyph_atlas_render_line_sized(Free_Glyph_Atlas *atlas, Simple_Renderer *sr, const char *text, size_t text_size, Vec2f *pos, Vec4f color)
{
    size_t i = 0;
    while (i < text_size) {
        uint32_t cp;
        i += utf8_decode(text + i, text_size - i, &cp);

        Glyph_Metric metric = free_glyph_atlas_glyph(atlas, cp);
        float x2 = pos->x + metric.bl;
        float y2 = -pos->y - metric.bt;
        float w  = metric.bw;
        float h  = metric.bh;

        pos->x += metric.ax;
        pos->y += metric.ay;

        simple_renderer_image_rect(
            sr,
            vec2f(x2, -y2),
            vec2f(w, -h),
            vec2f(metric.tx, metric.ty),
            vec2f(metric.bw / (float) atlas->atlas_width, metric.bh / (float) atlas->atlas_height),
            color);
    }
}
