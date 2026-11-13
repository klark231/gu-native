#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void ui_capture_launch_args(int argc, char **argv);
// The settings' screens of their own, opened on the host thread (runtime/ui.c): Android's activities, or on a desktop
// the system's dialogs (runtime/pc_actions.c)
void ui_request_licenses(void);
void ui_request_game_folder(void);
void ui_request_save_import(void);
void ui_request_save_export(void);
void ui_request_save_folder(void);  // Settings -> Save folder (Android's SaveFolderActivity; a PC's: not yet)
void ui_request_share_log(void);
void ui_request_import(int kind);  // the import screen (Android's ImportActivity): 0 cheats, 1 a mod, 2 DLC
#ifdef __ANDROID__
void ui_request_gpu_driver(void);
bool vk_gpu_adreno(void);          // (nvn/vulkan/device.c)
bool vk_gpu_adreno_fix_applies(void);  // (an Adreno on a Vulkan 1.1 driver: [Performance] AdrenoFix)
const char *gpu_driver_name(void);  // (platform/android_gpu.c: "" the system's)
#else
// runtime/pc_actions.c (host thread): the game folder, a save in or out, the log's folder, cheats and a mod copied in
void pc_choose_game_folder(void);
void pc_import_save(void);
void pc_export_save(void);
void pc_open_log_folder(void);
void pc_import_cheats(void);
void pc_import_mod(void);
void pc_mod_dialog(bool zip);  // (any thread) the dialog the next pc_import_mod opens: the mod's .zip, else its folder
// The Cheats and Mods tabs (the menu's thread): the cheat files (UTF-8 names, each up to 256 bytes, at most max) in the
// cheats folders the game reads, one added since the start too, sorted
int pc_cheat_files(char (*names)[256], int max);
// Removes the cheat file `file` (pc_cheat_files' name; "" once it is gone) whose cheats the game loaded as `loaded`
// (cheats_file; "" when it didn't): deleted from every cheats folder, its lines out of cheats/_off.txt, its cheats
// stopped now. false: it couldn't be deleted (err says why)
bool pc_remove_cheat_file(const char *file, const char *loaded, char *err, size_t errsz);
// Removes the mod `name` (mods_installed's): out of the list and of load_order.txt at once, its folder deleted on a
// thread of its own (1) or, as the game loaded it at this start, at the next one (2: a restart takes it out of the
// game); 0: it couldn't be (err says why)
int pc_remove_mod(const char *name, char *err, size_t errsz);
// The mod `name` (mods_installed's) is a link to a folder elsewhere (a junction, a symbolic link): removing it deletes
// the link alone, and that folder is kept
bool pc_mod_is_link(const char *name);
// The menu's answer to the save chosen (any thread): Import and restart (it is kept for the next start; false: it
// couldn't be) or Cancel
bool pc_import_save_confirm(void);
void pc_import_save_cancel(void);
// runtime/main.c, before the game runs: a save imported is put in place, the current one kept in save_backups; the
// first time, the port's optional cheats copied in, off; the mods removed or replaced while the game used them
void pc_apply_imported_save(void);
void pc_install_bundled_cheats(void);
void pc_apply_mod_changes(void);
#endif
void ui_request_action(bool restart); // Called after the in-game confirmation, handled on the host thread.
bool ui_action_failed(void);          // Take and clear a failed restart notification.
#ifdef __cplusplus
}
#endif
