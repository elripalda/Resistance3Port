/* clang -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_flip_status.c -Wl,-dead_strip -pthread
 *   -o /tmp/test_gcm_flip_status && /tmp/test_gcm_flip_status
 *
 * The flip status against a flip completion that is delivered late: the
 * ticker marks a completion pending, but the flip handler only runs when a
 * guest thread next pumps, and the title may have requested another flip by
 * then. That newer request must stay WAITING until a tick of its own
 * completes it, and so must one that outlives cellGcmSetWaitFlip's bound.
 */
#include "../cellGcmSys.c"
#include <assert.h>
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
void rsx_live_draw_set_display_buffer(u32 b, u32 l, u32 o, u32 p, u32 w, u32 h) { (void)b; (void)l; (void)o; (void)p; (void)w; (void)h; }
static unsigned flips;
static void callback(uint32_t opd, uint64_t a, uint64_t b, uint64_t c,
                     uint64_t d, uint64_t e, uint64_t f, uint64_t h, uint64_t i)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)h; (void)i;
    if (opd == 0x400) flips++;
}
/* A flip the RSX has executed (a direct cellGcmSetFlipCommand, without its
 * request-time callback). A bare cellGcmResetFlipStatus is not a flip: the
 * tick completes only a flip that happened (RPCS3 handle_emu_flip). */
static void request(void)
{
    cellGcmResetFlipStatus();
    s_flip_direct_pending = 1;
}
int main(void)
{
    g_ps3_guest_caller = callback;
    s_flip_handler_opd = 0x400;

    /* The plain case: request, tick, deliver. */
    request();
    assert(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING);
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == 1 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);
    /* No flip outstanding: a tick completes nothing and calls no handler,
     * and a status reset alone requests none. */
    cellGcmResetFlipStatus();
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == 1 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING);
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == 1);

    /* A newer request between the tick and the delivery: the late completion
     * runs the handler for the old flip but leaves the new one WAITING. */
    request();
    cellGcmTickFlip();
    request();
    ppu_gcm_pump();
    assert(flips == 2 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING);
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == 3 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);

    /* cellGcmSetWaitFlip gives up after its bound, but the flip it waited
     * for is still outstanding and its handler still has to run. */
    request();
    cellGcmSetWaitFlip();
    assert(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING);
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == 4 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);

    puts("GCM flip status checks passed");
}
