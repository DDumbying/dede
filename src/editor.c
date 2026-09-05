#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include "./editor.h"
#include "./common.h"

#define UNDO_MERGE_MS 500

static void editor_data_insert_at(Editor *e, size_t pos, const char *buf, size_t buf_len)
{
    if (pos > e->data.count) {
        pos = e->data.count;
    }

    for (size_t i = 0; i < buf_len; ++i) {
        da_append(&e->data, '\0');
    }
    memmove(
        &e->data.items[pos + buf_len],
        &e->data.items[pos],
        e->data.count - pos - buf_len
    );
    memcpy(&e->data.items[pos], buf, buf_len);
}

static void editor_data_insert(Editor *e, const char *buf, size_t buf_len)
{
    editor_data_insert_at(e, e->cursor, buf, buf_len);
    e->cursor += buf_len;
}

static void editor_data_remove(Editor *e, size_t pos, size_t len)
{
    if (pos > e->data.count) {
        pos = e->data.count;
    }
    if (pos + len > e->data.count) {
        len = e->data.count - pos;
    }

    memmove(
        &e->data.items[pos],
        &e->data.items[pos + len],
        e->data.count - pos - len
    );
    e->data.count -= len;
}

static void editor_clear_redo(Editor *e)
{
    for (size_t i = 0; i < e->redo_stack.count; ++i) {
        for (size_t j = 0; j < e->redo_stack.items[i].count; ++j) {
            free(e->redo_stack.items[i].items[j].text.items);
        }
        free(e->redo_stack.items[i].items);
    }
    free(e->redo_stack.items);
    e->redo_stack = (Edit_History){0};
}

void editor_flush_group(Editor *e)
{
    if (!e->group_open) return;
    e->group_open = false;
    e->group_time = 0;

    da_append(&e->undo_stack, e->current_group);
    e->current_group = (Edit_Ops){0};
}

static void editor_add_op(Editor *e, Edit_Kind kind, size_t pos, const char *text, size_t text_len)
{
    Uint32 now = SDL_GetTicks();
    if (e->group_open && now - e->group_time > UNDO_MERGE_MS) {
        editor_flush_group(e);
    }
    e->group_open = true;
    e->group_time = now;
    e->dirty = true;

    Edit_Op op = {
        .kind = kind,
        .pos = pos,
        .text = {0},
    };
    sb_append_buf(&op.text, text, text_len);
    da_append(&e->current_group, op);

    editor_clear_redo(e);
}

static void editor_delete_selection(Editor *e)
{
    if (!e->selection) return;

    size_t begin = e->select_begin;
    size_t end = e->cursor;
    if (begin > end) SWAP(size_t, begin, end);

    e->selection = false;

    if (begin == end) return;

    size_t len = end - begin;
    String_Builder sel_text = {0};
    sb_append_buf(&sel_text, e->data.items + begin, len);

    e->cursor = begin;
    editor_data_remove(e, begin, len);
    editor_add_op(e, EDIT_DELETE, begin, sel_text.items, sel_text.count);
    free(sel_text.items);

    editor_retokenize(e);
}

static void editor_apply_undo(Editor *e, Edit_Op *op)
{
    switch (op->kind) {
    case EDIT_INSERT:
        e->cursor = op->pos;
        editor_data_remove(e, op->pos, op->text.count);
        break;
    case EDIT_DELETE:
        e->cursor = op->pos;
        editor_data_insert(e, op->text.items, op->text.count);
        break;
    }
}

static void editor_apply_redo(Editor *e, Edit_Op *op)
{
    switch (op->kind) {
    case EDIT_INSERT:
        e->cursor = op->pos;
        editor_data_insert(e, op->text.items, op->text.count);
        break;
    case EDIT_DELETE:
        e->cursor = op->pos;
        editor_data_remove(e, op->pos, op->text.count);
        break;
    }
}

void editor_undo(Editor *e)
{
    editor_stop_search(e);
    editor_flush_group(e);
    if (e->undo_stack.count == 0) return;

    Edit_Ops group = da_last(&e->undo_stack);
    e->undo_stack.count -= 1;
    da_append(&e->redo_stack, group);

    e->selection = false;
    for (size_t i = group.count; i > 0; --i) {
        editor_apply_undo(e, &group.items[i - 1]);
    }

    // Bypasses editor_add_op (it's replaying recorded ops, not creating
    // new ones), so it must set this itself: undoing back to an earlier
    // state still leaves the buffer different from what's on disk unless
    // that earlier state happens to be the last-saved one, which isn't
    // worth tracking separately - better to over-warn on save/quit than
    // silently let a discard slip through.
    e->dirty = true;

    editor_retokenize(e);
    e->last_stroke = SDL_GetTicks();
}

void editor_redo(Editor *e)
{
    editor_stop_search(e);
    editor_flush_group(e);
    if (e->redo_stack.count == 0) return;

    Edit_Ops group = da_last(&e->redo_stack);
    e->redo_stack.count -= 1;
    da_append(&e->undo_stack, group);

    e->selection = false;
    for (size_t i = 0; i < group.count; ++i) {
        editor_apply_redo(e, &group.items[i]);
    }
    e->dirty = true; // see editor_undo's comment on the same line

    editor_retokenize(e);
    e->last_stroke = SDL_GetTicks();
}

static void editor_free_history(Editor *e)
{
    for (size_t i = 0; i < e->undo_stack.count; ++i) {
        for (size_t j = 0; j < e->undo_stack.items[i].count; ++j) {
            free(e->undo_stack.items[i].items[j].text.items);
        }
        free(e->undo_stack.items[i].items);
    }
    free(e->undo_stack.items);

    for (size_t i = 0; i < e->redo_stack.count; ++i) {
        for (size_t j = 0; j < e->redo_stack.items[i].count; ++j) {
            free(e->redo_stack.items[i].items[j].text.items);
        }
        free(e->redo_stack.items[i].items);
    }
    free(e->redo_stack.items);

    for (size_t j = 0; j < e->current_group.count; ++j) {
        free(e->current_group.items[j].text.items);
    }
    free(e->current_group.items);

    e->undo_stack = (Edit_History){0};
    e->redo_stack = (Edit_History){0};
    e->current_group = (Edit_Ops){0};
    e->group_open = false;
    e->group_time = 0;
}

void editor_backspace(Editor *e)
{
    if (e->searching) {
        if (e->search.count > 0) {
            e->search.count -= 1;
        }
    } else {
        if (e->cursor > e->data.count) {
            e->cursor = e->data.count;
        }
        if (e->cursor == 0) return;

        if (e->selection) {
            editor_delete_selection(e);
            return;
        }

        size_t pos = e->cursor - 1;
        char c = e->data.items[pos];
        e->cursor = pos;
        editor_data_remove(e, pos, 1);
        editor_add_op(e, EDIT_DELETE, pos, &c, 1);
        editor_retokenize(e);
    }
}

void editor_delete(Editor *e)
{
    if (e->searching) return;

    if (e->selection) {
        editor_delete_selection(e);
        return;
    }

    if (e->cursor >= e->data.count) return;
    char c = e->data.items[e->cursor];
    editor_data_remove(e, e->cursor, 1);
    editor_add_op(e, EDIT_DELETE, e->cursor, &c, 1);
    editor_retokenize(e);
}

// TODO: make sure that you always have new line at the end of the file while saving
// https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap03.html#tag_03_206

Errno editor_save_as(Editor *e, const char *file_path)
{
    printf("Saving as %s...\n", file_path);
    Errno err = write_entire_file(file_path, e->data.items, e->data.count);
    if (err != 0) return err;
    e->file_path.count = 0;
    sb_append_cstr(&e->file_path, file_path);
    sb_append_null(&e->file_path);
    e->dirty = false;
    return 0;
}

Errno editor_save(const Editor *e)
{
    assert(e->file_path.count > 0);
    printf("Saving as %s...\n", e->file_path.items);
    return write_entire_file(e->file_path.items, e->data.items, e->data.count);
}

Errno editor_load_from_file(Editor *e, const char *file_path)
{
    printf("Loading %s\n", file_path);

    editor_free_history(e);

    e->data.count = 0;
    Errno err = read_entire_file(file_path, &e->data);
    if (err != 0) return err;

    e->cursor = 0;

    editor_retokenize(e);

    e->file_path.count = 0;
    sb_append_cstr(&e->file_path, file_path);
    sb_append_null(&e->file_path);
    e->dirty = false;

    return 0;
}

// Resets the editor to a blank, unsaved buffer with no file backing it
// yet (Ctrl+N). Mirrors editor_load_from_file's reset of history/cursor/
// selection/search, minus the actual file read.
void editor_new_file(Editor *e)
{
    editor_free_history(e);
    editor_stop_search(e);

    e->data.count = 0;
    e->cursor = 0;
    e->selection = false;
    e->file_path.count = 0;
    e->dirty = false;

    editor_retokenize(e);
}

size_t editor_row_at(const Editor *e, size_t pos)
{
    assert(e->lines.count > 0);
    for (size_t row = 0; row < e->lines.count; ++row) {
        Line line = e->lines.items[row];
        if (line.begin <= pos && pos <= line.end) {
            return row;
        }
    }
    return e->lines.count - 1;
}

size_t editor_cursor_row(const Editor *e)
{
    return editor_row_at(e, e->cursor);
}

// ------------------------------------------------------------------------
// Pure motions
//
// Each of these computes a target buffer position from an arbitrary
// starting position `pos`. They never touch `e->cursor`, never call
// `editor_stop_search`, and have no other side effects - they are plain
// functions of (buffer, pos) -> pos.
//
// This is the shared core that normal-mode cursor movement, future
// operator-pending ranges (e.g. `dw`, `ciw`), and visual-mode selection
// extension all build on: one implementation, several consumers, instead
// of the motion logic being welded to `e->cursor` mutation.
// ------------------------------------------------------------------------

size_t editor_find_line_up(const Editor *e, size_t pos)
{
    size_t row = editor_row_at(e, pos);
    if (row == 0) return pos;
    size_t col = pos - e->lines.items[row].begin;
    Line prev_line = e->lines.items[row - 1];
    size_t prev_line_size = prev_line.end - prev_line.begin;
    if (col > prev_line_size) col = prev_line_size;
    return prev_line.begin + col;
}

size_t editor_find_line_down(const Editor *e, size_t pos)
{
    size_t row = editor_row_at(e, pos);
    if (row + 1 >= e->lines.count) return pos;
    size_t col = pos - e->lines.items[row].begin;
    Line next_line = e->lines.items[row + 1];
    size_t next_line_size = next_line.end - next_line.begin;
    if (col > next_line_size) col = next_line_size;
    return next_line.begin + col;
}

size_t editor_find_char_left(const Editor *e, size_t pos)
{
    (void) e;
    return pos > 0 ? pos - 1 : pos;
}

size_t editor_find_char_right(const Editor *e, size_t pos)
{
    return pos < e->data.count ? pos + 1 : pos;
}

size_t editor_find_word_left(const Editor *e, size_t pos)
{
    while (pos > 0 && !isalnum((unsigned char) e->data.items[pos - 1])) {
        pos -= 1;
    }
    while (pos > 0 && isalnum((unsigned char) e->data.items[pos - 1])) {
        pos -= 1;
    }
    return pos;
}

size_t editor_find_word_right(const Editor *e, size_t pos)
{
    while (pos < e->data.count && !isalnum((unsigned char) e->data.items[pos])) {
        pos += 1;
    }
    while (pos < e->data.count && isalnum((unsigned char) e->data.items[pos])) {
        pos += 1;
    }
    return pos;
}

// Vim's `e`: land ON the last character of the current/next word, as
// opposed to `w`'s "start of the next word". Uses the same word
// definition as word_left/word_right (maximal alnum run; everything
// else is a separator) - this codebase does not distinguish punctuation
// runs from word runs the way real Vim does, so `e` is an approximation
// consistent with the rest of the file, not a full Vim word model.
size_t editor_find_word_end(const Editor *e, size_t pos)
{
    size_t count = e->data.count;
    if (count == 0) return pos;

    // Always step forward at least once so repeated `e` presses advance
    // instead of getting stuck once the cursor already sits on a word end.
    if (pos + 1 >= count) return pos;
    pos += 1;

    while (pos < count && !isalnum((unsigned char) e->data.items[pos])) {
        pos += 1;
    }
    if (pos >= count) return count - 1;

    while (pos + 1 < count && isalnum((unsigned char) e->data.items[pos + 1])) {
        pos += 1;
    }
    return pos;
}

size_t editor_find_line_begin(const Editor *e, size_t pos)
{
    size_t row = editor_row_at(e, pos);
    return e->lines.items[row].begin;
}

size_t editor_find_line_end(const Editor *e, size_t pos)
{
    size_t row = editor_row_at(e, pos);
    return e->lines.items[row].end;
}

size_t editor_find_paragraph_up(const Editor *e, size_t pos)
{
    size_t row = editor_row_at(e, pos);
    while (row > 0 && e->lines.items[row].end - e->lines.items[row].begin <= 1) {
        row -= 1;
    }
    while (row > 0 && e->lines.items[row].end - e->lines.items[row].begin > 1) {
        row -= 1;
    }
    return e->lines.items[row].begin;
}

size_t editor_find_paragraph_down(const Editor *e, size_t pos)
{
    size_t row = editor_row_at(e, pos);
    while (row + 1 < e->lines.count && e->lines.items[row].end - e->lines.items[row].begin <= 1) {
        row += 1;
    }
    while (row + 1 < e->lines.count && e->lines.items[row].end - e->lines.items[row].begin > 1) {
        row += 1;
    }
    return e->lines.items[row].begin;
}

// ------------------------------------------------------------------------
// Cursor mutators
//
// Same observable behavior as before the refactor - they still stop an
// active search and move `e->cursor` - just expressed as pure motion +
// assignment so the pure motion can be reused elsewhere.
// ------------------------------------------------------------------------

void editor_move_line_up(Editor *e)
{
    editor_stop_search(e);
    e->cursor = editor_find_line_up(e, e->cursor);
}

void editor_move_line_down(Editor *e)
{
    editor_stop_search(e);
    e->cursor = editor_find_line_down(e, e->cursor);
}

void editor_move_char_left(Editor *e)
{
    editor_stop_search(e);
    e->cursor = editor_find_char_left(e, e->cursor);
}

void editor_move_char_right(Editor *e)
{
    editor_stop_search(e);
    e->cursor = editor_find_char_right(e, e->cursor);
}

void editor_move_word_left(Editor *e)
{
    editor_stop_search(e);
    e->cursor = editor_find_word_left(e, e->cursor);
}

void editor_move_word_right(Editor *e)
{
    editor_stop_search(e);
    e->cursor = editor_find_word_right(e, e->cursor);
}

void editor_insert_char(Editor *e, char x)
{
    editor_insert_buf(e, &x, 1);
}

void editor_insert_buf(Editor *e, char *buf, size_t buf_len)
{
    if (e->searching) {
        sb_append_buf(&e->search, buf, buf_len);
        bool matched = false;
        for (size_t pos = e->cursor; pos < e->data.count; ++pos) {
            if (editor_search_matches_at(e, pos)) {
                e->cursor = pos;
                matched = true;
                break;
            }
        }
        if (!matched) e->search.count -= buf_len;
    } else {
        if (e->cursor > e->data.count) {
            e->cursor = e->data.count;
        }
        if (e->selection) {
            editor_delete_selection(e);
        }

        size_t pos = e->cursor;
        editor_data_insert(e, buf, buf_len);
        editor_add_op(e, EDIT_INSERT, pos, buf, buf_len);
        editor_retokenize(e);
    }
}

static size_t editor_row_of(const Editor *e, size_t pos)
{
    assert(e->lines.count > 0);
    for (size_t row = 0; row < e->lines.count; ++row) {
        Line line = e->lines.items[row];
        if (line.begin <= pos && pos <= line.end) {
            return row;
        }
    }
    return e->lines.count - 1;
}

// Maps a pre-edit position to its position after every selected row `row_begin..row_end`
// gained `deltas[r]` bytes (negative when bytes were removed). Works only while
// `e->lines` still reflects the pre-edit layout.
static size_t editor_apply_deltas(Editor *e, size_t pos, size_t row_begin, size_t row_end, const ptrdiff_t *deltas)
{
    ptrdiff_t shift = 0;
    for (size_t r = row_begin; r <= row_end; ++r) {
        if (e->lines.items[r].begin <= pos) {
            shift += deltas[r];
        }
    }
    return (size_t) ((ptrdiff_t) pos + shift);
}

// Clamped, non-zero indent width for this call - editor_indent's own
// stack buffer is sized to EDITOR_MAX_INDENT_WIDTH, and a width of 0
// would make Tab a silent no-op (see Editor::indent_width's comment).
static size_t editor_clamped_indent_width(const Editor *e)
{
    size_t width = e->indent_width;
    if (width == 0) width = 1;
    if (width > EDITOR_MAX_INDENT_WIDTH) width = EDITOR_MAX_INDENT_WIDTH;
    return width;
}

void editor_indent(Editor *e)
{
    if (e->searching) return;

    char indent[EDITOR_MAX_INDENT_WIDTH];
    size_t width = editor_clamped_indent_width(e);
    memset(indent, ' ', width);

    if (!e->selection) {
        editor_insert_buf(e, indent, width);
        return;
    }

    size_t begin = e->select_begin;
    size_t end = e->cursor;
    if (begin > end) SWAP(size_t, begin, end);

    if (begin == end) {
        e->selection = false;
        editor_insert_buf(e, indent, width);
        return;
    }

    size_t row_begin = editor_row_of(e, begin);
    size_t row_end = editor_row_of(e, end);
    if (row_end > row_begin && e->lines.items[row_end].begin == end) {
        row_end -= 1; // the selection ends exactly where a line starts
    }

    ptrdiff_t *deltas = calloc(e->lines.count, sizeof(*deltas));

    // Insert bottom-up so the shorter (upper) lines keep valid positions.
    for (size_t r = row_end + 1; r > row_begin; --r) {
        size_t rr = r - 1;
        size_t at = e->lines.items[rr].begin;
        editor_data_insert_at(e, at, indent, width);
        editor_add_op(e, EDIT_INSERT, at, indent, width);
        deltas[rr] = (ptrdiff_t) width;
    }

    e->select_begin = editor_apply_deltas(e, begin, row_begin, row_end, deltas);
    e->cursor = editor_apply_deltas(e, end, row_begin, row_end, deltas);
    free(deltas);

    editor_retokenize(e);
    e->last_stroke = SDL_GetTicks();
}

void editor_unindent(Editor *e)
{
    if (e->searching) return;

    size_t width = editor_clamped_indent_width(e);

    if (!e->selection) {
        size_t row = editor_row_of(e, e->cursor);
        size_t at = e->lines.items[row].begin;
        size_t end = e->lines.items[row].end;

        size_t remove = 0;
        while (remove < width && at + remove < end && e->data.items[at + remove] == ' ') {
            remove += 1;
        }
        if (remove == 0) return;

        char spaces[EDITOR_MAX_INDENT_WIDTH] = {0};
        memcpy(spaces, e->data.items + at, remove);
        editor_data_remove(e, at, remove);
        editor_add_op(e, EDIT_DELETE, at, spaces, remove);
        if (e->cursor >= at + remove) {
            e->cursor -= remove;
        } else {
            e->cursor = at;
        }
        editor_retokenize(e);
        e->last_stroke = SDL_GetTicks();
        return;
    }

    size_t begin = e->select_begin;
    size_t end = e->cursor;
    if (begin > end) SWAP(size_t, begin, end);

    if (begin == end) {
        e->selection = false;
        editor_unindent(e);
        return;
    }

    size_t row_begin = editor_row_of(e, begin);
    size_t row_end = editor_row_of(e, end);
    if (row_end > row_begin && e->lines.items[row_end].begin == end) {
        row_end -= 1;
    }

    ptrdiff_t *deltas = calloc(e->lines.count, sizeof(*deltas));

    // Remove bottom-up so the shorter (upper) lines keep valid positions.
    char spaces[EDITOR_MAX_INDENT_WIDTH] = {0};
    for (size_t r = row_end + 1; r > row_begin; --r) {
        size_t rr = r - 1;
        size_t at = e->lines.items[rr].begin;
        size_t line_end = e->lines.items[rr].end;

        size_t remove = 0;
        while (remove < width && at + remove < line_end && e->data.items[at + remove] == ' ') {
            remove += 1;
        }
        if (remove == 0) continue;

        memcpy(spaces, e->data.items + at, remove);
        editor_data_remove(e, at, remove);
        editor_add_op(e, EDIT_DELETE, at, spaces, remove);
        deltas[rr] = -((ptrdiff_t) remove);
    }

    e->select_begin = editor_apply_deltas(e, begin, row_begin, row_end, deltas);
    e->cursor = editor_apply_deltas(e, end, row_begin, row_end, deltas);
    free(deltas);

    editor_retokenize(e);
    e->last_stroke = SDL_GetTicks();
}

void editor_retokenize(Editor *e)
{
    // Lines
    {
        e->lines.count = 0;

        Line line;
        line.begin = 0;

        for (size_t i = 0; i < e->data.count; ++i) {
            if (e->data.items[i] == '\n') {
                line.end = i;
                da_append(&e->lines, line);
                line.begin = i + 1;
            }
        }

        line.end = e->data.count;
        da_append(&e->lines, line);
    }

    // Syntax Highlighting
    {
        e->tokens.count = 0;
        Lexer l = lexer_new(e->atlas, e->data.items, e->data.count);
        Token t = lexer_next(&l);
        while (t.kind != TOKEN_END) {
            da_append(&e->tokens, t);
            t = lexer_next(&l);
        }
    }
}

bool editor_line_starts_with(Editor *e, size_t row, size_t col, const char *prefix)
{
    size_t prefix_len = strlen(prefix);
    if (prefix_len == 0) {
        return true;
    }
    Line line = e->lines.items[row];
    if (col + prefix_len - 1 >= line.end) {
        return false;
    }
    for (size_t i = 0; i < prefix_len; ++i) {
        if (prefix[i] != e->data.items[line.begin + col + i]) {
            return false;
        }
    }
    return true;
}

const char *editor_line_starts_with_one_of(Editor *e, size_t row, size_t col, const char **prefixes, size_t prefixes_count)
{
    for (size_t i = 0; i < prefixes_count; ++i) {
        if (editor_line_starts_with(e, row, col, prefixes[i])) {
            return prefixes[i];
        }
    }
    return NULL;
}

void editor_render(SDL_Window *window, Free_Glyph_Atlas *atlas, Simple_Renderer *sr, Editor *editor)
{
    int w, h;
    SDL_GetWindowSize(window, &w, &h);

    float max_line_len = 0.0f;

    sr->resolution = vec2f(w, h);
    sr->time = (float) SDL_GetTicks() / 1000.0f;

    // Render selection
    {
        simple_renderer_set_shader(sr, SHADER_FOR_COLOR);
        if (editor->selection) {
            for (size_t row = 0; row < editor->lines.count; ++row) {
                size_t select_begin_chr = editor->select_begin;
                size_t select_end_chr = editor->cursor;
                if (select_begin_chr > select_end_chr) {
                    SWAP(size_t, select_begin_chr, select_end_chr);
                }

                Line line_chr = editor->lines.items[row];

                if (select_begin_chr < line_chr.begin) {
                    select_begin_chr = line_chr.begin;
                }

                if (select_end_chr > line_chr.end) {
                    select_end_chr = line_chr.end;
                }

                if (select_begin_chr <= select_end_chr) {
                    Vec2f select_begin_scr = vec2f(0, -((float)row + CURSOR_OFFSET) * FREE_GLYPH_FONT_SIZE);
                    free_glyph_atlas_measure_line_sized(
                        atlas, editor->data.items + line_chr.begin, select_begin_chr - line_chr.begin,
                        &select_begin_scr);

                    Vec2f select_end_scr = select_begin_scr;
                    free_glyph_atlas_measure_line_sized(
                        atlas, editor->data.items + select_begin_chr, select_end_chr - select_begin_chr,
                        &select_end_scr);

                    Vec4f selection_color = vec4f(.25, .25, .25, 1);
                    simple_renderer_solid_rect(sr, select_begin_scr, vec2f(select_end_scr.x - select_begin_scr.x, FREE_GLYPH_FONT_SIZE), selection_color);
                }
            }
        }
        simple_renderer_flush(sr);
    }

    Vec2f cursor_pos = vec2fs(0.0f);
    {
        size_t cursor_row = editor_cursor_row(editor);
        Line line = editor->lines.items[cursor_row];
        size_t cursor_col = editor->cursor - line.begin;
        cursor_pos.y = -((float)cursor_row + CURSOR_OFFSET) * FREE_GLYPH_FONT_SIZE;
        cursor_pos.x = free_glyph_atlas_cursor_pos(
                           atlas,
                           editor->data.items + line.begin, line.end - line.begin,
                           vec2f(0.0, cursor_pos.y),
                           cursor_col
                       );
    }

    // Render search
    {
        if (editor->searching) {
            simple_renderer_set_shader(sr, SHADER_FOR_COLOR);
            Vec4f selection_color = vec4f(.10, .10, .25, 1);
            Vec2f p1 = cursor_pos;
            Vec2f p2 = p1;
            free_glyph_atlas_measure_line_sized(editor->atlas, editor->search.items, editor->search.count, &p2);
            simple_renderer_solid_rect(sr, p1, vec2f(p2.x - p1.x, FREE_GLYPH_FONT_SIZE), selection_color);
            simple_renderer_flush(sr);
        }
    }

    // Render text
    {
        simple_renderer_set_shader(sr, SHADER_FOR_TEXT);
        for (size_t i = 0; i < editor->tokens.count; ++i) {
            Token token = editor->tokens.items[i];
            Vec2f pos = token.position;
            Vec4f color = vec4fs(1);
            switch (token.kind) {
            case TOKEN_PREPROC:
                color = hex_to_vec4f(0x95A99FFF);
                break;
            case TOKEN_KEYWORD:
                color = hex_to_vec4f(0xFFDD33FF);
                break;
            case TOKEN_COMMENT:
                color = hex_to_vec4f(0xCC8C3CFF);
                break;
            case TOKEN_STRING:
                color = hex_to_vec4f(0x73c936ff);
                break;
            default:
            {}
            }
            free_glyph_atlas_render_line_sized(atlas, sr, token.text, token.text_len, &pos, color);
            // TODO: the max_line_len should be calculated based on what's visible on the screen right now
            if (max_line_len < pos.x) max_line_len = pos.x;
        }
        simple_renderer_flush(sr);
    }

    // Render cursor
    simple_renderer_set_shader(sr, SHADER_FOR_COLOR);
    {
        float CURSOR_WIDTH = 5.0f;
        if (editor->cursor_block) {
            // Normal-mode block cursor: fill the actual glyph cell under
            // the cursor (its real advance width), not just a thicker bar.
            // Falls back to the space glyph's width past end-of-buffer/line
            // or for control characters like '\n', which aren't in the
            // printable-ASCII metrics table.
            unsigned char ch = ' ';
            if (editor->cursor < editor->data.count) {
                ch = (unsigned char) editor->data.items[editor->cursor];
            }
            if (ch < 32 || ch >= GLYPH_METRICS_CAPACITY) ch = ' ';
            CURSOR_WIDTH = atlas->metrics[ch].ax;
            if (CURSOR_WIDTH <= 0.0f) CURSOR_WIDTH = atlas->metrics[(unsigned char) ' '].ax;
        }
        Uint32 CURSOR_BLINK_THRESHOLD = 500;
        Uint32 CURSOR_BLINK_PERIOD = 1000;
        Uint32 t = SDL_GetTicks() - editor->last_stroke;

        sr->verticies_count = 0;
        if (t < CURSOR_BLINK_THRESHOLD || t/CURSOR_BLINK_PERIOD%2 != 0) {
            simple_renderer_solid_rect(
                sr,
                cursor_pos, vec2f(CURSOR_WIDTH, FREE_GLYPH_FONT_SIZE),
                vec4fs(1));
        }

        simple_renderer_flush(sr);
    }

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

void editor_update_selection(Editor *e, bool shift)
{
    if (e->searching) return;
    if (shift) {
        if (!e->selection) {
            e->selection = true;
            e->select_begin = e->cursor;
        }
    } else {
        if (e->selection) {
            e->selection = false;
        }
    }
}

void editor_clipboard_copy(Editor *e)
{
    if (e->searching) return;
    if (e->selection) {
        size_t begin = e->select_begin;
        size_t end = e->cursor;
        if (begin > end) SWAP(size_t, begin, end);

        e->clipboard.count = 0;
        sb_append_buf(&e->clipboard, &e->data.items[begin], end - begin);
        sb_append_null(&e->clipboard);

        if (SDL_SetClipboardText(e->clipboard.items) < 0) {
            fprintf(stderr, "ERROR: SDL ERROR: %s\n", SDL_GetError());
        }
    }
}

void editor_clipboard_cut(Editor *e)
{
    if (e->searching) return;
    if (e->selection) {
        editor_clipboard_copy(e);
        editor_delete_selection(e);
    }
}

void editor_clipboard_paste(Editor *e)
{
    char *text = SDL_GetClipboardText();
    size_t text_len = strlen(text);
    if (text_len > 0) {
        editor_insert_buf(e, text, text_len);
    } else {
        fprintf(stderr, "ERROR: SDL ERROR: %s\n", SDL_GetError());
    }
    SDL_free(text);
}

void editor_start_search(Editor *e)
{
    if (e->searching) {
        for (size_t pos = e->cursor + 1; pos < e->data.count; ++pos) {
            if (editor_search_matches_at(e, pos)) {
                e->cursor = pos;
                break;
            }
        }
    } else {
        e->searching = true;
        if (e->selection) {
            e->selection = false;
            // TODO: put the selection into the search automatically
        } else {
            e->search.count = 0;
        }
    }
}

void editor_stop_search(Editor *e)
{
    e->searching = false;
}

bool editor_search_matches_at(Editor *e, size_t pos)
{
    if (e->data.count - pos < e->search.count) return false;
    for (size_t i = 0; i < e->search.count; ++i) {
        if (e->search.items[i] != e->data.items[pos + i]) {
            return false;
        }
    }
    return true;
}

void editor_move_to_begin(Editor *e)
{
    editor_stop_search(e);
    e->cursor = 0;
}

void editor_move_to_end(Editor *e)
{
    editor_stop_search(e);
    e->cursor = e->data.count;
}

void editor_move_to_line_begin(Editor *e)
{
    editor_stop_search(e);
    e->cursor = editor_find_line_begin(e, e->cursor);
}

void editor_move_to_line_end(Editor *e)
{
    editor_stop_search(e);
    e->cursor = editor_find_line_end(e, e->cursor);
}

void editor_move_paragraph_up(Editor *e)
{
    editor_stop_search(e);
    e->cursor = editor_find_paragraph_up(e, e->cursor);
}

void editor_move_paragraph_down(Editor *e)
{
    editor_stop_search(e);
    e->cursor = editor_find_paragraph_down(e, e->cursor);
}
