/* Real-Vulkan engine harness: rsx_vulkan_backend.c driven headless under
 * VK_LAYER_KHRONOS_validation (synchronization validation on), through the
 * backend vtable directly and through the draw engine's NV4097 / NV3089
 * entry points, reading the results back and checking them.
 *
 * Exit 0 with "SKIP" when no Vulkan device can be created. Every validation
 * warning or error the layer reports counts as a failure.
 *
 * Environment (all optional): PS3RECOMP_VK_VALIDATION (default "sync"; "0"
 * runs without the layer), VKREAL_ONLY=<substring> runs only matching
 * tests. Built by scratch\enginetest\build_vkreal.ps1. */
#include "rsx_draw_engine.h"
#include "rsx_vulkan_backend.h"
#include "rsx_test_programs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define GUEST_BYTES     (16u << 20)
#define GUEST_LOCAL_EA  (8u << 20)

u8* vm_base;
u32 ppu_vm_size;
u32 cellGcmResolveLocated(int local, u32 offset) { return local ? GUEST_LOCAL_EA + offset : offset; }
u32 cellGcmResolveIO(u32 offset) { return offset < GUEST_LOCAL_EA ? offset : 0; }
void cellGcm_replay_display_buffers_to_engine(void) {}
float cellAvconfExt_display_gamma(void) { return 1.0f; }

static int g_fail, g_pass;
#define CHECK(c, ...) do { if (c) { g_pass++; printf("[PASS] "); } \
    else { g_fail++; printf("[FAIL] "); } printf(__VA_ARGS__); printf("\n"); } while (0)

static const rsx_draw_backend* be;
static void* U;

static int env_set(const char* k, const char* v)
{
#ifdef _WIN32
    return _putenv_s(k, v);
#else
    return setenv(k, v, 1);
#endif
}

static int near8(u32 a, u32 b, u32 tol) { return (a > b ? a - b : b - a) <= tol; }

/* ======================================================================== */
/* ---- direct backend tests ---------------------------------------------- */

static const char* fmt_name(rsx_be_format f)
{
    switch (f) {
    case RSX_BE_FMT_R5G6B5: return "R5G6B5";
    case RSX_BE_FMT_A1R5G5B5: return "A1R5G5B5";
    case RSX_BE_FMT_R8: return "R8(B8)";
    case RSX_BE_FMT_R8G8: return "R8G8(G8B8)";
    case RSX_BE_FMT_R8G8B8A8: return "RGBA8";
    default: return "?";
    }
}

/* The RGBA8 lanes a native target holds after storing `in` (RGBA8 lanes):
 * the backend's own CPU conversion (vk_cv_from_rgba / vk_cv_to_rgba). */
static u8 q_ex(u32 v8, u32 max) { const u32 q = (v8 * max + 127u) / 255u; return (u8)((q * 255u + max / 2u) / max); }
static void expect_lanes(rsx_be_format f, const u8 in[4], u8 out[4])
{
    switch (f) {
    case RSX_BE_FMT_R5G6B5:
        out[0] = q_ex(in[0], 31); out[1] = q_ex(in[1], 63); out[2] = q_ex(in[2], 31); out[3] = 255; break;
    case RSX_BE_FMT_A1R5G5B5:
        out[0] = q_ex(in[0], 31); out[1] = q_ex(in[1], 31); out[2] = q_ex(in[2], 31);
        out[3] = in[3] >= 128 ? 255 : 0; break;
    case RSX_BE_FMT_R8: out[0] = out[1] = out[2] = out[3] = in[2]; break;
    case RSX_BE_FMT_R8G8: out[0] = out[2] = in[2]; out[1] = out[3] = in[1]; break;
    default: memcpy(out, in, 4); break;
    }
}
/* GPU float -> UNORM conversion of a clear may round a tie either way. */
static u32 lane_tol(rsx_be_format f)
{
    return (f == RSX_BE_FMT_R5G6B5 || f == RSX_BE_FMT_A1R5G5B5) ? 9u : 1u;
}

static void test_native_formats_direct(void)
{
    printf("-- native 16/8-bit colour targets (direct)\n");
    static const rsx_be_format fmts[] = { RSX_BE_FMT_R5G6B5, RSX_BE_FMT_A1R5G5B5,
                                          RSX_BE_FMT_R8, RSX_BE_FMT_R8G8, RSX_BE_FMT_R8G8B8A8 };
    for (u32 i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
        const rsx_be_format f = fmts[i];
        const int sup = be->color_format_supported ? be->color_format_supported(U, f) : 0;
        printf("   %s supported: %d\n", fmt_name(f), sup);
        if (f != RSX_BE_FMT_R8G8B8A8 && !sup) continue;
        const u32 W = 16, H = 16;
        static u8 seed[16 * 16 * 4], rb[16 * 16 * 4];
        for (u32 p = 0; p < W * H; p++) {
            seed[p * 4 + 0] = (u8)(p * 7); seed[p * 4 + 1] = (u8)(p * 13 + 5);
            seed[p * 4 + 2] = (u8)(255 - p); seed[p * 4 + 3] = (u8)((p & 1) ? 255 : 0);
        }
        const u32 t = be->color_target_create(U, f, W, H, seed, W * 4);
        CHECK(t != 0, "%s: target created", fmt_name(f));
        if (!t) continue;
        memset(rb, 0xCD, sizeof rb);
        be->readback(U, t, 0, 0, W, H, rb, W * 4);
        int ok = 1; u32 bad = 0;
        for (u32 p = 0; p < W * H && ok; p++) {
            u8 e[4]; expect_lanes(f, seed + p * 4, e);
            for (u32 c = 0; c < 4; c++)
                if (rb[p * 4 + c] != e[c]) { ok = 0; bad = p; }
        }
        CHECK(ok, "%s: seeded contents read back exactly (pixel %u: %02X %02X %02X %02X)",
              fmt_name(f), bad, rb[bad * 4], rb[bad * 4 + 1], rb[bad * 4 + 2], rb[bad * 4 + 3]);

        const float clr[4] = { 1.0f, 0.5f, 0.25f, 0.75f };
        be->clear_color(U, t, clr);
        be->readback(U, t, 0, 0, W, H, rb, W * 4);
        const u8 cl8[4] = { 255, 128, 64, 191 };
        u8 e[4]; expect_lanes(f, cl8, e);
        ok = 1;
        for (u32 p = 0; p < W * H; p++)
            for (u32 c = 0; c < 4; c++)
                if (!near8(rb[p * 4 + c], e[c], lane_tol(f))) ok = 0;
        CHECK(ok, "%s: clear (1,.5,.25,.75) reads %02X %02X %02X %02X (want ~%02X %02X %02X %02X)",
              fmt_name(f), rb[0], rb[1], rb[2], rb[3], e[0], e[1], e[2], e[3]);
        memcpy(e, rb, 4);   /* the cleared value as the GPU rounded it */

        if (be->color_target_write) {
            u8 rows[4 * 3 * 4];
            for (u32 p = 0; p < 12; p++) {
                rows[p * 4 + 0] = (u8)(0x10 * p); rows[p * 4 + 1] = (u8)(0xF0 - 0x11 * p);
                rows[p * 4 + 2] = (u8)(0x30 + 0x09 * p); rows[p * 4 + 3] = 0xFF;
            }
            be->color_target_write(U, t, 5, 6, 4, 3, rows, 16);
            be->readback(U, t, 0, 0, W, H, rb, W * 4);
            ok = 1;
            for (u32 y = 0; y < H; y++)
                for (u32 x = 0; x < W; x++) {
                    u8 w[4];
                    if (x >= 5 && x < 9 && y >= 6 && y < 9) expect_lanes(f, rows + ((y - 6) * 4 + (x - 5)) * 4, w);
                    else memcpy(w, e, 4);
                    for (u32 c = 0; c < 4; c++)
                        if (rb[(y * W + x) * 4 + c] != w[c]) ok = 0;
                }
            CHECK(ok, "%s: color_target_write lands exactly at (5,6) 4x3", fmt_name(f));
        }
        be->color_target_release(U, t);
    }
}

/* Each pixel of an RGBA8 ramp: R = x, G = y. */
static void ramp(u8* px, u32 w, u32 h)
{
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            u8* p = px + (y * w + x) * 4;
            p[0] = (u8)(x * 16 + 8); p[1] = (u8)(y * 16 + 8); p[2] = 0x55; p[3] = 0xFF;
        }
}

static void test_blit_flip_direct(void)
{
    printf("-- mirrored blits (direct)\n");
    if (!be->color_target_blit_flip) { CHECK(0, "backend has color_target_blit_flip"); return; }
    static const rsx_be_format fmts[] = { RSX_BE_FMT_R8G8B8A8, RSX_BE_FMT_R5G6B5, RSX_BE_FMT_A1R5G5B5 };
    for (u32 fi = 0; fi < 3; fi++) {
        const rsx_be_format f = fmts[fi];
        if (f != RSX_BE_FMT_R8G8B8A8 && !(be->color_format_supported && be->color_format_supported(U, f)))
            continue;
        u8 src[8 * 8 * 4], rb[8 * 8 * 4];
        ramp(src, 8, 8);
        const u32 s = be->color_target_create(U, f, 8, 8, src, 32);
        const u32 d = be->color_target_create(U, f, 8, 8, NULL, 0);
        const float z[4] = { 0, 0, 0, 0 };
        for (u32 flip = 1; flip <= 3; flip++) {
            be->clear_color(U, d, z);
            be->color_target_blit_flip(U, s, 0, 0, 8, 8, d, 0, 0, 8, 8, 0, flip);
            be->readback(U, d, 0, 0, 8, 8, rb, 32);
            int ok = 1;
            for (u32 y = 0; y < 8; y++)
                for (u32 x = 0; x < 8; x++) {
                    const u32 sx = (flip & 1) ? 7 - x : x, sy = (flip & 2) ? 7 - y : y;
                    u8 e[4]; expect_lanes(f, src + (sy * 8 + sx) * 4, e);
                    for (u32 c = 0; c < 3; c++)
                        if (rb[(y * 8 + x) * 4 + c] != e[c]) ok = 0;
                }
            CHECK(ok, "%s flip %u: dst(0,0) = src(%u,%u) (got R %02X G %02X)", fmt_name(f), flip,
                  (flip & 1) ? 7 : 0, (flip & 2) ? 7 : 0, rb[0], rb[1]);
        }
        {
            be->clear_color(U, d, z);
            be->color_target_blit_flip(U, s, 2, 1, 4, 3, d, 3, 4, 4, 3, 0, 1u);
            be->readback(U, d, 0, 0, 8, 8, rb, 32);
            int ok = 1;
            for (u32 y = 0; y < 8; y++)
                for (u32 x = 0; x < 8; x++) {
                    u8 e[4];
                    if (x >= 3 && x < 7 && y >= 4 && y < 7) {
                        const u32 sx = 2 + (3 - (x - 3)), sy = 1 + (y - 4);
                        expect_lanes(f, src + (sy * 8 + sx) * 4, e);
                    } else {
                        const u8 z8[4] = { 0, 0, 0, 0 }; expect_lanes(f, z8, e);
                    }
                    for (u32 c = 0; c < 3; c++)
                        if (rb[(y * 8 + x) * 4 + c] != e[c]) ok = 0;
                }
            CHECK(ok, "%s: sub-rect (2,1 4x3) mirrored in X onto (3,4)", fmt_name(f));
        }
        if (be->color_target_blit_filtered) {
            const u32 d2 = be->color_target_create(U, f, 16, 16, NULL, 0);
            u8 rb2[16 * 16 * 4];
            be->color_target_blit_filtered(U, s, 0, 0, 8, 8, d2, 0, 0, 16, 16, 0);
            be->readback(U, d2, 0, 0, 16, 16, rb2, 64);
            int ok = 1;
            for (u32 y = 0; y < 16; y++)
                for (u32 x = 0; x < 16; x++) {
                    u8 e[4]; expect_lanes(f, src + ((y / 2) * 8 + x / 2) * 4, e);
                    for (u32 c = 0; c < 3; c++)
                        if (rb2[(y * 16 + x) * 4 + c] != e[c]) ok = 0;
                }
            CHECK(ok, "%s: nearest 2x blit replicates pixels", fmt_name(f));
            be->color_target_release(U, d2);
        }
        be->color_target_release(U, s);
        be->color_target_release(U, d);
    }
}

static void test_zeta_direct(void)
{
    printf("-- zeta / stencil readback (direct)\n");
    if (!be->depth_target_create_ex || !be->zeta_readback) { CHECK(0, "zeta hooks present"); return; }
    static const struct { u32 zfmt, fl; const char* n; } k[] = {
        { 2, 0, "Z24S8" }, { 1, 0, "Z16" }, { 2, 1, "Z24S8 float" }, { 1, 1, "Z16 float" } };
    for (u32 i = 0; i < 4; i++) {
        const u32 W = 24, H = 10;
        const u32 dt = be->depth_target_create_ex(U, W, H, k[i].zfmt, k[i].fl);
        CHECK(dt != 0, "%s: depth target created", k[i].n);
        if (!dt) continue;
        be->clear_depth_stencil(U, dt, RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 0.25f, 0x5A);
        static float f[24 * 10], f2[24 * 10];
        static u32 raw[24 * 10];
        static u8 st[24 * 10], st2[24 * 10];
        u32 bits = 77; int hs = -1;
        memset(f, 0, sizeof f); memset(raw, 0, sizeof raw); memset(st, 0, sizeof st);
        const int r = be->zeta_readback(U, dt, 2, 3, 8, 4, f, raw, &bits, st, &hs);
        CHECK(r == 0, "%s: zeta_readback succeeds (%d)", k[i].n, r);
        if (r == 0) {
            if (bits) {
                const u32 want = bits == 24 ? 4194304u : 16384u;
                int ok = 1;
                for (u32 p = 0; p < 32; p++) if (!near8(raw[p], want, 1)) ok = 0;
                CHECK(ok, "%s: raw %u-bit depth 0.25 = %u (got %u)", k[i].n, bits, want, raw[0]);
            } else {
                int ok = 1;
                for (u32 p = 0; p < 32; p++) if (fabsf(f[p] - 0.25f) > 1e-4f) ok = 0;
                CHECK(ok, "%s: float depth 0.25 (got %f)", k[i].n, f[0]);
            }
            if (hs) {
                int ok = 1;
                for (u32 p = 0; p < 32; p++) if (st[p] != 0x5A) ok = 0;
                CHECK(ok, "%s: stencil 0x5A (got %02X)", k[i].n, st[0]);
            }
            CHECK(hs == (k[i].zfmt == 2), "%s: has stencil = %d", k[i].n, hs);
        }
        if (be->depth_readback) {
            memset(f2, 0, sizeof f2);
            const int r2 = be->depth_readback(U, dt, 0, 0, W, H, f2, W * 4);
            int ok = r2 == 0;
            for (u32 p = 0; ok && p < W * H; p++) if (fabsf(f2[p] - 0.25f) > 1e-4f) ok = 0;
            CHECK(ok, "%s: depth_readback 0.25 (r %d, got %f)", k[i].n, r2, f2[0]);
        }
        if (be->stencil_readback && k[i].zfmt == 2) {
            memset(st2, 0, sizeof st2);
            const int r3 = be->stencil_readback(U, dt, 0, 0, W, H, st2, W);
            int ok = r3 == 0;
            for (u32 p = 0; ok && p < W * H; p++) if (st2[p] != 0x5A) ok = 0;
            CHECK(ok, "%s: stencil_readback 0x5A (r %d, got %02X)", k[i].n, r3, st2[0]);
        }
        be->depth_target_release(U, dt);
    }
}

/* texture_copy_from_color into cube / volume textures: legal copies that
 * report success (the engine test samples the results). */
static void test_tcfc_direct(void)
{
    printf("-- texture_copy_from_color (direct)\n");
    if (!be->texture_copy_from_color) { CHECK(0, "backend has texture_copy_from_color"); return; }
    static u8 src[16 * 96 * 4];
    for (u32 p = 0; p < 16 * 96; p++) { src[p * 4] = (u8)(p / (16 * 16) * 40); src[p * 4 + 1] = 1; src[p * 4 + 2] = 2; src[p * 4 + 3] = 255; }
    const u32 rt = be->color_target_create(U, RSX_BE_FMT_R8G8B8A8, 16, 96, src, 64);
    const u32 cube = be->texture_create(U, RSX_BE_FMT_R8G8B8A8, 16, 16, 2, 6, 0xAAE4u, 0xA5u);
    int ok = rt && cube;
    for (u32 face = 0; ok && face < 6; face++) {
        ok &= be->texture_copy_from_color(U, cube, face, 0, rt, 0, face * 16, 16, 16) == 1;
        ok &= be->texture_copy_from_color(U, cube, face, 1, rt, 0, face * 16, 8, 8) == 1;
    }
    CHECK(ok, "cube: 6 faces x 2 levels copied");
    if (be->texture_create_3d) {
        const u32 vol = be->texture_create_3d(U, RSX_BE_FMT_R8G8B8A8, 16, 16, 4, 1, 0xAAE4u, 0xA5u);
        ok = vol != 0;
        for (u32 z = 0; ok && z < 4; z++) ok &= be->texture_copy_from_color(U, vol, z, 0, rt, 0, z * 16, 16, 16) == 1;
        CHECK(ok, "volume: 4 slices copied");
        if (vol) be->texture_release(U, vol);
    }
    if (be->color_format_supported && be->color_format_supported(U, RSX_BE_FMT_R5G6B5)) {
        const u32 rt16 = be->color_target_create(U, RSX_BE_FMT_R5G6B5, 16, 16, NULL, 0);
        const int r = be->texture_copy_from_color(U, cube, 0, 0, rt16, 0, 0, 16, 16);
        CHECK(r == 0, "an R5G6B5 target is not copied into an RGBA8 cube (%d)", r);
        be->color_target_release(U, rt16);
    }
    be->texture_release(U, cube);
    be->color_target_release(U, rt);
    be->submit_and_wait(U, RSX_BE_FLUSH_GUEST_REFERENCE);
}

/* ======================================================================== */
/* ---- engine (method stream) tests -------------------------------------- */

#define DMA_LOCAL   0xFEED0000u
#define VTX_OFFSET  0x00010000u        /* main */
#define FP_COL      0x00060000u        /* main */
#define FP_TEX      0x00060100u
#define TEX_MAIN    0x00070000u        /* main: guest textures */
/* FORMAT bit 3 (CELL_GCM_TEXTURE_BORDER_COLOR): no border texels in memory. */
#define TEX_BORDER_COLOR (1u << 3)

static void m(u32 method, u32 arg) { rsx_draw_engine_method(method, arg); }
static u32 fbits(float f) { u32 w; memcpy(&w, &f, 4); return w; }
static void mf(u32 method, float f) { m(method, fbits(f)); }
static void put_be32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static void put_bef(u8* p, float f) { put_be32(p, fbits(f)); }

/* Three vertices (stride 48): position (attr 0), colour (attr 3), texcoord
 * (attr 8). A full-surface triangle at clip depth z. */
static void set_tri(float z, const float col[4], const float tc[4])
{
    static const float xy[3][2] = { { -1, -1 }, { 3, -1 }, { -1, 3 } };
    for (u32 v = 0; v < 3; v++) {
        u8* p = vm_base + VTX_OFFSET + v * 48u;
        put_bef(p + 0, xy[v][0]); put_bef(p + 4, xy[v][1]); put_bef(p + 8, z); put_bef(p + 12, 1.0f);
        for (u32 c = 0; c < 4; c++) put_bef(p + 16 + c * 4, col[c]);
        for (u32 c = 0; c < 4; c++) put_bef(p + 32 + c * 4, tc[c]);
    }
    rsx_draw_engine_note_guest_write(VTX_OFFSET, 3 * 48);
}

static void draw(void)
{
    m(0x1808, RSX_PRIMITIVE_TRIANGLES);
    m(0x1814, 0u | ((3u - 1u) << 24));
    m(0x1808, 0u);
}

static void use_fp(u32 off) { m(0x08E4, off | 2u); m(0x1D60, 0x40u); }

/* MOV o0, v0 ; MOV o1, v3 (or TXL o1, v8 on vertex unit 0) ; MOV o7, v8 */
static void load_vp(int vtex)
{
    u8 vp[48];
    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    if (vtex) rsx_test_vp_txl_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 0, 1, 0);
    else      rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 0);
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 12; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0x0B80 + i * 4, w); }
    m(0x1EA0, 0);
}

static void set_surface(u32 cfmt_full, u32 w, u32 h, u32 off, u32 pitch)
{
    m(0x0194, DMA_LOCAL);
    m(0x0200, w << 16); m(0x0204, h << 16);
    m(0x0208, cfmt_full);
    m(0x0220, 1u);
    m(0x020C, pitch);
    m(0x0210, off);
    m(0x0A00, w << 16); m(0x0A04, h << 16);
    m(0x08C0, w << 16); m(0x08C4, h << 16);
    mf(0x0A20, (float)w * 0.5f); mf(0x0A24, (float)h * 0.5f);
    mf(0x0A30, (float)w * 0.5f); mf(0x0A34, (float)h * -0.5f);
}

static void engine_up(void)
{
    rsx_draw_engine_shutdown();
    rsx_draw_engine_set_backend(be);
    if (rsx_draw_engine_init(1280, 720) != 0) { printf("[FAIL] engine init\n"); exit(1); }
    rsx_test_fp_mov_r0_col0(vm_base + FP_COL, RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_tex_r0_tc0(vm_base + FP_TEX, 0);
    rsx_draw_engine_note_guest_write(FP_COL, 0x200);
    set_surface(8u | (2u << 5), 64, 64, 0x40000u, 256u);
    m(0x022C, 256u);
    for (u32 a = 0; a < 16; a++) m(0x1740 + a * 4, 2u);
    m(0x1740, 2u | (4u << 4) | (48u << 8)); m(0x1680, 0x80000000u | VTX_OFFSET);
    m(0x174C, 2u | (4u << 4) | (48u << 8)); m(0x168C, 0x80000000u | (VTX_OFFSET + 16));
    m(0x1760, 2u | (4u << 4) | (48u << 8)); m(0x16A0, 0x80000000u | (VTX_OFFSET + 32));
    load_vp(0);
    use_fp(FP_COL);
    m(0x0324, 0x01010101u);
    m(0x0310, 0u);
    m(0x0A74, 0u); m(0x0A70, 0u);
}

static const u8* guest_local(u32 off) { return vm_base + GUEST_LOCAL_EA + off; }

static void store_color(u32 off, u32 len)
{
    rsx_draw_engine_range_read_color(RSX_LOCATION_LOCAL, off, len);
}

/* Unit 0 bound at `offset`, A8R8G8B8 linear (fmt_extra ORed into FORMAT),
 * w x h, pitch, CONTROL3 depth; nearest, clamped. */
static void bind_tex0(u32 location, u32 offset, u32 fmt_extra, u32 w, u32 h, u32 pitch, u32 depth)
{
    const u32 dma = (location == RSX_LOCATION_MAIN) ? 2u : 1u;
    m(0x1A00, offset);
    m(0x1A04, dma | TEX_BORDER_COLOR | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16) | fmt_extra);
    m(0x1A08, 0x00030303u);
    m(0x1A0C, 0x80000000u);
    m(0x1A10, 0xAAE4u);
    m(0x1A14, (1u << 16) | (1u << 24));
    m(0x1A18, (w << 16) | h);
    m(0x1840, (depth << 20) | pitch);
}

/* Big-endian 16-bit packed fields within one step of each other. */
static int packed16_near(u32 v, u32 w, int r565)
{
    const int sh[3] = { r565 ? 11 : 10, 5, 0 };
    const u32 fm[3] = { 31u, r565 ? 63u : 31u, 31u };
    for (u32 c = 0; c < 3; c++)
        if (!near8((v >> sh[c]) & fm[c], (w >> sh[c]) & fm[c], 1)) return 0;
    return 1;
}

static int check_fill(const u8* g, u32 W, u32 H, u32 pitch, u32 bpp, u32 cfmt, const u8* want)
{
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) {
            const u8* p = g + y * pitch + x * bpp;
            if (cfmt == 3u || cfmt == 1u || cfmt == 2u) {
                if (!packed16_near(((u32)p[0] << 8) | p[1], ((u32)want[0] << 8) | want[1], cfmt == 3u))
                    return 0;
            } else {
                for (u32 c = 0; c < bpp; c++) if (!near8(p[c], want[c], 1)) return 0;
            }
        }
    return 1;
}

/* -- clear + draw into native 16/8-bit targets, stored to guest memory. */
static void test_native_targets_engine(void)
{
    printf("-- native targets through the engine (clear, draw, store)\n");
    static const struct { u32 cfmt, bpp; const char* n; } k[] = {
        { 3u, 2u, "R5G6B5" }, { 2u, 2u, "X1R5G5B5_O" }, { 1u, 2u, "X1R5G5B5_Z" },
        { 9u, 1u, "B8" }, { 0xAu, 2u, "G8B8" }, { 8u, 4u, "A8R8G8B8" } };
    for (u32 i = 0; i < sizeof k / sizeof k[0]; i++) {
        engine_up();
        const u32 W = 32, H = 16, pitch = 64u * k[i].bpp, off = 0x40000u;
        set_surface(k[i].cfmt | (2u << 5), W, H, off, pitch);
        m(0x1D90, 0x80FF8040u);          /* A 0.5, R 1.0, G 0.5, B 0.25 */
        m(0x1D94, 0xF0u);
        store_color(off, pitch * H);
        const u8* g = guest_local(off);
        u8 want[4] = { 0, 0, 0, 0 };
        switch (k[i].cfmt) {
        /* RPCS3 get_rgb565_clear_color / get_a1rgb555: the clear word is a
         * raw copy, so a 16-bit target takes its low half (G << 8 | B); the
         * X1 bit reads 0 (_Z) or 1 (_O). */
        case 3u: want[0] = 0x80; want[1] = 0x40; break;
        case 1u: want[0] = 0x00; want[1] = 0x40; break;
        case 2u: want[0] = 0x80; want[1] = 0x40; break;
        case 9u: want[0] = 0x40; break;
        case 0xAu: want[0] = 0x80; want[1] = 0x40; break;
        default: want[0] = 0x80; want[1] = 0xFF; want[2] = 0x80; want[3] = 0x40; break;
        }
        CHECK(check_fill(g, W, H, pitch, k[i].bpp, k[i].cfmt, want),
              "%s clear stored: %02X %02X %02X %02X (want %02X %02X ..)", k[i].n, g[0], g[1], g[2], g[3],
              want[0], want[1]);

        const float col[4] = { 0.25f, 0.75f, 1.0f, 1.0f }, tc[4] = { 0, 0, 0, 1 };
        set_tri(0.0f, col, tc);
        draw();
        memset((u8*)g, 0xA5, pitch * H);
        store_color(off, pitch * H);
        switch (k[i].cfmt) {
        case 3u: { const u32 v = (8u << 11) | (47u << 5) | 31u; want[0] = (u8)(v >> 8); want[1] = (u8)v; break; }
        case 1u: case 2u: { const u32 v = (8u << 10) | (23u << 5) | 31u; want[0] = (u8)(v >> 8); want[1] = (u8)v; break; }
        case 9u: want[0] = 0xFF; break;
        case 0xAu: want[0] = 0xBF; want[1] = 0xFF; break;
        default: want[0] = 0xFF; want[1] = 0x40; want[2] = 0xBF; want[3] = 0xFF; break;
        }
        CHECK(check_fill(g, W, H, pitch, k[i].bpp, k[i].cfmt, want),
              "%s draw stored: %02X %02X %02X %02X (want %02X %02X ..)", k[i].n, g[0], g[1], g[2], g[3],
              want[0], want[1]);
    }
}

/* -- mirrored NV3089 blit through the engine: rsx_draw_engine_blit_flip. */
static void test_blit_flip_engine(void)
{
    printf("-- mirrored NV3089 blit through the engine\n");
    engine_up();
    const u32 W = 32, H = 32, off = 0x40000u, pitch = 128u;
    set_surface(8u | (2u << 5), W, H, off, pitch);
    m(0x08C4, (H / 2u) << 16);
    m(0x1D90, 0xFFFF0000u); m(0x1D94, 0xF0u);          /* rows 0..15 red */
    m(0x08C4, (H / 2u) | ((H / 2u) << 16));
    m(0x1D90, 0xFF00FF00u); m(0x1D94, 0xF0u);          /* rows 16..31 green */
    m(0x08C4, H << 16);
    const u32 doff = 0x300000u, dpitch = 128u;
    /* On a mirrored axis the source start is the first texel read (row H-1). */
    const int ok = rsx_draw_engine_blit_flip(RSX_LOCATION_LOCAL, off, pitch, 0, H - 1u, 1u << 20, 1u << 20,
                                             RSX_LOCATION_LOCAL, doff, dpitch, 0, 0, W, H, 0, 0, 4u, 2u);
    CHECK(ok, "the Y-mirrored blit ran on the GPU (%d)", ok);
    store_color(doff, dpitch * H);
    const u8* g = guest_local(doff);
    const u8* top = g + 2 * dpitch;
    const u8* bot = g + (H - 2) * dpitch;
    CHECK(top[1] == 0x00 && top[2] == 0xFF && bot[1] == 0xFF && bot[2] == 0x00,
          "mirrored: top row green (%02X %02X %02X %02X), bottom row red (%02X %02X %02X %02X)",
          top[0], top[1], top[2], top[3], bot[0], bot[1], bot[2], bot[3]);
}

/* -- programmable (signed) blending against a target snapshot. */
static void test_signed_blend_engine(void)
{
    printf("-- signed blend equations (programmable blending)\n");
    /* dst 0x80 (signed -128); src R 0xC0 (signed -64), G 0x40 (+64) */
    static const struct { u32 eq; u8 r, g; const char* n; } k[] = {
        { 0xF006u, 0x40, 0xC0, "ADD_SIGNED: signed s + d" },
        { 0xF005u, 0xC0, 0x40, "REVERSE_SUBTRACT_SIGNED: d - signed s" },
        { 0xF007u, 0x40, 0x00, "REVERSE_ADD_SIGNED: s + signed d" },
    };
    for (u32 i = 0; i < 3; i++) {
        engine_up();
        const u32 W = 16, H = 16, off = 0x40000u, pitch = 64u;
        set_surface(8u | (2u << 5), W, H, off, pitch);
        m(0x1D90, 0x80808080u); m(0x1D94, 0xF0u);
        m(0x0310, 1u);
        m(0x0314, 1u | (1u << 16));
        m(0x0318, 1u | (1u << 16));
        m(0x0320, k[i].eq | (0x8006u << 16));
        const float col[4] = { 192.0f / 255.0f, 64.0f / 255.0f, 0.0f, 0.25f }, tc[4] = { 0, 0, 0, 1 };
        set_tri(0.0f, col, tc);
        draw();
        m(0x0310, 0u);
        store_color(off, pitch * H);
        const u8* g = guest_local(off);
        CHECK(near8(g[1], k[i].r, 1) && near8(g[2], k[i].g, 1),
              "%s: R %02X (want %02X) G %02X (want %02X) A %02X", k[i].n, g[1], k[i].r, g[2], k[i].g, g[0]);
    }
}

/* -- X8R8G8B8_Z / _O blending on raw alpha with the shader-side blend. */
static void test_x8_blend_engine(void)
{
    printf("-- X8 _Z/_O blending (programmable blending)\n");
    for (u32 o = 0; o < 2; o++) {
        engine_up();
        const u32 W = 16, H = 16, off = 0x40000u, pitch = 64u;
        set_surface((o ? 5u : 4u) | (2u << 5), W, H, off, pitch);
        m(0x1D90, 0x00202020u); m(0x1D94, 0xF0u);
        m(0x0310, 1u);
        m(0x0314, 0x0302u | (1u << 16));            /* SRC_ALPHA, ONE */
        m(0x0318, 0x0303u | (0u << 16));            /* 1-SRC_ALPHA, ZERO */
        m(0x0320, 0x8006u | (0x8006u << 16));
        const float col[4] = { 1.0f, 1.0f, 1.0f, 0.5f }, tc[4] = { 0, 0, 0, 1 };
        set_tri(0.0f, col, tc);
        draw();
        m(0x0310, 0u);
        store_color(off, pitch * H);
        const u8* g = guest_local(off);
        CHECK(near8(g[1], 0x90, 2) && near8(g[2], 0x90, 2) && near8(g[3], 0x90, 2) &&
              g[0] == (o ? 0xFF : 0x00),
              "%s: SRC_ALPHA blend on raw alpha 0.5: %02X %02X %02X %02X", o ? "_O" : "_Z",
              g[0], g[1], g[2], g[3]);
    }
}

/* -- zeta store (combined depth+stencil readback) via a texture bind. */
static void test_zeta_store_engine(void)
{
    printf("-- zeta stored to guest memory (zeta_readback)\n");
    for (u32 zf = 1; zf <= 2; zf++) {
        for (u32 fl = 0; fl < 2; fl++) {
            engine_up();
            const u32 W = 16, H = 8, zoff = 0x140000u, zpitch = 128u;
            set_surface(8u | (zf << 5), W, H, 0x90000u, 64u);
            m(0x0198, DMA_LOCAL);
            m(0x0214, zoff);
            m(0x022C, zpitch);
            if (fl) m(0x03B0, 1u << 12);
            u8* guest = (u8*)guest_local(zoff);
            memset(guest, 0xA6, zpitch * H);
            m(0x1D8C, zf == 2 ? (0xFFFFFFu << 8) | 0x5Au : 0xFFFFu);
            m(0x1D94, 0x03u);
            m(0x0A74, 1u); m(0x0A70, 1u);
            m(0x0A6C, 0x0207u);                         /* ALWAYS */
            const float col[4] = { 1, 1, 1, 1 }, tc[4] = { 0, 0, 0, 1 };
            set_tri(-0.5f, col, tc);                    /* depth 0.25 */
            draw();
            m(0x0A74, 0u); m(0x0A70, 0u);
            /* Sampled with a texel size the zeta has not got (A8R8G8B8 over
             * Z16, R5G6B5 over Z24S8), which no depth reinterpretation
             * covers: the engine stores the zeta to guest memory first. */
            bind_tex0(RSX_LOCATION_LOCAL, zoff, 0, W, H, zpitch, 1);
            m(0x1A04, 1u | TEX_BORDER_COLOR | (2u << 4) | (((zf == 2 ? 0x84u : 0x85u) | 0x20u) << 8) | (1u << 16));
            use_fp(FP_TEX);
            draw();
            use_fp(FP_COL);
            const u32 bpp = zf == 2 ? 4u : 2u;
            u32 want;
            if (zf == 2) {
                if (fl) { const float d = 0.25f; u32 fb; memcpy(&fb, &d, 4); want = ((fb >> 7) & 0xFFFFFFu) << 8 | 0x5Au; }
                else want = (4194304u << 8) | 0x5Au;
            } else want = fl ? 0xE000u : 16384u;      /* e4m12 0.25: exponent 14, mantissa 0 */
            int ok = 1; u32 got = 0;
            for (u32 y = 0; y < H; y++)
                for (u32 x = 0; x < W; x++) {
                    const u8* q = guest + y * zpitch + x * bpp;
                    u32 v = 0;
                    for (u32 b = 0; b < bpp; b++) v = (v << 8) | q[b];
                    const int good = zf == 2 ? (near8(v >> 8, want >> 8, 1) && (v & 0xFF) == (want & 0xFF))
                                             : near8(v, want, 1);
                    if (!good && ok) { got = v; ok = 0; }
                }
            if (ok) for (u32 b = 0; b < bpp; b++) got = (got << 8) | guest[b];
            CHECK(ok, "Z%s%s stored: %08X (want %08X)", zf == 2 ? "24S8" : "16", fl ? " float" : "", got, want);
            CHECK(guest[W * bpp] == 0xA6, "row padding untouched");
        }
    }
}

/* -- cube / volume texture unwrapped from a render target (texture_copy_from_color). */
static void test_unwrap_engine(void)
{
    printf("-- cube / volume unwrapped from a render target, sampled\n");
    static const u32 face_col[6] = { 0xFF102030u, 0xFF405060u, 0xFF708090u,
                                     0xFFA0B0C0u, 0xFFD0E0F0u, 0xFF0A0B0Cu };
    for (u32 kind = 0; kind < 2; kind++) {
        engine_up();
        const u32 src = 0x40000u, W = 16, H = 96, pitch = 64u;
        set_surface(8u | (2u << 5), W, H, src, pitch);
        const u32 n = kind == 0 ? 6u : 4u;
        for (u32 f = 0; f < n; f++) {
            m(0x08C4, (f * 16u) | (16u << 16));
            m(0x1D90, face_col[f]); m(0x1D94, 0xF0u);
        }
        m(0x08C4, H << 16);
        if (kind == 0) {
            store_color(src, pitch * H);
            int bands = 1;
            for (u32 f = 0; f < n; f++) {
                const u8* p = guest_local(src + (f * 16u + 8u) * pitch + 32u);
                if (!near8(p[1], (face_col[f] >> 16) & 0xFF, 1) || !near8(p[3], face_col[f] & 0xFF, 1)) {
                    printf("   rt band %u: %02X %02X %02X %02X\n", f, p[0], p[1], p[2], p[3]);
                    bands = 0;
                }
            }
            CHECK(bands, "16x96 target holds the six scissor-cleared bands");
        }
        const u32 dst = 0x80000u;
        set_surface(8u | (2u << 5), 16, 16, dst, 64u);
        if (kind == 0) bind_tex0(RSX_LOCATION_LOCAL, src, (1u << 2), 16, 16, pitch, 1);
        else {
            bind_tex0(RSX_LOCATION_LOCAL, src, 0, 16, 16, pitch, 4);
            m(0x1A04, 1u | TEX_BORDER_COLOR | (3u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
        }
        use_fp(FP_TEX);
        int ok = 1;
        for (u32 f = 0; f < n; f++) {
            static const float dir[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
                                             { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
            float tc[4] = { 0.5f, 0.5f, ((float)f + 0.5f) / (float)n, 1.0f };
            if (kind == 0) { tc[0] = dir[f][0]; tc[1] = dir[f][1]; tc[2] = dir[f][2]; }
            const float col[4] = { 0, 0, 0, 1 };
            set_tri(0.0f, col, tc);
            draw();
            store_color(dst, 64u * 16u);
            const u8* g = guest_local(dst + 8 * 64 + 8 * 4);
            const u32 v = (u32)g[0] << 24 | (u32)g[1] << 16 | (u32)g[2] << 8 | g[3];
            const int match = near8(g[1], (face_col[f] >> 16) & 0xFF, 1) &&
                              near8(g[2], (face_col[f] >> 8) & 0xFF, 1) &&
                              near8(g[3], face_col[f] & 0xFF, 1);
            if (!match) { printf("   %s %u: got %08X want %08X\n", kind ? "slice" : "face", f, v, face_col[f]); ok = 0; }
        }
        CHECK(ok, "%s: every %s samples its own region of the target", kind ? "volume" : "cube",
              kind ? "slice" : "face");
        use_fp(FP_COL);
    }
}

/* -- RT mip gather: levels 0 and 1 both targets; min LOD picks the level. */
static void test_mip_gather_engine(void)
{
    printf("-- render-target mip chain gathered\n");
    engine_up();
    const u32 base = 0x40000u, pitch = 256u;
    set_surface(8u | (2u << 5), 64, 64, base, pitch);
    m(0x1D90, 0xFFFF0000u); m(0x1D94, 0xF0u);                 /* level 0 red */
    set_surface(8u | (2u << 5), 32, 32, base + pitch * 64u, pitch);
    m(0x1D90, 0xFF00FF00u); m(0x1D94, 0xF0u);                 /* level 1 green */
    set_surface(8u | (2u << 5), 16, 16, 0x90000u, 64u);
    bind_tex0(RSX_LOCATION_LOCAL, base, 0, 64, 64, pitch, 1);
    m(0x1A04, 1u | TEX_BORDER_COLOR | (2u << 4) | ((0x85u | 0x20u) << 8) | (2u << 16));
    m(0x1A14, (3u << 16) | (1u << 24));                       /* nearest, nearest mip */
    use_fp(FP_TEX);
    const float col[4] = { 0, 0, 0, 1 }, tc[4] = { 0.5f, 0.5f, 0, 1 };
    set_tri(0.0f, col, tc);
    m(0x1A0C, 0x80000000u | (512u << 7) | (0u << 19));        /* LOD 0..2 */
    draw();
    store_color(0x90000u, 64u * 16u);
    const u8* g = guest_local(0x90000u + 8 * 64 + 32);
    CHECK(g[1] == 0xFF && g[2] == 0x00, "min LOD 0: level 0 (red) (%02X %02X %02X %02X)", g[0], g[1], g[2], g[3]);
    m(0x1A0C, 0x80000000u | (512u << 7) | (256u << 19));      /* min LOD 1 */
    draw();
    store_color(0x90000u, 64u * 16u);
    CHECK(g[1] == 0x00 && g[2] == 0xFF, "min LOD 1: level 1 (green) (%02X %02X %02X %02X)", g[0], g[1], g[2], g[3]);
    m(0x1A04, 1u | TEX_BORDER_COLOR | (2u << 4) | ((0x85u | 0x20u) << 8) | (3u << 16));
    m(0x1A0C, 0x80000000u | (512u << 7) | (512u << 19));      /* min LOD 2 of 2 target levels */
    draw();
    store_color(0x90000u, 64u * 16u);
    CHECK(g[1] == 0x00 && g[2] == 0xFF, "min LOD 2 of a two-target chain clamps to level 1 (%02X %02X)", g[1], g[2]);
    use_fp(FP_COL);
}

/* -- shadow compare: a depth target sampled with a compare function. */
static void test_shadow_engine(void)
{
    printf("-- shadow compare against a depth target\n");
    for (u32 fl = 0; fl < 2; fl++) {
        engine_up();
        const u32 W = 16, H = 16, zoff = 0x140000u, zpitch = 64u;
        set_surface(8u | (2u << 5), W, H, 0x90000u, 64u);
        m(0x0198, DMA_LOCAL); m(0x0214, zoff); m(0x022C, zpitch);
        if (fl) m(0x03B0, 1u << 12);
        m(0x1D8C, 0xFFFFFF00u); m(0x1D94, 0x03u);
        m(0x0A74, 1u); m(0x0A70, 1u); m(0x0A6C, 0x0207u);
        const float col[4] = { 1, 1, 1, 1 };
        const float tc[4] = { 0.5f, 0.5f, 0.5f, 1 };
        set_tri(-0.5f, col, tc);                    /* depth 0.25 */
        draw();
        m(0x0A74, 0u); m(0x0A70, 0u);
        /* Draw elsewhere, sampling the zeta as DEPTH24_D8 / _FLOAT with a compare. */
        set_surface(8u | (2u << 5), W, H, 0xA0000u, 64u);
        bind_tex0(RSX_LOCATION_LOCAL, zoff, 0, W, H, zpitch, 1);
        m(0x1A04, 1u | TEX_BORDER_COLOR | (2u << 4) | (((fl ? 0x91u : 0x90u) | 0x20u) << 8) | (1u << 16));
        use_fp(FP_TEX);
        static const struct { u32 zf; u8 want; const char* n; } c[] = {
            /* RSX ZFUNC runs the other way round from the host compare (RPCS3
             * get_compare_func(zfunc, reverse_direction = true)): texel FUNC ref. */
            { 4u, 0x00, "GREATER (0.25 > 0.5)" }, { 1u, 0xFF, "LESS (0.25 < 0.5)" } };
        for (u32 i = 0; i < 2; i++) {
            m(0x1A08, 0x00030303u | (c[i].zf << 28));
            draw();
            store_color(0xA0000u, 64u * H);
            const u8* g = guest_local(0xA0000u + 8 * 64 + 32);
            CHECK(near8(g[1], c[i].want, 1), "%s %s: R %02X (want %02X)", fl ? "float" : "D24",
                  c[i].n, g[1], c[i].want);
        }
        use_fp(FP_COL);
    }
}

/* -- vertex textures: cube and 3D, sampled by TXL in the vertex program. */
static void test_vertex_texture_engine(void)
{
    printf("-- vertex texture cube / volume\n");
    for (u32 kind = 0; kind < 2; kind++) {
        engine_up();
        set_surface(8u | (2u << 5), 16, 16, 0x90000u, 64u);
        load_vp(1);
        /* W32_Z32_Y32_X32_FLOAT (0x9B) 1x1 texels: 6 faces / 2 slices. */
        u8* t = vm_base + TEX_MAIN;
        const u32 n = kind == 0 ? 6u : 2u;
        for (u32 f = 0; f < n; f++)
            for (u32 c = 0; c < 4; c++)
                put_bef(t + f * 16u + c * 4u, c == 3 ? 1.0f : (float)(f + 1) / 8.0f * (float)(c + 1) / 3.0f);
        rsx_draw_engine_note_guest_write(TEX_MAIN, n * 16u);
        m(0x0900, TEX_MAIN);
        m(0x0904, 2u | TEX_BORDER_COLOR | (kind == 0 ? (1u << 2) : 0u) | ((kind == 0 ? 2u : 3u) << 4) |
                  ((0x9Bu | 0x20u) << 8) | (1u << 16));
        m(0x0908, 0x00030303u);
        m(0x090C, 0x80000000u);
        m(0x0910, ((kind == 0 ? 1u : 2u) << 20) | 16u);
        m(0x0914, 0u);
        m(0x0918, (1u << 16) | 1u);
        int ok = 1;
        for (u32 f = 0; f < n; f++) {
            static const float dir[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
                                             { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
            float tc[4] = { 0.5f, 0.5f, ((float)f + 0.5f) / (float)n, 0.0f };
            if (kind == 0) { tc[0] = dir[f][0]; tc[1] = dir[f][1]; tc[2] = dir[f][2]; }
            const float col[4] = { 0, 0, 0, 1 };
            set_tri(0.0f, col, tc);
            draw();
            store_color(0x90000u, 64u * 16u);
            const u8* g = guest_local(0x90000u + 8 * 64 + 32);
            const u8 er = (u8)((float)(f + 1) / 8.0f * 1.0f / 3.0f * 255.0f + 0.5f);
            const u8 eg = (u8)((float)(f + 1) / 8.0f * 2.0f / 3.0f * 255.0f + 0.5f);
            if (!near8(g[1], er, 1) || !near8(g[2], eg, 1)) {
                printf("   %s %u: got %02X %02X want %02X %02X\n", kind ? "slice" : "face", f, g[1], g[2], er, eg);
                ok = 0;
            }
        }
        CHECK(ok, "vertex %s texture: every %s sampled", kind ? "3D" : "cube", kind ? "slice" : "face");
        m(0x090C, 0u);
        load_vp(0);
    }
}

/* -- many submissions with texture / sampler churn (perf caches, slot waits). */
static void test_submit_churn_engine(void)
{
    printf("-- submit churn (descriptor / sampler caches, 256 submits a slot)\n");
    engine_up();
    set_surface(8u | (2u << 5), 16, 16, 0x90000u, 64u);
    u8* t = vm_base + TEX_MAIN + 0x1000;
    for (u32 i = 0; i < 4; i++)
        for (u32 p = 0; p < 16; p++) put_be32(t + i * 0x100 + p * 4, 0xFF000000u | ((0x40u * i) << 16) | 0x80u);
    rsx_draw_engine_note_guest_write(TEX_MAIN + 0x1000, 0x400);
    use_fp(FP_TEX);
    const float col[4] = { 0, 0, 0, 1 }, tc[4] = { 0.5f, 0.5f, 0, 1 };
    set_tri(0.0f, col, tc);
    for (u32 i = 0; i < 700; i++) {
        bind_tex0(RSX_LOCATION_MAIN, TEX_MAIN + 0x1000 + (i & 3u) * 0x100, 0, 4, 4, 16, 1);
        m(0x1A14, ((((i >> 2) & 1u) ? 2u : 1u) << 16) | (1u << 24));
        draw();
        be->submit_and_wait(U, RSX_BE_FLUSH_GUEST_REFERENCE);
    }
    store_color(0x90000u, 64u * 16u);
    const u8* g = guest_local(0x90000u + 8 * 64 + 32);
    CHECK(g[1] == 0xC0 && g[3] == 0x80, "the last of 700 submissions shows texture 3 (%02X %02X %02X %02X)",
          g[0], g[1], g[2], g[3]);
    use_fp(FP_COL);
}

/* ======================================================================== */

static int want_test(const char* name)
{
    const char* o = getenv("VKREAL_ONLY");
    return !o || !*o || strstr(name, o) != NULL;
}

#define RUN(fn) do { if (want_test(#fn)) { const u32 v0 = rsx_vulkan_backend_validation_messages(); fn(); \
    const u32 dv = rsx_vulkan_backend_validation_messages() - v0; \
    if (dv) printf("   (%u validation messages in %s)\n", dv, #fn); } } while (0)

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!getenv("PS3RECOMP_VK_VALIDATION")) env_set("PS3RECOMP_VK_VALIDATION", "sync");
    env_set("PS3RECOMP_VK_GUEST_PROGRAMS", "1");
    env_set("PS3RECOMP_RSX_ENGINE", "dispatch");
    env_set("RSX_VK_PCACHE", "0");
    env_set("RSX_VK_VARIANT_CACHE", "0");
    env_set("RSX_ENGINE_PROGRAM_CACHE", "0");
    env_set("RSX_VK_ASYNC_PIPELINES", "0");
    /* The test thread is the engine's thread and pokes guarded guest memory
     * directly as "the guest": that must not store the surface first. */
    env_set("RSX_ENG_WCB_SELF_FLUSH", "0");
#ifdef _WIN32
    vm_base = (u8*)VirtualAlloc(NULL, GUEST_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    vm_base = (u8*)calloc(1, GUEST_BYTES);
#endif
    if (!vm_base) { printf("[FAIL] guest memory\n"); return 1; }
    ppu_vm_size = GUEST_BYTES;

    if (rsx_vulkan_backend_init(1280, 720, "vkreal") != 0) {
        printf("SKIP: no Vulkan device (rsx_vulkan_backend_init failed)\n");
        return 0;
    }
    be = (const rsx_draw_backend*)rsx_vulkan_backend_engine_backend();
    if (!be || !rsx_draw_engine_enabled()) {
        printf("SKIP: the Vulkan engine backend is not active\n");
        rsx_vulkan_backend_shutdown();
        return 0;
    }
    U = be->user;
    const u32 v_init = rsx_vulkan_backend_validation_messages();

    RUN(test_native_formats_direct);
    RUN(test_blit_flip_direct);
    RUN(test_zeta_direct);
    RUN(test_tcfc_direct);
    RUN(test_native_targets_engine);
    RUN(test_blit_flip_engine);
    RUN(test_signed_blend_engine);
    RUN(test_x8_blend_engine);
    RUN(test_zeta_store_engine);
    RUN(test_unwrap_engine);
    RUN(test_mip_gather_engine);
    RUN(test_shadow_engine);
    RUN(test_vertex_texture_engine);
    RUN(test_submit_churn_engine);

    engine_up();                    /* one live engine for the backend's own shutdown */
    rsx_vulkan_backend_shutdown();
    const u32 v = rsx_vulkan_backend_validation_messages();
    printf("validation messages: %u (%u during init)\n", v, v_init);
    if (v) g_fail++;
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
