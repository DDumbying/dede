# dede — Notes / known quirks / stray TODOs

See [`CHANGELOG.md`](CHANGELOG.md) for the full history and [`ROADMAP.md`](ROADMAP.md) for planned work.

## Known quirks

- Undo/redo place the caret at the start (undo) or end (redo) of the change; exact caret restoration is not implemented.
- Grouping is time-based (500 ms): a long continuous typing burst without any movement key is still a single undo step.
- `SDL_GetTicks()` returns 0 until `SDL_INIT_TIMER` is initialized — headless tests must init the timer for the merge window to behave realistically.
- `editor_clipboard_copy/cut/paste` go through SDL's system clipboard (`SDL_Set/GetClipboardText`), which needs the video subsystem initialized to work reliably — `tes_motion.c` runs with no `SDL_Init` at all, so its yank/paste round-trip check is skipped (not failed) if the clipboard isn't actually functional in that environment. Delete and change are verified independently of whether the copy half succeeds.

## TODOs still in the code

From `src/app.c` and elsewhere:

- Search does not auto-fill from the current selection.
- File browser has no incremental search.
- `fb->dir_path` grows indefinitely if you navigate up at the root (`src/file_browser.c`).
- No trailing-whitespace stripping / auto-save.
- `type_of_file()` is unimplemented on Windows (`#error`), and the MSVC CI job is disabled (#29).
