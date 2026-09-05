#include "./vim.h"

Vim_State vim_state_init(void)
{
    return (Vim_State) {
        .mode = VIM_MODE_NORMAL,
        .pending_g = false,
        .pending_op = VIM_OP_NONE,
        .count = 0,
        .pending_op_count = 0,
        .visual_linewise = false,
        .consumed_textinput = false,
    };
}

bool vim_take_consumed_textinput(Vim_State *vs)
{
    bool result = vs->consumed_textinput;
    vs->consumed_textinput = false;
    return result;
}

static void vim_enter_insert(Vim_State *vs, Editor *e)
{
    vs->mode = VIM_MODE_INSERT;
    e->cursor_block = false;
}

static void vim_enter_normal(Vim_State *vs, Editor *e)
{
    vs->mode = VIM_MODE_NORMAL;
    vs->pending_g = false;
    vs->pending_op = VIM_OP_NONE;
    e->cursor_block = true;

    // Leaving Insert steps the cursor back one column (never past line start).
    size_t line_begin = editor_find_line_begin(e, e->cursor);
    if (e->cursor > line_begin) {
        e->cursor = editor_find_char_left(e, e->cursor);
    }
}

static void vim_exit_visual(Vim_State *vs, Editor *e)
{
    vs->mode = VIM_MODE_NORMAL;
    vs->visual_linewise = false;
    e->selection = false;
    e->cursor_block = true;
}

static void vim_accumulate_count(Vim_State *vs, size_t digit)
{
    vs->count = vs->count * 10 + digit;
    if (vs->count > 100000) vs->count = 100000;
}

static size_t vim_line_pos(const Editor *e, size_t line_number)
{
    size_t row = line_number - 1;
    if (row >= e->lines.count) row = e->lines.count - 1;
    return e->lines.items[row].begin;
}

static void vim_goto_begin_or_line(Editor *e, size_t line_number)
{
    if (line_number > 0) e->cursor = vim_line_pos(e, line_number);
    else editor_move_to_begin(e);
}

static void vim_goto_end_or_line(Editor *e, size_t line_number)
{
    if (line_number > 0) e->cursor = vim_line_pos(e, line_number);
    else editor_move_to_end(e);
}

static void vim_select_range(Editor *e, size_t begin, size_t end)
{
    e->selection = true;
    e->select_begin = begin;
    e->cursor = end;
}

static void vim_delete_range(Editor *e, size_t begin, size_t end)
{
    vim_select_range(e, begin, end);
    editor_clipboard_cut(e); // leaves e->cursor == begin
}

static void vim_yank_range(Editor *e, size_t begin, size_t end)
{
    vim_select_range(e, begin, end);
    editor_clipboard_copy(e);
    e->selection = false;
    e->cursor = begin;
}

// [begin, end) for 'dd'/'yy' with `count` lines from the one containing
// `pos`, newline included so the lines are fully removed - except the
// buffer's last line, which eats the *previous* line's newline instead
// since it has none of its own.
static void vim_linewise_range_n(const Editor *e, size_t pos, size_t count, size_t *out_begin, size_t *out_end)
{
    if (count == 0) count = 1;
    size_t row = editor_row_at(e, pos);
    size_t last_row = row + count - 1;
    if (last_row >= e->lines.count) last_row = e->lines.count - 1;

    if (last_row + 1 < e->lines.count) {
        *out_begin = e->lines.items[row].begin;
        *out_end = e->lines.items[last_row + 1].begin;
    } else if (row > 0) {
        *out_begin = e->lines.items[row - 1].end;
        *out_end = e->lines.items[last_row].end;
    } else {
        *out_begin = e->lines.items[row].begin;
        *out_end = e->lines.items[last_row].end;
    }
}

// [begin, end) for 'cc': `count` lines' content merged into one,
// excluding the newline after the last of them (unlike dd, cc keeps a line).
static void vim_line_content_range_n(const Editor *e, size_t pos, size_t count, size_t *out_begin, size_t *out_end)
{
    if (count == 0) count = 1;
    size_t row = editor_row_at(e, pos);
    size_t last_row = row + count - 1;
    if (last_row >= e->lines.count) last_row = e->lines.count - 1;
    *out_begin = e->lines.items[row].begin;
    *out_end = e->lines.items[last_row].end;
}

// Same idea, but for an already-known [begin, end) span (Visual linewise).
static void vim_visual_linewise_range(const Editor *e, size_t begin, size_t end, size_t *out_begin, size_t *out_end)
{
    size_t row_begin = editor_row_at(e, begin);
    size_t row_end = editor_row_at(e, end);
    *out_begin = e->lines.items[row_begin].begin;
    if (row_end + 1 < e->lines.count) {
        *out_end = e->lines.items[row_end + 1].begin;
    } else {
        *out_end = e->lines.items[row_end].end;
    }
}

// (buffer, pos) -> pos for a single motion key. `shift` matters only
// for '0' (never) and '$' (always). `*out_inclusive` says whether the
// landing char is itself part of an operator's range (true for e/$).
static bool vim_resolve_motion(const Editor *e, SDL_Keysym key, bool shift, size_t pos, size_t *out_pos, bool *out_inclusive)
{
    *out_inclusive = false;
    switch (key.sym) {
    case SDLK_h: *out_pos = editor_find_char_left(e, pos);  return true;
    case SDLK_l: *out_pos = editor_find_char_right(e, pos); return true;
    case SDLK_w: *out_pos = editor_find_word_right(e, pos); return true;
    case SDLK_b: *out_pos = editor_find_word_left(e, pos);  return true;

    case SDLK_e:
        *out_pos = editor_find_word_end(e, pos);
        *out_inclusive = true;
        return true;

    case SDLK_0:
        if (shift) return false;
        *out_pos = editor_find_line_begin(e, pos);
        return true;

    case SDLK_4: // Shift+4 == '$'
        if (!shift) return false;
        *out_pos = editor_find_line_end(e, pos);
        *out_inclusive = true;
        return true;

    default:
        return false;
    }
}

static bool vim_resolve_motion_n(const Editor *e, SDL_Keysym key, bool shift, size_t count, size_t *out_pos, bool *out_inclusive)
{
    if (count == 0) count = 1;
    size_t pos = e->cursor;
    bool inclusive = false;
    for (size_t i = 0; i < count; ++i) {
        size_t next;
        if (!vim_resolve_motion(e, key, shift, pos, &next, &inclusive)) return false;
        pos = next;
    }
    *out_pos = pos;
    *out_inclusive = inclusive;
    return true;
}

// h/l/w/b/e/0/$/j/k with the pending count applied - shared by Normal
// and Visual dispatch. gg/G aren't here: their two-key/count-as-line-
// number shape doesn't fit "resolve one target position".
static bool vim_try_movement(Vim_State *vs, Editor *e, SDL_Keysym key, bool shift, size_t *out_pos)
{
    switch (key.sym) {
    case SDLK_h:
    case SDLK_l:
    case SDLK_w:
    case SDLK_b:
    case SDLK_e:
    case SDLK_0:
    case SDLK_4: {
        bool inclusive;
        return vim_resolve_motion_n(e, key, shift, vs->count, out_pos, &inclusive);
    }

    case SDLK_j: {
        size_t pos = e->cursor;
        size_t count = vs->count ? vs->count : 1;
        for (size_t i = 0; i < count; ++i) pos = editor_find_line_down(e, pos);
        *out_pos = pos;
        return true;
    }

    case SDLK_k: {
        size_t pos = e->cursor;
        size_t count = vs->count ? vs->count : 1;
        for (size_t i = 0; i < count; ++i) pos = editor_find_line_up(e, pos);
        *out_pos = pos;
        return true;
    }

    default:
        return false;
    }
}

static void vim_delete_char_under_cursor(Editor *e, size_t count)
{
    if (count == 0) count = 1;
    size_t begin = e->cursor;
    size_t line_end = editor_find_line_end(e, begin);
    if (begin >= line_end) return;
    size_t end = begin;
    for (size_t i = 0; i < count && end < line_end; ++i) {
        end = editor_find_char_right(e, end);
    }
    vim_delete_range(e, begin, end);
}

static void vim_delete_to_line_end(Editor *e)
{
    size_t end = editor_find_line_end(e, e->cursor);
    if (e->cursor < end) vim_delete_range(e, e->cursor, end);
}

static void vim_change_to_line_end(Vim_State *vs, Editor *e)
{
    vim_delete_to_line_end(e);
    vim_enter_insert(vs, e);
}

static void vim_yank_line(Editor *e)
{
    size_t begin, end;
    vim_linewise_range_n(e, e->cursor, 1, &begin, &end);
    vim_yank_range(e, begin, end);
}

// p/P: charwise put only - the clipboard has no linewise flag, so both
// just insert at a cursor position rather than a new line below/above.
static void vim_paste(Editor *e, bool after)
{
    if (after && e->cursor < e->data.count) {
        e->cursor = editor_find_char_right(e, e->cursor);
    }
    editor_clipboard_paste(e);
}

// Finishes the pending operator against [begin, end) (order doesn't
// matter). Delete/change treat an empty range as "nothing to delete",
// not "skip the operator" - cc/cw on an empty line still enters Insert.
static void vim_apply_pending_op(Vim_State *vs, Editor *e, size_t begin, size_t end)
{
    if (begin > end) SWAP(size_t, begin, end);
    Vim_Op op = vs->pending_op;
    vs->pending_op = VIM_OP_NONE;

    switch (op) {
    case VIM_OP_DELETE:
        if (begin < end) vim_delete_range(e, begin, end);
        else e->cursor = begin;
        break;

    case VIM_OP_YANK:
        if (begin < end) vim_yank_range(e, begin, end);
        break;

    case VIM_OP_CHANGE:
        if (begin < end) vim_delete_range(e, begin, end);
        else e->cursor = begin;
        vim_enter_insert(vs, e);
        break;

    case VIM_OP_NONE:
        break;
    }
}

static bool vim_dispatch_pending_op(Vim_State *vs, Editor *e, SDL_Keysym key, bool shift, bool ctrl)
{
    if (ctrl) {
        vs->pending_op = VIM_OP_NONE;
        vs->count = 0;
        vs->pending_op_count = 0;
        return false;
    }

    if (!shift && key.sym >= SDLK_0 && key.sym <= SDLK_9 && !(key.sym == SDLK_0 && vs->count == 0)) {
        vim_accumulate_count(vs, key.sym - SDLK_0);
        return true;
    }

    size_t total_count = (vs->pending_op_count ? vs->pending_op_count : 1) * (vs->count ? vs->count : 1);

    bool is_same_letter =
        (vs->pending_op == VIM_OP_DELETE && key.sym == SDLK_d && !shift) ||
        (vs->pending_op == VIM_OP_CHANGE && key.sym == SDLK_c && !shift) ||
        (vs->pending_op == VIM_OP_YANK   && key.sym == SDLK_y && !shift);
    if (is_same_letter) {
        size_t begin, end;
        if (vs->pending_op == VIM_OP_CHANGE) {
            vim_line_content_range_n(e, e->cursor, total_count, &begin, &end);
        } else {
            vim_linewise_range_n(e, e->cursor, total_count, &begin, &end);
        }
        vs->count = 0;
        vs->pending_op_count = 0;
        vim_apply_pending_op(vs, e, begin, end);
        return true;
    }

    size_t target;
    bool inclusive;
    if (vim_resolve_motion_n(e, key, shift, total_count, &target, &inclusive)) {
        size_t begin = e->cursor, end = target;
        if (begin > end) SWAP(size_t, begin, end);
        if (inclusive && end < e->data.count) end += 1;
        vs->count = 0;
        vs->pending_op_count = 0;
        vim_apply_pending_op(vs, e, begin, end);
        return true;
    }

    vs->pending_op = VIM_OP_NONE;
    vs->count = 0;
    vs->pending_op_count = 0;
    if (key.sym == SDLK_ESCAPE) return false; // let main.c's own Escape handling still run
    return true;
}

// Delete/change/yank the current Visual selection, then leave Visual
// mode - except for change, which drops into Insert instead.
static void vim_visual_apply(Vim_State *vs, Editor *e, Vim_Op op)
{
    size_t begin = e->select_begin;
    size_t end = e->cursor;
    if (begin > end) SWAP(size_t, begin, end);

    if (vs->visual_linewise) {
        vim_visual_linewise_range(e, begin, end, &begin, &end);
    } else if (end < e->data.count) {
        end += 1; // Visual charwise selection is inclusive of its end
    }

    e->selection = false;
    vs->visual_linewise = false;
    vs->pending_op = op;
    vim_apply_pending_op(vs, e, begin, end);

    if (vs->mode == VIM_MODE_VISUAL) {
        vs->mode = VIM_MODE_NORMAL;
        e->cursor_block = true;
    }
}

static bool vim_dispatch_visual_key(Vim_State *vs, Editor *e, SDL_Keysym key, bool shift, bool ctrl)
{
    if (ctrl) {
        vs->count = 0;
        return false;
    }

    if (!shift && key.sym >= SDLK_0 && key.sym <= SDLK_9 && !(key.sym == SDLK_0 && vs->count == 0)) {
        vim_accumulate_count(vs, key.sym - SDLK_0);
        return true;
    }

    if (vs->pending_g) {
        vs->pending_g = false;
        if (key.sym == SDLK_g && !shift) {
            vim_goto_begin_or_line(e, vs->count);
            vs->count = 0;
            return true;
        }
    }

    size_t pos;
    if (vim_try_movement(vs, e, key, shift, &pos)) {
        e->cursor = pos;
        vs->count = 0;
        return true;
    }

    switch (key.sym) {
    case SDLK_g:
        if (shift) {
            vim_goto_end_or_line(e, vs->count);
            vs->count = 0;
        } else {
            vs->pending_g = true;
        }
        return true;

    case SDLK_v:
        if (shift) {
            if (vs->visual_linewise) vim_exit_visual(vs, e);
            else vs->visual_linewise = true;
        } else {
            if (!vs->visual_linewise) vim_exit_visual(vs, e);
            else vs->visual_linewise = false;
        }
        vs->count = 0;
        return true;

    case SDLK_ESCAPE:
        vim_exit_visual(vs, e);
        vs->count = 0;
        return false;

    case SDLK_d:
    case SDLK_x:
        if (shift) break;
        vim_visual_apply(vs, e, VIM_OP_DELETE);
        vs->count = 0;
        return true;

    case SDLK_c:
        if (shift) break;
        vim_visual_apply(vs, e, VIM_OP_CHANGE);
        vs->count = 0;
        return true;

    case SDLK_y:
        if (shift) break;
        vim_visual_apply(vs, e, VIM_OP_YANK);
        vs->count = 0;
        return true;

    default:
        break;
    }

    vs->count = 0;
    return false;
}

static bool vim_dispatch_key(Vim_State *vs, Editor *e, SDL_Keysym key)
{
    if (e->searching) return false;

    if (vs->mode == VIM_MODE_INSERT) {
        if (key.sym == SDLK_ESCAPE) vim_enter_normal(vs, e); // don't consume; main.c's Escape still runs
        return false;
    }

    bool shift = key.mod & KMOD_SHIFT;
    bool ctrl  = key.mod & KMOD_CTRL;

    if (vs->mode == VIM_MODE_VISUAL) {
        return vim_dispatch_visual_key(vs, e, key, shift, ctrl);
    }

    if (vs->pending_g) {
        vs->pending_g = false;
        if (key.sym == SDLK_g && !shift && !ctrl) {
            vim_goto_begin_or_line(e, vs->count);
            vs->count = 0;
            return true;
        }
    }

    if (ctrl) {
        vs->pending_op = VIM_OP_NONE;
        vs->count = 0;
        vs->pending_op_count = 0;
        return false;
    }

    if (!shift && key.sym >= SDLK_0 && key.sym <= SDLK_9 && !(key.sym == SDLK_0 && vs->count == 0)) {
        vim_accumulate_count(vs, key.sym - SDLK_0);
        return true;
    }

    if (vs->pending_op != VIM_OP_NONE) {
        return vim_dispatch_pending_op(vs, e, key, shift, ctrl);
    }

    switch (key.sym) {
    case SDLK_i:
        if (!shift) {
            vim_enter_insert(vs, e);
            vs->count = 0;
            return true;
        }
        break;

    case SDLK_h:
    case SDLK_l:
    case SDLK_w:
    case SDLK_b:
    case SDLK_e:
    case SDLK_0:
    case SDLK_4:
    case SDLK_j:
    case SDLK_k: {
        size_t pos;
        if (vim_try_movement(vs, e, key, shift, &pos)) {
            e->cursor = pos;
            vs->count = 0;
            return true;
        }
        break;
    }

    case SDLK_g:
        if (shift) {
            vim_goto_end_or_line(e, vs->count);
            vs->count = 0;
        } else {
            vs->pending_g = true;
        }
        return true;

    case SDLK_v:
        vs->mode = VIM_MODE_VISUAL;
        vs->visual_linewise = shift;
        e->selection = true;
        e->select_begin = e->cursor;
        vs->count = 0;
        return true;

    case SDLK_x:
        if (!shift) {
            vim_delete_char_under_cursor(e, vs->count);
            vs->count = 0;
            return true;
        }
        break;

    case SDLK_d:
        if (shift) {
            vim_delete_to_line_end(e);
            vs->count = 0;
        } else {
            vs->pending_op_count = vs->count;
            vs->count = 0;
            vs->pending_op = VIM_OP_DELETE;
        }
        return true;

    case SDLK_c:
        if (shift) {
            vim_change_to_line_end(vs, e);
            vs->count = 0;
        } else {
            vs->pending_op_count = vs->count;
            vs->count = 0;
            vs->pending_op = VIM_OP_CHANGE;
        }
        return true;

    case SDLK_y:
        if (shift) {
            vim_yank_line(e);
            vs->count = 0;
        } else {
            vs->pending_op_count = vs->count;
            vs->count = 0;
            vs->pending_op = VIM_OP_YANK;
        }
        return true;

    case SDLK_p:
        vim_paste(e, !shift);
        vs->count = 0;
        return true;

    case SDLK_ESCAPE:
        vs->count = 0;
        break; // let main.c's own Escape command still run

    default:
        break;
    }

    return false;
}

bool vim_handle_key(Vim_State *vs, Editor *e, SDL_Keysym key)
{
    bool was_insert = (vs->mode == VIM_MODE_INSERT);
    bool handled = vim_dispatch_key(vs, e, key);
    if (!was_insert && handled) {
        vs->consumed_textinput = true;
    }
    return handled;
}

bool vim_handle_browser_key(Vim_State *vs, File_Browser *fb, SDL_Keysym key)
{
    bool shift = key.mod & KMOD_SHIFT;
    bool ctrl  = key.mod & KMOD_CTRL;

    if (ctrl) {
        vs->count = 0;
        return false;
    }

    if (!shift && key.sym >= SDLK_0 && key.sym <= SDLK_9 && !(key.sym == SDLK_0 && vs->count == 0)) {
        vim_accumulate_count(vs, key.sym - SDLK_0);
        return true;
    }

    if (vs->pending_g) {
        vs->pending_g = false;
        if (key.sym == SDLK_g && !shift) {
            fb->cursor = 0;
            vs->count = 0;
            return true;
        }
    }

    size_t count = vs->count ? vs->count : 1;
    switch (key.sym) {
    case SDLK_j:
        fb->cursor += count;
        if (fb->cursor >= fb->files.count) fb->cursor = fb->files.count ? fb->files.count - 1 : 0;
        vs->count = 0;
        return true;

    case SDLK_k:
        fb->cursor = (count <= fb->cursor) ? fb->cursor - count : 0;
        vs->count = 0;
        return true;

    case SDLK_g:
        if (shift) {
            fb->cursor = fb->files.count ? fb->files.count - 1 : 0;
            vs->count = 0;
        } else {
            vs->pending_g = true;
        }
        return true;

    default:
        vs->count = 0;
        return false;
    }
}
