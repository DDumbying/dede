#ifndef CONFIG_H_
#define CONFIG_H_

#include <stdbool.h>
#include "./common.h"

#define CONFIG_FONT_PATH_CAP 512

typedef struct {
    char font_path[CONFIG_FONT_PATH_CAP];
    size_t tab_width;
    bool vim_mode;              // Vim keybindings on/off, everywhere (editor + file browser)
    bool line_numbers;          // stored for now; rendering is a follow-up
    bool relative_line_numbers; // stored for now; rendering is a follow-up
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
