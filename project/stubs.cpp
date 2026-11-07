/*
 * ps3recomp game project -- game-specific hooks and overrides.
 *
 * main.cpp is the runner and is game-agnostic. This file is where a port puts
 * everything that is not: the PRX loading hook it must answer, overrides for
 * firmware functions the HLE library gets wrong for this title, and patches
 * over recompiled code.
 *
 * It starts out as the empty version of all of that, which is what a game with
 * no lifted PRX and no overrides needs.
 */

#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <unordered_map>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

/* ---------------------------------------------------------------------------
 * PRX loading
 *
 * main.cpp calls this once, after the lifted function table is registered and
 * vm_base is live, before the game runs. A title that lifts a real system PRX
 * -- libsre, which is cellSpurs and cellSync -- loads its image into guest RAM
 * here and registers its exports. Most ports never need to: the HLE library
 * answers those modules instead.
 * -----------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * Resistance port: sys_game_process_exitspawn
 *
 * The game is two programs: EBOOT.BIN (front end / menus) exitspawns
 * /dev_bdvd/PS3_GAME/USRDIR/game.self to start a level, and game.self can
 * exitspawn back. Each is lifted into its own executable, so exitspawn starts
 * the other executable with the new process's argv/envp/data (handed over
 * through PS3_BOOT_PATH / PS3_ARGV / PS3_ENVP / PS3_EXITSPAWN_DATA, which the
 * runtime's loader places exactly as lv2 does) and this process exits.
 * -----------------------------------------------------------------------*/
#include "ppu_recomp.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <string>

/* Debug side files in logs\ (cameras, sndsetup, gore, ...) are written only with R2_DIAG=1. */
static int r2_diag(void)
{
    static int d = -1;
    if (d < 0) { const char* e = getenv("R2_DIAG"); d = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return d;
}

extern "C" uint8_t* vm_base;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

static std::string guest_str(uint32_t ea)
{
    std::string r;
    if (!ea || !vm_base) return r;
    for (uint32_t i = 0; i < 1024; i++) { char c = (char)vm_base[ea + i]; if (!c) break; r += c; }
    return r;
}
static uint32_t guest_u32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void hle_game_process_exitspawn(ppu_context* ctx)
{
#ifdef _WIN32
    /* Only one exitspawn may win: the guest can reach it from more than one
     * thread, and a second spawn started a second copy of the game. */
    static volatile LONG s_spawning = 0;
    if (InterlockedExchange(&s_spawning, 1) != 0) { for (;;) Sleep(1000); }
#endif
    const uint32_t path_ea = (uint32_t)ctx->gpr[3], argv_ea = (uint32_t)ctx->gpr[4];
    const uint32_t envp_ea = (uint32_t)ctx->gpr[5], data_ea = (uint32_t)ctx->gpr[6];
    uint32_t data_sz = (uint32_t)ctx->gpr[7];
    std::string path = guest_str(path_ea), args, envs;
    for (int i = 0; argv_ea && i < 30; i++) {
        uint32_t p = guest_u32(argv_ea + 4u * i); if (!p) break;
        if (!args.empty()) args += ';';
        args += guest_str(p);
    }
    for (int i = 0; envp_ea && i < 30; i++) {
        uint32_t p = guest_u32(envp_ea + 4u * i); if (!p) break;
        if (!envs.empty()) envs += ';';
        envs += guest_str(p);
    }
    fprintf(stderr, "[exitspawn] path=\"%s\" argv=\"%s\" envp=\"%s\" data=0x%08X size=0x%X prio=%d flags=0x%llX\n",
            path.c_str(), args.c_str(), envs.c_str(), data_ea, data_sz, (int)ctx->gpr[8],
            (unsigned long long)ctx->gpr[9]);
    fflush(stderr); fflush(stdout);
#ifdef _WIN32
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    std::string root(exe);
    root = root.substr(0, root.find_last_of("\\/"));      /* ...\build or ...\build_game */
    root = root.substr(0, root.find_last_of("\\/"));      /* ...\Resistance2Port         */
    std::string lower = path; for (auto& c : lower) c = (char)tolower((unsigned char)c);
    std::string target_exe, target_elf;
    /* Resistance 2: EBOOT.BIN and game.self are the same program */
    if (lower.find("game.self") != std::string::npos || lower.find("eboot.bin") != std::string::npos) {
        target_exe = root + "\\build\\Resistance3.exe";
        target_elf = root + "\\game\\EBOOT.elf";
    } else {
        fprintf(stderr, "[exitspawn] no executable for \"%s\" -- exiting\n", path.c_str());
        fflush(stderr); ExitProcess(0);
    }
    std::string datafile;
    if (data_ea && data_sz) {
        if (data_sz > 0x1000) data_sz = 0x1000;
        datafile = root + "\\logs\\exitspawn_data.bin";
        FILE* f = fopen(datafile.c_str(), "wb");
        /* Always a whole 0x1000 block with the data at its START: the kernel
         * (RPCS3 sys_game exitspawn + _sys_process_exit2) hands the new process
         * exactly 0x1000 bytes at the stack top, so the title reads it from
         * top - 0x1000. Writing only data_sz bytes put it at top - data_sz,
         * and game.self booted as if from the XMB (intro movie, front end). */
        if (f) {
            static unsigned char blk[0x1000];
            memset(blk, 0, sizeof blk);
            memcpy(blk, vm_base + data_ea, data_sz);
            fwrite(blk, 1, sizeof blk, f); fclose(f);
        }
    }
    SetEnvironmentVariableA("PS3_BOOT_PATH", path.c_str());
    SetEnvironmentVariableA("PS3_ARGV", args.empty() ? NULL : args.c_str());
    SetEnvironmentVariableA("PS3_ENVP", envs.empty() ? NULL : envs.c_str());
    SetEnvironmentVariableA("PS3_EXITSPAWN_DATA", datafile.empty() ? NULL : datafile.c_str());
    /* CHILD_<NAME>=<v> in this process becomes <NAME>=<v> in the spawned one:
     * diagnostics meant only for game.self (frame captures by frame number). */
    { std::vector<std::pair<std::string, std::string>> kv;
      LPCH env = GetEnvironmentStringsA();
      for (LPCH e = env; e && *e; e += strlen(e) + 1)
          if (!strncmp(e, "CHILD_", 6)) {
              const char* eq = strchr(e + 6, '=');
              if (eq) kv.emplace_back(std::string(e + 6, (size_t)(eq - (e + 6))), std::string(eq + 1));
          }
      if (env) FreeEnvironmentStringsA(env);
      for (auto& p : kv) {
          SetEnvironmentVariableA(p.first.c_str(), p.second.c_str());
          fprintf(stderr, "[exitspawn] child env %s=%s\n", p.first.c_str(), p.second.c_str());
      } }
    std::string cmd = "\"" + target_exe + "\" \"" + target_elf + "\"";
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    ZeroMemory(&pi, sizeof pi);
    std::vector<char> cl(cmd.begin(), cmd.end()); cl.push_back(0);
    if (CreateProcessA(target_exe.c_str(), cl.data(), NULL, NULL, TRUE, 0, NULL, root.c_str(), &si, &pi)) {
        fprintf(stderr, "[exitspawn] started %s\n", target_exe.c_str());
        fflush(stderr);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        fprintf(stderr, "[exitspawn] FAILED to start %s (error %lu)\n", target_exe.c_str(), GetLastError());
        fflush(stderr);
    }
    ExitProcess(0);
#else
    exit(0);
#endif
}

/* The toolkit's live debug console + hang watchdog (from runtime/ppu/tests/
 * boot_main.cpp, which the template runner does not include):
 *   PS3_DEBUG=<file>  write a command (threads / stat / hle / mem <hex> [len])
 *                     into <file>; the answer is appended to <file>.out
 *   WATCHDOG_AT=a,b   dump every thread's stack at a and b seconds */
#include <cstdarg>
#include "debugtools.inc"

/* Resistance port: PHYS_ABANDON (default on). The physics job waits print
 * "Physics SPU ... Timed Out!" and then wait forever for a job that will
 * never finish (its SPU input is corrupt). Abandoning the batch keeps the
 * game running; that frame's physics results are dropped. PHYS_ABANDON=0
 * restores the original wait. */
extern "C" void spu_phys_trace_dump(const char* path);
extern "C" int port_phys_abandon(const char* where)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("PHYS_ABANDON"); en = e ? atoi(e) : 1; }
    if (!en) return 0;
    static long n = 0;
    if (++n <= 20 || (n % 100) == 0)
        fprintf(stderr, "[phys-abandon] %s: job batch timed out -- abandoned (%ld so far)\n", where, n);
    /* v83 diag: the first 3 timeouts dump the global physics/job DMA trace */
    if (n <= 3) {
        char fn[64]; snprintf(fn, sizeof fn, "logs/phys_timeout_%ld.txt", n);
        spu_phys_trace_dump(fn);
        fprintf(stderr, "[phys-trace] dumped %s (%s)\n", fn, where);
    }
    return 1;
}
/* Resistance port (v84): atomic WWS job-list reset.
 *
 * The job library's list reset (func_0052904C / func_0052A4D0 / func_0052A564)
 * writes the 128-byte list line with plain stores, header first: slots = FFFF,
 * header {A, B} = 0, then entries[0..11] = 0. A policy module polling the list
 * claims entry B whenever its command is nonzero -- it does not check A. Here
 * every one of those stores goes through the lock-line slow path (the PM
 * holds a reservation on the line), so between "header = 0" and "entry 0 = 0"
 * there is room for a GETLLAR: the PM sees B = 0 with the PREVIOUS batch's
 * entry 0 still in place, claims it, and runs the solver on inputs that are
 * not ready yet. That job never finishes -> "Physics SPU Solver Timed Out",
 * the static-sound freeze. On hardware the stores gather in the PPU's L2 and
 * reach the line together. This does the same: the whole line in one
 * lock-line critical section. JOBLIST_ATOMIC=0 restores the original order. */
extern "C" void spu_coh_notify_write(uint32_t ea);
extern "C" void spu_phys_trace_raw(uint32_t spu, uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size, const uint32_t* d);
extern "C" int  spu_coh_is_reserved(uint32_t addr);
extern "C" void spu_lockline_lock(void);
extern "C" void spu_lockline_unlock(void);
extern "C" void port_joblist_reset(uint32_t list)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("JOBLIST_ATOMIC"); en = e ? atoi(e) : 1;
                  fprintf(stderr, "[joblist] atomic list reset %s\n", en ? "on" : "off (JOBLIST_ATOMIC=0)"); }
    if (!en) {
        vm_write16(list + 0x1E, 0xFFFF); vm_write16(list + 0x14, 0xFFFF);
        vm_write16(list + 0x12, 0);      vm_write16(list + 0x10, 0);
        vm_write16(list + 0x16, 0xFFFF); vm_write16(list + 0x18, 0xFFFF);
        vm_write16(list + 0x1A, 0xFFFF); vm_write16(list + 0x1C, 0xFFFF);
        for (int i = 0; i < 12; i++) vm_write64(list + 0x20 + i * 8, 0);
        return;
    }
    uint8_t* p = vm_base + list;
    spu_lockline_lock();
    memset(p + 0x10, 0x00, 4);      /* header: A = B = 0 */
    memset(p + 0x14, 0xFF, 12);     /* per-SPU slots idle */
    memset(p + 0x20, 0x00, 0x60);   /* entries 0..11 */
    if (spu_coh_is_reserved(list)) spu_coh_notify_write(list);
    spu_lockline_unlock();
    /* physics trace: spu=0xCD cmd 5E = an atomic list reset */
    uint32_t d[8] = { list, 0, 0, 0, 0, 0, 0, 0 };
    spu_phys_trace_raw(0xCD, 0x5E, 0, list, 0x80, d);
}


/* Resistance port: PS3_FTRACE spans for the game's synchronous SPU-job waits
 * (func_00070408: build a job chain, kick it, poll until done; func_000B7B8C:
 * poll a command job's status word). Recorded as SYSCALL spans with
 * a = 0x20000 | (tag & 0xFFFF), b = tag. */
#include "ps3emu/ps3_ftrace.h"
extern "C" unsigned long long port_ftw_now(void) { return PS3_FT_ON() ? ps3_ft_now() : 0ull; }
extern "C" void port_ftw_span(unsigned tag, unsigned long long t0, ppu_context* ctx)
{
    if (!t0 || !PS3_FT_ON()) return;
    ps3_ft_span(PS3_FT_SYSCALL, (uint32_t)ctx->thread_id, 0x20000u | (tag & 0xFFFFu), tag, t0, ps3_ft_now());
}

/* Resistance port: func_001C821C starts an SPU job list and polls it for
 * completion with no timeout -- every recent York freeze sits there with the
 * SPUs idle. port_jobwait_begin() marks the start of a wait; each poll that
 * reports "not done" calls port_jobwait_spin(obj). After 3 s it dumps the job
 * list (header, per-SPU counters, entries) once; with JOBWAIT_ABANDON=<sec> it
 * reports the list done after that long (default 0 = keep waiting). */
static thread_local unsigned long long s_jw_t0 = 0;
static thread_local int s_jw_dumped = 0;
extern "C" void port_jobwait_begin(void) { s_jw_t0 = GetTickCount64(); s_jw_dumped = 0; }
extern "C" int port_jobwait_spin(uint32_t obj)
{
    const unsigned long long now = GetTickCount64();
    if (!s_jw_t0) s_jw_t0 = now;
    const unsigned long long waited = now - s_jw_t0;
    if (waited >= 3000 && !s_jw_dumped) {
        s_jw_dumped = 1;
        const uint32_t list = vm_read32(obj + 4);
        fprintf(stderr, "[jobwait] 001C821C stuck %llums: obj=0x%08X list=0x%08X\n", waited, obj, list);
        fprintf(stderr, "[jobwait] obj :");
        for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", vm_read32(obj + i * 4));
        fprintf(stderr, "\n[jobwait] list:");
        for (int i = 0; i < 16; i++) fprintf(stderr, " %08X", vm_read32(list + i * 4));
        const uint32_t idx = vm_read16(list + 0x10), cnt = vm_read16(list + 0x12);
        fprintf(stderr, "\n[jobwait] next=%u lim=%u per-SPU:", idx, cnt);
        for (int i = 0; i < 6; i++) fprintf(stderr, " %u", vm_read16(list + 0x14 + i * 2));
        fprintf(stderr, "\n[jobwait] entries:");
        for (int i = 0; i < 24; i++) {
            if (i % 6 == 0) fprintf(stderr, "\n   [%2d]", i);
            fprintf(stderr, " %08X%08X", vm_read32(list + 0x20 + i * 8), vm_read32(list + 0x24 + i * 8));
        }
        fprintf(stderr, "\n");
        /* The SPURS instance the job manager runs under (obj word 0): the
         * kernel's per-workload ready counts, state, enabled/signal bits. */
        const uint32_t sp = vm_read32(obj);
        fprintf(stderr, "[jobwait] spurs 0x%08X:", sp);
        for (int i = 0; i < 0xC0; i += 4) {
            if (i % 32 == 0) fprintf(stderr, "\n   +%03X", i);
            fprintf(stderr, " %08X", vm_read32(sp + i));
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    static int ab = -1;
    if (ab < 0) { const char* e = getenv("JOBWAIT_ABANDON"); ab = e ? atoi(e) : 5; }
    if (ab > 0 && waited >= (unsigned long long)ab * 1000ull) {
        static long n = 0;
        if (++n <= 20) fprintf(stderr, "[jobwait] abandoning job list 0x%08X after %llums (%ld)\n", vm_read32(obj + 4), waited, n);
        s_jw_t0 = 0;
        return 1;
    }
    return 0;
}
/* Resistance port: func_000508A8 waits for an SPU job to set bit 0x2 of a
 * status word, warning once a second and never giving up. When the job is lost
 * the game froze there with every SPU idle. port_flagwait_begin() at entry;
 * port_flagwait_timeout() on each 1 s warning returns 1 after FLAGWAIT_ABANDON
 * seconds (default 3, 0 = wait forever) and the wait is treated as done. */
static thread_local int s_fw_n = 0;
extern "C" void port_flagwait_begin(void) { s_fw_n = 0; }
extern "C" int port_flagwait_timeout(const char* where)
{
    static int lim = -1;
    if (lim < 0) { const char* e = getenv("FLAGWAIT_ABANDON"); lim = e ? atoi(e) : 3; }
    ++s_fw_n;
    if (!lim || s_fw_n < lim) return 0;
    static long n = 0;
    if (++n <= 20 || (n % 100) == 0)
        fprintf(stderr, "[flagwait] %s: SPU flag never set after %d s -- carrying on (%ld so far)\n", where, s_fw_n, n);
    s_fw_n = 0;
    return 1;
}
/* ---------------------------------------------------------------------------
 * Resistance port (v126): frame rate.
 *
 * The game clock (0x10330B40) clamps every frame's step to [min, max] (clock
 * +4 / +8, func_001EAA40). Gameplay sets both to 1/30, so the simulation
 * advances a fixed 1/30 s per frame. The frame limiter (func_001E5D0C) waits
 * until 0.9 x min has passed since the previous flip, then flips; on the PS3
 * the flip itself then waits in the RSX for label 68, which the game's vblank
 * handler raises every 2nd vblank, and that made it exactly 30 fps. That
 * acquire does not block here (GCM_SEMA_ACQUIRE=0), so the limiter alone set
 * the pace: 0.9 x 1/30 = 30 ms -- 33 fps with 1/30 steps, the game running
 * 10% fast.
 *
 * PORT_FPS=<n> (default 60): min step = 1/n and the limiter waits the full
 * step, precisely (host wait, not 500 us spins). The game's max stays (1/30 in
 * gameplay), so the step is the real frame time anywhere between n and 30 fps
 * and the game runs at real speed. PORT_FPS=30 is the original 30 fps without
 * the 10% overspeed. PORT_FPS=0 = uncapped (240). PORT_FPS=orig = untouched.
 * PORT_DT_MAX=<s> raises the max step (e.g. 0.05: real speed down to 20 fps;
 * the game itself slows down below 30 fps otherwise, as on the PS3).
 * Above 60 the PPU-side cellGcmSetWaitFlip would hold every frame for the
 * 60 Hz flip beat, so it stops blocking there.
 * -----------------------------------------------------------------------*/
extern "C" uint64_t ppu_timebase_now(void);
extern "C" void ppu_gcm_pump(void);
extern "C" void cellGcm_set_waitflip_block(int on);
static volatile int s_rf_mode = -2;    /* -2 unset, -1 original, else the fps target */
static float s_rf_min = 1.0f / 60.0f;  /* min step (s) */
static float s_rf_max = 0.0f;          /* max step floor (s), 0 = the game's own */
static void port_fps_init(void)
{
    if (s_rf_mode != -2) return;
    int m = 60;
    const char* e = getenv("PORT_FPS");
    if (e && *e) {
        if (!_stricmp(e, "orig") || !_stricmp(e, "original") || !_stricmp(e, "off")) m = -1;
        else { m = atoi(e); if (m <= 0) m = 240; }
    }
    if (m > 0 && m < 30) m = 30;
    if (m > 1000) m = 1000;
    const char* x = getenv("PORT_DT_MAX");
    float mx = x ? (float)atof(x) : 0.0f;
    if (mx < 0.0f) mx = 0.0f;
    if (mx > 0.1f) mx = 0.1f;
    s_rf_min = m > 0 ? 1.0f / (float)m : 0.0f;
    s_rf_max = mx;
    if (m > 60) cellGcm_set_waitflip_block(0);
    s_rf_mode = m;
    if (m < 0) fprintf(stderr, "[fps] PORT_FPS=orig: the game's own pacing (33 fps, 1/30 steps)\n");
    else fprintf(stderr, "[fps] target %d fps: min step %.5f s, max step %s%s\n", m, (double)s_rf_min,
                 mx > 0.0f ? "raised to " : "the game's", mx > 0.0f ? x : "");
}
/* v170: scripted player sequences (a chapter's opening walk: input locked,
 * the game walks and turns the player) are timed in frames, and at high frame
 * rates they ended after a fraction of the walk. While one runs (player byte
 * +0x3F07 == 1, set by port_mouselook_pre) the game keeps its own 1/30 step,
 * i.e. the original 30 fps; full rate returns with control. PORT_SCRIPT30=0 off. */
static volatile int g_port_scripted;
extern "C" volatile unsigned long long g_port_play_ms;
extern "C" double port_dt_min(uint32_t bits)
{
    port_fps_init();
    float v; memcpy(&v, &bits, 4);
    if (s_rf_mode < 0 || !(v > 0.0f)) return (double)v;
    if (g_port_scripted) return (double)v;
    /* v174: menus over the game (pause, options) at 60 fps. At 165 the pause
     * menu's full-screen dimming layer was left out of some frames (the game's
     * menu code skips it now and then at high rates): a visible flicker every
     * few seconds. The look code not running for 200 ms = a menu is up. */
    /* PORT_MENU_FPS=<n> (default 90; 0 = off) */
    { static int en = -1; if (en < 0) { const char* e = getenv("PORT_MENU_FPS"); en = e ? atoi(e) : 90; }
      if (en > 0 && g_port_play_ms && GetTickCount64() - g_port_play_ms > 200) {
          const float m = 1.0f / (float)en; return (double)(s_rf_min > m ? s_rf_min : (m < v ? m : v)); } }
    return (double)(s_rf_min < v ? s_rf_min : v);
}
extern "C" double port_dt_max(uint32_t bits)
{
    port_fps_init();
    float v; memcpy(&v, &bits, 4);
    if (s_rf_mode < 0 || !(v > 0.0f)) return (double)v;
    return (double)(s_rf_max > v ? s_rf_max : v);
}
extern "C" double port_limit_factor(double f)
{
    port_fps_init();
    return s_rf_mode < 0 ? f : 1.0;
}
/* The limiter's wait: until `target` timebase ticks have passed since the
 * previous flip's stamp (u64 at last_ea). 0 = run the game's own loop. */
extern "C" int port_frame_wait(uint64_t target, uint32_t last_ea)
{
    port_fps_init();
    if (s_rf_mode < 0) return 0;
    const uint64_t last = vm_read64(last_ea);
    for (;;) {
        const uint64_t now = ppu_timebase_now();
        const uint64_t el = now - last;
        if (el >= target) break;
        const double rem_us = (double)(target - el) / 79.8;
        ppu_gcm_pump();                       /* the usleep loop pumped vblank/flip work */
#ifdef _WIN32
        if (rem_us > 2600.0) Sleep((DWORD)(rem_us / 1000.0) - 1u);
        else if (rem_us > 150.0) SwitchToThread();
        else YieldProcessor();
#endif
    }
    /* frame pacing stats, every 5 s: frames, mean and worst frame time */
    { static uint64_t t0 = 0, prev = 0, worst = 0; static unsigned n = 0; static double sum = 0;
      const uint64_t now = ppu_timebase_now();
      if (prev) { const uint64_t d = now - prev; sum += (double)d; n++; if (d > worst) worst = d; }
      prev = now;
      if (!t0) t0 = now;
      if (now - t0 >= 5ull * 79800000ull && n) {
          fprintf(stderr, "[fps] 5 s: %u frames, %.1f fps, frame %.2f ms avg, %.2f ms worst (target %d)\n",
                  n, n * 79800000.0 / (double)(now - t0), sum / n / 79800.0, (double)worst / 79800.0, (int)s_rf_mode);
          t0 = now; n = 0; sum = 0; worst = 0;
      } }
    return 1;
}

/* Resistance port (v130): the movie player's "wait for my event" helper
 * (func_005E9474: event at +0x1C8, sys_cond_wait with no timeout). At the end
 * of York's l02_s019 movie the audio output component (AudioDisplayPS3) sees
 * its controller's "done" flag and parks there; the controller then takes the
 * play-next-item path, stops the components and joins AudioDisplay -- which
 * nothing ever signals: black screen, joined forever. A bounded wait returns
 * to the component's step loop, which sees the stop request and exits; while
 * nothing stops it, it simply waits again. PORT_MOVIE_EVWAIT_MS=<ms> (default
 * 100), 0 = the original unbounded wait. */
/* Resistance port (v134): field of view. The camera setter func_000B674C
 * takes the horizontal FOV in degrees (82 = the game's gameplay view, ~65 =
 * carbine zoom, ~27 = sniper scope) and derives the projection, the vertical
 * FOV (from the aspect) and the culling frustum from it, so widening it here
 * widens everything consistently. PORT_FOV=<degrees> sets the gameplay FOV;
 * the widening tapers off below 82 so zoomed views keep their zoom and the
 * zoom-in animation stays smooth. 0 / unset = the original. */
#include <cmath>
static volatile double g_port_game_fov = 82.0;   /* last FOV the game set (degrees, before widening) */
extern "C" volatile double g_port_view_fov;   /* v190e: cellPad.c scope mode reads the player's FOV */
extern "C" double port_fov_adjust(double f)
{
    static double k = -1.0;
    const double base = 82.0, zoom = 65.0, d2r = 3.14159265358979323846 / 180.0;
    if (k < 0.0) {
        const char* e = getenv("PORT_FOV");
        const double u = e ? atof(e) : 0.0;
        if (u >= 30.0 && u <= 160.0 && fabs(u - base) > 0.01) {
            k = tan(u * 0.5 * d2r) / tan(base * 0.5 * d2r);
            fprintf(stderr, "[fov] horizontal FOV %.0f (game default %.0f)%c", u, base, 10);
        } else k = 1.0;
    }
    if (k == 1.0 || !(f > 1.0 && f < 179.0)) return f;
    double t = (f - zoom) / (base - zoom);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    const double kk = 1.0 + (k - 1.0) * t;
    double r = 2.0 * atan(tan(f * 0.5 * d2r) * kk) / d2r;
    if (r > 170.0) r = 170.0;
    return r;
}
/* v182: the zoom-scaled mouse used the LAST FOV any camera was given. York has
 * other cameras (narrow ones -- spotlight/shadow views, monitors) set after the
 * player's in some places, so looking that way cut the mouse speed as if
 * zoomed ("a whole direction slows the mouse down"). Only the player's view
 * camera counts now: the camera object that is given the plain gameplay FOV
 * (82) -- re-learned whenever one is. PORT_FOV_ANYCAM=1 = old. */
static volatile uint32_t s_port_view_cam = 0;
extern "C" void port_fov_note(uint32_t cam, double f)
{
    if (!(f > 1.0 && f < 179.0)) return;
    static int any = -1; if (any < 0) any = getenv("PORT_FOV_ANYCAM") ? 1 : 0;
    if (fabs(f - 82.0) < 0.01) s_port_view_cam = cam;
    { static uint32_t seen[32]; static int ns = 0; int k = 0;   /* diag: distinct cameras */
      for (; k < ns; k++) if (seen[k] == cam) break;
      if (k == ns && ns < 32 && r2_diag()) { seen[ns++] = cam;
          FILE* fl = fopen("logs\\cameras.txt", ns == 1 ? "w" : "a");
          if (fl) { fprintf(fl, "camera 0x%08X first fov %.2f\n", cam, f); fclose(fl); } } }
    if (any || cam == s_port_view_cam || !s_port_view_cam) { g_port_game_fov = f; g_port_view_fov = f; }
}
/* The camera's +0x10 / +0x14 (horizontal / vertical FOV, radians) drive the
 * game's level-of-detail choice, not the projection: widened, distant models
 * looked smaller and switched to far LODs (York's hex windows turned into a
 * glowing blue proxy). After the setter runs, put them back to the values the
 * UNwidened FOV gives; the projection and frustum keep the wide view. */
extern "C" void port_fov_lod_fix(uint32_t cam, double f, double aspect)
{
    if (!cam || !(aspect > 0.1)) return;
    if (fabs(port_fov_adjust(f) - f) < 1e-9) return;   /* not widened */
    const double d2r = 3.14159265358979323846 / 180.0;
    const double t = tan(f * 0.5 * d2r);
    const float h = (float)(f * d2r), v = (float)(2.0 * atan(t / aspect));
    uint32_t hb, vb; memcpy(&hb, &h, 4); memcpy(&vb, &v, 4);
    vm_write32(cam + 0x10, hb);
    vm_write32(cam + 0x14, vb);
}
/* Resistance port (v139): the RANDOM STEREO SOUND. The game picks its SCREAM
 * speaker layout from a global (0x1009FF38: 2 in the ELF, set to the output's
 * channel count -- 6 here -- by the audio-out setup func_000D2FFC once
 * cellAudioInit succeeds). The sound-system setup func_000D37B0 reads it
 * through func_000D316C: 6 -> "5ch_60_120" (5.1), 8 -> 7.1, anything else ->
 * "2ch_90". The two run on different threads; when the sound setup won the
 * race it read the initial 2 and the whole session mixed in the 2ch layout
 * (only L/R, quieter, positions wrong). Return the configured layout either
 * way. PORT_SPEAKERS=6 (default) / 8 / 2 (= the game's stereo layout always). */
extern "C" uint32_t port_speakers(uint32_t v)
{
    static int sp = -1;
    if (sp < 0) { const char* e = getenv("PORT_SPEAKERS"); sp = e ? atoi(e) : 6;
                  if (sp != 2 && sp != 6 && sp != 8) sp = 6; }
    if (sp == 2) return 2;
    if (v == 2) {
        static int n = 0;
        if (n < 2) { n++; fprintf(stderr, "[audio] speaker layout read before the audio-out setup -- using %d channels%c", sp, 10); }
        return (uint32_t)sp;
    }
    return v;
}
/* Resistance port (v144, diagnostic): the sound-layout setup chain, so the
 * next launch that comes up with broken sound says which step differed.
 * logs\sndsetup.txt (appended, one block per launch): audio-out setup
 * (D2FFC), sound setup (D37B0), the speaker-count getter's raw value, the
 * SCREAM bus count (515E4C) and every speaker layout the game selects
 * (51E738, with its name). port_snd_mode adds the layout in use each time the
 * 10 s channel monitor runs. */
static uint32_t s_snd_slot = 0;
static void snd_layout_name(uint32_t elem, char* out, int n)
{
    out[0] = 0;
    if (!elem) return;
    const uint32_t s = vm_read32(elem) + 52;
    int k = 0;
    for (int i = 0; i < 6 && k < n - 1; i++) {
        const uint32_t w = vm_read32(s + 4 * i);
        for (int b = 3; b >= 0 && k < n - 1; b--) {
            const char c = (char)(w >> (8 * b));
            if (!c) { out[k] = 0; return; }
            out[k++] = (c >= 32 && c < 127) ? c : '?';
        }
    }
    out[k] = 0;
}
static void snd_log(const char* line)
{
    if (!r2_diag()) return;
    FILE* f = fopen("logs/sndsetup.txt", "a");
    if (!f) return;
    fprintf(f, "pid=%lu t=%llu tid=%lu %s\n", (unsigned long)GetCurrentProcessId(),
            (unsigned long long)GetTickCount64(), (unsigned long)GetCurrentThreadId(), line);
    fclose(f);
}
extern "C" void port_snd_note(int kind, uint32_t v, uint32_t lr, uint32_t extra)
{
    static volatile LONG cnt[8];
    if (kind == 2) s_snd_slot = extra;
    if (InterlockedIncrement(&cnt[kind & 7]) > 24) return;
    char line[160], nm[32] = "";
    static const char* const kn[8] = { "?", "busCount(515E4C)", "selectLayout(51E738)", "speakerGetter(D316C) raw",
                                       "audioOutSetup(D2FFC) enter", "?", "soundSetup(D37B0) enter", "?" };
    if (kind == 2) snd_layout_name(v, nm, sizeof nm);
    snprintf(line, sizeof line, "%s v=0x%X lr=0x%X %s", kn[kind & 7], v, lr, nm);
    snd_log(line);
}
extern "C" void port_snd_mode(int surround, double c_pct, double nan_pct)
{
    static int n = 0, last = -1;
    if (n >= 40 && surround == last) return;
    n++; last = surround;
    char nm[32] = "";
    if (s_snd_slot) snd_layout_name(vm_read32(s_snd_slot), nm, sizeof nm);
    char line[160];
    snprintf(line, sizeof line, "mode %s C=%.0f%% NaN=%.2f%% layout-in-use=%s",
             surround ? "surround" : "STEREO", c_pct, nan_pct, nm);
    snd_log(line);
}
/* Resistance port (v145): guest setjmp/longjmp. Both ELFs carry Lua, whose
 * protected calls (pcall, the menus' script calls) setjmp and whose errors
 * longjmp back. Lifted, the guest longjmp only RESTORED REGISTERS and then
 * returned into luaD_throw -- which went on into its "no handler" branch
 * with r3 = the jump buffer, read G(L) from garbage, called a NULL panic
 * function and exited the process. That is the main menu's Location Select:
 * a script error there (handled on a PS3) closed the game every time.
 * Now each lifted setjmp call site does a HOST setjmp in its own frame
 * (port_jb_arm registers it under the guest buffer address), and the lifted
 * longjmp, after restoring the guest registers, host-longjmps to it. Entries
 * are per thread; a new setjmp drops entries from frames at or below its
 * guest stack pointer (frames that have returned). */
#include <setjmp.h>
namespace {
struct PortJb { uint32_t ea, sp; int depth, vsp; jmp_buf env; };
thread_local PortJb t_jb[64];
thread_local int t_jb_n = 0;
thread_local int t_jb_land = -1;   /* entry being landed on */
}
/* ppu_loader.cpp: the indirect-call nesting count and vcall stack pointer --
 * host bookkeeping of the frames a longjmp skips, put back as they were at
 * the setjmp. */
extern "C" int* ps3_ppu_dispatch_depth_slot(void);
extern "C" int* ps3_ppu_vcall_sp_slot(void);
extern "C" jmp_buf* port_jb_arm(ppu_context* ctx)
{
    const uint32_t ea = ((uint32_t)ctx->gpr[3] + 15u) & ~15u;
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    while (t_jb_n > 0 && t_jb[t_jb_n - 1].sp <= sp) t_jb_n--;
    if (t_jb_n >= 64) {   /* cannot happen with sane nesting; keep the newest */
        memmove(&t_jb[0], &t_jb[1], sizeof(PortJb) * 63); t_jb_n = 63;
    }
    PortJb* e = &t_jb[t_jb_n++];
    e->ea = ea; e->sp = sp;
    e->depth = *ps3_ppu_dispatch_depth_slot(); e->vsp = *ps3_ppu_vcall_sp_slot();
    return &e->env;
}
extern "C" void port_jb_landed(ppu_context* ctx)
{
    if (t_jb_land >= 0 && t_jb_land < t_jb_n) {
        *ps3_ppu_dispatch_depth_slot() = t_jb[t_jb_land].depth;
        *ps3_ppu_vcall_sp_slot() = t_jb[t_jb_land].vsp;
    }
    t_jb_land = -1;
    static volatile LONG n = 0;
    if (InterlockedIncrement(&n) <= 8)
        fprintf(stderr, "[setjmp] guest longjmp landed (r3=%llu lr=0x%08llX sp=0x%08llX) -- script error handled%c",
                (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->lr, (unsigned long long)ctx->gpr[1], 10);
}
extern "C" void port_jb_jump(ppu_context* ctx, uint32_t buf_ea)
{
    for (int i = t_jb_n - 1; i >= 0; i--) {
        if (t_jb[i].ea != buf_ea) continue;
        t_jb_n = i + 1;
        t_jb_land = i;
        longjmp(t_jb[i].env, 1);
    }
    static volatile LONG n = 0;
    if (InterlockedIncrement(&n) <= 8)
        fprintf(stderr, "[setjmp] guest longjmp to 0x%08X with no live setjmp frame -- returning (old behaviour)%c",
                buf_ea, 10);
    (void)ctx;
}
/* Resistance port (v148/v149): direct mouse look -- see cellPad.c.
 * The player look code func_002A4D14 turns the right stick into rates,
 * player+976 (pitch) and +980 (yaw), and integrates them at 0x2A5060..0x2A50B8
 * into pitch (+864, clamped afterwards) and yaw (+868, wrapped).
 *  port_mouselook_pre (0x2A5060, before the integration): checks the turn
 *    direction the game derived from the stick the mouse emulation fed it --
 *    that is how the game's own invert setting is followed -- and zeroes the
 *    two rates, so the stick no longer turns the view.
 *  port_mouselook (0x2A50B8, after it, before the clamps): adds the mouse
 *    counts since the last frame to yaw and pitch.
 * The directions start from mouselook.cfg (written whenever they change;
 * defaults: the ones measured on this setup) and follow the game if it ever
 * disagrees for 15 frames running, e.g. after flipping invert in the options.
 * The game's controller sensitivity (0-9) scales it too (port_ml_game_sens). */
extern "C" volatile long g_ml_mode, g_kbm_ml_dx, g_kbm_ml_dy;
extern "C" volatile double g_ml_deg_per_count;
extern "C" volatile int g_ml_invert_y, g_ml_fed_rx, g_ml_fed_ry, g_kbm_capture, g_ml_rs_keys;
extern "C" volatile ULONGLONG g_ml_kbm_ms;
extern "C" volatile uint32_t g_port_opts_ea;
/* v151: is the right stick the mouse right now? (not a controller, not the look keys) */
static int ml_stick_is_mouse(void) { return !g_ml_rs_keys && GetTickCount64() - g_ml_kbm_ms < 200; }
static float ml_rdf(uint32_t a) { uint32_t w = vm_read32(a); float f; memcpy(&f, &w, 4); return f; }
static void ml_wrf(uint32_t a, float f) { uint32_t w; memcpy(&w, &f, 4); vm_write32(a, w); }
static int s_ml_sx = 0, s_ml_sy = 0;          /* yaw / pitch direction per positive count */
static int s_ml_bad_x = 0, s_ml_bad_y = 0;
static ULONGLONG s_ml_last = 0;
static void ml_save(void)
{
    FILE* f = fopen("mouselook.cfg", "w");
    if (f) { fprintf(f, "yaw_sign = %d\npitch_sign = %d\n", s_ml_sx, s_ml_sy); fclose(f); }
}
static void ml_load(void)
{
    if (s_ml_sx) return;
    s_ml_sx = -1; s_ml_sy = 1;   /* measured on this setup (v148 run) */
    FILE* f = fopen("mouselook.cfg", "r");
    if (f) { char l[128]; int v;
        while (fgets(l, sizeof l, f)) {
            if (sscanf(l, " yaw_sign = %d", &v) == 1 && (v == 1 || v == -1)) s_ml_sx = v;
            if (sscanf(l, " pitch_sign = %d", &v) == 1 && (v == 1 || v == -1)) s_ml_sy = v;
        }
        fclose(f); }
    fprintf(stderr, "[mouselook] direct mouse look: yaw %d, pitch %d%c", s_ml_sx, s_ml_sy, 10);
}
static void ml_check(int fed, float rate, int* sign, int* bad, const char* what)
{
    if (!(fed >= 16 || fed <= -16) || !(rate > 1e-5f || rate < -1e-5f)) return;
    const int s = ((fed > 0) == (rate > 0)) ? 1 : -1;
    if (s == *sign) { *bad = 0; return; }
    if (++*bad >= 15) {
        *sign = s; *bad = 0; ml_save();
        fprintf(stderr, "[mouselook] %s direction now %d (following the game's setting)%c", what, s, 10);
    }
}
/* v149 diag: the profile options block the look code reads its invert
 * flags from ([player+16016]+6848 -> bytes +72/+73/+116); log it when it
 * changes, to find the 0-9 sensitivity byte. */
static void ml_opts_watch(uint32_t player)
{
    const uint32_t c = vm_read32(player + 16016); if (!c) return;
    const uint32_t o = vm_read32(c + 6848); if (!o) return;
    static uint8_t last[160]; static int have = 0, n = 0;
    uint8_t cur[160]; for (int i = 0; i < 160; i += 4) { uint32_t w = vm_read32(o + i); cur[i] = w >> 24; cur[i+1] = w >> 16; cur[i+2] = w >> 8; cur[i+3] = w; }
    if (have && !memcmp(cur, last, sizeof cur)) return;
    if (r2_diag() && n++ < 40) {
        FILE* f = fopen("logs\\mlopts.txt", have ? "a" : "w");
        if (f) { fprintf(f, "t=%llu opts@0x%08X:", (unsigned long long)GetTickCount64(), o);
                 for (int i = 0; i < 160; i++) fprintf(f, "%s%02X", (i % 16) ? "" : " |", cur[i]);
                 fputc(10, f); fclose(f); }
    }
    memcpy(last, cur, sizeof last); have = 1;
}
extern "C" volatile unsigned long long g_port_play_ms;
extern "C" volatile int g_port_pause_ui;
extern "C" volatile int g_port_space_start;
/* ---------------------------------------------------------------------------
 * Resistance port (v168): falling-through-the-floor detector + rescue key.
 * Player object (the pointer port_mouselook gets), found from a 30 s trace:
 *   +0x3E80/84/88  feet position x, height, y      (+0x340.. = eye, 1.7 above)
 *   +0xF0/F4/F8    velocity (copy at +0x110)        +0x1EB  1 = on the ground
 * The detector keeps the last 3 s of the player object; when the player drops
 * more than PORT_FALL_M (default 6) below the last ground they stood on within
 * 3 s of leaving it, it writes logs\fall_<ms>.bin (same format as
 * player_trace.bin) and a line in the log. F4 puts the player back on the
 * last ground they stood on for at least half a second (about 1.5 s before
 * leaving it) -- usable any time, not only after a detected fall.
 * -------------------------------------------------------------------------*/
#define FALL_RING 270
static uint8_t  s_fall_ring[FALL_RING][16384];
static uint64_t s_fall_ring_t[FALL_RING];
static unsigned s_fall_ring_n;
typedef struct { float x, z, y; ULONGLONG t; } SafePos;
static SafePos s_safe[32]; static unsigned s_safe_n;
static void fall_watch(uint32_t player)
{
    if (!player) return;
    static int on = -1; static float lim = 6.0f;
    if (on < 0) { const char* e = getenv("PORT_FALL_M"); lim = e ? (float)atof(e) : 6.0f; on = lim > 0.0f; }
    const ULONGLONG now = GetTickCount64();
    const float fx = ml_rdf(player + 0x3E80), fz = ml_rdf(player + 0x3E84), fy = ml_rdf(player + 0x3E88);
    const int ground = vm_read8(player + 0x1EB) == 1;
    if (!(fz == fz) || fabsf(fz) > 1e5f) return;
    /* v183: snap guard. Grimsby fall capture: at the top of a jump (no hitch,
     * ~95 fps) the feet height went 76.80 -> -0.39 in ONE frame with x/y
     * unchanged; the game's ground probe (+0x24) read -5000 = no hit that
     * frame and +0x114 = the implied -7296 m/s. Not a fall -- a bad snap. A
     * vertical jump of more than 8 m between consecutive frames with under
     * 1.5 m of horizontal motion puts feet + eye back where they were and
     * zeroes vertical speed. Gives up after 20 frames in a row (a real
     * teleport). PORT_SNAP_GUARD=0 off. */
    {
        static int sg = -1; if (sg < 0) { const char* e = getenv("PORT_SNAP_GUARD"); sg = e ? atoi(e) : 1; }
        static float px, pz, py, pex, pez, pey; static ULONGLONG pt = 0; static int streak = 0; static unsigned nlog = 0;
        const float ex = ml_rdf(player + 0x340), ez = ml_rdf(player + 0x344), ey = ml_rdf(player + 0x348);
        if (sg && pt && now - pt < 200 && !g_port_scripted && streak < 20 &&
            fabsf(fz - pz) > 8.0f && fabsf(fx - px) < 1.5f && fabsf(fy - py) < 1.5f) {
            streak++;
            ml_wrf(player + 0x3E80, px); ml_wrf(player + 0x3E84, pz); ml_wrf(player + 0x3E88, py);
            ml_wrf(player + 0x340, pex); ml_wrf(player + 0x344, pez); ml_wrf(player + 0x348, pey);
            ml_wrf(player + 0xF4, 0.0f); ml_wrf(player + 0x114, 0.0f);
            if (nlog++ < 50) fprintf(stderr, "[fall] snap blocked: height %.2f -> %.2f in one frame at %.2f %.2f (probe %.1f), restored\n",
                                     (double)pz, (double)fz, (double)px, (double)py, (double)ml_rdf(player + 0x24));
            pt = now;
            return;
        }
        if (fabsf(fz - pz) <= 8.0f) streak = 0;
        px = fx; pz = fz; py = fy; pex = ex; pez = ez; pey = ey; pt = now;
    }
    static ULONGLONG ground_since = 0, left_ground = 0; static float last_ground_z = 0.0f; static int was_ground = 0;
    static ULONGLONG last_safe = 0, fired_at = 0;
    if (ground) {
        if (!was_ground) ground_since = now;
        last_ground_z = fz;
        if (now - ground_since > 500 && now - last_safe > 250) {
            s_safe[s_safe_n % 32] = { fx, fz, fy, now }; s_safe_n++; last_safe = now;
        }
    } else if (was_ground) left_ground = now;
    was_ground = ground;
    /* ring of the whole object, every other frame */
    static unsigned tick = 0;
    if (on && (++tick & 1)) {
        uint8_t* b = s_fall_ring[s_fall_ring_n % FALL_RING];
        for (uint32_t i = 0; i < 16384; i += 4) { const uint32_t w = vm_read32(player + i);
            b[i] = (uint8_t)(w >> 24); b[i+1] = (uint8_t)(w >> 16); b[i+2] = (uint8_t)(w >> 8); b[i+3] = (uint8_t)w; }
        s_fall_ring_t[s_fall_ring_n % FALL_RING] = now; s_fall_ring_n++;
    }
    if (on && !ground && left_ground && now - left_ground < 3000 && fz < last_ground_z - lim && now - fired_at > 10000) {
        fired_at = now;
        char fn[64]; snprintf(fn, sizeof fn, "logs\\fall_%llu.bin", (unsigned long long)now);
        FILE* f = fopen(fn, "wb");
        if (f) {
            const unsigned cnt = s_fall_ring_n < FALL_RING ? s_fall_ring_n : FALL_RING;
            for (unsigned k = s_fall_ring_n - cnt; k < s_fall_ring_n; k++) {
                const uint64_t ms = s_fall_ring_t[k % FALL_RING]; const uint32_t hdr[2] = { player, 0 };
                fwrite(&ms, 8, 1, f); fwrite(hdr, 4, 2, f); fwrite(s_fall_ring[k % FALL_RING], 1, 16384, f);
            }
            fclose(f);
        }
        fprintf(stderr, "[fall] player dropped %.1f below the ground it left %llu ms ago (at %.2f %.2f %.2f) -> %s. F4 = back to safe ground\n",
                (double)(last_ground_z - fz), (unsigned long long)(now - left_ground), (double)fx, (double)last_ground_z, (double)fy, fn);
    }
    /* F4: rescue */
    static int f4was = 0; const int f4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
    if (f4 && !f4was && s_safe_n) {
        /* newest safe spot at least 1.5 s older than the moment ground was last left */
        const ULONGLONG ref = ground ? now : left_ground;
        int pick = -1;
        for (unsigned k = 0; k < 32 && k < s_safe_n; k++) {
            const unsigned i = (s_safe_n - 1 - k) % 32;
            if (s_safe[i].t + 1500 <= ref) { pick = (int)i; break; }
        }
        if (pick < 0) pick = (int)((s_safe_n - 1) % 32);
        const SafePos sp = s_safe[pick];
        const float ex = ml_rdf(player + 0x340) - fx, ez = ml_rdf(player + 0x344) - fz, ey = ml_rdf(player + 0x348) - fy;
        const float up = 0.3f;
        ml_wrf(player + 0x3E80, sp.x); ml_wrf(player + 0x3E84, sp.z + up); ml_wrf(player + 0x3E88, sp.y);
        ml_wrf(player + 0x340, sp.x + ex); ml_wrf(player + 0x344, sp.z + up + ez); ml_wrf(player + 0x348, sp.y + ey);
        for (uint32_t o : { 0xF0u, 0xF4u, 0xF8u, 0x110u, 0x114u, 0x118u }) ml_wrf(player + o, 0.0f);
        fprintf(stderr, "[fall] F4: back to %.2f %.2f %.2f (stood there %llu ms ago)\n",
                (double)sp.x, (double)sp.z, (double)sp.y, (unsigned long long)(now - sp.t));
    }
    f4was = f4;
}
static volatile LONG s_ptrace_armed;   /* v169c: Shift+F10 seen by the key thread */
static DWORD WINAPI ptrace_key_thread(LPVOID) {
    int kw = 0;
    for (;;) { const int k = (GetAsyncKeyState(VK_F10) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
               if (k && !kw) InterlockedExchange(&s_ptrace_armed, 1); kw = k; Sleep(20); }
}
static int s_ptrace_key_init = (CloseHandle(CreateThread(NULL, 0, ptrace_key_thread, NULL, 0, NULL)), 0);
static ULONGLONG s_ml_script_until;   /* v169: scripted camera active until (ms) */
extern "C" void port_mouselook_pre(uint32_t player)
{
    fall_watch(player);   /* v168 */
    { static int en = -1; if (en < 0) { const char* e = getenv("PORT_SCRIPT30"); en = e ? atoi(e) : 1; }
      const int sc = en && player && vm_read8(player + 0x3F07) == 1;
      if (sc != g_port_scripted) { g_port_scripted = sc; static int n = 0;
          if (n++ < 40) fprintf(stderr, "[script] player sequence %s%s\n", sc ? "started: 30 fps pacing" : "ended: full frame rate", ""); } }
    g_port_space_start = 0;   /* v160v: in play */
    g_port_play_ms = GetTickCount64();   /* v162: in-game menu mode is off while this is fresh */
    g_port_pause_ui = 0;
    /* v167 diag: Shift+F10 records the player object (first 16 KB, every frame,
     * 30 s) to logs\\player_trace.bin, to find where position / velocity live.
     * Record: u64 ms, u32 player ea, u32 0, then 16384 bytes big-endian as in memory. */
    { static int was = 0; static FILE* pf = NULL; static ULONGLONG t0 = 0; static unsigned nfr = 0;
      /* v169c: also a press made while this hook was not running (loading screen) */
      const int dn = InterlockedExchange(&s_ptrace_armed, 0) != 0;
      if (dn && !was && !pf && player) {
          pf = fopen("logs\\player_trace.bin", "wb"); t0 = GetTickCount64(); nfr = 0;
          fprintf(stderr, "[ptrace] recording the player object (0x%08X) for 30 s\n", player);
      }
      was = dn;
      if (pf) {
          const ULONGLONG t = GetTickCount64();
          if (!player || t - t0 > 30000) {
              fclose(pf); pf = NULL;
              fprintf(stderr, "[ptrace] done: %u frames -> logs\\player_trace.bin\n", nfr);
          } else {
              /* v169b: + the object at [player+16016] (16 KB more), every 2nd frame */
              static uint8_t buf[32768]; static unsigned odd = 0;
              if (!(++odd & 1)) goto ptrace_skip;
              { const uint32_t c = vm_read32(player + 16016);
                for (uint32_t i = 0; i < 32768; i += 4) {
                  const uint32_t w = i < 16384 ? vm_read32(player + i) : (c ? vm_read32(c + i - 16384) : 0u);
                  buf[i] = (uint8_t)(w >> 24); buf[i+1] = (uint8_t)(w >> 16); buf[i+2] = (uint8_t)(w >> 8); buf[i+3] = (uint8_t)w; }
                const uint64_t ms = t - t0; const uint32_t hdr[2] = { player, c };
                fwrite(&ms, 8, 1, pf); fwrite(hdr, 4, 2, pf); fwrite(buf, 1, sizeof buf, pf); }
              nfr++;
              ptrace_skip:;
          }
      } }
    /* v162f: gore level. Options word [[[r2-0x77D0]-0x8000]+0x934+0x354]: 2 = full gore (blood),
       1 = reduced (set at boot from the console's license area, sysparam 0x0122). Log it, and
       keep it at 2 (PORT_GORE=0 leaves it alone). */
    { static int force = -100; if (force == -100) { const char* e = getenv("PORT_GORE"); force = e ? atoi(e) : 1; }
      static ULONGLONG lg = 0; const ULONGLONG t = GetTickCount64();
      const uint32_t tb = vm_read32(0x006BD648u - 0x77D0u); const uint32_t base = tb ? vm_read32(tb - 0x8000u) : 0;
      if (base) {
          const uint32_t ea = base + 0x934u + 0x354u; const uint32_t g = vm_read32(ea);
          /* v163d: log on change (whole session) instead of the first 5 minutes */
          const uint32_t td0 = vm_read32(0x006BD648u - 0x7FA0u);
          /* v163f: the real difficulty, as the "Difficulty level" strat node reads it (func_00242B48):
             word [[[r2-0x78E4]-0x78F0]] (0 easy .. 3 superhuman presumably) */
          const uint32_t tdf = vm_read32(0x006BD648u - 0x78E4u);
          const uint32_t pdf = tdf ? vm_read32(tdf - 0x78F0u) : 0;
          const uint32_t diff = pdf ? vm_read32(pdf) : 0xFFFFFFFFu;
          const uint32_t key = g | ((diff & 0xFFu) << 8)
                             | ((td0 ? (vm_read32(vm_read32(td0 - 0x7FF8u) + 0xCu) & 0xFFu) : 0xFFu) << 16);
          static uint32_t lastkey = 0xFFFFFFFFu;
          if (key != lastkey) { lastkey = key; lg = t; static int n = 0;
              if (r2_diag() && n++ < 200) { const uint32_t ta = vm_read32(0x006BD648u - 0x7DC0u); const uint32_t area = ta ? vm_read32(vm_read32(ta - 0x7FF0u)) : 0xFFFFFFFFu;
                  FILE* f = fopen("logs\\gore.txt", n == 1 ? "w" : "a");
                  /* v163c: difficulty as func_0001C7E0 reads it: [[r2-0x7FA0]-0x7FF8]+0xC (word, 0 = none),
                     byte [[r2-0x7FA0]-0x7FF4]+0xC (raw level; the func returns 2 for raw >= 2) */
                  const uint32_t td = vm_read32(0x006BD648u - 0x7FA0u);
                  const uint32_t dw = td ? vm_read32(vm_read32(td - 0x7FF8u) + 0xCu) : 0xFFFFFFFFu;
                  const uint32_t db = td ? vm_read8(vm_read32(td - 0x7FF4u) + 0xCu) : 0xFFu;
                  if (f) { fprintf(f, "t=%llu gore@0x%08X=%u area=%d%s word=%u mode=%u DIFFICULTY=%d (@0x%08X)\n", t, ea, g, (int)area,
                                   (force > 0 && g == 1) ? " -> 2" : (force < 0 && g == 2) ? " -> 1" : "", dw, db, (int)diff, pdf); fclose(f); } } }
          if (force > 0 && g == 1) vm_write32(ea, 2);
          else if (force < 0 && g == 2) vm_write32(ea, 1);   /* v163e: PORT_GORE=-1 = reduced gore (A/B test) */
      } }
    /* v164: dynamic difficulty. func_000108C0 eases the damage scalar d (0x10040654) the longer you
       spend in a section and the more you die: penalty (0x10179DB0, only ever grows, capped at 3) is
       subtracted from the difficulty's base d, floored at its minimum (Hard: base 8.5, min 7.0, max 11).
       The timers persist through Continue, so a long-tested section stays at the floor. From d the
       game derives 0x10040644 = 1.5^x, 0x10040648/0x1004064C = 0.667^x, 0x10040650 = 2^x, x = (d-5)/5.
       Default keeps the original behaviour; PORT_DYNDIFF=0 pins d to the selected difficulty's base. */
    { static int dyn = -1; if (dyn < 0) { const char* e = getenv("PORT_DYNDIFF"); dyn = (e && atoi(e) == 0) ? 0 : 1; }   /* v164b: original behaviour by default; PORT_DYNDIFF=0 pins d */
      const uint32_t diff = vm_read32(0x1033CB00u);
      if (!dyn && diff >= 1 && diff <= 4) {
          const float base = ml_rdf(0x1004052Cu + 56u * diff + 0xCu);   /* per-difficulty table, entry +0xC */
          const float d = ml_rdf(0x10040654u);
          if (base > 0.0f && base < 20.0f && (d != base || ml_rdf(0x10179DB0u) != 0.0f)) {
              const float x0 = (d - 5.0f) / 5.0f, x = (base - 5.0f) / 5.0f;
              const float m644 = ml_rdf(0x10040644u);
              ml_wrf(0x10179DB0u, 0.0f);
              ml_wrf(0x10040654u, base);
              ml_wrf(0x10040650u, powf(2.0f, x));
              ml_wrf(0x1004064Cu, powf(0.667f, x));
              ml_wrf(0x10040648u, powf(0.667f, x));
              ml_wrf(0x10040644u, m644 * powf(1.5f, x - x0));   /* keeps the co-op factor, if any */
              static int n = 0;
              if (r2_diag() && n++ < 50) { FILE* f = fopen("logs\\dyndiff.txt", n == 1 ? "w" : "a");
                  if (f) { fprintf(f, "t=%llu diff=%u d %.3f -> %.3f\n", GetTickCount64(), diff, d, base); fclose(f); } }
          } } }
    if (!g_ml_mode || !player) return;
    ml_load();
    if (!ml_stick_is_mouse()) return;   /* controller / look keys: the stick turns the view as on PS3 */
    ml_check(g_ml_fed_rx, ml_rdf(player + 980), &s_ml_sx, &s_ml_bad_x, "yaw");
    ml_check(g_ml_fed_ry, ml_rdf(player + 976), &s_ml_sy, &s_ml_bad_y, "pitch");
    /* v169: scripted sequences (a chapter's opening walk) turn the camera by
     * writing these look rates themselves while the player's input is locked.
     * Zeroing them froze the scripted camera (and the script, waiting for its
     * turn to finish, cut the walk short); the mouse, applied straight to the
     * view, fought it. A look rate with no stick deflection fed = the game is
     * turning the view: leave it, and drop mouse input until it stops. */
    { const float rp = ml_rdf(player + 976), ry = ml_rdf(player + 980);
      const int fed = (g_ml_fed_rx >= 16 || g_ml_fed_rx <= -16 || g_ml_fed_ry >= 16 || g_ml_fed_ry <= -16);
      if (!fed && (fabsf(rp) > 0.02f || fabsf(ry) > 0.02f)) {
          if (GetTickCount64() > s_ml_script_until) { static int n = 0; if (n++ < 20) fprintf(stderr, "[mouselook] scripted camera: mouse paused%c", 10); }
          s_ml_script_until = GetTickCount64() + 400;
          return;
      } }
    if (GetTickCount64() < s_ml_script_until || vm_read8(player + 0x3F07) == 1) return;   /* v176: also the lock flag */
    ml_wrf(player + 976, 0.0f);
    ml_wrf(player + 980, 0.0f);
}
extern "C" void port_mouselook(uint32_t player)
{
    if (!g_ml_mode || !player) return;
    ml_load();
    { const uint32_t c = vm_read32(player + 16016);
      g_port_opts_ea = c ? vm_read32(c + 6848) : 0; }
    const ULONGLONG now = GetTickCount64();
    long dx = InterlockedExchange(&g_kbm_ml_dx, 0), dy = InterlockedExchange(&g_kbm_ml_dy, 0);
    /* counts gathered while the look code was not running (menus, pause,
     * cutscenes, a vehicle) are dropped, not applied in one jump */
    if (now - s_ml_last > 150 || !g_kbm_capture || !ml_stick_is_mouse()) dx = dy = 0;
    if (now < s_ml_script_until) dx = dy = 0;   /* v169: scripted camera */
    if (g_port_scripted || vm_read8(player + 0x3F07) == 1) dx = dy = 0;   /* v176: input-locked sequence (player +0x3F07) */
    s_ml_last = now;
    /* v150: the game's controller sensitivity (options 0-9, stored as
     * setting/9 at [[player+16016]+6848]+68) scales the mouse the way it
     * scales the stick's turn speed (func_0029ADF8: speed * (0.5 + s), i.e.
     * 0.5x at 0, 1x at the middle, 1.5x at 9). */
    double gs = 1.0;
    { const uint32_t c = vm_read32(player + 16016);
      const uint32_t o = c ? vm_read32(c + 6848) : 0;
      if (o) { float sv = ml_rdf(o + 68); if (sv == sv) { if (sv < 0) sv = 0; if (sv > 1) sv = 1; gs = 0.5 + sv; } } }
    /* v166: aiming down sights / scopes narrow the FOV; scale the mouse by the
     * same magnification (ratio of tan(fov/2), as displayed) so a mouse move
     * covers the same part of the screen zoomed or not. PORT_ZOOM_SENS=0 off,
     * or a 0..2 strength (1 = matched). */
    { static double zs = -1.0; if (zs < 0.0) { const char* e = getenv("PORT_ZOOM_SENS"); zs = e ? atof(e) : 1.0;
                                               if (zs < 0.0) zs = 0.0; if (zs > 2.0) zs = 2.0; }
      const double f = g_port_game_fov, d2r = 3.14159265358979323846 / 180.0;
      if (zs > 0.0 && f < 81.5 && f > 1.0) {
          const double fa = port_fov_adjust(f), fb = port_fov_adjust(82.0);
          const double r = tan(fa * 0.5 * d2r) / tan(fb * 0.5 * d2r);
          if (r > 0.0 && r < 1.0) gs *= pow(r, zs);
      } }
    const double k = g_ml_deg_per_count * gs * (3.14159265358979323846 / 180.0);
    if (dx) {
        double y = (double)ml_rdf(player + 868) + (double)s_ml_sx * (double)dx * k;
        while (y > 3.14159265358979323846) y -= 6.28318530717958647692;
        while (y < -3.14159265358979323846) y += 6.28318530717958647692;
        ml_wrf(player + 868, (float)y);
    }
    if (g_ml_invert_y) dy = -dy;
    if (dy) ml_wrf(player + 864, (float)((double)ml_rdf(player + 864) + (double)s_ml_sy * (double)dy * k));
}
/* Resistance 2: direct mouse look (mouse_look = direct in controls.txt).
 * The hero look code func_00107B04 turns the look stick (slot+0x18/0x1C of
 * the controller history) into turn rates with func_000F5F10, written to its
 * stack at +0x90 (yaw) / +0x94 (pitch); zoom scales them by f29 / f28 when
 * hero+0xAEC != 0, then scripted / aim inputs hero+0x55C / +0x560 are added
 * and the sums stored at hero+0x4B4 (yaw rate) / +0x4B0 (pitch rate). At
 * 0x00108CC4 they become this frame's pitch / yaw deltas (f29 / f1 after
 * func_006C00D0 at 0x00108D0C), added to the pitch (clamped to
 * hero+0x484..0x488) and turned into the new view matrix at hero+0x350.
 *  r2_mouselook_pre (0x00108280, after F5F10): learns the turn directions
 *    from the stick the mouse emulation fed (so the game's invert setting is
 *    followed), then zeroes the stick's rates -- the scripted / aim inputs
 *    added afterwards stay.
 *  r2_mouselook (0x00108D0C): adds the mouse counts since the last frame to
 *    the yaw / pitch deltas; the game's own clamp and wrap follow.
 * Vehicles, turrets and menus don't run this code: there the mouse still
 * drives the emulated right stick (cellPad.c). */
extern "C" volatile unsigned long long g_r2_look_ms;
volatile unsigned long long g_r2_look_ms = 0;
static int s_r2ml_sx = 0, s_r2ml_sy = 0, s_r2ml_bad_x = 0, s_r2ml_bad_y = 0, s_r2ml_saved = 0;
static uint32_t s_r2ml_hero = 0;      /* set by the pre hook for this frame's call */
static float s_r2ml_zoom = 1.0f;
static ULONGLONG s_r2ml_last = 0, s_r2ml_lock_until = 0;
static int s_r2ml_lock_n = 0;
static void r2ml_save(void)
{
    FILE* f = fopen("mouselook_r2.cfg", "w");
    if (f) { fprintf(f, "yaw_sign = %d\npitch_sign = %d\n", s_r2ml_sx, s_r2ml_sy); fclose(f); }
    s_r2ml_saved = 1;
}
static void r2ml_load(void)
{
    if (s_r2ml_sx) return;
    s_r2ml_sx = -1; s_r2ml_sy = -1;   /* stick right / down = negative rates (the game negates the stick) */
    FILE* f = fopen("mouselook_r2.cfg", "r");
    if (f) { char l[128]; int v;
        while (fgets(l, sizeof l, f)) {
            if (sscanf(l, " yaw_sign = %d", &v) == 1 && (v == 1 || v == -1)) s_r2ml_sx = v;
            if (sscanf(l, " pitch_sign = %d", &v) == 1 && (v == 1 || v == -1)) s_r2ml_sy = v;
        }
        fclose(f); s_r2ml_saved = 1; }
    fprintf(stderr, "[mouselook] R2 direct mouse look: yaw %d, pitch %d%c", s_r2ml_sx, s_r2ml_sy, 10);
}
static void r2ml_check(int fed, float rate, int* sign, int* bad, const char* what)
{
    if (!(fed >= 16 || fed <= -16) || !(rate > 1e-5f || rate < -1e-5f)) return;
    const int s = ((fed > 0) == (rate > 0)) ? 1 : -1;
    if (s == *sign) { *bad = 0; return; }
    if (++*bad >= (s_r2ml_saved ? 15 : 4)) {
        *sign = s; *bad = 0; r2ml_save();
        fprintf(stderr, "[mouselook] %s direction now %d (following the game's setting)%c", what, s, 10);
    }
}
/* R2 frame rate.  func_00635D28(f1 = period in seconds, scaled by the float at
 * +0x238) is the engine's frame limiter: it sleeps until the deadline at +0x208,
 * then advances it.  Game logic uses the measured frame time, which is what
 * RPCS3's "Unlock FPS" patch (Whatcookie) relies on: it removes that wait and
 * sets the 1/30 (0x00D529D8) and 1/75 (0x00D69EB8) frame-time floors to 0.0001.
 * R2_FPS != 30 does the same and paces frames here instead, precisely, at
 * 1/R2_FPS. */
extern "C" void ppu_gcm_pump(void);
extern "C" double r2_frame_period(double f1)
{
    static int fps = -1;
    if (fps < 0) {
        const char* e = getenv("R2_FPS");
        fps = e ? atoi(e) : 30;
        if (fps < 30) fps = 30;
        if (fps > 240) fps = 240;
        if (fps != 30) {
            const float c = 0.0001f; uint32_t w; memcpy(&w, &c, 4);
            if (vm_read32(0x00D529D8u) == 0x3D088889u) vm_write32(0x00D529D8u, w);
            { const uint32_t o = vm_read32(0x00D69EB8u); float f; memcpy(&f, &o, 4);
              if (f > 0.013f && f < 0.0135f) vm_write32(0x00D69EB8u, w); }
        }
        fprintf(stderr, "[fps] target %d (floors %08x %08x)\n", fps, vm_read32(0x00D529D8u), vm_read32(0x00D69EB8u));
    }
    if (r2_diag()) {
        static double seen[8]; static int ns;
        int k = 0; while (k < ns && seen[k] != f1) k++;
        if (k == ns && ns < 8) { seen[ns++] = f1; fprintf(stderr, "[fps] limiter period %.5f\n", f1); }
    }
    if (fps == 30) return f1;

    /* host pacing: wait until 1/fps after the previous frame */
    static uint64_t next = 0;
    const uint64_t step = 79800000ull / (uint64_t)fps;
    uint64_t now = ppu_timebase_now();
    if (next && now < next) {
        for (;;) {
            now = ppu_timebase_now();
            if (now >= next) break;
            const double rem_us = (double)(next - now) / 79.8;
            ppu_gcm_pump();
            if (rem_us > 2600.0) Sleep((DWORD)(rem_us / 1000.0) - 1u);
            else if (rem_us > 150.0) SwitchToThread();
            else YieldProcessor();
        }
        next += step;
    } else next = now + step;   /* first frame, or running behind: no catch-up burst */

    { static uint64_t t0 = 0; static unsigned n = 0;
      if (!t0) t0 = now;
      n++;
      if (now - t0 >= 5ull * 79800000ull) {
          fprintf(stderr, "[fps] 5 s: %.1f fps (target %d)\n", n * 79800000.0 / (double)(now - t0), fps);
          t0 = now; n = 0;
      } }
    return 0.0001;   /* the game's own wait then never holds the frame */
}

/* the hero's controller is controller 0 (pad port 0 = keyboard & mouse)? */
static int r2ml_is_pad0(uint32_t slot)
{
    const uint32_t t = vm_read32(0x00DED008u - 0x7E98u); if (!t) return 1;
    const uint32_t p = vm_read32(t - 0x8000u);            if (!p) return 1;
    const uint32_t base = vm_read32(p);                   if (!base) return 1;
    if (slot < base || slot >= base + 8u * 0x1AC4u) return 1;   /* unknown: don't block */
    return slot - base < 0x1AC4u;
}
extern "C" void r2_mouselook_pre(ppu_context* ctx, uint32_t hero, uint32_t slot, uint32_t sp)
{
    s_r2ml_hero = 0;
    if (!g_ml_mode || !hero || !r2ml_is_pad0(slot)) return;
    g_r2_look_ms = GetTickCount64();
    r2ml_load();
    if (!ml_stick_is_mouse()) return;   /* controller / look keys: the stick turns the view as on PS3 */
    const float ry = ml_rdf(sp + 0x90), rp = ml_rdf(sp + 0x94);
    r2ml_check(g_ml_fed_rx, ry, &s_r2ml_sx, &s_r2ml_bad_x, "yaw");
    r2ml_check(g_ml_fed_ry, rp, &s_r2ml_sy, &s_r2ml_bad_y, "pitch");
    /* input locked (scripted sequence, func_001A3548): the game zeroes the
     * stick, so a clearly deflected fed stick gives no rate. No mouse then. */
    { const int big = g_ml_fed_rx >= 40 || g_ml_fed_rx <= -40 || g_ml_fed_ry >= 40 || g_ml_fed_ry <= -40;
      if (big && ry == 0.0f && rp == 0.0f) { if (++s_r2ml_lock_n >= 3) s_r2ml_lock_until = GetTickCount64() + 300; }
      else if (big) s_r2ml_lock_n = 0; }
    s_r2ml_zoom = 1.0f;
    if (vm_read16(hero + 0xAEC)) {   /* zoomed: the game scales the stick's yaw rate by f29 */
        const double z = ctx->fpr[29];
        static double zs = -1.0; if (zs < 0.0) { const char* e = getenv("R2_ZOOM_SENS"); zs = e ? atof(e) : 1.0;
                                                 if (zs < 0.0) zs = 0.0; if (zs > 2.0) zs = 2.0; }
        if (z > 0.01 && z < 1.0) s_r2ml_zoom = (float)pow(z, zs);
    }
    ml_wrf(sp + 0x90, 0.0f);
    ml_wrf(sp + 0x94, 0.0f);
    s_r2ml_hero = hero;
}
extern "C" void r2_mouselook(ppu_context* ctx, uint32_t hero)
{
    if (!g_ml_mode || !hero) return;
    const ULONGLONG now = GetTickCount64();
    const int mine = hero == s_r2ml_hero; s_r2ml_hero = 0;
    if (!mine) return;
    long dx = InterlockedExchange(&g_kbm_ml_dx, 0), dy = InterlockedExchange(&g_kbm_ml_dy, 0);
    /* counts gathered while this code was not running (menus, pause,
     * cutscenes, a vehicle) are dropped, not applied in one jump */
    if (now - s_r2ml_last > 150 || !g_kbm_capture || now < s_r2ml_lock_until) dx = dy = 0;
    s_r2ml_last = now;
    if (!dx && !dy) return;
    const double k = g_ml_deg_per_count * (double)s_r2ml_zoom * (3.14159265358979323846 / 180.0);
    if (g_ml_invert_y) dy = -dy;
    ctx->fpr[1]  = (double)(float)(ctx->fpr[1]  + (double)s_r2ml_sx * (double)dx * k);   /* yaw delta */
    ctx->fpr[29] = (double)(float)(ctx->fpr[29] + (double)s_r2ml_sy * (double)dy * k);   /* pitch delta */
    if (r2_diag()) { static int n = 0;
        if (n++ < 30) fprintf(stderr, "[mouselook] hero 0x%08X dx %ld dy %ld -> yaw %+.4f pitch %+.4f (pitch %.3f, limits %.3f..%.3f, zoom %.2f)%c",
                                      hero, dx, dy, ctx->fpr[1], ctx->fpr[29], ctx->fpr[22], (double)ml_rdf(hero + 0x484),
                              (double)ml_rdf(hero + 0x488), (double)s_r2ml_zoom, 10); }
}
/* Resistance port (v152): on-screen button prompts for keyboard & mouse.
 * The game's text ("Press <glyph> to jump") marks buttons with private-use
 * characters U+0A09..U+0A26, drawn as PS3 button icons by the font. When a
 * text file is registered (game func_000B98F0 / front end func_000C7180, the
 * loader of IGHW section 0x2600) and no controller is plugged in, each glyph
 * is replaced in memory by a 3-character key name from controls.txt ("[E]",
 * "LMB", "Spc"...). 3 characters because a glyph is 3 bytes of UTF-8 and the
 * string tables point into the text by offset.
 * Action glyphs (U+0A1B..) mean an ACTION (they follow the controller layout
 * on PS3); keys are bound to actions in the launcher, so the prompt shows the
 * key bound to that action's default button. */
#include <xinput.h>
#include <cctype>
#pragma comment(lib, "xinput.lib")
static const struct { uint16_t cp; const char* pad; const char* fixed; const char* full; } s_glyph_map[] = {
    { 0x0A0A, "L2", 0 }, { 0x0A0B, "R2", 0 },                     /* vehicle brake/reverse, accelerate */
    { 0x0A12, 0, "Mov", "WASD" }, { 0x0A16, 0, "Mse", "Mouse" },                   /* left stick, right stick */
    { 0x0A17, "DPAD_UP", 0 }, { 0x0A18, "DPAD_DOWN", 0 }, { 0x0A19, "DPAD_LEFT", 0 }, { 0x0A1A, "DPAD_RIGHT", 0 },
    { 0x0A1B, "TRIANGLE", 0 }, { 0x0A1C, "R1", 0 }, { 0x0A1D, "L1", 0 }, { 0x0A1E, "R2", 0 },
    { 0x0A1F, "CIRCLE", 0 }, { 0x0A20, "CROSS", 0 }, { 0x0A21, "R3", 0 }, { 0x0A22, "L2", 0 },
    { 0x0A23, "SQUARE", 0 }, { 0x0A24, "DPAD_RIGHT", 0 }, { 0x0A25, "DPAD_UP", 0 },
};
/* Short (3-char) name: fallback when a string can't be relocated. */
static void key3(const char* k, char out[4])
{
    static const struct { const char* n; const char* s; } t[] = {
        {"MOUSE1","LMB"},{"MOUSE2","RMB"},{"MOUSE3","MMB"},{"MOUSE4","MB4"},{"MOUSE5","MB5"},
        {"WHEELUP","WhU"},{"WHEELDOWN","WhD"},{"SPACE","Spc"},{"SHIFT","Sft"},{"LSHIFT","Sft"},{"RSHIFT","Sft"},
        {"CTRL","Ctl"},{"LCTRL","Ctl"},{"RCTRL","Ctl"},{"ALT","Alt"},{"TAB","Tab"},{"ENTER","Ent"},{"ESC","Esc"},
        {"BACKSPACE","Bks"},{"UP","Up "},{"DOWN","Dn "},{"LEFT","Lt "},{"RIGHT","Rt "},{"CAPSLOCK","Cap"},
        {"INSERT","Ins"},{"DELETE","Del"},{"HOME","Hom"},{"END","End"},{"PGUP","PgU"},{"PGDN","PgD"},
        {"GRAVE","[`]"},{"MINUS","[-]"},{"EQUALS","[=]"},{"LBRACKET","[[]"},{"RBRACKET","[]]"},{"BACKSLASH","[\\]"},
        {"SEMICOLON","[;]"},{"APOSTROPHE","[']"},{"COMMA","[,]"},{"PERIOD","[.]"},{"SLASH","[/]"},
    };
    for (auto& e : t) if (!_stricmp(k, e.n)) { memcpy(out, e.s, 4); return; }
    if (strlen(k) == 1) { out[0] = '['; out[1] = (char)toupper((unsigned char)k[0]); out[2] = ']'; out[3] = 0; return; }
    if ((k[0] == 'F' || k[0] == 'f') && strlen(k) <= 3) { snprintf(out, 4, "%-3s", k); return; }
    if (!_strnicmp(k, "NUM", 3) && strlen(k) == 4) { out[0] = 'N'; out[1] = 'm'; out[2] = k[3]; out[3] = 0; return; }
    snprintf(out, 4, "%.3s", k);
}
/* Full name (v153): "[Space]", "[Left Shift]"... Mouse buttons stay LMB/RMB. */
static void keyfull(const char* k, char out[24])
{
    static const struct { const char* n; const char* s; } t[] = {
        {"MOUSE1","LMB"},{"MOUSE2","RMB"},{"MOUSE3","MMB"},{"MOUSE4","Mouse 4"},{"MOUSE5","Mouse 5"},
        {"WHEELUP","Wheel Up"},{"WHEELDOWN","Wheel Down"},
        {"SPACE","[Space]"},{"SHIFT","[Shift]"},{"LSHIFT","[Shift]"},{"RSHIFT","[Right Shift]"},
        {"CTRL","[Ctrl]"},{"LCTRL","[Ctrl]"},{"RCTRL","[Right Ctrl]"},{"ALT","[Alt]"},{"TAB","[Tab]"},
        {"ENTER","[Enter]"},{"ESC","[Esc]"},{"BACKSPACE","[Backspace]"},{"UP","[Up]"},{"DOWN","[Down]"},
        {"LEFT","[Left]"},{"RIGHT","[Right]"},{"CAPSLOCK","[Caps Lock]"},{"INSERT","[Insert]"},
        {"DELETE","[Delete]"},{"HOME","[Home]"},{"END","[End]"},{"PGUP","[Page Up]"},{"PGDN","[Page Down]"},
        {"GRAVE","[`]"},{"MINUS","[-]"},{"EQUALS","[=]"},{"LBRACKET","[[]"},{"RBRACKET","[]]"},{"BACKSLASH","[\\]"},
        {"SEMICOLON","[;]"},{"APOSTROPHE","[']"},{"COMMA","[,]"},{"PERIOD","[.]"},{"SLASH","[/]"},
    };
    for (auto& e : t) if (!_stricmp(k, e.n)) { snprintf(out, 24, "%s", e.s); return; }
    if (strlen(k) == 1) { snprintf(out, 24, "[%c]", toupper((unsigned char)k[0])); return; }
    if (!_strnicmp(k, "NUM", 3) && strlen(k) == 4) { snprintf(out, 24, "[Num %c]", k[3]); return; }
    snprintf(out, 24, "[%.20s]", k);
    for (char* c = out + 2; *c; c++) *c = (char)tolower((unsigned char)*c);   /* "[F1]" stays, "[PAUSE]" -> "[Pause]" */
}
static int pad_key_for(const char* pad, char out3[4], char outf[24])
{
    FILE* f = fopen("controls.txt", "r");
    if (!f) return 0;
    char line[256]; int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        if (char* h = strchr(line, '#')) *h = 0;
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0;
        char k[64], v[64];
        if (sscanf(line, " %63s", k) != 1 || sscanf(eq + 1, " %63s", v) != 1) continue;
        if (!_stricmp(v, pad)) { key3(k, out3); keyfull(k, outf); found = 1; }
    }
    fclose(f);
    return found;
}
extern "C" uint32_t ps3_hle_heap_alloc(uint32_t size);
static char s_rep3[0x30][4], s_repf[0x30][24]; static int s_have[0x30];
/* v153: the action glyph (Triangle) is "use" in nearly every prompt; it shows
 * the INTERACT key, except in text about melee, which shows the melee key. */
static char s_melee3[4], s_meleef[24]; static int s_have_melee = 0;
static int glyph_at(const uint8_t* q)            /* index of a button glyph at q, or -1 */
{
    if (q[0] != 0xE0 || q[1] != 0xA8 || q[2] < 0x80 || q[2] > 0xBF) return -1;
    const int k = 0x0A00 + (q[2] - 0x80) - 0x0A09;
    return (k >= 0 && k < 0x30 && s_have[k]) ? k : -1;
}
static inline uint32_t be32p(const uint8_t* q) { return (uint32_t)((q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3]); }
/* v160v: while a level loads and shows "Press the START button", Space also
 * presses Start (cellPad.c); cleared as soon as the player's look code runs,
 * so in play Space is only jump and never pauses. */
extern "C" volatile int g_port_space_start;
extern "C" void port_text_patch(uint32_t file_obj)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("PORT_KEY_PROMPTS"); en = e ? atoi(e) : 1; }
    if (!en || !file_obj) return;
    int pad = 0;   /* controller: keep the PS3 icons, only reword the grapple line (v157) */
    { XINPUT_STATE xs; if (XInputGetState(0, &xs) == ERROR_SUCCESS) pad = 1; }
    const uint32_t base = vm_read32(file_obj + 4);
    if (!base || vm_read32(base) != 0x49474857u) return;        /* "IGHW" */
    const uint32_t n = vm_read32(base + 8);
    if (n == 0 || n > 64) return;
    uint32_t end = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t off = vm_read32(base + 0x10 + 16 * i + 4), sz = vm_read32(base + 0x10 + 16 * i + 8) & 0xFFFFFFu;
        if (off + sz > end) end = off + sz;
    }
    if (!end || end > (16u << 20)) return;
    static int init = 0;
    if (!init) { init = 1;
        for (auto& g : s_glyph_map) { const int i = g.cp - 0x0A00 - 0x09; if (i < 0 || i >= 0x30) continue;
            if (g.fixed) { memcpy(s_rep3[i], g.fixed, 4); snprintf(s_repf[i], 24, "%s", g.full); s_have[i] = 1; }
            else if (g.cp == 0x0A1B) {
                s_have_melee = pad_key_for("TRIANGLE", s_melee3, s_meleef);
                s_have[i] = pad_key_for("INTERACT", s_rep3[i], s_repf[i]);
                if (!s_have[i] && s_have_melee) { memcpy(s_rep3[i], s_melee3, 4); memcpy(s_repf[i], s_meleef, 24); s_have[i] = 1; }
                if (!s_have_melee) { memcpy(s_melee3, s_rep3[i], 4); memcpy(s_meleef, s_repf[i], 24); s_have_melee = s_have[i]; }
            }
            else s_have[i] = pad_key_for(g.pad, s_rep3[i], s_repf[i]); }
        /* v160: the front end is driven by the menu controls (cellPad.c):
         * left click selects, Esc goes back. */
        { char mp[MAX_PATH] = ""; GetModuleFileNameA(NULL, mp, MAX_PATH);
          const char* b = strrchr(mp, '\\'); b = b ? b + 1 : mp;
          if (!_stricmp(b, "PortFrontEnd.exe")) {
              const int cx = 0x0A20 - 0x0A09, ci = 0x0A1F - 0x0A09;
              memcpy(s_rep3[cx], "Spc", 4); snprintf(s_repf[cx], 24, "[Space]"); s_have[cx] = 1;
              memcpy(s_rep3[ci], "Esc", 4); snprintf(s_repf[ci], 24, "[Esc]"); s_have[ci] = 1;
          } } }
    extern uint8_t* vm_base;
    uint8_t* p = vm_base + base;
    int moved = 0;
    /* 1. Full names (v153b): copy each string that has a button glyph, with the
     *    glyphs spelled out, into new guest memory, and repoint whatever refers
     *    to it. The reference is found by value: a 32-bit word equal to the
     *    string's offset (from the file or from one of its sections) that sits
     *    among other words pointing at string starts (a string table). The game
     *    adds it to the base in 32 bits, so a wrapped offset still lands on the copy. */
    static int full = -1;
    if (full < 0) { const char* e = getenv("PORT_KEY_FULLNAMES"); full = e ? atoi(e) : 1; }
    uint32_t nrefs = 0;
    std::vector<uint32_t> starts;                           /* strings to rewrite */
    std::vector<const char*> isgrab;   /* replacement text, or 0 (v160n) */
    for (uint32_t i = 0; !pad && i + 2 < end; i++) {        /* strings with a glyph */
        if (glyph_at(p + i) < 0) continue;
        uint32_t st = i; while (st > 0 && p[st - 1] && i - st < 4096) st--;
        if (st == 0 || p[st - 1]) { i += 2; continue; }
        starts.push_back(st); isgrab.push_back(nullptr);
        while (i < end && p[i]) i++;
    }
    /* v154: the "shake both sticks to break free" text -> a mouse version */
    const char* const kGrabText = pad ? "Rapidly move both sticks to break a Chimeran grapple."
                                      : "Shake the mouse to break a Chimeran grapple.";
    static int grab_en = -1;
    if (grab_en < 0) { const char* e = getenv("PORT_GRAB_PROMPT"); grab_en = e ? atoi(e) : 1; }
    /* v160n: front end: the button pictures next to BACK / SELECT are hidden
     * (rsx_d3d12_backend.c) and the words carry the key instead. */
    static int fe_menu = -1;
    if (fe_menu < 0) { char mp[MAX_PATH] = ""; GetModuleFileNameA(NULL, mp, MAX_PATH);
        const char* b = strrchr(mp, '\\'); b = b ? b + 1 : mp; const char* e = getenv("RSX_FE_KEYCAPS");
        fe_menu = (!_stricmp(b, "PortFrontEnd.exe") && e && atoi(e) == 3) ? 1 : 0; }   /* v160p: off -- menus use Anark scene strings */
    /* v160v: "Press the START button" (level intro card): on keyboard it's Space */
    for (uint32_t i = 1; !pad && i < end; i++) {
        if (p[i - 1] != 0 || p[i] == 0) continue;
        uint32_t m = 0; while (i + m < end && p[i + m] && m < 64) m++;
        const char* r = nullptr;
        if (m == 22 && !memcmp(p + i, "Press the START button", 22)) r = "Press the SPACE bar";
        else if (m == 34 && !memcmp(p + i, "Press the START button to continue", 34)) r = "Press the SPACE bar to continue";
        else if (m == 35 && !memcmp(p + i, " Press the START button to continue", 35)) r = " Press the SPACE bar to continue";
        if (r) { size_t si = 0; while (si < starts.size() && starts[si] != i) si++;
                 if (si == starts.size()) { starts.push_back(i); isgrab.push_back(r); } else isgrab[si] = r; }
        i += m;
    }
    { char mp[MAX_PATH] = ""; GetModuleFileNameA(NULL, mp, MAX_PATH);   /* a level is loading: Space = Start until the player moves */
      const char* b = strrchr(mp, '\\'); b = b ? b + 1 : mp;
      /* v162b: not when the pause menu loads its text (play was running a moment ago) -- Space there is Cross */
      const ULONGLONG t = GetTickCount64();
      if (_stricmp(b, "PortFrontEnd.exe") && (!g_port_play_ms || t - g_port_play_ms > 3000)) g_port_space_start = 1;
      else if (_stricmp(b, "PortFrontEnd.exe")) g_port_pause_ui = 1; }   /* v162c: text loaded mid-play = the pause menu */
    for (uint32_t i = 1; fe_menu && !pad && i < end; i++) {
        if (p[i - 1] != 0 || p[i] == 0) continue;
        uint32_t m = 0; while (i + m < end && p[i + m] && m < 64) m++;
        const char* r = nullptr;
        if (m == 4 && !memcmp(p + i, "BACK", 4)) r = "ESC  BACK";
        else if (m == 6 && !memcmp(p + i, "SELECT", 6)) r = "SPACE  SELECT";
        if (r) { starts.push_back(i); isgrab.push_back(r); }
        i += m;
    }
    for (uint32_t i = 1; grab_en && i < end; i++) {
        if (p[i - 1] != 0 || p[i] == 0) continue;
        uint32_t m = 0; while (i + m < end && p[i + m] && m < 1024) m++;
        if (m >= 12 && m < 1024) {
            std::string low; low.reserve(m); int lglyph = 0, rglyph = 0;
            for (uint32_t k = 0; k < m; k++) {
                const int g = (k + 2 < m) ? glyph_at(p + i + k) : -1;
                if (g == 0x0A12 - 0x0A09) lglyph = 1;
                if (g == 0x0A16 - 0x0A09) rglyph = 1;
                low += (char)tolower(p[i + k]);
            }
            (void)rglyph;
            const bool sticks = lglyph && low.find("grapple") != std::string::npos;
            const bool escape = true || low.find("free") != std::string::npos || low.find("grasp") != std::string::npos ||
                                low.find("escape") != std::string::npos || low.find("struggl") != std::string::npos ||
                                low.find("loose") != std::string::npos || low.find("grip") != std::string::npos ||
                                low.find("shake") != std::string::npos;
            if (sticks && escape) {
                size_t si = 0; while (si < starts.size() && starts[si] != i) si++;
                if (si == starts.size()) { starts.push_back(i); isgrab.push_back(kGrabText); } else isgrab[si] = kGrabText;
            }
        }
        i += m;
    }
    { static int logged_files = 0;                          /* what was found, for checking */
      if (!starts.empty() && logged_files++ < 12) {
        char pfn[MAX_PATH + 32]; { char mp[MAX_PATH] = ""; GetModuleFileNameA(NULL, mp, MAX_PATH);
          const char* b = strrchr(mp, '\\'); b = b ? b + 1 : mp; snprintf(pfn, sizeof pfn, "logs\\prompts_%s.txt", b); }
        if (FILE* f = fopen(pfn, logged_files == 1 ? "w" : "a")) {
            fprintf(f, "== text file 0x%08X (%u bytes)\n", base, end);
            for (size_t si = 0; si < starts.size(); si++) {
                fprintf(f, "%s @0x%X: ", isgrab[si] ? "REPL  " : "prompt", starts[si]);
                for (uint32_t k = starts[si]; k < end && p[k]; k++) {
                    const int g = (k + 2 < end) ? glyph_at(p + k) : -1;
                    if (g >= 0) { fprintf(f, "{%04X}", 0x0A09 + g); k += 2; }
                    else fputc(p[k] >= 0x20 && p[k] < 0x7F ? p[k] : '?', f);
                }
                fputc('\n', f);
            }
            fclose(f); } } }
    std::vector<char> relocated(starts.size(), 0);
    if (full) {
        std::vector<uint32_t> rels(1, 0);
        rels.push_back(0u - base);       /* v155: the loader has already made the table entries absolute addresses */
        for (uint32_t i = 0; i < n; i++) { const uint32_t so = be32p(p + 0x10 + 16 * i + 4); if (so && so < end) rels.push_back(so); }
        auto plausible = [&](uint32_t v, uint32_t rel) -> bool {
            const uint32_t x = rel + v; return x > 0 && x < end && p[x - 1] == 0 && p[x] != 0; };
        std::unordered_multimap<uint32_t, uint32_t> words;  /* value -> position */
        words.reserve(end / 4);
        for (uint32_t i = 0x10; i + 4 <= end; i += 4) words.emplace(be32p(p + i), i);
        struct Ref { uint32_t pos, rel, si; };
        std::vector<Ref> refs;
        for (uint32_t si = 0; si < starts.size(); si++) {
            const uint32_t st = starts[si];
            for (uint32_t rel : rels) {
                if (rel > st && rel != 0u - base) continue;
                auto rg = words.equal_range(st - rel);
                for (auto it = rg.first; it != rg.second; ++it) {
                    const uint32_t i = it->second; int ok = 0;   /* at least two table neighbours */
                    for (uint32_t d = 4; d <= 16; d += 4)
                        ok += (i >= 0x10 + d && plausible(be32p(p + i - d), rel)) + (i + d + 4 <= end && plausible(be32p(p + i + d), rel));
                    if (ok >= 2) refs.push_back({ i, rel, si });
                }
            }
        }
        if (!refs.empty()) {
            std::string out; std::vector<uint32_t> at(starts.size(), 0xFFFFFFFFu);
            for (auto& r : refs) {
                if (at[r.si] != 0xFFFFFFFFu) continue;
                const uint32_t o = starts[r.si];
                size_t m = 0; for (; o + m < end && p[o + m]; m++) {}
                int melee = 0;
                for (size_t i = 0; i + 5 <= m && !melee; i++) melee = !_strnicmp((const char*)p + o + i, "melee", 5);
                at[r.si] = (uint32_t)out.size();
                if (isgrab[r.si]) { out += isgrab[r.si]; out += '\0'; continue; }
                for (size_t i = 0; i < m; i++) {
                    const int k = (i + 2 < m) ? glyph_at(p + o + i) : -1;
                    if (k == 0x0A1B - 0x0A09 && melee) { out += s_meleef; i += 2; }
                    else if (k >= 0) { out += s_repf[k]; i += 2; } else out += (char)p[o + i];
                }
                out += '\0';
            }
            const uint32_t blk = ps3_hle_heap_alloc((uint32_t)out.size() + 16);
            if (blk) {
                memcpy(vm_base + blk, out.data(), out.size());
                for (auto& r : refs) vm_write32(base + r.pos, blk + at[r.si] - base - r.rel);
                nrefs = (uint32_t)refs.size();
                for (uint32_t si = 0; si < starts.size(); si++) if (at[si] != 0xFFFFFFFFu) { moved++; relocated[si] = 1; }
            }
        } else if (!starts.empty()) {                       /* layout not understood: keep a copy to look at */
            static int dumps = 0;
            if (dumps < 4) { char fn[64]; snprintf(fn, sizeof fn, "logs/textfile_%d.bin", dumps++);
                if (FILE* f = fopen(fn, "wb")) { fwrite(p, 1, end, f); fclose(f); } }
        }
    }
    /* 2. Anything left (no table found, or no memory): the grab text in place
     *    when it fits, and 3-character names for the glyphs. */
    for (size_t si = 0; si < starts.size(); si++) {
        if (!isgrab[si] || relocated[si]) continue;
        const uint32_t o = starts[si]; size_t m = 0; while (o + m < end && p[o + m]) m++;
        if (m >= strlen(isgrab[si])) { memcpy(p + o, isgrab[si], strlen(isgrab[si]) + 1); }
    }
    int done = 0;
    for (uint32_t i = 0; !pad && i + 2 < end; i++) {
        const int k = glyph_at(p + i);
        if (k < 0) continue;
        memcpy(p + i, s_rep3[k], 3); i += 2; done++;
    }
    static int logged = 0;
    if ((done || moved) && logged++ < 8)
        fprintf(stderr, "[prompts] text file 0x%08X: %d string(s) with full key names (%u reference(s)), %d icon(s) as short names%c",
                base, moved, nrefs, done, 10);
}
/* Resistance 2: on-screen button prompts for keyboard & mouse.
 * R2's text marks buttons with private-use characters drawn as PS3 button
 * icons by the font: U+E001..E012 are the physical buttons (E00A / E00E the
 * left / right stick) and U+E101..E116 are ACTIONS ("jump", "fire"...) that
 * the game points at a button icon of the current controller layout
 * (func_002AB274 -> func_0076C5D8; default layout func_002AC9EC). The hook on
 * the text-file registration (func_0068AF40, tools/hooks/r2.json) calls
 * r2_text_patch with the file object (+4 = the IGHW file). Every string with a
 * glyph is copied into new guest memory with the glyphs spelled out as the key
 * bound to that action in controls.txt ("[Space]", "LMB"), and the string
 * table entries that point at it are recorded; strings no table entry was
 * found for get a 3-character key name in place (a glyph is 3 bytes of UTF-8).
 * Both edits are kept as {original, keyboard} pairs and switched whenever a
 * controller is plugged in or out (cellPad.c g_r2_input_mode_cb), so a
 * controller shows the PS3 icons. R2_KEY_PROMPTS=0 turns this off. */
#include <mutex>
struct R2Glyph { uint16_t cp; const char* pad; const char* hint; const char* fixed; const char* full; };
static const R2Glyph s_r2_glyphs[] = {
    /* physical buttons */
    { 0xE001, "CIRCLE", "back" }, { 0xE002, "SQUARE", "reload" }, { 0xE003, "CROSS", "jump" },
    { 0xE004, "TRIANGLE", "switch weapon" }, { 0xE005, "SELECT", "" }, { 0xE006, "START", "" },
    { 0xE007, "L1", "" }, { 0xE008, "L2", "crouch" }, { 0xE009, "L3", "" },
    { 0xE00A, 0, 0, "Mov", "WASD" }, { 0xE00B, "R1", "" }, { 0xE00C, "R2", "" }, { 0xE00D, "R3", "" },
    { 0xE00E, 0, 0, "Mse", "Mouse" },
    { 0xE00F, "DPAD_UP", "" }, { 0xE010, "DPAD_DOWN", "" }, { 0xE011, "DPAD_LEFT", "" }, { 0xE012, "DPAD_RIGHT", "" },
    /* actions (the default layout's button) */
    { 0xE101, "INTERACT", "" }, { 0xE102, "R1", "fire" }, { 0xE103, "R2", "alternate" },
    { 0xE104, "TRIANGLE", "switch weapon" }, { 0xE105, "CIRCLE", "grenade" }, { 0xE106, "CROSS", "jump" },
    { 0xE107, "L1", "aim" }, { 0xE108, "L2", "crouch" }, { 0xE109, "SQUARE", "reload" },
    { 0xE10A, "DPAD_RIGHT", "flashlight" }, { 0xE10B, "DPAD_UP", "grenade" }, { 0xE10C, "DPAD_DOWN", "" },
    { 0xE10D, "DPAD_RIGHT", "" }, { 0xE10E, "CROSS", "confirm" }, { 0xE10F, "CIRCLE", "back" },
    { 0xE111, "DPAD_LEFT", "map" }, { 0xE114, "L2", "sprint" }, { 0xE115, "R3", "melee" },
    { 0xE116, "DPAD_DOWN", "" },
};
static char s_r2_rep3[128][4], s_r2_repf[128][24]; static int s_r2_have[128];
static int r2_glyph_at(const uint8_t* q)          /* slot of a mapped glyph at q, or -1 */
{
    if (q[0] != 0xEE || (q[1] != 0x80 && q[1] != 0x84) || q[2] < 0x80 || q[2] > 0xBF) return -1;
    const int k = (q[1] == 0x84 ? 64 : 0) + (q[2] - 0x80);
    return s_r2_have[k] ? k : -1;
}
/* the key for a button: the first controls.txt line for it whose comment
 * (the launcher row) mentions hint, else the first line for it */
static int r2_bound_key(const char* pad, const char* hint, char key[64])
{
    FILE* f = fopen("controls.txt", "r");
    if (!f) return 0;
    char line[256], first[64] = "", best[64] = "";
    while (!best[0] && fgets(line, sizeof line, f)) {
        char cmt[128] = "";
        if (char* h = strchr(line, '#')) {
            *h = 0; int n = 0;
            for (h++; *h && n < 127; h++) cmt[n++] = (char)tolower((unsigned char)*h);
            cmt[n] = 0; }
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0;
        char k[64], v[64];
        if (sscanf(line, " %63s", k) != 1 || sscanf(eq + 1, " %63s", v) != 1 || _stricmp(v, pad)) continue;
        if (!first[0]) snprintf(first, sizeof first, "%s", k);
        if (hint && hint[0] && strstr(cmt, hint)) snprintf(best, sizeof best, "%s", k);
    }
    fclose(f);
    snprintf(key, 64, "%s", best[0] ? best : first);
    return key[0] != 0;
}
static int r2_key_for(const char* pad, const char* hint, char out3[4], char outf[24])
{
    char k[64];
    if (!r2_bound_key(pad, hint, k)) return 0;
    key3(k, out3); keyfull(k, outf);
    return 1;
}
struct R2RefRec { uint32_t ea, orig, kbm; };
struct R2ByteRec { uint32_t ea; uint8_t orig[3], kbm[3]; };
struct R2TextFile { uint32_t base, end; int kbm; std::vector<R2RefRec> refs; std::vector<R2ByteRec> bytes; };
static std::mutex s_r2_mu;
static std::vector<R2TextFile> s_r2_files;
static volatile int s_r2_mode = -1;                  /* the mode every file is in, -1 = some differ */
static void r2_apply(R2TextFile& t, int kbm)        /* only where the memory still holds the other version */
{
    if (t.kbm == kbm) return;
    t.kbm = kbm;
    if (vm_read32(t.base) != 0x49474857u) return;    /* "IGHW": the file is gone */
    for (auto& r : t.refs) { const uint32_t cur = vm_read32(r.ea);
        if (cur == (kbm ? r.orig : r.kbm)) vm_write32(r.ea, kbm ? r.kbm : r.orig); }
    for (auto& b : t.bytes) { uint8_t* q = vm_base + b.ea;
        if (!memcmp(q, kbm ? b.orig : b.kbm, 3)) memcpy(q, kbm ? b.kbm : b.orig, 3); }
}
static void r2_icons_mode(int kbm);
static void r2_input_mode(int kbm)
{
    r2_icons_mode(kbm);
    if (s_r2_mode == kbm) return;
    std::lock_guard<std::mutex> lk(s_r2_mu);
    for (auto& t : s_r2_files) r2_apply(t, kbm);
    s_r2_mode = kbm;
}
extern "C" void (*volatile g_r2_input_mode_cb)(int kbm);
extern "C" void r2_text_patch(uint32_t file_obj)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("R2_KEY_PROMPTS"); en = e ? atoi(e) : 1; }
    if (!en || !file_obj || !vm_base) return;
    const uint32_t base = vm_read32(file_obj + 4);
    if (!base || vm_read32(base) != 0x49474857u) return;        /* "IGHW" */
    const uint32_t n = vm_read32(base + 8);
    if (n == 0 || n > 64) return;
    uint32_t end = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t off = vm_read32(base + 0x10 + 16 * i + 4), sz = vm_read32(base + 0x10 + 16 * i + 8) & 0xFFFFFFu;
        if (off + sz > end) end = off + sz;
    }
    if (!end || end > (16u << 20)) return;
    std::lock_guard<std::mutex> lk(s_r2_mu);
    static int init = 0;
    if (!init) { init = 1;
        for (auto& g : s_r2_glyphs) {
            const int i = ((g.cp >> 8) & 1 ? 64 : 0) + (g.cp & 0x3F);
            if (g.fixed) { memcpy(s_r2_rep3[i], g.fixed, 4); snprintf(s_r2_repf[i], 24, "%s", g.full); s_r2_have[i] = 1; }
            else {
                s_r2_have[i] = r2_key_for(g.pad, g.hint, s_r2_rep3[i], s_r2_repf[i]);
                if (!s_r2_have[i] && !strcmp(g.pad, "INTERACT"))      /* no use key: the Triangle key */
                    s_r2_have[i] = r2_key_for("TRIANGLE", "", s_r2_rep3[i], s_r2_repf[i]);
            } }
        g_r2_input_mode_cb = r2_input_mode;
    }
    /* the same file registered again: put it back as it was first */
    for (size_t fi = 0; fi < s_r2_files.size(); fi++)
        if (s_r2_files[fi].base == base) { r2_apply(s_r2_files[fi], 0); s_r2_files.erase(s_r2_files.begin() + fi); break; }
    int kbm = 1;
    { XINPUT_STATE xs; if (XInputGetState(0, &xs) == ERROR_SUCCESS) kbm = 0; }
    uint8_t* p = vm_base + base;
    std::vector<uint32_t> starts;                           /* strings with a glyph */
    for (uint32_t i = 0; i + 2 < end; i++) {
        if (r2_glyph_at(p + i) < 0) continue;
        uint32_t st = i; while (st > 0 && p[st - 1] && i - st < 4096) st--;
        if (st == 0 || p[st - 1]) { i += 2; continue; }
        starts.push_back(st);
        while (i < end && p[i]) i++;
    }
    if (starts.empty()) return;
    R2TextFile tf; tf.base = base; tf.end = end; tf.kbm = 0;
    { static int logged_files = 0;
      if (r2_diag() && logged_files++ < 12)
        if (FILE* f = fopen("logs\\prompts_r2.txt", logged_files == 1 ? "w" : "a")) {
            fprintf(f, "== text file 0x%08X (%u bytes)\n", base, end);
            for (uint32_t st : starts) {
                fprintf(f, "@0x%X: ", st);
                for (uint32_t k = st; k < end && p[k]; k++) {
                    const int g = (k + 2 < end) ? r2_glyph_at(p + k) : -1;
                    if (g >= 0) { fprintf(f, "{%s}", s_r2_repf[g]); k += 2; }
                    else fputc(p[k] >= 0x20 && p[k] < 0x7F ? p[k] : '?', f);
                }
                fputc('\n', f);
            }
            fclose(f); } }
    /* 1. Full names: find the string-table words that point at each string (by
     *    value: an offset from the file / a section, or an absolute address once
     *    the loader fixed them up, with other string pointers 4..24 bytes away). */
    std::vector<uint32_t> rels(1, 0);
    rels.push_back(0u - base);
    for (uint32_t i = 0; i < n; i++) { const uint32_t so = be32p(p + 0x10 + 16 * i + 4); if (so && so < end) rels.push_back(so); }
    auto plausible = [&](uint32_t v, uint32_t rel) -> bool {
        const uint32_t x = rel + v; return x > 0 && x < end && p[x - 1] == 0 && p[x] != 0; };
    std::unordered_multimap<uint32_t, uint32_t> words;
    words.reserve(end / 4);
    for (uint32_t i = 0x10; i + 4 <= end; i += 4) words.emplace(be32p(p + i), i);
    struct Ref { uint32_t pos, rel, si; };
    std::vector<Ref> refs;
    for (uint32_t si = 0; si < starts.size(); si++) {
        const uint32_t st = starts[si];
        for (uint32_t rel : rels) {
            if (rel > st && rel != 0u - base) continue;
            auto rg = words.equal_range(st - rel);
            for (auto it = rg.first; it != rg.second; ++it) {
                const uint32_t i = it->second; int ok = 0;
                for (uint32_t d = 4; d <= 24; d += 4)
                    ok += (i >= 0x10 + d && plausible(be32p(p + i - d), rel)) + (i + d + 4 <= end && plausible(be32p(p + i + d), rel));
                if (ok >= 2) refs.push_back({ i, rel, si });
            }
        }
    }
    std::vector<char> relocated(starts.size(), 0);
    if (!refs.empty()) {
        std::string out; std::vector<uint32_t> at(starts.size(), 0xFFFFFFFFu);
        for (auto& r : refs) {
            if (at[r.si] != 0xFFFFFFFFu) continue;
            const uint32_t o = starts[r.si];
            size_t m = 0; for (; o + m < end && p[o + m]; m++) {}
            at[r.si] = (uint32_t)out.size();
            for (size_t i = 0; i < m; i++) {
                const int k = (i + 2 < m) ? r2_glyph_at(p + o + i) : -1;
                if (k >= 0) { out += s_r2_repf[k]; i += 2; } else out += (char)p[o + i];
            }
            out += '\0';
        }
        const uint32_t blk = ps3_hle_heap_alloc((uint32_t)out.size() + 16);
        if (blk) {
            memcpy(vm_base + blk, out.data(), out.size());
            for (auto& r : refs) {
                const uint32_t ea = base + r.pos;
                tf.refs.push_back({ ea, vm_read32(ea), blk + at[r.si] - base - r.rel });
            }
            for (uint32_t si = 0; si < starts.size(); si++) if (at[si] != 0xFFFFFFFFu) relocated[si] = 1;
        }
    }
    /* 2. The rest: 3-character names in place. */
    for (size_t si = 0; si < starts.size(); si++) {
        if (relocated[si]) continue;
        for (uint32_t i = starts[si]; i + 2 < end && p[i]; i++) {
            const int k = r2_glyph_at(p + i);
            if (k < 0) continue;
            R2ByteRec b; b.ea = base + i; memcpy(b.orig, p + i, 3); memcpy(b.kbm, s_r2_rep3[k], 3);
            tf.bytes.push_back(b); i += 2;
        }
    }
    r2_apply(tf, kbm);
    static int logged = 0;
    if (logged++ < 8)
        fprintf(stderr, "[prompts] text file 0x%08X: %zu string(s), %zu table reference(s) to full key names, "
                "%zu icon(s) as short names, showing %s%c", base, starts.size(), tf.refs.size(), tf.bytes.size(),
                kbm ? "keys" : "PS3 buttons", 10);
    s_r2_files.push_back(std::move(tf));
    if (s_r2_mode != kbm) s_r2_mode = -1;
}
/* Resistance port (v170): keyboard keys on the Scaleform menu button icons.
 * The menus' SWFs show their button legends with loadMovie("img://button
 * enter") & co. (bottombar.swf, frontend.swf, pausemenu.swf...); the game
 * registers those names with the image loader from its own textures:
 * func_007675D8(name, texture header) builds a CellGcmTexture from the header
 * and hands it to the Scaleform image registry (func_00767438). The hook at the
 * top of func_007675D8 (tools/hooks/r2.json) points every "button X" name at a
 * linear A8R8G8B8 copy of the icon in a 1 MB block of guest memory we map for
 * the RSX (cellGcmMapMainMemory). That copy holds the game's icon (decoded)
 * while a controller is connected and a keycap with the key bound in
 * controls.txt otherwise, rewritten in place when a controller is plugged in or
 * out (g_r2_input_mode_cb; the RSX texture cache sees the write through its
 * page protection). An icon whose format can't be decoded is only replaced
 * when no controller is connected at registration. R2_KEY_PROMPTS=0 turns
 * this off too. */
#pragma comment(lib, "gdi32.lib")
extern "C" int32_t cellGcmMapMainMemory(uint32_t ea, uint32_t size, uint32_t* offset);
extern "C" uint32_t cellGcmResolveLocated(int local, uint32_t offset);
struct R2Icon { std::string name; uint32_t src, ea, hdr; int w, h, live; std::vector<uint8_t> pad, key; };
static std::mutex s_icon_mu;
static std::vector<R2Icon> s_icons;
static uint32_t s_icon_blk = 0, s_icon_io = 0, s_icon_used = 0;
static int s_icon_map = 0;                           /* 0 untried, 1 mapped, -1 failed */
static volatile int s_icon_mode = -1;
static const struct { const char* name; const char* pad; const char* hint; } s_icon_keys[] = {
    { "enter", "CROSS", "jump" }, { "back", "CIRCLE", "back" }, { "start", "START", "start" },
    { "select", "SELECT", "" }, { "triangle", "TRIANGLE", "switch weapon" }, { "square", "SQUARE", "reload" },
    { "l3", "L3", "" }, { "r3", "R3", "melee" },
};
static void r2_icon_label(const char* k, char out[16])
{
    static const struct { const char* n; const char* s; } t[] = {
        {"SPACE","Space"},{"BACKSPACE","Bksp"},{"ENTER","Enter"},{"ESC","Esc"},{"TAB","Tab"},
        {"SHIFT","Shift"},{"LSHIFT","Shift"},{"RSHIFT","RShift"},{"CTRL","Ctrl"},{"LCTRL","Ctrl"},{"RCTRL","RCtrl"},
        {"ALT","Alt"},{"MOUSE1","LMB"},{"MOUSE2","RMB"},{"MOUSE3","MMB"},{"MOUSE4","M4"},{"MOUSE5","M5"},
        {"WHEELUP","WhUp"},{"WHEELDOWN","WhDn"},{"UP","Up"},{"DOWN","Down"},{"LEFT","Left"},{"RIGHT","Right"},
        {"CAPSLOCK","Caps"},{"INSERT","Ins"},{"DELETE","Del"},{"HOME","Home"},{"END","End"},{"PGUP","PgUp"},{"PGDN","PgDn"},
        {"GRAVE","`"},{"MINUS","-"},{"EQUALS","="},{"LBRACKET","["},{"RBRACKET","]"},{"BACKSLASH","\\"},
        {"SEMICOLON",";"},{"APOSTROPHE","'"},{"COMMA",","},{"PERIOD","."},{"SLASH","/"},
    };
    for (auto& e : t) if (!_stricmp(k, e.n)) { snprintf(out, 16, "%s", e.s); return; }
    if (!_strnicmp(k, "NUM", 3) && strlen(k) == 4) { snprintf(out, 16, "N%c", k[3]); return; }
    snprintf(out, 16, "%.15s", k);
    for (char* c = out + 1; *c; c++) *c = (char)tolower((unsigned char)*c);
    out[0] = (char)toupper((unsigned char)out[0]);
}
/* A keycap: dark rounded key, light rim, the label in white, drawn 4x and
 * averaged down. Out: w*h pixels, A R G B bytes (a big-endian A8R8G8B8 word). */
static void r2_icon_render(const char* label, int w, int h, uint8_t* out)
{
    const int S = 4, W = w * S, H = h * S;
    std::vector<float> cov((size_t)w * h, 0.0f);
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bm = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    const float m = (float)(w < h ? w : h);
    const float inset = m * 0.05f + 0.5f, rim = m * 0.08f > 1.0f ? m * 0.08f : 1.0f, rad = m * 0.22f;
    if (bm && bits) {
        HGDIOBJ obm = SelectObject(dc, bm);
        memset(bits, 0, (size_t)W * H * 4);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        const int len = (int)strlen(label);
        const int tw = (int)((w - 2 * (inset + rim) - m * 0.06f) * S);
        HFONT f = NULL; HGDIOBJ of = NULL; SIZE sz = {};
        for (int fh = (int)(h * 0.60f * S); fh >= 4; fh -= 1) {
            f = CreateFontA(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                            NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
            if (!f) break;
            of = SelectObject(dc, f);
            GetTextExtentPoint32A(dc, label, len, &sz);
            if (sz.cx <= tw || fh <= 4) break;
            SelectObject(dc, of); DeleteObject(f); f = NULL;
        }
        if (f) {
            TextOutA(dc, (W - sz.cx) / 2, (H - sz.cy) / 2, label, len);
            GdiFlush();
            const uint8_t* px = (const uint8_t*)bits;
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
                cov[(size_t)(y / S) * w + x / S] += px[((size_t)y * W + x) * 4 + 1] / (255.0f * S * S);
            SelectObject(dc, of); DeleteObject(f);
        }
        SelectObject(dc, obm);
    }
    if (bm) DeleteObject(bm);
    if (dc) DeleteDC(dc);
    const float cx = w * 0.5f, cy = h * 0.5f, hx = w * 0.5f - inset, hy = h * 0.5f - inset;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const float qx = fabsf(x + 0.5f - cx) - (hx - rad), qy = fabsf(y + 0.5f - cy) - (hy - rad);
        const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, mx = qx > qy ? qx : qy;
        const float d = sqrtf(ox * ox + oy * oy) + (mx < 0 ? mx : 0) - rad;   /* < 0 inside the key */
        auto cl = [](float v) { return v < 0 ? 0.0f : v > 1 ? 1.0f : v; };
        const float outer = cl(0.5f - d), inner = cl(0.5f - (d + rim));
        float a = outer * 0.94f, c = inner * 28.0f + (outer - inner) * 225.0f;   /* body / rim grey */
        c = outer > 0 ? c / outer : 0;
        const float t = cl(cov[(size_t)y * w + x]) * inner;
        c = c * (1 - t) + 255.0f * t; a = a > t ? a : t;
        uint8_t* o = out + ((size_t)y * w + x) * 4;
        o[0] = (uint8_t)(a * 255.0f + 0.5f); o[1] = o[2] = o[3] = (uint8_t)(c + 0.5f);
    }
}
/* the game's icon as A R G B bytes; 0 for a format we don't decode */
static int r2_icon_decode(int fmt, int lin, uint32_t src, uint32_t pitch, int w, int h, uint8_t* out)
{
    const uint8_t* s = vm_base + src;
    auto lg = [](int v) { int l = 0; while ((1 << l) < v) l++; return (1 << l) == v ? l : -1; };
    if (fmt == 0x85) {
        const int lw = lg(w), lh = lg(h);
        if (!lin && (lw < 0 || lh < 0)) return 0;
        if (lin && pitch < (uint32_t)w * 4) pitch = (uint32_t)w * 4;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            uint32_t o;
            if (lin) o = (uint32_t)y * pitch + (uint32_t)x * 4;
            else { o = 0; int sh = 0;
                for (int b = 0; b < lw || b < lh; b++) {
                    if (b < lw) o |= (((uint32_t)x >> b) & 1u) << sh++;
                    if (b < lh) o |= (((uint32_t)y >> b) & 1u) << sh++; }
                o *= 4; }
            memcpy(out + ((size_t)y * w + x) * 4, s + o, 4);
        }
        return 1;
    }
    if (fmt < 0x86 || fmt > 0x88) return 0;
    const int bs = fmt == 0x86 ? 8 : 16, bw = (w + 3) / 4;
    for (int by = 0; by < (h + 3) / 4; by++) for (int bx = 0; bx < bw; bx++) {
        const uint8_t* b = s + ((size_t)by * bw + bx) * bs;
        const uint8_t* cb = fmt == 0x86 ? b : b + 8;
        const int c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8;
        int pal[4][4];
        for (int i = 0; i < 2; i++) { const int c = i ? c1 : c0;
            pal[i][0] = ((c >> 11) & 31) * 255 / 31; pal[i][1] = ((c >> 5) & 63) * 255 / 63; pal[i][2] = (c & 31) * 255 / 31; pal[i][3] = 255; }
        for (int k = 0; k < 3; k++) {
            if (fmt != 0x86 || c0 > c1) { pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3; pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3; }
            else { pal[2][k] = (pal[0][k] + pal[1][k]) / 2; pal[3][k] = 0; }
        }
        pal[2][3] = 255; pal[3][3] = (fmt != 0x86 || c0 > c1) ? 255 : 0;
        int al[8] = {};
        if (fmt == 0x88) {
            al[0] = b[0]; al[1] = b[1];
            for (int i = 2; i < 8; i++)
                al[i] = al[0] > al[1] ? ((8 - i) * al[0] + (i - 1) * al[1]) / 7
                      : i < 6 ? ((6 - i) * al[0] + (i - 1) * al[1]) / 5 : i == 6 ? 0 : 255;
        }
        const uint64_t abits = fmt == 0x88 ? ((uint64_t)b[2] | (uint64_t)b[3] << 8 | (uint64_t)b[4] << 16 |
            (uint64_t)b[5] << 24 | (uint64_t)b[6] << 32 | (uint64_t)b[7] << 40) : 0;
        const uint32_t ix = cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24;
        for (int py = 0; py < 4; py++) for (int pxx = 0; pxx < 4; pxx++) {
            const int x = bx * 4 + pxx, y = by * 4 + py, i = py * 4 + pxx;
            if (x >= w || y >= h) continue;
            const int* c = pal[(ix >> (2 * i)) & 3];
            int a = c[3];
            if (fmt == 0x87) a = ((b[i / 2] >> ((i & 1) * 4)) & 15) * 17;
            else if (fmt == 0x88) a = al[(abits >> (3 * i)) & 7];
            uint8_t* o = out + ((size_t)y * w + x) * 4;
            o[0] = (uint8_t)a; o[1] = (uint8_t)c[0]; o[2] = (uint8_t)c[1]; o[3] = (uint8_t)c[2];
        }
    }
    return 1;
}
static void r2_icons_apply(int kbm)                  /* s_icon_mu held */
{
    for (auto& ic : s_icons)
        if (ic.live && ic.ea) memcpy(vm_base + ic.ea, kbm ? ic.key.data() : ic.pad.data(), ic.key.size());
}
static void r2_icons_mode(int kbm)
{
    if (s_icon_mode == kbm) return;
    std::lock_guard<std::mutex> lk(s_icon_mu);
    if (s_icon_mode == kbm) return;
    r2_icons_apply(kbm);
    s_icon_mode = kbm;
}
static uint32_t r2_icon_alloc(uint32_t bytes)        /* in the RSX-mapped block, 128-byte aligned */
{
    if (s_icon_map == 0) {
        s_icon_map = -1;
        const uint32_t raw = ps3_hle_heap_alloc(0x200000), out = ps3_hle_heap_alloc(16);
        if (raw && out) {
            const uint32_t blk = (raw + 0xFFFFFu) & ~0xFFFFFu;
            if (cellGcmMapMainMemory(blk, 0x100000, (uint32_t*)(uintptr_t)out) == 0) {
                s_icon_blk = blk; s_icon_io = vm_read32(out); s_icon_map = 1; }
        }
        if (s_icon_map < 0) fprintf(stderr, "[prompts] menu icons: could not map texture memory for the RSX%c", 10);
    }
    if (s_icon_map < 0 || s_icon_used + bytes > 0x100000) return 0;
    const uint32_t ea = s_icon_blk + s_icon_used;
    s_icon_used = (s_icon_used + bytes + 127) & ~127u;
    return ea;
}
extern "C" void r2_menu_icon(ppu_context* ctx)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("R2_KEY_PROMPTS"); en = e ? atoi(e) : 1; }
    const uint32_t name_ea = (uint32_t)ctx->gpr[3], hdr = (uint32_t)ctx->gpr[4];
    if (!en || !vm_base || !name_ea || !hdr) return;
    char name[32]; int n = 0;
    for (; n < 31 && vm_base[name_ea + n]; n++) name[n] = (char)vm_base[name_ea + n];
    name[n] = 0;
    if (strncmp(name, "button ", 7)) return;
    const char* pad = 0; const char* hint = "";
    for (auto& k : s_icon_keys) if (!strcmp(name + 7, k.name)) { pad = k.pad; hint = k.hint; }
    if (!pad) return;
    const uint32_t flags = vm_read32(hdr + 4), wh = vm_read32(hdr + 0x18);
    const int w = (int)(wh >> 16), h = (int)(wh & 0xFFFF);
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return;
    std::lock_guard<std::mutex> lk(s_icon_mu);
    int kbm = 1;
    { XINPUT_STATE xs; if (XInputGetState(0, &xs) == ERROR_SUCCESS) kbm = 0; }
    if (s_icon_mode < 0) s_icon_mode = kbm;
    kbm = s_icon_mode;
    R2Icon* ic = nullptr;
    for (auto& e : s_icons) if (e.name == name && e.src == hdr) ic = &e;
    if (!ic) {
        R2Icon e; e.name = name; e.src = hdr; e.w = w; e.h = h; e.live = 0; e.ea = 0; e.hdr = 0;
        char key[64] = "", label[16] = "";
        if (!r2_bound_key(pad, hint, key)) {
            fprintf(stderr, "[prompts] menu icon '%s': no key bound to %s, keeping the PS3 icon%c", name, pad, 10);
            return; }
        r2_icon_label(key, label);
        e.key.resize((size_t)w * h * 4); e.pad.resize(e.key.size());
        r2_icon_render(label, w, h, e.key.data());
        static const int fmts[8] = { 0x81, 0, 0, 0, 0x85, 0x86, 0x87, 0x88 };
        const int fi = (int)((flags >> 8) & 0x1F) - 1, fmt = fi >= 0 && fi < 8 ? fmts[fi] : 0;
        const uint32_t src = cellGcmResolveLocated(((flags >> 1) & 1) == 0, vm_read32(hdr));
        const uint32_t mip = vm_read32(hdr + 0xC);
        int ok = src && ((mip >> 15) & 15) >= ((mip >> 27) & 15) &&
                 r2_icon_decode(fmt, (flags & 0x2000) != 0, src, vm_read32(hdr + 0x1C) & 0xFFFFF, w, h, e.pad.data());
        if (ok) { ok = 0; for (size_t i = 0; i < e.pad.size(); i += 4) ok |= e.pad[i]; }   /* not all transparent */
        e.ea = r2_icon_alloc((uint32_t)e.key.size());
        e.hdr = e.ea ? ps3_hle_heap_alloc(0x20) : 0;
        if (e.hdr) {
            memset(vm_base + e.hdr, 0, 0x20);
            vm_write32(e.hdr + 0x00, s_icon_io + (e.ea - s_icon_blk));
            vm_write32(e.hdr + 0x04, (5u << 8) | 0x2000u | (flags & 0x4000u) | 2u);   /* A8R8G8B8, linear, main memory */
            vm_write32(e.hdr + 0x0C, 0);                                            /* one mip level */
            vm_write32(e.hdr + 0x10, 0xAAE4);                                       /* identity remap */
            vm_write32(e.hdr + 0x18, wh);
            vm_write32(e.hdr + 0x1C, (uint32_t)w * 4);
            e.live = ok;
        }
        fprintf(stderr, "[prompts] menu icon '%s' %dx%d fmt 0x%02X remap 0x%X -> key '%s'%s%c", name, w, h, fmt, vm_read32(hdr + 0x10), label,
                !e.hdr ? " (no texture memory: PS3 icon)" : ok ? "" : " (icon not decoded: no live switch)", 10);
        s_icons.push_back(std::move(e));
        ic = &s_icons.back();
        if (ic->live) memcpy(vm_base + ic->ea, kbm ? ic->key.data() : ic->pad.data(), ic->key.size());
        else if (ic->hdr) memcpy(vm_base + ic->ea, ic->key.data(), ic->key.size());
        g_r2_input_mode_cb = r2_input_mode;
    }
    if (ic->hdr && (ic->live || kbm)) ctx->gpr[4] = ic->hdr;
}
/* Resistance port (v153): separate melee and interact.
 * The game's "use" checks -- func_00296EBC (switches, doors, turrets...: claims
 * the player's use slot, then counts the button held/pressed) and func_00295FC8
 * (press-and-hold, e.g. leaving a vehicle) -- read the Action button (bit
 * 0x04000000 of the player's controller history slot, +0x54 held / +0x58 just
 * pressed, plus its time-since-pressed at ctrl+0x19A0+26*4, a press buffer). For the length of those
 * calls the bit is replaced by the INTERACT input (libs/input/cellPad.c), and
 * put back afterwards, so the melee code elsewhere still sees Triangle. */
extern "C" volatile int g_port_use_kbm, g_port_kbm_interact_bound, g_port_input_kbm;
extern "C" volatile uint16_t g_port_pad_btns0, g_port_pad_interact;
static thread_local int t_use_depth = 0;
static thread_local uint32_t t_use_slot = 0, t_use_ctrl = 0, t_use_s54, t_use_s58, t_use_hold;
static uint32_t s_use_last_slot = 0; static int s_use_held = 0, s_use_prev = 0;
static double s_use_since = 100.0; static LARGE_INTEGER s_use_t0;
extern "C" void port_use_begin(uint32_t player, uint32_t mask)
{
    if (t_use_depth++ || mask != 0x04000000u) return;
    t_use_slot = 0;
    static int en = -1;
    if (en < 0) { const char* e = getenv("PORT_SPLIT_USE"); en = e ? atoi(e) : 1; }
    if (!en || !player) return;
    int held;
    if (g_port_input_kbm) { if (!g_port_kbm_interact_bound) return; held = g_port_use_kbm != 0; }
    else { if (!g_port_pad_interact) return; held = (g_port_pad_btns0 & g_port_pad_interact) != 0; }
    const uint32_t ctrl = vm_read32(player + 0x3E90);
    if (!ctrl || ctrl >= 0xF0000000u) return;
    if (g_port_opts_ea && vm_read32(ctrl + 0x1AC0) != g_port_opts_ea) return;   /* not the player's controller */
    const uint32_t idx = vm_read32(ctrl + 0x1AA4);
    if (idx >= 64) return;
    const uint32_t slot = ctrl + idx * 96;
    if (slot != s_use_last_slot) {                  /* a new game frame */
        LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
        const double dt = s_use_t0.QuadPart ? (double)(now.QuadPart - s_use_t0.QuadPart) / (double)f.QuadPart : 0.0;
        s_use_t0 = now;
        s_use_last_slot = slot; s_use_prev = s_use_held; s_use_held = held;
        s_use_since = (s_use_held && !s_use_prev) ? 0.0 : s_use_since + (dt > 0.1 ? 0.1 : dt);
    }
    const int pressed = s_use_held && !s_use_prev;
    t_use_slot = slot; t_use_ctrl = ctrl;
    t_use_s54 = vm_read32(slot + 0x54); t_use_s58 = vm_read32(slot + 0x58);
    t_use_hold = vm_read32(ctrl + 0x19A0 + 26 * 4);
    vm_write32(slot + 0x54, (t_use_s54 & ~mask) | (s_use_held ? mask : 0));
    vm_write32(slot + 0x58, (t_use_s58 & ~mask) | (pressed ? mask : 0));
    { float h = (float)s_use_since; uint32_t u; memcpy(&u, &h, 4); vm_write32(ctrl + 0x19A0 + 26 * 4, u); }
    static int logged = 0;
    if (pressed && logged++ < 20) fprintf(stderr, "[use] interact pressed (player 0x%08X)%c", player, 10);
}
extern "C" void port_use_end(void)
{
    if (t_use_depth > 0) t_use_depth--;
    if (t_use_depth || !t_use_slot) return;
    const uint32_t m = 0x04000000u, slot = t_use_slot;
    vm_write32(slot + 0x54, (vm_read32(slot + 0x54) & ~m) | (t_use_s54 & m));
    vm_write32(slot + 0x58, (vm_read32(slot + 0x58) & ~m) | (t_use_s58 & m));
    vm_write32(t_use_ctrl + 0x19A0 + 26 * 4, t_use_hold);
    t_use_slot = 0;
}
/* v154: breaking free from a grab (Menial etc.). func_00294318 is the game's
 * "how hard are the sticks being shaken" meter: over the last moments of the
 * controller history it sums |change| of both sticks' X and Y (slot+0x40..0x4C)
 * weighted by frame time, into *out. On PS3 you shake both sticks; on keyboard
 * & mouse only the mouse (fed to the right stick) should be needed, so with no
 * controller the meter counts double -- the mouse alone does what both sticks
 * did. PORT_GRAB_MOUSE_SCALE overrides the factor. A controller is untouched. */
/* v159: the grab escape meter on keyboard & mouse is the MOUSE alone. The
 * game's meter (func_00294318) sums both sticks' movement over the last
 * window_s seconds (x frame time); WASD (on/off keys) dominated it and the
 * smoothed mouse stick barely registered. With no controller the reading is
 * replaced by the mouse travel over the same window, scaled so vigorous
 * shaking (~3000 counts/s) matches flicking both sticks hard (~32 stick
 * units/s x 1/30 s). WASD no longer counts. PORT_GRAB_MOUSE_K overrides the
 * scale (per count), PORT_GRAB_MOUSE=0 = the game's own meter.
 * logs\grab.txt: game reading vs mouse reading, every 100 ms while it runs. */
extern "C" volatile unsigned long long g_kbm_mouse_abs;
extern "C" void port_waggle(uint32_t out_ea, double window_s)
{
    if (!out_ea) return;
    static int en = -1; static double k = 0;
    if (en < 0) { const char* e = getenv("PORT_GRAB_MOUSE"); en = e ? atoi(e) : 1;
                  const char* q = getenv("PORT_GRAB_MOUSE_K"); k = q ? atof(q) : 3.75e-5; }   /* v159c: ~2-3 s of hard shaking */
    /* mouse travel history: (ms, cumulative counts), one sample per call */
    static ULONGLONG ht[256]; static unsigned long long hc[256]; static int hn = 0, hi = 0;
    const ULONGLONG now = GetTickCount64();
    const unsigned long long cur = g_kbm_mouse_abs;
    ht[hi] = now; hc[hi] = cur; hi = (hi + 1) & 255; if (hn < 256) hn++;
    if (!(window_s > 0)) window_s = 1.0;
    if (window_s > 5) window_s = 5;
    if (window_s < 4) window_s = 4;   /* v159c: mouse looks back 4 s, so a harder setting stays reachable */
    unsigned long long base = cur;
    for (int j = 1; j <= hn; j++) {
        const int x = (hi - j) & 255;
        if (now - ht[x] > (ULONGLONG)(window_s * 1000.0)) break;
        base = hc[x];
    }
    const double mouse = (double)(cur - base) * k;
    uint32_t w = vm_read32(out_ea); float f; memcpy(&f, &w, 4);
    const int kbm = g_port_input_kbm != 0 || now - g_ml_kbm_ms < 500;   /* keyboard pad fed port 0 recently */
    { static int n = 0; static ULONGLONG last = 0;
      if (r2_diag() && (f > 0 || mouse > 0) && n < 3000 && now - last >= 100) { last = now;
        if (FILE* lf = fopen("logs\\grab.txt", n++ ? "a" : "w")) {
            fprintf(lf, "t=%llu kbm=%d window=%.2f game=%.4f mouse=%.4f\n", (unsigned long long)now, kbm, window_s, f, mouse); fclose(lf); } } }
    if (!kbm || !en) return;
    f = (float)mouse; memcpy(&w, &f, 4); vm_write32(out_ea, w);
}
/* v160b diag: menu probe (front end). To put a mouse pointer on the menus
 * the port needs the menu's "highlighted item" variable. Each time an arrow
 * key (or W/S) is pressed in the front end, guest RAM is copied, and 400 ms
 * later compared: every byte / halfword / word that moved by exactly +1 on
 * Down/Right or -1 on Up/Left scores a point (and loses one on a wrong move).
 * The best candidates go to logs\menuprobe.txt after every press.
 * PORT_MENU_PROBE=0 disables. */
static DWORD WINAPI menu_probe_thread(LPVOID)
{
    /* v160g: the highlight is "some value that is a function of the
     * position". Position p follows the presses (Down +1, Up -1, from 0).
     * Every word that changed on the first press is recorded after each press;
     * the ones whose value is the same whenever p is the same, and different
     * whenever p differs, are what the menu keeps (an index, an item pointer
     * or a position -- whatever form). logs\menuprobe.txt after each press. */
    extern uint8_t* vm_base;
    std::vector<uint32_t> ea; std::vector<std::vector<uint32_t>> vals; std::vector<int> pos;
    int was[4] = { 0, 0, 0, 0 }, p = 0;
    const int vks[4][2] = { { VK_DOWN, 'S' }, { VK_UP, 'W' }, { VK_RIGHT, 'D' }, { VK_LEFT, 'A' } };
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> snap; ULONGLONG snap_t = 0;
    auto focused = []() { HWND fg = GetForegroundWindow(); DWORD pid = 0; if (fg) GetWindowThreadProcessId(fg, &pid); return pid == GetCurrentProcessId(); };
    auto rd = [&](uint32_t a) { const uint8_t* q = vm_base + a; return (uint32_t)((q[0] << 24) | (q[1] << 16) | (q[2] << 8) | q[3]); };
    for (;;) {
        Sleep(10);
        if (!vm_base) continue;
        if (pos.empty() && GetTickCount64() - snap_t > 2000 && focused()) {
            snap.clear(); size_t total = 0;
            for (uintptr_t q = 0x10000; q < 0xC0000000u; ) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(vm_base + q, &mi, sizeof mi)) break;
                uintptr_t end = (uintptr_t)((uint8_t*)mi.BaseAddress + mi.RegionSize) - (uintptr_t)vm_base;
                if (end > 0xC0000000u) end = 0xC0000000u;
                if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                    (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && total + (end - q) <= (768u << 20)) {
                    snap.push_back({ (uint32_t)q, std::vector<uint8_t>(vm_base + q, vm_base + end) });
                    total += end - q;
                }
                q = end;
            }
            snap_t = GetTickCount64();
        }
        int dir = 0;
        for (int k = 0; k < 4; k++) {
            const int d = (GetAsyncKeyState(vks[k][0]) & 0x8000) || (GetAsyncKeyState(vks[k][1]) & 0x8000);
            if (d && !was[k]) dir = (k == 0 || k == 2) ? 1 : -1;
            was[k] = d;
        }
        if (!dir || !focused()) continue;
        if (pos.empty()) {
            if (snap.empty()) continue;
            pos.push_back(0);                               /* the baseline */
            Sleep(500);
            for (auto& r : snap) {
                const uint8_t* o = r.second.data(); const uint8_t* n = vm_base + r.first;
                for (size_t i = 0; i + 4 <= r.second.size(); i += 4)
                    if (*(const uint32_t*)(o + i) != *(const uint32_t*)(n + i) && ea.size() < 4000000) {
                        ea.push_back((uint32_t)(r.first + i));
                        vals.push_back({ (uint32_t)((o[i] << 24) | (o[i + 1] << 16) | (o[i + 2] << 8) | o[i + 3]) });
                    }
            }
            snap.clear(); snap.shrink_to_fit();
            p += dir; pos.push_back(p);
            for (size_t c = 0; c < ea.size(); c++) vals[c].push_back(rd(ea[c]));
        } else {
            Sleep(500);
            p += dir; pos.push_back(p);
            for (size_t c = 0; c < ea.size(); c++) vals[c].push_back(rd(ea[c]));
        }
        /* consistency: same p -> same value, different p -> different value */
        int ndist = 0; { int seen[64] = {0}; for (int q : pos) if (q >= -32 && q < 32 && !seen[q + 32]++) ndist++; }
        std::vector<size_t> good;
        for (size_t c = 0; c < ea.size(); c++) {
            uint32_t byp[64]; bool have[64] = {false}; bool ok = true;
            for (size_t k = 0; k < pos.size() && ok; k++) {
                const int q = pos[k] + 32; if (q < 0 || q >= 64) continue;
                if (have[q] && byp[q] != vals[c][k]) ok = false;
                byp[q] = vals[c][k]; have[q] = true;
            }
            for (int x = 0; x < 64 && ok; x++) for (int y = x + 1; y < 64 && ok; y++)
                if (have[x] && have[y] && byp[x] == byp[y]) ok = false;
            if (ok) good.push_back(c);
        }
        /* v160h: once all positions were seen twice: dump the objects the
         * pointer-type candidates point at, the memory around the holders,
         * and catch the code that writes the first holder (debug-register
         * write watch on every thread, 20 s -- keep pressing up/down). */
        static int dumped = 0;
        if (!dumped && pos.size() >= 13 && !good.empty()) {
            dumped = 1;
            if (FILE* f = fopen("logs\\menudump.txt", "w")) {
                auto hexdump = [&](uint32_t base, uint32_t len) {
                    for (uint32_t o = 0; o < len; o += 16) {
                        fprintf(f, "  %08X:", base + o);
                        for (uint32_t k = 0; k < 16; k += 4) fprintf(f, " %08X", rd(base + o + k));
                        fprintf(f, "  |");
                        for (uint32_t k = 0; k < 16; k += 4) { uint32_t u = rd(base + o + k); float fv; memcpy(&fv, &u, 4);
                            if (fv == fv && fabsf(fv) > 1e-4f && fabsf(fv) < 1e6f) fprintf(f, " %10.4f", fv); else fprintf(f, " %10s", "."); }
                        fputc('\n', f);
                    } };
                for (size_t gi = 0; gi < good.size(); gi++) {
                    const size_t c = good[gi];
                    fprintf(f, "== holder 0x%08X\n", ea[c]); hexdump((ea[c] & ~15u) - 0x100, 0x200);
                    std::vector<uint32_t> seenv;
                    for (uint32_t v : vals[c]) {
                        if (v < 0x10000 || v >= 0xC0000000u) continue;
                        if (std::find(seenv.begin(), seenv.end(), v) != seenv.end()) continue;
                        seenv.push_back(v);
                        fprintf(f, "== object 0x%08X (from holder 0x%08X)\n", v, ea[c]); hexdump(v & ~15u, 0x180);
                    }
                }
                fclose(f);
            }
            /* write watch on the first pointer-like holder */
            uint32_t h = 0;
            for (size_t gi = 0; gi < good.size() && !h; gi++) if (vals[good[gi]][1] >= 0x10000 && vals[good[gi]][1] < 0xC0000000u) h = ea[good[gi]];
            if (h) {
                static PVOID vh = NULL; if (!vh) vh = AddVectoredExceptionHandler(1, pw_veh);
                s_pw_n = 0; s_pw_on = 1;
                DWORD64 aw[4] = { (DWORD64)(vm_base + h), 0, 0, 0 };
                static DWORD tids[512]; int nt = 0;
                { HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te; te.dwSize = sizeof te;
                  if (sn != INVALID_HANDLE_VALUE && Thread32First(sn, &te)) {
                      do { if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId() && nt < 512) tids[nt++] = te.th32ThreadID; } while (Thread32Next(sn, &te)); }
                  if (sn != INVALID_HANDLE_VALUE) CloseHandle(sn); }
                for (int i = 0; i < nt; i++) pw_set(tids[i], aw, 1);
                Sleep(20000);
                const DWORD64 z[4] = { 0, 0, 0, 0 }; for (int i = 0; i < nt; i++) pw_set(tids[i], z, 0);
                Sleep(50); s_pw_on = 0;
                const DWORD64 mb = (DWORD64)GetModuleHandleA(NULL);
                if (FILE* f = fopen("logs\\menudump.txt", "a")) {
                    fprintf(f, "== accesses to holder 0x%08X (module base 0x%llX)\n", h, (unsigned long long)mb);
                    for (LONG i = 0; i < s_pw_n && i < 128; i++)
                        fprintf(f, "rva=0x%llX hits=%ld tid=%lu\n", (unsigned long long)(s_pw_hits[i].rip - mb), s_pw_hits[i].n, s_pw_hits[i].tid);
                    fclose(f);
                }
            }
        }
        if (FILE* f = fopen("logs\\menuprobe.txt", "w")) {
            fprintf(f, "presses=%zu positions:", pos.size() - 1);
            for (int q : pos) fprintf(f, " %d", q);
            fprintf(f, "\ntracked=%zu consistent=%zu (positions seen %d)\n", ea.size(), good.size(), ndist);
            for (size_t i = 0; i < good.size() && i < 300; i++) {
                const size_t c = good[i];
                fprintf(f, "ea=0x%08X:", ea[c]);
                for (uint32_t v : vals[c]) fprintf(f, " %08X", v);
                fputc('\n', f);
            }
            fclose(f);
        }
    }
    return 0;
}
/* v160p: the front-end menus are Anark scenes; their button hints are plain
 * strings in the loaded scene data, "O - BACK" and "X - SELECT", and the
 * text renderer turns the "O - " / "X - " prefix into the button picture.
 * Rewritten in place (same length, so nothing in the scene moves) whenever
 * a scene is loaded: "ESC BACK", "SPC SELECT" -- no prefix, so plain words.
 * Scans guest RAM every 2 s (front end only). PORT_FE_HINTS=0 disables. */
static DWORD WINAPI fe_hints_thread(LPVOID)
{
    extern uint8_t* vm_base;
    static const struct { const char* from; const char* to; } rp[] = {
        { "O - BACK", "ESC BACK" }, { "X - SELECT", "SPC SELECT" },
    };
    int total = 0;
    for (;;) {
        Sleep(250);
        if (!vm_base) continue;
        int n = 0;
        for (uintptr_t q = 0x40000000u; q < 0x50000000u; ) {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(vm_base + q, &mi, sizeof mi)) break;
            uintptr_t end = (uintptr_t)((uint8_t*)mi.BaseAddress + mi.RegionSize) - (uintptr_t)vm_base;
            if (end > 0x50000000u) end = 0x50000000u;
            if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
                uint8_t* bp = vm_base + q; const size_t len = end - q;
                /* v160q: also the UTF-16 (big-endian) copies the text renderer may keep */
                for (size_t i = 2; i + 26 < len; i += 2) {
                    if (bp[i] != 0 || (bp[i + 1] != 'O' && bp[i + 1] != 'X')) continue;
                    for (auto& r : rp) {
                        const size_t L = strlen(r.from); size_t k = 0;
                        for (; k < L && i + 2 * k + 1 < len; k++) if (bp[i + 2 * k] != 0 || bp[i + 2 * k + 1] != (uint8_t)r.from[k]) break;
                        if (k == L && i + 2 * L + 1 < len && bp[i + 2 * L] == 0 && bp[i + 2 * L + 1] == 0) {
                            for (k = 0; k < L; k++) bp[i + 2 * k + 1] = (uint8_t)r.to[k];
                            n++; static int u16 = 0; if (u16++ < 5) fprintf(stderr, "[menu] UTF-16 hint at 0x%08X%c", (unsigned)(q + i), 10);
                        }
                    }
                }
                for (size_t i = 1; i + 12 < len; i++) {
                    if (bp[i] != 'O' && bp[i] != 'X') continue;
                    if (bp[i - 1] != 0x00 && bp[i - 1] != 0xFF) continue;
                    for (auto& r : rp) {
                        const size_t L = strlen(r.from);
                        if (i + L < len && !memcmp(bp + i, r.from, L) && bp[i + L] == 0) { memcpy(bp + i, r.to, L); n++;
                            static int a8 = 0; if (a8++ < 30) fprintf(stderr, "[menu] hint at 0x%08X%c", (unsigned)(q + i), 10); }
                    }
                }
            }
            q = end;
        }
        if (n) { total += n; static int said = 0; if (said++ < 10) fprintf(stderr, "[menu] %d button hint(s) -> key names (%d so far)%c", n, total, 10); }
    }
    return 0;
}
static void menu_probe_start(void)
{
    { char mp[MAX_PATH] = ""; GetModuleFileNameA(NULL, mp, MAX_PATH);
      const char* b = strrchr(mp, '\\'); b = b ? b + 1 : mp;
      const char* eh = getenv("PORT_FE_HINTS");
      if (!_stricmp(b, "PortFrontEnd.exe") && eh && atoi(eh)) CreateThread(NULL, 0, fe_hints_thread, NULL, 0, NULL); }   /* v160r: off (the scene strings are not what is drawn) */
    char mp[MAX_PATH] = ""; GetModuleFileNameA(NULL, mp, MAX_PATH);
    const char* b = strrchr(mp, '\\'); b = b ? b + 1 : mp;
    const char* e = getenv("PORT_MENU_PROBE");   /* v160i: off unless PORT_MENU_PROBE=1 */
    if (_stricmp(b, "PortFrontEnd.exe") || !e || !atoi(e)) return;
    CreateThread(NULL, 0, menu_probe_thread, NULL, 0, NULL);
}
/* v161 diag: the black screen after the Grimsby door lever -- the main
 * thread loops in func_0002F54C waiting for a file job (func_00043488 opens
 * the path at job+0x1C, func_000434DC steps it; state at +4, handle +0x11C).
 * Log each open, and the job's state once a second while it is polled. */
extern "C" void port_ld_open(uint32_t job)
{
    extern uint8_t* vm_base;
    char path[256]; int k = 0;
    for (; k < 255; k++) { const char c = (char)vm_base[job + 0x1C + k]; if (!c) break; path[k] = c; }
    path[k] = 0;
    fprintf(stderr, "[loadjob] open job=0x%08X '%s'%c", job, path, 10);
}
extern "C" void port_ld_vcall(uint32_t fn, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t obj)
{
    extern uint8_t* vm_base;
    char path[128]; int k = 0;
    if (a4 && a4 < 0xC0000000u) for (; k < 127; k++) { const char c = (char)vm_base[a4 + k]; if (!c || c < 32 || c > 126) break; path[k] = c; }
    path[k] = 0;
    static int n = 0;
    if (n++ < 60) fprintf(stderr, "[loadjob] vfs open fn=0x%08X obj=0x%08X path='%s' r5=0x%X r6=0x%X%c", fn, obj, path, a5, a6, 10);
    if (n <= 2 && obj) {   /* v161c: the IO object's three mount roots and their device ids (func_000DAF10) */
        auto str = [&](uint32_t ea, char* o) { int i = 0; for (; i < 250; i++) { const char c = (char)vm_base[ea + i]; if (!c) break; o[i] = (c >= 32 && c < 127) ? c : '?'; } o[i] = 0; };
        char m0[256], m1[256], m2[256]; str(obj + 5, m0); str(obj + 0x10A, m1); str(obj + 0x20A, m2);
        fprintf(stderr, "[loadjob] io mounts: '%s' -> 0x%X | '%s' -> 0x%X | '%s' -> 0x%X | +534=0x%X +105=%d +4=%d%c",
                m0, vm_read32(obj + 0x528), m1, vm_read32(obj + 0x52C), m2, vm_read32(obj + 0x530), vm_read32(obj + 0x534),
                vm_base[obj + 0x105], vm_base[obj + 4], 10);
    }
    if (n <= 2 && obj) {   /* v161d: the 64 open-file slots (func_000DCF48 needs one with +0x110 == 0) */
        const uint32_t t = vm_read32(obj + 0x534);
        for (int i = 0; i < 64 && t; i++) {
            const uint32_t e = t + i * 0x138; char nm[256]; int k = 0;
            for (; k < 200; k++) { const char c = (char)vm_base[e + k]; if (!c) break; nm[k] = (c >= 32 && c < 127) ? c : '?'; }
            nm[k] = 0;
            fprintf(stderr, "[loadjob] slot %2d use=0x%X st=0x%X +104=0x%X +108=0x%X +10C=0x%X +114=0x%X '%s'%c", i, vm_read32(e + 0x110), vm_read32(e + 0x100),
                    vm_read32(e + 0x104), vm_read32(e + 0x108), vm_read32(e + 0x10C), vm_read32(e + 0x114), nm, 10);
        }
        const uint32_t r = vm_read32(obj + 0x538); int busy = 0;
        for (int i = 0; i < 64 && r; i++) busy += vm_read32(r + i * 8 + 4) != 0;
        fprintf(stderr, "[loadjob] request slots busy: %d of 64%c", busy, 10);
    }
}
extern "C" void port_ld_vret(int32_t r)
{
    static int n = 0;
    if (n++ < 60) fprintf(stderr, "[loadjob] vfs open -> %d (0x%X)%c", r, (uint32_t)r, 10);
}
/* v161e: inside func_000DDA18 (game IO async open): 1 = device from func_000DAF10 (dir, name), 2 = file/request slot, 3 = device open result */
static void port_ld_str(uint32_t ea, char* out, int n)
{
    int i = 0;
    if (ea) for (; i < n - 1; i++) { const char c = (char)vm_read8(ea + i); if (!c) break; out[i] = (c >= 32 && c < 127) ? c : '?'; }
    out[i] = 0;
}
/* v161f FIX: func_000D9B9C splits paths it doesn't recognise after the 2nd '/', so
   '/dev_hdd1/cache/BCES00226/hdd1_movie_stash' becomes dir '/dev_hdd1' and func_000DAF10 finds no
   device (0) -> the open fails forever (Grimsby lever black screen). If that happens, re-split the
   path against the IO object's own mount roots and return that mount's device. */
extern "C" uint32_t port_ld_fixdev(uint32_t obj, uint32_t dir_ea, uint32_t name_ea)
{
    char d[256], f[256], full[520];
    port_ld_str(dir_ea, d, 256); port_ld_str(name_ea, f, 256);
    snprintf(full, sizeof full, "%s/%s", d, f[0] == '/' ? f + 1 : f);
    static const uint32_t roots[3] = { 5, 0x10A, 0x20A }, devs[3] = { 0x528, 0x52C, 0x530 };
    int best = -1; size_t bl = 0;
    for (int k = 0; k < 3; k++) {
        char m[256]; port_ld_str(obj + roots[k], m, 256);
        const size_t ml = strlen(m);
        if (ml && ml > bl && !strncmp(full, m, ml) && full[ml] == '/' && vm_read32(obj + devs[k])) { best = k; bl = ml; }
    }
    if (best < 0) { fprintf(stderr, "[loadjob] fixdev: no mount for '%s'%c", full, 10); return 0; }
    const char* rest = full + bl + 1;
    for (size_t i = 0; i < 255; i++) { vm_write8(dir_ea + (uint32_t)i, (uint8_t)(i < bl ? full[i] : 0)); if (i >= bl) break; }
    const size_t rl = strlen(rest);
    for (size_t i = 0; i <= rl && i < 255; i++) vm_write8(name_ea + (uint32_t)i, (uint8_t)rest[i]);
    const uint32_t dev = vm_read32(obj + devs[best]);
    fprintf(stderr, "[loadjob] fixdev: '%s' -> dir='%.*s' name='%s' device=0x%X%c", full, (int)bl, full, rest, dev, 10);
    return dev;
}
extern "C" void port_ld_step(int tag, uint32_t a, uint32_t b, uint32_t c)
{
    static int n = 0;
    if (tag < 4 && n++ >= 90) return;
    if (tag == 1) {
        char d[256], f[256]; port_ld_str(b, d, 256); port_ld_str(c, f, 256);
        fprintf(stderr, "[loadjob] ioopen device=0x%X dir='%s' name='%s'%c", a, d, f, 10);
    } else if (tag == 4 || tag == 5) {
        static int m = 0; if (m++ >= 40) return;
        char r[256], q[256]; port_ld_str(a, r, 256); port_ld_str(b, q, 256);
        fprintf(stderr, "[loadjob] split root%s='%s' path='%s'%c", tag == 4 ? "+8" : "+C", r, q, 10);
    } else if (tag == 2) {
        fprintf(stderr, "[loadjob] ioopen fileslot=%d reqslot=%d%c", (int32_t)a, (int32_t)b, 10);
    } else {
        fprintf(stderr, "[loadjob] ioopen dev open -> %d (0x%X) slot=0x%X dev=0x%X%c", (int32_t)a, a, b, c, 10);
    }
}
extern "C" void port_ld_poll(uint32_t job, int)
{
    static ULONGLONG last = 0; static uint32_t lj = 0;
    const ULONGLONG now = GetTickCount64();
    if (job == lj && now - last < 1000) return;
    last = now; lj = job;
    fprintf(stderr, "[loadjob] poll job=0x%08X state=%d handle=%d +10=0x%X +14=0x%X +18=0x%X +120=0x%X%c", job,
            (int)vm_read32(job + 4), (int)vm_read32(job + 0x11C), vm_read32(job + 0x10), vm_read32(job + 0x14),
            vm_read32(job + 0x18), vm_read32(job + 0x120), 10);
}
extern "C" uint64_t port_movie_evwait_us(void)
{
    static long long us = -1;
    if (us < 0) { const char* e = getenv("PORT_MOVIE_EVWAIT_MS"); us = (e ? atoll(e) : 100) * 1000; if (us < 0) us = 0; }
    return (uint64_t)us;
}

/* Resistance 3 imports the shared libraries do not cover.
 * sys_prx_load_module(_on_memcontainer): the game asks for firmware modules by
 * path. Those under /dev_flash/ are HLE'd already, so hand back a module id
 * (from 0x23000000, as the firmware does); anything else is genuinely absent. */
static void hle_r3_prx_load(ppu_context* ctx)
{
    static uint32_t s_next = 0x23000000u;
    const std::string path = guest_str((uint32_t)ctx->gpr[3]);
    if (path.rfind("/dev_flash/", 0) == 0) ctx->gpr[3] = s_next++;
    else ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010006u;   /* ENOENT */
    fprintf(stderr, "[prx] load '%s' -> 0x%08X\n", path.c_str(), (uint32_t)ctx->gpr[3]);
}
/* cellKbRead(port, CellKbData*): no key events (keyboard goes through the
 * port's own input layer); len = 0 so the game never reads stale codes. */
static void hle_r3_kb_read(ppu_context* ctx)
{
    const uint32_t d = (uint32_t)ctx->gpr[4];
    if (d) { vm_base[d + 8] = vm_base[d + 9] = vm_base[d + 10] = vm_base[d + 11] = 0; }
    ctx->gpr[3] = 0;
}

extern "C" void ps3_load_prx_modules(void)
{
    ps3_hle_register_ctx(0xFC52A7A9u, "sys_game_process_exitspawn", hle_game_process_exitspawn);
    ps3_hle_register_ctx(0x26090058u, "sys_prx_load_module", hle_r3_prx_load);
    ps3_hle_register_ctx(0xAA6D9BFFu, "sys_prx_load_module_on_memcontainer", hle_r3_prx_load);
    ps3_hle_register_ctx(0xFF0A21B7u, "cellKbRead", hle_r3_kb_read);
    menu_probe_start();   /* v160b diag */
#ifdef _WIN32
    { static char dbgpath[1024];
      const char* dp = getenv("PS3_DEBUG");
      s_dbg_main_tid = GetCurrentThreadId();
      { const char* mc = getenv("PORT_MAIN_CORE");
        if (!mc || atoi(mc)) CreateThread(NULL, 0, main_core_keeper, NULL, 0, NULL); }
      if (dp && *dp) { snprintf(dbgpath, sizeof dbgpath, "%s", dp);
                       CreateThread(NULL, 0, debug_console, dbgpath, 0, NULL); } }
    if (getenv("WATCHDOG_AT")) CreateThread(NULL, 0, hang_watchdog, NULL, 0, NULL);
    if (getenv("FREEZE_RELAUNCH")) CreateThread(NULL, 0, freeze_relauncher, NULL, 0, NULL);
    CreateThread(NULL, 0, movie_watchdog, NULL, 0, NULL);   /* v126: logs\\moviehang_*.txt */
#endif
}

/* ---------------------------------------------------------------------------
 * Example: Override a specific NID
 *
 * The module system resolves function imports by NID.  To replace a default
 * HLE stub with custom logic, register your function at init time.
 * -----------------------------------------------------------------------*/

/*
 * Example: override cellGcmSetFlipMode (NID 0xA53D12AE) to force vsync.
 *
 *   static void my_cellGcmSetFlipMode(u32 mode)
 *   {
 *       printf("[hook] cellGcmSetFlipMode: forcing VSYNC (was %u)\n", mode);
 *       // Force vsync regardless of what the game requested
 *       cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
 *   }
 *
 * Then in your init code:
 *
 *   ps3::modules::override_nid(0xA53D12AE,
 *       reinterpret_cast<void*>(my_cellGcmSetFlipMode));
 */

/* ---------------------------------------------------------------------------
 * Example: Hook into module loading
 *
 * You can intercept cellSysmoduleLoadModule to perform custom actions
 * when a particular module is loaded.
 * -----------------------------------------------------------------------*/

/*
 *   static s32 my_loadmodule_hook(u16 id)
 *   {
 *       printf("[hook] Module 0x%04X loaded\n", id);
 *
 *       if (id == CELL_SYSMODULE_AUDIO) {
 *           // Perform custom audio backend initialization
 *           init_my_audio_backend();
 *       }
 *
 *       // Call the original implementation
 *       return cellSysmoduleLoadModule(id);
 *   }
 */

/* ---------------------------------------------------------------------------
 * Game-specific patches
 *
 * Sometimes you need to patch recompiled code at specific addresses.
 * Use the patch system to modify behavior without editing generated code.
 * -----------------------------------------------------------------------*/

/*
 *   // Skip an integrity check at guest address 0x00812FA0
 *   ps3::patches::nop_range(0x00812FA0, 0x00812FB0);
 *
 *   // Replace a function call with a direct return
 *   ps3::patches::force_return(0x009A1000, 0);  // return 0
 */
