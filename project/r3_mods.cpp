// Resistance 3 PC port: mods folder (R2_MODS).
//
// R2_MODS=1 (launcher: Display > PC > Mods folder) reads the "mods" folder next to the
// executable (R2_MODS=<folder> names another one). Each sub-folder is one mod, laid out like
// the game's PS3_GAME folder:
//
//     mods\MyMod\USRDIR\<the game file to replace>
//
// A file there is opened in place of the game's own copy, by every file layer of the runtime
// (cellFs, sys_fs: the g_vfs_override_hook in runtime/syscalls/sys_fs.c). Mods are applied in
// alphabetical order of their folder names; when two replace the same file the later one wins.
// A folder whose name starts with '_' or '-' is skipped (disabled). The overrides are listed in
// the log at start-up.
//
// Single members of a .psarc archive: a FOLDER named like the archive holds the members to
// replace, by their path inside it, and single assets of a level .dat file by tuid
// (mods_psarc.h, docs/MODDING.md):
//
//     mods\MyMod\USRDIR\packed\levels\<lvl>\level_cached.psarc\built\levels\<lvl>\mobys.dat
//     mods\MyMod\USRDIR\packed\levels\<lvl>\level_cached.psarc\built\levels\<lvl>\shaders.dat.tuid\<tuid>.bin
//
// The game then opens a patched copy of the archive, built on first use and kept in
// mods\.cache\ until the archive or a mod file changes.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <mutex>
#include "mods_psarc.h"

extern "C" void (*volatile g_vfs_override_hook)(char* path, size_t cap);   // runtime/syscalls/sys_fs.c

namespace {

std::map<std::string, std::string> s_index;   // "usrdir/x/y.bin" (lower case, '/') -> host path
std::string s_root;

// Archives with member / tuid overrides: "usrdir/.../x.psarc" -> member path in it -> host path.
struct ArchivePlan {
    std::map<std::string, std::string> files;
    modpsarc::Plan plan;
    std::string built;   // patched copy, once made
    bool tried = false;
};
std::map<std::string, ArchivePlan> s_archives;
std::mutex s_archive_mtx;

std::string lower_slash(std::string s)
{
    for (auto& c : s) c = c == '\\' ? '/' : (char)tolower((unsigned char)c);
    return s;
}

void scan(const std::string& dir, const std::string& rel, std::map<std::string, std::string>& out)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        const std::string full = dir + "\\" + fd.cFileName;
        const std::string r = rel.empty() ? std::string(fd.cFileName) : rel + "/" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) scan(full, r, out);
        else out[lower_slash(r)] = full;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void build_index(const std::string& root)
{
    std::vector<std::string> mods;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == '.' || fd.cFileName[0] == '_' || fd.cFileName[0] == '-') continue;
            mods.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(mods.begin(), mods.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    for (const auto& m : mods) {
        std::map<std::string, std::string> files;
        scan(root + "\\" + m, "", files);
        fprintf(stderr, "[mods] %s: %zu file(s)\n", m.c_str(), files.size());
        for (auto& [k, v] : files) {
            const size_t a = k.find(".psarc/");
            if (a != std::string::npos) {
                auto& f = s_archives[k.substr(0, a + 6)].files;
                const std::string member = k.substr(a + 7);
                if (f.count(member)) fprintf(stderr, "[mods]   %s overrides the same archive member from an earlier mod\n", k.c_str());
                f[member] = v;
                continue;
            }
            if (s_index.count(k)) fprintf(stderr, "[mods]   %s overrides the same file from an earlier mod\n", k.c_str());
            s_index[k] = v;
        }
    }
    for (const auto& [k, v] : s_index) fprintf(stderr, "[mods] override: %s -> %s\n", k.c_str(), v.c_str());
    size_t members = 0;
    for (auto it = s_archives.begin(); it != s_archives.end();) {
        for (const auto& [m, f] : it->second.files) {
            std::string why;
            if (modpsarc::plan_add(it->second.plan, m, f, &why))
                fprintf(stderr, "[mods] archive override: %s : %s -> %s\n", it->first.c_str(), m.c_str(), f.c_str());
            else fprintf(stderr, "[mods] ignored %s/%s: %s\n", it->first.c_str(), m.c_str(), why.c_str());
        }
        members += it->second.plan.members.size() + it->second.plan.tuids.size();
        if (it->second.plan.empty()) it = s_archives.erase(it);
        else ++it;
    }
    fprintf(stderr, "[mods] %zu mod(s), %zu file override(s), %zu archive member/asset override(s) from %s\n",
            mods.size(), s_index.size(), members, root.c_str());
}

// The part of a resolved game path a mod replaces: from "PS3_GAME/" on (without it), else from "USRDIR/".
const char* game_rel(const char* p)
{
    for (const char* q = p; *q; q++) {
        if ((q == p || q[-1] == '/' || q[-1] == '\\') && !_strnicmp(q, "PS3_GAME", 8) && (q[8] == '/' || q[8] == '\\'))
            return q + 9;
    }
    for (const char* q = p; *q; q++) {
        if ((q == p || q[-1] == '/' || q[-1] == '\\') && !_strnicmp(q, "USRDIR", 6) && (q[6] == '/' || q[6] == '\\'))
            return q;
    }
    return nullptr;
}

// The patched copy of archive `key` (base: the resolved game path, or a whole-file override).
const std::string* archive_copy(const std::string& key, const std::string& base)
{
    const auto it = s_archives.find(key);
    if (it == s_archives.end()) return nullptr;
    std::lock_guard<std::mutex> lk(s_archive_mtx);
    ArchivePlan& ap = it->second;
    if (!ap.tried) {
        ap.tried = true;
        std::string name = key;
        for (auto& c : name) if (c == '/' || c == ':') c = '_';
        const std::string dir = s_root + "\\.cache";
        char hex[24];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)modpsarc::plan_key(base, ap.plan));
        const std::string out = dir + "\\" + name + "." + hex + ".psarc";
        if (GetFileAttributesA(out.c_str()) != INVALID_FILE_ATTRIBUTES) {
            fprintf(stderr, "[mods] %s: using the patched copy %s\n", key.c_str(), out.c_str());
            ap.built = out;
        } else {
            CreateDirectoryA(dir.c_str(), nullptr);
            // older copies of this archive
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA((dir + "\\" + name + ".*.psarc").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do DeleteFileA((dir + "\\" + fd.cFileName).c_str());
                while (FindNextFileA(h, &fd));
                FindClose(h);
            }
            std::string log;
            const DWORD t0 = GetTickCount();
            const bool ok = modpsarc::build(base, ap.plan, out, &log);
            fprintf(stderr, "[mods] %s: building the patched copy from %s\n%s", key.c_str(), base.c_str(), log.c_str());
            if (ok) {
                ap.built = out;
                fprintf(stderr, "[mods] %s: done in %lu ms\n", key.c_str(), (unsigned long)(GetTickCount() - t0));
            } else fprintf(stderr, "[mods] %s: failed, the archive is used unchanged\n", key.c_str());
        }
    }
    return ap.built.empty() ? nullptr : &ap.built;
}

void override_hook(char* path, size_t cap)
{
    if (s_index.empty() && s_archives.empty()) return;
    const char* rel = game_rel(path);
    if (!rel) return;
    std::string key = lower_slash(rel);
    // collapse "a//b" and "./"
    std::string k2; k2.reserve(key.size());
    for (size_t i = 0; i < key.size(); i++) {
        if (key[i] == '/' && !k2.empty() && k2.back() == '/') continue;
        if (key[i] == '.' && (i + 1 < key.size() && key[i + 1] == '/') && (k2.empty() || k2.back() == '/')) { i++; continue; }
        k2 += key[i];
    }
    const auto it = s_index.find(k2);
    const std::string* to = it == s_index.end() ? nullptr : &it->second;
    if (!s_archives.empty() && k2.size() > 6 && !k2.compare(k2.size() - 6, 6, ".psarc")) {
        if (const std::string* copy = archive_copy(k2, to ? *to : std::string(path))) to = copy;
    }
    if (!to || to->size() + 1 > cap) return;
    static std::mutex mtx;
    static std::map<std::string, int> said;
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (!said[k2]++) fprintf(stderr, "[mods] using %s\n", to->c_str());
    }
    snprintf(path, cap, "%s", to->c_str());
}

std::string exe_dir()
{
    char p[MAX_PATH] = "";
    GetModuleFileNameA(NULL, p, MAX_PATH);
    if (char* s = strrchr(p, '\\')) *s = 0;
    return p;
}

struct ModsInit {
    ModsInit()
    {
        const char* e = getenv("R2_MODS");
        if (!e || !*e || *e == '0' || !_stricmp(e, "off")) return;
        std::string root;
        if (!strcmp(e, "1") || !_stricmp(e, "on")) {
            // next to the exe, or its parent (build\Resistance2.exe with mods\ in the port folder)
            const std::string d = exe_dir();
            root = d + "\\mods";
            if (GetFileAttributesA(root.c_str()) == INVALID_FILE_ATTRIBUTES) root = d + "\\..\\mods";
            if (GetFileAttributesA(root.c_str()) == INVALID_FILE_ATTRIBUTES) root = "mods";
        } else root = e;
        if (GetFileAttributesA(root.c_str()) == INVALID_FILE_ATTRIBUTES) {
            fprintf(stderr, "[mods] R2_MODS=%s: folder %s not found, no mods\n", e, root.c_str());
            return;
        }
        s_root = root;
        build_index(root);
        if (!s_index.empty() || !s_archives.empty()) g_vfs_override_hook = override_hook;
    }
} s_mods_init;

} // namespace
