#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <assert.h>

#include <SDL2/SDL.h>
#define GLEW_STATIC
#include <GL/glew.h>
#define GL_GLEXT_PROTOTYPES
#include <SDL2/SDL_opengl.h>

#include "./app.h"
#include "./editor.h"
#include "./file_browser.h"
#include "./la.h"
#include "./free_glyph.h"
#include "./simple_renderer.h"
#include "./common.h"
#include "./vim.h"
#include "./command.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

// TODO: Delete a word

static Free_Glyph_Atlas atlas = {0};
static Simple_Renderer sr = {0};
static Editor editor = {0};
static File_Browser fb = {0};
static Vim_State vim = {0};

// When false, vim_handle_key/vim_handle_browser_key are never called -
// Vim is fully inert, not just "returning false a lot".
static bool vim_enabled = true;

// Which full-screen view is active. Mutually exclusive by construction
// (unlike plain bools) - exactly one of these is ever "current" at a time.
typedef enum {
    APP_MODE_EDITOR,
    APP_MODE_FILE_BROWSER,
    APP_MODE_SAVE_AS, // typing a destination path for F2/Ctrl+Shift+S
    APP_MODE_CONFIRM, // "you have unsaved changes, proceed anyway? (y/n)"
    APP_MODE_COMMAND, // typing an Ex-style :command (Vim Normal mode only)
} App_Mode;

// What to do once APP_MODE_CONFIRM's y/n is answered - set by
// request_action(), read back by perform_action().
typedef enum {
    CONFIRM_ACTION_OPEN_BROWSER, // F3
    CONFIRM_ACTION_NEW_FILE,     // Ctrl+N
    CONFIRM_ACTION_QUIT,         // Ctrl+Q / closing the window
} Confirm_Action;

static App_Mode mode = APP_MODE_EDITOR;
static Confirm_Action pending_action = CONFIRM_ACTION_QUIT;
static bool quit = false;

// True for one SDL_TEXTINPUT following a SDL_KEYDOWN that changed `mode` -
// that trailing text belongs to the mode we left and must never be typed.
static bool app_mode_changed_this_key = false;

#define SAVE_AS_PATH_CAP 1024
static char save_as_path[SAVE_AS_PATH_CAP] = {0};
static size_t save_as_path_len = 0;

#define COMMAND_LINE_CAP 128
static char command_line[COMMAND_LINE_CAP] = {0};
static size_t command_line_len = 0;

// Removes the last full UTF-8 character from a NUL-terminated buffer
// (steps back over trailing continuation bytes first) - shared by the
// Save-As and :-command prompts' own Backspace handling below.
static void backspace_utf8(char *buf, size_t *len)
{
    if (*len == 0) return;
    size_t new_len = *len - 1;
    while (new_len > 0 && utf8_is_continuation((unsigned char) buf[new_len])) {
        new_len -= 1;
    }
    *len = new_len;
    buf[*len] = '\0';
}

#define ERROR_DISPLAY_MS 4000
static char error_message[256] = {0};
static Uint32 error_message_time = 0;

// Also kept around (not just stderr) to flash in a bottom bar for a
// few seconds the next time the editor view renders.
static void flash_error(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");

    va_start(args, fmt);
    vsnprintf(error_message, sizeof(error_message), fmt, args);
    va_end(args);
    error_message_time = SDL_GetTicks();
}

// Carries out a Confirm_Action - either request_action skipped the
// prompt (nothing to lose), or the user just answered 'y' to it.
static void perform_action(Confirm_Action action)
{
    editor_flush_group(&editor);
    switch (action) {
    case CONFIRM_ACTION_OPEN_BROWSER:
        mode = APP_MODE_FILE_BROWSER;
        break;
    case CONFIRM_ACTION_NEW_FILE:
        editor_new_file(&editor);
        vim = vim_state_init();
        editor.cursor_block = vim_enabled;
        mode = APP_MODE_EDITOR;
        break;
    case CONFIRM_ACTION_QUIT:
        quit = true;
        break;
    }
}

// Entry point for anything that would discard unsaved work: runs
// immediately if the buffer isn't dirty, else parks it behind a y/n prompt.
static void request_action(Confirm_Action action)
{
    if (editor.dirty) {
        pending_action = action;
        mode = APP_MODE_CONFIRM;
    } else {
        perform_action(action);
    }
}

static const char *confirm_message(Confirm_Action action)
{
    switch (action) {
    case CONFIRM_ACTION_OPEN_BROWSER:
        return "Unsaved changes! Discard and open the file browser? (y/n)";
    case CONFIRM_ACTION_NEW_FILE:
        return "Unsaved changes! Discard and start a new file? (y/n)";
    case CONFIRM_ACTION_QUIT:
        return "Unsaved changes! Quit without saving? (y/n)";
    }
    UNREACHABLE("confirm_message");
}

// Fixed zoom for screen-space UI overlays (the bottom bar), independent
// of the document's own camera.
#define UI_TEXT_SCALE 0.3125f
#define UI_BAR_HEIGHT_PX 36.0f
#define UI_TEXT_PADDING_PX 12.0f

// Points sr's camera at the fixed UI overlay space the status bar uses:
// (0,0) becomes the window's bottom-left corner, independent of the
// document's own pan/zoom. Caller must restore sr->camera_pos/camera_scale
// afterward (see draw_status_bar).
static void ui_bar_camera_begin(Simple_Renderer *sr, float *out_half_w, float *out_half_h)
{
    sr->camera_scale = UI_TEXT_SCALE;
    *out_half_w = sr->resolution.x / (2.0f * UI_TEXT_SCALE);
    *out_half_h = sr->resolution.y / (2.0f * UI_TEXT_SCALE);
    sr->camera_pos = vec2f(*out_half_w, *out_half_h);
}

// Single bar pinned to the bottom edge, always exactly UI_BAR_HEIGHT_PX
// pixels tall regardless of the document's own pan/zoom. Normally shows
// the permanent status (filename + dirty marker on the left, 1-based
// line:col on the right); a confirm/save-as/command-line prompt or a
// flashed error takes it over entirely instead of stacking a second bar,
// since only one of those is ever relevant at a time.
static void draw_status_bar(Simple_Renderer *sr, Free_Glyph_Atlas *atlas)
{
    Vec2f saved_camera_pos = sr->camera_pos;
    float saved_camera_scale = sr->camera_scale;

    float half_w, half_h;
    ui_bar_camera_begin(sr, &half_w, &half_h);

    float bar_h = UI_BAR_HEIGHT_PX / UI_TEXT_SCALE;
    float pad = UI_TEXT_PADDING_PX / UI_TEXT_SCALE;
    Vec4f fg = vec4fs(1.0f);
    Vec4f bg = hex_to_vec4f(0x252525FF);

    char left[SAVE_AS_PATH_CAP + 32];
    const char *right = NULL;
    char right_buf[64];
    int right_len = 0;
    Vec2f right_measure = vec2fs(0.0f);

    if (mode == APP_MODE_CONFIRM) {
        bg = hex_to_vec4f(0x4A1D1DFF);
        snprintf(left, sizeof(left), "%s", confirm_message(pending_action));
    } else if (mode == APP_MODE_SAVE_AS) {
        bg = hex_to_vec4f(0x1D2E4AFF);
        snprintf(left, sizeof(left), "Save as: %s_", save_as_path);
    } else if (mode == APP_MODE_COMMAND) {
        bg = hex_to_vec4f(0x1D2E4AFF);
        snprintf(left, sizeof(left), ":%s_", command_line);
    } else if (error_message[0] != '\0' && SDL_GetTicks() - error_message_time < ERROR_DISPLAY_MS) {
        bg = hex_to_vec4f(0x4A1D1DFF);
        snprintf(left, sizeof(left), "%s", error_message);
    } else {
        size_t row = editor_cursor_row(&editor);
        Line line = editor.lines.items[row];
        right_len = snprintf(right_buf, sizeof(right_buf), "Ln %zu, Col %zu", row + 1, editor.cursor - line.begin + 1);
        right = right_buf;
        free_glyph_atlas_measure_line_sized(atlas, right, (size_t) right_len, &right_measure);

        // Elide the path down to just its filename (with a ".../"
        // lead-in) if the full path would otherwise run into line:col.
        const char *path = editor.file_path.count > 0 ? editor.file_path.items : "[No Name]";
        const char *dirty_marker = editor.dirty ? " [+]" : "";
        snprintf(left, sizeof(left), "%s%s", path, dirty_marker);

        Vec2f left_measure = vec2fs(0.0f);
        free_glyph_atlas_measure_line_sized(atlas, left, strlen(left), &left_measure);
        float available = 2.0f * half_w - 2.0f * pad - right_measure.x - pad;
        if (left_measure.x > available) {
            const char *base = strrchr(path, '/');
            base = base ? base + 1 : path;
            snprintf(left, sizeof(left), ".../%s%s", base, dirty_marker);
        }
    }

    simple_renderer_set_shader(sr, SHADER_FOR_COLOR);
    simple_renderer_solid_rect(sr, vec2f(0.0f, 0.0f), vec2f(2.0f * half_w, bar_h), bg);
    simple_renderer_flush(sr);

    simple_renderer_set_shader(sr, SHADER_FOR_TEXT);
    float text_y = bar_h * 0.28f;

    Vec2f left_pos = vec2f(pad, text_y);
    free_glyph_atlas_render_line_sized(atlas, sr, left, strlen(left), &left_pos, fg);

    if (right != NULL) {
        Vec2f right_pos = vec2f(2.0f * half_w - pad - right_measure.x, text_y);
        free_glyph_atlas_render_line_sized(atlas, sr, right, (size_t) right_len, &right_pos, fg);
    }
    simple_renderer_flush(sr);

    sr->camera_pos = saved_camera_pos;
    sr->camera_scale = saved_camera_scale;
}

// Commands - see command.h. Movement commands don't call
// editor_update_selection themselves; the dispatcher does that once, generically.

static void cmd_move_line_begin(void)
{
    editor_flush_group(&editor);
    editor_move_to_line_begin(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_buffer_begin(void)
{
    editor_flush_group(&editor);
    editor_move_to_begin(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_line_end(void)
{
    editor_flush_group(&editor);
    editor_move_to_line_end(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_buffer_end(void)
{
    editor_flush_group(&editor);
    editor_move_to_end(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_char_left(void)
{
    editor_flush_group(&editor);
    editor_move_char_left(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_word_left(void)
{
    editor_flush_group(&editor);
    editor_move_word_left(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_char_right(void)
{
    editor_flush_group(&editor);
    editor_move_char_right(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_word_right(void)
{
    editor_flush_group(&editor);
    editor_move_word_right(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_line_up(void)
{
    editor_flush_group(&editor);
    editor_move_line_up(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_paragraph_up(void)
{
    editor_flush_group(&editor);
    editor_move_paragraph_up(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_line_down(void)
{
    editor_flush_group(&editor);
    editor_move_line_down(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_paragraph_down(void)
{
    editor_flush_group(&editor);
    editor_move_paragraph_down(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_backspace(void)
{
    editor_backspace(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_delete_forward(void)
{
    editor_delete(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_save(void)
{
    if (editor.file_path.count > 0) {
        editor_flush_group(&editor);
        Errno err = editor_save(&editor);
        if (err != 0) {
            flash_error("Could not save currently edited file: %s", strerror(err));
        } else {
            editor.dirty = false;
        }
    } else {
        editor_flush_group(&editor);
        save_as_path[0] = '\0';
        save_as_path_len = 0;
        mode = APP_MODE_SAVE_AS;
    }
}

static void cmd_save_as(void)
{
    editor_flush_group(&editor);
    save_as_path[0] = '\0';
    save_as_path_len = 0;
    if (editor.file_path.count > 0) {
        snprintf(save_as_path, SAVE_AS_PATH_CAP, "%s", editor.file_path.items);
        save_as_path_len = strlen(save_as_path);
    }
    mode = APP_MODE_SAVE_AS;
}

static void cmd_open_file_browser(void)
{
    request_action(CONFIRM_ACTION_OPEN_BROWSER);
}

static void cmd_new_file(void)
{
    request_action(CONFIRM_ACTION_NEW_FILE);
}

static void cmd_quit(void)
{
    request_action(CONFIRM_ACTION_QUIT);
}

static void cmd_reload_shaders(void)
{
    simple_renderer_reload_shaders(&sr);
}

static void cmd_undo(void)
{
    editor_undo(&editor);
}

static void cmd_redo(void)
{
    editor_redo(&editor);
}

static void cmd_confirm_line(void)
{
    if (editor.searching) {
        editor_flush_group(&editor);
        editor_stop_search(&editor);
    } else {
        editor_insert_char(&editor, '\n');
        editor.last_stroke = SDL_GetTicks();
    }
}

static void cmd_start_search(void)
{
    editor_flush_group(&editor);
    editor_start_search(&editor);
}

// ':' from Vim's Normal mode only - Insert/Visual just type the literal
// character, and incremental search (started independently of Vim's
// mode, via Ctrl+F) gets first refusal on all keys of its own.
static void cmd_start_command_line(void)
{
    if (!vim_enabled || editor.searching || vim.mode != VIM_MODE_NORMAL) return;
    editor_flush_group(&editor);
    command_line[0] = '\0';
    command_line_len = 0;
    mode = APP_MODE_COMMAND;
}

// The handful of Ex commands this editor understands - :w/:q/:wq/:q!,
// all reusing the same save/quit paths as their keybindings (F2, Ctrl+Q).
// :%s substitution is deliberately not here yet; it needs a find/replace
// engine this codebase doesn't have (see doc/ROADMAP.md).
static void execute_command_line(const char *cmd)
{
    if (strcmp(cmd, "w") == 0) {
        cmd_save();
    } else if (strcmp(cmd, "wq") == 0) {
        cmd_save();
        if (!editor.dirty) request_action(CONFIRM_ACTION_QUIT);
    } else if (strcmp(cmd, "q") == 0) {
        request_action(CONFIRM_ACTION_QUIT);
    } else if (strcmp(cmd, "q!") == 0) {
        quit = true;
    } else if (cmd[0] != '\0') {
        flash_error("Not an editor command: :%s", cmd);
    }
}

// Shift+Escape while both searching and holding a selection no longer
// clears that selection (the dispatcher now resolves Shift before this runs).
static void cmd_escape(void)
{
    editor_flush_group(&editor);
    editor_stop_search(&editor);
}

static void cmd_select_all(void)
{
    editor_flush_group(&editor);
    editor.selection = true;
    editor.select_begin = 0;
    editor.cursor = editor.data.count;
}

static void cmd_indent(void)
{
    editor_indent(&editor);
}

static void cmd_unindent(void)
{
    editor_unindent(&editor);
}

static void cmd_copy(void)
{
    editor_flush_group(&editor);
    editor_clipboard_copy(&editor);
}

static void cmd_cut(void)
{
    editor_flush_group(&editor);
    editor_clipboard_cut(&editor);
}

static void cmd_paste(void)
{
    editor_flush_group(&editor);
    editor_clipboard_paste(&editor);
}

// Not bound to any default chord - opt in via `bind <chord> =
// toggle-vim-mode` in dede.conf. See vim_enabled's own comment.
static void cmd_toggle_vim_mode(void)
{
    vim_enabled = !vim_enabled;
    if (vim_enabled) {
        vim = vim_state_init();
        editor.cursor_block = true;
    } else {
        editor.cursor_block = false;
        editor.selection = false;
    }
}

typedef struct {
    const char *name;
    Command_Fn fn;
    bool shift_extends_selection;
} Command_Def;

// Commands that must not run outside Insert mode (Enter/Backspace/Delete/
// Tab skip vim.c's own screening since they raise no SDL_TEXTINPUT).
static bool vim_blocks_command(const char *name)
{
    static const char *blocked[] = {
        "confirm-line", "backspace", "delete-forward", "indent", "unindent",
    };
    for (size_t i = 0; i < ARRAY_LEN(blocked); ++i) {
        if (strcmp(name, blocked[i]) == 0) return true;
    }
    return false;
}

static const Command_Def default_commands[] = {
    {"move-line-begin",     cmd_move_line_begin,     true},
    {"move-buffer-begin",   cmd_move_buffer_begin,   true},
    {"move-line-end",       cmd_move_line_end,       true},
    {"move-buffer-end",     cmd_move_buffer_end,     true},
    {"move-char-left",      cmd_move_char_left,      true},
    {"move-word-left",      cmd_move_word_left,      true},
    {"move-char-right",     cmd_move_char_right,     true},
    {"move-word-right",     cmd_move_word_right,     true},
    {"move-line-up",        cmd_move_line_up,        true},
    {"move-paragraph-up",   cmd_move_paragraph_up,   true},
    {"move-line-down",      cmd_move_line_down,      true},
    {"move-paragraph-down", cmd_move_paragraph_down, true},
    {"escape",              cmd_escape,              true},

    {"backspace",           cmd_backspace,           false},
    {"delete-forward",      cmd_delete_forward,      false},
    {"save",                cmd_save,                false},
    {"save-as",             cmd_save_as,             false},
    {"open-file-browser",   cmd_open_file_browser,   false},
    {"new-file",            cmd_new_file,            false},
    {"quit",                cmd_quit,                false},
    {"reload-shaders",      cmd_reload_shaders,      false},
    {"undo",                cmd_undo,                false},
    {"redo",                cmd_redo,                false},
    {"confirm-line",        cmd_confirm_line,        false},
    {"start-search",        cmd_start_search,        false},
    {"start-command-line",  cmd_start_command_line,  false},
    {"select-all",          cmd_select_all,          false},
    {"indent",              cmd_indent,              false},
    {"unindent",            cmd_unindent,            false},
    {"copy",                cmd_copy,                false},
    {"cut",                 cmd_cut,                 false},
    {"paste",               cmd_paste,               false},
    {"toggle-vim-mode",     cmd_toggle_vim_mode,     false},
};

typedef struct {
    const char *chord;
    const char *command;
} Keybind_Def;

// The built-in keymap. Every entry a movement/escape command needs is
// listed twice (bare and Shift-prefixed) since shift_extends_selection
// is handled generically by the dispatcher rather than by matching a
// "don't care about Shift" wildcard chord - see command.h.
//
// Three spots deliberately do NOT replicate a modifier-agnostic quirk
// the original hardcoded switch had (nobody chose it - the switch
// simply never bothered checking a modifier it didn't need):
// Backspace/Delete/Return/F2/F3/F5 no longer also fire with Ctrl/Shift
// incidentally held; Escape no longer also fires on Ctrl+Escape;
// Tab/Shift+Tab no longer also fire on Ctrl+Tab/Ctrl+Shift+Tab (frees
// those up for a future tab-switching feature instead of indenting).
static const Keybind_Def default_keybindings[] = {
    {"home",             "move-line-begin"},
    {"shift+home",       "move-line-begin"},
    {"ctrl+home",        "move-buffer-begin"},
    {"ctrl+shift+home",  "move-buffer-begin"},
    {"end",              "move-line-end"},
    {"shift+end",        "move-line-end"},
    {"ctrl+end",         "move-buffer-end"},
    {"ctrl+shift+end",   "move-buffer-end"},
    {"left",             "move-char-left"},
    {"shift+left",       "move-char-left"},
    {"ctrl+left",        "move-word-left"},
    {"ctrl+shift+left",  "move-word-left"},
    {"right",            "move-char-right"},
    {"shift+right",      "move-char-right"},
    {"ctrl+right",       "move-word-right"},
    {"ctrl+shift+right", "move-word-right"},
    {"up",               "move-line-up"},
    {"shift+up",         "move-line-up"},
    {"ctrl+up",          "move-paragraph-up"},
    {"ctrl+shift+up",    "move-paragraph-up"},
    {"down",             "move-line-down"},
    {"shift+down",       "move-line-down"},
    {"ctrl+down",        "move-paragraph-down"},
    {"ctrl+shift+down",  "move-paragraph-down"},
    {"escape",           "escape"},
    {"shift+escape",     "escape"},

    {"backspace",        "backspace"},
    {"delete",           "delete-forward"},
    {"f2",               "save"},
    {"ctrl+shift+s",     "save-as"},
    {"f3",               "open-file-browser"},
    {"ctrl+n",           "new-file"},
    {"ctrl+q",           "quit"},
    {"f5",               "reload-shaders"},
    {"ctrl+z",           "undo"},
    {"ctrl+shift+z",     "redo"},
    {"ctrl+y",           "redo"},
    {"return",           "confirm-line"},
    {"ctrl+f",           "start-search"},
    {"shift+;",          "start-command-line"},
    {"ctrl+a",           "select-all"},
    {"tab",              "indent"},
    {"shift+tab",        "unindent"},
    {"ctrl+c",           "copy"},
    {"ctrl+x",           "cut"},
    {"ctrl+v",           "paste"},
};

void app_register_commands(void)
{
    for (size_t i = 0; i < ARRAY_LEN(default_commands); ++i) {
        const Command_Def *def = &default_commands[i];
        command_register(def->name, def->fn, def->shift_extends_selection);
    }
    for (size_t i = 0; i < ARRAY_LEN(default_keybindings); ++i) {
        const Keybind_Def *def = &default_keybindings[i];
        bool ok = keymap_bind(def->chord, def->command);
        assert(ok && "a built-in keybinding failed to bind - this is a bug in default_keybindings/default_commands itself, not a runtime condition");
    }
}

int app_run(SDL_Window *window, FT_Face face, Config *cfg, int argc, char **argv)
{
    simple_renderer_init(&sr);
    free_glyph_atlas_init(&atlas, face);

    editor.indent_width = cfg->tab_width;
    editor.line_numbers = cfg->line_numbers;
    editor.relative_line_numbers = cfg->relative_line_numbers;
    vim_enabled = cfg->vim_mode;

    Errno err;
    if (argc > 1) {
        const char *file_path = argv[1];
        err = editor_load_from_file(&editor, file_path);
        if (err != 0) {
            fprintf(stderr, "ERROR: Could not read file %s: %s\n", file_path, strerror(err));
            return 1;
        }
    }

    const char *dir_path = ".";
    err = fb_open_dir(&fb, dir_path);
    if (err != 0) {
        fprintf(stderr, "ERROR: Could not read directory %s: %s\n", dir_path, strerror(err));
        return 1;
    }

    editor.atlas = &atlas;
    editor_retokenize(&editor);

    vim = vim_state_init();
    editor.cursor_block = vim_enabled;

    while (!quit) {
        const Uint32 start = SDL_GetTicks();
        SDL_Event event = {0};
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT: {
                request_action(CONFIRM_ACTION_QUIT);
            }
            break;

            case SDL_KEYDOWN: {
                App_Mode mode_before_key = mode;
                switch (mode) {
                case APP_MODE_FILE_BROWSER: {
                    if (!(vim_enabled && vim_handle_browser_key(&vim, &fb, event.key.keysym))) {
                        switch (event.key.keysym.sym) {
                        case SDLK_F3: {
                            mode = APP_MODE_EDITOR;
                        }
                        break;

                        case SDLK_UP: {
                            if (fb.cursor > 0) fb.cursor -= 1;
                        }
                        break;

                        case SDLK_DOWN: {
                            if (fb.cursor + 1 < fb.files.count) fb.cursor += 1;
                        }
                        break;

                        case SDLK_RETURN: {
                            const char *file_path = fb_file_path(&fb);
                            if (file_path) {
                                File_Type ft;
                                err = type_of_file(file_path, &ft);
                                if (err != 0) {
                                    flash_error("Could not determine type of file %s: %s", file_path, strerror(err));
                                } else {
                                    switch (ft) {
                                    case FT_DIRECTORY: {
                                        err = fb_change_dir(&fb);
                                        if (err != 0) {
                                            flash_error("Could not change directory to %s: %s", file_path, strerror(err));
                                        }
                                    }
                                    break;

                                    case FT_REGULAR: {
                                        // F3 already confirmed discarding unsaved changes.
                                        err = editor_load_from_file(&editor, file_path);
                                        if (err != 0) {
                                            flash_error("Could not open file %s: %s", file_path, strerror(err));
                                        } else {
                                            vim = vim_state_init();
                                            editor.cursor_block = vim_enabled;
                                            mode = APP_MODE_EDITOR;
                                        }
                                    }
                                    break;

                                    case FT_OTHER: {
                                        flash_error("%s is neither a regular file nor a directory. We can't open it.", file_path);
                                    }
                                    break;

                                    default:
                                        UNREACHABLE("unknown File_Type");
                                    }
                                }
                            }
                        }
                        break;
                        }
                    }
                } break;

                case APP_MODE_CONFIRM: {
                    switch (event.key.keysym.sym) {
                    case SDLK_y:
                        perform_action(pending_action);
                        break;

                    // Only explicit 'y' confirms discarding work. Enter is
                    // deliberately NOT an alias for it - this prompt exists
                    // specifically to catch a moment where Enter is easy to
                    // hit out of habit (mid-edit, or closing the window),
                    // so it has to fall back to the safe (cancel) option
                    // like 'n'/Escape do, not the destructive one.
                    case SDLK_n:
                    case SDLK_RETURN:
                    case SDLK_ESCAPE:
                        mode = APP_MODE_EDITOR;
                        break;

                    default:
                        break;
                    }
                } break;

                case APP_MODE_SAVE_AS: {
                    switch (event.key.keysym.sym) {
                    case SDLK_RETURN: {
                        if (save_as_path_len > 0) {
                            editor_flush_group(&editor);
                            err = editor_save_as(&editor, save_as_path);
                            if (err != 0) {
                                flash_error("Could not save as %s: %s", save_as_path, strerror(err));
                                // stay in the prompt so the path can be fixed
                            } else {
                                mode = APP_MODE_EDITOR;
                            }
                        }
                    }
                    break;

                    case SDLK_ESCAPE: {
                        mode = APP_MODE_EDITOR;
                    }
                    break;

                    case SDLK_BACKSPACE: {
                        backspace_utf8(save_as_path, &save_as_path_len);
                    }
                    break;

                    default:
                        break;
                    }
                } break;

                case APP_MODE_COMMAND: {
                    switch (event.key.keysym.sym) {
                    case SDLK_RETURN: {
                        execute_command_line(command_line);
                        // Only close the prompt if execute_command_line
                        // didn't already switch to something else itself
                        // (e.g. :q on a dirty buffer parks in APP_MODE_CONFIRM).
                        if (mode == APP_MODE_COMMAND) mode = APP_MODE_EDITOR;
                    }
                    break;

                    case SDLK_ESCAPE: {
                        mode = APP_MODE_EDITOR;
                    }
                    break;

                    case SDLK_BACKSPACE: {
                        backspace_utf8(command_line, &command_line_len);
                    }
                    break;

                    default:
                        break;
                    }
                } break;

                case APP_MODE_EDITOR: {
                    if (vim_enabled && vim_handle_key(&vim, &editor, event.key.keysym)) {
                        editor.last_stroke = SDL_GetTicks();
                    } else {
                        const Command *cmd = keymap_resolve(event.key.keysym);
                        // Exempt while searching: vim.c itself always yields then too.
                        bool blocked = cmd != NULL && vim_enabled && !editor.searching &&
                                       vim.mode != VIM_MODE_INSERT && vim_blocks_command(cmd->name);
                        if (cmd != NULL && !blocked) {
                            if (cmd->shift_extends_selection) {
                                bool extend = (event.key.keysym.mod & KMOD_SHIFT) ||
                                              (vim_enabled && vim.mode == VIM_MODE_VISUAL);
                                editor_update_selection(&editor, extend);
                            }
                            cmd->fn();
                        }
                    }

                    // Keep Vim's mode in sync with editor.selection in case
                    // a non-vim.c command (undo/redo, select-all) changed it.
                    if (vim_enabled) {
                        if (vim.mode == VIM_MODE_VISUAL && !editor.selection) {
                            vim.mode = VIM_MODE_NORMAL;
                            vim.visual_linewise = false;
                            editor.cursor_block = true;
                        } else if (vim.mode == VIM_MODE_NORMAL && editor.selection) {
                            vim.mode = VIM_MODE_VISUAL;
                            vim.visual_linewise = false;
                        }
                    }
                } break;
                }
                app_mode_changed_this_key = (mode != mode_before_key);
            }
            break;

            case SDL_TEXTINPUT: {
                if (app_mode_changed_this_key) {
                    app_mode_changed_this_key = false;
                    // See the flag's own comment: this key just switched
                    // which app-level mode we're in, so its trailing text
                    // event belongs to the mode we left and must be
                    // swallowed no matter which mode we're now in.
                } else
                switch (mode) {
                case APP_MODE_FILE_BROWSER:
                    // Nothing for now
                    // Once we have incremental search in the file browser this may become useful
                    break;

                case APP_MODE_CONFIRM:
                    // y/n is handled entirely in SDL_KEYDOWN above.
                    break;

                case APP_MODE_SAVE_AS: {
                    const char *text = event.text.text;
                    size_t text_len = strlen(text);
                    for (size_t i = 0; i < text_len && save_as_path_len + 1 < SAVE_AS_PATH_CAP; ++i) {
                        save_as_path[save_as_path_len++] = text[i];
                    }
                    save_as_path[save_as_path_len] = '\0';
                }
                break;

                case APP_MODE_COMMAND: {
                    const char *text = event.text.text;
                    size_t text_len = strlen(text);
                    for (size_t i = 0; i < text_len && command_line_len + 1 < COMMAND_LINE_CAP; ++i) {
                        command_line[command_line_len++] = text[i];
                    }
                    command_line[command_line_len] = '\0';
                }
                break;

                case APP_MODE_EDITOR:
                    if (vim_take_consumed_textinput(&vim)) {
                        // key already claimed by vim_handle_key; don't also type it
                    } else if (!editor.searching && vim_enabled && (vim.mode == VIM_MODE_NORMAL || vim.mode == VIM_MODE_VISUAL)) {
                        // unbound letters in these modes are commands, not text
                    } else {
                        const char *text = event.text.text;
                        size_t text_len = strlen(text);
                        for (size_t i = 0; i < text_len; ++i) {
                            editor_insert_char(&editor, text[i]);
                        }
                        editor.last_stroke = SDL_GetTicks();
                    }
                    break;
                }
            }
            break;
            }
        }

        {
            int w, h;
            SDL_GetWindowSize(window, &w, &h);
            // TODO(#19): update the viewport and the resolution only on actual window change
            glViewport(0, 0, w, h);
        }

        Vec4f bg = hex_to_vec4f(0x181818FF);
        glClearColor(bg.x, bg.y, bg.z, bg.w);
        glClear(GL_COLOR_BUFFER_BIT);

        if (mode == APP_MODE_FILE_BROWSER) {
            fb_render(&fb, window, &atlas, &sr);
        } else {
            editor_render(window, &atlas, &sr, &editor);
            draw_status_bar(&sr, &atlas);
        }

        SDL_GL_SwapWindow(window);

        const Uint32 duration = SDL_GetTicks() - start;
        const Uint32 delta_time_ms = 1000 / FPS;
        if (duration < delta_time_ms) {
            SDL_Delay(delta_time_ms - duration);
        }
    }

    return 0;
}

// TODO: ability to search within file browser
// Very useful when you have a lot of files
// TODO: ability to remove trailing whitespaces
