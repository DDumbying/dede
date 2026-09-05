# dede — Roadmap

What's left, in rough priority order. See [`CHANGELOG.md`](CHANGELOG.md) for what's already done and why, and [`NOTES.md`](NOTES.md) for known quirks and stray in-code TODOs.

## Main track

1. ~~**Unsaved-changes prompt**~~ / ~~**Save-as / new-file dialog**~~ — done, see CHANGELOG §2.6.
2. ~~**Config**~~ — font path, tab width, keybinding overrides, and Vim on/off all done, see CHANGELOG §2.8/§2.10. Theme/color config and the line-number gutter are the missing pieces (below).
3. **Line-number gutter rendering** — `Config`/`Editor` already have `line_numbers`/`relative_line_numbers` (§2.10); the actual gutter drawing in `editor_render` (plus its effect on cursor/camera math) is the remaining piece.
4. **Status bar** — line:col, filename, dirty indicator (a permanent one — §2.6's bottom bar only appears for prompts/errors, not standing status), adjustable horizontal scroll.
5. **Search improvements** — find next/prev, highlight all matches, replace / replace-all.
6. **File-browser search** — type to filter the file list (TODO in `src/app.c`).
7. **UTF-8 support** — the big correctness gap: the glyph atlas is ASCII-only (128 glyphs), and editing is byte-wise, so word jumps, Backspace, and highlighting all break on non-ASCII text.
8. **Editing polish** — delete word, replace.
9. **Multiple buffers / tabs**, split view, project-wide search, git integration.
10. **Mouse support** — click to place cursor, wheel scroll, drag to select.
11. **Theme/color config** — background, selection, syntax-highlight, and UI-bar colors are still hardcoded hex literals in `editor.c`/`app.c`; a natural next slice of the config file.
12. **Plugin/scripting layer** — deliberately not started: `command_register`/`keymap_bind` (§2.8) are the seam it would attach to (a script registers its own commands and binds keys to them, the same way `app.c`'s own defaults do), and §2.10's `vim_enabled` pattern is the model for making a scripting layer itself optional/toggleable too - but there's no embedded language yet. Lua is the natural choice if/when this is worth doing (small, fast, trivial to vendor, and the proven model for exactly this kind of editor).

## Vim modal editing track

See `src/vim.h`'s header comment for the authoritative scope notes.

1. ~~**Counts**~~ / ~~**Visual mode**~~ — done, see CHANGELOG §2.9.
2. **Text objects** — `iw`, `aw`, `ci(`, `di"`, etc. Needs a notion of "object under cursor", not just "motion from cursor".
3. **Linewise motions composed with operators** — `dj`, `dk`, `dgg`, `dG`. Blocked on generalizing `vim_resolve_motion` (currently only single `(buffer, pos) -> pos` motions) to express linewise ranges.
4. **Registers** — named (`"ay`) and numbered, instead of always going through the one system clipboard.
5. **Authentic linewise put** — `p`/`P` after a linewise yank should insert a new line below/above, not paste charwise at the cursor.
6. **Command-line mode** — `:w`, `:q`, `:%s/.../.../`.
7. **Macros and marks** — `qa...q`, `@a`, `` ` ``/`'`.
8. **A real Vim word model** — `w`/`b`/`e` currently use this codebase's existing alnum-vs-everything-else split; real Vim distinguishes punctuation runs from word runs too.
9. **Sticky column for `j`/`k`** — see NOTES.md; matters more once Vim's vertical motions are counted/composed.
