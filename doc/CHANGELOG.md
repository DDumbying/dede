# dede — Changelog

A chronological log of what changed and why, since this fork diverged from upstream `ded`. See [`ROADMAP.md`](ROADMAP.md) for what's left and [`NOTES.md`](NOTES.md) for known quirks and stray TODOs.

## 1. Starting point

`ded` ("Dramatic EDitor", by Alexey Kutepov / tsoding) is an unfinished text editor in C11. At the point this fork started, it could already:

- **Open/save** a file: load via command line or the file browser, save with `F2`.
- **File browser** (`F3`): sorted file listing, directory navigation, path normalization, and a rainbow "epicness" shader.
- **Basic editing**: insert text, Backspace/Delete, Home/End (Ctrl = whole document, else line), word jumps (Ctrl+arrows), paragraph jumps (Ctrl+Up/Down), Tab inserting 4 spaces at the cursor.
- **Selection + clipboard**: Shift+arrows to select, Ctrl+A, Ctrl+C copy, Ctrl+V paste (system clipboard).
- **Incremental search**: Ctrl+F then type to search live; Enter/Esc to stop.
- **Syntax highlighting**: C/C++ lexer — keywords, strings, `//` comments, preprocessor directives, symbols, braces/parens/semicolons.
- **Rendering**: GPU glyph atlas (FreeType), blinking cursor, smooth eased camera, live shader reload on `F5`.
- **Cross-platform build**: `build.sh` (POSIX), `build_msvc.bat`, `build_msys2_mingw64.sh` (Windows), GitHub Actions CI (Linux gcc/clang, macOS).

Stack: SDL2 + OpenGL 3.3 + GLEW + FreeType, using hand-rolled data structures (dynamic arrays, string views, string builders, arena, glyph atlas, immediate-mode mesh renderer).

## 2. What changed (chronological)

### 2.1 Undo / redo — Ctrl+Z, Ctrl+Y / Ctrl+Shift+Z
`src/editor.h`, `src/editor.c`, `src/main.c`

- Every edit is recorded as an `Edit_Op` (`EDIT_INSERT` / `EDIT_DELETE` with position + affected text).
- Ops group into undo steps: rapid consecutive edits (< 500 ms) merge into one step; navigation/mode keys call `editor_flush_group` to seal the current step.
- Undo replays the group's inverse ops in reverse; redo replays forward. Cursor lands at the start of the change on undo, end on redo.
- A new edit clears the redo stack; history is freed when a new file is loaded.
- Added `Edit_History` (undo/redo stacks) and `Edit_Ops` (current group) to the `Editor` struct.

### 2.2 Selection delete, cut, type-over
`src/editor.h`, `src/editor.c`, `src/main.c`

- Backspace/Delete on a selection now deletes the whole selection and clears it.
- Typing or pasting over a selection replaces it (single undo step reverts the whole type-over).
- Ctrl+X = cut (copy then delete selection).
- Fixed an off-by-one in `editor_clipboard_copy` (`end - begin + 1`) that copied one char past the selection and could read out of bounds.

### 2.3 Indent / unindent — Tab, Shift+Tab
`src/editor.h`, `src/editor.c`, `src/main.c`

- Tab with a selection indents every selected line (selection preserved and follows the text).
- Shift+Tab with a selection dedents each selected line (removes up to 4 leading spaces).
- Shift+Tab without a selection dedents the current line; Tab without a selection still inserts 4 spaces at the cursor.
- Generalised `editor_data_insert` into `editor_data_insert_at(pos)` so text can be inserted at any position.
- Under the hood: lines are mutated bottom-up so offsets stay valid, then the selection positions are remapped using per-line deltas (`editor_apply_deltas`).
- Undo/redo now clear the selection (positions would otherwise be stale).

### 2.4 Vim modal editing (Steps 1-3)
`src/vim.c`, `src/vim.h` (new), `src/editor.c`, `src/editor.h`, `src/main.c`, `src/tes_motion.c` (new, headless test)

- **Step 1 — pure motions**: refactored the existing cursor-movement mutators (`editor_move_line_up`, `editor_move_word_left`, ...) into pure `editor_find_*(e, pos) -> pos` functions plus a thin mutator wrapper around each. Zero behavior change (`tes_motion.c` cross-checks every mutator against its pure function) — this is the shared core Vim's motions, and any future operator/visual-mode range, build on.
- **Step 2 — Normal/Insert dispatch**: `vim.c` owns a `Vim_Mode` + `Vim_State`, dispatched from `main.c` via `vim_handle_key()` before any other key handling. `editor.c` has no idea Vim exists — it only grew a `cursor_block` rendering hint. Normal-mode motions: `hjkl`, `w`/`b`/`e` (approximate word model — see the comment on `editor_find_word_end`), `0`/`$`, `gg`/`G`. `i` enters Insert; Escape returns to Normal (and steps the cursor back one column, matching Vim convention).
- **Step 3 — operators**: `d`/`c`/`y` composed with a motion (`dw`, `d$`, `cw`, ...) or doubled with themselves (`dd`/`cc`/`yy`, linewise), plus `x`, `D`, `C`, `Y`, `p`/`P`. These do **not** introduce a separate yank register — they drive the editor's existing selection + system-clipboard primitives (`editor_clipboard_copy/cut/paste`), the same ones Shift+arrow selection and Ctrl+C/X/V already use. `cc` on a line clears its content but keeps the line itself (and the buffer's line count) — unlike `dd`, which removes the line's newline along with it; this distinction is what a regression test in `tes_motion.c` locks in after being caught by it.
- Out of scope so far (tracked in `vim.h`'s header comment): counts (`3dw`), Visual mode, command-line mode (`:w`/`:q`), registers/macros/marks, text objects (`iw`, `ci(`, ...), operators composed with `j`/`k`/`gg`/`G` (`dj`, `dgg`, `dG` — these motions aren't expressible as the simple `(buffer, pos) -> pos` model `vim_resolve_motion` needs), and authentic linewise `p`/`P` (the clipboard is charwise-only, so both just insert at a cursor position).

### 2.5 Two Vim-integration bugs found and fixed
`src/vim.c`, `src/vim.h`, `src/editor.c`, `src/main.c`

- **`i` leaked into the buffer as text.** SDL fires `SDL_TEXTINPUT` right after `SDL_KEYDOWN` for every printable key — including ones Normal mode just consumed as a command. The `SDL_TEXTINPUT` handler swallowed these by checking "are we in Normal mode", but `i` flips the mode to Insert as part of handling the very keydown that TEXTINPUT event belongs to, so by the time it arrived the check no longer matched and the letter got typed. Fixed with `Vim_State::consumed_textinput`, set whenever `vim_handle_key` claims a key while *starting out* in Normal mode (captured before dispatch), and read via `vim_take_consumed_textinput()` in `main.c`'s `SDL_TEXTINPUT` handler.
- **Normal-mode cursor was a thicker bar, not a block.** `cursor_block` only changed a hardcoded width (5px → 12px); it never actually filled the glyph cell. Fixed by sizing the block to the real glyph advance (`atlas->metrics[ch].ax`) of the character under the cursor, falling back to the space glyph's width past end-of-buffer/line or on control characters.
- Incidental bug caught by the second fix's review: `vim_handle_key` was running unconditionally even while `e->searching` — meaning typing an incremental-search query while in Normal mode (the default mode) got intercepted as Vim commands instead of reaching the search box. Fixed by having Vim defer entirely to the editor's search handling while `e->searching` is true.

### 2.6 Unsaved-changes prompt, save-as, and new-file
`src/editor.c`, `src/editor.h`, `src/main.c`

- `Editor` grew a `dirty` flag, set in the one place every content mutation already funnels through (`editor_add_op`) plus `editor_undo`/`editor_redo` (which bypass that funnel by replaying recorded ops directly). Cleared on a successful save/save-as/load/new-file.
- `main.c` replaced its `file_browser` bool with an `App_Mode` enum (`APP_MODE_EDITOR` / `_FILE_BROWSER` / `_SAVE_AS` / `_CONFIRM`) — mutually exclusive by construction, unlike stacking more bools would have been.
- **F3** (open file browser), **Ctrl+N** (new file), and closing the window / **Ctrl+Q** (quit) all go through `request_action()`: if `editor.dirty`, it parks the request behind an `APP_MODE_CONFIRM` y/n prompt instead of running it immediately. Only explicit `y` confirms — Enter is deliberately *not* an alias for it, so an accidental Enter while the prompt is up (easy to hit out of habit) falls back to the safe (cancel) option like `n`/Escape, not the destructive one.
- **F2** now prompts for a path (`APP_MODE_SAVE_AS`) instead of erroring when the buffer has none yet; **Ctrl+Shift+S** always opens that prompt (pre-filled with the current path, if any) as an explicit "Save As".
- **`draw_bottom_bar`**: the renderer has no screen-space drawing mode of its own — every vertex goes through the same camera transform as the document text (`shaders/simple.vert`). Used for the confirm prompt, the save-as path entry, and now also `flash_error` messages, which used to go to stderr only.

### 2.7 Two bugs found in the above and fixed
`src/main.c`

- **The bottom bar could overflow past the window.** It originally rode the document's own live `sr->camera_pos`/`camera_scale`, so it zoomed with the document — and the document's auto-zoom goes extreme on a near-empty buffer (the target scale divides by the longest visible line's length), blowing the bar up well past the window edges. Fixed by giving `draw_bottom_bar` its own fixed camera transform (`UI_TEXT_SCALE`), saved and restored around the draw call, so the bar is now always exactly `UI_BAR_HEIGHT_PX` screen pixels tall regardless of window size or the document's zoom.
- **Confirming a prompt (e.g. `y` for "new file, discard changes?") could leak that letter into the buffer.** Same root cause as 2.5's `i` bug, in a new spot: pressing `y` exits `APP_MODE_CONFIRM` back to `APP_MODE_EDITOR` within the same keystroke, so the trailing `SDL_TEXTINPUT('y')` arrives already in editor mode — and if Vim was still in Insert mode underneath (e.g. the user hadn't pressed Escape before triggering Ctrl+N), it got typed into the freshly-created buffer. Generalized the same "did this key just get consumed by a mode I've since left" pattern one level up: `app_mode_changed_this_key` is set whenever an `SDL_KEYDOWN` changes `mode`, and `SDL_TEXTINPUT` swallows unconditionally when it's set, regardless of which mode was switched *into*. This subsumes the old `APP_MODE_CONFIRM`-specific no-op and fixes the symmetric case (`n`/Escape cancelling) too, not just `y`.

### 2.8 Command registry, data-driven keymap, and a config file
`src/command.h`, `src/command.c` (new), `src/config.h`, `src/config.c` (new), `src/editor.h`, `src/editor.c`, `src/main.c`, `src/tes_command.c` (new, headless test)

Architectural groundwork, not a user-facing feature by itself: `main.c`'s ~240-line `switch (event.key.keysym.sym)` for Normal-mode editor keys (the one thing standing in the way of configurable keybindings, and the thing a future plugin system would otherwise have to edit directly) is gone, replaced by two new, narrow, dependency-free modules:

- **`command.c`**: a registry of named, zero-argument `Command`s (`command_register`/`command_find`), and a `Keybinding` table resolving an exact key+Ctrl+Shift chord to one (`keymap_bind`/`keymap_resolve`). Knows nothing about the editor, Vim, or anything else in this app — deliberately, so it stays usable as-is if a scripting layer registers its own commands later. `keymap_bind` replaces an existing binding for the same chord in place rather than shadowing it with a duplicate, which is what lets a config override "just work" against a built-in default.
- **`config.c`**: loads a simple `key = value` / `bind <chord> = <command>` file (`#` comments, one setting per line) on top of `config_default()`'s values. A missing file is the expected common case (not an error); a line it can't parse is reported to stderr and skipped, never fatal to the rest of the file. `bind` lines call `keymap_bind` directly, so config-driven and built-in keybindings go through the exact same parser and the exact same table.
- **`main.c`** now holds ~30 small `cmd_*` functions (one per actual action — `cmd_move_char_left`, `cmd_save`, `cmd_undo`, ...) plus two static tables: `default_commands[]` registers all of them, `default_keybindings[]` binds ~44 chords to them. `register_default_commands_and_keymap()` runs both before `config_load()`, so a user's `bind` line overrides a default instead of racing it. Shift-as-"extend the current selection" (arrows, Home/End, Escape) is handled once, generically, by the dispatcher — via each command's `shift_extends_selection` flag — rather than by every movement command calling `editor_update_selection` itself.
- **`Editor::indent_width`** replaces the old compile-time `INDENT_WIDTH` macro, so `tab_width` in the config file actually does something; capped at the new `EDITOR_MAX_INDENT_WIDTH` (16), which sizes `editor_indent`/`editor_unindent`'s scratch buffers.
- Three small, deliberate behavior simplifications made along the way (none were an intentional design choice in the original switch - just modifiers it never bothered checking because it didn't need to for a single hardcoded binding): Backspace/Delete/Return/F2/F3/F5 no longer also fire with Ctrl and/or Shift incidentally held; Escape no longer also fires on Ctrl+Escape; Tab/Shift+Tab no longer also fire on Ctrl+Tab/Ctrl+Shift+Tab, freeing those chords for a future tab-switching feature instead of silently indenting.
- One accepted, narrow behavior change from the refactor itself: Escape's Shift-extends-selection handling now runs *before* `editor_stop_search` instead of after (the dispatcher always resolves shift first, generically), so `editor_update_selection`'s own `if (e->searching) return` guard can now suppress it in the specific case of Shift+Escape pressed while both searching and holding a selection. A zero-argument command can't reproduce ordering that depends on the specific event that resolved to it; this combination was judged too obscure to be worth breaking that abstraction over.
- `./dede.conf` (repo/cwd-relative — not yet an XDG-style user config path, a natural follow-up) is loaded unconditionally at startup; `font`, `tab_width`, and arbitrary `bind` overrides are all it understands so far. Colors/theming are not configurable yet.

### 2.9 Two more bugs, and counts + Visual mode
`src/main.c`, `src/vim.c`, `src/vim.h`, `src/tes_motion.c`

- New file / opening a file from the browser reset the buffer but not Vim's own state, so it could enter the fresh buffer still in Insert mode if you hadn't pressed Escape first. Both now reset `vim` via `vim_state_init()`.
- Comments across the last few commits had grown too verbose; trimmed throughout, going forward capped at ~2 lines and only where genuinely non-obvious.
- **Counts**: digits accumulate in `Vim_State::count`, cleared once a motion/operator consumes them. Works with motions (`5j`, `3w`), operators (`3dd`, `d3w`, and `2d3w` which multiplies), and `x`; `5G`/`5gg` jump to an absolute line number instead of buffer start/end.
- **Visual mode**: `v`/`V` (charwise/linewise) reuse the editor's existing selection mechanism directly — entering Visual just anchors `select_begin` at the cursor, and every motion moves `e->cursor` as normal, so the selection grows for free. `d`/`x`/`c`/`y` act on the current selection (Visual's charwise selection is inclusive of both ends, unlike a motion's usual exclusive end); `v`/`V` toggle mode or exit; Escape exits without changing the buffer. Arrow-key movement in Visual mode needed one change in `main.c`: `shift_extends_selection` commands now also extend when `vim.mode == VIM_MODE_VISUAL`, not just on an actual Shift press, so plain arrows don't clear the selection while in Visual mode.
- Not done: counts don't apply to Visual-mode operators, entering Visual with a count, or `i`-with-count (real Vim repeats the insertion); text objects; `dj`/`dgg`/`dG` still unsupported (same pre-existing limit as before, see `vim.h`).

### 2.10 Modular restructure + Vim as a runtime toggle
`src/main.c`, `src/app.c`/`.h` (new), `src/vim.c`/`.h`, `src/config.c`/`.h`, `src/editor.h`, `src/tes_motion.c`, `src/tes_command.c`

`main.c` had grown to ~950 lines mixing GL/SDL/FreeType bootstrap, the App_Mode event loop, and ~30 command implementations. Split along that seam:

- **`main.c`** (~110 lines now): FreeType/SDL/GL/window setup, then hands off to `app_run()`.
- **`app.c`/`app.h`** (new): everything else that was in `main.c` — `App_Mode`, `Confirm_Action`, the `editor`/`fb`/`vim` state, all `cmd_*` commands + the default-commands/keybindings tables, the bottom bar, and the event loop itself, behind one entry point (`app_run`).
- **Vim is now a genuine runtime toggle**, not just always-on: `Config.vim_mode` (default `true`) drives a `vim_enabled` flag in `app.c`. When false, `vim_handle_key`/`vim_handle_browser_key` are never called at all - Vim is fully inert, not "returning false a lot" - so a bug in `vim.c` can't affect classic editing or browsing while it's off. New unbound-by-default command `toggle-vim-mode` flips it live (`bind <chord> = toggle-vim-mode` to use it).
- **Vim now reaches the file browser**: `vim_handle_browser_key(Vim_State*, File_Browser*, SDL_Keysym)` in `vim.c` adds `j`/`k` (with counts), `gg`/`G` for the flat file list, tried before the existing Up/Down/Enter/F3 handling (unchanged, still the fallback with Vim off or on an unclaimed key).
- **`Editor`/`Config`** gained `line_numbers`/`relative_line_numbers` (parsed, stored, not yet rendered - the gutter itself is a follow-up).

### 2.11 Renamed to dede
`README.md`, `doc/` (new), `src/main.c`, `src/app.c`, `build.sh`, `build_msvc.bat`, `build_msys2_mingw64.sh`, `.gitignore`, `.github/workflows/ci.yml`

- Project renamed from `ded` ("Dramatic EDitor") to **dede** ("Dramatically Expanded Dramatic Editor") to reflect that this is now a personal, actively-expanded fork rather than upstream `ded` itself. Binary name, window title, and config filename (`dede.conf`) all follow.
- This changelog, the roadmap, and known-quirks/TODO notes moved out of the repo-root `PROGRESS.md` into `doc/` (`CHANGELOG.md`, `ROADMAP.md`, `NOTES.md`), so project docs have a real home instead of one growing file.

### 2.12 Logo refresh + splash/about screen layout
`src/app.c`, `assets/dede.png`

- `assets/dede.png` replaced with new logo art that already has the "dede" wordmark drawn into it below the mark itself. The separate `draw_centered_line(..., "dede", ...)` row previously drawn underneath the logo on both the splash and About screens was removed, since it now just duplicated the image.
- Both screens used to start drawing at a fixed pixel offset from the top of the window, so on anything taller than a small window the content sat in a cramped block with a large empty margin below it. Rewrote both to compute their total content height up front (logo + menu/about rows + optional recent-files block + hint line) and start drawing at half that height, so the whole block is vertically centered on the window's actual center at any window size, not just horizontally.
- Added a subtle horizontal divider between the menu and the "Recent Files" section on the splash screen for a bit of visual grouping.
- New helpers: `logo_world_height()` (works out the logo's on-screen height without drawing it, so a caller can lay out the rest of the screen around it first) and `draw_divider()`.

### 2.13 Glyph atlas texture bleeding ("tearing" between characters)
`src/free_glyph.c`

- Reported as characters looking "torn"/low quality, worst on the splash screen. Root cause: `free_glyph_atlas_glyph()` packed every glyph edge-to-edge into the shared atlas texture with zero gap between them, sampled with `GL_LINEAR`. Since it's one shared texture, bilinear filtering at a glyph's boundary blended in a sliver of whatever glyph happened to be packed next to it — visible as faint vertical seams between characters, worse the more the text is magnified (the splash screen renders the same glyphs at roughly 3x the on-screen size of normal editing view, so a bleed invisible at editing zoom was clearly visible there).
- Fix, done in two passes: added `GLYPH_ATLAS_PADDING`, a transparent gap left around every packed glyph (both on the normal packing advance and when wrapping to a new shelf row). 1px fully fixed it at normal editing zoom but left a faint residual at the splash screen's higher magnification, confirmed with a raw pixel dump of the affected region (not just eyeballing a zoomed screenshot); raised to 4px, which fixed it there too, re-verified the same way plus a re-check that normal editing view was still clean.
- Also zero-initialized the atlas texture up front — `glTexImage2D` was previously called with a NULL data pointer, leaving its contents driver-defined, so the new padding gaps (and any not-yet-packed shelf space) are now guaranteed transparent black instead of possibly garbage.
- Along the way, changed `Glyph_Metric.ax`/`ay` (advance width) from truncating to a whole pixel (`glyph->advance.x >> 6`) to keeping its true 1/64px precision (`/ 64.0f`) — a real rounding bug (every glyph's cursor advance was silently rounded down by up to just under a pixel), though on this font/size it turned out not to be the cause of the seam above; kept as a correctness fix regardless.

### 2.14 Block cursor no longer hides the character underneath
`src/editor.c`

- The Normal-mode block cursor was drawn as a fully opaque white rectangle after the text, completely covering whatever character it sat on — you couldn't tell what you were standing on.
- Now redraws that one character on top of the block afterward, in the editor's own background color (`0x181818`), matching how a terminal's reverse-video block cursor behaves. Reuses the exact source bytes already decoded for sizing the cursor's width, and positions the glyph using the same y-baseline the normal text-rendering loop uses (the cursor block's own vertical anchor is nudged by `CURSOR_OFFSET` for visual alignment and isn't the same baseline).
- Only fires when there's an actual printable character under the cursor; past end-of-buffer, on whitespace, or on a control character like `\n`, it's left as a plain block since there's nothing to preserve.

### 2.15 Find next/prev, highlight all matches, replace/replace-all
`src/editor.c`, `src/editor.h`, `src/app.c`, `src/tes_motion.c`

- **Find next/prev**: `Ctrl+F` already advanced through matches one at a time, but never wrapped past the end of the buffer; `editor_search_next`/`editor_search_prev` (pure, wrapping) back both the search-as-you-type path and explicit next/prev now. `Ctrl+Shift+F` adds find-*previous* (`find-prev` command), symmetric with `Ctrl+F`.
- **Highlight all matches**: `editor_render`'s search block used to draw one highlight rect over whatever was at the cursor. It now scans every line for occurrences of the live query and highlights every one (dim), with the match under the cursor brighter, so find-next/prev has a visible "current match" to move from and the total match count is visible at a glance.
- **Replace/replace-all**: `Ctrl+H` (while a search is active) opens replace-text input, piggybacking on the same `e->searching`-gated redirection in `editor_insert_buf`/`editor_backspace` that `e->search` itself already used (`e->replacing`/`e->replace`, see `editor_start_replace`). `Enter` replaces the match at the cursor and advances to the next (`editor_replace_current_match`); `Ctrl+Enter` replaces every occurrence in one pass (`editor_replace_all_matches`, one `editor_retokenize` at the end rather than one per match — same pattern `editor_indent` already used for bulk edits) and reports how many. `Esc` backs out one level at a time: replace input first, then the search itself.
- `editor_replace_all_matches` resumes scanning right after each inserted replacement (not from the deleted match's old position), so a replacement that itself contains the search text — e.g. `"a"` → `"aa"` — can't re-match its own output and loop forever; a regression test in `tes_motion.c` locks this in.

### 2.16 Theme/color config
`src/config.h`, `src/config.c`, `src/editor.h`, `src/editor.c`, `src/app.c`, `src/common.h`, `src/tes_command.c`, `dede.conf` (new)

- Background, selection, syntax-highlight, and UI-bar colors were hardcoded hex literals scattered across `editor.c`/`app.c` (some duplicated — the editor background alone appeared in three places). Added a `Theme` struct (`config.h`) with one field per color — `bg`, `fg`, `selection`, `search_current`, `search_other`, `gutter_bg`, `gutter_fg`, `accent`, `comment`, `string`, `preproc`, `error_text`, `ui_bar_bg`, `ui_bar_prompt_bg`, `ui_bar_error_bg`, `divider` — as a `Theme theme` field on `Config`, copied onto `Editor::theme` once at startup (`app_run`) since `editor_render` only has an `Editor*` to work with.
- `config_default()`'s theme values are exactly the old hardcoded literals, so a config file that doesn't mention any of them renders identically to before this landed.
- Each field name doubles as a config-file key (`config.c`'s `theme_fields` table, built with `offsetof`), so all sixteen colors share one hex-color parser (`sv_to_hex_color`: `"RRGGBB"` or `"RRGGBBAA"`, optional leading `#`, case-insensitive) instead of growing `config_apply_line`'s settings chain by sixteen near-identical branches.
- `dede.conf` (new, repo-root) sets `bg = 282828` — a personal preference, not a new shipped default.

### 2.17 File-browser incremental filter
`src/file_browser.h`, `src/file_browser.c`, `src/vim.c`, `src/app.c`, `src/tes_motion.c`

- Typing any printable character while browsing narrows the listing to entries containing it (case-insensitive substring, not fuzzy), the same "type to filter" the file browser's own TODO called for. Backspace edits the query; Escape clears it. The status bar (now also drawn in file-browser mode, which previously had none at all - so browser-side errors like a failed directory change are visible for the first time too) shows the live query while filtering, or the current directory otherwise.
- `File_Browser` gains `searching`/`search` - the same shape as `Editor`'s own incremental search. `fb->cursor` is redefined to always index the *currently visible* list (all of `files` with no query, just the matches otherwise) rather than the raw array, via a new `fb_visible_count`/internal `fb_visible_files` used consistently by `fb_render`, `fb_change_dir`, and `fb_file_path` so none of them can disagree about what's currently shown.
- `vim_handle_browser_key` now defers entirely while `fb->searching` (mirroring `vim_handle_key`'s existing deferral to `e->searching`) - otherwise `j`/`k`/`g` would still be swallowed as motions instead of reaching the filter as text. Up/Down/Enter, handled outside Vim's own dispatch, work unchanged during a filter.
- A query doesn't survive a directory change or leaving the browser (F3) - always starts fresh, like the main editor's search does on each `Ctrl+F`.

### 2.18 Two bugs found in the file-browser filter, and an absolute dir_path
`src/file_browser.c`, `src/vim.c`, `src/app.c`

- **§2.17's filter ate Vim's `j`/`k`/`g` motions.** `vim_handle_browser_key` claims those as motions on `SDL_KEYDOWN`, but SDL still fires the matching `SDL_TEXTINPUT` right after - and §2.17's file-browser `SDL_TEXTINPUT` case started/fed the filter on *any* printable character unconditionally, the exact same leak class as §2.5's `i`-leaks-into-the-buffer bug, just never guarded against here. With Vim on, filtering now only starts on an explicit `:` (`SDLK_SEMICOLON` + Shift, same chord shape as the editor's own `shift+;` → `start-command-line`) - a new `fb_search_started_this_key` flag (mirroring `app_mode_changed_this_key`) swallows that keystroke's own trailing `SDL_TEXTINPUT` so the `:` itself doesn't become the filter's first character. With Vim off there's no motion to protect, so any character still starts filtering immediately, same as before.
- **`fb->dir_path` displayed as a bare `"."`.** `fb_open_dir` stored its `dir_path` argument verbatim - at startup that argument is literally `"."`, and nothing rendered `dir_path` anywhere until §2.17 added the file-browser status bar, so the bug was invisible until then. Now resolved through `realpath()` (falling back to the raw path if it doesn't resolve) so the status bar - and every path built from it during subsequent navigation - shows a real absolute path instead.
