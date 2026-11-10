/*
 * ps3recomp - CPU evaluation of an RSX transform program's position output.
 * See rsx_vp_eval.h. Field layout as in rsx_vp_decompiler.c (NV40, RPCS3's
 * D0..D3 / SRC unions as the oracle).
 */
#include "rsx_vp_eval.h"

#include <math.h>
#include <string.h>

static float vpe_f(u32 bits)
{
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static float vpe_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

int rsx_vp_eval_position(const u32* words, u32 n_slots, u32 start_slot,
                         const u32 (*constants)[4], u32 branch_bits,
                         const float in0[4], float out[4])
{
    static const float zero[4] = { 0, 0, 0, 0 };
    float r[64][4];
    float o[16][4];
    float cst[4];
    int a[2][4];
    u32 stack[8];
    int sp = 0, done = 0;
    memset(r, 0, sizeof r);
    memset(o, 0, sizeof o);
    memset(a, 0, sizeof a);
    for (int i = 0; i < 16; i++) o[i][3] = 1.0f;
    if (!words || !constants || !in0 || !out) return 0;

    u32 pc = start_slot;
    for (int steps = 0; steps < 2048 && !done; steps++) {
        if (pc >= n_slots) return 0;
        const u32 d0 = words[pc * 4 + 0], d1 = words[pc * 4 + 1];
        const u32 d2 = words[pc * 4 + 2], d3 = words[pc * 4 + 3];
        const u32 src[3] = {
            ((d2 >> 23) & 0x1FFu) | ((d1 & 0xFFu) << 9),
            (d2 >> 6) & 0x1FFFFu,
            ((d3 >> 21) & 0x7FFu) | ((d2 & 0x3Fu) << 11),
        };
        if (!(src[0] & 3u) || !(src[1] & 3u) || !(src[2] & 3u)) break;   /* end, as RPCS3 */

        const u32 vop = (d1 >> 22) & 0x1Fu, sop = (d1 >> 27) & 0x1Fu;
        const u32 cond = (d0 >> 10) & 7u;
        if (vop == 0x19) return 0;                            /* TXL */
        if (((d0 >> 13) & 1u) && cond != 7u) return 0;        /* cond-code test */

        float s[3][4];
        for (int n = 0; n < 3; n++) {
            const u32 sv = src[n];
            const float* p = zero;
            switch (sv & 3u) {
            case 1: p = r[(sv >> 2) & 0x3Fu]; break;
            case 2: p = ((d1 >> 8) & 0xFu) == 0 ? in0 : zero; break;
            case 3: {
                const u32 cs = (d1 >> 12) & 0x3FFu;
                int idx;
                if (!((d3 >> 1) & 1u)) idx = cs < 468u ? (int)cs : -1;
                else idx = (a[(d0 >> 24) & 1u][d0 & 3u] + (int)cs) & 511;
                if (idx >= 0 && idx < RSX_DSP_NUM_CONSTANTS) {
                    for (int i = 0; i < 4; i++) cst[i] = vpe_f(constants[idx][i]);
                    p = cst;
                }
                break;
            }
            default: break;
            }
            const u32 sw[4] = { (sv >> 14) & 3u, (sv >> 12) & 3u, (sv >> 10) & 3u, (sv >> 8) & 3u };
            const int ab = (int)((d0 >> (21 + n)) & 1u), ng = (int)((sv >> 16) & 1u);
            for (int i = 0; i < 4; i++) {
                float x = p[sw[i]];
                if (ab) x = fabsf(x);
                if (ng) x = -x;
                s[n][i] = x;
            }
        }

        float vr[4] = { 0, 0, 0, 0 };
        const int vwrite = vop != 0;
        float d;
        switch (vop) {
        case 0x00: break;
        case 0x01: case 0x0D: for (int i = 0; i < 4; i++) vr[i] = s[0][i]; break;   /* MOV, ARL */
        case 0x02: for (int i = 0; i < 4; i++) vr[i] = s[0][i] * s[1][i]; break;
        case 0x03: for (int i = 0; i < 4; i++) vr[i] = s[0][i] + s[2][i]; break;
        case 0x04: for (int i = 0; i < 4; i++) vr[i] = s[0][i] * s[1][i] + s[2][i]; break;
        case 0x05: d = s[0][0] * s[1][0] + s[0][1] * s[1][1] + s[0][2] * s[1][2];
                   for (int i = 0; i < 4; i++) vr[i] = d; break;
        case 0x06: d = s[0][0] * s[1][0] + s[0][1] * s[1][1] + s[0][2] * s[1][2] + s[1][3];
                   for (int i = 0; i < 4; i++) vr[i] = d; break;
        case 0x07: d = s[0][0] * s[1][0] + s[0][1] * s[1][1] + s[0][2] * s[1][2] + s[0][3] * s[1][3];
                   for (int i = 0; i < 4; i++) vr[i] = d; break;
        case 0x08: vr[0] = 1.0f; vr[1] = s[0][1] * s[1][1]; vr[2] = s[0][2]; vr[3] = s[1][3]; break;
        case 0x09: for (int i = 0; i < 4; i++) vr[i] = fminf(s[0][i], s[1][i]); break;
        case 0x0A: for (int i = 0; i < 4; i++) vr[i] = fmaxf(s[0][i], s[1][i]); break;
        case 0x0B: for (int i = 0; i < 4; i++) vr[i] = s[0][i] <  s[1][i] ? 1.0f : 0.0f; break;
        case 0x0C: for (int i = 0; i < 4; i++) vr[i] = s[0][i] >= s[1][i] ? 1.0f : 0.0f; break;
        case 0x0E: for (int i = 0; i < 4; i++) vr[i] = s[0][i] - floorf(s[0][i]); break;
        case 0x0F: for (int i = 0; i < 4; i++) vr[i] = floorf(s[0][i]); break;
        case 0x10: for (int i = 0; i < 4; i++) vr[i] = s[0][i] == s[1][i] ? 1.0f : 0.0f; break;
        case 0x11: break;                                                          /* SFL */
        case 0x12: for (int i = 0; i < 4; i++) vr[i] = s[0][i] >  s[1][i] ? 1.0f : 0.0f; break;
        case 0x13: for (int i = 0; i < 4; i++) vr[i] = s[0][i] <= s[1][i] ? 1.0f : 0.0f; break;
        case 0x14: for (int i = 0; i < 4; i++) vr[i] = s[0][i] != s[1][i] ? 1.0f : 0.0f; break;
        case 0x15: for (int i = 0; i < 4; i++) vr[i] = 1.0f; break;                /* STR */
        case 0x16: for (int i = 0; i < 4; i++)
                       vr[i] = s[0][i] > 0 ? 1.0f : s[0][i] < 0 ? -1.0f : 0.0f; break;
        default: return 0;
        }

        float sr[4] = { 0, 0, 0, 0 };
        int swrite = 1;
        const float* s2 = s[2];
        switch (sop) {
        case 0x01: for (int i = 0; i < 4; i++) sr[i] = s2[i]; break;
        case 0x02: for (int i = 0; i < 4; i++) sr[i] = 1.0f / s2[i]; break;
        case 0x03: for (int i = 0; i < 4; i++)
                       sr[i] = fminf(fmaxf(1.0f / s2[i], 5.42101e-20f), 1.884467e19f); break;
        case 0x04: d = 1.0f / sqrtf(fmaxf(fabsf(s2[0]), 1e-38f));
                   for (int i = 0; i < 4; i++) sr[i] = d; break;
        case 0x05: for (int i = 0; i < 4; i++) sr[i] = expf(s2[i]); break;
        case 0x06: for (int i = 0; i < 4; i++) sr[i] = logf(s2[i]); break;
        case 0x07: {
            const float py = vpe_clampf(s2[3], -127.9961f, 127.9961f);
            sr[0] = 1.0f; sr[1] = fmaxf(s2[0], 0.0f);
            sr[2] = s2[0] > 0.0f ? exp2f(py * log2f(fmaxf(s2[1], 1e-38f))) : 0.0f;
            sr[3] = 1.0f;
            break;
        }
        case 0x0D: for (int i = 0; i < 4; i++) sr[i] = log2f(fmaxf(fabsf(s2[i]), 1e-38f)); break;
        case 0x0E: for (int i = 0; i < 4; i++) sr[i] = exp2f(s2[i]); break;
        case 0x0F: for (int i = 0; i < 4; i++) sr[i] = sinf(s2[i]); break;
        case 0x10: for (int i = 0; i < 4; i++) sr[i] = cosf(s2[i]); break;
        default: swrite = 0; break;
        }

        if (cond != 0) {
            const int sat = (int)((d0 >> 26) & 1u), vres = (int)((d0 >> 30) & 1u);
            const u32 dst = (d3 >> 2) & 0x1Fu;
            if (vwrite) {
                const int m[4] = { (int)((d3 >> 16) & 1u), (int)((d3 >> 15) & 1u),
                                   (int)((d3 >> 14) & 1u), (int)((d3 >> 13) & 1u) };
                const u32 dt = (d0 >> 15) & 0x3Fu;
                for (int i = 0; i < 4; i++) {
                    if (!m[i]) continue;
                    if (vop == 0x0D) { a[dt & 1u][i] = (int)vr[i]; continue; }
                    const float v = sat ? vpe_clampf(vr[i], 0.0f, 1.0f) : vr[i];
                    if (vres && dst != 0x1Fu && dst < 16u) o[dst][i] = v;
                    if (dt != 0x3Fu) r[dt][i] = v;
                }
            }
            if (swrite) {
                const int m[4] = { (int)((d3 >> 20) & 1u), (int)((d3 >> 19) & 1u),
                                   (int)((d3 >> 18) & 1u), (int)((d3 >> 17) & 1u) };
                const u32 st = (d3 >> 7) & 0x3Fu;
                for (int i = 0; i < 4; i++) {
                    if (!m[i]) continue;
                    const float v = sat ? vpe_clampf(sr[i], 0.0f, 1.0f) : sr[i];
                    if (!vres && dst != 0x1Fu && dst < 16u) o[dst][i] = v;
                    if (st != 0x3Fu) r[st][i] = v;
                }
            }
        }

        const u32 target = (((d0 >> 23) & 1u) << 9) | ((d2 & 0x3Fu) << 3) | ((d3 >> 29) & 7u);
        const u32 bit = (branch_bits >> ((d3 >> 23) & 0x1Fu)) & 1u;
        const int bcond = ((d3 >> 28) & 1u) ? (int)bit : !bit;
        switch (sop) {
        case 0x11:                                                   /* BRB */
            if (bcond) { pc = target; continue; }
            break;
        case 0x12:                                                   /* CLB */
            if (bcond) {
                if (sp >= 8) return 0;
                stack[sp++] = pc + 1; pc = target; continue;
            }
            break;
        case 0x0A:                                                   /* CAL */
            if (sp >= 8) return 0;
            stack[sp++] = pc + 1; pc = target; continue;
        case 0x0C:                                                   /* RET */
            if (!sp) { done = 1; break; }
            pc = stack[--sp]; continue;
        case 0x08: case 0x09: case 0x0B: case 0x13: case 0x14:       /* BRA BRI CLI PSH POP */
            return 0;
        default: break;
        }
        if (done || (d3 & 1u)) break;
        pc++;
    }

    for (int i = 0; i < 4; i++) {
        out[i] = o[0][i];
        if (!isfinite(out[i])) return 0;
    }
    return 1;
}

int rsx_vp_extract_affine(const rsx_dispatch* rsx, double m[4][4])
{
    static const float pts[5][4] = {
        { 0, 0, 0, 1 }, { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 0.37f, -1.3f, 2.1f, 1 } };
    if (!rsx || !m) return 0;
    const u32 start = rsx_dsp_vp_start(rsx);
    if (start >= RSX_DSP_VP_INSTR) return 0;
    const u32 bits = rsx_dsp_reg(rsx, 0x1FF8);
    float p[5][4];
    for (int i = 0; i < 5; i++)
        if (!rsx_vp_eval_position(rsx->vp, RSX_DSP_VP_INSTR, start,
                                  (const u32 (*)[4])rsx->constants, bits, pts[i], p[i]))
            return 0;
    for (int r = 0; r < 4; r++) {
        m[r][3] = p[0][r];
        for (int c = 0; c < 3; c++) m[r][c] = (double)p[c + 1][r] - p[0][r];
    }
    double mx = 0;
    for (int r = 0; r < 4; r++) mx = fmax(mx, fabs(p[4][r]));
    for (int r = 0; r < 4; r++) {
        double pr = m[r][3];
        for (int c = 0; c < 3; c++) pr += m[r][c] * pts[4][c];
        if (fabs(pr - p[4][r]) > 1e-3 * (1.0 + mx)) return 0;
    }
    return 1;
}
