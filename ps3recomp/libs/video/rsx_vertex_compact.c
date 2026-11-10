#include "rsx_vertex_compact.h"
#include "rsx_commands.h"
#include "rsx_vertex_formats.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

int rsx_vertex_legacy(void)
{
    static int s_legacy = -1;
    if (s_legacy < 0) {
        const char* e = getenv("RSX_VTX_LEGACY");
        s_legacy = (e && *e == '1') ? 1 : 0;
    }
    return s_legacy;
}

/* The element an attribute reads for one reference.
 *
 * RPCS3 uploads each array from the draw's first vertex -- (min index + base
 * index) & 0xFFFFF for an indexed draw, the first array vertex otherwise --
 * and the shader indexes that upload (RSXVertexFetch.glsl read_location,
 * RSXThread.cpp calculate_required_range, VKVertexBuffers.cpp:226-235):
 *   division:  vertex_base / f + (id - min) / f
 *   modulo:    (index + base index) % f indexed, (id - min) % f for arrays,
 *              and only once the draw reaches index f (max_index >= f),
 *              else the plain per-vertex fetch
 *   plain:     vertex_base + (id - min), no 20-bit wrap for arrays. */
static u32 rsx_vertex_plan_element(const rsx_vertex_fetch_plan* plan,
                                   u32 attr, const rsx_vertex_ref* ref)
{
    const u32 freq = plan->attr[attr].desc.frequency;
    const u32 modulo = (plan->divider_mask >> attr) & 1u;
    if (plan->freq_mode == RSX_VERTEX_FREQ_LEGACY)
        return rsx_vertex_element_index(ref->vertex_id, ref->base_index,
                                        freq, modulo);
    const int indexed = plan->freq_mode == RSX_VERTEX_FREQ_RPCS3_INDEXED;
    const u32 rel = ref->vertex_id - plan->draw_min;
    const u32 eb = plan->elem_base[attr];
    if (freq <= 1)
        return eb + rel;
    if (modulo) {
        const u32 vertex_base = indexed
            ? ((plan->draw_min + ref->base_index) & 0x000FFFFFu) : plan->draw_min;
        const u32 max_index = vertex_base + (plan->draw_max - plan->draw_min);
        if (max_index < freq)
            return eb + rel;
        return eb + (indexed ? (ref->vertex_id + ref->base_index) % freq
                             : rel % freq);
    }
    return eb + rel / freq;
}

/* RPCS3 analyse_inputs_interleaved (RSXDrawCommands.cpp:118-180) groups the
 * VP's array inputs, ascending, into blocks of equal stride whose offsets
 * (& 0x7FFFFFFF) lie within one stride of the block's lowest; stride 0 is a
 * block of its own. A block uploads from one first element,
 * calculate_required_range (RSXThread.cpp:63-130): the draw's vertex base,
 * lowered to base / f by a division member and to 0 by an active modulo
 * member, and every member reads relative to it (RSXVertexFetch.glsl). So a
 * plain attribute interleaved with a divided one reads from base / f + rel,
 * which is RPCS3's behaviour, not the hardware's. */
static void rsx_vertex_plan_blocks(rsx_vertex_fetch_plan* plan)
{
    const int indexed = plan->freq_mode == RSX_VERTEX_FREQ_RPCS3_INDEXED;
    struct { u32 stride, base, first; } blk[RSX_DSP_NUM_VERTEX_ATTR];
    u8 blk_of[RSX_DSP_NUM_VERTEX_ATTR];
    u32 n_blk = 0;
    u32 member = 0;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        plan->elem_base[attr] = 0;
        if (!(plan->layout.mask & (1u << attr)) || !fetch->desc.type ||
            !fetch->desc.size || !fetch->desc.stride)
            continue;
        const u32 off = fetch->desc.offset & 0x7FFFFFFFu;
        u32 b = 0;
        for (; b < n_blk; b++) {
            if (blk[b].stride != fetch->desc.stride)
                continue;
            const u32 diff = off > blk[b].base ? off - blk[b].base : blk[b].base - off;
            if (diff > fetch->desc.stride)
                continue;
            if (off < blk[b].base)
                blk[b].base = off;
            break;
        }
        if (b == n_blk) {
            blk[n_blk].stride = fetch->desc.stride;
            blk[n_blk].base = off;
            blk[n_blk].first = UINT_MAX;
            n_blk++;
        }
        blk_of[attr] = (u8)b;
        member |= 1u << attr;
    }
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        if (!(member & (1u << attr)))
            continue;
        /* Every reference of a draw shares one base index. */
        const u32 vertex_base = indexed
            ? ((plan->draw_min + plan->draw_base_index) & 0x000FFFFFu)
            : plan->draw_min;
        const u32 freq = plan->attr[attr].desc.frequency;
        u32 start = vertex_base;
        if (freq > 1) {
            if ((plan->divider_mask >> attr) & 1u) {
                if (vertex_base + (plan->draw_max - plan->draw_min) >= freq)
                    start = 0;
            } else {
                start = vertex_base / freq;
            }
        }
        if (start < blk[blk_of[attr]].first)
            blk[blk_of[attr]].first = start;
    }
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++)
        if (member & (1u << attr))
            plan->elem_base[attr] = blk[blk_of[attr]].first;
}

static float rsx_compact_be_f32(const u8* p)
{
    const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

static float rsx_compact_be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0) {
        /* unpackHalf2x16 (RPCS3) keeps denormals: man * 2^-24. */
        float f = (float)man * (1.0f / 16777216.0f);
        return sign ? -f : f;
    } else if (exp == 31)
        out = sign | 0x7F800000u | (man << 13);
    else
        out = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, sizeof(f));
    return f;
}

int rsx_vertex_decode_element(
    u32 type, u32 size, const u8* source, float out[4])
{
    if (!source || !out)
        return 0;
    /* RPCS3 fetch_attribute: absent components read 0 and go through the
     * same decode (S1 biases them to 0.5/32767.5); w is 1 when size < 4. */
    if (type == RSX_VTX_TYPE_SNORM16)
        out[0] = out[1] = out[2] = out[3] = 0.5f / 32767.5f;
    else
        out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (size < 4)
        out[3] = 1.0f;
    for (u32 component = 0; component < size && component < 4;
         component++) {
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            out[component] = rsx_compact_be_f32(source + component * 4);
            break;
        case RSX_VTX_TYPE_HALF:
            out[component] = rsx_compact_be_f16(source + component * 2);
            break;
        case RSX_VTX_TYPE_UNORM8:
            out[component] = source[component] / 255.0f;
            break;
        case RSX_VTX_TYPE_UINT8:
            out[component] = (float)source[component];
            break;
        case RSX_VTX_TYPE_SNORM16: {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = ((float)value + 0.5f) / 32767.5f;
            break;
        }
        case RSX_VTX_TYPE_SINT16: {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = (float)value;
            break;
        }
        case RSX_VTX_TYPE_CMP32: {
            const u32 word =
                ((u32)source[0] << 24) | ((u32)source[1] << 16) |
                ((u32)source[2] << 8) | (u32)source[3];
            /* RPCS3: sext16(x << 5), sext16(y << 5), sext16(z << 6),
             * all over 32767; w = 1. */
            const s16 x = (s16)(u16)((word & 0x7FFu) << 5);
            const s16 y = (s16)(u16)(((word >> 11) & 0x7FFu) << 5);
            const s16 z = (s16)(u16)(((word >> 22) & 0x3FFu) << 6);
            out[0] = (float)x / 32767.0f;
            out[1] = (float)y / 32767.0f;
            out[2] = (float)z / 32767.0f;
            out[3] = 1.0f;
            return 1;
        }
        default:
            return 0;
        }
    }
    return 1;
}

/* ---- (type, size)-specialised decoders ----------------------------------
 * The same arithmetic as rsx_vertex_decode_element, with the type switch and
 * component count resolved once per attribute per draw (_prepare) instead
 * of per component per vertex. Results are bit-identical: float/NaN words
 * are moved, never converted, and every expression matches the generic
 * decoder's. */

#if defined(__clang__) || defined(__GNUC__)
#define RSX_VC_INLINE static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define RSX_VC_INLINE static __forceinline
#else
#define RSX_VC_INLINE static inline
#endif

#if defined(__clang__) || defined(__GNUC__)
#define RSX_VC_BSWAP32(x) __builtin_bswap32(x)
#define RSX_VC_BSWAP16(x) __builtin_bswap16(x)
#else
#include <stdlib.h>
#define RSX_VC_BSWAP32(x) _byteswap_ulong(x)
#define RSX_VC_BSWAP16(x) _byteswap_ushort(x)
#endif

RSX_VC_INLINE u32 rsx_vc_be32(const u8* p)
{
    u32 w;
    memcpy(&w, p, 4);
    return RSX_VC_BSWAP32(w);
}

RSX_VC_INLINE u16 rsx_vc_be16(const u8* p)
{
    u16 h;
    memcpy(&h, p, 2);
    return (u16)RSX_VC_BSWAP16(h);
}

RSX_VC_INLINE u32 rsx_vc_fbits(float f)
{
    u32 w;
    memcpy(&w, &f, 4);
    return w;
}

RSX_VC_INLINE u32 rsx_vc_half_bits(u16 h)
{
    /* Branchless form of rsx_compact_be_f16: negating the exact denormal
     * product only sets the sign bit, and exponent 31 maps to 255. */
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    const u32 denormal =
        rsx_vc_fbits((float)man * (1.0f / 16777216.0f)) | sign;
    const u32 e = exp == 31 ? 255u : exp + 112u;
    const u32 normal = sign | (e << 23) | (man << 13);
    return exp == 0 ? denormal : normal;
}

/* size is 1..4 (a declared size above 4 decodes as 4). */
RSX_VC_INLINE void rsx_vc_decode(u32 type, u32 size, const u8* s, u8* dst)
{
    u32 w[4];
    if (type == RSX_VTX_TYPE_CMP32) {
        const u32 word = rsx_vc_be32(s);
        const s16 x = (s16)(u16)((word & 0x7FFu) << 5);
        const s16 y = (s16)(u16)(((word >> 11) & 0x7FFu) << 5);
        const s16 z = (s16)(u16)(((word >> 22) & 0x3FFu) << 6);
        w[0] = rsx_vc_fbits((float)x / 32767.0f);
        w[1] = rsx_vc_fbits((float)y / 32767.0f);
        w[2] = rsx_vc_fbits((float)z / 32767.0f);
        w[3] = rsx_vc_fbits(1.0f);
        memcpy(dst, w, 16);
        return;
    }
    const u32 fill = rsx_vc_fbits(
        type == RSX_VTX_TYPE_SNORM16 ? 0.5f / 32767.5f : 0.0f);
    w[0] = w[1] = w[2] = fill;
    w[3] = size < 4 ? rsx_vc_fbits(1.0f) : fill;
    for (u32 c = 0; c < size; c++) {
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            w[c] = rsx_vc_be32(s + c * 4);
            break;
        case RSX_VTX_TYPE_HALF:
            w[c] = rsx_vc_half_bits(rsx_vc_be16(s + c * 2));
            break;
        case RSX_VTX_TYPE_UNORM8:
            w[c] = rsx_vc_fbits(s[c] / 255.0f);
            break;
        case RSX_VTX_TYPE_UINT8:
            w[c] = rsx_vc_fbits((float)s[c]);
            break;
        case RSX_VTX_TYPE_SNORM16:
            w[c] = rsx_vc_fbits(
                ((float)(s16)rsx_vc_be16(s + c * 2) + 0.5f) / 32767.5f);
            break;
        case RSX_VTX_TYPE_SINT16:
            w[c] = rsx_vc_fbits((float)(s16)rsx_vc_be16(s + c * 2));
            break;
        default:
            break;
        }
    }
    memcpy(dst, w, 16);
}

/* How a slot finds its element (see rsx_vertex_plan_element). */
enum {
    RSX_VC_DEFAULT = 0, /* disabled: the register/default value          */
    RSX_VC_PLAIN,       /* RPCS3, frequency 0/1 (or inactive modulo)      */
    RSX_VC_LEGACY,      /* legacy rule, frequency 0/1                     */
    RSX_VC_DIV,         /* RPCS3 division                                 */
    RSX_VC_MOD,         /* RPCS3 array draw, active modulo                */
    RSX_VC_GENERAL      /* rsx_vertex_plan_element                        */
};

/* One decoder and one PLAIN column walker per (type 1..7, size 1..4);
 * decoder id 0 = generic rsx_vertex_decode_element. */
typedef u32 (*rsx_vc_column_fn)(
    const rsx_vertex_fetch_fast* f, const rsx_vertex_ref* refs, u32 i,
    u32 count, u32 draw_min, u8* out, u32 out_stride);

#define RSX_VC_DECODERS(T, S)                                                \
    static u32 rsx_vc_col_##T##_##S(                                         \
        const rsx_vertex_fetch_fast* f, const rsx_vertex_ref* refs, u32 i,   \
        u32 count, u32 draw_min, u8* out, u32 out_stride)                    \
    {                                                                        \
        const u8* base = f->base;                                            \
        const u32 eb = f->elem_base, first = f->first, last = f->last;       \
        const u64 stride = f->stride;                                        \
        for (; i < count; i++) {                                             \
            const u32 el = eb + (refs[i].vertex_id - draw_min);              \
            if (el < first || el > last)                                     \
                return i;                                                    \
            rsx_vc_decode(T, S, base + (u64)(el - first) * stride,           \
                          out + (size_t)i * out_stride);                     \
        }                                                                    \
        return count;                                                        \
    }
#define RSX_VC_DECODERS_T(T)                                                 \
    RSX_VC_DECODERS(T, 1) RSX_VC_DECODERS(T, 2)                              \
    RSX_VC_DECODERS(T, 3) RSX_VC_DECODERS(T, 4)
RSX_VC_DECODERS_T(1)
RSX_VC_DECODERS_T(2)
RSX_VC_DECODERS_T(3)
RSX_VC_DECODERS_T(4)
RSX_VC_DECODERS_T(5)
RSX_VC_DECODERS_T(6)
RSX_VC_DECODERS_T(7)
#define RSX_VC_ROW(T) rsx_vc_col_##T##_1, rsx_vc_col_##T##_2,                \
                      rsx_vc_col_##T##_3, rsx_vc_col_##T##_4
static const rsx_vc_column_fn s_rsx_vc_column[1 + 7 * 4] = {
    NULL, RSX_VC_ROW(1), RSX_VC_ROW(2), RSX_VC_ROW(3), RSX_VC_ROW(4),
    RSX_VC_ROW(5), RSX_VC_ROW(6), RSX_VC_ROW(7)
};
#undef RSX_VC_ROW

/* The decoder table as an inlined jump table: no call per attribute. */
RSX_VC_INLINE void rsx_vc_decode_id(u32 id, const u8* s, u8* dst)
{
#define RSX_VC_CASE(T, S) case 1 + (T - 1) * 4 + (S - 1):                   \
        rsx_vc_decode(T, S, s, dst);                                         \
        return;
#define RSX_VC_CASES(T) RSX_VC_CASE(T, 1) RSX_VC_CASE(T, 2)                  \
                        RSX_VC_CASE(T, 3) RSX_VC_CASE(T, 4)
    switch (id) {
    RSX_VC_CASES(1) RSX_VC_CASES(2) RSX_VC_CASES(3) RSX_VC_CASES(4)
    RSX_VC_CASES(5) RSX_VC_CASES(6) RSX_VC_CASES(7)
    default:
        return;
    }
#undef RSX_VC_CASES
#undef RSX_VC_CASE
}

static u8 rsx_vc_decoder_id(u32 type, u32 size)
{
    if (type < 1 || type > 7 || !size)
        return 0;
    return (u8)(1u + (type - 1u) * 4u + ((size > 4 ? 4u : size) - 1u));
}

void rsx_vertex_layout_plan_init(
    rsx_vertex_layout_plan* plan, u32 input_mask)
{
    if (!plan)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->mask = input_mask & 0xFFFFu;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++)
        plan->slot_by_attr[attr] = -1;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        if (!(plan->mask & (1u << attr)))
            continue;
        const u32 slot = plan->count++;
        plan->attrs[slot] = (u8)attr;
        plan->slot_by_attr[attr] = (s8)slot;
    }
    plan->stride = (u32)plan->count * 16u;
}

static u32 rsx_vertex_input_mask(const rsx_dispatch* rsx)
{
    /* VERTEX_ATTRIB_INPUT_MASK (0x1FF0) resets to 0xFFFF. */
    if (!rsx->seen[0x1FF0 >> 2])
        return 0xFFFFu;
    return rsx_dsp_reg(rsx, 0x1FF0) & 0xFFFFu;
}

/* The VERTEX_DATA4F register (every DATA* variant folds into it). Written
 * zeros are real values; only a never-written register reads (0,0,0,1). */
static void rsx_vertex_register_value(
    const rsx_dispatch* rsx, u32 attr, float out[4])
{
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!rsx || attr >= RSX_DSP_NUM_VERTEX_ATTR)
        return;
    const u32 reg = 0x1C00u + attr * 0x10u;
    u32 written = 0;
    for (u32 c = 0; c < 4; c++)
        written |= rsx->seen[(reg >> 2) + c];
    if (!written) {
        rsx_dsp_vertex_default(rsx, attr, out);
        return;
    }
    for (u32 c = 0; c < 4; c++) {
        const u32 w = rsx_dsp_reg(rsx, reg + c * 4u);
        memcpy(&out[c], &w, 4);
    }
}

void rsx_vertex_fetch_plan_init(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const rsx_vertex_layout_plan* layout,
    rsx_vertex_guest_ptr_fn guest_ptr, void* guest_user)
{
    if (!plan || !rsx || !layout)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->layout = *layout;
    plan->base_offset = rsx_dsp_vertex_data_base_offset(rsx);
    plan->divider_mask = rsx_dsp_reg(rsx, 0x1FC0);
    plan->guest_ptr = guest_ptr;
    plan->guest_user = guest_user;
    /* RPCS3 analyse_inputs_interleaved: an attribute outside
     * VERTEX_ATTRIB_INPUT_MASK (reset value 0xFFFF) is not fed at all and
     * reads (0,0,0,1); inside it, an array with size > 0 wins, else the
     * register value written through VERTEX_DATA*, else (0,0,0,1). */
    const u32 input_mask = rsx_vertex_input_mask(rsx);
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        rsx_dsp_get_vertex_attr(rsx, attr, &fetch->desc);
        /* RPCS3 to_vertex_base_type: type 0 decodes as UB256 (gcm_enums.h
         * "expected(vertex_base_type::ub256)"); an array is in use by its
         * size alone. */
        if (!fetch->desc.type && fetch->desc.size && !rsx_vertex_legacy())
            fetch->desc.type = RSX_VERTEX_TYPE_UB256;
        if (!(input_mask & (1u << attr)) || !fetch->desc.size)
            fetch->desc.type = 0;
        if (!fetch->desc.type)
            fetch->desc.size = 0;
        fetch->elem_size =
            rsx_vertex_attrib_size(fetch->desc.type, fetch->desc.size);
        /* Stride 0 is a single element, not a tightly packed array. */
        fetch->stride = fetch->desc.stride;
        if (input_mask & (1u << attr))
            rsx_vertex_register_value(rsx, attr, fetch->default_value);
        else
            rsx_vertex_register_value(NULL, attr, fetch->default_value);
    }
}

u32 rsx_vertex_inline_layout(
    const rsx_dispatch* rsx, u32 offsets[RSX_DSP_NUM_VERTEX_ATTR])
{
    /* RPCS3 (analyse_inputs_interleaved, inlined_array): every array with
     * size > 0 is packed in attribute order, each taking its host size
     * (three 8/16-bit components pad to four); the declared stride is
     * ignored. */
    u32 packed = 0;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_dsp_vertex_attr desc;
        rsx_dsp_get_vertex_attr(rsx, attr, &desc);
        if (offsets)
            offsets[attr] = packed;
        if (!desc.type && desc.size && !rsx_vertex_legacy())
            desc.type = RSX_VERTEX_TYPE_UB256;
        if (!desc.type || !desc.size)
            continue;
        u32 size = rsx_vertex_attrib_size(desc.type, desc.size);
        if (desc.size == 3 && desc.type != RSX_VERTEX_TYPE_F &&
            desc.type != RSX_VERTEX_TYPE_CMP)
            size = rsx_vertex_attrib_size(desc.type, 4);
        packed += size;
    }
    return packed;
}

void rsx_vertex_fetch_plan_set_inline(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const u8* data, u32 bytes)
{
    if (!plan)
        return;
    plan->inline_data = NULL;
    plan->inline_bytes = 0;
    if (!data || !bytes || !rsx)
        return;
    plan->inline_stride = rsx_vertex_inline_layout(rsx, plan->inline_off);
    plan->inline_data = data;
    plan->inline_bytes = bytes;
}

void rsx_vertex_fetch_plan_prepare(
    rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count)
{
    if (!plan)
        return;
    /* Ordinary per-vertex attributes (frequency 0/1) all span the same
     * elements: find them once for the draw instead of once per attribute.
     * The per-attribute walk was 2.2 ms of the RSX thread a frame in
     * inFAMOUS 2's New Marais. */
    int plain_known = 0;
    u32 plain_first = UINT_MAX, plain_last = 0;
    if (plan->freq_mode != RSX_VERTEX_FREQ_LEGACY && rsx_vertex_legacy())
        plan->freq_mode = RSX_VERTEX_FREQ_LEGACY;
    plan->draw_min = 0;
    plan->draw_max = 0;
    if (plan->freq_mode != RSX_VERTEX_FREQ_LEGACY && refs && count) {
        u32 lo = UINT_MAX, hi = 0;
        for (u32 i = 0; i < count; i++) {
            if (refs[i].vertex_id < lo) lo = refs[i].vertex_id;
            if (refs[i].vertex_id > hi) hi = refs[i].vertex_id;
        }
        plan->draw_min = lo;
        plan->draw_max = hi;
        plan->draw_base_index = refs[0].base_index;
        if (!plan->inline_data)
            rsx_vertex_plan_blocks(plan);
    }
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        if (!refs || !count ||
            !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;

        /* INLINE_ARRAY: the stream is already one contiguous, correctly
         * strided array per attribute, which is exactly what stable_base
         * describes -- so it needs no resolver and no bounds fallback, only
         * the right base. */
        if (plan->inline_data) {
            const u32 stride =
                plan->inline_stride ? plan->inline_stride : fetch->stride;
            const u32 off = plan->inline_off[attr];
            if (!stride || off + fetch->elem_size > stride)
                continue;
            const u32 verts = plan->inline_bytes / stride;
            if (!verts)
                continue;
            fetch->stride = stride;
            fetch->stable_base = plan->inline_data + off;
            fetch->stable_first = 0;
            fetch->stable_last = verts - 1;
            continue;
        }

        if (!plan->guest_ptr)
            continue;

        u32 first = UINT_MAX;
        u32 last = 0;
        if (fetch->desc.frequency <= 1 &&
            plan->freq_mode != RSX_VERTEX_FREQ_LEGACY) {
            /* Interleaved blocks give plain attributes different origins. */
            first = plan->elem_base[attr];
            last = first + (plan->draw_max - plan->draw_min);
        } else if (fetch->desc.frequency <= 1) {
            if (!plain_known) {
                plain_known = 1;
                for (u32 i = 0; i < count; i++) {
                    const u32 element = rsx_vertex_plan_element(
                        plan, attr, &refs[i]);
                    if (element < plain_first) plain_first = element;
                    if (element > plain_last) plain_last = element;
                }
            }
            first = plain_first;
            last = plain_last;
        } else for (u32 i = 0; i < count; i++) {
            const u32 element = rsx_vertex_plan_element(plan, attr, &refs[i]);
            if (element < first) first = element;
            if (element > last) last = element;
        }

        const u64 prefix =
            (u64)(((u64)plan->base_offset + (u64)fetch->desc.offset) &
                  0x0FFFFFFFu);
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end =
            prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start ||
            end - start > UINT_MAX)
            continue;

        const u8* base = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, (u32)start,
            (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
    }

    /* Resolve each slot's element rule and decoder once for the draw. */
    const int legacy = plan->freq_mode == RSX_VERTEX_FREQ_LEGACY;
    const int indexed = plan->freq_mode == RSX_VERTEX_FREQ_RPCS3_INDEXED;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        rsx_vertex_fetch_fast* f = &plan->fast[slot];
        memset(f, 0, sizeof(*f));
        f->attr = (u8)attr;
        if (!fetch->desc.type || !fetch->desc.size) {
            f->kind = RSX_VC_DEFAULT;
            continue;
        }
        f->base = fetch->stable_base;
        f->first = fetch->stable_first;
        f->last = fetch->stable_last;
        f->stride = fetch->stride;
        f->elem_base = plan->elem_base[attr];
        f->frequency = fetch->desc.frequency;
        f->decoder = rsx_vc_decoder_id(fetch->desc.type, fetch->desc.size);
        const u32 freq = fetch->desc.frequency;
        if (legacy)
            f->kind = freq <= 1 ? RSX_VC_LEGACY : RSX_VC_GENERAL;
        else if (freq <= 1)
            f->kind = RSX_VC_PLAIN;
        else if (!((plan->divider_mask >> attr) & 1u))
            f->kind = RSX_VC_DIV;
        else if (indexed)
            f->kind = RSX_VC_GENERAL; /* modulo rebases on each base index */
        else
            /* Array draw: vertex_base + span is draw_max. */
            f->kind = plan->draw_max < freq ? RSX_VC_PLAIN : RSX_VC_MOD;
    }
    plan->fast_ready = 2;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const rsx_vertex_fetch_fast* f = &plan->fast[slot];
        if (f->kind != RSX_VC_PLAIN || !f->decoder || !f->base)
            plan->fast_ready = 1;
    }
}

/* Everything rsx_vertex_fetch_one does for one attribute once the element
 * is known: the stable span, the per-element resolvers, the generic
 * decoder and the default/ATTR0 failure rule. */
static int rsx_vertex_fetch_slot_tail(
    const rsx_vertex_fetch_plan* plan, u32 attr, u32 element, u8* out)
{
    const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
    float* value = (float*)out;
    const u8* source = NULL;
    if (fetch->stable_base &&
        element >= fetch->stable_first && element <= fetch->stable_last) {
        source = fetch->stable_base +
            (u64)(element - fetch->stable_first) * fetch->stride;
    } else if (plan->inline_data) {
        /* An inline draw has no vertex array to fall back to: the array
         * offsets still hold whatever the last ordinary draw bound, and
         * reading them would feed this draw another mesh's vertices.
         * An attribute the stream does not supply gets its default,
         * which is what the hardware feeds. */
        source = NULL;
    } else if (plan->guest_ptr) {
        const u32 prefix =
            (plan->base_offset + fetch->desc.offset) & 0x0FFFFFFFu;
        const u32 offset = prefix + element * fetch->stride;
        source = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, offset,
            fetch->elem_size);
        /* RPCS3 resolves only the array's base (get_address in
         * analyse_inputs_interleaved) and copies linearly from that
         * effective address, so an element whose own offset does not
         * resolve is still read when the memory after the base is. */
        const u64 rel = (u64)element * fetch->stride;
        if (!source && !rsx_vertex_legacy() && rel &&
            rel + fetch->elem_size <= UINT_MAX) {
            const u8* base = plan->guest_ptr(
                plan->guest_user, fetch->desc.location, prefix,
                (u32)(rel + fetch->elem_size));
            if (base)
                source = base + rel;
        }
    }

    if (source && rsx_vertex_decode_element(
            fetch->desc.type, fetch->desc.size, source, value))
        return 1;
    if (attr == 0)
        return 0;
    memcpy(value, fetch->default_value, 16);
    return 1;
}

RSX_VC_INLINE int rsx_vertex_fetch_slot_fast(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_fetch_fast* f,
    const rsx_vertex_ref* ref, u8* out)
{
    u32 element;
    switch (f->kind) {
    case RSX_VC_DEFAULT:
        memcpy(out, plan->attr[f->attr].default_value, 16);
        return 1;
    case RSX_VC_PLAIN:
        element = f->elem_base + (ref->vertex_id - plan->draw_min);
        break;
    case RSX_VC_LEGACY:
        element = (ref->vertex_id + ref->base_index) & 0x000FFFFFu;
        break;
    case RSX_VC_DIV:
        element = f->elem_base +
            (ref->vertex_id - plan->draw_min) / f->frequency;
        break;
    case RSX_VC_MOD:
        element = f->elem_base +
            (ref->vertex_id - plan->draw_min) % f->frequency;
        break;
    default:
        element = rsx_vertex_plan_element(plan, f->attr, ref);
        break;
    }
    if (f->decoder && f->base && element >= f->first && element <= f->last) {
        rsx_vc_decode_id(
            f->decoder, f->base + (u64)(element - f->first) * f->stride, out);
        return 1;
    }
    return rsx_vertex_fetch_slot_tail(plan, f->attr, element, out);
}

int rsx_vertex_fetch_one(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* ref, u8* out)
{
    if (!plan || !ref || (!out && plan->layout.stride))
        return 0;
    if (plan->fast_ready == 2) {
        /* Every slot is a plain array with a known decoder. */
        const u32 rel = ref->vertex_id - plan->draw_min;
        const u32 n = plan->layout.count;
        for (u32 slot = 0; slot < n; slot++) {
            const rsx_vertex_fetch_fast* f = &plan->fast[slot];
            const u32 element = f->elem_base + rel;
            if (element >= f->first && element <= f->last)
                rsx_vc_decode_id(f->decoder,
                                 f->base + (u64)(element - f->first) * f->stride,
                                 out + slot * 16u);
            else if (!rsx_vertex_fetch_slot_tail(
                         plan, f->attr, element, out + slot * 16u))
                return 0;
        }
        return 1;
    }
    if (plan->fast_ready) {
        for (u32 slot = 0; slot < plan->layout.count; slot++)
            if (!rsx_vertex_fetch_slot_fast(
                    plan, &plan->fast[slot], ref, out + slot * 16u))
                return 0;
        return 1;
    }
    /* Not prepared: resolve every slot from the descriptors. */
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        if (!fetch->desc.type || !fetch->desc.size) {
            memcpy(out + slot * 16u, fetch->default_value, 16);
            continue;
        }
        if (!rsx_vertex_fetch_slot_tail(
                plan, attr, rsx_vertex_plan_element(plan, attr, ref),
                out + slot * 16u))
            return 0;
    }
    return 1;
}

int rsx_vertex_fetch_many(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count,
    u8* out)
{
    if (!plan || (!refs && count))
        return 0;
    const u32 vs = plan->layout.stride;
    if (!count || !plan->layout.count)
        return 1;
    if (!out)
        return 0;
    if (!plan->fast_ready) {
        for (u32 i = 0; i < count; i++)
            if (!rsx_vertex_fetch_one(plan, &refs[i], out + (size_t)i * vs))
                return 0;
        return 1;
    }
    /* Slot 0 is ATTR0 whenever ATTR0 is in the layout, so a failing
     * position is found before any other attribute is written. */
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const rsx_vertex_fetch_fast* f = &plan->fast[slot];
        u8* o = out + slot * 16u;
        if (f->kind == RSX_VC_DEFAULT) {
            const float* dv = plan->attr[f->attr].default_value;
            for (u32 i = 0; i < count; i++)
                memcpy(o + (size_t)i * vs, dv, 16);
            continue;
        }
        if (f->kind == RSX_VC_PLAIN && f->decoder && f->base) {
            const rsx_vc_column_fn column = s_rsx_vc_column[f->decoder];
            u32 i = 0;
            while ((i = column(f, refs, i, count, plan->draw_min, o, vs)) <
                   count) {
                if (!rsx_vertex_fetch_slot_fast(
                        plan, f, &refs[i], o + (size_t)i * vs))
                    return 0;
                i++;
            }
            continue;
        }
        for (u32 i = 0; i < count; i++)
            if (!rsx_vertex_fetch_slot_fast(
                    plan, f, &refs[i], o + (size_t)i * vs))
                return 0;
    }
    return 1;
}

static u64 rsx_vertex_remap_hash(u64 value)
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

/* Dedupe ids in [lo, lo + range) sharing one base index. Returns 0 only
 * when the table cannot be allocated (the caller then hashes). */
static int rsx_vertex_remap_dense(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32 lo, u32 range, u32* unique_count)
{
    if (range > remap->dense_capacity) {
        u32 capacity = remap->dense_capacity ? remap->dense_capacity : 4096u;
        while (capacity < range)
            capacity *= 2u;
        u64* dense = (u64*)realloc(remap->dense, (size_t)capacity * sizeof(u64));
        if (!dense)
            return 0;
        remap->dense = dense;
        remap->dense_capacity = capacity;
        memset(remap->dense, 0, (size_t)capacity * sizeof(u64));
        remap->dense_generation = 1u;
    } else {
        remap->dense_generation++;
        if (!remap->dense_generation) {
            memset(remap->dense, 0,
                   (size_t)remap->dense_capacity * sizeof(u64));
            remap->dense_generation = 1u;
        }
    }
    u64* dense = remap->dense;
    u32* occurrence_map = remap->occurrence_to_unique;
    const u64 tag = (u64)remap->dense_generation << 32;
    u32 unique = 0;
    for (u32 occurrence = 0; occurrence < count; occurrence++) {
        const rsx_vertex_ref ref = refs[occurrence];
        u64* entry = &dense[ref.vertex_id - lo];
        if ((*entry & 0xFFFFFFFF00000000ull) != tag) {
            *entry = tag | unique;
            refs[unique] = ref;
            occurrence_map[occurrence] = unique;
            unique++;
        } else {
            occurrence_map[occurrence] = (u32)*entry;
        }
    }
    *unique_count = unique;
    return 1;
}

int rsx_vertex_remap_build(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* unique_count)
{
    if (!remap || (!refs && count) || !unique_count)
        return 0;
    *unique_count = count;
    if (!count)
        return 1;

    if (count > remap->occurrence_capacity) {
        u32* occurrences = (u32*)realloc(
            remap->occurrence_to_unique, (size_t)count * sizeof(u32));
        if (!occurrences)
            return 0;
        remap->occurrence_to_unique = occurrences;
        remap->occurrence_capacity = count;
    }

    if (count > UINT_MAX / 2u)
        return 0;
    u32 slot_capacity = 16u;
    const u32 required_slots = count * 2u;
    while (slot_capacity < required_slots) {
        if (slot_capacity > UINT_MAX / 2u)
            return 0;
        slot_capacity *= 2u;
    }

    /* One pass decides whether the hash is needed at all. Every key is
     * (base_index, vertex_id); with one base index throughout, strictly
     * increasing ids (array draws, restart-cut runs) are all distinct and
     * the remap is the identity, and ids within a narrow range dedupe
     * through a direct-mapped table. All paths emit the same first-use
     * order the hash does. */
    {
        const u32 base0 = refs[0].base_index;
        u32 lo = refs[0].vertex_id, hi = lo, prev = lo;
        u32 same_base = 1, increasing = 1;
        for (u32 i = 1; i < count; i++) {
            const u32 v = refs[i].vertex_id;
            same_base &= refs[i].base_index == base0;
            increasing &= v > prev;
            prev = v;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        if (same_base && increasing) {
            for (u32 i = 0; i < count; i++)
                remap->occurrence_to_unique[i] = i;
            *unique_count = count;
            return 1;
        }
        const u64 range = (u64)hi - lo + 1u;
        const u64 dense_limit = count < 2048u ? 8192u : (u64)count * 4u;
        if (same_base && range <= dense_limit && range <= (1u << 22) &&
            rsx_vertex_remap_dense(remap, refs, count, lo, (u32)range,
                                   unique_count))
            return 1;
    }

    if (slot_capacity > remap->slot_capacity) {
        rsx_vertex_remap_slot* slots = (rsx_vertex_remap_slot*)realloc(
            remap->slots, (size_t)slot_capacity * sizeof(*slots));
        if (!slots)
            return 0;
        remap->slots = slots;
        remap->slot_capacity = slot_capacity;
        memset(remap->slots, 0,
               (size_t)remap->slot_capacity * sizeof(*remap->slots));
        remap->generation = 1u;
    } else {
        remap->generation++;
        if (!remap->generation) {
            memset(remap->slots, 0,
                   (size_t)remap->slot_capacity * sizeof(*remap->slots));
            remap->generation = 1u;
        }
    }

    u32 unique = 0;
    const u32 mask = remap->slot_capacity - 1u;
    for (u32 occurrence = 0; occurrence < count; occurrence++) {
        const rsx_vertex_ref ref = refs[occurrence];
        const u64 key = ((u64)ref.base_index << 32) | ref.vertex_id;
        u32 slot_index = (u32)rsx_vertex_remap_hash(key) & mask;
        for (;;) {
            rsx_vertex_remap_slot* slot = &remap->slots[slot_index];
            if (slot->generation != remap->generation) {
                slot->key = key;
                slot->value = unique;
                slot->generation = remap->generation;
                refs[unique] = ref;
                remap->occurrence_to_unique[occurrence] = unique;
                unique++;
                break;
            }
            if (slot->key == key) {
                remap->occurrence_to_unique[occurrence] = slot->value;
                break;
            }
            slot_index = (slot_index + 1u) & mask;
        }
    }
    *unique_count = unique;
    return 1;
}

u32 rsx_vertex_remap_index(
    const rsx_vertex_remap* remap, u32 occurrence)
{
    return remap && occurrence < remap->occurrence_capacity
        ? remap->occurrence_to_unique[occurrence] : occurrence;
}

void rsx_vertex_remap_destroy(rsx_vertex_remap* remap)
{
    if (!remap)
        return;
    free(remap->occurrence_to_unique);
    free(remap->slots);
    free(remap->dense);
    memset(remap, 0, sizeof(*remap));
}

int rsx_vertex_topology_plan(
    u32 primitive, int refs_remapped, int* indexed)
{
    if (!indexed)
        return 0;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        *indexed = refs_remapped != 0;
        return 1;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_QUADS:
        *indexed = 1;
        return 1;
    default:
        *indexed = 0;
        return 0;
    }
}
