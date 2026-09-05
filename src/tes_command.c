// Headless test for the command registry, keymap, and config loader
// (src/command.c, src/config.c). No SDL_Init/window needed - keymap
// resolution only touches a plain SDL_Keysym struct, and config_load
// only touches the filesystem via read_entire_file.

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include "./command.h"
#include "./config.h"
#include "./common.h"

static int failures = 0;

#define CHECK(desc, cond) do { \
    if (cond) { \
        printf("ok:   %s\n", desc); \
    } else { \
        printf("FAIL: %s\n", desc); \
        failures++; \
    } \
} while (0)

#define KEY(SYM, MODF) (SDL_Keysym){.scancode = 0, .sym = (SYM), .mod = (MODF), .unused = 0}

static int noop_calls = 0;
static void cmd_noop_a(void) { noop_calls += 1; }
static void cmd_noop_b(void) { noop_calls += 10; }

int main(void)
{
    printf("--- command registry + keymap ---\n");

    command_register("test.a", cmd_noop_a, false);
    command_register("test.b", cmd_noop_b, true);

    CHECK("command_find finds a registered command", command_find("test.a") != NULL);
    CHECK("command_find returns NULL for an unregistered name", command_find("test.nope") == NULL);
    CHECK("shift_extends_selection is stored as registered", command_find("test.b")->shift_extends_selection);
    CHECK("shift_extends_selection defaults false when registered false", !command_find("test.a")->shift_extends_selection);

    CHECK("keymap_bind succeeds for a plain key", keymap_bind("f2", "test.a"));
    CHECK("keymap_bind succeeds for ctrl+key", keymap_bind("ctrl+a", "test.a"));
    CHECK("keymap_bind succeeds for ctrl+shift+key", keymap_bind("ctrl+shift+s", "test.b"));
    CHECK("keymap_bind accepts modifiers in either case", keymap_bind("Ctrl+Shift+n", "test.b"));

    CHECK("keymap_bind fails for an unregistered command", !keymap_bind("f3", "test.nonexistent"));
    CHECK("keymap_bind fails for an unrecognized key name", !keymap_bind("not-a-real-key", "test.a"));
    CHECK("keymap_bind fails for an unrecognized modifier name", !keymap_bind("meta+a", "test.a"));
    CHECK("keymap_bind fails for an empty chord", !keymap_bind("", "test.a"));
    CHECK("keymap_bind fails for a chord with only modifiers, no key", !keymap_bind("ctrl+shift", "test.a"));

    const Command *resolved = keymap_resolve(KEY(SDLK_F2, KMOD_NONE));
    CHECK("keymap_resolve finds the plain-key binding", resolved == command_find("test.a"));

    resolved = keymap_resolve(KEY(SDLK_a, KMOD_LCTRL));
    CHECK("keymap_resolve finds a ctrl+key binding with left ctrl", resolved == command_find("test.a"));
    resolved = keymap_resolve(KEY(SDLK_a, KMOD_RCTRL));
    CHECK("keymap_resolve finds a ctrl+key binding with right ctrl", resolved == command_find("test.a"));

    resolved = keymap_resolve(KEY(SDLK_a, KMOD_NONE));
    CHECK("keymap_resolve does NOT match ctrl+a for bare 'a' (no binding for that chord)", resolved == NULL);

    resolved = keymap_resolve(KEY(SDLK_s, KMOD_LCTRL | KMOD_LSHIFT));
    CHECK("keymap_resolve finds a ctrl+shift+key binding", resolved == command_find("test.b"));
    resolved = keymap_resolve(KEY(SDLK_s, KMOD_LCTRL));
    CHECK("keymap_resolve does NOT match ctrl+shift+s for bare ctrl+s", resolved == NULL);

    // Rebinding the same chord replaces it rather than adding a second,
    // shadowed entry - this is what lets a config `bind` override a
    // built-in default.
    CHECK("rebinding f2 to a different command succeeds", keymap_bind("f2", "test.b"));
    resolved = keymap_resolve(KEY(SDLK_F2, KMOD_NONE));
    CHECK("f2 now resolves to the new command, not the old one", resolved == command_find("test.b"));

    noop_calls = 0;
    resolved = keymap_resolve(KEY(SDLK_a, KMOD_LCTRL));
    resolved->fn();
    CHECK("resolved command's fn actually runs", noop_calls == 1);

    printf("\n--- config ---\n");

    Config cfg = config_default();
    CHECK("config_default sets a non-empty font path", cfg.font_path[0] != '\0');
    CHECK("config_default sets tab_width to 4", cfg.tab_width == 4);
    CHECK("config_default enables vim_mode", cfg.vim_mode);
    CHECK("config_default disables line_numbers", !cfg.line_numbers);
    CHECK("config_default disables relative_line_numbers", !cfg.relative_line_numbers);

    Errno err = config_load("/nonexistent/path/that/should/not/exist.conf", &cfg);
    CHECK("config_load on a missing file returns success (defaults stand)", err == 0);
    CHECK("config_load on a missing file leaves tab_width untouched", cfg.tab_width == 4);

    const char *tmp_path = "/tmp/ded_tes_command_config.conf";
    FILE *f = fopen(tmp_path, "w");
    assert(f != NULL && "could not create temp config file for the test");
    fprintf(f,
        "# a comment\n"
        "\n"
        "font = ./fonts/VictorMono-Regular.ttf\n"
        "tab_width = 2\n"
        "vim_mode = false\n"
        "line_numbers = true\n"
        "relative_line_numbers = TRUE\n"
        "bind ctrl+shift+q = test.a\n"
        "tab_width = 999\n"       // out of range - should warn and be ignored
        "not_a_real_setting = 1\n" // unknown key - should warn and be ignored
    );
    fclose(f);

    Config file_cfg = config_default();
    err = config_load(tmp_path, &file_cfg);
    CHECK("config_load on a real file returns success", err == 0);
    CHECK("config_load applies font override", strcmp(file_cfg.font_path, "./fonts/VictorMono-Regular.ttf") == 0);
    CHECK("config_load applies tab_width override", file_cfg.tab_width == 2);
    CHECK("config_load ignores an out-of-range tab_width, keeping the last valid one", file_cfg.tab_width == 2);
    CHECK("config_load applies vim_mode = false", !file_cfg.vim_mode);
    CHECK("config_load applies line_numbers = true", file_cfg.line_numbers);
    CHECK("config_load parses bool values case-insensitively", file_cfg.relative_line_numbers);

    resolved = keymap_resolve(KEY(SDLK_q, KMOD_LCTRL | KMOD_LSHIFT));
    CHECK("config_load's 'bind' line actually bound the chord", resolved == command_find("test.a"));

    remove(tmp_path);

    if (failures == 0) {
        printf("\nALL CHECKS PASSED\n");
        return 0;
    } else {
        printf("\n%d CHECK(S) FAILED\n", failures);
        return 1;
    }
}
