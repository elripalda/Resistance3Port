/* Resistance PC ports: the "Report a problem" bundler and its launcher UI.
 * See port_report.h. Shared by the R1/R2/R3 ports -- keep the copies identical.
 *
 * The zip (Desktop\<Game>_report_<yyyymmdd_hhmmss>.zip):
 *   report.txt        what this is: game, version, git commit, exe builds,
 *                     Windows, CPU, RAM, monitors, GPUs + driver versions (DXGI
 *                     and Vulkan), the last crash line, what was left out
 *   note.txt          the tester's description (when one was typed)
 *   crash\            the newest dump (.dmp) and its readable report (.txt),
 *                     the other recent reports (.txt), crashes.txt
 *   logs\             run.log, out.log, launcher.log, ... (the last 8 MB of each)
 *   settings\         settings.ini, settings.bat, rpcs3_config.yml, controls.txt,
 *                     mouselook*.cfg, run.bat
 *   saves\            only when "Include my save files" is ticked
 * Never: game data, shader caches, builds, anything not on the lists above.
 * Everything packed is at most 50 MB (before compression); what does not fit is
 * left out and listed in report.txt. The Windows user name and profile path are
 * replaced by <user> / %USERPROFILE% in the text files. The zip is made by the
 * tar.exe that ships with Windows 10 1803+ (deflate); without it, by the
 * stored-zip writer below. */
#include "port_report.h"
#ifdef _WIN32
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <dxgi.h>
#include <intrin.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

namespace port_report {

static std::string fmt(const char* f, ...)
{
    char b[2048];
    va_list a; va_start(a, f);
    int n = vsnprintf(b, sizeof b, f, a);
    va_end(a);
    if (n < 0) return std::string();
    return std::string(b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}
static std::wstring widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string narrow(const std::wstring& w, UINT cp = CP_UTF8)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(cp, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s((size_t)n, 0);
    WideCharToMultiByte(cp, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}
static bool exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool is_dir(const std::string& p)
{
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static ULONGLONG ft64(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static std::string stamp(bool file_form)
{
    SYSTEMTIME t; GetLocalTime(&t);
    return fmt(file_form ? "%04u%02u%02u_%02u%02u%02u" : "%04u-%02u-%02u %02u:%02u:%02u",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}
static std::string ftime(const FILETIME& f)
{
    FILETIME lt; SYSTEMTIME t;
    FileTimeToLocalFileTime(&f, &lt); FileTimeToSystemTime(&lt, &t);
    return fmt("%04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
}

/* whole file, or its last `tail` bytes; shared read so a running game's logs work too */
static bool read_file(const std::string& p, std::string& out, uint64_t tail = 0, uint64_t* full_size = NULL)
{
    HANDLE h = CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    uint64_t n = (uint64_t)sz.QuadPart, from = 0;
    if (full_size) *full_size = n;
    if (tail && n > tail) { from = n - tail; n = tail; }
    bool ok = n < 0x7FFFFFF0ull;
    if (ok) {
        out.assign((size_t)n, 0);
        LARGE_INTEGER o; o.QuadPart = (LONGLONG)from; SetFilePointerEx(h, o, NULL, FILE_BEGIN);
        size_t got = 0;
        while (got < n) {
            DWORD r = 0;
            const DWORD want = (DWORD)(n - got < (1u << 24) ? n - got : (1u << 24));
            if (!ReadFile(h, &out[got], want, &r, NULL) || !r) break;
            got += r;
        }
        out.resize(got);
        if (from) {   /* start at a whole line */
            const size_t nl = out.find('\n');
            if (nl != std::string::npos && nl < 4096) out.erase(0, nl + 1);
            out = fmt("[... first %llu bytes left out ...]\r\n", (unsigned long long)from) + out;
        }
    }
    CloseHandle(h);
    return ok;
}
static bool write_file(const std::string& p, const std::string& data)
{
    HANDLE h = CreateFileA(p.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &w, NULL) && w == data.size());
    CloseHandle(h);
    return ok;
}
static void make_dirs(const std::string& p)
{
    for (size_t i = 3; i <= p.size(); i++)
        if (i == p.size() || p[i] == '\\') CreateDirectoryA(p.substr(0, i).c_str(), NULL);
}
static void remove_tree(const std::string& dir)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            const std::string p = dir + "\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
            else { SetFileAttributesA(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileA(p.c_str()); }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
}

uint64_t dir_size(const std::string& dir, uint64_t stop_after)
{
    uint64_t total = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += dir_size(dir + "\\" + fd.cFileName, stop_after - total);
        else total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (total <= stop_after && FindNextFileA(h, &fd));
    FindClose(h);
    return total;
}

/* The game folder: the launcher's folder, or its parent for a test build in a
 * subfolder (build_qa\); same rule as the old crash notice. */
std::string find_root(const char* exe_dir)
{
    std::string d = exe_dir && *exe_dir ? exe_dir : "";
    if (d.empty()) {
        char p[MAX_PATH]; GetModuleFileNameA(NULL, p, sizeof p);
        if (char* s = strrchr(p, '\\')) *s = 0;
        d = p;
    }
    while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    if (!exists(d + "\\run.bat") && !exists(d + "\\logs")) {
        const size_t s = d.rfind('\\');
        if (s != std::string::npos && (exists(d.substr(0, s) + "\\run.bat") || exists(d.substr(0, s) + "\\logs")))
            d.resize(s);
    }
    return d;
}

std::string detect_game(const std::string& root)
{
    static const char* known[][2] = {
        { "build\\Resistance3.exe", "Resistance3" }, { "build\\Resistance2.exe", "Resistance2" },
        { "build\\ResistanceFoMGame.exe", "ResistanceFoM" }, { "build\\ResistanceFoM.exe", "ResistanceFoM" },
    };
    for (auto& k : known) if (exists(root + "\\" + k[0])) return k[1];
    std::string n = root.substr(root.rfind('\\') + 1);
    if (n.size() > 4 && !_stricmp(n.c_str() + n.size() - 4, "Port")) n.resize(n.size() - 4);
    std::string clean;
    for (char c : n) if (isalnum((unsigned char)c) || c == '-' || c == '_') clean += c;
    return clean.empty() ? "Resistance" : clean;
}

std::vector<std::string> save_dirs(const std::string& root, const std::string& game)
{
    std::vector<std::string> v;
    auto add = [&](const std::string& gd) {
        const std::string p = gd + "\\dev_hdd0\\home\\00000001\\savedata";
        if (!is_dir(p)) return;
        for (const std::string& q : v) if (!_stricmp(q.c_str(), p.c_str())) return;
        v.push_back(p);
    };
    if (const char* e = getenv("PS3_GAMEDATA_ROOT")) if (*e) add(e);
    if (game == "ResistanceFoM") {   /* rfom_pcport.inc: Saved Games unless RFOM_SAVE_DIR=portable */
        PWSTR sg = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, NULL, &sg)) && sg)
            add(narrow(sg, CP_ACP) + "\\Resistance Fall of Man");
        if (sg) CoTaskMemFree(sg);
        if (const char* e = getenv("RFOM_SAVE_DIR")) if (*e && _stricmp(e, "portable")) add(e);
    }
    add(root + "\\gamedata");
    return v;
}

/* ---- system information ---- */
static std::string reg_str(HKEY root, const char* key, const char* val)
{
    char b[256]; DWORD n = sizeof b;
    if (RegGetValueA(root, key, val, RRF_RT_REG_SZ, NULL, b, &n) == ERROR_SUCCESS) return b;
    return std::string();
}
static DWORD reg_dw(HKEY root, const char* key, const char* val)
{
    DWORD v = 0, n = sizeof v;
    return RegGetValueA(root, key, val, RRF_RT_REG_DWORD, NULL, &v, &n) == ERROR_SUCCESS ? v : 0;
}

/* Vulkan, without the SDK headers: just the bits read here. */
typedef void* VkInst;
typedef void* VkPhys;
struct VkAppInfo { int sType; const void* pNext; const char* app; uint32_t appv; const char* eng; uint32_t engv; uint32_t api; };
struct VkInstInfo { int sType; const void* pNext; uint32_t flags; const VkAppInfo* app; uint32_t nl; const char* const* l;
                    uint32_t ne; const char* const* e; };
struct VkDriverProps { int sType; void* pNext; uint32_t id; char name[256]; char info[256]; uint8_t conf[4]; };
struct VkProps2 { int sType; void* pNext; unsigned char props[1024]; };   /* VkPhysicalDeviceProperties is 824 bytes */
typedef int (__stdcall* vkCreateInstance_t)(const VkInstInfo*, const void*, VkInst*);
typedef void (__stdcall* vkDestroyInstance_t)(VkInst, const void*);
typedef int (__stdcall* vkEnumPhys_t)(VkInst, uint32_t*, VkPhys*);
typedef void (__stdcall* vkGetProps_t)(VkPhys, void*);
typedef void* (__stdcall* vkGetInstanceProcAddr_t)(VkInst, const char*);

static std::string vk_version(uint32_t v) { return fmt("%u.%u.%u", v >> 22, (v >> 12) & 0x3FF, v & 0xFFF); }
static std::string vk_driver_version(uint32_t vendor, uint32_t v)
{
    if (vendor == 0x10DE) return fmt("%u.%02u", (v >> 22) & 0x3FF, (v >> 14) & 0xFF);   /* NVIDIA */
    if (vendor == 0x8086) return fmt("%u.%u", v >> 14, v & 0x3FFF);                     /* Intel (Windows) */
    return vk_version(v);
}

static void vulkan_info_inner(std::string& s, HMODULE vk)
{
    auto gipa = (vkGetInstanceProcAddr_t)GetProcAddress(vk, "vkGetInstanceProcAddr");
    if (!gipa) return;
    auto create = (vkCreateInstance_t)gipa(NULL, "vkCreateInstance");
    if (!create) return;
    VkAppInfo app = { 0, NULL, "port_report", 1, "none", 1, (1u << 22) | (1u << 12) };   /* 1.1: properties2 */
    VkInstInfo ii = { 1, NULL, 0, &app, 0, NULL, 0, NULL };
    VkInst inst = NULL;
    const int rc = create(&ii, NULL, &inst);
    if (rc != 0 || !inst) { s += fmt("Vulkan: vkCreateInstance failed (%d)\r\n", rc); return; }
    auto destroy = (vkDestroyInstance_t)gipa(inst, "vkDestroyInstance");
    auto enumerate = (vkEnumPhys_t)gipa(inst, "vkEnumeratePhysicalDevices");
    auto props = (vkGetProps_t)gipa(inst, "vkGetPhysicalDeviceProperties");
    auto props2 = (vkGetProps_t)gipa(inst, "vkGetPhysicalDeviceProperties2");
    VkPhys devs[8]; uint32_t n = 8;
    if (enumerate && props && enumerate(inst, &n, devs) >= 0) {
        for (uint32_t i = 0; i < n && i < 8; i++) {
            VkDriverProps dp; memset(&dp, 0, sizeof dp); dp.sType = 1000196000;
            VkProps2 p2; memset(&p2, 0, sizeof p2); p2.sType = 1000059001; p2.pNext = &dp;
            if (props2) props2(devs[i], &p2); else props(devs[i], p2.props);
            const unsigned char* p = p2.props;
            uint32_t api, drv, vendor, device, type;
            memcpy(&api, p, 4); memcpy(&drv, p + 4, 4); memcpy(&vendor, p + 8, 4); memcpy(&device, p + 12, 4); memcpy(&type, p + 16, 4);
            char name[257]; memcpy(name, p + 20, 256); name[256] = 0;
            static const char* types[] = { "other", "integrated", "discrete", "virtual", "cpu" };
            s += fmt("Vulkan GPU %u: %s [%04X:%04X, %s] Vulkan %s, driver %s", i, name, vendor, device,
                     type < 5 ? types[type] : "?", vk_version(api).c_str(), vk_driver_version(vendor, drv).c_str());
            dp.name[255] = dp.info[255] = 0;
            if (dp.name[0]) s += fmt(" (%s %s)", dp.name, dp.info);
            s += "\r\n";
        }
    }
    if (destroy) destroy(inst, NULL);
}
static void vulkan_info(std::string& s)
{
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { s += "Vulkan: no vulkan-1.dll (no Vulkan driver installed)\r\n"; return; }
    __try { vulkan_info_inner(s, vk); }
    __except (EXCEPTION_EXECUTE_HANDLER) { s += "Vulkan: the driver faulted while being queried\r\n"; }
    /* vulkan-1.dll stays loaded: some layers do not like being unloaded */
}

static void dxgi_info(std::string& s)
{
    HMODULE dx = LoadLibraryA("dxgi.dll");
    if (!dx) return;
    typedef HRESULT (WINAPI* cf_t)(REFIID, void**);
    auto cf = (cf_t)GetProcAddress(dx, "CreateDXGIFactory1");
    IDXGIFactory1* f = NULL;
    if (!cf || FAILED(cf(__uuidof(IDXGIFactory1), (void**)&f)) || !f) return;
    IDXGIAdapter1* a = NULL;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(a->GetDesc1(&d))) {
            LARGE_INTEGER umd; umd.QuadPart = 0;
            std::string ver = "?";
            if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
                ver = fmt("%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart));
            s += fmt("GPU %u: %s [%04X:%04X] %llu MB VRAM, driver %s%s\r\n", i, narrow(d.Description).c_str(), d.VendorId,
                     d.DeviceId, (unsigned long long)(d.DedicatedVideoMemory >> 20), ver.c_str(),
                     (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
        }
        a->Release();
    }
    f->Release();
}

static std::string exe_info(const std::string& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) return std::string();
    std::string s = fmt("%s  %s  %llu bytes", path.substr(path.rfind('\\') + 1).c_str(), ftime(fa.ftLastWriteTime).c_str(),
                        ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
    DWORD dummy = 0;
    const DWORD vs = GetFileVersionInfoSizeA(path.c_str(), &dummy);
    if (vs) {
        std::vector<char> vb(vs);
        VS_FIXEDFILEINFO* fi = NULL; UINT fl = 0;
        if (GetFileVersionInfoA(path.c_str(), 0, vs, vb.data()) && VerQueryValueA(vb.data(), "\\", (void**)&fi, &fl) && fi)
            s += fmt("  version %u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                     HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    }
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        unsigned char b[4096]; DWORD got = 0;
        if (ReadFile(h, b, sizeof b, &got, NULL) && got >= 0x40) {
            const LONG pe = *(const LONG*)(b + 0x3C);
            if (pe > 0 && (DWORD)pe + 8 < got && !memcmp(b + pe, "PE\0\0", 4))
                s += fmt("  PE timestamp 0x%08lX", (unsigned long)*(const DWORD*)(b + pe + 8));
        }
        CloseHandle(h);
    }
    return s;
}

static std::string git_info(const std::string& root)
{
    std::string head;
    if (!read_file(root + "\\.git\\HEAD", head)) return std::string();
    while (!head.empty() && (head.back() == '\n' || head.back() == '\r' || head.back() == ' ')) head.pop_back();
    if (head.compare(0, 5, "ref: ")) return head.substr(0, 12);
    const std::string ref = head.substr(5);
    std::string br = ref.compare(0, 11, "refs/heads/") ? ref : ref.substr(11), h;
    std::string rp = ref; std::replace(rp.begin(), rp.end(), '/', '\\');
    if (read_file(root + "\\.git\\" + rp, h)) return h.substr(0, 12) + " (" + br + ")";
    std::string packed;
    if (read_file(root + "\\.git\\packed-refs", packed)) {
        const size_t at = packed.find(" " + ref);
        if (at != std::string::npos && at >= 40) return packed.substr(at - 40, 12) + " (" + br + ")";
    }
    return br;
}

std::string system_info(const std::string& root)
{
    std::string s;
    /* Windows */
    typedef LONG (WINAPI* rgv_t)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi; memset(&vi, 0, sizeof vi); vi.dwOSVersionInfoSize = sizeof vi;
    if (auto fn = (rgv_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion")) fn(&vi);
    const char* cv = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string product = reg_str(HKEY_LOCAL_MACHINE, cv, "ProductName");
    if (vi.dwBuildNumber >= 22000 && product.find("Windows 10") == 0) product.replace(8, 2, "11");   /* the registry still says 10 */
    s += fmt("Windows:   %s %s (%lu.%lu build %lu.%lu)\r\n", product.c_str(), reg_str(HKEY_LOCAL_MACHINE, cv, "DisplayVersion").c_str(),
             vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber, reg_dw(HKEY_LOCAL_MACHINE, cv, "UBR"));
    /* CPU, RAM */
    int r[4]; char brand[49] = {0};
    __cpuid(r, 0x80000000);
    if ((unsigned)r[0] >= 0x80000004u) for (int i = 0; i < 3; i++) { __cpuid(r, 0x80000002 + i); memcpy(brand + 16 * i, r, 16); }
    const char* bp = brand; while (*bp == ' ') bp++;
    __cpuid(r, 7);
    const bool avx2 = (r[1] >> 5) & 1, avx512 = (r[1] >> 16) & 1;
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
    s += fmt("CPU:       %s, %lu logical processors%s%s\r\n", bp, si.dwNumberOfProcessors, avx2 ? ", AVX2" : "", avx512 ? ", AVX-512" : "");
    s += fmt("RAM:       %.1f GB (%.1f GB free now)\r\n", ms.ullTotalPhys / 1073741824.0, ms.ullAvailPhys / 1073741824.0);
    /* monitors */
    DISPLAY_DEVICEA dd; dd.cb = sizeof dd;
    for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); i++, dd.cb = sizeof dd) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        DEVMODEA m; memset(&m, 0, sizeof m); m.dmSize = sizeof m;
        EnumDisplaySettingsA(dd.DeviceName, ENUM_CURRENT_SETTINGS, &m);
        s += fmt("Display:   %s %lux%lu @ %lu Hz on %s%s\r\n", dd.DeviceName, m.dmPelsWidth, m.dmPelsHeight, m.dmDisplayFrequency,
                 dd.DeviceString, (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " (primary)" : "");
    }
    dxgi_info(s);
    vulkan_info(s);
    /* the port */
    std::string ver;
    if (read_file(root + "\\VERSION", ver)) { while (!ver.empty() && (ver.back() == '\n' || ver.back() == '\r')) ver.pop_back(); }
    s += fmt("Version:   %s\r\n", ver.empty() ? "?" : ver.c_str());
    const std::string git = git_info(root);
    if (!git.empty()) s += fmt("Git:       %s\r\n", git.c_str());
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + "\\build\\*.exe").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do s += "Exe:       build\\" + exe_info(root + "\\build\\" + fd.cFileName) + "\r\n";
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    h = FindFirstFileA((root + "\\*.exe").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do if (!strstr(fd.cFileName, "unins") && !strstr(fd.cFileName, "Setup"))
               s += "Exe:       " + exe_info(root + "\\" + fd.cFileName) + "\r\n";
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return s;
}

/* ---- privacy: the user name and profile path out of text files ---- */
static void replace_all_ci(std::string& s, const std::string& what, const std::string& with)
{
    if (what.size() < 3) return;
    for (size_t i = 0; i + what.size() <= s.size();) {
        if (!_strnicmp(s.c_str() + i, what.c_str(), what.size())) { s.replace(i, what.size(), with); i += with.size(); }
        else i++;
    }
}
static void scrub(std::string& s)
{
    char prof[MAX_PATH] = "", user[256] = "";
    GetEnvironmentVariableA("USERPROFILE", prof, sizeof prof);
    GetEnvironmentVariableA("USERNAME", user, sizeof user);
    if (prof[0]) {
        replace_all_ci(s, prof, "%USERPROFILE%");
        std::string fwd = prof; std::replace(fwd.begin(), fwd.end(), '\\', '/');
        replace_all_ci(s, fwd, "%USERPROFILE%");
    }
    if (strlen(user) >= 3) {
        /* only as a path part / whole word, not inside other words */
        const std::string u = user;
        for (size_t i = 0; i + u.size() <= s.size();) {
            const bool l = i == 0 || !isalnum((unsigned char)s[i - 1]);
            const bool r = i + u.size() == s.size() || !isalnum((unsigned char)s[i + u.size()]);
            if (l && r && !_strnicmp(s.c_str() + i, u.c_str(), u.size())) { s.replace(i, u.size(), "<user>"); i += 6; }
            else i++;
        }
    }
}

/* ---- the zip: tar.exe (Windows 10 1803+), else stored entries ---- */
static unsigned s_crc[256];
static unsigned crc32(unsigned c, const unsigned char* p, size_t n)
{
    if (!s_crc[1]) for (unsigned i = 0; i < 256; i++) { unsigned k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1; s_crc[i] = k; }
    c = ~c;
    while (n--) c = s_crc[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void put16(std::string& s, unsigned v) { s += (char)(v & 0xFF); s += (char)(v >> 8 & 0xFF); }
static void put32(std::string& s, unsigned v) { put16(s, v & 0xFFFF); put16(s, v >> 16); }

static void list_tree(const std::string& dir, const std::string& rel, std::vector<std::string>& out)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        const std::string r = rel.empty() ? fd.cFileName : rel + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) list_tree(dir + "\\" + fd.cFileName, r, out);
        else out.push_back(r);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static bool stored_zip(const std::string& staging, const std::wstring& zip)
{
    std::vector<std::string> files;
    list_tree(staging, "", files);
    FILE* f = _wfopen(zip.c_str(), L"wb");
    if (!f) return false;
    std::string central;
    unsigned count = 0;
    bool ok = true;
    for (const std::string& rel : files) {
        std::string data;
        if (!read_file(staging + "\\" + rel, data)) continue;
        std::string name = rel; std::replace(name.begin(), name.end(), '\\', '/');
        WIN32_FILE_ATTRIBUTE_DATA fa; WORD dd = 0x21, dt = 0;
        FILETIME lt;
        if (GetFileAttributesExA((staging + "\\" + rel).c_str(), GetFileExInfoStandard, &fa) && FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt))
            FileTimeToDosDateTime(&lt, &dd, &dt);
        const unsigned crc = crc32(0, (const unsigned char*)data.data(), data.size());
        const unsigned off = (unsigned)ftell(f), n = (unsigned)name.size(), sz = (unsigned)data.size();
        std::string h;
        put32(h, 0x04034b50); put16(h, 20); put16(h, 0); put16(h, 0); put16(h, dt); put16(h, dd);
        put32(h, crc); put32(h, sz); put32(h, sz); put16(h, n); put16(h, 0); h += name;
        ok &= fwrite(h.data(), 1, h.size(), f) == h.size();
        ok &= fwrite(data.data(), 1, data.size(), f) == data.size();
        put32(central, 0x02014b50); put16(central, 20); put16(central, 20); put16(central, 0); put16(central, 0);
        put16(central, dt); put16(central, dd); put32(central, crc); put32(central, sz); put32(central, sz);
        put16(central, n); put16(central, 0); put16(central, 0); put16(central, 0); put16(central, 0);
        put32(central, 0); put32(central, off); central += name;
        count++;
    }
    const unsigned off = (unsigned)ftell(f);
    std::string e;
    put32(e, 0x06054b50); put16(e, 0); put16(e, 0); put16(e, count); put16(e, count);
    put32(e, (unsigned)central.size()); put32(e, off); put16(e, 0);
    ok &= fwrite(central.data(), 1, central.size(), f) == central.size();
    ok &= fwrite(e.data(), 1, e.size(), f) == e.size();
    ok &= !ferror(f);
    fclose(f);
    if (!ok) _wremove(zip.c_str());
    return ok;
}

static bool tar_zip(const std::string& staging, const std::vector<std::string>& top, const std::wstring& zip)
{
    wchar_t sys[MAX_PATH];
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return false;
    const std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    if (GetFileAttributesW(tar.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring cmd = L"\"" + tar + L"\" -a -c -f \"" + zip + L"\" -C \"" + widen(staging) + L"\"";
    for (const std::string& t : top) cmd += L" \"" + widen(t) + L"\"";
    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    std::vector<wchar_t> cl(cmd.begin(), cmd.end()); cl.push_back(0);
    if (!CreateProcessW(NULL, cl.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return false;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (code != 0 || !GetFileAttributesExW(zip.c_str(), GetFileExInfoStandard, &fa) || (!fa.nFileSizeLow && !fa.nFileSizeHigh)) {
        DeleteFileW(zip.c_str());
        return false;
    }
    return true;
}

/* ---- the bundle ---- */
struct Packer {
    std::string staging;
    uint64_t cap, used = 0;
    Result* r;
    std::vector<std::string> top;
    std::string listing;

    void note_top(const std::string& rel)
    {
        const std::string t = rel.substr(0, rel.find('\\'));
        if (std::find(top.begin(), top.end(), t) == top.end()) top.push_back(t);
    }
    bool put(const std::string& rel, std::string data, bool text, const char* what = NULL)
    {
        if (text) scrub(data);
        if (used + data.size() > cap) {
            r->skipped.push_back(fmt("%s (%.1f MB, over the %llu MB limit)", rel.c_str(), data.size() / 1048576.0,
                                     (unsigned long long)(cap >> 20)));
            return false;
        }
        const std::string p = staging + "\\" + rel;
        make_dirs(p.substr(0, p.rfind('\\')));
        if (!write_file(p, data)) { r->skipped.push_back(rel + " (could not be written)"); return false; }
        used += data.size(); r->files++;
        note_top(rel);
        listing += fmt("  %-44s %10llu bytes%s%s\r\n", rel.c_str(), (unsigned long long)data.size(), what ? "  " : "", what ? what : "");
        return true;
    }
    /* a file, or (tail) its last bytes; `text` files are scrubbed */
    bool file(const std::string& src, const std::string& rel, bool text, uint64_t tail = 0)
    {
        if (!exists(src)) return false;
        uint64_t full = 0;
        std::string data;
        if (!text) {   /* binary: never cut, skip when too big, before reading it */
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (!GetFileAttributesExA(src.c_str(), GetFileExInfoStandard, &fa)) return false;
            full = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            if (used + full > cap) {
                r->skipped.push_back(fmt("%s (%.1f MB, over the %llu MB limit)", rel.c_str(), full / 1048576.0, (unsigned long long)(cap >> 20)));
                return false;
            }
        }
        if (!read_file(src, data, tail, &full)) { r->skipped.push_back(rel + " (could not be read)"); return false; }
        return put(rel, std::move(data), text, full > data.size() ? "(the end of it)" : NULL);
    }
};

struct DumpFile { std::string path, name; ULONGLONG t; };
static std::vector<DumpFile> find_files(const std::string& dir, const char* pattern)
{
    std::vector<DumpFile> v;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        v.push_back({ dir + "\\" + fd.cFileName, fd.cFileName, ft64(fd.ftLastWriteTime) });
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(v.begin(), v.end(), [](const DumpFile& a, const DumpFile& b) { return a.t > b.t; });
    return v;
}

static std::string last_crash_line(const std::string& root)
{
    std::string all, last;
    if (!read_file(root + "\\logs\\crash\\crashes.txt", all, 64 * 1024)) return last;
    size_t s = 0;
    while (s < all.size()) {
        size_t e = all.find('\n', s);
        if (e == std::string::npos) e = all.size();
        std::string l = all.substr(s, e - s);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty() && l[0] > ' ' && l.compare(0, 4, "[...")) last = l;
        s = e + 1;
    }
    const size_t arrow = last.find(" -> ");
    if (arrow != std::string::npos) last.resize(arrow);
    return last;
}

static std::wstring desktop()
{
    PWSTR p = NULL;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, NULL, &p)) && p) d = p;
    if (p) CoTaskMemFree(p);
    return d;
}

bool create(const Options& o, Result& r)
{
    r = Result();
    const std::string root = o.root.empty() ? find_root(NULL) : o.root;
    const std::string game = o.game.empty() ? detect_game(root) : o.game;
    const std::string ts = stamp(true);
    std::wstring out = o.out_dir.empty() ? desktop() : o.out_dir;
    if (out.empty() || GetFileAttributesW(out.c_str()) == INVALID_FILE_ATTRIBUTES) {
        out = widen(root + "\\logs");
        CreateDirectoryW(out.c_str(), NULL);
    }
    r.zip = out + L"\\" + widen(game + "_report_" + ts + ".zip");

    Packer pk;
    pk.r = &r;
    pk.cap = o.cap_bytes ? o.cap_bytes : (50ull << 20);
    pk.staging = root + "\\logs\\report_staging_" + ts;
    CreateDirectoryA((root + "\\logs").c_str(), NULL);
    remove_tree(pk.staging);
    if (!CreateDirectoryA(pk.staging.c_str(), NULL)) { r.error = "could not create " + pk.staging; return false; }
    pk.cap -= 64 * 1024;   /* room for report.txt, written last */

    if (!o.note.empty()) {
        std::string n = o.note;
        pk.put("note.txt", n, false);   /* the tester's own words: not scrubbed */
    }
    /* crash reports: the newest dump and its .txt first, then the other recent reports */
    const std::string cdir = root + "\\logs\\crash";
    const std::vector<DumpFile> dumps = find_files(cdir, "*.dmp");
    const std::vector<DumpFile> txts = find_files(cdir, "*_*_*.txt");
    std::string newest_txt;
    if (!dumps.empty()) {
        newest_txt = dumps[0].path.substr(0, dumps[0].path.size() - 4) + ".txt";
        pk.file(newest_txt, "crash\\" + dumps[0].name.substr(0, dumps[0].name.size() - 4) + ".txt", true);
    }
    pk.file(cdir + "\\crashes.txt", "crash\\crashes.txt", true, 256 * 1024);
    /* settings */
    static const char* settings[] = { "settings.ini", "settings.bat", "rpcs3_config.yml", "controls.txt", "run.bat",
                                      "mouselook.cfg", "mouselook_r2.cfg", "mouselook_r3.cfg" };
    for (const char* s : settings) pk.file(root + "\\" + s, std::string("settings\\") + s, true, 1u << 20);
    /* the dump */
    if (!dumps.empty()) pk.file(dumps[0].path, "crash\\" + dumps[0].name, false);
    /* logs: the end of each */
    static const char* logs[] = { "run.log", "out.log", "launcher.log", "update.log", "rpcs3_rsx.log", "rpcs3_rsx.prev.log",
                                  "semlog.txt", "dbg.txt", "hitch.txt" };
    for (const char* l : logs) pk.file(root + "\\logs\\" + l, std::string("logs\\") + l, true, 8ull << 20);
    /* other recent crash / hang reports (small text) */
    int more = 0;
    FILETIME nowft; GetSystemTimeAsFileTime(&nowft);
    for (const DumpFile& t : txts) {
        if (!_stricmp(t.path.c_str(), newest_txt.c_str()) || t.t + 14ull * 24 * 3600 * 10000000ull < ft64(nowft)) continue;
        if (more++ >= 6) break;
        pk.file(t.path, "crash\\" + t.name, true, 512 * 1024);
    }
    /* saves */
    std::string saves_line;
    if (o.include_saves) {
        for (const std::string& sd : save_dirs(root, game)) {
            std::vector<std::string> files;
            list_tree(sd, "", files);
            int n = 0;
            for (const std::string& f : files) n += pk.file(sd + "\\" + f, "saves\\" + f, false) ? 1 : 0;
            saves_line += fmt("saves:     %d files from %s\r\n", n, sd.c_str());
        }
        if (saves_line.empty()) saves_line = "saves:     asked for, but no save folder was found\r\n";
    } else {
        saves_line = "saves:     not included\r\n";
    }

    /* report.txt */
    std::string rep;
    rep += fmt("%s PC port -- problem report\r\n", game.c_str());
    rep += fmt("made:      %s\r\n", stamp(false).c_str());
    rep += fmt("folder:    %s\r\n", root.c_str());
    rep += system_info(root);
    const std::string lc = last_crash_line(root);
    rep += fmt("last crash/hang: %s\r\n", lc.empty() ? "(none recorded)" : lc.c_str());
    rep += saves_line;
    rep += "\r\nfiles in this zip:\r\n" + pk.listing;
    if (!r.skipped.empty()) {
        rep += "\r\nleft out:\r\n";
        for (const std::string& s : r.skipped) rep += "  " + s + "\r\n";
    }
    rep += "\r\nNothing was uploaded. Game data, shader caches and builds are never included.\r\n";
    pk.cap += 64 * 1024;
    pk.put("report.txt", rep, true);
    r.packed_bytes = pk.used;

    r.compressed = tar_zip(pk.staging, pk.top, r.zip);
    bool ok = r.compressed || stored_zip(pk.staging, r.zip);
    remove_tree(pk.staging);
    if (!ok) r.error = "could not write " + narrow(r.zip, CP_ACP);
    return ok;
}

} // namespace port_report

/* ---- launcher UI ---- */
namespace port_report_ui_impl {

static std::string s_root;
static HWND s_owner, s_win, s_text, s_note, s_saves, s_create, s_cancel;
static bool s_open;
static int s_result;            /* 0 open, 1 done, 2 cancelled */
static std::string s_reason;
static ULONGLONG s_ack;         /* dump time acknowledged when this window closes */
static bool s_ack_on_create_only;   /* the button: only a created report acknowledges it */
static HFONT s_font;
enum { ID_CREATE = 201, ID_CANCEL, ID_SAVES };

static std::string root() { if (s_root.empty()) s_root = port_report::find_root(NULL); return s_root; }

static ULONGLONG seen()
{
    std::string s;
    if (!port_report::read_file(root() + "\\logs\\crash\\.seen", s)) return 0;
    return _strtoui64(s.c_str(), NULL, 10);
}
static void mark_seen(ULONGLONG t)
{
    if (!t || t <= seen()) return;
    CreateDirectoryA((root() + "\\logs").c_str(), NULL);
    CreateDirectoryA((root() + "\\logs\\crash").c_str(), NULL);
    port_report::write_file(root() + "\\logs\\crash\\.seen", port_report::fmt("%llu\n", t));
}
/* the newest dump since the last acknowledged one (and at most a week old); 0 = none */
static ULONGLONG unseen(bool* crash)
{
    const auto ds = port_report::find_files(root() + "\\logs\\crash", "*.dmp");
    FILETIME now; GetSystemTimeAsFileTime(&now);
    const ULONGLONG s = seen(), week = 7ull * 24 * 3600 * 10000000ull;
    ULONGLONG newest = 0;
    *crash = false;
    for (const auto& d : ds)
        if (d.t > s && d.t + week > port_report::ft64(now)) {
            if (!newest) newest = d.t;
            if (!strstr(d.name.c_str(), "_hang")) *crash = true;
        }
    return newest;
}

static void finish(int result)
{
    s_result = result;
    if (s_ack && (result == 1 || !s_ack_on_create_only)) mark_seen(s_ack);
    if (s_owner) EnableWindow(s_owner, TRUE);
    if (s_win) DestroyWindow(s_win);
    s_win = NULL;
    if (s_owner) SetForegroundWindow(s_owner);
}

static void do_create()
{
    port_report::Options o;
    o.root = root();
    o.include_saves = SendMessageA(s_saves, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const int n = GetWindowTextLengthW(s_note);
    if (n > 0) {
        std::wstring w((size_t)n + 1, 0);
        GetWindowTextW(s_note, &w[0], n + 1);
        w.resize((size_t)n);
        o.note = port_report::narrow(w);
    }
    SetWindowTextA(s_text, "Creating the report...");
    EnableWindow(s_create, FALSE); EnableWindow(s_cancel, FALSE);
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    port_report::Result r;
    const bool ok = port_report::create(o, r);
    SetCursor(old);
    HWND owner = s_owner;
    finish(1);
    if (ok) {
        ShellExecuteW(owner, L"open", L"explorer.exe", (L"/select,\"" + r.zip + L"\"").c_str(), NULL, SW_SHOWNORMAL);
        std::wstring m = L"Saved on your Desktop:\n" + r.zip + L"\n\nAttach this zip to your bug report. Nothing was sent anywhere.";
        if (!r.skipped.empty()) m += L"\n\nSome files were too large and were left out (listed in report.txt).";
        MessageBoxW(owner, m.c_str(), L"Report a problem", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxA(owner, ("Could not create the report: " + r.error).c_str(), "Report a problem", MB_OK | MB_ICONWARNING);
    }
}

static LRESULT CALLBACK proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == ID_CREATE) { do_create(); return 0; }
        if (LOWORD(wp) == ID_CANCEL) { finish(2); return 0; }
        break;
    case WM_CLOSE: finish(2); return 0;
    case WM_CTLCOLORSTATIC: SetBkMode((HDC)wp, TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

/* Modal: returns when the window is closed. */
static void run_dialog(HWND owner, const std::string& reason)
{
    if (s_open) return;
    s_open = true;
    s_owner = owner;
    s_result = 0;
    static bool reg = false;
    if (!reg) {
        WNDCLASSA wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "ResistanceReportProblem";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.hIcon = LoadIcon(NULL, IDI_INFORMATION);
        RegisterClassA(&wc); reg = true;
    }
    const UINT dpi = owner && GetDpiForWindow(owner) ? GetDpiForWindow(owner) : 96;
    auto S = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };
    const int W = S(560), H = S(330);
    RECT orc = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if (owner) GetWindowRect(owner, &orc);
    RECT rc = { 0, 0, W, H };
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME;
    AdjustWindowRectEx(&rc, style, FALSE, ex);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    s_win = CreateWindowExA(ex, "ResistanceReportProblem", "Report a problem", style,
                            orc.left + (orc.right - orc.left - ww) / 2, orc.top + (orc.bottom - orc.top - wh) / 2, ww, wh,
                            owner, NULL, GetModuleHandleA(NULL), NULL);
    if (!s_win) { s_open = false; return; }
    if (!s_font) s_font = CreateFontA(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    const std::string game = port_report::detect_game(root());
    uint64_t saves = 0;
    for (const std::string& d : port_report::save_dirs(root(), game)) saves += port_report::dir_size(d, 1ull << 34);
    std::string msg = reason.empty() ? std::string() : reason + "\r\n\r\n";
    msg += "This packs the latest crash report, the logs, your settings and a system summary (Windows, CPU, GPU and "
           "driver) into one zip file on your Desktop, for you to attach to your bug report. Nothing is uploaded. "
           "Game files and shader caches are never included.";
    s_text = CreateWindowExA(0, "STATIC", msg.c_str(), WS_CHILD | WS_VISIBLE, S(16), S(12), W - S(32), S(100), s_win, NULL, NULL, NULL);
    HWND lbl = CreateWindowExA(0, "STATIC", "What happened? (optional: what you were doing, what you saw)", WS_CHILD | WS_VISIBLE,
                               S(16), S(118), W - S(32), S(20), s_win, NULL, NULL, NULL);
    s_note = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
                             ES_AUTOVSCROLL | ES_WANTRETURN, S(16), S(140), W - S(32), S(90), s_win, NULL, NULL, NULL);
    const std::string sl = saves ? port_report::fmt("Include my save files (%.1f MB)", saves / 1048576.0)
                                 : std::string("Include my save files (none found)");
    s_saves = CreateWindowExA(0, "BUTTON", sl.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                              S(16), S(238), W - S(32), S(24), s_win, (HMENU)(INT_PTR)ID_SAVES, NULL, NULL);
    if (!saves) EnableWindow(s_saves, FALSE);
    const int bw = S(150), bh = S(32), gap = S(10);
    s_create = CreateWindowExA(0, "BUTTON", "Create report", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                               W - S(16) - 2 * bw - gap, H - S(14) - bh, bw, bh, s_win, (HMENU)(INT_PTR)ID_CREATE, NULL, NULL);
    s_cancel = CreateWindowExA(0, "BUTTON", reason.empty() ? "Cancel" : "Not now", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                               W - S(16) - bw, H - S(14) - bh, bw, bh, s_win, (HMENU)(INT_PTR)ID_CANCEL, NULL, NULL);
    for (HWND h : { s_text, lbl, s_note, s_saves, s_create, s_cancel }) SendMessageA(h, WM_SETFONT, (WPARAM)s_font, TRUE);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(s_win, SW_SHOW);
    SetForegroundWindow(s_win);
    SetFocus(s_note);
    MSG m;
    while (s_win && GetMessageA(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE && s_win) { finish(2); continue; }
        if (s_win && IsDialogMessageA(s_win, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    if (s_win) finish(2);
    s_open = false;
}

static void check(HWND owner)
{
    if (s_open || !IsWindowVisible(owner) || IsIconic(owner)) return;
    if (GetForegroundWindow() != owner) return;   /* not while the game (or anything else) is in front */
    bool crash = false;
    const ULONGLONG t = unseen(&crash);
    if (!t) return;
    std::string reason = crash ? "The game crashed last time. Create a report?" : "The game froze last time. Create a report?";
    const std::string lc = port_report::last_crash_line(root());
    if (!lc.empty()) reason += "\r\n" + lc;
    s_ack = t;
    run_dialog(owner, reason);
    s_ack = 0;
}

static void CALLBACK on_timer(HWND hw, UINT, UINT_PTR, DWORD) { check(hw); }

} // namespace port_report_ui_impl

void port_report_start(HWND owner, const char* exe_dir)
{
    port_report_ui_impl::s_root = port_report::find_root(exe_dir);
    port_report_ui_impl::check(owner);
    SetTimer(owner, 0x7C4A, 3000, port_report_ui_impl::on_timer);   /* own TIMERPROC: never reaches the launcher's WM_TIMER */
}

void port_report_ui(HWND owner)
{
    bool crash = false;
    port_report_ui_impl::s_ack = port_report_ui_impl::unseen(&crash);   /* a report made now covers a pending crash too */
    port_report_ui_impl::s_ack_on_create_only = true;
    port_report_ui_impl::run_dialog(owner, std::string());
    port_report_ui_impl::s_ack = 0;
    port_report_ui_impl::s_ack_on_create_only = false;
}

#endif /* _WIN32 */
