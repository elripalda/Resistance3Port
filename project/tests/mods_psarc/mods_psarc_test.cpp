// Offline tests for the mods folder's archive overrides (mods_psarc.cpp, r2_mods.cpp / r3_mods.cpp).
//
//   mods_psarc_test unit            synthetic PSARC: member and tuid overrides, lookup, read back
//   mods_psarc_test setup <dir>     writes a fake game + mods folder for the hook tests
//   mods_psarc_test hook <dir>      R2_MODS=<dir>/mods: the file hook redirects to a patched copy
//   mods_psarc_test rebuild <dir>   a changed mod file rebuilds the copy and drops the old one
//   mods_psarc_test nohook          R2_MODS unset: no hook installed (game files untouched)
//   mods_psarc_test real <archive>  real level_cached.psarc: tuid override round trip (77 = skipped)
#include "mods_psarc.h"
#include "miniz.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern "C" void (*volatile g_vfs_override_hook)(char* path, size_t cap) = nullptr;   // sys_fs.c in the game

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static Bytes pattern(size_t n, uint32_t seed, bool compressible)
{
    Bytes b(n);
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        b[i] = compressible ? (uint8_t)((i / 64) + seed) : (uint8_t)(x >> 24);
    }
    return b;
}

static void write_file(const fs::path& p, const Bytes& b)
{
    fs::create_directories(p.parent_path());
    FILE* f = fopen(p.string().c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", p.string().c_str()); g_fail++; return; }
    if (b.size()) fwrite(b.data(), 1, b.size(), f);
    fclose(f);
}

static Bytes read_file(const fs::path& p)
{
    Bytes b;
    FILE* f = fopen(p.string().c_str(), "rb");
    if (!f) return b;
    fseek(f, 0, SEEK_END);
    b.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    if (b.size() && fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
    fclose(f);
    return b;
}

// Minimal PSARC writer like the game's: 64K blocks, zlib where it helps, raw otherwise.
static Bytes make_psarc(const std::vector<std::pair<std::string, Bytes>>& files)
{
    const uint32_t bs = 0x10000;
    std::vector<Bytes> members;
    std::string manifest;
    for (size_t i = 0; i < files.size(); i++) manifest += (i ? "\n" : "") + files[i].first;
    members.push_back(Bytes(manifest.begin(), manifest.end()));
    for (const auto& f : files) members.push_back(f.second);
    std::vector<uint32_t> sizes;
    Bytes data;
    struct E { uint32_t block; uint64_t size, off; };
    std::vector<E> ents;
    for (const auto& m : members) {
        ents.push_back({ (uint32_t)sizes.size(), m.size(), data.size() });
        for (size_t o = 0; o < m.size(); o += bs) {
            const uint32_t chunk = (uint32_t)std::min<size_t>(bs, m.size() - o);
            mz_ulong zn = mz_compressBound(chunk);
            Bytes z(zn);
            if (mz_compress2(z.data(), &zn, &m[o], chunk, 9) == MZ_OK && zn < chunk) {
                data.insert(data.end(), z.begin(), z.begin() + zn);
                sizes.push_back((uint32_t)zn);
            } else {
                data.insert(data.end(), m.begin() + o, m.begin() + o + chunk);
                sizes.push_back(chunk == bs ? 0 : chunk);
            }
        }
    }
    const uint32_t toc = 32 + (uint32_t)members.size() * 30 + (uint32_t)sizes.size() * 2;
    Bytes out(toc);
    memcpy(&out[0], "PSAR", 4);
    out[5] = 1; out[7] = 3;
    memcpy(&out[8], "zlib", 4);
    put32(&out[12], toc);
    put32(&out[16], 30);
    put32(&out[20], (uint32_t)members.size());
    put32(&out[24], bs);
    put32(&out[28], 3);
    for (size_t i = 0; i < ents.size(); i++) {
        uint8_t* e = &out[32 + i * 30];
        e[0] = (uint8_t)i;   // md5: not checked by the reader
        put32(e + 16, ents[i].block);
        e[20] = (uint8_t)(ents[i].size >> 32); put32(e + 21, (uint32_t)ents[i].size);
        const uint64_t off = ents[i].off + toc;
        e[25] = (uint8_t)(off >> 32); put32(e + 26, (uint32_t)off);
    }
    for (size_t i = 0; i < sizes.size(); i++) {
        out[32 + ents.size() * 30 + i * 2] = (uint8_t)(sizes[i] >> 8);
        out[33 + ents.size() * 30 + i * 2] = (uint8_t)sizes[i];
    }
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// IGHW v1.1 with pointer sections {id, entries {tuid, offset, length}}.
struct Ptr { uint64_t tuid; uint32_t off, len; };
static Bytes make_assetlookup(const std::vector<std::pair<uint32_t, std::vector<Ptr>>>& secs)
{
    Bytes b(0x20 + secs.size() * 16);
    memcpy(&b[0], "IGHW", 4);
    b[5] = 1; b[7] = 1;
    put32(&b[8], (uint32_t)secs.size());
    for (size_t s = 0; s < secs.size(); s++) {
        while (b.size() % 16) b.push_back(0);
        const uint32_t at = (uint32_t)b.size();
        for (const auto& p : secs[s].second) {
            b.resize(b.size() + 16);
            put64(&b[b.size() - 16], p.tuid); put32(&b[b.size() - 8], p.off); put32(&b[b.size() - 4], p.len);
        }
        uint8_t* h = &b[0x20 + s * 16];
        put32(h, secs[s].first); put32(h + 4, at); put32(h + 8, 1); put32(h + 12, (uint32_t)secs[s].second.size() * 16);
    }
    return b;
}

// level .toc: {nameOffset, size, 0, 0} records, other data, then the names.
static Bytes make_toc(const std::vector<std::pair<std::string, uint32_t>>& recs)
{
    Bytes b(recs.size() * 16 + 0x40, 0);
    for (size_t i = 0; i < recs.size(); i++) {
        put32(&b[i * 16], (uint32_t)b.size());
        put32(&b[i * 16 + 4], recs[i].second);
        b.insert(b.end(), recs[i].first.begin(), recs[i].first.end());
        b.push_back(0);
    }
    return b;
}

static uint32_t toc_size(const Bytes& toc, const std::string& name)
{
    for (size_t i = 0; i + 16 <= toc.size(); i += 16) {
        const uint32_t n = be32(&toc[i]);
        if (n + name.size() < toc.size() && !_stricmp((const char*)&toc[n], name.c_str())) return be32(&toc[i + 4]);
    }
    return 0xFFFFFFFF;
}

static Bytes member(const fs::path& a, const std::string& m)
{
    Bytes d;
    CHECK(modpsarc::read_member(a.string(), m, d));
    return d;
}

static const uint64_t TA = 0x0123456789ABCDEFull, TB = 0x1111222233334444ull, TC = 0xFEDCBA9876543210ull, TX = 0x5555666677778888ull;

// A level archive: shaders.dat 150016 bytes (3 blocks) with 3 assets, textures.dat, a .toc, assetlookup.
static Bytes level_archive(Bytes* shaders_out = nullptr)
{
    Bytes shaders = pattern(150016, 1, true);
    if (shaders_out) *shaders_out = shaders;
    Bytes al = make_assetlookup({ { 0x1D100, { { TA, 0, 0x100 }, { TB, 0x100, 0x1000 }, { TC, 0x20000, 0x400 } } },
                                  { 0x1D101, { { 0, 0, 0 } } },
                                  { 0x1D180, { { TX, 0, 1000 } } } });
    Bytes toc = make_toc({ { "/built/levels/t/shaders.dat", (uint32_t)shaders.size() }, { "/built/levels/t/textures.dat", 1000 } });
    return make_psarc({ { "/packed/levels/t/level_cached.toc", toc }, { "/built/levels/t/shaders.dat", shaders },
                        { "/built/levels/t/textures.dat", pattern(1000, 2, false) }, { "/built/levels/t/assetlookup.dat", al },
                        { "/other/readme.txt", Bytes(5, 'x') }, { "/big/raw.bin", pattern(200000, 3, false) } });
}

static int unit()
{
    const fs::path dir = fs::absolute("unit");
    fs::remove_all(dir);
    Bytes shaders;
    const fs::path base = dir / "level_cached.psarc";
    write_file(base, level_archive(&shaders));

    // reader
    std::vector<std::string> names;
    CHECK(modpsarc::list_members(base.string(), names));
    CHECK(names.size() == 6 && names[1] == "built/levels/t/shaders.dat");
    CHECK(member(base, "/built/levels/t/shaders.dat") == shaders);
    CHECK(member(base, "BUILT/levels/t/textures.dat") == pattern(1000, 2, false));
    CHECK(member(base, "big/raw.bin") == pattern(200000, 3, false));

    // lookup
    const Bytes al = member(base, "built/levels/t/assetlookup.dat");
    CHECK(modpsarc::asset_section_for("built/levels/t/shaders.dat") == 0x1D100);
    CHECK(modpsarc::asset_section_for("built/levels/t/mobys.dat") == 0x1D600);
    CHECK(modpsarc::asset_section_for("built/levels/t/gameplay.dat") == 0);
    CHECK(modpsarc::find_asset(al, 0x1D100, TB) >= 0);
    CHECK(modpsarc::find_asset(al, 0x1D180, TB) < 0);
    CHECK(modpsarc::find_asset(al, 0x1D180, TX) >= 0);

    // plan parsing
    modpsarc::Plan bad;
    std::string why;
    CHECK(!modpsarc::plan_add(bad, "built/levels/t/shaders.dat.tuid/nothex.bin", "x", &why));
    CHECK(!modpsarc::plan_add(bad, "built/levels/t/gameplay.dat.tuid/0123.bin", "x", &why));
    CHECK(!modpsarc::plan_add(bad, "built/levels/t/shaders.dat.tuid/sub/0123.bin", "x", &why));
    CHECK(modpsarc::plan_add(bad, "built/levels/t/shaders.dat.tuid/0x0123", "x", &why) && bad.tuids.back().tuid == 0x123);

    // a whole member (resized), A smaller (in place), C larger (appended), an unknown tuid, a new member
    const Bytes tex2 = pattern(2500, 7, false), a2 = pattern(0x80, 8, false), c2 = pattern(0x900, 9, true);
    write_file(dir / "m" / "textures.dat", tex2);
    write_file(dir / "m" / "a.bin", a2);
    write_file(dir / "m" / "c.bin", c2);
    modpsarc::Plan plan;
    CHECK(modpsarc::plan_add(plan, "built/levels/t/textures.dat", (dir / "m" / "textures.dat").string(), &why));
    CHECK(modpsarc::plan_add(plan, "built/levels/t/shaders.dat.tuid/0123456789abcdef.bin", (dir / "m" / "a.bin").string(), &why));
    CHECK(modpsarc::plan_add(plan, "built/levels/t/shaders.dat.tuid/FEDCBA9876543210.bin", (dir / "m" / "c.bin").string(), &why));
    CHECK(modpsarc::plan_add(plan, "built/levels/t/shaders.dat.tuid/00000000000000aa.bin", (dir / "m" / "a.bin").string(), &why));
    CHECK(modpsarc::plan_add(plan, "built/levels/t/new_member.dat", (dir / "m" / "a.bin").string(), &why));
    const fs::path out = dir / "patched.psarc";
    std::string log;
    CHECK(modpsarc::build(base.string(), plan, out.string(), &log));
    fputs(log.c_str(), stdout);
    CHECK(!fs::exists(out.string() + ".tmp"));

    CHECK(member(out, "built/levels/t/textures.dat") == tex2);
    const Bytes s2 = member(out, "built/levels/t/shaders.dat");
    const Bytes al2 = member(out, "built/levels/t/assetlookup.dat");
    const long long ea = modpsarc::find_asset(al2, 0x1D100, TA), eb = modpsarc::find_asset(al2, 0x1D100, TB),
                    ec = modpsarc::find_asset(al2, 0x1D100, TC);
    CHECK(ea >= 0 && eb >= 0 && ec >= 0 && s2.size() == 150016 + 0x900);
    if (ea >= 0 && eb >= 0 && ec >= 0 && s2.size() == 150016 + 0x900) {
        CHECK(be32(&al2[ea + 8]) == 0 && be32(&al2[ea + 12]) == 0x80);
        CHECK(Bytes(s2.begin(), s2.begin() + 0x80) == a2);
        CHECK(Bytes(s2.begin() + 0x80, s2.begin() + 0x100) == Bytes(0x80, 0));
        CHECK(be32(&al2[eb + 8]) == 0x100 && be32(&al2[eb + 12]) == 0x1000);
        CHECK(Bytes(s2.begin() + 0x100, s2.begin() + 150016) == Bytes(shaders.begin() + 0x100, shaders.end()));
        CHECK(be32(&al2[ec + 8]) == 150016 && be32(&al2[ec + 12]) == 0x900);
        CHECK(Bytes(s2.begin() + 150016, s2.end()) == c2);
    }
    const Bytes toc2 = member(out, "packed/levels/t/level_cached.toc");
    CHECK(toc_size(toc2, "/built/levels/t/shaders.dat") == 150016 + 0x900);
    CHECK(toc_size(toc2, "/built/levels/t/textures.dat") == 2500);
    // untouched members, the manifest and the member order
    CHECK(member(out, "other/readme.txt") == Bytes(5, 'x'));
    CHECK(member(out, "big/raw.bin") == pattern(200000, 3, false));
    std::vector<std::string> names2;
    CHECK(modpsarc::list_members(out.string(), names2) && names2 == names);
    // the original archive is read only
    CHECK(member(base, "/built/levels/t/shaders.dat") == shaders);

    // a plan that changes nothing writes nothing
    modpsarc::Plan none;
    CHECK(modpsarc::plan_add(none, "built/levels/t/shaders.dat.tuid/00000000000000aa.bin", (dir / "m" / "a.bin").string(), &why));
    log.clear();
    CHECK(!modpsarc::build(base.string(), none, (dir / "none.psarc").string(), &log));
    CHECK(!fs::exists(dir / "none.psarc"));

    // the cache key follows the mod files
    const uint64_t k1 = modpsarc::plan_key(base.string(), plan);
    CHECK(k1 == modpsarc::plan_key(base.string(), plan));
    write_file(dir / "m" / "c.bin", pattern(0x901, 9, true));
    CHECK(k1 != modpsarc::plan_key(base.string(), plan));
    return 0;
}

static int setup(const fs::path& dir)
{
    fs::remove_all(dir);
    const fs::path game = dir / "game" / "PS3_GAME" / "USRDIR";
    write_file(game / "packed" / "levels" / "t" / "level_cached.psarc", level_archive());
    write_file(game / "packed" / "plain.bin", Bytes(4, 1));
    write_file(game / "packed" / "keep.bin", Bytes(4, 2));
    const fs::path mod = dir / "mods" / "MyMod" / "USRDIR" / "packed";
    write_file(mod / "plain.bin", Bytes(4, 9));
    const fs::path arc = mod / "levels" / "t" / "level_cached.psarc" / "built" / "levels" / "t";
    write_file(arc / "textures.dat", pattern(3000, 11, false));
    write_file(arc / "shaders.dat.tuid" / "fedcba9876543210.bin", pattern(0x500, 12, false));
    // a later mod adds a member override; a disabled one must be ignored
    write_file(dir / "mods" / "ZLater" / "USRDIR" / "packed" / "levels" / "t" / "level_cached.psarc" / "other" / "readme.txt", Bytes(3, 'z'));
    write_file(dir / "mods" / "_Off" / "USRDIR" / "packed" / "keep.bin", Bytes(4, 7));
    return 0;
}

static std::string hook(const fs::path& p)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", p.string().c_str());
    if (g_vfs_override_hook) g_vfs_override_hook(buf, sizeof buf);
    return buf;
}

static int hook_test(const fs::path& dir)
{
    CHECK(g_vfs_override_hook != nullptr);
    if (!g_vfs_override_hook) return 1;
    const fs::path game = dir / "game" / "PS3_GAME" / "USRDIR";
    const fs::path arc = game / "packed" / "levels" / "t" / "level_cached.psarc";
    CHECK(fs::equivalent(hook(game / "packed" / "plain.bin"), dir / "mods" / "MyMod" / "USRDIR" / "packed" / "plain.bin"));
    CHECK(hook(game / "packed" / "keep.bin") == (game / "packed" / "keep.bin").string());
    const std::string to = hook(arc);
    printf("archive -> %s\n", to.c_str());
    CHECK(to != arc.string() && to.find(".cache") != std::string::npos);
    CHECK(hook(arc) == to);   // built once
    CHECK(member(to, "built/levels/t/textures.dat") == pattern(3000, 11, false));
    CHECK(member(to, "other/readme.txt") == Bytes(3, 'z'));
    const Bytes al = member(to, "built/levels/t/assetlookup.dat"), s = member(to, "built/levels/t/shaders.dat");
    const long long e = modpsarc::find_asset(al, 0x1D100, TC);
    CHECK(e >= 0);
    if (e >= 0) {
        const uint32_t off = be32(&al[e + 8]), len = be32(&al[e + 12]);
        CHECK(len == 0x500 && off + len <= s.size());
        if (off + len <= s.size()) CHECK(Bytes(s.begin() + off, s.begin() + off + len) == pattern(0x500, 12, false));
    }
    CHECK(read_file(arc) == level_archive());   // the game's own file is never written
    return 0;
}

// After a mod file changes the copy is rebuilt and the old one deleted (runs after "hook").
static int rebuild_test(const fs::path& dir)
{
    CHECK(g_vfs_override_hook != nullptr);
    if (!g_vfs_override_hook) return 1;
    const fs::path arc = dir / "game" / "PS3_GAME" / "USRDIR" / "packed" / "levels" / "t" / "level_cached.psarc";
    write_file(dir / "mods" / "MyMod" / "USRDIR" / "packed" / "levels" / "t" / "level_cached.psarc" / "built" / "levels" / "t" /
               "shaders.dat.tuid" / "fedcba9876543210.bin", pattern(0x600, 13, false));
    const std::string to = hook(arc);
    int copies = 0;
    for (const auto& e : fs::directory_iterator(dir / "mods" / ".cache")) copies += e.path().extension() == ".psarc";
    CHECK(copies == 1);
    const Bytes al = member(to, "built/levels/t/assetlookup.dat"), s = member(to, "built/levels/t/shaders.dat");
    const long long e = modpsarc::find_asset(al, 0x1D100, TC);
    CHECK(e >= 0);
    if (e >= 0) {
        const uint32_t off = be32(&al[e + 8]), len = be32(&al[e + 12]);
        CHECK(len == 0x600 && off + len <= s.size());
        if (off + len <= s.size()) CHECK(Bytes(s.begin() + off, s.begin() + off + len) == pattern(0x600, 13, false));
    }
    return 0;
}

static int real(const fs::path& archive)
{
    if (!fs::exists(archive)) { printf("no game data at %s, skipped\n", archive.string().c_str()); return 77; }
    std::vector<std::string> names;
    CHECK(modpsarc::list_members(archive.string(), names));
    std::string dat, alname;
    for (const auto& n : names) {
        if (n.size() > 16 && !n.compare(n.size() - 16, 16, "/assetlookup.dat")) alname = n;
        if (n.size() > 12 && !n.compare(n.size() - 12, 12, "/shaders.dat")) dat = n;
    }
    CHECK(!dat.empty() && !alname.empty());
    if (dat.empty() || alname.empty()) return 1;
    const Bytes al = member(archive, alname), s = member(archive, dat);
    // the first shader the lookup lists, replaced by a larger blob
    long long e = -1;
    uint64_t tuid = 0;
    for (uint32_t i = 0, n = be32(&al[8]); i < n && e < 0; i++)
        if (be32(&al[0x20 + i * 16]) == 0x1D100 && be32(&al[0x20 + i * 16 + 12]) >= 16) {
            const uint32_t at = be32(&al[0x20 + i * 16 + 4]);
            tuid = (uint64_t)be32(&al[at]) << 32 | be32(&al[at + 4]);
            e = modpsarc::find_asset(al, 0x1D100, tuid);
        }
    CHECK(e >= 0);
    if (e < 0) return 1;
    const uint32_t len = be32(&al[e + 12]);
    const fs::path dir = fs::absolute("real");
    fs::remove_all(dir);
    const Bytes blob = pattern(len + 0x123, 5, false);
    char hex[32];
    snprintf(hex, sizeof hex, "%016llx.bin", (unsigned long long)tuid);
    write_file(dir / hex, blob);
    modpsarc::Plan plan;
    std::string why, log;
    CHECK(modpsarc::plan_add(plan, dat + ".tuid/" + hex, (dir / hex).string(), &why));
    const fs::path out = dir / "out.psarc";
    CHECK(modpsarc::build(archive.string(), plan, out.string(), &log));
    fputs(log.c_str(), stdout);
    const Bytes al2 = member(out, alname), s2 = member(out, dat);
    const long long e2 = modpsarc::find_asset(al2, 0x1D100, tuid);
    CHECK(e2 == e);
    if (e2 >= 0) {
        const uint32_t off = be32(&al2[e2 + 8]);
        CHECK(off == ((s.size() + 0x7F) & ~(size_t)0x7F) && be32(&al2[e2 + 12]) == blob.size());
        CHECK(s2.size() >= off + blob.size());
        if (s2.size() >= off + blob.size()) {
            CHECK(Bytes(s2.begin() + off, s2.begin() + off + blob.size()) == blob);
            CHECK(Bytes(s2.begin(), s2.begin() + s.size()) == s);
        }
    }
    // every other member reads back unchanged; the level .toc lists the new size
    std::vector<std::string> names2;
    CHECK(modpsarc::list_members(out.string(), names2) && names2 == names);
    int tocs = 0;
    for (const auto& n : names) {
        if (n == dat || n == alname) continue;
        Bytes a, b;
        CHECK(modpsarc::read_member(archive.string(), n, a) && modpsarc::read_member(out.string(), n, b));
        if (n.size() > 4 && !n.compare(n.size() - 4, 4, ".toc") && toc_size(a, "/" + dat) != 0xFFFFFFFF) {
            CHECK(toc_size(a, "/" + dat) == s.size() && toc_size(b, "/" + dat) == s2.size());
            tocs++;
            continue;
        }
        if (a != b) { fprintf(stderr, "member %s changed\n", n.c_str()); g_fail++; }
    }
    printf("%zu members checked, %d level .toc updated\n", names.size(), tocs);
    CHECK(tocs == 1);
    fs::remove_all(dir);
    return 0;
}

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "unit";
    int r = 0;
    if (mode == "unit") r = unit();
    else if (mode == "setup" && argc > 2) r = setup(fs::absolute(argv[2]));
    else if (mode == "hook" && argc > 2) r = hook_test(fs::absolute(argv[2]));
    else if (mode == "rebuild" && argc > 2) r = rebuild_test(fs::absolute(argv[2]));
    else if (mode == "nohook") CHECK(g_vfs_override_hook == nullptr);
    else if (mode == "real" && argc > 2) r = real(argv[2]);
    else { fprintf(stderr, "usage: mods_psarc_test unit|setup <dir>|hook <dir>|nohook|real <archive>\n"); return 2; }
    if (r == 77) return 77;
    printf("%s: %s (%d failure(s))\n", mode.c_str(), g_fail || r ? "FAILED" : "ok", g_fail);
    return g_fail || r ? 1 : 0;
}
