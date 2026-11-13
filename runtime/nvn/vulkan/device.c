// Vulkan instance, device, swapchain and presentation (on the game window from runtime/ui.c).
//   MHGU_VK_VALIDATION=1   Khronos validation layer (MSYS2 package vulkan-validation-layers)
//   MHGU_GPU=<index>       which Vulkan device (default: [Display] GPU, a device's name; Auto: the first discrete GPU)
//   MHGU_VSYNC=0|1         0: no vsync (tearing possible, lowest latency), 1: classic vsync (FIFO). Default: the
//                          settings' VSync - On: mailbox (no tearing; the game paces itself), FIFO where there is
//                          none (the R9's driver), Off: immediate. MHGU_VK_PRESENT=fifo|mailbox|immediate picks the present mode directly.
//   MHGU_SHOT_AT=<frame>[,<frame>...]   save the window image at those presented frames as shots/frame_N.png
//   MHGU_FPS=1             frames per second in the log every 2 s
//   MHGU_VK_RESERVE_MB=<n> desktop: device memory set aside at start-up (default 1024; 0 none: see mem_reserve)
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../overlay.h"
#include "fsr.h"
#include "fsr4.h"
#include "vk.h"
#include "xess.h"
#include "../../touch.h"
#ifndef __ANDROID__
#include "../../hle/mousemenu.h"
#endif
#include "../../settings_locale.h"
void vk_wait_site(int site);  // (exec.c: the next GPU wait's site, for MHGU_VK_STATS)

VkG g_vk;
static VkDebugUtilsMessengerEXT messenger;
// Windows: the swapchain says it never wants exclusive full screen (VK_EXT_full_screen_exclusive). Otherwise NVIDIA's
// driver takes a borderless window covering the screen into exclusive mode, and alt-tab or F11 then blacks out the
// monitor until its "Vulkan/OpenGL present method" is set to "Prefer layered on DXGI Swapchain" (issue #100).
// VkSurfaceFullScreenExclusiveInfoEXT and its enum live in vulkan_win32.h, which needs windows.h: the same layout here.
typedef struct {
    VkStructureType sType;
    void *pNext;
    int32_t fullScreenExclusive;  // VkFullScreenExclusiveEXT
} FullScreenExclusiveInfo;
enum { FULL_SCREEN_EXCLUSIVE_DISALLOWED = 2 };  // VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT
static bool surface_caps2, no_exclusive;  // VK_KHR_get_surface_capabilities2 (instance), VK_EXT_full_screen_exclusive

static pthread_mutex_t g_vk_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_vk_owner;  // who holds it: exit() may run inside a submission, from the guest's own code
static volatile bool exiting;     // exit() has begun: the driver's own teardown may be one step away
static pthread_t vk_exit_thread;  // the thread running exit(): it may still enter here (the atexit handler drains)

bool vk_exiting(void) { return exiting; }

void vk_lock(void) {
    if (HITCH_ON) hitch_mutex_lock(&g_vk_mutex, HC_VKLOCK);  // ((debug) MHGU_HITCHLOG: its waits timed)
    else pthread_mutex_lock(&g_vk_mutex);
    if (exiting && pthread_self() != vk_exit_thread) {  // a late guest thread: park it away from the driver
        pthread_mutex_unlock(&g_vk_mutex);
        for (;;) sleep(3600);  // (not pause(): mingw-w64 has no pause, sleep is in both unistd.h)
    }
    if (g_vk_owner != pthread_self()) g_vk_owner = pthread_self();  // (re-entry by the holder: leave it recorded)
}

void vk_unlock(void) { pthread_mutex_unlock(&g_vk_mutex); }

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    static int n;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && n++ < 500) LOG("[vk] %s\n", d->pMessage);
    return VK_FALSE;
}

// Overwolf's implicit Vulkan layers hook vkCreateDevice and crash the process on startup, so switch them off for
// this process (the loader reads these when the instance is created). MHGU_KEEP_VK_OVERLAYS=1 leaves them alone.
static void disable_overwolf_layers(void) {
    if (getenv("MHGU_KEEP_VK_OVERLAYS")) return;
    static const char *const vars[] = {"DISABLE_VULKAN_OW_OVERLAY_LAYER", "DISABLE_VULKAN_OW_OBS_CAPTURE"};
    for (size_t i = 0; i < sizeof vars / sizeof *vars; i++)
        if (!getenv(vars[i])) SDL_setenv_unsafe(vars[i], "1", 0);
}

static uint32_t instance_api;  // the Vulkan version asked of the instance
extern bool maxwell_no_int64;   // (nvn/maxwell/translate.c)

static uint32_t msaa_floor(uint32_t s);
bool vk_gpu_adreno(void);
#ifdef __ANDROID__
PFN_vkGetInstanceProcAddr gpu_driver_loader(void);  // (platform/android_gpu.c)
bool gpu_driver_surface(SDL_Window *win, VkInstance inst, VkSurfaceKHR *out);
void gpu_driver_good(void);
void gpu_driver_check(void);  // (the device made: the player's driver is the one running, or it is given up)
#define make_surface() gpu_driver_surface(ui_window(), g_vk.inst, &g_vk.surface)
#else
#define make_surface() SDL_Vulkan_CreateSurface(ui_window(), g_vk.inst, NULL, &g_vk.surface)
#endif

// (a desktop) No Vulkan driver the game can use: the player is told what to do before the fatal line (whose own box then
// shows no second one: runtime/platform/windows.c). Windows has no Vulkan driver for the graphics of before about 2015.
void vk_no_driver_box(const char *detail) {
#ifndef __ANDROID__
    LOG("[vk] no usable Vulkan driver: %s\n", detail);
    plat_error_box(settings_text(config_language(),
                                 "This PC's graphics has no Vulkan driver, which the game needs. Update the graphics driver from "
                                 "the maker of the graphics card. Older graphics have none for Windows: Intel HD Graphics from "
                                 "before 2015 (HD 2000 to HD 6000), NVIDIA GeForce 500 or older, AMD Radeon HD 6000 or older."));
#else
    (void)detail;
#endif
}

static void create_instance(void) {
#ifdef __ANDROID__
    PFN_vkGetInstanceProcAddr loader = gpu_driver_loader();
    if (!loader) FATAL("no vkGetInstanceProcAddr: is there a Vulkan driver?\n");
    volkInitializeCustom(loader);
#elif defined(VOLK_H_)
    // (a desktop: the Vulkan loader SDL opened for the window, so the instance and SDL's surface come from the same one,
    // whatever SDL_VULKAN_LIBRARY says)
    PFN_vkGetInstanceProcAddr loader = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!loader) vk_no_driver_box("no loader"), FATAL("no Vulkan loader (%s): is the GPU's driver installed?\n", SDL_GetError());
    volkInitializeCustom(loader);
#endif
    disable_overwolf_layers();
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    if (!sdl_ext) FATAL("SDL_Vulkan_GetInstanceExtensions: %s (is the window a Vulkan window?)\n", SDL_GetError());
    const char *ext[32];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 13; i++) ext[n_ext++] = sdl_ext[i];
    uint32_t ni = 0;  // VK_EXT_full_screen_exclusive (create_device) needs this one
    vkEnumerateInstanceExtensionProperties(NULL, &ni, NULL);
    VkExtensionProperties *ip = calloc(ni ? ni : 1, sizeof *ip);
    vkEnumerateInstanceExtensionProperties(NULL, &ni, ip);
    for (uint32_t i = 0; i < ni; i++)
        if (!strcmp(ip[i].extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) surface_caps2 = true;
    free(ip);
    if (surface_caps2) ext[n_ext++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
    // MHGU_VK11=1: work as Vulkan 1.1 on a newer driver (tests what a 1.1 driver gets)
    instance_api = getenv("MHGU_VK11") ? VK_API_VERSION_1_1
                   : g_config.fsr && g_config.fsr_upscaler == FSR_UP_4 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;  // (FSR 4's shaders use SPIR-V 1.6)
    // XeSS-SR (only when it is the upscaler): libxess.dll loaded, its instance extensions and Vulkan version (xess.c)
    uint32_t n_base = n_ext, base_api = instance_api;
    bool xess = !getenv("MHGU_VK11") && g_config.fsr && g_config.fsr_upscaler == FSR_UP_XESS &&
                vk_xess_instance_exts(ext, &n_ext, 31, &instance_api);
    bool validate = getenv("MHGU_VK_VALIDATION") != NULL;
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "MHGURecomp",
                             .pEngineName = "MHGURecomp", .apiVersion = instance_api};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r == VK_ERROR_LAYER_NOT_PRESENT && validate) {  // (first: a missing validation layer is nobody else's doing)
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;  // (VK_EXT_debug_utils, the last one)
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS && xess) {  // (XeSS-SR's extensions or version refused: made as without it, and FSR 3.1 runs)
        vk_xess_creation_failed("vkCreateInstance", r);
        n_ext = n_base;
        if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        ci.enabledExtensionCount = n_ext;
        app.apiVersion = instance_api = base_api;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS && validate) {
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS) vk_no_driver_box("vkCreateInstance"), FATAL("vkCreateInstance: %d (is a Vulkan 1.1 driver installed?)\n", r);
#ifdef VOLK_H_
    volkLoadInstance(g_vk.inst);  // (through the instance, as the library's own exports go; the device's are not bypassed)
#endif
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.inst, &mi, NULL, &messenger);
    }
}

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// The devices that can run the game, one per name in the driver's order, and the one picked: the settings' GPU row
#define GPUS_MAX 16
static char gpu_names[GPUS_MAX][VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
static int gpu_count, gpu_used = -1;  // (gpu_count is stored last: the menu reads them from another thread)
int vk_gpu_list(const char **names, int max, int *in_use) {
    int n = __atomic_load_n(&gpu_count, __ATOMIC_ACQUIRE);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) names[i] = gpu_names[i];
    if (in_use) *in_use = n ? gpu_used : -1;
    return n;
}

#ifdef _WIN32
// The picked device's LUID (VkPhysicalDeviceIDProperties): the name Windows' own GPU figures go by, for the performance
// readout (platform/win_stats.c reads it from its own thread). false: no device yet, or its driver gives none
static uint8_t dev_luid[VK_LUID_SIZE];
static int dev_luid_ok;  // (stored last)
static void device_luid(void) {
    VkPhysicalDeviceIDProperties idp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &idp};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    if (!idp.deviceLUIDValid) return;
    memcpy(dev_luid, idp.deviceLUID, sizeof dev_luid);
    __atomic_store_n(&dev_luid_ok, 1, __ATOMIC_RELEASE);
}
bool vk_device_luid(uint8_t luid[8]) {
    if (!__atomic_load_n(&dev_luid_ok, __ATOMIC_ACQUIRE)) return false;
    memcpy(luid, dev_luid, sizeof dev_luid);
    return true;
}
#endif

// A queue family of d that can draw, compute and present to the window; UINT32_MAX: none
static uint32_t draw_queue(VkPhysicalDevice d) {
    uint32_t nq = 0, found = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, qp);
    for (uint32_t i = 0; i < nq && found == UINT32_MAX; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(d, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) found = i;
    }
    free(qp);
    return found;
}

// Discrete GPU first (MHGU_GPU=<index> picks one; else [Display] GPU, a device's name); needs Vulkan 1.1 and a queue that
// can draw, compute and present (one picked without it gives way to the next best). A Vulkan 1.1 driver (older phones)
// has no timeline semaphores (exec.c: fences), gets SPIR-V 1.3 (shaders.c) and no FSR.
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) vk_no_driver_box("no device"), FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    int *score = calloc(n, sizeof *score);  // (-1: can't run the game)
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    const char *env = getenv("MHGU_GPU"), *name = env ? "" : g_config.gpu;
    bool by_name = *name && SDL_strcasecmp(name, "Auto"), named = false, any = false;
    int want = env ? atoi(env) : -1, listed = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        score[i] = -1;
        if (p.apiVersion < VK_API_VERSION_1_1) continue;
        bool seen = false;  // (two of the same card: one name, the first; MHGU_GPU picks the other)
        for (int k = 0; k < listed; k++) seen |= !strcmp(gpu_names[k], p.deviceName);
        if (!seen && listed < GPUS_MAX) snprintf(gpu_names[listed++], sizeof gpu_names[0], "%s", p.deviceName);
        score[i] = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (p.apiVersion >= VK_API_VERSION_1_2) score[i] += 10;
        if ((int)i == want) score[i] = 100;
        if (by_name && !named && !SDL_strcasecmp(p.deviceName, name)) score[i] = 100, named = true;
        any = true;
    }
    if (by_name && !named) LOG("[vk] the settings' GPU \"%s\" is not here: the automatic choice\n", name);
    if (!any) vk_no_driver_box("no 1.1 device"), FATAL("no Vulkan 1.1 device\n");
    g_vk.qfam = UINT32_MAX;
    for (;;) {  // (the best first; the first of equals)
        int best = -1;
        for (uint32_t i = 0; i < n; i++)
            if (score[i] >= 0 && (best < 0 || score[i] > score[best])) best = (int)i;
        if (best < 0) vk_no_driver_box("no queue"), FATAL("no graphics+present queue\n");
        if ((g_vk.qfam = draw_queue(pd[best])) != UINT32_MAX) {
            g_vk.phys = pd[best];
            break;
        }
        LOG("[vk] gpu %d has no queue that draws in the window: the next one\n", best);
        score[best] = -1;
    }
    free(pd);
    free(score);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    for (int k = 0; k < listed; k++)
        if (!strcmp(gpu_names[k], g_vk.props.deviceName)) gpu_used = k;
    __atomic_store_n(&gpu_count, listed, __ATOMIC_RELEASE);
#ifdef _WIN32
    device_luid();
#endif
    g_vk.api12 = g_vk.props.apiVersion >= VK_API_VERSION_1_2 && instance_api >= VK_API_VERSION_1_2;
    LOG("[vk] using %s, driver %#x\n", g_vk.props.deviceName, g_vk.props.driverVersion);
}

// ((debug) MHGU_HITCHLOG) VMA's device memory allocations, timed: its blocks and dedicated allocations
static VKAPI_ATTR VkResult VKAPI_CALL timed_allocate(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *cb,
                                                     VkDeviceMemory *m) {
    uint64_t t0 = plat_now_ns();
    VkResult r = vkAllocateMemory(d, ai, cb, m);
    hitch_add(HC_VKALLOC, plat_now_ns() - t0, ai->allocationSize);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL timed_free(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *cb) {
    uint64_t t0 = plat_now_ns();
    vkFreeMemory(d, m, cb);
    hitch_add(HC_VKFREE, plat_now_ns() - t0, 0);
}

#ifndef __ANDROID__
// ---- device memory set aside at start-up (desktop)
// VMA takes device memory from the driver as the game's textures, render targets and vertex cache outgrow the blocks
// it has: a new block (256 MB; 32, 64 and 128 MB first) by vkAllocateMemory on the render thread with vk_lock held,
// 13-16 ms each on an RX 6800 XT (MHGU_HITCHLOG's vkalloc), and a hunt had taken 0.74 GB of blocks. So that memory is
// taken here, before the game runs: a VMA pool of fixed-size blocks for the memory type those resources get, made with
// its blocks (minBlockCount: kept for good), which never gives one image an allocation of its own either (VMA's
// default pools do for a big one, or for one the driver says it prefers alone). vk_create_image / vk_create_buffer
// place a resource there when it is of the pool's type and fits a block; anything else, or once the pool can't grow,
// goes to VMA's own blocks as before. A quarter of the heap's budget at most: VK_EXT_memory_budget's figure (what this
// process may use beside the others) where the driver has it, else 80% of the heap.
//   MHGU_VK_RESERVE_MB=<n>   how much (default 1024); 0: none, VMA's blocks grow in play as before
#define RESERVE_MB 1024
static bool mem_budget_ext;  // VK_EXT_memory_budget enabled
static VmaPool res_pool[2];
static uint32_t res_type[2], res_blocks[2], res_n;
static VkDeviceSize res_block;    // the pools' block size
static VkDeviceSize res_peak[2];  // the most each pool has held (logged at exit: what the reserve should be)

// The memory type VMA picks for a resource only the GPU uses (VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE without host
// access: device-local preferred, AMD's uncached types avoided) among those its requirements allow
static uint32_t gpu_mem_type(uint32_t bits) {
    VmaAllocationCreateInfo ai = {.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    uint32_t t;
    return vmaFindMemoryTypeIndex(g_vk.vma, bits, &ai, &t) == VK_SUCCESS ? t : UINT32_MAX;
}

// The type a texture (RGBA8, sampled and rendered to) or a vertex cache buffer gets here
static uint32_t probe_type(bool image) {
    VkMemoryRequirements r = {0};
    if (image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                                .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {256, 256, 1},
                                .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
        VkImage im;
        if (vkCreateImage(g_vk.dev, &ci, NULL, &im) != VK_SUCCESS) return UINT32_MAX;
        vkGetImageMemoryRequirements(g_vk.dev, im, &r);
        vkDestroyImage(g_vk.dev, im, NULL);
    } else {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 65536,
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VkBuffer b;
        if (vkCreateBuffer(g_vk.dev, &bi, NULL, &b) != VK_SUCCESS) return UINT32_MAX;
        vkGetBufferMemoryRequirements(g_vk.dev, b, &r);
        vkDestroyBuffer(g_vk.dev, b, NULL);
    }
    return r.memoryTypeBits ? gpu_mem_type(r.memoryTypeBits) : UINT32_MAX;
}

// A pool of `blocks` blocks of memory type `type`, half as many each time the driver refuses: the blocks made, 0 none
static uint32_t reserve_pool(uint32_t type, uint32_t blocks) {
    for (; blocks; blocks /= 2) {
        VmaPoolCreateInfo pi = {.memoryTypeIndex = type, .blockSize = res_block, .minBlockCount = blocks};
        if (vmaCreatePool(g_vk.vma, &pi, &res_pool[res_n]) == VK_SUCCESS) {
            res_type[res_n] = type, res_blocks[res_n++] = blocks;
            return blocks;
        }
    }
    return 0;
}

static void mem_reserve(void) {
    const char *e = getenv("MHGU_VK_RESERVE_MB");
    long mb = e ? atol(e) : RESERVE_MB;
    if (mb <= 0) { LOG("[vk] device memory: none set aside (MHGU_VK_RESERVE_MB)\n"); return; }
    uint32_t ti = probe_type(true), tb = probe_type(false);
    if (ti == UINT32_MAX) return;
    const VkPhysicalDeviceMemoryProperties *mp;
    vmaGetMemoryProperties(g_vk.vma, &mp);
    uint32_t heap = mp->memoryTypes[ti].heapIndex;
    VmaBudget bud[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(g_vk.vma, bud);
    VkDeviceSize avail = bud[heap].budget > bud[heap].usage ? bud[heap].budget - bud[heap].usage : 0;
    VkDeviceSize want = (VkDeviceSize)mb << 20;
    if (want > avail / 4) want = avail / 4;
    res_block = want >= 512ull << 20 ? 256ull << 20 : 128ull << 20;
    uint32_t blocks = (uint32_t)(want / res_block);
    // (a driver that gives buffers a type of their own: a third of the blocks for the vertex cache's)
    uint32_t nb = tb != ti && tb != UINT32_MAX && blocks >= 2 ? (blocks / 3 ? blocks / 3 : 1) : 0;
    uint32_t got = blocks > nb ? reserve_pool(ti, blocks - nb) : 0, got_b = nb ? reserve_pool(tb, nb) : 0;
    char vc[64] = "";
    if (got_b) snprintf(vc, sizeof vc, ", %u MB of type %u for the vertex cache", got_b * (uint32_t)(res_block >> 20), tb);
    if (got || got_b)
        LOG("[vk] device memory set aside: %u MB in blocks of %u MB (type %u; heap %u: %llu MB, budget %llu MB%s)%s\n",
            got * (uint32_t)(res_block >> 20), (uint32_t)(res_block >> 20), ti, heap,
            (unsigned long long)(mp->memoryHeaps[heap].size >> 20), (unsigned long long)(bud[heap].budget >> 20),
            mem_budget_ext ? "" : ", estimated", vc);
    else
        LOG("[vk] device memory: none set aside (heap %u: %llu MB, budget %llu MB%s)\n", heap,
            (unsigned long long)(mp->memoryHeaps[heap].size >> 20), (unsigned long long)(bud[heap].budget >> 20),
            mem_budget_ext ? "" : ", estimated");
}

// The reserve's pool for a resource of these requirements: of the type VMA would pick for it, and no bigger than a
// block (NULL: VMA's own blocks)
static VmaPool reserve_for(const VkMemoryRequirements *r, VkBool32 dedicated_only) {
    if (!res_n || dedicated_only || r->size > res_block) return VK_NULL_HANDLE;
    uint32_t t = gpu_mem_type(r->memoryTypeBits);
    for (uint32_t i = 0; i < res_n; i++)
        if (res_type[i] == t) return res_pool[i];
    return VK_NULL_HANDLE;
}

static void reserve_used(VmaPool p) {  // (an allocation was just made there: its peak use)
    for (uint32_t i = 0; i < res_n; i++)
        if (res_pool[i] == p) {
            VmaStatistics st;
            vmaGetPoolStatistics(g_vk.vma, p, &st);
            if (st.allocationBytes > res_peak[i]) res_peak[i] = st.allocationBytes;
        }
}

// What VMA takes from the driver (its blocks, allocations of their own), per heap. Once start-up is over each new one
// is logged, the first 20: the reserve or the staging chunks fell short, or a resource didn't fit a block
static uint64_t dm_bytes[VK_MAX_MEMORY_HEAPS];
static uint32_t dm_heap[VK_MAX_MEMORY_TYPES];
static bool dm_started;
static void VKAPI_PTR dm_allocated(VmaAllocator a, uint32_t type, VkDeviceMemory m, VkDeviceSize size, void *user) {
    (void)a, (void)m, (void)user;
    uint32_t h = dm_heap[type];
    uint64_t total = __atomic_add_fetch(&dm_bytes[h], size, __ATOMIC_RELAXED);
    static int n;
    if (__atomic_load_n(&dm_started, __ATOMIC_RELAXED) && __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 20)
        LOG("[vk] device memory taken after start-up: %.1f MB (type %u), heap %u now %llu MB\n", size / 1048576.0, type, h,
            (unsigned long long)(total >> 20));
}
static void VKAPI_PTR dm_freed(VmaAllocator a, uint32_t type, VkDeviceMemory m, VkDeviceSize size, void *user) {
    (void)a, (void)m, (void)user;
    __atomic_sub_fetch(&dm_bytes[dm_heap[type]], size, __ATOMIC_RELAXED);
}
#endif

// The bytes VMA gave images (textures and render targets), for the [mem] line: added here, taken off as each is destroyed
// (textures.c vk_collect_dead)
static _Atomic int64_t image_bytes;
void vk_image_bytes_add(int64_t n) { atomic_fetch_add_explicit(&image_bytes, n, memory_order_relaxed); }

VkResult vk_create_image(const VkImageCreateInfo *ci, VkImage *img, VmaAllocation *mem) {
#ifndef __ANDROID__
    VkImage im;
    if (res_n && vkCreateImage(g_vk.dev, ci, NULL, &im) == VK_SUCCESS) {
        VkMemoryDedicatedRequirements dr = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 r = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dr};
        VkImageMemoryRequirementsInfo2 ri = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = im};
        vkGetImageMemoryRequirements2(g_vk.dev, &ri, &r);
        VmaAllocationCreateInfo ai = {.pool = reserve_for(&r.memoryRequirements, dr.requiresDedicatedAllocation)};
        VmaAllocation a;
        if (ai.pool && vmaAllocateMemoryForImage(g_vk.vma, im, &ai, &a, NULL) == VK_SUCCESS) {
            if (vmaBindImageMemory(g_vk.vma, a, im) == VK_SUCCESS) {
                reserve_used(ai.pool);
                vk_image_bytes_add((int64_t)r.memoryRequirements.size);
                return *img = im, *mem = a, VK_SUCCESS;
            }
            vmaFreeMemory(g_vk.vma, a);
        }
        vkDestroyImage(g_vk.dev, im, NULL);  // (no room there: made again below, in VMA's own blocks)
    }
#endif
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VmaAllocationInfo info;
    VkResult r = vmaCreateImage(g_vk.vma, ci, &ai, img, mem, &info);
    if (r == VK_SUCCESS) vk_image_bytes_add((int64_t)info.size);
    return r;
}

VkResult vk_create_buffer(const VkBufferCreateInfo *bi, VkBuffer *buf, VmaAllocation *mem) {
#ifndef __ANDROID__
    VkBuffer b;
    if (res_n && vkCreateBuffer(g_vk.dev, bi, NULL, &b) == VK_SUCCESS) {
        VkMemoryDedicatedRequirements dr = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 r = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dr};
        VkBufferMemoryRequirementsInfo2 ri = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, .buffer = b};
        vkGetBufferMemoryRequirements2(g_vk.dev, &ri, &r);
        VmaAllocationCreateInfo ai = {.pool = reserve_for(&r.memoryRequirements, dr.requiresDedicatedAllocation)};
        VmaAllocation a;
        if (ai.pool && vmaAllocateMemoryForBuffer(g_vk.vma, b, &ai, &a, NULL) == VK_SUCCESS) {
            if (vmaBindBufferMemory(g_vk.vma, a, b) == VK_SUCCESS) {
                reserve_used(ai.pool);
                return *buf = b, *mem = a, VK_SUCCESS;
            }
            vmaFreeMemory(g_vk.vma, a);
        }
        vkDestroyBuffer(g_vk.dev, b, NULL);
    }
#endif
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return vmaCreateBuffer(g_vk.vma, bi, &ai, buf, mem, NULL);
}

// ---- PowerVR: one driver compile at a time
// Its shader compiler crashed building several pipelines at once: a phone that had played once closed at every later start,
// while three workers built the saved pipelines (issues #73, #99). 0.1.8 then built every shader and pipeline on the render
// thread, and the start built every saved program and pipeline there before its first picture: a black screen for minutes
// on a Helio G35 (#157). The builds stay on the workers instead (one: exec.c compile_workers), and every call into the
// driver's compiler takes compiler_lock, from any thread (the worker, the render thread's own pipelines, the overlay's):
// volk's pointers are replaced, which those callers go through, and FSR's backend, which looks the functions up itself,
// gets the same ones from vk_device_proc_addr. Never two at once, and the render thread keeps presenting while the saved
// ones build. MHGU_VK_POWERVR=1 treats another GPU as a PowerVR (this and the rest it gets).
bool vk_gpu_powervr(void) {
    static int v = -1;
    if (v < 0 && g_vk.props.deviceName[0])
        v = strstr(g_vk.props.deviceName, "PowerVR") != NULL || getenv("MHGU_VK_POWERVR") != NULL;
    return v > 0;
}

#ifdef VOLK_H_
static pthread_mutex_t compiler_lock = PTHREAD_MUTEX_INITIALIZER;
static PFN_vkCreateShaderModule drv_create_module;
static PFN_vkCreateGraphicsPipelines drv_create_graphics;
static PFN_vkCreateComputePipelines drv_create_compute;
static PFN_vkGetPipelineCacheData drv_cache_data;
static _Atomic uint64_t compiler_calls, compiler_wait_max;  // (the [mem] line: vk_mem_summary)
static void compiler_enter(void) {
    uint64_t t0 = plat_now_ns();
    pthread_mutex_lock(&compiler_lock);
    uint64_t w = plat_now_ns() - t0, m = atomic_load(&compiler_wait_max);
    while (w > m && !atomic_compare_exchange_weak(&compiler_wait_max, &m, w)) {}
    atomic_fetch_add(&compiler_calls, 1);
}
static VKAPI_ATTR VkResult VKAPI_CALL one_create_module(VkDevice d, const VkShaderModuleCreateInfo *ci,
                                                        const VkAllocationCallbacks *a, VkShaderModule *m) {
    compiler_enter();
    VkResult r = drv_create_module(d, ci, a, m);
    pthread_mutex_unlock(&compiler_lock);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL one_create_graphics(VkDevice d, VkPipelineCache c, uint32_t n,
                                                          const VkGraphicsPipelineCreateInfo *ci,
                                                          const VkAllocationCallbacks *a, VkPipeline *p) {
    compiler_enter();
    VkResult r = drv_create_graphics(d, c, n, ci, a, p);
    pthread_mutex_unlock(&compiler_lock);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL one_create_compute(VkDevice d, VkPipelineCache c, uint32_t n,
                                                         const VkComputePipelineCreateInfo *ci,
                                                         const VkAllocationCallbacks *a, VkPipeline *p) {
    compiler_enter();
    VkResult r = drv_create_compute(d, c, n, ci, a, p);
    pthread_mutex_unlock(&compiler_lock);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL one_cache_data(VkDevice d, VkPipelineCache c, size_t *size, void *data) {
    pthread_mutex_lock(&compiler_lock);  // (the cache the compiles write to: not read while one runs)
    VkResult r = drv_cache_data(d, c, size, data);
    pthread_mutex_unlock(&compiler_lock);
    return r;
}
// The upscalers' SDKs look the device's functions up themselves (FSR 3.1's backend asks getDeviceProcAddr for
// vkCreateShaderModule and vkCreateComputePipelines, and builds its pipelines on the render thread when its context is
// made), so they get the ones above too: never beside the worker's build
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL one_device_proc(VkDevice d, const char *name) {
    PFN_vkVoidFunction f = !strcmp(name, "vkCreateShaderModule") ? (PFN_vkVoidFunction)one_create_module
                         : !strcmp(name, "vkCreateGraphicsPipelines") ? (PFN_vkVoidFunction)one_create_graphics
                         : !strcmp(name, "vkCreateComputePipelines") ? (PFN_vkVoidFunction)one_create_compute
                         : !strcmp(name, "vkGetPipelineCacheData") ? (PFN_vkVoidFunction)one_cache_data : NULL;
    if (!f) return vkGetDeviceProcAddr(d, name);
    LOG("[vk] PowerVR: %s handed to an upscaler takes the compiler's lock\n", name);
    return f;
}
#endif

PFN_vkGetDeviceProcAddr vk_device_proc_addr(void) {
#ifdef VOLK_H_
    if (drv_create_graphics) return one_device_proc;  // (PowerVR: one compile at a time)
#endif
    return vkGetDeviceProcAddr;
}

static void one_compiler_install(void) {
#ifdef VOLK_H_
    if (!vk_gpu_powervr() || drv_create_graphics) return;
    drv_create_module = vkCreateShaderModule, vkCreateShaderModule = one_create_module;
    drv_create_graphics = vkCreateGraphicsPipelines, vkCreateGraphicsPipelines = one_create_graphics;
    drv_create_compute = vkCreateComputePipelines, vkCreateComputePipelines = one_create_compute;
    drv_cache_data = vkGetPipelineCacheData, vkGetPipelineCacheData = one_cache_data;
    LOG("[vk] PowerVR: one shader / pipeline build at a time, on one worker (its compiler crashed building several at once)%s\n",
        strstr(g_vk.props.deviceName, "PowerVR") ? "" : " (MHGU_VK_POWERVR)");
#endif
}

static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[32];
    uint32_t n_ext = 0;
    if (!g_vk.api12 && g_config.fsr) {
        LOG("[vk] FSR needs Vulkan 1.2: off\n");
        g_config.fsr = 0;
    }
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    bool mirror_ext = !g_vk.api12 && has_ext(ep, ne, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);  // (in 1.2: a feature)
    if (mirror_ext) ext[n_ext++] = VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME;
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.stencil_export = has_ext(ep, ne, VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);  // (MSAA stencil resolve)
    if (g_vk.stencil_export) ext[n_ext++] = VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME;
    no_exclusive = surface_caps2 && has_ext(ep, ne, "VK_EXT_full_screen_exclusive") && !getenv("MHGU_VK_EXCLUSIVE");
    if (no_exclusive) ext[n_ext++] = "VK_EXT_full_screen_exclusive";
#ifdef __ANDROID__
    if (vk_pacing_device_ext(ep, ne)) ext[n_ext++] = VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME;  // (MHGU_PERF_PRESENT_TIME)
#endif
#ifndef __ANDROID__
    mem_budget_ext = has_ext(ep, ne, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);  // (how much mem_reserve may set aside)
    if (mem_budget_ext) ext[n_ext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
#endif
    bool fsr4 = g_config.fsr && g_config.fsr_upscaler == FSR_UP_4 && vk_fsr4_device_exts(ep, ne, ext, &n_ext);  // FSR 4: its extensions
    uint32_t n_ext_base = n_ext;  // XeSS-SR: its extensions (only when it is the upscaler; xess.c)
    bool xess = g_config.fsr && g_config.fsr_upscaler == FSR_UP_XESS && vk_xess_device_exts(ep, ne, ext, &n_ext, 32);
    free(ep);

    // optional features: whatever the GPU has of what the backend can use
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
                                                             .pNext = g_vk.divisor ? &div_have : NULL};
    void *ext_have = g_vk.custom_border ? (void *)&cbc_have : g_vk.divisor ? (void *)&div_have : NULL;
    VkPhysicalDeviceVulkan12Features f12_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = ext_have};
    VkPhysicalDeviceVulkan11Features f11_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12_have};
    // (these two structures belong to Vulkan 1.2: a 1.1 driver gets neither, here or below)
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                      .pNext = g_vk.api12 ? (void *)&f11_have : ext_have};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    g_vk.has_timeline = g_vk.api12 && f12_have.timelineSemaphore;
    g_vk.mirror_clamp = g_vk.api12 ? f12_have.samplerMirrorClampToEdge : mirror_ext;
    maxwell_no_int64 = !have.features.shaderInt64 || getenv("MHGU_VK_NO_INT64");  // (the env: tests)
    // The translator's performance switches, read once here, before any program is translated; on unless set to 0.
    // NMin / NMax / NClamp only on Vulkan 1.2 drivers but a PowerVR's, until others are checked (old Adreno drivers are 1.1)
    {
        const char *e;
        // (unset: on, measured in 0.1.9's perf pass; 0 the old way)
        bool nmm = (e = getenv("MHGU_PERF_NMINMAX")) ? atoi(e) > 0 : true;
        maxwell_nminmax = nmm && g_vk.api12 && !vk_gpu_powervr();
        // (VSTRIM, and the render pass and descriptor switches in exec.c and fsr.c: off by default on a Vulkan 1.1 driver,
        // where none was ever checked; a Mali-G57 on 1.1.177 drew the 3D picture almost black with 0.1.9's defaults.)
        // IPA stays on there: without it a varying the shader multiplies by 1 / w is read as v * gl_FragCoord.w * (1 / w),
        // a round trip through position w that an Adreno 618 on 1.1.128 doesn't survive far away - with 0.1.9.1 the
        // Volcano's distant plants and trees were black bars and white noise, with 0.1.9 (IPA on) they were right. The
        // varying read as it is is also the exact value
        maxwell_ipa_fold = (e = getenv("MHGU_PERF_IPA")) ? atoi(e) > 0 : true;
        maxwell_vs_trim = (e = getenv("MHGU_PERF_VSTRIM")) ? atoi(e) > 0 : g_vk.api12;
        if (nmm || maxwell_ipa_fold || maxwell_vs_trim)
            LOG("[shader] translator switches: NMINMAX %d%s, IPA %d, VSTRIM %d\n", maxwell_nminmax,
                nmm && !maxwell_nminmax ? " (asked: not on this driver)" : "", maxwell_ipa_fold, maxwell_vs_trim);
    }
    if (!g_vk.has_timeline) {
        VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        LOG("[vk] Vulkan 1.1 path: fences for the submissions, SPIR-V 1.3, mirror clamp %d, shaderInt64 %d, subgroups: size %u, "
            "stages 0x%x, operations 0x%x\n", g_vk.mirror_clamp, have.features.shaderInt64, sg.subgroupSize, sg.supportedStages,
            sg.supportedOperations);
    }
    g_vk.divisor = g_vk.divisor && div_have.vertexAttributeInstanceRateDivisor;
    g_vk.custom_border = g_vk.custom_border && cbc_have.customBorderColors && cbc_have.customBorderColorWithoutFormat;
    // (tests: a GPU without them, as an Adreno 640 on 1.1.128 has no custom border colours and a Mali no divisor: #171)
    if (getenv("MHGU_VK_NO_DIVISOR")) g_vk.divisor = false;
    if (getenv("MHGU_VK_NO_BORDER")) g_vk.custom_border = false;

    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, .vertexAttributeInstanceRateDivisor = VK_TRUE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = g_vk.has_timeline, .shaderInt8 = f12_have.shaderInt8,
                                            .samplerMirrorClampToEdge = f12_have.samplerMirrorClampToEdge,
                                            .uniformAndStorageBuffer8BitAccess = f12_have.uniformAndStorageBuffer8BitAccess,
                                            .storageBuffer8BitAccess = f12_have.storageBuffer8BitAccess,
                                            .scalarBlockLayout = f12_have.scalarBlockLayout,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = f11_have.storageBuffer16BitAccess, .pNext = &f12};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                   .pNext = g_vk.api12 ? (void *)&f11 : g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->logicOp = h->logicOp;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
    b->textureCompressionASTC_LDR = h->textureCompressionASTC_LDR;
    b->shaderInt64 = !maxwell_no_int64;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    g_vk.depth_clamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = g_vk.sample_shading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    b->shaderClipDistance = h->shaderClipDistance;
    b->multiViewport = h->multiViewport;
    if (g_config.fsr) {  // FSR: its shaders write images declared without a format, and have 16-bit float variants
        b->shaderStorageImageWriteWithoutFormat = g_vk.fsr_storage_wo_format = h->shaderStorageImageWriteWithoutFormat;
        b->shaderStorageImageReadWithoutFormat = h->shaderStorageImageReadWithoutFormat;
        f12.shaderFloat16 = b->shaderInt16 = g_vk.fsr_fp16 = f12_have.shaderFloat16 && h->shaderInt16;  // (both, or neither)
    }
    if (fsr4) vk_fsr4_device_features(&f11, &f12, b);  // FSR 4: cooperative matrix, float8... (only when asked for)
    // XeSS-SR: its features merged into the chain by hand (only when it is the upscaler). The chain as it was is kept, for
    // a device made without it if vkCreateDevice refuses them
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f_keep = f;
    VkPhysicalDeviceVulkan11Features f11_keep = f11;
    VkPhysicalDeviceVulkan12Features f12_keep = f12;
    xess = xess && vk_xess_device_features(&f, &f11, &f12, &f13, instance_api >= VK_API_VERSION_1_3 &&
                                                                     g_vk.props.apiVersion >= VK_API_VERSION_1_3);
    if (!xess) n_ext = n_ext_base;
    if (maxwell_no_int64) LOG("[vk] no shaderInt64: the shaders do their 64-bit math in 32 bits\n");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi[2] = {{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                      .queueCount = 1, .pQueuePriorities = &prio}};
    uint32_t nq = xess ? vk_xess_device_queues(qi, 1, 2) : 1;  // (XeSS-SR: a compute queue too, xess.c)
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = nq,
                             .pQueueCreateInfos = qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    if (!xess) {
        VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
    } else {
        VkResult dr = vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev);
        vk_xess_device_done();
        if (dr != VK_SUCCESS) {  // (XeSS-SR's additions refused: the device as without it, and FSR 3.1 runs)
            vk_xess_creation_failed("vkCreateDevice", dr);
            f = f_keep, f11 = f11_keep, f12 = f12_keep;
            di.enabledExtensionCount = n_ext = n_ext_base;
            di.queueCreateInfoCount = 1;
            VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
        }
    }
    one_compiler_install();  // (PowerVR: before anything is compiled)
#ifdef __ANDROID__
    gpu_driver_check();
    vk_pacing_device();  // (MHGU_PERF_PRESENT_TIME: present_pacing.c)
    {   // (Vulkan 1.2's, and only by the device: the libvulkan of an older Android doesn't know them)
        PFN_vkWaitSemaphores wait = (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(g_vk.dev, "vkWaitSemaphores");
        PFN_vkGetSemaphoreCounterValue value = (PFN_vkGetSemaphoreCounterValue)vkGetDeviceProcAddr(g_vk.dev, "vkGetSemaphoreCounterValue");
        if (!wait) wait = (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(g_vk.dev, "vkWaitSemaphoresKHR");
        if (!value) value = (PFN_vkGetSemaphoreCounterValue)vkGetDeviceProcAddr(g_vk.dev, "vkGetSemaphoreCounterValueKHR");
        vkWaitSemaphores = wait, vkGetSemaphoreCounterValue = value;
    }
#endif
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    LOG("[vk] features: divisor %d, custom border %d, BC %d, ASTC %d, exclusive full screen off %d\n", g_vk.divisor,
        g_vk.custom_border, h->textureCompressionBC, h->textureCompressionASTC_LDR, no_exclusive);
    {   // (a player's log tells what the GPU allows: no PowerVR here to read it from, #100)
        const VkPhysicalDeviceLimits *l = &g_vk.props.limits;
        LOG("[vk] limits: vertex streams %u, inputs %u; a stage: uniform buffers %u, storage buffers %u, textures %u, samplers %u, "
            "%u in all; images up to %u, samplers %u, allocations %u\n", l->maxVertexInputBindings, l->maxVertexInputAttributes,
            l->maxPerStageDescriptorUniformBuffers, l->maxPerStageDescriptorStorageBuffers, l->maxPerStageDescriptorSampledImages,
            l->maxPerStageDescriptorSamplers, l->maxPerStageResources, l->maxImageDimension2D, l->maxSamplerAllocationCount,
            l->maxMemoryAllocationCount);
    }

    // MSAA (settings): as many samples as asked for that colour and depth attachments both support and shaders can
    // read from depth (its resolve is a shader: exec.c vk_ms_resolve)
    const VkPhysicalDeviceLimits *lim = &g_vk.props.limits;
    VkSampleCountFlags counts = lim->framebufferColorSampleCounts & lim->framebufferDepthSampleCounts &
                                lim->framebufferStencilSampleCounts & lim->sampledImageDepthSampleCounts;
    if (g_vk.stencil_export) counts &= lim->sampledImageStencilSampleCounts;
    g_vk.msaa = 1;
    g_vk.msaa_counts = counts & (VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT);
    for (uint32_t s = (uint32_t)g_config.msaa; s > 1 && g_vk.msaa == 1; s >>= 1)
        if (g_vk.msaa_counts & s) g_vk.msaa = s;
    if (g_vk.msaa != (uint32_t)g_config.msaa) LOG("[vk] MSAA %dx not supported: %ux\n", g_config.msaa, g_vk.msaa);
    g_vk.msaa = msaa_floor(g_vk.msaa);

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = g_vk.api12 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1,
                                 // (blocks of 32 MB, not VMA's 256: a phone holds a block whole however little of it is
                                 // used; 7 blocks, 928 MB, and vkQueueSubmit out of device memory on a 2.7 GB phone)
                                 .preferredLargeHeapBlockSize = 32ull << 20};
    // PowerVR (BXM-8-256, Dimensity 7020 to 7300): games closed after 5 to 15 minutes, out of the process's memory map
    // entries (mprotect: "out of memory" on a phone with 11 GB; pipelines failing with OUT_OF_HOST_MEMORY), while the
    // write tracking had armed only 16 runs. A driver that prefers an allocation of its own for each image gets one
    // VkDeviceMemory each from VMA, and one mapping each on such a GPU. Told it is Vulkan 1.0, VMA never asks: images
    // share its blocks, a few hundred megabytes each, as on the GPUs where the map entries stay flat
    if (vk_gpu_powervr() && !getenv("MHGU_VMA_DEDICATED")) {
        ai.vulkanApiVersion = VK_API_VERSION_1_0;
        LOG("[vk] PowerVR: images share VMA's memory blocks (no allocation of their own)\n");
    }
    static VmaVulkanFunctions timed = {.vkAllocateMemory = timed_allocate, .vkFreeMemory = timed_free};
    if (HITCH_ON) ai.pVulkanFunctions = &timed;  // (the others: VMA's own, as without it)
#ifndef __ANDROID__
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) dm_heap[i] = mp.memoryTypes[i].heapIndex;
    static const VmaDeviceMemoryCallbacks dm = {.pfnAllocate = dm_allocated, .pfnFree = dm_freed};
    ai.pDeviceMemoryCallbacks = &dm;
    if (mem_budget_ext) ai.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
#endif
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));
#ifndef __ANDROID__
    mem_reserve();
#endif

    if (g_vk.has_timeline) {
        VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
        VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    }
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));

    // pipeline cache: cache/vk_pipelines.bin (written at exit)
    size_t sz = 0;
    void *data = NULL;
    FILE *fp = fopen("cache/vk_pipelines.bin", "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        sz = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = malloc(sz);
        if (fread(data, 1, sz, fp) != sz) sz = 0;
        fclose(fp);
    }
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz, .pInitialData = data};
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        pc.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    free(data);
}

static void pipeline_cache_save(void) {
    size_t sz = 0;
    void *data = NULL;
    VkResult res = VK_INCOMPLETE;
    // (the shader workers may add pipelines between the size and the data: VK_INCOMPLETE, asked again with the new size)
    for (int tries = 0; tries < 3 && res == VK_INCOMPLETE; tries++) {
        free(data), data = NULL;
        if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) != VK_SUCCESS || !sz) return;
        sz += 64 * 1024;  // (room for what is added meanwhile)
        if (!(data = malloc(sz))) return;
        res = vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data);
    }
    if (res == VK_SUCCESS) {
        mkdir("cache", 0755);
        FILE *f = fopen("cache/vk_pipelines.bin.tmp", "wb");
        if (f) {
            bool ok = fwrite(data, 1, sz, f) == sz;
            fclose(f);
            if (ok) { remove("cache/vk_pipelines.bin"); rename("cache/vk_pipelines.bin.tmp", "cache/vk_pipelines.bin"); }
        }
    }
    free(data);
}

// An Adreno on a Vulkan 1.1 driver (seen: Adreno 710, 1.1.128) shows the 3D picture repeated in a grid over the screen with
// a small 3D picture and no MSAA, in a hunt; with 2x it is right (the report's own test: 894x402). Why is not known and no
// device here does it, so those drivers keep 2x below 720 lines, and nothing else changes
// [Performance] AdrenoFix Off: never (an Adreno 610 on 1.1.128 was dark and flickering with it, at 40 to 70%; and 2x costs)
static uint32_t msaa_floor(uint32_t s) {
    if (s > 1 || g_vk.api12 || !vk_gpu_adreno() || g_config.render_h >= 720 || !(g_vk.msaa_counts & 2)) return s;
    if (g_config.adreno_fix) {
        static bool said;
        if (!said) LOG("[vk] MSAA as set: AdrenoFix is off (this driver may repeat a small picture without MSAA)\n");
        said = true;
        return s;
    }
    static bool told;
    if (!told) LOG("[vk] MSAA 2x kept: this Adreno driver repeats a small picture without it\n");
    told = true;
    return 2;
}
// (tests: MHGU_VK_ADRENO=1 takes the Adreno-only paths on another GPU - one sample's alpha to coverage as an alpha test,
// AdrenoFix's MSAA floor on a Vulkan 1.1 driver, the clears as draws under 1.1.128 - and MHGU_VK_ADRENO=0 leaves them out on
// an Adreno: a Mali phone can show what they do, issues #70, #161, #171)
bool vk_gpu_adreno(void) {
    static int v = -1;
    if (v < 0) {
        if (!g_vk.props.deviceName[0]) return false;  // (no device chosen yet)
        const char *e = getenv("MHGU_VK_ADRENO");
        v = e ? atoi(e) != 0 : strstr(g_vk.props.deviceName, "Adreno") != NULL;
        if (e) LOG("[vk] MHGU_VK_ADRENO=%d: the Adreno-only paths %s\n", v, v ? "taken" : "left out");
    }
    return v;
}
bool vk_gpu_adreno_fix_applies(void) { return vk_gpu_adreno() && !g_vk.api12; }  // ([Performance] AdrenoFix: its menu row)

// (platform/android_perf.c, the minute's [mem] line) VMA's VkDeviceMemory blocks, its allocations and their bytes, and
// what uses them: images (textures, render targets), the vertex cache and the staging ring (exec.c)
void vk_mem_summary(char *out, size_t size) {
    out[0] = 0;
    if (!g_vk.vma) return;
    VmaTotalStatistics t;
    vmaCalculateStatistics(g_vk.vma, &t);
    uint64_t staging = 0, vc = 0;
    int chunks = 0;
    vk_exec_mem(&staging, &chunks, &vc);
    int64_t img = atomic_load_explicit(&image_bytes, memory_order_relaxed), bcn = vk_bcn_bytes();
    int n = snprintf(out, size,
                     ", GPU memory %u blocks, %u allocations, %llu MB (used %llu: images %lld, of which decoded BC %lld, vertex cache %llu, "
                     "staging %llu in %d)",
                     t.total.statistics.blockCount, t.total.statistics.allocationCount,
                     (unsigned long long)(t.total.statistics.blockBytes >> 20), (unsigned long long)(t.total.statistics.allocationBytes >> 20),
                     (long long)(img > 0 ? img >> 20 : 0), (long long)(bcn > 0 ? bcn >> 20 : 0), (unsigned long long)(vc >> 20),
                     (unsigned long long)(staging >> 20), chunks);
#ifdef VOLK_H_
    if (drv_create_graphics && n > 0 && (size_t)n < size)  // (PowerVR: the driver's compiles so far, one at a time)
        snprintf(out + n, size - (size_t)n, ", %llu compiles one at a time (longest wait %.0f ms)",
                 (unsigned long long)atomic_load(&compiler_calls), atomic_load(&compiler_wait_max) / 1e6);
#else
    (void)n;
#endif
}

// MSAA can't change while FSR runs (it does the anti-aliasing) or with Low-end mode (it has none). A desktop's plain
// scaling below 100% without either (fsr.c: g_fsr_on, no FSR) keeps the player's MSAA, as it is at 100%. A desktop's
// Low-end mode locks it whether or not it scales (at 720 lines or fewer its RenderScale stays 100: no plain scaling)
static bool msaa_locked(void) {
#ifdef __ANDROID__
    if (g_fsr_on) LOG("[vk] MSAA stays off while FSR is on\n");
    return g_fsr_on;
#else
    bool locked = g_config.low_end || (g_fsr_on && g_config.fsr);
    if (locked) LOG("[vk] MSAA stays off while %s is on\n", g_config.low_end ? "Low-end mode" : "FSR");
    return locked;
#endif
}

// F7 (host_input.c): MSAA off -> 2x -> 4x -> 8x (those the GPU has) -> off, from the next pass on.
void vk_cycle_msaa(void) {
    if (msaa_locked()) return;
    uint32_t s = g_vk.msaa;
    do s = s >= 8 ? 1 : s * 2; while (s > 1 && !(g_vk.msaa_counts & s));
    g_vk.msaa = s;
    LOG("[vk] MSAA %ux\n", s);
}

// MHGU_CONTROL "msaa <n>": as many samples as the GPU allows up to n (test runs).
void vk_set_msaa(uint32_t n) {
    if (msaa_locked()) return;
    uint32_t s = 1;
    for (uint32_t c = 2; c <= 8; c *= 2) if (c <= n && (g_vk.msaa_counts & c)) s = c;
    g_vk.msaa = s = msaa_floor(s);
    LOG("[vk] MSAA %ux\n", s);
}

// ================================================================== swapchain
static int swapchain_vsync = -1;
static int configured_msaa = -1;
static bool swapchain_paced;  // FIFO, its presents timed to the display's vsyncs (Android: present_pacing.c)
#ifdef __ANDROID__
// Should the swapchain be the timed one now? (MHGU_PERF_PRESENT_TIME, the settings' VSync on, the FPS limit a whole fraction
// of the display's rate; never with MHGU_VK_PRESENT or MHGU_VSYNC given)
static bool paced_wanted(void) {
    static int env = -1;
    if (env < 0) env = getenv("MHGU_VK_PRESENT") || getenv("MHGU_VSYNC");
    return !env && config_live_vsync() && vk_pacing_fifo();
}
#endif
static VkPresentModeKHR present_mode(void) {
    swapchain_vsync = config_live_vsync();
    // MAILBOX by default: no tearing, and the game paces itself (its own 30 fps vsync waits); with FIFO the game's frames
    // also queued behind the display's, dropping to ~26 fps on the R9. FIFO where there is no mailbox.
    const char *e = getenv("MHGU_VK_PRESENT"), *v = getenv("MHGU_VSYNC");
    if (!e && v) e = atoi(v) ? "fifo" : "immediate";
    swapchain_paced = false;
#ifdef __ANDROID__
    // (FIFO for presents timed to the display: MAILBOX would let a newer picture replace one waiting for its time)
    if (!e && paced_wanted()) e = "fifo", swapchain_paced = true;
#endif
    if (!e) e = swapchain_vsync ? "mailbox" : "immediate";  // (mhgurecomp.ini VSync)
    VkPresentModeKHR want = !strcmp(e, "fifo") ? VK_PRESENT_MODE_FIFO_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    LOG("[vk] present mode %s not available: FIFO (vsync)\n", e);
    return VK_PRESENT_MODE_FIFO_KHR;
}

static VkRenderPass sc_pass;
static VkFramebuffer sc_fb[VK_SC_MAX];
// The window image drawn turned for the panel (Android, MHGU_PERF_PREROTATE=1): a phone's panel is portrait, and a picture
// presented upright in landscape (IDENTITY) is turned by the system on every frame. Where the display hardware turns it
// as it scans out (an Anbernic RG406V's DPU) that costs nothing, but a Moto g06's MediaTek composer sends the layer through
// its MDP block ("MM" in dumpsys SurfaceFlinger), which reads the whole picture and writes a turned copy every frame. So
// the swapchain takes the surface's own transform and portrait size, and its one pass draws turned:
// the game's picture (exec.c vk_present_draw), the overlay (overlay.cpp) and a screenshot's pixels turned back upright.
// sc_extent stays the window's (landscape) size, what everything is drawn for; sc_phys is the images' own.
// Only at 90 degrees (transform 0x2: the Moto, the Anbernic and the Galaxy as they are usually held): there the screen
// shows the same pixels as upright (the settings menu held over a frame, both ways: 0 of 691200 differ). At 270 (0x8)
// a pixel centre right on an edge goes to the other side of it (Vulkan draws one on a top or left edge, and the turn makes
// ImGui's glyph edges, which sit on half pixels across, bottom and top ones): 266 pixels of the menu's text differed, up
// to 48 levels. There the system keeps turning the picture, as before ("prerotate 2" in the control file tries it).
static int sc_turn;           // quarter turns the window image is drawn with: 0, 1 (90 degrees) or 3 (270)
static VkExtent2D sc_phys;    // the window images' own size (sc_extent turned when sc_turn is odd)
static VkSurfaceTransformFlagBitsKHR sc_transform;  // the swapchain's preTransform
int vk_window_turn(void) { return sc_turn; }
#ifdef __ANDROID__
int g_prerotate_test = -1;  // (MHGU_CONTROL "prerotate 0|1|2": tests, the same frame both ways, 2 at 270 too; -1: the switch)
static bool sc_wants_turn;     // (the switch or a test on: the panel's transform looked at again at a SUBOPTIMAL)
// The quarter turns to draw with for the surface as it is now: 1 at 90 degrees with the switch on, else 0
static int turn_wanted(const VkSurfaceCapabilitiesKHR *caps) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MHGU_PERF_PREROTATE");
        on = e ? *e == '1' : 1;  // (unset: on)
    }
    int t = __atomic_load_n(&g_prerotate_test, __ATOMIC_RELAXED), want = t >= 0 ? t : on;
    VkSurfaceTransformFlagsKHR cur = caps->currentTransform;
    if (!want || !(caps->supportedTransforms & cur)) return 0;
    return cur == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ? 1 : cur == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR && want == 2 ? 3 : 0;
}
#endif

static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
#ifdef __ANDROID__
    // (the app in the background: its window, and with it the surface, is gone until it comes back: no swapchain)
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps) != VK_SUCCESS) {
        LOG("[vk] no surface (app in the background)\n");
        return;
    }
#else
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps));
#endif
    int w, h;
    SDL_GetWindowSizeInPixels(ui_window(), &w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
#ifdef __ANDROID__
    // A phone held sideways: the surface reports the panel's own (portrait) size, with a 90/270 degree transform or
    // (a Galaxy S25 on Android 17) with none at all. The picture is drawn upright at the window's own size, which is
    // what the activity shows (landscape), and the compositor turns it (identity): the size is the window's, not the
    // one the surface reports (a portrait buffer in a landscape window came out squeezed, and as frame_begin compares
    // the swapchain with the window it was made again every frame).
    static int logged;
    if (!logged++)
        LOG("[vk] surface: current %ux%u, transform 0x%x (supported 0x%x), extent %ux%u..%ux%u; window %dx%d\n",
            caps.currentExtent.width, caps.currentExtent.height, caps.currentTransform, caps.supportedTransforms,
            caps.minImageExtent.width, caps.minImageExtent.height, caps.maxImageExtent.width, caps.maxImageExtent.height, w, h);
    if (w > 0 && h > 0) {
        ext.width = (uint32_t)w, ext.height = (uint32_t)h;
        if (caps.maxImageExtent.width && caps.maxImageExtent.height) {  // (within what the surface takes)
            if (ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
            if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
        }
        if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    } else if (caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) {
        uint32_t t = ext.width;
        ext.width = ext.height, ext.height = t;
    }
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sc_turn = w > 0 && h > 0 ? turn_wanted(&caps) : 0;
    {
        VkSurfaceCapabilitiesKHR any = caps;
        any.currentTransform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR, any.supportedTransforms = ~0u;
        sc_wants_turn = turn_wanted(&any) != 0;  // (the switch on: a swapchain left upright looks again at a SUBOPTIMAL)
    }
    if (sc_turn) transform = caps.currentTransform;  // (MHGU_PERF_PREROTATE: drawn turned for the panel)
#endif
    VkExtent2D phys = sc_turn ? (VkExtent2D){ext.height, ext.width} : ext;
    // (Android drivers often only offer INHERIT)
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        alpha = caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
              : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    if (!ext.width || !ext.height) return;  // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote: a UNORM swapchain (sRGB game images are read through UNORM views)
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    uint32_t count = caps.minImageCount + 1 < 3 ? 3 : caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = caps.minImageCount > 8 ? caps.minImageCount : 8;
    if (getenv("MHGU_VK_IMAGES")) count = (uint32_t)atoi(getenv("MHGU_VK_IMAGES"));  // (tests)
    VkSwapchainKHR old = g_vk.swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = phys, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = transform,
        .compositeAlpha = alpha, .presentMode = present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old};
    FullScreenExclusiveInfo fse = {.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT,
                                   .fullScreenExclusive = FULL_SCREEN_EXCLUSIVE_DISALLOWED};
    if (no_exclusive) ci.pNext = &fse;
#ifdef __ANDROID__
    VkResult cr = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain);
    if (cr != VK_SUCCESS) {
        LOG("[vk] vkCreateSwapchainKHR: %d (app in the background?)\n", cr);
        if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
        g_vk.swapchain = VK_NULL_HANDLE;
        return;
    }
#else
    VK_CHECK(vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain));
#endif
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        vkDestroyFramebuffer(g_vk.dev, sc_fb[i], NULL);
        vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    }
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    if (sc_pass && sf.format != g_vk.sc_format) { vkDestroyRenderPass(g_vk.dev, sc_pass, NULL); sc_pass = VK_NULL_HANDLE; }
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    sc_phys = phys, sc_transform = transform;
    if (!sc_pass) {  // the window image: cleared, then presented (copied from for screenshots first)
        VkAttachmentDescription a = {.format = sf.format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                     .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                     .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref};
        VkSubpassDependency dep = {.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
                                   .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                   .srcAccessMask = VK_ACCESS_ALL_WRITES,
                                   .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT};
        VkRenderPassCreateInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &a,
                                     .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 1, .pDependencies = &dep};
        VK_CHECK(vkCreateRenderPass(g_vk.dev, &rp, NULL, &sc_pass));
    }
    g_vk.sc_count = VK_SC_MAX;
    if (vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images) != VK_SUCCESS)
        FATAL("the window has more than %d images\n", VK_SC_MAX);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
        VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = sc_pass, .attachmentCount = 1,
                                      .pAttachments = &g_vk.sc_views[i], .width = phys.width, .height = phys.height, .layers = 1};
        VK_CHECK(vkCreateFramebuffer(g_vk.dev, &fi, NULL, &sc_fb[i]));
    }
    LOG("[vk] swapchain %ux%u, %u images (asked for %u, least %u), format %d%s%s\n", ext.width, ext.height, g_vk.sc_count, count,
        caps.minImageCount, sf.format, swapchain_paced ? ", FIFO with presents timed to the display" : "",
        sc_turn == 1 ? ", drawn turned 90 degrees for the panel" : sc_turn == 3 ? ", drawn turned 270 degrees for the panel" : "");
#ifdef __ANDROID__
    vk_pacing_swapchain();  // (the display's period, the cadence anew)
#endif
}

static void recreate_swapchain(void) {
    uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
    vk_wait_seq(vk_pending_seq() - 1);
    vkDeviceWaitIdle(g_vk.dev);
    create_swapchain();
    if (HITCH_ON) hitch_add(HC_SWAPCHAIN, plat_now_ns() - t0, 0);
}

VkRenderPass vk_window_pass(void) { return sc_pass; }

// ================================================================== frames
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[VK_SC_MAX];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

typedef struct { uint32_t image; VkSemaphore acquired; bool ok; } Frame;
static Frame cur_frame;

// Acquire the next window image and begin its render pass (cleared to black). false: nothing to draw into.
#ifdef __ANDROID__
// The activity's window comes and goes (runtime/ui.c: 1 in the background, 2 back with a new window): nothing is
// drawn meanwhile, and on return the surface and swapchain are made again for the new window.
volatile int g_android_window_state;
volatile uint64_t g_android_bg_ns;
static void android_window_check(void) {
    int st = __atomic_load_n(&g_android_window_state, __ATOMIC_ACQUIRE);
    if (st != 2) return;
    vk_wait_seq(vk_pending_seq() - 1);
    vkDeviceWaitIdle(g_vk.dev);
    if (g_vk.swapchain) vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
    g_vk.swapchain = VK_NULL_HANDLE;
    if (g_vk.surface) vkDestroySurfaceKHR(g_vk.inst, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    if (!make_surface()) {
        static int nlog;
        if (nlog++ < 5) LOG("[vk] SDL_Vulkan_CreateSurface after resume: %s\n", SDL_GetError());
        return;  // (tried again next frame)
    }
    create_swapchain();
    if (g_vk.swapchain) {
        int was = 2;  // (unless it went to the background again meanwhile: that stays for frame_begin)
        __atomic_compare_exchange_n(&g_android_window_state, &was, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        LOG("[vk] window back after %.0f s: surface and swapchain made again\n", g_android_bg_ns ? (plat_now_ns() - g_android_bg_ns) / 1e9 : 0.0);
    }
}

// Android ends a backgrounded app with SIGKILL, no atexit: the pipeline cache is kept as it goes there, once (under
// vk_lock). The first frame presented in the background does it, or the game's update thread as it stops there
// (vk_background_save, runtime/hle/vi.c: [Application] PauseInBackground), whichever comes first.
static bool bg_cache_saved;
static void background_cache_save(void) {
    if (bg_cache_saved || !g_vk.dev || !g_vk.pcache) return;
    pipeline_cache_save();
    bg_cache_saved = true;
    LOG("[vk] in the background: pipeline cache saved\n");
}
void vk_background_save(void) {
    vk_lock();
    background_cache_save();
    vk_unlock();
}
#endif

static bool frame_begin(void) {
    cur_frame.ok = false;
#ifdef __ANDROID__
    if (__atomic_load_n(&g_android_window_state, __ATOMIC_ACQUIRE) == 1) {
        background_cache_save();
        return false;
    }
    bg_cache_saved = false;
    android_window_check();
    if (__atomic_load_n(&g_android_window_state, __ATOMIC_ACQUIRE)) return false;
#endif
    int w, h;
    SDL_GetWindowSizeInPixels(ui_window(), &w, &h);
    if (!w || !h) return false;  // minimised
    // All GPU mutations happen here under vk_lock(), after resolving the old
    // scene. Existing attachment caches defer old image destruction safely.
    int msaa = config_live_msaa();
    if (configured_msaa < 0) configured_msaa = g_config.msaa;
    if (msaa != configured_msaa) { vk_set_msaa((uint32_t)msaa); configured_msaa = msaa; }
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height ||
        config_live_vsync() != swapchain_vsync) recreate_swapchain();
#ifdef __ANDROID__
    else if (paced_wanted() != swapchain_paced) recreate_swapchain();  // (the FPS limit or the display's rate changed)
    else if (__atomic_load_n(&g_prerotate_test, __ATOMIC_RELAXED) >= 0) {  // (a test's switch: made again when it changes)
        static int was = -1;
        int t = __atomic_load_n(&g_prerotate_test, __ATOMIC_RELAXED);
        if (t != was) was = t, recreate_swapchain();
    }
#endif
    if (!g_vk.swapchain) return false;
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    vk_wait_site(5);  // (exec.c W_PRESENT)
    vk_wait_seq(slot_seq[slot]);
    uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    if (HITCH_ON) hitch_add(HC_ACQUIRE, plat_now_ns() - t0, 0);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("[vk] acquire failed: %d\n", r); return false; }
    cur_frame.acquired = acquire_sem[slot];
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // (the game's image is complete before it is sampled)
    VkClearValue black = {.color.float32 = {0, 0, 0, 1}};
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = sc_pass,
                                .framebuffer = sc_fb[cur_frame.image], .renderArea = {{0, 0}, sc_phys},
                                .clearValueCount = 1, .pClearValues = &black};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = {0, 0, (float)sc_phys.width, (float)sc_phys.height, 0, 1};
    VkRect2D sc = {{0, 0}, sc_phys};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    cur_frame.ok = true;
    return true;
}

// A screenshot being written: the window image's pixels (B8G8R8A8 or R8G8B8A8), made RGBA and saved as PNG.
typedef struct { char path[300]; uint8_t *px; uint32_t w, h; bool bgr; int turn; } ShotJob;
static void *shot_thread(void *arg) {
    ShotJob *j = arg;
    if (j->px && j->turn) {  // (drawn turned for the panel: the window's w x h from the image's h x w, MHGU_PERF_PREROTATE)
        uint32_t *src = (uint32_t *)j->px, *dst = malloc((size_t)j->w * j->h * 4), W = j->w, H = j->h;
        for (uint32_t y = 0; dst && y < H; y++)
            for (uint32_t x = 0; x < W; x++)
                dst[(size_t)y * W + x] = j->turn == 1 ? src[(size_t)x * H + (H - 1 - y)] : src[(size_t)(W - 1 - x) * H + y];
        free(j->px);
        j->px = (uint8_t *)dst;
    }
    if (j->px) {
        uint64_t n = (uint64_t)j->w * j->h;
        for (uint64_t i = 0; j->bgr && i < n; i++) { uint8_t t = j->px[i * 4]; j->px[i * 4] = j->px[i * 4 + 2]; j->px[i * 4 + 2] = t; }
        for (uint64_t i = 0; i < n; i++) j->px[i * 4 + 3] = 255;
        vk_save_png(j->path, j->px, j->w, j->h);
    } else {
        LOG("[vk] saving %s failed: out of memory\n", j->path);
    }
    free(j->px);
    free(j);
    return NULL;
}

// End the window image's render pass and present it. shot_path: also save the window image as PNG. new_frame: a game
// frame (false: the last picture again, the settings menu holding the game).
static void frame_end(const char *shot_path, bool new_frame) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    vkCmdEndRenderPass(cb);
    { void vk_gpuprof_mark(const char *label); vk_gpuprof_mark("window image: the game's picture, the overlay"); }
    VkSpan shot = {0};
    uint32_t img = cur_frame.image;
    if (shot_path) {  // PRESENT_SRC -> TRANSFER_SRC, copy, back
        uint32_t w = sc_phys.width, h = sc_phys.height;
        shot = vk_readback((uint64_t)w * h * 4);
        VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                  .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                  .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_vk.sc_images[img],
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy rg = {.bufferOffset = shot.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyImageToBuffer(cb, g_vk.sc_images[img], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot.buf, 1, &rg);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    }
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(cur_frame.acquired, done_sem[img]);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done_sem[img],
                           .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
#ifdef __ANDROID__
    if (swapchain_paced) vk_pacing_present(&pi, new_frame);  // (its time on the display's vsync grid: present_pacing.c)
#else
    (void)new_frame;
#endif
    uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
    VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
    if (HITCH_ON) hitch_add(HC_PRESENT, plat_now_ns() - t0, 0);
#ifdef __ANDROID__
    // (Android says SUBOPTIMAL every frame while the picture is not pre-rotated for the panel (identity transform on a
    // portrait panel held sideways): the compositor turns it, which is fine; remaking the swapchain for it drained the
    // GPU every frame - half the frame time on a Galaxy S25. Only a swapchain out of date is made again.)
    if (r == VK_ERROR_OUT_OF_DATE_KHR) recreate_swapchain();
    else if (r == VK_SUBOPTIMAL_KHR && sc_wants_turn) {  // (MHGU_PERF_PREROTATE: the phone turned round since? 4 a second at most)
        static uint64_t asked;
        uint64_t now = plat_now_ns();
        VkSurfaceCapabilitiesKHR caps;
        if (now - asked >= 250000000ull && (asked = now) &&
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps) == VK_SUCCESS && turn_wanted(&caps) != sc_turn) {
            LOG("[vk] the panel's transform is now 0x%x: the swapchain made again\n", caps.currentTransform);
            recreate_swapchain();
        }
    }
#else
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreate_swapchain();
#endif
    cur_frame.ok = false;
    if (shot_path) {  // (the copy is taken here; the PNG is made on a thread of its own: encoding the window's size
                      // stalled the game ~0.8 s on a Retina Mac)
        vk_wait_seq(seq);
        ShotJob *j = malloc(sizeof *j);
        if (!j) { LOG("[vk] saving %s failed: out of memory\n", shot_path); return; }
        j->w = g_vk.sc_extent.width, j->h = g_vk.sc_extent.height, j->turn = sc_turn;
        j->bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        j->px = malloc((size_t)j->w * j->h * 4);
        snprintf(j->path, sizeof j->path, "%s", shot_path);
        pthread_t th;
        if (j->px) memcpy(j->px, shot.ptr, (size_t)j->w * j->h * 4);
        if (!j->px || pthread_create(&th, NULL, shot_thread, j) != 0) shot_thread(j);
        else pthread_detach(th);
    }
}

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
#ifdef __ANDROID__
    char full[1200];  // (SDL opens a relative path as an APK asset there: the home folder's, as everywhere else)
    if (path[0] != '/' && getenv("MHGU_HOME")) snprintf(full, sizeof full, "%s/%s", getenv("MHGU_HOME"), path), path = full;
#endif
    if (!s || !SDL_SavePNG(s, path)) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    SDL_DestroySurface(s);
}

// ================================================================== presentation (nvnQueuePresentTexture)
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh);  // (exec.c)

static uint64_t g_frames;
uint64_t vk_frame_counter(void) { return g_frames; }
// The picture presented last (after FSR's), for vk_present_again, and when (plat_now_ns): the update thread's menu pause
static NvnTexture *g_last_picture;
static volatile uint64_t g_last_present_ns;
uint64_t vk_last_present_ns(void) { return __atomic_load_n(&g_last_present_ns, __ATOMIC_ACQUIRE); }

// The MHGU_SHOT_AT list: is this frame one of them?
static bool shot_frame(uint64_t f) {
    static const char *list = (const char *)1;
    if (list == (const char *)1) list = getenv("MHGU_SHOT_AT");
    for (const char *p = list; p && *p;) {
        if ((uint64_t)strtoull(p, (char **)&p, 10) == f) return true;
        if (*p == ',') p++; else break;
    }
    return false;
}

void backend_present(NvnTexture *t) {
    extern void vi_frame_presented(void);
    vi_frame_presented();  // (the frame rate target back in place; MHGU_STEPLOG's count: runtime/hle/vi.c)
    vk_lock();
    extern void vk_color_fill_flush(TexBackend *b);
    vk_color_fill_flush(NULL);  // (MHGU_PERF_HUDSTART_FUSE, MHGU_PERF_FXAA_COPYDRAW: a picture still to be drawn, now)
    if (g_fsr_on) t = vk_fsr_present(t);  // FSR: its output-size picture with the HUD (fsr.c)
    g_last_picture = t;
    TexBackend *b = vk_tex(t, true);
    b->presented = true;
    vk_end_pass();
    vk_ms_resolve(b);
    if (frame_begin()) {
        // the game's image, as large as fits in the picture's shape, centred: g_render_aspect, the image's own shape
        // unless the window has changed shape since the start (a desktop: config.c config_window_shape), when the image
        // made at the start is stretched to it, its 3D drawn for that shape
        double ww = g_vk.sc_extent.width, wh = g_vk.sc_extent.height;
#ifdef __ANDROID__
        double a = (double)t->width / t->height;
#else
        double a = g_render_aspect > 0 ? g_render_aspect : (double)t->width / t->height;
#endif
        double vw = ww, vh = ww / a;
        if (vh > wh) vh = wh, vw = wh * a;
        vk_present_draw(vk_cmd(), b, (float)((ww - vw) / 2), (float)((wh - vh) / 2), (float)vw, (float)vh);
        OverlayTarget ot = {g_vk.inst, g_vk.phys, g_vk.dev, g_vk.qfam, g_vk.queue, g_vk.pcache, sc_pass,
                            g_vk.sc_extent.width, g_vk.sc_extent.height, sc_turn};
        overlay_draw(vk_cmd(), &ot);  // (Forge plugins' ImGui overlay, over the game: runtime/overlay.cpp)
        char path[300];
        extern char g_shot_request[256];  // (runtime/host_input.c: MHGU_CONTROL "shot <name>")
        extern char g_burst_name[200];
        extern int g_burst_left, g_burst_index;
        if (!g_shot_request[0] && g_burst_left > 0) {
            g_burst_left--;
            snprintf(g_shot_request, sizeof g_shot_request, "shots/%s_%d.png", g_burst_name, g_burst_index++);
        }
        bool shot = shot_frame(g_frames + 1) || g_shot_request[0];
        if (shot) {
            mkdir("shots", 0755);
            if (g_shot_request[0]) { snprintf(path, sizeof path, "%s", g_shot_request); g_shot_request[0] = 0; }
            else snprintf(path, sizeof path, "shots/frame_%05llu.png", (unsigned long long)(g_frames + 1));
        }
        frame_end(shot ? path : NULL, true);
    } else {
        vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    g_frames++;
    g_present_count = g_frames;
#ifdef __ANDROID__
    if (g_frames == 120) gpu_driver_good();  // (a driver of the player's own has drawn: it stays)
#endif
    touch_hud_frame();  // (the touch controls: where this frame's HUD parts were, runtime/touch.cpp)
    ui_loop_wake();  // (a frame: the window loop runs a pass now if it waits between events, MHGU_PERF_INPUT_WAKE, ui.c)
#ifndef __ANDROID__
    mousemenu_frame();  // (and the mouse in the game's menus: where this frame's menu pieces were, runtime/hle/mousemenu.c)
#endif
    {   // (debug) MHGU_CONTROL "dump": the next whole frame's passes (exec.c dump_pass)
        extern volatile int g_dump_active, g_dump_request;
        extern int g_dump_no, g_dump_frame;
        static int left;
        if (g_dump_active) {
            LOG("[dump] frame %d done\n", g_dump_frame);
            if (--left > 0) { g_dump_frame++; g_dump_no = 0; }
            else g_dump_active = 0;
        } else if (g_dump_request) { left = g_dump_request; g_dump_request = 0; g_dump_no = 0; g_dump_frame = 0; g_dump_active = 1; }
    }
    {   // (debug) MHGU_CONTROL "hudtrace": the next whole frame's HUD quads (exec.c cbuf_info)
        extern volatile int g_hudtrace_active, g_hudtrace_request;
        if (g_hudtrace_active) { LOG("[hud] frame done\n"); g_hudtrace_active--; }  // (2: the frame recorded next may run late)
        else if (g_hudtrace_request) { g_hudtrace_request = 0; g_hudtrace_active = 2; }
    }
    { void vk_gpuprof_frame(void); vk_gpuprof_frame(); }  // (debug) MHGU_CONTROL "gpuprof"
    vk_unlock();
#if defined(__ANDROID__) || defined(_WIN32)
    perf_hint_frame();  // (this frame's work to the performance hint session, outside the lock: runtime/platform/android_perf.c;
                        // Windows: the readout's render thread, platform/win_stats.c)
#endif
    static int fps = -1;
    if (fps < 0) fps = getenv("MHGU_FPS") != NULL;
    static uint64_t t0, n0;
    uint64_t now = plat_now_ns();
    if (!t0) t0 = now;
    if (now - t0 >= 2000000000ull) {
        if (fps) LOG("[vk] %.1f fps (frame %llu)\n", (g_frames - n0) * 1e9 / (double)(now - t0), (unsigned long long)g_frames);
        t0 = now, n0 = g_frames;
    }
    if (HITCH_ON) hitch_frame(HT_RENDER, 0);  // ((debug) MHGU_HITCHLOG: present to present, runtime/hitchlog.c)
    __atomic_store_n(&g_last_present_ns, plat_now_ns(), __ATOMIC_RELEASE);
    // (the app in the background, [Application] PauseInBackground: the presenting thread stops here, outside the
    // Vulkan lock, until it is back; the next frame then makes the window's surface again: frame_begin)
    extern void vi_pause_wait(void);
    vi_pause_wait();
}

// The settings menu holds the game (runtime/overlay.cpp, [Application] PauseInMenu): the last picture again, with the
// overlay's newest frame over it, while the game's presenting thread waits (runtime/hle/vi.c vi_menu_hold). false: there
// is no picture yet.
bool vk_present_again(void) {
    vk_lock();
    NvnTexture *t = g_last_picture;
    if (!t) { vk_unlock(); return false; }
    TexBackend *b = vk_tex(t, true);
    vk_end_pass();
    if (frame_begin()) {
        double ww = g_vk.sc_extent.width, wh = g_vk.sc_extent.height;
#ifdef __ANDROID__
        double a = (double)t->width / t->height;
#else
        double a = g_render_aspect > 0 ? g_render_aspect : (double)t->width / t->height;
#endif
        double vw = ww, vh = ww / a;
        if (vh > wh) vh = wh, vw = wh * a;
        void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh);  // (exec.c)
        vk_present_draw(vk_cmd(), b, (float)((ww - vw) / 2), (float)((wh - vh) / 2), (float)vw, (float)vh);
        OverlayTarget ot = {g_vk.inst, g_vk.phys, g_vk.dev, g_vk.qfam, g_vk.queue, g_vk.pcache, sc_pass,
                            g_vk.sc_extent.width, g_vk.sc_extent.height, sc_turn};
        overlay_draw(vk_cmd(), &ot);
        frame_end(NULL, false);
    } else {
        vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    vk_unlock();
    return true;
}

// (runtime/overlay.cpp, the settings menu holding the game with MHGU_PERF_MENU_PRESENT=1) the window needs a present
// whatever the menu shows: back from the background (a new surface to make), no swapchain, the window's size or the
// VSync setting changed (frame_begin makes it again)
bool vk_present_needed(void) {
#ifdef __ANDROID__
    if (__atomic_load_n(&g_android_window_state, __ATOMIC_ACQUIRE)) return true;
#endif
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(ui_window(), &w, &h);
    vk_lock();
    bool need = !g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height ||
                config_live_vsync() != swapchain_vsync;
    vk_unlock();
    return need;
}

// ================================================================== init
void vk_exec_init(void);
void vk_shaders_init(void);

// exit() from any thread. The atexit chain is the danger: SDL_Quit and libnvidia's own destructors tear the
// driver down while other threads are still inside nvnQueueSubmitCommands / vkWaitSemaphores (the exit-time
// SIGSEGV in libnvidia-glcore). So: park the guest threads (vk_lock), drain what is in flight, let the GPU
// finish, keep the pipeline cache - and leave with _exit, running no further teardown.
static void exit_handler(void) {
    exiting = true;
    vk_exit_thread = pthread_self();
    if (g_vk.dev && g_vk_owner != pthread_self()) {  // (inside a submission already: the holder is us, don't self-lock)
        pthread_mutex_lock(&g_vk_mutex);
        pthread_mutex_unlock(&g_vk_mutex);
    }
    if (g_vk.dev) {
        vkDeviceWaitIdle(g_vk.dev);
        pipeline_cache_save();
    }
#ifndef __ANDROID__
    for (uint32_t i = 0; i < res_n; i++)
        LOG("[vk] device memory set aside (type %u): at most %llu of its %llu MB held\n", res_type[i],
            (unsigned long long)(res_peak[i] >> 20), (unsigned long long)(res_blocks[i] * res_block >> 20));
#endif
    fflush(stdout);
    rt_log_flush();  // (Windows: the log's queued lines, runtime/logwriter.c; elsewhere fflush(stderr))
    _exit(0);
}

#ifdef __ANDROID__
// The first surface, while the game starts. Sent to the background before it (Home in the first seconds of loading),
// the activity has no window ("Android native window is not available") and the game stopped there with an error.
// It waits for the window on its own thread instead (SDL's main thread keeps the lifecycle: runtime/ui.c), and goes on
// once the app is back; the first frame then makes the surface and swapchain again, as after any return. Only a window
// missing for 5 s with the app in the foreground is an error.
static bool start_surface(void) {
    uint64_t fg = plat_now_ns();  // (since when the app is in the foreground)
    bool waited = false;
    while (!make_surface()) {
        if (__atomic_load_n(&g_android_window_state, __ATOMIC_ACQUIRE) == 1) fg = plat_now_ns();
        else if (plat_now_ns() - fg > 5000000000ull) return false;
        if (!waited) LOG("[vk] no window for the surface (%s): the app is in the background, waiting for it\n", SDL_GetError());
        waited = true;
        SDL_Delay(50);
    }
    if (waited) LOG("[vk] the window is back: the start goes on\n");
    return true;
}
#else
#define start_surface() make_surface()
#endif

void backend_init(void) {
    static bool done;
    if (done) return;
    done = true;
    if (!ui_window()) FATAL("no window\n");
    create_instance();
    if (!start_surface()) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
    vk_exec_init();
    vk_shaders_init();
    vk_fsr_init();  // ([Upscaler] FSR: fsr.c)
    create_swapchain();
    atexit(exit_handler);
#ifndef __ANDROID__
    __atomic_store_n(&dm_started, true, __ATOMIC_RELAXED);  // (device memory taken from here on is logged)
#endif
}
