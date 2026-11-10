/*
 * ps3recomp - RSX Vertex Program (NV40 ISA) → HLSL decompiler
 *
 * Translates an RSX vertex shader (NVIDIA NV40 "nvfx" vertex program, a VLIW
 * that co-issues one vector ALU op and one scalar ALU op per instruction) into
 * an HLSL vertex shader.
 *
 * Encoding reference: RPCS3 RSXVertexProgram.h (D0..D3 bitfields) + Mesa
 * nouveau nvfx_shader.h opcode tables. Words are stored little-endian (RPCS3
 * native order), 4 words / 16 bytes per instruction, no inline constants
 * (VP constants live in a separate bank), program ends at the D3.end bit.
 *
 * Self-contained: no D3D12 dependency, build/test standalone.
 */
#ifndef PS3RECOMP_RSX_VP_DECOMPILER_H
#define PS3RECOMP_RSX_VP_DECOMPILER_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Number of instructions in the program at `ucode` (each 16 bytes), up to and
 * including the one with the D3.end bit, bounded by max_bytes. 0 if none. */
u32 rsx_vp_program_size_instrs(const u8* ucode, u32 max_bytes);

typedef struct rsx_vp_input_analysis {
    u32 input_mask;
    int exact;
} rsx_vp_input_analysis;

/* Report the guest input registers statically read by the program.  `exact`
 * is zero and input_mask is 0xFFFF when an unsupported opcode/control-flow
 * construct makes the result unsafe to narrow.  Returns the instruction
 * count, or -1 for invalid/non-terminated input. */
int rsx_vp_analyze_inputs(const u8* ucode, u32 max_bytes,
                          rsx_vp_input_analysis* analysis);

/* Decompile an NV40 vertex program into an HLSL vertex shader.
 *   ucode    : VP bytecode (little-endian words, as in RPCS3's shader cache).
 *   max_bytes: safety bound.
 *   out      : caller buffer for generated HLSL (NUL-terminated).
 *   out_size : size of out in bytes.
 * Returns instruction count (>=0), or -1 on error (null args / overflow).
 * Emits `VSOutput main(VSInput input)`: 16 float4 inputs (ATTR0..15), a
 * `cbuffer VPConst : register(b0)` with 512 vec4 constants + vp_posscale/
 * vp_posoffset (the RSX viewport transform mapped to D3D clip space; the
 * caller computes them per draw), SV_Position + COLOR0/1 + FOG + TEXCOORD0..7
 * varyings routed per the NV40 output register map (o0/o1/o2/o5/o7..o14).
 * Condition codes and flow control follow RPCS3's VertexProgramDecompiler. TXL
 * is stubbed to zero unless rsx_vp_decompile_ex receives a bound-unit mask. */
int rsx_vp_decompile(const u8* ucode, u32 max_bytes, char* out, u32 out_size);

/* The VP slot the next program decompiled starts at (NV4097 transform
 * program start). Branch targets are absolute slots; this makes them relative
 * to the ucode pointer handed in. Defaults to 0. */
void rsx_vp_set_branch_base(u32 start_slot);

/* Emit SV_ClipDistance0..5 for the user clip planes (RPCS3 reg_table UC0-5).
 * Off by default: a Vulkan backend turns it on only once the device has
 * shaderClipDistance enabled. */
void rsx_vp_set_clip_distances(int enabled);
/* Vertex texture units (bits 0-3) whose dimension is 1: TXL reads them at
 * float2(coord.x, 0.5) (RPCS3 VERTEX_TEXTURE_FETCH1D). Default 0. */
void rsx_vp_set_vtex_1d_mask(u32 mask);
/* Vertex texture units whose dimension is 3 / cubemap: declared Texture3D /
 * TextureCube and sampled at coord.xyz (RPCS3 VERTEX_TEXTURE_FETCH3D/CUBE).
 * Default 0 (every unit a Texture2D). The 1D mask wins over both. */
void rsx_vp_set_vtex_3d_mask(u32 mask);
void rsx_vp_set_vtex_cube_mask(u32 mask);
/* RPCS3 VKVertexProgram.cpp:367: CMP_FIXUP around SLT..SNE only on NVIDIA
 * hosts. Default 1 (always on); pass vendorID == 0x10DE. RSX_VP_CMP_FIXUP=0
 * still disables it everywhere. */
void rsx_vp_set_low_precision_compare(int enabled);
/* CELL_GCM_TEXTURE_UN vertex units (bits 0-3; RPCS3 VKDraw.cpp:593-619
 * unnormalizedCoordinates). 1D/2D units in the mask sample at coord / size:
 * dims[u] = {width, height} of the guest texture baked into the shader, or
 * {0,0} / dims NULL to use the bound image's GetDimensions. 3D/cube units
 * ignore it. Default 0. RSX_VP_VTEX_UN=0 disables. */
void rsx_vp_set_vtex_unnorm(u32 mask, const u32 dims[4][2]);

/* vp_branch, the uint4 after vp_posoffset:
 *   x = TRANSFORM_BRANCH_BITS (0x1FF8)
 *   y = VERTEX_ATTRIB_OUTPUT_MASK (0x1FF4)
 *   z = USER_CLIP_PLANE_CONTROL (0x1478) | RSX_VP_BRANCH_ENV_VALID
 *   w = POINT_SIZE (0x1EE0) float bits
 * Without RSX_VP_BRANCH_ENV_VALID in z (callers that fill only x), every
 * output stays enabled, no plane clips and the point size is 1. */
#define RSX_VP_BRANCH_ENV_VALID 0x80000000u
/* vp_posscale.w (otherwise unused): a multiplier for the point size the
 * program writes itself, the backend's resolution scale; 0 or less means 1. */

/* Bit N in vtex_mask declares NV40 2D vertex-texture unit N at t(16+N),
 * sampler sN, and turns TXL for that unit into SampleLevel(..., LOD 0).
 * Unmasked units retain the defined-zero fallback. */
int rsx_vp_decompile_ex(const u8* ucode, u32 max_bytes, u32 vtex_mask,
                        char* out, u32 out_size);

/* Compact-input variant.  The generated VSInput declares only the original
 * ATTRn semantics selected by input_mask.  The mask is checked against a
 * fresh static analysis; any uncertainty or missing referenced bit falls
 * back to all 16 inputs. */
int rsx_vp_decompile_compact_ex(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, u32 input_mask,
    char* out, u32 out_size);

/* RPCS3 vertex_program_utils::analyse_vertex_program. Walks the program from
 * `entry` through transform program memory (vp_mem, total_slots 16-byte
 * slots, at most 544): static BRI and CAL/CLI/CLB/RET are followed,
 * conditional BRI/BRB targets extend the range and are walked afterwards, and
 * an end bit stops the walk only once no branch reaches past it. The result
 * is the touched slot range [base, base + count), the entry relative to base
 * and a live-instruction mask (bit i = slot base + i). Returns count or -1. */
#define RSX_VP_MASK_WORDS 17
typedef struct rsx_vp_program_info {
    u32 base;
    u32 count;
    u32 entry;
    int has_branch;
    u32 mask[RSX_VP_MASK_WORDS];
} rsx_vp_program_info;
int rsx_vp_analyse_program(const u8* vp_mem, u32 total_slots, u32 entry,
                           rsx_vp_program_info* info);

/* Analyse (above) and decompile the program that starts at entry_slot, with
 * branch targets relative to the analysed base and dead slots skipped; a
 * program end does not stop decoding, it returns. compact_inputs as for
 * rsx_vp_decompile_compact_ex (branching programs keep all 16 inputs). */
int rsx_vp_decompile_program(const u8* vp_mem, u32 total_slots, u32 entry_slot,
                             u32 vtex_mask, u32 input_mask, int compact_inputs,
                             char* out, u32 out_size);

/* Mnemonics for the vector / scalar opcode fields ("?" if unknown). */
const char* rsx_vp_vec_name(u32 op);
const char* rsx_vp_sca_name(u32 op);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_VP_DECOMPILER_H */
