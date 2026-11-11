// Mods folder: per-member PSARC overrides and per-asset (tuid) overrides.
//
// A mod folder named like an archive (mods\<mod>\USRDIR\packed\levels\<lvl>\level_cached.psarc\)
// holds files that replace single members of that archive, by their path inside it:
//
//     ...\level_cached.psarc\built\levels\<lvl>\shaders.dat
//
// and single assets of a level's .dat files, by the 64-bit tuid that the level's
// assetlookup.dat lists for them:
//
//     ...\level_cached.psarc\built\levels\<lvl>\shaders.dat.tuid\<tuid in hex>.bin
//
// build() writes a copy of the archive with those changes: every untouched member keeps its
// original compressed bytes, changed members are appended, and the archive's table of
// contents (and the level's .toc member, which lists the .dat sizes) is updated.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace modpsarc {

struct MemberFile { std::string member; std::string file; };              // member: lower case, '/', no leading '/'
struct TuidFile   { std::string dat; uint64_t tuid; std::string file; };  // dat: member path of the .dat holding the asset

struct Plan {
    std::vector<MemberFile> members;
    std::vector<TuidFile> tuids;
    bool empty() const { return members.empty() && tuids.empty(); }
};

// Adds one mod file, given by its path below the archive folder (lower case, '/').
// false (and *why) when it names neither a member nor a valid tuid file.
bool plan_add(Plan& plan, const std::string& rel, const std::string& host_file, std::string* why);

// Fingerprint of the inputs (paths, sizes, times) for naming the cached archive.
uint64_t plan_key(const std::string& base, const Plan& plan);

// Writes `base` with `plan` applied to `out` (through out + ".tmp"). Appends log lines to *log.
bool build(const std::string& base, const Plan& plan, const std::string& out, std::string* log);

// Archive helpers (also used by the tests).
bool list_members(const std::string& archive, std::vector<std::string>& names);
bool read_member(const std::string& archive, const std::string& member, std::vector<uint8_t>& data);

// assetlookup.dat section that points into a level .dat (by file name), 0 = none.
uint32_t asset_section_for(const std::string& dat_member);

// Finds `tuid` in the pointer section `section` of an assetlookup.dat image.
// Returns the byte offset of its 16-byte entry {u64 tuid, u32 offset, u32 length}, or -1.
long long find_asset(const std::vector<uint8_t>& assetlookup, uint32_t section, uint64_t tuid);

} // namespace modpsarc
