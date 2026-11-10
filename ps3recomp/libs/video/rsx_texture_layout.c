/*
 * ps3recomp - RSX texture layout (see rsx_texture_layout.h)
 */
#include "rsx_texture_layout.h"

#include <stdlib.h>   /* getenv, atoi */
#include <string.h>   /* memcpy       */

u32 rsx_log2_ceil(u32 v)
{
    u32 l = 0;
    while ((1u << l) < v) l++;
    return l;
}

u32 rsx_swizzle_offset(u32 x, u32 y, u32 log2w, u32 log2h)
{
    u32 off = 0, shift = 0;
    while (log2w && log2h) {
        off |= (x & 1u) << shift; x >>= 1; shift++;
        off |= (y & 1u) << shift; y >>= 1; shift++;
        log2w--; log2h--;
    }
    off |= (x | y) << shift;     /* only one of x/y still has bits */
    return off;
}

u32 rsx_swizzle_offset_3d(u32 x, u32 y, u32 z, u32 log2w, u32 log2h, u32 log2d)
{
    u32 off = 0, shift = 0;
    while (log2w || log2h || log2d) {
        if (log2w) { off |= (x & 1u) << shift; x >>= 1; shift++; log2w--; }
        if (log2h) { off |= (y & 1u) << shift; y >>= 1; shift++; log2h--; }
        if (log2d) { off |= (z & 1u) << shift; z >>= 1; shift++; log2d--; }
    }
    return off;
}

void rsx_texture_layout(u32 rsx_fmt, u32 w, u32 h, rsx_tex_layout* out)
{
    rsx_texture_layout_pitched(rsx_fmt, w, h, 0, out);
}

void rsx_texture_layout_pitched(u32 rsx_fmt, u32 w, u32 h, u32 pitch,
                                rsx_tex_layout* out)
{
    if (!out) return;
    const int has_border = (rsx_fmt & RSX_TEX_FMT_HAS_BORDER) && !rsx_texture_legacy();
    rsx_fmt &= 0xFFu;

    /* Format classes (base = fmt & 0x9F, masking off the LN/UN flag bits).
     * The LBP loading screen exercises the first group: 0x85 A8R8G8B8
     * (swizzled UI art), 0x8B G8B8 (the 1024x2048 linear font atlas -- without
     * it no text renders at all), 0x86/87/88 DXT1/23/45 (512x512 detail and
     * LUT layers bound on every draw). The rest are what the live draw engine
     * reached for once a real title's frame was being drawn. */
    u32 basef = rsx_fmt & 0x9Fu;
    u32 src_bpp = 1, dst_bpp = 1;

    out->base_format    = basef;
    out->compressed     = 0;
    out->block_bytes    = 0;
    out->fmt            = RSX_TEXFMT_R8;

    /* RPCS3 upload_texture_subresource / get_compatible_sampler_format for
     * the formats the legacy table below misclassified (RSX_TEX_LEGACY=1
     * restores it): the 4:2:2 pairs and R6G5B5 decode to RGBA8, the float
     * depth formats to R32F, HILO8 is two unorm bytes and HILO_S8 (an
     * R8G8_SNORM view on RPCS3) is decoded to signed halves. 0x8C is not a
     * texture format. */
    if (!rsx_texture_legacy()) {
        int handled = 1;
        switch (basef) {
        case 0x8D: case 0x8E: case 0x8F:
            out->fmt = RSX_TEXFMT_R8G8B8A8; src_bpp = 2; dst_bpp = 4; break;
        case 0x91: out->fmt = RSX_TEXFMT_R32F; src_bpp = dst_bpp = 4; break;
        case 0x93: out->fmt = RSX_TEXFMT_R32F; src_bpp = 2; dst_bpp = 4; break;
        case 0x98: out->fmt = RSX_TEXFMT_R8G8; src_bpp = dst_bpp = 2; break;
        case 0x99: out->fmt = RSX_TEXFMT_R16G16F; src_bpp = 2; dst_bpp = 4; break;
        case 0x8C: break;
        default: handled = 0; break;
        }
        if (handled) basef = 0x100u;   /* skip the legacy table */
    }

    switch (basef) {
    /* Four bytes, A,R,G,B. D8R8G8B8 is the same image with no alpha channel:
     * the byte is there but means nothing, so the decode forces it to one
     * rather than letting whatever the guest left behind modulate a draw. */
    case 0x85: case 0x9E:
        out->fmt = RSX_TEXFMT_R8G8B8A8; src_bpp = dst_bpp = 4; break;
    /* Two 8-bit channels. HILO is a normal-map pair; HILO_S8's channels are
     * signed, which is not modelled -- neither reference backend samples it,
     * so there is nothing to mirror and unorm is the conservative reading. */
    case 0x8B: case 0x8C: case 0x8D:
        out->fmt = RSX_TEXFMT_R8G8;     src_bpp = dst_bpp = 2; break;
    /* Packed 16-bit colour: two source bytes unpacked to four host ones. */
    case 0x82: case 0x83: case 0x84: case 0x97: case 0x9D:
        out->fmt = RSX_TEXFMT_R8G8B8A8; src_bpp = 2; dst_bpp = 4; break;
    case 0x86: out->fmt = RSX_TEXFMT_BC1; out->compressed = 1; out->block_bytes = 8;  break;
    case 0x87: out->fmt = RSX_TEXFMT_BC2; out->compressed = 1; out->block_bytes = 16; break;
    case 0x88: out->fmt = RSX_TEXFMT_BC3; out->compressed = 1; out->block_bytes = 16; break;
    /* Depth read as colour. Both sample as a single red channel: a shader
     * reading one of these wants the depth value, and spreading it across RGB
     * would only make the crossbar's job ambiguous. DEPTH24_D8's 24-bit
     * integer has no host format, so it is normalised into a float, which is
     * exact -- 24 bits fit a float32 mantissa. */
    case 0x90: out->fmt = RSX_TEXFMT_R32F;  src_bpp = dst_bpp = 4; break;
    case 0x92: case 0x94:
        out->fmt = RSX_TEXFMT_R16;      src_bpp = dst_bpp = 2; break;
    case 0x95: out->fmt = RSX_TEXFMT_R16G16; src_bpp = dst_bpp = 4; break;
    /* Float render targets sampled back as textures. Host formats take these
     * bit for bit; only the guest's big-endian component order changes. */
    case 0x9A: out->fmt = RSX_TEXFMT_R16G16B16A16F; src_bpp = dst_bpp = 8;  break;
    case 0x9B: out->fmt = RSX_TEXFMT_R32G32B32A32F; src_bpp = dst_bpp = 16; break;
    case 0x9C: out->fmt = RSX_TEXFMT_R32F;   src_bpp = dst_bpp = 4; break;
    case 0x9F: out->fmt = RSX_TEXFMT_R16G16F; src_bpp = dst_bpp = 4; break;
    default:   break;   /* R8, one byte per texel -- the pre-existing fallback */
    }

    if (out->compressed) {
        /* Compressed data is stored as linear rows of 4x4 blocks, and is never
         * Morton-swizzled on RSX. */
        out->bytes_per_texel = 0;
        out->swizzled = 0;
        out->row_bytes = ((w + 3u) / 4u) * out->block_bytes;
        out->rows      = (h + 3u) / 4u;
        out->dst_bytes_per_texel = 0;
        out->dst_row_bytes       = out->row_bytes;
        /* Resistance port: a LINEAR compressed image keeps the pitch register
         * as its source stride between block rows; the host copy stays
         * packed (dst_row_bytes). */
        if ((rsx_fmt & 0x20u) && pitch > out->row_bytes)
            out->row_bytes = pitch;
    } else {
        /* Swizzled unless the LN bit is set. RPCS3 (texture_cache.h
         * is_swizzled = !(format & LN)) has no power-of-two condition: a
         * NPOT swizzled image is Morton-ordered over ceil-log2 extents
         * (calculate_z_index). The legacy layout reads NPOT as linear. */
        out->swizzled  = !(rsx_fmt & 0x20u) && w && h &&
                         (!rsx_texture_legacy() ||
                          ((w & (w - 1u)) == 0u && (h & (h - 1u)) == 0u));
        out->bytes_per_texel = src_bpp;
        out->row_bytes = w * src_bpp;
        /* A swizzled image has no rows to space out, so the pitch register
         * only applies to a linear one. */
        if (pitch > out->row_bytes && (rsx_fmt & 0x20u))
            out->row_bytes = pitch;
        out->rows      = h;
        out->dst_bytes_per_texel = dst_bpp;
        out->dst_row_bytes       = w * dst_bpp;
        /* B8R8_G8R8 / R8B8_R8G8: 32-bit blocks of two texels, never swizzled
         * (the firmware value carries LN; RPCS3 copy_decoded_rb_rg_block). */
        if (!rsx_texture_legacy() &&
            (out->base_format == 0x8Du || out->base_format == 0x8Eu)) {
            out->swizzled  = 0;
            out->row_bytes = ((w + 1u) / 2u) * 4u;
            /* RPCS3 upload_texture: a swizzled (no LN) image uses the packed
             * pitch whatever the register holds. */
            if (pitch > out->row_bytes && (rsx_fmt & 0x20u)) out->row_bytes = pitch;
        }
    }

    out->face_bytes = out->row_bytes * out->rows;
    out->border = out->pad_w = out->pad_h = 0;

    /* RPCS3 get_subresources_layout_impl / get_texture_size with border
     * texels (DXT and the 4:2:2 pairs never carry one): a linear image has a
     * one-texel porch on every side within its pitch, h + 2 rows; a swizzled
     * one is Morton-ordered over next_pow2(w + 8) x next_pow2(h + 8) with a
     * four-texel porch. */
    if (has_border && !out->compressed && out->bytes_per_texel &&
        out->base_format != 0x8Du && out->base_format != 0x8Eu) {
        const u32 bpp = out->bytes_per_texel;
        if (out->swizzled) {
            out->border = 4;
            out->pad_w = 1u << rsx_log2_ceil(w + 8u);
            out->pad_h = 1u << rsx_log2_ceil(h + 8u);
            out->face_bytes = out->pad_w * out->pad_h * bpp;
        } else {
            out->border = 1;
            /* RPCS3: a nonzero pitch narrower than the porched row is kept
             * (the width shrinks to fit); only a zero pitch packs. */
            if (!pitch && out->row_bytes < (w + 2u) * bpp) out->row_bytes = (w + 2u) * bpp;
            out->face_bytes = out->row_bytes * (h + 2u);
        }
    }
}

u32 rsx_texture_mip_chain(u32 rsx_fmt, u32 w, u32 h, u32 levels, u32 pitch,
                          rsx_tex_level* out)
{
    if (!out || !w || !h) return 0;
    if (levels == 0) levels = 1;
    if (levels > RSX_MAX_TEXTURE_LEVELS) levels = RSX_MAX_TEXTURE_LEVELS;
    /* RPCS3 get_exact_mipmap_count: a DXT chain ends where either side
     * reaches one block, floor_log2(min(w/4, h/4)) + 1 levels. */
    const u32 base = rsx_fmt & 0x9Fu;
    if (!rsx_texture_legacy() && base >= 0x86u && base <= 0x88u) {
        const u32 mb = (w / 4u < h / 4u) ? w / 4u : h / 4u;
        u32 exact = 1;
        for (u32 v = mb; v > 1u; v >>= 1) exact++;
        if (levels > exact) levels = exact;
    }

    u32 mw = w, mh = h, off = 0, n = 0;
    while (n < levels) {
        rsx_tex_level* lv = &out[n];
        lv->w = mw; lv->h = mh; lv->offset = off;
        /* RPCS3 get_subresources_layout_impl: a linear (LN) image, DXT
         * included, is padded_row -- every level uses the pitch register as
         * its row stride. A swizzled one packs each level at its own width. */
        const u32 lv_pitch = (n == 0 || ((rsx_fmt & 0x20u) && !rsx_texture_legacy())) ? pitch : 0;
        rsx_texture_layout_pitched(rsx_fmt, mw, mh, lv_pitch, &lv->tl);
        off += lv->tl.face_bytes;
        n++;
        if (mw == 1 && mh == 1) break;   /* no level below 1x1 */
        if (mw > 1) mw >>= 1;
        if (mh > 1) mh >>= 1;
    }
    return n;
}

u32 rsx_texture_cube_face_stride(u32 rsx_fmt, u32 w, u32 h, u32 levels,
                                 u32 pitch)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(rsx_fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    const u32 total = lv[n - 1].offset + lv[n - 1].tl.face_bytes;
    /* Only swizzled images align each face to 128 bytes. */
    if ((rsx_fmt & 0x20u) && !rsx_texture_legacy()) return total;
    return (total + 127u) & ~127u;
}

int rsx_texture_argb_is_rgba(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("TEX_RGBA");
        cached = e ? atoi(e) : 0;
    }
    return cached;
}

int rsx_texture_legacy(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("RSX_TEX_LEGACY");
        cached = (e && atoi(e)) ? 1 : 0;
    }
    return cached;
}

/* RPCS3 rsx_state::init seeds every CONTROL1 to 0xAAE4 and decodes a written
 * 0 as-is (all lanes ZERO). RSX_TEX_REMAP_ZERO=0 (or RSX_TEX_LEGACY) reads 0
 * as the identity again. */
u32 rsx_texture_control1(u32 control1)
{
    static int zero = -1;
    if (zero < 0) { const char* e = getenv("RSX_TEX_REMAP_ZERO"); zero = !(e && e[0] == '0'); }
    if (!(control1 & 0xFFFFu) && (!zero || rsx_texture_legacy())) return 0xAAE4u;
    return control1;
}

/* IEEE half to float bits (RPCS3 cs_fconvert_task<f16, f32>). */
static float half_to_float(u32 h)
{
    const u32 sign = (h & 0x8000u) << 16;
    u32 exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu, bits;
    if (exp == 0x1Fu) {
        bits = sign | 0x7F800000u | (man << 13);
    } else if (exp) {
        bits = sign | ((exp + 112u) << 23) | (man << 13);
    } else if (man) {
        exp = 113u;
        while (!(man & 0x400u)) { man <<= 1; exp--; }
        bits = sign | (exp << 23) | ((man & 0x3FFu) << 13);
    } else {
        bits = sign;
    }
    float f; memcpy(&f, &bits, sizeof f);
    return f;
}

/* Float to IEEE half bits, round to nearest; only used for values in [-1,1]. */
static u16 float_to_half(float f)
{
    u32 b; memcpy(&b, &f, sizeof b);
    const u32 sign = (b >> 16) & 0x8000u;
    const int exp = (int)((b >> 23) & 0xFFu) - 127 + 15;
    u32 man = b & 0x7FFFFFu;
    if (exp <= 0) {
        if (exp < -10) return (u16)sign;
        man |= 0x800000u;
        const u32 shift = (u32)(14 - exp);
        u32 hm = man >> shift;
        if ((man >> (shift - 1u)) & 1u) hm++;
        return (u16)(sign | hm);
    }
    if (exp >= 31) return (u16)(sign | 0x7C00u);
    u32 hv = ((u32)exp << 10) | (man >> 13);
    if (man & 0x1000u) hv++;
    return (u16)(sign | hv);
}

/* A 5- or 6-bit field widened to 8 bits, rounded to nearest (RPCS3's
 * software expansion, (v * 527 + 23) >> 6; its native packed host formats
 * convert to v / 31 exactly, which this is the nearest byte to). The legacy
 * truncation stays under RSX_TEX_LEGACY. A 4-bit field widens exactly. */
static u8 expand5(u32 v)
{
    return rsx_texture_legacy() ? (u8)(v * 255u / 31u) : (u8)((v * 527u + 23u) >> 6);
}
static u8 expand6(u32 v)
{
    return rsx_texture_legacy() ? (u8)(v * 255u / 63u) : (u8)((v * 259u + 33u) >> 6);
}
static u8 expand4(u32 v) { return (u8)(v * 17u); }

/* Guest half-words and words are big-endian. */
static u32 be16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }
static u32 be32(const u8* p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* `n` components of `width` bytes, big-endian in guest memory and
 * little-endian on every host this runs on. */
static void swap_components(u8* d, const u8* s, u32 n, u32 width)
{
    for (u32 c = 0; c < n; c++)
        for (u32 b = 0; b < width; b++)
            d[c * width + b] = s[c * width + (width - 1u - b)];
}

/* One source texel at `s` to one host texel at `d`, which receives
 * tl->dst_bytes_per_texel bytes. */
static void decode_texel(const rsx_tex_layout* tl, const u8* s, u8* d,
                         int argb_as_rgba)
{
    switch (tl->base_format) {
    case 0x85:            /* A8R8G8B8 */
    case 0x9E:            /* D8R8G8B8: the alpha byte is not a channel */
        if (argb_as_rgba) {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        } else {
            /* guest A,R,G,B -> host R,G,B,A */
            d[0] = s[1]; d[1] = s[2]; d[2] = s[3]; d[3] = s[0];
        }
        if (tl->base_format == 0x9E) d[3] = 255;
        return;

    /* Packed 16-bit colour. RSX numbers these fields from the high bit of a
     * big-endian half-word, so the half-word is read that way and each field
     * widened to a byte. */
    case 0x82: case 0x9D: {   /* A1R5G5B5, and D1R5G5B5 with no alpha */
        const u32 v = be16(s);
        d[0] = expand5((v >> 10) & 0x1Fu);
        d[1] = expand5((v >>  5) & 0x1Fu);
        d[2] = expand5( v        & 0x1Fu);
        d[3] = (tl->base_format == 0x9D) ? 255u
             : (u8)((v & 0x8000u) ? 255u : 0u);
        return;
    }
    case 0x83: {              /* A4R4G4B4 */
        const u32 v = be16(s);
        d[0] = expand4((v >> 8) & 0xFu);
        d[1] = expand4((v >> 4) & 0xFu);
        d[2] = expand4( v       & 0xFu);
        d[3] = expand4((v >> 12) & 0xFu);
        return;
    }
    case 0x84: {              /* R5G6B5 */
        const u32 v = be16(s);
        d[0] = expand5((v >> 11) & 0x1Fu);
        d[1] = expand6((v >>  5) & 0x3Fu);
        d[2] = expand5( v        & 0x1Fu);
        d[3] = 255;
        return;
    }
    case 0x97: {              /* R5G5B5A1: alpha is the LOW bit, not the high */
        const u32 v = be16(s);
        d[0] = expand5((v >> 11) & 0x1Fu);
        d[1] = expand5((v >>  6) & 0x1Fu);
        d[2] = expand5((v >>  1) & 0x1Fu);
        d[3] = (u8)((v & 1u) ? 255u : 0u);
        return;
    }

    case 0x90: {              /* DEPTH24_D8: depth in the top 24 bits */
        const float f = (float)(be32(s) >> 8) / 16777215.0f;
        memcpy(d, &f, sizeof f);
        return;
    }

    /* Only reached outside RSX_TEX_LEGACY (the legacy layout gives these
     * other classes). RPCS3 convert_rgb655_to_bgra8 widening, alpha opaque
     * as through its R5G6B5 view. */
    case 0x8F:
        if (tl->fmt != RSX_TEXFMT_R8G8B8A8) break;
        {
            const u32 v = be16(s);
            d[0] = (u8)((((v >> 10) & 0x3Fu) * 259u + 33u) >> 6);
            d[1] = (u8)((((v >>  5) & 0x1Fu) * 527u + 23u) >> 6);
            d[2] = (u8)((( v        & 0x1Fu) * 527u + 23u) >> 6);
            d[3] = 255;
        }
        return;
    case 0x91:                /* DEPTH24_D8_FLOAT: d24f_to_f32 = bits << 7 */
        if (tl->fmt != RSX_TEXFMT_R32F) break;
        {
            const u32 bits = (be32(s) >> 8) << 7;
            memcpy(d, &bits, 4);
        }
        return;
    case 0x93:                /* DEPTH16_FLOAT: a half, widened (D32_SFLOAT) */
        if (tl->fmt != RSX_TEXFMT_R32F) break;
        {
            const float f = half_to_float(be16(s));
            memcpy(d, &f, 4);
        }
        return;
    case 0x99:                /* HILO_S8: two snorm bytes as halves */
        if (tl->fmt != RSX_TEXFMT_R16G16F) break;
        for (u32 c = 0; c < 2; c++) {
            float f = (float)(signed char)s[c] / 127.0f;
            if (f < -1.0f) f = -1.0f;
            const u16 hv = float_to_half(f);
            memcpy(d + c * 2u, &hv, 2);
        }
        return;

    default:
        break;
    }

    /* Everything left keeps its bits. One byte per component needs nothing
     * done to it; wider components are swapped out of big-endian. */
    switch (tl->fmt) {
    case RSX_TEXFMT_R16:             swap_components(d, s, 1, 2); return;
    case RSX_TEXFMT_R16G16:
    case RSX_TEXFMT_R16G16F:         swap_components(d, s, 2, 2); return;
    case RSX_TEXFMT_R16G16B16A16F:   swap_components(d, s, 4, 2); return;
    case RSX_TEXFMT_R32F:            swap_components(d, s, 1, 4); return;
    case RSX_TEXFMT_R32G32B32A32F:   swap_components(d, s, 4, 4); return;
    default:
        for (u32 b = 0; b < tl->dst_bytes_per_texel; b++) d[b] = s[b];
        return;
    }
}

void rsx_texture_decode(void* dst, u32 dst_pitch,
                        const u8* src, u32 w, u32 h,
                        const rsx_tex_layout* tl, int argb_as_rgba)
{
    if (!dst || !src || !tl || !w || !h) return;

    u8* d = (u8*)dst;

    if (tl->compressed) {
        /* BC1/2/3 are bit-identical to DXT1/23/45, so the payload is copied
         * rather than converted -- only the row stride changes. */
        for (u32 y = 0; y < tl->rows; y++)
            memcpy(d + (size_t)y * dst_pitch,
                   src + (size_t)y * tl->row_bytes, tl->dst_row_bytes);
        return;
    }

    const u32 l2w = rsx_log2_ceil(w), l2h = rsx_log2_ceil(h);
    const u32 sbpp = tl->bytes_per_texel, dbpp = tl->dst_bytes_per_texel;

    /* Border texels (RPCS3 copy_unmodified_block[_swizzled] with border):
     * a linear image starts one row and one texel into its porch; a
     * swizzled one reads (x + 4, y + 4) of its padded Morton square, words
     * of a texel wider than 32 bits each at its own offset. */
    if (tl->border) {
        rsx_tex_layout ltl = *tl;
        ltl.border = 0;
        if (!tl->swizzled) {
            rsx_texture_decode(dst, dst_pitch, src + tl->row_bytes + sbpp, w, h, &ltl, argb_as_rgba);
            return;
        }
        const u32 wpt = sbpp > 4u ? sbpp / 4u : 1u, esz = sbpp > 4u ? 4u : sbpp;
        const u32 l2pw = rsx_log2_ceil(tl->pad_w * wpt), l2ph = rsx_log2_ceil(tl->pad_h);
        u8 tmp[16];
        for (u32 y = 0; y < h; y++) {
            u8* drow = d + (size_t)y * dst_pitch;
            for (u32 x = 0; x < w; x++) {
                for (u32 k = 0; k < wpt; k++)
                    memcpy(tmp + k * esz,
                           src + (size_t)rsx_swizzle_offset((x + 4u) * wpt + k, y + 4u, l2pw, l2ph) * esz,
                           esz);
                decode_texel(tl, tmp, drow + (size_t)x * dbpp, argb_as_rgba);
            }
        }
        return;
    }

    /* B8R8_G8R8 / R8B8_R8G8 (RPCS3 copy_decoded_rb_rg_block): each 32-bit
     * block holds two texels sharing B and G, read as bytes
     * B,R0,G,R1 and R0,B,R1,G respectively. Linear only. */
    if ((tl->base_format == 0x8Du || tl->base_format == 0x8Eu) &&
        tl->fmt == RSX_TEXFMT_R8G8B8A8 && sbpp == 2u) {
        const int br = tl->base_format == 0x8Du;
        for (u32 y = 0; y < h; y++) {
            u8* drow = d + (size_t)y * dst_pitch;
            const u8* srow = src + (size_t)y * tl->row_bytes;
            for (u32 x = 0; x < w; x++) {
                const u8* b = srow + (size_t)(x >> 1) * 4u;
                const u8 r = br ? b[(x & 1u) ? 3 : 1] : b[(x & 1u) ? 2 : 0];
                drow[x * 4u + 0] = r;
                drow[x * 4u + 1] = br ? b[2] : b[3];
                drow[x * 4u + 2] = br ? b[0] : b[1];
                drow[x * 4u + 3] = 255;
            }
        }
        return;
    }

    /* RPCS3 copy_unmodified_block_swizzled: RSX swizzles at most 32-bit
     * blocks, so a swizzled 64/128-bit texel is bpp/4 words of a u32 image
     * (bpp/4) times as wide, each word at its own Morton offset. */
    if (tl->swizzled && sbpp > 4u && !rsx_texture_legacy()) {
        const u32 wpt = sbpp / 4u;
        const u32 l2ww = rsx_log2_ceil(w * wpt);
        u8 tmp[16];
        for (u32 y = 0; y < h; y++) {
            u8* drow = d + (size_t)y * dst_pitch;
            for (u32 x = 0; x < w; x++) {
                for (u32 k = 0; k < wpt; k++)
                    memcpy(tmp + k * 4u,
                           src + (size_t)rsx_swizzle_offset(x * wpt + k, y, l2ww, l2h) * 4u, 4);
                decode_texel(tl, tmp, drow + (size_t)x * dbpp, argb_as_rgba);
            }
        }
        return;
    }

    /* R8 and R8G8 reach the host byte for byte. A linear one is then a
     * straight row copy, which is worth keeping as a separate path: it is the
     * common case (Bink video planes, the G8B8 font atlas) and by far the
     * faster one. G8B8's channel placement is the sampler's component remap,
     * not this. */
    if ((tl->fmt == RSX_TEXFMT_R8 || tl->fmt == RSX_TEXFMT_R8G8) && !tl->swizzled) {
        for (u32 y = 0; y < h; y++)
            memcpy(d + (size_t)y * dst_pitch,
                   src + (size_t)y * tl->row_bytes, (size_t)w * sbpp);
        return;
    }

    /* Resistance port (v127): the per-texel loop below spent most of its time
     * re-deriving the Morton offset bit by bit (rsx_swizzle_offset) and
     * switching on the format for every texel -- decode_texel and this
     * function were the render thread's top entries while York streamed in
     * new textures. The x and y bits of a Morton offset land in disjoint
     * positions, so offset(x, y) = offset(x, 0) | offset(0, y): two per-axis
     * tables make it one OR. A8R8G8B8 (most of the game's art) gets its own
     * loop, a 32-bit rotate per texel. Output is identical; RSX_TEXDEC_FAST=0
     * restores the old loop. */
    {
        static int fast = -1;
        if (fast < 0) { const char* e = getenv("RSX_TEXDEC_FAST"); fast = e ? atoi(e) : 1; }
        static const u32 one = 1u;
        const int little = *(const u8*)&one == 1u;
        static _Thread_local u32* s_tab = NULL;      /* tx[4096] then ty[4096] */
        if (fast && w <= 4096u && h <= 4096u && sbpp &&
            (s_tab || (s_tab = (u32*)malloc(2u * 4096u * sizeof(u32))) != NULL)) {
            u32* tx = s_tab; u32* ty = s_tab + 4096;
            const int swz = tl->swizzled;
            if (swz) {
                for (u32 x = 0; x < w; x++) tx[x] = rsx_swizzle_offset(x, 0, l2w, l2h);
                for (u32 y = 0; y < h; y++) ty[y] = rsx_swizzle_offset(0, y, l2w, l2h);
            }
            const u32 bf = tl->base_format;
            if ((bf == 0x85u || bf == 0x9Eu) && sbpp == 4u && dbpp == 4u && little) {
                const u32 amask = (bf == 0x9Eu) ? 0xFF000000u : 0u;
                for (u32 y = 0; y < h; y++) {
                    u8* drow = d + (size_t)y * dst_pitch;
                    const u8* srow = src + (size_t)y * tl->row_bytes;
                    const u32 yo = swz ? ty[y] : 0u;
                    for (u32 x = 0; x < w; x++) {
                        const u8* sp = swz ? src + (size_t)(tx[x] | yo) * 4u : srow + (size_t)x * 4u;
                        u32 v; memcpy(&v, sp, 4);
                        if (!argb_as_rgba) v = (v >> 8) | (v << 24);   /* A,R,G,B -> R,G,B,A */
                        v |= amask;
                        memcpy(drow + (size_t)x * 4u, &v, 4);
                    }
                }
                return;
            }
            for (u32 y = 0; y < h; y++) {
                u8* drow = d + (size_t)y * dst_pitch;
                const u8* srow = src + (size_t)y * tl->row_bytes;
                const u32 yo = swz ? ty[y] : 0u;
                for (u32 x = 0; x < w; x++) {
                    const u8* sp = swz ? src + (size_t)(tx[x] | yo) * sbpp : srow + (size_t)x * sbpp;
                    decode_texel(tl, sp, drow + (size_t)x * dbpp, argb_as_rgba);
                }
            }
            return;
        }
    }

    /* Morton order interleaves the low bits of x and y, so a swizzled image
     * has to be walked texel by texel; so does anything the host cannot take
     * as stored. */
    for (u32 y = 0; y < h; y++) {
        u8* drow = d + (size_t)y * dst_pitch;
        for (u32 x = 0; x < w; x++) {
            const u8* s = tl->swizzled
                ? src + (size_t)rsx_swizzle_offset(x, y, l2w, l2h) * sbpp
                : src + (size_t)y * tl->row_bytes + (size_t)x * sbpp;
            decode_texel(tl, s, drow + (size_t)x * dbpp, argb_as_rgba);
        }
    }
}

u32 rsx_texture_decoded_remap(u32 control1, u32 rsx_fmt)
{
    control1 = rsx_texture_control1(control1);
    const u32 ovr = (control1 >> 16) & 0xFFFFu;
    switch (rsx_fmt & 0x9Fu) {
    case 0x9A: case 0x9B: case 0x9C:
        /* Float textures cannot be remapped on real hardware. */
        if (rsx_texture_legacy()) break;
        return (control1 & ~0xFFu) | 0xE4u;
    case 0x9F:
        if (rsx_texture_legacy()) break;
        return (control1 & ~0xFFu) | (ovr ? 0x56u : 0x66u);
    case 0x94:
        if (rsx_texture_legacy()) break;
        /* fall through */
    case 0x95: case 0x98: case 0x99: {
        u32 lo = control1 & 0xFFu;
        switch (lo) {
        case 0xE4: lo = ovr ? 0x56u : 0x66u; break;
        case 0x4E: lo = ovr ? 0xA9u : 0x99u; break;
        case 0xEE: lo = 0xAAu; break;
        case 0x44: lo = 0x55u; break;
        default: break;
        }
        return (control1 & ~0xFFu) | lo;
    }
    default: break;
    }
    return control1;
}

void rsx_texture_component_remap(u32 control1, u32 rsx_fmt, u8 out[4])
{
    if (!out) return;

    /* Source codes index the presented vector {A,R,G,B}. The uploaded resource
     * holds R,G,B,A at components 0..3, so A is component 3 and R,G,B are
     * 0,1,2 -- that is what lanes_argb says. G8B8 only has two real channels,
     * and the sampler presents them as {G,R,G,R}. B8 decodes to a one-channel
     * resource whose byte is presented as R, G and B with an opaque A, which
     * is what the D3D12 live engine's CPU decode builds (decode_texel); read
     * through lanes_argb its B source would be component 2, always zero.
     * X16 is one 16-bit channel decoded to R16 and is presented the same way:
     * God of War: Ascension samples its X16 font atlas through crossbar 0xAAEE
     * (every output from G or B), which read zeros -- all menu text invisible. */
    static const u8 lanes_argb[4] = {3, 0, 1, 2};
    static const u8 lanes_g8b8[4] = {1, 0, 1, 0};
    static const u8 lanes_b8[4]   = {RSX_REMAP_ONE, 0, 0, 0};
    const u32 base = rsx_fmt & 0x9Fu;

    if (!rsx_texture_legacy()) {
        /* RPCS3 vk::get_component_mapping (per-format, ARGB order) applied
         * through fragment_texture::decoded_remap, as
         * apply_component_mapping_flags does. Entries are in OUR decoded
         * resource's components: G8B8/HILO8 keep the guest bytes unswapped
         * (component 0 = first byte), where RPCS3 uploads them as byteswapped
         * u16s -- so its VK R is our component 1 and its G our 0. Depth
         * formats present their value on every lane, ignoring CONTROL1. */
        static const u8 m_g8b8[4]  = {0, 1, 0, 1};       /* {G,R,G,R} */
        static const u8 m_hilo8[4] = {1, 0, 1, 0};       /* {R,G,R,G} */
        static const u8 m_rg[4]    = {0, 1, 0, 1};       /* {R,G,R,G}: halves decoded in order */
        static const u8 m_y16x16[4]= {1, 0, 1, 0};       /* {G,R,G,R} */
        static const u8 m_x16[4]   = {0, RSX_REMAP_ONE, 0, RSX_REMAP_ONE};
        static const u8 m_rrrr[4]  = {0, 0, 0, 0};
        static const u8 m_one_rgb[4] = {RSX_REMAP_ONE, 0, 1, 2};
        const u8* map;
        switch (base) {
        case 0x90: case 0x91: case 0x92: case 0x93:
            out[0] = out[1] = out[2] = out[3] = 0;
            return;
        case 0x81: map = lanes_b8; break;
        case 0x8B: map = m_g8b8; break;
        case 0x94: map = m_x16; break;
        case 0x95: map = m_y16x16; break;
        case 0x98: case 0x99: map = m_hilo8; break;
        case 0x9F: map = m_rg; break;
        case 0x9C: map = m_rrrr; break;
        case 0x9D: case 0x9E: map = m_one_rgb; break;
        default:   map = lanes_argb; break;
        }
        const u32 c1 = rsx_texture_decoded_remap(control1, rsx_fmt);
        for (int i = 0; i < 4; i++) {
            const u32 s  = (c1 >> (i * 2)) & 3u;
            const u32 op = (c1 >> (8 + i * 2)) & 3u;
            out[i] = (op == 0) ? (u8)RSX_REMAP_ZERO
                   : (op == 1) ? (u8)RSX_REMAP_ONE
                               : map[s];
        }
        return;
    }

    const u8* src2res = (base == 0x8Bu) ? lanes_g8b8
                      : (base == 0x81u || base == 0x94u) ? lanes_b8 : lanes_argb;

    control1 = rsx_texture_control1(control1);

    /* Y16_X16 (and HILO8): two 16-bit channels whose crossbar is encoded in
     * 16-bit blocks -- 0xE4 or 0x4E only -- and presented interleaved
     * R-G-R-G, or R-R-R-G when CONTROL1's high word (the remap override,
     * bit 16) is set (RPCS3 fragment_texture::decoded_remap, verified on
     * hardware with Virtua Fighter 5 / Mass Effect 3). Through the plain
     * ARGB table a .w read got the resource's empty alpha, 1.0, instead of
     * Y: inFAMOUS 2's water reads its 64x64 wave map through .yw that way. */
    if (base == 0x95u || base == 0x98u || base == 0x99u) {
        const int ovr = (control1 >> 16) & 0xFFFFu;
        u32 lo = control1 & 0xFFu;
        switch (lo) {
        case 0xE4: lo = ovr ? 0x56u : 0x66u; break;
        case 0x4E: lo = ovr ? 0xA9u : 0x99u; break;
        case 0xEE: lo = 0xAAu; break;
        case 0x44: lo = 0x55u; break;
        default: break;
        }
        control1 = (control1 & ~0xFFu) | lo;
    }

    /* i runs A, R, G, B -- the crossbar's field order, LSB first. */
    for (int i = 0; i < 4; i++) {
        u32 s  = (control1 >> (i * 2)) & 3u;
        u32 op = (control1 >> (8 + i * 2)) & 3u;
        out[i] = (op == 0) ? (u8)RSX_REMAP_ZERO
               : (op == 1) ? (u8)RSX_REMAP_ONE
                           : src2res[s];
    }
}

/* ADDRESS gamma flags name final RGBA lanes, after remapping. Signed input
 * interpretation wins; its FILTER ARGB flags follow the source crossbar. */
u8 rsx_texture_gamma_mask(u32 format, u32 control1, u32 filter, u32 address)
{
    switch (format & 0x9Fu) {
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x88: case 0x8B: case 0x8D: case 0x8E: case 0x8F:
    case 0x97: case 0x9D: case 0x9E: break;
    default: return 0;
    }
    u8 gamma = (u8)((address >> 20) & 15u);
    if (!gamma) return 0;
    control1 = rsx_texture_control1(control1);
    u8 remap[4]; rsx_texture_component_remap(control1, format, remap);
    static const u8 rgba_lane[4] = {3, 0, 1, 2};
    for (u32 i = 0; i < 4; ++i) {
        const u32 source = (control1 >> (2u * i)) & 3u;
        if (!rsx_texture_legacy() && ((control1 >> (8u + 2u * i)) & 3u) != 2u)
            continue;   /* shuffle_mask_bits: only a REMAP lane carries a source flag */
        if (remap[i] < RSX_REMAP_ZERO && ((filter >> (28u + source)) & 1u))
            gamma &= (u8)~(1u << rgba_lane[i]);
    }
    return gamma;
}

u8 rsx_texture_expand_mask(u32 format, u32 control1, u32 filter, u32 address)
{
    /* RPCS3 rsx::get_format_features, RSX_FORMAT_FEATURE_BIASED_NORMALIZATION:
     * the 8-bit colour formats, the depth formats, X16, Y16_X16 and HILO8.
     * Floats and the 32-bit depth-as-colour reads have nothing to expand. */
    switch (format & 0x9Fu) {
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x88: case 0x8B: case 0x8D: case 0x8E: case 0x8F:
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x98:
    case 0x97: case 0x9D: case 0x9E: break;
    default: return 0;
    }
    /* RPCS3 RSXTexture.cpp:95-101: unsigned_remap() is the 4-bit field and
     * only BIASED (1) expands. RSX_TEX_REMAP_FIELD=0: bit 12 alone. */
    {
        static int field = -1;
        if (field < 0) { const char* e = getenv("RSX_TEX_REMAP_FIELD"); field = !(e && e[0] == '0'); }
        if (field ? ((address >> 12) & 0xFu) != 1u : !((address >> 12) & 1u)) return 0;
    }
    control1 = rsx_texture_control1(control1);
    u8 remap[4]; rsx_texture_component_remap(control1, format, remap);
    static const u8 rgba_lane[4] = {3, 0, 1, 2};
    u8 mask = 0;
    if (!rsx_texture_legacy()) {
        /* RPCS3 format_ex: decoded_remap().shuffle_mask_bits(0xF) less the
         * signed lanes, which exist only on SIGNED_COMPONENTS formats. */
        const u32 base = format & 0x9Fu;
        const int has_signed = !((base >= 0x90u && base <= 0x94u) || base == 0x98u);
        const u32 c1 = rsx_texture_decoded_remap(control1, format);
        for (u32 i = 0; i < 4; ++i) {
            const u32 source = (c1 >> (2u * i)) & 3u;
            if (((c1 >> (8u + 2u * i)) & 3u) == 2u &&   /* shuffle_mask_bits: REMAP only */
                !(has_signed && ((filter >> (28u + source)) & 1u)))
                mask |= (u8)(1u << rgba_lane[i]);
        }
    } else
    for (u32 i = 0; i < 4; ++i) {
        const u32 source = (control1 >> (2u * i)) & 3u;
        if (remap[i] < RSX_REMAP_ZERO && !((filter >> (28u + source)) & 1u))
            mask |= (u8)(1u << rgba_lane[i]);
    }
    /* Bit 4: 16-bit channels (X16, Y16_X16) re-centre on 32768/32767. */
    if (mask && ((format & 0x9Fu) == 0x94u || (format & 0x9Fu) == 0x95u)) mask |= 0x10u;
    return mask;
}
