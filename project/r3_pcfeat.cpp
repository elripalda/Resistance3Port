// Resistance 3 PC port: performance overlay (F3, R2_OVERLAY), toast notifications (trophy
// unlocks) and hints (cutscene skip). The panels are painted with GDI on their own thread and
// drawn into the game's swapchain image (port_osd.h, the RPCS3 renderer's overlay pass), so they
// show in exclusive fullscreen too. Before the first present (start-up) and with
// PORT_OSD=layered they are small topmost layered Win32 windows over the game window's client
// area instead. Either way they are hidden while the game is not the foreground window and never
// take focus or input.
//
// R2_OVERLAY   unset: off, F3 toggles | 1: on at start, F3 toggles | 0: off and F3 does nothing
// R2_OVERLAY_POS  tl (default) | tr | bl | br  corner of the game window
//
// Lines: game fps and frame time (avg / worst over the last 0.5 s, from the game's frame
// limiter), RSX (GPU-side) frames per second, draws per frame and the average CPU time per draw
// (the RPCS3 renderer's r2prof counters, switched on only while the overlay is shown).
//
// Exports for the in-game PC Options menu: r2pf_overlay_get / r2pf_overlay_set,
// r2pf_toast(title, text, badge) (queued, 5 s each, top-right),
// r2pf_hint(title, text, ms) (own panel at the bottom, shown at once, replaces the previous hint).
//
// r2pf_status(text): a centred status panel ("Compiling shaders N / M" with a progress bar) shown
// while the renderer compiles the cached pipelines at start-up (VKGSRender.cpp preload_shaders);
// NULL or "" hides it. Shown only after 300 ms, so a quick warm-cache preload does not flash it.
// A second line ("...\n<text>") replaces the panel's subtitle. With the swapchain OSD the renderer
// presents black frames with the OSD layers while it preloads (VKGSRender.cpp, R2_PRELOAD_OSD), so
// the panel shows in exclusive fullscreen too, with the skip keys on its second line; otherwise the
// game window's GDI start-up screen shows the progress and this panel is not used.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>

#include "port_osd.h"

extern "C" void* g_kbm_hwnd;                                                        // cellPad.c
extern "C" void r2rsx_prof_enable(int on);                                          // r2rsx_bridge.cpp
extern "C" int r2rsx_prof_sample(double* frames, double* draws_per_frame, double* us_per_draw);
extern "C" int r2rsx_enabled(void);

namespace {

std::atomic<int> s_overlay{0};
std::atomic<int> s_overlay_allowed{1};
std::atomic<int> s_thread_started{0};

// Game frame times (QPC ticks between frame limiter calls)
std::mutex s_ft_mtx;
double s_ft_sum = 0, s_ft_max = 0; int s_ft_n = 0;
LARGE_INTEGER s_qpf;

struct Toast { std::wstring title, text; char badge; };
std::mutex s_toast_mtx;
std::deque<Toast> s_toasts;

std::wstring widen(const char* s)
{
    if (!s || !*s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    w.resize((size_t)n - 1);
    return w;
}

HWND game_hwnd() { return (HWND)g_kbm_hwnd; }

bool game_visible(RECT& client_screen)
{
    HWND g = game_hwnd();
    if (!g || !IsWindow(g) || IsIconic(g) || GetForegroundWindow() != g) return false;
    RECT rc;
    if (!GetClientRect(g, &rc) || rc.right <= 0 || rc.bottom <= 0) return false;
    POINT a{ rc.left, rc.top }, b{ rc.right, rc.bottom };
    ClientToScreen(g, &a); ClientToScreen(g, &b);
    client_screen = { a.x, a.y, b.x, b.y };
    return true;
}

// ---- overlay ----
HWND s_ov = nullptr, s_tw = nullptr, s_hw = nullptr;
HFONT s_font = nullptr, s_font_b = nullptr, s_font_badge = nullptr;
wchar_t s_ov_text[512] = L"";
int s_ov_w = 0, s_ov_h = 0;

void ov_measure()
{
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ of = SelectObject(dc, s_font);
    RECT r{ 0, 0, 1000, 1000 };
    DrawTextW(dc, s_ov_text, -1, &r, DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, of);
    DeleteDC(dc);
    s_ov_w = r.right + 16; s_ov_h = r.bottom + 10;
}

void paint_ov(HDC dc, const RECT& rc)
{
    HBRUSH bg = CreateSolidBrush(RGB(12, 14, 18));
    FillRect(dc, &rc, bg); DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(150, 255, 150));
    HGDIOBJ of = SelectObject(dc, s_font);
    RECT t{ 8, 5, rc.right - 4, rc.bottom - 2 };
    DrawTextW(dc, s_ov_text, -1, &t, DT_NOPREFIX);
    SelectObject(dc, of);
}

LRESULT layer_msg(HWND h, UINT m, WPARAM w, LPARAM l, void (*paint)(HDC, const RECT&))
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        paint(dc, rc);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return layer_msg(h, m, w, l, paint_ov); }

// ---- start-up status panel (r2pf_status) ----
std::mutex s_st_mtx;
std::wstring s_st_text, s_st_sub;
unsigned s_st_done = 0, s_st_total = 0;
ULONGLONG s_st_t0 = 0;
HWND s_sw = nullptr;
const int SW_W = 520, SW_H = 78;

void paint_status(HDC dc, const RECT& rc)
{
    HBRUSH bg = CreateSolidBrush(RGB(20, 22, 28));
    FillRect(dc, &rc, bg); DeleteObject(bg);
    std::wstring text, sub; unsigned done, total;
    { std::lock_guard<std::mutex> lk(s_st_mtx); text = s_st_text; sub = s_st_sub; done = s_st_done; total = s_st_total; }
    if (sub.empty()) sub = L"Preparing the graphics for this PC, the game starts when done";
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, s_font_b);
    SetTextColor(dc, RGB(235, 235, 240));
    RECT t1{ 14, 8, rc.right - 14, 30 };
    DrawTextW(dc, text.c_str(), -1, &t1, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, s_font);
    SetTextColor(dc, RGB(175, 180, 190));
    RECT t2{ 14, 32, rc.right - 14, 50 };
    DrawTextW(dc, sub.c_str(), -1, &t2, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT bar{ 14, 56, rc.right - 14, 66 };
    HBRUSH back = CreateSolidBrush(RGB(50, 54, 64)); FillRect(dc, &bar, back); DeleteObject(back);
    if (total) {
        bar.right = bar.left + (LONG)((bar.right - bar.left) * (double)std::min(done, total) / total);
        HBRUSH fill = CreateSolidBrush(RGB(90, 160, 230)); FillRect(dc, &bar, fill); DeleteObject(fill);
    }
    SelectObject(dc, of);
}

LRESULT CALLBACK status_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return layer_msg(h, m, w, l, paint_status); }

// The game window is shown (also while it is not yet the foreground window during start-up)
bool game_shown(RECT& client_screen)
{
    HWND g = game_hwnd();
    if (!g || !IsWindow(g) || !IsWindowVisible(g) || IsIconic(g)) return false;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg && fg != g) GetWindowThreadProcessId(fg, &pid);
    if (fg && fg != g && pid != GetCurrentProcessId()) return false;
    RECT rc;
    if (!GetClientRect(g, &rc) || rc.right <= 0 || rc.bottom <= 0) return false;
    POINT a{ rc.left, rc.top }, b{ rc.right, rc.bottom };
    ClientToScreen(g, &a); ClientToScreen(g, &b);
    client_screen = { a.x, a.y, b.x, b.y };
    return true;
}

// ---- toast and hint ----
Toast s_cur; bool s_cur_on = false; ULONGLONG s_cur_t0 = 0, s_prev_now = 0;
const int TW = 360, TH = 72;

// Hints (r2pf_hint) have their own panel: they show at once instead of queueing behind toasts.
std::mutex s_hint_mtx;
Toast s_hint_new; unsigned s_hint_new_ms = 0; bool s_hint_new_set = false;
Toast s_hint; bool s_hint_on = false; ULONGLONG s_hint_t0 = 0; unsigned s_hint_ms = 0;

COLORREF badge_color(char b)
{
    switch (b) {
    case 'P': return RGB(170, 200, 235);
    case 'G': return RGB(230, 185, 60);
    case 'S': return RGB(190, 195, 205);
    case 'B': return RGB(200, 125, 70);
    default:  return RGB(90, 160, 230);
    }
}

void paint_toast(HDC dc, const RECT& rc, const Toast& t)
{
    HBRUSH bg = CreateSolidBrush(RGB(20, 22, 28));
    FillRect(dc, &rc, bg); DeleteObject(bg);
    const COLORREF bc = badge_color(t.badge);
    HBRUSH bar = CreateSolidBrush(bc);
    RECT strip{ 0, 0, 4, rc.bottom }; FillRect(dc, &strip, bar);
    // badge: a filled circle with the grade letter
    HGDIOBJ ob = SelectObject(dc, bar);
    HPEN pen = CreatePen(PS_SOLID, 1, bc); HGDIOBJ op = SelectObject(dc, pen);
    const int cy = rc.bottom / 2;
    Ellipse(dc, 16, cy - 20, 56, cy + 20);
    SelectObject(dc, op); DeleteObject(pen);
    SelectObject(dc, ob); DeleteObject(bar);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, s_font_badge);
    SetTextColor(dc, RGB(20, 22, 28));
    wchar_t letter[2] = { (wchar_t)(t.badge ? t.badge : L'i'), 0 };
    RECT br{ 16, cy - 20, 56, cy + 20 };
    DrawTextW(dc, letter, 1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, s_font_b);
    SetTextColor(dc, RGB(235, 235, 240));
    RECT t1{ 68, 10, rc.right - 10, 32 };
    DrawTextW(dc, t.title.c_str(), -1, &t1, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, s_font);
    SetTextColor(dc, RGB(175, 180, 190));
    RECT t2{ 68, 34, rc.right - 10, rc.bottom - 6 };
    DrawTextW(dc, t.text.c_str(), -1, &t2, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
}

void paint_cur_toast(HDC dc, const RECT& rc) { paint_toast(dc, rc, s_cur); }
void paint_cur_hint(HDC dc, const RECT& rc) { paint_toast(dc, rc, s_hint); }
LRESULT CALLBACK toast_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return layer_msg(h, m, w, l, paint_cur_toast); }
LRESULT CALLBACK hint_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return layer_msg(h, m, w, l, paint_cur_hint); }

HWND make_layer(const wchar_t* cls, WNDPROC proc)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                             cls, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);
    if (h) SetLayeredWindowAttributes(h, 0, 220, LWA_ALPHA);
    return h;
}

void place(HWND h, int x, int y, int w, int hh)
{
    SetWindowPos(h, HWND_TOPMOST, x, y, w, hh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void hide(HWND h) { if (h && IsWindowVisible(h)) ShowWindow(h, SW_HIDE); }

// Paints one panel into the swapchain OSD slot (client coordinates of the game window).
void osd_commit(int slot, int x, int y, int w, int h, int cw, int ch, int alpha, void (*paint)(HDC, const RECT&))
{
    PosdCanvas c;
    if (!c.begin(w, h)) return;
    const RECT rc{ 0, 0, w, h };
    paint(c.dc, rc);
    c.commit(slot, x, y, cw, ch, alpha / 255.f);
}

// 200 ms fade-in, full until `life - 600`, then a 600 ms fade-out; 0..230
int fade_alpha(ULONGLONG age, ULONGLONG life)
{
    if (age >= life) return 0;
    if (age < 200) return (int)(230 * age / 200);
    if (age + 600 > life) return (int)(230 * (life - age) / 600);
    return 230;
}

void update_overlay_text(bool fresh)
{
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (!fresh && now - last < 500) return;
    const double dt = last ? (now - last) / 1000.0 : 0.5;
    last = now;
    double sum, mx; int n;
    {
        std::lock_guard<std::mutex> lk(s_ft_mtx);
        sum = s_ft_sum; mx = s_ft_max; n = s_ft_n;
        s_ft_sum = s_ft_max = 0; s_ft_n = 0;
    }
    std::wstring t;
    wchar_t b[160];
    if (n > 0) {
        const double avg = sum / n;
        swprintf(b, 160, L"Game  %5.1f fps   %5.2f ms (max %5.2f)", avg > 0 ? 1000.0 / avg : 0.0, avg, mx);
    } else swprintf(b, 160, L"Game  --");
    t += b;
    double fr = 0, dpf = 0, upd = 0;
    if (r2rsx_enabled() && r2rsx_prof_sample(&fr, &dpf, &upd)) {
        swprintf(b, 160, L"\nRSX   %5.1f fps   %4.0f draws/frame   %5.1f us/draw", fr / std::max(dt, 0.05), dpf, upd);
        t += b;
    }
    wcsncpy(s_ov_text, t.c_str(), 511);
    s_ov_text[511] = 0;
}

void ui_thread()
{
    QueryPerformanceFrequency(&s_qpf);
    while (!g_kbm_hwnd) Sleep(250);
    s_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    s_font_b = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_font_badge = CreateFontW(-22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_ov = make_layer(L"R3PortPerfOverlay", ov_proc);
    s_sw = make_layer(L"R3PortStatus", status_proc);
    s_tw = make_layer(L"R3PortToast", toast_proc);
    s_hw = make_layer(L"R3PortHint", hint_proc);
    const char* pos = getenv("R2_OVERLAY_POS");
    const bool right = pos && (pos[1] == 'r' || pos[1] == 'R'), bottom = pos && (pos[0] == 'b' || pos[0] == 'B');
    int was_f3 = 0, prof_on = 0;
    bool osd_was = false;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        // Swapchain OSD (port_osd.h) once the renderer presents; the layered windows before that
        // (start-up) and with PORT_OSD=layered.
        const bool osd = posd_on() != 0;
        if (osd != osd_was) {
            osd_was = osd;
            if (osd) { hide(s_ov); hide(s_sw); hide(s_tw); hide(s_hw); }
            else for (int s : { POSD_PERF, POSD_STATUS, POSD_TOAST, POSD_HINT }) r2rsx_osd_hide(s);
        }
        RECT cs{};
        const bool vis = game_visible(cs);
        const int cw = cs.right - cs.left, ch = cs.bottom - cs.top;
        // F3 toggles (only while the game has focus)
        const int f3 = vis && s_overlay_allowed && (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        if (f3 && !was_f3) s_overlay = !s_overlay;
        was_f3 = f3;
        if (s_overlay != prof_on) {
            prof_on = s_overlay;
            r2rsx_prof_enable(prof_on);
            if (prof_on) update_overlay_text(true);
        }
        if (vis && s_overlay && s_ov) {
            const ULONGLONG before = GetTickCount64();
            static ULONGLONG painted = 0;
            static int osd_cw = 0, osd_ch = 0;
            const bool repaint = before - painted >= 500;
            if (repaint) { update_overlay_text(true); ov_measure(); painted = before; }
            const int x = right ? cw - s_ov_w - 8 : 8, y = bottom ? ch - s_ov_h - 8 : 8;
            if (osd) {
                if (repaint || cw != osd_cw || ch != osd_ch || !r2rsx_osd_visible(POSD_PERF)) {
                    osd_cw = cw; osd_ch = ch;
                    osd_commit(POSD_PERF, x, y, s_ov_w, s_ov_h, cw, ch, 220, paint_ov);
                }
            } else {
                if (repaint) InvalidateRect(s_ov, nullptr, FALSE);
                place(s_ov, cs.left + x, cs.top + y, s_ov_w, s_ov_h);
            }
        } else if (osd) r2rsx_osd_hide(POSD_PERF);
        else hide(s_ov);

        // start-up shader compile status, centred in the lower third of the game window
        bool st_on = false;
        if (s_sw) {
            ULONGLONG t0; std::wstring key;
            { std::lock_guard<std::mutex> lk(s_st_mtx); t0 = s_st_t0; key = s_st_text + L"\n" + s_st_sub; }
            RECT ss{};
            if (t0 && GetTickCount64() - t0 >= 300 && game_shown(ss)) {
                st_on = true;
                const int sw = ss.right - ss.left, sh = ss.bottom - ss.top;
                const int x = (sw - SW_W) / 2, y = sh * 2 / 3;
                if (osd) {
                    static std::wstring shown; static int scw = 0, sch = 0;
                    if (key != shown || sw != scw || sh != sch || !r2rsx_osd_visible(POSD_STATUS)) {
                        shown = key; scw = sw; sch = sh;
                        osd_commit(POSD_STATUS, x, y, SW_W, SW_H, sw, sh, 220, paint_status);
                    }
                } else {
                    InvalidateRect(s_sw, nullptr, FALSE);
                    place(s_sw, ss.left + x, ss.top + y, SW_W, SW_H);
                }
            } else if (osd) r2rsx_osd_hide(POSD_STATUS);
            else hide(s_sw);
        }

        // toasts: 5 s each, the last 0.6 s fading out; queued while the game is in the background
        const ULONGLONG now = GetTickCount64();
        bool fading = false;
        if (s_tw) {
            if (s_cur_on && now - s_cur_t0 > 5000) { s_cur_on = false; hide(s_tw); r2rsx_osd_hide(POSD_TOAST); }
            bool fresh = false;
            if (!s_cur_on && vis) {
                std::lock_guard<std::mutex> lk(s_toast_mtx);
                if (!s_toasts.empty()) { s_cur = s_toasts.front(); s_toasts.pop_front(); s_cur_on = true; s_cur_t0 = now; fresh = true; InvalidateRect(s_tw, nullptr, TRUE); }
            }
            if (s_cur_on && vis) {
                const ULONGLONG age = now - s_cur_t0;
                const int a = fade_alpha(age, 5000);
                fading = age < 200 || age > 4400;
                const int x = right ? 16 : cw - TW - 16, y = 16;   // opposite corner to the overlay
                if (osd) {
                    static int tcw = 0, tch = 0;
                    if (fresh || cw != tcw || ch != tch || !r2rsx_osd_visible(POSD_TOAST)) {
                        tcw = cw; tch = ch;
                        osd_commit(POSD_TOAST, x, y, TW, TH, cw, ch, a, paint_cur_toast);
                    } else posd_move(POSD_TOAST, x, y, cw, ch, a / 255.f);
                } else {
                    SetLayeredWindowAttributes(s_tw, 0, (BYTE)std::max(a, 0), LWA_ALPHA);
                    place(s_tw, cs.left + x, cs.top + y, TW, TH);
                }
            } else if (!vis && s_cur_on) { s_cur_t0 += now - s_prev_now; hide(s_tw); r2rsx_osd_hide(POSD_TOAST); }   // paused in the background
            s_prev_now = now;
        }

        // hint: replaces the previous one at once; its clock runs on in the background
        if (s_hw) {
            bool fresh = false;
            {
                std::lock_guard<std::mutex> lk(s_hint_mtx);
                if (s_hint_new_set) {
                    s_hint_new_set = false;
                    s_hint = s_hint_new; s_hint_ms = std::max(s_hint_new_ms, 1000u);
                    s_hint_on = true; s_hint_t0 = now; fresh = true;
                    InvalidateRect(s_hw, nullptr, TRUE);
                }
            }
            if (s_hint_on && now - s_hint_t0 > s_hint_ms) { s_hint_on = false; hide(s_hw); r2rsx_osd_hide(POSD_HINT); }
            if (s_hint_on && vis) {
                const ULONGLONG age = now - s_hint_t0;
                const int a = fade_alpha(age, s_hint_ms);
                fading |= age < 200 || age + 600 > s_hint_ms;
                // lower corner on the toast's side, away from the overlay when it is down there
                const int x = (right && bottom) ? 16 : cw - TW - 16, y = ch - TH - 16;
                if (osd) {
                    static int hcw = 0, hch = 0;
                    if (fresh || cw != hcw || ch != hch || !r2rsx_osd_visible(POSD_HINT)) {
                        hcw = cw; hch = ch;
                        osd_commit(POSD_HINT, x, y, TW, TH, cw, ch, a, paint_cur_hint);
                    } else posd_move(POSD_HINT, x, y, cw, ch, a / 255.f);
                } else {
                    SetLayeredWindowAttributes(s_hw, 0, (BYTE)std::max(a, 0), LWA_ALPHA);
                    place(s_hw, cs.left + x, cs.top + y, TW, TH);
                }
            } else if (s_hint_on) { hide(s_hw); r2rsx_osd_hide(POSD_HINT); }
        }

        if (osd) r2rsx_osd_refresh();   // re-asks for a present if the game is parked (pause menus)
        Sleep(osd && fading ? 16 : st_on ? 50 : s_overlay || s_cur_on || s_hint_on ? 33 : 100);
    }
}

void start_thread()
{
    if (s_thread_started.exchange(1)) return;
    std::thread(ui_thread).detach();
}

struct PcFeatInit {
    PcFeatInit()
    {
        const char* e = getenv("R2_OVERLAY");
        if (e && *e == '0') s_overlay_allowed = 0;
        else if (e && *e == '1') s_overlay = 1;
        start_thread();
    }
} s_init;

} // namespace

// Called once per game frame (stubs.cpp, the frame limiter hook).
extern "C" void r2pf_frame_tick(void)
{
    static LARGE_INTEGER last{};
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (last.QuadPart && s_qpf.QuadPart && s_overlay) {
        const double ms = (now.QuadPart - last.QuadPart) * 1000.0 / s_qpf.QuadPart;
        if (ms < 1000.0) {
            std::lock_guard<std::mutex> lk(s_ft_mtx);
            s_ft_sum += ms; s_ft_n++; if (ms > s_ft_max) s_ft_max = ms;
        }
    }
    last = now;
}

extern "C" int r2pf_overlay_get(void) { return s_overlay; }
extern "C" void r2pf_overlay_set(int on) { if (s_overlay_allowed) s_overlay = on ? 1 : 0; }

// Start-up status panel: "Compiling shaders 120 / 1500" (UTF-8). A trailing "N / M" also drives the
// progress bar; an optional second line after '\n' replaces the subtitle. NULL or "" hides the panel
// (the OSD slot at once, so the renderer's next present is without it).
extern "C" void r2pf_status(const char* text)
{
    std::lock_guard<std::mutex> lk(s_st_mtx);
    if (!text || !*text) { s_st_text.clear(); s_st_sub.clear(); s_st_t0 = 0; s_st_done = s_st_total = 0; r2rsx_osd_hide(POSD_STATUS); return; }
    const char* nl = std::strchr(text, '\n');
    const std::string first = nl ? std::string(text, nl) : std::string(text);
    s_st_text = widen(first.c_str());
    s_st_sub = nl ? widen(nl + 1) : std::wstring();
    text = first.c_str();
    unsigned d = 0, t = 0;
    const char* p = std::strrchr(text, '/');
    if (p && p > text) {
        const char* q = p - 1;
        while (q > text && *q == ' ') --q;
        while (q > text && q[-1] >= '0' && q[-1] <= '9') --q;
        d = (unsigned)std::strtoul(q, nullptr, 10);
        t = (unsigned)std::strtoul(p + 1, nullptr, 10);
    }
    s_st_done = d; s_st_total = t;
    if (!s_st_t0) s_st_t0 = GetTickCount64();
}

// A toast at the game window's top-right corner for 5 s. title/text are UTF-8; badge is a
// trophy grade 'P' 'G' 'S' 'B' (coloured circle with the letter) or 0 for a plain notice.
extern "C" void r2pf_toast(const char* title, const char* text, char badge)
{
    std::lock_guard<std::mutex> lk(s_toast_mtx);
    if (s_toasts.size() < 32) s_toasts.push_back({ widen(title), widen(text), badge });
}

// A hint at the game window's lower corner for `ms` (at least 1 s), shown at once in its own panel
// (not queued behind toasts) and replacing the previous hint. title/text are UTF-8.
extern "C" void r2pf_hint(const char* title, const char* text, unsigned ms)
{
    std::lock_guard<std::mutex> lk(s_hint_mtx);
    s_hint_new = { widen(title), widen(text), 0 };
    s_hint_new_ms = ms;
    s_hint_new_set = true;
}