/*
 * ps3recomp - platform-neutral RSX draw engine (see rsx_draw_engine.h)
 *
 * The orchestration carried across from libs/video/rsx_live_draw.c, the
 * NV4097 -> D3D12 engine a title has shipped on. Line numbers in the comments
 * below are that file's, so the two can be read side by side; the file itself
 * is vendored and stays exactly as it is.
 *
 * What is deliberately left out, because it is not renderer behaviour a title
 * needs: the shader disk cache, movie mode and its compositor, the a010
 * probe, and every YZ_PERF_PROFILE block.
 */
#include "rsx_draw_engine.h"

#include "rsx_fp_decompiler.h"
#include "rsx_primitives.h"
#include "rsx_restart_cuts.h"
#include "rsx_vertex_formats.h"
#include "rsx_vp_decompiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ps3emu/log_first.h"

/* Guest memory and the RSX offset resolvers, declared the way every backend
 * that reads guest data declares them (rsx_vertex_fetch.h). */
extern u8* vm_base;
u32 cellGcmResolveLocated(int local, u32 offset);
/* cellGcmSys.c: the display buffers registered before the engine came up. */
void cellGcm_replay_display_buffers_to_engine(void);
/* Non-zero only when the offset's page is in the IO table, so a caller can
 * tell IO-mapped main memory from "assume VRAM". */
u32 cellGcmResolveIO(u32 offset);
/* The guest VM's OOB guard: the size a host backed, or 0 for "the whole 32-bit
 * space is backed and no check is needed" (runtime/memory/vm.h). */
extern u32 ppu_vm_size;

/* ------------------------------------------------------------------------- */

#define ENG_MAX_SURFACES   64
#define ENG_MAX_ZDEPTHS    64
#define ENG_MAX_TEXTURES   RSX_DRAW_ENGINE_TEXTURE_CACHE
#define ENG_MAX_VTEXTURES  64
#define ENG_MAX_PIPELINES  8192
#define ENG_MAX_BATCHES    256
#define ENG_VERT_STRIDE    (RSX_DSP_NUM_VERTEX_ATTR * 16u)
#define ENG_INVALID        0xFFFFFFFFu
/* Guest textures larger than this are a misdecoded register, not an image;
 * rsx_live_draw.c rejects the same 4096 bound (2033). */
#define ENG_MAX_TEX_DIM    4096u
/* A surface declaration beyond this is a guest pointer read as clip
 * dimensions. rsx_live_draw.c's surface-guard (2625) preserves the existing
 * target rather than destroying a live one. */
#define ENG_MAX_SURFACE_DIM 8192u

#define ENG_VP_CB_BYTES    ((RSX_DSP_NUM_CONSTANTS + 3) * 16u)
#define ENG_FP_MAX_BYTES   0x10000u

#define M_ALPHA_TEST_ENABLE   0x0304
#define M_ALPHA_FUNC          0x0308
#define M_ALPHA_REF           0x030C
#define M_BLEND_ENABLE        0x0310
#define M_BLEND_SFACTOR       0x0314
#define M_BLEND_DFACTOR       0x0318
#define M_BLEND_EQUATION      0x0320
#define M_COLOR_MASK          0x0324
#define M_STENCIL_TEST_ENABLE 0x0328
#define M_STENCIL_MASK        0x032C
#define M_STENCIL_FUNC        0x0330
#define M_STENCIL_FUNC_REF    0x0334
#define M_STENCIL_FUNC_MASK   0x0338
#define M_STENCIL_OP_FAIL     0x033C
#define M_STENCIL_OP_ZFAIL    0x0340
#define M_STENCIL_OP_ZPASS    0x0344
#define M_TWO_SIDED_STENCIL   0x0348
#define M_BACK_STENCIL_FUNC   0x0350
#define M_BACK_STENCIL_OP_FAIL  0x035C
#define M_BACK_STENCIL_OP_ZFAIL 0x0360
#define M_BACK_STENCIL_OP_ZPASS 0x0364
#define M_DEPTH_BOUNDS_ENABLE 0x0380
#define M_DEPTH_BOUNDS_MIN    0x0384
#define M_DEPTH_BOUNDS_MAX    0x0388
#define M_SCISSOR_HORIZONTAL  0x08C0
#define M_SCISSOR_VERTICAL    0x08C4
#define M_DEPTH_FUNC          0x0A6C
#define M_DEPTH_WRITE         0x0A70
#define M_DEPTH_TEST_ENABLE   0x0A74
#define M_ZSTENCIL_CLEAR      0x1D8C

/* RSX_ENG_LEGACY_SURFACES=1: the pre-RPCS3-port surface handling -- format
 * reinterpretation and NV3089 transfers between targets go through guest
 * memory on the CPU, depth snapshots live until the next clear, texture
 * invalidation only on exact base addresses -- for A/B testing. */
/* rsx_draw_engine_set_last_provoking_vertex. */
static int s_eng_last_provoking;
static int s_eng_flat_shade = 1;   /* RSX_ENG_FLAT_SHADE, read at init */
static int s_eng_point_sprite = 1; /* RSX_ENG_POINT_SPRITE, read at init */

static int eng_legacy_surfaces(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("RSX_ENG_LEGACY_SURFACES");
        v = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return v;
}

static int eng_env_flag(const char* name, int* cache)
{
    if (*cache < 0) {
        const char* e = getenv(name);
        *cache = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return *cache;
}

/* Performance-pass kill switches: RSX_PERF_<name>=0 disables one
 * optimisation, RSX_PERF_ALL=0 all of them (cached on first read). */
static int eng_perf_on(const char* name, int* cache)
{
    if (*cache < 0) {
        static int all = -1;
        if (all < 0) { const char* a = getenv("RSX_PERF_ALL"); all = !(a && a[0] == '0'); }
        const char* e = getenv(name);
        *cache = all && !(e && e[0] == '0');
    }
    return *cache;
}

/* RSX_ROP_LEGACY=1: pre-parity raster/ROP rules (draw scissor not clipped to
 * the viewport rect, old alpha-ref format codes). */
static int eng_rop_legacy(void)
{
    static int v = -1;
    return eng_env_flag("RSX_ROP_LEGACY", &v);
}

/* RSX_ENG_SURFACE_AT_CLOSEST=1: a guest address inside several same-pitch
 * surfaces resolves to the closest base (the pre-parity rule) instead of
 * RPCS3's newest contents. */
static int eng_surface_at_closest(void)
{
    static int v = -1;
    return eng_legacy_surfaces() || eng_env_flag("RSX_ENG_SURFACE_AT_CLOSEST", &v);
}

/* RSX_ENG_AA_RESOLVE_INHERIT=1: a surface rebound under another antialias
 * mode inherits its sibling's contents scaled (resolved) onto its own sample
 * grid, and overlapping surfaces at other addresses are not inherited at all
 * (the pre-parity rules). Default: RPCS3 intersect_surface_region. */
static int eng_aa_resolve_inherit(void)
{
    static int v = -1;
    return eng_legacy_surfaces() || eng_env_flag("RSX_ENG_AA_RESOLVE_INHERIT", &v);
}

/* cellGcmSetTile regions (rsx_draw_engine_set_tile). RPCS3 records the
 * compression mode but never models it. On RSX a colour tile compressed 2x1
 * or 2x2 (CELL_GCM_COMPMODE_C32_2X1 = 7, C32_2X2 = 8) stores each pixel as
 * its AA sample group: a 1-sample surface bound over an AA one at the same
 * address and pitch reads the whole picture (each pixel the average of its
 * samples) and writes each pixel to all its samples. R2 renders its scene 2x1
 * into 0x880000 (tile 1, comp 7) and composites it there without AA.
 * RSX_ENG_TILE_AA_COMPRESS=1 turns it on. Default off, strict RPCS3
 * (byte-aligned sample copy): the model put R2's menu smoke at half scale in
 * the wrong place. */
#define ENG_MAX_TILES 16u
static struct { u32 location, offset, size, pitch, comp, bank; } s_eng_tiles[ENG_MAX_TILES];

void rsx_draw_engine_set_tile(u32 index, u32 location, u32 offset, u32 size,
                              u32 pitch, u32 comp, u32 bank, int bound)
{
    if (index >= ENG_MAX_TILES) return;
    s_eng_tiles[index].location = location;
    s_eng_tiles[index].offset = offset;
    s_eng_tiles[index].size = bound ? size : 0u;
    s_eng_tiles[index].pitch = pitch;
    s_eng_tiles[index].comp = comp;
    s_eng_tiles[index].bank = bank;
}

static int eng_tile_aa_compress(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("RSX_ENG_TILE_AA_COMPRESS");
        v = (e && e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* Whether a colour surface at (location, offset) with row pitch `pitch` lies
 * in a bound 2x1/2x2 colour-compressed tile of that pitch; *fx, *fy (when
 * given) receive the tile's sample grid (comp 7: 2x1, comp 8: 2x2). */
static int eng_tile_aa_factor(u32 location, u32 offset, u32 pitch, u32* fx, u32* fy)
{
    if (!eng_tile_aa_compress()) return 0;
    for (u32 i = 0; i < ENG_MAX_TILES; i++) {
        const u32 c = s_eng_tiles[i].comp;
        if (!s_eng_tiles[i].size || (c != 7u && c != 8u) ||
            s_eng_tiles[i].location != location || offset < s_eng_tiles[i].offset ||
            offset - s_eng_tiles[i].offset >= s_eng_tiles[i].size)
            continue;
        if (pitch && s_eng_tiles[i].pitch && s_eng_tiles[i].pitch != pitch) continue;
        if (fx) *fx = 2u;
        if (fy) *fy = c == 8u ? 2u : 1u;
        return 1;
    }
    return 0;
}

static int eng_tile_aa_at(u32 location, u32 offset, u32 pitch)
{
    return eng_tile_aa_factor(location, offset, pitch, NULL, NULL);
}

/* RSX_ENG_TYPELESS_INHERIT=0: colour<->depth and cross-format surfaces do not
 * inherit each other's bytes (RPCS3 copy_image_typeless); only same-format
 * copies run, as before. */
static int eng_typeless_inherit(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("RSX_ENG_TYPELESS_INHERIT");
        v = (e && e[0] == '0') ? 0 : 1;
    }
    return v && !eng_legacy_surfaces();
}

/* RSX_ENG_PITCH_DISCARD=0: a render target rebound at its address with the
 * same size but another pitch keeps its pixels (pre-parity). Default: RPCS3
 * bind_surface_address -- convert_pitch is unimplemented, so the surface is
 * invalidate_surface_contents'ed (erase_bkgnd: cleared) keeping its age. */
static int eng_pitch_discard(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("RSX_ENG_PITCH_DISCARD");
        v = (e && e[0] == '0') ? 0 : 1;
    }
    return v && !eng_legacy_surfaces();
}

/* RSX_ENG_SEED_GUEST=1: a new MAIN-memory RGBA8 render target starts with the
 * guest's bytes (pre-parity). Default: RPCS3 initialize_memory with Read Color
 * Buffers off -- a new target is clear_memory'ed to (0,0,0,1). */
static int eng_seed_guest(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("RSX_ENG_SEED_GUEST");
        v = (e && e[0] == '1') ? 1 : 0;
    }
    return v || eng_legacy_surfaces();
}

/* SURFACE_FORMAT colour field of the render-target bind in progress
 * (eng_current_target_set_ex), 0 outside one: names the guest layout of a
 * surface eng_surface_get creates or rebinds before its caller records it. */
static u32 s_eng_bind_cfmt;

/* ---- module state -------------------------------------------------------- */

typedef struct {
    u32 location, offset;
    u32 w, h;
    u32 pitch;              /* guest bytes per row, from SET_SURFACE_PITCH */
    rsx_be_format fmt;
    rsx_be_format hfmt;     /* host format of handle (eng_host_color_format); 0 = fmt */
    u32 handle;
    u32 feedback;          /* last immutable own-target copy, engine-owned */
    u32 region;            /* last sub-rectangle copy (color_region_snapshot) */
    u32 region_w, region_h;
    u64 region_serial;     /* color_serial the region copy was taken at */
    int region_resolved;   /* region is a tile-AA resolve (eng_tile_aa_at), not a crop */
    u32 cfmt;              /* SURFACE_FORMAT colour field it was last drawn as */
    u32 aa_sx, aa_sy;      /* sample grid per pixel this surface is held at   */
    u64 color_serial;       /* g.write_serial of the last colour write, or 0 */
    int guest_dirty;        /* rendered since guest memory last got a copy   */
    u32 reint;              /* GPU reinterpretation as another format, or 0 */
    u32 reint_fmt, reint_remap, reint_w, reint_h;
    u64 reint_serial;       /* color_serial it was built at */
    u32 aa_flags;           /* target_set_aa result: RSX_BE_AA_* */
    int swz;                /* NV309E blit destination: linear here, Morton-ordered in guest memory */
    u64 tag[3];             /* RPCS3 memory_tag_samples at the last write (eng_tag_sync) */
    u32 tag_ok;             /* bit n: tag[n] holds a sample */
    int storage;            /* split_surface_region clone never bound since (surface_usage_flags::storage) */
} eng_surface;

static rsx_be_format eng_hfmt(const eng_surface* s) { return s->hfmt ? s->hfmt : s->fmt; }

typedef struct {
    u32 location, offset;
    u32 w, h;
    u32 handle;
    u32 snapshot;          /* sampleable copy, or 0                        */
    int cleared;
    int had_write;
    int snapshot_valid;
    u64 depth_serial;      /* g.write_serial of the last depth write or clear */
    int guest_dirty;       /* written since guest memory last got a copy     */
    u32 zfmt, pitch;       /* SURFACE_FORMAT depth field and zeta pitch then */
    u32 used_w, used_h;    /* the clip that write went through              */
    u64 snapshot_serial;   /* depth_serial the snapshot was taken at        */
    u32 reint;             /* GPU reinterpretation as a colour format, or 0 */
    u32 reint_fmt, reint_remap, reint_w, reint_h;
    u64 reint_serial;      /* depth_serial it was built at */
    u32 depth_float;       /* CONTROL0 depth_float the target was created for */
    u32 host_zfmt;         /* zfmt the host target was created for, 0 = generic */
    u32 aa_sx, aa_sy, aa_flags; /* last target_set_aa */
    u64 tag[3];            /* RPCS3 memory_tag_samples at the last write */
    u32 tag_ok;
} eng_zdepth;

typedef struct {
    u32 location, offset, format, width, height, pitch, remap, cubemap;
    u32 handle;
    u64 content_hash;
    u64 sparse_hash;            /* eng_texture_sparse_hash, the per-frame check */
    u32 last_full_frame;        /* g.frames of the last full content hash */
    u32 last_hash_frame;
    u64 last_use_serial;
    u32 depth;                  /* slices of a 3D texture, else 1 */
    u32 tc_seen;                /* s_tc_epoch when its pages were last checked */
} eng_texture;

typedef struct {
    u64 key;
    u32 handle;
    u8 fixed;
    u8 depth_cmp_unit;      /* rsx_fp_frag_state depth_compare unit + 1, 0 = off */
    u8 pb_unit;             /* rsx_fp_frag_state pb unit + 1, 0 = off */
    u16 tex_used;           /* units the pixel program samples */
    u64 fp_hash;            /* eng_fp_debug_hash of the guest fragment program */
    u32 topo_mask;          /* rsx_topology bits it was drawn with (program cache) */
} eng_pipeline;

typedef struct { u32 first, count; } eng_batch;

typedef struct {
    u32 location, offset, pitch, width, height;
    int valid;
} eng_display_buffer;

static struct {
    const rsx_draw_backend* be;
    int ready;
    int default_on;
    u32 width, height;

    rsx_dispatch rsx;

    eng_surface surfaces[ENG_MAX_SURFACES];
    u32 n_surfaces;
    eng_zdepth zdepths[ENG_MAX_ZDEPTHS];
    u32 n_zdepths;
    eng_texture textures[ENG_MAX_TEXTURES];
    u32 n_textures;
    eng_pipeline pipelines[ENG_MAX_PIPELINES];
    u32 n_pipelines;
    eng_display_buffer display_buffers[8];

    u32 frames;
    u64 texture_use_serial;
    u32 guest_draws;
    u32 last_guest_draws;
    u32 last_present_surface;
    u32 last_flip_buffer;
    u64 write_serial;       /* orders colour and depth writes to one address */

    /* staging: decoded texture levels, the constant blocks, the index list */
    u8* tex_staging;
    u32 tex_staging_cap;
    u8* vp_cb;
    u8* fp_cb;
    u32 fp_cb_cap;
    u32* indices;
    u32 index_cap;

    rsx_fp_constant_block fp_constants;
} g;

/* The decompilers' HLSL. The VP decompiler builds bodies up to 192 KB. */
static char s_vs_hlsl[256 * 1024];
static char s_ps_hlsl[256 * 1024];

/* The draw accumulator: one BEGIN_END group's batches, references and cuts. */
static struct {
    eng_batch arr[ENG_MAX_BATCHES];
    u32 n_arr;
    eng_batch idx[ENG_MAX_BATCHES];
    u32 n_idx;
    u32 n_packets;

    rsx_vertex_ref* refs;
    u32 n_refs, cap_refs, n_source_refs;
    int refs_remapped;
    rsx_vertex_remap ref_remap;

    u32* cuts;
    u32 n_cuts, cap_cuts;

    u8* verts;
    u64 verts_cap;
    u32 n_verts;

    /* Canonical ATTR0..15 float4 snapshots for register-based vertices.
     * Bounded separately from compact GPU staging; never fetch stale arrays. */
    float (*immediate)[RSX_DSP_NUM_VERTEX_ATTR][4];
    u32 n_immediate, cap_immediate;

    rsx_vertex_layout_plan layout;
    rsx_vertex_fetch_plan fetch_plan;
    int fetch_ok;

    /* INLINE_ARRAY stream for this group, when the vertices came down the
     * FIFO rather than from a vertex array. */
    const u8* inl;
    u32 inl_bytes;
    u32 inline_dropped_at_begin;

    /* ARRAY_ELEMENT16/32 index list for this group (dispatcher-owned). */
    const u32* elem;
    u32 n_elem;
    u32 elem_dropped_at_begin;
} dc;

/* ---- guest memory -------------------------------------------------------- */

/* rsx_dsp_* locations are RSX_LOCATION_LOCAL = 0 / _MAIN = 1;
 * cellGcmResolveLocated's argument is the opposite sense (1 = local), and
 * getting that backwards resolves every VRAM object through the IO table. */
static rsx_vertex_guest_ptr_fn s_guest_reader;
static void* s_guest_user;

void rsx_draw_engine_set_guest_memory(rsx_vertex_guest_ptr_fn reader, void* user)
{
    s_guest_reader = reader;
    s_guest_user = user;
}

static const u8* eng_guest_ptr(void* user, u32 location, u32 offset,
                               u32 min_bytes)
{
    (void)user;
    if (!min_bytes || min_bytes > (256u << 20)) return NULL;
    if (s_guest_reader) return s_guest_reader(s_guest_user, location, offset, min_bytes);
    if (!vm_base) return NULL;
    const u32 ea = cellGcmResolveLocated(location == RSX_LOCATION_LOCAL, offset);
    if (!ea) return NULL;
    if (ppu_vm_size && (u64)ea + min_bytes > ppu_vm_size) return NULL;
    return vm_base + ea;
}

/* The host's always-writable second view of the guest space (project
 * main.cpp alloc_guest_vm); absent in unit tests. RPCS3 flushes a section
 * through vm::get_super_ptr while its pages stay protected, so a guest access
 * racing the flush still faults and waits for it, and peeks a protected page
 * the same way. The engine stores, and reads memory tags, through it and
 * lifts a guard only afterwards. RSX_ENG_SUDO=0: lift the guard first and
 * store through vm_base (and hold pages open for a peek), as before. */
extern u8* vm_sudo_base __attribute__((weak));
static int s_eng_sudo = -1;
static u8* eng_sudo_base(void)
{
    if (s_eng_sudo < 0) { const char* e = getenv("RSX_ENG_SUDO"); s_eng_sudo = !(e && *e == '0'); }
    return s_eng_sudo && &vm_sudo_base && vm_base ? vm_sudo_base : NULL;
}
static int eng_sudo_on(void) { return eng_sudo_base() != NULL; }

/* A byte in vm_base's view -> the same byte in the sudo view (p itself when
 * there is none or p is not guest memory). */
static u8* eng_store_ptr(u8* p)
{
    u8* s = eng_sudo_base();
    if (!s || !p || p < vm_base || (u64)(p - vm_base) > 0xFFFFFFFFull) return p;
    return s + (p - vm_base);
}
static void eng_store_done(const u8* p, u32 n);

/* ---- RSX memory tiling ---------------------------------------------------
 * RPCS3 get_tiled_memory_region + tiled_dma_copy (Common/tiled_dma_copy.hpp,
 * RSXMemoryTiling.glsl): RSX reads and writes a bound tile in MAIN memory
 * through the memory controller's tiling, so the CPU and SPUs see its bytes
 * permuted in 32-byte runs; local memory is tiled transparently. RPCS3 encodes
 * a surface flushed into such a tile and decodes a texture / render target
 * initialised out of one. R3's SPU depth of field reads and writes its HDR
 * buffer in tile 4 that way. Off unless RSX_ENG_TILED_MEMORY=1, as RPCS3's
 * "Handle RSX Memory Tiling" (default false). RPCS3 steps rows at the tile
 * pitch whatever the image's own pitch (tiled_dma_copy row_offset); the row
 * loops below take that from eng_tiled_row_pitch. */
typedef struct { u32 base, size, tiles_per_row, factor, bank, pitch; } eng_tiled;

static int s_eng_tiled_mem;   /* read at rsx_draw_engine_init */
/* RSX_ENG_TILED_ROW_PITCH=0: rows at the image pitch inside a tile too. */
static int s_eng_tiled_row_pitch = 1;
static int eng_tiled_memory(void) { return s_eng_tiled_mem; }

/* The tile whose range holds `offset` (RPCS3: the first tile that overlaps
 * the access and contains its start). */
static int eng_tiled_at(u32 location, u32 offset, eng_tiled* t)
{
    if (location != RSX_LOCATION_MAIN || !eng_tiled_memory()) return 0;
    for (u32 i = 0; i < ENG_MAX_TILES; i++) {
        const u32 pitch = s_eng_tiles[i].pitch;
        if (!s_eng_tiles[i].size || s_eng_tiles[i].location != RSX_LOCATION_MAIN || !pitch ||
            offset < s_eng_tiles[i].offset || offset - s_eng_tiles[i].offset >= s_eng_tiles[i].size)
            continue;
        u32 prime = 0, factor = 0;
        const u32 base = pitch >> 8;
        if (!(pitch & (pitch - 1u))) { prime = 1u; factor = base; }
        else {
            static const u32 primes[] = { 3u, 5u, 7u, 11u, 13u };
            for (u32 k = 0; k < 5u; k++)
                if (base % primes[k] == 0u) { prime = primes[k]; factor = base / primes[k]; break; }
        }
        if (!prime || !factor) return 0;
        t->base = s_eng_tiles[i].offset;
        t->size = s_eng_tiles[i].size;
        t->tiles_per_row = prime * factor;
        t->factor = factor;
        t->bank = s_eng_tiles[i].bank;
        t->pitch = pitch;
        return 1;
    }
    return 0;
}

/* The distance between rows of an image `pitch` apart at (location, offset):
 * the tile's pitch inside a tile (RPCS3 tiled_dma_copy: row * tile_pitch +
 * tile_address_offset, the image pitch only indexing the linear side). */
static u32 eng_tiled_row_pitch(u32 location, u32 offset, u32 pitch)
{
    eng_tiled t;
    if (!s_eng_tiled_row_pitch || !eng_tiled_at(location, offset, &t)) return pitch;
    return t.pitch;
}

/* Where linear byte `rel` of the tile (from its base) really lives, from the
 * base: RPCS3 tiled_dma_copy's tile_address - tile_base_address. Bits 0-4
 * pass through. The base is the IO offset: tiles are 64 KB aligned and IO
 * pages 1 MB, so it agrees with the effective address in every bit used. */
static u32 eng_tile_xlate(const eng_tiled* t, u32 rel)
{
    static const u32 bank_lut[16] = { 0, 1, 2, 3, 2, 3, 0, 1, 1, 2, 3, 0, 3, 0, 1, 2 };
    const u32 this_address = t->base + rel;
    const u32 texel_offset = rel / 256u;
    const u32 tile_x = texel_offset % t->tiles_per_row;
    const u32 tile_y = (texel_offset / t->tiles_per_row) / 64u;
    const u32 tile_id = tile_y * t->tiles_per_row + tile_x;
    const u32 tile_selector = (tile_id + (t->base >> 14)) & 0x3ffffu;
    const u32 row_address = (tile_selector >> 2) & 0xffffu;
    u32 bank = 0;
    if (t->factor == 1u) bank = tile_selector & 3u;
    else if (t->factor == 2u)
        bank = bank_lut[((tile_selector + ((tile_y & 1u) << 1)) & 3u) * 4u + (tile_y & 3u)];
    else if (t->factor >= 4u) bank = bank_lut[(tile_selector & 3u) * 4u + (tile_y & 3u)];
    bank = (bank + t->bank) & 3u;
    const u32 line = (texel_offset / t->tiles_per_row) % 64u;
    u32 column = 0;
    column |= ((line >> 3) & 7u) << 7;
    column |= ((this_address >> 5) & 7u) << 4;
    column |= (line & 3u) << 2;
    const u32 partition = (((line >> 2) & 1u) + ((this_address >> 6) & 1u)) & 1u;
    u32 a = 0;
    a |= (row_address & 0xFFFFu) << 16;
    a |= (bank & 3u) << 14;
    a |= ((column >> 4) & 0x3Fu) << 8;
    a |= (partition & 1u) << 7;
    a |= ((column >> 2) & 3u) << 5;
    a |= this_address & 0x1Fu;
    a ^= (((a >> 12) ^ ((bank ^ tile_selector) & 1u) ^ (a >> 14)) & 1u) << 9;
    a ^= ((a >> 11) & 1u) << 10;
    return a - t->base;
}

/* RPCS3 flush_set (texture_cache.h:806-889) + imp_flush_memcpy
 * (texture_cache_utils.h:1535): a surface written back to guest memory skips
 * the address range of every newer surface whose data is already there, so
 * old content never tramples new. eng_flush_scope_begin collects those ranges
 * for one surface's store; eng_guest_tiled_copy honours them while set.
 * RSX_ENG_FLUSH_EXCLUSIONS=0 turns it off. */
#define ENG_EXCL_MAX 32
static struct { u32 lo, hi; } s_excl[ENG_EXCL_MAX];
static u32 s_excl_n;
static int s_excl_depth;

static int eng_flush_exclusions_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_ENG_FLUSH_EXCLUSIONS"); v = !(e && *e == '0'); }
    return v;
}

static void eng_excl_add(u32 lo, u64 hi)
{
    if (hi > 0xFFFFFFFFull) hi = 0xFFFFFFFFull;
    if (s_excl_n < ENG_EXCL_MAX && (u32)hi > lo) {
        s_excl[s_excl_n].lo = lo; s_excl[s_excl_n].hi = (u32)hi; s_excl_n++;
    }
}

/* `serial`: the age of the surface about to be stored; is_z/slot: itself. */
static void eng_flush_scope_begin(u32 location, u32 offset, u64 span, u64 serial, int is_z, u32 slot)
{
    if (s_excl_depth++ || !eng_flush_exclusions_on()) return;
    s_excl_n = 0;
    const u64 end = (u64)offset + span;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* o = &g.surfaces[i];
        if ((!is_z && i == slot) || !o->handle || o->guest_dirty || o->swz ||
            o->location != location || o->color_serial <= serial)
            continue;
        const u64 olo = o->offset, ohi = olo + (u64)o->pitch * o->h;
        if (olo < end && offset < ohi) eng_excl_add(o->offset, ohi);
    }
    for (u32 i = 0; i < g.n_zdepths; i++) {
        const eng_zdepth* o = &g.zdepths[i];
        if ((is_z && i == slot) || !o->handle || o->guest_dirty ||
            o->location != location || o->depth_serial <= serial)
            continue;
        const u32 h = o->used_h && o->used_h < o->h ? o->used_h : o->h;
        const u64 olo = o->offset, ohi = olo + (u64)o->pitch * h;
        if (olo < end && offset < ohi) eng_excl_add(o->offset, ohi);
    }
}

static void eng_flush_scope_end(void)
{
    if (s_excl_depth > 0 && --s_excl_depth == 0) s_excl_n = 0;
}

/* memcpy(dst, src, n) for guest address `addr`, minus the excluded ranges. */
static void eng_excl_copy(u8* dst, const u8* src, u32 addr, u32 n)
{
    const u64 end = (u64)addr + n;
    u64 a = addr;
    while (a < end) {
        u64 skip = 0;
        for (u32 k = 0; k < s_excl_n; k++)
            if (s_excl[k].lo <= a && a < s_excl[k].hi && s_excl[k].hi > skip) skip = s_excl[k].hi;
        if (skip) { a = skip < end ? skip : end; continue; }
        u64 next = end;
        for (u32 k = 0; k < s_excl_n; k++)
            if (s_excl[k].lo > a && s_excl[k].lo < next) next = s_excl[k].lo;
        memcpy(dst + (a - addr), src + (a - addr), (size_t)(next - a));
        a = next;
    }
}

#define ENG_COPY_OUT(dst, src, addr, n) \
    do { if (s_excl_n) eng_excl_copy((dst), (src), (addr), (n)); else memcpy((dst), (src), (n)); } while (0)

/* RPCS3 flush_set sorts by last_write_tag, oldest first, so new data
 * tramples old: the flush loops queue here, then run in age order. */
typedef struct { u32 n; struct { u64 serial; u32 slot; u32 z; } it[ENG_EXCL_MAX * 2]; } eng_flq;
static void eng_flq_add(eng_flq* q, u32 slot, int z)
{
    if (q->n >= ENG_EXCL_MAX * 2) return;
    const u64 s = z ? g.zdepths[slot].depth_serial : g.surfaces[slot].color_serial;
    u32 k = q->n++;
    while (k && q->it[k - 1].serial > s) { q->it[k] = q->it[k - 1]; k--; }
    q->it[k].serial = s; q->it[k].slot = slot; q->it[k].z = (u32)z;
}
static int eng_surface_to_guest(u32 slot);
static int eng_zdepth_to_guest_at(u32 slot, int site);
static void eng_flq_run(eng_flq* q, int site)
{
    for (u32 k = 0; k < q->n; k++) {
        if (q->it[k].z) eng_zdepth_to_guest_at(q->it[k].slot, site);
        else eng_surface_to_guest(q->it[k].slot);
    }
    q->n = 0;
}

/* Copy `n` bytes of the guest image at (location, offset) between guest
 * memory and `buf`, through the tile there if any (to_guest: encode, else
 * decode). Bytes whose tiled home falls outside the tile are skipped, as
 * RPCS3's out-of-bounds test does; bytes past the tile's end are linear. */
static int eng_guest_tiled_copy(u32 location, u32 offset, u8* buf, u32 n, int to_guest)
{
    if (!n) return 1;
    eng_tiled t;
    if (!eng_tiled_at(location, offset, &t)) {
        u8* p = (u8*)eng_guest_ptr(NULL, location, offset, n);
        if (!p) return 0;
        if (to_guest) { ENG_COPY_OUT(eng_store_ptr(p), buf, offset, n); eng_store_done(p, n); }
        else memcpy(buf, p, n);
        return 1;
    }
    u8* tile = (u8*)eng_guest_ptr(NULL, location, t.base, t.size);
    if (!tile) return 0;
    u8* const wtile = to_guest ? eng_store_ptr(tile) : tile;
    const u32 rel0 = offset - t.base;
    const u32 in_tile = t.size - rel0 < n ? t.size - rel0 : n;
    u32 lo = 0xFFFFFFFFu, hi = 0;
    for (u32 i = 0; i < in_tile;) {
        const u32 rel = rel0 + i;
        const u32 run = 32u - (rel & 31u) < in_tile - i ? 32u - (rel & 31u) : in_tile - i;
        const u32 at = eng_tile_xlate(&t, rel);
        if (at < t.size && at + run <= t.size) {
            if (to_guest) {
                ENG_COPY_OUT(wtile + at, buf + i, offset + i, run);
                if (at < lo) lo = at;
                if (at + run > hi) hi = at + run;
            }
            else memcpy(buf + i, tile + at, run);
        }
        i += run;
    }
    if (to_guest && hi > lo) eng_store_done(tile + lo, hi - lo);
    if (in_tile < n) {
        u8* p = (u8*)eng_guest_ptr(NULL, location, t.base + t.size, n - in_tile);
        if (!p) return 0;
        if (to_guest) {
            ENG_COPY_OUT(eng_store_ptr(p), buf + in_tile, offset + in_tile, n - in_tile);
            eng_store_done(p, n - in_tile);
        }
        else memcpy(buf + in_tile, p, n - in_tile);
    }
    return 1;
}

/* eng_guest_ptr for reading an image: guest memory itself, or (in a tile) a
 * decoded copy in a scratch buffer valid until the next call. */
static u8* s_detile_buf;
static u32 s_detile_cap;
static const u8* eng_guest_image(u32 location, u32 offset, u32 n)
{
    eng_tiled t;
    if (!eng_tiled_at(location, offset, &t)) return eng_guest_ptr(NULL, location, offset, n);
    if (!n || n > (256u << 20)) return NULL;
    if (s_detile_cap < n) {
        u8* p = (u8*)realloc(s_detile_buf, n);
        if (!p) return NULL;
        s_detile_buf = p;
        s_detile_cap = n;
    }
    return eng_guest_tiled_copy(location, offset, s_detile_buf, n, 0) ? s_detile_buf : NULL;
}

static u64 eng_fnv1a(const void* data, u32 n, u64 hash)
{
    const u8* p = (const u8*)data;
    for (u32 i = 0; i < n; i++) { hash ^= p[i]; hash *= 1099511628211ull; }
    return hash;
}

/* RSX_ENGINE_PROFILE=1: per-5 s wall-time breakdown of the engine's hot paths
 * (inclusive times; nested calls count in both). */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static u64 eng_prof_now(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (u64)c.QuadPart; }
static double eng_prof_ms(u64 t) { static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f); return (double)t * 1000.0 / (double)f.QuadPart; }
#else
#include <time.h>
#include <sys/stat.h>
static u64 eng_prof_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec; }
static double eng_prof_ms(u64 t) { return (double)t / 1e6; }
#endif
static int s_prof_on = -1;
/* Depth stores per caller line (each one a full GPU wait). */
static struct { int site; u32 n; } s_zsite[8];
static u64 s_prof_t[8], s_prof_n[8], s_prof_last;
/* Perf-pass cache hit/miss counters, printed with [eng prof]. */
enum { PC_TAG, PC_PMEMO, PC_VPCB, PC_TEXSLOT, PC_N };
static u64 s_pc_hit[PC_N], s_pc_miss[PC_N];
static const char* const s_pc_names[PC_N] = { "tagread", "pipememo", "vpconst", "texslot" };
#define ENG_PC(i, hit) do { if (hit) s_pc_hit[i]++; else s_pc_miss[i]++; } while (0)
/* eng_texture_slot's key -> index + 1 hints (RSX_PERF_TEXHASH). */
static u32 s_tex_hint[4096];

/* Invalidation generations for the per-draw memos (perf pass):
 *  s_eng_pgen  - any NV4097 register a pipeline could depend on changed
 *                (sink_reg_changed minus eng_reg_pipeline_irrelevant), a
 *                register written behind the dispatcher's back (vertex-data
 *                folds, driver/flip methods) or the engine (re)started;
 *  s_eng_vpgen - the transform program store (upload window) changed;
 *  s_eng_cgen  - the transform constants changed (upload window, clause
 *                replays, driver methods). */
static u32 s_eng_pgen = 1u, s_eng_vpgen = 1u, s_eng_cgen = 1u;
/* Transform-constant slots written since the last vertex constant build
 * (inclusive range; lo > hi = none). */
static u32 s_eng_cdirty_lo = 0u, s_eng_cdirty_hi = 0x7FFFFFFFu;
static void eng_const_dirty(u32 lo, u32 hi)
{
    if (lo < s_eng_cdirty_lo) s_eng_cdirty_lo = lo;
    if (hi > s_eng_cdirty_hi) s_eng_cdirty_hi = hi;
    s_eng_cgen++;
}
static void eng_gen_bump_all(void)
{
    s_eng_pgen++; s_eng_vpgen++;
    s_eng_cdirty_lo = 0u; s_eng_cdirty_hi = 0x7FFFFFFFu; s_eng_cgen++;
}

/* Generations for a method about to reach rsx_dispatch_method. Writes that
 * leave no reg_changed trace: the program and constant upload windows store
 * into vp[] / constants[], and driver methods (flip reset) rewrite the
 * register file. (The vertex-data folds also write behind the dispatcher,
 * but only into VTX_ATTR_4F, which no pipeline input reads.) */
static void eng_method_gens(u32 m)
{
    if (m >= 0x2000u) { eng_gen_bump_all(); return; }
    if (m >= 0x0B80u && m < 0x0C00u) { s_eng_vpgen++; return; }
    if (m >= 0x1F00u && m < 0x1F80u) {
        /* A write deferred into a draw clause lands later (clause replay):
         * sink_end treats any clause with barriers as all-dirty. */
        const u32 base = g.rsx.regs[0x1EFCu >> 2];
        const u32 slot = base + ((m - 0x1F00u) >> 4);
        eng_const_dirty(slot, slot);
        return;
    }
}

/* Registers no pipeline-key input reads: per-draw addresses and parameters
 * whose only consumers are the fetch, binding and viewport paths. A texture
 * OFFSET is read by the hardware sRGB/SNORM view choice only through the
 * surface lookup, which the memo re-validates itself. */
static int eng_reg_pipeline_irrelevant(u32 m)
{
    if (m >= 0x1A00u && m < 0x1C00u) {
        const u32 r = (m - 0x1A00u) & 0x1Fu;
        return r == 0x00u || r == 0x1Cu;          /* TEXTURE_OFFSET, BORDER_COLOR */
    }
    if (m >= 0x0900u && m < 0x0980u) return ((m - 0x0900u) & 0x1Fu) == 0x00u; /* VTX TEXTURE_OFFSET */
    if (m >= 0x1680u && m < 0x16C0u) return 1;    /* VERTEX_DATA_ARRAY_OFFSET */
    if (m >= 0x1F00u && m < 0x1F80u) return 1;    /* TRANSFORM_CONSTANT window */
    if (m >= 0x0B80u && m < 0x0C00u) return 1;    /* TRANSFORM_PROGRAM window (vpgen) */
    /* VERTEX_DATA* immediates and the VTX_ATTR_4F file they fold into: read
     * by the vertex fetch only. */
    if ((m >= 0x1E40u && m < 0x1E80u) || (m >= 0x1500u && m < 0x1600u) ||
        (m >= 0x1880u && m < 0x1A00u) || (m >= 0x0A80u && m < 0x0B00u) ||
        (m >= 0x1C00u && m < 0x1D00u))
        return 1;
    if (m >= 0x0A20u && m < 0x0A40u) return 1;    /* VIEWPORT translate/scale */
    switch (m) {
    case 0x0A00u: case 0x0A04u:                   /* VIEWPORT horiz/vert */
    case 0x1808u: case 0x180Cu: case 0x1810u: case 0x1814u: case 0x1818u:
    case 0x181Cu: case 0x1820u: case 0x1824u:     /* BEGIN_END, draw/index/inline arrays */
    case 0x1EFCu: case 0x1E9Cu:                   /* constant / program load pointers */
    case 0x08C0u: case 0x08C4u:                   /* SCISSOR */
    case 0x0384u: case 0x0388u:                   /* DEPTH_BOUNDS min/max */
    case 0x1D8Cu: case 0x1D90u: case 0x1D94u:     /* clear values, CLEAR */
    case 0x030Cu:                                 /* ALPHA_REF (fragment constant) */
    case 0x0210u: case 0x0214u: case 0x0218u: case 0x020Cu: case 0x022Cu:
    case 0x0280u: case 0x0284u: case 0x0288u: case 0x028Cu:
    case 0x021Cu:                                 /* surface offsets / pitches */
        return 1;
    default:
        return 0;
    }
}
static const char* const s_prof_names[8] = { "tex_hash", "tex_upload", "pipeline", "draw(end)", "guest_read", "z_to_guest", "present", "method(all)" };
/* The start time, read only while profiling (or before it is known). */
static u64 eng_prof_t0(void) { return s_prof_on ? eng_prof_now() : 0u; }
static void eng_prof_add(int idx, u64 t0)
{
    if (s_prof_on < 0) { const char* e = getenv("RSX_ENGINE_PROFILE"); s_prof_on = e && *e == '1'; }
    if (!s_prof_on) return;
    const u64 t1 = eng_prof_now();
    s_prof_t[idx] += t1 - t0; s_prof_n[idx]++;
    if (idx != 6) return;                               /* report on present */
    if (!s_prof_last) { s_prof_last = t1; return; }
    if (eng_prof_ms(t1 - s_prof_last) < 5000.0) return;
    const double wall = eng_prof_ms(t1 - s_prof_last);
    fprintf(stderr, "[eng prof] %.0f ms wall, %llu frames:", wall, (unsigned long long)s_prof_n[6]);
    for (int i = 0; i < 8; i++)
        fprintf(stderr, " %s %.0fms/%llu", s_prof_names[i], eng_prof_ms(s_prof_t[i]), (unsigned long long)s_prof_n[i]);
    for (int k = 0; k < 8 && s_zsite[k].site; k++)
        fprintf(stderr, " z@%d:%u", s_zsite[k].site, s_zsite[k].n);
    for (int k = 0; k < PC_N; k++)
        if (s_pc_hit[k] + s_pc_miss[k])
            fprintf(stderr, " %s %llu/%llu", s_pc_names[k], (unsigned long long)s_pc_hit[k],
                    (unsigned long long)(s_pc_hit[k] + s_pc_miss[k]));
    fprintf(stderr, "\n");
    memset(s_pc_hit, 0, sizeof s_pc_hit); memset(s_pc_miss, 0, sizeof s_pc_miss);
    memset(s_zsite, 0, sizeof s_zsite);
    memset(s_prof_t, 0, sizeof s_prof_t); memset(s_prof_n, 0, sizeof s_prof_n);
    s_prof_last = t1;
}

/* R2 patch to RPCS3 (VKGSRender::on_framebuffer_bound): a colour surface at a
 * display buffer's base is never locked for Write Color Buffers, so nothing
 * stores it to guest memory on a CPU/transfer/texture read. R2 samples VRAM
 * offset 0 (the front buffer) as a dummy DXT1 texture; the stored frame would
 * decode to white sparkles. RSX_R2PATCH_LEGACY=1 stores display buffers too. */
static int eng_r2patch_legacy(void)
{
    static int v = -1;
    return eng_env_flag("RSX_R2PATCH_LEGACY", &v);
}

static int eng_surface_is_display_buffer(u32 location, u32 offset)
{
    if (location != RSX_LOCATION_LOCAL || eng_r2patch_legacy()) return 0;
    for (u32 k = 0; k < 8; k++)
        if (g.display_buffers[k].valid && g.display_buffers[k].location == RSX_LOCATION_LOCAL &&
            g.display_buffers[k].offset == offset)
            return 1;
    return 0;
}

/* Engine calls in progress on this thread (ENG_SCOPE at the public entry
 * points that run engine work). A WCB fault on the engine's thread at depth 0
 * comes from guest code it runs, not from the engine mid-command. */
#ifdef _WIN32
static __declspec(thread) int s_eng_depth;
#else
static __thread int s_eng_depth;
#endif
static void eng_scope_leave(int* p) { (void)p; s_eng_depth--; }
#define ENG_SCOPE() int eng_scope_ __attribute__((cleanup(eng_scope_leave), unused)) = (s_eng_depth++, 0)

/* ---- guest page guards (shared) and Write Color Buffers fault flush ------ */
/*
 * One owner mask per 4 KB guest page, shared by everything that makes guest
 * pages inaccessible to catch the guest's first touch (cellGcmSys's zcull
 * report pages, the WCB surfaces below), so one owner lifting its guard never
 * drops another's. A page is NOACCESS while any owner holds it and nobody has
 * it held open (rsx_page_guard_hold: the runtime's own access to it).
 *
 * WCB, RPCS3's texture_cache with "Write Color Buffers": a colour surface the
 * GPU rendered and guest memory does not have yet is write-protected; the
 * guest's first touch (read or write) has it flushed GPU->guest and the guard
 * lifted. The flush runs on the engine's thread (the backend is not
 * thread-safe): a faulting guest thread posts the page, kicks the walker and
 * waits, bounded -- the RSX may itself be waiting on that thread's label, so
 * a timeout lifts the guard without the flush. A fault on the engine's own
 * thread stores in place when it comes from outside the engine (s_eng_depth
 * 0) and otherwise only lifts the guard. RSX_ENG_WCB_FAULT=0 disables it. */
#ifdef _WIN32
#define PG_SHIFT 12u
#define PG_COUNT (1u << 20)
static volatile u8 s_pg_mask[PG_COUNT];
static u8 s_pg_hold[PG_COUNT];
static SRWLOCK s_pg_lock = SRWLOCK_INIT;
/* Bumped (under s_pg_lock) whenever any guest page's protection is lowered
 * below NOACCESS; starts at 1 so a zeroed cache entry never matches. */
static volatile u32 s_pg_open_gen = 1u;

/* PG_OWNER_TEX: RPCS3's texture cache section protection (protection::ro,
 * texture_cache.h lock_memory_region / cached_texture_section::protect): a
 * page holding a cached texture is read-only; the guest's first write faults,
 * invalidates the textures on it and opens it. Every other owner needs the
 * first touch of any kind (NOACCESS). */
#define PG_OWNER_TEX 4u

/* The protection an owner mask asks for: 0 read-write, 1 read-only, 2 none. */
static int pg_level(u8 m)
{
    return (m & (u8)~PG_OWNER_TEX) ? 2 : (m ? 1 : 0);
}

static int pg_protect_run(u32 pn, u32 n, int level)
{
    if (!vm_base || !n) return 1;
    DWORD old;
    /* Any protection below NOACCESS lets something touch the page without a
     * fault: the tag-read cache (eng_tag_read) is stale from here on. */
    if (level < 2) { s_pg_open_gen++; MemoryBarrier(); }
    return VirtualProtect(vm_base + ((size_t)pn << PG_SHIFT), (size_t)n << PG_SHIFT,
                          level >= 2 ? PAGE_NOACCESS : level ? PAGE_READONLY : PAGE_READWRITE,
                          &old) != 0;
}

static void eng_pg_reconcile_install(void);

/* Add or drop `owner` on the pages of [ea, ea + len). Runs of pages whose
 * effective protection changes to the same level are applied with one call.
 * Returns 0 when a guard could not be applied (those pages do not take the
 * owner). */
static int pg_guard_range(u32 ea, u32 len, u8 owner, int on)
{
    if (!vm_base || !len) return 0;
    if (on) eng_pg_reconcile_install();
    const u32 p0 = ea >> PG_SHIFT, p1 = (u32)(((u64)ea + len - 1u) >> PG_SHIFT);
    int ok = 1;
    AcquireSRWLockExclusive(&s_pg_lock);
    u32 run = 0, run_n = 0;
    int run_level = 0;
    for (u32 pn = p0;; pn++) {
        int change = 0, level = 0;
        if (pn <= p1) {
            const u8 old = s_pg_mask[pn], nw = on ? (u8)(old | owner) : (u8)(old & ~owner);
            level = pg_level(nw);
            change = !s_pg_hold[pn] && pg_level(old) != level;
            if (!change) s_pg_mask[pn] = nw;
        }
        if (run_n && (!change || level != run_level)) {
            /* Guarding: publish the owner before the page's protection rises,
             * so a fault in between is recognised; take it back if that
             * failed. Unguarding: lower the protection first, then drop the
             * owner. */
            if (on) {
                for (u32 k = run; k < run + run_n; k++) s_pg_mask[k] = (u8)(s_pg_mask[k] | owner);
                MemoryBarrier();
                if (!pg_protect_run(run, run_n, run_level)) {
                    for (u32 k = run; k < run + run_n; k++) s_pg_mask[k] = (u8)(s_pg_mask[k] & ~owner);
                    ok = 0;
                }
            } else {
                pg_protect_run(run, run_n, run_level);
                MemoryBarrier();
                for (u32 k = run; k < run + run_n; k++) s_pg_mask[k] = (u8)(s_pg_mask[k] & ~owner);
            }
            run_n = 0;
        }
        if (change) {
            if (!run_n) { run = pn; run_level = level; }
            run_n++;
        }
        if (pn >= p1 && !run_n) break;
    }
    ReleaseSRWLockExclusive(&s_pg_lock);
    return ok;
}

int rsx_page_guard_set(u32 page, u32 owner, int on)
{
    return pg_guard_range(page & ~((1u << PG_SHIFT) - 1u), 1u << PG_SHIFT, (u8)owner, on);
}

void rsx_page_guard_hold(u32 page, int delta)
{
    if (!vm_base) return;
    const u32 pn = page >> PG_SHIFT;
    AcquireSRWLockExclusive(&s_pg_lock);
    const u8 h = s_pg_hold[pn];
    if (delta > 0 && h < 255u) {
        if (!h && s_pg_mask[pn]) pg_protect_run(pn, 1, 0);
        s_pg_hold[pn] = (u8)(h + 1u);
    } else if (delta < 0 && h) {
        s_pg_hold[pn] = (u8)(h - 1u);
        if (h == 1u && s_pg_mask[pn]) pg_protect_run(pn, 1, pg_level(s_pg_mask[pn]));
    }
    const int tex = delta < 0 && h == 1u && (s_pg_mask[pn] & PG_OWNER_TEX);
    ReleaseSRWLockExclusive(&s_pg_lock);
    /* The page was writable while held: whatever the holder stored there is a
     * write the texture pages never faulted on. */
    if (tex) rsx_draw_engine_note_guest_write(pn << PG_SHIFT, 1u << PG_SHIFT);
}

/* An engine store through the sudo view never faults, so a texture page
 * (PG_OWNER_TEX) it lands on is noted as written here instead. */
static void eng_store_done(const u8* p, u32 n)
{
    if (!n || !vm_base || !eng_sudo_on() || p < vm_base || (u64)(p - vm_base) + n > 0x100000000ull)
        return;
    const u32 ea = (u32)(p - vm_base);
    const u32 p0 = ea >> PG_SHIFT, p1 = (u32)(((u64)ea + n - 1u) >> PG_SHIFT);
    for (u32 pn = p0; pn <= p1; pn++)
        if (s_pg_mask[pn] & PG_OWNER_TEX)
            rsx_draw_engine_note_guest_write(pn << PG_SHIFT, 1u << PG_SHIFT);
}

/* The host's last word on a protection fault no owner claimed (project
 * main.cpp vm_commit_veh): the page goes back to what its owner mask asks for,
 * under the guard lock, and the tag cache is told protection moved. If that
 * allows the access it is retried; otherwise the owners are stale (a guard
 * nobody will lift), so they are dropped and the page opened -- through the
 * registry, so later decisions see it. Returns 0 only if the page cannot be
 * opened. */
int rsx_page_guard_reconcile(u32 ea, int is_write)
{
    if (!vm_base) return 0;
    const u32 pn = ea >> PG_SHIFT;
    AcquireSRWLockExclusive(&s_pg_lock);
    int level = s_pg_hold[pn] ? 0 : pg_level(s_pg_mask[pn]);
    int ok = pg_protect_run(pn, 1, level);
    const int allowed = ok && (level == 0 || (level == 1 && !is_write));
    u8 dropped = 0;
    if (!allowed) {
        dropped = s_pg_mask[pn];
        ok = pg_protect_run(pn, 1, 0);
        MemoryBarrier();
        s_pg_mask[pn] = 0;
    }
    s_pg_open_gen++;
    MemoryBarrier();
    ReleaseSRWLockExclusive(&s_pg_lock);
    if (dropped & PG_OWNER_TEX) rsx_draw_engine_note_guest_write(pn << PG_SHIFT, 1u << PG_SHIFT);
    if (dropped) {
        static volatile LONG logs = 0;
        if (InterlockedIncrement(&logs) <= 8)
            fprintf(stderr, "[rsx engine] page guard 0x%08X (owners 0x%X) faulted unclaimed; opened\n",
                    pn << PG_SHIFT, dropped);
    }
    return ok;
}

/* Taken by the host's fault handler when it defines the pointer.
 * RSX_ENG_PG_RECONCILE=0 leaves it unset (the host opens the page itself). */
extern int (*g_vm_guard_reconcile_hook)(u32, int) __attribute__((weak));
static void eng_pg_reconcile_install(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    const char* e = getenv("RSX_ENG_PG_RECONCILE");
    if (e && *e == '0') return;
    if (&g_vm_guard_reconcile_hook && !g_vm_guard_reconcile_hook)
        g_vm_guard_reconcile_hook = rsx_page_guard_reconcile;
}

#define PG_OWNER_WCB 2u
#define WCB_MAX_REQ 64u

extern int (*g_vm_access_violation_hook)(u32, int) __attribute__((weak));
extern void* cellGcm_fifo_kick_event(void);

static struct {
    u32 ea, len;                       /* the guarded guest range, 0 = none */
} s_wcb_rng[ENG_MAX_SURFACES];
static u16 s_wcb_refs[PG_COUNT];       /* guarded surfaces per page (engine thread) */
static volatile DWORD s_wcb_tid;       /* the engine's thread */
static SRWLOCK s_wcb_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_wcb_cv = CONDITION_VARIABLE_INIT;
static u32 s_wcb_req[WCB_MAX_REQ];
static volatile LONG s_wcb_nreq;
static int s_wcb_installed;

static int eng_wcb_enabled(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_ENG_WCB_FAULT"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}

static DWORD eng_wcb_wait_ms(void)
{
    /* RPCS3 has no timeout at all: the faulting thread waits for the flush.
     * The bound only keeps a walker parked on that very thread's label from
     * deadlocking it; 1 s rides out long engine work (shader compiles, big
     * readbacks) that 100 ms did not. */
    static int ms = -1;
    if (ms < 0) { const char* e = getenv("RSX_ENG_WCB_WAIT_MS"); ms = e ? atoi(e) : 1000; if (ms < 1) ms = 1; }
    return (DWORD)ms;
}

/* RSX_ENG_WCB_FULL_WAIT=0: a fault finding the request queue full opens its
 * page at once (no flush) instead of waiting for a slot. */
static int eng_wcb_full_wait(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_ENG_WCB_FULL_WAIT"); on = !(e && *e == '0'); }
    return on;
}

/* RSX_ENG_WCB_SELF_FLUSH=0: a fault on the engine's thread outside any engine
 * call (a guest flip / vblank handler it runs) only opens the page, as one
 * inside the engine must. */
static int s_wcb_self_flush = -1;   /* re-read at rsx_draw_engine_init */
static int eng_wcb_self_flush(void)
{
    int on = s_wcb_self_flush;
    if (on < 0) { const char* e = getenv("RSX_ENG_WCB_SELF_FLUSH"); s_wcb_self_flush = on = !(e && *e == '0'); }
    return on;
}

static void eng_wcb_flush_page(u32 pn);

/* Claims a fault on a WCB-guarded or texture-protected page; 0 = not ours. */
static int eng_wcb_fault(u32 ea, int is_write)
{
    const u32 pn = ea >> PG_SHIFT;
    if (!(s_pg_mask[pn] & PG_OWNER_WCB)) {
        /* RPCS3 on_access_violation -> invalidate_range: a write to a
         * protected texture page invalidates the textures on it (re-checked
         * at their next use) and unprotects it, then the write proceeds. */
        if (!(s_pg_mask[pn] & PG_OWNER_TEX)) return 0;
        (void)is_write;
        rsx_draw_engine_note_guest_write(pn << PG_SHIFT, 1u << PG_SHIFT);
        pg_guard_range(pn << PG_SHIFT, 1u << PG_SHIFT, PG_OWNER_TEX, 0);
        return 1;
    }
    if (GetCurrentThreadId() == s_wcb_tid) {
        /* Outside every engine call (eng_depth 0: guest code the engine's
         * thread runs, e.g. a flip handler) nothing is mid-command, so the
         * surfaces on the page are stored here, as RPCS3's fault handler
         * flushes on whichever thread faulted. Inside one (a texture or
         * transfer over the surface) nothing can flush: open the page; the
         * surface stays dirty and is stored on its next barrier. */
        if (!s_eng_depth && eng_wcb_self_flush() && g.ready) {
            static volatile LONG logs = 0;
            if (InterlockedIncrement(&logs) <= 8)
                fprintf(stderr, "[rsx engine] WCB: engine thread touched 0x%08X outside the engine;"
                                " storing in place\n", pn << PG_SHIFT);
            eng_wcb_flush_page(pn);
        }
        if (s_pg_mask[pn] & PG_OWNER_WCB)
            pg_guard_range(pn << PG_SHIFT, 1u << PG_SHIFT, PG_OWNER_WCB, 0);
        return 1;
    }
    HANDLE kick = (HANDLE)cellGcm_fifo_kick_event();
    const ULONGLONG t0 = GetTickCount64();
    const DWORD lim = eng_wcb_wait_ms();
    int queued = 0, full = 0;
    AcquireSRWLockExclusive(&s_wcb_lock);
    /* Wait for the request to leave the queue: rsx_draw_engine_wcb_service
     * removes it only after the surface is stored. The page mask is not a
     * completion signal without the sudo view, since the store then unguards
     * before it writes. A full queue (RPCS3 has none) is waited on for a slot
     * rather than opening the page unflushed. */
    for (;;) {
        int pending = 0;
        for (LONG i = 0; i < s_wcb_nreq; i++) if (s_wcb_req[i] == pn) { pending = 1; break; }
        if (!pending) {
            if (queued) break;                                    /* stored */
            if (!(s_pg_mask[pn] & PG_OWNER_WCB)) break;           /* opened meanwhile */
            if (s_wcb_nreq < (LONG)WCB_MAX_REQ) {
                s_wcb_req[s_wcb_nreq] = pn; s_wcb_nreq++;
                queued = 1;
                if (kick) SetEvent(kick);
            } else {
                full = 1;
                if (!eng_wcb_full_wait()) break;
                if (kick) SetEvent(kick);
            }
        } else if (!queued) {
            queued = 1;                                /* another thread's request */
            if (kick) SetEvent(kick);
        }
        const ULONGLONG el = GetTickCount64() - t0;
        if (el >= lim) break;
        SleepConditionVariableSRW(&s_wcb_cv, &s_wcb_lock, (DWORD)(lim - el) < 4u ? (DWORD)(lim - el) : 4u, 0);
    }
    ReleaseSRWLockExclusive(&s_wcb_lock);
    if (s_pg_mask[pn] & PG_OWNER_WCB) {
        static volatile LONG logs = 0;
        if (InterlockedIncrement(&logs) <= 8)
            fprintf(stderr, "[rsx engine] WCB: flush of guest 0x%08X timed out after %u ms (thread %lu,"
                            " %s%s, engine thread %lu); opening the page without it\n",
                    pn << PG_SHIFT, (unsigned)(GetTickCount64() - t0), (unsigned long)GetCurrentThreadId(),
                    is_write ? "write" : "read", full ? ", queue full" : "", (unsigned long)s_wcb_tid);
        pg_guard_range(pn << PG_SHIFT, 1u << PG_SHIFT, PG_OWNER_WCB, 0);
    }
    return 1;
}

static int eng_wcb_hook(u32 ea, int is_write) { return eng_wcb_fault(ea, is_write); }

static LONG WINAPI eng_wcb_veh(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || !vm_base)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t a = (uintptr_t)er->ExceptionInformation[1];
    if (a < (uintptr_t)vm_base || (u64)(a - (uintptr_t)vm_base) > 0xFFFFFFFFull)
        return EXCEPTION_CONTINUE_SEARCH;
    return eng_wcb_fault((u32)(a - (uintptr_t)vm_base), er->ExceptionInformation[0] == 1)
        ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

/* The runtime's VM fault handler calls g_vm_access_violation_hook for
 * protection faults on guest pages (and SPU DMA only proceeds onto a
 * NOACCESS page when it is set), so WCB takes that hook when it is free;
 * otherwise it adds its own handler. */
static int eng_wcb_install(void)
{
    if (s_wcb_installed) return s_wcb_installed > 0;
    s_wcb_installed = -1;
    int hooked = 0;
    if (&g_vm_access_violation_hook) {
        if (!g_vm_access_violation_hook) g_vm_access_violation_hook = eng_wcb_hook;
        hooked = g_vm_access_violation_hook == eng_wcb_hook;
    }
    if (hooked || AddVectoredExceptionHandler(1, eng_wcb_veh)) s_wcb_installed = 1;
    fprintf(stderr, "[rsx engine] WCB: CPU access to rendered colour surfaces flushes them (%s)\n",
            s_wcb_installed > 0 ? (hooked ? "VM fault hook" : "exception handler") : "unavailable");
    return s_wcb_installed > 0;
}

/* The guest range of a surface's pixel rows when it is contiguous in the
 * guest address space; 0 when it is not. */
static u32 eng_wcb_range(u32 location, u32 offset, u32 len, u32* ea)
{
    if (!vm_base || !len) return 0;
    const u32 a = cellGcmResolveLocated(location == RSX_LOCATION_LOCAL, offset);
    const u32 b = cellGcmResolveLocated(location == RSX_LOCATION_LOCAL, offset + len - 1u);
    if (!a || !b || b < a || b - a != len - 1u) return 0;
    if (ppu_vm_size && (u64)a + len > ppu_vm_size) return 0;
    *ea = a;
    return len;
}

static void eng_wcb_unguard(u32 slot)
{
    if (slot >= ENG_MAX_SURFACES || !s_wcb_rng[slot].len) return;
    const u32 ea = s_wcb_rng[slot].ea, len = s_wcb_rng[slot].len;
    s_wcb_rng[slot].len = 0;
    const u32 p0 = ea >> PG_SHIFT, p1 = (u32)(((u64)ea + len - 1u) >> PG_SHIFT);
    u32 run = 0, run_n = 0;
    for (u32 pn = p0; pn <= p1 + 1u; pn++) {
        const int drop = pn <= p1 && s_wcb_refs[pn] && --s_wcb_refs[pn] == 0;
        if (drop) { if (!run_n) run = pn; run_n++; continue; }
        if (run_n) pg_guard_range(run << PG_SHIFT, run_n << PG_SHIFT, PG_OWNER_WCB, 0);
        run_n = 0;
    }
}

/* A colour surface the GPU wrote: guard its pages until the guest touches
 * them, if eng_surface_to_guest can store it. */
static int s_texcache_legacy;
static u32 eng_color_bytes(const eng_surface* s);
static int eng_surface_packed_kind(const eng_surface* s);
static void eng_wcb_guard(u32 slot)
{
    if (slot >= ENG_MAX_SURFACES || s_wcb_rng[slot].len || !eng_wcb_enabled() ||
        eng_legacy_surfaces())
        return;
    const eng_surface* s = &g.surfaces[slot];
    /* RPCS3 locks every colour format it can flush; RSX_TEXCACHE_LEGACY=1
     * guards A8R8G8B8 only (what eng_surface_to_guest then stores). */
    const int flt = !s_texcache_legacy && !s->swz &&
        (s->fmt == RSX_BE_FMT_R16G16B16A16F || s->fmt == RSX_BE_FMT_R32G32B32A32F ||
         s->fmt == RSX_BE_FMT_R32F);
    const u32 bpp = s->fmt == RSX_BE_FMT_R8G8B8A8 ? (eng_surface_packed_kind(s) ? eng_color_bytes(s) : 4u)
                  : flt ? eng_color_bytes(s) : 0u;
    if (!s->handle || !s->guest_dirty || !bpp ||
        !s->pitch || s->pitch < s->w * bpp || !g.be->readback ||
        eng_surface_is_display_buffer(s->location, s->offset))
        return;
    u32 ea = 0;
    const u32 len = eng_wcb_range(s->location, s->offset, s->pitch * s->h, &ea);
    if (!len || !eng_wcb_install()) return;
    s_wcb_tid = GetCurrentThreadId();
    const u32 p0 = ea >> PG_SHIFT, p1 = (u32)(((u64)ea + len - 1u) >> PG_SHIFT);
    u32 run = 0, run_n = 0;
    for (u32 pn = p0; pn <= p1 + 1u; pn++) {
        const int add = pn <= p1 && s_wcb_refs[pn]++ == 0;
        if (add) { if (!run_n) run = pn; run_n++; continue; }
        if (run_n) pg_guard_range(run << PG_SHIFT, run_n << PG_SHIFT, PG_OWNER_WCB, 1);
        run_n = 0;
    }
    s_wcb_rng[slot].ea = ea;
    s_wcb_rng[slot].len = len;
}

static void eng_wcb_forget_all(void)
{
    for (u32 i = 0; i < ENG_MAX_SURFACES; i++) eng_wcb_unguard(i);
}
#else
int rsx_page_guard_set(u32 page, u32 owner, int on) { (void)page; (void)owner; (void)on; return 0; }
void rsx_page_guard_hold(u32 page, int delta) { (void)page; (void)delta; }
int rsx_page_guard_reconcile(u32 ea, int is_write) { (void)ea; (void)is_write; return 0; }
static void eng_store_done(const u8* p, u32 n) { (void)p; (void)n; }
static void eng_wcb_unguard(u32 slot) { (void)slot; }
static void eng_wcb_guard(u32 slot) { (void)slot; }
static void eng_wcb_forget_all(void) {}
#endif

static u64 eng_texture_content_hash(u32 location, u32 offset, u32 span, int* readable);
static u32 eng_texture_upload(u32 location, u32 offset, u32 fmt, u32 w, u32 h, u32 levels, u32 pitch, int cube, u32 remap, u32 depth);
static u32 eng_pipeline_get(const rsx_vertex_layout_plan* layout, const rsx_be_render_state* rs, rsx_be_format rt_fmt, u32 rt_count, int* out_fixed, u32* out_tex_used, u64* out_fp_hash);
static void sink_end(void* user, const rsx_dispatch* r);
static int eng_zdepth_to_guest_at(u32 slot, int site);
#define eng_zdepth_to_guest(slot) eng_zdepth_to_guest_at((slot), __LINE__)
static void eng_present(u32 buffer_id);

/* ---- render state -------------------------------------------------------- */

static float eng_half(u32 h)
{
    const u32 s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0)       v = (float)m / 16777216.0f;           /* m * 2^-24 */
    else if (e == 31) { const u32 bits = (s << 31) | 0x7F800000u | (m << 13); memcpy(&v, &bits, 4); return v; }
    else              v = (float)(1024u + m) * (float)(1u << e) / 33554432.0f; /* 2^(e-15)*(1+m/1024) */
    return s ? -v : v;
}

void rsx_draw_engine_trim_color_mask(u32 color_format, u32* color_mask, u32* color_mask_mrt)
{
    static int legacy = -1;
    if (eng_env_flag("RSX_COLOR_MASK_LEGACY", &legacy)) return;
    /* Lanes kept, as COLOR_MASK bytes and as an MRT nibble (A=1 R=2 G=4 B=8). */
    u32 keep, nib;
    switch (color_format) {
    case 0x03u: keep = 0x00FFFFFFu; nib = 0xEu; break;   /* R5G6B5: rgb */
    case 0x09u: keep = 0x000000FFu; nib = 0x8u; break;   /* B8: r=b -> .b */
    case 0x0Au: keep = 0x0000FFFFu; nib = 0xCu; break;   /* G8B8: r=b, g -> .g.b */
    case 0x0Du: keep = 0x00FF0000u; nib = 0x2u; break;   /* X32: r */
    default: return;
    }
    if (color_mask) *color_mask &= keep;
    if (color_mask_mrt) *color_mask_mrt &= (nib * 0x1111u) & 0xFFF0u;
}

/* RPCS3 evaluate_programmable_blending_state (RSXGraphEngine.cpp:73-112) on
 * the X1R5G5B5/X8R8G8B8/X8B8G8R8 _Z/_O targets: when a blend reads alpha,
 * it runs in the shader on the RAW shader alpha and only then is the alpha
 * remapped to 0 (_Z) / 1 (_O) (RSXROPEpilogue.glsl:51-65), the destination
 * read back being remapped too. Same result in fixed function: keep the raw
 * alpha out of the shader, replace the destination-alpha factors by that
 * constant, and store the constant alpha (_Z: alpha factors ZERO/ZERO;
 * _O: alpha left as the earlier remapped writes stored it). Signed
 * equations are left to the hardware path. RSX_ENG_XALPHA_PB=0: the remap
 * runs before the blend, as before. */
static u32 eng_xalpha_factor(u32 f, u32 c)
{
    switch (f) {
    case 0x0304u: return c ? 1u : 0u;              /* DST_ALPHA */
    case 0x0305u: return c ? 0u : 1u;              /* ONE_MINUS_DST_ALPHA */
    case 0x0308u: return c ? 0u : 0x0302u;         /* SRC_ALPHA_SATURATE: min(as, 1-c) */
    default: return f;
    }
}

/* RPCS3 surface_color_target_mask for SURFACE_COLOR_TARGET. */
static u32 eng_color_target_mask(u32 target)
{
    switch (target) {
    case 0x01u: return 0x1u;
    case 0x02u: return 0x2u;
    case 0x13u: return 0x3u;
    case 0x17u: return 0x7u;
    case 0x1Fu: return 0xFu;
    default:    return 0u;
    }
}

static void eng_x_alpha_blend(const rsx_dispatch* rsx, u32 color_format, rsx_be_render_state* rs)
{
    static int en = -1, mask_en = -1;
    if (en < 0) { const char* e = getenv("RSX_ENG_XALPHA_PB"); en = !(e && e[0] == '0'); }
    if (mask_en < 0) { const char* e = getenv("RSX_ENG_XALPHA_MASK"); mask_en = !(e && e[0] == '0'); }
    u32 c;
    switch (color_format) {
    case 0x01u: case 0x04u: case 0x0Eu: c = 0u; break;
    case 0x02u: case 0x05u: case 0x0Fu: c = 1u; break;
    default: return;
    }
    if (!en || rs->logic_op_enable) return;
    const u32 A = 0x01000000u;
    int writes_alpha;
    if (mask_en) {
        /* RSXGraphEngine.cpp:28,78-88: blend_enabled_mask & target mask, and
         * color_mask_a(i) up to the highest blended target. */
        const u32 bm = (rs->blend_enable | (rs->blend_mrt & 0xEu)) &
                       eng_color_target_mask(rsx_dsp_reg(rsx, 0x0220) & 0xFFu);
        if (!bm) return;
        writes_alpha = 0;
        for (u32 i = 0, b = bm; b; b >>= 1, i++)
            if (i == 0 ? (rs->color_mask & A) != 0 : ((rs->color_mask_mrt >> (4u * i)) & 1u) != 0)
                writes_alpha = 1;
    } else {
        if (!rs->blend_enable) return;
        writes_alpha = (rs->color_mask & A) || (rs->color_mask_mrt & 0x1110u);
    }
    const int reads_alpha = (rs->sf_rgb >= 0x0302u && rs->sf_rgb <= 0x0305u) || rs->sf_rgb == 0x0308u ||
                            (rs->df_rgb >= 0x0302u && rs->df_rgb <= 0x0305u) || rs->df_rgb == 0x0308u;
    const int alpha_blended = rs->eq_a == 0x800Bu || rs->eq_a == 0x8007u || rs->eq_a == 0x8008u ||
                              rs->sf_a != 1u || rs->df_a != 0u;
    if (!reads_alpha && !(writes_alpha && alpha_blended)) return;
    rs->x_alpha_pb = c + 1u;
    rs->sf_rgb = eng_xalpha_factor(rs->sf_rgb, c);
    rs->df_rgb = eng_xalpha_factor(rs->df_rgb, c);
    rs->eq_a = 0x8006u;     /* FUNC_ADD */
    if (c == 0u) {
        rs->sf_a = 0u; rs->df_a = 0u;
    } else {
        rs->sf_a = 0u; rs->df_a = 1u;
    }
}

void rsx_draw_engine_decode_render_state(const rsx_dispatch* rsx,
                                         rsx_be_render_state* rs)
{
    memset(rs, 0, sizeof(*rs));
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(rsx, &sf);
    /* RPCS3 rsx_methods.h alpha_test_enabled(): X32 and W32Z32Y32X32 targets
     * have no alpha test (fp16 W16Z16Y16X16 keeps it). */
    rs->alpha_test_enable = (sf.color_format == 0x0Cu || sf.color_format == 0x0Du)
        ? 0u : (rsx_dsp_reg(rsx, M_ALPHA_TEST_ENABLE) & 1);
    rs->alpha_func   = rsx_dsp_reg(rsx, M_ALPHA_FUNC);
    rs->alpha_ref_raw = rsx_dsp_reg(rsx, M_ALPHA_REF);
    rs->alpha_ref_format = sf.color_format;
    rs->rt_fp16 = sf.color_format == RSX_SURFACE_FMT_F_W16Z16Y16X16;
    /* RPCS3 blend_enabled_mask: BLEND_ENABLE_MRT bit 0 also enables surface 0. */
    rs->blend_enable = (rsx_dsp_reg(rsx, M_BLEND_ENABLE) | rsx_dsp_reg(rsx, 0x036C)) & 1;
    const u32 sfac = rsx_dsp_reg(rsx, M_BLEND_SFACTOR);
    const u32 dfac = rsx_dsp_reg(rsx, M_BLEND_DFACTOR);
    const u32 eq   = rsx_dsp_reg(rsx, M_BLEND_EQUATION);
    rs->sf_rgb = sfac & 0xFFFF; rs->sf_a = sfac >> 16;
    rs->df_rgb = dfac & 0xFFFF; rs->df_a = dfac >> 16;
    rs->eq_rgb = eq & 0xFFFF;   rs->eq_a = eq >> 16;
    rs->depth_test  = rsx_dsp_reg(rsx, M_DEPTH_TEST_ENABLE) & 1;
    rs->depth_write = rsx_dsp_reg(rsx, M_DEPTH_WRITE) & 1;
    rs->depth_func  = rsx_dsp_reg(rsx, M_DEPTH_FUNC);
    rs->cull_enable = rsx_dsp_reg(rsx, 0x183C) & 1;
    rs->cull_face   = rsx_dsp_reg(rsx, 0x1830);
    rs->front_face  = rsx_dsp_reg(rsx, 0x1834);
    /* The RAW register: a game-written 0 is a real "write no colour channel"
     * (a depth-prime pass), and rsx_dispatch_init seeds the nv40 reset value
     * so never-written reads as all-on. */
    rs->color_mask  = rsx_dsp_reg(rsx, M_COLOR_MASK);
    rs->color_mask_mrt = rsx_dsp_reg(rsx, 0x0370) & 0xFFF0u;
    rsx_draw_engine_trim_color_mask(sf.color_format, &rs->color_mask, &rs->color_mask_mrt);
    rs->blend_mrt      = rsx_dsp_reg(rsx, 0x036C) & 0xFu;
    rs->stencil_enable    = rsx_dsp_reg(rsx, M_STENCIL_TEST_ENABLE) & 1;
    rs->stencil_two_sided = rsx_dsp_reg(rsx, M_TWO_SIDED_STENCIL) & 1;
    rs->s_func       = rsx_dsp_reg(rsx, M_STENCIL_FUNC);
    rs->s_func_mask  = rsx_dsp_reg(rsx, M_STENCIL_FUNC_MASK) & 0xFF;
    rs->s_write_mask = rsx_dsp_reg(rsx, M_STENCIL_MASK) & 0xFF;
    rs->s_fail       = rsx_dsp_reg(rsx, M_STENCIL_OP_FAIL);
    rs->s_zfail      = rsx_dsp_reg(rsx, M_STENCIL_OP_ZFAIL);
    rs->s_zpass      = rsx_dsp_reg(rsx, M_STENCIL_OP_ZPASS);
    rs->bs_func      = rsx_dsp_reg(rsx, M_BACK_STENCIL_FUNC);
    rs->bs_fail      = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_FAIL);
    rs->bs_zfail     = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZFAIL);
    rs->bs_zpass     = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZPASS);
    rs->depth_bounds_test = rsx_dsp_reg(rsx, M_DEPTH_BOUNDS_ENABLE) & 1;
    rs->logic_op_enable = rsx_dsp_reg(rsx, 0x0374) & 1;
    rs->logic_op        = rs->logic_op_enable ? rsx_dsp_reg(rsx, 0x0378) : 0;
    rs->blend_color     = rsx_dsp_reg(rsx, 0x031C);
    rs->blend_color2    = rsx_dsp_reg(rsx, 0x037C);
    {
        const u32 zmm = rsx_dsp_reg(rsx, 0x1D78);
        rs->depth_clamp = (((zmm >> 4) & 0xFu) != 0 || (zmm & 0xFu) == 0) ? 1u : 0u;
    }
    rs->bs_func_mask  = rs->stencil_two_sided ? (rsx_dsp_reg(rsx, 0x0358) & 0xFFu) : 0u;
    rs->bs_write_mask = rs->stencil_two_sided ? (rsx_dsp_reg(rsx, 0x034C) & 0xFFu) : 0u;
    {
        const u32 pf = rsx_dsp_reg(rsx, 0x1828), pb = rsx_dsp_reg(rsx, 0x182C);
        rs->polygon_mode_front = (pf == 0x1B00u || pf == 0x1B01u) ? pf : 0x1B02u;
        rs->polygon_mode_back  = (pb == 0x1B00u || pb == 0x1B01u) ? pb : 0x1B02u;
    }
    rs->aa_samples = sf.aa_sx * sf.aa_sy;
    rs->aa_control = rsx_dsp_reg(rsx, 0x1D7C);
    eng_x_alpha_blend(rsx, sf.color_format, rs);
}

u64 rsx_draw_engine_hash_render_state(const rsx_be_render_state* rs, u64 hash)
{
#define ENG_HASH_FIELD(name) hash = eng_fnv1a(&rs->name, sizeof(rs->name), hash)
    /* The alpha REFERENCE is deliberately absent: it lives in the fragment
     * constant block, so a title animating a fade must not compile a new
     * pipeline per frame. Enable and compare mode still select a variant.
     * Every field is listed by name so struct padding can never be identity
     * (rsx_live_draw.c:3151-3185). */
    ENG_HASH_FIELD(alpha_test_enable);
    ENG_HASH_FIELD(alpha_func);
    ENG_HASH_FIELD(blend_enable);
    ENG_HASH_FIELD(sf_rgb);
    ENG_HASH_FIELD(df_rgb);
    ENG_HASH_FIELD(sf_a);
    ENG_HASH_FIELD(df_a);
    ENG_HASH_FIELD(eq_rgb);
    ENG_HASH_FIELD(eq_a);
    ENG_HASH_FIELD(depth_test);
    ENG_HASH_FIELD(depth_write);
    ENG_HASH_FIELD(depth_func);
    ENG_HASH_FIELD(cull_enable);
    ENG_HASH_FIELD(cull_face);
    ENG_HASH_FIELD(front_face);
    ENG_HASH_FIELD(color_mask);
    ENG_HASH_FIELD(color_mask_mrt);
    ENG_HASH_FIELD(blend_mrt);
    ENG_HASH_FIELD(rt_fp16);
    ENG_HASH_FIELD(stencil_enable);
    ENG_HASH_FIELD(stencil_two_sided);
    ENG_HASH_FIELD(s_func);
    ENG_HASH_FIELD(s_func_mask);
    ENG_HASH_FIELD(s_write_mask);
    ENG_HASH_FIELD(s_fail);
    ENG_HASH_FIELD(s_zfail);
    ENG_HASH_FIELD(s_zpass);
    ENG_HASH_FIELD(bs_func);
    ENG_HASH_FIELD(bs_fail);
    ENG_HASH_FIELD(bs_zfail);
    ENG_HASH_FIELD(bs_zpass);
    ENG_HASH_FIELD(depth_bounds_test);
    ENG_HASH_FIELD(logic_op_enable);
    ENG_HASH_FIELD(logic_op);
    ENG_HASH_FIELD(depth_clamp);
    ENG_HASH_FIELD(bs_func_mask);
    ENG_HASH_FIELD(bs_write_mask);
    ENG_HASH_FIELD(polygon_mode_front);
    ENG_HASH_FIELD(polygon_mode_back);
    ENG_HASH_FIELD(aa_samples);
    ENG_HASH_FIELD(aa_control);
    ENG_HASH_FIELD(x_alpha_pb);
#undef ENG_HASH_FIELD
    return hash;
}

/* ---- topology expansion -------------------------------------------------- */

/* Every expansion is bounded by the restart cuts, so a strip that the guest
 * broke with the sentinel index gets no connecting triangle across the break.
 * Missing that decode is what upstream's aa7fb63 records as "the exploded
 * spiky mesh that occluded the scene". */
u32 rsx_draw_engine_topology_index_count(u32 primitive, u32 source_refs,
                                         const u32* cuts, u32 cut_count)
{
    const u32 segments = cut_count + 1;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        return source_refs - source_refs % 3u;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_QUAD_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON: {
        /* RPCS3 draws a quad strip as a native triangle strip, so an odd
         * trailing vertex still closes a triangle. */
        u32 total = 0;
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            (void)begin;
            if (count >= 3) total += (count - 2) * 3;
        }
        return total;
    }
    case RSX_PRIMITIVE_QUADS: {
        u32 total = 0;
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            (void)begin;
            total += (count / 4u) * 6u;
        }
        return total;
    }
    /* Line strips and loops become line lists, so a restart cut breaks the
     * strip instead of bridging it; a loop adds its closing edge back to
     * the segment's first vertex (RPCS3 expand_line_loop_array). */
    case RSX_PRIMITIVE_LINE_STRIP:
    case RSX_PRIMITIVE_LINE_LOOP: {
        u32 total = 0;
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            (void)begin;
            if (primitive == RSX_PRIMITIVE_LINE_LOOP && cut_count &&
                !rsx_vertex_legacy()) {
                if (count >= 2) total += (count - 1u) * 2u;
                if (s == segments - 1u && count && cuts[0] != 0) total += 2u;
                continue;
            }
            if (count < 2) continue;
            total += (count - 1u) * 2u;
            if (primitive == RSX_PRIMITIVE_LINE_LOOP) total += 2u;
        }
        return total;
    }
    default:
        return 0;
    }
}

static u32 eng_topology_vertex(const u32* occurrence_to_unique, u32 occurrence)
{
    return occurrence_to_unique ? occurrence_to_unique[occurrence] : occurrence;
}

void rsx_draw_engine_write_topology_indices(u32 primitive, u32 source_refs,
                                            const u32* cuts, u32 cut_count,
                                            const u32* occurrence_to_unique,
                                            u32* indices)
{
    u32 write = 0;
    const u32 segments = cut_count + 1;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES: {
        const u32 count = source_refs - source_refs % 3u;
        for (u32 o = 0; o < count; o++)
            indices[write++] = eng_topology_vertex(occurrence_to_unique, o);
        break;
    }
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 3) continue;
            /* Odd triangles swap their first two vertices, so the whole strip
             * keeps one winding. */
            for (u32 i = 0; i + 2 < count; i++) {
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + (i & 1u));
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + 1u - (i & 1u));
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + 2u);
            }
        }
        break;
    /* A polygon is a convex fan around its own first vertex, which is what
     * both of this tree's other renderers make of one. */
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 3) continue;
            for (u32 i = 1; i + 1 < count; i++) {
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i + 1u);
            }
        }
        break;
    case RSX_PRIMITIVE_QUADS:
        /* A restart cut empties the pending four-vertex set
         * (RPCS3 expand_indexed_quads). */
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            for (u32 q = 0; q < count / 4u; q++) {
                const u32 base = begin + q * 4u;
                indices[write++] = eng_topology_vertex(occurrence_to_unique, base);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 1u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 2u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 2u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 3u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, base);
            }
        }
        break;
    case RSX_PRIMITIVE_LINE_STRIP:
    case RSX_PRIMITIVE_LINE_LOOP:
        /* RPCS3 uploads an indexed loop as one restart-split strip and
         * appends src[0] (BufferUtils.cpp write_index_array_data_to_buffer_impl),
         * so only the last segment closes, and to the draw's very first index.
         * A leading restart index leaves nothing to close to. */
        if (primitive == RSX_PRIMITIVE_LINE_LOOP && cut_count &&
            !rsx_vertex_legacy()) {
            for (u32 s = 0; s < segments; s++) {
                u32 begin, count;
                rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                           &begin, &count);
                for (u32 i = 0; i + 1 < count; i++) {
                    indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i);
                    indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i + 1u);
                }
                if (s == segments - 1u && count && cuts[0] != 0) {
                    indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + count - 1u);
                    indices[write++] = eng_topology_vertex(occurrence_to_unique, 0);
                }
            }
            break;
        }
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 2) continue;
            for (u32 i = 0; i + 1 < count; i++) {
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i + 1u);
            }
            if (primitive == RSX_PRIMITIVE_LINE_LOOP) {
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + count - 1u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin);
            }
        }
        break;
    default:
        break;
    }
}

/* Does this primitive become an indexed list, and is the index buffer
 * needed at all? rsx_vertex_topology_plan answers that for the reference
 * engine, whose own expansion stops at quads. Quad strips, polygons and
 * line strips/loops are this engine's, so their cases are decided here
 * rather than in the shared helper, which the vendored rsx_live_draw.c also
 * calls. Line strips and loops become line lists (eng_topology_host). */
static int eng_topology_rebuild(u32 primitive, int refs_remapped, int* indexed)
{
    if (primitive == RSX_PRIMITIVE_QUAD_STRIP ||
        primitive == RSX_PRIMITIVE_POLYGON ||
        primitive == RSX_PRIMITIVE_LINE_STRIP ||
        primitive == RSX_PRIMITIVE_LINE_LOOP) {
        (void)refs_remapped;
        *indexed = 1;
        return 1;
    }
    return rsx_vertex_topology_plan(primitive, refs_remapped, indexed);
}

/* The host topology a rebuilt primitive is drawn with. */
static rsx_topology eng_topology_host(u32 primitive)
{
    return (primitive == RSX_PRIMITIVE_LINE_STRIP ||
            primitive == RSX_PRIMITIVE_LINE_LOOP)
        ? RSX_TOPOLOGY_LINES : RSX_TOPOLOGY_TRIANGLES;
}

/* ---- surfaces ------------------------------------------------------------ */

static rsx_be_format eng_surface_format(u32 color_format)
{
    switch (color_format & 0x1Fu) {
    case RSX_SURFACE_FMT_F_W16Z16Y16X16: return RSX_BE_FMT_R16G16B16A16F;
    case 0x0C: return RSX_BE_FMT_R32G32B32A32F;
    case 0x0D: return RSX_BE_FMT_R32F;
    default:   return RSX_BE_FMT_R8G8B8A8;
    }
}

/* Decode the guest's own bytes behind a colour surface into host R,G,B,A rows
 * so a freshly created target starts with what the title CPU-initialised it
 * to. Returns the staging pointer, or NULL when the bytes are unreadable or
 * the format is not one the decoder handles. */
static const void* eng_surface_seed(u32 location, u32 offset, u32 w, u32 h,
                                    rsx_be_format fmt, u32* out_row_bytes)
{
    *out_row_bytes = 0;
    if (fmt != RSX_BE_FMT_R8G8B8A8) return NULL;
    /* Only a surface in IO-mapped MAIN memory is seeded. RSX local memory is
     * reserved and not backed by this tree's guest VM -- runtime/memory/vm.h
     * maps the whole space PROT_NONE and commits main memory and the stacks,
     * leaving a runner to commit what its own title needs -- so reading a VRAM
     * surface's backing would fault long before it could help. Both halves are
     * checked: the DMA context says which space the offset is in, and the IO
     * table says the page is really mapped. */
    if (location != RSX_LOCATION_MAIN ||
        (!s_guest_reader && !cellGcmResolveIO(offset))) return NULL;
    rsx_tex_layout tl;
    /* A8R8G8B8 with the LN bit: a surface is linear, never swizzled. */
    rsx_texture_layout(0x85u | 0x20u, w, h, &tl);
    if (!tl.face_bytes || !tl.dst_row_bytes) return NULL;
    const u8* src = eng_guest_image(location, offset, tl.face_bytes);
    if (!src) return NULL;
    const u32 bytes = tl.dst_row_bytes * tl.rows;
    if (g.tex_staging_cap < bytes) {
        u8* n = (u8*)realloc(g.tex_staging, bytes);
        if (!n) return NULL;
        g.tex_staging = n;
        g.tex_staging_cap = bytes;
    }
    rsx_texture_decode(g.tex_staging, tl.dst_row_bytes, src, w, h, &tl,
                       rsx_texture_argb_is_rgba());
    *out_row_bytes = tl.dst_row_bytes;
    return g.tex_staging;
}

/* The colour target for this (location, offset), created or reallocated when
 * the size or format moved. Returns a slot index, or ENG_INVALID.
 * rsx_live_draw.c's surface_get (2605-2690).
 *
 * A smaller clip on the same rows is the same memory, not a new surface: with
 * the same format and pitch, RSX draws into the top-left of what is already
 * there. God of War: Ascension renders its scene into a 1280x720 target, then
 * runs a 640x360 pass at the same address and pitch and copies the full
 * 1280x720 out for its SPU anti-aliasing; recreating the target at 640x360
 * lost the scene and left that quarter of the frame black. */
/* The colour surface at an address whose contents are newest -- with one
 * surface per antialias mode there can be two. A tie goes to a current
 * attachment (`targets`, may be NULL), which was just brought up to date. */
static u32 eng_surface_newest(u32 location, u32 offset, const u32* targets, u32 n_targets)
{
    u32 best = ENG_INVALID;
    int best_bound = 0;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* s = &g.surfaces[i];
        if (!s->handle || s->location != location || s->offset != offset) continue;
        int bound = 0;
        for (u32 k = 0; k < n_targets; k++) if (targets[k] == i) bound = 1;
        if (best != ENG_INVALID) {
            const u64 bs = g.surfaces[best].color_serial;
            if (s->color_serial < bs || (s->color_serial == bs && (best_bound || !bound)))
                continue;
        }
        best = i;
        best_bound = bound;
    }
    return best;
}

/* Bring surface `dst` up to date from `src`, the same address under another
 * antialias mode, when src holds the newer contents: the overlapping pixels
 * are scaled from src's sample grid onto dst's, a 2x1 grid resolving to one
 * sample per pixel by the linear filter. */
static void eng_surface_inherit(u32 dst, u32 src)
{
    if (src == ENG_INVALID || src == dst || !g.be->color_target_blit) return;
    eng_surface* d = &g.surfaces[dst];
    const eng_surface* s = &g.surfaces[src];
    if (!s->handle || !d->handle || s->fmt != d->fmt ||
        s->color_serial <= d->color_serial)
        return;
    u32 pw = s->w / s->aa_sx, ph = s->h / s->aa_sy;
    if (d->w / d->aa_sx < pw) pw = d->w / d->aa_sx;
    if (d->h / d->aa_sy < ph) ph = d->h / d->aa_sy;
    if (!pw || !ph) return;
    g.be->color_target_blit(g.be->user, s->handle, 0, 0, pw * s->aa_sx, ph * s->aa_sy,
                            d->handle, 0, 0, pw * d->aa_sx, ph * d->aa_sy);
    d->color_serial = s->color_serial;
    d->guest_dirty = s->guest_dirty;
    if (d->region) { g.be->texture_release(g.be->user, d->region); d->region = 0; }
}

/* RPCS3 rsx::intersect_region for two surfaces laid out with one pitch, in
 * normalized bytes (RPCS3 get_normalized_memory_area: a row is width x bpp x
 * samples_x bytes, a surface height x samples_y rows): where src's memory
 * falls inside dst's. Returns 0 when they do not meet. */
typedef struct { u32 sxb, sy, dxb, dy, wb, rows; } eng_isect;
static int eng_intersect(u32 src_off, u32 src_wb, u32 src_rows,
                         u32 dst_off, u32 dst_wb, u32 dst_rows, u32 pitch, eng_isect* r)
{
    if (!pitch) return 0;
    if (src_wb > pitch) src_wb = pitch;
    if (dst_wb > pitch) dst_wb = pitch;
    if (src_off >= dst_off) {
        const u32 d = src_off - dst_off, y = d / pitch, x = d % pitch;
        if (y >= dst_rows || x >= dst_wb) return 0;
        r->sxb = 0; r->sy = 0; r->dxb = x; r->dy = y;
        r->wb = src_wb < dst_wb - x ? src_wb : dst_wb - x;
        r->rows = src_rows < dst_rows - y ? src_rows : dst_rows - y;
    } else {
        const u32 d = dst_off - src_off, y = d / pitch, x = d % pitch;
        if (y >= src_rows || x >= src_wb) return 0;
        r->sxb = x; r->sy = y; r->dxb = 0; r->dy = 0;
        r->wb = dst_wb < src_wb - x ? dst_wb : src_wb - x;
        r->rows = dst_rows < src_rows - y ? dst_rows : src_rows - y;
    }
    return r->wb && r->rows;
}

static u32 eng_transfer_bpp(rsx_be_format fmt);
#define ENG_LOC_SCRATCH 2u

/* ---- RPCS3 typeless inheritance (copy_image_typeless) --------------------
 * formats_are_bitcast_compatible (VKFormats.cpp): a colour and a depth
 * surface never are (aspects differ), two zetas only with the same format
 * class (Z16 vs Z24S8, fixed vs depth_float all differ), two colour surfaces
 * only with the same texel layout. Anything else hands its guest bytes over
 * reinterpreted, x scaled by src_bpp / dst_bpp (the scaling hint). */
static u32 eng_zeta_bpp(u32 zfmt);

/* RSX_BE_TYPELESS_* of a colour surface's guest layout (RPCS3's surface
 * format table, VKGSRender.cpp get_compatible_surface_format; the X1A7
 * formats RPCS3 has no entry for fall back to B8G8R8A8 there, ARGB8 here),
 * or ENG_INVALID for a host format without one. */
static u32 eng_color_tkind(const eng_surface* s)
{
    if (s->fmt == RSX_BE_FMT_R32F) return RSX_BE_TYPELESS_X32;
    if (s->fmt == RSX_BE_FMT_R16G16B16A16F) return RSX_BE_TYPELESS_FP16X4;
    if (s->fmt == RSX_BE_FMT_R32G32B32A32F) return RSX_BE_TYPELESS_FP32X4;
    if (s->fmt != RSX_BE_FMT_R8G8B8A8) return ENG_INVALID;
    switch (s->cfmt) {
    case 0u: case 4u: case 5u: case 6u: case 7u: case 8u: return RSX_BE_TYPELESS_ARGB8;
    case 1u: case 2u:                   return RSX_BE_TYPELESS_ARGB1555;
    case 3u:                            return RSX_BE_TYPELESS_RGB565;
    case 9u:                            return RSX_BE_TYPELESS_B8;
    case 0xAu:                          return RSX_BE_TYPELESS_G8B8;
    case 0xEu: case 0xFu: case 0x10u:   return RSX_BE_TYPELESS_ABGR8;
    default: return ENG_INVALID;
    }
}

/* SURFACE_FORMAT depth field a zeta's host target holds (1 Z16, 2 Z24S8). */
static u32 eng_zeta_zf(const eng_zdepth* z)
{
    if (z->host_zfmt) return z->host_zfmt;
    return z->zfmt == 1u ? 1u : 2u;
}

static u32 eng_zeta_tkind(u32 zf, u32 depth_float)
{
    if (zf == 1u) return depth_float ? RSX_BE_TYPELESS_Z16_F : RSX_BE_TYPELESS_Z16;
    return depth_float ? RSX_BE_TYPELESS_Z24S8_F : RSX_BE_TYPELESS_Z24S8;
}

static u32 eng_tkind_bpp(u32 k) { return RSX_BE_TYPELESS_BPP(k); }

/* ---- RPCS3 memory tags (surface_utils.h queue_tag / sync_tag / test) ------
 * on_write samples three u64 of guest memory under the surface -- its first
 * word, the last 8 bytes of its last sample row, and the middle -- and
 * memory_barrier passes an old_contents source on only while those still
 * read the same (`accept_all` when the destination's own tag holds). Memory
 * the CPU or a DMA rewrote since a surface was drawn no longer holds what it
 * drew: RPCS3 lets the new surface keep its clear_memory instead of
 * inheriting the stale GPU copy, and get_merged_texture_memory_region does
 * not sample it. RSX_ENG_MEMORY_TAG=0 skips the test. */
static int eng_memory_tag(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("RSX_ENG_MEMORY_TAG");
        v = (e && e[0] == '0') ? 0 : 1;
    }
    return v && !eng_legacy_surfaces();
}

/* get_bpp: the guest bytes per sample of the surface's SURFACE_FORMAT colour
 * format, the host format's when none was recorded. */
static u32 eng_color_bytes(const eng_surface* s)
{
    switch (s->cfmt) {
    case 9:                    return 1u;
    case 1: case 2: case 3:
    case 0xA:                  return 2u;
    case 0xB:                  return 8u;
    case 0xC:                  return 16u;
    case 0:                    break;
    default:                   return 4u;
    }
    switch (s->fmt) {
    case RSX_BE_FMT_R16G16B16A16F: return 8u;
    case RSX_BE_FMT_R32G32B32A32F: return 16u;
    default: return 4u;
    }
}

/* queue_tag's sample addresses for a surface `row_bytes` wide (native_pitch,
 * samples included) and `rows` sample rows high at `pitch`. */
static void eng_tag_points(u32 offset, u32 row_bytes, u32 rows, u32 pitch, u32 at[3])
{
    const u32 sx = row_bytes > 8u ? row_bytes - 8u : 0u;
    const u32 sy = rows ? rows - 1u : 0u;
    at[0] = offset;
    at[1] = offset + sy * pitch + sx;
    at[2] = offset + (sy / 2u) * pitch + sx / 2u;
}

/* One u64 of guest memory for a tag, read without tripping a WCB guard:
 * through the sudo view, or (without one) a guarded page is held open around
 * the read (rsx_page_guard_hold), so the surface keeps its guard and stays
 * dirty. */
static int eng_tag_read(u32 location, u32 offset, u64* out)
{
    const u8* p = eng_guest_ptr(NULL, location, offset, 8u);
    if (!p) return 0;
#ifdef _WIN32
    if (vm_base && p >= vm_base && (u64)(p - vm_base) + 8u <= 0x100000000ull) {
        const u32 ea = (u32)(p - vm_base);
        const u32 p0 = ea >> PG_SHIFT, p1 = (ea + 7u) >> PG_SHIFT;
        const int g0 = pg_level(s_pg_mask[p0]) > 1,
                  g1 = p1 != p0 && pg_level(s_pg_mask[p1]) > 1;
        /* RPCS3 peeks a protected section through the super pointer: no
         * protection moves and no write can slip in. */
        if ((g0 || g1) && eng_sudo_on()) {
            memcpy(out, eng_sudo_base() + ea, 8u);
            return 1;
        }
        /* RSX_PERF_TAGPEEK: a NOACCESS page nobody holds open cannot change
         * without its protection being lowered first (s_pg_open_gen), so a
         * value read from it stays exact until that counter moves. Misses
         * peek through PAGE_READONLY under the guard lock, so no write can
         * slip in while the page is open for the read. */
        static int peek = -1;
        if (g0 && (p1 == p0 || g1) && eng_perf_on("RSX_PERF_TAGPEEK", &peek)) {
            static struct { u32 ea, gen; u64 v; } cache[1024];
            const u32 ci = (ea >> 3) & 1023u;
            const u32 gen = s_pg_open_gen;
            MemoryBarrier();
            if (cache[ci].ea == ea && cache[ci].gen == gen &&
                pg_level(s_pg_mask[p0]) > 1 && !s_pg_hold[p0] &&
                pg_level(s_pg_mask[p1]) > 1 && !s_pg_hold[p1]) {
                *out = cache[ci].v;
                ENG_PC(PC_TAG, 1);
                return 1;
            }
            int done = 0;
            AcquireSRWLockExclusive(&s_pg_lock);
            if (pg_level(s_pg_mask[p0]) > 1 && !s_pg_hold[p0] &&
                pg_level(s_pg_mask[p1]) > 1 && !s_pg_hold[p1]) {
                const u32 n = p1 - p0 + 1u;
                u8* base = vm_base + ((size_t)p0 << PG_SHIFT);
                DWORD old;
                if (VirtualProtect(base, (size_t)n << PG_SHIFT, PAGE_READONLY, &old)) {
                    memcpy(out, p, 8u);
                    VirtualProtect(base, (size_t)n << PG_SHIFT, PAGE_NOACCESS, &old);
                    cache[ci].ea = ea; cache[ci].gen = s_pg_open_gen; cache[ci].v = *out;
                    done = 1;
                }
            }
            ReleaseSRWLockExclusive(&s_pg_lock);
            ENG_PC(PC_TAG, 0);
            if (done) return 1;
        }
        if (g0) rsx_page_guard_hold(p0 << PG_SHIFT, 1);
        if (g1) rsx_page_guard_hold(p1 << PG_SHIFT, 1);
        memcpy(out, p, 8u);
        if (g1) rsx_page_guard_hold(p1 << PG_SHIFT, -1);
        if (g0) rsx_page_guard_hold(p0 << PG_SHIFT, -1);
        return 1;
    }
#endif
    memcpy(out, p, 8u);
    return 1;
}

static void eng_tag_sync(u32 location, u32 offset, u32 row_bytes, u32 rows, u32 pitch,
                         u64 tag[3], u32* ok)
{
    *ok = 0;
    if (!eng_memory_tag() || location >= ENG_LOC_SCRATCH || !rows || !row_bytes) return;
    if (!pitch) pitch = row_bytes;
    u32 at[3];
    eng_tag_points(offset, row_bytes, rows, pitch, at);
    for (u32 n = 0; n < 3u; n++)
        if (eng_tag_read(location, at[n], &tag[n])) *ok |= 1u << n;
}

static int eng_tag_test(u32 location, u32 offset, u32 row_bytes, u32 rows, u32 pitch,
                        const u64 tag[3], u32 ok)
{
    if (!eng_memory_tag() || !ok) return 1;
    if (!pitch) pitch = row_bytes;
    u32 at[3];
    eng_tag_points(offset, row_bytes, rows, pitch, at);
    for (u32 n = 0; n < 3u; n++) {
        u64 v;
        if (!((ok >> n) & 1u) || !eng_tag_read(location, at[n], &v)) continue;
        if (v != tag[n]) return 0;
    }
    return 1;
}

static void eng_surface_tag(u32 slot)
{
    eng_surface* s = &g.surfaces[slot];
    if (s->swz) { s->tag_ok = 0; return; }
    eng_tag_sync(s->location, s->offset, s->w * eng_color_bytes(s), s->h, s->pitch,
                 s->tag, &s->tag_ok);
}

static int eng_surface_tag_test(const eng_surface* s)
{
    return eng_tag_test(s->location, s->offset, s->w * eng_color_bytes(s), s->h, s->pitch,
                        s->tag, s->tag_ok);
}

static void eng_zdepth_tag_rect(const eng_zdepth* z, u32* row_bytes, u32* rows)
{
    const u32 sx = z->aa_sx ? z->aa_sx : 1u, sy = z->aa_sy ? z->aa_sy : 1u;
    u32 rb = (z->used_w ? z->used_w : z->w) * sx * eng_zeta_bpp(z->zfmt);
    if (z->pitch && rb > z->pitch) rb = z->pitch;
    *row_bytes = rb;
    *rows = (z->used_h ? z->used_h : z->h) * sy;
}

static void eng_zdepth_tag(u32 slot)
{
    eng_zdepth* z = &g.zdepths[slot];
    u32 rb, rows;
    eng_zdepth_tag_rect(z, &rb, &rows);
    eng_tag_sync(z->location, z->offset, rb, rows, z->pitch, z->tag, &z->tag_ok);
}

static int eng_zdepth_tag_test(const eng_zdepth* z)
{
    u32 rb, rows;
    eng_zdepth_tag_rect(z, &rb, &rows);
    return eng_tag_test(z->location, z->offset, rb, rows, z->pitch, z->tag, z->tag_ok);
}

/* Whether colour surface `s` hands its texels to `d` as they are: the same
 * guest layout (RPCS3 formats_are_bitcast_compatible plus the byte shuffle
 * copy_image_typeless does for any other pair -- the guest bytes always
 * survive, so only an identical layout is a value copy). `dk`: d's kind, or
 * ENG_INVALID with the typeless path off, where the host format decides. */
static int eng_color_same_layout(const eng_surface* s, const eng_surface* d, u32 dk)
{
    const u32 sk = dk == ENG_INVALID ? ENG_INVALID : eng_color_tkind(s);
    if (sk == ENG_INVALID) return s->fmt == d->fmt;
    return sk == dk;
}

/* The guest bytes per sample of colour surface `s` the overlap maths uses:
 * its layout's with the typeless path on, else the host texel's. */
static u32 eng_color_isect_bpp(const eng_surface* s, u32 kind)
{
    return kind != ENG_INVALID ? eng_tkind_bpp(kind) : eng_transfer_bpp(s->fmt);
}

/* RSX_ENG_SPLIT_SURFACES=0: a render target replaced at its address by a
 * smaller one drops the pixels outside the new one (pre-parity). Default:
 * RPCS3 split_surface_region keeps them as clones at their own addresses,
 * and intersect_surface_region frees such a clone once a newer surface
 * inherits all of it (superseded_surfaces). */
static int eng_split_surfaces(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("RSX_ENG_SPLIT_SURFACES");
        v = (e && e[0] == '0') ? 0 : 1;
    }
    return v && !eng_legacy_surfaces();
}

static void eng_wcb_unguard(u32 slot);

/* invalidate(): drop colour surface `i` from the store; its slot is free. */
static void eng_surface_free(u32 i)
{
    eng_surface* s = &g.surfaces[i];
    if (s->feedback) g.be->texture_release(g.be->user, s->feedback);
    if (s->region) g.be->texture_release(g.be->user, s->region);
    if (s->reint) g.be->texture_release(g.be->user, s->reint);
    if (s->handle) g.be->color_target_release(g.be->user, s->handle);
    eng_wcb_unguard(i);
    memset(s, 0, sizeof *s);
}

/* intersect_surface_region: a split clone whose every byte the new surface
 * inherited (inherit_surface_contents == full) is freed. */
static u64 s_eng_rt_keep;
static int s_eng_occl_active;   /* host occlusion queries begun, not ended */
static int eng_surface_pinned(u32 i)
{
    return ((s_eng_rt_keep >> i) & 1u) || i == g.last_present_surface;
}

static void eng_surface_superseded(u32 i, const eng_isect* r, u32 sbpp)
{
    const eng_surface* s = &g.surfaces[i];
    if (!s->storage || !eng_split_surfaces() || eng_surface_pinned(i)) return;
    if (r->sxb == 0 && r->sy == 0 && r->wb >= s->w * sbpp && r->rows >= s->h)
        eng_surface_free(i);
}

/* intersect_surface_region invalidates a fully covered source in either
 * store: a zeta whose every byte a new surface inherited is no longer
 * written back (the inheritor carries its dirt). RSX_ENG_XKIND_SUPERSEDE=0
 * keeps it dirty, as before. */
static int eng_xkind_supersede(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_ENG_XKIND_SUPERSEDE"); v = !(e && e[0] == '0'); }
    return v;
}
static void eng_zdepth_superseded(eng_zdepth* z, const eng_isect* r, u32 sbpp)
{
    const u32 zw = z->used_w ? z->used_w : z->w, zh = z->used_h ? z->used_h : z->h;
    if (eng_xkind_supersede() && r->sxb == 0 && r->sy == 0 && r->wb >= zw * sbpp && r->rows >= zh)
        z->guest_dirty = 0;
}

/* The typeless copy of the bytes `r` names, whole texels on both sides. */
static int eng_typeless_isect(u32 src, u32 sk, u32 dst, u32 dk, const eng_isect* r)
{
    const u32 sb = eng_tkind_bpp(sk), db = eng_tkind_bpp(dk);
    if (!g.be->surface_typeless_copy || (r->sxb % sb) || (r->dxb % db)) return 0;
    const u32 wb = r->wb - r->wb % (sb > db ? sb : db);
    if (!wb) return 0;
    return g.be->surface_typeless_copy(g.be->user, src, sk, r->sxb / sb, r->sy, wb / sb, r->rows,
                                       dst, dk, r->dxb / db, r->dy, wb / db, r->rows);
}

/* RPCS3 memory_barrier (VKRenderTargets.cpp:1037-1131): a reused surface
 * whose own tag fails and whose every overlapping newer source fails its tag
 * is erased (colour 0,0,0,1; depth 1.0 / stencil 255), not left holding its
 * old frame. RSX_ENG_STALE_ERASE=0 keeps the old contents. */
static int eng_stale_erase(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_ENG_STALE_ERASE"); v = !(e && e[0] == '0'); }
    return v;
}

/* RPCS3 surface_store::intersect_surface_region (+ set_old_contents for the
 * same address under another antialias mode): every colour surface newer
 * than `dst` in the same memory partition, laid out with the same pitch and
 * overlapping its memory, hands over the bytes they share, oldest first so
 * the newest wins. In sample space the copy is 1:1 in rows and scaled by
 * src_bpp / dst_bpp in x (transfer_scale), so an AA 2x1 surface rebound
 * without AA at its pitch gets the left half of the sample rows as the
 * hardware's memory would hold them, not a resolve. Point sampled, as
 * memory_barrier's scale_image (linear = false). RPCS3 scans the depth store
 * too (list2), and bind_surface_address takes a zeta at this very address
 * as old_surface: those, and colour surfaces of another layout, are its
 * typeless copy (surface_typeless_copy). */
static void eng_surface_intersect(u32 dst)
{
    eng_surface* d = &g.surfaces[dst];
    if (!d->handle || !d->pitch || d->swz || d->location >= ENG_LOC_SCRATCH ||
        !g.be->color_target_blit)
        return;
    const u32 dk = eng_typeless_inherit() ? eng_color_tkind(d) : ENG_INVALID;
    const u32 dbpp = eng_color_isect_bpp(d, dk);
    if (!dbpp) return;
    const u64 base = d->color_serial;
    /* memory_barrier accept_all: a written destination whose tag still holds. */
    const int accept = d->color_serial && d->tag_ok && eng_surface_tag_test(d);
    u32 n_stale = 0;
    /* order[]: colour slot i, or ENG_MAX_SURFACES + zeta slot. */
    u32 order[ENG_MAX_SURFACES + ENG_MAX_ZDEPTHS], n = 0;
    for (u32 i = 0; i < g.n_surfaces + g.n_zdepths; i++) {
        u64 ser;
        if (i < g.n_surfaces) {
            const eng_surface* s = &g.surfaces[i];
            if (i == dst || !s->handle || s->swz || s->location != d->location ||
                s->color_serial <= base)
                continue;
            if (!eng_color_same_layout(s, d, dk) &&
                (dk == ENG_INVALID || eng_color_tkind(s) == ENG_INVALID))
                continue;
            if (s->pitch != d->pitch && s->h / s->aa_sy != 1u && d->h / d->aa_sy != 1u) continue;
            if (!accept && !eng_surface_tag_test(s)) {
                eng_isect r;
                n_stale += eng_intersect(s->offset, s->w * eng_color_bytes(s), s->h, d->offset,
                                         d->w * dbpp, d->h, d->pitch, &r) != 0;
                continue;
            }
            ser = s->color_serial;
        } else {
            const eng_zdepth* z = &g.zdepths[i - g.n_surfaces];
            if (dk == ENG_INVALID || !z->handle || z->location != d->location ||
                z->depth_serial <= base)
                continue;
            const u32 zrows = z->used_h ? z->used_h : z->h;
            if (z->pitch != d->pitch && zrows / (z->aa_sy ? z->aa_sy : 1u) != 1u &&
                d->h / d->aa_sy != 1u)
                continue;
            if (!accept && !eng_zdepth_tag_test(z)) {
                eng_isect r;
                const u32 zw = z->used_w ? z->used_w : z->w;
                n_stale += eng_intersect(z->offset, zw * eng_zeta_bpp(z->zfmt), zrows, d->offset,
                                         d->w * dbpp, d->h, d->pitch, &r) != 0;
                continue;
            }
            ser = z->depth_serial;
        }
        const u32 e = i < g.n_surfaces ? i : ENG_MAX_SURFACES + (i - g.n_surfaces);
        u32 k = n++;
        while (k) {
            const u32 p = order[k - 1];
            const u64 ps = p < ENG_MAX_SURFACES ? g.surfaces[p].color_serial
                                                : g.zdepths[p - ENG_MAX_SURFACES].depth_serial;
            if (ps <= ser) break;
            order[k] = p;
            k--;
        }
        order[k] = e;
    }
    for (u32 k = 0; k < n; k++) {
        eng_isect r;
        if (order[k] >= ENG_MAX_SURFACES) {
            const eng_zdepth* z = &g.zdepths[order[k] - ENG_MAX_SURFACES];
            const u32 sk = eng_zeta_tkind(eng_zeta_zf(z), z->depth_float);
            const u32 sbpp = eng_tkind_bpp(sk);
            const u32 zw = z->used_w ? z->used_w : z->w, zh = z->used_h ? z->used_h : z->h;
            if (!eng_intersect(z->offset, zw * sbpp, zh, d->offset, d->w * dbpp, d->h,
                               d->pitch, &r) ||
                !eng_typeless_isect(z->handle, sk, d->handle, dk, &r))
                continue;
            d->color_serial = z->depth_serial;
            if (z->guest_dirty) d->guest_dirty = 1;
            if (d->region) { g.be->texture_release(g.be->user, d->region); d->region = 0; }
            eng_zdepth_superseded(&g.zdepths[order[k] - ENG_MAX_SURFACES], &r, sbpp);
            continue;
        }
        const eng_surface* s = &g.surfaces[order[k]];
        if (!eng_color_same_layout(s, d, dk)) {
            const u32 sk = eng_color_tkind(s);
            if (!eng_intersect(s->offset, s->w * eng_tkind_bpp(sk), s->h, d->offset,
                               d->w * dbpp, d->h, d->pitch, &r) ||
                !eng_typeless_isect(s->handle, sk, d->handle, dk, &r))
                continue;
            d->color_serial = s->color_serial;
            if (s->guest_dirty) d->guest_dirty = 1;
            if (d->region) { g.be->texture_release(g.be->user, d->region); d->region = 0; }
            eng_surface_superseded(order[k], &r, eng_tkind_bpp(sk));
            continue;
        }
        const u32 sbpp = eng_color_isect_bpp(s, dk == ENG_INVALID ? ENG_INVALID : eng_color_tkind(s));
        if (sbpp && s->offset == d->offset && s->pitch == d->pitch &&
            (s->aa_sx != d->aa_sx || s->aa_sy != d->aa_sy) &&
            eng_tile_aa_at(d->location, d->offset, d->pitch)) {
            /* Compressed AA tile: pixel for pixel across the sample grids, a
             * resolve (box average, the linear filter at 2:1) into fewer
             * samples, a replicate (point) into more. */
            u32 pw = s->w / s->aa_sx, ph = s->h / s->aa_sy;
            if (d->w / d->aa_sx < pw) pw = d->w / d->aa_sx;
            if (d->h / d->aa_sy < ph) ph = d->h / d->aa_sy;
            if (!pw || !ph) continue;
            const int down = s->aa_sx * s->aa_sy > d->aa_sx * d->aa_sy;
            if (g.be->color_target_blit_filtered)
                g.be->color_target_blit_filtered(g.be->user, s->handle, 0, 0, pw * s->aa_sx,
                                                 ph * s->aa_sy, d->handle, 0, 0, pw * d->aa_sx,
                                                 ph * d->aa_sy, down);
            else
                g.be->color_target_blit(g.be->user, s->handle, 0, 0, pw * s->aa_sx,
                                        ph * s->aa_sy, d->handle, 0, 0, pw * d->aa_sx,
                                        ph * d->aa_sy);
            d->color_serial = s->color_serial;
            if (s->guest_dirty) d->guest_dirty = 1;
            if (d->region) { g.be->texture_release(g.be->user, d->region); d->region = 0; }
            continue;
        }
        if (!sbpp || !eng_intersect(s->offset, s->w * sbpp, s->h, d->offset, d->w * dbpp, d->h,
                                    d->pitch, &r))
            continue;
        const u32 sw = r.wb / sbpp, dw = r.wb / dbpp;
        if (!sw || !dw) continue;
        if (g.be->color_target_blit_filtered)
            g.be->color_target_blit_filtered(g.be->user, s->handle, r.sxb / sbpp, r.sy, sw, r.rows,
                                             d->handle, r.dxb / dbpp, r.dy, dw, r.rows, 0);
        else
            g.be->color_target_blit(g.be->user, s->handle, r.sxb / sbpp, r.sy, sw, r.rows,
                                    d->handle, r.dxb / dbpp, r.dy, dw, r.rows);
        d->color_serial = s->color_serial;
        if (s->guest_dirty) d->guest_dirty = 1;
        if (d->region) { g.be->texture_release(g.be->user, d->region); d->region = 0; }
        eng_surface_superseded(order[k], &r, sbpp);
    }
    if (n_stale && base && !accept && d->color_serial == base && eng_stale_erase()) {
        static const float erase[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g.be->clear_color(g.be->user, d->handle, erase);
        if (d->region) { g.be->texture_release(g.be->user, d->region); d->region = 0; }
        if (d->reint) { g.be->texture_release(g.be->user, d->reint); d->reint = 0; }
        d->guest_dirty = 0;
        eng_wcb_unguard(dst);
        d->color_serial = ++g.write_serial;
        eng_surface_tag(dst);
    }
}

/* RPCS3's surface store is unbounded (surface_store.h keeps every target in
 * its std::unordered_map and only spills under memory pressure). A full table
 * here evicts its least recently written colour target instead of dropping
 * the draw: one without unflushed rendering first, else the oldest, written
 * back to guest memory beforehand. s_eng_rt_keep holds the attachments of the
 * target set being bound. */
static int eng_surface_to_guest(u32 slot);
static u32 eng_surface_evict(void)
{
    u32 best = ENG_INVALID;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* s = &g.surfaces[i];
        if (!s->handle || eng_surface_pinned(i)) continue;
        if (best == ENG_INVALID) { best = i; continue; }
        const eng_surface* b = &g.surfaces[best];
        if ((!s->guest_dirty && b->guest_dirty) ||
            (!s->guest_dirty == !b->guest_dirty && s->color_serial < b->color_serial))
            best = i;
    }
    if (best == ENG_INVALID) return ENG_INVALID;
    if (g.surfaces[best].guest_dirty) eng_surface_to_guest(best);
    eng_surface_free(best);
    return best;
}

/* A free colour slot (an invalidated one, else the next), or ENG_INVALID. */
/* RPCS3 bind_surface_address (surface_store.h:600-618): binding a colour at
 * an address removes the zeta there from the depth store (and vice versa),
 * so it can never be written back over newer memory. Retire the other kind
 * when it is older than this kind's own surface at that address.
 * RSX_ENG_ALIAS_RETIRE=0 disables. */
static void eng_alias_retire(u32 location, u32 offset, int binding_z)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_ENG_ALIAS_RETIRE"); on = !(e && *e == '0'); }
    if (!on || location >= ENG_LOC_SCRATCH) return;
    u64 mine = 0;
    if (binding_z) {
        for (u32 i = 0; i < g.n_zdepths; i++)
            if (g.zdepths[i].handle && g.zdepths[i].location == location &&
                g.zdepths[i].offset == offset && g.zdepths[i].depth_serial > mine)
                mine = g.zdepths[i].depth_serial;
        if (!mine) return;
        for (u32 i = 0; i < g.n_surfaces; i++) {
            eng_surface* s = &g.surfaces[i];
            if (s->handle && s->guest_dirty && s->location == location && s->offset == offset &&
                s->color_serial < mine)
                s->guest_dirty = 0;
        }
    } else {
        for (u32 i = 0; i < g.n_surfaces; i++)
            if (g.surfaces[i].handle && g.surfaces[i].location == location &&
                g.surfaces[i].offset == offset && g.surfaces[i].color_serial > mine)
                mine = g.surfaces[i].color_serial;
        if (!mine) return;
        for (u32 i = 0; i < g.n_zdepths; i++) {
            eng_zdepth* z = &g.zdepths[i];
            if (z->handle && z->guest_dirty && z->location == location && z->offset == offset &&
                z->depth_serial < mine)
                z->guest_dirty = 0;
        }
    }
}

static u32 eng_surface_slot_alloc(void)
{
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (!g.surfaces[i].handle) return i;
    if (g.n_surfaces < ENG_MAX_SURFACES) return g.n_surfaces++;
    return ENG_INVALID;
}

/* One insert_new_surface of split_surface_region: rectangle (sx, sy, w, h)
 * of surface `src` (samples) as a clone at guest `offset`, unless a surface
 * there is newer. A compatible older one there takes the rectangle; any
 * other older one is invalidated. */
static void eng_surface_split_insert(u32 src, u32 offset, u32 sx, u32 sy, u32 w, u32 h)
{
    const eng_surface* p = &g.surfaces[src];
    if (sx >= p->w || sy >= p->h || !w || !h) return;
    if (w > p->w - sx) w = p->w - sx;
    if (h > p->h - sy) h = p->h - sy;
    u32 dst = ENG_INVALID;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        eng_surface* f = &g.surfaces[i];
        if (i == src || !f->handle || f->location != p->location || f->offset != offset) continue;
        const int compatible = f->fmt == p->fmt && f->cfmt == p->cfmt && f->aa_sx == p->aa_sx &&
                               f->aa_sy == p->aa_sy && f->pitch == p->pitch && !f->swz &&
                               f->w >= w && f->h >= h;
        if (compatible) {
            if (f->color_serial >= p->color_serial) return;   /* newer memory */
            g.be->color_target_blit(g.be->user, p->handle, sx, sy, w, h, f->handle, 0, 0, w, h);
            if (f->region) { g.be->texture_release(g.be->user, f->region); f->region = 0; }
            f->color_serial = p->color_serial;
            if (p->guest_dirty) f->guest_dirty = 1;
            return;
        }
        const int pitch_ok = !f->pitch || f->pitch == p->pitch;
        if (f->color_serial >= p->color_serial) {
            /* A newer owner: kept (pitch incompatible) or merged over the
             * clone, which newest-wins reads already give. */
            if (!pitch_ok) return;
            continue;
        }
        /* A bound attachment, blit source or presented surface stays: callers
         * hold its slot index (newest-wins reads still prefer the clone). */
        if (eng_surface_pinned(i)) continue;
        eng_surface_free(i);
        if (dst == ENG_INVALID) dst = i;
    }
    if (dst == ENG_INVALID) dst = eng_surface_slot_alloc();
    if (dst == ENG_INVALID) return;
    p = &g.surfaces[src];
    const u32 handle = g.be->color_target_create(g.be->user, eng_hfmt(p), w, h, NULL, 0);
    if (!handle) return;
    g.be->color_target_blit(g.be->user, p->handle, sx, sy, w, h, handle, 0, 0, w, h);
    eng_surface* c = &g.surfaces[dst];
    memset(c, 0, sizeof *c);
    c->location = p->location; c->offset = offset;
    c->w = w; c->h = h; c->fmt = p->fmt; c->hfmt = p->hfmt; c->cfmt = p->cfmt;
    c->pitch = p->pitch;
    c->aa_sx = p->aa_sx; c->aa_sy = p->aa_sy;
    c->handle = handle;
    c->aa_flags = (g.be->target_set_aa && (c->aa_sx > 1u || c->aa_sy > 1u))
                ? g.be->target_set_aa(g.be->user, handle, c->aa_sx, c->aa_sy) : 0u;
    c->color_serial = p->color_serial;           /* on_clone_from: last_use_tag */
    c->guest_dirty = p->guest_dirty;
    c->storage = 1;
    eng_surface_tag(dst);                         /* clone_surface queue_tag */
}

/* RPCS3 surface_store::split_surface_region for colour surface `slot`, about
 * to be replaced at its address by one `new_wb` bytes x `new_rows` sample
 * rows: the memory it covers beyond that (to the right, then below) lives on
 * in clones, if its tag still holds. */
static void eng_surface_split(u32 slot, u32 new_wb, u32 new_rows)
{
    const eng_surface* p = &g.surfaces[slot];
    if (!eng_split_surfaces() || !p->handle || !p->pitch || p->swz || !p->color_serial ||
        p->location >= ENG_LOC_SCRATCH || !g.be->color_target_blit)
        return;
    const u32 tb = eng_color_bytes(p) * p->aa_sx;   /* bytes per pixel column */
    const u32 old_wb = p->w * eng_color_bytes(p), old_rows = p->h;
    if (old_wb <= new_wb && old_rows <= new_rows) return;
    if (!eng_surface_tag_test(p)) return;
    const u32 offset = p->offset, pitch = p->pitch, ax = p->aa_sx, ay = p->aa_sy;
    if (old_wb > new_wb) {
        u32 cw = (old_wb - new_wb) / tb;
        if (!cw) cw = 1;
        eng_surface_split_insert(slot, offset + new_wb, (new_wb / tb) * ax, 0, cw * ax,
                                 g.surfaces[slot].h);
    }
    if (old_rows > new_rows) {
        u32 cw = (new_wb < old_wb ? new_wb : old_wb) / tb, ch = (old_rows - new_rows) / ay;
        if (!cw) cw = 1;
        if (!ch) ch = 1;
        eng_surface_split_insert(slot, offset + new_rows * pitch, 0, (new_rows / ay) * ay,
                                 cw * ax, ch * ay);
    }
}

/* RPCS3's store holds one surface per address: an antialias change there
 * makes the old surface old_contents and drops it. Once `slot` holds the
 * newest bytes of a same-address sibling of another AA mode (through
 * eng_surface_intersect) and covers all of its memory, the sibling goes. */
static void eng_surface_retire_aa_siblings(u32 slot)
{
    if (eng_aa_resolve_inherit() || !eng_split_surfaces()) return;
    const eng_surface* d = &g.surfaces[slot];
    if (!d->handle || !d->pitch || d->location >= ENG_LOC_SCRATCH) return;
    for (u32 k = 0; k < g.n_surfaces; k++) {
        eng_surface* o = &g.surfaces[k];
        if (k == slot || !o->handle || o->location != d->location || o->offset != d->offset ||
            (o->aa_sx == d->aa_sx && o->aa_sy == d->aa_sy) || eng_surface_pinned(k))
            continue;
        if (o->pitch != d->pitch || o->h > d->h ||
            o->w * eng_color_bytes(o) > d->w * eng_color_bytes(d))
            continue;
        if (o->guest_dirty && (!d->color_serial || d->color_serial < o->color_serial)) continue;
        eng_surface_free(k);
    }
}

/* RPCS3 VKGSRender.cpp:43-60 get_compatible_surface_format (non-Apple):
 * r5g6b5 renders into R5G6B5_UNORM_PACK16, x1r5g5b5 (_Z and _O) into
 * A1R5G5B5_UNORM_PACK16, b8 into R8_UNORM and g8b8 into R8G8_UNORM, so blending
 * and the stored values are quantised as the guest's are. The surface stays
 * logically RGBA8 (s->fmt): the backend reads, writes, seeds and samples it in
 * the RGBA8 layout (B8 in .b, G8B8 in .g/.b), and the shader's ROP remap
 * (.bbbb / .bgbg) lands B (and G) in .r (.g). A backend without
 * color_format_supported, or not supporting the format, keeps RGBA8.
 * RSX_ENG_NATIVE_16BIT=0: every 8-bit format on RGBA8, as before. */
static rsx_be_format eng_host_color_format(rsx_be_format fmt, u32 cfmt, u32 location)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_ENG_NATIVE_16BIT"); en = !(e && e[0] == '0'); }
    if (!en || fmt != RSX_BE_FMT_R8G8B8A8 || !g.be->color_format_supported ||
        eng_legacy_surfaces() || location >= ENG_LOC_SCRATCH)
        return fmt;
    rsx_be_format h;
    switch (cfmt) {
    case 0x3u:            h = RSX_BE_FMT_R5G6B5;   break;
    case 0x1u: case 0x2u: h = RSX_BE_FMT_A1R5G5B5; break;
    case 0x9u:            h = RSX_BE_FMT_R8;       break;
    case 0xAu:            h = RSX_BE_FMT_R8G8;     break;
    default:              return fmt;
    }
    return g.be->color_format_supported(g.be->user, h) ? h : fmt;
}

/* The pipeline's rt_fmt: the bound target's host format (RPCS3 hashes the
 * surface's VkFormat into the pipeline key, VKGSRender.cpp:43-60). */
static rsx_be_format eng_draw_rt_format(u32 target, u32 color_format)
{
    const rsx_be_format f = eng_surface_format(color_format);
    if (target < g.n_surfaces && g.surfaces[target].handle && g.surfaces[target].fmt == f)
        return eng_hfmt(&g.surfaces[target]);
    return f;
}

/* Blit-destination creation hints for eng_surface_get (outside a bind): the
 * guest colour format of a new surface, and RPCS3's force_data_load
 * (texture_cache.h:1813-1823) -- guest memory loaded into a new target before
 * the inherited sources are copied over it. */
static u32 s_eng_new_cfmt;
/* Host GPU is NVIDIA (rsx_draw_engine_set_host_nvidia; default 1). */
static int s_eng_host_nv = 1;
void rsx_draw_engine_set_host_nvidia(int is_nvidia) { s_eng_host_nv = is_nvidia != 0; }
static int s_eng_force_load;
static void eng_surface_upload_rows(u32 slot, u32 location, u32 offset, u32 pitch, u32 w, u32 h);

static u32 eng_surface_get(u32 location, u32 offset, u32 want_w, u32 want_h,
                          rsx_be_format want_fmt, u32 pitch, u32 aa_sx, u32 aa_sy)
{
    if (!want_w) want_w = g.width;
    if (!want_h) want_h = g.height;
    if (!aa_sx) aa_sx = 1;
    if (!aa_sy) aa_sy = 1;
    eng_alias_retire(location, offset, 0);
    const u32 new_cfmt = s_eng_bind_cfmt ? s_eng_bind_cfmt : s_eng_new_cfmt;
    rsx_be_format want_hfmt = new_cfmt
        ? eng_host_color_format(want_fmt, new_cfmt, location) : want_fmt;

    /* A surface per antialias mode at an address while both hold live bytes:
     * R2 renders its scene 2x AA into 0x880000, then binds the same address
     * without AA for the composites and the upscale, which read the resolved
     * image. RPCS3 keeps one per address; eng_surface_retire_aa_siblings
     * drops a sibling once the bound surface holds all of its bytes. */
    u32 slot = ENG_MAX_SURFACES, sibling = ENG_INVALID;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* s = &g.surfaces[i];
        if (!s->handle || s->location != location || s->offset != offset) continue;
        if (s->aa_sx != aa_sx || s->aa_sy != aa_sy) {
            if (s->color_serial &&
                (sibling == ENG_INVALID ||
                 s->color_serial > g.surfaces[sibling].color_serial))
                sibling = i;
            continue;
        }
        if (slot != ENG_MAX_SURFACES) continue;
        /* RPCS3 is_compatible_surface compares the guest colour format: one
         * of another byte layout (ARGB8 -> RGB565, B8, ...) on the same host
         * format is a new surface, old_contents the typeless copy. */
        int layout_ok = 1;
        if (s_eng_bind_cfmt && s->cfmt && s->cfmt != s_eng_bind_cfmt && eng_typeless_inherit() &&
            !eng_legacy_surfaces()) {
            eng_surface nt = *s;
            nt.fmt = want_fmt;
            nt.cfmt = s_eng_bind_cfmt;
            layout_ok = eng_color_same_layout(s, &nt, eng_color_tkind(&nt));
        }
        /* RPCS3 matches_dimensions: a bound render target matches only at
         * its exact size; any other is replaced (old contents inherited, the
         * remainder split off). Other lookups (blit destinations) may reuse a
         * larger same-pitch surface. */
        const int exact_only = s_eng_bind_cfmt && !eng_legacy_surfaces() &&
                               location < ENG_LOC_SCRATCH;
        if (s->fmt == want_fmt && layout_ok && (!s_eng_bind_cfmt || eng_hfmt(s) == want_hfmt) &&
            ((s->w == want_w && s->h == want_h) ||
             (!exact_only && pitch && s->pitch == pitch && s->w >= want_w && s->h >= want_h))) {
            if (sibling == ENG_INVALID)
                for (u32 k = i + 1; k < g.n_surfaces; k++) {
                    const eng_surface* o = &g.surfaces[k];
                    if (o->handle && o->location == location && o->offset == offset &&
                        (o->aa_sx != aa_sx || o->aa_sy != aa_sy) && o->color_serial &&
                        (sibling == ENG_INVALID ||
                         o->color_serial > g.surfaces[sibling].color_serial))
                        sibling = k;
                }
            g.surfaces[i].storage = 0;            /* bound: an attachment again */
            if (eng_aa_resolve_inherit()) {
                eng_surface_inherit(i, sibling);
            } else {
                eng_surface* m = &g.surfaces[i];
                if (s_eng_bind_cfmt) {
                    /* RPCS3 bind_surface_address, pitch incompatible but the
                     * properties match: convert_pitch (unimplemented) sets
                     * erase_bkgnd and invalidate_surface_contents drops the
                     * pixels -- memory_barrier clear_memory()s them to
                     * (0,0,0,1) (VKRenderTargets.cpp:510) -- while
                     * new_content_tag keeps the surface's age for the
                     * intersection that follows. */
                    if (pitch && m->pitch && m->pitch != pitch && eng_pitch_discard() &&
                        location < ENG_LOC_SCRATCH) {
                        static const float zero[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                        g.be->clear_color(g.be->user, m->handle, zero);
                        m->guest_dirty = 0;
                        if (m->region) { g.be->texture_release(g.be->user, m->region); m->region = 0; }
                        if (m->feedback) { g.be->texture_release(g.be->user, m->feedback); m->feedback = 0; }
                        if (m->reint) { g.be->texture_release(g.be->user, m->reint); m->reint = 0; }
                    }
                    m->cfmt = s_eng_bind_cfmt;
                }
                const int repitched = pitch && m->pitch != pitch;
                if (pitch) m->pitch = pitch;
                {
                    const u64 before = m->color_serial;
                    eng_surface_intersect(i);
                    if (m->color_serial != before || repitched) eng_surface_tag(i);
                }
                eng_surface_retire_aa_siblings(i);
            }
            return i;
        }
        slot = i;
    }

    /* Never destroy a usable render target because one malformed command
     * decoded a guest pointer as clip dimensions. */
    if (want_w > ENG_MAX_SURFACE_DIM || want_h > ENG_MAX_SURFACE_DIM) {
        static u32 logs = 0;
        if (PS3_LOG_FIRST(logs, 8))
            fprintf(stderr, "[rsx engine] rejected implausible surface 0x%X %ux%u;"
                            " keeping the %s target\n", offset, want_w, want_h,
                    slot < ENG_MAX_SURFACES ? "existing" : "absent");
        return slot < ENG_MAX_SURFACES ? slot : ENG_INVALID;
    }
    if (slot == ENG_MAX_SURFACES) {
        slot = g.n_surfaces;
        for (u32 i = 0; i < g.n_surfaces; i++)
            if (!g.surfaces[i].handle) { slot = i; break; }
        if (slot >= ENG_MAX_SURFACES && !s_texcache_legacy) {
            const u64 keep = s_eng_rt_keep;
            if (sibling != ENG_INVALID) s_eng_rt_keep |= 1ull << sibling;
            slot = eng_surface_evict();
            s_eng_rt_keep = keep;
        }
        if (slot >= ENG_MAX_SURFACES) return ENG_INVALID;
    }

    /* RPCS3 (R2 patch, surface_store.h): a same-format replacement at the
     * address, e.g. a resize, inherits the old target's rendered pixels
     * instead of reseeding from guest memory, which may be stale. Only the
     * same guest layout is a value copy; any other is the typeless copy. */
    /* A lookup outside a bind (blit destination) keeps the host format of the
     * surface it replaces. */
    if (!s_eng_bind_cfmt && slot < g.n_surfaces && g.surfaces[slot].handle &&
        g.surfaces[slot].fmt == want_fmt)
        want_hfmt = eng_hfmt(&g.surfaces[slot]);
    u32 c_sk = ENG_INVALID, c_dk = ENG_INVALID;
    int c_same = 1;
    if (slot < g.n_surfaces && g.surfaces[slot].handle && eng_typeless_inherit()) {
        eng_surface nt = g.surfaces[slot];
        nt.fmt = want_fmt;
        if (s_eng_bind_cfmt) nt.cfmt = s_eng_bind_cfmt;
        c_dk = eng_color_tkind(&nt);
        c_sk = eng_color_tkind(&g.surfaces[slot]);
        c_same = eng_color_same_layout(&g.surfaces[slot], &nt, c_dk);
    }
    const int carry = !eng_legacy_surfaces() && sibling == ENG_INVALID &&
                      slot < g.n_surfaces && g.surfaces[slot].handle &&
                      g.surfaces[slot].fmt == want_fmt && eng_hfmt(&g.surfaces[slot]) == want_hfmt && c_same &&
                      (eng_aa_resolve_inherit() || !pitch || !g.surfaces[slot].pitch ||
                       g.surfaces[slot].pitch == pitch) &&
                      g.surfaces[slot].color_serial && g.be->color_target_blit &&
                      eng_surface_tag_test(&g.surfaces[slot]);
    u32 seed_row = 0;
    const void* seed = (sibling == ENG_INVALID && !carry && eng_seed_guest())
        ? eng_surface_seed(location, offset, want_w, want_h, want_fmt, &seed_row)
        : NULL;
    const u32 handle = g.be->color_target_create(g.be->user, want_hfmt,
                                                 want_w, want_h, seed, seed_row);
    if (!handle)
        return (slot < g.n_surfaces && g.surfaces[slot].handle) ? slot : ENG_INVALID;

    eng_surface* s = &g.surfaces[slot];
    const u64 prev_serial = (slot < g.n_surfaces && s->handle) ? s->color_serial : 0u;
    const u32 prev_pitch = (slot < g.n_surfaces && s->handle) ? s->pitch : 0u;
    u64 carried_serial = 0;
    u8 carried_dirty = 0;
    if (carry) {
        const u32 cw = s->w < want_w ? s->w : want_w, ch = s->h < want_h ? s->h : want_h;
        if (cw && ch) {
            g.be->color_target_blit(g.be->user, s->handle, 0, 0, cw, ch, handle, 0, 0, cw, ch);
            carried_serial = s->color_serial;
            carried_dirty = (u8)(s->guest_dirty != 0);
        }
    } else if (prev_serial && sibling == ENG_INVALID && !c_same &&
               eng_typeless_inherit() && g.be->surface_typeless_copy &&
               (!pitch || !prev_pitch || prev_pitch == pitch) && eng_surface_tag_test(s)) {
        /* RPCS3 old_surface of another colour layout (ARGB8 <-> X32, RGB565,
         * B8, FP16...): the typeless copy of the bytes the two share, x
         * scaled by src_bpp / dst_bpp. */
        const u32 sb = c_sk != ENG_INVALID ? eng_tkind_bpp(c_sk) : 0u;
        const u32 db = c_dk != ENG_INVALID ? eng_tkind_bpp(c_dk) : 0u;
        u32 wb = (sb && db) ? (s->w * sb < want_w * db ? s->w * sb : want_w * db) : 0u;
        if (wb) wb -= wb % (sb > db ? sb : db);
        const u32 ch = s->h < want_h ? s->h : want_h;
        if (wb && ch &&
            g.be->surface_typeless_copy(g.be->user, s->handle, c_sk, 0, 0, wb / sb, ch,
                                        handle, c_dk, 0, 0, wb / db, ch)) {
            carried_serial = s->color_serial;
            carried_dirty = (u8)(s->guest_dirty != 0);
        }
    }
    /* bind_surface_address, pitch compatible but other properties: the
     * old surface's memory outside the new one survives as clones. */
    if (s->handle && sibling == ENG_INVALID && s->location == location && s->offset == offset &&
        (!pitch || !prev_pitch || prev_pitch == pitch)) {
        eng_surface nt = *s;
        nt.fmt = want_fmt;
        if (s_eng_bind_cfmt) nt.cfmt = s_eng_bind_cfmt;
        eng_surface_split(slot, want_w * eng_color_bytes(&nt), want_h);
        s = &g.surfaces[slot];
    }
    if (s->feedback) { g.be->texture_release(g.be->user, s->feedback); s->feedback = 0; }
    if (s->region) { g.be->texture_release(g.be->user, s->region); s->region = 0; }
    if (s->reint) { g.be->texture_release(g.be->user, s->reint); s->reint = 0; }
    if (s->handle) g.be->color_target_release(g.be->user, s->handle);
    eng_wcb_unguard(slot);
    s->storage = 0;
    s->location = location; s->offset = offset;
    s->w = want_w; s->h = want_h; s->fmt = want_fmt; s->hfmt = want_hfmt;
    s->aa_sx = aa_sx; s->aa_sy = aa_sy;
    s->handle = handle;
    s->swz = 0;
    s->aa_flags = (g.be->target_set_aa && (aa_sx > 1u || aa_sy > 1u))
                ? g.be->target_set_aa(g.be->user, handle, aa_sx, aa_sy) : 0u;
    s->color_serial = carried_serial;
    s->guest_dirty = carried_dirty;
    if (new_cfmt) s->cfmt = new_cfmt;
    /* RPCS3 new_content_tag: a replacement after a pitch change starts with
     * none of the old pixels but the old surface's age. */
    if (!carry && s_eng_bind_cfmt && eng_pitch_discard() && prev_serial && prev_pitch &&
        pitch && prev_pitch != pitch && location < ENG_LOC_SCRATCH)
        s->color_serial = prev_serial;
    if (slot == g.n_surfaces) g.n_surfaces++;
    { static u32 logs = 0; if (PS3_LOG_FIRST(logs, 16))
        fprintf(stderr, "[rsx engine] surface %u:0x%08X %ux%u fmt %d aa %ux%u%s\n",
                location, offset, want_w, want_h, (int)want_fmt, aa_sx, aa_sy,
                seed ? " (seeded from guest memory)" : ""); }
    if (eng_aa_resolve_inherit()) {
        eng_surface_inherit(slot, sibling);
    } else {
        if (pitch) s->pitch = pitch;
        s->tag_ok = 0;
        if (s_eng_force_load && !carry && sibling == ENG_INVALID && !seed)
            eng_surface_upload_rows(slot, location, offset, pitch, want_w, want_h);
        eng_surface_intersect(slot);
        if (s->color_serial) eng_surface_tag(slot);
        eng_surface_retire_aa_siblings(slot);
    }
    return slot;
}

/* RPCS3 binds only the colour buffers get_framebuffer_layout keeps; a dropped
 * one is not a surface at all, so it must not create, resize or reseed the
 * target living at its guest address. The host pass still needs an
 * attachment in that position (and at least one), so a dropped buffer binds a
 * scratch target in location ENG_LOC_SCRATCH, which no guest lookup, seed or
 * write-back ever matches and which is never marked written. */
#define ENG_LOC_SCRATCH 2u
/* The present path's read-from-cell copy of a display buffer (offset = the
 * buffer id), RPCS3's upload_image_simple texture: never a guest surface. */
#define ENG_LOC_PRESENT 3u
static u32 eng_scratch_target(u32 pos, u32 w, u32 h, rsx_be_format fmt, u32 aa_sx, u32 aa_sy)
{
    return eng_surface_get(ENG_LOC_SCRATCH, (pos << 28) | ((w & 0x3FFFu) << 14) | (h & 0x3FFFu),
                           w, h, fmt, 0, aa_sx, aa_sy);
}

/* The colour targets SET_SURFACE_COLOR_TARGET names, as slots into
 * g.surfaces, with A first. One for an ordinary draw; an MRT set adds B, C
 * and D, each created at target A's size and format, which is the rule the
 * vtable path's target_for binds by. Resolution stops at the first member
 * that cannot be created, as that path stops at the first gap. Returns how
 * many were resolved, or 0.
 * `fbl` (may be NULL): the layout of the operation; buffers it drops bind
 * scratch targets, and its pitch-corrected width narrows the surfaces. */
struct eng_fb_layout;
static u32 eng_current_target_set_ex(u32 slots[RSX_BE_MAX_COLOR_TARGETS],
                                     const struct eng_fb_layout* fbl);
static u32 eng_current_target_set(u32 slots[RSX_BE_MAX_COLOR_TARGETS])
{
    return eng_current_target_set_ex(slots, NULL);
}

/* ---- coherence with guest memory around 2D transfers ------------------- */

/* Bytes per pixel of a surface a 2D transfer can touch: A8R8G8B8, and the
 * FP16 RGBA target an HDR pass renders into (Twisted Metal copies its bloom
 * out of one for the SPU post-FX). 0 for any other format. */
static u32 eng_transfer_bpp(rsx_be_format fmt)
{
    switch (fmt) {
    case RSX_BE_FMT_R8G8B8A8:      return 4u;
    case RSX_BE_FMT_R16G16B16A16F: return 8u;
    default:                       return 0u;
    }
}

/* The surface holding guest bytes [offset, offset + pitch * h), with the byte
 * position of `offset` inside its row (*xb) and its row (*y). Only a surface
 * laid out with the transfer's own pitch qualifies: another pitch puts the
 * rows elsewhere. The CLOSEST base at or below the address wins: surfaces'
 * guest ranges overlap, and an FP16 target right after an RGBA8 one must not
 * resolve into the RGBA8 one's rows (rsx_live_draw.c's resolve takes the same
 * rule, and its comment records copying out of a stale surface at 0x0 before
 * it did). */
static u32 eng_surface_at(u32 location, u32 offset, u32 pitch, u32* xb, u32* y)
{
    u32 best = ENG_INVALID;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* s = &g.surfaces[i];
        if (!s->handle || s->location != location || offset < s->offset) continue;
        const u32 bpp = eng_transfer_bpp(s->fmt);
        if (!bpp || s->pitch != pitch) continue;
        const u32 d = offset - s->offset;
        if (d / pitch >= s->h || d % pitch >= s->w * bpp) continue;
        if (best != ENG_INVALID && !eng_surface_at_closest()) {
            /* RPCS3 get_merged_texture_memory_region / the blit engine's
             * lookup walk overlapping surfaces newest first: the last
             * written bytes win, the closest base only breaks a tie. */
            const u64 bs = g.surfaces[best].color_serial;
            if (s->color_serial < bs) continue;
            if (s->color_serial == bs && g.surfaces[best].offset >= s->offset) continue;
        } else {
        if (best != ENG_INVALID && g.surfaces[best].offset > s->offset) continue;
        if (best != ENG_INVALID && g.surfaces[best].offset == s->offset &&
            g.surfaces[best].color_serial > s->color_serial) continue;
        }
        best = i;
        *xb = d % pitch;
        *y = d / pitch;
    }
    return best;
}

static u8* s_coherence_rows;
static u32 s_coherence_cap;

/* RSX_ENGINE_COHERENCE_LOG=<n>: log the first n transfers the engine syncs,
 * with how many non-zero pixels moved -- the question when a composite comes
 * out black is whether the GPU surface had anything in it. */
static int eng_coherence_log(void)
{
    static int left = -1;
    if (left < 0) { const char* e = getenv("RSX_ENGINE_COHERENCE_LOG"); left = e ? atoi(e) : 0; }
    return left > 0 ? left-- : 0;
}

/* Pixels of `bpp` bytes whose colour (the first three quarters: R,G,B) is
 * not all zero. */
static u32 eng_nonzero_pixels(const u8* rows, u32 n, u32 bpp)
{
    u32 k = 0;
    for (u32 i = 0; i < n; i++, rows += bpp) {
        u8 any = 0;
        for (u32 b = 0; b < bpp * 3u / 4u; b++) any |= rows[b];
        k += any != 0;
    }
    return k;
}

static u8* eng_coherence_buffer(u32 bytes)
{
    if (s_coherence_cap < bytes) {
        u8* n = (u8*)realloc(s_coherence_rows, bytes);
        if (!n) return NULL;
        s_coherence_rows = n;
        s_coherence_cap = bytes;
    }
    return s_coherence_rows;
}

/* One row of 32-bit pixels between the backend's R,G,B,A and the guest's
 * A8R8G8B8 bytes (or straight through with TEX_RGBA). Copied a byte at a
 * time, with the TEX_RGBA test inside the loop, these two conversions were
 * about a quarter of the FIFO thread's time while Twisted Metal booted; at
 * the -O2 the runtime builds with, GCC vectorises neither form. Each pixel is
 * one 32-bit rotate instead: bytes s0..s3 load as s0 | s1<<8 | s2<<16 | s3<<24
 * on a little-endian host (ps3emu/endian.h), so rotating left by 8 gives
 * s3,s0,s1,s2 (to the guest) and right by 8 gives s1,s2,s3,s0 (from it). */
static void eng_row_convert(u8* dst, const u8* src, u32 w, int rgba, int to_guest)
{
    if (rgba) { memcpy(dst, src, (size_t)w * 4u); return; }
    for (u32 c = 0; c < w; c++) {
        u32 v;
        memcpy(&v, src + (size_t)c * 4u, 4);
        v = to_guest ? (v << 8) | (v >> 24) : (v >> 8) | (v << 24);
        memcpy(dst + (size_t)c * 4u, &v, 4);
    }
}

/* NV309E (RPCS3 texture cache: a blit into a swizzled destination is a
 * cached section, flushed only when memory is needed). A surface marked swz
 * holds that destination's image linearly on the GPU; it lives only while
 * it is newer than guest memory. Storing it Morton-orders the pixels, as
 * nv3089 swizzled_copy_2 does, at the next power-of-two size. */
static int s_eng_swz_used;
#define ENG_SWZ_DEAD 0xFFFFFFFFu

static int eng_swz_to_guest(u32 slot)
{
    const eng_surface* s = &g.surfaces[slot];
    u32 lw = 0, lh = 0;
    while ((1u << lw) < s->w) lw++;
    while ((1u << lh) < s->h) lh++;
    u8* rows = eng_coherence_buffer(s->w * s->h * 4u);
    eng_tiled tt;
    const int tiled = eng_tiled_at(s->location, s->offset, &tt);
    const u32 span = (4u << lw) << lh;
    u8* dst = tiled ? (u8*)eng_guest_image(s->location, s->offset, span)
                    : (u8*)eng_guest_ptr(NULL, s->location, s->offset, span);
    if (!rows || !dst || !g.be->readback) return 0;
    u8* const gdst = dst;
    if (!tiled) dst = eng_store_ptr(dst);
    g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    g.be->readback(g.be->user, s->handle, 0, 0, s->w, s->h, rows, s->w * 4u);
    const int rgba = rsx_texture_argb_is_rgba();
    for (u32 y = 0; y < s->h; y++) {
        u8* r = rows + (size_t)y * s->w * 4u;
        if (!rgba) eng_row_convert(r, r, s->w, 0, 1);
        for (u32 x = 0; x < s->w; x++)
            memcpy(dst + (size_t)rsx_swizzle_offset_3d(x, y, 0, lw, lh, 0) * 4u,
                   r + (size_t)x * 4u, 4);
    }
    if (tiled && !eng_guest_tiled_copy(s->location, s->offset, dst, span, 1)) return 0;
    if (!tiled) eng_store_done(gdst, span);
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] swizzled blit target %u:0x%08X %ux%u stored to guest memory\n",
                s->location, s->offset, s->w, s->h);
    return 1;
}

/* Guest bytes in [offset, +length) were overwritten: a swizzled-destination
 * surface there no longer stands for memory (RPCS3 invalidates the section).
 * Its slot stays allocated, parked at an address nothing names, for reuse. */
static void eng_swz_drop(u32 location, u32 offset, u64 length)
{
    if (!s_eng_swz_used || !length) return;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        eng_surface* s = &g.surfaces[i];
        if (!s->swz || !s->handle || s->location != location || s->offset == ENG_SWZ_DEAD)
            continue;
        const u64 span = (u64)s->w * s->h * 4u;
        if ((u64)offset >= (u64)s->offset + span || (u64)s->offset >= (u64)offset + length)
            continue;
        eng_wcb_unguard(i);
        s->guest_dirty = 0;
        s->offset = ENG_SWZ_DEAD;
    }
}

static int eng_surface_to_guest(u32 slot);

static void rsx_draw_engine_guest_read_impl(u32 location, u32 offset, u32 pitch, u32 w, u32 h)
{
    if (!g.ready || !g.be->readback || !pitch || !w || !h) return;
    u32 xb = 0, y = 0;
    const u32 slot = eng_surface_at(location, offset, pitch, &xb, &y);
    if (slot == ENG_INVALID) {
        if (eng_coherence_log())
            fprintf(stderr, "[rsx engine] guest_read %u:0x%08X %ux%u: no surface\n",
                    location, offset, w, h);
        return;
    }
    const eng_surface* s = &g.surfaces[slot];
    if (s->swz) { eng_surface_to_guest(slot); return; }
    if (eng_surface_packed_kind(s)) { eng_surface_to_guest(slot); return; }
    /* RPCS3 flushes only sections whose GPU copy is newer than memory: a
     * surface stored since its last write is already there (no stall). */
    if (!s->guest_dirty && !eng_legacy_surfaces()) return;
    const u32 bpp = eng_transfer_bpp(s->fmt);
    /* The transfer counts 32-bit pixels, so w * 4 bytes per row; an FP16
     * surface pixel is two of them. Read back every surface pixel those bytes
     * touch, then store exactly the bytes. */
    u32 bytes = w * 4u;
    if (xb + bytes > s->w * bpp) bytes = s->w * bpp - xb;
    if (y + h > s->h) h = s->h - y;
    const u32 x = xb / bpp, skip = xb % bpp;
    const u32 n = (skip + bytes + bpp - 1u) / bpp;
    u8* rows = eng_coherence_buffer(n * bpp * h);
    if (!rows) return;
    g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    g.be->readback(g.be->user, s->handle, x, y, n, h, rows, n * bpp);
    /* Without the sudo view the guard has to go before the bytes land. */
    if (!eng_sudo_on()) eng_wcb_unguard(slot);
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] guest_read %u:0x%08X %ux%u <- surface 0x%08X (%u bpp) at %u,%u:"
                        " %u non-zero pixels\n", location, offset, w, h, s->offset, bpp, x, y,
                eng_nonzero_pixels(rows, n * h, bpp));
    const int rgba = rsx_texture_argb_is_rgba();
    const u32 rpitch = eng_tiled_row_pitch(location, offset, pitch);
    for (u32 r = 0; r < h; r++) {
        u8* src = rows + (size_t)r * n * bpp;
        if (bpp == 8u) {
            /* R,G,B,A halves, each stored big-endian: the layout
             * rsx_texture_decode reads W16_Z16_Y16_X16_FLOAT from. */
            for (u32 c = 0; c < n * 4u; c++) {
                const u8 t = src[2 * c]; src[2 * c] = src[2 * c + 1]; src[2 * c + 1] = t;
            }
        } else if (!rgba) {
            eng_row_convert(src, src, n, 0, 1);   /* in place: one pixel per load/store */
        }
        if (!eng_guest_tiled_copy(location, offset + r * rpitch, src + skip, bytes, 1)) {
            eng_wcb_unguard(slot);
            return;
        }
    }
    /* Stored while still guarded (sudo view): only now may the guest in. */
    eng_wcb_unguard(slot);
    /* RPCS3 flush: sync_tag after the GPU copy reaches memory. */
    eng_surface_tag(slot);
}

void rsx_draw_engine_guest_read(u32 location, u32 offset, u32 pitch, u32 w, u32 h)
{
    ENG_SCOPE();
    const u64 t0 = eng_prof_t0();
    rsx_draw_engine_guest_read_impl(location, offset, pitch, w, h);
    eng_prof_add(4, t0);
}

void rsx_draw_engine_guest_wrote(u32 location, u32 offset, u32 pitch, u32 w, u32 h)
{
    ENG_SCOPE();
    if (!g.ready || !g.be->color_target_write || !pitch || !w || !h) return;
    eng_swz_drop(location, offset, (u64)pitch * (h - 1u) + (u64)w * 4u);
    u32 x = 0, y = 0;
    const u32 slot = eng_surface_at(location, offset, pitch, &x, &y);
    /* color_target_write takes R,G,B,A bytes: an A8R8G8B8 surface only. */
    if (slot == ENG_INVALID || g.surfaces[slot].fmt != RSX_BE_FMT_R8G8B8A8) {
        if (eng_coherence_log())
            fprintf(stderr, "[rsx engine] guest_wrote %u:0x%08X %ux%u: no A8R8G8B8 surface\n",
                    location, offset, w, h);
        return;
    }
    x /= 4u;
    const eng_surface* s = &g.surfaces[slot];
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    u8* rows = eng_coherence_buffer(w * 4u * h);
    if (!rows) return;
    const int rgba = rsx_texture_argb_is_rgba();
    const u32 rpitch = eng_tiled_row_pitch(location, offset, pitch);
    for (u32 r = 0; r < h; r++) {
        const u8* src = eng_guest_image(location, offset + r * rpitch, w * 4u);
        if (!src) return;
        eng_row_convert(rows + (size_t)r * w * 4u, src, w, rgba, 0);
    }
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] guest_wrote %u:0x%08X %ux%u -> surface 0x%08X at %u,%u:"
                        " %u non-zero pixels\n", location, offset, w, h, s->offset, x, y,
                eng_nonzero_pixels(rows, w * h, 4u));
    g.be->color_target_write(g.be->user, s->handle, x, y, w, h, rows, w * 4u);
    if (!eng_legacy_surfaces()) {
        /* The target changed outside a draw: copies keyed to its last write
         * no longer hold its pixels. */
        eng_surface* ms = &g.surfaces[slot];
        if (ms->region) { g.be->texture_release(g.be->user, ms->region); ms->region = 0; }
        if (ms->reint) { g.be->texture_release(g.be->user, ms->reint); ms->reint = 0; }
    }
    eng_surface_tag(slot);
}

static u32 eng_current_surface(void)
{
    u32 slots[RSX_BE_MAX_COLOR_TARGETS];
    return eng_current_target_set(slots) ? slots[0] : ENG_INVALID;
}

/* Guest A8R8G8B8 rows [offset, + pitch * h) into the top-left of surface
 * `slot` (RPCS3 upload_image_simple for a present read from cell). */
static void eng_surface_upload_rows(u32 slot, u32 location, u32 offset, u32 pitch, u32 w, u32 h)
{
    const eng_surface* s = &g.surfaces[slot];
    if (!g.be->color_target_write || s->fmt != RSX_BE_FMT_R8G8B8A8 || !pitch) return;
    if (w > s->w) w = s->w;
    if (h > s->h) h = s->h;
    if (!w || !h) return;
    u8* rows = eng_coherence_buffer(w * 4u * h);
    if (!rows) return;
    const int rgba = rsx_texture_argb_is_rgba();
    const u32 rpitch = eng_tiled_row_pitch(location, offset, pitch);
    for (u32 r = 0; r < h; r++) {
        const u8* src = eng_guest_image(location, offset + r * rpitch, w * 4u);
        if (!src) return;
        eng_row_convert(rows + (size_t)r * w * 4u, src, w, rgba, 0);
    }
    g.be->color_target_write(g.be->user, s->handle, 0, 0, w, h, rows, w * 4u);
    eng_surface* ms = &g.surfaces[slot];
    if (ms->region) { g.be->texture_release(g.be->user, ms->region); ms->region = 0; }
    if (ms->reint) { g.be->texture_release(g.be->user, ms->reint); ms->reint = 0; }
}

/* ---- per-zeta depth ------------------------------------------------------ */

/* RPCS3 float depth (CONTROL0 depth_float): Z16 holds an unsigned e4m12
 * float, Z24S8 an f24 -- the top 24 bits of an IEEE float shifted down by
 * 7 (TextureUtils get_depth_clear_value, ShuffleBytes.glsl). */
static float eng_bits_f32(u32 b) { float f; memcpy(&f, &b, 4); return f; }
static u32 eng_f32_bits(float f) { u32 b; memcpy(&b, &f, 4); return b; }
static float eng_e4m12_to_f32(u32 v)
{
    return eng_bits_f32((v << 11) & 0x07FFF800u) * eng_bits_f32(0x77000000u);
}
static u32 eng_f32_to_e4m12(float f)
{
    const float hi = eng_bits_f32(0x3F7FF800u);
    if (!(f > 0.0f)) f = 0.0f;
    if (f > hi) f = hi;
    return ((eng_f32_bits(f * eng_bits_f32(0x08000000u)) + 0x400u) >> 11) & 0xFFFFu;
}
static float eng_f24_to_f32(u32 v) { return eng_bits_f32((v & 0xFFFFFFu) << 7); }
static u32 eng_f32_to_f24(float f)
{
    if (!(f > 0.0f)) return 0u;
    return (eng_f32_bits(f) >> 7) & 0xFFFFFFu;
}

/* RPCS3 graph_engine::get_framebuffer_layout (RSXGraphEngine.cpp:180-560):
 * which of the colour buffers and the zeta a clear or draw really touches.
 * The context is RPCS3's framebuffer_creation_context: draw 0, clear colour
 * 1, clear depth 2, clear all 3. */
#define ENG_FB_DRAW        0u
#define ENG_FB_CLEAR_COLOR 1u
#define ENG_FB_CLEAR_DEPTH 2u
typedef struct eng_fb_layout {
    int color_live[4];   /* by buffer index A..D                          */
    int any_color;       /* rtt_config_valid from a colour buffer         */
    int zeta_live;       /* layout.zeta_address != 0                      */
    int zeta_used;       /* !depth_buffer_unused for the context          */
    int zeta_write;      /* layout.zeta_write_enabled                     */
    int color_write[4];  /* layout.color_write_enabled, by buffer index   */
    u32 width;           /* layout.width in pixels after the pitch fit    */
    int contested;       /* rtt_config_contested                          */
    int invalid;         /* clip w/h 0: rtt_config_valid cleared outright  */
} eng_fb_layout;

/* rsx::utility::get_rtt_indexes. An undefined target value is read as A,
 * as eng_current_target_set binds it. */
static u32 eng_rtt_indexes(u32 target, u32 idx[4])
{
    switch (target) {
    case CELL_GCM_SURFACE_TARGET_NONE: return 0;
    case CELL_GCM_SURFACE_TARGET_1:    idx[0] = 1; return 1;
    case CELL_GCM_SURFACE_TARGET_MRT1: idx[0] = 0; idx[1] = 1; return 2;
    case CELL_GCM_SURFACE_TARGET_MRT2: idx[0] = 0; idx[1] = 1; idx[2] = 2; return 3;
    case CELL_GCM_SURFACE_TARGET_MRT3: idx[0] = 0; idx[1] = 1; idx[2] = 2; idx[3] = 3; return 4;
    default:                           idx[0] = 0; return 1;
    }
}

static u32 eng_color_texel_bytes(u32 cfmt)
{
    switch (cfmt) {
    case 9:                    return 1u;   /* B8                        */
    case 1: case 2: case 3:
    case 0xA:                  return 2u;   /* X1R5G5B5 x2, R5G6B5, G8B8 */
    case 0xB:                  return 8u;   /* FP16 RGBA                 */
    case 0xC:                  return 16u;  /* FP32 RGBA                 */
    default:                   return 4u;
    }
}

static void eng_fb_layout_get(const rsx_dispatch* r, u32 context, eng_fb_layout* out)
{
    memset(out, 0, sizeof *out);
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(r, &sf);
    const u32 aa_u = sf.aa_sx ? sf.aa_sx : 1u, aa_v = sf.aa_sy ? sf.aa_sy : 1u;
    const u32 width = sf.clip_w / aa_u, height = sf.clip_h / aa_v;
    if (!width || !height) { out->invalid = 1; return; }

    const int z16 = sf.depth_format == 1u;
    const u32 depth_texel = (z16 ? 2u : 4u) * aa_u;
    const u32 color_texel = eng_color_texel_bytes(sf.color_format) * aa_u;
    const int stencil_test = !z16 && (rsx_dsp_reg(r, M_STENCIL_TEST_ENABLE) & 1u);
    const int depth_test = rsx_dsp_reg(r, M_DEPTH_TEST_ENABLE) & 1u;
    const int depth_write = rsx_dsp_reg(r, M_DEPTH_WRITE) & 1u;

    out->zeta_write = depth_test && depth_write;
    if (!out->zeta_write && stencil_test) {
        const u32 keep = 0x1E00u;
        u32 mask = rsx_dsp_reg(r, M_STENCIL_MASK) & 0xFFu;
        int active = rsx_dsp_reg(r, M_STENCIL_OP_ZPASS) != keep ||
                     rsx_dsp_reg(r, M_STENCIL_OP_FAIL) != keep ||
                     rsx_dsp_reg(r, M_STENCIL_OP_ZFAIL) != keep;
        if ((!mask || !active) && (rsx_dsp_reg(r, M_TWO_SIDED_STENCIL) & 1u)) {
            mask |= rsx_dsp_reg(r, 0x034C) & 0xFFu;
            active |= rsx_dsp_reg(r, 0x0364) != keep ||
                      rsx_dsp_reg(r, M_BACK_STENCIL_OP_FAIL) != keep ||
                      rsx_dsp_reg(r, 0x0360) != keep;
        }
        out->zeta_write = mask && active;
    }

    u32 idx[4];
    const u32 n_idx = eng_rtt_indexes(sf.color_target, idx);
    int color_write[4] = { 0, 0, 0, 0 }, any_write = 0;
    for (u32 i = 0; i < n_idx; i++) {
        const int en = i == 0 ? rsx_dsp_reg(r, M_COLOR_MASK) != 0u
                              : ((rsx_dsp_reg(r, 0x0370) >> (i * 4u)) & 0xFu) != 0u;
        if (en) { color_write[idx[i]] = 1; any_write = 1; }
    }

    int depth_unused = 0, color_unused = 0;
    if (context == ENG_FB_CLEAR_DEPTH) color_unused = 1;
    else if (context == ENG_FB_CLEAR_COLOR) depth_unused = 1;
    else if (context == ENG_FB_DRAW) {
        if (!stencil_test) {
            if (!depth_test) depth_unused = 1;
            else if (!depth_write) {
                const u32 f = rsx_dsp_reg(r, M_DEPTH_FUNC);
                if (f == 0x0200u || f == 0x0207u) depth_unused = 1;   /* never / always */
            }
            if (depth_unused && (rsx_dsp_reg(r, M_DEPTH_BOUNDS_ENABLE) & 1u)) {
                float bmin, bmax;
                const u32 a = rsx_dsp_reg(r, M_DEPTH_BOUNDS_MIN), b = rsx_dsp_reg(r, M_DEPTH_BOUNDS_MAX);
                memcpy(&bmin, &a, 4); memcpy(&bmax, &b, 4);
                if (bmin > 0.0f || bmax < 1.0f) depth_unused = 0;
            }
        }
        color_unused = !any_write || sf.color_target == CELL_GCM_SURFACE_TARGET_NONE;
        if (color_unused || depth_unused) out->contested = 1;
    }

    /* Pitch-linear rasters must fit the row the scissor can write; a pitch of
     * 64 bytes or less that cannot is dropped (a larger one only narrows the
     * surface, the surface store's business). Never-written scissor
     * registers read their reset, 4096 wide at 0. */
    int zeta = !depth_unused;
    out->zeta_used = zeta;
    out->width = width;
    for (u32 k = 0; k < 4; k++) out->color_write[k] = color_write[k];
    int color[4] = { 0, 0, 0, 0 };
    for (u32 i = 0; i < n_idx; i++) color[idx[i]] = !color_unused;
    if (sf.raster_type != 2u) {
        const u32 sh = r->seen[M_SCISSOR_HORIZONTAL >> 2] ? rsx_dsp_reg(r, M_SCISSOR_HORIZONTAL) : 0x10000000u;
        const u32 sx2 = (sh & 0xFFFFu) + (sh >> 16);
        const u32 limit = width < sx2 ? width : sx2;
        /* RPCS3's corrected_width: a pitch shorter than the clip row (and,
         * when it fails the scissor row too, longer than 64 bytes) narrows
         * the framebuffer to what the pitch holds. */
        u32 corrected = 0xFFFFu;
        if (zeta) {
            if (sf.zeta_pitch < depth_texel * limit) {
                if (sf.zeta_pitch > 64u) corrected = sf.zeta_pitch / depth_texel;
            } else if (width * depth_texel > sf.zeta_pitch) {
                corrected = sf.zeta_pitch / depth_texel;
            }
        }
        for (u32 i = 0; i < n_idx; i++) {
            const u32 k = idx[i], p = sf.color_pitch[k];
            if (!color[k]) continue;
            if (p < color_texel * limit) {
                if (p > 64u && p / color_texel < corrected) corrected = p / color_texel;
            } else if (width * color_texel > p && p / color_texel < corrected) {
                corrected = p / color_texel;
            }
        }
        if (corrected != 0xFFFFu) out->width = corrected;
        if (zeta && sf.zeta_pitch < depth_texel * limit && sf.zeta_pitch <= 64u) zeta = 0;
        for (u32 i = 0; i < n_idx; i++) {
            const u32 k = idx[i];
            if (color[k] && sf.color_pitch[k] < color_texel * limit && sf.color_pitch[k] <= 64u)
                color[k] = 0;
        }
    } else {
        /* Swizzled: RPCS3's second colour pass keeps minimum_color_pitch = 64
         * and checks the raw register pitch (rsx_dsp_get_surface already
         * replaced sf.color_pitch with the packed row). */
        static const u32 praw[4] = { 0x020Cu, 0x021Cu, 0x0280u, 0x0284u };   /* SURFACE_PITCH_A..D */
        for (u32 i = 0; i < n_idx; i++) {
            const u32 k = idx[i];
            if (color[k] && rsx_dsp_reg(r, praw[k]) < 64u) color[k] = 0;
        }
    }

    /* Aliasing colour and zeta (fb_aliasing_bias _auto): a depth or stencil
     * test, or a zeta-only write, keeps the zeta; anything else keeps the
     * colour. Compared at the raw addresses, before the window offset. */
    static const u32 coff_reg[4] = { 0x0210, 0x0218, 0x0288, 0x028C };
    const u32 zoff = rsx_dsp_reg(r, 0x0214);
    for (u32 i = 0; i < n_idx; i++) {
        const u32 k = idx[i];
        if (!color[k] || !zeta) continue;
        if (sf.color_location[k] != sf.zeta_location || rsx_dsp_reg(r, coff_reg[k]) != zoff)
            continue;
        out->contested = 1;
        if (depth_test || stencil_test || (!color_write[k] && out->zeta_write))
            color[k] = 0;
        else
            zeta = 0;
    }

    for (u32 k = 0; k < 4; k++) {
        out->color_live[k] = color[k];
        if (color[k]) out->any_color = 1;
    }
    out->zeta_live = zeta;
}

/* RSX_ENG_FB_SURFACES=0: bind every named colour buffer and the zeta whatever
 * the framebuffer layout drops or leaves unused (the pre-port behaviour). */
static int eng_fb_surfaces(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("RSX_ENG_FB_SURFACES");
        v = (e && e[0] == '0') ? 0 : 1;
    }
    return v && !eng_legacy_surfaces();
}

/* RSX_ENG_PRESENT_MERGED=0: present the exact-address surface (pre-port)
 * instead of RPCS3 get_present_source's newest overlapping same-pitch one. */
static int eng_present_merged(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("RSX_ENG_PRESENT_MERGED");
        v = (e && e[0] == '0') ? 0 : 1;
    }
    return v && !eng_legacy_surfaces();
}

static u32 eng_surface_bpp(u32 fmt)
{
    switch (fmt) {
    case RSX_BE_FMT_R8: return 1u;
    case RSX_BE_FMT_R8G8: case RSX_BE_FMT_R16: return 2u;
    case RSX_BE_FMT_R16G16B16A16F: return 8u;
    case RSX_BE_FMT_R32G32B32A32F: return 16u;
    default: return 4u;
    }
}

static u32 eng_current_target_set_ex(u32 slots[RSX_BE_MAX_COLOR_TARGETS],
                                     const eng_fb_layout* fbl)
{
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    if (!eng_fb_surfaces()) fbl = NULL;
    const rsx_be_format fmt = eng_surface_format(sf.color_format);
    const u32 aa_x = sf.aa_sx ? sf.aa_sx : 1u;
    /* RPCS3 layout.width after the pitch fit, in samples. */
    u32 want_w = sf.clip_w;
    if (fbl && fbl->width && fbl->width * aa_x < want_w) want_w = fbl->width * aa_x;
    if (fbl && !fbl->any_color) {
        const u32 s0 = eng_scratch_target(0, want_w, sf.clip_h, fmt, sf.aa_sx, sf.aa_sy);
        if (s0 == ENG_INVALID) return 0;
        slots[0] = s0;
        return 1;
    }
    /* SET_SURFACE_COLOR_TARGET 2 selects B alone; every other value starts at
     * A, and the MRT selectors add B, C and D on top of it. */
    const u32 sel = (sf.color_target == CELL_GCM_SURFACE_TARGET_1) ? 1u : 0u;
    s_eng_bind_cfmt = sf.color_format & 0x1Fu;
    const u32 first = (!fbl || fbl->color_live[sel])
        ? eng_surface_get(sf.color_location[sel], sf.color_offset[sel],
                          want_w, sf.clip_h, fmt, sf.color_pitch[sel],
                          sf.aa_sx, sf.aa_sy)
        : eng_scratch_target(0, want_w, sf.clip_h, fmt, sf.aa_sx, sf.aa_sy);
    if (first == ENG_INVALID) { s_eng_bind_cfmt = 0; return 0; }
    if (g.surfaces[first].location != ENG_LOC_SCRATCH) {
        g.surfaces[first].pitch = sf.color_pitch[sel];
        g.surfaces[first].cfmt = sf.color_format & 0x1Fu;
    }
    slots[0] = first;
    u32 n = 1;
    s_eng_rt_keep = 1ull << first;

    static const u32 mrt_from[3] = {
        CELL_GCM_SURFACE_TARGET_MRT1, CELL_GCM_SURFACE_TARGET_MRT2,
        CELL_GCM_SURFACE_TARGET_MRT3
    };
    for (u32 i = 0; i < 3; i++) {
        if (sf.color_target < mrt_from[i]) break;
        /* At A's size, which may be larger than the clip (see above): the
         * framebuffer takes A's extent, and every attachment must cover it. */
        const u32 slot = (!fbl || fbl->color_live[i + 1])
            ? eng_surface_get(sf.color_location[i + 1], sf.color_offset[i + 1],
                              g.surfaces[first].w, g.surfaces[first].h, fmt,
                              sf.color_pitch[i + 1], sf.aa_sx, sf.aa_sy)
            : eng_scratch_target(i + 1, g.surfaces[first].w, g.surfaces[first].h,
                                 fmt, sf.aa_sx, sf.aa_sy);
        if (slot == ENG_INVALID) break;
        if (g.surfaces[slot].location != ENG_LOC_SCRATCH) {
            g.surfaces[slot].pitch = sf.color_pitch[i + 1];
            g.surfaces[slot].cfmt = sf.color_format & 0x1Fu;
        }
        /* A set naming one buffer twice would attach the same target twice,
         * which no host API allows. */
        int seen = 0;
        for (u32 k = 0; k < n; k++) if (slots[k] == slot) seen = 1;
        if (seen) break;
        slots[n++] = slot;
        s_eng_rt_keep |= 1ull << slot;
    }
    s_eng_rt_keep = 0;
    s_eng_bind_cfmt = 0;
    return n;
}

/* RPCS3 graph_engine::bind_framebuffer: the layout is rebuilt only when a
 * surface register changed (rtt_config_dirty), the creation context differs,
 * or a contested layout gains a use for what it left out; otherwise the bound
 * framebuffer stays as it is, with only the write masks re-evaluated
 * (on_framebuffer_options_changed). A rebuilt layout naming the same
 * surfaces keeps them bound without prepare_render_target (ignore_change).
 * RSX_ENG_FB_CACHE=0 rebuilds and rebinds on every draw and clear. */
static u32 eng_zdepth_get(u32 location, u32 offset, u32 rt_w, u32 rt_h);
typedef struct eng_fb_cache {
    int have, dirty;
    u32 context;
    eng_fb_layout fbl;
    int bound;
    rsx_dsp_surface lsf, sf;
    u32 gw, gh, zfloat;
    eng_fb_layout bfbl;
    u32 n, t[RSX_BE_MAX_COLOR_TARGETS], th[RSX_BE_MAX_COLOR_TARGETS];
    u32 tloc[RSX_BE_MAX_COLOR_TARGETS], toff[RSX_BE_MAX_COLOR_TARGETS];
    u32 tw[RSX_BE_MAX_COLOR_TARGETS], tht[RSX_BE_MAX_COLOR_TARGETS];
    u32 tpitch[RSX_BE_MAX_COLOR_TARGETS];
    u32 z, zh, zloc, zoff;
} eng_fb_cache;
static eng_fb_cache s_fbc;

static int eng_fb_cache_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("RSX_ENG_FB_CACHE");
        v = (e && e[0] == '0') ? 0 : 1;
    }
    return v && eng_fb_surfaces();
}

static void eng_fb_cache_reset(void) { memset(&s_fbc, 0, sizeof s_fbc); }

/* zeta_write_enabled from depth alone (evaluate_depth_buffer_state) and,
 * when that is off, from the stencil ops (evaluate_stencil_buffer_state). */
static int eng_fb_stencil_writes(const rsx_dispatch* r)
{
    const u32 keep = 0x1E00u;
    u32 mask = rsx_dsp_reg(r, 0x032C) & 0xFFu;
    int active = rsx_dsp_reg(r, 0x0344) != keep || rsx_dsp_reg(r, 0x033C) != keep ||
                 rsx_dsp_reg(r, 0x0340) != keep;
    if ((!mask || !active) && (rsx_dsp_reg(r, 0x0348) & 1u)) {
        mask |= rsx_dsp_reg(r, 0x034C) & 0xFFu;
        active |= rsx_dsp_reg(r, 0x0364) != keep || rsx_dsp_reg(r, 0x035C) != keep ||
                  rsx_dsp_reg(r, 0x0360) != keep;
    }
    return mask && active;
}

static void eng_fb_reg_changed(const rsx_dispatch* r, u32 m, u32 old_value, u32 new_value)
{
    if (!s_fbc.have) return;
    switch (m) {
    case 0x0208:   /* SURFACE_FORMAT: the high bits are log2 dimensions */
        if (((old_value ^ new_value) & 0xFFFFu) == 0) return;
        /* fallthrough */
    case 0x0200: case 0x0204: case 0x0210: case 0x0218: case 0x0288: case 0x028C:
    case 0x0220: case 0x0214: case 0x0194: case 0x018C: case 0x01B4: case 0x01B8:
    case 0x0198: case 0x020C: case 0x021C: case 0x0280: case 0x0284: case 0x022C:
    case 0x02B8:
        s_fbc.dirty = 1;
        s_fbc.fbl.contested = 0;
        return;
    default:
        break;
    }
    if (s_fbc.dirty) return;
    eng_fb_layout* l = &s_fbc.fbl;
    const int z16 = s_fbc.lsf.depth_format == 1u;
    const int depth_test = rsx_dsp_reg(r, 0x0A74) & 1u;
    const int depth_zw = depth_test && (rsx_dsp_reg(r, 0x0A70) & 1u);
    const int stencil_read = !z16 && (rsx_dsp_reg(r, 0x0328) & 1u);
    int z_contested_check = 0;
    switch (m) {
    case 0x0A74: case 0x0A70: case 0x0A6C:
        l->zeta_write = depth_zw;
        z_contested_check = 1;
        break;
    case 0x0348: case 0x0328: case 0x032C: case 0x0344: case 0x033C: case 0x0340:
    case 0x034C: case 0x0364: case 0x035C: case 0x0360:
        l->zeta_write = depth_zw;
        if (!l->zeta_write && stencil_read) l->zeta_write = eng_fb_stencil_writes(r);
        z_contested_check = 1;
        break;
    case 0x0324: case 0x0370: {
        int old_any = 0;
        for (u32 k = 0; k < 4; k++) if (l->color_write[k]) old_any = 1;
        u32 idx[4];
        const u32 n_idx = eng_rtt_indexes(s_fbc.lsf.color_target, idx);
        int any = 0;
        for (u32 k = 0; k < 4; k++) l->color_write[k] = 0;
        for (u32 i = 0; i < n_idx; i++) {
            const int en = i == 0 ? rsx_dsp_reg(r, 0x0324) != 0u
                                  : ((rsx_dsp_reg(r, 0x0370) >> (i * 4u)) & 0xFu) != 0u;
            if (en) { l->color_write[idx[i]] = 1; any = 1; }
        }
        if (l->contested && !old_any && any) s_fbc.dirty = 1;
        return;
    }
    default:
        return;
    }
    /* evaluate_depth_buffer_contested: a contested layout without a zeta
     * that now tests or writes depth, or tests stencil, is rebuilt. */
    if (z_contested_check && l->contested && !l->zeta_live &&
        (l->zeta_write || depth_test || stencil_read))
        s_fbc.dirty = 1;
}

static void sink_reg_changed(void* user, const rsx_dispatch* r, u32 m, u32 old_value,
                             u32 new_value)
{
    (void)user;
    if (!eng_reg_pipeline_irrelevant(m)) s_eng_pgen++;
    eng_fb_reg_changed(r, m, old_value, new_value);
}

static int eng_fb_cache_slots_ok(void)
{
    for (u32 i = 0; i < s_fbc.n; i++) {
        if (s_fbc.t[i] >= g.n_surfaces) return 0;
        const eng_surface* s = &g.surfaces[s_fbc.t[i]];
        if (s->handle != s_fbc.th[i] ||
            s->location != s_fbc.tloc[i] || s->offset != s_fbc.toff[i] ||
            s->w != s_fbc.tw[i] || s->h != s_fbc.tht[i] || s->pitch != s_fbc.tpitch[i])
            return 0;
    }
    if (s_fbc.z != ENG_INVALID) {
        if (s_fbc.z >= g.n_zdepths) return 0;
        const eng_zdepth* z = &g.zdepths[s_fbc.z];
        if (z->handle != s_fbc.zh ||
            z->location != s_fbc.zloc || z->offset != s_fbc.zoff)
            return 0;
    }
    return 1;
}

/* The layout for `context` and the colour targets and zeta it binds (zslot
 * ENG_INVALID when the layout has no zeta). Returns the colour target count,
 * 0 when nothing could be bound. */
static u32 eng_fb_bind(u32 context, eng_fb_layout* fbl,
                       u32 targets[RSX_BE_MAX_COLOR_TARGETS], u32* zslot)
{
    const int on = eng_fb_cache_on();
    rsx_dsp_surface sf;
    memset(&sf, 0, sizeof sf);
    rsx_dsp_get_surface(&g.rsx, &sf);
    if (!on || !s_fbc.have || s_fbc.dirty || s_fbc.context != context) {
        eng_fb_layout_get(&g.rsx, context, &s_fbc.fbl);
        s_fbc.have = 1;
        s_fbc.dirty = 0;
        s_fbc.context = context;
        s_fbc.lsf = sf;
    }
    *fbl = s_fbc.fbl;
    *zslot = ENG_INVALID;
    const u32 zfloat = (rsx_dsp_reg(&g.rsx, 0x03B0) >> 12) & 1u;
    /* ignore_change: the same surfaces at the same size and format. */
    if (on && s_fbc.bound && s_fbc.gw == g.width && s_fbc.gh == g.height &&
        s_fbc.zfloat == zfloat && !memcmp(&s_fbc.sf, &sf, sizeof sf) &&
        s_fbc.bfbl.width == fbl->width &&
        s_fbc.bfbl.zeta_live == fbl->zeta_live && s_fbc.bfbl.any_color == fbl->any_color &&
        !memcmp(s_fbc.bfbl.color_live, fbl->color_live, sizeof fbl->color_live) &&
        eng_fb_cache_slots_ok()) {
        for (u32 i = 0; i < s_fbc.n; i++) targets[i] = s_fbc.t[i];
        *zslot = s_fbc.z;
        return s_fbc.n;
    }
    s_fbc.bound = 0;
    /* rtt_config_valid false: a clear with nothing left binds nothing. */
    if (context != ENG_FB_DRAW && !fbl->any_color && !fbl->zeta_live) return 0;
    const u32 n = eng_current_target_set_ex(targets, fbl);
    if (!n) return 0;
    /* RSX_ENG_FB_SURFACES=0 keeps a draw's unused zeta bound. */
    const int bind_z = (context == ENG_FB_DRAW && !eng_fb_surfaces())
                     ? !(fbl->zeta_used && !fbl->zeta_live) : fbl->zeta_live;
    if (bind_z) {
        u32 zw = sf.clip_w;
        const u32 fw = fbl->width * (sf.aa_sx ? sf.aa_sx : 1u);
        if (eng_fb_surfaces() && fbl->width && fw < zw) zw = fw;
        *zslot = eng_zdepth_get(sf.zeta_location, sf.zeta_offset, zw, sf.clip_h);
    }
    if (!on || (bind_z && *zslot == ENG_INVALID)) return n;
    s_fbc.bound = 1;
    s_fbc.sf = sf;
    s_fbc.gw = g.width; s_fbc.gh = g.height; s_fbc.zfloat = zfloat;
    s_fbc.bfbl = *fbl;
    s_fbc.n = n;
    for (u32 i = 0; i < n; i++) {
        const eng_surface* s = &g.surfaces[targets[i]];
        s_fbc.t[i] = targets[i];
        s_fbc.th[i] = s->handle; s_fbc.tloc[i] = s->location; s_fbc.toff[i] = s->offset;
        s_fbc.tw[i] = s->w; s_fbc.tht[i] = s->h; s_fbc.tpitch[i] = s->pitch;
    }
    s_fbc.z = *zslot;
    if (*zslot != ENG_INVALID) {
        s_fbc.zh = g.zdepths[*zslot].handle;
        s_fbc.zloc = g.zdepths[*zslot].location;
        s_fbc.zoff = g.zdepths[*zslot].offset;
    }
    return n;
}

/* RPCS3 surface_store::on_write after a draw or clear: only the colour
 * buffers the layout kept AND whose write mask is on take the new contents;
 * a masked-off or dropped buffer keeps its tag. Scratch targets never do. */
static void eng_surfaces_wrote(const u32* targets, u32 n_targets);
static void eng_surfaces_wrote_fb(const u32* targets, u32 n_targets, const eng_fb_layout* fbl)
{
    if (!eng_fb_surfaces()) { eng_surfaces_wrote(targets, n_targets); return; }
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    u32 idx[4];
    const u32 n_idx = eng_rtt_indexes(sf.color_target, idx);
    u32 out[RSX_BE_MAX_COLOR_TARGETS], n = 0;
    for (u32 i = 0; i < n_targets && i < n_idx; i++) {
        if (g.surfaces[targets[i]].location == ENG_LOC_SCRATCH) continue;
        if (fbl && (!fbl->color_live[idx[i]] || !fbl->color_write[idx[i]])) continue;
        out[n++] = targets[i];
    }
    if (n) eng_surfaces_wrote(out, n);
}

/* RPCS3 graph_engine::resolve_scissor: a scissor (clipped against the
 * viewport for draws, not for clears) that is empty or starts at or beyond
 * WINDOW_CLIP_HORIZONTAL/VERTICAL (0x02C0/0x02C4) leaves rtt_config invalid,
 * and the draw or clear is skipped. Registers the title never wrote take the
 * nv40 reset values (scissor and viewport 4096 wide at 0, window clip
 * 0x0FFF0000); the window clip test is on the raw register, so a title that
 * writes 0 there skips every draw, as on RPCS3. */
static int eng_scissor_invalid(int clip_viewport)
{
    const rsx_dispatch* r = &g.rsx;
    const u32 rst = 0x10000000u;
    const u32 sh = r->seen[M_SCISSOR_HORIZONTAL >> 2] ? rsx_dsp_reg(r, M_SCISSOR_HORIZONTAL) : rst;
    const u32 sv = r->seen[M_SCISSOR_VERTICAL >> 2] ? rsx_dsp_reg(r, M_SCISSOR_VERTICAL) : rst;
    u16 x1 = (u16)(sh & 0xFFFFu), x2 = (u16)((sh & 0xFFFFu) + (sh >> 16));
    u16 y1 = (u16)(sv & 0xFFFFu), y2 = (u16)((sv & 0xFFFFu) + (sv >> 16));
    if (clip_viewport) {
        const u32 vh = r->seen[0x0A00 >> 2] ? rsx_dsp_reg(r, 0x0A00) : rst;
        const u32 vv = r->seen[0x0A04 >> 2] ? rsx_dsp_reg(r, 0x0A04) : rst;
        const u32 rx2 = (vh & 0xFFFFu) + (vh >> 16), ry2 = (vv & 0xFFFFu) + (vv >> 16);
        const u32 sx2 = (sh & 0xFFFFu) + (sh >> 16), sy2 = (sv & 0xFFFFu) + (sv >> 16);
        if ((vh & 0xFFFFu) > x1) x1 = (u16)(vh & 0xFFFFu);
        if ((vv & 0xFFFFu) > y1) y1 = (u16)(vv & 0xFFFFu);
        x2 = (u16)(sx2 < rx2 ? sx2 : rx2);
        y2 = (u16)(sy2 < ry2 ? sy2 : ry2);
    }
    /* RPCS3 compares x1 / y1 with the raw registers (rsx_methods.h
     * window_clip_horizontal), whose reset value is 0x0FFF0000
     * (rsx_state::init / reset); R2 never writes them. */
    const u32 wrst = 0x0FFF0000u;
    const u32 wch = r->seen[0x02C0 >> 2] ? rsx_dsp_reg(r, 0x02C0) : wrst;
    const u32 wcv = r->seen[0x02C4 >> 2] ? rsx_dsp_reg(r, 0x02C4) : wrst;
    return x2 <= x1 || y2 <= y1 || x1 >= wch || y1 >= wcv;
}

/* Bytes per depth sample of SURFACE_FORMAT depth field `zfmt` (1 = Z16). */
static u32 eng_zeta_bpp(u32 zfmt) { return zfmt == 1u ? 2u : 4u; }

/* RPCS3 intersect_surface_region / set_old_contents for a depth surface:
 * every zeta newer than `slot` in the same partition, with the same pitch and
 * overlapping its memory -- `prev`, the surface this one replaces at its own
 * address, included -- hands over the bytes they share, oldest first, through
 * the backend's point-sampled depth copy (scale_image, linear = false). The
 * regions are RPCS3's normalized bytes, so a sample grid change at one pitch
 * is a 1:1 copy of sample rows and a bpp change scales x by src_bpp/dst_bpp.
 * dw x dh: the new surface in samples. Colour surfaces over the same memory
 * (RPCS3 scans the colour store as list1, and bind_surface_address takes a
 * colour surface at this very address as old_surface) and zetas of another
 * format class are the typeless copy (surface_typeless_copy): the guest
 * words reinterpreted, Z16 <-> Z24S8 at bpp scale, fixed <-> float d24 as
 * raw bits, never a value conversion. */
static void eng_zdepth_intersect(u32 slot, const eng_zdepth* prev, const rsx_dsp_surface* zsf,
                                 u32 dw, u32 dh)
{
    eng_zdepth* d = &g.zdepths[slot];
    const u32 pitch = zsf->zeta_pitch;
    if ((!g.be->depth_target_copy && !g.be->surface_typeless_copy) || !d->handle || !pitch ||
        d->location >= ENG_LOC_SCRATCH)
        return;
    const u32 dbpp = eng_zeta_bpp(zsf->depth_format);
    const int typeless = eng_typeless_inherit();
    const u32 dk = eng_zeta_tkind(zsf->depth_format == 1u ? 1u : 2u, d->depth_float);
    const u64 base = d->depth_serial;
    const int accept = d->depth_serial && d->tag_ok && eng_zdepth_tag_test(d);
    const u64 entry_serial = d->depth_serial;
    u32 n_stale = 0;
    /* order[]: a zeta (prev or a g.zdepths entry), or a colour surface. */
    struct { const eng_zdepth* z; const eng_surface* c; u64 ser; } order[ENG_MAX_ZDEPTHS + 1 + ENG_MAX_SURFACES];
    u32 n = 0;
    for (u32 i = 0; i <= g.n_zdepths + g.n_surfaces; i++) {
        const eng_zdepth* s = NULL;
        const eng_surface* c = NULL;
        u64 ser;
        u32 srows, saa;
        if (i <= g.n_zdepths) {
            s = i < g.n_zdepths ? &g.zdepths[i] : prev;
            if (!s || s == d || !s->handle || s->location != d->location ||
                s->depth_serial <= base)
                continue;
            if (s != prev && s->offset == d->offset) continue;
            ser = s->depth_serial; srows = s->used_h ? s->used_h : s->h; saa = s->aa_sy;
            if (s->pitch != pitch && srows / (saa ? saa : 1u) != 1u &&
                dh / (zsf->aa_sy ? zsf->aa_sy : 1u) != 1u)
                continue;
            if (!accept && !eng_zdepth_tag_test(s)) {
                eng_isect r;
                const u32 sw_s = s->used_w ? s->used_w : s->w;
                n_stale += eng_intersect(s->offset, sw_s * eng_zeta_bpp(s->zfmt), srows,
                                         d->offset, dw * dbpp, dh, pitch, &r) != 0;
                continue;
            }
        } else {
            c = &g.surfaces[i - g.n_zdepths - 1u];
            if (!typeless || !c->handle || c->swz || c->location != d->location ||
                c->color_serial <= base || eng_color_tkind(c) == ENG_INVALID)
                continue;
            if (c->pitch != pitch && c->h / c->aa_sy != 1u &&
                dh / (zsf->aa_sy ? zsf->aa_sy : 1u) != 1u)
                continue;
            if (!accept && !eng_surface_tag_test(c)) {
                eng_isect r;
                n_stale += eng_intersect(c->offset, c->w * eng_color_bytes(c), c->h, d->offset,
                                         dw * dbpp, dh, pitch, &r) != 0;
                continue;
            }
            ser = c->color_serial;
        }
        u32 k = n++;
        while (k && order[k - 1].ser > ser) { order[k] = order[k - 1]; k--; }
        order[k].z = s; order[k].c = c; order[k].ser = ser;
    }
    for (u32 k = 0; k < n; k++) {
        const eng_zdepth* s = order[k].z;
        const eng_surface* c = order[k].c;
        eng_isect r;
        if (c) {
            const u32 sk = eng_color_tkind(c);
            if (!eng_intersect(c->offset, c->w * eng_tkind_bpp(sk), c->h, d->offset, dw * dbpp, dh,
                               pitch, &r) ||
                !eng_typeless_isect(c->handle, sk, d->handle, dk, &r))
                continue;
            d->depth_serial = c->color_serial;
            d->cleared = 1;
            d->had_write = 1;
            if (c->guest_dirty) d->guest_dirty = 1;
            if (eng_xkind_supersede())
                eng_surface_superseded((u32)(c - g.surfaces), &r, eng_tkind_bpp(sk));
        } else {
            const u32 sk = eng_zeta_tkind(eng_zeta_zf(s), s->depth_float);
            const u32 sbpp = eng_zeta_bpp(s->zfmt);
            const u32 sw_s = s->used_w ? s->used_w : s->w, sh_s = s->used_h ? s->used_h : s->h;
            if (!eng_intersect(s->offset, sw_s * sbpp, sh_s, d->offset, dw * dbpp, dh, pitch, &r))
                continue;
            if (typeless && sk != dk && g.be->surface_typeless_copy) {
                if (!eng_typeless_isect(s->handle, sk, d->handle, dk, &r)) continue;
            } else {
                const u32 sw = r.wb / sbpp, w = r.wb / dbpp;
                if (!sw || !w || !g.be->depth_target_copy ||
                    !g.be->depth_target_copy(g.be->user, s->handle, r.sxb / sbpp, r.sy, sw, r.rows,
                                             d->handle, r.dxb / dbpp, r.dy, w, r.rows))
                    continue;
            }
            d->depth_serial = s->depth_serial;
            d->cleared = 1;
            if (s->had_write) d->had_write = 1;
            if (s->guest_dirty) d->guest_dirty = 1;
            if (s != prev) eng_zdepth_superseded((eng_zdepth*)s, &r, sbpp);
        }
        d->snapshot_valid = 0;
        d->zfmt = zsf->depth_format;
        d->pitch = pitch;
        if (!d->used_w || d->used_w < dw) d->used_w = dw;
        if (!d->used_h || d->used_h < dh) d->used_h = dh;
    }
    if (n_stale && base && !accept && d->depth_serial == entry_serial && eng_stale_erase() &&
        g.be->clear_depth_stencil) {
        g.be->clear_depth_stencil(g.be->user, d->handle,
                                  RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 1.0f, 0xFF);
        d->cleared = 1;
        d->snapshot_valid = 0;
        d->guest_dirty = 0;
        d->depth_serial = ++g.write_serial;
    }
    if (d->depth_serial != entry_serial) eng_zdepth_tag(slot);
}

/* One depth target per guest zeta address. Sharing a single resource across a
 * title's shadow, scene and post passes cross-contaminates later depth tests;
 * upstream's ef3271b records that as "the black player-character mass". */
static u32 eng_zdepth_get(u32 location, u32 offset, u32 rt_w, u32 rt_h)
{
    /* RPCS3 surface_store.h bind_surface_address (453-653) with
     * surface_matches_properties / matches_dimensions: a zeta matches only
     * at its exact size and sample count; any other is a new surface that
     * inherits the old one (eng_zdepth_intersect). RSX_ENG_ZETA_EXACT=0:
     * one growing target per address, at least the canvas, as before. */
    static int exact_en = -1;
    if (exact_en < 0) { const char* e = getenv("RSX_ENG_ZETA_EXACT"); exact_en = !(e && e[0] == '0'); }
    const int exact = exact_en && !eng_legacy_surfaces() && location < ENG_LOC_SCRATCH && rt_w && rt_h;
    /* The attachment must cover the whole canvas: an early pass declaring a
     * smaller clip than the live viewport otherwise gets a target the host
     * API rejects when a later, larger pass binds it. (Exact: a larger pass
     * is a new surface anyway.) */
    u32 want_w = exact ? rt_w : (rt_w > g.width ? rt_w : g.width);
    u32 want_h = exact ? rt_h : (rt_h > g.height ? rt_h : g.height);
    if (want_w > ENG_MAX_SURFACE_DIM || want_h > ENG_MAX_SURFACE_DIM)
        return ENG_INVALID;
    eng_alias_retire(location, offset, 1);

    u32 slot = ENG_MAX_ZDEPTHS;
    /* RPCS3 surface_depth_format2: the host format follows SURFACE_FORMAT's
     * depth field and CONTROL0 (0x03B0) depth_float bit 12; a different
     * format at the address is a new surface there. */
    rsx_dsp_surface zsf;
    rsx_dsp_get_surface(&g.rsx, &zsf);
    const u32 want_zfmt = g.be->depth_target_create_ex ? (zsf.depth_format == 1u ? 1u : 2u) : 0u;
    const u32 want_float = want_zfmt ? ((rsx_dsp_reg(&g.rsx, 0x03B0) >> 12) & 1u) : 0u;
    for (u32 i = 0; i < g.n_zdepths; i++) {
        eng_zdepth* z = &g.zdepths[i];
        if (z->location == location && z->offset == offset) {
            const int same_fmt = z->host_zfmt == want_zfmt && z->depth_float == want_float;
            const int size_ok = exact
                ? (z->w == want_w && z->h == want_h && z->aa_sx == zsf.aa_sx && z->aa_sy == zsf.aa_sy)
                : (z->w >= want_w && z->h >= want_h);
            if (same_fmt && size_ok) {
                if (g.be->target_set_aa &&
                    (z->aa_sx != zsf.aa_sx || z->aa_sy != zsf.aa_sy)) {
                    z->aa_sx = zsf.aa_sx; z->aa_sy = zsf.aa_sy;
                    z->aa_flags = g.be->target_set_aa(g.be->user, z->handle,
                                                      zsf.aa_sx, zsf.aa_sy);
                }
                /* RPCS3 convert_pitch (VK: unimplemented -> erase_bkgnd) and
                 * invalidate_surface_contents: rebound at a new pitch, the
                 * surface keeps its age but none of its pixels. */
                if (eng_pitch_discard() && z->pitch && zsf.zeta_pitch &&
                    z->pitch != zsf.zeta_pitch && location < ENG_LOC_SCRATCH) {
                    g.be->clear_depth_stencil(g.be->user, z->handle,
                                              RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL,
                                              1.0f, 0xFF);
                    z->cleared = 1;
                    z->had_write = 0;
                    z->snapshot_valid = 0;
                    z->guest_dirty = 0;
                    if (z->reint) { g.be->texture_release(g.be->user, z->reint); z->reint = 0; }
                    z->pitch = zsf.zeta_pitch;
                }
                if (!eng_aa_resolve_inherit())
                    eng_zdepth_intersect(i, NULL, &zsf, rt_w ? rt_w : g.width,
                                         rt_h ? rt_h : g.height);
                return i;
            }
            slot = i;
            if (!exact) {
                if (want_w < z->w) want_w = z->w;
                if (want_h < z->h) want_h = z->h;
            }
            break;
        }
    }
    if (slot == ENG_MAX_ZDEPTHS) {
        for (u32 i = 0; i < g.n_zdepths && slot == ENG_MAX_ZDEPTHS; i++)
            if (!g.zdepths[i].handle) slot = i;
        if (slot == ENG_MAX_ZDEPTHS && g.n_zdepths < ENG_MAX_ZDEPTHS) slot = g.n_zdepths;
        if (slot == ENG_MAX_ZDEPTHS && !s_texcache_legacy) {
            /* As eng_surface_evict: RPCS3's store is unbounded; evict the
             * least recently written zeta, written back first if dirty. */
            u32 best = ENG_INVALID;
            for (u32 i = 0; i < g.n_zdepths; i++) {
                const eng_zdepth* z = &g.zdepths[i];
                if (best == ENG_INVALID ||
                    (!z->guest_dirty && g.zdepths[best].guest_dirty) ||
                    (!z->guest_dirty == !g.zdepths[best].guest_dirty &&
                     z->depth_serial < g.zdepths[best].depth_serial))
                    best = i;
            }
            if (best != ENG_INVALID) {
                eng_zdepth* z = &g.zdepths[best];
                if (z->guest_dirty) eng_zdepth_to_guest(best);
                if (z->reint) g.be->texture_release(g.be->user, z->reint);
                g.be->depth_target_release(g.be->user, z->handle);
                memset(z, 0, sizeof *z);
                z->location = ENG_INVALID;
                slot = best;
            }
        }
        if (slot == ENG_MAX_ZDEPTHS) return ENG_INVALID;
    }

    /* RPCS3 split_surface_region<depth>: the replaced zeta's memory beyond
     * the new one (e.g. Z24S8 -> Z16 halves the bytes per row) lives on as a
     * clone holding its old bytes. This target is one per address and never
     * shrinks, so it grows to cover that memory and inherits the clone's
     * bytes past the clip; reads there see the same newest-wins data. */
    u32 iw = rt_w ? rt_w : g.width, ih = rt_h ? rt_h : g.height;
    if (!exact && slot < g.n_zdepths && g.zdepths[slot].handle && eng_split_surfaces() &&
        g.zdepths[slot].pitch && zsf.zeta_pitch == g.zdepths[slot].pitch &&
        g.zdepths[slot].depth_serial && eng_zdepth_tag_test(&g.zdepths[slot])) {
        const eng_zdepth* p = &g.zdepths[slot];
        const u32 pb = eng_zeta_bpp(p->zfmt), nb = eng_zeta_bpp(zsf.depth_format);
        const u32 pw = p->used_w ? p->used_w : p->w, ph = p->used_h ? p->used_h : p->h;
        if (nb && pw * pb > iw * nb) iw = (pw * pb) / nb;
        if (ph > ih) ih = ph;
        if (iw > ENG_MAX_SURFACE_DIM) iw = ENG_MAX_SURFACE_DIM;
        if (want_w < iw) want_w = iw;
        if (want_h < ih) want_h = ih;
    }

    const u32 handle = want_zfmt
        ? g.be->depth_target_create_ex(g.be->user, want_w, want_h, want_zfmt, want_float)
        : g.be->depth_target_create(g.be->user, want_w, want_h);
    if (!handle)
        return (slot < g.n_zdepths && g.zdepths[slot].handle) ? slot : ENG_INVALID;

    eng_zdepth* z = &g.zdepths[slot];
    /* RPCS3 bind_surface_address: the surface replaced at this address
     * (format, depth_float or size change) is old_surface, inherited from
     * below before it is released. */
    eng_zdepth prev = *z;
    const int has_prev = slot < g.n_zdepths && z->handle;
    if (z->reint) { g.be->texture_release(g.be->user, z->reint); z->reint = 0; }
    z->location = location; z->offset = offset;
    z->w = want_w; z->h = want_h;
    z->handle = handle;
    z->host_zfmt = want_zfmt;
    z->depth_float = want_float;
    z->aa_sx = zsf.aa_sx; z->aa_sy = zsf.aa_sy;
    z->aa_flags = g.be->target_set_aa
                ? g.be->target_set_aa(g.be->user, handle, zsf.aa_sx, zsf.aa_sy) : 0u;
    z->snapshot = 0;
    z->cleared = 0;
    z->had_write = 0;
    z->snapshot_valid = 0;
    z->depth_serial = 0;
    z->tag_ok = 0;
    z->guest_dirty = 0;
    if (slot == g.n_zdepths) g.n_zdepths++;
    if (!eng_aa_resolve_inherit())
        eng_zdepth_intersect(slot, has_prev ? &prev : NULL, &zsf, iw, ih);
    if (has_prev) g.be->depth_target_release(g.be->user, prev.handle);
    return slot;
}

/* RPCS3 render_target::clear_memory for a new (erase_bkgnd) zeta: depth 1.0,
 * stencil 255 over the whole surface, before its first draw or clear. */
static void eng_zdepth_init(u32 slot)
{
    eng_zdepth* z = &g.zdepths[slot];
    if (z->cleared || !z->handle) return;
    g.be->clear_depth_stencil(g.be->user, z->handle,
                              RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 1.0f, 0xFF);
    z->cleared = 1;
    z->had_write = 0;
}

/* A depth target read back as a texture, but only once the pass has executed
 * a depth-WRITING draw: a write-enable bit alone does not prove the pass
 * produced a usable depth map, and a clear-only zeta falls through to guest
 * memory instead (rsx_live_draw.c:6016-6018, 6265-6269). */
static u32 eng_zdepth_snapshot(u32 slot)
{
    eng_zdepth* z = &g.zdepths[slot];
    if (!z->handle || !z->had_write) return 0;
    if (z->snapshot_valid && z->snapshot &&
        (eng_legacy_surfaces() || z->snapshot_serial == z->depth_serial))
        return z->snapshot;
    if (!g.be->depth_snapshot) return 0;
    const u32 tex = g.be->depth_snapshot(g.be->user, z->handle, z->w, z->h);
    if (!tex) return 0;
    z->snapshot = tex;
    z->snapshot_valid = 1;
    z->snapshot_serial = z->depth_serial;
    return tex;
}

/* ---- textures ------------------------------------------------------------ */

static rsx_be_format eng_texfmt(rsx_texfmt f)
{
    switch (f) {
    case RSX_TEXFMT_R8:              return RSX_BE_FMT_R8;
    case RSX_TEXFMT_R8G8:            return RSX_BE_FMT_R8G8;
    case RSX_TEXFMT_R8G8B8A8:        return RSX_BE_FMT_R8G8B8A8;
    case RSX_TEXFMT_BC1:             return RSX_BE_FMT_BC1;
    case RSX_TEXFMT_BC2:             return RSX_BE_FMT_BC2;
    case RSX_TEXFMT_BC3:             return RSX_BE_FMT_BC3;
    case RSX_TEXFMT_R16:             return RSX_BE_FMT_R16;
    case RSX_TEXFMT_R16G16:          return RSX_BE_FMT_R16G16;
    case RSX_TEXFMT_R16G16F:         return RSX_BE_FMT_R16G16F;
    case RSX_TEXFMT_R16G16B16A16F:   return RSX_BE_FMT_R16G16B16A16F;
    case RSX_TEXFMT_R32F:            return RSX_BE_FMT_R32F;
    case RSX_TEXFMT_R32G32B32A32F:   return RSX_BE_FMT_R32G32B32A32F;
    default:                         return RSX_BE_FMT_R8;
    }
}

/* The layout format of a fragment texture: its format byte, with
 * RSX_TEX_FMT_HAS_BORDER for border texels (2D and cube images). */
static u32 eng_tex_lfmt(const rsx_dsp_texture* t)
{
    return t->format | ((t->border && t->dimension != 3u) ? RSX_TEX_FMT_HAS_BORDER : 0u);
}

/* How many guest bytes a texture occupies: the whole mip chain, times six for
 * a cube map. That is what the content hash has to cover, or a title
 * animating one face or one lower level keeps its stale upload. */
static u32 eng_texture_span(u32 fmt, u32 w, u32 h, u32 levels, u32 pitch, int cube)
{
    if (cube) return rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) * 6u;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    return lv[n - 1].offset + lv[n - 1].tl.face_bytes;
}

/* A 3D texture: each level is `depth` slices (halving with the level), one
 * face_bytes each -- consecutive for a linear image, one Morton block for a
 * swizzled one. */
static u32 eng_texture_span_3d(u32 fmt, u32 w, u32 h, u32 depth, u32 levels, u32 pitch)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    u32 total = 0, d = depth ? depth : 1u;
    for (u32 m = 0; m < n; m++) { total += lv[m].tl.face_bytes * d; if (d > 1) d >>= 1; }
    return total;
}

/* Slice z of a swizzled 3D level, de-interleaved into row-major rows of
 * w * bpp bytes, so the 2D decode can convert it as a linear image. */
static void eng_deswizzle_3d_slice(u8* dst, const u8* src, u32 w, u32 h, u32 d, u32 z, u32 bpp)
{
    /* RPCS3 copy_unmodified_block_swizzled (TextureUtils.cpp:258-271): 64-
     * and 128-bit texels are swizzled as 32-bit words.
     * RSX_ENG_SWZ3D_WORDS=0: whole-texel Morton, as before. */
    static int words = -1;
    if (words < 0) { const char* e = getenv("RSX_ENG_SWZ3D_WORDS"); words = !(e && e[0] == '0'); }
    if (words && bpp > 4u && !(bpp & 3u)) { w *= bpp / 4u; bpp = 4u; }
    u32 lw = 0, lh = 0, ld = 0;
    while ((1u << lw) < w) lw++;
    while ((1u << lh) < h) lh++;
    while ((1u << ld) < d) ld++;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
            memcpy(dst + ((size_t)y * w + x) * bpp,
                   src + (size_t)rsx_swizzle_offset_3d(x, y, z, lw, lh, ld) * bpp, bpp);
}

/* RPCS3 copy_unmodified_block_vtc (TextureUtils.cpp:281-338): a DXT volume
 * level is NVIDIA VTC-tiled, four slices interleaved block by block per
 * group, the last 1-3 slices interleaved likewise. Gathers slice z's
 * blocks (eb bytes each, wb x rb of them) back to back. */
static void eng_vtc_slice(u8* dst, const u8* src, u32 wb, u32 rb, u32 d, u32 z, u32 eb)
{
    const u32 n = wb * rb, d4 = d & ~3u;
    const u32 stride = z < d4 ? 4u : d - d4;
    const size_t base = (size_t)(z / 4u) * 4u * n + (z & 3u);
    for (u32 i = 0; i < n; i++)
        memcpy(dst + (size_t)i * eb, src + (base + (size_t)i * stride) * eb, eb);
}

/* One hash per cached texture per presented frame. Word-at-a-time FNV over
 * the whole span is deliberately cheap: this is a mutation detector, not a
 * content id (rsx_live_draw.c:2002-2026). */
static u64 eng_texture_content_hash_impl(u32 location, u32 offset, u32 span,
                                    int* readable)
{
    const u8* src = span ? eng_guest_ptr(NULL, location, offset, span) : NULL;
    if (!src) { *readable = 0; return 0; }
    u64 hash = 1469598103934665603ull;
    u32 i = 0;
    for (; i + 8 <= span; i += 8) {
        u64 word;
        memcpy(&word, src + i, sizeof(word));
        hash ^= word;
        hash *= 1099511628211ull;
    }
    for (; i < span; i++) { hash ^= src[i]; hash *= 1099511628211ull; }
    *readable = 1;
    return hash;
}

static u64 eng_texture_content_hash(u32 location, u32 offset, u32 span, int* readable)
{
    const u64 t0 = eng_prof_t0();
    const u64 r_ = eng_texture_content_hash_impl(location, offset, span, readable);
    eng_prof_add(0, t0);
    return r_;
}

/* The cheap per-frame change check: 64 bytes out of every 2 KB, plus the
 * last 64. A rewrite of the whole texture (a video frame, a UI redraw, a
 * streamed replacement) changes it at once; a small write between the
 * sampled blocks is left to the full hash, which every texture still gets
 * every ENG_TEXHASH_FULL_EVERY frames. */
#define ENG_TEXHASH_STRIDE      2048u
#define ENG_TEXHASH_FULL_EVERY  8u          /* a power of two */
#define ENG_TEXHASH_SMALL       16384u      /* at or below: full every frame */
static u64 eng_texture_sparse_hash(u32 location, u32 offset, u32 span,
                                   int* readable)
{
    const u8* src = span ? eng_guest_ptr(NULL, location, offset, span) : NULL;
    if (!src) { *readable = 0; return 0; }
    u64 hash = 1469598103934665603ull;
    for (u32 i = 0; i + 64 <= span; i += ENG_TEXHASH_STRIDE)
        for (u32 k = 0; k < 64; k += 8) {
            u64 word;
            memcpy(&word, src + i + k, sizeof(word));
            hash = (hash ^ word) * 1099511628211ull;
        }
    for (u32 i = span > 64 ? span - 64 : 0; i < span; i++)
        hash = (hash ^ src[i]) * 1099511628211ull;
    *readable = 1;
    return hash;
}

/* Whether a cached texture gets the full hash at this use: always when it
 * is small or under PS3RECOMP_ENG_TEXHASH_FULL=1 (the old behaviour, read
 * at engine init), otherwise once ENG_TEXHASH_FULL_EVERY frames have passed
 * since its last one. New entries start staggered by slot, so the full
 * hashes of a scene's textures spread over the frames instead of landing on
 * the same one. */
static int s_texhash_full_always;
static int eng_texture_full_rehash(u32 span, u32 last_full_frame)
{
    return s_texhash_full_always || span <= ENG_TEXHASH_SMALL ||
           (u32)(g.frames - last_full_frame) >= ENG_TEXHASH_FULL_EVERY;
}

static int eng_staging_reserve(u32 bytes)
{
    if (g.tex_staging_cap >= bytes) return 1;
    u8* n = (u8*)realloc(g.tex_staging, bytes);
    if (!n) return 0;
    g.tex_staging = n;
    g.tex_staging_cap = bytes;
    return 1;
}

/* Decode a guest texture out of guest memory and hand every face and level to
 * the backend. Returns the backend handle, or 0. */
static u32 eng_texture_upload_impl(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                              u32 levels, u32 pitch, int cube, u32 remap, u32 depth)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!nlv || !lv[0].tl.face_bytes) return 0;
    /* A volume: every slice of every level decoded and uploaded on its own.
     * inFAMOUS 2's occlusion volumes read an 8x8x8 cone LUT; uploaded as its
     * first slice and sampled in 2D it darkened whole boxes round every prop. */
    if (depth > 1 && !cube && g.be->texture_create_3d && g.be->texture_upload_3d &&
        (lv[0].tl.bytes_per_texel || lv[0].tl.compressed)) {
        const u32 span = eng_texture_span_3d(fmt, w, h, depth, levels, pitch);
        const u8* src = eng_guest_image(location, offset, span);
        if (!src) return 0;
        const u32 handle = g.be->texture_create_3d(g.be->user, eng_texfmt(lv[0].tl.fmt),
                                                   w, h, depth, nlv, remap, fmt & 0xFFu);
        if (!handle && lv[0].tl.compressed) goto flat;   /* no BC volumes: slice 0 as before */
        if (!handle) return 0;
        const u8* level_src = src;
        u32 d = depth;
        for (u32 m = 0; m < nlv; m++) {
            const rsx_tex_layout* tl = &lv[m].tl;
            const u32 lin_bytes = tl->swizzled ? tl->row_bytes * tl->rows : 0u;
            if (!eng_staging_reserve(tl->dst_row_bytes * tl->rows + lin_bytes)) {
                g.be->texture_release(g.be->user, handle);
                return 0;
            }
            /* TextureUtils.cpp:1073-1083 (no VTC decoding on Vulkan): a
             * power-of-two DXT volume is un-tiled; any other is planar. */
            const int vtc = tl->compressed && !(lv[m].w & (lv[m].w - 1u)) && !(lv[m].h & (lv[m].h - 1u));
            for (u32 z = 0; z < d; z++) {
                if (vtc) {
                    eng_vtc_slice(g.tex_staging, level_src, (lv[m].w + 3u) / 4u, tl->rows, d, z,
                                  tl->block_bytes);
                } else if (tl->swizzled) {
                    u8* lin = g.tex_staging + tl->dst_row_bytes * tl->rows;
                    eng_deswizzle_3d_slice(lin, level_src, lv[m].w, lv[m].h, d, z, tl->bytes_per_texel);
                    rsx_tex_layout ltl = *tl; ltl.swizzled = 0;
                    rsx_texture_decode(g.tex_staging, tl->dst_row_bytes, lin, lv[m].w, lv[m].h,
                                       &ltl, rsx_texture_argb_is_rgba());
                } else {
                    rsx_texture_decode(g.tex_staging, tl->dst_row_bytes,
                                       level_src + (size_t)z * tl->face_bytes, lv[m].w, lv[m].h,
                                       tl, rsx_texture_argb_is_rgba());
                }
                g.be->texture_upload_3d(g.be->user, handle, z, m, lv[m].w, lv[m].h,
                                        g.tex_staging, tl->dst_row_bytes);
            }
            level_src += (size_t)tl->face_bytes * d;
            if (d > 1) d >>= 1;
        }
        return handle;
    }
flat:;
    const u32 faces = cube ? 6u : 1u;
    const u32 face_stride = cube
        ? rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) : 0u;
    const u32 span = eng_texture_span(fmt, w, h, levels, pitch, cube);
    const u8* src = eng_guest_image(location, offset, span);
    if (!src) return 0;

    /* RPCS3 upload_texture: a linear, uncompressed texture whose packed row
     * is longer than its pitch register is uploaded pitch / bpp texels wide
     * (rows at the pitch) and sampled with scale.width *= packed / pitch, so
     * a texel column past the narrowed width reads through the S wrap mode.
     * Here the narrowed levels are decoded and widened back to the
     * registers' width, each column past it taking the texel that wrap mode
     * lands on (eng_tc_narrow_key put wrap_s + 1 in remap bits 16-19). */
    const u32 narrow_wrap = (remap >> 16) & 0xFu;
    remap &= 0xFFFFu;
    if (narrow_wrap && !cube && (fmt & 0x20u) && pitch && !lv[0].tl.compressed &&
        lv[0].tl.bytes_per_texel && pitch < w * lv[0].tl.bytes_per_texel) {
        const u32 nw = pitch / lv[0].tl.bytes_per_texel;
        rsx_tex_level nl[RSX_MAX_TEXTURE_LEVELS];
        const u32 nn = nw ? rsx_texture_mip_chain(fmt, nw, h, levels, pitch, nl) : 0u;
        if (!nn) return 0;                 /* RPCS3: invalid dimensions, no image */
        const u32 n = nn < nlv ? nn : nlv;
        const u32 handle = g.be->texture_create(g.be->user, eng_texfmt(lv[0].tl.fmt),
                                                w, h, n, 1u, remap, fmt & 0xFFu);
        if (!handle) return 0;
        for (u32 m = 0; m < n; m++) {
            const rsx_tex_layout* wl = &lv[m].tl;
            const rsx_tex_layout* tl = &nl[m].tl;
            const u32 wide = wl->dst_row_bytes * wl->rows;
            if (!eng_staging_reserve(wide + tl->dst_row_bytes * tl->rows)) {
                g.be->texture_release(g.be->user, handle);
                return 0;
            }
            u8* narrow = g.tex_staging + wide;
            rsx_texture_decode(narrow, tl->dst_row_bytes, src + nl[m].offset, nl[m].w, nl[m].h,
                               tl, rsx_texture_argb_is_rgba());
            const u32 db = nl[m].w ? tl->dst_row_bytes / nl[m].w : 0u;
            const u32 nwm = nl[m].w;
            for (u32 r = 0; r < wl->rows && r < tl->rows; r++) {
                u8* drow = g.tex_staging + (size_t)r * wl->dst_row_bytes;
                const u8* srow = narrow + (size_t)r * tl->dst_row_bytes;
                for (u32 x = 0; x < lv[m].w; x++) {
                    u32 sx = x;
                    if (x >= nwm) {
                        switch (narrow_wrap - 1u) {
                        case 1u: sx = x % nwm; break;                          /* wrap */
                        case 2u: { const u32 p = x % (2u * nwm);                 /* mirror */
                                   sx = p < nwm ? p : 2u * nwm - 1u - p; } break;
                        default: sx = nwm - 1u; break;  /* clamps, border, mirror-once (|u| > 1) */
                        }
                    }
                    memcpy(drow + (size_t)x * db, srow + (size_t)sx * db, db);
                }
            }
            g.be->texture_upload(g.be->user, handle, 0, m, lv[m].w, lv[m].h,
                                 g.tex_staging, wl->dst_row_bytes, wl->rows);
        }
        return handle;
    }

    const u32 handle = g.be->texture_create(g.be->user, eng_texfmt(lv[0].tl.fmt),
                                            w, h, nlv, faces, remap, fmt & 0xFFu);
    if (!handle) return 0;

    for (u32 f = 0; f < faces; f++) {
        const u8* face = src + (size_t)f * face_stride;
        for (u32 m = 0; m < nlv; m++) {
            const rsx_tex_layout* tl = &lv[m].tl;
            if (!eng_staging_reserve(tl->dst_row_bytes * tl->rows)) {
                g.be->texture_release(g.be->user, handle);
                return 0;
            }
            rsx_texture_decode(g.tex_staging, tl->dst_row_bytes,
                               face + lv[m].offset, lv[m].w, lv[m].h, tl,
                               rsx_texture_argb_is_rgba());
            /* RPCS3 upload_texture: a linear texture with pitch 0 is sampled
             * with texcoord scale {1, 0, 0} -- every row reads row 0. */
            if ((fmt & 0x20u) && !pitch && !tl->compressed && !rsx_texture_legacy())
                for (u32 r = 1; r < tl->rows; r++)
                    memcpy(g.tex_staging + (size_t)r * tl->dst_row_bytes, g.tex_staging,
                           tl->dst_row_bytes);
            g.be->texture_upload(g.be->user, handle, f, m, lv[m].w, lv[m].h,
                                 g.tex_staging, tl->dst_row_bytes, tl->rows);
        }
    }
    return handle;
}

static u32 eng_texture_upload(u32 location, u32 offset, u32 fmt, u32 w, u32 h, u32 levels, u32 pitch, int cube, u32 remap, u32 depth)
{
    const u64 t0 = eng_prof_t0();
    const u32 r_ = eng_texture_upload_impl(location, offset, fmt, w, h, levels, pitch, cube, remap, depth);
    eng_prof_add(1, t0);
    return r_;
}

/* RSX_TEXCACHE_LEGACY=1 (read at engine init): the pre-parity texture cache
 * coherence rules -- a failed upload never retried, SPU writes not tracked,
 * a surface bound whole whatever the texture's pitch, size or dimension, and
 * a texture inside a surface decoded from guest memory as it stands
 * (scratch/parity/texcache.md). */
static int s_texcache_legacy;

/* Guest writes the engine is told about (rsx_draw_engine_note_guest_write:
 * SPU DMA puts), by 4 KB page of the guest EA space -- the part of RPCS3's
 * page protection (a write fault invalidating every cached section on the
 * page, texture_cache invalidate_range_impl_base) the engine can see. A
 * cached texture whose pages were written since its last check is re-hashed
 * in full at that use, not at the next frame. A writer stores its pages
 * before it advances the epoch, so a check never misses one. */
#define ENG_TC_PAGE_SHIFT 12u
#define ENG_TC_PAGES      (1u << (32u - ENG_TC_PAGE_SHIFT))
static volatile u32 s_tc_page_gen[ENG_TC_PAGES];
static volatile long s_tc_epoch = 1;

void rsx_draw_engine_note_guest_write(u32 ea, u32 size)
{
    if (!size) return;
    const u32 gen = (u32)s_tc_epoch + 1u;
    const u32 p0 = ea >> ENG_TC_PAGE_SHIFT;
    const u32 p1 = (u32)(((u64)ea + size - 1u) >> ENG_TC_PAGE_SHIFT);
    for (u32 p = p0; p <= p1 && p < ENG_TC_PAGES; p++) s_tc_page_gen[p] = gen;
    /* Release: a reader that sees the new epoch must see the pages too (the
     * interlocked add is a full barrier on x86/MSVC, but say so for the
     * compiler and weaker targets). RSX_ENG_TC_FENCE=0 drops it. */
    static int fence = -1;
    if (fence < 0) { const char* e = getenv("RSX_ENG_TC_FENCE"); fence = !(e && *e == '0'); }
#ifdef _WIN32
    if (fence) MemoryBarrier();
    InterlockedIncrement(&s_tc_epoch);
#else
    if (fence) __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_add_fetch(&s_tc_epoch, 1, __ATOMIC_SEQ_CST);
#endif
}

/* Whether a page of [offset, +span) was written since *seen; *seen moves to
 * the current epoch. A page written in the epoch of the last check counts
 * again: at worst one more full hash. */
static int eng_tc_pages_written(u32 location, u32 offset, u32 span, u32* seen)
{
    const u32 now = (u32)s_tc_epoch, old = *seen;
    if (now == old || !span) return 0;
    *seen = now;
    const int local = location == RSX_LOCATION_LOCAL;
    const u32 a = cellGcmResolveLocated(local, offset);
    const u32 b = cellGcmResolveLocated(local, offset + span - 1u);
    if (a && b >= a && b - a == span - 1u) {
        for (u32 p = a >> ENG_TC_PAGE_SHIFT; p <= (b >> ENG_TC_PAGE_SHIFT); p++) {
            const u32 gen = s_tc_page_gen[p];
            if (gen && (int)(gen - old) >= 0) return 1;
        }
        return 0;
    }
    for (u64 o = offset & ~((1u << ENG_TC_PAGE_SHIFT) - 1u); o < (u64)offset + span;
         o += 1u << ENG_TC_PAGE_SHIFT) {
        const u32 ea = cellGcmResolveLocated(local, (u32)o);
        const u32 gen = ea ? s_tc_page_gen[ea >> ENG_TC_PAGE_SHIFT] : 0u;
        if (gen && (int)(gen - old) >= 0) return 1;
    }
    return 0;
}

/* RPCS3 cached_texture_section::protect(protection::ro): the pages of a
 * cached texture become read-only, so the guest's (PPU, SPU DMA, cellFs
 * read, runtime memcpy) first write to them faults, notes the write and opens
 * the page (eng_wcb_fault). Applied before the bytes are hashed, so a write
 * racing the upload is still seen. The content hash stays as the fallback for
 * writes no fault can see. RSX_TEXCACHE_PROTECT=0 (read at engine init)
 * turns it off. */
static int s_tc_protect = 1;
static int eng_tc_protect_on(void)
{
    return s_tc_protect && !s_texcache_legacy;
}

static void eng_tc_protect(u32 location, u32 offset, u32 span)
{
#ifdef _WIN32
    /* A guest page must be a host page, or protecting it catches its
     * neighbours' writes too. */
    if (!span || !vm_base || ((uintptr_t)vm_base & ((1u << PG_SHIFT) - 1u)) ||
        !eng_tc_protect_on() || !eng_wcb_install()) return;
    u32 ea = 0;
    if (eng_wcb_range(location, offset, span, &ea)) {
        pg_guard_range(ea, span, PG_OWNER_TEX, 1);
        return;
    }
    const int local = location == RSX_LOCATION_LOCAL;
    for (u64 o = offset & ~((1u << PG_SHIFT) - 1u); o < (u64)offset + span; o += 1u << PG_SHIFT) {
        const u32 a = cellGcmResolveLocated(local, (u32)o);
        if (a && (!ppu_vm_size || (u64)a + (1u << PG_SHIFT) <= ppu_vm_size))
            pg_guard_range(a & ~((1u << PG_SHIFT) - 1u), 1u << PG_SHIFT, PG_OWNER_TEX, 1);
    }
#else
    (void)location; (void)offset; (void)span;
#endif
}

/* Shutdown: every texture page back to what its other owners ask for. */
static void eng_tc_unprotect_all(void)
{
#ifdef _WIN32
    if (!vm_base) return;
    u32 run = 0, run_n = 0;
    for (u32 pn = 0; pn <= PG_COUNT; pn++) {
        if (pn < PG_COUNT && (s_pg_mask[pn] & PG_OWNER_TEX)) { if (!run_n) run = pn; run_n++; continue; }
        if (run_n) pg_guard_range(run << PG_SHIFT, run_n << PG_SHIFT, PG_OWNER_TEX, 0);
        run_n = 0;
    }
#endif
}

/* The cache slot for a texture unit's bytes, keyed on where they are and what
 * the registers say they are. An already-cached entry is re-hashed at most
 * once per presented frame and re-decoded when the guest changed it, which is
 * what makes an animated UI or a video texture update at all. A miss with the
 * cache full evicts the least recently used entry rather than returning
 * nothing: returning white "made the recovered orphanage render as flat
 * green/black geometry" (rsx_live_draw.c:2326-2334). */
static u32 eng_texture_slot(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                            u32 levels, u32 pitch, int cube, u32 remap, u32 depth)
{
    if (!w || !h || w > ENG_MAX_TEX_DIM || h > ENG_MAX_TEX_DIM) return 0;
    if (!depth) depth = 1;
    const u32 span = depth > 1 ? eng_texture_span_3d(fmt, w, h, depth, levels, pitch)
                               : eng_texture_span(fmt, w, h, levels, pitch, cube);
    if (!span) return 0;

    /* RSX_PERF_TEXHASH: a direct-mapped hint of where a key was last found.
     * Keys are unique in the table (an entry is only added after a miss), so
     * a hint whose entry still holds the full key is the entry the scan
     * would find; any other hint falls back to the scan. */
    static int perf_th = -1;
    const int th = eng_perf_on("RSX_PERF_TEXHASH", &perf_th);
    u32 hk = 0, i0 = 0;
    if (th) {
        u64 k = ((u64)location << 32 | offset) * 0x9E3779B97F4A7C15ull;
        k ^= ((u64)fmt << 40 | (u64)remap << 8 | (u64)(cube != 0)) * 0xC2B2AE3D27D4EB4Full;
        k ^= ((u64)w << 48 | (u64)h << 32 | (u64)pitch << 8 | depth) * 0x165667B19E3779F9ull;
        hk = (u32)(k >> 52);
        const u32 hi = s_tex_hint[hk];
        int hit = 0;
        if (hi && hi - 1u < g.n_textures) {
            const eng_texture* e = &g.textures[hi - 1u];
            if (e->location == location && e->offset == offset && e->format == fmt &&
                e->width == w && e->height == h && e->pitch == pitch && e->remap == remap &&
                e->cubemap == (u32)(cube != 0) && e->depth == depth) {
                i0 = hi - 1u;
                hit = 1;
            }
        }
        ENG_PC(PC_TEXSLOT, hit);
    }
    for (u32 i = i0; i < g.n_textures; i++) {
        eng_texture* e = &g.textures[i];
        if (e->location != location || e->offset != offset ||
            e->format != fmt || e->width != w || e->height != h ||
            e->pitch != pitch || e->remap != remap ||
            e->cubemap != (u32)(cube != 0) || e->depth != depth)
            continue;
        if (th) s_tex_hint[hk] = i + 1u;
        if (!s_texcache_legacy && e->handle &&
            eng_tc_pages_written(location, offset, span, &e->tc_seen)) {
            eng_tc_protect(location, offset, span);
            e->last_hash_frame = g.frames - 1u;
            e->last_full_frame = g.frames - ENG_TEXHASH_FULL_EVERY;
        }
        /* RPCS3 upload_texture looks the texture up again at every use: one
         * whose bytes could not be read or decoded (an IO range not mapped
         * yet) is tried again, once a frame, instead of staying null until
         * the entry is evicted. */
        if (!s_texcache_legacy && !e->handle && e->last_hash_frame != g.frames) {
            int readable = 0;
            e->last_hash_frame = g.frames;
            eng_tc_protect(location, offset, span);
            e->tc_seen = (u32)s_tc_epoch;
            const u64 hash = eng_texture_content_hash(location, offset, span, &readable);
            if (readable) {
                e->handle = eng_texture_upload(location, offset, fmt, w, h, levels, pitch,
                                               cube, remap, depth);
                if (e->handle) {
                    e->content_hash = hash;
                    e->sparse_hash = eng_texture_sparse_hash(location, offset, span, &readable);
                    e->last_full_frame = g.frames;
                }
            }
        }
        if (e->handle && e->last_hash_frame != g.frames) {
            /* Every cached texture used to be hashed in full every frame;
             * in a big scene that is hundreds of MB a second for the few
             * that change. Now the sparse check every frame, and the full
             * hash when it differs, when this entry's turn has come, for a
             * small texture, or after a write the engine saw. */
            int readable = 0;
            u64 hash = e->content_hash;
            const u64 sparse = eng_texture_sparse_hash(location, offset, span,
                                                       &readable);
            if (readable && (sparse != e->sparse_hash ||
                             eng_texture_full_rehash(span, e->last_full_frame))) {
                hash = eng_texture_content_hash(location, offset, span, &readable);
                e->last_full_frame = g.frames;
            }
            if (readable) e->sparse_hash = sparse;
            e->last_hash_frame = g.frames;
            if (readable && hash != e->content_hash) {
                const u32 fresh = eng_texture_upload(location, offset, fmt, w, h,
                                                     levels, pitch, cube, remap, depth);
                if (fresh) {
                    g.be->texture_release(g.be->user, e->handle);
                    e->handle = fresh;
                    e->content_hash = hash;
                }
            }
        }
        e->last_use_serial = ++g.texture_use_serial;
        return e->handle;
    }

    u32 index;
    u32 evicted = 0;
    if (g.n_textures < ENG_MAX_TEXTURES) {
        index = g.n_textures++;
    } else {
        index = 0;
        for (u32 i = 1; i < g.n_textures; i++)
            if (g.textures[i].last_use_serial < g.textures[index].last_use_serial)
                index = i;
        evicted = g.textures[index].handle;
    }

    eng_texture e;
    memset(&e, 0, sizeof(e));
    e.location = location; e.offset = offset; e.format = fmt;
    e.width = w; e.height = h; e.pitch = pitch;
    e.remap = remap; e.cubemap = (u32)(cube != 0); e.depth = depth;
    e.last_hash_frame = g.frames;
    e.last_full_frame = g.frames - (index & (ENG_TEXHASH_FULL_EVERY - 1u));
    e.last_use_serial = ++g.texture_use_serial;
    eng_tc_protect(location, offset, span);
    e.tc_seen = (u32)s_tc_epoch;
    { int readable = 0;
      e.content_hash = eng_texture_content_hash(location, offset, span, &readable);
      e.sparse_hash = eng_texture_sparse_hash(location, offset, span, &readable); }
    e.handle = eng_texture_upload(location, offset, fmt, w, h, levels, pitch,
                                  cube, remap, depth);
    if (e.handle && evicted) g.be->texture_release(g.be->user, evicted);
    if (e.handle || !evicted) {
        g.textures[index] = e;
        if (th) s_tex_hint[hk] = index + 1u;
    }
    return e.handle;
}

/* ---- surfaces read through guest memory --------------------------------- */

/* A texture may name a surface's bytes in a format the surface was not
 * rendered in: a deferred renderer reads its D24S8 depth buffer as A8R8G8B8
 * to rebuild positions, samples its Z16 shadow map as DEPTH16, and reads back
 * as X32_FLOAT a float it packed into an RGBA8 target. On RSX that is only
 * memory. Here the surface is stored into guest memory in its own layout --
 * the bytes RSX would have left there -- and the texture is then decoded from
 * guest memory like any other, so every format the decoder knows reads them
 * the way RSX would. A surface is stored at most once per write to it. */

/* Re-hash, on their next use rather than next frame, the cached textures
 * whose bytes [offset, offset + bytes) changed: those overlapping it, or with
 * RSX_ENG_LEGACY_SURFACES=1 only those starting inside it. */
static void eng_textures_touch(u32 location, u32 offset, u32 bytes)
{
    const int legacy = eng_legacy_surfaces();
    for (u32 i = 0; i < g.n_textures; i++) {
        eng_texture* e = &g.textures[i];
        if (e->location != location) continue;
        int hit;
        if (legacy) {
            hit = e->offset >= offset && e->offset - offset < bytes;
        } else {
            /* RPCS3 invalidates every cached section the written range
             * overlaps, not only those starting inside it. Swizzled
             * textures carry no pitch: bound them at 4 bytes a texel. */
            const u64 row = e->pitch ? e->pitch : (u64)e->width * 4u;
            u64 span = row * (e->height ? e->height : 1u) * (e->depth ? e->depth : 1u);
            if (e->cubemap) span *= 6u;
            hit = (u64)e->offset < (u64)offset + bytes && (u64)offset < (u64)e->offset + span;
        }
        if (hit) {
            e->last_hash_frame = g.frames - 1u;
            e->last_full_frame = g.frames - ENG_TEXHASH_FULL_EVERY;   /* hash it all */
        }
    }
}

/* Formats that read an RGBA8 surface's 32-bit pixels as something else: the
 * host image cannot be sampled as them, the guest bytes can. */
static int eng_format_reinterprets_rgba8(u32 base_fmt)
{
    switch (base_fmt) {
    case RSX_TEX_FMT_DEPTH24_D8:
    case 0x91u:     /* DEPTH24_D8_FLOAT */
    case 0x92u:     /* DEPTH16          */
    case 0x93u:     /* DEPTH16_FLOAT    */
    case 0x95u:     /* Y16_X16          */
    case 0x9Cu:     /* X32_FLOAT        */
    case 0x9Fu:     /* Y16_X16_FLOAT    */
        return 1;
    default:
        return 0;
    }
}

/* An FP16 RGBA, FP32 RGBA or FP32 R target into guest memory at its pitch:
 * each half or float big-endian, R first -- the bytes RSX leaves and RPCS3's
 * flush writes (its copy swaps 2- or 4-byte words for these formats; the
 * layout rsx_texture_decode reads W16_Z16_Y16_X16_FLOAT, W32_Z32_Y32_X32_FLOAT
 * and X32_FLOAT from). */
static int eng_surface_store_float(const eng_surface* s)
{
    const u32 bpp = eng_color_bytes(s);
    const u32 word = s->fmt == RSX_BE_FMT_R16G16B16A16F ? 2u : 4u;
    if (!s->handle || !s->w || !s->h || s->pitch < s->w * bpp || !g.be->readback) return 0;
    const u32 row = s->w * bpp;
    u8* rows = eng_coherence_buffer(row * s->h);
    if (!rows) return 0;
    g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    g.be->readback(g.be->user, s->handle, 0, 0, s->w, s->h, rows, row);
    for (u32 r = 0; r < s->h; r++) {
        u8* src = rows + (size_t)r * row;
        for (u32 c = 0; c < row; c += word) {
            if (word == 2u) { const u8 t = src[c]; src[c] = src[c + 1]; src[c + 1] = t; }
            else {
                u32 v; memcpy(&v, src + c, 4);
                v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
                memcpy(src + c, &v, 4);
            }
        }
        if (!eng_guest_tiled_copy(s->location, s->offset + r * eng_tiled_row_pitch(s->location, s->offset, s->pitch), src, row, 1)) return 0;
    }
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] float surface %u:0x%08X %ux%u (%u bpp) stored to guest memory\n",
                s->location, s->offset, s->w, s->h, bpp);
    return 1;
}

/* RPCS3 stores an RGBA8-host colour surface in its own guest layout
 * (VKGSRender.cpp get_compatible_surface_format host formats, flushed with
 * get_compatible_gcm_format's swap: every multibyte format byte-swapped, B8
 * not). Our host lanes hold what RPCS3's host image would: R5G6B5/A1R5G5B5
 * as R,G,B(,A); B8 in R (the .bbbb remap); G8B8 as R=B, G=G (.bgbg); ABGR8
 * as R,G,B,A (R8G8B8A8). RSX_ENG_WCB_PACKED=0 keeps them unstored. */
static int eng_wcb_packed(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_ENG_WCB_PACKED"); v = !(e && e[0] == '0'); }
    return v && !s_texcache_legacy;
}

static int eng_surface_packed_kind(const eng_surface* s)
{
    if (!eng_wcb_packed() || s->swz || s->fmt != RSX_BE_FMT_R8G8B8A8) return 0;
    const u32 k = eng_color_tkind(s);
    return k == RSX_BE_TYPELESS_ARGB1555 || k == RSX_BE_TYPELESS_RGB565 || k == RSX_BE_TYPELESS_B8 ||
           k == RSX_BE_TYPELESS_G8B8 || k == RSX_BE_TYPELESS_ABGR8;
}

static u32 eng_unorm_q(u32 v8, u32 maxv) { return (v8 * maxv + 127u) / 255u; }

static int eng_surface_store_packed(const eng_surface* s)
{
    const u32 k = eng_color_tkind(s), bpp = eng_color_bytes(s);
    if (!s->handle || !s->w || !s->h || s->pitch < s->w * bpp || !g.be->readback) return 0;
    const u32 hrow = s->w * 4u, grow = s->w * bpp;
    u8* rows = eng_coherence_buffer(hrow * s->h);
    if (!rows) return 0;
    g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    g.be->readback(g.be->user, s->handle, 0, 0, s->w, s->h, rows, hrow);
    for (u32 r = 0; r < s->h; r++) {
        u8* src = rows + (size_t)r * hrow;
        /* In place: each output texel is no wider than its input texel. */
        for (u32 c = 0; c < s->w; c++) {
            const u8* p = src + (size_t)c * 4u;
            u8* d = src + (size_t)c * bpp;
            const u32 R = p[0], G = p[1], B = p[2], A = p[3];
            u32 v;
            switch (k) {
            case RSX_BE_TYPELESS_RGB565:
                v = eng_unorm_q(R, 31u) << 11 | eng_unorm_q(G, 63u) << 5 | eng_unorm_q(B, 31u);
                d[0] = (u8)(v >> 8); d[1] = (u8)v; break;
            case RSX_BE_TYPELESS_ARGB1555:
                v = (A >= 128u ? 0x8000u : 0u) | eng_unorm_q(R, 31u) << 10 | eng_unorm_q(G, 31u) << 5 |
                    eng_unorm_q(B, 31u);
                d[0] = (u8)(v >> 8); d[1] = (u8)v; break;
            case RSX_BE_TYPELESS_B8:
                d[0] = (u8)R; break;
            case RSX_BE_TYPELESS_G8B8:
                d[0] = (u8)G; d[1] = (u8)R; break;
            default: /* ABGR8: R8G8B8A8 texel byte-swapped -> A,B,G,R */
                d[0] = (u8)A; d[1] = (u8)B; d[2] = (u8)G; d[3] = (u8)R; break;
            }
        }
        if (!eng_guest_tiled_copy(s->location, s->offset + r * eng_tiled_row_pitch(s->location, s->offset, s->pitch), src, grow, 1)) return 0;
    }
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] packed surface %u:0x%08X %ux%u (cfmt 0x%X, %u bpp) stored to guest memory\n",
                s->location, s->offset, s->w, s->h, s->cfmt, bpp);
    return 1;
}

/* Store a colour surface into guest memory, A,R,G,B per pixel at its own
 * pitch. Returns 0 when it cannot, and the caller samples as before. */
static int eng_surface_to_guest_impl(u32 slot);
static int eng_surface_to_guest(u32 slot)
{
    const eng_surface* s = &g.surfaces[slot];
    if (!s->guest_dirty) return 1;
    eng_flush_scope_begin(s->location, s->offset, (u64)s->pitch * s->h, s->color_serial, 0, slot);
    const int r_ = eng_surface_to_guest_impl(slot);
    eng_flush_scope_end();
    return r_;
}

static int eng_surface_to_guest_impl(u32 slot)
{
    eng_surface* s = &g.surfaces[slot];
    if (!s->guest_dirty) return 1;
    if (s->fmt != RSX_BE_FMT_R8G8B8A8) {
        /* RPCS3 flushes every colour format it renders; RSX_TEXCACHE_LEGACY=1
         * stores A8R8G8B8 only. */
        if (s_texcache_legacy || s->swz ||
            (s->fmt != RSX_BE_FMT_R16G16B16A16F && s->fmt != RSX_BE_FMT_R32G32B32A32F &&
             s->fmt != RSX_BE_FMT_R32F) ||
            s->pitch < s->w * eng_color_bytes(s) || !g.be->readback)
            return 0;
        /* With the sudo view the store lands while the pages are still
         * guarded and the guard goes after it (RPCS3 flush, then unprotect);
         * without it the guard has to go first. */
        if (!eng_sudo_on()) eng_wcb_unguard(slot);
        const int ok = eng_surface_store_float(s);
        eng_wcb_unguard(slot);
        if (!ok) return 0;
        s->guest_dirty = 0;
        eng_surface_tag(slot);
        eng_textures_touch(s->location, s->offset, s->pitch * s->h);
        return 1;
    }
    if (eng_surface_packed_kind(s)) {
        if (!eng_sudo_on()) eng_wcb_unguard(slot);
        const int ok = eng_surface_store_packed(s);
        eng_wcb_unguard(slot);
        if (!ok) return 0;
        s->guest_dirty = 0;
        eng_surface_tag(slot);
        eng_textures_touch(s->location, s->offset, s->pitch * s->h);
        return 1;
    }
    if (s->pitch < s->w * 4u || !g.be->readback)
        return 0;
    if (!eng_sudo_on()) eng_wcb_unguard(slot);
    if (s->swz) {
        /* Stored once, then memory is the copy: the slot is parked (see
         * eng_swz_drop) so nothing samples a GPU image the CPU may overwrite. */
        const int ok = eng_swz_to_guest(slot);
        eng_wcb_unguard(slot);
        if (!ok) return 0;
        s->guest_dirty = 0;
        eng_textures_touch(s->location, s->offset, s->w * s->h * 4u);
        s->offset = ENG_SWZ_DEAD;
        return 1;
    }
    rsx_draw_engine_guest_read(s->location, s->offset, s->pitch, s->w, s->h);
    eng_wcb_unguard(slot);
    s->guest_dirty = 0;
    eng_surface_tag(slot);
    eng_textures_touch(s->location, s->offset, s->pitch * s->h);
    return 1;
}

/* Store the surfaces guarding guest page `pn` and drop the WCB guard there;
 * engine thread only. */
static void eng_wcb_flush_page(u32 pn)
{
#ifdef _WIN32
    ENG_SCOPE();
    const u32 page = pn << PG_SHIFT;
    for (u32 i = 0; g.ready && i < g.n_surfaces && i < ENG_MAX_SURFACES; i++) {
        if (!s_wcb_rng[i].len || (u64)page >= (u64)s_wcb_rng[i].ea + s_wcb_rng[i].len ||
            (u64)s_wcb_rng[i].ea >= (u64)page + (1u << PG_SHIFT))
            continue;
        const eng_surface* s = &g.surfaces[i];
        u32 ea = 0;
        const int same = s->handle && eng_wcb_range(s->location, s->offset, s->pitch * s->h, &ea) &&
                         ea == s_wcb_rng[i].ea;
        if (same && s->guest_dirty && !eng_surface_is_display_buffer(s->location, s->offset)) {
            static u32 logs = 0;
            if (PS3_LOG_FIRST(logs, 8))
                fprintf(stderr, "[rsx engine] WCB: guest touched 0x%08X; storing surface 0x%08X %ux%u\n",
                        page, s->offset, s->w, s->h);
            eng_surface_to_guest(i);
        }
        eng_wcb_unguard(i);
    }
    if (s_pg_mask[pn] & PG_OWNER_WCB)          /* nothing left claims it */
        pg_guard_range(page, 1u << PG_SHIFT, PG_OWNER_WCB, 0);
#else
    (void)pn;
#endif
}

/* WCB: store the surfaces whose guarded pages a guest thread faulted on
 * (eng_wcb_fault), on the engine's thread, and wake the waiters. */
void rsx_draw_engine_wcb_service(void)
{
#ifdef _WIN32
    if (!s_wcb_nreq || GetCurrentThreadId() != s_wcb_tid) return;
    u32 req[WCB_MAX_REQ], n;
    AcquireSRWLockExclusive(&s_wcb_lock);
    n = (u32)s_wcb_nreq;
    memcpy(req, s_wcb_req, n * sizeof req[0]);
    ReleaseSRWLockExclusive(&s_wcb_lock);
    for (u32 k = 0; k < n; k++) eng_wcb_flush_page(req[k]);
    AcquireSRWLockExclusive(&s_wcb_lock);
    /* Requests posted while this ran stay queued for the next pass. */
    const u32 left = (u32)s_wcb_nreq - n;
    memmove(s_wcb_req, s_wcb_req + n, left * sizeof s_wcb_req[0]);
    s_wcb_nreq = (LONG)left;
    ReleaseSRWLockExclusive(&s_wcb_lock);
    WakeAllConditionVariable(&s_wcb_cv);
#endif
}

static float* s_depth_rows;
static u32 s_depth_cap;

/* Store a depth target into guest memory as its zeta format lays it out:
 * Z16 as one big-endian 16-bit value, Z24S8 as a big-endian word with the
 * depth in the top 24 bits and the stencil byte below it (0 when the backend
 * cannot read stencil back). Returns 0 when it cannot. */
static int eng_zdepth_to_guest_impl(u32 slot)
{
    eng_zdepth* z = &g.zdepths[slot];
    if (!z->guest_dirty) return 1;
    if (!g.be->depth_readback || !z->handle) return 0;
    const u32 bpp = (z->zfmt == 1u) ? 2u : 4u;      /* SURFACE_FORMAT Z16 = 1 */
    const u32 w = z->used_w < z->w ? z->used_w : z->w;
    const u32 h = z->used_h < z->h ? z->used_h : z->h;
    if (!w || !h || z->pitch < w * bpp) return 0;
    if (s_depth_cap < w * h) {
        float* n = (float*)realloc(s_depth_rows, (size_t)w * h * sizeof(float));
        if (!n) return 0;
        s_depth_rows = n;
        s_depth_cap = w * h;
    }
    g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    /* RPCS3 stores D24S8 with its stencil byte; a backend without the
     * stencil read (or a stencil-less host image) stores 0 there. */
    static u8* s_stencil_rows;
    static u32 s_stencil_cap;
    static u32* s_raw_rows;
    static u32 s_raw_cap;
    int have_stencil = 0;
    u32 raw_bits = 0;
    static int perf = -1;
    if (g.be->zeta_readback && eng_perf_on("RSX_PERF_ZREADBACK", &perf)) {
        /* One submission for depth and stencil, and the host's raw UNORM
         * value when it has one (RSX_PERF_ZREADBACK=0: the two reads). */
        if (s_raw_cap < w * h) {
            u32* n = (u32*)realloc(s_raw_rows, (size_t)w * h * sizeof(u32));
            if (n) { s_raw_rows = n; s_raw_cap = w * h; }
        }
        if (bpp == 4u && s_stencil_cap < w * h) {
            u8* n = (u8*)realloc(s_stencil_rows, (size_t)w * h);
            if (n) { s_stencil_rows = n; s_stencil_cap = w * h; }
        }
        u8* st_out = (bpp == 4u && s_stencil_cap >= w * h) ? s_stencil_rows : NULL;
        u32* raw_out = s_raw_cap >= w * h ? s_raw_rows : NULL;
        int hs = 0;
        if (g.be->zeta_readback(g.be->user, z->handle, 0, 0, w, h, s_depth_rows,
                                raw_out, &raw_bits, st_out, &hs) != 0)
            return 0;
        if (!raw_out) raw_bits = 0;
        have_stencil = st_out && hs;
    } else {
        if (g.be->depth_readback(g.be->user, z->handle, 0, 0, w, h, s_depth_rows,
                                 w * (u32)sizeof(float)) != 0)
            return 0;
        if (bpp == 4u && g.be->stencil_readback) {
            if (s_stencil_cap < w * h) {
                u8* n = (u8*)realloc(s_stencil_rows, (size_t)w * h);
                if (n) { s_stencil_rows = n; s_stencil_cap = w * h; }
            }
            if (s_stencil_cap >= w * h)
                have_stencil = g.be->stencil_readback(g.be->user, z->handle, 0, 0, w, h,
                                                      s_stencil_rows, w) == 0;
        }
    }
    /* A raw UNORM value stores as itself when the zeta is the same width:
     * v -> float (as depth_readback divides) -> quantized back is the
     * identity for every 16- and 24-bit v (checked exhaustively). Any other
     * pairing goes through the floats depth_readback would have returned. */
    const int raw_direct = !z->depth_float &&
        ((bpp == 4u && raw_bits == 24u) || (bpp == 2u && raw_bits == 16u));
    if (raw_bits && !raw_direct) {
        const size_t n = (size_t)w * h;
        if (raw_bits == 16u)
            for (size_t i = 0; i < n; i++) s_depth_rows[i] = (float)(s_raw_rows[i] & 0xFFFFu) / 65535.0f;
        else
            for (size_t i = 0; i < n; i++)
                s_depth_rows[i] = (float)((double)(s_raw_rows[i] & 0xFFFFFFu) / 16777215.0);
    }
    u8* zrow = eng_coherence_buffer(w * bpp);
    if (!zrow) return 0;
    for (u32 r = 0; r < h; r++) {
        u8* dst = zrow;
        const u8* srow = have_stencil ? s_stencil_rows + (size_t)r * w : NULL;
        if (raw_direct) {
            const u32* src = s_raw_rows + (size_t)r * w;
            if (bpp == 2u) {
                for (u32 c = 0; c < w; c++) {
                    const u32 v = src[c];
                    dst[2u * c] = (u8)(v >> 8); dst[2u * c + 1u] = (u8)v;
                }
            } else {
                for (u32 c = 0; c < w; c++) {
                    const u32 v = ((src[c] & 0xFFFFFFu) << 8) | (srow ? srow[c] : 0u);
                    dst[4u * c] = (u8)(v >> 24); dst[4u * c + 1u] = (u8)(v >> 16);
                    dst[4u * c + 2u] = (u8)(v >> 8); dst[4u * c + 3u] = (u8)v;
                }
            }
            if (!eng_guest_tiled_copy(z->location, z->offset + r * eng_tiled_row_pitch(z->location, z->offset, z->pitch), zrow, w * bpp, 1)) return 0;
            continue;
        }
        const float* src = s_depth_rows + (size_t)r * w;
        for (u32 c = 0; c < w; c++) {
            const u8 st = srow ? srow[c] : 0u;
            float d = src[c];
            if (!(d > 0.0f)) d = 0.0f;
            if (d > 1.0f) d = 1.0f;
            if (z->depth_float) {
                if (bpp == 2u) {
                    const u32 v = eng_f32_to_e4m12(d);
                    dst[0] = (u8)(v >> 8); dst[1] = (u8)v;
                } else {
                    const u32 v = eng_f32_to_f24(d);
                    dst[0] = (u8)(v >> 16); dst[1] = (u8)(v >> 8); dst[2] = (u8)v; dst[3] = st;
                }
            } else if (bpp == 2u) {
                const u32 v = (u32)(d * 65535.0f + 0.5f);
                dst[0] = (u8)(v >> 8); dst[1] = (u8)v;
            } else {
                /* Float32 rounding at this magnitude can turn depth 1 into
                 * 0x1000000. Quantize in double before storing the 24-bit value. */
                const u32 v = (u32)((double)d * 16777215.0 + 0.5);
                dst[0] = (u8)(v >> 16); dst[1] = (u8)(v >> 8); dst[2] = (u8)v; dst[3] = st;
            }
            dst += bpp;
        }
        if (!eng_guest_tiled_copy(z->location, z->offset + r * eng_tiled_row_pitch(z->location, z->offset, z->pitch), zrow, w * bpp, 1)) return 0;
    }
    z->guest_dirty = 0;
    eng_zdepth_tag(slot);
    eng_textures_touch(z->location, z->offset, z->pitch * h);
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] depth 0x%08X (%s, pitch %u) %ux%u stored to guest memory\n",
                z->offset, bpp == 2u ? "Z16" : "Z24S8", z->pitch, w, h);
    return 1;
}

static int eng_zdepth_to_guest_at(u32 slot, int site)
{
    const u64 t0 = eng_prof_t0();
    const eng_zdepth* zz = &g.zdepths[slot];
    const int dirty = zz->guest_dirty;
    if (dirty) {
        const u32 zh = zz->used_h && zz->used_h < zz->h ? zz->used_h : zz->h;
        eng_flush_scope_begin(zz->location, zz->offset, (u64)zz->pitch * zh, zz->depth_serial, 1, slot);
    }
    const int r_ = eng_zdepth_to_guest_impl(slot);
    if (dirty) eng_flush_scope_end();
    if (dirty && s_prof_on > 0)
        for (u32 k = 0; k < 8; k++)
            if (!s_zsite[k].site || s_zsite[k].site == site) { s_zsite[k].site = site; s_zsite[k].n++; break; }
    eng_prof_add(5, t0);
    return r_;
}

static int eng_ranges_overlap(u32 a, u32 alen, u32 b, u32 blen)
{
    return (u64)a < (u64)b + blen && (u64)b < (u64)a + alen;
}

/* RSX_CFG_LEGACY=1: undo the rpcs3_config.yml parity rules (config.md). */
static int eng_cfg_legacy(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_CFG_LEGACY"); v = e && *e && *e != '0'; }
    return v;
}

/* Colour surfaces overlapping [offset, +length) go to guest memory; depth
 * too only when `depth`. */
static void eng_range_read(u32 location, u32 offset, u32 length, int depth);

/* A transfer (NV0039, a CPU-side reader) is about to read [offset, +length):
 * every rendered colour surface that overlaps it is stored to guest memory
 * first. RPCS3 "Write Depth Buffer: false" never locks depth in the texture
 * cache (VKGSRender.cpp:2571/2639/2691) and read_barrier only syncs zcull
 * (nv0039.cpp:106), so a CPU-side read of a depth range sees guest memory
 * as it was. RSX_CFG_LEGACY=1 stores depth surfaces as well. */
void rsx_draw_engine_range_read(u32 location, u32 offset, u32 length)
{
    ENG_SCOPE();
    eng_range_read(location, offset, length, eng_cfg_legacy());
}

void rsx_draw_engine_range_read_color(u32 location, u32 offset, u32 length)
{
    ENG_SCOPE();
    eng_range_read(location, offset, length, 0);
}

static void eng_range_read(u32 location, u32 offset, u32 length, int depth)
{
    if (!g.ready || !length) return;
    eng_flq q; q.n = 0;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* s = &g.surfaces[i];
        if (!s->handle || !s->guest_dirty || s->location != location ||
            eng_surface_is_display_buffer(s->location, s->offset))
            continue;
        if (eng_ranges_overlap(offset, length, s->offset, s->pitch * s->h))
            eng_flq_add(&q, i, 0);
    }
    for (u32 i = 0; i < g.n_zdepths && depth; i++) {
        const eng_zdepth* z = &g.zdepths[i];
        if (!z->handle || !z->guest_dirty || z->location != location) continue;
        const u32 h = z->used_h < z->h ? z->used_h : z->h;
        if (eng_ranges_overlap(offset, length, z->offset, z->pitch * h))
            eng_flq_add(&q, i, 1);
    }
    eng_flq_run(&q, __LINE__);
}

/* The texture half of range_wrote (see the header). */
void rsx_draw_engine_textures_wrote(u32 location, u32 offset, u32 length)
{
    ENG_SCOPE();
    if (!g.ready || !length) return;
    if (!eng_legacy_surfaces()) eng_textures_touch(location, offset, length);
    eng_swz_drop(location, offset, length);
}

/* RSX_ENG_DMA_RELOAD=1: a transfer into a 1x A8R8G8B8 colour surface reloads
 * the rows it wrote from guest memory and leaves every other surface as it
 * was (pre-parity). Default: RPCS3's. */
static int eng_dma_reload(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_ENG_DMA_RELOAD"); v = e && e[0] == '1'; }
    return v || eng_legacy_surfaces() || eng_cfg_legacy();
}

/* A transfer is about to write [offset, +length): RPCS3's write fault on the
 * Write Color Buffers sections there (texture_cache.h invalidate_range_impl_base,
 * cause write: flush_set, then unprotect) -- the dirty colour surfaces it
 * overlaps reach guest memory first, oldest first, so the bytes the transfer
 * leaves alone keep the rendered pixels. */
void rsx_draw_engine_range_will_write(u32 location, u32 offset, u32 length)
{
    ENG_SCOPE();
    if (!g.ready || !length || eng_dma_reload()) return;
    eng_range_read(location, offset, length, 0);
}

static void eng_range_reload(u32 location, u32 offset, u32 length);

/* A transfer wrote [offset, +length) of guest memory. RPCS3 write_barrier ->
 * surface_store::invalidate_range (surface_store.h:1572): every colour and
 * depth surface overlapping it is erase_bkgnd'ed, and with Read Color/Depth
 * Buffers off its next use clear_memory()s it (VKRenderTargets.cpp:811) to
 * (0,0,0,1), or depth 1.0 / stencil 255, as the newest write there. Guest
 * memory keeps the transfer's bytes. */
void rsx_draw_engine_range_wrote(u32 location, u32 offset, u32 length)
{
    ENG_SCOPE();
    if (!g.ready || !length) return;
    if (!eng_legacy_surfaces()) eng_textures_touch(location, offset, length);
    eng_swz_drop(location, offset, length);
    if (!eng_dma_reload()) {
        static const float erase[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        for (u32 i = 0; i < g.n_surfaces; i++) {
            eng_surface* s = &g.surfaces[i];
            if (!s->handle || s->swz || s->location != location || !s->pitch) continue;
            if (!eng_ranges_overlap(offset, length, s->offset, s->pitch * s->h)) continue;
            g.be->clear_color(g.be->user, s->handle, erase);
            if (s->region) { g.be->texture_release(g.be->user, s->region); s->region = 0; }
            if (s->feedback) { g.be->texture_release(g.be->user, s->feedback); s->feedback = 0; }
            if (s->reint) { g.be->texture_release(g.be->user, s->reint); s->reint = 0; }
            s->guest_dirty = 0;
            eng_wcb_unguard(i);
            s->color_serial = ++g.write_serial;
            eng_surface_tag(i);
        }
        for (u32 i = 0; i < g.n_zdepths; i++) {
            eng_zdepth* z = &g.zdepths[i];
            if (!z->handle || z->location != location || !z->pitch) continue;
            if (!eng_ranges_overlap(offset, length, z->offset, z->pitch * z->h)) continue;
            g.be->clear_depth_stencil(g.be->user, z->handle,
                                      RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 1.0f, 0xFF);
            z->cleared = 1;
            z->snapshot_valid = 0;
            z->guest_dirty = 0;
            z->depth_serial = ++g.write_serial;
            eng_zdepth_tag(i);
        }
        return;
    }
    eng_range_reload(location, offset, length);
}

/* The CPU side of the RSX (NV3089's software swizzle) wrote [offset, +length)
 * of guest memory. RPCS3 has no write_barrier there: the write fault flushed
 * and unprotected the Write Color Buffers sections (range_will_write), the
 * surfaces keep their pixels, and their memory tags no longer match, so
 * textures read the new bytes from memory. */
void rsx_draw_engine_range_cpu_wrote(u32 location, u32 offset, u32 length)
{
    ENG_SCOPE();
    if (!g.ready || !length) return;
    if (eng_dma_reload()) { rsx_draw_engine_range_wrote(location, offset, length); return; }
    eng_textures_touch(location, offset, length);
    eng_swz_drop(location, offset, length);
    for (u32 i = 0; i < g.n_surfaces; i++) {
        eng_surface* s = &g.surfaces[i];
        if (!s->handle || s->swz || s->location != location || !s->pitch) continue;
        if (!eng_ranges_overlap(offset, length, s->offset, s->pitch * s->h)) continue;
        s->guest_dirty = 0;
        eng_wcb_unguard(i);
    }
}

static void eng_range_reload(u32 location, u32 offset, u32 length)
{
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* s = &g.surfaces[i];
        if (!s->handle || s->location != location || !s->pitch) continue;
        /* An antialiased surface holds samples, not the guest's pixel rows. */
        if (s->aa_sx > 1 || s->aa_sy > 1) continue;
        const u32 span = s->pitch * s->h;
        if (!eng_ranges_overlap(offset, length, s->offset, span)) continue;
        const u32 lo = offset > s->offset ? offset - s->offset : 0;
        const u64 end = (u64)offset + length - s->offset;
        const u32 hi = end < span ? (u32)end : span;
        const u32 r0 = lo / s->pitch, r1 = (hi - 1u) / s->pitch + 1u;
        const u32 w = s->pitch / 4u < s->w ? s->pitch / 4u : s->w;
        rsx_draw_engine_guest_wrote(location, s->offset + r0 * s->pitch, s->pitch, w, r1 - r0);
    }
}

/* RPCS3 create_blit_dst (texture_cache.h:1676): an NV3089 copy out of a
 * render target into LOCAL memory no surface holds makes a destination
 * surface there instead of going through guest memory.
 * RSX_ENG_BLIT_CREATE_TARGETS=0: only a depth source does, as before. */
static int eng_blit_create_targets(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_ENG_BLIT_CREATE_TARGETS"); v = !(e && e[0] == '0'); }
    return v;
}

/* RSX_ENG_BLIT_CREATE_16=0: no R5G6B5 blit destination is created (as before). */
static int eng_blit_dst16(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_ENG_BLIT_CREATE_16"); v = !(e && e[0] == '0'); }
    return v;
}

/* RSX_ENG_BLIT_PITCH_TRUNC=0: a created destination is never shortened. */
static int eng_blit_pitch_trunc(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_ENG_BLIT_PITCH_TRUNC"); v = !(e && e[0] == '0'); }
    return v;
}

/* RSX_ENG_BLIT_FORCE_LOAD=0: guest bytes seed a created destination only
 * when nothing was inherited, after the inheritance (as before). */
static int eng_blit_force_load(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_ENG_BLIT_FORCE_LOAD"); v = !(e && e[0] == '0'); }
    return v;
}

/* Rows of a new blit target at `offset` before the first surface of another
 * pitch starting inside it that is newer than whatever sits at `offset`
 * (surface_store.h truncate_memory_range_by_pitch); never under min_rows. */
static u32 eng_blit_trunc_rows(u32 location, u32 offset, u32 pitch, u32 rows, u32 min_rows)
{
    u64 ref = 0;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* o = &g.surfaces[i];
        if (o->handle && o->location == location && o->offset == offset && o->color_serial > ref)
            ref = o->color_serial;
    }
    for (u32 i = 0; i < g.n_zdepths; i++) {
        const eng_zdepth* z = &g.zdepths[i];
        if (z->handle && z->location == location && z->offset == offset && z->depth_serial > ref)
            ref = z->depth_serial;
    }
    u64 limit = (u64)offset + (u64)pitch * rows;
    for (u32 k = 0; k < g.n_surfaces + g.n_zdepths; k++) {
        u32 lo, op; u64 tag;
        if (k < g.n_surfaces) {
            const eng_surface* o = &g.surfaces[k];
            if (!o->handle || o->location != location) continue;
            lo = o->offset; op = o->pitch; tag = o->color_serial;
        } else {
            const eng_zdepth* z = &g.zdepths[k - g.n_surfaces];
            if (!z->handle || z->location != location) continue;
            lo = z->offset; op = z->pitch; tag = z->depth_serial;
        }
        if (lo <= offset || lo >= limit || op == pitch || tag <= ref) continue;
        limit = lo;
    }
    u32 r = (u32)((limit - offset) / pitch);
    return r < min_rows ? min_rows : r;
}

/* The Z24S8 zeta holding guest bytes [offset, ...) at `pitch`, written,
 * newest first -- an A8R8G8B8 blit source (eng_surface_at's rules for a depth
 * surface). An antialiased zeta's memory is laid out in samples, as its
 * target is, so the guest coordinates address samples directly (RPCS3's
 * rtt_lookup accepts any AA mode). RSX_ENG_BLIT_ZETA_AA=0: single-sample
 * only, as before. */
static u32 eng_zdepth_at(u32 location, u32 offset, u32 pitch, u32* xb, u32* y)
{
    static int aa_ok = -1;
    if (aa_ok < 0) { const char* e = getenv("RSX_ENG_BLIT_ZETA_AA"); aa_ok = !(e && e[0] == '0'); }
    u32 best = ENG_INVALID;
    for (u32 i = 0; i < g.n_zdepths; i++) {
        const eng_zdepth* z = &g.zdepths[i];
        if (!z->handle || z->location != location || offset < z->offset || !z->depth_serial ||
            z->pitch != pitch || eng_zeta_zf(z) != 2u ||
            (!aa_ok && (z->aa_sx > 1u || z->aa_sy > 1u)))
            continue;
        const u32 d = offset - z->offset;
        const u32 zw = z->used_w ? z->used_w : z->w, zh = z->used_h ? z->used_h : z->h;
        if (d / pitch >= zh || d % pitch >= zw * 4u) continue;
        if (best != ENG_INVALID && g.zdepths[best].depth_serial >= z->depth_serial) continue;
        best = i;
        *xb = d % pitch;
        *y = d / pitch;
    }
    return best;
}

/* RSX_ENG_BLIT_DEPTH_SRC=0: an NV3089 copy out of a depth surface goes
 * through guest memory, as before. */
static int eng_blit_depth_src(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_ENG_BLIT_DEPTH_SRC"); v = !(e && e[0] == '0'); }
    return v;
}

int rsx_draw_engine_blit(u32 src_location, u32 src_offset, u32 src_pitch,
                         u32 src_x, u32 src_y, u32 ds_dx, u32 dt_dy,
                         u32 dst_location, u32 dst_offset, u32 dst_pitch,
                         u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h, int linear, u32 in_wh)
{
    ENG_SCOPE();
    return rsx_draw_engine_blit_bpp(src_location, src_offset, src_pitch, src_x, src_y,
                                    ds_dx, dt_dy, dst_location, dst_offset, dst_pitch,
                                    dst_x, dst_y, dst_w, dst_h, linear, in_wh, 4u);
}

/* `bpp` 4: A8R8G8B8 on both sides; 2: R5G6B5 on both sides (RPCS3 copies
 * either between render targets; a conversion stays on the CPU). */
int rsx_draw_engine_blit_bpp(u32 src_location, u32 src_offset, u32 src_pitch,
                             u32 src_x, u32 src_y, u32 ds_dx, u32 dt_dy,
                             u32 dst_location, u32 dst_offset, u32 dst_pitch,
                             u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h, int linear, u32 in_wh,
                             u32 bpp)
{
    ENG_SCOPE();
    return rsx_draw_engine_blit_flip(src_location, src_offset, src_pitch, src_x, src_y,
                                     ds_dx, dt_dy, dst_location, dst_offset, dst_pitch,
                                     dst_x, dst_y, dst_w, dst_h, linear, in_wh, bpp, 0u);
}

/* RSX_ENG_BLIT_MIRROR=0: a mirrored NV3089 transfer stays on the CPU. */
static int eng_blit_mirror(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_ENG_BLIT_MIRROR"); v = !(e && e[0] == '0'); }
    return v;
}

int rsx_draw_engine_blit_flip(u32 src_location, u32 src_offset, u32 src_pitch,
                              u32 src_x, u32 src_y, u32 ds_dx, u32 dt_dy,
                              u32 dst_location, u32 dst_offset, u32 dst_pitch,
                              u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h, int linear, u32 in_wh,
                              u32 bpp, u32 flip)
{
    ENG_SCOPE();
    if (!g.ready || eng_legacy_surfaces() || !g.be->color_target_blit) return 0;
    flip &= 3u;
    if (flip && (!eng_blit_mirror() || !g.be->color_target_blit_flip)) return 0;
    if (!src_pitch || !dst_pitch || !dst_w || !dst_h || !ds_dx || !dt_dy) return 0;
    if (bpp != 4u && bpp != 2u) return 0;
    const u64 sw64 = ((u64)dst_w * ds_dx) >> 20, sh64 = ((u64)dst_h * dt_dy) >> 20;
    u32 sw = sw64 ? (u32)(sw64 > 0xFFFFu ? 0xFFFFu : sw64) : 1u;
    u32 sh = sh64 ? (u32)(sh64 > 0xFFFFu ? 0xFFFFu : sh64) : 1u;
    /* RPCS3 texture_cache.cpp:262-276: "Transfer cropped" -- the source
     * extent stops at the image-in size; the destination is unchanged. */
    if (in_wh && !rsx_texture_legacy()) {
        const u32 iw = in_wh & 0xFFFFu, ih = in_wh >> 16;
        if ((iw && src_x >= iw) || (ih && src_y >= ih)) return 0;
        if (iw && !(flip & 1u) && src_x + sw > iw) sw = iw - src_x;
        if (ih && !(flip & 2u) && src_y + sh > ih) sh = ih - src_y;
    }
    /* RPCS3 texture_cache.cpp:277-287 (apply_pixel_transforms): a mirrored
     * axis reads the src_h (src_w) texels ending at the start texel, then
     * the blit flips that area. The span must stay inside the image. */
    if (flip & 1u) { if (src_x + 1u < sw) return 0; src_x = src_x + 1u - sw; }
    if (flip & 2u) { if (src_y + 1u < sh) return 0; src_y = src_y + 1u - sh; }

    u32 sxb = 0, sy = 0;
    const u64 sa = (u64)src_offset + (u64)src_y * src_pitch + (u64)src_x * bpp;
    if (sa > 0xFFFFFFFFull) return 0;
    u32 si = eng_surface_at(src_location, (u32)sa, src_pitch, &sxb, &sy);
    /* RPCS3 upload_scaled_image: rtt_lookup finds depth surfaces too, and a
     * Z24S8 source is read typeless as A8R8G8B8 (configure_typeless_format),
     * then scaled into the destination. With "Write Depth Buffer: false"
     * guest memory never receives that depth, so a CPU copy reads stale
     * bytes (R2's quarter-res particle zeta, blitted out of the scene's). */
    u32 zi = ENG_INVALID, zxb = 0, zy = 0;
    if (bpp == 4u && eng_blit_depth_src() && g.be->surface_typeless_copy &&
        g.be->color_target_create && g.be->color_target_release) {
        zi = eng_zdepth_at(src_location, (u32)sa, src_pitch, &zxb, &zy);
        if (zi != ENG_INVALID && si != ENG_INVALID &&
            g.surfaces[si].color_serial > g.zdepths[zi].depth_serial)
            zi = ENG_INVALID;
    }
    const eng_surface* s = NULL;
    u32 sx = 0;
    if (zi != ENG_INVALID) {
        si = ENG_INVALID;
        if (zxb & 3u) return 0;
        sx = zxb / 4u; sy = zy;
        const eng_zdepth* z = &g.zdepths[zi];
        if (sx + sw > z->w || sy + sh > z->h) return 0;
    } else {
        if (si == ENG_INVALID) return 0;
        s = &g.surfaces[si];
        if (s->fmt != RSX_BE_FMT_R8G8B8A8 || (sxb % bpp) || !s->color_serial || s->swz ||
            eng_color_bytes(s) != bpp || (bpp == 2u && s->cfmt != 3u))
            return 0;
        sx = sxb / bpp;
        if (sx + sw > s->w || sy + sh > s->h) return 0;
    }

    u32 dxb = 0, dy = 0;
    const u64 da = (u64)dst_offset + (u64)dst_y * dst_pitch + (u64)dst_x * bpp;
    if (da > 0xFFFFFFFFull) return 0;
    u32 di = eng_surface_at(dst_location, (u32)da, dst_pitch, &dxb, &dy);
    if (di == ENG_INVALID) {
        /* RPCS3 create_blit_dst for a render-target source into local
         * memory: a surface at the destination block's base, pitch / bpp
         * wide (get_optimal_blit_target_properties), as tall as the write --
         * at least the source's height when as wide as it, at least 720 rows
         * when 1280 or 2560 wide -- with the rectangle at (dst_x, dst_y). */
        const int b16 = bpp == 2u && s && eng_blit_dst16() &&
            eng_host_color_format(RSX_BE_FMT_R8G8B8A8, 3u, dst_location) == RSX_BE_FMT_R5G6B5;
        if ((!eng_blit_create_targets() && zi == ENG_INVALID) || (bpp != 4u && !b16) ||
            dst_location != RSX_LOCATION_LOCAL || dst_x + dst_w > dst_pitch / bpp)
            return 0;
        u32 tw = dst_pitch / bpp, th = dst_y + dst_h;
        if (tw > 4096u) tw = dst_x + dst_w;
        const u32 srcw = s ? s->w : g.zdepths[zi].w, srch = s ? s->h : g.zdepths[zi].h;
        /* No R5G6B5 guest upload: a 16-bit target only when the write covers it. */
        if (b16 && (dst_x || dst_y || dst_w != tw)) return 0;
        if (!b16 && (tw == 1280u || tw == 2560u)) { if (th < 720u) th = 720u; }
        else if (!b16 && tw == srcw && th < srch) th = srch;
        /* RPCS3 truncate_memory_range_by_pitch (texture_cache.h:1700-1718):
         * stop above a newer surface of another pitch. */
        if (eng_blit_pitch_trunc() && th > dst_y + dst_h)
            th = eng_blit_trunc_rows(dst_location, dst_offset, dst_pitch, th, dst_y + dst_h);
        /* Whatever the guest left around the rectangle seeds the target. */
        rsx_draw_engine_range_read(dst_location, dst_offset, dst_pitch * th);
        const int partial = dst_x || dst_y || dst_w != tw || dst_h != th;
        s_eng_rt_keep = si != ENG_INVALID ? 1ull << si : 0;
        s_eng_new_cfmt = b16 ? 3u : 0u;
        s_eng_force_load = partial && !b16 && eng_blit_force_load();
        di = eng_surface_get(dst_location, dst_offset, tw, th,
                             RSX_BE_FMT_R8G8B8A8, dst_pitch, 1, 1);
        s_eng_rt_keep = 0;
        s_eng_new_cfmt = 0;
        s_eng_force_load = 0;
        if (di == ENG_INVALID || di == si) return 0;
        eng_surface* n = &g.surfaces[di];
        if (n->fmt != RSX_BE_FMT_R8G8B8A8) return 0;
        n->pitch = dst_pitch;
        if (!n->cfmt) n->cfmt = 8u;     /* A8R8G8B8 */
        if (!n->color_serial && !b16)
            rsx_draw_engine_guest_wrote(dst_location, dst_offset, dst_pitch, tw, th);
        dxb = dst_x * bpp; dy = dst_y;
        if (si != ENG_INVALID) s = &g.surfaces[si];
    }
    if (di == si) return 0;
    eng_surface* d = &g.surfaces[di];
    if (d->fmt != RSX_BE_FMT_R8G8B8A8 || (dxb % bpp) || d->swz || eng_color_bytes(d) != bpp ||
        (bpp == 2u && d->cfmt != 3u))
        return 0;
    const u32 dx = dxb / bpp;
    if (dx + dst_w > d->w || dy + dst_h > d->h) return 0;

    u32 src_handle = s ? s->handle : 0, tmp = 0;
    if (zi != ENG_INVALID) {
        /* Typeless first, at the source's size, then the scaled copy. */
        tmp = g.be->color_target_create(g.be->user, RSX_BE_FMT_R8G8B8A8, sw, sh, NULL, 0);
        if (!tmp) return 0;
        const eng_zdepth* z = &g.zdepths[zi];
        if (!g.be->surface_typeless_copy(g.be->user, z->handle,
                                         eng_zeta_tkind(2u, z->depth_float), sx, sy, sw, sh,
                                         tmp, RSX_BE_TYPELESS_ARGB8, 0, 0, sw, sh)) {
            g.be->color_target_release(g.be->user, tmp);
            return 0;
        }
        src_handle = tmp;
        sx = 0; sy = 0;
    }
    if (flip)
        g.be->color_target_blit_flip(g.be->user, src_handle, sx, sy, sw, sh,
                                     d->handle, dx, dy, dst_w, dst_h, linear, flip);
    else if (g.be->color_target_blit_filtered)
        g.be->color_target_blit_filtered(g.be->user, src_handle, sx, sy, sw, sh,
                                         d->handle, dx, dy, dst_w, dst_h, linear);
    else
        g.be->color_target_blit(g.be->user, src_handle, sx, sy, sw, sh,
                                d->handle, dx, dy, dst_w, dst_h);
    if (tmp) g.be->color_target_release(g.be->user, tmp);
    d->color_serial = ++g.write_serial;
    d->guest_dirty = 1;
    eng_wcb_guard(di);
    eng_surface_tag(di);
    if (d->region) { g.be->texture_release(g.be->user, d->region); d->region = 0; }
    eng_textures_touch(dst_location, (u32)da, dst_pitch * dst_h);
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] blit %u:0x%08X%s %u,%u %ux%u -> %u:0x%08X %u,%u %ux%u on the GPU%s\n",
                src_location, s ? s->offset : g.zdepths[zi].offset, s ? "" : " (zeta)",
                sx, sy, sw, sh, dst_location, d->offset,
                dx, dy, dst_w, dst_h, linear ? " (bilinear)" : "");
    return 1;
}

/* RPCS3 upload_scaled_image into a swizzled (NV309E) destination: the copy
 * stays on the GPU in a target standing for the destination (eng_swz_*), and
 * guest memory receives the Morton-ordered bytes only when something reads
 * them -- a texture that is not exactly that image, a transfer, or a CPU
 * access (WCB). Taken only when the source surface is newer than guest
 * memory, i.e. when the CPU copy would have had to wait for the GPU. */
int rsx_draw_engine_blit_swizzled(u32 src_location, u32 src_offset, u32 src_pitch,
                                  u32 src_x, u32 src_y, u32 ds_dx, u32 dt_dy,
                                  u32 dst_location, u32 dst_offset,
                                  u32 dst_w, u32 dst_h, int linear)
{
    ENG_SCOPE();
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_ENG_SWIZZLE_BLIT"); en = !(e && e[0] == '0'); }
    if (!en || !g.ready || eng_legacy_surfaces() || !g.be->color_target_blit ||
        !g.be->readback)
        return 0;
    if (!src_pitch || !dst_w || !dst_h || !ds_dx || !dt_dy ||
        (dst_w & (dst_w - 1u)) || (dst_h & (dst_h - 1u)) ||
        dst_w > ENG_MAX_SURFACE_DIM || dst_h > ENG_MAX_SURFACE_DIM)
        return 0;
    const u64 sw64 = ((u64)dst_w * ds_dx) >> 20, sh64 = ((u64)dst_h * dt_dy) >> 20;
    const u32 sw = sw64 ? (u32)(sw64 > 0xFFFFu ? 0xFFFFu : sw64) : 1u;
    const u32 sh = sh64 ? (u32)(sh64 > 0xFFFFu ? 0xFFFFu : sh64) : 1u;
    u32 sxb = 0, sy = 0;
    const u64 sa = (u64)src_offset + (u64)src_y * src_pitch + (u64)src_x * 4u;
    if (sa > 0xFFFFFFFFull) return 0;
    const u32 si = eng_surface_at(src_location, (u32)sa, src_pitch, &sxb, &sy);
    if (si == ENG_INVALID) return 0;
    const eng_surface* s = &g.surfaces[si];
    if (s->fmt != RSX_BE_FMT_R8G8B8A8 || (sxb & 3u) || !s->color_serial ||
        !s->guest_dirty || s->swz || s->aa_sx > 1u || s->aa_sy > 1u)
        return 0;
    const u32 sx = sxb / 4u;
    if (sx + sw > s->w || sy + sh > s->h) return 0;

    const u32 bytes = dst_w * dst_h * 4u;
    u32 di = ENG_INVALID, spare = ENG_INVALID;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* o = &g.surfaces[i];
        if (!o->handle) continue;
        if (o->swz && o->offset == ENG_SWZ_DEAD) {
            if (spare == ENG_INVALID) spare = i;
            continue;
        }
        if (o->location != dst_location) continue;
        const u32 orow = o->pitch > o->w * 4u ? o->pitch : o->w * 4u;
        const u32 ospan = o->swz ? o->w * o->h * 4u : orow * o->h;
        if (!eng_ranges_overlap(dst_offset, bytes, o->offset, ospan)) continue;
        if (o->swz && o->offset == dst_offset && o->w == dst_w && o->h == dst_h) {
            di = i;
            continue;
        }
        if (!o->swz) return 0;      /* a render target lives there */
        eng_surface_to_guest(i);    /* another swizzled image: keep its bytes */
    }
    if (di == ENG_INVALID) {
        if (spare != ENG_INVALID) {
            g.surfaces[spare].location = dst_location;
            g.surfaces[spare].offset = dst_offset;
        }
        s_eng_rt_keep = 1ull << si;
        di = eng_surface_get(dst_location, dst_offset, dst_w, dst_h,
                             RSX_BE_FMT_R8G8B8A8, 0, 1, 1);
        s_eng_rt_keep = 0;
    }
    if (di == ENG_INVALID || di == si) {
        if (spare != ENG_INVALID && g.surfaces[spare].swz &&
            g.surfaces[spare].offset == dst_offset)
            g.surfaces[spare].offset = ENG_SWZ_DEAD;
        return 0;
    }
    eng_surface* d = &g.surfaces[di];
    if (d->fmt != RSX_BE_FMT_R8G8B8A8 || d->w != dst_w || d->h != dst_h ||
        d->aa_sx != 1u || d->aa_sy != 1u) {
        if (d->swz) d->offset = ENG_SWZ_DEAD;
        return 0;
    }
    d->pitch = dst_w * 4u;
    d->cfmt = 8u;                   /* A8R8G8B8 */
    d->swz = 1;
    s_eng_swz_used = 1;
    if (g.be->color_target_blit_filtered)
        g.be->color_target_blit_filtered(g.be->user, s->handle, sx, sy, sw, sh,
                                         d->handle, 0, 0, dst_w, dst_h, linear);
    else
        g.be->color_target_blit(g.be->user, s->handle, sx, sy, sw, sh,
                                d->handle, 0, 0, dst_w, dst_h);
    d->color_serial = ++g.write_serial;
    d->guest_dirty = 1;
    if (d->region) { g.be->texture_release(g.be->user, d->region); d->region = 0; }
    eng_wcb_guard(di);
    eng_textures_touch(dst_location, dst_offset, bytes);
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] swizzled blit %u:0x%08X %u,%u %ux%u -> %u:0x%08X %ux%u on the GPU%s\n",
                src_location, s->offset, sx, sy, sw, sh, dst_location, dst_offset,
                dst_w, dst_h, linear ? " (bilinear)" : "");
    return 1;
}

/* A depth write or clear through the current zeta, recorded with what the
 * guest-memory copy needs to lay it out. */
static void eng_zdepth_wrote(u32 slot, const rsx_dsp_surface* sf)
{
    eng_zdepth* z = &g.zdepths[slot];
    z->depth_serial = ++g.write_serial;
    z->guest_dirty = 1;
    z->zfmt = sf->depth_format;
    z->pitch = sf->zeta_pitch;
    z->used_w = sf->clip_w ? sf->clip_w : g.width;
    z->used_h = sf->clip_h ? sf->clip_h : g.height;
    /* RPCS3 prepare_render_target: the zeta takes layout.width, the clip
     * after the pitch fit, as the colour targets do. */
    if (s_fbc.have && s_fbc.fbl.width) {
        const u32 fw = s_fbc.fbl.width * (sf->aa_sx ? sf->aa_sx : 1u);
        if (fw < z->used_w) z->used_w = fw;
    }
    eng_zdepth_tag(slot);
}

static void eng_surfaces_wrote(const u32* targets, u32 n_targets)
{
    const u64 serial = ++g.write_serial;
    for (u32 i = 0; i < n_targets; i++) {
        g.surfaces[targets[i]].color_serial = serial;
        g.surfaces[targets[i]].guest_dirty = 1;
        g.surfaces[targets[i]].swz = 0;     /* drawn into: a render target now */
        eng_wcb_guard(targets[i]);
        eng_surface_tag(targets[i]);
    }
}

/* SET_TEXTURE_FILTER's min field is [18:16] (1 NEAREST, 2 LINEAR, then 3..6,
 * the four combinations of a nearest/linear minification with a
 * nearest/linear mip filter) and mag [26:24]; SET_TEXTURE_CONTROL0 carries
 * max LOD at [18:7] and min LOD at [30:19], both 4.8 fixed point, and the
 * LOD bias is SET_TEXTURE_FILTER [12:0], signed 4.8 (RPCS3
 * fragment_texture::bias, clamped to [-16, 16)). */
static void eng_decode_sampler(u32 filter, u32 wrap, u32 control0, u32 border,
                               rsx_be_sampler_desc* out)
{
    const u32 minf = (filter >> 16) & 7u;
    const u32 magf = (filter >> 24) & 7u;
    /* RPCS3 VKFormats.cpp get_min_filter/get_mag_filter: CONVOLUTION_MIN (7)
     * is LINEAR with no mip sampling, CONVOLUTION_MAG (4) is LINEAR. */
    out->min_linear  = (u8)(minf == 2 || minf == 4 || minf == 6 ||
                            (minf == 7 && !rsx_texture_legacy()));
    out->mag_linear  = (u8)(magf == 2 || (magf == 4 && !rsx_texture_legacy()));
    out->mip_present = (u8)(minf >= 3 && (minf != 7 || rsx_texture_legacy()));
    out->mip_linear  = (u8)(minf == 5 || minf == 6);
    out->wrap_s = (u8)(wrap & 0xFu);
    out->wrap_t = (u8)((wrap >> 8) & 0xFu);
    out->wrap_r = (u8)((wrap >> 16) & 0xFu);
    out->min_lod = (float)((control0 >> 19) & 0xFFFu) / 256.0f;
    out->max_lod = out->mip_present ? (float)((control0 >> 7) & 0xFFFu) / 256.0f
                                    : 0.0f;
    if (out->max_lod < out->min_lod) out->max_lod = out->min_lod;
    {
        const int b13 = (int)(filter & 0x1FFFu);
        float bias = (float)((b13 ^ 0x1000) - 0x1000) / 256.0f;
        if (bias > 16.0f - 1.0f / 256.0f) bias = 16.0f - 1.0f / 256.0f;
        out->lod_bias = bias;
    }
    out->border_color = border;
    out->border_rgba[0] = (float)((border >> 16) & 0xFFu) / 255.0f;
    out->border_rgba[1] = (float)((border >> 8) & 0xFFu) / 255.0f;
    out->border_rgba[2] = (float)(border & 0xFFu) / 255.0f;
    out->border_rgba[3] = (float)(border >> 24) / 255.0f;
    {
        static const u8 levels[8] = { 1, 2, 4, 6, 8, 10, 12, 16 };
        static int ovr = -1;
        if (ovr < 0) { const char* e = getenv("RSX_ANISO"); ovr = e ? atoi(e) : 0; }
        out->max_aniso = ovr > 0 ? (u8)(ovr > 16 ? 16 : ovr) : levels[(control0 >> 4) & 7u];
    }
}

/* RPCS3 VKGSRender::load_texture_env sampler setup: LOD range and bias apply
 * only when the min filter samples mips AND the image really has more than
 * one (fragment_texture::get_exact_mipmap_count); then both bounds clamp to
 * the last level. Otherwise min/max LOD and bias are zero and the mip mode
 * nearest -- a negative bias on a single-level image must not switch the
 * minify/magnify decision. RSX_TEX_LEGACY=1 keeps the raw values. */
static void eng_sampler_clamp_mips(rsx_be_sampler_desc* s, u32 format, u32 w, u32 h,
                                   u32 mipmaps)
{
    if (rsx_texture_legacy()) return;
    const u32 base = format & 0x9Fu;
    u32 dim = (base >= 0x86u && base <= 0x88u)
            ? ((w / 4u < h / 4u) ? w / 4u : h / 4u)
            : (w > h ? w : h);
    u32 maxc = 1;
    while (dim > 1u) { dim >>= 1; maxc++; }
    if (!mipmaps) mipmaps = 1;
    const u32 count = mipmaps < maxc ? mipmaps : maxc;
    if (s->mip_present && count > 1u) {
        const float last = (float)(count - 1u);
        if (s->min_lod > last) s->min_lod = last;
        if (s->max_lod > last) s->max_lod = last;
    } else {
        s->min_lod = s->max_lod = 0.0f;
        s->lod_bias = 0.0f;
        s->mip_linear = 0;
    }
}

/* RPCS3 hardware texel remapping (VKDraw.cpp:365-395, hw_SNORM_possible in
 * TextureUtils.cpp:856-859, get_host_format_snorm_mask 1337-1357,
 * VKFormats.cpp get_compatible_snorm_format 268-289): a shader_read texture
 * not fetched with _bx2 whose signed lanes are exactly the host format's
 * SNORM-able lanes is sampled through an SNORM view, its SEXT bits erased
 * -- so it keeps the game's filter (VKDraw.cpp:447-452 only point-samples
 * while the shader still sign-extends). Only host formats with an SNORM twin
 * (R8, R8G8, RGBA8, R16G16) qualify; a render-target read is not
 * shader_read. RSX_TEX_HW_SNORM=0 or RSX_TEX_LEGACY=1 keeps the shader path. */
static u32 s_eng_hw_snorm_units;
/* RPCS3 "MSAA: Auto" (VKGSRender backend_config): with hardware MSAA every
 * pipeline does alpha-to-coverage (and alpha-to-one when the device can) in
 * the multisample state, so the shader emulation stands down. */
static u32 eng_hw_aa_caps(void)
{
    static const rsx_draw_backend* for_be;
    static u32 caps;
    if (for_be != g.be) {
        for_be = g.be;
        caps = (g.be && g.be->target_set_aa) ? g.be->target_set_aa(g.be->user, 0, 1, 1) : 0u;
    }
    return caps;
}
static int eng_tex_hw_snorm(const rsx_dsp_texture* t, u32 bx2_units, u32 unit)
{
    static int env = -1;
    if (env < 0) { const char* e = getenv("RSX_TEX_HW_SNORM"); env = !(e && *e == '0'); }
    if (!env || rsx_texture_legacy() || !g.be->texture_snorm_view) return 0;
    if ((bx2_units >> unit) & 1u) return 0;
    if (!t->width || !t->height || (t->dimension == 3 && !t->cubemap)) return 0;
    u32 need;
    switch (t->format & 0x9Fu) {
    case 0x81: case 0x9E: case 0x8D: case 0x8E: need = 0x7u; break;   /* alpha hardcoded */
    case 0x85: case 0x8B: case 0x95:            need = 0xFu; break;
    default: return 0;
    }
    if ((rsx_fp_texture_sext_mask(t->format, t->remap, t->filter) & 0xFu) != need) return 0;
    rsx_tex_layout L;
    rsx_texture_layout(t->format, t->width, t->height, &L);
    if (L.fmt != RSX_TEXFMT_R8 && L.fmt != RSX_TEXFMT_R8G8 &&
        L.fmt != RSX_TEXFMT_R8G8B8A8 && L.fmt != RSX_TEXFMT_R16G16) return 0;
    /* B8R8_G8R8 / R8B8_R8G8 never read a render target (eng_tex_format_skips_rtt). */
    if ((t->format & 0x9Fu) == 0x8Du || (t->format & 0x9Fu) == 0x8Eu) return 1;
    if (eng_surface_newest(t->location, t->offset, NULL, 0) != ENG_INVALID) return 0;
    for (u32 i = 0; i < g.n_zdepths; i++)
        if (g.zdepths[i].location == t->location && g.zdepths[i].offset == t->offset &&
            g.zdepths[i].depth_serial)
            return 0;
    return 1;
}

/* RPCS3 hw_SRGB_possible (TextureUtils.cpp:861-865, VKDraw.cpp:383-388), the
 * else-branch of the SNORM view: a shader_read texture not fetched with _bx2
 * whose CONTROL1 is exactly the identity (remap() == 0xAAE4, the whole
 * register) and whose gamma lanes, less the signed ones, are exactly R|G|B
 * is sampled through an sRGB view and its gamma bits erased -- hardware
 * decode before filtering. Any other mix (R only, RG, RGBA, a crossbar) keeps
 * the shader's post-filter decode, as in RPCS3. Only formats RPCS3 samples
 * from R8/R8G8/B8G8R8A8/BC1-3 have an sRGB twin (the packed 16-bit ones do
 * not). RSX_TEX_HW_SRGB=0, RSX_CFG_LEGACY=1 or RSX_TEX_LEGACY=1: shader. */
static u32 s_eng_hw_srgb_units;
static int eng_tex_hw_srgb(const rsx_dsp_texture* t, u32 bx2_units, u32 unit)
{
    static int env = -1;
    if (env < 0) { const char* e = getenv("RSX_TEX_HW_SRGB"); env = !(e && *e == '0'); }
    if (!env || eng_cfg_legacy() || rsx_texture_legacy() || !g.be->texture_srgb_view) return 0;
    if ((bx2_units >> unit) & 1u) return 0;
    if (!t->width || !t->height || t->remap != 0xAAE4u) return 0;
    switch (t->format & 0x9Fu) {
    case 0x81: case 0x85: case 0x86: case 0x87: case 0x88: case 0x8B:
    case 0x8D: case 0x8E: case 0x9E: break;
    default: return 0;
    }
    if (rsx_texture_gamma_mask(t->format, t->remap, t->filter, t->wrap) != 0x7u) return 0;
    rsx_tex_layout L;
    rsx_texture_layout(t->format, t->width, t->height, &L);
    if (!g.be->texture_srgb_view(g.be->user, 0, eng_texfmt(L.fmt))) return 0;
    if ((t->format & 0x9Fu) == 0x8Du || (t->format & 0x9Fu) == 0x8Eu) return 1;
    if (eng_surface_newest(t->location, t->offset, NULL, 0) != ENG_INVALID) return 0;
    for (u32 i = 0; i < g.n_zdepths; i++)
        if (g.zdepths[i].location == t->location && g.zdepths[i].offset == t->offset &&
            g.zdepths[i].depth_serial)
            return 0;
    return 1;
}

/* The border colour a unit's sampler gets, as RPCS3 VKDraw.cpp load_texture_env
 * builds it (417-445 fragment, 600-605 vertex). Not border-clamped by its
 * dimension (rsx::is_border_clamped_texture: 1D S, 2D/cube S|T, 3D S|T|R)
 * -> opaque black. Fragment units still sign-extended in the shader take
 * fragment_texture::border_color(true): each byte flagged in FILTER[31:28]
 * (bit0 A .. bit3 B) becomes (v - 128) & 0xFF. Then the crossbar applies
 * (remapped_border_color; NVIDIA/Intel need the pre-swizzled colour,
 * require_border_color_remap), and a unit read through an SNORM view gets
 * (c - 128/255) * 255/127 on its signed lanes. Result re-packed A8R8G8B8,
 * clamped. rgba gets the same colour unclamped, R,G,B,A (RPCS3's color4f, what
 * VK_EXT_custom_border_color samples). The crossbar is skipped when the
 * register is the identity (remapped_border_color tests remap(), not the
 * decoded one) or when the backend's device swizzles the border itself
 * (require_border_color_remap false: AMD, or borderColorSwizzleFromImage).
 * RSX_TEX_LEGACY=1 keeps the raw register. */
static int s_eng_border_remap = 1;

static void eng_border_unpack(u32 argb, float rgba[4])
{
    rgba[0] = (float)((argb >> 16) & 0xFFu) / 255.0f;
    rgba[1] = (float)((argb >> 8) & 0xFFu) / 255.0f;
    rgba[2] = (float)(argb & 0xFFu) / 255.0f;
    rgba[3] = (float)(argb >> 24) / 255.0f;
}

static u32 eng_border_color(u32 raw, u32 wrap, u32 dimension, u32 argb_signed, int sext,
                            int hw_snorm, u32 control1, u32 format, int vertex, float rgba[4])
{
    if (rsx_texture_legacy()) { eng_border_unpack(raw, rgba); return raw; }
    const u32 ws = wrap & 0xFu, wt = (wrap >> 8) & 0xFu, wr = (wrap >> 16) & 0xFu;
    int clamped;
    switch (dimension) {
    case 1:  clamped = ws == 4u; break;
    case 2:  clamped = ws == 4u || wt == 4u; break;
    case 3:  clamped = ws == 4u || wt == 4u || wr == 4u; break;
    default: clamped = 0; break;
    }
    if (!clamped) { eng_border_unpack(0xFF000000u, rgba); return 0xFF000000u; }
    if (vertex) { eng_border_unpack(raw, rgba); return raw; }
    argb_signed &= 0xFu;
    if (sext && argb_signed) {
        u32 mask = 0;
        if (argb_signed & 1u) mask |= 0xFF000000u;
        if (argb_signed & 2u) mask |= 0x00FF0000u;
        if (argb_signed & 4u) mask |= 0x0000FF00u;
        if (argb_signed & 8u) mask |= 0x000000FFu;
        raw ^= 0x80808080u & mask;
    }
    float c[4] = { (float)(raw >> 24) / 255.0f, (float)((raw >> 16) & 0xFFu) / 255.0f,
                   (float)((raw >> 8) & 0xFFu) / 255.0f, (float)(raw & 0xFFu) / 255.0f };
    const u32 dr = rsx_texture_decoded_remap(control1, format);
    if (s_eng_border_remap && control1 != 0xAAE4u && (dr & 0xFFFFu) != 0xAAE4u) {
        float o[4];
        for (u32 i = 0; i < 4; i++) {
            const u32 ctl = (dr >> (8 + i * 2)) & 3u;
            o[i] = ctl == 0u ? 0.0f : ctl == 1u ? 1.0f : c[(dr >> (i * 2)) & 3u];
        }
        memcpy(c, o, sizeof c);
    }
    if (hw_snorm)
        for (u32 i = 0; i < 4; i++)
            if ((argb_signed >> i) & 1u)
                c[i] = (c[i] - 128.0f / 255.0f) * (255.0f / 127.0f);
    rgba[0] = c[1]; rgba[1] = c[2]; rgba[2] = c[3]; rgba[3] = c[0];
    u32 out = 0;
    for (u32 i = 0; i < 4; i++) {
        float v = c[i] < 0.0f ? 0.0f : c[i] > 1.0f ? 1.0f : c[i];
        out |= (u32)(v * 255.0f + 0.5f) << (24 - i * 8);
    }
    return out;
}

/* RPCS3 texture_cache.h fast_texture_search / upload_texture: these formats
 * never resolve to a render target (or its depth); they come from the cached
 * or CPU-uploaded section at their address. Binding the target instead
 * samples it as RGBA8 and drops the format's crossbar. */
static int eng_tex_format_skips_rtt(u32 format)
{
    switch (format & 0x9Fu) {
    case 0x86: case 0x87: case 0x88: case 0x8D: case 0x8E: case 0x98: case 0x99:
        return !rsx_texture_legacy();
    default:
        return 0;
    }
}

/* ---- pipelines ----------------------------------------------------------- */

static u32 eng_vtex_mask(void)
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        if (vt.enabled && vt.width && vt.height) mask |= 1u << u;
    }
    return mask;
}

/* Units addressed in texels (RSX_TEX_FMT_UNNORM), with each one's size in
 * dim. No host sampling mode is unnormalised for these, so the fragment
 * program divides the coordinates instead, and the divisor makes the sizes
 * pipeline identity (rsx_live_draw.c keys them the same way). */
static u32 eng_unnorm_units(u32 dim[RSX_DSP_NUM_TEXTURES][2])
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        dim[u][0] = dim[u][1] = 0;
        if (!t.enabled || !(t.format & RSX_TEX_FMT_UNNORM) || !t.width || !t.height)
            continue;
        mask |= 1u << u;
        dim[u][0] = t.width;
        dim[u][1] = t.height;
    }
    return mask;
}

/* Units bound to a 3D texture (SET_TEXTURE_FORMAT dimension 3 with a
 * depth), declared Texture3D and sampled with three coordinates. Only when
 * the backend can create one: otherwise the first slice stays a 2D texture
 * and the program samples it in 2D, as before. */
static u32 eng_volume_mask(void)
{
    if (!g.be->texture_create_3d || !g.be->texture_upload_3d) return 0;
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        if (t.enabled && !t.cubemap && t.dimension == 3 && t.depth > 1 && t.width && t.height)
            mask |= 1u << u;
    }
    return mask;
}

static u32 eng_cube_mask(void)
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        /* A cube face is square by construction, so a non-square image
         * cannot be one; the fragment program is compiled against this mask
         * and a texturecube slot filled with a 2D texture is a validation
         * failure, not a wrong pixel. */
        if (t.enabled && t.cubemap && t.width && t.width == t.height)
            mask |= 1u << u;
    }
    return mask;
}

/* Units whose SET_TEXTURE_FORMAT dimension is 1: RPCS3 samples them through
 * a sampler1D with coordinate .x alone (TEX1D*), so the fragment program
 * reads them at float2(x, 0.5) of the height-1 image. RSX_TEX_1D_LEGACY=1
 * (or RSX_TEX_LEGACY=1) samples .xy as a 2D unit, as before. */
static u32 eng_tex_1d_mask(void)
{
    static int legacy = -1;
    if (rsx_texture_legacy() || eng_env_flag("RSX_TEX_1D_LEGACY", &legacy)) return 0;
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        if (t.enabled && !t.cubemap && t.dimension == 1)
            mask |= 1u << u;
    }
    return mask;
}

/* The vertex units' counterpart (RPCS3 VERTEX_TEXTURE_FETCH1D), same toggle. */
static u32 eng_vtex_1d_mask(void)
{
    static int legacy = -1;
    if (rsx_texture_legacy() || eng_env_flag("RSX_TEX_1D_LEGACY", &legacy)) return 0;
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        if (vt.enabled && vt.dimension == 1) mask |= 1u << u;
    }
    return mask;
}

/* Are both of the guest's own programs resident? One answer, used by the
 * layout and by the pipeline, because they must not disagree: the built-in
 * program declares all sixteen inputs, so narrowing the layout for it would
 * leave the pipeline's vertex descriptor short of what the shader reads. */
/* RPCS3 analyse_vertex_program of the current transform program: the slot
 * range it touches (which can start before the entry and run past the first
 * end bit), the entry within it and its live instructions. */
static rsx_vp_program_info s_eng_vp_info;
/* RSX_PERF_VP_MEMO: the analysis above (and the layout's input mask) only
 * change with the program store (s_eng_vpgen) or the entry (a register). */
static struct {
    int valid;
    u32 vpgen, start, ok, in_mask;
    int in_valid;
} s_vpm;

static int eng_guest_programs(const u8** out_vp, u32* out_vp_instrs,
                              const u8** out_fp, u32* out_fp_size)
{
    static int perf = -1;
    const u8* vp_uc = NULL;
    u32 vp_instrs = 0;
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (eng_perf_on("RSX_PERF_VP_MEMO", &perf) && s_vpm.valid &&
        s_vpm.vpgen == s_eng_vpgen && s_vpm.start == start) {
        if (s_vpm.ok) {
            vp_uc = (const u8*)(g.rsx.vp + s_eng_vp_info.base * 4);
            vp_instrs = s_eng_vp_info.count;
        }
    } else {
        s_vpm.valid = 0;
        s_vpm.in_valid = 0;
        s_vpm.ok = start < RSX_DSP_VP_INSTR &&
            rsx_vp_analyse_program((const u8*)g.rsx.vp, RSX_DSP_VP_INSTR, start,
                                   &s_eng_vp_info) > 0;
        if (s_vpm.ok) {
            vp_uc = (const u8*)(g.rsx.vp + s_eng_vp_info.base * 4);
            vp_instrs = s_eng_vp_info.count;
        }
        s_vpm.vpgen = s_eng_vpgen;
        s_vpm.start = start;
        s_vpm.valid = 1;
    }

    /* A zero SET_SHADER_PROGRAM is "none", not "the program at offset 0": the
     * register carries the location in its low two bits, so a real program can
     * never read back as zero. */
    const u8* fp_uc = NULL;
    u32 fp_size = 0;
    if (rsx_dsp_reg(&g.rsx, 0x08E4)) {
        u32 fp_loc = 0;
        const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
        fp_uc = eng_guest_ptr(NULL, fp_loc, fp_off, 16);
        fp_size = fp_uc ? rsx_fp_program_size(fp_uc, ENG_FP_MAX_BYTES) : 0;
        if (fp_size) fp_uc = eng_guest_ptr(NULL, fp_loc, fp_off, fp_size);
        if (!fp_uc) fp_size = 0;
    }

    if (out_vp) *out_vp = vp_uc;
    if (out_vp_instrs) *out_vp_instrs = vp_instrs;
    if (out_fp) *out_fp = fp_uc;
    if (out_fp_size) *out_fp_size = fp_size;
    return vp_instrs && fp_size;
}

/* The layout the active vertex program actually reads. An uncertain analysis
 * falls back to all sixteen registers (rsx_live_draw.c:3625-3643). */
static void eng_vertex_layout(rsx_vertex_layout_plan* layout)
{
    u32 mask = 0xFFFFu;
    const u8* uc = NULL;
    u32 instrs = 0;
    if (eng_guest_programs(&uc, &instrs, NULL, NULL)) {
        if (s_vpm.valid && s_vpm.in_valid) {
            mask = s_vpm.in_mask;
        } else {
            rsx_vp_input_analysis analysis = { 0xFFFFu, 0 };
            if (!s_eng_vp_info.has_branch && !s_eng_vp_info.entry &&
                rsx_vp_analyze_inputs(uc, instrs * 16u, &analysis) == (int)instrs &&
                analysis.exact && analysis.input_mask)
                mask = analysis.input_mask;
            if (s_vpm.valid) { s_vpm.in_mask = mask; s_vpm.in_valid = 1; }
        }
    }
    rsx_vertex_layout_plan_init(layout, mask);
}

/* What a draw runs before a title has loaded programs of its own.
 *
 * The reference engine has no fallback and drops such a draw, because a title
 * always has both programs resident by the time it draws anything. A general
 * toolkit does not get to assume that: the host harness's fixed-function modes
 * are exactly this case, and so is the first frame of a title that clears and
 * flips before its first SET_TRANSFORM_PROGRAM. It is written in the same HLSL
 * shape the decompilers emit -- ATTRn inputs, the COLOR0/TEXCOORD varyings, the
 * VPConst block and the viewport epilogue -- so it goes through the one
 * pipeline_create the backend already implements rather than needing a second
 * entry point for a built-in shader.
 *
 * Vertex constant rows 0..3 are the transform when the guest programmed them
 * and identity when it did not: an all-zero matrix would collapse every vertex
 * onto the origin. */
static const char* const kEngFixedVS =
"struct VSInput {\n"
"    float4 a0:ATTR0;  float4 a1:ATTR1;  float4 a2:ATTR2;  float4 a3:ATTR3;\n"
"    float4 a4:ATTR4;  float4 a5:ATTR5;  float4 a6:ATTR6;  float4 a7:ATTR7;\n"
"    float4 a8:ATTR8;  float4 a9:ATTR9;  float4 a10:ATTR10; float4 a11:ATTR11;\n"
"    float4 a12:ATTR12; float4 a13:ATTR13; float4 a14:ATTR14; float4 a15:ATTR15;\n"
"};\n"
"struct VSOutput {\n"
"    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;\n"
"    float4 fog:FOG;\n"
"    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;\n"
"    float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;\n"
"#ifdef RSX_FRAGCOORD_W_IS_RCP\n"
"    [[vk::builtin(\"PointSize\")]]\n"
"#endif\n"
"    float psize:PSIZE;\n"
"};\n"
"cbuffer VPConst : register(b0) {\n"
"    float4 vp_c[512];\n"
"    float4 vp_posscale;\n"
"    float4 vp_posoffset;\n"
"    uint4  vp_branch;\n"
"};\n"
"VSOutput main(VSInput input) {\n"
"    float4 _p = float4(dot(vp_c[0], input.a0), dot(vp_c[1], input.a0),\n"
"                       dot(vp_c[2], input.a0), dot(vp_c[3], input.a0));\n"
"    float _m = dot(abs(vp_c[0]), 1.0) + dot(abs(vp_c[1]), 1.0)\n"
"             + dot(abs(vp_c[2]), 1.0) + dot(abs(vp_c[3]), 1.0);\n"
"    if (_m == 0.0) _p = input.a0;\n"
"    VSOutput Out;\n"
"    Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);\n"
"    if (vp_posoffset.w > 0.0 && Out.pos.w != 0.0) {\n"
"        float _zd = Out.pos.z / Out.pos.w; float _zn = floor(_zd + 0.5);\n"
"        if (abs(_zd - _zn) < vp_posoffset.w) Out.pos.z = _zn * Out.pos.w;\n"
"    }\n"
"    Out.col0 = input.a3; Out.col1 = float4(0,0,0,1); Out.fog = (float4)0;\n"
"    Out.t0 = input.a8; Out.t1 = (float4)0; Out.t2 = (float4)0; Out.t3 = (float4)0;\n"
"    Out.t4 = (float4)0; Out.t5 = (float4)0; Out.t6 = (float4)0; Out.t7 = (float4)0;\n"
"    Out.t8 = (float4)0; Out.t9 = (float4)0;\n"
"    Out.psize = (vp_branch.z & 0x80000000u) != 0u ? asfloat(vp_branch.w) : 1.0;\n"
"    return Out;\n"
"}\n";

static const char* const kEngFixedPS =
"struct PSInput {\n"
"    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
"    float4 fog : FOG;\n"
"    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
"    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
"};\n"
"float4 main(PSInput input) : SV_TARGET { return input.col0; }\n";

/* The texture units a translated pixel program samples. Every sampling call
 * the decompiler emits names its unit's sampler, rsx_samp[N], whatever the
 * texture's own spelling (rsx_tex[N] or, with a cube unit, rsx_texN); the
 * declaration's rsx_samp[16] is out of range and so not counted. */
static u32 eng_sampled_units(const char* ps)
{
    u32 mask = 0;
    for (const char* q = ps; (q = strstr(q, "rsx_samp[")) != NULL; q += 9) {
        const unsigned long u = strtoul(q + 9, NULL, 10);
        if (u < RSX_BE_MAX_TEXTURES) mask |= 1u << u;
    }
    return mask;
}

/* The pipeline for this draw, built once per distinct key. A negative result
 * is cached too (handle 0), so a program pair that will not translate is not
 * retried on every draw of every frame. `out_tex_used` gets the units the
 * pixel program samples: all of them for the built-in pair, which keeps
 * every enabled unit bound as before. */
/* Up to 16 comma-separated hex hashes from an environment variable. */
static u32 eng_parse_hash_list(const char* name, u64 out[16])
{
    const char* e = getenv(name);
    u32 n = 0;
    while (e && *e && n < 16) {
        char* end;
        const u64 v = strtoull(e, &end, 16);
        if (end == e) break;
        if (v) out[n++] = v;
        if (*end != ',') break;
        e = end + 1;
    }
    return n;
}

/* The per-draw fragment-program debug knobs; nonzero drops the draw.
 *
 * RSX_ENGINE_SKIP_FP=<hex>[,<hex>...] drops every draw through those
 * fragment programs: which pass paints a wrong smear, halo or black band is
 * found by removing passes one at a time.
 *
 * RSX_ENGINE_REGDUMP_FP=<hex>[,<hex>...] prints the non-zero words of the
 * whole RSX register file at the first draw through each of those programs,
 * so the state of a broken pass can be diffed register by register against a
 * working one -- which state it sets that the engine does not honour. */
static int eng_debug_fp(u64 fp_hash, u32 pipeline)
{
    static int init;
    static u64 skip[16], dump[16];
    static u32 n_skip, n_dump, dumped;
    if (!init) {
        init = 1;
        n_skip = eng_parse_hash_list("RSX_ENGINE_SKIP_FP", skip);
        n_dump = eng_parse_hash_list("RSX_ENGINE_REGDUMP_FP", dump);
    }
    for (u32 i = 0; i < n_dump; i++) {
        if (dump[i] != fp_hash || (dumped & (1u << i))) continue;
        dumped |= 1u << i;
        fprintf(stderr, "[rsx regdump] fp %016llx pipe %u frame %u begin\n",
                (unsigned long long)fp_hash, pipeline, g.frames);
        for (u32 r = 0; r < RSX_DSP_NUM_REGS; r++)
            if (g.rsx.regs[r])
                fprintf(stderr, "[rsx regdump] 0x%04X = 0x%08X\n", r * 4u, g.rsx.regs[r]);
        fprintf(stderr, "[rsx regdump] fp %016llx end\n", (unsigned long long)fp_hash);
    }
    for (u32 i = 0; i < n_skip; i++)
        if (skip[i] == fp_hash) return 1;
    return 0;
}

/* A guest fragment program's name for the debug knobs: its structural hash
 * (inline constants excluded) seeded with the FNV offset basis, the value
 * RSX_FP_DEBUG matches in the decompiler. Unlike a backend pipeline handle it
 * does not depend on the order programs are first drawn in, so a program
 * found in one run is named the same in the next. 0 for the built-in one. */
static u64 eng_fp_debug_hash(const u8* fp_uc, u32 fp_size)
{
    return fp_uc && fp_size
        ? rsx_fp_structural_hash(fp_uc, fp_size, 1469598103934665603ull) : 0;
}

/* RPCS3 decode_constant_blend_color: FP16 targets read four halves
 * (R,G in BLEND_COLOR, B,A in BLEND_COLOR2), the rest A8R8G8B8. */
static void eng_blend_constants(u32 color_format, float k[4])
{
    const u32 c = rsx_dsp_reg(&g.rsx, 0x031C);
    if (color_format == RSX_SURFACE_FMT_F_W16Z16Y16X16) {
        const u32 c2 = rsx_dsp_reg(&g.rsx, 0x037C);
        k[0] = eng_half(c & 0xFFFFu);  k[1] = eng_half(c >> 16);
        k[2] = eng_half(c2 & 0xFFFFu); k[3] = eng_half(c2 >> 16);
    } else {
        k[0] = (float)((c >> 16) & 0xFFu) / 255.0f;
        k[1] = (float)((c >> 8) & 0xFFu) / 255.0f;
        k[2] = (float)(c & 0xFFu) / 255.0f;
        k[3] = (float)(c >> 24) / 255.0f;
    }
}

/* The last eng_pipeline_get's emulated depth-compare unit + 1 (0 = none),
 * for sink_end to bind the depth snapshot at. */
static u32 s_eng_depth_cmp_unit;
/* Likewise its programmable-blend destination unit + 1 (rsx_fp_frag_state pb),
 * and the render state it was built with (hardware blending off). */
static u32 s_eng_pb_unit;
static rsx_be_render_state s_pb_rs;
static u32 s_pb_copy;           /* the last pb destination copy, engine-owned */
/* The last eng_pipeline_get's g.pipelines index, for eng_pcache_note_topology. */
static u32 s_eng_pipe_idx = 0xFFFFFFFFu;

/* ---- persisted program cache ----------------------------------------------
 * RPCS3 shaders_cache: every pipeline the engine builds is appended to a file
 * as its key, the two decompiled programs and the state they were built
 * with, plus the topologies it was drawn with; a later run of the same
 * executable preloads them all at the first draw, so with an asynchronous
 * backend (pipeline_async) they compile in the background before a draw asks.
 * The header carries the executable's size and timestamp, so a rebuilt
 * decompiler never sees stale programs. Only used when the backend compiles
 * asynchronously (a synchronous one would compile the lot at boot).
 * RSX_ENGINE_PROGRAM_CACHE=0 disables it; RSX_ENGINE_PROGRAM_CACHE_PATH
 * moves it from shadercache/rsx_engine_programs.bin. */
#define ENG_PCACHE_MAGIC   0x50585352u   /* "RSXP" */
/* 2: decompiler/key changes (1D texture masks, alpha-ref format, bx2 lanes,
 * VP start offset) -- discard programs stored by older builds even when the
 * executable's size and timestamp happen to match. */
/* 3: depth_cmp_unit carries the programmable-blend unit + 1 in bits 8-15. */
#define ENG_PCACHE_VERSION 3u
#define ENG_PCACHE_TAG_P   0x50u
#define ENG_PCACHE_TAG_T   0x54u

typedef struct {
    u32 magic, version, rs_size, layout_size;
    u64 exe_size, exe_mtime;
} eng_pcache_hdr;

typedef struct {
    u64 key, fp_hash;
    u32 fixed, depth_cmp_unit, tex_used, rt_fmt, rt_count, topo_mask, vs_len, ps_len;
} eng_pcache_rec;

static struct { int state; FILE* f; } s_epc;   /* state: 0 untried, 1 open, -1 off */

static void eng_pcache_header(eng_pcache_hdr* h)
{
    memset(h, 0, sizeof *h);
    h->magic = ENG_PCACHE_MAGIC;
    h->version = ENG_PCACHE_VERSION;
    h->rs_size = (u32)sizeof(rsx_be_render_state);
    h->layout_size = (u32)sizeof(rsx_vertex_layout_plan);
#ifdef _WIN32
    char path[1024];
    const DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof path);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (n && n < sizeof path && GetFileAttributesExA(path, GetFileExInfoStandard, &a)) {
        h->exe_size = ((u64)a.nFileSizeHigh << 32) | a.nFileSizeLow;
        h->exe_mtime = ((u64)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    }
#else
    struct stat st;
    if (!stat("/proc/self/exe", &st)) {
        h->exe_size = (u64)st.st_size;
        h->exe_mtime = (u64)st.st_mtime;
    }
#endif
}

/* key -> index+1 over g.pipelines, open addressing, built lazily: first
 * match wins, as the linear scan it replaces. */
#define ENG_PIDX_SIZE (ENG_MAX_PIPELINES * 2u)
static u32 s_pidx[ENG_PIDX_SIZE];
static u32 s_pidx_n;

static int eng_pcache_find(u64 key)
{
    if (s_pidx_n > g.n_pipelines) { memset(s_pidx, 0, sizeof s_pidx); s_pidx_n = 0; }
    for (; s_pidx_n < g.n_pipelines; s_pidx_n++) {
        const u64 k = g.pipelines[s_pidx_n].key;
        u32 h = (u32)(k ^ (k >> 29) ^ (k >> 47)) & (ENG_PIDX_SIZE - 1u);
        for (;;) {
            if (!s_pidx[h]) { s_pidx[h] = s_pidx_n + 1u; break; }
            if (g.pipelines[s_pidx[h] - 1u].key == k) break;
            h = (h + 1u) & (ENG_PIDX_SIZE - 1u);
        }
    }
    u32 h = (u32)(key ^ (key >> 29) ^ (key >> 47)) & (ENG_PIDX_SIZE - 1u);
    while (s_pidx[h]) {
        if (g.pipelines[s_pidx[h] - 1u].key == key) return (int)(s_pidx[h] - 1u);
        h = (h + 1u) & (ENG_PIDX_SIZE - 1u);
    }
    return -1;
}

static void eng_pcache_open(void)
{
    if (s_epc.state) return;
    s_epc.state = -1;
    const char* e = getenv("RSX_ENGINE_PROGRAM_CACHE");
    if (e && *e == '0') return;
    if (!g.be->pipeline_async || !g.be->pipeline_async(g.be->user)) return;
    const char* pe = getenv("RSX_ENGINE_PROGRAM_CACHE_PATH");
    const char* path = (pe && *pe) ? pe : "shadercache/rsx_engine_programs.bin";
    eng_pcache_hdr want;
    eng_pcache_header(&want);
    u32 loaded = 0, warmed = 0;
    int valid = 0;
    FILE* f = fopen(path, "rb");
    if (f) {
        eng_pcache_hdr h;
        valid = fread(&h, sizeof h, 1, f) == 1 && !memcmp(&h, &want, sizeof h);
        while (valid) {
            u32 tag;
            if (fread(&tag, sizeof tag, 1, f) != 1) break;          /* clean end */
            if (tag == ENG_PCACHE_TAG_T) {
                u64 key; u32 m;
                if (fread(&key, sizeof key, 1, f) != 1 || fread(&m, sizeof m, 1, f) != 1) { valid = 0; break; }
                const int i = eng_pcache_find(key);
                if (i >= 0) g.pipelines[i].topo_mask |= m;
                continue;
            }
            eng_pcache_rec r;
            rsx_be_render_state rs;
            rsx_vertex_layout_plan lp;
            if (tag != ENG_PCACHE_TAG_P || fread(&r, sizeof r, 1, f) != 1 ||
                fread(&rs, sizeof rs, 1, f) != 1 || fread(&lp, sizeof lp, 1, f) != 1 ||
                r.vs_len >= sizeof s_vs_hlsl || r.ps_len >= sizeof s_ps_hlsl ||
                fread(s_vs_hlsl, 1, r.vs_len, f) != r.vs_len ||
                fread(s_ps_hlsl, 1, r.ps_len, f) != r.ps_len) { valid = 0; break; }
            s_vs_hlsl[r.vs_len] = 0;
            s_ps_hlsl[r.ps_len] = 0;
            /* untrusted file: indices that later address fixed arrays */
            if ((r.depth_cmp_unit & 0xFFu) > RSX_BE_MAX_TEXTURES ||
                (r.depth_cmp_unit >> 8) > RSX_BE_MAX_TEXTURES || r.rt_count > RSX_BE_MAX_COLOR_TARGETS ||
                lp.count > RSX_DSP_NUM_VERTEX_ATTR) { valid = 0; break; }
            for (u32 k = 0; k < lp.count; k++)
                if (lp.attrs[k] >= RSX_DSP_NUM_VERTEX_ATTR) { valid = 0; break; }
            if (!valid) break;
            r.tex_used &= (1u << RSX_BE_MAX_TEXTURES) - 1u;
            if (eng_pcache_find(r.key) >= 0 || g.n_pipelines >= ENG_MAX_PIPELINES) continue;
            eng_pipeline* p = &g.pipelines[g.n_pipelines++];
            memset(p, 0, sizeof *p);
            p->key = r.key;
            p->fp_hash = r.fp_hash;
            p->handle = g.be->pipeline_create(g.be->user, s_vs_hlsl, s_ps_hlsl, &rs, &lp,
                                              lp.stride, (rsx_be_format)r.rt_fmt, r.rt_count);
            p->fixed = (u8)r.fixed;
            p->depth_cmp_unit = (u8)r.depth_cmp_unit;
            p->pb_unit = (u8)(r.depth_cmp_unit >> 8);
            p->tex_used = (u16)r.tex_used;
            p->topo_mask = r.topo_mask;
            loaded++;
        }
        fclose(f);
    }
    if (g.be->pipeline_prewarm)
        for (u32 i = 0; i < g.n_pipelines; i++)
            if (g.pipelines[i].handle && g.pipelines[i].topo_mask) {
                g.be->pipeline_prewarm(g.be->user, g.pipelines[i].handle, g.pipelines[i].topo_mask);
                warmed++;
            }
    /* A damaged or stale file is started again; what loaded stays loaded. */
    s_epc.f = fopen(path, valid ? "ab" : "wb");
    if (!s_epc.f && !(pe && *pe)) {
#ifdef _WIN32
        CreateDirectoryA("shadercache", NULL);
#endif
        s_epc.f = fopen(path, valid ? "ab" : "wb");
    }
    if (s_epc.f && !valid) {
        fwrite(&want, sizeof want, 1, s_epc.f);
        fflush(s_epc.f);
    }
    s_epc.state = s_epc.f ? 1 : -1;
    fprintf(stderr, "[rsx engine] program cache %s: %u pipelines preloaded, %u prewarmed%s\n",
            path, loaded, warmed, s_epc.f ? "" : " (cannot write)");
}

/* Append the pipeline just built from s_vs_hlsl / s_ps_hlsl. */
static void eng_pcache_append(const eng_pipeline* p, const rsx_be_render_state* rs,
                              const rsx_vertex_layout_plan* layout, rsx_be_format rt_fmt,
                              u32 rt_count)
{
    if (s_epc.state <= 0 || !p->handle) return;
    eng_pcache_rec r;
    memset(&r, 0, sizeof r);
    r.key = p->key;
    r.fp_hash = p->fp_hash;
    r.fixed = p->fixed;
    r.depth_cmp_unit = p->depth_cmp_unit | ((u32)p->pb_unit << 8);
    r.tex_used = p->tex_used;
    r.rt_fmt = (u32)rt_fmt;
    r.rt_count = rt_count;
    r.vs_len = (u32)strlen(s_vs_hlsl);
    r.ps_len = (u32)strlen(s_ps_hlsl);
    const u32 tag = ENG_PCACHE_TAG_P;
    fwrite(&tag, sizeof tag, 1, s_epc.f);
    fwrite(&r, sizeof r, 1, s_epc.f);
    fwrite(rs, sizeof *rs, 1, s_epc.f);
    fwrite(layout, sizeof *layout, 1, s_epc.f);
    fwrite(s_vs_hlsl, 1, r.vs_len, s_epc.f);
    fwrite(s_ps_hlsl, 1, r.ps_len, s_epc.f);
    fflush(s_epc.f);
}

/* A draw is about to use `pipeline` with `topology`: remember it, so the
 * next run's preload builds that variant too. */
static void eng_pcache_note_topology(u32 pipeline, u32 topology)
{
    if (s_eng_pipe_idx >= g.n_pipelines || topology > 31u) return;
    eng_pipeline* p = &g.pipelines[s_eng_pipe_idx];
    const u32 bit = 1u << topology;
    if (p->handle != pipeline || (p->topo_mask & bit)) return;
    p->topo_mask |= bit;
    if (s_epc.state <= 0) return;
    const u32 tag = ENG_PCACHE_TAG_T;
    fwrite(&tag, sizeof tag, 1, s_epc.f);
    fwrite(&p->key, sizeof p->key, 1, s_epc.f);
    fwrite(&bit, sizeof bit, 1, s_epc.f);
    fflush(s_epc.f);
}

static void eng_pcache_close(void)
{
    if (s_epc.f) fclose(s_epc.f);
    s_epc.f = NULL;
    s_epc.state = 0;
}

/* RSX_PERF_PIPE_MEMO: the last pipeline lookup's inputs and results. Every
 * input of eng_pipeline_get_impl is a register (s_eng_pgen), the program
 * store (s_eng_vpgen), the fragment program's guest bytes (compared), the
 * arguments (compared), init-time switches and backend caps (reset at init),
 * or -- for the hardware SNORM / sRGB view choice -- the surface store, which
 * is re-evaluated for each sampled unit on every hit. */
static struct {
    int valid;
    u32 pgen, vpgen;
    const rsx_draw_backend* be;
    rsx_be_render_state rs;
    u32 lmask, lstride, rt_fmt, rt_count, prim;
    int host_nv, provoking, fixed;
    u32 fp_reg, fp_size, fp_cap;
    u8* fp_copy;
    u32 recheck, bx2, snorm, srgb;
    u32 handle, idx;
    u64 key;
    u32 nconst, nconst_cap;
    u32 (*cvals)[4];
} s_pm;

static void eng_pmemo_rs_norm(rsx_be_render_state* o, const rsx_be_render_state* rs)
{
    *o = *rs;
    /* Not read by the pipeline path (fragment constant only). */
    o->alpha_ref_raw = 0;
    o->alpha_ref_format = 0;
}

static int eng_pmemo_hit(const rsx_vertex_layout_plan* layout, const rsx_be_render_state* rs,
                         rsx_be_format rt_fmt, u32 rt_count, int* out_fixed,
                         u32* out_tex_used, u64* out_fp_hash, u32* out_handle)
{
    if (!s_pm.valid || s_pm.pgen != s_eng_pgen || s_pm.vpgen != s_eng_vpgen ||
        s_pm.be != g.be || s_pm.lmask != layout->mask || s_pm.lstride != layout->stride ||
        s_pm.rt_fmt != (u32)rt_fmt || s_pm.rt_count != rt_count ||
        s_pm.prim != g.rsx.current_primitive || s_pm.host_nv != s_eng_host_nv ||
        s_pm.provoking != s_eng_last_provoking)
        return 0;
    if (s_pm.idx >= g.n_pipelines || g.pipelines[s_pm.idx].key != s_pm.key ||
        g.pipelines[s_pm.idx].handle != s_pm.handle)
        return 0;
    rsx_be_render_state n;
    eng_pmemo_rs_norm(&n, rs);
    if (memcmp(&n, &s_pm.rs, sizeof n)) return 0;
    const u8* vp_uc = NULL;
    const u8* fp_uc = NULL;
    u32 vp_instrs = 0, fp_size = 0;
    const int fixed = !eng_guest_programs(&vp_uc, &vp_instrs, &fp_uc, &fp_size);
    if (fixed != s_pm.fixed) return 0;
    if (!fixed) {
        if (fp_size != s_pm.fp_size || rsx_dsp_reg(&g.rsx, 0x08E4) != s_pm.fp_reg ||
            memcmp(fp_uc, s_pm.fp_copy, fp_size))
            return 0;
        u32 sn = 0, sr = 0;
        for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
            if (!((s_pm.recheck >> u) & 1u)) continue;
            rsx_dsp_texture t;
            rsx_dsp_get_texture(&g.rsx, u, &t);
            if (eng_tex_hw_snorm(&t, s_pm.bx2, u)) sn |= 1u << u;
            else if (eng_tex_hw_srgb(&t, s_pm.bx2, u)) sr |= 1u << u;
        }
        if (sn != s_pm.snorm || sr != s_pm.srgb) return 0;
    }
    /* g.fp_constants: entries past count are zero (every writer fills it
     * from a cleared block), so only the stale tail needs clearing. */
    const u32 prev = g.fp_constants.count;
    if (s_pm.nconst) memcpy(g.fp_constants.values, s_pm.cvals, s_pm.nconst * 16u);
    if (prev > s_pm.nconst && prev <= RSX_FP_MAX_INLINE_CONSTANTS)
        memset(g.fp_constants.values[s_pm.nconst], 0, (prev - s_pm.nconst) * 16u);
    else if (prev > RSX_FP_MAX_INLINE_CONSTANTS)
        memset(&g.fp_constants, 0, sizeof g.fp_constants);
    g.fp_constants.count = s_pm.nconst;
    const u32 i = s_pm.idx;
    s_eng_hw_snorm_units = s_pm.snorm;
    s_eng_hw_srgb_units = s_pm.srgb;
    *out_fixed = g.pipelines[i].fixed;
    *out_tex_used = g.pipelines[i].tex_used;
    *out_fp_hash = g.pipelines[i].fp_hash;
    s_eng_depth_cmp_unit = g.pipelines[i].depth_cmp_unit;
    s_eng_pb_unit = g.pipelines[i].pb_unit;
    s_eng_pipe_idx = i;
    *out_handle = g.pipelines[i].handle;
    return 1;
}

static void eng_pmemo_store(const rsx_vertex_layout_plan* layout, const rsx_be_render_state* rs,
                            rsx_be_format rt_fmt, u32 rt_count, int fixed,
                            const u8* fp_uc, u32 fp_size, u32 recheck, u32 bx2, u32 idx)
{
    s_pm.valid = 0;
    if (idx >= g.n_pipelines) return;
    if (!fixed) {
        if (s_pm.fp_cap < fp_size) {
            u8* n = (u8*)realloc(s_pm.fp_copy, fp_size);
            if (!n) return;
            s_pm.fp_copy = n;
            s_pm.fp_cap = fp_size;
        }
        memcpy(s_pm.fp_copy, fp_uc, fp_size);
    }
    const u32 nc = g.fp_constants.count;
    if (nc > RSX_FP_MAX_INLINE_CONSTANTS) return;
    if (s_pm.nconst_cap < nc) {
        void* n = realloc(s_pm.cvals, (size_t)nc * 16u);
        if (!n) return;
        s_pm.cvals = (u32 (*)[4])n;
        s_pm.nconst_cap = nc;
    }
    if (nc) memcpy(s_pm.cvals, g.fp_constants.values, nc * 16u);
    s_pm.nconst = nc;
    s_pm.pgen = s_eng_pgen;
    s_pm.vpgen = s_eng_vpgen;
    s_pm.be = g.be;
    eng_pmemo_rs_norm(&s_pm.rs, rs);
    s_pm.lmask = layout->mask;
    s_pm.lstride = layout->stride;
    s_pm.rt_fmt = (u32)rt_fmt;
    s_pm.rt_count = rt_count;
    s_pm.prim = g.rsx.current_primitive;
    s_pm.host_nv = s_eng_host_nv;
    s_pm.provoking = s_eng_last_provoking;
    s_pm.fixed = fixed;
    s_pm.fp_reg = rsx_dsp_reg(&g.rsx, 0x08E4);
    s_pm.fp_size = fixed ? 0 : fp_size;
    s_pm.recheck = fixed ? 0 : recheck;
    s_pm.bx2 = bx2;
    s_pm.snorm = s_eng_hw_snorm_units;
    s_pm.srgb = s_eng_hw_srgb_units;
    s_pm.idx = idx;
    s_pm.key = g.pipelines[idx].key;
    s_pm.handle = g.pipelines[idx].handle;
    s_pm.valid = 1;
}

static u32 eng_pipeline_get_impl(const rsx_vertex_layout_plan* layout,
                            const rsx_be_render_state* rs,
                            rsx_be_format rt_fmt, u32 rt_count, int* out_fixed,
                            u32* out_tex_used, u64* out_fp_hash)
{
    eng_pcache_open();
    static int pm_perf = -1;
    const int pm_on = eng_perf_on("RSX_PERF_PIPE_MEMO", &pm_perf);
    const rsx_be_render_state* const rs_in = rs;
    if (pm_on) {
        u32 h = 0;
        const int hit = eng_pmemo_hit(layout, rs, rt_fmt, rt_count, out_fixed,
                                      out_tex_used, out_fp_hash, &h);
        ENG_PC(PC_PMEMO, hit);
        if (hit) return h;
    }
    u32 pm_recheck = 0, pm_bx2 = 0;
    s_eng_pipe_idx = 0xFFFFFFFFu;
    s_eng_hw_snorm_units = 0;
    s_eng_hw_srgb_units = 0;
    *out_fixed = 1;
    *out_tex_used = 0;
    *out_fp_hash = 0;
    s_eng_depth_cmp_unit = 0;
    s_eng_pb_unit = 0;
    const u8* vp_uc = NULL;
    const u8* fp_uc = NULL;
    u32 vp_instrs = 0, fp_size = 0;
    /* No resident program pair means the built-in one; see kEngFixedVS. */
    const int fixed = !eng_guest_programs(&vp_uc, &vp_instrs, &fp_uc, &fp_size);

    const u32 fp_ctrl  = rsx_dsp_shader_control(&g.rsx);
    const u32 srgb_output = !fixed && rsx_fp_srgb_output_enabled(
        rsx_dsp_reg(&g.rsx, 0x1FECu), fp_ctrl,
        rsx_dsp_reg(&g.rsx, 0x0208u) & 0x1Fu);
    const u32 cube_mask = eng_cube_mask();
    const u32 vol_mask = eng_volume_mask();
    const u32 tex_1d_mask = eng_tex_1d_mask() & ~(cube_mask | vol_mask);
    const u32 vtex_mask = eng_vtex_mask();
    const u32 vtex_1d_mask = eng_vtex_1d_mask() & vtex_mask;
    /* RPCS3 VertexProgramDecompiler.cpp:220-240: 3D and cube vertex units. */
    u32 vtex_3d_mask = 0, vtex_cube_mask = 0, vtex_un_mask = 0;
    if (!rsx_texture_legacy())
        for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
            if (!((vtex_mask >> u) & 1u)) continue;
            rsx_dsp_vertex_texture vt;
            rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
            if (vt.cubemap && vt.dimension == 2) vtex_cube_mask |= 1u << u;
            else if (vt.dimension == 3) vtex_3d_mask |= 1u << u;
            /* RPCS3 VKDraw.cpp:593: CELL_GCM_TEXTURE_UN (0x40) in vertex_texture::format(). */
            if (vt.format & 0x40u) vtex_un_mask |= 1u << u;
        }
    rsx_fp_shadow_desc shadow[RSX_DSP_NUM_TEXTURES] = {{0}};
    u32 shadow_mask = 0;
    const u32 sampled_units = fixed ? 0 : rsx_fp_texture_units(fp_uc, fp_size);
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        if (!(sampled_units & (1u << u))) continue;
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        rsx_fp_shadow_desc_from_texture(&shadow[u], t.format, t.dimension,
            t.cubemap, t.mipmaps, t.filter, t.wrap, t.control0);
        if (shadow[u].enabled) shadow_mask |= 1u << u;
    }
    u8 gamma_masks[RSX_DSP_NUM_TEXTURES] = {0};
    if (!fixed) for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u) {
        rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
        if (t.enabled) gamma_masks[u] = rsx_texture_gamma_mask(t.format, t.remap, t.filter, t.wrap);
    }
    const u32 gamma_units = fixed ? 0 : rsx_fp_texture_gamma_units(fp_uc, fp_size, gamma_masks);
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u)
        if (!(gamma_units & (1u << u))) gamma_masks[u] = 0;
    /* Texture sign expansion: SET_TEXTURE_ADDRESS's UNSIGNED_REMAP_BIASED
     * asks that every fetch come back re-centred to -1..1. Same rule as
     * gamma: only units the program samples count. */
    u8 expand_masks[RSX_DSP_NUM_TEXTURES] = {0};
    if (!fixed) for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u) {
        rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
        if (t.enabled) expand_masks[u] = rsx_texture_expand_mask(t.format, t.remap, t.filter, t.wrap);
    }
    const u32 expand_units = fixed ? 0 : rsx_fp_texture_expand_units(fp_uc, fp_size, expand_masks);
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u)
        if (!(expand_units & (1u << u))) expand_masks[u] = 0;

    /* Fixed-function fragment state RPCS3 emulates in the shader
     * (graph_engine::get_current_fragment_program); each part only where
     * the program can observe it, so unaffected pipelines keep their keys. */
    rsx_fp_frag_state fs;
    memset(&fs, 0, sizeof fs);
    int fs_any = 0;
    if (!fixed) {
        const u32 inputs = rsx_fp_input_mask(fp_uc, fp_size);
        const u32 surface_fmt = rsx_dsp_reg(&g.rsx, 0x0208u) & 0x1Fu;
        const u32 aa = rsx_dsp_reg(&g.rsx, 0x1D7Cu);
        const u32 window = rsx_dsp_reg(&g.rsx, 0x1D88u);
        if (inputs & (1u << 3)) {
            fs.fog = 1;
            fs.fog_mode = rsx_dsp_reg(&g.rsx, 0x08CCu);
        }
        if (inputs & 1u) {
            fs.wpos = 1;
            fs.wpos_origin_bottom = ((window >> 12) & 15u) == 1u;
            fs.wpos_center_integer = ((window >> 16) & 15u) == 1u;
        }
        if ((rsx_dsp_reg(&g.rsx, 0x142Cu) & 1u) && (inputs & 6u))
            fs.two_sided = 1;
        /* SHADE_MODE (0x0368) FLAT 0x1D00: RPCS3 RSXGraphEngine.cpp:1154 sets
         * RSX_SHADER_CONTROL_FLAT_SHADING only with a last-vertex provoking
         * convention, else colours stay smooth. RSX_ENG_FLAT_SHADE=0 off. */
        if (s_eng_flat_shade && s_eng_last_provoking && (inputs & 6u) &&
            rsx_dsp_reg(&g.rsx, 0x0368u) == 0x1D00u)
            fs.flat_shade = 1;
        for (u32 t = 0; t < 10; ++t)
            if ((inputs & (1u << (4u + t))) && (rsx_dsp_reg(&g.rsx, 0x0B40u + 4u * t) & 1u))
                fs.texcoord_2d_mask |= (u16)(1u << t);
        /* POINT_SPRITE_CONTROL (0x1EE8): bit 0 enable, bits 1-2 R mode,
         * bits 8-17 the TC0..9 to replace, on point draws only (RPCS3
         * RSXGraphEngine.cpp:1172). RSX_ENG_POINT_SPRITE=0 turns it off. */
        if (s_eng_point_sprite && g.rsx.current_primitive == 1u &&
            g.be->point_sprite_supported && g.be->point_sprite_supported(g.be->user)) {
            const u32 psc = rsx_dsp_reg(&g.rsx, 0x1EE8u);
            if (psc & 1u)
                fs.point_sprite_mask = (u16)(((psc >> 8) & 0x3FFu) & (inputs >> 4));
        }
        if ((rsx_dsp_reg(&g.rsx, 0x147Cu) & 1u) && g.rsx.current_primitive >= 5u) {
            fs.stipple = 1;
            for (u32 y = 0; y < 32; ++y)
                fs.stipple_pattern[y] = rsx_dsp_reg(&g.rsx, 0x1480u + 4u * y);
        }
        const u32 bx2_units = rsx_fp_texture_bx2_units(fp_uc, fp_size);
        pm_bx2 = bx2_units;
        for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u) {
            if (!(sampled_units & (1u << u))) continue;
            rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
            /* RPCS3 _enable_texture_expand: _bx2 acts only on formats with
             * BIASED_NORMALIZATION (TextureUtils.cpp get_format_features),
             * on the lanes decoded_remap().shuffle_mask_bits(0xF) takes from
             * the texture (RSXGraphEngine.cpp:1403); a disabled unit has no
             * flags at all. */
            if ((bx2_units >> u) & 1u) {
                int biased = 0;
                if (t.enabled) switch (t.format & 0x9Fu) {
                case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
                case 0x86: case 0x87: case 0x88: case 0x8B: case 0x8D: case 0x8E:
                case 0x8F: case 0x90: case 0x91: case 0x92: case 0x93: case 0x94:
                case 0x95: case 0x97: case 0x98: case 0x9D: case 0x9E:
                    biased = 1; break;
                default: break;
                }
                if (!biased) fs.bx2_lanes[u] = 0x20u;
                else {
                    static const u8 rgba_lane[4] = {3, 0, 1, 2};
                    const u32 c1 = rsx_texture_decoded_remap(t.remap, t.format);
                    u8 lanes = 0;
                    for (u32 i = 0; i < 4; ++i)
                        if (((c1 >> (8u + 2u * i)) & 3u) >= 2u) lanes |= (u8)(1u << rgba_lane[i]);
                    fs.bx2_lanes[u] = (u8)(0x10u | lanes);
                }
            }
            if (!t.enabled) continue;
            pm_recheck |= 1u << u;
            if (t.control0 & 4u) fs.alpha_kill_mask |= (u16)(1u << u);
            if (eng_tex_hw_snorm(&t, bx2_units, u)) s_eng_hw_snorm_units |= 1u << u;
            else {
                fs.sext_masks[u] = rsx_fp_texture_sext_mask(t.format, t.remap, t.filter);
                /* The sRGB view decodes R,G,B: the shader neither decodes
                 * them nor (as RPCS3's EXPAND bits never name gamma lanes)
                 * re-centres them. */
                if (eng_tex_hw_srgb(&t, bx2_units, u)) {
                    s_eng_hw_srgb_units |= 1u << u;
                    gamma_masks[u] = 0;
                    expand_masks[u] &= (u8)~0x7u;
                }
            }
        }
        fs.mrt_count = (u8)(rt_count > 1u ? (rt_count < 4u ? rt_count : 4u) : 0u);
        fs.rop_round = surface_fmt != 0x0Bu && surface_fmt != 0x0Cu && surface_fmt != 0x0Du;
        /* RPCS3 TextureUtils.cpp get_ROP_output_shuffle_index, for the
         * formats rsx_decode.h is_remapped_format names. */
        switch (surface_fmt) {
        case 0x09u: fs.rop_remap = 1u; break;                       /* B8 */
        case 0x0Au: fs.rop_remap = 2u; break;                       /* G8B8 */
        case 0x01u: case 0x04u: case 0x0Eu: fs.rop_remap = rs->x_alpha_pb ? 0u : 3u; break; /* X.._Z.. */
        case 0x02u: case 0x05u: case 0x0Fu: fs.rop_remap = rs->x_alpha_pb ? 0u : 4u; break; /* X.._O.. */
        default: break;
        }
        fs.alpha_to_coverage = (aa & 0x10u) != 0u && !(eng_hw_aa_caps() & RSX_BE_AA_HW_MSAA);
        fs.alpha_to_one = (aa & 0x100u) != 0u && !(eng_hw_aa_caps() & RSX_BE_AA_HW_A2ONE);
        /* RPCS3 emulate_depth_compare (Emulate Special Depth Comparison):
         * a depth-EQUAL polygon draw into a zeta snaps a fragment within one
         * zeta LSB of the stored depth onto it. The stored depth comes from
         * the backend's depth snapshot, bound at the highest unit the program
         * neither samples nor declares as a cube/volume.
         * RSX_ENGINE_DEPTH_COMPARE=0 turns it off. */
        {
            static int dc_env = -1;
            if (dc_env < 0) {
                const char* e = getenv("RSX_ENGINE_DEPTH_COMPARE");
                dc_env = (e && *e == '0') ? 0 : 1;
            }
            if (dc_env && g.be->depth_snapshot && rs->depth_test &&
                rs->depth_func == 0x0202u && g.rsx.current_primitive >= 5u) {
                for (u32 u = RSX_DSP_NUM_TEXTURES; u-- > 0; )
                    if (!(((sampled_units | cube_mask | vol_mask) >> u) & 1u)) {
                        fs.depth_compare = 1;
                        fs.depth_compare_unit = (u8)u;
                        fs.depth_compare_24 =
                            ((rsx_dsp_reg(&g.rsx, 0x0208u) >> 5) & 7u) != 1u;
                        break;
                    }
            }
        }
        /* RPCS3 evaluate_programmable_blending_state (RSXGraphEngine.cpp:
         * 23-146, Vulkan supports_programmable_blending VKGSRender.cpp:636):
         * a signed BLEND_EQUATION (gcm_enums.h:187-189) on any target but the
         * X1/X8 _Z/_O formats blends in the shader (do_blend) against the
         * destination, hardware blending off. RPCS3 reads it as an input
         * attachment (VKRenderPass.cpp:378-390); here a copy of colour target
         * 0 taken before the draw is bound at a free unit. Single target,
         * no host MSAA. Otherwise, and with RSX_ENG_SIGNED_BLEND=0, the
         * hardware fallback of VKGSRender.cpp:171-180 runs (vk_gcm_blend_op). */
        {
            static int pb_env = -1;
            if (pb_env < 0) {
                const char* e = getenv("RSX_ENG_SIGNED_BLEND");
                pb_env = (e && *e == '0') ? 0 : 1;
            }
            const u32 er = rs->eq_rgb, ea = rs->eq_a;
            const int signed_eq = (er >= 0xF005u && er <= 0xF007u) || (ea >= 0xF005u && ea <= 0xF007u);
            int x_fmt = 0;
            switch (surface_fmt) {
            case 0x01u: case 0x02u: case 0x04u: case 0x05u: case 0x0Eu: case 0x0Fu: x_fmt = 1; break;
            default: break;
            }
            const int host_msaa = ((rsx_dsp_reg(&g.rsx, 0x0208u) >> 12) & 0xFu) != 0u &&
                                  (eng_hw_aa_caps() & RSX_BE_AA_HW_MSAA);
            /* The X1/X8 _Z/_O formats needing PB (eng_x_alpha_blend) blend in
             * the shader on the raw alpha with the guest's own factors, then
             * remap alpha (rop_remap). RSX_ENG_XALPHA_SHADER_PB=0 keeps the
             * fixed-function emulation. */
            static int xpb_env = -1;
            if (xpb_env < 0) {
                const char* e = getenv("RSX_ENG_XALPHA_SHADER_PB");
                xpb_env = (e && *e == '0') ? 0 : 1;
            }
            const int x_pb = x_fmt && rs->x_alpha_pb && xpb_env;
            u32 sf_rgb = rs->sf_rgb, sf_a = rs->sf_a, df_rgb = rs->df_rgb, df_a = rs->df_a;
            u32 eq_rgb = er, eq_a = ea;
            if (x_pb) {
                const u32 sfac = rsx_dsp_reg(&g.rsx, M_BLEND_SFACTOR);
                const u32 dfac = rsx_dsp_reg(&g.rsx, M_BLEND_DFACTOR);
                const u32 eq = rsx_dsp_reg(&g.rsx, M_BLEND_EQUATION);
                sf_rgb = sfac & 0xFFFFu; sf_a = sfac >> 16;
                df_rgb = dfac & 0xFFFFu; df_a = dfac >> 16;
                eq_rgb = eq & 0xFFFFu;   eq_a = eq >> 16;
            }
            if (((pb_env && signed_eq && !x_fmt) || x_pb) && !host_msaa && rt_count == 1u &&
                g.be->color_snapshot && (rs->blend_enable || x_pb) && !rs->logic_op_enable &&
                rs->color_mask)
                for (u32 u = RSX_DSP_NUM_TEXTURES; u-- > 0; ) {
                    if ((((sampled_units | cube_mask | vol_mask) >> u) & 1u) ||
                        (fs.depth_compare && fs.depth_compare_unit == u))
                        continue;
                    fs.pb = 1;
                    fs.pb_unit = (u8)u;
                    fs.pb_int = surface_fmt != 0x0Bu && surface_fmt != 0x0Cu && surface_fmt != 0x0Du;
                    fs.pb_eq[0] = (u16)eq_rgb; fs.pb_eq[1] = (u16)eq_a;
                    fs.pb_sf[0] = (u16)sf_rgb; fs.pb_sf[1] = (u16)sf_a;
                    fs.pb_df[0] = (u16)df_rgb; fs.pb_df[1] = (u16)df_a;
                    if (x_pb) fs.rop_remap = (u8)(rs->x_alpha_pb == 1u ? 3u : 4u);
                    const u32 f4[4] = { sf_rgb, sf_a, df_rgb, df_a };
                    int uses_k = 0;
                    for (u32 i = 0; i < 4; i++) uses_k |= f4[i] >= 0x8001u && f4[i] <= 0x8004u;
                    if (uses_k) eng_blend_constants(surface_fmt, fs.pb_const);
                    s_pb_rs = *rs;
                    s_pb_rs.blend_enable = 0;
                    s_pb_rs.blend_mrt = 0;
                    rs = &s_pb_rs;
                    break;
                }
        }
        /* TEX_COORD_CONTROL bit 4 asks for centroid interpolation on polygon
         * draws into an antialiased surface. RPCS3 has no centroid support
         * (pixel-centre interpolation), so this is opt-in: RSX_FP_CENTROID=1. */
        static int centroid_on = -1;
        if (centroid_on < 0) {
            const char* e = getenv("RSX_FP_CENTROID");
            centroid_on = e && *e && *e != '0';
        }
        if (centroid_on && ((rsx_dsp_reg(&g.rsx, 0x0208u) >> 12) & 15u) != 0u &&
            g.rsx.current_primitive >= 5u)
            for (u32 t = 0; t < 10; ++t)
                if ((inputs & (1u << (4u + t))) &&
                    (rsx_dsp_reg(&g.rsx, 0x0B40u + 4u * t) & 0x10u))
                    fs.centroid_mask |= (u16)(1u << t);
        static const rsx_fp_frag_state zero_fs;
        /* RPCS3 VKGSRender.cpp:645-649: point and line draws run the
         * low-precision compare fixup (shader set-ops and alpha test). */
        {
            const u32 prim = g.rsx.current_primitive;
            if (prim >= 1u && prim <= 4u && s_eng_host_nv) fs.low_precision_tests = 1;
            fs.host_not_nvidia = (u8)!s_eng_host_nv;
        }
        fs_any = memcmp(&fs, &zero_fs, sizeof fs) != 0;
    }
    u32 unnorm_dim[RSX_DSP_NUM_TEXTURES][2];
    u32 unnorm_depth[RSX_DSP_NUM_TEXTURES] = { 0 };
    /* Shadow helpers normalize against the bound resource themselves. Only
     * sampled ordinary UN units need baked dimensions or cache identity. */
    const u32 unnorm_mask = eng_unnorm_units(unnorm_dim) &
        (fixed ? 0xFFFFu : sampled_units) & ~shadow_mask;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++)
        if (!(unnorm_mask & (1u << u)))
            unnorm_dim[u][0] = unnorm_dim[u][1] = 0;
        else if (vol_mask & (1u << u)) {
            rsx_dsp_texture t;
            rsx_dsp_get_texture(&g.rsx, u, &t);
            unnorm_depth[u] = t.depth;
        }
    /* RPCS3 RSXGraphEngine.cpp:1265-1278: UN units with a NEAREST min or mag
     * filter sample at (x + 0.01) / size. */
    u32 unnorm_nearest = 0;
    if (unnorm_mask && !rsx_texture_legacy())
        for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
            if (!(unnorm_mask & (1u << u))) continue;
            rsx_dsp_texture t;
            rsx_dsp_get_texture(&g.rsx, u, &t);
            if (((t.filter >> 16) & 7u) == 1u || ((t.filter >> 24) & 7u) == 1u)
                unnorm_nearest |= 1u << u;
        }

    /* rsx_fp_collect_constants clears the block itself. */
    if (fixed || !pm_on) memset(&g.fp_constants, 0, sizeof g.fp_constants);
    if (!fixed && rsx_fp_collect_constants(fp_uc, fp_size, &g.fp_constants) < 0)
        return 0;

    /* Identity: the vertex program's own bytes, the fragment program's
     * STRUCTURE (its inline constants are hoisted into the buffered block, so
     * a constant change must not be a new pipeline), the export-width bit of
     * SHADER_CONTROL, the cube and vertex-texture masks, the input layout,
     * and the structural render state. */
    u64 key = 1469598103934665603ull;
    if (fixed) {
        static const u32 fixed_tag = 0x4E464958u;   /* "XIFN" */
        key = eng_fnv1a(&fixed_tag, sizeof fixed_tag, key);
    } else {
        key = eng_fnv1a(vp_uc, vp_instrs * 16u, key);
        /* RPCS3 vertex program hash: the live instructions and the entry. */
        key = eng_fnv1a(&s_eng_vp_info.entry, sizeof s_eng_vp_info.entry, key);
        key = eng_fnv1a(s_eng_vp_info.mask, sizeof s_eng_vp_info.mask, key);
        key = rsx_fp_structural_hash(fp_uc, fp_size, key);
        if (!key) return 0;
        const u32 fp_ctrl_key = fp_ctrl & 0x4Eu; /* colour width and fragment depth export */
        key = eng_fnv1a(&fp_ctrl_key, sizeof fp_ctrl_key, key);
        if (srgb_output) key = eng_fnv1a(&srgb_output, sizeof srgb_output, key);
        if (gamma_units) key = eng_fnv1a(gamma_masks, sizeof gamma_masks, key);
        if (expand_units) key = eng_fnv1a(expand_masks, sizeof expand_masks, key);
        key = eng_fnv1a(&cube_mask, sizeof cube_mask, key);
        if (vol_mask) key = eng_fnv1a(&vol_mask, sizeof vol_mask, key);
        if (tex_1d_mask) key = eng_fnv1a(&tex_1d_mask, sizeof tex_1d_mask, key);
        if (shadow_mask) {
            key = eng_fnv1a(&shadow_mask, sizeof shadow_mask, key);
            key = eng_fnv1a(shadow, sizeof shadow, key);
        }
        key = eng_fnv1a(&vtex_mask, sizeof vtex_mask, key);
        if (vtex_1d_mask) key = eng_fnv1a(&vtex_1d_mask, sizeof vtex_1d_mask, key);
        if (vtex_3d_mask) key = eng_fnv1a(&vtex_3d_mask, sizeof vtex_3d_mask, key);
        if (vtex_cube_mask) key = eng_fnv1a(&vtex_cube_mask, sizeof vtex_cube_mask, key);
        if (vtex_un_mask) key = eng_fnv1a(&vtex_un_mask, sizeof vtex_un_mask, key);
        key = eng_fnv1a(&unnorm_mask, sizeof unnorm_mask, key);
        if (unnorm_mask) {
            key = eng_fnv1a(unnorm_dim, sizeof unnorm_dim, key);
            if (unnorm_mask & vol_mask) key = eng_fnv1a(unnorm_depth, sizeof unnorm_depth, key);
        }
        if (unnorm_nearest) key = eng_fnv1a(&unnorm_nearest, sizeof unnorm_nearest, key);
        if (fs_any) key = eng_fnv1a(&fs, sizeof fs, key);
    }
    key = eng_fnv1a(&layout->mask, sizeof layout->mask, key);
    key = eng_fnv1a(&layout->stride, sizeof layout->stride, key);
    key = eng_fnv1a(&rt_fmt, sizeof rt_fmt, key);
    /* How many colour attachments the pass will have is pipeline identity:
     * a host API matches the two against each other. */
    key = eng_fnv1a(&rt_count, sizeof rt_count, key);
    key = rsx_draw_engine_hash_render_state(rs, key);

    {
        const int pi = eng_pcache_find(key);
        if (pi >= 0) {
            const u32 i = (u32)pi;
            *out_fixed = g.pipelines[i].fixed;
            *out_tex_used = g.pipelines[i].tex_used;
            *out_fp_hash = g.pipelines[i].fp_hash;
            s_eng_depth_cmp_unit = g.pipelines[i].depth_cmp_unit;
            s_eng_pb_unit = g.pipelines[i].pb_unit;
            s_eng_pipe_idx = i;
            if (pm_on) eng_pmemo_store(layout, rs_in, rt_fmt, rt_count, fixed, fp_uc,
                                       fp_size, pm_recheck, pm_bx2, i);
            return g.pipelines[i].handle;
        }
    }
    if (g.n_pipelines >= ENG_MAX_PIPELINES) return 0;

    u32 handle = 0;
    int vi = 1, fi = 1;
    u32 nconst = 0;
    if (fixed) {
        snprintf(s_vs_hlsl, sizeof s_vs_hlsl, "%s", kEngFixedVS);
        snprintf(s_ps_hlsl, sizeof s_ps_hlsl, "%s", kEngFixedPS);
    } else {
        rsx_vp_set_vtex_1d_mask(vtex_1d_mask);
        rsx_vp_set_vtex_3d_mask(vtex_3d_mask);
        rsx_vp_set_vtex_cube_mask(vtex_cube_mask);
        rsx_vp_set_vtex_unnorm(vtex_un_mask, NULL);
        vi = rsx_vp_decompile_program((const u8*)g.rsx.vp, RSX_DSP_VP_INSTR,
                                      rsx_dsp_vp_start(&g.rsx), vtex_mask,
                                      layout->mask, 1, s_vs_hlsl,
                                      sizeof s_vs_hlsl);
        rsx_vp_set_vtex_1d_mask(0);
        rsx_vp_set_vtex_3d_mask(0);
        rsx_vp_set_vtex_cube_mask(0);
        rsx_vp_set_vtex_unnorm(0, NULL);
        rsx_fp_set_tex_1d_mask(tex_1d_mask);
        fi = rsx_fp_decompile_engine(fp_uc, fp_size, fp_ctrl, cube_mask, vol_mask,
                shadow_mask ? shadow : NULL, gamma_masks, expand_units ? expand_masks : NULL,
                fs_any ? &fs : NULL, s_ps_hlsl, sizeof s_ps_hlsl, &nconst);
        rsx_fp_set_tex_1d_mask(0);
        if (vi > 0 && fs.two_sided &&
            rsx_fp_patch_vs_back_colors(s_vs_hlsl, sizeof s_vs_hlsl) < 0)
            vi = -1;
        if (fi > 0 && nconst != g.fp_constants.count) fi = -1;
        if (fi > 0 && rsx_fp_apply_unnorm_scale_vol(s_ps_hlsl, sizeof s_ps_hlsl, unnorm_mask,
                                                (const u32 (*)[2])unnorm_dim, cube_mask,
                                                unnorm_nearest, vol_mask, unnorm_depth) < 0)
            fi = -1;
        if (fi > 0 && srgb_output &&
            rsx_fp_apply_srgb_output(s_ps_hlsl, sizeof s_ps_hlsl) < 0)
            fi = -1;
        /* RPCS3 ROP epilogue order: sRGB, 8-bit rounding, alpha test, A2C. */
        if (fi > 0 && fs_any && rsx_fp_apply_rop_stage(s_ps_hlsl, sizeof s_ps_hlsl, &fs, 0) < 0)
            fi = -1;
        if (fi > 0 && rs->alpha_test_enable &&
            rsx_fp_apply_alpha_test_buffered(s_ps_hlsl, sizeof s_ps_hlsl,
                                             rs->alpha_func) < 0)
            fi = -1;
        if (fi > 0 && fs_any && rsx_fp_apply_rop_stage(s_ps_hlsl, sizeof s_ps_hlsl, &fs, 1) < 0)
            fi = -1;
    }
    if (vi > 0 && fi > 0)
        handle = g.be->pipeline_create(g.be->user, s_vs_hlsl, s_ps_hlsl, rs,
                                       layout, layout->stride, rt_fmt, rt_count);
    const u64 fp_hash = fixed ? 0 : eng_fp_debug_hash(fp_uc, fp_size);
    /* RSX_ENGINE_SHADER_DUMP=<existing dir>: each new pipeline's two programs
     * as <dir>/pipe<handle>.vs.hlsl and .ps.hlsl, the handle being the one
     * RSX_ENGINE_PASS_DUMP's index names, and a guest fragment program's own
     * ucode as .fp.bin with its address and debug hash (the value
     * RSX_FP_DEBUG, RSX_ENGINE_SKIP_FP and RSX_ENGINE_REGDUMP_FP take) in
     * .fp.txt. A failed pipeline is pipe0. */
    {
        static const char* dir_c = NULL;
        static int dir_init = 0;
        if (!dir_init) { dir_c = getenv("RSX_ENGINE_SHADER_DUMP"); dir_init = 1; }
        const char* dir = dir_c;
        if (dir && *dir) {
            char path[1024];
            FILE* f;
            snprintf(path, sizeof path, "%s/pipe%u.vs.hlsl", dir, handle);
            if ((f = fopen(path, "w"))) { fputs(s_vs_hlsl, f); fclose(f); }
            snprintf(path, sizeof path, "%s/pipe%u.ps.hlsl", dir, handle);
            if ((f = fopen(path, "w"))) { fputs(s_ps_hlsl, f); fclose(f); }
            if (!fixed && fp_uc && fp_size) {
                u32 fp_loc = 0;
                const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
                snprintf(path, sizeof path, "%s/pipe%u.fp.bin", dir, handle);
                if ((f = fopen(path, "wb"))) { fwrite(fp_uc, 1, fp_size, f); fclose(f); }
                snprintf(path, sizeof path, "%s/pipe%u.fp.txt", dir, handle);
                if ((f = fopen(path, "w"))) {
                    fprintf(f, "location %u offset 0x%08X size %u ctrl 0x%08X\n"
                               "fp %016llx\n",
                            fp_loc, fp_off, fp_size, fp_ctrl,
                            (unsigned long long)fp_hash);
                    fclose(f);
                }
            }
        }
    }
    { static u32 logs = 0; if (PS3_LOG_FIRST(logs, 32))
        fprintf(stderr, "[rsx engine] pipeline %016llx: %s vp %d, fp %d,"
                        " %u constants, fp %016llx -> %s\n",
                (unsigned long long)key, fixed ? "built-in" : "guest",
                vi, fi, nconst, (unsigned long long)fp_hash,
                handle ? "ok" : "FAILED (draw dropped)"); }

    g.pipelines[g.n_pipelines].key = key;
    g.pipelines[g.n_pipelines].fp_hash = fp_hash;
    g.pipelines[g.n_pipelines].handle = handle;
    g.pipelines[g.n_pipelines].fixed = (u8)fixed;
    g.pipelines[g.n_pipelines].depth_cmp_unit =
        (u8)((!fixed && fs.depth_compare) ? fs.depth_compare_unit + 1u : 0u);
    s_eng_depth_cmp_unit = g.pipelines[g.n_pipelines].depth_cmp_unit;
    g.pipelines[g.n_pipelines].pb_unit = (u8)((!fixed && fs.pb) ? fs.pb_unit + 1u : 0u);
    s_eng_pb_unit = g.pipelines[g.n_pipelines].pb_unit;
    g.pipelines[g.n_pipelines].tex_used =
        (u16)(fixed ? 0xFFFFu : eng_sampled_units(s_ps_hlsl));
    *out_tex_used = g.pipelines[g.n_pipelines].tex_used;
    *out_fp_hash = fp_hash;
    g.pipelines[g.n_pipelines].topo_mask = 0;
    s_eng_pipe_idx = g.n_pipelines;
    eng_pcache_append(&g.pipelines[g.n_pipelines], rs, layout, rt_fmt, rt_count);
    g.n_pipelines++;
    *out_fixed = fixed;
    if (pm_on) eng_pmemo_store(layout, rs_in, rt_fmt, rt_count, fixed, fp_uc, fp_size,
                               pm_recheck, pm_bx2, s_eng_pipe_idx);
    return handle;
}

static u32 eng_pipeline_get(const rsx_vertex_layout_plan* layout, const rsx_be_render_state* rs, rsx_be_format rt_fmt, u32 rt_count, int* out_fixed, u32* out_tex_used, u64* out_fp_hash)
{
    const u64 t0 = eng_prof_t0();
    const u32 r_ = eng_pipeline_get_impl(layout, rs, rt_fmt, rt_count, out_fixed, out_tex_used, out_fp_hash);
    eng_prof_add(2, t0);
    return r_;
}

/* ---- draw accumulation --------------------------------------------------- */

static void dc_reset(void)
{
    dc.n_arr = dc.n_idx = dc.n_packets = 0;
    dc.n_refs = dc.n_source_refs = dc.n_verts = dc.n_cuts = 0;
    dc.refs_remapped = 0;
    dc.fetch_ok = 1;
    dc.inl = NULL;
    dc.inl_bytes = 0;
    dc.n_immediate = 0;
    dc.elem = NULL;
    dc.n_elem = 0;
}

static int dc_push_ref(u32 vertex_id, u32 base_index)
{
    if (dc.n_refs >= dc.cap_refs) {
        const u32 next = dc.cap_refs ? dc.cap_refs * 2u : 4096u;
        rsx_vertex_ref* r = (rsx_vertex_ref*)realloc(dc.refs,
                                                     (size_t)next * sizeof(*r));
        if (!r) return 0;
        dc.refs = r;
        dc.cap_refs = next;
    }
    dc.refs[dc.n_refs].vertex_id = vertex_id;
    dc.refs[dc.n_refs].base_index = base_index;
    dc.n_refs++;
    return 1;
}

static int dc_reserve_verts(u64 bytes)
{
    if (bytes <= dc.verts_cap) return 1;
    u64 next = dc.verts_cap ? dc.verts_cap : (1u << 20);
    while (next < bytes) next *= 2u;
    u8* v = (u8*)realloc(dc.verts, (size_t)next);
    if (!v) return 0;
    dc.verts = v;
    dc.verts_cap = next;
    return 1;
}

static int dc_reserve_indices(u32 count)
{
    if (count <= g.index_cap) return 1;
    u32 next = g.index_cap ? g.index_cap : 4096u;
    while (next < count) next *= 2u;
    u32* p = (u32*)realloc(g.indices, (size_t)next * sizeof(u32));
    if (!p) return 0;
    g.indices = p;
    g.index_cap = next;
    return 1;
}

/* Turn this group's batches into a reference list, recording a CUT rather
 * than fetching a phantom vertex wherever the restart sentinel appears, then
 * collapse repeated references and fetch each unique vertex once.
 * rsx_live_draw.c's fetch_batches_hoisted (4165-4292). */
static void dc_fetch(const rsx_vertex_layout_plan* layout, int allow_remap)
{
    if (dc.n_immediate) {
        dc.n_source_refs = dc.n_immediate;
        if (!dc_reserve_verts((u64)dc.n_immediate * layout->stride)) {
            dc.fetch_ok = 0;
            return;
        }
        /* RPCS3 analyse_inputs_interleaved: an attribute outside
         * VERTEX_ATTRIB_INPUT_MASK is not fed at all, push buffer or not. */
        const u32 in_mask = (g.rsx.seen[0x1FF0 >> 2] && !rsx_vertex_legacy())
            ? (rsx_dsp_reg(&g.rsx, 0x1FF0) & 0xFFFFu) : 0xFFFFu;
        static const float k_unfed[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        for (u32 v = 0; v < dc.n_immediate; v++)
            for (u32 slot = 0; slot < layout->count; slot++)
                memcpy(dc.verts + (u64)v * layout->stride + slot * 16u,
                       (in_mask >> layout->attrs[slot]) & 1u
                           ? dc.immediate[v][layout->attrs[slot]] : k_unfed,
                       16u);
        dc.n_verts = dc.n_immediate;
        return;
    }
    for (u32 b = 0; b < dc.n_arr && dc.fetch_ok; b++)
        for (u32 i = 0; i < dc.arr[b].count && dc.fetch_ok; i++)
            if (!dc_push_ref(dc.arr[b].first + i, 0)) dc.fetch_ok = 0;

    /* ARRAY_ELEMENT indices: RPCS3 uploads the push buffer as a u32 index
     * array regardless of INDEX_ARRAY_FORMAT, though restart_index_enabled()
     * still consults that format; the base index applies as for any indexed
     * draw. */
    u32 idx_min = 0xFFFFFFFFu, idx_max = 0;
    if (dc.n_elem && dc.fetch_ok) {
        const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
        rsx_dsp_index_array eia;
        rsx_dsp_get_index_array(&g.rsx, &eia);
        const int restart_en = rsx_dsp_restart_index_enabled(&g.rsx, eia.is_u32);
        const u32 restart_val = rsx_dsp_restart_index(&g.rsx);
        for (u32 i = 0; i < dc.n_elem && dc.fetch_ok; i++) {
            if (restart_en && dc.elem[i] == restart_val) {
                if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts,
                                          dc.n_refs))
                    dc.fetch_ok = 0;
                continue;
            }
            if (dc.elem[i] < idx_min) idx_min = dc.elem[i];
            if (dc.elem[i] > idx_max) idx_max = dc.elem[i];
            if (!dc_push_ref(dc.elem[i], base_index)) dc.fetch_ok = 0;
        }
    }

    if (dc.n_idx && dc.fetch_ok) {
        const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
        rsx_dsp_index_array ia;
        rsx_dsp_get_index_array(&g.rsx, &ia);
        const u32 esz = ia.is_u32 ? 4u : 2u;
        const int restart_en = rsx_dsp_restart_index_enabled(&g.rsx, ia.is_u32);
        const u32 restart_val = rsx_dsp_restart_index(&g.rsx);
        /* The index array's location field is a DMA selector, not the
         * rsx_dsp location enum: 0 selects local memory. */
        const u32 ia_loc = ia.location ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
        /* "Force aligned indices as realhw" (RPCS3 get_raw_index_array). */
        ia.offset &= ~(esz - 1u);
        for (u32 b = 0; b < dc.n_idx && dc.fetch_ok; b++) {
            const u32 first = dc.idx[b].first, count = dc.idx[b].count;
            const u64 start = (u64)ia.offset + (u64)first * esz;
            const u64 bytes = (u64)count * esz;
            const u8* run = NULL;
            if (count && start <= 0xFFFFFFFFull && bytes <= 0xFFFFFFFFull &&
                start + bytes <= 0x100000000ull)
                run = eng_guest_ptr(NULL, ia_loc, (u32)start, (u32)bytes);
            for (u32 i = 0; i < count && dc.fetch_ok; i++) {
                const u8* p = run ? run + (size_t)i * esz
                    : eng_guest_ptr(NULL, ia_loc, ia.offset + (first + i) * esz, esz);
                if (!p) { dc.fetch_ok = 0; break; }
                const u32 index = ia.is_u32
                    ? (((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3])
                    : (u32)((p[0] << 8) | p[1]);
                if (restart_en && index == restart_val) {
                    if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts,
                                              dc.n_refs))
                        dc.fetch_ok = 0;
                    continue;
                }
                if (index < idx_min) idx_min = index;
                if (index > idx_max) idx_max = index;
                if (!dc_push_ref(index, base_index)) dc.fetch_ok = 0;
            }
        }
    }
    if (!dc.fetch_ok) return;
    /* RPCS3 VKVertexBuffers.cpp draw_indexed_array_command: an index set whose
     * non-restart min/max are inverted (empty) or equal (single vertex, unless
     * points) is not drawn. */
    if (!dc.n_arr && (dc.n_idx || dc.n_elem) && !rsx_vertex_legacy() &&
        (idx_min > idx_max ||
         (idx_min == idx_max && g.rsx.current_primitive != RSX_PRIMITIVE_POINTS))) {
        dc.n_refs = 0;
        dc.n_verts = 0;
        return;
    }

    dc.n_source_refs = dc.n_refs;
    /* Only a topology this engine rebuilds through an index buffer may share
     * repeated references. A line strip or a point list is drawn in the order
     * it arrived, so collapsing duplicates would reorder it. */
    if (allow_remap && dc.n_refs > 1) {
        u32 unique = dc.n_refs;
        if (rsx_vertex_remap_build(&dc.ref_remap, dc.refs, dc.n_refs, &unique)) {
            dc.refs_remapped = unique < dc.n_refs;
            dc.n_refs = unique;
        }
    }

    dc.layout = *layout;
    rsx_vertex_fetch_plan_init(&dc.fetch_plan, &g.rsx, layout, eng_guest_ptr, NULL);
    rsx_vertex_fetch_plan_set_inline(&dc.fetch_plan, &g.rsx, dc.inl, dc.inl_bytes);
    if (!dc.inl)
        dc.fetch_plan.freq_mode = (dc.n_idx || dc.n_elem)
            ? RSX_VERTEX_FREQ_RPCS3_INDEXED : RSX_VERTEX_FREQ_RPCS3_ARRAYS;
    rsx_vertex_fetch_plan_prepare(&dc.fetch_plan, dc.refs, dc.n_refs);
    if (!dc_reserve_verts((u64)dc.n_refs * layout->stride)) {
        dc.fetch_ok = 0;
        return;
    }
    if (layout->stride && dc.verts) {
        if (!rsx_vertex_fetch_many(&dc.fetch_plan, dc.refs, dc.n_refs, dc.verts)) {
            dc.fetch_ok = 0;
            return;
        }
    } else for (u32 i = 0; i < dc.n_refs; i++) {
        u8* dst = layout->stride ? dc.verts + (u64)i * layout->stride : NULL;
        if (!rsx_vertex_fetch_one(&dc.fetch_plan, &dc.refs[i], dst)) {
            dc.fetch_ok = 0;
            return;
        }
    }
    dc.n_verts = dc.n_refs;
}

/* ---- sink ---------------------------------------------------------------- */

static void sink_begin(void* user, const rsx_dispatch* r, u32 prim)
{
    (void)user; (void)prim;
    dc_reset();
    dc.inline_dropped_at_begin = r->inline_dropped;
    dc.elem_dropped_at_begin = r->elem_dropped;
}

/* ARRAY_ELEMENT16/32: RPCS3 set_begin_end draws the pushed indices only when
 * the group issued no DRAW_ARRAYS / DRAW_INDEX_ARRAY batch (command none). */
static void sink_array_elements(void* user, const rsx_dispatch* r,
                                const u32* indices, u32 count)
{
    (void)user; (void)r;
    if (dc.n_arr || dc.n_idx || !count) return;
    dc.elem = indices;
    dc.n_elem = count;
    dc.n_packets++;
}

/* A malformed/unbounded group must not render a truncated primitive. */
#define ENG_MAX_IMMEDIATE_VERTICES 4096u
static void eng_imm_snapshot(const rsx_dispatch* r, float (*v)[4])
{
    /* RPCS3 append_to_push_buffer: only the program's inputs (0x1FF0).
     * RSX_IMM_INPUT_MASK=0 snapshots every attribute, as before. */
    static int imask_on = -1;
    if (imask_on < 0) {
        const char* e = getenv("RSX_IMM_INPUT_MASK");
        imask_on = !(e && *e == '0');
    }
    const u32 in_mask = imask_on ? rsx_dsp_reg(r, 0x1FF0u) : 0xFFFFu;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        float* value = v[attr];
        rsx_dsp_vertex_default(r, attr, value);
        if (!(in_mask & (1u << attr))) continue;
        /* The legacy default getter treats all-zero words as unwritten.
         * Direct and folded constants may explicitly set W/alpha to zero. */
        const u32 base = (0x1C00u + attr * 16u) >> 2;
        if (r->seen[base] || r->seen[base + 1] ||
            r->seen[base + 2] || r->seen[base + 3])
            memcpy(value, &r->regs[base], 16u);
    }
}

static void sink_immediate_vertex(void* user, const rsx_dispatch* r)
{
    (void)user;
    dc.n_packets++;
    if (!dc.fetch_ok) return;
    if (dc.n_immediate == ENG_MAX_IMMEDIATE_VERTICES) {
        dc.fetch_ok = 0;
        return;
    }
    if (dc.n_immediate == dc.cap_immediate) {
        const u32 next = dc.cap_immediate ? dc.cap_immediate * 2u : 64u;
        void* p = realloc(dc.immediate, (size_t)next * sizeof(*dc.immediate));
        if (!p) { dc.fetch_ok = 0; return; }
        dc.immediate = p;
        dc.cap_immediate = next;
    }
    eng_imm_snapshot(r, dc.immediate[dc.n_immediate]);
    dc.n_immediate++;
}

static void sink_draw_arrays(void* user, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)user; (void)r;
    dc.n_packets++;
    if (dc.n_arr >= ENG_MAX_BATCHES) return;
    dc.arr[dc.n_arr].first = first;
    dc.arr[dc.n_arr].count = count;
    dc.n_arr++;
}

/* INLINE_ARRAY delivers one finished stream rather than a (first, count)
 * range; queue it as an ordinary consecutive batch and let the fetch plan
 * read the vertices out of it (rsx_vertex_fetch_plan_set_inline). */
static void sink_inline_array(void* user, const rsx_dispatch* r,
                              const u8* data, u32 bytes)
{
    (void)user;
    /* Record source presence even when its declaration or length is invalid,
     * so mixed immediate/inline groups cannot draw a partial prefix. */
    dc.inl = data;
    dc.inl_bytes = bytes;
    const u32 stride = rsx_vertex_inline_layout(r, NULL);
    if (!stride || bytes < stride) return;
    dc.n_packets++;
    if (dc.n_arr >= ENG_MAX_BATCHES) return;
    dc.arr[dc.n_arr].first = 0;
    dc.arr[dc.n_arr].count = bytes / stride;
    dc.n_arr++;
}

static void sink_draw_index(void* user, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)user; (void)r;
    dc.n_packets++;
    if (dc.n_idx >= ENG_MAX_BATCHES) return;
    dc.idx[dc.n_idx].first = first;
    dc.idx[dc.n_idx].count = count;
    dc.n_idx++;
}

/* Which texture units this draw binds, and from where: a unit naming a
 * registered colour surface samples that live target rather than guest
 * memory (every post-process, shadow and reflection in a PS3 title is a
 * render-to-texture read back through a texture unit), a unit naming a
 * tracked zeta in DEPTH24_D8 samples its snapshot, and everything else is a
 * guest upload (rsx_live_draw.c:5986-6062). */
/* A direct copy is valid only for the entire matching physical sample
 * layout. Reinterpreted, swizzled, mipmapped and subrectangle aliases need a
 * different conversion and must not silently sample stale guest bytes. */
static int eng_color_feedback_compatible(const eng_surface* s,
                                          const rsx_dsp_texture* t,
                                          const rsx_dsp_surface* sf)
{
    const u32 base = t->format & 0x9fu;
    u32 bpp = 0, color = 0;
    switch (s->fmt) {
    case RSX_BE_FMT_R8G8B8A8:        if (base == 0x85u) { bpp = 4; color = 8; } break;
    case RSX_BE_FMT_R16G16B16A16F:   if (base == 0x9au) { bpp = 8; color = 11; } break;
    case RSX_BE_FMT_R32G32B32A32F:   if (base == 0x9bu) { bpp = 16; color = 12; } break;
    case RSX_BE_FMT_R32F:            if (base == 0x9cu) { bpp = 4; color = 13; } break;
    default: break;
    }
    if (!bpp || sf->color_format != color || sf->raster_type != 1 ||
        !(t->format & 0x20u) || t->dimension != 2 ||
        t->cubemap || t->mipmaps > 1 || t->depth > 1 ||
        t->width != s->w || t->height != s->h) return 0;
    const u64 row = (u64)s->w * bpp;
    const u64 sp = s->pitch ? s->pitch : row;
    const u64 tp = t->pitch ? t->pitch : row;
    return row && sp >= row && tp == sp;
}

/* RPCS3 never drops a draw over a texture its framebuffer copy cannot
 * serve: upload_texture flushes the overlapping targets (read barrier) and
 * decodes what reached memory. Only a failed upload rejects the draw. */
static u32 eng_color_feedback_failure(u32 unit, const eng_surface* s,
                                       const rsx_dsp_texture* t)
{
    static u32 logs;
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    u32 tex = 0;
    if (!eng_legacy_surfaces()) {
        eng_surface_to_guest((u32)(s - g.surfaces));
        tex = eng_texture_slot(t->location, t->offset, eng_tex_lfmt(t), t->width, t->height,
                               t->mipmaps, t->pitch, (int)t->cubemap, t->remap & 0xFFFFu,
                               (t->dimension == 3 && !t->cubemap) ? t->depth : 1u);
    }
    if (PS3_LOG_FIRST(logs, 8))
        fprintf(stderr, "[rsx engine] colour feedback unit %u %s: "
                "target %ux%u fmt%u pitch%u, texture %ux%u fmt%02X pitch%u "
                "dim%u levels%u cube%u color%u raster%u\n", unit,
                tex ? "read through guest memory" : "rejected", s->w, s->h, (u32)s->fmt,
                s->pitch, t->width, t->height, t->format, t->pitch,
                t->dimension, t->mipmaps, t->cubemap, sf.color_format, sf.raster_type);
    return tex ? tex : ENG_INVALID;
}

/* A texture naming a surface's address and pitch but a smaller extent reads
 * the top-left (w, h) of its sample grid, as RPCS3's framebuffer sampling
 * copies that subregion (texture_cache_helpers.h, process_framebuffer_
 * resource_fast). Binding the whole target instead stretches the image: R2
 * renders 1152x640 into a 1280x704 target and upscales it from there. Returns
 * the cached copy for the surface's current contents, or 0 to bind as before. */
static u32 eng_tc_pitch(const rsx_dsp_texture* t, u32* packed);
static u32 eng_tc_surface_pitch(const eng_surface* s);
static u32 eng_surface_region(eng_surface* s, const rsx_dsp_texture* t)
{
    if (!g.be->color_region_snapshot || t->dimension != 2 || t->cubemap ||
        !t->width || !t->height)
        return 0;
    /* An antialiased surface is held at sample resolution. RPCS3
     * check_framebuffer_resource / get_surface_info compare the texture
     * against the surface in samples (get_surface_width<samples>) and clip
     * the top-left width x height samples: the texture reads the raw sample
     * grid exactly as guest memory lays it out. RSX_TEX_LEGACY=1 keeps the
     * old pixel-extent doubling. */
    u32 rw = t->width, rh = t->height;
    if (rsx_texture_legacy() && (s->aa_sx > 1 || s->aa_sy > 1)) {
        if (rw * s->aa_sx <= s->w && rh * s->aa_sy <= s->h) {
            rw *= s->aa_sx;
            rh *= s->aa_sy;
        }
    }
    if (rw > s->w || rh > s->h || (rw == s->w && rh == s->h))
        return 0;
    if (s_texcache_legacy) {
        if (t->pitch && s->pitch && t->pitch != s->pitch) return 0;
    } else {
        /* RPCS3 check_framebuffer_resource -> pitch_compatible: the texture's
         * pitch (its packed row for pitch 0 or a swizzled layout) is the
         * surface's, unless either is a single row. */
        u32 packed;
        const u32 tp = eng_tc_pitch(t, &packed);
        /* (A compressed AA tile's resolve -- below, beyond RPCS3, which has
         * no tile compression -- keeps the pitch-0 match.) */
        const int tile_aa = !t->pitch && (s->aa_sx > 1u || s->aa_sy > 1u) &&
                            eng_tile_aa_at(s->location, s->offset, s->pitch);
        if (!tile_aa &&
            (!tp || (rh != 1u && s->h != 1u && tp != eng_tc_surface_pitch(s)))) return 0;
    }
    {
        const u32 base = t->format & 0x9fu;
        const int same = (s->fmt == RSX_BE_FMT_R8G8B8A8 && base == 0x85u) ||
                         (s->fmt == RSX_BE_FMT_R16G16B16A16F && base == 0x9au) ||
                         (s->fmt == RSX_BE_FMT_R32G32B32A32F && base == 0x9bu) ||
                         (s->fmt == RSX_BE_FMT_R32F && base == 0x9cu);
        if (!same) return 0;
    }
    /* Compressed AA tile (eng_tile_aa_at): a texture of the pixel extent
     * reads the picture, each texel the average of its sample group. */
    if (!rsx_texture_legacy() && (s->aa_sx > 1u || s->aa_sy > 1u) &&
        t->width * s->aa_sx <= s->w && t->height * s->aa_sy <= s->h &&
        g.be->color_target_create && g.be->color_target_release &&
        eng_tile_aa_at(s->location, s->offset, s->pitch)) {
        const u32 tw = t->width, th = t->height;
        if (s->region && s->region_resolved == 1 && s->region_w == tw && s->region_h == th &&
            s->region_serial == s->color_serial)
            return s->region;
        const u32 tmp = g.be->color_target_create(g.be->user, eng_hfmt(s), tw, th, NULL, 0);
        if (!tmp) return 0;
        if (g.be->color_target_blit_filtered)
            g.be->color_target_blit_filtered(g.be->user, s->handle, 0, 0, tw * s->aa_sx,
                                             th * s->aa_sy, tmp, 0, 0, tw, th, 1);
        else if (g.be->color_target_blit)
            g.be->color_target_blit(g.be->user, s->handle, 0, 0, tw * s->aa_sx, th * s->aa_sy,
                                    tmp, 0, 0, tw, th);
        const u32 copy = (g.be->color_target_blit_filtered || g.be->color_target_blit)
            ? g.be->color_region_snapshot(g.be->user, tmp, 0, 0, tw, th) : 0;
        g.be->color_target_release(g.be->user, tmp);
        if (!copy) return 0;
        if (s->region) g.be->texture_release(g.be->user, s->region);
        s->region = copy;
        s->region_resolved = 1;
        s->region_w = tw;
        s->region_h = th;
        s->region_serial = s->color_serial;
        return copy;
    }
    if (s->region && !s->region_resolved && s->region_w == rw && s->region_h == rh &&
        s->region_serial == s->color_serial)
        return s->region;
    const u32 copy = g.be->color_region_snapshot(g.be->user, s->handle, 0, 0, rw, rh);
    if (!copy) return 0;
    if (s->region) g.be->texture_release(g.be->user, s->region);
    s->region = copy;
    s->region_resolved = 0;
    s->region_w = rw;
    s->region_h = rh;
    s->region_serial = s->color_serial;
    return copy;
}

static int eng_surface_tex_same_fmt(const eng_surface* s, u32 format)
{
    const u32 base = format & 0x9fu;
    return (s->fmt == RSX_BE_FMT_R8G8B8A8 && base == 0x85u) ||
           (s->fmt == RSX_BE_FMT_R16G16B16A16F && base == 0x9au) ||
           (s->fmt == RSX_BE_FMT_R32G32B32A32F && base == 0x9bu) ||
           (s->fmt == RSX_BE_FMT_R32F && base == 0x9cu);
}

/* A texture at a surface's address and pitch reaching past it. RPCS3
 * fast_texture_search: check_framebuffer_resource fails on the size, so
 * get_merged_texture_memory_region collects every surface over the range
 * (the newest first, a failing memory tag skipped), and merge_cache_resources
 * makes an atlas_gather (VKTextureCache.cpp generate_atlas_from_images): an
 * image initialised from guest memory when the sections leave holes, then
 * every section copied in at its byte position, oldest to newest. The draw
 * is never dropped.
 * Compressed AA tile (eng_tile_aa_factor): a non-AA picture read as the
 * tile's sample grid -- exactly its width x height times the grid -- reads
 * every pixel replicated to its samples (the inverse of the tile resolve).
 * Returns an immutable copy (cached in s->region, region_resolved 2), or 0. */
static u32 eng_surface_gather(eng_surface* s, const rsx_dsp_texture* t)
{
    if (rsx_texture_legacy() || eng_legacy_surfaces() || !g.be->color_region_snapshot ||
        !g.be->color_target_create || !g.be->color_target_release ||
        (!g.be->color_target_blit && !g.be->color_target_blit_filtered) ||
        t->dimension != 2 || t->cubemap || t->mipmaps > 1u || t->depth > 1u ||
        !t->width || !t->height || t->width > ENG_MAX_SURFACE_DIM ||
        t->height > ENG_MAX_SURFACE_DIM || s->swz || s->location >= ENG_LOC_SCRATCH ||
        (t->width <= s->w && t->height <= s->h) || !eng_surface_tex_same_fmt(s, t->format))
        return 0;
    const u32 bpp = eng_color_bytes(s);
    const u32 pitch = t->pitch ? t->pitch : s->pitch;
    if (!pitch || (s->pitch && pitch != s->pitch) || t->location != s->location) return 0;
    const u32 tw = t->width, th = t->height;
    u32 fx = 1u, fy = 1u;
    const int tile = s->aa_sx == 1u && s->aa_sy == 1u && t->offset == s->offset &&
                     eng_tile_aa_factor(s->location, s->offset, pitch, &fx, &fy) &&
                     !(tw % fx) && !(th % fy) && tw / fx <= s->w && th / fy <= s->h;
    /* Sources, oldest first; key: the newest write among them. */
    u32 src[ENG_MAX_SURFACES], n = 0;
    u64 key = s->color_serial;
    int complete = 0;
    if (tile) {
        src[n++] = (u32)(s - g.surfaces);
        complete = 1;
    } else {
        for (u32 i = 0; i < g.n_surfaces; i++) {
            const eng_surface* o = &g.surfaces[i];
            eng_isect r;
            if (!o->handle || o->swz || o->location != s->location || !o->color_serial ||
                o->fmt != s->fmt || o->pitch != pitch || eng_color_bytes(o) != bpp ||
                !eng_intersect(o->offset, o->w * bpp, o->h, t->offset, tw * bpp, th, pitch, &r))
                continue;
            if (o != s && !eng_surface_tag_test(o)) continue;
            if (!r.dxb && !r.dy && r.wb >= tw * bpp && r.rows >= th) complete = 1;
            if (o->color_serial > key) key = o->color_serial;
            u32 k = n++;
            while (k && g.surfaces[src[k - 1]].color_serial > o->color_serial) {
                src[k] = src[k - 1];
                k--;
            }
            src[k] = i;
        }
        if (!n) return 0;
    }
    if (s->region && s->region_resolved == 2 && s->region_w == tw && s->region_h == th &&
        s->region_serial == key && complete)
        return s->region;
    /* force_bg_load: the holes read guest memory (A8R8G8B8 in IO-mapped main
     * memory, as eng_surface_seed can); otherwise they stay 0. */
    const void* seed = NULL;
    u32 seed_row = 0;
    if (!complete && s->fmt == RSX_BE_FMT_R8G8B8A8 && s->location == RSX_LOCATION_MAIN &&
        (s_guest_reader || cellGcmResolveIO(t->offset))) {
        u8* rows = eng_coherence_buffer(tw * 4u * th);
        const int rgba = rsx_texture_argb_is_rgba();
        for (u32 r = 0; rows && r < th; r++) {
            const u8* p = eng_guest_image(t->location, t->offset + r * eng_tiled_row_pitch(t->location, t->offset, pitch), tw * 4u);
            if (!p) { rows = NULL; break; }
            eng_row_convert(rows + (size_t)r * tw * 4u, p, tw, rgba, 0);
        }
        if (rows) { seed = rows; seed_row = tw * 4u; }
    } else if (!complete && !s_texcache_legacy && s->fmt != RSX_BE_FMT_R8G8B8A8 &&
               s->location == RSX_LOCATION_MAIN &&
               (s_guest_reader || cellGcmResolveIO(t->offset))) {
        /* The float formats' holes, as RPCS3 force_bg_load: the guest's
         * big-endian halves / floats, word-swapped to the host order. */
        const u32 wsz = s->fmt == RSX_BE_FMT_R16G16B16A16F ? 2u : 4u;
        u8* rows = eng_coherence_buffer(tw * bpp * th);
        for (u32 r = 0; rows && r < th; r++) {
            const u8* p = eng_guest_image(t->location, t->offset + r * eng_tiled_row_pitch(t->location, t->offset, pitch), tw * bpp);
            if (!p) { rows = NULL; break; }
            u8* d = rows + (size_t)r * tw * bpp;
            for (u32 b = 0; b < tw * bpp; b += wsz)
                for (u32 k = 0; k < wsz; k++) d[b + k] = p[b + wsz - 1u - k];
        }
        if (rows) { seed = rows; seed_row = tw * bpp; }
    }
    const u32 tmp = g.be->color_target_create(g.be->user, eng_hfmt(s), tw, th, seed, seed_row);
    if (!tmp) return 0;
    for (u32 k = 0; k < n; k++) {
        const eng_surface* o = &g.surfaces[src[k]];
        u32 sx = 0, sy = 0, sw, sh, dx = 0, dy = 0, dw, dh;
        if (tile) {
            sw = tw / fx; sh = th / fy; dw = tw; dh = th;
        } else {
            eng_isect r;
            eng_intersect(o->offset, o->w * bpp, o->h, t->offset, tw * bpp, th, pitch, &r);
            sx = r.sxb / bpp; sy = r.sy; dx = r.dxb / bpp; dy = r.dy;
            sw = dw = r.wb / bpp; sh = dh = r.rows;
            if (!sw) continue;
        }
        if (g.be->color_target_blit_filtered)
            g.be->color_target_blit_filtered(g.be->user, o->handle, sx, sy, sw, sh,
                                             tmp, dx, dy, dw, dh, 0);
        else
            g.be->color_target_blit(g.be->user, o->handle, sx, sy, sw, sh, tmp, dx, dy, dw, dh);
    }
    const u32 copy = g.be->color_region_snapshot(g.be->user, tmp, 0, 0, tw, th);
    g.be->color_target_release(g.be->user, tmp);
    if (!copy) return 0;
    if (s->region) g.be->texture_release(g.be->user, s->region);
    s->region = copy;
    s->region_resolved = 2;
    s->region_w = tw;
    s->region_h = th;
    s->region_serial = key;
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] texture %u:0x%08X %ux%u gathered from %u surface(s)%s\n",
                t->location, t->offset, tw, th, n,
                tile ? " (tile AA unresolve)" : complete ? "" : " over guest memory");
    return copy;
}

/* The engine keeps B8, G8B8 and the X8 colour formats as RGBA8 targets holding
 * the shader's own output channels, where RSX stores B8 as the B output only,
 * G8B8 as (B, G), and X8 with a constant alpha (RPCS3's ROP channel remap,
 * RSXFragmentPrologue.glsl remap_ROP_output, and its surface format table in
 * VKGSRender.cpp). Reading such a target as the matching texture format is
 * rewritten here as an A8R8G8B8 read through a crossbar that picks those
 * channels, so the backend's view needs no format of its own. */
u32 rsx_texture_control1(u32 control1); /* rsx_texture_layout.c */

static void eng_surface_crossbar(const eng_surface* s, u32 format, u32 remap,
                                 u32* out_format, u32* out_remap)
{
    *out_format = format;
    *out_remap = remap & 0xFFFFu;
    const u32 base = format & 0x9Fu;
    const u32 c1 = rsx_texture_control1(remap & 0xFFFFu);
    int kind = 0;
    if (s->cfmt == 0x09u && base == 0x81u) kind = 1;
    else if (s->cfmt == 0x0Au && base == 0x8Bu) kind = 2;
    else if ((s->cfmt == 0x04u || s->cfmt == 0x0Eu) && base == 0x85u) kind = 3;
    else if ((s->cfmt == 0x05u || s->cfmt == 0x0Fu) && base == 0x85u) kind = 4;
    if (!kind) return;
    u32 out = 0;
    for (u32 i = 0; i < 4; i++) {
        u32 src = (c1 >> (i * 2)) & 3u;
        u32 op = (c1 >> (8 + i * 2)) & 3u;
        if (op >= 2u) {
            switch (kind) {
            case 1: if (src == 0u) op = 1u; else src = 3u; break;
            case 2: src = (src == 0u || src == 2u) ? 2u : 3u; break;
            case 3: if (src == 0u) op = 0u; break;
            case 4: if (src == 0u) op = 1u; break;
            }
        }
        out |= (src << (i * 2)) | (op << (8 + i * 2));
    }
    *out_format = (format & ~0x9Fu) | 0x85u;
    *out_remap = out;
}

static u32 eng_reinterpret(u32 src_handle, u32 kind, u32 src_w, u32 src_h, u32 src_pitch,
                           const rsx_dsp_texture* t, u32* cache, u32* c_fmt, u32* c_remap,
                           u32* c_w, u32* c_h, u64* c_serial, u64 serial);

/* ---- texture cache over render targets (scratch/parity/texcache.md) ----- */

/* RPCS3 upload_texture's attributes.pitch -- the pitch register of a linear
 * texture, the packed row of a swizzled one or of pitch 0 -- and the packed
 * row (*packed) its width-in-bytes tests use. */
static u32 eng_tc_pitch(const rsx_dsp_texture* t, u32* packed)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    *packed = 0;
    if (!t->width || !rsx_texture_mip_chain(t->format, t->width,
                                            t->dimension == 1 ? 1u : t->height, 1, 0, lv))
        return 0;
    *packed = lv[0].tl.row_bytes;
    return ((t->format & 0x20u) && t->pitch) ? t->pitch : *packed;
}

/* The remap a guest-memory upload is keyed and decoded with: a linear 1D/2D
 * texture whose packed row exceeds its pitch register carries its S wrap
 * mode + 1 in bits 16-19, for eng_texture_upload_impl's width reduction. */
static u32 eng_tc_narrow_key(const rsx_dsp_texture* t)
{
    const u32 remap = t->remap & 0xFFFFu;
    u32 packed;
    if (s_texcache_legacy || !(t->format & 0x20u) || !t->pitch || t->cubemap ||
        t->dimension == 3u)
        return remap;
    eng_tc_pitch(t, &packed);
    return packed > t->pitch ? remap | (((t->wrap & 0xFu) + 1u) << 16) : remap;
}

/* The surface's pitch as RPCS3 keeps it: a swizzled (packed) raster's is its
 * row, width x bpp, whatever the pitch register held -- the only way the
 * register can be short of the row of a surface the layout kept. */
static u32 eng_tc_surface_pitch(const eng_surface* s)
{
    const u32 row = s->w * eng_color_bytes(s);
    return s->pitch < row ? row : s->pitch;
}

/* RPCS3 texture_cache_helpers check_framebuffer_resource: a surface at the
 * texture's address is sampled for it only when pitch compatible (the same
 * pitch, or a single row) and covering it, in samples -- 1D its width, 2D
 * width and height, 3D depth slices of its height, a cubemap six faces of
 * slice height with the surface exactly the face height wide. */
static int eng_tc_fb_fits(const eng_surface* s, const rsx_dsp_texture* t)
{
    u32 packed;
    const u32 pitch = eng_tc_pitch(t, &packed);
    const u32 th = t->dimension == 1 ? 1u : t->height;
    if (!pitch || (th != 1u && s->h != 1u && pitch != eng_tc_surface_pitch(s))) return 0;
    if (t->dimension == 1) return s->w >= t->width;
    if (t->cubemap) {
        const u32 span = eng_texture_span(eng_tex_lfmt(t), t->width, t->height, t->mipmaps,
                                          t->pitch, 1);
        const u32 slice_h = span / pitch / 6u;
        return s->w == t->height && s->w >= t->width && s->h >= slice_h * 6u;
    }
    if (t->dimension == 3) return s->w >= t->width && s->h >= t->height * (t->depth ? t->depth : 1u);
    return s->w >= t->width && s->h >= t->height;
}

/* Copies of a rectangle of a colour surface, kept until the surface is
 * written again: a texture inside a target (eng_tc_overlap) or a 1D read of
 * its first row. */
#define ENG_TC_SUBS 32u
static struct { u32 copy, surface, x, y, w, h; u64 serial, use; } s_tc_sub[ENG_TC_SUBS];
static u64 s_tc_sub_use;

static u32 eng_tc_subregion(const eng_surface* s, u32 x, u32 y, u32 w, u32 h)
{
    if (!g.be->color_region_snapshot || !s->handle || !w || !h) return 0;
    u32 victim = 0;
    for (u32 i = 0; i < ENG_TC_SUBS; i++) {
        if (s_tc_sub[i].copy && s_tc_sub[i].surface == s->handle &&
            s_tc_sub[i].serial == s->color_serial && s_tc_sub[i].x == x &&
            s_tc_sub[i].y == y && s_tc_sub[i].w == w && s_tc_sub[i].h == h) {
            s_tc_sub[i].use = ++s_tc_sub_use;
            return s_tc_sub[i].copy;
        }
        if (s_tc_sub[i].use < s_tc_sub[victim].use) victim = i;
    }
    const u32 copy = g.be->color_region_snapshot(g.be->user, s->handle, x, y, w, h);
    if (!copy) return 0;
    if (s_tc_sub[victim].copy) g.be->texture_release(g.be->user, s_tc_sub[victim].copy);
    s_tc_sub[victim].copy = copy;
    s_tc_sub[victim].surface = s->handle;
    s_tc_sub[victim].serial = s->color_serial;
    s_tc_sub[victim].x = x; s_tc_sub[victim].y = y;
    s_tc_sub[victim].w = w; s_tc_sub[victim].h = h;
    s_tc_sub[victim].use = ++s_tc_sub_use;
    return copy;
}

static void eng_tc_sub_release_all(void)
{
    for (u32 i = 0; i < ENG_TC_SUBS; i++)
        if (s_tc_sub[i].copy && g.be && g.be->texture_release)
            g.be->texture_release(g.be->user, s_tc_sub[i].copy);
    memset(s_tc_sub, 0, sizeof s_tc_sub);
    s_tc_sub_use = 0;
}

/* RPCS3 upload_texture's CPU path: invalidate_range_impl_base (cause read)
 * over the texture's range stores every colour target written there (Write
 * Color Buffers; depth is not, Write Depth Buffer being off), whatever its
 * pitch, before the decode reads memory. A target being drawn is left.
 * Returns 0 when the surface `must` (or -1) could not be stored. */
static int eng_tc_store_over(const rsx_dsp_texture* t, const u32* targets, u32 n_targets,
                             int must)
{
    if (must >= 0 && !eng_surface_to_guest((u32)must)) return 0;
    const u32 depth = (t->dimension == 3 && !t->cubemap && t->depth > 1) ? t->depth : 1u;
    const u32 th = t->dimension == 1 ? 1u : t->height;
    const u32 span = depth > 1
        ? eng_texture_span_3d(t->format, t->width, th, depth, t->mipmaps, t->pitch)
        : eng_texture_span(eng_tex_lfmt(t), t->width, th, t->mipmaps, t->pitch, (int)t->cubemap);
    eng_flq q; q.n = 0;
    for (u32 i = 0; span && i < g.n_surfaces; i++) {
        const eng_surface* o = &g.surfaces[i];
        int attached = 0;
        for (u32 k = 0; k < n_targets; k++) if (targets[k] == i) attached = 1;
        if (!o->handle || !o->guest_dirty || o->swz || attached || o->location != t->location ||
            eng_surface_is_display_buffer(o->location, o->offset))
            continue;
        if (eng_ranges_overlap(t->offset, span, o->offset, o->pitch * o->h))
            eng_flq_add(&q, i, 0);
    }
    eng_flq_run(&q, __LINE__);
    return 1;
}

/* RPCS3 texture_cache_helpers.h:806-829 process_framebuffer_resource_fast:
 * a cubemap or volume whose surface passes check_framebuffer_resource is
 * an unwrap of that surface (texture_cache.h:2341-2440): face n's level m
 * copied on the GPU from (0, n * slice_h + the rows of levels < m), a
 * volume's slice n (level 0 only, exact_mip_count) from (0, n * height). Here
 * when the target is stored as the texture reads it (no conversion) and the
 * backend can copy into layers; otherwise (a bordered cube too) the
 * guest-memory path (eng_tc_store_over) as before. RSX_ENG_GATHER_3D=0: always memory. Kept
 * until the surface is written again. */
#define ENG_TC_UNWRAPS 8u
static struct {
    u32 tex, surface, offset, w, h, depth, mips, remap, fmt;
    u64 serial, use;
} s_tc_unwrap[ENG_TC_UNWRAPS];
static u64 s_tc_unwrap_use;

static void eng_tc_unwrap_release_all(void)
{
    for (u32 i = 0; i < ENG_TC_UNWRAPS; i++)
        if (s_tc_unwrap[i].tex && g.be && g.be->texture_release)
            g.be->texture_release(g.be->user, s_tc_unwrap[i].tex);
    memset(s_tc_unwrap, 0, sizeof s_tc_unwrap);
    s_tc_unwrap_use = 0;
}

static u32 eng_tc_unwrap(const eng_surface* s, const rsx_dsp_texture* t, u32 xfmt, u32 xremap)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_ENG_GATHER_3D"); en = !(e && e[0] == '0'); }
    const int cube = t->cubemap != 0;
    const u32 depth = (!cube && t->dimension == 3) ? (t->depth ? t->depth : 1u) : 1u;
    if (!en || s_texcache_legacy || !g.be->texture_copy_from_color || !g.be->texture_release ||
        (!cube && depth < 2u) || (cube && t->border) || !s->handle || s->swz || s->aa_sx != 1u || s->aa_sy != 1u ||
        eng_hfmt(s) != s->fmt || !(t->format & 0x20u) || t->location != s->location ||
        t->offset != s->offset || !eng_surface_tex_same_fmt(s, xfmt) ||
        (cube && !g.be->texture_create) || (!cube && !g.be->texture_create_3d) ||
        !eng_tc_fb_fits(s, t))
        return 0;
    u32 packed;
    const u32 pitch = eng_tc_pitch(t, &packed);
    if (!pitch) return 0;
    u32 mips = cube ? (t->mipmaps ? t->mipmaps : 1u) : 1u;
    u32 slice_h = t->height;
    if (cube) {
        const u32 span = eng_texture_span(eng_tex_lfmt(t), t->width, t->height, t->mipmaps,
                                          t->pitch, 1);
        slice_h = span / pitch / 6u;
        /* A chain cut where a level would be empty (RPCS3 caps the count). */
        u32 m = 1u;
        while (m < mips && ((t->width >> m) || (t->height >> m))) m++;
        mips = m;
    }
    u32 victim = 0;
    for (u32 i = 0; i < ENG_TC_UNWRAPS; i++) {
        if (s_tc_unwrap[i].tex && s_tc_unwrap[i].surface == s->handle &&
            s_tc_unwrap[i].serial == s->color_serial && s_tc_unwrap[i].offset == t->offset &&
            s_tc_unwrap[i].w == t->width && s_tc_unwrap[i].h == t->height &&
            s_tc_unwrap[i].depth == (cube ? 0u : depth) && s_tc_unwrap[i].mips == mips &&
            s_tc_unwrap[i].remap == xremap && s_tc_unwrap[i].fmt == xfmt) {
            s_tc_unwrap[i].use = ++s_tc_unwrap_use;
            return s_tc_unwrap[i].tex;
        }
        if (s_tc_unwrap[i].use < s_tc_unwrap[victim].use) victim = i;
    }
    const u32 tex = cube
        ? g.be->texture_create(g.be->user, s->fmt, t->width, t->height, mips, 6u, xremap, xfmt)
        : g.be->texture_create_3d(g.be->user, s->fmt, t->width, t->height, depth, 1u, xremap, xfmt);
    if (!tex) return 0;
    int ok = 1;
    for (u32 n = 0; ok && n < (cube ? 6u : depth); n++) {
        u32 y = n * slice_h;
        for (u32 m = 0; ok && m < mips; m++) {
            const u32 mw = (t->width >> m) ? (t->width >> m) : 1u;
            const u32 mh = (t->height >> m) ? (t->height >> m) : 1u;
            ok = y + mh <= s->h &&
                 g.be->texture_copy_from_color(g.be->user, tex, n, m, s->handle, 0, y, mw, mh);
            y += mh;
        }
    }
    if (!ok) { g.be->texture_release(g.be->user, tex); return 0; }
    if (s_tc_unwrap[victim].tex) g.be->texture_release(g.be->user, s_tc_unwrap[victim].tex);
    s_tc_unwrap[victim].tex = tex;
    s_tc_unwrap[victim].surface = s->handle;
    s_tc_unwrap[victim].serial = s->color_serial;
    s_tc_unwrap[victim].offset = t->offset;
    s_tc_unwrap[victim].w = t->width; s_tc_unwrap[victim].h = t->height;
    s_tc_unwrap[victim].depth = cube ? 0u : depth;
    s_tc_unwrap[victim].mips = mips;
    s_tc_unwrap[victim].remap = xremap; s_tc_unwrap[victim].fmt = xfmt;
    s_tc_unwrap[victim].use = ++s_tc_unwrap_use;
    return tex;
}

/* A read decoded from memory samples the texture's own levels and LOD range
 * (shader_read), not the single-level rules of a surface-backed read that
 * sink_bind_textures applied when it found a surface at the address. */
static void eng_tc_memory_sampler(u32 unit, const rsx_dsp_texture* t, rsx_be_sampler_desc* smp)
{
    rsx_be_sampler_desc d;
    memset(&d, 0, sizeof d);
    eng_decode_sampler(t->filter, t->wrap, t->control0, t->border_color, &d);
    eng_sampler_clamp_mips(&d, t->format, t->width, t->height, t->mipmaps);
    smp->min_lod = d.min_lod;
    smp->max_lod = d.max_lod;
    smp->lod_bias = d.lod_bias;
    if (!(!rsx_texture_legacy() && !((s_eng_hw_snorm_units >> unit) & 1u) &&
          rsx_fp_texture_sext_mask(t->format, t->remap, t->filter)))
        smp->mip_linear = d.mip_linear;
}

/* Guest bytes a fragment texture reads: every level, face and slice
 * (RPCS3 get_texture_size). RSX_ENG_TEX_FLUSH_SPAN=0: the RT write-back
 * before a CPU decode ignores 3D depth (and vertex cube faces), as before. */
static int eng_tex_flush_span(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_ENG_TEX_FLUSH_SPAN"); v = !(e && e[0] == '0'); }
    return v;
}

static u32 eng_tex_read_span(const rsx_dsp_texture* t)
{
    const u32 th = t->dimension == 1 ? 1u : t->height;
    if (t->dimension == 3 && !t->cubemap && t->depth > 1)
        return eng_texture_span_3d(t->format, t->width, th, t->depth, t->mipmaps, t->pitch);
    return eng_texture_span(eng_tex_lfmt(t), t->width, th, t->mipmaps, t->pitch, (int)t->cubemap);
}

static void eng_zdepth_free(u32 i)
{
    eng_zdepth* z = &g.zdepths[i];
    if (z->reint) g.be->texture_release(g.be->user, z->reint);
    if (z->handle) g.be->depth_target_release(g.be->user, z->handle);
    memset(z, 0, sizeof *z);
    z->location = ENG_INVALID;
}

/* RPCS3 get_merged_texture_memory_region: a surface over a texture's range
 * whose memory tag fails (the CPU or a DMA rewrote its memory) is
 * invalidate_surface_address'd unless bound, so the texture reads guest
 * memory and the stale surface is never written back over it.
 * RSX_ENG_TEX_TAG_DROP=0 keeps such surfaces sampleable. */
static void eng_tex_drop_stale(u32 location, u32 offset, u32 span, const u32* targets,
                               u32 n_targets, u32 zslot)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_ENG_TEX_TAG_DROP"); on = !(e && e[0] == '0'); }
    if (!on || !span || !eng_memory_tag() || location >= ENG_LOC_SCRATCH) return;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        eng_surface* s = &g.surfaces[i];
        if (!s->handle || s->swz || !s->tag_ok || s->location != location ||
            s->offset == ENG_SWZ_DEAD || eng_surface_pinned(i))
            continue;
        int bound = 0;
        for (u32 k = 0; k < n_targets; k++) if (targets[k] == i) bound = 1;
        if (bound) continue;
        const u32 len = s->pitch ? s->pitch * s->h : s->w * eng_color_bytes(s) * s->h;
        if (!eng_ranges_overlap(offset, span, s->offset, len) || eng_surface_tag_test(s))
            continue;
        if (eng_coherence_log())
            fprintf(stderr, "[rsx engine] surface 0x%08X failed its memory tag: dropped\n",
                    s->offset);
        s->guest_dirty = 0;
        eng_surface_free(i);
    }
    for (u32 i = 0; i < g.n_zdepths; i++) {
        eng_zdepth* z = &g.zdepths[i];
        if (i == zslot || !z->handle || !z->tag_ok || z->location != location || !z->pitch)
            continue;
        u32 rb, rows;
        eng_zdepth_tag_rect(z, &rb, &rows);
        if (!eng_ranges_overlap(offset, span, z->offset, z->pitch * rows) ||
            eng_zdepth_tag_test(z))
            continue;
        if (eng_coherence_log())
            fprintf(stderr, "[rsx engine] zeta 0x%08X failed its memory tag: dropped\n",
                    z->offset);
        z->guest_dirty = 0;
        eng_zdepth_free(i);
    }
}

/* A texture overlapping colour targets without starting at one
 * (eng_surface_newest is exact-base). RPCS3 fast_texture_search:
 * get_merged_texture_memory_region collects the pitch-compatible surfaces
 * over the range (a failing memory tag skipped) and fast_fbo_check takes the
 * newest; when that one holds the whole texture box it is copied from at the
 * box's position (process_framebuffer_resource_fast with an offset).
 * Otherwise the written targets are stored for the CPU path (RPCS3's atlas
 * gather of several sections reads the same bytes). Returns the copy's view,
 * or 0 for the guest-memory upload. */
static u32 eng_tc_overlap(const rsx_dsp_texture* t, const u32* targets, u32 n_targets,
                          rsx_be_sampler_desc* smp)
{
    u32 packed;
    const u32 pitch = eng_tc_pitch(t, &packed);
    const u32 th = t->dimension == 1 ? 1u : t->height;
    if (!pitch || !packed || packed > pitch) return 0;
    const u32 tbpp = packed / t->width;
    const u32 depth = (t->dimension == 3 && !t->cubemap && t->depth > 1) ? t->depth : 1u;
    const u32 span = depth > 1
        ? eng_texture_span_3d(t->format, t->width, th, depth, t->mipmaps, t->pitch)
        : eng_texture_span(eng_tex_lfmt(t), t->width, th, t->mipmaps, t->pitch, (int)t->cubemap);
    if (!span) return 0;
    int newest = -1, dirty = 0;
    eng_isect nr;
    memset(&nr, 0, sizeof nr);
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* o = &g.surfaces[i];
        if (!o->handle || o->swz || o->location != t->location || o->offset == ENG_SWZ_DEAD)
            continue;
        if (!eng_ranges_overlap(t->offset, span, o->offset, o->pitch * o->h))
            continue;
        if (o->guest_dirty) dirty = 1;
        if (th != 1u && o->h != 1u && eng_tc_surface_pitch(o) != pitch) continue;
        eng_isect r;
        const u32 ob = eng_color_bytes(o);
        if (!eng_intersect(o->offset, o->w * ob, o->h, t->offset, t->width * tbpp, th,
                           th == 1u ? eng_tc_surface_pitch(o) : pitch, &r) || r.wb < tbpp)
            continue;
        if (!eng_surface_tag_test(o)) continue;
        if (newest < 0 || o->color_serial > g.surfaces[newest].color_serial) {
            newest = (int)i;
            nr = r;
        }
    }
    /* RPCS3 get_merged_texture_memory_region walks the depth surfaces too: a
     * newer pitch-compatible zeta over the box is what the texture reads.
     * Its bytes are stored in the zeta layout and decoded from memory (the
     * texels RPCS3's typeless atlas copy produces, stencil aside). */
    int znewest = -1;
    for (u32 i = 0; i < g.n_zdepths; i++) {
        const eng_zdepth* z = &g.zdepths[i];
        if (!z->handle || !z->depth_serial || z->location != t->location || !z->pitch)
            continue;
        const u32 zb = (z->zfmt == 1u) ? 2u : 4u;
        if (!eng_ranges_overlap(t->offset, span, z->offset, z->pitch * z->h)) continue;
        if (th != 1u && z->h != 1u && z->pitch != pitch) continue;
        eng_isect r;
        if (!eng_intersect(z->offset, z->w * zb, z->h, t->offset, t->width * tbpp, th,
                           th == 1u ? z->pitch : pitch, &r))
            continue;
        if (newest >= 0 && g.surfaces[newest].color_serial >= z->depth_serial) continue;
        if (znewest < 0 || z->depth_serial > g.zdepths[znewest].depth_serial) znewest = (int)i;
    }
    if (znewest >= 0) {
        if (!g.zdepths[znewest].guest_dirty && !dirty) return 0;
        eng_tc_store_over(t, targets, n_targets, -1);
        eng_zdepth_to_guest((u32)znewest);
        /* RPCS3 VKDraw.cpp:468-481: a depth-class image read as colour
         * samples nearest, so packed depth bytes are never blended. */
        if (!rsx_texture_legacy() && ((t->format & 0x9Fu) == 0x85u || (t->format & 0x9Fu) == 0x9Eu))
            smp->min_linear = smp->mag_linear = smp->mip_linear = 0;
        if (eng_coherence_log())
            fprintf(stderr, "[rsx engine] texture %u:0x%08X %ux%u over zeta 0x%08X: stored\n",
                    t->location, t->offset, t->width, th, g.zdepths[znewest].offset);
        return 0;
    }
    if (!dirty) return 0;
    if (newest >= 0 && t->dimension <= 2u && !t->cubemap && nr.dxb == 0u && nr.dy == 0u &&
        nr.wb >= t->width * tbpp && nr.rows >= th) {
        eng_surface* s = &g.surfaces[newest];
        u32 xfmt, xremap;
        eng_surface_crossbar(s, t->format, t->remap, &xfmt, &xremap);
        if (s->aa_sx == 1u && s->aa_sy == 1u && eng_surface_tex_same_fmt(s, xfmt) &&
            eng_color_bytes(s) == tbpp) {
            const u32 copy = eng_tc_subregion(s, nr.sxb / tbpp, nr.sy, t->width, th);
            if (copy) {
                if (!rsx_texture_legacy()) {
                    smp->min_lod = smp->max_lod = smp->lod_bias = 0.0f;
                    smp->mip_linear = 0;
                }
                const u32 view = g.be->surface_view
                    ? g.be->surface_view(g.be->user, copy, xremap, xfmt) : 0;
                if (eng_coherence_log())
                    fprintf(stderr, "[rsx engine] texture %u:0x%08X %ux%u read at (%u,%u) of "
                            "surface 0x%08X\n", t->location, t->offset, t->width, th,
                            nr.sxb / tbpp, nr.sy, s->offset);
                return view ? view : copy;
            }
        }
    }
    eng_tc_store_over(t, targets, n_targets, -1);
    return 0;
}

static u32 sink_bind_textures(const u32* target_slots, u32 n_targets,
                              u32 current_zslot, u32 tex_used,
                              u32 textures[RSX_BE_MAX_TEXTURES],
                              rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES])
{
    u32 mask = 0;
    u32 feedback[RSX_BE_MAX_COLOR_TARGETS] = {0};
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        /* A unit left enabled by an earlier draw but not sampled by this
         * program is skipped: resolving it would cost a cache check, and an
         * aliased surface a store to guest memory, for nothing. */
        if (!t.enabled || !((tex_used >> u) & 1u)) continue;
        mask |= 1u << u;
        eng_tex_drop_stale(t.location, t.offset, eng_tex_read_span(&t), target_slots, n_targets,
                           current_zslot);
        eng_decode_sampler(t.filter, t.wrap, t.control0, t.border_color, &samplers[u]);
        eng_sampler_clamp_mips(&samplers[u], t.format, t.width, t.height, t.mipmaps);
        /* RPCS3 VKDraw.cpp:445-480: a colour texture still needing the
         * shader's sign extension (no SNORM view, eng_tex_hw_snorm) is
         * point-sampled -- filtering the raw unsigned bytes would blend
         * across the sign wrap. */
        if (!rsx_texture_legacy() && !((s_eng_hw_snorm_units >> u) & 1u) &&
            rsx_fp_texture_sext_mask(t.format, t.remap, t.filter))
            samplers[u].min_linear = samplers[u].mag_linear = samplers[u].mip_linear = 0;
        samplers[u].border_color = eng_border_color(
            t.border_color, t.wrap, t.dimension, t.filter >> 28,
            !((s_eng_hw_snorm_units >> u) & 1u) &&
                rsx_fp_texture_sext_mask(t.format, t.remap, t.filter) != 0,
            (int)((s_eng_hw_snorm_units >> u) & 1u), t.remap, t.format, 0,
            samplers[u].border_rgba);
        int no_rtt = eng_tex_format_skips_rtt(t.format);
        int sampled = -1, own = 0, surface_at = -1;
        {
            const u32 i = eng_surface_newest(t.location, t.offset, target_slots, n_targets);
            if (i != ENG_INVALID) {
                surface_at = (int)i;
                /* A current attachment requires an ordered immutable copy; it
                 * cannot be sampled directly while the host pass writes it. */
                for (u32 k = 0; k < n_targets; k++) if (target_slots[k] == i) own = 1;
                if (!own) sampled = (int)i;
            }
        }
        /* RPCS3 upload_texture -> append_mipmap_level (mipmap_gather): a 2D
         * mipped texture over a render target gathers the levels that are
         * render targets too and keeps its LOD range. Here: write the level
         * targets back (Write Color Buffers) and upload the chain from guest
         * memory, through the decode path below. */
        if (!no_rtt && sampled >= 0 && t.mipmaps > 1u && t.dimension == 2 && !t.cubemap &&
            !rsx_texture_legacy()) {
            const u32 l0 = t.pitch ? t.pitch * t.height
                                   : eng_texture_span(eng_tex_lfmt(&t), t.width, t.height, 1u, 0u, 0);
            if (l0 && eng_surface_newest(t.location, t.offset + l0, target_slots, n_targets) !=
                          ENG_INVALID) {
                no_rtt = 1;
                /* RPCS3 gathers only the contiguous render-target levels
                 * (texture_cache.h:3264-3305): the image ends there, so
                 * sampling clamps at the last one. RSX_ENG_RT_MIP_CLAMP=0
                 * samples the whole chain from memory, as before. */
                static int clamp_en = -1;
                if (clamp_en < 0) { const char* e = getenv("RSX_ENG_RT_MIP_CLAMP"); clamp_en = !(e && e[0] == '0'); }
                rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
                const u32 nl = clamp_en ? rsx_texture_mip_chain(eng_tex_lfmt(&t), t.width, t.height,
                                                                t.mipmaps, t.pitch, lv) : 0u;
                u32 rt_levels = 0, loff = 0;
                while (rt_levels < nl) {
                    const u32 si = eng_surface_newest(t.location, t.offset + loff,
                                                      target_slots, n_targets);
                    if (si == ENG_INVALID || g.surfaces[si].w < lv[rt_levels].w ||
                        g.surfaces[si].h < lv[rt_levels].h)
                        break;
                    loff += t.pitch ? t.pitch * lv[rt_levels].h
                                    : eng_texture_span(eng_tex_lfmt(&t), lv[rt_levels].w,
                                                       lv[rt_levels].h, 1u, 0u, 0);
                    rt_levels++;
                }
                if (rt_levels && rt_levels < nl) {
                    const float last = (float)(rt_levels - 1u);
                    if (samplers[u].max_lod > last) samplers[u].max_lod = last;
                    if (samplers[u].min_lod > last) samplers[u].min_lod = last;
                }
            }
        }
        /* A swizzled-destination image (rsx_draw_engine_blit_swizzled) is
         * sampled directly only as the exact single-level swizzled
         * A8R8G8B8 2D texture it stands for; any other overlapping read
         * gets its Morton-ordered bytes in guest memory first. */
        if (s_eng_swz_used) {
            const int keep = surface_at >= 0 && g.surfaces[surface_at].swz &&
                g.surfaces[surface_at].guest_dirty && !own && t.mipmaps <= 1u &&
                !(t.format & 0x20u) && (t.format & 0x9Fu) == 0x85u &&
                t.dimension == 2u && !t.cubemap &&
                t.width == g.surfaces[surface_at].w && t.height == g.surfaces[surface_at].h;
            const u64 trow = t.pitch ? t.pitch : (u64)t.width * 16u;
            u64 tspan = trow * (t.height ? t.height : 1u) * (t.depth ? t.depth : 1u) *
                        (t.cubemap ? 6u : 1u) * (t.mipmaps > 1u ? 2u : 1u);
            if (tspan > 0xFFFFFFFFull) tspan = 0xFFFFFFFFull;
            eng_flq q; q.n = 0;
            for (u32 i = 0; i < g.n_surfaces; i++) {
                const eng_surface* o = &g.surfaces[i];
                if (!o->handle || !o->swz || !o->guest_dirty || o->offset == ENG_SWZ_DEAD ||
                    o->location != t.location || (keep && (int)i == surface_at))
                    continue;
                if (eng_ranges_overlap(t.offset, (u32)tspan, o->offset, o->w * o->h * 4u))
                    eng_flq_add(&q, i, 0);
            }
            eng_flq_run(&q, __LINE__);
            if (surface_at >= 0 && g.surfaces[surface_at].swz && !keep) {
                surface_at = sampled = -1;
                own = 0;
            }
        }
        if (no_rtt) {
            /* upload_texture's CPU path first runs invalidate_range_impl_base
             * (cause read) over the texture's range, flushing the colour
             * targets written there (Write Color Buffers) -- the decode then
             * reads what RSX left in memory. A target being drawn is left. */
            const u32 span = eng_tex_flush_span()
                ? eng_tex_read_span(&t)
                : eng_texture_span(eng_tex_lfmt(&t), t.width, t.height, t.mipmaps,
                                   t.pitch, (int)t.cubemap);
            eng_flq q; q.n = 0;
            for (u32 i = 0; span && i < g.n_surfaces; i++) {
                const eng_surface* o = &g.surfaces[i];
                int attached = 0;
                for (u32 k = 0; k < n_targets; k++) if (target_slots[k] == i) attached = 1;
                if (!o->handle || !o->guest_dirty || o->swz || attached ||
                    o->location != t.location ||
                    eng_surface_is_display_buffer(o->location, o->offset))
                    continue;
                if (eng_ranges_overlap(t.offset, span, o->offset, o->pitch * o->h))
                    eng_flq_add(&q, i, 0);
            }
            eng_flq_run(&q, __LINE__);
            surface_at = sampled = -1;
            own = 0;
        }
        const u32 base_fmt = t.format & RSX_TEX_FMT_BASE_MASK & ~(u32)RSX_TEX_FMT_UNNORM;
        /* The depth at this address, when it was written after any colour
         * there: a depth-only pass names its zeta as a colour target too.
         * The newest zeta at the base wins (RPCS3 fast_texture_search
         * .back() of the last_use_tag-sorted list). */
        int zsrc = -1;
        u64 zsrc_serial = 0;
        for (u32 i = 0; i < g.n_zdepths && !no_rtt; i++) {
            const eng_zdepth* z = &g.zdepths[i];
            if (z->location != t.location || z->offset != t.offset || !z->depth_serial ||
                z->depth_serial <= zsrc_serial)
                continue;
            zsrc_serial = z->depth_serial;
            zsrc = (int)i;
        }
        if (zsrc >= 0 && surface_at >= 0 && zsrc_serial <= g.surfaces[surface_at].color_serial)
            zsrc = -1;
        /* RPCS3 VKDraw.cpp:482-519: a render-target-backed read is not
         * shader_read, so actual_mipmaps is 1 -- no LOD range or bias, mip
         * mode nearest -- while the game's min/mag filter is kept. Only a
         * depth image redirected to an A8R8G8B8/D8R8G8B8 read is never
         * filtered (VKDraw.cpp:453-458), so the bitcast depth bits do not
         * blend. RSX_TEX_LEGACY=1 skips this. */
        if (!rsx_texture_legacy() && (surface_at >= 0 || zsrc >= 0)) {
            samplers[u].min_lod = samplers[u].max_lod = samplers[u].lod_bias = 0.0f;
            samplers[u].mip_linear = 0;
            if (zsrc >= 0 && ((base_fmt & 0x9Fu) == 0x85u || (base_fmt & 0x9Fu) == 0x9Eu))
                samplers[u].min_linear = samplers[u].mag_linear = 0;
        }
        if (zsrc >= 0) {
            /* DEPTH24_D8 keeps the backend's own snapshot when it has one;
             * anything else goes through guest memory. */
            if (base_fmt == RSX_TEX_FMT_DEPTH24_D8 && current_zslot != (u32)zsrc) {
                const u32 snap = eng_zdepth_snapshot((u32)zsrc);
                if (snap) { textures[u] = snap; continue; }
            }
            if (!g.zdepths[zsrc].depth_float) {
                eng_zdepth* z = &g.zdepths[zsrc];
                const u32 tex = eng_reinterpret(
                    z->handle, z->zfmt == 1u ? RSX_BE_REINTERPRET_DEPTH_Z16
                                             : RSX_BE_REINTERPRET_DEPTH_Z24S8,
                    z->w, z->h, z->pitch, &t, &z->reint, &z->reint_fmt, &z->reint_remap,
                    &z->reint_w, &z->reint_h, &z->reint_serial, z->depth_serial);
                if (tex) { textures[u] = tex; continue; }
            }
            if (eng_coherence_log() || s_prof_on > 0) {
                static u32 s_seen[32]; static u32 s_nseen;
                const eng_zdepth* z = &g.zdepths[zsrc];
                const u32 key = t.offset ^ (t.format << 24) ^ (t.width << 12) ^ t.height;
                u32 k = 0;
                while (k < s_nseen && s_seen[k] != key) k++;
                if (k == s_nseen && s_nseen < 32) {
                    s_seen[s_nseen++] = key;
                    fprintf(stderr, "[rsx engine] depth texture via guest: 0x%08X fmt 0x%02X %ux%u pitch %u mips %u dim %u cube %u <- zeta %s %ux%u pitch %u float %u\n",
                            t.offset, t.format, t.width, t.height, t.pitch, t.mipmaps, t.dimension, t.cubemap,
                            z->zfmt == 1u ? "Z16" : "Z24S8", z->w, z->h, z->pitch, z->depth_float);
                }
            }
            if (eng_zdepth_to_guest((u32)zsrc)) {
                textures[u] = eng_texture_slot(t.location, t.offset, t.format /* surface hit: no border */,
                                               t.width, t.height, t.mipmaps, t.pitch,
                                               (int)t.cubemap, t.remap & 0xFFFFu,
                                       (t.dimension == 3 && !t.cubemap) ? t.depth : 1u);
                continue;
            }
        }
        if (own && zsrc < 0 && g.be->color_snapshot) {
            eng_surface* s = &g.surfaces[surface_at];
            rsx_dsp_surface sf;
            rsx_dsp_get_surface(&g.rsx, &sf);
            /* A subregion of the target being drawn: an ordered copy of just
             * that rectangle is as immutable as a full snapshot. */
            u32 xfmt, xremap;
            eng_surface_crossbar(s, t.format, t.remap, &xfmt, &xremap);
            rsx_dsp_texture tx = t;
            tx.format = xfmt;
            const u32 sub = eng_surface_region(s, &tx);
            const u32 gat = sub ? 0u : eng_surface_gather(s, &tx);
            if (sub || gat) {
                const u32 img = sub ? sub : gat;
                const u32 view = g.be->surface_view
                    ? g.be->surface_view(g.be->user, img, xremap, xfmt) : 0;
                textures[u] = view ? view : img;
                continue;
            }
            int ok = eng_color_feedback_compatible(s, &t, &sf);
            u32 k = 0;
            if (ok) {
                while (k < n_targets && target_slots[k] != (u32)surface_at) k++;
                ok = k < n_targets;
            }
            if (ok && !feedback[k]) {
                const u32 copy = g.be->color_snapshot(g.be->user, s->handle);
                if (copy) {
                    if (s->feedback) g.be->texture_release(g.be->user, s->feedback);
                    s->feedback = feedback[k] = copy;
                } else {
                    ok = 0;
                }
            }
            if (ok) {
                if (g.be->surface_view) {
                    textures[u] = g.be->surface_view(g.be->user, feedback[k], xremap, xfmt);
                    ok = textures[u] != 0;
                } else if (xremap == 0xaae4u) {
                    textures[u] = feedback[k];
                } else {
                    ok = 0;
                }
            }
            if (!ok) {
                const u32 fb = eng_color_feedback_failure(u, s, &t);
                if (fb == ENG_INVALID) return ENG_INVALID;
                textures[u] = fb;
            }
            continue;
        }
        if (sampled >= 0 && zsrc < 0 &&
            g.surfaces[sampled].fmt == RSX_BE_FMT_R8G8B8A8 &&
            eng_format_reinterprets_rgba8(base_fmt)) {
            eng_surface* s = &g.surfaces[sampled];
            u32 tex = 0;
            if (s->aa_sx == 1u && s->aa_sy == 1u)
                tex = eng_reinterpret(s->handle, RSX_BE_REINTERPRET_COLOR_ARGB8,
                                      s->w, s->h, s->pitch, &t, &s->reint, &s->reint_fmt,
                                      &s->reint_remap, &s->reint_w, &s->reint_h,
                                      &s->reint_serial, s->color_serial);
            if (tex) { textures[u] = tex; continue; }
        }
        if (sampled >= 0 && zsrc < 0 &&
            g.surfaces[sampled].fmt == RSX_BE_FMT_R8G8B8A8 &&
            eng_format_reinterprets_rgba8(base_fmt) &&
            eng_surface_to_guest((u32)sampled)) {
            textures[u] = eng_texture_slot(t.location, t.offset, t.format /* surface hit: no border */,
                                           t.width, t.height, t.mipmaps, t.pitch,
                                           (int)t.cubemap, t.remap & 0xFFFFu,
                                       (t.dimension == 3 && !t.cubemap) ? t.depth : 1u);
            continue;
        }
        int tc_memory = 0;
        if (sampled >= 0) {
            u32 xfmt, xremap;
            eng_surface_crossbar(&g.surfaces[sampled], t.format, t.remap, &xfmt, &xremap);
            rsx_dsp_texture tx = t;
            tx.format = xfmt;
            const u32 sub = eng_surface_region(&g.surfaces[sampled], &tx);
            const u32 gat = sub ? 0u : eng_surface_gather(&g.surfaces[sampled], &tx);
            /* RPCS3 check_framebuffer_resource: a 1D read copies the first
             * row; a cubemap, a volume, a pitch the surface does not have or
             * an extent it does not cover takes the CPU path over the stored
             * targets instead of sampling the whole surface as a 2D image. */
            u32 row = 0;
            if (!sub && !gat && !s_texcache_legacy) {
                eng_surface* s = &g.surfaces[sampled];
                const int fits = eng_tc_fb_fits(s, &t);
                if (fits && t.dimension == 1 && s->aa_sx == 1u && s->aa_sy == 1u &&
                    eng_surface_tex_same_fmt(s, xfmt))
                    row = eng_tc_subregion(s, 0, 0, t.width, 1);
                if (fits && (t.cubemap || t.dimension == 3) && !row) {
                    const u32 unw = eng_tc_unwrap(s, &t, xfmt, xremap);
                    if (unw) {
                        eng_tc_memory_sampler(u, &t, &samplers[u]);
                        textures[u] = unw;
                        if ((s_eng_hw_snorm_units >> u) & 1u) {
                            const u32 v = g.be->texture_snorm_view(g.be->user, unw);
                            if (v) textures[u] = v;
                        }
                        if ((s_eng_hw_srgb_units >> u) & 1u) {
                            const u32 v = g.be->texture_srgb_view(g.be->user, unw, RSX_BE_FMT_NONE);
                            if (v) textures[u] = v;
                        }
                        continue;
                    }
                }
                if (!row && (!fits || t.cubemap || t.dimension != 2) &&
                    eng_tc_store_over(&t, target_slots, n_targets, sampled))
                    tc_memory = 1;
            }
            if (!tc_memory) {
                const u32 src = sub ? sub : gat ? gat : row ? row : g.surfaces[sampled].handle;
                const u32 view = g.be->surface_view
                    ? g.be->surface_view(g.be->user, src, xremap, xfmt)
                    : 0;
                textures[u] = view ? view : src;
                continue;
            }
            eng_tc_memory_sampler(u, &t, &samplers[u]);
        }
        if (base_fmt == RSX_TEX_FMT_DEPTH24_D8 && !tc_memory) {
            for (u32 i = 0; i < g.n_zdepths; i++)
                if (g.zdepths[i].location == t.location &&
                    g.zdepths[i].offset == t.offset && current_zslot != i) {
                    const u32 snap = eng_zdepth_snapshot(i);
                    if (snap) textures[u] = snap;
                    break;
                }
            if (textures[u]) continue;
        }
        if (!tc_memory && !s_texcache_legacy && !no_rtt && surface_at < 0) {
            const u32 ov = eng_tc_overlap(&t, target_slots, n_targets, &samplers[u]);
            if (ov) { textures[u] = ov; continue; }
        }
        textures[u] = eng_texture_slot(t.location, t.offset, eng_tex_lfmt(&t),
                                       t.width, t.height, t.mipmaps, t.pitch,
                                       (int)t.cubemap, eng_tc_narrow_key(&t),
                                       (t.dimension == 3 && !t.cubemap) ? t.depth : 1u);
        if (textures[u] && ((s_eng_hw_snorm_units >> u) & 1u)) {
            const u32 v = g.be->texture_snorm_view(g.be->user, textures[u]);
            if (v) textures[u] = v;
        }
        if (textures[u] && ((s_eng_hw_srgb_units >> u) & 1u)) {
            const u32 v = g.be->texture_srgb_view(g.be->user, textures[u], RSX_BE_FMT_NONE);
            if (v) textures[u] = v;
        }
        if (!textures[u] && tc_memory)
            textures[u] = g.surfaces[sampled].handle;
    }
    return mask;
}

/* A texture naming a colour or depth target's memory in a format the target
 * cannot be sampled as, built on the GPU by the backend's reinterpretation
 * pass (RPCS3's typeless copy / bitcast) instead of storing the target to
 * guest memory and decoding it back (a GPU stall per use). The result is the
 * texel the guest-memory path would decode, so only layouts where the target
 * and the texture agree texel for texel qualify: a linear, single-level 2D
 * texture at the target's address and pitch. Kept until the target is
 * written again. Returns 0 to take the guest-memory path. */
static u32 eng_reinterpret(u32 src_handle, u32 kind, u32 src_w, u32 src_h, u32 src_pitch,
                           const rsx_dsp_texture* t, u32* cache, u32* c_fmt, u32* c_remap,
                           u32* c_w, u32* c_h, u64* c_serial, u64 serial)
{
    if (eng_legacy_surfaces() || !g.be->surface_reinterpret || rsx_texture_argb_is_rgba())
        return 0;
    /* RPCS3 texture_cache.h:3110-3126: a swizzled texture's pitch is its
     * packed pitch, so it finds a depth target of that pitch in the surface
     * cache the same way a linear one does (the target itself is linear on
     * the host). Colour swizzled targets keep their own path. */
    const int swizzled = !(t->format & RSX_TEX_FMT_LINEAR);
    if ((swizzled && kind == RSX_BE_REINTERPRET_COLOR_ARGB8) ||
        t->cubemap || t->dimension != 2 || t->mipmaps > 1)
        return 0;
    const u32 base = t->format & RSX_TEX_FMT_BASE_MASK;
    const u32 bpp = (base == 0x92u) ? 2u : 4u;
    const u32 tpitch = swizzled ? t->width * bpp : t->pitch;
    if (!t->width || !t->height || t->width > src_w || t->height > src_h ||
        !src_pitch || tpitch != src_pitch)
        return 0;
    if ((kind == RSX_BE_REINTERPRET_DEPTH_Z16) != (bpp == 2u) || src_pitch < t->width * bpp)
        return 0;
    const u32 remap = t->remap & 0xFFFFu;
    if (*cache && *c_fmt == t->format && *c_remap == remap && *c_w == t->width &&
        *c_h == t->height && *c_serial == serial)
        return *cache;
    const u32 tex = g.be->surface_reinterpret(g.be->user, src_handle, kind, 0, 0,
                                              t->width, t->height, base, remap, t->format);
    if (!tex) return 0;
    if (*cache) g.be->texture_release(g.be->user, *cache);
    *cache = tex;
    *c_fmt = t->format; *c_remap = remap; *c_w = t->width; *c_h = t->height;
    *c_serial = serial;
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] target 0x%08X read as format 0x%02X %ux%u on the GPU\n",
                t->offset, base, t->width, t->height);
    return tex;
}

static u32 sink_bind_vertex_textures(
    u32 vtex_mask, u32 textures[RSX_BE_MAX_VERTEX_TEXTURES],
    rsx_be_sampler_desc samplers[RSX_BE_MAX_VERTEX_TEXTURES],
    const u32* target_slots, u32 n_targets, u32 current_zslot)
{
    u32 bound = 0;
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        if (!((vtex_mask >> u) & 1u)) continue;
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        eng_decode_sampler(vt.filter, vt.wrap, vt.control0, vt.border_color, &samplers[u]);
        /* RPCS3 VKDraw.cpp vertex samplers: always NEAREST, no mip filter,
         * no bias or anisotropy, R wraps (NV40 vertex fetch is point-only). */
        samplers[u].min_linear = samplers[u].mag_linear = samplers[u].mip_linear = 0;
        samplers[u].lod_bias = 0.0f;
        samplers[u].max_aniso = 1;
        samplers[u].wrap_r = 1;
        /* RPCS3 VKDraw.cpp:594-619: the vertex sampler takes the unit's
         * min/max LOD as they are (mip mode nearest), not zero when the
         * filter names no mips -- the vertex program picks the level. */
        if (!rsx_texture_legacy()) {
            samplers[u].mip_present = 1;
            samplers[u].min_lod = (float)((vt.control0 >> 19) & 0xFFFu) / 256.0f;
            samplers[u].max_lod = (float)((vt.control0 >> 7) & 0xFFFu) / 256.0f;
            if (samplers[u].max_lod < samplers[u].min_lod)
                samplers[u].max_lod = samplers[u].min_lod;
        }
        /* A vertex unit has no crossbar, so its remap is the identity. */
        if (!rsx_texture_legacy()) {
            samplers[u].border_color = eng_border_color(vt.border_color, vt.wrap, vt.dimension,
                                                        0, 0, 0, 0xAAE4u, vt.format, 1,
                                                        samplers[u].border_rgba);
            /* RPCS3 runs vertex units through upload_texture too: a render
             * target (or depth) written at the address is what they read.
             * Store it to guest memory in its own layout (as the fragment
             * reinterpret path does) so the decode sees those bytes. */
            const u32 vspan = eng_tex_flush_span()
                ? ((vt.dimension == 3 && !vt.cubemap && vt.depth > 1)
                       ? eng_texture_span_3d(vt.format, vt.width, vt.height, vt.depth,
                                             vt.mipmaps, vt.pitch)
                       : eng_texture_span(vt.format, vt.width, vt.height, vt.mipmaps,
                                          vt.pitch, (int)vt.cubemap))
                : eng_texture_span(vt.format, vt.width, vt.height, vt.mipmaps, vt.pitch, 0);
            eng_tex_drop_stale(vt.location, vt.offset, vspan, target_slots, n_targets,
                               current_zslot);
            eng_flq vq; vq.n = 0;
            for (u32 i = 0; vspan && i < g.n_surfaces; i++) {
                const eng_surface* o = &g.surfaces[i];
                if (o->handle && o->guest_dirty && o->location == vt.location &&
                    eng_ranges_overlap(vt.offset, vspan, o->offset, o->pitch * o->h))
                    eng_flq_add(&vq, i, 0);
            }
            for (u32 i = 0; vspan && i < g.n_zdepths; i++) {
                const eng_zdepth* z = &g.zdepths[i];
                if (z->location == vt.location && z->offset == vt.offset && z->depth_serial &&
                    z->guest_dirty)
                    eng_flq_add(&vq, i, 1);
            }
            eng_flq_run(&vq, __LINE__);
        }
        textures[u] = eng_texture_slot(vt.location, vt.offset, vt.format,
                                       vt.width, vt.height, vt.mipmaps,
                                       vt.pitch, (int)vt.cubemap, 0xAAE4u,
                                       (vt.dimension == 3 && !vt.cubemap) ? vt.depth : 1u);
        if (textures[u]) bound |= 1u << u;
    }
    return bound;
}

/* ---- per-pass capture (diagnostic) --------------------------------------- */

/* RSX_ENGINE_PASS_DUMP=<existing dir>: capture the draws pass by pass, from
 * the first draw RSX_ENGINE_PASS_DUMP_AFTER seconds after the engine's first
 * one or, without that, from the frame after the first presented frame of at
 * least RSX_ENGINE_PASS_DUMP_MINDRAWS guest draws (default 1).
 *
 * A pass is a run of draws and clears into one target set and zeta. When the
 * set changes, each colour target of the pass that ended is read back to
 * <dir>/passNNN_tK.ppm, its zeta to <dir>/passNNN_z.pgm (stretched from its
 * smallest to its largest value), and <dir>/passes.txt gets one line: the
 * targets, the zeta and its depth range, the draw and clear counts, each
 * clear's scissor, and the first draw's state and textures. Each texture
 * names the colour surface (S) and the depth target (Z) at its address with
 * the serial of their last write -- the newer one is what was sampled -- or G
 * for guest memory. Each presented surface goes to <dir>/presentNNN.ppm,
 * named after the pass count, and the capture runs on across presents until
 * RSX_ENGINE_PASS_DUMP_MAX passes (default 64). RSX_ENGINE_PASS_DUMP_DRAWS=1
 * also writes one line per draw to <dir>/draws.txt: its pipeline, colour
 * mask, blend, depth state and the textures it samples.
 *
 * The question it answers is which pass of a multi-pass frame first comes out
 * wrong.
 *
 * RSX_ENGINE_PASS_DUMP_TRIGGER=<file>: instead of arming once by time or
 * draw count, look for <file> every 30 presents; when it exists its content
 * names a subdirectory, the file is deleted and a window of _MAX passes is
 * captured into <dir>/<name>/ from the next frame, then the dump waits for
 * the next trigger. The same scheme drives RPCS3's VKR2PassDump.cpp. */
static struct {
    int state;              /* 0 unread, 1 waiting, 2 capturing, 3 done/off */
    const char* dir;
    const char* base;       /* RSX_ENGINE_PASS_DUMP */
    const char* trigger;    /* RSX_ENGINE_PASS_DUMP_TRIGGER */
    u32 flips;
    char sub[1024];
    u32 min_draws;
    u32 after_s;
    u32 max_passes;
    time_t start;
    FILE* index;
    FILE* draws_log;        /* RSX_ENGINE_PASS_DUMP_DRAWS: one line a draw */
    u32 pass;
    int open;
    u32 n_targets, targets[RSX_BE_MAX_COLOR_TARGETS], zslot;
    u32 draws, clears;
    char first[2048];
} s_pd;

static int pd_open_index(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/passes.txt", s_pd.dir);
    s_pd.index = fopen(path, "w");
    if (!s_pd.index) { s_pd.state = s_pd.trigger ? 1 : 3; return 0; }
    s_pd.pass = 0;
    if (getenv("RSX_ENGINE_PASS_DUMP_DRAWS")) {
        snprintf(path, sizeof path, "%s/draws.txt", s_pd.dir);
        s_pd.draws_log = fopen(path, "w");
    }
    s_pd.state = 2;
    s_pd.open = 0;
    return 1;
}

static int pd_capturing(void)
{
    if (s_pd.state == 0) {
        s_pd.dir = getenv("RSX_ENGINE_PASS_DUMP");
        const char* m = getenv("RSX_ENGINE_PASS_DUMP_MINDRAWS");
        s_pd.min_draws = m ? (u32)strtoul(m, NULL, 0) : 1u;
        const char* a = getenv("RSX_ENGINE_PASS_DUMP_AFTER");
        s_pd.after_s = a ? (u32)strtoul(a, NULL, 0) : 0u;
        const char* x = getenv("RSX_ENGINE_PASS_DUMP_MAX");
        s_pd.max_passes = x ? (u32)strtoul(x, NULL, 0) : 64u;
        s_pd.start = time(NULL);
        s_pd.base = s_pd.dir;
        s_pd.trigger = getenv("RSX_ENGINE_PASS_DUMP_TRIGGER");
        if (s_pd.trigger && !*s_pd.trigger) s_pd.trigger = NULL;
        s_pd.state = (s_pd.dir && *s_pd.dir) ? 1 : 3;
    }
    if (s_pd.state == 1 && s_pd.after_s && !s_pd.trigger &&
        difftime(time(NULL), s_pd.start) >= (double)s_pd.after_s && pd_open_index())
        fprintf(s_pd.index, "from a draw of frame %u, %u s in\n", g.frames, s_pd.after_s);
    return s_pd.state == 2;
}

static float pd_half(u16 h)
{
    const u32 e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0)       v = (float)m / 16777216.0f;              /* m * 2^-24 */
    else if (e == 31) v = m ? 0.0f : 65504.0f;
    else {
        v = (1.0f + (float)m / 1024.0f);
        for (u32 i = e; i > 15; i--) v *= 2.0f;
        for (u32 i = e; i < 15; i++) v *= 0.5f;
    }
    return (h & 0x8000u) ? -v : v;
}

static u8 pd_unit(float v)
{
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return (u8)(v * 255.0f + 0.5f);
}

/* One colour surface to a binary PPM, alpha dropped, floats clamped to [0,1]. */
static void pd_write_surface_rect(u32 slot, const char* path, u32 rx, u32 ry, u32 rw, u32 rh);
static void pd_write_surface(u32 slot, const char* path)
{
    pd_write_surface_rect(slot, path, 0, 0, 0, 0);
}

/* RSX_ENGINE_PASS_DUMP_DRAWIMG=<pass>[,x,y,w,h]: target 0 (or that crop of
 * it) after every draw of that pass, to find the draw that wrote a region. */
static void pd_draw_image(void)
{
    static int want = -2;
    static u32 rx, ry, rw, rh;
    if (want == -2) {
        const char* e = getenv("RSX_ENGINE_PASS_DUMP_DRAWIMG");
        want = -1;
        if (e && *e) {
            unsigned a = 0, b = 0, c = 0, d = 0, n = 0;
            const int k = sscanf(e, "%u,%u,%u,%u,%u", &n, &a, &b, &c, &d);
            want = (int)n;
            if (k == 5) { rx = a; ry = b; rw = c; rh = d; }
        }
    }
    if (want < 0 || !s_pd.open || s_pd.pass != (u32)want || !s_pd.n_targets || !s_pd.draws)
        return;
    char path[1024];
    snprintf(path, sizeof path, "%s/pass%03u_d%04u.ppm", s_pd.dir, s_pd.pass, s_pd.draws - 1u);
    pd_write_surface_rect(s_pd.targets[0], path, rx, ry, rw, rh);
}

static void pd_write_surface_rect(u32 slot, const char* path, u32 rx, u32 ry, u32 rw, u32 rh)
{
    const eng_surface* s = &g.surfaces[slot];
    u32 bpp;
    switch (s->fmt) {
    case RSX_BE_FMT_R8G8B8A8:      bpp = 4;  break;
    case RSX_BE_FMT_R16G16B16A16F: bpp = 8;  break;
    case RSX_BE_FMT_R32G32B32A32F: bpp = 16; break;
    case RSX_BE_FMT_R32F:          bpp = 4;  break;
    default: return;
    }
    if (!s->handle || !s->w || !s->h || !g.be->readback) return;
    if (!rw || !rh || rx >= s->w || ry >= s->h) { rx = ry = 0; rw = s->w; rh = s->h; }
    if (rx + rw > s->w) rw = s->w - rx;
    if (ry + rh > s->h) rh = s->h - ry;
    u8* raw = (u8*)malloc((size_t)rw * rh * bpp);
    u8* rgb = (u8*)malloc((size_t)rw * rh * 3u);
    FILE* f = (raw && rgb) ? fopen(path, "wb") : NULL;
    if (f) {
        memset(raw, 0, (size_t)rw * rh * bpp);
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        g.be->readback(g.be->user, s->handle, rx, ry, rw, rh, raw, rw * bpp);
        for (size_t i = 0; i < (size_t)rw * rh; i++) {
            const u8* p = raw + i * bpp;
            u8* o = rgb + i * 3u;
            if (s->fmt == RSX_BE_FMT_R8G8B8A8) {
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
            } else if (s->fmt == RSX_BE_FMT_R16G16B16A16F) {
                for (u32 c = 0; c < 3; c++) {
                    u16 h; memcpy(&h, p + c * 2u, 2);
                    o[c] = pd_unit(pd_half(h));
                }
            } else if (s->fmt == RSX_BE_FMT_R32G32B32A32F) {
                for (u32 c = 0; c < 3; c++) {
                    float v; memcpy(&v, p + c * 4u, 4);
                    o[c] = pd_unit(v);
                }
            } else {
                float v; memcpy(&v, p, 4);
                o[0] = o[1] = o[2] = pd_unit(v);
            }
        }
        fprintf(f, "P6\n%u %u\n255\n", rw, rh);
        fwrite(rgb, 1, (size_t)rw * rh * 3u, f);
        fclose(f);
    }
    free(raw);
    free(rgb);
}

/* A depth target to a greyscale PPM, stretched from its smallest to its
 * largest value so a shallow range still shows; the range goes to the index. */
static void pd_write_depth(u32 zslot, const char* path, float* lo_out, float* hi_out)
{
    const eng_zdepth* z = &g.zdepths[zslot];
    *lo_out = *hi_out = -1.0f;
    if (!g.be->depth_readback || !z->handle) return;
    const u32 w = z->used_w && z->used_w < z->w ? z->used_w : z->w;
    const u32 h = z->used_h && z->used_h < z->h ? z->used_h : z->h;
    float* d = (float*)malloc((size_t)w * h * sizeof(float));
    u8* px = (u8*)malloc((size_t)w * h);
    FILE* f = (d && px) ? fopen(path, "wb") : NULL;
    if (f) {
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        if (g.be->depth_readback(g.be->user, z->handle, 0, 0, w, h, d, w * 4u) == 0) {
            float lo = 1.0f, hi = 0.0f;
            for (size_t i = 0; i < (size_t)w * h; i++) {
                if (d[i] < lo) lo = d[i];
                if (d[i] > hi) hi = d[i];
            }
            const float span = hi > lo ? hi - lo : 1.0f;
            for (size_t i = 0; i < (size_t)w * h; i++)
                px[i] = (u8)((d[i] - lo) / span * 255.0f);
            fprintf(f, "P5\n%u %u\n255\n", w, h);
            fwrite(px, 1, (size_t)w * h, f);
            *lo_out = lo; *hi_out = hi;
        }
        fclose(f);
    }
    free(d);
    free(px);
}

static void pd_close_pass(void)
{
    if (!s_pd.open) return;
    pd_draw_image();
    char path[1024];
    fprintf(s_pd.index, "pass %03u: draws %u clears %u |", s_pd.pass, s_pd.draws,
            s_pd.clears);
    for (u32 k = 0; k < s_pd.n_targets; k++) {
        const eng_surface* s = &g.surfaces[s_pd.targets[k]];
        fprintf(s_pd.index, " %c 0x%08X fmt %d %ux%u p%u", 'A' + (int)k, s->offset,
                (int)s->fmt, s->w, s->h, s->pitch);
        snprintf(path, sizeof path, "%s/pass%03u_t%u.ppm", s_pd.dir, s_pd.pass, k);
        pd_write_surface(s_pd.targets[k], path);
    }
    if (s_pd.zslot != ENG_INVALID) {
        float lo, hi;
        snprintf(path, sizeof path, "%s/pass%03u_z.pgm", s_pd.dir, s_pd.pass);
        pd_write_depth(s_pd.zslot, path, &lo, &hi);
        const eng_zdepth* z = &g.zdepths[s_pd.zslot];
        fprintf(s_pd.index, " | zeta 0x%08X p%u depth %.9g..%.9g (%ux%u, written %ux%u)",
                z->offset, z->pitch, lo, hi, z->w, z->h, z->used_w, z->used_h);
    }
    fprintf(s_pd.index, " |%s\n", s_pd.first);
    fflush(s_pd.index);
    s_pd.pass++;
    s_pd.open = 0;
    if (s_pd.pass >= s_pd.max_passes) {
        fprintf(s_pd.index, "stopped after %u passes\n", s_pd.pass);
        fclose(s_pd.index);
        s_pd.index = NULL;
        if (s_pd.draws_log) { fclose(s_pd.draws_log); s_pd.draws_log = NULL; }
        s_pd.state = s_pd.trigger ? 1 : 3;        /* a trigger window: wait for the next */
        fprintf(stderr, "[rsx engine] pass dump: %u passes written to %s\n",
                s_pd.pass, s_pd.dir);
    }
}

/* A draw (or, with rs NULL, a clear) into this target set: close the pass
 * when the set changed, and describe the first draw of a new one. */
static void pd_note(const u32* targets, u32 n_targets, u32 zslot,
                    const rsx_be_render_state* rs, u32 pipeline, u32 tex_mask)
{
    if (!pd_capturing()) return;
    pd_draw_image();
    int same = s_pd.open && s_pd.n_targets == n_targets && s_pd.zslot == zslot;
    for (u32 k = 0; same && k < n_targets; k++) same = s_pd.targets[k] == targets[k];
    if (!same) {
        pd_close_pass();
        s_pd.open = 1;
        s_pd.n_targets = n_targets;
        memcpy(s_pd.targets, targets, n_targets * sizeof(u32));
        s_pd.zslot = zslot;
        s_pd.draws = s_pd.clears = 0;
        s_pd.first[0] = 0;
    }
    if (!rs) {
        /* A clear: `pipeline` carries sink_clear's outcome (bit 0 zeta live,
         * bit 1 colour live, bits 4-7 RSX_BE_CLEAR_* issued, bits 8-11 the
         * colour channel mask issued) and `tex_mask` the CLEAR_SURFACE mask
         * as the guest wrote it. */
        s_pd.clears++;
        const u32 h = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
        const u32 v = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
        rsx_dsp_surface csf;
        rsx_dsp_get_surface(&g.rsx, &csf);
        const u32 zs = rsx_dsp_reg(&g.rsx, M_ZSTENCIL_CLEAR);
        const u32 smask = rsx_dsp_reg(&g.rsx, M_STENCIL_MASK) & 0xFFu;
        const u32 zfloat = (rsx_dsp_reg(&g.rsx, 0x03B0) >> 12) & 1u;
        char info[256];
        snprintf(info, sizeof info,
                 "mask 0x%02X zs 0x%08X smask 0x%02X zfmt %u%s aa %u tgt 0x%02X"
                 " z %u:0x%08X p%u c0 0x%08X p%u zlive %u clive %u ds %X ch %X",
                 tex_mask & 0xFFu, zs, smask, csf.depth_format, zfloat ? "f" : "",
                 csf.antialias, csf.color_target, csf.zeta_location, csf.zeta_offset,
                 csf.zeta_pitch, csf.color_offset[0], csf.color_pitch[0],
                 pipeline & 1u, (pipeline >> 1) & 1u, (pipeline >> 4) & 0xFu,
                 (pipeline >> 8) & 0xFu);
        const size_t n = strlen(s_pd.first);
        snprintf(s_pd.first + n, sizeof s_pd.first - n,
                 " [clear after %u draws, scissor %u,%u %ux%u, %s]",
                 s_pd.draws, h & 0xFFFFu, v & 0xFFFFu, h >> 16, v >> 16, info);
        if (s_pd.draws_log)
            fprintf(s_pd.draws_log, "pass %03u clear after %u draws: %s\n",
                    s_pd.pass, s_pd.draws, info);
        return;
    }
    if (s_pd.draws_log) {
        fprintf(s_pd.draws_log, "pass %03u draw %u: pipe %u cmask %08X blend %u %X/%X"
                " dt %u dw %u at %u |", s_pd.pass, s_pd.draws, pipeline, rs->color_mask,
                rs->blend_enable, rs->sf_rgb, rs->df_rgb, rs->depth_test, rs->depth_write,
                rs->alpha_test_enable);
        for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
            if (!(tex_mask & (1u << u))) continue;
            rsx_dsp_texture t;
            rsx_dsp_get_texture(&g.rsx, u, &t);
            fprintf(s_pd.draws_log, " t%u %u:0x%08X f%02X %ux%u", u, t.location, t.offset,
                    t.format & 0xFFu, t.width, t.height);
        }
        fprintf(s_pd.draws_log, "\n");
    }
    if (s_pd.draws++) return;

    rsx_dsp_surface sf;
    rsx_dsp_viewport vp;
    rsx_dsp_get_surface(&g.rsx, &sf);
    rsx_dsp_get_viewport(&g.rsx, &vp);
    size_t at = strlen(s_pd.first);
    at += (size_t)snprintf(s_pd.first + at, sizeof s_pd.first - at,
        " rt_sel %u cfmt %u zfmt %u clip %ux%u | pipe %u dt %u dw %u df %u cmask %08X"
        " blend %u %X/%X stencil %u vpz %g+%g |",
        sf.color_target, sf.color_format, sf.depth_format, sf.clip_w, sf.clip_h,
        pipeline, rs->depth_test, rs->depth_write, rs->depth_func, rs->color_mask,
        rs->blend_enable, rs->sf_rgb, rs->df_rgb, rs->stencil_enable,
        vp.scale[2], vp.translate[2]);
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES && at < sizeof s_pd.first; u++) {
        if (!(tex_mask & (1u << u))) continue;
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        /* S<n> a colour surface, Z<n> a depth target, each with the serial
         * of its last write: the newer one is what the binding sampled. */
        char src[48] = "G";
        size_t sl = 0;
        {
            const u32 i = eng_surface_newest(t.location, t.offset, NULL, 0);
            if (i != ENG_INVALID)
                sl = (size_t)snprintf(src, sizeof src, "S%u@%llu", i,
                                      (unsigned long long)g.surfaces[i].color_serial);
        }
        for (u32 i = 0; i < g.n_zdepths; i++)
            if (g.zdepths[i].location == t.location && g.zdepths[i].offset == t.offset) {
                snprintf(src + sl, sizeof src - sl, "%sZ%u@%llu", sl ? "," : "", i,
                         (unsigned long long)g.zdepths[i].depth_serial);
                break;
            }
        at += (size_t)snprintf(s_pd.first + at, sizeof s_pd.first - at,
            " t%u %u:0x%08X f%02X %ux%u p%u%s r%04X zf%u %s", u, t.location, t.offset,
            t.format & 0xFFu, t.width, t.height, t.pitch, t.cubemap ? " cube" : "",
            t.remap & 0xFFFFu, t.wrap >> 28, src);
    }
}

/* RSX_ENGINE_PASS_DUMP_TRIGGER: every 30 presents while idle, a trigger file
 * names the subdirectory of the next capture window; it is consumed. */
static int pd_trigger_poll(void)
{
    if (s_pd.flips++ % 30u) return 0;
    FILE* f = fopen(s_pd.trigger, "rb");
    if (!f) return 0;
    char name[128] = {0};
    const size_t n = fread(name, 1, sizeof name - 1, f);
    fclose(f);
    remove(s_pd.trigger);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const char c = name[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '-' || c == '_' || c == '.')
            name[k++] = c;
        else if (c == '\r' || c == '\n') break;
    }
    name[k] = 0;
    if (!k || !strcmp(name, ".") || !strcmp(name, ".."))
        snprintf(name, sizeof name, "trigger%u", s_pd.flips);
    snprintf(s_pd.sub, sizeof s_pd.sub, "%s/%s", s_pd.base, name);
#ifdef _WIN32
    CreateDirectoryA(s_pd.sub, NULL);
#else
    mkdir(s_pd.sub, 0755);
#endif
    s_pd.dir = s_pd.sub;
    if (!pd_open_index()) return 0;
    fprintf(s_pd.index, "trigger '%s' at frame %u (after a frame of %u guest draws)\n",
            name, g.frames + 1, g.guest_draws);
    fprintf(stderr, "[rsx engine] pass dump: trigger '%s', capturing into %s\n", name,
            s_pd.dir);
    return 1;
}

/* Called on every present: arm on the first frame heavy enough when no delay
 * was asked for, and while capturing, close the pass in progress and write
 * the presented surface. */
static void pd_present(u32 presented)
{
    if (s_pd.state == 0) pd_capturing();
    if (s_pd.state == 1 && s_pd.trigger) {
        pd_trigger_poll();
        return;
    }
    if (s_pd.state == 1 && !s_pd.after_s && g.guest_draws >= s_pd.min_draws) {
        if (pd_open_index())
            fprintf(s_pd.index, "frame %u (after a frame of %u guest draws)\n",
                    g.frames + 1, g.guest_draws);
        return;
    }
    if (s_pd.state == 2) {
        char path[1024];
        pd_close_pass();
        if (s_pd.state != 2) return;               /* that pass was the last */
        snprintf(path, sizeof path, "%s/present%03u.ppm", s_pd.dir, s_pd.pass);
        pd_write_surface(presented, path);
        fprintf(s_pd.index, "present%03u: surface %u 0x%08X, %u guest draws\n",
                s_pd.pass, presented, g.surfaces[presented].offset, g.guest_draws);
        fflush(s_pd.index);
    }
}

/* rsx_draw_engine_set_aa_hooks: the title layer's temporal AA policy. */
static const rsx_aa_hooks* s_aa_hooks;

static void sink_end_impl(void* user, const rsx_dispatch* r)
{
    (void)user; (void)r;
    if (!g.ready || !dc.n_packets || !dc.fetch_ok) return;
    /* Mixed geometry sources have no defined concatenation here. Reject the
     * whole group instead of drawing partial/misordered geometry. */
    if (dc.n_immediate && (dc.n_arr || dc.n_idx || dc.inl_bytes || dc.n_elem ||
                          r->inline_dropped != dc.inline_dropped_at_begin)) return;
    /* An ARRAY_ELEMENT list that overran the dispatcher cap draws nothing. */
    if (!dc.n_arr && !dc.n_idx && r->elem_dropped != dc.elem_dropped_at_begin) return;

    const u32 prim = g.rsx.current_primitive;
    /* Everything that becomes triangles -- lists, strips, fans, quads, quad
     * strips and polygons -- is REBUILT into one triangle list through an
     * index buffer. A host strip topology cannot express a restart cut, which
     * is the whole reason the reference engine rebuilds them, and rebuilding
     * is also what lets repeated references share an uploaded vertex. Points
     * and lines pass through as the guest issued them, and anything left that
     * would need an expansion this engine does not have is dropped rather
     * than drawn as something else. */
    rsx_topology topology = RSX_TOPOLOGY_TRIANGLES;
    int scratch = 0;
    const int rebuild = eng_topology_rebuild(prim, 0, &scratch) != 0;
    if (rebuild) {
        topology = eng_topology_host(prim);
    } else {
        if (rsx_primitive_needs_expansion(prim)) return;
        topology = rsx_primitive_topology(prim);
        if (topology == RSX_TOPOLOGY_UNSUPPORTED) return;
    }

    /* RPCS3 analyse_inputs_interleaved: a push buffer of one vertex is not
     * used; the registers at draw time feed it (an attribute written after
     * the position counts). RSX_ENG_IMM_SINGLE_LATE=0: the ATTR0 snapshot. */
    if (dc.n_immediate == 1u) {
        static int late = -1;
        if (late < 0) { const char* e = getenv("RSX_ENG_IMM_SINGLE_LATE"); late = !(e && e[0] == '0'); }
        if (late) eng_imm_snapshot(r, dc.immediate[0]);
    }
    rsx_vertex_layout_plan layout;
    eng_vertex_layout(&layout);
    dc_fetch(&layout, rebuild);
    if (!dc.n_verts || !dc.fetch_ok) return;

    int indexed = 0;
    u32 n_draw = dc.n_source_refs;
    if (rebuild) {
        if (!eng_topology_rebuild(prim, dc.refs_remapped, &indexed)) return;
        n_draw = indexed
            ? rsx_draw_engine_topology_index_count(prim, dc.n_source_refs,
                                                   dc.cuts, dc.n_cuts)
            : dc.n_source_refs - dc.n_source_refs % 3u;
    }
    if (!n_draw) return;

    rsx_be_render_state rs;
    rsx_draw_engine_decode_render_state(&g.rsx, &rs);

    /* RPCS3 get_framebuffer_layout for the draw context, taken before any
     * surface is looked up: what it drops or leaves unused is not bound and
     * must not create, resize or reformat the surface at that address. */
    eng_fb_layout fbl;
    u32 targets[RSX_BE_MAX_COLOR_TARGETS];
    u32 zslot = ENG_INVALID;
    const u32 n_targets = eng_fb_bind(ENG_FB_DRAW, &fbl, targets, &zslot);
    if (!n_targets) return;
    /* RPCS3 should_skip_draw: no attachment left draws nothing unless a ZPASS
     * query is counting; an invalid layout (clip w/h 0) never draws. */
    if (eng_fb_surfaces() && fbl.invalid) return;
    if (eng_fb_surfaces() && !s_eng_occl_active && !fbl.any_color && !fbl.zeta_live) return;
    if (!eng_legacy_surfaces() && eng_scissor_invalid(1)) return;
    const u32 target = targets[0];

    rsx_dsp_surface sf;
    rsx_dsp_viewport vp;
    rsx_dsp_get_surface(&g.rsx, &sf);
    rsx_dsp_get_viewport(&g.rsx, &vp);
    /* A zeta the layout drops (pitch too small for the written row, or the
     * colour buffer's address claimed by an untested draw) is not bound, so
     * the draw runs with no depth or stencil test; R2's low-resolution
     * particle passes set zeta pitch 0 with the test left on. One it leaves
     * unused (no test reads it: test off, or no write with NEVER/ALWAYS) is
     * not bound either, as RPCS3 binds zeta_address 0 -- and a host pass with
     * no depth attachment has no depth test, so the state goes off with it.
     * RSX_ENG_FB_SURFACES=0 keeps an unused zeta bound with the state as
     * decoded. A colour buffer it drops -- target NONE, no channel written,
     * a pitch too small, or the address taken by the depth buffer -- binds a
     * scratch attachment (eng_current_target_set_ex) and receives no write. */
    const int z_dropped = zslot == ENG_INVALID;
    if (z_dropped) {
        rs.depth_test = 0;
        rs.depth_write = 0;
        rs.stencil_enable = 0;
    }
    {
        u32 idx[4];
        const u32 n_idx = eng_rtt_indexes(sf.color_target, idx);
        if (!n_idx || !fbl.color_live[idx[0]]) rs.color_mask = 0;
        for (u32 i = 1; i < n_idx; i++)
            if (!fbl.color_live[idx[i]]) rs.color_mask_mrt &= ~(0xFu << (i * 4u));
    }

    /* A guest program pair that will not translate has no fallback: the draw
     * is dropped, as the reference engine drops it. Substituting the built-in
     * program would draw the geometry in the wrong colours, which is harder to
     * see than a missing object and hides the translation failure. */
    int pipeline_is_fixed = 1;
    u32 tex_used = 0;
    u64 fp_hash = 0;
    const u32 pipeline = eng_pipeline_get(&layout, &rs,
                                          eng_draw_rt_format(target, sf.color_format),
                                          n_targets, &pipeline_is_fixed, &tex_used,
                                          &fp_hash);
    if (!pipeline) return;
    if (fp_hash && eng_debug_fp(fp_hash, pipeline)) return;

    if (s_aa_hooks && s_aa_hooks->on_draw) {
        rsx_aa_draw_info ai;
        memset(&ai, 0, sizeof ai);
        ai.rsx = &g.rsx;
        ai.color = g.surfaces[target].handle;
        ai.color_location = g.surfaces[target].location;
        ai.color_offset = g.surfaces[target].offset;
        ai.depth = zslot != ENG_INVALID ? g.zdepths[zslot].handle : 0;
        ai.depth_location = sf.zeta_location;
        ai.depth_offset = zslot != ENG_INVALID ? sf.zeta_offset : ~0u;
        ai.w = sf.clip_w ? sf.clip_w : g.width;
        ai.h = sf.clip_h ? sf.clip_h : g.height;
        ai.aa_sx = sf.aa_sx; ai.aa_sy = sf.aa_sy;
        ai.depth_test = rs.depth_test; ai.depth_write = rs.depth_write;
        ai.depth_func = rs.depth_func;
        ai.tex_used = tex_used;
        for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
            if (!(tex_used & (1u << u))) continue;
            rsx_dsp_texture t;
            rsx_dsp_get_texture(&g.rsx, u, &t);
            ai.tex_location[u] = t.location; ai.tex_offset[u] = t.offset;
        }
        /* Stored depth -> the normalised z of the epilogue below. */
        ai.zA = 1.0f; ai.zB = 0.0f;
        if (g.be->set_depth_range) {
            float n = 0.0f, f = 1.0f;
            const u32 cn = rsx_dsp_reg(&g.rsx, 0x0394), cf = rsx_dsp_reg(&g.rsx, 0x0398);
            if (g.rsx.seen[0x0394 >> 2]) memcpy(&n, &cn, 4);
            if (g.rsx.seen[0x0398 >> 2]) memcpy(&f, &cf, 4);
            if (n == n && f == f && n >= 0.0f && n <= 1.0f && f >= 0.0f && f <= 1.0f &&
                (f - n > 0.000001f || n - f > 0.000001f)) {
                ai.zA = 1.0f / (f - n); ai.zB = -n / (f - n);
            }
        }
        s_aa_hooks->on_draw(s_aa_hooks->user, &ai);
    }

    /* Textures before any pipeline or target binding: a cache refresh can
     * replace a resource, and the backend is free to submit while doing it. */
    u32 textures[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
    u32 vtextures[RSX_BE_MAX_VERTEX_TEXTURES];
    rsx_be_sampler_desc vsamplers[RSX_BE_MAX_VERTEX_TEXTURES];
    memset(textures, 0, sizeof textures);
    memset(samplers, 0, sizeof samplers);
    memset(vtextures, 0, sizeof vtextures);
    memset(vsamplers, 0, sizeof vsamplers);
    const u32 tex_mask = sink_bind_textures(targets, n_targets, zslot, tex_used,
                                            textures, samplers);
    if (tex_mask == ENG_INVALID) return;
    u32 dc_tex_mask = tex_mask;
    const u32 dc_unit = s_eng_depth_cmp_unit;
    if (dc_unit && zslot != ENG_INVALID) {
        /* rsx_fp_frag_state depth_compare: the zeta as it stands before
         * this draw. With no snapshot the unit keeps the backend's null
         * texture and every fragment keeps its own depth. */
        const u32 snap = eng_zdepth_snapshot(zslot);
        if (snap) {
            textures[dc_unit - 1u] = snap;
            memset(&samplers[dc_unit - 1u], 0, sizeof samplers[0]);
            samplers[dc_unit - 1u].wrap_s = samplers[dc_unit - 1u].wrap_t =
                samplers[dc_unit - 1u].wrap_r = 3;   /* CLAMP_TO_EDGE */
            dc_tex_mask |= 1u << (dc_unit - 1u);
        }
    }
    u32 bind_tex_mask = dc_tex_mask;
    const u32 pb_unit = s_eng_pb_unit;
    if (pb_unit && target < g.n_surfaces && g.surfaces[target].handle) {
        /* rsx_fp_frag_state pb: colour target 0 as it stands before this
         * draw, the destination do_blend reads (RPCS3 subpassLoad). */
        if (s_pb_copy) g.be->texture_release(g.be->user, s_pb_copy);
        s_pb_copy = g.be->color_snapshot(g.be->user, g.surfaces[target].handle);
        if (s_pb_copy) {
            textures[pb_unit - 1u] = s_pb_copy;
            memset(&samplers[pb_unit - 1u], 0, sizeof samplers[0]);
            samplers[pb_unit - 1u].wrap_s = samplers[pb_unit - 1u].wrap_t =
                samplers[pb_unit - 1u].wrap_r = 3;   /* CLAMP_TO_EDGE */
            bind_tex_mask |= 1u << (pb_unit - 1u);
        }
    }
    const u32 vtex_mask = sink_bind_vertex_textures(eng_vtex_mask(), vtextures,
                                                    vsamplers, targets, n_targets, zslot);

    if (indexed) {
        if (!dc_reserve_indices(n_draw)) return;
        rsx_draw_engine_write_topology_indices(
            prim, dc.n_source_refs, dc.cuts, dc.n_cuts,
            dc.refs_remapped ? dc.ref_remap.occurrence_to_unique : NULL,
            g.indices);
    }

    /* The transform constant block, then the viewport epilogue the vertex
     * program's tail multiplies by: RSX window coordinates mapped into the
     * host's clip space (rsx_live_draw.c:6143-6156). */
    const float W = sf.clip_w ? (float)sf.clip_w : (float)g.width;
    const float H = sf.clip_h ? (float)sf.clip_h : (float)g.height;
    /* The guest viewport is in pixels; an antialiased surface's clip is in
     * samples (aa_sx x aa_sy per pixel), so map NDC over the pixel extent. */
    const float Wv = W / (float)sf.aa_sx;
    const float Hv = H / (float)sf.aa_sy;
    float xf[8] = { 1, 1, 1, 0, 0, 0, 0, 0 };
    if (!rsx_vertex_legacy()) {
        /* RPCS3 fill_scale_offset_data, always applied: a never-written
         * scale/offset reads the reset image (2048, 2048, 0.5)
         * (rsx_methods.cpp:474-481), and a written zero is a real zero. */
        static const float k_vp_reset[3] = { 2048.0f, 2048.0f, 0.5f };
        for (u32 i = 0; i < 3; i++) {
            if (!g.rsx.seen[(0x0A30u >> 2) + i]) vp.scale[i] = k_vp_reset[i];
            if (!g.rsx.seen[(0x0A20u >> 2) + i]) vp.translate[i] = k_vp_reset[i];
        }
    }
    if (!rsx_vertex_legacy() || vp.scale[0] != 0.0f || vp.translate[0] != 0.0f) {
        xf[0] = vp.scale[0] / (Wv * 0.5f);
        xf[1] = -(vp.scale[1] / (Hv * 0.5f));
        xf[2] = vp.scale[2];
        xf[4] = (vp.translate[0] - Wv * 0.5f) / (Wv * 0.5f);
        xf[5] = -((vp.translate[1] - Hv * 0.5f) / (Hv * 0.5f));
        xf[6] = vp.translate[2];
    }
    /* RSX clips window z to [CLIP_MIN, CLIP_MAX] and stores it unchanged
     * (RPCS3 apply_zclip_xform + viewport minDepth/maxDepth = clip range):
     * normalise z over the clip range here, the viewport maps it back. */
    float zr_n = 0.0f, zr_f = 1.0f;
    if (g.be->set_depth_range) {
        const u32 cn = rsx_dsp_reg(&g.rsx, 0x0394), cf = rsx_dsp_reg(&g.rsx, 0x0398);
        if (g.rsx.seen[0x0394 >> 2]) memcpy(&zr_n, &cn, 4);
        if (g.rsx.seen[0x0398 >> 2]) memcpy(&zr_f, &cf, 4);
        if (!(zr_n == zr_n) || !(zr_f == zr_f)) { zr_n = 0.0f; zr_f = 1.0f; }
        if (zr_n != 0.0f || zr_f != 1.0f) {
            const float lo = zr_n < zr_f ? zr_n : zr_f, hi = zr_n < zr_f ? zr_f : zr_n;
            const float inv = (hi - lo) > 0.000001f ? 1.0f / (hi - lo) : 0.0f;
            xf[2] *= inv;
            xf[6] = (xf[6] - lo) * inv;
        }
        /* RPCS3 set_viewport: the raw clip range under
         * VK_EXT_depth_range_unrestricted, else [0, 1] (the backend decides). */
        g.be->set_depth_range(g.be->user, zr_n, zr_f);
    }
    /* RPCS3 apply_zclip_xform (RSXVertexPrologue.glsl): a normalised depth
     * within one 24-bit step of an integer snaps to it. vp_posoffset.w
     * carries that step, 1 / (range * 16777215); 0 disables the snap. */
    if (!rsx_vertex_legacy()) {
        const float range = zr_n < zr_f ? zr_f - zr_n : zr_n - zr_f;
        xf[7] = range > 0.000001f ? 1.0f / (range * 16777215.0f) : 0.0f;
    }
    {
        /* RSX_PERF_VPCB: only the constant slots written since the last
         * build are copied. A draw clause's replayed constant writes bypass
         * the method hook, so a clause with barriers copies everything, now
         * and for the next draw (trailing barriers land after its last END). */
        static int perf = -1;
        const u32 ncb = RSX_DSP_NUM_CONSTANTS;
        if (!eng_perf_on("RSX_PERF_VPCB", &perf) || g.rsx.clause_n_barriers) {
            memcpy(g.vp_cb, g.rsx.constants, ncb * 16u);
            ENG_PC(PC_VPCB, 0);
            if (g.rsx.clause_n_barriers) { s_eng_cdirty_lo = 0u; s_eng_cdirty_hi = 0x7FFFFFFFu; }
            else { s_eng_cdirty_lo = 0xFFFFFFFFu; s_eng_cdirty_hi = 0u; }
        } else {
            if (s_eng_cdirty_lo <= s_eng_cdirty_hi && s_eng_cdirty_lo < ncb) {
                const u32 hi = s_eng_cdirty_hi < ncb ? s_eng_cdirty_hi : ncb - 1u;
                memcpy(g.vp_cb + s_eng_cdirty_lo * 16u, g.rsx.constants[s_eng_cdirty_lo],
                       (hi - s_eng_cdirty_lo + 1u) * 16u);
                ENG_PC(PC_VPCB, 0);
            } else {
                ENG_PC(PC_VPCB, 1);
            }
            s_eng_cdirty_lo = 0xFFFFFFFFu; s_eng_cdirty_hi = 0u;
        }
    }
    if (s_aa_hooks && s_aa_hooks->on_transform) {
        float jp[2] = { 0.0f, 0.0f };
        if (s_aa_hooks->on_transform(s_aa_hooks->user, xf, jp)) {
            xf[4] += 2.0f * jp[0] / W;      /* D3D NDC: +y up, pixels down */
            xf[5] -= 2.0f * jp[1] / H;
        }
    }
    memcpy(g.vp_cb + RSX_DSP_NUM_CONSTANTS * 16u, xf, sizeof xf);
    {
        /* vp_branch (rsx_vp_decompiler.h): RPCS3's vertex context words. */
        u32 vb[4];
        vb[0] = rsx_dsp_reg(&g.rsx, 0x1FF8);
        vb[1] = rsx_dsp_reg(&g.rsx, 0x1FF4);
        vb[2] = (rsx_dsp_reg(&g.rsx, 0x1478) & 0x00FFFFFFu) | RSX_VP_BRANCH_ENV_VALID;
        vb[3] = g.rsx.seen[0x1EE0 >> 2] ? rsx_dsp_reg(&g.rsx, 0x1EE0) : 0x3F800000u;
        memcpy(g.vp_cb + (RSX_DSP_NUM_CONSTANTS + 2u) * 16u, vb, sizeof vb);
    }

    /* The buffered fragment constants, then fp_alpha: the layout
     * rsx_fp_decompile_buffered_ex compiled the shader against. */
    const u32 nslots = g.fp_constants.count ? g.fp_constants.count : 1u;
    const u32 fp_bytes = (nslots + 1u) * 16u;
    if (g.fp_cb_cap < fp_bytes) {
        u8* n = (u8*)realloc(g.fp_cb, fp_bytes);
        if (!n) return;
        g.fp_cb = n;
        g.fp_cb_cap = fp_bytes;
    }
    memset(g.fp_cb, 0, fp_bytes);
    if (g.fp_constants.count)
        memcpy(g.fp_cb, g.fp_constants.values, g.fp_constants.count * 16u);
    { float* alpha = (float*)(g.fp_cb + nslots * 16u);
      alpha[0] = rsx_fp_alpha_ref(rs.alpha_ref_raw, rs.alpha_ref_format);
      /* rsx_fp_frag_state's per-draw values: FOG_PARAMS, WPOS y bias. */
      const u32 fog0 = rsx_dsp_reg(&g.rsx, 0x08D0u), fog1 = rsx_dsp_reg(&g.rsx, 0x08D4u);
      const u32 window = rsx_dsp_reg(&g.rsx, 0x1D88u);
      memcpy(&alpha[1], &fog0, 4);
      memcpy(&alpha[2], &fog1, 4);
      alpha[3] = (((window >> 12) & 15u) == 1u ? (float)(window & 0xFFFu) : 0.0f) -
                 (((window >> 16) & 15u) == 1u ? 0.5f : 0.0f); }

    pd_note(targets, n_targets, zslot, &rs, pipeline, tex_mask);

    /* The depth attachment is cleared the first time it is bound, so a title
     * that never clears its shadow zeta still tests against something. */
    const u32 depth_handle = (zslot != ENG_INVALID) ? g.zdepths[zslot].handle : 0;
    if (zslot != ENG_INVALID) eng_zdepth_init(zslot);

    u32 handles[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i < n_targets; i++) handles[i] = g.surfaces[targets[i]].handle;
    g.be->bind_targets(g.be->user, handles, n_targets, depth_handle);
    g.be->bind_pipeline(g.be->user, pipeline);
    g.be->bind_vs_constants(g.be->user, g.vp_cb, ENG_VP_CB_BYTES);
    g.be->bind_ps_constants(g.be->user, g.fp_cb, fp_bytes);
    g.be->bind_textures(g.be->user, textures, samplers, bind_tex_mask);
    g.be->bind_vertex_textures(g.be->user, vtextures, vsamplers, vtex_mask);
    g.be->set_viewport(g.be->user, 0.0f, 0.0f, W, H);

    /* The guest scissor intersected with the surface. The nv40 reset is a
     * full 4096x4096 window and a never-written register reads 0 here, which
     * the w == 0 test treats as no scissor, so only a real game scissor
     * narrows anything (rsx_live_draw.c:6180-6197). */
    {
        u32 sx = 0, sy = 0;
        u32 sw = g.surfaces[target].w, sh = g.surfaces[target].h;
        u32 h = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
        u32 v = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
        /* RPCS3 resolve_scissor(clip_viewport = true) for draws: the scissor
         * is intersected with the VIEWPORT_HORIZONTAL/VERTICAL rect (unwritten
         * registers at the nv40 reset, 4096 at 0). eng_scissor_invalid has
         * already skipped an empty result. */
        if (!eng_rop_legacy()) {
            const rsx_dispatch* r = &g.rsx;
            const u32 rst = 0x10000000u;
            const u32 axes[2][2] = {
                { r->seen[M_SCISSOR_HORIZONTAL >> 2] ? h : rst,
                  r->seen[0x0A00 >> 2] ? rsx_dsp_reg(r, 0x0A00) : rst },
                { r->seen[M_SCISSOR_VERTICAL >> 2] ? v : rst,
                  r->seen[0x0A04 >> 2] ? rsx_dsp_reg(r, 0x0A04) : rst } };
            u32 res[2];
            for (u32 a = 0; a < 2; ++a) {
                const u32 s0 = axes[a][0] & 0xFFFFu, s1 = s0 + (axes[a][0] >> 16);
                const u32 v0 = axes[a][1] & 0xFFFFu, v1 = v0 + (axes[a][1] >> 16);
                /* x2 = min(sx + sw, vx + vw) truncated to RPCS3's u16; a
                 * wrapped end lands below x1 and the draw is skipped. */
                const u32 lo = s0 > v0 ? s0 : v0, hi = (s1 < v1 ? s1 : v1) & 0xFFFFu;
                res[a] = hi > lo ? (((hi - lo) & 0xFFFFu) << 16) | lo : 0u;
            }
            h = res[0]; v = res[1];
            if (!h || !v) h = v = 0x00010000u, sw = sh = 0;   /* empty: draws nothing */
        }
        /* The scissor is in pixels: scale it to the sample grid. */
        const u32 gx = (h & 0xFFFFu) * sf.aa_sx, gw = (h >> 16) * sf.aa_sx;
        const u32 gy = (v & 0xFFFFu) * sf.aa_sy, gh = (v >> 16) * sf.aa_sy;
        if (gw > 0 && gh > 0) {
            u32 right = sx + sw, bottom = sy + sh;
            if (gx > sx) sx = gx;
            if (gy > sy) sy = gy;
            if (gx + gw < right)  right = gx + gw;
            if (gy + gh < bottom) bottom = gy + gh;
            sw = right > sx ? right - sx : 0;
            sh = bottom > sy ? bottom - sy : 0;
        }
        g.be->set_scissor(g.be->user, sx, sy, sw, sh);
    }
    /* Dynamic, and deliberately outside the pipeline key. */
    g.be->set_stencil_ref(g.be->user,
                          rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF) & 0xFFu);
    if (g.be->set_raster_dynamic) {
        const u32 pfill = rsx_dsp_reg(&g.rsx, 0x0A68) & 1u;
        const u32 ps = rsx_dsp_reg(&g.rsx, 0x0A78), pb = rsx_dsp_reg(&g.rsx, 0x0A7C);
        float fscale, fbias;
        memcpy(&fscale, &ps, sizeof fscale);
        memcpy(&fbias, &pb, sizeof fbias);
        const u32 lw = rsx_dsp_reg(&g.rsx, 0x03B8);
        const float line_w = (float)(lw >> 3) + (float)(lw & 7u) / 8.0f;
        const u32 two = rsx_dsp_reg(&g.rsx, M_TWO_SIDED_STENCIL) & 1u;
        const u32 bref = rsx_dsp_reg(&g.rsx, two ? 0x0354u : M_STENCIL_FUNC_REF) & 0xFFu;
        g.be->set_raster_dynamic(g.be->user, pfill, fscale, fbias,
                                 line_w > 0.0f ? line_w : 1.0f, bref);
    }
    /* The bounds too: a deferred renderer moves them between its light and
     * sky passes without changing anything else. Raw floats, as written. */
    if (g.be->set_depth_bounds) {
        const u32 lo = rsx_dsp_reg(&g.rsx, M_DEPTH_BOUNDS_MIN);
        const u32 hi = rsx_dsp_reg(&g.rsx, M_DEPTH_BOUNDS_MAX);
        float fmin, fmax;
        memcpy(&fmin, &lo, sizeof fmin);
        memcpy(&fmax, &hi, sizeof fmax);
        g.be->set_depth_bounds(g.be->user, fmin, fmax);
    }
    /* RPCS3 decode_constant_blend_color: FP16 targets read four halves
     * (R,G in BLEND_COLOR, B,A in BLEND_COLOR2), the rest A8R8G8B8. */
    if (g.be->set_blend_constants) {
        float k[4];
        eng_blend_constants(sf.color_format, k);
        g.be->set_blend_constants(g.be->user, k);
    }

    const u32 uploaded = indexed ? dc.n_verts : n_draw;
    eng_pcache_note_topology(pipeline, (u32)topology);
    g.be->draw(g.be->user, topology, dc.verts, uploaded, layout.stride,
               indexed ? g.indices : NULL, indexed ? n_draw : 0);
    /* Counted only when the draw ran the guest's OWN programs, which is what
     * the test hook means: a draw through the built-in pair is a draw, not
     * evidence that the decompile-translate-compile path worked. */
    if (!pipeline_is_fixed) g.guest_draws++;

    if (eng_fb_surfaces()) eng_surfaces_wrote_fb(targets, n_targets, &fbl);
    else if (rs.color_mask) eng_surfaces_wrote(targets, n_targets);
    /* RPCS3 VKDraw.cpp:1407 on_write(color, zeta_write_enabled): a pass that
     * only writes stencil also writes the zeta. */
    const int zeta_written = eng_fb_surfaces() ? fbl.zeta_write : (rs.depth_test && rs.depth_write);
    if (zslot != ENG_INVALID && zeta_written) {
        g.zdepths[zslot].had_write = 1;
        /* An emulated depth-EQUAL draw only rewrites the depth it matched,
         * so the snapshot it read stays current (one copy per pass, not per
         * draw). */
        if (dc_tex_mask == tex_mask) eng_zdepth_wrote(zslot, &sf);
    }
}

static void sink_end(void* user, const rsx_dispatch* r)
{
    const u64 t0 = eng_prof_t0();
    sink_end_impl(user, r);
    eng_prof_add(3, t0);
}

/* RPCS3 RSXCoreRenderer decode_clear_state: COLOR_CLEAR_VALUE is a raw copy
 * of the surface word, so the logical colour a clear writes depends on the
 * surface format (color_utils.hpp get_*_clear_color / get_*_clearmask). The
 * engine keeps every 8-bit format as an RGBA8 target of logical channels,
 * with B8 in .b and G8B8 in .g/.b (eng_surface_crossbar), so those keep
 * their channels and only lose the ones the format has not got. Returns the
 * RGBA write mask (bit0 R .. bit3 A); 0: the format takes no colour clear
 * (x32 and the float formats are no-ops on RPCS3). */
static u8 eng_renorm8(u32 v, u32 base) { return (u8)(((v & base) * 255u) / base); }

static u32 eng_clear_color_remap(u32 color_format, u32 guest_mask, u32 c, float rgba[4])
{
    u32 r = (c >> 16) & 0xFFu, gr = (c >> 8) & 0xFFu, b = c & 0xFFu, a = (c >> 24) & 0xFFu;
    u32 m = ((guest_mask & RSX_CLEAR_COLOR_R) ? 1u : 0u) | ((guest_mask & RSX_CLEAR_COLOR_G) ? 2u : 0u) |
            ((guest_mask & RSX_CLEAR_COLOR_B) ? 4u : 0u) | ((guest_mask & RSX_CLEAR_COLOR_A) ? 8u : 0u);
    switch (color_format & 0x1Fu) {
    case 0x0Bu: case 0x0Cu: case 0x0Du:            /* w16z16y16x16, w32z32y32x32, x32 */
        return 0;
    case 0x09u: m &= 4u; break;                    /* b8 */
    case 0x0Au: m &= 6u; break;                    /* g8b8 */
    case 0x03u: {                                  /* r5g6b5 */
        const u32 raw = (gr << 8) | b;
        b = eng_renorm8(raw, 0x1Fu); gr = eng_renorm8(raw >> 5, 0x3Fu); r = eng_renorm8(raw >> 11, 0x1Fu);
        break;
    }
    case 0x01u: case 0x02u: {                      /* x1r5g5b5 _z / _o */
        const u32 raw = (gr << 8) | b;
        b = eng_renorm8(raw, 0x1Fu); gr = eng_renorm8(raw >> 5, 0x1Fu); r = eng_renorm8(raw >> 10, 0x1Fu);
        a = (color_format & 0x1Fu) == 0x02u ? 255u : 0u;
        break;
    }
    case 0x0Eu: case 0x0Fu: case 0x10u: {          /* x8b8g8r8 _z / _o, a8b8g8r8 */
        const u32 t = r; r = b; b = t;
        m = (m & 0xAu) | ((m & 1u) << 2) | ((m & 4u) >> 2);
        break;
    }
    default: break;
    }
    rgba[0] = (float)r / 255.0f; rgba[1] = (float)gr / 255.0f;
    rgba[2] = (float)b / 255.0f; rgba[3] = (float)a / 255.0f;
    return m;
}

static void sink_clear(void* user, const rsx_dispatch* r, u32 mask)
{
    (void)user; (void)r;
    if (!g.ready) return;
    /* RPCS3 core_renderer::clear (RSXCoreRenderer.cpp:659): a zero
     * STENCIL_MASK drops the stencil bit, and a mask with no RGBA, depth or
     * stencil bit left is no clear at all. */
    const u32 guest_mask = mask;
    if (!(rsx_dsp_reg(&g.rsx, M_STENCIL_MASK) & 0xFFu)) mask &= ~(u32)RSX_CLEAR_STENCIL;
    if (!(mask & 0xF3u)) return;
    if (!eng_legacy_surfaces() && eng_scissor_invalid(0)) return;
    const int want_color = (mask & 0xF0u) != 0;
    const int want_zs = (mask & (RSX_CLEAR_DEPTH | RSX_CLEAR_STENCIL)) != 0;
    /* bind_framebuffer for context_clear_color / _depth / _all: an unused
     * side is not bound, a pitch or alias failure drops a buffer, and with
     * neither left (rtt_config_valid false) the clear is skipped. */
    eng_fb_layout fbl;
    u32 targets[RSX_BE_MAX_COLOR_TARGETS];
    u32 bound_z = ENG_INVALID;
    /* A depth-only clear, or a colour buffer the layout drops, binds scratch
     * attachments instead of creating surfaces at those addresses. */
    const u32 n_targets = eng_fb_bind((want_color ? ENG_FB_CLEAR_COLOR : 0u) |
                                      (want_zs ? ENG_FB_CLEAR_DEPTH : 0u),
                                      &fbl, targets, &bound_z);
    if (!fbl.any_color && !fbl.zeta_live) return;
    if (!n_targets) return;
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);

    /* RPCS3 VKGSRender::clear_surface clears the scissor clipped to the
     * surface, not the whole target (split-screen and partial passes). In
     * the sample grid, as the draw scissor is; an unwritten scissor reads 0
     * and leaves the whole target. */
    const u32 tw = g.surfaces[targets[0]].w, th = g.surfaces[targets[0]].h;
    u32 cx = 0, cy = 0, cw = tw, ch = th;
    {
        const u32 h = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
        const u32 v = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
        const u32 ax = sf.aa_sx ? sf.aa_sx : 1u, ay = sf.aa_sy ? sf.aa_sy : 1u;
        const u32 gx = (h & 0xFFFFu) * ax, gw = (h >> 16) * ax;
        const u32 gy = (v & 0xFFFFu) * ay, gh = (v >> 16) * ay;
        if (gw > 0 && gh > 0) {
            u32 right = tw, bottom = th;
            cx = gx < tw ? gx : tw;
            cy = gy < th ? gy : th;
            if (gx + gw < right)  right = gx + gw;
            if (gy + gh < bottom) bottom = gy + gh;
            cw = right > cx ? right - cx : 0;
            ch = bottom > cy ? bottom - cy : 0;
        }
    }
    const int rect_full = cx == 0 && cy == 0 && cw >= tw && ch >= th;
    if (!cw || !ch) return;

    float rgba[4] = { 0, 0, 0, 0 };
    u32 cmask = 0;
    /* Colour goes only to the buffers the layout kept. */
    u32 ctargets[RSX_BE_MAX_COLOR_TARGETS], n_ctargets = 0;
    if (want_color && fbl.any_color) {
        u32 idx[4];
        const u32 n_idx = eng_rtt_indexes(sf.color_target, idx);
        for (u32 i = 0; i < n_targets && i < n_idx; i++)
            if (fbl.color_live[idx[i]]) ctargets[n_ctargets++] = targets[i];
        if (n_ctargets)
            cmask = eng_clear_color_remap(sf.color_format, mask, rsx_dsp_clear_color(&g.rsx), rgba);
    }

    u32 zslot = ENG_INVALID, flags = 0;
    const u32 smask = rsx_dsp_reg(&g.rsx, M_STENCIL_MASK) & 0xFFu;
    float depth = 1.0f;
    u8 stencil = 0;
    if (want_zs && fbl.zeta_live)
        zslot = bound_z;
    if (zslot != ENG_INVALID) {
        const int z16 = sf.depth_format == 1u;
        if (mask & RSX_CLEAR_DEPTH) flags |= RSX_BE_CLEAR_DEPTH;
        /* Stencil only on a Z24S8 zeta; a zero STENCIL_MASK already dropped
         * the bit above. */
        if ((mask & RSX_CLEAR_STENCIL) && !z16) flags |= RSX_BE_CLEAR_STENCIL;
        /* RPCS3 decode_clear_state / get_depth_clear_value: ZSTENCIL_CLEAR_VALUE
         * is laid out as the zeta is -- depth24 << 8 | stencil8 for Z24S8, a
         * bare depth16 for Z16 -- and CONTROL0 bit 12 makes it float (f24 is
         * the f32 bits >> 7, z16 float e4m12). A value of 0 is depth 0.0;
         * the never-written register reads its reset 0xFFFFFF00 (1.0 / 0). */
        const u32 zs = rsx_dsp_reg(&g.rsx, M_ZSTENCIL_CLEAR);
        const int zfloat = (rsx_dsp_reg(&g.rsx, 0x03B0) >> 12) & 1u;
        depth = zfloat ? (z16 ? eng_e4m12_to_f32(zs & 0xFFFFu)
                              : eng_f24_to_f32(zs >> 8))
              : z16 ? (float)(zs & 0xFFFFu) / 65535.0f
                    : (float)(zs >> 8) / 16777215.0f;
        stencil = z16 ? 0 : (u8)(zs & 0xFFu);
    }
    if (pd_capturing()) {
        u32 pz = zslot;
        for (u32 i = 0; pz == ENG_INVALID && i < g.n_zdepths; i++)
            if (g.zdepths[i].location == sf.zeta_location && g.zdepths[i].offset == sf.zeta_offset)
                pz = i;
        pd_note(targets, n_targets, pz, NULL,
                (fbl.zeta_live ? 1u : 0u) | (fbl.any_color ? 2u : 0u) |
                (flags << 4) | ((cmask & 0xFu) << 8), guest_mask);
    }
    if (!cmask && !flags) return;

    /* A new zeta starts at RPCS3's clear_memory values; a clear that leaves
     * part of it (an aspect, a rectangle or stencil bits) must not expose
     * whatever the host image was created with. */
    if (flags && !eng_legacy_surfaces() && !g.zdepths[zslot].cleared) {
        const u32 both = sf.depth_format == 1u ? RSX_BE_CLEAR_DEPTH
                                               : (RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL);
        const int full = rect_full && (flags & both) == both &&
                         (!(flags & RSX_BE_CLEAR_STENCIL) || smask == 0xFFu);
        if (!full) eng_zdepth_init(zslot);
    }

    const int partial = (cmask && cmask != 0xFu) ||
                        ((flags & RSX_BE_CLEAR_STENCIL) && smask != 0xFFu);
    if (g.be->clear_rect && !eng_legacy_surfaces() && (!rect_full || partial)) {
        /* The backend needs a colour attachment to frame the pass: a
         * depth-only clear passes the bound set with no channel to clear. */
        const u32* set = n_ctargets ? ctargets : targets;
        const u32 n_set = n_ctargets ? n_ctargets : n_targets;
        u32 handles[RSX_BE_MAX_COLOR_TARGETS];
        for (u32 i = 0; i < n_set; i++) handles[i] = g.surfaces[set[i]].handle;
        g.be->clear_rect(g.be->user, handles, n_set,
                         zslot != ENG_INVALID ? g.zdepths[zslot].handle : 0,
                         rgba, cmask, flags, depth, stencil, smask, cx, cy, cw, ch);
    } else {
        /* Every target the set names, not only A: a deferred pass clears its
         * whole G-buffer in one CLEAR_SURFACE, and B, C and D would otherwise
         * keep whatever the last frame left in them. */
        if (cmask)
            for (u32 i = 0; i < n_ctargets; i++)
                g.be->clear_color(g.be->user, g.surfaces[ctargets[i]].handle, rgba);
        if (flags)
            g.be->clear_depth_stencil(g.be->user, g.zdepths[zslot].handle, flags, depth, stencil);
    }
    if (cmask) eng_surfaces_wrote(ctargets, n_ctargets);
    if (flags) {
        g.zdepths[zslot].cleared = 1;
        if (flags & RSX_BE_CLEAR_DEPTH) {
            /* A clear invalidates the older published depth image; the next
             * texture consumer resolves the newly written pass exactly once. */
            g.zdepths[zslot].had_write = 0;
            g.zdepths[zslot].snapshot_valid = 0;
        }
        /* RPCS3 VKGSRender::clear: a depth or stencil aspect cleared is an
         * on_write of the zeta (age, tag). */
        eng_zdepth_wrote(zslot, &sf);
    }
}

/* Resolve a flip's buffer id to a REGISTERED surface rather than whatever is
 * currently bound: the live target at that instant is often an offscreen
 * shadow or post-process surface, and copying it presents black despite the
 * scene's draws having executed (rsx_live_draw.c:7557-7570). */
/* RPCS3's VKGSRender::get_present_source: the display buffer's rectangle
 * comes from the surface store when a surface starts at its address and
 * covers it (cropped to the buffer), or one inset by a symmetric border
 * (letterboxing); otherwise the image is read from guest memory. It never
 * falls back to whatever target is bound -- that put shadow maps and
 * half-built targets on screen as black or zoomed frames. */
static u32 eng_present_surface(u32 buffer_id, u32* sx, u32* sy, u32* sw, u32* sh)
{
    *sx = *sy = 0;
    if (buffer_id >= 8 || !g.display_buffers[buffer_id].valid) {
        const u32 cur = eng_current_surface();
        if (cur != ENG_INVALID) { *sw = g.surfaces[cur].w; *sh = g.surfaces[cur].h; }
        return cur;
    }
    const eng_display_buffer* d = &g.display_buffers[buffer_id];
    const u32 bw = d->width ? d->width : g.width;
    const u32 bh = d->height ? d->height : g.height;
    const u32 pitch = d->pitch ? d->pitch : bw * 4u;

    int from_merged = 0;
    if (eng_present_merged()) {
        /* surface_store::get_merged_texture_memory_region over the buffer
         * (transfer read, 4 bytes per pixel): written colour surfaces of the
         * buffer's pitch that overlap its rows, newest last; RPCS3 then takes
         * only that newest one -- one at the buffer's address fits or is
         * cropped, one inside it must be a symmetric inset, and anything
         * else (a newer surface below the address, or a misfit) reads the
         * buffer from guest memory. */
        const u32 req_wb = bw * 4u;
        u32 best = ENG_INVALID, best_exact = 0, best_clipped = 0, best_area = 0;
        if (req_wb <= pitch) {
            const u64 t0 = d->offset, t1 = (u64)d->offset + (u64)pitch * bh - (pitch - req_wb);
            for (u32 k = 0; k < g.n_surfaces; k++) {
                const eng_surface* s = &g.surfaces[k];
                if (!s->handle || !s->color_serial || s->location != d->location) continue;
                if (s->h != 1u && bh != 1u && s->pitch != pitch) continue;
                const u32 sbpp = eng_surface_bpp(s->fmt);
                const u32 s_wb = s->w * sbpp, s_h = s->h;
                const u64 r0 = s->offset, r1 = (u64)s->offset + (u64)s->pitch * (s_h / (s->aa_sy ? s->aa_sy : 1u));
                if (r1 <= t0 || r0 >= t1) continue;
                u32 wb, hh;
                if (r0 >= t0) {
                    const u32 off = (u32)(r0 - t0), dy = off / pitch, dx = off % pitch;
                    if (dx >= req_wb || dy >= bh) continue;
                    wb = s_wb < req_wb - dx ? s_wb : req_wb - dx;
                    hh = s_h < bh - dy ? s_h : bh - dy;
                } else {
                    const u32 off = (u32)(t0 - r0), oy = off / s->pitch, ox = off % s->pitch;
                    if (ox >= s_wb || oy >= s_h) continue;
                    wb = req_wb < s_wb - ox ? req_wb : s_wb - ox;
                    hh = bh < s_h - oy ? bh : s_h - oy;
                }
                if (wb < 4u) continue;
                wb -= wb % 4u;
                const u32 exact = r0 == t0, clipped = wb < req_wb || hh < bh;
                const u32 area = (wb / 4u) * hh;
                if (best != ENG_INVALID) {
                    const u64 a = g.surfaces[best].color_serial, b = s->color_serial;
                    int newer;
                    if (a != b) newer = b > a;
                    else if (best_exact && !best_clipped) newer = 0;
                    else if (exact && !clipped) newer = 1;
                    else if (best_clipped != clipped) newer = best_clipped;
                    else newer = area > best_area;
                    if (!newer) continue;
                }
                best = k; best_exact = exact; best_clipped = clipped; best_area = area;
            }
        }
        if (best != ENG_INVALID) {
            const eng_surface* s = &g.surfaces[best];
            if (s->offset == d->offset) {
                /* Fit or crop, compared in samples as RPCS3 does; the crop
                 * keeps bw pixels of an antialiased surface (sbw samples):
                 * cropping to bw samples was the "zoom to the left". */
                if (s->w >= bw && s->h >= bh) {
                    const u32 sbw = bw * s->aa_sx, sbh = bh * s->aa_sy;
                    *sw = sbw < s->w ? sbw : s->w;
                    *sh = sbh < s->h ? sbh : s->h;
                    return best;
                }
            } else if (s->offset > d->offset) {
                const u32 inset = s->offset - d->offset;
                const u32 iy = inset / pitch, ix = (inset % pitch) / 4u;
                if (s->w + 2u * ix == bw && s->h + 2u * iy == bh) {
                    *sw = s->w; *sh = s->h;
                    return best;
                }
            }
        }
        from_merged = 1;
        goto read_from_cell;
    }

    u32 i = eng_surface_newest(d->location, d->offset, NULL, 0);
    if (i != ENG_INVALID && g.surfaces[i].color_serial) {
        /* RPCS3 requires the surface to cover the buffer; a smaller rendered
         * one is shown cropped rather than replaced (replacing would drop the
         * GPU-side image the next frame renders on top of). */
        /* Surface sizes are in samples: an antialiased scene at the buffer
         * (2x1 for R2's 0x880000) covers bw*aa_sx of them. Cropping to bw
         * showed only its left half, stretched -- the "zoom to the left". */
        const u32 sbw = bw * g.surfaces[i].aa_sx, sbh = bh * g.surfaces[i].aa_sy;
        *sw = sbw < g.surfaces[i].w ? sbw : g.surfaces[i].w;
        *sh = sbh < g.surfaces[i].h ? sbh : g.surfaces[i].h;
        return i;
    }
    /* Inset: the newest written surface starting inside the buffer whose
     * borders are equal on both sides. */
    u32 best = ENG_INVALID;
    for (u32 k = 0; k < g.n_surfaces; k++) {
        const eng_surface* s = &g.surfaces[k];
        if (!s->handle || !s->color_serial || s->location != d->location ||
            s->offset <= d->offset || s->offset >= d->offset + pitch * bh) continue;
        const u32 inset = s->offset - d->offset;
        const u32 iy = inset / pitch, ix = (inset % pitch) / 4u;
        if (s->w + 2u * ix != bw || s->h + 2u * iy != bh) continue;
        if (best != ENG_INVALID && g.surfaces[best].color_serial >= s->color_serial) continue;
        best = k;
    }
    if (best != ENG_INVALID) {
        *sw = g.surfaces[best].w; *sh = g.surfaces[best].h;
        return best;
    }
read_from_cell:
    /* Read from cell: an A8R8G8B8 surface at the buffer, refreshed from guest
     * memory (rendered offscreen or in software and copied there). Any GPU
     * surface whose data the CPU copy came from is written back first. */
    rsx_draw_engine_range_read(d->location, d->offset, pitch * bh);
    if (from_merged) {
        /* RPCS3 uploads the buffer's bytes afresh into a texture of its own
         * on every read from cell; a guest surface at the address (one that
         * did not fit) is left alone. */
        i = eng_surface_get(ENG_LOC_PRESENT, buffer_id, bw, bh, RSX_BE_FMT_R8G8B8A8, 0, 1, 1);
        if (i == ENG_INVALID) return ENG_INVALID;
        eng_surface_upload_rows(i, d->location, d->offset, pitch, bw, bh);
        *sw = bw < g.surfaces[i].w ? bw : g.surfaces[i].w;
        *sh = bh < g.surfaces[i].h ? bh : g.surfaces[i].h;
        return i;
    }
    i = eng_surface_get(d->location, d->offset, bw, bh, RSX_BE_FMT_R8G8B8A8, pitch, 1, 1);
    if (i == ENG_INVALID) return ENG_INVALID;
    g.surfaces[i].pitch = pitch;
    if (!g.surfaces[i].color_serial)
        rsx_draw_engine_guest_wrote(d->location, d->offset, pitch, bw, bh);
    *sw = bw < g.surfaces[i].w ? bw : g.surfaces[i].w;
    *sh = bh < g.surfaces[i].h ? bh : g.surfaces[i].h;
    return i;
}

static void eng_present_impl(u32 buffer_id)
{
    if (!g.ready) return;
    u32 sx = 0, sy = 0, sw = 0, sh = 0;
    const u32 target = eng_present_surface(buffer_id, &sx, &sy, &sw, &sh);
    if (s_aa_hooks && s_aa_hooks->on_flip) s_aa_hooks->on_flip(s_aa_hooks->user);
    if (target == ENG_INVALID) return;
    static int frame_dump = -1;
    if (frame_dump < 0) frame_dump = getenv("PS3RECOMP_METAL_FRAME_DUMP") != NULL;
    if (frame_dump && g.frames % 120 == 0) {
        u32 current = eng_current_surface();
        fprintf(stderr, "[rsx capture] frame=%u buffer=%u selected=%u current=%u draws=%u surfaces=%u\n",
                g.frames, buffer_id, target, current, g.guest_draws, g.n_surfaces);
        for (u32 i = 0; i < g.n_surfaces; ++i)
            fprintf(stderr, "[rsx capture] surface %u loc=%u offset=%08X size=%ux%u handle=%u\n",
                    i, g.surfaces[i].location, g.surfaces[i].offset,
                    g.surfaces[i].w, g.surfaces[i].h, g.surfaces[i].handle);
    }
    pd_present(target);
    g.last_present_surface = target;
    if (g.be->present_region && sw && sh) {
        /* avconf: 16:9 unless RSX_PRESENT_STRETCH=1. */
        static int stretch = -1;
        if (stretch < 0) { const char* e = getenv("RSX_PRESENT_STRETCH"); stretch = e && *e == '1'; }
        u32 aw = stretch ? 0u : 16u, ah = stretch ? 0u : 9u;
        /* RPCS3 avconf::aspect_convert_region (rsx_utils.cpp:178-196): the
         * presented rectangle (at most the buffer, VKPresent.cpp:553/602) is
         * fitted into the video frame, and that 16:9 frame into the window;
         * an inset (1280x704 in 1280x720) is letterboxed, not stretched. The
         * frame is the display buffer (R2: 1280x720 = its video mode). The
         * merged aspect is exact for a 16:9 window. RSX_CFG_LEGACY=1: 16:9. */
        if (!stretch && !eng_cfg_legacy() && buffer_id < 8 && g.display_buffers[buffer_id].valid) {
            const eng_display_buffer* d = &g.display_buffers[buffer_id];
            const u32 vw = d->width ? d->width : g.width, vh = d->height ? d->height : g.height;
            const u32 rw = sw < vw ? sw : vw, rh = sh < vh ? sh : vh;
            if (vw && vh && rw && rh) {
                const double ratio = ((double)rw / rh) / ((double)vw / vh);
                u32 fw = vw, fh = vh;
                if (ratio > 1.0) fh = (u32)(vh / ratio);
                else if (ratio < 1.0) fw = (u32)(vw * ratio);
                if (fw && fh) { aw = fw * 16u * vh; ah = fh * 9u * vw; }
            }
        }
        g.be->present_region(g.be->user, g.surfaces[target].handle, sx, sy, sw, sh, aw, ah);
    } else {
        g.be->present(g.be->user, g.surfaces[target].handle);
    }
    g.frames++;
    g.last_guest_draws = g.guest_draws;
    g.guest_draws = 0;
}

static void eng_present(u32 buffer_id)
{
    const u64 t0 = eng_prof_t0();
    eng_present_impl(buffer_id);
    eng_prof_add(6, t0);
}

static void sink_flip(void* user, const rsx_dispatch* r, u32 arg)
{
    (void)user; (void)r;
    g.last_flip_buffer = arg & 7u;
    eng_present(g.last_flip_buffer);
}

/* ---- public API ---------------------------------------------------------- */

void rsx_draw_engine_set_backend(const rsx_draw_backend* backend)
{
    g.be = backend;
}

void rsx_draw_engine_set_last_provoking_vertex(int on)
{
    s_eng_last_provoking = on ? 1 : 0;
}

void rsx_draw_engine_set_border_color_remap(int on)
{
    s_eng_border_remap = on ? 1 : 0;
}

void rsx_draw_engine_set_default(int on)
{
    g.default_on = on;
}

int rsx_draw_engine_enabled(void)
{
    static int cached = -1;
    static int env_seen = 0;
    static int env_on = 0;
    if (!env_seen) {
        const char* e = getenv("PS3RECOMP_RSX_ENGINE");
        env_seen = 1;
        if (e && *e) env_on = (strcmp(e, "dispatch") == 0) ? 1 : -1;
    }
    /* The environment is authoritative in both directions; without it the
     * backend's own default decides. Cached after the first backend has
     * registered, so the FIFO walker pays one compare. */
    const int on = env_on > 0 ? 1 : (env_on < 0 ? 0 : g.default_on);
    if (!g.be) return 0;
    if (cached < 0) cached = on;
    return cached;
}

int rsx_draw_engine_init(u32 width, u32 height)
{
    ENG_SCOPE();
    if (!g.be) return -1;
    g.width  = width  ? width  : 1280;
    g.height = height ? height : 720;
    { const char* e = getenv("PS3RECOMP_ENG_TEXHASH_FULL");
      s_texhash_full_always = e && *e && strcmp(e, "0") != 0; }
    { const char* e = getenv("RSX_TEXCACHE_LEGACY");
      s_texcache_legacy = e && *e && strcmp(e, "0") != 0; }
    { const char* e = getenv("RSX_ENG_TILED_MEMORY");
      s_eng_tiled_mem = e && e[0] == '1'; }
    { const char* e = getenv("RSX_ENG_TILED_ROW_PITCH");
      s_eng_tiled_row_pitch = !(e && *e == '0'); }
    s_eng_sudo = -1;           /* RSX_ENG_SUDO, re-read */
#ifdef _WIN32
    s_wcb_self_flush = -1;     /* RSX_ENG_WCB_SELF_FLUSH, re-read */
#endif
    { const char* e = getenv("RSX_TEXCACHE_PROTECT");
      s_tc_protect = !(e && *e == '0'); }
    { const char* e = getenv("RSX_ENG_FLAT_SHADE");
      s_eng_flat_shade = !(e && *e == '0'); }
    { const char* e = getenv("RSX_ENG_POINT_SPRITE");
      s_eng_point_sprite = !(e && *e == '0'); }
    g.vp_cb = (u8*)calloc(1, ENG_VP_CB_BYTES);
    if (!g.vp_cb) return -1;
    if (g.be->init && g.be->init(g.be->user, g.width, g.height) != 0) {
        free(g.vp_cb); g.vp_cb = NULL;
        return -1;
    }
    rsx_dispatch_sink sink;
    memset(&sink, 0, sizeof sink);
    sink.clear            = sink_clear;
    sink.begin            = sink_begin;
    sink.end              = sink_end;
    sink.draw_arrays      = sink_draw_arrays;
    sink.draw_index_array = sink_draw_index;
    sink.inline_array     = sink_inline_array;
    sink.immediate_vertex = sink_immediate_vertex;
    sink.array_elements   = sink_array_elements;
    sink.flip             = sink_flip;
    sink.reg_changed      = sink_reg_changed;
    eng_fb_cache_reset();
    rsx_dispatch_init(&g.rsx, &sink);
    eng_gen_bump_all();
    s_pm.valid = 0;
    s_vpm.valid = 0;
    memset(s_tex_hint, 0, sizeof s_tex_hint);
    g.ready = 1;
    /* Display buffers the title registered before the backend came up. */
    cellGcm_replay_display_buffers_to_engine();
    fprintf(stderr, "[rsx engine] register-file draw engine up (%ux%u)\n",
            g.width, g.height);
    return 0;
}

void rsx_draw_engine_shutdown(void)
{
    ENG_SCOPE();
    if (!g.be) return;
    if (g.ready && g.be->submit_and_wait)
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_SHUTDOWN);
    for (u32 i = 0; i < g.n_textures; i++)
        if (g.textures[i].handle) g.be->texture_release(g.be->user, g.textures[i].handle);
    for (u32 i = 0; i < g.n_pipelines; i++)
        if (g.pipelines[i].handle) g.be->pipeline_release(g.be->user, g.pipelines[i].handle);
    eng_pcache_close();
    for (u32 i = 0; i < g.n_surfaces; i++) {
        if (g.surfaces[i].feedback) g.be->texture_release(g.be->user, g.surfaces[i].feedback);
        if (g.surfaces[i].region) g.be->texture_release(g.be->user, g.surfaces[i].region);
        if (g.surfaces[i].reint) g.be->texture_release(g.be->user, g.surfaces[i].reint);
        if (g.surfaces[i].handle) g.be->color_target_release(g.be->user, g.surfaces[i].handle);
    }
    for (u32 i = 0; i < g.n_zdepths; i++) {
        if (g.zdepths[i].reint) g.be->texture_release(g.be->user, g.zdepths[i].reint);
        if (g.zdepths[i].handle) g.be->depth_target_release(g.be->user, g.zdepths[i].handle);
    }
    eng_tc_sub_release_all();
    eng_tc_unwrap_release_all();
    if (s_pb_copy) { g.be->texture_release(g.be->user, s_pb_copy); s_pb_copy = 0; }
    if (g.ready && g.be->shutdown) g.be->shutdown(g.be->user);

    rsx_draw_engine_set_guest_memory(NULL, NULL);
    eng_wcb_forget_all();
    eng_tc_unprotect_all();
    free(dc.refs); free(dc.cuts); free(dc.verts); free(dc.immediate);
    rsx_vertex_remap_destroy(&dc.ref_remap);
    memset(&dc, 0, sizeof dc);
    free(g.tex_staging); free(g.vp_cb); free(g.fp_cb); free(g.indices);
    const rsx_draw_backend* be = g.be;
    memset(&g, 0, sizeof g);
    g.be = be;
}

static void rsx_draw_engine_method_impl(u32 method, u32 arg)
{
    if (!g.ready) return;
    /* The subchannel is a binding slot, not an engine selector, and a title
     * whose SPU-built command lists bind NV4097 elsewhere would otherwise
     * store its state in the wrong register bank.  Standard NV4097 methods
     * (< 0x2000) strip the subchannel with & 0x1FFC.  Driver methods
     * (0xE9xx, 0xEBxx) encode the subchannel bits as part of the address;
     * preserve them with the wider mask. */
    u32 m = (method >= 0xE000u) ? (method & 0xFFFCu) : (method & 0x1FFCu);
    eng_method_gens(m);
    rsx_dispatch_method(&g.rsx, m, arg);
}

#ifdef _WIN32
/* Host thread running the engine (the F11 profiler samples it). */
static volatile unsigned long s_eng_tid;
unsigned long rsx_draw_engine_host_tid(void) { return s_eng_tid; }
#endif

void rsx_draw_engine_method(u32 method, u32 arg)
{
    ENG_SCOPE();
#ifdef _WIN32
    if (!s_eng_tid) s_eng_tid = GetCurrentThreadId();
#endif
    const u64 t0 = eng_prof_t0();
    rsx_draw_engine_method_impl(method, arg);
    eng_prof_add(7, t0);
}

void rsx_draw_engine_set_display_buffer(u32 buffer_id, u32 location, u32 offset,
                                        u32 pitch, u32 width, u32 height)
{
    if (buffer_id >= 8) return;
    eng_display_buffer* d = &g.display_buffers[buffer_id];
    d->location = location;
    d->offset = offset;
    d->pitch = pitch;
    d->width = width;
    d->height = height;
    d->valid = width && height;
}

void rsx_draw_engine_flush(void)
{
    ENG_SCOPE();
    if (g.ready && g.be->submit_and_wait)
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

void rsx_draw_engine_present(void)
{
    ENG_SCOPE();
    /* A host that drives the flip itself gets whichever buffer the guest's
     * own last flip named, and display buffer 0 before there has been one:
     * a runner that both consumes 0xE944 and calls here would otherwise
     * re-present a double-buffered title's OTHER scanout. Presenting twice is
     * harmless either way -- the engine never clears the surface, so a second
     * present just blits the same image again. */
    eng_present(g.last_flip_buffer);
}

void rsx_draw_engine_present_buffer(u32 buffer_id)
{
    ENG_SCOPE();
    g.last_flip_buffer = buffer_id & 7u;
    eng_present(g.last_flip_buffer);
}

void rsx_draw_engine_note_flip(u32 buffer_id)
{
    g.last_flip_buffer = buffer_id & 7u;
}

u32 rsx_draw_engine_guest_draws(void)
{
    return g.last_guest_draws;
}

u32 rsx_draw_engine_readback_center(void)
{
    ENG_SCOPE();
    if (!g.ready || !g.be->readback) return 0;
    const u32 slot = g.last_present_surface;
    if (slot >= g.n_surfaces) return 0;
    const eng_surface* s = &g.surfaces[slot];
    if (!s->handle || s->fmt != RSX_BE_FMT_R8G8B8A8 || !s->w || !s->h) return 0;
    u8 px[4] = { 0, 0, 0, 0 };
    g.be->readback(g.be->user, s->handle, s->w / 2u, s->h / 2u, 1, 1, px, 4);
    /* Decoded rows are R,G,B,A; the guest's own clear colour is A8R8G8B8, and
     * a host comparing the two wants them in the same order. */
    return ((u32)px[3] << 24) | ((u32)px[0] << 16) |
           ((u32)px[1] << 8)  |  (u32)px[2];
}

/* ---- host occlusion queries (ZCULL ZPASS reports, cellGcmSys) ----------- */

int rsx_draw_engine_occlusion_begin(u32 id)
{
    ENG_SCOPE();
    if (!g.ready || !g.be || !g.be->occlusion_begin || !g.be->occlusion_end ||
        !g.be->occlusion_result || !g.be->occlusion_discard) return 0;
    const int ok = g.be->occlusion_begin(g.be->user, id);
    if (ok) s_eng_occl_active++;
    return ok;
}

void rsx_draw_engine_occlusion_end(u32 id)
{
    ENG_SCOPE();
    if (s_eng_occl_active > 0) s_eng_occl_active--;
    if (g.ready && g.be && g.be->occlusion_end) g.be->occlusion_end(g.be->user, id);
}

int rsx_draw_engine_occlusion_result(u32 id, int wait, u64* out)
{
    ENG_SCOPE();
    if (!g.ready || !g.be || !g.be->occlusion_result) return 0;
    return g.be->occlusion_result(g.be->user, id, wait, out);
}

void rsx_draw_engine_occlusion_discard(u32 id)
{
    ENG_SCOPE();
    if (g.ready && g.be && g.be->occlusion_discard) g.be->occlusion_discard(g.be->user, id);
}

void rsx_draw_engine_occlusion_kick(u32 id)
{
    ENG_SCOPE();
    if (g.ready && g.be && g.be->occlusion_result) (void)g.be->occlusion_result(g.be->user, id, 2, NULL);
}

void rsx_draw_engine_set_aa_hooks(const rsx_aa_hooks* hooks)
{
    s_aa_hooks = hooks;
}

int rsx_draw_engine_scene_aa(const rsx_be_scene_aa* req)
{
    ENG_SCOPE();
    if (!req || !g.ready || !g.be || !g.be->scene_aa) return 0;
    return g.be->scene_aa(g.be->user, req);
}
