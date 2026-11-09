// XeSS-SR (XeSS Super Resolution) as the third upscaler ([Upscaler] Upscaler = XeSS): runtime/nvn/vulkan/xess.c.
// It takes exactly the inputs fsr.c prepares for FSR 3.1 and is dispatched where the FSR 3.1 dispatch would be.
// Windows only: libxess.dll (the XeSS SDK's runtime, unmodified) is loaded at run time from MHGU.exe's folder, never
// linked. On other systems, or when it is missing or can't run here, everything says "no" and fsr.c uses FSR 3.1,
// with the reason in the log and in g_xess_fallback (the menu's note).
// Nothing here runs unless Upscaler = XeSS at start: device.c asks for XeSS's instance / device extensions and features
// only then, so the device and every frame of the other upscalers stay as they are.
#pragma once
#include "vk.h"

typedef struct {
    VkCommandBuffer cb;
    TexBackend *color, *depth, *motion, *reactive, *output;  // render size but the output; all live in GENERAL (vk.h)
    float jitter[2];  // the picture shift in render pixels, x right, y down (as for FSR 3.1)
    uint32_t render[2], out[2];
    bool reset;
} XessDispatch;

// device.c create_instance, only for FSR on + Upscaler = XeSS: load libxess.dll, add the instance extensions it asks
// for that ext[] lacks (room for max in all) and raise *api to its Vulkan version. false: XeSS-SR can't run (logged,
// the menu's note set) and nothing was added.
bool vk_xess_instance_exts(const char **ext, uint32_t *n_ext, uint32_t max, uint32_t *api);
// device.c create_device: the device extensions XeSS asks for (before the extension list is used up), then its
// features, merged into our chain by hand: VkPhysicalDeviceFeatures2 into f, what Vulkan 1.1 / 1.2 / 1.3 took in into
// f11 / f12 / f13 (f13 linked after f12 when it is needed and api13: the device works as 1.3), anything else linked
// after f12. Both false: XeSS-SR can't run (logged, the note set); then the extensions stay out (n_ext as it was) and
// the chain is as it was.
bool vk_xess_device_exts(const VkExtensionProperties *ep, uint32_t n, const char **ext, uint32_t *n_ext, uint32_t max);
bool vk_xess_device_features(VkPhysicalDeviceFeatures2 *f, VkPhysicalDeviceVulkan11Features *f11,
                             VkPhysicalDeviceVulkan12Features *f12, VkPhysicalDeviceVulkan13Features *f13, bool api13);
// then its queues: qi[0..n) are the game's (qi[0] its own), room for max; returns the new count (n: none added)
uint32_t vk_xess_device_queues(VkDeviceQueueCreateInfo *qi, uint32_t n, uint32_t max);
void vk_xess_device_done(void);  // right after vkCreateDevice with them: XeSS's own structures unlinked from our chain
// vkCreateInstance / vkCreateDevice refused XeSS's additions: made again without them, and XeSS-SR is off
void vk_xess_creation_failed(const char *what, VkResult r);

// fsr.c vk_fsr_init (start-up): whether XeSS-SR can start (the device took its needs); nothing of XeSS is called yet.
// false: FSR 3.1
bool vk_xess_init(int quality);
// fsr.c hud_start, every frame while Upscaler = XeSS (the drawing thread, vk_lock held): XeSS's context, its preset and
// its pipeline build at the first one, so every XeSS call from the context on is made on that thread, as its guide
// asks; later, a context XeSS stopped with is destroyed there once the GPU no longer uses it
void vk_xess_frame(void);
bool vk_xess_ready(void);        // XeSS-SR started and has not failed (yet)
const char *vk_xess_name(void);  // "XeSS-SR 2.0.2"
// Record the upscale into d->cb (vk_lock held; the first one also initializes XeSS). false: it failed, XeSS-SR is off
// from here on (logged) and the images are back in GENERAL
bool vk_xess_dispatch(const XessDispatch *d);
