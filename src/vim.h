#ifndef VIM_H_
#define VIM_H_

#include <stdbool.h>
#include <SDL2/SDL.h>
#include "./editor.h"
#include "./file_browser.h"

// Modal editing (Normal/Insert/Visual), operators (d/c/y) with counts,
// and motions built on editor.c's pure editor_find_* functions.
// editor.c has no idea Vim exists. No register of its own - yank/
// delete/put drive the editor's existing selection + system clipboard.
//
// Not implemented: text objects, named/numbered registers, marks,
// macros, command-line mode (:w, :q), linewise dj/dk/dgg/dG with
// operators, authentic linewise p/P.

typedef enum {
    VIM_MODE_NORMAL,
    VIM_MODE_INSERT,
    VIM_MODE_VISUAL,
} Vim_Mode;

typedef enum {
    VIM_OP_NONE,
    VIM_OP_DELETE, // d
    VIM_OP_CHANGE, // c
    VIM_OP_YANK,   // y
} Vim_Op;

typedef struct {
    Vim_Mode mode;
    bool pending_g;          // waiting for the second 'g' of 'gg'
    Vim_Op pending_op;       // d/c/y waiting for its motion or repeat
    size_t count;            // digits typed for the pending motion/operator
    size_t pending_op_count; // count typed before the operator itself (e.g. the '2' in "2d3w")
    bool visual_linewise;    // Visual mode entered via 'V' rather than 'v'

    // Set whenever vim_handle_key claims a key while starting out in
    // Normal or Visual mode. SDL's SDL_TEXTINPUT for that same key
    // arrives after the mode may have already changed (e.g. 'i'), so
    // checking "are we in Normal mode" there isn't enough - the caller
    // must check and clear this instead.
    bool consumed_textinput;
} Vim_State;

Vim_State vim_state_init(void);

bool vim_take_consumed_textinput(Vim_State *vs);

// Call for every SDL_KEYDOWN, before any other key handling. Returns
// true if fully handled (caller does nothing else this frame); false
// lets the caller's own mode-agnostic handling run (Ctrl+Z, F2, arrow
// keys, Backspace, ...). Updates e->cursor_block for the current mode.
bool vim_handle_key(Vim_State *vs, Editor *e, SDL_Keysym key);

// Vim-style navigation for the file browser's flat file list: j/k (with
// counts), gg/G for first/last. Reuses `vs` only for pending_g/count -
// a file list has no Insert/Visual/operator concept. Returns true if
// handled; caller's own arrow-key/Enter/F3 handling still runs otherwise.
bool vim_handle_browser_key(Vim_State *vs, File_Browser *fb, SDL_Keysym key);

#endif // VIM_H_
