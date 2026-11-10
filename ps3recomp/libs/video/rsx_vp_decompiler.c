/*
 * ps3recomp - RSX Vertex Program (NV40 ISA) -> HLSL decompiler
 *
 * See rsx_vp_decompiler.h. The 4-word instruction encoding (D0..D3 fields,
 * source operand layout, opcode numbers) follows the public NV40 vertex
 * program layout as documented in the NV_vertex_program3 extension spec and
 * the envytools rnndb; RPCS3 was consulted as a READ-ONLY semantics oracle
 * for the field positions and the dest-write rules (no code copied).
 *
 * Field facts used (verified against the oracle 2026-07-03):
 *   D0: mask_w/z/y/x pairs [2:9] are the CC swizzle, cond [10:12],
 *       cond_test_enable [13], dst_tmp [15:20], src0/1/2_abs [21:23],
 *       addr_reg_sel [24], saturate [26], vec_result [30]
 *   D1: src0h [0:7], input_src [8:11], const_src [12:21],
 *       vec_opcode [22:26], sca_opcode [27:31]
 *   D2: src2h [0:5], src1 [6:22], src0l [23:31]
 *   D3: end [0], index_const [1], dst [2:6], sca_dst_tmp [7:12],
 *       vec_writemask w/z/y/x [13:16], sca_writemask w/z/y/x [17:20],
 *       src2l [21:31]
 *   SRC (17 bits): reg_type [0:1] (1 = temp, 2 = input, 3 = constant),
 *       tmp_src [2:7], swz w/z/y/x [8:15] (2 bits each), neg [16]
 *   Dest rules: the VEC result goes to output o[D3.dst] when D0.vec_result
 *       (dst 0x1F = none) AND to temp r[D0.dst_tmp] (0x3F = none); the SCA
 *       result goes to o[D3.dst] when !D0.vec_result, else to
 *       r[D3.sca_dst_tmp]. The SCA unit reads SRC2.
 *   VEC ADD reads src0 + src2 (not src1).
 *   Output register map: o0 = HPOS, o1 = COL0, o2 = COL1, o3/o4 = back-face
 *       colors, o5.x = FOG, o6 = point size / TEX9, o7..o14 = TEX0..7,
 *       o15 = TEX8. Unwritten outputs default to (0,0,0,1).
 */
#include "rsx_vp_decompiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- little-endian word read (RPCS3-capture native order) --------------- */
static u32 rd_le(const u8* p)
{ return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24); }

/* ---- opcode names ------------------------------------------------------ */
const char* rsx_vp_vec_name(u32 op)
{
    static const char* n[] = {"NOP","MOV","MUL","ADD","MAD","DP3","DPH","DP4",
        "DST","MIN","MAX","SLT","SGE","ARL","FRC","FLR","SEQ","SFL","SGT","SLE",
        "SNE","STR","SSG","?","?","TXL"};
    return op < 26 ? n[op] : "?";
}
const char* rsx_vp_sca_name(u32 op)
{
    static const char* n[] = {"NOP","MOV","RCP","RCC","RSQ","EXP","LOG","LIT",
        "BRA","BRI","CAL","CLI","RET","LG2","EX2","SIN","COS","BRB","CLB","PSH","POP"};
    return op < 21 ? n[op] : "?";
}

u32 rsx_vp_program_size_instrs(const u8* ucode, u32 max_bytes)
{
    if (!ucode) return 0;
    u32 off = 0, instrs = 0;
    while (off + 16 <= max_bytes) {
        u32 d3 = rd_le(ucode + off + 12);
        instrs++;
        off += 16;
        if (d3 & 1u) return instrs;   /* D3.end */
    }
    return 0;
}

/* ---- output appender --------------------------------------------------- */
typedef struct { char* p; u32 cap; u32 len; int overflow; } Out;
static void emit(Out* o, const char* s)
{
    u32 n = (u32)strlen(s);
    if (o->len + n + 1 > o->cap) { o->overflow = 1; return; }
    memcpy(o->p + o->len, s, n); o->len += n; o->p[o->len] = '\0';
}

/* ---- source operand ---------------------------------------------------- */
static const char SWZ[4] = {'x','y','z','w'};

/* Build the HLSL expression for one 17-bit source field.
 * reg_type: 1 = temp r[], 2 = input v[], 3 = constant vp_c[]. */
static void emit_src(u32 src, int is_abs, u32 input_src, u32 const_src,
                     int index_const, u32 addr_sel, u32 addr_swz,
                     char* buf, u32 bufsz)
{
    u32 reg_type = src & 3;
    u32 tmp_src  = (src >> 2) & 0x3F;
    u32 sw = (src >> 8) & 3, sz = (src >> 10) & 3;
    u32 sy = (src >> 12) & 3, sx = (src >> 14) & 3;
    int neg = (src >> 16) & 1;

    char base[64];
    switch (reg_type) {
    case 2:  snprintf(base, sizeof(base), "v[%u]", input_src & 15); break;
    case 3:
        /* RPCS3 GetSRC: a direct constant past 467 reads float4(0) with no
         * swizzle/abs/neg; an indexed one wraps at 512 and reads 0 past 467
         * (_fetch_indexed_constant). */
        if (index_const)
            snprintf(base, sizeof(base), "vp_cidx((%uu + (uint)a%u.%c) & 511u)",
                     const_src, addr_sel & 1, SWZ[addr_swz & 3]);
        else if (const_src >= 468u) {
            snprintf(buf, bufsz, "float4(0,0,0,0)");
            return;
        } else
            snprintf(base, sizeof(base), "vp_c[%u]", const_src);
        break;
    default: snprintf(base, sizeof(base), "r[%u]", tmp_src & 63); break;
    }

    char swz[8];
    swz[0]='.'; swz[1]=SWZ[sx]; swz[2]=SWZ[sy]; swz[3]=SWZ[sz]; swz[4]=SWZ[sw]; swz[5]='\0';

    const char* pre = neg ? "-" : "";
    if (is_abs) snprintf(buf, bufsz, "%sabs(%s)%s", pre, base, swz);
    else        snprintf(buf, bufsz, "%s(%s)%s", pre, base, swz);
}

/* writemask string from 4 bits (x,y,z,w order) -> "xyzw" subset */
static void mask_str(int mx,int my,int mz,int mw, char* m)
{
    int i=0;
    if (mx) m[i++]='x';
    if (my) m[i++]='y';
    if (mz) m[i++]='z';
    if (mw) m[i++]='w';
    m[i]='\0';
}

/* Emit "    dst.mask = src.mask;" (skips when mask empty). */
static void emit_store_src(Out* b, const char* dst_fmt, u32 idx, const char* m, const char* src)
{
    if (!m[0]) return;
    char line[96];
    char dst[32];
    snprintf(dst, sizeof(dst), dst_fmt, idx);
    snprintf(line, sizeof(line), "        %s.%s = %s.%s;\n", dst, m, src, m);
    emit(b, line);
}

/* ---- flow control -------------------------------------------------------- */
/* RPCS3 rsx::max_vertex_program_instructions. At 512, a branching program
 * longer than that (R3 91D15F3FE80BE076, 543 slots) never opened blocks past
 * slot 512 while its branches still closed them: unbalanced braces. */
#define RSX_VP_MAX_INSTR 544

static u32 s_vp_branch_base = 0;   /* VP slot the program being decompiled starts at */
void rsx_vp_set_branch_base(u32 start_slot) { s_vp_branch_base = start_slot; }
static int s_vp_clip_distances = 0;
void rsx_vp_set_clip_distances(int enabled) { s_vp_clip_distances = enabled != 0; }
/* RPCS3 VERTEX_TEXTURE_FETCH1D: a 1D vertex unit reads coordinate .x only. */
static u32 s_vp_vtex_1d_mask;
void rsx_vp_set_vtex_1d_mask(u32 mask) { s_vp_vtex_1d_mask = mask; }
/* RPCS3 VERTEX_TEXTURE_FETCH3D / FETCHCUBE (GLSLCommon.cpp:647-649): 3D and
 * cube vertex units read coordinate .xyz from a sampler3D / samplerCube
 * (VertexProgramDecompiler.cpp:220-240). */
static u32 s_vp_vtex_3d_mask, s_vp_vtex_cube_mask;
void rsx_vp_set_vtex_3d_mask(u32 mask) { s_vp_vtex_3d_mask = mask; }
void rsx_vp_set_vtex_cube_mask(u32 mask) { s_vp_vtex_cube_mask = mask; }
/* H2-01: RPCS3 VKVertexProgram.cpp:367 low_precision_tests = is_NVIDIA. */
static int s_vp_low_precision = 1;
void rsx_vp_set_low_precision_compare(int enabled) { s_vp_low_precision = enabled != 0; }
/* T2-08: RPCS3 VKDraw.cpp:593-619 vertex samplers take unnormalizedCoordinates
 * from CELL_GCM_TEXTURE_UN. 1D/2D units in the mask divide the coordinate by
 * the texture size: the baked guest size when given, else GetDimensions. */
static u32 s_vp_vtex_un_mask, s_vp_vtex_un_dim[4][2];
void rsx_vp_set_vtex_unnorm(u32 mask, const u32 dims[4][2])
{
    s_vp_vtex_un_mask = mask & 15u;
    for (u32 u = 0; u < 4; u++) {
        s_vp_vtex_un_dim[u][0] = dims ? dims[u][0] : 0u;
        s_vp_vtex_un_dim[u][1] = dims ? dims[u][1] : 0u;
    }
}
static int vp_env_on(const char* name, int* cache)
{
    if (*cache < 0) { const char* e = getenv(name); *cache = !(e && *e == '0'); }
    return *cache;
}
/* H2-02: RPCS3 keeps all 6-bit temps distinct; RSX_VP_TEMP64=0 aliases
 * writes to R32+ onto R0+ again. */
static int vp_temp64(void) { static int c = -1; return vp_env_on("RSX_VP_TEMP64", &c); }
static int vp_vtex_un(void) { static int c = -1; return vp_env_on("RSX_VP_VTEX_UN", &c); }
/* H2-07: RSXVertexPrologue.glsl snaps with floor(d + 0.5); RSX_VP_ZSNAP_FLOOR=0
 * restores round(). */
static int vp_zsnap_floor(void) { static int c = -1; return vp_env_on("RSX_VP_ZSNAP_FLOOR", &c); }

/* BRA BRI CAL CLI RET BRB CLB (scalar opcodes). */
static int vp_is_flow(u32 sca_op)
{
    return sca_op == 0x08 || sca_op == 0x09 || sca_op == 0x0A || sca_op == 0x0B ||
           sca_op == 0x0C || sca_op == 0x11 || sca_op == 0x12;
}

/* Branch target (NV40 layout, RPCS3 as oracle): D0[23] << 9 | D2[0:5] << 3 |
 * D3[29:31], an absolute VP slot; made relative to the program start. */
static u32 vp_branch_target_from(u32 d0, u32 d2, u32 d3, u32 base)
{
    const u32 t = (((d0 >> 23) & 1u) << 9) | ((d2 & 0x3Fu) << 3) | ((d3 >> 29) & 7u);
    return t >= base ? t - base : t;
}

/* Condition-code state for one instruction (D0). The NV40 VP has two CC
 * registers; a write under cond-test lands only in the components whose
 * CC component (through the D0 CC swizzle) compares true against zero.
 * cond: 0 = never, 1 LT, 2 EQ, 3 LE, 4 GT, 5 NE, 6 GE, 7 = always. */
typedef struct { int test; u32 cond, sel; char swz[4]; } CondCtx;

/* Emit the store, predicated per component when a cond test is active. */
static void emit_store_cc_src(Out* b, const char* dst_fmt, u32 idx, const char* m,
                              const CondCtx* cc, const char* src)
{
    if (!cc->test) { emit_store_src(b, dst_fmt, idx, m, src); return; }
    if (cc->cond == 0) return;                      /* FL: never writes */
    static const char* cmp[8] = { 0, "<", "==", "<=", ">", "!=", ">=", 0 };
    char dst[32];
    snprintf(dst, sizeof(dst), dst_fmt, idx);
    for (const char* c = m; *c; c++) {
        const int ci = (*c == 'x') ? 0 : (*c == 'y') ? 1 : (*c == 'z') ? 2 : 3;
        char line[160];
        snprintf(line, sizeof(line), "        if (cc[%u].%c %s 0.0) %s.%c = %s.%c;\n",
                 cc->sel, cc->swz[ci], cmp[cc->cond], dst, *c, src, *c);
        emit(b, line);
    }
}
static void emit_store_cc(Out* b, const char* dst_fmt, u32 idx, const char* m, const CondCtx* cc)
{
    emit_store_cc_src(b, dst_fmt, idx, m, cc, "_v");
}

static u32 vec_source_use(u32 op, int* exact)
{
    switch (op) {
    case 0x00: return 0;       /* NOP */
    case 0x01: return 1u;      /* src0 */
    case 0x02: return 3u;      /* src0, src1 */
    case 0x03: return 5u;      /* src0, src2 */
    case 0x04: return 7u;      /* src0, src1, src2 */
    case 0x05: case 0x06: case 0x07: case 0x08:
    case 0x09: case 0x0A: case 0x0B: case 0x0C:
    case 0x10: case 0x12: case 0x13: case 0x14:
        return 3u;             /* src0, src1 */
    case 0x0D: case 0x0E: case 0x0F: case 0x16: case 0x19:
        return 1u;             /* src0 */
    case 0x11: case 0x15:
        return 0;              /* source-independent constants */
    default:
        *exact = 0;
        return 0;
    }
}

static u32 sca_source_use(u32 op, int* exact)
{
    switch (op) {
    case 0x00:
        return 0;
    case 0x01: case 0x02: case 0x03: case 0x04:
    case 0x05: case 0x06: case 0x07:
    case 0x0D: case 0x0E: case 0x0F: case 0x10:
        return 4u;             /* scalar ALU reads src2 */
    default:
        /* Flow control and currently unsupported scalar opcodes make a
         * narrowed data-flow result unsafe. */
        *exact = 0;
        return 0;
    }
}

int rsx_vp_analyze_inputs(const u8* ucode, u32 max_bytes,
                          rsx_vp_input_analysis* analysis)
{
    if (!ucode || !analysis) return -1;
    analysis->input_mask = 0;
    analysis->exact = 1;
    u32 off = 0;
    int instrs = 0;
    int ended = 0;
    while (off + 16 <= max_bytes) {
        const u32 d1 = rd_le(ucode + off + 4);
        const u32 d2 = rd_le(ucode + off + 8);
        const u32 d3 = rd_le(ucode + off + 12);
        off += 16;
        instrs++;

        const u32 vec_op = (d1 >> 22) & 0x1F;
        const u32 sca_op = (d1 >> 27) & 0x1F;
        const u32 input_src = (d1 >> 8) & 0xF;
        const u32 src0 = ((d2 >> 23) & 0x1FF) | ((d1 & 0xFF) << 9);
        const u32 src1 = (d2 >> 6) & 0x1FFFF;
        const u32 src2 = ((d3 >> 21) & 0x7FF) | ((d2 & 0x3F) << 11);
        const u32 sources[3] = {src0, src1, src2};
        /* An empty write mask is a full one (RPCS3 GetMask); flow control
         * makes the straight-line walk inexact. */
        u32 use = 0;
        if (vec_op)
            use |= vec_source_use(vec_op, &analysis->exact);
        if (sca_op)
            use |= sca_source_use(sca_op, &analysis->exact);
        for (u32 source = 0; source < 3; source++)
            if ((use & (1u << source)) && (sources[source] & 3u) == 2u)
                analysis->input_mask |= 1u << input_src;

        if (d3 & 1u) {
            ended = 1;
            break;
        }
    }
    if (!ended) {
        analysis->input_mask = 0xFFFFu;
        analysis->exact = 0;
        return -1;
    }
    if (!analysis->exact)
        analysis->input_mask = 0xFFFFu;
    return instrs;
}

static int rsx_vp_decompile_impl(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, int compact_inputs,
    u32 input_mask, char* out, u32 out_size);

int rsx_vp_decompile(const u8* ucode, u32 max_bytes, char* out, u32 out_size)
{
    return rsx_vp_decompile_ex(ucode, max_bytes, 0u, out, out_size);
}

int rsx_vp_decompile_ex(const u8* ucode, u32 max_bytes, u32 vtex_mask,
                        char* out, u32 out_size)
{
    return rsx_vp_decompile_impl(
        ucode, max_bytes, vtex_mask, 0, 0xFFFFu, out, out_size);
}

int rsx_vp_decompile_compact_ex(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, u32 input_mask,
    char* out, u32 out_size)
{
    rsx_vp_input_analysis analysis;
    if (rsx_vp_analyze_inputs(ucode, max_bytes, &analysis) < 0 ||
        !analysis.exact ||
        (analysis.input_mask & ~(input_mask & 0xFFFFu)) != 0)
        input_mask = 0xFFFFu;
    else
        input_mask = analysis.input_mask;
    return rsx_vp_decompile_impl(
        ucode, max_bytes, vtex_mask, 1, input_mask, out, out_size);
}

static int rsx_vp_decompile_core(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, int compact_inputs,
    u32 input_mask, const rsx_vp_program_info* info, char* out, u32 out_size);

static int rsx_vp_decompile_impl(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, int compact_inputs,
    u32 input_mask, char* out, u32 out_size)
{
    return rsx_vp_decompile_core(ucode, max_bytes, vtex_mask, compact_inputs,
                                 input_mask, NULL, out, out_size);
}

/* ---- program analysis (RPCS3 vertex_program_utils::analyse_vertex_program) */
static int vp_walk(const u8* mem, u32 total, u32 start, int fast_exit,
                   rsx_vp_program_info* r, u32* range_lo, u32* range_hi,
                   int* has_branch, u32* call_stack, u32* csp, int depth)
{
    u32 cur = start;
    u32 targets[RSX_VP_MAX_INSTR];
    u32 n_targets = 0;
    if (depth > 64) return -1;
    for (;;) {
        if (cur >= total) return -1;
        if (r->mask[cur >> 5] & (1u << (cur & 31))) break;   /* self-intersection */
        const u8* p = mem + cur * 16u;
        const u32 d0 = rd_le(p), d1 = rd_le(p + 4), d2 = rd_le(p + 8), d3 = rd_le(p + 12);
        r->mask[cur >> 5] |= 1u << (cur & 31);
        if (cur < *range_lo) *range_lo = cur;
        if (cur > *range_hi) *range_hi = cur;

        const u32 sca = (d1 >> 27) & 0x1Fu;
        int static_jump = 0, function_call = 1, jumped = 0;
        switch (sca) {
        case 0x09:                                   /* BRI */
            static_jump = ((d0 >> 10) & 7u) == 7u;
            /* fallthrough */
        case 0x11:                                   /* BRB */
            function_call = 0;
            /* fallthrough */
        case 0x0A: case 0x0B: case 0x12: {           /* CAL CLI CLB */
            *has_branch = 1;
            const u32 t = (((d0 >> 23) & 1u) << 9) | ((d2 & 0x3Fu) << 3) | ((d3 >> 29) & 7u);
            if (function_call) {
                if (*csp >= 64) return -1;
                call_stack[(*csp)++] = cur + 1;
                cur = t; jumped = 1;
            } else if (static_jump) {
                cur = t; jumped = 1;
            } else {
                u32 k = 0;
                while (k < n_targets && targets[k] != t) k++;
                if (k == n_targets && n_targets < RSX_VP_MAX_INSTR) targets[n_targets++] = t;
                if (t > *range_hi) *range_hi = t;
            }
            break;
        }
        case 0x0C:                                   /* RET */
            if (*csp) { cur = call_stack[--(*csp)]; jumped = 1; }
            break;
        default: break;
        }
        if (jumped) continue;

        if (d3 & 1u) {
            if (!*has_branch || fast_exit || cur >= *range_hi) break;
        } else if (cur + 1 == total) {
            break;
        }
        cur++;
    }
    /* std::set iteration order: ascending */
    for (u32 i = 0; i < n_targets; i++)
        for (u32 j = i + 1; j < n_targets; j++)
            if (targets[j] < targets[i]) { const u32 t = targets[i]; targets[i] = targets[j]; targets[j] = t; }
    for (u32 i = 0; i < n_targets; i++) {
        const u32 t = targets[i];
        if (t < total && !(r->mask[t >> 5] & (1u << (t & 31))))
            if (vp_walk(mem, total, t, 1, r, range_lo, range_hi, has_branch,
                        call_stack, csp, depth + 1) < 0)
                return -1;
    }
    return 0;
}

int rsx_vp_analyse_program(const u8* vp_mem, u32 total_slots, u32 entry,
                           rsx_vp_program_info* info)
{
    if (!vp_mem || !info) return -1;
    if (total_slots > RSX_VP_MAX_INSTR) total_slots = RSX_VP_MAX_INSTR;
    memset(info, 0, sizeof *info);
    if (entry >= total_slots) return -1;
    u32 lo = 0xFFFFFFFFu, hi = 0, csp = 0;
    u32 call_stack[64];
    int has_branch = 0;
    if (vp_walk(vp_mem, total_slots, entry, 0, info, &lo, &hi, &has_branch,
                call_stack, &csp, 0) < 0 || lo > hi || hi >= total_slots)
        return -1;
    info->base = lo;
    info->count = hi - lo + 1u;
    info->entry = entry - lo;
    info->has_branch = has_branch;
    /* Shift the touched-instruction mask down to the program base. */
    u32 shifted[RSX_VP_MASK_WORDS];
    memset(shifted, 0, sizeof shifted);
    for (u32 i = 0; i < info->count; i++) {
        const u32 s = lo + i;
        if (info->mask[s >> 5] & (1u << (s & 31))) shifted[i >> 5] |= 1u << (i & 31);
    }
    memcpy(info->mask, shifted, sizeof shifted);
    return (int)info->count;
}

/* Whole-program entry point: analyse from `entry_slot` over the full
 * transform program memory, then decompile the touched range with branch
 * targets relative to its base (RPCS3 patches them the same way). */
int rsx_vp_decompile_program(const u8* vp_mem, u32 total_slots, u32 entry_slot,
                             u32 vtex_mask, u32 input_mask, int compact_inputs,
                             char* out, u32 out_size)
{
    rsx_vp_program_info info;
    if (rsx_vp_analyse_program(vp_mem, total_slots, entry_slot, &info) < 0) return -1;
    const u8* uc = vp_mem + info.base * 16u;
    const u32 bytes = info.count * 16u;
    if (compact_inputs) {
        rsx_vp_input_analysis analysis;
        if (rsx_vp_analyze_inputs(uc, bytes, &analysis) < 0 || !analysis.exact ||
            info.has_branch || info.entry ||
            (analysis.input_mask & ~(input_mask & 0xFFFFu)) != 0)
            input_mask = 0xFFFFu;
        else
            input_mask = analysis.input_mask;
    }
    return rsx_vp_decompile_core(uc, bytes, vtex_mask, compact_inputs, input_mask,
                                 &info, out, out_size);
}

/* Destination write (RPCS3 VertexProgramDecompiler::SetDST / GetDST):
 *   - cond == FL: nothing is written, cond test or not;
 *   - an empty write mask is a full one;
 *   - o[dst] when this unit owns the result (VEC: vec_result, SCA: !vec_result)
 *     and dst != 0x1F (dst > 15 is not an output and is dropped);
 *   - the temp (address register for ARL) when its index != 0x3F;
 *   - otherwise CC when this unit does not own the result, or, with no
 *     destination at all, when either CC update bit is set.
 * Cond-tested writes go per component through the CC swizzle. */
static void vp_write(Out* b, const char* value, int is_sca, int is_arl,
                     u32 d0, u32 d3, const CondCtx* cc, u32* out_written)
{
    if (((d0 >> 10) & 7u) == 0u) return;
    char m[6];
    if (is_sca) mask_str((d3 >> 20) & 1, (d3 >> 19) & 1, (d3 >> 18) & 1, (d3 >> 17) & 1, m);
    else        mask_str((d3 >> 16) & 1, (d3 >> 15) & 1, (d3 >> 14) & 1, (d3 >> 13) & 1, m);
    if (!m[0]) memcpy(m, "xyzw", 5);
    const u32 dst = (d3 >> 2) & 0x1Fu;
    const u32 tmp = is_sca ? ((d3 >> 7) & 0x3Fu) : ((d0 >> 15) & 0x3Fu);
    const int is_result = is_sca ? !((d0 >> 30) & 1u) : (int)((d0 >> 30) & 1u);
    const int saturate = (d0 >> 26) & 1;

    char line[700];
    snprintf(line, sizeof line, "    { float4 _v = (float4)(%s);%s\n", value,
             (saturate && !is_arl) ? " _v = saturate(_v);" : "");
    emit(b, line);
    if (dst != 0x1Fu || tmp != 0x3Fu) {
        if (is_result && dst != 0x1Fu && !is_arl && dst < 16u) {
            emit_store_cc(b, "o[%u]", dst, m, cc);
            *out_written |= 1u << dst;
        }
        if (tmp != 0x3Fu) {
            if (is_arl) {
                /* RPCS3 writes "a" + dst_tmp; only a0/a1 are ever read. */
                emit(b, "        int4 _vi = (int4)_v;\n");
                if (tmp < 2u) emit_store_cc_src(b, "a%u", tmp, m, cc, "_vi");
            } else {
                emit_store_cc(b, "r[%u]", tmp & (vp_temp64() ? 63u : 31u), m, cc);
            }
        } else if (!is_result) {
            emit_store_cc(b, "cc[%u]", cc->sel, m, cc);
        }
    } else if (((d0 >> 14) & 1u) || ((d0 >> 29) & 1u)) {
        emit_store_cc(b, "cc[%u]", cc->sel, m, cc);
    }
    emit(b, "    }\n");
}

static int rsx_vp_decompile_core(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, int compact_inputs,
    u32 input_mask, const rsx_vp_program_info* info, char* out, u32 out_size)
{
    if (!ucode || !out || out_size < 256) return -1;

    /* Body is built first, preamble/epilogue wrap it. */
    static char body[192 * 1024];
    Out b = { body, sizeof(body), 0, 0 };
    body[0] = '\0';

    u32 out_written = 0;   /* o[] registers the program writes (RPCS3 HasParam) */
    /* Flow control: a branching program is emitted as a block state machine.
     * Leaders are the entry, every branch target, the instruction after
     * every branch or program exit, and the first live instruction after a
     * dead one; each block is "if (_pc == N) { ... _pc = next; }" inside a
     * bounded loop, with a small call stack for CAL/RET. Branch-free
     * programs are emitted straight. With `info` (rsx_vp_decompile_program)
     * the instruction range, entry and live mask come from the RPCS3-style
     * walk and branch targets are relative to info->base; without it the
     * program runs from the first instruction to the first end bit and
     * targets are relative to rsx_vp_set_branch_base(). */
    const u32 base = info ? info->base : s_vp_branch_base;
    u32 n_instr = 0;
    if (info) {
        n_instr = info->count;
        if (n_instr * 16u > max_bytes) return -1;
    } else {
        n_instr = rsx_vp_program_size_instrs(ucode, max_bytes);
        if (!n_instr) n_instr = max_bytes / 16u;
    }
    if (n_instr > RSX_VP_MAX_INSTR) n_instr = RSX_VP_MAX_INSTR;
#define VP_LIVE(k) (info ? ((info->mask[(k) >> 5] >> ((k) & 31)) & 1u) : 1u)
    const u32 entry = info ? info->entry : 0u;

    static u8 leader[RSX_VP_MAX_INSTR + 1];
    memset(leader, 0, sizeof leader);
    int has_flow = 0;
    for (u32 k = 0; k < n_instr; k++) {
        if (!VP_LIVE(k)) continue;
        const u8* p = ucode + k * 16u;
        const u32 e0 = rd_le(p), e1 = rd_le(p + 4), e2 = rd_le(p + 8), e3 = rd_le(p + 12);
        if (vp_is_flow((e1 >> 27) & 0x1F)) {
            has_flow = 1;
            const u32 t = vp_branch_target_from(e0, e2, e3, base);
            if (t < RSX_VP_MAX_INSTR) leader[t] = 1;
            leader[k + 1] = 1;
        }
        if (e3 & 1u) leader[k + 1] = 1;
        if (k && !VP_LIVE(k - 1)) leader[k] = 1;
    }
    if (entry) has_flow = 1;
    leader[entry] = 1;
    leader[0] = 1;
    int block_open = 0;

    int instrs = 0;
    for (u32 i = 0; i < n_instr; i++) {
        if (!VP_LIVE(i)) continue;
        const u8* p = ucode + i * 16u;
        u32 d0 = rd_le(p), d1 = rd_le(p + 4), d2 = rd_le(p + 8), d3 = rd_le(p + 12);
        if (has_flow && leader[i]) {
            char line[64];
            if (block_open) {
                snprintf(line, sizeof line, "        _pc = %u;\n    }\n", i);
                emit(&b, line);
            }
            snprintf(line, sizeof line, "    if (_pc == %u) {\n", i);
            emit(&b, line);
            block_open = 1;
        }
        instrs = (int)i + 1;

        u32 vec_op = (d1 >> 22) & 0x1F;
        u32 sca_op = (d1 >> 27) & 0x1F;
        u32 input_src = (d1 >> 8) & 0xF;
        u32 const_src = (d1 >> 12) & 0x3FF;
        int index_const = (d3 >> 1) & 1;

        /* assemble the three 17-bit source fields */
        u32 src0 = ((d2 >> 23) & 0x1FF) | ((d1 & 0xFF) << 9);
        u32 src1 = (d2 >> 6) & 0x1FFFF;
        u32 src2 = ((d3 >> 21) & 0x7FF) | ((d2 & 0x3F) << 11);
        int a0 = (d0 >> 21) & 1, a1 = (d0 >> 22) & 1, a2 = (d0 >> 23) & 1;
        u32 addr_sel = (d0 >> 24) & 1;
        u32 addr_swz = d0 & 3;
        int cond_test = (d0 >> 13) & 1;
        u32 cond = (d0 >> 10) & 7;

        /* RPCS3: a source with register type 0 aborts the program there. */
        int program_end = 0;
        if (!(src0 & 3u) || !(src1 & 3u) || !(src2 & 3u)) {
            emit(&b, "    /* src check failed: program aborts */\n");
            program_end = 1;
            vec_op = sca_op = 0;
        }

        char A[96], B[96], C[96];
        emit_src(src0, a0, input_src, const_src, index_const, addr_sel, addr_swz, A, sizeof(A));
        emit_src(src1, a1, input_src, const_src, index_const, addr_sel, addr_swz, B, sizeof(B));
        emit_src(src2, a2, input_src, const_src, index_const, addr_sel, addr_swz, C, sizeof(C));

        /* Condition codes (D0: CC swizzle [2:9], cond [10:12], test [13],
         * update-enable [14] / [29], CC register select [25]). */
        CondCtx cc = { cond_test && cond != 7, cond, (d0 >> 25) & 1, {0} };
        cc.swz[0] = SWZ[(d0 >> 8) & 3]; cc.swz[1] = SWZ[(d0 >> 6) & 3];
        cc.swz[2] = SWZ[(d0 >> 4) & 3]; cc.swz[3] = SWZ[(d0 >> 2) & 3];

        /* ---- vector ALU ---- */
        if (vec_op != 0x00) {
            char rhs[512];
            int handled = 1, is_arl = 0;
            switch (vec_op) {
            case 0x01: snprintf(rhs,sizeof rhs,"%s",A); break;                 /* MOV */
            case 0x02: snprintf(rhs,sizeof rhs,"vp_mul((%s),(%s))",A,B); break;  /* MUL */
            case 0x03: snprintf(rhs,sizeof rhs,"(%s)+(%s)",A,C); break;        /* ADD: src0 + src2 */
            case 0x04: snprintf(rhs,sizeof rhs,"vp_mad((%s),(%s),(%s))",A,B,C); break; /* MAD */
            case 0x05: snprintf(rhs,sizeof rhs,"vp_dot3((%s),(%s))",A,B); break; /* DP3 */
            case 0x06: snprintf(rhs,sizeof rhs,"(vp_dot3((%s),(%s))+(%s).w)",A,B,B); break; /* DPH */
            case 0x07: snprintf(rhs,sizeof rhs,"vp_dot4((%s),(%s))",A,B); break; /* DP4 */
            case 0x08: snprintf(rhs,sizeof rhs,"float4(1,(%s).y*(%s).y,(%s).z,(%s).w)",A,B,A,B); break; /* DST */
            case 0x09: snprintf(rhs,sizeof rhs,"min((%s),(%s))",A,B); break;   /* MIN */
            case 0x0A: snprintf(rhs,sizeof rhs,"max((%s),(%s))",A,B); break;   /* MAX */
            case 0x0B: snprintf(rhs,sizeof rhs,"(float4)(vp_cmpfix(%s)<vp_cmpfix(%s))",A,B); break;  /* SLT */
            case 0x0C: snprintf(rhs,sizeof rhs,"(float4)(vp_cmpfix(%s)>=vp_cmpfix(%s))",A,B); break; /* SGE */
            case 0x0D: snprintf(rhs,sizeof rhs,"%s",A); is_arl = 1; break;     /* ARL: int4($0), truncating */
            case 0x0E: snprintf(rhs,sizeof rhs,"frac(%s)",A); break;           /* FRC */
            case 0x0F: snprintf(rhs,sizeof rhs,"floor(%s)",A); break;          /* FLR */
            case 0x10: snprintf(rhs,sizeof rhs,"(float4)(vp_cmpfix(%s)==vp_cmpfix(%s))",A,B); break; /* SEQ */
            case 0x11: snprintf(rhs,sizeof rhs,"(float4)0"); break;            /* SFL */
            case 0x12: snprintf(rhs,sizeof rhs,"(float4)(vp_cmpfix(%s)>vp_cmpfix(%s))",A,B); break;  /* SGT */
            case 0x13: snprintf(rhs,sizeof rhs,"(float4)(vp_cmpfix(%s)<=vp_cmpfix(%s))",A,B); break; /* SLE */
            case 0x14: snprintf(rhs,sizeof rhs,"(float4)(vp_cmpfix(%s)!=vp_cmpfix(%s))",A,B); break; /* SNE */
            case 0x15: snprintf(rhs,sizeof rhs,"float4(1,1,1,1)"); break;      /* STR */
            case 0x16: snprintf(rhs,sizeof rhs,"sign(%s)",A); break;           /* SSG */
            case 0x19: {                                                       /* TXL */
                const u32 tex_unit = (d2 >> 8) & 3u;
                /* T2-08: UN 1D/2D units divide by the texture size. */
                const int un = vp_vtex_un() && (((vtex_mask & s_vp_vtex_un_mask) >> tex_unit) & 1u) &&
                    !((((s_vp_vtex_3d_mask | s_vp_vtex_cube_mask) & ~s_vp_vtex_1d_mask) >> tex_unit) & 1u);
                if (((vtex_mask & s_vp_vtex_1d_mask) >> tex_unit) & 1u)
                    snprintf(rhs, sizeof rhs,
                        un ? "rsx_vtex%u.SampleLevel(rsx_vsamp%u, float2((%s).x / _rsx_vtex_size%u().x, 0.5), 0.0)"
                           : "rsx_vtex%u.SampleLevel(rsx_vsamp%u, float2((%s).x, 0.5), 0.0)",
                        tex_unit, tex_unit, A, tex_unit);
                else if (((vtex_mask & (s_vp_vtex_3d_mask | s_vp_vtex_cube_mask)) >> tex_unit) & 1u)
                    snprintf(rhs, sizeof rhs,
                        "rsx_vtex%u.SampleLevel(rsx_vsamp%u, (%s).xyz, 0.0)",
                        tex_unit, tex_unit, A);
                else if ((vtex_mask >> tex_unit) & 1u)
                    snprintf(rhs, sizeof rhs,
                        un ? "rsx_vtex%u.SampleLevel(rsx_vsamp%u, (%s).xy / _rsx_vtex_size%u(), 0.0)"
                           : "rsx_vtex%u.SampleLevel(rsx_vsamp%u, (%s).xy, 0.0)",
                        tex_unit, tex_unit, A, tex_unit);
                else
                    snprintf(rhs, sizeof rhs, "float4(0,0,0,0)");
                break;
            }
            default:
                /* RPCS3: unknown vector opcode ends the program. */
                emit(&b, "    /* unknown VP vec op: program aborts */\n");
                program_end = 1;
                handled = 0;
                break;
            }
            if (handled)
                vp_write(&b, rhs, 0, is_arl, d0, d3, &cc, &out_written);
        }

        /* ---- scalar ALU (reads SRC2, per component like RPCS3) ---- */
        char flow_term[256] = "";
        if (sca_op != 0x00 && !vp_is_flow(sca_op)) {
            char rhs[256];
            rhs[0] = '\0';
            switch (sca_op) {
            case 0x01: snprintf(rhs,sizeof rhs,"%s",C); break;                          /* MOV */
            case 0x02: snprintf(rhs,sizeof rhs,"(1.0/(%s))",C); break;                  /* RCP */
            case 0x03: snprintf(rhs,sizeof rhs,"clamp(1.0/(%s), 5.42101e-20, 1.884467e19)",C); break; /* RCC */
            case 0x04: snprintf(rhs,sizeof rhs,"(float4)(1.0/sqrt(max((%s).x, 1e-10)))",C); break; /* RSQ */
            case 0x05: snprintf(rhs,sizeof rhs,"exp(%s)",C); break;                     /* EXP */
            case 0x06: snprintf(rhs,sizeof rhs,"log(%s)",C); break;                     /* LOG */
            case 0x07: snprintf(rhs,sizeof rhs,"vp_lit(%s)",C); break;                  /* LIT */
            case 0x0D: snprintf(rhs,sizeof rhs,"log2(max((%s), 1e-10))",C); break;      /* LG2 */
            case 0x0E: snprintf(rhs,sizeof rhs,"exp2(%s)",C); break;                    /* EX2 */
            case 0x0F: snprintf(rhs,sizeof rhs,"sin(%s)",C); break;                     /* SIN */
            case 0x10: snprintf(rhs,sizeof rhs,"cos(%s)",C); break;                     /* COS */
            case 0x13: case 0x14:                                                       /* PSH POP */
                emit(&b, "    /* PSH/POP: unimplemented (RPCS3 too) */\n");
                break;
            default:
                emit(&b, "    /* unknown VP sca op: program aborts */\n");
                program_end = 1;
                break;
            }
            if (rhs[0])
                vp_write(&b, rhs, 1, 0, d0, d3, &cc, &out_written);
        }

        /* Branches are decoded last, after a co-issued vector op (RPCS3). */
        if (vp_is_flow(sca_op)) {
            const u32 tgt = vp_branch_target_from(d0, d2, d3, base);
            char line[160];
            snprintf(line, sizeof line, "    /* FLOW %s @%u -> %u cond=%u test=%d swz=%c%c%c%c */\n",
                     rsx_vp_sca_name(sca_op), i, tgt, cond, cond_test,
                     cc.swz[0], cc.swz[1], cc.swz[2], cc.swz[3]);
            emit(&b, line);
            /* RPCS3 GetCond: any() of the CC swizzle compared against zero,
             * applied whether or not the cond-test bit is set. */
            static const char* cmp[8] = { 0, "<", "==", "<=", ">", "!=", ">=", 0 };
            char c[64];
            if (cond == 7)      snprintf(c, sizeof c, "true");
            else if (cond == 0) snprintf(c, sizeof c, "false");
            else snprintf(c, sizeof c, "any(cc[%u].%c%c%c%c %s (float4)0)", cc.sel,
                          cc.swz[0], cc.swz[1], cc.swz[2], cc.swz[3], cmp[cond]);
            const u32 next = i + 1;
            if (sca_op == 0x09 || sca_op == 0x08)          /* BRI, BRA */
                snprintf(flow_term, sizeof flow_term, "        _pc = %s ? %u : %u;\n", c, tgt, next);
            else if (sca_op == 0x0A || sca_op == 0x0B)     /* CAL, CLI */
                snprintf(flow_term, sizeof flow_term,
                         "        if (%s) { _cs[_csp] = %u; _csp = min(_csp + 1, 7); _pc = %u; } else _pc = %u;\n",
                         c, next, tgt, next);
            else if (sca_op == 0x0C)                       /* RET */
                /* RPCS3: a RET inside a call returns unconditionally; at top
                 * level it ends the program when its condition holds. */
                snprintf(flow_term, sizeof flow_term,
                         "        if (_csp > 0) { _csp--; _pc = _cs[_csp]; } else _pc = (%s) ? -1 : %u;\n",
                         c, next);
            else {                                         /* BRB/CLB: transform branch bits */
                /* D3[23:27] = bit index, D3[28] = taken when the bit is set
                 * (else when clear); RPCS3's GetOptionalBranchCond. */
                char bc[80];
                snprintf(bc, sizeof bc, "((vp_branch.x & %uu) %s 0u)",
                         1u << ((d3 >> 23) & 0x1Fu), ((d3 >> 28) & 1u) ? "!=" : "==");
                if (sca_op == 0x11)                        /* BRB */
                    snprintf(flow_term, sizeof flow_term, "        _pc = %s ? %u : %u;\n", bc, tgt, next);
                else                                       /* CLB */
                    snprintf(flow_term, sizeof flow_term,
                             "        if (%s) { _cs[_csp] = %u; _csp = min(_csp + 1, 7); _pc = %u; } else _pc = %u;\n",
                             bc, next, tgt, next);
            }
        }

        const int exits = program_end || (d3 & 1u);
        if (has_flow) {
            if (exits) {
                emit(&b, "        _pc = -1;\n    }\n");
                block_open = 0;
            } else if (flow_term[0]) {
                emit(&b, flow_term);
                emit(&b, "    }\n");
                block_open = 0;
            }
        } else if (exits) {
            break;
        }
    }
    if (block_open) emit(&b, "        _pc = -1;\n    }\n");
#undef VP_LIVE

    if (b.overflow) return -1;
    /* RPCS3 BuildCode: a program that never writes o[0] is NOPed whole; the
     * position stays (0,0,0,1) and every other output its default. */
    if (!(out_written & 1u)) {
        body[0] = '\0';
        out_written = 0;
        has_flow = 0;
    }

    /* ---- assemble full shader ---- */
    Out o = { out, out_size, 0, 0 };
    if (!compact_inputs) {
        emit(&o,
            /* All 16 RSX vertex attributes arrive as float4 (the harness
             * fetches and converts them on the CPU). */
            "struct VSInput {\n"
            "    float4 a0:ATTR0;  float4 a1:ATTR1;  float4 a2:ATTR2;  float4 a3:ATTR3;\n"
            "    float4 a4:ATTR4;  float4 a5:ATTR5;  float4 a6:ATTR6;  float4 a7:ATTR7;\n"
            "    float4 a8:ATTR8;  float4 a9:ATTR9;  float4 a10:ATTR10; float4 a11:ATTR11;\n"
            "    float4 a12:ATTR12; float4 a13:ATTR13; float4 a14:ATTR14; float4 a15:ATTR15;\n"
            "};\n");
    } else {
        emit(&o, "struct VSInput {\n");
        if (!(input_mask & 0xFFFFu))
            emit(&o, "    uint vertex_id:SV_VertexID;\n");
        for (u32 attr = 0; attr < 16; attr++) {
            if (!(input_mask & (1u << attr))) continue;
            char declaration[64];
            snprintf(declaration, sizeof(declaration),
                     "    float4 a%u:ATTR%u;\n", attr, attr);
            emit(&o, declaration);
        }
        emit(&o, "};\n");
    }
    emit(&o,
        "struct VSOutput {\n"
        "    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;\n"
        "    float4 fog:FOG;\n"
        "    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
        "    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;\n"
        "    float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;\n"
        /* Built-ins last: they take no interpolant location, and the
         * varyings above keep theirs. glslang does not map PSIZE to the
         * PointSize built-in by itself (it would take location 15); its
         * paths define RSX_FRAGCOORD_W_IS_RCP, FXC never sees the attribute. */
        "#ifdef RSX_FRAGCOORD_W_IS_RCP\n"
        "    [[vk::builtin(\"PointSize\")]]\n"
        "#endif\n"
        "    float psize:PSIZE;\n");
    if (s_vp_clip_distances)
        emit(&o, "    float4 cd0:SV_ClipDistance0; float2 cd1:SV_ClipDistance1;\n");
    {
        static int legacy_mul = -1;
        if (legacy_mul < 0) {
            const char* e = getenv("RSX_VP_LEGACY_MUL");
            legacy_mul = e && *e && *e != '0';
        }
        if (legacy_mul)
            emit(&o, "#define RSX_VP_LEGACY_MUL 1\n");
    }
    emit(&o,
        "};\n"
        /* b0: 512 vec4 transform constants + the RSX viewport transform,
         * pre-mapped by the harness to D3D clip space:
         *   ndc.xyz = clip.xyz * vp_posscale.xyz + clip.w * vp_posoffset.xyz */
        "cbuffer VPConst : register(b0) {\n"
        "    float4 vp_c[512];\n"
        "    float4 vp_posscale;\n"
        "    float4 vp_posoffset;\n"
        "    uint4  vp_branch;   /* x: branch bits, y: output mask, z: clip ctl|valid, w: point size */\n"
        /* Resistance port: GPU vertex fetch (rsx_d3d12_backend gf_*). Per
         * attribute: x = byte base in the raw buffer (signed), y = stride,
         * z = type | n<<8 | mode<<16 (mode 1 = array, else constant);
         * vp_gfc = the constant register / defaults; vp_gfx.x = base index. */
        "    uint4  vp_gfa[16];\n"
        "    float4 vp_gfc[16];\n"
        "    uint4  vp_gfx;\n"
        "};\n"
        /* NV vertex programs multiply by the legacy rule: 0 times anything,
         * INF and NaN included, is 0. IEEE gives NaN, and one RCP of a zero
         * distance then poisons a whole lighting sum -- Tornado Outbreak's UI
         * text came out NaN, which the FP's NaN guard turned into black. */
        /* Scalar logic per component: glslang's HLSL front end (the Metal
         * path) does not take || on vectors. */
        "#ifdef RSX_VP_LEGACY_MUL\n"
        "float vp_mul1(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }\n"
        "float4 vp_mul(float4 a, float4 b) { return float4(vp_mul1(a.x, b.x), vp_mul1(a.y, b.y),"
        " vp_mul1(a.z, b.z), vp_mul1(a.w, b.w)); }\n"
        "float vp_dot3(float4 a, float4 b) { float4 m = vp_mul(a, b); return m.x + m.y + m.z; }\n"
        "float vp_dot4(float4 a, float4 b) { float4 m = vp_mul(a, b); return m.x + m.y + m.z + m.w; }\n"
        "float4 vp_mad(float4 a, float4 b, float4 c) { return vp_mul(a, b) + c; }\n"
        "#else\n"
        /* RPCS3 VertexProgramDecompiler: MUL `$0 * $1`, MAD `fma`, DPx `dot`
         * -- plain IEEE. RSX_VP_LEGACY_MUL=1 restores the 0*x=0 rule. */
        "float4 vp_mul(float4 a, float4 b) { return a * b; }\n"
        "float vp_dot3(float4 a, float4 b) { return dot(a.xyz, b.xyz); }\n"
        "float vp_dot4(float4 a, float4 b) { return dot(a, b); }\n"
        "float4 vp_mad(float4 a, float4 b, float4 c) { return mad(a, b, c); }\n"
        "#endif\n"
        /* RPCS3 _fetch_indexed_constant: in range wraps at 512, past 467 reads 0. */
        "float4 vp_cidx(uint i) { return i < 468u ? vp_c[i] : float4(0, 0, 0, 0); }\n"
        /* RPCS3 lit_legacy (VertexProgramDecompiler LIT). */
        "float4 vp_lit(float4 v) { float4 t = float4(max(v.x, 0.0), max(v.y, 1e-10), v.z, v.w);"
        " return float4(1.0, t.x, t.x > 0.0 ? exp2(t.w * log2(t.y)) : 0.0, 1.0); }\n");
    {
        /* RPCS3 VKVertexProgram.cpp:367 low_precision_tests = is_NVIDIA ->
         * _GPU_LOW_PRECISION_COMPARE (GLSLCommon.cpp:183-186), CMP_FIXUP(a) =
         * (sign(a) * 16. + a) (RSXProgramCommon.glsl:9-13) around both
         * operands of SEQ..SNE (VertexProgramDecompiler.cpp:625-634).
         * RSX_VP_CMP_FIXUP=0 compares the raw operands. */
        static int cmp_fixup = -1;
        if (cmp_fixup < 0) {
            const char* e = getenv("RSX_VP_CMP_FIXUP");
            cmp_fixup = !(e && *e == '0');
        }
        emit(&o, (cmp_fixup && s_vp_low_precision)
            ? "float4 vp_cmpfix(float4 a) { return sign(a) * 16.0 + a; }\n"
            : "float4 vp_cmpfix(float4 a) { return a; }\n");
    }

    for (u32 vtu = 0; vtu < 4; vtu++) {
        if (!((vtex_mask >> vtu) & 1u)) continue;
        char decl[256];
        const u32 bit = 1u << vtu;
        snprintf(decl, sizeof(decl),
            "%s rsx_vtex%u : register(t%u);\n"
            "SamplerState rsx_vsamp%u : register(s%u);\n",
            (s_vp_vtex_1d_mask & bit) ? "Texture2D" :
            (s_vp_vtex_3d_mask & bit) ? "Texture3D" :
            (s_vp_vtex_cube_mask & bit) ? "TextureCube" : "Texture2D",
            vtu, 16u + vtu, vtu, vtu);
        emit(&o, decl);
        if (vp_vtex_un() && (s_vp_vtex_un_mask & bit) &&
            ((s_vp_vtex_1d_mask & bit) || !((s_vp_vtex_3d_mask | s_vp_vtex_cube_mask) & bit))) {
            if (s_vp_vtex_un_dim[vtu][0] && s_vp_vtex_un_dim[vtu][1])
                snprintf(decl, sizeof(decl),
                    "float2 _rsx_vtex_size%u() { return float2(%u.0, %u.0); }\n",
                    vtu, s_vp_vtex_un_dim[vtu][0], s_vp_vtex_un_dim[vtu][1]);
            else
                snprintf(decl, sizeof(decl),
                    "float2 _rsx_vtex_size%u() { uint w, h; rsx_vtex%u.GetDimensions(w, h); return float2(w, h); }\n",
                    vtu, vtu);
            emit(&o, decl);
        }
    }

    emit(&o, "VSOutput main(VSInput input) {\n");
    if (!compact_inputs) {
        emit(&o,
            "    float4 v[16];\n"
            "    v[0]=input.a0;  v[1]=input.a1;  v[2]=input.a2;  v[3]=input.a3;\n"
            "    v[4]=input.a4;  v[5]=input.a5;  v[6]=input.a6;  v[7]=input.a7;\n"
            "    v[8]=input.a8;  v[9]=input.a9;  v[10]=input.a10; v[11]=input.a11;\n"
            "    v[12]=input.a12; v[13]=input.a13; v[14]=input.a14; v[15]=input.a15;\n");
    } else {
        emit(&o,
            "    float4 v[16];\n"
            "    [unroll] for (int _k=0;_k<16;_k++) v[_k]=float4(0,0,0,1);\n");
        for (u32 attr = 0; attr < 16; attr++) {
            if (!(input_mask & (1u << attr))) continue;
            char assignment[64];
            snprintf(assignment, sizeof(assignment),
                     "    v[%u]=input.a%u;\n", attr, attr);
            emit(&o, assignment);
        }
    }
    emit(&o,
        "    float4 r[64]; float4 o[16];\n"
        "    [unroll] for (int _i=0;_i<64;_i++) r[_i]=(float4)0;\n"
        "    [unroll] for (int _j=0;_j<16;_j++) o[_j]=float4(0,0,0,1);\n"
        "    int4 a0 = (int4)0; int4 a1 = (int4)0;\n"
        "    float4 cc[2]; cc[0] = (float4)0; cc[1] = (float4)0;\n");

    if (has_flow) {
        char line[64];
        snprintf(line, sizeof line, "    int _pc = %u; int _cs[8]; int _csp = 0;\n", entry);
        emit(&o, line);
        emit(&o, "    [loop] for (int _it = 0; _it < 16384 && _pc >= 0; _it++) {\n");
    }
    emit(&o, body);
    if (has_flow) emit(&o, "    }\n");

    /* Output register map (RPCS3 VKVertexProgram reg_table/insertMainEnd):
     * o0 = HPOS through the RSX viewport transform, o1/o2 = colours, o5.x =
     * fog, o5.yzw / o6.yzw = user clip distances 0-2 / 3-5, o6.x = point
     * size, o7..o14 = TEX0..7, o15 = TEX8, o6 = TEX9. A varying the program
     * never writes, or that VERTEX_ATTRIB_OUTPUT_MASK leaves off, reads
     * (0,0,0,1); a point size it does not supply comes from POINT_SIZE. */
    emit(&o,
        "    VSOutput Out;\n"
        "    const uint _om = (vp_branch.z & 0x80000000u) != 0u ? vp_branch.y : 0xFFFFFFFFu;\n"
        "    const float4 _d = float4(0, 0, 0, 1);\n"
        "    float4 _p = o[0];\n"
        "    Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);\n"
        /* RPCS3 apply_zclip_xform snap; vp_posoffset.w = 1/(range*16777215). */
        "    if (vp_posoffset.w > 0.0 && Out.pos.w != 0.0) {\n");
    emit(&o, vp_zsnap_floor()
        ? "        float _zd = Out.pos.z / Out.pos.w; float _zn = floor(_zd + 0.5);\n"
        : "        float _zd = Out.pos.z / Out.pos.w; float _zn = round(_zd);\n");
    emit(&o,
        "        if (abs(_zd - _zn) < vp_posoffset.w) Out.pos.z = _zn * Out.pos.w;\n"
        "    }\n");
    {
        static const struct { const char* dst; u32 reg; u32 mask; const char* swz; } k_out[] = {
            { "col0", 1,  (1u << 0) | (1u << 2), "" },
            { "col1", 2,  (1u << 1) | (1u << 3), "" },
            { "fog",  5,  1u << 4,  ".xxxx" },
            { "t0",   7,  1u << 14, "" }, { "t1", 8,  1u << 15, "" },
            { "t2",   9,  1u << 16, "" }, { "t3", 10, 1u << 17, "" },
            { "t4",   11, 1u << 18, "" }, { "t5", 12, 1u << 19, "" },
            { "t6",   13, 1u << 20, "" }, { "t7", 14, 1u << 21, "" },
            { "t8",   15, 1u << 12, "" }, { "t9", 6,  1u << 13, "" },
        };
        char line[160];
        for (u32 i = 0; i < sizeof k_out / sizeof k_out[0]; i++) {
            if (out_written & (1u << k_out[i].reg))
                snprintf(line, sizeof line,
                         "    Out.%s = (_om & %uu) != 0u ? o[%u]%s : _d;\n",
                         k_out[i].dst, k_out[i].mask, k_out[i].reg, k_out[i].swz);
            else
                snprintf(line, sizeof line, "    Out.%s = _d;\n", k_out[i].dst);
            emit(&o, line);
        }
    }
    /* RPCS3 VKVertexProgram.cpp:305/465-498: gl_PointSize = dst_reg6.x only
     * when the POINTSIZE output-mask bit is set and the program writes o6;
     * otherwise (mask bit clear included) `gl_PointSize = point_size`, the
     * POINT_SIZE register (already resolution-scaled in vp_branch.w). */
    emit(&o,
        "    Out.psize = (vp_branch.z & 0x80000000u) != 0u ? asfloat(vp_branch.w) : 1.0;\n");
    if (out_written & (1u << 6))
        emit(&o, "    if ((_om & 32u) != 0u) Out.psize = o[6].x;\n");
    if (s_vp_clip_distances) {
        /* USER_CLIP_PLANE_CONTROL, a nibble per plane: 1 = LT (factor -1),
         * 2 = GE (+1), else disabled (0.5, i.e. never clipped). */
        static const char* const k_uc[6] = {
            "o[5].y", "o[5].z", "o[5].w", "o[6].y", "o[6].z", "o[6].w" };
        static const char* const k_cd[6] = {
            "cd0.x", "cd0.y", "cd0.z", "cd0.w", "cd1.x", "cd1.y" };
        char line[200];
        for (u32 i = 0; i < 6; i++) {
            if (out_written & (1u << (i < 3 ? 5u : 6u)))
                snprintf(line, sizeof line,
                         "    { const uint _op = (vp_branch.z >> %uu) & 15u;"
                         " Out.%s = ((_om & %uu) != 0u && (_op == 1u || _op == 2u))"
                         " ? %s * (_op == 2u ? 1.0 : -1.0) : 0.5; }\n",
                         i * 4u, k_cd[i], 1u << (6u + i), k_uc[i]);
            else
                snprintf(line, sizeof line, "    Out.%s = 0.5;\n", k_cd[i]);
            emit(&o, line);
        }
    }
    emit(&o,
        "    return Out;\n"
        "}\n");

    if (o.overflow) return -1;
    return instrs;
}
