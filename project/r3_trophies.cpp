// Resistance 3 PC port: trophies as local achievements.
//
// The runtime's sceNpTrophy HLE already records unlocks (gamedata\trophies\<comm id>.json). This
// file hooks it (g_trophy_unlock_hook) to:
//   - name the trophy from the game's own TROPHY.TRP (read from the user's game files at run
//     time: gamefiles\PS3_GAME\TROPDIR\<comm id>_00\TROPHY.TRP),
//   - mirror the unlock list to %USERPROFILE%\Saved Games\Resistance 3\trophies.json,
//   - show a toast in the game window (R2_TROPHY_TOAST=0 turns it off),
//   - award the platinum once every trophy that counts towards it is unlocked (the HLE has no
//     grade data of its own).
// The launcher's Trophies page shows the same list.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>
#include "r3_trophy_data.h"
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

extern "C" int (*volatile g_trophy_unlock_hook)(const char* comm_id, int trophy_id, const unsigned char* unlocked, unsigned count);
extern "C" const char* ppu_vfs_root;
extern "C" void r2pf_toast(const char* title, const char* text, char badge);

namespace {

std::mutex s_mtx;
std::string s_loaded_comm;
std::vector<r2t::Trophy> s_list;
std::string s_title;
std::vector<int> s_saved;   // ids in the Saved Games record (announced before)

std::string saved_games_dir()
{
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &p)) && p) {
        char b[MAX_PATH * 3];
        if (WideCharToMultiByte(CP_UTF8, 0, p, -1, b, sizeof b, nullptr, nullptr) > 0) out = b;
    }
    if (p) CoTaskMemFree(p);
    if (out.empty()) { const char* u = getenv("USERPROFILE"); out = std::string(u ? u : ".") + "\\Saved Games"; }
    out += "\\Resistance 3";
    return out;
}

void load(const char* comm)
{
    if (s_loaded_comm == comm) return;
    s_loaded_comm = comm;
    const std::string root = ppu_vfs_root && *ppu_vfs_root ? ppu_vfs_root : "gamefiles";
    std::string trp = root + "/PS3_GAME/TROPDIR/" + comm + "_00/TROPHY.TRP";
    if (GetFileAttributesA(trp.c_str()) == INVALID_FILE_ATTRIBUTES)
        trp = std::string("gamefiles/PS3_GAME/TROPDIR/") + comm + "_00/TROPHY.TRP";
    if (r2t::load_trp(trp.c_str(), s_list, &s_title))
        fprintf(stderr, "[trophy] %zu trophies (%s) from %s\n", s_list.size(), s_title.c_str(), trp.c_str());
    else
        fprintf(stderr, "[trophy] %s not readable: unlocks are recorded without names\n", trp.c_str());
    const std::string sg = saved_games_dir() + "\\trophies.json";
    r2t::merge_json_times(sg.c_str(), s_list);
    for (auto& x : s_list) if (x.unlocked) s_saved.push_back(x.id);   // before the runtime's record, which already has this unlock
    // the runtime's own record (sceNpTrophy.c), for unlocks made before this mirror existed
    const char* gd = getenv("PS3_GAMEDATA_ROOT");
    const std::string hle = std::string(gd && *gd ? gd : "gamedata") + "/trophies/" + comm + ".json";
    r2t::merge_json_times(hle.c_str(), s_list);
}

int hook(const char* comm_id, int id, const unsigned char* unlocked, unsigned count)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    load(comm_id ? comm_id : "");
    const long long now = (long long)time(nullptr);
    r2t::Trophy* t = nullptr;
    bool earned = false;   // already in the Saved Games record (e.g. gamedata was reset): no second toast
    for (int sid : s_saved) if (sid == id) earned = true;
    if (!earned) s_saved.push_back(id);
    for (auto& x : s_list) {
        if (x.id >= 0 && (unsigned)x.id < count && unlocked[x.id] && !x.unlocked) x.unlocked = now;
        if (x.id == id) t = &x;
    }
    // platinum: every trophy with pid == its id unlocked
    int plat = -1;
    for (auto& p : s_list) {
        if (p.grade != 'P' || p.unlocked || p.id < 0 || (unsigned)p.id >= count || unlocked[p.id]) continue;
        bool all = true, any = false;
        for (auto& x : s_list)
            if (x.pid == p.id && x.id != p.id) { any = true; if (!x.unlocked) { all = false; break; } }
        if (any && all) { p.unlocked = now; plat = p.id; }
    }

    const std::string dir = saved_games_dir();
    SHCreateDirectoryExA(nullptr, dir.c_str(), nullptr);
    std::vector<r2t::Trophy> out = s_list;
    if (out.empty() || !t) {   // no trophy data: still record the id
        r2t::Trophy x; x.id = id; x.unlocked = now; x.name = "Trophy " + std::to_string(id);
        bool have = false;
        for (auto& o : out) if (o.id == id) have = true;
        if (!have) out.push_back(x);
    }
    r2t::write_json((dir + "\\trophies.json").c_str(), s_title.empty() ? "Resistance 3" : s_title.c_str(), comm_id ? comm_id : "", out);

    const char* e = getenv("R2_TROPHY_TOAST");
    const bool toast = !(e && *e == '0');
    auto show = [&](const r2t::Trophy* tr, int tid) {
        char head[96];
        snprintf(head, sizeof head, "%s trophy unlocked", tr ? r2t::grade_name(tr->grade) : "Trophy");
        std::string body = tr ? tr->name : "Trophy #" + std::to_string(tid);
        if (tr && !tr->detail.empty()) body += " \xE2\x80\x94 " + tr->detail;
        fprintf(stderr, "[trophy] unlocked #%d %s\n", tid, tr ? tr->name.c_str() : "");
        if (toast) r2pf_toast(head, body.c_str(), tr ? tr->grade : 'B');
    };
    if (earned) fprintf(stderr, "[trophy] #%d already earned (Saved Games record), not announced again\n", id);
    else show(t, id);
    if (plat >= 0) {
        const r2t::Trophy* p = nullptr;
        for (auto& x : s_list) if (x.id == plat) p = &x;
        show(p, plat);
    }
    return plat;
}

struct TrophyInit {
    TrophyInit() { g_trophy_unlock_hook = hook; }
} s_init;

} // namespace

