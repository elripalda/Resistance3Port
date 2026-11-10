/* RPCS3 parity of the GCM method handling (sweep2 M2-01, M2-02, M2-03, M2-04, M2-07):
 * zcull bind / ZCULL surface state, invalid-method recovery, NV3089 into MAIN-memory tiles,
 * NV3089 transfer validation. */
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
int  rsx_draw_engine_enabled(void) { return 0; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
void rsx_draw_engine_note_flip(u32 buffer_id) { (void)buffer_id; }
void rsx_draw_engine_guest_read(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_guest_wrote(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_range_read(u32 l, u32 o, u32 n) { (void)l; (void)o; (void)n; }
void rsx_draw_engine_range_wrote(u32 l, u32 o, u32 n) { (void)l; (void)o; (void)n; }
void rsx_draw_engine_range_will_write(u32 l, u32 o, u32 n) { (void)l; (void)o; (void)n; }
void rsx_draw_engine_range_cpu_wrote(u32 l, u32 o, u32 n) { (void)l; (void)o; (void)n; }
int rsx_draw_engine_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g_, u32 h, u32 i, u32 j,
                         u32 k, u32 l, u32 m, u32 n, int lin, u32 iwh)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g_; (void)h; (void)i; (void)j;
  (void)k; (void)l; (void)m; (void)n; (void)lin; (void)iwh; return 0; }
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

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00400000u
#define CMD_SIZE  0x00080000u
#define LOCAL_EA  0x01000000u

static uint32_t g_heap = 0x00800000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void host_w32(u32 ea, u32 v) { vm_write32(ea, v); }

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

/* M2-01: SetZcull / BindZcull (12 args) bind the region and fill the guest
 * CellGcmZcullInfo mirror (RPCS3 GcmZcullInfo::pack, sys_rsx 0x301). */
static void test_zcull_bind(void)
{
    check(cellGcmSetZcull(0, 0x00100000u, 1280, 720, 0x1000, 2, 0, 0, 0, 0, 0, 0) == CELL_OK &&
          s_zcull[0].bound == 1, "SetZcull binds the region");
    const u32 ea = s_zculls_guest_ea;
    check(ea && vm_read32(ea + 0) == (1u | (2u << 4)) &&
          vm_read32(ea + 4) == (((1280u >> 6) << 22) | ((720u >> 6) << 6)) &&
          vm_read32(ea + 8) == 0x1000u && vm_read32(ea + 12) == 0x00100000u &&
          vm_read32(ea + 20) == 0x00202000u, "packed CellGcmZcullInfo written to the guest table");
    check(cellGcmUnbindZcull(1) == CELL_OK && s_zcull[1].bound == 0 &&
          cellGcmBindZcull(1, 0x00200000u, 640, 360, 0, 1, 0, 0, 0, 0, 0, 0) == CELL_OK &&
          s_zcull[1].bound == 1 && s_zcull[1].zFormat == 1u && s_zcull[1].width == 640u,
          "BindZcull takes the twelve SetZcull arguments and binds");
    check(cellGcmSetZcull(2, 0x00100000u, 0, 720, 0, 2, 0, 0, 0, 0, 0, 0) == CELL_EINVAL &&
          s_zcull[2].bound == 0, "an empty cull range is CELL_EINVAL and stays unbound");
    check((u32)(uintptr_t)cellGcmGetZcullInfo(0) == s_zculls_guest_ea, "GetZcullInfo returns the guest table");
}

static void zc(u32 m, u32 v) { (void)gcm_zc_pre(m, v); }

/* M2-01/M2-02: the zcull unit is active for a draw whose zeta surface is the
 * bound region (format, AA and offset match) and actually uses depth. */
static void test_zcull_surface(void)
{
    zc(0x0198u, 0xFEED0000u);
    zc(0x0208u, 0x0145u);                 /* Z24S8, linear, A8R8G8B8, no AA */
    zc(0x0214u, 0x00100000u);
    zc(0x022Cu, 0x1400u);
    zc(0x0200u, 1280u << 16);
    zc(0x08C0u, 1280u << 16);
    zc(0x0A6Cu, 0x201u);
    zc(0x0A70u, 1u);
    zc(0x0A74u, 1u);
    zc(0x1808u, 5u); zc(0x1808u, 0u);
    check(s_zc.surface_active == 1, "draw with depth test on a bound zeta: zcull surface active");
    zc(0x1D94u, 0xF0u);
    check(s_zc.surface_active == 0, "colour-only clear: depth unused, zcull inactive");
    zc(0x1D94u, 0xF3u);
    check(s_zc.surface_active == 1, "colour + depth clear: active again");
    zc(0x1808u, 5u); zc(0x1808u, 0u);
    zc(0x0A74u, 0u);
    zc(0x1808u, 5u); zc(0x1808u, 0u);
    check(s_zc.surface_active == 1, "depth test off between draws does not rebind (RPCS3 on_framebuffer_options_changed)");
    zc(0x1D94u, 0xF0u);
    zc(0x1808u, 5u); zc(0x1808u, 0u);
    check(s_zc.surface_active == 0, "rebound for a draw without depth test: inactive");
    zc(0x0A74u, 1u);
    zc(0x0214u, 0x00300000u);
    zc(0x1808u, 5u); zc(0x1808u, 0u);
    check(s_zc.surface_active == 0, "zeta offset that is not a bound region: inactive");
    zc(0x0214u, 0x00100000u);
    zc(0x0208u, 0x0125u);                 /* Z16 does not match the Z24S8 region */
    zc(0x1808u, 5u); zc(0x1808u, 0u);
    check(s_zc.surface_active == 0, "zeta format that differs from the region: inactive");
    zc(0x0208u, 0x0145u);
    zc(0x1808u, 5u); zc(0x1808u, 0u);
    check(s_zc.surface_active == 1, "matching again: active");
    (void)cellGcmUnbindZcull(0);
    zc(0x1808u, 5u); zc(0x1808u, 0u);
    check(s_zc.surface_active == 0, "UnbindZcull: inactive");
}

/* M2-03: methods RPCS3 has no handler for recover the FIFO. */
static void test_invalid_methods(void)
{
    const unsigned long n0 = s_invalid_methods;
    check(!gcm_method_invalid(0x1808u, 5u) && !gcm_method_invalid(0x0100u, 0u) &&
          !gcm_method_invalid(0x1D94u, 0u) && !gcm_method_invalid(0x0A74u, 0u),
          "known NV4097 methods are valid");
    check(gcm_method_invalid(0x1FFCu, 0u) && s_invalid_methods == n0 + 1,
          "an unlisted method (0x1FFC) is invalid and counted");

    /* Walker: NOP, invalid method, then put. */
    const u32 ctrl = (u32)(uintptr_t)cellGcmGetControlRegister();
    const u32 base = 0x00010000u;
    vm_write32(IO_ADDR + base + 0, (1u << 18) | 0x0100u);
    vm_write32(IO_ADDR + base + 4, 0);
    vm_write32(IO_ADDR + base + 8, (1u << 18) | 0x1FFCu);
    vm_write32(IO_ADDR + base + 12, 0x12345678u);
    const u32 put = base + 16;
    s_fifo_getoff = base;
    vm_write32(ctrl + 4, base);
    vm_write32(ctrl + 0, put);
    const unsigned long n1 = s_invalid_methods;
    cellGcm_rsx_process_fifo();
    check(s_invalid_methods == n1 + 1, "walker: the invalid method was detected");
    check(vm_read32(ctrl + 4) == base, "walker: get is back at the restore point (RPCS3 recover_fifo)");
}

/* M2-04: RPCS3 tiled_dma_copy addressing, hand-evaluated for a 0x200-pitch
 * tile (factor 2, bank 0) at a 16 KB-aligned base: linear byte 0x200 (row 1)
 * goes to 0x20, byte 32 to 0x100, byte 64 to 0x280. */
static void test_tiling(void)
{
    const u32 base = IO_ADDR + 0x00200000u;
    check(gcm_tile_address(0, 0, 4, base, 0, 0x200, 0, 2, 2) == base &&
          gcm_tile_address(1, 0, 4, base, 0, 0x200, 0, 2, 2) == base + 0x20u &&
          gcm_tile_address(0, 8, 4, base, 0, 0x200, 0, 2, 2) == base + 0x100u &&
          gcm_tile_address(0, 16, 4, base, 0, 0x200, 0, 2, 2) == base + 0x280u,
          "tile address of known texels");

    /* Two tile rows (four tiles, one row address) map onto 64 KB one to one. */
    static u8 seen[0x10000];
    memset(seen, 0, sizeof seen);
    u32 dup = 0, oob = 0;
    for (u32 r = 0; r < 128; r++)
        for (u32 c = 0; c < 128; c++) {
            const u32 o = gcm_tile_address(r, c, 4, base, 0, 0x200, 0, 2, 2) - base;
            if (o + 4u > sizeof seen) { oob++; continue; }
            if (seen[o]) dup++;
            seen[o] = 1;
        }
    check(!dup && !oob, "tile addressing is one to one over the 64 KB");

    /* End to end: NV3089 into a bound MAIN tile stores tiled (GCM_HANDLE_TILED_MEMORY=1). */
    check(cellGcmSetTile(1, 1, 0x00200000u, 0x10000u, 0x200, 0, 0, 0) == CELL_OK, "SetTile MAIN tile");
    u32 tb = 0;
    check(gcm_tiled_region(base + 0x40u, 0x200u, &tb) != NULL && tb == base, "the MAIN tile is found");
    check(gcm_tiled_region(LOCAL_EA, 0x200u, &tb) == NULL, "untiled memory is not");
    const u32 w = 32, h = 4, sp = w * 4u;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) vm_write32(LOCAL_EA + y * sp + x * 4u, 0xFF000000u | (y << 8) | x);
    memset(vm_base + base, 0, 0x2000);
    memset(&s_nv3089, 0, sizeof s_nv3089); memset(&s_gcm2d, 0, sizeof s_gcm2d);
    s_nv3089.fmt = 3u; s_nv3089.src_dma = 0xFEED0000u;
    s_nv3089.out_sz = s_nv3089.clip_sz = s_nv3089.in_sz = (h << 16) | w;
    s_nv3089.in_fmt = (2u << 16) | sp; s_nv3089.in_off = 0;
    s_nv3089.ds_dx = s_nv3089.dt_dy = 0x100000u;
    s_gcm2d.dst_dma = 0xFEED0001u; s_gcm2d.dst_offset = 0x00200000u;
    s_gcm2d.pitch = 0x200u << 16; s_gcm2d.color_fmt = 0xAu;
    nv3089_image_in();
    u32 bad = 0;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
            if (vm_read32(gcm_tile_address(y, x, 4, base, 0, 0x200, 0, 2, 2)) != (0xFF000000u | (y << 8) | x)) bad++;
    check(!bad, "NV3089 blit lands at the tiled addresses");
    check(vm_read32(base + 0x20u) == 0xFF000100u && vm_read32(base + 0x280u) == 0xFF000010u,
          "row 1 col 0 at +0x20, row 0 col 16 at +0x280");
    (void)cellGcmUnbindTile(1);
}

/* M2-07: decode_transfer_registers validation. */
static void test_nv3089_validate(void)
{
    memset(&s_nv3089, 0, sizeof s_nv3089); memset(&s_gcm2d, 0, sizeof s_gcm2d);
    s_nv3089.fmt = 3u; s_nv3089.src_dma = 0xFEED0000u;
    s_gcm2d.dst_dma = 0xFEED0000u; s_gcm2d.color_fmt = 0xAu;
    s_nv3089_recover = 0;
    check(!nv3089_validate() && !s_nv3089_recover, "A8R8G8B8 -> A8R8G8B8 is valid");
    s_gcm2d.color_fmt = 0u;
    check(nv3089_validate() && s_nv3089_recover, "unknown dst format: dropped, FIFO recover");
    s_gcm2d.color_fmt = 0xAu; s_nv3089_recover = 0;
    s_nv3089.op_written = 1; s_nv3089.op = 1;
    check(nv3089_validate() && s_nv3089_recover, "non-SRCCOPY op: dropped, FIFO recover");
    s_nv3089.op = 3; s_nv3089_recover = 0;
    s_nv3089.dst_surf = 0x12345678u;
    check(nv3089_validate() && !s_nv3089_recover, "unknown context surface: NOP without recover");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    _putenv_s("GCM_HANDLE_TILED_MEMORY", "1");
    const uint32_t vm_size = ppu_hle_inject_base + 0x00210000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;

    (void)cellGcmSetupContext(host_alloc(4, 4), CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    s_config.localAddress = LOCAL_EA;

    test_zcull_bind();
    test_zcull_surface();
    test_invalid_methods();
    test_tiling();
    test_nv3089_validate();

    printf("[gcm_rpcs3_parity] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
