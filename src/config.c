#include <string.h>
#include <errno.h>
#include <stdio.h>

#include "./config.h"
#include "./command.h"
#include "./editor.h"
#include "./sv.h"

Config config_default(void)
{
    Config cfg = {0};
    snprintf(cfg.font_path, sizeof(cfg.font_path), "%s", "./fonts/iosevka-regular.ttf");
    cfg.tab_width = 4;
    cfg.vim_mode = true;
    cfg.line_numbers = false;
    cfg.relative_line_numbers = false;
    return cfg;
}

// Copies a trimmed String_View into a small stack cstr buffer (`bind`
// lines need a NUL-terminated chord and command name to hand to
// keymap_bind, but a token's backing memory isn't necessarily
// terminated right after it - see command.c's parse_chord for the same
// concern). Returns false if it doesn't fit.
static bool sv_to_cstr(String_View sv, char *out, size_t out_cap)
{
    if (sv.count >= out_cap) return false;
    memcpy(out, sv.data, sv.count);
    out[sv.count] = '\0';
    return true;
}

static bool sv_to_bool(String_View sv, bool *out)
{
    if (sv_eq_ignorecase(sv, SV("true"))) { *out = true; return true; }
    if (sv_eq_ignorecase(sv, SV("false"))) { *out = false; return true; }
    return false;
}

static void config_apply_line(String_View line, size_t line_no, Config *cfg)
{
    line = sv_trim(line);
    if (line.count == 0 || line.data[0] == '#') return;

    if (sv_starts_with(line, SV("bind "))) {
        sv_chop_left(&line, 5); // discard the "bind " keyword itself
        String_View rest = sv_trim(line);
        String_View chord_sv = sv_trim(sv_chop_by_delim(&rest, '='));
        String_View command_sv = sv_trim(rest);

        char chord[64], command_name[64];
        if (!sv_to_cstr(chord_sv, chord, sizeof(chord)) ||
            !sv_to_cstr(command_sv, command_name, sizeof(command_name))) {
            fprintf(stderr, "config:%zu: 'bind' chord or command name too long, ignoring\n", line_no);
            return;
        }
        if (!keymap_bind(chord, command_name)) {
            fprintf(stderr, "config:%zu: could not bind '%s' to '%s' (unrecognized chord or command), ignoring\n",
                    line_no, chord, command_name);
        }
        return;
    }

    String_View key = sv_trim(sv_chop_by_delim(&line, '='));
    String_View value = sv_trim(line);

    if (sv_eq_ignorecase(key, SV("font"))) {
        if (!sv_to_cstr(value, cfg->font_path, sizeof(cfg->font_path))) {
            fprintf(stderr, "config:%zu: font path too long, ignoring\n", line_no);
        }
    } else if (sv_eq_ignorecase(key, SV("tab_width"))) {
        size_t width = (size_t) sv_to_u64(value);
        if (width == 0 || width > EDITOR_MAX_INDENT_WIDTH) {
            fprintf(stderr, "config:%zu: tab_width must be between 1 and %d, ignoring\n", line_no, EDITOR_MAX_INDENT_WIDTH);
        } else {
            cfg->tab_width = width;
        }
    } else if (sv_eq_ignorecase(key, SV("vim_mode"))) {
        if (!sv_to_bool(value, &cfg->vim_mode)) {
            fprintf(stderr, "config:%zu: vim_mode must be true or false, ignoring\n", line_no);
        }
    } else if (sv_eq_ignorecase(key, SV("line_numbers"))) {
        if (!sv_to_bool(value, &cfg->line_numbers)) {
            fprintf(stderr, "config:%zu: line_numbers must be true or false, ignoring\n", line_no);
        }
    } else if (sv_eq_ignorecase(key, SV("relative_line_numbers"))) {
        if (!sv_to_bool(value, &cfg->relative_line_numbers)) {
            fprintf(stderr, "config:%zu: relative_line_numbers must be true or false, ignoring\n", line_no);
        }
    } else {
        fprintf(stderr, "config:%zu: unknown setting '"SV_Fmt"', ignoring\n", line_no, SV_Arg(key));
    }
}

Errno config_load(const char *path, Config *cfg)
{
    String_Builder sb = {0};
    Errno err = read_entire_file(path, &sb);
    if (err != 0) {
        free(sb.items);
        return err == ENOENT ? 0 : err;
    }

    String_View content = sb_to_sv(sb);
    size_t line_no = 0;
    while (content.count > 0) {
        line_no += 1;
        String_View line = sv_chop_by_delim(&content, '\n');
        config_apply_line(line, line_no, cfg);
    }

    free(sb.items);
    return 0;
}
