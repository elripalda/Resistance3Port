// Resistance 3: temporal AA on the native Vulkan renderer (R2_RSX=vulkan): RSX_FSR=2 DLAA (DLSS),
// RSX_FSR=3 AMD FSR 3.1 native AA, RSX_FSR=1 FSR1 output scaling only.
// Ported from the Resistance 2 port's r2_native_aa.cpp, with R3's split frames (below).
//
// The policy of the RPCS3 path's VKR2Upscaler (patches/r3-rpcs3.patch,
// r2aa_on_draw / r2aa_on_flip / r2aa_run), driven by ps3recomp's generic
// draw-engine hooks (rsx_draw_engine_set_aa_hooks) and the Vulkan backend's
// temporal-upscaler slot, with project/r2_dlss.cpp (r2_dlss.dll, NGX) or ps3recomp's rsx_vk_fsr3
// (amd_fidelityfx_vk.dll, FidelityFX SDK 1.1.4) as the upscaler:
//  * scene  = the depth buffer with the most depth-tested draws last frame and
//             its busiest, latest colour target; it is drawn in several runs
//             (pre-pass, material pass), the AA runs after the last one, before
//             transparents/post/HUD (first other target or the flip);
//  * split  = R3 renders the next frame's geometry pre-pass before the flip, so
//             (as in R3's VKR2Upscaler, RSX_AA_SPLIT, default on) the logical
//             frame ends after the material pass (a scene-pair run of >= 8
//             depth-tested draws without depth writes): the AA runs at the next
//             other target, and the next frame starts at the next scene-pair
//             draw; flips in between are ignored (up to 3). RSX_AA_SPLIT=0 =
//             the R2 policy (frame boundary at the flip);
//  * jitter = Halton(2,3) over RSX_AA_PHASES frames, added to the viewport of
//             scene draws only, once the upscaler ran on an unchanged scene;
//             FSR 3.1 uses its own sequence and phase count (ffxQuery; RSX_AA_PHASES
//             overrides the count only when set), in render pixels, +x right,
//             +y down, the same value for the projection and for the upscaler;
//  * motion = camera reprojection: each scene draw's object->clip matrix is
//             recovered by evaluating its vertex program on the CPU, matched
//             to last frame's by (program, vertex array, index array,
//             occurrence), and the majority M_prev * M_cur^-1 is the camera.
// Motion vectors: current -> previous, the backend scales them to render pixels.
// RSX_SHARPEN (0..1, or a percentage) sets FSR 3.1's RCAS (default off) and FSR1's.
// If FSR 3.1 can't load or start, FSR1 output scaling is used instead (logged once).
// RSX_UPSCALE < 1 exists on the RPCS3 path only (the native pass is 1:1).
// (RSX_FSR=1 means FSR 3.1 on the RPCS3 path; the launcher writes 3 for the native renderer.)
#if defined(_WIN32) && defined(PS3RECOMP_RSX_VULKAN)

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../ps3recomp/libs/video/rsx_draw_engine.h"
#include "../ps3recomp/libs/video/rsx_vp_eval.h"
#include "../ps3recomp/libs/video/rsx_vulkan_backend.h"
#include "../ps3recomp/libs/video/rsx_vk_fsr3.h"

namespace {

void aalog(const char* fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[r3aa] %s\n", b);
}

void dlss_log(const char* s) { aalog("%s", s); }

// ---- r2_dlss.dll --------------------------------------------------------
struct Dlss {
    HMODULE dll = nullptr;
    int (*required)(unsigned*, const char***, unsigned*, const char***) = nullptr;
    int (*init)(void*, void*, void*, void*, void*, const wchar_t*, void (*)(const char*)) = nullptr;
    int (*create)(void*, unsigned, unsigned, unsigned, unsigned, int, unsigned) = nullptr;
    void (*set_mask)(u64, u64, int) = nullptr;
    int (*eval)(void*, u64, u64, int, u64, u64, int, u64, u64, int, u64, u64, int, unsigned, unsigned,
                float, float, float, float, int, float) = nullptr;
    void (*shutdown)(void) = nullptr;
} g_dlss;

std::wstring exe_dir()
{
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path, n);
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

template <class F> bool sym(F& f, const char* name)
{
    f = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(g_dlss.dll, name)));
    return f != nullptr;
}

bool dlss_load()
{
    const std::wstring path = exe_dir() + L"\\r2_dlss.dll";
    g_dlss.dll = LoadLibraryW(path.c_str());
    if (!g_dlss.dll) { aalog("r2_dlss.dll not found next to the game; DLAA unavailable"); return false; }
    if (!sym(g_dlss.required, "r2_dlss_required_extensions") || !sym(g_dlss.init, "r2_dlss_init") ||
        !sym(g_dlss.create, "r2_dlss_create") || !sym(g_dlss.set_mask, "r2_dlss_set_mask") ||
        !sym(g_dlss.eval, "r2_dlss_eval") || !sym(g_dlss.shutdown, "r2_dlss_shutdown")) {
        aalog("r2_dlss.dll is incomplete; DLAA unavailable");
        FreeLibrary(g_dlss.dll);
        g_dlss.dll = nullptr;
        return false;
    }
    return true;
}

// ---- settings (the launcher's / the RPCS3 path's environment) -----------
struct Cfg {
    float jsign = 1.f;     // RSX_AA_JITTER_SIGN=-1 flips the jitter handed to DLSS
    unsigned preset = 0;   // RSX_DLSS_PRESET
    unsigned phases = 8;   // RSX_AA_PHASES
    bool phases_set = false;
    bool fsr3 = false;     // RSX_FSR=3
    bool debug = false;    // RSX_AA_DEBUG
    unsigned view = 0;     // RSX_AA_DEBUG_VIEW: 1 |AA - input| x8, 2 split screen
    bool mask = true;      // RSX_DLSS_MASK=0: no moving-object mask
    bool split = true;     // RSX_AA_SPLIT=0: frame boundary at the flip (R2)
} g_cfg;

void read_cfg()
{
    if (const char* e = std::getenv("RSX_AA_JITTER_SIGN")) g_cfg.jsign = std::atof(e) < 0 ? -1.f : 1.f;
    if (const char* e = std::getenv("RSX_DLSS_PRESET")) g_cfg.preset = (unsigned)std::atoi(e);
    if (const char* e = std::getenv("RSX_AA_PHASES")) {
        const int p = std::atoi(e);
        g_cfg.phases = (unsigned)(p < 1 ? 1 : p > 64 ? 64 : p);
        g_cfg.phases_set = true;
    }
    if (const char* e = std::getenv("RSX_AA_DEBUG")) g_cfg.debug = std::atoi(e) != 0;
    if (const char* e = std::getenv("RSX_AA_DEBUG_VIEW")) g_cfg.view = (unsigned)std::atoi(e);
    if (const char* e = std::getenv("RSX_DLSS_MASK")) g_cfg.mask = std::atoi(e) != 0;
    if (const char* e = std::getenv("RSX_AA_SPLIT")) g_cfg.split = std::atoi(e) != 0;
}

// ---- the backend's upscaler ops over r2_dlss.dll -------------------------
int op_required(void*, unsigned* ni, const char*** ie, unsigned* nd, const char*** de)
{
    return g_dlss.required(ni, ie, nd, de);
}

int op_init(void*, void* inst, void* phys, void* dev, void* gipa, void* gdpa)
{
    static const std::wstring dir = exe_dir();
    return g_dlss.init(inst, phys, dev, gipa, gdpa, dir.c_str(), dlss_log);
}

int op_create(void*, void* cmd, unsigned w, unsigned h, unsigned ow, unsigned oh, int flags)
{
    return g_dlss.create(cmd, w, h, ow, oh, flags & 1, g_cfg.preset) == 1;
}

int op_evaluate(void*, void* cmd, const rsx_vk_upscaler_image* c, const rsx_vk_upscaler_image* d,
                const rsx_vk_upscaler_image* m, const rsx_vk_upscaler_image* k,
                const rsx_vk_upscaler_image* o, unsigned ow, unsigned oh, float jx, float jy,
                float sx, float sy, int reset, float ms)
{
    if (k && k->image) g_dlss.set_mask(k->image, k->view, k->format);
    else g_dlss.set_mask(0, 0, 0);
    return g_dlss.eval(cmd, c->image, c->view, c->format, d->image, d->view, d->format,
                       m->image, m->view, m->format, o->image, o->view, o->format, ow, oh,
                       jx, jy, sx, sy, reset, ms) == 1;
}

void op_shutdown(void*) { g_dlss.shutdown(); }

const rsx_vk_upscaler_ops g_ops = { nullptr, op_required, op_init, op_create, op_evaluate, op_shutdown };

// ---- 4x4 (row-major, m[r][c]) --------------------------------------------
struct m4 { double m[4][4]; };

m4 m4_identity()
{
    m4 r{};
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0;
    return r;
}

m4 m4_mul(const m4& a, const m4& b)
{
    m4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

bool m4_inverse(const m4& a, m4& out)
{
    double t[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) t[r][c] = c < 4 ? a.m[r][c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(t[r][c]) > std::fabs(t[p][c])) p = r;
        if (std::fabs(t[p][c]) < 1e-12) return false;
        if (p != c)
            for (int k = 0; k < 8; ++k) std::swap(t[p][k], t[c][k]);
        const double inv = 1.0 / t[c][c];
        for (int k = 0; k < 8; ++k) t[c][k] *= inv;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = t[r][c];
            if (f != 0.0)
                for (int k = 0; k < 8; ++k) t[r][k] -= f * t[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out.m[r][c] = t[r][c + 4];
    return true;
}

double m4_maxabs(const m4& a)
{
    double v = 0;
    for (const auto& r : a.m)
        for (double x : r) v = std::fmax(v, std::fabs(x));
    return v;
}

double m4_maxdiff(const m4& a, const m4& b)
{
    double v = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) v = std::fmax(v, std::fabs(a.m[i][j] - b.m[i][j]));
    return v;
}

// The engine's clip -> NDC epilogue (unjittered), as Vulkan NDC (y down).
struct Screen { float xf[8]; bool valid = false; };

m4 m4_screen(const Screen& s)
{
    m4 r{};
    r.m[0][0] = s.xf[0]; r.m[0][3] = s.xf[4];
    r.m[1][1] = -s.xf[1]; r.m[1][3] = -s.xf[5];
    r.m[2][2] = s.xf[2]; r.m[2][3] = s.xf[6];
    r.m[3][3] = 1.0;
    return r;
}

float halton(unsigned index, unsigned base)
{
    float f = 1.f, r = 0.f;
    while (index > 0) {
        f /= (float)base;
        r += f * (float)(index % base);
        index /= base;
    }
    return r;
}

u64 mix64(u64 h, u64 v) { return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2)); }

// ---- per-frame state -------------------------------------------------------
struct Tally { unsigned count = 0, first_draw = 0, segments = 0; };

struct State {
    bool disabled = false;
    std::unordered_map<u64, Tally> tally;
    u64 last_pair = ~0ull;
    unsigned draw_index = 0;
    bool scene_valid = false, scene_changed = true;
    u32 scene_rt = 0, scene_ds = 0;
    unsigned seg_count = 0, seg_target = 1;
    unsigned scene_draws = 0;
    bool aa_done = false, cur_scene = false;
    // the scene as last seen this frame
    u32 color = 0, depth = 0, w = 0, h = 0;
    u32 rt_loc = 0, rt_off = 0;
    float zA = 1.f, zB = 0.f;
    bool inverted = false, msaa = false;
    // camera
    std::unordered_map<u64, m4> cur_M, prev_M;
    std::unordered_map<u64, unsigned> occurrences;
    unsigned evals = 0, eval_fail = 0;
    Screen frame_S, prev_S;
    bool prev_valid = false, ran_ok = false, need_reset = true;
    m4 last_T = m4_identity();
    unsigned frame = 0, jitter_cooldown = 0;
    float jx = 0.f, jy = 0.f;
    std::chrono::steady_clock::time_point last_run{};
    unsigned runs = 0, vote_hits = 0, vote_total = 0;
    std::string last_skip;
    // split frames (Cfg::split)
    bool run_scene = false;          // the current run of draws goes into the scene pair
    unsigned run_draws = 0, run_dw = 0;   // its depth-tested draws / those with depth writes
    bool mat_done = false;           // a material pass (scene-pair run without depth writes) has finished
    bool boundary_pending = false;   // the upscaler ran: the next logical frame starts at the next scene draw
    unsigned flips_since = 0;        // flips since the last logical frame boundary
} g_st;

// All scene passes of the logical frame are done: the upscaler may run at the next non-scene target.
bool scene_passes_done()
{
    const auto& st = g_st;
    if (!g_cfg.split) return st.seg_count >= st.seg_target;
    return st.mat_done || (st.run_scene && st.run_draws >= 8 && !st.run_dw);
}

void frame_boundary();

void skip_once(const char* why)
{
    if (g_st.last_skip != why) { g_st.last_skip = why; aalog("not applied: %s", why); }
}

u32 addr_key(u32 present, u32 loc, u32 off) { return present ? (0x80000000u | (loc << 30) | (off & 0x3FFFFFFFu)) : 0u; }

bool vote_camera(m4& R, unsigned& matched, unsigned& members)
{
    std::vector<m4> rs;
    rs.reserve(g_st.cur_M.size());
    for (const auto& kv : g_st.cur_M) {
        const auto it = g_st.prev_M.find(kv.first);
        if (it == g_st.prev_M.end()) continue;
        m4 inv;
        if (!m4_inverse(kv.second, inv)) continue;
        const m4 r = m4_mul(it->second, inv);
        if (!std::isfinite(m4_maxabs(r))) continue;
        rs.push_back(r);
    }
    matched = (unsigned)rs.size();
    members = 0;
    if (rs.empty()) return false;
    size_t best = 0;
    for (size_t i = 0; i < rs.size(); ++i) {
        const double tol = 1e-3 * (1.0 + m4_maxabs(rs[i]));
        unsigned n = 0;
        for (size_t j = 0; j < rs.size(); ++j)
            if (m4_maxdiff(rs[i], rs[j]) <= tol) ++n;
        if (n > members) { members = n; best = i; }
    }
    R = rs[best];
    return members >= 4 && members * 4 >= matched;
}

void run()
{
    auto& st = g_st;
    st.aa_done = true;
    st.boundary_pending = g_cfg.split;
    if (st.disabled) return;
    if (!rsx_vulkan_backend_temporal_upscaler_ready()) { skip_once("the upscaler is not ready"); return; }
    if (!st.scene_draws || !st.color || !st.depth || !st.frame_S.valid || st.w < 16 || st.h < 16) {
        skip_once("no scene this frame");
        return;
    }
    if (st.msaa) { skip_once("the scene is multisampled"); return; }

    m4 T = m4_identity();
    bool reset = st.need_reset || !st.prev_valid;
    if (st.prev_valid) {
        m4 R, sci;
        unsigned matched = 0, members = 0;
        st.vote_total++;
        if (vote_camera(R, matched, members) && m4_inverse(m4_screen(st.frame_S), sci)) {
            T = m4_mul(m4_screen(st.prev_S), m4_mul(R, sci));
            st.vote_hits++;
            // A camera cut still votes consistently, just with a huge motion: measure it on distant geometry.
            const double zfar = (st.inverted ? 0.002 : 0.998) * st.zA + st.zB;
            double sum = 0;
            int pts = 0;
            for (const double px : { -0.8, 0.0, 0.8 })
                for (const double py : { -0.8, 0.0, 0.8 }) {
                    const double v[4] = { px, py, zfar, 1.0 };
                    double q[4];
                    for (int r = 0; r < 4; ++r)
                        q[r] = T.m[r][0] * v[0] + T.m[r][1] * v[1] + T.m[r][2] * v[2] + T.m[r][3] * v[3];
                    if (std::fabs(q[3]) < 1e-9) continue;
                    sum += std::hypot(q[0] / q[3] - px, q[1] / q[3] - py) * 0.5;
                    ++pts;
                }
            const double motion = pts ? sum / pts : 1e9;
            if (!(motion < 0.2)) {
                reset = true;
                T = m4_identity();
                if (g_cfg.debug) aalog("camera cut (motion %.2f of the screen): history reset", motion);
            }
            st.last_T = T;
        } else if (!matched) {
            reset = true;
        } else {
            T = st.last_T;   // no clear majority (lots of animation): assume the camera kept moving
        }
        if (g_cfg.debug && (st.frame % 120) == 0) aalog("motion vote: %u of %u draws agree", members, matched);
    }
    if (reset) st.last_T = m4_identity();
    st.need_reset = false;

    const auto now = std::chrono::steady_clock::now();
    float ms = st.runs ? std::chrono::duration<float, std::milli>(now - st.last_run).count() : 16.6f;
    ms = std::fmin(std::fmax(ms, 1.f), 100.f);
    st.last_run = now;

    rsx_be_scene_aa q;
    std::memset(&q, 0, sizeof q);
    q.color = st.color; q.depth = st.depth; q.w = st.w; q.h = st.h;
    q.jitter_px[0] = st.jx; q.jitter_px[1] = st.jy;
    q.eval_jitter[0] = st.jx * g_cfg.jsign; q.eval_jitter[1] = st.jy * g_cfg.jsign;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) q.reproject[r * 4 + c] = (float)T.m[r][c];
    q.zA = st.zA; q.zB = st.zB;
    q.inverted = st.inverted ? 1u : 0u;
    q.reset = reset ? 1u : 0u;
    q.frame_ms = ms;
    q.debug_view = g_cfg.view;
    q.use_mask = g_cfg.mask ? 1u : 0u;
    if (!rsx_draw_engine_scene_aa(&q)) {
        skip_once("the upscaler pass failed");
        return;
    }
    if (!st.runs++) aalog("first frame processed (%u scene draws, camera motion %s)", st.scene_draws,
                          st.vote_hits ? "found" : "not yet");
    st.last_skip.clear();
    st.ran_ok = true;
}

// ---- engine hooks ------------------------------------------------------------
void on_draw(void*, const rsx_aa_draw_info* in)
{
    auto& st = g_st;
    st.cur_scene = false;
    if (st.disabled) return;
    if (!rsx_vulkan_backend_temporal_upscaler_ready()) {
        st.disabled = true;
        if (g_cfg.fsr3) {
            aalog("FSR 3.1 unavailable on this device; FSR1 output scaling instead");
            rsx_vulkan_backend_force_output_fsr1();
        } else {
            aalog("DLAA unavailable on this device; native anti-aliasing off");
        }
        return;
    }
    const u32 rt = addr_key(in->color, in->color_location, in->color_offset);
    const u32 ds = in->depth_offset != ~0u ? addr_key(in->depth, in->depth_location, in->depth_offset) : 0u;
    const bool dtest = in->depth_test != 0, dwrite = in->depth_write != 0;
    // Split frames: the upscaler ran; the next logical frame starts with this frame's first scene-pair draw.
    if (st.boundary_pending && st.scene_valid && rt == st.scene_rt && ds == st.scene_ds) frame_boundary();
    const u64 pair = ((u64)rt << 32) | ds;
    const bool new_run = pair != st.last_pair;
    st.last_pair = pair;
    if (ds && (dtest || dwrite)) {
        auto& t = st.tally[pair];
        if (!t.count) t.first_draw = st.draw_index;
        t.count++;
        if (new_run) t.segments++;
    }
    const bool scene_pair = st.scene_valid && rt == st.scene_rt && ds == st.scene_ds;
    if (scene_pair && new_run) st.seg_count++;
    if (new_run) {
        if (st.run_scene && st.run_draws >= 8 && !st.run_dw) st.mat_done = true;
        st.run_scene = scene_pair;
        st.run_draws = st.run_dw = 0;
    }
    if (scene_pair && (dtest || dwrite)) {
        st.run_draws++;
        if (dwrite) st.run_dw++;
    }
    st.draw_index++;

    if (st.scene_valid && !st.aa_done && st.scene_draws && !scene_pair && scene_passes_done()) {
        // The last run of draws into the scene pair is over (geometry pre-pass, then the material pass after
        // the light passes). Anything else now (transparents, post, HUD, a pass that samples the scene): AA first.
        run();
    }

    if (st.scene_valid && !st.aa_done && ds == st.scene_ds && (dtest || dwrite)) {
        st.cur_scene = true;
        st.scene_draws++;
        st.depth = in->depth;
        if (rt == st.scene_rt) {
            st.color = in->color;
            st.rt_loc = in->color_location;
            st.rt_off = in->color_offset;
            st.w = in->w; st.h = in->h;
            st.msaa = in->aa_sx * in->aa_sy > 1;
            st.zA = in->zA; st.zB = in->zB;
            st.inverted = in->depth_func == 0x0204 || in->depth_func == 0x0206;   // GREATER / GEQUAL
        }
        if (st.evals < 384) {
            st.evals++;
            u64 key = mix64(rsx_dsp_vp_start(in->rsx), rsx_dsp_reg(in->rsx, 0x1680));
            key = mix64(key, rsx_dsp_reg(in->rsx, 0x181C));
            const unsigned occ = st.occurrences[key]++;
            key = mix64(key, occ);
            double M[4][4];
            if (rsx_vp_extract_affine(in->rsx, M)) {
                m4 m;
                std::memcpy(m.m, M, sizeof m.m);
                st.cur_M[key] = m;
            } else {
                st.eval_fail++;
            }
        }
    }
}

int on_transform(void*, const float xf[8], float jitter_px[2])
{
    auto& st = g_st;
    if (st.disabled || !st.cur_scene) return 0;
    std::memcpy(st.frame_S.xf, xf, sizeof st.frame_S.xf);
    st.frame_S.valid = true;
    if (st.jx == 0.f && st.jy == 0.f) return 0;
    jitter_px[0] = st.jx;
    jitter_px[1] = st.jy;
    return 1;
}

void on_flip(void*)
{
    auto& st = g_st;
    if (st.disabled) return;
    if (g_cfg.split && st.scene_valid && st.flips_since < 3) {
        // The next frame's pre-pass straddles the flip: the logical frame ends after the upscaler.
        st.flips_since++;
        return;
    }
    if (st.scene_draws && !st.aa_done) run();
    frame_boundary();
}

// Next logical frame: pick its scene from this frame's draws, set its jitter.
void frame_boundary()
{
    auto& st = g_st;
    st.boundary_pending = false;
    st.flips_since = 0;
    st.mat_done = false;
    st.run_scene = false;
    st.run_draws = st.run_dw = 0;

    // Next frame's scene: the depth buffer with the most depth-tested draws, then its busiest colour target.
    u32 best_ds = 0;
    {
        std::unordered_map<u32, unsigned> per_ds;
        unsigned best = 0;
        for (const auto& kv : st.tally) {
            if (!(kv.first >> 32)) continue;   // depth-only (shadow maps) can't be the scene
            const u32 ds = (u32)kv.first;
            const unsigned n = (per_ds[ds] += kv.second.count);
            if (n > best) { best = n; best_ds = ds; }
        }
    }
    unsigned max_count = 0;
    for (const auto& kv : st.tally)
        if ((u32)kv.first == best_ds && (kv.first >> 32)) max_count = max_count > kv.second.count ? max_count : kv.second.count;
    bool found = false;
    u32 new_rt = 0;
    unsigned latest_first = 0;
    const unsigned floor_n = max_count / 4 > 30u ? max_count / 4 : 30u;
    for (const auto& kv : st.tally) {
        if ((u32)kv.first != best_ds || !(kv.first >> 32)) continue;
        if (kv.second.count < floor_n) continue;
        if (!found || kv.second.first_draw > latest_first) {
            found = true;
            latest_first = kv.second.first_draw;
            new_rt = (u32)(kv.first >> 32);
        }
    }
    const bool was_valid = st.scene_valid;
    const u32 old_rt = st.scene_rt, old_ds = st.scene_ds;
    st.scene_valid = found;
    if (found) { st.scene_rt = new_rt; st.scene_ds = best_ds; }
    st.scene_changed = !was_valid || !found || old_rt != st.scene_rt || old_ds != st.scene_ds;
    if (found) {
        const auto it = st.tally.find(((u64)st.scene_rt << 32) | st.scene_ds);
        st.seg_target = it != st.tally.end() && it->second.segments ? it->second.segments : 1u;
    }
    if (st.scene_changed && found) {
        aalog("scene: colour 0x%08x, depth 0x%08x (%u draws, %u passes)", st.scene_rt, st.scene_ds, max_count,
              st.seg_target);
        st.need_reset = true;
    }
    if (g_cfg.debug && (st.frame % 300) == 0)
        aalog("frame %u: %u scene draws, %u matrix evals (%u failed), vote %u/%u", st.frame, st.scene_draws,
              st.evals, st.eval_fail, st.vote_hits, st.vote_total);

    st.prev_M.swap(st.cur_M);
    st.cur_M.clear();
    st.occurrences.clear();
    st.tally.clear();
    st.last_pair = ~0ull;
    st.seg_count = 0;
    st.prev_S = st.frame_S;
    st.prev_valid = st.frame_S.valid && st.ran_ok;
    // Jitter only while the upscaler is known to run on this scene: it ran last frame and the scene is unchanged.
    if (st.jitter_cooldown) st.jitter_cooldown--;
    const bool jitter_on = st.ran_ok && st.scene_valid && !st.scene_changed && !st.jitter_cooldown;
    st.ran_ok = false;
    st.scene_draws = 0;
    st.draw_index = 0;
    st.evals = st.eval_fail = 0;
    st.aa_done = false;
    st.cur_scene = false;
    st.color = st.depth = 0;
    st.frame_S.valid = false;
    st.frame++;
    const unsigned phase = (st.frame % g_cfg.phases) + 1;
    st.jx = jitter_on ? halton(phase, 2) - 0.5f : 0.f;
    st.jy = jitter_on ? halton(phase, 3) - 0.5f : 0.f;
    unsigned rw = 0, rh = 0;
    float ox = 0.f, oy = 0.f;
    if (jitter_on && g_cfg.fsr3 && st.w && rsx_vk_fsr3_render_size(&rw, &rh) &&
        rsx_vk_fsr3_jitter(st.frame, g_cfg.phases_set ? g_cfg.phases : 0u, &ox, &oy)) {
        // FSR's offset is in render pixels; the engine's jitter_px is in guest pixels (the backend scales
        // eval_jitter back by the same render scale), so the upscaler gets exactly FSR's value.
        const float ars = (float)rw / (float)st.w;
        st.jx = ox / ars;
        st.jy = oy / ars;
    }
}

const rsx_aa_hooks g_hooks = { nullptr, on_draw, on_transform, on_flip };

} // namespace

// Before rsx_vulkan_backend_init: RSX_FSR=2 (the launcher's DLAA choice) registers DLSS, RSX_FSR=3 FSR 3.1
// (falling back to FSR1 output scaling), RSX_FSR=1 selects FSR1 output scaling. 1 when an upscaler is
// registered.
extern "C" int r3_native_aa_register(void)
{
    const char* e = std::getenv("RSX_FSR");
    const int mode = e ? std::atoi(e) : 0;
    if (mode == 1) {
        aalog("FSR1 (RSX_FSR=1): spatial output scaling on the native renderer");
        rsx_vulkan_backend_force_output_fsr1();
        return 0;
    }
    if (mode != 2 && mode != 3) return 0;
    if (const char* u = std::getenv("RSX_UPSCALE"))
        if (std::atof(u) > 0.0 && std::atof(u) < 0.999)
            aalog("RSX_UPSCALE < 1 is not supported on the native renderer; %s at full resolution",
                  mode == 3 ? "FSR 3.1" : "DLAA");
    read_cfg();
    aalog("frame boundary: %s", g_cfg.split
              ? "R3 split frames (after the material pass, as R3's RPCS3 VKR2Upscaler; not yet verified in game "
                "on the native renderer; RSX_AA_SPLIT=0 = at the flip)"
              : "at the flip (RSX_AA_SPLIT=0, the R2 policy)");
    if (mode == 3) {
        g_cfg.fsr3 = true;
        if (!rsx_vk_fsr3_load(nullptr, dlss_log) || !rsx_vk_fsr3_ops()) {
            aalog("FSR 3.1 unavailable; FSR1 output scaling instead");
            rsx_vulkan_backend_force_output_fsr1();
            return 0;
        }
        float sharp = 0.f;
        if (const char* s = std::getenv("RSX_SHARPEN")) {
            sharp = (float)std::atof(s);
            if (sharp > 1.f) sharp *= 0.01f;   // a percentage
        }
        rsx_vk_fsr3_set_sharpness(sharp);
        rsx_vulkan_backend_set_temporal_upscaler(rsx_vk_fsr3_ops());
        rsx_draw_engine_set_aa_hooks(&g_hooks);
        aalog("FSR 3.1 native AA requested for the native renderer (sharpening %.2f, %s jitter phases)",
              (double)(sharp < 0.f ? 0.f : sharp > 1.f ? 1.f : sharp),
              g_cfg.phases_set ? std::to_string(g_cfg.phases).c_str() : "FSR's");
        return 1;
    }
    if (!dlss_load()) return 0;
    rsx_vulkan_backend_set_temporal_upscaler(&g_ops);
    rsx_draw_engine_set_aa_hooks(&g_hooks);
    aalog("DLAA requested for the native renderer (preset %u, %u jitter phases)", g_cfg.preset, g_cfg.phases);
    return 1;
}

#else
extern "C" int r3_native_aa_register(void) { return 0; }
#endif
