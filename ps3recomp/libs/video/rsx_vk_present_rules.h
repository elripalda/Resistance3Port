/*
 * ps3recomp - RPCS3's Vulkan swapchain and raster selection rules as pure
 * functions (vkutils/swapchain.cpp swapchain_WSI::init, VKGSRender.cpp
 * get_cull_face). Plain integers carrying the Vulkan enum values, so the
 * unit tests build without Vulkan headers (tests/test_vk_present_rules.c).
 */
#ifndef RSX_VK_PRESENT_RULES_H
#define RSX_VK_PRESENT_RULES_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* VkPresentModeKHR values. */
enum {
    RSX_VK_PM_IMMEDIATE    = 0,
    RSX_VK_PM_MAILBOX      = 1,
    RSX_VK_PM_FIFO         = 2,
    RSX_VK_PM_FIFO_RELAXED = 3
};

/* RSX_VSYNC as r2rsx_bridge.cpp maps it onto RPCS3's vsync_mode: "adaptive"
 * is adaptive, a positive number is full vsync, anything else is off. */
enum { RSX_VK_VSYNC_OFF = 0, RSX_VK_VSYNC_FULL = 1, RSX_VK_VSYNC_ADAPTIVE = 2 };

/* ASCII case-insensitive equality (the bridge uses _stricmp). */
static inline int rsx_vk_ieq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return *a == *b;
}

static inline int rsx_vk_vsync_mode(const char* v)
{
    if (!v || !*v) return RSX_VK_VSYNC_OFF;
    if (rsx_vk_ieq(v, "adaptive"))
        return RSX_VK_VSYNC_ADAPTIVE;
    return atoi(v) > 0 ? RSX_VK_VSYNC_FULL : RSX_VK_VSYNC_OFF;
}

/* swapchain_WSI::init: off prefers IMMEDIATE > MAILBOX > FIFO_RELAXED,
 * adaptive MAILBOX > FIFO_RELAXED, full uses FIFO; FIFO when none of the
 * preferred modes is supported. */
static inline uint32_t rsx_vk_pick_present_mode(int vsync, const uint32_t* modes, uint32_t n)
{
    static const uint32_t off[] = { RSX_VK_PM_IMMEDIATE, RSX_VK_PM_MAILBOX, RSX_VK_PM_FIFO_RELAXED };
    static const uint32_t adaptive[] = { RSX_VK_PM_MAILBOX, RSX_VK_PM_FIFO_RELAXED };
    const uint32_t* pref = NULL;
    uint32_t np = 0;
    if (vsync == RSX_VK_VSYNC_OFF) { pref = off; np = 3; }
    else if (vsync == RSX_VK_VSYNC_ADAPTIVE) { pref = adaptive; np = 2; }
    for (uint32_t k = 0; k < np; k++)
        for (uint32_t i = 0; i < n; i++)
            if (modes[i] == pref[k]) return pref[k];
    return RSX_VK_PM_FIFO;
}

/* swapchain_WSI::init: triple buffering (min + 2, at least 3) when the
 * surface has a maximum, settling for the maximum; min + 1 otherwise. */
static inline uint32_t rsx_vk_swap_image_count(uint32_t min_count, uint32_t max_count)
{
    if (!max_count) return min_count + 1u;
    uint32_t n = min_count + 2u;
    if (n < 3u) n = 3u;
    return n > max_count ? max_count : n;
}

/* RSX_DISPLAY onto RPCS3's vk_exclusive_fs_mode: unset is unspecified (no
 * VkSurfaceFullScreenExclusiveInfoEXT), "exclusive" enable (ALLOWED), any
 * other value disable (DISALLOWED). */
enum { RSX_VK_EXCL_UNSPECIFIED = 0, RSX_VK_EXCL_ENABLE = 1, RSX_VK_EXCL_DISABLE = 2 };

static inline int rsx_vk_exclusive_mode(const char* v)
{
    if (!v || !*v) return RSX_VK_EXCL_UNSPECIFIED;
    return rsx_vk_ieq(v, "exclusive")
        ? RSX_VK_EXCL_ENABLE : RSX_VK_EXCL_DISABLE;
}

/* get_cull_face for CULL_FACE (0x1830) under CULL_FACE_ENABLE (0x183C), as
 * VkCullModeFlags: FRONT 0x0404 -> FRONT_BIT (1), BACK 0x0405 -> BACK_BIT
 * (2), FRONT_AND_BACK 0x0408 -> FRONT_AND_BACK (3). RPCS3 throws on any
 * other value; it is treated as BACK, the register's reset value. */
static inline uint32_t rsx_vk_cull_mode(uint32_t enable, uint32_t face)
{
    if (!enable) return 0u;
    switch (face) {
    case 0x0404u: return 1u;
    case 0x0408u: return 3u;
    default:      return 2u;
    }
}

/* VkBorderColor values. */
enum {
    RSX_VK_BORDER_TRANSPARENT_BLACK = 0,
    RSX_VK_BORDER_OPAQUE_BLACK      = 2,
    RSX_VK_BORDER_OPAQUE_WHITE      = 4,
    RSX_VK_BORDER_CUSTOM            = 1000287003
};

/* RPCS3 vk::border_color_t(color4f) (vkutils/sampler.cpp, VKFormats.cpp
 * get_border_color): rsx::encode_color_to_storage_key truncates each lane to
 * a byte (A<<24 | B<<16 | G<<8 | R); 0, 0xFFFFFFFF and 0xFF000000 are the
 * core colours, anything else is FLOAT_CUSTOM_EXT when the device enabled
 * VK_EXT_custom_border_color, else get_closest_border_color_enum on the
 * floats. *key: the storage key for a custom colour (the sampler pool's
 * identity for it), else 0. rgba is R,G,B,A. */
static inline uint32_t rsx_vk_border_select(const float rgba[4], int custom_ok, uint32_t* key)
{
    const uint32_t enc = ((uint32_t)(uint8_t)(int32_t)(rgba[3] * 255.0f) << 24) |
                         ((uint32_t)(uint8_t)(int32_t)(rgba[2] * 255.0f) << 16) |
                         ((uint32_t)(uint8_t)(int32_t)(rgba[1] * 255.0f) << 8) |
                          (uint32_t)(uint8_t)(int32_t)(rgba[0] * 255.0f);
    *key = 0;
    if (enc == 0x00000000u) return RSX_VK_BORDER_TRANSPARENT_BLACK;
    if (enc == 0xFFFFFFFFu) return RSX_VK_BORDER_OPAQUE_WHITE;
    if (enc == 0xFF000000u) return RSX_VK_BORDER_OPAQUE_BLACK;
    if (custom_ok) { *key = enc; return RSX_VK_BORDER_CUSTOM; }
    if (rgba[0] + rgba[1] + rgba[2] > 1.35f) return RSX_VK_BORDER_OPAQUE_WHITE;
    if (rgba[3] > 0.5f) return RSX_VK_BORDER_OPAQUE_BLACK;
    return RSX_VK_BORDER_TRANSPARENT_BLACK;
}

/* RPCS3 vkutils/device.cpp custom_border_color_features: with
 * VK_EXT_border_color_swizzle the device needs the colour pre-swizzled
 * unless borderColorSwizzleFromImage; without it, every vendor but AMD
 * (0x1002) does (require_border_color_remap). */
static inline int rsx_vk_border_needs_remap(int has_swizzle_ext, int swizzle_from_image,
                                            uint32_t vendor)
{
    if (has_swizzle_ext) return !swizzle_from_image;
    return vendor != 0x1002u;
}

/* VKPresent.cpp present: the video_out_calibration pass runs instead of the
 * raw blit when the output is limited range or the avconf gamma (set by
 * cellVideoOutSetGamma, 1 by default) is not 1 within rsx::fcmp's 1e-6. */
static inline int rsx_vk_present_calibrate(float gamma, int full_rgb_range)
{
    const float d = gamma - 1.0f;
    return !full_rgb_range || d > 0.000001f || d < -0.000001f;
}

/* VideoOutCalibrationPass.glsl main() on one channel: rgb = pow(rgb, gamma),
 * then the limited-range squeeze (c * 220 + 16) / 255. Alpha is written 1. */
static inline float rsx_vk_calibrate_channel(float c, float gamma, int limited)
{
    float v = c > 0.0f ? powf(c, gamma) : 0.0f;
    return limited ? (v * 220.0f + 16.0f) / 255.0f : v;
}

#endif
