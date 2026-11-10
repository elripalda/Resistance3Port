/*
 * ps3recomp - NV4097 method dispatcher (Track B / LAYER 2)
 *
 * A clean-room register-file model of the RSX 3D engine's method interface:
 * dispatch(method, arg) stores every write into a 16 K-word register file
 * (mirroring the hardware, so a captured initial register state can seed it
 * with no side effects) and triggers execution callbacks for the handful of
 * methods that DO something (clear, begin/end, draw batches, program and
 * constant uploads, flip).
 *
 * Consumers read decoded state through the rsx_dsp_* getters; the dispatcher
 * itself touches no GPU API and no guest memory, so the same code can drive
 * the offline capture-replay harness and, later, the live FIFO consumer.
 *
 * Method numbers and bitfield layouts come from MIT sources only: the
 * envytools rnndb NV30-40 3D register database (tools/nv40_methods.py is
 * generated from it), Mesa's nv30 driver headers, and the psdevwiki RSX
 * notes (docs/PSDEVWIKI_REFS.md). RPCS3 was consulted as a read-only
 * semantics oracle (e.g. transform-program load auto-advance); no code
 * was copied.
 */

#ifndef PS3RECOMP_RSX_DISPATCH_H
#define PS3RECOMP_RSX_DISPATCH_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_DSP_NUM_REGS        (0x10000 / 4)
#define RSX_DSP_VP_INSTR        544              /* transform program slots  */
#define RSX_DSP_VP_WORDS        (RSX_DSP_VP_INSTR * 4)
#define RSX_DSP_NUM_CONSTANTS   512              /* vec4 transform constants */
#define RSX_DSP_NUM_VERTEX_ATTR 16
#define RSX_DSP_NUM_TEXTURES    16               /* fragment texture units   */

/* rsx_dsp_texture.format values (gcm public format byte, incl. the LN/UN
 * modifier bits: 0x20 = linear/no-swizzle, 0x40 = unnormalized coords) */
#define RSX_TEX_FMT_B8              0x81
#define RSX_TEX_FMT_A1R5G5B5        0x82
#define RSX_TEX_FMT_A4R4G4B4        0x83
#define RSX_TEX_FMT_R5G6B5          0x84
#define RSX_TEX_FMT_A8R8G8B8        0x85
#define RSX_TEX_FMT_DXT1            0x86
#define RSX_TEX_FMT_DXT23           0x87
#define RSX_TEX_FMT_DXT45           0x88
#define RSX_TEX_FMT_G8B8            0x8B
#define RSX_TEX_FMT_DEPTH24_D8      0x90
#define RSX_TEX_FMT_LINEAR          0x20         /* modifier: no swizzle     */
#define RSX_TEX_FMT_UNNORM          0x40         /* modifier: texel coords   */
#define RSX_TEX_FMT_BASE_MASK       0x9F         /* format sans modifiers    */

/* rsx_dsp_vertex_attr.type values (NV4097 vertex array format field) */
#define RSX_VTX_TYPE_SNORM16    1
#define RSX_VTX_TYPE_FLOAT      2
#define RSX_VTX_TYPE_HALF       3
#define RSX_VTX_TYPE_UNORM8     4
#define RSX_VTX_TYPE_SINT16     5
#define RSX_VTX_TYPE_CMP32      6
#define RSX_VTX_TYPE_UINT8      7

/* CLEAR_BUFFERS argument bits (nouveau NV30_3D_CLEAR_BUFFERS layout) */
#define RSX_CLEAR_DEPTH         0x01
#define RSX_CLEAR_STENCIL       0x02
#define RSX_CLEAR_COLOR_R       0x10
#define RSX_CLEAR_COLOR_G       0x20
#define RSX_CLEAR_COLOR_B       0x40
#define RSX_CLEAR_COLOR_A       0x80

/* Memory locations (bit 31 of offsets / DMA context selectors) */
#define RSX_LOCATION_LOCAL      0
#define RSX_LOCATION_MAIN       1

/* Coverage classification for the report */
#define RSX_DSP_CLASS_TODO      0   /* stored in the register file only     */
#define RSX_DSP_CLASS_STATE     1   /* decoded by a getter the pipeline uses */
#define RSX_DSP_CLASS_EXEC      2   /* triggers an execution callback        */

/* In-struct capacity of one INLINE_ARRAY stream. A stream is bounded by a
 * single BEGIN_END pair, and titles use inline arrays for small
 * immediate-mode batches (UI quads, debug geometry); 64 KB is several
 * hundred vertices of a fat format. A longer stream moves to a grown heap
 * buffer (RPCS3 keeps it in an unbounded simple_array, draw_call.hpp
 * inline_vertex_array) up to RSX_DSP_GROW_MAX_BYTES; RSX_DSP_GROW=0 restores
 * the old fixed cap. Overruns past the limit are counted, not silently
 * truncated -- half a vertex stream is worse than no draw. */
#define RSX_DSP_INLINE_MAX_BYTES 65536
/* In-struct capacity of one ARRAY_ELEMENT16/32 index list (in indices); grows
 * like the inline stream (RPCS3 m_element_push_buffer, RSXDrawCommands.cpp). */
#define RSX_DSP_ELEMENT_MAX 16384
#define RSX_DSP_GROW_MAX_BYTES (64u << 20)
/* Pseudo-method (subchannel 7 method 0x1EB0, never a guest method: the FIFO
 * walker drops it) that abandons the open draw without drawing it: RPCS3
 * thread::recover_fifo -> discard_draw (RSXThread.cpp:1764-1771). */
#define RSX_DSP_DISCARD_DRAW 0xFEB0u
#define RSX_DSP_CLAUSE_MAX_RANGES   1024
#define RSX_DSP_CLAUSE_MAX_BARRIERS 8192
/* RPCS3 nv4097.h max_transform_constants: writes past it are NOPs. */
#define RSX_DSP_MAX_TRANSFORM_CONSTANTS 468
#define RSX_DSP_ELEMENT_DROPPED 0xFFFFFFFFu   /* elem_count after an overrun */

typedef struct rsx_dispatch rsx_dispatch;

/* Execution sink: what the dispatcher calls when a method DOES something.
 * All state (clear color, primitive, attrib layout, ...) is read back from
 * the dispatcher via the getters below. Any callback may be NULL. */
typedef struct rsx_dispatch_sink {
    void* user;
    void (*clear)(void* user, const rsx_dispatch* rsx, u32 mask);
    void (*begin)(void* user, const rsx_dispatch* rsx, u32 primitive);
    void (*end)(void* user, const rsx_dispatch* rsx);
    /* DRAW_ARRAYS batch: consecutive vertices [first, first+count) */
    void (*draw_arrays)(void* user, const rsx_dispatch* rsx, u32 first, u32 count);
    /* DRAW_INDEX_ARRAY batch: consecutive indices [first, first+count) */
    void (*draw_index_array)(void* user, const rsx_dispatch* rsx, u32 first, u32 count);
    /* INLINE_ARRAY batch: the vertices were pushed through the FIFO itself
     * rather than fetched from memory, so they arrive here instead of as a
     * (first, count) range. `data` is the raw stream in guest byte order and
     * `bytes` its length; the layout is the ordinary VTXFMT declaration --
     * see the note on M_INLINE_ARRAY in rsx_dispatch.c. Delivered at END. */
    void (*inline_array)(void* user, const rsx_dispatch* rsx,
                         const u8* data, u32 bytes);
    void (*flip)(void* user, const rsx_dispatch* rsx, u32 arg);
    /* A completed ATTR0 DATA3F/DATA4F inside BEGIN emits one vertex.
     * Snapshot the current attributes now; later register writes replace them.
     * Array formats, offsets and frequencies do not describe this stream. */
    void (*immediate_vertex)(void* user, const rsx_dispatch* rsx);
    /* ARRAY_ELEMENT16/32 (0x180C/0x1810) inside BEGIN: indices pushed through
     * the FIFO. RPCS3 draws them as a u32 indexed draw over the vertex arrays
     * (draw_command_processor::append_array_element). Delivered at END. */
    void (*array_elements)(void* user, const rsx_dispatch* rsx,
                           const u32* indices, u32 count);
    /* A NV4097 register (< 0x2000) took a value other than the one it held,
     * already stored: RPCS3's method handlers compare against the latch. */
    void (*reg_changed)(void* user, const rsx_dispatch* rsx, u32 method,
                        u32 old_value, u32 new_value);
} rsx_dispatch_sink;

struct rsx_dispatch {
    u32 regs[RSX_DSP_NUM_REGS];       /* register file: regs[method >> 2]   */
    u32 vp[RSX_DSP_VP_WORDS];         /* transform (vertex) program words   */
    u32 constants[RSX_DSP_NUM_CONSTANTS][4]; /* vec4 f32 bit patterns       */

    rsx_dispatch_sink sink;

    int in_begin_end;                 /* between BEGIN_END(prim) and (0)    */
    u32 current_primitive;

    u32 inline_len;                   /* bytes accumulated this primitive   */
    u32 inline_dropped;               /* streams lost to the cap            */

    /* coverage accounting */
    u32 seen[RSX_DSP_NUM_REGS];       /* per-register write count           */
    u8  klass[RSX_DSP_NUM_REGS];      /* RSX_DSP_CLASS_*                    */

    /* INLINE_ARRAY accumulation, reset at every BEGIN_END(prim). The first
     * RSX_DSP_INLINE_MAX_BYTES live here; a longer stream is moved to a heap
     * buffer owned by rsx_dispatch.c (not a pointer in this struct, because
     * rsx_dispatch_init memsets the whole struct). Consumers get the bytes
     * through the inline_array sink, never from this array directly.
     *
     * LAST on purpose. Sitting it next to current_primitive pushed seen[]
     * and klass[] 64 KB away from the register file, and those three are
     * touched together on EVERY method write: The Simpsons Game, which
     * emits no inline arrays at all, still lost an order of magnitude of
     * poll rate in the regression gate purely from the added stride. */
    u8  inline_data[RSX_DSP_INLINE_MAX_BYTES];

    /* ARRAY_ELEMENT16/32 accumulation, reset at every BEGIN_END(prim). */
    u32 elem_count;
    u32 elem_dropped;                 /* lists lost to the cap              */
    u32 elem_data[RSX_DSP_ELEMENT_MAX];

    /* The draw clause, RPCS3 rsx::draw_clause: DRAW_ARRAYS/DRAW_INDEX_ARRAY
     * ranges (merged when contiguous, de-duplicated and sorted by first
     * since the last execution barrier) plus the deferred parameter writes
     * (vertex/index base, array offsets, transform constant load/data) that
     * arrived after the first range. Executed at END, one sub-draw per
     * range, each barrier applied just before the range it precedes. */
    u32 clause_command;               /* 0 none, 1 array, 2 indexed         */
    u32 clause_n_ranges;
    u32 clause_cur;                   /* current_range_index                */
    u32 clause_last_exec;             /* last_execution_barrier_index       */
    u32 clause_n_barriers;
    u32 clause_overflows;
    /* The clause executed by the last END. RPCS3 keeps it until the next
     * valid BEGIN (draw_clause::reset), so an invalid BEGIN re-runs it. */
    u32 clause_prev_command;
    u32 clause_prev_n_ranges;
    u32 clause_prev_n_barriers;
    struct { u32 first, count; } clause_ranges[RSX_DSP_CLAUSE_MAX_RANGES];
    struct { u32 draw_id, method, arg; } clause_barriers[RSX_DSP_CLAUSE_MAX_BARRIERS];
};

/* ---- lifecycle -------------------------------------------------------- */

void rsx_dispatch_init(rsx_dispatch* rsx, const rsx_dispatch_sink* sink);
/* RPCS3 rsx_state::reset image (NV4097 registers), applied at init and by
 * GCM_FLIP_COMMAND 0xFEAC (RSX_FLIP_RESET=0 disables the latter). */
void rsx_dispatch_reset_state(rsx_dispatch* rsx);

/* Seed the register file / transform program from a captured initial state.
 * No side effects, no callbacks — mirrors loading hardware context. */
void rsx_dispatch_seed_registers(rsx_dispatch* rsx, const u32* regs, u32 count);
void rsx_dispatch_seed_transform_program(rsx_dispatch* rsx, const u32* words, u32 count);
void rsx_dispatch_seed_transform_constants(rsx_dispatch* rsx, const u32* words, u32 count);

/* Dispatch one register write. */
void rsx_dispatch_method(rsx_dispatch* rsx, u32 method, u32 arg);

/* ---- decoded state getters -------------------------------------------- */

static inline u32 rsx_dsp_reg(const rsx_dispatch* rsx, u32 method)
{
    return rsx->regs[(method & 0xFFFFC) >> 2];
}

/* CLEAR_COLOR_VALUE: A8R8G8B8 */
u32  rsx_dsp_clear_color(const rsx_dispatch* rsx);
/* CLEAR_DEPTH_VALUE: depth [8:31], stencil [0:7] */
u32  rsx_dsp_clear_zstencil(const rsx_dispatch* rsx);

/* Surface (render target) state */
typedef struct rsx_dsp_surface {
    u32 color_format;    /* SURFACE_FORMAT color field (5 = A8R8G8B8)      */
/* SET_SURFACE_FORMAT colour field: FP16 RGBA, the HDR target format. */
#define RSX_SURFACE_FMT_F_W16Z16Y16X16 0xB
    u32 depth_format;
    u32 raster_type;     /* 1 = pitch (linear), 2 = swizzle                */
    u32 clip_x, clip_y, clip_w, clip_h;
    u32 color_offset[4];
    u32 color_pitch[4];
    u32 color_location[4];  /* RSX_LOCATION_* from the DMA context handle  */
    u32 color_target;       /* RT_ENABLE selector                          */
    u32 zeta_offset;
    u32 zeta_pitch;
    u32 zeta_location;
    /* SURFACE_FORMAT antialias field (bits 12-15) and the sample grid it
     * implies. An antialiased surface is held at its sample resolution, as
     * in guest memory: DIAGONAL_CENTERED_2 (3) is 2x1 samples per pixel,
     * SQUARE_CENTERED_4 (4) and SQUARE_ROTATED_4 (5) are 2x2. clip_x/y/w/h
     * above are already multiplied by it; the viewport and scissor registers
     * stay in pixels, so consumers scale them by aa_sx/aa_sy. */
    u32 antialias, aa_sx, aa_sy;
} rsx_dsp_surface;
void rsx_dsp_get_surface(const rsx_dispatch* rsx, rsx_dsp_surface* out);

/* Viewport: offset/size in window coords + scale/translate transform */
typedef struct rsx_dsp_viewport {
    u32 x, y, w, h;
    float scale[4];      /* VIEWPORT_SCALE x,y,z,w                          */
    float translate[4];  /* VIEWPORT_TRANSLATE x,y,z,w                      */
} rsx_dsp_viewport;
void rsx_dsp_get_viewport(const rsx_dispatch* rsx, rsx_dsp_viewport* out);

/* Vertex attribute stream layout (VTXFMT / VTXBUF_OFFSET registers) */
typedef struct rsx_dsp_vertex_attr {
    u32 type;            /* RSX_VTX_TYPE_*; 0 = disabled                    */
    u32 size;            /* component count 0..4                            */
    u32 stride;          /* bytes                                           */
    u32 frequency;
    u32 offset;          /* into location's address space                   */
    u32 location;        /* RSX_LOCATION_*                                  */
} rsx_dsp_vertex_attr;
void rsx_dsp_get_vertex_attr(const rsx_dispatch* rsx, u32 index, rsx_dsp_vertex_attr* out);

/* VERTEX_DATA_BASE_OFFSET / BASE_INDEX applied on top of attr offsets */
u32 rsx_dsp_vertex_data_base_offset(const rsx_dispatch* rsx);
u32 rsx_dsp_vertex_data_base_index(const rsx_dispatch* rsx);

/* RSX adds the element-base index to indexed draws in a 20-bit domain before
 * vertex fetch. Non-indexed array draws use their range.first directly.
 * The index-array element width controls only how the raw index is read; it
 * does not truncate the resolved address back to 16 bits. */
static inline u32 rsx_dsp_resolve_vertex_index(u32 index, u32 base_index)
{
    return (index + base_index) & 0x000FFFFFu;
}

/* Index array state (IDXBUF_OFFSET / IDXBUF_FORMAT) */
typedef struct rsx_dsp_index_array {
    u32 offset;
    u32 location;
    u32 is_u32;          /* 1 = 32-bit indices, 0 = 16-bit                  */
} rsx_dsp_index_array;
void rsx_dsp_get_index_array(const rsx_dispatch* rsx, rsx_dsp_index_array* out);

/* Fragment texture unit state (TEX_OFFSET/FORMAT/WRAP/ENABLE/SWIZZLE/FILTER/
 * NPOT_SIZE at 0x1A00 + unit*0x20, pitch from TEX_SIZE1 at 0x1840 + unit*4) */
typedef struct rsx_dsp_texture {
    u32 enabled;         /* TEX_ENABLE (control0) bit 31                    */
    u32 offset;          /* byte offset into location's address space       */
    u32 location;        /* RSX_LOCATION_* (from the format DMA selector)   */
    u32 format;          /* RSX_TEX_FMT_* byte incl. LN/UN modifier bits    */
    u32 dimension;       /* 1/2/3 = 1D/2D/3D                                */
    u32 cubemap;
    u32 mipmaps;         /* level count                                     */
    u32 width, height;   /* from TEX_NPOT_SIZE (image rect)                 */
    u32 pitch;           /* bytes per row (TEX_SIZE1 [0:19]); 0 for po2     */
    u32 depth;           /* TEX_SIZE1 [20:31]                               */
    u32 wrap;            /* raw TEX_ADDRESS word (wrap s/t/r, anisobias)    */
    u32 remap;           /* raw component-remap (swizzle) word              */
    u32 filter;          /* raw min/mag filter word (TEX_FILTER)            */
    u32 control0;        /* raw TEX_CONTROL0 word (enable[31], LOD fields)  */
    u32 border_color;
    u32 border;          /* 1: border texels (FORMAT bit 3 clear, RPCS3 !border_type()) */
} rsx_dsp_texture;
void rsx_dsp_get_texture(const rsx_dispatch* rsx, u32 unit, rsx_dsp_texture* out);

/* Vertex-texture units (NV4097_SET_VERTEX_TEXTURE_* at
 * 0x900 + unit*0x20). Transform-program TXL instructions use these for
 * per-instance placement and displacement data. */
#define RSX_DSP_NUM_VERTEX_TEXTURES 4
#define RSX_TEX_FMT_W16Z16Y16X16_FLOAT 0x9A
#define RSX_TEX_FMT_W32Z32Y32X32_FLOAT 0x9B
#define RSX_TEX_FMT_X32_FLOAT          0x9C
#define RSX_TEX_FMT_Y16X16_FLOAT       0x9F

typedef struct rsx_dsp_vertex_texture {
    u32 enabled;
    u32 offset;
    u32 location;
    u32 format;
    u32 dimension;
    u32 cubemap;
    u32 mipmaps;
    u32 width, height;
    u32 pitch;
    u32 depth;
    u32 wrap;
    u32 filter;
    u32 control0;
    u32 border_color;
} rsx_dsp_vertex_texture;
void rsx_dsp_get_vertex_texture(const rsx_dispatch* rsx, u32 unit,
                                rsx_dsp_vertex_texture* out);

/* Transform constants as floats (vec4 slot) */
static inline const u32* rsx_dsp_constant(const rsx_dispatch* rsx, u32 slot)
{
    return rsx->constants[slot < RSX_DSP_NUM_CONSTANTS ? slot : 0];
}

/* Fragment program: SHADER_PROGRAM register (offset | dma-context in [1:0],
 * 1 = local, 2 = main). Returns the byte offset, *location gets RSX_LOCATION_*. */
u32 rsx_dsp_fragment_program(const rsx_dispatch* rsx, u32* location);

/* SHADER_CONTROL raw word (register count, fp16-output flag, ...) */
u32 rsx_dsp_shader_control(const rsx_dispatch* rsx);

/* Transform program execution start slot (VP_START_FROM_ID) */
u32 rsx_dsp_vp_start(const rsx_dispatch* rsx);

/* Primitive restart (NV4097_SET_RESTART_INDEX_ENABLE/_INDEX): a sentinel
 * index value that ends the current TRIANGLE_STRIP/FAN run with NO
 * connecting triangle to whatever comes next. index_is_u32 selects which
 * width rule applies (RPCS3 oracle: rsx_methods.h restart_index_enabled()
 * disables restart for 16-bit index buffers if the configured value
 * doesn't fit in 16 bits). */
int rsx_dsp_restart_index_enabled(const rsx_dispatch* rsx, int index_is_u32);
u32 rsx_dsp_restart_index(const rsx_dispatch* rsx);

/* Per-attribute immediate/default value (VTX_ATTR_4F register block):
 * what a disabled vertex attribute reads as. Falls back to (0,0,0,1) when
 * the register block was never written. */
void rsx_dsp_vertex_default(const rsx_dispatch* rsx, u32 index, float out[4]);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_DISPATCH_H */
