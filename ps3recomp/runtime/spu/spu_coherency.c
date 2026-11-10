/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * See spu_coherency.h for what this is for. The state here is deliberately
 * plain: a bit per 128-byte line, a small table of the SPU contexts that have
 * ever reserved one, and the lock-line spinlock. Everything that WRITES it runs
 * with the lock-line lock held (the SPU's GETLLAR, the PPU's coherent store),
 * so the read-modify-write on a shared bitmap byte is serialized. Only
 * spu_coh_is_reserved reads without the lock, which is the entire point: it is
 * on every PPU store.
 */

#include <stdlib.h>
#include "spu_coherency.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ps3emu/log_first.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * The lock-line lock
 *
 * Moved here from spu_channels.c, where it was a file-static and so could
 * serialize SPU against SPU but not SPU against PPU -- which left the PPU free
 * to write a line in the middle of a PUTLLC's compare-and-commit. Same
 * spinlock, same semantics, now shared. _InterlockedExchange is a clang-cl/MSVC
 * intrinsic (no runtime library symbol needed); elsewhere use the C11
 * equivalent, which lowers to the same LL/SC or lock-xchg on every target.
 * ===========================================================================*/
#if defined(_MSC_VER)
#include <intrin.h>
#include <windows.h>   /* SwitchToThread */
/* The lock and its release sequence share one cache line of their own: every
 * locker touches both, and nothing else in this file is written as often.
 * g_lockline_seq is bumped (under the lock) by every unlock, which makes the
 * pair a seqlock for spu_coh_llar_fast: a reader that sees the lock free and
 * the same sequence before and after its copy overlapped no locked writer. */
static __declspec(align(64)) volatile long g_lockline_pair[16];
#define g_lockline     g_lockline_pair[0]
#define g_lockline_seq g_lockline_pair[1]
/* Test-and-test-and-set: spin on a plain read (no cache-line ping-pong) with a
 * pause, and yield after a long wait. The bare exchange loop let GH3's four
 * idle job-policy pollers saturate the line and delay every other SPU atomic
 * -- FMOD's mixer task among them, 25-40 ms at a time. */
SPU_THREAD_LOCAL spu_rt_lane g_spu_rt;
void spu_lockline_lock(void)
{
    unsigned spins = 0;
    if (!_InterlockedExchange(&g_lockline, 1)) return;
    /* contended: on the audio lane, account the wait (spu_coherency.h) */
    LARGE_INTEGER t0; t0.QuadPart = 0;
    if (g_spu_rt.on) QueryPerformanceCounter(&t0);
    do {
        do {
            _mm_pause();
            if (++spins >= 4096) { spins = 0; SwitchToThread(); if (t0.QuadPart) g_spu_rt.yield_n++; }
        } while (g_lockline);
    } while (_InterlockedExchange(&g_lockline, 1));
    if (t0.QuadPart) {
        LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        g_spu_rt.lock_n++; g_spu_rt.lock_t += t1.QuadPart - t0.QuadPart;
    }
}
void spu_lockline_unlock(void)
{
    g_lockline_seq = g_lockline_seq + 1;   /* single writer: the lock holder */
    _InterlockedExchange(&g_lockline, 0);
}
#else
#include <stdatomic.h>
static atomic_flag g_lockline = ATOMIC_FLAG_INIT;
SPU_THREAD_LOCAL spu_rt_lane g_spu_rt;
void spu_lockline_lock(void)   { while (atomic_flag_test_and_set_explicit(&g_lockline, memory_order_acquire)) { } }
void spu_lockline_unlock(void) { atomic_flag_clear_explicit(&g_lockline, memory_order_release); }
#endif

/* ===========================================================================
 * The reserved-line bitmap
 *
 * One bit per 128-byte line over the whole 32-bit guest address space: 4 MiB of
 * zero-initialized BSS, of which only the pages around addresses an SPU has
 * actually reserved are ever touched. The reference implementation scopes its
 * bitmap to the one window a single title's SPURS structures live in; this
 * runtime hands out guest memory from several disjoint regions (the ELF image,
 * the sys_memory window at 0x40000000, the raw-SPU local-store windows), and a
 * line outside a hardcoded window would be silently non-coherent, which is the
 * failure this whole file exists to remove.
 * ===========================================================================*/
#define SPU_COH_LINE_SHIFT  7                                  /* 128-byte lines */
#define SPU_COH_LINES       (1u << (32 - SPU_COH_LINE_SHIFT))   /* 2^25 lines */
#define SPU_COH_BITMAP_SZ   (SPU_COH_LINES / 8)                 /* 4 MiB */

static unsigned char s_coh_bitmap[SPU_COH_BITMAP_SZ];

/* Zero until the first GETLLAR anywhere. Keeps the PPU fast path off the
 * bitmap entirely for a title that runs no SPU code, and keeps every existing
 * test that stores through vm_write* paying one compare against a hot global.
 * Written once, under the lock; read unlocked, where a stale zero costs at most
 * one missed event on the very first reservation of the run. */
static int s_coh_armed;
/* Resistance port: the same two, exported for the generated code's inline
 * store fast path (ppu_recomp.h vm_st_fast). */
unsigned char* g_spu_coh_bitmap_p = s_coh_bitmap;
volatile int g_spu_coh_armed = 0;

/* Every live virtual context may retain a reservation between scheduling
 * quanta; physical SPU count is not a bound on this set. All access is under
 * the lock-line lock. Nodes are removed before their context's lifetime ends. */
typedef struct spu_coh_entry {
    spu_context* ctx;
    struct spu_coh_entry* next;
} spu_coh_entry;
static spu_coh_entry* s_coh_ctxs;

unsigned long g_spu_lr_raise = 0;

/* v163: the lifted PPU code's stack stores (ppu_recomp.h vm_write*_stk) skip
 * the reservation bitmap on the premise that no SPU reserves a line of a PPU
 * stack (0xD0000000..0xE0000000). Say so loudly if one ever does. */
volatile int g_spu_coh_stack = 0;

/* Registration cache: the holder list only shrinks through spu_coh_unregister
 * and spu_coh_forget_range, both of which bump s_coh_gen under the lock, so a
 * thread that found `ctx` in the list at generation g knows it is still there
 * while s_coh_gen == g. Saves the list walk on every GETLLAR. */
static volatile long s_coh_gen = 1;
static SPU_THREAD_LOCAL spu_context* t_coh_reg_ctx;
static SPU_THREAD_LOCAL long t_coh_reg_gen;

void spu_coh_reserve(spu_context* ctx, uint32_t ea)
{
    if (__builtin_expect(ea >= 0xD0000000u && ea < 0xE0000000u, 0) && !g_spu_coh_stack) {
        g_spu_coh_stack = 1;
        fprintf(stderr, "[coh] SPU reservation on a PPU stack line 0x%08X (img=%d pc=0x%05X) -- the v163 stack fast path "
                        "needs -DPPU_STACK_COH\n", ea, ctx ? ctx->image_id : -1, ctx ? (unsigned)(ctx->pc & 0x3FFFF) : 0u);
        FILE* f = fopen("logs\\coh_stack.txt", "a");
        if (f) { fprintf(f, "SPU reservation on PPU stack line 0x%08X img=%d pc=0x%05X\n", ea, ctx ? ctx->image_id : -1,
                         ctx ? (unsigned)(ctx->pc & 0x3FFFF) : 0u); fclose(f); }
    }
    uint32_t line = ea >> SPU_COH_LINE_SHIFT;
    /* Test before set: every PPU store reads the bitmap and the armed flag
     * (spu_coh_is_reserved), so rewriting them with the same value on each
     * GETLLAR only pulled those cache lines away from the PPU threads. */
    const unsigned char bit = (unsigned char)(1u << (line & 7));
    if (!(s_coh_bitmap[line >> 3] & bit)) s_coh_bitmap[line >> 3] |= bit;
    if (!s_coh_armed) { s_coh_armed = 1; g_spu_coh_armed = 1; }

    if (!ctx) return;
    if (ctx == t_coh_reg_ctx && t_coh_reg_gen == s_coh_gen) return;
    for (spu_coh_entry* entry = s_coh_ctxs; entry; entry = entry->next)
        if (entry->ctx == ctx) { t_coh_reg_ctx = ctx; t_coh_reg_gen = s_coh_gen; return; }
    spu_coh_entry* entry = (spu_coh_entry*)malloc(sizeof(*entry));
    if (!entry) {
        fprintf(stderr, "[spu-coh] cannot register reservation holder %p\n", (void*)ctx);
        fflush(stderr);
        abort(); /* A missed holder loses invalidation events, including ABA. */
    }
    entry->ctx = ctx;
    entry->next = s_coh_ctxs;
    s_coh_ctxs = entry;
    t_coh_reg_ctx = ctx; t_coh_reg_gen = s_coh_gen;
}

/* Lock-free GETLLAR re-read (SPU_FAST_LLAR, default on).
 *
 * The polling idiom re-issues GETLLAR on a line it already holds a valid
 * reservation on. The locked path would only re-copy the line and re-take the
 * reservation it already has, and its lock RMW bounces the lock line between
 * every SPU thread and every PPU store to a reserved line. Here the copy is a
 * seqlock read instead: every writer of a reserved line (PPU coherent store,
 * PUTLLC/PUTLLUC, coherent DMA PUT, HLE sync) holds the lock-line lock while it
 * writes and notifies, and every unlock bumps g_lockline_seq. So if the lock is
 * free and the sequence is unchanged across the copy, no writer overlapped it:
 * the bytes are one consistent snapshot, exactly what the locked copy gives.
 *
 * Reservation state is not written here. resv_valid/resv_ea are checked after
 * the first sequence read, so a writer that clears them must take the lock and
 * fails the recheck; a writer after the recheck sees the reservation and raises
 * SPU_EVENT_LR as it would after a locked GETLLAR. Anything else (no current
 * reservation, a different line, a context not yet registered) falls back to
 * the locked path. Returns 1 if the GETLLAR was done, 0 to take the slow path. */
extern uint8_t* vm_base;
int spu_coh_llar_fast(spu_context* ctx, uint32_t ea, uint8_t* ls)
{
#if defined(_MSC_VER)
    static int s_on = -1;
    if (__builtin_expect(s_on < 0, 0)) { const char* e = getenv("SPU_FAST_LLAR"); s_on = e ? atoi(e) != 0 : 1; }
    if (!s_on) return 0;
    const long s1 = g_lockline_seq;
    _ReadWriteBarrier();
    if (g_lockline) return 0;
    if (!ctx->resv_valid || ctx->resv_ea != ea) return 0;
    if (ctx != t_coh_reg_ctx || t_coh_reg_gen != s_coh_gen) return 0;
    { const uint32_t line = ea >> SPU_COH_LINE_SHIFT;
      if (!((s_coh_bitmap[line >> 3] >> (line & 7)) & 1u)) return 0; }
    memcpy(ls, vm_base + ea, 128);
    /* x86 keeps loads in order: the rechecks below cannot pass the copy. */
    _ReadWriteBarrier();
    if (g_lockline || g_lockline_seq != s1) return 0;
    memcpy(ctx->resv_line, ls, 128);
    ctx->atomic_stat = 4;
    return 1;
#else
    (void)ctx; (void)ea; (void)ls;
    return 0;
#endif
}

/* Drop a context from the reserving set.
 *
 * There was no way to do this, and the contexts that most need it are
 * TRANSIENT: spu_run_lifted_job_abi runs each job on a STACK-LOCAL
 * spu_context, which registered itself here on its first reservation and then
 * returned. The entry outlived the object, so spu_coh_notify_write walked a
 * dangling pointer -- reading resv_valid, writing event_status and calling
 * spu_ch_wake on reclaimed stack. Undefined behaviour, and observable: Guitar
 * Hero III showed THREE live-looking contexts for one dispatch, stealing each
 * other`s reservations, with every PUTLLC failing "no reservation". */
void spu_coh_unregister(spu_context* ctx)
{
    if (!ctx) return;
    /* Under the lock-line lock, which every walker of s_coh_ctxs holds: a
     * notifier that loaded this pointer just before removal would
     * otherwise go on dereferencing it after the job returned and its stack
     * (or its whole async thread) was gone. GH3 at -O2 crashed that way in
     * spu_coh_notify_write, reading a freed thread stack. */
    spu_lockline_lock();
    for (spu_coh_entry** link = &s_coh_ctxs; *link; link = &(*link)->next) {
        if ((*link)->ctx != ctx) continue;
        spu_coh_entry* entry = *link;
        *link = entry->next;
        free(entry);
        s_coh_gen++;
        break;
    }
    spu_lockline_unlock();
}

/* Drop every registered context that lies in [lo, hi) -- a host thread's
 * stack, called as that thread exits. A stack-local context still registered
 * then has leaked past its unregister; the next walker would read an unmapped
 * stack (GH3 at the end of a Havok task: spu_coh_notify_write faulting).
 * ponytail: safety net that also names the leak; find the path that skips
 * spu_coh_unregister if it keeps firing. */
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi)
{
    spu_lockline_lock();
    for (spu_coh_entry** link = &s_coh_ctxs; *link;) {
        spu_coh_entry* entry = *link;
        uintptr_t c = (uintptr_t)entry->ctx;
        if (c >= lo && c < hi) {
            static int _n = 0;
            if (PS3_LOG_FIRST(_n, 16))
                fprintf(stderr, "[spu-coh] leaked ctx %p (img=%d) dropped at thread exit\n",
                        (void*)c, entry->ctx->image_id);
            *link = entry->next;
            free(entry);
            s_coh_gen++;
        } else {
            link = &entry->next;
        }
    }
    spu_lockline_unlock();
}

int spu_coh_is_reserved(uint32_t addr)
{
    if (!s_coh_armed) return 0;
    uint32_t line = addr >> SPU_COH_LINE_SHIFT;
    return (s_coh_bitmap[line >> 3] >> (line & 7)) & 1u;
}

/* A DMA PUT is issued BY an SPU, and hardware does not take that SPU's own
 * reservation away for its own MFC write -- the MFC is part of the same SPE.
 * Our notify had no way to say "everyone but me", so an SPU that reserved a
 * line and then DMA'd a buffer overlapping it killed its own reservation and
 * its next PUTLLC could never succeed. Guitar Hero III's job policy module
 * livelocks exactly there: 16 million atomic ops on one line, every PUTLLC
 * failing for "no reservation", 600k of them self-inflicted.
 *
 * The PUTLLC path already guards this case by dropping its own reservation
 * before notifying; this gives the DMA path the same ability. */
/* ppu_loader.cpp, when linked. The loader is compiled per game, so the host,
 * the fixtures and any build without it have no PPU reservations to break.
 * POSIX: a weak no-op definition the loader's overrides (a weak undefined
 * reference is not enough: ld64 only lets those stay unresolved for dylib
 * symbols). link.exe/lld-link: the linker's own default (see ppu_gcm_pump in
 * ppu_loader.cpp). */
#if defined(_MSC_VER)
void ppu_resv_break_line(uint32_t ea);
void ppu_resv_break_line_absent(uint32_t ea) { (void)ea; }
#pragma comment(linker, "/alternatename:ppu_resv_break_line=ppu_resv_break_line_absent")
#else
__attribute__((weak)) void ppu_resv_break_line(uint32_t ea) { (void)ea; }
#endif

void spu_coh_notify_write_except(uint32_t ea, const void* self)
{
    uint32_t line = ea & ~127u;
    ppu_resv_break_line(ea);
    for (spu_coh_entry* entry = s_coh_ctxs; entry; entry = entry->next) {
        spu_context* c = entry->ctx;
        if (!c || (const void*)c == self) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            SPU_EVENT_SET(c, SPU_EVENT_LR);
            c->resv_valid = 0;
            g_spu_lr_raise++;
            spu_ch_wake(c);
        }
    }
}

/* v190e diag: which host threads take reservations away on the line whose
 * PUTLLCs fail most (spu_channels.c picks it every 5 s and prints this). */
volatile uint32_t g_coh_hot_line = 0xFFFFFFFFu;
volatile long g_coh_kill_n[8]; volatile unsigned long g_coh_kill_tid[8];
/* v190f: and where in the host code the killing writes come from (sampled
 * call stacks, exe RVAs -- symbolize with the build's pdb). */
volatile unsigned long long g_coh_kill_st[8][4]; volatile long g_coh_kill_stn[8];
static void coh_note_kill(void)
{
#ifdef _WIN32
    { static volatile long smp;
      if ((InterlockedIncrement(&smp) & 31) == 0) {
          void* fr[6] = {0}; const unsigned short nf = RtlCaptureStackBackTrace(1, 6, fr, NULL);
          const unsigned long long mb = (unsigned long long)GetModuleHandleA(NULL);
          unsigned long long k[4] = {0, 0, 0, 0};
          for (int i = 0; i < 4 && i + 1 < nf; i++) k[i] = (unsigned long long)fr[i + 1] - mb;
          for (int i = 0; i < 8; i++) {
              if (g_coh_kill_stn[i] && !memcmp((const void*)g_coh_kill_st[i], k, sizeof k)) { InterlockedIncrement(&g_coh_kill_stn[i]); break; }
              if (!g_coh_kill_stn[i]) { memcpy((void*)g_coh_kill_st[i], k, sizeof k); g_coh_kill_stn[i] = 1; break; }
          }
      } }
    const unsigned long t = GetCurrentThreadId();
    for (int i = 0; i < 8; i++) {
        if (g_coh_kill_tid[i] == t) { InterlockedIncrement(&g_coh_kill_n[i]); return; }
        if (g_coh_kill_tid[i] == 0) { g_coh_kill_tid[i] = t; InterlockedIncrement(&g_coh_kill_n[i]); return; }
    }
#endif
}

void spu_coh_notify_write(uint32_t ea)
{
    uint32_t line = ea & ~127u;
    ppu_resv_break_line(ea);
    for (spu_coh_entry* entry = s_coh_ctxs; entry; entry = entry->next) {
        spu_context* c = entry->ctx;
        if (!c) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            if (line == g_coh_hot_line) coh_note_kill();
            SPU_EVENT_SET(c, SPU_EVENT_LR);
            c->resv_valid = 0;          /* reservation lost, PUTLLC must fail */
            /* SPU_PUTLLC_WHY=1: name the agent that killed it. A PUTLLC that
             * always fails for "no reservation" is useless without knowing who
             * took it away -- a peer SPU, or the PPU committing to the line. */
            { static int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) { static unsigned long long n;
                  if ((++n % 200000) == 1)
                      fprintf(stderr, "[resv-lost] %llu: line 0x%08X taken from img=%d ctx=%p\n",
                              n, line, c->image_id, (void*)c); } }
            g_spu_lr_raise++;
            /* The event_status store has to be visible before the wake. The
             * waiter re-polls its predicate on a timeout as well, so a
             * straggling store costs latency and not the wakeup itself. */
            spu_ch_wake(c);
        }
    }
}

#ifdef __cplusplus
}
#endif
