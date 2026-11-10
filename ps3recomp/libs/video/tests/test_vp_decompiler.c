/*
 * Vertex program decompiler parity tests against RPCS3's
 * VertexProgramDecompiler / analyse_vertex_program rules. Synthetic programs
 * check the individual rules; the RPCS3 shader-cache corpus (when present)
 * must decompile and compile as vs_5_0.
 */
#include "rsx_vp_decompiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3dcompiler.h>

static int s_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); s_fail++; } } while (0)

static char hlsl[256 * 1024];

/* ---- encoder ---------------------------------------------------------- */
#define SWZ_ID 0x1B00u                     /* x y z w */
static u32 src_temp(u32 r)  { return 1u | (r << 2) | SWZ_ID; }
static u32 src_in(void)     { return 2u | SWZ_ID; }
static u32 src_const(void)  { return 3u | SWZ_ID; }

typedef struct {
    u32 vec, sca, src0, src1, src2, input, cst, index_const;
    u32 dst, dst_tmp, sca_tmp, vmask, smask, vec_result, end;
    u32 cond, cond_test, cc_up0, cc_up1, saturate, addr_sel, addr_swz, cc_swz;
    u32 target, brb_bit, brb_true;
} Ins;

static void enc(u8* p, const Ins* i)
{
    const u32 s0 = i->src0, s1 = i->src1, s2 = i->src2;
    u32 d0 = (i->addr_swz & 3u) | ((i->cc_swz & 0xFFu) << 2) | ((i->cond & 7u) << 10) |
             (i->cond_test << 13) | (i->cc_up0 << 14) | ((i->dst_tmp & 0x3Fu) << 15) |
             (((i->target >> 9) & 1u) << 23) | (i->addr_sel << 24) |
             (i->saturate << 26) | (i->cc_up1 << 29) | (i->vec_result << 30);
    u32 d1 = ((s0 >> 9) & 0xFFu) | ((i->input & 0xFu) << 8) | ((i->cst & 0x3FFu) << 12) |
             ((i->vec & 0x1Fu) << 22) | ((i->sca & 0x1Fu) << 27);
    u32 d2 = ((s2 >> 11) & 0x3Fu) | ((s1 & 0x1FFFFu) << 6) | ((s0 & 0x1FFu) << 23);
    u32 d3 = i->end | (i->index_const << 1) | ((i->dst & 0x1Fu) << 2) |
             ((i->sca_tmp & 0x3Fu) << 7) | ((i->vmask & 0xFu) << 13) |
             ((i->smask & 0xFu) << 17) | ((s2 & 0x7FFu) << 21);
    if (i->sca == 0x09 || i->sca == 0x11 || i->sca == 0x0A || i->sca == 0x0C) {
        d2 = (d2 & ~0x3Fu) | ((i->target >> 3) & 0x3Fu);
        d3 = (d3 & ~(0x1FFu << 23)) | ((i->brb_bit & 0x1Fu) << 23) |
             (i->brb_true << 28) | ((i->target & 7u) << 29);
    }
    const u32 w[4] = { d0, d1, d2, d3 };
    memcpy(p, w, 16);
}

/* Defaults: no destinations, sources valid, cond always. */
static Ins base_ins(void)
{
    Ins i; memset(&i, 0, sizeof i);
    i.src0 = i.src1 = i.src2 = src_in();
    i.dst = 0x1F; i.dst_tmp = 0x3F; i.sca_tmp = 0x3F; i.cond = 7; i.cc_swz = 0x1B;
    return i;
}
static Ins mov_out(u32 dst, u32 src) { Ins i = base_ins(); i.vec = 1; i.src0 = src; i.dst = dst; i.vec_result = 1; i.vmask = 0xF; return i; }

static int decompile(const u8* mem, u32 slots, u32 entry)
{
    return rsx_vp_decompile_program(mem, slots, entry, 0, 0xFFFFu, 0, hlsl, sizeof hlsl);
}

static int compiles(const char* src)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), "vp", NULL, NULL, "main", "vs_5_0",
                            D3DCOMPILE_SKIP_OPTIMIZATION, 0, &code, &err);
    if (FAILED(hr) && err) printf("%.*s\n", (int)err->lpVtbl->GetBufferSize(err) > 600 ? 600 :
                                  (int)err->lpVtbl->GetBufferSize(err),
                                  (const char*)err->lpVtbl->GetBufferPointer(err));
    if (code) code->lpVtbl->Release(code);
    if (err) err->lpVtbl->Release(err);
    return SUCCEEDED(hr);
}

static void test_rules(void)
{
    u8 m[16 * 16];
    Ins i;

    /* ARL truncates (int4 cast), into a[dst_tmp]; indexed constants wrap at
     * 512 and read 0 past 467; a direct constant past 467 is float4(0). */
    memset(m, 0, sizeof m);
    i = base_ins(); i.vec = 0x0D; i.dst_tmp = 1; i.vmask = 0xF; enc(m, &i);
    i = mov_out(0, src_const()); i.cst = 40; i.index_const = 1; i.addr_sel = 1; i.addr_swz = 2; enc(m + 16, &i);
    i = mov_out(1, src_const()); i.cst = 470; i.end = 1; enc(m + 32, &i);
    CHECK(decompile(m, 3, 0) > 0, "decompile arl");
    CHECK(strstr(hlsl, "(int4)_v") && !strstr(hlsl, "floor(_v") && !strstr(hlsl, "floor((") , "ARL must truncate");
    CHECK(strstr(hlsl, "a1.xyzw = _vi.xyzw"), "ARL writes a[dst_tmp]");
    CHECK(strstr(hlsl, "vp_cidx((40u + (uint)a1.z) & 511u)"), "indexed constant via vp_cidx");
    CHECK(!strstr(hlsl, "vp_c[470]") && strstr(hlsl, "(float4)(float4(0,0,0,0))"), "const >= 468 reads 0");
    CHECK(compiles(hlsl), "arl program compiles");

    /* cond == FL writes nothing; a VEC op with no destination but CC update
     * writes CC; a temp write with CC update bits does not also write CC. */
    memset(m, 0, sizeof m);
    i = base_ins(); i.vec = 1; i.dst_tmp = 3; i.vmask = 0xF; i.cond = 0; enc(m, &i);
    i = base_ins(); i.vec = 1; i.vmask = 0xF; i.cc_up0 = 1; enc(m + 16, &i);
    i = base_ins(); i.vec = 1; i.dst_tmp = 4; i.vmask = 0xF; i.cc_up0 = 1; i.cc_up1 = 1; enc(m + 32, &i);
    i = mov_out(0, src_temp(4)); i.end = 1; enc(m + 48, &i);
    CHECK(decompile(m, 4, 0) > 0, "decompile cc");
    CHECK(!strstr(hlsl, "r[3]."), "cond FL must not write");
    {
        const char* a = strstr(hlsl, "cc[0].xyzw = _v.xyzw");
        CHECK(a && !strstr(a + 1, "cc[0].xyzw = _v.xyzw"), "exactly one CC-only write");
    }
    CHECK(compiles(hlsl), "cc program compiles");

    /* Branch condition: any() over the whole CC swizzle. */
    memset(m, 0, sizeof m);
    i = base_ins(); i.sca = 0x09; i.cond = 2; i.target = 2; enc(m, &i);
    i = mov_out(0, src_in()); enc(m + 16, &i);
    i = mov_out(1, src_in()); i.end = 1; enc(m + 32, &i);
    CHECK(decompile(m, 3, 0) > 0, "decompile bri");
    CHECK(strstr(hlsl, "any(cc[0].xyzw == (float4)0)"), "BRI uses any() of the CC swizzle");
    CHECK(compiles(hlsl), "bri program compiles");

    /* Entry past a subroutine: BRB at slot 4 (entry) branches back to slot
     * 2; slot 3 is never reached. Base 2, entry 2 relative, slot 3 dead. */
    memset(m, 0, sizeof m);
    i = mov_out(1, src_in()); i.end = 1; enc(m, &i);                 /* slot 0: other program */
    i = mov_out(0, src_in()); i.input = 1; i.end = 1; enc(m + 32, &i);
    i = mov_out(2, src_in()); i.end = 1; enc(m + 48, &i);             /* dead */
    i = base_ins(); i.sca = 0x11; i.target = 2; i.brb_bit = 3; i.brb_true = 1; enc(m + 64, &i);
    i = mov_out(0, src_in()); i.end = 1; enc(m + 80, &i);
    {
        rsx_vp_program_info info;
        CHECK(rsx_vp_analyse_program(m, 16, 4, &info) == 4, "analysis count");
        CHECK(info.base == 2 && info.entry == 2 && info.has_branch, "analysis base/entry");
        CHECK(info.mask[0] == 0xDu, "analysis live mask %x", info.mask[0]);
    }
    CHECK(decompile(m, 16, 4) > 0, "decompile entry");
    CHECK(strstr(hlsl, "int _pc = 2;"), "starts at the relative entry");
    CHECK(strstr(hlsl, "((vp_branch.x & 8u) != 0u) ? 0 : 3"), "BRB target relative to base");
    CHECK(!strstr(hlsl, "o[2]."), "dead slot skipped");
    CHECK(compiles(hlsl), "entry program compiles");

    /* Conditional forward branch past an end bit keeps decoding. */
    memset(m, 0, sizeof m);
    i = base_ins(); i.sca = 0x11; i.target = 3; i.brb_bit = 0; enc(m, &i);
    i = mov_out(0, src_in()); i.end = 1; enc(m + 16, &i);
    i = mov_out(2, src_in()); i.end = 1; enc(m + 32, &i);
    i = mov_out(0, src_in()); i.input = 2; i.end = 1; enc(m + 48, &i);
    {
        /* RPCS3 walks on until the furthest target, so slot 2 is live too. */
        rsx_vp_program_info info;
        const int n = rsx_vp_analyse_program(m, 16, 0, &info);
        CHECK(n == 4 && info.mask[0] == 0xFu, "branch past end: count %d mask %x", n, info.mask[0]);
    }

    /* Scalar ops are per component; RSQ broadcasts x without abs; LG2 has no abs. */
    memset(m, 0, sizeof m);
    i = base_ins(); i.sca = 0x04; i.sca_tmp = 1; i.smask = 0xF; enc(m, &i);
    i = base_ins(); i.sca = 0x0D; i.sca_tmp = 2; i.smask = 0xF; enc(m + 16, &i);
    i = base_ins(); i.sca = 0x02; i.dst = 0; i.smask = 0xF; i.end = 1; enc(m + 32, &i);
    CHECK(decompile(m, 3, 0) > 0, "decompile sca");
    CHECK(strstr(hlsl, "1.0/sqrt(max(("), "RSQ form");
    CHECK(strstr(hlsl, "log2(max(((v[0]).xyzw), 1e-10))"), "LG2 per component, no abs");
    CHECK(strstr(hlsl, "(1.0/((v[0]).xyzw))") && strstr(hlsl, "o[0].xyzw"), "SCA RCP writes o[dst] when !vec_result");
    CHECK(compiles(hlsl), "sca program compiles");

    /* No position write: the whole body is NOPed. */
    memset(m, 0, sizeof m);
    i = mov_out(1, src_in()); i.end = 1; enc(m, &i);
    CHECK(decompile(m, 1, 0) > 0, "decompile nopos");
    CHECK(!strstr(hlsl, "o[1].xyzw = _v") && strstr(hlsl, "Out.col0 = _d;"), "no-POS program is NOPed");

    /* Register type 0 aborts the program there. */
    memset(m, 0, sizeof m);
    i = mov_out(0, src_in()); enc(m, &i);
    i = mov_out(1, src_in()); i.src1 = 0; enc(m + 16, &i);
    i = mov_out(2, src_in()); i.end = 1; enc(m + 32, &i);
    CHECK(decompile(m, 3, 0) > 0, "decompile abort");
    CHECK(strstr(hlsl, "o[0].xyzw") && !strstr(hlsl, "o[1].xyzw = _v") && !strstr(hlsl, "o[2].xyzw = _v"),
          "invalid source ends the program");

    /* NVIDIA compares (RPCS3 VKVertexProgram.cpp:367 low_precision_tests):
     * both operands through CMP_FIXUP. Point size: POINT_SIZE whenever the
     * program's o6.x is not exported, mask bit clear included. */
    memset(m, 0, sizeof m);
    i = mov_out(0, src_in()); i.vec = 0x0B; i.src1 = src_const(); i.cst = 3; i.end = 1; enc(m, &i);
    CHECK(decompile(m, 1, 0) > 0, "decompile slt");
    CHECK(strstr(hlsl, "(float4)(vp_cmpfix(") && strstr(hlsl, "return sign(a) * 16.0 + a;"),
          "SLT compares CMP_FIXUP'd operands");
    CHECK(strstr(hlsl, "Out.psize = (vp_branch.z & 0x80000000u) != 0u ? asfloat(vp_branch.w) : 1.0;"),
          "point size defaults to POINT_SIZE");
    CHECK(compiles(hlsl), "slt program compiles");

    /* 3D / cube vertex textures sample .xyz (RPCS3 VERTEX_TEXTURE_FETCH3D/CUBE). */
    memset(m, 0, sizeof m);
    i = mov_out(0, src_in()); i.vec = 0x19; i.end = 1; enc(m, &i);
    rsx_vp_set_vtex_3d_mask(1u);
    CHECK(rsx_vp_decompile_program(m, 1, 0, 1u, 0xFFFFu, 0, hlsl, sizeof hlsl) > 0, "decompile txl 3d");
    CHECK(strstr(hlsl, "Texture3D rsx_vtex0") && strstr(hlsl, "rsx_vtex0.SampleLevel(rsx_vsamp0, (") &&
          strstr(hlsl, ").xyz, 0.0)"),
          "3D vertex texture");
    CHECK(compiles(hlsl), "txl 3d compiles");
    rsx_vp_set_vtex_3d_mask(0);
    rsx_vp_set_vtex_cube_mask(1u);
    CHECK(rsx_vp_decompile_program(m, 1, 0, 1u, 0xFFFFu, 0, hlsl, sizeof hlsl) > 0, "decompile txl cube");
    CHECK(strstr(hlsl, "TextureCube rsx_vtex0"), "cube vertex texture");
    CHECK(compiles(hlsl), "txl cube compiles");
    rsx_vp_set_vtex_cube_mask(0);

    /* H2-02: temp writes keep the full 6-bit index (R33 is not R1). */
    memset(m, 0, sizeof m);
    i = base_ins(); i.vec = 1; i.dst_tmp = 33; i.vmask = 0xF; enc(m, &i);
    i = mov_out(0, src_temp(33)); i.end = 1; enc(m + 16, &i);
    CHECK(decompile(m, 2, 0) > 0, "decompile temp33");
    CHECK(strstr(hlsl, "r[33].xyzw = _v") && !strstr(hlsl, "r[1].xyzw = _v"), "temp 33 write stays r[33]");
    CHECK(compiles(hlsl), "temp33 compiles");

    /* H2-07: z-clip snap is floor(d + 0.5) (RSXVertexPrologue.glsl). */
    CHECK(strstr(hlsl, "float _zn = floor(_zd + 0.5);"), "zsnap floor");

    /* H2-01: no CMP_FIXUP on a non-NVIDIA host. */
    memset(m, 0, sizeof m);
    i = mov_out(0, src_in()); i.vec = 0x0B; i.src1 = src_const(); i.cst = 3; i.end = 1; enc(m, &i);
    rsx_vp_set_low_precision_compare(0);
    CHECK(decompile(m, 1, 0) > 0, "decompile slt non-nv");
    CHECK(strstr(hlsl, "float4 vp_cmpfix(float4 a) { return a; }"), "non-NVIDIA compare is plain");
    rsx_vp_set_low_precision_compare(1);

    /* T2-08: UN vertex textures divide by the texture size (baked or queried). */
    memset(m, 0, sizeof m);
    i = mov_out(0, src_in()); i.vec = 0x19; i.end = 1; enc(m, &i);
    {
        const u32 dims[4][2] = { { 256, 128 }, { 0, 0 }, { 0, 0 }, { 0, 0 } };
        rsx_vp_set_vtex_unnorm(1u, dims);
        CHECK(rsx_vp_decompile_program(m, 1, 0, 1u, 0xFFFFu, 0, hlsl, sizeof hlsl) > 0, "decompile txl un");
        CHECK(strstr(hlsl, "return float2(256.0, 128.0);") && strstr(hlsl, ").xy / _rsx_vtex_size0(), 0.0)"),
              "UN vertex texture baked size");
        CHECK(compiles(hlsl), "txl un compiles");
        rsx_vp_set_vtex_unnorm(1u, NULL);
        CHECK(rsx_vp_decompile_program(m, 1, 0, 1u, 0xFFFFu, 0, hlsl, sizeof hlsl) > 0, "decompile txl un dyn");
        CHECK(strstr(hlsl, "rsx_vtex0.GetDimensions(w, h)"), "UN vertex texture queried size");
        CHECK(compiles(hlsl), "txl un dyn compiles");
        rsx_vp_set_vtex_3d_mask(1u);
        CHECK(rsx_vp_decompile_program(m, 1, 0, 1u, 0xFFFFu, 0, hlsl, sizeof hlsl) > 0, "decompile txl un 3d");
        CHECK(!strstr(hlsl, "_rsx_vtex_size0"), "UN ignored on 3D units");
        rsx_vp_set_vtex_3d_mask(0);
        rsx_vp_set_vtex_unnorm(0, NULL);
        CHECK(rsx_vp_decompile_program(m, 1, 0, 1u, 0xFFFFu, 0, hlsl, sizeof hlsl) > 0, "decompile txl norm");
        CHECK(!strstr(hlsl, "_rsx_vtex_size0"), "normalized vertex texture unchanged");
    }
}

static void test_corpus(void)
{
    static const char* dirs[] = {
        "..\\..\\shadercache\\rpcs3\\shaders_cache\\raw",
    };
    int total = 0, ok = 0;
    for (u32 d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        char pat[512];
        snprintf(pat, sizeof pat, "%s\\*.vp", dirs[d]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            char path[600];
            snprintf(path, sizeof path, "%s\\%s", dirs[d], fd.cFileName);
            FILE* f = fopen(path, "rb");
            if (!f) continue;
            static u8 buf[544 * 16];
            const u32 n = (u32)fread(buf, 1, sizeof buf, f);
            fclose(f);
            total++;
            rsx_vp_program_info info;
            const int c = rsx_vp_analyse_program(buf, n / 16u, 0, &info);
            if (c <= 0) { printf("FAIL analyse %s\n", fd.cFileName); s_fail++; continue; }
            /* The one branching program: BRB/BRI back edges, single end at 514. */
            if (!strcmp(fd.cFileName, "AD6A2788EE61A799.vp"))
                CHECK(c == 515 && info.base == 0 && info.has_branch, "ubershader range %d", c);
            if (decompile(buf, n / 16u, 0) <= 0) { printf("FAIL decompile %s\n", fd.cFileName); s_fail++; continue; }
            if (!compiles(hlsl)) { printf("FAIL compile %s\n", fd.cFileName); s_fail++; continue; }
            ok++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    printf("corpus: %d/%d vertex programs ok\n", ok, total);
}

int main(void)
{
    test_rules();
    test_corpus();
    printf(s_fail ? "%d FAIL\n" : "all passed\n", s_fail);
    return s_fail != 0;
}
