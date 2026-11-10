/*
 * ps3recomp - pure maths of the Vulkan backend's raster state and internal
 * resolution scaling (RSX_RES_SCALE). No Vulkan types, so the unit tests
 * build it without a device (tests/test_vk_scale_math.c).
 */
#ifndef RSX_VK_SCALE_MATH_H
#define RSX_VK_SCALE_MATH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The kind of host depth format a draw renders into. */
enum {
    RSX_VK_ZHOST_UNORM16 = 0,   /* D16_UNORM                       */
    RSX_VK_ZHOST_UNORM24 = 1,   /* D24_UNORM_S8_UINT, X8_D24       */
    RSX_VK_ZHOST_FLOAT   = 2    /* D32_SFLOAT, D32_SFLOAT_S8_UINT  */
};

/* vkCmdSetDepthBias's constant factor for POLYGON_OFFSET_BIAS.
 *
 * RPCS3 VKDraw.cpp update_draw_state passes the register straight through
 * (Z16 -> D16_UNORM and Z24S8 -> D24S8 / float have the same minimum
 * resolvable difference R as the RSX), halved for D24S8 on NVIDIA, whose R
 * is twice the RSX's 1 / (2^24 - 1). The resolution scale does not enter:
 * R is a property of the format, not of the pixel grid.
 *
 * One case RPCS3 never hits: a fixed-point Z16 zeta held in a 24-bit or
 * float image (RSX_VK_DEPTH_LEGACY, or no D16 attachment support). The
 * guest's units are then 2^-16 and the host's 2^-24 (a float depth's R near
 * the [0.5, 1) range RPCS3's comment relies on is 2^-24 as well), so the
 * constant is 256x larger to offset the same depth. */
static inline float rsx_vk_depth_bias_constant(float bias, int guest_z16_fixed,
                                               int host_kind, int nv_d24_half)
{
    if (!(bias == bias)) return 0.0f;
    float k = 1.0f;
    if (guest_z16_fixed && host_kind != RSX_VK_ZHOST_UNORM16) k = 256.0f;
    if (host_kind == RSX_VK_ZHOST_UNORM24 && nv_d24_half) k *= 0.5f;
    return bias * k;
}

/* vkCmdSetDepthBias's slope factor. RPCS3 passes POLYGON_OFFSET_SCALE_FACTOR
 * through at every resolution scale; `scale_by_rs` (RSX_VK_BIAS_SLOPE_RS=1)
 * multiplies it by the scale instead, since a triangle's depth slope per
 * render pixel is 1/rs of its slope per guest pixel. */
static inline float rsx_vk_depth_bias_slope(float scale, float rs, int scale_by_rs)
{
    if (!(scale == scale)) return 0.0f;
    return (scale_by_rs && rs > 1.0f) ? scale * rs : scale;
}

/* vkCmdSetLineWidth's width: RPCS3 VKDraw.cpp update_draw_state uses
 * line_width x the resolution scale with wideLines and 1.0 without; Vulkan
 * also wants it inside VkPhysicalDeviceLimits::lineWidthRange. A width that
 * is not a positive number reads as 1. */
static inline float rsx_vk_line_width(float guest, float rs, int wide_lines,
                                      float range_min, float range_max)
{
    if (!wide_lines) return 1.0f;
    float w = (guest > 0.0f && guest == guest) ? guest : 1.0f;
    if (rs > 1.0f) w *= rs;
    if (!(range_max >= 1.0f)) return 1.0f;      /* range unknown: the one legal width */
    if (!(range_min > 0.0f) || range_min > 1.0f) range_min = 1.0f;
    if (w < range_min) w = range_min;
    if (w > range_max) w = range_max;
    return w;
}

/* The scale every attachment of one render-target binding renders at: the
 * largest of theirs ("if either qualifies, both scale"), so a pass's colour
 * and depth images always share a pixel grid. */
static inline float rsx_vk_binding_scale(const float* rs, unsigned n)
{
    float s = 1.0f;
    for (unsigned i = 0; i < n; i++)
        if (rs[i] > s) s = rs[i];
    return s;
}

/* Nearest source index of output i of n over pn source texels. */
static inline uint32_t rsx_vk_rs_nearest(uint32_t i, uint32_t n, uint32_t pn)
{
    if (!n || !pn) return 0;
    const uint32_t v = (uint32_t)(((uint64_t)i * 2u + 1u) * pn / (2u * (uint64_t)n));
    return v < pn ? v : pn - 1u;
}

/* A w x h block of bpp-byte texels (src_pitch apart) point-sampled up to
 * dw x dh tightly packed rows at dst: the host side of an upload into a
 * texture backed by a scaled image. */
static inline void rsx_vk_rs_upscale_rows(uint8_t* dst, uint32_t dw, uint32_t dh,
                                          const uint8_t* src, uint32_t w, uint32_t h,
                                          size_t src_pitch, uint32_t bpp)
{
    for (uint32_t y = 0; y < dh; y++) {
        const uint8_t* s = src + (size_t)rsx_vk_rs_nearest(y, dh, h) * src_pitch;
        uint8_t* d = dst + (size_t)y * dw * bpp;
        for (uint32_t x = 0; x < dw; x++)
            memcpy(d + (size_t)x * bpp, s + (size_t)rsx_vk_rs_nearest(x, dw, w) * bpp, bpp);
    }
}

#endif /* RSX_VK_SCALE_MATH_H */
