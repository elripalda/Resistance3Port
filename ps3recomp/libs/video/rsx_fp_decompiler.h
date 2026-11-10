/*
 * ps3recomp - RSX Fragment Program (NV40 ISA) → HLSL decompiler
 *
 * Translates an RSX fragment shader (NVIDIA NV40 "nvfx" fragment program
 * encoding) into an HLSL pixel shader source string suitable for D3DCompile.
 *
 * See docs/RSX_FRAGMENT_PROGRAM.md for the instruction encoding reference
 * (sourced from Mesa/nouveau nvfx_shader.h). This module is self-contained:
 * it has no D3D12 dependency and can be built/tested standalone.
 */

#ifndef PS3RECOMP_RSX_FP_DECOMPILER_H
#define PS3RECOMP_RSX_FP_DECOMPILER_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_FP_BUFFERED_CONSTANTS_API 1
#define RSX_FP_MAX_INLINE_CONSTANTS 2048u

/* Source capacity for native backends decoding up to 4096 bytecode bytes.
 * Predicated instructions can expand beyond 32 KiB of HLSL. This is a caller
 * allocation size, not a limit imposed by the decompiler API. */
#define RSX_FP_HLSL_BUFFER_SIZE (256u * 1024u)

/* Runtime payload for a structurally decompiled fragment program. Values are
 * retained as host-order IEEE-754 bit patterns so signed zero, infinities,
 * NaNs, and NaN payloads survive the guest-word conversion exactly. One slot
 * is consumed per instruction that references CONST; multiple CONST operands
 * in that instruction share the slot, matching the RSX encoding. */
typedef struct rsx_fp_constant_block {
    u32 count;
    u32 values[RSX_FP_MAX_INLINE_CONSTANTS][4];
} rsx_fp_constant_block;

/* Read one fragment-program word from guest (PS3) memory: big-endian load
 * followed by a 16-bit half-word swap, yielding a host-order instruction
 * word. The decoder operates on host-order words. */
u32 rsx_fp_read_word(const u8* p);
u32 rsx_fp_sanitize_constant_word(u32 w);
/* Internal-resolution scale the generated WPOS / unnormalised lookups divide
 * by. Default: RSX_RES_SCALE (the D3D12 backend scales its surfaces); a
 * backend that renders at guest resolution sets 1 before compiling. */
void rsx_fp_set_res_scale(double scale);
/* 1: engine-path shaders take the scale per draw from PSConstants: after
 * fp_alpha, float4 fp_rscale[5] -- [0].x = 1 / the colour target's scale,
 * [1 + u/4][u%4] = 1 / texture unit u's. */
void rsx_fp_set_res_scale_dynamic(int on);
/* Texture units whose TEXTURE_FORMAT dimension is 1 (not cube/volume): the
 * next decompile samples them at float2(coord.x, 0.5) -- RPCS3's TEX1D*
 * read .x only. 0 (the default) is every unit 2D. */
void rsx_fp_set_tex_1d_mask(u32 mask);
extern unsigned long long g_rsx_fp_nonfinite_constants;

/* Decompile an NV40 fragment program into an HLSL pixel shader.
 *
 *   ucode     : pointer to the fragment-program bytecode in guest memory
 *               (big-endian, half-word-swapped — i.e. exactly as the RSX
 *               reads it; this function applies rsx_fp_read_word internally).
 *   max_bytes : safety bound on how far to read (program stops at PROGRAM_END
 *               or when this many bytes are consumed).
 *   out       : caller buffer for the generated HLSL (NUL-terminated).
 *   out_size  : size of `out` in bytes.
 *
 * Returns the number of instructions decoded (>=0) on success, or -1 on
 * error (null args, output overflow). The emitted entry point is
 * `float4 main(PSInput input) : SV_TARGET`, matching the backend's
 * placeholder PSInput { float4 position : SV_POSITION; float4 col : COLOR; }.
 *
 * Texture sampling and full input plumbing are partial — see the doc's
 * "Integration TODO". Unhandled opcodes emit a comment and a safe default so
 * the shader still compiles.
 *
 *   ctrl : the NV4097_SET_SHADER_CONTROL word (reg 0x1D60) for this program.
 *          Bit 0x40 (CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS) selects the
 *          fragment-output register set: clear => fp16 output (color = h0),
 *          set => fp32 output (color = r0). Pass RSX_FP_CTRL_AUTO when no
 *          control word is available (standalone tests) to fall back to the
 *          legacy "whichever of r0/h0 was written" heuristic. */
#define RSX_FP_CTRL_AUTO 0xFFFFFFFFu
int rsx_fp_decompile(const u8* ucode, u32 max_bytes, u32 ctrl, char* out, u32 out_size);

/* Apply the NV4097 fixed-function alpha test to an already-decompiled pixel
 * shader. D3D12 has no fixed-function alpha test, so the comparison is
 * emitted immediately before the shader's final color return. `func` uses
 * the CELL_GCM comparison values 0x200..0x207 and `ref` is normalized to the
 * render target's component representation. Returns 1 when a test was
 * inserted, 0 for ALWAYS, and -1 if the generated shader could not be
 * patched safely. */
int rsx_fp_apply_alpha_test_dynamic(char* hlsl, u32 out_size);
int rsx_fp_apply_alpha_test(char* hlsl, u32 out_size, u32 func, float ref);

/* Decode NV4097_SET_ALPHA_REF for the active surface color format. Ordinary
 * integer render targets use the low 8 bits as UNORM8; W16Z16Y16X16 uses a
 * half float and X32/W32Z32Y32X32 use a 32-bit float. */
float rsx_fp_alpha_ref(u32 raw, u32 surface_color_format);

/* Scale texel-space (RSX_TEX_FMT_UNNORM) coordinates into HLSL's 0..1 range.
 * unnorm_mask selects the units; dim[u] gives that unit's {width, height};
 * cube units are skipped. Returns the number of Sample sites patched, or -1 if
 * the buffer would overflow. A no-op when unnorm_mask is 0. */
int rsx_fp_apply_unnorm_scale(char* hlsl, u32 hlsl_size, u32 unnorm_mask,
                              const u32 dim[][2], u32 cube_mask);
/* As above; units in nearest_mask (min or mag filter NEAREST) also get
 * RPCS3's subpixel bias, (coord + 0.01) / size. */
int rsx_fp_apply_unnorm_scale_ex(char* hlsl, u32 hlsl_size, u32 unnorm_mask,
                                 const u32 dim[][2], u32 cube_mask, u32 nearest_mask);
/* As _ex; units in vol_mask are Texture3D and scale all three coordinates,
 * the third by 1 / vol_depth[u]. */
int rsx_fp_apply_unnorm_scale_vol(char* hlsl, u32 hlsl_size, u32 unnorm_mask,
                                  const u32 dim[][2], u32 cube_mask, u32 nearest_mask,
                                  u32 vol_mask, const u32* vol_depth);

/* Decode NV4097_SET_ALPHA_REF for the active surface color format. Ordinary
 * integer render targets use the low 8 bits as UNORM8; W16Z16Y16X16 uses a
 * half float and X32/W32Z32Y32X32 use a 32-bit float. */
float rsx_fp_alpha_ref(u32 raw, u32 surface_color_format);

/* Same as rsx_fp_decompile, plus per-texture-unit dimensionality.
 *
 *   tex_cube_mask : bit N set => texture unit N is a CUBEMAP. Cube units are
 *                   declared `TextureCube` and sampled with the full
 *                   3-component direction (a.xyz); 2D units keep `Texture2D`
 *                   and a.xy sampling. Pass 0 for the legacy all-2D behavior
 *                   (rsx_fp_decompile forwards 0, so its output is unchanged
 *                   byte-for-byte).
 *
 * The mask must match what the backend actually binds: a cube unit needs a
 * TextureCube SRV at register tN, and a 2D unit a Texture2D SRV. See the
 * fragment lane report (scratch/a010_reports/fragment.md) for replay_main.c
 * call-site guidance. */
int rsx_fp_decompile_ex(const u8* ucode, u32 max_bytes, u32 ctrl,
                        u32 tex_cube_mask, char* out, u32 out_size);

/* Buffered-constant variant of rsx_fp_decompile_ex. Inline CONST payloads are
 * represented as fp_constants[N] in a b1 PSConstants cbuffer rather than
 * printed as HLSL literals. `out_constant_count` receives the exact slot
 * count and may be NULL. */
int rsx_fp_decompile_buffered_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    char* out, u32 out_size, u32* out_constant_count);

/* Native depth texture comparison metadata. Disabled descriptors preserve the
 * original shader byte-for-byte. RSX ADDRESS zfunc 0 / 7..15 and texture
 * alpha-kill bypass comparison; modes 1..6 compare the source reference
 * against stored depth. R32 color snapshots are sampled explicitly, so no
 * native depth texture/SampleCmp binding is needed.
 * As RPCS3 (RSXGraphEngine.cpp:1303-1345, GLSLCommon.cpp:596-616,
 * RSXFragmentTextureOps.glsl:100-222):
 *  - D24_D8 / D16 clamp the reference to [0,1]; DEPTH24_D8_FLOAT /
 *    DEPTH16_FLOAT (depth_float) compare it unclamped;
 *  - 2D units compare (xyz) / TXP (xyz / w) with implicit LOD;
 *  - cube units compare direction xyz against reference w (TEX3D_SHADOW);
 *    a cube TXP (RPCS3 "impossible") samples normally;
 *  - 1D and 3D units have no RPCS3 compare form and sample normally
 *    (rsx_fp_shadow_desc_from_texture leaves them disabled);
 *  - min filter LINEAR / *_LINEAR mips / CONVOLUTION and mag LINEAR /
 *    CONVOLUTION filter the compared taps.
 * A shadow unit never fails translation. RSX_FP_SHADOW_EXT=0 restores the
 * old 2D-integer-only contract (enabled=2 for anything else, translation
 * rejected). The shared engine and legacy Metal supply this state. */
typedef struct rsx_fp_shadow_desc {
    u8 enabled, func, linear, unnormalized;
    u8 depth_float;   /* DEPTH16_FLOAT / DEPTH24_D8_FLOAT: unclamped reference */
    u8 pad[3];
} rsx_fp_shadow_desc;
void rsx_fp_shadow_desc_from_texture(rsx_fp_shadow_desc* out, u32 format,
    u32 dimension, u32 cube, u32 mips, u32 filter, u32 address, u32 control0);
u32 rsx_fp_texture_units(const u8* ucode, u32 max_bytes);
int rsx_fp_decompile_buffered_shadow_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    const rsx_fp_shadow_desc shadow[16],
    char* out, u32 out_size, u32* out_constant_count);

/* Extract the exact runtime inline-CONST payload. Returns the slot count on
 * success or -1 for an unterminated/truncated/oversized program. */
int rsx_fp_collect_constants(
    const u8* ucode, u32 max_bytes, rsx_fp_constant_block* out);

/* Canonical structural fragment-program hash. Instruction bytes and their
 * order are hashed; inline CONST payload bytes are skipped. Returns zero for
 * malformed input. */
u64 rsx_fp_structural_hash(const u8* ucode, u32 max_bytes, u64 seed);

/* Hash the exact legacy HLSL literal spellings (%g) for a program while also
 * retaining structural identity. This is intended for aggregate diagnostics;
 * PSO identity should use rsx_fp_structural_hash in buffered mode. */
u64 rsx_fp_literal_source_hash(
    const u8* ucode, u32 max_bytes, u64 seed);

/* Buffered-alpha counterpart to rsx_fp_apply_alpha_test. It emits the compare
 * against fp_alpha.x from PSConstants instead of embedding a numeric value. */
int rsx_fp_apply_alpha_test_buffered(
    char* hlsl, u32 out_size, u32 func);

/* Plan one 256-byte-aligned allocation in a fence-retired PS constant ring.
 * Returns 1 when it fits, 0 when the caller must submit/wait/reset first, and
 * -1 when one allocation can never fit. */
int rsx_fp_constant_ring_plan(
    u32 used, u32 capacity, u32 data_bytes,
    u32* out_offset, u32* out_allocation_bytes);

/* Plan one already-aligned fixed-size vertex-constant block.  The caller must
 * fence and retry from offset zero when this returns 0; silently dropping the
 * draw would leave a multi-buffered display surface on an older generation. */
int rsx_vertex_constant_ring_plan(
    u32 used, u32 capacity, u32 block_bytes, u32* out_offset);

/* Integer framebuffer sRGB conversion (NV4097_SET_SHADER_PACKER / 0x1FEC).
 * Pass the decoded surface color format. Any nonzero packer enables it for
 * half exports; floating surfaces and 32-bit exports bypass conversion. */
int rsx_fp_srgb_output_enabled(u32 packer, u32 ctrl, u32 color_format);

/* Apply RGB-only linear-to-sRGB conversion to all emitted color targets,
 * before output quantization. Alpha and optional depth exports are preserved.
 * Returns 1, or -1 without mutation for malformed source/insufficient space.
 * Apply before alpha-test wrapping; disabled draws should skip this helper. */
int rsx_fp_apply_srgb_output(char* hlsl, u32 capacity);

/* Gamma metadata is a post-filter/post-remap RGBA lane mask, with formats
 * validated by rsx_texture_gamma_mask. EXP_TEX overrides gamma per fetch.
 * Old entry points pass NULL and preserve their source byte for byte. */
u32 rsx_fp_texture_gamma_units(const u8* ucode, u32 max_bytes,
                                const u8 gamma_masks[16]);
int rsx_fp_decompile_buffered_color_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    const u8 gamma_masks[16], char* out, u32 out_size, u32* out_constant_count);
/* Both: depth comparison per unit and post-filter gamma per unit. */
/* Texture sign expansion (SET_TEXTURE_ADDRESS UNSIGNED_REMAP_BIASED):
 * expand_masks[u] is a post-remap RGBA lane mask of the lanes unit u
 * re-centres, plus bit 4 for 16-bit channels (rsx_texture_expand_mask).
 * Every TEX/TXP/TXB/TXL from such a unit returns (round(x*255)-128)/127 on
 * those lanes, less gamma lanes. A fetch carrying EXP_TEX (OPDEST bit 21,
 * the _bx2 modifier) returns x*2-1 on every lane on any unit and skips gamma
 * (RPCS3 _process_texel), from every entry point. The units whose fetches
 * expand are part of the pipeline's identity. */
u32 rsx_fp_texture_expand_units(const u8* ucode, u32 max_bytes,
                                 const u8 expand_masks[16]);
u32 rsx_fp_texture_bx2_units(const u8* ucode, u32 max_bytes);
/* Unbuffered form with per-unit sign expansion (D3D12 backend). */
int rsx_fp_decompile_expand_ex(const u8* ucode, u32 max_bytes, u32 ctrl,
                               u32 tex_cube_mask, const u8 expand_masks[16],
                               char* out, u32 out_size);
int rsx_fp_decompile_shadow_expand_ex(const u8* ucode, u32 max_bytes, u32 ctrl,
                                      u32 tex_cube_mask, const rsx_fp_shadow_desc shadow[16],
                                      const u8 expand_masks[16], char* out, u32 out_size);
int rsx_fp_decompile_buffered_shadow_color_expand_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    const rsx_fp_shadow_desc shadow[16], const u8 gamma_masks[16],
    const u8 expand_masks[16],
    char* out, u32 out_size, u32* out_constant_count);
/* As above, with tex_vol_mask naming the units bound to a 3D texture: they
 * are declared Texture3D and sampled with three coordinates (TXP divides
 * all three by w), like cube units on their own t-register. */
int rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask, u32 tex_vol_mask,
    const rsx_fp_shadow_desc shadow[16], const u8 gamma_masks[16],
    const u8 expand_masks[16],
    char* out, u32 out_size, u32* out_constant_count);
int rsx_fp_decompile_buffered_shadow_color_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    const rsx_fp_shadow_desc shadow[16], const u8 gamma_masks[16],
    char* out, u32 out_size, u32* out_constant_count);

/* Fixed-function fragment state RPCS3 emulates in the shader (RSXGraphEngine
 * get_current_fragment_program, RSXFragmentPrologue/ROPPrologue/ROPEpilogue,
 * _process_texel). Buffered form only: the dynamic parts live in fp_alpha,
 * which the caller fills per draw as
 *   x = alpha ref, y = FOG_PARAMS[0], z = FOG_PARAMS[1],
 *   w = WPOS y bias (origin bottom ? SHADER_WINDOW height : 0, less 0.5 for
 *       an integer pixel centre).
 * Zeroed fields leave the program as the plain buffered entry point emits it. */
typedef struct rsx_fp_frag_state {
    u32 fog_mode;            /* NV4097_SET_FOG_MODE, used when fog is set */
    u32 stipple_pattern[32]; /* NV4097_SET_POLYGON_STIPPLE_PATTERN rows */
    u16 texcoord_2d_mask;    /* NV4097_SET_TEX_COORD_CONTROL bit 0, per TC0..9 */
    u16 alpha_kill_mask;     /* TEXTURE_CONTROL0 bit 2, per unit */
    u8  sext_masks[16];      /* rsx_fp_texture_sext_mask per unit */
    u8  fog;                 /* FOGC reads go through RPCS3 fetch_fog_value */
    u8  wpos;                /* WPOS emulated from SHADER_WINDOW */
    u8  wpos_origin_bottom;
    u8  wpos_center_integer;
    u8  two_sided;           /* TWO_SIDE_LIGHT_EN: back colours on back faces */
    u8  stipple;             /* polygon stipple test (polygon primitives) */
    u8  mrt_count;           /* surface colour targets; each one is written */
    u8  rop_round;           /* 8-bit framebuffer output quantisation */
    u8  alpha_to_coverage;   /* stochastic A2C emulation */
    u8  alpha_to_one;
    /* RPCS3 RSX_SHADER_CONTROL_ROP_OUTPUT_REMAP (RSXROPEpilogue.glsl
     * remap_ROP_output, after the alpha test): 0 RGBA, 1 BBBB (B8),
     * 2 GBGB as .bgbg (G8B8), 3 RGB0 (X..Z formats), 4 RGB1 (X..O). */
    u8  rop_remap;
    /* RPCS3 emulate_depth_compare (RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE):
     * a depth-EQUAL polygon draw reads the bound depth (a snapshot bound at
     * texture unit depth_compare_unit) and exports it when within one LSB
     * of the fragment's depth (24-bit or 16-bit zeta). */
    u8  depth_compare;
    u8  depth_compare_unit;
    u8  depth_compare_24;
    /* RPCS3 RSX_SHADER_CONTROL_FLAT_SHADING (SHADE_MODE FLAT, only with a
     * last-vertex provoking convention): COL0/COL1 and the back colours are
     * declared `nointerpolation` (GLSLCommon.cpp:738). */
    u8  flat_shade;
    /* TEX_COORD_CONTROL bit 4 per TC0..9, only for AA polygon draws: the
     * input is declared `centroid`. */
    u16 centroid_mask;
    /* NV4097_SET_POINT_SPRITE_CONTROL on a point draw (RPCS3
     * texcoord_is_point_coord): bit N replaces TCN by the sprite coordinate,
     * (PointCoord, r, q) or, under texcoord_2d_mask, (PointCoord, 0, w).
     * RMODE is ignored, as RPCS3 does. Needs a glslang target: the coordinate input
     * is RSX_SPIRV_POINTCOORD_LOCATION, rewritten to BuiltIn PointCoord. */
    u16 point_sprite_mask;
    u8  point_sprite_rmode;  /* unused: RPCS3 ignores RMODE */
    /* RPCS3 VKFragmentProgram.cpp:407 low_precision_tests (NVIDIA without
     * RSX_SHADER_CONTROL_ATTRIBUTE_INTERPOLATION, i.e. point and line draws
     * on a barycentric-capable NVIDIA device, RSXGraphEngine.cpp:1159-1165):
     * SEQ..SNE and the alpha test compare CMP_FIXUP'd operands. */
    u8  low_precision_tests;
    /* RPCS3 enables _GPU_LOW_PRECISION_COMPARE (VKFragmentProgram.cpp:407)
     * and has_low_precision_rounding (VKFragmentProgram.cpp:638, the FLR
     * floor(x + 0.000001) bias) only on NVIDIA hosts. 1 = the host GPU is
     * not NVIDIA (Vulkan vendorID != 0x10DE): neither is emitted. 0 (the
     * default) keeps both on, as before. */
    u8  host_not_nvidia;
    u8  pad1;
    /* EXP_TEX (_bx2) gating per unit, RPCS3 _enable_texture_expand: 0 =
     * every lane; 0x10 | lanes = biased-renormalisation format, expand the
     * RGBA lanes the remap takes from the texture; 0x20 = format without
     * BIASED_RENORMALIZATION, _bx2 ignored. */
    u8  bx2_lanes[16];
    /* RPCS3 RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING (RSXProgrammableBlend
     * Prologue.glsl do_blend, applied after the alpha test and before the
     * output remap, RSXROPEpilogue.glsl:52-58): colour target 0 blends in
     * the shader against the destination read from a copy of the target
     * bound at texture unit pb_unit, loaded at the fragment's pixel. pb_eq /
     * pb_sf / pb_df are the BLEND_EQUATION / SFACTOR / DFACTOR halves
     * ([0] RGB, [1] alpha), pb_const the CONSTANT_* colour, pb_int an
     * integer framebuffer (INT_FRAMEBUFFER_BIT: saturation and rounding). */
    u8  pb;
    u8  pb_unit;
    u8  pb_int;
    u8  pad2;
    u16 pb_eq[2];
    u16 pb_sf[2];
    u16 pb_df[2];
    float pb_const[4];
} rsx_fp_frag_state;

/* Input registers (bit = OPDEST.INPUT_SRC) the program's sources read. */
u32 rsx_fp_input_mask(const u8* ucode, u32 max_bytes);
/* ARGB8_SIGNED (FILTER [31:28]) as a post-remap RGBA lane mask, bit 4 for
 * 16-bit channels; 0 for formats without RSX_FORMAT_FEATURE_SIGNED_COMPONENTS. */
u8 rsx_fp_texture_sext_mask(u32 format, u32 control1, u32 filter);
int rsx_fp_decompile_engine(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask, u32 tex_vol_mask,
    const rsx_fp_shadow_desc shadow[16], const u8 gamma_masks[16],
    const u8 expand_masks[16], const rsx_fp_frag_state* fs,
    char* out, u32 out_size, u32* out_constant_count);
/* ROP stages around the alpha test, in RPCS3's order: stage 0 (8-bit output
 * rounding) after sRGB output and before rsx_fp_apply_alpha_test_buffered,
 * stage 1 (alpha-to-coverage then alpha-to-one) after it. Returns 1 when
 * applied, 0 when the state needs nothing, -1 without mutation on failure. */
int rsx_fp_apply_rop_stage(char* hlsl, u32 capacity, const rsx_fp_frag_state* fs,
                           int stage);
/* Add back colours (VP o3/o4) to a rsx_vp_decompile_* VSOutput, for a
 * two-sided fragment program from rsx_fp_decompile_engine. */
int rsx_fp_patch_vs_back_colors(char* vs_hlsl, u32 capacity);

/* Return the mnemonic for an NV40 fragment opcode (or "?" if unknown).
 * Useful for disassembly/logging. */
const char* rsx_fp_opcode_name(u32 opcode);

/* Total byte length of the fragment program at `ucode` (including inline
 * CONST slots), up to and including the PROGRAM_END instruction, bounded by
 * `max_bytes`. Returns 0 if no terminator is found within the bound. Shared
 * walk logic so callers (e.g. a shader cache hashing the bytecode) agree with
 * the decompiler on where a program ends. */
u32 rsx_fp_program_size(const u8* ucode, u32 max_bytes);

#ifdef __cplusplus
}
#endif

/* Kept from this tree (see the .c) -- used by rsx_d3d12_backend.c. */
int rsx_fp_extract_consts(const u8* ucode, u32 max_bytes, float* out, int max_out);
u32 rsx_fp_code_hash(const u8* ucode, u32 max_bytes);
#endif /* PS3RECOMP_RSX_FP_DECOMPILER_H */
