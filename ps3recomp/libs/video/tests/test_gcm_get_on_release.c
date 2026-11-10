/* The control register's get is past a semaphore RELEASE by the time its
 * label lands (cellGcmSys.c).
 *
 * On the RSX the semaphore write happens after the command has been fetched,
 * so a title that sees a label move may read get once and take it as the
 * RSX's position. The walker wrote the label at once but published get only
 * at a JUMP and at the end of the pass, so that read returned where the pass
 * started: Ratchet & Clank: Tools of Destruction then spins until its own put
 * equals that get, which it never does.
 *
 * The probe is GCM_SET_USER_COMMAND (subchannel 7, method 0x0B00), which
 * calls rsx_raise_user_cmd from inside the pass; the stub records the label
 * and get there. A probe before any RELEASE sees neither move, a single
 * RELEASE and a two-method ACQUIRE+RELEASE each leave get past the whole
 * command, and get does not move with the methods after it. With
 * GCM_GET_EQ_PUT=1 (argument "eq-put") the RELEASE publishes nothing and get
 * is put at the end of the pass, as before.
 *
 * Also: a blocking ACQUIRE with its bound and the label-68 exemption (D1),
 * device/unaligned RELEASE rules (D11), and recover_fifo back to the restore
 * point for the RPCS3 desync cases (D2).
 *
 * gcc -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_get_on_release.c -ffunction-sections
 *   -fdata-sections -Wl,--gc-sections -pthread -o /tmp/test_gcm_get_on_release
 *   (clang on macOS: -Wl,-dead_strip)
 */
#include "../cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define SEMA_OFF  0x10u
#define LABEL_EA  (GCM_LABEL_GUEST_BASE + SEMA_OFF)

/* Records the label and get each time a probe user command runs. */
static u32 seen_get[16], seen_label[16], seen_cnt[16], seen_n;
void rsx_raise_user_cmd(uint32_t arg)
{
    if (arg < 16) {
        seen_cnt[arg]++;
        seen_get[arg] = vm_read32(GCM_CONTROL_GUEST_ADDR + 4);
        seen_label[arg] = vm_read32(LABEL_EA);
        seen_n++;
    }
}

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00100000u
#define CMD_SIZE  0x00010000u

static uint32_t g_heap = 0x00400000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void gw32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

/* An incrementing method header: count words, subchannel subch. */
static u32 put_cmd(u32 io, u32 subch, u32 method, u32 count, const u32* args)
{
    gw32(IO_ADDR + io, (count << 18) | (subch << 13) | method);
    for (u32 i = 0; i < count; i++) gw32(IO_ADDR + io + 4 + i * 4, args[i]);
    return io + 4 + count * 4;
}
static u32 put_probe(u32 io, u32 arg) { return put_cmd(io, 7, 0x0B00u, 1, &arg); }

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const int eq_put = argc > 1 && !strcmp(argv[1], "eq-put");
    setenv("GCM_SEMA_ACQUIRE_MS", "50", 1);
    if (eq_put != gcm_get_eq_put()) { puts("FAIL: GCM_GET_EQ_PUT does not match the argument"); return 1; }
    const uint32_t vm_size = ppu_hle_inject_base + 0x40000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;

    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();

    /* probe 0, OFFSET, RELEASE 0x11, probe 1, ACQUIRE 0x11 + RELEASE 0x22 in
     * one two-method command, probe 2, probe 3; put after the last probe. */
    const u32 off = SEMA_OFF, rel1 = 0x11u, acq_rel[2] = { 0x11u, 0x22u };
    u32 io = put_probe(0, 0);
    io = put_cmd(io, 0, 0x64u, 1, &off);
    io = put_cmd(io, 0, 0x6Cu, 1, &rel1);
    const u32 end_rel1 = io;
    io = put_probe(io, 1);
    io = put_cmd(io, 0, 0x68u, 2, acq_rel);
    const u32 end_rel2 = io;
    io = put_probe(io, 2);
    io = put_probe(io, 3);
    const u32 put = io;
    u32 put_end = put;

    s_fifo_getoff = 0;
    vm_write32(ctrl + 4, 0);
    vm_write32(ctrl + 0, put);
    const u32 label_before = vm_read32(LABEL_EA);   /* RPCS3's 0x1337C0D3 fill */
    cellGcm_rsx_process_fifo();

    check(seen_n == 4, "the four probe commands ran");
    check(seen_get[0] == 0 && seen_label[0] == label_before,
          "before any RELEASE, neither get nor the label has moved");
    check(seen_label[1] == 0x11u, "the RELEASE wrote its label");
    check(seen_label[2] == 0x22u, "the RELEASE inside ACQUIRE+RELEASE wrote its label");
    if (!eq_put) {
        check(seen_get[1] == end_rel1, "get is past the RELEASE when its label is seen");
        check(seen_get[2] == end_rel2, "get is past the whole ACQUIRE+RELEASE command");
        check(seen_get[3] == end_rel2, "get does not move with the methods after it");
    } else {
        check(seen_get[1] == 0 && seen_get[2] == 0 && seen_get[3] == 0,
              "GCM_GET_EQ_PUT: a RELEASE publishes nothing mid-pass");
    }
    check(vm_read32(ctrl + 4) == put, "after the pass, get is put");

    /* D1 (RPCS3 nv406e.cpp:57-107): an unsatisfied ACQUIRE blocks the walker
     * until the label matches, bounded by GCM_SEMA_ACQUIRE_MS (50 here), and
     * the flip-pacing label 68 (offset 0x440) never blocks. */
    {
        const u32 o20 = 0x20u, five = 5u;
        vm_write32(GCM_LABEL_GUEST_BASE + 0x20u, 0);
        u32 p = put_cmd(put, 0, 0x64u, 1, &o20);
        const u32 acq_at = p;
        p = put_cmd(p, 0, 0x68u, 1, &five);
        p = put_probe(p, 4);
        vm_write32(ctrl + 0, p);
        cellGcm_rsx_process_fifo();
        check(s_fifo_getoff == acq_at && !seen_cnt[4], "an unsatisfied ACQUIRE stops the walker on it");
        cellGcm_rsx_process_fifo();
        check(s_fifo_getoff == acq_at && !seen_cnt[4], "...and the next pass still waits");
        vm_write32(GCM_LABEL_GUEST_BASE + 0x20u, 5u);
        cellGcm_rsx_process_fifo();
        check(seen_cnt[4] == 1 && s_fifo_getoff == p, "the label lands: the walker goes on");

        const u32 o30 = 0x30u, nine = 9u;
        vm_write32(GCM_LABEL_GUEST_BASE + 0x30u, 0);
        u32 q = put_cmd(p, 0, 0x64u, 1, &o30);
        q = put_cmd(q, 0, 0x68u, 1, &nine);
        q = put_probe(q, 5);
        vm_write32(ctrl + 0, q);
        extern unsigned long long ps3_ms_now(void);
        const unsigned long long t0 = ps3_ms_now();
        while (!seen_cnt[5] && ps3_ms_now() - t0 < 2000u) { cellGcm_rsx_process_fifo(); }
        const unsigned long long dt = ps3_ms_now() - t0;
        check(seen_cnt[5] == 1 && dt >= 40u && s_fifo_getoff == q,
              "a never-satisfied ACQUIRE gives up after GCM_SEMA_ACQUIRE_MS");

        const u32 o440 = 0x440u, one = 1u;
        vm_write32(GCM_LABEL_GUEST_BASE + 0x440u, 0);
        u32 r = put_cmd(q, 0, 0x64u, 1, &o440);
        r = put_cmd(r, 0, 0x68u, 1, &one);
        r = put_probe(r, 6);
        vm_write32(ctrl + 0, r);
        cellGcm_rsx_process_fifo();
        check(seen_cnt[6] == 1 && s_fifo_getoff == r, "the flip-pacing ACQUIRE (label 68) does not block");
        put_end = r;
    }

    /* D11 (RPCS3 nv406e.cpp:110-150): a RELEASE through the device context
     * lands at device+offset and 0 at device+0x30 is written as 1; an
     * unaligned offset is ignored. */
    {
        const u32 dev = 0x56616660u, o30 = 0x30u, zero = 0u, o31 = 0x31u, five = 5u, lab = 0x66616661u;
        vm_write32(R2RSX_DEVICE_EA + 0x30u, 0xABCDu);
        u32 p = put_cmd(put_end, 0, 0x60u, 1, &dev);
        p = put_cmd(p, 0, 0x64u, 1, &o30);
        p = put_cmd(p, 0, 0x6Cu, 1, &zero);
        vm_write32(ctrl + 0, p);
        cellGcm_rsx_process_fifo();
        check(s_fifo_getoff == p && vm_read32(R2RSX_DEVICE_EA + 0x30u) == 1u,
              "a RELEASE of 0 to device+0x30 writes 1");
        p = put_cmd(p, 0, 0x64u, 1, &o31);
        p = put_cmd(p, 0, 0x6Cu, 1, &five);
        p = put_cmd(p, 0, 0x60u, 1, &lab);
        vm_write32(ctrl + 0, p);
        cellGcm_rsx_process_fifo();
        check(s_fifo_getoff == p && vm_read32(R2RSX_DEVICE_EA + 0x30u) == 1u &&
              vm_read32(R2RSX_DEVICE_EA + 0x34u) == 0u, "an unaligned RELEASE is ignored");
        put_end = p;
    }

    /* D2 (RPCS3 RSXFIFO.cpp:777-833, RSXThread.cpp:1740-1780): a RET with no
     * CALL, a CALL inside a CALL, a malformed word, an unaligned 0x1D70 and an
     * invalid INDEX_ARRAY_DMA go back to the restore point; the commands from
     * there run again once the producer has fixed the word. */
    {
        struct { const char* what; u32 bad_hdr, bad_arg; int sub; } cases[] = {
            { "RET without CALL", 0x00020000u, 0, 0 },
            { "malformed word",   0x00010040u, 0, 0 },
            { "unaligned 0x1D70", (2u << 18) | 0x1D6Cu, 4u, 3 },   /* offset 4, then 0x1D70 */
            { "INDEX_ARRAY_DMA 2", (1u << 18) | 0x1820u, 2u, 1 },
            { "CALL inside CALL", 0, 0, 2 },
        };
        for (u32 c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            const u32 start = put_end;
            const u32 sub = start + 0x400u;
            u32 bad_at, p;
            seen_cnt[7] = seen_cnt[8] = 0;
            p = put_probe(start, 7);
            bad_at = p;
            if (cases[c].sub == 2) {
                gw32(IO_ADDR + bad_at, sub | 2u);         /* CALL sub */
                gw32(IO_ADDR + sub, (sub + 8u) | 2u);     /* CALL inside it */
                gw32(IO_ADDR + sub + 4u, 0x00020000u);    /* RET */
                gw32(IO_ADDR + sub + 8u, 0x00020000u);
                p = bad_at + 4u;
            } else {
                gw32(IO_ADDR + bad_at, cases[c].bad_hdr);
                if (cases[c].sub >= 1) gw32(IO_ADDR + bad_at + 4u, cases[c].bad_arg);
                if (cases[c].sub == 3) gw32(IO_ADDR + bad_at + 8u, 1u);
                p = bad_at + (cases[c].sub == 3 ? 12u : cases[c].sub == 1 ? 8u : 4u);
            }
            p = put_probe(p, 8);
            s_fifo_sync_req = 1;
            vm_write32(ctrl + 0, p);
            cellGcm_rsx_process_fifo();
            char msg[96];
            snprintf(msg, sizeof msg, "%s: back to the restore point, nothing after it ran", cases[c].what);
            check(s_fifo_getoff == start && seen_cnt[7] == 1 && !seen_cnt[8], msg);
            /* the producer fixes the word: a NOP (and a NOP over the argument). */
            if (cases[c].sub == 2) gw32(IO_ADDR + sub, 0);
            else if (cases[c].sub == 3) gw32(IO_ADDR + bad_at + 4u, 0x10u);   /* aligned offset */
            else { gw32(IO_ADDR + bad_at, 0); if (cases[c].sub == 1) gw32(IO_ADDR + bad_at + 4u, 0); }
            cellGcm_rsx_process_fifo();
            snprintf(msg, sizeof msg, "%s: once fixed, the commands from the restore point run again", cases[c].what);
            check(s_fifo_getoff == p && seen_cnt[7] == 2 && seen_cnt[8] == 1, msg);
            put_end = (sub + 0x20u + 15u) & ~15u;
            s_fifo_getoff = p; s_fifo_calloff = GCM_CALL_NONE;
            if (put_end < p) put_end = p;
            /* move get on to the next case without the gap in between */
            gw32(IO_ADDR + p, put_end | 0x20000000u);
            vm_write32(ctrl + 0, put_end);
            cellGcm_rsx_process_fifo();
        }
    }

    printf("[gcm_get_on_release%s] %s (%d failed)\n", eq_put ? " eq-put" : "", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
