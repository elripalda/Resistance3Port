/* Test for the tester crash / problem reporter: port_crash.h (crash and hang
 * capture in the game exe) and port_report.cpp (the "Report a problem" zip).
 *
 * Build (from a VS x64 prompt, clang-cl on PATH), against one port's project\:
 *   clang-cl /nologo /O1 /Zi /EHsc /std:c++17 /I <port>\project crash_report_test.cpp /Fe:crash_report_test.exe /link /DEBUG
 * Run:  crash_report_test.exe <work folder> [--quick]
 * Every crash case runs in a child process (a copy of this exe in its own fake
 * game folder <work>\<case>\bin, so the reporter writes <work>\<case>\logs\crash)
 * whose stdout is <work>\<case>\logs\run.log, like run.bat does. Prints one
 * PASS/FAIL line per check and "N/M passed"; exit code 0 = all passed.
 * --quick skips the hang-watchdog cases (about a minute). */
#include "port_crash.h"
#include "port_report.cpp"
#include <dbghelp.h>
#include <process.h>
#pragma comment(lib, "dbghelp.lib")

/* ------------------------------------------------------------------ child */
static volatile LONG g_frames, g_rsx;
static volatile int g_paused;
static uint8_t* g_vm;
static const uint64_t VM_TEST_SIZE = 1 << 20;

static uint64_t t_frames() { return (uint64_t)g_frames; }
static uint64_t t_rsx() { return (uint64_t)g_rsx; }
static int t_paused() { return g_paused; }
static void t_before() { printf("before_report called\n"); fflush(stdout); }

static void nop_impl(int) {}
static void (*volatile g_nop)(int) = nop_impl;
extern "C" __declspec(noinline) int lifted_func_82001234(int* p)
{
    g_nop(1);          /* not a leaf: has unwind data, like the lifted functions */
    *p = 42;
    g_nop(2);
    return *p;
}
/* the "function map": lifted_func_82001234 is the only registered function */
static uint32_t t_resolve(void* pc)
{
    const uintptr_t a = (uintptr_t)pc, f = (uintptr_t)&lifted_func_82001234;
    return a >= f && a < f + 0x10000 ? 0x82001234u : 0u;
}
extern "C" __declspec(noinline) int caller_b(int* p) { g_nop(3); return lifted_func_82001234(p) + 1; }
extern "C" __declspec(noinline) int caller_a(int* p) { g_nop(4); return caller_b(p) + 1; }

static __declspec(noinline) int recurse(int n)
{
    volatile char buf[4096];
    buf[0] = (char)n; buf[4095] = (char)n;
    return recurse(n + 1) + buf[0] + buf[4095];
}

static LONG WINAPI t_commit_veh(EXCEPTION_POINTERS* ep)   /* like main.cpp vm_commit_veh */
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t a = er->ExceptionInformation[1];
    if (a < (uintptr_t)g_vm || a >= (uintptr_t)g_vm + VM_TEST_SIZE) return EXCEPTION_CONTINUE_SEARCH;
    VirtualAlloc((void*)(a & ~(uintptr_t)0xFFF), 0x1000, MEM_COMMIT, PAGE_READWRITE);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static LPTOP_LEVEL_EXCEPTION_FILTER s_after_prev;
static LONG WINAPI t_before_filter(EXCEPTION_POINTERS*)
{
    if (FILE* f = fopen("..\\chain_before.txt", "w")) { fputs("called", f); fclose(f); }
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI t_after_filter(EXCEPTION_POINTERS* ep)   /* like rsx_null_backend.c nb_refresh_crash */
{
    if (FILE* f = fopen("..\\chain_after.txt", "w")) { fputs("called", f); fclose(f); }
    return s_after_prev ? s_after_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static unsigned __stdcall worker_crash(void*)
{
    typedef HRESULT (WINAPI* std_t)(HANDLE, PCWSTR);
    if (auto f = (std_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription")) f(GetCurrentThread(), L"RSX test worker");
    Sleep(100);
    caller_a(NULL);
    return 0;
}
static unsigned __stdcall racer(void*)
{
    Sleep(200);
    *(volatile int*)16 = 1;
    return 0;
}
static unsigned __stdcall ticker(void* arg)
{
    const int mode = (int)(intptr_t)arg;   /* 0 = frames stop after 2 s, 1 = rsx stops after 2 s, 2 = both run */
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        Sleep(50);
        const bool early = GetTickCount64() - t0 < 2000;
        if (mode != 0 || early) InterlockedIncrement(&g_frames);
        if (mode != 1 || early) InterlockedIncrement(&g_rsx);
    }
}

/* clang-cl only catches faults in a callee under /EHsc */
static __declspec(noinline) void poke_low() { *(volatile int*)8 = 1; }

static int child(const char* mode)
{
    printf("child-log-marker: mode %s\n", mode);
    for (int i = 0; i < 200; i++) printf("filler log line %d\n", i);
    printf("child-log-last-line\n");
    fflush(stdout);
    g_vm = (uint8_t*)VirtualAlloc(NULL, VM_TEST_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    if (!strcmp(mode, "chain")) SetUnhandledExceptionFilter(t_before_filter);
    PortCrashConfig c = {};
    c.tag = "TEST"; c.title = "Crash test"; c.env_prefix = "TESTPFX";
    c.main_progress = t_frames; c.main_start_min = 1;
    c.rsx_progress = t_rsx; c.paused = t_paused;
    c.vm_base = &g_vm; c.vm_size = VM_TEST_SIZE;
    c.guest_resolve = t_resolve; c.before_report = t_before;
    port_crash::init(c);
    if (!strcmp(mode, "chain")) s_after_prev = SetUnhandledExceptionFilter(t_after_filter);
    void* commit_veh = AddVectoredExceptionHandler(1, t_commit_veh);   /* registered after the reporter, first in line -- as in the ports */

    if (!strcmp(mode, "av") || !strcmp(mode, "chain")) return caller_a(NULL);
    if (!strcmp(mode, "guestav")) { RemoveVectoredExceptionHandler(commit_veh); g_vm[0x1234] = 1; return 0; }
    if (!strcmp(mode, "guestfault")) {   /* handled by the runtime: no report, the program goes on */
        g_vm[0x2000] = 7; g_vm[0x9008] = 9;
        printf("survived guest faults: %d %d\n", g_vm[0x2000], g_vm[0x9008]); fflush(stdout);
        __try { poke_low(); } __except (EXCEPTION_EXECUTE_HANDLER) { printf("SEH-handled fault ok\n"); }
        fflush(stdout);
        return 0;
    }
    if (!strcmp(mode, "stackoverflow")) return recurse(0);
    if (!strcmp(mode, "abort")) { abort(); }
    if (!strcmp(mode, "invparam")) { char* d = NULL; strcpy_s(d, 0, "x"); return 0; }
    if (!strcmp(mode, "worker")) {
        HANDLE h = (HANDLE)_beginthreadex(NULL, 0, worker_crash, NULL, 0, NULL);
        WaitForSingleObject(h, INFINITE);
        return 0;
    }
    if (!strcmp(mode, "race")) {
        HANDLE h[4];
        for (int i = 0; i < 4; i++) h[i] = (HANDLE)_beginthreadex(NULL, 0, racer, NULL, 0, NULL);
        WaitForMultipleObjects(4, h, TRUE, INFINITE);
        return 0;
    }
    if (!strcmp(mode, "hang") || !strcmp(mode, "rsxhang") || !strcmp(mode, "paused") || !strcmp(mode, "nohang")) {
        const int m = !strcmp(mode, "rsxhang") ? 1 : !strcmp(mode, "nohang") ? 2 : 0;
        CloseHandle((HANDLE)_beginthreadex(NULL, 0, ticker, (void*)(intptr_t)m, 0, NULL));
        if (!strcmp(mode, "paused")) { Sleep(1500); g_paused = 1; }
        Sleep(11000);
        printf("hang child done\n"); fflush(stdout);
        return 0;
    }
    return 99;
}

/* ----------------------------------------------------------------- parent */
static int g_pass, g_fail;
static void check(bool ok, const std::string& what, const std::string& detail = std::string())
{
    (ok ? g_pass : g_fail)++;
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : " -- ", detail.c_str());
    fflush(stdout);
}
static std::string slurp(const std::string& p) { std::string s; port_report::read_file(p, s); return s; }
static bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

struct Run { std::string root; DWORD code; std::vector<port_report::DumpFile> dumps, txts; std::string txt, log, crashes; };

static Run run_child(const std::string& work, const char* mode, const char* env_extra = NULL, const char* tag = NULL)
{
    Run r;
    r.root = work + "\\" + (tag ? tag : mode);
    port_report::remove_tree(r.root);
    port_report::make_dirs(r.root + "\\bin");
    port_report::make_dirs(r.root + "\\logs");
    char self[MAX_PATH]; GetModuleFileNameA(NULL, self, sizeof self);
    const std::string exe = r.root + "\\bin\\crash_report_test.exe";
    CopyFileA(self, exe.c_str(), FALSE);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE log = CreateFileA((r.root + "\\logs\\run.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    /* environment: ours without the switches under test, plus the case's */
    std::string env;
    char* block = GetEnvironmentStringsA();
    for (const char* e = block; *e; e += strlen(e) + 1)
        if (_strnicmp(e, "PORT_", 5) && _strnicmp(e, "TESTPFX_", 8)) env.append(e, strlen(e) + 1);
    FreeEnvironmentStringsA(block);
    env.append("PORT_TEST_MARK=hello"); env.push_back(0);
    if (env_extra) for (const char* e = env_extra; *e; e += strlen(e) + 1) env.append(e, strlen(e) + 1);
    env.push_back(0);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = log; si.hStdError = log; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    std::string cmd = "\"" + exe + "\" child " + mode;
    r.code = 0xFFFFFFFF;
    if (CreateProcessA(NULL, &cmd[0], NULL, NULL, TRUE, 0, (void*)env.data(), (r.root + "\\bin").c_str(), &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, 90000) == WAIT_TIMEOUT) { TerminateProcess(pi.hProcess, 0xDEAD); WaitForSingleObject(pi.hProcess, 5000); }
        GetExitCodeProcess(pi.hProcess, &r.code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    CloseHandle(log);
    r.dumps = port_report::find_files(r.root + "\\logs\\crash", "*.dmp");
    r.txts = port_report::find_files(r.root + "\\logs\\crash", "TEST_*.txt");
    if (!r.txts.empty()) r.txt = slurp(r.txts[0].path);
    r.log = slurp(r.root + "\\logs\\run.log");
    r.crashes = slurp(r.root + "\\logs\\crash\\crashes.txt");
    return r;
}

/* the minidump: readable, has the exception (crash) / thread list, small */
static void check_dump(const Run& r, const std::string& name, bool want_exception, DWORD want_code = 0)
{
    if (r.dumps.empty()) { check(false, name + ": minidump written"); return; }
    const std::string& p = r.dumps[0].path;
    HANDLE f = CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz; sz.QuadPart = 0; GetFileSizeEx(f, &sz);
    HANDLE m = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
    void* base = m ? MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : NULL;
    bool threads = false, exc = false;
    DWORD code = 0;
    ULONG n = 0;
    if (base) {
        PMINIDUMP_DIRECTORY d; void* s;
        if (MiniDumpReadDumpStream(base, ThreadListStream, &d, &s, &n)) threads = ((MINIDUMP_THREAD_LIST*)s)->NumberOfThreads > 0;
        if (MiniDumpReadDumpStream(base, ExceptionStream, &d, &s, &n)) { exc = true; code = ((MINIDUMP_EXCEPTION_STREAM*)s)->ExceptionRecord.ExceptionCode; }
        UnmapViewOfFile(base);
    }
    if (m) CloseHandle(m);
    CloseHandle(f);
    check(threads && sz.QuadPart > 0 && sz.QuadPart < (50ll << 20), name + ": minidump readable, has threads, small",
          port_report::fmt("%.1f MB", sz.QuadPart / 1048576.0));
    if (want_exception)
        check(exc && (!want_code || code == want_code), name + ": minidump has the exception", port_report::fmt("0x%08lX", code));
    else
        check(!exc, name + ": hang dump has no exception record");
}

static void check_crash_common(const Run& r, const std::string& name, const char* code_text)
{
    check(r.dumps.size() == 1, name + ": exactly one dump", port_report::fmt("%zu", r.dumps.size()));
    check(!r.txt.empty(), name + ": crash .txt next to the dump");
    if (!r.dumps.empty() && !r.txts.empty())
        check(r.dumps[0].name.substr(0, r.dumps[0].name.size() - 4) == r.txts[0].name.substr(0, r.txts[0].name.size() - 4) &&
              has(r.dumps[0].name, "_crash.dmp") && !r.dumps[0].name.compare(0, 5, "TEST_"),
              name + ": file names TEST_<time>_crash.dmp/.txt", r.dumps[0].name);
    check(has(r.txt, code_text), name + ": code in the report", code_text);
    check(has(r.txt, "address:    0x") && (has(r.txt, "crash_report_test.exe+0x") || has(r.txt, ".dll+0x")),
          name + ": faulting module + offset");
    check(has(r.txt, "==== threads") && has(r.txt, "<== CRASHED HERE") && has(r.txt, "#0 ") && has(r.txt, "#1 "),
          name + ": stack trace of the threads");
    check(has(r.txt, "child-log-last-line") && has(r.txt, "==== last 80 lines of"), name + ": run.log tail");
    check(has(r.txt, "PORT_TEST_MARK=hello") && has(r.txt, "system:") && has(r.txt, "exe build:"), name + ": env / system / exe summary");
    check(has(r.crashes, "[crash]") && has(r.crashes, " -> "), name + ": crashes.txt line");
    check(has(r.log, "before_report called"), name + ": before_report hook ran");
    check(r.code != 0 && r.code != 0xDEAD && r.code != 0xFFFFFFFF, name + ": process still dies", port_report::fmt("exit 0x%08lX", r.code));
}

static void touch(const std::string& p, size_t bytes, char fill = 'x')
{
    port_report::make_dirs(p.substr(0, p.rfind('\\')));
    std::string d(bytes, fill);
    for (size_t i = 99; i < bytes; i += 100) d[i] = '\n';
    port_report::write_file(p, d);
}
static std::string run_tar(const std::wstring& args)
{
    std::string out;
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" " + args;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr; CreatePipe(&rd, &wr, &sa, 0); SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr;
    PROCESS_INFORMATION pi;
    std::vector<wchar_t> cl(cmd.begin(), cmd.end()); cl.push_back(0);
    if (CreateProcessW(NULL, cl.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(wr);
        char b[4096]; DWORD n;
        while (ReadFile(rd, b, sizeof b, &n, NULL) && n) out.append(b, n);
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    } else CloseHandle(wr);
    CloseHandle(rd);
    return out;
}
static std::string zip_list(const std::wstring& zip)
{
    std::string out = run_tar(L"-tf \"" + zip + L"\"");
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}
static std::string zip_read(const std::wstring& zip, const char* member, const std::string& scratch)
{
    port_report::remove_tree(scratch);
    port_report::make_dirs(scratch);
    run_tar(L"-xf \"" + zip + L"\" -C \"" + port_report::widen(scratch) + L"\" " + port_report::widen(member));
    std::string m = member; std::replace(m.begin(), m.end(), '/', '\\');
    return slurp(scratch + "\\" + m);
}

static void bundle_tests(const std::string& work, const Run& crashed)
{
    SetEnvironmentVariableA("PS3_GAMEDATA_ROOT", NULL);
    const std::string root = work + "\\bundle";
    port_report::remove_tree(root);
    char prof[MAX_PATH] = ""; GetEnvironmentVariableA("USERPROFILE", prof, sizeof prof);
    touch(root + "\\run.bat", 300);
    touch(root + "\\build\\Resistance3.exe", 4096);
    touch(root + "\\launcher\\x.txt", 10);
    port_report::write_file(root + "\\settings.ini", "fps=60\r\n");
    port_report::write_file(root + "\\settings.bat", "set R2_FPS=60\r\n");
    port_report::write_file(root + "\\rpcs3_config.yml", std::string("Core:\r\n  Cache path: ") + prof + "\\AppData\\cache\r\n");
    port_report::write_file(root + "\\VERSION", "9.9.9\n");
    touch(root + "\\logs\\run.log", 10 << 20);
    port_report::write_file(root + "\\logs\\out.log", "out log line\r\n");
    touch(root + "\\logs\\build_unrelated.log", 2 << 20);
    touch(root + "\\shadercache\\pipelines.bin", 1 << 20);
    touch(root + "\\gamedata\\dev_hdd0\\game\\BCUS98232\\USRDIR\\big.psarc", 1 << 20);
    touch(root + "\\gamedata\\dev_hdd0\\home\\00000001\\savedata\\BCUS98232_SAVE\\SAVE.DAT", 300 << 10);
    port_report::make_dirs(root + "\\logs\\crash");
    if (!crashed.dumps.empty() && !crashed.txts.empty()) {
        CopyFileA(crashed.dumps[0].path.c_str(), (root + "\\logs\\crash\\" + crashed.dumps[0].name).c_str(), FALSE);
        CopyFileA(crashed.txts[0].path.c_str(), (root + "\\logs\\crash\\" + crashed.txts[0].name).c_str(), FALSE);
        CopyFileA((crashed.root + "\\logs\\crash\\crashes.txt").c_str(), (root + "\\logs\\crash\\crashes.txt").c_str(), FALSE);
    }
    const std::string out = work + "\\bundle_out";
    port_report::remove_tree(out);
    port_report::make_dirs(out);

    check(port_report::find_root((root + "\\launcher").c_str()) == root, "bundle: game folder found from a subfolder");
    check(port_report::detect_game(root) == "Resistance3", "bundle: game detected from build\\*.exe");

    port_report::Options o;
    o.root = root; o.out_dir = port_report::widen(out); o.note = "fell through the floor in chapter 2";
    port_report::Result r;
    const bool ok = port_report::create(o, r);
    const std::string name = port_report::narrow(r.zip);
    check(ok && GetFileAttributesW(r.zip.c_str()) != INVALID_FILE_ATTRIBUTES, "bundle: zip written", name + " " + r.error);
    check(has(name, "\\Resistance3_report_") && has(name, ".zip"), "bundle: named <Game>_report_<time>.zip");
    check(r.compressed, "bundle: compressed with tar.exe");
    const std::string ls = zip_list(r.zip);
    check(has(ls, "report.txt") && has(ls, "note.txt") && has(ls, "logs/run.log") && has(ls, "logs/out.log") &&
          has(ls, "settings/settings.ini") && has(ls, "settings/settings.bat") && has(ls, "settings/rpcs3_config.yml") &&
          has(ls, "settings/run.bat") && has(ls, "crash/crashes.txt") && has(ls, "_crash.dmp") && has(ls, "_crash.txt"),
          "bundle: has report, note, logs, settings, dump + crash.txt", ls);
    check(!has(ls, "saves/") && !has(ls, "shadercache") && !has(ls, "psarc") && !has(ls, "build_unrelated") && !has(ls, ".exe"),
          "bundle: no saves (unticked), shader cache, game data, other logs, exes");
    const std::string rep = zip_read(r.zip, "report.txt", work + "\\x");
    check(has(rep, "Windows:") && has(rep, "CPU:") && has(rep, "RAM:") && has(rep, "Version:   9.9.9") && has(rep, "Exe:       build\\Resistance3.exe"),
          "bundle: report.txt has OS / CPU / RAM / version / exe");
    check(has(rep, "GPU 0:") && has(rep, "driver"), "bundle: report.txt has GPU + driver (DXGI)");
    check(has(rep, "Vulkan"), "bundle: report.txt has the Vulkan line");
    check(has(rep, "last crash/hang: ") && has(rep, "[crash]"), "bundle: report.txt has the last crash line");
    const std::string note = zip_read(r.zip, "note.txt", work + "\\x");
    check(note == "fell through the floor in chapter 2", "bundle: note.txt is the tester's text");
    const std::string yml = zip_read(r.zip, "settings/rpcs3_config.yml", work + "\\x");
    check(!yml.empty() && (!prof[0] || !has(yml, prof)) && has(yml, "%USERPROFILE%"), "bundle: profile path scrubbed from text files");
    const std::string runlog = zip_read(r.zip, "logs/run.log", work + "\\x");
    check(runlog.size() <= (8u << 20) + 200 && runlog.size() > (7u << 20) && has(runlog, "left out"), "bundle: big log cut to its last 8 MB",
          port_report::fmt("%zu bytes", runlog.size()));
    check(r.packed_bytes <= (50ull << 20), "bundle: under the 50 MB cap", port_report::fmt("%.1f MB", r.packed_bytes / 1048576.0));
    check(port_report::find_files(root + "\\logs", "report_staging_*").empty() && !port_report::is_dir(root + "\\logs\\report_staging_x"),
          "bundle: staging folder removed");

    o.include_saves = true; o.note.clear();
    Sleep(1100);   /* a new time stamp */
    port_report::Result r2;
    port_report::create(o, r2);
    const std::string ls2 = zip_list(r2.zip);
    check(has(ls2, "saves/BCUS98232_SAVE/SAVE.DAT") && !has(ls2, "psarc") && !has(ls2, "note.txt"), "bundle: saves included when ticked (only savedata)");

    /* the cap: a 60 MB dump does not fit, everything else still goes in */
    touch(root + "\\logs\\crash\\TEST_29990101_000000_crash.dmp", 60 << 20, 'd');
    o.include_saves = false;
    Sleep(1100);
    port_report::Result r3;
    port_report::create(o, r3);
    const std::string ls3 = zip_list(r3.zip);
    bool listed = false;
    for (auto& s : r3.skipped) listed |= has(s, "TEST_29990101_000000_crash.dmp");
    const std::string rep3 = zip_read(r3.zip, "report.txt", work + "\\x");
    check(listed && !has(ls3, "29990101_000000_crash.dmp") && has(ls3, "logs/run.log") && has(rep3, "left out:"),
          "bundle: a dump over the 50 MB cap is left out and listed");
    DeleteFileA((root + "\\logs\\crash\\TEST_29990101_000000_crash.dmp").c_str());

    /* a small cap: the run.log tail (8 MB) no longer fits */
    o.cap_bytes = 3 << 20;
    Sleep(1100);
    port_report::Result r4;
    port_report::create(o, r4);
    const std::string ls4 = zip_list(r4.zip);
    check(r4.packed_bytes <= (3u << 20) && !has(ls4, "logs/run.log") && has(ls4, "report.txt") && has(ls4, "settings/settings.ini"),
          "bundle: the cap is a hard total", port_report::fmt("%.2f MB", r4.packed_bytes / 1048576.0));

    /* the stored-zip fallback (no tar.exe) */
    const std::string st = work + "\\stored_src";
    port_report::remove_tree(st);
    port_report::make_dirs(st + "\\sub");
    port_report::write_file(st + "\\a.txt", "hello stored zip");
    touch(st + "\\sub\\b.bin", 100000, 'b');
    const std::wstring sz = port_report::widen(work + "\\stored.zip");
    const bool sok = port_report::stored_zip(st, sz);
    const std::string sls = zip_list(sz);
    check(sok && has(sls, "a.txt") && has(sls, "sub/b.bin") && zip_read(sz, "a.txt", work + "\\x") == "hello stored zip" &&
          zip_read(sz, "sub/b.bin", work + "\\x").size() == 100000, "bundle: stored-zip fallback readable");

    /* the dialog opens and closes (no input); Cancel does not acknowledge a pending crash */
    struct Closer { static unsigned __stdcall run(void*) {
        for (int i = 0; i < 100; i++) { Sleep(100); if (HWND h = FindWindowA("ResistanceReportProblem", NULL)) { Sleep(300); PostMessageA(h, WM_CLOSE, 0, 0); return 1; } }
        return 0; } };
    port_report_ui_impl::s_root = root;
    DeleteFileA((root + "\\logs\\crash\\.seen").c_str());
    HANDLE th = (HANDLE)_beginthreadex(NULL, 0, Closer::run, NULL, 0, NULL);
    port_report_ui(NULL);
    DWORD found = 0;
    WaitForSingleObject(th, 15000); GetExitCodeThread(th, &found); CloseHandle(th);
    check(found == 1 && !port_report_ui_impl::s_win, "ui: report dialog opens and closes");
    bool crash = false;
    check(port_report_ui_impl::unseen(&crash) != 0 && crash, "ui: cancelled report leaves the crash prompt pending");
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc >= 3 && !strcmp(argv[1], "child")) {
        _set_abort_behavior(0, _WRITE_ABORT_MSG);   /* no "abort() has been called" box */
        return child(argv[2]);
    }
    std::string work = argc >= 2 ? argv[1] : "crash_report_work";
    { char full[MAX_PATH]; GetFullPathNameA(work.c_str(), MAX_PATH, full, NULL); work = full; }
    port_report::make_dirs(work);
    const bool quick = argc >= 3 && !strcmp(argv[2], "--quick");

    Run av = run_child(work, "av");
    check_crash_common(av, "av", "0xC0000005 (access violation)");
    check(has(av.txt, "guest fn:   func_82001234"), "av: lifted guest function resolved");
    check(has(av.txt, "[func_82001234+0x"), "av: guest function in the stack trace");
    check(has(av.txt, "writing 0x0"), "av: fault address");
    check(has(av.crashes, "in func_82001234"), "av: crashes.txt names the guest function");
    check(has(av.txt, "exe pdb:    "), "av: PDB identity recorded");
    check_dump(av, "av", true, EXCEPTION_ACCESS_VIOLATION);

    Run gav = run_child(work, "guestav");
    check(has(gav.txt, "writing guest 0x00001234"), "guestav: AV shown as a guest address");
    check(gav.dumps.size() == 1 && gav.code == EXCEPTION_ACCESS_VIOLATION, "guestav: reported, process dies", port_report::fmt("0x%08lX", gav.code));

    Run gf = run_child(work, "guestfault");
    check(gf.code == 0 && has(gf.log, "survived guest faults: 7 9") && has(gf.log, "SEH-handled fault ok"),
          "guestfault: runtime VEH + SEH faults still handled", port_report::fmt("exit %lu", gf.code));
    check(gf.dumps.empty() && gf.txts.empty() && gf.crashes.empty(), "guestfault: no report for handled faults");

    Run ch = run_child(work, "chain");
    check_crash_common(ch, "chain", "0xC0000005");
    check(GetFileAttributesA((ch.root + "\\chain_before.txt").c_str()) != INVALID_FILE_ATTRIBUTES, "chain: the filter installed before is called");
    check(GetFileAttributesA((ch.root + "\\chain_after.txt").c_str()) != INVALID_FILE_ATTRIBUTES, "chain: a filter installed after (refresh switch) still runs");

    Run so = run_child(work, "stackoverflow");
    check_crash_common(so, "stackoverflow", "0xC00000FD (stack overflow)");
    check(has(so.txt, "#20 "), "stackoverflow: deep stack walked");
    check_dump(so, "stackoverflow", true, EXCEPTION_STACK_OVERFLOW);

    Run ab = run_child(work, "abort");
    check_crash_common(ab, "abort", "0x40000015");
    check(has(ab.txt, "abort() / std::terminate"), "abort: reported as abort");

    Run ip = run_child(work, "invparam");
    check_crash_common(ip, "invparam", "0xC0000417");

    Run wk = run_child(work, "worker");
    check_crash_common(wk, "worker", "0xC0000005");
    check(has(wk.txt, "\"RSX test worker\"  <== CRASHED HERE"), "worker: crashing thread named and marked");
    check(has(wk.txt, "func_82001234"), "worker: guest function on a worker thread");

    Run rc = run_child(work, "race");
    check(rc.dumps.size() == 1 && rc.txts.size() == 1 && rc.code != 0, "race: 4 threads crash at once -> one report",
          port_report::fmt("%zu dumps", rc.dumps.size()));

    if (!quick) {
        Run hg = run_child(work, "hang", "PORT_HANG_DUMP=1\0PORT_HANG_S=5\0");
        check(hg.code == 0 && hg.dumps.size() == 1 && has(hg.dumps[0].name, "_hang.dmp"), "hang: one hang dump, game keeps running",
              port_report::fmt("exit %lu, %zu dumps", hg.code, hg.dumps.size()));
        check(has(hg.txt, "no game-loop progress for 5 s") && has(hg.txt, "==== threads") && has(hg.txt, "(hang watchdog)"),
              "hang: report says what stalled, with all threads");
        check(has(hg.crashes, "[hang]"), "hang: crashes.txt line");
        check_dump(hg, "hang", false);
        Run rh = run_child(work, "rsxhang", "PORT_HANG_DUMP=1\0PORT_HANG_S=5\0");
        check(rh.dumps.size() == 1 && has(rh.txt, "no renderer flip for 5 s"), "rsxhang: renderer stall detected while the loop runs");
        Run pz = run_child(work, "paused", "PORT_HANG_DUMP=1\0PORT_HANG_S=5\0");
        check(pz.dumps.empty() && pz.code == 0, "paused: no hang report while paused");
        Run nh = run_child(work, "nohang", "PORT_HANG_DUMP=1\0PORT_HANG_S=5\0");
        check(nh.dumps.empty() && has(nh.log, "hang watchdog strict"), "nohang: no report while both progress");
        Run off = run_child(work, "hang", "PORT_HANG_DUMP=0\0", "hang_off");
        check(off.dumps.empty() && has(off.log, "hang watchdog off"), "PORT_HANG_DUMP=0: watchdog off");
        Run def = run_child(work, "hang", NULL, "hang_default");
        check(def.dumps.empty() && has(def.log, "hang limit 90 s"), "unset: the old 90 s watchdog (no dump within 11 s)");
    }
    Run dis = run_child(work, "av", "TESTPFX_CRASH_DUMPS=0\0", "av_disabled");
    check(dis.dumps.empty() && dis.txts.empty() && dis.code == EXCEPTION_ACCESS_VIOLATION, "<prefix>_CRASH_DUMPS=0: reporter off, crash unchanged");

    bundle_tests(work, av);

    printf("\n%d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
