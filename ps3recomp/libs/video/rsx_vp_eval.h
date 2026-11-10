/*
 * ps3recomp - CPU evaluation of an RSX transform (vertex) program's position
 *
 * Enough of the NV40 vertex ISA to run a program on one vertex on the CPU and
 * read its position output: the vector and scalar ALUs, address registers,
 * static (branch-bit) branches and calls. Texture fetches, condition-code
 * tests and dynamic branches are not modelled; such a program reports
 * failure rather than a wrong answer.
 *
 * A temporal upscaler uses it to recover each draw's object -> clip matrix,
 * from which camera motion between frames follows (RPCS3 VKR2Upscaler's
 * vp_run / vp_extract_matrix, decoded here with the field layout of
 * rsx_vp_decompiler.c).
 */
#ifndef PS3RECOMP_RSX_VP_EVAL_H
#define PS3RECOMP_RSX_VP_EVAL_H

#include "rsx_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Run the program at absolute slot `start_slot` (words = the transform
 * program from slot 0, `n_slots` slots) on a vertex whose input 0 is `in0`;
 * every other input reads 0. Writes output register 0 to `out`. Returns 1 on
 * success, 0 when the program does something not modelled or the result is
 * not finite. */
int rsx_vp_eval_position(const u32* words, u32 n_slots, u32 start_slot,
                         const u32 (*constants)[4], u32 branch_bits,
                         const float in0[4], float out[4]);

/* The current program's position output as an affine function of input 0:
 * clip = m * (x, y, z, 1), row-major (m[r][3] is the translation). Checked
 * against a fifth point; returns 0 when the program is not affine in its
 * position input or cannot be evaluated. */
int rsx_vp_extract_affine(const rsx_dispatch* rsx, double m[4][4]);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_VP_EVAL_H */
