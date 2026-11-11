// Mods folder: per-member PSARC overrides and per-asset (tuid) overrides. See mods_psarc.h.
//
// PSARC (big-endian): 32-byte header {"PSAR", u16 major, u16 minor, "zlib", u32 tocLength,
// u32 entrySize (30), u32 entryCount, u32 blockSize, u32 flags}, then entryCount entries
// {md5[16], u32 firstBlock, u40 size, u40 offset}, then the block-size table up to tocLength
// (2 bytes per block when blockSize <= 64K; 0 = one full raw block). A block whose stored size
// equals its plain size is raw, any other is zlib. Entry 0 is the manifest (names, '\n').
//
// Rebuilt archive: same header and entries, the block table grows by the appended blocks,
// the old data region is copied as is (its offsets shift by the table growth), and changed
// members get new blocks after it.
#include "mods_psarc.h"
#include "miniz.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <system_error>

namespace fs = std::filesystem;

namespace modpsarc {
namespace {

constexpr uint32_t kFormat = 1;   // bump when the rebuilt archive layout changes (renames cached archives)

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t be40(const uint8_t* p) { return (uint64_t)p[0] << 32 | be32(p + 1); }
uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
void put40(uint8_t* p, uint64_t v) { p[0] = (uint8_t)(v >> 32); put32(p + 1, (uint32_t)v); }
uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

std::string lower(std::string s)
{
    for (auto& c : s) c = c == '\\' ? '/' : (char)tolower((unsigned char)c);
    return s;
}

std::string file_name(const std::string& p)
{
    const size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string dir_name(const std::string& p)
{
    const size_t s = p.rfind('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

std::string hex64(uint64_t v)
{
    char b[24];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

void say(std::string* log, const std::string& s)
{
    if (log) *log += s + "\n";
}

struct File {
    FILE* f = nullptr;
    explicit File(const std::string& p, const char* mode) { f = fopen(p.c_str(), mode[0] == 'w' ? "wb" : "rb"); }
    ~File() { if (f) fclose(f); }
    bool seek(uint64_t o) { return _fseeki64(f, (long long)o, SEEK_SET) == 0; }
    bool read(void* d, size_t n) { return n == 0 || fread(d, 1, n, f) == n; }
    bool write(const void* d, size_t n) { return n == 0 || fwrite(d, 1, n, f) == n; }
};

bool read_file(const std::string& p, std::vector<uint8_t>& out)
{
    File f(p, "r");
    if (!f.f) return false;
    _fseeki64(f.f, 0, SEEK_END);
    const long long n = _ftelli64(f.f);
    if (n < 0) return false;
    out.resize((size_t)n);
    return f.seek(0) && f.read(out.data(), out.size());
}

struct Entry { uint32_t block; uint64_t size, offset; };

struct Archive {
    File file;
    uint8_t header[32];
    uint32_t toc_len = 0, entry_size = 0, block_size = 0, width = 0;
    uint64_t file_size = 0;
    std::vector<uint8_t> entry_bytes;   // as stored
    std::vector<Entry> entries;
    std::vector<uint32_t> blocks;
    std::vector<std::string> names;     // names[i] belongs to entries[i + 1]; lower case, no leading '/'
    std::string error;

    explicit Archive(const std::string& path) : file(path, "r") {}

    bool open()
    {
        if (!file.f) return fail("cannot open");
        _fseeki64(file.f, 0, SEEK_END);
        file_size = (uint64_t)_ftelli64(file.f);
        if (!file.seek(0) || !file.read(header, 32) || memcmp(header, "PSAR", 4)) return fail("not a PSARC");
        if (memcmp(header + 8, "zlib", 4)) return fail("not a zlib PSARC");
        toc_len = be32(header + 12);
        entry_size = be32(header + 16);
        const uint32_t count = be32(header + 20);
        block_size = be32(header + 24);
        if (entry_size != 30 || !count || !block_size || toc_len > file_size || 32ull + (uint64_t)count * 30 > toc_len)
            return fail("unsupported PSARC header");
        width = block_size <= 0x10000 ? 2 : block_size <= 0x1000000 ? 3 : 4;
        std::vector<uint8_t> toc(toc_len - 32);
        if (!file.read(toc.data(), toc.size())) return fail("short table of contents");
        entry_bytes.assign(toc.begin(), toc.begin() + count * 30);
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t* e = &entry_bytes[i * 30];
            entries.push_back({ be32(e + 16), be40(e + 20), be40(e + 25) });
        }
        const size_t nb = (toc.size() - count * 30) / width;
        for (size_t i = 0; i < nb; i++) {
            const uint8_t* p = &toc[count * 30 + i * width];
            uint32_t v = 0;
            for (uint32_t k = 0; k < width; k++) v = v << 8 | p[k];
            blocks.push_back(v);
        }
        std::vector<uint8_t> manifest;
        if (!read_entry(0, manifest)) return false;
        std::string cur;
        for (uint8_t c : manifest) {
            if (c == '\n') { names.push_back(cur); cur.clear(); }
            else if (c != '\r') cur += (char)c;
        }
        if (!cur.empty() || names.size() < count - 1) names.push_back(cur);
        for (auto& n : names) {
            n = lower(n);
            while (!n.empty() && n[0] == '/') n.erase(0, 1);
        }
        names.resize(count - 1);
        return true;
    }

    bool fail(const char* why) { error = why; return false; }

    int find(const std::string& member) const
    {
        for (size_t i = 0; i < names.size(); i++)
            if (names[i] == member) return (int)i + 1;
        return -1;
    }

    bool read_entry(int index, std::vector<uint8_t>& out)
    {
        const Entry& e = entries[index];
        out.resize((size_t)e.size);
        if (!file.seek(e.offset)) return fail("seek failed");
        std::vector<uint8_t> buf;
        uint64_t done = 0;
        for (uint32_t b = e.block; done < e.size; b++) {
            if (b >= blocks.size()) return fail("block index out of range");
            const uint32_t chunk = (uint32_t)std::min<uint64_t>(block_size, e.size - done);
            const uint32_t stored = blocks[b] ? blocks[b] : block_size;
            buf.resize(stored);
            if (!file.read(buf.data(), stored)) return fail("short block");
            if (stored == chunk) memcpy(&out[(size_t)done], buf.data(), chunk);
            else {
                mz_ulong n = chunk;
                if (mz_uncompress(&out[(size_t)done], &n, buf.data(), stored) != MZ_OK || n != chunk) return fail("bad zlib block");
            }
            done += chunk;
        }
        return true;
    }
};

// IGHW section table: {id, offset, total bytes}.
struct Section { uint32_t id, offset, bytes; };

bool ighw_sections(const std::vector<uint8_t>& b, std::vector<Section>& out)
{
    if (b.size() < 0x20 || memcmp(b.data(), "IGHW", 4) || (b[4] == 0 && b[5] == 0)) return false;   // version 0 (R1) has another layout
    const uint32_t n = be32(&b[8]);
    if (0x20ull + n * 16ull > b.size()) return false;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* s = &b[0x20 + i * 16];
        const uint32_t count = be32(s + 8), len = be32(s + 12);
        const uint64_t bytes = (count & 0x10000000) ? (uint64_t)(count & 0x0FFFFFFF) * len : len;
        if (be32(s + 4) + bytes > b.size()) return false;
        out.push_back({ be32(s), be32(s + 4), (uint32_t)bytes });
    }
    return true;
}

void stamp(uint64_t& h, const void* d, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t*)d)[i]; h *= 0x100000001b3ull; }
}

void stamp_file(uint64_t& h, const std::string& p)
{
    std::error_code ec;
    const uint64_t size = (uint64_t)fs::file_size(fs::path(p), ec);
    const long long t = (long long)fs::last_write_time(fs::path(p), ec).time_since_epoch().count();
    stamp(h, p.data(), p.size() + 1);
    stamp(h, &size, sizeof size);
    stamp(h, &t, sizeof t);
}

// Updates every {u32 nameOffset, u32 size, u64 0} record of a level .toc that names `member`
// with size `old_size`. Returns the number of records changed.
int toc_set_size(std::vector<uint8_t>& toc, const std::string& member, uint64_t old_size, uint64_t new_size)
{
    int changed = 0;
    const std::string want = "/" + member;
    for (size_t i = 0; i + 16 <= toc.size(); i += 16) {
        const uint32_t name = be32(&toc[i]), size = be32(&toc[i + 4]);
        if (size != old_size || be64(&toc[i + 8]) != 0 || name >= toc.size() || name + want.size() >= toc.size()) continue;
        if (_strnicmp((const char*)&toc[name], want.c_str(), want.size()) || toc[name + want.size()] != 0) continue;
        put32(&toc[i + 4], (uint32_t)new_size);
        changed++;
    }
    return changed;
}

} // namespace

uint32_t asset_section_for(const std::string& dat_member)
{
    static const std::map<std::string, uint32_t> k = {
        { "shaders.dat", 0x1D100 }, { "textures.dat", 0x1D180 }, { "highmips.dat", 0x1D1C0 },
        { "cubemaps.dat", 0x1D200 }, { "ties.dat", 0x1D300 }, { "foliages.dat", 0x1D400 },
        { "shrubs.dat", 0x1D500 }, { "mobys.dat", 0x1D600 }, { "animsets.dat", 0x1D700 },
        { "cinematics.dat", 0x1D800 }, { "zones.dat", 0x1DA00 }, { "lighting.dat", 0x1DB00 },
        { "highlight.dat", 0x1DB80 },
    };
    const auto it = k.find(lower(file_name(dat_member)));
    return it == k.end() ? 0 : it->second;
}

long long find_asset(const std::vector<uint8_t>& al, uint32_t section, uint64_t tuid)
{
    std::vector<Section> secs;
    if (!ighw_sections(al, secs)) return -1;
    for (const auto& s : secs) {
        if (s.id != section) continue;
        for (uint32_t o = 0; o + 16 <= s.bytes; o += 16)
            if (be64(&al[s.offset + o]) == tuid) return (long long)s.offset + o;
    }
    return -1;
}

bool plan_add(Plan& plan, const std::string& rel_in, const std::string& host_file, std::string* why)
{
    std::string rel = lower(rel_in);
    while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    const size_t t = rel.find(".tuid/");
    if (t == std::string::npos) {
        if (rel.empty()) { if (why) *why = "empty member path"; return false; }
        plan.members.push_back({ rel, host_file });
        return true;
    }
    const std::string dat = rel.substr(0, t);
    std::string name = rel.substr(t + 6);
    if (name.find('/') != std::string::npos) { if (why) *why = "a .tuid folder holds files only"; return false; }
    if (!asset_section_for(dat)) { if (why) *why = file_name(dat) + " has no per-asset lookup"; return false; }
    name = name.substr(0, name.find('.'));
    if (name.size() > 2 && name[0] == '0' && name[1] == 'x') name.erase(0, 2);
    if (name.empty() || name.size() > 16 || name.find_first_not_of("0123456789abcdef") != std::string::npos) {
        if (why) *why = "file name is not a hex tuid";
        return false;
    }
    plan.tuids.push_back({ dat, std::stoull(name, nullptr, 16), host_file });
    return true;
}

uint64_t plan_key(const std::string& base, const Plan& plan)
{
    uint64_t h = 0xcbf29ce484222325ull;
    stamp(h, &kFormat, sizeof kFormat);
    stamp_file(h, base);
    for (const auto& m : plan.members) { stamp(h, m.member.data(), m.member.size() + 1); stamp_file(h, m.file); }
    for (const auto& t : plan.tuids) { stamp(h, t.dat.data(), t.dat.size() + 1); stamp(h, &t.tuid, sizeof t.tuid); stamp_file(h, t.file); }
    return h;
}

bool list_members(const std::string& archive, std::vector<std::string>& names)
{
    Archive a(archive);
    if (!a.open()) return false;
    names = a.names;
    return true;
}

bool read_member(const std::string& archive, const std::string& member, std::vector<uint8_t>& data)
{
    Archive a(archive);
    if (!a.open()) return false;
    std::string m = lower(member);
    while (!m.empty() && m[0] == '/') m.erase(0, 1);
    const int i = a.find(m);
    return i > 0 && a.read_entry(i, data);
}

bool build(const std::string& base, const Plan& plan, const std::string& out, std::string* log)
{
    Archive a(base);
    if (!a.open()) { say(log, base + ": " + a.error); return false; }

    std::map<int, std::vector<uint8_t>> changed;   // entry index -> new contents
    std::map<int, uint64_t> unread_hash;            // members read from the archive (not replaced): hash of the original
    auto contents = [&](int i) -> std::vector<uint8_t>* {
        auto it = changed.find(i);
        if (it != changed.end()) return &it->second;
        std::vector<uint8_t> d;
        if (!a.read_entry(i, d)) return nullptr;
        uint64_t h = 0xcbf29ce484222325ull;
        stamp(h, d.data(), d.size());
        unread_hash[i] = h;
        return &(changed[i] = std::move(d));
    };

    for (const auto& m : plan.members) {
        const int i = a.find(m.member);
        if (i < 0) { say(log, "  " + m.member + ": not in the archive (new members are not supported), skipped"); continue; }
        std::vector<uint8_t> d;
        if (!read_file(m.file, d)) { say(log, "  " + m.member + ": cannot read " + m.file); continue; }
        changed[i] = std::move(d);
        say(log, "  member " + m.member + " <- " + m.file);
    }

    for (const auto& t : plan.tuids) {
        const std::string what = t.dat + " tuid " + hex64(t.tuid);
        const int di = a.find(t.dat), ai = a.find(dir_name(t.dat) + "/assetlookup.dat");
        if (di < 0) { say(log, "  " + what + ": " + t.dat + " is not in this archive, skipped"); continue; }
        if (ai < 0) { say(log, "  " + what + ": no assetlookup.dat next to it, skipped"); continue; }
        std::vector<uint8_t> blob;
        if (!read_file(t.file, blob)) { say(log, "  " + what + ": cannot read " + t.file); continue; }
        std::vector<uint8_t>* al = contents(ai);
        std::vector<uint8_t>* dat = contents(di);
        if (!al || !dat) { say(log, "  " + what + ": " + a.error); return false; }
        const long long at = find_asset(*al, asset_section_for(t.dat), t.tuid);
        if (at < 0) { say(log, "  " + what + ": tuid not listed in assetlookup.dat, skipped"); continue; }
        const uint32_t off = be32(&(*al)[(size_t)at + 8]), len = be32(&(*al)[(size_t)at + 12]);
        if ((uint64_t)off + len > dat->size()) { say(log, "  " + what + ": lookup entry points outside the file, skipped"); continue; }
        if (blob.size() <= len) {
            memcpy(&(*dat)[off], blob.data(), blob.size());
            memset(&(*dat)[off + blob.size()], 0, len - blob.size());
            put32(&(*al)[(size_t)at + 12], (uint32_t)blob.size());
            say(log, "  " + what + " <- " + t.file + " (in place)");
        } else {
            const uint64_t at_end = align_up(dat->size(), 0x80);
            if (at_end + blob.size() > 0xFFFFFFFFull) { say(log, "  " + what + ": file would exceed 4 GB, skipped"); continue; }
            dat->resize((size_t)align_up(at_end + blob.size(), 0x80), 0);
            memcpy(&(*dat)[(size_t)at_end], blob.data(), blob.size());
            put32(&(*al)[(size_t)at + 8], (uint32_t)at_end);
            put32(&(*al)[(size_t)at + 12], (uint32_t)blob.size());
            say(log, "  " + what + " <- " + t.file + " (appended, +" + std::to_string(blob.size()) + " bytes)");
        }
    }

    // The level .toc members list the .dat sizes; keep them in step.
    std::vector<std::pair<int, uint64_t>> resized;
    for (const auto& [i, d] : changed)
        if (d.size() != a.entries[i].size) resized.push_back({ i, d.size() });
    if (!resized.empty()) {
        for (size_t ti = 0; ti < a.names.size(); ti++) {
            const std::string& n = a.names[ti];
            if (n.size() < 4 || n.compare(n.size() - 4, 4, ".toc")) continue;
            std::vector<uint8_t>* toc = contents((int)ti + 1);
            if (!toc) { say(log, "  " + n + ": " + a.error); return false; }
            for (const auto& [i, size] : resized)
                if (toc_set_size(*toc, a.names[i - 1], a.entries[i].size, size))
                    say(log, "  " + n + ": size of " + a.names[i - 1] + " updated");
        }
    }
    // members that were read for patching but ended up unchanged keep their original blocks
    for (auto it = changed.begin(); it != changed.end();) {
        const auto u = unread_hash.find(it->first);
        uint64_t h = 0xcbf29ce484222325ull;
        if (u != unread_hash.end()) stamp(h, it->second.data(), it->second.size());
        if (u != unread_hash.end() && it->second.size() == a.entries[it->first].size && h == u->second) it = changed.erase(it);
        else ++it;
    }
    if (changed.empty()) { say(log, "  nothing to change"); return false; }

    // Compress the changed members into new blocks.
    struct NewMember { int index; uint32_t first_block; uint64_t size, rel_offset; };
    std::vector<NewMember> added;
    std::vector<uint32_t> new_sizes;
    std::vector<uint8_t> new_data;
    std::vector<uint8_t> z;
    for (const auto& [i, d] : changed) {
        added.push_back({ i, (uint32_t)(a.blocks.size() + new_sizes.size()), d.size(), new_data.size() });
        for (size_t o = 0; o < d.size(); o += a.block_size) {
            const uint32_t chunk = (uint32_t)std::min<size_t>(a.block_size, d.size() - o);
            mz_ulong zn = mz_compressBound(chunk);
            z.resize(zn);
            if (mz_compress2(z.data(), &zn, &d[o], chunk, 6) == MZ_OK && zn < chunk) {
                new_data.insert(new_data.end(), z.begin(), z.begin() + zn);
                new_sizes.push_back((uint32_t)zn);
            } else {
                new_data.insert(new_data.end(), d.begin() + o, d.begin() + o + chunk);
                new_sizes.push_back(chunk == a.block_size ? 0 : chunk);
            }
        }
    }

    const uint64_t old_toc = a.toc_len;
    const uint64_t new_toc = 32 + a.entry_bytes.size() + (a.blocks.size() + new_sizes.size()) * (uint64_t)a.width;
    const uint64_t delta = new_toc - (32 + a.entry_bytes.size() + a.blocks.size() * (uint64_t)a.width);
    const uint64_t new_toc_len = old_toc + delta;
    const uint64_t data_end = a.file_size + delta;
    if (data_end + new_data.size() > 0xFFFFFFFFFFull) { say(log, "  archive too large"); return false; }

    std::vector<uint8_t> head(a.header, a.header + 32);
    put32(&head[12], (uint32_t)new_toc_len);
    std::vector<uint8_t> ents = a.entry_bytes;
    for (size_t i = 0; i < a.entries.size(); i++) put40(&ents[i * 30 + 25], a.entries[i].offset + delta);
    for (const auto& m : added) {
        uint8_t* e = &ents[m.index * 30];
        put32(e + 16, m.first_block);
        put40(e + 20, m.size);
        put40(e + 25, data_end + m.rel_offset);
    }
    std::vector<uint8_t> table;
    auto push_size = [&](uint32_t v) { for (int k = (int)a.width - 1; k >= 0; k--) table.push_back((uint8_t)(v >> (8 * k))); };
    for (uint32_t v : a.blocks) push_size(v);
    for (uint32_t v : new_sizes) push_size(v);
    // bytes between the old block table and the data (normally none) are kept
    std::vector<uint8_t> gap(old_toc - (32 + a.entry_bytes.size() + a.blocks.size() * (uint64_t)a.width));
    if (!gap.empty() && (!a.file.seek(old_toc - gap.size()) || !a.file.read(gap.data(), gap.size()))) { say(log, "  read failed"); return false; }

    const std::string tmp = out + ".tmp";
    std::error_code ec;
    fs::create_directories(fs::path(out).parent_path(), ec);
    {
        File o(tmp, "w");
        bool ok = o.f && o.write(head.data(), head.size()) && o.write(ents.data(), ents.size()) &&
                  o.write(table.data(), table.size()) && o.write(gap.data(), gap.size()) && a.file.seek(old_toc);
        std::vector<uint8_t> buf(1 << 20);
        for (uint64_t left = a.file_size - old_toc; ok && left;) {
            const size_t n = (size_t)std::min<uint64_t>(buf.size(), left);
            ok = a.file.read(buf.data(), n) && o.write(buf.data(), n);
            left -= n;
        }
        ok = ok && o.write(new_data.data(), new_data.size());
        if (o.f && fclose(o.f)) ok = false;
        o.f = nullptr;
        if (!ok) { fs::remove(fs::path(tmp), ec); say(log, "  cannot write " + tmp); return false; }
    }
    fs::rename(fs::path(tmp), fs::path(out), ec);
    if (ec) { fs::remove(fs::path(tmp), ec); say(log, "  cannot rename " + tmp); return false; }
    say(log, "  wrote " + out + " (" + std::to_string(changed.size()) + " member(s) changed)");
    return true;
}

} // namespace modpsarc
