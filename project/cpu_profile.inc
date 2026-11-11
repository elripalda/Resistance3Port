/* Resistance port: CPU detection, CPU profile and soft thread placement.
 * Included by main.cpp; port_cpu_profile_init() runs first thing in main(),
 * before any subsystem starts or reads its switches.
 *
 * Detection: physical cores, logical CPUs, SMT and hybrid P/E cores
 * (GetLogicalProcessorInformationEx, EfficiencyClass), counted inside the
 * process affinity mask, so "start /affinity F" is seen as a 2-core CPU.
 *
 * Profile: PORT_CPU_PROFILE=auto (default) | low | mid | high | off.
 *   auto: low <= 4 physical cores, mid 5-7, high >= 8 (hybrid: P + E/2).
 *   The profile only sets the existing env switches the user has not set:
 *   low  GCM_VBLANK_SPIN_US=0 R2_PACE_SPIN_US=100 RSX_SPIN_US=50
 *        RSX_PERF_VK_SUBMIT_THREAD=0 RSX_VK_PIPELINE_THREADS=1 (2 with >= 8
 *        logical CPUs) PS3_PRIO_BOOST=0
 *   mid  GCM_VBLANK_SPIN_US=100 R2_PACE_SPIN_US=250
 *   high nothing (the shipped defaults).
 *   off: no switches and no placement -- exactly the old behaviour.
 *
 * Placement (PORT_THREAD_PLACEMENT=0 disables; off with PORT_CPU_PROFILE=off):
 *   hybrid CPUs with >= 4 P-cores: process default CPU sets = the P-cores, so
 *   the PPU, SPU, FIFO walker and RSX engine threads stay off the E-cores;
 *   background threads (monitors, pipeline compilers) get all CPUs back and a
 *   lower priority. With SMT, the main PPU thread, the RSX engine thread and
 *   the FIFO walker get ideal processors on different physical cores (a soft
 *   hint; the scheduler still moves them), unless main_core_keeper pins them
 *   (>= 12 logical CPUs or PORT_MAIN_CPU). */
#ifdef _WIN32
#include <intrin.h>
#include <tlhelp32.h>
#include <wchar.h>

enum { PORT_CPU_OFF = 0, PORT_CPU_LOW, PORT_CPU_MID, PORT_CPU_HIGH };
static int s_cpu_profile = PORT_CPU_OFF;
static int s_cpu_place = 0;

static struct {
    int sys_phys, sys_logical;      /* the whole CPU */
    int phys, logical;              /* inside the process affinity mask */
    int smt, hybrid, pcores, ecores;
    int ncore;
    DWORD_PTR core_mask[64];        /* available logical CPUs of each core, P-cores first */
    BYTE core_eff[64];
    DWORD_PTR pmask;                /* logical CPUs of available P-cores (all when not hybrid) */
    DWORD_PTR affinity;
} s_cpu;

static int port_popcount(DWORD_PTR m) { int n = 0; for (; m; m &= m - 1) n++; return n; }

static void port_cpu_detect(void)
{
    DWORD_PTR pm = 0, sm = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm) || !pm) pm = ~(DWORD_PTR)0;
    s_cpu.affinity = pm;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    BYTE* buf = len ? (BYTE*)malloc(len) : NULL;
    BYTE maxeff = 0;
    struct { DWORD_PTR m; BYTE eff; } cores[64]; int nc = 0;
    if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        for (DWORD off = 0; off < len;) {
            const PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            if (e->Relationship == RelationProcessorCore) {
                const GROUP_AFFINITY& g = e->Processor.GroupMask[0];
                s_cpu.sys_phys++;
                s_cpu.sys_logical += port_popcount(g.Mask);
                const DWORD_PTR m = g.Group == 0 ? (g.Mask & pm) : 0;
                if (m && nc < 64) {
                    cores[nc].m = m; cores[nc].eff = e->Processor.EfficiencyClass; nc++;
                    if (e->Processor.EfficiencyClass > maxeff) maxeff = e->Processor.EfficiencyClass;
                }
            }
            off += e->Size;
        }
    }
    free(buf);
    if (!nc) {   /* no topology: treat every logical CPU as a core */
        SYSTEM_INFO si; GetSystemInfo(&si);
        s_cpu.sys_phys = s_cpu.sys_logical = (int)si.dwNumberOfProcessors;
        for (int i = 0; i < 64 && nc < 64; i++)
            if (pm >> i & 1) { cores[nc].m = (DWORD_PTR)1 << i; cores[nc].eff = 0; nc++; }
    }
    BYTE mineff = 255;
    for (int i = 0; i < nc; i++) if (cores[i].eff < mineff) mineff = cores[i].eff;
    s_cpu.hybrid = nc && mineff != maxeff;
    /* P-cores first, in topology order */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nc; i++) {
            const int p = cores[i].eff == maxeff;
            if (pass == 0 ? !p : p) continue;
            s_cpu.core_mask[s_cpu.ncore] = cores[i].m; s_cpu.core_eff[s_cpu.ncore] = cores[i].eff; s_cpu.ncore++;
            if (p) { s_cpu.pcores++; s_cpu.pmask |= cores[i].m; } else s_cpu.ecores++;
            s_cpu.phys++;
            s_cpu.logical += port_popcount(cores[i].m);
            if (port_popcount(cores[i].m) > 1) s_cpu.smt = 1;
        }
    if (!s_cpu.hybrid) s_cpu.ecores = 0;
}

static void port_cpu_setdef(const char* name, const char* value, char* log, size_t cap)
{
    const char* cur = getenv(name);
    if (cur) { /* the user's (or settings.bat's) value wins */
        size_t n = strlen(log); snprintf(log + n, cap - n, " %s=%s(user)", name, cur); return; }
    _putenv_s(name, value);
    size_t n = strlen(log); snprintf(log + n, cap - n, " %s=%s", name, value);
}

/* PORT_THREAD_PLACEMENT=0 or PORT_CPU_PROFILE=off: no placement anywhere. */
extern "C" int port_thread_placement_on(void) { return s_cpu_place; }

/* CPU set IDs of the logical CPUs in mask (group 0, inside the process affinity). */
static ULONG port_cpu_set_ids(DWORD_PTR mask, ULONG* ids, ULONG cap)
{
    typedef BOOL (WINAPI *gscsi_t)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    gscsi_t g = (gscsi_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetSystemCpuSetInformation");
    ULONG need = 0, n = 0;
    if (g) g(NULL, 0, &need, GetCurrentProcess(), 0);
    BYTE* b = need ? (BYTE*)malloc(need) : NULL;
    if (b && g((PSYSTEM_CPU_SET_INFORMATION)b, need, &need, GetCurrentProcess(), 0))
        for (ULONG o = 0; o < need && n < cap;) {
            const PSYSTEM_CPU_SET_INFORMATION c = (PSYSTEM_CPU_SET_INFORMATION)(b + o);
            if (c->Type == CpuSetInformation && c->CpuSet.Group == 0 &&
                (mask & s_cpu.affinity) >> c->CpuSet.LogicalProcessorIndex & 1) ids[n++] = c->CpuSet.Id;
            o += c->Size;
        }
    free(b);
    return n;
}

/* CPU pass, task "P-cores": on Intel hybrid CPUs the threads the frame rate
 * hangs on -- main PPU, rawSPU0 (SPU job kernel), rsx::thread, the FIFO
 * walker / RSX engine -- get thread CPU sets = the P-cores. The process
 * default sets do the same only with >= 4 P-cores, and threads that later get
 * a background placement or an explicit affinity are not covered by them.
 * PORT_PCORE_HOT=0 off; default on with hybrid CPUs only (no-op elsewhere). */
static int s_pcore_hot = 0;
static ULONG s_pset_ids[256], s_pset_n = 0;
static DWORD s_place_main_tid = 0;
extern "C" unsigned long long port_cpu_pmask(void) { return s_cpu.hybrid ? (unsigned long long)s_cpu.pmask : 0; }
/* Lowest logical CPU of physical core #core (P-cores first, topology order,
 * inside the process affinity mask); -1 when there is no such core. */
extern "C" int port_cpu_core_first(int core)
{
    if (core < 0 || core >= s_cpu.ncore || !s_cpu.core_mask[core]) return -1;
    int c = 0; for (DWORD_PTR m = s_cpu.core_mask[core]; !(m & 1); m >>= 1) c++;
    return c;
}
static int port_place_pcore(HANDLE th)
{
    if (!s_pcore_hot || !s_pset_n) return 0;
    typedef BOOL (WINAPI *stscs_t)(HANDLE, const ULONG*, ULONG);
    static stscs_t stscs = (stscs_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadSelectedCpuSets");
    return stscs && stscs(th, s_pset_ids, s_pset_n);
}

/* Background threads: lower priority, and every CPU (E-cores included). */
extern "C" void port_place_background(HANDLE th)
{
    if (!s_cpu_place) return;
    SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
    if (s_cpu.hybrid) {
        typedef BOOL (WINAPI *stscs_t)(HANDLE, const ULONG*, ULONG);
        static stscs_t stscs = (stscs_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadSelectedCpuSets");
        static ULONG all[256]; static ULONG nall = 0; static int init = 0;
        if (!init) {
            init = 1;
            typedef BOOL (WINAPI *gscsi_t)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
            gscsi_t g = (gscsi_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetSystemCpuSetInformation");
            ULONG need = 0;
            if (g) g(NULL, 0, &need, GetCurrentProcess(), 0);
            BYTE* b = need ? (BYTE*)malloc(need) : NULL;
            if (b && g((PSYSTEM_CPU_SET_INFORMATION)b, need, &need, GetCurrentProcess(), 0))
                for (ULONG o = 0; o < need && nall < 256;) {
                    const PSYSTEM_CPU_SET_INFORMATION c = (PSYSTEM_CPU_SET_INFORMATION)(b + o);
                    if (c->Type == CpuSetInformation && c->CpuSet.Group == 0 &&
                        (s_cpu.affinity >> c->CpuSet.LogicalProcessorIndex & 1)) all[nall++] = c->CpuSet.Id;
                    o += c->Size;
                }
            free(b);
        }
        if (stscs && nall) stscs(th, all, nall);
    }
}

/* Main thread / FIFO walker / RSX engine: ideal processor on its own core. */
enum { PORT_ROLE_MAIN = 0, PORT_ROLE_ENGINE = 1, PORT_ROLE_WALKER = 2 };
static int s_place_ideal = 0;
static void port_place_hot(HANDLE th, int role)
{
    if (!s_cpu_place || !s_place_ideal || s_cpu.ncore < 2) return;
    /* hot cores: P-cores, skipping core 0 (most interrupts land there) when there are >= 4 */
    const int np = s_cpu.hybrid ? s_cpu.pcores : s_cpu.ncore;
    const int first = np >= 4 ? 1 : 0, avail = np - first;
    if (avail < 1) return;
    /* two cores: the walker shares the engine's (they run in lockstep), never main's */
    const int slot = (role == PORT_ROLE_WALKER && avail < 3) ? PORT_ROLE_ENGINE : role;
    const DWORD_PTR m = s_cpu.core_mask[first + slot % avail];
    PROCESSOR_NUMBER pn = { 0 };
    pn.Group = 0;
    for (BYTE i = 0; i < 64; i++) if (m >> i & 1) { pn.Number = i; break; }
    SetThreadIdealProcessorEx(th, &pn, NULL);
}

extern "C" unsigned long rsx_draw_engine_host_tid(void);
extern "C" unsigned long spu_raw_host_tid(unsigned n);
extern "C" unsigned long rsx_fifo_host_tid(void);
static DWORD s_place_walker_tid = 0;

/* Applies placement to threads that appear later: the RSX engine thread
 * (RSX_PERF_MT starts it on the first FIFO work) and background threads that
 * name themselves (pipeline compilers, monitors). Every 2 s while threads keep
 * appearing, then every PORT_KEEPER_MS (default 5000), below normal. */
static DWORD WINAPI port_place_keeper(LPVOID)
{
    SetThreadDescription(GetCurrentThread(), L"placement keeper");
    port_place_background(GetCurrentThread());
    typedef HRESULT (WINAPI *gtd_t)(HANDLE, PWSTR*);
    static gtd_t gtd = (gtd_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    static DWORD seen[1024]; int nseen = 0;
    DWORD eng_done = 0;
    DWORD hot_done[4] = { 0, 0, 0, 0 };
    const char* ke = getenv("PORT_KEEPER_MS");
    const DWORD idle_ms = ke && atoi(ke) >= 250 ? (DWORD)atoi(ke) : 5000;
    int quiet = 0;
    const DWORD pid = GetCurrentProcessId();
    for (;;) {
        int fresh = 0;
        const DWORD et = (DWORD)rsx_draw_engine_host_tid();
        if (et && et != eng_done && et != s_place_walker_tid) {
            HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, et);
            if (h) { port_place_hot(h, PORT_ROLE_ENGINE); CloseHandle(h); eng_done = et; fresh = 1; }
        }
        if (s_pcore_hot) {
            const DWORD hot[4] = { s_place_main_tid, (DWORD)spu_raw_host_tid(0), (DWORD)rsx_fifo_host_tid(), et };
            for (int i = 0; i < 4; i++)
                if (hot[i] && hot[i] != hot_done[i]) {
                    HANDLE h = OpenThread(THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, hot[i]);
                    if (h) { port_place_pcore(h); hot_done[i] = hot[i]; CloseHandle(h); fresh = 1; }
                }
        }
        HANDLE snap = gtd ? CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0) : INVALID_HANDLE_VALUE;
        THREADENTRY32 te; te.dwSize = sizeof te;
        if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid) continue;
                int k = 0; while (k < nseen && seen[k] != te.th32ThreadID) k++;
                if (k < nseen) continue;
                HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (!h) continue;
                PWSTR d = NULL;
                if (SUCCEEDED(gtd(h, &d)) && d && d[0]) {
                    /* named threads are named for good: decide once */
                    if (wcsstr(d, L"ompil") || wcsstr(d, L"shader") || wcsstr(d, L"monitor") ||
                        wcsstr(d, L"thrcpu") || wcsstr(d, L"affinity keeper") || wcsstr(d, L"watchdog"))
                        port_place_background(h);
                    else if (!wcscmp(d, L"rsx::thread") || !wcscmp(d, L"FIFO walker"))
                        port_place_pcore(h);
                    if (nseen < 1024) seen[nseen++] = te.th32ThreadID;
                    fresh = 1;
                }
                if (d) LocalFree(d);
                CloseHandle(h);
            } while (Thread32Next(snap, &te));
        }
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        quiet = fresh ? 0 : quiet + 1;
        Sleep(quiet >= 5 ? idle_ms : 2000);
    }
}

/* The FIFO walker (frame_clock) calls this from its own thread. */
static void port_place_walker(void)
{
    s_place_walker_tid = GetCurrentThreadId();
    SetThreadDescription(GetCurrentThread(), L"FIFO walker");
    port_place_hot(GetCurrentThread(), PORT_ROLE_WALKER);
    port_place_pcore(GetCurrentThread());
}

#ifdef PORT_PGO_GEN
/* PGO training build (PORT_PGO=gen): the counters live in memory and are
 * written to LLVM_PROFILE_FILE only by an orderly exit, which the game does
 * not always reach (window close -> TerminateProcess paths). Write them every
 * PORT_PGO_DUMP_S seconds (default 30) and at exit; each write replaces the
 * file with the counters so far. */
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);
static DWORD WINAPI port_pgo_dumper(LPVOID p)
{
    SetThreadDescription(GetCurrentThread(), L"pgo monitor");
    const DWORD ms = (DWORD)(uintptr_t)p * 1000;
    for (;;) { Sleep(ms); __llvm_profile_write_file(); }
}
static void port_pgo_atexit(void) { __llvm_profile_write_file(); }
static void port_pgo_start(void)
{
    /* the profile runtime read the variable before main(): set the name directly */
    if (!getenv("LLVM_PROFILE_FILE")) { _putenv_s("LLVM_PROFILE_FILE", "pgo\\game-%p.profraw");
                                        __llvm_profile_set_filename("pgo\\game-%p.profraw"); }
    CreateDirectoryA("pgo", NULL);
    const char* e = getenv("PORT_PGO_DUMP_S");
    const int s = e && atoi(e) > 0 ? atoi(e) : 30;
    atexit(port_pgo_atexit);
    HANDLE h = CreateThread(NULL, 0, port_pgo_dumper, (LPVOID)(uintptr_t)s, 0, NULL);
    if (h) CloseHandle(h);
    fprintf(stderr, "[pgo] instrumented build: profile -> %s every %d s and at exit\n", getenv("LLVM_PROFILE_FILE"), s);
}
#endif

static void port_cpu_profile_init(void)
{
#ifdef PORT_PGO_GEN
    port_pgo_start();
#endif
    port_cpu_detect();
    int brand_r[4]; char brand[49] = { 0 };
    __cpuid(brand_r, 0x80000000);
    if ((unsigned)brand_r[0] >= 0x80000004u)
        for (int i = 0; i < 3; i++) { __cpuid(brand_r, 0x80000002 + i); memcpy(brand + 16 * i, brand_r, 16); }
    const char* b = brand; while (*b == ' ') b++;

    const double eff_cores = s_cpu.hybrid ? s_cpu.pcores + s_cpu.ecores / 2.0 : (double)s_cpu.phys;
    const int autop = eff_cores <= 4.0 ? PORT_CPU_LOW : eff_cores < 8.0 ? PORT_CPU_MID : PORT_CPU_HIGH;
    const char* e = getenv("PORT_CPU_PROFILE");
    int prof = autop, forced = 0;
    if (e && *e && _stricmp(e, "auto")) {
        forced = 1;
        if (!_stricmp(e, "low")) prof = PORT_CPU_LOW;
        else if (!_stricmp(e, "mid")) prof = PORT_CPU_MID;
        else if (!_stricmp(e, "high")) prof = PORT_CPU_HIGH;
        else if (!_stricmp(e, "off") || !strcmp(e, "0")) prof = PORT_CPU_OFF;
        else forced = 0;
    }
    s_cpu_profile = prof;
    static const char* const pn[] = { "off", "low", "mid", "high" };
    fprintf(stderr, "[cpu] %s: %d cores / %d threads%s%s | available to the process: %d cores / %d threads "
                    "(affinity 0x%llX)%s, %d P + %d E | profile %s (%s)\n",
            *b ? b : "unknown CPU", s_cpu.sys_phys, s_cpu.sys_logical, s_cpu.smt ? " SMT" : "",
            s_cpu.hybrid ? " hybrid" : "", s_cpu.phys, s_cpu.logical, (unsigned long long)s_cpu.affinity,
            s_cpu.smt ? " SMT" : " no SMT", s_cpu.hybrid ? s_cpu.pcores : s_cpu.phys, s_cpu.ecores,
            pn[prof], forced ? "PORT_CPU_PROFILE" : "auto");
    if (prof == PORT_CPU_OFF) return;

    char log[1024] = { 0 };
    if (prof == PORT_CPU_LOW) {
        port_cpu_setdef("GCM_VBLANK_SPIN_US", "0", log, sizeof log);
        port_cpu_setdef("R2_PACE_SPIN_US", "100", log, sizeof log);
        port_cpu_setdef("RSX_SPIN_US", "50", log, sizeof log);
        port_cpu_setdef("RSX_PERF_VK_SUBMIT_THREAD", "0", log, sizeof log);
        port_cpu_setdef("RSX_VK_PIPELINE_THREADS", s_cpu.logical >= 8 ? "2" : "1", log, sizeof log);
        port_cpu_setdef("PS3_PRIO_BOOST", "0", log, sizeof log);
    } else if (prof == PORT_CPU_MID) {
        port_cpu_setdef("GCM_VBLANK_SPIN_US", "100", log, sizeof log);
        port_cpu_setdef("R2_PACE_SPIN_US", "250", log, sizeof log);
    }
    if (log[0]) fprintf(stderr, "[cpu] profile %s sets:%s\n", pn[prof], log);

    const char* pl = getenv("PORT_THREAD_PLACEMENT");
    s_cpu_place = !(pl && *pl && atoi(pl) == 0);
    if (!s_cpu_place) { fprintf(stderr, "[cpu] thread placement off (PORT_THREAD_PLACEMENT=0)\n"); return; }

    char plog[512] = { 0 };
    if (s_cpu.hybrid && s_cpu.pcores >= 4) {
        typedef BOOL (WINAPI *gscsi_t)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
        typedef BOOL (WINAPI *spdcs_t)(HANDLE, const ULONG*, ULONG);
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        gscsi_t g = (gscsi_t)GetProcAddress(k32, "GetSystemCpuSetInformation");
        spdcs_t s = (spdcs_t)GetProcAddress(k32, "SetProcessDefaultCpuSets");
        ULONG need = 0, ids[256], n = 0;
        if (g && s) g(NULL, 0, &need, GetCurrentProcess(), 0);
        BYTE* buf = need ? (BYTE*)malloc(need) : NULL;
        if (buf && g((PSYSTEM_CPU_SET_INFORMATION)buf, need, &need, GetCurrentProcess(), 0))
            for (ULONG o = 0; o < need && n < 256;) {
                const PSYSTEM_CPU_SET_INFORMATION c = (PSYSTEM_CPU_SET_INFORMATION)(buf + o);
                if (c->Type == CpuSetInformation && c->CpuSet.Group == 0 &&
                    (s_cpu.pmask >> c->CpuSet.LogicalProcessorIndex & 1)) ids[n++] = c->CpuSet.Id;
                o += c->Size;
            }
        free(buf);
        if (n && s(GetCurrentProcess(), ids, n))
            snprintf(plog + strlen(plog), sizeof plog - strlen(plog), " game threads on the %d P-cores (%lu CPU sets);",
                     s_cpu.pcores, (unsigned long)n);
    }
    /* main_core_keeper (debugtools.inc) hard-pins main and the FIFO thread
     * with >= 12 logical CPUs or PORT_MAIN_CPU; leave those to it. */
    s_place_main_tid = GetCurrentThreadId();
    {
        const char* ph = getenv("PORT_PCORE_HOT");
        if (s_cpu.hybrid && s_cpu.pcores >= 1 && !(ph && *ph && atoi(ph) == 0)) {
            s_pset_n = port_cpu_set_ids(s_cpu.pmask, s_pset_ids, 256);
            s_pcore_hot = s_pset_n > 0;
            if (s_pcore_hot && port_place_pcore(GetCurrentThread()))
                snprintf(plog + strlen(plog), sizeof plog - strlen(plog),
                         " main / rawSPU0 / rsx::thread / FIFO threads on the P-cores (thread CPU sets);");
        }
    }
    const char* mc = getenv("PORT_MAIN_CORE");
    const int keeper_pins = !(mc && atoi(mc) == 0) && (getenv("PORT_MAIN_CPU") || s_cpu.logical >= 12);
    s_place_ideal = !keeper_pins && s_cpu.ncore >= 2;
    if (s_place_ideal) {
        port_place_hot(GetCurrentThread(), PORT_ROLE_MAIN);
        snprintf(plog + strlen(plog), sizeof plog - strlen(plog),
                 " main / RSX engine / FIFO walker ideal CPUs on separate cores;");
    }
    HANDLE kh = CreateThread(NULL, 0, port_place_keeper, NULL, 0, NULL);
    if (kh) CloseHandle(kh);
    fprintf(stderr, "[cpu] thread placement:%s background threads below normal\n", plog);
}
#else
static void port_cpu_profile_init(void) {}
static void port_place_walker(void) {}
extern "C" unsigned long long port_cpu_pmask(void) { return 0; }
extern "C" int port_cpu_core_first(int core) { (void)core; return -1; }
extern "C" int port_thread_placement_on(void) { return 0; }
#endif
