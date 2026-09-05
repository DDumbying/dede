// Headless sanity check for the Step 1 motion refactor.
// Builds an Editor's `data`/`lines` by hand (no GL/atlas/window needed)
// and exercises the new pure `editor_find_*` functions directly.

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include "./editor.h"
#include "./common.h"
#include "./vim.h"
#include "./file_browser.h"

static int failures = 0;

#define CHECK_EQ(desc, got, want) do { \
    size_t _g = (got), _w = (want); \
    if (_g != _w) { \
        printf("FAIL: %s -> got %zu, want %zu\n", desc, _g, _w); \
        failures++; \
    } else { \
        printf("ok:   %s -> %zu\n", desc, _g); \
    } \
} while (0)

static void build_lines(Editor *e)
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

int main(void)
{
    Editor e = {0};
    const char *text = "hello world foo\n"   // line 0: 0..15
                        "\n"                 // line 1: 16..16 (blank -> paragraph break)
                        "second paragraph\n" // line 2: 17..33
                        "short";              // line 3: 34..39
    sb_append_buf(&e.data, text, strlen(text));
    build_lines(&e);

    printf("lines: %zu\n", e.lines.count);

    // word motions
    CHECK_EQ("word_right from 0 (\"hello\"->space)", editor_find_word_right(&e, 0), 5);
    CHECK_EQ("word_right from 5 (space->\"world\" end)", editor_find_word_right(&e, 5), 11);
    CHECK_EQ("word_left from 11 (\"world\"->start)", editor_find_word_left(&e, 11), 6);
    CHECK_EQ("word_left from 6 (space->\"hello\" start)", editor_find_word_left(&e, 6), 0);

    // char motions clamp at buffer edges
    CHECK_EQ("char_left at 0 stays 0", editor_find_char_left(&e, 0), 0);
    CHECK_EQ("char_right at end stays put", editor_find_char_right(&e, e.data.count), e.data.count);

    // line begin/end
    CHECK_EQ("line_begin from mid line 0", editor_find_line_begin(&e, 8), 0);
    CHECK_EQ("line_end from mid line 0", editor_find_line_end(&e, 8), 15);
    CHECK_EQ("line_begin on line 2", editor_find_line_begin(&e, 20), 17);
    CHECK_EQ("line_end on line 2", editor_find_line_end(&e, 20), 33);

    // vertical motion with column clamping: line 3 "short" is only 5 chars.
    // Starting at col 10 on line 2, moving down should clamp to col 5 (end of "short").
    size_t pos_on_line2_col10 = 17 + 10; // 'g' in "paragraph"
    size_t down = editor_find_line_down(&e, pos_on_line2_col10);
    CHECK_EQ("line_down clamps column to shorter line", down, 34 + 5);

    // NOTE: this codebase has no "sticky/desired column" concept - each
    // vertical move reads the column off the CURRENT position, not the
    // originally-intended one. So after clamping down to col 5 on the
    // short line, moving back up uses col 5 again, landing on line 2's
    // col 5, not the original col 10. That's a pre-existing property of
    // the algorithm (unchanged by this refactor) worth remembering once
    // Vim's j/k need a real desired-column register.
    size_t up = editor_find_line_up(&e, down);
    CHECK_EQ("line_up uses clamped column, not original (pre-existing quirk)", up, 17 + 5);

    // moving up from the very first line is a no-op
    CHECK_EQ("line_up on first line is no-op", editor_find_line_up(&e, 3), 3);
    // moving down from the very last line is a no-op
    CHECK_EQ("line_down on last line is no-op", editor_find_line_down(&e, 36), 36);

    // paragraph motions: this algorithm lands ON the blank boundary line
    // itself (like Vim's `{`/`}`), not on the first line of text past it.
    // Both directions converge on line 1, the blank line at offset 16.
    CHECK_EQ("paragraph_down from line 0 lands on the blank line", editor_find_paragraph_down(&e, 3), 16);
    CHECK_EQ("paragraph_up from line 2 lands on the blank line", editor_find_paragraph_up(&e, 20), 16);

    // Cross-check: mutators must still produce identical results to the
    // pure functions they now wrap (this is the actual "no behavior
    // change" guarantee of the refactor).
    e.cursor = 0;
    editor_move_word_right(&e);
    CHECK_EQ("mutator editor_move_word_right matches pure fn", e.cursor, editor_find_word_right(&e, 0));

    e.cursor = pos_on_line2_col10;
    editor_move_line_down(&e);
    CHECK_EQ("mutator editor_move_line_down matches pure fn", e.cursor, down);

    if (failures == 0) {
        printf("\nALL PURE-MOTION CHECKS PASSED\n");
    } else {
        printf("\n%d PURE-MOTION CHECK(S) FAILED\n", failures);
    }

    // ------------------------------------------------------------------
    // Step 2: Vim Normal/Insert dispatch. vim_handle_key only touches a
    // plain SDL_Keysym struct and the Editor - no window, no GL context,
    // no SDL_Init required - so this exercises the real dispatch code
    // exactly as main.c calls it.
    // ------------------------------------------------------------------
    printf("\n--- vim dispatch ---\n");

    #define KEY(SYM) (SDL_Keysym){.scancode = 0, .sym = (SYM), .mod = 0, .unused = 0}
    #define KEY_SHIFT(SYM) (SDL_Keysym){.scancode = 0, .sym = (SYM), .mod = KMOD_SHIFT, .unused = 0}
    #define KEY_CTRL(SYM) (SDL_Keysym){.scancode = 0, .sym = (SYM), .mod = KMOD_CTRL, .unused = 0}

    Editor v = {0};
    sb_append_buf(&v.data, "hello world\nsecond line\nx", strlen("hello world\nsecond line\nx"));
    build_lines(&v);
    Vim_State vs = vim_state_init();

    if (vs.mode != VIM_MODE_NORMAL) { printf("FAIL: should start in Normal mode\n"); failures++; }
    else printf("ok:   starts in Normal mode\n");

    // hjkl move the cursor and report "handled"
    v.cursor = 0;
    bool handled = vim_handle_key(&vs, &v, KEY(SDLK_l));
    if (!handled || v.cursor != 1) { printf("FAIL: 'l' should move right and be handled (cursor=%zu)\n", v.cursor); failures++; }
    else printf("ok:   'l' moves right (cursor=%zu)\n", v.cursor);

    // NOTE: 'w' reuses editor_move_word_right as-is (per the Step 1
    // architecture: no new motion primitives, just rebind existing
    // ones). That function stops right after the current word (matching
    // this codebase's existing Ctrl+Right behavior), NOT at the start of
    // the next word the way real Vim's `w` does. From cursor=1 (inside
    // "hello"), that lands on the trailing space at index 5, not on
    // "world" at index 6. Getting authentic Vim `w` will need its own
    // pure motion later (skip word, then skip the separator after it) -
    // tracked as a known gap here rather than silently "fixed" by
    // fudging the binding.
    handled = vim_handle_key(&vs, &v, KEY(SDLK_w));
    CHECK_EQ("'w' (approximation) stops after current word, not at next word start", v.cursor, 5);

    v.cursor = 5;
    handled = vim_handle_key(&vs, &v, KEY(SDLK_l));
    (void) handled;

    handled = vim_handle_key(&vs, &v, KEY(SDLK_e));
    CHECK_EQ("'e' jumps to end of word", v.cursor, 10);
    (void) handled;

    v.cursor = 10;
    vim_handle_key(&vs, &v, KEY_SHIFT(SDLK_4)); // '$'
    CHECK_EQ("'$' jumps to line end", v.cursor, 11);

    vim_handle_key(&vs, &v, KEY(SDLK_0));
    CHECK_EQ("'0' jumps to line begin", v.cursor, 0);

    // gg / G
    v.cursor = 5;
    vim_handle_key(&vs, &v, KEY_SHIFT(SDLK_g)); // G
    CHECK_EQ("'G' jumps to end of buffer", v.cursor, v.data.count);

    v.cursor = 5;
    bool first_g = vim_handle_key(&vs, &v, KEY(SDLK_g));
    if (!first_g || !vs.pending_g) { printf("FAIL: first 'g' should be consumed and set pending_g\n"); failures++; }
    else printf("ok:   first 'g' consumed, waiting for second\n");
    vim_handle_key(&vs, &v, KEY(SDLK_g));
    CHECK_EQ("'gg' jumps to start of buffer", v.cursor, 0);
    if (vs.pending_g) { printf("FAIL: pending_g should clear after 'gg'\n"); failures++; }

    // a lone 'g' followed by a non-'g' key cancels the pending state but
    // still processes the second key normally (doesn't eat it)
    v.cursor = 5;
    vim_handle_key(&vs, &v, KEY(SDLK_g));
    vim_handle_key(&vs, &v, KEY(SDLK_l));
    CHECK_EQ("'g' then 'l' cancels gg and still moves right", v.cursor, 6);

    // Ctrl-combos are never hijacked by Normal mode
    v.cursor = 5;
    bool ctrl_handled = vim_handle_key(&vs, &v, KEY_CTRL(SDLK_z));
    if (ctrl_handled) { printf("FAIL: Ctrl+z should NOT be claimed by vim_handle_key\n"); failures++; }
    else printf("ok:   Ctrl+z falls through (not claimed by vim layer)\n");

    // Regression check for the 'i' bug: pressing 'i' must flag the
    // trailing SDL_TEXTINPUT('i') for the caller to swallow, same as any
    // other claimed Normal-mode key - otherwise the letter that switched
    // to Insert mode also gets typed into the buffer as its first
    // character. This is the actual caller-facing contract main.c's
    // SDL_TEXTINPUT handler relies on (see vim_take_consumed_textinput).
    vim_handle_key(&vs, &v, KEY(SDLK_i));
    if (vs.mode != VIM_MODE_INSERT) { printf("FAIL: 'i' should enter Insert mode\n"); failures++; }
    else printf("ok:   'i' enters Insert mode\n");
    if (!vim_take_consumed_textinput(&vs)) { printf("FAIL: 'i' should flag its trailing SDL_TEXTINPUT to be swallowed (this is the 'i' text-leak bug)\n"); failures++; }
    else printf("ok:   'i' flags its trailing SDL_TEXTINPUT to be swallowed\n");
    if (vim_take_consumed_textinput(&vs)) { printf("FAIL: vim_take_consumed_textinput should clear the flag once taken\n"); failures++; }
    else printf("ok:   vim_take_consumed_textinput clears the flag once taken\n");

    bool j_in_insert = vim_handle_key(&vs, &v, KEY(SDLK_j));
    if (j_in_insert) { printf("FAIL: plain keys in Insert mode should not be claimed (typed as text elsewhere)\n"); failures++; }
    else printf("ok:   'j' in Insert mode is not claimed (falls through to text input)\n");

    // Escape -> back to Normal, cursor steps back one column (not past line start)
    v.cursor = 6; // on line "hello world", col 6 ('w')
    vim_handle_key(&vs, &v, KEY(SDLK_ESCAPE));
    if (vs.mode != VIM_MODE_NORMAL) { printf("FAIL: Escape should return to Normal mode\n"); failures++; }
    else printf("ok:   Escape returns to Normal mode\n");
    CHECK_EQ("leaving Insert steps cursor back one (not past line start)", v.cursor, 5);

    v.cursor = 0; // already at line start
    vs.mode = VIM_MODE_INSERT;
    vim_handle_key(&vs, &v, KEY(SDLK_ESCAPE));
    CHECK_EQ("leaving Insert at column 0 does not go negative/underflow", v.cursor, 0);

    // ------------------------------------------------------------------
    // Step 3: operators (d/c/y) composed with motions, plus x/D/C/Y/p/P.
    // These drive real buffer mutation through editor_clipboard_cut/copy
    // (which call SDL_Set/GetClipboardText) - no SDL_Init is done here
    // either, so clipboard calls may themselves fail/no-op depending on
    // platform, but the delete/change side of every op happens
    // independently of whether the copy succeeded, which is what these
    // checks verify. Only the yank-then-paste round trip needs a working
    // clipboard, so it's the one check skipped if that round trip fails.
    // ------------------------------------------------------------------
    printf("\n--- vim operators ---\n");

    #define CHECK_STR(desc, got_buf, got_len, want) do { \
        const char *_w = (want); \
        size_t _wl = strlen(_w); \
        if ((got_len) != _wl || memcmp((got_buf), _w, _wl) != 0) { \
            printf("FAIL: %s -> got \"%.*s\", want \"%s\"\n", desc, (int) (got_len), (got_buf), _w); \
            failures++; \
        } else { \
            printf("ok:   %s -> \"%.*s\"\n", desc, (int) (got_len), (got_buf)); \
        } \
    } while (0)

    Editor o = {0};
    Vim_State ov;

    #define RESET_OP_TEST(text) do { \
        o.data.count = 0; \
        sb_append_buf(&o.data, (text), strlen(text)); \
        editor_retokenize(&o); /* rebuilds o.lines; safe with o.atlas == NULL */ \
        o.cursor = 0; \
        o.selection = false; \
        ov = vim_state_init(); \
    } while (0)

    // x: delete char under cursor
    RESET_OP_TEST("hello");
    vim_handle_key(&ov, &o, KEY(SDLK_x));
    CHECK_STR("'x' deletes char under cursor", o.data.items, o.data.count, "ello");
    CHECK_EQ("'x' leaves cursor in place", o.cursor, 0);

    // x on an empty line is a no-op (nothing under the cursor to delete)
    RESET_OP_TEST("\nsecond");
    vim_handle_key(&ov, &o, KEY(SDLK_x));
    CHECK_STR("'x' on empty line is a no-op", o.data.items, o.data.count, "\nsecond");

    // dw: delete from cursor through the current word (charwise, exclusive)
    RESET_OP_TEST("hello world");
    vim_handle_key(&ov, &o, KEY(SDLK_d));
    if (ov.pending_op != VIM_OP_DELETE) { printf("FAIL: 'd' should set pending_op to DELETE\n"); failures++; }
    else printf("ok:   'd' sets pending_op to DELETE\n");
    vim_handle_key(&ov, &o, KEY(SDLK_w));
    CHECK_STR("'dw' deletes through the word (this file's 'w' approximation)", o.data.items, o.data.count, " world");
    if (ov.pending_op != VIM_OP_NONE) { printf("FAIL: pending_op should clear after 'dw'\n"); failures++; }
    else printf("ok:   pending_op clears after 'dw'\n");

    // d$ : delete to end of line, inclusive
    RESET_OP_TEST("hello world");
    o.cursor = 5; // the space right after "hello"
    vim_handle_key(&ov, &o, KEY(SDLK_d));
    vim_handle_key(&ov, &o, KEY_SHIFT(SDLK_4)); // '$'
    CHECK_STR("'d$' deletes from cursor to end of line inclusive", o.data.items, o.data.count, "hello");

    // dd: linewise delete of the whole line, including its newline
    RESET_OP_TEST("one\ntwo\nthree");
    o.cursor = 5; // inside "two"
    vim_handle_key(&ov, &o, KEY(SDLK_d));
    vim_handle_key(&ov, &o, KEY(SDLK_d));
    CHECK_STR("'dd' removes the whole line and its newline", o.data.items, o.data.count, "one\nthree");
    CHECK_EQ("'dd' leaves cursor at the deleted line's old position", o.cursor, 4);

    // dd on the buffer's only line clears it but leaves one empty line
    RESET_OP_TEST("only");
    vim_handle_key(&ov, &o, KEY(SDLK_d));
    vim_handle_key(&ov, &o, KEY(SDLK_d));
    CHECK_STR("'dd' on the only line clears it instead of erroring", o.data.items, o.data.count, "");

    // dd on the last line eats the previous line's newline, not its own
    RESET_OP_TEST("one\ntwo");
    o.cursor = 5; // inside "two", the last line
    vim_handle_key(&ov, &o, KEY(SDLK_d));
    vim_handle_key(&ov, &o, KEY(SDLK_d));
    CHECK_STR("'dd' on the last line removes it and the preceding newline", o.data.items, o.data.count, "one");

    // cw: delete through the word, then enter Insert mode at that position
    RESET_OP_TEST("hello world");
    vim_handle_key(&ov, &o, KEY(SDLK_c));
    vim_handle_key(&ov, &o, KEY(SDLK_w));
    CHECK_STR("'cw' deletes through the word", o.data.items, o.data.count, " world");
    if (ov.mode != VIM_MODE_INSERT) { printf("FAIL: 'cw' should enter Insert mode\n"); failures++; }
    else printf("ok:   'cw' enters Insert mode\n");

    // cc on an empty line: nothing to delete, but must still enter Insert
    RESET_OP_TEST("\nsecond");
    vim_handle_key(&ov, &o, KEY(SDLK_c));
    vim_handle_key(&ov, &o, KEY(SDLK_c));
    CHECK_STR("'cc' on an empty line changes nothing", o.data.items, o.data.count, "\nsecond");
    if (ov.mode != VIM_MODE_INSERT) { printf("FAIL: 'cc' on an empty line should still enter Insert mode\n"); failures++; }
    else printf("ok:   'cc' on an empty line still enters Insert mode\n");

    // cc on a normal middle line clears only its content, keeping the
    // line (and buffer line count) intact - unlike 'dd', which removes
    // the line's newline along with it.
    RESET_OP_TEST("one\ntwo\nthree");
    o.cursor = 5; // inside "two"
    vim_handle_key(&ov, &o, KEY(SDLK_c));
    vim_handle_key(&ov, &o, KEY(SDLK_c));
    CHECK_STR("'cc' clears the line's content but keeps its newline", o.data.items, o.data.count, "one\n\nthree");
    CHECK_EQ("'cc' leaves the cursor on the now-empty line", o.cursor, 4);

    // yy / y$ never modify the buffer, only move the cursor to the range start
    RESET_OP_TEST("one\ntwo\nthree");
    o.cursor = 5; // inside "two"
    vim_handle_key(&ov, &o, KEY(SDLK_y));
    vim_handle_key(&ov, &o, KEY(SDLK_y));
    CHECK_STR("'yy' does not modify the buffer", o.data.items, o.data.count, "one\ntwo\nthree");
    CHECK_EQ("'yy' moves cursor to the start of the yanked line", o.cursor, 4);

    // An operator followed by an unrecognized key cancels silently
    // instead of running that key as its own command.
    RESET_OP_TEST("hello");
    vim_handle_key(&ov, &o, KEY(SDLK_d));
    bool i_handled = vim_handle_key(&ov, &o, KEY(SDLK_i));
    if (ov.mode == VIM_MODE_INSERT) { printf("FAIL: 'd' then 'i' should cancel the pending delete, not enter Insert mode\n"); failures++; }
    else printf("ok:   'd' then 'i' cancels the pending delete instead of entering Insert mode\n");
    if (!i_handled) { printf("FAIL: the cancelling key should still be consumed\n"); failures++; }
    CHECK_STR("'d' then 'i' left the buffer untouched", o.data.items, o.data.count, "hello");
    if (ov.pending_op != VIM_OP_NONE) { printf("FAIL: pending_op should be cleared after cancelling\n"); failures++; }
    else printf("ok:   pending_op cleared after cancelling\n");

    // yank into the clipboard, then paste back - only meaningful if the
    // headless SDL clipboard round-trip actually works without SDL_Init.
    RESET_OP_TEST("hello world");
    vim_handle_key(&ov, &o, KEY(SDLK_y));
    vim_handle_key(&ov, &o, KEY(SDLK_w));
    o.cursor = o.data.count; // move to end before pasting
    vim_handle_key(&ov, &o, KEY(SDLK_p));
    if (o.data.count == strlen("hello world") + strlen("hello ")
            && memcmp(o.data.items, "hello worldhello ", o.data.count) == 0) {
        printf("ok:   'yw' then 'p' pastes the yanked text after the cursor\n");
    } else {
        printf("skip: 'yw' then 'p' round trip needs a working SDL clipboard without SDL_Init (got \"%.*s\") - exercise this manually in the real app instead\n",
               (int) o.data.count, o.data.items);
    }

    printf("\n--- vim counts ---\n");

    RESET_OP_TEST("hello world");
    vim_handle_key(&ov, &o, KEY(SDLK_5));
    vim_handle_key(&ov, &o, KEY(SDLK_l));
    CHECK_EQ("'5l' moves right 5 chars", o.cursor, 5);
    CHECK_EQ("count resets after being consumed", ov.count, 0);

    RESET_OP_TEST("one two three four");
    vim_handle_key(&ov, &o, KEY(SDLK_3));
    vim_handle_key(&ov, &o, KEY(SDLK_w));
    CHECK_EQ("'3w' moves 3 words forward", o.cursor, 13);

    RESET_OP_TEST("l0\nl1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\nl11");
    vim_handle_key(&ov, &o, KEY(SDLK_1));
    vim_handle_key(&ov, &o, KEY(SDLK_0));
    vim_handle_key(&ov, &o, KEY(SDLK_j));
    CHECK_EQ("'10j' (multi-digit count) moves down 10 lines", o.cursor, 30);

    RESET_OP_TEST("one two three four");
    vim_handle_key(&ov, &o, KEY(SDLK_3));
    vim_handle_key(&ov, &o, KEY(SDLK_d));
    vim_handle_key(&ov, &o, KEY(SDLK_w));
    CHECK_STR("'3dw' deletes through 3 words", o.data.items, o.data.count, " four");

    RESET_OP_TEST("one two three four");
    vim_handle_key(&ov, &o, KEY(SDLK_d));
    vim_handle_key(&ov, &o, KEY(SDLK_3));
    vim_handle_key(&ov, &o, KEY(SDLK_w));
    CHECK_STR("'d3w' deletes through 3 words", o.data.items, o.data.count, " four");

    RESET_OP_TEST("a b c d e f g h");
    vim_handle_key(&ov, &o, KEY(SDLK_2));
    vim_handle_key(&ov, &o, KEY(SDLK_d));
    vim_handle_key(&ov, &o, KEY(SDLK_3));
    vim_handle_key(&ov, &o, KEY(SDLK_w));
    CHECK_STR("'2d3w' multiplies counts (deletes 6 words)", o.data.items, o.data.count, " g h");

    RESET_OP_TEST("one\ntwo\nthree\nfour");
    vim_handle_key(&ov, &o, KEY(SDLK_3));
    vim_handle_key(&ov, &o, KEY(SDLK_d));
    vim_handle_key(&ov, &o, KEY(SDLK_d));
    CHECK_STR("'3dd' deletes 3 lines", o.data.items, o.data.count, "four");

    RESET_OP_TEST("hello");
    vim_handle_key(&ov, &o, KEY(SDLK_3));
    vim_handle_key(&ov, &o, KEY(SDLK_x));
    CHECK_STR("'3x' deletes 3 chars", o.data.items, o.data.count, "lo");

    RESET_OP_TEST("one\ntwo\nthree\nfour\nfive");
    vim_handle_key(&ov, &o, KEY(SDLK_3));
    vim_handle_key(&ov, &o, KEY_SHIFT(SDLK_g));
    CHECK_EQ("'3G' jumps to line 3", o.cursor, 8);

    RESET_OP_TEST("one\ntwo\nthree\nfour\nfive");
    vim_handle_key(&ov, &o, KEY(SDLK_3));
    vim_handle_key(&ov, &o, KEY(SDLK_g));
    vim_handle_key(&ov, &o, KEY(SDLK_g));
    CHECK_EQ("'3gg' jumps to line 3", o.cursor, 8);

    printf("\n--- vim visual mode ---\n");

    RESET_OP_TEST("hello world");
    vim_handle_key(&ov, &o, KEY(SDLK_v));
    if (ov.mode != VIM_MODE_VISUAL) { printf("FAIL: 'v' should enter Visual mode\n"); failures++; }
    else printf("ok:   'v' enters Visual mode\n");
    if (!o.selection) { printf("FAIL: 'v' should start a selection anchored at the cursor\n"); failures++; }
    else printf("ok:   'v' starts a selection anchored at the cursor\n");

    vim_handle_key(&ov, &o, KEY(SDLK_l));
    vim_handle_key(&ov, &o, KEY(SDLK_l));
    vim_handle_key(&ov, &o, KEY(SDLK_l));
    vim_handle_key(&ov, &o, KEY(SDLK_x));
    CHECK_STR("Visual 'lllx' deletes the 4 selected chars (inclusive)", o.data.items, o.data.count, "o world");
    if (ov.mode != VIM_MODE_NORMAL) { printf("FAIL: deleting a Visual selection should return to Normal mode\n"); failures++; }
    else printf("ok:   deleting a Visual selection returns to Normal mode\n");
    if (o.selection) { printf("FAIL: deleting a Visual selection should clear it\n"); failures++; }
    else printf("ok:   deleting a Visual selection clears it\n");

    RESET_OP_TEST("one two three");
    vim_handle_key(&ov, &o, KEY(SDLK_v));
    vim_handle_key(&ov, &o, KEY(SDLK_w));
    vim_handle_key(&ov, &o, KEY(SDLK_y));
    CHECK_STR("Visual yank does not modify the buffer", o.data.items, o.data.count, "one two three");
    CHECK_EQ("Visual yank leaves cursor at the selection start", o.cursor, 0);
    if (ov.mode != VIM_MODE_NORMAL) { printf("FAIL: yanking a Visual selection should return to Normal mode\n"); failures++; }
    else printf("ok:   yanking a Visual selection returns to Normal mode\n");

    RESET_OP_TEST("hello world");
    vim_handle_key(&ov, &o, KEY(SDLK_v));
    vim_handle_key(&ov, &o, KEY(SDLK_e));
    vim_handle_key(&ov, &o, KEY(SDLK_c));
    CHECK_STR("Visual change deletes the selection", o.data.items, o.data.count, " world");
    if (ov.mode != VIM_MODE_INSERT) { printf("FAIL: Visual 'c' should enter Insert mode\n"); failures++; }
    else printf("ok:   Visual change enters Insert mode\n");

    RESET_OP_TEST("one\ntwo\nthree\nfour");
    o.cursor = 5; // inside "two"
    vim_handle_key(&ov, &o, KEY_SHIFT(SDLK_v));
    vim_handle_key(&ov, &o, KEY(SDLK_j));
    vim_handle_key(&ov, &o, KEY(SDLK_d));
    CHECK_STR("Visual-line 'Vjd' deletes 2 whole lines", o.data.items, o.data.count, "one\nfour");

    RESET_OP_TEST("hello world");
    vim_handle_key(&ov, &o, KEY(SDLK_v));
    vim_handle_key(&ov, &o, KEY(SDLK_l));
    vim_handle_key(&ov, &o, KEY(SDLK_ESCAPE));
    CHECK_STR("Escape in Visual mode leaves the buffer untouched", o.data.items, o.data.count, "hello world");
    if (ov.mode != VIM_MODE_NORMAL) { printf("FAIL: Escape should return to Normal mode from Visual\n"); failures++; }
    else printf("ok:   Escape returns to Normal mode from Visual\n");
    if (o.selection) { printf("FAIL: Escape from Visual should clear the selection\n"); failures++; }
    else printf("ok:   Escape from Visual clears the selection\n");

    RESET_OP_TEST("hello world");
    vim_handle_key(&ov, &o, KEY(SDLK_v));
    vim_handle_key(&ov, &o, KEY(SDLK_v));
    if (ov.mode != VIM_MODE_NORMAL) { printf("FAIL: pressing 'v' again should toggle Visual mode off\n"); failures++; }
    else printf("ok:   pressing 'v' again toggles Visual mode off\n");

    RESET_OP_TEST("hello world");
    vim_handle_key(&ov, &o, KEY_SHIFT(SDLK_v));
    vim_handle_key(&ov, &o, KEY(SDLK_v));
    if (ov.mode != VIM_MODE_VISUAL || ov.visual_linewise) { printf("FAIL: 'v' after 'V' should switch to charwise, staying in Visual\n"); failures++; }
    else printf("ok:   'v' after 'V' switches Visual mode to charwise\n");

    printf("\n--- vim file-browser navigation ---\n");

    const char *browser_names[] = {"a.txt", "b.txt", "c.txt", "d.txt", "e.txt"};
    File_Browser fbv = {0};
    fbv.files.items = browser_names;
    fbv.files.count = sizeof(browser_names) / sizeof(browser_names[0]);
    fbv.cursor = 0;
    Vim_State bv = vim_state_init();

    bool browser_handled = vim_handle_browser_key(&bv, &fbv, KEY(SDLK_j));
    CHECK_EQ("'j' moves the browser cursor down one", fbv.cursor, 1);
    if (!browser_handled) { printf("FAIL: 'j' should be claimed by vim_handle_browser_key\n"); failures++; }
    else printf("ok:   'j' is claimed by vim_handle_browser_key\n");

    vim_handle_browser_key(&bv, &fbv, KEY(SDLK_3));
    vim_handle_browser_key(&bv, &fbv, KEY(SDLK_j));
    CHECK_EQ("'3j' moves the browser cursor down 3 more (clamped to last file)", fbv.cursor, 4);

    fbv.cursor = 4;
    vim_handle_browser_key(&bv, &fbv, KEY(SDLK_j));
    CHECK_EQ("'j' at the last file stays clamped there", fbv.cursor, 4);

    fbv.cursor = 4;
    vim_handle_browser_key(&bv, &fbv, KEY(SDLK_k));
    CHECK_EQ("'k' moves the browser cursor up one", fbv.cursor, 3);

    fbv.cursor = 1;
    vim_handle_browser_key(&bv, &fbv, KEY(SDLK_k));
    vim_handle_browser_key(&bv, &fbv, KEY(SDLK_k));
    CHECK_EQ("'k' at the first file stays clamped there", fbv.cursor, 0);

    fbv.cursor = 2;
    vim_handle_browser_key(&bv, &fbv, KEY_SHIFT(SDLK_g));
    CHECK_EQ("'G' jumps to the last file", fbv.cursor, 4);

    vim_handle_browser_key(&bv, &fbv, KEY(SDLK_g));
    vim_handle_browser_key(&bv, &fbv, KEY(SDLK_g));
    CHECK_EQ("'gg' jumps to the first file", fbv.cursor, 0);

    fbv.cursor = 0;
    browser_handled = vim_handle_browser_key(&bv, &fbv, KEY_CTRL(SDLK_z));
    if (browser_handled) { printf("FAIL: Ctrl-combos should not be claimed by vim_handle_browser_key\n"); failures++; }
    else printf("ok:   Ctrl-combos fall through vim_handle_browser_key\n");

    fbv.cursor = 0;
    browser_handled = vim_handle_browser_key(&bv, &fbv, KEY(SDLK_RETURN));
    if (browser_handled) { printf("FAIL: Enter should not be claimed, so the caller's own open-file handling still runs\n"); failures++; }
    else printf("ok:   Enter falls through vim_handle_browser_key\n");

    if (failures == 0) {
        printf("\nALL CHECKS PASSED\n");
        return 0;
    } else {
        printf("\n%d CHECK(S) FAILED\n", failures);
        return 1;
    }
}
