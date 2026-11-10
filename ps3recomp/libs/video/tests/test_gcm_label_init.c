/* Initial label / report memory (cellGcmSys.c gcm_init_report_memory).
 *
 * RPCS3's sys_rsx_context_allocate fills RsxReports before the title runs: the
 * semaphore words repeat 0x1337C0D3, 0x1337BABE, 0x1337BEEF, 0x1337F001 (so every
 * 16-byte label reads 0x1337C0D3) and the 2048 local reports read
 * {timestamp -1, value 0, pad -1}. RSX_LABEL_INIT_LEGACY=1 keeps zeros.
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

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x00210000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;
    const char* e = getenv("RSX_LABEL_INIT_LEGACY");
    const int legacy = e && e[0] && e[0] != '0';

    (void)cellGcmSetupContext(host_alloc(4, 4), CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);

    const u32 l0 = (u32)(uintptr_t)cellGcmGetLabelAddress(0);
    const u32 l255 = (u32)(uintptr_t)cellGcmGetLabelAddress(255);
    const u32 r0 = (u32)(uintptr_t)cellGcmGetReportDataAddress(0);
    const u32 r2047 = (u32)(uintptr_t)cellGcmGetReportDataAddress(2047);
    if (!legacy) {
        check(vm_read32(l0) == 0x1337C0D3u && vm_read32(l255) == 0x1337C0D3u,
              "every label reads 0x1337C0D3");
        check(vm_read32(l0 + 4) == 0x1337BABEu && vm_read32(l0 + 8) == 0x1337BEEFu &&
              vm_read32(l0 + 12) == 0x1337F001u, "the semaphore words between labels follow the pattern");
        check(vm_read32(r0) == 0xFFFFFFFFu && vm_read32(r0 + 4) == 0xFFFFFFFFu &&
              vm_read32(r0 + 8) == 0u && vm_read32(r0 + 12) == 0xFFFFFFFFu,
              "report 0 is {timestamp -1, value 0, pad -1}");
        check(vm_read32(r2047) == 0xFFFFFFFFu && vm_read32(r2047 + 12) == 0xFFFFFFFFu &&
              vm_read32(r2047 + 16) == 0u, "through report 2047, not past it");
        check(s_report_data[5].timestamp == ~0ull && s_report_data[5].value == 0u,
              "the host report mirror agrees");
        check(cellGcmGetReport(1, 0) == 0u, "an unwritten report value reads 0");
        check(vm_read32((u32)(uintptr_t)cellGcmGetControlRegister() + 8) == 0xFFFFFFFFu,
              "ref starts at 0xFFFFFFFF (RPCS3 rsx_methods.cpp:683)");
    } else {
        check(vm_read32(l0) == 0u && vm_read32(l0 + 4) == 0u && vm_read32(r0) == 0u &&
              vm_read32(r0 + 12) == 0u, "RSX_LABEL_INIT_LEGACY=1: labels and reports stay zero");
    }

    printf("[gcm_label_init] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}