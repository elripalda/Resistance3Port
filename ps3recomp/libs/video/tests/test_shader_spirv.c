/*
 * ps3recomp - HLSL -> SPIR-V translation cache test
 *
 * rsx_hlsl_to_spirv keeps every translation it makes, in memory and, with
 * PS3RECOMP_SPIRV_CACHE=<dir>, on disk. This checks that:
 *   - the same stage and text is translated once, and the hit returns the
 *     same words;
 *   - the same text for another stage is its own entry;
 *   - a new process (simulated by rsx_hlsl_to_spirv_cache_reset) is served
 *     from the disk file, with the same words;
 *   - a damaged file is a miss, and is rewritten;
 *   - a failed translation is not cached;
 *   - a hit larger than the caller's buffer still fails as before.
 *
 * Built as a CMake target (test_shader_spirv) because it links glslang.
 * Needs no GPU. On Windows (scratch\enginetest\run_all.ps1) the cache
 * directory is made under the working directory.
 *
 * It also decompiles the texture-gamma fetch variants (every fetch opcode,
 * 2D/cube/volume/1D units, the fragment-state wrappers around a sample) and
 * translates each one, so a helper the HLSL calls but never declares fails
 * here instead of at pipeline creation.
 */
#include "../rsx_shader_spirv.h"
#include "../rsx_fp_decompiler.h"
#include "../rsx_test_programs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
static int setenv(const char* n, const char* v, int o) { (void)o; return _putenv_s(n, v); }
static int unsetenv(const char* n) { return _putenv_s(n, ""); }
static char* mkdtemp(char* t)
{
    if (_mktemp_s(t, strlen(t) + 1) || _mkdir(t)) return NULL;
    return t;
}
static int truncate(const char* path, long size)
{
    int fd = -1;
    if (_sopen_s(&fd, path, _O_RDWR | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE)) return -1;
    const int r = _chsize_s(fd, size) ? -1 : 0;
    _close(fd);
    return r;
}
#define rmdir _rmdir
#else
#include <dirent.h>
#include <unistd.h>
#endif

static int g_pass = 0, g_fail = 0;

static void check(const char* what, int ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) g_pass++; else g_fail++;
}

static const char* kVS =
    "struct VOut { float4 pos : SV_Position; float4 col : COLOR0; };\n"
    "VOut main(float4 p : ATTR0, float4 c : ATTR3) {\n"
    "    VOut o; o.pos = p; o.col = c; return o;\n"
    "}\n";

static const char* kPS =
    "float4 main(float4 pos : SV_Position, float4 col : COLOR0) : SV_Target0 {\n"
    "    return col.zyxw;\n"
    "}\n";

/* Valid as either stage: no inputs, one output. */
static const char* kBoth =
    "float4 main() : SV_Target0 { return float4(1, 0, 0, 1); }\n";

#define MAX_WORDS 4096u
static u32 g_a[MAX_WORDS], g_b[MAX_WORDS];

static int translate(const char* hlsl, int stage, u32* words, u32* n)
{
    char log[512] = "";
    int r = rsx_hlsl_to_spirv(hlsl, stage, words, MAX_WORDS, n, log, sizeof log);
    if (r) printf("       (%s)\n", log);
    return r;
}

static RsxSpirvCacheStats stats(void)
{
    RsxSpirvCacheStats s;
    rsx_hlsl_to_spirv_cache_stats(&s);
    return s;
}

/* The one .spv file in dir, or "" when there is not exactly one. */
static void only_file(const char* dir, char* out, size_t size)
{
    int n = 0;
    out[0] = 0;
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s/*.spv", dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pattern, &fd);
    if (h == -1) return;
    do {
        snprintf(out, size, "%s/%s", dir, fd.name);
        n++;
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len > 4 && strcmp(e->d_name + len - 4, ".spv") == 0) {
            snprintf(out, size, "%s/%s", dir, e->d_name);
            n++;
        }
    }
    closedir(d);
#endif
    if (n != 1) out[0] = 0;
}

/* Gamma (RPCS3 _process_texel) on every fetch opcode and unit shape, alone
 * and under each fragment-state wrapper: alpha kill, sign extension, EXP_TEX
 * on a biased format, EXP_TEX the format ignores (bx2_lanes 0x20), and the
 * unit's own sign expansion. Each variant must reach SPIR-V. */
static void check_gamma_variants(void)
{
    static char hlsl[131072];
    static u32 words[65536];
    const u32 ops[] = {0x17, 0x18, 0x19, 0x2F, 0x31};   /* TEX TXP TXD TXL TXB */
    int total = 0, failed = 0;
    for (u32 op = 0; op < 5; ++op)
    for (u32 unit = 0; unit < 16; unit += 15)
    for (u32 dim = 0; dim < 4; ++dim)        /* 2D, cube, volume, 1D */
    for (u32 fsv = 0; fsv < 7; ++fsv)
    for (u32 mask = 1; mask < 16; mask += 2) {
        u8 fp[32] = {0};
        const u32 bx2 = (fsv == 4 || fsv == 5) ? (1u << 21) : 0;
        rsx_test_fp_put_word(fp + 0, RSX_TEST_FP_OPC(ops[op]) | RSX_TEST_FP_MASK_XYZW |
                             RSX_TEST_FP_INSRC_TC0 | RSX_TEST_FP_TEXU(unit) |
                             RSX_TEST_FP_END | bx2);
        rsx_test_fp_put_word(fp + 4, RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT |
                             RSX_TEST_FP_EXEC_ALWAYS);
        rsx_test_fp_put_word(fp + 8, RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT | (5u << 2));
        rsx_test_fp_put_word(fp + 12, RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT | (6u << 2));
        u8 gamma[16] = {0}, expand[16] = {0};
        gamma[unit] = (u8)mask;
        rsx_fp_frag_state fs;
        memset(&fs, 0, sizeof fs);
        if (fsv == 2) fs.alpha_kill_mask = (u16)(1u << unit);
        if (fsv == 3) fs.sext_masks[unit] = (u8)(mask ^ 5u);
        if (fsv == 4) fs.bx2_lanes[unit] = 0x17u;
        if (fsv == 5) fs.bx2_lanes[unit] = 0x20u;
        if (fsv == 6) expand[unit] = 0xFu;
        rsx_fp_set_tex_1d_mask(dim == 3 ? 1u << unit : 0);
        rsx_fp_shadow_desc shadow[16];
        memset(shadow, 0, sizeof shadow);
        u32 constants = 0, n = 0;
        char log[1024] = "";
        ++total;
        const int r = rsx_fp_decompile_engine(fp, sizeof fp, 0x40, dim == 1 ? 1u << unit : 0,
                                              dim == 2 ? 1u << unit : 0, shadow, gamma, expand,
                                              fsv ? &fs : NULL, hlsl, sizeof hlsl, &constants);
        if (r <= 0 ||
            rsx_hlsl_to_spirv(hlsl, RSX_SHADER_STAGE_FRAGMENT, words, 65536, &n, log, sizeof log)) {
            if (++failed <= 4)
                printf("       op %02X unit %u dim %u state %u mask %u: %s\n",
                       ops[op], unit, dim, fsv, mask, r <= 0 ? "decompile failed" : log);
        }
    }
    rsx_fp_set_tex_1d_mask(0);
    char what[96];
    snprintf(what, sizeof what, "gamma fetch variants reach SPIR-V (%d/%d)", total - failed, total);
    check(what, failed == 0);
}

int main(void)
{
    u32 na = 0, nb = 0;
    RsxSpirvCacheStats s;

    if (!rsx_hlsl_to_spirv_available()) {
        printf("[FAIL] built without glslang\n");
        return 1;
    }
    unsetenv("PS3RECOMP_SPIRV_CACHE");
    rsx_hlsl_to_spirv_cache_reset();

    /* Memory. */
    check("vertex program translates", translate(kVS, RSX_SHADER_STAGE_VERTEX, g_a, &na) == 0 && na > 5);
    check("SPIR-V magic", g_a[0] == 0x07230203u);
    check("same request again succeeds", translate(kVS, RSX_SHADER_STAGE_VERTEX, g_b, &nb) == 0);
    check("... with the same words", na == nb && memcmp(g_a, g_b, na * 4u) == 0);
    s = stats();
    check("... served from memory, glslang ran once", s.translations == 1 && s.memory_hits == 1);

    check("fragment program translates", translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb) == 0);
    check("... with other words", na != nb || memcmp(g_a, g_b, na * 4u) != 0);

    check("same text as a vertex program", translate(kBoth, RSX_SHADER_STAGE_VERTEX, g_a, &na) == 0);
    check("same text as a fragment program", translate(kBoth, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb) == 0);
    s = stats();
    check("... two entries, not a hit", s.translations == 4 && s.memory_hits == 1);
    check("... with other words", na != nb || memcmp(g_a, g_b, na * 4u) != 0);

    {
        u32 small[4], n = 123;
        int r = rsx_hlsl_to_spirv(kPS, RSX_SHADER_STAGE_FRAGMENT, small, 4, &n, NULL, 0);
        check("a hit larger than the buffer fails", r == -1 && n == 0);
    }

    check("broken HLSL fails", translate("float4 main( {", RSX_SHADER_STAGE_FRAGMENT, g_a, &na) != 0);
    check("... and again (not cached)", translate("float4 main( {", RSX_SHADER_STAGE_FRAGMENT, g_a, &na) != 0);
    s = stats();
    check("... counted as no translation", s.translations == 4);

    /* Disk. */
#ifdef _WIN32
    char dir[] = "ps3recomp_spirv_cache_XXXXXX";
#else
    char dir[] = "/tmp/ps3recomp_spirv_cache_XXXXXX";
#endif
    if (!mkdtemp(dir)) { printf("[FAIL] mkdtemp\n"); return 1; }
    setenv("PS3RECOMP_SPIRV_CACHE", dir, 1);
    rsx_hlsl_to_spirv_cache_reset();

    check("disk: first run translates", translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_a, &na) == 0);
    s = stats();
    check("... with glslang", s.translations == 1 && s.disk_hits == 0);
    char path[1024];
    only_file(dir, path, sizeof path);
    check("... and writes one file", path[0] != 0);

    rsx_hlsl_to_spirv_cache_reset();
    check("disk: next run succeeds", translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb) == 0);
    s = stats();
    check("... from the file, no glslang", s.disk_hits == 1 && s.translations == 0);
    check("... with the same words", na == nb && memcmp(g_a, g_b, na * 4u) == 0);
    translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb);
    s = stats();
    check("... then from memory", s.memory_hits == 1 && s.disk_hits == 1);

    /* Damage the stored key: the file no longer matches the request. */
    if (path[0]) {
        FILE* f = fopen(path, "r+b");
        if (f) { fseek(f, 13 * 4 + 10, SEEK_SET); fputc('#', f); fclose(f); }
    }
    rsx_hlsl_to_spirv_cache_reset();
    check("disk: damaged file still succeeds", translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb) == 0);
    s = stats();
    check("... as a miss, with glslang", s.disk_hits == 0 && s.translations == 1);
    check("... with the same words", na == nb && memcmp(g_a, g_b, na * 4u) == 0);
    rsx_hlsl_to_spirv_cache_reset();
    translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb);
    s = stats();
    check("... and the file was rewritten", s.disk_hits == 1 && s.translations == 0);

    /* A truncated file. */
    if (path[0] && truncate(path, 20) == 0) {
        rsx_hlsl_to_spirv_cache_reset();
        check("disk: truncated file still succeeds", translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb) == 0);
        s = stats();
        check("... as a miss", s.disk_hits == 0 && s.translations == 1);
    } else {
        check("truncate", 0);
    }

    only_file(dir, path, sizeof path);
    if (path[0]) remove(path);
    rmdir(dir);

    unsetenv("PS3RECOMP_SPIRV_CACHE");
    rsx_hlsl_to_spirv_cache_reset();
    check_gamma_variants();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
