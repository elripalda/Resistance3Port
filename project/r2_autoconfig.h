// r2_autoconfig.h -- hardware detection and the recommended graphics settings for the RPCS3 renderer.
//
// Shared by Launcher.exe (project/launcher.cpp: first-run defaults, "Auto-detect (recommended)") and the
// game (R2_PRESET=auto|low|medium|high|ultra, r2rsx_bridge.cpp apply_launcher_overrides), so both pick
// the same settings from the same rules. Header-only and Windows-only; dxgi.dll is loaded at run time.
//
//   Hardware hw; detect(hw);                    // GPU (the one the renderer uses), CPU, RAM, screen
//   Settings s = recommend(hw);                 // or recommend(hw, parse_preset("high"))
//   apply_preset_env("auto", Phase::renderer);  // the game: export the settings as unset env vars
//
// Values are the ones settings.bat uses (RSX_RES_SCALE is relative to the PS3's 1280 x 720).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

namespace r2ac
{
enum Tier { T_AUTO = -1, T_LOW = 0, T_MEDIUM, T_HIGH, T_ULTRA, T_COUNT };
inline const char* tier_name(int t)
{
    static const char* n[T_COUNT] = { "Low", "Medium", "High", "Ultra" };
    return t >= 0 && t < T_COUNT ? n[t] : "Auto";
}
// "auto" (or empty / unknown) -> T_AUTO
inline int parse_preset(const char* p)
{
    if (!p || !*p) return T_AUTO;
    if (!_stricmp(p, "low") || !_stricmp(p, "min") || !_stricmp(p, "minimum")) return T_LOW;
    if (!_stricmp(p, "medium") || !_stricmp(p, "mid")) return T_MEDIUM;
    if (!_stricmp(p, "high")) return T_HIGH;
    if (!_stricmp(p, "ultra") || !_stricmp(p, "max")) return T_ULTRA;
    return T_AUTO;
}

struct Hardware
{
    // raw (detect() fills these; tests may fill them by hand and call classify())
    char gpu[128] = "unknown GPU";
    unsigned vendor = 0;          // PCI vendor id: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
    double vram_gb = 0;           // dedicated video memory
    int cores = 0, threads = 0;   // physical cores / logical CPUs
    int pcores = 0, ecores = 0;   // hybrid CPUs (Intel 12th gen+): performance / efficiency cores
    double ram_gb = 0;
    int screen_w = 0, screen_h = 0, refresh = 60;   // primary display, physical pixels
    // derived (classify())
    bool integrated = false;
    bool rtx = false;             // GeForce / Quadro RTX: DLSS
    bool strong = false;          // RTX 4070 class or better (or the AMD equivalent)
    int gpu_class = 0;            // 0 very weak / iGPU, 1 weak (2-4 GB), 2 mid, 3 high, 4 strong
    double eff_cores = 0;         // cores the game can use well (hybrid: P + E/2), as cpu_profile.inc
    bool wine = false;            // running under Wine / Proton
    bool deck = false;            // Steam Deck (Proton)
};

struct Settings
{
    int tier = T_MEDIUM;
    const char* fps = "60";       // R2_FPS
    const char* vsync = "auto";   // RSX_VSYNC
    const char* res = "1.5";      // RSX_RES_SCALE
    const char* fsr = "1";        // RSX_FSR (RPCS3 renderer): 0 off, 1 FSR 3.1, 2 DLSS
    const char* upscale = "1";    // RSX_UPSCALE (with RSX_FSR > 0)
    const char* aniso = "16";     // RSX_ANISO
    const char* filter = "linear";// RSX_FILTER: linear | point | fsr (FSR 1)
    const char* sharpen = "0.25"; // RSX_SHARPEN
    const char* cpu = "auto";     // PORT_CPU_PROFILE
    const char* shader = "async"; // R2_SHADER_MODE
    const char* window = "1920x1080"; // RSX_WINDOW (windowed mode)
};

// ---- helpers ---------------------------------------------------------------
inline bool has(const char* s, const char* sub)
{
    for (; *s; s++) {
        const char *a = s, *b = sub;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}
// The model number after a marker ("RTX 4070 Ti" -> 4070; "RX 9070 XT" -> 9070), 0 if none.
inline int model_after(const char* s, const char* marker)
{
    const size_t ml = strlen(marker);
    for (; *s; s++) {
        if (_strnicmp(s, marker, ml)) continue;
        const char* p = s + ml;
        while (*p == ' ') p++;
        if (isdigit((unsigned char)*p)) return atoi(p);
    }
    return 0;
}

// Integrated GPUs: DXGI has no flag for it, so name and memory (iGPUs report a small carve-out).
inline bool is_integrated(unsigned vendor, const char* name, double vram_gb)
{
    if (vendor == 0x10DE) return false;
    if (vram_gb < 0.5) return true;
    if (vendor == 0x8086)   // Arc A/B desktop and laptop cards are discrete; "Arc(TM) Graphics" (Meteor Lake) is not
        return !(model_after(name, "Arc(TM) A") || model_after(name, "Arc(TM) B") || model_after(name, "Arc A") ||
                 model_after(name, "Arc B") || has(name, "Iris Xe MAX"));
    if (vendor == 0x1002)   // "AMD Radeon(TM) Graphics", "Radeon 780M Graphics", "Vega 8 Graphics"
        return has(name, "Graphics") && !has(name, "RX") && !has(name, "Pro ");
    return vendor != 0x1414 ? vram_gb < 1.0 : true;
}

// Fills the derived fields from the raw ones.
inline void classify(Hardware& h)
{
    const char* n = h.gpu;
    const double gb = h.vram_gb;
    h.integrated = is_integrated(h.vendor, n, gb);
    h.rtx = h.vendor == 0x10DE && has(n, "RTX");
    if (h.cores <= 0) h.cores = h.threads > 0 ? h.threads : 4;
    if (h.threads < h.cores) h.threads = h.cores;
    h.eff_cores = (h.pcores && h.ecores) ? h.pcores + h.ecores / 2.0 : (double)h.cores;

    int cls;
    if (h.integrated) {
        // recent iGPUs are about a GTX 1050-1650: AMD 680M/780M/880M/890M/8060S, Intel Arc (Meteor/Lunar Lake)
        const bool fast_igpu = has(n, "680M") || has(n, "760M") || has(n, "780M") || has(n, "880M") ||
                               has(n, "890M") || has(n, "8050S") || has(n, "8060S") || has(n, "Arc");
        cls = fast_igpu ? 1 : 0;
    } else if (gb < 1.5) cls = 0;
    else if (gb < 3.5) cls = 1;
    else if (gb < 7.0) cls = 2;
    else cls = 3;

    if (!h.integrated) {
        // name-based caps for old cards with a lot of memory, and the strong class
        if (h.vendor == 0x10DE) {
            const int m = h.rtx ? model_after(n, "RTX") : model_after(n, "GTX");
            const int gen = m / 100, tier = m % 100;     // 4070 -> 40, 70; 1060 -> 10, 60; 970 -> 9, 70
            if (!h.rtx && m) {
                if (gen < 10) cls = cls < 1 ? cls : 1;                  // GTX 9xx / 7xx
                else if (!(gen == 10 && tier >= 80)) cls = cls < 2 ? cls : 2;   // GTX 1050-1070, 16xx
            }
            if (h.rtx && m && tier <= 50) cls = cls < 2 ? cls : 2;      // RTX 2050 / 3050
            h.strong = h.rtx && gb >= 10.0 &&
                       (m ? ((gen >= 40 && tier >= 70) || (gen == 30 && tier >= 80)) : gb >= 16.0);  // RTX A5000 etc.
        } else if (h.vendor == 0x1002) {
            const int m = model_after(n, "RX");
            if (m >= 400 && m < 600) cls = cls < 2 ? cls : 2;           // Polaris RX 4xx / 5xx
            const int gen = m / 1000, tier = m % 1000;
            h.strong = gb >= 12.0 && (((gen == 6 || gen == 7) && tier >= 800) || (gen >= 9 && tier >= 70));
        } else if (h.vendor == 0x8086) {
            if (model_after(n, "Arc(TM) A") && model_after(n, "Arc(TM) A") < 700) cls = cls < 2 ? cls : 2;
        } else {
            h.strong = gb >= 16.0;
        }
        if (h.strong) cls = 4;
    }
    h.gpu_class = cls;
}

inline void detect_gpu(Hardware& h)
{
    HMODULE dx = LoadLibraryA("dxgi.dll");
    if (!dx) return;
    typedef HRESULT (WINAPI *CF)(REFIID, void**);
    CF cf = (CF)(void*)GetProcAddress(dx, "CreateDXGIFactory1");
    IDXGIFactory1* f = nullptr;
    if (!cf || FAILED(cf(__uuidof(IDXGIFactory1), (void**)&f)) || !f) { FreeLibrary(dx); return; }
    IDXGIFactory6* f6 = nullptr;
    if (FAILED(f->QueryInterface(__uuidof(IDXGIFactory6), (void**)&f6))) f6 = nullptr;

    // High-performance order first (on laptops: the discrete GPU before the iGPU). The renderer takes the
    // first discrete Vulkan device (VKGSRender), so: the first discrete adapter, else the first hardware one.
    bool have = false, have_discrete = false;
    for (UINT i = 0;; i++) {
        IDXGIAdapter1* a = nullptr;
        HRESULT hr = f6 ? f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), (void**)&a)
                        : f->EnumAdapters1(i, &a);
        if (hr == DXGI_ERROR_NOT_FOUND || FAILED(hr) || !a) break;
        DXGI_ADAPTER_DESC1 d;
        const bool ok = SUCCEEDED(a->GetDesc1(&d));
        a->Release();
        if (!ok || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == 0x1414) continue;
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, NULL, NULL);
        const double gb = d.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0);
        const bool discrete = !is_integrated(d.VendorId, name, gb);
        // without IDXGIFactory6 the order is arbitrary: then the discrete adapter with the most memory
        const bool take = !have || (discrete && !have_discrete) || (discrete && !f6 && gb > h.vram_gb);
        if (!take) continue;
        strncpy_s(h.gpu, name, _TRUNCATE);
        h.vendor = d.VendorId; h.vram_gb = gb;
        have = true;
        have_discrete = have_discrete || discrete;
    }
    if (f6) f6->Release();
    f->Release();
    FreeLibrary(dx);
}

inline void detect_cpu(Hardware& h)
{
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    if (len) {
        BYTE* buf = (BYTE*)malloc(len);
        if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
            int cls_n[256] = {}; BYTE maxe = 0, mine = 255;
            for (DWORD off = 0; off < len;) {
                auto* e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
                if (e->Relationship == RelationProcessorCore) {
                    h.cores++;
                    for (WORD g = 0; g < e->Processor.GroupCount; g++) {
                        KAFFINITY m = e->Processor.GroupMask[g].Mask;
                        while (m) { h.threads += (int)(m & 1); m >>= 1; }
                    }
                    const BYTE ec = e->Processor.EfficiencyClass;
                    cls_n[ec]++;
                    if (ec > maxe) maxe = ec;
                    if (ec < mine) mine = ec;
                }
                off += e->Size;
            }
            if (maxe != mine) { h.pcores = cls_n[maxe]; h.ecores = h.cores - h.pcores; }
        }
        free(buf);
    }
    if (!h.cores) { SYSTEM_INFO si; GetSystemInfo(&si); h.cores = h.threads = (int)si.dwNumberOfProcessors; }
    MEMORYSTATUSEX ms = { sizeof ms };
    if (GlobalMemoryStatusEx(&ms)) h.ram_gb = ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
}

inline void detect_screen(Hardware& h)
{
    DEVMODEA dm = {}; dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        h.screen_w = (int)dm.dmPelsWidth; h.screen_h = (int)dm.dmPelsHeight;
        if (dm.dmDisplayFrequency > 1) h.refresh = (int)dm.dmDisplayFrequency;
    }
    if (h.screen_w <= 0 || h.screen_h <= 0) { h.screen_w = GetSystemMetrics(SM_CXSCREEN); h.screen_h = GetSystemMetrics(SM_CYSCREEN); }
}

// Wine / Proton (Linux, Steam Deck): ntdll exports wine_get_version. Steam sets SteamDeck=1 on the Deck;
// its APU also reports as "AMD Custom GPU 0405" (LCD) / "0932" (OLED) or "VanGogh".
inline void detect_platform(Hardware& h)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    h.wine = nt && GetProcAddress(nt, "wine_get_version") != NULL;
    char v[8] = "";
    h.deck = (GetEnvironmentVariableA("SteamDeck", v, sizeof v) && v[0] == '1') ||
             has(h.gpu, "Custom GPU 0405") || has(h.gpu, "Custom GPU 0932") || has(h.gpu, "VanGogh");
    if (h.deck) h.integrated = true;
}

inline void detect(Hardware& h)
{
    detect_gpu(h);
    detect_cpu(h);
    detect_screen(h);
    classify(h);
    detect_platform(h);
}

// RSX_RES_SCALE for an output height (the scale's 720p steps that the launcher offers).
inline const char* res_for_height(int sh)
{
    return sh >= 2160 ? "3" : sh >= 1800 ? "2.5" : sh >= 1440 ? "2" : sh >= 1080 ? "1.5" : "1";
}
inline int height_of_res(const char* r) { return (int)(atof(r) * 720 + 0.5); }

// The mapping. tier = T_AUTO: from the hardware; otherwise that preset on this hardware (DLSS still
// only on RTX; the screen and CPU still decide resolution and frame rate).
inline Settings recommend(const Hardware& h, int tier = T_AUTO)
{
    Settings s;
    int cls = h.gpu_class;
    if (tier != T_AUTO) cls = tier == T_LOW ? 0 : tier == T_MEDIUM ? 2 : tier == T_HIGH ? 3 : 4;
    const int sh = h.screen_h > 0 ? h.screen_h : 1080;
    const bool weak_cpu = h.eff_cores <= 4.0;
    const bool very_weak_cpu = weak_cpu && h.threads <= 4;
    const bool strong_cpu = h.eff_cores >= 8.0;

    // tier shown to the user (and applied by the per-tier options): GPU class, capped by a 4-core CPU
    s.tier = tier != T_AUTO ? tier : cls <= 1 ? T_LOW : cls == 2 ? T_MEDIUM : cls == 3 ? T_HIGH : T_ULTRA;
    if (tier == T_AUTO && weak_cpu && s.tier > T_MEDIUM) s.tier = T_MEDIUM;

    s.cpu = h.eff_cores <= 4.0 ? "low" : h.eff_cores < 8.0 ? "mid" : "high";
    s.shader = "async";       // async recompiler: no compile stalls; the interpreter mode is untested
    s.vsync = "auto";         // FIFO when the target matches the display, tear-free MAILBOX otherwise
    const char* aa = h.rtx && !h.wine ? "2" : "1";   // DLSS under Proton needs extra setup: FSR there
    const char* screen = res_for_height(sh);

    switch (cls) {
    case 0:   // iGPU / < 1.5 GB: PS3 resolution, no temporal AA; FSR 1 scales it to the screen
        s.res = "1"; s.fsr = "0"; s.upscale = "1"; s.aniso = "0";
        s.filter = sh > 720 ? "fsr" : "linear"; s.sharpen = sh > 720 ? "0.5" : "0.25";
        break;
    case 1:   // GTX 1050 / RX 560 / 780M: 1080p at most, AA upscaling from ~720p
        s.res = sh >= 1080 ? "1.5" : "1"; s.fsr = aa; s.upscale = sh >= 1080 ? "0.667" : "1"; s.aniso = "4";
        break;
    case 2:   // GTX 1060-1660 / RX 580 / RTX 3050: 1440p at most
        s.res = sh >= 1440 ? "2" : screen; s.fsr = aa; s.upscale = sh >= 1440 ? "0.667" : "1"; s.aniso = "8";
        break;
    case 3:   // RTX 2070-4060 / RX 6600-7700: the screen's resolution, upscaled above 1080p
        s.res = screen; s.fsr = aa; s.aniso = "16";
        s.upscale = sh >= 1800 ? "0.5" : sh >= 1440 ? "0.667" : "1";
        break;
    default:  // strong: 4K screen -> 4K DLSS Performance; 1440p -> DLAA (16 GB+) or Quality; 1080p -> DLAA
        s.res = screen; s.fsr = aa; s.aniso = "16";
        s.upscale = sh >= 1800 ? (h.rtx ? "0.5" : "0.58") : sh >= 1440 ? (h.vram_gb >= 15.5 ? "1" : "0.667") : "1";
        break;
    }
    if (cls > 0 && height_of_res(s.res) < sh) {   // rendering below the screen: FSR 1 scales it to fit
        s.filter = "fsr"; s.sharpen = "0.25";
    }

    // frame rate: 60 doubles the CPU work; 120 only on high-refresh screens with 8+ cores and a fast GPU
    if (cls == 0 || very_weak_cpu || (cls == 1 && weak_cpu)) s.fps = "30";
    else if (cls >= 3 && strong_cpu && h.refresh >= 119) s.fps = "120";
    else s.fps = "60";

    static const char* const wins[] = { "3840x2160", "2560x1440", "1920x1080", "1600x900", "1280x720" };
    s.window = "1280x720";
    for (const char* w : wins)
        if (atoi(w) < h.screen_w && atoi(strchr(w, 'x') + 1) < sh) { s.window = w; break; }
    if (h.deck && tier == T_AUTO) {   // Steam Deck: PS3 resolution, FSR 1 to the 1280x800 screen, 30 fps
        s.tier = T_LOW; s.res = "1"; s.fsr = "0"; s.upscale = "1"; s.aniso = "0";
        s.filter = "fsr"; s.sharpen = "0.25"; s.fps = "30"; s.window = "1280x800";
    }
    return s;
}

// ---- text ------------------------------------------------------------------
inline void describe_hw(const Hardware& h, char* out, size_t n)
{
    char cpu[64];
    if (h.pcores && h.ecores) snprintf(cpu, sizeof cpu, "%dP+%dE cores / %d threads", h.pcores, h.ecores, h.threads);
    else snprintf(cpu, sizeof cpu, "%d cores / %d threads", h.cores, h.threads);
    char gpu[48];
    if (h.integrated) snprintf(gpu, sizeof gpu, "integrated");
    else snprintf(gpu, sizeof gpu, "%.0f GB%s", h.vram_gb, h.rtx ? ", RTX" : "");
    snprintf(out, n, "%s (%s), %s, %.0f GB RAM, %dx%d @ %d Hz%s", h.gpu, gpu, cpu, h.ram_gb, h.screen_w, h.screen_h, h.refresh,
             h.deck ? ", Steam Deck" : h.wine ? ", Wine / Proton" : "");
}
inline const char* aa_text(const Settings& s)
{
    const bool native = atof(s.upscale) >= 0.99;
    if (!strcmp(s.fsr, "2")) return native ? "DLAA" : atof(s.upscale) > 0.6 ? "DLSS Quality" : atof(s.upscale) > 0.55 ? "DLSS Balanced" : "DLSS Performance";
    if (!strcmp(s.fsr, "1")) return native ? "FSR 3.1 Native AA" : atof(s.upscale) > 0.6 ? "FSR 3.1 Quality" : atof(s.upscale) > 0.55 ? "FSR 3.1 Balanced" : "FSR 3.1 Performance";
    return !strcmp(s.filter, "fsr") ? "no AA, FSR 1 scaling" : "no AA";
}
inline void describe(const Settings& s, char* out, size_t n)
{
    const int h = height_of_res(s.res);
    char af[24];
    if (!strcmp(s.aniso, "0")) snprintf(af, sizeof af, "trilinear");
    else snprintf(af, sizeof af, "%sx anisotropic", s.aniso);
    snprintf(out, n, "%s: %dp, %s, %s fps, %s, CPU profile %s", tier_name(s.tier), h, aa_text(s), s.fps, af, s.cpu);
}

// ---- the game: R2_PRESET ---------------------------------------------------
// Exports the preset's settings as environment variables, only the ones that are not set already
// (explicit settings always win). Phase::renderer: what the RPCS3 renderer reads when it starts
// (RSX_RES_SCALE, RSX_FILTER, RSX_SHARPEN, RSX_ANISO, RSX_FSR, RSX_UPSCALE). Phase::startup also covers
// what the game reads before the renderer (R2_FPS, RSX_VSYNC, PORT_CPU_PROFILE, R2_SHADER_MODE) and must
// run first thing in main(). Returns the number of variables set; summary (optional) describes it.
enum class Phase { renderer, startup };
inline int apply_preset_env(const char* preset, Phase phase, char* summary = nullptr, size_t n = 0)
{
    Hardware h; detect(h);
    const int t = parse_preset(preset);
    const Settings s = recommend(h, t);
    const char* vars[][2] = {
        { "RSX_RES_SCALE", s.res }, { "RSX_FILTER", s.filter }, { "RSX_SHARPEN", s.sharpen }, { "RSX_ANISO", s.aniso },
        { "RSX_FSR", s.fsr }, { "RSX_UPSCALE", !strcmp(s.fsr, "0") ? "1" : s.upscale },
        { "R2_FPS", s.fps }, { "RSX_VSYNC", s.vsync }, { "PORT_CPU_PROFILE", s.cpu }, { "R2_SHADER_MODE", s.shader },
    };
    const int count = phase == Phase::startup ? 10 : 6;
    int set = 0;
    for (int i = 0; i < count; i++) {
        const char* cur = getenv(vars[i][0]);
        if (cur && *cur) continue;
        if (_putenv_s(vars[i][0], vars[i][1]) == 0) set++;
    }
    if (summary && n) {
        char hw[256], st[192];
        describe_hw(h, hw, sizeof hw);
        describe(s, st, sizeof st);
        snprintf(summary, n, "R2_PRESET=%s -> %s | %s | %d unset variable(s) filled", preset, st, hw, set);
    }
    return set;
}
} // namespace r2ac
