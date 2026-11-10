/* NV3089 blit: the source origin (IMAGE_IN_POINT, 12.4 fixed point per axis) and the engine write-back before the copy.
 *
 * With the draw engine on, the scene a title scales with NV3089 exists only on the GPU, so nv3089_blit asks the
 * engine to write the source rectangle back to guest memory (nv3089_engine_read) before it copies from there. The
 * steps (DS_DX, DT_DY) have 20 fractional bits, so the origin enters the copy as origin << 16; it used to be << 8, which
 * counts it 256 times too small: a one texel origin (in_uv 0x00100010, a title's 2:1 resolve tiles carry it) read
 * column 0 and row 0 instead of 1 and the write-back, which took the origin as u0 >> 4, did not cover them (stale guest
 * memory: the first column and row of every tile came out black).
 *
 * The test plays the engine: guest_read stores into the rectangle it is asked for a pixel that names its own source
 * position, and nothing else. Destination pixel (x, y) must then be the source pixel at (origin + x * scale,
 * origin + y * scale).
 *
 * clang -std=gnu17 -I include -I libs/video libs/video/tests/test_nv3089_engine_read.c -Wl,-dead_strip -pthread
 *   -o /tmp/test_nv3089_engine_read && /tmp/test_nv3089_engine_read
 *   (gcc: -ffunction-sections -fdata-sections -Wl,--gc-sections)
 */
#include "../cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
/* The rest of the walker's world: no graphics backend, no lifted program. */
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
int  rsx_process_method(rsx_state* state, u32 method, u32 data) { (void)state; (void)method; (void)data; return 0; }
int  rsx_draw_engine_enabled(void) { return 1; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
void rsx_draw_engine_note_flip(u32 buffer_id) { (void)buffer_id; }
/* guest_read is defined below: it plays the engine's part. */
void rsx_draw_engine_guest_wrote(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_range_read(u32 l, u32 o, u32 n) { (void)l; (void)o; (void)n; }
void rsx_draw_engine_range_wrote(u32 l, u32 o, u32 n) { (void)l; (void)o; (void)n; }
void rsx_draw_engine_range_will_write(u32 l, u32 o, u32 n) { (void)l; (void)o; (void)n; }
void rsx_draw_engine_range_cpu_wrote(u32 l, u32 o, u32 n) { (void)l; (void)o; (void)n; }
int rsx_draw_engine_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g_, u32 h, u32 i, u32 j,
                         u32 k, u32 l, u32 m, u32 n, int lin, u32 iwh)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g_; (void)h; (void)i; (void)j;
  (void)k; (void)l; (void)m; (void)n; (void)lin; (void)iwh; return 0; }
int rsx_draw_engine_blit_bpp(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g_, u32 h, u32 i,
                             u32 j, u32 k, u32 l, u32 m, u32 n, int lin, u32 iwh, u32 bpp)
{ (void)bpp; return rsx_draw_engine_blit(a, b, c, d, e, f, g_, h, i, j, k, l, m, n, lin, iwh); }
int rsx_draw_engine_blit_flip(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g_, u32 h, u32 i,
                              u32 j, u32 k, u32 l, u32 m, u32 n, int lin, u32 iwh, u32 bpp, u32 flip)
{ return flip ? 0 : rsx_draw_engine_blit_bpp(a, b, c, d, e, f, g_, h, i, j, k, l, m, n, lin, iwh, bpp); }
int rsx_draw_engine_blit_swizzled(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g_, u32 h, u32 i,
                                  u32 j, u32 k, int lin)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g_; (void)h; (void)i; (void)j;
  (void)k; (void)lin; return 0; }
int  rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_method(u32 method, u32 arg) { (void)method; (void)arg; }
int  rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
int  rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h,
                                u32 i, u32 j, u32 k, u32 l)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; (void)l; return 0; }


#define LOCAL_BASE 0x00200000u      /* where "local memory" sits in the test's guest VM */
#define SRC_OFF    0x00000000u
#define DST_OFF    0x00100000u

/* A non-zero pixel that names the source position it stands for. */
static u32 src_pixel(u32 col, u32 row) { return 0xFF000000u | (row << 12) | col; }

static int s_reads;

/* What the engine does for a transfer: the pixels of the surface in this rectangle, as 32-bit guest words. */
void rsx_draw_engine_guest_read(u32 loc, u32 off, u32 pitch, u32 w, u32 h)
{
    s_reads++;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
            vm_write32(cellGcmResolveLocated(loc == 0u /* RSX_LOCATION_LOCAL */, off + y * pitch + x * 4u),
                       src_pixel((off - SRC_OFF) % pitch / 4u + x, (off - SRC_OFF) / pitch + y));
}

static int fails;

/* One blit of `out` x `out` pixels from a source `scale` times larger, its origin at (uv >> 4) texels. */
static void blit_case(const char* what, u32 out, u32 scale, u32 uv, u32 interp)
{
    const u32 src_w = out * scale + (uv >> 4) + 8u, src_pitch = src_w * 4u, dst_pitch = out * 4u;
    memset(vm_base + LOCAL_BASE + SRC_OFF, 0, (size_t)src_pitch * src_w);      /* stale guest memory */
    memset(vm_base + LOCAL_BASE + DST_OFF, 0, (size_t)dst_pitch * out);
    s_reads = 0;

    memset(&s_nv3089, 0, sizeof s_nv3089);
    memset(&s_gcm2d, 0, sizeof s_gcm2d);
    s_nv3089.fmt     = 3u;                                  /* A8R8G8B8 in the scale-format enum */
    s_nv3089.out_sz  = (out << 16) | out;
    s_nv3089.in_sz   = ((out * scale) << 16) | (out * scale);
    s_nv3089.in_fmt  = (interp << 24) | (2u << 16) | src_pitch;   /* corner origin, interpolator, pitch */
    s_nv3089.in_off  = SRC_OFF;
    s_nv3089.in_uv   = (uv << 16) | uv;
    s_nv3089.ds_dx   = s_nv3089.dt_dy = scale << 20;         /* 12.20 */
    s_gcm2d.pitch      = dst_pitch << 16;
    s_gcm2d.dst_offset = DST_OFF;
    nv3089_blit();

    const u32 origin = uv >> 4;                                 /* texels */
    u32 wrong = 0, zero = 0;
    for (u32 y = 0; y < out; y++)
        for (u32 x = 0; x < out; x++) {
            const u32 got = vm_read32(LOCAL_BASE + DST_OFF + y * dst_pitch + x * 4u);
            if (got == 0) zero++;
            /* RPCS3's blit samples the destination pixel centre: floor((x + 0.5) * scale). */
            if (got != src_pixel(origin + (2u * x + 1u) * scale / 2u, origin + (2u * y + 1u) * scale / 2u)) wrong++;
        }
    const int ok = s_reads > 0 && wrong == 0;
    printf("[%s] %s: %u of %u destination pixels are not the expected source pixel (%u of them never got engine data)\n",
           ok ? "PASS" : "FAIL", what, wrong, out * out, zero);
    fails += !ok;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = LOCAL_BASE + 0x00400000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;
    ppu_hle_inject_base = vm_size;
    s_config.localAddress = LOCAL_BASE;

    blit_case("2:1 resolve, no source offset", 16, 2, 0x00, 0);
    blit_case("2:1 resolve, one texel source offset (in_uv 0x10)", 16, 2, 0x10, 0);
    blit_case("2:1 resolve, 3 texel source offset", 16, 2, 0x30, 0);
    blit_case("1:1 copy, 20 texel source offset", 16, 1, 0x140, 0);
    blit_case("1:1 copy, one texel source offset", 16, 1, 0x10, 0);
    blit_case("4:1 resolve, one texel source offset", 16, 4, 0x10, 0);

    /* RPCS3 decode_transfer_registers geometry. */
    { nv3089_geom g; int ok;
      /* clip point kept: destination at the output point, scaled image read from the clip point */
      ok = nv3089_geom_calc(0x00010000u, 0x00080008u, 0x00020003u, 0x00040004u, 0, &g) &&
           g.x0 == 0 && g.y0 == 1 && g.w == 4 && g.h == 4 && g.ix0 == 3 && g.iy0 == 2;
      printf("[%s] geom: clip point reprojected onto the source\n", ok ? "PASS" : "FAIL"); fails += !ok;
      /* out_x + clip_x + clip_w > out_w resets clip_x (only); y kept */
      ok = nv3089_geom_calc(0x00000002u, 0x00080008u, 0x00010003u, 0x00040004u, 0, &g) &&
           g.x0 == 2 && g.ix0 == 0 && g.iy0 == 1 && g.w == 4;
      printf("[%s] geom: clip x reset when it overruns the output\n", ok ? "PASS" : "FAIL"); fails += !ok;
      /* clip size clamped to output size; zero clip is a NOP */
      ok = nv3089_geom_calc(0, 0x00100020u, 0, 0x01000100u, 0, &g) && g.w == 0x20 && g.h == 0x10 &&
           !nv3089_geom_calc(0, 0x00100020u, 0, 0x00100000u, 0, &g);
      printf("[%s] geom: clip clamped to output, zero clip NOP\n", ok ? "PASS" : "FAIL"); fails += !ok;
      /* Tornado Outbreak's bottom band: both rules write 208 rows at y=512 from texel row 0 */
      { nv3089_geom a, b;
        ok = nv3089_geom_calc(0x02000000u, 0x02000200u, 0x02000000u, 0x00D00500u, 0, &a) &&
             nv3089_geom_calc(0x02000000u, 0x02000200u, 0x02000000u, 0x00D00500u, 1, &b) &&
             a.y0 == 512 && a.h == 208 && a.iy0 == 0 && b.y0 == 512 && b.h == 208 && b.iy0 == 0; }
      printf("[%s] geom: band clip agrees with the legacy rule\n", ok ? "PASS" : "FAIL"); fails += !ok; }

    /* End to end: an 8x8 output at (0,1) clipped to the 4x4 block at (3,2) of the image. */
    { const u32 sp = 32u * 4u, dp = 8u * 4u;
      memset(vm_base + LOCAL_BASE + SRC_OFF, 0, sp * 32u);
      for (u32 i = 0; i < 16u * 8u; i++) vm_write32(LOCAL_BASE + DST_OFF + i * 4u, 0xDEADBEEFu);
      memset(&s_nv3089, 0, sizeof s_nv3089); memset(&s_gcm2d, 0, sizeof s_gcm2d);
      s_nv3089.fmt = 3u; s_nv3089.out_pt = 0x00010000u; s_nv3089.out_sz = 0x00080008u;
      s_nv3089.clip_pt = 0x00020003u; s_nv3089.clip_sz = 0x00040004u;
      s_nv3089.in_sz = 0x00200020u; s_nv3089.in_fmt = sp; s_nv3089.in_off = SRC_OFF;
      s_nv3089.ds_dx = s_nv3089.dt_dy = 0x100000u;
      s_gcm2d.pitch = dp << 16; s_gcm2d.dst_offset = DST_OFF; s_gcm2d.color_fmt = 0xAu;
      nv3089_image_in();
      u32 bad = 0;
      for (u32 y = 0; y < 16u; y++)
          for (u32 x = 0; x < 8u; x++) {
              const u32 got = vm_read32(LOCAL_BASE + DST_OFF + y * dp + x * 4u);
              const int in = x < 4u && y >= 1u && y < 5u;
              if (in ? got != src_pixel(3u + x, 2u + (y - 1u)) : got != 0xDEADBEEFu) bad++;
          }
      printf("[%s] clipped blit: %u wrong destination pixels\n", bad ? "FAIL" : "PASS", bad); fails += bad != 0; }

    /* NV308A A8R8G8B8 -> R5G6B5 is RPCS3's (c * 32) / 255, overflow included. */
    { const int ok = nv308a_to_r5g6b5(0xFFFFFFFFu) == 0x0820u && nv308a_to_r5g6b5(0x00808080u) == 0x8410u &&
                     nv308a_to_r5g6b5(0) == 0;
      printf("[%s] nv308a r5g6b5 conversion\n", ok ? "PASS" : "FAIL"); fails += !ok; }

    /* R2's RPCS3 patch: a source that scales down to 0 columns (16 * DS_DX 0x8000 >> 20 == 0) is
     * dropped when local memory is involved; 1 column survives; IO-to-IO still copies. */
    { const u32 dp = 16u * 4u;
      u32 written[3] = { 0, 0, 0 };
      const u32 dsdx[3] = { 0x8000u, 0x10000u, 0x8000u }, dma[3] = { 0xFEED0000u, 0xFEED0000u, 0xFEED0001u };
      for (int c = 0; c < 3; c++) {
          for (u32 i = 0; i < 64u * 64u; i++) vm_write32(LOCAL_BASE + SRC_OFF + i * 4u, 0x11223344u);
          for (u32 i = 0; i < 16u * 16u; i++) vm_write32(LOCAL_BASE + DST_OFF + i * 4u, 0u);
          memset(&s_nv3089, 0, sizeof s_nv3089); memset(&s_gcm2d, 0, sizeof s_gcm2d);
          s_nv3089.fmt = 3u; s_nv3089.out_sz = 0x00100010u; s_nv3089.clip_sz = 0x00100010u;
          s_nv3089.in_sz = 0x00400040u; s_nv3089.in_fmt = 64u * 4u; s_nv3089.in_off = SRC_OFF;
          s_nv3089.ds_dx = dsdx[c]; s_nv3089.dt_dy = 0x100000u;
          s_nv3089.src_dma = s_gcm2d.dst_dma = dma[c];
          s_gcm2d.pitch = dp << 16; s_gcm2d.dst_offset = DST_OFF;
          if (dma[c] == 0xFEED0001u) {   /* IO: map offset 0 -> LOCAL_BASE */
              memset(s_io_address_table, 0xFF, sizeof s_io_address_table);
              populate_offset_table(LOCAL_BASE, 0, 0x00200000u);
          }
          nv3089_blit();
          for (u32 i = 0; i < 16u * 16u; i++) written[c] += vm_read32(LOCAL_BASE + DST_OFF + i * 4u) != 0u;
      }
      const int ok = written[0] == 0u && written[1] != 0u && written[2] != 0u;
      printf("[%s] degenerate blit dropped (wrote %u / %u / %u pixels)\n", ok ? "PASS" : "FAIL",
             written[0], written[1], written[2]);
      fails += !ok; }

    /* NV3089 R5G6B5 <-> A8R8G8B8 */
    { const int ok = nv3089_r5g6b5_to_argb(0xFFFFu) == 0xFFFFFFFFu && nv3089_r5g6b5_to_argb(0xF800u) == 0xFFFF0000u &&
                     nv3089_argb_to_r5g6b5(0xFF00FF00u) == 0x07E0u;
      printf("[%s] nv3089 565 conversions\n", ok ? "PASS" : "FAIL"); fails += !ok; }
    printf("[nv3089_engine_read] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
