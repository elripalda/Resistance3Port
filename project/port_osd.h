// Port UI drawn into the game's swapchain image (libs/video/rpcs3_bridge/r2rsx_osd.cpp) instead of
// topmost layered windows, which exclusive fullscreen does not show. The UI code paints with GDI
// as before, into a PosdCanvas (a 32-bit top-down DIB), and commits it to one OSD slot placed in
// the game window's client coordinates. PORT_OSD=layered keeps the old windows.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <string.h>

extern "C" {
int  r2rsx_osd_wanted(void);
int  r2rsx_osd_ready(void);
void r2rsx_osd_layer(int slot, const void* bgra, int w, int h, int x, int y, int ref_w, int ref_h, float alpha);
void r2rsx_osd_hide(int slot);
int  r2rsx_osd_visible(int slot);
void r2rsx_osd_suppress(int on);
unsigned r2rsx_osd_presents(void);
void r2rsx_osd_refresh(void);
}

// One slot per independent panel (the bridge has 16; the last one is its own).
enum {
    POSD_PERF = 0,     // F3 performance overlay (pcfeat)
    POSD_STATUS = 1,   // start-up "Compiling shaders" panel (pcfeat)
    POSD_TOAST = 2,    // trophy / notice toasts (pcfeat)
    POSD_HINT = 3,     // time-critical hints, e.g. the cutscene skip (pcfeat r2pf_hint)
    POSD_XMENU = 4,    // More PC Options, photo-mode note, controller pause (pcextra)
    POSD_XHAIR = 5,    // crosshair (pcextra)
    POSD_PCMENU = 6,   // PC menu, quit prompt, screenshot toast, F10 hint (pcport)
};

// Swapchain OSD in use right now (PORT_OSD not "layered", the RPCS3 renderer, a first present done).
inline int posd_on(void) { return r2rsx_osd_ready(); }

inline bool posd_client_size(HWND g, int* w, int* h)
{
    RECT rc;
    if (!g || !GetClientRect(g, &rc) || rc.right - rc.left <= 0 || rc.bottom - rc.top <= 0) return false;
    *w = rc.right - rc.left; *h = rc.bottom - rc.top;
    return true;
}

struct PosdCanvas {
    HDC dc = NULL;
    HBITMAP bm = NULL;
    HGDIOBJ old = NULL;
    uint32_t* px = NULL;
    int w = 0, h = 0;

    PosdCanvas() = default;
    PosdCanvas(const PosdCanvas&) = delete;
    PosdCanvas& operator=(const PosdCanvas&) = delete;
    ~PosdCanvas() { release(); }

    bool begin(int w_, int h_)
    {
        release();
        if (w_ <= 0 || h_ <= 0) return false;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w_; bi.bmiHeader.biHeight = -h_;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = NULL;
        dc = CreateCompatibleDC(NULL);
        bm = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
        if (!bm || !bits) { release(); return false; }
        old = SelectObject(dc, bm);
        px = (uint32_t*)bits; w = w_; h = h_;
        memset(px, 0, (size_t)w * h * 4);
        return true;
    }

    // GDI leaves the alpha byte alone: the panel is opaque (its fade is `alpha`), and pixels of
    // the colour key, if any, are transparent.
    void commit(int slot, int x, int y, int ref_w, int ref_h, float alpha, COLORREF key = CLR_INVALID)
    {
        if (!px) return;
        GdiFlush();
        const uint32_t k = key == CLR_INVALID ? 0xFFFFFFFFu
                         : ((uint32_t)GetRValue(key) << 16) | ((uint32_t)GetGValue(key) << 8) | GetBValue(key);
        const size_t n = (size_t)w * h;
        for (size_t i = 0; i < n; i++) {
            const uint32_t c = px[i] & 0x00FFFFFFu;
            px[i] = c == k ? 0u : c | 0xFF000000u;
        }
        r2rsx_osd_layer(slot, px, w, h, x, y, ref_w, ref_h, alpha);
    }

    void release()
    {
        if (dc && old) SelectObject(dc, old);
        if (bm) DeleteObject(bm);
        if (dc) DeleteDC(dc);
        dc = NULL; bm = NULL; old = NULL; px = NULL; w = h = 0;
    }
};

// Move / fade a committed slot without new pixels.
inline void posd_move(int slot, int x, int y, int ref_w, int ref_h, float alpha)
{
    r2rsx_osd_layer(slot, NULL, 0, 0, x, y, ref_w, ref_h, alpha);
}

// Screenshots: hide every OSD layer and wait (bounded) until a frame without them was presented.
inline void posd_suppress_for_capture(void)
{
    if (!r2rsx_osd_visible(-1)) { r2rsx_osd_suppress(1); return; }
    r2rsx_osd_suppress(1);
    const unsigned p0 = r2rsx_osd_presents();
    const ULONGLONG t0 = GetTickCount64();
    while (r2rsx_osd_presents() - p0 < 2 && GetTickCount64() - t0 < 250) { r2rsx_osd_refresh(); Sleep(4); }
}
