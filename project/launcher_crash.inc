/* Resistance port launcher: "the last session ended in a crash" notice.
 *
 * The game's crash reporter (r2_crash.inc / r3_crash.inc / rfom_crash.inc)
 * writes logs\crash\*_crash.dmp, *_hang.dmp and crashes.txt. When the launcher
 * starts, or is in front again later, and there is a dump newer than the one
 * last acknowledged (logs\crash\.seen), a small window offers:
 *   Make bug report  -> logs\crash\bugreport_<time>.zip (the new dumps, crashes.txt,
 *                       the logs, settings, a system summary), shown in Explorer
 *   Open folder      -> logs\crash in Explorer
 *   Dismiss
 * Nothing is uploaded anywhere. Usage: crashnote_start(main_window, exe_dir)
 * once the main window is shown. */
#include <intrin.h>

namespace crashnote {

static char s_root[MAX_PATH];
static HWND s_owner, s_win, s_text;
static bool s_open;
static ULONGLONG s_newest;          // newest dump time of the current notice
static std::string s_status;

struct Dump { std::string path, name; ULONGLONG t, size; bool crash; };

static std::string at(const char* rel) { return std::string(s_root) + "\\" + rel; }
static bool exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static ULONGLONG ft64(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static std::vector<Dump> dumps()
{
    std::vector<Dump> v;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(at("logs\\crash\\*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        Dump d;
        d.name = fd.cFileName;
        d.path = at("logs\\crash\\") + fd.cFileName;
        d.t = ft64(fd.ftLastWriteTime);
        d.size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        d.crash = strstr(fd.cFileName, "_hang") == NULL;
        v.push_back(d);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(v.begin(), v.end(), [](const Dump& a, const Dump& b) { return a.t > b.t; });
    return v;
}

static ULONGLONG seen()
{
    ULONGLONG t = 0;
    if (FILE* f = fopen(at("logs\\crash\\.seen").c_str(), "rb")) { if (fscanf(f, "%llu", &t) != 1) t = 0; fclose(f); }
    return t;
}

static void mark_seen(ULONGLONG t)
{
    if (FILE* f = fopen(at("logs\\crash\\.seen").c_str(), "wb")) { fprintf(f, "%llu\n", t); fclose(f); }
}

static std::vector<Dump> unseen()
{
    std::vector<Dump> v, all = dumps();
    const ULONGLONG s = seen();
    FILETIME now; GetSystemTimeAsFileTime(&now);
    const ULONGLONG week = 7ull * 24 * 3600 * 10000000ull;
    for (const Dump& d : all)
        if (d.t > s && d.t + week > ft64(now)) v.push_back(d);
    return v;
}

static std::string last_crash_line()
{
    std::string last;
    if (FILE* f = fopen(at("logs\\crash\\crashes.txt").c_str(), "rb")) {
        char line[1024];
        while (fgets(line, sizeof line, f)) if (line[0] > ' ') last = line;
        fclose(f);
    }
    while (!last.empty() && (last.back() == '\n' || last.back() == '\r')) last.pop_back();
    const size_t arrow = last.find(" -> ");
    if (arrow != std::string::npos) last.resize(arrow);   // drop the dump path
    return last;
}

/* ---- minimal zip writer (stored entries, zip32) ---- */
static unsigned s_crc[256];
static unsigned crc32(unsigned c, const unsigned char* p, size_t n)
{
    if (!s_crc[1]) for (unsigned i = 0; i < 256; i++) { unsigned k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1; s_crc[i] = k; }
    c = ~c;
    while (n--) c = s_crc[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

struct Zip {
    FILE* f = NULL;
    std::string central;
    unsigned count = 0;
    static void put16(std::string& s, unsigned v) { s += (char)(v & 0xFF); s += (char)(v >> 8 & 0xFF); }
    static void put32(std::string& s, unsigned v) { put16(s, v & 0xFFFF); put16(s, v >> 16); }
    bool add(const char* name, const std::string& data, FILETIME mt)
    {
        if (!f || data.size() >= 0xFFFFFFF0u) return false;
        const unsigned crc = crc32(0, (const unsigned char*)data.data(), data.size());
        FILETIME lt; WORD dd = 0x21, dt = 0;
        if (FileTimeToLocalFileTime(&mt, &lt)) FileTimeToDosDateTime(&lt, &dd, &dt);
        const unsigned off = (unsigned)ftell(f), n = (unsigned)strlen(name), sz = (unsigned)data.size();
        std::string h;
        put32(h, 0x04034b50); put16(h, 20); put16(h, 0); put16(h, 0); put16(h, dt); put16(h, dd);
        put32(h, crc); put32(h, sz); put32(h, sz); put16(h, n); put16(h, 0); h += name;
        fwrite(h.data(), 1, h.size(), f);
        fwrite(data.data(), 1, data.size(), f);
        put32(central, 0x02014b50); put16(central, 20); put16(central, 20); put16(central, 0); put16(central, 0);
        put16(central, dt); put16(central, dd); put32(central, crc); put32(central, sz); put32(central, sz);
        put16(central, n); put16(central, 0); put16(central, 0); put16(central, 0); put16(central, 0);
        put32(central, 0); put32(central, off); central += name;
        count++;
        return true;
    }
    bool finish()
    {
        if (!f) return false;
        const unsigned off = (unsigned)ftell(f);
        std::string e;
        put32(e, 0x06054b50); put16(e, 0); put16(e, 0); put16(e, count); put16(e, count);
        put32(e, (unsigned)central.size()); put32(e, off); put16(e, 0);
        fwrite(central.data(), 1, central.size(), f);
        fwrite(e.data(), 1, e.size(), f);
        const bool ok = !ferror(f);
        fclose(f); f = NULL;
        return ok;
    }
};

// whole file, or its last `tail` bytes; shared read so a running game's logs work too
static bool read_file(const std::string& p, std::string& out, FILETIME& mt, ULONGLONG tail = 0)
{
    HANDLE h = CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz); GetFileTime(h, NULL, NULL, &mt);
    ULONGLONG n = (ULONGLONG)sz.QuadPart, from = 0;
    if (tail && n > tail) { from = n - tail; n = tail; }
    bool ok = n < 0x7FFFFFF0ull;
    if (ok) {
        out.assign((size_t)n, 0);
        LARGE_INTEGER o; o.QuadPart = (LONGLONG)from; SetFilePointerEx(h, o, NULL, FILE_BEGIN);
        size_t got = 0;
        while (got < n) { DWORD r = 0; const DWORD want = (DWORD)(n - got < (1u << 24) ? n - got : (1u << 24));
            if (!ReadFile(h, &out[got], want, &r, NULL) || !r) break; got += r; }
        out.resize(got);
    }
    CloseHandle(h);
    return ok;
}

static std::string sysinfo(const std::vector<Dump>& ds)
{
    std::string s;
    char b[512];
    SYSTEMTIME st; GetLocalTime(&st);
    snprintf(b, sizeof b, "bug report made %04d-%02d-%02d %02d:%02d:%02d\r\ngame folder: %s\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, s_root); s += b;
    typedef LONG (WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi = { sizeof vi };
    if (auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion")) fn(&vi);
    snprintf(b, sizeof b, "Windows %lu.%lu build %lu\r\n", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber); s += b;
    int r[4]; char brand[49] = {0};
    __cpuid(r, 0x80000000);
    if ((unsigned)r[0] >= 0x80000004u) for (int i = 0; i < 3; i++) { __cpuid(r, 0x80000002 + i); memcpy(brand + 16 * i, r, 16); }
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms = { sizeof ms }; GlobalMemoryStatusEx(&ms);
    snprintf(b, sizeof b, "CPU: %s (%lu logical)\r\nRAM: %.1f GB\r\n", brand, si.dwNumberOfProcessors, ms.ullTotalPhys / 1073741824.0); s += b;
    DISPLAY_DEVICEA dd = { sizeof dd };
    for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); i++)
        if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) { snprintf(b, sizeof b, "GPU: %s\r\n", dd.DeviceString); s += b; break; }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(at("build\\*.exe").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            FILETIME lt; SYSTEMTIME t; FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lt); FileTimeToSystemTime(&lt, &t);
            snprintf(b, sizeof b, "build\\%s  %04d-%02d-%02d %02d:%02d  %lu bytes\r\n", fd.cFileName, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, fd.nFileSizeLow); s += b;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    s += "dumps in this report:\r\n";
    for (const Dump& d : ds) { snprintf(b, sizeof b, "  %s (%llu KB)\r\n", d.name.c_str(), d.size / 1024); s += b; }
    return s;
}

static bool make_report(std::string& zip_path)
{
    std::vector<Dump> ds = unseen();
    if (ds.empty()) { std::vector<Dump> all = dumps(); if (!all.empty()) ds.push_back(all[0]); }
    if (ds.size() > 3) ds.resize(3);
    SYSTEMTIME st; GetLocalTime(&st);
    char name[64];
    snprintf(name, sizeof name, "logs\\crash\\bugreport_%04d%02d%02d_%02d%02d%02d.zip", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    zip_path = at(name);
    Zip z;
    z.f = fopen(zip_path.c_str(), "wb");
    if (!z.f) return false;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    std::string data; FILETIME mt;
    std::string notes;
    for (const Dump& d : ds) {
        if (d.size > (512ull << 20)) { notes += "skipped (too big for the zip, attach it separately): " + d.name + "\r\n"; continue; }
        if (read_file(d.path, data, mt)) z.add(("crash/" + d.name).c_str(), data, mt);
    }
    if (read_file(at("logs\\crash\\crashes.txt"), data, mt)) z.add("crash/crashes.txt", data, mt);
    static const char* logs[] = { "logs\\run.log", "logs\\out.log", "logs\\rpcs3_rsx.log", "logs\\launcher.log", "logs\\update.log" };
    for (const char* l : logs)
        if (read_file(at(l), data, mt, 16ull << 20)) z.add((std::string("logs/") + (strrchr(l, '\\') + 1)).c_str(), data, mt);
    static const char* cfg[] = { "settings.bat", "rpcs3_config.yml", "controls.txt", "run.bat" };
    for (const char* c : cfg)
        if (read_file(at(c), data, mt, 4ull << 20)) z.add((std::string("settings/") + c).c_str(), data, mt);
    z.add("system.txt", sysinfo(ds) + notes, now);
    if (!z.finish()) { DeleteFileA(zip_path.c_str()); return false; }
    return true;
}

/* ---- the notice window ---- */
enum { ID_REPORT = 101, ID_FOLDER, ID_DISMISS };

static void close_notice()
{
    if (s_newest) mark_seen(s_newest);
    if (s_owner) { EnableWindow(s_owner, TRUE); SetForegroundWindow(s_owner); }
    if (s_win) DestroyWindow(s_win);
    s_win = NULL;
    s_open = false;
}

static LRESULT CALLBACK notice_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_REPORT: {
            SetWindowTextA(s_text, "Making the bug report...");
            UpdateWindow(s_text);
            std::string zp;
            if (make_report(zp)) {
                mark_seen(s_newest);
                ShellExecuteA(hw, "open", "explorer.exe", ("/select,\"" + zp + "\"").c_str(), NULL, SW_SHOWNORMAL);
                SetWindowTextA(s_text, ("Saved " + zp + "\r\n\r\nAttach this zip to your bug report. Nothing was sent anywhere.").c_str());
            } else {
                SetWindowTextA(s_text, ("Could not write the zip in " + at("logs\\crash") + ".").c_str());
            }
            return 0; }
        case ID_FOLDER:
            ShellExecuteA(hw, "open", at("logs\\crash").c_str(), NULL, NULL, SW_SHOWNORMAL);
            return 0;
        case ID_DISMISS: close_notice(); return 0;
        }
        break;
    case WM_CLOSE: close_notice(); return 0;
    case WM_CTLCOLORSTATIC: SetBkMode((HDC)wp, TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

static void show(HWND owner, const std::vector<Dump>& ds)
{
    s_open = true;
    s_owner = owner;
    s_newest = ds[0].t;
    bool crash = false;
    for (const Dump& d : ds) crash |= d.crash;
    std::string msg = crash ? "The last session ended in a crash." : "The game froze during the last session (a hang report was written).";
    if (crash) { const std::string l = last_crash_line(); if (!l.empty()) msg += "\r\n\r\n" + l; }
    msg += "\r\n\r\n\"Make bug report\" packs the crash dump, the logs and your settings into one zip file "
           "in logs\\crash for you to attach to a bug report. Nothing is uploaded.";
    static bool reg = false;
    if (!reg) {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = notice_proc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "ResistanceCrashNotice";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.hIcon = LoadIcon(NULL, IDI_WARNING);
        RegisterClassA(&wc); reg = true;
    }
    const UINT dpi = GetDpiForWindow(owner) ? GetDpiForWindow(owner) : 96;
    auto S = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };
    const int W = S(520), H = S(250);
    RECT orc; GetWindowRect(owner, &orc);
    RECT rc = { 0, 0, W, H };
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME;
    AdjustWindowRectEx(&rc, style, FALSE, ex);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    s_win = CreateWindowExA(ex, "ResistanceCrashNotice", "Crash report", style,
                            orc.left + (orc.right - orc.left - ww) / 2, orc.top + (orc.bottom - orc.top - wh) / 2, ww, wh,
                            owner, NULL, GetModuleHandleA(NULL), NULL);
    if (!s_win) { s_open = false; return; }
    HFONT font = CreateFontA(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    s_text = CreateWindowExA(0, "STATIC", msg.c_str(), WS_CHILD | WS_VISIBLE, S(16), S(14), W - S(32), H - S(80), s_win, NULL, NULL, NULL);
    const char* labels[3] = { "Make bug report", "Open folder", "Dismiss" };
    const int bw = S(150), bh = S(32), gap = S(10), x0 = W - S(16) - 3 * bw - 2 * gap;
    HWND b[3];
    for (int i = 0; i < 3; i++)
        b[i] = CreateWindowExA(0, "BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i == 0 ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                               x0 + i * (bw + gap), H - S(16) - bh, bw, bh, s_win, (HMENU)(INT_PTR)(ID_REPORT + i), NULL, NULL);
    SendMessageA(s_text, WM_SETFONT, (WPARAM)font, TRUE);
    for (HWND h : b) SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    EnableWindow(owner, FALSE);
    ShowWindow(s_win, SW_SHOW);
    SetForegroundWindow(s_win);
    SetFocus(b[0]);
}

static void check(HWND owner)
{
    if (s_open || !IsWindowVisible(owner) || IsIconic(owner)) return;
    if (GetForegroundWindow() != owner) return;      // not while the game (or anything else) is in front
    const std::vector<Dump> ds = unseen();
    if (!ds.empty()) show(owner, ds);
}

static void CALLBACK on_timer(HWND hw, UINT, UINT_PTR, DWORD) { check(hw); }

} // namespace crashnote

static void crashnote_start(HWND owner, const char* exe_dir)
{
    // the game folder: the launcher's own folder, or its parent for a test build in a subfolder (build_qa\)
    snprintf(crashnote::s_root, MAX_PATH, "%s", exe_dir);
    char up[MAX_PATH];
    snprintf(up, sizeof up, "%s\\..\\run.bat", exe_dir);
    if (!crashnote::exists(crashnote::at("run.bat")) && GetFileAttributesA(up) != INVALID_FILE_ATTRIBUTES)
        if (char* sl = strrchr(crashnote::s_root, '\\')) *sl = 0;
    crashnote::check(owner);
    SetTimer(owner, 0x7C4A, 3000, crashnote::on_timer);   // own TIMERPROC: never reaches the launcher's WM_TIMER
}
