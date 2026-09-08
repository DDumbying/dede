#ifndef CONFIG_H_
#define CONFIG_H_

#include <stdbool.h>
#include <stdint.h>
#include "./common.h"

#define CONFIG_FONT_PATH_CAP 512

// Every themable color, as 0xRRGGBBAA (same encoding hex_to_vec4f takes)
// - background, selection, syntax-highlight, and UI-bar colors that used
// to be hardcoded literals scattered across editor.c/app.c. Field names
// double as config file keys (see config_apply_line's theme_fields
// table), so renaming one means renaming the other to match.
typedef struct {
    uint32_t bg;              // editor/splash/about background; also the block cursor's reverse-video fill
    uint32_t fg;               // default text color
    uint32_t selection;        // text selection highlight
    uint32_t search_current;   // search match under the cursor
    uint32_t search_other;     // every other search match
    uint32_t gutter_bg;        // line-number gutter's background
    uint32_t gutter_fg;        // line-number text
    uint32_t accent;           // keyword syntax color; also splash/about's selected-row and title accents
    uint32_t comment;          // // comment syntax color
    uint32_t string;           // string literal syntax color
    uint32_t preproc;          // preprocessor directive syntax color
    uint32_t error_text;       // splash-screen error message text
    uint32_t ui_bar_bg;        // status bar's normal (Ln/Col) background
    uint32_t ui_bar_prompt_bg; // status bar's save-as/command/replace prompt background
    uint32_t ui_bar_error_bg;  // status bar's confirm/error prompt background
    uint32_t divider;          // splash divider line; also the selected splash/command row's highlight background
} Theme;

typedef struct {
    char font_path[CONFIG_FONT_PATH_CAP];
    size_t tab_width;
    bool vim_mode;              // Vim keybindings on/off, everywhere (editor + file browser)
    bool line_numbers;          // show the gutter
    bool relative_line_numbers; // gutter shows distance-from-cursor instead of absolute numbers, except on the cursor's own line
    Theme theme;
} Config;

// Built-in defaults - what the editor uses with no config file, or for
// any setting a config file doesn't mention. Not the same as `Config{0}`
// (empty font_path, tab_width 0 - see Editor::indent_width's comment).
Config config_default(void);

// Loads `path` on top of `*cfg` (already holding at least
// config_default()'s values), applying only the settings the file
// actually mentions. A missing file is the expected common case, not a
// fault: `*cfg` is left untouched and this returns 0. Any `bind <chord>
// = <command>` line calls keymap_bind (see command.h) directly, so
// config-driven keybindings and the built-in defaults go through the
// exact same parser and the exact same keymap - a config bind for a
// chord that's already bound (built-in or from an earlier config line)
// replaces it rather than adding a second, shadowed entry. A line this
// can't parse (bad syntax, unknown key, unrecognized chord or command
// name) is reported to stderr and skipped - never fatal to the rest of
// the file, since a config file is user-editable text a typo shouldn't
// brick the editor over.
Errno config_load(const char *path, Config *cfg);

#endif // CONFIG_H_
