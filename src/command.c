#include <string.h>
#include <assert.h>

#include "./command.h"
#include "./common.h"
#include "./sv.h"

typedef struct {
    Command *items;
    size_t count;
    size_t capacity;
} Commands;

typedef struct {
    SDL_Keycode key;
    bool ctrl;
    bool shift;
    const Command *command;
} Keybinding;

typedef struct {
    Keybinding *items;
    size_t count;
    size_t capacity;
} Keybindings;

static Commands commands = {0};
static Keybindings keymap = {0};

void command_register(const char *name, Command_Fn fn, bool shift_extends_selection)
{
    assert(command_find(name) == NULL && "command already registered under this name");
    Command cmd = {
        .name = name,
        .fn = fn,
        .shift_extends_selection = shift_extends_selection,
    };
    da_append(&commands, cmd);
}

const Command *command_find(const char *name)
{
    for (size_t i = 0; i < commands.count; ++i) {
        if (strcmp(commands.items[i].name, name) == 0) {
            return &commands.items[i];
        }
    }
    return NULL;
}

// Splits `chord` on '+' into up to 4 trimmed tokens (more than enough
// for "ctrl+shift+<key>") - every token but the last must be "ctrl" or
// "shift" (case-insensitive), and the last is the key name, resolved
// via SDL_GetKeyFromName. Returns false for anything that doesn't fit
// that shape: no tokens, more than 4, an unrecognized modifier name, or
// a key name SDL itself doesn't recognize.
static bool parse_chord(const char *chord, SDL_Keycode *out_key, bool *out_ctrl, bool *out_shift)
{
    String_View sv = sv_trim(sv_from_cstr(chord));

    String_View tokens[4];
    size_t token_count = 0;
    while (sv.count > 0 && token_count < 4) {
        tokens[token_count++] = sv_trim(sv_chop_by_delim(&sv, '+'));
    }
    if (token_count == 0 || sv.count > 0) return false;

    bool ctrl = false;
    bool shift = false;
    for (size_t i = 0; i + 1 < token_count; ++i) {
        if (sv_eq_ignorecase(tokens[i], SV("ctrl"))) {
            ctrl = true;
        } else if (sv_eq_ignorecase(tokens[i], SV("shift"))) {
            shift = true;
        } else {
            return false;
        }
    }

    // SDL_GetKeyFromName wants a NUL-terminated cstr; the token's own
    // backing memory isn't necessarily terminated right after it (it
    // points into the middle of `chord` whenever trailing modifiers or
    // whitespace follow), so copy it out first.
    String_View key_tok = tokens[token_count - 1];
    char key_name[32];
    if (key_tok.count == 0 || key_tok.count >= sizeof(key_name)) return false;
    memcpy(key_name, key_tok.data, key_tok.count);
    key_name[key_tok.count] = '\0';

    SDL_Keycode key = SDL_GetKeyFromName(key_name);
    if (key == SDLK_UNKNOWN) return false;

    *out_key = key;
    *out_ctrl = ctrl;
    *out_shift = shift;
    return true;
}

bool keymap_bind(const char *chord, const char *command_name)
{
    const Command *cmd = command_find(command_name);
    if (cmd == NULL) return false;

    SDL_Keycode key;
    bool ctrl, shift;
    if (!parse_chord(chord, &key, &ctrl, &shift)) return false;

    for (size_t i = 0; i < keymap.count; ++i) {
        if (keymap.items[i].key == key && keymap.items[i].ctrl == ctrl && keymap.items[i].shift == shift) {
            keymap.items[i].command = cmd;
            return true;
        }
    }

    Keybinding kb = { .key = key, .ctrl = ctrl, .shift = shift, .command = cmd };
    da_append(&keymap, kb);
    return true;
}

const Command *keymap_resolve(SDL_Keysym key)
{
    bool ctrl = key.mod & KMOD_CTRL;
    bool shift = key.mod & KMOD_SHIFT;
    for (size_t i = 0; i < keymap.count; ++i) {
        if (keymap.items[i].key == key.sym && keymap.items[i].ctrl == ctrl && keymap.items[i].shift == shift) {
            return keymap.items[i].command;
        }
    }
    return NULL;
}
