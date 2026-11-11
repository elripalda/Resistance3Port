// r*_trophy_data.h, as the launcher's Trophies page uses it: TROPHY.TRP entry table and TROP.SFM
// parsing (grades, hidden flags), icon entries next to it, and merging the two unlock records
// (the runtime's gamedata\trophies\<id>.json and the port's Saved Games mirror; earliest time wins).
#if __has_include("r3_trophy_data.h")
#include "r3_trophy_data.h"
#else
#include "r2_trophy_data.h"
#endif
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void put_be(std::vector<unsigned char>& d, size_t at, uint64_t v, int n)
{
    for (int i = 0; i < n; i++) d[at + i] = (unsigned char)(v >> (8 * (n - 1 - i)));
}
static std::vector<unsigned char> make_trp(const std::vector<std::pair<std::string, std::string>>& files)
{
    const size_t esz = 64;
    std::vector<unsigned char> d(64 + files.size() * esz, 0);
    put_be(d, 0, 0xDCA24D00u, 4); put_be(d, 4, 1, 4);
    put_be(d, 16, files.size(), 4); put_be(d, 20, esz, 4);
    for (size_t i = 0; i < files.size(); i++) {
        const size_t e = 64 + i * esz, off = d.size();
        memcpy(&d[e], files[i].first.c_str(), files[i].first.size());
        put_be(d, e + 32, off, 8); put_be(d, e + 40, files[i].second.size(), 8);
        d.insert(d.end(), files[i].second.begin(), files[i].second.end());
    }
    put_be(d, 8, d.size(), 8);
    return d;
}
static bool write_file(const char* p, const void* b, size_t n)
{
    FILE* f = fopen(p, "wb"); if (!f) return false;
    const bool ok = fwrite(b, 1, n, f) == n; fclose(f); return ok;
}

int main()
{
    const std::string sfm =
        "<!--Sce-Signature--><trophyconf><title-name>Test &amp; Game</title-name>"
        "<trophy id=\"000\" hidden=\"no\" ttype=\"P\" pid=\"-1\"><name>All</name><detail>Every trophy.</detail></trophy>"
        "<trophy id=\"001\" hidden=\"no\" ttype=\"B\" pid=\"000\"><name>First</name><detail>Do a thing.</detail></trophy>"
        "<trophy id=\"002\" hidden=\"yes\" ttype=\"G\" pid=\"000\"><name>Secret</name><detail>Spoiler.</detail></trophy>"
        "<trophy id=\"003\" hidden=\"no\" ttype=\"S\" pid=\"000\"><name>Third</name><detail>&lt;3</detail></trophy>"
        "</trophyconf>";
    const auto trp = make_trp({ { "TROPCONF.SFM", "x" }, { "TROP.SFM", sfm }, { "ICON0.PNG", "\x89PNG...." }, { "TROP001.PNG", "\x89PNGicon1" } });
    CHECK(write_file("test_trophy_list.trp", trp.data(), trp.size()));

    std::vector<r2t::Trophy> list; std::string title;
    CHECK(r2t::load_trp("test_trophy_list.trp", list, &title));
    CHECK(title == "Test & Game");
    CHECK(list.size() == 4);
    if (list.size() == 4) {
        CHECK(list[0].grade == 'P' && list[1].grade == 'B' && list[2].grade == 'G' && list[3].grade == 'S');
        CHECK(!list[1].hidden && list[2].hidden && list[1].pid == 0 && list[0].pid == -1);
        CHECK(list[3].detail == "<3");
        for (const auto& t : list) CHECK(t.unlocked == 0);
    }
    // the icon entries the launcher shows: TROPnnn.PNG at the offsets in the entry table
    {
        const uint32_t count = r2t::be32(&trp[16]), esz = r2t::be32(&trp[20]);
        bool found = false;
        for (uint32_t i = 0; i < count; i++) {
            const unsigned char* e = &trp[64 + (size_t)i * esz];
            if (!strcmp((const char*)e, "TROP001.PNG"))
                found = !memcmp(&trp[r2t::be64(e + 32)], "\x89PNGicon1", (size_t)r2t::be64(e + 40));
        }
        CHECK(found);
    }

    // runtime record ("timestamp") and the port's mirror ("unlocked"); either counts, the earliest time wins
    const char* hle = "{ \"trophies\": [ { \"id\": 1, \"timestamp\": 1700000500 }, { \"id\": 3, \"timestamp\": 1700000900 } ] }";
    const char* mirror = "{ \"game\": \"x\", \"trophies\": [ { \"id\": 1, \"grade\": \"B\", \"name\": \"First\", \"unlocked\": 1700000400 },"
                         " { \"id\": 0, \"grade\": \"P\", \"name\": \"All\", \"unlocked\": 1700001000 } ] }";
    CHECK(write_file("test_trophy_hle.json", hle, strlen(hle)));
    CHECK(write_file("test_trophy_mirror.json", mirror, strlen(mirror)));
    r2t::merge_json_times("test_trophy_hle.json", list);
    r2t::merge_json_times("test_trophy_mirror.json", list);
    r2t::merge_json_times("no_such_file.json", list);         // no data yet: nothing changes
    if (list.size() == 4) {
        CHECK(list[0].unlocked == 1700001000);   // platinum: only in the mirror
        CHECK(list[1].unlocked == 1700000400);   // both: earliest
        CHECK(list[2].unlocked == 0);            // hidden, locked
        CHECK(list[3].unlocked == 1700000900);   // only in the runtime record
    }

    // not a TRP / missing
    CHECK(write_file("test_trophy_bad.trp", "nope", 4));
    CHECK(!r2t::load_trp("test_trophy_bad.trp", list));
    CHECK(!r2t::load_trp("no_such_file.trp", list));

    remove("test_trophy_list.trp"); remove("test_trophy_hle.json"); remove("test_trophy_mirror.json"); remove("test_trophy_bad.trp");
    printf(fails ? "trophy_list: %d failure(s)\n" : "trophy_list: all passed\n", fails);
    return fails ? 1 : 0;
}
