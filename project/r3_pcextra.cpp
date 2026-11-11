/* Resistance 3 PC port: PC extras (r3_pcport.cpp's "More PC Options" page and
 * everything behind it). Runs on the game window's thread from r3_pcport.cpp
 * ui_tick (r2x_ui_tick) plus the frame limiter (r2x_frame_tick, stubs.cpp).
 * Same code as Resistance 2's project/r2_pcextra.cpp, with Resistance 3's
 * movies, levels and benchmark route.
 *
 *   Extras menu      F10 -> More PC Options (or F9 directly): colour-blind filter,
 *                    crosshair, HDR / Reflex (next start), cutscene skip, intro
 *                    skip, Discord, key bindings, controller layout. Changes go to
 *                    settings.ini + settings.bat (launcher).
 *   Key bindings     controls.txt rows (KEY = ACTION # Label); Enter = press a new
 *                    key, Esc cancels; cellPad re-reads the file at once.
 *   Controller       PAD_MAP_<button>=<button>, PAD_SOUTHPAW=1 (cellPad XInput hook).
 *   Post effects     R3_FX_FILMGRAIN / R3_FX_DOF / R3_FX_MOTIONBLUR (default 1 = game's
 *                    own; 0 = off, live in the menu): hooks in tools/hooks/r3.json zero
 *                    the strength the post pass hands each effect (see fx_init below).
 *   Cutscene skip    R3_CUTSCENE_SKIP (default on): hold Space / Enter / A / Menu
 *                    for 1 s while a pre-rendered movie (.bik) plays.
 *   Photo mode       R3_PHOTO (default on; 0 = off), F7: pause, hide the port's
 *                    overlays, F12 = shot.
 *   Crosshair        R3_CROSSHAIR=dot|cross: a small marker at the centre while on foot.
 *   Pause on pad loss R3_PAD_DISCONNECT_PAUSE (default on).
 *   Discord          R3_DISCORD=1 (+ R3_DISCORD_APPID or discord_appid.txt): Rich Presence over the
 *                    local IPC pipe (\\.\pipe\discord-ipc-N); silent if not running.
 *   Benchmark        R3_BENCH=1: scripted boot into the first level, measure
 *                    R3_BENCH_SECONDS, write logs\bench\R3_bench_<date>.txt, quit.
 *
 * The menu, the photo-mode note, the controller-lost dialog and the crosshair are
 * drawn into the game's swapchain image (port_osd.h), so they show in exclusive
 * fullscreen too; layered windows only with PORT_OSD=layered or before the renderer's
 * first present. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <vector>
#include <string>
#include <algorithm>
#include <mutex>
#include "port_osd.h"
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#ifndef PCX_PREFIX
#define PCX_PREFIX "R3"
#define PCX_GAME_NAME "Resistance 3"
#define PCX_TAG "r3x"
#endif

extern "C" {
extern volatile int g_port_ui_modal;                         /* cellPad.c */
extern volatile int g_port_input_kbm;                        /* cellPad.c: port 0 is the keyboard */
extern volatile unsigned long long g_pad_inj_until;          /* cellPad.c: injected presses on port 0 */
extern volatile uint32_t g_pad_inj_btn, g_pad_inj_sticks;
void cellpad_reload_controls(void);                          /* cellPad.c */
extern void (*volatile g_port_xi_hook)(int port, void* gamepad);
extern void (*volatile g_port_file_open_cb)(int slot, const char* path);   /* sys_fs.c */
extern void (*volatile g_port_file_close_cb)(int slot);
extern volatile unsigned long long g_r2_look_ms;             /* stubs.cpp: on-foot look code last ran */
extern uint8_t* vm_base;
void r2pf_toast(const char* title, const char* text, char badge);
void r2pf_hint(const char* title, const char* text, unsigned ms);
int r2pf_overlay_get(void);
void r2pf_overlay_set(int on);
void r2rsx_prof_enable(int on);
int r2rsx_prof_sample(double* frames, double* draws_per_frame, double* us_per_draw);
/* RPCS3 side (VKR2PcExtras.cpp): live post-process parameters */
extern volatile int g_pc_cvd_mode;                           /* 0 off, 1 protan, 2 deutan, 3 tritan */
extern volatile int g_pc_hdr_active;                         /* the swapchain is HDR */
extern volatile int g_pc_reflex_active;
void r2pc_reflex_sleep(void);
}

/* ---- small helpers (same file formats as r3_pcport.cpp / the launcher) ---- */
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
static bool write_lines(const char* path, const std::vector<std::string>& out)
{
    char tmp[MAX_PATH]; snprintf(tmp, sizeof tmp, "%s.pcx.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) return false;
    for (auto& l : out) fprintf(f, "%s\n", l.c_str());
    const bool ok = fclose(f) == 0;
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(tmp); return false; }
    return true;
}
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
    return write_lines(path, out);
}
static void ini_set(const char* key, const std::string& v) { file_set("settings.ini", std::string(key) + "=", std::string(key) + "=" + v, NULL); }
static void bat_set(const char* env, const std::string& v)
{
    file_set("settings.bat", std::string("set \"") + env + "=", v.empty() ? std::string() : std::string("set \"") + env + "=" + v + "\"", "@echo off");
}
static std::string envs(const char* name, const char* def)
{
    char full[96]; snprintf(full, sizeof full, "%s_%s", PCX_PREFIX, name);
    const char* e = getenv(full); return (e && *e) ? e : def;
}
static int envi(const char* name, int def) { std::string v = envs(name, ""); return v.empty() ? def : atoi(v.c_str()); }
/* the "Hold Space / A to skip" hint when a movie starts. PORT_SKIP_HINT=0 (or <prefix>_SKIP_HINT=0; launcher
   "Show cutscene skip hint") hides it; holding the button still skips. */
static int skip_hint_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("PORT_SKIP_HINT");
        const std::string v = (e && *e) ? std::string(e) : envs("SKIP_HINT", "");
        on = !(v == "0" || _stricmp(v.c_str(), "off") == 0 || _stricmp(v.c_str(), "false") == 0);
    }
    return on;
}
static double now_s(void)
{
    static LARGE_INTEGER f = {}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart;
}
static int host_focused(HWND game)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() && !IsIconic(game);
}
#define LOG(...) do { fprintf(stderr, "[" PCX_TAG "] " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* ======================================================================
 * Post effects. The engine's post pass (0x746A00 area) reads the per-view
 * post block (0x12B3E80 + view*0x230; filled every frame from post volumes
 * and the Lua pfx_* nodes) and hands each effect a parameter struct on its
 * stack. Each effect function skips itself when its strength is zero, so
 * the r3.json hooks zero the strength on entry (func_00744A18 film grain:
 * p+8; func_007424F8 camera motion blur: p+0x14; func_00855A98 object motion
 * blur: p+0xC; func_008557A8 depth of field: near/far max blur p+0x28/0x38).
 * The game then takes its own "effect off" path. Nothing persistent changes. */
static volatile int s_fx_grain = 1, s_fx_dof = 1, s_fx_mb = 1, s_fx_init = 0;
static void fx_init(void)
{
    if (s_fx_init) return;
    s_fx_grain = envi("FX_FILMGRAIN", 1) != 0;
    s_fx_dof = envi("FX_DOF", 1) != 0;
    s_fx_mb = envi("FX_MOTIONBLUR", 1) != 0;
    s_fx_init = 1;
    if (!s_fx_grain || !s_fx_dof || !s_fx_mb)
        LOG("post effects: film grain %s, depth of field %s, motion blur %s", s_fx_grain ? "on" : "off",
            s_fx_dof ? "on" : "off", s_fx_mb ? "on" : "off");
}
static void fx_zero(uint32_t ea) { if (ea && vm_base) memset(vm_base + ea, 0, 4); }   /* 0.0f, any endianness */
extern "C" void r3_fx_pass(int kind, uint32_t p)   /* r3.json hooks, game thread */
{
    if (!s_fx_init) fx_init();
    switch (kind) {
    case 0: if (!s_fx_grain) fx_zero(p + 0x8); break;
    case 1: if (!s_fx_mb) fx_zero(p + 0x14); break;
    case 2: if (!s_fx_mb) fx_zero(p + 0xC); break;
    case 3: if (!s_fx_dof) { fx_zero(p + 0x28); fx_zero(p + 0x38); } break;
    }
}
static void fx_apply(void) { if (!s_fx_init) fx_init(); }

/* ======================================================================
 * Controller layout (XInput hook in cellPad.c pad_poll_xinput) */
enum { XB_A, XB_B, XB_X, XB_Y, XB_LB, XB_RB, XB_LT, XB_RT, XB_LS, XB_RS, XB_BACK, XB_START, XB_COUNT };
static const char* s_xb_key[XB_COUNT] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "BACK", "START" };
static const wchar_t* s_xb_name[XB_COUNT] = { L"A", L"B", L"X", L"Y", L"LB", L"RB", L"LT", L"RT", L"Left stick click",
                                              L"Right stick click", L"View", L"Menu" };
static const WORD s_xb_bit[XB_COUNT] = { XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
    XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0, XINPUT_GAMEPAD_LEFT_THUMB,
    XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START };
static volatile int s_xmap[XB_COUNT];   /* physical -> acts as */
static volatile int s_southpaw = 0, s_xmap_ident = 1;
static void xmap_load(void)
{
    s_xmap_ident = 1;
    for (int i = 0; i < XB_COUNT; i++) {
        char k[32]; snprintf(k, sizeof k, "PAD_MAP_%s", s_xb_key[i]);
        const char* e = getenv(k); s_xmap[i] = i;
        if (e && *e) for (int j = 0; j < XB_COUNT; j++) if (!_stricmp(e, s_xb_key[j])) s_xmap[i] = j;
        if (s_xmap[i] != i) s_xmap_ident = 0;
    }
    const char* sp = getenv("PAD_SOUTHPAW"); s_southpaw = sp && *sp && *sp != '0';
}
static void xi_hook(int port, void* gp_)
{
    (void)port;
    XINPUT_GAMEPAD* gp = (XINPUT_GAMEPAD*)gp_;
    if (s_southpaw) {
        std::swap(gp->sThumbLX, gp->sThumbRX);
        std::swap(gp->sThumbLY, gp->sThumbRY);
    }
    if (s_xmap_ident) return;
    int down[XB_COUNT];
    for (int i = 0; i < XB_COUNT; i++) down[i] = s_xb_bit[i] ? (gp->wButtons & s_xb_bit[i]) != 0 : 0;
    down[XB_LT] = gp->bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    down[XB_RT] = gp->bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    const BYTE lt = gp->bLeftTrigger, rt = gp->bRightTrigger;
    WORD b = gp->wButtons;
    for (int i = 0; i < XB_COUNT; i++) b &= ~s_xb_bit[i];
    BYTE olt = 0, ort = 0;
    for (int i = 0; i < XB_COUNT; i++) {
        if (!down[i]) continue;
        const int t = s_xmap[i];
        const BYTE analog = i == XB_LT ? lt : i == XB_RT ? rt : 255;   /* trigger to trigger keeps its travel */
        if (t == XB_LT) olt = std::max(olt, analog);
        else if (t == XB_RT) ort = std::max(ort, analog);
        else b |= s_xb_bit[t];
    }
    gp->wButtons = b; gp->bLeftTrigger = olt; gp->bRightTrigger = ort;
}

/* ======================================================================
 * Movies (cutscene skip) and level names (Discord) from the file hooks */
static volatile int s_movie_slot = -1;
static volatile ULONGLONG s_movie_t0 = 0;
static std::mutex s_lvl_mu;
static std::string s_level;                /* "" = not known yet */
static volatile int s_level_gen = 0;
static void file_open_cb(int slot, const char* path)
{
    if (!path) return;
    std::string p(path);
    for (auto& c : p) { if (c == '\\') c = '/'; c = (char)tolower((unsigned char)c); }
    if (slot >= 0 && p.size() > 4 && p.compare(p.size() - 4, 4, ".bik") == 0) {
        /* not the boot movies (SKIP_INTRO) or the weapon previews in the menus */
        if (p.find("lby_insom") == std::string::npos && p.find("opn_loadingmovie") == std::string::npos &&
            p.find("mini_wpn") == std::string::npos) { s_movie_slot = slot; s_movie_t0 = GetTickCount64(); }
        return;
    }
    const size_t k = p.find("/levels/");   /* packed/levels/<name>/level_cached.psarc */
    if (k == std::string::npos || p.find("/level_") == std::string::npos) return;
    const size_t e = p.find('/', k + 8);
    if (e == std::string::npos) return;
    std::string lv = p.substr(k + 8, e - (k + 8));
    std::lock_guard<std::mutex> g(s_lvl_mu);
    if (lv != s_level) { s_level = lv; s_level_gen++; LOG("level: %s", lv.c_str()); }
}
static void file_close_cb(int slot) { if (slot == s_movie_slot) s_movie_slot = -1; }
static std::string level_pretty(void)
{
    std::string lv; { std::lock_guard<std::mutex> g(s_lvl_mu); lv = s_level; }
    if (lv.empty()) return "Starting up";
    if (lv == "lobby" || lv == "frontend" || lv == "mainmenu" || lv.compare(0, 3, "mp_") == 0) return "In the menus";
    for (auto& c : lv) if (c == '_') c = ' ';
    bool up = true;
    for (auto& c : lv) { if (up && c >= 'a' && c <= 'z') c -= 32; up = c == ' '; }
    return lv;
}

/* ======================================================================
 * Discord Rich Presence over the local IPC pipe */
static volatile int s_discord_quit = 0;
static bool pipe_send(HANDLE h, uint32_t op, const std::string& json)
{
    std::string buf(8 + json.size(), 0);
    const uint32_t len = (uint32_t)json.size();
    memcpy(&buf[0], &op, 4); memcpy(&buf[4], &len, 4); memcpy(&buf[8], json.data(), json.size());
    DWORD w = 0;
    return WriteFile(h, buf.data(), (DWORD)buf.size(), &w, NULL) && w == buf.size();
}
static void pipe_drain(HANDLE h)
{
    DWORD avail = 0; char tmp[4096];
    while (PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) && avail) { DWORD r = 0; if (!ReadFile(h, tmp, std::min<DWORD>(avail, sizeof tmp), &r, NULL) || !r) break; }
}
static std::string json_esc(const std::string& s)
{
    std::string o; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; } return o;
}
static DWORD WINAPI discord_thread(LPVOID)
{
    std::string appid = envs("DISCORD_APPID", "");
    if (appid.empty()) {                         /* discord_appid.txt in the game folder (the launcher can't hold text) */
        std::vector<std::string> l;
        if (read_lines("discord_appid.txt", l))
            for (auto& s : l) { std::string t = ltrim(s); while (!t.empty() && (unsigned char)t.back() <= ' ') t.pop_back();
                                if (!t.empty() && isdigit((unsigned char)t[0])) { appid = t; break; } }
    }
    if (appid.empty()) { LOG("Discord: no application id (set " PCX_PREFIX "_DISCORD_APPID or discord_appid.txt); Rich Presence off"); return 0; }
    const long long start = (long long)time(NULL);
    HANDLE h = INVALID_HANDLE_VALUE; int sent_gen = -1; ULONGLONG retry = 0, next_send = 0; int nonce = 0;
    while (!s_discord_quit) {
        const ULONGLONG now = GetTickCount64();
        if (h == INVALID_HANDLE_VALUE) {
            if (now < retry) { Sleep(500); continue; }
            retry = now + 60000;
            for (int i = 0; i < 10 && h == INVALID_HANDLE_VALUE; i++) {
                char name[64]; snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
                h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            }
            if (h == INVALID_HANDLE_VALUE) continue;   /* Discord not running: try again in a minute */
            if (!pipe_send(h, 0, "{\"v\":1,\"client_id\":\"" + json_esc(appid) + "\"}")) { CloseHandle(h); h = INVALID_HANDLE_VALUE; continue; }
            LOG("Discord: connected");
            sent_gen = -1;
        }
        pipe_drain(h);
        if (sent_gen != s_level_gen || now >= next_send) {
            sent_gen = s_level_gen; next_send = now + 15000 * 4;
            char js[1024];
            snprintf(js, sizeof js,
                "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{\"details\":\"%s\",\"state\":\"%s\","
                "\"timestamps\":{\"start\":%lld},\"assets\":{\"large_image\":\"logo\",\"large_text\":\"%s\"}}},\"nonce\":\"%d\"}",
                GetCurrentProcessId(), PCX_GAME_NAME, json_esc(level_pretty()).c_str(), start, PCX_GAME_NAME, ++nonce);
            if (!pipe_send(h, 1, js)) { CloseHandle(h); h = INVALID_HANDLE_VALUE; retry = now + 60000; continue; }
        }
        Sleep(1000);
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return 0;
}

/* ======================================================================
 * Benchmark: inputs on a timeline, then frame times for N seconds */
struct BenchStep { double at; uint32_t btn; };
static std::vector<BenchStep> s_bench_steps;
static int s_bench = 0, s_bench_step = 0, s_bench_state = 0;   /* 0 script, 1 measuring, 2 done */
static double s_bench_t0 = 0, s_bench_m0 = 0, s_bench_secs = 60;
static std::vector<float> s_bench_ft;
static double s_last_frame = 0;
static uint32_t btn_named(const std::string& n)
{
    static const struct { const char* n; uint32_t b; } t[] = { {"SELECT",1},{"L3",2},{"R3",4},{"START",8},{"UP",0x10},{"RIGHT",0x20},
        {"DOWN",0x40},{"LEFT",0x80},{"L2",0x100},{"R2",0x200},{"L1",0x400},{"R1",0x800},{"TRIANGLE",0x1000},{"CIRCLE",0x2000},
        {"CROSS",0x4000},{"SQUARE",0x8000} };
    for (auto& e : t) if (!_stricmp(n.c_str(), e.n)) return e.b;
    return 0;
}
/* "wait:BUTTON,wait:BUTTON,...,wait" -- seconds after the previous step; the
 * last plain number is the settle time before measuring. Default: the
 * scratch\r3bench.ps1 route (title, main menu, Campaign, continue / first
 * level), timed from process start. */
static void bench_parse(void)
{
    std::string sc = envs("BENCH_SCRIPT", "100:CROSS,8:CROSS,8:START,8:CROSS,8:CROSS,60:START,3:CROSS,30");
    double t = 0; size_t i = 0;
    while (i < sc.size()) {
        size_t e = sc.find(',', i); if (e == std::string::npos) e = sc.size();
        std::string tok = sc.substr(i, e - i); i = e + 1;
        const size_t c = tok.find(':');
        t += atof(tok.c_str());
        s_bench_steps.push_back({ t, c == std::string::npos ? 0u : btn_named(tok.substr(c + 1)) });
    }
}
static void bench_report(void)
{
    std::vector<float> v = s_bench_ft;
    const double secs = now_s() - s_bench_m0;
    double us_draw = 0, dpf = 0, fr = 0;
    r2rsx_prof_sample(&fr, &dpf, &us_draw);
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) { if (v.empty()) return 0.0; size_t k = (size_t)((v.size() - 1) * p); return (double)v[k]; };
    auto low = [&](double frac) {   /* average fps over the slowest `frac` of frames */
        if (v.empty()) return 0.0; size_t n = std::max<size_t>(1, (size_t)(v.size() * frac)); double s = 0;
        for (size_t k = v.size() - n; k < v.size(); k++) s += v[k]; return 1000.0 * n / s; };
    const double avg = v.empty() ? 0 : v.size() / secs;
    CreateDirectoryA("logs", NULL); CreateDirectoryA("logs\\bench", NULL);
    SYSTEMTIME t; GetLocalTime(&t);
    char path[MAX_PATH]; snprintf(path, sizeof path, "logs\\bench\\%s_bench_%04u%02u%02u_%02u%02u%02u.txt", PCX_PREFIX,
                                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s built-in benchmark\n", PCX_GAME_NAME);
        fprintf(f, "level            %s\n", level_pretty().c_str());
        fprintf(f, "seconds          %.1f\n", secs);
        fprintf(f, "frames           %zu\n", v.size());
        fprintf(f, "avg fps          %.2f\n", avg);
        fprintf(f, "1%% low fps       %.2f\n", low(0.01));
        fprintf(f, "0.1%% low fps     %.2f\n", low(0.001));
        fprintf(f, "median ms        %.3f\n", pct(0.5));
        fprintf(f, "p99 ms           %.3f\n", pct(0.99));
        fprintf(f, "max ms           %.3f\n", v.empty() ? 0.0 : (double)v.back());
        fprintf(f, "draws/frame      %.1f\n", dpf);
        fprintf(f, "us/draw          %.3f\n", us_draw);
        fprintf(f, "frame cap        %s\n", getenv("R2_FPS") ? getenv("R2_FPS") : "default");
        fclose(f);
    }
    const bool hdr = GetFileAttributesA("logs\\bench\\bench_history.csv") == INVALID_FILE_ATTRIBUTES;
    if (FILE* h = fopen("logs\\bench\\bench_history.csv", "a")) {
        if (hdr) fprintf(h, "date,game,level,seconds,frames,avg_fps,low1_fps,low01_fps,draws_per_frame,us_per_draw\n");
        fprintf(h, "%04u-%02u-%02u %02u:%02u:%02u,%s,%s,%.1f,%zu,%.2f,%.2f,%.2f,%.1f,%.3f\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                t.wMinute, t.wSecond, PCX_PREFIX, level_pretty().c_str(), secs, v.size(), avg, low(0.01), low(0.001), dpf, us_draw);
        fclose(h);
    }
    LOG("benchmark: avg %.2f fps, 1%% low %.2f, %.3f us/draw -> %s", avg, low(0.01), us_draw, path);
}
static void bench_tick(HWND game)
{
    if (!s_bench || s_bench_state == 2) return;
    const double t = now_s() - s_bench_t0;
    if (s_bench_state == 0) {
        while (s_bench_step < (int)s_bench_steps.size() && t >= s_bench_steps[s_bench_step].at) {
            const BenchStep& st = s_bench_steps[s_bench_step++];
            if (st.btn) { g_pad_inj_sticks = 0x80808080u; g_pad_inj_btn = st.btn; g_pad_inj_until = GetTickCount64() + 150; }
            LOG("benchmark: step %d at %.0f s", s_bench_step, t);
        }
        if (s_bench_step >= (int)s_bench_steps.size()) {
            s_bench_state = 1; s_bench_m0 = now_s(); s_bench_ft.clear(); s_bench_ft.reserve(60 * 400);
            r2rsx_prof_enable(1); r2rsx_prof_sample(NULL, NULL, NULL);
            LOG("benchmark: measuring for %.0f s", s_bench_secs);
        }
    } else if (now_s() - s_bench_m0 >= s_bench_secs) {
        s_bench_state = 2;
        bench_report();
        r2rsx_prof_enable(0);
        if (envi("BENCH_QUIT", 1) && game) PostMessageA(game, WM_CLOSE, 0, 0);
    }
}
/* stubs.cpp r2_frame_period: once per game frame */
extern "C" void r2x_frame_tick(void)
{
    const double t = now_s();
    if (s_bench_state == 1 && s_last_frame > 0) s_bench_ft.push_back((float)((t - s_last_frame) * 1000.0));
    s_last_frame = t;
    r2pc_reflex_sleep();                         /* NVIDIA Reflex: no-op unless R3_REFLEX is on and the driver has it */
    static int n = 0;
    if ((++n & 15) == 0) fx_apply();
}

/* ======================================================================
 * The extras menu */
struct XChoice { std::wstring label; std::string value; };
struct XOpt {
    const wchar_t* title; const wchar_t* help; const char* ini; const char* env; int restart;
    std::vector<XChoice> ch; int sel, run;
    void (*live)(const std::string& v);
};
static std::vector<XOpt> s_opts;
static int s_photo_on = 0;
static int s_crosshair = 0;             /* 0 off, 1 dot, 2 cross */
static int s_disc_pause_on = 1, s_cut_skip = 1, s_photo_allowed = 1;
static void live_fx_grain(const std::string& v) { fx_init(); s_fx_grain = v != "0"; }
static void live_fx_dof(const std::string& v) { fx_init(); s_fx_dof = v != "0"; }
static void live_fx_mb(const std::string& v) { fx_init(); s_fx_mb = v != "0"; }
static void live_cvd(const std::string& v) { g_pc_cvd_mode = atoi(v.c_str()); }
static int crosshair_parse(const std::string& v) { return v == "dot" || v == "1" ? 1 : v == "cross" || v == "2" ? 2 : 0; }
static void live_xhair(const std::string& v) { s_crosshair = crosshair_parse(v); }
static void live_dpause(const std::string& v) { s_disc_pause_on = v != "0"; }
static void live_cut(const std::string& v) { s_cut_skip = v != "0"; }
extern "C" void cellpad_set_pad_opt(const char* key, const char* v);   /* cellPad.c (SDL3 backend) */
static void live_gyro(const std::string& v) { cellpad_set_pad_opt("gyro", v.c_str()); }
static void live_gyrosens(const std::string& v) { cellpad_set_pad_opt("gyrosens", v.c_str()); }
static void live_gyroinvy(const std::string& v) { cellpad_set_pad_opt("gyroinvy", v.c_str()); }
static void live_gyrospace(const std::string& v) { cellpad_set_pad_opt("gyrospace", v.c_str()); }
static void live_lightbar(const std::string& v) { cellpad_set_pad_opt("lightbar", v.c_str()); }
static void live_nswap(const std::string& v) { cellpad_set_pad_opt("nswap", v.c_str()); }
static void opt_add(const wchar_t* title, const wchar_t* help, const char* ini, const char* env, int restart,
                    std::vector<XChoice> ch, const char* def, void (*live)(const std::string&))
{
    XOpt o{ title, help, ini, env, restart, std::move(ch), 0, 0, live };
    const char* e = getenv(env); std::string cur = (e && *e) ? e : def;
    o.run = 0;
    for (size_t k = 0; k < o.ch.size(); k++) if (!_stricmp(o.ch[k].value.c_str(), cur.c_str())) o.run = (int)k;
    o.sel = o.run;
    s_opts.push_back(std::move(o));
}
#define ENVN(x) PCX_PREFIX "_" x
static void opts_init(void)
{
    if (!s_opts.empty()) return;
    /* values match launcher_extra.inc exactly ("" = default: no settings.bat line), or the launcher drops them */
    const std::vector<XChoice> onoff = { {L"On", ""}, {L"Off", "0"} };
    opt_add(L"Film grain", L"The game's film grain noise (levels, multiplayer modes).", "fx_filmgrain", ENVN("FX_FILMGRAIN"), 0,
            onoff, "", live_fx_grain);
    opt_add(L"Depth of field", L"Background / foreground blur (cutscenes, aiming, some areas).", "fx_dof", ENVN("FX_DOF"), 0,
            onoff, "", live_fx_dof);
    opt_add(L"Motion blur", L"Camera motion blur and per-object motion blur.", "fx_motionblur", ENVN("FX_MOTIONBLUR"), 0,
            onoff, "", live_fx_mb);
    opt_add(L"Colour-blind filter", L"Daltonization filter on the final image.", "colorblind", ENVN("COLORBLIND"), 0,
            { {L"Off", ""}, {L"Protanopia (red)", "1"}, {L"Deuteranopia (green)", "2"}, {L"Tritanopia (blue)", "3"} }, "", live_cvd);
    opt_add(L"Crosshair", L"A small extra marker at the screen centre while on foot.", "crosshair", ENVN("CROSSHAIR"), 0,
            { {L"Off", ""}, {L"Dot", "dot"}, {L"Cross", "cross"} }, "", live_xhair);
    opt_add(L"Hold to skip movies", L"Hold Space / Enter / A / Menu for 1 s to skip a pre-rendered movie.", "cutscene_skip",
            ENVN("CUTSCENE_SKIP"), 0, onoff, "", live_cut);
    opt_add(L"Skip intro movies", L"Skips the start-up logo and loading movies.", "skipintro", "SKIP_INTRO", 1, onoff, "", NULL);
    opt_add(L"Pause on controller loss", L"Pauses when the controller in use disconnects.", "pad_disconnect_pause",
            ENVN("PAD_DISCONNECT_PAUSE"), 0, onoff, "", live_dpause);
    opt_add(L"HDR output", L"Experimental. Needs Windows HDR on (SDR otherwise). Auto: HDR10 only when Windows HDR is on.",
            "hdr", ENVN("HDR"), 1,
            { {L"Off", ""}, {L"Auto", "auto"}, {L"On (HDR10)", "1"}, {L"On (scRGB)", "scrgb"} }, "", NULL);
    opt_add(L"HDR paper white", L"Brightness of SDR white / the HUD in nits. Only highlights go above it.", "hdr_paper",
            ENVN("HDR_PAPER"), 1,
            { {L"203 nits", ""}, {L"100 nits", "100"}, {L"160 nits", "160"}, {L"250 nits", "250"}, {L"300 nits", "300"},
              {L"400 nits", "400"} }, "", NULL);
    opt_add(L"HDR peak", L"Brightest highlights. Display: the peak Windows reports. Same as paper white = no expansion.",
            "hdr_peak", ENVN("HDR_PEAK"), 1,
            { {L"Display", ""}, {L"400 nits", "400"}, {L"600 nits", "600"}, {L"800 nits", "800"}, {L"1000 nits", "1000"},
              {L"1500 nits", "1500"}, {L"2000 nits", "2000"} }, "", NULL);
    opt_add(L"HDR saturation", L"Colour saturation in HDR (1 = as the game).", "hdr_sat", ENVN("HDR_SAT"), 1,
            { {L"1.0", ""}, {L"0.9", "0.9"}, {L"1.1", "1.1"}, {L"1.2", "1.2"} }, "", NULL);
    opt_add(L"NVIDIA Reflex", L"Low-latency mode (VK_NV_low_latency2, GeForce). Experimental.", "reflex", ENVN("REFLEX"), 1,
            { {L"Off", ""}, {L"On", "1"}, {L"On + Boost", "2"} }, "", NULL);
    opt_add(L"Frame Generation", L"NVIDIA DLSS Frame Generation (RTX 40: 2x, RTX 50: up to 4x). Smoother when the CPU is the limit; "
            L"a little more input lag. Needs Anti-aliasing = NVIDIA DLSS. FSR 3 2x: AMD frame interpolation for other GPUs "
            L"(Anti-aliasing = FSR, DLSS or XeSS; no Reflex).", "dlss_fg", ENVN("DLSS_FG"), 1,
            { {L"Off", ""}, {L"2x", "2"}, {L"3x", "3"}, {L"4x", "4"}, {L"FSR 3 2x", "fsr3"} }, "", NULL);
    opt_add(L"Discord Rich Presence", L"Shows the game and level in Discord (needs discord_appid.txt or " ENVN("DISCORD_APPID") L").",
            "discord", ENVN("DISCORD"), 1, { {L"Off", ""}, {L"On", "1"} }, "", NULL);
    /* controllers (SDL3 backend, cellPad.c); keys and values match the launcher's Controls page */
    opt_add(L"Controller support", L"SDL3: DualShock / DualSense / Switch / Xbox directly (prompts, rumble, light bar, gyro). "
            L"XInput: the old path.", "padbackend", "PAD_BACKEND", 1, { {L"SDL3", ""}, {L"XInput", "xinput"} }, "", NULL);
    opt_add(L"Gyro aiming", L"Turn the view by turning a DualShock 4 / DualSense / Switch controller. While aiming: only with "
            L"the Aim button held.", "gyro", "PAD_GYRO", 0, { {L"Off", ""}, {L"On", "1"}, {L"While aiming", "aim"} }, "", live_gyro);
    opt_add(L"Gyro sensitivity", L"View degrees per controller degree.", "gyrosens", "PAD_GYRO_SENS", 0,
            { {L"Default (2x)", ""}, {L"0.5x", "0.5"}, {L"1x", "1"}, {L"1.5x", "1.5"}, {L"3x", "3"}, {L"4x", "4"}, {L"6x", "6"} },
            "", live_gyrosens);
    opt_add(L"Gyro vertical", L"Invert the up / down gyro.", "gyroinvy", "PAD_GYRO_INVERT_Y", 0,
            { {L"Normal", ""}, {L"Inverted", "1"} }, "", live_gyroinvy);
    opt_add(L"Gyro turning axis", L"Controller: the controller's own yaw axis. Player: follows gravity (flat or upright).",
            "gyrospace", "PAD_GYRO_SPACE", 0, { {L"Controller", ""}, {L"Player", "player"} }, "", live_gyrospace);
    opt_add(L"Light bar", L"DualShock 4 / DualSense light bar in the player's colour.", "lightbar", "PAD_LIGHTBAR", 0,
            { {L"Player colour", ""}, {L"Off", "0"} }, "", live_lightbar);
    opt_add(L"Nintendo button layout", L"Switch Pro / Joy-Con: Nintendo style confirms with A and shows Xbox-style letters.",
            "nswap", "PAD_NINTENDO_SWAP", 0, { {L"By position", ""}, {L"Nintendo style (A confirms)", "1"} }, "", live_nswap);
}
enum { PG_MAIN, PG_KEYS, PG_PAD };
static int s_open = 0, s_page = PG_MAIN, s_sel = 0, s_top = 0, s_capture = 0, s_release = 0;
static HWND s_ov = NULL, s_game = NULL;
static int s_prev_in = 0;

/* key binding rows: controls.txt lines that bind a key */
struct KeyRow { int line; std::string key, action, label; };
static std::vector<std::string> s_ctl_lines;
static std::vector<KeyRow> s_keys;
static void keys_load(void)
{
    s_ctl_lines.clear(); s_keys.clear();
    read_lines("controls.txt", s_ctl_lines);
    for (int i = 0; i < (int)s_ctl_lines.size(); i++) {
        std::string l = s_ctl_lines[i], com;
        const size_t h = l.find('#'); if (h != std::string::npos) { com = ltrim(l.substr(h + 1)); l = l.substr(0, h); }
        const size_t eq = l.find('='); if (eq == std::string::npos) continue;
        char k[64] = {}, v[64] = {};
        if (sscanf(l.substr(0, eq).c_str(), " %63s", k) != 1 || sscanf(l.substr(eq + 1).c_str(), " %63s", v) != 1) continue;
        if (starts_ci(k, "mouse_") || starts_ci(k, "menu_") || starts_ci(k, "pad_")) continue;
        s_keys.push_back({ i, k, v, com.empty() ? v : com });
    }
}
static const struct { const char* n; int vk; } s_vkn[] = {
    {"SPACE",VK_SPACE},{"LSHIFT",VK_LSHIFT},{"RSHIFT",VK_RSHIFT},{"LCTRL",VK_LCONTROL},{"RCTRL",VK_RCONTROL},{"ALT",VK_MENU},{"TAB",VK_TAB},
    {"ENTER",VK_RETURN},{"BACKSPACE",VK_BACK},{"UP",VK_UP},{"DOWN",VK_DOWN},{"LEFT",VK_LEFT},{"RIGHT",VK_RIGHT},
    {"MOUSE1",VK_LBUTTON},{"MOUSE2",VK_RBUTTON},{"MOUSE3",VK_MBUTTON},{"MOUSE4",VK_XBUTTON1},{"MOUSE5",VK_XBUTTON2},
    {"F1",VK_F1},{"F2",VK_F2},{"F4",VK_F4},{"F5",VK_F5},{"F6",VK_F6},{"F11",VK_F11},
    {"CAPSLOCK",VK_CAPITAL},{"INSERT",VK_INSERT},{"DELETE",VK_DELETE},{"HOME",VK_HOME},{"END",VK_END},{"PGUP",VK_PRIOR},{"PGDN",VK_NEXT},
    {"NUM0",VK_NUMPAD0},{"NUM1",VK_NUMPAD1},{"NUM2",VK_NUMPAD2},{"NUM3",VK_NUMPAD3},{"NUM4",VK_NUMPAD4},{"NUM5",VK_NUMPAD5},
    {"NUM6",VK_NUMPAD6},{"NUM7",VK_NUMPAD7},{"NUM8",VK_NUMPAD8},{"NUM9",VK_NUMPAD9},
    {"GRAVE",VK_OEM_3},{"MINUS",VK_OEM_MINUS},{"EQUALS",VK_OEM_PLUS},{"LBRACKET",VK_OEM_4},{"RBRACKET",VK_OEM_6},{"BACKSLASH",VK_OEM_5},
    {"SEMICOLON",VK_OEM_1},{"APOSTROPHE",VK_OEM_7},{"COMMA",VK_OEM_COMMA},{"PERIOD",VK_OEM_PERIOD},{"SLASH",VK_OEM_2},
};
/* a newly pressed key's controls.txt name ("" = none yet). F3/F7-F10/F12 and
 * Esc are the port's own keys and are not offered. */
static std::string capture_key(void)
{
    static bool held[256];
    std::string got;
    for (int vk = 1; vk < 256; vk++) {
        const bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (d && !held[vk] && got.empty()) {
            if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) got = std::string(1, (char)vk);
            else for (auto& e : s_vkn) if (e.vk == vk) { got = e.n; break; }
        }
        held[vk] = d;
    }
    return got;
}
static void keys_set(int row, const std::string& key)
{
    KeyRow& r = s_keys[row];
    char line[256]; snprintf(line, sizeof line, "%-10s = %-10s # %s", key.c_str(), r.action.c_str(), r.label.c_str());
    s_ctl_lines[r.line] = line; r.key = key;
    if (write_lines("controls.txt", s_ctl_lines)) cellpad_reload_controls();
    LOG("key binding: %s -> %s (%s)", key.c_str(), r.action.c_str(), r.label.c_str());
}

static int rows(void)
{
    if (s_page == PG_MAIN) return (int)s_opts.size() + 3;    /* + Key bindings, Controller layout, Back */
    if (s_page == PG_KEYS) return (int)s_keys.size() + 2;    /* + Restore defaults, Back */
    return XB_COUNT + 2;                                     /* + Southpaw, Back */
}
static std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
static void row_text(int i, std::wstring& l, std::wstring& v)
{
    l.clear(); v.clear();
    const int n = rows();
    if (i == n - 1) { l = L"Back"; return; }
    if (s_page == PG_MAIN) {
        const int no = (int)s_opts.size();
        if (i == no) { l = L"Key bindings (keyboard & mouse)"; v = L"\u25B8"; return; }
        if (i == no + 1) { l = L"Controller layout"; v = L"\u25B8"; return; }
        const XOpt& o = s_opts[i];
        l = o.title; v = L"\u25C2  " + o.ch[o.sel].label + (o.restart && o.sel != o.run ? L" *" : L"") + L"  \u25B8";
    } else if (s_page == PG_KEYS) {
        if (i == (int)s_keys.size()) { l = L"Restore default keys"; return; }
        l = wide(s_keys[i].label);
        v = (s_capture && i == s_sel) ? L"press a key...  (Esc cancels)" : wide(s_keys[i].key);
    } else {
        if (i == XB_COUNT) { l = L"Southpaw (swap sticks)"; v = s_southpaw ? L"\u25C2  On  \u25B8" : L"\u25C2  Off  \u25B8"; return; }
        l = s_xb_name[i]; v = std::wstring(L"\u25C2  acts as ") + s_xb_name[s_xmap[i]] + L"  \u25B8";
    }
}
static std::wstring help_text(void)
{
    if (s_page == PG_MAIN && s_sel < (int)s_opts.size()) {
        const XOpt& o = s_opts[s_sel]; std::wstring s = o.help;
        if (o.restart) s += L"  (Applies the next time the game starts.)";
        return s;
    }
    if (s_page == PG_KEYS) return L"Enter / A: bind a new key or mouse button.  Saved to controls.txt.";
    if (s_page == PG_PAD) return L"\u2190\u2192 choose what each controller button does. Saved for the launcher too.";
    return L"";
}
static void paint(HDC dc, const RECT& rc)
{
    const int n = rows(), vis = std::min(n, 14);
    const int H = rc.bottom - rc.top, W = rc.right - rc.left, rh = H / (vis + 4);
    if (s_sel < s_top) s_top = s_sel;
    if (s_sel >= s_top + vis) s_top = s_sel - vis + 1;
    const int top = rc.top + rh * 3 / 2;
    auto font = [&](int px, int wt) { return CreateFontW(-px, 0, 0, 0, wt, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI"); };
    HFONT ft = font(rh * 62 / 100, FW_BOLD), fr = font(rh * 46 / 100, FW_SEMIBOLD), fs = font(rh * 34 / 100, FW_NORMAL);
    HGDIOBJ of = SelectObject(dc, ft);
    RECT r = { rc.left + rh / 2, rc.top + rh / 3, rc.right - rh / 2, top };
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, s_page == PG_MAIN ? L"MORE PC OPTIONS" : s_page == PG_KEYS ? L"KEY BINDINGS" : L"CONTROLLER LAYOUT", -1, &r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, fr);
    const int vx = rc.left + W * 50 / 100;
    for (int k = 0; k < vis && s_top + k < n; k++) {
        const int i = s_top + k;
        RECT rr = { rc.left + 6, top + k * rh, rc.right - 6, top + (k + 1) * rh };
        if (i == s_sel) { HBRUSH b = CreateSolidBrush(RGB(150, 32, 24)); FillRect(dc, &rr, b); DeleteObject(b); }
        SetTextColor(dc, i == s_sel ? RGB(255, 255, 255) : RGB(205, 205, 210));
        std::wstring l, v; row_text(i, l, v);
        RECT rl = { rr.left + rh / 2, rr.top, vx, rr.bottom };
        DrawTextW(dc, l.c_str(), -1, &rl, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT rv = { vx, rr.top, rr.right - rh / 2, rr.bottom };
        DrawTextW(dc, v.c_str(), -1, &rv, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    SelectObject(dc, fs);
    SetTextColor(dc, RGB(185, 185, 190));
    RECT rh1 = { rc.left + rh / 2, top + vis * rh + rh / 6, rc.right - rh / 2, top + vis * rh + rh * 7 / 6 };
    DrawTextW(dc, help_text().c_str(), -1, &rh1, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    SetTextColor(dc, RGB(140, 140, 150));
    RECT rh2 = { rc.left + rh / 2, top + vis * rh + rh * 7 / 6, rc.right - rh / 2, rc.bottom - rh / 6 };
    DrawTextW(dc, L"\u2191\u2193 select    \u2190\u2192 change    Enter / A  OK    Esc / B  back", -1, &rh2,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, of); DeleteObject(ft); DeleteObject(fr); DeleteObject(fs);
}
static std::wstring s_msg1, s_msg2;     /* a dialog instead of the menu (controller lost, photo mode) */
static void ov_draw(HDC mdc, const RECT& rc)
{
    HBRUSH bg = CreateSolidBrush(RGB(16, 16, 20)); FillRect(mdc, &rc, bg); DeleteObject(bg);
    HBRUSH fr = CreateSolidBrush(RGB(190, 40, 30));
    for (int i = 0; i < 2; i++) { RECT r = { rc.left + i, rc.top + i, rc.right - i, rc.bottom - i }; FrameRect(mdc, &r, fr); }
    DeleteObject(fr);
    SetBkMode(mdc, TRANSPARENT);
    if (s_open) paint(mdc, rc);
    else {
        const int H = rc.bottom - rc.top;
        HFONT f1 = CreateFontW(-H * 26 / 100, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT f2 = CreateFontW(-H * 15 / 100, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ of = SelectObject(mdc, f1);
        RECT r1 = { rc.left + 8, rc.top + H * 10 / 100, rc.right - 8, rc.top + H * 55 / 100 };
        SetTextColor(mdc, RGB(255, 255, 255));
        DrawTextW(mdc, s_msg1.c_str(), -1, &r1, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mdc, f2);
        RECT r2 = { rc.left + 8, rc.top + H * 55 / 100, rc.right - 8, rc.bottom - H * 10 / 100 };
        SetTextColor(mdc, RGB(200, 200, 205));
        DrawTextW(mdc, s_msg2.c_str(), -1, &r2, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mdc, of); DeleteObject(f1); DeleteObject(f2);
    }
}
static LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
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
/* kind 0 = menu (centre, large), 1 = dialog (centre), 2 = small note at the top.
 * Drawn into the swapchain (port_osd.h) when it is up, else a layered window. */
static int s_ov_kind = -1, s_ov_osd = 0, s_ov_cw = 0, s_ov_ch = 0;
static void ov_show(int kind)
{
    RECT rc; GetClientRect(s_game, &rc);
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    int w, h, x, y;
    if (kind == 0) {
        const int rh = std::max(30, ch * 48 / 1000);
        w = std::max(640, cw * 50 / 100); h = rh * (std::min(rows(), 14) + 4);
        w = std::min(w, cw); h = std::min(h, ch);
        x = (cw - w) / 2; y = (ch - h) / 2;
    } else {
        w = std::max(kind == 2 ? 300 : 420, cw * (kind == 2 ? 28 : 46) / 100);
        h = std::max(kind == 2 ? 56 : 130, ch * (kind == 2 ? 8 : 20) / 100);
        x = (cw - w) / 2; y = kind == 2 ? ch * 4 / 100 : (ch - h) / 2;
    }
    s_ov_kind = kind; s_ov_cw = cw; s_ov_ch = ch;
    s_ov_osd = posd_on();
    if (s_ov_osd) {
        if (s_ov && IsWindowVisible(s_ov)) ShowWindow(s_ov, SW_HIDE);
        PosdCanvas c;
        if (!c.begin(w, h)) return;
        const RECT r = { 0, 0, w, h };
        ov_draw(c.dc, r);
        c.commit(POSD_XMENU, x, y, cw, ch, 235 / 255.f);
        return;
    }
    r2rsx_osd_hide(POSD_XMENU);
    if (!s_ov) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.lpfnWndProc = ov_proc; wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = L"pcx_overlay";
        RegisterClassExW(&wc);
        s_ov = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                               L"pcx_overlay", L"", WS_POPUP, 0, 0, 10, 10, s_game, NULL, wc.hInstance, NULL);
        if (s_ov) SetLayeredWindowAttributes(s_ov, 0, 235, LWA_ALPHA);
    }
    if (!s_ov) return;
    POINT tl = { 0, 0 }; ClientToScreen(s_game, &tl);
    SetWindowPos(s_ov, HWND_TOPMOST, tl.x + x, tl.y + y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(s_ov, NULL, TRUE);
    UpdateWindow(s_ov);
}
static void ov_hide(void) { s_ov_kind = -1; if (s_ov) ShowWindow(s_ov, SW_HIDE); r2rsx_osd_hide(POSD_XMENU); }
/* the content changed (menu selection, a value) */
static void ov_refresh(void)
{
    if (s_ov_kind < 0) return;
    if (s_ov_osd) ov_show(s_ov_kind);
    else if (s_ov) InvalidateRect(s_ov, NULL, FALSE);
}
/* per tick: follow a change of the client size or of the OSD becoming ready */
static void ov_follow(void)
{
    if (s_ov_kind < 0) return;
    RECT rc; GetClientRect(s_game, &rc);
    if (posd_on() != s_ov_osd || rc.right - rc.left != s_ov_cw || rc.bottom - rc.top != s_ov_ch) ov_show(s_ov_kind);
}

enum { IN_YES = 1, IN_NO = 2, IN_UP = 8, IN_DOWN = 16, IN_LEFT = 32, IN_RIGHT = 64, IN_F10 = 128 };
/* All controllers' buttons (XInput bits), the left stick as the d-pad;
 * from cellPad.c, which reads SDL3 or XInput (PAD_BACKEND). */
extern "C" unsigned cellpad_menu_buttons(int* any_connected);
static WORD xi_raw(int* any_connected) { return (WORD)cellpad_menu_buttons(any_connected); }
static int read_input(int focused)
{
    int r = 0;
    if (focused) {
        auto kd = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        if (kd(VK_RETURN) || kd(VK_SPACE)) r |= IN_YES;
        if (kd(VK_ESCAPE) || kd(VK_BACK)) r |= IN_NO;
        if (kd(VK_F10) || kd(VK_F9)) r |= IN_F10;
        if (kd(VK_UP)) r |= IN_UP;
        if (kd(VK_DOWN)) r |= IN_DOWN;
        if (kd(VK_LEFT)) r |= IN_LEFT;
        if (kd(VK_RIGHT)) r |= IN_RIGHT;
    }
    const WORD b = xi_raw(NULL);
    if (b & XINPUT_GAMEPAD_A) r |= IN_YES;
    if (b & XINPUT_GAMEPAD_B) r |= IN_NO;
    if (b & XINPUT_GAMEPAD_DPAD_UP) r |= IN_UP;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN) r |= IN_DOWN;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT) r |= IN_LEFT;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT) r |= IN_RIGHT;
    return r;
}
static int edges(int in)
{
    static ULONGLONG rep_at = 0;
    const ULONGLONG now = GetTickCount64();
    int e = in & ~s_prev_in;
    const int dirs = in & (IN_UP | IN_DOWN | IN_LEFT | IN_RIGHT);
    if (e & dirs) rep_at = now + 400;
    else if (dirs && rep_at && now >= rep_at) { e |= dirs; rep_at = now + 110; }
    if (!dirs) rep_at = 0;
    s_prev_in = in;
    return e;
}
static void opt_change(int i, int d)
{
    XOpt& o = s_opts[i];
    const int n = (int)o.ch.size();
    o.sel = (o.sel + d + n) % n;
    const std::string& v = o.ch[o.sel].value;
    ini_set(o.ini, v);
    bat_set(o.env, v);
    if (o.live) o.live(v);
    LOG("%ls = %ls%s", o.title, o.ch[o.sel].label.c_str(), o.restart ? " (next start)" : "");
}
static void pad_change(int i, int d)
{
    if (i == XB_COUNT) {
        s_southpaw = !s_southpaw;
        ini_set("pad_southpaw", s_southpaw ? "1" : ""); bat_set("PAD_SOUTHPAW", s_southpaw ? "1" : "");
        return;
    }
    s_xmap[i] = (s_xmap[i] + d + XB_COUNT) % XB_COUNT;
    int ident = 1; for (int k = 0; k < XB_COUNT; k++) if (s_xmap[k] != k) ident = 0;
    s_xmap_ident = ident;
    char ini[32], env[32]; snprintf(ini, sizeof ini, "padmap_%s", s_xb_key[i]); snprintf(env, sizeof env, "PAD_MAP_%s", s_xb_key[i]);
    for (char* c = ini; *c; c++) *c = (char)tolower((unsigned char)*c);
    const std::string v = s_xmap[i] == i ? "" : s_xb_key[s_xmap[i]];
    ini_set(ini, v); bat_set(env, v);
}
extern "C" int r2x_menu_open(void) { return s_open || s_release; }
extern "C" void r2x_open_menu(void)
{
    opts_init();
    s_open = 1; s_page = PG_MAIN; s_sel = 0; s_top = 0; s_capture = 0;
    s_prev_in = -1;                              /* the key that opened it is not a press */
    g_port_ui_modal = 1;
    ClipCursor(NULL);
    ov_show(0);
    LOG("extras menu");
}
static void menu_close(void) { s_open = 0; s_capture = 0; s_release = 1; ov_hide(); }
static void menu_tick(int focused)
{
    g_port_ui_modal = 1;
    if (s_capture) {
        const std::string k = capture_key();
        if (s_capture == 1) { if (k.empty() && !(GetAsyncKeyState(VK_RETURN) & 0x8000) && !(GetAsyncKeyState(VK_SPACE) & 0x8000)) s_capture = 2; return; }
        if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000)) { s_capture = 0; s_prev_in = read_input(focused); ov_refresh(); return; }
        if (!k.empty() && focused) { keys_set(s_sel, k); s_capture = 0; s_prev_in = read_input(focused) | IN_YES; ov_refresh(); }
        return;
    }
    const int in = read_input(focused);
    if (s_prev_in < 0) { s_prev_in = in; return; }
    const int e = edges(in), n = rows();
    if (e & IN_F10) { menu_close(); return; }
    if (e & IN_UP) s_sel = (s_sel + n - 1) % n;
    else if (e & IN_DOWN) s_sel = (s_sel + 1) % n;
    else if (e & IN_NO) {
        if (s_page == PG_MAIN) { menu_close(); return; }
        s_sel = (int)s_opts.size() + (s_page == PG_PAD); s_page = PG_MAIN; s_top = 0; ov_show(0); return;
    } else if (e & (IN_LEFT | IN_RIGHT | IN_YES)) {
        const int d = (e & IN_LEFT) ? -1 : 1;
        const bool ok = (e & IN_YES) != 0;
        if (s_sel == n - 1) {                    /* Back */
            if (!ok) return;
            if (s_page == PG_MAIN) { menu_close(); return; }
            s_sel = (int)s_opts.size() + (s_page == PG_PAD); s_page = PG_MAIN; s_top = 0; ov_show(0); return;
        }
        if (s_page == PG_MAIN) {
            const int no = (int)s_opts.size();
            if (s_sel < no) opt_change(s_sel, d);
            else if (ok) { s_page = s_sel == no ? PG_KEYS : PG_PAD; s_sel = 0; s_top = 0; if (s_page == PG_KEYS) keys_load(); ov_show(0); return; }
        } else if (s_page == PG_KEYS) {
            if (!ok) return;
            if (s_sel == (int)s_keys.size()) {   /* defaults: cellPad writes a fresh file */
                CopyFileA("controls.txt", "controls.txt.bak", FALSE);
                DeleteFileA("controls.txt"); cellpad_reload_controls();
                LOG("key bindings reset (old file: controls.txt.bak)");
                s_page = PG_MAIN; s_sel = (int)s_opts.size(); ov_show(0); return;
            }
            s_capture = 1;
        } else pad_change(s_sel, d);
    } else return;
    ov_refresh();
}

/* ---- crosshair: a click-through colour-keyed window (or OSD layer) at the centre ---- */
static HWND s_xh = NULL; static int s_xh_kind = -1;
static void xh_draw(HDC dc, const RECT& rc)
{
    HBRUSH k = CreateSolidBrush(RGB(255, 0, 255)); FillRect(dc, &rc, k); DeleteObject(k);
    const int c = rc.right / 2;
    HBRUSH o = CreateSolidBrush(RGB(0, 0, 0)), f = CreateSolidBrush(RGB(240, 240, 240));
    if (s_xh_kind == 1) {
        RECT a = { c - 3, c - 3, c + 3, c + 3 }; FillRect(dc, &a, o); RECT b = { c - 2, c - 2, c + 2, c + 2 }; FillRect(dc, &b, f);
    } else {
        RECT a = { c - 10, c - 2, c - 3, c + 2 }, b = { c + 3, c - 2, c + 10, c + 2 }, d = { c - 2, c - 10, c + 2, c - 3 }, e = { c - 2, c + 3, c + 2, c + 10 };
        for (RECT* r : { &a, &b, &d, &e }) { FillRect(dc, r, o); RECT in = { r->left + 1, r->top + 1, r->right - 1, r->bottom - 1 }; FillRect(dc, &in, f); }
    }
    DeleteObject(o); DeleteObject(f);
}
static LRESULT CALLBACK xh_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        xh_draw(dc, rc);
        EndPaint(h, &ps); return 0;
    }
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}
static void crosshair_tick(int show)
{
    if (!show) { if (s_xh && IsWindowVisible(s_xh)) ShowWindow(s_xh, SW_HIDE); r2rsx_osd_hide(POSD_XHAIR); return; }
    RECT rc; GetClientRect(s_game, &rc);
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (posd_on()) {
        if (s_xh && IsWindowVisible(s_xh)) ShowWindow(s_xh, SW_HIDE);
        static int kcw = 0, kch = 0;
        if (s_xh_kind != s_crosshair || cw != kcw || ch != kch || !r2rsx_osd_visible(POSD_XHAIR)) {
            s_xh_kind = s_crosshair; kcw = cw; kch = ch;
            if (s_xh) InvalidateRect(s_xh, NULL, TRUE);
            PosdCanvas c;
            if (!c.begin(24, 24)) return;
            const RECT r = { 0, 0, 24, 24 };
            xh_draw(c.dc, r);
            c.commit(POSD_XHAIR, cw / 2 - 12, ch / 2 - 12, cw, ch, 220 / 255.f, RGB(255, 0, 255));
        }
        return;
    }
    r2rsx_osd_hide(POSD_XHAIR);
    if (!s_xh) {
        WNDCLASSEXW wc = { sizeof wc }; wc.lpfnWndProc = xh_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"pcx_crosshair";
        RegisterClassExW(&wc);
        s_xh = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                               L"pcx_crosshair", L"", WS_POPUP, 0, 0, 24, 24, s_game, NULL, wc.hInstance, NULL);
        if (!s_xh) return;
        SetLayeredWindowAttributes(s_xh, RGB(255, 0, 255), 220, LWA_COLORKEY | LWA_ALPHA);
    }
    if (s_xh_kind != s_crosshair) { s_xh_kind = s_crosshair; InvalidateRect(s_xh, NULL, TRUE); }
    POINT c = { cw / 2, ch / 2 }; ClientToScreen(s_game, &c);
    SetWindowPos(s_xh, HWND_TOPMOST, c.x - 12, c.y - 12, 24, 24, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

/* ======================================================================
 * The per-tick driver, called at the end of r3_pcport.cpp ui_tick */
static int s_init = 0, s_disc_paused = 0, s_photo_ov = 0;
static void init_once(void)
{
    if (s_init) return;
    s_init = 1;
    opts_init();
    for (auto& o : s_opts) if (o.live) o.live(o.ch[o.run].value);   /* apply the start-up values */
    s_photo_allowed = envi("PHOTO", 1) != 0;
    if (envi("DISCORD", 0)) { HANDLE t = CreateThread(NULL, 0, discord_thread, NULL, 0, NULL); if (t) CloseHandle(t); }
    s_bench = envi("BENCH", 0) != 0;
    if (s_bench) {
        bench_parse();
        s_bench_secs = std::max(5.0, atof(envs("BENCH_SECONDS", "60").c_str()));
        FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        FILETIME nowft; GetSystemTimeAsFileTime(&nowft);
        const double since = ((double)((((ULONGLONG)nowft.dwHighDateTime) << 32 | nowft.dwLowDateTime) -
                                       (((ULONGLONG)c.dwHighDateTime) << 32 | c.dwLowDateTime))) / 1e7;
        s_bench_t0 = now_s() - since;            /* the script is timed from process start */
        LOG("benchmark mode: %zu steps, then %.0f s", s_bench_steps.size(), s_bench_secs);
    }
    LOG("extras: F9 / PC menu -> More PC Options; photo mode %s (F7); cutscene skip %s; crosshair %d; pause on pad loss %s",
        s_photo_allowed ? "on" : "off", s_cut_skip ? "on" : "off", s_crosshair, s_disc_pause_on ? "on" : "off");
}
static int s_want_pause = 0;
extern "C" int r2x_want_pause(void) { return s_want_pause; }
extern "C" void r2x_ui_tick(void* hwnd_, int focused, int other_prompt)
{
    HWND hwnd = (HWND)hwnd_;
    s_game = hwnd;
    init_once();
    const ULONGLONG now = GetTickCount64();
    bench_tick(hwnd);

    /* F9: the extras menu directly; F10 -> More PC Options opens it from the PC menu */
    { static int was = 0; const int d = focused && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
      if (d && !was && !s_open && !other_prompt && !s_photo_on) r2x_open_menu();
      was = d; }
    if (s_open) menu_tick(focused);
    else if (s_release) {                        /* hold the game's input until the closing key is let go */
        if (!(read_input(focused) & (IN_YES | IN_NO | IN_F10))) { s_release = 0; g_port_ui_modal = 0; }
        else g_port_ui_modal = 1;
    }

    /* photo mode */
    { static int was = 0; const int d = focused && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
      if (d && !was && s_photo_allowed && !s_open && !other_prompt) {
          s_photo_on = !s_photo_on;
          static int ov_was = 0;
          if (s_photo_on) { ov_was = r2pf_overlay_get(); r2pf_overlay_set(0); s_msg1 = L"Photo mode"; s_msg2 = L"F12 screenshot    F7 back to the game";
                            ov_show(2); s_photo_ov = 1; LOG("photo mode on"); }
          else { r2pf_overlay_set(ov_was); ov_hide(); s_photo_ov = 0; s_release = 1; LOG("photo mode off"); }
      }
      was = d; }
    /* the note fades after 2 s so it is not in the way (screenshots never include it) */
    { static ULONGLONG t_on = 0;
      if (s_photo_on && s_photo_ov) { if (!t_on) t_on = now; if (now - t_on > 2000) { ov_hide(); s_photo_ov = 0; } }
      if (!s_photo_on) t_on = 0; }

    /* pause when the controller in use goes away */
    { int conn = 0; xi_raw(&conn);
      static int was_conn = -1;
      if (was_conn == 1 && !conn && s_disc_pause_on && !s_open && !s_photo_on && !s_bench) {
          s_disc_paused = 1; s_msg1 = L"Controller disconnected"; s_msg2 = L"Reconnect it, or press Enter to continue";
          ov_show(1); LOG("controller lost: paused");
      }
      if (s_disc_paused && (conn || (focused && (GetAsyncKeyState(VK_RETURN) & 0x8000)))) {
          s_disc_paused = 0; s_release = 1; if (!s_open) ov_hide(); LOG("controller pause ended");
      }
      was_conn = conn; }

    /* hold to skip a pre-rendered movie */
    if (s_cut_skip && s_movie_slot >= 0 && !s_open) {
        static ULONGLONG held_since = 0, told = 0;
        const WORD b = xi_raw(NULL);
        const int held = (focused && ((GetAsyncKeyState(VK_SPACE) & 0x8000) || (GetAsyncKeyState(VK_RETURN) & 0x8000))) ||
                         (b & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START));
        if (told != s_movie_t0 && now - s_movie_t0 > 500) { told = s_movie_t0; if (skip_hint_on()) r2pf_hint("Cutscene", "Hold Space / A to skip", 4000); }
        if (!held) held_since = 0;
        else if (!held_since) held_since = now;
        else if (now - held_since >= 1000 && (now - held_since) % 400 < 120) {
            g_pad_inj_sticks = 0x80808080u;
            g_pad_inj_btn = ((now - held_since) / 400) & 1 ? 0x4000u /* CROSS */ : 0x0008u /* START */;
            g_pad_inj_until = now + 80;
        }
    }

    const int on_foot = g_r2_look_ms && now - g_r2_look_ms < 250;
    crosshair_tick(s_crosshair && focused && on_foot && !s_open && !s_photo_on && !other_prompt);

    ov_follow();
    s_want_pause = s_open || s_photo_on || s_disc_paused;
    /* the game gets no input while any of ours is up; released once, when they all end */
    { static int held = 0;
      if (s_open || s_release || s_photo_on || s_disc_paused) { g_port_ui_modal = 1; held = 1; }
      else if (held) { held = 0; if (!other_prompt) g_port_ui_modal = 0; } }
}

/* registered before main(); callbacks run once the game does */
static struct PcExtraInit {
    PcExtraInit()
    {
        xmap_load();
        g_port_xi_hook = xi_hook;
        g_port_file_open_cb = file_open_cb;
        g_port_file_close_cb = file_close_cb;
    }
} s_pcextra_init;
