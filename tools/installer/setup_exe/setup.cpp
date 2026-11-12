// Setup.exe: one-click setup for the Resistance PC ports (the same source for all three games).
//
// 1. checks the build tools (tools\installer\prereqs.ps1 -Check) and installs the missing ones
//    with one UAC prompt (prereqs.ps1 -Install: winget, or pinned direct downloads without winget)
// 2. checks the player's own game copy (tools\installer\check_game.ps1)
// 3. runs setup.ps1 -Unattended (copy, decrypt, generate, build) with job counts chosen from the
//    RAM (PORT_PPU_JOBS, PORT_JOBS / RFOM_JOBS, RPCS3_JOBS), shows [N/M] progress and an ETA, logs
//    everything to logs\setup.log and retries with fewer jobs when the compiler runs out of memory
// 4. makes desktop and Start-menu shortcuts and starts the launcher
// Rerunning resumes: setup.ps1 skips finished steps and Ninja only rebuilds what is missing.
//
//   Setup.exe                 the window
//   Setup.exe --dry-run       prints what it would do (checks only; installs, copies and builds nothing)
//   Setup.exe --uninstall     removes the build outputs and shortcuts (keeps saves and settings)
//   options for --dry-run tests: --game <path> --ram-gb N --free-gb N --cores N --ppu-gb N
//                                --reduce N (OOM retry level) --parse-log <file> (progress/OOM parser)
// Build: tools\installer\setup_exe\build_setup.bat. See docs\INSTALL.md.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <cwctype>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")

using std::wstring;
typedef std::vector<wstring> Lines;

// ---------------------------------------------------------------- small helpers
static wstring g_port;  // the folder Setup.exe is in (the port folder)
static wstring P(const wstring& rel) { return g_port + L"\\" + rel; }
static bool Exists(const wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool IsDir(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static wstring Fmt(const wchar_t* f, ...) {
    wchar_t buf[4096];
    va_list a;
    va_start(a, f);
    _vsnwprintf_s(buf, _TRUNCATE, f, a);
    va_end(a);
    return buf;
}
static wstring Widen(const std::string& s, UINT cp) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    wstring w(n, 0);
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string Utf8(const wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static wstring Trim(const wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == wstring::npos ? L"" : s.substr(a, b - a + 1);
}
static wstring Lower(wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
static bool StartsWith(const wstring& s, const wstring& p) { return s.compare(0, p.size(), p) == 0; }
static bool Contains(const wstring& s, const wstring& p) { return s.find(p) != wstring::npos; }
static Lines Split(const wstring& s, wchar_t c) {
    Lines r;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(c, a);
        r.push_back(s.substr(a, b == wstring::npos ? wstring::npos : b - a));
        if (b == wstring::npos) break;
        a = b + 1;
    }
    return r;
}
static bool ReadBytes(const wstring& p, std::string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr) && got == out.size());
    CloseHandle(h);
    return ok;
}
static Lines ReadLines(const wstring& p) {
    std::string b;
    if (!ReadBytes(p, b)) return {};
    if (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) b.erase(0, 3);
    Lines r;
    for (auto& l : Split(Widen(b, CP_UTF8), L'\n')) r.push_back(Trim(l));
    while (!r.empty() && r.back().empty()) r.pop_back();
    return r;
}
// one argument for a Windows command line (CommandLineToArgvW rules)
static wstring Arg(const wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == wstring::npos) return a;
    wstring r = L"\"";
    for (size_t i = 0;; ++i) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { ++i; ++bs; }
        if (i == a.size()) { r.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { r.append(bs * 2 + 1, L'\\'); r += L'"'; }
        else { r.append(bs, L'\\'); r += a[i]; }
    }
    return r + L"\"";
}
static double ToGB(double bytes) { return bytes / (1024.0 * 1024.0 * 1024.0); }
static wstring GB(double bytes) { return Fmt(L"%.1f GB", ToGB(bytes)); }
static wstring Duration(double sec) {
    if (sec < 90) return L"about a minute";
    int m = (int)(sec / 60 + 0.5);
    if (m < 60) return Fmt(L"about %d min", m);
    return Fmt(L"about %d h %02d min", m / 60, m % 60);
}
static wstring PowerShell() {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    return wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
}
static wstring PsFile(const wstring& script) {
    return Arg(PowerShell()) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + Arg(script);
}
static ULONGLONG FreeBytes(const wstring& dir) {
    ULARGE_INTEGER fb{};
    if (!GetDiskFreeSpaceExW(dir.c_str(), &fb, nullptr, nullptr)) return 0;
    return fb.QuadPart;
}
static wstring DriveOf(const wstring& p) {
    wchar_t root[MAX_PATH] = {};
    if (GetVolumePathNameW(p.c_str(), root, MAX_PATH)) return Lower(root);
    return Lower(p.substr(0, 3));
}
static wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR s = nullptr;
    wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &s))) r = s;
    CoTaskMemFree(s);
    return r;
}

// ---------------------------------------------------------------- game profile (tools\installer\game_profile.psd1)
struct Profile {
    wstring name, titleId, region, sizeLabel, saveFolder;
    double minBytes = 0, buildBytes = 0;
    Lines required;
} g_prof;

static bool LoadProfile() {
    Lines ls = ReadLines(P(L"tools\\installer\\game_profile.psd1"));
    if (ls.empty()) return false;
    for (auto& raw : ls) {
        wstring l = raw;
        size_t eq = l.find(L'=');
        if (eq == wstring::npos || StartsWith(l, L"#")) continue;
        wstring key = Trim(l.substr(0, eq)), v = Trim(l.substr(eq + 1));
        auto quoted = [&](const wstring& s) -> wstring {
            size_t a = s.find(L'\'');
            if (a == wstring::npos) return L"";
            size_t b = s.find(L'\'', a + 1);
            return b == wstring::npos ? L"" : s.substr(a + 1, b - a - 1);
        };
        if (key == L"GameName") g_prof.name = quoted(v);
        else if (key == L"TitleId") g_prof.titleId = quoted(v);
        else if (key == L"RegionLabel") g_prof.region = quoted(v);
        else if (key == L"SizeLabel") g_prof.sizeLabel = quoted(v);
        else if (key == L"SaveFolder") g_prof.saveFolder = quoted(v);
        else if (key == L"MinBytes") g_prof.minBytes = _wtof(v.c_str());
        else if (key == L"BuildBytes") g_prof.buildBytes = _wtof(v.c_str());
        else if (key == L"Required") {
            wstring s = v.substr(0, v.find(L'#'));
            size_t a = 0;
            while ((a = s.find(L'\'', a)) != wstring::npos) {
                size_t b = s.find(L'\'', a + 1);
                if (b == wstring::npos) break;
                wstring f = s.substr(a + 1, b - a - 1);
                std::replace(f.begin(), f.end(), L'/', L'\\');
                g_prof.required.push_back(f);
                a = b + 1;
            }
        }
    }
    if (g_prof.saveFolder.empty()) g_prof.saveFolder = g_prof.name;
    return !g_prof.name.empty() && !g_prof.titleId.empty();
}
static bool TwoPrograms() {  // R1 builds build\ and build_game\ (two Ninja runs)
    std::string b;
    return ReadBytes(P(L"build.bat"), b) && b.find("build_game") != std::string::npos;
}
static bool NeedsRpcs3() { return Exists(P(L"tools\\fetch_rpcs3.ps1")); }

// ---------------------------------------------------------------- options
struct Options {
    bool dryRun = false, uninstall = false, help = false, prepare = false;
    wstring game, parseLog;
    double ramGB = 0, freeGB = 0, ppuGB = 0;
    int cores = 0, reduce = -1;
} g_opt;

// ---------------------------------------------------------------- output: console (dry run) or window + logs\setup.log
static HWND g_hwnd = nullptr;
static bool g_console = false;
static HANDLE g_out = nullptr;
// --prepare (RecompLauncher): no window; "progress <0..1> <text>" lines on stdout; exit code 0 = ready
static bool g_prepare = false;
static int g_prepProg = 0;  // 0..1000
static int g_result = 1;
static wstring g_prepStep, g_prepLast;
static FILE* g_log = nullptr;
static std::mutex g_logMx;

static void ConsoleWrite(const wstring& s) {
    if (!g_out) return;
    DWORD mode, n;
    if (GetConsoleMode(g_out, &mode)) WriteConsoleW(g_out, s.c_str(), (DWORD)s.size(), &n, nullptr);
    else { std::string u = Utf8(s); WriteFile(g_out, u.data(), (DWORD)u.size(), &n, nullptr); }
}
static void OpenConsole() {
    g_console = true;
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!g_out || g_out == INVALID_HANDLE_VALUE || GetFileType(g_out) == FILE_TYPE_UNKNOWN) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        g_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
        ConsoleWrite(L"\r\n");
    }
}
static void OpenLog(const wchar_t* name) {
    CreateDirectoryW(P(L"logs").c_str(), nullptr);
    wstring path = P(wstring(L"logs\\") + name);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeHigh == 0 && fa.nFileSizeLow > 32u * 1024 * 1024) {
        wstring old = path.substr(0, path.size() - 4) + L".old.log";
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    _wfopen_s(&g_log, path.c_str(), L"ab");
}
static void LogRaw(const wstring& s) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard<std::mutex> lk(g_logMx);
    std::string u = Utf8(Fmt(L"%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond) + s + L"\r\n");
    fwrite(u.data(), 1, u.size(), g_log);
    fflush(g_log);
}

enum { WM_APP_UI = WM_APP + 1, WM_APP_DONE, WM_APP_TOOLS, WM_APP_PREFILL };
struct UiState {
    std::mutex mx;
    std::deque<wstring> lines;
    wstring step, detail, tools, mem, game;
    int progress = 0;  // 0..1000
    bool dirty = false;
} g_ui;
static void Kick() { if (g_hwnd) PostMessageW(g_hwnd, WM_APP_UI, 0, 0); }
static void PrepProgress(const wstring& text) {
    wstring t = text;
    for (auto& ch : t) if (ch == L'\r' || ch == L'\n') ch = L' ';
    if (t == g_prepLast) return;
    g_prepLast = t;
    ConsoleWrite(Fmt(L"progress %d.%03d ", g_prepProg / 1000, g_prepProg % 1000) + t + L"\r\n");
}

// a line for the log box (or the console) and logs\setup.log
static void Say(const wstring& s) {
    LogRaw(s);
    if (g_console) { ConsoleWrite(s + L"\r\n"); return; }
    { std::lock_guard<std::mutex> lk(g_ui.mx); g_ui.lines.push_back(s); if (g_ui.lines.size() > 2000) g_ui.lines.pop_front(); }
    Kick();
}
static void SetStep(const wstring& s) {
    LogRaw(L"[step] " + s);
    if (g_prepare) { g_prepStep = s; PrepProgress(s); return; }
    if (g_console) { ConsoleWrite(L"== " + s + L"\r\n"); return; }
    { std::lock_guard<std::mutex> lk(g_ui.mx); g_ui.step = s; g_ui.dirty = true; }
    Kick();
}
static wstring g_lastDetail;
static void SetDetail(const wstring& s, int progress = -1) {
    if (g_prepare) {
        if (progress >= 0) g_prepProg = std::min(1000, std::max(0, progress));
        PrepProgress(s.empty() ? g_prepStep : s);
        return;
    }
    if (g_console) {
        if (!s.empty() && s != g_lastDetail) ConsoleWrite(L"   " + s + L"\r\n");
        g_lastDetail = s;
        return;
    }
    { std::lock_guard<std::mutex> lk(g_ui.mx); g_ui.detail = s; if (progress >= 0) g_ui.progress = progress; g_ui.dirty = true; }
    Kick();
}

static int Ask(const wstring& text, UINT flags) {
    if (g_console) { ConsoleWrite(text + L"\r\n"); return (flags & MB_YESNO) ? IDNO : IDOK; }
    return MessageBoxW(g_hwnd, text.c_str(), (g_prof.name + L" setup").c_str(), flags | MB_SETFOREGROUND);
}

// ---------------------------------------------------------------- running PowerShell and the build
static HANDLE g_job = nullptr;  // every child (PowerShell, cmd, ninja, clang-cl) is in it; closing Setup.exe stops them
static std::atomic<bool> g_cancel{false};

static void NewJob() {
    if (g_job) CloseHandle(g_job);
    g_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
}
// Runs a command hidden, with stdout+stderr line by line to onLine. Returns the exit code (-1: could not start).
static int Run(const wstring& cmd, const std::function<void(const wstring&)>& onLine) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;  // a Read-Host gets end-of-input instead of waiting forever
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(0);
    BOOL ok = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             g_port.c_str(), &si, &pi);
    CloseHandle(wr);
    CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return -1; }
    AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::string pend;
    char buf[8192];
    DWORD n;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) {
        pend.append(buf, n);
        size_t a = 0;
        for (size_t i = 0; i < pend.size(); ++i) {
            if (pend[i] == '\n' || pend[i] == '\r') {
                if (i > a) onLine(Widen(pend.substr(a, i - a), CP_OEMCP));
                a = i + 1;
            }
        }
        pend.erase(0, a);
    }
    if (!pend.empty()) onLine(Widen(pend, CP_OEMCP));
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// key=value file (check_game.ps1 output)
struct KV {
    Lines keys, vals;
    wstring operator[](const wstring& k) const {
        for (size_t i = 0; i < keys.size(); ++i) if (keys[i] == k) return vals[i];
        return L"";
    }
};
static KV ReadKV(const wstring& path) {
    KV kv;
    for (auto& l : ReadLines(path)) {
        size_t e = l.find(L'=');
        if (e == wstring::npos) continue;
        kv.keys.push_back(l.substr(0, e));
        kv.vals.push_back(l.substr(e + 1));
    }
    return kv;
}

// ---------------------------------------------------------------- 1. build tools
struct Tool { wstring key, status, version, path, label, id; };
static std::vector<Tool> g_tools;
static std::mutex g_toolsMx;
static bool g_toolsChecked = false;

static bool CheckTools() {
    wstring out = P(L"logs\\prereqs_check.txt");
    DeleteFileW(out.c_str());
    int rc = Run(PsFile(P(L"tools\\installer\\prereqs.ps1")) + L" -Check -Out " + Arg(out), [](const wstring& l) { LogRaw(L"  prereqs: " + l); });
    std::vector<Tool> t;
    for (auto& l : ReadLines(out)) {
        Lines f = Split(l, L'|');
        if (f.size() < 6) continue;
        t.push_back({f[0], f[1], f[2], f[3], f[4], f[5]});
    }
    std::lock_guard<std::mutex> lk(g_toolsMx);
    g_tools = t;
    g_toolsChecked = rc == 0 && !t.empty();
    return g_toolsChecked;
}
static const Tool* FindTool(const wstring& key) {
    for (auto& t : g_tools) if (t.key == key) return &t;
    return nullptr;
}
// tools to install: missing or too old, and not the GPU-driver / winget lines
static Lines MissingIds(wstring* names) {
    Lines ids;
    for (auto& t : g_tools) {
        if (t.status == L"ok" || t.id.empty()) continue;
        ids.push_back(t.id);
        if (names) *names += L"  \x2022 " + t.label + (t.status == L"old" ? L" (update from " + t.version + L")" : t.status == L"nosdk" ? L" (add the Windows SDK)" : L"") + L"\r\n";
    }
    return ids;
}
static wstring ToolsText() {
    std::lock_guard<std::mutex> lk(g_toolsMx);
    if (g_tools.empty()) return L"Checking the build tools...";
    wstring s;
    for (auto& t : g_tools) {
        if (t.key == L"winget" || t.key == L"vulkan") continue;
        wstring name = t.key == L"python" ? L"Python" : t.key == L"cmake" ? L"CMake" : t.key == L"ninja" ? L"Ninja" :
                       t.key == L"clang" ? L"LLVM" : t.key == L"git" ? L"Git" : L"VS Build Tools";
        wstring mark = t.status == L"ok" ? L"\x2713 " : L"\x2717 ";
        wstring st = t.status == L"ok" ? t.version : t.status == L"old" ? t.version + L" (will update)" :
                     t.status == L"nosdk" ? L"needs the Windows SDK" : L"will be installed";
        s += mark + name + L" " + st + L"     ";
    }
    const Tool* w = FindTool(L"winget");
    const Tool* v = FindTool(L"vulkan");
    s += L"\r\n";
    wstring dummy;
    bool any = !MissingIds(&dummy).empty();
    if (any) s += w && w->status == L"ok" ? L"Missing tools are installed with winget (one administrator prompt)."
                                         : L"winget is not available: Setup downloads the official installers directly (one administrator prompt).";
    else s += L"All build tools are installed.";
    if (NeedsRpcs3() && v && v->status != L"ok") s += L"\r\nNo Vulkan driver found (vulkan-1.dll): install your GPU's latest driver before playing.";
    return s;
}
static void ShowTools() {
    if (g_console) return;
    { std::lock_guard<std::mutex> lk(g_ui.mx); g_ui.tools = ToolsText(); g_ui.dirty = true; }
    Kick();
}
// rough extra space on the system drive for the tools
static double ToolsBytes(const Lines& ids) {
    double b = 0;
    for (auto& i : ids) {
        if (i == L"Microsoft.VisualStudio.2022.BuildTools") b += 7.0e9;
        else if (i == L"VSModify") b += 2.5e9;
        else if (i == L"LLVM.LLVM") b += 3.0e9;
        else b += 0.4e9;
    }
    return b;
}
static wstring InstallCommandArgs(const Lines& ids, bool dry) {
    wstring list;
    for (auto& i : ids) list += (list.empty() ? L"" : L",") + i;
    return L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File " + Arg(P(L"tools\\installer\\prereqs.ps1")) +
           L" -Install -Ids " + Arg(list) + L" -Log " + Arg(P(L"logs\\prereqs.log")) + L" -Out " + Arg(P(L"logs\\prereqs_result.txt")) +
           (dry ? L" -DryRun" : L"");
}
// the only elevated step: one UAC prompt for all the tools
static bool InstallTools(const Lines& ids, wstring& err, wstring& todo) {
    wstring result = P(L"logs\\prereqs_result.txt"), logp = P(L"logs\\prereqs.log");
    DeleteFileW(result.c_str());
    wstring args = InstallCommandArgs(ids, false);
    Say(L"Installing the build tools (elevated): " + args);
    SHELLEXECUTEINFOW se{sizeof se};
    se.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    se.hwnd = g_hwnd;
    se.lpVerb = L"runas";
    wstring ps = PowerShell();
    se.lpFile = ps.c_str();
    se.lpParameters = args.c_str();
    se.lpDirectory = g_port.c_str();
    se.nShow = SW_HIDE;
    if (!ShellExecuteExW(&se) || !se.hProcess) {
        DWORD e = GetLastError();
        err = e == ERROR_CANCELLED ? L"Setup needs administrator permission once, to install the build tools."
                                   : Fmt(L"Could not start the tool installer (error %lu).", e);
        todo = L"Click Install again and answer Yes, or install the tools yourself (docs\\INSTALL.md, \"Manual install\").";
        return false;
    }
    // show the installer's log while it runs (Visual Studio alone can take 15+ minutes)
    LONGLONG off = 0;
    ULONGLONG t0 = GetTickCount64();
    for (;;) {
        DWORD w = WaitForSingleObject(se.hProcess, 1000);
        std::string b;
        if (ReadBytes(logp, b) && (LONGLONG)b.size() > off) {
            for (auto& l : Split(Widen(b.substr((size_t)off), CP_UTF8), L'\n')) {
                wstring t = Trim(l);
                if (t.empty()) continue;
                if (g_console) ConsoleWrite(L"   " + t + L"\r\n");
                else { std::lock_guard<std::mutex> lk(g_ui.mx); g_ui.lines.push_back(L"   " + t); }
                if (Contains(t, L"== ")) SetDetail(L"Installing " + t.substr(t.find(L"== ") + 3) + L" ...");
            }
            off = (LONGLONG)b.size();
            Kick();
        }
        if (w == WAIT_OBJECT_0) break;
        SetStep(L"Installing the build tools (" + Duration((GetTickCount64() - t0) / 1000.0).substr(6) + L" so far; Visual Studio can take 15+ minutes)");
    }
    CloseHandle(se.hProcess);
    Lines res = ReadLines(result);
    bool reboot = false;
    wstring failed;
    for (auto& l : res) {
        Lines f = Split(l, L'|');
        if (f.size() >= 2 && f[1] == L"fail") failed += L"  \x2022 " + f[0] + (f.size() >= 4 && !f[3].empty() ? L": " + f[3] : L"") + L"\r\n";
        if (f.size() >= 4 && f[3] == L"reboot") reboot = true;
    }
    if (res.empty() || res.back() != L"done") failed += L"  (the installer stopped early; see logs\\prereqs.log)\r\n";
    if (!failed.empty()) {
        err = L"Some build tools did not install:\r\n" + failed;
        todo = L"Check the internet connection, restart the PC and click Resume. If it keeps failing, install them yourself "
               L"(docs\\INSTALL.md, \"Manual install\"). Details: logs\\prereqs.log.";
        return false;
    }
    if (reboot) Say(L"The tool installer asks for a restart. If the build cannot find the tools, restart the PC and click Resume.");
    return true;
}

// ---------------------------------------------------------------- 2. the game copy
static bool HaveGameFiles() {
    if (Exists(P(L"gamefiles.copying"))) return false;
    if (g_prof.required.empty()) return Exists(P(L"gamefiles\\PS3_GAME\\USRDIR\\EBOOT.BIN"));
    for (auto& r : g_prof.required) if (!Exists(P(L"gamefiles\\" + r))) return false;
    return true;
}
static KV CheckGame(const wstring& path) {
    wstring out = P(L"logs\\game_check.txt");
    DeleteFileW(out.c_str());
    Run(PsFile(P(L"tools\\installer\\check_game.ps1")) + L" -Game " + Arg(path) + L" -Dest " + Arg(P(L"gamefiles")) + L" -Out " + Arg(out),
        [](const wstring& l) { LogRaw(L"  check_game: " + l); });
    KV kv = ReadKV(out);
    if (kv.keys.empty()) { kv.keys = {L"status", L"code", L"problem"}; kv.vals = {L"skip", L"SCRIPT", L"The game check did not run (PowerShell blocked?)."}; }
    LogRaw(L"Game check of " + path + L": status=" + kv[L"status"] + L" code=" + kv[L"code"] + L" titleid=" + kv[L"titleid"]);
    return kv;
}
static wstring GameCheckText(const KV& kv) {
    wstring st = kv[L"status"];
    if (st == L"ok") return L"\x2713 " + kv[L"title"] + L" (" + kv[L"titleid"] + L"), " + GB(_wtof(kv[L"bytes"].c_str())) + L". Good to go.";
    wstring s = (st == L"warn" ? L"\x26A0 " : L"\x2717 ") + kv[L"problem"];
    if (!kv[L"fix"].empty()) s += L"\r\nWhat to do: " + kv[L"fix"];
    return s;
}

// ---------------------------------------------------------------- memory-aware job counts
struct Machine { double totalGB = 0, availGB = 0; int cores = 1; };
struct Jobs { int ppu = 1, game = 1, rpcs3 = 1; double ppuGB = 8; };

static Machine GetMachine() {
    Machine m;
    MEMORYSTATUSEX ms{sizeof ms};
    GlobalMemoryStatusEx(&ms);
    m.totalGB = ToGB((double)ms.ullTotalPhys);
    m.availGB = ToGB((double)ms.ullAvailPhys);
    m.cores = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (g_opt.ramGB > 0) m.totalGB = g_opt.ramGB;
    if (g_opt.freeGB > 0) m.availGB = g_opt.freeGB;
    else if (g_opt.ramGB > 0) m.availGB = std::min(m.availGB, g_opt.ramGB * 0.75);
    if (g_opt.cores > 0) m.cores = g_opt.cores;
    if (m.cores < 1) m.cores = 1;
    return m;
}
// RAM one lifted ppu_recomp_NNN.cpp needs in clang-cl. Measured 3.7-8.4 GB on R2/R3 (files up to 194 MB);
// R1's files are 10x smaller (max 10 MB, 2 min compiles instead of 15).
static double PpuGB() {
    if (g_opt.ppuGB > 0) return g_opt.ppuGB;
    wchar_t v[32] = {};
    if (GetEnvironmentVariableW(L"PORT_PPU_GB", v, 32) && _wtof(v) > 0) return _wtof(v);
    return g_prof.titleId == L"BCUS98107" ? 4.0 : 8.0;
}
// ppu  = lifted-PPU files compiled at once: (total RAM - 6 GB) / ppuGB, but no more than the free RAM allows
// game = all compiles at once (-j): the ppu ones plus ~2 GB per other file, at most one per logical core
// rpcs3 = -j for the RPCS3 renderer library (~2 GB per file)
// reduce = OOM retries: each one halves the counts.
static Jobs PlanJobs(const Machine& m, int reduce) {
    Jobs j;
    j.ppuGB = PpuGB();
    double total = m.totalGB, avail = std::max(m.availGB, 1.0);
    j.ppu = (int)std::min(std::floor((total - 6) / j.ppuGB), std::floor((avail - 2) / j.ppuGB));
    j.ppu = std::max(1, std::min(j.ppu, m.cores));
    j.game = j.ppu + (int)std::floor((avail - j.ppuGB * j.ppu - 2) / 2);
    j.game = std::max(1, std::min(j.game, m.cores));
    j.ppu = std::min(j.ppu, j.game);
    j.rpcs3 = std::max(1, std::min((int)std::floor((avail - 2) / 2), m.cores));
    for (int i = 0; i < reduce; ++i) {
        j.ppu = std::max(1, j.ppu / 2);
        j.game = std::max(1, j.game / 2);
        j.rpcs3 = std::max(1, j.rpcs3 / 2);
    }
    return j;
}
static wstring JobsText(const Machine& m, const Jobs& j) {
    wstring s = Fmt(L"%.0f GB RAM (%.1f GB free), %d logical cores \x2192 %d big game-code file%s at a time, -j %d",
                    m.totalGB, m.availGB, m.cores, j.ppu, j.ppu == 1 ? L"" : L"s", j.game);
    if (NeedsRpcs3()) s += Fmt(L" (RPCS3: -j %d)", j.rpcs3);
    return s;
}
static wstring RamWarning(const Machine& m) {
    if (m.totalGB < 11.5)
        return Fmt(L"This PC has %.0f GB of RAM. The build needs at least 12 GB (16 GB or more recommended): one game-code file "
                   L"can take up to %.0f GB to compile. It may fail with \"out of memory\"; enlarging the Windows page file can help.", m.totalGB, PpuGB());
    if (m.totalGB < 15.5)
        return Fmt(L"This PC has %.0f GB of RAM (16 GB or more recommended). Setup compiles one file at a time, so the build "
                   L"takes several hours. Close other programs (browsers, games) while it runs.", m.totalGB);
    if (m.availGB < 10)
        return Fmt(L"Only %.1f GB of RAM is free right now. Close other programs for a faster build.", m.availGB);
    return L"";
}
static void ApplyJobs(const Jobs& j) {
    SetEnvironmentVariableW(L"PORT_PPU_JOBS", std::to_wstring(j.ppu).c_str());
    SetEnvironmentVariableW(L"PORT_JOBS", std::to_wstring(j.game).c_str());
    SetEnvironmentVariableW(L"RFOM_JOBS", std::to_wstring(j.game).c_str());
    SetEnvironmentVariableW(L"RPCS3_JOBS", std::to_wstring(j.rpcs3).c_str());
}

// ---------------------------------------------------------------- build progress and errors
// Windows PowerShell's Tee-Object writes UTF-16LE with a BOM; everything else is UTF-8
static bool IsUtf16Bom(const std::string& s) { return s.size() >= 2 && (unsigned char)s[0] == 0xFF && (unsigned char)s[1] == 0xFE; }
static wstring DecodeLog(const std::string& s) {
    if (IsUtf16Bom(s)) return wstring((const wchar_t*)(s.data() + 2), (s.size() - 2) / 2);
    return Widen(s, CP_UTF8);
}
struct BuildLog { wstring rel, what; int lo, hi; LONGLONG off = 0; std::string pend; int enc = -1; /* -1 unknown, 0 UTF-8, 1 UTF-16LE */ };
static std::vector<BuildLog> BuildLogs() {
    if (TwoPrograms()) return {{L"logs\\build.log", L"Building the front end", 30, 50}, {L"logs\\build_game.log", L"Building the game", 50, 98}};
    if (NeedsRpcs3()) return {{L"logs\\build_rpcs3.log", L"Building the RPCS3 renderer", 30, 42}, {L"logs\\build.log", L"Building the game", 42, 98}};
    return {{L"logs\\build.log", L"Building the game", 30, 98}};
}
struct Meter {
    wstring what;
    int n = 0, m = 0, n0 = 0;
    ULONGLONG t0 = 0;
    int lo = 0, hi = 100;
    // one "[N/M] ..." line from Ninja; returns the detail text
    bool Feed(const wstring& line, const BuildLog& src) {
        if (line.size() < 5 || line[0] != L'[') return false;
        int a = 0, b = 0;
        if (swscanf_s(line.c_str(), L"[%d/%d]", &a, &b) != 2 || b <= 0) return false;
        if (src.what != what || b != m || a < n) { what = src.what; m = b; n0 = a; t0 = GetTickCount64(); lo = src.lo; hi = src.hi; }
        n = a;
        return true;
    }
    wstring Text() const {
        wstring s = Fmt(L"%s: %d / %d files", what.c_str(), n, m);
        double el = (GetTickCount64() - t0) / 1000.0;
        if (n - n0 >= 3 && el > 20 && n < m) s += L", " + Duration(el / (n - n0) * (m - n)) + L" left for this part";
        return s;
    }
    int Progress() const { return m ? (lo * 10 + (hi - lo) * 10 * n / m) : lo * 10; }
};
static const wchar_t* kOomSigns[] = {L"out of memory", L"bad_alloc", L"allocation failed", L"not enough memory",
                                     L"paging file", L"insufficient memory", L"failed due to signal", L"0xc0000017",
                                     L"memory allocation of", L"cannot allocate memory"};
static bool LooksLikeOom(const wstring& text) {
    for (auto& line : Split(Lower(text), L'\n')) {
        // skip the source lines clang quotes under a diagnostic ("   22 | #define ... Not enough memory")
        size_t k = line.find_first_not_of(L" \t0123456789");
        if (k == wstring::npos || line[k] == L'|' || Contains(line, L"#define") || Contains(line, L"//") || Contains(line, L"/*")) continue;
        for (auto s : kOomSigns) if (Contains(line, s)) return true;
    }
    return false;
}
// the tail of each build log written since t (FILETIME), for the OOM check
static wstring RecentBuildLogText(const FILETIME& since) {
    wstring all;
    for (auto& b : BuildLogs()) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(P(b.rel).c_str(), GetFileExInfoStandard, &fa) || CompareFileTime(&fa.ftLastWriteTime, &since) < 0) continue;
        std::string s;
        if (!ReadBytes(P(b.rel), s)) continue;
        bool u16 = IsUtf16Bom(s);
        if (s.size() > 512 * 1024) s.erase(0, s.size() - 512 * 1024);
        if (u16) { if (s.size() & 1) s.erase(0, 1); all += wstring((const wchar_t*)s.data(), s.size() / 2) + L"\n"; }
        else all += Widen(s, CP_UTF8) + L"\n";
    }
    return all;
}

// ---------------------------------------------------------------- resume state (logs\setup_state.ini)
static wstring StatePath() { return P(L"logs\\setup_state.ini"); }
static wstring GetState(const wchar_t* k) {
    wchar_t b[2048] = {};
    GetPrivateProfileStringW(L"setup", k, L"", b, 2048, StatePath().c_str());
    return b;
}
static void PutState(const wchar_t* k, const wstring& v) {
    if (g_opt.dryRun) return;
    CreateDirectoryW(P(L"logs").c_str(), nullptr);
    WritePrivateProfileStringW(L"setup", k, v.c_str(), StatePath().c_str());
}

// ---------------------------------------------------------------- shortcuts
static bool MakeLink(const wstring& lnk, const wstring& target, const wstring& desc) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(g_port.c_str());
    sl->SetDescription(desc.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    bool ok = SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
    if (pf) pf->Release();
    sl->Release();
    return ok;
}
static wstring StartMenuDir() { return KnownFolder(FOLDERID_Programs) + L"\\" + g_prof.saveFolder; }
static wstring DesktopLink() { return KnownFolder(FOLDERID_Desktop) + L"\\" + g_prof.saveFolder + L".lnk"; }
static void MakeShortcuts(bool desktop) {
    // runs on the setup worker thread, which has no COM apartment of its own (CoCreateInstance fails without one)
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wstring sm = StartMenuDir();
    CreateDirectoryW(sm.c_str(), nullptr);
    bool a = MakeLink(sm + L"\\" + g_prof.saveFolder + L".lnk", P(L"Launcher.exe"), g_prof.name + L" - PC port launcher");
    bool b = MakeLink(sm + L"\\Setup (repair, rebuild, uninstall).lnk", P(L"Setup.exe"), g_prof.name + L" - setup");
    bool c = !desktop || MakeLink(DesktopLink(), P(L"Launcher.exe"), g_prof.name + L" - PC port launcher");
    Say(a && b && c ? L"Shortcuts created (Start menu" + wstring(desktop ? L" and desktop)." : L").") : L"Some shortcuts could not be created.");
    if (SUCCEEDED(co)) CoUninitialize();
}

// ---------------------------------------------------------------- uninstall
struct RmItem { wstring path; bool dir; };
static std::vector<RmItem> UninstallList() {
    std::vector<RmItem> r;
    for (auto d : {L"build", L"build_game", L"build_regen", L"build_rpcs3", L"build_shaderscan", L"rsxcap", L"recompiled", L"recompiled_game",
                   L"shadercache", L"game", L"tools\\ffmpeg_bin", L"tools\\dlss", L"tools\\xess\\bin", L"tools\\xess\\inc"})
        if (IsDir(P(d))) r.push_back({P(d), true});
    if (NeedsRpcs3() && IsDir(P(L"ps3recomp\\third_party\\rpcs3"))) r.push_back({P(L"ps3recomp\\third_party\\rpcs3"), true});
    // generated SPU job modules (project\spu\<module>\, jobmods_register.c)
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(P(L"project\\spu\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
                r.push_back({P(L"project\\spu\\") + fd.cFileName, true});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (auto f : {L"project\\spu\\jobmods_register.c", L"tools\\key_vault.cpp", L"tools\\shaderscan.exe", L"Launcher.exe", L"gamefiles.copying", L"logs\\setup_state.ini"})
        if (Exists(P(f))) r.push_back({P(f), false});
    wstring sm = StartMenuDir(), dl = DesktopLink();
    if (IsDir(sm)) r.push_back({sm, true});
    if (Exists(dl)) r.push_back({dl, false});
    return r;
}
static ULONGLONG TreeSize(const wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return 0;
    if (!(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    if (fa.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return 0;
    ULONGLONG s = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((p + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) s += TreeSize(p + L"\\" + fd.cFileName);
        else s += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return s;
}
static bool DeleteTree(const wstring& p) {
    wstring lp = StartsWith(p, L"\\\\?\\") ? p : L"\\\\?\\" + p;
    DWORD a = GetFileAttributesW(lp.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(lp.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(lp.c_str()) != 0;
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {  // never follow junctions
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((lp + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) DeleteTree(lp + L"\\" + fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    return RemoveDirectoryW(lp.c_str()) != 0;
}
static const wchar_t* kKeep = L"Kept: your saves (gamedata\\), settings (settings.ini, settings.bat, controls.txt, mouselook.cfg, "
                              L"%USERPROFILE%\\Saved Games\\...), screenshots and logs\\.";
static int Uninstall() {
    auto items = UninstallList();
    bool gf = IsDir(P(L"gamefiles"));
    ULONGLONG total = 0;
    wstring list;
    for (auto& i : items) {
        ULONGLONG s = TreeSize(i.path);
        total += s;
        list += Fmt(L"  %-60s %8s\r\n", i.path.c_str(), GB((double)s).c_str());
    }
    if (g_opt.dryRun) {
        ConsoleWrite(L"Uninstall (dry run): would delete " + GB((double)total) + L":\r\n" + list);
        if (gf) ConsoleWrite(L"  and would ask before deleting your game copy " + P(L"gamefiles") + L" (default: keep it)\r\n");
        ConsoleWrite(wstring(kKeep) + L"\r\n");
        return 0;
    }
    if (items.empty() && !gf) { Ask(L"There is nothing to remove.", MB_OK | MB_ICONINFORMATION); return 0; }
    if (Ask(L"Remove the build of " + g_prof.name + L" (" + GB((double)total) + L") and its shortcuts?\r\n\r\n" + kKeep +
            L"\r\n\r\nThe port's own files stay, so you can run Setup.exe again later.", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return 1;
    bool delGame = gf && Ask(L"Also delete your game copy in " + P(L"gamefiles") + L" (" + GB((double)TreeSize(P(L"gamefiles"))) +
                                 L")?\r\n\r\nYou need it again to rebuild the port. Your original ISO or folder is not touched.",
                             MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    if (delGame) items.push_back({P(L"gamefiles"), true});
    wstring failed;
    for (auto& i : items) {
        LogRaw(L"Uninstall: deleting " + i.path);
        if (!DeleteTree(i.path)) failed += L"  " + i.path + L"\r\n";
    }
    if (!failed.empty()) {
        Ask(L"Some files could not be deleted (is the game or the launcher still running?):\r\n" + failed, MB_OK | MB_ICONWARNING);
        return 1;
    }
    Ask(L"Done. " + wstring(kKeep) + L"\r\nTo remove everything, delete the folder " + g_port + L" (after copying gamedata\\ if you want to keep your saves).", MB_OK | MB_ICONINFORMATION);
    return 0;
}

// ---------------------------------------------------------------- the setup run
struct Choice {
    wstring game, copyMode;  // copyMode: Copy / Move (game folder on the same drive)
    bool sdks = false, launch = true, desktop = true;
};

static wstring Hint(const wstring& msg) {
    wstring l = Lower(msg);
    if (Contains(l, L"internet") || Contains(l, L"download") || Contains(l, L"github"))
        return L"Check the internet connection (and any proxy or firewall), then click Resume.";
    if (Contains(l, L"space") || Contains(l, L"disk"))
        return L"Free up disk space (see docs\\INSTALL.md, \"Disk space\"), then click Resume.";
    if (Contains(l, L"build failed") || Contains(l, L"configure failed"))
        return L"Antivirus programs sometimes block the compiler: add the port folder to the antivirus exclusions "
               L"(docs\\INSTALL.md, \"Antivirus\"), then click Resume. The build continues where it stopped.";
    if (Contains(l, L"not the expected version") || Contains(l, L"game copy"))
        return L"Pick the supported game version (" + g_prof.titleId + L"), then click Resume.";
    return L"Fix the problem above and click Resume: finished steps are skipped.";
}

// runs setup.ps1 once; returns 0 ok, 1 failed, 2 out of memory, 3 cancelled
static int RunSetupOnce(const Choice& c, const Jobs& j, wstring& stopMsg) {
    wstring cmd = PsFile(P(L"setup.ps1")) + L" -Unattended";
    if (!c.game.empty()) cmd += L" -Game " + Arg(c.game);
    if (!c.copyMode.empty()) cmd += L" -GameCopyMode " + c.copyMode;
    cmd += c.sdks ? L" -AcceptSdkLicenses" : L" -NoOptionalSdks";
    ApplyJobs(j);
    Say(L"Running: " + cmd);
    Say(Fmt(L"Jobs: PORT_PPU_JOBS=%d PORT_JOBS/RFOM_JOBS=%d RPCS3_JOBS=%d", j.ppu, j.game, j.rpcs3));
    FILETIME start;
    GetSystemTimeAsFileTime(&start);

    // follow the build logs for Ninja's [N/M] (build_rpcs3.log is not echoed to the console)
    std::atomic<bool> stop{false};
    // a ticking clock on the step line and a note during quiet stretches, so a long step never looks hung;
    // while the game is copied, an ETA from the size of gamefiles\ (Write-Progress does not reach us)
    std::function<double(const wstring&)> dirBytes = [&](const wstring& d) {
        double t = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return t;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) t += dirBytes(d + L"\\" + fd.cFileName);
            else t += (double)(((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return t;
    };
    std::mutex stepMx;
    wstring stepBase;
    ULONGLONG stepT0 = GetTickCount64();
    std::atomic<ULONGLONG> lastOut{GetTickCount64()};
    std::thread tail([&] {
        auto logs = BuildLogs();
        Meter meter;
        ULONGLONG lastShown = 0, lastFeed = 0, lastClock = 0, copyT0 = 0;
        double copy0 = -1, copyNow = 0;
        int copyTick = 0;
        while (!stop) {
            bool fed = false;
            for (auto& b : logs) {
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                if (!GetFileAttributesExW(P(b.rel).c_str(), GetFileExInfoStandard, &fa) || CompareFileTime(&fa.ftLastWriteTime, &start) < 0) continue;
                LONGLONG size = ((LONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
                if (size < b.off) { b.off = 0; b.pend.clear(); }
                if (size == b.off) continue;
                HANDLE h = CreateFileW(P(b.rel).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (h == INVALID_HANDLE_VALUE) continue;
                if (b.enc < 0) {
                    if (size < 2) { CloseHandle(h); continue; }
                    char bom[2] = {};
                    DWORD g2 = 0;
                    ReadFile(h, bom, 2, &g2, nullptr);
                    b.enc = (g2 == 2 && (unsigned char)bom[0] == 0xFF && (unsigned char)bom[1] == 0xFE) ? 1 : 0;
                    if (b.enc == 1 && b.off == 0) b.off = 2;
                }
                if (size - b.off > 4 * 1024 * 1024) { b.off = size - 64 * 1024; if (b.enc == 1) b.off &= ~1LL; b.pend.clear(); }
                LARGE_INTEGER li;
                li.QuadPart = b.off;
                SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
                std::string chunk((size_t)(size - b.off), 0);
                DWORD got = 0;
                ReadFile(h, &chunk[0], (DWORD)chunk.size(), &got, nullptr);
                CloseHandle(h);
                chunk.resize(got);
                b.off += got;
                b.pend += chunk;
                if (b.enc == 1) {
                    wstring w((const wchar_t*)b.pend.data(), b.pend.size() / 2);
                    size_t a = 0, i;
                    while ((i = w.find(L'\n', a)) != wstring::npos) { fed |= meter.Feed(Trim(w.substr(a, i - a)), b); a = i + 1; }
                    b.pend.erase(0, a * 2);
                } else {
                    size_t a = 0, i;
                    while ((i = b.pend.find('\n', a)) != std::string::npos) {
                        fed |= meter.Feed(Trim(Widen(b.pend.substr(a, i - a), CP_UTF8)), b);
                        a = i + 1;
                    }
                    b.pend.erase(0, a);
                }
            }
            ULONGLONG now = GetTickCount64();
            if (fed) lastFeed = now;
            bool quiet = now - lastOut > 30000 && now - lastFeed > 30000;
            if (meter.m && now - lastShown > 900) {
                SetDetail(meter.Text(), meter.Progress());
                lastShown = now;
            }
            if ((!g_console || g_prepare) && now - lastClock >= (ULONGLONG)(g_prepare ? 5000 : 1000)) {
                lastClock = now;
                wstring base;
                ULONGLONG t0;
                { std::lock_guard<std::mutex> lk(stepMx); base = stepBase; t0 = stepT0; }
                if (!base.empty()) {
                    ULONGLONG s = (now - t0) / 1000;
                    wstring clock = s >= 3600 ? Fmt(L"%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60))
                                              : Fmt(L"%d:%02d", (int)(s / 60), (int)(s % 60));
                    int copyProg = -1;
                    if (StartsWith(base, L"2/5") && g_prof.minBytes > 0) {
                        if (copyTick++ % 2 == 0) {
                            copyNow = dirBytes(P(L"gamefiles"));
                            if (copy0 < 0) { copy0 = copyNow; copyT0 = now; }
                        }
                        double total = std::max(g_prof.minBytes, copyNow * 1.01);
                        clock += Fmt(L"  \x2022  %.1f of about %.1f GB", copyNow / 1e9, total / 1e9);
                        double el = (now - copyT0) / 1000.0, done = copyNow - copy0;
                        if (el > 15 && done > 200e6) clock += L"  \x2022  " + Duration((total - copyNow) / (done / el)) + L" left";
                        copyProg = 100 + (int)(100 * std::min(1.0, copyNow / total));
                    }
                    if (g_prepare) {
                        if (copyProg >= 0) g_prepProg = copyProg;
                        if (copyProg >= 0 || quiet) PrepProgress((meter.m && copyProg < 0 ? meter.Text() : base) + L"   (" + clock + L")");
                        continue;
                    }
                    std::lock_guard<std::mutex> lk(g_ui.mx);
                    g_ui.step = base + L"   (" + clock + L")";
                    if (copyProg >= 0) g_ui.progress = copyProg;
                    if (quiet && !meter.m)
                        g_ui.detail = Fmt(L"Still working: no new messages for %d min %02d s. Some steps print nothing for 10+ minutes; this is normal.",
                                          (int)((now - lastOut) / 60000), (int)((now - lastOut) / 1000 % 60));
                    g_ui.dirty = true;
                }
                Kick();
            }
            for (int k = 0; k < 10 && !stop; ++k) Sleep(100);
        }
    });

    bool complete = false;
    std::deque<wstring> last;
    int rc = Run(cmd, [&](const wstring& raw) {
        wstring l = Trim(raw);
        if (l.empty()) return;
        LogRaw(L"| " + raw);
        lastOut = GetTickCount64();
        if (!g_console) { std::lock_guard<std::mutex> lk(g_ui.mx); g_ui.lines.push_back(l); if (g_ui.lines.size() > 2000) g_ui.lines.pop_front(); Kick(); }
        last.push_back(l);
        if (last.size() > 400) last.pop_front();
        if (StartsWith(l, L"== ")) {
            wstring s = l.substr(3);
            SetStep(s);
            { std::lock_guard<std::mutex> lk(stepMx); stepBase = s; stepT0 = GetTickCount64(); }
            int p = StartsWith(s, L"1/5") ? 50 : StartsWith(s, L"2/5") ? 100 : StartsWith(s, L"3/5") ? 200 : StartsWith(s, L"4/5") ? 230 : StartsWith(s, L"5/5") ? 280 : 20;
            SetDetail(L"", p);
        } else if (StartsWith(l, L"SETUP STOPPED")) {
            stopMsg = l.substr(l.find(L':') + 1);
        } else if (StartsWith(l, L"SETUP COMPLETE")) {
            complete = true;
        } else if (l[0] != L'[' && !g_console) {
            // the latest message, unless the Ninja progress ("Building ...") is showing
            std::lock_guard<std::mutex> lk(g_ui.mx);
            if (g_ui.detail.empty() || !StartsWith(g_ui.detail, L"Building")) g_ui.detail = l.substr(0, 160);
            g_ui.dirty = true;
        }
    });
    stop = true;
    tail.join();
    if (g_cancel) return 3;
    Say(Fmt(L"setup.ps1 exited with code %d", rc));
    if (rc == 0 && complete) return 0;
    wstring text;
    for (auto& l : last) text += l + L"\n";
    if (LooksLikeOom(text) || LooksLikeOom(RecentBuildLogText(start))) return 2;
    if (stopMsg.empty()) stopMsg = rc == -1 ? L"Could not start PowerShell." : Fmt(L"setup.ps1 stopped with exit code %d.", rc);
    stopMsg = Trim(stopMsg);
    return 1;
}

static void Fail(const wstring& what, const wstring& todo) {
    Say(L"STOPPED: " + what);
    PutState(L"lastError", what);
    SetStep(L"Stopped. Fix the problem below and click Resume.");
    wstring t = what + L"\r\n\r\nWhat to do: " + todo +
                L"\r\n\r\nFull log: logs\\setup.log. To report a problem, see docs\\REPORTING.md and attach the logs folder.\r\n\r\nOpen the logs folder now?";
    if (Ask(t, MB_YESNO | MB_ICONERROR) == IDYES) ShellExecuteW(g_hwnd, L"explore", P(L"logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void SetupWorker(Choice c) {
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);  // no sleep while it builds
    PutState(L"phase", L"tools");
    PutState(L"sdks", c.sdks ? L"accept" : L"skip");
    LogRaw(L"==== Setup.exe start: " + g_port);

    // 1. tools
    SetStep(L"Checking the build tools");
    SetDetail(L"", 10);
    if (!g_toolsChecked) CheckTools();
    ShowTools();
    wstring names;
    Lines ids = MissingIds(&names);
    if (!g_toolsChecked) { Fail(L"The build-tool check did not run.", L"Make sure PowerShell is not blocked (docs\\INSTALL.md), then click Resume."); goto out; }
    if (!ids.empty()) {
        wstring t = L"Setup will install these free build tools:\r\n\r\n" + names +
                    Fmt(L"\r\nThey need about %.0f GB on drive C: and a download of several GB. Windows asks once for administrator permission.\r\n\r\nContinue?", ToGB(ToolsBytes(ids)) + 0.5);
        if (Ask(t, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) { SetStep(L"Cancelled."); goto out; }
        SetStep(L"Installing the build tools");
        wstring err, todo;
        if (!InstallTools(ids, err, todo)) { Fail(err, todo); goto out; }
        CheckTools();
        ShowTools();
        names.clear();
        ids = MissingIds(&names);
        if (!ids.empty()) {
            Fail(L"These tools are still missing after the install:\r\n" + names,
                 L"Restart the PC (some installers need it) and click Resume. If they are still missing, install them yourself (docs\\INSTALL.md, \"Manual install\").");
            goto out;
        }
    }
    if (g_cancel) goto out;

    // 2. game copy
    {
        PutState(L"phase", L"game");
        SetStep(L"Checking your game copy");
        bool have = HaveGameFiles();
        if (!have) {
            if (c.game.empty()) {
                Fail(Exists(P(L"gamefiles.copying")) ? L"Copying the game was interrupted." : L"No game copy picked yet.",
                     L"Pick your " + g_prof.name + L" ISO or game folder (ISO... or Folder...) and click " + (Exists(StatePath()) ? L"Resume." : L"Install."));
                goto out;
            }
            KV kv = CheckGame(c.game);
            wstring st = kv[L"status"];
            Say(L"Game check: " + GameCheckText(kv));
            if (st == L"error" || st == L"skip") { Fail(kv[L"problem"], kv[L"fix"].empty() ? L"Pick another copy of the game." : kv[L"fix"]); goto out; }
            if (st == L"warn" && Ask(GameCheckText(kv) + L"\r\n\r\nContinue anyway?", MB_YESNO | MB_ICONWARNING) != IDYES) { SetStep(L"Cancelled."); goto out; }
            double bytes = _wtof(kv[L"bytes"].c_str());
            if (c.copyMode.empty()) {
                c.copyMode = L"Copy";
                if (kv[L"kind"] == L"folder" && DriveOf(kv[L"root"]) == DriveOf(g_port)) {
                    if (Ask(L"Your game folder is on the same drive as the port.\r\n\r\nMove it into the port folder instead of copying it? "
                            L"Moving is instant and needs no extra space, but the folder is then gone from its old place "
                            L"(RPCS3 will not find it there any more).\r\n\r\nYes = move, No = copy (needs " + GB(bytes) + L" more).",
                            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
                        c.copyMode = L"Move";
                }
            }
            double need = g_prof.buildBytes + (c.copyMode == L"Move" ? 0 : bytes);
            double freeB = (double)FreeBytes(g_port);
            if (freeB < need) {
                Fail(L"Not enough disk space: " + GB(need) + L" is needed on " + DriveOf(g_port) + L" and " + GB(freeB) + L" is free.",
                     L"Free up space, or move the port folder to a drive with more space and run Setup.exe from there.");
                goto out;
            }
            PutState(L"game", c.game);
            PutState(L"copymode", c.copyMode);
        } else {
            c.game.clear();
            c.copyMode.clear();
            Say(L"Game files already in place: " + P(L"gamefiles"));
            double freeB = (double)FreeBytes(g_port);
            if (!Exists(P(L"Launcher.exe")) && freeB < g_prof.buildBytes)
                Say(L"Warning: only " + GB(freeB) + L" free; the build needs about " + GB(g_prof.buildBytes) + L".");
        }
    }
    if (g_cancel) goto out;

    // 3. setup.ps1 (copy, decrypt, generate, build), with fewer jobs after an out-of-memory failure
    {
        PutState(L"phase", L"build");
        Machine m = GetMachine();
        int reduce = _wtoi(GetState(L"reduce").c_str());
        Jobs j = PlanJobs(m, reduce);
        SetStep(L"Building");
        { std::lock_guard<std::mutex> lk(g_ui.mx); g_ui.mem = JobsText(m, j); g_ui.dirty = true; }
        Say(JobsText(m, j));
        wstring w = RamWarning(m);
        if (!w.empty()) Say(L"Note: " + w);
        for (;;) {
            wstring msg;
            int r = RunSetupOnce(c, j, msg);
            if (r == 3) { SetStep(L"Stopped. Click Resume to continue where it stopped."); goto out; }
            if (r == 0) break;
            if (r == 2) {
                Jobs k = PlanJobs(m, reduce + 1);
                if (k.ppu == j.ppu && k.game == j.game && k.rpcs3 == j.rpcs3) {
                    Fail(L"The compiler ran out of memory, even with one file at a time.",
                         L"Close other programs, make the Windows page file bigger (System > About > Advanced system settings > "
                         L"Performance > Advanced > Virtual memory: let Windows manage it, or at least 16 GB), then click Resume.");
                    goto out;
                }
                ++reduce;
                PutState(L"reduce", std::to_wstring(reduce));
                j = k;
                Say(Fmt(L"The compiler ran out of memory. Trying again with fewer files at a time (%d big, -j %d). The build continues where it stopped.", j.ppu, j.game));
                { std::lock_guard<std::mutex> lk(g_ui.mx); g_ui.mem = JobsText(m, j) + L" (reduced after running out of memory)"; g_ui.dirty = true; }
                c.game.clear();  // copied already (or setup.ps1 asks for it again with the saved path)
                if (!HaveGameFiles()) c.game = GetState(L"game");
                continue;
            }
            Fail(msg, Hint(msg));
            goto out;
        }
    }

    // 4. finish
    PutState(L"phase", L"done");
    PutState(L"lastError", L"");
    SetStep(L"Done! " + g_prof.name + L" is ready.");
    SetDetail(L"Start the game with the launcher (Start menu or desktop).", 1000);
    MakeShortcuts(c.desktop);
    Say(L"SETUP COMPLETE.");
    g_result = 0;
    if (g_prepare) {
        // the RecompLauncher shows settings.ini: write the values recommended for this PC (never over the player's own)
        if (!Exists(P(L"settings.ini")) && Exists(P(L"Launcher.exe")))
            Run(Arg(P(L"Launcher.exe")) + L" --resave", [](const wstring&) {});
        goto out;
    }
    if (c.launch && Exists(P(L"Launcher.exe"))) ShellExecuteW(nullptr, L"open", P(L"Launcher.exe").c_str(), nullptr, g_port.c_str(), SW_SHOWNORMAL);
    else if (!c.launch) Ask(g_prof.name + L" is ready. Start it with the launcher (Start menu or desktop shortcut).", MB_OK | MB_ICONINFORMATION);
out:
    SetThreadExecutionState(ES_CONTINUOUS);
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

// ---------------------------------------------------------------- dry run (console)
static int DryRun() {
    ConsoleWrite(L"Setup.exe --dry-run: " + g_prof.name + L" (" + g_prof.titleId + L", " + g_prof.region + L"), folder " + g_port + L"\r\n");
    ConsoleWrite(L"Nothing is installed, copied, built or deleted.\r\n\r\n");

    if (!g_opt.parseLog.empty()) {  // progress and out-of-memory parser on an existing log
        std::string b;
        if (!ReadBytes(g_opt.parseLog, b)) { ConsoleWrite(L"Cannot read " + g_opt.parseLog + L"\r\n"); return 1; }
        BuildLog src{L"(log)", L"Building", 30, 98};
        Meter m;
        int lines = 0, updates = 0;
        wstring text = DecodeLog(b);
        for (auto& l : Split(text, L'\n')) { ++lines; if (m.Feed(Trim(l), src)) ++updates; }
        ConsoleWrite(Fmt(L"== Parsed %s: %d lines, %d progress lines, last [%d/%d] = %d.%d%% of the bar, out of memory: %s\r\n\r\n",
                         g_opt.parseLog.c_str(), lines, updates, m.n, m.m, m.Progress() / 10, m.Progress() % 10,
                         LooksLikeOom(text) ? L"YES (would retry with fewer jobs)" : L"no"));
        return 0;
    }

    ConsoleWrite(L"== Build tools (tools\\installer\\prereqs.ps1 -Check)\r\n");
    if (!CheckTools()) ConsoleWrite(L"   the check did not run\r\n");
    for (auto& t : g_tools) ConsoleWrite(Fmt(L"   %-8s %-8s %-14s %s\r\n", t.key.c_str(), t.status.c_str(), t.version.c_str(), t.path.c_str()));
    wstring names;
    Lines ids = MissingIds(&names);
    if (ids.empty()) ConsoleWrite(L"   nothing to install\r\n");
    else {
        ConsoleWrite(L"   would install (one UAC prompt):\r\n" + names);
        ConsoleWrite(L"   elevated command: powershell " + InstallCommandArgs(ids, false) + L"\r\n   its dry run:\r\n");
        Run(PsFile(P(L"tools\\installer\\prereqs.ps1")) + L" -Install -DryRun -Ids " + Arg([&] { wstring s; for (auto& i : ids) s += (s.empty() ? L"" : L",") + i; return s; }()),
            [](const wstring& l) { ConsoleWrite(L"     " + l + L"\r\n"); });
    }

    ConsoleWrite(L"\r\n== Game copy\r\n");
    bool have = HaveGameFiles();
    double gameBytes = 0;
    wstring copyMode;
    ConsoleWrite(have ? L"   gamefiles\\ is complete: setup.ps1 skips the copy\r\n"
                      : Exists(P(L"gamefiles.copying")) ? L"   gamefiles.copying exists: the last copy was interrupted, it is redone\r\n"
                                                         : L"   no game copy in gamefiles\\ yet\r\n");
    wstring game = g_opt.game.empty() ? GetState(L"game") : g_opt.game;
    if (!game.empty()) {
        KV kv = CheckGame(game);
        ConsoleWrite(L"   check of " + game + L":\r\n");
        for (size_t i = 0; i < kv.keys.size(); ++i) if (!kv.vals[i].empty()) ConsoleWrite(L"     " + kv.keys[i] + L" = " + kv.vals[i] + L"\r\n");
        ConsoleWrite(L"   \x2192 " + GameCheckText(kv) + L"\r\n");
        gameBytes = _wtof(kv[L"bytes"].c_str());
        copyMode = kv[L"kind"] == L"folder" && DriveOf(kv[L"root"]) == DriveOf(g_port) ? L"Copy (would ask: Move or Copy)" : L"Copy";
    } else if (!have) ConsoleWrite(L"   (pass --game <iso or folder> to test the check)\r\n");

    ConsoleWrite(L"\r\n== Disk space\r\n");
    double freeB = (double)FreeBytes(g_port);
    double need = g_prof.buildBytes + (have ? 0 : (gameBytes > 0 ? gameBytes : g_prof.minBytes));
    ConsoleWrite(Fmt(L"   %s free on %s; needs about %s (build %s%s)%s\r\n", GB(freeB).c_str(), DriveOf(g_port).c_str(), GB(need).c_str(),
                     GB(g_prof.buildBytes).c_str(), have ? L"" : (L" + game " + GB(gameBytes > 0 ? gameBytes : g_prof.minBytes)).c_str(),
                     freeB < need ? L"  -> NOT ENOUGH" : L""));
    if (!ids.empty()) ConsoleWrite(Fmt(L"   plus about %.0f GB on C: for the tools\r\n", ToGB(ToolsBytes(ids)) + 0.5));

    ConsoleWrite(L"\r\n== Memory and job counts\r\n");
    Machine m = GetMachine();
    int reduce = g_opt.reduce >= 0 ? g_opt.reduce : _wtoi(GetState(L"reduce").c_str());
    Jobs j = PlanJobs(m, reduce);
    ConsoleWrite(L"   " + JobsText(m, j) + Fmt(L"  (%.0f GB per big file, retry level %d)\r\n", j.ppuGB, reduce));
    wstring w = RamWarning(m);
    if (!w.empty()) ConsoleWrite(L"   WARNING: " + w + L"\r\n");
    for (int r = reduce + 1; r <= reduce + 3; ++r) {
        Jobs k = PlanJobs(m, r);
        ConsoleWrite(Fmt(L"   after out-of-memory retry %d: ppu %d, -j %d, rpcs3 -j %d\r\n", r, k.ppu, k.game, k.rpcs3));
        if (k.ppu == 1 && k.game == 1 && k.rpcs3 == 1) break;
    }

    ConsoleWrite(L"\r\n== Resume state (logs\\setup_state.ini)\r\n");
    wstring phase = GetState(L"phase");
    if (phase.empty()) ConsoleWrite(L"   none: a fresh setup\r\n");
    else ConsoleWrite(L"   phase=" + phase + L" game=" + GetState(L"game") + L" copymode=" + GetState(L"copymode") + L" sdks=" + GetState(L"sdks") +
                      L" reduce=" + GetState(L"reduce") + L"\r\n   last error: " + GetState(L"lastError") + L"\r\n");
    ConsoleWrite(L"   button: " + wstring(phase.empty() ? L"Install" : phase == L"done" ? L"Rebuild" : L"Resume") + L"\r\n");
    for (auto& b : BuildLogs()) {
        bool built = Exists(P(b.rel));
        ConsoleWrite(L"   " + b.what + L": " + (built ? L"a previous build log exists; Ninja rebuilds only what is missing or changed" : L"not built yet") + L"\r\n");
    }

    ConsoleWrite(L"\r\n== setup.ps1 command\r\n");
    ConsoleWrite(Fmt(L"   set PORT_PPU_JOBS=%d PORT_JOBS=%d RFOM_JOBS=%d RPCS3_JOBS=%d\r\n", j.ppu, j.game, j.game, j.rpcs3));
    wstring cmd = PsFile(P(L"setup.ps1")) + L" -Unattended";
    if (!have && !game.empty()) cmd += L" -Game " + Arg(game) + L" -GameCopyMode Copy";
    ConsoleWrite(L"   " + cmd + L" -NoOptionalSdks  (or -AcceptSdkLicenses when the SDK box is ticked)\r\n");

    ConsoleWrite(L"\r\n== Shortcuts at the end\r\n   " + StartMenuDir() + L"\\" + g_prof.saveFolder + L".lnk  -> Launcher.exe\r\n   " +
                 StartMenuDir() + L"\\Setup (repair, rebuild, uninstall).lnk  -> Setup.exe\r\n   " + DesktopLink() + L"  -> Launcher.exe (optional)\r\n");
    if (g_opt.uninstall) { ConsoleWrite(L"\r\n== "); Uninstall(); }
    return 0;
}

// ---------------------------------------------------------------- the window
enum {
    ID_GAME = 100, ID_ISO, ID_FOLDER, ID_SDK, ID_LAUNCH, ID_DESKTOP, ID_GO, ID_CANCEL, ID_LOGS, ID_UNINSTALL, ID_HELP, ID_CLOSE,
};
static HWND h_tools, h_game, h_gameSt, h_sdk, h_launch, h_desktop, h_mem, h_prog, h_step, h_detail, h_log, h_go, h_cancel, h_uninst;
static HFONT g_font, g_bold;
static int g_dpi = 96;
static std::atomic<bool> g_busy{false};
static std::thread g_worker;
static int S(int v) { return MulDiv(v, g_dpi, 96); }

static HWND Ctl(HWND p, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f = nullptr) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), p, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
    return c;
}
static wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    wstring s(n, 0);
    if (n) GetWindowTextW(h, &s[0], n + 1);
    return Trim(s);
}
static void UpdateGoButton() {
    wstring phase = GetState(L"phase");
    SetWindowTextW(h_go, phase.empty() ? L"Install" : phase == L"done" ? L"Rebuild" : L"Resume");
}
static void ShowGameState() {
    if (HaveGameFiles()) SetWindowTextW(h_gameSt, (L"\x2713 Your game is already copied into " + P(L"gamefiles") + L". You don't need to pick it again.").c_str());
    else if (Exists(P(L"gamefiles.copying"))) SetWindowTextW(h_gameSt, L"The last copy of the game was interrupted. Pick the same ISO or folder again (it is filled in if Setup remembers it) and click Resume.");
    else SetWindowTextW(h_gameSt, (L"Pick your own copy of " + g_prof.name + L" (" + g_prof.region + L" disc, " + g_prof.titleId + L", " + g_prof.sizeLabel +
                                   L"): a decrypted .iso, or the folder that contains PS3_GAME.").c_str());
}
static void CheckPickedGame(const wstring& path) {
    SetWindowTextW(h_game, path.c_str());
    SetWindowTextW(h_gameSt, L"Checking...");
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    KV kv = CheckGame(path);
    SetCursor(old);
    SetWindowTextW(h_gameSt, GameCheckText(kv).c_str());
}
static void PickIso(HWND w) {
    wchar_t file[MAX_PATH * 4] = {};
    OPENFILENAMEW of{sizeof of};
    of.hwndOwner = w;
    of.lpstrFilter = L"PS3 disc image (*.iso)\0*.iso\0Any file inside the game folder (*.*)\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH * 4;
    of.lpstrTitle = L"Pick your game's ISO";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameW(&of)) CheckPickedGame(file);
}
static void PickFolder(HWND w) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Pick the game folder (the one that contains PS3_GAME)");
    if (SUCCEEDED(d->Show(w))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { CheckPickedGame(p); CoTaskMemFree(p); }
            it->Release();
        }
    }
    d->Release();
}
static void SetBusy(bool b) {
    g_busy = b;
    for (HWND h : {h_go, h_uninst, h_game, h_sdk, h_launch, h_desktop}) EnableWindow(h, !b);
    EnableWindow(GetDlgItem(g_hwnd, ID_ISO), !b);
    EnableWindow(GetDlgItem(g_hwnd, ID_FOLDER), !b);
    EnableWindow(h_cancel, b);
}
static void StopWorker() {
    g_cancel = true;
    if (g_job) TerminateJobObject(g_job, 1);
    if (g_worker.joinable()) g_worker.join();
    NewJob();
    g_cancel = false;
}
static void AppendLog(const std::deque<wstring>& ls) {
    wstring add;
    for (auto& l : ls) add += l + L"\r\n";
    int len = GetWindowTextLengthW(h_log);
    if (len > 200000) { SendMessageW(h_log, EM_SETSEL, 0, 100000); SendMessageW(h_log, EM_REPLACESEL, FALSE, (LPARAM)L""); len = GetWindowTextLengthW(h_log); }
    SendMessageW(h_log, EM_SETSEL, len, len);
    SendMessageW(h_log, EM_REPLACESEL, FALSE, (LPARAM)add.c_str());
}

static LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = w;
        Ctl(w, L"STATIC", (g_prof.name + L" \x2014 PC port setup").c_str(), 0, 12, 10, 676, 28, 0, g_bold);
        Ctl(w, L"STATIC", (L"Builds the port on this PC from your own copy of the game. Folder: " + g_port).c_str(), SS_PATHELLIPSIS, 12, 40, 676, 20, 0);
        Ctl(w, L"BUTTON", L"1   Build tools", BS_GROUPBOX, 12, 64, 676, 80, 0);
        h_tools = Ctl(w, L"STATIC", L"Checking the build tools...", 0, 24, 84, 652, 54, 0);
        Ctl(w, L"BUTTON", L"2   Your game", BS_GROUPBOX, 12, 150, 676, 116, 0);
        h_game = Ctl(w, L"EDIT", (g_opt.game.empty() ? GetState(L"game") : g_opt.game).c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 24, 172, 452, 24, ID_GAME);
        Ctl(w, L"BUTTON", L"ISO...", WS_TABSTOP, 484, 171, 90, 26, ID_ISO);
        Ctl(w, L"BUTTON", L"Folder...", WS_TABSTOP, 582, 171, 94, 26, ID_FOLDER);
        h_gameSt = Ctl(w, L"STATIC", L"", 0, 24, 202, 652, 58, 0);
        h_sdk = Ctl(w, L"BUTTON", L"Also download the optional NVIDIA DLSS and Intel XeSS SDKs (I accept NVIDIA's and Intel's SDK licences; AMD FSR works without them)",
                    BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, 12, 272, 676, 34, ID_SDK);
        h_desktop = Ctl(w, L"BUTTON", L"Desktop shortcut", BS_AUTOCHECKBOX | WS_TABSTOP, 12, 308, 200, 22, ID_DESKTOP);
        h_launch = Ctl(w, L"BUTTON", L"Start the launcher when done (it can compile the shaders before you play)", BS_AUTOCHECKBOX | WS_TABSTOP, 220, 308, 468, 22, ID_LAUNCH);
        SendMessageW(h_desktop, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(h_launch, BM_SETCHECK, BST_CHECKED, 0);
        if (GetState(L"sdks") == L"accept") SendMessageW(h_sdk, BM_SETCHECK, BST_CHECKED, 0);
        Ctl(w, L"BUTTON", L"3   Build", BS_GROUPBOX, 12, 336, 676, 112, 0);
        Machine m = GetMachine();
        Jobs j = PlanJobs(m, _wtoi(GetState(L"reduce").c_str()));
        wstring rw = RamWarning(m);
        h_mem = Ctl(w, L"STATIC", (JobsText(m, j) + (rw.empty() ? L"" : L"\r\n\x26A0 " + rw)).c_str(), 0, 24, 354, 652, 34, 0);
        h_prog = Ctl(w, PROGRESS_CLASSW, L"", PBS_SMOOTH, 24, 390, 652, 16, 0);
        SendMessageW(h_prog, PBM_SETRANGE32, 0, 1000);
        h_step = Ctl(w, L"STATIC", GetState(L"lastError").empty() ? L"Ready." : (L"Last time: " + GetState(L"lastError")).c_str(), SS_ENDELLIPSIS, 24, 410, 652, 18, 0, g_bold);
        h_detail = Ctl(w, L"STATIC", L"", SS_ENDELLIPSIS, 24, 428, 652, 18, 0);
        h_log = Ctl(w, L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 12, 454, 676, 118, 0);
        h_go = Ctl(w, L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, 12, 580, 120, 30, ID_GO, g_bold);
        h_cancel = Ctl(w, L"BUTTON", L"Stop", WS_TABSTOP, 140, 580, 90, 30, ID_CANCEL);
        Ctl(w, L"BUTTON", L"Open logs", WS_TABSTOP, 238, 580, 100, 30, ID_LOGS);
        h_uninst = Ctl(w, L"BUTTON", L"Uninstall...", WS_TABSTOP, 346, 580, 100, 30, ID_UNINSTALL);
        Ctl(w, L"BUTTON", L"Help", WS_TABSTOP, 454, 580, 80, 30, ID_HELP);
        Ctl(w, L"BUTTON", L"Close", WS_TABSTOP, 588, 580, 100, 30, ID_CLOSE);
        EnableWindow(h_cancel, FALSE);
        UpdateGoButton();
        ShowGameState();
        if (!g_opt.game.empty() && !HaveGameFiles())  // the installer passes the copy picked in its wizard
            PostMessageW(w, WM_APP_PREFILL, 0, 0);
        g_busy = true;
        EnableWindow(h_go, FALSE);
        std::thread([] { CheckTools(); if (g_hwnd) PostMessageW(g_hwnd, WM_APP_TOOLS, 0, 0); }).detach();
        return 0;
    }
    case WM_APP_PREFILL:
        CheckPickedGame(g_opt.game);
        return 0;
    case WM_APP_TOOLS:
        SetWindowTextW(h_tools, ToolsText().c_str());
        if (!g_worker.joinable()) { g_busy = false; EnableWindow(h_go, TRUE); }
        return 0;
    case WM_APP_UI: {
        std::deque<wstring> ls;
        wstring step, detail, tools, mem;
        int prog;
        bool dirty;
        {
            std::lock_guard<std::mutex> lk(g_ui.mx);
            ls.swap(g_ui.lines);
            dirty = g_ui.dirty;
            g_ui.dirty = false;
            step = g_ui.step; detail = g_ui.detail; tools = g_ui.tools; mem = g_ui.mem; prog = g_ui.progress;
        }
        if (!ls.empty()) AppendLog(ls);
        if (dirty) {
            if (!step.empty()) SetWindowTextW(h_step, step.c_str());
            SetWindowTextW(h_detail, detail.c_str());
            if (!tools.empty()) SetWindowTextW(h_tools, tools.c_str());
            if (!mem.empty()) SetWindowTextW(h_mem, mem.c_str());
            SendMessageW(h_prog, PBM_SETPOS, prog, 0);
        }
        return 0;
    }
    case WM_APP_DONE:
        if (g_worker.joinable()) g_worker.join();
        SetBusy(false);
        UpdateGoButton();
        ShowGameState();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ISO: PickIso(w); return 0;
        case ID_FOLDER: PickFolder(w); return 0;
        case ID_GO: {
            if (g_busy) return 0;
            Choice c;
            c.game = GetText(h_game);
            if (!c.game.empty() && c.game != GetState(L"game")) PutState(L"copymode", L"");
            else c.copyMode = GetState(L"copymode");
            c.sdks = SendMessageW(h_sdk, BM_GETCHECK, 0, 0) == BST_CHECKED;
            c.launch = SendMessageW(h_launch, BM_GETCHECK, 0, 0) == BST_CHECKED;
            c.desktop = SendMessageW(h_desktop, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!HaveGameFiles() && c.game.empty()) {
                Ask(L"Pick your copy of " + g_prof.name + L" first: click ISO... or Folder...", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (!g_opt.dryRun) {
                Machine m = GetMachine();
                if (m.totalGB < 15.5 && Ask(RamWarning(m) + L"\r\n\r\nContinue?", MB_OKCANCEL | MB_ICONWARNING) != IDOK) return 0;
            }
            SetBusy(true);
            g_worker = std::thread(SetupWorker, c);
            return 0;
        }
        case ID_CANCEL:
            if (g_busy && Ask(L"Stop the setup? Click Resume later to continue where it stopped.", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                SetStep(L"Stopping...");
                g_cancel = true;
                if (g_job) TerminateJobObject(g_job, 1);
            }
            return 0;
        case ID_LOGS: ShellExecuteW(w, L"explore", P(L"logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL); return 0;
        case ID_HELP: ShellExecuteW(w, L"open", P(L"docs\\INSTALL.md").c_str(), nullptr, nullptr, SW_SHOWNORMAL); return 0;
        case ID_UNINSTALL:
            if (!g_busy) { Uninstall(); UpdateGoButton(); ShowGameState(); }
            return 0;
        case ID_CLOSE: SendMessageW(w, WM_CLOSE, 0, 0); return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == h_gameSt || (HWND)lp == h_step) {
            wchar_t c0[2] = {};
            GetWindowTextW((HWND)lp, c0, 2);
            HDC dc = (HDC)wp;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, c0[0] == 0x2717 || StartsWith(GetText((HWND)lp), L"Stopped") || StartsWith(GetText((HWND)lp), L"Last time") ? RGB(190, 0, 0)
                             : c0[0] == 0x2713 || StartsWith(GetText((HWND)lp), L"Done") ? RGB(0, 128, 0) : GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_CLOSE:
        if (g_busy && g_worker.joinable()) {
            if (Ask(L"Setup is still running. Stop it and close? Run Setup.exe again later and click Resume: it continues where it stopped.",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return 0;
            StopWorker();
        }
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        g_hwnd = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

// ---------------------------------------------------------------- main
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    g_port = exe;
    g_port.erase(g_port.find_last_of(L'\\'));

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        wstring a = Lower(argv[i]);
        auto next = [&]() -> wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--help" || a == L"-h" || a == L"/?") g_opt.help = true;
        else if (a == L"--dry-run") g_opt.dryRun = true;
        else if (a == L"--uninstall") g_opt.uninstall = true;
        else if (a == L"--prepare") g_opt.prepare = true;
        else if (a == L"--game") g_opt.game = next();
        else if (a == L"--ram-gb") g_opt.ramGB = _wtof(next().c_str());
        else if (a == L"--free-gb") g_opt.freeGB = _wtof(next().c_str());
        else if (a == L"--cores") g_opt.cores = _wtoi(next().c_str());
        else if (a == L"--ppu-gb") g_opt.ppuGB = _wtof(next().c_str());
        else if (a == L"--reduce") g_opt.reduce = _wtoi(next().c_str());
        else if (a == L"--parse-log") { g_opt.parseLog = next(); g_opt.dryRun = true; }
        else if (a == L"--port") g_port = argv[i + 1 < argc ? ++i : i];  // testing: act on another port folder
    }
    LocalFree(argv);
    if (!g_port.empty() && g_port.back() == L'\\') g_port.pop_back();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (g_opt.help) {
        OpenConsole();
        ConsoleWrite(L"Setup.exe: one-click setup of this PC port (window). Options:\r\n"
                     L"  --dry-run            check this PC and print what Setup would do; changes nothing\r\n"
                     L"  --game <iso|folder>  fill in the game copy (the installer passes it); with --dry-run: also check it\r\n"
                     L"  --uninstall          remove what Setup made (with --dry-run: only list it)\r\n"
                     L"  --prepare            no window (RecompLauncher): install tools, copy the game (--game) and build;\r\n"
                     L"                       prints \"progress <0..1> <text>\" lines; exit code 0 = ready to play\r\n"
                     L"  --ram-gb N, --free-gb N, --cores N, --ppu-gb N, --reduce N\r\n"
                     L"                       pretend this RAM / free RAM / core count / GB per big file / out-of-memory retry\r\n"
                     L"                       level when planning the build job counts (also for a real run)\r\n"
                     L"  --parse-log <log>    dry run of the build-progress and out-of-memory parser on a build log\r\n"
                     L"  --port <folder>      act on another port folder (testing)\r\n"
                     L"Unattended setup without the window: setup.ps1 -Unattended -Game <iso|folder> [-AcceptSdkLicenses | -NoOptionalSdks]\r\n"
                     L"(set PORT_PPU_JOBS, PORT_JOBS, RFOM_JOBS and RPCS3_JOBS to limit the build; see docs\\INSTALL.md).\r\n");
        return 0;
    }
    if (g_opt.dryRun) OpenConsole();
    if (g_opt.prepare && !g_opt.dryRun) { g_prepare = true; OpenConsole(); }
    if (!LoadProfile() || !Exists(P(L"setup.ps1"))) {
        wstring t = L"Setup.exe must be in the port folder, next to setup.ps1 and the tools folder (" + g_port +
                    L").\r\n\r\nUnzip the whole release zip first, then run Setup.exe from the unzipped folder.";
        if (g_console) ConsoleWrite(t + L"\r\n"); else MessageBoxW(nullptr, t.c_str(), L"Setup", MB_OK | MB_ICONERROR);
        return 1;
    }
    NewJob();
    if (g_opt.dryRun) {
        OpenLog(L"setup_dryrun.log");
        int r = DryRun();
        if (g_log) fclose(g_log);
        return r;
    }

    // one Setup.exe per port folder at a time
    wstring mx = L"Local\\ResistancePortSetup_" + g_prof.titleId;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mx.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"ResistancePortSetup", nullptr);
        if (other) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        if (g_prepare) { ConsoleWrite(L"Setup is already running for this game. Finish or close it, then try again.\r\n"); return 1; }
        return 0;
    }
    OpenLog(L"setup.log");
    if (g_prepare) {
        Choice c;
        c.game = g_opt.game;
        c.launch = false;
        c.desktop = false;  // the launcher starts the game; a Start menu entry is still made
        wstring sdk = GetState(L"sdks");
        if (sdk.empty())
            sdk = MessageBoxW(nullptr, (L"Also download the optional NVIDIA DLSS and Intel XeSS SDKs for " + g_prof.name +
                                        L"?\r\n\r\nYes = I accept NVIDIA's and Intel's SDK licences.\r\nNo = skip them (AMD FSR works without them).").c_str(),
                              (g_prof.name + L" setup").c_str(), MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDYES
                      ? L"accept" : L"skip";
        c.sdks = sdk == L"accept";
        SetupWorker(c);
        if (g_result) ConsoleWrite(L"Setup did not finish. Details: " + P(L"logs\\setup.log") + L"\r\n");
        if (g_log) fclose(g_log);
        if (mutex) CloseHandle(mutex);
        return g_result;
    }
    if (g_opt.uninstall) {
        int r = Uninstall();
        if (g_log) fclose(g_log);
        return r;
    }

    INITCOMMONCONTROLSEX ic{sizeof ic, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    NONCLIENTMETRICSW nm{sizeof nm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0);
    g_font = CreateFontIndirectW(&nm.lfMessageFont);
    LOGFONTW bl = nm.lfMessageFont;
    bl.lfWeight = FW_BOLD;
    g_bold = CreateFontIndirectW(&bl);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"ResistancePortSetup";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    RECT r{0, 0, S(700), S(622)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    HWND w = CreateWindowExW(0, wc.lpszClassName, (g_prof.name + L" - Setup").c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(w, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(w, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    if (g_worker.joinable()) StopWorker();
    if (g_log) fclose(g_log);
    if (mutex) CloseHandle(mutex);
    return 0;
}
