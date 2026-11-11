/*
 * ps3recomp - host wait accounting, guest-memory notify and perf switches.
 * Dependency-free (stdint only) so the generated-code side (stubs.cpp,
 * ppu_sysprx.cpp) can include it without the runtime's ppu_context.h.
 */
#ifndef PS3_WAITSTAT_H
#define PS3_WAITSTAT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Host perf switch: 1 unless <name>=0, or PS3_PERF_ALL=0 / RSX_PERF_ALL=0 (and
 * the default `def` when unset). Reads the environment: callers cache it. */
int ps3_perf_on(const char* name, int def);

/* Guest-memory write notification. Anything that writes guest memory a PPU
 * thread may be polling with a short sys_timer_usleep (RSX report / label /
 * semaphore writes, flip status) calls ps3_guest_mem_notify(ea) after the
 * store; a guest usleep under 1 ms that is spinning returns early, so the poll
 * loop re-checks at once instead of at the end of its 30-50 us sleep.
 * Lock-free and cheap: one load, plus one interlocked increment only while a
 * thread is waiting. Also callable through the g_guest_mem_notify pointer. */
extern volatile long g_guest_mem_seq;
void ps3_guest_mem_notify(uint32_t ea);
void ps3_guest_mem_notify_put(uint32_t ea);   /* SPU plain PUT; PS3_USLEEP_NOTIFY_PUT */

/* R3_SEMLOG: bumped when an RSX label acquire has waited 250 ms. A PPU thread
 * in a guest wait (usleep / lwmutex / lwcond) that sees a new value prints its
 * guest call chain once ([CHAIN:semstall-*]). */
extern volatile long g_ps3_stall_seq;
void ps3_stall_chain(const char* tag);
#define PS3_STALL_CHAIN(tag) do { if (g_ps3_stall_seq) ps3_stall_chain(tag); } while (0)
extern void (*volatile g_guest_mem_notify)(uint32_t ea);

/* Per-host-thread wait accounting (the [thrcpu] table in debugtools.inc):
 * how much of a thread's CPU time is a guest wait spinning or sleeping.
 * Each slot is written only by its own thread; readers diff two samples. */
#define PS3_WAITSTAT_MAX 96
typedef struct ps3_waitstat {
    volatile unsigned long tid;
    volatile unsigned long long short_n, short_qpc;   /* usleep < 1 ms: spin */
    volatile unsigned long long long_n, long_qpc;     /* usleep >= 1 ms: sleep */
    volatile unsigned long long early_n;              /* short spins ended by a notify */
    volatile unsigned long long yield_n;              /* sys_ppu_thread_yield */
    volatile unsigned long long lwm_block_n, lwm_block_qpc; /* contended sys_lwmutex_lock */
    volatile unsigned long long lwc_wait_n, lwc_wait_qpc;   /* sys_lwcond_wait */
} ps3_waitstat;
extern ps3_waitstat g_ps3_waitstat[PS3_WAITSTAT_MAX];
ps3_waitstat* ps3_waitstat_self(void);   /* this thread's slot, or NULL if full */
int64_t ps3_qpc_freq(void);

#ifdef __cplusplus
}
#endif
#endif /* PS3_WAITSTAT_H */
