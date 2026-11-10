/*
 * Standalone unit test for the RSX fragment-program decompiler.
 *
 * Build (no D3D12 dependency), from this directory:
 *   cc -std=c11 -Wall -Wextra -I../../../include \
 *       test_fp_decompiler.c ../rsx_fp_decompiler.c -o test_fp
 *
 * Exercises the decode path (word swap, source swizzle/type, opcode -> HLSL,
 * dest mask, inline constants) by hand-assembling tiny programs and checking
 * the generated HLSL contains the expected expressions, then the colour
 * export selection: which register file the fragment colour comes out of, and
 * how many SV_TARGETs a program that writes more than one export gets.
 */

#define _POSIX_C_SOURCE 200809L   /* setenv */
#include "../rsx_fp_decompiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

/* Store host word H into guest bytes so that rsx_fp_read_word(p) == H.
 * rsx_fp_read_word does be32-load then 16-bit half-swap, so we invert: the
 * guest stores big-endian of swap16(H). */
static void put_word(u8* p, u32 h)
{
    u32 be = (h << 16) | (h >> 16);
    p[0] = (u8)(be >> 24); p[1] = (u8)(be >> 16);
    p[2] = (u8)(be >> 8);  p[3] = (u8)(be);
}

static void put_float(u8* p, float f)
{
    u32 h; memcpy(&h, &f, 4);
    put_word(p, h);
}

static void check(const char* test, const char* hlsl, const char* needle)
{
    if (strstr(hlsl, needle)) {
        printf("[PASS] %s -- found \"%s\"\n", test, needle);
        g_pass++;
    } else {
        printf("[FAIL] %s -- missing \"%s\"\nHLSL:\n%s\n", test, needle, hlsl);
        g_fail++;
    }
}

static void check_absent(const char* test, const char* hlsl, const char* needle)
{
    if (!strstr(hlsl, needle)) {
        printf("[PASS] %s -- no \"%s\"\n", test, needle);
        g_pass++;
    } else {
        printf("[FAIL] %s -- unexpected \"%s\"\nHLSL:\n%s\n", test, needle, hlsl);
        g_fail++;
    }
}

static void check_true(const char* test, int cond)
{
    if (cond) { printf("[PASS] %s\n", test); g_pass++; }
    else      { printf("[FAIL] %s\n", test); g_fail++; }
}

/* OPDEST/SRC bit helpers (mirror the decoder's macros). */
#define OPC(x)      ((u32)(x) << 24)
#define OUTMASK_ALL (0xFu << 9)
#define END         (1u << 0)
#define OUTREG(i)   ((u32)(i) << 1)
#define OUTHALF     (1u << 7)
#define INSRC(x)    ((u32)(x) << 13)
#define TEXU(x)     ((u32)(x) << 17)
#define SWZ_IDENT   ((0u<<9)|(1u<<11)|(2u<<13)|(3u<<15))
#define T_TEMP      0u
#define T_INPUT     1u
#define T_CONST     2u
#define REG(i)      ((u32)(i) << 2)
/* The execution condition, exec_if lt/eq/gt at [18:20] with its swizzle at
 * [21:28]. All three bits set is unconditional; an all-zero source word is
 * "never", which suppresses the write -- so every instruction below carries
 * this, as a real program's does. */
#define EXEC_ALWAYS ((7u << 18) | (0u << 21) | (1u << 23) | (2u << 25) | (3u << 27))

/* SHADER_CONTROL: 0x40 exports fp32 (colour from r0/r2/r3/r4), clear exports
 * fp16 (colour from h0/h4/h6/h8). */
#define CTRL_32BIT  0x40u
#define CTRL_16BIT  0x00u

int main(void)
{
    char hlsl[16384];

    /* An unnormalised 3D unit scales its float3 coordinate on all three
     * axes (RPCS3 texture_cache.h); a 2D unit keeps the float2 scale. */
    {
        const u32 dim[16][2] = { [1] = { 64, 32 }, [2] = { 32, 32 } };
        const u32 depth[16] = { [2] = 16 };
        snprintf(hlsl, sizeof hlsl,
                 "a = rsx_tex1.Sample(rsx_samp[1], c.xy);\n"
                 "b = rsx_tex2.Sample(rsx_samp[2], c.xyz);\n");
        const int n = rsx_fp_apply_unnorm_scale_vol(hlsl, sizeof hlsl, 0x6u, dim, 0, 0x4u,
                                                    0x4u, depth);
        check_true("unnorm_vol_count", n == 2);
        check("unnorm_vol_2d", hlsl, "rsx_tex1.Sample(rsx_samp[1], c.xy * float2(0.015625, 0.03125))");
        check("unnorm_vol_3d", hlsl,
              "rsx_tex2.Sample(rsx_samp[2],( c.xyz) * float3(0.03125, 0.03125, 0.0625)"
              " + float3(0.0003125, 0.0003125, 0.000625))");
    }
    /* RSX_FP_DEBUG=<hash>:<n>:<reg>: the named register, captured after the
     * program's n-th instruction, becomes colour target 0 with alpha 1 --
     * in that program only. The variable is read once, on the first
     * decompile, so this runs first. */
    {
        u8 prog[32];
        u32 mask_xy = (1u<<9)|(1u<<10);
        put_word(prog + 0, OPC(0x02) | mask_xy | OUTREG(1));          /* MUL r1.xy, r0, r0 */
        put_word(prog + 4, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, T_TEMP | REG(0) | SWZ_IDENT);
        put_word(prog + 12, 0);
        put_word(prog + 16, OPC(0x01) | OUTMASK_ALL | INSRC(1) | END); /* MOV r0, COL0 */
        put_word(prog + 20, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 24, 0);
        put_word(prog + 28, 0);
        char spec[64];
        snprintf(spec, sizeof spec, "%llx:1:r1.xyyy", (unsigned long long)
                 rsx_fp_structural_hash(prog, sizeof prog, 1469598103934665603ull));
        setenv("RSX_FP_DEBUG", spec, 1);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check("fp_debug_capture", hlsl,
              "r[1].xy = _v.xy; }\n"
              "    _rsx_dbg = (float4)((r[1]).xyyy);\n"
              "    { float4 _v = (float4)((saturate(input.col0)).xyzw);");
        check("fp_debug_export", hlsl, "_d.w = 1.0; r[0] = _d; h[0] = _d; }\n"
                                       "    float4 _o = r[0];");
        u8 other[16];
        memcpy(other, prog + 16, sizeof other);
        rsx_fp_decompile(other, sizeof(other), CTRL_32BIT, hlsl, sizeof(hlsl));
        check_absent("fp_debug_other_program", hlsl, "_rsx_dbg");
        unsetenv("RSX_FP_DEBUG");
    }

    /* Test 1: MOV r0, COL0  (write all, END). */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | END); /* MOV, COL0 */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);        /* src0 = INPUT */
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        int n = rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        printf("-- test1 MOV: %d instr\n", n);
        check("mov_dest",   hlsl, "r[0].xyzw =");
        check("mov_input",  hlsl, "input.col0");
        check("mov_entry",  hlsl, "float4 main(PSInput input) : SV_TARGET {");
        check("mov_return", hlsl, "    float4 _o = r[0];\n"
                                  "    return _o;\n}\n");
    }

    /* Test 2: MUL r1.xy, r0, r0  (partial write mask). */
    {
        u8 prog[16];
        u32 mask_xy = (1u<<9)|(1u<<10);
        put_word(prog + 0, OPC(0x02) | mask_xy | OUTREG(1) | END); /* MUL, dest r1 */
        put_word(prog + 4, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, T_TEMP | REG(0) | SWZ_IDENT);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check("mul_mask", hlsl, "r[1].xy =");
        check("mul_expr", hlsl, "(r[0]).xyzw) * ((r[0]).xyzw)");
    }

    /* Test 3: MAD r0, r0, CONST, r0 with inline constant + saturate. */
    {
        u8 prog[32];
        put_word(prog + 0, OPC(0x04) | OUTMASK_ALL | (1u<<31) | END); /* MAD + SAT */
        put_word(prog + 4, T_TEMP  | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, T_CONST | SWZ_IDENT);                      /* src1 CONST */
        put_word(prog + 12, T_TEMP | REG(0) | SWZ_IDENT);
        put_float(prog + 16, 0.5f);  /* inline constant float4 */
        put_float(prog + 20, 0.25f);
        put_float(prog + 24, 0.0f);
        put_float(prog + 28, 1.0f);
        int n = rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        printf("-- test3 MAD: %d instr\n", n);
        check("mad_sat",   hlsl, "_v = _rsx_pc(_v, 0.0, 1.0);");
        check("mad_const", hlsl, "float4(0.5,0.25,0,1)");

        /* ...and the buffered variant, which hoists that constant into b1. */
        u32 nk = 0;
        rsx_fp_decompile_buffered_ex(prog, sizeof(prog), CTRL_32BIT, 0,
                                     hlsl, sizeof(hlsl), &nk);
        check_true("mad_buffered_count", nk == 1);
        check("mad_buffered", hlsl, "fp_constants[0]");
    }

    /* RPCS3 SetDst at Shader Precision High (no native half): an fp16-
     * precision write to an H register goes through clamp16; an R
     * destination is untouched. */
    {
        u8 prog[32];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | OUTHALF | OUTREG(1) | (1u << 22)); /* MOV h1 (prec fp16) */
        put_word(prog + 4, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        put_word(prog + 16, OPC(0x01) | OUTMASK_ALL | OUTREG(2) | (1u << 22) | END); /* MOV r2 (prec fp16) */
        put_word(prog + 20, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 24, 0);
        put_word(prog + 28, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check("half_dest_cast", hlsl, "_v = _rsx_h16(_v); h[1].xyzw = _v.xyzw;");
        check_absent("half_dest_no_c16", hlsl, "_v = _rsx_c16(_v)");
        check("full_dest_plain", hlsl, "((r[0]).xyzw); r[2].xyzw = _v.xyzw;");
    }

    /* Shader Precision High: H registers are float, so a write without fp16
     * precision, an exempt opcode (MAX) and a saturate get no fp16 cast. */
    {
        u8 prog[48];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | OUTHALF | OUTREG(1));                /* MOV h1 (prec 0) */
        put_word(prog + 4, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        put_word(prog + 16, OPC(0x09) | OUTMASK_ALL | OUTHALF | OUTREG(2) | (1u << 22));  /* MAX h2 (prec fp16) */
        put_word(prog + 20, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 24, T_TEMP | REG(1) | SWZ_IDENT);
        put_word(prog + 28, 0);
        put_word(prog + 32, OPC(0x02) | OUTMASK_ALL | OUTHALF | OUTREG(3) | (1u << 22) | (1u << 31) | END); /* MUL_SAT h3 */
        put_word(prog + 36, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 40, T_TEMP | REG(0) | SWZ_IDENT);
        put_word(prog + 44, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check_absent("high_prec_no_half_cast", hlsl, "_rsx_h16(_v)");
        check("high_prec_half_plain", hlsl, "((r[0]).xyzw); h[1].xyzw = _v.xyzw;");
        check("high_prec_sat_half", hlsl, "_v = _rsx_pc(_v, 0.0, 1.0); h[3].xyzw = _v.xyzw;");
    }

    /* Test 4: TEX r0, TC0 from texture unit 3. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | INSRC(4) | TEXU(3) | END); /* TEX */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check("tex_sample", hlsl, "rsx_tex[3].Sample(rsx_samp[3]");
        check("tex_coord",  hlsl, "input.tc0");
    }

    /* Test 5: negate + abs on a source (MOV r0, -|r1|). */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | END);
        put_word(prog + 4, T_TEMP | REG(1) | SWZ_IDENT | (1u<<17) | (1u<<29)
                           | EXEC_ALWAYS); /* neg+abs */
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check("neg_abs", hlsl, "-(abs((r[1]).xyzw))");
    }

    /* Test 6: the export register file follows SHADER_CONTROL, not whichever
     * temp was written. A program writing h0 exports h0 with 16-bit exports
     * and r0 with 32-bit ones. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | OUTHALF | END);
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_16BIT, hlsl, sizeof(hlsl));
        check("h0_16bit", hlsl, "float4 _o = h[0];");
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check("h0_32bit", hlsl, "float4 _o = r[0];");
    }

    /* Test 7: MRT. The second colour export is r2 with 32-bit exports and h4
     * with 16-bit ones, so the same instruction is a second render target in
     * one mode and an ordinary temp write in the other. A deferred pass
     * writing both planes has to come out as a struct of SV_TARGETs; a
     * material shader accumulating into r2 while its colour goes to h0 must
     * not. */
    {
        u8 prog[32];
        put_word(prog +  0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | OUTREG(0));
        put_word(prog +  4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog +  8, 0);
        put_word(prog + 12, 0);
        put_word(prog + 16, OPC(0x01) | OUTMASK_ALL | INSRC(2) | OUTREG(2) | END);
        put_word(prog + 20, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 24, 0);
        put_word(prog + 28, 0);

        int n = rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        printf("-- test7 MRT: %d instr\n", n);
        check("mrt_struct",  hlsl, "struct PSOutput {\n"
                                   "    float4 t0 : SV_TARGET0;\n"
                                   "    float4 t1 : SV_TARGET1;\n"
                                   "};\n");
        check("mrt_entry",   hlsl, "PSOutput main(PSInput input) {");
        check("mrt_t0",      hlsl, "float4 _o0 = r[0];\n"
                                   "    _out.t0 = _o0;");
        check("mrt_t1",      hlsl, "float4 _o1 = r[2];\n"
                                   "    _out.t1 = _o1;");
        check("mrt_return",  hlsl, "    return _out;\n}\n");
        check_absent("mrt_no_target2", hlsl, "SV_TARGET2");

        /* ...and the alpha test still patches it, testing target 0's alpha. */
        check_true("mrt_alpha_patched",
                   rsx_fp_apply_alpha_test(hlsl, sizeof(hlsl), 0x204u, 0.5f) == 1);
        check("mrt_alpha_type", hlsl, "PSOutput _rsx_out = _out;");
        check("mrt_alpha_cmp",  hlsl, "if (!(_rsx_out.t0.a > _rsx_alpha_ref)) discard;");

        /* The same program under 16-bit exports: r2 is a plain temp there, so
         * the colour is h0 alone and nothing about the output changes. */
        rsx_fp_decompile(prog, sizeof(prog), CTRL_16BIT, hlsl, sizeof(hlsl));
        check_absent("mrt_16bit_single", hlsl, "PSOutput");
        check("mrt_16bit_h0", hlsl, "float4 _o = h[0];");
    }

    /* Test 8: all four exports, and the highest written one decides how many
     * targets come out -- r3 alone still declares SV_TARGET0 through 2. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | OUTREG(3) | END);
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check("r3_target0", hlsl, "float4 t0 : SV_TARGET0;");
        check("r3_target2", hlsl, "float4 t2 : SV_TARGET2;");
        check("r3_reads",   hlsl, "float4 _o2 = r[3];");
        check_absent("r3_no_target3", hlsl, "SV_TARGET3");
    }

    /* Test 9: the structural hash separates programs that differ only in
     * which export register they write -- so a pipeline cache keyed on it
     * cannot hand an MRT program the single-target program's shader. It is
     * the instruction bytes that are hashed, and the destination register is
     * one of them; nothing had to be added for the MRT case. */
    {
        u8 single[16], mrt[16];
        put_word(single + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | OUTREG(0) | END);
        put_word(single + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(single + 8, 0);
        put_word(single + 12, 0);
        memcpy(mrt, single, sizeof mrt);
        put_word(mrt + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | OUTREG(2) | END);

        const u64 seed = 1469598103934665603ull;
        const u64 hs = rsx_fp_structural_hash(single, sizeof single, seed);
        const u64 hm = rsx_fp_structural_hash(mrt, sizeof mrt, seed);
        check_true("hash_nonzero", hs != 0 && hm != 0);
        check_true("hash_distinguishes_mrt", hs != hm);
        check_true("hash_stable",
                   hs == rsx_fp_structural_hash(single, sizeof single, seed));
    }

    /* Test 10: IFE with an else branch. The IFE's SRC1/SRC2 hold the else and
     * end addresses in words; each side must land in its own HLSL block, not
     * run one after the other (Crazy Taxi's uber shader drew the city black
     * that way). @0 IFE(cc0.x > 0) else=@32 end=@48; @16 MOV r0,COL0;
     * @32 MOV r0,COL1; @48 LIF r1,r0 END. */
    {
        u8 prog[64];
        const u32 exec_gt_x = (1u << 20);   /* exec_if gr, cond swizzle x */
        put_word(prog +  0, OPC(0x02));
        put_word(prog +  4, exec_gt_x);
        put_word(prog +  8, (1u << 31) | (32u >> 2));   /* branch, else */
        put_word(prog + 12, 48u >> 2);                  /* end */
        put_word(prog + 16, OPC(0x01) | OUTMASK_ALL | INSRC(1));
        put_word(prog + 20, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 24, 0);
        put_word(prog + 28, 0);
        put_word(prog + 32, OPC(0x01) | OUTMASK_ALL | INSRC(2));
        put_word(prog + 36, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 40, 0);
        put_word(prog + 44, 0);
        put_word(prog + 48, OPC(0x3C) | OUTMASK_ALL | OUTREG(1) | END);
        put_word(prog + 52, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 56, 0);
        put_word(prog + 60, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        const char* ife  = strstr(hlsl, "    if (any(cc0.xxxx > 0.0)) {\n");
        const char* then = strstr(hlsl, "input.col0");
        const char* els  = strstr(hlsl, "    } else {\n");
        const char* alt  = strstr(hlsl, "input.col1");
        const char* lif  = strstr(hlsl, "float4(1.0, ");
        check_true("ife_order", ife && then && els && alt && lif &&
                                ife < then && then < els && els < alt && alt < lif);
        check_absent("ife_not_skipped", hlsl, "flow-control op skipped");
        check_absent("lif_handled", hlsl, "unhandled FP opcode");
    }

    /* A unit bound to a 3D texture (SET_TEXTURE_FORMAT dimension 3) is
     * declared Texture3D and sampled with three coordinates; TXP divides
     * them by w. inFAMOUS 2's occlusion volumes look up an 8x8x8 cone LUT. */
    {
        u8 prog[16];
        u32 nk = 0;
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | INSRC(4) | TEXU(0) | END); /* TEX TC0 */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(prog, sizeof prog, CTRL_32BIT, 0, 1u,
                                                             NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check("vol_decl",   hlsl, "Texture3D   rsx_tex0 : register(t0);");
        check("vol_sample", hlsl, "rsx_tex0.Sample(rsx_samp[0], ((input.tc0).xyzw).xyz)");
        put_word(prog + 0, OPC(0x18) | OUTMASK_ALL | INSRC(4) | TEXU(0) | END); /* TXP TC0 */
        rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(prog, sizeof prog, CTRL_32BIT, 0, 1u,
                                                             NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check("vol_txp", hlsl, "rsx_tex0.Sample(rsx_samp[0], ((input.tc0).xyzw).xyz / ((input.tc0).xyzw).w)");
        /* No volume units: the array declaration and 2D sample as before. */
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | INSRC(4) | TEXU(0) | END);
        rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(prog, sizeof prog, CTRL_32BIT, 0, 0,
                                                             NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check("vol_none", hlsl, "rsx_tex[0].Sample(rsx_samp[0], ((input.tc0).xyzw).xy)");
    }

    /* RPCS3 TEX1D / TEX1D_PROJ: a 1D unit reads .x only (x / w projected);
     * the height-1 image is sampled at y = 0.5 so T never contributes. */
    {
        u8 prog[16];
        u32 nk = 0;
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | INSRC(4) | TEXU(0) | END); /* TEX TC0 */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_set_tex_1d_mask(1u);
        rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(prog, sizeof prog, CTRL_32BIT, 0, 0,
                                                             NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check("tex1d_sample", hlsl, "rsx_tex[0].Sample(rsx_samp[0], float2(((input.tc0).xyzw).x, 0.5))");
        put_word(prog + 0, OPC(0x18) | OUTMASK_ALL | INSRC(4) | TEXU(0) | END); /* TXP TC0 */
        rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(prog, sizeof prog, CTRL_32BIT, 0, 0,
                                                             NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check("tex1d_txp", hlsl,
              "rsx_tex[0].Sample(rsx_samp[0], float2(((input.tc0).xyzw).x / ((input.tc0).xyzw).w, 0.5))");
        /* A cube/volume unit wins over the 1D bit. */
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | INSRC(4) | TEXU(0) | END);
        rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(prog, sizeof prog, CTRL_32BIT, 0, 1u,
                                                             NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check("tex1d_vol_wins", hlsl, "rsx_tex0.Sample(rsx_samp[0], ((input.tc0).xyzw).xyz)");
        rsx_fp_set_tex_1d_mask(0);
        rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(prog, sizeof prog, CTRL_32BIT, 0, 0,
                                                             NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check("tex1d_off", hlsl, "rsx_tex[0].Sample(rsx_samp[0], ((input.tc0).xyzw).xy)");
    }

    /* A 4096-byte program fits the D3D12 caller's input bound, but its
     * predicated, CC-writing instructions expand beyond 32 KiB of HLSL.
     * Assemble it here instead of depending on a captured game shader. */
    {
        enum { INSTRUCTION_COUNT = 256 };
        u8 prog[INSTRUCTION_COUNT * 16];
        static char backend_hlsl[RSX_FP_HLSL_BUFFER_SIZE];
        for (int i = 0; i < INSTRUCTION_COUNT; i++) {
            u8* ins = prog + i * 16;
            u32 end = i == INSTRUCTION_COUNT - 1 ? END : 0u;
            u32 opcode = i == 0 ? OPC(0x01) : OPC(0x04);
            u32 condition = i == 0 ? EXEC_ALWAYS : (1u << 20);
            put_word(ins + 0, opcode | OUTMASK_ALL | INSRC(1) |
                             (1u << 8) | end); /* MOV/MAD r0, set CC0 */
            put_word(ins + 4, T_INPUT | SWZ_IDENT | condition);
            put_word(ins + 8, T_TEMP | REG(0) | SWZ_IDENT);
            put_word(ins + 12, T_TEMP | REG(1) | SWZ_IDENT);
        }
        int n = rsx_fp_decompile(prog, sizeof(prog), RSX_FP_CTRL_AUTO,
                                 backend_hlsl, sizeof(backend_hlsl));
        printf("-- source capacity: %d instr, %zu HLSL bytes\n",
               n, strlen(backend_hlsl));
        check_true("backend_source_capacity", n == INSTRUCTION_COUNT);
        check_true("source_exceeds_old_capacity", strlen(backend_hlsl) > 32768u);
        check("large_program_return", backend_hlsl, "    return _o;");
    }

    /* A predicated write is a select, not a blend: MOV r0, COL0 executed
     * only where CC0 > 0 must keep r0 bit for bit in the other components.
     * lerp(r0, v, mask) is arithmetic, so an Inf/NaN in either operand
     * leaks into the components the condition leaves alone. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | END); /* MOV r0, COL0 */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | (1u << 20) |          /* exec_if gt */
                           (0u << 21) | (1u << 23) | (2u << 25) | (3u << 27));
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check("pred_select", hlsl,
              "r[0].xyzw = (_p.xyzw != 0.0) ? _v.xyzw : r[0].xyzw;");
        check_absent("pred_no_lerp", hlsl, "lerp(");
    }

    /* Depth export: SHADER_CONTROL bits 1-3 (0xE) make r1.z the fragment
     * depth. The program must declare SV_Depth and write r1.z, clamped,
     * even with a single colour target; without those bits it keeps the
     * single float4 return. */
    {
        u8 prog[32];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | OUTREG(1) | INSRC(4)); /* MOV r1, TC0 */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        put_word(prog + 16, OPC(0x01) | OUTMASK_ALL | INSRC(1) | END);     /* MOV r0, COL0 */
        put_word(prog + 20, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 24, 0);
        put_word(prog + 28, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT | 0xEu, hlsl, sizeof(hlsl));
        check("depth_export_semantic", hlsl, "    float depth : SV_Depth;\n");
        check("depth_export_entry",    hlsl, "PSOutput main(PSInput input) {");
        check("depth_export_colour",   hlsl, "_out.t0 = _o0;");
        check("depth_export_value",    hlsl, "    float _d = r[1].z;\n"
                                             "    _out.depth = _d;\n");
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check_absent("no_depth_export_semantic", hlsl, "SV_Depth");
        check("no_depth_export_entry", hlsl, "float4 main(PSInput input) : SV_TARGET {");
    }

    /* Sign expansion: a TEX on a unit whose SET_TEXTURE_ADDRESS asks for the
     * biased unsigned remap returns the masked lanes re-centred, and with
     * EXP_TEX (OPDEST bit 21, the _bx2 modifier) as x*2-1 (RPCS3
     * _process_texel). inFAMOUS 2's deferred composite reads its normals
     * G-buffer that way; read unexpanded, normalize() ran on 0..1 encodings
     * and every normal leaned towards (1,1,1). */
    {
        u8 prog[16];
        u8 expand[16] = {0};
        u32 nk = 0;
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | INSRC(4) | TEXU(0) | (1u << 21) | END); /* TEX TC0, EXP_TEX */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        expand[0] = 0x7;   /* R, G, B lanes signed */
        check_true("expand_units", rsx_fp_texture_expand_units(prog, sizeof prog, expand) == 1u);
        rsx_fp_decompile_buffered_shadow_color_expand_ex(prog, sizeof prog, CTRL_32BIT, 0, NULL, NULL,
                                                         expand, hlsl, sizeof hlsl, &nk);
        check("expand_wrapped", hlsl, "_rsx_expand_input(rsx_tex[0].Sample(rsx_samp[0], ((input.tc0).xyzw).xy), 15u, true)");
        check("expand_helper",  hlsl, "float4 _rsx_expand_input(float4 v, uint mask, bool bx2)");
        check("expand_math",    hlsl, "v * 2.0 - 1.0");

        expand[0] = 0;     /* the unit is not biased: _bx2 still expands every lane */
        check_true("expand_units_none", rsx_fp_texture_expand_units(prog, sizeof prog, expand) == 0u);
        check_true("bx2_units", rsx_fp_texture_bx2_units(prog, sizeof prog) == 1u);
        rsx_fp_decompile_buffered_shadow_color_expand_ex(prog, sizeof prog, CTRL_32BIT, 0, NULL, NULL,
                                                         expand, hlsl, sizeof hlsl, &nk);
        check("expand_bx2_unbiased", hlsl, "_rsx_expand_input(rsx_tex[0].Sample(rsx_samp[0], ((input.tc0).xyzw).xy), 15u, true)");
        /* ...and the older entry point, with no masks, is byte-identical. */
        char plain[16384];
        rsx_fp_decompile_buffered_shadow_color_ex(prog, sizeof prog, CTRL_32BIT, 0, NULL, NULL,
                                                  plain, sizeof plain, &nk);
        check_true("expand_old_entry_identical", strcmp(plain, hlsl) == 0);

        /* Biased but no EXP_TEX bit: the fetch still re-centres (RPCS3 applies
         * the remap to every read; only the formula and gamma differ). */
        expand[0] = 0x7;
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | INSRC(4) | TEXU(0) | END);
        check_true("expand_units_nobit", rsx_fp_texture_expand_units(prog, sizeof prog, expand) == 1u);
        rsx_fp_decompile_buffered_shadow_color_expand_ex(prog, sizeof prog, CTRL_32BIT, 0, NULL, NULL,
                                                         expand, hlsl, sizeof hlsl, &nk);
        check("expand_nobit_wrapped", hlsl, "_rsx_expand_input(rsx_tex[0].Sample(rsx_samp[0], ((input.tc0).xyzw).xy), 7u, false)");
        check("expand_nobit_math",    hlsl, "(floor(v * 255.0 + 0.5) - 128.0) / 127.0");

        /* Gamma lanes are decoded, not re-centred: gamma rgb + biased rgba
         * leaves the expansion with the alpha lane only, around the gamma. */
        u8 gamma[16] = {0}; gamma[0] = 0x7; expand[0] = 0xF;
        rsx_fp_decompile_buffered_shadow_color_expand_ex(prog, sizeof prog, CTRL_32BIT, 0, NULL, gamma,
                                                         expand, hlsl, sizeof hlsl, &nk);
        check("expand_after_gamma", hlsl,
              "_rsx_expand_input(_rsx_gamma_input(rsx_tex[0].Sample(rsx_samp[0], ((input.tc0).xyzw).xy), 7u), 8u, false)");
        /* Gamma on every biased lane: nothing left to expand. */
        gamma[0] = 0xF;
        rsx_fp_decompile_buffered_shadow_color_expand_ex(prog, sizeof prog, CTRL_32BIT, 0, NULL, gamma,
                                                         expand, hlsl, sizeof hlsl, &nk);
        check_absent("expand_all_gamma", hlsl, "_rsx_expand_input(rsx_tex");
        /* With EXP_TEX, gamma steps aside and every biased lane is x*2-1. */
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | INSRC(4) | TEXU(0) | (1u << 21) | END);
        rsx_fp_decompile_buffered_shadow_color_expand_ex(prog, sizeof prog, CTRL_32BIT, 0, NULL, gamma,
                                                         expand, hlsl, sizeof hlsl, &nk);
        check("expand_bx2_over_gamma", hlsl, "_rsx_expand_input(rsx_tex[0].Sample(rsx_samp[0], ((input.tc0).xyzw).xy), 15u, true)");
        check_absent("expand_bx2_no_gamma", hlsl, "_rsx_gamma_input(");
        /* Bit 4 (16-bit channels) rides along in the mask argument. */
        expand[0] = 0x13;
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | INSRC(4) | TEXU(0) | END);
        rsx_fp_decompile_buffered_shadow_color_expand_ex(prog, sizeof prog, CTRL_32BIT, 0, NULL, NULL,
                                                         expand, hlsl, sizeof hlsl, &nk);
        check("expand_16bit", hlsl, "_rsx_expand_input(rsx_tex[0].Sample(rsx_samp[0], ((input.tc0).xyzw).xy), 19u, false)");
    }

    /* SRC2 bit 31 (perspective_corr) on a texcoord read multiplies the input
     * by 1/w (RPCS3 FragmentProgramDecompiler: "Perspective correct flag
     * multiplies the result by 1/w"). inFAMOUS 2's water writes uv*w from the
     * vertex program and samples TC2 with the flag set; read raw, uv*w lands
     * far outside the screen copy and the river samples black. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | INSRC(6) | TEXU(0) | END); /* TEX TC2 */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 1u << 31);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check("persp_tc_scaled", hlsl, "(input.tc2 * _rsx_inv_w(input.position))");
        check("persp_helper",    hlsl, "#define _rsx_inv_w(p)");

        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check("persp_off_raw",    hlsl, "((input.tc2).xyzw).xy");
        check_absent("persp_off_no_helper", hlsl, "_rsx_inv_w");
    }

    /* Point sprites (POINT_SPRITE_CONTROL texcoord mask, RPCS3
     * texcoord_is_point_coord): the unit reads the sprite coordinate from a
     * fixed-location input the SPIR-V translation makes BuiltIn PointCoord,
     * and a perspective_corr read of it is not divided. */
    {
        u8 prog[16];
        u32 nk = 0;
        rsx_fp_frag_state fs;
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | INSRC(6) | TEXU(0) | END); /* TEX TC2 */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 1u << 31);
        memset(&fs, 0, sizeof fs);
        fs.point_sprite_mask = 1u << 2;
        rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, NULL, NULL, NULL, &fs,
                                hlsl, sizeof hlsl, &nk);
        check("psprite_decl", hlsl, "    [[vk::location(30)]] float2 _rsx_pcoord : RSX_PCOORD;\n#endif\n");
        check("psprite_read", hlsl, "    input.tc2 = float4(input._rsx_pcoord, 1.0, 0.0);\n#endif\n");
        check_absent("psprite_no_persp", hlsl, "(input.tc2 * _rsx_inv_w(input.position))");
        fs.texcoord_2d_mask = 1u << 2;
        rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, NULL, NULL, NULL, &fs,
                                hlsl, sizeof hlsl, &nk);
        check("psprite_2d", hlsl,
              "    input.tc2 = float4(input._rsx_pcoord, 0.0, 1.0 / _rsx_inv_w(input.position));\n");
        fs.texcoord_2d_mask = 0;
        fs.point_sprite_rmode = 2;
        rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, NULL, NULL, NULL, &fs,
                                hlsl, sizeof hlsl, &nk);
        check("psprite_rmode_ignored", hlsl, "float4(input._rsx_pcoord, 1.0, 0.0)"); /* RPCS3 ignores RMODE */
        fs.point_sprite_mask = 0;
        rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, NULL, NULL, NULL, &fs,
                                hlsl, sizeof hlsl, &nk);
        check_absent("psprite_off", hlsl, "_rsx_pcoord");
        check("psprite_off_persp", hlsl, "(input.tc2 * _rsx_inv_w(input.position))");
    }

    /* RPCS3 FragmentProgramDecompiler op semantics: NRM keeps w = z,
     * DIVSQ's fallback is a, DDX/DDY/DST/LIT/STR/SFL/DP2A/RFL emit code. */
    {
        static const struct { u32 op; const char* name; const char* needle; } ops[] = {
            { 0x39, "nrm_w_is_z",  "_rsx_nrm(((r[0]).xyzw).xyz).xyzz" },
            { 0x3B, "divsq_fallback", ": ((r[0]).xyzw))" },
            { 0x15, "ddx",  "ddx((r[0]).xyzw)" },
            { 0x16, "ddy",  "ddy((r[0]).xyzw)" },
            { 0x07, "dst",  "float4(1.0," },
            { 0x1E, "lit",  "log2(max(" },
            { 0x20, "str",  "(float4)1.0" },
            { 0x21, "sfl",  "(float4)0.0" },
            { 0x2E, "dp2a", "dot(((r[0]).xyzw).xy, ((r[1]).xyzw).xy) + ((r[2]).xyzw).x" },
            { 0x36, "rfl",  "reflect(((r[0]).xyzw), ((r[1]).xyzw))" },
        };
        for (u32 i = 0; i < sizeof ops / sizeof ops[0]; i++) {
            u8 prog[16];
            put_word(prog + 0, OPC(ops[i].op) | OUTMASK_ALL | OUTREG(3) | END);
            put_word(prog + 4, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
            put_word(prog + 8, T_TEMP | REG(1) | SWZ_IDENT);
            put_word(prog + 12, T_TEMP | REG(2) | SWZ_IDENT);
            rsx_fp_decompile(prog, sizeof prog, CTRL_32BIT, hlsl, sizeof hlsl);
            check(ops[i].name, hlsl, ops[i].needle);
            check_absent(ops[i].name, hlsl, "TODO");
        }
    }

    /* Texture fetch paths (RPCS3 insert_texture_fetch): a comparison unit
     * maps TXB/TXL/TXD to the shadow compare instead of failing the program;
     * TXD is SampleGrad with the two gradient operands; a cube TXP divides
     * by w. */
    {
        u8 prog[16];
        u32 nk = 0;
        rsx_fp_shadow_desc shadow[16];
        memset(shadow, 0, sizeof shadow);
        shadow[0].enabled = 1;
        shadow[0].func = 1;
        static const u32 shops[3] = { 0x31, 0x2F, 0x19 };
        for (u32 i = 0; i < 3; i++) {
            put_word(prog + 0, OPC(shops[i]) | OUTMASK_ALL | INSRC(4) | TEXU(0) | END);
            put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
            put_word(prog + 8, T_TEMP | REG(1) | SWZ_IDENT);
            put_word(prog + 12, T_TEMP | REG(2) | SWZ_IDENT);
            const int n = rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, shadow,
                                                  NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
            check_true("shadow_txb_txl_txd_ok", n >= 0);
            check("shadow_txb_txl_txd", hlsl, "_rsx_shadow0(((input.tc0).xyzw).xyz)");
        }
        put_word(prog + 0, OPC(0x19) | OUTMASK_ALL | INSRC(4) | TEXU(0) | END); /* TXD */
        rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, NULL,
                                NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check("txd_grad", hlsl,
              "SampleGrad(rsx_samp[0], ((input.tc0).xyzw).xy, ((r[1]).xyzw).xy, ((r[2]).xyzw).xy)");
        put_word(prog + 0, OPC(0x18) | OUTMASK_ALL | INSRC(4) | TEXU(0) | END); /* TXP */
        rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 1u, 0, NULL,
                                NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check("txp_cube_divides", hlsl, ".xyz / (");
    }

    /* T2-01/T2-02: RPCS3 shadow compare on float depth (unclamped reference),
     * cube units (samplerCubeShadow, reference in w), every filter; forms
     * RPCS3 cannot express (3D, cube TXP) sample plainly instead of failing
     * the program. The compare takes an implicit LOD. */
    {
        u8 prog[16];
        u32 nk = 0;
        rsx_fp_shadow_desc shadow[16];
        memset(shadow, 0, sizeof shadow);
        shadow[0].enabled = 1;
        shadow[0].func = 3;
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | INSRC(4) | TEXU(0) | END); /* TEX */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        int n = rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, shadow,
                                        NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check_true("shadow_int_ok", n >= 0);
        check("shadow_int_ref_clamped", hlsl, "float reference = saturate(coordinate.z);");
        check("shadow_implicit_lod", hlsl, "_lod = rsx_tex[0].CalculateLevelOfDetail(rsx_samp[0], uv * rsx_shscale.xy);");
        check("shadow_lod_sample", hlsl, "SampleLevel(rsx_samp[0], uv, _lod)");
        shadow[0].depth_float = 1;
        rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, shadow,
                                NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check("shadow_float_ref_unclamped", hlsl, "float reference = coordinate.z;");
        shadow[0].linear = 1;
        rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, shadow,
                                NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check("shadow_linear_lod_level", hlsl, "GetDimensions(0u, _w0, _h0, _nl)");
        check("shadow_linear_lod_tap", hlsl, "/ _ls, _lv).r");
        shadow[0].depth_float = 0;
        shadow[0].linear = 0;
        n = rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 1u, 0, shadow,
                                    NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check_true("shadow_cube_ok", n >= 0);
        check("shadow_cube_call", hlsl, "_rsx_shadow0(((input.tc0).xyzw))");
        check("shadow_cube_helper", hlsl, "float4 _rsx_shadow0(float4 coordinate)");
        check("shadow_cube_ref_w", hlsl, "float reference = saturate(coordinate.w);");
        put_word(prog + 0, OPC(0x18) | OUTMASK_ALL | INSRC(4) | TEXU(0) | END); /* TXP */
        n = rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 1u, 0, shadow,
                                    NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check_true("shadow_cube_txp_ok", n >= 0);
        check("shadow_cube_txp_plain", hlsl, ".xyz / (");
        check_absent("shadow_cube_txp_no_compare", hlsl, "_rsx_shadow0((");
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | INSRC(4) | TEXU(0) | END); /* TEX, 3D */
        n = rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 1u, shadow,
                                    NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check_true("shadow_3d_ok", n >= 0);
        check_absent("shadow_3d_plain", hlsl, "_rsx_shadow");
        shadow[0].func = 9;
        n = rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, shadow,
                                    NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check_true("shadow_bad_func_ok", n >= 0);
        check_absent("shadow_bad_func_plain", hlsl, "_rsx_shadow");
        shadow[0].func = 3;
        put_word(prog + 0, OPC(0x33) | OUTMASK_ALL | INSRC(4) | TEXU(0) | END); /* TEXBEM */
        n = rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, shadow,
                                    NULL, NULL, NULL, hlsl, sizeof hlsl, &nk);
        check_true("shadow_texbem_ok", n >= 0);

        /* Metadata: D24F + CONVOLUTION_MIN compares linearly; 3D / func 8+ do not compare. */
        rsx_fp_shadow_desc d;
        rsx_fp_shadow_desc_from_texture(&d, 0x93u, 2, 0, 1, 7u << 16 | 1u << 24, 3u << 28, 0x80000000u);
        check_true("shadow_meta_float", d.enabled == 1 && d.depth_float == 1 && d.linear == 1 && d.func == 3);
        rsx_fp_shadow_desc_from_texture(&d, 0x92u, 2, 1, 1, 1u << 16 | 1u << 24, 3u << 28, 0x80000000u);
        check_true("shadow_meta_cube", d.enabled == 1 && !d.linear && !d.depth_float);
        rsx_fp_shadow_desc_from_texture(&d, 0x92u, 3, 0, 1, 1u << 16 | 1u << 24, 3u << 28, 0x80000000u);
        check_true("shadow_meta_3d_off", d.enabled == 0);
        rsx_fp_shadow_desc_from_texture(&d, 0x92u, 2, 0, 1, 1u << 16 | 1u << 24, 9u << 28, 0x80000000u);
        check_true("shadow_meta_func8_off", d.enabled == 0);
    }

    /* _bx2 (EXP_TEX) per RPCS3 _enable_texture_expand: no effect on a format
     * without biased renormalisation (gamma stays), and on a biased one only
     * the remap's texture-sourced lanes expand. */
    {
        u8 prog[16];
        u32 nk = 0;
        u8 gamma[16] = {0};
        rsx_fp_frag_state fs;
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | INSRC(4) | TEXU(0) | (1u << 21) | END);
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        memset(&fs, 0, sizeof fs);
        gamma[0] = 0x7;
        fs.bx2_lanes[0] = 0x20;
        rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, NULL, gamma, NULL, &fs,
                                hlsl, sizeof hlsl, &nk);
        check_absent("bx2_unbiased_no_expand", hlsl, "_rsx_expand_input(rsx_tex");
        check("bx2_unbiased_keeps_gamma", hlsl, "_rsx_gamma_input(");
        fs.bx2_lanes[0] = 0x10 | 0x7;
        rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, NULL, gamma, NULL, &fs,
                                hlsl, sizeof hlsl, &nk);
        check("bx2_biased_lanes", hlsl, ", 7u, true)");
        check_absent("bx2_biased_erases_gamma", hlsl, "_rsx_gamma_input(rsx_tex");
    }

    /* RPCS3 loops: for (iN = 0; iN < rep_count; iN++), and an input read
     * with SRC2.use_index_reg inside one is _indexed_load(iN + addr_reg). */
    {
        u8 prog[48];
        put_word(prog + 0, OPC(0x03) | OUTMASK_ALL);                   /* LOOP */
        put_word(prog + 4, EXEC_ALWAYS);
        put_word(prog + 8, (1u << 31) | (4u << 2) | (3u << 10) | (2u << 19));
        put_word(prog + 12, 32u >> 2);
        put_word(prog + 16, OPC(0x03) | OUTMASK_ALL | INSRC(4));       /* ADD r0, r0, TC[i+1] */
        put_word(prog + 20, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 24, T_INPUT | SWZ_IDENT);
        put_word(prog + 28, (1u << 30) | (1u << 19));
        put_word(prog + 32, OPC(0x01) | OUTMASK_ALL | INSRC(1) | OUTREG(1) | END);
        put_word(prog + 36, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 40, 0);
        put_word(prog + 44, 0);
        rsx_fp_decompile(prog, sizeof prog, CTRL_32BIT, hlsl, sizeof hlsl);
        check("loop_rep_count", hlsl, "for (int _l0 = 0; _l0 < 4; _l0++)");
        check("indexed_load", hlsl, "_rsx_iload(input, _l0 + 1)");
        check("indexed_helper", hlsl, "return float4(0.0, 0.0, 0.0, 1.0);");
        check("indexed_tc9_declared", hlsl, "float4 tc9:TEXCOORD9;");
        check_true("indexed_input_mask", rsx_fp_input_mask(prog, sizeof prog) == ((0x3FFu << 4) | 2u));
    }

    /* Source type 3 is not a register file: RPCS3 marks the ucode invalid
     * and the program discards. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | END);
        put_word(prog + 4, 3u | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof prog, CTRL_32BIT, hlsl, sizeof hlsl);
        check("src_type3_discard", hlsl, "discard");
    }

    /* ALPHA_REF by surface format, RPCS3 rsx_methods.h alpha_ref():
     * W16Z16Y16X16 (0xB) fp16, W32Z32Y32X32 (0xC) / X32 (0xD) bit-cast,
     * A8B8G8R8 (0x10) / X8B8G8R8 (0xE) and the rest 8-bit unorm. */
    {
        const float one = 1.0f; u32 one_bits; memcpy(&one_bits, &one, 4);
        check_true("alpha_ref_fp16", rsx_fp_alpha_ref(0x12343C00u, 0x0Bu) == 1.0f);
        check_true("alpha_ref_fp32", rsx_fp_alpha_ref(one_bits, 0x0Cu) == 1.0f);
        check_true("alpha_ref_x32", rsx_fp_alpha_ref(one_bits, 0x0Du) == 1.0f);
        check_true("alpha_ref_a8b8g8r8", rsx_fp_alpha_ref(0x3C80u, 0x10u) == 128.0f / 255.0f);
        check_true("alpha_ref_x8b8g8r8", rsx_fp_alpha_ref(0x3C80u, 0x0Eu) == 128.0f / 255.0f);
        check_true("alpha_ref_a8r8g8b8", rsx_fp_alpha_ref(0x1FFu, 0x08u) == 1.0f);
    }

    /* RPCS3 NVIDIA FLR: _builtin_floor(x) = floor(x + 0.000001)
     * (FragmentProgramDecompiler.cpp:972-983, has_low_precision_rounding). */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x11) | OUTMASK_ALL | INSRC(1) | END);   /* FLR r0, COL0 */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof prog, CTRL_32BIT, hlsl, sizeof hlsl);
        check("flr_nv_bias", hlsl, ") + 0.000001)");
    }

    /* RET (RPCS3 AddFlowOp("return") inside fs_main): the body moves into
     * _rsx_fs_main over static registers, main still exports after it. */
    {
        u8 prog[32];
        put_word(prog +  0, OPC(0x05));                                 /* RET if cc0.x > 0 */
        put_word(prog +  4, 1u << 20);
        put_word(prog +  8, 1u << 31);
        put_word(prog + 12, 0);
        put_word(prog + 16, OPC(0x01) | OUTMASK_ALL | INSRC(1) | END);  /* MOV r0, COL0 */
        put_word(prog + 20, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 24, 0);
        put_word(prog + 28, 0);
        rsx_fp_decompile(prog, sizeof prog, CTRL_32BIT, hlsl, sizeof hlsl);
        const char* fn = strstr(hlsl, "void _rsx_fs_main(PSInput input) {\n");
        const char* ret = strstr(hlsl, "    if (any(cc0.xxxx > 0.0)) return;\n");
        const char* mn = strstr(hlsl, "float4 main(PSInput input) : SV_TARGET {\n");
        const char* call = strstr(hlsl, "    _rsx_fs_main(input);\n");
        const char* exp = strstr(hlsl, "    float4 _o = r[0];\n    return _o;\n}\n");
        check("ret_static_regs", hlsl, "static float4 r[48]; static float4 h[48];\n");
        check_true("ret_layout", fn && ret && mn && call && exp &&
                                 fn < ret && ret < mn && mn < call && call < exp);
        check_absent("ret_not_skipped", hlsl, "flow-control op skipped");
        check_absent("ret_marker_gone", hlsl, "_rsx_fs_main_end");
        check_true("ret_alpha_test", rsx_fp_apply_alpha_test(hlsl, sizeof hlsl, 0x201u, 0.5f) == 1);
        /* a program without RET keeps the single-function text */
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | END);
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        rsx_fp_decompile(prog, 16, CTRL_32BIT, hlsl, sizeof hlsl);
        check_absent("no_ret_no_split", hlsl, "_rsx_fs_main");
    }

    /* Branch join (RPCS3 RegisterDependencyPass.cpp:373-444): h1 written on
     * the if path, r0 on the else path, r0 read after the join -- the if
     * path ends with the r0.zw <- h1 barrier. */
    {
        u8 prog[64];
        put_word(prog +  0, OPC(0x02));                                 /* IFE cc0.x > 0 */
        put_word(prog +  4, 1u << 20);
        put_word(prog +  8, (1u << 31) | (32u >> 2));
        put_word(prog + 12, 48u >> 2);
        put_word(prog + 16, OPC(0x01) | OUTMASK_ALL | OUTHALF | OUTREG(1) | INSRC(1)); /* MOV h1, COL0 */
        put_word(prog + 20, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 24, 0);
        put_word(prog + 28, 0);
        put_word(prog + 32, OPC(0x01) | OUTMASK_ALL | INSRC(2));        /* MOV r0, COL1 */
        put_word(prog + 36, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 40, 0);
        put_word(prog + 44, 0);
        put_word(prog + 48, OPC(0x01) | OUTMASK_ALL | OUTREG(2) | END); /* MOV r2, r0 */
        put_word(prog + 52, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 56, 0);
        put_word(prog + 60, 0);
        rsx_fp_decompile(prog, sizeof prog, CTRL_32BIT, hlsl, sizeof hlsl);
        const char* h1w = strstr(hlsl, "input.col0");
        const char* bz = strstr(hlsl, "    r[0].z = asfloat((asuint(r[0].z) & 0xFFFF0000u) | (f32tof16(h[1].x) & 0xFFFFu));\n");
        const char* bw = strstr(hlsl, "    r[0].w = asfloat((asuint(r[0].w) & 0xFFFFu) | (f32tof16(h[1].w) << 16));\n");
        const char* els = strstr(hlsl, "    } else {\n");
        check_true("join_barrier_in_if_path", h1w && bz && bw && els && h1w < bz && bz < els && bw < els);
        check_absent("join_no_xy_barrier", hlsl, "r[0].x = asfloat");
        check_absent("join_markers_stripped", hlsl, "/*@B");
    }

    /* Point/line draws on NVIDIA (low_precision_tests): set ops and the
     * alpha test compare CMP_FIXUP(a) = sign(a) * 16 + a. */
    {
        u8 prog[16];
        u32 nk = 0;
        rsx_fp_frag_state fs;
        put_word(prog + 0, OPC(0x0A) | OUTMASK_ALL | INSRC(1) | END);   /* SLT r0, COL0, r0 */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, T_TEMP | REG(0) | SWZ_IDENT);
        put_word(prog + 12, 0);
        memset(&fs, 0, sizeof fs);
        rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, NULL, NULL, NULL, &fs,
                                hlsl, sizeof hlsl, &nk);
        check_absent("cmpfix_off", hlsl, "_rsx_cmpfix");
        fs.low_precision_tests = 1;
        rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, NULL, NULL, NULL, &fs,
                                hlsl, sizeof hlsl, &nk);
        check("cmpfix_define", hlsl, "#define _rsx_cmpfix(a) (sign(a) * 16.0 + (a))\n");
        check("cmpfix_slt", hlsl, "(float4)(_rsx_cmpfix(");
        check_true("cmpfix_alpha_applied", rsx_fp_apply_alpha_test_buffered(hlsl, sizeof hlsl, 0x201u) == 1);
        check("cmpfix_alpha", hlsl, "_rsx_cmpfix(_rsx_out.a) < _rsx_cmpfix(_rsx_alpha_ref)");
        /* H2-01: RPCS3 gates both the compare fixup and the FLR bias on an
         * NVIDIA host. */
        fs.host_not_nvidia = 1;
        rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, NULL, NULL, NULL, &fs,
                                hlsl, sizeof hlsl, &nk);
        check_absent("cmpfix_not_nv", hlsl, "_rsx_cmpfix");
        put_word(prog + 0, OPC(0x11) | OUTMASK_ALL | INSRC(1) | END);   /* FLR r0, COL0 */
        rsx_fp_decompile_engine(prog, sizeof prog, CTRL_32BIT, 0, 0, NULL, NULL, NULL, &fs,
                                hlsl, sizeof hlsl, &nk);
        check_absent("flr_not_nv_no_bias", hlsl, "0.000001");
        check("flr_not_nv_floor", hlsl, "floor(");
    }

    printf("\n===========================================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
