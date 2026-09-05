#ifndef APP_H_
#define APP_H_

#include <SDL2/SDL.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "./config.h"

// Registers every built-in command and default keybinding (see
// command.h). Must run before config_load(), so a config `bind` line
// overrides a default rather than racing it.
void app_register_commands(void);

// Owns the editor/file-browser/Vim state and runs the event loop until
// quit. `window`/`face` are already set up by main.c; this creates its
// own renderer + glyph atlas from them. Applies `cfg` (tab width, Vim
// on/off, line-number settings), loads argv[1] if given, and opens the
// file browser at the current directory. Returns the process exit code.
int app_run(SDL_Window *window, FT_Face face, Config *cfg, int argc, char **argv);

#endif // APP_H_
