/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * An SPU that issues GETLLAR holds a reservation on a 128-byte line and then
 * spins, waiting for someone else to change it. On real hardware a store by
 * ANY other processor to that line kills the reservation and raises
 * SPU_EVENT_LR on the reserving SPU, which is how a parked SPURS idle service
 * learns that the PPU has posted work. Emulated, the two halves live on
 * different host threads with nothing between them, so the SPU half alone gets
 * PUTLLC right (it compares the line before committing) while the PPU half
 * never tells anyone it wrote. The service sleeps through the signal.
 *
 * This is the missing half. Every line an SPU reserves is marked in a bitmap;
 * the PPU store paths (runtime/ppu/ppu_memory.h and runtime/ppu/ppu_loader.cpp)
 * check the bitmap, and on a hit commit the store under the same lock-line lock
 * the SPU's GETLLAR/PUTLLC transaction takes, then notify. That does two things
 * at once: the store can no longer land inside a PUTLLC's compare/commit window
 * (where it would be silently reverted), and the reserving SPU gets its event.
 *
 * The bitmap check is a shift and a load against a global that stays zero until
 * an SPU actually reserves something, so a title that never runs SPU code pays
 * one predictable branch per store.
 */

#ifndef SPU_COHERENCY_H
#define SPU_COHERENCY_H

#include "spu_context.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPU external event: lock-line reservation lost. */
#define SPU_EVENT_LR  0x400u

/* The lock-line lock. One process-wide spinlock serializing every lock-line
 * transaction, SPU and PPU alike: the SPU's GETLLAR snapshot and PUTLLC
 * compare-and-commit, and a PPU store or stwcx to a line some SPU has
 * reserved. The critical sections are two 128-byte memcpy and a memcmp. */
void spu_lockline_lock(void);
void spu_lockline_unlock(void);

/* Resistance 2 audio: per-host-thread state of a "real-time lane" -- the SPURS
 * lane running the nextsynth job list (cellSpurs.c sets `on` around that
 * lane's policy-module runs). The DMA repeat-poll back-off (spu_dma.h) spins
 * instead of napping for the first SPURS_AUDIO_SPIN_US of a poll on such a
 * lane, and both it and the lock-line lock account their waits here so
 * [spurs-audio] can say where the lane's wall time goes. Times are QPC ticks. */
typedef struct spu_rt_lane {
    int on;
    long long poll_t0;                 /* QPC: start of the current spin budget (0 = none) */
    long long nap_n, nap_t;            /* repeat-poll naps (real sleeps) */
    long long spin_n, spin_t;          /* repeat-poll spins (CPU, replaces naps) */
    long long lock_n, lock_t;          /* contended lock-line acquisitions */
    long long yield_n;                 /* SwitchToThread calls inside those */
    uint32_t poll_ea, poll_pc;         /* last repeat-polled EA and pc | overlay << 18 */
} spu_rt_lane;
extern SPU_THREAD_LOCAL spu_rt_lane g_spu_rt;

/* Mark `ea`'s 128-byte line as SPU-reserved and remember `ctx` as a possible
 * reservation holder. Called from the SPU's GETLLAR with the lock held.
 *
 * Lines are marked and never unmarked. A line the SPURS kernel reserves is
 * reserved again microseconds later, so unmarking would buy nothing; a stale
 * mark costs one locked PPU store to a line the kernel owns anyway. */
void spu_coh_reserve(spu_context* ctx, uint32_t ea);

/* GETLLAR re-read of a line `ctx` already holds a valid reservation on, as a
 * seqlock read without taking the lock-line lock (see spu_coherency.c).
 * Copies the line to `ls` and the reservation snapshot and returns 1, or
 * returns 0 when the caller must take the locked path. SPU_FAST_LLAR=0 off. */
int spu_coh_llar_fast(spu_context* ctx, uint32_t ea, uint8_t* ls);

/* Is `addr`'s line marked? The PPU store fast path -- a range-free O(1) bitmap
 * lookup guarded by a global that is zero until the first GETLLAR. */
int spu_coh_is_reserved(uint32_t addr);

/* A store by the PPU landed on `ea`'s line. Raise SPU_EVENT_LR on every SPU
 * whose live reservation covers it, drop that reservation, and wake the host
 * thread in case it is parked in `rdch SPU_RdEventStat`.
 *
 * Expects the lock-line lock held, so that the store this is reporting and the
 * reservation state it clears are seen by an SPU's PUTLLC as one transaction. */
void spu_coh_notify_write(uint32_t ea);
/* Remove a context from the reserving set. MUST be called before a
 * transient (stack-allocated) spu_context goes out of scope. */
void spu_coh_unregister(spu_context* ctx);
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi);  /* thread exit */
/* As above, but leaves `self`'s own reservation alone: an SPU's own MFC
 * write does not cost it its reservation on hardware. */
void spu_coh_notify_write_except(uint32_t ea, const void* self);

/* Count of LR events delivered. A parked SPURS service that never wakes with
 * this at zero is a coherence miss; nonzero moves the question downstream. */
extern unsigned long g_spu_lr_raise;

#ifdef __cplusplus
}
#endif

#endif /* SPU_COHERENCY_H */
