// The Vulkan driver: the system's, as always, unless the player picked one of their own (Settings, Graphics, GPU driver:
// android/.../DriverActivity.java), for an Adreno, at their own risk.
//
// The renderer's vk* are function pointers (third_party/volk, force-included by native/CMakeLists.txt), filled from the
// vkGetInstanceProcAddr given here. With the system's driver that is the one of the system's libvulkan.so: every call
// reaches the same functions the library exports. A driver of one's own takes libadrenotools: a private copy of
// libvulkan.so, hooked to load that driver instead of the system's.
//
// A driver that doesn't work must not leave the game unable to start. A trial file is written before it is opened and
// removed once frames have been drawn with it (gpu_driver_good): found at the next start, that run never got there, so the
// choice is moved to failed (DriverActivity tells the player) and the system's driver is used.
//
// files/gpu_drivers/: <folder>/<driver>.so, selected (its folder, then its library's file name), failed, trial
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <android/native_window.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "../rt.h"
#include "adrenotools/driver.h"

static bool g_custom;
static char g_name[128];
static char g_dir[600];  // ("/gpu_drivers/<folder>/": gpu_driver_check)

bool gpu_driver_custom(void) { return g_custom; }
const char *gpu_driver_name(void) { return g_name; }

static void chomp(char *s) { s[strcspn(s, "\r\n")] = 0; }

PFN_vkGetInstanceProcAddr gpu_driver_loader(void) {
    void *lib = NULL;
    char root[400], selected[512], trial[512], failed[512];
    const char *home = SDL_GetAndroidInternalStoragePath();
    snprintf(root, sizeof root, "%s/gpu_drivers", home ? home : ".");
    snprintf(selected, sizeof selected, "%s/selected", root);
    snprintf(trial, sizeof trial, "%s/trial", root);
    snprintf(failed, sizeof failed, "%s/failed", root);
    FILE *f = home ? fopen(selected, "r") : NULL;
    if (f) {
        char folder[128] = "", file[128] = "";
        if (!fgets(folder, sizeof folder, f)) folder[0] = 0;
        if (!fgets(file, sizeof file, f)) file[0] = 0;
        fclose(f);
        chomp(folder), chomp(file);
        if (access(trial, F_OK) == 0) {  // (the last run with it drew nothing: back to the system's)
            LOG("[gpu] driver %s failed in the last run: the system's instead\n", folder);
            rename(selected, failed);
            unlink(trial);
        } else if (folder[0] && file[0]) {
            char dir[560], hooks[512] = "";
            snprintf(dir, sizeof dir, "%s/%s/", root, folder);
            Dl_info me;  // (the hooks are beside this library: the app's nativeLibraryDir)
            if (dladdr((void *)gpu_driver_loader, &me) && me.dli_fname) {
                snprintf(hooks, sizeof hooks, "%s", me.dli_fname);
                char *slash = strrchr(hooks, '/');
                if (slash) slash[1] = 0;
            }
            FILE *t = fopen(trial, "w");
            if (t) fclose(t);
            lib = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, hooks, dir, file, NULL, NULL);
            if (lib) {
                g_custom = true;
                snprintf(g_name, sizeof g_name, "%s", folder);
                snprintf(g_dir, sizeof g_dir, "/gpu_drivers/%s/", folder);
                LOG("[gpu] driver of the player's: %s (%s)\n", folder, file);
            } else {
                LOG("[gpu] driver %s could not be opened: %s; the system's instead\n", folder, dlerror());
                rename(selected, failed);
                unlink(trial);
            }
        }
    }
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    return lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : NULL;
}

// The device is made: whether the player's driver is the one running. A library that could not be opened, or one named as
// the system's (vulkan.adreno.so: Android's own interface has that one loaded already), leaves the system's driver in
// its place without any error, and the menu said the player's worked (seen on an Adreno 630 with "Adreno 840" in the
// menu and the system's 1.1.87 driver running). So the loaded libraries are looked through for one from its folder; none:
// the choice is moved to failed (DriverActivity says so), and the menu says the system's.
// (by the folder's own part of the path: the loader keeps real paths, /data/data/..., where the app was given /data/user/0/...)
static int found_in_dir(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    return info->dlpi_name && strstr(info->dlpi_name, (const char *)data) != NULL;
}

void gpu_driver_check(void) {
    if (!g_custom || dl_iterate_phdr(found_in_dir, g_dir)) return;
    LOG("[gpu] driver %s did not load (no library from %s is running): the system's driver runs instead\n", g_name, g_dir);
    char root[400], selected[512], failed[512], trial[512];
    snprintf(root, sizeof root, "%s/gpu_drivers", SDL_GetAndroidInternalStoragePath());
    snprintf(selected, sizeof selected, "%s/selected", root);
    snprintf(failed, sizeof failed, "%s/failed", root);
    snprintf(trial, sizeof trial, "%s/trial", root);
    rename(selected, failed);
    unlink(trial);
    g_custom = false, g_name[0] = 0;
}

// Frames have been drawn with it
void gpu_driver_good(void) {
    if (!g_custom) return;
    char trial[512];
    snprintf(trial, sizeof trial, "%s/gpu_drivers/trial", SDL_GetAndroidInternalStoragePath());
    unlink(trial);
    LOG("[gpu] driver %s works\n", g_name);
}

// A surface on the same libvulkan as the instance: SDL's own is the system's
bool gpu_driver_surface(SDL_Window *win, VkInstance inst, VkSurfaceKHR *out) {
    if (!g_custom) return SDL_Vulkan_CreateSurface(win, inst, NULL, out);
    struct ANativeWindow *nw = SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
    if (!nw) return SDL_SetError("no Android window");
    VkAndroidSurfaceCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = nw};
    VkResult r = vkCreateAndroidSurfaceKHR(inst, &ci, NULL, out);
    return r == VK_SUCCESS ? true : SDL_SetError("vkCreateAndroidSurfaceKHR: %d", r);
}
