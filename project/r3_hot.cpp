// R3 port: native fast paths for hot lifted PPU functions.
//
// ---- occlusion-query report wait (func_0074EAB0) ----
// Same engine routine as R2's func_006483A8 (project/r2_hot.cpp in the R2 port): at the start of
// frame N the main thread reads the visibility results of the occlusion queries it issued in
// frame N-1. It polls the 16-byte RSX report of the LAST query in the set until its timestamp is
// non-zero (sys_timer_usleep(30) per poll, 1 s timeout), then turns every report into a visible
// flag. On timeout it returns without touching the flags or marking the set as read. While it
// sleeps, the main thread and rsx::thread take turns instead of overlapping.
//   R3_OCC_SKIP=<n>      when the result is not in yet, take the timeout path at once, at most n
//                        frames in a row per set (the n+1-th waits as before). Default 0 = the
//                        original wait every frame; 1 is faster but objects pop in and out.
//   R3_OCC_WAIT_US=<us>  wait natively at most <us>, then the timeout path (unset: the lifted wait)
// [occ] lines (every ~10 s): sets read, ready at once, skipped, waited, timeouts.
#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include "ppu_recomp.h"

namespace {

constexpr uint64_t kLo = 0x10000u, kHi = 0xDFFFFFF8u;
constexpr uint64_t kPollLo = 0x20002000u, kPollHi = 0x20002010u;

inline bool span_ok(uint32_t ea, uint64_t n)
{
    const uint64_t a = ea & ~7ull, b = ((uint64_t)ea + n + 7) & ~7ull;
    return a >= kLo && b <= kHi && !(a < kPollHi && b > kPollLo);
}
inline uint32_t ld_be32(uint32_t ea) { uint32_t v; memcpy(&v, vm_base + ea, 4); return _byteswap_ulong(v); }
inline uint16_t ld_be16(uint32_t ea) { uint16_t v; memcpy(&v, vm_base + ea, 2); return _byteswap_ushort(v); }

int env_int(const char* name, int def)
{
    const char* e = getenv(name);
    return (e && *e) ? atoi(e) : def;
}

int g_occ_init = 0;
long long g_occ_us = -1;
int g_occ_skip = 0;
std::atomic<long long> g_occ_calls{0}, g_occ_ready{0}, g_occ_skipped{0}, g_occ_waited{0}, g_occ_timeouts{0}, g_occ_wait_ns{0};
std::atomic<unsigned long long> g_occ_next{0};
struct OccSet { uint32_t obj; int32_t b; int run; };
OccSet g_occ_sets[16];   /* main thread only */

void occ_report()
{
    static long long lc, lr, ls, lw, lt, lns;
    const long long c = g_occ_calls.load(), r = g_occ_ready.load(), s = g_occ_skipped.load(), w = g_occ_waited.load(),
                    t = g_occ_timeouts.load(), ns = g_occ_wait_ns.load();
    fprintf(stderr, "[occ] report sets %lld: ready %lld, skipped %lld (R3_OCC_SKIP=%d), waited %lld", c - lc, r - lr, s - ls,
            g_occ_skip, w - lw);
    if (g_occ_us >= 0)
        fprintf(stderr, " (%.2f ms/s, %.0f us each, timeouts %lld, R3_OCC_WAIT_US=%lld)", (double)(ns - lns) / 1e7,
                w - lw ? (double)(ns - lns) / 1e3 / (double)(w - lw) : 0.0, t - lt, g_occ_us);
    fprintf(stderr, "\n");
    lc = c; lr = r; ls = s; lw = w; lt = t; lns = ns;
}

OccSet* occ_set(uint32_t obj, int32_t b)
{
    OccSet* lru = &g_occ_sets[0];
    for (OccSet& e : g_occ_sets) {
        if (e.obj == obj && e.b == b) return &e;
        if (!e.obj) { e.obj = obj; e.b = b; e.run = 0; return &e; }
    }
    lru->obj = obj; lru->b = b; lru->run = 0;
    return lru;
}
}  // namespace

/* 1: return from func_0074EAB0 at once (its timeout path: no flags written, set not marked);
   0: run the lifted body. */
extern "C" int r3_occ_wait(ppu_context* ctx)
{
    if (__builtin_expect(!g_occ_init, 0)) {
        const char* e = getenv("R3_OCC_WAIT_US");
        g_occ_us = (e && *e) ? atoll(e) : -1;
        g_occ_skip = env_int("R3_OCC_SKIP", 0);
        if (g_occ_skip < 0) g_occ_skip = 0;
        g_occ_init = 1;
        fprintf(stderr, "[occ] occlusion report wait: skip %d frame(s) in a row, native timeout %lld us (-1 = lifted)\n",
                g_occ_skip, g_occ_us);
        g_occ_next.store(GetTickCount64() + 10000);
    }
    if (g_occ_us < 0 && !g_occ_skip) return 0;

    /* same address arithmetic as the lifted entry; anything unexpected runs the lifted body */
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    const int32_t b = (int32_t)ctx->gpr[4];
    if (b < 0 || b > 64) return 0;
    if (!span_ok(obj, 0x88 + 4 * (uint64_t)b) || !span_ok(0x012B1F20 + 0xB4, 4) || !span_ok(0x012D6F74, 4)) return 0;
    const uint32_t frame = ld_be32(0x012B1F20 + 0xB4);
    if (ld_be32(obj + 8 + 4 * b) < frame - 1) return 0;   /* not last frame's set: no wait */
    const uint32_t count = ld_be16(obj + 0x84 + 2 * b);
    if (!count) return 0;
    const uint32_t list = ld_be32(obj + 0x74 + 4 * b);
    if (!span_ok(list + 2 * (count - 1), 2)) return 0;
    const uint32_t last = ld_be16(list + 2 * (count - 1));
    const uint32_t rep = ld_be32(0x012D6F74) + (last << 4);
    if (!span_ok(rep, 8)) return 0;

    g_occ_calls.fetch_add(1, std::memory_order_relaxed);
    const unsigned long long now = GetTickCount64();
    unsigned long long nx = g_occ_next.load(std::memory_order_relaxed);
    if (now >= nx && g_occ_next.compare_exchange_strong(nx, now + 10000)) occ_report();

    const volatile uint64_t* ts = (const volatile uint64_t*)(vm_base + rep);
    OccSet* st = occ_set(obj, b);
    if (*ts) { st->run = 0; g_occ_ready.fetch_add(1, std::memory_order_relaxed); return 0; }
    if (st->run < g_occ_skip) { st->run++; g_occ_skipped.fetch_add(1, std::memory_order_relaxed); return 1; }
    st->run = 0;
    g_occ_waited.fetch_add(1, std::memory_order_relaxed);
    if (g_occ_us < 0) return 0;   /* the lifted wait */

    LARGE_INTEGER f, q0, q; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&q0);
    const long long lim = g_occ_us * f.QuadPart / 1000000;
    int timed_out = 0;
    for (unsigned i = 0;; i++) {
        if (*ts) break;
        QueryPerformanceCounter(&q);
        if (q.QuadPart - q0.QuadPart >= lim) { timed_out = 1; break; }
        if (i < 64) _mm_pause(); else SwitchToThread();
    }
    QueryPerformanceCounter(&q);
    g_occ_wait_ns.fetch_add((q.QuadPart - q0.QuadPart) * 1000000000 / f.QuadPart, std::memory_order_relaxed);
    if (timed_out) g_occ_timeouts.fetch_add(1, std::memory_order_relaxed);
    return timed_out;
}
