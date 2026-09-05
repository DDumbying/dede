#ifndef EDITOR_H_
#define EDITOR_H_

#include <stdlib.h>
#include "common.h"
#include "free_glyph.h"
#include "simple_renderer.h"
#include "lexer.h"

#include <SDL2/SDL.h>

typedef struct {
    size_t begin;
    size_t end;
} Line;

typedef struct {
    Line *items;
    size_t count;
    size_t capacity;
} Lines;

typedef struct {
    Token *items;
    size_t count;
    size_t capacity;
} Tokens;

typedef enum {
    EDIT_INSERT,
    EDIT_DELETE,
} Edit_Kind;

typedef struct {
    Edit_Kind kind;
    size_t pos;
    String_Builder text;
} Edit_Op;

typedef struct {
    Edit_Op *items;
    size_t count;
    size_t capacity;
} Edit_Ops;

typedef struct {
    Edit_Ops *items;
    size_t count;
    size_t capacity;
} Edit_History;

// Hard cap on Editor::indent_width - not a "reasonable default", just
// the size of the on-stack scratch buffers editor_indent/editor_unindent
// use to avoid a heap allocation for something this small. config_load
// clamps into this range; editor_new_file/editor_load_from_file don't
// touch indent_width at all (it's a user setting, not per-buffer state).
#define EDITOR_MAX_INDENT_WIDTH 16

typedef struct {
    Free_Glyph_Atlas *atlas;

    String_Builder data;
    Lines lines;
    Tokens tokens;
    String_Builder file_path;

    bool searching;
    String_Builder search;

    bool selection;
    size_t select_begin;
    size_t cursor;

    Uint32 last_stroke;

    String_Builder clipboard;

    Edit_History undo_stack;
    Edit_History redo_stack;
    Edit_Ops current_group;
    bool group_open;
    Uint32 group_time;

    // Purely a rendering hint set by whatever owns modal state (see
    // vim.h) - the editor engine itself has no concept of Vim modes.
    // true -> draw a full-width block cursor (Normal mode convention),
    // false -> draw the thin insertion bar (current/Insert behavior).
    bool cursor_block;

    // True from the moment the buffer diverges from what's on disk (or
    // from a blank new buffer) until the next successful save. Set in
    // one place - editor_add_op, the funnel every content mutation
    // (insert/delete/indent/dedent) already goes through - plus
    // editor_undo/editor_redo, which bypass that funnel by design (they
    // replay recorded ops directly) but still change the buffer. Cleared
    // by editor_save/editor_save_as on success, editor_load_from_file,
    // and editor_new_file. The caller (main.c) is expected to check this
    // before destructive actions (opening another file, quitting) and
    // confirm with the user first.
    bool dirty;

    // How many spaces Tab/editor_indent inserts (and Shift-Tab/
    // editor_unindent removes up to). A user setting, not per-buffer
    // state - editor_new_file/editor_load_from_file leave it alone.
    // Zero-initializing an Editor ({0}) leaves this 0, which makes Tab a
    // silent no-op rather than a crash - harmless, but not the intended
    // default of 4, so app.c sets this explicitly at startup from
    // Config (see config.h). Clamped to EDITOR_MAX_INDENT_WIDTH.
    size_t indent_width;

    // User settings from Config, set once at startup. Not read by
    // editor_render yet - the gutter itself is a follow-up.
    bool line_numbers;
    bool relative_line_numbers;
} Editor;

Errno editor_save_as(Editor *editor, const char *file_path);
Errno editor_save(const Editor *editor);
Errno editor_load_from_file(Editor *editor, const char *file_path);
void editor_new_file(Editor *editor);

void editor_backspace(Editor *editor);
void editor_delete(Editor *editor);
size_t editor_cursor_row(const Editor *e);
size_t editor_row_at(const Editor *e, size_t pos);

// Pure motions: (buffer, pos) -> pos. No side effects, do not touch
// e->cursor or e->searching. Shared by cursor movement, future
// operator-pending ranges, and visual-mode selection extension.
size_t editor_find_line_up(const Editor *e, size_t pos);
size_t editor_find_line_down(const Editor *e, size_t pos);
size_t editor_find_char_left(const Editor *e, size_t pos);
size_t editor_find_char_right(const Editor *e, size_t pos);
size_t editor_find_word_left(const Editor *e, size_t pos);
size_t editor_find_word_right(const Editor *e, size_t pos);
size_t editor_find_line_begin(const Editor *e, size_t pos);
size_t editor_find_line_end(const Editor *e, size_t pos);
size_t editor_find_paragraph_up(const Editor *e, size_t pos);
size_t editor_find_paragraph_down(const Editor *e, size_t pos);
size_t editor_find_word_end(const Editor *e, size_t pos);

// Cursor mutators: move e->cursor via the pure motions above, and stop
// any active incremental search (existing behavior, unchanged).
void editor_move_line_up(Editor *e);
void editor_move_line_down(Editor *e);
void editor_move_char_left(Editor *e);
void editor_move_char_right(Editor *e);
void editor_move_word_left(Editor *e);
void editor_move_word_right(Editor *e);

void editor_move_to_begin(Editor *e);
void editor_move_to_end(Editor *e);
void editor_move_to_line_begin(Editor *e);
void editor_move_to_line_end(Editor *e);

void editor_move_paragraph_up(Editor *e);
void editor_move_paragraph_down(Editor *e);

void editor_indent(Editor *e);
void editor_unindent(Editor *e);

void editor_insert_char(Editor *e, char x);
void editor_insert_buf(Editor *e, char *buf, size_t buf_len);
void editor_retokenize(Editor *e);
void editor_undo(Editor *e);
void editor_redo(Editor *e);
void editor_flush_group(Editor *e);
void editor_render(SDL_Window *window, Free_Glyph_Atlas *atlas, Simple_Renderer *sr, Editor *editor);
void editor_update_selection(Editor *e, bool shift);
void editor_clipboard_copy(Editor *e);
void editor_clipboard_cut(Editor *e);
void editor_clipboard_paste(Editor *e);
void editor_start_search(Editor *e);
void editor_stop_search(Editor *e);
bool editor_search_matches_at(Editor *e, size_t pos);

#endif // EDITOR_H_
