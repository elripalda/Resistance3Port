/* Resistance port: crash and hang reports for play-testing (from the R2 port).
 *
 * A crash (an exception nobody handles, or a fatal code no handler can recover
 * from: stack overflow, illegal instruction, heap corruption, fast fail) writes
 * logs\crash\R3_<time>_crash.dmp and a one-line summary to logs\crash\crashes.txt,
 * then lets the process die as before. A hang (the game's frame loop,
 * r2_game_frames, counted in the stubs.cpp frame limiter, does not advance for
 * R2_HANG_S seconds, default 90 -- longer than any level load) writes
 * R3_<time>_hang.dmp once per stall and keeps running. The dumps hold thread
 * stacks and the memory they point at, not the 4 GB guest space, so they stay
 * small. R2_CRASH_DUMPS=0 turns both off; R2_CRASH_DUMPS=full (or the older
 * CRASH_DUMP_FULL=1) writes full memory dumps (several GB). */
#ifdef _WIN32
#include <dbghelp.h>
#include <time.h>

extern "C" volatile LONG r2_game_frames = 0;   /* stubs.cpp frame limiter */

static char s_crash_dir[MAX_PATH];
static int  s_crash_full = 0;
static volatile LONG s_crash_busy = 0;
typedef BOOL (WINAPI *r3_mdw_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
static r3_mdw_t s_mdw = NULL;

static void r2_crash_path(char* out, size_t n, const char* kind)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_s(&tmv, &t);
    snprintf(out, n, "%s\\R3_%04d%02d%02d_%02d%02d%02d_%s.dmp", s_crash_dir, tmv.tm_year + 1900,
             tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, kind);
}

static int r2_write_dump(const char* path, EXCEPTION_POINTERS* ep)
{
    if (!s_mdw) return 0;
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), ep, FALSE };
    const MINIDUMP_TYPE type = s_crash_full
        ? (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithThreadInfo | MiniDumpWithHandleData)
        : (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                          MiniDumpScanMemory | MiniDumpWithUnloadedModules);
    const BOOL ok = s_mdw(GetCurrentProcess(), GetCurrentProcessId(), f, type, ep ? &mei : NULL, NULL, NULL);
    CloseHandle(f);
    return ok != FALSE;
}

static void r2_crash_note(const char* line)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\crashes.txt", s_crash_dir);
    if (FILE* f = fopen(p, "a")) { fputs(line, f); fclose(f); }
    fputs(line, stderr);
    fflush(stderr);
}

static void r2_crash_report(EXCEPTION_POINTERS* ep, const char* how)
{
    if (InterlockedExchange(&s_crash_busy, 1)) return;
    fflush(stdout);
    char path[MAX_PATH];
    r2_crash_path(path, sizeof path, "crash");
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const ULONG_PTR addr = (ULONG_PTR)er->ExceptionAddress;
    HMODULE mod = NULL;
    char mname[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) && mod) {
        GetModuleFileNameA(mod, mname, sizeof mname);
    }
    const char* base = strrchr(mname, '\\');
    char fault[96] = "";
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const uintptr_t fa = er->ExceptionInformation[1];
        const char* op = er->ExceptionInformation[0] == 1 ? "writing" : er->ExceptionInformation[0] == 8 ? "executing" : "reading";
        if (vm_base && fa >= (uintptr_t)vm_base && fa < (uintptr_t)vm_base + VM_SIZE)
            snprintf(fault, sizeof fault, ", %s guest 0x%08X", op, (uint32_t)(fa - (uintptr_t)vm_base));
        else
            snprintf(fault, sizeof fault, ", %s 0x%llX", op, (unsigned long long)fa);
    }
    const int ok = r2_write_dump(path, ep);
    char line[1024];
    snprintf(line, sizeof line, "[crash] %s: code 0x%08lX at %s+0x%llX%s (thread %lu), frame %ld -> %s\n",
             how, er->ExceptionCode, base ? base + 1 : mname,
             (unsigned long long)(addr - (ULONG_PTR)mod), fault, GetCurrentThreadId(), r2_game_frames,
             ok ? path : "dump FAILED");
    r2_crash_note(line);
}

static LONG WINAPI r2_crash_filter(EXCEPTION_POINTERS* ep)
{
    r2_crash_report(ep, "unhandled exception");
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Faults that never reach the unhandled filter (stack overflow, and host code
 * that installs its own last-chance filter later) are caught here; only codes
 * no handler can recover from. */
static LONG WINAPI r2_crash_veh(EXCEPTION_POINTERS* ep)
{
    const DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == EXCEPTION_STACK_OVERFLOW || c == EXCEPTION_ILLEGAL_INSTRUCTION ||
        c == EXCEPTION_PRIV_INSTRUCTION || c == 0xC0000374 /* heap corruption */ || c == 0xC0000409 /* fast fail */)
        r2_crash_report(ep, "fatal exception");
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI r2_hang_watch(LPVOID)
{
    int limit_s = 90;
    if (const char* e = getenv("R2_HANG_S")) { const int v = atoi(e); if (v >= 10) limit_s = v; }
    LONG last = r2_game_frames;
    ULONGLONG since = GetTickCount64();
    int reported = 0;
    for (;;) {
        Sleep(1000);
        const LONG now = r2_game_frames;
        if (now != last) { last = now; since = GetTickCount64(); reported = 0; continue; }
        /* not counting until the game loop has started (boot, intro videos) */
        if (now == 0 || reported) continue;
        if (GetTickCount64() - since < (ULONGLONG)limit_s * 1000ull) continue;
        reported = 1;
        char path[MAX_PATH], line[1024];
        r2_crash_path(path, sizeof path, "hang");
        const int ok = r2_write_dump(path, NULL);
        snprintf(line, sizeof line, "[hang] no game frame for %d s (frame %ld) -> %s\n", limit_s, now,
                 ok ? path : "dump FAILED");
        r2_crash_note(line);
    }
    return 0;
}

static void r2_crash_init(void)
{
    const char* e = getenv("R2_CRASH_DUMPS");
    if (e && *e == '0') return;
    const char* full = getenv("CRASH_DUMP_FULL");
    s_crash_full = (e && !_stricmp(e, "full")) || (full && *full == '1');
    if (HMODULE dh = LoadLibraryA("dbghelp.dll")) s_mdw = (r3_mdw_t)GetProcAddress(dh, "MiniDumpWriteDump");
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    if (char* s = strrchr(exe, '\\')) *s = 0;
    snprintf(s_crash_dir, sizeof s_crash_dir, "%s\\..\\logs", exe);
    CreateDirectoryA(s_crash_dir, NULL);
    strncat(s_crash_dir, "\\crash", sizeof s_crash_dir - strlen(s_crash_dir) - 1);
    CreateDirectoryA(s_crash_dir, NULL);
    AddVectoredExceptionHandler(0, r2_crash_veh);
    SetUnhandledExceptionFilter(r2_crash_filter);
    CloseHandle(CreateThread(NULL, 64 * 1024, r2_hang_watch, NULL, 0, NULL));
}
#else
static void r2_crash_init(void) {}
#endif
