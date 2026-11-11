/*
 * ps3recomp - frame hitch logger (PORT_HITCH_LOG, default on; PORT_HITCH_LOG=0 turns it off).
 *
 * Hot paths (game, RSX, render, mixer threads) add the time they spent in a few
 * known-blocking places to per-category accumulators: one rdtsc pair and one
 * interlocked add, no lock, no I/O. Once per game frame the frame limiter
 * (project/stubs.cpp, r2_frame_period) calls port_hitch_frame(), which takes the
 * accumulators and prints a single [hitch] line for a frame over PORT_HITCH_MS
 * (default 25 ms) and a [hitch] 5 s summary (p50 / p99 / max frame time).
 *
 * Header-only (selectany state) so C runtime libs, the RPCS3 RSX library and the
 * port all share one copy without a link dependency. The reporting half is
 * compiled where PORT_HITCH_IMPLEMENTATION is defined (stubs.cpp).
 */
#ifndef PORT_HITCH_H
#define PORT_HITCH_H
#include <stdint.h>
#include <intrin.h>

#ifdef __cplusplus
extern "C" {
#endif

enum port_hitch_cat {
    PH_LIMIT = 0,   /* main thread: frame limiter wait (stubs.cpp)                       */
    PH_TICK,        /* main thread: per-frame PC extras (overlay, benchmark, Reflex sleep) */
    PH_SYNC,        /* main thread: blocking lv2 waits (event queue/flag, semaphore, cond) */
    PH_PAD,         /* game thread: cellPadGetData                                         */
    PH_RSX,         /* rsx::thread: longest flip-to-flip interval (max, not a sum)         */
    PH_PRESENT,     /* rsx::thread: present_frame (DLSS, overlays, submit, vkQueuePresent) */
    PH_ACQUIRE,     /* rsx::thread: vkAcquireNextImageKHR                                  */
    PH_GPUWAIT,     /* rsx::thread: waits for an older frame's command buffer (GPU bound)  */
    PH_DLSS,        /* rsx::thread: r2aa_on_flip (upscaler dispatch)                       */
    PH_PIPE,        /* render thread: pipeline / shader compile stalls (VKR2Pipe)          */
    PH_TEX,         /* rsx::thread: texture uploads (incl. texture-pack hashing)           */
    PH_AUDIO,       /* mixer thread: late guest block wait                                 */
    PH_PRESENT_IV,  /* rsx::thread: longest present-to-present interval (max, not a sum)   */
    PH_FIFOIDLE,    /* rsx::thread: FIFO empty, waiting for the PPU to put commands         */
    PH_LABEL,       /* rsx::thread: NV406E semaphore acquire waits (label not yet released) */
    PH_ZCULL,       /* rsx::thread: ZCULL / occlusion report reads waiting for the GPU      */
    PH_HARDSYNC,    /* rsx::thread: flush_command_queue(hard_sync) (submit + GPU idle)      */
    PH_SUBMIT,      /* rsx::thread: command buffer submits (vkQueueSubmit)                  */
    PH_RSXLIMIT,    /* rsx::thread: RPCS3 flip frame limiter sleep                          */
    PH_RING,        /* PPU (any thread): command ring wrap, waiting for the RSX to drain it */
    PH_AVFLUSH,     /* PPU / SPU (any thread): fault on a GPU-written surface, RSX readback */
    PH_MAINRSX,     /* frame-loop thread's share of PH_RING + PH_AVFLUSH (not "game" time)  */
    PH_COUNT
};

#define PORT_HITCH_LR_SLOTS 6

typedef struct port_hitch_state {
    volatile int64_t acc[PH_COUNT];   /* TSC ticks since the last frame                 */
    volatile int64_t mx[PH_COUNT];    /* longest single event since the last frame      */
    volatile long    cnt[PH_COUNT];   /* events since the last frame                    */
    volatile double  tsc_per_ns;      /* calibrated by port_hitch_frame (0: not yet)    */
    volatile unsigned long main_tid;  /* host thread id of the game's frame loop        */
    volatile int     off;             /* PORT_HITCH_LOG=0                               */
    /* main thread only: guest waits (usleep / lwmutex / lwcond) by guest call site, QPC ticks */
    uint32_t         lr[PORT_HITCH_LR_SLOTS];
    int64_t          lr_qpc[PORT_HITCH_LR_SLOTS];
} port_hitch_state;

#ifdef __cplusplus
__declspec(selectany) port_hitch_state g_port_hitch = {};
#else
__declspec(selectany) port_hitch_state g_port_hitch = { { 0 } };
#endif

static __inline uint64_t port_hitch_t0(void) { return __rdtsc(); }

static __inline void port_hitch_add_ticks(int cat, int64_t d)
{
    if (d <= 0) return;
    _InterlockedExchangeAdd64((volatile long long*)&g_port_hitch.acc[cat], d);
    _InterlockedIncrement(&g_port_hitch.cnt[cat]);
    if (d > g_port_hitch.mx[cat]) g_port_hitch.mx[cat] = d;   /* racy max: diagnostics only */
}

/* Time since port_hitch_t0() into a category. */
static __inline void port_hitch_end(int cat, uint64_t t0)
{
    port_hitch_add_ticks(cat, (int64_t)(__rdtsc() - t0));
}

/* A wait polled in steps (e.g. the RSX semaphore acquire loop): call once per
 * poll iteration; the gaps between calls under 0.5 ms are summed, and a longer
 * gap starts a new wait (the event count). */
static __inline void port_hitch_poll_step(int cat, uint64_t* last)
{
    const uint64_t now = __rdtsc();
    const double k = g_port_hitch.tsc_per_ns;
    const int64_t d = (int64_t)(now - *last);
    *last = now;
    if (k > 0.0 && d > 0 && (double)d < k * 5e5) {
        _InterlockedExchangeAdd64((volatile long long*)&g_port_hitch.acc[cat], d);
        if (d > g_port_hitch.mx[cat]) g_port_hitch.mx[cat] = d;
    } else {
        _InterlockedIncrement(&g_port_hitch.cnt[cat]);
    }
}

static __inline void port_hitch_add_ns(int cat, uint64_t ns)
{
    const double k = g_port_hitch.tsc_per_ns;
    port_hitch_add_ticks(cat, (int64_t)((double)ns * (k > 0.0 ? k : 3.0)));
}

/* Longest-interval categories (PH_RSX): keeps the max only. */
static __inline void port_hitch_max_ticks(int cat, int64_t d)
{
    if (d > g_port_hitch.mx[cat]) g_port_hitch.mx[cat] = d;
    _InterlockedIncrement(&g_port_hitch.cnt[cat]);
}

/* 1 on the game's frame-loop thread (set by port_hitch_frame). */
static __inline int port_hitch_on_main(void)
{
#if defined(_M_X64) || defined(__x86_64__)
    return g_port_hitch.main_tid && __readgsdword(0x48) == g_port_hitch.main_tid;   /* TEB ClientId.UniqueThread */
#else
    return 0;
#endif
}

/* A guest thread blocked inside an HLE call until the RSX catches up (ring wrap,
 * surface readback). These are not guest usleeps, so without this the frame
 * loop's share read as "game" (PPU) time in the [hitch] line. */
static __inline void port_hitch_end_ppu_rsx(int cat, uint64_t t0)
{
    const int64_t d = (int64_t)(__rdtsc() - t0);
    port_hitch_add_ticks(cat, d);
    if (port_hitch_on_main()) port_hitch_add_ticks(PH_MAINRSX, d);
}

/* A guest wait on the frame-loop thread (sys_timer usleep, lwmutex, lwcond): its
 * guest call site and QPC ticks, so a [hitch] line names what the game waited on.
 * Written and read on the main thread only (no atomics). */
static __inline void port_hitch_guest_wait(uint32_t lr, int64_t qpc)
{
    int i, lo = 0;
    if (g_port_hitch.off || !port_hitch_on_main()) return;
    for (i = 0; i < PORT_HITCH_LR_SLOTS; i++) {
        if (g_port_hitch.lr[i] == lr) { g_port_hitch.lr_qpc[i] += qpc; return; }
        if (g_port_hitch.lr_qpc[i] < g_port_hitch.lr_qpc[lo]) lo = i;
    }
    if (g_port_hitch.lr_qpc[lo] <= qpc) { g_port_hitch.lr[lo] = lr; g_port_hitch.lr_qpc[lo] = qpc; }
}

/* Once per game frame, on the frame-loop thread. target_fps <= 0: uncapped.
 * discard = 1 after a pause / long wait that is not a hitch. */
void port_hitch_frame(double target_fps, int discard);

#ifdef __cplusplus
}
#endif

/* ------------------------------------------------------------------------- */
/* Asynchronous log lines for hot threads (render / RSX / frame loop). A printf
 * + fflush there blocks on the CRT stream lock and the file write: the [pipe]
 * line every 5 s on the render thread cost a ~21 ms present (one [hitch] per 5 s
 * in every run). port_log_async formats into a buffer under a short lock; a
 * writer thread flushes it every 100 ms. PORT_LOG_SYNC=1: write at once. */
#ifdef __cplusplus
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <mutex>
#include <thread>
#include <chrono>
#include <condition_variable>
struct port_alog_queue
{
    std::mutex mtx;
    std::condition_variable cv;
    std::string out, err;
    bool started = false;
    int sync = -1;
};
inline port_alog_queue& port_alog_q()
{
    static port_alog_queue* q = new port_alog_queue();   /* never destroyed: the writer outlives static teardown */
    return *q;
}
inline void port_alog_drain()
{
    auto& q = port_alog_q();
    std::string o, e;
    { std::lock_guard<std::mutex> lk(q.mtx); o.swap(q.out); e.swap(q.err); }
    if (!o.empty()) { fwrite(o.data(), 1, o.size(), stdout); fflush(stdout); }
    if (!e.empty()) { fwrite(e.data(), 1, e.size(), stderr); fflush(stderr); }
}
inline void port_log_async(int to_stderr, const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    auto& q = port_alog_q();
    if (q.sync < 0) { const char* s = getenv("PORT_LOG_SYNC"); q.sync = (s && *s && *s != '0') ? 1 : 0; }
    if (q.sync) { FILE* f = to_stderr ? stderr : stdout; fputs(buf, f); fflush(f); return; }
    bool start = false;
    {
        std::lock_guard<std::mutex> lk(q.mtx);
        (to_stderr ? q.err : q.out) += buf;
        if (!q.started) { q.started = true; start = true; }
    }
    if (start) {
        atexit([] { port_alog_drain(); });
        std::thread([] {
            auto& q = port_alog_q();
            for (;;) {
                { std::unique_lock<std::mutex> lk(q.mtx);
                  q.cv.wait_for(lk, std::chrono::milliseconds(100)); }
                port_alog_drain();
            }
        }).detach();
    }
}
#endif

/* ------------------------------------------------------------------------- */
/* Present-to-present intervals: what the display is actually handed, as opposed
 * to the game's CPU frame time above. Called by the RSX display engine
 * (RSXDisplayEngine.cpp, PORT_HITCH_PRESENT_IMPL) right after each successful
 * present_frame -- with FIFO the present / acquire blocks on the swap chain, so
 * this follows scan-out closely; an even 60 fps reads p50 16.7 / p99 ~17. Every
 * 5 s on that thread:
 *   [hitch] present 5 s: N presents, interval p50/p99/max, sd, uneven M
 *   (|interval - median| > 4 ms), and at a fixed PORT_DISPLAY_HZ refresh how many
 *   frames would be held 1 / 2 / 3 / 4+ refreshes ("if no VRR": with G-Sync the
 *   display follows the interval instead). A mix of 2 and 3 = judder.
 * Per frame, the longest interval goes into the [hitch] line as present-iv. */
#if defined(__cplusplus) && defined(PORT_HITCH_PRESENT_IMPL)
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <algorithm>
#include <vector>
static inline void port_hitch_present(void)
{
    static uint64_t last = 0, t_sum = 0;
    static std::vector<float> iv;
    if (g_port_hitch.off) return;
    const uint64_t now = __rdtsc();
    const double k = g_port_hitch.tsc_per_ns;
    if (!last || k <= 0.0) { last = now; t_sum = now; return; }
    const int64_t d = (int64_t)(now - last);
    last = now;
    port_hitch_max_ticks(PH_PRESENT_IV, d);
    const double ms = (double)d / (k * 1e6);
    if (ms < 1000.0) iv.push_back((float)ms);   /* a load / pause gap is not pacing */
    if ((double)(now - t_sum) < k * 5e9) return;
    t_sum = now;
    if (iv.size() < 8) { iv.clear(); return; }
    std::vector<float> s(iv);
    std::sort(s.begin(), s.end());
    const size_t n = s.size();
    const float p50 = s[n / 2], p99 = s[(std::min)(n - 1, (n * 99) / 100)], mx = s[n - 1];
    double mean = 0.0, sd = 0.0;
    for (float v : s) mean += v;
    mean /= (double)n;
    unsigned uneven = 0;
    for (float v : iv) { sd += (v - mean) * (v - mean); if (fabs(v - p50) > 4.0) uneven++; }
    double hz = 0.0;
    if (const char* h = getenv("PORT_DISPLAY_HZ")) hz = atof(h);
    char held[112] = "";
    if (hz >= 20.0) {
        unsigned r[4] = { 0, 0, 0, 0 };
        const double per = 1000.0 / hz;
        for (float v : iv) { long c = lround(v / per); c = c < 1 ? 1 : c > 4 ? 4 : c; r[c - 1]++; }
        snprintf(held, sizeof held, " | at %.0f Hz if no VRR, refreshes held 1/2/3/4+: %u/%u/%u/%u", hz, r[0], r[1], r[2], r[3]);
    }
    port_log_async(1, "[hitch] present 5 s: %zu presents (%.1f/s), interval p50 %.1f p99 %.1f max %.1f ms (sd %.2f), uneven %u%s\n",
            n, 1000.0 / mean, p50, p99, mx, sqrt(sd / (double)n), uneven, held);
    iv.clear();
}
#endif

/* ------------------------------------------------------------------------- */
#ifdef PORT_HITCH_IMPLEMENTATION
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <vector>
#include "../../runtime/syscalls/ps3_waitstat.h"

extern "C" void port_hitch_frame(double target_fps, int discard)
{
    static int init = 0, on = 1;
    static double thr_ms = 25.0;
    static LARGE_INTEGER qf, q0;
    static uint64_t tsc0 = 0, last = 0, sum_t0 = 0;
    static std::vector<float> win;
    static unsigned n_over = 0, n_2x = 0, lines_s = 0, dropped = 0, n_disc = 0;
    static uint64_t line_win_t0 = 0;
    static ps3_waitstat* ws = NULL;
    static unsigned long long ws_prev4[4] = { 0, 0, 0, 0 };
    static ULONG64 cyc_prev = 0;
    static double sm_game = 0, sm_gw = 0, sm_cpu = 0, sm_fifo = 0, sm_label = 0, sm_zcull = 0, sm_hard = 0, sm_rlim = 0, sm_sub = 0, sm_lim = 0;
    static double sm_mrsx = 0, sm_ring = 0, sm_av = 0;
    static double prev_ft = 0.0;
    if (!init) {
        init = 1;
        const char* e = getenv("PORT_HITCH_LOG");
        on = !(e && *e == '0');
        g_port_hitch.off = !on;
        if (const char* m = getenv("PORT_HITCH_MS")) { const double v = atof(m); if (v >= 5.0) thr_ms = v; }
        QueryPerformanceFrequency(&qf);
        QueryPerformanceCounter(&q0);
        tsc0 = __rdtsc();
        last = tsc0; sum_t0 = tsc0; line_win_t0 = tsc0;
        g_port_hitch.main_tid = GetCurrentThreadId();
        win.reserve(1024);
        if (on) fprintf(stderr, "[hitch] logger on: frames over %.0f ms (or 2x the %.1f fps target) are listed with a breakdown; "
                                "5 s summary p50/p99/max (PORT_HITCH_LOG=0 off, PORT_HITCH_MS=<ms>)\n", thr_ms, target_fps);
        ws = ps3_waitstat_self();
    }
    if (!on) return;
    const uint64_t now = __rdtsc();
    {   /* TSC rate against QPC over the whole run (stable after the first second) */
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        const double ns = (double)(q.QuadPart - q0.QuadPart) * 1e9 / (double)qf.QuadPart;
        if (ns > 2e8) g_port_hitch.tsc_per_ns = (double)(now - tsc0) / ns;
    }
    const double k = g_port_hitch.tsc_per_ns;
    double ms[PH_COUNT], mxms[PH_COUNT]; long cnt[PH_COUNT];
    for (int i = 0; i < PH_COUNT; i++) {
        const int64_t a = _InterlockedExchange64((volatile long long*)&g_port_hitch.acc[i], 0);
        const int64_t m = _InterlockedExchange64((volatile long long*)&g_port_hitch.mx[i], 0);
        cnt[i] = _InterlockedExchange(&g_port_hitch.cnt[i], 0);
        ms[i] = k > 0.0 ? (double)a / (k * 1e6) : 0.0;
        mxms[i] = k > 0.0 ? (double)m / (k * 1e6) : 0.0;
    }
    /* the main thread's own guest waits (usleep polls, lwmutex / lwcond) from the [thrcpu] accounting:
     * spin = usleep < 1 ms (poll loops), sleep = usleep >= 1 ms */
    double ws_ms = 0.0, ws_spin = 0.0, ws_sleep = 0.0, ws_lwm = 0.0, ws_lwc = 0.0;
    if (ws) {
        const double f = 1000.0 / (double)qf.QuadPart;
        const unsigned long long s0 = ws->short_qpc, s1 = ws->long_qpc, s2 = ws->lwm_block_qpc, s3 = ws->lwc_wait_qpc;
        ws_spin = (double)(s0 - ws_prev4[0]) * f; ws_sleep = (double)(s1 - ws_prev4[1]) * f;
        ws_lwm = (double)(s2 - ws_prev4[2]) * f;  ws_lwc = (double)(s3 - ws_prev4[3]) * f;
        ws_prev4[0] = s0; ws_prev4[1] = s1; ws_prev4[2] = s2; ws_prev4[3] = s3;
        ws_ms = ws_spin + ws_sleep + ws_lwm + ws_lwc;
    }
    /* the guest call site the frame loop waited at longest */
    uint32_t top_lr = 0; double top_ms = 0.0;
    for (int i = 0; i < PORT_HITCH_LR_SLOTS; i++) {
        const double v = (double)g_port_hitch.lr_qpc[i] * 1000.0 / (double)qf.QuadPart;
        if (v > top_ms) { top_ms = v; top_lr = g_port_hitch.lr[i]; }
        g_port_hitch.lr[i] = 0; g_port_hitch.lr_qpc[i] = 0;
    }
    /* CPU time the frame-loop thread actually ran: "game" minus this = preempted / not scheduled */
    double cpu_ms = -1.0;
    {
        ULONG64 cyc = 0;
        if (QueryThreadCycleTime(GetCurrentThread(), &cyc) && k > 0.0) {
            if (cyc_prev) cpu_ms = (double)(cyc - cyc_prev) / (k * 1e6);
            cyc_prev = cyc;
        }
    }
    const double ft = k > 0.0 ? (double)(now - last) / (k * 1e6) : 0.0;
    last = now;
    if (k <= 0.0 || discard) { n_disc += discard ? 1 : 0; return; }

    win.push_back((float)ft);
    const double waits = ms[PH_LIMIT] + ms[PH_TICK] + ms[PH_SYNC] + ws_ms + ms[PH_PAD] + ms[PH_MAINRSX];
    const double game = (std::max)(0.0, ft - waits);
    {   /* 5 s means, so the steady state (not only the hitches) shows where the frame goes */
        sm_game += game; sm_gw += ws_ms; sm_cpu += cpu_ms > 0.0 ? cpu_ms : 0.0;
        sm_fifo += ms[PH_FIFOIDLE]; sm_label += ms[PH_LABEL]; sm_zcull += ms[PH_ZCULL]; sm_hard += ms[PH_HARDSYNC];
        sm_rlim += ms[PH_RSXLIMIT]; sm_sub += cnt[PH_SUBMIT]; sm_lim += ms[PH_LIMIT];
        sm_mrsx += ms[PH_MAINRSX]; sm_ring += ms[PH_RING]; sm_av += ms[PH_AVFLUSH];
    }
    const double period = target_fps > 0.0 ? 1000.0 / target_fps : 0.0;
    const double thr = (std::max)(thr_ms, period * 1.5);
    const int over2x = period > 0.0 && ft > 2.0 * period;
    if (ft > thr || over2x) {
        n_over += ft > thr; n_2x += over2x;
        if ((double)(now - line_win_t0) > k * 1e9) { line_win_t0 = now; lines_s = 0; }
        if (lines_s++ < 20) {
            char cpu[32] = "";
            if (cpu_ms >= 0.0) snprintf(cpu, sizeof cpu, " (cpu %.1f)", cpu_ms);
            port_log_async(1, "[hitch] %.1f ms%s (prev %.1f) | game %.1f%s limiter %.1f extras %.1f sync %.1f guestwait %.1f"
                            " (spin %.1f sleep %.1f lwm %.1f lwc %.1f, top 0x%08X %.1f) pad %.2f on-rsx %.1f"
                            " | rsx-iv %.1f present %.1f (acquire %.1f gpu-wait %.1f dlss %.1f) fifo-idle %.1f label %.1f/%ld zcull %.1f/%ld"
                            " hard-sync %.1f/%ld submits %ld rsx-limit %.1f pipe %.1f/%ld tex %.1f/%ld (max %.1f)"
                            " | guest threads on rsx: ring %.1f/%ld av-flush %.1f/%ld | audio-wait %.1f | present-iv %.1f/%ld\n",
                    ft, over2x ? " >2x" : "", prev_ft, game, cpu, ms[PH_LIMIT], ms[PH_TICK], ms[PH_SYNC], ws_ms,
                    ws_spin, ws_sleep, ws_lwm, ws_lwc, top_lr, top_ms, ms[PH_PAD], ms[PH_MAINRSX],
                    mxms[PH_RSX], ms[PH_PRESENT], ms[PH_ACQUIRE], ms[PH_GPUWAIT], ms[PH_DLSS], ms[PH_FIFOIDLE],
                    ms[PH_LABEL], cnt[PH_LABEL], ms[PH_ZCULL], cnt[PH_ZCULL], ms[PH_HARDSYNC], cnt[PH_HARDSYNC], cnt[PH_SUBMIT],
                    ms[PH_RSXLIMIT], ms[PH_PIPE], cnt[PH_PIPE],
                    ms[PH_TEX], cnt[PH_TEX], mxms[PH_TEX], ms[PH_RING], cnt[PH_RING], ms[PH_AVFLUSH], cnt[PH_AVFLUSH],
                    ms[PH_AUDIO], mxms[PH_PRESENT_IV], cnt[PH_PRESENT_IV]);
        } else dropped++;
    }
    prev_ft = ft;
    if ((double)(now - sum_t0) >= k * 5e9 && !win.empty()) {
        std::vector<float> s(win);
        std::sort(s.begin(), s.end());
        const size_t n = s.size();
        const float p50 = s[n / 2], p99 = s[(std::min)(n - 1, (n * 99) / 100)], mx = s[n - 1];
        double sd = 0.0, mean = 0.0;
        for (float v : s) mean += v;
        mean /= (double)n;
        for (float v : win) sd += (v - mean) * (v - mean);
        const double dn = (double)n;
        char extra[64] = "";
        if (dropped) snprintf(extra, sizeof extra, " (%u lines not shown)", dropped);
        port_log_async(1, "[hitch] 5 s: %zu frames, frame time p50 %.1f p99 %.1f max %.1f ms (sd %.2f) | over %.0f ms: %u, over 2x target: %u%s"
                          " | per frame: game %.1f (cpu %.1f) guestwait %.1f on-rsx %.2f limiter %.1f | rsx fifo-idle %.1f label %.1f zcull %.2f hard-sync %.2f"
                          " rsx-limit %.1f submits %.1f | guest threads on rsx: ring %.2f av-flush %.2f\n",
                n, p50, p99, mx, sqrt(sd / dn), thr, n_over, n_2x, extra,
                sm_game / dn, sm_cpu / dn, sm_gw / dn, sm_mrsx / dn, sm_lim / dn, sm_fifo / dn, sm_label / dn, sm_zcull / dn, sm_hard / dn,
                sm_rlim / dn, sm_sub / dn, sm_ring / dn, sm_av / dn);
        sm_game = sm_gw = sm_cpu = sm_fifo = sm_label = sm_zcull = sm_hard = sm_rlim = sm_sub = sm_lim = 0.0;
        sm_mrsx = sm_ring = sm_av = 0.0;
        win.clear(); n_over = n_2x = dropped = n_disc = 0;
        sum_t0 = now;
    }
}
#endif /* PORT_HITCH_IMPLEMENTATION */

#endif /* PORT_HITCH_H */
