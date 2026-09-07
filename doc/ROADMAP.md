# dede — Roadmap

What's left, in rough priority order. See [`CHANGELOG.md`](CHANGELOG.md) for what's already done and why, and [`NOTES.md`](NOTES.md) for known quirks and stray in-code TODOs.

## Main track

1. ~~**Unsaved-changes prompt**~~ / ~~**Save-as / new-file dialog**~~ — done, see CHANGELOG §2.6.
2. ~~**Config**~~ / ~~**Line-number gutter rendering**~~ — font path, tab width, keybinding overrides, Vim on/off, and the gutter all done, see CHANGELOG §2.8/§2.10. Theme/color config is the missing piece (below). The gutter is pinned to the viewport's left edge via the live camera transform (so it doesn't pan away on a horizontally-scrolled line) but shares the document's own camera_scale for row alignment; `relative_line_numbers` shows distance-from-cursor except on the cursor's own line.
3. ~~**Status bar**~~ — done: a single permanent bar (`draw_status_bar` in `app.c`), normally showing filename (`.../`-elided path plus a `[+]` dirty marker) on the left and 1-based `Ln, Col` on the right - a confirm/save-as/`:`-command prompt or a flashed error takes the same bar over entirely instead of stacking a second one, since only one is ever relevant at a time. Adjustable horizontal scroll is intentionally not part of this - the editor has no manual scroll control yet, only the automatic cursor-following camera; that'd be its own feature.
4. **Search improvements** — find next/prev, highlight all matches, replace / replace-all.
5. **File-browser search** — type to filter the file list (TODO in `src/app.c`).
6. ~~**UTF-8 support**~~ — done at the codepoint level: `common.h`'s `utf8_decode`/`utf8_is_continuation` back cursor motion, word motions, and Backspace/Delete in `editor.c` (all move/remove a whole character, never split mid-sequence); the glyph atlas (`free_glyph.c`) now loads any codepoint on demand into a shelf-packed 2048x2048 texture instead of a fixed 128-entry ASCII table, falling back to `'?'`/the font's own `.notdef` glyph rather than crashing on anything unsupported; the lexer glosses non-ASCII bytes into identifiers instead of fragmenting them. Not attempted: grapheme clusters (combining marks, ZWJ emoji), RTL/bidi, complex script shaping - out of scope for a simple editor, see `src/free_glyph.c`'s and `src/editor.c`'s comments.
7. **Editing polish** — delete word, replace.
8. **Multiple buffers / tabs**, split view, project-wide search, git integration.
9. **Mouse support** — click to place cursor, wheel scroll, drag to select.
10. **Theme/color config** — background, selection, syntax-highlight, and UI-bar colors are still hardcoded hex literals in `editor.c`/`app.c`; a natural next slice of the config file.
11. **Plugin/scripting layer** — deliberately not started: `command_register`/`keymap_bind` (§2.8) are the seam it would attach to (a script registers its own commands and binds keys to them, the same way `app.c`'s own defaults do), and §2.10's `vim_enabled` pattern is the model for making a scripting layer itself optional/toggleable too - but there's no embedded language yet. Lua is the natural choice if/when this is worth doing (small, fast, trivial to vendor, and the proven model for exactly this kind of editor).

## Vim modal editing track

See `src/vim.h`'s header comment for the authoritative scope notes.

1. ~~**Counts**~~ / ~~**Visual mode**~~ — done, see CHANGELOG §2.9.
2. ~~**Linewise motions composed with operators**~~ — `dj`, `dk`, `dgg`, `dG` done: `vim_dispatch_pending_op` resolves them directly to a row range (`vim_apply_pending_linewise_op`) instead of going through `vim_resolve_motion`, which stays charwise-only.
3. **Text objects** — `iw`, `aw`, `ci(`, `di"`, etc. Needs a notion of "object under cursor", not just "motion from cursor".
4. **Registers** — named (`"ay`) and numbered, instead of always going through the one system clipboard.
5. **Authentic linewise put** — `p`/`P` after a linewise yank should insert a new line below/above, not paste charwise at the cursor.
6. ~~**Command-line mode**~~ — `:w`/`:q`/`:wq`/`:q!` done: `:` (Normal mode only) enters `app.c`'s `APP_MODE_COMMAND`, bound via the keymap like Ctrl+F search rather than wired into `vim.c` at all. The command-line prompt shares the status bar's single row rather than stacking a separate bar. `:%s/.../.../ ` is deliberately still not here - it needs a find/replace engine this codebase doesn't have yet (Main track #4, "Search improvements"); building it now would mean redoing it once that lands.
7. **Macros and marks** — `qa...q`, `@a`, `` ` ``/`'`.
8. **A real Vim word model** — `w`/`b`/`e` currently use this codebase's existing alnum-vs-everything-else split; real Vim distinguishes punctuation runs from word runs too.
9. **Sticky column for `j`/`k`** — see NOTES.md; matters more once Vim's vertical motions are counted/composed.
