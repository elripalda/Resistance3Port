/*
 * ps3recomp - RSX Fragment Program (NV40 ISA) → HLSL decompiler
 *
 * See rsx_fp_decompiler.h and docs/RSX_FRAGMENT_PROGRAM.md.
 *
 * The encoding (4 words / 16 bytes per instruction; CONST sources carry an
 * inline 16-byte constant in the following slot) is taken verbatim from
 * Mesa/nouveau nvfx_shader.h. Bit positions are spelled out as named macros
 * below so this file is self-documenting.
 */

#include <stdlib.h>
#include "rsx_fp_decompiler.h"
#include <stdio.h>
#include <string.h>
#include "ps3emu/log_first.h"
#include "rsx_texture_layout.h"
#include "rsx_shader_spirv.h"   /* RSX_SPIRV_POINTCOORD_LOCATION */

/* Resistance port (v136): RSX_RES_SCALE (see the D3D12 backend). Surfaces are
 * rendered at scale, so SV_Position counts render pixels; the guest's WPOS and
 * texel-addressed shadow lookups count guest pixels. 1 = no change (and the
 * generated HLSL is byte-identical, so cached shaders stay valid). */
static double s_fp_res_inv = -1.0;
static double fp_res_inv(void)
{
    double inv = s_fp_res_inv;
    if (inv < 0.0) {
        const char* e = getenv("RSX_RES_SCALE");
        double v = e ? atof(e) : 1.0;
        if (!(v >= 1.0)) v = 1.0;
        if (v > 4.0) v = 4.0;
        inv = s_fp_res_inv = 1.0 / v;
    }
    return inv;
}
void rsx_fp_set_res_scale(double scale)
{
    if (!(scale >= 1.0)) scale = 1.0;
    if (scale > 4.0) scale = 4.0;
    s_fp_res_inv = 1.0 / scale;
}
/* Engine-path shaders read the scale per draw from PSConstants.fp_rscale. */
static int s_fp_res_dyn;
void rsx_fp_set_res_scale_dynamic(int on) { s_fp_res_dyn = on ? 1 : 0; }
/* RPCS3 TEX1D* (GLSLCommon sampler1D): a 1D unit reads coordinate .x only. */
static u32 s_fp_tex_1d_mask;
void rsx_fp_set_tex_1d_mask(u32 mask) { s_fp_tex_1d_mask = mask; }

/* ---- DWORD 0 (OPDEST) --------------------------------------------------- */
#define FP_END              (1u << 0)
#define FP_OUT_REG_SHIFT    1
#define FP_OUT_REG_MASK     (63u << 1)
#define FP_OUT_HALF         (1u << 7)
#define FP_COND_WRITE       (1u << 8)
#define FP_OUT_MASK_SHIFT   9          /* X@9 Y@10 Z@11 W@12 */
#define FP_INPUT_SRC_SHIFT  13
#define FP_INPUT_SRC_MASK   (15u << 13)
#define FP_TEX_UNIT_SHIFT   17
#define FP_TEX_UNIT_MASK    (15u << 17)
#define FP_OPCODE_SHIFT     24
#define FP_OPCODE_MASK      (0x3Fu << 24)
#define FP_OUT_NONE         (1u << 30)
#define FP_OUT_SAT          (1u << 31)

/* ---- DWORD 1/2/3 (SRC0/1/2) --------------------------------------------- */
#define FP_REG_TYPE_SHIFT   0
#define FP_REG_TYPE_MASK    (3u << 0)
#define   FP_REG_TYPE_TEMP  0
#define   FP_REG_TYPE_INPUT 1
#define   FP_REG_TYPE_CONST 2
#define FP_SRC_REG_SHIFT    2
#define FP_SRC_REG_MASK     (63u << 2)
#define FP_SRC_HALF         (1u << 8)
#define FP_SRC_SWZ_SHIFT    9          /* X@9 Y@11 Z@13 W@15, 2 bits each */
#define FP_SRC_NEGATE       (1u << 17)
#define FP_SRC0_ABS         (1u << 29) /* in DWORD1 */
#define FP_SRC1_ABS         (1u << 18) /* in DWORD2 */
#define FP_BRANCH           (1u << 31) /* DWORD2 bit31: instruction is a branch */

/* ---- Execution-condition / condition-code fields (SRC0 = DWORD1) ---------
 * The NV40 fragment ISA predicates each instruction on a condition register
 * (CC0/CC1). Layout verified against RPCS3's RSXFragmentProgram.h SRC0 union
 * (exec_if_lt@18, exec_if_eq@19, exec_if_gr@20, cond_swizzle@21..28,
 * cond_mod_reg_index@30, cond_reg_index@31) and OPDEST.set_cond@8. All three
 * exec bits set (0b111) == unconditional (the default); none set == never.
 * Dropped entirely until now: MEASURED present in 4 of this capture's 43
 * fragment programs (scratch/a010_reports/fragment/, cond_detail_out.txt) —
 * genuine SET_CC→exec_if producer/consumer pairs (incl. conditional TEX). */
#define FP_EXEC_IF_LT       (1u << 18)
#define FP_EXEC_IF_EQ       (1u << 19)
#define FP_EXEC_IF_GR       (1u << 20)
#define FP_COND_SWZ_SHIFT   21          /* X@21 Y@23 Z@25 W@27, 2 bits each */
#define FP_COND_MOD_REG     (1u << 30)  /* set_cond WRITE target: cc0/cc1     */
#define FP_COND_REG         (1u << 31)  /* exec_if READ source:   cc0/cc1     */

/* ---- Opcodes ------------------------------------------------------------ */
enum {
    OP_NOP=0x00, OP_MOV=0x01, OP_MUL=0x02, OP_ADD=0x03, OP_MAD=0x04,
    OP_DP3=0x05, OP_DP4=0x06, OP_DST=0x07, OP_MIN=0x08, OP_MAX=0x09,
    OP_SLT=0x0A, OP_SGE=0x0B, OP_SLE=0x0C, OP_SGT=0x0D, OP_SNE=0x0E,
    OP_SEQ=0x0F, OP_FRC=0x10, OP_FLR=0x11, OP_KIL=0x12, OP_PK4B=0x13,
    OP_UP4B=0x14, OP_DDX=0x15, OP_DDY=0x16, OP_TEX=0x17, OP_TXP=0x18,
    OP_TXD=0x19, OP_RCP=0x1A, OP_RSQ=0x1B, OP_EX2=0x1C, OP_LG2=0x1D,
    OP_LIT=0x1E, OP_LRP=0x1F, OP_STR=0x20, OP_SFL=0x21, OP_COS=0x22,
    OP_SIN=0x23, OP_PK2H=0x24, OP_UP2H=0x25, OP_POW=0x26, OP_PK4UB=0x27,
    OP_UP4UB=0x28, OP_PK2US=0x29, OP_UP2US=0x2A, OP_PKG=0x2C, OP_UPG=0x2D, OP_DP2A=0x2E, OP_TXL=0x2F,
    OP_TXB=0x31, OP_TEXBEM=0x33, OP_TXPBEM=0x34, OP_RFL=0x36, OP_DP2=0x38, OP_NRM=0x39, OP_DIV=0x3A,
    OP_DIVSQ=0x3B, OP_LIF=0x3C, OP_FENCT=0x3D, OP_FENCB=0x3E
};

const char* rsx_fp_opcode_name(u32 op)
{
    switch (op) {
    case OP_NOP: return "NOP"; case OP_MOV: return "MOV"; case OP_MUL: return "MUL";
    case OP_ADD: return "ADD"; case OP_MAD: return "MAD"; case OP_DP3: return "DP3";
    case OP_DP4: return "DP4"; case OP_DST: return "DST"; case OP_MIN: return "MIN";
    case OP_MAX: return "MAX"; case OP_SLT: return "SLT"; case OP_SGE: return "SGE";
    case OP_SLE: return "SLE"; case OP_SGT: return "SGT"; case OP_SNE: return "SNE";
    case OP_SEQ: return "SEQ"; case OP_FRC: return "FRC"; case OP_FLR: return "FLR";
    case OP_KIL: return "KIL"; case OP_DDX: return "DDX"; case OP_DDY: return "DDY";
    case OP_TEX: return "TEX"; case OP_TXP: return "TXP"; case OP_TXD: return "TXD";
    case OP_RCP: return "RCP"; case OP_RSQ: return "RSQ"; case OP_EX2: return "EX2";
    case OP_LG2: return "LG2"; case OP_LIT: return "LIT"; case OP_LRP: return "LRP";
    case OP_STR: return "STR"; case OP_SFL: return "SFL"; case OP_COS: return "COS";
    case OP_SIN: return "SIN"; case OP_POW: return "POW"; case OP_DP2A: return "DP2A";
    case OP_TXL: return "TXL"; case OP_TXB: return "TXB"; case OP_RFL: return "RFL";
    case OP_DIV: return "DIV"; case OP_DP2: return "DP2"; case OP_NRM: return "NRM";
    case OP_DIVSQ: return "DIVSQ"; case OP_LIF: return "LIF";
    case OP_PK4B: return "PK4B"; case OP_UP4B: return "UP4B"; case OP_PK2H: return "PK2H";
    case OP_UP2H: return "UP2H"; case OP_PK4UB: return "PK4UB"; case OP_UP4UB: return "UP4UB";
    case OP_PK2US: return "PK2US"; case OP_UP2US: return "UP2US"; case OP_PKG: return "PKG";
    case OP_UPG: return "UPG";
    case OP_FENCT: return "FENCT"; case OP_FENCB: return "FENCB";
    default:     return "?";
    }
}

/* ------------------------------------------------------------------------- */

u32 rsx_fp_read_word(const u8* p)
{
    u32 be = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
    return (be << 16) | (be >> 16);   /* 16-bit half-word swap */
}

u32 rsx_fp_program_size(const u8* ucode, u32 max_bytes)
{
    if (!ucode) return 0;
    u32 off = 0;
    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        /* A CONST source pulls an inline 16-byte constant slot. */
        if (((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST ||
            ((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST ||
            ((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) {
            if (off + 16 <= max_bytes) off += 16;
        }
        if (w0 & FP_END) return off;
    }
    return 0;
}

static int fp_instruction_has_constant(u32 w1, u32 w2, u32 w3)
{
    return
        ((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) ==
            FP_REG_TYPE_CONST ||
        ((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) ==
            FP_REG_TYPE_CONST ||
        ((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) ==
            FP_REG_TYPE_CONST;
}

/* RPCS3 (ProgramStateCache analyse_fragment_program) treats a leading NOP
 * with END as a 16-byte "nop shader": its literal slot is never fetched, so
 * a capture may legitimately end right after the instruction. */
static int fp_terminal_nop_without_literal(
    u32 w0, u32 w2, u32 off, u32 max_bytes)
{
    return (w0 & FP_END) && (w0 & FP_OPCODE_MASK) == 0 &&
           !(w2 & FP_BRANCH) && off + 16 > max_bytes;
}

unsigned long long g_rsx_fp_nonfinite_constants = 0;

/* An inline fragment constant is never legitimately non-finite.  A NaN one
 * poisons every pixel the program touches, which on an FP16 HDR target shows
 * up as a saturated white fill rather than as an obviously broken draw.
 * Substitute zero and count it so the underlying bad read stays visible. */
u32 rsx_fp_sanitize_constant_word(u32 w)
{
    float f;
    memcpy(&f, &w, sizeof f);
    if (f == f && f <= 3.0e38f && f >= -3.0e38f)
        return w;
    g_rsx_fp_nonfinite_constants++;
    /* RPCS3 uploads inline constants bit-exact (Inf/NaN included).
     * RSX_FP_SANITIZE_K=1 restores the old zeroing. */
    static int zero = -1;
    if (zero < 0) {
        const char* e = getenv("RSX_FP_SANITIZE_K");
        zero = e && *e && *e != '0';
    }
    return zero ? 0 : w;
}

static void fp_fmt_k(char* out, size_t n, float v)
{
    if (v == v && v <= 3.4028235e38f && v >= -3.4028235e38f) {
        snprintf(out, n, "%.9g", (double)v);
    } else {
        u32 bits;
        memcpy(&bits, &v, sizeof bits);
        snprintf(out, n, "asfloat(0x%08Xu)", bits);
    }
}

int rsx_fp_collect_constants(
    const u8* ucode, u32 max_bytes, rsx_fp_constant_block* out)
{
    if (!ucode || !out)
        return -1;
    memset(out, 0, sizeof(*out));

    u32 off = 0;
    while (off + 16 <= max_bytes) {
        const u32 w0 = rsx_fp_read_word(ucode + off + 0);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        if (fp_instruction_has_constant(w1, w2, w3) &&
            !fp_terminal_nop_without_literal(w0, w2, off, max_bytes)) {
            if (off + 16 > max_bytes ||
                out->count >= RSX_FP_MAX_INLINE_CONSTANTS)
                return -1;
            for (u32 component = 0; component < 4; ++component)
                out->values[out->count][component] =
                    rsx_fp_sanitize_constant_word(
                        rsx_fp_read_word(ucode + off + component * 4));
            out->count++;
            off += 16;
        }
        if (w0 & FP_END)
            return (int)out->count;
    }
    return -1;
}

static u64 fp_hash_bytes(const void* data, u32 size, u64 hash)
{
    const u8* bytes = (const u8*)data;
    for (u32 i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

u64 rsx_fp_structural_hash(const u8* ucode, u32 max_bytes, u64 seed)
{
    if (!ucode)
        return 0;
    static const u32 structural_tag = 0x31535046u; /* "FPS1" */
    u64 hash = fp_hash_bytes(&structural_tag, sizeof(structural_tag), seed);
    u32 off = 0;
    u32 instruction_count = 0;
    u32 constant_count = 0;
    while (off + 16 <= max_bytes) {
        const u32 w0 = rsx_fp_read_word(ucode + off + 0);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        hash = fp_hash_bytes(ucode + off, 16, hash);
        instruction_count++;
        off += 16;
        if (fp_instruction_has_constant(w1, w2, w3) &&
            !fp_terminal_nop_without_literal(w0, w2, off, max_bytes)) {
            if (off + 16 > max_bytes ||
                constant_count >= RSX_FP_MAX_INLINE_CONSTANTS)
                return 0;
            constant_count++;
            off += 16;
        }
        if (w0 & FP_END) {
            hash = fp_hash_bytes(
                &instruction_count, sizeof(instruction_count), hash);
            hash = fp_hash_bytes(
                &constant_count, sizeof(constant_count), hash);
            return hash;
        }
    }
    return 0;
}

u64 rsx_fp_literal_source_hash(
    const u8* ucode, u32 max_bytes, u64 seed)
{
    const u64 structural =
        rsx_fp_structural_hash(ucode, max_bytes, seed);
    if (!structural)
        return 0;
    rsx_fp_constant_block constants;
    if (rsx_fp_collect_constants(ucode, max_bytes, &constants) < 0)
        return 0;
    u64 hash = structural;
    for (u32 slot = 0; slot < constants.count; ++slot) {
        float value[4];
        for (u32 component = 0; component < 4; ++component)
            memcpy(
                &value[component],
                &constants.values[slot][component],
                sizeof(value[component]));
        char literal[128];
        const int length = snprintf(
            literal, sizeof(literal), "float4(%g,%g,%g,%g)",
            value[0], value[1], value[2], value[3]);
        if (length < 0 || (u32)length >= sizeof(literal))
            return 0;
        hash = fp_hash_bytes(literal, (u32)length, hash);
    }
    return hash;
}

int rsx_fp_constant_ring_plan(
    u32 used, u32 capacity, u32 data_bytes,
    u32* out_offset, u32* out_allocation_bytes)
{
    if (!out_offset || !out_allocation_bytes || data_bytes == 0)
        return -1;
    if (data_bytes > 0xFFFFFF00u)
        return -1;
    const u32 allocation = (data_bytes + 255u) & ~255u;
    if (allocation > capacity)
        return -1;
    if (used > capacity || allocation > capacity - used)
        return 0;
    *out_offset = used;
    *out_allocation_bytes = allocation;
    return 1;
}

int rsx_vertex_constant_ring_plan(
    u32 used, u32 capacity, u32 block_bytes, u32* out_offset)
{
    if (!out_offset || block_bytes == 0 || (block_bytes & 255u) != 0 ||
        block_bytes > capacity)
        return -1;
    if (used > capacity || block_bytes > capacity - used)
        return 0;
    *out_offset = used;
    return 1;
}

/* Bounded string appender. */
typedef struct { char* p; u32 cap; u32 len; int ok; } Out;

static void out_puts(Out* o, const char* s)
{
    if (!o->ok) return;
    u32 n = (u32)strlen(s);
    if (o->len + n + 1 > o->cap) { o->ok = 0; return; }
    memcpy(o->p + o->len, s, n);
    o->len += n;
    o->p[o->len] = '\0';
}

/* Replace every `pat` in o's text by `rep`, which is no longer. */
static void out_replace_shorter(Out* o, const char* pat, const char* rep)
{
    const size_t pn = strlen(pat), rn = strlen(rep);
    if (!o->ok || !pn || rn > pn) return;
    char* s = o->p;
    while ((s = strstr(s, pat)) != NULL) {
        memcpy(s, rep, rn);
        memmove(s + rn, s + pn, strlen(s + pn) + 1);
        o->len -= (u32)(pn - rn);
        s += rn;
    }
}

/* Decoded view of one source word. */
typedef struct {
    u32 type;       /* FP_REG_TYPE_* */
    u32 index;
    int half;
    int negate;
    int abs;
    int prec;       /* SRC1 per-source precision modifier (0 = none) */
    char swz[5];    /* e.g. "xyzw" */
} Src;

static void decode_src(u32 w, int abs_bit, Src* s)
{
    static const char comp[4] = {'x','y','z','w'};
    s->type   = (w & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT;
    s->index  = (w & FP_SRC_REG_MASK)  >> FP_SRC_REG_SHIFT;
    s->half   = (w & FP_SRC_HALF) ? 1 : 0;
    s->negate = (w & FP_SRC_NEGATE) ? 1 : 0;
    s->abs    = abs_bit ? 1 : 0;
    s->prec   = 0;
    for (int i = 0; i < 4; i++)
        s->swz[i] = comp[(w >> (FP_SRC_SWZ_SHIFT + 2 * i)) & 3];
    s->swz[4] = '\0';
}

/* HLSL expression for the interpolated input selected by OPDEST.INPUT_SRC.
 * The backend's passthrough VS forwards COL0/COL1/FOG/TEXCOORD0..7; WPOS maps
 * to SV_POSITION (screen-space — an approximation of the RSX window coord). */
static const char* input_expr(u32 input_src)
{
    static const char* tc[8] = {
        "input.tc0", "input.tc1", "input.tc2", "input.tc3",
        "input.tc4", "input.tc5", "input.tc6", "input.tc7" };
    switch (input_src) {
    /* WPOS as the RSX defines it (v125): y counted from the SHADER_WINDOW
     * origin (bottom for most titles, D3D's SV_POSITION is top-down), w = 1/w
     * (GL FragCoord semantics; FXC's SV_POSITION.w is the clip w). See the
     * _rsx_wpos macro in the preamble. */
    case 0x0: return "_rsx_wpos(input.position)"; /* WPOS */
    /* RSX clamps fragment color-input reads before swizzle/abs/negate.
     * Texture coordinates and the vertex output registers remain unbounded. */
    case 0x1: return "saturate(input.col0)"; /* COL0 */
    case 0x2: return "saturate(input.col1)"; /* COL1 */
    case 0x3: return "input.fog";      /* FOGC */
    case 0xC: return "input.tc8";      /* TC8 (VP o15) */
    case 0xD: return "input.tc9";      /* TC9 (VP o6) */
    /* FACING: +1 on a front face, -1 on a back face (RPCS3's `ssa`). Two-sided
     * materials multiply the interpolated normal by it; read as zero, inFAMOUS
     * 2's foliage and character heads normalised a null vector into the
     * G-buffer and lit black. */
    case 0xE: return "(input._rsx_front ? float4(1,1,1,1) : float4(-1,-1,-1,-1))";
    default:
        if (input_src >= 0x4 && input_src <= 0xB) return tc[input_src - 0x4]; /* TC0..7 */
        return "float4(0,0,0,0)";
    }
}

/* v142 (Resistance port): nonzero = an inline constant in the unbuffered form
 * is emitted as fp_k[slot] (slot = the program's n-th constant, < 64) instead
 * of a float4 literal. The D3D12 backend uploads the live values to fp_k[]
 * for every draw (rsx_fp_extract_consts). With literals, every new value a
 * title patched into a program -- a light direction, a fog colour, the camera
 * -- was a different shader: compiled from scratch (30-120 ms), missed the
 * disk cache on every launch, and froze the constant at whatever value the
 * first compile saw. Set by the caller around the decompile call. */
int g_rsx_fp_kslot_mode = 0;

/* Build the swizzled/negated/abs'd HLSL for one source into `buf`. */
static void emit_src(const Src* s, u32 input_src, int persp, const float* k, int has_k,
                     int buffered, u32 constant_slot, const char* dyn_input,
                     char* buf, u32 bufsz)
{
    char base[96];
    if (s->type == FP_REG_TYPE_TEMP) {
        snprintf(base, sizeof(base), "%s[%u]", s->half ? "h" : "r", s->index);
    } else if (s->type == FP_REG_TYPE_INPUT && dyn_input) {
        snprintf(base, sizeof(base), persp ? "(%s * _rsx_inv_w(input.position))" : "%s",
                 dyn_input);
    } else if (s->type == FP_REG_TYPE_INPUT) {
        /* SRC2.perspective_corr on a texcoord read (TC0..TC9) multiplies it
         * by 1/w (RPCS3 FragmentProgramDecompiler: "Perspective correct flag
         * multiplies the result by 1/w"). A vertex program that writes uv*w
         * relies on it: inFAMOUS 2's water samples its screen copy that way,
         * and read raw it lands far outside the copy and the river is black. */
        if (persp && input_src >= 0x4 && input_src <= 0xD)
            snprintf(base, sizeof(base), "(%s * _rsx_inv_w(input.position))",
                     input_expr(input_src));
        else
            snprintf(base, sizeof(base), "%s", input_expr(input_src));
    } else { /* CONST */
        if (buffered && has_k)
            snprintf(
                base, sizeof(base), "fp_constants[%u]", constant_slot);
        else if (has_k && g_rsx_fp_kslot_mode && constant_slot < 64u)
            snprintf(base, sizeof(base), "fp_k[%u]", constant_slot);
        else if (has_k) {
            char kx[32], ky[32], kz[32], kw[32];
            fp_fmt_k(kx, sizeof kx, k[0]); fp_fmt_k(ky, sizeof ky, k[1]);
            fp_fmt_k(kz, sizeof kz, k[2]); fp_fmt_k(kw, sizeof kw, k[3]);
            snprintf(base, sizeof(base), "float4(%s,%s,%s,%s)", kx, ky, kz, kw);
        }
        else
            snprintf(base, sizeof(base), "float4(0,0,0,0)");
    }

    char swz[160];
    snprintf(swz, sizeof(swz), "(%s).%s", base, s->swz);

    /* RSX precision modifiers on a source read: 1 = fp16, 2 = fx12 [-2,2],
     * 3 = fx9 [-1,1], 4 = saturate [0,1] (NaN reads as 0). Saturate applies
     * before abs, the others after it; negate comes last. A half temp is
     * already fp16, and colour inputs are pre-saturated. */
    int prec = s->prec;
    if (s->type == FP_REG_TYPE_TEMP && s->half && prec == 1) prec = 0;
    if (s->type == FP_REG_TYPE_INPUT && (input_src == 1 || input_src == 2 || input_src >= 0xE)) prec = 0;
    if (prec > 4) prec = 0;
    static const char* pc_lim[5] = { "", "", "-2.0, 2.0", "-1.0, 1.0", "0.0, 1.0" };
    char t1[220], t2[240];
    if (prec == 4) { snprintf(t1, sizeof t1, "_rsx_pc(%s, 0.0, 1.0)", swz); }
    else snprintf(t1, sizeof t1, "%s", swz);
    if (s->abs) { snprintf(t2, sizeof t2, "abs(%s)", t1); memcpy(t1, t2, sizeof t1 - 1); t1[sizeof t1 - 1] = 0; }
    /* RPCS3 clamp16: an IEEE f32->f16->f32 round trip (overflow -> inf, NaN kept) */
    if (prec == 1) snprintf(t2, sizeof t2, "_rsx_h16(%s)", t1);
    else if (prec == 2 || prec == 3) snprintf(t2, sizeof t2, "_rsx_pc(%s, %s)", t1, pc_lim[prec]);
    else snprintf(t2, sizeof t2, "%s", t1);
    if (s->negate) snprintf(buf, bufsz, "-(%s)", t2);
    else           snprintf(buf, bufsz, "%s", t2);
}

/* Dest write-mask letters from OPDEST bits 9..12 (X..W). */
static void dest_mask(u32 op0, char* m)
{
    int n = 0;
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 0))) m[n++] = 'x';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 1))) m[n++] = 'y';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 2))) m[n++] = 'z';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 3))) m[n++] = 'w';
    if (n == 0) { m[0] = 'x'; m[1] = 'y'; m[2] = 'z'; m[3] = 'w'; n = 4; }
    m[n] = '\0';
}

/* CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS: when set, fragment output is fp32
 * (color = r0); when clear, output is fp16 (color = h0). Matches RPCS3
 * FragmentProgramDecompiler::BuildCode (gcm_enums.h). */
#define FP_CTRL_32BIT_EXPORTS 0x40u

/* The four colour exports live at fixed register indices, one set per export
 * width: r0/r2/r3/r4 with 32-bit exports and h0/h4/h6/h8 with 16-bit ones
 * (rsx_commands.h, SHADER_CONTROL 0x40; RPCS3 FragmentProgramDecompiler's
 * BuildCode table). Everything else in the file is a plain temp. */
#define FP_MAX_COLOR_TARGETS 4
static const u32 fp_export_reg[2][FP_MAX_COLOR_TARGETS] = {
    { 0, 2, 3, 4 },   /* 32-bit exports: r */
    { 0, 4, 6, 8 },   /* 16-bit exports: h */
};

/* Which colour target a destination register is, or -1 for a plain temp. */
static int fp_export_target(u32 index, int half)
{
    for (int t = 0; t < FP_MAX_COLOR_TARGETS; t++)
        if (fp_export_reg[half][t] == index) return t;
    return -1;
}

int rsx_fp_decompile(const u8* ucode, u32 max_bytes, u32 ctrl, char* out, u32 out_size)
{
    return rsx_fp_decompile_ex(ucode, max_bytes, ctrl, 0u, out, out_size);
}

/* T2-01: RPCS3 shadow compare on float depth, cube units and every filter
 * (see rsx_fp_shadow_desc). RSX_FP_SHADOW_EXT=0 restores the old contract
 * (2D integer depth only, anything else rejects translation). */
static int fp_shadow_ext(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_FP_SHADOW_EXT"); v = !(e && *e == '0'); }
    return v;
}

/* T2-02: RPCS3 texture(samplerShadow, ...) takes an implicit LOD.
 * RSX_FP_SHADOW_LOD=0 samples level 0 as before. */
static int fp_shadow_lod(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_FP_SHADOW_LOD"); v = !(e && *e == '0'); }
    return v;
}

void rsx_fp_shadow_desc_from_texture(rsx_fp_shadow_desc* out, u32 format,
    u32 dimension, u32 cube, u32 mips, u32 filter, u32 address, u32 control0)
{
    memset(out, 0, sizeof *out);
    (void)mips;
    const u32 base = format & 0x9Fu, func = (address >> 28) & 15u;
    if (!(control0 & 0x80000000u) || (control0 & 4u) ||
        (base != 0x90 && base != 0x91 && base != 0x92 && base != 0x93) ||
        func == 0 || func == 7) return;
    const u32 minf = (filter >> 16) & 7u, magf = (filter >> 24) & 7u;
    if (fp_shadow_ext()) {
        /* RPCS3 RSXGraphEngine.cpp:1328-1335: never < zfunc < always. */
        if (func > 7) return;
        /* No RPCS3 compare form for 1D (GLSLCommon.cpp:578-581, unimplemented)
         * or 3D (no sampler3DShadow): the unit samples as a plain texture. */
        if (dimension != 2) return;
        out->enabled = 1;
        out->func = (u8)func;
        /* vk::get_min_filter / get_mag_filter: CONVOLUTION_MIN (7) and
         * CONVOLUTION_MAG (4) are LINEAR. */
        out->linear = (u8)(minf == 2 || minf == 4 || minf == 6 || minf == 7 ||
                           magf == 2 || magf == 4);
        out->unnormalized = (u8)(!cube && (format & 0x40u) != 0);
        out->depth_float = (u8)(base == 0x91 || base == 0x93);
        return;
    }
    const int minlin = minf == 2 || minf == 4 || minf == 6;
    const int maglin = magf == 2;
    /* RPCS3 compares on any mip count or filter pair; this 2D shader compare
     * samples level 0 and filters linearly if either filter is linear. */
    out->enabled = (base == 0x91 || base == 0x93 || func > 7 || dimension != 2 || cube ||
        minf < 1 || minf > 6 || magf < 1 || magf > 2) ? 2 : 1;
    out->func = (u8)func;
    out->linear = (u8)(minlin || maglin);
    out->unnormalized = (u8)((format & 0x40u) != 0);
}

u32 rsx_fp_texture_units(const u8* ucode, u32 max_bytes)
{
    u32 units = 0;
    if (!ucode) return 0;
    for (u32 off = 0; off + 16 <= max_bytes;) {
        const u32 w0 = rsx_fp_read_word(ucode + off);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        const u32 op = (w0 >> 24) & 63u;
        if (!(w2 & FP_BRANCH) && (op == OP_TEX || op == OP_TXP ||
            op == OP_TXB || op == OP_TXL || op == OP_TXD ||
            op == OP_TEXBEM || op == OP_TXPBEM))
            units |= 1u << ((w0 >> FP_TEX_UNIT_SHIFT) & 15u);
        off += 16;
        if (!(w2 & FP_BRANCH) && ((w1 & 3u) == 2 || (w2 & 3u) == 2 || (w3 & 3u) == 2))
            off += 16;
        if (w0 & FP_END) break;
    }
    return units;
}

/* Highest temp index each register file touches ([0] = r, [1] = h). RPCS3
 * declares every rN/hN a program names (AddReg), not a fixed 48: the 6-bit
 * index field reaches 63, and the h file is twice as deep as the r file. */
static void fp_temp_reg_extent(const u8* ucode, u32 max_bytes, u32 max_idx[2])
{
    max_idx[0] = max_idx[1] = 0;
    if (!ucode) return;
    for (u32 off = 0; off + 16 <= max_bytes;) {
        const u32 w[4] = {
            rsx_fp_read_word(ucode + off),
            rsx_fp_read_word(ucode + off + 4),
            rsx_fp_read_word(ucode + off + 8),
            rsx_fp_read_word(ucode + off + 12) };
        off += 16;
        if (!(w[2] & FP_BRANCH)) {
            const u32 d = (w[0] & FP_OUT_REG_MASK) >> FP_OUT_REG_SHIFT;
            const int dh = (w[0] & FP_OUT_HALF) ? 1 : 0;
            if (d > max_idx[dh]) max_idx[dh] = d;
            for (int s = 1; s < 4; s++) {
                if ((w[s] & FP_REG_TYPE_MASK) != FP_REG_TYPE_TEMP) continue;
                const u32 i = (w[s] & FP_SRC_REG_MASK) >> FP_SRC_REG_SHIFT;
                const int sh = (w[s] & FP_SRC_HALF) ? 1 : 0;
                if (i > max_idx[sh]) max_idx[sh] = i;
            }
            if ((w[1] & 3u) == 2 || (w[2] & 3u) == 2 || (w[3] & 3u) == 2)
                off += 16;
        }
        if (w[0] & FP_END) break;
    }
}

/* RPCS3 register annotation skips write-to-self MOV delay slots: same file,
 * no modifiers, identity swizzle on the written lanes. */
static int fp_is_delay_slot(const u32 w[4])
{
    const u32 op = ((w[0] & FP_OPCODE_MASK) >> FP_OPCODE_SHIFT) |
                   ((w[2] & FP_BRANCH) ? 0x40u : 0u);
    const u32 wmask = (w[0] >> FP_OUT_MASK_SHIFT) & 15u;
    const u32 dreg = (w[0] & FP_OUT_REG_MASK) >> FP_OUT_REG_SHIFT;
    const u32 prec = (w[0] >> 22) & 3u;
    int ds = op == OP_MOV && !(w[0] & FP_OUT_NONE) &&
        (w[1] & FP_REG_TYPE_MASK) == FP_REG_TYPE_TEMP &&
        ((w[1] & FP_SRC_REG_MASK) >> FP_SRC_REG_SHIFT) == dreg &&
        !(w[0] & FP_OUT_HALF) == !(w[1] & FP_SRC_HALF) &&
        !(w[1] & (FP_SRC0_ABS | FP_SRC_NEGATE)) && !(w[0] & FP_OUT_SAT) &&
        !((w[2] >> 28) & 7u) &&
        (prec == 0 || (prec == 1 && (w[1] & FP_SRC_HALF)));
    for (u32 c = 0; ds && c < 4; c++)
        if ((wmask & (1u << c)) &&
            ((w[1] >> (FP_SRC_SWZ_SHIFT + 2 * c)) & 3u) != c)
            ds = 0;
    return ds;
}

/* RPCS3 FragmentProgramDecompiler marks a program invalid ucode and compiles
 * it to a bare `discard` (Decompile, m_is_valid_ucode) when its register
 * annotation (Assembler/Passes/FP/RegisterAnnotationPass.cpp,
 * FPOpcodes.cpp) finds an unimplemented opcode or a live temp operand or
 * destination at index >= 48, or GetSRC reads source type 3 or an input
 * attribute past `ssa`. Such programs are corrupt or partially uploaded
 * ucode; real hardware does nothing meaningful with them either. */
static int fp_ucode_valid(const u8* ucode, u32 max_bytes)
{
    if (!ucode) return 0;
    for (u32 off = 0; off + 16 <= max_bytes;) {
        const u32 w[4] = {
            rsx_fp_read_word(ucode + off),
            rsx_fp_read_word(ucode + off + 4),
            rsx_fp_read_word(ucode + off + 8),
            rsx_fp_read_word(ucode + off + 12) };
        off += 16;
        if (!(w[2] & FP_BRANCH) &&
            ((w[1] & 3u) == 2 || (w[2] & 3u) == 2 || (w[3] & 3u) == 2))
            off += 16;
        const u32 op = ((w[0] & FP_OPCODE_MASK) >> FP_OPCODE_SHIFT) |
                       ((w[2] & FP_BRANCH) ? 0x40u : 0u);
        if (op != OP_NOP) {
            u32 nsrc = 0;
            int masked = 0;   /* lane mask is ANDed with the write mask */
            switch (op) {
            case 0x26: case 0x2B: case 0x33: case 0x34: case 0x35: case 0x37:
                return 0;     /* POW, BEM, TEXBEM, TXPBEM, BEMLUM, TIMESWTEX */
            case OP_MOV: case OP_FRC: case OP_FLR: case OP_DDX: case OP_DDY:
                nsrc = 1; masked = 1; break;
            case OP_MUL: case OP_ADD: case OP_MIN: case OP_MAX:
            case OP_SLT: case OP_SGE: case OP_SLE: case OP_SGT:
            case OP_SNE: case OP_SEQ: case OP_DIV: case OP_DIVSQ:
                nsrc = 2; masked = 1; break;
            case OP_MAD: case OP_LRP:
                nsrc = 3; masked = 1; break;
            case OP_DP3: case OP_DP4: case OP_DST: case OP_TXL: case OP_TXB:
            case OP_RFL: case OP_DP2:
                nsrc = 2; break;
            case OP_DP2A:
                nsrc = 3; break;
            case OP_PK4B: case OP_UP4B: case OP_TEX: case OP_TXP: case OP_TXD:
            case OP_RCP: case OP_RSQ: case OP_EX2: case OP_LG2: case OP_LIT:
            case OP_COS: case OP_SIN: case OP_PK2H: case OP_UP2H:
            case OP_PK4UB: case OP_UP4UB: case OP_PK2US: case OP_UP2US:
            case OP_PKG: case OP_UPG: case OP_NRM: case OP_LIF:
                nsrc = 1; break;
            default:
                if (op > 0x47) return 0;
                break;
            }
            const int no_dest = (w[0] & FP_OUT_NONE) != 0;
            const u32 wmask = (w[0] >> FP_OUT_MASK_SHIFT) & 15u;
            const u32 dreg = (w[0] & FP_OUT_REG_MASK) >> FP_OUT_REG_SHIFT;
            const int delay_slot = fp_is_delay_slot(w);
            const u32 nread = op == OP_TXD ? 3 : nsrc;
            for (u32 s = 0; s < nread; s++) {
                const u32 sw = w[1 + s];
                const u32 type = sw & FP_REG_TYPE_MASK;
                if (type == 3) return 0;
                if (type == FP_REG_TYPE_INPUT) {
                    const u32 attr = (w[3] & (1u << 30))
                        ? ((w[3] >> 19) & 0x7FFu) + 4
                        : (w[0] & FP_INPUT_SRC_MASK) >> FP_INPUT_SRC_SHIFT;
                    if (attr >= 15) return 0;
                }
                if (type == FP_REG_TYPE_TEMP && s < nsrc && !delay_slot &&
                    (!masked || (!no_dest && wmask)) &&
                    ((sw & FP_SRC_REG_MASK) >> FP_SRC_REG_SHIFT) >= 48)
                    return 0;
            }
            if (!no_dest && !delay_slot && dreg >= 48)
                return 0;
        }
        if (w[0] & FP_END) break;
    }
    return 1;
}

/* Channels of temp register an operand reads, swizzle applied (RPCS3
 * FPOpcodes.cpp get_src_vector_lane_mask_shuffled). tex_wide: the fetch's
 * unit is 3D or cube. */
static u32 fp_src_lane_mask(const u32 w[4], u32 operand, int tex_wide)
{
    const u32 op = (w[0] & FP_OPCODE_MASK) >> FP_OPCODE_SHIFT;
    const u32 wm = (w[0] & FP_OUT_NONE) ? 0u : (w[0] >> FP_OUT_MASK_SHIFT) & 15u;
    u32 m = 0;
    switch (op) {
    case OP_MOV: case OP_FRC: case OP_FLR: case OP_DDX: case OP_DDY:
        m = operand < 1 ? wm : 0; break;
    case OP_MUL: case OP_ADD: case OP_MIN: case OP_MAX:
    case OP_SLT: case OP_SGE: case OP_SLE: case OP_SGT: case OP_SNE: case OP_SEQ:
        m = operand < 2 ? wm : 0; break;
    case OP_MAD: case OP_LRP:
        m = wm; break;
    case OP_DIV: case OP_DIVSQ:
        m = operand == 0 ? wm : operand == 1 ? (wm & 1u) : 0; break;
    case OP_DP3: m = operand < 2 ? 7u : 0; break;
    case OP_DP4: m = operand < 2 ? 15u : 0; break;
    case OP_DST: m = operand == 0 ? 6u : operand == 1 ? 10u : 0; break;
    case OP_RFL: m = operand < 2 ? 15u : 0; break;
    case OP_DP2: m = operand < 2 ? 3u : 0; break;
    case OP_DP2A: m = operand < 2 ? 3u : 1u; break;
    case OP_TXL: case OP_TXB: m = operand == 0 ? 3u : operand == 1 ? 1u : 0; break;
    case OP_PK4B: case OP_PK4UB: case OP_PKG: case OP_LIT:
        m = operand == 0 ? 15u : 0; break;
    case OP_PK2H: case OP_PK2US: m = operand == 0 ? 3u : 0; break;
    case OP_UP4B: case OP_UP4UB: case OP_UPG: case OP_UP2H: case OP_UP2US:
    case OP_RCP: case OP_RSQ: case OP_EX2: case OP_LG2: case OP_COS: case OP_SIN:
        m = operand == 0 ? 1u : 0; break;
    case OP_NRM: m = operand == 0 ? 7u : 0; break;
    case OP_LIF: m = operand == 0 ? 10u : 0; break;
    case OP_TEX: case OP_TXD: m = operand == 0 ? (tex_wide ? 7u : 3u) : 0; break;
    case OP_TXP: m = operand == 0 ? (tex_wide ? 15u : 7u) : 0; break;
    default: break;
    }
    const u32 sw = w[1 + operand];
    if (!m || (sw & FP_REG_TYPE_MASK) != FP_REG_TYPE_TEMP) return 0;
    u32 r = 0;
    for (u32 c = 0; c < 4; c++)
        if (m & (1u << c)) r |= 1u << ((sw >> (FP_SRC_SWZ_SHIFT + 2 * c)) & 3u);
    return r;
}

/* Branch joins (RPCS3 RegisterDependencyPass.cpp:373-444): a lane on which an
 * IFE's two paths disagree becomes pending on a join record (rf value 4 + j)
 * holding each path's state and the marker line at that path's end. A later
 * read resolves it lazily: each path whose lane was last written through the
 * other file gets the barrier at its end marker, as RPCS3 appends it to the
 * clobbering predecessor's epilogue (only for lanes some successor reads). A
 * path without an else ends before the `if` (the anchor's epilogue,
 * FragmentProgramDecompiler.cpp:1424-1436). Markers are stripped afterwards. */
#define FP_JOIN_MAX 128u
typedef struct { u16 st[2][384]; char mk[2][24]; } FpJoin;
typedef struct { FpJoin* j; u32 n; } FpJoins;

static void fp_insert_at_marker(Out* o, const char* marker, const char* text)
{
    if (!o->ok) return;
    char* m = strstr(o->p, marker);
    if (!m) return;
    const size_t tl = strlen(text);
    if ((size_t)o->len + tl + 1 > o->cap) { o->ok = 0; return; }
    memmove(m + tl, m, (size_t)o->len - (size_t)(m - o->p) + 1);
    memcpy(m, text, tl);
    o->len += (u32)tl;
}

static void fp_alias_lane_barrier(u32 u, int f16, char* line, size_t n)
{
    static const char cn[4] = { 'x', 'y', 'z', 'w' };
    if (f16) {
        const u32 id = u >> 2, c = u & 3;
        snprintf(line, n, "    h[%u].%c = f16tof32(asuint(r[%u].%c) >> %uu);\n",
                 id, cn[c], id >> 1, cn[2 * (id & 1) + c / 2], 16 * (c & 1));
    } else {
        const u32 id = u >> 3, ch = (u & 7) >> 1, hid = 2 * id + ch / 2;
        if (!(u & 1))
            snprintf(line, n,
                     "    r[%u].%c = asfloat((asuint(r[%u].%c) & 0xFFFF0000u) | (f32tof16(h[%u].%c) & 0xFFFFu));\n",
                     id, cn[ch], id, cn[ch], hid, (ch & 1) ? 'z' : 'x');
        else
            snprintf(line, n,
                     "    r[%u].%c = asfloat((asuint(r[%u].%c) & 0xFFFFu) | (f32tof16(h[%u].%c) << 16));\n",
                     id, cn[ch], id, cn[ch], hid, (ch & 1) ? 'w' : 'y');
    }
}

static void fp_alias_resolve(Out* o, FpJoins* J, u32 j, u32 u, int f16, int depth)
{
    if (!J || j >= J->n || depth > 64) return;
    FpJoin* jn = &J->j[j];
    for (u32 p = 0; p < 2; p++) {
        const u16 s = jn->st[p][u];
        if (s == (f16 ? 2 : 1)) {
            char line[160];
            fp_alias_lane_barrier(u, f16, line, sizeof line);
            fp_insert_at_marker(o, jn->mk[p], line);
            jn->st[p][u] = 3;
        } else if (s >= 4) {
            fp_alias_resolve(o, J, s - 4u, u, f16, depth + 1);
        }
    }
}

/* Strip the join marker lines. */
static void fp_strip_join_markers(Out* o)
{
    if (!o->ok) return;
    char* m;
    while ((m = strstr(o->p, "    /*@B")) != NULL) {
        char* e = strstr(m, "*/\n");
        if (!e) return;
        e += 3;
        memmove(m, e, (size_t)o->len - (size_t)(e - o->p) + 1);
        o->len -= (u32)(e - m);
    }
}

/* RPCS3 RegisterDependencyPass: h(2n) and h(2n+1) are the two halves of
 * r(n) in one 768-byte register file. rf[] holds, per 16-bit lane, which
 * file last wrote it (0 unknown, 1 h, 2 r, 3 both, 4 + j pending on join j).
 * Reading a lane last written through the other file first converts it (UP2 /
 * PK2 / OR16 barriers), emitted before the instruction: h barriers, then r. */
static void fp_alias_barriers(Out* o, u16 rf[384], FpJoins* J, const u32 w[4], int tex_wide)
{
    u32 b16[12] = {0}, b32[12] = {0};
    for (u32 s = 0; s < 3; s++) {
        const u32 m = fp_src_lane_mask(w, s, tex_wide);
        const u32 id = (w[1 + s] & FP_SRC_REG_MASK) >> FP_SRC_REG_SHIFT;
        if (!m || id >= 48) continue;
        const int f16 = (w[1 + s] & FP_SRC_HALF) != 0;
        for (u32 c = 0; c < 4; c++) {
            if (!(m & (1u << c))) continue;
            for (u32 k = 0; k < (f16 ? 1u : 2u); k++) {
                const u32 u = f16 ? 4 * id + c : 8 * id + 2 * c + k;
                const u16 t = rf[u];
                if (t >= 4) { fp_alias_resolve(o, J, t - 4u, u, f16, 0); continue; }
                if (!t || t == 3 || t == (f16 ? 1 : 2)) continue;
                rf[u] = 3;
                (f16 ? b16 : b32)[u >> 5] |= 1u << (u & 31);
            }
        }
    }
    if (!(w[0] & FP_OUT_NONE)) {
        const u32 id = (w[0] & FP_OUT_REG_MASK) >> FP_OUT_REG_SHIFT;
        const u32 wm = (w[0] >> FP_OUT_MASK_SHIFT) & 15u;
        const int f16 = (w[0] & FP_OUT_HALF) != 0;
        for (u32 c = 0; id < 48 && c < 4; c++) {
            if (!(wm & (1u << c))) continue;
            if (f16) rf[4 * id + c] = 1;
            else rf[8 * id + 2 * c] = rf[8 * id + 2 * c + 1] = 2;
        }
    }
    static const char cn[4] = { 'x', 'y', 'z', 'w' };
    char line[160];
    for (u32 u = 0; u < 384; u++) {
        if (!((b16[u >> 5] >> (u & 31)) & 1u)) continue;
        const u32 id = u >> 2, c = u & 3;
        snprintf(line, sizeof line,
                 "    h[%u].%c = f16tof32(asuint(r[%u].%c) >> %uu);\n",
                 id, cn[c], id >> 1, cn[2 * (id & 1) + c / 2], 16 * (c & 1));
        out_puts(o, line);
    }
    for (u32 u = 0; u < 384; u += 2) {
        const u32 lo = (b32[u >> 5] >> (u & 31)) & 1u;
        const u32 hi = (b32[u >> 5] >> ((u + 1) & 31)) & 1u;
        if (!lo && !hi) continue;
        const u32 id = u >> 3, ch = (u & 7) >> 1, hid = 2 * id + ch / 2;
        const char hl = (ch & 1) ? 'z' : 'x', hh = (ch & 1) ? 'w' : 'y';
        if (lo && hi)
            snprintf(line, sizeof line,
                     "    r[%u].%c = asfloat(f32tof16(h[%u].%c) | (f32tof16(h[%u].%c) << 16));\n",
                     id, cn[ch], hid, hl, hid, hh);
        else if (lo)
            snprintf(line, sizeof line,
                     "    r[%u].%c = asfloat((asuint(r[%u].%c) & 0xFFFF0000u) | (f32tof16(h[%u].%c) & 0xFFFFu));\n",
                     id, cn[ch], id, cn[ch], hid, hl);
        else
            snprintf(line, sizeof line,
                     "    r[%u].%c = asfloat((asuint(r[%u].%c) & 0xFFFFu) | (f32tof16(h[%u].%c) << 16));\n",
                     id, cn[ch], id, cn[ch], hid, hh);
        out_puts(o, line);
    }
}

static void fp_shadow_helper(Out* p, u32 unit, u32 cube_mask,
                              const rsx_fp_shadow_desc* d, int rdyn, int buffered, int cube)
{
    char tex[32], text[4096];
    snprintf(tex, sizeof tex, cube_mask ? "rsx_tex%u" : "rsx_tex[%u]", unit);
    const int ext = fp_shadow_ext(), lod = ext && fp_shadow_lod();
    /* RPCS3 _texcoord_xform_shadow: DEPTH_FLOAT keeps the reference as is;
     * integer depth clamps it to 1 and Vulkan clamps a UNORM Dref to [0,1]. */
    const int dfloat = ext && d->depth_float;
    /* RSX texture ZFUNC runs the other way round from the D3D/GL/Vulkan
     * sampler compare (RPCS3: get_compare_func(zfunc, reverse_direction=true)).
     * Taken literally, the game's GEQUAL shadow test lit only the pixels
     * under a caster and darkened everything else in the projected area. */
    const char* cmp[] = { "0.0", "reference > depth", "reference == depth",
        "reference >= depth", "reference < depth", "reference != depth",
        "reference <= depth", "1.0" };
    const u32 func = d->func & 7u;
    if (cube) {
        /* RPCS3 TEX3D_SHADOW: texture(samplerCubeShadow, vec4(xyz, w)), the
         * direction xyz compared against w. The bound sampler filters. */
        snprintf(text, sizeof text,
            "float _rsx_shadow_cmp%u(float reference, float depth) { return (float)(%s); }\n"
            "float4 _rsx_shadow%u(float4 coordinate) {\n"
            "    float reference = %s;\n"
            "    float depth = %s.%s(rsx_samp[%u], coordinate.xyz%s).r;\n"
            "    return (float4)_rsx_shadow_cmp%u(reference, depth);\n}\n",
            unit, cmp[func], unit, dfloat ? "coordinate.w" : "saturate(coordinate.w)",
            tex, lod ? "Sample" : "SampleLevel", unit, lod ? "" : ", 0.0", unit);
        out_puts(p, text);
        return;
    }
    snprintf(text, sizeof text,
        "float _rsx_shadow_cmp%u(float reference, float depth) { return (float)(%s); }\n"
        "float4 _rsx_shadow%u(float3 coordinate) {\n"
        "    float reference = %s;\n",
        unit, cmp[func], unit, dfloat ? "coordinate.z" : "saturate(coordinate.z)");
    out_puts(p, text);
    if (d->linear || d->unnormalized) {
        snprintf(text, sizeof text, "    uint width, height; %s.GetDimensions(width, height);\n"
            "    float2 size = float2(width, height);\n", tex);
        out_puts(p, text);
        if (d->unnormalized && rdyn) {
            snprintf(text, sizeof text, "    size *= fp_rscale[%u].%c;\n", 1u + unit / 4u, "xyzw"[unit & 3u]);
            out_puts(p, text);
        } else if (d->unnormalized && fp_res_inv() != 1.0) {   /* v136: texel addresses are guest texels */
            snprintf(text, sizeof text, "    size *= %.9g;\n", fp_res_inv());
            out_puts(p, text);
        }
    }
    out_puts(p, !d->unnormalized ? "    float2 uv = coordinate.xy;\n" :
                (!d->linear && !rsx_texture_legacy())   /* RPCS3 RSXGraphEngine.cpp:1265-1278 */
                    ? "    float2 uv = (coordinate.xy + 0.01) / (size * rsx_shscale.xy);\n"
                    : "    float2 uv = coordinate.xy / (size * rsx_shscale.xy);\n");
    /* Implicit LOD, taken in uniform control flow (before the early-out). */
    if (lod) {
        snprintf(text, sizeof text,
            "    float _lod = %s.CalculateLevelOfDetail(rsx_samp[%u], uv * rsx_shscale.xy);\n", tex, unit);
        out_puts(p, text);
    }
    /* Outside the map = lit on the legacy backends, whose samplers are static
     * WRAP: a shadow map (the game sets BORDER) tiled its caster across the
     * whole receiver. The engine's samplers carry the unit's wrap modes and
     * border colour, so outside the map compares what the address mode
     * fetches -- the edge texel, the tiled map or the border depth -- as
     * RPCS3's compare sampler does (VKDraw.cpp:406-445). */
    if (!buffered || rsx_texture_legacy())
        out_puts(p, "    if (any(uv < 0.0) || any(uv > 1.0)) return (float4)1.0;\n");
    out_puts(p, "    uv *= rsx_shscale.xy;\n");
    if (!d->linear) {
        snprintf(text, sizeof text,
            "    float depth = %s.SampleLevel(rsx_samp[%u], uv, %s).r;\n"
            "    return (float4)_rsx_shadow_cmp%u(reference, depth);\n}\n",
            tex, unit, lod ? "_lod" : "0.0", unit);
        out_puts(p, text);
        return;
    }
    if (lod) {
        /* Compared taps on the selected level (its size in guest texels). */
        snprintf(text, sizeof text,
            "    uint _w0, _h0, _nl; %s.GetDimensions(0u, _w0, _h0, _nl);\n"
            "    float _lv = clamp(floor(_lod + 0.5), 0.0, (float)(_nl - 1u));\n"
            "    float2 _ls = max(floor(size / exp2(_lv)), 1.0);\n"
            "    float2 position = uv * _ls - 0.5;\n"
            "    float2 base = floor(position); float2 fraction = position - base;\n", tex);
        out_puts(p, text);
    } else
    out_puts(p, "    float2 position = uv * size - 0.5;\n"
        "    float2 base = floor(position); float2 fraction = position - base;\n");
    for (u32 tap = 0; tap < 4; tap++) {
        snprintf(text, sizeof text,
            "    float d%u = %s.SampleLevel(rsx_samp[%u], (base + float2(%s, %s)) / %s, %s).r;\n"
            "    float c%u = _rsx_shadow_cmp%u(reference, d%u);\n",
            tap, tex, unit, (tap & 1) ? "1.5" : "0.5", (tap & 2) ? "1.5" : "0.5",
            lod ? "_ls" : "size", lod ? "_lv" : "0.0", tap, unit, tap);
        out_puts(p, text);
    }
    out_puts(p, "    return (float4)lerp(lerp(c0, c1, fraction.x), lerp(c2, c3, fraction.x), fraction.y);\n}\n");
}

/* EXP_TEX overrides gamma on compatible normalized color formats. Metadata
 * has already excluded formats without gamma support. */
u32 rsx_fp_texture_gamma_units(const u8* ucode, u32 max_bytes,
                                const u8 gamma_masks[16])
{
    if (!ucode || !gamma_masks) return 0;
    u32 units = 0;
    for (u32 off = 0; off + 16 <= max_bytes;) {
        const u32 w0 = rsx_fp_read_word(ucode + off);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        const u32 op = (w0 >> 24) & 63u, unit = (w0 >> FP_TEX_UNIT_SHIFT) & 15u;
        if (!(w2 & FP_BRANCH) && !(w0 & (1u << 21)) && gamma_masks[unit] &&
            (op == OP_TEX || op == OP_TXP || op == OP_TXB || op == OP_TXL ||
             op == OP_TXD || op == OP_TEXBEM || op == OP_TXPBEM))
            units |= 1u << unit;
        off += 16;
        if (!(w2 & FP_BRANCH) && ((w1 & 3u) == 2 || (w2 & 3u) == 2 || (w3 & 3u) == 2)) off += 16;
        if (w0 & FP_END) return units;
    }
    return 0;
}

static void fp_gamma_helper(Out* out)
{
    out_puts(out,
        "float4 _rsx_gamma_input(float4 v, uint mask) {\n"
        "    float4 lo = v / 12.92;\n"
        "    float4 hi = pow(max((v + 0.055) / 1.055, (float4)0), (float4)2.4);\n"
        "    float4 decoded = float4(v.r > 0.04045 ? hi.r : lo.r,\n"
        "                            v.g > 0.04045 ? hi.g : lo.g,\n"
        "                            v.b > 0.04045 ? hi.b : lo.b,\n"
        "                            v.a > 0.04045 ? hi.a : lo.a);\n"
        "    return float4((mask & 1u) ? decoded.r : v.r,\n"
        "                  (mask & 2u) ? decoded.g : v.g,\n"
        "                  (mask & 4u) ? decoded.b : v.b,\n"
        "                  (mask & 8u) ? decoded.a : v.a);\n}\n");
}

/* Texture sign expansion: units a TEX/TXP/TXB/TXL fetches from whose
 * expand mask is set. The same walk as the gamma one. */
u32 rsx_fp_texture_expand_units(const u8* ucode, u32 max_bytes,
                                 const u8 expand_masks[16])
{
    if (!ucode || !expand_masks) return 0;
    u32 units = 0;
    for (u32 off = 0; off + 16 <= max_bytes;) {
        const u32 w0 = rsx_fp_read_word(ucode + off);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        const u32 op = (w0 >> 24) & 63u, unit = (w0 >> FP_TEX_UNIT_SHIFT) & 15u;
        if (!(w2 & FP_BRANCH) && expand_masks[unit] &&
            (op == OP_TEX || op == OP_TXP || op == OP_TXB || op == OP_TXL || op == OP_TXD))
            units |= 1u << unit;
        off += 16;
        if (!(w2 & FP_BRANCH) && ((w1 & 3u) == 2 || (w2 & 3u) == 2 || (w3 & 3u) == 2)) off += 16;
        if (w0 & FP_END) return units;
    }
    return 0;
}

/* Units a TEX/TXP/TXB/TXL reads with EXP_TEX (OPDEST bit 21, _bx2). */
u32 rsx_fp_texture_bx2_units(const u8* ucode, u32 max_bytes)
{
    if (!ucode) return 0;
    u32 units = 0;
    for (u32 off = 0; off + 16 <= max_bytes;) {
        const u32 w0 = rsx_fp_read_word(ucode + off);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        const u32 op = (w0 >> 24) & 63u, unit = (w0 >> FP_TEX_UNIT_SHIFT) & 15u;
        if (!(w2 & FP_BRANCH) && (w0 & (1u << 21)) &&
            (op == OP_TEX || op == OP_TXP || op == OP_TXB || op == OP_TXL || op == OP_TXD))
            units |= 1u << unit;
        off += 16;
        if (!(w2 & FP_BRANCH) && ((w1 & 3u) == 2 || (w2 & 3u) == 2 || (w3 & 3u) == 2)) off += 16;
        if (w0 & FP_END) return units;
    }
    return 0;
}

static void fp_expand_helper(Out* out)
{
    out_puts(out,
        "float4 _rsx_expand_input(float4 v, uint mask, bool bx2) {\n"
        "    float4 e = bx2 ? v * 2.0 - 1.0\n"
        "             : (mask & 16u) ? (floor(v * 65535.0 + 0.5) - 32768.0) / 32767.0\n"
        "                            : (floor(v * 255.0 + 0.5) - 128.0) / 127.0;\n"
        "    return float4((mask & 1u) ? e.r : v.r,\n"
        "                  (mask & 2u) ? e.g : v.g,\n"
        "                  (mask & 4u) ? e.b : v.b,\n"
        "                  (mask & 8u) ? e.a : v.a);\n}\n");
}

static int rsx_fp_decompile_core(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask, u32 tex_vol_mask,
    int buffered, const rsx_fp_shadow_desc* shadow, const u8 gamma_masks[16],
    const u8* expand_masks, const rsx_fp_frag_state* fs, int eng,
    char* out, u32 out_size, u32* out_constant_count);

static int rsx_fp_decompile_internal(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask, u32 tex_vol_mask,
    int buffered, const rsx_fp_shadow_desc* shadow, const u8 gamma_masks[16],
    const u8* expand_masks,
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_core(ucode, max_bytes, ctrl, tex_cube_mask, tex_vol_mask,
        buffered, shadow, gamma_masks, expand_masks, NULL, 0, out, out_size,
        out_constant_count);
}

int rsx_fp_decompile_engine(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask, u32 tex_vol_mask,
    const rsx_fp_shadow_desc shadow[16], const u8 gamma_masks[16],
    const u8 expand_masks[16], const rsx_fp_frag_state* fs,
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_core(ucode, max_bytes, ctrl, tex_cube_mask, tex_vol_mask,
        1, shadow, gamma_masks, expand_masks, fs, 1, out, out_size, out_constant_count);
}

u32 rsx_fp_input_mask(const u8* ucode, u32 max_bytes)
{
    u32 mask = 0;
    if (!ucode) return 0;
    for (u32 off = 0; off + 16 <= max_bytes;) {
        const u32 w0 = rsx_fp_read_word(ucode + off);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        if (!(w2 & FP_BRANCH) &&
            ((w1 & FP_REG_TYPE_MASK) == FP_REG_TYPE_INPUT ||
             (w2 & FP_REG_TYPE_MASK) == FP_REG_TYPE_INPUT ||
             (w3 & FP_REG_TYPE_MASK) == FP_REG_TYPE_INPUT))
            mask |= 1u << ((w0 & FP_INPUT_SRC_MASK) >> FP_INPUT_SRC_SHIFT);
        /* SRC2.use_index_reg: an indexed read (_rsx_iload) may reach any TC. */
        if (!(w2 & FP_BRANCH) && ((w3 >> 30) & 1u) &&
            ((w1 & FP_REG_TYPE_MASK) == FP_REG_TYPE_INPUT ||
             (w2 & FP_REG_TYPE_MASK) == FP_REG_TYPE_INPUT ||
             (w3 & FP_REG_TYPE_MASK) == FP_REG_TYPE_INPUT))
            mask |= 0x3FFu << 4;
        off += 16;
        if (!(w2 & FP_BRANCH) && ((w1 & 3u) == 2 || (w2 & 3u) == 2 || (w3 & 3u) == 2))
            off += 16;
        if (w0 & FP_END) break;
    }
    return mask;
}

/* RPCS3 fragment_texture::format_ex: argb_signed_ =
 * decoded_remap().shuffle_mask_bits(argb_signed()) on the formats with
 * RSX_FORMAT_FEATURE_SIGNED_COMPONENTS (the 8-bit colour formats and
 * Y16_X16). Same lane walk as rsx_texture_gamma_mask. */
u32 rsx_texture_control1(u32 control1);
u8 rsx_fp_texture_sext_mask(u32 format, u32 control1, u32 filter)
{
    int wide = 0;
    switch (format & 0x9Fu) {
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x88: case 0x8B: case 0x8D: case 0x8E: case 0x8F:
    case 0x97: case 0x9D: case 0x9E: break;
    case 0x95: wide = 1; break;
    default: return 0;
    }
    if (!((filter >> 28) & 15u)) return 0;
    control1 = rsx_texture_control1(control1);
    if (!rsx_texture_legacy()) control1 = rsx_texture_decoded_remap(control1, format);
    u8 remap[4]; rsx_texture_component_remap(control1, format, remap);
    static const u8 rgba_lane[4] = {3, 0, 1, 2};
    u8 mask = 0;
    for (u32 i = 0; i < 4; ++i) {
        const u32 source = (control1 >> (2u * i)) & 3u;
        if (remap[i] < RSX_REMAP_ZERO && ((filter >> (28u + source)) & 1u))
            mask |= (u8)(1u << rgba_lane[i]);
    }
    if (mask && wide) mask |= 0x10u;
    return mask;
}

#define FP_FS_MAIN_END "    /*_rsx_fs_main_end*/\n"
#define FP_CMPFIX_DEFINE "#define _rsx_cmpfix(a) (sign(a) * 16.0 + (a))\n"

/* RET programs, after RPCS3 VKFragmentProgram.cpp:455-606: the decoded body
 * becomes `void _rsx_fs_main(PSInput input)` over static register files, and
 * main runs the prologue, calls it, then exports. `out` holds the register
 * declarations [0, decl_end), the body up to FP_FS_MAIN_END, then the export
 * tail; `pre` is the preamble, whose main signature starts at main_sig_off.
 * Returns 1 when assembled into `out`. */
static int fp_split_fs_main(char* out, u32 out_size, u32 o_len, u32 decl_end,
                            const char* pre, u32 main_sig_off, u32 pre_len,
                            u32 nr, u32 nh, const char* decl_init, int ucode_bad,
                            int dbg)
{
    char* mark = strstr(out + decl_end, FP_FS_MAIN_END);
    if (!mark) return 0;
    const u32 body_len = (u32)(mark - out) - decl_end;
    const char* tail = mark + strlen(FP_FS_MAIN_END);
    const u32 tail_len = o_len - (u32)(tail - out);
    char head[768];
    const int hn = snprintf(head, sizeof head,
        "static float4 r[%u]; static float4 h[%u];\n"
        "static float4 cc0; static float4 cc1;\n"
        "%s"
        "void _rsx_fs_main(PSInput input) {\n"
        "%s%s"
        "    cc0 = (float4)0; cc1 = (float4)0;\n"
        "%s",
        nr, nh, dbg ? "static float4 _rsx_dbg;\n" : "", decl_init,
        ucode_bad ? "    discard;\n" : "", dbg ? "    _rsx_dbg = (float4)0;\n" : "");
    if (hn < 0 || (u32)hn >= sizeof head) return 0;
    static const char close_fn[] = "}\n";
    static const char call[] = "    _rsx_fs_main(input);\n";
    const u32 total = main_sig_off + (u32)hn + body_len + (u32)(sizeof close_fn - 1) +
                      (pre_len - main_sig_off) + (u32)(sizeof call - 1) + tail_len;
    if (total + 1 > out_size) return 0;
    char* save = (char*)malloc((size_t)body_len + tail_len + 1);
    if (!save) return 0;
    memcpy(save, out + decl_end, body_len);
    memcpy(save + body_len, tail, tail_len);
    char* w = out;
    memcpy(w, pre, main_sig_off); w += main_sig_off;
    memcpy(w, head, (size_t)hn); w += hn;
    memcpy(w, save, body_len); w += body_len;
    memcpy(w, close_fn, sizeof close_fn - 1); w += sizeof close_fn - 1;
    memcpy(w, pre + main_sig_off, pre_len - main_sig_off); w += pre_len - main_sig_off;
    memcpy(w, call, sizeof call - 1); w += sizeof call - 1;
    memcpy(w, save + body_len, tail_len); w += tail_len;
    *w = '\0';
    free(save);
    return 1;
}

/* RPCS3 FragmentProgramDecompiler.cpp:972-983: with has_low_precision_rounding
 * (VKFragmentProgram.cpp:638, is_NVIDIA) _builtin_floor(x) is
 * floor(x + 0.000001), used by FLR (FragmentProgramDecompiler.cpp:1118).
 * RSX_FP_FLR_BIAS=0 restores plain floor. */
static int fp_flr_bias(void)
{
    static int bias = -1;
    if (bias < 0) {
        const char* e = getenv("RSX_FP_FLR_BIAS");
        bias = !(e && *e == '0');
    }
    return bias;
}

static int rsx_fp_decompile_core_j(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask, u32 tex_vol_mask,
    int buffered, const rsx_fp_shadow_desc* shadow, const u8 gamma_masks[16],
    const u8* expand_masks, const rsx_fp_frag_state* fs, int eng,
    char* out, u32 out_size, u32* out_constant_count, FpJoins* J);

static int rsx_fp_decompile_core(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask, u32 tex_vol_mask,
    int buffered, const rsx_fp_shadow_desc* shadow, const u8 gamma_masks[16],
    const u8* expand_masks, const rsx_fp_frag_state* fs, int eng,
    char* out, u32 out_size, u32* out_constant_count)
{
    FpJoins J = { NULL, 0 };
    const int r = rsx_fp_decompile_core_j(ucode, max_bytes, ctrl, tex_cube_mask, tex_vol_mask,
                                          buffered, shadow, gamma_masks, expand_masks, fs, eng,
                                          out, out_size, out_constant_count, &J);
    free(J.j);
    return r;
}

static int rsx_fp_decompile_core_j(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask, u32 tex_vol_mask,
    int buffered, const rsx_fp_shadow_desc* shadow, const u8 gamma_masks[16],
    const u8* expand_masks, const rsx_fp_frag_state* fs, int eng,
    char* out, u32 out_size, u32* out_constant_count, FpJoins* J)
{
    if (!ucode || !out || out_size == 0) return -1;
    if (!buffered) fs = NULL;   /* the dynamic state lives in fp_alpha */
    const int lpt = fs && fs->low_precision_tests && !fs->host_not_nvidia;
    /* RPCS3 has_low_precision_rounding: NVIDIA hosts only (H2-01). */
    const int flr_bias = fp_flr_bias() && !(fs && fs->host_not_nvidia);
    const int rdyn = eng && buffered && s_fp_res_dyn;
    /* Cube and volume units are declared on their own t-register; with none
     * the 2D array keeps every 2D-only program byte-identical. */
    const u32 tex_sep_mask = tex_cube_mask | tex_vol_mask;
    /* 1D units stay Texture2D (height 1) sampled at y = 0.5: T never
     * reaches the result, so filtering, wrap, border and LOD are 1D's. */
    const u32 tex_1d_mask = s_fp_tex_1d_mask & ~tex_sep_mask;
    const u32 gamma_units = rsx_fp_texture_gamma_units(ucode, max_bytes, gamma_masks);
    const u32 expand_units = rsx_fp_texture_expand_units(ucode, max_bytes, expand_masks);
    /* _bx2 (EXP_TEX) fetches expand whatever unit they read: the modifier is
     * the instruction's, not the texture's. inFAMOUS 2 stores tangent-space
     * normal maps and its occlusion-cone LUTs unsigned and reads them with
     * TEX_bx2; about a quarter of its fragment programs do. */
    const u32 bx2_units = rsx_fp_texture_bx2_units(ucode, max_bytes);

    const u32 sampled_units = rsx_fp_texture_units(ucode, max_bytes);
    /* Per-unit comparison form: 0 = plain sample, 1 = 2D compare, 2 = cube
     * compare (RPCS3 TEX3D_SHADOW). 1D and 3D units, and a zfunc past
     * ALWAYS, have no RPCS3 compare form and sample normally. */
    u8 shc[16] = {0};
    if (shadow) for (u32 u = 0; u < 16; u++) {
        if (!(sampled_units & (1u << u)) || !shadow[u].enabled) continue;
        if (!fp_shadow_ext()) {
            if (shadow[u].enabled != 1 || shadow[u].func > 7 ||
                shadow[u].linear > 1 || shadow[u].unnormalized > 1 ||
                (tex_sep_mask & (1u << u))) {
                static u32 n_layout_errors;
                if (PS3_LOG_FIRST(n_layout_errors, 32))
                    fprintf(stderr, "[RSX fp] unsupported depth comparison layout on texture unit %u\n", u);
                return -1;
            }
            shc[u] = 1;
            continue;
        }
        if (shadow[u].func > 7 || ((tex_1d_mask >> u) & 1u) ||
            ((tex_vol_mask & ~tex_cube_mask) >> u) & 1u) continue;
        shc[u] = ((tex_cube_mask >> u) & 1u) ? 2 : 1;
    }
    u32 buffered_constant_count = 0;
    if (buffered) {
        rsx_fp_constant_block constants;
        const int result =
            rsx_fp_collect_constants(ucode, max_bytes, &constants);
        if (result < 0)
            return -1;
        buffered_constant_count = constants.count;
    }
    if (out_constant_count)
        *out_constant_count = buffered_constant_count;

    /* The BODY is emitted first and the preamble is inserted in front of it
     * afterwards. main's signature names the return type, and the return type
     * is a struct as soon as the program turns out to write a second colour
     * export -- which is only known once every instruction has been decoded.
     * Emitting the preamble here would mean guessing it. */
    Out o = { out, out_size, 0, 1 };
    out[0] = '\0';
    int uses_pack = 0;   /* a pack or unpack opcode was decoded: the preamble gets its helpers */
    int used_akill = 0, used_sext = 0, used_gamma = 0;
    /* RET (RPCS3 FragmentProgramDecompiler.cpp:1480-1481, AddFlowOp("return")):
     * RPCS3 decodes the program into fs_main() over global registers and
     * exports from main after it returns (VKFragmentProgram.cpp:455-606). A
     * program with a RET gets the same shape (fp_split_fs_main); one without
     * keeps its single-function text. */
    int has_ret = 0;
    u32 fp_nr, fp_nh, decl_end;
    char decl_init[256];
    const int ucode_bad = !fp_ucode_valid(ucode, max_bytes);
    {
        /* 48 keeps every program that stays inside it byte-identical. */
        u32 ext[2];
        fp_temp_reg_extent(ucode, max_bytes, ext);
        const u32 nr = ext[0] >= 48 ? 64 : 48, nh = ext[1] >= 48 ? 64 : 48;
        fp_nr = nr; fp_nh = nh;
        char decl[256];
        /* Fully initialise both register files: RSX programs routinely read a
         * register lane before writing it (the hardware reads undefined), but
         * HLSL rejects use-before-init (error X4000). Zero everything. */
        if (nr == nh)
            snprintf(decl_init, sizeof decl_init,
                "    [unroll] for (int _i = 0; _i < %u; _i++) { r[_i] = (float4)0; h[_i] = (float4)0; }\n",
                nr);
        else
            snprintf(decl_init, sizeof decl_init,
                "    [unroll] for (int _i = 0; _i < %u; _i++) r[_i] = (float4)0;\n"
                "    [unroll] for (int _i = 0; _i < %u; _i++) h[_i] = (float4)0;\n",
                nr, nh);
        snprintf(decl, sizeof decl, "    float4 r[%u]; float4 h[%u];\n%s", nr, nh, decl_init);
        out_puts(&o, decl);
        if (ucode_bad)
            out_puts(&o, "    discard;\n");
    }
    out_puts(&o,
        /* Condition-code registers for exec_if predication / set_cond. Reset
         * to 0 (NV40 CC power-up state). Dead in non-predicated programs. */
        "    float4 cc0 = (float4)0; float4 cc1 = (float4)0;\n");
    /* RSX_FP_DEBUG=<structural hash hex>:<n>:<reg[.swz]>: in the program
     * with that structural hash (rsx_fp_structural_hash with the FNV offset
     * basis as seed, the "fp" the engine's pipeline log and shader dump
     * print), copy register <reg> (r3, h1, h1.wwww ...) right after its
     * <n>-th translated instruction and export it as colour target 0 with
     * alpha 1. An intermediate of a long shader is then seen on screen
     * instead of reasoned out from the listing. */
    static long dbg_n = -2; static char dbg_reg[32]; static u64 dbg_hash;
    if (dbg_n == -2) {
        dbg_n = -1;
        const char* e = getenv("RSX_FP_DEBUG");
        if (e) {
            char* d;
            dbg_hash = strtoull(e, &d, 16);
            if (*d == ':') dbg_n = strtol(d + 1, &d, 10);
            if (*d == ':') snprintf(dbg_reg, sizeof dbg_reg, "%s", d + 1);
            if (!(dbg_reg[0] == 'r' || dbg_reg[0] == 'h')) dbg_n = -1;
        }
    }
    const int dbg_here = dbg_n >= 0 &&
        rsx_fp_structural_hash(ucode, max_bytes, 1469598103934665603ull) == dbg_hash;
    u32 dbg_count = 0;
    int dbg_done = 0;
    if (dbg_here) out_puts(&o, "    float4 _rsx_dbg = (float4)0;\n");
    decl_end = o.len;

    /* Which colour export each register file received a write to, indexed
     * [half][target]. The whole file is tracked, not just target 0, because
     * the highest one written decides how many SV_TARGETs come out. */
    int wrote[2][FP_MAX_COLOR_TARGETS];
    memset(wrote, 0, sizeof wrote);
    int count = 0;
    u32 off = 0;
    u32 constant_slot = 0;

    /* Open IFE/LOOP/REP blocks: the byte offset each one's else branch starts
     * at (0 = none) and where it ends. See the branch case below. */
    enum { FP_MAX_BLOCKS = 32 };
    u32 blk_else[FP_MAX_BLOCKS], blk_end[FP_MAX_BLOCKS];
    int blk_loop[FP_MAX_BLOCKS];
    int n_blk = 0, loops_open = 0;

    /* h/r alias lane state (fp_alias_barriers). An IFE's paths each start
     * from the state before it; lanes on which they disagree go pending on a
     * join record (FpJoin) and a later read places the barriers at the path
     * ends, RPCS3 RegisterDependencyPass.cpp:373-444. RSX_FP_JOIN_BARRIER=0
     * merges them to unknown (no barrier) instead. */
    enum { FP_ALIAS_DEPTH = 8 };
    static int join_barrier = -1;
    if (join_barrier < 0) {
        const char* e = getenv("RSX_FP_JOIN_BARRIER");
        join_barrier = !(e && *e == '0');
    }
    u16 alias_rf[384], alias_pre[FP_ALIAS_DEPTH][384], alias_if[FP_ALIAS_DEPTH][384];
    int alias_else[FP_ALIAS_DEPTH];
    u32 alias_id[FP_ALIAS_DEPTH], alias_next = 0;
    memset(alias_rf, 0, sizeof alias_rf);

    while (off + 16 <= max_bytes) {
        /* Close blocks ending here, innermost first, then open an else. Ends
         * before elses, as RPCS3's FragmentProgramDecompiler::Decompile does. */
        while (n_blk > 0 && blk_end[n_blk - 1] == off) {
            const int ab = !blk_loop[n_blk - 1] && n_blk - 1 < FP_ALIAS_DEPTH;
            if (ab) {
                char mk[32];
                snprintf(mk, sizeof mk, "    /*@B%uE*/\n", alias_id[n_blk - 1]);
                out_puts(&o, mk);
            }
            out_puts(&o, "    }\n");
            --n_blk;
            if (ab) {
                const u16* other = alias_else[n_blk] ? alias_if[n_blk] : alias_pre[n_blk];
                int differ = 0;
                for (u32 u = 0; u < 384 && !differ; u++) differ = alias_rf[u] != other[u];
                if (differ && join_barrier && J->n < FP_JOIN_MAX) {
                    if (!J->j) J->j = (FpJoin*)malloc(sizeof(FpJoin) * FP_JOIN_MAX);
                    if (J->j) {
                        FpJoin* jn = &J->j[J->n];
                        /* path 0: the if path; path 1: the else path, or the
                         * state before the IFE (marker before the `if`). */
                        memcpy(jn->st[0], alias_else[n_blk] ? alias_if[n_blk] : alias_rf, sizeof alias_rf);
                        memcpy(jn->st[1], alias_else[n_blk] ? alias_rf : alias_pre[n_blk], sizeof alias_rf);
                        snprintf(jn->mk[0], sizeof jn->mk[0], "    /*@B%u%c*/\n",
                                 alias_id[n_blk], alias_else[n_blk] ? 'I' : 'E');
                        snprintf(jn->mk[1], sizeof jn->mk[1], "    /*@B%u%c*/\n",
                                 alias_id[n_blk], alias_else[n_blk] ? 'E' : 'P');
                        for (u32 u = 0; u < 384; u++)
                            if (jn->st[0][u] != jn->st[1][u]) alias_rf[u] = (u16)(4u + J->n);
                        J->n++;
                        differ = 0;
                    }
                }
                if (differ)
                    for (u32 u = 0; u < 384; u++)
                        if (alias_rf[u] != other[u]) alias_rf[u] = 0;
            }
            loops_open -= blk_loop[n_blk];
        }
        if (n_blk > 0 && blk_else[n_blk - 1] == off) {
            if (n_blk - 1 < FP_ALIAS_DEPTH) {
                char mk[32];
                snprintf(mk, sizeof mk, "    /*@B%uI*/\n", alias_id[n_blk - 1]);
                out_puts(&o, mk);
            }
            out_puts(&o, "    } else {\n");
            blk_else[n_blk - 1] = 0;
            if (n_blk - 1 < FP_ALIAS_DEPTH) {
                memcpy(alias_if[n_blk - 1], alias_rf, sizeof alias_rf);
                memcpy(alias_rf, alias_pre[n_blk - 1], sizeof alias_rf);
                alias_else[n_blk - 1] = 1;
            }
        }
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        count++;

        u32 opcode    = (w0 & FP_OPCODE_MASK) >> FP_OPCODE_SHIFT;
        u32 input_src = (w0 & FP_INPUT_SRC_MASK) >> FP_INPUT_SRC_SHIFT;
        u32 tex_unit  = (w0 & FP_TEX_UNIT_MASK) >> FP_TEX_UNIT_SHIFT;
        if (!(w2 & FP_BRANCH) && shadow && shadow[tex_unit].enabled && !fp_shadow_ext() &&
            (opcode == OP_TEXBEM || opcode == OP_TXPBEM)) {
            static u32 n_fetch_errors;
            if (PS3_LOG_FIRST(n_fetch_errors, 32))
                fprintf(stderr, "[RSX fp] unsupported depth comparison fetch on texture unit %u\n", tex_unit);
            return -1;
        }
        int is_branch = (w2 & FP_BRANCH) ? 1 : 0;
        /* RPCS3 GetSRC: SRC2.use_index_reg outside a loop reads input
         * addr_reg + 4 (a texture coordinate) instead of OPDEST's input. */
        if (!is_branch && ((w3 >> 30) & 1u) && !loops_open) {
            const u32 ir = ((w3 >> 19) & 0x7FFu) + 4u;
            if (ir <= 0xEu) input_src = ir;
        }

        /* Execution condition (exec_if) + condition-code write (set_cond).
         * Faithful to RPCS3 FragmentProgramDecompiler {GetRawCond, AddCodeCond,
         * SetDst}: all-set == unconditional, none-set == never, otherwise a
         * per-component select of the result vs the destination, gated by a
         * sign test of the cond-swizzled CC register. */
        int exec_lt   = (w1 & FP_EXEC_IF_LT) ? 1 : 0;
        int exec_eq   = (w1 & FP_EXEC_IF_EQ) ? 1 : 0;
        int exec_gr   = (w1 & FP_EXEC_IF_GR) ? 1 : 0;
        int is_uncond = (exec_lt && exec_eq && exec_gr);
        int is_never  = (!exec_lt && !exec_eq && !exec_gr);
        int set_cond  = (w0 & FP_COND_WRITE) ? 1 : 0;
        u32 cc_read   = (w1 & FP_COND_REG)     ? 1u : 0u;
        u32 cc_write  = (w1 & FP_COND_MOD_REG) ? 1u : 0u;
        static const char comp_c[4] = {'x','y','z','w'};
        char cswz[5];
        for (int ci = 0; ci < 4; ci++)
            cswz[ci] = comp_c[(w1 >> (FP_COND_SWZ_SHIFT + 2 * ci)) & 3];
        cswz[4] = '\0';
        /* HLSL comparison operator for the exec_if sign mask (matches the
         * oracle's SGE/SLE/SNE/SGT/SLT/SEQ selection in GetRawCond). */
        const char* cmp_op = "==";
        if      (exec_gr && exec_eq) cmp_op = ">=";
        else if (exec_lt && exec_eq) cmp_op = "<=";
        else if (exec_gr && exec_lt) cmp_op = "!=";
        else if (exec_gr)            cmp_op = ">";
        else if (exec_lt)            cmp_op = "<";
        else                         cmp_op = "==";  /* exec_eq only */

        Src s0, s1, s2;
        decode_src(w1, w1 & FP_SRC0_ABS, &s0);
        decode_src(w2, w2 & FP_SRC1_ABS, &s1);
        decode_src(w3, w3 & FP_SRC1_ABS, &s2);   /* SRC2.abs: bit 18, as SRC1 */
        if (!is_branch) {
            s0.prec = (int)((w2 >> 19) & 7u);
            s1.prec = (int)((w2 >> 22) & 7u);
            s2.prec = (int)((w2 >> 25) & 7u);
        }

        /* Inline constant: any CONST source pulls the next 16 bytes as a
         * float4 literal and advances past it. */
        float k[4] = {0,0,0,0};
        int has_k = 0;
        if (s0.type == FP_REG_TYPE_CONST || s1.type == FP_REG_TYPE_CONST ||
            s2.type == FP_REG_TYPE_CONST) {
            if (off + 16 <= max_bytes) {
                for (int i = 0; i < 4; i++) {
                    u32 cw = rsx_fp_sanitize_constant_word(
                        rsx_fp_read_word(ucode + off + i * 4));
                    memcpy(&k[i], &cw, 4);
                }
                off += 16;
            }
            has_k = 1;
        }

        /* SRC2 bit 31 is perspective_corr for the instruction's input read
         * (RPCS3 RSXFragmentProgram.h SRC2); on a branch the word is an
         * address instead. */
        const int persp = !is_branch && (w3 >> 31) != 0;
        /* SRC2.use_index_reg (bit 30) inside a loop: RPCS3 reads the input
         * dynamically, _indexed_load(i<innermost> + addr_reg [19:29]). */
        char dyn[48];
        const char* dyn_input = NULL;
        if (!is_branch && ((w3 >> 30) & 1u) && loops_open) {
            snprintf(dyn, sizeof dyn, "_rsx_iload(input, _l%d + %u)",
                     loops_open - 1, (w3 >> 19) & 0x7FFu);
            dyn_input = dyn;
        }
        char a[320], b[320], c[320];
        emit_src(
            &s0, input_src, persp, k, has_k, buffered, constant_slot, dyn_input,
            a, sizeof(a));
        emit_src(
            &s1, input_src, persp, k, has_k, buffered, constant_slot, dyn_input,
            b, sizeof(b));
        emit_src(
            &s2, input_src, persp, k, has_k, buffered, constant_slot, dyn_input,
            c, sizeof(c));
        if (has_k)
            constant_slot++;

        /* Flow control: SRC1.branch set, and OPDEST's opcode field selects
         * BRK/CAL/IFE/LOOP/REP/RET (RPCS3 RSXFragmentProgram.h, 0x40..0x45).
         * IFE carries the else and end addresses in SRC1/SRC2 [0:30] as word
         * offsets; LOOP/REP carry end/init/increment counters in SRC1 [2:9],
         * [10:17], [19:26] and the end address in SRC2. The condition is the
         * exec_if test on the first component of the condition swizzle.
         * Skipping these ran both sides of every if/else: Crazy Taxi's uber
         * material shader then overwrote its lit colour with a COLOR0 the
         * vertex program never writes, and the whole city drew black. */
        if (is_branch) {
            char cond[64], line[160];
            if (is_uncond)
                snprintf(cond, sizeof cond, "true");
            else if (is_never)
                snprintf(cond, sizeof cond, "false");
            else
                /* RPCS3 GetCond: any() over the full condition swizzle. */
                snprintf(cond, sizeof cond, "any(cc%u.%s %s 0.0)", cc_read, cswz, cmp_op);
            const u32 end_at = (w3 & 0x7FFFFFFFu) << 2;
            switch (opcode) {
            case 0x02: /* IFE */
            case 0x03: /* LOOP */
            case 0x04: /* REP */
                if (n_blk == FP_MAX_BLOCKS || end_at <= off) {
                    out_puts(&o, "    /* TODO: malformed flow-control op skipped */\n");
                    break;
                }
                if (opcode == 0x02) {
                    const u32 else_at = (w2 & 0x7FFFFFFFu) << 2;
                    blk_else[n_blk] = else_at != end_at ? else_at : 0;
                    blk_loop[n_blk] = 0;
                    snprintf(line, sizeof line, "    if (%s) {\n", cond);
                } else {
                    /* RPCS3: for (int iN = 0; iN < rep_count; iN++), N the
                     * loop depth; init_counter and increment are ignored. */
                    const u32 end_n = (w2 >> 2) & 0xFFu;
                    blk_else[n_blk] = 0;
                    blk_loop[n_blk] = 1;
                    snprintf(line, sizeof line,
                             "    if (%s) [loop] for (int _l%d = 0; _l%d < %u; _l%d++) {\n",
                             cond, loops_open, loops_open, end_n, loops_open);
                    loops_open++;
                }
                blk_end[n_blk++] = end_at;
                if (n_blk - 1 < FP_ALIAS_DEPTH) {
                    memcpy(alias_pre[n_blk - 1], alias_rf, sizeof alias_rf);
                    alias_else[n_blk - 1] = 0;
                    alias_id[n_blk - 1] = alias_next++;
                    if (opcode == 0x02) {
                        char mk[32];
                        snprintf(mk, sizeof mk, "    /*@B%uP*/\n", alias_id[n_blk - 1]);
                        out_puts(&o, mk);
                    }
                }
                out_puts(&o, line);
                break;
            case 0x00: /* BRK */
                snprintf(line, sizeof line, "    if (%s) break;\n", cond);
                if (loops_open) out_puts(&o, line);
                break;
            case 0x05: /* RET: RPCS3 AddFlowOp("return"), never = a comment */
                if (!is_never) {
                    snprintf(line, sizeof line, "    if (%s) return;\n", cond);
                    out_puts(&o, line);
                    has_ret = 1;
                }
                break;
            default:   /* CAL: unimplemented in RPCS3 too (FragmentProgramDecompiler.cpp:1456-1457) */
                out_puts(&o, "    /* TODO: CAL flow-control op skipped */\n");
                break;
            }
            if (w0 & FP_END) break;
            continue;
        }

        if (gamma_masks && gamma_masks[tex_unit] && !(w0 & (1u << 21)) &&
            (opcode == OP_TEXBEM || opcode == OP_TXPBEM)) {
            static u32 reported;
            if (PS3_LOG_FIRST(reported, 32))
                fprintf(stderr, "[RSX fp] gamma fetch opcode %u unsupported on unit %u\n", opcode, tex_unit);
            return -1;
        }

        if (dbg_here && !dbg_done && dbg_count == (u32)dbg_n) {
            /* "h1" -> (h[1]), "h1.wwww" -> (h[1]).wwww */
            const char* dot = strchr(dbg_reg, '.');
            const unsigned idx = (unsigned)strtoul(dbg_reg + 1, NULL, 10);
            char line[96];
            snprintf(line, sizeof line, "    _rsx_dbg = (float4)((%c[%u])%s);\n",
                     dbg_reg[0], idx < 48u ? idx : 0u, dot ? dot : "");
            out_puts(&o, line);
            dbg_done = 1;
        }
        /* Build the RHS expression for this opcode. */
        if (opcode != OP_NOP) {
            const u32 wv[4] = { w0, w1, w2, w3 };
            if (!fp_is_delay_slot(wv))
                fp_alias_barriers(&o, alias_rf, J, wv, (int)((tex_sep_mask >> tex_unit) & 1u));
        }
        char rhs[1600];
        int handled = 1;
        /* RPCS3 TEX3D_SHADOW_PROJ is "impossible": a cube TXP samples. */
        const int sh_here = shc[tex_unit] && !(shc[tex_unit] == 2 && opcode == OP_TXP);
        const char* sh_sw = shc[tex_unit] == 2 ? "" : ".xyz";
        switch (opcode) {
        case OP_NOP: rhs[0] = '\0'; handled = 0; break;
        case OP_MOV: snprintf(rhs, sizeof(rhs), "%s", a); break;
        case OP_MUL: snprintf(rhs, sizeof(rhs), "(%s) * (%s)", a, b); break;
        case OP_ADD: snprintf(rhs, sizeof(rhs), "(%s) + (%s)", a, b); break;
        case OP_MAD: snprintf(rhs, sizeof(rhs), "mad((%s), (%s), (%s))", a, b, c); break;   /* RPCS3 fma */
        case OP_DP3: snprintf(rhs, sizeof(rhs), "dot((%s).xyz, (%s).xyz)", a, b); break;
        case OP_DP4: snprintf(rhs, sizeof(rhs), "dot((%s), (%s))", a, b); break;
        case OP_MIN: snprintf(rhs, sizeof(rhs), "min((%s), (%s))", a, b); break;
        case OP_MAX: snprintf(rhs, sizeof(rhs), "max((%s), (%s))", a, b); break;
        case OP_FRC: snprintf(rhs, sizeof(rhs), "frac(%s)", a); break;
        case OP_FLR: snprintf(rhs, sizeof(rhs), flr_bias ? "floor((%s) + 0.000001)" : "floor(%s)", a); break;
        case OP_RCP: snprintf(rhs, sizeof(rhs), "(1.0 / (%s).x)", a); break;
        /* Pack/unpack (RPCS3 FragmentProgramDecompiler PK/UP cases and the pkg/upg
         * builtins): the packed word is a scalar that replicates. */
        case OP_PK4UB: uses_pack = 1; snprintf(rhs, sizeof(rhs), "asfloat(_rsx_pk4ub(%s))", a); break;
        case OP_UP4UB: uses_pack = 1; snprintf(rhs, sizeof(rhs), "_rsx_up4ub((%s).x)", a); break;
        case OP_PKG:   uses_pack = 1; snprintf(rhs, sizeof(rhs), "asfloat(_rsx_pk4ub(_rsx_lin2srgb(%s)))", a); break;
        case OP_UPG:   uses_pack = 1; snprintf(rhs, sizeof(rhs), "_rsx_srgb2lin(_rsx_up4ub((%s).x))", a); break;
        case OP_PK4B:  uses_pack = 1; snprintf(rhs, sizeof(rhs), "asfloat(_rsx_pk4b(%s))", a); break;
        case OP_UP4B:  uses_pack = 1; snprintf(rhs, sizeof(rhs), "_rsx_up4b((%s).x)", a); break;
        case OP_PK2US: uses_pack = 1; snprintf(rhs, sizeof(rhs), "asfloat(_rsx_pk2us(%s))", a); break;
        case OP_UP2US: uses_pack = 1; snprintf(rhs, sizeof(rhs), "_rsx_up2us((%s).x)", a); break;
        case OP_PK2H:  uses_pack = 1; snprintf(rhs, sizeof(rhs), "asfloat(_rsx_pk2h(%s))", a); break;
        case OP_UP2H:  uses_pack = 1; snprintf(rhs, sizeof(rhs), "_rsx_up2h((%s).x)", a); break;
        /* Real RSX RSQ ignores the input's sign — 1/sqrt(|x|), not 1/sqrt(x)
         * (oracle: RPCS3 FragmentProgramDecompiler.cpp, "RSQ ignores the sign
         * of the inputs (Metro Last Light, GTA4)"). The strict form NaN'd the
         * normal-map Z-reconstruction (1-x²-y² measurably negative for most
         * pixels, median -0.0078) and poisoned the whole lighting chain — the
         * s25-s27 black/blue character class (scratch/s26_fp_bisect.md s27
         * part 4; two independent shader pairs confirmed). */
        case OP_RSQ: snprintf(rhs, sizeof(rhs), "rsqrt(abs((%s).x))", a); break;
        case OP_EX2: snprintf(rhs, sizeof(rhs), "exp2((%s).x)", a); break;
        case OP_LG2: snprintf(rhs, sizeof(rhs), "log2((%s).x)", a); break;
        case OP_COS: snprintf(rhs, sizeof(rhs), "cos((%s).x)", a); break;
        case OP_SIN: snprintf(rhs, sizeof(rhs), "sin((%s).x)", a); break;
        case OP_POW: snprintf(rhs, sizeof(rhs), "pow((%s).x, (%s).x)", a, b); break;
        case OP_DIV: snprintf(rhs, sizeof(rhs), "(%s) / (%s).x", a, b); break;
        /* Real RSX DIVSQ shares RSQ's sign-ignoring sqrt on the denominator
         * (oracle: RPCS3 FragmentProgramDecompiler.cpp's _builtin_sqrt macro,
         * reused by _builtin_divsq), and where the NUMERATOR component is
         * not > 0 in magnitude the result is the numerator itself (0, -0 or
         * NaN), whatever the denominator ("DIVSQ is not compliant. Result is
         * 0 if numerator is 0 regardless of denominator"). A true
         * per-component select (HLSL vector `?:`, not lerp/mix) keeps the
         * 0/0 branch's NaN out, as RPCS3's _select does. */
        case OP_DIVSQ:
            snprintf(rhs, sizeof(rhs),
                     "(abs(%s) > 0.0 ? (%s) / sqrt(abs((%s).x)) : (%s))",
                     a, a, b, a);
            break;
        case OP_DP2: snprintf(rhs, sizeof(rhs), "dot((%s).xy, (%s).xy)", a, b); break;
        /* RPCS3: _builtin_normalize($0.xyz).xyzz -- w is the normalised z. */
        case OP_NRM: snprintf(rhs, sizeof(rhs), "_rsx_nrm((%s).xyz).xyzz", a); break;
        /* RPCS3 handle_sct_scb / handle_tex_srb / GLSLCommon getFunctionImpl. */
        case OP_DDX: snprintf(rhs, sizeof(rhs), "ddx(%s)", a); break;
        case OP_DDY: snprintf(rhs, sizeof(rhs), "ddy(%s)", a); break;
        case OP_DST: snprintf(rhs, sizeof(rhs), "float4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", a, b, a, b); break;
        case OP_DP2A: snprintf(rhs, sizeof(rhs), "(dot((%s).xy, (%s).xy) + (%s).x)", a, b, c); break;
        case OP_RFL: snprintf(rhs, sizeof(rhs), "reflect((%s), (%s))", a, b); break;
        case OP_STR: snprintf(rhs, sizeof(rhs), "(float4)1.0"); break;
        case OP_SFL: snprintf(rhs, sizeof(rhs), "(float4)0.0"); break;
        /* _builtin_lit: t = (max(x,0), max(y,1e-10), z, w);
         * (1, t.x, t.x > 0 ? exp2(t.w * log2(t.y)) : 0, 1). */
        case OP_LIT:
            snprintf(rhs, sizeof(rhs),
                     "float4(1.0, max((%s).x, 0.0), max((%s).x, 0.0) > 0.0 ? "
                     "exp2((%s).w * log2(max((%s).y, 1e-10))) : 0.0, 1.0)", a, a, a, a);
            break;
        case OP_LRP: snprintf(rhs, sizeof(rhs), "lerp((%s), (%s), (%s))", c, b, a); break;
        /* Lighting coefficients: (1, y, y > 0 ? 2^w : 0, 1) (RPCS3
         * FragmentProgramDecompiler, RSX_FP_OPCODE_LIF). */
        case OP_LIF:
            snprintf(rhs, sizeof(rhs),
                     "float4(1.0, (%s).y, (%s).y > 0.0 ? exp2((%s).w) : 0.0, 1.0)", a, a, a);
            break;
        /* Texture/branch fences: ordering hints with no result -- no-op. */
        case OP_FENCT: case OP_FENCB: rhs[0] = '\0'; handled = 0; break;
        case OP_SLT: snprintf(rhs, sizeof(rhs), lpt ? "(float4)(_rsx_cmpfix(%s) <  _rsx_cmpfix(%s))" : "(float4)((%s) <  (%s))", a, b); break;
        case OP_SGE: snprintf(rhs, sizeof(rhs), lpt ? "(float4)(_rsx_cmpfix(%s) >= _rsx_cmpfix(%s))" : "(float4)((%s) >= (%s))", a, b); break;
        case OP_SLE: snprintf(rhs, sizeof(rhs), lpt ? "(float4)(_rsx_cmpfix(%s) <= _rsx_cmpfix(%s))" : "(float4)((%s) <= (%s))", a, b); break;
        case OP_SGT: snprintf(rhs, sizeof(rhs), lpt ? "(float4)(_rsx_cmpfix(%s) >  _rsx_cmpfix(%s))" : "(float4)((%s) >  (%s))", a, b); break;
        case OP_SNE: snprintf(rhs, sizeof(rhs), lpt ? "(float4)(_rsx_cmpfix(%s) != _rsx_cmpfix(%s))" : "(float4)((%s) != (%s))", a, b); break;
        case OP_SEQ: snprintf(rhs, sizeof(rhs), lpt ? "(float4)(_rsx_cmpfix(%s) == _rsx_cmpfix(%s))" : "(float4)((%s) == (%s))", a, b); break;
        case OP_TEX:
            if (sh_here) {
                snprintf(rhs, sizeof(rhs), "_rsx_shadow%u((%s)%s)", tex_unit, a, sh_sw);
                break;
            }
            if ((tex_1d_mask >> tex_unit) & 1u)
                snprintf(rhs, sizeof(rhs), "%s%u%s.Sample(rsx_samp[%u], float2((%s).x, 0.5))",
                         tex_sep_mask ? "rsx_tex" : "rsx_tex[", tex_unit, tex_sep_mask ? "" : "]",
                         tex_unit, a);
            else if ((tex_sep_mask >> tex_unit) & 1u)
                /* Cubemap or volume: sample with all three coordinates. */
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xyz)", tex_unit, tex_unit, a);
            else if (tex_sep_mask)
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xy)", tex_unit, tex_unit, a);
            else
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex[%u].Sample(rsx_samp[%u], (%s).xy)", tex_unit, tex_unit, a);
            break;
        case OP_TXP:
            if (sh_here) {
                snprintf(rhs, sizeof(rhs), "_rsx_shadow%u((%s).xyz / (%s).w)", tex_unit, a, a);
                break;
            }
            if ((tex_1d_mask >> tex_unit) & 1u)   /* RPCS3 TEX1D_PROJ: x / w */
                snprintf(rhs, sizeof(rhs),
                         "%s%u%s.Sample(rsx_samp[%u], float2((%s).x / (%s).w, 0.5))",
                         tex_sep_mask ? "rsx_tex" : "rsx_tex[", tex_unit, tex_sep_mask ? "" : "]",
                         tex_unit, a, a);
            else if ((tex_sep_mask >> tex_unit) & 1u)
                /* Cube or volume: RPCS3 TEX3D_PROJ samples xyz / w for both
                 * (a negative w flips a cube direction). */
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xyz / (%s).w)",
                         tex_unit, tex_unit, a, a);
            else if (tex_sep_mask)
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xy / (%s).w)",
                         tex_unit, tex_unit, a, a);
            else
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex[%u].Sample(rsx_samp[%u], (%s).xy / (%s).w)",
                         tex_unit, tex_unit, a, a);
            break;
        case OP_TXB: case OP_TXL: {
            /* Biased / explicit-LOD sample; bias or LOD is src1.x (RPCS3
             * TEXTURE_SAMPLE2D_BIAS / _LOD). GH3's fret buttons and fret lines
             * are TXB: unhandled, they sampled nothing and output alpha 0.
             * On a comparison unit RPCS3 maps every non-projective fetch to
             * TEX2D_SHADOW($0.xyz), dropping the bias / LOD. */
            if (sh_here) {
                snprintf(rhs, sizeof(rhs), "_rsx_shadow%u((%s)%s)", tex_unit, a, sh_sw);
                break;
            }
            const int cube = (tex_sep_mask >> tex_unit) & 1u;   /* cube or volume: xyz */
            char tn[32];
            snprintf(tn, sizeof(tn), tex_sep_mask ? "rsx_tex%u" : "rsx_tex[%u]", tex_unit);
            if ((tex_1d_mask >> tex_unit) & 1u)
                snprintf(rhs, sizeof(rhs), "%s.%s(rsx_samp[%u], float2((%s).x, 0.5), (%s).x)", tn,
                         opcode == OP_TXB ? "SampleBias" : "SampleLevel", tex_unit, a, b);
            else
            snprintf(rhs, sizeof(rhs), "%s.%s(rsx_samp[%u], (%s).%s, (%s).x)", tn,
                     opcode == OP_TXB ? "SampleBias" : "SampleLevel", tex_unit, a,
                     cube ? "xyz" : "xy", b);
            break;
        }
        case OP_TXD: {
            /* RPCS3 TEX2D_GRAD($0.xy, $1.xy, $2.xy) / TEX3D_GRAD(xyz). */
            if (sh_here) {
                snprintf(rhs, sizeof(rhs), "_rsx_shadow%u((%s)%s)", tex_unit, a, sh_sw);
                break;
            }
            const char* sw = ((tex_sep_mask >> tex_unit) & 1u) ? "xyz" : "xy";
            char tn[32];
            snprintf(tn, sizeof(tn), tex_sep_mask ? "rsx_tex%u" : "rsx_tex[%u]", tex_unit);
            if ((tex_1d_mask >> tex_unit) & 1u)   /* RPCS3 TEX1D_GRAD: x gradients */
                snprintf(rhs, sizeof(rhs), "%s.SampleGrad(rsx_samp[%u], float2((%s).x, 0.5), "
                         "float2((%s).x, 0.0), float2((%s).x, 0.0))", tn, tex_unit, a, b, c);
            else
            snprintf(rhs, sizeof(rhs), "%s.SampleGrad(rsx_samp[%u], (%s).%s, (%s).%s, (%s).%s)",
                     tn, tex_unit, a, sw, b, sw, c, sw);
            break;
        }
        case OP_KIL:
            /* Fragment kill. Predicated by the same exec_if condition as any
             * other instruction (RPCS3 FragmentProgramDecompiler case
             * RSX_FP_OPCODE_KIL: AddFlowOp("discard") under GetCond());
             * unconditional encodings discard outright. Dropping this (the
             * old TODO) rendered every alpha-cutout material as a solid
             * quad. */
            if (is_never) {
                rhs[0] = '\0';
            } else if (is_uncond) {
                out_puts(&o, "    discard;\n");
            } else {
                char kil[128];
                snprintf(kil, sizeof(kil),
                         "    if (any(cc%u.%s %s (float4)0)) discard;\n",
                         cc_read, cswz, cmp_op);
                out_puts(&o, kil);
            }
            handled = 0;   /* no destination write */
            break;
        default: {
            /* The number and the raw words, so an opcode outside the table
             * can be identified from the dumped shader alone. */
            char todo[160];
            snprintf(todo, sizeof todo,
                     "    /* TODO: unhandled FP opcode 0x%02X (%s) words %08X %08X %08X %08X */\n",
                     opcode, rsx_fp_opcode_name(opcode), w0, w1, w2, w3);
            out_puts(&o, todo);
            handled = 0;
            break; }
            break;
        }

        /* RPCS3 _process_texel, ahead of gamma: texture alpha kill
         * (TEXTURE_CONTROL0 bit 2) discards on the raw texel, then the
         * ARGB8_SIGNED lanes are sign-extended. A comparison fetch has
         * neither (alpha kill already turns the comparison off). */
        /* RPCS3 insert_texture_fetch: every non-comparison TEX/TXP/TXB/TXL/TXD
         * goes through _process_texel. EXP_TEX (_bx2) only takes effect on a
         * format with BIASED_RENORMALIZATION (_enable_texture_expand); the
         * engine marks the others in fs->bx2_lanes (0x20) and gives the
         * texture-sourced remap lanes of the rest (0x10 | lanes). */
        const int is_fetch = opcode == OP_TEX || opcode == OP_TXP || opcode == OP_TXB ||
                             opcode == OP_TXL || opcode == OP_TXD;
        const int bx2 = (w0 & (1u << 21)) != 0 &&
                        !(fs && (fs->bx2_lanes[tex_unit] & 0x20u));
        if (fs && rhs[0] && !sh_here && is_fetch) {
            if ((fs->alpha_kill_mask >> tex_unit) & 1u) {
                char sample[sizeof rhs]; memcpy(sample, rhs, sizeof sample);
                const int length = snprintf(rhs, sizeof rhs, "_rsx_akill(%s)", sample);
                if (length < 0 || (size_t)length >= sizeof rhs) return -1;
                used_akill = 1;
            }
            if (fs->sext_masks[tex_unit] & 15u) {
                char sample[sizeof rhs]; memcpy(sample, rhs, sizeof sample);
                const int length = snprintf(rhs, sizeof rhs, "_rsx_sext_input(%s, %uu)",
                                            sample, fs->sext_masks[tex_unit] & 0x1Fu);
                if (length < 0 || (size_t)length >= sizeof rhs) return -1;
                used_sext = 1;
            }
        }
        if (gamma_masks && gamma_masks[tex_unit] && !bx2 &&
            !sh_here && is_fetch) {
            char sample[sizeof rhs]; memcpy(sample, rhs, sizeof sample);
            const u32 gmask = gamma_masks[tex_unit] & 15u &
                ~(fs ? (u32)fs->sext_masks[tex_unit] : 0u);
            const int length = snprintf(rhs, sizeof rhs, "_rsx_gamma_input(%s, %uu)",
                                        sample, gmask);
            if (length < 0 || (size_t)length >= sizeof rhs) return -1;
            used_gamma = 1;
        }
        /* Texture sign expansion (RPCS3 _process_texel). A unit whose
         * SET_TEXTURE_ADDRESS asks for UNSIGNED_REMAP_BIASED returns every
         * fetch re-centred on the lanes the mask names, (round(x*255)-128)/127
         * (16-bit channels: /32767), after gamma and not on gamma lanes. A
         * fetch carrying EXP_TEX returns x*2-1 on the texture-sourced lanes,
         * whatever the unit says, and bypasses gamma. A comparison fetch has
         * nothing to expand. */
        if ((bx2 || (expand_masks && expand_masks[tex_unit])) &&
            !sh_here && is_fetch) {
            u32 mask = bx2 ? ((fs && (fs->bx2_lanes[tex_unit] & 0x10u))
                                  ? (fs->bx2_lanes[tex_unit] & 15u) : 15u) |
                             (expand_masks ? (expand_masks[tex_unit] & 15u) : 0u)
                           : (expand_masks[tex_unit] & 0x1Fu);
            if (!bx2 && gamma_masks) mask &= ~(gamma_masks[tex_unit] & 15u);
            if (fs) mask &= ~(fs->sext_masks[tex_unit] & 15u);
            if (mask & 15u) {
                char sample[sizeof rhs]; memcpy(sample, rhs, sizeof sample);
                const int length = snprintf(rhs, sizeof rhs, "_rsx_expand_input(%s, %uu, %s)",
                                            sample, mask, bx2 ? "true" : "false");
                if (length < 0 || (size_t)length >= sizeof rhs) return -1;
            }
        }

        int has_dest = !(w0 & FP_OUT_NONE);
        if (dbg_here && handled && !dbg_done) dbg_count++;
        if (handled && (has_dest || set_cond)) {
            u32 dst_idx = (w0 & FP_OUT_REG_MASK) >> FP_OUT_REG_SHIFT;
            int dst_half = (w0 & FP_OUT_HALF) ? 1 : 0;
            const char* reg = dst_half ? "h" : "r";
            char m[5];
            dest_mask(w0, m);

            /* Result precision (OPDEST bits 22-23) when not saturating:
             * fx12 clamps to [-2,2], fp16 on a half register to +-65504 (NaN
             * becomes 0). Ops whose result is already bounded or must keep
             * its range are exempt, matching the hardware-tested set. */
            /* RPCS3 SetDst with native half support (Vulkan float16): a
             * half register holds an fp16 value, so every write to one is
             * cast (round to nearest, overflow to Inf, NaN kept) before the
             * saturate / precision clamp, and the fp16 precision clamp is
             * then a no-op. Saturate is precision_clamp(x, 0, 1): NaN -> 0. */
            /* Shader Precision High (rpcs3_config.yml) leaves
             * has_native_half_support off (VKFragmentProgram::Decompile sets it
             * only for Low): H registers are float, and only a fp16-precision
             * write to one goes through clamp16. RSX_FP_NATIVE_HALF=1 or
             * RSX_CFG_LEGACY=1 restores the native-half cast above. */
            static int native_half = -1;
            if (native_half < 0) {
                const char* e = getenv("RSX_FP_NATIVE_HALF");
                const char* l = getenv("RSX_CFG_LEGACY");
                native_half = (e && *e && *e != '0') || (l && *l && *l != '0');
            }
            char sat_buf[96] = "";
            const u32 dprec = (w0 >> 22) & 3u;
            const char* h16 = (dst_half && native_half) ? " _v = _rsx_h16(_v);" : "";
            if (!native_half && dst_half && dprec == 1 && !(w0 & FP_OUT_SAT)) {
                int exempt = opcode == OP_NRM || opcode == OP_MAX || opcode == OP_MIN ||
                             opcode == OP_COS || opcode == OP_SIN || opcode == OP_RFL ||
                             opcode == OP_FRC || opcode == OP_LIT || opcode == OP_LIF ||
                             opcode == OP_LG2;
                if (opcode == OP_MOV && s0.half && s0.type == FP_REG_TYPE_TEMP) exempt = 1;
                if (!exempt) h16 = " _v = _rsx_h16(_v);";
            }
            if (w0 & FP_OUT_SAT) {
                snprintf(sat_buf, sizeof sat_buf, "%s _v = _rsx_pc(_v, 0.0, 1.0);", h16);
            } else if (dprec && !(dprec == 1 && !dst_half)) {
                int exempt = opcode == OP_NRM || opcode == OP_MAX || opcode == OP_MIN ||
                             opcode == OP_COS || opcode == OP_SIN || opcode == OP_RFL ||
                             opcode == OP_FRC || opcode == OP_LIT || opcode == OP_LIF ||
                             opcode == OP_LG2;
                if (opcode == OP_MOV && dst_half && s0.half && s0.type == FP_REG_TYPE_TEMP) exempt = 1;
                if (!exempt && dprec != 1)
                    snprintf(sat_buf, sizeof sat_buf, "%s%s", h16,
                             dprec == 2 ? " _v = _rsx_pc(_v, -2.0, 2.0);" : " _v = _rsx_pc(_v, -1.0, 1.0);");
                else
                    snprintf(sat_buf, sizeof sat_buf, "%s", h16);
            } else {
                snprintf(sat_buf, sizeof sat_buf, "%s", h16);
            }
            const char* sat = sat_buf;
            /* NV40 per-instruction result-scale modifier (SRC1 word bits
             * 28-30): 1/2/3 = *2/*4/*8, 5/6/7 = /2//4//8; applied to the
             * result BEFORE saturate (RPCS3 FragmentProgramDecompiler.cpp
             * SetDst, RSXFragmentProgram.h SRC1.scale:3). Dropped entirely
             * until s32: draw 1625's two-tap box filter (÷2 on the final
             * ADD) summed unaveraged -- the flat 2x composite gain behind
             * the whiteout/overbright class (scratch/s32_gain_hunt.md). */
            u32 res_scale = (w2 >> 28) & 0x7u;
            static const char* scale_txt[8] = { "", " * 2.0", " * 4.0", " * 8.0",
                                                "", " / 2.0", " / 4.0", " / 8.0" };

            /* Broadcast the result to float4 first so the write-mask picks the
             * CORRESPONDING components: `dst.w = rhs` would HLSL-truncate a
             * vector rhs to its .x, but NV40 writes rhs.w to dst.w. Scalar
             * results (DPx/RCP/...) replicate, which is also the hardware
             * behavior. */
            if (is_never) {
                /* RPCS3 SetDst: exec_if none returns before any write, the
                 * condition register included. */
                out_puts(&o, "    /* exec_if=none: instruction suppressed */\n");
            } else if (has_dest && is_uncond && !set_cond) {
                /* Common unconditional path — kept byte-identical so the 39
                 * non-predicated programs in this capture emit unchanged. */
                char line[2000];
                snprintf(line, sizeof(line),
                         "    { float4 _v = (float4)(%s)%s;%s %s[%u].%s = _v.%s; }\n",
                         rhs, scale_txt[res_scale], sat, reg, dst_idx, m, m);
                out_puts(&o, line);
            } else {
                /* Predicated and/or CC-writing instruction. A no-dest one
                 * skips saturate and precision (RPCS3 SetDst: only
                 * `if (!dst.no_dest)`); the scale still applies. */
                char buf[2400];
                snprintf(buf, sizeof(buf),
                         "    { float4 _v = (float4)(%s)%s;%s\n",
                         rhs, scale_txt[res_scale], has_dest ? sat : "");
                out_puts(&o, buf);
                if (has_dest) {
                    if (is_uncond) {
                        snprintf(buf, sizeof(buf),
                                 "      %s[%u].%s = _v.%s;\n", reg, dst_idx, m, m);
                    } else if (is_never) {
                        snprintf(buf, sizeof(buf),
                                 "      /* exec_if=none: write suppressed */\n");
                    } else {
                        /* Per-component select gated by the CC sign test.
                         * A real select, not lerp(dst, _v, mask): lerp is
                         * arithmetic (dst + mask * (_v - dst)), so an Inf or
                         * NaN in the unselected operand poisons the result
                         * (0 * Inf = NaN), and a register holding raw bits
                         * (a packed word) loses its denormal and NaN
                         * patterns on the way through the ALU. */
                        snprintf(buf, sizeof(buf),
                                 "      float4 _p = (float4)((cc%u).%s %s (float4)0.0);\n"
                                 "      %s[%u].%s = (_p.%s != 0.0) ? _v.%s : %s[%u].%s;\n",
                                 cc_read, cswz, cmp_op,
                                 reg, dst_idx, m, m, m, reg, dst_idx, m);
                    }
                    out_puts(&o, buf);
                }
                if (set_cond) {
                    /* CC receives the destination value (post-select), masked.
                     * No-dest: `$ifcond cc$m = code` -- the raw result, under
                     * any() of the execution condition. */
                    if (has_dest)
                        snprintf(buf, sizeof(buf),
                                 "      cc%u.%s = %s[%u].%s;\n", cc_write, m, reg, dst_idx, m);
                    else if (is_uncond)
                        snprintf(buf, sizeof(buf),
                                 "      cc%u.%s = _v.%s;\n", cc_write, m, m);
                    else
                        snprintf(buf, sizeof(buf),
                                 "      if (any((cc%u).%s %s (float4)0.0)) cc%u.%s = _v.%s;\n",
                                 cc_read, cswz, cmp_op, cc_write, m, m);
                    out_puts(&o, buf);
                }
                out_puts(&o, "    }\n");
            }

            if (has_dest) {
                const int target = fp_export_target(dst_idx, dst_half);
                if (target >= 0) wrote[dst_half][target] = 1;
            }
        }

        if (w0 & FP_END) break;
    }
    /* A block whose end address is at or past END never saw it: close it. */
    while (n_blk-- > 0)
        out_puts(&o, "    }\n");
    fp_strip_join_markers(&o);
    if (has_ret) out_puts(&o, FP_FS_MAIN_END);

    /* Fragment color output register selection. The NV40 hardware picks the
     * output from the SHADER_CONTROL word's 32_BITS_EXPORTS bit, not from
     * whichever temp happened to be written: fp32-export programs output r0,
     * fp16-export programs (the default) output h0 (RPCS3
     * FragmentProgramDecompiler::BuildCode). Many real material/lighting
     * shaders accumulate into fp32 temps (r2/r3/r4) but write their FINAL
     * color to h0 — the old "wrote_r0 => return r0" heuristic returned a stale
     * intermediate, producing flat/constant surfaces. */
    int half_exports;
    if (ctrl == RSX_FP_CTRL_AUTO) {
        /* No control word (standalone tests): legacy heuristic. */
        half_exports = (wrote[0][0] || !wrote[1][0]) ? 0 : 1;
    } else {
        half_exports = (ctrl & FP_CTRL_32BIT_EXPORTS) ? 0 : 1;
    }
    const char* export_file = half_exports ? "h" : "r";

    /* How many colour targets the program feeds: one past the highest export
     * register of the selected file it wrote. A program that writes only the
     * first keeps the single-value return it has always had, byte for byte;
     * only a program that reaches the second, third or fourth grows a struct.
     * Writes to the OTHER file's export registers do not count -- r2/r3/r4 are
     * ordinary temps under 16-bit exports, and accumulating into them while
     * the colour goes to h0 is what most material shaders do. */
    u32 targets = 1;
    for (u32 t = FP_MAX_COLOR_TARGETS; t-- > 1; )
        if (wrote[half_exports][t]) { targets = t + 1; break; }
    /* RPCS3 gathers every colour target the surface has from its export
     * register (zero-initialised when the program never writes it). */
    if (fs && fs->mrt_count > targets)
        targets = fs->mrt_count < FP_MAX_COLOR_TARGETS ? fs->mrt_count : FP_MAX_COLOR_TARGETS;

    /* Depth export. SHADER_CONTROL bits 1-3 (0xE,
     * CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT) make the program's depth the
     * fragment depth: always the fp32 register r1.z, whatever the colour
     * export width, unclamped. Without SV_Depth the fragment keeps the
     * interpolated depth of the primitive, so a full-screen pass that rebuilds
     * a depth buffer writes the quad's constant depth instead. The output
     * becomes a struct even for one colour target. */
    const int depth_export = ctrl != RSX_FP_CTRL_AUTO && (ctrl & 0xEu) != 0u;
    const int depth_cmp = fs && fs->depth_compare && fs->depth_compare_unit < 16u;
    if (dbg_here) {
        static u32 reported;
        if (PS3_LOG_FIRST(reported, 8))
            fprintf(stderr, "[RSX fp] RSX_FP_DEBUG %016llx: %s after instruction %ld%s\n",
                    (unsigned long long)dbg_hash, dbg_reg, dbg_n,
                    dbg_done ? "" : " not reached (program is shorter)");
        if (dbg_done)
            out_puts(&o, "    { float4 _d = _rsx_dbg; _d.w = 1.0;"
                         " r[0] = _d; h[0] = _d; }\n");
    }
    if (targets == 1 && !depth_export && !depth_cmp) {
        char tail[96];
        /* Resistance port diagnostic: FP_SWAP_RB=1 swaps red/blue in every
         * single-target fragment program's output. */
        static int swap_rb = -1;
        if (swap_rb < 0) { const char* e = getenv("FP_SWAP_RB"); swap_rb = (e && *e != '0') ? 1 : 0; }
        snprintf(tail, sizeof(tail),
                 "    float4 _o = %s[0]%s;\n"
                 "    return _o;\n}\n", export_file,
                 swap_rb ? ".bgra" : "");
        out_puts(&o, tail);
    } else {
        /* Multiple render targets, written as the program left them (RPCS3
         * VKFragmentProgram: no NaN filtering on colour exports). */
        out_puts(&o, "    PSOutput _out;\n");
        for (u32 t = 0; t < targets; t++) {
            char line[192];
            snprintf(line, sizeof(line),
                     "    float4 _o%u = %s[%u];\n"
                     "    _out.t%u = _o%u;\n",
                     t, export_file, fp_export_reg[half_exports][t],
                     t, t);
            out_puts(&o, line);
        }
        /* RPCS3 VKFragmentProgram.cpp:589-598: depth writes are unclamped
         * (hardware tests disprove the clamp); r1.z is exported raw. */
        if (depth_export)
            out_puts(&o, "    float _d = r[1].z;\n"
                         "    _out.depth = _d;\n");
        else if (depth_cmp) {
            /* RPCS3 RSXROPEpilogue _ENABLE_DEPTH_COMPARE (a program depth
             * export still wins, as in VKFragmentProgram). */
            char dl[320];
            snprintf(dl, sizeof dl,
                     "    float _dd = %s%u%s.Load(int3(input.position.xy, 0)).x;\n"
                     "    float _ds = input.position.z;\n"
                     "    _out.depth = (abs(_ds - _dd) < (1.0 / %s)) ? _dd : _ds;\n",
                     tex_sep_mask ? "rsx_tex" : "rsx_tex[", (u32)fs->depth_compare_unit,
                     tex_sep_mask ? "" : "]",
                     fs->depth_compare_24 ? "16777215.0" : "65535.0");
            out_puts(&o, dl);
        }
        out_puts(&o, "    return _out;\n}\n");
    }

    /* Fixed-function fragment state at the top of main (rsx_fp_frag_state):
     * RPCS3's fragment prologue (fog, two-sided colour, TEX_COORD_CONTROL)
     * and ROP prologue (polygon stipple). Built before the preamble so its
     * declarations can follow what it reads. */
    char prolog_buf[4096];
    Out q = { prolog_buf, sizeof prolog_buf, 0, 1 };
    prolog_buf[0] = '\0';
    if (fs) {
        if (fs->stipple)
            out_puts(&q, rdyn ? "    { uint2 _sp = uint2(input.position.xy * fp_rscale[0].x) & 31u;"
                                " if (((_rsx_stipple[_sp.y] >> _sp.x) & 1u) == 0u) discard; }\n"
                              : "    { uint2 _sp = uint2(input.position.xy) & 31u;"
                                " if (((_rsx_stipple[_sp.y] >> _sp.x) & 1u) == 0u) discard; }\n");
        if (fs->two_sided)
            out_puts(&q, "    if (!input._rsx_front) { input.col0 = input.bc0; input.col1 = input.bc1; }\n");
        if (fs->fog) {
            const char* x = "input.fog.x";
            const char* ax = "abs(input.fog.x)";
            char fl[256];
            switch (fs->fog_mode) {
            case 0x2601: case 0x0804:   /* LINEAR, LINEAR_ABS */
                snprintf(fl, sizeof fl, "fp_alpha.z * %s + (fp_alpha.y - 1.0)",
                         fs->fog_mode == 0x2601 ? x : ax);
                break;
            case 0x0800: case 0x0802:   /* EXP, EXP_ABS */
                snprintf(fl, sizeof fl, "exp(11.084 * (fp_alpha.z * %s + fp_alpha.y - 1.5))",
                         fs->fog_mode == 0x0800 ? x : ax);
                break;
            case 0x0801: case 0x0803:   /* EXP2, EXP2_ABS */
                snprintf(fl, sizeof fl, "exp(-pow(4.709 * (fp_alpha.z * %s + fp_alpha.y - 1.5), 2.0))",
                         fs->fog_mode == 0x0801 ? x : ax);
                break;
            default: fl[0] = '\0'; break;   /* RPCS3: (fog.x, 0, 0, 0), unclamped */
            }
            char line[352];
            if (fl[0])
                snprintf(line, sizeof line, "    input.fog = float4(input.fog.x, saturate(%s), 0.0, 0.0);\n", fl);
            else
                snprintf(line, sizeof line, "    input.fog = float4(input.fog.x, 0.0, 0.0, 0.0);\n");
            out_puts(&q, line);
        }
        /* TEX_COORD_CONTROL 2D: RPCS3 reads (xy, 0, w) and its perspective
         * divide then yields (xy*w, 0, 1); keeping clip w in .w gives both
         * our plain and perspective-corrected reads the same values. */
        for (u32 t = 0; t < 10; t++)
            if ((fs->texcoord_2d_mask >> t) & 1u) {
                char line[128];
                snprintf(line, sizeof line,
                         "    input.tc%u = float4(input.tc%u.xy, 0.0, 1.0 / _rsx_inv_w(input.position));\n",
                         t, t);
                out_puts(&q, line);
            }
        /* Point sprites (RPCS3 FragmentProgramDecompiler, TEX0-TEX9 with
         * texcoord_is_point_coord): the unit reads the sprite coordinate,
         * which also wins over a perspective_corr read; with the 2D mask it
         * reads (PointCoord, 0, w). Only where the target has PointCoord
         * (the glslang paths); elsewhere the interpolated TC stays. */
        if (fs->point_sprite_mask & 0x3FFu) {
            out_puts(&q, "#ifdef RSX_FRAGCOORD_W_IS_RCP\n");
            for (u32 t = 0; t < 10; t++) {
                if (!((fs->point_sprite_mask >> t) & 1u)) continue;
                char pat[64], rep[16], line[192];
                snprintf(pat, sizeof pat, "(input.tc%u * _rsx_inv_w(input.position))", t);
                snprintf(rep, sizeof rep, "input.tc%u", t);
                out_replace_shorter(&o, pat, rep);
                /* RPCS3: (PointCoord, 1, 0), or (PointCoord, 0, in_w) under
                 * the 2D mask; POINT_SPRITE_CONTROL RMODE is not modelled. */
                if ((fs->texcoord_2d_mask >> t) & 1u)
                    snprintf(line, sizeof line,
                             "    input.tc%u = float4(input._rsx_pcoord, 0.0, 1.0 / _rsx_inv_w(input.position));\n",
                             t);
                else
                    snprintf(line, sizeof line,
                             "    input.tc%u = float4(input._rsx_pcoord, 1.0, 0.0);\n", t);
                out_puts(&q, line);
            }
            out_puts(&q, "#endif\n");
        }
    }
    if (!q.ok) return -1;

    /* Preamble: PSInput matches the backend's placeholder layout; temp/half
     * register files; texture+sampler banks for TEX. Built here, in front of
     * the body, now that the return type is known. */
    char preamble[32768]; /* bounded room for all 16 comparison helpers */
    Out p = { preamble, sizeof(preamble), 0, 1 };
    preamble[0] = '\0';
    if (fs && fs->flat_shade)
        out_puts(&p,
        "struct PSInput {\n"
        "    float4 position : SV_POSITION; nointerpolation float4 col0 : COLOR0;"
        " nointerpolation float4 col1 : COLOR1;\n"
        "    float4 fog : FOG;\n");
    else
    out_puts(&p,
        "struct PSInput {\n"
        "    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
        "    float4 fog : FOG;\n");
    if (!fs || !fs->centroid_mask) {
        out_puts(&p,
        "    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
        "    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n");
    } else {
        /* RPCS3 (r2 patch): TEX_COORD_CONTROL bit 4 interpolates at the
         * centroid on multisampled polygon draws. */
        for (u32 t = 0; t < 8; t++) {
            char m[48];
            snprintf(m, sizeof m, "%s%sfloat4 tc%u:TEXCOORD%u;%s",
                     (t & 3u) ? "" : "    ",
                     ((fs->centroid_mask >> t) & 1u) ? "centroid " : "", t, t,
                     (t & 3u) == 3u ? "\n" : " ");
            out_puts(&p, m);
        }
    }
    /* TC8/TC9 and the face flag only where the body reads them, so every other
     * program stays byte-identical (and keeps its cached pipeline). TC8 and
     * TC9 follow TC7 so the earlier interpolants keep their locations. */
    if (strstr(o.p, "input.tc8") || strstr(o.p, "input.tc9") ||
        strstr(o.p, "_rsx_iload(") ||
        strstr(prolog_buf, "input.tc8") || strstr(prolog_buf, "input.tc9") ||
        (fs && fs->two_sided)) {
        if (fs && (fs->centroid_mask & 0x300u)) {
            char m[96];
            snprintf(m, sizeof m, "    %sfloat4 tc8:TEXCOORD8; %sfloat4 tc9:TEXCOORD9;\n",
                     (fs->centroid_mask & 0x100u) ? "centroid " : "",
                     (fs->centroid_mask & 0x200u) ? "centroid " : "");
            out_puts(&p, m);
        } else {
            out_puts(&p, "    float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;\n");
        }
    }
    /* Back colours after TC9: glslang numbers the interpolants in declaration
     * order, matching rsx_fp_patch_vs_back_colors' VSOutput. */
    if (fs && fs->two_sided)
        out_puts(&p, fs->flat_shade
                     ? "    nointerpolation float4 bc0:COLOR2; nointerpolation float4 bc1:COLOR3;\n"
                     : "    float4 bc0:COLOR2; float4 bc1:COLOR3;\n");
    if (strstr(o.p, "input._rsx_front") || strstr(prolog_buf, "input._rsx_front"))
        out_puts(&p, "    bool _rsx_front : SV_IsFrontFace;\n");
    /* The sprite coordinate: no HLSL semantic, so a fixed location that the
     * SPIR-V translation turns into BuiltIn PointCoord (rsx_shader_spirv.h). */
    if (strstr(prolog_buf, "input._rsx_pcoord")) {
        char m[160];
        snprintf(m, sizeof m,
                 "#ifdef RSX_FRAGCOORD_W_IS_RCP\n"
                 "    [[vk::location(%u)]] float2 _rsx_pcoord : RSX_PCOORD;\n"
                 "#endif\n", (unsigned)RSX_SPIRV_POINTCOORD_LOCATION);
        out_puts(&p, m);
    }
    out_puts(&p, "};\n");
    /* RPCS3 _indexed_load: an input read with SRC2.use_index_reg inside a
     * loop picks TC(loop counter + addr_reg); out of range reads (0,0,0,1). */
    if (strstr(o.p, "_rsx_iload("))
        out_puts(&p,
            "float4 _rsx_iload(PSInput i, int n) {\n"
            "    switch (n) {\n"
            "    case 0: return i.tc0; case 1: return i.tc1; case 2: return i.tc2;\n"
            "    case 3: return i.tc3; case 4: return i.tc4; case 5: return i.tc5;\n"
            "    case 6: return i.tc6; case 7: return i.tc7; case 8: return i.tc8;\n"
            "    case 9: return i.tc9;\n"
            "    }\n"
            "    return float4(0.0, 0.0, 0.0, 1.0);\n"
            "}\n");
    /* 1/w of the fragment for perspective_corr reads. SV_POSITION.w is the
     * clip w under FXC (D3D12) but already 1/w once glslang lowers it to
     * SPIR-V FragCoord (Vulkan, and Metal's [[position]] is (x, y, z, 1/w));
     * the glslang paths define RSX_FRAGCOORD_W_IS_RCP. Emitted only where the
     * body uses it, so every other program stays byte-identical. */
    if (strstr(o.p, "_rsx_inv_w(") || strstr(prolog_buf, "_rsx_inv_w("))
        out_puts(&p,
            "#ifdef RSX_FRAGCOORD_W_IS_RCP\n"
            "#define _rsx_inv_w(p) ((p).w)\n"
            "#else\n"
            "#define _rsx_inv_w(p) (1.0 / (p).w)\n"
            "#endif\n");
    /* v125: the RSX window position. rsx_shscale.z = 1 when the guest's
     * SHADER_WINDOW origin is the bottom edge, .w = the window height (the
     * D3D12 backend writes both per draw; other backends leave them 0 = no
     * flip). The game's ground fog and soft particles index the depth
     * buffer with (2x, 720 - y): unflipped they read the mirrored row. */
    const int uses_wpos = strstr(o.p, "_rsx_wpos(") != NULL;
    if (uses_wpos) {
        out_puts(&p,
            "#ifdef RSX_FRAGCOORD_W_IS_RCP\n"
            "#define _rsx_wpos_w(p) ((p).w)\n"
            "#else\n"
            "#define _rsx_wpos_w(p) (1.0 / (p).w)\n"
            "#endif\n");
        if (fs && fs->wpos && rdyn) {
            char wl[320];
            snprintf(wl, sizeof wl,
            "#define _rsx_wpos(p) float4((p).x * fp_rscale[0].x + %.9g, (p).y * %sfp_rscale[0].x + fp_alpha.w, (p).z, _rsx_wpos_w(p))\n",
            fs->wpos_center_integer ? -0.5 : 0.0, fs->wpos_origin_bottom ? "-" : "");
            out_puts(&p, wl);
        } else if (rdyn) {
            out_puts(&p,
            "#define _rsx_wpos(p) float4((p).x * fp_rscale[0].x, (rsx_shscale.z > 0.5) ? (rsx_shscale.w - (p).y * fp_rscale[0].x) : (p).y * fp_rscale[0].x, (p).z, _rsx_wpos_w(p))\n");
        } else if (fs && fs->wpos) {
            /* RPCS3 fragment prologue: wpos = FragCoord scaled to the
             * guest's resolution, y mirrored about the SHADER_WINDOW height
             * for a bottom origin, and offset by -0.5 for an integer pixel
             * centre. fp_alpha.w carries the y bias, per draw. */
            const double r = fp_res_inv();
            char wl[320];
            snprintf(wl, sizeof wl,
            "#define _rsx_wpos(p) float4((p).x * %.9g + %.9g, (p).y * %.9g + fp_alpha.w, (p).z, _rsx_wpos_w(p))\n",
            r, fs->wpos_center_integer ? -0.5 : 0.0, fs->wpos_origin_bottom ? -r : r);
            out_puts(&p, wl);
        } else if (fp_res_inv() == 1.0)
            out_puts(&p,
            "#define _rsx_wpos(p) float4((p).x, (rsx_shscale.z > 0.5) ? (rsx_shscale.w - (p).y) : (p).y, (p).z, _rsx_wpos_w(p))\n");
        else {
            char wl[320];
            snprintf(wl, sizeof wl,
            "#define _rsx_wpos(p) float4((p).x * %.9g, (rsx_shscale.z > 0.5) ? (rsx_shscale.w - (p).y * %.9g) : (p).y * %.9g, (p).z, _rsx_wpos_w(p))\n",
            fp_res_inv(), fp_res_inv(), fp_res_inv());
            out_puts(&p, wl);
        }
    }
    /* Texture bank. With no cube units (the default) emit the exact legacy
     * array declaration so 2D-only programs are byte-identical. When any unit
     * is a cubemap, declare each unit individually at its t-register so the
     * cube units can be TextureCube while the rest stay Texture2D. */
    if (tex_sep_mask == 0) {
        out_puts(&p, "Texture2D    rsx_tex[16] : register(t0);\n");
    } else {
        char decl[64];
        for (u32 u = 0; u < 16; u++) {
            snprintf(decl, sizeof(decl), "%s rsx_tex%u : register(t%u);\n",
                     ((tex_cube_mask >> u) & 1u) ? "TextureCube" :
                     ((tex_vol_mask >> u) & 1u)  ? "Texture3D  " : "Texture2D  ", u, u);
            out_puts(&p, decl);
        }
    }
    out_puts(&p,
        "SamplerState rsx_samp[16] : register(s0);\n"
    );
    if (buffered) {
        char constants_decl[192];
        snprintf(
            constants_decl, sizeof(constants_decl),
            "cbuffer PSConstants : register(b1) {\n"
            "    float4 fp_constants[%u];\n"
            "    float4 fp_alpha;\n"
            "%s"
            "};\n",
            buffered_constant_count ? buffered_constant_count : 1u,
            rdyn ? "    float4 fp_rscale[5];\n" : "");
        out_puts(&p, constants_decl);
    }
    int any_sh = 0;
    if (shadow)
        for (u32 u = 0; u < 16; u++)
            if (shc[u]) any_sh = 1;
    if (!any_sh && uses_wpos) {
        /* WPOS without a shadow map still needs rsx_shscale (same b1 slot,
         * same declaration, so the alpha-test patcher extends it as usual) */
        if (!buffered)
            out_puts(&p, "cbuffer RsxShadowScale : register(b1) { float4 _rsx_b1pad[69]; float4 rsx_shscale; };\n");
        else
            out_puts(&p, "static const float4 rsx_shscale = float4(1.0, 1.0, 0.0, 0.0);\n");
    }
    if (shadow) {
        int any = any_sh;
        /* Shadow-map sub-rect scale: the depth buffer behind a shadow map can
         * be larger than the texture the guest declares (the map is the
         * top-left of a bigger surface). xy = texture size / buffer size.
         * Lives in the per-draw b1 slot at float4 index 69, past the
         * texscale/alphatest/fp_k block the D3D12 backend already writes. */
        if (any && !buffered)
            out_puts(&p, "cbuffer RsxShadowScale : register(b1) { float4 _rsx_b1pad[69]; float4 rsx_shscale; };\n");
        else if (any)
            out_puts(&p, "static const float4 rsx_shscale = float4(1.0, 1.0, 0.0, 0.0);\n");
        for (u32 u = 0; u < 16; u++)
            if (shc[u])
                fp_shadow_helper(&p, u, tex_sep_mask, &shadow[u], rdyn, buffered, shc[u] == 2);
    }
    /* RSX precision clamps (NaN reads as 0) and NRM, whose zero vector stays
     * zero on the hardware instead of normalising to NaN. */
    out_puts(&p,
        "float4 _rsx_pc(float4 x, float lo, float hi) { bool4 n = (asuint(x) & 0x7FFFFFFFu) > 0x7F800000u; return clamp(n ? (float4)0 : x, lo, hi); }\n"
        "float4 _rsx_c16(float4 x) { return _rsx_pc(x, -65504.0, 65504.0); }\n"
        "float4 _rsx_h16(float4 x) { return f16tof32(f32tof16(x)); }\n"
        "float3 _rsx_nrm(float3 v) { float l = dot(v, v); return l > 0.0 ? v * rsqrt(l) : v; }\n");
    /* RSXProgramCommon.glsl:9-13 CMP_FIXUP under _GPU_LOW_PRECISION_COMPARE
     * (GLSLCommon.cpp:183-186); rsx_fp_apply_alpha_test* key off it too. */
    if (lpt) out_puts(&p, FP_CMPFIX_DEFINE);
    /* rsx_fp_texture_gamma_units does not see fs: an EXP_TEX fetch the
     * frag state ignores (bx2_lanes 0x20) still decodes gamma. */
    if (gamma_units || used_gamma) fp_gamma_helper(&p);
    if (expand_units || bx2_units) fp_expand_helper(&p);
    /* NV40 pack/unpack. A packed word travels on in a float's bits, so the
     * packs bitcast and the unpacks bitcast back; the sRGB forms follow
     * RPCS3's _builtin_pkg/_builtin_upg (linear_to_srgb, srgb_to_linear).
     * These were the opcodes inFAMOUS 2's character materials used and the
     * decompiler skipped, leaving the destination with whatever it held.
     * Only for a program that uses one, so every other program's text is
     * unchanged. */
    if (uses_pack) out_puts(&p,
        "uint   _rsx_pk4ub(float4 v) { uint4 b = (uint4)round(saturate(v) * 255.0); return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24); }\n"
        "float4 _rsx_up4ub(float v) { uint u = asuint(v); return float4(u & 255u, (u >> 8) & 255u, (u >> 16) & 255u, (u >> 24) & 255u) / 255.0; }\n"
        "uint   _rsx_pk4b(float4 v) { uint4 b = (uint4)((int4)round(clamp(v, -1.0, 1.0) * 127.0)) & 255u; return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24); }\n"
        "float4 _rsx_up4b(float v) { uint u = asuint(v); int4 b = int4(u & 255u, (u >> 8) & 255u, (u >> 16) & 255u, (u >> 24) & 255u); b = (b << 24) >> 24; return clamp((float4)b / 127.0, -1.0, 1.0); }\n"
        "uint   _rsx_pk2us(float4 v) { uint2 b = (uint2)round(saturate(v.xy) * 65535.0); return b.x | (b.y << 16); }\n"
        "float4 _rsx_up2us(float v) { uint u = asuint(v); return float4(u & 65535u, u >> 16, u & 65535u, u >> 16) / 65535.0; }\n"
        "uint   _rsx_pk2h(float4 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }\n"
        "float4 _rsx_up2h(float v) { uint u = asuint(v); return float4(f16tof32(u & 65535u), f16tof32(u >> 16), f16tof32(u & 65535u), f16tof32(u >> 16)); }\n"
        "float4 _rsx_lin2srgb(float4 cl) { cl = saturate(cl); float4 low = cl * 12.92; float4 high = 1.055 * pow(cl, 1.0 / 2.4) - 0.055; return saturate((cl < 0.0031308) ? low : high); }\n"
        "float4 _rsx_srgb2lin(float4 cs) { float4 a = cs / 12.92; float4 b = pow(max(cs + 0.055, 0.0) / 1.055, 2.4); return (cs > 0.04045) ? b : a; }\n");
    /* RPCS3 _process_texel: TEXTURE_CONTROL0 alpha kill, and _sext_unorm8x4 /
     * the 16-bit form for ARGB8_SIGNED lanes (bit 4: 16-bit channels). */
    if (used_akill) out_puts(&p,
        "float4 _rsx_akill(float4 v) { if (v.a < 0.000001) discard; return v; }\n");
    if (used_sext) out_puts(&p,
        "float4 _rsx_sext_input(float4 v, uint mask) {\n"
        "    bool w = (mask & 16u) != 0u;\n"
        "    uint4 u = (uint4)floor(saturate(v) * (w ? 65535.0 : 255.0) + 0.5);\n"
        "    int4 s = w ? (asint(u << 16) >> 16) : (asint(u << 24) >> 24);\n"
        "    float4 e = (float4)s / (w ? 32767.0 : 127.0);\n"
        "    bool4 m = bool4((mask & 1u) != 0u, (mask & 2u) != 0u, (mask & 4u) != 0u, (mask & 8u) != 0u);\n"
        "    return m ? e : v;\n"
        "}\n");
    if (fs && fs->stipple) {
        char row[24];
        out_puts(&p, "static const uint _rsx_stipple[32] = {");
        for (u32 y = 0; y < 32; y++) {
            snprintf(row, sizeof row, "%s0x%08Xu", y ? "," : "", fs->stipple_pattern[y]);
            out_puts(&p, row);
        }
        out_puts(&p, "};\n");
    }
    const u32 main_sig_off = p.len;
    if (targets == 1 && !depth_export && !depth_cmp) {
        out_puts(&p, "float4 main(PSInput input) : SV_TARGET {\n");
    } else {
        out_puts(&p, "struct PSOutput {\n");
        for (u32 t = 0; t < targets; t++) {
            char member[64];
            snprintf(member, sizeof(member),
                     "    float4 t%u : SV_TARGET%u;\n", t, t);
            out_puts(&p, member);
        }
        if (depth_export || depth_cmp)
            out_puts(&p, "    float depth : SV_Depth;\n");
        out_puts(&p, "};\n"
                     "PSOutput main(PSInput input) {\n");
    }
    out_puts(&p, prolog_buf);

    if (!o.ok || !p.ok) return -1;
    if (has_ret)
        return fp_split_fs_main(out, out_size, o.len, decl_end, preamble, main_sig_off, p.len,
                                fp_nr, fp_nh, decl_init, ucode_bad, dbg_here)
               ? count : -1;
    if (o.len + p.len + 1 > out_size) return -1;
    memmove(out + p.len, out, o.len + 1);
    memcpy(out, preamble, p.len);
    return count;
}

/* Resistance port: the unbuffered (inline-constant) form with per-unit sign
 * expansion, for the D3D12 backend -- it used the plain entry point, so a
 * texture bound UNSIGNED_REMAP_BIASED was never re-centred and every such
 * fetch read +0.5 too bright (the game's carbine came out washed white). */
int rsx_fp_decompile_expand_ex(const u8* ucode, u32 max_bytes, u32 ctrl,
                               u32 tex_cube_mask, const u8 expand_masks[16],
                               char* out, u32 out_size)
{
    return rsx_fp_decompile_internal(
        ucode, max_bytes, ctrl, tex_cube_mask, 0, 0, NULL, NULL, expand_masks, out, out_size, NULL);
}

/* As above, plus per-unit depth-compare (shadow map) sampling. */
int rsx_fp_decompile_shadow_expand_ex(const u8* ucode, u32 max_bytes, u32 ctrl,
                                      u32 tex_cube_mask, const rsx_fp_shadow_desc shadow[16],
                                      const u8 expand_masks[16], char* out, u32 out_size)
{
    return rsx_fp_decompile_internal(
        ucode, max_bytes, ctrl, tex_cube_mask, 0, 0, shadow, NULL, expand_masks, out, out_size, NULL);
}

int rsx_fp_decompile_ex(const u8* ucode, u32 max_bytes, u32 ctrl,
                        u32 tex_cube_mask, char* out, u32 out_size)
{
    return rsx_fp_decompile_internal(
        ucode, max_bytes, ctrl, tex_cube_mask, 0, 0, NULL, NULL, NULL, out, out_size, NULL);
}

int rsx_fp_decompile_buffered_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(
        ucode, max_bytes, ctrl, tex_cube_mask, 0, 1, NULL, NULL, NULL, out, out_size,
        out_constant_count);
}

int rsx_fp_decompile_buffered_shadow_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    const rsx_fp_shadow_desc shadow[16],
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(ucode, max_bytes, ctrl, tex_cube_mask, 0,
        1, shadow, NULL, NULL, out, out_size, out_constant_count);
}

int rsx_fp_decompile_buffered_shadow_color_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    const rsx_fp_shadow_desc shadow[16], const u8 gamma_masks[16],
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(ucode, max_bytes, ctrl, tex_cube_mask, 0,
        1, shadow, gamma_masks, NULL, out, out_size, out_constant_count);
}

int rsx_fp_decompile_buffered_shadow_color_expand_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    const rsx_fp_shadow_desc shadow[16], const u8 gamma_masks[16],
    const u8 expand_masks[16],
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(ucode, max_bytes, ctrl, tex_cube_mask, 0,
        1, shadow, gamma_masks, expand_masks, out, out_size, out_constant_count);
}

int rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask, u32 tex_vol_mask,
    const rsx_fp_shadow_desc shadow[16], const u8 gamma_masks[16],
    const u8 expand_masks[16],
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(ucode, max_bytes, ctrl, tex_cube_mask, tex_vol_mask,
        1, shadow, gamma_masks, expand_masks, out, out_size, out_constant_count);
}

static float fp_half_to_float(u16 h)
{
    const u32 sign = (u32)(h & 0x8000u) << 16;
    u32 exponent = (h >> 10) & 0x1Fu;
    u32 mantissa = h & 0x03FFu;
    u32 bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 127u - 15u + 1u;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                exponent--;
            }
            mantissa &= 0x03FFu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + (127u - 15u)) << 23) | (mantissa << 13);
    }
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

float rsx_fp_alpha_ref(u32 raw, u32 surface_color_format)
{
    /* RPCS3 rsx_methods.h alpha_ref(): W16Z16Y16X16 (0xB) takes the low half
     * as fp16, W32Z32Y32X32 (0xC) / X32 (0xD) bit-cast the word, everything
     * else is an 8-bit unorm. 14..16 are X8B8G8R8/A8B8G8R8, not float
     * formats; RSX_ROP_LEGACY=1 restores that old (wrong) mapping. */
    static int legacy = -1;
    if (legacy < 0) { const char* e = getenv("RSX_ROP_LEGACY"); legacy = (e && *e && *e != '0') ? 1 : 0; }
    const u32 f16 = legacy ? 14u : 0x0Bu;
    const u32 f32a = legacy ? 15u : 0x0Cu, f32b = legacy ? 16u : 0x0Du;
    if (surface_color_format == f16)
        return fp_half_to_float((u16)(raw & 0xFFFFu));
    if (surface_color_format == f32a || surface_color_format == f32b) {
        float value;
        memcpy(&value, &raw, sizeof(value));
        return value;
    }
    return (float)(raw & 0xFFu) / 255.0f;
}

/* The comparison the alpha test has to pass. The alpha it tests is colour
 * target 0's, which for a single-target program is the returned value itself
 * and for an MRT one is the returned struct's first member -- the NV40 alpha
 * test has one input however many targets the program feeds. `cmp` is the
 * CELL_GCM function minus 0x200, with NEVER (0) spelled out and ALWAYS (7)
 * already handled by the caller. */
static void fp_alpha_pass_expr(u32 cmp, const char* alpha, int cmpfix, char* out, u32 out_size)
{
    static const char* op[8] = { NULL, "<", "==", "<=", ">", "!=", ">=", NULL };
    if (cmp == 0 || cmp >= 7 || !op[cmp])
        snprintf(out, out_size, "false");
    else if (cmpfix)
        /* RSXFragmentPrologue.glsl:136-150 comparison_passes, CMP_FIXUP'd. */
        snprintf(out, out_size, "_rsx_cmpfix(%s.a) %s _rsx_cmpfix(_rsx_alpha_ref)", alpha, op[cmp]);
    else
        snprintf(out, out_size, "%s.a %s _rsx_alpha_ref", alpha, op[cmp]);
}

/* Where that alpha lives, and what type the value being returned has. */
static int fp_returns_struct(const char* hlsl)
{
    return strstr(hlsl, "struct PSOutput") != NULL;
}

int rsx_fp_apply_alpha_test(char* hlsl, u32 out_size, u32 func, float ref)
{
    if (!hlsl || out_size == 0)
        return -1;

    const u32 cmp = (func | 0x0200u) <= 0x0207u ? (func | 0x0200u) - 0x0200u : 7u;
    if (cmp == 7u)
        return 0;

    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p;
         p = strstr(p + 1, "    return "))
        marker = p;
    if (!marker)
        return -1;
    char* expression = marker + strlen("    return ");
    char* terminator = strstr(expression, ";\n}\n");
    if (!terminator || terminator == expression ||
        (size_t)(terminator - expression) >= 96)
        return -1;
    char output[96];
    memcpy(output, expression, (size_t)(terminator - expression));
    output[terminator - expression] = '\0';

    const int mrt = fp_returns_struct(hlsl);
    char pass[96];
    fp_alpha_pass_expr(cmp, mrt ? "_rsx_out.t0" : "_rsx_out",
                       strstr(hlsl, FP_CMPFIX_DEFINE) != NULL, pass, sizeof(pass));
    char suffix[512];
    const int n = snprintf(suffix, sizeof(suffix),
        "    %s _rsx_out = %s;\n"
        "    const float _rsx_alpha_ref = %.9g;\n"
        "    if (!(%s)) discard;\n"
        "    return _rsx_out;\n}\n",
        mrt ? "PSOutput" : "float4", output, (double)ref, pass);
    if (n < 0 || (u32)n >= sizeof(suffix))
        return -1;
    const size_t prefix = (size_t)(marker - hlsl);
    if (prefix + (size_t)n + 1 > out_size)
        return -1;
    memcpy(marker, suffix, (size_t)n + 1);
    return 1;
}

/* Resistance port: alpha test driven by the per-draw b1 slot the D3D12
 * backend fills (float4 #4 = rsx_alphatest: x enable, y ref, z func & 7), so
 * one compiled program serves every alpha-test state. The backend never
 * applied the test at all: alpha-cut foliage (York's weeds), fences and
 * decals drew as solid cards. Shares b1 with the shadow-map scale cbuffer
 * when the program has one. Returns 1 if applied. */
int rsx_fp_apply_alpha_test_dynamic(char* hlsl, u32 out_size)
{
    if (!hlsl || out_size == 0) return -1;
    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p; p = strstr(p + 1, "    return "))
        marker = p;
    if (!marker) return -1;
    char* expression = marker + strlen("    return ");
    char* terminator = strstr(expression, ";\n}\n");
    if (!terminator || terminator == expression || (size_t)(terminator - expression) >= 96)
        return -1;
    char output[96];
    memcpy(output, expression, (size_t)(terminator - expression));
    output[terminator - expression] = '\0';
    const int mrt = fp_returns_struct(hlsl);
    char suffix[1024];
    const int n = snprintf(suffix, sizeof(suffix),
        "    %s _rsx_out = %s;\n"
        "    if (rsx_alphatest.x > 0.5) {\n"
        /* v163b: the RSX tests the alpha the colour buffer would get -- clamped
         * to [0,1] and quantised to 8 bits -- against the 8-bit ref. Fall of
         * Man's grass outputs 0.5 - coverage and tests EQUAL 0: every covered
         * pixel clamps to 0 and passes. Compared raw (negative) it all failed. */
        "        const float _ra = round(rsx_alphatest.y * 255.0); const uint _rf = (uint)(rsx_alphatest.z + 0.5);\n"
        "        const float _aa = round(saturate(%s.a) * 255.0);\n"
        "        const bool _rp = (_rf == 1u) ? (_aa < _ra) : (_rf == 2u) ? (_aa == _ra) :\n"
        "                         (_rf == 3u) ? (_aa <= _ra) : (_rf == 4u) ? (_aa > _ra) :\n"
        "                         (_rf == 5u) ? (_aa != _ra) : (_rf == 6u) ? (_aa >= _ra) : (_rf == 7u);\n"
        "        if (!_rp) discard;\n"
        "    }\n"
        "    return _rsx_out;\n}\n",
        mrt ? "PSOutput" : "float4", output, mrt ? "_rsx_out.t0" : "_rsx_out");
    if (n < 0 || (u32)n >= sizeof(suffix)) return -1;
    /* the cbuffer: extend the shadow one, or declare our own before main */
    static const char shold[] = "cbuffer RsxShadowScale : register(b1) { float4 _rsx_b1pad[69]; float4 rsx_shscale; };\n";
    static const char shnew[] = "cbuffer RsxShadowScale : register(b1) { float4 _rsx_b1pad[4]; float4 rsx_alphatest; float4 _rsx_b1pad2[64]; float4 rsx_shscale; };\n";
    static const char own[]   = "cbuffer RsxAlphaTest : register(b1) { float4 _rsx_atpad[4]; float4 rsx_alphatest; };\n";
    char* sh = strstr(hlsl, shold);
    const char* ins = NULL; char* at = NULL; size_t cut = 0;
    if (sh) { ins = shnew; at = sh; cut = sizeof(shold) - 1; }
    else {
        at = strstr(hlsl, "float4 main(");
        if (!at) at = strstr(hlsl, "PSOutput main(");
        if (!at) return -1;
        ins = own; cut = 0;
    }
    const size_t len = strlen(hlsl), insl = strlen(ins);
    const size_t prefix = (size_t)(marker - hlsl);
    (void)len;
    if (prefix + (size_t)n + insl - cut + 1 > out_size) return -1;
    /* 1. replace the return tail (marker is after `at`) */
    memcpy(marker, suffix, (size_t)n + 1);
    /* 2. insert/replace the cbuffer declaration */
    {
        const size_t tail = strlen(at + cut) + 1;
        memmove(at + insl, at + cut, tail);
        memcpy(at, ins, insl);
    }
    return 1;
}

int rsx_fp_apply_alpha_test_buffered(
    char* hlsl, u32 out_size, u32 func)
{
    if (!hlsl || out_size == 0)
        return -1;

    const u32 cmp =
        (func | 0x0200u) <= 0x0207u ? (func | 0x0200u) - 0x0200u : 7u;
    if (cmp == 7u)
        return 0;

    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p;
         p = strstr(p + 1, "    return "))
        marker = p;
    if (!marker)
        return -1;
    char* expression = marker + strlen("    return ");
    char* terminator = strstr(expression, ";\n}\n");
    if (!terminator || terminator == expression ||
        (size_t)(terminator - expression) >= 96)
        return -1;
    char output[96];
    memcpy(output, expression, (size_t)(terminator - expression));
    output[terminator - expression] = '\0';

    const int mrt = fp_returns_struct(hlsl);
    char pass[96];
    fp_alpha_pass_expr(cmp, mrt ? "_rsx_out.t0" : "_rsx_out",
                       strstr(hlsl, FP_CMPFIX_DEFINE) != NULL, pass, sizeof(pass));
    char suffix[512];
    const int n = snprintf(
        suffix, sizeof(suffix),
        "    %s _rsx_out = %s;\n"
        "    const float _rsx_alpha_ref = fp_alpha.x;\n"
        "    if (!(%s)) discard;\n"
        "    return _rsx_out;\n}\n",
        mrt ? "PSOutput" : "float4", output, pass);
    if (n < 0 || (u32)n >= sizeof(suffix))
        return -1;
    const size_t prefix = (size_t)(marker - hlsl);
    if (prefix + (size_t)n + 1 > out_size)
        return -1;
    memcpy(marker, suffix, (size_t)n + 1);
    return 1;
}

/* --- kept from this tree when adopting the newer decompiler ---------------
 * rsx_d3d12_backend.c calls both of these: extract_consts feeds the per-draw
 * fp_k[] slots, code_hash keys the PSO cache on the ucode rather than the
 * constants. The live-draw engine does not need them, so they are absent
 * upstream in caner's fork. */

/* Walk a program the same way the decompiler does and copy out its inline
 * constants in the SAME order the decompiler assigns fp_k[] indices. The backend
 * calls this per draw, so re-patched constants take effect without recompiling. */
int rsx_fp_extract_consts(const u8* ucode, u32 max_bytes, float* out, int max_out)
{
    if (!ucode || !out) return 0;
    u32 off = 0; int n = 0;
    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        int is_branch = (w2 & FP_BRANCH) != 0;
        (void)is_branch;   /* v142: count slots exactly as the decompiler does -- it does not skip branches */
        int has_k =
            ((((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) ||
             (((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) ||
             (((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST));
        if (has_k) {
            if (off + 16 <= max_bytes) {
                for (int i = 0; i < 4; i++) {
                    u32 cw = rsx_fp_sanitize_constant_word(
                        rsx_fp_read_word(ucode + off + i * 4));
                    if (n < max_out) memcpy(&out[n * 4 + i], &cw, 4);
                }
                off += 16;
            }
            if (n < max_out) n++;
        }
        if (w0 & FP_END) break;
    }
    { static int dbg = -1;
      if (dbg < 0) { const char* e = getenv("FP_KDBG"); dbg = e ? atoi(e) : 0; }
      if (dbg) { static int m = 0; if (PS3_LOG_FIRST(m, 3)) {
        fprintf(stderr, "[FPK] %d consts%c", n, 10);
        for (int _q = 0; _q < n; _q++)
            fprintf(stderr, "   k[%2d] = (%g %g %g %g)%c", _q, out[_q*4+0],
                    out[_q*4+1], out[_q*4+2], out[_q*4+3], 10); } }
    }
    return n;
}

/* FNV-1a over a program's INSTRUCTION words only, skipping the inline constant
 * slots. With constants hoisted into fp_k[] the compiled shader does not depend
 * on their values, so including them in the pipeline key made it change every
 * time the title re-patched a constant -- which is exactly what thrashed the
 * cache. */
u32 rsx_fp_code_hash(const u8* ucode, u32 max_bytes)
{
    u32 h = 2166136261u;
    if (!ucode) return h;
    u32 off = 0;
    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        for (u32 i = 0; i < 16; i++) { h ^= ucode[off + i]; h *= 16777619u; }
        off += 16;
        int is_branch = (w2 & FP_BRANCH) != 0;
        int has_k = !is_branch &&
            ((((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) ||
             (((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) ||
             (((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST));
        if (has_k) off += 16;            /* skip the constant, do not hash it */
        if (w0 & FP_END) break;
    }
    return h;
}

/* Scale texel-space coordinates to the 0..1 range HLSL sampling expects.
 *
 * An RSX texture whose format carries RSX_TEX_FMT_UNNORM (0x40) is addressed in
 * TEXELS, not normalised units. ps1_netemu composites the PS1 framebuffer with
 * exactly that: format 0xE2 (A1R5G5B5 | UNNORM | LINEAR) over a 1024x512
 * texture, and a quad whose texture coordinates run 1..638 by 2..474 --
 *
 *     [uvdbg] unit=0 ps1fb 1024x512 pitch=2048 fmt=0xE2
 *     [uvdbg]   v0 pos(-1.0 -1.0) tc(1.0000 2.0000)
 *     [uvdbg]   v2 pos(0.8 0.8)   tc(638.0000 474.0000)
 *
 * The bit was only ever masked OUT of the base format so the decoder could
 * recognise A1R5G5B5, and never honoured for addressing. So the sampler was
 * handed coordinates hundreds of times out of range, which under clamp lands on
 * one edge texel for the whole quad -- a flat, uniform result. That is the
 * black screen: the PS1 renders, the composite pass runs, and it samples one
 * corner texel.
 *
 * D3D12 has no unnormalised addressing mode, so the division has to happen in
 * the shader. Doing it as a post-pass over the generated HLSL rather than
 * inside the emitter keeps it beside rsx_fp_apply_alpha_test, which already
 * works this way, and leaves every non-UNNORM program byte-identical.
 *
 * Cube units are skipped: their coordinate is a float3 direction, and
 * RSX_TEX_FMT_UNNORM has no meaning for one.
 */
int rsx_fp_apply_unnorm_scale(char* hlsl, u32 hlsl_size, u32 unnorm_mask,
                              const u32 dim[][2], u32 cube_mask)
{
    return rsx_fp_apply_unnorm_scale_ex(hlsl, hlsl_size, unnorm_mask, dim, cube_mask, 0);
}

/* nearest_mask: units whose min or mag filter is NEAREST get RPCS3's
 * subpixel bias (RSXGraphEngine.cpp:1265-1278): (x + 0.01) * scale, so a
 * texel address that lands exactly on a texel edge rounds into the texel
 * it names despite the division's precision. */
int rsx_fp_apply_unnorm_scale_ex(char* hlsl, u32 hlsl_size, u32 unnorm_mask,
                                 const u32 dim[][2], u32 cube_mask, u32 nearest_mask)
{
    return rsx_fp_apply_unnorm_scale_vol(hlsl, hlsl_size, unnorm_mask, dim, cube_mask,
                                         nearest_mask, 0, NULL);
}

/* vol_mask: units declared Texture3D, whose float3 coordinate is scaled by
 * 1 / {width, height, vol_depth[u]} (RPCS3 texture_cache.h upload_texture:
 * an unnormalised 3D texture divides all three axes). */
int rsx_fp_apply_unnorm_scale_vol(char* hlsl, u32 hlsl_size, u32 unnorm_mask,
                                  const u32 dim[][2], u32 cube_mask, u32 nearest_mask,
                                  u32 vol_mask, const u32* vol_depth)
{
    if (!hlsl || !unnorm_mask) return 0;
    int patched = 0;

    for (u32 u = 0; u < 16; u++) {
        if (!((unnorm_mask >> u) & 1u)) continue;
        if ((cube_mask >> u) & 1u) continue;
        const u32 w = dim[u][0], h = dim[u][1];
        if (!w || !h) continue;
        const int bias = (int)((nearest_mask >> u) & 1u);
        const u32 d = (vol_depth && ((vol_mask >> u) & 1u)) ? vol_depth[u] : 0u;

        /* Both spellings the emitter produces for a 2D unit. */
        char needles[2][40];
        snprintf(needles[0], sizeof needles[0], "rsx_tex[%u].Sample(rsx_samp[%u],", u, u);
        snprintf(needles[1], sizeof needles[1], "rsx_tex%u.Sample(rsx_samp[%u],", u, u);

        for (int nidx = 0; nidx < 2; nidx++) {
            const char* needle = needles[nidx];
            const u32 nlen = (u32)strlen(needle);
            for (;;) {
                char* at = strstr(hlsl, needle);
                if (!at) break;
                /* The '(' of Sample(, then its matching ')'. */
                char* open = at + nlen - 1;   /* the ',' ... step back to '(' */
                while (open > at && *open != '(') open--;
                if (*open != '(') break;
                int depth = 0;
                char* p = open;
                for (; *p; p++) {
                    if (*p == '(') depth++;
                    else if (*p == ')') { depth--; if (!depth) break; }
                }
                if (*p != ')') break;

                char scale[200];
                if (d && bias)
                    snprintf(scale, sizeof scale,
                             ") * float3(%.9g, %.9g, %.9g) + float3(%.9g, %.9g, %.9g)",
                             1.0 / (double)w, 1.0 / (double)h, 1.0 / (double)d,
                             0.01 / (double)w, 0.01 / (double)h, 0.01 / (double)d);
                else if (d)
                    snprintf(scale, sizeof scale, " * float3(%.9g, %.9g, %.9g)",
                             1.0 / (double)w, 1.0 / (double)h, 1.0 / (double)d);
                else if (bias)   /* the coordinate is parenthesised: (c) * s + b */
                    snprintf(scale, sizeof scale, ") * float2(%.9g, %.9g) + float2(%.9g, %.9g)",
                             1.0 / (double)w, 1.0 / (double)h,
                             0.01 / (double)w, 0.01 / (double)h);
                else
                    snprintf(scale, sizeof scale, " * float2(%.9g, %.9g)",
                             1.0 / (double)w, 1.0 / (double)h);
                const u32 slen = (u32)strlen(scale);
                const u32 used = (u32)strlen(hlsl);
                if (used + slen + (bias ? 1u : 0u) + 1 > hlsl_size) return -1;

                memmove(p + slen, p, used - (u32)(p - hlsl) + 1);
                memcpy(p, scale, slen);
                if (bias) {
                    char* c = at + nlen;
                    memmove(c + 1, c, strlen(c) + 1);
                    *c = '(';
                }
                patched++;

                /* Mark this call site done so the next strstr moves past it:
                 * the inserted text is inside the call, so an unmarked scan
                 * would find the same site forever. */
                memcpy(at, "rsx_TEX", 7);
            }
        }
        /* Restore the marker. */
        for (char* q = hlsl; (q = strstr(q, "rsx_TEX")) != NULL; )
            memcpy(q, "rsx_tex", 7);
    }
    return patched;
}

/* NV4097_SET_SHADER_PACKER accepts any nonzero value. Floating surfaces and
 * 32-bit exports bypass the integer framebuffer's RGB transfer function. */
int rsx_fp_srgb_output_enabled(u32 packer, u32 ctrl, u32 color_format)
{
    return packer != 0 && !(ctrl & FP_CTRL_32BIT_EXPORTS) &&
           color_format != 0x0Bu && color_format != 0x0Cu && color_format != 0x0Du;
}

int rsx_fp_apply_srgb_output(char* hlsl, u32 capacity)
{
    static const char helper[] =
        "float4 _rsx_srgb_output(float4 v) {\n"
        "    float3 lo = v.rgb * 12.92;\n"
        "    float3 hi = 1.055 * pow(max(v.rgb, (float3)0), (float3)(1.0 / 2.4)) - 0.055;\n"
        "    float3 rgb = float3(v.r < 0.0031308 ? lo.r : hi.r,\n"
        "                         v.g < 0.0031308 ? lo.g : hi.g,\n"
        "                         v.b < 0.0031308 ? lo.b : hi.b);\n"
        "    return float4(saturate(rgb), v.a);\n}\n";
    if (!hlsl || !capacity) return -1;
    size_t len = 0;
    while (len < capacity && hlsl[len]) ++len;
    if (len == capacity || strstr(hlsl, "_rsx_srgb_output")) return -1;
    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p;
         p = strstr(p + 1, "    return ")) marker = p;
    if (!marker) return -1;
    const char* expression = marker + strlen("    return ");
    const char* end = strstr(expression, ";\n}\n");
    if (!end || end == expression || (size_t)(end - expression) >= 96 || end[4])
        return -1;
    char output[96];
    memcpy(output, expression, (size_t)(end - expression));
    output[end - expression] = 0;
    const int mrt = strstr(hlsl, "struct PSOutput") != NULL;
    char suffix[1024];
    int n = snprintf(suffix, sizeof suffix, "    %s _rsx_color_out = %s;\n",
                     mrt ? "PSOutput" : "float4", output);
    if (n < 0 || (size_t)n >= sizeof suffix) return -1;
    if (mrt) {
        unsigned targets = 0;
        for (unsigned t = 0; t < 4; ++t) {
            char field[64];
            snprintf(field, sizeof field, "    float4 t%u : SV_TARGET%u;", t, t);
            if (!strstr(hlsl, field)) continue;
            const int added = snprintf(suffix + n, sizeof suffix - (size_t)n,
                "    _rsx_color_out.t%u = _rsx_srgb_output(_rsx_color_out.t%u);\n", t, t);
            if (added < 0 || (size_t)added >= sizeof suffix - (size_t)n) return -1;
            n += added; ++targets;
        }
        if (!targets) return -1;
    } else {
        const int added = snprintf(suffix + n, sizeof suffix - (size_t)n,
            "    _rsx_color_out = _rsx_srgb_output(_rsx_color_out);\n");
        if (added < 0 || (size_t)added >= sizeof suffix - (size_t)n) return -1;
        n += added;
    }
    const int added = snprintf(suffix + n, sizeof suffix - (size_t)n,
                              "    return _rsx_color_out;\n}\n");
    if (added < 0 || (size_t)added >= sizeof suffix - (size_t)n) return -1;
    n += added;
    const size_t prefix = (size_t)(marker - hlsl);
    const size_t helper_len = sizeof helper - 1;
    if (helper_len + prefix + (size_t)n + 1 > capacity) return -1;
    /* All parsing/capacity checks precede the first mutation. */
    memcpy(marker, suffix, (size_t)n + 1);
    memmove(hlsl + helper_len, hlsl, prefix + (size_t)n + 1);
    memcpy(hlsl, helper, helper_len);
    return 1;
}

/* RPCS3 RSXProgrammableBlendPrologue.glsl get_blend_factor_rgb / _a over
 * _pbs (shader colour), _pbd (destination) and _pbk (blend constants). */
static const char* fp_pb_factor(u32 f, int alpha)
{
    switch (f) {
    case 0x0000u: return alpha ? "0.0" : "(float3)0.0";
    case 0x0001u: return alpha ? "1.0" : "(float3)1.0";
    case 0x0300u: return alpha ? "_pbs.a" : "_pbs.rgb";
    case 0x0301u: return alpha ? "(1.0 - _pbs.a)" : "(1.0 - _pbs.rgb)";
    case 0x0302u: return alpha ? "_pbs.a" : "_pbs.aaa";
    case 0x0303u: return alpha ? "(1.0 - _pbs.a)" : "(1.0 - _pbs.aaa)";
    case 0x0304u: return alpha ? "_pbd.a" : "_pbd.aaa";
    case 0x0305u: return alpha ? "(1.0 - _pbd.a)" : "(1.0 - _pbd.aaa)";
    case 0x0306u: return alpha ? "_pbd.a" : "_pbd.rgb";
    case 0x0307u: return alpha ? "(1.0 - _pbd.a)" : "(1.0 - _pbd.rgb)";
    case 0x0308u: return alpha ? "1.0" : "(float3)min(_pbs.a, 1.0 - _pbd.a)";
    case 0x8001u: return alpha ? "_pbk.a" : "_pbk.rgb";
    case 0x8002u: return alpha ? "(1.0 - _pbk.a)" : "(1.0 - _pbk.rgb)";
    case 0x8003u: return alpha ? "_pbk.a" : "_pbk.aaa";
    case 0x8004u: return alpha ? "(1.0 - _pbk.a)" : "(1.0 - _pbk.aaa)";
    default:      return alpha ? "0.0" : "(float3)0.0";
    }
}

/* RPCS3 apply_blend_func_rgb / _a: the signed equations ignore the factors;
 * _blend_saturate(_signed) only act on an integer framebuffer. */
static int fp_pb_channel(char* out, size_t size, const rsx_fp_frag_state* fs, int alpha)
{
    const char* c = alpha ? "a" : "rgb";
    const char* ut = alpha ? "uint" : "uint3";
    const char* ft = alpha ? "float" : "float3";
    char s[64], d[64], ss[160], ds[160], sat_s[64];
    snprintf(s, sizeof s, "_pbs.%s", c);
    snprintf(d, sizeof d, "_pbd.%s", c);
    if (fs->pb_int) {
        snprintf(ss, sizeof ss, "(%s(asint(%s(saturate(_pbs.%s) * 255.0 + 0.5) << 24) >> 24) / 255.0)",
                 ft, ut, c);
        snprintf(ds, sizeof ds, "(%s(asint(%s(saturate(_pbd.%s) * 255.0 + 0.5) << 24) >> 24) / 255.0)",
                 ft, ut, c);
        snprintf(sat_s, sizeof sat_s, "saturate(_pbs.%s)", c);
    } else {
        snprintf(ss, sizeof ss, "_pbs.%s", c);
        snprintf(ds, sizeof ds, "_pbd.%s", c);
        snprintf(sat_s, sizeof sat_s, "_pbs.%s", c);
    }
    const u32 eq = fs->pb_eq[alpha ? 1 : 0];
    const char* fsrc = fp_pb_factor(fs->pb_sf[alpha ? 1 : 0], alpha);
    const char* fdst = fp_pb_factor(fs->pb_df[alpha ? 1 : 0], alpha);
    const char* sat_open = fs->pb_int ? "saturate(" : "(";
    switch (eq) {
    case 0x8007u: return snprintf(out, size, "min(%s, %s)", sat_s, d);
    case 0x8008u: return snprintf(out, size, "max(%s, %s)", sat_s, d);
    case 0xF005u: return snprintf(out, size, "%s - %s", d, ss);
    case 0xF006u: return snprintf(out, size, "%s + %s", ss, d);
    case 0xF007u: return snprintf(out, size, "%s + %s", sat_s, ds);
    case 0x8006u: return snprintf(out, size, "%s%s * %s) + %s * %s", sat_open, s, fsrc, d, fdst);
    case 0x800Au: return snprintf(out, size, "%s%s * %s) - %s * %s", sat_open, s, fsrc, d, fdst);
    case 0x800Bu: return snprintf(out, size, "%s * %s - %s%s * %s)", d, fdst, sat_open, s, fsrc);
    default:      return snprintf(out, size, "(%s)0.0", ft);
    }
}

/* RPCS3 RSXROPEpilogue around the alpha test. Stage 0: round_to_8bit on
 * every colour target of an integer framebuffer (_fx12_truncate, then
 * floor(v*255+0.5)/255, negatives to 0). Stage 1: the stochastic
 * coverage_test_passes on col0 (RPCS3's emulation when hardware A2C is not
 * used, as here: the backend has neither A2C nor multisampling), then
 * alpha-to-one, which RPCS3 leaves to the hardware's blend state. */
int rsx_fp_apply_rop_stage(char* hlsl, u32 capacity, const rsx_fp_frag_state* fs,
                           int stage)
{
    if (!hlsl || !capacity || !fs) return -1;
    if (stage == 0 ? !fs->rop_round
                   : !(fs->alpha_to_coverage || fs->alpha_to_one || fs->rop_remap || fs->pb))
        return 0;
    size_t len = 0;
    while (len < capacity && hlsl[len]) ++len;
    if (len == capacity) return -1;
    const char* var = stage == 0 ? "_rsx_rnd" : "_rsx_cov";
    if (strstr(hlsl, var)) return -1;
    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p;
         p = strstr(p + 1, "    return ")) marker = p;
    if (!marker) return -1;
    const char* expression = marker + strlen("    return ");
    const char* end = strstr(expression, ";\n}\n");
    if (!end || end == expression || (size_t)(end - expression) >= 96 || end[4])
        return -1;
    char output[96];
    memcpy(output, expression, (size_t)(end - expression));
    output[end - expression] = 0;
    const int mrt = strstr(hlsl, "struct PSOutput") != NULL;
    char targets[4][16];
    unsigned ntargets = 0;
    if (mrt) {
        for (unsigned t = 0; t < 4; ++t) {
            char field[64];
            snprintf(field, sizeof field, "    float4 t%u : SV_TARGET%u;", t, t);
            if (strstr(hlsl, field))
                snprintf(targets[ntargets++], sizeof targets[0], "%s.t%u", var, t);
        }
        if (!ntargets) return -1;
    } else {
        snprintf(targets[ntargets++], sizeof targets[0], "%s", var);
    }
    char suffix[4096];
    int n = snprintf(suffix, sizeof suffix, "    %s %s = %s;\n",
                     mrt ? "PSOutput" : "float4", var, output);
    if (n < 0 || (size_t)n >= sizeof suffix) return -1;
    for (unsigned i = 0; i < ntargets; ++i) {
        int added = 0;
        if (stage == 0)
            added = snprintf(suffix + n, sizeof suffix - (size_t)n,
                "    %s = max(floor(asfloat(asuint(%s) & 0xFFFFF000u) * 255.0 + 0.5), (float4)0) / 255.0;\n",
                targets[i], targets[i]);
        else if (i == 0 && fs->alpha_to_coverage)
            added = snprintf(suffix + n, sizeof suffix - (size_t)n,
                "    if (!(%s.a > frac(sin(dot(input.position.xy, float2(12.9898, 78.233))) * 43758.5453))) discard;\n",
                targets[i]);
        if (added < 0 || (size_t)added >= sizeof suffix - (size_t)n) return -1;
        n += added;
    }
    if (stage != 0 && fs->pb) {
        char rgb[512], a[512];
        if (fp_pb_channel(rgb, sizeof rgb, fs, 0) >= (int)sizeof rgb ||
            fp_pb_channel(a, sizeof a, fs, 1) >= (int)sizeof a) return -1;
        const int sep = strstr(hlsl, "Texture2D    rsx_tex[16]") == NULL;
        int added = snprintf(suffix + n, sizeof suffix - (size_t)n,
            "    float4 _pbs = %s;\n"
            "    float4 _pbd = rsx_tex%s%u%s.Load(int3(input.position.xy, 0));\n"
            "    float4 _pbk = float4(%.9g, %.9g, %.9g, %.9g);\n"
            "    %s = float4(%s, %s);\n",
            targets[0], sep ? "" : "[", (u32)fs->pb_unit, sep ? "" : "]",
            fs->pb_const[0], fs->pb_const[1], fs->pb_const[2], fs->pb_const[3],
            targets[0], rgb, a);
        if (added < 0 || (size_t)added >= sizeof suffix - (size_t)n) return -1;
        n += added;
        /* do_blend under _ENABLE_ROP_OUTPUT_ROUNDING: round_to_8bit again. */
        if (fs->pb_int) {
            added = snprintf(suffix + n, sizeof suffix - (size_t)n,
                "    %s = max(floor(asfloat(asuint(%s) & 0xFFFFF000u) * 255.0 + 0.5), (float4)0) / 255.0;\n",
                targets[0], targets[0]);
            if (added < 0 || (size_t)added >= sizeof suffix - (size_t)n) return -1;
            n += added;
        }
    }
    if (stage != 0 && fs->rop_remap >= 1u && fs->rop_remap <= 4u) {
        static const char* const remap[5] = { "", "%s = %s.bbbb;\n", "%s = %s.bgbg;\n",
                                              "%s.a = 0.0;\n", "%s.a = 1.0;\n" };
        for (unsigned i = 0; i < ntargets; ++i) {
            char line[64];
            snprintf(line, sizeof line, remap[fs->rop_remap], targets[i], targets[i]);
            const int added = snprintf(suffix + n, sizeof suffix - (size_t)n, "    %s", line);
            if (added < 0 || (size_t)added >= sizeof suffix - (size_t)n) return -1;
            n += added;
        }
    }
    /* Alpha-to-one is pipeline state in RPCS3 (alphaToOneEnable): it acts
     * on the stored sample after the shader's output remap. */
    if (stage != 0 && fs->alpha_to_one)
        for (unsigned i = 0; i < ntargets; ++i) {
            const int added = snprintf(suffix + n, sizeof suffix - (size_t)n,
                                       "    %s.a = 1.0;\n", targets[i]);
            if (added < 0 || (size_t)added >= sizeof suffix - (size_t)n) return -1;
            n += added;
        }
    const int added = snprintf(suffix + n, sizeof suffix - (size_t)n,
                               "    return %s;\n}\n", var);
    if (added < 0 || (size_t)added >= sizeof suffix - (size_t)n) return -1;
    n += added;
    const size_t prefix = (size_t)(marker - hlsl);
    if (prefix + (size_t)n + 1 > capacity) return -1;
    memcpy(marker, suffix, (size_t)n + 1);
    return 1;
}

/* RPCS3 declares the back colours (VP o3/o4) whenever two-sided lighting is
 * on and selects them on gl_FrontFacing. Appended after the TC9 output so
 * every other interpolant keeps its location. */
int rsx_fp_patch_vs_back_colors(char* vs_hlsl, u32 capacity)
{
    static const char decl_at[] = "float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;\n";
    static const char decl[] = "    float4 bc0:COLOR2; float4 bc1:COLOR3;\n";
    static const char ret_at[] = "    return Out;";
    static const char assign[] =
        "    Out.bc0 = (_om & 5u) != 0u ? o[3] : _d; Out.bc1 = (_om & 10u) != 0u ? o[4] : _d;\n";
    if (!vs_hlsl || !capacity) return -1;
    if (strstr(vs_hlsl, "bc0:COLOR2")) return 0;
    size_t len = 0;
    while (len < capacity && vs_hlsl[len]) ++len;
    if (len == capacity) return -1;
    char* d = strstr(vs_hlsl, decl_at);
    char* r = NULL;
    for (char* p = strstr(vs_hlsl, ret_at); p; p = strstr(p + 1, ret_at)) r = p;
    if (!d || !r || r < d) return -1;
    const size_t dl = sizeof decl - 1, al = sizeof assign - 1;
    if (len + dl + al + 1 > capacity) return -1;
    /* The return first: it lies after the declaration, which then stays put. */
    memmove(r + al, r, len - (size_t)(r - vs_hlsl) + 1);
    memcpy(r, assign, al);
    len += al;
    d += sizeof decl_at - 1;
    memmove(d + dl, d, len - (size_t)(d - vs_hlsl) + 1);
    memcpy(d, decl, dl);
    return 1;
}

int rsx_fp_decompile_buffered_color_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    const u8 gamma_masks[16], char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(ucode, max_bytes, ctrl, tex_cube_mask, 0,
        1, NULL, gamma_masks, NULL, out, out_size, out_constant_count);
}
