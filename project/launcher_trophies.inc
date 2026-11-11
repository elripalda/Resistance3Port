// Launcher Trophies page (Resistance 2 / Resistance 3) and fan-made Achievements page (Resistance: Fall of Man).
// Shared, identical, by the three launchers; included from launcher.cpp after layout's helpers.
//
// Before including, define:
//   LT_R1 1            Fall of Man: achievements computed from PROFILE.SAV (rfom_achievements.h), or
//   LT_GAME "..."      Resistance 2 / 3: the folder name under Saved Games ("Resistance 3").
// and add D_TROPHY to the Deco kinds, painted by lt_paint().
//
// Read-only: every file is opened "rb" and nothing is ever written. Nothing is read until the page is
// shown (lt_layout -> lt_refresh); afterwards the files are re-read only when their size or time changes.
//
// R2 / R3 sources (the same the game uses, project/r*_trophies.cpp):
//   trophy list + icons  gamefiles\PS3_GAME\TROPDIR\<comm id>_00\TROPHY.TRP  (TROP.SFM, TROPnnn.PNG)
//   unlocks              (PS3_GAMEDATA_ROOT or gamedata)\trophies\<comm id>.json   (the runtime's record)
//                        Saved Games\<game>\trophies.json                          (the port's mirror)
//   merged: a trophy counts as earned if either has it; the earliest time wins.
#include <ctime>
#include <map>
#include <objidl.h>
#ifndef LT_R1
#define LT_R1 0
#endif

static const Color LT_GRADE_P(255, 196, 212, 236), LT_GRADE_G(255, 228, 186, 72), LT_GRADE_S(255, 188, 194, 204),
    LT_GRADE_B(255, 198, 130, 84);
static const Color& lt_grade_color(char g) { return g == 'P' ? LT_GRADE_P : g == 'G' ? LT_GRADE_G : g == 'S' ? LT_GRADE_S : LT_GRADE_B; }
static const char* lt_grade_name(char g) { return g == 'P' ? "Platinum" : g == 'G' ? "Gold" : g == 'S' ? "Silver" : "Bronze"; }
static int lt_grade_idx(char g) { return g == 'P' ? 0 : g == 'G' ? 1 : g == 'S' ? 2 : 3; }

struct LtRow {
    int id = 0;
    char grade = 'B';
    bool hidden = false, earned = false;
    long long when = 0;              // unix time, 0 = unknown
    int have = 0, need = 0;          // progress (R1)
    std::string name, detail;
};
static struct {
    bool loaded = false;             // the page was opened once
    DWORD checked = 0;
    std::string sig;                 // sizes + times of the source files at the last read
    std::vector<LtRow> rows;         // display order
    std::string status;              // shown instead of the list when there is nothing to list
    std::string source;              // where the data came from (the intro line)
    bool any_record = false;         // some unlock record / save exists
    int earned = 0, per[4][2] = {};  // [grade][earned, total]
    std::map<int, std::vector<unsigned char>> png;   // TROPnnn.PNG bytes by trophy id
    std::map<int, Bitmap*> icon[2];  // decoded on first paint: [0] locked (grey), [1] earned
} g_lt;

static std::string lt_stat(const std::string& p)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &a)) return "-|";
    char b[64];
    snprintf(b, sizeof b, "%lu:%lu:%lu|", a.nFileSizeLow, a.ftLastWriteTime.dwLowDateTime, a.ftLastWriteTime.dwHighDateTime);
    return b;
}
static bool lt_exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::string lt_saved_games()
{
    PWSTR sg = nullptr; char b[MAX_PATH * 3] = "";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &sg)) && sg)
        WideCharToMultiByte(CP_UTF8, 0, sg, -1, b, sizeof b, nullptr, nullptr);
    if (sg) CoTaskMemFree(sg);
    return b;
}
static void lt_free_icons()
{
    for (auto& m : g_lt.icon) { for (auto& e : m) delete e.second; m.clear(); }
}
static std::string lt_date(long long t, bool with_time)
{
    if (t <= 0) return "";
    time_t tt = (time_t)t; struct tm lt;
    if (localtime_s(&lt, &tt)) return "";
    char b[64]; strftime(b, sizeof b, with_time ? "%Y-%m-%d %H:%M" : "%Y-%m-%d", &lt);
    return b;
}

#if LT_R1
// ---- Fall of Man: fan-made achievements from PROFILE.SAV ----
static std::string lt_r1_save_root()
{
    const char* gd = getenv("PS3_GAMEDATA_ROOT");
    if (gd && *gd) return gd;
    if (!strcmp(val(S_SAVES), "portable") || lt_exists(path_of("portable.txt"))) return path_of("gamedata");
    const char* sel = getenv("RFOM_SAVE_DIR");
    if (sel && *sel && _stricmp(sel, "portable")) return sel;
    return lt_saved_games() + "\\Resistance Fall of Man";
}
static std::string lt_r1_profile()
{
    const char* rel = "\\dev_hdd0\\home\\00000001\\savedata\\BCUS98107_PROFILE\\PROFILE.SAV";
    const std::string p = lt_r1_save_root() + rel;
    if (lt_exists(p)) return p;
    const std::string old = path_of("gamedata") + rel;     // not copied to Saved Games yet (the game does it on start)
    return lt_exists(old) ? old : p;
}
static void lt_load()
{
    const std::string path = lt_r1_profile();
    g_lt.rows.clear(); g_lt.status.clear(); g_lt.any_record = false;
    rfach::Profile prof;
    const bool ok = rfach::load(path.c_str(), prof);
    g_lt.any_record = lt_exists(path);
    if (g_lt.any_record && !ok) g_lt.status = prof.error;
    long long mtime = 0;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a)) {
        ULARGE_INTEGER u; u.LowPart = a.ftLastWriteTime.dwLowDateTime; u.HighPart = a.ftLastWriteTime.dwHighDateTime;
        mtime = (long long)(u.QuadPart / 10000000ULL) - 11644473600LL;
    }
    g_lt.source = g_lt.any_record ? "From your save, PROFILE.SAV (last saved " + lt_date(mtime, true) + ")."
                                  : "Computed from your save (PROFILE.SAV) once the game has made one.";
    int i = 0;
    for (const auto& x : rfach::evaluate(prof)) {
        LtRow r; r.id = i++; r.grade = x.grade; r.earned = ok && x.earned; r.have = ok ? x.have : 0; r.need = x.need;
        r.name = x.name; r.detail = x.detail;
        g_lt.rows.push_back(r);
    }
}
static std::string lt_signature() { return lt_stat(lt_r1_profile()) + val(S_SAVES); }
#else
// ---- Resistance 2 / 3: the game's trophies ----
static std::string lt_comm_dir()
{
    WIN32_FIND_DATAA fd; std::string comm;
    HANDLE h = FindFirstFileA(path_of("gamefiles\\PS3_GAME\\TROPDIR\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') { comm = fd.cFileName; break; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return comm;
}
static void lt_paths(std::string& trp, std::string& hle, std::string& mirror)
{
    std::string comm = lt_comm_dir();
    trp = comm.empty() ? "" : path_of(("gamefiles\\PS3_GAME\\TROPDIR\\" + comm + "\\TROPHY.TRP").c_str());
    if (comm.size() > 3 && comm.compare(comm.size() - 3, 3, "_00") == 0) comm.resize(comm.size() - 3);
    const char* gd = getenv("PS3_GAMEDATA_ROOT");
    hle = comm.empty() ? "" : (gd && *gd ? std::string(gd) : path_of("gamedata")) + "\\trophies\\" + comm + ".json";
    const std::string sg = lt_saved_games();
    mirror = sg.empty() ? "" : sg + "\\" LT_GAME "\\trophies.json";
}
// The TROPnnn.PNG icons of a TROPHY.TRP (the entry table r2t::load_trp reads), by trophy id.
static void lt_read_icons(const std::string& trp)
{
    g_lt.png.clear();
    FILE* f = fopen(trp.c_str(), "rb");
    if (!f) return;
    std::vector<unsigned char> d;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 64 && n < 64 * 1024 * 1024) { d.resize((size_t)n); if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear(); }
    fclose(f);
    if (d.size() < 64 || r2t::be32(d.data()) != 0xDCA24D00u) return;
    const uint32_t count = r2t::be32(&d[16]), esz = r2t::be32(&d[20]);
    if (esz < 48 || count > 1024 || 64 + (uint64_t)count * esz > d.size()) return;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char* e = &d[64 + (size_t)i * esz];
        char name[33] = {}; memcpy(name, e, 32);
        const uint64_t off = r2t::be64(e + 32), sz = r2t::be64(e + 40);
        int id = -1;
        if (strlen(name) == 11 && !_strnicmp(name, "TROP", 4) && !_stricmp(name + 7, ".PNG") && sscanf(name + 4, "%3d", &id) == 1 &&
            id >= 0 && off + sz <= d.size() && sz > 8 && sz < 4 * 1024 * 1024)
            g_lt.png[id].assign(d.begin() + (size_t)off, d.begin() + (size_t)(off + sz));
    }
}
static std::string lt_trp_cached;    // the TRP whose icons are in g_lt.png
static void lt_load()
{
    std::string trp, hle, mirror; lt_paths(trp, hle, mirror);
    g_lt.rows.clear(); g_lt.status.clear();
    g_lt.any_record = (!hle.empty() && lt_exists(hle)) || (!mirror.empty() && lt_exists(mirror));
    g_lt.source = "Earned in game and kept on this PC (Saved Games\\" LT_GAME "\\trophies.json).";
    std::vector<r2t::Trophy> list;
    if (trp.empty() || !r2t::load_trp(trp.c_str(), list)) {
        g_lt.status = "The trophy list is read from the game's files (gamefiles\\PS3_GAME\\TROPDIR) after Setup.";
        return;
    }
    if (lt_trp_cached != trp + lt_stat(trp)) { lt_free_icons(); lt_read_icons(trp); lt_trp_cached = trp + lt_stat(trp); }
    if (!hle.empty()) r2t::merge_json_times(hle.c_str(), list);
    if (!mirror.empty()) r2t::merge_json_times(mirror.c_str(), list);
    for (const auto& t : list) {
        LtRow r; r.id = t.id; r.grade = t.grade; r.hidden = t.hidden; r.earned = t.unlocked != 0; r.when = t.unlocked;
        r.name = t.name; r.detail = t.detail;
        g_lt.rows.push_back(r);
    }
    std::stable_sort(g_lt.rows.begin(), g_lt.rows.end(), [](const LtRow& a, const LtRow& b) {
        if (a.earned != b.earned) return a.earned;
        return a.earned ? a.when > b.when : false;      // newest first, the rest in the game's order
    });
}
static std::string lt_signature()
{
    std::string trp, hle, mirror; lt_paths(trp, hle, mirror);
    return trp + lt_stat(trp) + lt_stat(hle) + lt_stat(mirror);
}
#endif

static void lt_refresh()
{
    if (g_lt.loaded && GetTickCount() - g_lt.checked < 2000) return;
    g_lt.checked = GetTickCount();
    const std::string sig = lt_signature();
    if (g_lt.loaded && sig == g_lt.sig) return;
    g_lt.loaded = true; g_lt.sig = sig;
    lt_load();
    g_lt.earned = 0; memset(g_lt.per, 0, sizeof g_lt.per);
    for (const auto& r : g_lt.rows) {
        const int gi = lt_grade_idx(r.grade);
        g_lt.per[gi][1]++;
        if (r.earned) { g_lt.per[gi][0]++; g_lt.earned++; }
    }
}

#if LT_R1
#define LT_NOUN "achievement"
#else
#define LT_NOUN "trophy"
#endif

static void lt_layout(float x, float& y, float w)
{
    lt_refresh();
    deco(D_H1, RectF(x, y, w, SF(34)), LT_R1 ? "Achievements" : "Trophies"); y += SF(36);
    deco(D_INTRO, RectF(x, y, w, SF(20)), g_lt.source); y += SF(30);
#if LT_R1
    deco(D_TROPHY, RectF(x, y, w, SF(24)), "U"); y += SF(34);
#else
    y += SF(10);
#endif
    if (g_lt.rows.empty()) {
        deco(D_PANEL, RectF(x - SF(12), y, w + SF(24), SF(64)), "");
        deco(D_NOTE, RectF(x, y + SF(14), w, SF(40)), g_lt.status);
        y += SF(64);
        return;
    }
    const float ph = SF(126);
    deco(D_PANEL, RectF(x - SF(12), y, w + SF(24), ph), "");
    deco(D_TROPHY, RectF(x, y, w, ph), "S");
    y += ph + SF(22);
    if (!g_lt.status.empty()) { deco(D_NOTE, RectF(x, y, w, SF(36)), g_lt.status); y += SF(40); }
    const int n = (int)g_lt.rows.size();
    for (int pass = 0; pass < 2; pass++) {
        const int cnt = pass == 0 ? g_lt.earned : n - g_lt.earned;
        if (!cnt) continue;
        char h[64]; snprintf(h, sizeof h, pass == 0 ? "EARNED  (%d)" : "NOT EARNED YET  (%d)", cnt);
        deco(D_H2, RectF(x, y, w, SF(22)), h); y += SF(30);
        for (int i = 0; i < n; i++) {
            const LtRow& t = g_lt.rows[i];
            if (t.earned != (pass == 0)) continue;
            const bool secret = t.hidden && !t.earned;
            const float rh = SF(66);
            Widget& row = add(W_ROW, RectF(x - SF(12), y, w + SF(24), rh), -200 - i);
            row.title = secret ? (LT_R1 ? "Hidden achievement" : "Hidden trophy") : t.name;
            row.help = std::string(lt_grade_name(t.grade)) + ". " +
                       (secret ? "Its details show once you earn it." : t.detail) +
                       (t.earned && t.when ? "  Earned " + lt_date(t.when, true) + "." : "");
            char k[16];
            snprintf(k, sizeof k, "I%d", i); deco(D_TROPHY, RectF(x, y + SF(8), SF(50), SF(50)), k);
            const float tx = x + SF(64), rw = SF(190);
            deco(t.earned ? D_LABEL : D_LABEL_DIM, RectF(tx, y + SF(6), w - SF(64) - rw - SF(8), SF(28)),
                 secret ? (LT_R1 ? "Hidden achievement" : "Hidden trophy") : t.name);
            deco(D_NOTE, RectF(tx, y + SF(34), w - SF(64) - rw - SF(8), SF(30)),
                 secret ? "Its details show once you earn it." : t.detail);
            snprintf(k, sizeof k, "G%d", i); deco(D_TROPHY, RectF(x + w - rw, y + SF(8), rw, SF(50)), k);
            y += rh + SF(4);
        }
        y += SF(14);
    }
}

// ---- painting ----
static Bitmap* lt_icon(const LtRow& t, int px)
{
    auto& cache = g_lt.icon[t.earned ? 1 : 0];
    auto it = cache.find(t.id);
    if (it != cache.end()) return it->second;
    Bitmap* out = nullptr;
    auto p = g_lt.png.find(t.id);
    if (p != g_lt.png.end() && !(t.hidden && !t.earned)) {
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, p->second.size());
        IStream* st = nullptr;
        if (hg) {
            memcpy(GlobalLock(hg), p->second.data(), p->second.size()); GlobalUnlock(hg);
            if (FAILED(CreateStreamOnHGlobal(hg, TRUE, &st))) { GlobalFree(hg); st = nullptr; }
        }
        if (st) {
            Bitmap* src = Bitmap::FromStream(st);
            if (src && src->GetLastStatus() == Ok) {
                out = new Bitmap(px, px, PixelFormat32bppPARGB);
                Graphics g(out);
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(PixelOffsetModeHalf);
                ImageAttributes ia;
                if (!t.earned) {            // locked: grey and faded
                    ColorMatrix m = { { { .3f, .3f, .3f, 0, 0 }, { .59f, .59f, .59f, 0, 0 }, { .11f, .11f, .11f, 0, 0 },
                                        { 0, 0, 0, .45f, 0 }, { 0, 0, 0, 0, 1 } } };
                    ia.SetColorMatrix(&m);
                }
                g.DrawImage(src, RectF(0, 0, (REAL)px, (REAL)px), 0, 0, (REAL)src->GetWidth(), (REAL)src->GetHeight(), UnitPixel, &ia);
            }
            delete src;
            st->Release();
        }
    }
    cache[t.id] = out;
    return out;
}

static void lt_badge(Graphics& g, RectF r, char grade, bool earned, const wchar_t* glyph)
{
    Color c = lt_grade_color(grade);
    if (!earned) c = Color(255, 70, 74, 80);
    fill_rr(g, r, r.Width * 0.22f, Color(255, (BYTE)(c.GetR() / 4), (BYTE)(c.GetG() / 4), (BYTE)(c.GetB() / 4)));
    stroke_rr(g, r, r.Width * 0.22f, c, SF(2));
    text(g, glyph, f_h1, c, r, AL_C);
}

static void lt_paint(Graphics& g, const std::string& s, RectF r)
{
    if (s.empty()) return;
    if (s[0] == 'U') {           // R1: the unofficial label
        fill_rr(g, RectF(r.X, r.Y, SF(132), r.Height), SF(5), C_WARN);
        text(g, L"UNOFFICIAL \x00B7 FAN-MADE", f_tiny, C_BG, RectF(r.X, r.Y, SF(132), r.Height), AL_C);
        text(g, L"Not in the original game, which has no trophies. Shown only here.",
             f_small, C_DIM, RectF(r.X + SF(144), r.Y, r.Width - SF(144), r.Height));
        return;
    }
    if (s[0] == 'S') {           // summary: "23 / 51", percentage, bar, per-grade counts
        const int n = (int)g_lt.rows.size(), e = g_lt.earned, pct = n ? e * 100 / n : 0;
        wchar_t b[96];
        swprintf(b, 96, L"%d / %d", e, n);
        text(g, b, f_h1, C_TEXT, RectF(r.X, r.Y + SF(14), SF(200), SF(36)));
        swprintf(b, 96, L"%d%%", pct);
        text(g, b, f_h1, C_ACCENT, RectF(r.X + r.Width - SF(120), r.Y + SF(14), SF(120), SF(36)), AL_R);
        if (!g_lt.any_record || !e)
            text(g, W(LT_R1 ? "Play the game to start earning achievements." : "Play the game to start earning trophies."),
                 f_small, C_DIM, RectF(r.X + SF(130), r.Y + SF(14), r.Width - SF(260), SF(36)), AL_C);
        const RectF bar(r.X, r.Y + SF(58), r.Width, SF(8));
        fill_rr(g, bar, SF(4), C_CTL);
        if (n && e) fill_rr(g, RectF(bar.X, bar.Y, std::max(bar.Width * e / n, SF(8)), bar.Height), SF(4), C_ACCENT);
        float gx = r.X;
        for (int gi = 0; gi < 4; gi++) {
            const char gr = "PGSB"[gi];
            if (!g_lt.per[gi][1]) continue;
            const RectF dot(gx, r.Y + SF(86), SF(14), SF(14));
            SolidBrush db(lt_grade_color(gr)); g.FillEllipse(&db, dot);
            swprintf(b, 96, L"%hs  %d / %d", lt_grade_name(gr), g_lt.per[gi][0], g_lt.per[gi][1]);
            text(g, b, f_small, C_DIM, RectF(gx + SF(20), r.Y + SF(80), SF(150), SF(26)));
            gx += SF(160);
        }
        return;
    }
    const int i = atoi(s.c_str() + 1);
    if (i < 0 || i >= (int)g_lt.rows.size()) return;
    const LtRow& t = g_lt.rows[i];
    if (s[0] == 'I') {           // icon (the game's trophy art), else a grade badge
        const bool secret = t.hidden && !t.earned;
        Bitmap* bm = lt_icon(t, (int)r.Width);
        if (bm) {
            g.DrawImage(bm, r);
            if (t.earned) stroke_rr(g, r, SF(6), lt_grade_color(t.grade), SF(1.5));
        } else lt_badge(g, r, t.grade, t.earned, secret ? L"?" : t.grade == 'P' ? L"P" : t.grade == 'G' ? L"G" : t.grade == 'S' ? L"S" : L"B");
        return;
    }
    if (s[0] == 'G') {           // right column: grade pill, then the date or the progress
        const RectF pill(r.X + r.Width - SF(84), r.Y + SF(2), SF(84), SF(22));
        const Color& c = t.earned ? lt_grade_color(t.grade) : C_FAINT;
        stroke_rr(g, pill, SF(11), c, SF(1.2));
        text(g, W(lt_grade_name(t.grade)), f_tiny, c, pill, AL_C);
        std::wstring w2;
        if (t.earned) w2 = t.when ? W("Earned " + lt_date(t.when, false)) : L"Earned";
        else if (t.need > 1) {
            wchar_t b[48]; swprintf(b, 48, L"%d / %d", t.have, t.need); w2 = b;
            const RectF bar(r.X + r.Width - SF(84), r.Y + SF(38), SF(84), SF(5));
            fill_rr(g, bar, SF(2.5), C_CTL);
            if (t.have > 0) fill_rr(g, RectF(bar.X, bar.Y, bar.Width * t.have / t.need, bar.Height), SF(2.5), C_ACCENT_DIM);
            text(g, w2, f_small, C_FAINT, RectF(r.X, r.Y + SF(26), r.Width - SF(92), SF(20)), AL_R);
            return;
        } else w2 = L"Not earned";
        text(g, w2, f_small, t.earned ? C_DIM : C_FAINT, RectF(r.X, r.Y + SF(26), r.Width, SF(20)), AL_R);
    }
}
