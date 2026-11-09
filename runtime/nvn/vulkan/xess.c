// XeSS-SR through the XeSS SDK's runtime, libxess.dll (see xess.h and third_party/xess/README.md).
//
// The DLL is the SDK 3.0.2's, shipped beside MHGU.exe byte for byte as released: its license forbids changing it or its
// operation, so it is only loaded (LoadLibraryExW) and called through its public C API (third_party/xess/inc, MIT).
// It needs the Microsoft Visual C++ Redistributable (x64) 14.40 or later; without it, without the DLL, or on a GPU or
// driver XeSS's own queries refuse, FSR 3.1 runs instead.
//
// The order is XeSS's: its instance extensions and Vulkan version before vkCreateInstance (device.c), its device
// extensions, features and queues before vkCreateDevice; then, on the thread that draws (fsr.c hud_start, its first
// frame), the context and the pipeline build, started without waiting; xessVKInit at the first dispatch and
// xessVKExecute recording into fsr.c's command buffer, every frame with FSR. XeSS is not thread safe and its guide
// wants every call from the thread it was initialized on: from the context on, all of them are made there (fsr.c's
// recording thread), with vk_lock held as fsr.c's own work. A context XeSS stopped with is destroyed there too: at once
// when it has recorded nothing, else once the GPU is past the submission it recorded into.
// Its queue (seen with a logging layer on an RX 6800 XT): xessVKCreateContext takes the first queue of family 0 and
// makes a command pool there; xessVKInit submits once to that queue, with a fence of its own, and xessVKExecute never
// does. Our device's queue is that one (draw_queue: family 0 on every desktop GPU seen; another family: no XeSS-SR),
// and vk_lock keeps the game's submits away meanwhile. As in the XeSS SDK's Vulkan sample, the device also gets a queue
// in the dedicated compute family when XeSS-SR is the upscaler, in case its implementation on some GPU takes that one.
// It gets fsr.c's inputs for FSR 3.1: the post-processed (LDR, gamma-encoded) picture, the R32F depth copy (0 near ..
// 1 far), the render-size motion vectors (previous minus current position in render pixels, no jitter: XeSS's own
// convention), the reactive mask as its responsive pixel mask, and the jitter, the picture shift in render pixels (x
// right, y down: as its guide's projection formula has it). They live in GENERAL; XeSS wants its inputs in
// SHADER_READ_ONLY_OPTIMAL and does no synchronization itself, so each dispatch moves them there and back.
//
//   MHGU_XESS_DLL=<path>          another libxess.dll (a missing one shows the fallback)
//   MHGU_XESS_PRESET=<name>       aa, ultraqualityplus, ultraquality, quality, balanced, performance, ultraperformance
//   MHGU_XESS_JITTER_SIGN=sx,sy   the jitter given times these (default 1,1)
//   MHGU_XESS_MV_SCALE=sx,sy      xessSetVelocityScale (default 1,1; 0,0: no motion vectors, for a still scene)
//   MHGU_XESS_NOMASK=1            no responsive pixel mask
//   MHGU_XESS_LOG=1               XeSS's own messages from DEBUG on (default: warnings and errors)
//   MHGU_XESS_DUMP=<folder>       xessStartDump: 8 frames (inputs, output) from the 600th dispatch on; the folder must exist
//   MHGU_XESS_FAILTEST=<n>|init|build|runtime|unsupported|instance|device   (test) the n-th dispatch, xessVKInit or
//                                 xessVKBuildPipelines fails, the Visual C++ runtime is missing, the GPU unsupported,
//                                 vkCreateInstance / vkCreateDevice refuse (a made-up extension name added with XeSS's)
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../../rt.h"
#include "xess.h"

// g_xess_fallback (the settings panel shows it on the Upscaler row; settings_locale.h translates it)
#define NO_XESS_DLL "libxess.dll is missing beside MHGU.exe, so FSR 3.1 is used."
#define NO_XESS_RUNTIME "XeSS-SR needs the Microsoft Visual C++ Redistributable (x64): install it and restart the game. Until then FSR 3.1 is used."
#define NO_XESS_GPU "This GPU or its driver can't run XeSS-SR, so FSR 3.1 is used."
#define NO_XESS_START "XeSS-SR could not start, so FSR 3.1 is used. The [xess] lines in log.txt say why."

#ifndef _WIN32

bool vk_xess_instance_exts(const char **ext, uint32_t *n_ext, uint32_t max, uint32_t *api) {
    (void)ext, (void)n_ext, (void)max, (void)api;
    LOG("[xess] XeSS-SR is only in the Windows version: FSR 3.1 is used\n");
    g_xess_fallback = NO_XESS_START;
    return false;
}
bool vk_xess_device_exts(const VkExtensionProperties *ep, uint32_t n, const char **ext, uint32_t *n_ext, uint32_t max) { (void)ep, (void)n, (void)ext, (void)n_ext, (void)max; return false; }
bool vk_xess_device_features(VkPhysicalDeviceFeatures2 *f, VkPhysicalDeviceVulkan11Features *f11, VkPhysicalDeviceVulkan12Features *f12, VkPhysicalDeviceVulkan13Features *f13, bool api13) { (void)f, (void)f11, (void)f12, (void)f13, (void)api13; return false; }
uint32_t vk_xess_device_queues(VkDeviceQueueCreateInfo *qi, uint32_t n, uint32_t max) { (void)qi, (void)max; return n; }
void vk_xess_device_done(void) {}
void vk_xess_creation_failed(const char *what, VkResult r) { (void)what, (void)r; }
bool vk_xess_init(int quality) { (void)quality; if (!g_xess_fallback) g_xess_fallback = NO_XESS_START; return false; }
void vk_xess_frame(void) {}
bool vk_xess_ready(void) { return false; }
const char *vk_xess_name(void) { return "XeSS-SR"; }
bool vk_xess_dispatch(const XessDispatch *d) { (void)d; return false; }

#else

#include <windows.h>

#include <xess/xess_debug.h>
#include <xess/xess_vk.h>

// the layouts libxess.dll was built with (MSVC, 8-byte packing, 4-byte enums)
_Static_assert(sizeof(xess_vk_image_view_info) == 48 && offsetof(xess_vk_image_view_info, format) == 36, "image view info");
_Static_assert(sizeof(xess_vk_execute_params_t) == 360 && offsetof(xess_vk_execute_params_t, jitterOffsetX) == 288, "execute");
_Static_assert(sizeof(xess_vk_init_params_t) == 64 && offsetof(xess_vk_init_params_t, tempBufferHeap) == 24, "init params");
_Static_assert(sizeof(xess_version_t) == 8 && sizeof(xess_properties_t) == 24, "small structures");

// libxess.dll's functions this file calls, as pointers of their own types (from GetProcAddress)
#define XESS_FNS(F)                                                                                                    \
    F(xessGetVersion) F(xessVKGetRequiredInstanceExtensions) F(xessVKGetRequiredDeviceExtensions)                     \
    F(xessVKGetRequiredDeviceFeatures) F(xessVKCreateContext) F(xessVKBuildPipelines) F(xessVKInit) F(xessVKExecute) \
    F(xessDestroyContext) F(xessGetOptimalInputResolution) F(xessSetLoggingCallback) F(xessIsOptimalDriver)          \
    F(xessGetIntelXeFXVersion) F(xessGetPipelineBuildStatus) F(xessGetProperties) F(xessSetVelocityScale)
#define DECLARE(fn) static __typeof__(fn) *p_##fn;
XESS_FNS(DECLARE)
static __typeof__(xessStartDump) *p_xessStartDump;  // (MHGU_XESS_DUMP; not needed otherwise)

static HMODULE lib;
static uint32_t min_api;                    // the Vulkan version XeSS asked of the instance
static bool inst_ok, dev_exts_ok, dev_ok;   // its instance / device extensions / features were taken
static int quality_asked = -1;              // FSRQuality (vk_xess_init); -1: XeSS-SR is not to start
static xess_context_handle_t ctx;
static xess_quality_settings_t preset;
static uint32_t flags;
static bool started, ready, inited, failed;  // (started: the context was tried, on the drawing thread)
static uint64_t frames;
static uint64_t destroy_at;  // a context XeSS stopped with, after recording: destroyed once the GPU is past this submission
static char name[48] = "XeSS-SR";
static DWORD ctx_thread, init_thread;  // (the thread that made the context; the one xessVKInit ran on)
static const char *failtest;  // MHGU_XESS_FAILTEST

static const char *result_name(xess_result_t r) {
    switch (r) {
    case XESS_RESULT_WARNING_NONEXISTING_FOLDER: return "warning: the folder does not exist";
    case XESS_RESULT_WARNING_OLD_DRIVER: return "warning: an old driver";
    case XESS_RESULT_SUCCESS: return "success";
    case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE: return "unsupported device";
    case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER: return "unsupported driver";
    case XESS_RESULT_ERROR_UNINITIALIZED: return "not initialized";
    case XESS_RESULT_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "out of GPU memory";
    case XESS_RESULT_ERROR_DEVICE: return "device error";
    case XESS_RESULT_ERROR_NOT_IMPLEMENTED: return "not implemented";
    case XESS_RESULT_ERROR_INVALID_CONTEXT: return "invalid context";
    case XESS_RESULT_ERROR_OPERATION_IN_PROGRESS: return "operation in progress";
    case XESS_RESULT_ERROR_UNSUPPORTED: return "unsupported";
    case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY: return "can't load a library";
    case XESS_RESULT_ERROR_WRONG_CALL_ORDER: return "wrong call order";
    default: return "unknown error";
    }
}

// the menu's note after a failed call: the GPU / driver, or anything else
static void refused(xess_result_t r) {
    g_xess_fallback = r == XESS_RESULT_ERROR_UNSUPPORTED_DEVICE || r == XESS_RESULT_ERROR_UNSUPPORTED_DRIVER ||
                              r == XESS_RESULT_ERROR_UNSUPPORTED
                          ? NO_XESS_GPU
                          : NO_XESS_START;
}

static bool test(const char *what) { return failtest && !strcmp(failtest, what); }

static void file_version(const wchar_t *path, unsigned v[4]) {
    v[0] = v[1] = v[2] = v[3] = 0;
    DWORD z, sz = GetFileVersionInfoSizeW(path, &z);
    void *buf = sz ? malloc(sz) : NULL;
    VS_FIXEDFILEINFO *fi;
    UINT l;
    if (buf && GetFileVersionInfoW(path, 0, sz, buf) && VerQueryValueW(buf, L"\\", (void **)&fi, &l) && fi)
        v[0] = HIWORD(fi->dwFileVersionMS), v[1] = LOWORD(fi->dwFileVersionMS), v[2] = HIWORD(fi->dwFileVersionLS),
        v[3] = LOWORD(fi->dwFileVersionLS);
    free(buf);
}

static bool exe_dir(wchar_t *dir) {  // (with its last backslash)
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t *slash = n && n < MAX_PATH ? wcsrchr(dir, L'\\') : NULL;
    if (!slash) return false;
    slash[1] = 0;
    return true;
}

// The Visual C++ runtime libxess.dll imports, where Windows would take it from: already in the process, beside
// MHGU.exe, or System32. false: a library is missing, or msvcp140.dll is older than 14.40.33810 (the SDK's minimum)
static bool vc_runtime(void) {
    static const wchar_t *const libs[] = {L"msvcp140.dll", L"vcruntime140.dll", L"vcruntime140_1.dll"};
    wchar_t dir[MAX_PATH] = L"", sys[MAX_PATH] = L"", path[MAX_PATH];
    exe_dir(dir);
    UINT ns = GetSystemDirectoryW(sys, MAX_PATH);
    if (ns && ns < MAX_PATH - 1) wcscat(sys, L"\\");
    for (int i = 0; i < 3; i++) {
        HMODULE m = GetModuleHandleW(libs[i]);
        const char *where = "already loaded";
        if (!m || !GetModuleFileNameW(m, path, MAX_PATH)) {
            where = "beside MHGU.exe";
            swprintf(path, MAX_PATH, L"%ls%ls", dir, libs[i]);
            if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
                where = "System32";
                swprintf(path, MAX_PATH, L"%ls%ls", sys, libs[i]);
            }
        }
        if (test("runtime") || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
            LOG("[xess] %ls is missing: libxess.dll needs the Microsoft Visual C++ Redistributable (x64), 14.40 or later. "
                "FSR 3.1 is used\n", libs[i]);
            return false;
        }
        if (i) continue;
        unsigned v[4];
        file_version(path, v);
        uint64_t have = (uint64_t)v[0] << 32 | (uint64_t)v[1] << 16 | v[2], need = 14ull << 32 | 40ull << 16 | 33810;
        LOG("[xess] Visual C++ runtime: %ls %u.%u.%u.%u (%s)\n", path, v[0], v[1], v[2], v[3], where);
        if (have < need) {
            LOG("[xess] libxess.dll needs the Visual C++ runtime 14.40.33810 or later: FSR 3.1 is used\n");
            return false;
        }
    }
    return true;
}

// libxess.dll and its functions; false: FSR 3.1 (logged, the menu's note set)
static bool load(void) {
    failtest = getenv("MHGU_XESS_FAILTEST");
    wchar_t path[MAX_PATH] = L"";
    const char *env = getenv("MHGU_XESS_DLL");  // (test: another copy, or a missing one to see the fallback)
    if (env) {
        wchar_t rel[MAX_PATH];
        DWORD n = MultiByteToWideChar(CP_UTF8, 0, env, -1, rel, MAX_PATH) ? GetFullPathNameW(rel, MAX_PATH, path, NULL) : 0;
        if (!n || n >= MAX_PATH) path[0] = 0;  // (a path that does not fit: as missing)
    } else if (exe_dir(path)) {
        size_t n = wcslen(path);
        if (n + wcslen(L"libxess.dll") < MAX_PATH) wcscpy(path + n, L"libxess.dll");
        else path[0] = 0;  // (MHGU.exe's folder too long for the name after it: as missing)
    }
    if (!path[0] || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        LOG("[xess] %ls is missing (it comes beside MHGU.exe in the PC version's zip): FSR 3.1 is used\n",
            path[0] ? path : L"libxess.dll (its path does not fit)");
        g_xess_fallback = NO_XESS_DLL;
        return false;
    }
    if (!vc_runtime()) {
        g_xess_fallback = NO_XESS_RUNTIME;
        return false;
    }
    lib = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!lib) {
        DWORD e = GetLastError();
        bool runtime = e == ERROR_MOD_NOT_FOUND || e == ERROR_PROC_NOT_FOUND;  // (126 / 127: a library it imports, or a function in one)
        LOG("[xess] %ls could not be loaded (error %lu%s): FSR 3.1 is used\n", path, e,
            runtime ? ": a library it needs is missing or too old, most likely the Microsoft Visual C++ Redistributable "
                      "(x64) 14.40 or later" : "");
        g_xess_fallback = runtime ? NO_XESS_RUNTIME : NO_XESS_START;
        return false;
    }
    unsigned fv[4];
    file_version(path, fv);
#define GET(fn)                                                                    \
    if (!(p_##fn = (__typeof__(fn) *)(void *)GetProcAddress(lib, #fn))) {          \
        LOG("[xess] %ls has no %s: FSR 3.1 is used\n", path, #fn);                 \
        g_xess_fallback = NO_XESS_START;                                           \
        FreeLibrary(lib); /* (nothing of it was called) */                         \
        lib = NULL;                                                                \
        return false;                                                              \
    }
    XESS_FNS(GET)
#undef GET
    p_xessStartDump = (__typeof__(xessStartDump) *)(void *)GetProcAddress(lib, "xessStartDump");
    xess_version_t v = {0};
    p_xessGetVersion(&v);
    snprintf(name, sizeof name, "XeSS-SR %u.%u.%u", v.major, v.minor, v.patch);
    LOG("[xess] loaded %ls, file version %u.%u.%u.%u, %s\n", path, fv[0], fv[1], fv[2], fv[3], name);
    return true;
}

static bool listed(const char *const *list, uint32_t n, const char *s) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(list[i], s)) return true;
    return false;
}

// ================================================================== instance / device (device.c)
bool vk_xess_instance_exts(const char **ext, uint32_t *n_ext, uint32_t max, uint32_t *api) {
    if (!load()) return false;
    uint32_t n = 0, need = 0;
    const char *const *names = NULL;
    xess_result_t r = p_xessVKGetRequiredInstanceExtensions(&n, &names, &need);
    if (r != XESS_RESULT_SUCCESS) {
        LOG("[xess] xessVKGetRequiredInstanceExtensions: %s (%d): FSR 3.1 is used\n", result_name(r), (int)r);
        refused(r);
        return false;
    }
    uint32_t keep = *n_ext;
    char list[512] = "";
    for (uint32_t i = 0; i < n; i++) {
        snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? ", " : "", names[i]);
        if (listed(ext, *n_ext, names[i])) continue;
        if (*n_ext >= max) {
            LOG("[xess] no room for its instance extension %s: FSR 3.1 is used\n", names[i]);
            *n_ext = keep;
            g_xess_fallback = NO_XESS_START;
            return false;
        }
        ext[(*n_ext)++] = names[i];
    }
    if (test("instance") && *n_ext < max) ext[(*n_ext)++] = "VK_MHGU_xess_failtest";
    min_api = need;
    LOG("[xess] instance: Vulkan %u.%u.%u or later, extensions: %s\n", VK_API_VERSION_MAJOR(need), VK_API_VERSION_MINOR(need),
        VK_API_VERSION_PATCH(need), n ? list : "none");
    if (need > *api) *api = need;
    inst_ok = true;
    return true;
}

void vk_xess_creation_failed(const char *what, VkResult r) {
    LOG("[xess] %s refused XeSS-SR's extensions or features (%d): made again without them, FSR 3.1 is used\n", what, r);
    inst_ok = dev_exts_ok = dev_ok = false;
    g_xess_fallback = NO_XESS_START;
}

bool vk_xess_device_exts(const VkExtensionProperties *ep, uint32_t n, const char **ext, uint32_t *n_ext, uint32_t max) {
    if (!inst_ok) return false;
    if (g_vk.qfam != 0) {  // (see the top: it would take a queue this device doesn't make)
        LOG("[xess] the game draws on queue family %u, and XeSS-SR uploads through family 0's first queue: FSR 3.1 is used\n",
            g_vk.qfam);
        g_xess_fallback = NO_XESS_GPU;
        return false;
    }
    uint32_t have = g_vk.props.apiVersion;
    if (VK_API_VERSION_MAJOR(have) < VK_API_VERSION_MAJOR(min_api) ||
        (VK_API_VERSION_MAJOR(have) == VK_API_VERSION_MAJOR(min_api) && VK_API_VERSION_MINOR(have) < VK_API_VERSION_MINOR(min_api))) {
        LOG("[xess] the GPU's Vulkan %u.%u is older than XeSS-SR's %u.%u: FSR 3.1 is used\n", VK_API_VERSION_MAJOR(have),
            VK_API_VERSION_MINOR(have), VK_API_VERSION_MAJOR(min_api), VK_API_VERSION_MINOR(min_api));
        g_xess_fallback = NO_XESS_GPU;
        return false;
    }
    uint32_t cnt = 0;
    const char *const *names = NULL;
    xess_result_t r = test("unsupported") ? XESS_RESULT_ERROR_UNSUPPORTED_DRIVER
                                          : p_xessVKGetRequiredDeviceExtensions(g_vk.inst, g_vk.phys, &cnt, &names);
    if (r != XESS_RESULT_SUCCESS) {
        LOG("[xess] xessVKGetRequiredDeviceExtensions: %s (%d)%s: FSR 3.1 is used\n", result_name(r), (int)r,
            test("unsupported") ? " (MHGU_XESS_FAILTEST)" : "");
        refused(r);
        return false;
    }
    uint32_t keep = *n_ext;
    char list[1024] = "";
    for (uint32_t i = 0; i < cnt; i++) {
        snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? ", " : "", names[i]);
        bool gpu_has = false;
        for (uint32_t j = 0; j < n && !gpu_has; j++) gpu_has = !strcmp(ep[j].extensionName, names[i]);
        if (!gpu_has || *n_ext >= max) {
            LOG("[xess] %s its device extension %s: FSR 3.1 is used\n", gpu_has ? "no room for" : "the GPU lacks", names[i]);
            *n_ext = keep;
            g_xess_fallback = gpu_has ? NO_XESS_START : NO_XESS_GPU;
            return false;
        }
        if (!listed(ext, *n_ext, names[i])) ext[(*n_ext)++] = names[i];
    }
    if (test("device") && *n_ext < max) ext[(*n_ext)++] = "VK_MHGU_xess_failtest";
    LOG("[xess] device extensions: %s\n", cnt ? list : "none");
    dev_exts_ok = true;
    return true;
}

// The device feature structures XeSS may ask for, and where their fields go in our chain: into 0 the
// VkPhysicalDeviceFeatures2 (f), 1 / 2 / 3 the Vulkan 1.1 / 1.2 / 1.3 structures (what those versions took in: a chain
// with VkPhysicalDeviceVulkan12Features must not hold VkPhysicalDeviceShaderFloat16Int8Features too, and so on), at
// byte `at`, `count` VkBool32 fields in the same order (checked below). Anything else is linked into the chain as it is.
typedef struct {
    VkStructureType type;
    const char *name;
    uint8_t into;
    uint16_t at, count;
} Known;
#define FIELDS(S, a, b) (uint16_t)((offsetof(S, b) - offsetof(S, a)) / sizeof(VkBool32) + 1)
#define V11 VkPhysicalDeviceVulkan11Features
#define V12 VkPhysicalDeviceVulkan12Features
#define V13 VkPhysicalDeviceVulkan13Features
#define TAKEN(T, S, into, V, a, b) {T, #S, into, (uint16_t)offsetof(V, a), FIELDS(S, a, b)}
#define SAME_ORDER(S, V, a, b) _Static_assert(offsetof(V, b) - offsetof(V, a) == offsetof(S, b) - offsetof(S, a) && \
                                              offsetof(S, a) == sizeof(VkBaseOutStructure), #S)
static const Known known[] = {
    {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, "VkPhysicalDeviceFeatures2", 0, (uint16_t)offsetof(VkPhysicalDeviceFeatures2, features),
     FIELDS(VkPhysicalDeviceFeatures, robustBufferAccess, inheritedQueries)},
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, VkPhysicalDeviceVulkan11Features, 1, V11, storageBuffer16BitAccess,
          shaderDrawParameters),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, VkPhysicalDeviceVulkan12Features, 2, V12, samplerMirrorClampToEdge,
          subgroupBroadcastDynamicId),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, VkPhysicalDeviceVulkan13Features, 3, V13, robustImageAccess,
          maintenance4),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, VkPhysicalDevice16BitStorageFeatures, 1, V11,
          storageBuffer16BitAccess, storageInputOutput16),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, VkPhysicalDeviceMultiviewFeatures, 1, V11, multiview,
          multiviewTessellationShader),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, VkPhysicalDeviceVariablePointersFeatures, 1, V11,
          variablePointersStorageBuffer, variablePointers),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES, VkPhysicalDeviceProtectedMemoryFeatures, 1, V11,
          protectedMemory, protectedMemory),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, VkPhysicalDeviceSamplerYcbcrConversionFeatures, 1,
          V11, samplerYcbcrConversion, samplerYcbcrConversion),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, VkPhysicalDeviceShaderDrawParametersFeatures, 1, V11,
          shaderDrawParameters, shaderDrawParameters),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, VkPhysicalDevice8BitStorageFeatures, 2, V12,
          storageBuffer8BitAccess, storagePushConstant8),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES, VkPhysicalDeviceShaderAtomicInt64Features, 2, V12,
          shaderBufferInt64Atomics, shaderSharedInt64Atomics),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, VkPhysicalDeviceShaderFloat16Int8Features, 2, V12,
          shaderFloat16, shaderInt8),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, VkPhysicalDeviceDescriptorIndexingFeatures, 2, V12,
          shaderInputAttachmentArrayDynamicIndexing, runtimeDescriptorArray),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, VkPhysicalDeviceScalarBlockLayoutFeatures, 2, V12,
          scalarBlockLayout, scalarBlockLayout),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES, VkPhysicalDeviceImagelessFramebufferFeatures, 2, V12,
          imagelessFramebuffer, imagelessFramebuffer),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, VkPhysicalDeviceUniformBufferStandardLayoutFeatures,
          2, V12, uniformBufferStandardLayout, uniformBufferStandardLayout),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures,
          2, V12, shaderSubgroupExtendedTypes, shaderSubgroupExtendedTypes),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures,
          2, V12, separateDepthStencilLayouts, separateDepthStencilLayouts),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, VkPhysicalDeviceHostQueryResetFeatures, 2, V12, hostQueryReset,
          hostQueryReset),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, VkPhysicalDeviceTimelineSemaphoreFeatures, 2, V12,
          timelineSemaphore, timelineSemaphore),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, VkPhysicalDeviceBufferDeviceAddressFeatures, 2, V12,
          bufferDeviceAddress, bufferDeviceAddressMultiDevice),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, VkPhysicalDeviceVulkanMemoryModelFeatures, 2, V12,
          vulkanMemoryModel, vulkanMemoryModelAvailabilityVisibilityChains),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES, VkPhysicalDeviceImageRobustnessFeatures, 3, V13,
          robustImageAccess, robustImageAccess),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES, VkPhysicalDeviceInlineUniformBlockFeatures, 3, V13,
          inlineUniformBlock, descriptorBindingInlineUniformBlockUpdateAfterBind),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES, VkPhysicalDevicePipelineCreationCacheControlFeatures,
          3, V13, pipelineCreationCacheControl, pipelineCreationCacheControl),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, VkPhysicalDevicePrivateDataFeatures, 3, V13, privateData, privateData),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES,
          VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures, 3, V13, shaderDemoteToHelperInvocation, shaderDemoteToHelperInvocation),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES, VkPhysicalDeviceShaderTerminateInvocationFeatures, 3,
          V13, shaderTerminateInvocation, shaderTerminateInvocation),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, VkPhysicalDeviceSubgroupSizeControlFeatures, 3, V13,
          subgroupSizeControl, computeFullSubgroups),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, VkPhysicalDeviceSynchronization2Features, 3, V13,
          synchronization2, synchronization2),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES, VkPhysicalDeviceTextureCompressionASTCHDRFeatures, 3,
          V13, textureCompressionASTC_HDR, textureCompressionASTC_HDR),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES,
          VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures, 3, V13, shaderZeroInitializeWorkgroupMemory,
          shaderZeroInitializeWorkgroupMemory),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, VkPhysicalDeviceDynamicRenderingFeatures, 3, V13,
          dynamicRendering, dynamicRendering),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, VkPhysicalDeviceShaderIntegerDotProductFeatures, 3, V13,
          shaderIntegerDotProduct, shaderIntegerDotProduct),
    TAKEN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, VkPhysicalDeviceMaintenance4Features, 3, V13, maintenance4,
          maintenance4),
};
SAME_ORDER(VkPhysicalDevice16BitStorageFeatures, V11, storageBuffer16BitAccess, storageInputOutput16);
SAME_ORDER(VkPhysicalDeviceMultiviewFeatures, V11, multiview, multiviewTessellationShader);
SAME_ORDER(VkPhysicalDeviceVariablePointersFeatures, V11, variablePointersStorageBuffer, variablePointers);
SAME_ORDER(VkPhysicalDevice8BitStorageFeatures, V12, storageBuffer8BitAccess, storagePushConstant8);
SAME_ORDER(VkPhysicalDeviceShaderAtomicInt64Features, V12, shaderBufferInt64Atomics, shaderSharedInt64Atomics);
SAME_ORDER(VkPhysicalDeviceShaderFloat16Int8Features, V12, shaderFloat16, shaderInt8);
SAME_ORDER(VkPhysicalDeviceDescriptorIndexingFeatures, V12, shaderInputAttachmentArrayDynamicIndexing, runtimeDescriptorArray);
SAME_ORDER(VkPhysicalDeviceBufferDeviceAddressFeatures, V12, bufferDeviceAddress, bufferDeviceAddressMultiDevice);
SAME_ORDER(VkPhysicalDeviceVulkanMemoryModelFeatures, V12, vulkanMemoryModel, vulkanMemoryModelAvailabilityVisibilityChains);
SAME_ORDER(VkPhysicalDeviceInlineUniformBlockFeatures, V13, inlineUniformBlock, descriptorBindingInlineUniformBlockUpdateAfterBind);
SAME_ORDER(VkPhysicalDeviceSubgroupSizeControlFeatures, V13, subgroupSizeControl, computeFullSubgroups);
_Static_assert(offsetof(VkPhysicalDeviceFeatures2, features) == sizeof(VkBaseOutStructure), "features");

static const Known *known_type(VkStructureType t) {
    for (size_t i = 0; i < sizeof known / sizeof *known; i++)
        if (known[i].type == t) return &known[i];
    return NULL;
}

// (the log) the fields of the four structures fields are merged into, in order
static const char *const field_names[4] = {
    "robustBufferAccess fullDrawIndexUint32 imageCubeArray independentBlend geometryShader tessellationShader "
    "sampleRateShading dualSrcBlend logicOp multiDrawIndirect drawIndirectFirstInstance depthClamp depthBiasClamp "
    "fillModeNonSolid depthBounds wideLines largePoints alphaToOne multiViewport samplerAnisotropy textureCompressionETC2 "
    "textureCompressionASTC_LDR textureCompressionBC occlusionQueryPrecise pipelineStatisticsQuery "
    "vertexPipelineStoresAndAtomics fragmentStoresAndAtomics shaderTessellationAndGeometryPointSize shaderImageGatherExtended "
    "shaderStorageImageExtendedFormats shaderStorageImageMultisample shaderStorageImageReadWithoutFormat "
    "shaderStorageImageWriteWithoutFormat shaderUniformBufferArrayDynamicIndexing shaderSampledImageArrayDynamicIndexing "
    "shaderStorageBufferArrayDynamicIndexing shaderStorageImageArrayDynamicIndexing shaderClipDistance shaderCullDistance "
    "shaderFloat64 shaderInt64 shaderInt16 shaderResourceResidency shaderResourceMinLod sparseBinding sparseResidencyBuffer "
    "sparseResidencyImage2D sparseResidencyImage3D sparseResidency2Samples sparseResidency4Samples sparseResidency8Samples "
    "sparseResidency16Samples sparseResidencyAliased variableMultisampleRate inheritedQueries",
    "storageBuffer16BitAccess uniformAndStorageBuffer16BitAccess storagePushConstant16 storageInputOutput16 multiview "
    "multiviewGeometryShader multiviewTessellationShader variablePointersStorageBuffer variablePointers protectedMemory "
    "samplerYcbcrConversion shaderDrawParameters",
    "samplerMirrorClampToEdge drawIndirectCount storageBuffer8BitAccess uniformAndStorageBuffer8BitAccess storagePushConstant8 "
    "shaderBufferInt64Atomics shaderSharedInt64Atomics shaderFloat16 shaderInt8 descriptorIndexing "
    "shaderInputAttachmentArrayDynamicIndexing shaderUniformTexelBufferArrayDynamicIndexing "
    "shaderStorageTexelBufferArrayDynamicIndexing shaderUniformBufferArrayNonUniformIndexing "
    "shaderSampledImageArrayNonUniformIndexing shaderStorageBufferArrayNonUniformIndexing "
    "shaderStorageImageArrayNonUniformIndexing shaderInputAttachmentArrayNonUniformIndexing "
    "shaderUniformTexelBufferArrayNonUniformIndexing shaderStorageTexelBufferArrayNonUniformIndexing "
    "descriptorBindingUniformBufferUpdateAfterBind descriptorBindingSampledImageUpdateAfterBind "
    "descriptorBindingStorageImageUpdateAfterBind descriptorBindingStorageBufferUpdateAfterBind "
    "descriptorBindingUniformTexelBufferUpdateAfterBind descriptorBindingStorageTexelBufferUpdateAfterBind "
    "descriptorBindingUpdateUnusedWhilePending descriptorBindingPartiallyBound descriptorBindingVariableDescriptorCount "
    "runtimeDescriptorArray samplerFilterMinmax scalarBlockLayout imagelessFramebuffer uniformBufferStandardLayout "
    "shaderSubgroupExtendedTypes separateDepthStencilLayouts hostQueryReset timelineSemaphore bufferDeviceAddress "
    "bufferDeviceAddressCaptureReplay bufferDeviceAddressMultiDevice vulkanMemoryModel vulkanMemoryModelDeviceScope "
    "vulkanMemoryModelAvailabilityVisibilityChains shaderOutputViewportIndex shaderOutputLayer subgroupBroadcastDynamicId",
    "robustImageAccess inlineUniformBlock descriptorBindingInlineUniformBlockUpdateAfterBind pipelineCreationCacheControl "
    "privateData shaderDemoteToHelperInvocation shaderTerminateInvocation subgroupSizeControl computeFullSubgroups "
    "synchronization2 textureCompressionASTC_HDR shaderZeroInitializeWorkgroupMemory dynamicRendering shaderIntegerDotProduct "
    "maintenance4"};
_Static_assert(FIELDS(VkPhysicalDeviceFeatures, robustBufferAccess, inheritedQueries) == 55 &&
                   FIELDS(V11, storageBuffer16BitAccess, shaderDrawParameters) == 12 &&
                   FIELDS(V12, samplerMirrorClampToEdge, subgroupBroadcastDynamicId) == 47 &&
                   FIELDS(V13, robustImageAccess, maintenance4) == 15,
               "the names above");

static void field_name(int into, unsigned i, char *out, size_t n) {
    const char *p = field_names[into];
    for (unsigned k = 0; k < i && p; k++)
        if ((p = strchr(p, ' '))) p++;
    if (p && *p) snprintf(out, n, "%.*s", (int)strcspn(p, " "), p);
    else snprintf(out, n, "#%u", i);
}

static bool in_chain(const void *head, VkStructureType t) {
    for (const VkBaseOutStructure *s = head; s; s = s->pNext)
        if (s->sType == t) return true;
    return false;
}

// XeSS's own structures linked into our chain as they are: their pNext as XeSS made it, given back after vkCreateDevice
// (the memory is XeSS's; ours is a stack frame that ends)
#define LINKED_MAX 8
static VkBaseOutStructure *linked[LINKED_MAX];
static void *linked_next[LINKED_MAX];
static int n_linked;

void vk_xess_device_done(void) {
    for (int i = 0; i < n_linked; i++) linked[i]->pNext = linked_next[i];
    n_linked = 0;
}

bool vk_xess_device_features(VkPhysicalDeviceFeatures2 *f, VkPhysicalDeviceVulkan11Features *f11,
                             VkPhysicalDeviceVulkan12Features *f12, VkPhysicalDeviceVulkan13Features *f13, bool api13) {
    if (!dev_exts_ok) return false;
    void *chain = NULL;  // (NULL: XeSS makes a chain of its own, merged here: no structure twice, VUID-VkDeviceCreateInfo-pNext-02830)
    xess_result_t r = p_xessVKGetRequiredDeviceFeatures(g_vk.inst, g_vk.phys, &chain);
    if (r != XESS_RESULT_SUCCESS) {
        LOG("[xess] xessVKGetRequiredDeviceFeatures: %s (%d): FSR 3.1 is used\n", result_name(r), (int)r);
        refused(r);
        return false;
    }
    // first only look: a structure that can't be merged leaves our chain as it is
    int to_link = 0;
    for (const VkBaseOutStructure *s = chain; s; s = s->pNext) {
        const Known *k = known_type(s->sType);
        if ((!k || (k->into == 3 && !api13)) && ++to_link > LINKED_MAX) {
            LOG("[xess] it asks for more than %d feature structures of other kinds: FSR 3.1 is used\n", LINKED_MAX);
            g_xess_fallback = NO_XESS_START;
            return false;
        }
        if (k && k->into == 3 && !api13 && k->type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            LOG("[xess] it asks for Vulkan 1.3 features, and the device works as an older Vulkan: FSR 3.1 is used\n");
            g_xess_fallback = NO_XESS_GPU;
            return false;
        }
        if ((!k || (k->into == 3 && !api13)) && in_chain(f, s->sType)) {
            LOG("[xess] it asks for a feature structure (%d) our chain already has: FSR 3.1 is used\n", (int)s->sType);
            g_xess_fallback = NO_XESS_START;
            return false;
        }
    }
    void *into[4] = {f, f11, f12, f13};
    bool use13 = false;
    for (VkBaseOutStructure *s = chain, *next; s; s = next) {
        next = s->pNext;
        const Known *k = known_type(s->sType);
        char on[1024] = "", fn[64];
        if (k && (k->into < 3 || api13)) {  // its fields into ours
            VkBool32 *dst = (VkBool32 *)((char *)into[k->into] + k->at);
            const VkBool32 *src = (const VkBool32 *)((const char *)s + (k->into == 0 ? k->at : sizeof(VkBaseOutStructure)));
            unsigned first = k->into ? (unsigned)(k->at - sizeof(VkBaseOutStructure)) / sizeof(VkBool32) : 0;
            for (uint32_t i = 0; i < k->count; i++)
                if (src[i]) {
                    dst[i] = VK_TRUE;
                    field_name(k->into, first + i, fn, sizeof fn);
                    snprintf(on + strlen(on), sizeof on - strlen(on), "%s%s", *on ? ", " : "", fn);
                    use13 |= k->into == 3;
                }
            static const char *const where[4] = {"VkPhysicalDeviceFeatures2", "the Vulkan 1.1 features", "the Vulkan 1.2 features",
                                                 "the Vulkan 1.3 features"};
            LOG("[xess] device features: %s: %s -> %s\n", k->name, *on ? on : "none", where[k->into]);
        } else {  // its own structure (an extension's, or what 1.3 took in on an older device): linked after the 1.2 one
            linked[n_linked] = s, linked_next[n_linked++] = next;
            s->pNext = f12->pNext;
            f12->pNext = s;
            LOG("[xess] device features: structure %d%s linked as it is\n", (int)s->sType,
                s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT
                    ? " (VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT)" : "");
        }
    }
    if (use13) {
        f13->pNext = f12->pNext;
        f12->pNext = f13;
    }
    dev_ok = true;
    return true;
}

// Its queues: qi[0] is the game's (family 0, see the top); a queue in the dedicated compute family (the first with
// compute and without graphics) is added, as the XeSS SDK's Vulkan sample makes one. The game never uses it
uint32_t vk_xess_device_queues(VkDeviceQueueCreateInfo *qi, uint32_t n, uint32_t max) {
    if (!dev_ok || n >= max) return n;
    uint32_t nf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nf, NULL);
    VkQueueFamilyProperties *fp = calloc(nf ? nf : 1, sizeof *fp);
    if (fp) vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nf, fp);
    for (uint32_t i = 0; fp && i < nf; i++) {
        if (!(fp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) || (fp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !fp[i].queueCount) continue;
        bool have = false;
        for (uint32_t k = 0; k < n; k++) have |= qi[k].queueFamilyIndex == i;
        if (!have) {
            qi[n] = qi[0];
            qi[n].queueFamilyIndex = i, qi[n].queueCount = 1;
            n++;
            LOG("[xess] the device also gets a queue in family %u, the compute one (the game never uses it)\n", i);
        }
        break;
    }
    free(fp);
    return n;
}

// ================================================================== the context (fsr.c)
static void xess_log(const char *msg, xess_logging_level_t level) {  // (any thread)
    static const char *const lv[] = {"debug", "info", "warning", "ERROR"};
    static int n;
    int k = __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED);
    if (k < 300) LOG("[xess] runtime %s: %s\n", (unsigned)level < 4 ? lv[level] : "?", msg);
    else if (k == 300) LOG("[xess] runtime: more messages, not logged\n");
}

static const struct { const char *name, *key; xess_quality_settings_t q; } presets[] = {
    {"Native Anti-Aliasing", "aa", XESS_QUALITY_SETTING_AA},
    {"Ultra Quality Plus", "ultraqualityplus", XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS},
    {"Ultra Quality", "ultraquality", XESS_QUALITY_SETTING_ULTRA_QUALITY},
    {"Quality", "quality", XESS_QUALITY_SETTING_QUALITY},
    {"Balanced", "balanced", XESS_QUALITY_SETTING_BALANCED},
    {"Performance", "performance", XESS_QUALITY_SETTING_PERFORMANCE},
    {"Ultra Performance", "ultraperformance", XESS_QUALITY_SETTING_ULTRA_PERFORMANCE},
};
static const char *preset_name(xess_quality_settings_t q) {
    for (size_t i = 0; i < sizeof presets / sizeof *presets; i++)
        if (presets[i].q == q) return presets[i].name;
    return "?";
}

// XeSS takes the 3D picture at any size within the range xessGetOptimalInputResolution gives a preset
static bool covers(xess_quality_settings_t q, const xess_2d_t *out, uint32_t rw, uint32_t rh, xess_2d_t *opt, xess_2d_t *lo,
                   xess_2d_t *hi) {
    *opt = *lo = *hi = (xess_2d_t){0, 0};
    return p_xessGetOptimalInputResolution(ctx, out, q, opt, lo, hi) == XESS_RESULT_SUCCESS && rw >= lo->x && rh >= lo->y &&
           rw <= hi->x && rh <= hi->y;
}

// The context destroyed, its pipelines and memory with it (drawing thread, vk_lock held): only once nothing the GPU
// still has to run uses it. A pipeline build of its own still running is waited for first (up to 2 s)
static void destroy_now(const char *why) {
    if (!ctx) return;
    for (int i = 0; i < 2000 && p_xessGetPipelineBuildStatus(ctx) == XESS_RESULT_ERROR_OPERATION_IN_PROGRESS; i++) Sleep(1);
    xess_result_t r = p_xessDestroyContext(ctx);
    LOG("[xess] context destroyed (%s): %s\n", why, result_name(r));
    ctx = NULL;
    destroy_at = 0;
}

// XeSS-SR stops here (logged): FSR 3.1 from now on. When XeSS has recorded anything (this call, or a dispatch of an
// earlier frame the GPU may still be running), its context lives until the GPU is past the pending submission
// (vk_xess_frame), which comes after all of those; else it goes at once
static bool fail(const char *why, xess_result_t r, bool recorded) {
    LOG("[xess] %s (%s, %d): XeSS-SR is off from here on, FSR 3.1 takes over\n", why, result_name(r), (int)r);
    failed = true;
    refused(r);
    if (ctx && (recorded || frames)) {
        destroy_at = vk_pending_seq();
        LOG("[xess] its context is destroyed once the GPU has run submission %llu\n", (unsigned long long)destroy_at);
    } else {
        destroy_now("nothing of it recorded");
    }
    return false;
}

// fsr.c vk_fsr_init (start-up): nothing of XeSS is called here, only whether it can start (the device took its needs)
bool vk_xess_init(int quality) {
    if (!dev_ok) {
        if (!g_xess_fallback) g_xess_fallback = NO_XESS_START;
        return false;
    }
    quality_asked = quality < 0 || quality > 4 ? 0 : quality;
    LOG("[xess] its context is made on the thread that draws, at its first frame there\n");
    return true;
}

// The context, its preset and its pipeline build, started without waiting (drawing thread, vk_lock held). false: FSR 3.1
static bool start(void) {
    int quality = quality_asked;
    uint32_t rw = g_render_size[0], rh = g_render_size[1], ow = g_fsr_output[0], oh = g_fsr_output[1];
    uint64_t t0 = plat_now_ns();
    xess_result_t r = p_xessVKCreateContext(g_vk.inst, g_vk.phys, g_vk.dev, &ctx);
    if (r != XESS_RESULT_SUCCESS || !ctx) {
        LOG("[xess] xessVKCreateContext: %s (%d): FSR 3.1 is used\n", result_name(r), (int)r);
        ctx = NULL;
        refused(r);
        return false;
    }
    ctx_thread = GetCurrentThreadId();
    p_xessSetLoggingCallback(ctx, getenv("MHGU_XESS_LOG") ? XESS_LOGGING_LEVEL_DEBUG : XESS_LOGGING_LEVEL_WARNING, xess_log);
    r = p_xessIsOptimalDriver(ctx);
    xess_version_t fx = {0};
    p_xessGetIntelXeFXVersion(ctx, &fx);
    LOG("[xess] %s on %s: driver %s; Intel XeFX library %u.%u.%u%s\n", name, g_vk.props.deviceName,
        r == XESS_RESULT_SUCCESS ? "fine" : r == XESS_RESULT_WARNING_OLD_DRIVER ? "old (or the validation layer is on)" : result_name(r),
        fx.major, fx.minor, fx.patch, fx.major || fx.minor || fx.patch ? "" : " (none: XeSS's cross-vendor implementation)");
    // the preset: FSRQuality's of the same name (NativeAA: XeSS's Native Anti-Aliasing), or the first whose input range
    // holds the 3D picture's size (config.c keeps output / render within the FSRQuality ratio); MHGU_XESS_PRESET
    static const xess_quality_settings_t same_name[] = {XESS_QUALITY_SETTING_AA, XESS_QUALITY_SETTING_QUALITY,
                                                        XESS_QUALITY_SETTING_BALANCED, XESS_QUALITY_SETTING_PERFORMANCE,
                                                        XESS_QUALITY_SETTING_ULTRA_PERFORMANCE};
    static const char *const fsr_names[] = {"NativeAA", "Quality", "Balanced", "Performance", "UltraPerformance"};
    preset = same_name[quality];
    const char *env = getenv("MHGU_XESS_PRESET"), *how = "the FSRQuality of the same name";
    for (size_t i = 0; env && i < sizeof presets / sizeof *presets; i++)
        if (!strcasecmp(env, presets[i].key)) preset = presets[i].q, how = "MHGU_XESS_PRESET";
    xess_2d_t out = {ow, oh}, opt, lo, hi;
    if (!covers(preset, &out, rw, rh, &opt, &lo, &hi)) {
        LOG("[xess] preset %s takes %ux%u .. %ux%u for %ux%u, not the 3D picture's %ux%u: another one\n", preset_name(preset),
            lo.x, lo.y, hi.x, hi.y, ow, oh, rw, rh);
        bool found = false;
        for (size_t i = 0; i < sizeof presets / sizeof *presets && !found; i++)
            if ((found = covers(presets[i].q, &out, rw, rh, &opt, &lo, &hi))) preset = presets[i].q, how = "the first that takes its size";
        if (!found) {
            LOG("[xess] no preset takes a %ux%u picture for %ux%u: FSR 3.1 is used\n", rw, rh, ow, oh);
            g_xess_fallback = NO_XESS_START;
            destroy_now("nothing of it recorded");
            return false;
        }
    }
    LOG("[xess] FSRQuality %s -> preset %s (%s): the 3D picture %ux%u, x%.3f; for %ux%u it takes %ux%u .. %ux%u (its own size "
        "%ux%u)\n", fsr_names[quality], preset_name(preset), how, rw, rh, (double)ow / rw, ow, oh, lo.x, lo.y, hi.x, hi.y, opt.x, opt.y);
    flags = XESS_INIT_FLAG_LDR_INPUT_COLOR | (getenv("MHGU_XESS_NOMASK") ? 0 : XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK);
    xess_properties_t pr = {0};
    if (p_xessGetProperties(ctx, &out, &pr) == XESS_RESULT_SUCCESS)
        LOG("[xess] temporary memory of its own: %.1f MB buffers, %.1f MB images\n", pr.tempBufferHeapSize / 1048576.0,
            pr.tempTextureHeapSize / 1048576.0);
    // its pipelines built now, in the background (into the pipeline cache: cache/vk_pipelines.bin), waited for at its init
    uint64_t t1 = plat_now_ns();
    r = test("build") ? XESS_RESULT_ERROR_UNKNOWN : p_xessVKBuildPipelines(ctx, g_vk.pcache, false, flags);
    LOG("[xess] context made on thread %lu, pipeline build started (%s%s, %.1f ms; %.1f ms in all)\n", (unsigned long)ctx_thread,
        result_name(r), test("build") ? ", MHGU_XESS_FAILTEST" : "", (plat_now_ns() - t1) / 1e6, (plat_now_ns() - t0) / 1e6);
    if (r != XESS_RESULT_SUCCESS) {
        refused(r);
        destroy_now("nothing of it recorded");
        return false;
    }
    ready = true;
    return true;
}

// fsr.c hud_start, every frame while Upscaler = XeSS (the drawing thread, vk_lock held): the context at the first one;
// later, a context XeSS stopped with destroyed once the GPU is done with it
void vk_xess_frame(void) {
    if (!started && quality_asked >= 0) {
        started = true;
        if (!start()) {
            failed = true;
            LOG("[xess] XeSS-SR could not start: FSR 3.1 is used\n");
        }
    }
    if (destroy_at && vk_completed_seq() >= destroy_at) destroy_now("the GPU is past its last submission");
}

bool vk_xess_ready(void) { return ready && !failed; }
const char *vk_xess_name(void) { return name; }

// ================================================================== dispatch
static bool init_now(const XessDispatch *d) {
    xess_vk_init_params_t ip = {.outputResolution = {d->out[0], d->out[1]}, .qualitySetting = preset, .initFlags = flags,
                                .pipelineCache = g_vk.pcache};
    uint64_t t0 = plat_now_ns();
    xess_result_t built = p_xessGetPipelineBuildStatus(ctx);
    xess_result_t r = test("init") ? XESS_RESULT_ERROR_UNKNOWN : p_xessVKInit(ctx, &ip);
    if (r != XESS_RESULT_SUCCESS)  // (its upload, if any, is over: xessVKInit waits for it, it frees its staging memory)
        return fail(test("init") ? "xessVKInit failed (MHGU_XESS_FAILTEST)" : "xessVKInit failed", r, false);
    init_thread = GetCurrentThreadId();
    LOG("[xess] initialized in %.0f ms (pipelines %s), %ux%u -> %ux%u, preset %s, LDR colour%s; on thread %lu (the "
        "context was made on %lu%s)\n", (plat_now_ns() - t0) / 1e6,
        built == XESS_RESULT_SUCCESS ? "built already" : "still being built: waited for", d->render[0], d->render[1], d->out[0],
        d->out[1], preset_name(preset), flags & XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK ? ", responsive pixel mask" : "",
        (unsigned long)init_thread, (unsigned long)ctx_thread, init_thread == ctx_thread ? ", the same" : ": ANOTHER ONE");
    float mx = 1, my = 1;
    const char *e = getenv("MHGU_XESS_MV_SCALE");
    if (e && sscanf(e, "%f,%f", &mx, &my) == 2) {
        p_xessSetVelocityScale(ctx, mx, my);
        LOG("[xess] velocity scale %g, %g (MHGU_XESS_MV_SCALE)\n", mx, my);
    }
    inited = true;
    return true;
}

static xess_vk_image_view_info view_info(TexBackend *b) {
    static const VkComponentMapping id = {0};
    xess_vk_image_view_info v = {0};
    if (!b) return v;
    v.imageView = vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, b->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
    v.image = b->image;
    v.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    v.format = b->format;
    v.width = b->width, v.height = b->height;
    return v;
}

// The inputs between GENERAL (where every image lives here) and SHADER_READ_ONLY_OPTIMAL (where XeSS reads them)
static void inputs_layout(VkCommandBuffer cb, TexBackend *const *in, uint32_t n, bool to_xess) {
    VkImageMemoryBarrier b[4];
    for (uint32_t i = 0; i < n; i++)
        b[i] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                      .srcAccessMask = to_xess ? VK_ACCESS_ALL_WRITES : VK_ACCESS_SHADER_READ_BIT,
                                      .dstAccessMask = to_xess ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_ALL,
                                      .oldLayout = to_xess ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      .newLayout = to_xess ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
                                      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .image = in[i]->image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, to_xess ? VK_STAGES_ALL : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         to_xess ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_STAGES_ALL, 0, 0, NULL, 0, NULL, n, b);
}

bool vk_xess_dispatch(const XessDispatch *d) {
    if (!vk_xess_ready()) return false;
    if (!inited && !init_now(d)) return false;
    static bool other_thread;
    if (!other_thread && GetCurrentThreadId() != init_thread) {
        other_thread = true;
        LOG("[xess] a dispatch from thread %lu, not %lu where it was initialized (calls stay one at a time: vk_lock)\n",
            (unsigned long)GetCurrentThreadId(), (unsigned long)init_thread);
    }
    bool mask = flags & XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
    xess_vk_execute_params_t ep = {.colorTexture = view_info(d->color), .velocityTexture = view_info(d->motion),
                                   .depthTexture = view_info(d->depth), .outputTexture = view_info(d->output),
                                   .exposureScale = 1.0f, .resetHistory = d->reset, .inputWidth = d->render[0],
                                   .inputHeight = d->render[1]};
    if (mask) ep.responsivePixelMaskTexture = view_info(d->reactive);
    if (!ep.colorTexture.imageView || !ep.velocityTexture.imageView || !ep.depthTexture.imageView ||
        !ep.outputTexture.imageView || (mask && !ep.responsivePixelMaskTexture.imageView))
        return fail("an image view for it could not be made", XESS_RESULT_ERROR_DEVICE, false);
    static float js[2];  // (MHGU_XESS_JITTER_SIGN=sx,sy: other sign conventions, to check this one)
    if (!js[0]) {
        const char *e = getenv("MHGU_XESS_JITTER_SIGN");
        if (!e || sscanf(e, "%f,%f", &js[0], &js[1]) != 2 || !js[0]) js[0] = js[1] = 1.0f;
        else LOG("[xess] jitter times %g, %g (MHGU_XESS_JITTER_SIGN)\n", js[0], js[1]);
    }
    ep.jitterOffsetX = js[0] * d->jitter[0];
    ep.jitterOffsetY = js[1] * d->jitter[1];
    TexBackend *in[4] = {d->color, d->motion, d->depth, d->reactive};
    uint32_t nin = mask ? 4 : 3;
    inputs_layout(d->cb, in, nin, true);
    static long fail_at = -2;  // (test: MHGU_XESS_FAILTEST=n reports the n-th dispatch as failed, to see the fallback)
    if (fail_at == -2) fail_at = failtest && failtest[0] >= '0' && failtest[0] <= '9' ? atol(failtest) : -1;
    bool fake = fail_at >= 0 && frames == (uint64_t)fail_at;
    xess_result_t r = fake ? XESS_RESULT_ERROR_UNKNOWN : p_xessVKExecute(ctx, d->cb, &ep);
    inputs_layout(d->cb, in, nin, false);  // (failed too: FSR 3.1 takes them in GENERAL)
    if (r != XESS_RESULT_SUCCESS)  // (it may have recorded part of its work before failing)
        return fail(fake ? "xessVKExecute failed (MHGU_XESS_FAILTEST)" : "xessVKExecute failed", r, true);
    if (++frames == 1)
        LOG("[xess] first dispatch recorded: colour %d, motion vectors %d, depth %d, mask %d, output %d (VkFormat)\n",
            d->color->format, d->motion->format, d->depth->format, mask ? d->reactive->format : 0, d->output->format);
    static const char *dump;
    if (frames == 1) dump = getenv("MHGU_XESS_DUMP");
    if (dump && frames == 600) {
        xess_dump_parameters_t dp = {.path = dump, .frame_idx = 0, .frame_count = 8, .dump_elements_mask = XESS_DUMP_ALL_INPUTS | XESS_DUMP_OUTPUT};
        xess_result_t dr = p_xessStartDump ? p_xessStartDump(ctx, &dp) : XESS_RESULT_ERROR_NOT_IMPLEMENTED;
        LOG("[xess] dump of 8 frames into %s: %s\n", dump, result_name(dr));
    }
    return true;
}

#endif
