/*
 * ps3recomp - the platform-neutral halves of the RSX draw engine
 *
 * Everything here runs with no GPU and no host graphics API: the engine is
 * driven through rsx_draw_engine_method(), exactly as the FIFO walker drives
 * it, against a stub backend that records what it was asked for. That is the
 * only way to check the four pieces whose bugs are silent -- a surface that is
 * reallocated when it should not be, a texture unit that samples guest memory
 * instead of the render target at the same address, a texture that never
 * updates because its content hash is only taken once, and a pipeline that is
 * recompiled every time a constant changes.
 *
 * Builds and runs on Linux as well as macOS; nothing in it is Apple-specific.
 */
#include "rsx_draw_engine.h"
#include "rsx_primitives.h"   /* RSX_PRIMITIVE_* */
#include "rsx_test_programs.h"
#include "rsx_vertex_compact.h"   /* rsx_vertex_legacy */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <malloc.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
static int setenv(const char* k, const char* v, int o) { (void)o; return _putenv_s(k, v); }
static int unsetenv(const char* k) { return _putenv_s(k, ""); }
#endif

static int g_failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            printf("[PASS] " __VA_ARGS__); printf("\n");                      \
        } else {                                                              \
            printf("[FAIL] " __VA_ARGS__);                                    \
            printf("   (%s:%d)\n", __FILE__, __LINE__);                       \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

/* ---- the guest memory the engine reads ---------------------------------- */
/*
 * Two number spaces, as on the hardware: an RSX offset means one thing in the
 * IO-mapped main-memory context and another in local video memory. The test's
 * map puts main at the bottom of the buffer and local above it, which is what
 * lets the surface-keying check bind two different objects at one offset.
 */
#define GUEST_BYTES     (16u << 20)
#define GUEST_LOCAL_EA  (8u << 20)

u8* vm_base;
u32 ppu_vm_size;

u32 cellGcmResolveLocated(int local, u32 offset)
{
    return local ? GUEST_LOCAL_EA + offset : offset;
}

u32 cellGcmResolveIO(u32 offset)
{
    return offset < GUEST_LOCAL_EA ? offset : 0;
}

/* No cellGcmSys here: nothing was registered before the engine came up. */
void cellGcm_replay_display_buffers_to_engine(void) {}

/* ---- the stub backend ---------------------------------------------------- */

#define STUB_MAX_OBJECTS 8192
/* Colour clears kept by surface, so a test can say which targets were hit. */
#define STUB_MAX_CLEARED 8

typedef struct {
    u32 next_handle;
    u8  alive[STUB_MAX_OBJECTS];

    int n_texture_create, n_texture_release, n_texture_upload;
    int n_color_create, n_color_release, n_color_seeded;
    int n_depth_create, n_depth_snapshot;
    int n_pipeline_create;
    rsx_be_render_state pipeline_rs;   /* the last pipeline's render state */
    int n_draw, n_clear_color, n_clear_ds, n_present;
    u32 cleared[STUB_MAX_CLEARED];

    /* the last draw's bindings */
    u32 bound_tex[RSX_BE_MAX_TEXTURES];
    float bound_max_lod0;
    u32 bound_mask;
    u32 bound_rt[RSX_BE_MAX_COLOR_TARGETS];
    u32 bound_rt_count;
    u32 pipeline_rt_count;
    u32 bound_surface, bound_depth, bound_pipeline;
    u32 draw_vertices, draw_indices, draw_stride;
    float captured_vertices[16][64];
    u32 captured_indices[24];
    float draw_first_vertex[4];   /* ATTR0 of the first vertex handed over */
    rsx_topology draw_topology;
    u32 scissor[4];
    u32 stencil_ref;
    float depth_bounds[2];
    int n_depth_bounds;
    u32 last_created_texture, last_created_color;
    u32 last_released;
    float last_clear_depth;
    int n_depth_readback;
    u32 color_w, color_h;         /* size of the last colour surface created */
    float vp_xf[8];               /* viewport epilogue after the VS constants */
    u32 vp_c10[4];                /* VS constant slot 10 as bound */
    float last_clear_rgba[4];     /* the last clear_color's colour */
    /* the last clear_rect (g_stub_backend_rect only) */
    int n_clear_rect;
    u32 cr_rect[4], cr_mask, cr_flags, cr_smask, cr_stencil, cr_n_rts;
    float cr_rgba[4];
} StubState;

static StubState stub;
static char g_stub_last_ps[65536];   /* the last pipeline's pixel shader */

static u32 stub_handle(void)
{
    if (stub.next_handle + 1 >= STUB_MAX_OBJECTS) return 0;
    const u32 h = ++stub.next_handle;
    stub.alive[h] = 1;
    return h;
}

static int stub_init(void* u, u32 w, u32 h) { (void)u; (void)w; (void)h; return 0; }
static void stub_shutdown(void* u) { (void)u; }
static void stub_submit(void* u, u32 reason) { (void)u; (void)reason; }

static u32 stub_texture_create(void* u, rsx_be_format f, u32 w, u32 h, u32 mips,
                               u32 faces, u32 remap, u32 rsx_fmt)
{
    (void)u; (void)f; (void)w; (void)h; (void)mips; (void)faces;
    (void)remap; (void)rsx_fmt;
    stub.n_texture_create++;
    stub.last_created_texture = stub_handle();
    return stub.last_created_texture;
}

static void stub_texture_upload(void* u, u32 t, u32 face, u32 mip, u32 w, u32 h,
                                const void* src, u32 row, u32 rows)
{
    (void)u; (void)t; (void)face; (void)mip; (void)w; (void)h;
    (void)src; (void)row; (void)rows;
    stub.n_texture_upload++;
}

static void stub_release(void* u, u32 handle)
{
    (void)u;
    if (handle && handle < STUB_MAX_OBJECTS) stub.alive[handle] = 0;
    stub.last_released = handle;
}

static void stub_texture_release(void* u, u32 handle)
{ stub.n_texture_release++; stub_release(u, handle); }

static u32 stub_color_create(void* u, rsx_be_format f, u32 w, u32 h,
                             const void* seed, u32 seed_row)
{
    (void)u; (void)f; (void)seed_row;
    stub.n_color_create++;
    stub.color_w = w; stub.color_h = h;
    if (seed) stub.n_color_seeded++;
    stub.last_created_color = stub_handle();
    return stub.last_created_color;
}

static void stub_color_release(void* u, u32 handle)
{ stub.n_color_release++; stub_release(u, handle); }

static u32 stub_surface_view(void* u, u32 surface, u32 remap, u32 fmt)
{ (void)u; (void)remap; (void)fmt; return surface; }

static u32 stub_depth_create(void* u, u32 w, u32 h)
{ (void)u; (void)w; (void)h; stub.n_depth_create++; return stub_handle(); }

static u32 stub_depth_snapshot(void* u, u32 d, u32 w, u32 h)
{ (void)u; (void)d; (void)w; (void)h; stub.n_depth_snapshot++; return stub_handle(); }

static u32 stub_pipeline_create(void* u, const char* vs, const char* ps,
                                const rsx_be_render_state* rs,
                                const rsx_vertex_layout_plan* layout,
                                u32 stride, rsx_be_format rt, u32 rt_count)
{
    (void)u; (void)rs; (void)layout; (void)stride; (void)rt;
    if (!vs || !ps || !*vs || !*ps) return 0;
    stub.n_pipeline_create++;
    snprintf(g_stub_last_ps, sizeof g_stub_last_ps, "%s", ps);
    stub.pipeline_rt_count = rt_count;
    stub.pipeline_rs = *rs;
    return stub_handle();
}

static void stub_pipeline_release(void* u, u32 p) { (void)u; (void)p; }

static void stub_bind_targets(void* u, const u32* s, u32 count, u32 d)
{
    (void)u;
    stub.bound_surface = count ? s[0] : 0;
    stub.bound_rt_count = count;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
        stub.bound_rt[i] = (i < count) ? s[i] : 0;
    stub.bound_depth = d;
}
static void stub_bind_pipeline(void* u, u32 p) { (void)u; stub.bound_pipeline = p; }
static void stub_bind_vs(void* u, const void* d, u32 n)
{
    (void)u;
    if (d && n >= RSX_DSP_NUM_CONSTANTS * 16u + sizeof stub.vp_xf)
        memcpy(stub.vp_xf, (const u8*)d + RSX_DSP_NUM_CONSTANTS * 16u, sizeof stub.vp_xf);
    if (d && n >= 11u * 16u) memcpy(stub.vp_c10, (const u8*)d + 10u * 16u, 16);
}
static void stub_bind_ps(void* u, const void* d, u32 n) { (void)u; (void)d; (void)n; }

static void stub_bind_textures(void* u, const u32* tex,
                               const rsx_be_sampler_desc* smp, u32 mask)
{
    (void)u;
    memcpy(stub.bound_tex, tex, sizeof stub.bound_tex);
    stub.bound_max_lod0 = smp ? smp[0].max_lod : 0.0f;
    stub.bound_mask = mask;
}

static void stub_bind_vtextures(void* u, const u32* tex,
                                const rsx_be_sampler_desc* smp, u32 mask)
{ (void)u; (void)tex; (void)smp; (void)mask; }

static void stub_viewport(void* u, float x, float y, float w, float h)
{ (void)u; (void)x; (void)y; (void)w; (void)h; }

static void stub_scissor(void* u, u32 x, u32 y, u32 w, u32 h)
{
    (void)u;
    stub.scissor[0] = x; stub.scissor[1] = y;
    stub.scissor[2] = w; stub.scissor[3] = h;
}

static void stub_stencil_ref(void* u, u32 r) { (void)u; stub.stencil_ref = r; }
static void stub_depth_bounds(void* u, float lo, float hi)
{
    (void)u;
    stub.depth_bounds[0] = lo; stub.depth_bounds[1] = hi;
    stub.n_depth_bounds++;
}

static void stub_draw(void* u, rsx_topology topology, const void* verts,
                      u32 count, u32 stride, const u32* indices,
                      u32 index_count)
{
    (void)u; (void)indices;
    stub.n_draw++;
    memset(stub.draw_first_vertex, 0, sizeof stub.draw_first_vertex);
    if (verts && stride >= sizeof stub.draw_first_vertex)
        memcpy(stub.draw_first_vertex, verts, sizeof stub.draw_first_vertex);
    memset(stub.captured_vertices, 0, sizeof stub.captured_vertices);
    memset(stub.captured_indices, 0, sizeof stub.captured_indices);
    if (verts && stride <= sizeof stub.captured_vertices[0])
        for (u32 i = 0; i < count && i < 16; i++)
            memcpy(stub.captured_vertices[i], (const u8*)verts + i * stride, stride);
    if (indices)
        memcpy(stub.captured_indices, indices,
               (index_count < 24 ? index_count : 24) * sizeof(u32));
    stub.draw_topology = topology;
    stub.draw_vertices = count;
    stub.draw_stride = stride;
    stub.draw_indices = index_count;
}

static void stub_clear_color(void* u, u32 s, const float rgba[4])
{
    (void)u; (void)rgba;
    if (stub.n_clear_color < (int)STUB_MAX_CLEARED)
        stub.cleared[stub.n_clear_color] = s;
    memcpy(stub.last_clear_rgba, rgba, sizeof stub.last_clear_rgba);
    stub.n_clear_color++;
}

static void stub_clear_rect(void* u, const u32* rts, u32 n_rts, u32 depth,
                            const float rgba[4], u32 color_mask, u32 ds_flags,
                            float depth_value, u32 stencil, u32 stencil_mask,
                            u32 x, u32 y, u32 w, u32 h)
{
    (void)u; (void)rts; (void)depth; (void)depth_value;
    stub.n_clear_rect++;
    stub.cr_n_rts = n_rts;
    memcpy(stub.cr_rgba, rgba, sizeof stub.cr_rgba);
    stub.cr_mask = color_mask; stub.cr_flags = ds_flags;
    stub.cr_stencil = stencil; stub.cr_smask = stencil_mask;
    stub.cr_rect[0] = x; stub.cr_rect[1] = y; stub.cr_rect[2] = w; stub.cr_rect[3] = h;
}

static void stub_clear_ds(void* u, u32 d, u32 f, float z, u8 s)
{ (void)u; (void)d; (void)f; (void)s; stub.n_clear_ds++; stub.last_clear_depth = z; }

static u32 presented_surface;
static void stub_present(void* u, u32 s) { (void)u; presented_surface = s; stub.n_present++; }

/* One pixel of a surface, in the R,G,B,A order rsx_texture_decode produces:
 * the readback hook has to put those back into the guest's A8R8G8B8. */
static u8 g_stub_pixel[4] = { 0x11, 0x22, 0x33, 0x44 };
/* An FP16 surface's pixel: R,G,B,A halves 1.0, 2.0, 3.0, 4.0, little-endian
 * as a host GPU hands them back. */
static const u8 g_stub_pixel16[8] = { 0x00, 0x3C, 0x00, 0x40, 0x00, 0x42, 0x00, 0x44 };

/* With g_stub_ramp set, pixel (c, r) reads back as R,G,B,A = 4c+1, 4c+2,
 * 4c+3, 0x80+r instead, so every byte of a row is distinct. */
static int g_stub_ramp;

/* The engine asks for rows packed at the surface's own pixel size, so the
 * pitch tells the two formats apart. */
static void stub_readback(void* u, u32 s, u32 x, u32 y, u32 w, u32 h,
                          void* out, u32 pitch)
{
    (void)u; (void)s; (void)x; (void)y;
    const u32 bpp = pitch >= w * 8u ? 8u : 4u;
    for (u32 r = 0; r < h; r++)
        for (u32 c = 0; c < w; c++) {
            u8* p = (u8*)out + (size_t)r * pitch + c * bpp;
            if (bpp == 8u) { memcpy(p, g_stub_pixel16, 8); continue; }
            if (!g_stub_ramp) { memcpy(p, g_stub_pixel, 4); continue; }
            p[0] = (u8)(4u * c + 1u); p[1] = (u8)(4u * c + 2u); p[2] = (u8)(4u * c + 3u);
            p[3] = (u8)(0x80u + r);
        }
}

/* Every depth target reads back as 0.5, which Z24S8 stores as 0x800000. */
static int stub_depth_readback(void* u, u32 d, u32 x, u32 y, u32 w, u32 h,
                               float* out, u32 pitch)
{
    (void)u; (void)d; (void)x; (void)y;
    stub.n_depth_readback++;
    for (u32 r = 0; r < h; r++)
        for (u32 c = 0; c < w; c++)
            *(float*)((u8*)out + (size_t)r * pitch + c * 4u) = 0.5f;
    return 0;
}

/* The last rectangle pushed into a colour target, its first pixel and the
 * first 64 bytes of its first row. */
static struct { int n; u32 surface, x, y, w, h; u8 first[4]; u8 row[64]; } g_written;

static void stub_color_write(void* u, u32 s, u32 x, u32 y, u32 w, u32 h,
                             const void* rows, u32 row_bytes)
{
    (void)u; (void)row_bytes;
    g_written.n++;
    g_written.surface = s; g_written.x = x; g_written.y = y;
    g_written.w = w; g_written.h = h;
    memcpy(g_written.first, rows, 4);
    memcpy(g_written.row, rows, w * 4u < sizeof g_written.row ? w * 4u : sizeof g_written.row);
}

static const rsx_draw_backend g_stub_backend = {
    .user = NULL,
    .init = stub_init,
    .shutdown = stub_shutdown,
    .submit_and_wait = stub_submit,
    .texture_create = stub_texture_create,
    .texture_upload = stub_texture_upload,
    .texture_release = stub_texture_release,
    .color_target_create = stub_color_create,
    .color_target_release = stub_color_release,
    .color_target_write = stub_color_write,
    .surface_view = stub_surface_view,
    .depth_target_create = stub_depth_create,
    .depth_target_release = stub_release,
    .depth_snapshot = stub_depth_snapshot,
    .pipeline_create = stub_pipeline_create,
    .pipeline_release = stub_pipeline_release,
    .bind_targets = stub_bind_targets,
    .bind_pipeline = stub_bind_pipeline,
    .bind_vs_constants = stub_bind_vs,
    .bind_ps_constants = stub_bind_ps,
    .bind_textures = stub_bind_textures,
    .bind_vertex_textures = stub_bind_vtextures,
    .set_viewport = stub_viewport,
    .set_scissor = stub_scissor,
    .set_stencil_ref = stub_stencil_ref,
    .set_depth_bounds = stub_depth_bounds,
    .draw = stub_draw,
    .clear_color = stub_clear_color,
    .clear_depth_stencil = stub_clear_ds,
    .present = stub_present,
    .readback = stub_readback,
    .depth_readback = stub_depth_readback,
};

/* ---- driving the engine -------------------------------------------------- */

#define M_CONTEXT_DMA_COLOR_B 0x018C
#define M_CONTEXT_DMA_COLOR_A 0x0194
#define M_CONTEXT_DMA_COLOR_C 0x01B4
#define M_CONTEXT_DMA_COLOR_D 0x01B8
#define M_SURFACE_CLIP_H      0x0200
#define M_SURFACE_CLIP_V      0x0204
#define M_SURFACE_FORMAT      0x0208
#define M_COLOR_A_PITCH       0x020C
#define M_COLOR_A_OFFSET      0x0210
#define M_ZETA_OFFSET         0x0214
#define M_COLOR_B_OFFSET      0x0218
#define M_COLOR_TARGET        0x0220
#define M_CONTEXT_DMA_ZETA    0x0198
#define M_ZSTENCIL_CLEAR      0x1D8C
#define M_ZETA_PITCH          0x022C
#define M_COLOR_MASK          0x0324
#define M_DEPTH_WRITE         0x0A70
#define M_DEPTH_TEST          0x0A74
#define M_COLOR_C_OFFSET      0x0288
#define M_COLOR_D_OFFSET      0x028C
#define M_SCISSOR_H           0x08C0
#define M_SCISSOR_V           0x08C4
#define M_VTXBUF_OFFSET       0x1680
#define M_VTXFMT              0x1740
#define M_BEGIN_END           0x1808
#define M_DRAW_ARRAYS         0x1814
#define M_INLINE_ARRAY        0x1818
#define M_TEX_UNIT            0x1A00   /* + unit * 0x20 */
#define M_STENCIL_FUNC_REF    0x0334
#define M_DEPTH_BOUNDS_ENABLE 0x0380
#define M_DEPTH_BOUNDS_MIN    0x0384
#define M_DEPTH_BOUNDS_MAX    0x0388
#define M_CLEAR_BUFFERS       0x1D94
#define M_VP_UPLOAD_CONST_ID  0x1EFC
#define M_VP_UPLOAD_CONST     0x1F00

#define DMA_LOCAL  0xFEED0000u
#define DMA_MAIN   0xFEED0001u

#define VTX_OFFSET  0x00010000u        /* main memory, well clear of ea 0 */
#define TEX_OFFSET  0x00020000u

static void m(u32 method, u32 arg) { rsx_draw_engine_method(method, arg); }

/* engine_up registers this backend instead of g_stub_backend when set. */
static const rsx_draw_backend* g_stub_be_override;

static void engine_up(void)
{
    memset(&stub, 0, sizeof stub);
    rsx_draw_engine_set_backend(g_stub_be_override ? g_stub_be_override : &g_stub_backend);
    if (rsx_draw_engine_init(1280, 720) != 0) {
        printf("[FAIL] engine init\n");
        exit(1);
    }
    /* One 256x256 surface in local memory, and a vertex array in main. */
    m(M_CONTEXT_DMA_COLOR_A, DMA_LOCAL);
    m(M_SURFACE_CLIP_H, 256u << 16);
    m(M_SURFACE_CLIP_V, 256u << 16);
    m(M_COLOR_A_OFFSET, 0x40000u);
    /* A8R8G8B8 over RPCS3's reset 0x121 (X1R5G5B5_Z, Z16, linear): the
     * tests' byte expectations are for 32-bit ARGB pixels. */
    m(M_SURFACE_FORMAT, 8u | (1u << 5) | (1u << 8));
    /* Target A at a pitch that holds a row, and a zeta likewise: RPCS3's
     * framebuffer layout drops target NONE (the register's 0) and any pitch
     * of 64 bytes or less that cannot hold the written row. */
    m(0x0220, 1u);                 /* SURFACE_COLOR_TARGET = A */
    m(0x020C, 1024u);              /* SURFACE_PITCH_A          */
    m(M_ZETA_PITCH, 1024u);
    /* type 2 (float32), size 4, stride 16; bit 31 of the offset = main. */
    m(M_VTXFMT, 2u | (4u << 4) | (16u << 8));
    m(M_VTXBUF_OFFSET, 0x80000000u | VTX_OFFSET);
}

static void engine_down(void)
{
    rsx_draw_engine_shutdown();
    rsx_draw_engine_set_backend(NULL);
}

static void draw_triangle(void)
{
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_DRAW_ARRAYS, 0u | ((3u - 1u) << 24));
    m(M_BEGIN_END, 0u);
}

/* Bind unit 0 at `offset` in `location` (0 = local, 1 = main), 4x4 A8R8G8B8
 * linear with one level. */
static void bind_texture_unit(u32 location, u32 offset)
{
    const u32 dma = (location == RSX_LOCATION_MAIN) ? 2u : 1u;
    m(M_TEX_UNIT + 0x00, offset);
    m(M_TEX_UNIT + 0x04, dma | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
    m(M_TEX_UNIT + 0x0C, 0x80000000u);        /* control0: unit enable */
    m(M_TEX_UNIT + 0x10, 0xAAE4u);            /* identity crossbar */
    m(M_TEX_UNIT + 0x18, (4u << 16) | 4u);    /* image rect 4x4 */
}

/* ---- 1. the surface set -------------------------------------------------- */

static void test_surface_set(void)
{
    printf("-- surface set\n");
    engine_up();

    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 1 && stub.n_clear_color == 1,
          "a clear registers the bound colour surface (%d create, %d clear)",
          stub.n_color_create, stub.n_clear_color);
    const u32 first = stub.last_created_color;

    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 1,
          "the same (context, offset, size, format) is the same surface");

    /* A size change reallocates and retires the old target. */
    m(M_SURFACE_CLIP_H, 512u << 16);
    m(M_SURFACE_CLIP_V, 512u << 16);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 2 && stub.n_color_release == 1 &&
          stub.alive[first] == 0,
          "a size change reallocates and releases the old target");

    /* An offset change is a different surface, and does not disturb the
     * first. */
    m(M_COLOR_A_OFFSET, 0x80000u);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 3 && stub.n_color_release == 1,
          "a different offset is a different surface");

    /* The same offset in the OTHER context DMA is also a different surface:
     * RSX offsets are two overlapping number spaces. */
    m(M_CONTEXT_DMA_COLOR_A, DMA_MAIN);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 4 && stub.n_color_release == 1,
          "the same offset in another context DMA is another surface");

    /* RPCS3 (Read Color Buffers off): a new surface is cleared, never loaded
     * from guest bytes. RSX_ENG_SEED_GUEST=1 restores the seed. */
    CHECK(stub.n_color_seeded == 0,
          "no surface was seeded from guest bytes (%d)",
          stub.n_color_seeded);

    /* An implausible clip does not destroy a live target. */
    const int creates = stub.n_color_create;
    m(M_SURFACE_CLIP_H, 60000u << 16);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == creates,
          "an implausible surface declaration keeps the existing target");

    engine_down();
}

/* ---- 2. surface aliasing on texture bind --------------------------------- */

static void test_surface_aliasing(void)
{
    printf("-- surface aliasing\n");
    engine_up();

    /* Render into a surface in MAIN memory, then sample a texture unit bound
     * at the same offset in the same context: the unit must read the live
     * target, not the guest bytes behind it. That is what every reflection,
     * shadow composite and post-process pass depends on. */
    m(M_CONTEXT_DMA_COLOR_A, DMA_MAIN);
    m(M_COLOR_A_OFFSET, TEX_OFFSET);
    m(M_CLEAR_BUFFERS, 0xF0u);
    const u32 surface = stub.last_created_color;

    /* Now draw into a DIFFERENT surface while sampling that one. */
    m(M_COLOR_A_OFFSET, 0x90000u);
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    m(0x1840 /* control3: the surface's pitch (check_framebuffer_resource) */, 1024u);
    draw_triangle();
    CHECK(stub.n_draw == 1, "the draw executed");
    CHECK(stub.bound_mask == 1u, "unit 0 is the only bound unit (0x%X)",
          stub.bound_mask);
    CHECK(stub.bound_tex[0] == surface,
          "the unit samples the registered surface (%u, want %u)",
          stub.bound_tex[0], surface);
    CHECK(stub.n_texture_create == 0,
          "and nothing was uploaded from guest memory (%d uploads)",
          stub.n_texture_create);

    /* A unit naming the surface the pass is drawing INTO must not alias it:
     * reading a target a pass is writing is undefined on every API. */
    m(M_COLOR_A_OFFSET, TEX_OFFSET);
    draw_triangle();
    CHECK(stub.bound_tex[0] != surface,
          "a unit naming its own render target does not alias it");

    /* The same offset in the other context DMA is a different object. */
    m(M_COLOR_A_OFFSET, 0x90000u);
    bind_texture_unit(RSX_LOCATION_LOCAL, TEX_OFFSET);
    const int before = stub.n_texture_create;
    draw_triangle();
    engine_down();
}

/* RPCS3 mipmap_gather: a mipped texture over a render target whose level 1
 * is a render target too samples the whole chain with its LOD range; with
 * level 0 alone it is the single-level surface read (LOD 0). */
static void test_rtt_mip_gather(void)
{
    printf("-- render-target mip chain\n");
    engine_up();
    m(M_CONTEXT_DMA_COLOR_A, DMA_MAIN);
    m(M_COLOR_A_OFFSET, TEX_OFFSET);
    m(M_CLEAR_BUFFERS, 0xF0u);
    const u32 level0 = stub.last_created_color;
    m(M_COLOR_A_OFFSET, 0x90000u);
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    m(M_TEX_UNIT + 0x04, 2u | (2u << 4) | ((0x85u | 0x20u) << 8) | (2u << 16));
    m(M_TEX_UNIT + 0x0C, 0x80000000u | (512u << 7));  /* max LOD 2.0 (4.8) */
    m(M_TEX_UNIT + 0x14, (6u << 16) | (2u << 24));  /* linear-mipmap-linear */
    m(M_TEX_UNIT + 0x18, (256u << 16) | 256u);
    m(0x1840, 1024u);
    draw_triangle();
    CHECK(stub.bound_tex[0] == level0 && stub.bound_max_lod0 == 0.0f,
          "level 0 alone: the surface, LOD 0 (%u want %u, max lod %.2f)",
          stub.bound_tex[0], level0, stub.bound_max_lod0);

    /* Level 1 (128x128 at pitch 1024, after 256 rows) rendered too. */
    m(M_SURFACE_CLIP_H, 128u << 16);
    m(M_SURFACE_CLIP_V, 128u << 16);
    m(M_COLOR_A_OFFSET, TEX_OFFSET + 1024u * 256u);
    m(M_CLEAR_BUFFERS, 0xF0u);
    m(M_COLOR_A_OFFSET, 0x90000u);
    const int before = stub.n_texture_create;
    draw_triangle();
    CHECK(stub.bound_tex[0] != level0 && stub.n_texture_create == before + 1 &&
          stub.bound_max_lod0 > 0.0f,
          "both levels targets: the gathered chain with its LOD range (max lod %.2f, tex %u, +%d)",
          stub.bound_max_lod0, stub.bound_tex[0], stub.n_texture_create - before);

    /* T2-07: three levels, the third not a target -- LOD clamps at 1. */
    m(M_TEX_UNIT + 0x04, 2u | (2u << 4) | ((0x85u | 0x20u) << 8) | (3u << 16));
    draw_triangle();
    CHECK(stub.bound_max_lod0 == 1.0f,
          "two target levels of three: max LOD clamps to 1 (%.2f)", stub.bound_max_lod0);
    engine_down();
}

/* ---- 2b. the MRT set ----------------------------------------------------- */

static void test_mrt_set(void)
{
    printf("-- MRT set\n");
    engine_up();

    /* Targets B, C and D alongside the A engine_up declared, then MRT3 --
     * the way a deferred pass declares its G-buffer. */
    m(M_CONTEXT_DMA_COLOR_B, DMA_LOCAL);
    m(M_CONTEXT_DMA_COLOR_C, DMA_LOCAL);
    m(M_CONTEXT_DMA_COLOR_D, DMA_LOCAL);
    m(M_COLOR_B_OFFSET, 0x50000u);
    m(M_COLOR_C_OFFSET, 0x60000u);
    m(M_COLOR_D_OFFSET, 0x70000u);
    m(0x021C, 1024u);              /* SURFACE_PITCH_B/C/D */
    m(0x0280, 1024u);
    m(0x0284, 1024u);
    m(M_COLOR_TARGET, CELL_GCM_SURFACE_TARGET_MRT3);

    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 4,
          "an MRT set registers all four targets (%d)", stub.n_color_create);
    CHECK(stub.n_clear_color == 4,
          "and one CLEAR_SURFACE clears every one of them (%d)",
          stub.n_clear_color);
    CHECK(stub.cleared[0] != stub.cleared[1] &&
          stub.cleared[1] != stub.cleared[2] &&
          stub.cleared[2] != stub.cleared[3] &&
          stub.cleared[0] != stub.cleared[3],
          "four distinct surfaces, not target A four times (%u,%u,%u,%u)",
          stub.cleared[0], stub.cleared[1], stub.cleared[2], stub.cleared[3]);

    draw_triangle();
    CHECK(stub.bound_rt_count == 4 && stub.bound_rt[0] == stub.cleared[0],
          "the draw binds all four, A first (%u bound)", stub.bound_rt_count);
    CHECK(stub.pipeline_rt_count == 4,
          "and its pipeline is built for four attachments (%u)",
          stub.pipeline_rt_count);

    /* A texture unit naming target C must not alias it: reading a target the
     * pass writes is undefined, and an MRT set writes more than one. */
    bind_texture_unit(RSX_LOCATION_LOCAL, 0x60000u);
    const int before = stub.n_texture_create;
    draw_triangle();
    CHECK(stub.n_texture_create == before + 1 &&
          stub.bound_tex[0] != stub.bound_rt[2],
          "a unit naming target C uploads instead of aliasing it");

    /* Back to target A alone: one attachment, and a pipeline of its own,
     * because the attachment count is part of the pipeline key. */
    const int pipelines = stub.n_pipeline_create;
    m(M_COLOR_TARGET, CELL_GCM_SURFACE_TARGET_0);
    draw_triangle();
    CHECK(stub.bound_rt_count == 1 && stub.pipeline_rt_count == 1 &&
          stub.n_pipeline_create == pipelines + 1,
          "target A alone binds one and builds its own pipeline (%u bound, %d)",
          stub.bound_rt_count, stub.n_pipeline_create);

    engine_down();
}

/* ---- 3. the texture cache ------------------------------------------------ */

static void test_texture_cache(void)
{
    printf("-- texture cache\n");
    engine_up();
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    memset(vm_base + TEX_OFFSET, 0x11, 4 * 4 * 4);

    draw_triangle();
    CHECK(stub.n_texture_create == 1 && stub.n_texture_upload == 1,
          "the first bind uploads the texture");
    const u32 first = stub.last_created_texture;

    draw_triangle();
    draw_triangle();
    CHECK(stub.n_texture_create == 1,
          "a repeat bind in the same frame reuses it (%d uploads)",
          stub.n_texture_create);

    /* The guest rewrites the image in place -- an animated UI, a video frame,
     * a surface the 2D engine wrote. The cache must notice, and it costs one
     * content hash per entry per frame, not one per draw. */
    rsx_draw_engine_present();
    memset(vm_base + TEX_OFFSET, 0x22, 4 * 4 * 4);
    draw_triangle();
    CHECK(stub.n_texture_create == 2 && stub.alive[first] == 0,
          "changed guest bytes are re-decoded and the old resource retired");

    rsx_draw_engine_present();
    draw_triangle();
    CHECK(stub.n_texture_create == 2,
          "unchanged bytes in a later frame do not re-decode");

    /* Fill the cache past its capacity with distinct descriptors, then come
     * back to the first: it must have been evicted, not still resident, and
     * the most recent one must still be there. Returning nothing instead of
     * evicting is what made a recovered scene render as flat geometry. */
    for (u32 i = 0; i < RSX_DRAW_ENGINE_TEXTURE_CACHE; i++) {
        rsx_draw_engine_present();
        bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET + 0x1000u + i * 0x100u);
        draw_triangle();
    }
    CHECK(stub.n_texture_release >= 1,
          "the full cache evicts rather than giving up (%d evictions)",
          stub.n_texture_release);
    const u32 newest = stub.last_created_texture;
    const int creates = stub.n_texture_create;
    draw_triangle();
    CHECK(stub.n_texture_create == creates && stub.bound_tex[0] == newest,
          "the most recently used entry survived");

    rsx_draw_engine_present();
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    draw_triangle();
    CHECK(stub.n_texture_create == creates + 1,
          "the least recently used entry was the one evicted");

    engine_down();
}

/* A 128x128 A8R8G8B8 linear texture on unit 0: 64 KB, past the size that is
 * hashed in full every frame. */
#define BIG_TEX_OFFSET 0x00100000u
#define BIG_TEX_DIM    128u
#define BIG_TEX_BYTES  (BIG_TEX_DIM * BIG_TEX_DIM * 4u)
static void bind_big_texture(void)
{
    m(M_TEX_UNIT + 0x00, BIG_TEX_OFFSET);
    m(M_TEX_UNIT + 0x04, 2u | (1u << 3) | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
    m(M_TEX_UNIT + 0x0C, 0x80000000u);
    m(M_TEX_UNIT + 0x10, 0xAAE4u);
    m(M_TEX_UNIT + 0x18, (BIG_TEX_DIM << 16) | BIG_TEX_DIM);
    m(0x1840 /* control3: pitch */, BIG_TEX_DIM * 4u);
}

/* Draw once a frame until the cache re-decodes the texture: how many
 * presents that took, or -1 if not within `limit`. */
static int frames_to_notice(int limit)
{
    const int before = stub.n_texture_create;
    for (int f = 0; f <= limit; f++) {
        draw_triangle();
        if (stub.n_texture_create != before) return f;
        rsx_draw_engine_present();
    }
    return -1;
}

static void test_texture_sparse_rehash(void)
{
    printf("-- texture re-hash\n");
    u8* tex = vm_base + BIG_TEX_OFFSET;
    /* The hash fallback alone: page protection would see every write below
     * at once (test_texcache_coherence). */
    setenv("RSX_TEXCACHE_PROTECT", "0", 1);
    engine_up();
    bind_big_texture();
    memset(tex, 0x11, BIG_TEX_BYTES);
    draw_triangle();
    CHECK(stub.n_texture_create == 1, "the big texture is uploaded");

    /* What the per-frame sparse check sees is noticed in the next frame:
     * a whole rewrite, a byte inside one of the sampled 64-byte blocks (one
     * per 2 KB), a byte of the tail. */
    rsx_draw_engine_present();
    memset(tex, 0x22, BIG_TEX_BYTES);
    CHECK(frames_to_notice(0) == 0, "a rewrite of the whole texture is noticed at once");
    rsx_draw_engine_present();
    tex[3 * 2048 + 5] ^= 0xFF;
    CHECK(frames_to_notice(0) == 0, "a write in a sampled block is noticed at once");
    rsx_draw_engine_present();
    tex[BIG_TEX_BYTES - 1] ^= 0xFF;
    CHECK(frames_to_notice(0) == 0, "a write to the last 64 bytes is noticed at once");

    /* A byte between the sampled blocks waits for the full hash, which comes
     * 8 frames after the last one: the last full hash was the frame above,
     * so this one is seen 7 presents later, and not before. */
    rsx_draw_engine_present();
    tex[3 * 2048 + 100] ^= 0xFF;
    const int late = frames_to_notice(10);
    CHECK(late == 7, "a write between the sampled blocks is noticed by the full hash (%d frames)", late);

    /* A small texture is still hashed in full every frame. */
    rsx_draw_engine_present();
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    draw_triangle();
    rsx_draw_engine_present();
    vm_base[TEX_OFFSET + 37] ^= 0xFF;
    CHECK(frames_to_notice(0) == 0, "a small texture is hashed in full every frame");
    engine_down();

    /* PS3RECOMP_ENG_TEXHASH_FULL=1: the full hash every frame, as before. */
    setenv("PS3RECOMP_ENG_TEXHASH_FULL", "1", 1);
    engine_up();
    bind_big_texture();
    draw_triangle();
    rsx_draw_engine_present();
    tex[5 * 2048 + 100] ^= 0xFF;
    CHECK(frames_to_notice(0) == 0, "PS3RECOMP_ENG_TEXHASH_FULL=1 hashes everything every frame");
    engine_down();
    unsetenv("PS3RECOMP_ENG_TEXHASH_FULL");
    unsetenv("RSX_TEXCACHE_PROTECT");
}

/* ---- texture cache coherence (scratch/parity/texcache.md) ---------------- */

static int g_tc_block;   /* the guest reader fails the 4x4 texture's bytes */
static const u8* tc_guest_read(void* user, u32 location, u32 offset, u32 bytes)
{
    (void)user;
    const u32 ea = location == 0 ? GUEST_LOCAL_EA + offset : offset;
    if (g_tc_block && location != 0 && offset < TEX_OFFSET + 64u && offset + bytes > TEX_OFFSET)
        return NULL;
    if ((u64)ea + bytes > GUEST_BYTES) return NULL;
    return vm_base + ea;
}

static struct { int n; u32 src, x, y, w, h, ret; } g_tcr;
static u32 tc_rec_region(void* u, u32 s, u32 x, u32 y, u32 w, u32 h)
{
    (void)u;
    g_tcr.n++; g_tcr.src = s; g_tcr.x = x; g_tcr.y = y; g_tcr.w = w; g_tcr.h = h;
    return g_tcr.ret = stub_handle();
}

/* Render into the 256x256 A8R8G8B8 surface at local 0x40000, then move
 * target A to 0x80000 so the first surface can be sampled. */
static u32 tc_render_then_retarget(void)
{
    m(M_COLOR_MASK, 0x01010101u);
    m(M_SURFACE_FORMAT, 8u | (2u << 5) | (1u << 8));
    draw_triangle();
    const u32 surface = stub.bound_rt[0];
    m(M_COLOR_A_OFFSET, 0x80000u);
    return surface;
}

static void test_texcache_coherence(void)
{
    printf("-- texture cache coherence\n");
    u8* tex = vm_base + BIG_TEX_OFFSET;

    /* A write the engine is told about (SPU DMA put) re-hashes the texture
     * at its next use, in the same frame -- RPCS3's page fault invalidation
     * -- even where only the 8-frame full hash would see it. */
    for (int legacy = 0; legacy < 2; legacy++) {
        if (legacy) setenv("RSX_TEXCACHE_LEGACY", "1", 1);
        engine_up();
        bind_big_texture();
        memset(tex, 0x11, BIG_TEX_BYTES);
        draw_triangle();
        rsx_draw_engine_present();
        draw_triangle();
        const int before = stub.n_texture_create;
        tex[3 * 2048 + 100] ^= 0xFF;
        rsx_draw_engine_note_guest_write(BIG_TEX_OFFSET + 3 * 2048 + 100, 1);
        draw_triangle();
        if (!legacy)
            CHECK(stub.n_texture_create == before + 1,
                  "a noted guest write re-uploads the texture in the same frame");
        else
            CHECK(stub.n_texture_create == before,
                  "RSX_TEXCACHE_LEGACY=1 leaves a noted write to the frame hashes");
        engine_down();
        if (legacy) unsetenv("RSX_TEXCACHE_LEGACY");
    }

    /* RPCS3 protects a cached texture's pages read-only: a plain CPU write
     * (no note) faults, invalidates and opens the page, and the next use in
     * the same frame re-uploads. Legacy leaves the pages writable. */
#ifdef _WIN32
    for (int legacy = 0; legacy < 2; legacy++) {
        if (legacy) setenv("RSX_TEXCACHE_LEGACY", "1", 1);
        memset(tex, 0x11, BIG_TEX_BYTES);
        engine_up();
        bind_big_texture();
        draw_triangle();
        rsx_draw_engine_present();
        draw_triangle();
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery(tex + 3 * 2048, &mbi, sizeof mbi);
        const DWORD prot = mbi.Protect;
        const int before = stub.n_texture_create;
        tex[3 * 2048 + 100] ^= 0xFF;
        VirtualQuery(tex + 3 * 2048, &mbi, sizeof mbi);
        const DWORD opened = mbi.Protect;
        draw_triangle();
        VirtualQuery(tex + 3 * 2048, &mbi, sizeof mbi);
        const DWORD again = mbi.Protect;
        if (!legacy) {
            CHECK(prot == PAGE_READONLY && opened == PAGE_READWRITE && again == PAGE_READONLY,
                  "a cached texture's page is read-only, opened by a write, re-protected at"
                  " its next use (0x%lX 0x%lX 0x%lX)", prot, opened, again);
            CHECK(stub.n_texture_create == before + 1,
                  "a CPU write to a protected texture re-uploads it in the same frame");
        } else {
            CHECK(prot == PAGE_READWRITE && stub.n_texture_create == before,
                  "RSX_TEXCACHE_LEGACY=1 does not protect texture pages");
        }
        engine_down();
        VirtualQuery(tex + 3 * 2048, &mbi, sizeof mbi);
        CHECK(mbi.Protect == PAGE_READWRITE, "engine shutdown unprotects texture pages");
        if (legacy) unsetenv("RSX_TEXCACHE_LEGACY");
    }
#endif

    /* A texture whose bytes could not be read is tried again the next
     * frame instead of staying null until evicted. */
    for (int legacy = 0; legacy < 2; legacy++) {
        if (legacy) setenv("RSX_TEXCACHE_LEGACY", "1", 1);
        engine_up();
        rsx_draw_engine_set_guest_memory(tc_guest_read, NULL);
        bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
        memset(vm_base + TEX_OFFSET, 0x33, 64);
        g_tc_block = 1;
        draw_triangle();
        const int unread = stub.bound_tex[0] == 0;
        g_tc_block = 0;
        rsx_draw_engine_present();
        draw_triangle();
        if (!legacy)
            CHECK(unread && stub.bound_tex[0] != 0,
                  "an unreadable texture is uploaded once its memory reads");
        else
            CHECK(unread && stub.bound_tex[0] == 0,
                  "RSX_TEXCACHE_LEGACY=1 keeps the failed entry");
        rsx_draw_engine_set_guest_memory(NULL, NULL);
        engine_down();
        if (legacy) unsetenv("RSX_TEXCACHE_LEGACY");
    }

    /* check_framebuffer_resource: a texture at a surface's address with
     * another pitch (4x4 packed: 16 bytes, the surface's 1024) is not the
     * surface -- the written target is stored and the texture decoded. */
    for (int legacy = 0; legacy < 2; legacy++) {
        if (legacy) setenv("RSX_TEXCACHE_LEGACY", "1", 1);
        engine_up();
        memset(vm_base + GUEST_LOCAL_EA + 0x40000u, 0xEE, 16);
        const u32 surface = tc_render_then_retarget();
        bind_texture_unit(RSX_LOCATION_LOCAL, 0x40000u);
        const int creates = stub.n_texture_create;
        draw_triangle();
        if (!legacy)
            CHECK(stub.bound_tex[0] != surface && stub.n_texture_create == creates + 1 &&
                  vm_base[GUEST_LOCAL_EA + 0x40000u] != 0xEE,
                  "a pitch-incompatible texture reads the stored target from memory");
        else
            CHECK(stub.bound_tex[0] == surface,
                  "RSX_TEXCACHE_LEGACY=1 samples the whole surface");
        engine_down();
        if (legacy) unsetenv("RSX_TEXCACHE_LEGACY");
    }

    /* A texture inside a target, not at its base: RPCS3 copies the box from
     * the newest surface there (fast_fbo_check) instead of decoding stale
     * guest memory. A 1D texture at a surface reads its first row. */
    {
        static rsx_draw_backend be;
        be = g_stub_backend;
        be.color_region_snapshot = tc_rec_region;
        g_stub_be_override = &be;
        for (int legacy = 0; legacy < 2; legacy++) {
            if (legacy) setenv("RSX_TEXCACHE_LEGACY", "1", 1);
            engine_up();
            memset(&g_tcr, 0, sizeof g_tcr);
            const u32 surface = tc_render_then_retarget();
            bind_texture_unit(RSX_LOCATION_LOCAL, 0x40000u + 8u * 1024u + 16u * 4u);
            m(0x1840 /* control3: pitch */, 1024u);
            const int creates = stub.n_texture_create;
            draw_triangle();
            if (!legacy)
                CHECK(g_tcr.n == 1 && g_tcr.src == surface && g_tcr.x == 16 && g_tcr.y == 8 &&
                      g_tcr.w == 4 && g_tcr.h == 4 && stub.bound_tex[0] == g_tcr.ret &&
                      stub.n_texture_create == creates,
                      "a texture inside a target copies its box (%u,%u %ux%u)",
                      g_tcr.x, g_tcr.y, g_tcr.w, g_tcr.h);
            else
                CHECK(g_tcr.n == 0 && stub.n_texture_create == creates + 1,
                      "RSX_TEXCACHE_LEGACY=1 decodes the texture from memory");

            /* 1D, 64 texels, at the surface base. */
            memset(&g_tcr, 0, sizeof g_tcr);
            m(M_TEX_UNIT + 0x00, 0x40000u);
            m(M_TEX_UNIT + 0x04, 1u | (1u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
            m(M_TEX_UNIT + 0x18, (64u << 16) | 1u);
            draw_triangle();
            if (!legacy)
                CHECK(g_tcr.n == 1 && g_tcr.x == 0 && g_tcr.y == 0 && g_tcr.w == 64 &&
                      g_tcr.h == 1 && stub.bound_tex[0] == g_tcr.ret,
                      "a 1D texture of a surface reads its first row");
            else
                CHECK(g_tcr.n == 0 && stub.bound_tex[0] == surface,
                      "RSX_TEXCACHE_LEGACY=1 samples the whole surface as 1D");
            engine_down();
            if (legacy) unsetenv("RSX_TEXCACHE_LEGACY");
        }
        g_stub_be_override = NULL;
    }
}

/* ---- RPCS3 texture-cache parity, round 2 --------------------------------- */

static struct { int n; u32 w; u8 row[16]; } g_tcup;
static void tc_rec_upload(void* u, u32 t, u32 face, u32 mip, u32 w, u32 h,
                          const void* src, u32 row, u32 rows)
{
    stub_texture_upload(u, t, face, mip, w, h, src, row, rows);
    if (face || mip) return;
    g_tcup.n++; g_tcup.w = w;
    memcpy(g_tcup.row, src, row < 16u ? row : 16u);
}

#define ZETA_OFFSET  0x00100000u        /* local memory */

static void test_texcache_round2(void)
{
    printf("-- texture cache round 2 (RPCS3 parity)\n");
    static rsx_draw_backend be;
    for (int legacy = 0; legacy < 2; legacy++) {
        if (legacy) setenv("RSX_TEXCACHE_LEGACY", "1", 1);

        /* upload_texture: a linear row longer than the pitch is pitch / bpp
         * texels wide and the rest of the width reads through the S wrap. */
        be = g_stub_backend;
        be.texture_upload = tc_rec_upload;
        g_stub_be_override = &be;
        engine_up();
        memset(&g_tcup, 0, sizeof g_tcup);
        u8* p = vm_base + TEX_OFFSET;
        for (u32 i = 0; i < 64; i++) p[i] = (u8)(i + 1u);
        bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
        m(0x1840 /* control3: pitch */, 8u);
        m(M_TEX_UNIT + 0x08, 1u | (1u << 8) | (1u << 16));   /* wrap */
        draw_triangle();
        if (!legacy)
            CHECK(g_tcup.n == 1 && g_tcup.w == 4 && !memcmp(g_tcup.row, g_tcup.row + 8, 8) &&
                  memcmp(g_tcup.row, g_tcup.row + 4, 4),
                  "a row longer than its pitch repeats its 2 texels by the wrap mode");
        else
            CHECK(g_tcup.n == 1 && memcmp(g_tcup.row, g_tcup.row + 8, 8),
                  "RSX_TEXCACHE_LEGACY=1 reads the packed row");
        engine_down();
        g_stub_be_override = NULL;

        /* FP16 target sampled pitch-incompatibly: stored to guest memory
         * (big-endian halves), as RPCS3 flushes every colour format. */
        engine_up();
        m(M_COLOR_MASK, 0x01010101u);
        m(M_COLOR_A_PITCH, 256u * 8u);
        m(M_SURFACE_FORMAT, 0xBu | (2u << 5) | (1u << 8));
        draw_triangle();
        const u32 fp = stub.bound_rt[0];
        m(M_COLOR_A_OFFSET, 0x90000u);
        bind_texture_unit(RSX_LOCATION_LOCAL, 0x40000u);
        m(M_TEX_UNIT + 0x04, 1u | (2u << 4) | ((0x9Au | 0x20u) << 8) | (1u << 16));
        draw_triangle();
        const u8* q = vm_base + GUEST_LOCAL_EA + 0x40000u;
        if (!legacy)
            CHECK(q[0] == 0x3C && q[1] == 0x00 && q[2] == 0x40 && q[3] == 0x00 &&
                  stub.bound_tex[0] != fp,
                  "an FP16 target is stored as big-endian halves (%02X %02X %02X %02X)",
                  q[0], q[1], q[2], q[3]);
        else
            CHECK(stub.bound_tex[0] == fp, "RSX_TEXCACHE_LEGACY=1 binds the FP16 target");
        engine_down();

        /* A texture inside a written zeta reads the zeta's bytes. */
        engine_up();
        m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
        m(M_SURFACE_FORMAT, 8u | (2u << 5));
        m(M_ZETA_OFFSET, ZETA_OFFSET);
        m(M_ZETA_PITCH, 1024u);
        m(M_COLOR_MASK, 0u);
        m(M_DEPTH_TEST, 1u);
        m(M_DEPTH_WRITE, 1u);
        draw_triangle();
        m(M_DEPTH_WRITE, 0u);
        m(M_COLOR_MASK, 0x01010101u);
        bind_texture_unit(RSX_LOCATION_LOCAL, ZETA_OFFSET + 8u * 1024u + 64u);
        m(0x1840, 1024u);
        draw_triangle();
        if (!legacy)
            CHECK(stub.n_depth_readback == 1,
                  "a texture inside a zeta stores it first (%d)", stub.n_depth_readback);
        else
            CHECK(stub.n_depth_readback == 0,
                  "RSX_TEXCACHE_LEGACY=1 leaves zetas out of the overlap search");
        engine_down();

        /* pitch_compatible: a pitch-0 texture at a target's base is packed
         * (16 bytes), not the target's 1024, so it is not its sub-region. */
        be = g_stub_backend;
        be.color_region_snapshot = tc_rec_region;
        g_stub_be_override = &be;
        engine_up();
        memset(&g_tcr, 0, sizeof g_tcr);
        tc_render_then_retarget();
        bind_texture_unit(RSX_LOCATION_LOCAL, 0x40000u);
        m(0x1840 /* control3: pitch 0 (the reset image holds 8) */, 0u);
        draw_triangle();
        if (!legacy)
            CHECK(g_tcr.n == 0, "a pitch-0 texture is not a pitch-1024 target's region");
        else
            CHECK(g_tcr.n == 1, "RSX_TEXCACHE_LEGACY=1 matches pitch 0 to any target (%d)", g_tcr.n);
        engine_down();
        g_stub_be_override = NULL;

        /* RPCS3's surface store is unbounded: past the table's 64 targets
         * the oldest is evicted (stored first) instead of the draw dropped. */
        engine_up();
        m(M_COLOR_MASK, 0x01010101u);
        m(M_SURFACE_FORMAT, 8u | (2u << 5) | (1u << 8));
        m(M_SURFACE_CLIP_H, 32u << 16);
        m(M_SURFACE_CLIP_V, 32u << 16);
        m(M_COLOR_A_PITCH, 128u);
        for (u32 i = 0; i < 70; i++) {
            m(M_COLOR_A_OFFSET, 0x200000u + i * 0x2000u);
            draw_triangle();
        }
        if (!legacy)
            CHECK(stub.n_draw == 70 && stub.n_color_release >= 6,
                  "a full surface table evicts (%d draws, %d released)",
                  stub.n_draw, stub.n_color_release);
        else
            CHECK(stub.n_draw < 70, "RSX_TEXCACHE_LEGACY=1 drops draws past the table");
        engine_down();

        if (legacy) unsetenv("RSX_TEXCACHE_LEGACY");
    }
}

/* ---- 4. the pipeline key ------------------------------------------------- */

static void test_pipeline_key(void)
{
    printf("-- pipeline key\n");

    /* The key over the render state directly: what must and must not change
     * it. The alpha REFERENCE lives in the fragment constant block, so a
     * title fading something out must not compile a pipeline per frame. */
    rsx_be_render_state a;
    memset(&a, 0, sizeof a);
    a.alpha_test_enable = 1;
    a.alpha_func = 0x0204;
    a.alpha_ref_raw = 0x40;
    a.depth_func = 0x0201;
    const u64 base = rsx_draw_engine_hash_render_state(&a, 1469598103934665603ull);

    rsx_be_render_state b = a;
    b.alpha_ref_raw = 0xC0;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) == base,
          "the alpha reference is not part of the key");

    b = a; b.alpha_func = 0x0202;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "the alpha compare mode is");

    b = a; b.depth_func = 0x0203;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "so is the depth compare mode");

    b = a; b.color_mask = 0x01010101u;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "so is the colour mask");

    b = a; b.rt_fp16 = 1;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "so is an FP16 render target");

    b = a; b.depth_bounds_test = 1;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "so is the depth bounds test");

    /* MRT targets 1..3 carry their own colour mask (COLOR_MASK_MRT, 0x0370)
     * and blend enable (BLEND_ENABLE_MRT, 0x036C). inFAMOUS 2's light volumes
     * add into target 0 with targets 1 and 2 masked off; giving them target
     * 0's state added every light into the G-buffer. */
    b = a; b.color_mask_mrt = 0x0DF0u;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "so is the MRT colour mask");

    b = a; b.blend_mrt = 0x2u;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "so is the MRT blend enable");

    /* And through the engine: transform constants change every draw of every
     * frame, and must not multiply pipelines. */
    engine_up();
    draw_triangle();
    CHECK(stub.n_pipeline_create == 1, "the first draw builds a pipeline");

    for (u32 i = 0; i < 8; i++) {
        m(M_VP_UPLOAD_CONST_ID, i);
        for (u32 w = 0; w < 4; w++) m(M_VP_UPLOAD_CONST + w * 4, 0x3F800000u + i);
        draw_triangle();
    }
    CHECK(stub.n_pipeline_create == 1,
          "changing transform constants does not (%d pipelines)",
          stub.n_pipeline_create);

    /* The stencil reference is encoder state, not pipeline state. */
    m(M_STENCIL_FUNC_REF, 0x7Fu);
    draw_triangle();
    CHECK(stub.n_pipeline_create == 1 && stub.stencil_ref == 0x7Fu,
          "nor does the stencil reference, which reaches the encoder (%u)",
          stub.stencil_ref);

    /* The depth bounds test is pipeline state; its bounds are not. A deferred
     * renderer draws its lights in [0, 0.999984] and its sky in
     * [0.999984, 1] with the same programs. */
    m(M_DEPTH_BOUNDS_ENABLE, 1u);
    draw_triangle();
    CHECK(stub.n_pipeline_create == 2,
          "enabling the depth bounds test builds another pipeline (%d)",
          stub.n_pipeline_create);
    m(M_DEPTH_BOUNDS_MIN, 0x00000000u);      /* 0.0  */
    m(M_DEPTH_BOUNDS_MAX, 0x3F7FFEF9u);      /* 0.999984 */
    draw_triangle();
    CHECK(stub.depth_bounds[0] == 0.0f && stub.depth_bounds[1] > 0.99998f &&
          stub.depth_bounds[1] < 0.99999f,
          "the bounds reach the backend as floats (%g, %g)",
          stub.depth_bounds[0], stub.depth_bounds[1]);
    /* Bounds of [0, 1] leave the zeta unused (RPCS3 get_framebuffer_layout),
     * so only the narrowed bounds bind it: count from there. */
    const int np_bounds = stub.n_pipeline_create;
    m(M_DEPTH_BOUNDS_MIN, 0x3F7FFEF9u);
    m(M_DEPTH_BOUNDS_MAX, 0x3F800000u);      /* 1.0  */
    draw_triangle();
    CHECK(stub.n_pipeline_create == np_bounds && stub.depth_bounds[1] == 1.0f,
          "and moving them does not build a pipeline (%d)",
          stub.n_pipeline_create);
    m(M_DEPTH_BOUNDS_ENABLE, 0u);

    /* The decode carries the two MRT registers through to the backend. */
    m(0x0370, 0x0DF0);
    m(0x036C, 0x2);
    draw_triangle();
    CHECK(stub.pipeline_rs.color_mask_mrt == 0x0DF0u && stub.pipeline_rs.blend_mrt == 0x2u,
          "the pipeline is built with COLOR_MASK_MRT and BLEND_ENABLE_MRT");

    engine_down();
}

/* ---- 5. restart cuts and topology expansion ------------------------------ */

static void test_restart_expansion(void)
{
    printf("-- restart cuts\n");

    /* Eight strip vertices with the guest's restart sentinel after the
     * fourth. Without the cut the expansion joins the two runs with two
     * triangles that were never in the mesh -- upstream's "exploded spiky
     * mesh that occluded the scene". */
    const u32 cuts[1] = { 4 };
    u32 idx[64];

    const u32 whole = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_TRIANGLE_STRIP, 8, NULL, 0);
    const u32 cut = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_TRIANGLE_STRIP, 8, cuts, 1);
    CHECK(whole == (8 - 2) * 3, "an uncut strip of 8 is 6 triangles (%u)", whole);
    CHECK(cut == (4 - 2) * 3 * 2,
          "a strip cut in half is two runs of 2 triangles (%u)", cut);

    memset(idx, 0xFF, sizeof idx);
    rsx_draw_engine_write_topology_indices(RSX_PRIMITIVE_TRIANGLE_STRIP, 8,
                                           cuts, 1, NULL, idx);
    int spans = 0;
    for (u32 t = 0; t < cut / 3; t++) {
        int below = 0, above = 0;
        for (u32 k = 0; k < 3; k++)
            if (idx[t * 3 + k] < 4) below++; else above++;
        if (below && above) spans = 1;
    }
    CHECK(!spans, "no triangle spans the cut");

    /* Winding alternates per triangle within a segment, and each segment
     * starts over: a strip's second triangle swaps its first two vertices. */
    CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 2,
          "strip triangle 0 is 0,1,2 (%u,%u,%u)", idx[0], idx[1], idx[2]);
    CHECK(idx[3] == 2 && idx[4] == 1 && idx[5] == 3,
          "strip triangle 1 swaps to 2,1,3 (%u,%u,%u)", idx[3], idx[4], idx[5]);
    CHECK(idx[6] == 4 && idx[7] == 5 && idx[8] == 6,
          "the segment after the cut starts its winding again (%u,%u,%u)",
          idx[6], idx[7], idx[8]);

    /* Fans and quads are cut the same way. */
    const u32 fan = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_TRIANGLE_FAN, 8, cuts, 1);
    CHECK(fan == (4 - 2) * 3 * 2, "a cut fan is two runs of 2 triangles (%u)", fan);
    rsx_draw_engine_write_topology_indices(RSX_PRIMITIVE_TRIANGLE_FAN, 8,
                                           cuts, 1, NULL, idx);
    CHECK(idx[0] == 0 && idx[3] == 0 && idx[6] == 4 && idx[9] == 4,
          "each fan segment fans around its own first vertex");

    const u32 quads = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_QUADS, 8, NULL, 0);
    CHECK(quads == 12, "two quads are four triangles (%u)", quads);

    /* A quad strip advances two vertices per quad, so eight vertices are
     * three quads whole and one quad per half once the strip is cut. */
    const u32 qstrip = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_QUAD_STRIP, 8, NULL, 0);
    const u32 qstrip_cut = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_QUAD_STRIP, 8, cuts, 1);
    CHECK(qstrip == 3 * 6, "an uncut quad strip of 8 is three quads (%u)", qstrip);
    CHECK(qstrip_cut == 2 * 6,
          "a quad strip cut in half is one quad per half (%u)", qstrip_cut);

    memset(idx, 0xFF, sizeof idx);
    rsx_draw_engine_write_topology_indices(RSX_PRIMITIVE_QUAD_STRIP, 8,
                                           cuts, 1, NULL, idx);
    /* RPCS3 draws a quad strip as a native triangle strip, so the second
     * triangle of each quad has the strip's alternating winding. */
    CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 2 &&
          idx[3] == 2 && idx[4] == 1 && idx[5] == 3,
          "a quad strip is a triangle strip "
          "(%u,%u,%u %u,%u,%u)", idx[0], idx[1], idx[2], idx[3], idx[4], idx[5]);
    CHECK(idx[6] == 4 && idx[7] == 5 && idx[8] == 6,
          "the segment after the cut pairs from its own first vertex "
          "(%u,%u,%u)", idx[6], idx[7], idx[8]);
    spans = 0;
    for (u32 t = 0; t < qstrip_cut / 3; t++) {
        int below = 0, above = 0;
        for (u32 k = 0; k < 3; k++)
            if (idx[t * 3 + k] < 4) below++; else above++;
        if (below && above) spans = 1;
    }
    CHECK(!spans, "and no quad spans the cut");

    /* A polygon is a fan around its own first vertex, and a cut starts a
     * second polygon rather than joining the two. */
    const u32 poly = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_POLYGON, 5, NULL, 0);
    const u32 poly_cut = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_POLYGON, 8, cuts, 1);
    CHECK(poly == (5 - 2) * 3, "a pentagon is three triangles (%u)", poly);
    CHECK(poly_cut == (4 - 2) * 3 * 2,
          "a cut polygon is two runs of 2 triangles (%u)", poly_cut);

    memset(idx, 0xFF, sizeof idx);
    rsx_draw_engine_write_topology_indices(RSX_PRIMITIVE_POLYGON, 8,
                                           cuts, 1, NULL, idx);
    CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 2 &&
          idx[3] == 0 && idx[4] == 2 && idx[5] == 3,
          "the first polygon fans around vertex 0 (%u,%u,%u %u,%u,%u)",
          idx[0], idx[1], idx[2], idx[3], idx[4], idx[5]);
    CHECK(idx[6] == 4 && idx[9] == 4,
          "the one after the cut fans around vertex 4 (%u, %u)", idx[6], idx[9]);

    /* An occurrence-to-unique map is applied to every emitted index, which is
     * what lets a remapped draw share one uploaded vertex per reference. */
    u32 remap[8];
    for (u32 i = 0; i < 8; i++) remap[i] = 7u - i;
    rsx_draw_engine_write_topology_indices(RSX_PRIMITIVE_TRIANGLES, 6,
                                           NULL, 0, remap, idx);
    CHECK(idx[0] == 7 && idx[5] == 2, "the remap is applied to every index");
}

/* ---- 6. what reaches the backend as an indexed draw ---------------------- */

static void test_indexed_draws(void)
{
    printf("-- indexed draws\n");
    engine_up();

    /* A triangle list of distinct vertices has nothing to share, so it is
     * drawn straight. */
    draw_triangle();
    CHECK(stub.draw_indices == 0 && stub.draw_vertices == 3,
          "a triangle list of distinct vertices is not indexed (%u verts, %u indices)",
          stub.draw_vertices, stub.draw_indices);

    /* A strip is expanded into a triangle list through an INDEX buffer: six
     * vertices become twelve indices over six uploaded vertices, rather than
     * twelve duplicated ones. */
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLE_STRIP);
    m(M_DRAW_ARRAYS, 0u | ((6u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_indices == (6 - 2) * 3 && stub.draw_vertices == 6,
          "a strip is indexed, not duplicated (%u verts, %u indices)",
          stub.draw_vertices, stub.draw_indices);

    /* Only the attributes the program reads are fetched, so the stride is the
     * layout's rather than all sixteen registers. The built-in program reads
     * every register, so this is the full one; a guest program that reads two
     * would halve it. */
    CHECK(stub.draw_stride == 16u * 16u,
          "the built-in program's layout is all sixteen attributes (%u bytes)",
          stub.draw_stride);

    /* Quads have no host equivalent and become an indexed triangle list. */
    m(M_BEGIN_END, RSX_PRIMITIVE_QUADS);
    m(M_DRAW_ARRAYS, 0u | ((8u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_TRIANGLES &&
          stub.draw_indices == 12,
          "two quads become twelve triangle indices (%u)", stub.draw_indices);

    /* So do quad strips and polygons, the two the engine used to drop. Six
     * quad-strip vertices are two quads, and a five-vertex polygon is a fan
     * of three triangles. */
    m(M_BEGIN_END, RSX_PRIMITIVE_QUAD_STRIP);
    m(M_DRAW_ARRAYS, 0u | ((6u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_TRIANGLES &&
          stub.draw_indices == 12 && stub.draw_vertices == 6,
          "a quad strip of six becomes twelve indices over six vertices "
          "(%u verts, %u indices)", stub.draw_vertices, stub.draw_indices);

    m(M_BEGIN_END, RSX_PRIMITIVE_POLYGON);
    m(M_DRAW_ARRAYS, 0u | ((5u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_TRIANGLES &&
          stub.draw_indices == 9 && stub.draw_vertices == 5,
          "a five-vertex polygon becomes nine indices (%u verts, %u indices)",
          stub.draw_vertices, stub.draw_indices);

    /* Points and lines are drawn as they are, in the order they arrived: an
     * index buffer would be pointless and collapsing repeats would reorder
     * them. */
    m(M_BEGIN_END, RSX_PRIMITIVE_LINES);
    m(M_DRAW_ARRAYS, 0u | ((4u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_LINES && stub.draw_indices == 0 &&
          stub.draw_vertices == 4,
          "lines pass through unindexed (%u verts, %u indices)",
          stub.draw_vertices, stub.draw_indices);

    m(M_BEGIN_END, RSX_PRIMITIVE_POINTS);
    m(M_DRAW_ARRAYS, 0u | ((2u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_POINTS && stub.draw_vertices == 2,
          "so do points (%u verts)", stub.draw_vertices);

    /* A line loop is a line list closed back to its first vertex (RPCS3
     * expand_line_loop_array): four vertices are four edges. */
    m(M_BEGIN_END, RSX_PRIMITIVE_LINE_LOOP);
    m(M_DRAW_ARRAYS, 0u | ((4u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_LINES && stub.draw_indices == 8,
          "a line loop of four is eight line-list indices (%u)", stub.draw_indices);

    /* ARRAY_ELEMENT16/32 inside BEGIN/END are an inline index list over the
     * bound arrays; a 16-bit word carries two indices, low half first. */
    const u32 draws_before = stub.n_draw;
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(0x180Cu, 0u | (1u << 16));
    m(0x1810u, 2u);
    m(M_BEGIN_END, 0u);
    CHECK(stub.n_draw == draws_before + 1 && stub.draw_topology == RSX_TOPOLOGY_TRIANGLES &&
          (stub.draw_indices == 3 || stub.draw_vertices == 3),
          "three array elements draw one triangle (%u draws, %u verts, %u indices)",
          stub.n_draw - draws_before, stub.draw_vertices, stub.draw_indices);

    engine_down();
}

/* ---- 7. the guest scissor ------------------------------------------------ */

static void test_scissor(void)
{
    printf("-- scissor\n");
    engine_up();

    draw_triangle();
    CHECK(stub.scissor[2] == 256 && stub.scissor[3] == 256,
          "an unwritten scissor register is the whole surface (%ux%u)",
          stub.scissor[2], stub.scissor[3]);

    m(M_SCISSOR_H, (64u << 16) | 16u);
    m(M_SCISSOR_V, (32u << 16) | 8u);
    draw_triangle();
    CHECK(stub.scissor[0] == 16 && stub.scissor[1] == 8 &&
          stub.scissor[2] == 64 && stub.scissor[3] == 32,
          "a guest scissor narrows it (%u,%u %ux%u)",
          stub.scissor[0], stub.scissor[1], stub.scissor[2], stub.scissor[3]);

    m(M_SCISSOR_H, (4096u << 16) | 0u);
    m(M_SCISSOR_V, (4096u << 16) | 0u);
    draw_triangle();
    CHECK(stub.scissor[2] == 256 && stub.scissor[3] == 256,
          "a scissor larger than the surface is clipped to it (%ux%u)",
          stub.scissor[2], stub.scissor[3]);

    /* RPCS3 resolve_scissor(clip_viewport) for draws: the scissor is also
     * clipped to the VIEWPORT_HORIZONTAL/VERTICAL rectangle. */
    m(M_SCISSOR_H, (64u << 16) | 16u);
    m(M_SCISSOR_V, (32u << 16) | 8u);
    m(0x0A00u, (32u << 16) | 24u);
    m(0x0A04u, (16u << 16) | 0u);
    draw_triangle();
    CHECK(stub.scissor[0] == 24 && stub.scissor[1] == 8 &&
          stub.scissor[2] == 32 && stub.scissor[3] == 8,
          "a draw scissor is clipped to the viewport rect (%u,%u %ux%u)",
          stub.scissor[0], stub.scissor[1], stub.scissor[2], stub.scissor[3]);
    m(0x0A00u, 4096u << 16);
    m(0x0A04u, 4096u << 16);

    /* RPCS3 keeps x2 = min(sx + sw, vx + vw) in a u16: an end past 65535
     * wraps below x1, the scissor is invalid and the draw is skipped. */
    m(M_SCISSOR_H, (0x20u << 16) | 0xFFF0u);
    m(0x0A00u, (0x20u << 16) | 0xFFF0u);
    m(M_SCISSOR_V, 4096u << 16);
    {
        const int nd = stub.n_draw;
        draw_triangle();
        CHECK(stub.n_draw == nd, "a scissor whose x + w wraps 16 bits skips the draw (%d)",
              stub.n_draw - nd);
    }
    m(M_SCISSOR_H, 4096u << 16);
    m(0x0A00u, 4096u << 16);

    engine_down();
}

/* ---- 8. the presented pixel ---------------------------------------------- */

static void test_readback(void)
{
    printf("-- readback\n");
    engine_up();
    m(M_CLEAR_BUFFERS, 0xF0u);
    rsx_draw_engine_present();
    CHECK(stub.n_present == 1, "the flip presented a surface");
    CHECK(rsx_draw_engine_readback_center() == 0x44112233u,
          "the presented pixel comes back as A8R8G8B8 (0x%08X)",
          rsx_draw_engine_readback_center());
    engine_down();
}

static void test_queued_flip_buffer(void)
{
    engine_up();
    draw_triangle();
    u32 first = stub.bound_rt[0];
    rsx_draw_engine_set_display_buffer(0, RSX_LOCATION_LOCAL, 0x40000, 1024, 256, 256);
    /* Buffers that do not overlap: a newer surface over part of buffer 0
     * would win RPCS3's merged lookup and force a read from memory. */
    m(M_COLOR_A_OFFSET, 0x80000);
    draw_triangle();
    u32 second = stub.bound_rt[0];
    rsx_draw_engine_set_display_buffer(1, RSX_LOCATION_LOCAL, 0x80000, 1024, 256, 256);
    rsx_draw_engine_present_buffer(0);
    CHECK(presented_surface == first, "queued flip selects first buffer over current target");
    rsx_draw_engine_present_buffer(1);
    CHECK(presented_surface == second && first != second, "queued flip selects second buffer");
    rsx_draw_engine_present();
    CHECK(presented_surface == second, "host present retains last queued buffer");
    int presents = stub.n_present;
    rsx_draw_engine_note_flip(0);
    CHECK(stub.n_present == presents, "noting a flip does not present");
    rsx_draw_engine_present();
    CHECK(presented_surface == first, "host present follows the noted flip");
    engine_down();
}

/* A 2D transfer reads and writes guest memory; the pixels it touches live in
 * the engine's surfaces, so they are synced both ways around it. */
static void test_transfer_coherence(void)
{
    printf("-- transfer coherence\n");
    engine_up();
    m(M_COLOR_A_PITCH, 1024u);                 /* 256 pixels of A8R8G8B8 */
    draw_triangle();
    const u32 surface = stub.bound_rt[0];

    /* The guest wrote A,R,G,B bytes at (2,3): they reach the surface as R,G,B,A. */
    u8* px = vm_base + GUEST_LOCAL_EA + 0x40000u + 3u * 1024u + 2u * 4u;
    px[0] = 0x80; px[1] = 0x01; px[2] = 0x02; px[3] = 0x03;
    memset(&g_written, 0, sizeof g_written);
    rsx_draw_engine_guest_wrote(RSX_LOCATION_LOCAL, 0x40000u + 3u * 1024u + 2u * 4u,
                                1024u, 5u, 4u);
    CHECK(g_written.n == 1 && g_written.surface == surface,
          "a transfer into a surface's bytes is pushed to that surface");
    CHECK(g_written.x == 2 && g_written.y == 3 && g_written.w == 5 && g_written.h == 4,
          "at the rectangle the bytes describe (%u,%u %ux%u)",
          g_written.x, g_written.y, g_written.w, g_written.h);
    CHECK(g_written.first[0] == 0x01 && g_written.first[1] == 0x02 &&
          g_written.first[2] == 0x03 && g_written.first[3] == 0x80,
          "A8R8G8B8 guest bytes arrive as R,G,B,A");

    /* Another pitch lays the rows out elsewhere, and other memory is not a surface. */
    rsx_draw_engine_guest_wrote(RSX_LOCATION_LOCAL, 0x40000u, 2048u, 4u, 4u);
    rsx_draw_engine_guest_wrote(RSX_LOCATION_MAIN, 0x40000u, 1024u, 4u, 4u);
    CHECK(g_written.n == 1, "a different pitch or memory space touches no surface");

    /* A transfer reading a surface's bytes gets the GPU's pixels first. */
    u8* rd = vm_base + GUEST_LOCAL_EA + 0x40000u + 1024u;
    memset(rd, 0, 8);
    rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, 0x40000u + 1024u, 1024u, 2u, 1u);
    CHECK(rd[0] == 0x44 && rd[1] == 0x11 && rd[2] == 0x22 && rd[3] == 0x33 &&
          rd[4] == 0x44 && rd[7] == 0x33,
          "the surface's R,G,B,A pixels land in guest memory as A8R8G8B8");
    engine_down();
}

/* RPCS3 tiled_dma_copy (Common/tiled_dma_copy.hpp), as it steps rows and
 * columns: where linear (row, col) of a tile at `base` lives, from the base. */
static u32 ref_tiled_offset(u32 base, u32 pitch, u32 bank, u32 row, u32 col, u32 bpp)
{
    static const u32 lut[16] = { 0, 1, 2, 3, 2, 3, 0, 1, 1, 2, 3, 0, 3, 0, 1, 2 };
    u32 prime = 1, factor = pitch >> 8;
    if (pitch & (pitch - 1u))
        for (u32 p = 3; p <= 13; p += 2)
            if (factor % p == 0) { prime = p; factor /= p; break; }
    const u32 tpr = prime * factor;
    const u32 this_address = row * pitch + base + col * bpp;
    const u32 texel_offset = (this_address - base) / 256u;
    const u32 tile_x = texel_offset % tpr, tile_y = (texel_offset / tpr) / 64u;
    const u32 sel = (tile_y * tpr + tile_x + (base >> 14)) & 0x3ffffu;
    u32 bs = 0;
    if (factor == 1) bs = sel & 3u;
    else if (factor == 2) bs = lut[((sel + ((tile_y & 1u) << 1)) & 3u) * 4u + (tile_y & 3u)];
    else if (factor >= 4) bs = lut[(sel & 3u) * 4u + (tile_y & 3u)];
    bs = (bs + bank) & 3u;
    const u32 line = (texel_offset / tpr) % 64u;
    const u32 cs = (((line >> 3) & 7u) << 7) | (((this_address >> 5) & 7u) << 4) | ((line & 3u) << 2);
    const u32 part = (((line >> 2) & 1u) + ((this_address >> 6) & 1u)) & 1u;
    u32 a = (((sel >> 2) & 0xFFFFu) << 16) | (bs << 14) | (((cs >> 4) & 0x3Fu) << 8) |
            (part << 7) | (((cs >> 2) & 3u) << 5) | (this_address & 0x1Fu);
    a ^= (((a >> 12) ^ ((bs ^ sel) & 1u) ^ (a >> 14)) & 1u) << 9;
    a ^= ((a >> 11) & 1u) << 10;
    return a - base;
}

/* RPCS3 "Handle RSX Memory Tiling": a surface in a bound MAIN-memory tile is
 * stored tiled and read back detiled; off by default, and local memory never. */
static void test_tiled_memory(void)
{
    printf("-- tiled memory\n");
    const u32 base = 0x100000u;
    static const u32 cfg[2][2] = { { 1024u, 1u }, { 768u, 0u } };   /* pitch, bank */
    for (int on = 0; on < 2; on++)
        for (int k = 0; k < 2; k++) {
            const u32 pitch = cfg[k][0], bank = cfg[k][1], w = pitch / 4u, h = 64u;
            if (on) setenv("RSX_ENG_TILED_MEMORY", "1", 1);
            engine_up();
            rsx_draw_engine_set_tile(4, RSX_LOCATION_MAIN, base, 0x100000u, pitch, 0u, bank, 1);
            m(M_CONTEXT_DMA_COLOR_A, DMA_MAIN);
            m(M_COLOR_A_OFFSET, base);
            m(M_COLOR_A_PITCH, pitch);
            draw_triangle();
            memset(vm_base + base, 0, 0x100000u);
            g_stub_ramp = 1;
            rsx_draw_engine_guest_read(RSX_LOCATION_MAIN, base, pitch, w, h);
            g_stub_ramp = 0;
            int ok = 1, moved = 0;
            for (u32 r = 0; ok && r < h; r++)
                for (u32 c = 0; ok && c < w; c++) {
                    const u32 lin = r * pitch + c * 4u;
                    const u32 at = on ? ref_tiled_offset(base, pitch, bank, r, c, 4u) : lin;
                    const u8* p = vm_base + base + at;
                    moved |= at != lin;
                    ok = p[0] == (u8)(0x80u + r) && p[1] == (u8)(4u * c + 1u) &&
                         p[2] == (u8)(4u * c + 2u) && p[3] == (u8)(4u * c + 3u);
                    if (!ok) printf("   pixel %u,%u at 0x%X\n", c, r, at);
                }
            CHECK(ok && moved == on, "pitch %u bank %u: a flushed surface is %s", pitch, bank,
                  on ? "tiled as RPCS3 tiles it" : "linear with tiling off");
            /* The guest's tiled bytes read back as the surface's pixel. */
            memset(&g_written, 0, sizeof g_written);
            rsx_draw_engine_guest_wrote(RSX_LOCATION_MAIN, base + pitch + 32u, pitch, 1u, 1u);
            CHECK(g_written.n == 1 && g_written.first[0] == 33u && g_written.first[1] == 34u &&
                  g_written.first[2] == 35u && g_written.first[3] == 0x81u,
                  "pitch %u bank %u: guest bytes decode to pixel (8,1)", pitch, bank);
            rsx_draw_engine_set_tile(4, 0, 0, 0, 0, 0, 0, 0);
            engine_down();
            if (on) unsetenv("RSX_ENG_TILED_MEMORY");
        }

    /* A surface narrower than its tile (R3's 960-wide targets in an 8 KB
     * pitch tile): rows step at the tile's pitch, as tiled_dma_copy does;
     * RSX_ENG_TILED_ROW_PITCH=0 steps them at the surface's own. */
    for (int legacy = 0; legacy < 2; legacy++) {
        const u32 tpitch = 1024u, spitch = 512u, w = 128u, h = 64u;
        setenv("RSX_ENG_TILED_MEMORY", "1", 1);
        if (legacy) setenv("RSX_ENG_TILED_ROW_PITCH", "0", 1);
        engine_up();
        rsx_draw_engine_set_tile(4, RSX_LOCATION_MAIN, base, 0x100000u, tpitch, 0u, 1u, 1);
        m(M_CONTEXT_DMA_COLOR_A, DMA_MAIN);
        m(M_SURFACE_CLIP_H, w << 16);
        m(M_COLOR_A_OFFSET, base);
        m(M_COLOR_A_PITCH, spitch);
        draw_triangle();
        memset(vm_base + base, 0, 0x100000u);
        g_stub_ramp = 1;
        rsx_draw_engine_guest_read(RSX_LOCATION_MAIN, base, spitch, w, h);
        g_stub_ramp = 0;
        int ok = 1;
        for (u32 r = 0; ok && r < h; r++)
            for (u32 c = 0; ok && c < w; c++) {
                const u32 at = ref_tiled_offset(base, tpitch, 1u, r, c, 4u);
                const u8* p = vm_base + base + at;
                ok = p[0] == (u8)(0x80u + r) && p[1] == (u8)(4u * c + 1u);
            }
        if (!legacy) CHECK(ok, "a 512-pitch surface in a 1024-pitch tile lands at tile rows");
        else CHECK(!ok, "RSX_ENG_TILED_ROW_PITCH=0 steps rows at the surface pitch");
        rsx_draw_engine_set_tile(4, 0, 0, 0, 0, 0, 0, 0);
        engine_down();
        unsetenv("RSX_ENG_TILED_MEMORY");
        if (legacy) unsetenv("RSX_ENG_TILED_ROW_PITCH");
    }
}

/* R2's RPCS3 patch: a surface at a display buffer is never stored to guest
 * memory for a read (R2 samples VRAM 0 as a dummy DXT1 texture); another is. */
static void test_display_buffer_not_stored(void)
{
    printf("-- display buffer not stored\n");
    engine_up();
    m(M_COLOR_A_PITCH, 1024u);
    draw_triangle();
    rsx_draw_engine_set_display_buffer(0, RSX_LOCATION_LOCAL, 0x40000, 1024, 256, 256);
    u8* db = vm_base + GUEST_LOCAL_EA + 0x40000u + 1024u;
    memset(db, 0, 8);
    rsx_draw_engine_range_read(RSX_LOCATION_LOCAL, 0x40000u, 1024u * 256u);
    CHECK(db[0] == 0 && db[3] == 0 && db[4] == 0, "a display buffer surface stays GPU-only");
    m(M_COLOR_A_OFFSET, 0x80000u);
    draw_triangle();
    u8* other = vm_base + GUEST_LOCAL_EA + 0x80000u + 1024u;
    memset(other, 0, 8);
    rsx_draw_engine_range_read(RSX_LOCATION_LOCAL, 0x80000u, 1024u * 256u);
    CHECK(other[0] == 0x44 && other[3] == 0x33, "another surface is stored for the read");
    engine_down();
}

/* Whole rows, every pixel different, both ways: a byte swapped between two
 * pixels, or a row converted only in part, shows up here. */
static void test_transfer_rows(void)
{
    printf("-- transfer rows\n");
    engine_up();
    m(M_COLOR_A_PITCH, 1024u);
    draw_triangle();

    /* Guest A,R,G,B -> surface R,G,B,A, 13 pixels (not a multiple of a vector). */
    u8* px = vm_base + GUEST_LOCAL_EA + 0x40000u + 5u * 1024u;
    for (u32 c = 0; c < 13; c++) {
        px[4 * c + 0] = (u8)(0xA0 + c); px[4 * c + 1] = (u8)(4 * c + 1);
        px[4 * c + 2] = (u8)(4 * c + 2); px[4 * c + 3] = (u8)(4 * c + 3);
    }
    memset(&g_written, 0, sizeof g_written);
    rsx_draw_engine_guest_wrote(RSX_LOCATION_LOCAL, 0x40000u + 5u * 1024u, 1024u, 13u, 2u);
    int ok = g_written.n == 1 && g_written.w == 13;
    for (u32 c = 0; ok && c < 13; c++)
        ok = g_written.row[4 * c + 0] == 4 * c + 1 && g_written.row[4 * c + 1] == 4 * c + 2 &&
             g_written.row[4 * c + 2] == 4 * c + 3 && g_written.row[4 * c + 3] == 0xA0 + c;
    CHECK(ok, "13 distinct A8R8G8B8 pixels arrive as R,G,B,A in order");

    /* Surface R,G,B,A -> guest A,R,G,B, 3 rows of 13 pixels, and nothing past them. */
    u8* rd = vm_base + GUEST_LOCAL_EA + 0x40000u + 8u * 1024u;
    for (u32 r = 0; r < 4; r++) memset(rd + r * 1024u, 0xEE, 14 * 4);
    g_stub_ramp = 1;
    rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, 0x40000u + 8u * 1024u, 1024u, 13u, 3u);
    g_stub_ramp = 0;
    ok = 1;
    for (u32 r = 0; ok && r < 3; r++)
        for (u32 c = 0; ok && c < 13; c++) {
            const u8* p = rd + r * 1024u + 4 * c;
            ok = p[0] == 0x80 + r && p[1] == 4 * c + 1 && p[2] == 4 * c + 2 && p[3] == 4 * c + 3;
        }
    CHECK(ok, "3 rows of 13 distinct R,G,B,A pixels land as A8R8G8B8 in order");
    CHECK(rd[13 * 4] == 0xEE && rd[3 * 1024u] == 0xEE,
          "the pixel after each row and the row after the rectangle are untouched");
    engine_down();
}

/* An HDR pass renders into an FP16 RGBA target, and the title copies it out
 * with a 2D transfer that counts 32-bit pixels: Twisted Metal's bloom and
 * flames reach its SPU post-FX that way. The copy has to get the GPU's
 * pixels, as big-endian halves, and only the bytes it names. */
static void test_transfer_reads_fp16(void)
{
    printf("-- transfer reads an FP16 surface\n");
    engine_up();
    m(M_SURFACE_FORMAT, 0xBu);                 /* F_W16Z16Y16X16 */
    m(M_COLOR_A_PITCH, 2048u);                 /* 256 pixels of 8 bytes */
    draw_triangle();

    const u32 row = 0x40000u + 3u * 2048u;
    u8* rd = vm_base + GUEST_LOCAL_EA + row;
    static const u8 px[8] = { 0x3C, 0x00, 0x40, 0x00, 0x42, 0x00, 0x44, 0x00 };

    /* Four 32-bit pixels from surface pixel 2: two FP16 pixels. */
    memset(rd, 0xEE, 64);
    rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, row + 2u * 8u, 2048u, 4u, 1u);
    CHECK(memcmp(rd + 16, px, 8) == 0 && memcmp(rd + 24, px, 8) == 0,
          "two FP16 pixels land as big-endian R,G,B,A halves (%02X %02X %02X %02X)",
          rd[16], rd[17], rd[18], rd[19]);
    CHECK(rd[15] == 0xEE && rd[32] == 0xEE, "and nothing around them is written");

    /* One 32-bit pixel starting half way into a surface pixel: its B,A. */
    memset(rd, 0xEE, 64);
    rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, row + 5u * 8u + 4u, 2048u, 1u, 1u);
    CHECK(memcmp(rd + 44, px + 4, 4) == 0 && rd[43] == 0xEE && rd[48] == 0xEE,
          "a transfer starting mid-pixel gets exactly the bytes it names");

    /* The last pixel of the row, asked for past the surface's width. */
    memset(rd + 2040, 0xEE, 8);
    rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, row + 255u * 8u, 2048u, 8u, 1u);
    CHECK(memcmp(rd + 2040, px, 8) == 0, "a transfer past the width stops at the surface's edge");

    /* Bytes copied INTO an FP16 target are not pushed as A8R8G8B8 rows. */
    memset(&g_written, 0, sizeof g_written);
    rsx_draw_engine_guest_wrote(RSX_LOCATION_LOCAL, row, 2048u, 4u, 1u);
    CHECK(g_written.n == 0, "a transfer into an FP16 surface does not write it as A8R8G8B8");
    engine_down();
}

static unsigned custom_reads;
static const u8* custom_guest_read(void* user, u32 location, u32 offset, u32 bytes)
{
    CHECK(user == &custom_reads, "custom memory resolver receives its user data");
    ++custom_reads;
    u32 ea = location == 0 ? GUEST_LOCAL_EA + offset : offset;
    if ((u64)ea + bytes > GUEST_BYTES) return NULL;
    return vm_base + ea;
}

static void test_custom_guest_mapping(void)
{
    engine_up();
    rsx_draw_engine_set_guest_memory(custom_guest_read, &custom_reads);
    custom_reads = 0;
    draw_triangle();
    CHECK(custom_reads > 0, "draw resolves vertices through runner's memory map");
    unsigned calls = custom_reads;
    rsx_draw_engine_set_guest_memory(NULL, NULL);
    draw_triangle();
    CHECK(custom_reads == calls, "clearing resolver restores default mapping");
    engine_down();
}

/* NV4097_INLINE_ARRAY: the vertices arrive IN the pushbuffer.
 *
 * Nothing consumed method 0x1818 at all, so a title that builds geometry this
 * way (Guitar Hero III draws its whole 2D layer as inline QUADS) issued draws
 * that reached the backend with no vertices and were counted as empty groups.
 * The regression this guards is silent by construction -- the draw is simply
 * absent -- so the check is both that a draw happens AND that its vertices
 * came from the stream rather than from the still-bound vertex array. */
static u32 f32_bits(float f) { u32 u; memcpy(&u, &f, 4); return u; }

static void test_inline_array(void)
{
    engine_up();
    /* ATTR0 alone: float4, stride 16. The array at VTX_OFFSET holds 1.0f in
     * every component (see main), so reading the array instead of the stream
     * is a distinguishable wrong answer. */
    m(M_VTXFMT, 2u | (4u << 4) | (16u << 8));

    const int before = stub.n_draw;
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    for (u32 v = 0; v < 3; v++) {
        m(M_INLINE_ARRAY, f32_bits(10.0f + (float)v));  /* x: 10, 11, 12 */
        m(M_INLINE_ARRAY, f32_bits(20.0f));
        m(M_INLINE_ARRAY, f32_bits(30.0f));
        m(M_INLINE_ARRAY, f32_bits(1.0f));
    }
    m(M_BEGIN_END, 0u);

    CHECK(stub.n_draw == before + 1, "inline array issues a draw");
    CHECK(stub.draw_vertices == 3, "inline array draws 3 vertices (got %u)",
          stub.draw_vertices);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_TRIANGLES,
          "inline array keeps the primitive type");
    CHECK(stub.draw_first_vertex[0] == 10.0f &&
          stub.draw_first_vertex[1] == 20.0f &&
          stub.draw_first_vertex[2] == 30.0f,
          "inline vertices come from the stream, not the vertex array "
          "(got %g,%g,%g)", stub.draw_first_vertex[0],
          stub.draw_first_vertex[1], stub.draw_first_vertex[2]);
    engine_down();
}

/* ---- 9. the depth clear value --------------------------------------------- */

#define ZETA_OFFSET  0x00100000u        /* local memory */

static void test_depth_clear_value(void)
{
    printf("-- depth clear value\n");
    engine_up();
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    /* A zeta pitch below bpp * width is RPCS3's "no depth" and skips the clear. */
    m(M_ZETA_PITCH, 256u * 4u);

    /* Z16 (depth field 1): the clear value is a bare depth16, and 0xFFFF its
     * far plane. Read as Z24S8 it would clear to 255/16777215. */
    m(M_SURFACE_FORMAT, 8u | (1u << 5));
    m(M_ZSTENCIL_CLEAR, 0xFFFFu);
    m(M_CLEAR_BUFFERS, 0x01u);
    CHECK(stub.n_clear_ds == 1 && stub.last_clear_depth == 1.0f,
          "a Z16 zeta cleared with 0xFFFF clears to 1.0 (%g)", stub.last_clear_depth);

    /* Z24S8 (depth field 2): depth24 << 8 | stencil8. */
    m(M_SURFACE_FORMAT, 8u | (2u << 5));
    m(M_ZSTENCIL_CLEAR, 0xFFFFFF00u);
    m(M_CLEAR_BUFFERS, 0x01u);
    CHECK(stub.last_clear_depth == 1.0f,
          "a Z24S8 zeta cleared with 0xFFFFFF00 clears to 1.0 (%g)", stub.last_clear_depth);
    engine_down();
}

/* ---- 8b. RPCS3 core_renderer::clear / get_framebuffer_layout rules ------ */

static void test_rpcs3_clear_rules(void)
{
    printf("-- RPCS3 clear rules\n");
    engine_up();
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));   /* A8R8G8B8, Z24S8, linear */

    /* get_depth_clear_value(z24s8, 0) is 0.0, not the far plane. */
    m(M_ZSTENCIL_CLEAR, 0u);
    int ds = stub.n_clear_ds;
    m(M_CLEAR_BUFFERS, 0x01u);
    /* (a first partial clear of a new Z24S8 zeta initialises it first) */
    CHECK(stub.n_clear_ds > ds && stub.last_clear_depth == 0.0f,
          "ZSTENCIL_CLEAR_VALUE 0 clears depth to 0.0 (%g)", stub.last_clear_depth);

    /* A zero STENCIL_MASK removes the stencil bit; a stencil-only clear is
     * then no clear. */
    m(0x032C, 0u);
    ds = stub.n_clear_ds;
    int cc = stub.n_clear_color;
    m(M_CLEAR_BUFFERS, 0x02u);
    CHECK(stub.n_clear_ds == ds && stub.n_clear_color == cc,
          "a stencil-only clear under STENCIL_MASK 0 does nothing");
    m(0x032C, 0xFFu);

    /* Bits outside 0xF3 are not a clear. */
    ds = stub.n_clear_ds; cc = stub.n_clear_color;
    m(M_CLEAR_BUFFERS, 0x0Cu);
    CHECK(stub.n_clear_ds == ds && stub.n_clear_color == cc,
          "mask 0x0C (no RGBA/Z/S bit) is ignored");

    /* Target NONE: no colour buffer, but a depth clear still lands. */
    m(0x0220, 0u);
    ds = stub.n_clear_ds; cc = stub.n_clear_color;
    m(M_CLEAR_BUFFERS, 0xF1u);
    CHECK(stub.n_clear_color == cc && stub.n_clear_ds == ds + 1,
          "target NONE clears depth only (%d colour, %d depth)",
          stub.n_clear_color - cc, stub.n_clear_ds - ds);
    m(0x0220, 1u);

    /* A zeta pitch of 64 or less that cannot hold the row is dropped:
     * depth-only clear skipped, a combined clear keeps the colour. */
    m(M_ZETA_PITCH, 64u);
    ds = stub.n_clear_ds; cc = stub.n_clear_color;
    m(M_CLEAR_BUFFERS, 0x01u);
    m(M_CLEAR_BUFFERS, 0xF1u);
    CHECK(stub.n_clear_ds == ds && stub.n_clear_color == cc + 1,
          "a dropped zeta takes no depth clear, the colour still clears");
    m(M_ZETA_PITCH, 1024u);

    /* Colour and zeta at one address: with no depth or stencil test the
     * colour keeps it, with the depth test on the zeta does. */
    m(M_ZETA_OFFSET, 0x40000u);
    m(M_DEPTH_TEST, 0u);
    ds = stub.n_clear_ds; cc = stub.n_clear_color;
    m(M_CLEAR_BUFFERS, 0xF1u);
    CHECK(stub.n_clear_color == cc + 1 && stub.n_clear_ds == ds,
          "aliased zeta, no depth test: colour wins");
    m(M_DEPTH_TEST, 1u);
    ds = stub.n_clear_ds; cc = stub.n_clear_color;
    m(M_CLEAR_BUFFERS, 0xF1u);
    CHECK(stub.n_clear_color == cc && stub.n_clear_ds > ds,
          "aliased zeta, depth test on: depth wins");
    m(M_DEPTH_TEST, 0u);
    m(M_ZETA_OFFSET, ZETA_OFFSET);

    /* A draw with target NONE writes no colour. */
    m(0x0220, 0u);
    draw_triangle();
    CHECK(stub.pipeline_rs.color_mask == 0u,
          "a draw with target NONE has colour mask 0 (%08X)", stub.pipeline_rs.color_mask);
    m(0x0220, 1u);
    engine_down();
}

/* ---- 8b'. RPCS3 surface_store binding / present-source rules ------------- */

static void test_surface_store_rules(void)
{
    printf("-- RPCS3 surface store rules\n");
    engine_up();
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));   /* A8R8G8B8, Z24S8, linear */

    /* A draw with no depth test leaves the zeta unused: none is created. */
    draw_triangle();
    const u32 first = stub.bound_rt[0];
    CHECK(stub.n_depth_create == 0 && stub.bound_depth == 0u,
          "a draw without depth test binds no zeta (%d created)", stub.n_depth_create);
    m(M_DEPTH_TEST, 1u);
    draw_triangle();
    CHECK(stub.n_depth_create == 1 && stub.bound_rt[0] == first,
          "the depth test binds one (%d created)", stub.n_depth_create);

    /* A buffer the layout drops binds a scratch target and never resizes the
     * surface at its guest address: a 512x512 depth-only clear and a
     * COLOR_MASK 0 draw keep the 256x256 colour surface at 0x40000. */
    m(M_SURFACE_CLIP_H, 512u << 16);
    m(M_SURFACE_CLIP_V, 512u << 16);
    m(M_ZETA_PITCH, 2048u);
    int rel = stub.n_color_release;
    m(M_CLEAR_BUFFERS, 0x01u);
    CHECK(stub.n_color_release == rel,
          "a depth-only clear does not recreate the colour surface (%d released)",
          stub.n_color_release - rel);
    m(M_COLOR_MASK, 0u);
    draw_triangle();
    CHECK(stub.n_color_release == rel && stub.bound_rt[0] != first,
          "nor does a COLOR_MASK 0 draw, which binds a scratch target");
    m(M_COLOR_MASK, 0x01010101u);
    m(M_SURFACE_CLIP_H, 256u << 16);
    m(M_SURFACE_CLIP_V, 256u << 16);
    m(M_ZETA_PITCH, 1024u);

    /* Colour and zeta at one address under a depth test: the zeta keeps it
     * and the colour surface there is not bound. */
    m(M_ZETA_OFFSET, 0x40000u);
    draw_triangle();
    CHECK(stub.bound_rt[0] != first && stub.n_color_release == rel,
          "an aliased colour under a depth test is not bound");
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_DEPTH_TEST, 0u);
    draw_triangle();
    CHECK(stub.bound_rt[0] == first, "and is bound again once the alias is gone");

    /* Present: get_present_source takes the newest written surface of the
     * buffer's pitch over its rows. A newer one of another pitch is ignored;
     * a newer same-pitch one inside it that is not an inset reads memory. */
    rsx_draw_engine_set_display_buffer(0, RSX_LOCATION_LOCAL, 0x40000, 1024, 256, 256);
    m(M_COLOR_A_PITCH, 2048u);
    m(M_COLOR_A_OFFSET, 0x48000u);
    draw_triangle();
    rsx_draw_engine_present_buffer(0);
    CHECK(presented_surface == first, "a newer overlap of another pitch is ignored");
    m(M_COLOR_A_PITCH, 1024u);
    m(M_COLOR_A_OFFSET, 0x48000u);
    draw_triangle();
    const u32 inner = stub.bound_rt[0];
    const int wr = g_written.n;
    rsx_draw_engine_present_buffer(0);
    CHECK(presented_surface != first && presented_surface != inner && g_written.n == wr + 1,
          "a newer same-pitch overlap forces a read from memory (%d uploads)",
          g_written.n - wr);
    m(M_COLOR_A_OFFSET, 0x40000u);
    draw_triangle();
    rsx_draw_engine_present_buffer(0);
    CHECK(presented_surface == first, "until the buffer's own surface is newest again");
    engine_down();
}

/* RPCS3 intersect_surface_region / set_old_contents: what a (re)bound surface
 * inherits from the newer surfaces over its memory. */
static struct { int n; u32 src, sx, sy, sw, sh, dst, dx, dy, dw, dh; } g_copy, g_blit;
static u32 g_zcreated;
static int rec_dcopy(void* u, u32 s, u32 sx, u32 sy, u32 sw, u32 sh,
                     u32 d, u32 dx, u32 dy, u32 dw, u32 dh)
{
    (void)u;
    g_copy.n++; g_copy.src = s; g_copy.sx = sx; g_copy.sy = sy; g_copy.sw = sw; g_copy.sh = sh;
    g_copy.dst = d; g_copy.dx = dx; g_copy.dy = dy; g_copy.dw = dw; g_copy.dh = dh;
    return 1;
}
static void rec_cblit(void* u, u32 s, u32 sx, u32 sy, u32 sw, u32 sh,
                      u32 d, u32 dx, u32 dy, u32 dw, u32 dh)
{
    (void)u;
    g_blit.n++; g_blit.src = s; g_blit.sx = sx; g_blit.sy = sy; g_blit.sw = sw; g_blit.sh = sh;
    g_blit.dst = d; g_blit.dx = dx; g_blit.dy = dy; g_blit.dw = dw; g_blit.dh = dh;
}
static u32 rec_dcreate_ex(void* u, u32 w, u32 h, u32 zfmt, u32 fl)
{
    (void)u; (void)w; (void)h; (void)zfmt; (void)fl;
    stub.n_depth_create++;
    return g_zcreated = stub_handle();
}
static int g_blit_lin;
static void rec_cblitf(void* u, u32 s, u32 sx, u32 sy, u32 sw, u32 sh,
                       u32 d, u32 dx, u32 dy, u32 dw, u32 dh, int linear)
{
    rec_cblit(u, s, sx, sy, sw, sh, d, dx, dy, dw, dh);
    g_blit_lin = linear;
}
static struct { int n; u32 src, w, h, ret; } g_region;
static u32 rec_region(void* u, u32 s, u32 x, u32 y, u32 w, u32 h)
{
    (void)u; (void)x; (void)y;
    g_region.n++; g_region.src = s; g_region.w = w; g_region.h = h;
    return g_region.ret = stub_handle();
}

/* RPCS3 bind_surface_address / surface_matches_properties: a zeta matches
 * only at its exact size; a smaller clip at the same address is a new zeta
 * that inherits the old one's overlap, and the old size again is another. */
static u32 g_zcw, g_zch;
static u32 rec_dcreate_ex_wh(void* u, u32 w, u32 h, u32 zfmt, u32 fl)
{
    g_zcw = w; g_zch = h;
    return rec_dcreate_ex(u, w, h, zfmt, fl);
}

static void test_zeta_exact(void)
{
    printf("-- exact zeta matching\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.depth_target_copy = rec_dcopy;
    be.color_target_blit = rec_cblit;
    be.depth_target_create_ex = rec_dcreate_ex_wh;
    g_stub_be_override = &be;
    engine_up();
    memset(&g_copy, 0, sizeof g_copy);
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    m(M_COLOR_MASK, 0u);
    m(M_CLEAR_BUFFERS, 0x03u);
    const u32 z0 = g_zcreated;
    CHECK(g_zcw == 256 && g_zch == 256, "the zeta is the clip's size, not the canvas (%ux%u)",
          g_zcw, g_zch);
    m(M_SURFACE_CLIP_H, 128u << 16);
    m(M_SURFACE_CLIP_V, 128u << 16);
    m(M_CLEAR_BUFFERS, 0x03u);
    const u32 z1 = g_zcreated;
    CHECK(z1 != z0 && g_zcw == 128 && g_zch == 128 && g_copy.n == 1 && g_copy.src == z0 &&
          g_copy.dst == z1 && g_copy.sw == 128 && g_copy.sh == 128,
          "a smaller clip is a new 128x128 zeta inheriting the overlap (%ux%u, %d copies)",
          g_zcw, g_zch, g_copy.n);
    m(M_CLEAR_BUFFERS, 0x03u);
    CHECK(g_zcreated == z1, "the same size binds the same zeta again");
    m(M_SURFACE_CLIP_H, 256u << 16);
    m(M_SURFACE_CLIP_V, 256u << 16);
    m(M_CLEAR_BUFFERS, 0x03u);
    CHECK(g_zcreated != z1 && g_zcw == 256 && g_zch == 256,
          "the larger size again is another new zeta (%ux%u)", g_zcw, g_zch);
    engine_down();
    g_stub_be_override = NULL;
}

/* RPCS3 VKGSRender.cpp:43-60 get_compatible_surface_format: r5g6b5,
 * x1r5g5b5 and g8b8 / b8 targets are R5G6B5 / A1R5G5B5 / R8G8 / R8 on the
 * host when the backend can render them, the pipeline built for that format;
 * without the hook or support they stay RGBA8. */
static rsx_be_format g_ccreate_fmt, g_pipe_rt;
static u32 g_native_mask;                      /* bit n: format n supported */
static u32 rec_ccreate_fmt(void* u, rsx_be_format f, u32 w, u32 h, const void* seed, u32 row)
{
    g_ccreate_fmt = f;
    return stub_color_create(u, f, w, h, seed, row);
}
static u32 rec_pipe_rt(void* u, const char* vs, const char* ps, const rsx_be_render_state* rs,
                       const rsx_vertex_layout_plan* layout, u32 stride, rsx_be_format rt,
                       u32 rt_count)
{
    g_pipe_rt = rt;
    return stub_pipeline_create(u, vs, ps, rs, layout, stride, rt, rt_count);
}
static int rec_fmt_supported(void* u, rsx_be_format f)
{
    (void)u;
    return (int)((g_native_mask >> (u32)f) & 1u);
}

static void test_native_16bit_targets(void)
{
    printf("-- native 16/8-bit colour targets\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.color_target_create = rec_ccreate_fmt;
    be.pipeline_create = rec_pipe_rt;
    g_stub_be_override = &be;
    engine_up();
    m(M_SURFACE_FORMAT, 3u | (2u << 5) | (1u << 8));
    draw_triangle();
    CHECK(g_ccreate_fmt == RSX_BE_FMT_R8G8B8A8 && g_pipe_rt == RSX_BE_FMT_R8G8B8A8,
          "no color_format_supported: R5G6B5 stays RGBA8 (%d, rt %d)", (int)g_ccreate_fmt,
          (int)g_pipe_rt);
    engine_down();

    be.color_format_supported = rec_fmt_supported;
    g_native_mask = (1u << RSX_BE_FMT_R5G6B5) | (1u << RSX_BE_FMT_R8) | (1u << RSX_BE_FMT_R8G8);
    engine_up();
    static const struct { u32 cfmt; rsx_be_format want; const char* name; } cases[] = {
        { 3u,   RSX_BE_FMT_R5G6B5,   "R5G6B5" },
        { 9u,   RSX_BE_FMT_R8,       "B8" },
        { 0xAu, RSX_BE_FMT_R8G8,     "G8B8" },
        { 2u,   RSX_BE_FMT_R8G8B8A8, "X1R5G5B5 without A1R5G5B5 support" },
        { 8u,   RSX_BE_FMT_R8G8B8A8, "A8R8G8B8" },
    };
    for (u32 i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        g_ccreate_fmt = RSX_BE_FMT_NONE;
        m(M_SURFACE_FORMAT, cases[i].cfmt | (2u << 5) | (1u << 8));
        draw_triangle();
        CHECK(g_ccreate_fmt == cases[i].want && g_pipe_rt == cases[i].want,
              "%s renders into host format %d (created %d, pipeline %d)", cases[i].name,
              (int)cases[i].want, (int)g_ccreate_fmt, (int)g_pipe_rt);
    }
    engine_down();
    be.color_format_supported = NULL;
    g_stub_be_override = NULL;
}

static void test_surface_inheritance(void)
{
    printf("-- RPCS3 surface inheritance\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.depth_target_copy = rec_dcopy;
    be.color_target_blit = rec_cblit;
    be.depth_target_create_ex = rec_dcreate_ex;
    g_stub_be_override = &be;
    engine_up();
    memset(&g_copy, 0, sizeof g_copy);
    memset(&g_blit, 0, sizeof g_blit);
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    m(M_COLOR_MASK, 0u);
    m(M_CLEAR_BUFFERS, 0x03u);                  /* depth + stencil */
    const u32 z0 = g_zcreated;

    /* depth_float flips the host format: a new surface inheriting the old
     * one's depth 1:1 (same bpp), which is released only afterwards. */
    m(0x03B0, 1u << 12);
    m(M_CLEAR_BUFFERS, 0x03u);
    const u32 z1 = g_zcreated;
    CHECK(z1 != z0 && g_copy.n == 1 && g_copy.src == z0 && g_copy.dst == z1 &&
          g_copy.sw == 256 && g_copy.dw == 256 && g_copy.sh == 256 && !stub.alive[z0],
          "a zeta format change inherits the old depth (%d copies, %ux%u -> %ux%u)",
          g_copy.n, g_copy.sw, g_copy.sh, g_copy.dw, g_copy.dh);

    /* Z16 at the same pitch: normalized bytes, so Z16 samples take the
     * Z24S8 ones at transfer_scale_x = src_bpp / dst_bpp. Exact matching
     * (RSX_ENG_ZETA_EXACT): the new zeta is its own 256 texels (512 bytes)
     * wide, inheriting the first 128 Z24S8 texels. */
    m(M_SURFACE_FORMAT, 5u | (1u << 5) | (1u << 8));
    m(M_CLEAR_BUFFERS, 0x03u);
    CHECK(g_copy.n == 2 && g_copy.src == z1 && g_copy.sw == 128 && g_copy.dw == 256,
          "a Z24S8 -> Z16 rebind scales x by bpp (%u -> %u)", g_copy.sw, g_copy.dw);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    m(M_CLEAR_BUFFERS, 0x03u);
    const u32 za = g_zcreated;

    /* A zeta at another address over the same memory and pitch: the newer
     * one's overlapping rows are inherited; another pitch is not. */
    int nc = g_copy.n;
    m(M_ZETA_OFFSET, ZETA_OFFSET + 1024u * 128u);
    m(M_CLEAR_BUFFERS, 0x03u);
    CHECK(g_copy.n == nc + 1 && g_copy.src == za && g_copy.sy == 128 && g_copy.dy == 0 &&
          g_copy.sh == 128 && g_copy.sw == 256,
          "an overlapping same-pitch zeta inherits the shared rows (sy %u, %u rows)",
          g_copy.sy, g_copy.sh);
    nc = g_copy.n;
    m(M_ZETA_PITCH, 2048u);
    m(M_ZETA_OFFSET, ZETA_OFFSET + 1024u * 64u);
    m(M_CLEAR_BUFFERS, 0x03u);
    CHECK(g_copy.n == nc, "one of another pitch inherits nothing");
    m(M_ZETA_PITCH, 1024u);
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_COLOR_MASK, 0x01010101u);

    /* Colour: a new surface 64 rows into a newer one at the same pitch. */
    draw_triangle();
    const u32 c0 = stub.bound_rt[0];
    int nb = g_blit.n;
    m(M_COLOR_A_OFFSET, 0x40000u + 1024u * 64u);
    draw_triangle();
    CHECK(g_blit.n == nb + 1 && g_blit.src == c0 && g_blit.sy == 64 && g_blit.dy == 0 &&
          g_blit.sh == 192 && g_blit.sw == 256 && g_blit.dw == 256,
          "an overlapping same-pitch colour surface is inherited (sy %u, %u rows)",
          g_blit.sy, g_blit.sh);
    /* Back at 0x40000 the newer surface at +64 rows flows back in. */
    nb = g_blit.n;
    m(M_COLOR_A_OFFSET, 0x40000u);
    draw_triangle();
    CHECK(g_blit.n == nb + 1 && g_blit.dst == c0 && g_blit.dy == 64,
          "and the older one takes the newer rows back on its rebind (dy %u)", g_blit.dy);
    /* A CPU write inside both: the newest contents (0x40000, row 100) take
     * it, not the closest base (0x50000, row 36). */
    const int nw = g_written.n;
    rsx_draw_engine_guest_wrote(RSX_LOCATION_LOCAL, 0x40000u + 1024u * 100u, 1024u, 4u, 1u);
    CHECK(g_written.n == nw + 1 && g_written.surface == c0 && g_written.y == 100,
          "a guest write over overlapping surfaces lands in the newest (y %u)", g_written.y);

    /* 2x1 AA then no AA at one address and pitch: RPCS3 copies the sample
     * rows byte-aligned (the left half of the image), it does not resolve. */
    m(M_COLOR_A_OFFSET, 0x200000u);
    m(M_SURFACE_CLIP_H, 128u << 16);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8) | (3u << 12));
    draw_triangle();
    const u32 aa = stub.bound_rt[0];
    nb = g_blit.n;
    m(M_SURFACE_CLIP_H, 256u << 16);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    draw_triangle();
    CHECK(g_blit.n == nb + 1 && g_blit.src == aa && g_blit.sw == 256 && g_blit.dw == 256,
          "an AA -> non-AA rebind copies sample rows 1:1 (%u -> %u)", g_blit.sw, g_blit.dw);

    /* R2 0x880000: AA 1280 px (2560 samples) then non-AA 1280 px at the same
     * pitch. RPCS3 (get_transferable_region + transform_pixels_to_samples)
     * copies sample columns 0..1279 only, i.e. the picture's left half. */
    m(M_COLOR_A_OFFSET, 0x300000u);
    m(M_SURFACE_CLIP_H, 128u << 16);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8) | (3u << 12));
    draw_triangle();
    const u32 aa2 = stub.bound_rt[0];
    nb = g_blit.n;
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    draw_triangle();
    const u32 plain = stub.bound_rt[0];
    CHECK(g_blit.n == nb + 1 && g_blit.src == aa2 && g_blit.dst == plain && g_blit.sx == 0 &&
          g_blit.sw == 128 && g_blit.dx == 0 && g_blit.dw == 128 && g_blit.sh == g_blit.dh,
          "R2 AA -> half-width non-AA takes sample columns 0..127 1:1 (sx %u sw %u -> dw %u)",
          g_blit.sx, g_blit.sw, g_blit.dw);
    /* ...and back: the non-AA pixels land 1:1 in AA sample columns 0..127. */
    nb = g_blit.n;
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8) | (3u << 12));
    draw_triangle();
    CHECK(g_blit.n == nb + 1 && g_blit.src == plain && g_blit.sw == 128 && g_blit.dx == 0 &&
          g_blit.dw == 128,
          "R2 non-AA -> AA rebind fills sample columns 0..127 1:1 (sw %u -> dw %u)",
          g_blit.sw, g_blit.dw);
    engine_down();
    g_stub_be_override = NULL;
}

/* RPCS3 memory_barrier's copy_image_typeless: colour <-> depth and depth
 * formats that are not bitcast-compatible inherit each other's guest words;
 * and bind_surface_address's unimplemented convert_pitch: a same-size rebind
 * at a new pitch drops the pixels. */
static struct { int n; u32 src, sk, sx, sy, sw, sh, dst, dk, dx, dy, dw, dh; } g_tl;
static int rec_typeless(void* u, u32 s, u32 sk, u32 sx, u32 sy, u32 sw, u32 sh,
                        u32 d, u32 dk, u32 dx, u32 dy, u32 dw, u32 dh)
{
    (void)u;
    g_tl.n++; g_tl.src = s; g_tl.sk = sk; g_tl.sx = sx; g_tl.sy = sy; g_tl.sw = sw; g_tl.sh = sh;
    g_tl.dst = d; g_tl.dk = dk; g_tl.dx = dx; g_tl.dy = dy; g_tl.dw = dw; g_tl.dh = dh;
    return 1;
}

static void test_typeless_surfaces(void)
{
    printf("-- RPCS3 typeless surface copies and pitch discard\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.depth_target_copy = rec_dcopy;
    be.color_target_blit = rec_cblit;
    be.depth_target_create_ex = rec_dcreate_ex;
    be.surface_typeless_copy = rec_typeless;
    g_stub_be_override = &be;
    engine_up();
    memset(&g_copy, 0, sizeof g_copy);
    memset(&g_blit, 0, sizeof g_blit);
    memset(&g_tl, 0, sizeof g_tl);
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    m(M_COLOR_MASK, 0u);
    m(M_CLEAR_BUFFERS, 0x03u);
    const u32 z0 = g_zcreated;

    /* Z24 fixed -> float: other depth-float classes, so not bitcast-
     * compatible: the raw d24 bits move, no depth_target_copy value copy. */
    m(0x03B0, 1u << 12);
    m(M_CLEAR_BUFFERS, 0x03u);
    const u32 z1 = g_zcreated;
    CHECK(g_copy.n == 0 && g_tl.n == 1 && g_tl.src == z0 && g_tl.dst == z1 &&
          g_tl.sk == RSX_BE_TYPELESS_Z24S8 && g_tl.dk == RSX_BE_TYPELESS_Z24S8_F &&
          g_tl.sw == 256 && g_tl.dw == 256 && g_tl.sh == 256,
          "a Z24 fixed -> float rebind is a typeless copy (%d, kinds %u -> %u)",
          g_tl.n, g_tl.sk, g_tl.dk);

    /* Float Z24S8 -> Z16 fixed: two Z16 texels a word over the new zeta's
     * own 512 bytes a row (exact size). */
    m(0x03B0, 0u);
    m(M_SURFACE_FORMAT, 5u | (1u << 5) | (1u << 8));
    m(M_CLEAR_BUFFERS, 0x03u);
    CHECK(g_copy.n == 0 && g_tl.n == 2 && g_tl.src == z1 &&
          g_tl.sk == RSX_BE_TYPELESS_Z24S8_F && g_tl.dk == RSX_BE_TYPELESS_Z16 &&
          g_tl.sw == 128 && g_tl.dw == 256,
          "a Z24S8 -> Z16 rebind reinterprets the bytes (%u -> %u texels)", g_tl.sw, g_tl.dw);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    m(M_CLEAR_BUFFERS, 0x03u);
    const u32 zz = g_zcreated;

    /* A colour target over the newer zeta's memory takes its words. */
    int nt = g_tl.n;
    m(M_COLOR_MASK, 0x01010101u);
    m(M_COLOR_A_OFFSET, ZETA_OFFSET);
    draw_triangle();
    const u32 col = stub.bound_rt[0];
    CHECK(g_tl.n == nt + 1 && g_tl.src == zz && g_tl.dst == col &&
          g_tl.sk == RSX_BE_TYPELESS_Z24S8 && g_tl.dk == RSX_BE_TYPELESS_ARGB8 &&
          g_tl.sw == 256 && g_tl.dw == 256,
          "colour bound over a newer zeta inherits it typeless (%d, kinds %u -> %u)",
          g_tl.n - nt, g_tl.sk, g_tl.dk);

    /* ...and the zeta, rebound after the colour wrote, takes the colour's. */
    nt = g_tl.n;
    m(M_COLOR_A_OFFSET, 0x40000u);
    m(M_COLOR_MASK, 0u);
    m(M_CLEAR_BUFFERS, 0x03u);
    CHECK(g_tl.n == nt + 1 && g_tl.src == col && g_tl.dst == zz &&
          g_tl.sk == RSX_BE_TYPELESS_ARGB8 && g_tl.dk == RSX_BE_TYPELESS_Z24S8,
          "a zeta rebound under newer colour inherits it typeless (%d, kinds %u -> %u)",
          g_tl.n - nt, g_tl.sk, g_tl.dk);

    /* Same size, new pitch: the pixels go (cleared), the surface stays. */
    m(M_COLOR_MASK, 0x01010101u);
    m(M_COLOR_A_OFFSET, 0x200000u);
    draw_triangle();
    const u32 c = stub.bound_rt[0];
    const int cc = stub.n_clear_color, nb = g_blit.n;
    nt = g_tl.n;
    m(M_COLOR_A_PITCH, 2048u);
    draw_triangle();
    CHECK(stub.bound_rt[0] == c && stub.n_clear_color == cc + 1 &&
          (cc >= (int)STUB_MAX_CLEARED || stub.cleared[cc] == c) && g_blit.n == nb &&
          g_tl.n == nt,
          "a same-size rebind at a new pitch discards the pixels (%d clears, %d copies)",
          stub.n_clear_color - cc, g_blit.n - nb);
    m(M_COLOR_A_PITCH, 1024u);

    /* The zeta likewise: depth 1.0 / stencil 255 before the clear. */
    m(M_COLOR_MASK, 0u);
    const int ds = stub.n_clear_ds, zc = stub.n_depth_create;
    m(M_ZETA_PITCH, 2048u);
    m(M_CLEAR_BUFFERS, 0x02u);                  /* stencil only */
    CHECK(stub.n_depth_create == zc && stub.n_clear_ds >= ds + 1,
          "a zeta rebound at a new pitch is erased in place (%d clears)",
          stub.n_clear_ds - ds);
    m(M_ZETA_PITCH, 1024u);
    engine_down();
    g_stub_be_override = NULL;
}

/* RPCS3 upload_scaled_image out of a depth surface: R2 point-samples its
 * Z24S8 scene zeta into a quarter-size A8R8G8B8 destination that its
 * low-res particle pass then binds as zeta. Typeless to ARGB8 first, then
 * the scaled copy into a destination surface, which the zeta inherits. */
static void test_blit_depth_source(void)
{
    printf("-- NV3089 blit out of a depth surface (typeless, then scaled)\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.depth_target_copy = rec_dcopy;
    be.color_target_blit = rec_cblit;
    be.color_target_blit_filtered = rec_cblitf;
    be.depth_target_create_ex = rec_dcreate_ex;
    be.surface_typeless_copy = rec_typeless;
    g_stub_be_override = &be;
    engine_up();
    memset(&g_blit, 0, sizeof g_blit);
    memset(&g_tl, 0, sizeof g_tl);
    const u32 zoff = 0x00100000u, doff = 0x00300000u;
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_ZETA_OFFSET, zoff);
    m(M_ZETA_PITCH, 1024u);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    m(M_COLOR_MASK, 0u);
    m(M_CLEAR_BUFFERS, 0x03u);
    const u32 scene_z = g_zcreated;

    const int ok = rsx_draw_engine_blit(RSX_LOCATION_LOCAL, zoff, 1024u, 0, 0, 4u << 20, 4u << 20,
                                        RSX_LOCATION_LOCAL, doff, 256u, 0, 0, 64, 64, 0, 0);
    CHECK(ok && g_tl.n == 1 && g_tl.src == scene_z && g_tl.sk == RSX_BE_TYPELESS_Z24S8 &&
          g_tl.dk == RSX_BE_TYPELESS_ARGB8 && g_tl.sw == 256 && g_tl.sh == 256,
          "the zeta is read typeless as ARGB8 at source size (ok %d, %d copies, %ux%u)",
          ok, g_tl.n, g_tl.sw, g_tl.sh);
    CHECK(g_blit.n == 1 && g_blit.src == g_tl.dst && g_blit.sw == 256 && g_blit.sh == 256 &&
          g_blit.dw == 64 && g_blit.dh == 64 && !g_blit_lin,
          "then point-scaled 256 -> 64 into the destination (%ux%u -> %ux%u)",
          g_blit.sw, g_blit.sh, g_blit.dw, g_blit.dh);
    const u32 dst_col = g_blit.dst;

    /* The particle pass binds the destination as its zeta: it inherits the
     * blitted words typeless instead of starting at depth 1.0. */
    const int nt = g_tl.n;
    m(M_SURFACE_CLIP_H, 64u << 16);
    m(M_SURFACE_CLIP_V, 64u << 16);
    m(M_ZETA_OFFSET, doff);
    m(M_ZETA_PITCH, 256u);
    m(M_CLEAR_BUFFERS, 0x02u);
    CHECK(g_tl.n == nt + 1 && g_tl.src == dst_col && g_tl.sk == RSX_BE_TYPELESS_ARGB8 &&
          g_tl.dk == RSX_BE_TYPELESS_Z24S8,
          "a zeta bound over the destination inherits it (%d, kinds %u -> %u)",
          g_tl.n - nt, g_tl.sk, g_tl.dk);
    engine_down();
    g_stub_be_override = NULL;
}

/* RPCS3 create_blit_dst: a render-target source copied into local memory
 * no surface holds gets a destination surface at the block's base, pitch /
 * 4 wide, the rectangle at (dst_x, dst_y) -- not a CPU copy. */
/* RPCS3 apply_pixel_transforms + VKTexture blit flip: a mirrored NV3089
 * transfer runs on the GPU, reading the source span that ends at the start
 * texel, flipped; without the backend hook it stays on the CPU. */
static u32 g_blit_flip;
static void rec_cblit_flip(void* u, u32 s, u32 sx, u32 sy, u32 sw, u32 sh,
                           u32 d, u32 dx, u32 dy, u32 dw, u32 dh, int linear, u32 flip)
{
    rec_cblitf(u, s, sx, sy, sw, sh, d, dx, dy, dw, dh, linear);
    g_blit_flip = flip;
}

static void test_blit_mirror(void)
{
    printf("-- mirrored NV3089 blit on the GPU\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.color_target_blit = rec_cblit;
    be.color_target_blit_filtered = rec_cblitf;
    g_stub_be_override = &be;
    engine_up();
    memset(&g_blit, 0, sizeof g_blit);
    draw_triangle();
    const u32 scene = stub.bound_rt[0];
    int ok = rsx_draw_engine_blit_flip(RSX_LOCATION_LOCAL, 0x40000u, 1024u, 0, 63, 1u << 20, 1u << 20,
                                       RSX_LOCATION_LOCAL, 0x300000u, 512u, 0, 0, 32, 32, 0, 0, 4u, 2u);
    CHECK(!ok && g_blit.n == 0, "no flip hook: the mirrored copy stays on the CPU (ok %d)", ok);
    be.color_target_blit_flip = rec_cblit_flip;
    ok = rsx_draw_engine_blit_flip(RSX_LOCATION_LOCAL, 0x40000u, 1024u, 0, 63, 1u << 20, 1u << 20,
                                   RSX_LOCATION_LOCAL, 0x300000u, 512u, 0, 0, 32, 32, 0, 0, 4u, 2u);
    CHECK(ok && g_blit.n == 1 && g_blit.src == scene && g_blit.sy == 32 && g_blit.sh == 32 &&
          g_blit_flip == 2u,
          "flipped in Y: rows 32..63 read bottom-up (ok %d, %d blits, sy %u, flip %u)",
          ok, g_blit.n, g_blit.sy, g_blit_flip);
    ok = rsx_draw_engine_blit_flip(RSX_LOCATION_LOCAL, 0x40000u, 1024u, 10, 0, 1u << 20, 1u << 20,
                                   RSX_LOCATION_LOCAL, 0x300000u, 512u, 0, 0, 32, 32, 0, 0, 4u, 1u);
    CHECK(!ok && g_blit.n == 1, "a mirrored span running off the image falls back (ok %d)", ok);
    engine_down();
    g_stub_be_override = NULL;
}

static void test_blit_creates_offset_target(void)
{
    printf("-- NV3089 blit of a render target into a fresh offset rectangle\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.color_target_blit = rec_cblit;
    be.color_target_blit_filtered = rec_cblitf;
    g_stub_be_override = &be;
    engine_up();
    memset(&g_blit, 0, sizeof g_blit);
    draw_triangle();
    const u32 scene = stub.bound_rt[0];
    const u32 doff = 0x00300000u;
    const int ok = rsx_draw_engine_blit(RSX_LOCATION_LOCAL, 0x40000u, 1024u, 0, 0, 1u << 20, 1u << 20,
                                        RSX_LOCATION_LOCAL, doff, 512u, 16, 8, 32, 32, 0, 0);
    CHECK(ok && g_blit.n == 1 && g_blit.src == scene && g_blit.dx == 16 && g_blit.dy == 8 &&
          g_blit.dw == 32 && g_blit.dh == 32 && stub.color_w == 128 && stub.color_h == 40,
          "the copy lands at 16,8 of a 128x40 target (ok %d, %d blits, at %u,%u, target %ux%u)",
          ok, g_blit.n, g_blit.dx, g_blit.dy, stub.color_w, stub.color_h);
    engine_down();
    g_stub_be_override = NULL;
}

/* T2-10: an R5G6B5 source blitted into free local memory creates a native
 * R5G6B5 destination; S2-07: a created destination stops above a newer
 * surface of another pitch. */
static void test_blit_create_16_and_trunc(void)
{
    printf("-- NV3089 blit target creation: R5G6B5, pitch truncation\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.color_target_blit = rec_cblit;
    be.color_target_blit_filtered = rec_cblitf;
    be.color_target_create = rec_ccreate_fmt;
    be.color_format_supported = rec_fmt_supported;
    g_native_mask = 1u << RSX_BE_FMT_R5G6B5;
    g_stub_be_override = &be;
    engine_up();
    memset(&g_blit, 0, sizeof g_blit);
    m(M_SURFACE_FORMAT, 3u | (2u << 5) | (1u << 8));
    m(0x020C, 512u);
    draw_triangle();
    const u32 scene = stub.bound_rt[0];
    g_ccreate_fmt = RSX_BE_FMT_NONE;
    int ok = rsx_draw_engine_blit_bpp(RSX_LOCATION_LOCAL, 0x40000u, 512u, 0, 0, 1u << 20, 1u << 20,
                                      RSX_LOCATION_LOCAL, 0x300000u, 64u, 0, 0, 32, 32, 0, 0, 2u);
    CHECK(ok && g_blit.n == 1 && g_blit.src == scene && g_ccreate_fmt == RSX_BE_FMT_R5G6B5 &&
          stub.color_w == 32 && stub.color_h == 32,
          "a 16-bit copy makes a 32x32 R5G6B5 target (ok %d, %d blits, fmt %d, %ux%u)",
          ok, g_blit.n, (int)g_ccreate_fmt, stub.color_w, stub.color_h);
    ok = rsx_draw_engine_blit_bpp(RSX_LOCATION_LOCAL, 0x40000u, 512u, 0, 0, 1u << 20, 1u << 20,
                                  RSX_LOCATION_LOCAL, 0x380000u, 64u, 4, 0, 16, 16, 0, 0, 2u);
    CHECK(!ok && g_blit.n == 1, "a partial 16-bit write creates nothing (ok %d)", ok);
    engine_down();

    engine_up();
    memset(&g_blit, 0, sizeof g_blit);
    draw_triangle();
    m(M_COLOR_A_OFFSET, 0x300000u + 100u * 1024u);
    m(0x020C, 2048u);
    draw_triangle();
    ok = rsx_draw_engine_blit(RSX_LOCATION_LOCAL, 0x40000u, 1024u, 0, 0, 1u << 20, 1u << 20,
                              RSX_LOCATION_LOCAL, 0x300000u, 1024u, 0, 0, 32, 32, 0, 0);
    CHECK(ok && g_blit.n == 1 && stub.color_w == 256 && stub.color_h == 100,
          "the 256-row target stops at the newer pitch-2048 surface (ok %d, %ux%u)",
          ok, stub.color_w, stub.color_h);
    engine_down();
    be.color_format_supported = NULL;
    g_stub_be_override = NULL;
}

/* RSX_ENG_TILE_AA_COMPRESS (default on): inside a bound 2x1/2x2 colour-
 * compressed tile an AA <-> non-AA rebind at one address and pitch resolves
 * or replicates pixel for pixel, and a pixel-extent texture of the AA surface
 * reads the resolved picture. */
static void test_tile_aa_compress(void)
{
    printf("-- compressed AA tile (cellGcmSetTile comp 7)\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.color_target_blit = rec_cblit;
    be.color_target_blit_filtered = rec_cblitf;
    be.color_region_snapshot = rec_region;
    g_stub_be_override = &be;
    engine_up();
    memset(&g_blit, 0, sizeof g_blit);
    memset(&g_region, 0, sizeof g_region);
    rsx_draw_engine_set_tile(1, RSX_LOCATION_LOCAL, 0x400000u, 0x100000u, 1024u, 7u, 0, 1);
    m(M_COLOR_MASK, 0x01010101u);

    /* 2x1 AA, 128 px (256 samples) wide, then the same pixels without AA. */
    m(M_COLOR_A_OFFSET, 0x400000u);
    m(M_SURFACE_CLIP_H, 128u << 16);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8) | (3u << 12));
    draw_triangle();
    const u32 aa = stub.bound_rt[0];
    int nb = g_blit.n;
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    draw_triangle();
    const u32 plain = stub.bound_rt[0];
    CHECK(g_blit.n == nb + 1 && g_blit.src == aa && g_blit.dst == plain && g_blit.sw == 256 &&
          g_blit.dw == 128 && g_blit.sh == 256 && g_blit.dh == 256 && g_blit_lin == 1,
          "AA -> non-AA resolves the whole picture (%u -> %u, linear %d)",
          g_blit.sw, g_blit.dw, g_blit_lin);
    nb = g_blit.n;
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8) | (3u << 12));
    draw_triangle();
    CHECK(g_blit.n == nb + 1 && g_blit.src == plain && g_blit.dst == aa && g_blit.sw == 128 &&
          g_blit.dw == 256 && g_blit_lin == 0,
          "non-AA -> AA replicates each pixel to its samples (%u -> %u, linear %d)",
          g_blit.sw, g_blit.dw, g_blit_lin);

    /* A 128x256 texture of the AA surface: the resolved picture. */
    m(M_COLOR_A_OFFSET, 0x40000u);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    bind_texture_unit(RSX_LOCATION_LOCAL, 0x400000u);
    m(0x1840 /* control3: pitch 0 (the reset image holds 8) */, 0u);
    m(M_TEX_UNIT + 0x18, (128u << 16) | 256u);
    nb = g_blit.n;
    draw_triangle();
    CHECK(g_blit.n == nb + 1 && g_blit.src == aa && g_blit.sw == 256 && g_blit.dw == 128 &&
          g_blit_lin == 1 && g_region.n == 1 && g_region.src == g_blit.dst &&
          g_region.w == 128 && stub.bound_tex[0] == g_region.ret,
          "a pixel-extent texture of the AA surface samples its resolve (%u -> %u)",
          g_blit.sw, g_blit.dw);

    /* Outside the tile (or under a tile of another pitch): RPCS3's
     * byte-aligned sample copy. */
    m(M_TEX_UNIT + 0x0C, 0u);
    rsx_draw_engine_set_tile(2, RSX_LOCATION_LOCAL, 0x600000u, 0x100000u, 2048u, 7u, 0, 1);
    m(M_COLOR_A_OFFSET, 0x600000u);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8) | (3u << 12));
    draw_triangle();
    nb = g_blit.n;
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    draw_triangle();
    CHECK(g_blit.n == nb + 1 && g_blit.sw == 128 && g_blit.dw == 128 && g_blit_lin == 0,
          "a tile of another pitch keeps RPCS3's byte-aligned copy (%u -> %u)",
          g_blit.sw, g_blit.dw);
    rsx_draw_engine_set_tile(1, 0, 0, 0, 0, 0, 0, 0);
    rsx_draw_engine_set_tile(2, 0, 0, 0, 0, 0, 0, 0);
    engine_down();
    g_stub_be_override = NULL;
}

static u32 rec_snapshot(void* u, u32 s) { (void)u; (void)s; return stub_handle(); }

/* A texture reaching past the surface at its address: RPCS3 atlas_gather
 * (never a dropped draw); inside a compressed AA tile the sample-grid view of
 * a non-AA picture replicates each pixel. Plus RPCS3's memory tag: rewritten
 * guest memory under a zeta stops it from being inherited. */
static void test_feedback_gather(void)
{
    printf("-- texture gather, feedback fallback, memory tags\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.color_target_blit = rec_cblit;
    be.color_target_blit_filtered = rec_cblitf;
    be.color_region_snapshot = rec_region;
    be.depth_target_create_ex = rec_dcreate_ex;
    be.surface_typeless_copy = rec_typeless;
    be.color_snapshot = rec_snapshot;
    g_stub_be_override = &be;
    engine_up();
    memset(&g_blit, 0, sizeof g_blit);
    memset(&g_region, 0, sizeof g_region);
    memset(&g_tl, 0, sizeof g_tl);
    m(M_COLOR_MASK, 0x01010101u);

    /* R2 menu: drawing the non-AA 128-px view of a comp-7 tile while
     * sampling it as the 256-sample grid. */
    rsx_draw_engine_set_tile(1, RSX_LOCATION_LOCAL, 0x400000u, 0x100000u, 1024u, 7u, 0, 1);
    m(M_COLOR_A_OFFSET, 0x400000u);
    m(M_SURFACE_CLIP_H, 128u << 16);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    draw_triangle();
    const u32 plain = stub.bound_rt[0];
    bind_texture_unit(RSX_LOCATION_LOCAL, 0x400000u);
    m(M_TEX_UNIT + 0x18, (256u << 16) | 256u);
    m(0x1840, 1024u);
    int nd = stub.n_draw, nb = g_blit.n;
    draw_triangle();
    CHECK(stub.n_draw == nd + 1 && g_blit.n == nb + 1 && g_blit.src == plain &&
          g_blit.sw == 128 && g_blit.dw == 256 && g_blit.sh == 256 && g_blit.dh == 256 &&
          g_blit_lin == 0 && g_region.w == 256 && stub.bound_tex[0] == g_region.ret,
          "a tile's sample-grid read of its non-AA target replicates pixels (%d draws, %u -> %u)",
          stub.n_draw - nd, g_blit.sw, g_blit.dw);
    rsx_draw_engine_set_tile(1, 0, 0, 0, 0, 0, 0, 0);

    /* No tile: the AA sibling (older, all 256 sample columns) then the
     * non-AA surface (newer, columns 0..127), at their byte positions. */
    m(M_TEX_UNIT + 0x0C, 0u);
    m(M_COLOR_A_OFFSET, 0x600000u);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8) | (3u << 12));
    draw_triangle();
    const u32 aa = stub.bound_rt[0];
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    draw_triangle();
    const u32 plain2 = stub.bound_rt[0];
    bind_texture_unit(RSX_LOCATION_LOCAL, 0x600000u);
    m(M_TEX_UNIT + 0x18, (256u << 16) | 256u);
    m(0x1840, 1024u);
    nd = stub.n_draw; nb = g_blit.n;
    draw_triangle();
    CHECK(stub.n_draw == nd + 1 && g_blit.n == nb + 2 && g_blit.src == plain2 &&
          g_blit.sw == 128 && g_blit.dw == 128 && g_blit.dx == 0 && g_region.w == 256 &&
          stub.bound_tex[0] == g_region.ret && aa != plain2,
          "outside a tile it gathers oldest to newest 1:1 (%d draws, %d copies)",
          stub.n_draw - nd, g_blit.n - nb);

    /* A read the framebuffer copy cannot serve (another pitch) goes through
     * guest memory instead of dropping the draw. */
    m(M_TEX_UNIT + 0x18, (4u << 16) | 4u);
    m(0x1840, 2048u);
    nd = stub.n_draw;
    draw_triangle();
    CHECK(stub.n_draw == nd + 1 && stub.bound_tex[0] != 0,
          "an incompatible feedback read falls back to guest memory (%d draws)",
          stub.n_draw - nd);
    m(M_TEX_UNIT + 0x0C, 0u);

    /* Memory tags: a zeta cleared, then its first word rewritten in guest
     * memory -- a colour bound over it no longer inherits it. */
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_SURFACE_CLIP_H, 256u << 16);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    m(M_COLOR_A_OFFSET, 0x40000u);
    m(M_COLOR_MASK, 0u);
    m(M_CLEAR_BUFFERS, 0x03u);
    vm_base[GUEST_LOCAL_EA + ZETA_OFFSET] ^= 0x5Au;
    int nt = g_tl.n;
    m(M_COLOR_MASK, 0x01010101u);
    m(M_COLOR_A_OFFSET, ZETA_OFFSET);
    draw_triangle();
    CHECK(g_tl.n == nt, "a zeta whose memory changed since its write is not inherited (%d)",
          g_tl.n - nt);
    /* Untouched memory: the next zeta write is inherited by a fresh colour. */
    m(M_COLOR_A_OFFSET, 0x40000u);
    m(M_COLOR_MASK, 0u);
    m(M_ZETA_OFFSET, ZETA_OFFSET + 0x80000u);
    m(M_CLEAR_BUFFERS, 0x03u);
    nt = g_tl.n;
    m(M_COLOR_MASK, 0x01010101u);
    m(M_COLOR_A_OFFSET, ZETA_OFFSET + 0x80000u);
    draw_triangle();
    CHECK(g_tl.n == nt + 1, "with its memory intact it is (%d)", g_tl.n - nt);
    engine_down();
    g_stub_be_override = NULL;
}

/* RPCS3 get_merged_texture_memory_region: a surface whose memory tag fails
 * when a texture reads its range is invalidated (unless bound); the texture
 * reads guest memory and a stale zeta is never stored over newer bytes. */
static void test_texture_tag_drop(void)
{
    printf("-- texture lookup drops tag-failing surfaces\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.color_snapshot = rec_snapshot;
    be.depth_target_create_ex = rec_dcreate_ex;
    g_stub_be_override = &be;
    engine_up();
    m(M_COLOR_MASK, 0x01010101u);
    draw_triangle();                 /* slot 0: the (never-presented) pinned one */
    m(M_COLOR_A_OFFSET, 0x500000u);
    draw_triangle();
    const u32 old = stub.bound_rt[0];
    m(M_COLOR_A_OFFSET, 0x40000u);
    draw_triangle();
    bind_texture_unit(RSX_LOCATION_LOCAL, 0x500000u);
    m(0x1840, 1024u);
    draw_triangle();
    CHECK(stub.alive[old], "an intact surface stays sampleable");
    vm_base[GUEST_LOCAL_EA + 0x500000u] ^= 0x5Au;
    draw_triangle();
    CHECK(!stub.alive[old] && stub.bound_tex[0] != old,
          "a surface whose memory changed is dropped and memory is read");

    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_DEPTH_TEST, 1u);
    m(M_DEPTH_WRITE, 1u);
    m(M_TEX_UNIT + 0x0C, 0u);
    draw_triangle();
    const u32 zh = stub.bound_depth;
    m(M_ZETA_OFFSET, ZETA_OFFSET + 0x80000u);
    draw_triangle();
    vm_base[GUEST_LOCAL_EA + ZETA_OFFSET] = 0x77u;
    const int nr = stub.n_depth_readback;
    bind_texture_unit(RSX_LOCATION_LOCAL, ZETA_OFFSET);
    draw_triangle();
    CHECK(zh && !stub.alive[zh] && stub.n_depth_readback == nr &&
          vm_base[GUEST_LOCAL_EA + ZETA_OFFSET] == 0x77u,
          "a stale zeta is dropped, not stored over the CPU's bytes (readbacks %d)",
          stub.n_depth_readback - nr);
    m(M_TEX_UNIT + 0x0C, 0u);
    engine_down();
    g_stub_be_override = NULL;
}

/* RPCS3 memory_barrier: a rebound surface whose memory changed, with every
 * overlapping newer source stale too, is erased instead of keeping its frame. */
static void test_stale_erase(void)
{
    printf("-- stale rebind with stale sources is erased\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.depth_target_create_ex = rec_dcreate_ex;
    be.depth_target_copy = rec_dcopy;
    be.color_target_blit = rec_cblit;
    g_stub_be_override = &be;
    engine_up();
    m(M_COLOR_MASK, 0x01010101u);
    draw_triangle();                                  /* slot 0 (pinned) */
    m(M_COLOR_A_OFFSET, 0x500000u);
    draw_triangle();
    const u32 a = stub.bound_rt[0];
    m(M_SURFACE_CLIP_V, 64u << 16);
    m(M_COLOR_A_OFFSET, 0x500000u + 1024u * 64u);
    draw_triangle();
    vm_base[GUEST_LOCAL_EA + 0x500000u] ^= 0x5Au;
    vm_base[GUEST_LOCAL_EA + 0x500000u + 1024u * 64u] ^= 0x5Au;
    m(M_SURFACE_CLIP_V, 256u << 16);
    m(M_COLOR_A_OFFSET, 0x500000u);
    const int nc = stub.n_clear_color;
    draw_triangle();
    CHECK(stub.bound_rt[0] == a && stub.n_clear_color == nc + 1 &&
          stub.last_clear_rgba[0] == 0.0f && stub.last_clear_rgba[3] == 1.0f,
          "colour: erased to (0,0,0,1) (%d clears)", stub.n_clear_color - nc);

    m(M_COLOR_A_OFFSET, 0x700000u);
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_DEPTH_TEST, 1u);
    m(M_DEPTH_WRITE, 1u);
    draw_triangle();
    const u32 z = stub.bound_depth;
    m(M_SURFACE_CLIP_V, 64u << 16);
    m(M_COLOR_A_OFFSET, 0x780000u);
    m(M_ZETA_OFFSET, ZETA_OFFSET + 1024u * 64u);
    draw_triangle();
    vm_base[GUEST_LOCAL_EA + ZETA_OFFSET] ^= 0x5Au;
    vm_base[GUEST_LOCAL_EA + ZETA_OFFSET + 1024u * 64u] ^= 0x5Au;
    m(M_SURFACE_CLIP_V, 256u << 16);
    m(M_COLOR_A_OFFSET, 0x700000u);
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    const int nd = stub.n_clear_ds;
    draw_triangle();
    CHECK(stub.bound_depth == z && stub.n_clear_ds == nd + 1 && stub.last_clear_depth == 1.0f,
          "zeta: erased to depth 1.0 (%d clears)", stub.n_clear_ds - nd);
    engine_down();
    g_stub_be_override = NULL;
}

/* RPCS3 split_surface_region / superseded_surfaces: a surface replaced at its
 * address by a narrower one keeps the memory beyond it as a clone. */
static void test_surface_split(void)
{
    printf("-- RPCS3 split_surface_region\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.depth_target_copy = rec_dcopy;
    be.color_target_blit = rec_cblit;
    be.depth_target_create_ex = rec_dcreate_ex;
    be.surface_typeless_copy = rec_typeless;
    g_stub_be_override = &be;
    engine_up();
    memset(&g_blit, 0, sizeof g_blit);
    memset(&g_tl, 0, sizeof g_tl);
    m(M_SURFACE_CLIP_H, 256u << 16);
    m(M_COLOR_MASK, 0x01010101u);
    m(M_COLOR_A_OFFSET, 0x600000u);
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    draw_triangle();
    const u32 old = stub.bound_rt[0];

    /* ARGB8 -> RGB565 at 256 px: 512 of the 1024 bytes a row stay; bytes
     * 512..1023 (old columns 128..255) become a clone at +512. */
    int nb = g_blit.n, nt = g_tl.n;
    m(M_SURFACE_FORMAT, 3u | (2u << 5) | (1u << 8));
    draw_triangle();
    const u32 clone = g_blit.dst;
    CHECK(g_tl.n == nt + 1 && g_tl.sk == RSX_BE_TYPELESS_ARGB8 &&
          g_tl.dk == RSX_BE_TYPELESS_RGB565 && g_tl.sw == 128 && g_tl.dw == 256,
          "ARGB8 -> RGB565 inherits the shared bytes typeless (%u -> %u texels)",
          g_tl.sw, g_tl.dw);
    CHECK(g_blit.n == nb + 1 && g_blit.src == old && g_blit.sx == 128 && g_blit.sw == 128 &&
          g_blit.sh == 256 && g_blit.dx == 0 && clone != stub.bound_rt[0] && stub.alive[clone] &&
          !stub.alive[old],
          "the replaced surface's remainder lives on as a clone (%d blits, sx %u sw %u)",
          g_blit.n - nb, g_blit.sx, g_blit.sw);

    /* A newer ARGB8 surface at +256 inherits the whole clone: it is freed. */
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    m(M_COLOR_A_OFFSET, 0x600000u + 256u);
    nb = g_blit.n;
    draw_triangle();
    CHECK(g_blit.n >= nb + 1 && !stub.alive[clone],
          "a surface inheriting all of the clone frees it (%d blits)", g_blit.n - nb);

    /* Again, then bound at the clone's own address and size: reused. */
    m(M_COLOR_A_OFFSET, 0x700000u);
    draw_triangle();
    m(M_SURFACE_FORMAT, 3u | (2u << 5) | (1u << 8));
    draw_triangle();
    const u32 clone2 = g_blit.dst;
    m(M_SURFACE_FORMAT, 5u | (2u << 5) | (1u << 8));
    m(M_SURFACE_CLIP_H, 128u << 16);
    m(M_COLOR_A_OFFSET, 0x700000u + 512u);
    draw_triangle();
    CHECK(stub.bound_rt[0] == clone2 && stub.alive[clone2],
          "binding the clone's address reuses it with its pixels");
    m(M_SURFACE_CLIP_H, 256u << 16);
    engine_down();
    g_stub_be_override = NULL;
}

/* RPCS3 tests the raw WINDOW_CLIP registers (reset 0x0FFF0000). */
static void test_window_clip(void)
{
    printf("-- RPCS3 window clip\n");
    engine_up();
    m(M_COLOR_MASK, 0x01010101u);
    int nd = stub.n_draw;
    draw_triangle();
    CHECK(stub.n_draw == nd + 1, "never-written window clip registers draw");
    nd = stub.n_draw;
    m(0x02C0, 0u);
    draw_triangle();
    CHECK(stub.n_draw == nd, "a zero window clip is tested raw and skips the draw");
    m(0x02C0, 0x0FFF0000u);
    nd = stub.n_draw;
    draw_triangle();
    CHECK(stub.n_draw == nd + 1, "the reset value draws again");
    engine_down();
}

/* ---- 8c. the draw clause (RPCS3 NV47/FW/draw_call) ------------------------ */

typedef struct {
    u32 n_begin, n_end, n_ranges;
    u32 first[8], count[8], vtx_off[8], c5x[8];
} ClauseLog;
static ClauseLog g_cl;

static void cl_begin(void* u, const rsx_dispatch* r, u32 p) { (void)u; (void)r; (void)p; g_cl.n_begin++; }
static void cl_end(void* u, const rsx_dispatch* r) { (void)u; (void)r; g_cl.n_end++; }
static void cl_range(void* u, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)u;
    if (g_cl.n_ranges < 8) {
        g_cl.first[g_cl.n_ranges] = first;
        g_cl.count[g_cl.n_ranges] = count;
        g_cl.vtx_off[g_cl.n_ranges] = rsx_dsp_reg(r, 0x1680);
        g_cl.c5x[g_cl.n_ranges] = rsx_dsp_constant(r, 5)[0];
    }
    g_cl.n_ranges++;
}

static void test_flip_reset(void)
{
    printf("-- flip command reset\n");
    static rsx_dispatch d;
    rsx_dispatch_init(&d, NULL);
    rsx_dispatch_method(&d, 0x1D8C, 0u);              /* stencil-only clear value */
    rsx_dispatch_method(&d, 0x0314, 0x03020302u);
    rsx_dispatch_method(&d, 0x0A74, 1u);              /* depth test on */
    rsx_dispatch_method(&d, 0xFEAC, 0u);
    CHECK(rsx_dsp_reg(&d, 0x1D8C) == 0xFFFFFF00u && rsx_dsp_reg(&d, 0x0314) == 0x00010001u &&
          rsx_dsp_reg(&d, 0x0A74) == 0u,
          "GCM_FLIP_COMMAND restores the RPCS3 reset image (zs 0x%08X)",
          rsx_dsp_reg(&d, 0x1D8C));
}

static void test_draw_clause(void)
{
    printf("-- draw clause\n");
    static rsx_dispatch d;
    rsx_dispatch_sink sink;
    memset(&sink, 0, sizeof sink);
    sink.begin = cl_begin; sink.end = cl_end; sink.draw_arrays = cl_range;
    rsx_dispatch_init(&d, &sink);
#define DM(mth, arg) rsx_dispatch_method(&d, (mth), (arg))
#define DRAW(first, count) DM(0x1814, (first) | (((count) - 1u) << 24))

    /* Contiguous batches merge into one sub-draw; a repeat is dropped. */
    memset(&g_cl, 0, sizeof g_cl);
    DM(0x1808, RSX_PRIMITIVE_TRIANGLES);
    DRAW(0, 3); DRAW(3, 3); DRAW(0, 6);
    DM(0x1808, 0);
    CHECK(g_cl.n_ranges == 1 && g_cl.first[0] == 0 && g_cl.count[0] == 6 &&
          g_cl.n_begin == 1 && g_cl.n_end == 1,
          "contiguous ranges merge, a duplicate is dropped (%u ranges, count %u)",
          g_cl.n_ranges, g_cl.count[0]);

    /* Out-of-order ranges are sorted and each is its own sub-draw. */
    memset(&g_cl, 0, sizeof g_cl);
    DM(0x1808, RSX_PRIMITIVE_TRIANGLE_STRIP);
    DRAW(10, 4); DRAW(0, 4);
    DM(0x1808, 0);
    CHECK(g_cl.n_ranges == 2 && g_cl.first[0] == 0 && g_cl.first[1] == 10 &&
          g_cl.n_begin == 2 && g_cl.n_end == 2,
          "disjoint ranges are sorted sub-draws, each begun and ended");

    /* A draw parameter written mid-clause applies from the next range on;
     * the register holds its old value until END. */
    memset(&g_cl, 0, sizeof g_cl);
    DM(0x1680, 0x1000u);
    DM(0x1EFC, 5u);
    DM(0x1F00, 0x3F800000u);                          /* c5.x = 1.0 */
    DM(0x1808, RSX_PRIMITIVE_TRIANGLES);
    DRAW(0, 3);
    DM(0x1680, 0x2000u);
    DM(0x1EFC, 5u);
    DM(0x1F00, 0x40000000u);                          /* c5.x = 2.0 */
    const u32 mid_off = rsx_dsp_reg(&d, 0x1680), mid_c = rsx_dsp_constant(&d, 5)[0];
    DRAW(0, 3);
    DM(0x1808, 0);
    CHECK(mid_off == 0x1000u && mid_c == 0x3F800000u,
          "inside the clause the parameter writes are deferred");
    CHECK(g_cl.n_ranges == 2 && g_cl.vtx_off[0] == 0x1000u && g_cl.vtx_off[1] == 0x2000u &&
          g_cl.c5x[0] == 0x3F800000u && g_cl.c5x[1] == 0x40000000u,
          "the same range after a barrier is redrawn with the new parameters");
    CHECK(rsx_dsp_reg(&d, 0x1680) == 0x2000u, "after END the register has the last value");

    /* A trailing barrier with no range after it still lands. */
    memset(&g_cl, 0, sizeof g_cl);
    DM(0x1808, RSX_PRIMITIVE_TRIANGLES);
    DRAW(0, 3);
    DM(0x1680, 0x3000u);
    DM(0x1808, 0);
    CHECK(g_cl.n_ranges == 1 && rsx_dsp_reg(&d, 0x1680) == 0x3000u,
          "a barrier after the last range applies at END");

    /* Constant writes past the 468th constant are NOPs. */
    DM(0x1EFC, 467u);
    DM(0x1F00 + 4 * 4, 0x12345678u);                   /* c468.x */
    CHECK(rsx_dsp_constant(&d, 468)[0] == 0u, "a write to c468 is dropped");

    /* BEGIN_END reads only its low byte. */
    memset(&g_cl, 0, sizeof g_cl);
    DM(0x1808, 0xFF00u | RSX_PRIMITIVE_TRIANGLES);
    DRAW(0, 3);
    DM(0x1808, 0xAB00u);
    CHECK(g_cl.n_ranges == 1 && g_cl.n_end == 1,
          "BEGIN_END ignores the upper bits (0xAB00 is END)");
#undef DRAW
#undef DM
}

/* ---- 9a. clears by surface format (RPCS3 decode_clear_state) ------------- */

#define M_COLOR_CLEAR_VALUE   0x1D90
#define M_STENCIL_MASK        0x032C
#define M_SHADE_MODE          0x0368

static const rsx_draw_backend* stub_backend_with_clear_rect(void)
{
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.clear_rect = stub_clear_rect;
    return &be;
}

/* RPCS3 hw_SRGB_possible: a fetched A8R8G8B8 whose gamma lanes are exactly
 * R,G,B under the identity crossbar samples through an sRGB view and the
 * shader drops its gamma decode; any other mix keeps the shader decode. */
static char g_srgb_ps[65536];
static int g_srgb_views;
static u32 srgb_capture_pipeline(void* u, const char* vs, const char* ps, const rsx_be_render_state* rs,
                                 const rsx_vertex_layout_plan* l, u32 stride, rsx_be_format f, u32 n)
{
    snprintf(g_srgb_ps, sizeof g_srgb_ps, "%s", ps);
    return stub_pipeline_create(u, vs, ps, rs, l, stride, f, n);
}
static u32 stub_srgb_view(void* u, u32 texture, rsx_be_format f)
{
    (void)u; (void)f;
    if (!texture) return 1;
    g_srgb_views++;
    return 0x7777u;
}

static void test_hw_srgb_view(void)
{
    printf("-- sRGB texture view (hw_SRGB_possible)\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.pipeline_create = srgb_capture_pipeline;
    be.texture_srgb_view = stub_srgb_view;
    g_stub_be_override = &be;
    engine_up();
    u8 vp[32];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 8; i++) { u32 x; memcpy(&x, vp + i * 4, 4); m(0xB80 + i * 4, x); }
    m(0x1EA0, 0);
    rsx_test_fp_instr(vm_base + 0x60000, RSX_TEST_FP_OPC(0x17) | RSX_TEST_FP_MASK_XYZW |
                      RSX_TEST_FP_INSRC_TC0 | RSX_TEST_FP_TEXU(0) | RSX_TEST_FP_END,
                      RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    m(0x8E4, 0x60002);
    m(0x1D60, 0x40);
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);

    m(M_TEX_UNIT + 8, 7u << 20);                   /* gamma R,G,B */
    draw_triangle();
    CHECK(g_srgb_views == 1 && stub.bound_tex[0] == 0x7777u &&
          !strstr(g_srgb_ps, "_rsx_gamma_input("),
          "gamma RGB samples the sRGB view, no shader decode (views %d)", g_srgb_views);

    m(M_TEX_UNIT + 8, 15u << 20);                  /* gamma R,G,B,A */
    draw_triangle();
    CHECK(g_srgb_views == 1 && stub.bound_tex[0] != 0x7777u &&
          strstr(g_srgb_ps, "_rsx_gamma_input(") && strstr(g_srgb_ps, "15u)"),
          "gamma RGBA keeps the shader decode on all four lanes");

    m(M_TEX_UNIT + 8, 1u << 20);                   /* gamma R only */
    draw_triangle();
    CHECK(g_srgb_views == 1 && strstr(g_srgb_ps, "_rsx_gamma_input("),
          "a partial gamma mask keeps the shader decode");

    m(M_TEX_UNIT + 8, 7u << 20);
    m(M_TEX_UNIT + 0x10, 0xAA1Bu);                 /* a crossbar, not the identity */
    draw_triangle();
    CHECK(g_srgb_views == 1 && strstr(g_srgb_ps, "_rsx_gamma_input("),
          "a non-identity crossbar keeps the shader decode");
    engine_down();
    g_stub_be_override = NULL;
}

/* RPCS3 VKFragmentProgram.cpp:407,638: the low-precision compare and the FLR
 * rounding bias are NVIDIA-host only. */
static void test_host_vendor_fp(void)
{
    printf("-- host vendor gates the FP precision fixups\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.pipeline_create = srgb_capture_pipeline;
    g_stub_be_override = &be;
    engine_up();
    u8 vp[32];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 8; i++) { u32 x; memcpy(&x, vp + i * 4, 4); m(0xB80 + i * 4, x); }
    m(0x1EA0, 0);
    rsx_test_fp_instr(vm_base + 0x60000, RSX_TEST_FP_OPC(0x11) | RSX_TEST_FP_MASK_XYZW |
                      RSX_TEST_FP_INSRC_TC0 | RSX_TEST_FP_END,
                      RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    m(0x8E4, 0x60002);
    m(0x1D60, 0x40);
    draw_triangle();
    CHECK(strstr(g_srgb_ps, "+ 0.000001)") != NULL, "NVIDIA host (default): FLR is biased");
    rsx_draw_engine_set_host_nvidia(0);
    m(M_BEGIN_END, 2u);    /* lines: low_precision_tests only on NVIDIA */
    m(M_DRAW_ARRAYS, 0u | ((2u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(strstr(g_srgb_ps, "+ 0.000001)") == NULL && strstr(g_srgb_ps, "floor("),
          "other host: plain floor");
    rsx_draw_engine_set_host_nvidia(1);
    engine_down();
    g_stub_be_override = NULL;
}

static int feq(float a, float b) { return a - b < 1e-4f && b - a < 1e-4f; }

/* RPCS3 programmable blending on X8R8G8B8_Z/_O: a blend reading alpha sees
 * the shader's raw alpha; only the stored alpha is the 0/1 constant. */
static rsx_be_render_state g_xa_rs;
static u32 xa_capture_pipeline(void* u, const char* vs, const char* ps, const rsx_be_render_state* rs,
                               const rsx_vertex_layout_plan* l, u32 stride, rsx_be_format f, u32 n)
{
    g_xa_rs = *rs;
    snprintf(g_srgb_ps, sizeof g_srgb_ps, "%s", ps);
    return stub_pipeline_create(u, vs, ps, rs, l, stride, f, n);
}

static u32 g_pb_snap, g_pb_snaps;
static u32 rec_pb_snapshot(void* u, u32 s)
{
    (void)u; (void)s;
    g_pb_snaps++;
    return g_pb_snap = stub_handle();
}

static void test_x8_alpha_blend(void)
{
    printf("-- X8 _Z/_O blending on raw alpha (programmable blending)\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.pipeline_create = xa_capture_pipeline;
    g_stub_be_override = &be;
    engine_up();
    {   u8 vp[32];
        rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_mov_out(vp + 16, 1, RSX_TEST_VP_SWZ_IDENT, 3, 1);
        m(0x1E9C, 0);
        for (u32 i = 0; i < 8; i++) { u32 x; memcpy(&x, vp + i * 4, 4); m(0xB80 + i * 4, x); }
        m(0x1EA0, 0);
        rsx_test_fp_mov_r0_col0(vm_base + 0x60000, RSX_TEST_FP_SWZ_IDENT);
        m(0x8E4, 0x60002);
        m(0x1D60, 0x40); }
    m(M_SURFACE_FORMAT, 4u | (2u << 5));            /* X8R8G8B8_Z8R8G8B8 */
    m(0x0310, 1u);
    m(0x0314, 0x0302u | (1u << 16));                /* SRC_ALPHA, ONE */
    m(0x0318, 0x0303u | (0u << 16));                /* 1-SRC_ALPHA, ZERO */
    m(0x0320, 0x8006u | (0x8006u << 16));
    draw_triangle();
    CHECK(g_xa_rs.x_alpha_pb == 1u && g_xa_rs.sf_rgb == 0x0302u && g_xa_rs.df_rgb == 0x0303u &&
          g_xa_rs.sf_a == 0u && g_xa_rs.df_a == 0u && !strstr(g_srgb_ps, ".a = 0.0;"),
          "_Z: the blend keeps SRC_ALPHA, the shader keeps its alpha, alpha stores 0");

    m(M_SURFACE_FORMAT, 5u | (2u << 5));            /* X8R8G8B8_O8R8G8B8 */
    m(0x0314, 0x0304u | (1u << 16));                /* DST_ALPHA */
    m(0x0318, 0x0305u | (0u << 16));                /* 1-DST_ALPHA */
    draw_triangle();
    CHECK(g_xa_rs.x_alpha_pb == 2u && g_xa_rs.sf_rgb == 1u && g_xa_rs.df_rgb == 0u &&
          g_xa_rs.sf_a == 0u && g_xa_rs.df_a == 1u,
          "_O: DST_ALPHA reads the constant 1 (ONE / ZERO), stored alpha kept");

    m(0x0314, 1u | (1u << 16));                     /* ONE, ONE: alpha passthrough */
    m(0x0318, 1u | (0u << 16));
    draw_triangle();
    CHECK(g_xa_rs.x_alpha_pb == 0u && strstr(g_srgb_ps, ".a = 1.0;"),
          "a blend not reading alpha keeps the shader remap");

    /* R2-03: blend_enabled_mask & surface_color_target_mask. */
    m(0x0314, 0x0302u | (1u << 16));
    m(0x0318, 0x0303u | (0u << 16));
    m(0x0310, 0u);
    m(0x036C, 2u);                                  /* BLEND_ENABLE_MRT: B only */
    draw_triangle();
    CHECK(g_xa_rs.x_alpha_pb == 0u, "blending a target not drawn needs no emulation");
    m(M_CONTEXT_DMA_COLOR_B, DMA_LOCAL);
    m(M_COLOR_B_OFFSET, 0x50000u);
    m(0x021C, 1024u);
    m(M_COLOR_TARGET, CELL_GCM_SURFACE_TARGET_MRT1);
    draw_triangle();
    CHECK(g_xa_rs.x_alpha_pb == 2u, "a target blended through BLEND_ENABLE_MRT only is emulated");
    m(0x036C, 0u);
    m(M_COLOR_TARGET, 1u);
    m(0x0310, 1u);
    engine_down();

    /* R2-01: with a target snapshot, the blend runs in the shader on the raw
     * (unclamped) alpha with the guest's factors, then alpha is remapped. */
    be.color_snapshot = rec_pb_snapshot;
    engine_up();
    {   u8 vp[32];
        rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_mov_out(vp + 16, 1, RSX_TEST_VP_SWZ_IDENT, 3, 1);
        m(0x1E9C, 0);
        for (u32 i = 0; i < 8; i++) { u32 x; memcpy(&x, vp + i * 4, 4); m(0xB80 + i * 4, x); }
        m(0x1EA0, 0);
        rsx_test_fp_mov_r0_col0(vm_base + 0x60000, RSX_TEST_FP_SWZ_IDENT);
        m(0x8E4, 0x60002);
        m(0x1D60, 0x40); }
    m(M_SURFACE_FORMAT, 4u | (2u << 5));            /* X8R8G8B8_Z8R8G8B8 */
    m(0x0310, 1u);
    m(0x0314, 0x0302u | (1u << 16));
    m(0x0318, 0x0303u | (0u << 16));
    m(0x0320, 0x8006u | (0x8006u << 16));
    g_pb_snaps = 0;
    draw_triangle();
    {
        const char* pbd = strstr(g_srgb_ps, "_pbd = ");
        const char* z = strstr(g_srgb_ps, ".a = 0.0;");
        CHECK(g_xa_rs.blend_enable == 0u && g_pb_snaps == 1 && pbd && z && z > pbd &&
              strstr(g_srgb_ps, "_pbs.aaa") && strstr(g_srgb_ps, "(1.0 - _pbs.aaa)"),
              "_Z: SRC_ALPHA blend in the shader, then alpha stores 0 (%u snapshots)", g_pb_snaps);
    }
    m(M_SURFACE_FORMAT, 5u | (2u << 5));            /* X8R8G8B8_O8R8G8B8 */
    draw_triangle();
    {
        const char* pbd = strstr(g_srgb_ps, "_pbd = ");
        const char* o = strstr(g_srgb_ps, ".a = 1.0;");
        CHECK(g_xa_rs.blend_enable == 0u && pbd && o && o > pbd,
              "_O: the blended alpha is stored as 1");
    }
    engine_down();
    g_stub_be_override = NULL;
}

/* RPCS3 evaluate_programmable_blending_state + RSXProgrammableBlendPrologue:
 * a signed blend equation on a non-X target blends in the shader against a
 * copy of the target bound at a free unit, hardware blending off; an X8 _Z
 * target, or no snapshot hook, keeps the hardware fallback. */
static void test_signed_blend(void)
{
    printf("-- signed blend equations (programmable blending)\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.pipeline_create = xa_capture_pipeline;
    be.color_snapshot = NULL;
    g_stub_be_override = &be;
    for (int pass = 0; pass < 2; pass++) {
        engine_up();
        {   u8 vp[32];
            rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
            rsx_test_vp_mov_out(vp + 16, 1, RSX_TEST_VP_SWZ_IDENT, 3, 1);
            m(0x1E9C, 0);
            for (u32 i = 0; i < 8; i++) { u32 x; memcpy(&x, vp + i * 4, 4); m(0xB80 + i * 4, x); }
            m(0x1EA0, 0);
            rsx_test_fp_mov_r0_col0(vm_base + 0x60000, RSX_TEST_FP_SWZ_IDENT);
            m(0x8E4, 0x60002);
            m(0x1D60, 0x40); }
        m(M_SURFACE_FORMAT, 8u | (2u << 5));        /* A8R8G8B8 */
        m(0x0310, 1u);
        m(0x0314, 1u | (1u << 16));
        m(0x0318, 1u | (1u << 16));
        m(0x0320, 0xF006u | (0x8006u << 16));       /* ADD_SIGNED, FUNC_ADD */
        g_pb_snaps = 0;
        draw_triangle();
        if (pass == 0) {
            CHECK(g_xa_rs.blend_enable == 1u && !strstr(g_srgb_ps, "_pbd") && g_pb_snaps == 0,
                  "no snapshot hook: the hardware fallback keeps blending on");
        } else {
            CHECK(g_xa_rs.blend_enable == 0u && strstr(g_srgb_ps, "rsx_tex[15].Load") &&
                  strstr(g_srgb_ps, "<< 24) >> 24") && g_pb_snaps == 1 &&
                  stub.bound_tex[15] == g_pb_snap && (stub.bound_mask & 0x8000u),
                  "ADD_SIGNED: blend in the shader on a copy at unit 15 (%u snapshots, blend %u)",
                  g_pb_snaps, g_xa_rs.blend_enable);
            draw_triangle();
            CHECK(g_pb_snaps == 2 && stub.bound_tex[15] == g_pb_snap,
                  "every such draw reads the target as it stands (%u snapshots)", g_pb_snaps);
            m(M_SURFACE_FORMAT, 4u | (2u << 5));    /* X8R8G8B8_Z8R8G8B8 */
            draw_triangle();
            CHECK(g_xa_rs.blend_enable == 0u && strstr(g_srgb_ps, "_pbd") &&
                  strstr(g_srgb_ps, ".a = 0.0;"),
                  "X8 _Z: a blend writing alpha (ONE/ONE) blends in the shader too");
            m(M_SURFACE_FORMAT, 8u | (2u << 5));
            m(0x0320, 0x8006u | (0x8006u << 16));
            draw_triangle();
            CHECK(g_xa_rs.blend_enable == 1u && !strstr(g_srgb_ps, "_pbd"),
                  "FUNC_ADD stays on hardware blending");
        }
        engine_down();
        be.color_snapshot = rec_pb_snapshot;
    }
    g_stub_be_override = NULL;
}
/* RPCS3 avconf::aspect_convert_region at present: the shown rectangle keeps
 * its own aspect inside the 16:9 frame. */
static struct { int n; u32 sw, sh, aw, ah; } g_pr;
static void rec_present_region(void* u, u32 s, u32 sx, u32 sy, u32 sw, u32 sh, u32 aw, u32 ah)
{
    (void)u; (void)sx; (void)sy;
    presented_surface = s; stub.n_present++;
    g_pr.n++; g_pr.sw = sw; g_pr.sh = sh; g_pr.aw = aw; g_pr.ah = ah;
}

static void test_present_aspect(void)
{
    printf("-- present aspect (avconf)\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.present_region = rec_present_region;
    g_stub_be_override = &be;
    engine_up();
    rsx_draw_engine_set_display_buffer(0, RSX_LOCATION_LOCAL, 0x40000, 1024, 256, 256);
    draw_triangle();
    rsx_draw_engine_present_buffer(0);
    CHECK(g_pr.n == 1 && g_pr.sw == 256 && g_pr.sh == 256 && g_pr.aw * 9u == g_pr.ah * 16u,
          "a full-buffer surface fills the 16:9 frame (%u:%u)", g_pr.aw, g_pr.ah);
    /* A 256x240 inset 8 rows down: letterboxed at its own aspect. */
    m(M_SURFACE_CLIP_V, 240u << 16);
    m(M_COLOR_A_OFFSET, 0x40000u + 8u * 1024u);
    draw_triangle();
    rsx_draw_engine_present_buffer(0);
    CHECK(g_pr.n == 2 && g_pr.sw == 256 && g_pr.sh == 240 &&
          g_pr.aw == 256u * 16u * 256u && g_pr.ah == 240u * 9u * 256u,
          "an inset keeps its aspect inside the 16:9 frame (%ux%u, %u:%u)",
          g_pr.sw, g_pr.sh, g_pr.aw, g_pr.ah);
    engine_down();
    g_stub_be_override = NULL;
}

static void test_clear_formats(void)
{
    printf("-- clear by surface format\n");
    g_stub_be_override = stub_backend_with_clear_rect();

    /* B8 (0x09): only the blue channel takes the clear. */
    engine_up();
    m(M_SURFACE_FORMAT, 0x09u);
    m(M_COLOR_CLEAR_VALUE, 0x80112233u);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_clear_rect == 1 && stub.cr_mask == 4u && stub.n_clear_color == 0,
          "B8 clears the blue channel only (rect %d mask %x color %d)",
          stub.n_clear_rect, stub.cr_mask, stub.n_clear_color);
    CHECK(feq(stub.cr_rgba[2], 0x33 / 255.0f), "B8 clear carries the guest blue");
    engine_down();

    /* A8B8G8R8 (0x10): R and B swap in the colour and in the mask. */
    engine_up();
    m(M_SURFACE_FORMAT, 0x10u);
    m(M_COLOR_CLEAR_VALUE, 0x00112233u);
    m(M_CLEAR_BUFFERS, 0x10u);
    CHECK(stub.n_clear_rect == 1 && stub.cr_mask == 4u,
          "A8B8G8R8 maps the R clear bit to B (mask %x)", stub.cr_mask);
    CHECK(feq(stub.cr_rgba[0], 0x33 / 255.0f) && feq(stub.cr_rgba[2], 0x11 / 255.0f),
          "A8B8G8R8 swaps R and B (%g %g)", stub.cr_rgba[0], stub.cr_rgba[2]);
    engine_down();

    /* X32 (0x0D): no colour clear at all. */
    engine_up();
    m(M_SURFACE_FORMAT, 0x0Du);
    m(M_COLOR_CLEAR_VALUE, 0xFFFFFFFFu);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_clear_rect == 0 && stub.n_clear_color == 0, "X32 takes no colour clear");
    engine_down();

    /* R5G6B5 (0x03): the guest word is renormalised from 5:6:5. */
    engine_up();
    m(M_SURFACE_FORMAT, 0x03u);
    m(M_COLOR_CLEAR_VALUE, 0x0000F800u);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_clear_color == 1 && stub.n_clear_rect == 0,
          "a full R5G6B5 clear takes the whole-target path");
    CHECK(feq(stub.last_clear_rgba[0], 1.0f) && feq(stub.last_clear_rgba[1], 0.0f) &&
          feq(stub.last_clear_rgba[2], 0.0f),
          "R5G6B5 0xF800 clears to red (%g %g %g)", stub.last_clear_rgba[0],
          stub.last_clear_rgba[1], stub.last_clear_rgba[2]);
    engine_down();

    /* A scissor inside the target clears only that rectangle. */
    engine_up();
    m(M_SURFACE_FORMAT, 0x08u);
    m(M_SCISSOR_H, 16u | (64u << 16));
    m(M_SCISSOR_V, 8u | (32u << 16));
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_clear_rect == 1 && stub.cr_mask == 0xFu && stub.cr_rect[0] == 16u &&
          stub.cr_rect[1] == 8u && stub.cr_rect[2] == 64u && stub.cr_rect[3] == 32u,
          "the scissor bounds the clear (%u,%u %ux%u)", stub.cr_rect[0], stub.cr_rect[1],
          stub.cr_rect[2], stub.cr_rect[3]);
    engine_down();

    /* STENCIL_MASK 0x0F: a partial stencil clear of a Z24S8 zeta. */
    engine_up();
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_ZETA_PITCH, 256u * 4u);
    m(M_SURFACE_FORMAT, 0x08u | (2u << 5));
    m(M_STENCIL_MASK, 0x0Fu);
    m(M_ZSTENCIL_CLEAR, 0xFFFFFF5Au);
    m(M_CLEAR_BUFFERS, 0x02u);
    /* n_clear_ds 1: the new zeta's clear_memory (1.0/255) under the partial
     * stencil clear. */
    CHECK(stub.n_clear_rect == 1 && (stub.cr_flags & RSX_BE_CLEAR_STENCIL) &&
          !(stub.cr_flags & RSX_BE_CLEAR_DEPTH) && stub.cr_smask == 0x0Fu &&
          stub.cr_stencil == 0x5Au && stub.n_clear_ds == 1 && stub.last_clear_depth == 1.0f,
          "STENCIL_MASK 0x0F gives a masked stencil clear (flags %x smask %x rect %d st %x ds %d d %g)",
          stub.cr_flags, stub.cr_smask, stub.n_clear_rect, stub.cr_stencil, stub.n_clear_ds,
          stub.last_clear_depth);
    engine_down();

    /* Without clear_rect a backend still gets the whole-target clear. */
    g_stub_be_override = NULL;
    engine_up();
    m(M_SURFACE_FORMAT, 0x08u);
    m(M_SCISSOR_H, 16u | (64u << 16));
    m(M_SCISSOR_V, 8u | (32u << 16));
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_clear_color == 1, "a backend without clear_rect falls back to clear_color");
    engine_down();
}

/* ---- 9a2. flat shading (RPCS3 RSXGraphEngine.cpp:1107) ------------------- */

/* A guest VP passing position through and an FP moving COL0 to the output. */
static void load_col0_programs(void)
{
    u8 vp[16];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 4; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0x0B80 + i * 4, w); }
    m(0x1EA0, 0);
    rsx_test_fp_mov_r0_col0(vm_base + TEX_OFFSET, RSX_TEST_FP_SWZ_IDENT);
    m(0x08E4, TEX_OFFSET | 2u);
    m(0x1D60, 64u);
}

static void test_flat_shading(void)
{
    printf("-- flat shading\n");
    rsx_draw_engine_set_last_provoking_vertex(1);
    engine_up();
    load_col0_programs();
    m(M_SHADE_MODE, 0x1D01u);
    draw_triangle();
    CHECK(stub.n_pipeline_create >= 1 && !strstr(g_stub_last_ps, "nointerpolation"),
          "smooth shading interpolates COL0");
    const int smooth = stub.n_pipeline_create;
    m(M_SHADE_MODE, 0x1D00u);
    draw_triangle();
    CHECK(stub.n_pipeline_create == smooth + 1 && strstr(g_stub_last_ps, "nointerpolation float4 col0"),
          "FLAT shading makes COL0 nointerpolation");
    engine_down();

    /* Without a last-vertex provoking convention the engine keeps smooth
     * (RPCS3 VKGSRender.cpp:639 does the same without the extension). */
    rsx_draw_engine_set_last_provoking_vertex(0);
    engine_up();
    load_col0_programs();
    m(M_SHADE_MODE, 0x1D00u);
    draw_triangle();
    CHECK(stub.n_pipeline_create >= 1 && !strstr(g_stub_last_ps, "nointerpolation"),
          "no provoking-vertex support: FLAT falls back to smooth");
    engine_down();

    setenv("RSX_ENG_FLAT_SHADE", "0", 1);
    rsx_draw_engine_set_last_provoking_vertex(1);
    engine_up();
    load_col0_programs();
    m(M_SHADE_MODE, 0x1D00u);
    draw_triangle();
    CHECK(stub.n_pipeline_create >= 1 && !strstr(g_stub_last_ps, "nointerpolation"),
          "RSX_ENG_FLAT_SHADE=0 disables flat shading");
    engine_down();
    unsetenv("RSX_ENG_FLAT_SHADE");
    rsx_draw_engine_set_last_provoking_vertex(0);
}

/* ---- 9b. antialiased surfaces at sample resolution ------------------------ */

#define M_VIEWPORT_TRANSLATE  0x0A20
#define M_VIEWPORT_SCALE      0x0A30

static void mf(u32 method, float f) { u32 w; memcpy(&w, &f, 4); m(method, w); }

/* An antialiased surface holds its sample grid in memory (SURFACE_FORMAT
 * bits 12-15: 3 = 2x1, 4 and 5 = 2x2), so a texture reading it sees that
 * size. The surface is created at sample resolution; the viewport and
 * scissor registers stay in guest pixels and are scaled to match. */
static void test_antialiased_surface(void)
{
    printf("-- antialiased surface\n");
    static const struct { u32 aa, w, h, sx, sy; } cases[] = {
        { 0, 128,  64, 1, 1 },     /* CENTER_1 */
        { 3, 256,  64, 2, 1 },     /* DIAGONAL_CENTERED_2 */
        { 4, 256, 128, 2, 2 },     /* SQUARE_CENTERED_4 */
        { 5, 256, 128, 2, 2 },     /* SQUARE_ROTATED_4 */
    };
    for (u32 i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        engine_up();
        m(M_SURFACE_FORMAT, 8u | (cases[i].aa << 12));
        m(M_SURFACE_CLIP_H, 128u << 16);
        m(M_SURFACE_CLIP_V, 64u << 16);
        m(M_COLOR_A_OFFSET, 0x80000u);
        m(M_SCISSOR_H, 8u | (32u << 16));
        m(M_SCISSOR_V, 4u | (16u << 16));
        /* The whole 128x64-pixel viewport. */
        mf(M_VIEWPORT_TRANSLATE + 0, 64.0f); mf(M_VIEWPORT_TRANSLATE + 4, 32.0f);
        mf(M_VIEWPORT_SCALE + 0, 64.0f);     mf(M_VIEWPORT_SCALE + 4, -32.0f);
        draw_triangle();
        CHECK(stub.color_w == cases[i].w && stub.color_h == cases[i].h,
              "antialias %u: a 128x64 surface is created at %ux%u (got %ux%u)",
              cases[i].aa, cases[i].w, cases[i].h, stub.color_w, stub.color_h);
        CHECK(stub.scissor[0] == 8u * cases[i].sx && stub.scissor[1] == 4u * cases[i].sy &&
              stub.scissor[2] == 32u * cases[i].sx && stub.scissor[3] == 16u * cases[i].sy,
              "antialias %u: scissor 8,4 32x16 covers %u,%u %ux%u samples (got %u,%u %ux%u)",
              cases[i].aa, 8u * cases[i].sx, 4u * cases[i].sy, 32u * cases[i].sx,
              16u * cases[i].sy, stub.scissor[0], stub.scissor[1], stub.scissor[2],
              stub.scissor[3]);
        CHECK(stub.vp_xf[0] == 1.0f && stub.vp_xf[1] == 1.0f &&
              stub.vp_xf[4] == 0.0f && stub.vp_xf[5] == 0.0f,
              "antialias %u: the full-pixel viewport still maps to the whole surface "
              "(scale %g,%g offset %g,%g)", cases[i].aa, stub.vp_xf[0], stub.vp_xf[1],
              stub.vp_xf[4], stub.vp_xf[5]);
        engine_down();
    }
}

/* ---- 10. a zeta or a target read in a texture format of its own --------- */

static void test_depth_read_as_color(void)
{
    printf("-- zeta read as A8R8G8B8\n");
    engine_up();
    /* A depth-only pass, with colour A named at the zeta's own address and
     * nothing written to it: how a title points an unused colour target. */
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_SURFACE_FORMAT, 8u | (2u << 5));
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_ZETA_PITCH, 256u * 4u);
    m(M_COLOR_A_OFFSET, ZETA_OFFSET);
    m(M_COLOR_MASK, 0u);
    m(M_DEPTH_TEST, 1u);
    m(M_DEPTH_WRITE, 1u);
    draw_triangle();
    CHECK(stub.n_draw == 1, "the depth-only pass drew");

    /* The next pass reads that address as A8R8G8B8, the zeta still bound:
     * the bytes are the zeta's, stored to guest memory from the depth
     * target, not the untouched colour target's. */
    m(M_COLOR_MASK, 0x01010101u);
    m(M_DEPTH_TEST, 0u);
    m(M_DEPTH_WRITE, 0u);
    m(M_COLOR_A_OFFSET, 0x90000u);
    bind_texture_unit(RSX_LOCATION_LOCAL, ZETA_OFFSET);
    const int uploads = stub.n_texture_create;
    draw_triangle();
    const u8* z = vm_base + GUEST_LOCAL_EA + ZETA_OFFSET;
    CHECK(stub.n_depth_readback == 1, "the depth target was read back (%d)",
          stub.n_depth_readback);
    CHECK(z[0] == 0x80 && z[1] == 0x00 && z[2] == 0x00 && z[3] == 0x00,
          "guest memory holds depth 0.5 laid out as Z24S8 (%02X %02X %02X %02X)",
          z[0], z[1], z[2], z[3]);
    CHECK(stub.n_texture_create == uploads + 1 &&
          stub.bound_tex[0] == stub.last_created_texture,
          "the unit samples a texture decoded from those bytes");

    /* No depth write since: nothing is read back again. */
    draw_triangle();
    CHECK(stub.n_depth_readback == 1,
          "an unchanged depth target is not read back again (%d)", stub.n_depth_readback);
    engine_down();
}

/* RPCS3 Write Depth Buffer: false -- a CPU-side transfer (NV0039) reading a
 * depth target's range sees guest memory as it was; colour is still stored. */
static void test_range_read_skips_depth(void)
{
    printf("-- range read leaves depth in VRAM (Write Depth Buffer off)\n");
    engine_up();
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_SURFACE_FORMAT, 8u | (2u << 5));
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_ZETA_PITCH, 256u * 4u);
    m(M_DEPTH_TEST, 1u);
    m(M_DEPTH_WRITE, 1u);
    draw_triangle();
    u8* z = vm_base + GUEST_LOCAL_EA + ZETA_OFFSET;
    memset(z, 0xA5, 4);
    rsx_draw_engine_range_read(RSX_LOCATION_LOCAL, ZETA_OFFSET, 1024u * 256u);
    rsx_draw_engine_range_read_color(RSX_LOCATION_LOCAL, ZETA_OFFSET, 1024u * 256u);
    CHECK(stub.n_depth_readback == 0 && z[0] == 0xA5 && z[3] == 0xA5,
          "the depth target is not stored for a CPU-side read (%d readbacks)",
          stub.n_depth_readback);
    engine_down();
}

/* RPCS3 flushes a 565 / B8 / G8B8 / ABGR target in its own guest layout
 * (get_compatible_gcm_format: multibyte byte-swapped, B8 not). The stub
 * reads every pixel back as host R,G,B,A = 11 22 33 44. */
/* RPCS3 copy_unmodified_block_vtc: a power-of-two DXT volume is VTC-tiled,
 * four slices' blocks interleaved; it is un-tiled into a BC volume. */
static struct { int n_create, n_up; u8 slice[4][16]; } g_vtc;
static u32 vtc_create_3d(void* u, rsx_be_format f, u32 w, u32 h, u32 d, u32 mips,
                         u32 remap, u32 rsx_fmt)
{
    (void)u; (void)f; (void)w; (void)h; (void)d; (void)mips; (void)remap; (void)rsx_fmt;
    g_vtc.n_create++;
    return stub_handle();
}
static void vtc_upload_3d(void* u, u32 t, u32 z, u32 mip, u32 w, u32 h,
                          const void* src, u32 row)
{
    (void)u; (void)t; (void)w; (void)h;
    g_vtc.n_up++;
    if (!mip && z < 4 && row >= 16u) memcpy(g_vtc.slice[z], src, 16);
}

static void test_dxt_volume_vtc(void)
{
    printf("-- DXT volume VTC de-tiling\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.texture_create_3d = vtc_create_3d;
    be.texture_upload_3d = vtc_upload_3d;
    g_stub_be_override = &be;
    engine_up();
    memset(&g_vtc, 0, sizeof g_vtc);
    /* 8x4x4 DXT1: 2 blocks a slice; tiled order s0b0 s1b0 s2b0 s3b0 s0b1 ... */
    u8* p = vm_base + TEX_OFFSET;
    for (u32 i = 0; i < 8; i++) memset(p + i * 8u, (int)(0x10u * (i & 3u) + (i >> 2)), 8);
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    m(M_TEX_UNIT + 0x04, 2u | (1u << 3) | (3u << 4) | (0x86u << 8) | (1u << 16));
    m(M_TEX_UNIT + 0x18, (8u << 16) | 4u);
    m(0x1840, (4u << 20) | 16u);
    draw_triangle();
    int ok = g_vtc.n_create == 1 && g_vtc.n_up == 4;
    for (u32 z = 0; ok && z < 4; z++)
        ok = g_vtc.slice[z][0] == (u8)(0x10u * z) && g_vtc.slice[z][8] == (u8)(0x10u * z + 1u);
    CHECK(ok, "slice z gets blocks z and 4+z (%d create, %d uploads, s1 %02X %02X)",
          g_vtc.n_create, g_vtc.n_up, g_vtc.slice[1][0], g_vtc.slice[1][8]);
    engine_down();
    g_stub_be_override = NULL;
}

static struct { int n; u32 tex, layer[8], mip[8], sy[8], w[8], h[8]; } g_unw;
static int rec_unwrap_copy(void* u, u32 tex, u32 layer, u32 mip, u32 surface,
                           u32 sx, u32 sy, u32 w, u32 h)
{
    (void)u; (void)surface; (void)sx;
    if (g_unw.n < 8) {
        g_unw.layer[g_unw.n] = layer; g_unw.mip[g_unw.n] = mip; g_unw.sy[g_unw.n] = sy;
        g_unw.w[g_unw.n] = w; g_unw.h[g_unw.n] = h;
    }
    g_unw.n++;
    g_unw.tex = tex;
    return 1;
}

/* RPCS3 cubemap_unwrap / _3d_unwrap: a cube or volume read over a render
 * target copies its faces / slices out of the target on the GPU. */
static void test_unwrap_cube_3d(void)
{
    printf("-- cube / volume textures unwrapped from a render target\n");
    static rsx_draw_backend be;
    be = g_stub_backend;
    be.texture_create_3d = vtc_create_3d;
    be.texture_copy_from_color = rec_unwrap_copy;
    g_stub_be_override = &be;
    for (int kind = 0; kind < 2; kind++) {
        engine_up();
        memset(&g_unw, 0, sizeof g_unw);
        m(M_SURFACE_CLIP_H, 16u << 16);
        m(M_SURFACE_CLIP_V, 144u << 16);
        m(M_COLOR_A_PITCH, 64u);
        draw_triangle();                            /* a 16x144 target at 0x40000 */
        m(M_COLOR_A_OFFSET, 0x80000u);              /* drawn elsewhere now */
        bind_texture_unit(RSX_LOCATION_LOCAL, 0x40000u);
        if (kind == 0) {                            /* 16x16 cube, 2 levels */
            m(M_TEX_UNIT + 0x04, 1u | (1u << 2) | (1u << 3) | (2u << 4) | ((0x85u | 0x20u) << 8) | (2u << 16));
            m(M_TEX_UNIT + 0x18, (16u << 16) | 16u);
            m(0x1840, (1u << 20) | 64u);
        } else {                                    /* 16x16x4 volume */
            m(M_TEX_UNIT + 0x04, 1u | (3u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
            m(M_TEX_UNIT + 0x18, (16u << 16) | 16u);
            m(0x1840, (4u << 20) | 64u);
        }
        draw_triangle();
        if (kind == 0) {
            /* slice_h = 16*64 + 8*64 bytes / 64 = 24 rows; level 1 below level 0. */
            CHECK(g_unw.n == 12 && g_unw.layer[2] == 1u && g_unw.sy[2] == 24u &&
                  g_unw.mip[3] == 1u && g_unw.sy[3] == 40u && g_unw.w[3] == 8u &&
                  stub.bound_tex[0] == g_unw.tex,
                  "six faces x two levels copied at slice_h 24 (%d copies, face1 y %u, mip1 y %u)",
                  g_unw.n, g_unw.sy[2], g_unw.sy[3]);
        } else {
            CHECK(g_unw.n == 4 && g_unw.layer[3] == 3u && g_unw.sy[3] == 48u &&
                  g_unw.h[3] == 16u && stub.bound_tex[0] == g_unw.tex,
                  "four slices of level 0 copied at height steps (%d copies, z3 y %u)",
                  g_unw.n, g_unw.sy[3]);
            const int n = g_unw.n;
            draw_triangle();
            CHECK(g_unw.n == n, "an unchanged target reuses the unwrap (%d copies)", g_unw.n);
        }
        engine_down();
    }
    g_stub_be_override = NULL;
}

static void test_packed_color_store(void)
{
    printf("-- packed colour targets stored in their guest layout\n");
    static const struct { u32 cfmt, bpp; u8 b[4]; const char* what; } k[] = {
        { 0x03u, 2u, { 0x11, 0x06 }, "R5G6B5 packs to BE16 0x1106" },
        { 0x02u, 2u, { 0x08, 0x86 }, "X1R5G5B5_O packs to BE16 0x0886 (alpha lane 0x44 -> 0)" },
        { 0x09u, 1u, { 0x11 },       "B8 stores the R lane (shader B)" },
        { 0x0Au, 2u, { 0x22, 0x11 }, "G8B8 stores G,B" },
        { 0x10u, 4u, { 0x44, 0x33, 0x22, 0x11 }, "A8B8G8R8 stores A,B,G,R" },
    };
    for (u32 i = 0; i < sizeof k / sizeof k[0]; i++) {
        engine_up();
        m(M_SURFACE_FORMAT, k[i].cfmt | (2u << 5));
        m(M_COLOR_A_PITCH, 256u * k[i].bpp);
        m(M_CLEAR_BUFFERS, 0xF0u);
        u8* p = vm_base + GUEST_LOCAL_EA + 0x40000u;
        memset(p, 0xA5, 8);
        rsx_draw_engine_range_read_color(RSX_LOCATION_LOCAL, 0x40000u, 256u * k[i].bpp * 4u);
        CHECK(!memcmp(p, k[i].b, k[i].bpp) && !memcmp(p + k[i].bpp, k[i].b, k[i].bpp),
              "%s (%02X %02X %02X %02X)", k[i].what, p[0], p[1], p[2], p[3]);
        engine_down();
    }
}

static void test_color_read_as_float(void)
{
    printf("-- colour target read as X32_FLOAT\n");
    engine_up();
    m(M_COLOR_A_PITCH, 256u * 4u);
    m(M_CLEAR_BUFFERS, 0xF0u);                   /* a colour write at 0x40000 */
    const u32 surface = stub.last_created_color;

    /* Read back as X32_FLOAT, the target's 32-bit pixels are floats a pass
     * packed into it: they come from guest memory, where the target's pixels
     * are stored as A,R,G,B first. */
    m(M_COLOR_A_OFFSET, 0x90000u);
    bind_texture_unit(RSX_LOCATION_LOCAL, 0x40000u);
    m(M_TEX_UNIT + 0x04, 1u | (2u << 4) | ((0x9Cu | 0x20u) << 8) | (1u << 16));
    const int uploads = stub.n_texture_create;
    draw_triangle();
    const u8* p = vm_base + GUEST_LOCAL_EA + 0x40000u;
    CHECK(p[0] == 0x44 && p[1] == 0x11 && p[2] == 0x22 && p[3] == 0x33,
          "the target was stored to guest memory as A,R,G,B (%02X %02X %02X %02X)",
          p[0], p[1], p[2], p[3]);
    CHECK(stub.n_texture_create == uploads + 1 && stub.bound_tex[0] != surface,
          "the unit decodes those bytes instead of sampling the RGBA8 image");
    engine_down();
}

/* The register-based fullscreen compositor has no array or inline stream.
 * Capture each position completion with the attributes current at that time. */
static void immediate_vertex(float x, float y, float u, float v)
{
    m(0x18C0, f32_bits(u)); m(0x18C4, f32_bits(v)); /* ATTR8 DATA2F */
    m(0x1500, f32_bits(x)); m(0x1504, f32_bits(y));
    m(0x1508, f32_bits(-1.0f));                    /* ATTR0 DATA3F */
}

static void test_immediate_compositor(void)
{
    engine_up();
    /* Leave array declarations pointing outside readable guest memory. */
    m(M_VTXBUF_OFFSET, 0xFFFFFFF0u);
    m(0x1738, 0x7FFFFFF0u); m(0x173C, 0x7FFFFFF0u);
    m(M_VTXFMT, 2u | (4u << 4) | (16u << 8) | (7u << 16));
    immediate_vertex(99, 98, 97, 96); /* constants outside BEGIN do not draw */
    m(M_BEGIN_END, 6u);              /* retail fullscreen TRIANGLE_STRIP */
    immediate_vertex(-1, -1, 0, 1);
    immediate_vertex(3, -1, 2, 1);
    immediate_vertex(-1, 3, 0, -1);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 1, "register fullscreen triangle issues a draw");
    CHECK(stub.draw_vertices == 3 && stub.draw_indices == 3,
          "register triangle uploads 3 vertices/indices, got %u/%u",
          stub.draw_vertices, stub.draw_indices);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_TRIANGLES, "strip rebuilt as triangles");
    const float expected[3][4] = {{-1,-1,0,1}, {3,-1,2,1}, {-1,3,0,-1}};
    for (u32 i = 0; i < 3; i++) {
        CHECK(stub.captured_vertices[i][0] == expected[i][0] &&
              stub.captured_vertices[i][1] == expected[i][1] &&
              stub.captured_vertices[i][2] == -1 && stub.captured_vertices[i][3] == 1,
              "immediate position %u preserved independently of arrays", i);
        CHECK(stub.captured_vertices[i][32] == expected[i][2] &&
              stub.captured_vertices[i][33] == expected[i][3] &&
              stub.captured_vertices[i][34] == 0 && stub.captured_vertices[i][35] == 1,
              "immediate UV %u preserved at position completion", i);
        CHECK(stub.captured_indices[i] == i, "immediate strip index %u", i);
    }
    m(M_BEGIN_END, 8u);
    immediate_vertex(-1,-1,0,0); immediate_vertex(1,-1,1,0);
    immediate_vertex(1,1,1,1); immediate_vertex(-1,1,0,1);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 2 && stub.draw_vertices == 4 && stub.draw_indices == 6,
          "immediate quad rebuilt without previous group vertices");
    m(M_BEGIN_END, 6u);
    m(0x18C0, f32_bits(8)); m(0x18C4, f32_bits(9));
    m(0x1500, f32_bits(8)); m(0x1504, f32_bits(9)); /* incomplete position */
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 2, "UV and incomplete position do not emit a vertex");
    /* DATA4F completes on W and preserves an explicit W. */
    m(M_BEGIN_END, 5u);
    for (u32 i = 0; i < 3; i++) {
        m(0x1C00, f32_bits((float)i)); m(0x1C04, f32_bits(2));
        m(0x1C08, f32_bits(3)); m(0x1C0C, f32_bits(4));
    }
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 3 && stub.draw_vertices == 3 &&
          stub.captured_vertices[2][0] == 2 && stub.captured_vertices[2][3] == 4,
          "DATA4F position emits once at W with explicit W preserved");
    m(M_BEGIN_END, 5u);
    immediate_vertex(1,2,3,4);
    m(M_DRAW_ARRAYS, 2u << 24);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 3, "mixed immediate and arrays reject the entire group");
    m(M_BEGIN_END, 5u);
    for (u32 i = 0; i < 4098; i++) immediate_vertex(1,2,3,4);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 3, "oversized immediate group never draws a truncated prefix");
    m(M_BEGIN_END, 6u);
    immediate_vertex(-1,-1,0,1); immediate_vertex(3,-1,2,1);
    immediate_vertex(-1,3,0,-1);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 4 && stub.draw_vertices == 3,
          "BEGIN recovers after oversized or mixed groups");
    m(M_BEGIN_END, 5u);
    for (u32 attr = 0; attr < 16; attr++)
        for (u32 lane = 0; lane < 4; lane++) m(0x1C00 + attr * 16 + lane * 4, 0);
    /* The ATTR0 above emits the first vertex, then emit two more zero positions. */
    for (u32 v = 0; v < 2; v++)
        for (u32 lane = 0; lane < 4; lane++) m(0x1C00 + lane * 4, 0);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 5 && stub.captured_vertices[1][3] == 0 &&
          stub.captured_vertices[1][35] == 0,
          "explicit all-zero DATA4F preserves position W and attribute alpha");
    /* R2-04: a one-vertex group takes the registers at END (UV written late). */
    const u32 nd = stub.n_draw;
    m(M_BEGIN_END, 1u);
    immediate_vertex(0.5f, 0.5f, 1, 2);
    m(0x18C0, f32_bits(7)); m(0x18C4, f32_bits(6));
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == nd + 1 && stub.captured_vertices[0][32] == 7 &&
          stub.captured_vertices[0][33] == 6,
          "single immediate vertex reads the late UV (%g,%g)",
          stub.captured_vertices[0][32], stub.captured_vertices[0][33]);
    engine_down();
}

static void test_immediate_packed_zero_and_inline(void)
{
    const u32 packed[] = {0x194Cu, 0x1998u, 0x0A98u};
    for (u32 k = 0; k < 3; k++) {
        engine_up();
        m(packed[k], 0);
        if (k) m(packed[k] + 4, 0);
        m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
        immediate_vertex(0,0,0,0); immediate_vertex(1,0,0,0);
        immediate_vertex(0,1,0,0);
        m(M_BEGIN_END, 0);
        /* SCALED4S is RPCS3's s1, (v + 0.5) / 32767.5, so its zero is 0.5/32767.5. */
        CHECK(stub.n_draw == 1 && stub.captured_vertices[0][15] >= 0 &&
              stub.captured_vertices[0][15] < 1e-4f,
              "packed all-zero method 0x%X preserves explicit alpha", packed[k]);
        engine_down();
    }
    engine_up();
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_INLINE_ARRAY, 0); /* shorter than declared stride */
    immediate_vertex(0,0,0,0); immediate_vertex(1,0,0,0);
    immediate_vertex(0,1,0,0);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 0, "short inline stream mixed with immediate rejects group");
    m(M_VTXFMT, 0); /* no enabled inline attribute */
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_INLINE_ARRAY, 0);
    immediate_vertex(0,0,0,0); immediate_vertex(1,0,0,0);
    immediate_vertex(0,1,0,0);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 0, "undeclared inline stream mixed with immediate rejects group");
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    immediate_vertex(0,0,0,0); immediate_vertex(1,0,0,0);
    immediate_vertex(0,1,0,0);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 1, "BEGIN clears rejected inline source state");
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    immediate_vertex(0,0,0,0); immediate_vertex(1,0,0,0);
    immediate_vertex(0,1,0,0);
    for (u32 i = 0; i <= RSX_DSP_INLINE_MAX_BYTES / 4u; i++)
        m(M_INLINE_ARRAY, 0);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 1, "overflowed inline stream mixed with immediate rejects group");
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    immediate_vertex(0,0,0,0); immediate_vertex(1,0,0,0);
    immediate_vertex(0,1,0,0);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 2, "BEGIN recovers after mixed inline overflow");
    engine_down();
}

/* ---- vertex input parity with RPCS3 (scratch/parity/vertex.md) ---------- */

#define FREQ_VTX_OFFSET 0x00018000u
#define M_IDX_OFFSET        0x181C
#define M_IDX_FORMAT        0x1820
#define M_DRAW_INDEX_ARRAY  0x1824

static float first_x_of_triangle(u32 i)
{
    return stub.captured_vertices[stub.draw_indices ? stub.captured_indices[i] : i][0];
}

static void put_be_f32(u8* p, float f)
{
    const u32 b = f32_bits(f);
    p[0] = (u8)(b >> 24); p[1] = (u8)(b >> 16); p[2] = (u8)(b >> 8); p[3] = (u8)b;
}

/* Main-memory offsets at or above g_hole_from do not resolve, as an unmapped
 * IO page would; everything resolved below it reads on linearly. */
static u32 g_hole_from = 0xFFFFFFFFu;
static const u8* hole_guest_read(void* user, u32 location, u32 offset, u32 bytes)
{
    (void)user;
    const u32 ea = location == 0 ? GUEST_LOCAL_EA + offset : offset;
    if (location != 0 && offset >= g_hole_from) return NULL;
    if ((u64)ea + bytes > GUEST_BYTES) return NULL;
    return vm_base + ea;
}

/* The second parity pass: zclip snap, interleaved blocks, empty indexed
 * draws, line-loop closing and reads past an unresolvable element. Each
 * reverts under RSX_VTX_LEGACY=1. */
static void test_vertex_gaps_rpcs3(void)
{
    printf("-- vertex input rules, second pass (RPCS3)\n");
    const int legacy = rsx_vertex_legacy();

    /* apply_zclip_xform: one 24-bit step of the default [0, 1] range. */
    engine_up();
    draw_triangle();
    CHECK(stub.n_draw == 1 &&
          stub.vp_xf[7] == (legacy ? 0.0f : 1.0f / 16777215.0f),
          "zclip snap epsilon in vp_posoffset.w (got %g)", stub.vp_xf[7]);
    engine_down();

    /* Interleaved block, stride 32: ATTR0 plain at +0, ATTR1 divided by 2 at
     * +16. DRAW_ARRAYS first 4: the block uploads from min(4, 4/2) = 2, so
     * ATTR0 reads 2,3,4 (RPCS3) rather than 4,5,6. */
    const u32 blk = FREQ_VTX_OFFSET + 0x400u;
    for (u32 k = 0; k < 8; k++) {
        put_be_f32(vm_base + blk + k * 32u, (float)k);
        put_be_f32(vm_base + blk + k * 32u + 12u, 1.0f);
        put_be_f32(vm_base + blk + k * 32u + 16u, (float)(100 + k));
    }
    engine_up();
    m(M_VTXBUF_OFFSET, 0x80000000u | blk);
    m(M_VTXBUF_OFFSET + 4u, 0x80000000u | (blk + 16u));
    m(M_VTXFMT, 2u | (4u << 4) | (32u << 8));
    m(M_VTXFMT + 4u, 2u | (4u << 4) | (32u << 8) | (2u << 16));
    m(0x1FC0, 0u);
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_DRAW_ARRAYS, 4u | ((3u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    {
        const float want = legacy ? 4.0f : 2.0f;
        CHECK(stub.n_draw == 1 && first_x_of_triangle(0) == want &&
              first_x_of_triangle(2) == want + 2.0f,
              "plain ATTR0 in a divided block reads from the block first (got %g..%g)",
              first_x_of_triangle(0), first_x_of_triangle(2));
    }
    engine_down();

    /* Indexed draw whose indices are all one value: RPCS3 skips it. */
    u8* ib = vm_base + FREQ_VTX_OFFSET + 0x700u;
    const u16 same[3] = { 2, 2, 2 }, distinct[3] = { 0, 1, 2 };
    engine_up();
    m(M_IDX_OFFSET, FREQ_VTX_OFFSET + 0x700u);
    m(M_IDX_FORMAT, 1u | (1u << 4));           /* main, u16 */
    for (u32 i = 0; i < 3; i++) { ib[i * 2] = 0; ib[i * 2 + 1] = (u8)same[i]; }
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_DRAW_INDEX_ARRAY, 0u | ((3u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.n_draw == (legacy ? 1u : 0u),
          "indexed draw with min == max is %s (draws %u)",
          legacy ? "drawn (legacy)" : "skipped", stub.n_draw);
    for (u32 i = 0; i < 3; i++) { ib[i * 2] = 0; ib[i * 2 + 1] = (u8)distinct[i]; }
    const u32 before = stub.n_draw;
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_DRAW_INDEX_ARRAY, 0u | ((3u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.n_draw == before + 1, "indexed draw over 0,1,2 is drawn");
    engine_down();

    /* Indexed line loop a b c | d e: one closing edge e -> a (RPCS3 appends
     * src[0] to the restart-split strip), not one per segment. */
    {
        const u32 cuts[1] = { 3 };
        u32 idx[16];
        const u32 n = rsx_draw_engine_topology_index_count(
            RSX_PRIMITIVE_LINE_LOOP, 5, cuts, 1);
        rsx_draw_engine_write_topology_indices(RSX_PRIMITIVE_LINE_LOOP, 5,
                                               cuts, 1, NULL, idx);
        if (legacy)
            CHECK(n == 10 && idx[4] == 2 && idx[5] == 0,
                  "legacy line loop closes each segment (%u)", n);
        else
            CHECK(n == 8 && idx[4] == 3 && idx[5] == 4 && idx[6] == 4 && idx[7] == 0,
                  "indexed line loop closes once, to the first index (%u: %u-%u)",
                  n, idx[6], idx[7]);
        const u32 lead[1] = { 0 };
        const u32 n_lead = rsx_draw_engine_topology_index_count(
            RSX_PRIMITIVE_LINE_LOOP, 3, lead, 1);
        CHECK(n_lead == (legacy ? 6u : 4u),
              "a leading restart leaves the loop open (%u)", n_lead);
    }

    /* An element whose own offset does not resolve is read linearly from the
     * array base, as RPCS3's single get_address + linear copy does. */
    const u32 lin = FREQ_VTX_OFFSET + 0x800u;
    for (u32 k = 0; k < 4; k++) {
        put_be_f32(vm_base + lin + k * 16u, (float)(10 + k));
        put_be_f32(vm_base + lin + k * 16u + 12u, 1.0f);
    }
    engine_up();
    rsx_draw_engine_set_guest_memory(hole_guest_read, NULL);
    g_hole_from = lin + 16u;
    m(M_VTXBUF_OFFSET, 0x80000000u | lin);
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_DRAW_ARRAYS, 1u | ((3u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    if (legacy)
        CHECK(stub.n_draw == 0, "legacy drops an unresolvable ATTR0 (draws %u)", stub.n_draw);
    else
        CHECK(stub.n_draw == 1 && first_x_of_triangle(0) == 11.0f &&
              first_x_of_triangle(2) == 13.0f,
              "ATTR0 past an unresolved offset reads linearly (got %g..%g)",
              first_x_of_triangle(0), first_x_of_triangle(2));
    g_hole_from = 0xFFFFFFFFu;
    rsx_draw_engine_set_guest_memory(NULL, NULL);
    engine_down();
}

static void test_vertex_input_rpcs3(void)
{
    printf("-- vertex input rules (RPCS3)\n");
    /* Vertex k of a float4 array holds x = k. */
    for (u32 k = 0; k < 8; k++) {
        const float v[4] = { (float)k, 0.0f, 0.0f, 1.0f };
        for (u32 c = 0; c < 4; c++) {
            const u32 b = f32_bits(v[c]);
            u8* p = vm_base + FREQ_VTX_OFFSET + k * 16u + c * 4u;
            p[0] = (u8)(b >> 24); p[1] = (u8)(b >> 16); p[2] = (u8)(b >> 8); p[3] = (u8)b;
        }
    }
    engine_up();
    m(M_VTXBUF_OFFSET, 0x80000000u | FREQ_VTX_OFFSET);
    /* Division by 2, DRAW_ARRAYS first 3 count 3: RPCS3 uploads from vertex 3
     * and reads 3/2 + (id-3)/2 = 1,1,2 (not (id)/2 = 1,2,2). */
    m(M_VTXFMT, 2u | (4u << 4) | (16u << 8) | (2u << 16));
    m(0x1FC0, 0u);
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_DRAW_ARRAYS, 3u | ((3u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.n_draw == 1 && first_x_of_triangle(0) == 1.0f &&
          first_x_of_triangle(1) == 1.0f && first_x_of_triangle(2) == 2.0f,
          "array division frequency reads 1,1,2 (got %g,%g,%g)",
          first_x_of_triangle(0), first_x_of_triangle(1), first_x_of_triangle(2));
    /* Modulo 4 over array vertices 3..5: (id - first) % 4 = 0,1,2. */
    m(M_VTXFMT, 2u | (4u << 4) | (16u << 8) | (4u << 16));
    m(0x1FC0, 1u);
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_DRAW_ARRAYS, 3u | ((3u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.n_draw == 2 && first_x_of_triangle(0) == 0.0f &&
          first_x_of_triangle(1) == 1.0f && first_x_of_triangle(2) == 2.0f,
          "array modulo frequency reads 0,1,2 (got %g,%g,%g)",
          first_x_of_triangle(0), first_x_of_triangle(1), first_x_of_triangle(2));
    /* Modulo 8 never reached (max index 5 < 8): the plain per-vertex fetch. */
    m(M_VTXFMT, 2u | (4u << 4) | (16u << 8) | (8u << 16));
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_DRAW_ARRAYS, 3u | ((3u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.n_draw == 3 && first_x_of_triangle(0) == 3.0f &&
          first_x_of_triangle(2) == 5.0f,
          "modulo below the draw's max index reads 3..5 (got %g..%g)",
          first_x_of_triangle(0), first_x_of_triangle(2));
    engine_down();

    /* Type 0 with a size decodes as UB256 (gcm_enums to_vertex_base_type). */
    u8* ub = vm_base + FREQ_VTX_OFFSET + 0x200u;
    for (u32 k = 0; k < 3; k++) {
        ub[k * 4 + 0] = 1; ub[k * 4 + 1] = 2; ub[k * 4 + 2] = 3; ub[k * 4 + 3] = 4;
    }
    engine_up();
    m(M_VTXBUF_OFFSET, 0x80000000u | (FREQ_VTX_OFFSET + 0x200u));
    m(M_VTXFMT, 0u | (4u << 4) | (4u << 8));
    draw_triangle();
    CHECK(stub.n_draw == 1 && stub.draw_first_vertex[0] == 1.0f &&
          stub.draw_first_vertex[1] == 2.0f && stub.draw_first_vertex[3] == 4.0f,
          "type 0 size 4 reads as UB256 (got %g,%g,%g,%g)",
          stub.draw_first_vertex[0], stub.draw_first_vertex[1],
          stub.draw_first_vertex[2], stub.draw_first_vertex[3]);

    /* Viewport scale/offset never written read the reset image (2048, 2048,
     * 0.5) and are always applied: 2048 / 128 over a 256-wide surface. */
    m(M_VTXBUF_OFFSET, 0x80000000u | VTX_OFFSET);
    m(M_VTXFMT, 2u | (4u << 4) | (16u << 8));
    draw_triangle();
    CHECK(stub.vp_xf[0] == 16.0f && stub.vp_xf[4] == 15.0f &&
          stub.vp_xf[1] == -16.0f && stub.vp_xf[5] == -15.0f,
          "unwritten viewport uses the 2048 reset (scale %g,%g offset %g,%g)",
          stub.vp_xf[0], stub.vp_xf[1], stub.vp_xf[4], stub.vp_xf[5]);
    mf(M_VIEWPORT_SCALE + 0, 0.0f); mf(M_VIEWPORT_TRANSLATE + 0, 0.0f);
    draw_triangle();
    CHECK(stub.vp_xf[0] == 0.0f && stub.vp_xf[4] == -1.0f,
          "a written zero viewport is applied, not identity (scale %g offset %g)",
          stub.vp_xf[0], stub.vp_xf[4]);
    engine_down();

    /* Immediate attributes outside VERTEX_ATTRIB_INPUT_MASK read (0,0,0,1). */
    engine_up();
    m(0x1FF0, 1u);
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    immediate_vertex(0, 0, 5, 6); immediate_vertex(1, 0, 5, 6);
    immediate_vertex(0, 1, 5, 6);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 1 && stub.captured_vertices[0][32] == 0.0f &&
          stub.captured_vertices[0][35] == 1.0f && stub.captured_vertices[1][0] == 1.0f,
          "masked-out immediate ATTR8 reads 0,0,0,1 (got %g,..,%g)",
          stub.captured_vertices[0][32], stub.captured_vertices[0][35]);
    m(0x1FF0, 0x101u);
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    immediate_vertex(0, 0, 5, 6); immediate_vertex(1, 0, 5, 6);
    immediate_vertex(0, 1, 5, 6);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 2 && stub.captured_vertices[0][32] == 5.0f,
          "an immediate ATTR8 inside the mask is fed (got %g)",
          stub.captured_vertices[0][32]);
    engine_down();

    test_vertex_gaps_rpcs3();
}

/* ---- per-draw CPU microbenchmark (non-failing) --------------------------- */

static double bench_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
#else
    return 0.0;
#endif
}

/* A 64-instruction guest VP and a texturing FP at TEX_OFFSET; one texture at
 * TEX_OFFSET + 0x1000 and a second at + 0x2000. */
static void bench_load_programs(u32 fp_off)
{
    m(0x1E9C, 0);
    for (u32 n = 0; n < 64; n++) {
        u8 vp[16];
        rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, n == 63 ? 0u : 7u, n == 63);
        for (u32 i = 0; i < 4; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0x0B80 + i * 4, w); }
    }
    m(0x1EA0, 0);
    rsx_test_fp_tex_r0_tc0(vm_base + fp_off, 0);
    m(0x08E4, fp_off | 2u);
    m(0x1D60, 64u);
}

#ifdef ENG_SAMPLER
void sampler_start(void);
void sampler_stop_report(const char* tag);
#endif

static void test_perf_bench(void)
{
    printf("-- per-draw CPU microbenchmark\n");
    const u32 N = 20000;
    engine_up();
    rsx_test_fp_tex_r0_tc0(vm_base + TEX_OFFSET + 0x800, 0);
    bench_load_programs(TEX_OFFSET);
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET + 0x1000);
    draw_triangle();
    const char* names[4] = { "same state", "VP const change/draw",
                             "texture swap/draw", "FP swap/draw" };
    for (int sc = 0; sc < 4; sc++) {
        const int pc0 = stub.n_pipeline_create;
#ifdef ENG_SAMPLER
        sampler_start();
#endif
        const double t0 = bench_now_us();
        for (u32 i = 0; i < N; i++) {
            if (sc == 1) {
                m(M_VP_UPLOAD_CONST_ID, 10u);
                for (u32 k = 0; k < 4; k++) m(M_VP_UPLOAD_CONST + k * 4, 0x3F800000u + i + k);
            } else if (sc == 2) {
                m(M_TEX_UNIT + 0x00, TEX_OFFSET + ((i & 1u) ? 0x2000u : 0x1000u));
            } else if (sc == 3) {
                m(0x08E4, (TEX_OFFSET + ((i & 1u) ? 0x800u : 0u)) | 2u);
            }
            draw_triangle();
        }
        const double us = (bench_now_us() - t0) / N;
        printf("[BENCH] %-22s %7.3f us/draw (%d pipelines created)\n",
               names[sc], us, stub.n_pipeline_create - pc0);
#ifdef ENG_SAMPLER
        sampler_stop_report(names[sc]);
#endif
    }
    m(0x08E4, TEX_OFFSET | 2u);
    engine_down();
}

/* The perf pass's memos (pipeline lookup, VP analysis, VS constant upload)
 * must see every change a draw depends on. */
static void test_perf_invalidation(void)
{
    printf("-- perf memo invalidation\n");
    engine_up();
    bench_load_programs(TEX_OFFSET);
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET + 0x1000);
    draw_triangle();
    int pc = stub.n_pipeline_create;
    const u32 p1 = stub.bound_pipeline;
    draw_triangle();
    CHECK(stub.n_pipeline_create == pc && stub.bound_pipeline == p1,
          "unchanged state reuses the pipeline");

    /* New VP ucode at the same address and entry: only the upload window. */
    {
        u8 vp[16];
        rsx_test_vp_mov_out(vp, 3, RSX_TEST_VP_SWZ_IDENT, 7u, 0);
        m(0x1E9C, 10u);
        for (u32 i = 0; i < 4; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0x0B80 + i * 4, w); }
        m(0x1E9C, 0u);
    }
    draw_triangle();
    CHECK(stub.n_pipeline_create == pc + 1 && stub.bound_pipeline != p1,
          "a VP ucode change at the same entry is a new pipeline (%d)",
          stub.n_pipeline_create - pc);
    pc = stub.n_pipeline_create;
    const u32 p2 = stub.bound_pipeline;

    /* New FP bytes at the same address, no register write. */
    rsx_test_fp_mov_r0_col0(vm_base + TEX_OFFSET, RSX_TEST_VP_SWZ_IDENT);
    draw_triangle();
    CHECK(stub.n_pipeline_create == pc + 1 && stub.bound_pipeline != p2,
          "an FP rewritten in place is a new pipeline (%d)",
          stub.n_pipeline_create - pc);
    rsx_test_fp_tex_r0_tc0(vm_base + TEX_OFFSET, 0);
    draw_triangle();
    CHECK(stub.bound_pipeline == p2, "the original FP bytes get the original pipeline back");

    /* A texture format change on a sampled unit: new pipeline key or not,
     * the lookup must run again (here the format is B8, sampled as such). */
    pc = stub.n_pipeline_create;
    m(M_TEX_UNIT + 0x04, 2u | (2u << 4) | ((0x81u | 0x20u) << 8) | (1u << 16));
    draw_triangle();
    const int fmt_new = stub.n_pipeline_create - pc;
    m(M_TEX_UNIT + 0x04, 2u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
    draw_triangle();
    CHECK(stub.bound_pipeline == p2,
          "restoring the texture format restores the pipeline (%d new on change)", fmt_new);

    /* Constants: a single slot write reaches the next draw's VS block. */
    m(M_VP_UPLOAD_CONST_ID, 10u);
    m(M_VP_UPLOAD_CONST + 0, 0x40490FDBu);
    m(M_VP_UPLOAD_CONST + 4, 0x3F800000u);
    draw_triangle();
    CHECK(stub.vp_c10[0] == 0x40490FDBu && stub.vp_c10[1] == 0x3F800000u,
          "a constant write is uploaded (%08X %08X)", stub.vp_c10[0], stub.vp_c10[1]);
    draw_triangle();
    CHECK(stub.vp_c10[0] == 0x40490FDBu, "an unchanged constant stays uploaded");
    /* Through a slot window that starts below the slot. */
    m(M_VP_UPLOAD_CONST_ID, 9u);
    m(M_VP_UPLOAD_CONST + 16, 0x11111111u);
    draw_triangle();
    CHECK(stub.vp_c10[0] == 0x11111111u, "a windowed constant write is uploaded (%08X)",
          stub.vp_c10[0]);
    /* Deferred inside a draw clause: the second range sees the new value,
     * and so does the next draw after a trailing write. */
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_DRAW_ARRAYS, 0u | ((3u - 1u) << 24));
    m(M_VP_UPLOAD_CONST_ID, 10u);
    m(M_VP_UPLOAD_CONST, 0x22222222u);
    m(M_DRAW_ARRAYS, 0u | ((3u - 1u) << 24));
    m(M_VP_UPLOAD_CONST_ID, 10u);
    m(M_VP_UPLOAD_CONST, 0x33333333u);
    m(M_BEGIN_END, 0u);
    CHECK(stub.vp_c10[0] == 0x22222222u, "a clause's deferred constant reaches its range (%08X)",
          stub.vp_c10[0]);
    draw_triangle();
    CHECK(stub.vp_c10[0] == 0x33333333u, "a clause's trailing constant reaches the next draw (%08X)",
          stub.vp_c10[0]);

    /* Texture offset change: the binding follows. */
    const u32 t1 = stub.bound_tex[0];
    m(M_TEX_UNIT + 0x00, TEX_OFFSET + 0x2000);
    draw_triangle();
    const u32 t2 = stub.bound_tex[0];
    CHECK(t2 && t2 != t1, "a texture offset change rebinds (%u -> %u)", t1, t2);
    m(M_TEX_UNIT + 0x00, TEX_OFFSET + 0x1000);
    draw_triangle();
    CHECK(stub.bound_tex[0] == t1, "and back (%u)", stub.bound_tex[0]);

    engine_down();
}

/* RSX_PERF_ZREADBACK: the combined depth+stencil read stores the same guest
 * bytes as depth_readback + stencil_readback. The stubs below describe one
 * image both ways (raw UNORM for the combined read, the floats
 * depth_readback divides it into for the separate one), so the expectations
 * hold with the kill switch either way. */
#define ZR_W 8u
#define ZR_H 2u
#define ZR_OFF 0x00140000u
static const u32 zr_raw24[ZR_W] = { 0, 1, 0x7FFFFF, 0x800000, 0xFFFFFE, 0xFFFFFF, 0xABCDEF, 0x123456 };
static u32 zr_bits;
static int zr_n_zeta, zr_n_sep;
static u32 zr_val(u32 x, u32 y)
{
    const u32 v = zr_raw24[(x + y) % ZR_W];
    return zr_bits == 16u ? v >> 8 : v;
}
static float zr_float(u32 v)
{
    return zr_bits == 16u ? (float)v / 65535.0f : (float)((double)v / 16777215.0);
}
static int zr_depth(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h, float* out, u32 pitch)
{
    (void)user; (void)depth; (void)x; (void)y;
    zr_n_sep++;
    for (u32 r = 0; r < h; r++)
        for (u32 c = 0; c < w; c++) *(float*)((u8*)out + r * pitch + c * 4u) = zr_float(zr_val(c, r));
    return 0;
}
static int zr_stencil(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h, u8* out, u32 pitch)
{
    (void)user; (void)depth; (void)x; (void)y;
    for (u32 r = 0; r < h; r++)
        for (u32 c = 0; c < w; c++) out[r * pitch + c] = (u8)(0x40u + c * 3u + r);
    return 0;
}
static int zr_zeta(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h, float* out_f,
                   u32* out_raw, u32* raw_bits, u8* out_stencil, int* have_stencil)
{
    zr_n_zeta++;
    *raw_bits = 0; *have_stencil = 0;
    if (out_raw) {
        *raw_bits = zr_bits;
        for (u32 r = 0; r < h; r++)
            for (u32 c = 0; c < w; c++) out_raw[r * w + c] = zr_val(c, r) | 0xA5000000u;  /* junk on top */
    } else {
        zr_n_sep--;
        zr_depth(user, depth, x, y, w, h, out_f, w * 4u);
    }
    if (out_stencil) { zr_stencil(user, depth, x, y, w, h, out_stencil, w); *have_stencil = 1; }
    return 0;
}
static void zr_case(u32 guest_bpp, u32 host_bits)
{
    zr_bits = host_bits; zr_n_zeta = zr_n_sep = 0;
    rsx_draw_backend be = g_stub_backend;
    be.depth_readback = zr_depth; be.stencil_readback = zr_stencil; be.zeta_readback = zr_zeta;
    memset(&stub, 0, sizeof stub);
    rsx_draw_engine_set_backend(&be);
    CHECK(rsx_draw_engine_init(32, 8) == 0, "zeta readback engine initializes");
    m(M_CONTEXT_DMA_COLOR_A, DMA_LOCAL);
    m(M_SURFACE_CLIP_H, ZR_W << 16); m(M_SURFACE_CLIP_V, ZR_H << 16);
    m(M_VTXFMT, 2u | (4u << 4) | (16u << 8)); m(M_VTXBUF_OFFSET, 0x80000000u | VTX_OFFSET);
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_SURFACE_FORMAT, 8u | ((guest_bpp == 4u ? 2u : 1u) << 5));
    m(M_ZETA_OFFSET, ZR_OFF); m(M_ZETA_PITCH, 64u); m(M_COLOR_A_OFFSET, 0x90000u);
    u8* guest = vm_base + GUEST_LOCAL_EA + ZR_OFF;
    memset(guest, 0xA6, 64u * 4u);
    m(M_DEPTH_TEST, 1); m(M_DEPTH_WRITE, 1);
    draw_triangle();
    m(M_DEPTH_TEST, 0); m(M_DEPTH_WRITE, 0);
    bind_texture_unit(RSX_LOCATION_LOCAL, ZR_OFF);
    m(M_TEX_UNIT + 0x18, (ZR_W << 16) | ZR_H); m(0x1840, 64u);
    draw_triangle();
    CHECK(zr_n_zeta + zr_n_sep == 1, "one depth read for the store (%d combined, %d separate)",
          zr_n_zeta, zr_n_sep);
    {
        const char* a = getenv("RSX_PERF_ALL"); const char* z = getenv("RSX_PERF_ZREADBACK");
        if (!(a && *a == '0') && !(z && *z == '0'))
            CHECK(zr_n_zeta == 1, "the store uses the combined read (%d)", zr_n_zeta);
    }
    int ok = 1;
    for (u32 y = 0; y < ZR_H; y++)
        for (u32 x = 0; x < ZR_W; x++) {
            const u8* q = guest + y * 64u + x * guest_bpp;
            const double d = (double)zr_float(zr_val(x, y));
            if (guest_bpp == 4u) {
                const u32 want = ((u32)(d * 16777215.0 + 0.5) << 8) | (0x40u + x * 3u + y);
                ok &= q[0] == (u8)(want >> 24) && q[1] == (u8)(want >> 16) &&
                      q[2] == (u8)(want >> 8) && q[3] == (u8)want;
            } else {
                const u32 want = (u32)((float)d * 65535.0f + 0.5f);
                ok &= q[0] == (u8)(want >> 8) && q[1] == (u8)want;
            }
        }
    CHECK(ok, "Z%u store from a %u-bit host depth matches the float path", guest_bpp == 4u ? 24u : 16u, host_bits);
    CHECK(guest[ZR_W * guest_bpp] == 0xA6, "row padding untouched");
    engine_down();
}
static void test_zeta_readback(void)
{
    zr_case(4u, 24u);
    zr_case(2u, 16u);
    zr_case(2u, 24u);
    zr_case(4u, 16u);
}

#ifdef _WIN32
/* The host's always-writable second view (project main.cpp); a separate
 * buffer here, so where a store lands shows which view it went through. */
u8* vm_sudo_base;

static DWORD page_prot(const void* p)
{
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(p, &mbi, sizeof mbi);
    return mbi.Protect;
}

/* RPCS3 flushes a guarded section through the super pointer and lifts the
 * protection after: the WCB store goes through vm_sudo_base while the page
 * is still NOACCESS. RSX_ENG_SUDO=0 (or no view) lifts it first and stores
 * through vm_base. */
static void test_wcb_sudo_store(void)
{
    printf("-- WCB store through the sudo view\n");
    u8* shadow = (u8*)VirtualAlloc(NULL, GUEST_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!shadow) { CHECK(0, "shadow view"); return; }
    u8* const px = vm_base + GUEST_LOCAL_EA + 0x40000u + 3u * 1024u + 2u * 4u;
    for (int off = 0; off < 2; off++) {
        if (off) setenv("RSX_ENG_SUDO", "0", 1);
        memset(vm_base + GUEST_LOCAL_EA + 0x40000u, 0, 256u * 1024u);
        memset(shadow, 0, GUEST_BYTES);
        vm_sudo_base = shadow;
        engine_up();
        draw_triangle();
        const DWORD guarded = page_prot(px);
        g_stub_ramp = 1;
        rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, 0x40000u + 3u * 1024u, 1024u, 4u, 1u);
        g_stub_ramp = 0;
        const DWORD after = page_prot(px);
        const u8* sp = shadow + (px - vm_base);
        if (!off)
            CHECK(guarded == PAGE_NOACCESS && after == PAGE_READWRITE && sp[0] == 0x80 &&
                  sp[1] == 9u && px[0] == 0 && px[1] == 0,
                  "the store goes through the sudo view, the guard lifts after (0x%lX 0x%lX %02X %02X)",
                  guarded, after, sp[0], px[0]);
        else
            CHECK(after == PAGE_READWRITE && sp[0] == 0 && px[0] == 0x80 && px[1] == 9u,
                  "RSX_ENG_SUDO=0 stores through vm_base (%02X %02X)", sp[0], px[0]);
        engine_down();
        vm_sudo_base = NULL;
        if (off) unsetenv("RSX_ENG_SUDO");
    }
    VirtualFree(shadow, 0, MEM_RELEASE);
}

/* A fault on the engine's own thread outside any engine call (a guest flip
 * or vblank handler it runs) stores the surfaces there before the page opens;
 * RSX_ENG_WCB_SELF_FLUSH=0 only opens it. */
static void test_wcb_self_flush(void)
{
    printf("-- WCB fault on the engine thread\n");
    for (int off = 0; off < 2; off++) {
        setenv("RSX_ENG_WCB_SELF_FLUSH", off ? "0" : "1", 1);
        memset(vm_base + GUEST_LOCAL_EA + 0x40000u, 0, 256u * 1024u);
        engine_up();
        draw_triangle();
        volatile u8* px = vm_base + GUEST_LOCAL_EA + 0x40000u + 3u * 1024u + 2u * 4u;
        const DWORD guarded = page_prot((const void*)px);
        g_stub_ramp = 1;
        const u8 v = px[0];
        g_stub_ramp = 0;
        if (!off)
            CHECK(guarded == PAGE_NOACCESS && v == 0x83,
                  "the guest handler reads the surface's pixel (0x%lX %02X)", guarded, v);
        else
            CHECK(guarded == PAGE_NOACCESS && v == 0,
                  "RSX_ENG_WCB_SELF_FLUSH=0 opens the page unstored (%02X)", v);
        engine_down();
    }
    setenv("RSX_ENG_WCB_SELF_FLUSH", "0", 1);
}

#define WCB_T 65
static u8 s_wcb_seen[WCB_T];
static DWORD WINAPI wcb_guest_thread(LPVOID arg)
{
    const u32 k = (u32)(uintptr_t)arg;
    s_wcb_seen[k] = *(volatile u8*)(vm_base + GUEST_LOCAL_EA + 0x40000u + k * 4096u);
    return 0;
}

/* Guest threads touching guarded surface pages wait for the engine's
 * thread to store them; one finding the request queue (64) full waits for
 * a slot instead of opening its page unstored. */
static void test_wcb_queue_full(void)
{
    printf("-- WCB request queue full\n");
    const u32 h = 300u;                    /* 75 pages at pitch 1024 */
    memset(vm_base + GUEST_LOCAL_EA + 0x40000u, 0, h * 1024u);
    memset(s_wcb_seen, 0xEE, sizeof s_wcb_seen);
    engine_up();
    m(M_SURFACE_CLIP_V, h << 16);
    draw_triangle();
    HANDLE th[WCB_T];
    for (u32 k = 0; k < WCB_T - 1; k++)
        th[k] = CreateThread(NULL, 0, wcb_guest_thread, (LPVOID)(uintptr_t)k, 0, NULL);
    Sleep(150);
    th[WCB_T - 1] = CreateThread(NULL, 0, wcb_guest_thread, (LPVOID)(uintptr_t)(WCB_T - 1), 0, NULL);
    Sleep(100);
    const u8* last = vm_base + GUEST_LOCAL_EA + 0x40000u + (WCB_T - 1) * 4096u;
    const int waiting = WaitForSingleObject(th[WCB_T - 1], 0) == WAIT_TIMEOUT;
    const DWORD prot = page_prot(last);
    g_stub_ramp = 1;
    rsx_draw_engine_wcb_service();
    WaitForMultipleObjects(WCB_T - 1, th, TRUE, 5000);
    WaitForSingleObject(th[WCB_T - 1], 5000);
    g_stub_ramp = 0;
    int ok = 1;
    for (u32 k = 0; k < WCB_T; k++) {
        if (s_wcb_seen[k] != (u8)(0x80u + 4u * k)) { ok = 0; printf("   page %u read %02X\n", k, s_wcb_seen[k]); }
        CloseHandle(th[k]);
    }
    CHECK(waiting && prot == PAGE_NOACCESS,
          "a fault on a full queue waits with its page still guarded (%d 0x%lX)", waiting, prot);
    CHECK(ok, "every faulting thread reads the stored surface");
    engine_down();
}

/* The host's unclaimed-fault fallback goes through the registry: protection
 * comes back from the owner mask, or the stale owners are dropped and the
 * page opened, so the mask and the page agree afterwards. */
static void test_page_guard_reconcile(void)
{
    printf("-- page guard reconcile\n");
    engine_up();
    const u32 ea = GUEST_LOCAL_EA + 0x200000u;
    u8* p = vm_base + ea;
    CHECK(rsx_page_guard_set(ea, 1u, 1) && page_prot(p) == PAGE_NOACCESS, "guarded");
    DWORD old;
    VirtualProtect(p, 4096, PAGE_READONLY, &old);          /* protection lost */
    CHECK(rsx_page_guard_reconcile(ea, 0) == 1 && page_prot(p) == PAGE_READWRITE,
          "an owner still guarding a faulting page is dropped and the page opened");
    CHECK(rsx_page_guard_set(ea, 1u, 1) && page_prot(p) == PAGE_NOACCESS,
          "the owner guards it again from a clean mask");
    rsx_page_guard_set(ea, 1u, 0);
    CHECK(page_prot(p) == PAGE_READWRITE, "and lifting it opens the page");
    VirtualProtect(p, 4096, PAGE_NOACCESS, &old);          /* nobody's guard */
    CHECK(rsx_page_guard_reconcile(ea, 1) == 1 && page_prot(p) == PAGE_READWRITE,
          "a page no owner guards goes back to read-write");
    engine_down();
}
#endif

int main(void)
{
    /* The compressed-AA tile model is opt-in (cached on first read); the
     * suite was written with it on. */
    setenv("RSX_ENG_TILE_AA_COMPRESS", "1", 1);
    /* The test thread is the engine's and pokes guarded guest memory as
     * the guest would from another thread: no store on that touch. */
    setenv("RSX_ENG_WCB_SELF_FLUSH", "0", 1);
    /* Page aligned, as the runtime's guest memory: WCB guards whole 4 KB
     * guest pages, and an unaligned base would protect a neighbour too. */
#ifdef _WIN32
    vm_base = (u8*)_aligned_malloc(GUEST_BYTES, 4096);
    if (vm_base) memset(vm_base, 0, GUEST_BYTES);
#else
    vm_base = (u8*)calloc(1, GUEST_BYTES);
#endif
    if (!vm_base) { printf("[FAIL] guest memory\n"); return 1; }
    ppu_vm_size = GUEST_BYTES;
    /* Three vertices of float4 position, so the fetch has something real to
     * read: the engine treats an unreadable ATTR0 as a failed group. */
    for (u32 i = 0; i < 3 * 4; i++) {
        u8* p = vm_base + VTX_OFFSET + i * 4;
        p[0] = 0x3F; p[1] = 0x80; p[2] = 0; p[3] = 0;    /* 1.0f big-endian */
    }

    test_surface_set();
    if (getenv("ENG_BENCH_ONLY") && *getenv("ENG_BENCH_ONLY")) {
        test_perf_bench();
        return g_failures ? 1 : 0;
    }
    test_surface_aliasing();
    test_rtt_mip_gather();
    test_mrt_set();
    test_texture_cache();
    test_texture_sparse_rehash();
    test_texcache_coherence();
    test_texcache_round2();
    test_pipeline_key();
    test_restart_expansion();
    test_indexed_draws();
    test_scissor();
    test_readback();
    test_custom_guest_mapping();
    test_queued_flip_buffer();
    test_transfer_coherence();
    test_tiled_memory();
#ifdef _WIN32
    test_wcb_sudo_store();
    test_wcb_self_flush();
    test_wcb_queue_full();
    test_page_guard_reconcile();
#endif
    test_display_buffer_not_stored();
    test_transfer_rows();
    test_transfer_reads_fp16();
    test_inline_array();
    test_depth_clear_value();
    test_rpcs3_clear_rules();
    test_surface_store_rules();
    test_surface_inheritance();
    test_zeta_exact();
    test_native_16bit_targets();
    test_tile_aa_compress();
    test_feedback_gather();
    test_texture_tag_drop();
    test_stale_erase();
    test_surface_split();
    test_window_clip();
    test_typeless_surfaces();
    test_blit_depth_source();
    test_blit_creates_offset_target();
    test_blit_create_16_and_trunc();
    test_blit_mirror();
    test_draw_clause();
    test_flip_reset();
    test_clear_formats();
    test_flat_shading();
    test_antialiased_surface();
    test_depth_read_as_color();
    test_color_read_as_float();
    test_packed_color_store();
    test_dxt_volume_vtc();
    test_unwrap_cube_3d();
    test_immediate_compositor();
    test_immediate_packed_zero_and_inline();
    test_vertex_input_rpcs3();
    test_present_aspect();
    test_range_read_skips_depth();
    test_hw_srgb_view();
    test_host_vendor_fp();
    test_x8_alpha_blend();
    test_signed_blend();
    test_perf_invalidation();
    test_zeta_readback();
    test_perf_bench();

#ifdef _WIN32
    _aligned_free(vm_base);
#else
    free(vm_base);
#endif
    printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n",
           g_failures);
    return g_failures ? 1 : 0;
}
