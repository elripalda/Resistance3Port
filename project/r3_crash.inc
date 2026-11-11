/* Resistance port: crash and hang reports for play-testing.
 *
 * The work is done by port_crash.h (shared by the R1/R2/R3 ports): a crash
 * writes logs\crash\R3_<time>_crash.dmp (small minidump) and R3_<time>_crash.txt
 * (exception, faulting module + offset, lifted guest function, every thread's
 * native stack, the end of run.log, the settings), adds a line to
 * logs\crash\crashes.txt and lets the process die as before (after the filter
 * that was installed before it). A hang -- the game loop, r2_game_frames,
 * counted in the stubs.cpp frame limiter, stops -- writes R3_<time>_hang.dmp/.txt
 * once per stall and keeps running.
 *   R2_CRASH_DUMPS=0 / PORT_CRASH_DUMPS=0          both off
 *   R2_CRASH_DUMPS=full (or CRASH_DUMP_FULL=1)      full-memory dumps (several GB)
 *   PORT_HANG_DUMP=1   strict watchdog: 20 s (PORT_HANG_S) without a game frame,
 *                      or without an RSX flip while frames run (not when paused)
 *   PORT_HANG_DUMP=0   no watchdog; unset: game frames only, R2_HANG_S (90) s
 * Included after vm_commit_veh: the guest arena (vm_base) is known. */
#ifdef _WIN32
#include "port_crash.h"

extern "C" volatile LONG r2_game_frames = 0;   /* stubs.cpp frame limiter */
extern "C" volatile int g_r2pc_paused;         /* r3_pcport.cpp: focus pause / quit prompt */
extern "C" uint32_t ppu_prof_resolve_host(void* ra);   /* ppu_loader.cpp: host pc -> lifted function */
extern "C" int r2rsx_enabled(void);
extern "C" uint64_t r2rsx_last_flip_time(void);
extern "C" void rsx_null_refresh_restore_on_crash(void);   /* rsx_null_backend.c: PORT_REFRESH_SWITCH */

static uint64_t r3_crash_frames(void) { return (uint64_t)(ULONG)r2_game_frames; }
static uint64_t r3_crash_rsx(void) { return r2rsx_enabled() ? r2rsx_last_flip_time() : 0; }
static int r3_crash_paused(void) { return g_r2pc_paused != 0; }

static void r2_crash_init(void)
{
    PortCrashConfig c = {};
    c.tag = "R3";
    c.title = "Resistance 3";
    c.env_prefix = "R2";
    c.main_progress = r3_crash_frames;
    c.main_start_min = 1;
    c.rsx_progress = r3_crash_rsx;
    c.paused = r3_crash_paused;
    c.vm_base = &vm_base;
    c.vm_size = VM_SIZE;
    c.guest_resolve = ppu_prof_resolve_host;
    c.before_report = rsx_null_refresh_restore_on_crash;
    port_crash::init(c);
}
#else
extern "C" volatile LONG r2_game_frames = 0;
static void r2_crash_init(void) {}
#endif