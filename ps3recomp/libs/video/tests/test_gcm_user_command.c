/* clang -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_user_command.c -Wl,-dead_strip -pthread
 *   -o /tmp/test_gcm_user_command && /tmp/test_gcm_user_command
 */
#include "../cellGcmSys.c"
#include <assert.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <process.h>
typedef HANDLE pthread_t;
typedef struct { void* (*fn)(void*); void* arg; } win_thread_start;
static unsigned __stdcall win_thread_entry(void* p)
{
    win_thread_start* s = (win_thread_start*)p;
    s->fn(s->arg);
    return 0;
}
static int pthread_create(pthread_t* t, const void* attr, void* (*fn)(void*), void* arg)
{
    static win_thread_start start;
    (void)attr;
    start.fn = fn; start.arg = arg;
    *t = (HANDLE)_beginthreadex(NULL, 0, win_thread_entry, &start, 0, NULL);
    return *t ? 0 : -1;
}
static int pthread_join(pthread_t t, void** ret)
{
    (void)ret;
    const int r = WaitForSingleObject(t, INFINITE) == WAIT_OBJECT_0 ? 0 : -1;
    CloseHandle(t);
    return r;
}
#define sched_yield() SwitchToThread()
#else
#include <pthread.h>
#include <sched.h>
#endif
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
static unsigned calls;
static uint32_t last;
static atomic_int paused, resume_pump;
static void* pump_thread(void* unused)
{
    (void)unused; ppu_gcm_pump(); return NULL;
}
static void callback(uint32_t opd, uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f, uint64_t h, uint64_t i)
{
    if (opd == 0x300) {
        atomic_store(&paused, 1);
        while (!atomic_load(&resume_pump)) sched_yield();
        return;
    }
    if (opd == 0x200) { cellGcmQueueUserCommand(0xDD); return; }
    assert(opd == 0x100 && !b && !c && !d && !e && !f && !h && !i);
    calls++; last = (uint32_t)a;
    if (a == 0xAA) { cellGcmQueueUserCommand(0xBB); ppu_gcm_pump(); }
    return;
}
int main(void)
{
    g_ps3_guest_caller = callback;
    cellGcmSetUserHandler((CellGcmUserHandler)(uintptr_t)0x100);
    cellGcmQueueUserCommand(1);
    cellGcmQueueUserCommand(0xAA);
    assert(calls == 0);
    ppu_gcm_pump();
    assert(calls == 1 && last == 0xAA);
    ppu_gcm_pump();
    assert(calls == 2 && last == 0xBB);
    ppu_gcm_pump(); assert(calls == 2);
    cellGcmQueueUserCommand(0); ppu_gcm_pump();
    assert(calls == 3 && last == 0);
    /* A vblank callback can publish another cause after this pump has
     * claimed the old interrupt. It must not replace that claimed cause. */
    s_vblank_handler_opd = 0x200;
    cellGcmQueueUserCommand(0xCC);
    cellGcmTickVBlank();
    ppu_gcm_pump(); assert(calls == 4 && last == 0xCC);
    s_vblank_handler_opd = 0;
    ppu_gcm_pump(); assert(calls == 5 && last == 0xDD);
    ppu_gcm_pump(); assert(calls == 5);
    /* While one host thread has claimed an interrupt but is still in a
     * vblank callback, another pump must not deliver a newer cause first. */
    pthread_t worker;
    s_vblank_handler_opd = 0x300;
    cellGcmQueueUserCommand(0xE1);
    cellGcmTickVBlank();
    assert(pthread_create(&worker, NULL, pump_thread, NULL) == 0);
    while (!atomic_load(&paused)) sched_yield();
    cellGcmQueueUserCommand(0xE2);
    ppu_gcm_pump();
    assert(calls == 5);
    atomic_store(&resume_pump, 1);
    assert(pthread_join(worker, NULL) == 0);
    assert(calls == 6 && last == 0xE1);
    s_vblank_handler_opd = 0;
    ppu_gcm_pump(); assert(calls == 7 && last == 0xE2);
    ppu_gcm_pump(); assert(calls == 7);
    cellGcmSetUserHandler(NULL);
    cellGcmQueueUserCommand(7); ppu_gcm_pump(); assert(calls == 7);
    puts("Deferred GCM user-command checks passed");
}
