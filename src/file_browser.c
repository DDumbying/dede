#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "file_browser.h"
#include "sv.h"

static int file_cmp(const void *ap, const void *bp)
{
    const char *a = *(const char**)ap;
    const char *b = *(const char**)bp;
    return strcmp(a, b);
}

void fb_start_search(File_Browser *fb)
{
    if (fb->searching) return;
    fb->searching = true;
    fb->search.count = 0;
}

void fb_stop_search(File_Browser *fb)
{
    fb->searching = false;
    fb->search.count = 0;
}

// Case-insensitive substring match - "type to filter", not a fuzzy
// matcher; every entry matches trivially with no active query.
static bool fb_entry_matches(const File_Browser *fb, const char *name)
{
    if (fb->search.count == 0) return true;

    size_t name_len = strlen(name);
    if (name_len < fb->search.count) return false;

    for (size_t i = 0; i + fb->search.count <= name_len; ++i) {
        size_t j = 0;
        for (; j < fb->search.count; ++j) {
            if (tolower((unsigned char) name[i + j]) != tolower((unsigned char) fb->search.items[j])) break;
        }
        if (j == fb->search.count) return true;
    }
    return false;
}

// The subset of `files` currently shown - all of it with no active
// query. Always a fresh allocation (even with no query) so callers
// never have to guess whether `out->items` aliases fb->files.items
// before freeing it.
static void fb_visible_files(const File_Browser *fb, Files *out)
{
    *out = (Files) {0};
    for (size_t i = 0; i < fb->files.count; ++i) {
        if (fb_entry_matches(fb, fb->files.items[i])) {
            da_append(out, fb->files.items[i]);
        }
    }
}

size_t fb_visible_count(const File_Browser *fb)
{
    if (fb->search.count == 0) return fb->files.count;

    size_t count = 0;
    for (size_t i = 0; i < fb->files.count; ++i) {
        if (fb_entry_matches(fb, fb->files.items[i])) count += 1;
    }
    return count;
}

Errno fb_open_dir(File_Browser *fb, const char *dir_path)
{
    fb->files.count = 0;
    fb->cursor = 0;
    fb_stop_search(fb);
    Errno err = read_entire_dir(dir_path, &fb->files);
    if (err != 0) {
        return err;
    }
    qsort(fb->files.items, fb->files.count, sizeof(*fb->files.items), file_cmp);

    fb->dir_path.count = 0;
    // realpath() gives a real, informative path for display (e.g. the
    // caller's "." resolves to the actual cwd) - fb_change_dir's own
    // normpath() only collapses "." / ".." components textually and
    // wouldn't turn a bare "." into anything more meaningful. Falls back
    // to the raw path verbatim if it doesn't resolve (e.g. permissions).
    char resolved[PATH_MAX];
    if (realpath(dir_path, resolved) != NULL) {
        sb_append_cstr(&fb->dir_path, resolved);
    } else {
        sb_append_cstr(&fb->dir_path, dir_path);
    }
    sb_append_null(&fb->dir_path);

    return 0;
}

#define PATH_SEP "/"
#define PATH_EMPTY ""
#define PATH_DOT "."
#define PATH_DOTDOT ".."

typedef struct {
    String_View *items;
    size_t count;
    size_t capacity;
} Comps;

void normpath(String_View path, String_Builder *result)
{
    size_t original_sb_size = result->count;

    if (path.count == 0) {
        sb_append_cstr(result, PATH_DOT);
        return;
    }

    int initial_slashes = 0;
    while (path.count > 0 && *path.data == *PATH_SEP) {
        initial_slashes += 1;
        sv_chop_left(&path, 1);
    }
    if (initial_slashes > 2) {
        initial_slashes = 1;
    }

    Comps new_comps = {0};

    while (path.count > 0) {
        String_View comp = sv_chop_by_delim(&path, '/');
        if (comp.count == 0 || sv_eq(comp, SV(PATH_DOT))) {
            continue;
        }
        if (!sv_eq(comp, SV(PATH_DOTDOT))) {
            da_append(&new_comps, comp);
            continue;
        }
        if (initial_slashes == 0 && new_comps.count == 0) {
            da_append(&new_comps, comp);
            continue;
        }
        if (new_comps.count > 0 && sv_eq(da_last(&new_comps), SV(PATH_DOTDOT))) {
            da_append(&new_comps, comp);
            continue;
        }
        if (new_comps.count > 0) {
            new_comps.count -= 1;
            continue;
        }
    }

    for (int i = 0; i < initial_slashes; ++i) {
        sb_append_cstr(result, PATH_SEP);
    }

    for (size_t i = 0; i < new_comps.count; ++i) {
        if (i > 0) sb_append_cstr(result, PATH_SEP);
        sb_append_buf(result, new_comps.items[i].data, new_comps.items[i].count);
    }

    if (original_sb_size == result->count) {
        sb_append_cstr(result, PATH_DOT);
    }

    free(new_comps.items);
}

Errno fb_change_dir(File_Browser *fb)
{
    assert(fb->dir_path.count > 0 && "You need to call fb_open_dir() before fb_change_dir()");
    assert(fb->dir_path.items[fb->dir_path.count - 1] == '\0');

    Files visible = {0};
    fb_visible_files(fb, &visible);
    if (fb->cursor >= visible.count) {
        free(visible.items);
        return 0;
    }
    const char *dir_name = visible.items[fb->cursor];
    free(visible.items);

    fb->dir_path.count -= 1;

    // TODO: fb->dir_path grows indefinitely if we hit the root
    sb_append_cstr(&fb->dir_path, "/");
    sb_append_cstr(&fb->dir_path, dir_name);

    String_Builder result = {0};
    normpath(sb_to_sv(fb->dir_path), &result);
    da_move(&fb->dir_path, result);
    sb_append_null(&fb->dir_path);

    printf("Changed dir to %s\n", fb->dir_path.items);

    fb->files.count = 0;
    fb->cursor = 0;
    fb_stop_search(fb);
    Errno err = read_entire_dir(fb->dir_path.items, &fb->files);

    if (err != 0) {
        return err;
    }
    qsort(fb->files.items, fb->files.count, sizeof(*fb->files.items), file_cmp);

    return 0;
}

void fb_render(const File_Browser *fb, SDL_Window *window, Free_Glyph_Atlas *atlas, Simple_Renderer *sr)
{
    Files visible = {0};
    fb_visible_files(fb, &visible);

    Vec2f cursor_pos = vec2f(0, -(float)fb->cursor * FREE_GLYPH_FONT_SIZE);

    int w, h;
    SDL_GetWindowSize(window, &w, &h);

    float max_line_len = 0.0f;

    sr->resolution = vec2f(w, h);
    sr->time = (float) SDL_GetTicks() / 1000.0f;

    simple_renderer_set_shader(sr, SHADER_FOR_COLOR);
    if (fb->cursor < visible.count) {
        const Vec2f begin = vec2f(0, -((float)fb->cursor + CURSOR_OFFSET) * FREE_GLYPH_FONT_SIZE);
        Vec2f end = begin;
        free_glyph_atlas_measure_line_sized(
            atlas, visible.items[fb->cursor], strlen(visible.items[fb->cursor]),
            &end);
        simple_renderer_solid_rect(sr, begin, vec2f(end.x - begin.x, FREE_GLYPH_FONT_SIZE), vec4f(.25, .25, .25, 1));
    }
    simple_renderer_flush(sr);

    simple_renderer_set_shader(sr, SHADER_FOR_EPICNESS);
    for (size_t row = 0; row < visible.count; ++row) {
        const Vec2f begin = vec2f(0, -(float)row * FREE_GLYPH_FONT_SIZE);
        Vec2f end = begin;
        free_glyph_atlas_render_line_sized(
            atlas, sr, visible.items[row], strlen(visible.items[row]),
            &end,
            vec4fs(0));
        // TODO: the max_line_len should be calculated based on what's visible on the screen right now
        float line_len = fabsf(end.x - begin.x);
        if (line_len > max_line_len) {
            max_line_len = line_len;
        }
    }

    simple_renderer_flush(sr);
    free(visible.items);

    // Update camera
    {
        if (max_line_len > 1000.0f) {
            max_line_len = 1000.0f;
        }

        float target_scale = w/3/(max_line_len*0.75); // TODO: division by 0

        Vec2f target = cursor_pos;
        float offset = 0.0f;

        if (target_scale > 3.0f) {
            target_scale = 3.0f;
        } else {
            offset = cursor_pos.x - w/3/sr->camera_scale;
            if (offset < 0.0f) offset = 0.0f;
            target = vec2f(w/3/sr->camera_scale + offset, cursor_pos.y);
        }

        sr->camera_vel = vec2f_mul(
                             vec2f_sub(target, sr->camera_pos),
                             vec2fs(2.0f));
        sr->camera_scale_vel = (target_scale - sr->camera_scale) * 2.0f;

        sr->camera_pos = vec2f_add(sr->camera_pos, vec2f_mul(sr->camera_vel, vec2fs(DELTA_TIME)));
        sr->camera_scale = sr->camera_scale + sr->camera_scale_vel * DELTA_TIME;
    }
}

const char *fb_file_path(File_Browser *fb)
{
    assert(fb->dir_path.count > 0 && "You need to call fb_open_dir() before fb_file_path()");
    assert(fb->dir_path.items[fb->dir_path.count - 1] == '\0');

    Files visible = {0};
    fb_visible_files(fb, &visible);
    if (fb->cursor >= visible.count) {
        free(visible.items);
        return NULL;
    }

    fb->file_path.count = 0;
    sb_append_buf(&fb->file_path, fb->dir_path.items, fb->dir_path.count - 1);
    sb_append_buf(&fb->file_path, "/", 1);
    sb_append_cstr(&fb->file_path, visible.items[fb->cursor]);
    sb_append_null(&fb->file_path);
    free(visible.items);

    return fb->file_path.items;
}
