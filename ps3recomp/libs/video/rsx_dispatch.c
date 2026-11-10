/*
 * ps3recomp - NV4097 method dispatcher (Track B / LAYER 2)
 *
 * See rsx_dispatch.h for the design and the clean-room source list.
 *
 * Register/method facts used here (envytools rnndb nv30-40_3d + Mesa nv30
 * + psdevwiki; RPCS3 consulted as a read-only semantics oracle):
 *
 *   0x0194 DMA_COLOR0, 0x018C DMA_COLOR1, 0x01B4/0x01B8 DMA_COLOR2/3,
 *   0x0198 DMA_ZETA                DMA handle 0xFEED0000 = RSX local memory,
 *                                  0xFEED0001 = main (IO-mapped) memory
 *   0x0200 RT_HORIZ  x[0:15] w[16:31]      0x0204 RT_VERT y/h
 *   0x0208 RT_FORMAT color[0:4] depth[5:7] type[8:11] aa[12:15]
 *   0x020C/021C/0280/0284 COLOR0-3_PITCH   0x0210/0218/0288/028C COLOR0-3_OFFSET
 *   0x0214 ZETA_OFFSET   0x022C ZETA_PITCH   0x0220 RT_ENABLE
 *   0x0A00 VIEWPORT_HORIZ x/w   0x0A04 VIEWPORT_VERT y/h
 *   0x0A20..0x0A2C VIEWPORT_TRANSLATE[4]  0x0A30..0x0A3C VIEWPORT_SCALE[4]
 *   0x0B80..0x0BFC VP_UPLOAD_INST window (load ptr auto-advances per vec4)
 *   0x1680+i*4 VTXBUF_OFFSET[i]  bit31 = location, [0:30] = offset
 *   0x1840+i*4 TEX_SIZE1[i] (control3) pitch[0:19] depth[20:31]
 *   0x1A00+i*0x20 fragment texture unit i (16 units, 8 words each):
 *     +0x00 TEX_OFFSET   +0x04 TEX_FORMAT dma[0:1] (1 = local, 2 = main)
 *                          cube[2] border[3] dims[4:7] fmt[8:15] mips[16:19]
 *     +0x08 TEX_WRAP     +0x0C TEX_ENABLE (control0, enable = bit 31)
 *     +0x10 TEX_SWIZZLE (component remap)  +0x14 TEX_FILTER
 *     +0x18 TEX_NPOT_SIZE height[0:15] width[16:31]  +0x1C TEX_BORDER_COLOR
 *   0x1738 VERTEX_DATA_BASE_OFFSET  0x173C VB_ELEMENT_BASE
 *   0x1740+i*4 VTXFMT[i] type[0:3] size[4:7] stride[8:15] frequency[16:31]
 *   0x1808 VERTEX_BEGIN_END  arg = primitive, 0 = end
 *   0x1814 VB_VERTEX_BATCH   first[0:23] (count-1)[24:31]
 *   0x1818 INLINE_ARRAY     vertex data pushed through the FIFO itself
 *   0x181C IDXBUF_OFFSET     0x1820 IDXBUF_FORMAT location[0:3] type[4:11]
 *   0x1824 VB_INDEX_BATCH    first[0:23] (count-1)[24:31]
 *   0x1D8C CLEAR_DEPTH_VALUE  0x1D90 CLEAR_COLOR_VALUE (A8R8G8B8)
 *   0x1D94 CLEAR_BUFFERS     Z=0x01 S=0x02 R=0x10 G=0x20 B=0x40 A=0x80
 *   0x1E9C VP_UPLOAD_FROM_ID (transform program load pointer)
 *   0x1EFC VP_UPLOAD_CONST_ID (transform constant load pointer; NOT
 *          auto-advancing — every burst re-reads it)
 *   0x1F00..0x1F7C VP_UPLOAD_CONST window: word i -> slot load + i/4, lane i%4
 *   0xE944 gcm driver-queue flip (established empirically from this title)
 */

#include "rsx_dispatch.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ps3emu/log_first.h"

#define M_DMA_COLOR1            0x018C
#define M_DMA_COLOR0            0x0194
#define M_DMA_ZETA              0x0198
#define M_DMA_COLOR2            0x01B4
#define M_DMA_COLOR3            0x01B8
#define M_RT_HORIZ              0x0200
#define M_RT_VERT               0x0204
#define M_RT_FORMAT             0x0208
#define M_COLOR0_PITCH          0x020C
#define M_COLOR0_OFFSET         0x0210
#define M_ZETA_OFFSET           0x0214
#define M_COLOR1_OFFSET         0x0218
#define M_COLOR1_PITCH          0x021C
#define M_RT_ENABLE             0x0220
#define M_ZETA_PITCH            0x022C
#define M_COLOR2_PITCH          0x0280
#define M_COLOR3_PITCH          0x0284
#define M_COLOR2_OFFSET         0x0288
#define M_COLOR3_OFFSET         0x028C
#define M_VIEWPORT_HORIZ        0x0A00
#define M_VIEWPORT_VERT         0x0A04
#define M_VIEWPORT_TRANSLATE    0x0A20
#define M_VIEWPORT_SCALE        0x0A30
#define M_VP_UPLOAD_INST        0x0B80
#define M_VTXBUF_OFFSET         0x1680
#define M_VERTEX_DATA_BASE      0x1738
#define M_VB_ELEMENT_BASE       0x173C
#define M_VTXFMT                0x1740
#define M_VERTEX_BEGIN_END      0x1808
#define M_ARRAY_ELEMENT16       0x180C
#define M_ARRAY_ELEMENT32       0x1810
#define M_VB_VERTEX_BATCH       0x1814
#define M_INLINE_ARRAY          0x1818
#define M_IDXBUF_OFFSET         0x181C
#define M_IDXBUF_FORMAT         0x1820
#define M_VB_INDEX_BATCH        0x1824
#define M_TEX_SIZE1             0x1840  /* + unit * 4  (control3)           */
#define M_TEX_OFFSET            0x1A00  /* + unit * 0x20 (8-word unit block) */
#define M_VERTEX_TEXTURE        0x0900  /* + unit * 0x20 (8-word unit block) */
#define M_FP_ACTIVE_PROGRAM     0x08E4  /* offset | dma-context [1:0]        */
#define M_FP_CONTROL            0x1D60
#define M_VP_START_FROM_ID      0x1EA0
#define M_VTX_ATTR_4F           0x1C00  /* + attr * 0x10, 4 float words      */
#define M_CLEAR_DEPTH_VALUE     0x1D8C
#define M_CLEAR_COLOR_VALUE     0x1D90
#define M_CLEAR_BUFFERS         0x1D94
#define M_VP_UPLOAD_FROM_ID     0x1E9C
#define M_VP_UPLOAD_CONST_ID    0x1EFC
#define M_VP_UPLOAD_CONST       0x1F00
#define M_GCM_FLIP_HEAD_0       0xE920
#define M_GCM_FLIP_HEAD_1       0xE924
#define M_GCM_DRIVER_FLIP       0xE944
#define M_GCM_FLIP_COMMAND      0xFEAC
/* Primitive restart: a sentinel index value that breaks a TRIANGLE_STRIP/
 * FAN mid-draw with no connecting triangle (RPCS3 oracle: gcm_enums.h
 * NV4097_SET_RESTART_INDEX_ENABLE/NV4097_SET_RESTART_INDEX; semantics in
 * rsx_methods.h restart_index_enabled()/restart_index() -- enabled only
 * when the enable bit is set AND, for 16-bit index buffers, the configured
 * value fits in 16 bits). */
#define M_RESTART_INDEX_ENABLE  0x1DAC
#define M_RESTART_INDEX         0x1DB0

/* Render state consumed directly by the replay harness's PSO builder
 * (rsx_dsp_reg reads straight out of the register file, so these already
 * took effect before being classified here; this fixes the coverage report
 * to match reality instead of showing them as TODO). Addresses from
 * tools/nv40_methods.py (envytools rnndb). */
#define M_ALPHA_FUNC_ENABLE     0x0304
#define M_ALPHA_FUNC_FUNC       0x0308
#define M_ALPHA_FUNC_REF        0x030C
#define M_BLEND_FUNC_ENABLE     0x0310
#define M_BLEND_FUNC_SRC        0x0314
#define M_BLEND_FUNC_DST        0x0318
#define M_COLOR_MASK            0x0324
#define M_MRT_COLOR_MASK        0x0370
#define M_DEPTH_FUNC            0x0A6C
#define M_DEPTH_WRITE_ENABLE    0x0A70
#define M_DEPTH_TEST_ENABLE     0x0A74
#define M_CULL_FACE             0x1830
#define M_FRONT_FACE            0x1834
#define M_CULL_FACE_ENABLE      0x183C
#define M_VTX_CACHE_INVALIDATE  0x1714
#define M_NV4097_NOP            0x0100
#define M_VP_ATTRIB_EN          0x1FF0
#define M_VP_RESULT_EN          0x1FF4

#define DMA_HANDLE_LOCAL        0xFEED0000u
#define DMA_HANDLE_MAIN         0xFEED0001u

static u32 dma_to_location(u32 handle)
{
    return handle == DMA_HANDLE_MAIN ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
}

/* RPCS3 rsx_state::reset (rsx_methods.cpp), NV4097 range only: the image
 * GCM_FLIP_COMMAND (0xFEAC) restores before every flip (flip_command ->
 * thread::reset). Titles rely on it: Resistance 2 never rewrites
 * ZSTENCIL_CLEAR_VALUE after a frame's last stencil-only clear (value 0), so
 * without the reset its next scene clear set depth to 0 and nothing passed
 * the depth test. Generated from the RPCS3 source, including its quirks
 * (texture 2's ADDRESS is never reset there). The 2D classes' entries are
 * left out: they would alias NV4097 once the subchannel is masked off. */
static const u32 k_dsp_reset_image[][2] = {
    { 0x0000, 0x31337000u },
    { 0x0060, 0x66616661u },
    { 0x0064, 0x00000010u },
    { 0x0100, 0x00000000u },
    { 0x0180, 0x66604200u },
    { 0x0184, 0xFEED0000u },
    { 0x0188, 0xFEED0001u },
    { 0x018C, 0xFEED0000u },
    { 0x0190, 0x00000000u },
    { 0x0194, 0xFEED0000u },
    { 0x0198, 0xFEED0000u },
    { 0x019C, 0xFEED0000u },
    { 0x01A0, 0xFEED0001u },
    { 0x01A4, 0x66606660u },
    { 0x01A8, 0x66626660u },
    { 0x01AC, 0x00000000u },
    { 0x01B0, 0x00000000u },
    { 0x01B4, 0xFEED0000u },
    { 0x01B8, 0xFEED0000u },
    { 0x0200, 0x00000000u },
    { 0x0204, 0x00000000u },
    { 0x0208, 0x00000121u },
    { 0x020C, 0x00000040u },
    { 0x0210, 0x00000000u },
    { 0x0214, 0x00000000u },
    { 0x0218, 0x00000000u },
    { 0x021C, 0x00000040u },
    { 0x0220, 0x00000001u },
    { 0x0224, 0x00000080u },
    { 0x0228, 0x00000100u },
    { 0x022C, 0x00000040u },
    { 0x0230, 0x00000000u },
    { 0x0238, 0x00000000u },
    { 0x0240, 0x0000FFFFu },
    { 0x0244, 0x00000000u },
    { 0x0248, 0x00000000u },
    { 0x024C, 0x00000000u },
    { 0x0280, 0x00000040u },
    { 0x0284, 0x00000040u },
    { 0x0288, 0x00000000u },
    { 0x028C, 0x00000000u },
    { 0x02B8, 0x00000000u },
    { 0x02BC, 0x00000000u },
    { 0x02C0, 0x0FFF0000u },
    { 0x02C4, 0x0FFF0000u },
    { 0x02C8, 0x0FFF0000u },
    { 0x02CC, 0x0FFF0000u },
    { 0x02D0, 0x0FFF0000u },
    { 0x02D4, 0x0FFF0000u },
    { 0x02D8, 0x0FFF0000u },
    { 0x02DC, 0x0FFF0000u },
    { 0x02E0, 0x0FFF0000u },
    { 0x02E4, 0x0FFF0000u },
    { 0x02E8, 0x0FFF0000u },
    { 0x02EC, 0x0FFF0000u },
    { 0x02F0, 0x0FFF0000u },
    { 0x02F4, 0x0FFF0000u },
    { 0x02F8, 0x0FFF0000u },
    { 0x02FC, 0x0FFF0000u },
    { 0x0300, 0x00000001u },
    { 0x0304, 0x00000000u },
    { 0x0308, 0x00000207u },
    { 0x030C, 0x00000000u },
    { 0x0310, 0x00000000u },
    { 0x0314, 0x00010001u },
    { 0x0318, 0x00000000u },
    { 0x031C, 0x00000000u },
    { 0x0320, 0x80068006u },
    { 0x0324, 0x01010101u },
    { 0x0328, 0x00000000u },
    { 0x032C, 0x000000FFu },
    { 0x0330, 0x00000207u },
    { 0x0334, 0x00000000u },
    { 0x0338, 0x000000FFu },
    { 0x033C, 0x00001E00u },
    { 0x0340, 0x00001E00u },
    { 0x0344, 0x00001E00u },
    { 0x0348, 0x00000000u },
    { 0x034C, 0x000000FFu },
    { 0x0350, 0x00000207u },
    { 0x0354, 0x00000000u },
    { 0x0358, 0x000000FFu },
    { 0x035C, 0x00001E00u },
    { 0x0360, 0x00001E00u },
    { 0x0364, 0x00001E00u },
    { 0x0368, 0x00001D01u },
    { 0x036C, 0x00000000u },
    { 0x0370, 0x00000000u },
    { 0x0374, 0x00000000u },
    { 0x0378, 0x00001503u },
    { 0x037C, 0x00000000u },
    { 0x0380, 0x00000000u },
    { 0x0384, 0x00000000u },
    { 0x0388, 0x3F800000u },
    { 0x0394, 0x00000000u },
    { 0x0398, 0x3F800000u },
    { 0x03B0, 0x00100000u },
    { 0x03B8, 0x00000008u },
    { 0x03BC, 0x00000000u },
    { 0x03C0, 0x00010101u },
    { 0x03C4, 0x00010101u },
    { 0x03C8, 0x00010101u },
    { 0x03CC, 0x00010101u },
    { 0x03D0, 0x00010101u },
    { 0x03D4, 0x00010101u },
    { 0x03D8, 0x00010101u },
    { 0x03DC, 0x00010101u },
    { 0x03E0, 0x00010101u },
    { 0x03E4, 0x00010101u },
    { 0x03E8, 0x00010101u },
    { 0x03EC, 0x00010101u },
    { 0x03F0, 0x00010101u },
    { 0x03F4, 0x00010101u },
    { 0x03F8, 0x00010101u },
    { 0x03FC, 0x00010101u },
    { 0x0400, 0x00007421u },
    { 0x0404, 0x00007421u },
    { 0x0408, 0x00007421u },
    { 0x040C, 0x00007421u },
    { 0x0410, 0x00007421u },
    { 0x0414, 0x00007421u },
    { 0x0418, 0x00007421u },
    { 0x041C, 0x00007421u },
    { 0x0420, 0x00007421u },
    { 0x0424, 0x00007421u },
    { 0x0428, 0x00007421u },
    { 0x042C, 0x00007421u },
    { 0x0430, 0x00007421u },
    { 0x0434, 0x00007421u },
    { 0x0438, 0x00007421u },
    { 0x043C, 0x00007421u },
    { 0x0440, 0x9AABAA98u },
    { 0x0444, 0x66666789u },
    { 0x0448, 0x98766666u },
    { 0x044C, 0x89AABAA9u },
    { 0x0450, 0x99999999u },
    { 0x0454, 0x88888889u },
    { 0x0458, 0x98888888u },
    { 0x045C, 0x99999999u },
    { 0x0460, 0x56676654u },
    { 0x0464, 0x33333345u },
    { 0x0468, 0x54333333u },
    { 0x046C, 0x45667665u },
    { 0x0470, 0xAABBBA99u },
    { 0x0474, 0x66667899u },
    { 0x0478, 0x99876666u },
    { 0x047C, 0x99ABBBAAu },
    { 0x08C0, 0x10000000u },
    { 0x08C4, 0x10000000u },
    { 0x08CC, 0x00000800u },
    { 0x08D0, 0x00000000u },
    { 0x08D4, 0x00000000u },
    { 0x08D8, 0x00000000u },
    { 0x0908, 0x00000101u },
    { 0x090C, 0x00060000u },
    { 0x0914, 0x00000000u },
    { 0x091C, 0x00000000u },
    { 0x0928, 0x00000101u },
    { 0x092C, 0x00060000u },
    { 0x0934, 0x00000000u },
    { 0x093C, 0x00000000u },
    { 0x0948, 0x00000101u },
    { 0x094C, 0x00060000u },
    { 0x0954, 0x00000000u },
    { 0x095C, 0x00000000u },
    { 0x0968, 0x00000101u },
    { 0x096C, 0x00060000u },
    { 0x0974, 0x00000000u },
    { 0x097C, 0x00000000u },
    { 0x0A00, 0x10000000u },
    { 0x0A04, 0x10000000u },
    { 0x0A0C, 0x00000000u },
    { 0x0A20, 0x45000000u },
    { 0x0A24, 0x45000000u },
    { 0x0A28, 0x3F000000u },
    { 0x0A2C, 0x00000000u },
    { 0x0A30, 0x45000000u },
    { 0x0A34, 0x45000000u },
    { 0x0A38, 0x3F000000u },
    { 0x0A3C, 0x00000000u },
    { 0x0A60, 0x00000000u },
    { 0x0A64, 0x00000000u },
    { 0x0A68, 0x00000000u },
    { 0x0A6C, 0x00000201u },
    { 0x0A70, 0x00000001u },
    { 0x0A74, 0x00000000u },
    { 0x0A78, 0x00000000u },
    { 0x0A7C, 0x00000000u },
    { 0x0B00, 0x00002DC8u },
    { 0x0B04, 0x00002DC8u },
    { 0x0B08, 0x00002DC8u },
    { 0x0B0C, 0x00002DC8u },
    { 0x0B10, 0x00002DC8u },
    { 0x0B14, 0x00002DC8u },
    { 0x0B18, 0x00002DC8u },
    { 0x0B1C, 0x00002DC8u },
    { 0x0B20, 0x00002DC8u },
    { 0x0B24, 0x00002DC8u },
    { 0x0B28, 0x00002DC8u },
    { 0x0B2C, 0x00002DC8u },
    { 0x0B30, 0x00002DC8u },
    { 0x0B34, 0x00002DC8u },
    { 0x0B38, 0x00002DC8u },
    { 0x0B3C, 0x00002DC8u },
    { 0x0B40, 0x00000000u },
    { 0x0B44, 0x00000000u },
    { 0x0B48, 0x00000000u },
    { 0x0B4C, 0x00000000u },
    { 0x0B50, 0x00000000u },
    { 0x0B54, 0x00000000u },
    { 0x0B58, 0x00000000u },
    { 0x0B5C, 0x00000000u },
    { 0x0B60, 0x00000000u },
    { 0x0B64, 0x00000000u },
    { 0x1428, 0x00000001u },
    { 0x142C, 0x00000000u },
    { 0x1450, 0x00080003u },
    { 0x1454, 0x00000000u },
    { 0x145C, 0x00000001u },
    { 0x1478, 0x00000000u },
    { 0x147C, 0x00000000u },
    { 0x1680, 0x00000000u },
    { 0x1684, 0x00000000u },
    { 0x1688, 0x00000000u },
    { 0x168C, 0x00000000u },
    { 0x1690, 0x00000000u },
    { 0x1694, 0x00000000u },
    { 0x1698, 0x00000000u },
    { 0x169C, 0x00000000u },
    { 0x16A0, 0x00000000u },
    { 0x16A4, 0x00000000u },
    { 0x16A8, 0x00000000u },
    { 0x16AC, 0x00000000u },
    { 0x16B0, 0x00000000u },
    { 0x16B4, 0x00000000u },
    { 0x16B8, 0x00000000u },
    { 0x16BC, 0x00000000u },
    { 0x1738, 0x00000000u },
    { 0x173C, 0x00000000u },
    { 0x1740, 0x00000002u },
    { 0x1744, 0x00000002u },
    { 0x1748, 0x00000002u },
    { 0x174C, 0x00000002u },
    { 0x1750, 0x00000002u },
    { 0x1754, 0x00000002u },
    { 0x1758, 0x00000002u },
    { 0x175C, 0x00000002u },
    { 0x1760, 0x00000002u },
    { 0x1764, 0x00000002u },
    { 0x1768, 0x00000002u },
    { 0x176C, 0x00000002u },
    { 0x1770, 0x00000002u },
    { 0x1774, 0x00000002u },
    { 0x1778, 0x00000002u },
    { 0x177C, 0x00000002u },
    { 0x17CC, 0x00000000u },
    { 0x1828, 0x00001B02u },
    { 0x182C, 0x00001B02u },
    { 0x1830, 0x00000405u },
    { 0x1834, 0x00000901u },
    { 0x1838, 0x00000000u },
    { 0x183C, 0x00000000u },
    { 0x1A08, 0x00030101u },
    { 0x1A0C, 0x00060000u },
    { 0x1A14, 0x02052000u },
    { 0x1A1C, 0x00000000u },
    { 0x1A28, 0x00030101u },
    { 0x1A2C, 0x00060000u },
    { 0x1A34, 0x02052000u },
    { 0x1A3C, 0x00000000u },
    { 0x1A4C, 0x00060000u },
    { 0x1A54, 0x02052000u },
    { 0x1A5C, 0x00000000u },
    { 0x1A68, 0x00030101u },
    { 0x1A6C, 0x00060000u },
    { 0x1A74, 0x02052000u },
    { 0x1A7C, 0x00000000u },
    { 0x1A88, 0x00030101u },
    { 0x1A8C, 0x00060000u },
    { 0x1A94, 0x02052000u },
    { 0x1A9C, 0x00000000u },
    { 0x1AA8, 0x00030101u },
    { 0x1AAC, 0x00060000u },
    { 0x1AB4, 0x02052000u },
    { 0x1ABC, 0x00000000u },
    { 0x1AC8, 0x00030101u },
    { 0x1ACC, 0x00060000u },
    { 0x1AD4, 0x02052000u },
    { 0x1ADC, 0x00000000u },
    { 0x1AE8, 0x00030101u },
    { 0x1AEC, 0x00060000u },
    { 0x1AF4, 0x02052000u },
    { 0x1AFC, 0x00000000u },
    { 0x1B08, 0x00030101u },
    { 0x1B0C, 0x00060000u },
    { 0x1B14, 0x02052000u },
    { 0x1B1C, 0x00000000u },
    { 0x1B28, 0x00030101u },
    { 0x1B2C, 0x00060000u },
    { 0x1B34, 0x02052000u },
    { 0x1B3C, 0x00000000u },
    { 0x1B48, 0x00030101u },
    { 0x1B4C, 0x00060000u },
    { 0x1B54, 0x02052000u },
    { 0x1B5C, 0x00000000u },
    { 0x1B68, 0x00030101u },
    { 0x1B6C, 0x00060000u },
    { 0x1B74, 0x02052000u },
    { 0x1B7C, 0x00000000u },
    { 0x1B88, 0x00030101u },
    { 0x1B8C, 0x00060000u },
    { 0x1B94, 0x02052000u },
    { 0x1B9C, 0x00000000u },
    { 0x1BA8, 0x00030101u },
    { 0x1BAC, 0x00060000u },
    { 0x1BB4, 0x02052000u },
    { 0x1BBC, 0x00000000u },
    { 0x1BC8, 0x00030101u },
    { 0x1BCC, 0x00060000u },
    { 0x1BD4, 0x02052000u },
    { 0x1BDC, 0x00000000u },
    { 0x1BE8, 0x00030101u },
    { 0x1BEC, 0x00060000u },
    { 0x1BF4, 0x02052000u },
    { 0x1BFC, 0x00000000u },
    { 0x1D64, 0x02000000u },
    { 0x1D78, 0x00000001u },
    { 0x1D7C, 0xFFFF0000u },
    { 0x1D80, 0x00000003u },
    { 0x1D88, 0x00001000u },
    { 0x1D8C, 0xFFFFFF00u },
    { 0x1D90, 0x00000000u },
    { 0x1D94, 0x00000000u },
    { 0x1D98, 0x0FFF0000u },
    { 0x1D9C, 0x0FFF0000u },
    { 0x1DA4, 0x00000000u },
    { 0x1DAC, 0x00000000u },
    { 0x1DB0, 0xFFFFFFFFu },
    { 0x1DB4, 0x00000000u },
    { 0x1E94, 0x00000011u },
    { 0x1E98, 0x01000000u },
    { 0x1EE0, 0x3F800000u },
    { 0x1EE4, 0x00000000u },
    { 0x1EE8, 0x00000000u },
    { 0x1FC0, 0x00000000u },
    { 0x1FC4, 0x06144321u },
    { 0x1FC8, 0xEDCBA987u },
    { 0x1FCC, 0x0000006Fu },
    { 0x1FD0, 0x00171615u },
    { 0x1FD4, 0x001B1A19u },
    { 0x1FE0, 0x00000001u },
    { 0x1FE8, 0x00000000u },
    { 0x1FEC, 0x00000000u },
    { 0x1FF0, 0x0000FFFFu },
    { 0x1FF4, 0x003FFFFFu },
    { 0x1FF8, 0x00000000u },
};

void rsx_dispatch_reset_state(rsx_dispatch* rsx)
{
    for (u32 i = 0; i < sizeof(k_dsp_reset_image) / sizeof(k_dsp_reset_image[0]); i++)
        rsx->regs[k_dsp_reset_image[i][0] >> 2] = k_dsp_reset_image[i][1];
}

static int dsp_flip_reset_enabled(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_FLIP_RESET"); on = !(e && *e == '0'); }
    return on;
}
/* Env switches for the RPCS3-parity rules below: "=0" restores the old
 * behaviour. Read once. */
static int dsp_env_on(const char* name, int* cache)
{
    if (*cache < 0) { const char* e = getenv(name); *cache = !(e && *e == '0'); }
    return *cache;
}
static int s_env_init_seeds = -1, s_env_enum_rollback = -1, s_env_imm_mask = -1,
           s_env_begin_keep = -1, s_env_zw_reset = -1, s_env_grow = -1;

/* ---- grown INLINE_ARRAY / ARRAY_ELEMENT storage (D13) ------------------
 * A stream that outgrows the in-struct arrays moves to a heap buffer kept
 * here rather than in the struct: rsx_dispatch_init memsets the struct, which
 * would leak a pointer, and the in-struct arrays stay where the cache layout
 * wants them. Keyed by instance; init releases the instance's buffers. */
typedef struct { const rsx_dispatch* owner; u8* inl; u32 inl_cap; u32* elem; u32 elem_cap; } dsp_spill;
static dsp_spill s_dsp_spill[8];

static dsp_spill* dsp_spill_for(const rsx_dispatch* rsx, int create)
{
    dsp_spill* free_slot = NULL;
    for (u32 i = 0; i < sizeof(s_dsp_spill) / sizeof(s_dsp_spill[0]); i++) {
        if (s_dsp_spill[i].owner == rsx) return &s_dsp_spill[i];
        if (!s_dsp_spill[i].owner && !free_slot) free_slot = &s_dsp_spill[i];
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->owner = rsx;
    return free_slot;
}

static void dsp_spill_release(const rsx_dispatch* rsx)
{
    dsp_spill* s = dsp_spill_for(rsx, 0);
    if (!s) return;
    free(s->inl);
    free(s->elem);
    memset(s, 0, sizeof(*s));
}

/* Where the current stream's bytes/indices live: in the struct until they
 * outgrow it, then in the grown buffer (the whole stream is moved over). */
static const u8* dsp_inline_ptr(const rsx_dispatch* rsx)
{
    if (rsx->inline_len <= RSX_DSP_INLINE_MAX_BYTES) return rsx->inline_data;
    const dsp_spill* s = dsp_spill_for(rsx, 0);
    return s ? s->inl : rsx->inline_data;
}
static const u32* dsp_elem_ptr(const rsx_dispatch* rsx)
{
    if (rsx->elem_count <= RSX_DSP_ELEMENT_MAX) return rsx->elem_data;
    const dsp_spill* s = dsp_spill_for(rsx, 0);
    return s ? s->elem : rsx->elem_data;
}

/* Room for `need` bytes of inline stream; NULL when it cannot grow. */
static u8* dsp_inline_room(rsx_dispatch* rsx, u32 need)
{
    if (need <= RSX_DSP_INLINE_MAX_BYTES) return rsx->inline_data;
    if (!dsp_env_on("RSX_DSP_GROW", &s_env_grow) || need > RSX_DSP_GROW_MAX_BYTES) return NULL;
    dsp_spill* s = dsp_spill_for(rsx, 1);
    if (!s) return NULL;
    if (need > s->inl_cap) {
        u32 cap = s->inl_cap ? s->inl_cap : RSX_DSP_INLINE_MAX_BYTES * 2u;
        while (cap < need) cap *= 2u;
        u8* p = (u8*)realloc(s->inl, cap);
        if (!p) return NULL;
        s->inl = p; s->inl_cap = cap;
    }
    if (rsx->inline_len <= RSX_DSP_INLINE_MAX_BYTES)
        memcpy(s->inl, rsx->inline_data, rsx->inline_len);
    return s->inl;
}
static u32* dsp_elem_room(rsx_dispatch* rsx, u32 need)
{
    if (need <= RSX_DSP_ELEMENT_MAX) return rsx->elem_data;
    if (!dsp_env_on("RSX_DSP_GROW", &s_env_grow) || need > RSX_DSP_GROW_MAX_BYTES / 4u) return NULL;
    dsp_spill* s = dsp_spill_for(rsx, 1);
    if (!s) return NULL;
    if (need > s->elem_cap) {
        u32 cap = s->elem_cap ? s->elem_cap : RSX_DSP_ELEMENT_MAX * 2u;
        while (cap < need) cap *= 2u;
        u32* p = (u32*)realloc(s->elem, (size_t)cap * 4u);
        if (!p) return NULL;
        s->elem = p; s->elem_cap = cap;
    }
    if (rsx->elem_count <= RSX_DSP_ELEMENT_MAX)
        memcpy(s->elem, rsx->elem_data, (size_t)rsx->elem_count * 4u);
    return s->elem;
}

/* RPCS3 rsx_state::init (rsx_methods.cpp:126-689): values set at
 * initialisation and by the commands cellGcmInit injects, NOT by a context
 * reset (so not part of k_dsp_reset_image). seen[] stays 0. */
static void dsp_seed_init_registers(rsx_dispatch* rsx)
{
    static const u32 seeds[][2] = {
        { 0x08E4, 0x00000002u },  /* SHADER_PROGRAM: LOCAL+1, offset 0   */
        { 0x0180, 0x66604200u },  /* CONTEXT_DMA_NOTIFIES                */
        { 0x0184, 0xFEED0000u },  /* CONTEXT_DMA_A                       */
        { 0x0188, 0xFEED0001u },  /* CONTEXT_DMA_B                       */
        { 0x018C, 0xFEED0000u },  /* CONTEXT_DMA_COLOR_B                 */
        { 0x0194, 0xFEED0000u },  /* CONTEXT_DMA_COLOR_A                 */
        { 0x0198, 0xFEED0000u },  /* CONTEXT_DMA_ZETA                    */
        { 0x019C, 0xFEED0000u },  /* CONTEXT_DMA_VERTEX_A                */
        { 0x01A0, 0xFEED0001u },  /* CONTEXT_DMA_VERTEX_B                */
        { 0x01A4, 0x66606660u },  /* CONTEXT_DMA_SEMAPHORE               */
        { 0x01A8, 0x66626660u },  /* CONTEXT_DMA_REPORT                  */
        { 0x01B4, 0xFEED0000u },  /* CONTEXT_DMA_COLOR_C                 */
        { 0x01B8, 0xFEED0000u },  /* CONTEXT_DMA_COLOR_D                 */
        { 0x0060, 0x66616661u },  /* NV406E CONTEXT_DMA_SEMAPHORE        */
        { 0x0208, 0x00000121u },  /* SURFACE_FORMAT                      */
        { 0x020C, 0x00000040u },  /* SURFACE_PITCH_A                     */
        { 0x021C, 0x00000040u },  /* SURFACE_PITCH_B                     */
        { 0x0220, 0x00000001u },  /* SURFACE_COLOR_TARGET                */
        { 0x0224, 0x00000080u },
        { 0x0228, 0x00000100u },
        { 0x022C, 0x00000040u },  /* SURFACE_PITCH_Z                     */
        { 0x0280, 0x00000040u },  /* SURFACE_PITCH_C                     */
        { 0x0284, 0x00000040u },  /* SURFACE_PITCH_D                     */
        { 0x03B0, 0x00100000u },  /* CONTROL0                            */
        { 0x1428, 0x00000001u },
        { 0x1450, 0x00080003u },
        { 0x145C, 0x00000001u },
        { 0x1D84, 0x00000003u },  /* ZCULL_EN                            */
        { 0x1EA4, 0x00000010u },  /* ZCULL_CONTROL0                      */
        { 0x1EA8, 0x01000100u },  /* ZCULL_CONTROL1                      */
        { 0x1EAC, 0xFF000002u },  /* SCULL_CONTROL                       */
        { 0x2324, 0x00000101u },
        /* 2D objects: NV3062 (0x6xxx), NV309E (0x8xxx), NV308A (0xAxxx),
         * NV3089 (0xCxxx) handles, DMAs and formats. */
        { 0x2000, 0x31337303u }, { 0x2180, 0x66604200u }, { 0x2184, 0xFEED0001u },
        { 0x2188, 0xFEED0000u },
        { 0x6000, 0x313371C3u }, { 0x6180, 0x66604200u }, { 0x6184, 0xFEED0000u },
        { 0x6188, 0xFEED0000u }, { 0x6300, 0x0000000Au }, { 0x6304, 0x00400040u },
        { 0x8000, 0x31337A73u }, { 0x8180, 0x66604200u }, { 0x8184, 0xFEED0000u },
        { 0x8300, 0x0001000Au },
        { 0xA000, 0x31337808u }, { 0xA180, 0x66604200u }, { 0xA19C, 0x313371C3u },
        { 0xA2FC, 0x00000003u }, { 0xA300, 0x00000004u },
        { 0xC000, 0x3137AF00u }, { 0xC180, 0x66604200u }, { 0xC184, 0xFEED0000u },
        { 0xC198, 0x313371C3u }, { 0xC2FC, 0x00000001u }, { 0xC300, 0x00000003u },
        { 0xC304, 0x00000003u }, { 0xC400, 0x00010002u }, { 0xC404, 0x00010000u },
        { 0xE000, 0xCAFEBABEu },
    };
    for (u32 i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
        rsx->regs[seeds[i][0] >> 2] = seeds[i][1];
    for (u32 i = 0; i < 16; i++) {
        rsx->regs[(0x1A04 + i * 0x20) >> 2] = 0x00018429u;  /* TEXTURE_FORMAT     */
        rsx->regs[(0x1A10 + i * 0x20) >> 2] = 0x0000AAE4u;  /* TEXTURE_CONTROL1   */
        rsx->regs[(0x1A18 + i * 0x20) >> 2] = 0x00080008u;  /* TEXTURE_IMAGE_RECT */
        rsx->regs[(0x1840 + i * 4) >> 2]    = 0x00100008u;  /* TEXTURE_CONTROL3   */
    }
    for (u32 i = 0; i < 4; i++) {
        rsx->regs[(0x0904 + i * 0x20) >> 2] = 0x0001BC21u;  /* VERTEX_TEXTURE_FORMAT     */
        rsx->regs[(0x0910 + i * 0x20) >> 2] = 0x00000008u;  /* VERTEX_TEXTURE_CONTROL3   */
        rsx->regs[(0x0918 + i * 0x20) >> 2] = 0x00080008u;  /* VERTEX_TEXTURE_IMAGE_RECT */
    }
    for (u32 m = 0x02C0; m <= 0x02FC; m += 4)
        rsx->regs[m >> 2] = 0x0FFF0000u;                    /* WINDOW_CLIP_*      */
}

/* RPCS3 rollback rules (nv4097.cpp): an invalid enum leaves the register at
 * its previous value. Returns 1 when `arg` is not a legal value. */
static int dsp_enum_invalid(u32 method, u32 arg)
{
    switch (method) {
    case 0x0324:  /* COLOR_MASK: set_color_mask, nv4097.cpp:340-355 */
        return (arg & 0xFEFEFEFEu) != 0;
    case 0x033C: case 0x0340: case 0x0344:  /* STENCIL_OP_*: set_stencil_op 357-372 */
    case 0x035C: case 0x0360: case 0x0364:
        return !(arg == 0x1E00u || arg == 0 || arg == 0x1E01u || arg == 0x1E02u ||
                 arg == 0x1E03u || arg == 0x150Au || arg == 0x8507u || arg == 0x8508u);
    case 0x1830:  /* CULL_FACE: set_face_property 570-596 */
        return !(arg == 0x404u || arg == 0x405u || arg == 0x408u);
    case 0x1834:  /* FRONT_FACE */
        return !(arg == 0x900u || arg == 0x901u);
    case 0x0320: {  /* BLEND_EQUATION: set_blend_equation 598-614 */
        for (int h = 0; h < 2; h++) {
            const u32 v = h ? arg >> 16 : arg & 0xFFFFu;
            if (!((v >= 0x8006u && v <= 0x800Bu) || (v >= 0xF005u && v <= 0xF007u))) return 1;
        }
        return 0;
    }
    case 0x0314: case 0x0318: {  /* BLEND_FUNC_S/DFACTOR: set_blend_factor 616-632 */
        for (int h = 0; h < 2; h++) {
            const u32 v = h ? arg >> 16 : arg & 0xFFFFu;
            if (!(v <= 1u || (v >= 0x0300u && v <= 0x8004u))) return 1;
        }
        return 0;
    }
    case 0x0368:  /* SHADE_MODE: set_shading_mode 725-740 */
        return !(arg == 0x1D00u || arg == 0x1D01u);
    case 0x1820:  /* INDEX_ARRAY_DMA: check_index_array_dma 468-481 */
        return (arg & ~0x11u) != 0;
    default:
        return 0;
    }
}

static void mark_class(rsx_dispatch* rsx, u32 method, u32 count, u8 klass)
{
    for (u32 i = 0; i < count; i++)
        rsx->klass[(method >> 2) + i] = klass;
}

void rsx_dispatch_init(rsx_dispatch* rsx, const rsx_dispatch_sink* sink)
{
    dsp_spill_release(rsx);
    memset(rsx, 0, sizeof(*rsx));
    if (sink)
        rsx->sink = *sink;
    rsx_dispatch_reset_state(rsx);
    /* RSX_INIT_SEEDS=0: leave the init-only registers at 0 as before. */
    if (dsp_env_on("RSX_INIT_SEEDS", &s_env_init_seeds))
        dsp_seed_init_registers(rsx);

    /* s31 (scratch/s31_blue_emitter.md): the nv40 RESET DEFAULT for
     * COLOR_MASK is all-channels-on (RPCS3 rsx_methods.cpp:345,983 seeds
     * registers[NV4097_SET_COLOR_MASK]=0x1010101). Seeding it here makes a
     * raw register read trustworthy for every consumer: a game-written 0 is
     * a REAL "write no color channels" (the character shadow-mask
     * depth-prime pass), distinguishable from never-initialized. Without
     * the seed, consumers substituted mask=ALL for 0 and force-wrote the
     * depth-only pass's garbage FP output (the blue-character class). */
    rsx->regs[M_COLOR_MASK >> 2] = 0x01010101u;

    /* NV40 reset state is counter-clockwise front winding.  Leaving this
     * register zero made a live stream that relied on the hardware default
     * cull the exterior of orphanage meshes, while captured replay was correct
     * because its seeded register image already contained 0x901. */
    rsx->regs[M_FRONT_FACE >> 2] = 0x0901u;

    /* NV40 reset ZSTENCIL_CLEAR_VALUE (Z24S8: depth24<<8 | stencil8) is
     * 0xFFFFFF00 = depth 1.0, stencil 0 (RPCS3 rsx_methods.cpp reset image
     * as numbering oracle). Seeded so a raw register read is trustworthy for
     * clear consumers, mirroring COLOR_MASK/FRONT_FACE above. */
    rsx->regs[0x1D8C >> 2] = 0xFFFFFF00u;

    /* The rest of the nv4097 draw-state reset image (RPCS3 rsx_state::reset,
     * rsx_methods.cpp:320-372, 492-523): a register the title never writes
     * reads what RPCS3 reads. seen[] stays 0. */
    {
        static const u32 reset_image[][2] = {
            { 0x0308, 0x00000207u },  /* ALPHA_FUNC always               */
            { 0x0314, 0x00010001u },  /* BLEND_FUNC_SFACTOR one/one      */
            { 0x0320, 0x80068006u },  /* BLEND_EQUATION add/add          */
            { 0x032C, 0x000000FFu },  /* STENCIL_MASK                    */
            { 0x0330, 0x00000207u },  /* STENCIL_FUNC always             */
            { 0x0338, 0x000000FFu },  /* STENCIL_FUNC_MASK               */
            { 0x033C, 0x00001E00u },  /* STENCIL_OP_FAIL keep            */
            { 0x0340, 0x00001E00u },  /* STENCIL_OP_ZFAIL keep           */
            { 0x0344, 0x00001E00u },  /* STENCIL_OP_ZPASS keep           */
            { 0x034C, 0x000000FFu },  /* BACK_STENCIL_MASK               */
            { 0x0350, 0x00000207u },  /* BACK_STENCIL_FUNC always        */
            { 0x0358, 0x000000FFu },  /* BACK_STENCIL_FUNC_MASK          */
            { 0x035C, 0x00001E00u },  /* BACK_STENCIL_OP_FAIL keep       */
            { 0x0360, 0x00001E00u },  /* BACK_STENCIL_OP_ZFAIL keep      */
            { 0x0364, 0x00001E00u },  /* BACK_STENCIL_OP_ZPASS keep      */
            { 0x0368, 0x00001D01u },  /* SHADE_MODE smooth               */
            { 0x0378, 0x00001503u },  /* LOGIC_OP copy                   */
            { 0x0388, 0x3F800000u },  /* DEPTH_BOUNDS_MAX 1.0            */
            { 0x0398, 0x3F800000u },  /* CLIP_MAX 1.0                    */
            { 0x0A6C, 0x00000201u },  /* DEPTH_FUNC less                 */
            { 0x0A70, 0x00000001u },  /* DEPTH_MASK write on             */
            { 0x1828, 0x00001B02u },  /* FRONT_POLYGON_MODE fill         */
            { 0x182C, 0x00001B02u },  /* BACK_POLYGON_MODE fill          */
            { 0x1830, 0x00000405u },  /* CULL_FACE back                  */
            { 0x1D78, 0x00000001u },  /* ZMIN_MAX_CONTROL                */
            { 0x1DB0, 0xFFFFFFFFu },  /* RESTART_INDEX                   */
        };
        for (u32 i = 0; i < sizeof(reset_image) / sizeof(reset_image[0]); i++)
            rsx->regs[reset_image[i][0] >> 2] = reset_image[i][1];
    }

    /* Execution methods */
    mark_class(rsx, M_VERTEX_BEGIN_END,  1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_VB_VERTEX_BATCH,   1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_INLINE_ARRAY,      1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_ARRAY_ELEMENT16,   2, RSX_DSP_CLASS_EXEC); /* +32 */
    mark_class(rsx, M_VB_INDEX_BATCH,    1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_CLEAR_BUFFERS,     1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_VP_UPLOAD_INST,   32, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_VP_UPLOAD_CONST,  32, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_GCM_FLIP_HEAD_0,   1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_GCM_FLIP_HEAD_1,   1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_GCM_DRIVER_FLIP,   1, RSX_DSP_CLASS_EXEC);

    /* State the getters decode */
    mark_class(rsx, M_DMA_COLOR1,        1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_DMA_COLOR0,        2, RSX_DSP_CLASS_STATE); /* +ZETA */
    mark_class(rsx, M_DMA_COLOR2,        2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_RT_HORIZ,         12, RSX_DSP_CLASS_STATE); /* 0x200..0x22C */
    mark_class(rsx, M_COLOR2_PITCH,      4, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VIEWPORT_HORIZ,    2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VIEWPORT_TRANSLATE, 8, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VTXBUF_OFFSET,    16, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VERTEX_DATA_BASE,  2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VTXFMT,           16, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_IDXBUF_OFFSET,     2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_TEX_SIZE1,        16, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_TEX_OFFSET, RSX_DSP_NUM_TEXTURES * 8, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_CLEAR_DEPTH_VALUE, 2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VP_UPLOAD_FROM_ID, 1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_RESTART_INDEX_ENABLE, 2, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VP_UPLOAD_CONST_ID, 1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_FP_ACTIVE_PROGRAM, 1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_FP_CONTROL,        1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VP_START_FROM_ID,  1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VTX_ATTR_4F,      64, RSX_DSP_CLASS_STATE);

    /* Alpha test / blend / color-mask / depth / cull: rsx_dsp_reg reads these
     * straight from the register file (replay_main.c's decode_render_state,
     * color-mask application), so they were already live; this only corrects
     * the coverage report. */
    mark_class(rsx, M_ALPHA_FUNC_ENABLE, 3, RSX_DSP_CLASS_STATE); /* ..FUNC_REF */
    mark_class(rsx, M_BLEND_FUNC_ENABLE, 5, RSX_DSP_CLASS_STATE); /* ..BLEND_EQUATION */
    mark_class(rsx, M_COLOR_MASK,        1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_MRT_COLOR_MASK,    1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_DEPTH_FUNC,        3, RSX_DSP_CLASS_STATE); /* ..TEST_ENABLE */
    mark_class(rsx, M_CULL_FACE,         1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_FRONT_FACE,        1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_CULL_FACE_ENABLE,  1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VP_ATTRIB_EN,      1, RSX_DSP_CLASS_STATE);
    mark_class(rsx, M_VP_RESULT_EN,      1, RSX_DSP_CLASS_STATE);

    /* Explicit no-ops: the vertex cache invalidate hint and the reserved
     * NOP method carry no state the replay harness needs (there is no CPU
     * vertex cache to invalidate; every draw re-fetches from guest memory
     * each time). Classifying them EXEC removes them from the TODO count
     * without pretending they do anything. */
    mark_class(rsx, M_VTX_CACHE_INVALIDATE, 1, RSX_DSP_CLASS_EXEC);
    mark_class(rsx, M_NV4097_NOP,           1, RSX_DSP_CLASS_EXEC);
}

void rsx_dispatch_seed_registers(rsx_dispatch* rsx, const u32* regs, u32 count)
{
    if (count > RSX_DSP_NUM_REGS)
        count = RSX_DSP_NUM_REGS;
    memcpy(rsx->regs, regs, count * sizeof(u32));
}

void rsx_dispatch_seed_transform_program(rsx_dispatch* rsx, const u32* words, u32 count)
{
    if (count > RSX_DSP_VP_WORDS)
        count = RSX_DSP_VP_WORDS;
    memcpy(rsx->vp, words, count * sizeof(u32));
}

void rsx_dispatch_seed_transform_constants(rsx_dispatch* rsx, const u32* words, u32 count)
{
    const u32 max_words = RSX_DSP_NUM_CONSTANTS * 4;
    if (count > max_words)
        count = max_words;
    memcpy(rsx->constants, words, count * sizeof(u32));
}

/* Every NV4097_SET_VERTEX_DATA* variant writes the same constant ("current")
 * attribute register a disabled array feeds. Only the 4F form was decoded, so a
 * colour set with SetVertexData4ub read as the (0,0,0,1) default: Tornado
 * Outbreak's UI draws its vertex colour that way, alpha 0 failed the alpha test
 * on every glyph, and 58 draws a frame reached an all-black screen. Fold each
 * variant into the 4F slots rsx_dsp_vertex_default reads. Offsets and packing
 * from the SDK (gcm_implementation_sub.h; x is the LOW byte/half). */
static void vtx_const_set(rsx_dispatch* rsx, u32 attr, u32 c, float v)
{
    u32 w; memcpy(&w, &v, 4);
    const u32 idx = (M_VTX_ATTR_4F + attr * 0x10 + c * 4) >> 2;
    rsx->regs[idx] = w;
    /* Folded packed constants also have an explicit canonical value, even
     * when every component is zero. Immediate snapshots need that presence. */
    rsx->seen[idx]++;
}

static void vtx_const_fold(rsx_dispatch* rsx, u32 m, u32 arg)
{
    float f;
    u32 attr, word;
    if (m >= 0x1E40 && m < 0x1E80) {                  /* DATA1F: (x,0,0,1) */
        attr = (m - 0x1E40) >> 2; memcpy(&f, &arg, 4);
        vtx_const_set(rsx, attr, 0, f); vtx_const_set(rsx, attr, 1, 0.0f);
        vtx_const_set(rsx, attr, 2, 0.0f); vtx_const_set(rsx, attr, 3, 1.0f);
    } else if (m >= 0x1880 && m < 0x1900) {           /* DATA2F: (x,y,0,1) */
        /* RPCS3 push_vertex_data sets the attribute's size to 2 on every
         * DATA2F write (NV47/HW/common.cpp:18-36), so z/w read 0/1 whichever
         * word was written; RSX_VTX_ZW_RESET=0 resets them on word 0 only. */
        attr = (m - 0x1880) >> 3; word = ((m - 0x1880) >> 2) & 1; memcpy(&f, &arg, 4);
        vtx_const_set(rsx, attr, word, f);
        if (word == 0 || dsp_env_on("RSX_VTX_ZW_RESET", &s_env_zw_reset)) {
            vtx_const_set(rsx, attr, 2, 0.0f); vtx_const_set(rsx, attr, 3, 1.0f);
        }
    } else if (m >= 0x1500 && m < 0x1600) {           /* DATA3F: (x,y,z,1) */
        attr = (m - 0x1500) >> 4; word = ((m - 0x1500) >> 2) & 3; memcpy(&f, &arg, 4);
        if (word < 3) vtx_const_set(rsx, attr, word, f);
        if (word == 0 || dsp_env_on("RSX_VTX_ZW_RESET", &s_env_zw_reset))
            vtx_const_set(rsx, attr, 3, 1.0f);
    } else if (m >= 0x1900 && m < 0x1940) {           /* DATA2S: (x,y,0,1), unnormalised */
        attr = (m - 0x1900) >> 2;
        vtx_const_set(rsx, attr, 0, (float)(int16_t)(arg & 0xFFFF));
        vtx_const_set(rsx, attr, 1, (float)(int16_t)(arg >> 16));
        vtx_const_set(rsx, attr, 2, 0.0f); vtx_const_set(rsx, attr, 3, 1.0f);
    } else if (m >= 0x1940 && m < 0x1980) {           /* DATA4UB: normalised bytes */
        attr = (m - 0x1940) >> 2;
        for (u32 c = 0; c < 4; c++)
            vtx_const_set(rsx, attr, c, (float)((arg >> (8 * c)) & 0xFF) / 255.0f);
    } else if ((m >= 0x1980 && m < 0x1A00) ||         /* DATA4S: unnormalised */
               (m >= 0x0A80 && m < 0x0B00)) {         /* DATA_SCALED4S: normalised */
        /* RPCS3 stores SCALED4S as s1 (set_vertex_data_scaled4s_m), decoded
         * (v + 0.5) / 32767.5; DATA4S is s32k, unnormalised. */
        const int s1 = m < 0x1980;
        const u32 base = s1 ? 0x0A80u : 0x1980u;
        attr = (m - base) >> 3; word = ((m - base) >> 2) & 1;
        const float x = (float)(int16_t)(arg & 0xFFFF);
        const float y = (float)(int16_t)(arg >> 16);
        vtx_const_set(rsx, attr, word * 2 + 0, s1 ? (x + 0.5f) / 32767.5f : x);
        vtx_const_set(rsx, attr, word * 2 + 1, s1 ? (y + 0.5f) / 32767.5f : y);
    }
}

/* ---- draw clause (RPCS3 NV47/FW/draw_call.{hpp,cpp}) ---------------------- */

static int dsp_is_draw_parameter(u32 m)
{
    /* nv4097 set_vertex_array_offset / set_vertex_base_offset /
     * set_index_base_offset / set_transform_constant_load push a barrier via
     * util::push_draw_parameter_change; set_transform_constant defers its
     * writes as a transform_constant_update_barrier. */
    return (m >= M_VTXBUF_OFFSET && m < M_VTXBUF_OFFSET + 16 * 4) ||
           m == M_VERTEX_DATA_BASE || m == M_VB_ELEMENT_BASE ||
           m == M_VP_UPLOAD_CONST_ID ||
           (m >= M_VP_UPLOAD_CONST && m < M_VP_UPLOAD_CONST + 32 * 4);
}

static void dsp_clause_reset(rsx_dispatch* rsx)
{
    rsx->clause_command = 0;
    rsx->clause_n_ranges = 0;
    rsx->clause_cur = 0;
    rsx->clause_last_exec = 0;
    rsx->clause_n_barriers = 0;
}

/* Run every range as its own sub-draw, applying each deferred parameter
 * write just before the range it was recorded against (RPCS3 end():
 * do { emit_geometry } while (draw_call.next()), each pass executing
 * execute_pipeline_dependencies). A trailing empty range only carries
 * barriers (post_execute_cleanup). The caller has cleared in_begin_end, so
 * the replayed writes take effect instead of being deferred again. The
 * first range reuses the BEGIN the sink already saw. */
static void dsp_clause_execute(rsx_dispatch* rsx, int deliver_pending)
{
    u32 b = 0;
    int began = 1;
    for (u32 i = 0; i < rsx->clause_n_ranges; i++) {
        while (b < rsx->clause_n_barriers && rsx->clause_barriers[b].draw_id <= i) {
            rsx_dispatch_method(rsx, rsx->clause_barriers[b].method,
                                rsx->clause_barriers[b].arg);
            b++;
        }
        const u32 first = rsx->clause_ranges[i].first;
        const u32 count = rsx->clause_ranges[i].count;
        if (!count) continue;
        if (!began && rsx->sink.begin)
            rsx->sink.begin(rsx->sink.user, rsx, rsx->current_primitive);
        began = 0;
        if (rsx->clause_command == 2) {
            if (rsx->sink.draw_index_array)
                rsx->sink.draw_index_array(rsx->sink.user, rsx, first, count);
        } else if (rsx->sink.draw_arrays) {
            rsx->sink.draw_arrays(rsx->sink.user, rsx, first, count);
        }
        if (deliver_pending && i == 0) {
            /* ARRAY_ELEMENT/INLINE data in the same group: hand it over as
             * before; the sink decides whether it is used (command none). */
            if (rsx->inline_len && rsx->sink.inline_array)
                rsx->sink.inline_array(rsx->sink.user, rsx,
                                       dsp_inline_ptr(rsx), rsx->inline_len);
            if (rsx->elem_count && rsx->elem_count != RSX_DSP_ELEMENT_DROPPED &&
                rsx->sink.array_elements)
                rsx->sink.array_elements(rsx->sink.user, rsx,
                                         dsp_elem_ptr(rsx), rsx->elem_count);
        }
        if (rsx->sink.end)
            rsx->sink.end(rsx->sink.user, rsx);
    }
    for (; b < rsx->clause_n_barriers; b++)
        rsx_dispatch_method(rsx, rsx->clause_barriers[b].method,
                            rsx->clause_barriers[b].arg);
    rsx->clause_prev_command = rsx->clause_command;
    rsx->clause_prev_n_ranges = rsx->clause_n_ranges;
    rsx->clause_prev_n_barriers = rsx->clause_n_barriers;
    dsp_clause_reset(rsx);
}

/* A clause that outgrew the fixed storage is drawn as far as it got and a
 * fresh one is started under the same BEGIN. */
static void dsp_clause_spill(rsx_dispatch* rsx)
{
    const int in_be = rsx->in_begin_end;
    rsx->clause_overflows++;
    rsx->in_begin_end = 0;
    dsp_clause_execute(rsx, 0);
    rsx->in_begin_end = in_be;
    if (in_be && rsx->sink.begin)
        rsx->sink.begin(rsx->sink.user, rsx, rsx->current_primitive);
}

/* draw_clause::append */
static void dsp_clause_append(rsx_dispatch* rsx, u32 first, u32 count)
{
    if (rsx->clause_n_ranges) {
        u32* last_first = &rsx->clause_ranges[rsx->clause_cur].first;
        u32* last_count = &rsx->clause_ranges[rsx->clause_cur].count;
        if (*last_count == 0) {
            /* Execution barrier placeholder. */
            *last_first = first;
            *last_count = count;
            return;
        }
        if (*last_first + *last_count == first) {
            *last_count += count;
            return;
        }
        for (u32 i = rsx->clause_last_exec; i < rsx->clause_n_ranges; i++) {
            if (rsx->clause_ranges[i].first == first &&
                rsx->clause_ranges[i].count == count)
                return;  /* duplicate request */
            if (rsx->clause_ranges[i].first > first) {
                if (rsx->clause_n_ranges >= RSX_DSP_CLAUSE_MAX_RANGES) break;
                memmove(&rsx->clause_ranges[i + 1], &rsx->clause_ranges[i],
                        (rsx->clause_n_ranges - i) * sizeof(rsx->clause_ranges[0]));
                rsx->clause_ranges[i].first = first;
                rsx->clause_ranges[i].count = count;
                rsx->clause_n_ranges++;
                rsx->clause_cur = i;
                for (u32 k = 0; k < rsx->clause_n_barriers; k++)
                    if (rsx->clause_barriers[k].draw_id >= i)
                        rsx->clause_barriers[k].draw_id++;
                return;
            }
        }
    }
    if (rsx->clause_n_ranges >= RSX_DSP_CLAUSE_MAX_RANGES)
        dsp_clause_spill(rsx);
    rsx->clause_cur = rsx->clause_n_ranges;
    rsx->clause_ranges[rsx->clause_n_ranges].first = first;
    rsx->clause_ranges[rsx->clause_n_ranges].count = count;
    rsx->clause_n_ranges++;
}

/* draw_clause::insert_command_barrier for an execution dependency. */
static void dsp_clause_barrier(rsx_dispatch* rsx, u32 method, u32 arg)
{
    if (rsx->clause_n_barriers >= RSX_DSP_CLAUSE_MAX_BARRIERS ||
        (rsx->clause_ranges[rsx->clause_n_ranges - 1].count > 0 &&
         rsx->clause_n_ranges >= RSX_DSP_CLAUSE_MAX_RANGES)) {
        dsp_clause_spill(rsx);
        rsx_dispatch_method(rsx, method, arg);  /* clause now empty: applies */
        return;
    }
    if (rsx->clause_ranges[rsx->clause_n_ranges - 1].count > 0) {
        rsx->clause_cur = rsx->clause_n_ranges;
        rsx->clause_ranges[rsx->clause_n_ranges].first = 0;
        rsx->clause_ranges[rsx->clause_n_ranges].count = 0;
        rsx->clause_n_ranges++;
    } else {
        rsx->clause_cur = rsx->clause_n_ranges - 1;
    }
    rsx->clause_barriers[rsx->clause_n_barriers].draw_id = rsx->clause_cur;
    rsx->clause_barriers[rsx->clause_n_barriers].method = method;
    rsx->clause_barriers[rsx->clause_n_barriers].arg = arg;
    rsx->clause_n_barriers++;
    rsx->clause_last_exec = rsx->clause_cur;
}

void rsx_dispatch_method(rsx_dispatch* rsx, u32 method, u32 arg)
{
    method &= 0xFFFFC;
    const u32 idx = method >> 2;
    /* regs[] covers methods 0..0xFFFC (subchannel bits included). A larger
     * value is not a method -- and indexing with it wrote past regs[] into
     * vp[] and constants[]: Tornado Outbreak's c[461] held two copies of a
     * "load constant 461" command header, i.e. FIFO words stored as shader
     * constants. Drop it, and say what it was. */
    if (idx >= RSX_DSP_NUM_REGS) {
        static int n = 0;
        if (PS3_LOG_FIRST(n, 8))
            fprintf(stderr, "[rsx-dispatch] method 0x%05X (arg 0x%08X) out of range -- dropped\n",
                    method, arg);
        return;
    }
    /* FIFO recovery abandons the open draw: RPCS3 recover_fifo ->
     * discard_draw, which drops the clause without drawing it and leaves
     * BEGIN (RSXThread.cpp:1764-1771, RSXCoreRenderer.cpp:701-716). */
    if (method == RSX_DSP_DISCARD_DRAW) {
        rsx->in_begin_end = 0;
        rsx->inline_len = 0;
        rsx->elem_count = 0;
        dsp_clause_reset(rsx);
        return;
    }
    /* A draw parameter changed after the clause holds a range is deferred
     * (the register keeps its old value) and re-applied between sub-draws:
     * RPCS3 util::push_draw_parameter_change / set_transform_constant. A
     * transform constant write that lands out of bounds is a plain NOP. */
    if (rsx->in_begin_end && rsx->clause_n_ranges && dsp_is_draw_parameter(method)) {
        if (method >= M_VP_UPLOAD_CONST) {
            const u32 load = rsx->regs[M_VP_UPLOAD_CONST_ID >> 2];
            if (load >= RSX_DSP_MAX_TRANSFORM_CONSTANTS ||
                load * 4u + ((method - M_VP_UPLOAD_CONST) >> 2) >=
                    RSX_DSP_MAX_TRANSFORM_CONSTANTS * 4u)
                return;
        }
        dsp_clause_barrier(rsx, method, arg);
        return;
    }
    /* RPCS3 rollback (nv4097.cpp set_color_mask/set_stencil_op/
     * set_face_property/set_blend_equation/set_blend_factor/set_shading_mode/ keeps the old value. A write equal to the latch is
     * accepted unchecked, as there. RSX_ENUM_ROLLBACK=0 stores it raw. */
    if (method < 0x2000u && arg != rsx->regs[idx] && dsp_enum_invalid(method, arg) &&
        dsp_env_on("RSX_ENUM_ROLLBACK", &s_env_enum_rollback)) {
        static int n = 0;
        if (PS3_LOG_FIRST(n, 8))
            fprintf(stderr, "[rsx-dispatch] invalid value 0x%08X for method 0x%04X -- kept 0x%08X\n",
                    arg, method, rsx->regs[idx]);
        return;
    }
    rsx->seen[idx]++;
    const u32 old_value = rsx->regs[idx];
    rsx->regs[idx] = arg;
    if (old_value != arg && method < 0x2000u && rsx->sink.reg_changed)
        rsx->sink.reg_changed(rsx->sink.user, rsx, method, old_value, arg);
    if ((method >= 0x1500 && method < 0x1A00) || (method >= 0x1E40 && method < 0x1E80) ||
        (method >= 0x0A80 && method < 0x0B00))
        vtx_const_fold(rsx, method, arg);

    /* Completing a floating-point position emits a register-based vertex.
     * Other attributes update the current values copied with that position.
     * Keep these writes as ordinary constant state outside BEGIN/END. */
    /* RPCS3 append_to_push_buffer drops a push for an attribute outside
     * VERTEX_ATTRIB_INPUT_MASK (0x1FF0) and counts vertices on ATTR0
     * (RSXDrawCommands.cpp:268-288): every trigger below is ATTR0, so a
     * masked ATTR0 pushes nothing. RSX_IMM_INPUT_MASK=0 ignores the mask. */
    if (rsx->in_begin_end && rsx->sink.immediate_vertex &&
        (method == 0x1508 /* 3F z */ || method == 0x1C0C /* 4F w */ ||
         method == 0x1884 /* 2F y */ || method == 0x1E40 /* 1F x */ ||
         method == 0x1900 /* 2S */   || method == 0x1940 /* 4UB */ ||
         method == 0x1984 /* 4S zw */ || method == 0x0A84 /* SCALED4S zw */) &&
        ((rsx->regs[0x1FF0 >> 2] & 1u) || !dsp_env_on("RSX_IMM_INPUT_MASK", &s_env_imm_mask)))
        rsx->sink.immediate_vertex(rsx->sink.user, rsx);

    /* Transform program upload window: word goes to instruction slot
     * VP_UPLOAD_FROM_ID; the load pointer advances after every completed
     * vec4 (hardware auto-increment, confirmed against the RPCS3 oracle).
     * Past the 544-instruction store the write is a NOP and the load
     * pointer stays put (RPCS3 set_transform_program, nv4097.cpp:168-243). */
    if (method >= M_VP_UPLOAD_INST && method < M_VP_UPLOAD_INST + 32 * 4) {
        const u32 lane = ((method - M_VP_UPLOAD_INST) >> 2) & 3;
        u32* load = &rsx->regs[M_VP_UPLOAD_FROM_ID >> 2];
        const u32 pos = *load * 4 + lane;
        if (pos < RSX_DSP_VP_WORDS) {
            rsx->vp[pos] = arg;
            if (lane == 3)
                (*load)++;
        }
        return;
    }

    /* Transform constant upload window: word i of the burst writes
     * constants[VP_UPLOAD_CONST_ID + i/4][i%4]; the load pointer does NOT
     * advance. */
    if (method >= M_VP_UPLOAD_CONST && method < M_VP_UPLOAD_CONST + 32 * 4) {
        const u32 word = (method - M_VP_UPLOAD_CONST) >> 2;
        const u32 slot = rsx->regs[M_VP_UPLOAD_CONST_ID >> 2] + (word >> 2);
        /* RPCS3 compute_write_range: past constant 467 the write is a NOP. */
        if (slot < RSX_DSP_MAX_TRANSFORM_CONSTANTS)
            rsx->constants[slot][word & 3] = arg;
        /* RSX_CONST_LOG=<lo>,<hi>: every transform-constant upload into that
         * slot range (first 400) -- what a shader's constants really were. */
        { static int lo = -2, hi = 0, n = 0;
          if (lo == -2) { const char* e = getenv("RSX_CONST_LOG"); lo = -1;
                          if (e) sscanf(e, "%d,%d", &lo, &hi); }
          if (lo >= 0 && (int)slot >= lo && (int)slot <= hi && n < 400) { n++;
              fprintf(stderr, "[const-load] c[%u].%c = 0x%08X (method 0x%04X id=%u)\n", slot,
                      "xyzw"[word & 3], arg, method, rsx->regs[M_VP_UPLOAD_CONST_ID >> 2]); } }
        return;
    }

    switch (method) {
    case M_CLEAR_BUFFERS:
        if (rsx->sink.clear)
            rsx->sink.clear(rsx->sink.user, rsx, arg);
        break;

    case M_VERTEX_BEGIN_END:
        /* RPCS3 set_begin_end: only the low byte is the primitive. An
         * unknown primitive enters BEGIN without resetting the clause. */
        if (arg & 0xFFu) {
            if ((arg & 0xFFu) > 10u && dsp_env_on("RSX_BEGIN_INVALID_KEEP", &s_env_begin_keep)) {
                /* RPCS3 set_begin_end (nv4097.cpp:483-497): an unknown
                 * primitive enters BEGIN without draw_clause::reset, so END
                 * re-runs the clause (and primitive) still held -- the one
                 * the last END executed when nothing was appended since. */
                rsx->in_begin_end = 1;
                rsx->inline_len = 0;
                rsx->elem_count = 0;
                if (!rsx->clause_n_ranges && rsx->clause_prev_n_ranges) {
                    rsx->clause_command = rsx->clause_prev_command;
                    rsx->clause_n_ranges = rsx->clause_prev_n_ranges;
                    rsx->clause_n_barriers = rsx->clause_prev_n_barriers;
                    rsx->clause_cur = rsx->clause_n_ranges - 1;
                    rsx->clause_last_exec = 0;
                }
                if (rsx->sink.begin)
                    rsx->sink.begin(rsx->sink.user, rsx, rsx->current_primitive);
                break;
            }
            rsx->in_begin_end = 1;
            /* RSX_BEGIN_INVALID_KEEP=0: an unknown primitive resets the
             * clause and draws as primitive 0, which every sink drops. */
            rsx->current_primitive = (arg & 0xFFu) <= 10u ? (arg & 0xFFu) : 0u;
            rsx->inline_len = 0;
            rsx->elem_count = 0;
            dsp_clause_reset(rsx);
            if (rsx->sink.begin)
                rsx->sink.begin(rsx->sink.user, rsx, rsx->current_primitive);
        } else {
            rsx->in_begin_end = 0;
            if (rsx->clause_n_ranges) {
                dsp_clause_execute(rsx, 1);
                rsx->inline_len = 0;
                rsx->elem_count = 0;
                break;
            }
            /* Hand an inline stream over BEFORE end: the sink reads end as
             * "draw what you have", so a batch delivered after it would be
             * drawn one primitive late, or not at all. */
            if (rsx->inline_len && rsx->sink.inline_array)
                rsx->sink.inline_array(rsx->sink.user, rsx,
                                       dsp_inline_ptr(rsx), rsx->inline_len);
            rsx->inline_len = 0;
            if (rsx->elem_count && rsx->elem_count != RSX_DSP_ELEMENT_DROPPED &&
                rsx->sink.array_elements)
                rsx->sink.array_elements(rsx->sink.user, rsx,
                                         dsp_elem_ptr(rsx), rsx->elem_count);
            rsx->elem_count = 0;
            if (rsx->sink.end)
                rsx->sink.end(rsx->sink.user, rsx);
        }
        break;

    /* INLINE_ARRAY: the vertex stream travels IN the pushbuffer rather than
     * being fetched from a vertex array, so there is no (first, count) --
     * every word between BEGIN_END(prim) and BEGIN_END(0) is vertex data.
     * The layout is still the ordinary VTXFMT declaration: enabled
     * attributes packed in ascending index order, one vertex every stride
     * bytes. Store the words back in guest byte order so the consumer
     * decodes them with the same big-endian readers a vertex array uses;
     * the dispatcher never interprets them.
     *
     * A title that builds geometry this way (Guitar Hero III draws its
     * entire 2D layer as inline QUADS) previously issued draws that arrived
     * carrying no vertices at all: the backend saw BEGIN/END with nothing
     * between them and counted an empty group. */
    case M_INLINE_ARRAY: {
        u8* dst = dsp_inline_room(rsx, rsx->inline_len + 4u);
        if (dst) {
            dst[rsx->inline_len + 0] = (u8)(arg >> 24);
            dst[rsx->inline_len + 1] = (u8)(arg >> 16);
            dst[rsx->inline_len + 2] = (u8)(arg >> 8);
            dst[rsx->inline_len + 3] = (u8)arg;
            rsx->inline_len += 4;
        } else if (rsx->inline_len) {
            /* Truncating leaves a torn final vertex; drop the whole stream
             * and count it instead of drawing something half-read. */
            rsx->inline_len = 0;
            rsx->inline_dropped++;
        }
        break;
    }

    /* ARRAY_ELEMENT16 packs two u16 indices, low half first; 32 carries one.
     * Outside BEGIN they are ignored (RPCS3 nv4097::set_array_element*). */
    case M_ARRAY_ELEMENT16:
    case M_ARRAY_ELEMENT32:
        if (rsx->in_begin_end && rsx->elem_count != RSX_DSP_ELEMENT_DROPPED) {
            const u32 n = method == M_ARRAY_ELEMENT16 ? 2u : 1u;
            u32* dst = dsp_elem_room(rsx, rsx->elem_count + n);
            if (dst) {
                if (n == 2u) {
                    dst[rsx->elem_count++] = arg & 0xFFFFu;
                    dst[rsx->elem_count++] = arg >> 16;
                } else {
                    dst[rsx->elem_count++] = arg;
                }
            } else {
                /* A truncated list would draw a partial primitive: drop the
                 * whole group's list and count it. */
                rsx->elem_count = RSX_DSP_ELEMENT_DROPPED;
                rsx->elem_dropped++;
            }
        }
        break;

    case M_VB_VERTEX_BATCH:
        rsx->clause_command = 1;
        dsp_clause_append(rsx, arg & 0xFFFFFF, (arg >> 24) + 1);
        break;

    case M_VB_INDEX_BATCH:
        rsx->clause_command = 2;
        dsp_clause_append(rsx, arg & 0xFFFFFF, (arg >> 24) + 1);
        break;

    case M_GCM_FLIP_COMMAND:
        if (dsp_flip_reset_enabled())
            rsx_dispatch_reset_state(rsx);
        break;

    case M_GCM_FLIP_HEAD_0:
    case M_GCM_FLIP_HEAD_1:
    case M_GCM_DRIVER_FLIP:
        if (rsx->sink.flip)
            rsx->sink.flip(rsx->sink.user, rsx, arg);
        break;

    default:
        break;
    }
}

/* ---- getters ----------------------------------------------------------- */

u32 rsx_dsp_clear_color(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_CLEAR_COLOR_VALUE);
}

u32 rsx_dsp_clear_zstencil(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_CLEAR_DEPTH_VALUE);
}

void rsx_dsp_get_surface(const rsx_dispatch* rsx, rsx_dsp_surface* out)
{
    const u32 fmt = rsx_dsp_reg(rsx, M_RT_FORMAT);
    out->color_format = fmt & 0x1F;
    out->depth_format = (fmt >> 5) & 7;
    out->raster_type  = (fmt >> 8) & 0xF;

    const u32 h = rsx_dsp_reg(rsx, M_RT_HORIZ);
    const u32 v = rsx_dsp_reg(rsx, M_RT_VERT);
    out->clip_x = h & 0xFFFF;
    out->clip_w = h >> 16;
    out->clip_y = v & 0xFFFF;
    out->clip_h = v >> 16;
    /* Antialiased surfaces at their sample resolution (rsx_dispatch.h). A
     * texture that reads an AA surface's memory sees the sample grid: a
     * 320x180 SQUARE_ROTATED_4 target is a 640x360 image. */
    out->antialias = (fmt >> 12) & 0xF;
    out->aa_sx = (out->antialias >= 3 && out->antialias <= 5) ? 2u : 1u;
    out->aa_sy = (out->antialias == 4 || out->antialias == 5) ? 2u : 1u;
    out->clip_x *= out->aa_sx; out->clip_w *= out->aa_sx;
    out->clip_y *= out->aa_sy; out->clip_h *= out->aa_sy;

    out->color_offset[0] = rsx_dsp_reg(rsx, M_COLOR0_OFFSET);
    out->color_offset[1] = rsx_dsp_reg(rsx, M_COLOR1_OFFSET);
    out->color_offset[2] = rsx_dsp_reg(rsx, M_COLOR2_OFFSET);
    out->color_offset[3] = rsx_dsp_reg(rsx, M_COLOR3_OFFSET);
    out->color_pitch[0]  = rsx_dsp_reg(rsx, M_COLOR0_PITCH);
    out->color_pitch[1]  = rsx_dsp_reg(rsx, M_COLOR1_PITCH);
    out->color_pitch[2]  = rsx_dsp_reg(rsx, M_COLOR2_PITCH);
    out->color_pitch[3]  = rsx_dsp_reg(rsx, M_COLOR3_PITCH);
    out->color_location[0] = dma_to_location(rsx_dsp_reg(rsx, M_DMA_COLOR0));
    out->color_location[1] = dma_to_location(rsx_dsp_reg(rsx, M_DMA_COLOR1));
    out->color_location[2] = dma_to_location(rsx_dsp_reg(rsx, M_DMA_COLOR2));
    out->color_location[3] = dma_to_location(rsx_dsp_reg(rsx, M_DMA_COLOR3));
    out->color_target  = rsx_dsp_reg(rsx, M_RT_ENABLE);
    out->zeta_offset   = rsx_dsp_reg(rsx, M_ZETA_OFFSET);
    out->zeta_pitch    = rsx_dsp_reg(rsx, M_ZETA_PITCH);
    out->zeta_location = dma_to_location(rsx_dsp_reg(rsx, M_DMA_ZETA));

    u32 ct;
    switch (out->color_format) {
    case 1: case 2: case 3: case 0xA: ct = 2; break;
    case 9: ct = 1; break;
    case 0xB: ct = 8; break;
    case 0xC: ct = 16; break;
    default: ct = 4; break;
    }
    const u32 zt = (out->depth_format == 1) ? 2u : 4u;

    /* RPCS3 get_framebuffer_layout actual_color_pitch / actual_zeta_pitch: a
     * swizzled raster is tightly packed, its pitch the clip row whatever the
     * pitch registers hold (R2's Z16 shadow maps: pitch 64, 1024 wide, so
     * 2048-byte rows). The surface store sees that pitch, and so the memory
     * the surface really covers. */
    if (out->raster_type == 2u && out->clip_w) {
        for (u32 i = 0; i < 4; i++) out->color_pitch[i] = out->clip_w * ct;
        out->zeta_pitch = out->clip_w * zt;
    }

    /* SET_WINDOW_OFFSET (0x02B8) moves the raster origin, which is how RPCS3
     * (RSXGraphEngine get_framebuffer_layout) treats it: every colour address
     * gains pitch * y + texel * x and zeta its own, texel sizes scaled by the
     * horizontal AA factor. */
    const u32 wo = rsx_dsp_reg(rsx, 0x02B8);
    if (wo) {
        const u32 wx = wo & 0xFFFF, wy = wo >> 16;
        for (u32 i = 0; i < 4; i++)
            out->color_offset[i] += out->color_pitch[i] * wy + ct * out->aa_sx * wx;
        out->zeta_offset += out->zeta_pitch * wy + zt * out->aa_sx * wx;
    }
}

void rsx_dsp_get_viewport(const rsx_dispatch* rsx, rsx_dsp_viewport* out)
{
    const u32 h = rsx_dsp_reg(rsx, M_VIEWPORT_HORIZ);
    const u32 v = rsx_dsp_reg(rsx, M_VIEWPORT_VERT);
    out->x = h & 0xFFFF;
    out->w = h >> 16;
    out->y = v & 0xFFFF;
    out->h = v >> 16;
    for (u32 i = 0; i < 4; i++) {
        const u32 t = rsx_dsp_reg(rsx, M_VIEWPORT_TRANSLATE + i * 4);
        const u32 s = rsx_dsp_reg(rsx, M_VIEWPORT_SCALE + i * 4);
        memcpy(&out->translate[i], &t, 4);
        memcpy(&out->scale[i], &s, 4);
    }
}

void rsx_dsp_get_vertex_attr(const rsx_dispatch* rsx, u32 index, rsx_dsp_vertex_attr* out)
{
    memset(out, 0, sizeof(*out));
    if (index >= RSX_DSP_NUM_VERTEX_ATTR)
        return;
    const u32 fmt = rsx_dsp_reg(rsx, M_VTXFMT + index * 4);
    const u32 off = rsx_dsp_reg(rsx, M_VTXBUF_OFFSET + index * 4);
    out->type      = fmt & 7;
    out->size      = (fmt >> 4) & 0xF;
    out->stride    = (fmt >> 8) & 0xFF;
    out->frequency = fmt >> 16;
    out->offset    = off & 0x7FFFFFFF;
    out->location  = off >> 31;
}

void rsx_dsp_get_texture(const rsx_dispatch* rsx, u32 unit, rsx_dsp_texture* out)
{
    memset(out, 0, sizeof(*out));
    if (unit >= RSX_DSP_NUM_TEXTURES)
        return;
    const u32 base = M_TEX_OFFSET + unit * 0x20;
    const u32 fmt  = rsx_dsp_reg(rsx, base + 0x04);
    /* The offset is 31 bits; bit 31 is not part of it (RPCS3 masks it with 0x7FFFFFFF for fragment and vertex
     * textures). Taken whole, a main-memory texture bound at 0x82A22900 missed the IO lookup. The location comes from
     * the format word alone. */
    out->offset    = rsx_dsp_reg(rsx, base) & 0x7FFFFFFFu;
    out->location  = (fmt & 3) == 2 ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
    out->cubemap   = (fmt >> 2) & 1;
    out->border    = !((fmt >> 3) & 1);
    out->dimension = (fmt >> 4) & 0xF;
    out->format    = (fmt >> 8) & 0xFF;
    out->mipmaps   = fmt >> 16;
    out->wrap      = rsx_dsp_reg(rsx, base + 0x08);
    out->control0  = rsx_dsp_reg(rsx, base + 0x0C);
    out->enabled   = out->control0 >> 31;
    out->remap     = rsx_dsp_reg(rsx, base + 0x10);
    out->filter    = rsx_dsp_reg(rsx, base + 0x14);
    const u32 rect = rsx_dsp_reg(rsx, base + 0x18);
    out->width     = rect >> 16;
    /* RPCS3 fragment_texture::height/depth: a 1D image is one row high and
     * only a 3D one has depth, whatever the registers hold. */
    out->height    = out->dimension == 1 ? 1u : (rect & 0xFFFF);
    out->border_color = rsx_dsp_reg(rsx, base + 0x1C);
    const u32 sz1  = rsx_dsp_reg(rsx, M_TEX_SIZE1 + unit * 4);
    out->pitch     = sz1 & 0xFFFFF;
    out->depth     = out->dimension == 3 ? (sz1 >> 20) : 1u;
}

void rsx_dsp_get_vertex_texture(const rsx_dispatch* rsx, u32 unit,
                                rsx_dsp_vertex_texture* out)
{
    memset(out, 0, sizeof(*out));
    if (unit >= RSX_DSP_NUM_VERTEX_TEXTURES)
        return;
    const u32 base = M_VERTEX_TEXTURE + unit * 0x20;
    const u32 fmt  = rsx_dsp_reg(rsx, base + 0x04);
    out->offset    = rsx_dsp_reg(rsx, base + 0x00) & 0x7FFFFFFFu;   /* 31-bit offset, as above */
    out->location  = (fmt & 3) == 2 ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
    out->cubemap   = (fmt >> 2) & 1;
    out->dimension = (fmt >> 4) & 0xF;
    out->format    = (fmt >> 8) & 0xFF;
    out->mipmaps   = fmt >> 16;
    out->wrap      = rsx_dsp_reg(rsx, base + 0x08);
    out->control0  = rsx_dsp_reg(rsx, base + 0x0C);
    out->enabled   = out->control0 >> 31;
    const u32 ctl3 = rsx_dsp_reg(rsx, base + 0x10);
    out->pitch     = ctl3 & 0xFFFFF;
    out->depth     = out->dimension == 3 ? (ctl3 >> 20) : 1u;   /* RPCS3 vertex_texture */
    out->filter    = rsx_dsp_reg(rsx, base + 0x14);
    const u32 rect = rsx_dsp_reg(rsx, base + 0x18);
    out->width     = rect >> 16;
    out->height    = out->dimension == 1 ? 1u : (rect & 0xFFFF);
    out->border_color = rsx_dsp_reg(rsx, base + 0x1C);
}

u32 rsx_dsp_vertex_data_base_offset(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_VERTEX_DATA_BASE);
}

u32 rsx_dsp_vertex_data_base_index(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_VB_ELEMENT_BASE);
}

u32 rsx_dsp_fragment_program(const rsx_dispatch* rsx, u32* location)
{
    const u32 v = rsx_dsp_reg(rsx, M_FP_ACTIVE_PROGRAM);
    if (location)
        *location = (v & 3) == 2 ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
    return v & ~3u;
}

u32 rsx_dsp_shader_control(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_FP_CONTROL);
}

u32 rsx_dsp_vp_start(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_VP_START_FROM_ID);
}

int rsx_dsp_restart_index_enabled(const rsx_dispatch* rsx, int index_is_u32)
{
    if (!rsx_dsp_reg(rsx, M_RESTART_INDEX_ENABLE))
        return 0;
    /* RPCS3 oracle (rsx_methods.h restart_index_enabled()): a 16-bit index
     * buffer disables restart entirely if the configured sentinel doesn't
     * fit in 16 bits, rather than truncating it. */
    if (!index_is_u32 && rsx_dsp_reg(rsx, M_RESTART_INDEX) > 0xFFFFu)
        return 0;
    return 1;
}

u32 rsx_dsp_restart_index(const rsx_dispatch* rsx)
{
    return rsx_dsp_reg(rsx, M_RESTART_INDEX);
}

void rsx_dsp_vertex_default(const rsx_dispatch* rsx, u32 index, float out[4])
{
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (index >= RSX_DSP_NUM_VERTEX_ATTR)
        return;
    u32 w[4];
    u32 any = 0;
    for (u32 i = 0; i < 4; i++) {
        w[i] = rsx_dsp_reg(rsx, M_VTX_ATTR_4F + index * 0x10 + i * 4);
        any |= w[i];
    }
    if (!any)
        return;   /* never written: hardware default (0,0,0,1) */
    memcpy(out, w, 16);
}

void rsx_dsp_get_index_array(const rsx_dispatch* rsx, rsx_dsp_index_array* out)
{
    out->offset   = rsx_dsp_reg(rsx, M_IDXBUF_OFFSET);
    const u32 fmt = rsx_dsp_reg(rsx, M_IDXBUF_FORMAT);
    out->location = fmt & 0xF;               /* location index directly     */
    out->is_u32   = ((fmt >> 4) & 0xFF) == 0; /* type 0 = u32, 1 = u16      */
}
