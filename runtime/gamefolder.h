// The game folder on a desktop (runtime/gamefolder.c): the player's own folder with exefs, romfs and the game code that
// tools/build_game.py makes beside them (mhgugame.dll and mhgugame.id), as Android's GameFolderActivity reads it.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
// What is wrong with dir for this version, in the player's language (language 1: Spanish); "" when it can be played.
// The same checks as Android's GameFolderActivity, with the PC's game code.
void gamefolder_problem(const char *dir, int language, char *why, size_t size);
// The folder to play, in the spelling the runtime's file functions take: MHGU_DATA_DIR, the one chosen before
// (game_folder.txt beside the settings), data beside the executable, or the executable's own folder; failing those, one
// the player chooses now (kept in game_folder.txt). false: the player gave up.
bool gamefolder_find(char *out, size_t size);
// The player's language before the settings are read: the system's (1: Spanish).
int gamefolder_language(void);
// A folder the player picked (UTF-8, from a dialog), or one level off (its data folder, the folder above), as on
// Android: true when it can be played, with its UTF-8 spelling and the runtime's; else why says what is wrong.
bool gamefolder_check_picked(const char *picked, int language, char *utf8, size_t utf8_size, char *native, size_t native_size,
                             char *why, size_t why_size);
// Keeps the folder (UTF-8) for the next start: game_folder.txt beside the settings. The one kept ("" when none).
void gamefolder_remember(const char *utf8);
void gamefolder_saved(char *utf8, size_t size);
// The game folder screen (runtime/launcher.cpp): 1 a folder that can be played was chosen (native: the runtime's
// spelling, kept in game_folder.txt), 0 the player left, -1 it couldn't make its window
int launcher_choose_folder(const char *problem, char *native, size_t native_size);
#ifdef __cplusplus
}
#endif
