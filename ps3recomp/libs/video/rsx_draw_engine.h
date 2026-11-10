/*
 * ps3recomp - platform-neutral RSX draw engine over the register file
 *
 * The vtable backends in rsx_commands.c are driven by rsx_state, a curated
 * dirty-flag struct that models no primitive restart, no vertex textures and
 * no two-sided stencil, and whose setters fire only at SET_BEGIN_END. The
 * register-file dispatcher in rsx_dispatch.c has all of it already, in
 * portable C, behind the rsx_dispatch_sink interface. This engine implements
 * that sink and drives a backend through the record-oriented interface below,
 * so a title's frame reaches the host API the way the hardware issued it.
 *
 * The engine logic is carried across from libs/video/rsx_live_draw.c, the
 * NV4097 -> D3D12 engine a title has actually shipped on. That file is
 * vendored and is NOT touched: what moves here is the orchestration, and the
 * comments name the reference line numbers so the two can be compared. The
 * mathematics is already shared and already portable -- rsx_vertex_compact.c,
 * rsx_restart_cuts.h, rsx_texture_layout.c, the two decompilers.
 *
 * The split between engine and backend is semantic, at the granularity the
 * engine thinks in, not a 1:1 wrapper over one host API's calls: resources by
 * what they are for, pipelines from the two HLSL strings plus render state
 * plus the vertex layout, and per-draw binding of what a draw names. Resource
 * state tracking, descriptor lifetime and command encoding belong entirely to
 * the backend, which is why nothing here mentions a barrier.
 *
 * Handles are small integers owned by the backend; 0 always means "none".
 */
#ifndef PS3RECOMP_RSX_DRAW_ENGINE_H
#define PS3RECOMP_RSX_DRAW_ENGINE_H

#include "rsx_dispatch.h"
#include "rsx_primitives.h"
#include "rsx_texture_layout.h"
#include "rsx_vertex_compact.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- what the engine hands a backend ------------------------------------ */

/* Surface and texture formats the engine asks for. The texture side is
 * rsx_texfmt's classification, which rsx_texture_decode already produces host
 * rows for, so a backend maps one enum rather than two. */
typedef enum {
    RSX_BE_FMT_NONE = 0,
    RSX_BE_FMT_R8,
    RSX_BE_FMT_R8G8,
    RSX_BE_FMT_R8G8B8A8,      /* decoded colour: always R,G,B,A byte order */
    RSX_BE_FMT_BC1,
    RSX_BE_FMT_BC2,
    RSX_BE_FMT_BC3,
    RSX_BE_FMT_R16,
    RSX_BE_FMT_R16G16,
    RSX_BE_FMT_R16G16F,
    RSX_BE_FMT_R16G16B16A16F, /* SET_SURFACE_FORMAT 0xB, the HDR target    */
    RSX_BE_FMT_R32F,
    RSX_BE_FMT_R32G32B32A32F,
    /* Native 16-bit colour targets (RPCS3 VKGSRender.cpp:43-60); appended so
     * the values above keep their numbers. */
    RSX_BE_FMT_R5G6B5,        /* SURFACE_FORMAT 3: R5G6B5_UNORM_PACK16      */
    RSX_BE_FMT_A1R5G5B5       /* SURFACE_FORMAT 1/2: A1R5G5B5_UNORM_PACK16  */
} rsx_be_format;

/* One texture unit's sampler, decoded out of SET_TEXTURE_FILTER,
 * SET_TEXTURE_ADDRESS and SET_TEXTURE_CONTROL0 the way rsx_live_draw.c's
 * decode_sampler (1464-1487) reads them. The wrap fields stay as the guest's
 * own 1..8 codes because that is what both backends already translate. */
typedef struct rsx_be_sampler_desc {
    u8  min_linear, mag_linear, mip_linear, mip_present;
    u8  wrap_s, wrap_t, wrap_r;
    float min_lod, max_lod;
    /* SET_TEXTURE_FILTER [12:0], signed 4.8 fixed point (RPCS3
     * fragment_texture::bias), and SET_TEXTURE_BORDER_COLOR, raw A8R8G8B8. */
    float lod_bias;
    u32 border_color;
    /* SET_TEXTURE_CONTROL0 [6:4] as a level (1,2,4,6,8,10,12,16), or the
     * RSX_ANISO override (RPCS3 anisotropic_level_override). */
    u8 max_aniso;
    /* The border colour as RPCS3's color4f (R,G,B,A), before any clamping:
     * a host-SNORM view's lanes can be negative. border_color is the same
     * colour packed and clamped. For VK_EXT_custom_border_color. */
    float border_rgba[4];
} rsx_be_sampler_desc;

/* The NV4097 render state a pipeline is built from: rsx_live_draw.c's
 * render_state_t (1312-1327) with the D3D12 enums left undecoded, since the
 * guest values are the portable form and each backend has its own table.
 * The stencil REFERENCE is deliberately absent -- it is set on the encoder,
 * not baked into the pipeline, and so is not part of the pipeline key. */
typedef struct rsx_be_render_state {
    u32 alpha_test_enable, alpha_func, alpha_ref_raw, alpha_ref_format;
    u32 blend_enable, sf_rgb, df_rgb, sf_a, df_a, eq_rgb, eq_a;
    u32 depth_test, depth_write, depth_func;
    u32 cull_enable, cull_face, front_face;
    u32 color_mask;
    /* Targets 1..3 of an MRT set: COLOR_MASK_MRT (0x0370), one nibble per
     * target at [4i..4i+3] = A,R,G,B, and BLEND_ENABLE_MRT (0x036C), bit i.
     * Target 0 keeps color_mask/blend_enable. Both reset to 0 (RPCS3). */
    u32 color_mask_mrt, blend_mrt;
    u32 rt_fp16;                    /* the target is an FP16 HDR surface */
    u32 stencil_enable, stencil_two_sided;
    u32 depth_bounds_test;          /* SET_DEPTH_BOUNDS_TEST_ENABLE */
    u32 s_func, s_func_mask, s_write_mask, s_fail, s_zfail, s_zpass;
    u32 bs_func, bs_fail, bs_zfail, bs_zpass;
    /* SET_LOGIC_OP_ENABLE / SET_LOGIC_OP (GL enums 0x1500..0x150F). An
     * enabled logic op replaces blending (RPCS3 VKGSRender.cpp). */
    u32 logic_op_enable, logic_op;
    /* SET_BLEND_COLOR (A8R8G8B8, or R16G16 of an FP16 target with
     * SET_BLEND_COLOR2 holding B16A16). Dynamic state: not in the
     * pipeline key. */
    u32 blend_color, blend_color2;
    /* RPCS3 get_pipeline_properties: depth clamp is on when ZMIN_MAX_CONTROL
     * (0x1D78) enables clamping [4:7] or disables clipping [0:3]. */
    u32 depth_clamp;
    /* BACK_STENCIL_FUNC_MASK (0x0358) / BACK_STENCIL_MASK (0x034C), used
     * only when two-sided stencil is on. */
    u32 bs_func_mask, bs_write_mask;
    /* FRONT_POLYGON_MODE (0x1828) / BACK_POLYGON_MODE (0x182C), normalised
     * to POINT 0x1B00 / LINE 0x1B01 / FILL 0x1B02 (anything else, including
     * a never-written 0, reads as FILL like RPCS3's to_polygon_mode). */
    u32 polygon_mode_front, polygon_mode_back;
    /* RPCS3 hardware MSAA: samples of the bound surface's SURFACE_FORMAT
     * antialias mode (1, 2 or 4) and ANTI_ALIASING_CONTROL (0x1D7C) raw:
     * [0] enable, [4] alpha-to-coverage, [8] alpha-to-one, [31:16] mask. */
    u32 aa_samples, aa_control;
    /* RPCS3 programmable blending on an X1/X8 _Z (1) or _O (2) target whose
     * blend reads alpha (RSXGraphEngine.cpp:73-112): the blend sees the
     * shader's raw alpha, and only the stored alpha is the constant 0 / 1.
     * The factors above are already rewritten for the constant destination
     * alpha, and the shader must not remap its alpha output. */
    u32 x_alpha_pb;
} rsx_be_render_state;

/* submit_and_wait reasons, for a backend that wants to log or count them. */
#define RSX_BE_FLUSH_VERTEX_RING     0
#define RSX_BE_FLUSH_GUEST_REFERENCE 1
#define RSX_BE_FLUSH_SHUTDOWN        2

/* clear_depth_stencil flags */
#define RSX_BE_CLEAR_DEPTH   0x1u
#define RSX_BE_CLEAR_STENCIL 0x2u

#define RSX_BE_MAX_TEXTURES        RSX_DSP_NUM_TEXTURES
#define RSX_BE_MAX_VERTEX_TEXTURES RSX_DSP_NUM_VERTEX_TEXTURES
/* Colour targets a draw can name: A alone, or an MRT set of A, B, C and D. */
#define RSX_BE_MAX_COLOR_TARGETS   4

/* How many distinct guest textures the engine keeps before it evicts the
 * least recently used one. Public so the eviction test can be exact about
 * where the boundary is rather than assuming a number. */
#define RSX_DRAW_ENGINE_TEXTURE_CACHE 1024

/* ---- the backend interface ---------------------------------------------- */

/* scene_aa: one temporal antialiasing/upscaling pass over the scene, before
 * the UI is drawn over it (see rsx_draw_engine_set_aa_hooks). */
typedef struct rsx_be_scene_aa {
    u32   color, depth;          /* engine target handles                   */
    u32   w, h;                  /* region from (0,0), target pixels        */
    float jitter_px[2];          /* sub-pixel offset the scene was drawn
                                  * with, target pixels, +x right, +y down  */
    float eval_jitter[2];        /* what the upscaler is told (sign/axes as
                                  * it expects), render pixels              */
    float reproject[16];         /* row-major: current unjittered NDC (x, y
                                  * down, z = zA*depth+zB, 1) -> previous   */
    float zA, zB;
    u32   inverted;              /* depth test GREATER: near = 1            */
    u32   reset;                 /* history invalid (cut, first frame)      */
    float frame_ms;
    u32   debug_view;            /* 0 off, 1 difference, 2 split screen     */
    u32   use_mask;              /* reactive mask from depth disagreement   */
} rsx_be_scene_aa;

#define RSX_BE_HAS_COLOR_SNAPSHOT 1
typedef struct rsx_draw_backend {
    void* user;

    /* Lifecycle and submission. submit_and_wait is fully synchronous: the
     * engine recycles its staging the moment it returns, which is the same
     * contract as rsx_live_draw.c's ld_flush (1659-1736). */
    int  (*init)(void* user, u32 width, u32 height);
    void (*shutdown)(void* user);
    void (*submit_and_wait)(void* user, u32 reason);

    /* Textures. `remap` is the raw TEXTURE_CONTROL1 crossbar and `rsx_fmt`
     * the SET_TEXTURE_FORMAT byte it is decoded against, because the crossbar
     * is a property of the view rather than of the pixels. Levels arrive one
     * upload each, already decoded to host rows by rsx_texture_decode. */
    u32  (*texture_create)(void* user, rsx_be_format fmt, u32 w, u32 h,
                           u32 mips, u32 faces, u32 remap, u32 rsx_fmt);
    void (*texture_upload)(void* user, u32 texture, u32 face, u32 mip,
                           u32 w, u32 h, const void* src, u32 row_bytes,
                           u32 rows);
    void (*texture_release)(void* user, u32 texture);

    /* Colour targets. `seed` is the guest's own bytes for the surface,
     * decoded to R,G,B,A rows, or NULL when they could not be resolved: a
     * title CPU-initialises its render targets and then samples them before
     * anything draws into them, which is why the D3D12 backend's off_rt_get
     * seeds a new target the same way. */
    u32  (*color_target_create)(void* user, rsx_be_format fmt, u32 w, u32 h,
                                const void* seed, u32 seed_row_bytes);
    void (*color_target_release)(void* user, u32 surface);
    /* Optional. Write R,G,B,A rows into a rectangle of a colour target: pixels
     * the guest put there outside any draw, with a 2D transfer. NULL when the
     * backend cannot, and the engine then leaves the target as it is. */
    void (*color_target_write)(void* user, u32 surface, u32 x, u32 y, u32 w,
                               u32 h, const void* rows, u32 row_bytes);
    /* A texture handle for a colour target a texture unit names, wearing that
     * unit's TEXTURE_CONTROL1 crossbar. An uploaded texture bakes the crossbar
     * in when it is decoded, so only a surface bind needs this; 0 means the
     * backend has no view and the engine binds the target itself. */
    u32  (*surface_view)(void* user, u32 surface, u32 remap, u32 rsx_format);

    /* One depth target per guest zeta address. */
    u32  (*depth_target_create)(void* user, u32 w, u32 h);
    void (*depth_target_release)(void* user, u32 depth);
    /* Resolve a depth target into a sampleable texture and return it; 0 when
     * the backend cannot. The engine only asks after a depth-writing draw. */
    u32  (*depth_snapshot)(void* user, u32 depth, u32 w, u32 h);

    /* Pipelines, from the decompilers' HLSL plus the state that selects a
     * variant. This is the one call that hides D3DCompile against glslang
     * plus spirv-cross plus -newLibraryWithSource:, and the input layout with
     * it. 0 means the pair could not be built; the engine caches that.
     *
     * `rt_count` is how many colour attachments the pass will have, and every
     * one of them takes target A's blend and colour mask: a zero-initialised
     * secondary attachment writes nothing at all, which would leave a
     * deferred pass with only its first G-buffer plane. It is part of the
     * engine's pipeline key, because a host API matches a pipeline's
     * attachments against the pass it is encoded in. */
    u32  (*pipeline_create)(void* user, const char* vs_hlsl, const char* ps_hlsl,
                            const rsx_be_render_state* rs,
                            const rsx_vertex_layout_plan* layout,
                            u32 vertex_stride, rsx_be_format rt_fmt,
                            u32 rt_count);
    void (*pipeline_release)(void* user, u32 pipeline);

    /* Per-draw binding. The backend owns every ring the D3D12 engine used to
     * own itself, so the engine hands over CPU-side arrays and never mentions
     * a descriptor. */
    /* The colour targets this draw writes, and the depth attachment under
     * them. `count` is 1 for an ordinary draw and up to
     * RSX_BE_MAX_COLOR_TARGETS for an MRT set, whose members are all target
     * A's size and format -- the rule both render paths bind by, and the one
     * a host API enforces on a pass's attachments. */
    void (*bind_targets)(void* user, const u32* surfaces, u32 count, u32 depth);
    void (*bind_pipeline)(void* user, u32 pipeline);
    void (*bind_vs_constants)(void* user, const void* data, u32 bytes);
    void (*bind_ps_constants)(void* user, const void* data, u32 bytes);
    void (*bind_textures)(void* user, const u32* textures,
                          const rsx_be_sampler_desc* samplers, u32 mask);
    void (*bind_vertex_textures)(void* user, const u32* textures,
                                 const rsx_be_sampler_desc* samplers, u32 mask);
    void (*set_viewport)(void* user, float x, float y, float w, float h);
    void (*set_scissor)(void* user, u32 x, u32 y, u32 w, u32 h);
    void (*set_stencil_ref)(void* user, u32 ref);
    /* Optional: the depth bounds for the next draw, called before every
     * draw like the stencil reference. The test itself is pipeline state
     * (rsx_be_render_state.depth_bounds_test); a backend that leaves this
     * NULL draws as if the test were off. */
    void (*set_depth_bounds)(void* user, float min, float max);
    /* Optional: SET_BLEND_COLOR decoded to floats (RGBA) for the CONSTANT_*
     * blend factors, called before every draw. */
    void (*set_blend_constants)(void* user, const float rgba[4]);
    /* One draw. `indices` is NULL for a non-indexed one. Everything the
     * hardware has and the host does not -- quads, quad strips, fans,
     * polygons -- has already been expanded into a triangle list by the time
     * it gets here, so the topology is only ever one a host API has. */
    void (*draw)(void* user, rsx_topology topology, const void* vertices,
                 u32 vertex_count, u32 stride, const u32* indices,
                 u32 index_count);

    /* Clears, which are ordered against draws rather than folded into a pass:
     * a title draws the world, clears depth, and draws the weapon over it. */
    void (*clear_color)(void* user, u32 surface, const float rgba[4]);
    void (*clear_depth_stencil)(void* user, u32 depth, u32 flags,
                                float depth_value, u8 stencil);

    /* Present the named surface, and read a rectangle of one back. The rows
     * come back in the format the target was created with, so R,G,B,A for an
     * ordinary colour surface. */
    void (*present)(void* user, u32 surface);
    void (*readback)(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                     void* out, u32 out_pitch);

    /* Optional: read a rectangle of a depth target back as floats in [0, 1],
     * `out_pitch` bytes per row. Returns 0 on success. The engine uses it to
     * store a depth buffer into guest memory when a texture reads those bytes
     * in a format of its own (a D24S8 buffer as A8R8G8B8, a Z16 shadow map as
     * DEPTH16); left NULL, such a texture reads guest memory as it is. */
    int  (*depth_readback)(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h,
                           float* out, u32 out_pitch);
    /* Optional: the stencil aspect of the same rectangle, one byte per pixel,
     * `out_pitch` bytes per row. Returns 0 on success. Left NULL (or failing),
     * a Z24S8 store writes stencil 0. */
    int  (*stencil_readback)(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h,
                             u8* out, u32 out_pitch);
    /* Optional same-target colour feedback. Return a fresh, immutable texture
     * of the target's full physical extent and storage format. Its GPU copy
     * must execute after all preceding writes and before the next draw. The
     * engine owns the returned handle and releases it with texture_release.
     * 0 rejects the consuming draw; NULL retains the legacy guest-upload route. */
    u32 (*color_snapshot)(void* user, u32 surface);

    /* Optional. 3D textures (SET_TEXTURE_FORMAT dimension 3, CONTROL3 depth):
     * a volume of `depth` slices per level, each slice one upload of decoded
     * rows. A backend that leaves these NULL gets the first slice as a 2D
     * texture through texture_create/texture_upload, as before. */
    u32  (*texture_create_3d)(void* user, rsx_be_format fmt, u32 w, u32 h,
                              u32 depth, u32 mips, u32 remap, u32 rsx_fmt);
    void (*texture_upload_3d)(void* user, u32 texture, u32 z, u32 mip,
                              u32 w, u32 h, const void* src, u32 row_bytes);

    /* Optional. A fresh, immutable texture holding the rectangle (x, y, w, h)
     * of a colour target, in the target's storage format: what a texture unit
     * sees when it names a surface's address and pitch but only part of its
     * extent (a 1152x640 scene inside a 1280x704 target, an AA sample grid).
     * Ordered like color_snapshot; the engine owns and releases the handle.
     * NULL or 0: the engine binds the whole target, as before. */
    u32 (*color_region_snapshot)(void* user, u32 surface, u32 x, u32 y,
                                 u32 w, u32 h);

    /* Optional. Copies the w x h rectangle at (sx, sy) of colour target
     * `surface` (guest pixels) into level `mip`, cube face / volume slice
     * `layer`, at (0, 0) of a texture made by texture_create(_3d) in the
     * target's storage format, ordered after every earlier draw (RPCS3
     * cubemap_unwrap / _3d_unwrap). Returns 0 when it cannot: the engine
     * then reads the texture from guest memory, as before. */
    int (*texture_copy_from_color)(void* user, u32 texture, u32 layer, u32 mip,
                                   u32 surface, u32 sx, u32 sy, u32 w, u32 h);

    /* Optional. Scaled, linearly filtered copy of rectangle (sx, sy, sw, sh)
     * of colour target `src` onto (dx, dy, dw, dh) of colour target `dst`,
     * both of one storage format, ordered after all preceding writes and
     * before the next draw. The engine uses it when a guest rebinds a surface
     * address with a different antialias mode: RPCS3 then makes a new surface
     * that inherits the old one's contents resolved (or spread) to the new
     * sample grid. NULL: the new surface starts from guest memory. */
    void (*color_target_blit)(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                              u32 dst, u32 dx, u32 dy, u32 dw, u32 dh);

    /* RPCS3's flip: present only the display buffer's rectangle (sx,sy,sw,sh
     * in surface pixels) of the named surface, letterboxed to aspect_w :
     * aspect_h (0 = stretch). NULL: present() of the whole surface. */
    void (*present_region)(void* user, u32 surface, u32 sx, u32 sy, u32 sw, u32 sh,
                           u32 aspect_w, u32 aspect_h);

    /* Optional. RPCS3's typeless / bitcast framebuffer sampling, done on the
     * GPU: a fresh immutable texture of w x h texels holding exactly what
     * texture_create + texture_upload would hold had the rectangle (x, y, w,
     * h) of `src` been stored to guest memory in its guest layout and then
     * decoded (rsx_texture_decode) as linear texture format `tex_base`
     * (format & 0x9F), viewed through `remap` / `rsx_fmt` like
     * texture_create's. `src_kind`: RSX_BE_REINTERPRET_* below. The engine
     * owns and releases the handle (texture_release). Ordered like
     * color_snapshot. NULL or 0: the engine stores the surface to guest
     * memory and decodes it from there (a GPU stall). */
    u32 (*surface_reinterpret)(void* user, u32 src, u32 src_kind,
                               u32 x, u32 y, u32 w, u32 h,
                               u32 tex_base, u32 remap, u32 rsx_fmt);

    /* Optional. color_target_blit with the filter chosen: `linear` 0 is
     * point sampling (NV3089 zero-order hold), 1 bilinear (first-order
     * hold), as RPCS3's blit engine picks from the transfer's interpolator.
     * NULL: color_target_blit (always linear). */
    void (*color_target_blit_filtered)(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                                       u32 dst, u32 dx, u32 dy, u32 dw, u32 dh,
                                       int linear);

    /* Optional (all four or none). Host occlusion queries for RPCS3's ZCULL
     * ZPASS_PIXEL_CNT reports. `id` names a logical query (0..4095) chosen by
     * the caller; every draw recorded between occlusion_begin and
     * occlusion_end counts towards it. begin returns 0 when it cannot start
     * one. occlusion_result returns 1 with the summed sample count in *out
     * once the GPU has it (wait != 0: submit and block until it does), else
     * 0. discard frees a query whose result is not wanted. NULL: the caller
     * reports everything visible. occlusion_result with wait == 2 only
     * submits the recorded work holding the query (no wait, no result). */
    int  (*occlusion_begin)(void* user, u32 id);
    void (*occlusion_end)(void* user, u32 id);
    int  (*occlusion_result)(void* user, u32 id, int wait, u64* out);
    void (*occlusion_discard)(void* user, u32 id);

    /* Optional. RPCS3 VKDraw update_draw_state's per-draw rasteriser state:
     * depth bias (POLYGON_OFFSET_FILL_ENABLE 0x0A68 with SCALE 0x0A78 as
     * the slope factor and BIAS 0x0A7C as the constant one; zero when
     * disabled), LINE_WIDTH (0x03B8, 5.3 fixed point, already decoded) for
     * point and line primitives, and the back-face stencil reference
     * (BACK_STENCIL_FUNC_REF 0x0354) when two-sided stencil is on, else the
     * front reference. NULL: no bias, width 1, one shared reference. */
    void (*set_raster_dynamic)(void* user, u32 poly_offset_fill, float scale,
                               float bias, float line_width, u32 back_stencil_ref);

    /* Optional. RPCS3 VKDraw hardware texel remapping (hw_SNORM_possible):
     * a view of a texture_create texture reading its 8/16-bit UNORM
     * components as SNORM, so a fully signed texture filters in hardware
     * instead of point-sampling the shader's sign extension. Returns 0 when
     * the format has no SNORM twin. NULL: signed reads stay in the shader. */
    u32 (*texture_snorm_view)(void* user, u32 texture);
    /* Optional. RPCS3 VKDraw hardware texel remapping (hw_SRGB_possible,
     * VKFormats.cpp get_compatible_srgb_format): a view of a texture_create
     * texture reading R8 / R8G8 / R8G8B8A8 / BC1-3 as their _SRGB twins, so
     * gamma is decoded before filtering. texture == 0 is a probe: nonzero
     * when textures of `fmt` can be viewed that way on this device. Returns 0
     * when they cannot. NULL: gamma stays in the shader. */
    u32 (*texture_srgb_view)(void* user, u32 texture, rsx_be_format fmt);
    /* Optional: viewport depth range [min_z, max_z] = CLIP_MIN/CLIP_MAX
     * (0x0394/0x0398), RPCS3 VKGSRender::set_viewport. The engine folds the
     * matching normalisation into the VS z transform. NULL: [0, 1]. */
    void (*set_depth_range)(void* user, float min_z, float max_z);

    /* Optional. RPCS3's async recompiler: pipeline_async returns 1 when
     * pipeline_create only queues the compile on worker threads (draws using
     * a pipeline that is not built yet are skipped by the backend); the
     * engine then preloads its persisted program cache at startup. Calling it
     * may start the backend's compiler pool. pipeline_prewarm names the
     * topologies (bit n = rsx_topology n) a pipeline was drawn with, so their
     * variants can compile before the first draw asks. NULL: synchronous. */
    int  (*pipeline_async)(void* user);
    void (*pipeline_prewarm)(void* user, u32 pipeline, u32 topology_mask);

    /* Optional. RPCS3 VKFormats get_compatible_depth_surface_format: a depth
     * target in the host format for SURFACE_FORMAT depth `zfmt` (1 = Z16,
     * 2 = Z24S8) and CONTROL0 depth_float. depth_readback keeps returning
     * the stored depth as floats. NULL: depth_target_create. */
    u32 (*depth_target_create_ex)(void* user, u32 w, u32 h, u32 zfmt, u32 depth_float);
    /* Optional. RPCS3 "MSAA: Auto": the target (colour or depth handle) is
     * an antialiased surface widened by aa_sx x aa_sy; the backend may draw
     * it through a real multisampled twin of the pixel size, resolving to
     * the widened image whenever it is read. Returns RSX_BE_AA_* flags; 0
     * keeps the engine's widened single-sample emulation. Handle 0 asks for
     * the backend-wide capability (alpha-to-coverage in hardware on every
     * pipeline, RPCS3 hw_a2c_1spp). */
    u32 (*target_set_aa)(void* user, u32 handle, u32 aa_sx, u32 aa_sy);
    /* Optional. Run a temporal upscaler over the scene (rsx_be_scene_aa),
     * writing the result back into req->color. 1 = done, 0 = not (the
     * caller turns jitter off). NULL: no temporal upscaling. */
    int (*scene_aa)(void* user, const rsx_be_scene_aa* req);
    /* Optional. A clear limited to a rectangle, some colour channels or some
     * stencil bits (RPCS3 VKGSRender::clear_surface: the scissored region,
     * attachment_clear_pass, stencil_clear_pass). rts/n_rts is the bound
     * colour set (all one size and format), depth the zeta (0: none).
     * color_mask: bit0 R, bit1 G, bit2 B, bit3 A, applied to every target;
     * ds_flags: RSX_BE_CLEAR_*; stencil_mask: the stencil bits written.
     * x, y, w, h: the rectangle in target pixels. NULL: the engine clears
     * whole targets through clear_color / clear_depth_stencil. */
    void (*clear_rect)(void* user, const u32* rts, u32 n_rts, u32 depth,
                       const float rgba[4], u32 color_mask, u32 ds_flags,
                       float depth_value, u32 stencil, u32 stencil_mask,
                       u32 x, u32 y, u32 w, u32 h);
    /* Optional. 1 when the backend's fragment stage has the point sprite
     * coordinate (rsx_fp_frag_state.point_sprite_mask, a glslang target that
     * rewrites RSX_SPIRV_POINTCOORD_LOCATION). NULL or 0: point draws keep
     * the interpolated texture coordinates. */
    int (*point_sprite_supported)(void* user);
    /* Optional. RPCS3 render_target::memory_barrier for a depth surface's
     * old_contents: copy rectangle (sx, sy, sw, sh) of depth target `src`
     * onto (dx, dy, dw, dh) of depth target `dst` -- depth and stencil, point
     * sampled (scale_image with linear = false), ordered after all preceding
     * writes and before the next draw. Returns 1 when copied, 0 when the
     * backend cannot (e.g. different host depth formats); the engine then
     * leaves dst at its clear_memory values. NULL: never copied. */
    int (*depth_target_copy)(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                             u32 dst, u32 dx, u32 dy, u32 dw, u32 dh);
    /* Optional. RPCS3 vk::copy_image_typeless (memory_barrier when the
     * source has the other aspect or a format that is not bitcast
     * compatible, VKFormats formats_are_bitcast_compatible): the guest bytes
     * of rectangle (sx, sy, sw, sh) of `src` -- as the guest would hold them
     * in kind `src_kind` -- reinterpreted as kind `dst_kind` onto (dx, dy,
     * dw, dh) of `dst`, depth AND stencil for a Z24S8 side. Each handle is a
     * colour or depth target as its kind (RSX_BE_TYPELESS_*) says. Rows are
     * 1:1 (sh == dh) and the byte widths agree (sw * src_bpp == dw *
     * dst_bpp, RSX_BE_TYPELESS_BPP). Returns 1 when copied, 0 when not (the engine then leaves dst
     * as it was). NULL: never copied. */
    int (*surface_typeless_copy)(void* user, u32 src, u32 src_kind,
                                 u32 sx, u32 sy, u32 sw, u32 sh,
                                 u32 dst, u32 dst_kind,
                                 u32 dx, u32 dy, u32 dw, u32 dh);
    /* Optional. color_target_blit_filtered with the source rectangle read
     * mirrored: `flip` bit 0 reverses X, bit 1 Y (RPCS3 blit_engine flip
     * for a negative NV3089 scale). NULL: mirrored transfers stay on the
     * CPU. */
    void (*color_target_blit_flip)(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                                   u32 dst, u32 dx, u32 dy, u32 dw, u32 dh,
                                   int linear, u32 flip);
    /* Optional. RPCS3 get_compatible_surface_format: nonzero when colour
     * targets of `fmt` (RSX_BE_FMT_R5G6B5, _A1R5G5B5, _R8, _R8G8) can be
     * created, rendered to, blended, blitted and sampled. Such a target is
     * still read, written, seeded and cleared in the RGBA8 target layout
     * (B8 in .b, G8B8 in .g/.b, R5G6B5 / X1R5G5B5 as R,G,B with A 1 / the
     * stored bit), and its sampled views present those lanes; the pipeline's
     * rt_fmt is the host format and the shader writes .r (.rg) for B8 (G8B8).
     * NULL: every colour target is RGBA8. */
    int (*color_format_supported)(void* user, rsx_be_format fmt);
    /* Optional (perf): depth_readback and stencil_readback of one rectangle
     * in a single submission, rows tight (w pixels). When the host stores
     * depth as UNORM16 / UNORM24 and `out_raw` is not NULL, *raw_bits is set
     * to 16 / 24 and out_raw gets each pixel's raw integer (low bits), the
     * value depth_readback would have divided by 65535 / 16777215; otherwise
     * *raw_bits is 0 and out_f gets what depth_readback returns. out_stencil
     * gets what stencil_readback returns, *have_stencil 1, when the target
     * has stencil, else *have_stencil 0. Returns 0 on success. NULL: the
     * two separate reads. */
    int (*zeta_readback)(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h,
                         float* out_f, u32* out_raw, u32* raw_bits,
                         u8* out_stencil, int* have_stencil);
} rsx_draw_backend;

/* surface_typeless_copy kinds: the guest layout of a target's bytes. */
#define RSX_BE_TYPELESS_ARGB8     0u  /* colour, A8R8G8B8 / X8R8G8B8 words (4) */
#define RSX_BE_TYPELESS_X32       1u  /* colour, F_X32 float words         (4) */
#define RSX_BE_TYPELESS_Z24S8     2u  /* depth, d24 unorm << 8 | stencil   (4) */
#define RSX_BE_TYPELESS_Z24S8_F   3u  /* depth, d24 float (f32 >> 7) << 8 | s  */
#define RSX_BE_TYPELESS_Z16       4u  /* depth, d16 unorm                  (2) */
#define RSX_BE_TYPELESS_Z16_F     5u  /* depth, d16 float (IEEE half)      (2) */
/* The other colour layouts, on RGBA8 targets holding the shader's channels
 * unless noted (the guest unit read big-endian): */
#define RSX_BE_TYPELESS_ABGR8     6u  /* A<<24 | B<<16 | G<<8 | R          (4) */
#define RSX_BE_TYPELESS_B8        7u  /* the B channel                     (1) */
#define RSX_BE_TYPELESS_G8B8      8u  /* G<<8 | B                          (2) */
#define RSX_BE_TYPELESS_RGB565    9u  /* R5<<11 | G6<<5 | B5               (2) */
#define RSX_BE_TYPELESS_ARGB1555 10u  /* A1<<15 | R5<<10 | G5<<5 | B5      (2) */
#define RSX_BE_TYPELESS_FP16X4   11u  /* RGBA16F target: halves R,G,B,A    (8) */
#define RSX_BE_TYPELESS_FP32X4   12u  /* RGBA32F target: floats R,G,B,A   (16) */
#define RSX_BE_TYPELESS_KINDS    13u
/* Guest bytes per texel of a kind, and whether it is a depth kind. */
#define RSX_BE_TYPELESS_BPP(k) \
    ((k) == RSX_BE_TYPELESS_B8 ? 1u : \
     ((k) == RSX_BE_TYPELESS_Z16 || (k) == RSX_BE_TYPELESS_Z16_F || \
      (k) == RSX_BE_TYPELESS_G8B8 || (k) == RSX_BE_TYPELESS_RGB565 || \
      (k) == RSX_BE_TYPELESS_ARGB1555) ? 2u : \
     (k) == RSX_BE_TYPELESS_FP16X4 ? 8u : (k) == RSX_BE_TYPELESS_FP32X4 ? 16u : 4u)
#define RSX_BE_TYPELESS_IS_DEPTH(k) ((k) >= RSX_BE_TYPELESS_Z24S8 && (k) <= RSX_BE_TYPELESS_Z16_F)

#define RSX_BE_AA_HW_MSAA   1u  /* draws rasterise multisampled          */
#define RSX_BE_AA_HW_A2ONE  2u  /* alpha-to-one is done by the hardware   */

/* surface_reinterpret source kinds: what the guest bytes of `src` are. */
#define RSX_BE_REINTERPRET_COLOR_ARGB8  0u  /* colour target, A8R8G8B8 words  */
#define RSX_BE_REINTERPRET_DEPTH_Z24S8  1u  /* depth target, d24 << 8 | 0     */
#define RSX_BE_REINTERPRET_DEPTH_Z16    2u  /* depth target, big-endian d16   */

/* ---- the engine --------------------------------------------------------- */

/* Register the backend the engine drives. Passing NULL unregisters it. */
void rsx_draw_engine_set_backend(const rsx_draw_backend* backend);
/* The host GPU vendor is NVIDIA (RPCS3 enables its low-precision compare and
 * rounding fixups only there). Default 1; the Vulkan backend sets it. */
void rsx_draw_engine_set_host_nvidia(int is_nvidia);

/* The backend rasterises with the last vertex of a primitive as the provoking
 * vertex (RSX/GL convention; VK_EXT_provoking_vertex). Only then does SHADE_MODE
 * FLAT flatten the colour interpolants, as RPCS3 supports_last_provoking_vertex
 * gates it (RSXGraphEngine.cpp:1108,1154); otherwise they stay smooth. */
void rsx_draw_engine_set_last_provoking_vertex(int on);

/* RPCS3 custom_border_color_features::require_border_color_remap: the device
 * samples the border colour without the view's swizzle, so the engine
 * applies the crossbar to it (remapped_border_color). Default 1. */
void rsx_draw_engine_set_border_color_remap(int on);

/* RPCS3 decode_blend_state: COLOR_MASK (B [0:7], G [8:15], R [16:23],
 * A [24:31]) and COLOR_MASK_MRT (one A,R,G,B nibble per target from bit 4i)
 * trimmed to the channels the SET_SURFACE_FORMAT colour format has
 * (get_write_output_mask), after the B8/G8B8 remap. The engine keeps B8 in
 * .b and G8B8 in .g/.b of its RGBA8 target (eng_surface_crossbar), so the
 * remap leaves those lanes. RSX_COLOR_MASK_LEGACY=1 keeps the raw masks. */
void rsx_draw_engine_trim_color_mask(u32 color_format, u32* color_mask, u32* color_mask_mrt);

/* Is the engine the path this run takes? True when PS3RECOMP_RSX_ENGINE names
 * "dispatch" (or the backend asked for it by default) and a backend has been
 * registered. Read once and cached, so it is cheap in the FIFO walker. */
int  rsx_draw_engine_enabled(void);

/* Called by a backend that wants the engine to be the default path with no
 * environment set. PS3RECOMP_RSX_ENGINE=vtable still turns it off. */
void rsx_draw_engine_set_default(int on);

/* A runner with its own RSX IO map supplies its address resolver before init.
 * NULL restores the toolkit's cellGcm mapping. */
void rsx_draw_engine_set_guest_memory(rsx_vertex_guest_ptr_fn reader, void* user);

int  rsx_draw_engine_init(u32 width, u32 height);
void rsx_draw_engine_shutdown(void);

/* One FIFO method, with its subchannel bits still on: the engine masks them
 * out itself, as rsx_live_draw_method (6902-6976) does, because a title whose
 * SPU-built command lists bind NV4097 to another subchannel would otherwise
 * write its state into the wrong register bank. */
void rsx_draw_engine_method(u32 method, u32 arg);

/* cellGcmSetDisplayBuffer, so the flip can resolve a buffer id to a surface
 * the engine has actually rendered rather than to whatever is bound. */
void rsx_draw_engine_set_display_buffer(u32 buffer_id, u32 location, u32 offset,
                                        u32 pitch, u32 width, u32 height);

/* The guest's SET_REFERENCE sync point: submit and wait, so a title spinning
 * on its fence sees the GPU catch up. */
void rsx_draw_engine_flush(void);

/* Host occlusion queries (backend occlusion_*). _begin returns 0 when the
 * backend has none; _result returns 1 when *out holds the result. _kick(id)
 * submits the recorded work holding query `id` if it is not submitted yet,
 * without waiting (RPCS3 on_sync_hint zcull_sync). */
int  rsx_draw_engine_occlusion_begin(u32 id);
void rsx_draw_engine_occlusion_end(u32 id);
int  rsx_draw_engine_occlusion_result(u32 id, int wait, u64* out);
void rsx_draw_engine_occlusion_discard(u32 id);
void rsx_draw_engine_occlusion_kick(u32 id);

/* Present whatever the frame has recorded, for a host that drives the flip
 * itself rather than through the FIFO's 0xE944. */
void rsx_draw_engine_present(void);
/* A runner retiring a queued flip names the buffer explicitly. */
void rsx_draw_engine_present_buffer(u32 buffer_id);
/* A flip the engine never sees as 0xE944 (a 0xFEAD driver word or the SetFlip
 * HLE, both handled by cellGcmSys): remember the buffer for the host's next
 * rsx_draw_engine_present() without presenting now. */
void rsx_draw_engine_note_flip(u32 buffer_id);
/* cellGcmSetTile / BindTile / UnbindTile / SetInvalidateTile: tile region
 * `index` (location 0 local, 1 main) and its compression mode. A bound colour
 * tile compressed 2x1 or 2x2 (comp 7, 8) holds each pixel as its AA sample
 * group, which the surface store models on AA <-> non-AA rebinds and reads
 * (RSX_ENG_TILE_AA_COMPRESS). A bound tile in main memory also permutes the
 * bytes RSX images keep there by pitch and `bank` (RPCS3 "Handle RSX Memory
 * Tiling"; RSX_ENG_TILED_MEMORY=1). size 0 or !bound removes it. Safe to call
 * whether or not the engine is up. */
void rsx_draw_engine_set_tile(u32 index, u32 location, u32 offset, u32 size,
                              u32 pitch, u32 comp, u32 bank, int bound);

/* Guest memory and the surfaces the engine renders on the host GPU, kept
 * coherent around a 2D transfer (NV3089) that the FIFO walker performs on
 * guest memory. A title copies its rendered scene out to main memory for
 * post-processing, back into VRAM, and into its display buffer with such
 * transfers; on the engine those pixels exist only on the GPU.
 *
 * Before the transfer reads guest bytes, guest_read writes the pixels of any
 * engine surface in that rectangle back to guest memory. After it writes,
 * guest_wrote pushes the guest bytes into any engine surface the rectangle
 * falls in. `offset` is the rectangle's first byte, `pitch` the guest row
 * pitch, `w` and `h` in 32-bit pixels. Only 8-bit-per-channel surfaces laid
 * out with the same pitch take part; anything else is left alone. */
void rsx_draw_engine_guest_read(u32 location, u32 offset, u32 pitch, u32 w, u32 h);
void rsx_draw_engine_guest_wrote(u32 location, u32 offset, u32 pitch, u32 w, u32 h);
/* Address-range forms for linear transfers (NV0039): store every rendered
 * colour surface overlapping the source range to guest memory before it is
 * read (depth too only with RSX_CFG_LEGACY=1: RPCS3 Write Depth Buffer off).
 * range_read_color never stores depth. A transfer about to write a range
 * calls range_will_write (RPCS3's write fault: overlapping rendered colour
 * surfaces are stored first) and range_wrote after it (RPCS3 write_barrier:
 * every overlapping colour/depth surface is erased to its initial clear;
 * RSX_ENG_DMA_RELOAD=1 reloads A8R8G8B8 rows instead). range_cpu_wrote is the
 * after-call for a CPU-side write with no write_barrier (NV3089's software
 * path): surfaces keep their pixels but stop owning the range. */
void rsx_draw_engine_range_read(u32 location, u32 offset, u32 length);
void rsx_draw_engine_range_read_color(u32 location, u32 offset, u32 length);
void rsx_draw_engine_range_will_write(u32 location, u32 offset, u32 length);
void rsx_draw_engine_range_wrote(u32 location, u32 offset, u32 length);
void rsx_draw_engine_range_cpu_wrote(u32 location, u32 offset, u32 length);
/* Only the texture half of range_wrote: cached textures (and swizzled-copy
 * records) overlapping [offset, +length) are re-hashed on their next use.
 * For a transfer whose surface rows are reloaded separately by guest_wrote. */
void rsx_draw_engine_textures_wrote(u32 location, u32 offset, u32 length);
/* A guest write the engine did not issue (SPU DMA puts, from cellGcmSys'
 * SPU write hook), by effective address: cached textures on the pages it
 * touched are re-hashed in full at their next use, as RPCS3's page
 * protection invalidates them at once. Callable from any thread. */
void rsx_draw_engine_note_guest_write(u32 ea, u32 size);

/* RPCS3's blit engine (texture_cache::upload_scaled_image) for an NV3089
 * scaled-image transfer of 32-bit pixels: when the source rectangle lies in
 * an engine colour surface and the destination in another (a destination
 * surface is made in LOCAL memory, as RPCS3 makes a blit destination
 * section, only with RSX_ENG_BLIT_CREATE_TARGETS=1: without CPU write
 * tracking such a target could shadow later CPU uploads), the copy is done
 * on the GPU and nothing goes through guest memory. Source start (src_x,
 * src_y) and destination (dst_x, dst_y,
 * dst_w, dst_h) are in pixels from the two offsets, clipped already; the
 * source extent is dst_w * ds_dx >> 20 by dst_h * dt_dy >> 20. Returns 1 when
 * done (the caller skips its guest-memory copy), 0 to fall back to it.
 * in_wh (in_h << 16 | in_w, 0 = none) is the NV3089 image-in size: RPCS3
 * apply_pixel_transforms crops the source extent to it and the cropped
 * source still fills the whole destination rectangle.
 * RSX_ENG_LEGACY_SURFACES=1 always returns 0. */
int rsx_draw_engine_blit(u32 src_location, u32 src_offset, u32 src_pitch,
                         u32 src_x, u32 src_y, u32 ds_dx, u32 dt_dy,
                         u32 dst_location, u32 dst_offset, u32 dst_pitch,
                         u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h, int linear, u32 in_wh);
/* The same at `bpp` 4 (A8R8G8B8) or 2 (R5G6B5 between R5G6B5 targets). */
int rsx_draw_engine_blit_bpp(u32 src_location, u32 src_offset, u32 src_pitch,
                             u32 src_x, u32 src_y, u32 ds_dx, u32 dt_dy,
                             u32 dst_location, u32 dst_offset, u32 dst_pitch,
                             u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h, int linear, u32 in_wh,
                             u32 bpp);
/* The same, mirrored: flip bit 0 X, bit 1 Y (NV3089 negative DS_DX /
 * DT_DY). On a mirrored axis src_x / src_y is the source texel the first
 * destination pixel reads, the source running backwards from it (cellGcmSys
 * nv3089_src_coord). RSX_ENG_BLIT_MIRROR=0 or no color_target_blit_flip:
 * a mirrored call returns 0. */
int rsx_draw_engine_blit_flip(u32 src_location, u32 src_offset, u32 src_pitch,
                              u32 src_x, u32 src_y, u32 ds_dx, u32 dt_dy,
                              u32 dst_location, u32 dst_offset, u32 dst_pitch,
                              u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h, int linear, u32 in_wh,
                              u32 bpp, u32 flip);
/* NV3089 into an NV309E swizzled destination (pow2 dst_w x dst_h at 0,0,
 * 4 bytes per texel), kept on the GPU until guest memory is read. Returns 1
 * if done, 0 when the caller must copy through guest memory. */
int rsx_draw_engine_blit_swizzled(u32 src_location, u32 src_offset, u32 src_pitch,
                                  u32 src_x, u32 src_y, u32 ds_dx, u32 dt_dy,
                                  u32 dst_location, u32 dst_offset,
                                  u32 dst_w, u32 dst_h, int linear);

/* --- test hooks ---------------------------------------------------------- */

/* Draws that ran the guest's OWN programs in the last presented frame. A draw
 * through the built-in pair is not one of them. */
u32  rsx_draw_engine_guest_draws(void);

/* The centre pixel of the surface the last flip presented, as A8R8G8B8 --
 * the convention the RSX clear colour arrives in, so a host can compare the
 * two. Zero when nothing has been presented or the surface is not an
 * 8-bit-per-channel one. */
u32  rsx_draw_engine_readback_center(void);

/* ---- pieces the engine exposes for its own tests ------------------------ */
/*
 * These are the neutral parts the unit tests in libs/video/tests exercise
 * without a GPU: the surface set's keying, the texture cache's revalidation
 * and eviction, the pipeline key's stability, and restart-cut expansion.
 */

/* Decode the NV4097 render state out of the register file. */
void rsx_draw_engine_decode_render_state(const rsx_dispatch* rsx,
                                         rsx_be_render_state* out);

/* Fold the structural render state into a pipeline key. The alpha REFERENCE
 * and everything else that lives in a constant buffer are deliberately left
 * out, so a title changing constants per draw does not multiply pipelines --
 * rsx_live_draw.c's ld_hash_structural_render_state (3148-3187). */
u64  rsx_draw_engine_hash_render_state(const rsx_be_render_state* rs, u64 hash);

/* How many indices the topology expansion produces for `source_refs`
 * references cut at `cuts`, and the indices themselves. `indices` must have
 * room for the count. Triangle strips alternate winding per segment; a quad
 * strip pairs its vertices from each segment's own start, and a polygon is a
 * fan around each segment's own first vertex. */
u32  rsx_draw_engine_topology_index_count(u32 primitive, u32 source_refs,
                                          const u32* cuts, u32 cut_count);
void rsx_draw_engine_write_topology_indices(u32 primitive, u32 source_refs,
                                            const u32* cuts, u32 cut_count,
                                            const u32* occurrence_to_unique,
                                            u32* indices);

/* Guest page guards shared by every owner that protects guest pages to catch
 * the guest's first touch (zcull reports = 1, WCB surfaces = 2): a page is
 * inaccessible while any owner guards it and nobody holds it open. `page` is
 * a guest address. set returns 0 when the guard could not be applied. */
int  rsx_page_guard_set(u32 page, u32 owner, int on);
void rsx_page_guard_hold(u32 page, int delta);
/* A protection fault on guest address `ea` that no owner claimed: re-applies
 * the page's protection from its owner mask (dropping owners that would leave
 * the access faulting) and invalidates protection-keyed caches. Returns 1 when
 * the access can be retried. The engine installs it as the host's
 * g_vm_guard_reconcile_hook (RSX_ENG_PG_RECONCILE=0: not). */
int  rsx_page_guard_reconcile(u32 ea, int is_write);
/* Write Color Buffers: stores the surfaces guest threads faulted on. Call on
 * the thread that runs the engine (no-op elsewhere). */
void rsx_draw_engine_wcb_service(void);

/* ---- temporal antialiasing / upscaling hooks ----------------------------
 * A title layer decides which target is the scene, when the scene is
 * complete (before the UI), and the camera motion; the engine reports each
 * draw, applies the sub-pixel jitter it is given, and runs the backend's
 * scene_aa on request. All optional: without hooks nothing changes. */
typedef struct rsx_aa_draw_info {
    const rsx_dispatch* rsx;     /* the register file (vertex program,
                                  * constants, vertex arrays, textures)     */
    u32 color, depth;            /* backend handles, 0 = none               */
    u32 color_location, color_offset;
    u32 depth_location, depth_offset;   /* depth_offset ~0u: no zeta        */
    u32 w, h;                    /* target size (samples)                   */
    u32 aa_sx, aa_sy;
    u32 depth_test, depth_write, depth_func;  /* depth_func: RSX enum       */
    u32 tex_used;                /* fragment texture units the draw samples */
    u32 tex_location[RSX_BE_MAX_TEXTURES], tex_offset[RSX_BE_MAX_TEXTURES];
    float zA, zB;                /* stored depth -> NDC z                   */
} rsx_aa_draw_info;

typedef struct rsx_aa_hooks {
    void* user;
    /* Before the draw binds anything; may call rsx_draw_engine_scene_aa. */
    void (*on_draw)(void* user, const rsx_aa_draw_info* info);
    /* The draw's clip -> NDC epilogue (vp_posscale.xyz, _, vp_posoffset.xyz,
     * _; D3D NDC, y up). Return 1 with jitter_px (target pixels, +x right,
     * +y down) to offset the draw. */
    int  (*on_transform)(void* user, const float xf[8], float jitter_px[2]);
    /* Flip requested, before the frame is presented; may call scene_aa. */
    void (*on_flip)(void* user);
} rsx_aa_hooks;

void rsx_draw_engine_set_aa_hooks(const rsx_aa_hooks* hooks);
/* Forward to the backend's scene_aa; 0 when it has none or it failed. */
int  rsx_draw_engine_scene_aa(const rsx_be_scene_aa* req);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_DRAW_ENGINE_H */
