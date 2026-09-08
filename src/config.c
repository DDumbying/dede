#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include "./config.h"
#include "./command.h"
#include "./editor.h"
#include "./sv.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

Config config_default(void)
{
    Config cfg = {0};
    snprintf(cfg.font_path, sizeof(cfg.font_path), "%s", "./fonts/iosevka-regular.ttf");
    cfg.tab_width = 4;
    cfg.vim_mode = true;
    cfg.line_numbers = false;
    cfg.relative_line_numbers = false;

    // The theme's built-in defaults are exactly the hex/color literals
    // this codebase used before theme.<name> config keys existed - so a
    // config file with no [theme-ish] lines in it renders identically to
    // before this feature landed.
    cfg.theme = (Theme) {
        .bg = 0x181818FF,
        .fg = 0xFFFFFFFF,
        .selection = 0x404040FF,
        .search_current = 0x1A1A40FF,
        .search_other = 0x1A1A2EFF,
        .gutter_bg = 0x212121FF,
        .gutter_fg = 0x808080FF,
        .accent = 0xFFDD33FF,
        .comment = 0xCC8C3CFF,
        .string = 0x73C936FF,
        .preproc = 0x95A99FFF,
        .error_text = 0xFF6B6BFF,
        .ui_bar_bg = 0x252525FF,
        .ui_bar_prompt_bg = 0x1D2E4AFF,
        .ui_bar_error_bg = 0x4A1D1DFF,
        .divider = 0x2A2A2AFF,
    };
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

// Parses a "RRGGBB" or "RRGGBBAA" hex color (an optional leading '#' is
// allowed, to accept colors copied from most color pickers/terminal
// themes) into hex_to_vec4f's 0xRRGGBBAA encoding. A 6-digit value gets
// an implied opaque (FF) alpha. Returns false, leaving *out untouched,
// for anything else - wrong length or non-hex characters.
static bool sv_to_hex_color(String_View sv, uint32_t *out)
{
    if (sv.count > 0 && sv.data[0] == '#') sv_chop_left(&sv, 1);
    if (sv.count != 6 && sv.count != 8) return false;

    char buf[9];
    memcpy(buf, sv.data, sv.count);
    buf[sv.count] = '\0';

    char *end;
    unsigned long value = strtoul(buf, &end, 16);
    if (*end != '\0') return false;

    if (sv.count == 6) value = (value << 8) | 0xFF;
    *out = (uint32_t) value;
    return true;
}

// Maps a config key (e.g. "bg") to the matching Theme field's offset
// within Config, so config_apply_line can handle all sixteen theme
// colors with one lookup instead of sixteen near-identical else-ifs.
typedef struct {
    const char *key;
    size_t offset;
} Theme_Field;

#define THEME_FIELD(name) { #name, offsetof(Config, theme.name) }
static const Theme_Field theme_fields[] = {
    THEME_FIELD(bg),
    THEME_FIELD(fg),
    THEME_FIELD(selection),
    THEME_FIELD(search_current),
    THEME_FIELD(search_other),
    THEME_FIELD(gutter_bg),
    THEME_FIELD(gutter_fg),
    THEME_FIELD(accent),
    THEME_FIELD(comment),
    THEME_FIELD(string),
    THEME_FIELD(preproc),
    THEME_FIELD(error_text),
    THEME_FIELD(ui_bar_bg),
    THEME_FIELD(ui_bar_prompt_bg),
    THEME_FIELD(ui_bar_error_bg),
    THEME_FIELD(divider),
};

// Looks up `key` in theme_fields and, if found, parses `value` as a hex
// color into that field of `*cfg`. Returns false if `key` doesn't name
// any theme field at all (the caller's cue to report "unknown setting"
// instead of a parse error) - true either way once a field matched,
// whether or not `value` itself was valid (a bad color is reported here
// and skipped, same as every other setting's own parse failure).
static bool config_apply_theme_line(String_View key, String_View value, size_t line_no, Config *cfg)
{
    for (size_t i = 0; i < ARRAY_LEN(theme_fields); ++i) {
        if (sv_eq_ignorecase(key, sv_from_cstr(theme_fields[i].key))) {
            uint32_t color;
            if (sv_to_hex_color(value, &color)) {
                *(uint32_t *) ((char *) cfg + theme_fields[i].offset) = color;
            } else {
                fprintf(stderr, "config:%zu: '%s' must be a 6- or 8-digit hex color (e.g. 282828 or 282828ff), ignoring\n",
                        line_no, theme_fields[i].key);
            }
            return true;
        }
    }
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
    } else if (!config_apply_theme_line(key, value, line_no, cfg)) {
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
