// Resistance 3 PC port: trophy list from the game's own trophy package (TROPHY.TRP), read at run
// time from the user's game files (PS3_GAME\TROPDIR\<comm id>_00\TROPHY.TRP); nothing of it ships
// with the port. Shared by the game (r3_trophies.cpp) and Launcher.exe (Trophies page).
//
// TRP: big-endian header { u32 magic 0xDCA24D00, u32 version, u64 file size, u32 count,
// u32 entry size, ... } (64 bytes), then `count` entries { char name[32], u64 offset, u64 size,
// ... }. TROP.SFM is plain XML (after a signature comment):
//   <trophy id="001" hidden="no" ttype="B" pid="000"><name>..</name><detail>..</detail></trophy>
// ttype P/G/S/B = platinum/gold/silver/bronze; pid = the platinum it counts towards (-1 none).
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace r2t {

struct Trophy {
    int id = -1;
    char grade = 'B';        // P G S B
    bool hidden = false;
    int pid = -1;
    std::string name, detail;  // UTF-8
    long long unlocked = 0;  // unix time, 0 = locked (filled from the save files)
};

inline uint64_t be64(const unsigned char* p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v; }
inline uint32_t be32(const unsigned char* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Bytes as UTF-8: kept if valid, else taken as Latin-1.
inline std::string to_utf8(const std::string& s)
{
    size_t i = 0; bool ok = true;
    while (i < s.size() && ok) {
        const unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || i + n >= s.size() + (n ? 0 : 1)) { ok = false; break; }
        for (int k = 1; k <= n; k++) if (((unsigned char)s[i + k] & 0xC0) != 0x80) ok = false;
        i += (size_t)n + 1;
    }
    if (ok) return s;
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x80) o += (char)c;
        else { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
    }
    return o;
}

inline std::string xml_unescape(std::string s)
{
    static const char* ent[][2] = { {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"} };
    for (auto& e : ent) {
        size_t p = 0;
        while ((p = s.find(e[0], p)) != std::string::npos) { s.replace(p, strlen(e[0]), e[1]); p += strlen(e[1]); }
    }
    return s;
}

inline std::string tag_text(const std::string& x, size_t from, size_t to, const char* tag)
{
    const std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
    const size_t a = x.find(open, from);
    if (a == std::string::npos || a > to) return "";
    const size_t b = x.find(close, a);
    if (b == std::string::npos || b > to) return "";
    return xml_unescape(to_utf8(x.substr(a + open.size(), b - a - open.size())));
}

inline std::string attr(const std::string& tag, const char* name)
{
    const std::string k = std::string(name) + "=\"";
    const size_t a = tag.find(k);
    if (a == std::string::npos) return "";
    const size_t b = tag.find('"', a + k.size());
    return b == std::string::npos ? "" : tag.substr(a + k.size(), b - a - k.size());
}

// Reads TROP.SFM (the default language) from a TROPHY.TRP. title gets the game's title.
inline bool load_trp(const char* path, std::vector<Trophy>& out, std::string* title = nullptr)
{
    out.clear();
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    std::vector<unsigned char> d;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 64 && n < 64 * 1024 * 1024) { d.resize((size_t)n); if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear(); }
    fclose(f);
    if (d.size() < 64 || be32(d.data()) != 0xDCA24D00u) return false;
    const uint32_t count = be32(&d[16]), esz = be32(&d[20]);
    if (esz < 48 || count > 1024 || 64 + (uint64_t)count * esz > d.size()) return false;
    std::string xml;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char* e = &d[64 + (size_t)i * esz];
        char name[33] = {}; memcpy(name, e, 32);
        const uint64_t off = be64(e + 32), sz = be64(e + 40);
        if (!strcmp(name, "TROP.SFM") && off + sz <= d.size()) { xml.assign((const char*)&d[off], (size_t)sz); break; }
    }
    if (xml.empty()) return false;
    if (title) *title = tag_text(xml, 0, xml.size(), "title-name");
    size_t p = 0;
    while ((p = xml.find("<trophy ", p)) != std::string::npos) {
        const size_t te = xml.find('>', p), end = xml.find("</trophy>", p);
        if (te == std::string::npos || end == std::string::npos) break;
        const std::string tag = xml.substr(p, te - p);
        Trophy t;
        t.id = atoi(attr(tag, "id").c_str());
        const std::string g = attr(tag, "ttype");
        t.grade = g.empty() ? 'B' : g[0];
        t.hidden = attr(tag, "hidden") == "yes";
        const std::string pid = attr(tag, "pid");
        t.pid = pid.empty() ? -1 : atoi(pid.c_str());
        t.name = tag_text(xml, te, end, "name");
        t.detail = tag_text(xml, te, end, "detail");
        out.push_back(t);
        p = end + 9;
    }
    return !out.empty();
}

inline const char* grade_name(char g)
{
    return g == 'P' ? "Platinum" : g == 'G' ? "Gold" : g == 'S' ? "Silver" : "Bronze";
}

// Unlock times from a json file holding {"id": N, ... "timestamp"|"unlocked": T} records
// (the runtime's gamedata\trophies\<comm id>.json and the port's Saved Games mirror).
inline void merge_json_times(const char* path, std::vector<Trophy>& list)
{
    FILE* f = fopen(path, "rb");
    if (!f) return;
    std::string s; char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    size_t p = 0;
    while ((p = s.find("\"id\"", p)) != std::string::npos) {
        const size_t c = s.find(':', p); if (c == std::string::npos) break;
        const int id = atoi(s.c_str() + c + 1);
        const size_t close = s.find('}', c);
        long long t = 0;
        for (const char* k : { "\"timestamp\"", "\"unlocked\"" }) {
            const size_t q = s.find(k, c);
            if (q != std::string::npos && q < close) { const size_t c2 = s.find(':', q); if (c2 != std::string::npos) t = atoll(s.c_str() + c2 + 1); }
        }
        for (auto& tr : list) if (tr.id == id && t > 0 && (!tr.unlocked || t < tr.unlocked)) tr.unlocked = t;
        p = c;
    }
}

inline std::string json_esc(const std::string& s)
{
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

// Saved Games mirror: {"game":..., "comm_id":..., "trophies":[{"id":N,"grade":"B","name":..,"unlocked":T},...]}
inline bool write_json(const char* path, const char* game, const char* comm, const std::vector<Trophy>& list)
{
    const std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "{\n  \"game\": \"%s\",\n  \"comm_id\": \"%s\",\n  \"trophies\": [\n", json_esc(game).c_str(), json_esc(comm).c_str());
    bool first = true;
    for (const auto& t : list) {
        if (!t.unlocked) continue;
        fprintf(f, "%s    { \"id\": %d, \"grade\": \"%c\", \"name\": \"%s\", \"unlocked\": %lld }", first ? "" : ",\n",
                t.id, t.grade, json_esc(t.name).c_str(), t.unlocked);
        first = false;
    }
    fprintf(f, "\n  ]\n}\n");
    fclose(f);
    remove(path);
    return rename(tmp.c_str(), path) == 0;
}

} // namespace r2t
