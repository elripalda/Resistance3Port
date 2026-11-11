/*
 * ps3recomp - Null RSX Backend (Win32 window + color clear)
 *
 * Implements rsx_backend callbacks using a simple Win32 GDI window.
 * No GPU rendering — just clears to the RSX clear color and presents.
 * Used for debugging command buffer flow before a real backend exists.
 */

#include "rsx_null_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../runtime/ps3_log.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    HWND     hwnd;
    HDC      hdc;
    u32      width;
    u32      height;

    /* Current clear color (ARGB -> COLORREF) */
    COLORREF clear_color;

    /* Frame counter */
    u64      frame_count;
    u64      last_fps_time;
    u32      fps;

    int      window_closed;

    /* Title-bar stats: base caption, and frames/time at the last refresh. */
    char     title[128];
    u64      title_frames;
    ULONGLONG title_time;

    /* Fullscreen = a borderless popup covering the monitor. With RSX_DISPLAY=exclusive
     * RPCS3's swapchain also allows Vulkan exclusive fullscreen (r2rsx_bridge.cpp). */
    int      fullscreen;
    RECT     windowed_rect;
} NullBackendState;

static NullBackendState s_state;

/* RSX_RESIZABLE=0: fixed-size window (no resize border / maximize box).
 * RSX_ALT_ENTER=0: Alt+Enter does not toggle fullscreen. Both default on. */
static int nb_env_on(const char* n) { const char* e = getenv(n); return !(e && e[0] == '0'); }
static DWORD nb_win_style(void)
{
    return nb_env_on("RSX_RESIZABLE") ? WS_OVERLAPPEDWINDOW
                                      : (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX));
}

/* PORT_REFRESH_SWITCH (default off, 1 = on; it blanked the screen and glitched audio on focus changes): in exclusive fullscreen
 * (RSX_DISPLAY=exclusive), when the frame-rate target (PORT_FPS_EFFECTIVE, set
 * by the port's r2_fps_target) does not divide the display refresh evenly --
 * 60 fps on 144 Hz shows frames for 2, then 3 refreshes -- switch the monitor
 * to the closest mode at the same resolution whose refresh is a multiple of the
 * target (144 -> 120 Hz). Temporary (CDS_FULLSCREEN) and given back when the
 * window loses focus, is minimised, leaves fullscreen, closes, the process
 * exits or crashes. Borderless / windowed never switch: there the [fps] note
 * suggests VRR or an even frame rate. With G-Sync / FreeSync it is not needed
 * (harmless: the VRR range still covers the target) -- PORT_REFRESH_SWITCH=0. */
static struct { int active, warned, hooked; char dev[CCHDEVICENAME]; DWORD from_hz, to_hz; } s_rs;
static LPTOP_LEVEL_EXCEPTION_FILTER s_rs_prev_filter;

static double nb_hz(DWORD f) { return (f + 1) % 30 == 0 ? (f + 1) * 1000.0 / 1001.0 : (double)f; }   /* 59 / 119 = NTSC */
static int nb_hz_even(double hz, double fps)
{
    const double q = hz / fps;
    return q >= 0.97 && fabs(q - floor(q + 0.5)) <= 0.03;
}

static void nb_refresh_restore(void)
{
    if (!s_rs.active) return;
    s_rs.active = 0;
    ChangeDisplaySettingsExA(s_rs.dev, NULL, NULL, 0, NULL);   /* back to the registry (desktop) mode */
    { char hz[16]; snprintf(hz, sizeof hz, "%lu", (unsigned long)s_rs.from_hz); _putenv_s("PORT_DISPLAY_HZ", hz); }   /* [hitch] present stats */
    fprintf(stderr, "[fps] display restored %lu->%lu Hz%c", (unsigned long)s_rs.to_hz, (unsigned long)s_rs.from_hz, 10);
}
static void nb_refresh_atexit(void) { nb_refresh_restore(); }
static LONG WINAPI nb_refresh_crash(EXCEPTION_POINTERS* ep)
{
    nb_refresh_restore();
    return s_rs_prev_filter ? s_rs_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}
/* The port's crash reporter (project\port_crash.h) calls this before it writes
 * a report, on every crash path -- also stack overflows and abort(), which never
 * reach the filter above -- so a crash does not leave the monitor switched. */
void rsx_null_refresh_restore_on_crash(void) { nb_refresh_restore(); }

static void nb_refresh_apply(HWND hwnd)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PORT_REFRESH_SWITCH"); on = e && e[0] == '1'; }
    const char* dm = getenv("RSX_DISPLAY");
    if (!on || s_rs.active || !hwnd || !s_state.fullscreen || !dm || _stricmp(dm, "exclusive")) return;
    const char* fe = getenv("PORT_FPS_EFFECTIVE");
    const char* rf = getenv("R2_FPS");   /* before r2_fps_target ran: plain numbers only, unset = 30 */
    const double fps = fe ? atof(fe) : !rf || !*rf ? 30.0 : (rf[0] >= '1' && rf[0] <= '9') ? atof(rf) : 0.0;
    if (fps < 20.0) return;   /* uncapped / unknown */
    MONITORINFOEXA mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO*)&mi)) return;
    DEVMODEA cur; memset(&cur, 0, sizeof cur); cur.dmSize = sizeof cur;
    if (!EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur) || cur.dmDisplayFrequency < 24) return;
    if (nb_hz_even(nb_hz(cur.dmDisplayFrequency), fps)) return;
    DEVMODEA best; memset(&best, 0, sizeof best);
    double bd = 1e9;
    for (DWORD i = 0;; i++) {
        DEVMODEA m; memset(&m, 0, sizeof m); m.dmSize = sizeof m;
        if (!EnumDisplaySettingsA(mi.szDevice, i, &m)) break;
        if (m.dmPelsWidth != cur.dmPelsWidth || m.dmPelsHeight != cur.dmPelsHeight || m.dmBitsPerPel != cur.dmBitsPerPel ||
            (m.dmDisplayFlags & DM_INTERLACED) || m.dmDisplayFrequency < 24 || !nb_hz_even(nb_hz(m.dmDisplayFrequency), fps)) continue;
        const double d = fabs((double)m.dmDisplayFrequency - (double)cur.dmDisplayFrequency);
        if (d < bd || (d == bd && m.dmDisplayFrequency > best.dmDisplayFrequency)) { bd = d; best = m; }
    }
    if (!best.dmDisplayFrequency) {
        if (!s_rs.warned) fprintf(stderr, "[fps] note: no %lux%lu mode refreshes at a multiple of %.0f fps; turn on G-Sync / FreeSync or pick an even frame rate%c",
                                  (unsigned long)cur.dmPelsWidth, (unsigned long)cur.dmPelsHeight, fps, 10);
        s_rs.warned = 1;
        return;
    }
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    const LONG rc = ChangeDisplaySettingsExA(mi.szDevice, &best, NULL, CDS_FULLSCREEN, NULL);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        if (!s_rs.warned) fprintf(stderr, "[fps] display switch to %lu Hz failed (%ld)%c", (unsigned long)best.dmDisplayFrequency, (long)rc, 10);
        s_rs.warned = 1;
        return;
    }
    memcpy(s_rs.dev, mi.szDevice, sizeof s_rs.dev);
    s_rs.from_hz = cur.dmDisplayFrequency; s_rs.to_hz = best.dmDisplayFrequency; s_rs.active = 1;
    { char hz[16]; snprintf(hz, sizeof hz, "%lu", (unsigned long)s_rs.to_hz); _putenv_s("PORT_DISPLAY_HZ", hz); }
    if (!s_rs.hooked) { s_rs.hooked = 1; atexit(nb_refresh_atexit); s_rs_prev_filter = SetUnhandledExceptionFilter(nb_refresh_crash); }
    fprintf(stderr, "[fps] display switched %lu->%lu Hz (%.0f fps target; PORT_REFRESH_SWITCH=0 keeps the desktop rate)%c",
            (unsigned long)s_rs.from_hz, (unsigned long)s_rs.to_hz, fps, 10);
}

static void nb_set_fullscreen(HWND hwnd, int on)
{
    if (on) {
        if (!s_state.fullscreen) GetWindowRect(hwnd, &s_state.windowed_rect);
        MONITORINFO mi = { sizeof mi };
        GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrA(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    } else {
        const RECT r = s_state.windowed_rect;
        SetWindowLongPtrA(hwnd, GWL_STYLE, nb_win_style() | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
    s_state.fullscreen = on;
    if (on) nb_refresh_apply(hwnd); else nb_refresh_restore();
}

/* ---------------------------------------------------------------------------
 * Win32 window procedure
 * -----------------------------------------------------------------------*/

static int s_present_suppressed = 0;

/* Start-up screen (rsx_null_backend_splash): while the RPCS3 renderer compiles
 * its shader cache before the first frame, nothing has been presented and the
 * GPU swap chain shows nothing, so this window paints a dark progress screen
 * with GDI. Written from the render thread, read on the window thread.
 *   step  -1 dark screen (renderer starting), 1..steps progress, 0 off */
static volatile LONG s_splash_step = 0, s_splash_steps = 0, s_splash_done = 0, s_splash_total = 0;
static volatile LONG s_splash_skip = 0, s_splash_blank_once = 0;
static volatile LONGLONG s_splash_t0 = 0, s_splash_inval = 0;
static HFONT s_splash_font[3];
/* rsx_null_backend_splash_gdi(0): the renderer presents the start-up screen into its swap chain
 * (VKGSRender.cpp preload_shaders, PORT_OSD), which exclusive fullscreen shows and GDI does not.
 * This window then keeps the splash state, the skip keys, the caption and the exit-on-close, but
 * never paints over the swap chain. */
static volatile LONG s_splash_gdi_off = 0;

static void nb_splash_paint(HWND hwnd, HDC hdc)
{
    RECT rc; GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HDC mdc = CreateCompatibleDC(hdc);
    HBITMAP bm = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ obm = SelectObject(mdc, bm);
    HBRUSH bg = CreateSolidBrush(RGB(14, 15, 18));
    FillRect(mdc, &rc, bg); DeleteObject(bg);

    const LONG step = s_splash_step;
    if (step > 0) {
        const unsigned done = (unsigned)s_splash_done, total = (unsigned)s_splash_total;
        const unsigned pct = total ? (unsigned)((unsigned long long)(done < total ? done : total) * 100 / total) : 0;
        const unsigned secs = (unsigned)((GetTickCount64() - (ULONGLONG)s_splash_t0) / 1000);
        int fh = h / 22; if (fh < 18) fh = 18; if (fh > 44) fh = 44;
        if (!s_splash_font[0]) {
            s_splash_font[0] = CreateFontW(-fh, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            s_splash_font[1] = CreateFontW(-(fh * 3 / 5), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            s_splash_font[2] = CreateFontW(-(fh / 2 < 13 ? 13 : fh / 2), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        }
        SetBkMode(mdc, TRANSPARENT);
        const int bw = w * 6 / 10, bx = (w - bw) / 2, cy = h / 2;
        wchar_t t[192];
        HGDIOBJ of = SelectObject(mdc, s_splash_font[0]);
        SetTextColor(mdc, RGB(235, 236, 240));
        swprintf(t, 192, L"Compiling shaders\u2026   %u / %u   (%u%%)", done, total, pct);
        RECT r1 = { bx, cy - fh * 3, bx + bw, cy - fh };
        DrawTextW(mdc, t, -1, &r1, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);

        RECT bar = { bx, cy - fh / 2, bx + bw, cy - fh / 2 + (fh / 2 > 8 ? fh / 2 : 8) };
        HBRUSH back = CreateSolidBrush(RGB(48, 52, 62)); FillRect(mdc, &bar, back); DeleteObject(back);
        if (total) {
            RECT fill = bar; fill.right = bar.left + (LONG)((bar.right - bar.left) * (double)pct / 100.0);
            HBRUSH fb = CreateSolidBrush(RGB(90, 160, 230)); FillRect(mdc, &fill, fb); DeleteObject(fb);
        }

        SelectObject(mdc, s_splash_font[1]);
        SetTextColor(mdc, RGB(175, 180, 190));
        swprintf(t, 192, L"Step %ld of %ld: %s   \u00B7   %u:%02u", (long)step, (long)(s_splash_steps ? s_splash_steps : step),
                 step == 1 && s_splash_steps > 1 ? L"shader programs" : L"pipelines", secs / 60, secs % 60);
        RECT r2 = { bx, bar.bottom + fh / 2, bx + bw, bar.bottom + fh * 3 / 2 };
        DrawTextW(mdc, t, -1, &r2, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

        SelectObject(mdc, s_splash_font[2]);
        SetTextColor(mdc, RGB(130, 135, 145));
        RECT r3 = { bx - bw / 6, r2.bottom + fh, bx + bw + bw / 6, r2.bottom + fh * 4 };
        DrawTextW(mdc, s_splash_skip
                  ? L"Skipping: the remaining shaders compile while you play\u2026"
                  : L"The first start after a graphics driver or game update takes longer.\n"
                    L"Esc, Space or a click: skip (the rest compile while you play)   \u00B7   F10: PC menu / quit",
                  -1, &r3, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(mdc, of);
    }
    BitBlt(hdc, 0, 0, w, h, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, obm);
    DeleteObject(bm);
    DeleteDC(mdc);
}

/* Hooks for a port's PC front-end layer (set from the game project; NULL = none):
 *  g_port_ui_tick(hwnd)    -- every pump of this window's message loop
 *  g_port_close_hook(hwnd) -- before the window closes (Alt+F4, the X button):
 *                             e.g. wait for a save to finish. */
void (*volatile g_port_ui_tick)(void* hwnd) = 0;
void (*volatile g_port_close_hook)(void* hwnd) = 0;
extern volatile int g_port_ui_modal;   /* libs/input/cellPad.c: a port overlay wants the cursor */

static LRESULT CALLBACK null_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        { void (*h)(void*) = g_port_close_hook; if (h) h(hwnd); }
        s_state.window_closed = 1;
        DestroyWindow(hwnd);
        return 0;

    case WM_ACTIVATEAPP:   /* PORT_REFRESH_SWITCH: the desktop rate while another app has focus */
        if (wp) nb_refresh_apply(hwnd); else nb_refresh_restore();
        break;

    case WM_DESTROY:
        nb_refresh_restore();
        PostQuitMessage(0);
        return 0;

    case WM_ERASEBKGND:
        /* A GPU swap chain owns the client area: erasing it with the class
         * brush flashes the window colour over the last presented frame (and
         * a GDI write knocks a fullscreen flip model out of independent flip,
         * which the monitor shows as flicker / a mode change). */
        if (s_present_suppressed || s_splash_step || s_splash_gdi_off) return 1;
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (s_splash_step || InterlockedExchange(&s_splash_blank_once, 0)) {
            if (!s_splash_gdi_off) nb_splash_paint(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        if (s_present_suppressed) {
            /* Validate only; the swap chain repaints on the next flip. */
            EndPaint(hwnd, &ps);
            return 0;
        }

        /* Fill with current clear color */
        HBRUSH brush = CreateSolidBrush(s_state.clear_color);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);

        /* Draw debug overlay text */
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        char buf[128];
        snprintf(buf, sizeof(buf), "ps3recomp null backend | %u FPS | frame %llu",
                 s_state.fps, (unsigned long long)s_state.frame_count);
        TextOutA(hdc, 10, 10, buf, (int)strlen(buf));

        char buf2[64];
        snprintf(buf2, sizeof(buf2), "Clear: #%06X",
                 (unsigned)(s_state.clear_color & 0xFFFFFF));
        TextOutA(hdc, 10, 30, buf2, (int)strlen(buf2));

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) nb_refresh_restore();
        if (LOWORD(lp) && HIWORD(lp)) { s_state.width = LOWORD(lp); s_state.height = HIWORD(lp); }
        break;

    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {      /* Alt+Enter */
            if (!(lp & (1 << 30)) && nb_env_on("RSX_ALT_ENTER")) nb_set_fullscreen(hwnd, !s_state.fullscreen);
            return 0;
        }
        if (wp == VK_F10) return 0;                     /* F10 is a game key, not the menu bar */
        break;
    case WM_SYSKEYUP:
        if (wp == VK_F10) return 0;
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0;                  /* no beep for Alt+Enter */
        break;

    case WM_LBUTTONDOWN:
        if (s_splash_step > 0 && !InterlockedExchange(&s_splash_skip, 1)) {
            printf("[RSX null] start-up shader compile: skip requested (click)\n");
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;

    case WM_KEYDOWN:
        if (s_splash_step > 0 && (wp == VK_ESCAPE || wp == VK_SPACE || wp == VK_RETURN)) {
            if (!InterlockedExchange(&s_splash_skip, 1)) {
                printf("[RSX null] start-up shader compile: skip requested (key)\n");
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        /* Esc is the in-game Start/Pause key; only quit when explicitly asked. */
        if (wp == VK_ESCAPE && getenv("ESC_CLOSES")) {
            s_state.window_closed = 1;
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_INPUT: {
        /* Raw mouse deltas for the keyboard+mouse pad (libs/input/cellPad.c),
         * as the D3D12 window does; this window hosts the RPCS3 renderer. */
        extern volatile long g_kbm_mouse_dx, g_kbm_mouse_dy, g_kbm_wheel, g_kbm_ml_dx, g_kbm_ml_dy;
        RAWINPUT ri; UINT sz = sizeof ri;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE) {
            if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                InterlockedExchangeAdd(&g_kbm_mouse_dx, ri.data.mouse.lLastX);
                InterlockedExchangeAdd(&g_kbm_mouse_dy, ri.data.mouse.lLastY);
                InterlockedExchangeAdd(&g_kbm_ml_dx, ri.data.mouse.lLastX);
                InterlockedExchangeAdd(&g_kbm_ml_dy, ri.data.mouse.lLastY);
            }
            if (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
                InterlockedExchangeAdd(&g_kbm_wheel, (short)ri.data.mouse.usButtonData);
        }
        break; }

    case WM_SETCURSOR: {
        extern volatile int g_kbm_capture;
        if (g_kbm_capture && !g_port_ui_modal && LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break; }
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---------------------------------------------------------------------------
 * Backend callbacks
 * -----------------------------------------------------------------------*/

static int null_init(void* ud, u32 width, u32 height)
{
    (void)ud;
    printf("[RSX null] init(%ux%u)\n", width, height);
    return 0;
}

static void null_shutdown(void* ud)
{
    (void)ud;
    printf("[RSX null] shutdown\n");
}

static void null_begin_frame(void* ud)
{
    (void)ud;
}

static void null_end_frame(void* ud)
{
    (void)ud;
    s_state.frame_count++;

    /* Compute FPS every second */
    ULONGLONG now = GetTickCount64();
    if (now - s_state.last_fps_time >= 1000) {
        /* Simple: just use frame_count delta. For first second, estimate. */
        s_state.fps = (u32)(s_state.frame_count -
                           (s_state.frame_count > 60 ? s_state.frame_count - 60 : 0));
        s_state.last_fps_time = now;
    }
}

static void null_present(void* ud, u32 buffer_id)
{
    /* The live draw engine presents through its own swap chain on this HWND;
     * leaving the GDI blit running underneath would fight it for the window. */
    if (s_present_suppressed) return;
    (void)ud;
    (void)buffer_id;

    /* Trigger a repaint */
    if (s_state.hwnd)
        InvalidateRect(s_state.hwnd, NULL, FALSE);
}

static void null_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;
    (void)flags;
    (void)depth;
    (void)stencil;

    /* Convert RSX ARGB to Win32 COLORREF (BGR) */
    u8 r = (color >> 16) & 0xFF;
    u8 g = (color >> 8) & 0xFF;
    u8 b = color & 0xFF;
    s_state.clear_color = RGB(r, g, b);
}

static void null_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    { static int _n = 0; if (ps3_log_verbose() && _n < 16 && ++_n) printf("[RSX null] set_render_target(format=0x%X, %ux%u)\n",
           state->surface_format, state->surface_clip_w, state->surface_clip_h); }
}

static void null_set_viewport(void* ud, const rsx_state* state)
{
    (void)ud;
    { static int _n = 0; if (ps3_log_verbose() && _n < 16 && ++_n) printf("[RSX null] set_viewport(%u,%u %ux%u)\n",
           state->viewport_x, state->viewport_y,
           state->viewport_w, state->viewport_h); }
}

static void null_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    static int s_draw_log_count = 0;
    if (s_draw_log_count < 20) {
        printf("[RSX null] draw_arrays(prim=%u, first=%u, count=%u)\n",
               primitive, first, count);
        s_draw_log_count++;
    }
}

static void null_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    (void)ud;
    static int s_draw_log_count = 0;
    if (s_draw_log_count < 20) {
        printf("[RSX null] draw_indexed(prim=%u, offset=%u, count=%u)\n",
               primitive, offset, count);
        s_draw_log_count++;
    }
}

/* ---------------------------------------------------------------------------
 * Backend registration
 * -----------------------------------------------------------------------*/

static rsx_backend s_null_backend = {
    .userdata          = &s_state,
    .init              = null_init,
    .shutdown          = null_shutdown,
    .begin_frame       = null_begin_frame,
    .end_frame         = null_end_frame,
    .present           = null_present,
    .set_render_target = null_set_render_target,
    .set_viewport      = null_set_viewport,
    .set_blend         = NULL,
    .set_depth_stencil = NULL,
    .clear             = null_clear,
    .draw_arrays       = null_draw_arrays,
    .draw_indexed      = null_draw_indexed,
    .bind_texture      = NULL,
};

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

static BOOL CALLBACK nb_mon_enum(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    struct NbMon { int want, i, found; RECT work; }* s = (struct NbMon*)lp;
    (void)dc; (void)r;
    if (++s->i != s->want) return TRUE;
    MONITORINFO mi = { sizeof mi };
    if (GetMonitorInfoA(m, &mi)) { s->work = mi.rcWork; s->found = 1; }
    return FALSE;
}

int rsx_null_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.width = width;
    s_state.height = height;
    s_state.clear_color = RGB(0, 0, 64); /* dark blue default */

    /* Register window class */
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = null_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    /* the exe's own icon (resource 1, project/app.rc) in the title bar and taskbar */
    wc.hIcon = LoadIconA(GetModuleHandle(NULL), MAKEINTRESOURCEA(1));
    wc.hIconSm = (HICON)LoadImageA(GetModuleHandle(NULL), MAKEINTRESOURCEA(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);   /* never a white flash before the first frame */
    wc.lpszClassName = "ps3recomp_null";
    RegisterClassExA(&wc);

    /* Real pixels: without DPI awareness Windows would stretch (blur) the window
     * at display scaling above 100%. */
    { typedef BOOL (WINAPI *SetCtx)(HANDLE);
      SetCtx f = (SetCtx)(void*)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
      if (f) f((HANDLE)-4); else SetProcessDPIAware(); }   /* -4 = PER_MONITOR_AWARE_V2 */

    /* Launcher settings: RSX_WINDOW=<w>x<h> windowed client size,
     * RSX_DISPLAY=windowed (default) | borderless | exclusive. */
    { const char* e = getenv("RSX_WINDOW"); unsigned a = 0, b = 0;
      if (e && sscanf(e, "%ux%u", &a, &b) == 2 && a >= 320 && b >= 200) { width = a; height = b; } }
    const char* dm = getenv("RSX_DISPLAY");
    const int start_full = dm && (!_stricmp(dm, "borderless") || !_stricmp(dm, "exclusive") || !_stricmp(dm, "fullscreen"));

    /* Window size from the client area, kept inside the screen and centred.
     * RSX_MONITOR=<n> (1-based, in EnumDisplayMonitors order) picks the
     * monitor; default / 0 = the primary one. */
    RECT work; SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    { const char* e = getenv("RSX_MONITOR"); const int want = e ? atoi(e) : 0;
      if (want > 0) {
          struct NbMon { int want, i, found; RECT work; } s = { want, 0, 0, {0} };
          EnumDisplayMonitors(NULL, NULL, nb_mon_enum, (LPARAM)&s);
          if (s.found) work = s.work;
          else printf("[RSX null] RSX_MONITOR=%d: no such monitor, using the primary one\n", want);
      } }
    RECT wr = {0, 0, (LONG)width, (LONG)height};
    AdjustWindowRect(&wr, nb_win_style(), FALSE);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    if (ww > work.right - work.left) ww = work.right - work.left;
    if (wh > work.bottom - work.top) wh = work.bottom - work.top;
    const int wx = work.left + (work.right - work.left - ww) / 2, wy = work.top + (work.bottom - work.top - wh) / 2;

    s_state.hwnd = CreateWindowExA(
        0, "ps3recomp_null",
        title ? title : "ps3recomp",
        nb_win_style() | (start_full ? 0 : WS_VISIBLE),
        wx, wy, ww, wh,
        NULL, NULL, GetModuleHandle(NULL), NULL);
    if (s_state.hwnd && start_full) nb_set_fullscreen(s_state.hwnd, 1);

    if (!s_state.hwnd) {
        printf("[RSX null] ERROR: CreateWindow failed (%lu)\n", GetLastError());
        return -1;
    }

    { extern void* g_kbm_hwnd;
      g_kbm_hwnd = s_state.hwnd;
      RAWINPUTDEVICE rid = {0};
      rid.usUsagePage = 0x01; rid.usUsage = 0x02;   /* generic desktop / mouse */
      rid.hwndTarget = s_state.hwnd;
      if (!RegisterRawInputDevices(&rid, 1, sizeof rid))
          printf("[RSX null] raw mouse input unavailable (%lu)\n", GetLastError()); }

    s_state.hdc = GetDC(s_state.hwnd);
    s_state.last_fps_time = GetTickCount64();
    snprintf(s_state.title, sizeof(s_state.title), "%s", title ? title : "ps3recomp");
    s_state.title_time = s_state.last_fps_time;

    /* Keep the display awake, as any PC game does. With the monitor asleep a
     * vsync'd Present crawls (~4 fps); guest frames stretch to 250 ms and GH3's
     * Havok step takes huge timesteps, which is how an unattended run stalled
     * in the intro movie while an attended one did not. */
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

    /* Register as the active RSX backend */
    rsx_set_backend(&s_null_backend);

    printf("[RSX null] Window created: %ux%u\n", width, height);
    return 0;
}

void rsx_null_backend_shutdown(void)
{
    if (s_state.hwnd) {
        ReleaseDC(s_state.hwnd, s_state.hdc);
        DestroyWindow(s_state.hwnd);
        s_state.hwnd = NULL;
    }
    rsx_set_backend(NULL);
    printf("[RSX null] Backend shut down after %llu frames\n",
           (unsigned long long)s_state.frame_count);
}

/* Closed during the start-up shader compile: the render thread and its workers
 * are inside the GPU driver, and ExitProcess would run the driver's DLL detach
 * while they are frozen mid-call. End the process outright; nothing of the
 * game has started yet. */
static void nb_exit_if_splash(void)
{
    if (s_splash_step == 0) return;
    printf("[RSX null] window closed during the start-up shader compile: exiting\n");
    fflush(stdout); fflush(stderr);
    nb_refresh_restore();
    TerminateProcess(GetCurrentProcess(), 0);
}

int rsx_null_backend_pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            nb_exit_if_splash();
            return -1;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (s_state.window_closed) nb_exit_if_splash();
    { void (*t)(void*) = g_port_ui_tick; if (t && s_state.hwnd && !s_state.window_closed) t(s_state.hwnd); }
    ULONGLONG now = GetTickCount64();
    if (s_state.hwnd && s_splash_step > 0 && now - s_state.title_time >= 250) {
        const unsigned done = (unsigned)s_splash_done, total = (unsigned)s_splash_total;
        char tb[256];
        snprintf(tb, sizeof(tb), "%s | Compiling shaders %u%% (step %ld of %ld)", s_state.title,
                 total ? (unsigned)((unsigned long long)(done < total ? done : total) * 100 / total) : 0,
                 (long)s_splash_step, (long)(s_splash_steps ? s_splash_steps : s_splash_step));
        SetWindowTextA(s_state.hwnd, tb);
        s_state.title_time = now;
        s_state.title_frames = s_state.frame_count;
        return s_state.window_closed ? -1 : 0;
    }
    /* RPCS3-style caption, once a second: guest FPS (flips), draws in the last
     * live-draw frame, window size. Here on the window's own thread, so the
     * WM_SETTEXT never blocks the renderer. */
    if (s_state.hwnd && now - s_state.title_time >= 1000) {
        extern u32 rsx_live_draw_get_last_draws(void);
        extern double rsx_live_draw_get_present_fps(void);
        /* Live draw owns presentation (and end_frame never runs) when enabled. */
        double fps = rsx_live_draw_get_present_fps();
        if (fps <= 0.0)
            fps = (s_state.frame_count - s_state.title_frames) * 1000.0 /
                  (double)(now - s_state.title_time);
        char tb[256];
        snprintf(tb, sizeof(tb), "%s | FPS: %.2f | draws: %u | %ux%u", s_state.title, fps,
                 rsx_live_draw_get_last_draws(), s_state.width, s_state.height);
        SetWindowTextA(s_state.hwnd, tb);
        s_state.title_frames = s_state.frame_count;
        s_state.title_time = now;
    }
    return s_state.window_closed ? -1 : 0;
}

/* Ported from caner (canersaka) Yakuza-Dead-Souls-EX: the live NV4097->D3D12
 * engine binds its swap chain to this window and then owns presentation, so it
 * needs the HWND and a way to silence the GDI present underneath it. */
void* rsx_null_backend_get_hwnd(void)
{
    return (void*)s_state.hwnd;
}

void rsx_null_backend_suppress_present(int on)
{
    s_present_suppressed = on;
}

int rsx_null_backend_splash(int step, int steps, unsigned done, unsigned total)
{
    const LONG prev = s_splash_step;
    if (step == 0) {
        if (prev == 0) return s_state.hwnd != NULL;
        InterlockedExchange(&s_splash_step, 0);
        InterlockedExchange(&s_splash_blank_once, 1);   /* one dark repaint, no stale 100% text */
        if (s_state.hwnd) {
            if (!s_splash_gdi_off) InvalidateRect(s_state.hwnd, NULL, FALSE);
            s_state.title_time = 0;                     /* the normal caption at the next pump */
        }
        return s_state.hwnd != NULL;
    }
    if (step > 0 && prev <= 0) {
        s_splash_t0 = (LONGLONG)GetTickCount64();
        InterlockedExchange(&s_splash_skip, 0);
    }
    InterlockedExchange(&s_splash_steps, steps);
    InterlockedExchange(&s_splash_done, (LONG)done);
    InterlockedExchange(&s_splash_total, (LONG)total);
    InterlockedExchange(&s_splash_step, step);
    const LONGLONG now = (LONGLONG)GetTickCount64();
    if (s_state.hwnd && !s_splash_gdi_off && (step != prev || done >= total || now - s_splash_inval >= 100)) {
        s_splash_inval = now;
        InvalidateRect(s_state.hwnd, NULL, FALSE);
    }
    return s_state.hwnd != NULL;
}

int rsx_null_backend_splash_skip(void)
{
    return s_splash_skip != 0;
}

void rsx_null_backend_splash_gdi(int on)
{
    InterlockedExchange(&s_splash_gdi_off, on ? 0 : 1);
}

#else /* !_WIN32 */

/* ---------------------------------------------------------------------------
 * Headless software backend
 *
 * No window and no GPU: the RSX target is a plain u32 framebuffer in host
 * memory, and triangles are filled on the CPU. That is enough to answer the
 * one question this backend exists to answer -- did the guest's command
 * stream actually reach a backend and produce the pixels it asked for -- on
 * any platform, including a CI runner with no display and no GPU driver.
 *
 * Linux has no real backend yet (Metal is Apple-only, D3D12 Windows-only), so
 * without this ps3recomp_host cannot be built or run there at all and the
 * whole cellGcm -> RSX -> backend bridge goes unexercised on the platform.
 *
 * ponytail: flat-shaded triangles from the first vertex's colour, no blending,
 * no stencil, no clipping beyond the framebuffer bounds, and the viewport is
 * the whole target. It is a correctness probe, not a renderer. Anything more
 * belongs in a real backend.
 *
 * It does keep a depth buffer, because draw ORDER is one of the things the
 * probe has to be able to answer: without one it draws in submission order and
 * cannot tell a working depth test from a missing one.
 * -----------------------------------------------------------------------*/

#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include <stdlib.h>

typedef struct {
    u32* fb;                /* width * height, 0x00RRGGBB          */
    float* depth;           /* width * height, window z in [0,1]   */
    u32  width, height;
    u32  clear_argb;        /* last NV4097_SET_COLOR_CLEAR_VALUE   */
    u32  last_present;      /* centre pixel of the last presented frame */
    int  presented;
    const rsx_state* state; /* live state, for the draw path       */

    /* One decoded texture unit. The point is not to be a sampler but to
     * prove the shared RSX texture path end to end -- layout, deswizzle,
     * channel order -- on a platform with no GPU at all. */
    u8*  tex;               /* decoded RGBA rows, tex_pitch * tex_h  */
    u32  tex_w, tex_h, tex_pitch;
    int  tex_ready;
} NullSoftState;

static NullSoftState s_soft;

static void nullsw_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud; (void)stencil;                      /* no stencil buffer here */
    s_soft.clear_argb = color;
    /* CLEAR_SURFACE bits: 0x01 depth, 0x02 stencil, 0xF0 the four colour
     * channels. rsx_commands.c decoded `depth` out of ZSTENCIL_CLEAR_VALUE's
     * top 24 bits, so it is already the [0,1] value to store. */
    if (s_soft.depth && (flags & 0x01u))
        for (u32 i = 0; i < s_soft.width * s_soft.height; i++) s_soft.depth[i] = depth;
    if (!s_soft.fb || !(flags & 0xF0u)) return;   /* colour bits only */
    u32 rgb = color & 0x00FFFFFFu;
    for (u32 i = 0; i < s_soft.width * s_soft.height; i++) s_soft.fb[i] = rgb;
}

static void nullsw_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud; s_soft.state = state;
}

static void nullsw_set_vertex_attribs(void* ud, const rsx_state* state)
{
    (void)ud; s_soft.state = state;
}

/* NDC -> framebuffer pixels, plus the window z the depth test compares. The
 * guest's clip-space position is divided by w (1.0 for the identity-transform
 * case) and mapped over the whole target; see the ponytail note above about
 * the viewport. z/w is already the [0,1] depth on this hardware, as it is in
 * D3D and Metal, so it needs no remapping. */
static void ndc_to_px(const float p[4], float* sx, float* sy, float* sz)
{
    float w = (p[3] != 0.0f) ? p[3] : 1.0f;
    *sx = ( p[0] / w * 0.5f + 0.5f) * (float)s_soft.width;
    *sy = (-p[1] / w * 0.5f + 0.5f) * (float)s_soft.height;
    *sz = p[2] / w;
}

/* The NV4097 depth function is the GL enum, 0x200 NEVER .. 0x207 ALWAYS.
 * Anything unrecognised passes, as it does in the D3D12 backend's gcm_cmp --
 * including the 1 rsx_state_init seeds the register with. */
static int depth_passes(u32 func, float src, float dst)
{
    switch (func) {
    case 0x0200: return 0;              /* NEVER    */
    case 0x0201: return src <  dst;     /* LESS     */
    case 0x0202: return src == dst;     /* EQUAL    */
    case 0x0203: return src <= dst;     /* LEQUAL   */
    case 0x0204: return src >  dst;     /* GREATER  */
    case 0x0205: return src != dst;     /* NOTEQUAL */
    case 0x0206: return src >= dst;     /* GEQUAL   */
    default:     return 1;              /* ALWAYS   */
    }
}

static u32 colour_of(const float c[4])
{
    float r = c[0] < 0 ? 0 : (c[0] > 1 ? 1 : c[0]);
    float g = c[1] < 0 ? 0 : (c[1] > 1 ? 1 : c[1]);
    float b = c[2] < 0 ? 0 : (c[2] > 1 ? 1 : c[2]);
    return ((u32)(r * 255.0f + 0.5f) << 16) |
           ((u32)(g * 255.0f + 0.5f) <<  8) |
            (u32)(b * 255.0f + 0.5f);
}

/* Decode the bound guest texture once, through the same rsx_texture_layout /
 * rsx_texture_decode the D3D12 backend uses. Only unit 0 is kept: this exists
 * to check that path produces the right pixels, not to be a texture cache. */
static void nullsw_bind_texture(void* ud, u32 unit, const rsx_texture_state* t)
{
    (void)ud;
    if (unit != 0 || !t) return;

    /* CONTROL0 bit 31 is the unit enable. A disabled unit unbinds. */
    if (!(t->control0 & 0x80000000u) || !t->offset) { s_soft.tex_ready = 0; return; }

    u32 w = (t->image_rect >> 16) & 0xFFFFu;
    u32 h =  t->image_rect        & 0xFFFFu;
    if (!w || !h || w > 4096u || h > 4096u) { s_soft.tex_ready = 0; return; }

    /* SET_TEXTURE_FORMAT as a title writes it: bits [1:0] are the location
     * (1 = local, 2 = main), [15:8] the format byte with its LN/UN flags.
     * The offset is a plain RSX offset. This used to read the format byte
     * from the low bits and a location flag from bit 31 of the offset --
     * a convention only this backend and the host harness shared; the
     * D3D12 backend, which has met real titles, decodes the register. */
    u32 ea = cellGcmResolveLocated((t->format & 3u) == 1u, t->offset);
    if (!vm_base || ea == 0xFFFFFFFFu) { s_soft.tex_ready = 0; return; }
    const u32 fmt = (t->format >> 8) & 0xFFu;

    rsx_tex_layout tl;
    rsx_texture_layout(fmt, w, h, &tl);
    if (tl.compressed) { s_soft.tex_ready = 0; return; }   /* no BC decode here */

    u32 pitch = w * 4u;
    u8* buf = (u8*)realloc(s_soft.tex, (size_t)pitch * h);
    if (!buf) { s_soft.tex_ready = 0; return; }
    s_soft.tex = buf;

    if (tl.fmt == RSX_TEXFMT_R8G8B8A8) {
        rsx_texture_decode(buf, pitch, vm_base + ea, w, h, &tl,
                           rsx_texture_argb_is_rgba());
    } else {
        /* Narrower formats decode to their own width; splay them to RGBA so
         * sampling below has one layout to read. */
        u32 srcp = tl.row_bytes;
        u8* tmp = (u8*)malloc((size_t)srcp * h);
        if (!tmp) { s_soft.tex_ready = 0; return; }
        rsx_texture_decode(tmp, srcp, vm_base + ea, w, h, &tl, 0);
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++) {
                const u8* sp = tmp + (size_t)y * srcp + (size_t)x * tl.bytes_per_texel;
                u8* dp = buf + (size_t)y * pitch + (size_t)x * 4u;
                dp[0] = sp[0];
                dp[1] = tl.bytes_per_texel > 1 ? sp[1] : sp[0];
                dp[2] = tl.bytes_per_texel > 2 ? sp[2] : sp[0];
                dp[3] = 255;
            }
        free(tmp);
    }

    s_soft.tex_w = w; s_soft.tex_h = h; s_soft.tex_pitch = pitch;
    s_soft.tex_ready = 1;
}

/* Nearest-neighbour point sample, wrapping. No filtering, no mip levels, no
 * addressing modes: the guest UVs in the harness land inside [0,1). */
static u32 sample_tex(float u, float v)
{
    if (!s_soft.tex_ready) return 0xFF00FFu;         /* magenta = not bound */
    long sx = (long)(u * (float)s_soft.tex_w);
    long sy = (long)(v * (float)s_soft.tex_h);
    sx %= (long)s_soft.tex_w; if (sx < 0) sx += (long)s_soft.tex_w;
    sy %= (long)s_soft.tex_h; if (sy < 0) sy += (long)s_soft.tex_h;
    const u8* px = s_soft.tex + (size_t)sy * s_soft.tex_pitch + (size_t)sx * 4u;
    return ((u32)px[0] << 16) | ((u32)px[1] << 8) | (u32)px[2];
}

/* Bounding-box scan with the three edge functions; fills when a pixel centre
 * is on the same side of all three edges, so winding does not matter. */
static void fill_triangle(const float a[4], const float b[4], const float c[4],
                          u32 rgb, const float uv[3][4], int textured)
{
    float ax, ay, az, bx, by, bz, cx, cy, cz;
    ndc_to_px(a, &ax, &ay, &az);
    ndc_to_px(b, &bx, &by, &bz);
    ndc_to_px(c, &cx, &cy, &cz);

    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0.0f) return;                       /* degenerate */
    const float inv = 1.0f / area;                  /* barycentric scale */

    /* Depth writes need the test enabled, exactly as on the hardware: with
     * SET_DEPTH_TEST_ENABLE off the RSX writes no depth whatever the mask
     * says, and both real backends express that the same way. */
    const rsx_state* dst = s_soft.state;
    const int ztest  = s_soft.depth && dst && dst->depth_test_enable;
    const int zwrite = ztest && dst->depth_mask;
    const u32 zfunc  = dst ? dst->depth_func : 0u;

    /* Bounding box. Plain casts truncate toward zero rather than flooring,
     * which only differs for negative coordinates -- and those clamp to 0 on
     * the next line anyway. +1 on the max side covers the truncation. Keeps
     * this file off libm. */
    float lox = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
    float hix = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    float loy = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
    float hiy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);

    long x0 = (long)lox,       y0 = (long)loy;
    long x1 = (long)hix + 1,   y1 = (long)hiy + 1;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > (long)s_soft.width)  x1 = (long)s_soft.width;
    if (y1 > (long)s_soft.height) y1 = (long)s_soft.height;

    for (long y = y0; y < y1; y++) {
        for (long x = x0; x < x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
            float w1 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
            float w2 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
            int neg = (w0 < 0) || (w1 < 0) || (w2 < 0);
            int pos = (w0 > 0) || (w1 > 0) || (w2 > 0);
            if (neg && pos) continue;               /* outside */

            /* Barycentric weights fall out of the same edge functions, so
             * interpolating costs nothing extra. Screen-space rather than
             * perspective-correct: the probe's geometry is drawn in a plane,
             * where the two agree. */
            float la = w1 * inv, lb = w2 * inv, lc = w0 * inv;

            if (ztest) {
                float z = la * az + lb * bz + lc * cz;
                float* zp = &s_soft.depth[(u32)y * s_soft.width + (u32)x];
                if (!depth_passes(zfunc, z, *zp)) continue;
                if (zwrite) *zp = z;
            }

            u32 out = rgb;
            if (textured) {
                float u = la * uv[0][0] + lb * uv[1][0] + lc * uv[2][0];
                float v = la * uv[0][1] + lb * uv[1][1] + lc * uv[2][1];
                out = sample_tex(u, v);
            }
            s_soft.fb[(u32)y * s_soft.width + (u32)x] = out;
        }
    }
}

/* Map sequence position -> guest vertex index, expanding the primitives the
 * hardware has and a triangle list does not. Returns the triangle count and
 * writes 3 indices per triangle through `emit`. */
static void draw_prim(u32 prim, u32 first, u32 count)
{
    const rsx_state* st = s_soft.state;
    if (!st || !s_soft.fb || count < 3) return;

    /* Position is attribute 0 and diffuse colour attribute 3 -- the same slots
     * the D3D12 and Metal fallback paths assume. */
    /* Texture coordinates are attribute 8 (texcoord0), the slot the D3D12 and
     * Metal fetch paths also read. A draw is textured when a texture is bound
     * AND that attribute is actually supplied. */
    const int textured = s_soft.tex_ready && st->vertex_attribs[8].enabled;

    #define TRI(i0, i1, i2) do {                                        \
        float p0[4], p1[4], p2[4], col[4], uv[3][4];                    \
        rsx_fetch_attrib(st, 0, (i0), p0);                              \
        rsx_fetch_attrib(st, 0, (i1), p1);                              \
        rsx_fetch_attrib(st, 0, (i2), p2);                              \
        rsx_fetch_attrib(st, 3, (i0), col);                             \
        rsx_fetch_attrib(st, 8, (i0), uv[0]);                           \
        rsx_fetch_attrib(st, 8, (i1), uv[1]);                           \
        rsx_fetch_attrib(st, 8, (i2), uv[2]);                           \
        fill_triangle(p0, p1, p2, colour_of(col), uv, textured);        \
    } while (0)

    switch (prim) {
    case RSX_PRIMITIVE_TRIANGLES:
        for (u32 i = 0; i + 2 < count; i += 3)
            TRI(first + i, first + i + 1, first + i + 2);
        break;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 i = 0; i + 2 < count; i++)
            TRI(first + i, first + i + 1 + (i & 1), first + i + 2 - (i & 1));
        break;
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 i = 1; i + 1 < count; i++)
            TRI(first, first + i, first + i + 1);
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 i = 0; i + 3 < count; i += 4) {
            TRI(first + i, first + i + 1, first + i + 2);
            TRI(first + i, first + i + 2, first + i + 3);
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 i = 0; i + 3 < count; i += 2) {
            TRI(first + i,     first + i + 1, first + i + 3);
            TRI(first + i,     first + i + 3, first + i + 2);
        }
        break;
    default:
        break;   /* points and lines contribute no coverage here */
    }
    #undef TRI
}

static void nullsw_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    draw_prim(primitive, first, count);
}

static void nullsw_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    /* ponytail: index buffers are not read; the probe's geometry is
     * non-indexed. A backend that needs them reads them for real. */
    (void)ud; (void)primitive; (void)offset; (void)count;
}

static void nullsw_present(void* ud, u32 buffer_id)
{
    (void)ud; (void)buffer_id;
    if (!s_soft.fb) return;
    s_soft.last_present =
        s_soft.fb[(s_soft.height / 2) * s_soft.width + s_soft.width / 2];
    s_soft.presented++;
}

static rsx_backend s_nullsw_backend = {
    .userdata          = &s_soft,
    .set_render_target = nullsw_set_render_target,
    .set_vertex_attribs= nullsw_set_vertex_attribs,
    .clear             = nullsw_clear,
    .bind_texture      = nullsw_bind_texture,
    .draw_arrays       = nullsw_draw_arrays,
    .draw_indexed      = nullsw_draw_indexed,
    .present           = nullsw_present,
};

int rsx_null_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_soft, 0, sizeof(s_soft));
    s_soft.width  = width  ? width  : 1280;
    s_soft.height = height ? height : 720;
    s_soft.fb = (u32*)calloc((size_t)s_soft.width * s_soft.height, sizeof(u32));
    if (!s_soft.fb) return -1;
    /* The far plane, not calloc's zero: zero is the NEAR plane, so a guest
     * that enables the depth test before its first depth clear would draw
     * nothing at all under LESS. */
    s_soft.depth = (float*)malloc((size_t)s_soft.width * s_soft.height * sizeof(float));
    if (!s_soft.depth) { free(s_soft.fb); s_soft.fb = NULL; return -1; }
    for (u32 i = 0; i < s_soft.width * s_soft.height; i++) s_soft.depth[i] = 1.0f;
    rsx_set_backend(&s_nullsw_backend);
    printf("[RSX null] headless software backend %ux%u (%s)\n",
           s_soft.width, s_soft.height, title ? title : "");
    return 0;
}

void rsx_null_backend_shutdown(void)
{
    rsx_set_backend(NULL);
    free(s_soft.fb);    s_soft.fb    = NULL;
    free(s_soft.depth); s_soft.depth = NULL;
    free(s_soft.tex);   s_soft.tex   = NULL;
    s_soft.tex_ready = 0;
}

/* Headless has no event queue and no window to close. */
int rsx_null_backend_pump_messages(void) { return 0; }

void rsx_null_backend_present(void) { nullsw_present(NULL, 0); }
void* rsx_null_backend_get_hwnd(void) { return 0; }
void  rsx_null_backend_suppress_present(int on) { (void)on; }
int   rsx_null_backend_splash(int step, int steps, unsigned done, unsigned total) { (void)step; (void)steps; (void)done; (void)total; return 0; }
int   rsx_null_backend_splash_skip(void) { return 0; }
void  rsx_null_backend_splash_gdi(int on) { (void)on; }

u32 rsx_null_backend_debug_color(void) { return s_soft.clear_argb; }

u32 rsx_null_backend_readback_center(void)
{
    /* 0 means "nothing presented yet", so give a presented black pixel a
     * non-zero alpha the way a real drawable readback would. */
    return s_soft.presented ? (0xFF000000u | s_soft.last_present) : 0u;
}

u32 rsx_null_backend_guest_draws(void) { return 0; }

#endif /* _WIN32 */
