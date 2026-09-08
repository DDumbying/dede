#ifndef FILE_BROWSER_H_
#define FILE_BROWSER_H_

#include "common.h"
#include "free_glyph.h"

#include <SDL2/SDL.h>

typedef struct {
    Files files;
    size_t cursor; // indexes the currently *visible* list - see fb_visible_count/fb_file_path, not necessarily `files` itself while searching
    String_Builder dir_path;
    String_Builder file_path;

    // Incremental "type to filter" search over `files` - same
    // searching/search shape as Editor's own (see editor.h), just
    // filtering the listing instead of highlighting matches in text.
    bool searching;
    String_Builder search;
} File_Browser;

Errno fb_open_dir(File_Browser *fb, const char *dir_path);
Errno fb_change_dir(File_Browser *fb);
void fb_render(const File_Browser *fb, SDL_Window *window, Free_Glyph_Atlas *atlas, Simple_Renderer *sr);
const char *fb_file_path(File_Browser *fb);

void fb_start_search(File_Browser *fb);
void fb_stop_search(File_Browser *fb);

// How many of `files` currently match `search` (or fb->files.count with
// no active query) - `fb->cursor`'s upper bound. Cheap bounds-check-only
// alternative to building the full filtered list.
size_t fb_visible_count(const File_Browser *fb);

#endif // FILE_BROWSER_H_
