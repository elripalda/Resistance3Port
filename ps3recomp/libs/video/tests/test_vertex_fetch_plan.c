/*
 * rsx_vertex_fetch_plan_prepare against the per-attribute walk it replaced.
 *
 * _prepare finds the span of ordinary per-vertex attributes (frequency 0/1)
 * once per draw instead of once per attribute. Random fetch plans -- every
 * vertex type, strides and offsets, frequency dividers in both modes, guest
 * memory that is partly unmapped, INLINE_ARRAY streams -- with random index
 * references, some past the 20-bit element wrap: every attribute's stable
 * range (base, first, last) must be what the old walk gave.
 *
 * Build (from the repo root):
 *   clang -std=gnu17 -O2 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
 *     -o /tmp/test_vertex_fetch_plan libs/video/tests/test_vertex_fetch_plan.c \
 *     libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c
 */
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails <= 20) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}

enum { GUEST = 1 << 16 };
static uint8_t guest[2][GUEST];         /* two locations */
static uint32_t mapped_end[2];          /* bytes past this are unmapped */

static const u8* guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user;
    const u32 loc = location & 1u;
    if ((u64)offset + min_bytes > mapped_end[loc]) return NULL;
    return guest[loc] + offset;
}

/* The walk _prepare used to do: every reference, once per attribute. */
static void ref_prepare(rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count)
{
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        if (!refs || !count || !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;
        if (plan->inline_data) {
            const u32 stride = plan->inline_stride ? plan->inline_stride : fetch->stride;
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
        u32 first = UINT_MAX, last = 0;
        for (u32 i = 0; i < count; i++) {
            const u32 element = rsx_vertex_element_index(
                refs[i].vertex_id, refs[i].base_index, fetch->desc.frequency,
                (plan->divider_mask >> attr) & 1u);
            if (element < first) first = element;
            if (element > last) last = element;
        }
        const u64 prefix = (u64)plan->base_offset + (u64)fetch->desc.offset;
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end = prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start || end - start > UINT_MAX)
            continue;
        const u8* base = plan->guest_ptr(plan->guest_user, fetch->desc.location,
                                         (u32)start, (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
    }
}

static void random_plan(rsx_vertex_fetch_plan* plan, int use_inline,
                        const u8* inl, u32 inl_bytes)
{
    memset(plan, 0, sizeof *plan);
    rsx_vertex_layout_plan_init(&plan->layout, rnd() & 0xFFFFu);
    plan->base_offset = (rnd() % 4 == 0) ? rnd() % 256u : 0;
    plan->divider_mask = rnd() & 0xFFFFu;
    plan->guest_ptr = guest_ptr;
    u32 inline_packed = 0;
    for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
        rsx_vertex_fetch_attr* f = &plan->attr[a];
        const u32 r = rnd() % 16;
        f->desc.type = r < 6 ? RSX_VTX_TYPE_FLOAT : r < 7 ? 0 : 1 + rnd() % 7;
        f->desc.size = (rnd() % 8 == 0) ? rnd() % 6 : 1 + rnd() % 4;
        f->elem_size = rsx_vertex_attrib_size(f->desc.type, f->desc.size);
        f->desc.stride = (rnd() % 4 == 0) ? 0 : f->elem_size + (rnd() % 3) * 4u;
        f->stride = f->desc.stride ? f->desc.stride : f->elem_size;
        f->desc.frequency = (rnd() % 4 == 0) ? rnd() % 4 : 0;
        f->desc.offset = rnd() % 4096u;
        f->desc.location = rnd() & 1u;
    }
    if (use_inline) {
        for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
            plan->inline_off[a] = inline_packed;
            if (plan->attr[a].desc.type && plan->attr[a].desc.size)
                inline_packed += plan->attr[a].elem_size;
        }
        plan->inline_stride = inline_packed ? inline_packed : 16u;
        plan->inline_data = inl;
        plan->inline_bytes = inl_bytes;
    }
}

/* ---- frozen reference: rsx_vertex_compact.c before the fast paths -------
 * Verbatim copies (renamed orig_*) of the element rule, block planning,
 * decoder, _prepare, _fetch_one and the hash remap. The differential test
 * below runs these against the live implementation and memcmp's every
 * output byte. */

static u32 orig_plan_element(const rsx_vertex_fetch_plan* plan,
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

static void orig_plan_blocks(rsx_vertex_fetch_plan* plan)
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

static float orig_be_f32(const u8* p)
{
    const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

static float orig_be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0) {
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

static int orig_decode_element(u32 type, u32 size, const u8* source, float out[4])
{
    if (!source || !out)
        return 0;
    if (type == RSX_VTX_TYPE_SNORM16)
        out[0] = out[1] = out[2] = out[3] = 0.5f / 32767.5f;
    else
        out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (size < 4)
        out[3] = 1.0f;
    for (u32 component = 0; component < size && component < 4; component++) {
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            out[component] = orig_be_f32(source + component * 4);
            break;
        case RSX_VTX_TYPE_HALF:
            out[component] = orig_be_f16(source + component * 2);
            break;
        case RSX_VTX_TYPE_UNORM8:
            out[component] = source[component] / 255.0f;
            break;
        case RSX_VTX_TYPE_UINT8:
            out[component] = (float)source[component];
            break;
        case RSX_VTX_TYPE_SNORM16: {
            const s16 value = (s16)((source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = ((float)value + 0.5f) / 32767.5f;
            break;
        }
        case RSX_VTX_TYPE_SINT16: {
            const s16 value = (s16)((source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = (float)value;
            break;
        }
        case RSX_VTX_TYPE_CMP32: {
            const u32 word = ((u32)source[0] << 24) | ((u32)source[1] << 16) |
                             ((u32)source[2] << 8) | (u32)source[3];
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

static void orig_fetch_plan_prepare(rsx_vertex_fetch_plan* plan,
                                    const rsx_vertex_ref* refs, u32 count)
{
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
            orig_plan_blocks(plan);
    }
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        if (!refs || !count || !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;
        if (plan->inline_data) {
            const u32 stride = plan->inline_stride ? plan->inline_stride : fetch->stride;
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
        if (fetch->desc.frequency <= 1 && plan->freq_mode != RSX_VERTEX_FREQ_LEGACY) {
            first = plan->elem_base[attr];
            last = first + (plan->draw_max - plan->draw_min);
        } else if (fetch->desc.frequency <= 1) {
            if (!plain_known) {
                plain_known = 1;
                for (u32 i = 0; i < count; i++) {
                    const u32 element = orig_plan_element(plan, attr, &refs[i]);
                    if (element < plain_first) plain_first = element;
                    if (element > plain_last) plain_last = element;
                }
            }
            first = plain_first;
            last = plain_last;
        } else for (u32 i = 0; i < count; i++) {
            const u32 element = orig_plan_element(plan, attr, &refs[i]);
            if (element < first) first = element;
            if (element > last) last = element;
        }
        const u64 prefix = (u64)(((u64)plan->base_offset + (u64)fetch->desc.offset) & 0x0FFFFFFFu);
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end = prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start || end - start > UINT_MAX)
            continue;
        const u8* base = plan->guest_ptr(plan->guest_user, fetch->desc.location,
                                         (u32)start, (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
    }
}

static int orig_fetch_one(const rsx_vertex_fetch_plan* plan,
                          const rsx_vertex_ref* ref, u8* out)
{
    if (!plan || !ref || (!out && plan->layout.stride))
        return 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        float* value = (float*)(out + slot * 16u);
        if (!fetch->desc.type || !fetch->desc.size) {
            memcpy(value, fetch->default_value, 16);
            continue;
        }
        const u32 element = orig_plan_element(plan, attr, ref);
        const u8* source = NULL;
        if (fetch->stable_base &&
            element >= fetch->stable_first && element <= fetch->stable_last) {
            source = fetch->stable_base + (u64)(element - fetch->stable_first) * fetch->stride;
        } else if (plan->inline_data) {
            source = NULL;
        } else if (plan->guest_ptr) {
            const u32 prefix = (plan->base_offset + fetch->desc.offset) & 0x0FFFFFFFu;
            const u32 offset = prefix + element * fetch->stride;
            source = plan->guest_ptr(plan->guest_user, fetch->desc.location, offset,
                                     fetch->elem_size);
            const u64 rel = (u64)element * fetch->stride;
            if (!source && !rsx_vertex_legacy() && rel &&
                rel + fetch->elem_size <= UINT_MAX) {
                const u8* base = plan->guest_ptr(plan->guest_user, fetch->desc.location,
                                                 prefix, (u32)(rel + fetch->elem_size));
                if (base)
                    source = base + rel;
            }
        }
        if (source && orig_decode_element(fetch->desc.type, fetch->desc.size, source, value))
            continue;
        if (attr == 0)
            return 0;
        memcpy(value, fetch->default_value, 16);
    }
    return 1;
}

static u64 orig_remap_hash(u64 value)
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

static int orig_remap_build(rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
                            u32* unique_count)
{
    if (!remap || (!refs && count) || !unique_count)
        return 0;
    *unique_count = count;
    if (!count)
        return 1;
    if (count > remap->occurrence_capacity) {
        u32* occurrences = (u32*)realloc(remap->occurrence_to_unique, (size_t)count * sizeof(u32));
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
    if (slot_capacity > remap->slot_capacity) {
        rsx_vertex_remap_slot* slots = (rsx_vertex_remap_slot*)realloc(
            remap->slots, (size_t)slot_capacity * sizeof(*slots));
        if (!slots)
            return 0;
        remap->slots = slots;
        remap->slot_capacity = slot_capacity;
        memset(remap->slots, 0, (size_t)remap->slot_capacity * sizeof(*remap->slots));
        remap->generation = 1u;
    } else {
        remap->generation++;
        if (!remap->generation) {
            memset(remap->slots, 0, (size_t)remap->slot_capacity * sizeof(*remap->slots));
            remap->generation = 1u;
        }
    }
    u32 unique = 0;
    const u32 mask = remap->slot_capacity - 1u;
    for (u32 occurrence = 0; occurrence < count; occurrence++) {
        const rsx_vertex_ref ref = refs[occurrence];
        const u64 key = ((u64)ref.base_index << 32) | ref.vertex_id;
        u32 slot_index = (u32)orig_remap_hash(key) & mask;
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

static void orig_remap_destroy(rsx_vertex_remap* remap)
{
    free(remap->occurrence_to_unique);
    free(remap->slots);
    memset(remap, 0, sizeof(*remap));
}

/* ---- differential: live implementation vs the frozen reference ---------- */

/* Byte patterns that stress every decoder: NaN/Inf/denormal floats and
 * halves (both signs, quiet and signalling payloads), 0x8000 shorts, etc. */
static void fill_guest_random(void)
{
    static const u8 special[][4] = {
        {0x7F,0x80,0x00,0x00}, {0xFF,0x80,0x00,0x00}, {0x7F,0xC0,0x00,0x01},
        {0x7F,0x80,0x00,0x01}, {0xFF,0xBF,0xFF,0xFF}, {0x00,0x00,0x00,0x01},
        {0x80,0x00,0x00,0x00}, {0x7C,0x00,0x7C,0x01}, {0xFC,0x00,0xFE,0x00},
        {0x00,0x01,0x83,0xFF}, {0x80,0x00,0x7F,0xFF}, {0xFF,0xFF,0x00,0x00},
    };
    for (int l = 0; l < 2; l++)
        for (u32 i = 0; i < GUEST; i += 4) {
            const u32 r = rnd();
            if (r % 8 == 0)
                memcpy(&guest[l][i], special[(r >> 8) % 12], 4);
            else {
                const u32 v = rnd();
                memcpy(&guest[l][i], &v, 4);
            }
        }
}

static void diff_random_plan(rsx_vertex_fetch_plan* plan, u8* inl, u32 inl_cap)
{
    memset(plan, 0, sizeof *plan);
    u32 mask = rnd() & 0xFFFFu;
    if (rnd() % 3 == 0) mask = (rnd() % 2 ? 0x000Fu : 0x0F01u) | (rnd() & 0x00F0u);
    rsx_vertex_layout_plan_init(&plan->layout, mask);
    plan->base_offset = (rnd() % 4 == 0) ? rnd() % 256u : 0;
    if (rnd() % 16 == 0) plan->base_offset = 0xFFFFFF00u + rnd() % 256u;
    plan->divider_mask = rnd() & 0xFFFFu;
    plan->guest_ptr = (rnd() % 64 == 0) ? NULL : guest_ptr;
    const u32 interleave = rnd() % 2;
    const u32 ib_stride = 4u * (4u + rnd() % 12u);
    u32 ib_off = rnd() % 1024u;
    for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
        rsx_vertex_fetch_attr* f = &plan->attr[a];
        const u32 r = rnd() % 20;
        f->desc.type = r < 5 ? RSX_VTX_TYPE_FLOAT : r < 6 ? 0
                     : r < 7 ? 8 + rnd() % 3 : 1 + rnd() % 7;
        f->desc.size = (rnd() % 10 == 0) ? rnd() % 7 : 1 + rnd() % 4;
        if (!f->desc.type || !f->desc.size) {
            if (rnd() % 2) f->desc.size = 0;
        }
        f->elem_size = rsx_vertex_attrib_size(f->desc.type, f->desc.size);
        if (f->desc.type > 7)
            f->elem_size = f->desc.size * 4u;
        if (interleave && f->elem_size <= ib_stride && rnd() % 4) {
            f->desc.stride = ib_stride;
            f->desc.offset = ib_off;
            ib_off += 4u;
        } else {
            f->desc.stride = (rnd() % 6 == 0) ? 0 : f->elem_size + (rnd() % 4) * 2u;
            f->desc.offset = rnd() % 4096u;
        }
        if (rnd() % 32 == 0) f->desc.offset |= 0x80000000u;
        f->stride = f->desc.stride;
        const u32 fr = rnd() % 8;
        f->desc.frequency = fr < 5 ? fr % 2 : 2 + rnd() % 300u;
        f->desc.location = rnd() & 1u;
        for (int c = 0; c < 4; c++) {
            const u32 bits = (rnd() % 8 == 0) ? 0x7FC00000u | (rnd() & 0x3FFFFFu) : rnd();
            memcpy(&f->default_value[c], &bits, 4);
        }
    }
    const u32 mode = rnd() % 8;
    plan->freq_mode = mode < 1 ? RSX_VERTEX_FREQ_LEGACY
                    : mode < 4 ? RSX_VERTEX_FREQ_RPCS3_ARRAYS : RSX_VERTEX_FREQ_RPCS3_INDEXED;
    if (rnd() % 3 == 0) {
        /* A typical game draw: every enabled array is valid, per-vertex. */
        for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
            rsx_vertex_fetch_attr* f = &plan->attr[a];
            if (!f->desc.type || f->desc.type > 7) f->desc.type = RSX_VTX_TYPE_FLOAT;
            if (!f->desc.size || f->desc.size > 4) f->desc.size = 1 + rnd() % 4;
            f->desc.frequency = rnd() % 2;
            f->elem_size = rsx_vertex_attrib_size(f->desc.type, f->desc.size);
            if (f->desc.stride && f->desc.stride < f->elem_size)
                f->desc.stride = f->stride = f->elem_size;
            f->desc.offset &= 0x7FFFFFFFu;
        }
        if (plan->freq_mode == RSX_VERTEX_FREQ_LEGACY)
            plan->freq_mode = RSX_VERTEX_FREQ_RPCS3_INDEXED;
        plan->guest_ptr = guest_ptr;
    }
    if (rnd() % 6 == 0) {
        u32 packed = 0;
        for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
            plan->inline_off[a] = packed;
            if (plan->attr[a].desc.type && plan->attr[a].desc.size)
                packed += plan->attr[a].elem_size;
        }
        plan->inline_stride = (rnd() % 8 == 0) ? 0 : packed;
        for (u32 i = 0; i < inl_cap; i++) inl[i] = (u8)rnd();
        plan->inline_data = inl;
        plan->inline_bytes = rnd() % inl_cap;
        if (rnd() % 2) plan->freq_mode = RSX_VERTEX_FREQ_LEGACY;
    }
}

static void diff_random_refs(rsx_vertex_ref* refs, u32 count)
{
    const u32 kind = rnd() % 8;
    const u32 base = (rnd() % 3 == 0) ? rnd() % 64u : 0;
    u32 start = (rnd() % 4 == 0) ? 0x000FFFFFu - rnd() % 400u : rnd() % 2000u;
    if (rnd() % 16 == 0) start = 0xFFFFFFFFu - rnd() % 300u;
    const u32 span = 1 + ((rnd() % 4 == 0) ? rnd() % 20000u : rnd() % 600u);
    for (u32 i = 0; i < count; i++) {
        switch (kind) {
        case 0: case 1:  /* DRAW_ARRAYS: sequential */
            refs[i].vertex_id = start + i;
            break;
        case 2:          /* sequential with gaps / restart-cut runs */
            refs[i].vertex_id = start + i + (i / 7) * (rnd() % 3);
            break;
        case 3:          /* sequential, then rewinds */
            refs[i].vertex_id = start + (i % (1 + span));
            break;
        case 4:          /* wild ids: forces the hash */
            refs[i].vertex_id = (rnd() % 4 == 0) ? rnd() : start + rnd() % span;
            break;
        default:         /* indexed mesh: shared vertices */
            refs[i].vertex_id = start + rnd() % span;
            break;
        }
        refs[i].base_index = base;
        if (rnd() % 512 == 0) refs[i].base_index = rnd() % 4u;
    }
    if (rnd() % 8 == 0 && count > 2) {
        for (u32 i = 0; i + 1 < count; i += 2) {  /* descending pairs */
            const rsx_vertex_ref t = refs[i];
            refs[i] = refs[i + 1];
            refs[i + 1] = t;
        }
    }
}

static int diff_compare_plans(const rsx_vertex_fetch_plan* a, const rsx_vertex_fetch_plan* b)
{
    if (a->draw_min != b->draw_min || a->draw_max != b->draw_max ||
        a->draw_base_index != b->draw_base_index || a->freq_mode != b->freq_mode)
        return 0;
    if (memcmp(a->elem_base, b->elem_base, sizeof a->elem_base))
        return 0;
    for (u32 at = 0; at < RSX_DSP_NUM_VERTEX_ATTR; at++)
        if (a->attr[at].stable_base != b->attr[at].stable_base ||
            a->attr[at].stable_first != b->attr[at].stable_first ||
            a->attr[at].stable_last != b->attr[at].stable_last ||
            a->attr[at].stride != b->attr[at].stride)
            return 0;
    return 1;
}

static int run_differential(int trials)
{
    enum { MAXR = 4096 };
    static rsx_vertex_ref ra[MAXR], rb[MAXR], extra[64];
    static u8 inl[8192];
    static u8 oa[MAXR * 256 + 64], ob[MAXR * 256 + 64];
    rsx_vertex_remap rma, rmb;
    memset(&rma, 0, sizeof rma);
    memset(&rmb, 0, sizeof rmb);
    const int fails_before = fails;
    u64 verts = 0, remapped = 0, failed_fetch = 0, by_type[16] = {0}, by_kind[8] = {0}, all_plain = 0, remap_path[3] = {0};
    fill_guest_random();

    for (int n = 0; n < trials; n++) {
        if (n % 500 == 0) fill_guest_random();
        for (int l = 0; l < 2; l++)
            mapped_end[l] = (rnd() % 4 == 0) ? rnd() % GUEST : GUEST;
        rsx_vertex_fetch_plan pa, pb;
        diff_random_plan(&pa, inl, sizeof inl);
        pb = pa;

        u32 count = (rnd() % 8 == 0) ? 1 + rnd() % MAXR : 1 + rnd() % 400u;
        if (rnd() % 64 == 0) count = 0;
        diff_random_refs(ra, count);
        memcpy(rb, ra, sizeof(rsx_vertex_ref) * count);

        u32 ua = count, ub = count;
        if (rnd() % 4) {
            const u32 gen_hash = rma.generation, gen_dense = rma.dense_generation;
            const int ka = rsx_vertex_remap_build(&rma, ra, count, &ua);
            const int kb = orig_remap_build(&rmb, rb, count, &ub);
            if (count > 1)
                remap_path[rma.generation != gen_hash ? 2 : rma.dense_generation != gen_dense ? 1 : 0]++;
            CHECK(ka == kb && ua == ub, "trial %d remap ret %d/%d unique %u/%u", n, ka, kb, ua, ub);
            CHECK(!memcmp(ra, rb, sizeof(rsx_vertex_ref) * count), "trial %d remap refs", n);
            if (count)
                CHECK(!memcmp(rma.occurrence_to_unique, rmb.occurrence_to_unique,
                              sizeof(u32) * count), "trial %d remap occurrence map", n);
            for (u32 i = 0; i < count; i += 1 + count / 8)
                CHECK(rsx_vertex_remap_index(&rma, i) == rmb.occurrence_to_unique[i],
                      "trial %d remap_index %u", n, i);
            remapped++;
        }
        const u32 pass_null = (rnd() % 128 == 0);
        rsx_vertex_fetch_plan_prepare(&pa, pass_null ? NULL : ra, ua);
        orig_fetch_plan_prepare(&pb, pass_null ? NULL : rb, ub);
        CHECK(diff_compare_plans(&pa, &pb), "trial %d prepare state differs", n);

        const u32 vstride = pa.layout.stride;
        memset(oa, 0xA5, (size_t)ua * vstride + 16);
        memset(ob, 0xA5, (size_t)ua * vstride + 16);
        for (u32 i = 0; i < ua; i++) {
            const int ka = rsx_vertex_fetch_one(&pa, &ra[i], vstride ? oa + (size_t)i * vstride : NULL);
            const int kb = orig_fetch_one(&pb, &rb[i], vstride ? ob + (size_t)i * vstride : NULL);
            CHECK(ka == kb, "trial %d vertex %u fetch ret %d/%d", n, i, ka, kb);
            if (!kb) failed_fetch++;
        }
        CHECK(!memcmp(oa, ob, (size_t)ua * vstride + 16), "trial %d fetch output differs", n);
        verts += ua;
        for (u32 s = 0; s < pa.layout.count; s++) {
            by_type[pa.attr[pa.layout.attrs[s]].desc.type & 15]++;
            by_kind[pa.fast[s].kind & 7]++;
        }
        all_plain += pa.fast_ready == 2;

        /* References the plan was not prepared over: out of the stable
         * spans, other base indices -- the per-element fallbacks. */
        const u32 n_extra = rnd() % 16;
        for (u32 i = 0; i < n_extra; i++) {
            extra[i] = ua ? ra[rnd() % ua] : (rsx_vertex_ref){0, 0};
            if (rnd() % 2) extra[i].vertex_id += rnd() % 64 - 32;
            if (rnd() % 4 == 0) extra[i].vertex_id = rnd();
            if (rnd() % 4 == 0) extra[i].base_index = rnd() % 8;
            memset(oa, 0x5A, vstride + 16);
            memset(ob, 0x5A, vstride + 16);
            const int ka = rsx_vertex_fetch_one(&pa, &extra[i], vstride ? oa : NULL);
            const int kb = orig_fetch_one(&pb, &extra[i], vstride ? ob : NULL);
            CHECK(ka == kb && !memcmp(oa, ob, vstride + 16), "trial %d extra ref %u", n, i);
        }

#ifdef RSX_VERTEX_HAVE_FETCH_MANY
        if (ua && vstride) {
            memset(oa, 0x3C, (size_t)ua * vstride + 16);
            memset(ob, 0x3C, (size_t)ua * vstride + 16);
            const int ka = rsx_vertex_fetch_many(&pa, ra, ua, oa);
            int kb = 1;
            for (u32 i = 0; i < ua && kb; i++)
                kb = orig_fetch_one(&pb, &rb[i], ob + (size_t)i * vstride);
            CHECK(ka == kb, "trial %d fetch_many ret %d/%d", n, ka, kb);
            if (ka && kb)
                CHECK(!memcmp(oa, ob, (size_t)ua * vstride + 16), "trial %d fetch_many output", n);
        }
#endif
    }

    /* Direct decoder sweep: every type x size over random and special bytes. */
    u64 decodes = 0;
    for (u32 type = 0; type <= 9; type++)
        for (u32 size = 0; size <= 6; size++)
            for (int k = 0; k < 4000; k++) {
                const u8* src = guest[0] + (rnd() % (GUEST - 32));
                float fa[4], fb[4];
                memset(fa, 0x77, sizeof fa);
                memset(fb, 0x77, sizeof fb);
                const int ka = rsx_vertex_decode_element(type, size, src, fa);
                const int kb = orig_decode_element(type, size, src, fb);
                CHECK(ka == kb && !memcmp(fa, fb, sizeof fa), "decode type %u size %u", type, size);
                decodes++;
            }

    rsx_vertex_remap_destroy(&rma);
    orig_remap_destroy(&rmb);
    printf("differential: %d draws, %llu vertices, %llu remaps, %llu failed fetches, "
           "%llu decodes; slots by type f%llu h%llu un8%llu u8%llu sn16%llu s16%llu cmp%llu "
           "none%llu bad%llu; slots by rule default%llu plain%llu legacy%llu div%llu mod%llu general%llu; "
           "%llu all-plain draws; %d failure(s)\n",
           trials, (unsigned long long)verts, (unsigned long long)remapped,
           (unsigned long long)failed_fetch, (unsigned long long)decodes,
           (unsigned long long)by_type[RSX_VTX_TYPE_FLOAT], (unsigned long long)by_type[RSX_VTX_TYPE_HALF],
           (unsigned long long)by_type[RSX_VTX_TYPE_UNORM8], (unsigned long long)by_type[RSX_VTX_TYPE_UINT8],
           (unsigned long long)by_type[RSX_VTX_TYPE_SNORM16], (unsigned long long)by_type[RSX_VTX_TYPE_SINT16],
           (unsigned long long)by_type[RSX_VTX_TYPE_CMP32], (unsigned long long)by_type[0],
           (unsigned long long)(by_type[8] + by_type[9] + by_type[10]),
           (unsigned long long)by_kind[0], (unsigned long long)by_kind[1], (unsigned long long)by_kind[2],
           (unsigned long long)by_kind[3], (unsigned long long)by_kind[4], (unsigned long long)by_kind[5],
           (unsigned long long)all_plain, fails - fails_before);
    for (int k = 0; k <= 5; k++)
        if (!by_kind[k]) {
            printf("FAIL: element rule %d never exercised\n", k);
            return 0;
        }
    printf("remap paths: identity %llu, dense %llu, hash %llu\n",
           (unsigned long long)remap_path[0], (unsigned long long)remap_path[1],
           (unsigned long long)remap_path[2]);
    if (all_plain < 100 || remapped < 1000 ||
        remap_path[0] < 100 || remap_path[1] < 100 || remap_path[2] < 100) {
        printf("FAIL: too few all-plain draws or remap paths exercised\n");
        return 0;
    }
    return fails == fails_before;
}

/* ---- micro-benchmark: `test_vertex_fetch_plan bench` -------------------- */

#ifdef _WIN32
#include <windows.h>
static double now_us(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
}
#else
#include <time.h>
static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}
#endif

/* A gameplay-shaped draw: position f32x3, normal cmp, tangent s16n x4,
 * uv0 half x2, uv1 f32x2, colour ub x4, one interleaved stream. */
static void bench_plan(rsx_vertex_fetch_plan* plan, u32 mode)
{
    memset(plan, 0, sizeof *plan);
    rsx_vertex_layout_plan_init(&plan->layout, 0x013Bu);
    plan->guest_ptr = guest_ptr;
    plan->freq_mode = (u8)mode;
    static const u32 types[6] = { RSX_VTX_TYPE_FLOAT, RSX_VTX_TYPE_CMP32, RSX_VTX_TYPE_SNORM16,
                                  RSX_VTX_TYPE_HALF, RSX_VTX_TYPE_FLOAT, RSX_VTX_TYPE_UNORM8 };
    static const u32 sizes[6] = { 3, 1, 4, 2, 2, 4 };
    static const u32 attrs[6] = { 0, 1, 3, 4, 5, 8 };
    u32 off = 0;
    for (int i = 0; i < 6; i++) {
        rsx_vertex_fetch_attr* f = &plan->attr[attrs[i]];
        f->desc.type = types[i];
        f->desc.size = sizes[i];
        f->elem_size = rsx_vertex_attrib_size(types[i], sizes[i]);
        f->desc.offset = off;
        off += f->elem_size;
    }
    for (int i = 0; i < 6; i++) {
        plan->attr[attrs[i]].desc.stride = off;
        plan->attr[attrs[i]].stride = off;
    }
}

static void run_bench(void)
{
    enum { DRAWS = 1300, FRAMES = 20, MAXR = 4096 };
    static rsx_vertex_ref src[DRAWS][MAXR / 4];
    static u32 n_src[DRAWS];
    static rsx_vertex_ref work[MAXR];
    static u8 out[MAXR * 16 * 16];
    for (int l = 0; l < 2; l++) mapped_end[l] = GUEST;
    fill_guest_random();
    /* Mixed frame: 3/4 indexed meshes (~2x vertex reuse), 1/4 array draws. */
    for (int d = 0; d < DRAWS; d++) {
        const u32 n = 60 + rnd() % 900u;
        n_src[d] = n;
        const int arrays = d % 4 == 3;
        for (u32 i = 0; i < n; i++) {
            src[d][i].vertex_id = arrays ? i : (rnd() % (n / 2 + 1));
            src[d][i].base_index = 0;
        }
    }
    rsx_vertex_remap remap;
    for (int impl = 0; impl < 3; impl++) {
        memset(&remap, 0, sizeof remap);
        double best = 1e30, t_remap = 0, t_prep = 0, t_fetch = 0;
        u64 vtx = 0;
        for (int f = 0; f < FRAMES; f++) {
            double tr = 0, tp = 0, tf = 0;
            const double t0 = now_us();
            vtx = 0;
            for (int d = 0; d < DRAWS; d++) {
                const int arrays = d % 4 == 3;
                const u32 n = n_src[d];
                memcpy(work, src[d], n * sizeof *work);
                rsx_vertex_fetch_plan plan;
                bench_plan(&plan, arrays ? RSX_VERTEX_FREQ_RPCS3_ARRAYS : RSX_VERTEX_FREQ_RPCS3_INDEXED);
                const double a = now_us();
                u32 unique = n;
                if (impl) rsx_vertex_remap_build(&remap, work, n, &unique);
                else orig_remap_build(&remap, work, n, &unique);
                const double b = now_us();
                if (impl) rsx_vertex_fetch_plan_prepare(&plan, work, unique);
                else orig_fetch_plan_prepare(&plan, work, unique);
                const double c = now_us();
                const u32 vs = plan.layout.stride;
                if (impl == 2)
                    rsx_vertex_fetch_many(&plan, work, unique, out);
                else for (u32 i = 0; i < unique; i++) {
                    if (impl) rsx_vertex_fetch_one(&plan, &work[i], out + (size_t)i * vs);
                    else orig_fetch_one(&plan, &work[i], out + (size_t)i * vs);
                }
                const double e = now_us();
                tr += b - a; tp += c - b; tf += e - c;
                vtx += unique;
            }
            const double t = now_us() - t0;
            if (t < best) { best = t; t_remap = tr; t_prep = tp; t_fetch = tf; }
        }
        printf("bench %-9s: frame %8.1f us (%d draws, %llu verts) = remap %7.1f + prepare %6.1f + fetch %8.1f us; %.2f us/draw\n",
               impl == 2 ? "opt+many" : impl ? "optimized" : "original", best, DRAWS, (unsigned long long)vtx,
               t_remap, t_prep, t_fetch, best / DRAWS);
        if (impl) rsx_vertex_remap_destroy(&remap);
        else orig_remap_destroy(&remap);
    }
}

/* Every 16-bit pattern for the half/short decoders, every byte for the
 * 8-bit ones, and random plus special words for float/cmp -- through the
 * specialised fetch paths (fetch_one and fetch_many), not just the generic
 * public decoder. */
static u8 exh_mem[1u << 21];
static const u8* exh_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user; (void)location;
    return (u64)offset + min_bytes <= sizeof exh_mem ? exh_mem + offset : NULL;
}

static int run_exhaustive_decoders(void)
{
    const int fails_before = fails;
    enum { N = 65536 };
    static rsx_vertex_ref refs[N];
    static u8 oa[N * 16], ob[N * 16], oc[N * 16];
    for (u32 i = 0; i < N; i++) {
        refs[i].vertex_id = i;
        refs[i].base_index = 0;
    }
    u64 checked = 0;
    for (u32 type = 1; type <= 7; type++)
        for (u32 size = 1; size <= 5; size++) {
            const u32 csize = rsx_vertex_attrib_size(type, size);
            const u32 stride = csize;
            /* Component c of vertex i holds pattern (i + c * 4099) for the
             * 16-bit types, (i + c * 61) & 0xFF for the 8-bit ones. */
            for (u32 i = 0; i < N; i++)
                for (u32 c = 0; c < (type == RSX_VTX_TYPE_CMP32 ? 1u : size); c++) {
                    u8* p = exh_mem + (size_t)i * stride;
                    switch (type) {
                    case RSX_VTX_TYPE_HALF: case RSX_VTX_TYPE_SNORM16: case RSX_VTX_TYPE_SINT16: {
                        const u32 v = (i + c * 4099u) & 0xFFFFu;
                        p[c * 2] = (u8)(v >> 8);
                        p[c * 2 + 1] = (u8)v;
                        break;
                    }
                    case RSX_VTX_TYPE_UNORM8: case RSX_VTX_TYPE_UINT8:
                        p[c] = (u8)(i + c * 61u);
                        break;
                    default: {
                        u32 v = rnd();
                        if (i % 7 == 0) v = (v & 0x807FFFFFu) | 0x7F800000u;   /* Inf/NaN */
                        if (i % 11 == 0) v &= 0x807FFFFFu;                     /* denormal */
                        p[c * 4] = (u8)(v >> 24); p[c * 4 + 1] = (u8)(v >> 16);
                        p[c * 4 + 2] = (u8)(v >> 8); p[c * 4 + 3] = (u8)v;
                        break;
                    }
                    }
                }
            rsx_vertex_fetch_plan pa, pb;
            memset(&pa, 0, sizeof pa);
            rsx_vertex_layout_plan_init(&pa.layout, 1u);
            pa.guest_ptr = exh_ptr;
            pa.freq_mode = RSX_VERTEX_FREQ_RPCS3_ARRAYS;
            pa.attr[0].desc.type = type;
            pa.attr[0].desc.size = size;
            pa.attr[0].desc.stride = stride;
            pa.attr[0].elem_size = csize;
            pa.attr[0].stride = stride;
            pb = pa;
            rsx_vertex_fetch_plan_prepare(&pa, refs, N);
            orig_fetch_plan_prepare(&pb, refs, N);
            CHECK(pa.fast_ready == 2, "exhaustive type %u size %u not on the plain path", type, size);
            int ka = 1, kb = 1;
            for (u32 i = 0; i < N; i++) {
                ka &= rsx_vertex_fetch_one(&pa, &refs[i], oa + (size_t)i * 16);
                kb &= orig_fetch_one(&pb, &refs[i], ob + (size_t)i * 16);
            }
            const int kc = rsx_vertex_fetch_many(&pa, refs, N, oc);
            CHECK(ka && kb && kc, "exhaustive type %u size %u fetch failed", type, size);
            CHECK(!memcmp(oa, ob, sizeof oa), "exhaustive type %u size %u fetch_one differs", type, size);
            CHECK(!memcmp(oc, ob, sizeof oc), "exhaustive type %u size %u fetch_many differs", type, size);
            checked += N;
        }
    printf("exhaustive decoders: %llu vertices, %d failure(s)\n",
           (unsigned long long)checked, fails - fails_before);
    return fails == fails_before;
}

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        run_bench();
        return 0;
    }
    const int exhaustive_ok = run_exhaustive_decoders();
    const int differential_ok = run_differential(argc > 1 ? atoi(argv[1]) : 20000);
    if (!exhaustive_ok || !differential_ok)
        return 1;
    enum { PLANS = 6000, MAXV = 200 };
    static rsx_vertex_ref refs[MAXV];
    static u8 inl[8192];
    int stable = 0, divided = 0;

    for (int n = 0; n < PLANS; n++) {
        for (int l = 0; l < 2; l++)
            mapped_end[l] = (rnd() % 3 == 0) ? rnd() % GUEST : GUEST;
        rsx_vertex_fetch_plan plan, ref;
        random_plan(&plan, rnd() % 5 == 0, inl, (u32)(rnd() % sizeof inl));

        const u32 count = 1 + rnd() % MAXV;
        const u32 base = (rnd() % 4 == 0) ? rnd() % 64u : 0;
        /* Some runs sit just below the 20-bit element wrap and cross it. */
        const u32 start = (rnd() % 4 == 0) ? 0x000FFFFFu - rnd() % 400u : 0;
        for (u32 i = 0; i < count; i++) {
            refs[i].vertex_id = start + ((rnd() % 8 == 0) ? rnd() : rnd() % 300u);
            refs[i].base_index = base;
        }
        ref = plan;
        rsx_vertex_fetch_plan_prepare(&plan, refs, count);
        ref_prepare(&ref, refs, count);
        for (u32 at = 0; at < RSX_DSP_NUM_VERTEX_ATTR; at++) {
            CHECK(plan.attr[at].stable_base == ref.attr[at].stable_base &&
                  plan.attr[at].stable_first == ref.attr[at].stable_first &&
                  plan.attr[at].stable_last == ref.attr[at].stable_last &&
                  plan.attr[at].stride == ref.attr[at].stride,
                  "plan %d attr %u: span %u..%u, walk %u..%u", n, at,
                  plan.attr[at].stable_first, plan.attr[at].stable_last,
                  ref.attr[at].stable_first, ref.attr[at].stable_last);
            if (ref.attr[at].stable_base) {
                stable++;
                if (plan.attr[at].desc.frequency > 1) divided++;
            }
        }
    }

    printf("test_vertex_fetch_plan: %d plans, %d stable ranges (%d divided), %d failure(s)\n",
           PLANS, stable, divided, fails);
    if (stable < PLANS || divided < 100) {
        printf("FAIL: too few cases exercised a path\n");
        return 1;
    }
    return fails ? 1 : 0;
}
