/*
 * ps3recomp - self-contained tests for rsx_vk_present_rules.h (RPCS3's
 * swapchain present-mode / image-count / exclusive-fullscreen rules and
 * get_cull_face, as the Vulkan backend applies them).
 *
 *   cc -I libs/video -o t libs/video/tests/test_vk_present_rules.c && ./t
 */
#include "rsx_vk_present_rules.h"

#include <stdio.h>

static int g_fail = 0;

#define CHECK_EQ(got, want)                                                \
    do { long _g = (long)(got), _w = (long)(want);                         \
         if (_g != _w) { printf("FAIL %s:%d  %s = %ld, want %ld\n",         \
                                __FILE__, __LINE__, #got, _g, _w);          \
                         g_fail++; } } while (0)

static void test_vsync_env(void)
{
    CHECK_EQ(rsx_vk_vsync_mode(NULL), RSX_VK_VSYNC_OFF);
    CHECK_EQ(rsx_vk_vsync_mode(""), RSX_VK_VSYNC_OFF);
    CHECK_EQ(rsx_vk_vsync_mode("0"), RSX_VK_VSYNC_OFF);
    CHECK_EQ(rsx_vk_vsync_mode("1"), RSX_VK_VSYNC_FULL);
    CHECK_EQ(rsx_vk_vsync_mode("3"), RSX_VK_VSYNC_FULL);     /* D3D12's frame-interval form */
    CHECK_EQ(rsx_vk_vsync_mode("-1"), RSX_VK_VSYNC_OFF);
    CHECK_EQ(rsx_vk_vsync_mode("adaptive"), RSX_VK_VSYNC_ADAPTIVE);
    CHECK_EQ(rsx_vk_vsync_mode("Adaptive"), RSX_VK_VSYNC_ADAPTIVE);
    CHECK_EQ(rsx_vk_vsync_mode("on"), RSX_VK_VSYNC_OFF);     /* atoi("on") == 0, as the bridge */
}

static void test_present_mode(void)
{
    const uint32_t all[] = { RSX_VK_PM_FIFO, RSX_VK_PM_FIFO_RELAXED, RSX_VK_PM_MAILBOX, RSX_VK_PM_IMMEDIATE };
    const uint32_t no_imm[] = { RSX_VK_PM_FIFO, RSX_VK_PM_MAILBOX, RSX_VK_PM_FIFO_RELAXED };
    const uint32_t relaxed[] = { RSX_VK_PM_FIFO, RSX_VK_PM_FIFO_RELAXED };
    const uint32_t fifo[] = { RSX_VK_PM_FIFO };
    /* Off: IMMEDIATE first (the old code preferred MAILBOX). */
    CHECK_EQ(rsx_vk_pick_present_mode(RSX_VK_VSYNC_OFF, all, 4), RSX_VK_PM_IMMEDIATE);
    CHECK_EQ(rsx_vk_pick_present_mode(RSX_VK_VSYNC_OFF, no_imm, 3), RSX_VK_PM_MAILBOX);
    CHECK_EQ(rsx_vk_pick_present_mode(RSX_VK_VSYNC_OFF, relaxed, 2), RSX_VK_PM_FIFO_RELAXED);
    CHECK_EQ(rsx_vk_pick_present_mode(RSX_VK_VSYNC_OFF, fifo, 1), RSX_VK_PM_FIFO);
    CHECK_EQ(rsx_vk_pick_present_mode(RSX_VK_VSYNC_OFF, NULL, 0), RSX_VK_PM_FIFO);
    /* Adaptive never tears through IMMEDIATE. */
    CHECK_EQ(rsx_vk_pick_present_mode(RSX_VK_VSYNC_ADAPTIVE, all, 4), RSX_VK_PM_MAILBOX);
    CHECK_EQ(rsx_vk_pick_present_mode(RSX_VK_VSYNC_ADAPTIVE, relaxed, 2), RSX_VK_PM_FIFO_RELAXED);
    const uint32_t imm_only[] = { RSX_VK_PM_FIFO, RSX_VK_PM_IMMEDIATE };
    CHECK_EQ(rsx_vk_pick_present_mode(RSX_VK_VSYNC_ADAPTIVE, imm_only, 2), RSX_VK_PM_FIFO);
    /* Full: FIFO regardless. */
    CHECK_EQ(rsx_vk_pick_present_mode(RSX_VK_VSYNC_FULL, all, 4), RSX_VK_PM_FIFO);
}

static void test_image_count(void)
{
    CHECK_EQ(rsx_vk_swap_image_count(2, 8), 4);    /* min + 2 */
    CHECK_EQ(rsx_vk_swap_image_count(1, 8), 3);    /* at least 3 */
    CHECK_EQ(rsx_vk_swap_image_count(2, 3), 3);    /* capped at the maximum */
    CHECK_EQ(rsx_vk_swap_image_count(2, 0), 3);    /* no maximum: min + 1 */
    CHECK_EQ(rsx_vk_swap_image_count(1, 2), 2);
}

static void test_exclusive_mode(void)
{
    CHECK_EQ(rsx_vk_exclusive_mode(NULL), RSX_VK_EXCL_UNSPECIFIED);
    CHECK_EQ(rsx_vk_exclusive_mode(""), RSX_VK_EXCL_UNSPECIFIED);
    CHECK_EQ(rsx_vk_exclusive_mode("exclusive"), RSX_VK_EXCL_ENABLE);
    CHECK_EQ(rsx_vk_exclusive_mode("EXCLUSIVE"), RSX_VK_EXCL_ENABLE);
    CHECK_EQ(rsx_vk_exclusive_mode("borderless"), RSX_VK_EXCL_DISABLE);
    CHECK_EQ(rsx_vk_exclusive_mode("windowed"), RSX_VK_EXCL_DISABLE);
    CHECK_EQ(rsx_vk_exclusive_mode("exclusiv"), RSX_VK_EXCL_DISABLE);
}

static void test_cull_mode(void)
{
    CHECK_EQ(rsx_vk_cull_mode(0, 0x0405u), 0);     /* disabled */
    CHECK_EQ(rsx_vk_cull_mode(0, 0x0408u), 0);
    CHECK_EQ(rsx_vk_cull_mode(1, 0x0404u), 1);     /* FRONT */
    CHECK_EQ(rsx_vk_cull_mode(1, 0x0405u), 2);     /* BACK */
    CHECK_EQ(rsx_vk_cull_mode(1, 0x0408u), 3);     /* FRONT_AND_BACK, not FRONT */
    CHECK_EQ(rsx_vk_cull_mode(1, 0u), 2);          /* never written: reset BACK */
}

static void test_border_select(void)
{
    uint32_t key;
    const float tb[4] = { 0, 0, 0, 0 }, ow[4] = { 1, 1, 1, 1 }, ob[4] = { 0, 0, 0, 1 };
    CHECK_EQ(rsx_vk_border_select(tb, 1, &key), RSX_VK_BORDER_TRANSPARENT_BLACK);
    CHECK_EQ(rsx_vk_border_select(ow, 1, &key), RSX_VK_BORDER_OPAQUE_WHITE);
    CHECK_EQ(rsx_vk_border_select(ob, 1, &key), RSX_VK_BORDER_OPAQUE_BLACK);
    CHECK_EQ(key, 0);
    /* Any other colour: custom with its A,B,G,R storage key ... */
    const float red[4] = { 1, 0, 0, 0.5f };
    CHECK_EQ(rsx_vk_border_select(red, 1, &key), RSX_VK_BORDER_CUSTOM);
    CHECK_EQ(key, 0x7F0000FFu);                    /* 0.5 * 255 truncates to 127 */
    /* ... else the closest core colour from the floats. */
    CHECK_EQ(rsx_vk_border_select(red, 0, &key), RSX_VK_BORDER_TRANSPARENT_BLACK);
    CHECK_EQ(key, 0);
    const float grey[4] = { 0.5f, 0.5f, 0.5f, 0.2f };
    CHECK_EQ(rsx_vk_border_select(grey, 0, &key), RSX_VK_BORDER_OPAQUE_WHITE);
    const float dark[4] = { 0.1f, 0.2f, 0.3f, 0.6f };
    CHECK_EQ(rsx_vk_border_select(dark, 0, &key), RSX_VK_BORDER_OPAQUE_BLACK);
    /* 0.999 truncates to 254: not the white key. */
    const float near_w[4] = { 0.999f, 1, 1, 1 };
    CHECK_EQ(rsx_vk_border_select(near_w, 1, &key), RSX_VK_BORDER_CUSTOM);
    CHECK_EQ(key, 0xFFFFFFFEu);
}

static void test_border_remap(void)
{
    CHECK_EQ(rsx_vk_border_needs_remap(0, 0, 0x10DEu), 1);   /* NVIDIA */
    CHECK_EQ(rsx_vk_border_needs_remap(0, 0, 0x8086u), 1);   /* Intel */
    CHECK_EQ(rsx_vk_border_needs_remap(0, 0, 0x1002u), 0);   /* AMD */
    CHECK_EQ(rsx_vk_border_needs_remap(1, 1, 0x10DEu), 0);   /* swizzle from image */
    CHECK_EQ(rsx_vk_border_needs_remap(1, 0, 0x1002u), 1);
}

static void test_present_calibration(void)
{
    /* Full range, gamma 1: raw blit (this config, R2 never sets a gamma). */
    CHECK_EQ(rsx_vk_present_calibrate(1.0f, 1), 0);
    CHECK_EQ(rsx_vk_present_calibrate(1.0000001f, 1), 0);   /* within fcmp's 1e-6 */
    CHECK_EQ(rsx_vk_present_calibrate(0.8f, 1), 1);
    CHECK_EQ(rsx_vk_present_calibrate(1.2f, 1), 1);
    CHECK_EQ(rsx_vk_present_calibrate(1.0f, 0), 1);          /* limited range */
    /* pow(c, gamma): gamma > 1 darkens the midtones, < 1 brightens them;
     * black and white stay put; limited range squeezes to 16..236. */
    CHECK_EQ((long)(rsx_vk_calibrate_channel(0.5f, 1.2f, 0) * 10000.0f + 0.5f), 4353);
    CHECK_EQ((long)(rsx_vk_calibrate_channel(0.5f, 0.8f, 0) * 10000.0f + 0.5f), 5743);
    CHECK_EQ((long)(rsx_vk_calibrate_channel(0.0f, 1.2f, 0) * 255.0f + 0.5f), 0);
    CHECK_EQ((long)(rsx_vk_calibrate_channel(1.0f, 1.2f, 0) * 255.0f + 0.5f), 255);
    CHECK_EQ((long)(rsx_vk_calibrate_channel(0.0f, 1.0f, 1) * 255.0f + 0.5f), 16);
    CHECK_EQ((long)(rsx_vk_calibrate_channel(1.0f, 1.0f, 1) * 255.0f + 0.5f), 236);
}

int main(void)
{
    test_vsync_env();
    test_present_mode();
    test_image_count();
    test_exclusive_mode();
    test_cull_mode();
    test_border_select();
    test_border_remap();
    test_present_calibration();
    if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
    printf("all vk present-rule checks passed\n");
    return 0;
}
