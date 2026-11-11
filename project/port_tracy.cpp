/* port_tracy.cpp -- Tracy timeline zones for a PORT_TRACY build (see
 * ps3recomp/include/ps3emu/port_tracy.h). Only compiled with -DPORT_TRACY=ON.
 *
 * Zones are written straight into Tracy's per-thread queue, so a span can be
 * sent after it ended with its real start time (spin-waits are only known to
 * be over once the laps stop). To keep each thread's zones in time order, a
 * pending spin span is flushed before any other zone of that thread starts or
 * ends. TRACY_ON_DEMAND is on: nothing is recorded until the viewer connects. */
#ifdef PORT_TRACY
#include "ps3emu/port_tracy.h"

#include <tracy/Tracy.hpp>
#include <client/TracyProfiler.hpp>

#include <windows.h>
#include <stdlib.h>

namespace tracy {

static const SourceLocationData s_pt_loc[PT_KIND_COUNT] = {
    { "syscall",  "lv2 syscall (value = number)",          __FILE__, (uint32_t)__LINE__, 0x4060A0 },
    { "hle",      "HLE import (value = NID)",              __FILE__, (uint32_t)__LINE__, 0x40A060 },
    { "spu job",  "WWS SPU job (value = overlay << 32 | entry pc)", __FILE__, (uint32_t)__LINE__, 0xC08030 },
    { "spu wait", "SPU blocking channel wait (value = channel)", __FILE__, (uint32_t)__LINE__, 0x606060 },
    { "spin",     "guest spin-wait (value = laps)",        __FILE__, (uint32_t)__LINE__, 0xC03030 },
};

namespace {

struct pt_thread {
    uint64_t conn[32];   /* connection id per open live zone, 0 = not sent */
    int depth;
    int64_t sp_t0, sp_last; uint64_t sp_laps; int sp_kind;
};
thread_local pt_thread t_pt;

int64_t s_gap_ticks = 0;     /* laps further apart than this start a new spin span */
int64_t s_min_ticks = 0;     /* spin spans shorter than this are dropped */
int64_t s_max_ticks = 0;     /* a spin longer than this is sent in pieces */

inline uint64_t pt_conn()
{
#ifdef TRACY_ON_DEMAND
    return GetProfiler().IsConnected() ? GetProfiler().ConnectionId() + 1 : 0;
#else
    return 1;
#endif
}

inline void pt_emit_begin(int kind, int64_t t, uint64_t value)
{
    {
        TracyQueuePrepare( QueueType::ZoneBegin );
        MemWrite( &item->zoneBegin.time, t );
        MemWrite( &item->zoneBegin.srcloc, (uint64_t)&s_pt_loc[kind] );
        TracyQueueCommit( zoneBeginThread );
    }
    {
        TracyQueuePrepare( QueueType::ZoneValue );
        MemWrite( &item->zoneValue.value, value );
        TracyQueueCommit( zoneValueThread );
    }
}

inline void pt_emit_end(int64_t t)
{
    TracyQueuePrepare( QueueType::ZoneEnd );
    MemWrite( &item->zoneEnd.time, t );
    TracyQueueCommit( zoneEndThread );
}

inline void pt_flush_spin(pt_thread& s)
{
    if (!s.sp_t0) return;
    if (s.sp_last - s.sp_t0 >= s_min_ticks && pt_conn()) {
        pt_emit_begin(s.sp_kind, s.sp_t0, s.sp_laps);
        pt_emit_end(s.sp_last);
    }
    s.sp_t0 = 0;
}

/* "RSX flip" frame marks: rsx::thread lives in the prebuilt RPCS3 library,
 * so poll its last-flip timestamp instead of editing it (~0.25 ms late). */
extern "C" uint64_t r2rsx_last_flip_time(void);

DWORD WINAPI pt_frame_poller(void*)
{
    SetThreadDescription(GetCurrentThread(), L"tracy flip poller");
    HANDLE tm = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    uint64_t last = 0;
    for (;;) {
        if (tm) {
            LARGE_INTEGER due; due.QuadPart = -2500;    /* 250 us */
            SetWaitableTimer(tm, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(tm, 5);
        } else {
            Sleep(1);
        }
        const uint64_t f = r2rsx_last_flip_time();
        if (f && f != last) { last = f; FrameMarkNamed("RSX flip"); }
    }
}

struct pt_init {
    pt_init()
    {
        LARGE_INTEGER qf, q0, q1; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
        const int64_t c0 = Profiler::GetTime();
        Sleep(10);
        QueryPerformanceCounter(&q1);
        const int64_t c1 = Profiler::GetTime();
        const double us = double(q1.QuadPart - q0.QuadPart) * 1e6 / double(qf.QuadPart);
        const double per_us = us > 0 ? double(c1 - c0) / us : 1000.0;
        const char* e = getenv("PORT_TRACY_SPIN_GAP_US");
        const double gap = e ? atof(e) : 20.0;
        const char* m = getenv("PORT_TRACY_SPIN_MIN_US");
        const double mn = m ? atof(m) : 5.0;
        s_gap_ticks = int64_t(gap * per_us);
        s_min_ticks = int64_t(mn * per_us);
        s_max_ticks = int64_t(20000.0 * per_us);
        if (s_gap_ticks < 1) s_gap_ticks = 1;
        const char* fp = getenv("PORT_TRACY_FRAMES");
        if (!fp || atoi(fp) != 0) {
            HANDLE h = CreateThread(nullptr, 0, pt_frame_poller, nullptr, 0, nullptr);
            if (h) CloseHandle(h);
        }
    }
} s_pt_init;

} // namespace
} // namespace tracy

using namespace tracy;

extern "C" uint64_t port_tracy_now(void) { return (uint64_t)Profiler::GetTime(); }

extern "C" void port_tracy_begin(int kind, uint64_t value)
{
    pt_thread& s = t_pt;
    pt_flush_spin(s);
    const uint64_t c = pt_conn();
    if (s.depth < 32) s.conn[s.depth] = c;
    s.depth++;
    if (c) pt_emit_begin(kind, Profiler::GetTime(), value);
}

extern "C" void port_tracy_end(void)
{
    pt_thread& s = t_pt;
    pt_flush_spin(s);
    if (s.depth <= 0) return;
    s.depth--;
    const uint64_t c = s.depth < 32 ? s.conn[s.depth] : 0;
    if (c && c == pt_conn()) pt_emit_end(Profiler::GetTime());
}

extern "C" void port_tracy_span(int kind, uint64_t t0, uint64_t t1, uint64_t value)
{
    pt_thread& s = t_pt;
    pt_flush_spin(s);
    if (!pt_conn()) return;
    pt_emit_begin(kind, (int64_t)t0, value);
    pt_emit_end((int64_t)t1);
}

extern "C" void port_tracy_spin_lap(int kind)
{
    pt_thread& s = t_pt;
    const int64_t now = Profiler::GetTime();
    if (s.sp_t0 && (now - s.sp_last > s_gap_ticks || now - s.sp_t0 > s_max_ticks || kind != s.sp_kind))
        pt_flush_spin(s);
    if (!s.sp_t0) { s.sp_t0 = now; s.sp_laps = 0; s.sp_kind = kind; }
    s.sp_last = now;
    s.sp_laps++;
}
#endif /* PORT_TRACY */
