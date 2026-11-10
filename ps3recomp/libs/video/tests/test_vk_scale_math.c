/*
 * ps3recomp - self-contained tests for rsx_vk_scale_math.h (the Vulkan
 * backend's depth-bias, line-width and resolution-scale maths).
 *
 *   cc -I libs/video -o t libs/video/tests/test_vk_scale_math.c && ./t
 */
#include "rsx_vk_scale_math.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond)                                                        \
    do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
                        g_fail++; } } while (0)

#define CHECK_F(got, want)                                                 \
    do { double _g = (double)(got), _w = (double)(want);                   \
         if (!(_g == _w)) { printf("FAIL %s:%d  %s = %g, want %g\n",        \
                                   __FILE__, __LINE__, #got, _g, _w);       \
                            g_fail++; } } while (0)

static void test_depth_bias(void)
{
    /* RPCS3 VKDraw: passed through for D16 / float, halved for D24S8 on NVIDIA. */
    CHECK_F(rsx_vk_depth_bias_constant(3.0f, 1, RSX_VK_ZHOST_UNORM16, 1), 3.0f);
    CHECK_F(rsx_vk_depth_bias_constant(3.0f, 0, RSX_VK_ZHOST_UNORM24, 0), 3.0f);
    CHECK_F(rsx_vk_depth_bias_constant(3.0f, 0, RSX_VK_ZHOST_UNORM24, 1), 1.5f);
    CHECK_F(rsx_vk_depth_bias_constant(3.0f, 0, RSX_VK_ZHOST_FLOAT, 1), 3.0f);
    CHECK_F(rsx_vk_depth_bias_constant(-2.0f, 0, RSX_VK_ZHOST_FLOAT, 0), -2.0f);
    /* A fixed Z16 zeta in a 24-bit or float image: 2^24 / 2^16 host units each. */
    CHECK_F(rsx_vk_depth_bias_constant(2.0f, 1, RSX_VK_ZHOST_UNORM24, 0), 512.0f);
    CHECK_F(rsx_vk_depth_bias_constant(2.0f, 1, RSX_VK_ZHOST_UNORM24, 1), 256.0f);
    CHECK_F(rsx_vk_depth_bias_constant(2.0f, 1, RSX_VK_ZHOST_FLOAT, 1), 512.0f);
    CHECK_F(rsx_vk_depth_bias_constant(NAN, 0, RSX_VK_ZHOST_FLOAT, 0), 0.0f);
    /* The slope passes through unless asked to follow the scale. */
    CHECK_F(rsx_vk_depth_bias_slope(1.25f, 2.0f, 0), 1.25f);
    CHECK_F(rsx_vk_depth_bias_slope(1.25f, 2.0f, 1), 2.5f);
    CHECK_F(rsx_vk_depth_bias_slope(1.25f, 1.0f, 1), 1.25f);
}

static void test_line_width(void)
{
    CHECK_F(rsx_vk_line_width(3.0f, 2.0f, 0, 1.0f, 64.0f), 1.0f);    /* no wideLines */
    CHECK_F(rsx_vk_line_width(3.0f, 1.0f, 1, 1.0f, 64.0f), 3.0f);
    CHECK_F(rsx_vk_line_width(3.0f, 2.0f, 1, 1.0f, 64.0f), 6.0f);    /* RPCS3: x scale */
    CHECK_F(rsx_vk_line_width(40.0f, 2.0f, 1, 1.0f, 64.0f), 64.0f);  /* clamped */
    CHECK_F(rsx_vk_line_width(0.25f, 1.0f, 1, 0.5f, 8.0f), 0.5f);
    CHECK_F(rsx_vk_line_width(0.0f, 2.0f, 1, 1.0f, 8.0f), 2.0f);     /* unset reads as 1 */
    CHECK_F(rsx_vk_line_width(NAN, 1.0f, 1, 1.0f, 8.0f), 1.0f);
    CHECK_F(rsx_vk_line_width(5.0f, 1.0f, 1, 0.0f, 0.0f), 1.0f);     /* range unknown */
}

static void test_binding_scale(void)
{
    const float a[3] = { 1.0f, 2.0f, 1.0f };
    const float b[2] = { 1.5f, 1.5f };
    CHECK_F(rsx_vk_binding_scale(a, 3), 2.0f);
    CHECK_F(rsx_vk_binding_scale(b, 2), 1.5f);
    CHECK_F(rsx_vk_binding_scale(a, 0), 1.0f);
}

static void test_upscale_rows(void)
{
    /* 2x2 texels of 2 bytes, padded rows, to 4x4 and to 3x3. */
    const unsigned char src[2 * 6] = { 1, 2, 3, 4, 0xEE, 0xEE,
                                       5, 6, 7, 8, 0xEE, 0xEE };
    unsigned char d[4 * 4 * 2];
    rsx_vk_rs_upscale_rows(d, 4, 4, src, 2, 2, 6, 2);
    const unsigned char want4[4 * 4 * 2] = {
        1, 2, 1, 2, 3, 4, 3, 4,   1, 2, 1, 2, 3, 4, 3, 4,
        5, 6, 5, 6, 7, 8, 7, 8,   5, 6, 5, 6, 7, 8, 7, 8 };
    CHECK(!memcmp(d, want4, sizeof want4));
    unsigned char e[3 * 3 * 2];
    rsx_vk_rs_upscale_rows(e, 3, 3, src, 2, 2, 6, 2);
    const unsigned char want3[3 * 3 * 2] = {
        1, 2, 3, 4, 3, 4,   5, 6, 7, 8, 7, 8,   5, 6, 7, 8, 7, 8 };
    CHECK(!memcmp(e, want3, sizeof want3));
    /* Same size is a copy; downsizing samples texel centres. */
    CHECK(rsx_vk_rs_nearest(0, 2, 2) == 0 && rsx_vk_rs_nearest(1, 2, 2) == 1);
    CHECK(rsx_vk_rs_nearest(0, 2, 4) == 1 && rsx_vk_rs_nearest(1, 2, 4) == 3);
    CHECK(rsx_vk_rs_nearest(5, 1, 0) == 0);
}

int main(void)
{
    test_depth_bias();
    test_line_width();
    test_binding_scale();
    test_upscale_rows();
    if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
    printf("all vk scale math checks passed\n");
    return 0;
}
