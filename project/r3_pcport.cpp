/* Resistance 3 PC port (from Resistance 2's r2_pcport.cpp): native-PC conveniences around the game window.
 *
 * Runs on the game window's thread (rsx_null_backend.c calls g_port_ui_tick
 * from its message pump, ~250 times a second; the window is the RPCS3 and
 * Vulkan renderers' window):
 *   F10 (or holding View+Menu / Select+Start on a controller for 1.5 s)
 *        "Quit to desktop?" prompt: Enter / Space / A = yes, Esc / Backspace / B
 *        = no. The game is paused and gets no input while it is up. Yes waits
 *        for a save in progress (cellSaveData.c g_savedata_busy, 15 s at most),
 *        flushes every open file and closes the window -- the same clean path
 *        as the X button, which (g_port_close_hook) also waits for the save.
 *        With the PC menu on (R2_MENU_PC, default 1) F10 / View+Menu open the
 *        PC menu instead: Resume / PC Options / Quit to Desktop (-> the prompt
 *        above). PC Options: frame rate, upscaler (next start), mouse
 *        sensitivity, field of view, controller prompts (at once); each change
 *        is saved where the launcher keeps it (settings.ini + settings.bat,
 *        controls.txt). The game's own pause / main menus are Scaleform movies
 *        the port does not edit; a small hint ("F10: PC menu") is shown while
 *        the pause menu is up and once at start (R2_MENU_PC_HINT=0 hides it;
 *        off by default in exclusive fullscreen unless the swapchain OSD is on).
 *   F12 / PrtScn  screenshot of the game window -> screenshots\R3_<date>_<time>.png
 *   Focus loss    R2_BACKGROUND = pause (default: the game's frame loop is held
 *                 in stubs.cpp r2_frame_period and the sound muted) | mute | run.
 *                 The cursor is always released (unclipped, shown) while the
 *                 window is in the background, paused or showing the prompt.
 * The menu, prompts and toasts are drawn into the game's swapchain image once the
 * renderer presents (port_osd.h; layered windows before that or with
 * PORT_OSD=layered), so exclusive fullscreen shows them too.
 * No guest code is touched here. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <xinput.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>
#include "port_osd.h"
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

extern "C" {
volatile int g_r2pc_paused = 0;                      /* read by stubs.cpp r2_frame_period, debugtools.inc */
extern volatile int g_port_ui_modal;                 /* libs/input/cellPad.c: game input blocked */
extern volatile int g_port_audio_mute;               /* libs/audio/cellAudio.c */
extern volatile long g_savedata_busy;                /* libs/system/cellSaveData.c */
extern volatile int g_kbm_capture;                   /* cellPad.c: F8 mouse release */
void sys_fs_flush_all(void);                         /* runtime/syscalls/sys_fs.c */
extern void (*volatile g_port_ui_tick)(void* hwnd);  /* libs/video/rsx_null_backend.c */
extern void (*volatile g_port_close_hook)(void* hwnd);
extern volatile unsigned long long g_r2_look_ms;     /* stubs.cpp: the on-foot look code last ran (ms) */
void cellpad_set_glyph_pref(const char* v);          /* cellPad.c: live R2_PAD_GLYPHS */
double cellpad_mouse_sens(void);
void cellpad_set_mouse_sens(double s);
extern uint8_t* vm_base;
}

enum { BG_PAUSE, BG_MUTE, BG_RUN };
static int s_bg = -1;
static HWND s_game = NULL;
static WNDPROC s_orig_proc = NULL;
static volatile int s_shot_req = 0;

/* ---- the prompt / toast / PC menu window ------------------------------- */
enum { OV_NONE, OV_ASK, OV_SAVING, OV_RELEASE, OV_MENU };
enum { KIND_DIALOG, KIND_TOAST, KIND_HINT, KIND_MENU };
static int s_ov_state = OV_NONE;
static HWND s_ov = NULL;
static wchar_t s_ov_l1[96], s_ov_l2[160];
static int s_ov_toast = 0;                           /* s_ov_kind != KIND_DIALOG / KIND_MENU */
static int s_ov_kind = KIND_DIALOG;
static ULONGLONG s_toast_until = 0, s_save_t0 = 0;

/* ---- PC menu model -------------------------------------------------------
 * Page 0: Resume / PC Options / Quit to Desktop. Page 1: the options + Back.
 * Every change is written at once to the files the launcher uses, so the
 * launcher shows it next time and run.bat (settings.bat) picks it up. */
static int s_menu_on = -1;                           /* R2_MENU_PC, default 1 */
static int s_menu_page = 0, s_menu_sel = 0, s_ask_from_menu = 0;
static int s_menu_row_top = 0, s_menu_row_h = 1, s_menu_val_x = 0;   /* last paint, for the mouse */
static POINT s_menu_mouse0; static int s_menu_mouse_armed = 0;        /* cursor at open; moved since */
static void menu_mouse_reset(void) { GetCursorPos(&s_menu_mouse0); s_menu_mouse_armed = 0; }
struct PcChoice { std::wstring label; std::string value; };
enum { OPT_FPS, OPT_FSR, OPT_SENS, OPT_FOV, OPT_PROMPTS, OPT_COUNT };
struct PcOpt { const wchar_t* title; const wchar_t* help; const char* ini; int restart; std::vector<PcChoice> ch; int sel, run; };
static PcOpt s_opt[OPT_COUNT];
static double s_sens = 1.0;
static int s_opt_init = 0;

static std::string ltrim(const std::string& s) { size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++; return s.substr(i); }
static bool starts_ci(const std::string& s, const std::string& p) { return s.size() >= p.size() && !_strnicmp(s.c_str(), p.c_str(), p.size()); }
static bool read_lines(const char* path, std::vector<std::string>& out)
{
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; out.push_back(line); }
    fclose(f);
    return true;
}
/* Replace the first line starting with `prefix` (case-insensitive, leading
 * blanks ignored) by `line` and drop any repeats; append it when there was
 * none. An empty `line` removes it. Written to a temporary file first. */
static bool file_set(const char* path, const std::string& prefix, const std::string& line, const char* header)
{
    std::vector<std::string> in, out;
    const bool had = read_lines(path, in);
    if (!had && line.empty()) return true;
    bool done = false;
    for (auto& l : in) {
        if (starts_ci(ltrim(l), prefix)) { if (!done && !line.empty()) out.push_back(line); done = true; continue; }
        out.push_back(l);
    }
    if (!done && !line.empty()) { if (!had && header) out.push_back(header); out.push_back(line); }
    char tmp[MAX_PATH]; snprintf(tmp, sizeof tmp, "%s.pcmenu.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) return false;
    for (auto& l : out) fprintf(f, "%s\n", l.c_str());
    const bool ok = fclose(f) == 0;
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(tmp); return false; }
    return true;
}
static std::string ini_get(const char* key)
{
    std::vector<std::string> in; read_lines("settings.ini", in);
    const std::string p = std::string(key) + "=";
    for (auto& l : in) if (!strncmp(l.c_str(), p.c_str(), p.size())) return l.substr(p.size());
    return std::string("\x01");                      /* not there */
}
static void ini_set(const char* key, const std::string& v) { file_set("settings.ini", std::string(key) + "=", std::string(key) + "=" + v, NULL); }
static void bat_set(const char* env, const std::string& v)
{
    file_set("settings.bat", std::string("set \"") + env + "=", v.empty() ? std::string() : std::string("set \"") + env + "=" + v + "\"",
             "@echo off");
}
static std::string env_or(const char* name, const char* def) { const char* e = getenv(name); return (e && *e) ? e : def; }

static void opt_add(int i, const wchar_t* title, const wchar_t* help, const char* ini, int restart,
                    std::vector<PcChoice> ch, const std::string& running)
{
    PcOpt& o = s_opt[i];
    o.title = title; o.help = help; o.ini = ini; o.restart = restart; o.ch = std::move(ch);
    auto find = [&](const std::string& v) { for (size_t k = 0; k < o.ch.size(); k++) if (!_stricmp(o.ch[k].value.c_str(), v.c_str())) return (int)k; return -1; };
    o.run = find(running);
    if (o.run < 0) {                                 /* a value set by hand: listed as is */
        std::wstring w(running.begin(), running.end());
        o.ch.push_back({ w, running }); o.run = (int)o.ch.size() - 1;
    }
    const std::string saved = ini ? ini_get(ini) : std::string("\x01");
    o.sel = saved != "\x01" && find(saved) >= 0 ? find(saved) : o.run;
}
static void opts_init(void)
{
    if (s_opt_init) return;
    s_opt_init = 1;
    std::string fps = env_or("R2_FPS", "60");
    if (!_stricmp(fps.c_str(), "uncapped") || !_stricmp(fps.c_str(), "max") || !_stricmp(fps.c_str(), "unlimited") || !_stricmp(fps.c_str(), "off")) fps = "0";
    if (!_stricmp(fps.c_str(), "display")) fps = "refresh";
    opt_add(OPT_FPS, L"Frame rate", L"30 is the PS3 original. Higher needs more CPU and GPU time.", "fps", 1,
            { {L"30 fps (PS3 original)", "30"}, {L"60 fps", "60"}, {L"120 fps", "120"}, {L"144 fps", "144"},
              {L"Match display", "refresh"}, {L"Uncapped", "0"} }, fps);
    std::string fsr = env_or("RSX_FSR", "0");
    if (fsr == "3") fsr = "1";                       /* FSR 3.1 on the native Vulkan renderer */
    opt_add(OPT_FSR, L"Upscaler / anti-aliasing", L"DLSS needs a GeForce RTX (falls back to FSR). Quality preset: launcher.", "fsr", 1,
            { {L"Off (PS3 original)", "0"}, {L"AMD FSR 3.1", "1"}, {L"NVIDIA DLSS", "2"} }, fsr);
    s_sens = cellpad_mouse_sens();
    opt_add(OPT_SENS, L"Mouse sensitivity", L"Also multiplied by the game's own Look Sensitivity option.", NULL, 0, {}, "");
    std::string fov = env_or("R2_PATCH_FOV_VALUE", "");
    if (fov.empty() && atoi(env_or("R2_PATCH_FOV", "0").c_str()) == 1) fov = "0.7";
    opt_add(OPT_FOV, L"Field of view", L"Wider views can misalign some shadows (RPCS3 \"Extended FOV\" patch).", "fov", 0,
            { {L"Original", ""}, {L"Wider", "0.85"}, {L"Widest", "0.7"} }, fov);
    opt_add(OPT_PROMPTS, L"Button prompts", L"Which buttons the game shows with a controller (keyboard always shows keys).", "padglyphs", 0,
            { {L"Auto", ""}, {L"Keyboard keys", "keys"}, {L"Xbox", "xbox"}, {L"PlayStation", "ps"} }, env_or("R2_PAD_GLYPHS", ""));
}
/* the Extended FOV constant (r3_patches.cpp): 1.0 original */
static void fov_live(const std::string& v)
{
    if (!vm_base) return;
    const uint32_t ea = 0x00F0608Cu;
    float cur; uint32_t u = _byteswap_ulong(*(uint32_t*)(vm_base + ea)); memcpy(&cur, &u, 4);
    if (!(cur >= 0.3f && cur <= 1.0f)) { fprintf(stderr, "[pcmenu] field of view: unexpected value at 0x%08X, not changed%c", ea, 10); return; }
    float f = v.empty() ? 1.0f : (float)atof(v.c_str());
    if (!(f >= 0.3f && f <= 1.0f)) f = 1.0f;
    memcpy(&u, &f, 4); *(volatile uint32_t*)(vm_base + ea) = _byteswap_ulong(u);
}
static void opt_apply(int i)
{
    PcOpt& o = s_opt[i];
    if (i == OPT_SENS) {
        cellpad_set_mouse_sens(s_sens);
        char l[64]; snprintf(l, sizeof l, "mouse_sensitivity = %.2f", s_sens);
        file_set("controls.txt", "mouse_sensitivity", l, NULL);
        fprintf(stderr, "[pcmenu] mouse sensitivity %.2f%c", s_sens, 10);
        return;
    }
    const std::string v = o.ch[o.sel].value;
    if (o.ini) ini_set(o.ini, v);
    switch (i) {
    case OPT_FPS: bat_set("R2_FPS", v); break;
    case OPT_FSR: {
        std::string r = ini_get("renderer"); if (r == "\x01" || r.empty()) r = env_or("R2_RSX", "rpcs3");
        bat_set("RSX_FSR", (!_stricmp(r.c_str(), "vulkan") && v == "1") ? "3" : v);
        std::string up = ini_get("upscale"); if (up == "\x01" || up.empty()) up = "1";
        bat_set("RSX_UPSCALE", v == "0" ? "1" : up);
        break; }
    case OPT_FOV: bat_set("R2_PATCH_FOV_VALUE", v); fov_live(v); break;
    case OPT_PROMPTS: bat_set("R2_PAD_GLYPHS", v); cellpad_set_glyph_pref(v.c_str()); break;
    }
    fprintf(stderr, "[pcmenu] %ls = %ls%s%c", o.title, o.ch[o.sel].label.c_str(), o.restart ? " (next start)" : "", 10);
}
static void opt_step(int i, int d)
{
    if (i == OPT_SENS) {
        double s = floor(s_sens * 20.0 + 0.5) / 20.0 + d * 0.05;
        if (s < 0.1) s = 0.1;
        if (s > 4.0) s = 4.0;
        if (fabs(s - s_sens) < 1e-9) return;
        s_sens = s;
    } else {
        PcOpt& o = s_opt[i];
        const int n = (int)o.ch.size();
        o.sel = (o.sel + d + n) % n;
    }
    opt_apply(i);
}
extern "C" void r2x_open_menu(void);   /* r3_pcextra.cpp: More PC Options */
extern "C" int r2x_menu_open(void);
extern "C" void r2x_ui_tick(void* hwnd, int focused, int other_prompt);
extern "C" int r2x_want_pause(void);
static int menu_rows(void) { return s_menu_page == 0 ? 4 : OPT_COUNT + 1; }
static std::wstring menu_value(int row)
{
    if (s_menu_page == 0 || row >= OPT_COUNT) return L"";
    wchar_t b[96];
    if (row == OPT_SENS) swprintf_s(b, L"\u25C2  %.2fx  \u25B8", s_sens);
    else {
        const PcOpt& o = s_opt[row];
        swprintf_s(b, L"\u25C2  %s%s  \u25B8", o.ch[o.sel].label.c_str(), o.restart && o.sel != o.run ? L" *" : L"");
    }
    return b;
}
static const wchar_t* menu_label(int row)
{
    static const wchar_t* main_rows[] = { L"Resume",     L"PC Options", L"More PC Options", L"Quit to Desktop" };
    if (s_menu_page == 0) return main_rows[row];
    return row < OPT_COUNT ? s_opt[row].title : L"Back";
}
static std::wstring menu_help(void)
{
    if (s_menu_page == 0) {
        static const wchar_t* h[] = { L"Back to the game.", L"Frame rate, upscaler, mouse, field of view, button prompts.",
                                              L"Colour filter, crosshair, HDR, Reflex, key bindings, controller layout.",
                                      L"Waits for any save in progress, then closes the game." };
        return h[s_menu_sel];
    }
    if (s_menu_sel >= OPT_COUNT) return L"Back to the PC menu.";
    const PcOpt& o = s_opt[s_menu_sel];
    std::wstring s = o.help;
    if (o.restart) s += o.sel != o.run ? L"  * Applies the next time the game starts." : L"  (Applies the next time the game starts.)";
    return s;
}

static void paint_menu(HDC dc, const RECT& rc)
{
    const int H = rc.bottom - rc.top, W = rc.right - rc.left, n = menu_rows();
    const int rh = s_menu_row_h = H / (n + 4);
    const int top = s_menu_row_top = rc.top + rh * 3 / 2;
    auto font = [&](int px, int wt) { return CreateFontW(-px, 0, 0, 0, wt, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI"); };
    HFONT ft = font(rh * 62 / 100, FW_BOLD), fr = font(rh * 48 / 100, FW_SEMIBOLD), fs = font(rh * 34 / 100, FW_NORMAL);
    HGDIOBJ of = SelectObject(dc, ft);
    RECT r = { rc.left + rh / 2, rc.top + rh / 3, rc.right - rh / 2, top };
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, s_menu_page == 0 ? L"PC MENU" : L"PC OPTIONS", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, fr);
    s_menu_val_x = rc.left + W * 46 / 100;
    for (int i = 0; i < n; i++) {
        RECT rr = { rc.left + 6, top + i * rh, rc.right - 6, top + (i + 1) * rh };
        if (i == s_menu_sel) { HBRUSH b = CreateSolidBrush(RGB(150, 32, 24)); FillRect(dc, &rr, b); DeleteObject(b); }
        SetTextColor(dc, i == s_menu_sel ? RGB(255, 255, 255) : RGB(205, 205, 210));
        RECT rl = { rr.left + rh / 2, rr.top, s_menu_val_x, rr.bottom };
        DrawTextW(dc, menu_label(i), -1, &rl, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        const std::wstring v = menu_value(i);
        if (!v.empty()) {
            RECT rv = { s_menu_val_x, rr.top, rr.right - rh / 2, rr.bottom };
            DrawTextW(dc, v.c_str(), -1, &rv, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    SelectObject(dc, fs);
    SetTextColor(dc, RGB(185, 185, 190));
    RECT rh1 = { rc.left + rh / 2, top + n * rh + rh / 6, rc.right - rh / 2, top + n * rh + rh * 7 / 6 };
    DrawTextW(dc, menu_help().c_str(), -1, &rh1, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    SetTextColor(dc, RGB(140, 140, 150));
    RECT rh2 = { rc.left + rh / 2, top + n * rh + rh * 7 / 6, rc.right - rh / 2, rc.bottom - rh / 6 };
    DrawTextW(dc, L"\u2191\u2193 select    \u2190\u2192 change    Enter / A  OK    Esc / B  back    F10  close", -1, &rh2,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, of); DeleteObject(ft); DeleteObject(fr); DeleteObject(fs);
}

static void menu_activate(int dir);
static void ov_refresh(void);
/* The mouse over the PC menu (y in menu coordinates): from the layered window, or
 * from the game window while the menu is an OSD layer. 1 = handled. */
static int menu_mouse(UINT m, int y)
{
    if (s_ov_kind != KIND_MENU || s_ov_state != OV_MENU) return 0;
    const int row = y >= s_menu_row_top && s_menu_row_h > 0 ? (y - s_menu_row_top) / s_menu_row_h : -1;
    /* The menu opens centred, right under the mouse-look cursor (the
     * window centre is on "Quit to Desktop"): the mouse only takes
     * over once it has really moved, and an unmoved click only selects. */
    int armed = s_menu_mouse_armed;
    if (!armed) {
        POINT p; GetCursorPos(&p);
        if (m != WM_MOUSEMOVE || abs(p.x - s_menu_mouse0.x) + abs(p.y - s_menu_mouse0.y) >= 8) s_menu_mouse_armed = 1;
        if (!s_menu_mouse_armed) return 1;
    }
    if (row >= 0 && row < menu_rows()) {
        if (row != s_menu_sel) { s_menu_sel = row; ov_refresh(); }
        if (!armed) return 1;
        if (m == WM_LBUTTONDOWN) menu_activate(1);   /* left click: OK / next value */
        if (m == WM_RBUTTONDOWN && s_menu_page == 1 && row < OPT_COUNT) menu_activate(-1);   /* right click: previous value */
    }
    return 1;
}
static void ov_draw(HDC mdc, const RECT& rc)
{
    HBRUSH bg = CreateSolidBrush(RGB(16, 16, 20)); FillRect(mdc, &rc, bg); DeleteObject(bg);
    HBRUSH fr = CreateSolidBrush(s_ov_toast ? RGB(90, 90, 100) : RGB(190, 40, 30));
    for (int i = 0; i < 2; i++) { RECT r = { rc.left + i, rc.top + i, rc.right - i, rc.bottom - i }; FrameRect(mdc, &r, fr); }
    DeleteObject(fr);
    SetBkMode(mdc, TRANSPARENT);
    if (s_ov_kind == KIND_MENU) paint_menu(mdc, rc);
    else {
        const int H = rc.bottom - rc.top;
        HFONT f1 = CreateFontW(-(s_ov_toast ? H * 30 / 100 : H * 24 / 100), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT f2 = CreateFontW(-(s_ov_toast ? H * 20 / 100 : H * 14 / 100), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ of = SelectObject(mdc, f1);
        RECT r1 = { rc.left + 8, rc.top + H * 12 / 100, rc.right - 8, rc.top + H * 55 / 100 };
        SetTextColor(mdc, RGB(255, 255, 255));
        DrawTextW(mdc, s_ov_l1, -1, &r1, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mdc, f2);
        RECT r2 = { rc.left + 8, rc.top + H * 55 / 100, rc.right - 8, rc.bottom - H * 10 / 100 };
        SetTextColor(mdc, RGB(200, 200, 205));
        DrawTextW(mdc, s_ov_l2, -1, &r2, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mdc, of); DeleteObject(f1); DeleteObject(f2);
    }
}
static LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (menu_mouse(m, (short)HIWORD(l))) return 0;
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        /* double-buffered: the menu repaints on every key press */
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, rc.right - rc.left, rc.bottom - rc.top);
        HGDIOBJ obm = SelectObject(mdc, bm);
        ov_draw(mdc, rc);
        BitBlt(dc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, obm); DeleteObject(bm); DeleteDC(mdc);
        EndPaint(h, &ps);
        return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}

/* Shown as an OSD layer in the game's swapchain (port_osd.h) when that is up, so
 * exclusive fullscreen shows it too; else as the layered window. Placement in
 * game-window client coordinates. */
static int s_ovr_shown = 0, s_ovr_osd = 0, s_ovr_x = 0, s_ovr_y = 0, s_ovr_w = 0, s_ovr_h = 0, s_ovr_cw = 0, s_ovr_ch = 0;
static void ov_show_kind(const wchar_t* l1, const wchar_t* l2, int kind)
{
    if (l1) wcsncpy_s(s_ov_l1, l1, _TRUNCATE);
    if (l2) wcsncpy_s(s_ov_l2, l2, _TRUNCATE);
    s_ov_kind = kind;
    s_ov_toast = kind == KIND_TOAST || kind == KIND_HINT;
    RECT rc; GetClientRect(s_game, &rc);
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    int w, h, x, y;
    if (kind == KIND_MENU) {
        const int rh = ch * 55 / 1000 < 34 ? 34 : ch * 55 / 1000;
        w = cw * 44 / 100; if (w < 560) w = 560;
        h = rh * (menu_rows() + 4);
        if (w > cw) w = cw;
        if (h > ch) h = ch;
        x = (cw - w) / 2; y = (ch - h) / 2;
    } else if (kind == KIND_HINT) {
        w = cw * 30 / 100; if (w < 380) w = 380;
        h = ch * 7 / 100; if (h < 54) h = 54;
        x = (cw - w) / 2; y = ch - h - ch * 4 / 100;
    } else {
        const int toast = kind == KIND_TOAST;
        w = toast ? cw * 30 / 100 : cw * 46 / 100; h = toast ? ch * 9 / 100 : ch * 22 / 100;
        if (w < (toast ? 260 : 420)) w = toast ? 260 : 420;
        if (h < (toast ? 60 : 130)) h = toast ? 60 : 130;
        x = (cw - w) / 2; y = toast ? ch * 6 / 100 : (ch - h) / 2;
    }
    s_ovr_shown = 1; s_ovr_x = x; s_ovr_y = y; s_ovr_w = w; s_ovr_h = h; s_ovr_cw = cw; s_ovr_ch = ch;
    s_ovr_osd = posd_on();
    if (s_ovr_osd) {
        if (s_ov && IsWindowVisible(s_ov)) ShowWindow(s_ov, SW_HIDE);
        PosdCanvas c;
        if (!c.begin(w, h)) return;
        const RECT r = { 0, 0, w, h };
        ov_draw(c.dc, r);
        c.commit(POSD_PCMENU, x, y, cw, ch, 235 / 255.f);
        return;
    }
    r2rsx_osd_hide(POSD_PCMENU);
    if (!s_ov) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.lpfnWndProc = ov_proc; wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = L"r3pc_overlay";
        RegisterClassExW(&wc);
        s_ov = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
                               L"r3pc_overlay", L"", WS_POPUP, 0, 0, 10, 10, s_game, NULL, wc.hInstance, NULL);
        if (s_ov) SetLayeredWindowAttributes(s_ov, 0, 235, LWA_ALPHA);
    }
    if (!s_ov) return;
    POINT tl = { 0, 0 }; ClientToScreen(s_game, &tl);
    SetWindowPos(s_ov, HWND_TOPMOST, tl.x + x, tl.y + y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(s_ov, NULL, TRUE);
    UpdateWindow(s_ov);
}
static void ov_show(const wchar_t* l1, const wchar_t* l2, int toast) { ov_show_kind(l1, l2, toast ? KIND_TOAST : KIND_DIALOG); }
static void ov_hide(void)
{
    if (s_ov) ShowWindow(s_ov, SW_HIDE);
    r2rsx_osd_hide(POSD_PCMENU);
    s_ovr_shown = 0; s_ov_toast = 0; s_toast_until = 0; s_ov_kind = KIND_DIALOG;
}
/* the content changed (menu selection, a value) */
static void ov_refresh(void)
{
    if (!s_ovr_shown) return;
    if (s_ovr_osd) ov_show_kind(NULL, NULL, s_ov_kind);
    else if (s_ov) InvalidateRect(s_ov, NULL, FALSE);
}
/* per tick: follow a change of the client size or of the OSD becoming ready */
static void ov_follow(void)
{
    if (!s_ovr_shown) return;
    RECT rc; GetClientRect(s_game, &rc);
    if (posd_on() != s_ovr_osd || rc.right - rc.left != s_ovr_cw || rc.bottom - rc.top != s_ovr_ch) ov_show_kind(NULL, NULL, s_ov_kind);
}
static void menu_show(void) { ov_show_kind(NULL, NULL, KIND_MENU); }
static int s_menu_prev = 0, s_prev_in = 0, s_hint_on = 0;
static void menu_close(void) { s_ov_state = OV_RELEASE; ov_hide(); }
/* OK / click (dir 1) or change an option (dir +-1) on the selected row */
static void menu_activate(int dir)
{
    if (s_ov_state != OV_MENU) return;
    if (s_menu_page == 0) {
        if (dir < 0) return;
        switch (s_menu_sel) {
        case 0: menu_close(); break;
        case 1: s_menu_page = 1; s_menu_sel = 0; menu_show(); break;
        case 2: s_ov_state = OV_NONE; ov_hide(); r2x_open_menu(); break;   /* r3_pcextra.cpp holds the input from here */
        case 3:
            s_ask_from_menu = 1; s_ov_state = OV_ASK; s_prev_in = s_menu_prev | 1 /* IN_YES */;
            ov_show(L"Quit to desktop?", L"Enter / A = Yes        Esc / B = No", 0);
            fprintf(stderr, "[pcport] quit prompt (PC menu)%c", 10);
            break;
        }
        return;
    }
    if (s_menu_sel >= OPT_COUNT) { if (dir > 0) { s_menu_page = 0; s_menu_sel = 1; menu_show(); } return; }
    opt_step(s_menu_sel, dir);
    ov_refresh();
}

/* ---- screenshots -------------------------------------------------------- */
struct Shot { int w, h; std::vector<uint8_t> bgr; wchar_t path[MAX_PATH]; };
static DWORD WINAPI shot_thread(LPVOID p)
{
    Shot* s = (Shot*)p;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const bool co = SUCCEEDED(hr);
    IWICImagingFactory* f = NULL; IWICStream* st = NULL; IWICBitmapEncoder* enc = NULL; IWICBitmapFrameEncode* fr = NULL;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(s->path, GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc)) &&
        SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, NULL)) &&
        SUCCEEDED(fr->Initialize(NULL)) && SUCCEEDED(fr->SetSize((UINT)s->w, (UINT)s->h))) {
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        const UINT stride = ((UINT)s->w * 3 + 3) & ~3u;
        ok = SUCCEEDED(fr->SetPixelFormat(&pf)) && IsEqualGUID(pf, GUID_WICPixelFormat24bppBGR) &&
             SUCCEEDED(fr->WritePixels((UINT)s->h, stride, (UINT)s->bgr.size(), s->bgr.data())) &&
             SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (f) f->Release();
    if (!ok) DeleteFileW(s->path);
    fprintf(stderr, "[pcport] screenshot %s: %ls%c", ok ? "saved" : "FAILED", s->path, 10);
    if (co) CoUninitialize();
    delete s;
    return 0;
}
static void take_screenshot(void)
{
    RECT rc; if (!GetClientRect(s_game, &rc)) return;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0 || IsIconic(s_game)) return;
    POINT tl = { 0, 0 }; ClientToScreen(s_game, &tl);
    const int osd = posd_on();
    if (osd) posd_suppress_for_capture();   /* a frame without the OSD layers on screen */
    HDC sdc = GetDC(NULL); HDC mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    Shot* s = NULL;
    if (bm && bits) {
        HGDIOBJ ob = SelectObject(mdc, bm);
        /* the screen as the desktop composes it (no CAPTUREBLT: our own
         * layered prompt/toast windows are left out; the OSD is off above) */
        if (BitBlt(mdc, 0, 0, w, h, sdc, tl.x, tl.y, SRCCOPY)) {
            GdiFlush();
            s = new Shot; s->w = w; s->h = h;
            const size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;
            s->bgr.assign(stride * h, 0);
            const uint8_t* src = (const uint8_t*)bits;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                const uint8_t* q = src + ((size_t)y * w + x) * 4; uint8_t* o = &s->bgr[y * stride + x * 3];
                o[0] = q[0]; o[1] = q[1]; o[2] = q[2];
            }
        }
        SelectObject(mdc, ob);
    }
    if (osd) r2rsx_osd_suppress(0);
    if (bm) DeleteObject(bm);
    DeleteDC(mdc); ReleaseDC(NULL, sdc);
    if (!s) { fprintf(stderr, "[pcport] screenshot: could not read the screen%c", 10); return; }
    wchar_t dir[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, dir);
    wcscat_s(dir, L"\\screenshots");
    CreateDirectoryW(dir, NULL);
    SYSTEMTIME t; GetLocalTime(&t);
    swprintf_s(s->path, L"%s\\R3_%04u%02u%02u_%02u%02u%02u.png", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    for (int n = 2; GetFileAttributesW(s->path) != INVALID_FILE_ATTRIBUTES && n < 100; n++)
        swprintf_s(s->path, L"%s\\R3_%04u%02u%02u_%02u%02u%02u_%d.png", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, n);
    const wchar_t* name = wcsrchr(s->path, L'\\'); name = name ? name + 1 : s->path;
    wchar_t l2[160]; swprintf_s(l2, L"screenshots\\%s", name);
    HANDLE th = CreateThread(NULL, 0, shot_thread, s, 0, NULL);
    if (th) CloseHandle(th); else delete s;
    if (s_ov_state == OV_NONE) { ov_show(L"Screenshot saved", l2, 1); s_toast_until = GetTickCount64() + 1800; }
}

/* ---- quitting ----------------------------------------------------------- */
static void wait_for_saves(DWORD max_ms)
{
    const ULONGLONG t0 = GetTickCount64();
    int said = 0;
    while (g_savedata_busy > 0 && GetTickCount64() - t0 < max_ms) {
        if (!said) { said = 1; fprintf(stderr, "[pcport] waiting for the save in progress before closing%c", 10); }
        Sleep(20);
    }
    sys_fs_flush_all();
    fflush(stdout); fflush(stderr);
}
static void close_hook(void* hwnd)
{
    (void)hwnd;
    if (s_ov_state != OV_SAVING) { s_ov_state = OV_SAVING; ov_show(L"Closing...", L"Finishing any save in progress", 0); }
    wait_for_saves(15000);
    fprintf(stderr, "[pcport] closing cleanly%c", 10);
}

/* ---- input for the prompt ----------------------------------------------- */
/* All controllers' buttons (XInput bits), the left stick as the d-pad; at most
 * every 15 ms. */
extern "C" unsigned cellpad_menu_buttons(int* any_connected);   /* cellPad.c: SDL3 or XInput */
static WORD xi_buttons(void) { return (WORD)cellpad_menu_buttons(NULL); }
enum { IN_YES = 1, IN_NO = 2, IN_ANY = 4, IN_UP = 8, IN_DOWN = 16, IN_LEFT = 32, IN_RIGHT = 64 };
static int prompt_input(int focused)
{
    int r = 0;
    if (focused) {
        auto kd = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        if (kd(VK_RETURN) || kd(VK_SPACE)) r |= IN_YES;
        if (kd(VK_ESCAPE) || kd(VK_BACK)) r |= IN_NO;
        if (kd(VK_F10)) r |= IN_ANY;
        if (kd(VK_UP) || kd('W')) r |= IN_UP;
        if (kd(VK_DOWN) || kd('S')) r |= IN_DOWN;
        if (kd(VK_LEFT) || kd('A')) r |= IN_LEFT;
        if (kd(VK_RIGHT) || kd('D')) r |= IN_RIGHT;
    }
    const WORD b = xi_buttons();
    if (b & XINPUT_GAMEPAD_A) r |= IN_YES;
    if (b & XINPUT_GAMEPAD_B) r |= IN_NO;
    if (b & (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK)) r |= IN_ANY;
    if (b & XINPUT_GAMEPAD_DPAD_UP) r |= IN_UP;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN) r |= IN_DOWN;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT) r |= IN_LEFT;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT) r |= IN_RIGHT;
    return r;
}
/* new presses, plus auto-repeat of a held direction (400 ms, then every 110 ms) */
static int menu_edges(int in, int* prev)
{
    static ULONGLONG rep_at = 0;
    const ULONGLONG now = GetTickCount64();
    int e = in & ~*prev;
    const int dirs = in & (IN_UP | IN_DOWN | IN_LEFT | IN_RIGHT);
    if (e & dirs) rep_at = now + 400;
    else if (dirs && rep_at && now >= rep_at) { e |= dirs; rep_at = now + 110; }
    if (!dirs) rep_at = 0;
    *prev = in;
    return e;
}
static int pad_quit_combo(void)                      /* View + Menu held on a controller */
{
    static ULONGLONG held_since = 0; static int fired = 0;
    const WORD m = XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK;
    if ((xi_buttons() & m) != m) { held_since = 0; fired = 0; return 0; }
    const ULONGLONG now = GetTickCount64();
    if (!held_since) held_since = now;
    if (!fired && now - held_since >= 1500) { fired = 1; return 1; }   /* once per hold */
    return 0;
}

static int host_focused(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() && !IsIconic(s_game);
}

static LRESULT CALLBACK game_subproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_KEYUP: case WM_SYSKEYUP:
        if (w == VK_SNAPSHOT) s_shot_req = 1;
        break;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && (g_r2pc_paused || g_port_ui_modal || !host_focused())) {
            SetCursor(LoadCursor(NULL, IDC_ARROW)); return TRUE; }
        break;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
        if (s_ovr_shown && s_ovr_osd) {                /* the PC menu as an OSD layer: the clicks land here */
            const int x = (short)LOWORD(l), y = (short)HIWORD(l);
            if (x >= s_ovr_x && x < s_ovr_x + s_ovr_w && y >= s_ovr_y && y < s_ovr_y + s_ovr_h) menu_mouse(m, y - s_ovr_y);
        }
        break;
    case WM_ACTIVATEAPP:
        if (!w) ClipCursor(NULL);
        break;
    case WM_KILLFOCUS:
        ClipCursor(NULL);
        break;
    }
    return CallWindowProcA(s_orig_proc, h, m, w, l);
}

static void ui_tick(void* hwnd_)
{
    HWND hwnd = (HWND)hwnd_;
    if (s_game != hwnd) {
        s_game = hwnd;
        s_orig_proc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)game_subproc);
    }
    if (s_bg < 0) {
        const char* e = getenv("R2_BACKGROUND");
        s_bg = (e && !_stricmp(e, "run")) ? BG_RUN : (e && !_stricmp(e, "mute")) ? BG_MUTE : BG_PAUSE;
        fprintf(stderr, "[pcport] window in the background: %s; F10 quit, F12 / PrtScn screenshot%c",
                s_bg == BG_RUN ? "keeps running" : s_bg == BG_MUTE ? "muted" : "paused and muted", 10);
    }
    const ULONGLONG now = GetTickCount64();
    const int focused = host_focused();

    /* focus: release the mouse at once; pause / mute as configured */
    { static int was = 1;
      if (was && !focused) { ClipCursor(NULL); fprintf(stderr, "[pcport] window lost focus%c", 10); }
      if (!was && focused) fprintf(stderr, "[pcport] window focused%c", 10);
      was = focused; }

    /* screenshot */
    { static int was = 0; const int d = focused && (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
      if (d && !was) s_shot_req = 1;
      was = d; }
    if (s_shot_req) { s_shot_req = 0; take_screenshot(); }
    if (s_toast_until && now >= s_toast_until && s_ov_state == OV_NONE) ov_hide();

    /* the PC menu / quit prompt */
    if (s_menu_on < 0) {
        const char* e = getenv("R2_MENU_PC");
        s_menu_on = (e && *e) ? atoi(e) != 0 : 1;
        const char* h = getenv("R2_MENU_PC_HINT"); const char* dm = getenv("RSX_DISPLAY");
        s_hint_on = !s_menu_on ? 0 : (h && *h) ? atoi(h) != 0 : (!(dm && !_stricmp(dm, "exclusive")) || r2rsx_osd_wanted());
        fprintf(stderr, "[pcport] PC menu (F10 / hold View+Menu): %s, hint %s%c", s_menu_on ? "on" : "off (quit prompt only)",
                s_hint_on ? "on" : "off", 10);
    }
    const int in = prompt_input(focused);
    switch (s_ov_state) {
    case OV_NONE: {
        static int was = 0; const int d = focused && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        const int open = ((d && !was) || pad_quit_combo()) && !r2x_menu_open();
        was = d;
        if (open) {
            g_port_ui_modal = 1;
            ClipCursor(NULL);
            if (s_menu_on) {
                opts_init();
                s_ov_state = OV_MENU; s_menu_page = 0; s_menu_sel = 0; s_menu_prev = in;
                menu_mouse_reset();
                menu_show();
                fprintf(stderr, "[pcport] PC menu%c", 10);
            } else {
                s_ov_state = OV_ASK; s_prev_in = in; s_ask_from_menu = 0;
                ov_show(L"Quit to desktop?", L"Enter / A = Yes        Esc / B = No", 0);
                fprintf(stderr, "[pcport] quit prompt%c", 10);
            }
        }
        break; }
    case OV_MENU: {
        const int edge = menu_edges(in, &s_menu_prev);
        const int n = menu_rows();
        if (edge & IN_UP) { s_menu_sel = (s_menu_sel + n - 1) % n; ov_refresh(); }
        else if (edge & IN_DOWN) { s_menu_sel = (s_menu_sel + 1) % n; ov_refresh(); }
        else if (edge & (IN_LEFT | IN_RIGHT)) {
            if (s_menu_page == 1 && s_menu_sel < OPT_COUNT) menu_activate(edge & IN_LEFT ? -1 : 1);
        }
        else if (edge & IN_YES) menu_activate(1);
        else if (edge & IN_ANY) menu_close();
        else if (edge & IN_NO) {
            if (s_menu_page == 1) { s_menu_page = 0; s_menu_sel = 1; menu_show(); }
            else menu_close();
        }
        break; }
    case OV_ASK: {
        const int edge = in & ~s_prev_in;
        s_prev_in = in;
        if (edge & IN_YES) {
            s_ov_state = OV_SAVING; s_save_t0 = now;
            ov_show(L"Quitting...", L"Finishing any save in progress", 0);
        } else if (edge & (IN_NO | IN_ANY)) {
            if (s_ask_from_menu && !(edge & IN_ANY)) {   /* No: back to the PC menu */
                s_ask_from_menu = 0; s_ov_state = OV_MENU; s_menu_prev = in; menu_mouse_reset(); menu_show();
            } else { s_ask_from_menu = 0; s_ov_state = OV_RELEASE; ov_hide(); }
        }
        break; }
    case OV_SAVING:
        if (g_savedata_busy <= 0 || now - s_save_t0 > 15000) {
            if (g_savedata_busy > 0) fprintf(stderr, "[pcport] save still busy after 15 s: closing anyway%c", 10);
            sys_fs_flush_all();
            fprintf(stderr, "[pcport] quit to desktop%c", 10);
            PostMessageA(s_game, WM_CLOSE, 0, 0);
            s_ov_state = OV_SAVING + 100;            /* closing; nothing more to do */
        }
        break;
    case OV_RELEASE:
        /* keep the game's input blocked until the answer key is let go, or
         * the Esc / B that said "no" would reach the game as a fresh press */
        if (!(in & (IN_YES | IN_NO | IN_ANY))) { s_ov_state = OV_NONE; g_port_ui_modal = 0; }
        break;
    }

    ov_follow();
    const int prompt = s_ov_state != OV_NONE;

    /* the hint: while the game's pause menu is up (a pause press during
     * on-foot play, then the look code stops), and once ~12 s after start */
    if (s_hint_on > 0) {
        static ULONGLONG t_first = 0, pause_t = 0;
        if (!t_first) t_first = now;
        const ULONGLONG look = g_r2_look_ms;
        const int playing = look && now - look < 200;
        { static int was = 0;
          const int d = (focused && ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || (GetAsyncKeyState(VK_RETURN) & 0x8000))) ||
                        (xi_buttons() & XINPUT_GAMEPAD_START);
          if (d && !was && playing) pause_t = now;
          was = d; }
        if (pause_t && ((playing && now - pause_t > 600) || now - pause_t > 120000)) pause_t = 0;
        const int want = !prompt && !s_toast_until && focused &&
                         ((pause_t && !playing && now - pause_t > 250) || (!look && now - t_first > 12000 && now - t_first < 22000));
        if (want && s_ov_kind != KIND_HINT)
            ov_show_kind(L"PC menu:  F10   or hold View + Menu", L"PC Options  \u00B7  Quit to Desktop", KIND_HINT);
        else if (!want && s_ov_kind == KIND_HINT && !prompt) ov_hide();
    }

    r2x_ui_tick(hwnd, focused, prompt);   /* r3_pcextra.cpp: extras menu, photo mode, pad-loss pause, cutscene skip */
    const int bg_pause = !focused && s_bg == BG_PAUSE;
    g_r2pc_paused = (prompt || bg_pause || r2x_want_pause()) ? 1 : 0;
    g_port_audio_mute = (g_r2pc_paused || (!focused && s_bg != BG_RUN)) ? 1 : 0;
    { static int was = 0;
      if (g_r2pc_paused != was) { was = g_r2pc_paused; fprintf(stderr, "[pcport] game %s%c", was ? "paused" : "resumed", 10);
          if (was) ClipCursor(NULL); } }
}

/* registered before main() runs; the window calls back once it exists */
static struct PcPortInit {
    PcPortInit() { g_port_ui_tick = ui_tick; g_port_close_hook = close_hook; }
} s_pcport_init;
