/* The buffer-full callback in RPCS3 mode uses the runtime ring in 32 KB
 * segments (cellGcmSys.c gcm_r2rsx_recycle_segment), as RPCS3's
 * cellGcmCallback does: JUMP from the write head to the next segment, put at
 * the write head, the context set to that segment (end leaves 4 bytes for the
 * next JUMP). It used to JUMP back to begin and wait for a full drain.
 * A reservation larger than a segment still takes the full drain, over the
 * whole ring.
 */#include "../cellGcmSys.c"
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
/* The walker's other consumers: no graphics backend, no lifted program. */
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
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

void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }
#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00100000u
#define CMD_SIZE  0x00021000u
#define SEG       0x00008000u

static uint32_t g_heap = 0x00400000u;
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

/* Wrap with the write head at io `cur`; expect the next context at io `nb`. */
static void wrap(u32 ctx, u32 ctrl, u32 cur, u32 count, u32 nb, u32 nend, u32 put, const char* what)
{
    char msg[160];
    vm_write32(ctx + 0x8, IO_ADDR + cur);
    vm_write32(IO_ADDR + cur, 0xDEADBEEFu);
    cellGcm_fifo_recycle_count(ctx, count);
    int ok = vm_read32(IO_ADDR + cur) == (0x20000000u | nb) &&
             vm_read32(ctrl + 0) == put &&
             vm_read32(ctx + 0x0) == IO_ADDR + nb &&
             vm_read32(ctx + 0x4) == IO_ADDR + nend &&
             vm_read32(ctx + 0x8) == IO_ADDR + nb;
    snprintf(msg, sizeof msg, "%s (jump %08X put %08X begin %08X end %08X cur %08X)", what,
             vm_read32(IO_ADDR + cur), vm_read32(ctrl + 0), vm_read32(ctx + 0x0),
             vm_read32(ctx + 0x4), vm_read32(ctx + 0x8));
    check(ok, msg);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;

    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    const u32 ctx = vm_read32(ctx_out);
    const u32 ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    s_r2rsx = 1;

    wrap(ctx, ctrl, CMD_SIZE - 0x10, 16, 0, SEG - 4, CMD_SIZE - 0x10,
         "first lap (whole ring) wraps to segment 0");
    wrap(ctx, ctrl, SEG - 4, 16, SEG, 2 * SEG - 4, SEG - 4,
         "segment 0 full: jump to segment 1, put at the jump");
    wrap(ctx, ctrl, 3 * SEG - 0x20, 16, 3 * SEG, 4 * SEG - 4, 3 * SEG - 0x20,
         "segment 2 -> segment 3");
    wrap(ctx, ctrl, 4 * SEG - 4, 16, 0, SEG - 4, 4 * SEG - 4,
         "last whole segment wraps to segment 0 (the tail is first-lap only)");
    wrap(ctx, ctrl, SEG + 0x100, SEG / 4, 0, CMD_SIZE, 0,
         "a reservation larger than a segment takes the full drain over the whole ring");

    printf("[gcm_ring_segment] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}