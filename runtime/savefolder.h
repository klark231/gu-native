// The save folder (Settings -> Save folder): where the save is kept, the game's own folder or one the player chose (a
// folder a sync app shares with their other devices, the phone's and the PC's). The same on every platform: Android's
// screens (android/.../SaveFolderActivity.java, SaveFolder.java) and the PC's menu write the files below, beside the
// settings (the folder the game runs in), and this part of the runtime reads them.
//
//   save_folder.txt          UTF-8: the chosen folder, the one that holds system and system_backup (on a phone the
//                            screens resolve a picked folder to its "MHGU save" folder); none or empty: the game's own
//   save_folder_pending.txt  folder=<UTF-8 path, or nothing: back to the game's own>, keep=folder|device: a change the
//                            menu chose while the game ran (it holds the save in memory and could write it at any
//                            moment), applied at the next start before the game runs (savefolder_apply_pending)
//   save_folder_state.txt    size=, time=: the folder's save as last seen at a start (an empty folder that had one is a
//                            problem the start gate shows; the screens write it)
//   save_folder_missed.txt   time=: the folder couldn't be reached at a save, so that save went to the game's own folder
//                            (savefolder_root); the next start asks which one to keep
//   device.txt               id=, name=: this device (made once), for the in-use mark
//   <folder>/.mhgu-in-use    id=, name=, time=: a device has the game open with that save: written at the start, kept
//                            fresh every 5 minutes, removed at a clean exit. Another device's, younger than 15 minutes,
//                            makes the start gate warn.
//
// Only save_0 (the game's save) moves; the SD card file system, bonus/ and save_backups/ stay in the game's own folder.
// MHGU_SAVE_DIR (tests) still decides everything. On Windows a path the ANSI code page can't spell is used through its
// short (8.3) form, as the game folder is.
//
// The platform's start gate settles what save_folder_missed.txt, save_folder_state.txt and the in-use mark ask before the
// game runs (Android: SaveFolderActivity.gate, which every start goes through; a PC's start must do the same once its menu
// offers the folder).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Once g_save_dir is known, before the game runs (main.c): a pending change applied (savefolder_apply_pending; on a
// phone GameFolderActivity did it already), save_folder.txt read, this device's in-use mark written.
void savefolder_init(void);
// The host folder for the save mount `sub` (services/fs.c, at every mount: the game mounts at each save, and at the
// start to read it). save_0 with a folder chosen: that folder when it can be reached (and written); else, and for anything
// else, g_save_dir/sub. Missed once, the game's own folder until the game closes, and a notice says so: the save the game
// holds may be the one it read there, never to be written over the folder's newer one. true: the chosen folder was missed
// (the mount is the game's own folder instead).
bool savefolder_root(const char *sub, char *out, size_t size);
// A save was written to the game's own folder because the chosen one was missed (services/fs.c, after its commit):
// written down (save_folder_missed.txt, the next start asks which one to keep) and told once over the game. Nothing is
// lost: the game writes its whole save each time.
void savefolder_missed(void);
// The host thread, every frame or so (runtime/ui.c): the in-use mark kept fresh; a notice that waited for the game's
// first frames.
void savefolder_tick(void);
// The chosen folder as save_folder.txt has it (UTF-8), "" for the game's own. For the menus.
const char *savefolder_chosen(void);

// For a menu (the PC's): what is in a save folder. 1: a save, with the hunters' names (", " between them, UTF-8) and
// the time it was saved (seconds since 1970); 0: none.
int savefolder_describe(const char *dir, char *names, size_t names_size, int64_t *saved);
// A change for the next start (folder UTF-8, NULL or "": back to the game's own; keep_folder: the folder's save stays,
// else this device's goes there). false: it couldn't be written.
bool savefolder_set_pending(const char *folder, bool keep_folder);
// The pending change, applied now: the saves copied first (checked), save_folder.txt written last; the save not kept
// goes to save_backups beside the game's own save folder. NULL: done (or nothing to do); else why not (nothing was
// changed, and the change is dropped).
const char *savefolder_apply_pending(void);

#ifdef __cplusplus
}
#endif
