/* RSX FIFO method/register parity with RPCS3 in the dispatch register file
 * (rsx_dispatch.c; sweep_methods D4-D13): init seeds, invalid-enum
 * rollback, invalid BEGIN, the recover_fifo discard, DATA2F/3F z/w reset,
 * the immediate-push input mask, transform-program bounds and growable
 * inline/element streams.
 *
 * clang-cl /std:c17 -I include -I libs/video -I runtime
 *   libs/video/tests/test_rsx_dispatch_methods.c libs/video/rsx_dispatch.c
 */
#include "rsx_dispatch.h"
#include "rsx_primitives.h"   /* RSX_PRIMITIVE_* */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) { printf("[PASS] "); printf(__VA_ARGS__); printf("\n"); }   \
        else { printf("[FAIL] "); printf(__VA_ARGS__); printf("\n");          \
               g_failures++; }                                                \
    } while (0)

#define M_VTX_ATTR_4F 0x1C00
static u32 f32_bits(float f) { u32 u; memcpy(&u, &f, 4); return u; }

typedef struct {
    u32 n_begin, n_end, n_ranges;
    u32 first[8], count[8];
} ClauseLog;
static ClauseLog g_cl;
static void cl_begin(void* u, const rsx_dispatch* r, u32 p) { (void)u; (void)r; (void)p; g_cl.n_begin++; }
static void cl_end(void* u, const rsx_dispatch* r) { (void)u; (void)r; g_cl.n_end++; }
static void cl_range(void* u, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)u; (void)r;
    if (g_cl.n_ranges < 8) { g_cl.first[g_cl.n_ranges] = first; g_cl.count[g_cl.n_ranges] = count; }
    g_cl.n_ranges++;
}
static u32 g_dp_inline_bytes, g_dp_elems, g_dp_last_elem, g_dp_imm;
static void dp_inline(void* u, const rsx_dispatch* r, const u8* p, u32 n)
{ (void)u; (void)r; g_dp_inline_bytes = n; (void)p; }
static void dp_elems(void* u, const rsx_dispatch* r, const u32* idx, u32 n)
{ (void)u; (void)r; g_dp_elems = n; g_dp_last_elem = n ? idx[n - 1] : 0; }
static void dp_imm(void* u, const rsx_dispatch* r) { (void)u; (void)r; g_dp_imm++; }

static void test_dispatch_methods_parity(void)
{
    printf("-- dispatch method parity (RPCS3)\n");
    static rsx_dispatch d;
    rsx_dispatch_sink sink;
    memset(&sink, 0, sizeof sink);
    sink.begin = cl_begin; sink.end = cl_end; sink.draw_arrays = cl_range;
    sink.inline_array = dp_inline; sink.array_elements = dp_elems; sink.immediate_vertex = dp_imm;
    rsx_dispatch_init(&d, &sink);
#define DM(mth, arg) rsx_dispatch_method(&d, (mth), (arg))

    /* D4: rsx_methods.cpp init seeds, without marking the register seen. */
    CHECK(rsx_dsp_reg(&d, 0x0208) == 0x121u && rsx_dsp_reg(&d, 0x1A04) == 0x00018429u &&
          rsx_dsp_reg(&d, 0x02C0) == 0x0FFF0000u && !d.seen[0x0208 >> 2],
          "init seeds SURFACE_FORMAT/TEXTURE_FORMAT/WINDOW_CLIP, seen[] stays 0");
    CHECK(rsx_dsp_reg(&d, 0x1A10) == 0xAAE4u && rsx_dsp_reg(&d, 0x1A10 + 15u * 0x20u) == 0xAAE4u,
          "T2-03: every fragment TEXTURE_CONTROL1 starts at 0xAAE4 (rsx_methods.cpp:560-635)");

    /* D7: an invalid enum keeps the previous value (nv4097.cpp:570-596, 725-740). */
    DM(0x1830, 0x404u); DM(0x1830, 0x123u);
    DM(0x0368, 0x1D01u); DM(0x0368, 0x5u);
    CHECK(rsx_dsp_reg(&d, 0x1830) == 0x404u && rsx_dsp_reg(&d, 0x0368) == 0x1D01u,
          "invalid CULL_FACE / SHADE_MODE values roll back");

    /* D6: an unknown primitive enters BEGIN keeping the clause; END redraws it. */
    memset(&g_cl, 0, sizeof g_cl);
    DM(0x1808, RSX_PRIMITIVE_TRIANGLES);
    DM(0x1814, 6u | (2u << 24));
    DM(0x1808, 0);
    DM(0x1808, 0x0Fu);
    DM(0x1808, 0);
    CHECK(g_cl.n_ranges == 2 && g_cl.first[1] == 6 && g_cl.count[1] == 3 && g_cl.n_end == 2,
          "an invalid BEGIN re-runs the held clause (%u ranges)", g_cl.n_ranges);

    /* RSX_DSP_DISCARD_DRAW (recover_fifo -> discard_draw) drops the open clause. */
    memset(&g_cl, 0, sizeof g_cl);
    DM(0x1808, RSX_PRIMITIVE_TRIANGLES);
    DM(0x1814, 0u | (2u << 24));
    DM(RSX_DSP_DISCARD_DRAW, 0);
    DM(0x1808, 0);
    CHECK(!d.in_begin_end && g_cl.n_ranges == 0, "a discarded draw never reaches the sink");

    /* D12: a DATA2F/DATA3F write to any word resets z/w (common.cpp:18-36). */
    float v[4];
    DM(M_VTX_ATTR_4F + 8, f32_bits(3.0f)); DM(M_VTX_ATTR_4F + 12, f32_bits(5.0f));
    DM(0x1884, f32_bits(2.0f));                       /* ATTR0 DATA2F y */
    rsx_dsp_vertex_default(&d, 0, v);
    CHECK(v[1] == 2.0f && v[2] == 0.0f && v[3] == 1.0f, "DATA2F y alone resets z=0 w=1");
    DM(M_VTX_ATTR_4F + 12, f32_bits(5.0f));
    DM(0x1504, f32_bits(4.0f));                       /* ATTR0 DATA3F y */
    rsx_dsp_vertex_default(&d, 0, v);
    CHECK(v[1] == 4.0f && v[3] == 1.0f, "DATA3F y alone resets w=1");

    /* D5: an immediate push is dropped when ATTR0 is outside the input mask. */
    g_dp_imm = 0;
    DM(0x1FF0, 0u);
    DM(0x1808, RSX_PRIMITIVE_POINTS);
    DM(0x1508, f32_bits(0.0f));
    DM(0x1FF0, 1u);
    DM(0x1508, f32_bits(0.0f));
    DM(0x1808, 0);
    CHECK(g_dp_imm == 1, "an immediate vertex outside VERTEX_ATTRIB_INPUT_MASK is not pushed");

    /* D9: past the 544-instruction store an upload is a NOP and the load
     * pointer stays (nv4097.cpp:168-243). */
    DM(0x1E9C, RSX_DSP_VP_INSTR - 1u);
    for (u32 i = 0; i < 8; i++) DM(0x0B80 + i * 4, 0x11110000u + i);
    CHECK(rsx_dsp_reg(&d, 0x1E9C) == RSX_DSP_VP_INSTR && d.vp[(RSX_DSP_VP_INSTR - 1u) * 4 + 3] == 0x11110003u,
          "a transform-program upload past the last instruction is dropped");

    /* D13: inline and element streams grow past the fixed buffers. */
    g_dp_inline_bytes = 0; g_dp_elems = 0;
    DM(0x1808, RSX_PRIMITIVE_POINTS);
    for (u32 i = 0; i < RSX_DSP_INLINE_MAX_BYTES / 4u + 16u; i++) DM(0x1818, i);
    DM(0x1808, 0);
    CHECK(g_dp_inline_bytes == RSX_DSP_INLINE_MAX_BYTES + 64u,
          "an inline stream longer than %u bytes is delivered whole (%u)",
          RSX_DSP_INLINE_MAX_BYTES, g_dp_inline_bytes);
    DM(0x1808, RSX_PRIMITIVE_POINTS);
    for (u32 i = 0; i < RSX_DSP_ELEMENT_MAX + 8u; i++) DM(0x1810, i);
    DM(0x1808, 0);
    CHECK(g_dp_elems == RSX_DSP_ELEMENT_MAX + 8u && g_dp_last_elem == RSX_DSP_ELEMENT_MAX + 7u,
          "an element list longer than %u indices is delivered whole (%u)",
          RSX_DSP_ELEMENT_MAX, g_dp_elems);
#undef DM
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_dispatch_methods_parity();
    printf("[rsx_dispatch_methods] %s (%d failed)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}