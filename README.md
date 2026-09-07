# dede

**dede** ("**D**ramatically **E**xpanded **D**ramatic **E**ditor") is a personal, actively-expanded fork of [`ded`](https://github.com/tsoding/ded) ("**D**ramatic **ED**itor", by Alexey Kutepov / tsoding) — a GPU-rendered text editor in C11. This fork is maintained by and primarily for its own author, as a daily-driver editor built up incrementally rather than a polished general-purpose release. Expect rough edges.

## Features

- Undo/redo, selection + system clipboard, indent/dedent, incremental search, C/C++ syntax highlighting, UTF-8 text.
- **Optional Vim modal editing** (Normal/Insert/Visual, operators, counts, motions, an Ex-style `:w`/`:q`/`:wq`/`:q!` command line) — on by default, toggleable at runtime, and works in the file browser too. See `dede.conf` below.
- A command registry + data-driven keymap: every keybinding is rebindable from a config file, not hardcoded.
- File browser, unsaved-changes prompts, save-as, a line-number gutter, and a status bar.
- A splash screen (`./dede` with no file argument) with recent files, and links to this repo and the configuration docs below.

See [`doc/CHANGELOG.md`](doc/CHANGELOG.md) for the full history of what's been added and why, [`doc/ROADMAP.md`](doc/ROADMAP.md) for what's planned, and [`doc/NOTES.md`](doc/NOTES.md) for known quirks and stray TODOs.

## Quick Start

### Dependencies

- [SDL2 2.0.9+](https://www.libsdl.org/)
- [SDL2_image 2.0+](https://github.com/libsdl-org/SDL_image) (splash screen logo)
- [FreeType 2.13.0+](https://freetype.org/)
- [GLEW 2.1.0+](https://glew.sourceforge.net/)

### POSIX

```console
$ ./build.sh
$ ./dede src/main.c
```

### Windows MSVC

```console
> .\setup_dependencies.bat
> .\build_msvc.bat
> .\dede.exe src\main.c
```

## Configuration

Create a `dede.conf` next to the binary (any setting you omit keeps its default):

```
font = ./fonts/iosevka-regular.ttf
tab_width = 4
vim_mode = true
bind ctrl+shift+q = quit
```

## Fonts

- Victor Mono: https://rubjo.github.io/victor-mono/
- Iosevka: https://github.com/be5invis/Iosevka
