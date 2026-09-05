#ifndef COMMAND_H_
#define COMMAND_H_

#include <stdbool.h>
#include <SDL2/SDL.h>

// A command is a named, zero-argument action - the indirection between
// "a key was pressed" and "something happened". This module knows
// nothing about the editor, Vim, or any other part of the app: it's
// generic infrastructure, deliberately kept that way so it stays the
// same regardless of what ends up calling into it - a config file's
// keybinding overrides today, conceivably a plugin/scripting layer
// registering its own commands later. main.c is the only place that
// knows what "move-char-left" or "save" actually DO.
typedef void (*Command_Fn)(void);

typedef struct {
    const char *name; // e.g. "move-char-left" - stable, used by keymaps/config
    Command_Fn fn;

    // True for cursor-movement commands: main.c's SDL_KEYDOWN dispatch
    // extends the current selection first (editor_update_selection)
    // when Shift was held on the chord that resolved to one of these.
    // Not "is this technically a motion" in any deep sense - just
    // "does Shift mean 'extend selection, then run this' for this
    // command" - which happens to be exactly the movement commands.
    // Left as main.c's concern (not handled inside command_dispatch
    // here) so this module stays free of any editor.h dependency.
    bool shift_extends_selection;
} Command;

// Registers a command under `name`. Meant to be called only during
// startup, before any keymap entry can reference it - not thread-safe,
// not meant to be called once the event loop is running. Asserts if
// `name` is already registered (a programming error, not a runtime one:
// this only happens from main.c's own fixed startup registration list).
void command_register(const char *name, Command_Fn fn, bool shift_extends_selection);

// Looks up a registered command by name, or NULL if none matches.
const Command *command_find(const char *name);

// Binds `chord` (e.g. "ctrl+shift+s", "f2", "left", "y") to the command
// already registered under `command_name`. The key part is resolved
// with SDL_GetKeyFromName, so it accepts the same names SDL itself
// does (case-insensitive; "Left", "F2", "Return", single letters, ...).
// Rebinds in place if `chord` is already bound - so a config override
// replaces a built-in default instead of shadowing it with a duplicate
// entry. Returns false, leaving the keymap unchanged, if `chord` can't
// be parsed or `command_name` isn't a registered command; the caller
// decides whether that's worth surfacing (main.c's built-in bindings
// are asserted, since a typo there is a programming error - config.c's
// `bind` lines instead report it and keep going, since a user's config
// typo shouldn't be fatal).
bool keymap_bind(const char *chord, const char *command_name);

// Resolves an SDL_KEYDOWN's key+modifiers to a bound command, or NULL if
// nothing is bound to that exact chord (the caller's own fallback
// handling, if any, still runs in that case).
const Command *keymap_resolve(SDL_Keysym key);

#endif // COMMAND_H_
