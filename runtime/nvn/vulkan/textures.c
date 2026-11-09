// Vulkan textures: NVN formats, images, change-detected uploads from guest memory, views and samplers. Ported from
// LAS Recomp's runtime/nvn/vulkan/textures.c (without its output scaling and texture patches); guest addresses go
// through G2H.
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "../astc.h"
#include <unistd.h>
#ifdef _WIN32
#include <SDL3/SDL_cpuinfo.h>  // (no sysconf)
#endif
#include "../bcn.h"

// ================================================================== formats
typedef struct { VkFormat vf; uint8_t expand_rgb, alpha_one; } VkFmt;

// DEPTH24_STENCIL8 as itself where the GPU has it for everything the game does with depth (NVIDIA, Intel); AMD has
// no D24S8 attachments: D32_SFLOAT_S8_UINT there. MHGU_VK_D24=0: always D32S8.
static VkFormat depth24_stencil8(void) {
    static VkFormat f;
    if (!f) {
        const char *e = getenv("MHGU_VK_D24");
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_D24_UNORM_S8_UINT, &p);
        VkFormatFeatureFlags want = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                    VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        bool ok = (p.optimalTilingFeatures & want) == want && !(e && !atoi(e));
        f = ok ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
        LOG("[vk] depth24 stencil8: %s\n", ok ? "D24_UNORM_S8_UINT" : "D32_SFLOAT_S8_UINT");
    }
    return f;
}
VkFormat vk_depth24_stencil8(void) { return depth24_stencil8(); }

static VkFmt fmt_of(uint32_t f) {
    switch (f) {
    case 0x01: return (VkFmt){VK_FORMAT_R8_UNORM};
    case 0x02: return (VkFmt){VK_FORMAT_R8_SNORM};
    case 0x03: return (VkFmt){VK_FORMAT_R8_UINT};
    case 0x04: return (VkFmt){VK_FORMAT_R8_SINT};
    case 0x05: return (VkFmt){VK_FORMAT_R16_SFLOAT};
    case 0x06: return (VkFmt){VK_FORMAT_R16_UNORM};
    case 0x07: return (VkFmt){VK_FORMAT_R16_SNORM};
    case 0x08: return (VkFmt){VK_FORMAT_R16_UINT};
    case 0x09: return (VkFmt){VK_FORMAT_R16_SINT};
    case 0x0A: return (VkFmt){VK_FORMAT_R32_SFLOAT};
    case 0x0B: return (VkFmt){VK_FORMAT_R32_UINT};
    case 0x0C: return (VkFmt){VK_FORMAT_R32_SINT};
    case 0x0D: return (VkFmt){VK_FORMAT_R8G8_UNORM};
    case 0x0E: return (VkFmt){VK_FORMAT_R8G8_SNORM};
    case 0x0F: return (VkFmt){VK_FORMAT_R8G8_UINT};
    case 0x10: return (VkFmt){VK_FORMAT_R8G8_SINT};
    case 0x11: return (VkFmt){VK_FORMAT_R16G16_SFLOAT};
    case 0x12: return (VkFmt){VK_FORMAT_R16G16_UNORM};
    case 0x13: return (VkFmt){VK_FORMAT_R16G16_SNORM};
    case 0x14: return (VkFmt){VK_FORMAT_R16G16_UINT};
    case 0x15: return (VkFmt){VK_FORMAT_R16G16_SINT};
    case 0x16: return (VkFmt){VK_FORMAT_R32G32_SFLOAT};
    case 0x17: return (VkFmt){VK_FORMAT_R32G32_UINT};
    case 0x18: return (VkFmt){VK_FORMAT_R32G32_SINT};
    case 0x19: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 1, 1};  // RGB8: expanded on upload
    case 0x25: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    case 0x26: return (VkFmt){VK_FORMAT_R8G8B8A8_SNORM};
    case 0x27: return (VkFmt){VK_FORMAT_R8G8B8A8_UINT};
    case 0x28: return (VkFmt){VK_FORMAT_R8G8B8A8_SINT};
    case 0x29: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT};
    case 0x2A: return (VkFmt){VK_FORMAT_R16G16B16A16_UNORM};
    case 0x2B: return (VkFmt){VK_FORMAT_R16G16B16A16_SNORM};
    case 0x2C: return (VkFmt){VK_FORMAT_R16G16B16A16_UINT};
    case 0x2D: return (VkFmt){VK_FORMAT_R16G16B16A16_SINT};
    case 0x2E: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT};
    case 0x2F: return (VkFmt){VK_FORMAT_R32G32B32A32_UINT};
    case 0x30: return (VkFmt){VK_FORMAT_R32G32B32A32_SINT};
    case 0x31: return (VkFmt){VK_FORMAT_S8_UINT};
    case 0x32: return (VkFmt){VK_FORMAT_D16_UNORM};
    case 0x33: return (VkFmt){VK_FORMAT_D32_SFLOAT};  // DEPTH24
    case 0x34: return (VkFmt){VK_FORMAT_D32_SFLOAT};
    case 0x35: return (VkFmt){depth24_stencil8()};
    case 0x36: return (VkFmt){VK_FORMAT_D32_SFLOAT_S8_UINT};
    case 0x37: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB, 0, 1};
    case 0x38: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB};
    case 0x39: return (VkFmt){VK_FORMAT_R4G4B4A4_UNORM_PACK16};
    case 0x3A: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};
    case 0x3B: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};
    case 0x3C: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};
    case 0x3D: return (VkFmt){VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    case 0x3E: return (VkFmt){VK_FORMAT_A2B10G10R10_UINT_PACK32};
    case 0x3F: return (VkFmt){VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    case 0x40: return (VkFmt){VK_FORMAT_E5B9G9R9_UFLOAT_PACK32};
    case 0x41: case 0x42: return (VkFmt){VK_FORMAT_BC1_RGBA_UNORM_BLOCK};
    case 0x43: return (VkFmt){VK_FORMAT_BC2_UNORM_BLOCK};
    case 0x44: return (VkFmt){VK_FORMAT_BC3_UNORM_BLOCK};
    case 0x45: case 0x46: return (VkFmt){VK_FORMAT_BC1_RGBA_SRGB_BLOCK};
    case 0x47: return (VkFmt){VK_FORMAT_BC2_SRGB_BLOCK};
    case 0x48: return (VkFmt){VK_FORMAT_BC3_SRGB_BLOCK};
    case 0x49: return (VkFmt){VK_FORMAT_BC4_UNORM_BLOCK};
    case 0x4A: return (VkFmt){VK_FORMAT_BC4_SNORM_BLOCK};
    case 0x4B: return (VkFmt){VK_FORMAT_BC5_UNORM_BLOCK};
    case 0x4C: return (VkFmt){VK_FORMAT_BC5_SNORM_BLOCK};
    case 0x4D: return (VkFmt){VK_FORMAT_BC7_UNORM_BLOCK};
    case 0x4E: return (VkFmt){VK_FORMAT_BC7_SRGB_BLOCK};
    case 0x4F: return (VkFmt){VK_FORMAT_BC6H_SFLOAT_BLOCK};
    case 0x50: return (VkFmt){VK_FORMAT_BC6H_UFLOAT_BLOCK};
    case 0x6D: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 0, 1};
    case 0x71: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT, 0, 1};
    case 0x76: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT, 0, 1};
    case 0x79: return (VkFmt){VK_FORMAT_ASTC_4x4_UNORM_BLOCK};
    case 0x7A: return (VkFmt){VK_FORMAT_ASTC_5x4_UNORM_BLOCK};
    case 0x7B: return (VkFmt){VK_FORMAT_ASTC_5x5_UNORM_BLOCK};
    case 0x7C: return (VkFmt){VK_FORMAT_ASTC_6x5_UNORM_BLOCK};
    case 0x7D: return (VkFmt){VK_FORMAT_ASTC_6x6_UNORM_BLOCK};
    case 0x7E: return (VkFmt){VK_FORMAT_ASTC_8x5_UNORM_BLOCK};
    case 0x7F: return (VkFmt){VK_FORMAT_ASTC_8x6_UNORM_BLOCK};
    case 0x80: return (VkFmt){VK_FORMAT_ASTC_8x8_UNORM_BLOCK};
    case 0x81: return (VkFmt){VK_FORMAT_ASTC_10x5_UNORM_BLOCK};
    case 0x82: return (VkFmt){VK_FORMAT_ASTC_10x6_UNORM_BLOCK};
    case 0x83: return (VkFmt){VK_FORMAT_ASTC_10x8_UNORM_BLOCK};
    case 0x84: return (VkFmt){VK_FORMAT_ASTC_10x10_UNORM_BLOCK};
    case 0x85: return (VkFmt){VK_FORMAT_ASTC_12x10_UNORM_BLOCK};
    case 0x86: return (VkFmt){VK_FORMAT_ASTC_12x12_UNORM_BLOCK};
    case 0x87: return (VkFmt){VK_FORMAT_ASTC_4x4_SRGB_BLOCK};
    case 0x88: return (VkFmt){VK_FORMAT_ASTC_5x4_SRGB_BLOCK};
    case 0x89: return (VkFmt){VK_FORMAT_ASTC_5x5_SRGB_BLOCK};
    case 0x8A: return (VkFmt){VK_FORMAT_ASTC_6x5_SRGB_BLOCK};
    case 0x8B: return (VkFmt){VK_FORMAT_ASTC_6x6_SRGB_BLOCK};
    case 0x8C: return (VkFmt){VK_FORMAT_ASTC_8x5_SRGB_BLOCK};
    case 0x8D: return (VkFmt){VK_FORMAT_ASTC_8x6_SRGB_BLOCK};
    case 0x8E: return (VkFmt){VK_FORMAT_ASTC_8x8_SRGB_BLOCK};
    case 0x8F: return (VkFmt){VK_FORMAT_ASTC_10x5_SRGB_BLOCK};
    case 0x90: return (VkFmt){VK_FORMAT_ASTC_10x6_SRGB_BLOCK};
    case 0x91: return (VkFmt){VK_FORMAT_ASTC_10x8_SRGB_BLOCK};
    case 0x92: return (VkFmt){VK_FORMAT_ASTC_10x10_SRGB_BLOCK};
    case 0x93: return (VkFmt){VK_FORMAT_ASTC_12x10_SRGB_BLOCK};
    case 0x94: return (VkFmt){VK_FORMAT_ASTC_12x12_SRGB_BLOCK};
    case 0x95: case 0x96: case 0x97: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};
    case 0x98: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM, 0, 1};
    case 0x99: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM};
    case 0x9A: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB, 0, 1};
    case 0x9B: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB};
    default: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    }
}

bool renderer_format_is_depth(uint32_t f) { return f >= 0x31 && f <= 0x36; }
bool renderer_format_has_stencil(uint32_t f) { return f == 0x31 || f == 0x35 || f == 0x36; }
VkFormat vk_format(uint32_t f) { return fmt_of(f).vf; }
bool vk_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }

static VkImageAspectFlags aspect_of(uint32_t f) {
    if (f == 0x31) return VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f == 0x35 || f == 0x36) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (renderer_format_is_depth(f)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

static bool format_supports(VkFormat f, VkFormatFeatureFlags want) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & want) == want;
}

// ---- wide render targets
// The game renders into RGBA8 targets only. A night sky leaves the scene pass a few codes above black (here R 5-18,
// G 12-28, B 15-32 over the whole sky) and the post passes after it (exposure, colour grading) brighten it several
// times, so the codes end up 2-5 apart on screen: bands. Render targets the game made RGBA8 are rendered as RGBA16
// UNORM instead (the same [0, 1] range, clamping and blending, 8 more bits), so only the final picture is rounded to
// 8 bits. Every RGBA8 2D texture the game renders into is re-made wide before its first pass (vk_rt_widen), except
// FSR's input (the post-processed picture: the brightening is done by then); exec.c's copies between such a target and
// guest memory convert. [Graphics] SmoothGradients (off by default: it costs GPU time), live: targets are re-made at
// their next pass.
bool vk_wide_rt_on(void) {
    static int supported = -1;
    if (supported < 0) {
        VkFormatFeatureFlags want = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                    VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        supported = format_supports(VK_FORMAT_R16G16B16A16_UNORM, want);
        if (!supported) LOG("[vk] RGBA16 UNORM render targets not supported: SmoothGradients has no effect\n");
    }
    return supported && config_live_smooth_gradients();
}

// ================================================================== images
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_ALL,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = img, .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_STAGES_ALL, 0, 0, NULL, 0, NULL, 1, &b);
}

// Objects destroyed by the guest (or replaced) are kept until the submissions that may use them have completed.
typedef struct { VkImage img; VmaAllocation mem; VkImageView view; VkFramebuffer fb; VkBuffer buf; uint64_t seq; } Dead;
static Dead *dead;
static int ndead, cap_dead;
static void views_forget(TexBackend *b);
static void up_forget(TexBackend *b);

void vk_collect_dead(void) {
    uint64_t done = vk_completed_seq();
    uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
    int k = 0;
    for (int i = 0; i < ndead; i++) {
        if (dead[i].seq > done) { dead[k++] = dead[i]; continue; }
        if (dead[i].fb) vkDestroyFramebuffer(g_vk.dev, dead[i].fb, NULL);
        else if (dead[i].buf) vmaDestroyBuffer(g_vk.vma, dead[i].buf, dead[i].mem);
        else if (dead[i].view) vkDestroyImageView(g_vk.dev, dead[i].view, NULL);
        else vmaDestroyImage(g_vk.vma, dead[i].img, dead[i].mem);
    }
    if (HITCH_ON && k < ndead) hitch_add(HC_DEAD, plat_now_ns() - t0, (uint64_t)(ndead - k));
    ndead = k;
}

static void defer(Dead d) {
    if (ndead == cap_dead) { cap_dead = cap_dead ? cap_dead * 2 : 256; dead = realloc(dead, cap_dead * sizeof *dead); }
    d.seq = vk_pending_seq();
    dead[ndead++] = d;
}

void vk_defer_image(VkImage img, VmaAllocation mem) { defer((Dead){.img = img, .mem = mem}); }
void vk_defer_view(VkImageView v) { vk_framebuffers_forget_view(v); defer((Dead){.view = v}); }
void vk_defer_framebuffer(VkFramebuffer f) { defer((Dead){.fb = f}); }
void vk_defer_buffer(VkBuffer b, VmaAllocation mem) { defer((Dead){.buf = b, .mem = mem}); }

// A phone with little memory and no BC support: every decoded texture takes four to eight times what it does compressed.
// There, 3D textures (those with mipmaps: menus and text have none, and stay sharp) lose their largest level, a quarter
// of the memory for a picture no sharper on such a screen. (Levels line up: the view's first level is the next one.)
static bool low_memory_textures(void) {
    static int on = -1;
    if (on < 0) {
#ifdef _WIN32
        uint64_t ram = (uint64_t)SDL_GetSystemRAM() << 20;
#else
        uint64_t ram = (uint64_t)sysconf(_SC_PHYS_PAGES) * (uint64_t)sysconf(_SC_PAGESIZE);
#endif
        on = ram < 5ull << 30;
        if (on) LOG("[vk] %llu MB of memory: 3D textures without their largest level\n", (unsigned long long)(ram >> 20));
    }
    return on;
}

// 3 GB or less (phones that ran out of GPU memory, vkQueueSubmit failing after minutes): big 3D textures lose two levels
static bool very_low_memory_textures(void) {
    static int on = -1;
    if (on < 0) {
#ifdef _WIN32
        on = 0;
#else
        on = (uint64_t)sysconf(_SC_PHYS_PAGES) * (uint64_t)sysconf(_SC_PAGESIZE) < 3584ull << 20;
        if (on) LOG("[vk] 3.5 GB or less: big 3D textures without their two largest levels\n");
#endif
    }
    return on;
}

static void create_image(NvnTexture *t, TexBackend *b) {
    VkFmt mf = fmt_of(t->format);
    uint32_t tw = t->width ? t->width : 1, th = t->height ? t->height : 1;
    VkImageType type = VK_IMAGE_TYPE_2D;
    uint32_t layers = 1, depth = 1;
    switch (t->target) {
    case 0: type = VK_IMAGE_TYPE_1D; th = 1; break;
    case 2: type = VK_IMAGE_TYPE_3D; depth = t->depth ? t->depth : 1; break;
    case 3: type = VK_IMAGE_TYPE_1D; layers = t->height ? t->height : 1; th = 1; break;
    case 4: case 6: layers = t->depth ? t->depth : 1; break;
    case 8: layers = 6; break;
    case 9: layers = t->depth ? t->depth : 6; break;
    default: break;
    }
    uint32_t levels = t->levels ? t->levels : 1;
    {   // never more levels than the size allows (odd guest values)
        uint32_t m = 1, sz = tw > th ? tw : th;
        if (depth > sz) sz = depth;
        while (sz > 1) { sz >>= 1; m++; }
        if (levels > m) levels = m;
    }
    uint32_t samples = t->samples > 1 ? t->samples : 1;
    if (samples > 1) levels = 1;
    VkFormat vf = mf.vf;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    b->astc_bw = b->astc_bh = 0;
    b->bcn = BCN_NONE;
    b->mip_skip = 0;
    if (!format_supports(vf, need)) {
        bool astc = vf >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && vf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
        const FormatInfo *fi = t->layout.fmt;
        int bcn = vf == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || vf == VK_FORMAT_BC1_RGBA_SRGB_BLOCK ? BCN_BC1
                : vf == VK_FORMAT_BC2_UNORM_BLOCK || vf == VK_FORMAT_BC2_SRGB_BLOCK           ? BCN_BC2
                : vf == VK_FORMAT_BC3_UNORM_BLOCK || vf == VK_FORMAT_BC3_SRGB_BLOCK           ? BCN_BC3
                : vf == VK_FORMAT_BC4_UNORM_BLOCK                                             ? BCN_BC4U
                : vf == VK_FORMAT_BC4_SNORM_BLOCK                                             ? BCN_BC4S
                : vf == VK_FORMAT_BC5_UNORM_BLOCK                                             ? BCN_BC5U
                : vf == VK_FORMAT_BC5_SNORM_BLOCK                                             ? BCN_BC5S
                : vf == VK_FORMAT_BC7_UNORM_BLOCK || vf == VK_FORMAT_BC7_SRGB_BLOCK           ? BCN_BC7
                : vf == VK_FORMAT_BC6H_SFLOAT_BLOCK                                           ? BCN_BC6HS
                : vf == VK_FORMAT_BC6H_UFLOAT_BLOCK                                           ? BCN_BC6HU
                                                                                              : BCN_NONE;
        if (bcn && fi && fi->bw == 4 && fi->bh == 4) {  // decoded on the CPU (bcn.c) into a plain format
            bool srgb = vf == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || vf == VK_FORMAT_BC2_SRGB_BLOCK ||
                        vf == VK_FORMAT_BC3_SRGB_BLOCK || vf == VK_FORMAT_BC7_SRGB_BLOCK;
            b->bcn = (uint8_t)bcn;
            vf = bcn == BCN_BC6HS || bcn == BCN_BC6HU ? VK_FORMAT_R16G16B16A16_SFLOAT
               : bcn == BCN_BC4S || bcn == BCN_BC5S   ? VK_FORMAT_R8G8B8A8_SNORM
               : srgb                                 ? VK_FORMAT_R8G8B8A8_SRGB
                                                      : VK_FORMAT_R8G8B8A8_UNORM;
            b->bcn_tb = (uint8_t)bcn_texel_bytes(bcn);
            // one and two channels in their own size (sampled the same: the missing ones read 0, alpha 1)
            VkFormat narrow = bcn == BCN_BC4U ? VK_FORMAT_R8_UNORM : bcn == BCN_BC4S ? VK_FORMAT_R8_SNORM
                            : bcn == BCN_BC5U ? VK_FORMAT_R8G8_UNORM : bcn == BCN_BC5S ? VK_FORMAT_R8G8_SNORM : VK_FORMAT_UNDEFINED;
            if (narrow != VK_FORMAT_UNDEFINED && format_supports(narrow, need)) {
                vf = narrow;
                b->bcn_tb = bcn == BCN_BC4U || bcn == BCN_BC4S ? 1 : 2;
            }
            if (levels >= 4 && layers == 1 && depth == 1 && samples == 1 && type == VK_IMAGE_TYPE_2D && t->target != 8 &&
                t->target != 9 && (tw >= 256 || th >= 256) && tw >= 4 && th >= 4 && (low_memory_textures() || g_config.low_end))
                b->mip_skip = (g_config.low_end || very_low_memory_textures()) && levels >= 5 && (tw >= 512 || th >= 512) ? 2
                            : g_config.low_end == LOW_END_TOASTER && levels >= 5 ? 2  // (Toaster: the 256s too)
                                                                                 : 1;
            static int nlog;
            if (nlog++ < 4) LOG("[vk] BC format %d not supported by the GPU: decoded on the CPU\n", mf.vf);
        } else if (astc && fi && fi->bpb == 16) {  // decoded on the CPU into an RGBA8 image of the same colour space
            b->astc_bw = fi->bw;
            b->astc_bh = fi->bh;
            b->astc_srgb = t->format >= 0x87;
            vf = b->astc_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        } else {
            static int nlog;
            if (nlog++ < 20) LOG("[vk] format %d (nvn %#x) not supported: RGBA8 instead\n", vf, t->format);
            vf = VK_FORMAT_R8G8B8A8_UNORM;
        }
    }
    if (b->wide) vf = VK_FORMAT_R16G16B16A16_UNORM;
    bool depthfmt = renderer_format_is_depth(t->format);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (depthfmt) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if (format_supports(vf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageCreateFlags flags = 0;
    if (!depthfmt) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;  // format views (NVN texture views, raw sRGB views)
    if (type == VK_IMAGE_TYPE_2D && layers >= 6 && tw == th && samples == 1) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = vf,
                            .extent = {tw >> b->mip_skip, th >> b->mip_skip, depth}, .mipLevels = levels - b->mip_skip,
                            .arrayLayers = layers,
                            .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = vk_create_image(&ci, &b->image, &b->mem);
    if (r != VK_SUCCESS) {
        LOG("[vk] image creation failed (%d): nvn fmt %#x %ux%ux%u target %u\n", r, t->format, t->width, t->height, t->depth, t->target);
        ci.format = vf = VK_FORMAT_R8G8B8A8_UNORM;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.flags &= ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        VK_CHECK(vk_create_image(&ci, &b->image, &b->mem));
        depthfmt = false;
    }
    b->format = vf;
    b->width = tw; b->height = th; b->depth = depth; b->layers = layers; b->levels = levels; b->samples = samples;
    b->aspect = depthfmt ? aspect_of(t->format) : VK_IMAGE_ASPECT_COLOR_BIT;
    b->base_type = type == VK_IMAGE_TYPE_1D ? (layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                 : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                 : t->target == 8 ? VK_IMAGE_VIEW_TYPE_CUBE : t->target == 9 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                 : layers > 1 || t->target == 4 || t->target == 6 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, b->aspect);
    // new images start zeroed (fresh memory on the Switch reads as whatever; zero is stable and matches the GPU
    // clears the game expects for never-written stencil)
    VkImageSubresourceRange rg = {b->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    if (depthfmt) {
        VkClearDepthStencilValue zero = {0.0f, 0};
        vkCmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
    } else if (!(vf >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && vf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK)) {
        VkClearColorValue zero = {{0, 0, 0, 0}};
        vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
    }
    vk_full_barrier(cb);
}

static uint32_t level_w(const TexBackend *b, uint32_t lv) { uint32_t v = b->width >> lv; return v ? v : 1; }
static uint32_t level_h(const TexBackend *b, uint32_t lv) { uint32_t v = b->height >> lv; return v ? v : 1; }

static void destroy_image(TexBackend *b) {
    if (b->ms) { destroy_image(b->ms); free(b->ms); b->ms = NULL; }
    b->ms_valid = b->ms_dirty = false;
    views_forget(b);
    if (b->image || b->mem) defer((Dead){.img = b->image, .mem = b->mem});
    b->image = VK_NULL_HANDLE;
    b->mem = VK_NULL_HANDLE;
}

// A multisampled image of b's format and size (one level, one layer), in `layout`. NULL: could not be created.
static TexBackend *ms_image(TexBackend *b, VkFormat format, VkImageAspectFlags aspect, uint32_t samples,
                            VkImageUsageFlags usage, VkImageLayout layout) {
    bool depth = aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = depth ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                            .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {b->width, b->height, 1},
                            .mipLevels = 1, .arrayLayers = 1, .samples = (VkSampleCountFlagBits)samples,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    TexBackend *m = calloc(1, sizeof *m);
    VkResult r = vk_create_image(&ci, &m->image, &m->mem);
    if (r != VK_SUCCESS) {
        static int nlog;
        if (nlog++ < 5) LOG("[vk] MSAA image %ux%u x%u failed (%d)\n", b->width, b->height, samples, r);
        free(m);
        return NULL;
    }
    m->format = format;
    m->width = b->width; m->height = b->height; m->depth = m->layers = m->levels = 1; m->samples = samples;
    m->aspect = aspect;
    m->base_type = VK_IMAGE_VIEW_TYPE_2D;
    m->gpu_owned = true;
    VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_ALL,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = layout,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = m->image, .subresourceRange = {m->aspect, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(vk_upload_cmd(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_STAGES_ALL, 0, 0, NULL, 0, NULL, 1, &ib);
    return m;
}

// MSAA (exec.c begin_pass): the multisampled image b's 3D scene passes render into. Only ever a render target (and a
// transfer source for its resolve: exec.c vk_ms_resolve), kept in the attachment layout: on GCN1 a multisampled image
// that shaders may read loses its compression (the scene passes took several times longer). NULL: not created.
TexBackend *vk_tex_ms(TexBackend *b, uint32_t samples) {
    bool depth = b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    TexBackend *m = b->ms;
    if (m && m->samples == samples && m->width == b->width && m->height == b->height && m->format == b->format) return m;
    if (m) { destroy_image(m); free(m); b->ms = NULL; }
    b->ms_valid = b->ms_dirty = false;
    b->ms = ms_image(b, b->format, b->aspect, samples, VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
                     depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    return b->ms;
}

// A shader-readable copy of the multisampled depth image m (exec.c vk_ms_resolve copies it there to resolve it).
TexBackend *vk_tex_ms_readable(TexBackend *m) {
    if (!m->ms) m->ms = ms_image(m, m->format, m->aspect, m->samples, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_IMAGE_LAYOUT_GENERAL);
    return m->ms;
}

static TexBackend *tex_backend(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b) return b;
    vk_collect_dead();
    b = calloc(1, sizeof *b);
    uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
    create_image(t, b);
    if (HITCH_ON) hitch_add(HC_TEX_CREATE, plat_now_ns() - t0, 0);
    t->backend = b;
    return b;
}

void backend_texture_destroyed(NvnTexture *t) {
    vk_lock();
    uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
    TexBackend *b = t->backend;
    if (b) {
        up_forget(b);  // (its first upload's hash, if still to come, goes nowhere)
        destroy_image(b);
        free(b->pending_rgba);
        free(b);
    }
    t->backend = NULL;
    if (HITCH_ON) hitch_add(HC_TEX_DESTROY, plat_now_ns() - t0, 0);
    vk_unlock();
}

// ================================================================== page write tracking
// Generations per guest heap page: each write tracking query (the OS resets its bits) stamps the pages it saw written
// with a new generation; a texture compares against the generation of its last check.
#define PT_PAGE 4096ull
static uint32_t *pt_gen;
static uint32_t pt_cur = 1;
static int pt_state = -1;  // 1 tracking, 0 none

static bool pt_init(void) {
    if (pt_state >= 0) return pt_state;
    uint64_t pg = PT_PAGE, one;
    int n = plat_written_pages((uint64_t)(uintptr_t)G2H(HEAP_BASE), PT_PAGE, &one, 1, &pg);
    pt_state = n >= 0 && pg == PT_PAGE;
    if (!pt_state) { LOG("[vk] no page write tracking (%d): textures are checked by hashing\n", n); return false; }
    pt_gen = calloc(HEAP_MAX / PT_PAGE, sizeof *pt_gen);
    pt_gen[0] = ++pt_cur;
    return true;
}

// Generation of the heap page holding guest address addr (as of the last vk_pages_changed over it); 0: unknown.
uint32_t vk_page_gen(uint64_t addr) {
    if (!pt_gen || addr < HEAP_BASE || addr - HEAP_BASE >= HEAP_MAX) return 0;
    return pt_gen[(addr - HEAP_BASE) / PT_PAGE];
}

int vk_pages_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now) {
    if (!size || addr < HEAP_BASE || addr - HEAP_BASE + size > HEAP_MAX || !pt_init()) return -1;
    uint64_t lo = (addr - HEAP_BASE) & ~(PT_PAGE - 1), hi = (addr - HEAP_BASE + size + PT_PAGE - 1) & ~(PT_PAGE - 1);
    static uint64_t pages[4096];
    uint64_t pg = PT_PAGE, host0 = (uint64_t)(uintptr_t)G2H(HEAP_BASE);
    int n = plat_written_pages(host0 + lo, hi - lo, pages, 4096, &pg);
    if (n < 0) return -1;
    uint32_t g = ++pt_cur;
    if (n >= 4096) for (uint64_t p = lo / PT_PAGE; p < hi / PT_PAGE; p++) pt_gen[p] = g;
    else for (int k = 0; k < n; k++) pt_gen[(pages[k] - host0) / PT_PAGE] = g;
    *now = g;
    for (uint64_t p = lo / PT_PAGE; p < hi / PT_PAGE; p++)
        if (pt_gen[p] > since) return 1;
    return 0;
}

// (a log line) the pages of [addr, addr + size) written after generation `since`, as vk_pages_changed last saw them
static uint32_t pages_written(uint64_t addr, uint64_t size, uint32_t since) {
    if (!pt_gen || !size || addr < HEAP_BASE || addr - HEAP_BASE + size > HEAP_MAX) return 0;
    uint64_t lo = (addr - HEAP_BASE) / PT_PAGE, hi = (addr - HEAP_BASE + size + PT_PAGE - 1) / PT_PAGE;
    uint32_t n = 0;
    for (uint64_t p = lo; p < hi; p++) n += pt_gen[p] > since;
    return n;
}

// ================================================================== uploads (change detected)
// Four independent lanes (dynamic textures are hashed every frame). Values are only compared within a run.
static uint64_t hash_full(const uint8_t *p, uint64_t n) {
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ n, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        h0 = (h0 ^ v[0]) * 0x100000001B3ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ v[1]) * 0x100000001B3ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ v[2]) * 0x100000001B3ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ v[3]) * 0x100000001B3ull; h3 ^= h3 >> 29;
    }
    uint64_t h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull);
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}

static uint64_t fnv_sample(const uint8_t *p, uint64_t n) {
    uint64_t h = 1469598103934665603ull;
    uint64_t step = n > 4096 ? n / 4096 : 1;
    for (uint64_t i = 0; i < n; i += step) { h ^= p[i]; h *= 1099511628211ull; }
    return h ^ n;
}

static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;

static void upload_pending(TexBackend *b) {
    pthread_mutex_lock(&pending_lock);
    void *px = b->pending_rgba;
    uint32_t w = b->pending_w, h = b->pending_h, stride = b->pending_stride;
    b->pending_rgba = NULL;
    pthread_mutex_unlock(&pending_lock);
    if (!px) return;
    b->ms_valid = false;
    if (w > b->width) w = b->width;
    if (h > b->height) h = b->height;
    VkSpan s = vk_transient_copy(px, (uint64_t)stride * h, 16);
    free(px);
    VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = stride / 4,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyBufferToImage(vk_upload_cmd(), s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
}

// ---- an upload's work: bands of guest memory, each hashed and then copied into staging memory
// A texture's layers and levels are cut into bands of about BAND bytes of guest memory: whole rows of GOB blocks of a
// block-linear level (8 << block height rows each), rows of a linear one; a 3D level, or one decoded on the CPU (BCn,
// ASTC), is one band. Whoever runs a band hashes it, then copies it. The texture's hash is its bands' hashes in order,
// so the check (hash only, on the render thread) and a first upload (on the workers) hash it the same way. A band is
// hashed right before it is copied: a write the game makes meanwhile can only make the next check see a change (one
// upload too many), never leave a hash of data that was not copied.
#define BAND (1u << 20)
enum { CV_NONE, CV_RGB8, CV_BCN, CV_ASTC };  // what a band's rows become: themselves, RGBA8 from RGB8, decoded texels
typedef struct UpRec {  // a first upload's band hashes (filled by its jobs), made its texture's at the submission
    TexBackend *b;
    struct UpRec *next;
    uint32_t n;
    uint64_t h[];
} UpRec;
typedef struct {
    UpRec *rec;          // the band's hash goes to rec->h[band] (NULL: copied only)
    const uint8_t *src;  // guest memory: rows of blocks, block-linear (stride 0) or linear (stride bytes apart)
    uint8_t *dst;        // staging memory (NULL: hashed only)
    uint64_t len;        // the guest bytes hashed
    uint32_t band, wb, hb, d, bpb, bh, bd, stride, w, h;  // (w, h: a decoded level's texels)
    uint8_t conv, bcn, tb, abw, abh, srgb;
} UpJob;

static uint64_t band_mix(uint64_t acc, uint64_t h) {
    acc = (acc ^ h) * 0x9E3779B97F4A7C15ull;
    return acc ^ (acc >> 31);
}

static uint64_t rec_hash(const UpRec *r) {
    uint64_t acc = 0x27D4EB2F165667C5ull;
    for (uint32_t i = 0; i < r->n; i++) acc = band_mix(acc, r->h[i]);
    return band_mix(acc, r->n);
}

static void run_job(const UpJob *j) {
    if (j->rec) j->rec->h[j->band] = hash_full(j->src, j->len);
    if (!j->dst) return;
    uint64_t row = (uint64_t)j->wb * j->bpb, n = (uint64_t)j->wb * j->hb * j->d;
    uint8_t *lin = j->conv == CV_NONE ? j->dst : malloc(row * j->hb * j->d + 1);
    if (!lin) return;  // (out of memory: the level keeps what it had)
    if (j->stride) for (uint32_t y = 0; y < j->hb; y++) memcpy(lin + y * row, j->src + (uint64_t)y * j->stride, row);
    else tegra_deswizzle(j->src, lin, j->wb, j->hb, j->d, j->bpb, j->bh, j->bd);
    if (j->conv == CV_RGB8) {
        uint8_t *o = j->dst;
        for (uint64_t i = 0; i < n; i++) { o[i * 4] = lin[i * 3]; o[i * 4 + 1] = lin[i * 3 + 1]; o[i * 4 + 2] = lin[i * 3 + 2]; o[i * 4 + 3] = 255; }
    } else if (j->conv == CV_BCN) {
        bcn_decode(j->bcn, lin, j->w, j->h, j->dst, j->tb);
    } else if (j->conv == CV_ASTC) {
        astc_decode_ldr(lin, j->w, j->h, j->abw, j->abh, j->srgb, j->dst);
    }
    if (lin != j->dst) free(lin);
}

// ---- the upload workers
// A texture's first upload used to be done whole on the render thread, inside the command list, before its draw was
// recorded: the hash, then every level deswizzled into staging memory. A new monster or area brings 20 to 30 MB of them
// in one frame (measured 7 to 19 ms of it; 45 ms with a new staging chunk). Now the render thread records the copies
// and queues the bands here, and the workers fill the staging memory while it goes on with the command list. The
// submission waits for them before it goes to the GPU (vk_upload_jobs_finish, where the render thread runs whatever no
// worker has taken yet). The copies still run on the GPU ahead of their submission's draws, with the same data, so
// nothing is drawn early or half uploaded. The same data: a write the CPU makes later in the same list (ClearBuffer,
// CopyBufferToBuffer, UpdateUniformBuffer) over memory a queued band has yet to read waits for the bands first
// (vk_upload_jobs_guard), so the draw before it sees the old contents. A texture under UP_ASYNC is done on the spot, as
// before: queuing it costs more than it saves. So is one decoded on the CPU (a phone's BCn, ASTC): bcn.c already spreads
// a big one over threads of its own, and three decodes at once would hold three times its buffers (a phone's memory).
// MHGU_TEX_ASYNC=0: everything on the spot (for comparisons).
#define UP_WORKERS 3         // (besides the render thread, as bcn.c's)
#define UP_ASYNC (256u << 10)
#define UP_BAND_MIN (64u << 10)  // (a big texture's small levels too: done here, in less time than queuing them takes)
#define UP_RANGES 128
typedef struct { uintptr_t lo, hi; } UpRange;  // (host addresses of guest memory)
static struct {
    pthread_mutex_t mu;
    pthread_cond_t go, idle;
    UpJob *q;
    int n, cap, next, busy, started;  // q[next .. n): not taken yet; busy: being run
    UpRec *recs;                      // (vk_lock) first uploads waiting for their hashes
    UpRange rg[UP_RANGES], all;       // (vk_lock) what the queued bands read: a texture's each, and the span of them all
    int nr;
} U = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER};

static void *up_worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&U.mu);
    for (;;) {
        while (U.next >= U.n) pthread_cond_wait(&U.go, &U.mu);
        UpJob j = U.q[U.next++];
        U.busy++;
        pthread_mutex_unlock(&U.mu);
        uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
        run_job(&j);
        if (HITCH_ON) hitch_add(HC_TEX_JOB, plat_now_ns() - t0, j.len);  // ((debug) MHGU_HITCHLOG: counted here, on O)
        pthread_mutex_lock(&U.mu);
        if (!--U.busy && U.next >= U.n) pthread_cond_broadcast(&U.idle);
    }
    return NULL;
}

static bool up_workers(void) {
    if (U.started) return U.started > 0;
    const char *e = getenv("MHGU_TEX_ASYNC");
#ifdef _WIN32
    long cores = SDL_GetNumLogicalCPUCores();
#else
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    int n = e && !atoi(e) ? 0 : cores > UP_WORKERS + 1 ? UP_WORKERS : cores > 1 ? (int)cores - 1 : 0;
    U.started = -1;
    for (int i = 0; i < n; i++) {
        pthread_t th;
        if (pthread_create(&th, NULL, up_worker, NULL)) break;
        pthread_detach(th);
        U.started = i + 1;
    }
    LOG("[vk] texture uploads: %d worker thread%s\n", U.started > 0 ? U.started : 0, U.started == 1 ? "" : "s");
    return U.started > 0;
}

static void up_push(const UpJob *j) {
    pthread_mutex_lock(&U.mu);
    if (U.n == U.cap) {
        int cap = U.cap ? U.cap * 2 : 256;
        UpJob *q = realloc(U.q, (size_t)cap * sizeof *q);
        if (!q) { pthread_mutex_unlock(&U.mu); run_job(j); return; }
        U.q = q, U.cap = cap;
    }
    U.q[U.n++] = *j;
    pthread_cond_signal(&U.go);
    pthread_mutex_unlock(&U.mu);
}

// (vk_lock) [lo, hi): what a texture's queued bands read, kept until they are done. One touching the last is merged
// into it (a pool's textures side by side); with no room left the last one grows to cover it (more waits, none missed).
static void up_range(uintptr_t lo, uintptr_t hi) {
    if (!U.nr || lo < U.all.lo) U.all.lo = lo;
    if (!U.nr || hi > U.all.hi) U.all.hi = hi;
    UpRange *r = U.nr ? &U.rg[U.nr - 1] : NULL;
    if (r && (U.nr == UP_RANGES || (lo <= r->hi && hi >= r->lo))) {
        if (lo < r->lo) r->lo = lo;
        if (hi > r->hi) r->hi = hi;
        return;
    }
    U.rg[U.nr++] = (UpRange){lo, hi};
}

// (exec.c vk_submit_extra before vkQueueSubmit, and vk_upload_jobs_guard; vk_lock held) Every queued band done: its
// staging memory written for this submission's copies. The bands nobody has taken yet are run here; then the first
// uploads get their hashes.
void vk_upload_jobs_finish(void) {
    U.nr = 0;
    if (!U.n && !U.recs) return;
    pthread_mutex_lock(&U.mu);
    while (U.next < U.n) {
        UpJob j = U.q[U.next++];
        U.busy++;
        pthread_mutex_unlock(&U.mu);
        uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
        run_job(&j);
        if (HITCH_ON) hitch_add(HC_TEX_JOB, plat_now_ns() - t0, j.len);
        pthread_mutex_lock(&U.mu);
        U.busy--;
    }
    if (U.busy) {
        uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
        while (U.busy) pthread_cond_wait(&U.idle, &U.mu);
        if (HITCH_ON) hitch_add(HC_TEX_WAIT, plat_now_ns() - t0, 0);  // ((debug) MHGU_HITCHLOG: the workers' last bands)
    }
    U.n = U.next = 0;
    pthread_mutex_unlock(&U.mu);
    for (UpRec *r = U.recs, *nx; r; r = nx) {
        nx = r->next;
        if (r->b) r->b->data_hash = rec_hash(r);
        free(r);
    }
    U.recs = NULL;
}

// (exec.c exec_list; vk_lock held) The CPU is about to write guest memory [addr, addr + size) in the middle of a command
// list: ClearBuffer, CopyBufferToBuffer, UpdateUniformBuffer, a counter's report, done at once, in order with the list.
// A draw earlier in the list must see what was there before, as on the GPU: if a queued band has yet to read any of it,
// every band is done first. A write that meets none (the usual case) costs a comparison or a few.
void vk_upload_jobs_guard(uint64_t addr, uint64_t size, const char *what) {
    if (!U.nr || !size) return;
    uintptr_t lo = (uintptr_t)G2H((uint32_t)addr), hi = lo + size;
    if (hi <= U.all.lo || lo >= U.all.hi) return;
    int i = 0;
    while (i < U.nr && (hi <= U.rg[i].lo || lo >= U.rg[i].hi)) i++;
    if (i == U.nr) return;
    static int nlog;
    if (nlog++ < 8)
        LOG("[tex] %s writes %llx (%llu bytes) in a command list, over a texture queued for upload earlier in it: the "
            "queued bands done first\n", what, (unsigned long long)addr, (unsigned long long)size);
    uint64_t t0 = HITCH_ON ? plat_now_ns() : 0;
    vk_upload_jobs_finish();
    if (HITCH_ON) hitch_add_label(HC_TEX_GUARD, plat_now_ns() - t0, 0, what, -1);  // ((debug) MHGU_HITCHLOG)
}

// (vk_lock) b is going away before its first upload's bands are done: they go on (they only write staging memory and
// their record), their hash goes nowhere
static void up_forget(TexBackend *b) {
    for (UpRec *r = U.recs; r; r = r->next)
        if (r->b == b) r->b = NULL;
}

// The guest bytes t's upload reads. A linear texture's are its rows at its stride: tegra_total_size is the block-linear
// size, rows rounded up to whole GOB blocks, which for a linear texture of another height reaches past its storage
// (nvnTextureBuilderGetStorageSize: stride x height) into whatever the game keeps after it.
static uint64_t upload_extent(const NvnTexture *t) {
    const FormatInfo *fi = t->layout.fmt;
    if (!(t->flags & 0x10) || !t->stride || t->layout.levels > 1 || t->layout.layers > 1 || t->layout.depth > 1)
        return tegra_total_size(&t->layout);
    uint64_t hb = (t->layout.height + fi->bh - 1) / fi->bh, row = (uint64_t)(t->layout.width + fi->bw - 1) / fi->bw * fi->bpb;
    return (hb ? hb - 1 : 0) * t->stride + row;
}

// t's upload, band by band, always in the same order. P_COUNT: how many bands. P_HASH: its hash, here. P_UPLOAD:
// staging memory, the copies recorded, and the bands run here or queued (async), hashed as well when rec.
enum { P_COUNT, P_HASH, P_UPLOAD };
static uint64_t plan(NvnTexture *t, TexBackend *b, const uint8_t *mem, int mode, UpRec *rec, bool async) {
    const FormatInfo *fi = t->layout.fmt;
    VkFmt mf = fmt_of(t->format);
    bool fmt_ok = b->format == mf.vf || b->astc_bw || b->bcn;  // (else: an unsupported format, nothing sensible to copy)
    bool linear = (t->flags & 0x10) != 0;
    uint32_t stride = linear ? (uint32_t)t->stride : 0;
    uint32_t layers = t->layout.layers ? t->layout.layers : 1;
    if (layers > b->layers && b->depth == 1) layers = b->layers;
    uint64_t layer_size = tegra_layer_size(&t->layout);
    uint32_t levels = t->layout.levels < b->levels ? t->layout.levels : b->levels;
    VkCommandBuffer cb = mode == P_UPLOAD && fmt_ok ? vk_upload_cmd() : VK_NULL_HANDLE;
    uint64_t acc = 0x27D4EB2F165667C5ull;
    uint32_t nb = 0;
    uintptr_t qlo = 0, qhi = 0;  // (what the queued bands read, for vk_upload_jobs_guard)
    for (uint32_t layer = 0; layer < layers; layer++) {
        for (uint32_t lv = b->mip_skip; lv < levels; lv++) {
            LevelLayout l = tegra_level(&t->layout, lv);
            const uint8_t *src = mem + layer * layer_size + l.offset;
            UpJob j = {.wb = l.wb, .hb = l.hb, .d = l.d, .bpb = fi->bpb, .bh = l.block_height_log2, .bd = l.block_depth_log2,
                       .stride = stride, .w = l.w, .h = l.h};
            uint32_t gr = 8u << l.block_height_log2, gd = 1u << l.block_depth_log2;
            uint64_t brow = (uint64_t)((l.wb * fi->bpb + 63) / 64) * (512ull << (l.block_height_log2 + l.block_depth_log2));
            uint64_t level_bytes = brow * ((l.hb + gr - 1) / gr) * ((l.d + gd - 1) / gd);
            uint32_t step = l.hb;  // rows a band takes
            VkSpan s = {0};
            uint64_t out_row = 0;
            VkBufferImageCopy rg = {0};
            if (b->bcn || b->astc_bw) {  // blocks -> rows of blocks -> texels (bcn.c, astc.c): the level in one band
                j.conv = b->bcn ? CV_BCN : CV_ASTC;
                j.bpb = b->bcn ? bcn_block_bytes(b->bcn) : 16, j.d = 1;
                j.bcn = b->bcn, j.tb = b->bcn_tb, j.abw = b->astc_bw, j.abh = b->astc_bh, j.srgb = b->astc_srgb;
                if (cb) {
                    s = vk_transient((uint64_t)l.w * l.h * (b->bcn ? b->bcn_tb : 4), 64);
                    rg = (VkBufferImageCopy){.bufferOffset = s.off, .bufferRowLength = l.w, .bufferImageHeight = l.h,
                                             .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, b->bcn ? lv - b->mip_skip : lv, layer, 1},
                                             .imageExtent = {l.w < level_w(b, lv) ? l.w : level_w(b, lv),
                                                             l.h < level_h(b, lv) ? l.h : level_h(b, lv), 1}};
                }
            } else {
                bool expand = mf.expand_rgb && fi->bpb == 3;  // RGB8 -> RGBA8
                j.conv = expand ? CV_RGB8 : CV_NONE;
                out_row = (uint64_t)l.wb * (expand ? 4 : fi->bpb);
                if (linear) step = stride ? (BAND / stride ? BAND / stride : 1) : l.hb;
                else if (l.d == 1) step = (uint32_t)(BAND / brow ? BAND / brow : 1) * gr;
                if (cb && b->aspect != (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {  // (packed D/S: no CPU layout)
                    s = vk_transient(out_row * l.hb * l.d, 64);
                    uint32_t w = l.w < level_w(b, lv) ? l.w : level_w(b, lv), hh = l.h < level_h(b, lv) ? l.h : level_h(b, lv);
                    rg = (VkBufferImageCopy){
                        .bufferOffset = s.off, .bufferRowLength = l.wb * fi->bw, .bufferImageHeight = l.hb * fi->bh,
                        .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect, lv,
                                             b->depth > 1 ? 0 : layer, 1},
                        .imageExtent = {w, b->base_type == VK_IMAGE_VIEW_TYPE_1D || b->base_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? 1 : hh,
                                        b->depth > 1 ? l.d : 1}};
                }
            }
            for (uint32_t y0 = 0; y0 < l.hb; y0 += step, nb++) {
                if (mode == P_COUNT) continue;
                uint32_t rows = l.hb - y0 < step ? l.hb - y0 : step;
                j.hb = rows;
                if (linear) j.src = src + (uint64_t)y0 * stride, j.len = (uint64_t)(rows - 1) * stride + (uint64_t)l.wb * j.bpb;
                else j.src = src + (uint64_t)(y0 / gr) * brow, j.len = step == l.hb ? level_bytes : (rows + gr - 1) / gr * brow;
                if (mode == P_HASH) { acc = band_mix(acc, hash_full(j.src, j.len)); continue; }
                j.rec = rec, j.band = nb;
                j.dst = s.ptr ? (uint8_t *)s.ptr + y0 * out_row : NULL;
                if (!j.rec && !j.dst) continue;
                if (async && j.len >= UP_BAND_MIN) {
                    up_push(&j);
                    uintptr_t a = (uintptr_t)j.src;
                    if (!qhi || a < qlo) qlo = a;
                    if (a + j.len > qhi) qhi = a + j.len;
                } else run_job(&j);
            }
            if (s.ptr) vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        }
    }
    if (qhi) up_range(qlo, qhi);
    return mode == P_COUNT ? nb : mode == P_HASH ? band_mix(acc, nb) : 0;
}

static uint64_t full_hash_budget, full_hash_budget_frame;
static uint64_t hl_hashed, hl_copied;  // ((debug) MHGU_HITCHLOG) what upload_texture just did: bytes hashed, uploaded

// A texture whose contents change at every check, frame after frame, is uploaded whole each time: a line once (for the
// first few such), saying what it is and who writes it.
static void say_every_frame(NvnTexture *t, TexBackend *b, uint64_t total, uint32_t since) {
    static int nsaid;
    if (b->up_said || nsaid >= 8) return;
    b->up_said = true, nsaid++;
    uint64_t tegra = tegra_total_size(&t->layout);
    LOG("[tex] changed and uploaded on each of %u frames in a row: fmt %#x %ux%ux%u, %u levels, %u layers, target %u, flags "
        "%#x (%s, stride %llu), %llu bytes checked (block-linear size %llu), %u of its %llu pages written since the last "
        "check, nvnTextureWriteTexels %u times so far, storage %llx, label '%s'\n",
        b->up_run, t->format, t->width, t->height, t->depth, t->levels, t->layout.layers, t->target, t->flags,
        t->flags & 0x10 ? "linear" : "block-linear", (unsigned long long)t->stride, (unsigned long long)total,
        (unsigned long long)tegra, pages_written(t->addr, total, since),
        (unsigned long long)((t->addr + total + PT_PAGE - 1) / PT_PAGE - t->addr / PT_PAGE),
        t->texel_writes, (unsigned long long)t->addr, t->label);
}

static void upload_texture(NvnTexture *t, TexBackend *b) {
    if (!t->addr || t->target == 10) return;
    uint64_t frame = vk_frame_counter();
    if (b->uploaded && b->checked_frame == frame + 1) return;
    if (b->uploaded && !b->dynamic && ((frame + ((uintptr_t)b >> 6)) & 7)) return;  // static ones: every 8th frame
    const FormatInfo *fi = t->layout.fmt;
    if (!fi || !fi->bpb) return;
    uint64_t total = upload_extent(t);
    b->checked_frame = frame + 1;
    if (t->flags & 0x10) b->dynamic = true;
    if (full_hash_budget_frame != frame + 1) { full_hash_budget_frame = frame + 1; full_hash_budget = 4 << 20; }
    const uint8_t *mem = (const uint8_t *)G2H((uint32_t)t->addr);
    bool full = b->dynamic || !b->uploaded;
    bool periodic = frame - b->full_frame >= 120 && full_hash_budget >= total;  // (safety net: a full hash now and then)
    if (!full && periodic) { full = true; full_hash_budget -= total; }
    // page write tracking: texture memory the CPU hasn't written since the last check is not hashed at all
    uint32_t now = 0, since = b->wgen;
    int ch = vk_pages_changed(t->addr, total, b->wgen, &now);
    if (ch >= 0) {
        b->wgen = now;
        if (!ch && b->uploaded && !periodic) return;
    }
    if (!full) {
        uint64_t sh = fnv_sample(mem, total);
        if (sh == b->sample_hash) return;
    }
    b->full_frame = frame;
    b->sample_hash = fnv_sample(mem, total);
    bool fmt_ok = b->format == fmt_of(t->format).vf || b->astc_bw || b->bcn;
    bool async = fmt_ok && !b->bcn && !b->astc_bw && total >= UP_ASYNC && up_workers();
    if (b->uploaded) {  // a check: hashed here, and if it changed, copied (on the workers when big)
        uint64_t h = plan(t, b, mem, P_HASH, NULL, false);
        hl_hashed = total;
        if (h == b->data_hash) { b->up_run = 0; return; }
        b->dynamic = true;
        b->data_hash = h;
        if (!fmt_ok) return;
        hl_copied = total;
        plan(t, b, mem, P_UPLOAD, NULL, async);
        b->up_run = b->up_last + 1 == frame ? b->up_run + 1 : 1;
        b->up_last = frame;
        if (b->up_run >= 120) say_every_frame(t, b, total, since);
        return;
    }
    // the first upload: each band hashed as it is copied, the hash the texture's once they are all done
    b->uploaded = true;
    if (fmt_ok) hl_copied = total;
    else hl_hashed = total;
    if (total != tegra_total_size(&t->layout)) {
        static int nlog;
        if (nlog++ < 8)
            LOG("[tex] linear texture fmt %#x %ux%u, stride %llu: checked over its %llu bytes of rows (before: %llu, its "
                "block-linear size)\n", t->format, t->width, t->height, (unsigned long long)t->stride, (unsigned long long)total,
                (unsigned long long)tegra_total_size(&t->layout));
    }
    uint32_t n = (uint32_t)plan(t, b, mem, P_COUNT, NULL, false);
    UpRec *rec = malloc(sizeof *rec + (n ? n : 1) * sizeof *rec->h);
    if (!rec) { b->data_hash = plan(t, b, mem, P_HASH, NULL, false); if (fmt_ok) plan(t, b, mem, P_UPLOAD, NULL, false); return; }
    rec->b = b, rec->n = n, rec->next = NULL;
    plan(t, b, mem, P_UPLOAD, rec, async);
    if (async) {
        rec->next = U.recs, U.recs = rec;
    } else {
        b->data_hash = rec_hash(rec);
        free(rec);
    }
}

// ((debug) MHGU_HITCHLOG) the hitch line's word for the texture just uploaded: format, size, levels, linear
static const char *hl_label(const NvnTexture *t) {
    static char s[40];
    snprintf(s, sizeof s, "fmt %#x %ux%u%s%s", t->format, t->width, t->height, t->levels > 1 ? " mips" : "",
             t->flags & 0x10 ? " linear" : "");
    return s;
}

TexBackend *vk_tex(NvnTexture *t, bool for_render) {
    TexBackend *b = tex_backend(t);
    if (b->pending_rgba) upload_pending(b);
    if (for_render) b->gpu_owned = true;
    else if (!b->gpu_owned) {
        extern uint64_t stat_upload_ns;
        uint64_t t0 = plat_now_ns();
        upload_texture(t, b);
        stat_upload_ns += plat_now_ns() - t0;
        if (HITCH_ON && (hl_hashed || hl_copied)) {  // ((debug) MHGU_HITCHLOG: an upload, or a whole hash; the KB of one)
            if (hl_copied) hitch_add_label(HC_TEX_UPLOAD, plat_now_ns() - t0, hl_copied, hl_label(t), (int)(hl_copied >> 10));
            else hitch_add(HC_TEX_HASH, plat_now_ns() - t0, hl_hashed);
            hl_hashed = hl_copied = 0;
        }
    }
    return b;
}

// (exec.c begin_pass, outside a render pass, before t is rendered to) An RGBA8 2D target re-made wide or back to
// RGBA8 (SmoothGradients turned off, or it turns out to be FSR's input), its contents blitted over.
void vk_rt_widen(NvnTexture *t, bool on) {
    if (t->format != 0x25 || t->target != 1) return;
    TexBackend *b = tex_backend(t);
    if (b->wide == on || b->format != (b->wide ? VK_FORMAT_R16G16B16A16_UNORM : VK_FORMAT_R8G8B8A8_UNORM)) return;
    if (b->pending_rgba) upload_pending(b);
    vk_ms_resolve(b);
    if (b->ms) { destroy_image(b->ms); free(b->ms); b->ms = NULL; }
    b->ms_valid = b->ms_dirty = false;
    views_forget(b);
    TexBackend old = *b;
    b->wide = on;
    create_image(t, b);
    b->wide = b->format == VK_FORMAT_R16G16B16A16_UNORM;  // (creation failed: RGBA8 after all)
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageBlit r[16];
    uint32_t n = b->levels < 16 ? b->levels : 16;
    for (uint32_t lv = 0; lv < n; lv++) {
        int32_t w = (int32_t)level_w(b, lv), h = (int32_t)level_h(b, lv);
        r[lv] = (VkImageBlit){.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, b->layers}, .srcOffsets = {{0, 0, 0}, {w, h, 1}},
                              .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, b->layers}, .dstOffsets = {{0, 0, 0}, {w, h, 1}}};
    }
    vkCmdBlitImage(cb, old.image, VK_IMAGE_LAYOUT_GENERAL, b->image, VK_IMAGE_LAYOUT_GENERAL, n, r, VK_FILTER_NEAREST);
    vk_full_barrier(cb);
    defer((Dead){.img = old.image, .mem = old.mem});
    static int nlog;
    if (nlog++ < 40) LOG("[vk] render target %ux%u (%u levels) %s\n", b->width, b->height, b->levels, b->wide ? "wide" : "RGBA8");
}

void renderer_mark_gpu_owned(NvnTexture *t) { tex_backend(t)->gpu_owned = true; }
void renderer_mark_dynamic(NvnTexture *t) { tex_backend(t)->dynamic = true; }
void backend_pool_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

// Host-produced texel data (decoded video, any thread): applied on the render thread at the texture's next use.
void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride) {
    TexBackend *b = t->backend;
    if (!b) return;
    void *copy = malloc((size_t)stride * h);
    memcpy(copy, rgba, (size_t)stride * h);
    pthread_mutex_lock(&pending_lock);
    free(b->pending_rgba);
    b->pending_rgba = copy;
    b->pending_w = w; b->pending_h = h; b->pending_stride = stride;
    b->gpu_owned = true;
    pthread_mutex_unlock(&pending_lock);
}

// ================================================================== views
#define VIEW_TAB 16384
typedef struct { TexBackend *b; uint64_t k1, k2; VkImageView view; } ViewEnt;
static ViewEnt view_tab[VIEW_TAB];

// Views may still be referenced by submitted or pending work: destroyed once that has completed.
void vk_views_forget(TexBackend *b) { views_forget(b); }
static void views_forget(TexBackend *b) {
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].b == b) { vk_defer_view(view_tab[i].view); view_tab[i].b = NULL; view_tab[i].view = VK_NULL_HANDLE; }
}

VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect) {
    uint64_t k1 = (uint64_t)type | (uint64_t)fmt << 8 | (uint64_t)lv0 << 40 | (uint64_t)nlv << 48 | (uint64_t)aspect << 56;
    uint64_t k2 = (uint64_t)ly0 | (uint64_t)nly << 16 | (uint64_t)sw.r << 32 | (uint64_t)sw.g << 40 | (uint64_t)sw.b << 48 | (uint64_t)sw.a << 56;
    uint64_t h = ((uint64_t)(uintptr_t)b * 0x9E3779B97F4A7C15ull) ^ (k1 * 0xC2B2AE3D27D4EB4Full) ^ (k2 * 0x165667B19E3779F9ull);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VIEW_TAB - 1);
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VIEW_TAB - 1)) {
        ViewEnt *e = &view_tab[slot];
        if (e->b == b && e->k1 == k1 && e->k2 == k2) return e->view;
        if (!e->b) break;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = b->image, .viewType = type,
                                .format = fmt, .components = sw, .subresourceRange = {aspect, lv0, nlv, ly0, nly}};
    VkImageView v;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &v) != VK_SUCCESS) {
        static int nfail;
        if (nfail++ < 20) LOG("[vk] view failed: type %d fmt %d (image fmt %d) levels %u+%u layers %u+%u\n", type, fmt, b->format, lv0, nlv, ly0, nly);
        return VK_NULL_HANDLE;
    }
    ViewEnt *e = &view_tab[slot];
    if (e->b) vk_defer_view(e->view);  // evict (16 probes taken)
    *e = (ViewEnt){b, k1, k2, v};
    return v;
}

static VkImageViewType view_type_for(int textype) {
    switch (textype) {
    case TEXTYPE_1D: return VK_IMAGE_VIEW_TYPE_1D;
    case TEXTYPE_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case TEXTYPE_CUBE: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TEXTYPE_2D_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TEXTYPE_CUBE_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case TEXTYPE_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

static const VkComponentMapping IDENTITY = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

// render: attachment view (one level, one layer). Otherwise a sampled view of type want_textype (TEXTYPE_*),
// with the NVN component swizzle, converting between compatible types.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype) {
    TexBackend *b = vk_tex(t, render);
    if (!has_view) v = NULL;
    VkFormat fmt = b->format;
    if (v && (v->setmask & 4) && !(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        VkFormat vf = fmt_of(v->format).vf;
        if (vk_format(t->format) == b->format) fmt = vf;  // (fallback images keep their own format)
        else if (b->wide && vf != VK_FORMAT_R8G8B8A8_UNORM) {
            static int nlog;
            if (nlog++ < 10) LOG("[vk] wide render target %ux%u read as nvn fmt %#x: as RGBA16 instead\n", b->width, b->height, v->format);
        }
    }
    uint32_t lv0 = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t nlv = v && (v->setmask & 1) && v->num_levels ? v->num_levels : b->levels - (lv0 < b->levels ? lv0 : 0);
    uint32_t ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
    uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers : b->layers - (ly0 < b->layers ? ly0 : 0);
    if (lv0 >= b->levels) lv0 = b->levels - 1;
    if (lv0 + nlv > b->levels) nlv = b->levels - lv0;
    if (ly0 >= b->layers) ly0 = b->layers - 1;
    if (ly0 + nly > b->layers) nly = b->layers - ly0;
    if (b->mip_skip) {  // (the image starts at the game's level mip_skip)
        uint32_t end = lv0 + nlv > (uint32_t)b->mip_skip + 1 ? lv0 + nlv : (uint32_t)b->mip_skip + 1;
        lv0 = lv0 > b->mip_skip ? lv0 - b->mip_skip : 0;
        nlv = end - b->mip_skip - lv0;
    }
    if (render) {
        if (b->base_type == VK_IMAGE_VIEW_TYPE_3D) return VK_NULL_HANDLE;  // (3D render targets: not supported)
        return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, fmt, lv0, 1, ly0, 1, IDENTITY, b->aspect);
    }
    VkImageAspectFlags aspect = b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect;
    VkComponentMapping sw = IDENTITY;
    if (!(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) && b->samples == 1) {
        const uint8_t *s = v && (v->setmask & 8) ? v->swizzle : t->swizzle;
        uint8_t c[4];
        for (int i = 0; i < 4; i++) c[i] = s[i] <= 5 ? s[i] : (uint8_t)(2 + i);
        if (c[0] || c[1] || c[2] || c[3]) {  // never set: identity
            if (fmt_of(t->format).alpha_one)
                for (int i = 0; i < 4; i++) if (c[i] == 5) c[i] = 1;
            // NVN swizzle 0 zero, 1 one, 2..5 r,g,b,a == VkComponentSwizzle - 1
            sw = (VkComponentMapping){(VkComponentSwizzle)(c[0] + 1), (VkComponentSwizzle)(c[1] + 1), (VkComponentSwizzle)(c[2] + 1),
                                      (VkComponentSwizzle)(c[3] + 1)};
        } else if (fmt_of(t->format).alpha_one) {
            sw.a = VK_COMPONENT_SWIZZLE_ONE;
        }
    }
    VkImageViewType want = view_type_for(want_textype);
    VkImageViewType have = b->base_type;
    if (b->samples > 1) return vk_dummy_view(want_textype);  // multisampled images can't be sampled as 2D
    if (want == have) {
        if (have == VK_IMAGE_VIEW_TYPE_CUBE) nly = 6;
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, nly, sw, aspect);
    }
    if (want == VK_IMAGE_VIEW_TYPE_2D && (have == VK_IMAGE_VIEW_TYPE_2D_ARRAY || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_2D_ARRAY && (have == VK_IMAGE_VIEW_TYPE_2D || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, have == VK_IMAGE_VIEW_TYPE_CUBE ? nly : 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_CUBE && have == VK_IMAGE_VIEW_TYPE_2D_ARRAY && b->layers >= 6 && b->width == b->height)
        return vk_image_view(b, want, fmt, lv0, nlv, ly0 + 6 <= b->layers ? ly0 : 0, 6, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_1D_ARRAY && have == VK_IMAGE_VIEW_TYPE_1D)
        return vk_image_view(b, want, fmt, lv0, nlv, 0, 1, sw, aspect);
    static int nlog;
    if (nlog++ < 30) LOG("[tex] type mismatch: want %d have %d (%ux%ux%u target %u fmt %#x layers %u)\n", want, have, t->width,
                         t->height, t->depth, t->target, t->format, b->layers);
    return vk_dummy_view(want_textype);
}

// 1x1 black textures for unbound slots, per type
VkImageView vk_dummy_view(int textype) {
    static TexBackend dummies[10];
    static VkImageView views[10];
    if (textype < 0 || textype >= 10) textype = TEXTYPE_2D;
    if (views[textype]) return views[textype];
    TexBackend *b = &dummies[textype];
    VkImageViewType vt = view_type_for(textype);
    bool cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                                       : vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = cube ? 6 : 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VK_CHECK(vk_create_image(&ci, &b->image, &b->mem));
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1};
    vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    b->format = VK_FORMAT_R8G8B8A8_UNORM;
    b->levels = 1;
    b->layers = cube ? 6 : 1;
    views[textype] = vk_image_view(b, vt, b->format, 0, 1, 0, cube ? 6 : 1, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
    return views[textype];
}

// ================================================================== samplers
static VkCompareOp compare_op(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

VkSampler vk_sampler(NvnSamplerDesc *d) {
    if (d->backend) return (VkSampler)d->backend;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d->mag_filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    uint32_t mf = d->min_filter;
    si.minFilter = (mf == 1 || mf == 3 || mf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mf == 4 || mf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[3];
    bool border = false;
    for (int i = 0; i < 3; i++) {
        switch (d->wrap[i]) {
        case 0: case 7: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case 1: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        case 2: case 3:
            modes[i] = g_vk.mirror_clamp ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            break;
        case 4: case 5: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; border = true; break;
        case 6: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }
    si.addressModeU = modes[0]; si.addressModeV = modes[1]; si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (mf <= 1) si.maxLod = si.minLod + 0.25f;  // not mipmapped: stay on the base level
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    si.mipLodBias = d->lod_bias;
    if (mf > 1) si.mipLodBias += g_fsr_lod_bias;  // (FSR: textures as sharp as at the output size; config.c)
    const char *force = getenv("MHGU_TEX_LOD");  // (debug) mipmapped samplers: only this level (shows mip contents)
    if (force && mf > 1) { si.minLod = si.maxLod = (float)atof(force); si.mipLodBias = 0; }
    // (Low-end mode: no anisotropic filtering, and one texture fetch for each: a Mali's trilinear takes two. A desktop's
    // keeps both, without which the mip levels' steps show on every floor; Toaster leaves out the anisotropic only)
#ifdef __ANDROID__
    bool no_aniso = g_config.low_end != 0, nearest_mips = g_config.low_end != 0;
#else
    bool no_aniso = g_config.low_end == LOW_END_TOASTER, nearest_mips = false;
#endif
    if (d->max_aniso > 1 && !no_aniso) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = fminf(d->max_aniso, 16); }
    if (nearest_mips && mf > 1) si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (d->compare_mode) { si.compareEnable = VK_TRUE; si.compareOp = compare_op(d->compare_func); }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (border) {  // the standard colours; others need VK_EXT_custom_border_color (else the nearest standard one)
        const float *c = d->border;
        if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (g_vk.custom_border) {
            memcpy(cb.customBorderColor.float32, c, 16);
            si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            si.pNext = &cb;
        } else {
            float l = (c[0] + c[1] + c[2]) / 3;
            si.borderColor = c[3] < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                           : l < 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        }
    }
    VkSampler s;
    VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s));
    if (getenv("MHGU_SAMPLER_LOG"))
        LOG("[sampler] min %u mag %u wrap %u,%u,%u lod %g..%g bias %g aniso %g compare %u\n", mf, d->mag_filter, d->wrap[0],
            d->wrap[1], d->wrap[2], d->lod_min, d->lod_max, d->lod_bias, d->max_aniso, d->compare_mode);
    d->backend = (void *)s;
    return s;
}

VkSampler vk_default_sampler(bool linear) {
    static VkSampler s[2];
    if (!s[linear]) {
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                  .magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
        VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s[linear]));
    }
    return s[linear];
}
