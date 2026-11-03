#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void ui_capture_launch_args(int argc, char **argv);
#ifdef __ANDROID__
void ui_request_licenses(void);     // The licenses' and the game folder's screens, opened on the host thread too
void ui_request_game_folder(void);
void ui_request_user_folder(void);  // Settings -> User folder (UserFolderActivity)
void ui_request_save_import(void);
void ui_request_save_export(void);
void ui_request_gpu_driver(void);
bool vk_gpu_adreno(void);          // (nvn/vulkan/device.c)
const char *gpu_driver_name(void);  // (platform/android_gpu.c: "" the system's)
void ui_request_share_log(void);
#endif
void ui_request_action(bool restart); // Called after the in-game confirmation, handled on the host thread.
bool ui_action_failed(void);          // Take and clear a failed restart notification.
#ifdef __cplusplus
}
#endif
