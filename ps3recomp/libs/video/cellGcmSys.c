/*
 * ps3recomp - cellGcmSys HLE module implementation
 *
 * Manages RSX initialization, display buffer registration, flip control,
 * command buffer control, tile/zcull configuration, IO memory mapping,
 * report/label areas, and address-to-offset translation.
 *
 * Actual rendering is handled elsewhere -- this module just tracks state.
 */

#include "rsx_live_draw.h"
#include "rsx_draw_engine.h"
#include "cellGcmSys.h"
#include "../../runtime/platform/win32_compat.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write32 (translate + byte-swap, OOB-safe) */
#include "../../runtime/memory/vm.h"    /* VM_HLE_INJECT_BASE */
#include "../../runtime/spu/spu_coherency.h" /* the blit row copy's reservation bookkeeping */
#include "rsx_commands.h"                    /* rsx_state, rsx_process_command_buffer */
#include "../../include/ps3emu/guest_call.h" /* g_ps3_guest_caller for direct handler dispatch */
#include "../../include/ps3emu/ps3_ftrace.h"  /* PS3_FTRACE timeline */

/* Guest EA of the GCM context (begin/end/current/callback) the title writes its
 * command stream into; recorded by cellGcmSetupContext, drained by the RSX. */
static u32 s_gcm_context_ea = 0;
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "ps3emu/log_first.h"
/* Resistance port: getenv() is called on hot paths (per draw / per method);
 * the UCRT version takes a lock and scans the environment every time, which
 * profiled at ~20% of the render thread. Cached by call-site string pointer. */
char* rsx_getenv(const char* name);
#define getenv(n) rsx_getenv(n)

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* ---------------------------------------------------------------------------
 * Timestamp helpers (same pattern as sys_timer)
 * -----------------------------------------------------------------------*/

#ifdef _WIN32
static LARGE_INTEGER s_qpc_freq;
static int           s_qpc_init = 0;

static void ensure_qpc_init(void)
{
    if (!s_qpc_init) {
        QueryPerformanceFrequency(&s_qpc_freq);
        s_qpc_init = 1;
    }
}

static u64 get_timestamp_ns(void)
{
    LARGE_INTEGER now;
    ensure_qpc_init();
    QueryPerformanceCounter(&now);
    /* Convert to nanoseconds: (count * 1e9) / freq */
    return (u64)((double)now.QuadPart * 1000000000.0 / (double)s_qpc_freq.QuadPart);
}
#else
static u64 get_timestamp_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_gcm_initialized = 0;
static u32  s_flip_mode   = CELL_GCM_DISPLAY_VSYNC;
static u32  s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
/* Flip requests (each puts the status back to WAITING) and the request a
 * pending flip completion was marked for: a completion delivered late must not
 * mark DONE a flip requested after it (see ppu_gcm_pump). */
static volatile u32 s_flip_req_seq, s_flip_tick_seq;
/* The status's read-modify-write transitions (request, tick, completion) run
 * on different threads; without a lock a completion could read "no newer
 * request" just before a request stored WAITING and then overwrite it. */
#ifdef _WIN32
static volatile LONG s_flip_lock;
#define FLIP_LOCK()   do { while (InterlockedCompareExchange(&s_flip_lock, 1, 0)) YieldProcessor(); } while (0)
#define FLIP_UNLOCK() InterlockedExchange(&s_flip_lock, 0)
#else
static volatile int s_flip_lock;
#define FLIP_LOCK()   do { while (__atomic_exchange_n(&s_flip_lock, 1, __ATOMIC_ACQUIRE)) {} } while (0)
#define FLIP_UNLOCK() __atomic_store_n(&s_flip_lock, 0, __ATOMIC_RELEASE)
#endif

/* Count of guest flip requests (SetFlipCommand / SetPrepareFlip). The present
 * loop uses this as the frame boundary: presenting only when a flip arrived
 * keeps partially-drained frames off screen (the ticker otherwise presents on
 * a fixed 16ms clock, and a drain that catches the guest mid-frame -- e.g.
 * while it's blocked in the FIFO-wrap recycle callback -- would show a
 * clear+few-draws frame as a visible flicker). */
static volatile u32 s_flip_request_count = 0;

u32 cellGcm_flip_request_count(void)
{
    return s_flip_request_count;
}
static u32  s_debug_level = CELL_GCM_DEBUG_LEVEL0;

/* Display buffers */
static CellGcmDisplayInfo s_display_buffers[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static int s_display_buffer_set[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static u32 s_current_display_buffer_id = 0;
/* Set by the flip (guest thread, at a get==put frame boundary); consumed by
 * the ticker, which presents the accumulated batch BEFORE draining further --
 * presenting on a raw flip-count change raced the drain and showed empty or
 * mixed batches (wave: black flashes, layout flicker). */
static volatile int s_flip_pending = 0;
/* A flip _cellGcmSetFlipCommand queued in the FIFO: | bufferId, bit 8 = ours. */
#define GCM_FLIP_MARKER 0xFEAD0100u
/* Flips queued into the command stream by gcm_flip_into_fifo, and how many of
 * those markers the drain has reached. The RSX completes a flip when it
 * executes it; until the drain reaches the marker, the flip has not happened,
 * however many vblanks go by. */
static volatile u32 s_fifo_flips_queued = 0, s_fifo_flips_reached = 0;
static u32 s_fifo_flips_completed = 0;   /* reached count at the last completion */
/* A flip requested outside the command stream (cellGcmSetFlipCommand, or a
 * 0xFEAD word the drain met), not completed yet. */
static volatile int s_flip_direct_pending = 0;

/* May this beat complete a flip? Yes if the drain has executed a flip marker
 * since the last completion, or if a flip that never went through the stream
 * is waiting. A flip that is queued, or written but not yet flushed, does not
 * hold back the completion of one already executed: the RSX reports the flip
 * it did, not the ones still ahead of it. With no flip outstanding there is
 * nothing to complete: Twisted Metal resets the flip status, takes about a
 * second to build its next frame here, then queues the flip, and a beat in
 * between that reported a flip made it take the new one for done. It then
 * rebuilt the command segment the walker was still waiting in. */
static int gcm_flip_executed_take(void)
{
    const u32 r = s_fifo_flips_reached;
    if (r != s_fifo_flips_completed) { s_fifo_flips_completed = r; return 1; }
    if (s_flip_direct_pending) { s_flip_direct_pending = 0; return 1; }
    return 0;
}
/* ponytail: fixed headroom, far above one frame's commands (Simpsons ~10 KB);
 * derive it from the observed per-frame volume if a title outgrows it. */
#define GCM_FLIP_WRAP_BYTES 0x40000u

int cellGcm_take_flip_pending(void)
{
    int v = s_flip_pending;
    s_flip_pending = 0;
    return v;
}

/* The flip now names this buffer. The shared draw engine presents on the
 * host's call and only learns a buffer from 0xE944 by itself, so it would
 * keep presenting display buffer 0 for a title that flips through 0xFEAD. */
static void gcm_set_current_display_buffer(u32 bufferId)
{
    s_current_display_buffer_id = bufferId;
    if (rsx_draw_engine_enabled())
        rsx_draw_engine_note_flip(bufferId);
}


/* Configuration */
static CellGcmConfig s_config;
/* Offset pages (1MB) known to be LOCAL-memory-derived (set by
 * cellGcmAddressToOffset when the guest converts a localAddress-range EA). */
static u8 s_local_offset_page[1024];

/* Command buffer control */
static CellGcmControl s_control;

/* Offset table storage
 *
 * set_offset_table/clear_offset_table keep these two host arrays in step with
 * the IO mappings, and that part was always fine. What was not: the tables
 * cellGcmGetOffsetTable hands out are indexed BY THE GUEST --
 * ioAddress[ea >> 20] and eaAddress[io >> 20] -- and it handed over 64-bit host
 * pointers through a struct whose guest form is two 4-byte fields, so the title
 * got two truncated addresses pointing at nothing it could read.
 *
 * The guest-visible copy lives in the reserved GCM window alongside the label
 * and control blocks, published on each call. 4096 pages covers the 4 GiB
 * address space at 1 MiB granularity, 8 KiB a side, and this is not a hot path.
 * 0xFFFF means "not mapped", as it does in the host tables. */
/* Default home for the HLE-visible GCM window; a port overrides it before
 * running guest code when its title claims this address range. */
uint32_t ppu_hle_inject_base = 0x20000000u;

static u16 s_io_address_table[65536];
static u16 s_ea_address_table[65536];

#define GCM_OFFSET_TABLE_PAGES   4096u                 /* 4 GiB / 1 MiB       */
#define GCM_OFFSET_TABLE_IO_ADDR (VM_HLE_INJECT_BASE + 0x3000u)           /* ea >> 20  -> io>>20 */
#define GCM_OFFSET_TABLE_EA_ADDR (VM_HLE_INJECT_BASE + 0x5000u)           /* io >> 20  -> ea>>20 */

/* Local memory bump allocator */
static u32 s_local_mem_allocated = 0;  /* next free offset in local memory */

/* IO memory mapping table */
typedef struct IoMapping {
    u32 ea;         /* effective address in main memory */
    u32 io;         /* IO offset (RSX-visible) */
    u32 size;       /* size of mapping */
    int active;     /* 1 if this slot is in use */
} IoMapping;

static IoMapping s_io_mappings[CELL_GCM_MAX_IO_MAPPINGS];
static int s_io_mapping_count = 0;

/* Callback handlers — these are GUEST OPD addresses passed by the
 * recompiled game, not host function pointers. Stored as uint32_t and
 * invoked through g_ps3_guest_caller (see ps3emu/guest_call.h). */

/* Handler mask exported for the RSX event delivery path. Bit layout matches
 * the RPCS3 SYS_RSX_EVENT enum (vblank=0x02, flip=0x04, user_cmd=0x80).
 * The runner's vblank tick reads this to decide which events to send to
 * the libgcm interrupt thread. Updated by Set*Handler below.
 *
 * If g_gcm_driver_info_ea is non-zero, the mask is also written to
 * driver_info + 0x12C0 so the game's own libgcm interrupt path can read it
 * directly from guest memory. The runner sets g_gcm_driver_info_ea during
 * RSX context allocation. */
uint32_t g_gcm_handler_mask = 0;
uint32_t g_gcm_driver_info_ea = 0;

static u32 s_flip_handler_opd     = 0;

/* GCM_FLIPCB_ONTICK=1: fire the guest flip handler only when the flip
 * completes (cellGcmTickFlip), not also when it is requested. */
static int gcm_flipcb_on_tick_only(void)
{
    static int v = -1;
    if (v < 0) v = getenv("GCM_FLIPCB_ONTICK") ? 1 : 0;
    return v;
}

static u32 s_vblank_handler_opd   = 0;
static u32 s_user_handler_opd     = 0;
static u32 s_second_v_handler_opd = 0;
/* YDKJ_HANDLERFIX: the handler OPD's code word (*(opd)) gets clobbered to 0 by a
 * mislifted store, so g_ps3_guest_caller reads code=0 -> "not registered" and the
 * game's per-flip/vblank callback never fires -> render/advance loop stalls. We
 * capture the code at Set*Handler time (OPD still valid) and restore the OPD word
 * before each invocation if it's been clobbered. */
extern u32 vm_read32(unsigned int);
static u32 s_flip_handler_code    = 0;
static u32 s_vblank_handler_code  = 0;
static void ydkj_restore_handler_opd(u32 opd, u32 code) {
    if (!getenv("YDKJ_HANDLERFIX") || !opd || !code) return;
    extern u8* vm_base; if (!vm_base) return;
    if (vm_read32(opd) == 0) {
        u8* p = vm_base + opd; p[0]=(u8)(code>>24); p[1]=(u8)(code>>16); p[2]=(u8)(code>>8); p[3]=(u8)code;
        static int _n=0; if(PS3_LOG_FIRST(_n, 6)) fprintf(stderr,"[HANDLERFIX] restored clobbered OPD 0x%08X code=0x%08X\n",opd,code);
    }
}
static void gcm_update_handler_mask(void)
{
    uint32_t m = 0;
    if (s_vblank_handler_opd) m |= 0x02;
    if (s_flip_handler_opd)   m |= 0x04;
    if (s_user_handler_opd)   m |= 0x80;
    g_gcm_handler_mask = m;
    if (g_gcm_driver_info_ea)
        vm_write32(g_gcm_driver_info_ea + 0x12C0, m);
}

/* Legacy host-typed slots kept around for any caller still treating
 * these as host pointers. New code should use the _opd slots. */
static CellGcmFlipHandler    s_flip_handler    = NULL;
static CellGcmVBlankHandler  s_vblank_handler  = NULL;
static CellGcmUserHandler    s_user_handler    = NULL;
static CellGcmSecondVHandler s_second_v_handler = NULL;

/* Flip timing */
static u64 s_last_flip_time = 0;

/* VBlank counter (incremented on each flip as approximation) */
static u32 s_vblank_count = 0;

/* IO map reservation tracking */
static u32 s_io_map_reserved = 0;  /* bytes reserved for IO mapping */

/* Default FIFO mode */
static s32 s_default_fifo_mode = 0;

/* Graphics / queue handlers */
static CellGcmGraphicsHandler s_graphics_handler = NULL;
static CellGcmQueueHandler    s_queue_handler    = NULL;

/* Second V / VBlank frequency */
static u32 s_second_v_frequency = 0;
static u32 s_vblank_frequency   = 0;

/* User command */
#ifdef _WIN32
static volatile LONG s_user_command = 0;
#define GCM_USER_STORE(cmd) InterlockedExchange(&s_user_command, (LONG)(cmd))
/* Pair the pending marker with its cause so a later producer cannot
 * overwrite a cause already claimed by the callback pump. */
static volatile LONG64 s_user_pending = 0;
#define GCM_USER_PENDING_STORE(value) InterlockedExchange64(&s_user_pending, (LONG64)(value))
#define GCM_USER_PENDING_TAKE() ((u64)InterlockedExchange64(&s_user_pending, 0))
#else
#include <stdatomic.h>
static atomic_uint s_user_command = 0;
#define GCM_USER_STORE(cmd) atomic_store(&s_user_command, (cmd))
static _Atomic(u64) s_user_pending = 0;
#define GCM_USER_PENDING_STORE(value) atomic_store(&s_user_pending, (value))
#define GCM_USER_PENDING_TAKE() atomic_exchange(&s_user_pending, 0)
#endif

/* Tile configuration (up to 15 tiles, 8 commonly used) */
static CellGcmTileInfo s_tiles[CELL_GCM_MAX_TILE_COUNT];

/* Zcull configuration (8 zcull regions) */
static CellGcmZcullInfo s_zcull[CELL_GCM_MAX_ZCULL_COUNT];

/* RPCS3 gcm_config tiles_addr / zculls_addr: the guest copies of the packed
 * CellGcmTileInfo (16 bytes) / CellGcmZcullInfo (24 bytes) arrays. */
static u32 s_tiles_guest_ea, s_zculls_guest_ea;
static void gcm_register_ctx_handlers(void);

/* Report data area (256 slots, each 16 bytes) */
static CellGcmReportData s_report_data[CELL_GCM_MAX_REPORT_COUNT];

/* NV4097_SET_CONTEXT_DMA_REPORT (0x1A8): where NV4097_GET_REPORT writes.
 * Local (0xFEED0000, the default): the s_report_data slots above, read back
 * through cellGcmGetReport* / cellGcmGetReportDataAddress. Main memory
 * (0xFEED0001, or CELL_GCM_CONTEXT_DMA_REPORT_LOCATION_MAIN 0xBAD68000): the
 * report's 24-bit offset is an IO offset, and the 16-byte report goes to
 * guest memory at gcm_io2ea(offset), where the title reads it itself.
 * Killzone 3 sets 0xFEED0001 and keeps its reports in its own frame data. */
static u32 s_report_dma = 0x66626660u;
/* RPCS3 rsx_methods.cpp init: CONTEXT_DMA_REPORT = REPORT_LOCATION_LOCAL.
 * GCM_REPORT_DMA_LOCAL_INIT=0 restores the old 0xFEED0000 seed. */
static u32 gcm_report_dma_init(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GCM_REPORT_DMA_LOCAL_INIT"); on = !(e && atoi(e) == 0); }
    return on ? 0x66626660u : 0xFEED0000u;
}
static u64 s_report_last_ts;

/* GET_REPORT timestamp in nanoseconds, strictly rising: two reports in one
 * walker pass must not share a time stamp or go backwards. */
static u64 gcm_report_timestamp(void)
{
    u64 t = get_timestamp_ns();
    if (t <= s_report_last_ts) t = s_report_last_ts + 1u;
    s_report_last_ts = t;
    return t;
}

/* Label area (256 labels, each u32). Labels live in GUEST memory so the
 * recompiled game can poll them via vm_read32; cellGcmGetLabelAddress returns a
 * guest address into this window (16-byte spaced as on hardware). A host array
 * pointer would be read as a guest offset and land out of bounds. */
#define GCM_LABEL_GUEST_BASE  (VM_HLE_INJECT_BASE + 0x0000u)
#define GCM_LABEL_STRIDE      0x10u
static u32 s_labels[CELL_GCM_MAX_LABEL_COUNT];

/* GCM control register (put/get/ref). This MUST live in guest memory: the title
 * takes the pointer from cellGcmGetControlRegister, writes `put` when it kicks
 * the FIFO, and spins reading `get`/`ref` to know the RSX has caught up. A host
 * pointer would be read through vm_base as a guest address and never update, so
 * every FIFO-space / cellGcmFinish wait would hang. Placed just past the 256
 * label slots (0x03000000..0x03001000). Fields are big-endian (vm_write32). */
#define GCM_CONTROL_GUEST_ADDR (VM_HLE_INJECT_BASE + 0x2000u)
/* Resistance port: the guest-visible local report area (65536 x 16 bytes). */
#define GCM_REPORT_GUEST_BASE (VM_HLE_INJECT_BASE + 0x200000u)

/* Synthetic guest code EA for the FIFO command-buffer-full callback. The title's
 * inline gcmReserve calls context->callback(context, count) when `current` nears
 * `end`; we point the context's callback OPD at this EA and register it in the
 * PPU function table (ppu_sysprx.cpp: hle_gcm_callback) so the indirect call
 * routes back into cellGcm_fifo_recycle(). Lives in the injected control page,
 * never real guest code. MUST match GCM_FIFO_CALLBACK_SENTINEL_EA there. */
#define GCM_FIFO_CALLBACK_SENTINEL_EA (VM_HLE_INJECT_BASE + 0x2F00u)

/* R2_RSX=rpcs3: RPCS3's RSX core does the RSX (libs/video/rpcs3_bridge). This
 * file then plays RPCS3's cellGcmSys HLE module: the RSX thread reads the FIFO
 * and put/get/ref itself, and labels/reports live where RPCS3 puts them -- the
 * RsxReports block (semaphores +0, notify +0x1000, reports +0x1400, 0x9400
 * bytes). The control register stays at GCM_CONTROL_GUEST_ADDR (the lifted
 * code polls it there), so RPCS3's RsxDmaControl starts 0x40 before it. */
#include "rpcs3_bridge/r2rsx_bridge.h"
#define R2RSX_LABEL_EA    (VM_HLE_INJECT_BASE + 0x10000u)
#define R2RSX_REPORT_EA   (R2RSX_LABEL_EA + 0x1400u)
#define R2RSX_DRVINFO_EA  (VM_HLE_INJECT_BASE + 0x1A000u)
#define R2RSX_DEVICE_EA   (VM_HLE_INJECT_BASE + 0x30000u)
static int s_r2rsx = 0;
int cellGcm_rpcs3_mode(void) { return s_r2rsx; }

/* EA the ticker's RSX drain has consumed up to. Exposed so the wrap callback can
 * wait for the RSX to catch up before recycling the ring (else the frame's
 * just-written commands are lost). Updated in cellGcm_rsx_process_fifo. */
volatile u32 g_gcm_fifo_drained_ea = 0;

/* The guest variable cellGcmInit's ctx_out pointed at -- the title's own
 * gCellGcmCurrentContext. A title that switches to a command buffer of its own
 * does it by repointing this, and until something reads it back the runtime has
 * no way to notice. GCM_CTXDBG=1 reports it. */
static u32 s_gcm_ctx_out_ea = 0;

/* Set by cellGcm_syscall_bringup: this run reaches RSX through the lv2 sys_rsx_*
 * syscalls (libs/video/sys_rsx.c) rather than through the HLE cellGcm* imports. */
static int s_gcm_syscall_mode = 0;

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -----------------------------------------------------------------------*/

/* Populate the offset table entries for an IO mapping */
static void populate_offset_table(u32 ea, u32 io, u32 size)
{
    /*
     * The offset table uses 1MB pages (20-bit shift).
     * ioAddress[ea >> 20] = io >> 20   (EA -> IO offset)
     * eaAddress[io >> 20] = ea >> 20   (IO offset -> EA)
     */
    /* Round UP: a mapping is described by whole 1MB table entries, so a size
     * that is not a multiple of 1MB still needs its final partial page mapped.
     * Truncating dropped that page -- and dropped the mapping ENTIRELY for any
     * size below 1MB (pages == 0) -- after which cellGcmResolveOffset finds no
     * IO entry and falls back to VRAM, where the guest never wrote anything. */
    u32 pages = (size + 0xFFFFFu) >> 20;  /* number of 1MB pages, rounded up */
    u32 ea_page = ea >> 20;
    u32 io_page = io >> 20;

    for (u32 i = 0; i < pages && (ea_page + i) < 65536 && (io_page + i) < 65536; i++) {
        s_io_address_table[ea_page + i] = (u16)(io_page + i);
        s_ea_address_table[io_page + i] = (u16)(ea_page + i);
    }
    if (s_r2rsx && pages) r2rsx_iomap(ea_page << 20, io_page << 20, pages << 20);
}

/* Clear offset table entries for an IO mapping */
static void clear_offset_table(u32 ea, u32 io, u32 size)
{
    u32 pages = size >> 20;
    u32 ea_page = ea >> 20;
    u32 io_page = io >> 20;

    for (u32 i = 0; i < pages && (ea_page + i) < 65536 && (io_page + i) < 65536; i++) {
        s_io_address_table[ea_page + i] = 0xFFFF;
        s_ea_address_table[io_page + i] = 0xFFFF;
    }
    if (s_r2rsx && pages) r2rsx_iounmap(io_page << 20, pages << 20);
}

/* Find an IO mapping by EA */
static IoMapping* find_mapping_by_ea(u32 ea)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active && s_io_mappings[i].ea == ea)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Find an IO mapping by IO offset */
static IoMapping* find_mapping_by_io(u32 io)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active && s_io_mappings[i].io == io)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Find a free IO mapping slot */
static IoMapping* find_free_mapping(void)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (!s_io_mappings[i].active)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Valid tiled pitch sizes (power-of-two aligned, common RSX values) */
static const u32 s_valid_pitches[] = {
    0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800,
    0x0A00, 0x0C00, 0x0D00, 0x0E00, 0x1000, 0x1400, 0x1800,
    0x1A00, 0x1C00, 0x2000, 0x2800, 0x3000, 0x3400, 0x3800,
    0x4000, 0x5000, 0x6000, 0x6800, 0x7000, 0x8000, 0xA000,
    0xC000, 0xD000, 0xE000, 0x10000
};
static const int s_valid_pitch_count = sizeof(s_valid_pitches) / sizeof(s_valid_pitches[0]);

/* ---------------------------------------------------------------------------
 * API implementations -- Initialization
 * -----------------------------------------------------------------------*/

/* RPCS3 sys_rsx_context_allocate fills RsxReports before the title runs: the
 * semaphore words (labels are every 4th, 16 bytes apart) repeat 0x1337C0D3,
 * 0x1337BABE, 0x1337BEEF, 0x1337F001, and the 2048 local reports read
 * {timestamp -1, value 0, pad -1}. A label or report the title reads before
 * anything wrote it sees that, not zero. RSX_LABEL_INIT_LEGACY=1 leaves zeros.
 * (RPCS3's notify[] block has no native counterpart: cellGcmGetNotifyDataAddress
 * hands out the title's IO page 0xF1.) */
static void gcm_init_report_memory(void)
{
    static int legacy = -1;
    if (legacy < 0) { const char* e = getenv("RSX_LABEL_INIT_LEGACY"); legacy = e && e[0] && e[0] != '0'; }
    if (legacy) return;
    /* ppu_vm_size != 0: a partially backed guest (unit tests); skip what is not there. */
    const int labels_ok = !ppu_vm_size ||
        (u64)GCM_LABEL_GUEST_BASE + CELL_GCM_MAX_LABEL_COUNT * GCM_LABEL_STRIDE <= ppu_vm_size;
    const int reports_ok = !ppu_vm_size || (u64)GCM_REPORT_GUEST_BASE + 2048u * 16u <= ppu_vm_size;
    static const u32 sema[4] = { 0x1337C0D3u, 0x1337BABEu, 0x1337BEEFu, 0x1337F001u };
    for (u32 i = 0; labels_ok && i < CELL_GCM_MAX_LABEL_COUNT * GCM_LABEL_STRIDE / 4u; i++)
        vm_write32(GCM_LABEL_GUEST_BASE + i * 4u, sema[i & 3u]);
    for (u32 i = 0; i < 2048u; i++) {
        const u32 r = GCM_REPORT_GUEST_BASE + i * 16u;
        if (reports_ok) {
            vm_write32(r + 0u, 0xFFFFFFFFu);
            vm_write32(r + 4u, 0xFFFFFFFFu);
            vm_write32(r + 8u, 0u);
            vm_write32(r + 12u, 0xFFFFFFFFu);
        }
        s_report_data[i].timestamp = ~0ull;
        s_report_data[i].value = 0u;
    }
}

/* NID: 0xB2E761D4 */
s32 cellGcmInit(u32 cmdSize, u32 ioSize, u32 ioAddress)
{
    printf("[cellGcmSys] Init(cmdSize=0x%X, ioSize=0x%X, ioAddr=0x%08X)\n",
           cmdSize, ioSize, ioAddress);

    if (s_gcm_initialized) {
        printf("[cellGcmSys] WARNING: already initialized\n");
        return CELL_GCM_ERROR_FAILURE;
    }

    memset(s_display_buffers, 0, sizeof(s_display_buffers));
    memset(s_display_buffer_set, 0, sizeof(s_display_buffer_set));
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_control, 0, sizeof(s_control));
    memset(s_io_mappings, 0, sizeof(s_io_mappings));
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_zcull, 0, sizeof(s_zcull));
    memset(s_report_data, 0, sizeof(s_report_data));
    s_report_dma = gcm_report_dma_init();
    memset(s_labels, 0, sizeof(s_labels));
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));

    /*
     * Populate a plausible configuration.
     * In a real implementation these would reflect actual allocated memory.
     */
    s_config.localAddress    = 0xC0000000;  /* typical RSX local mem base */
    s_config.localSize       = 256 * 1024 * 1024;  /* 256 MB */
    s_config.ioAddress       = ioAddress;
    s_config.ioSize          = ioSize;
    s_config.memoryFrequency = 650000000;   /* 650 MHz */
    s_config.coreFrequency   = 500000000;   /* 500 MHz */

    s_local_mem_allocated = 0;
    s_io_mapping_count = 0;
    /* Unmapped = 0xFFFF. The tables are static (zero-initialized), so without
     * this every unmapped EA page silently aliased io page 0 (and vice versa),
     * producing plausible-but-wrong offsets instead of a translation failure. */
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    s_current_display_buffer_id = 0;
    s_flip_handler = NULL;
    s_vblank_handler = NULL;
    s_user_handler = NULL;
    s_second_v_handler = NULL;
    s_last_flip_time = get_timestamp_ns();
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    s_flip_mode = CELL_GCM_DISPLAY_VSYNC;
    s_debug_level = CELL_GCM_DEBUG_LEVEL0;
    s_vblank_count = 0;
    s_io_map_reserved = 0;
    s_default_fifo_mode = 0;
    s_graphics_handler = NULL;
    s_queue_handler = NULL;
    s_second_v_frequency = 0;
    s_vblank_frequency = 0;
    GCM_USER_STORE(0);
    GCM_USER_PENDING_STORE(0);

    /* Set up the initial IO mapping for the command buffer region */
    if (ioAddress != 0 && ioSize > 0) {
        populate_offset_table(ioAddress, 0, ioSize);

        s_io_mappings[0].ea     = ioAddress;
        s_io_mappings[0].io     = 0;
        s_io_mappings[0].size   = ioSize;
        s_io_mappings[0].active = 1;
        s_io_mapping_count = 1;
    }

    /* Zero the guest-visible control register (put/get); ref starts at
     * 0xFFFFFFFF as RPCS3 rsx_methods.cpp:683 (NV406E_SET_REFERENCE = umax)
     * and RSXThread.cpp's dma control init. GCM_REF_INIT_MAX=0 starts it at 0. */
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, 0);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, 0);
    { const char* e = getenv("GCM_REF_INIT_MAX");
      vm_write32(GCM_CONTROL_GUEST_ADDR + 8, (e && atoi(e) == 0) ? 0u : 0xFFFFFFFFu); }

    /* Watch SPU DMA for pushbuffers. A title that builds its command stream on
     * an SPU never tells libgcm where that stream is, so the write is the only
     * evidence of it -- see gcm_note_spu_put. Costs one NULL check per SPU DMA
     * for titles that do not. */
    { extern void (*g_spu_put_hook)(u32 ea, u32 size);
      void gcm_note_spu_put(u32 ea, u32 size);       /* defined with the walker */
      void gcm_forget_spu_blocks(void);
      gcm_forget_spu_blocks();
      g_spu_put_hook = gcm_note_spu_put; }

    if (r2rsx_enabled()) {
        if (r2rsx_context_allocate(GCM_CONTROL_GUEST_ADDR - 0x40u, R2RSX_LABEL_EA,
                                   R2RSX_DRVINFO_EA, R2RSX_DEVICE_EA, s_config.localSize) == 0) {
            extern void (*g_spu_put_hook)(u32 ea, u32 size);
            g_spu_put_hook = NULL;          /* only our FIFO walker wanted it */
            s_r2rsx = 1;
            if (ioAddress != 0 && ioSize > 0)
                r2rsx_iomap(ioAddress, 0, ioSize);
            printf("[cellGcmSys] RSX: RPCS3 core (control=0x%08X labels=0x%08X)\n",
                   GCM_CONTROL_GUEST_ADDR, R2RSX_LABEL_EA);
        } else {
            printf("[cellGcmSys] RSX: RPCS3 core failed to start -- using the built-in walker\n");
        }
    }

    s_gcm_initialized = 1;
    if (!s_r2rsx) gcm_init_report_memory();
    return CELL_OK;
}

/* The runtime-owned ring (cellGcmSetupContext), for the segmented recycle. */
static u32 s_ring_base_ea = 0, s_ring_size = 0;

/* Reusable _cellGcmInitBody core (NID 0x15BAE46B). See cellGcmSys.h for why the
 * guest vm is injected as callbacks. The CellGcmContextData layout here is the
 * one proven in the shipping Simpsons port (begin@0/end@4/current@8/callback@C);
 * getting it wrong makes the game read `current` as the callback OPD and stall
 * in cellGcmFlush. The command buffer is the start of the game's IO region. */
u32 cellGcmSetupContext(u32 ctx_out_addr, u32 cmdSize, u32 ioSize, u32 ioAddress,
                        CellGcmGuestAlloc galloc, CellGcmGuestWrite32 gwrite32)
{
    if (cmdSize < 0x10000)
        cmdSize = 0x10000;

    /* PSL1GHT's gcmInitBody passes ioAddress = 0, expecting gcm to allocate the
     * IO region itself (the official SDK has the app allocate and pass it).
     * Taking the 0 literally put the FIFO command buffer at guest address 0 --
     * the guest then wrote its command stream over low memory and every offset
     * resolution failed. Allocate a guest region of ioSize instead. */
    if (ioAddress == 0 && galloc)
        ioAddress = galloc(ioSize ? ioSize : 0x100000, 0x100000);

    cellGcmInit(cmdSize, ioSize, ioAddress);
    gcm_register_ctx_handlers();
    if (galloc && !s_tiles_guest_ea) {
        s_tiles_guest_ea = galloc(16u * CELL_GCM_MAX_TILE_COUNT, 16);
        s_zculls_guest_ea = galloc(24u * CELL_GCM_MAX_ZCULL_COUNT, 16);
        for (u32 i = 0; s_tiles_guest_ea && i < 16u * CELL_GCM_MAX_TILE_COUNT; i += 4) vm_write32(s_tiles_guest_ea + i, 0);
        for (u32 i = 0; s_zculls_guest_ea && i < 24u * CELL_GCM_MAX_ZCULL_COUNT; i += 4) vm_write32(s_zculls_guest_ea + i, 0);
    }

    u32 cmdbuf = ioAddress;                 /* FIFO lives at the IO region start */
    s_ring_base_ea = cmdbuf;
    s_ring_size = cmdSize;
    u32 cdata  = galloc ? galloc(16, 16) : 0;
    if (cdata && gwrite32) {
        gwrite32(cdata + 0x0, cmdbuf);              /* begin   */
        gwrite32(cdata + 0x4, cmdbuf + cmdSize);    /* end     */
        gwrite32(cdata + 0x8, cmdbuf);              /* current */
        /* Command-buffer-full callback: a guest OPD {func, toc} routed to
         * hle_gcm_callback -> cellGcm_fifo_recycle. Without a real callback the
         * ring never recycles and the FIFO wedges once `current` reaches `end`. */
        u32 opd = galloc ? galloc(8, 8) : 0;
        if (opd) {
            gwrite32(opd + 0x0, GCM_FIFO_CALLBACK_SENTINEL_EA);  /* func EA */
            gwrite32(opd + 0x4, 0);                              /* toc (unused) */
            gwrite32(cdata + 0xC, opd);                          /* callback OPD */
        } else {
            gwrite32(cdata + 0xC, 0);
        }
        if (ctx_out_addr)
            gwrite32(ctx_out_addr, cdata);          /* *context = &ctxdata */
        s_gcm_ctx_out_ea = ctx_out_addr;   /* the title's gCellGcmCurrentContext */
        s_gcm_context_ea = cdata;                   /* RSX drains commands from here */
    }
    return cdata;
}

s32 cellGcmGetConfiguration(CellGcmConfig* config)
{
    /* `config` is a GUEST address; write the 6 u32 fields big-endian via
     * vm_write32 (a raw *config = s_config faults / is host-endian). The game
     * reads localAddress/localSize from here to lay out VRAM framebuffers. */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!cfg)
        return CELL_GCM_ERROR_INVALID_VALUE;

    vm_write32(cfg +  0, s_config.localAddress);
    vm_write32(cfg +  4, s_config.ioAddress);
    vm_write32(cfg +  8, s_config.localSize);
    vm_write32(cfg + 12, s_config.ioSize);
    vm_write32(cfg + 16, s_config.memoryFrequency);
    vm_write32(cfg + 20, s_config.coreFrequency);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Command buffer control
 * -----------------------------------------------------------------------*/

/* NID: 0x8572A8E0 */
CellGcmControl* cellGcmGetControlRegister(void)
{
    /* Return the GUEST address of the control register (not &s_control, a host
     * pointer). The recompiled title reads/writes it through vm_base, so it must
     * be a guest EA for put/get/ref to actually flow. */
    return (CellGcmControl*)(uintptr_t)GCM_CONTROL_GUEST_ADDR;
}

/* ---------------------------------------------------------------------------
 * Display / flip
 * -----------------------------------------------------------------------*/

/* NID: 0xDB23E867 */
u32 cellGcmGetCurrentField(void)
{
    /* Returns 0 or 1 for interlaced; always 0 for progressive. */
    return 0;
}

/* NID: 0xA53D12AE */
/* v179 diag: frames to capture automatically (rsxcap) when a movie ends
 * (flip mode VSYNC -> back to the game's mode): the intro-skip static flash.
 * RSX_CAP_MOVIEEND=<n> (default 0 = off). Read by the D3D12 backend. */
volatile int g_rsx_cap_request = 0;
void cellGcmSetFlipMode(u32 mode)
{
    printf("[cellGcmSys] SetFlipMode(mode=%u)\n", mode);
    static int seen2 = 0; if (mode == 2) seen2 = 1;   /* the initial mode is 2 too: only after an explicit SetFlipMode(2) */
    if (seen2 && s_flip_mode == 2 && mode != 2) {
        static int n = -1; if (n < 0) { const char* e = getenv("RSX_CAP_MOVIEEND"); n = e ? atoi(e) : 0; }
        if (n > 0) g_rsx_cap_request = n;
    }
    s_flip_mode = mode;
    if (s_r2rsx) r2rsx_set_flip_mode(mode);
}
/* v126: the port's profiler skips movie playback (the game sets VSYNC, 2, for movies) */
u32 cellGcm_get_flip_mode(void) { return s_flip_mode; }

void ppu_gcm_pump(void);
/* Resistance port (v126): a title that paces itself (the game's frame
 * limiter) above 60 fps would be held by this wait for the 60 Hz flip beat on
 * every frame. The port turns it off then (cellGcm_set_waitflip_block(0)). */
static volatile int s_waitflip_block = 1;
void cellGcm_set_waitflip_block(int on)
{
    s_waitflip_block = on ? 1 : 0;
    printf("[cellGcmSys] SetWaitFlip %s%c", on ? "waits for the flip" : "returns at once (title paces itself)", 10);
}
/* NID: 0xC44D8F34 */
static void gcm_r2rsx_emit(u32 ctx, const u32* words, u32 n);
void cellGcmSetWaitFlip(void)
{
    if (s_r2rsx) {
        /* RPCS3: the RSX waits on the flip semaphore (label 1) the flip clears. */
        const u32 w[3] = { 0x00080064u /* NV406E_SEMAPHORE_OFFSET x2 */, 0x10u, 0u };
        gcm_r2rsx_emit(0, w, 3);
        return;
    }
    if (!s_waitflip_block) return;
    /* Block until the pending flip completes (the present thread marks it done
     * via cellGcmTickFlip, ~once per display refresh). This throttles the title
     * to vsync; without it the guest loops unthrottled and many frames batch
     * into a single host present -> the console text stacks/duplicates. */
#ifdef _WIN32
    __declspec(dllimport) void __stdcall Sleep(unsigned long);
#endif  /* elsewhere: runtime/platform/win32_compat.h */
    /* Resistance port (v86): deliver the completion from THIS thread and
     * poll finely. The tick only marks a flip complete; status becomes DONE
     * when some guest thread passes ppu_gcm_pump, and this loop never called
     * it -- the waiting main thread depended on another thread happening to
     * make an HLE call, then slept a further 1-2 ms per Sleep(1). Same 64 ms
     * bound. GCM_WAITFLIP_OLD=1 restores the Sleep(1) loop. */
#ifdef _WIN32
    static int old = -1;
    if (old < 0) { const char* e = getenv("GCM_WAITFLIP_OLD"); old = e ? atoi(e) : 0; }
    if (old) {
        for (int i = 0; i < 64 && s_flip_status == CELL_GCM_FLIP_STATUS_WAITING; i++)
            Sleep(1);
        return;
    }
    {
        LARGE_INTEGER f, t0, t;
        QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        const LONGLONG lim = t0.QuadPart + f.QuadPart * 64 / 1000;
        unsigned k = 0;
        for (;;) {
            ppu_gcm_pump();
            if (s_flip_status != CELL_GCM_FLIP_STATUS_WAITING) break;
            QueryPerformanceCounter(&t);
            if (t.QuadPart >= lim) break;
            if (++k & 15u) SwitchToThread(); else Sleep(0);
        }
    }
#else
    for (int i = 0; i < 64 && s_flip_status == CELL_GCM_FLIP_STATUS_WAITING; i++)
        Sleep(1);
#endif
    /* A flip still WAITING after the bound is left to the tick. Forcing DONE
     * here skipped its completion: the tick only completes WAITING flips, so
     * the flip handler never ran for it, and inFAMOUS 2, whose handler writes
     * the FIFO put it deferred, hung with get == put whenever a frame took
     * longer than the bound. */
}

/* NID: 0xDF6476BD. The same wait without libgcm's command-buffer space check;
 * this HLE writes no command, so it is the same call. GH3 makes it ~36 times
 * a boot and got a faked CELL_OK (no wait at all) before this was registered. */
void cellGcmSetWaitFlipUnsafe(void) { cellGcmSetWaitFlip(); }

/* NID: 0x51C9D62B */
void cellGcmResetFlipStatus(void)
{
    { static int n = 0;
      if (getenv("FLIP_DBG") && PS3_LOG_FIRST(n, 12))
          fprintf(stderr, "[FLIP] ResetFlipStatus%c", 10); }
    if (s_r2rsx) { r2rsx_set_flip_status(CELL_GCM_FLIP_STATUS_WAITING); return; }
    FLIP_LOCK();
    s_flip_status = CELL_GCM_FLIP_STATUS_WAITING;
    s_flip_req_seq++;
    FLIP_UNLOCK();
}

/* NID: 0xE315A0B2 */
u32 cellGcmGetFlipStatus(void)
{
    if (s_r2rsx) return r2rsx_get_flip_status();
    /* Avoid host-calling the guest OPDs directly (they're not valid host
     * function pointers). Use cellGcmTickVBlank() / TickFlip() instead,
     * which dispatch via g_ps3_guest_caller — see below. */
    /* Report the CURRENT status; cellGcmTickFlip (the ticker's 60Hz vblank
     * beat) completes pending flips. The old self-completing version made
     * every wait-for-flip loop exit on its first poll, so titles ran
     * completely unpaced (wave: 95 fps with a fixed-dt simulation). */
    { static int n = 0;
      if (getenv("FLIP_DBG") && PS3_LOG_FIRST(n, 24))
          fprintf(stderr, "[FLIP] GetFlipStatus -> %u (0=DONE 1=WAITING)%c",
                  s_flip_status, 10); }
    return s_flip_status;
}

/* Host-side ticks. The game's host driver (e.g. flow main.cpp) calls
 * these periodically so registered guest handlers fire — that's what
 * drives the title-screen state machine for many games. */
#include "ps3emu/guest_call.h"

/* ---------------------------------------------------------------------------
 * Serialized vblank/flip/user handler delivery.
 *
 * Tick producers publish pending work. The runner pumps it at HLE boundaries
 * or on a dedicated callback thread. The pump serializes guest handlers with
 * one another; it does not stop unrelated guest execution on other threads.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
#include <windows.h>
static volatile LONG s_gcm_pending = 0;    /* bit0 = vblank, bit1 = flip */
#define GCM_PENDING_SET(bits)  InterlockedOr(&s_gcm_pending, (bits))
#define GCM_PENDING_TAKE()     InterlockedExchange(&s_gcm_pending, 0)
static volatile LONG s_gcm_pumping = 0;
#define GCM_PUMP_TRY_ENTER() (InterlockedCompareExchange(&s_gcm_pumping, 1, 0) == 0)
#define GCM_PUMP_LEAVE()     InterlockedExchange(&s_gcm_pumping, 0)
#else
#include <stdatomic.h>
static atomic_int s_gcm_pending = 0;
#define GCM_PENDING_SET(bits)  atomic_fetch_or(&s_gcm_pending, (bits))
#define GCM_PENDING_TAKE()     atomic_exchange(&s_gcm_pending, 0)
static atomic_flag s_gcm_pumping = ATOMIC_FLAG_INIT;
#define GCM_PUMP_TRY_ENTER() (!atomic_flag_test_and_set_explicit(&s_gcm_pumping, memory_order_acquire))
#define GCM_PUMP_LEAVE()     atomic_flag_clear_explicit(&s_gcm_pumping, memory_order_release)
#endif

/* Called by the vblank ticker thread. NO guest code -- advance the vblank count
 * and mark a vblank+flip tick pending for the main thread to deliver. */
static u32 s_fifo_getoff;   /* tentative: defined below */

void cellGcm_request_tick(void)
{
    if (s_r2rsx) return;                     /* RPCS3's display engine ticks */
    s_vblank_count++;
    /* GCM_FLIP_NEEDS_FIFO=1: only mark a flip pending once the walker has
     * actually consumed up to `put`.
     *
     * A title's fifo-finish can use flip status as a fast path -- Twisted
     * Metal's reads it first and, if the flip is done, skips waiting for `get`
     * entirely. Completing a flip on the 60 Hz beat regardless of FIFO
     * progress therefore tells it the RSX has caught up when it has not, and
     * it recycles a FIFO block the walker is still inside, overwriting the
     * JUMP out of it. Off by default: a title whose FIFO never drains would
     * stop flipping entirely. */
    { static int need = -1;
      if (need < 0) { const char* e = getenv("GCM_FLIP_NEEDS_FIFO"); need = e ? atoi(e) : 0; }
      if (need) {
          u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
          const int flip = s_fifo_getoff == put;
          if (flip) { FLIP_LOCK(); s_flip_tick_seq = s_flip_req_seq; FLIP_UNLOCK(); }
          GCM_PENDING_SET(flip ? 3 : 1);
          return;
      } }
    /* A flip queued in the command stream completes when the drain reaches
     * its marker, as on the RSX; until then this beat is a vblank only.
     * Completing it on the beat told Twisted Metal the RSX had finished frames
     * it had not reached, and the title rebuilt command segments the walker
     * was still waiting to execute. Flips that never went through the stream
     * are unaffected. The completion is stamped with the flip requested last
     * (see ppu_gcm_pump). */
    if (gcm_flip_executed_take()) {
        FLIP_LOCK(); s_flip_tick_seq = s_flip_req_seq; FLIP_UNLOCK();
        GCM_PENDING_SET(3);
    } else {
        GCM_PENDING_SET(1);
    }
}

/* Deliver pending handlers on the calling host thread. Serialize the entire
 * claim-and-deliver sequence across threads: an HLE boundary and a host ticker
 * may both pump, and newer causes must not overtake already claimed callbacks.
 * A nonblocking guard also prevents nested callbacks from reentering. Leave
 * pending notifications untouched when another pump is active. */
static void gcm_hle_fifo_sync(void);
void ppu_gcm_pump(void)
{
    if (s_r2rsx) return;                     /* handlers run on the bridge's intr thread */
    gcm_hle_fifo_sync();   /* v180 */
    if (!GCM_PUMP_TRY_ENTER()) return;
    long p = (long)GCM_PENDING_TAKE();
    u64 user = GCM_USER_PENDING_TAKE();
    /* GCM_PUMP_DBG=1: how often each guest handler is actually delivered. */
    { static int d = -1; static unsigned nv, nf, calls;
      if (d < 0) d = getenv("GCM_PUMP_DBG") ? 1 : 0;
      if (d) { calls++; if (p & 1) nv++; if (p & 2) nf++;
               if ((calls % 20000) == 0)
                   fprintf(stderr, "[gcm-pump] calls=%u vblank=%u flip=%u vcount=%u%c",
                           calls, nv, nf, s_vblank_count, 10); } }
    if ((p & 1) && s_vblank_handler_opd && g_ps3_guest_caller) {
        ydkj_restore_handler_opd(s_vblank_handler_opd, s_vblank_handler_code);
        /* RPCS3 RSXDisplayEngine post_vblank_event: the handler gets 1, not
         * the vblank count. GCM_VBLANK_ARG_COUNT=1 restores the count. */
        { static int cnt = -1;
          if (cnt < 0) cnt = getenv("GCM_VBLANK_ARG_COUNT") ? 1 : 0;
          g_ps3_guest_caller(s_vblank_handler_opd, cnt ? (uint64_t)s_vblank_count : 1u,
                             0, 0, 0, 0, 0, 0, 0); }
    }
    if (p & 2) {
        /* Delivery waits for a guest thread to reach an HLE call or a timer
         * sleep, so a newer flip may have been requested (status WAITING
         * again) since this completion was marked. Marking DONE then would
         * erase that request: the ticker only completes WAITING flips, so its
         * handler would never run. inFAMOUS 2 writes its next FIFO put from
         * the flip handler, and hung with get == put on exactly that. Leave
         * the newer flip WAITING for the next tick. */
        FLIP_LOCK();
        if (s_flip_tick_seq == s_flip_req_seq)
            s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
        FLIP_UNLOCK();
        if (s_flip_handler_opd && g_ps3_guest_caller) {
            ydkj_restore_handler_opd(s_flip_handler_opd, s_flip_handler_code);
            g_ps3_guest_caller(s_flip_handler_opd, 1, 0, 0, 0, 0, 0, 0, 0);
        }
    }
    if (user) {
        u32 cmd = (u32)user;
        if (s_user_handler_opd && g_ps3_guest_caller)
            g_ps3_guest_caller(s_user_handler_opd, (uint64_t)cmd, 0, 0, 0, 0, 0, 0, 0);
    }
    GCM_PUMP_LEAVE();
}

/* Back-compat: the old direct entry points now just mark a tick pending (so any
 * caller other than the ticker still works) -- delivery stays on the main thread. */
void cellGcmTickVBlank(void) { if (s_r2rsx) return; s_vblank_count++; GCM_PENDING_SET(1); }
/* Only deliver a flip completion when a flip is actually outstanding.
 *
 * Ticking unconditionally at 60 Hz fires the guest's flip handler even when
 * the guest never asked for a flip. A title whose handler touches the flip
 * status then livelocks: Tokyo Jungle's main loop is
 *
 *     do { usleep(100); } while (cellGcmGetFlipStatus() != DONE);
 *     cellGcmResetFlipStatus();
 *
 * and its handler runs code that puts the status back to WAITING, so the
 * poll never saw DONE and the game never rendered another frame. On hardware
 * the callback IS the flip interrupt -- no flip, no callback. */
/* RPCS3 handle_emu_flip with "Frame limit: Auto" (Vblank Rate 60): a flip
 * completes as soon as the RSX has executed it, but no sooner than one frame
 * period after the previous completion; a late flip completes at once and
 * restarts the cadence. Completing only on the 60 Hz beat instead made frame
 * times alternate between one and two periods whenever the drain reached the
 * flip just after a beat. GCM_FLIP_PACE=0 restores the beat. */
static int gcm_flip_pace_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GCM_FLIP_PACE"); on = e ? atoi(e) : 1; }
    return on;
}
static long long s_flip_pace_target_us = 0;
static volatile int s_flip_pace_host = 0;   /* the host calls the poll */
int cellGcm_flip_pace_poll(long long now_us, long long period_us)
{
    if (s_r2rsx || !gcm_flip_pace_on()) return 0;
    s_flip_pace_host = 1;
    rsx_draw_engine_wcb_service();
    if (s_flip_status != CELL_GCM_FLIP_STATUS_WAITING) return 0;
    if (now_us < s_flip_pace_target_us - 1000) return 0;
    FLIP_LOCK();
    const int done = s_flip_status == CELL_GCM_FLIP_STATUS_WAITING && gcm_flip_executed_take();
    if (done) s_flip_tick_seq = s_flip_req_seq;
    FLIP_UNLOCK();
    if (!done) return 0;
    s_flip_pace_target_us = (now_us > s_flip_pace_target_us ? now_us : s_flip_pace_target_us) + period_us;
    GCM_PENDING_SET(2);
    return 1;
}

void cellGcmTickFlip(void)
{
    if (s_r2rsx) return;
    if (s_flip_pace_host) return;            /* cellGcm_flip_pace_poll completes flips */
    /* Same rule as cellGcm_request_tick: a flip still ahead of the drain has
     * not happened yet. */
    FLIP_LOCK();
    const int waiting = s_flip_status == CELL_GCM_FLIP_STATUS_WAITING && gcm_flip_executed_take();
    if (waiting) s_flip_tick_seq = s_flip_req_seq;
    FLIP_UNLOCK();
    if (waiting) GCM_PENDING_SET(2);
}

/* Drain the game's GCM FIFO into the RSX backend. Called from the present thread
 * (boot_main vblank_ticker). Not yet active: s_control.put stays 0 because
 * cellGcmGetControlRegister returns a HOST pointer, so the title's put writes
 * don't reach this struct (the control register must live in guest memory for
 * the recompiled code to update it via vm_write). Once that's fixed (+ the title
 * runs past its early self-exit to actually draw), parse get..put here with
 * rsx_process_command_buffer so the game's clears/draws render. */
/* Translate an RSX IO offset to a guest EA via the offset table (1MB pages,
 * populated by cellGcmInit / cellGcmMapMainMemory / MapEaIoAddress). */
static u32 gcm_io2ea(u32 io)
{
    u32 page = io >> 20;
    if (page >= 65536) return 0;
    u16 ea_page = s_ea_address_table[page];
    if (ea_page == 0xFFFF) return 0;
    return ((u32)ea_page << 20) | (io & 0xFFFFFu);
}

/* For the RSX method layer: semaphores aimed at main memory carry IO offsets. */
u32 gcm_io_offset_to_ea(u32 io) { return gcm_io2ea(io); }

/* RSX get pointer (IO offset) + one-deep CALL return slot. The FIFO-wrap
 * recycle path teleports these when it resets a ring without a JUMP command. */
/* Milliseconds on a monotonic clock, for the runtime's own timeouts and rate
 * logs. Off Windows this returned 0, so every interval measured with it was
 * 0 there: rate logs never printed and timeouts never expired. */
unsigned long long ps3_ms_now(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

static u32 s_sema_offset = 0;   /* NV406E semaphore offset (label window) */
static int s_sema_blocked_pass = 0;   /* this drain stopped on a semaphore acquire */
static int s_park_pass = 0;           /* this drain stopped on a self-JUMP park */
/* Backlog past which the drain stops honouring one-flip-per-tick and
 * catches up instead. Sized well above a frame's command list so a
 * title that keeps up never sees this path. */
#define GCM_FIFO_CATCHUP_BYTES 0x10000u
/* How long a self-JUMP park may stand with `put` somewhere else before the
 * drain gives up on it being patched and follows `put` instead. */
#define GCM_PARK_GRACE_MS 50ull
/* How close `current` must get to `end` before the runtime will recycle a ring
 * on the title's behalf. One gcmReserve is 8 bytes; a page of slack keeps a
 * title that is merely near the end from being touched. */
#define GCM_RECYCLE_SLACK 0x1000u
/* How far past the ring's start the walker must have read before the runtime
 * will recycle on the title's behalf. The guest resumes writing at `begin`, so
 * this is the amount of already-consumed head room it gets. */
#define GCM_RECYCLE_MARGIN 0x20000u
static u32 gcm_ea2io(u32 ea);   /* defined with the wrap callback below */
static u32 s_fifo_getoff  = 0;
/* Backward JUMPs the walker has taken. The ring recycle waits for this to move:
 * it is the walker following the JUMP-to-begin the recycle wrote, i.e. having
 * consumed the whole tail. g_gcm_fifo_drained_ea == begin cannot say that on its
 * own, because it is also begin before the walker has moved at all. */
static volatile u32 s_fifo_back_jumps = 0;
/* Set while the title's buffer-full callback (cellGcm_fifo_recycle) is waiting
 * for the walker. Its context still reads current == end then, which is exactly
 * what the drain's own overrun recycle looks for; firing that under the
 * callback teleports get to begin and drops the tail the callback is waiting on. */
static volatile int s_fifo_recycle_waiting = 0;

/* Bytes the walker is behind the writer. Linear while put >= get. Once the
 * title's ring wraps, put sits BEHIND get and the backlog is the rest of the
 * ring plus everything from its start to put -- nearly the whole ring, not
 * nothing. Only computed when both pointers lie inside the title's own ring;
 * any other layout (segments linked by JUMPs) reports 0, as before. */
static u32 gcm_fifo_backlog(u32 put, u32 get)
{
    if (put >= get) return put - get;
    if (!s_gcm_context_ea) return 0;
    u32 b = gcm_ea2io(vm_read32(s_gcm_context_ea + 0x0));
    u32 e = gcm_ea2io(vm_read32(s_gcm_context_ea + 0x4));
    if (b == 0xFFFFFFFFu || e == 0xFFFFFFFFu || e <= b) return 0;
    if (get < b || get > e || put < b || put > e) return 0;
    return (e - get) + (put - b);
}
static u32 s_last_ring_off = 0;   /* last offset the walker actually consumed */
/* RPCS3 RSX_CALL_STACK_EMPTY (RSXFIFO.h): no CALL outstanding. 3 is not a
 * valid (4-aligned) return offset, so it cannot collide with one. */
#define GCM_CALL_NONE 3u
static u32 s_fifo_calloff = GCM_CALL_NONE;
/* RPCS3 recover_fifo state (RSXThread.cpp:886-891, 1740-1780): the restore
 * point is the command `get` stood on when a sync point was last requested
 * outside a BEGIN/END, and the CALL return that was live there. */
static u32 s_fifo_restore = 0xFFFFFFFFu;
static u32 s_fifo_saved_ret = GCM_CALL_NONE;
static int s_fifo_sync_req = 1;            /* RSXThread.cpp:1316, set at init */
static u32 s_sema406e_ctx = 0x66616661u;   /* rsx_methods.cpp init: NV406E 0x60 */
static u32 s_sema4097_off = 0;
static const char* s_fifo_rec_why = "";
/* GCM_FIFO_RECOVER=0: no recovery -- CALL-in-CALL overwrites the return, RET
 * with no CALL returns to 0, malformed words are skipped (the old walker). */
static int gcm_fifo_recover_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GCM_FIFO_RECOVER"); on = !(e && atoi(e) == 0); }
    return on;
}
/* Per-method desync checks the walker applies before dispatch (subchannel 0 /
 * 3D methods). 1 = recover_fifo instead of executing the word. */
static int gcm_walk_check(u32 m, u32 v)
{
    if (m == 0x0060u) { s_sema406e_ctx = v; return 0; }
    if (m == 0x1D6Cu) { s_sema4097_off = v; return 0; }
    if (m != 0x1D70u && m != 0x1D74u && m != 0x1820u) return 0;
    if (!gcm_fifo_recover_on()) return 0;
    /* nv4097.cpp:792-797 / 822-827: unaligned NV4097 semaphore -> recover. */
    if (m != 0x1820u && (s_sema4097_off & 15u)) { s_fifo_rec_why = "unaligned NV4097 semaphore"; return 1; }
    /* nv4097.cpp:468-479 check_index_array_dma: location/type invalid -> recover. */
    if (m == 0x1820u && (v & ~0x11u)) { s_fifo_rec_why = "invalid INDEX_ARRAY_DMA"; return 1; }
    return 0;
}

/* RPCS3 rsx_methods.cpp:1281-1773: every register not explicitly listed is
 * invalid_method -> recover_fifo. Bit per (subch << 13 | method) >> 2.
 * Generated from the RPCS3 table. */
static const u32 s_rsx_valid_methods[512] = {
    0x0F100001u, 0x00000000u, 0x00010013u, 0x00007FFFu, 0x000FFFFFu, 0xFFFFC00Fu, 0xFFFFFFFFu, 0xFFFFD067u,
    0xFFFFFFFFu, 0x0000FFFFu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x027B0000u, 0xFFFFFFFFu, 0x00000000u, 0xFF00FF8Bu, 0xFFFFFFFFu, 0x03FFFFFFu, 0xFFFFFFFFu,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0xC0B04C00u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0x0000FFFFu, 0xFFFFC0F0u, 0x000C0000u,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFB00FFFFu, 0x00007AFFu, 0xFFFF0000u, 0xC7000FE0u, 0xFFFFFFFFu, 0x7D7F0000u,
    0x00000001u, 0x00000000u, 0x00000001u, 0x00000007u, 0x00000000u, 0x00000000u, 0x000007F8u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000001u, 0x00000000u, 0x00000000u, 0x00000007u, 0x00000000u, 0x00000000u, 0x0000000Fu, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000001u, 0x00000000u, 0x00000000u, 0x00000003u, 0x00000000u, 0x00000000u, 0x00000003u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000001u, 0x00000000u, 0x00000000u, 0x000000FFu, 0x00000000u, 0xC0000000u, 0x0000000Fu, 0x00000000u,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0x00000001u, 0x00000000u, 0x00000000u, 0x0000007Fu, 0x00000000u, 0x80000000u, 0x000000FFu, 0x00000000u,
    0x0000000Fu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00FF0300u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000003u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x40000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000800u, 0x00000000u, 0x00000000u,
};
/* GCM_INVALID_METHOD_RECOVER=0: unlisted methods are stored/ignored as before. */
static int gcm_invalid_method_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GCM_INVALID_METHOD_RECOVER"); on = !(e && atoi(e) == 0); }
    return on && gcm_fifo_recover_on();
}
static unsigned long s_invalid_methods;
/* reg: the full 16-bit method, subchannel included. */
static int gcm_method_invalid(u32 reg, u32 v)
{
    reg &= 0xFFFCu;
    if (s_rsx_valid_methods[reg >> 7] & (1u << ((reg >> 2) & 31u))) return 0;
    if (!gcm_invalid_method_on()) return 0;
    s_invalid_methods++;
    { static int n = 0; if (PS3_LOG_FIRST(n, 16) || (s_invalid_methods & 1023u) == 0)
        fprintf(stderr, "[cellGcmSys] Invalid RSX method 0x%04X (arg=0x%08X) at get 0x%08X (#%lu)\n",
                reg, v, s_fifo_getoff, s_invalid_methods); }
    s_fifo_rec_why = "invalid method";
    return 1;
}
/* ---------------------------------------------------------------------------
 * 2D transfer engines (FIFO subchannels != 0).
 *
 * libgcm binds: sub 1/6 = NV0039 (m2mf buffer copy), sub 2 = NV3062 (context
 * surface 2D), sub 3 = NV309E (swizzled surface), sub 4 = NV308A (image from
 * CPU / inline transfer), sub 5 = NV3089 (scaled image).
 *
 * cellGcmSetFragmentProgramParameter patches a fragment program's inline
 * constants by emitting NV3062 (destination surface) + NV308A (COLOR data
 * words) through the FIFO -- it does NOT CPU-write the ucode copy. Without
 * this engine the patched constants stay zero in local memory (demosaic: all
 * its texture-size constants -> UVs scaled by 0 -> uniform output).
 * -----------------------------------------------------------------------*/
extern u32 cellGcmResolveLocated(int local, u32 offset);

/* RPCS3 Utils/rsx_utils.cpp:278-298 get_address: the two report DMA handles.
 * REPORT_LOCATION_MAIN (0xBAD68000) is IO 0x0E000000 + off (off < 16MB);
 * REPORT_LOCATION_LOCAL (0x66626660) is the report area the
 * cellGcmGetReportData* calls hand out. Returns 1 for a report handle, with
 * *ea = 0 when the offset is out of range. Shared with gcm_zc_sink. */
static int gcm_report_dma_ea(u32 dma, u32 off, u32* ea)
{
    switch (dma) {
    case 0xBAD68000u: *ea = off < 0x1000000u ? gcm_io2ea(0x0E000000u + off) : 0; return 1;
    case 0x66626660u: *ea = off < 0x100000u ? GCM_REPORT_GUEST_BASE + off : 0; return 1;
    default:          return 0;
    }
}

/* A 2D-engine (NV0039 / NV3062 / NV3089 / NV309E) DMA handle + offset -> guest
 * EA (RPCS3 rsx_utils.cpp:224 get_address). 0xFEED0001 is main memory (IO
 * offset), the report handles as above, anything else local memory. */
static u32 gcm_dma_ea(u32 dma, u32 off)
{
    u32 ea;
    if (gcm_report_dma_ea(dma, off, &ea)) return ea;
    return cellGcmResolveLocated(dma != 0xFEED0001u, off);
}

static struct {
    u32 src_dma;      /* NV3062 0x0184 SET_CONTEXT_DMA_IMAGE_SOURCE */
    u32 dst_dma;      /* NV3062 0x0188 SET_CONTEXT_DMA_IMAGE_DESTIN */
    u32 color_fmt;    /* NV3062 0x0300 SET_COLOR_FORMAT             */
    u32 pitch;        /* NV3062 0x0304 SET_PITCH (dst in bits 16-31) */
    u32 dst_offset;   /* NV3062 0x030C SET_OFFSET_DESTIN            */
    u32 point;        /* NV308A 0x0304 SET_POINT ((y<<16)|x)        */
    u32 size_out;     /* NV308A 0x0308 SET_SIZE_OUT                 */
} s_gcm2d = { 0xFEED0000u, 0xFEED0000u, 0xAu, 0x400040u };   /* rsx_methods.cpp:533-534, 659-660 init */

static struct {
    u32 src_dma, dst_surf, fmt, op, clip_pt, clip_sz, out_pt, out_sz;
    u32 ds_dx, dt_dy, in_sz, in_fmt, in_off, in_uv;
    int op_written;   /* SET_OPERATION seen: until then SRCCOPY is assumed */
    int flip_x, flip_y; /* negative DS_DX / DT_DY of the blit in flight (ds_dx /
                         * dt_dy then hold the magnitudes) */
} s_nv3089;

/* NV309E "swizzled surface" -- the destination object for a scaled-image blit
 * whose target is a swizzled (Morton-ordered) texture rather than a linear
 * surface. Rubber Ducky binds it to the same subchannel this file previously
 * assumed was always NV308A (image-from-CPU), so its SET_OFFSET was being read
 * as an NV308A "point" and the blit destination was never picked up at all.
 * The two are told apart by method 0x0300: NV308A has no such method. */
static struct {
    u32 fmt;        /* 0x0300: (log2h << 24) | (log2w << 16) | format */
    u32 offset;     /* 0x0304: destination offset                     */
    int active;
    int subch;      /* which subchannel NV309E is bound to. Per-subchannel:
                     * this title has NV309E on 4 and NV308A on 5, and a global
                     * flag made 5's NV308A POINT (also 0x0304) be swallowed as
                     * an NV309E offset, breaking every inline transfer. */
    u32 dma;        /* 0x0184: SET_CONTEXT_DMA_IMAGE (0 = not set: local) */
} s_nv309e;

/* Execute one NV3089 blit into the currently-bound NV3062 destination surface.
 * ds/dx and dt/dy are 20-bit fixed point (0x100000 = 1.0); the CPU copy
 * samples texel centres, bilinear under the FOH interpolator (nv3089_tap_at). */
/* Morton/Z-order offset for an NV309E swizzled surface: interleave the low
 * min(l2w,l2h) bits of x and y, then append the remaining bits of the larger
 * dimension above them. */
static u32 nv309e_swz(u32 x, u32 y, u32 l2w, u32 l2h)
{
    u32 n = l2w < l2h ? l2w : l2h, off = 0;
    for (u32 i = 0; i < n; i++)
        off |= (((x >> i) & 1u) << (2 * i)) | (((y >> i) & 1u) << (2 * i + 1));
    if (l2w > l2h)      off |= (x >> n) << (2 * n);
    else if (l2h > l2w) off |= (y >> n) << (2 * n);
    return off;
}

/* IMAGE_IN_POINT is 12.4 fixed point per axis; DS_DX / DT_DY step the source in 20 fractional bits (1.0 = 0x100000).
 * The origin in the steps' scale is therefore origin << 16: an origin of one texel (0x10) is 0x100000. */
static inline u64 nv3089_origin(u32 p) { return (u64)p << 16; }

/* RPCS3 nv3089.cpp: the destination is the NV309E swizzled surface when
 * SET_CONTEXT_SURFACE (0x0198) names the swizzle-2D context object, the NV3062
 * linear surface when it names the surface-2D one. Titles whose object handles
 * differ fall back to the old guess: a swizzled object is bound with a format
 * whose log2 size matches the output. */
#define NV3089_CTX_SURFACE2D 0x313371C3u
#define NV3089_CTX_SWIZZLE2D 0x31337A73u
static int nv3089_dst_swizzled(u32 out_w, u32 out_h)
{
    if (!s_nv309e.active) return 0;
    if (s_nv3089.dst_surf == NV3089_CTX_SWIZZLE2D) return 1;
    if (s_nv3089.dst_surf == NV3089_CTX_SURFACE2D) return 0;
    const u32 l2w = (s_nv309e.fmt >> 16) & 0xFF, l2h = (s_nv309e.fmt >> 24) & 0xFF;
    /* An NV309E surface at offset 0 is still swizzled (D7): gate on `active`. */
    return (s_nv3089.fmt & 0xFF) == 3 && l2w < 32 && l2h < 32 &&
           (1u << l2w) == out_w && (1u << l2h) == out_h;
}

/* RPCS3 nv3089.cpp decode_transfer_registers + clip_image: the destination is
 * the clip SIZE placed at the output POINT (clip size clamped to the output
 * size; zero is a NOP), and the clip POINT selects where in the scaled image
 * that rectangle starts -- reset to 0 when it would run past the output size.
 * Destination pixel (x0 + i, y0 + j) shows scaled-image texel (ix0 + i,
 * iy0 + j). `legacy` is the old absolute-destination clip: the clip rectangle
 * intersected with the output rectangle, both in destination coordinates. */
typedef struct { u32 x0, y0, w, h, ix0, iy0; } nv3089_geom;

static int nv3089_geom_calc(u32 out_pt, u32 out_sz, u32 clip_pt, u32 clip_sz, int legacy,
                            nv3089_geom* g)
{
    const u32 out_x = out_pt & 0xFFFF, out_y = out_pt >> 16;
    const u32 out_w = out_sz & 0xFFFF, out_h = out_sz >> 16;
    u32 clip_x = clip_pt & 0xFFFF, clip_y = clip_pt >> 16;
    u32 clip_w = clip_sz & 0xFFFF, clip_h = clip_sz >> 16;
    if (legacy) {
        const u32 cx1 = clip_w ? clip_x + clip_w : 0xFFFFFFFFu;
        const u32 cy1 = clip_h ? clip_y + clip_h : 0xFFFFFFFFu;
        const u32 x0 = out_x > clip_x ? out_x : clip_x, y0 = out_y > clip_y ? out_y : clip_y;
        const u32 x1 = out_x + out_w < cx1 ? out_x + out_w : cx1;
        const u32 y1 = out_y + out_h < cy1 ? out_y + out_h : cy1;
        if (x1 <= x0 || y1 <= y0) return 0;
        g->x0 = x0; g->y0 = y0; g->w = x1 - x0; g->h = y1 - y0;
        g->ix0 = x0 - out_x; g->iy0 = y0 - out_y;
        return 1;
    }
    if (clip_w > out_w) clip_w = out_w;
    if (clip_h > out_h) clip_h = out_h;
    if (!clip_w || !clip_h) return 0;
    if (clip_x && out_x + clip_x + clip_w > out_w) clip_x = 0;
    if (clip_y && out_y + clip_y + clip_h > out_h) clip_y = 0;
    g->x0 = out_x; g->y0 = out_y; g->w = clip_w; g->h = clip_h;
    g->ix0 = clip_x; g->iy0 = clip_y;
    return 1;
}

static int nv3089_dstclip_legacy(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("NV3089_DSTCLIP"); v = e && e[0] == '1'; }
    return v;
}

/* Big-endian guest pixels. R5G6B5 -> A8R8G8B8 widens by bit replication and
 * sets alpha opaque; A8R8G8B8 -> R5G6B5 truncates (swscale's conversions in
 * RPCS3's CPU blit path). */
static u32 nv3089_r5g6b5_to_argb(u16 c)
{
    const u32 r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

static u16 nv3089_argb_to_r5g6b5(u32 c)
{
    return (u16)((((c >> 19) & 31u) << 11) | (((c >> 10) & 63u) << 5) | ((c >> 3) & 31u));
}

static u32 nv3089_next_pow2(u32 v)
{
    u32 p = 1;
    while (p < v && p < 0x80000000u) p <<= 1;
    return p;
}

/* CPU scaled-copy sampling, one axis. RPCS3 Utils/rsx_utils.cpp:34
 * convert_scale_image runs swscale with SWS_FAST_BILINEAR when the
 * interpolator is FOH (nv3089.cpp:616) and SWS_POINT otherwise; both sample at
 * texel centres, as the GPU path's vkCmdBlitImage does. Output texel i of a
 * 20-bit fixed-point step ds reads image texels t0/t1 (relative to the image
 * origin), t1 weighted w/256:
 *   FOH: bilinear at (i+0.5)*ds - 0.5, the neighbour clamped to the n-texel
 *        input image;
 *   ZOH: nearest, floor((i+0.5)*ds).
 * At ds == 1.0 both give t0 = i, w = 0 (bit-exact 1:1 copies). */
typedef struct { u32 t0, t1, w; } nv3089_tap;

static nv3089_tap nv3089_tap_at(u32 i, u32 ds, u32 n, int foh)
{
    nv3089_tap r;
    const u64 c = ((2ull * i + 1ull) * ds) >> 1;          /* (i+0.5)*ds */
    if (!foh) {
        /* SWS_POINT never reads past the image: clamp to n-1 (NV3089_ZOH_CLAMP=0 off). */
        static int zclamp = -1;
        if (zclamp < 0) { const char* e = getenv("NV3089_ZOH_CLAMP"); zclamp = !(e && atoi(e) == 0); }
        r.t0 = r.t1 = (u32)(c >> 20);
        if (zclamp && n && r.t0 >= n) r.t0 = r.t1 = n - 1u;
        r.w = 0; return r;
    }
    const u64 p = c > 0x80000u ? c - 0x80000u : 0;        /* - 0.5, clamped at 0 */
    const u32 t = (u32)(p >> 20);
    r.w = (u32)(p >> 12) & 0xFFu;
    r.t0 = r.t1 = t;
    if (r.w) {
        if (n && t + 1u < n) r.t1 = t + 1u;
        else { if (n && t >= n) r.t0 = r.t1 = n - 1u; r.w = 0; }
    }
    return r;
}

/* Image texel t from origin o; mirrored (RPCS3 nv3089.cpp:535 _mirror_transform,
 * applied at 590-596 for a negative scale) it runs backwards from o. */
static inline u32 nv3089_src_coord(u32 o, u32 t, int flip)
{
    return flip ? (t > o ? 0u : o - t) : o + t;
}

static inline int nv3089_foh(void) { return ((s_nv3089.in_fmt >> 24) & 0xFFu) == 1u; }

/* Source texel span [lo, hi] output texels i0..i0+cnt-1 read on one axis. */
static void nv3089_src_span(u32 o, u32 i0, u32 cnt, u32 ds, u32 n, int foh, int flip, u32* lo, u32* hi)
{
    const nv3089_tap a = nv3089_tap_at(i0, ds, n, foh), b = nv3089_tap_at(i0 + cnt - 1u, ds, n, foh);
    const u32 s0 = nv3089_src_coord(o, a.t0, flip), s1 = nv3089_src_coord(o, b.t1, flip);
    *lo = s0 < s1 ? s0 : s1;
    *hi = s0 < s1 ? s1 : s0;
}

/* One guest texel (big-endian A8R8G8B8, or R5G6B5 widened) as A8R8G8B8. */
static inline u32 nv3089_px(u32 ea, u32 sbpp)
{
    extern u8* vm_base;
    const u8* p = vm_base + ea;
    if (sbpp == 2) return nv3089_r5g6b5_to_argb((u16)(((u32)p[0] << 8) | p[1]));
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* Bilinear per 8-bit channel (R5G6B5 is unpacked to 8 bits first, and packed
 * again by the caller). With both weights zero this is the exact texel. */
static u32 nv3089_sample(u32 base, u32 pitch, u32 sbpp, u32 sx0, u32 sx1, u32 wx,
                         u32 sy0, u32 sy1, u32 wy)
{
    const u32 r0 = base + sy0 * pitch, r1 = base + sy1 * pitch;
    const u32 c00 = nv3089_px(r0 + sx0 * sbpp, sbpp);
    if (!wx && !wy) return c00;
    const u32 c10 = nv3089_px(r0 + sx1 * sbpp, sbpp);
    const u32 c01 = nv3089_px(r1 + sx0 * sbpp, sbpp);
    const u32 c11 = nv3089_px(r1 + sx1 * sbpp, sbpp);
    const u32 w00 = (256u - wx) * (256u - wy), w10 = wx * (256u - wy);
    const u32 w01 = (256u - wx) * wy, w11 = wx * wy;
    u32 out = 0;
    for (u32 sh = 0; sh < 32; sh += 8) {
        const u32 v = (((c00 >> sh) & 0xFFu) * w00 + ((c10 >> sh) & 0xFFu) * w10 +
                       ((c01 >> sh) & 0xFFu) * w01 + ((c11 >> sh) & 0xFFu) * w11 + 32768u) >> 16;
        out |= (v > 255u ? 255u : v) << sh;
    }
    return out;
}

/* The draw engine renders on the host GPU, so the scene a title copies out
 * with NV3089 is not in guest memory, and what it copies in never reaches the
 * engine's surfaces. Around the guest-memory copy below: write the source
 * rectangle back from any engine surface first, and push the written
 * rectangle into any engine surface after. 32-bit pixels only. */
static void nv3089_engine_read(u32 in_pitch, u32 ix0, u32 iy0, u32 w, u32 h)
{
    if (!rsx_draw_engine_enabled() || !w || !h) return;
    const u32 u0 = s_nv3089.in_uv & 0xFFFF, v0 = s_nv3089.in_uv >> 16;   /* 12.4 */
    const int foh = nv3089_foh();
    u32 sx0, sx1, sy0, sy1;
    nv3089_src_span((u32)(nv3089_origin(u0) >> 20), ix0, w, s_nv3089.ds_dx, s_nv3089.in_sz & 0xFFFF,
                    foh, s_nv3089.flip_x, &sx0, &sx1);
    nv3089_src_span((u32)(nv3089_origin(v0) >> 20), iy0, h, s_nv3089.dt_dy, s_nv3089.in_sz >> 16,
                    foh, s_nv3089.flip_y, &sy0, &sy1);
    const u32 loc = (s_nv3089.src_dma != 0xFEED0001u) ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN;
    rsx_draw_engine_guest_read(loc, s_nv3089.in_off + sy0 * in_pitch + sx0 * 4u, in_pitch,
                               sx1 - sx0 + 1u, sy1 - sy0 + 1u);
}

static void nv3089_engine_wrote(u32 dst_pitch, u32 x0, u32 y0, u32 x1, u32 y1)
{
    if (!rsx_draw_engine_enabled() || x1 <= x0 || y1 <= y0) return;
    const u32 loc = (s_gcm2d.dst_dma != 0xFEED0001u) ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN;
    rsx_draw_engine_guest_wrote(loc, s_gcm2d.dst_offset + y0 * dst_pitch + x0 * 4u, dst_pitch,
                                x1 - x0, y1 - y0);
}

/* A guest-memory row copy with the same reservation bookkeeping a coherent
 * PPU store does (ppu_loader.cpp VM_WRITE_COH), once per row instead of per
 * pixel: an SPU holding a reservation on a 128-byte line the row overwrites
 * loses it and gets its lost-reservation event. The two byte swaps a
 * vm_read32/vm_write32 pair does cancel, so the bytes move verbatim. */
static void gcm_row_copy(u32 d, u32 s, u32 n)
{
    extern u8* vm_base;
    const u32 l0 = d & ~127u, l1 = (d + n - 1) & ~127u;
    int reserved = 0;
    for (u32 a = l0; a <= l1 && !reserved; a += 128) reserved = spu_coh_is_reserved(a);
    if (!reserved) { memmove(vm_base + d, vm_base + s, n); return; }
    spu_lockline_lock();
    memmove(vm_base + d, vm_base + s, n);
    for (u32 a = l0; a <= l1; a += 128)
        if (spu_coh_is_reserved(a)) spu_coh_notify_write(a);
    spu_lockline_unlock();
}

volatile int g_port_trace_left = 0;

/* R2 patch to RPCS3 (texture_cache.h blit): a blit whose source scales down
 * to zero columns or rows -- (u16)(clip_w / scale_x), scale_x = 1/|DS_DX| --
 * is reported handled and copies nothing. RPCS3 only reaches that path when
 * the source or destination is local memory (nv3089.cpp image_in).
 * RSX_R2PATCH_LEGACY=1 copies them as before. */
static int nv3089_degenerate_drop(u32 out_w, u32 out_h)
{
    static int legacy = -1;
    if (legacy < 0) { const char* e = getenv("RSX_R2PATCH_LEGACY"); legacy = e && e[0] && e[0] != '0'; }
    if (legacy) return 0;
    if (s_nv3089.src_dma == 0xFEED0001u && s_gcm2d.dst_dma == 0xFEED0001u) return 0;
    u32 clip_w = s_nv3089.clip_sz & 0xFFFF, clip_h = s_nv3089.clip_sz >> 16;
    if (!clip_w || clip_w > out_w) clip_w = out_w;
    if (!clip_h || clip_h > out_h) clip_h = out_h;
    const s32 dx = (s32)s_nv3089.ds_dx, dy = (s32)s_nv3089.dt_dy;
    const u64 adx = dx < 0 ? (u64)(-(s64)dx) : (u64)dx, ady = dy < 0 ? (u64)(-(s64)dy) : (u64)dy;
    if (!adx || !ady) return 0;
    const u16 src_w = (u16)(((u64)clip_w * adx) >> 20), src_h = (u16)(((u64)clip_h * ady) >> 20);
    return !src_w || !src_h;
}

/* RPCS3 Common/tiled_dma_copy.hpp tiled_dma_copy (encode): the tiled address
 * of linear byte (row, col) of an image at base_offset into a tile region. */
static u32 gcm_tile_address(u32 row, u32 col, u32 bpp, u32 base, u32 base_off, u32 pitch,
                            u32 bank, u32 tiles_per_row, u32 factor)
{
    static const u32 lut[16] = { 0, 1, 2, 3, 2, 3, 0, 1, 1, 2, 3, 0, 3, 0, 1, 2 };
    const u32 this_address = row * pitch + base + base_off + col * bpp;
    const u32 texel_offset = (this_address - base) / 256u;
    const u32 tile_x = texel_offset % tiles_per_row;
    const u32 tile_y = (texel_offset / tiles_per_row) / 64u;
    const u32 tile_id = tile_y * tiles_per_row + tile_x;
    const u32 tile_selector = (tile_id + (base >> 14)) & 0x3ffffu;
    const u32 row_address = (tile_selector >> 2) & 0xffffu;
    u32 bank_selector = 0;
    if (factor == 1) bank_selector = tile_selector & 3u;
    else if (factor == 2) bank_selector = lut[((tile_selector + ((tile_y & 1u) << 1)) & 3u) * 4u + (tile_y & 3u)];
    else if (factor >= 4) bank_selector = lut[(tile_selector & 3u) * 4u + (tile_y & 3u)];
    bank_selector = (bank_selector + bank) & 3u;
    const u32 line = (texel_offset / tiles_per_row) % 64u;
    u32 column_selector = ((line >> 3) & 7u) << 7;
    column_selector |= ((this_address >> 5) & 7u) << 4;
    column_selector |= (line & 3u) << 2;
    const u32 partition = (((line >> 2) & 1u) + ((this_address >> 6) & 1u)) & 1u;
    u32 a = (row_address & 0xFFFFu) << 16;
    a |= (bank_selector & 3u) << 14;
    a |= ((column_selector >> 4) & 0x3Fu) << 8;
    a |= (partition & 1u) << 7;
    a |= ((column_selector >> 2) & 3u) << 5;
    a |= this_address & 0x1Fu;
    a ^= (((a >> 12) ^ ((bank_selector ^ tile_selector) & 1u) ^ (a >> 14)) & 1u) << 9;
    a ^= ((a >> 11) & 1u) << 10;
    return a;
}

/* RPCS3 tile_texel_data<T, false>: lin (image_pitch = tile pitch) -> tiled
 * bytes written relative to dst_ea. */
static void gcm_tile_texel_data(u32 dst_ea, const u8* lin, u32 bpp, u32 base, u32 base_off,
                                u32 size, u32 bank, u32 pitch, u32 w, u32 h)
{
    const u32 b = pitch >> 8;
    u32 prime = 0, factor = 0;
    if ((pitch & (pitch - 1u)) == 0) { prime = 1; factor = b; }
    else {
        static const u32 primes[5] = { 3, 5, 7, 11, 13 };
        for (int k = 0; k < 5; k++) if (b % primes[k] == 0) { prime = primes[k]; factor = b / primes[k]; break; }
    }
    const u32 tpr = prime * factor;
    if (!tpr) return;
    extern u8* vm_base;
    for (u32 row = 0; row < h; row++)
        for (u32 col = 0; col < w; col++) {
            const u32 ta = gcm_tile_address(row, col, bpp, base, base_off, pitch, bank, tpr, factor);
            const u32 tbo = ta - base;
            if (tbo >= size) continue;
            memcpy(vm_base + dst_ea + (tbo - base_off), lin + row * pitch + col * bpp, bpp);
        }
}

/* RPCS3 fw_device_state::get_tiled_memory_region: only MAIN-memory tiles
 * (local tiling is transparent), and only with "Handle RSX Memory Tiling",
 * which RPCS3 ships off -- GCM_HANDLE_TILED_MEMORY=1 turns it on here. */
static int gcm_handle_tiled_memory(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GCM_HANDLE_TILED_MEMORY"); on = e && e[0] == '1'; }
    return on;
}

static const CellGcmTileInfo* gcm_tiled_region(u32 ea, u32 len, u32* base_ea)
{
    if (!gcm_handle_tiled_memory()) return NULL;
    for (u32 i = 0; i < CELL_GCM_MAX_TILE_COUNT; i++) {
        const CellGcmTileInfo* t = &s_tiles[i];
        if (!t->bound || (t->tile & 1u) != 1u || !t->size) continue;
        const u32 tb = gcm_io2ea(t->offset);
        if (!tb) continue;
        if (ea >= tb && ea - tb < t->size && (u64)ea + len > tb) { *base_ea = tb; return t; }
    }
    return NULL;
}

static void nv3089_blit(void)
{
    u32 out_w = s_nv3089.out_sz & 0xFFFF, out_h = s_nv3089.out_sz >> 16;
    u32 out_x = s_nv3089.out_pt & 0xFFFF, out_y = s_nv3089.out_pt >> 16;
    u32 in_pitch = s_nv3089.in_fmt & 0xFFFF;
    u32 dst_pitch = s_gcm2d.pitch >> 16;
    /* RPCS3 nv3089.cpp: a zero pitch means tightly packed rows. */
    if (!in_pitch)
        in_pitch = ((s_nv3089.fmt & 0xFF) == 7 ? 2u : 4u) * (s_nv3089.in_sz & 0xFFFF);
    if (!dst_pitch)
        dst_pitch = (s_gcm2d.color_fmt == 4u ? 2u : s_gcm2d.color_fmt == 1u ? 1u : 4u) * out_w;
    if (!out_w || !out_h || !in_pitch || !dst_pitch) return;
    if (nv3089_degenerate_drop(out_w, out_h)) return;
    /* A mirrored blit (negative DS_DX / DT_DY) stays on the CPU path, which
     * applies RPCS3's _mirror_transform; the GPU paths below take no flip. */
    const int mir = s_nv3089.flip_x || s_nv3089.flip_y;
    /* RPCS3 has one path per blit (scaled_image_from_memory or the CPU copy):
     * with the draw engine on, the legacy backend hooks below stay out.
     * GCM_NV3089_EXCLUSIVE=0 runs them as well. */
    static int excl = -1;
    if (excl < 0) { const char* e = getenv("GCM_NV3089_EXCLUSIVE"); excl = !(e && atoi(e) == 0); }
    const int legacy_hooks = !(excl && rsx_draw_engine_enabled());
#ifdef _WIN32
    /* Resistance port: a scaled blit INTO a display buffer is the frame's
     * resolve (the game renders the scene into an anti-aliased surface at
     * 0x740000 and scales it onto the display with NV3089). The scene only
     * exists on the GPU, so the guest-memory copy below moves black; have the
     * backend composite that surface onto the backbuffer instead.
     * NV3089_COMPOSITE=0 disables. */
    { static int en = -1;
      if (en < 0) { const char* e = getenv("NV3089_COMPOSITE"); en = e ? atoi(e) : 1; }
      extern int cellGcmOffsetIsDisplay(u32 offset);
      extern void rsx_d3d12_composite_from_offscreen(u32 raw_off);
      if (legacy_hooks && en && cellGcmOffsetIsDisplay(s_gcm2d.dst_offset)) {
          static int n = 0;
          if (n < 6) { n++;
              printf("[NV3089] display resolve: src off=0x%08X %ux%u -> dst 0x%08X %ux%u%c",
                     s_nv3089.in_off, s_nv3089.in_sz & 0xFFFF, s_nv3089.in_sz >> 16,
                     s_gcm2d.dst_offset, out_w, out_h, 10); }
          rsx_d3d12_composite_from_offscreen(s_nv3089.in_off);
      } }
#endif
#ifdef _WIN32
    /* v189: a copy out of a surface the D3D12 backend renders (the scene) is
     * done on the GPU too -- the guest-memory copy below moves bytes the RSX
     * never wrote there (the Auger's through-wall refraction read those: black). */
    { extern int cellGcmOffsetIsDisplay(u32 offset);
      extern int rsx_d3d12_blit_from_surface(u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32);
      const u32 ff = s_nv3089.fmt & 0xFF;
      const int sl = (s_nv3089.src_dma != 0xFEED0001u), dl = (s_gcm2d.dst_dma != 0xFEED0001u);
      const int swz = nv3089_dst_swizzled(out_w, out_h);
      if (legacy_hooks && !mir && sl && dl && !swz && (ff == 3 || ff == 4 || ff == 7) &&
          !cellGcmOffsetIsDisplay(s_gcm2d.dst_offset))
          rsx_d3d12_blit_from_surface(s_nv3089.in_off, s_nv3089.in_sz & 0xFFFF, s_nv3089.in_sz >> 16, in_pitch,
                                      s_nv3089.in_uv, s_nv3089.ds_dx, s_nv3089.dt_dy,
                                      s_gcm2d.dst_offset, dst_pitch, out_x, out_y, out_w, out_h); }
#endif
    /* NV3089_GATE=<file>: log every blit's geometry while <file> exists, so a
     * capture covers the scene of interest instead of the first N since boot. */
    { static const char* gate = (const char*)-1; static unsigned poll; static int open_gate;
      if (gate == (const char*)-1) gate = getenv("NV3089_GATE");
      if (gate) {
          if ((poll++ & 63u) == 0) { FILE* g = fopen(gate, "rb"); open_gate = g != NULL; if (g) fclose(g); }
          if (open_gate) {
              int _sl = (s_nv3089.src_dma != 0xFEED0001u), _dl = (s_gcm2d.dst_dma != 0xFEED0001u);
              printf("[NV3089G] fmt=0x%X out=%ux%u at %u,%u in=%ux%u uv=0x%08X ds=0x%X dt=0x%X src=0x%08X/%u dst=0x%08X/%u swz=%d%c",
                     s_nv3089.fmt & 0xFF, out_w, out_h, out_x, out_y,
                     s_nv3089.in_sz & 0xFFFF, s_nv3089.in_sz >> 16, s_nv3089.in_uv,
                     s_nv3089.ds_dx, s_nv3089.dt_dy,
                     cellGcmResolveLocated(_sl, s_nv3089.in_off), in_pitch,
                     cellGcmResolveLocated(_dl, s_gcm2d.dst_offset), dst_pitch,
                     s_nv309e.active, 10);
          }
      } }
    /* A copy out of a surface the live renderer drew exists only on the GPU;
     * mirror 1:1 blits there too (the guest-memory copy below still runs). */
    if (legacy_hooks && !mir && s_nv3089.ds_dx == 0x100000u && s_nv3089.dt_dy == 0x100000u &&
        rsx_live_draw_enabled()) {
        const u32 u0 = (s_nv3089.in_uv & 0xFFFF) >> 4, v0 = (s_nv3089.in_uv >> 16) >> 4;
        const int sl = (s_nv3089.src_dma != 0xFEED0001u);
        const int dl = (s_gcm2d.dst_dma  != 0xFEED0001u);
        rsx_live_draw_blit(sl ? 0u : 1u, s_nv3089.in_off + v0 * in_pitch + u0 * 4u, in_pitch,
                           dl ? 0u : 1u, s_gcm2d.dst_offset + (s_nv3089.out_pt >> 16) * dst_pitch
                                         + (s_nv3089.out_pt & 0xFFFF) * 4u, dst_pitch,
                           out_w, out_h);
    } else if (legacy_hooks && !mir && rsx_live_draw_enabled()) {
        /* Scaled: a resolve. Copy it on the GPU into the display buffer; fall
         * back to presenting the source when that cannot be done. */
        const u32 sl = s_nv3089.src_dma != 0xFEED0001u ? 0u : 1u;
        const u32 dl = s_gcm2d.dst_dma  != 0xFEED0001u ? 0u : 1u;
        const u32 u0 = (s_nv3089.in_uv & 0xFFFF) >> 4, v0 = (s_nv3089.in_uv >> 16) >> 4;
        const int rok = rsx_live_draw_resolve_blit(sl, s_nv3089.in_off + v0 * in_pitch + u0 * 4u, in_pitch,
                                        s_nv3089.ds_dx, s_nv3089.dt_dy, dl, s_gcm2d.dst_offset,
                                        dst_pitch, out_x, out_y, out_w, out_h);
        if (!rok && s_nv3089.in_uv <= 0x00100010u && !s_nv3089.out_pt)
            rsx_live_draw_note_resolve(sl, s_nv3089.in_off, dl, s_gcm2d.dst_offset);
    }
    /* Only the 32-bit colour formats are handled; anything else would need a
     * per-format converter and is better skipped loudly than written wrong. */
    u32 f = s_nv3089.fmt & 0xFF;
    /* NOTE: format 0x3 appears here as a downsample chain (1024x512 -> 1024x256
     * -> 512x128 -> 256x64 -> 128x32, all into one destination) that builds a
     * mip/blur pyramid, and skipping it leaves that texture empty. Treating it
     * as 4-byte A8R8G8B8 like 7/8/0xD is WRONG -- it floods the scene blue
     * (flat-blue coverage 17k -> 537k pixels), so its pixel layout differs.
     * Decode it properly before enabling; do not just add it to this list. */
    /* NV309E swizzled destination. Colour format 0x3 is A8R8G8B8 in the
     * scaled-image enum; this title uses it to build a mip/blur pyramid into a
     * swizzled texture (1024x512 -> 1024x256 -> ... all through NV309E).
     * Writing it LINEARLY into the NV3062 destination is what flooded the scene
     * blue -- wrong destination and wrong layout. */
    { u32 l2w = (s_nv309e.fmt >> 16) & 0xFF, l2h = (s_nv309e.fmt >> 24) & 0xFF;
      static int en = -1;
      /* On by default: this is the mip/blur pyramid. Without it textures have
       * no mip levels, which shows up as aliased blocky tiles and a noise patch
       * where a minified surface should be, and the shell that samples the
       * pyramid draws untextured. NV309E_BLIT=0 disables. */
      if (en < 0) { const char* e = getenv("NV309E_BLIT"); en = e ? atoi(e) : 1; }
      if (en && nv3089_dst_swizzled(out_w, out_h) &&
          (f == 3 || f == 4 || f == 7)) {
        /* RPCS3 nv3089.cpp swizzled_copy_2: the swizzle size is the output
         * size rounded up to powers of two (the NV309E log2 fields are not
         * used), and the image lands at out_x/out_y of an out_w-wide linear
         * pitch from the NV309E offset. */
        const u32 sw_w = nv3089_next_pow2(out_w), sw_h = nv3089_next_pow2(out_h);
        for (l2w = 0; (1u << l2w) < sw_w; l2w++) {}
        for (l2h = 0; (1u << l2h) < sw_h; l2h++) {}
        const u32 sbpp_s = (f == 7) ? 2u : 4u;
        const u32 dbpp_s = ((s_nv309e.fmt & 0xFFFF) == 4u /* R5G6B5 */) ? 2u : 4u;
        const int dl = (s_nv309e.dma != 0xFEED0001u);
        const u32 dloc = dl ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN;
        const u32 doff = s_nv309e.offset + out_x * dbpp_s + out_y * out_w * dbpp_s;
        int sl = (s_nv3089.src_dma != 0xFEED0001u);
        u32 src2 = gcm_dma_ea(s_nv3089.src_dma, s_nv3089.in_off);
        u32 dst2 = gcm_dma_ea(s_nv309e.dma, doff);
        if (!src2 || !dst2) return;
        u32 u0b = s_nv3089.in_uv & 0xFFFF, v0b = s_nv3089.in_uv >> 16;
        const u32 ccx = s_nv3089.clip_pt & 0xFFFF, ccy = s_nv3089.clip_pt >> 16;
        const int whole = !ccx && !ccy && (s_nv3089.clip_sz & 0xFFFF) >= out_w &&
                          (s_nv3089.clip_sz >> 16) >= out_h;
        /* The engine keeps the copy on the GPU when the source is a surface
         * newer than guest memory (no readback stall). */
        if (!mir && sbpp_s == 4 && dbpp_s == 4 && whole && !out_x && !out_y &&
            sw_w == out_w && sw_h == out_h && rsx_draw_engine_enabled() &&
            rsx_draw_engine_blit_swizzled(sl ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN,
                                          s_nv3089.in_off, in_pitch,
                                          (u32)(nv3089_origin(u0b) >> 20),
                                          (u32)(nv3089_origin(v0b) >> 20),
                                          s_nv3089.ds_dx, s_nv3089.dt_dy, dloc, doff,
                                          out_w, out_h,
                                          ((s_nv3089.in_fmt >> 24) & 0xFFu) == 1u))
            return;
        if (sbpp_s == 4) nv3089_engine_read(in_pitch, 0, 0, out_w, out_h);
        if (rsx_draw_engine_enabled())
            rsx_draw_engine_range_read(dloc, doff, sw_w * sw_h * dbpp_s);
        /* The clip rectangle of the scaled image lands at the swizzle origin
         * (RPCS3 swizzled_copy_1 + clip_image). NV3089_DSTCLIP=1: the whole
         * output. */
        nv3089_geom sg = { 0, 0, out_w, out_h, 0, 0 };
        if (!nv3089_dstclip_legacy()) {
            const u32 csz = ((s_nv3089.clip_sz >> 16) ? (s_nv3089.clip_sz & 0xFFFF0000u) : (out_h << 16)) |
                            ((s_nv3089.clip_sz & 0xFFFF) ? (s_nv3089.clip_sz & 0xFFFFu) : out_w);
            if (!nv3089_geom_calc(s_nv3089.out_pt, s_nv3089.out_sz, s_nv3089.clip_pt, csz, 0, &sg)) return;
        }
        /* RPCS3 nv3089.cpp:470-530 swizzled_copy_2 swizzles and writes the whole
         * sw_w x sw_h block: the clipped image sits at its origin in a buffer
         * that is zero elsewhere (swizzled_copy_1's temp3 and pad_texture's
         * sw_temp, Utils/image_utils.hpp:14), so every texel outside the clip
         * rectangle -- including the pow2 padding -- is written as zero.
         * Raw pointers rather than vm_read32/vm_write32: this moves ~500k
         * pixels per blit and several blits run per frame. */
        { extern u8* vm_base;
          u8* dbase = vm_base + dst2;
          const int foh = nv3089_foh();
          const u32 in_w = s_nv3089.in_sz & 0xFFFF, in_h = s_nv3089.in_sz >> 16;
          const u32 ox = (u32)(nv3089_origin(u0b) >> 20), oy = (u32)(nv3089_origin(v0b) >> 20);
          for (u32 y = 0; y < sw_h; y++) {
              nv3089_tap ty = { 0, 0, 0 };
              u32 sy0 = 0, sy1 = 0;
              if (y < sg.h) {
                  ty = nv3089_tap_at(sg.iy0 + y, s_nv3089.dt_dy, in_h, foh);
                  sy0 = nv3089_src_coord(oy, ty.t0, s_nv3089.flip_y);
                  sy1 = nv3089_src_coord(oy, ty.t1, s_nv3089.flip_y);
              }
              for (u32 x = 0; x < sw_w; x++) {
                  u8* dp = dbase + (u64)nv309e_swz(x, y, l2w, l2h) * dbpp_s;
                  if (y >= sg.h || x >= sg.w) { memset(dp, 0, dbpp_s); continue; }
                  const nv3089_tap tx = nv3089_tap_at(sg.ix0 + x, s_nv3089.ds_dx, in_w, foh);
                  const u32 c = nv3089_sample(src2, in_pitch, sbpp_s,
                                              nv3089_src_coord(ox, tx.t0, s_nv3089.flip_x),
                                              nv3089_src_coord(ox, tx.t1, s_nv3089.flip_x), tx.w,
                                              sy0, sy1, ty.w);
                  if (dbpp_s == 4) {
                      dp[0] = (u8)(c >> 24); dp[1] = (u8)(c >> 16); dp[2] = (u8)(c >> 8); dp[3] = (u8)c;
                  } else {                    /* -> R5G6B5 (big-endian) */
                      const u16 p = nv3089_argb_to_r5g6b5(c);
                      dp[0] = (u8)(p >> 8); dp[1] = (u8)p;
                  }
              }
          } }
        /* Textures sampled from here are rehashed, any engine record of a
         * swizzled copy there is dropped, and the surfaces it overlapped stop
         * owning those bytes (RPCS3: no write_barrier on this path). */
        if (rsx_draw_engine_enabled())
            rsx_draw_engine_range_cpu_wrote(dloc, doff, sw_w * sw_h * dbpp_s);
        { static int _n = 0; static int cap = -1;
          if (cap < 0) { const char* e = getenv("NV3089_DBG"); cap = e ? atoi(e) : 0; }
          if (cap && _n < cap) { _n++;
            printf("[NV3089] swizzled %ux%u fmt=0x%X src=0x%08X -> NV309E dst=0x%08X%c",
                   out_w, out_h, f, src2, dst2, 10); } }
        return;
      } }
    /* 0x3 is A8R8G8B8 in the scaled-image format enum. This title uses it for
     * two different things: the swizzled mip/blur pyramid handled above, and
     * (with a LINEAR NV3062 destination) copies of the rendered scene surface
     * into the textures its water and effects sample -- e.g.
     * src=0xCE340000 (the scene) -> dst=0xC3746200, which is the single most
     * sampled blended texture in the frame. Skipping those left every one of
     * those textures empty. Same 4-byte pixel layout as the formats already
     * handled. */
    if (f != 3 /* A8R8G8B8 (scale-format enum) */ && f != 4 /* X8R8G8B8: read as A8R8G8B8 (RPCS3) */ &&
        f != 7 /* R5G6B5 */) {   /* Y8 / X8B8G8R8 are rejected in nv3089_image_in */
        /* Log the GEOMETRY of a skipped blit, not just the format: a full-screen
         * copy from the scene surface to a registered display buffer is the
         * composite this title needs, and is worth telling apart from some
         * incidental small transfer. */
        { int _sl = (s_nv3089.src_dma != 0xFEED0001u);
          int _dl = (s_gcm2d.dst_dma  != 0xFEED0001u);
          printf("[NV3089] SKIPPED fmt=0x%X  %ux%u  src=0x%08X(pitch %u,%s) -> dst=0x%08X(pitch %u,%s) at %u,%u%c",
                 f, out_w, out_h,
                 cellGcmResolveLocated(_sl, s_nv3089.in_off), in_pitch, _sl?"LOCAL":"MAIN",
                 cellGcmResolveLocated(_dl, s_gcm2d.dst_offset), dst_pitch, _dl?"LOCAL":"MAIN",
                 out_x, out_y, 10); }
        static int _w = 0;
        if (PS3_LOG_FIRST(_w, 4)) printf("[NV3089] unsupported colour format 0x%X, blit skipped\n", f);
        return;
    }
    int src_local = (s_nv3089.src_dma != 0xFEED0001u);
    int dst_local = (s_gcm2d.dst_dma  != 0xFEED0001u);
    u32 src = gcm_dma_ea(s_nv3089.src_dma, s_nv3089.in_off);
    u32 dst = gcm_dma_ea(s_gcm2d.dst_dma, s_gcm2d.dst_offset);
    if (!src || !dst) return;
    u32 u0 = s_nv3089.in_uv & 0xFFFF, v0 = s_nv3089.in_uv >> 16;   /* 12.4 start */
    /* Destination rectangle and the scaled-image texel it starts at: RPCS3
     * geometry (nv3089_geom_calc), or with NV3089_DSTCLIP=1 the old
     * absolute-destination clip. A title tiling a resolve in 512x512 blocks
     * relies on the clip to stop at the surface edge: Tornado Outbreak's
     * bottom band is 512 rows tall with 208 left in the display buffer, and
     * writing all 512 ran on into the VRAM after it -- where its full-screen
     * quad's vertices were staged. Both rules stop it there. A caller that
     * bypasses nv3089_image_in (which NOPs a zero clip) gets the whole output. */
    nv3089_geom gm;
    { const u32 csz = ((s_nv3089.clip_sz >> 16) ? (s_nv3089.clip_sz & 0xFFFF0000u) : (s_nv3089.out_sz & 0xFFFF0000u)) |
                      ((s_nv3089.clip_sz & 0xFFFF) ? (s_nv3089.clip_sz & 0xFFFFu) : (s_nv3089.out_sz & 0xFFFFu));
      if (!nv3089_geom_calc(s_nv3089.out_pt, s_nv3089.out_sz, s_nv3089.clip_pt, csz,
                            nv3089_dstclip_legacy(), &gm)) return; }
    /* Bytes per pixel from the SDK enums (CELL_GCM_TRANSFER_SCALE_FORMAT_*:
     * 7 = R5G6B5). RPCS3 converts between R5G6B5 and A8R8G8B8 (its only
     * destination formats). */
    u32 sbpp = (f == 7) ? 2u : 4u;
    u32 dbpp = (s_gcm2d.color_fmt == 4u /* R5G6B5 */) ? 2u
             : (s_gcm2d.color_fmt == 1u /* Y8 */)     ? 1u : 4u;
    const int conv = (sbpp == 2u && dbpp == 4u) || (sbpp == 4u && dbpp == 2u);
    u32 bpp = (sbpp == dbpp) ? sbpp : 4u;
    if (conv) bpp = dbpp;
    const u32 x0 = gm.x0, y0 = gm.y0, x1 = gm.x0 + gm.w, y1 = gm.y0 + gm.h;
    /* RPCS3's blit engine: a copy between two engine colour surfaces runs on
     * the GPU, and guest memory catches up only when something reads it. */
    if (sbpp == dbpp && (bpp == 4 || bpp == 2) && rsx_draw_engine_enabled()) {
        /* Mirrored (RPCS3 apply_pixel_transforms + flipped blit area): the
         * start texel is the one output 0 reads, nv3089_src_coord's o - t. */
        const u32 offx = (u32)(((u64)gm.ix0 * s_nv3089.ds_dx) >> 20);
        const u32 offy = (u32)(((u64)gm.iy0 * s_nv3089.dt_dy) >> 20);
        const u32 ox = (u32)(nv3089_origin(u0) >> 20), oy = (u32)(nv3089_origin(v0) >> 20);
        const u32 bsx = s_nv3089.flip_x ? (ox >= offx ? ox - offx : 0xFFFFFFFFu)
                                        : (u32)((nv3089_origin(u0) + (u64)gm.ix0 * s_nv3089.ds_dx) >> 20);
        const u32 bsy = s_nv3089.flip_y ? (oy >= offy ? oy - offy : 0xFFFFFFFFu)
                                        : (u32)((nv3089_origin(v0) + (u64)gm.iy0 * s_nv3089.dt_dy) >> 20);
        const int linear = ((s_nv3089.in_fmt >> 24) & 0xFFu) == 1u;   /* FOH */
        if (bsx != 0xFFFFFFFFu && bsy != 0xFFFFFFFFu &&
            rsx_draw_engine_blit_flip(src_local ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN,
                                      s_nv3089.in_off, in_pitch, bsx, bsy,
                                      s_nv3089.ds_dx, s_nv3089.dt_dy,
                                      dst_local ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN,
                                      s_gcm2d.dst_offset, dst_pitch, x0, y0,
                                      gm.w, gm.h, linear, s_nv3089.in_sz, bpp,
                                      (s_nv3089.flip_x ? 1u : 0u) | (s_nv3089.flip_y ? 2u : 0u)))
            return;
    }
    if (sbpp == 4) nv3089_engine_read(in_pitch, gm.ix0, gm.iy0, gm.w, gm.h);
    /* RPCS3 nv3089.cpp:634-683: into a bound MAIN tile the scaled image is
     * produced linearly and stored with tile_texel_data32/16. */
    u32 t_base = 0, t_lo = 0, t_len = 0, t_pix = 0;
    u8* t_save = NULL;
    const CellGcmTileInfo* t_reg = (!dst_local && (bpp == 4 || bpp == 2))
        ? gcm_tiled_region(dst, dst_pitch * gm.h, &t_base) : NULL;
    if (t_reg) {
        extern u8* vm_base;
        t_pix = dst + out_y * dst_pitch + out_x * bpp;
        const u64 w0 = (u64)dst + (u64)y0 * dst_pitch + (u64)x0 * bpp;
        const u64 w1 = w0 + (u64)(gm.h - 1u) * dst_pitch + (u64)gm.w * bpp;
        const u64 l1 = (u64)t_pix + (u64)(gm.h - 1u) * t_reg->pitch + (u64)gm.w * bpp;
        const u64 lo = w0 < t_pix ? w0 : t_pix, hi = w1 > l1 ? w1 : l1;
        if (hi - lo <= 0x4000000u && hi <= 0x100000000ull && (t_save = (u8*)malloc((size_t)(hi - lo)))) {
            t_lo = (u32)lo; t_len = (u32)(hi - lo);
            memcpy(t_save, vm_base + t_lo, t_len);
        } else t_reg = NULL;
    }
    /* NV3089_ROWCOPY (default 1): an unscaled blit copies a row at a time.
     * The per-pixel loop pays a vm_read32 and a vm_write32 per pixel -- the
     * bounds and null-store checks, the store hooks, a reservation lookup and
     * two byte swaps that cancel -- and a title that copies 1280x720 surfaces
     * to main memory every frame (inFAMOUS 2 does, for its SPU lighting)
     * spent a quarter of the FIFO thread there. NV3089_ROWCOPY=0 keeps the
     * per-pixel copy, which is what the PPU store watches see. */
    static int rowcopy = -1;
    if (rowcopy < 0) { const char* e = getenv("NV3089_ROWCOPY"); rowcopy = e ? atoi(e) : 1; }
    if (rowcopy && !conv && !mir && s_nv3089.ds_dx == 0x100000u && s_nv3089.dt_dy == 0x100000u) {
        /* the source start in whole texels: IMAGE_IN_POINT is 12.4 */
        const u32 sx0 = (u32)(nv3089_origin(u0) >> 20) + gm.ix0;
        const u32 sy0 = (u32)(nv3089_origin(v0) >> 20) + gm.iy0;
        const u32 n = gm.w * bpp;
        for (u32 y = 0; y < gm.h; y++) {
            const u64 sa = (u64)src + (u64)(sy0 + y) * in_pitch + (u64)sx0 * bpp;
            const u64 da = (u64)dst + (u64)(y0 + y) * dst_pitch + (u64)x0 * bpp;
            if (sa + n > 0x100000000ull || da + n > 0x100000000ull) continue;
            gcm_row_copy((u32)da, (u32)sa, n);
        }
    } else if (dbpp == 2 || dbpp == 4) {
        /* Texel-centre point or FOH bilinear sampling (nv3089_tap_at), mirrored
         * for a negative scale; R5G6B5 is filtered at 8 bits per channel. */
        const int foh = nv3089_foh();
        const u32 in_w = s_nv3089.in_sz & 0xFFFF, in_h = s_nv3089.in_sz >> 16;
        const u32 ox = (u32)(nv3089_origin(u0) >> 20), oy = (u32)(nv3089_origin(v0) >> 20);
        for (u32 y = 0; y < gm.h; y++) {
            const nv3089_tap ty = nv3089_tap_at(gm.iy0 + y, s_nv3089.dt_dy, in_h, foh);
            const u32 sy0 = nv3089_src_coord(oy, ty.t0, s_nv3089.flip_y);
            const u32 sy1 = nv3089_src_coord(oy, ty.t1, s_nv3089.flip_y);
            for (u32 x = 0; x < gm.w; x++) {
                const nv3089_tap tx = nv3089_tap_at(gm.ix0 + x, s_nv3089.ds_dx, in_w, foh);
                const u32 c = nv3089_sample(src, in_pitch, sbpp,
                                            nv3089_src_coord(ox, tx.t0, s_nv3089.flip_x),
                                            nv3089_src_coord(ox, tx.t1, s_nv3089.flip_x), tx.w,
                                            sy0, sy1, ty.w);
                const u32 d = dst + (y0 + y) * dst_pitch + (x0 + x) * dbpp;
                if (dbpp == 4) vm_write32(d, c);
                else           vm_write16(d, nv3089_argb_to_r5g6b5(c));
            }
        }
    } else
    for (u32 y = 0; y < gm.h; y++) {
        const u32 dy = y0 + y;
        u64 sv = nv3089_origin(v0) + (u64)(gm.iy0 + y) * s_nv3089.dt_dy;
        u32 sy = (u32)(sv >> 20);
        for (u32 x = 0; x < gm.w; x++) {
            const u32 dx = x0 + x;
            u64 su = nv3089_origin(u0) + (u64)(gm.ix0 + x) * s_nv3089.ds_dx;
            u32 sx = (u32)(su >> 20);
            u32 s = src + sy * in_pitch + sx * (conv ? sbpp : bpp);
            u32 d = dst + dy * dst_pitch + dx * bpp;
            if (conv) {
                if (sbpp == 2) vm_write32(d, nv3089_r5g6b5_to_argb(vm_read16(s)));
                else           vm_write16(d, nv3089_argb_to_r5g6b5(vm_read32(s)));
            }
            else if (bpp == 4) vm_write32(d, vm_read32(s));
            else if (bpp == 2) vm_write16(d, vm_read16(s));
            else               vm_write8(d, vm_read8(s));
        }
    }
    if (t_reg) {
        extern u8* vm_base;
        u8* lin = (u8*)malloc(t_len);
        if (lin) {
            memcpy(lin, vm_base + t_lo, t_len);
            memcpy(vm_base + t_lo, t_save, t_len);
            gcm_tile_texel_data(t_pix, lin + (t_pix - t_lo), bpp, t_base, dst - t_base, t_reg->size,
                                t_reg->bank, t_reg->pitch, gm.w, gm.h);
            free(lin);
        }
        free(t_save);
    }
    if (bpp == 4) nv3089_engine_wrote(dst_pitch, x0, y0, x1, y1);
    /* Cached textures over the written rows are re-hashed before their next
     * use (RPCS3's write barrier on the blit destination). */
    if (rsx_draw_engine_enabled())
        rsx_draw_engine_textures_wrote(dst_local ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN,
                                       s_gcm2d.dst_offset + y0 * dst_pitch + x0 * bpp,
                                       (gm.h - 1u) * dst_pitch + gm.w * bpp);
    { static int _n = 0; static int cap = -1;
      if (cap < 0) { const char* e = getenv("NV3089_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && _n < cap) { _n++;
        printf("[NV3089] %ux%u fmt=0x%X src=0x%08X(pitch %u) -> dst=0x%08X(pitch %u) at %u,%u\n",
               out_w, out_h, f, src, in_pitch, dst, dst_pitch, out_x, out_y); } }
}

/* NV3089 IMAGE_IN trigger, with RPCS3's decode_transfer_registers checks in
 * front of the copy (nv3089.cpp): only SRCCOPY is implemented (anything else
 * is logged and dropped; NV3089_ANY_OP=1 copies anyway), a zero clip size is
 * a NOP, the source start is a whole texel (CENTER origin: -0.5 then floor;
 * a downscale snaps it to the scaled grid), and a 1:1 copy of an image onto
 * itself is a NOP. */
/* RPCS3 nv3089.cpp decode_transfer_registers validation: a non-SRCCOPY
 * operation, an unknown source / destination format or an unmapped
 * destination / source recovers the FIFO; an unknown context surface is a
 * NOP. GCM_NV3089_VALIDATE=0 restores the old skip-and-continue. */
static int s_nv3089_recover;
static int nv3089_validate_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GCM_NV3089_VALIDATE"); on = !(e && atoi(e) == 0); }
    return on;
}

static int nv3089_invalid(const char* what, u32 v)
{
    static u32 n = 0;
    if (n++ < 8) fprintf(stderr, "[NV3089] %s (0x%X): FIFO recover (as RPCS3)%c", what, v, 10);
    s_nv3089_recover = gcm_fifo_recover_on();
    return 1;
}

/* 1 = drop the blit (recover pending if s_nv3089_recover is set) */
static int nv3089_validate(void)
{
    if (!nv3089_validate_on()) return 0;
    if (s_nv3089.op_written && s_nv3089.op != 3u && !(getenv("NV3089_ANY_OP") && getenv("NV3089_ANY_OP")[0] == '1'))
        return nv3089_invalid("operation is not SRCCOPY", s_nv3089.op);
    const u32 sf = s_nv3089.fmt;
    if (sf < 1u || sf > 0xDu) return nv3089_invalid("unknown src color format", sf);
    const u32 surf = s_nv3089.dst_surf;
    if (surf && surf != NV3089_CTX_SURFACE2D && surf != NV3089_CTX_SWIZZLE2D) {
        static u32 n = 0;
        if (n++ < 8) fprintf(stderr, "[NV3089] unknown context surface 0x%X: NOP (as RPCS3)%c", surf, 10);
        return 1;
    }
    const int swz = surf == NV3089_CTX_SWIZZLE2D;
    const u32 df = swz ? (s_nv309e.fmt & 0xFFFFu) : s_gcm2d.color_fmt;
    if (df != 4u && df != 0xAu && df != 0xBu)
        return nv3089_invalid(swz ? "unknown NV309E dst color format" : "unknown NV3062 dst color format", df);
    if (!(swz ? gcm_dma_ea(s_nv309e.dma, s_nv309e.offset) : gcm_dma_ea(s_gcm2d.dst_dma, s_gcm2d.dst_offset)))
        return nv3089_invalid("unmapped dst", swz ? s_nv309e.offset : s_gcm2d.dst_offset);
    if (!gcm_dma_ea(s_nv3089.src_dma, s_nv3089.in_off))
        return nv3089_invalid("unmapped src", s_nv3089.in_off);
    return 0;
}

static void nv3089_image_in(void)
{
    if (!nv3089_validate_on() && s_nv3089.op_written && s_nv3089.op != 3u) {
        static int any = -1;
        if (any < 0) { const char* e = getenv("NV3089_ANY_OP"); any = e && e[0] == '1'; }
        static int _w = 0;
        if (PS3_LOG_FIRST(_w, 4))
            printf("[NV3089] operation %u is not SRCCOPY: %s%c", s_nv3089.op,
                   any ? "copied anyway" : "skipped", 10);
        if (!any) return;
    }
    /* RPCS3 nv3089.cpp:305-317: source formats other than A8R8G8B8, X8R8G8B8
     * (read as A8R8G8B8) and R5G6B5 are unsupported there (fmt::throw_exception,
     * which stops emulation). Y8 and X8B8G8R8 used to be copied as 32-bit
     * ARGB here (wrong width / swapped R and B); drop them instead. */
    { const u32 sf = s_nv3089.fmt & 0xFF;
      if (sf == 8u || sf == 0xDu) {
          static int _w = 0;
          if (!_w) { _w = 1;
              fprintf(stderr, "[NV3089] source format 0x%X (%s) unsupported (as RPCS3), blit dropped%c",
                      sf, sf == 8u ? "Y8" : "X8B8G8R8", 10); }
          return;
      } }
    const u32 out_w = s_nv3089.out_sz & 0xFFFF, out_h = s_nv3089.out_sz >> 16;
    u32 clip_w = s_nv3089.clip_sz & 0xFFFF, clip_h = s_nv3089.clip_sz >> 16;
    if (clip_w > out_w) clip_w = out_w;
    if (clip_h > out_h) clip_h = out_h;
    if (!clip_w || !clip_h || !s_nv3089.ds_dx || !s_nv3089.dt_dy) return;
    if (nv3089_validate()) return;
    /* DS_DX / DT_DY are signed 11.20 fixed point (RPCS3 rsx_decode.h:2052
     * decode_fxp<11,20>); a negative step mirrors the blit (nv3089.cpp:590).
     * Scaling and validation use the magnitude. */
    const s32 sdx = (s32)s_nv3089.ds_dx, sdy = (s32)s_nv3089.dt_dy;
    const u32 adx = sdx < 0 ? (u32)(-(s64)sdx) : (u32)sdx;
    const u32 ady = sdy < 0 ? (u32)(-(s64)sdy) : (u32)sdy;
    /* RPCS3: an empty input region throws (treated as a NOP here), and a
     * scale that maps the input to zero output pixels is ignored
     * (nv3089.cpp:320 convert_w = |scale_x| * in_w). */
    { const u32 in_w = s_nv3089.in_sz & 0xFFFF, in_h = s_nv3089.in_sz >> 16;
      if (!in_w || !in_h) return;
      if ((u64)in_w * 0x100000u < adx || (u64)in_h * 0x100000u < ady) return; }

    const u32 saved_uv = s_nv3089.in_uv;
    const int center = ((s_nv3089.in_fmt >> 16) & 0xFFu) == 1u;
    u32 ix = (saved_uv & 0xFFFF) >> 4, iy = (saved_uv >> 16) >> 4;
    if (center) {
        /* floor(max(v, 0.5) - 0.5) on 12.4: (v - 8) >> 4, clamped at 0 */
        ix = (saved_uv & 0xFFFF) > 8u ? ((saved_uv & 0xFFFF) - 8u) >> 4 : 0u;
        iy = (saved_uv >> 16) > 8u ? ((saved_uv >> 16) - 8u) >> 4 : 0u;
    }
    /* RPCS3 nv3089.cpp:161-172 subpixel snap with the signed scale:
     * in = floor(in * scale) / scale (floor without libm). */
    const float scale_x = 1048576.0f / (float)sdx;
    const float scale_y = 1048576.0f / (float)sdy;
    if (scale_x < 1.0f) { const float v = (float)ix * scale_x; s32 fl = (s32)v; if ((float)fl > v) fl--;
                          ix = (u32)((float)fl / scale_x); }
    if (scale_y < 1.0f) { const float v = (float)iy * scale_y; s32 fl = (s32)v; if ((float)fl > v) fl--;
                          iy = (u32)((float)fl / scale_y); }
    ix &= 0xFFFu; iy &= 0xFFFu;

    if (s_nv3089.ds_dx == 0x100000u && s_nv3089.dt_dy == 0x100000u &&
        (s_nv3089.in_sz & 0xFFFF) == clip_w && (s_nv3089.in_sz >> 16) == clip_h) {
        const int swz = nv3089_dst_swizzled(out_w, out_h);
        const u32 sbpp = (s_nv3089.fmt & 0xFF) == 7 ? 2u : 4u;
        const u32 dbpp = (swz ? (s_nv309e.fmt & 0xFFFF) : s_gcm2d.color_fmt) == 4u ? 2u : 4u;
        u32 ip = s_nv3089.in_fmt & 0xFFFF, op = swz ? 0u : (s_gcm2d.pitch >> 16);
        if (!ip) ip = sbpp * (s_nv3089.in_sz & 0xFFFF);
        if (!op) op = dbpp * out_w;
        if (ip == op && gcm_dma_ea(s_nv3089.src_dma, s_nv3089.in_off) ==
                        (swz ? gcm_dma_ea(s_nv309e.dma, s_nv309e.offset)
                             : gcm_dma_ea(s_gcm2d.dst_dma, s_gcm2d.dst_offset)))
            return;
    }
    s_nv3089.in_uv = (iy << 20) | (ix << 4);
    s_nv3089.ds_dx = adx; s_nv3089.dt_dy = ady;
    s_nv3089.flip_x = sdx < 0; s_nv3089.flip_y = sdy < 0;
    nv3089_blit();
    s_nv3089.ds_dx = (u32)sdx; s_nv3089.dt_dy = (u32)sdy;
    s_nv3089.flip_x = s_nv3089.flip_y = 0;
    s_nv3089.in_uv = saved_uv;
}

/* NV0039 "memory to memory format": the linear buffer copy libgcm's
 * cellGcmSetTransferData / cellGcmSetTransferImage emit. Registers (RPCS3
 * gcm_enums.h NV0039_*): 0x0184/0x0188 source/destination DMA, 0x030C/0x0310
 * offsets, 0x0314/0x0318 signed pitches, 0x031C line length (bytes), 0x0320
 * line count, 0x0324 format (input stride [7:0], output stride [15:8]) and
 * 0x0328 BUFFER_NOTIFY, the trigger. Semantics follow RPCS3's
 * nv0039::buffer_notify, including the byte-stride gather and the
 * overlapping-range double copy. */
static struct {
    u32 in_dma, out_dma, in_off, out_off, len, count, fmt;
    s32 in_pitch, out_pitch;
} s_nv0039 = { 0xFEED0001u, 0xFEED0000u };   /* rsx_methods.cpp:1211-1212 init */

static void gcm_zc_label_sync(void);

static void nv0039_copy(void)
{
    const u32 len = s_nv0039.len, count = s_nv0039.count;
    const u32 in_fmt = s_nv0039.fmt & 0xFFu, out_fmt = (s_nv0039.fmt >> 8) & 0xFFu;
    if (!len || !count || !in_fmt || in_fmt > 4 || !out_fmt || out_fmt > 4) return;
    /* RPCS3 nv0039.cpp:106 buffer_notify -> thread::read_barrier: ZCULL
     * reports still queued land before the copy reads them (a title copies its
     * report area). GCM_NV0039_ZSYNC=0 skips it. */
    { static int zs = -1;
      if (zs < 0) { const char* e = getenv("GCM_NV0039_ZSYNC"); zs = !(e && atoi(e) == 0); }
      if (zs) gcm_zc_label_sync(); }
    const s32 in_pitch = s_nv0039.in_pitch, out_pitch = s_nv0039.out_pitch;
    const int src_local = (s_nv0039.in_dma != 0xFEED0001u);
    const int dst_local = (s_nv0039.out_dma != 0xFEED0001u);
    const u32 src = gcm_dma_ea(s_nv0039.in_dma, s_nv0039.in_off);
    const u32 dst = gcm_dma_ea(s_nv0039.out_dma, s_nv0039.out_off);
    if (!src || !dst || src == 0xFFFFFFFFu || dst == 0xFFFFFFFFu) return;
    const u32 in_w = len * in_fmt, out_w = len * out_fmt;
    /* With a negative pitch the rows run downwards in memory: the touched
     * range starts at the last row. */
    const s64 in_span = (s64)in_pitch * (s64)(count - 1), out_span = (s64)out_pitch * (s64)(count - 1);
    const u32 read_len = (u32)((in_span < 0 ? -in_span : in_span) + in_w);
    const u32 write_len = (u32)((out_span < 0 ? -out_span : out_span) + out_w);
    const u32 read_off = s_nv0039.in_off + (u32)(in_span < 0 ? in_span : 0);
    const u32 write_off = s_nv0039.out_off + (u32)(out_span < 0 ? out_span : 0);
    const u32 src_lo = src + (u32)(in_span < 0 ? in_span : 0);
    const u32 dst_lo = dst + (u32)(out_span < 0 ? out_span : 0);
    if (rsx_draw_engine_enabled()) {
        rsx_draw_engine_range_read(src_local ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN,
                                   read_off, read_len);
        rsx_draw_engine_range_will_write(dst_local ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN,
                                         write_off, write_len);
    }
    extern u8* vm_base;
    u8* d = vm_base + dst;
    const u8* s = vm_base + src;
    if (in_fmt > 1 || out_fmt > 1) {
        for (u32 r = 0; r < count; r++)
            for (u32 c = 0; c < len; c++)
                d[(s64)r * out_pitch + (s64)c * out_fmt] = s[(s64)r * in_pitch + (s64)c * in_fmt];
    } else {
        const int overlap = src_local == dst_local &&
            (u64)src_lo < (u64)dst_lo + write_len && (u64)dst_lo < (u64)src_lo + read_len;
        const int block = (u32)in_pitch == in_w && (u32)out_pitch == out_w;
        if (block) memmove(d, s, read_len);
        else if (!overlap)
            for (u32 r = 0; r < count; r++)
                memcpy(d + (s64)r * out_pitch, s + (s64)r * in_pitch, len);
        else {
            u8* tmp = (u8*)malloc((size_t)len * count);
            if (!tmp) return;
            for (u32 r = 0; r < count; r++) memcpy(tmp + (size_t)r * len, s + (s64)r * in_pitch, len);
            for (u32 r = 0; r < count; r++) memcpy(d + (s64)r * out_pitch, tmp + (size_t)r * len, len);
            free(tmp);
        }
    }
    if (rsx_draw_engine_enabled())
        rsx_draw_engine_range_wrote(dst_local ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN,
                                    write_off, write_len);
    { static int _n = 0; static int cap = -1;
      if (cap < 0) { const char* e = getenv("NV0039_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && _n < cap) { _n++;
        printf("[NV0039] %ux%u fmt=0x%X src=0x%08X(pitch %d) -> dst=0x%08X(pitch %d)%c",
               len, count, s_nv0039.fmt, src, in_pitch, dst, out_pitch, 10); } }
}

static void nv0039_method(u32 method, u32 data)
{
    switch (method) {
    case 0x0184: s_nv0039.in_dma    = data; return;
    case 0x0188: s_nv0039.out_dma   = data; return;
    case 0x030C: s_nv0039.in_off    = data; return;
    case 0x0310: s_nv0039.out_off   = data; return;
    case 0x0314: s_nv0039.in_pitch  = (s32)data; return;
    case 0x0318: s_nv0039.out_pitch = (s32)data; return;
    case 0x031C: s_nv0039.len       = data; return;
    case 0x0320: s_nv0039.count     = data; return;
    case 0x0324: s_nv0039.fmt       = data; return;
    case 0x0328: nv0039_copy(); return;
    }
}

/* Which object subchannel 1 drives. libgcm binds NV0039 there (RPCS3 maps
 * subchannel 1 to NV0039 outright), but some titles bind NV4097 to it
 * (Twisted Metal, Yakuza Dead Souls) and SET_OBJECT is not always seen. The
 * NV0039 methods alias NV4097 blend/colour-mask state (0x030C..0x0328), so
 * routing them as 3D corrupts it. A SET_OBJECT matching subchannel 0's
 * handle decides 3D, a different one NV0039. Without one, any method outside
 * NV0039's set makes the subchannel 3D for good, replaying the NV0039-range
 * words held meanwhile; BUFFER_NOTIFY (0x0328) makes it NV0039 for good. */
static u32 s_subch_handle[8];
static int s_sub1_class;                  /* 0 unknown, 1 NV4097, 2 NV0039 */
static u32 s_sub1_held[12], s_sub1_held_mask;

static int nv0039_method_index(u32 m)
{
    if (m == 0x0184) return 0;
    if (m == 0x0188) return 1;
    if (m >= 0x030C && m <= 0x0328) return 2 + (int)((m - 0x030C) >> 2);
    return -1;
}

static void gcm_note_set_object(u32 subch, u32 handle)
{
    if (subch >= 8) return;
    s_subch_handle[subch] = handle;
    if (s_subch_handle[0] && s_subch_handle[1])
        s_sub1_class = (s_subch_handle[1] == s_subch_handle[0]) ? 1 : 2;
}

/* 1 when the method on subchannel 1 is NV0039's, 0 when it is NV4097's. */
static int gcm_sub1_is_nv0039(rsx_state* st, u32 m)
{
    if (s_sub1_class == 1) return 0;
    if (s_sub1_class == 2) return 1;
    const int idx = nv0039_method_index(m);
    if (idx < 0) {
        s_sub1_class = 1;
        fprintf(stderr, "[gcm] subchannel 1 carries NV4097 (method 0x%04X)%c", m, 10);
        for (int i = 0; i < 12; i++)
            if (s_sub1_held_mask & (1u << i)) {
                const u32 hm = i == 0 ? 0x0184u : i == 1 ? 0x0188u : 0x030Cu + (u32)(i - 2) * 4u;
                rsx_process_method(st, hm, s_sub1_held[i]);
                if (rsx_draw_engine_enabled()) rsx_draw_engine_method(hm, s_sub1_held[i]);
            }
        s_sub1_held_mask = 0;
        return 0;
    }
    if (m == 0x0328) {
        s_sub1_class = 2;
        s_sub1_held_mask = 0;
        fprintf(stderr, "[gcm] subchannel 1 carries NV0039%c", 10);
    }
    return 1;
}

static void gcm_sub1_nv0039(u32 m, u32 data)
{
    if (s_sub1_class == 0) {
        const int idx = nv0039_method_index(m);
        if (idx >= 0) { s_sub1_held[idx] = data; s_sub1_held_mask |= 1u << idx; }
    }
    nv0039_method(m, data);
}

/* RPCS3 nv308a.cpp color(): A8R8G8B8 -> R5G6B5 is (c*32)/255 per channel,
 * truncated to 16 bits (so a full-intensity channel carries into the next
 * field exactly as the hardware path RPCS3 models). */
static u16 nv308a_to_r5g6b5(u32 data)
{
    const u32 r = (((data >> 16) & 0xFFu) * 32u) / 255u;
    const u32 g = (((data >> 8) & 0xFFu) * 64u) / 255u;
    const u32 b = ((data & 0xFFu) * 32u) / 255u;
    return (u16)((r << 11) | (g << 5) | b);
}

/* NV308A writes guest memory behind the draw engine's back. RPCS3 sees every
 * such store through its memory-protection write barrier, so a texture or
 * surface over the destination is refreshed before its next use. Coalesce the
 * per-word writes of a method block and report them once. */
static struct { int pending, px4; u32 loc, off, len; } s_nv308a_dirty;

static void nv308a_flush_writes(void)
{
    if (!s_nv308a_dirty.pending) return;
    s_nv308a_dirty.pending = 0;
    if (!rsx_draw_engine_enabled()) return;
    const u32 loc = s_nv308a_dirty.loc, off = s_nv308a_dirty.off, len = s_nv308a_dirty.len;
    rsx_draw_engine_textures_wrote(loc, off, len);
    /* One row of 32-bit pixels: push exactly those into an engine surface
     * over them (whole-row reloads would clobber GPU-only pixels beside it). */
    if (s_nv308a_dirty.px4 && len >= 4u)
        rsx_draw_engine_guest_wrote(loc, off, len, len / 4u, 1u);
}

static void nv308a_note_write(int local, u32 off, u32 len)
{
    const u32 loc = local ? RSX_LOCATION_LOCAL : RSX_LOCATION_MAIN;
    const int px4 = len == 4u;
    if (s_nv308a_dirty.pending && s_nv308a_dirty.loc == loc && s_nv308a_dirty.px4 == px4 &&
        off >= s_nv308a_dirty.off && off <= s_nv308a_dirty.off + s_nv308a_dirty.len) {
        const u32 end = off + len, cur = s_nv308a_dirty.off + s_nv308a_dirty.len;
        if (end > cur) s_nv308a_dirty.len = end - s_nv308a_dirty.off;
        return;
    }
    nv308a_flush_writes();
    s_nv308a_dirty.pending = 1; s_nv308a_dirty.px4 = px4;
    s_nv308a_dirty.loc = loc; s_nv308a_dirty.off = off; s_nv308a_dirty.len = len;
}

static int gcm_2d_strict_subch(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GCM_2D_STRICT_SUBCH"); on = !(e && atoi(e) == 0); }
    return on;
}

static void gcm_2d_method(u32 subch, u32 method, u32 data)
{
    /* GCM2D_TRACE=1: histogram of (subchannel, method) actually seen. The
     * subchannel a title binds each 2D object to is libgcm-version-specific and
     * SET_OBJECT binds are not tracked, so state landing on an unexpected
     * subchannel is silently dropped -- which looks like a blit using stale
     * destination state rather than like missing state. */
    { static int tr = -1;
      if (tr < 0) { const char* e = getenv("GCM2D_TRACE"); tr = e ? atoi(e) : 0; }
      if (tr) {
          static u8 seen[8][1024]; static u64 n = 0;
          u32 mi = (method >> 2) & 1023;
          if (subch < 8 && !seen[subch][mi]) {
              seen[subch][mi] = 1;
              printf("[GCM2D-TRACE] subch=%u method=0x%04X (first, data=0x%08X)%c",
                     subch, method, data, 10);
          }
          if (++n == 400000ull) {
              printf("[GCM2D-TRACE] --- subchannels used: ");
              for (u32 sc = 0; sc < 8; sc++) {
                  u32 c = 0; for (u32 m = 0; m < 1024; m++) c += seen[sc][m];
                  if (c) printf("%u(%u methods) ", sc, c);
              }
              printf("---%c", 10);
          }
      } }
    /* Subchannel bindings are libgcm-version-specific (SET_OBJECT binds are
     * not tracked): demosaic's SDK emits dest-surface on sub 3 and image-from-
     * CPU on sub 5 (cellGcmSetInlineTransfer: 0x4630C / 0xCA304 / 0xA400
     * headers); other versions use the classic 2 / 4. Accept both. */
    const int strict = gcm_2d_strict_subch();
    if (strict && (subch == 2 || subch == 7)) {
        static int n = 0; if (PS3_LOG_FIRST(n, 8))
            printf("[GCM2D] subch %u method 0x%04X dropped (no 2D object there in RPCS3)%c", subch, method, 10);
        return;
    }
    if (subch == 2 || subch == 3) {         /* NV3062 context surface 2D */
        switch (method) {
        /* RPCS3 gcm_enums.h:768-769: IMAGE_SOURCE (0x0184) and IMAGE_DESTIN
         * (0x0188) are separate; only DESTIN locates the output (nv3089.cpp:103). */
        case 0x0184: s_gcm2d.src_dma = data; return;
        case 0x0188: s_gcm2d.dst_dma = data; return;
        case 0x0300: s_gcm2d.color_fmt  = data; return;
        case 0x0304: s_gcm2d.pitch      = data; return;
        case 0x030C: s_gcm2d.dst_offset = data; return;
        }
        return;
    }
    if (subch == 4 || subch == 5) {         /* NV308A image-from-CPU, or NV309E */
        /* Dispatch by the object bound with SET_OBJECT (RPCS3 binds by class;
         * NV308A_SET_COLOR_FORMAT is also 0x0300, gcm_enums.h:~794). With the
         * libgcm NV309E (0x31337A73) or NV308A (0x31337808, rsx_methods.cpp:535)
         * handle bound, that decides; otherwise SET_FORMAT (0x0300) identifies
         * NV309E as before. */
        const u32 h = s_subch_handle[subch];
        const int bound = h == NV3089_CTX_SWIZZLE2D || h == 0x31337808u;
        const int is309e = strict ? subch == 4u
                         : bound ? h == NV3089_CTX_SWIZZLE2D
                                 : (method == 0x0300 || (s_nv309e.active && s_nv309e.subch == (int)subch));
        if (method == 0x0300 && !is309e) return;   /* NV308A colour format: RPCS3 nv308a.cpp uses NV3062's */
        if (method == 0x0300) { s_nv309e.fmt = data; s_nv309e.active = 1;
                                s_nv309e.subch = (int)subch;
            { static int _n = 0; if (PS3_LOG_FIRST(_n, 4))
                printf("[NV309E] format=0x%08X (log2 %ux%u fmt=0x%X)%c", data,
                       1u << ((data >> 16) & 0xFF), 1u << ((data >> 24) & 0xFF),
                       data & 0xFFFF, 10); }
            return; }
        if (method == 0x0304 && is309e) {
            s_nv309e.offset = data; return; }
        if (method == 0x0184 && is309e) {
            s_nv309e.dma = data; return; }    /* SET_CONTEXT_DMA_IMAGE */
        if (strict && is309e) return;
        switch (method) {
        case 0x0304: s_gcm2d.point    = data; return;
        case 0x0308: s_gcm2d.size_out = data; return;
        case 0x030C: return;                /* SIZE_IN */
        }
        /* RPCS3 RSXFIFO.cpp:~911 / nv308a.cpp:16: NV308A_COLOR spans 0x700
         * methods (0x0400..0x1FFC), index = (method - 0x400) >> 2. */
        if (method >= 0x0400 && method <= 0x1FFC) {
            /* COLOR data word: write into the destination surface. Inline
             * transfers use format Y32 (4 bytes/px, one row per point.y).
             * DMA 0xFEED0000 = local memory, 0xFEED0001 = main memory. */
            u32 idx = (method - 0x0400) >> 2;
            /* RPCS3 nv308a.cpp: words past SIZE_OUT.x are dropped. */
            if (idx >= (s_gcm2d.size_out & 0xFFFF)) return;
            u32 px  = (s_gcm2d.point & 0xFFFF) + idx;
            u32 py  = s_gcm2d.point >> 16;
            u32 dst_pitch = s_gcm2d.pitch >> 16;
            int local = (s_gcm2d.dst_dma != 0xFEED0001u);
            u32 base = gcm_dma_ea(s_gcm2d.dst_dma, s_gcm2d.dst_offset);
            if (!base) return;
            { static int _it = 0;
              if (PS3_LOG_FIRST(_it, 6))
                  printf("[GCM2D] inline write dst=0x%08X (off=0x%X dma=0x%X pt=%u,%u pitch=%u) = 0x%08X\n",
                         base + py * dst_pitch + px * 4, s_gcm2d.dst_offset,
                         s_gcm2d.dst_dma, px, py, dst_pitch, data); }
            if (s_gcm2d.color_fmt == 4u) {     /* R5G6B5 destination: 2 bytes */
                vm_write16(base + py * dst_pitch + px * 2, nv308a_to_r5g6b5(data));
                nv308a_note_write(local, s_gcm2d.dst_offset + py * dst_pitch + px * 2, 2);
                return;
            }
            vm_write32(base + py * dst_pitch + px * 4, data);
            nv308a_note_write(local, s_gcm2d.dst_offset + py * dst_pitch + px * 4, 4);
            return;
        }
        return;
    }
    /* NV3089 "scaled image from memory" -- the blit PSGL uses to move a texture
     * it has assembled elsewhere into the VRAM the sampler reads. Without it a
     * title's textures stay whatever the destination was before (zero), so
     * geometry renders flat no matter how correct the sampler is.
     *
     * Registers: 0x0184 source DMA, 0x0198 destination surface (the NV3062
     * state above), 0x0300 colour format, 0x0304 operation, 0x0308/0x030C clip
     * point/size, 0x0310/0x0314 out point/size, 0x0318/0x031C ds/dx dt/dy in
     * 20.12 fixed point, 0x0400 in size, 0x0404 (origin<<16)|pitch, 0x0408 in
     * offset, 0x040C in u/v start -- which is also the trigger. */
    if (subch == 6 || subch == 7) {
        switch (method) {
        case 0x0184: s_nv3089.src_dma   = data; return;
        case 0x0198: s_nv3089.dst_surf  = data; return;
        case 0x0300: s_nv3089.fmt       = data; return;
        case 0x0304: s_nv3089.op        = data; s_nv3089.op_written = 1; return;
        case 0x0308: s_nv3089.clip_pt   = data; return;
        case 0x030C: s_nv3089.clip_sz   = data; return;
        case 0x0310: s_nv3089.out_pt    = data; return;
        case 0x0314: s_nv3089.out_sz    = data; return;
        case 0x0318: s_nv3089.ds_dx     = data; return;
        case 0x031C: s_nv3089.dt_dy     = data; return;
        case 0x0400: s_nv3089.in_sz     = data; return;
        case 0x0404: s_nv3089.in_fmt    = data; return;
        case 0x0408: s_nv3089.in_off    = data; return;
        case 0x040C: s_nv3089.in_uv     = data; nv3089_image_in(); return;
        }
        return;
    }
    /* NV0039 / NV309E: not implemented yet -- log first sightings. */
    /* GCM2D_DBG=<N> raises the cap and adds the data word: the fixed 8 showed
     * only that SOMETHING was unhandled, not enough to implement it. */
    static int warned = 0, wcap = -1;
    if (wcap < 0) { const char* e = getenv("GCM2D_DBG"); wcap = e ? atoi(e) : 8; }
    if (warned < wcap) { warned++;
        printf("[GCM2D-UNH] subch %u method 0x%04X data=0x%08X\n", subch, method, data); }
    if (0)
        printf("[cellGcmSys] FIFO subch %u method 0x%04X (unhandled 2D engine)\n",
               subch, method);
}

/* Present gate for the ticker: a flip is ready once the drain has consumed
 * everything up to put. The guest blocks in its own WaitFlip right after
 * flipping, so the FIFO holds EXACTLY the completed frame -- no guest-side
 * blocking needed (the old spin serialized two vsync-class waits per frame
 * and halved the frame rate). Call AFTER draining. */
int cellGcm_take_flip_pending_synced(void)
{
    if (!s_flip_pending) return 0;
    u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
    u32 get = vm_read32(GCM_CONTROL_GUEST_ADDR + 4);
    if (get != put) return 0;   /* frame not fully drained yet */
    s_flip_pending = 0;
    return 1;
}

/* ---- fence (SET_REFERENCE) observability ---------------------------------
 * cellGcmFinish waits for ctrl->ref with EQUALITY. On hardware every fence
 * value persists in the register for the RSX-time until the next fence
 * (~a frame), so an equality poll cannot miss it. Our drain consumes a whole
 * backlog in microseconds; publishing only the final value made intermediate
 * fences invisible -- LBP's boot deadlocked with a thread spinning on a fence
 * the counter had already passed (waiting 0x13 while ref climbed 0x51->0x1F4).
 * Queue every fence as it is drained and publish ONE per tick: the GPU work
 * is never throttled (draw/recycle proceed at full speed), only the ref
 * register advances at most one fence per tick, so every value is observable
 * for >= 16 ms, like hardware. (Throttling the DRAIN at fence boundaries
 * instead starved wrap recycles and wedged the backend -- don't.) */
#define GCM_REF_QLEN 4096
static u32 s_ref_q[GCM_REF_QLEN];
static volatile u32 s_ref_qhead = 0, s_ref_qtail = 0;   /* single producer+consumer: the ticker */

/* A JUMP/CALL whose target has no IO mapping must not be taken. Taking one
 * teleports the walker into unmapped space, where it can never reach `put`
 * again: the FIFO silently stops being processed, no fences are published, and
 * a title spinning on ctrl->ref waits forever with no indication why. Tokyo
 * Jungle did exactly this -- get left the 1 MB ring for IO 0x00F00000, which it
 * never mapped, while put sat at 0x604. Stop at the bad word and say so. */
static void gcm_fifo_bad_branch(const char* kind, u32 target, u32 word)
{
    static int n = 0;
    if (PS3_LOG_FIRST(n, 8))
        fprintf(stderr, "[cellGcmSys] FIFO %s to unmapped IO 0x%08X (word 0x%08X) "
                "-- not taken, drain stops here\n", kind, target, word);
}

/* A bad branch nearly always means the walker mis-counted a method batch a few
 * dwords back and is now reading vertex data -- a float 1.0f is 0x3F800000,
 * which decodes as JUMP 0x1F800000. Dump the run-up: the command that consumed
 * the wrong count is in here. */
/* Last words the walker actually consumed, so a bad branch can show the decode
 * chain that led there rather than raw memory around it. */
#define GCM_TRACE_N 64
static u32 s_tr_off[GCM_TRACE_N], s_tr_w[GCM_TRACE_N];
static u32 s_cf_off[8], s_cf_w[8], s_cf_i;   /* last JUMP/CALL words, for GCM_CIRCLE_DUMP */
static u32 s_tr_i = 0;

/* A branch we cannot take leaves the walker pointing at whatever it
 * mis-decoded, and it never recovers -- get freezes for the rest of the
 * run and nothing renders. Resynchronise to `put` instead, the same trade
 * the unmapped-get path already makes: the commands between here and put
 * are lost, but the next frame is written from a clean boundary and the
 * FIFO lives. */
static void gcm_ref_push_at(u32 v, u32 getoff);

/* ---- SPU-built pushbuffers ------------------------------------------------
 * Some titles do not build the RSX command stream on the PPU at all. The
 * Orange Box is one: its libgcm context points at a 64 KB buffer in the
 * binary's own .bss that is never mapped into IO space, because that buffer is
 * where the PPU records draw commands for an SPU to consume. CB.SPU reads it
 * and DMAs the real pushbuffer into mapped main memory, then `put` is set past
 * the end of what it wrote.
 *
 * So libgcm is never told where the stream is, and the walker -- which starts
 * at IO 0 and has no JUMP to follow, because the SPU side did the binding --
 * scans tens of megabytes of unrelated memory, NOPs through the zeros (a zero
 * word is a valid NOP) and dies on the first vertex float it meets.
 *
 * The DMA is the only evidence there is, so record it. Each PUT that lands in
 * a mapped IO range is a candidate pushbuffer; the one that ends just short of
 * `put` is the block the RSX is actually being pointed at. */
#define GCM_SPU_BLK_N 16
static struct { u32 io_start, io_end; } s_spu_blk[GCM_SPU_BLK_N];
static int s_spu_blk_w = 0;

/* EA -> IO through the mapping list (range lookup; the by-ea helper is exact). */
static u32 gcm_ea2io_range(u32 ea, u32* room)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        IoMapping* m = &s_io_mappings[i];
        if (!m->active || !m->size) continue;
        if (ea >= m->ea && ea < m->ea + m->size) {
            if (room) *room = m->ea + m->size - ea;
            return m->io + (ea - m->ea);
        }
    }
    return 0xFFFFFFFFu;
}

void gcm_forget_spu_blocks(void)
{
    memset(s_spu_blk, 0, sizeof s_spu_blk);
    s_spu_blk_w = 0;
}

void gcm_note_spu_put(u32 ea, u32 size)
{
    /* Before the pushbuffer filters: any SPU write may be a texture's bytes. */
    if (!s_r2rsx) rsx_draw_engine_note_guest_write(ea, size);
    if (!size || size > 0x40000u) return;
    u32 room = 0;
    u32 io = gcm_ea2io_range(ea, &room);
    if (io == 0xFFFFFFFFu || size > room) return;   /* not in RSX-visible memory */

    /* Coalesce with the previous block when an SPU emits one buffer in several
     * transfers -- otherwise only the last fragment is remembered and the
     * walker still starts in the middle of the stream. */
    int prev = (s_spu_blk_w + GCM_SPU_BLK_N - 1) % GCM_SPU_BLK_N;
    if (s_spu_blk[prev].io_end == io) { s_spu_blk[prev].io_end = io + size; return; }

    s_spu_blk[s_spu_blk_w].io_start = io;
    s_spu_blk[s_spu_blk_w].io_end   = io + size;
    s_spu_blk_w = (s_spu_blk_w + 1) % GCM_SPU_BLK_N;
}

/* The recorded block the RSX is being pointed at, or 0xFFFFFFFF.
 *
 * `put` sits at or just past the end of the block the title wants executed --
 * the gap is whatever libgcm appended after the SPU's data, a fence and its
 * argument in this title's case. Require the block to END within a small window
 * before `put` and to START before it, so an older double-buffered block (the
 * SPU alternates between two) is not chosen over the current one. */
static u32 gcm_spu_block_for_put(u32 put)
{
    u32 best = 0xFFFFFFFFu, best_end = 0;
    for (int i = 0; i < GCM_SPU_BLK_N; i++) {
        u32 s = s_spu_blk[i].io_start, e = s_spu_blk[i].io_end;
        if (!e || s >= put || e > put) continue;
        if (put - e > 0x40u) continue;              /* too far back to be current */
        if (e >= best_end) { best_end = e; best = s; }
    }
    return best;
}

/* A resync jumps `get` straight to `put`, which is what keeps the FIFO alive
 * when the walker cannot make progress -- but everything in between is
 * dropped, and if a SET_REFERENCE is in there the cellGcmFinish waiting on it
 * never wakes. Tokyo Jungle lost exactly one that way: two fences in the ring
 * (arg 0 at io 0x09A0, arg 1 at io 0x19E8), the walker parked before the
 * second, and the resync to put=0x19F0 stepped straight over it -- ref stayed
 * 0 while the title spun for ref==1 forever.
 *
 * Dropping DRAWS on a resync is survivable: a frame is lost. Dropping a FENCE
 * is not, it deadlocks the title. Sweep the skipped range first. */
/* Skip-ahead triggers (jump-park after the park grace, circling, stuck-get,
 * unmapped get / branch, recover-storm): a per-kind count, logged on the first
 * and every 256th, so it shows whether they fire in play. */
static void gcm_skip_ahead_note(const char* why)
{
    static const char* kinds[8]; static u32 counts[8]; static u32 total;
    int k = 0;
    for (; k < 8 && kinds[k]; k++) if (strcmp(kinds[k], why) == 0) break;
    if (k == 8) k = 7;
    else if (!kinds[k]) kinds[k] = why;
    const u32 c = ++counts[k];
    total++;
    if (c == 1 || (c & 255u) == 0)
        fprintf(stderr, "[skip-ahead] %s x%u (all kinds %u)\n", why, c, total);
}

static void gcm_fifo_resync_why(const char* why, u32* getoff, u32 put)
{
    gcm_skip_ahead_note(why);
    static int n = 0;
    if (PS3_LOG_FIRST(n, 8))
        fprintf(stderr, "[cellGcmSys] FIFO resync (%s) 0x%08X -> put 0x%08X\n",
                why, *getoff, put);
    /* If an SPU DMA'd a pushbuffer that ends just short of `put`, that is the
     * stream the RSX is being pointed at -- land at its START and walk it,
     * instead of landing on `put` and skipping it. Resyncing to `put` keeps the
     * FIFO alive but drops every command in between, and SET_BEGIN_END is one
     * of them: the draws still arrive, with no primitive type, and are dropped
     * by the backend as prim=0. */
    u32 land = gcm_spu_block_for_put(put);
    if (land == 0xFFFFFFFFu) land = put;
    else { static int b = 0; if (PS3_LOG_FIRST(b, 8))
        fprintf(stderr, "[cellGcmSys] resync landing on SPU pushbuffer "
                "0x%08X (put 0x%08X) instead of skipping to put\n", land, put); }

    /* Sweep only the range still being skipped; the walker will consume the
     * fences inside the block itself, and pushing them here too would publish
     * every one twice. */
    u32 from = (*getoff <= land) ? *getoff : s_last_ring_off;
    if (from < land) {
        unsigned rescued = 0;
        for (u32 io = from; io + 8 <= land; io += 4) {
            u32 ea = gcm_io2ea(io); if (!ea) continue;
            u32 w = vm_read32(ea);
            /* A flip marker skipped here will never be reached: count it, or
             * every flip after it waits for the drain forever. */
            if ((w & 0xFFFFFF00u) == GCM_FLIP_MARKER &&
                s_fifo_flips_reached != s_fifo_flips_queued) { s_fifo_flips_reached++; continue; }
            if ((w >> 29) != 0 || (w & 0x1FFCu) != 0x0050u) continue;
            u32 dea = gcm_io2ea(io + 4); if (!dea) continue;
            gcm_ref_push_at(vm_read32(dea), io);
            rescued++;
        }
        if (rescued) { static int m = 0; if (PS3_LOG_FIRST(m, 8))
            fprintf(stderr, "[cellGcmSys] resync rescued %u fence(s) from"
                    " 0x%08X..0x%08X\n", rescued, from, land); }
    }
    *getoff = land;
}

/* RPCS3 thread::recover_fifo (RSXThread.cpp:1740-1780): back to the restore
 * point with the CALL return saved there, 2 ms for the producer to finish
 * writing, and the draw in flight discarded. RPCS3 throws when 20 recoveries
 * fall inside ~2 s ("Dead FIFO commands queue"); a port cannot stop, so that
 * case resyncs to put instead. */
static void gcm_fifo_recover(const char* what, rsx_state* st, u32 put)
{
    static unsigned long long hist[20]; static int hn = 0, hi = 0;
    extern unsigned long long ps3_ms_now(void);
    const unsigned long long now = ps3_ms_now();
    { static int n = 0; if (PS3_LOG_FIRST(n, 16))
        fprintf(stderr, "[cellGcmSys] FIFO recover (%s) at get 0x%08X -> restore 0x%08X\n",
                what, s_fifo_getoff, s_fifo_restore); }
    int dead = s_fifo_restore == 0xFFFFFFFFu;
    if (hn == 20) {
        if (now - hist[hi] < 2000u) dead = 1;
        hi = (hi + 1) % 20; hn--;
    }
    if (st && st->in_begin_end) {
        st->in_begin_end = 0;
        if (rsx_draw_engine_enabled()) rsx_draw_engine_method(RSX_DSP_DISCARD_DRAW, 0);
    }
    if (dead) {
        hn = 0; hi = 0;
        s_fifo_calloff = GCM_CALL_NONE;
        gcm_fifo_resync_why("recover-storm", &s_fifo_getoff, put);
        s_fifo_restore = 0xFFFFFFFFu; s_fifo_sync_req = 1;
        return;
    }
    s_fifo_getoff = s_fifo_restore;
    s_fifo_calloff = s_fifo_saved_ret;
#ifdef _WIN32
    Sleep(2);
#endif
    hist[(hi + hn) % 20] = now;
    hn++;
}

/* GCM_SEMA_ADDR=0: NV406E semaphores always index the label window, with no
 * address checks (the old walker). */
static int gcm_sema_addr_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GCM_SEMA_ADDR"); on = !(e && atoi(e) == 0); }
    return on;
}
/* RPCS3 rsx_utils.cpp:224-345 get_address for the NV406E semaphore context
 * DMA (0x60): the semaphore handles index the label block (0x1000 bytes), the
 * device handles the device area, anything else as a 2D DMA. 0 = invalid. */
static u32 gcm_sema406e_ea(u32 off)
{
    if (!gcm_sema_addr_on()) return GCM_LABEL_GUEST_BASE + (off & 0xFFFFu);
    switch (s_sema406e_ctx) {
    case 0x66606660u: case 0x66616661u:
        return off < CELL_GCM_MAX_LABEL_COUNT * GCM_LABEL_STRIDE ? GCM_LABEL_GUEST_BASE + off : 0;
    case 0x56616660u: case 0x56616661u:
        return off < 0x100000u ? R2RSX_DEVICE_EA + off : 0;
    default:
        return gcm_dma_ea(s_sema406e_ctx, off);
    }
}
/* GCM_SEMA_ACQUIRE_MS=<ms>: the acquire bound, RPCS3's driver_recovery_timeout
 * (1000000 us by default; 0 = wait for ever). */
static unsigned gcm_sema_acquire_ms(void)
{
    static int ms = -1;
    if (ms < 0) { const char* t = getenv("GCM_SEMA_ACQUIRE_MS"); ms = (t && *t) ? atoi(t) : 1000; if (ms < 0) ms = 0; }
    return (unsigned)ms;
}
/* Does an unsatisfied ACQUIRE at this offset block the walker? Yes, as on the
 * hardware (nv406e.cpp:57-107), unless GCM_SEMA_ACQUIRE=0. The exception is
 * Resistance's flip pacing: the flip waits for label 68 (offset 0x440), which
 * the vblank handler raises every 2nd vblank -- the 30 fps cap PORT_FPS
 * replaces with its own limiter (project/stubs.cpp port_fps_init). Blocking
 * there would pin the game back to 30 fps, so that acquire only blocks under
 * PORT_FPS=orig. GCM_SEMA_ACQUIRE=2 blocks it too. */
static int gcm_sema_acquire_blocks(u32 off)
{
    static int blk = -1, pace_blocks = 0;
    if (blk < 0) {
        const char* e = getenv("GCM_SEMA_ACQUIRE"); blk = (e && *e) ? atoi(e) : 1;
        const char* f = getenv("PORT_FPS");
        pace_blocks = blk >= 2 || (f && (!strcmp(f, "orig") || !strcmp(f, "original") || !strcmp(f, "off") ||
                                         !strcmp(f, "ORIG") || !strcmp(f, "Orig")));
    }
    if (!blk) return 0;
    if (off == 0x440u && !pace_blocks) return 0;
    return 1;
}

static void gcm_fifo_dump_around(u32 getoff)
{
    /* Only ever called on a recovery (bad branch, stuck get), and capped, so
     * it no longer hides behind GCM_RECDBG: the words the walker stopped on
     * are the one thing a run that wedged once in three can still tell us,
     * and GCM_RECDBG is far too loud to leave on for a 15-minute run. */
    static int d = 0;
    if (!PS3_LOG_FIRST(d, 3)) return;
    for (u32 t = 0; t < GCM_TRACE_N; t++) {
        u32 k = (s_tr_i + t) % GCM_TRACE_N;
        if (!s_tr_w[k]) continue;
        u32 w = s_tr_w[k];
        fprintf(stderr, "[FIFOSTEP] io=%08X w=%08X  type=%u method=0x%04X count=%u\n",
                s_tr_off[k], w, w >> 29, w & 0x1FFCu, (w >> 18) & 0x7FFu);
    }
    for (int k = -20; k <= 2; k++) {
        u32 io = getoff + (u32)(k * 4);
        u32 ea = gcm_io2ea(io);
        if (!ea) continue;
        fprintf(stderr, "[FIFODUMP] %+3d io=%08X w=%08X%s\n",
                k, io, vm_read32(ea), k ? "" : "   <-- bad word");
    }
}


static void gcm_ref_push_at(u32 v, u32 getoff)
{
    { static int _d = -1; if (_d < 0) _d = getenv("GCM_REFLOG") ? 1 : 0;
      if (_d) fprintf(stderr, "[refq] drained fence 0x%X at getoff=%08X\n", v, getoff); }
    u32 t = s_ref_qtail;
    if (t - s_ref_qhead >= GCM_REF_QLEN) {   /* overflow: drop oldest (keeps liveness) */
        s_ref_qhead++;
        static int _o = 0;
        if (PS3_LOG_FIRST(_o, 4)) fprintf(stderr, "[cellGcmSys] fence queue overflow -- oldest dropped\n");
    }
    s_ref_q[t % GCM_REF_QLEN] = v;
    s_ref_qtail = t + 1;
}

/* Global fence-publication counter: lets the lwmutex convoy trace correlate a
 * long lock hold with the number of paced fence publications inside it (the
 * 200us pacing x hundreds of one-ahead fences = the ~156ms holds). */
volatile long long g_gcm_ref_pub_count = 0;
/* Absolute microseconds from the QPC hardware counter -- the SAME origin in every
 * translation unit, so two probes in different files can be compared directly.
 * That is the whole point: every wrong conclusion about this stall came from
 * comparing orderings instead of times. */
unsigned long long ps3_qpc_us(void)
{
    static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (unsigned long long)(n.QuadPart / (f.QuadPart / 1000000));
}

static void gcm_ref_publish_one(void)
{
    u32 h = s_ref_qhead;
    if (h == s_ref_qtail) return;
    vm_write32(GCM_CONTROL_GUEST_ADDR + 8, s_ref_q[h % GCM_REF_QLEN]);
    /* stderr, deliberately: [DRAIN] uses printf and stdout is block-buffered
     * when redirected, so its interleaving with an stderr probe is not a
     * timeline. Same stream = same ordering. */
    { static int _rd = -1; if (_rd < 0) _rd = getenv("GCM_REFPUB") ? 1 : 0;
      if (_rd) { static unsigned long _n = 0; if (++_n <= 24)
        fprintf(stderr, "[refpub] t=%lluus #%lu wrote 0x%08X -> readback 0x%08X\n", ps3_qpc_us(), _n,
                s_ref_q[h % GCM_REF_QLEN], vm_read32(GCM_CONTROL_GUEST_ADDR + 8)); } }
    s_ref_qhead = h + 1;
    g_gcm_ref_pub_count++;
}

/* Read-driven fence publication. cellGcmFinish / FIFO-space waits spin-read the
 * ref register (GCM_CONTROL+8). The 60 Hz present-thread ticker publishes only
 * one queued fence per frame, so when present overruns during heavy boot load
 * the tick rate -- and thus fence publication -- collapses and an equality-
 * waiter stalls seconds per fence (the [finspin] boot crawl; the game is not
 * deadlocked, just starved). Let the spinning reader drive publication too:
 * this is called from vm_read32 on each ref read, advancing one queued fence,
 * globally paced to at most one per millisecond so every value stays visible
 * >= 1 ms to any equality-waiter (no skip-past). Decoupled from present, so a
 * wait makes progress even when the ticker is starved. Idempotent + additive:
 * it only ever publishes fences the ticker would eventually publish anyway. */
/* Serializes the FIFO walker between the 60 Hz present thread and the
 * poll-path below. TryAcquire on the poll path avoids re-entrancy (the walker
 * itself performs guest reads) and never blocks a spinning game thread. */
static SRWLOCK s_gcm_fifo_lock = SRWLOCK_INIT;
static void gcm_rsx_process_fifo_unlocked(void);

/* Kick event: a game thread whose fence wait finds the queue DRY signals the
 * present thread to run the FIFO walker NOW instead of on its next 16 ms
 * tick. A movie frame issues ~46 one-ahead cellGcmFinish waits; paying a
 * 16 ms tick per fence was ~0.75 s/frame (the 2 FPS intro). The walker (and
 * the D3D12 backend it feeds) stays on the present thread -- running it from
 * the polling game thread killed the process silently. */
static HANDLE s_gcm_kick_ev = NULL;
void* cellGcm_fifo_kick_event(void)
{
    if (!s_gcm_kick_ev) s_gcm_kick_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    return (void*)s_gcm_kick_ev;
}

/* The present thread's idle wait. It used to be a flat Sleep(4), which meant
 * the kick above was created, signalled -- and waited on by nobody, so a
 * starved guest still paid the full tick. Waiting here instead costs nothing
 * when no one kicks (the timeout is the old sleep) and drains immediately when
 * someone does. */
void cellGcm_fifo_kick_wait(unsigned ms)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GCM_FIFO_KICK"); on = (e && *e == '0') ? 0 : 1; }
    HANDLE ev = on ? (HANDLE)cellGcm_fifo_kick_event() : NULL;
    if (ev) WaitForSingleObject(ev, ms);
    else    Sleep(ms);
}


/* Serializes fence publication between the present ticker and every guest
 * thread poll-reading the ref register (REFPOLL default-on made this path
 * multi-threaded; the SPSC queue head raced and publication order broke --
 * boot froze with all threads parked on fences that were never published in
 * order). TryAcquire: a contended poll just reads the current ref. */
static SRWLOCK s_ref_pub_lock = SRWLOCK_INIT;

void cellGcm_ref_on_poll(void)
{
    if (s_r2rsx) return;                     /* RPCS3 writes ref itself */
    /* Pace: leave each published value observable for >= ~200us of spins
     * (equality-waiters at any poll rate see every value; still ~5000/s so
     * loading's fence storms clear in seconds, not minutes). */
    static LARGE_INTEGER s_freq;
    static volatile LONGLONG s_last_pub;
    if (!TryAcquireSRWLockExclusive(&s_ref_pub_lock))
        return;
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    LONGLONG min_gap = s_freq.QuadPart / 5000;          /* ~200us */
    int dry = 0;
    if (now.QuadPart - s_last_pub >= min_gap) {
        gcm_ref_publish_one();
        s_last_pub = now.QuadPart;
        dry = (s_ref_qhead == s_ref_qtail);
    }
    ReleaseSRWLockExclusive(&s_ref_pub_lock);
    if (dry && s_gcm_kick_ev)
        SetEvent(s_gcm_kick_ev);       /* dry: ask the present thread to walk */
}

static uint64_t s_ft_sema_t0;          /* PS3_FTRACE: when the walker began waiting on the current acquire */

/* Resistance port: lets the host frame loop wait for NEW guest work instead of
 * a fixed Sleep(4) (15.6 ms at the default timer resolution) between drains. */
u32 cellGcm_rsx_put_snapshot(void)
{
    if (!s_gcm_context_ea && !s_gcm_syscall_mode) return 0;
    return vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
}

/* v180: the menus write each frame's quads into one ring at the top of local
 * memory and reuse it the next frame without waiting for the RSX (on hardware
 * it has long consumed them). The walker here lags the PPU, so on the frame a
 * menu inserts a quad, the previous frame read the NEXT frame's shifted
 * vertices -- the static flash after the boot intro. Called by the PPU at the
 * top of its frame (cellPadGetData): wake the walker and wait, bounded, until
 * it has consumed everything submitted so far. */
void cellGcm_wait_fifo_caught_up(unsigned max_ms)
{
    if (s_r2rsx) return;
    if (!s_gcm_context_ea && !s_gcm_syscall_mode) return;
    const u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
    if (s_fifo_getoff == put) return;
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    const LONGLONG lim = f.QuadPart * (LONGLONG)max_ms / 1000;
    if (s_gcm_kick_ev) SetEvent(s_gcm_kick_ev);
    int ok = 0;
    for (;;) {
        if (*(volatile u32*)&s_fifo_getoff == put) { ok = 1; break; }
        QueryPerformanceCounter(&t);
        if (t.QuadPart - t0.QuadPart > lim) break;
        Sleep(0);
    }
    { static unsigned n = 0, nok = 0; static double tot = 0; n++; nok += ok;
      QueryPerformanceCounter(&t); tot += (double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
      if (n <= 5 || (n % 200) == 0)
          fprintf(stderr, "[fifosync] %u waits, %u caught up, avg %.2f ms (last: get=%08X put=%08X)%c",
                  n, nok, tot / n, *(volatile u32*)&s_fifo_getoff, put, 10); }
}

/* v180: the same catch-up at every HLE import a GUEST thread makes, outside
 * gameplay. The menu kicks frame N and goes straight on to writing frame N+1's
 * quads into the one ring (the pad poll comes before the kick, too early);
 * its next import after the kick is where it waits now. Once per new `put`,
 * bounded, and not while the walker is parked on a semaphore the guest has to
 * release. PORT_MENU_FIFO_SYNC=0 off. */
static void gcm_hle_fifo_sync(void)
{
    static int ms = -1;
    if (ms < 0) { const char* e = getenv("PORT_MENU_FIFO_SYNC"); ms = e ? atoi(e) : 25; }
    if (ms <= 0) return;
    extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
    if (!g_active_ctx) return;                       /* the ticker's pump, not a guest thread */
    extern volatile unsigned long long g_port_play_ms;
    extern volatile int g_port_pause_ui;
    /* v181: the front end always; the game only in its pause menu. In-engine
     * cutscenes and loads also leave the look code idle, and waiting there cost
     * York's crater intro ~half the main thread (F11). */
    static int fe = -1;
    if (fe < 0) { char mp[MAX_PATH] = ""; GetModuleFileNameA(NULL, mp, MAX_PATH);
                  const char* b = strrchr(mp, '\\'); b = b ? b + 1 : mp;
                  fe = _stricmp(b, "PortFrontEnd.exe") ? 0 : 1; }
    if (!fe && !g_port_pause_ui) return;
    if (g_port_play_ms && GetTickCount64() - g_port_play_ms <= 200) return;   /* in play */
    if (!s_gcm_context_ea && !s_gcm_syscall_mode) return;
    static volatile u32 s_synced_put = 0xFFFFFFFFu;
    const u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
    if (put == s_synced_put) return;
    s_synced_put = put;
    if (*(volatile int*)&s_sema_blocked_pass) return;
    cellGcm_wait_fifo_caught_up((unsigned)ms);
}

void cellGcm_rsx_process_fifo(void)
{
    if (s_r2rsx) return;                     /* RPCS3's FIFO thread owns get */
    const uint64_t ft_t0 = PS3_FT_ON() ? ps3_ft_now() : 0;
    AcquireSRWLockExclusive(&s_gcm_fifo_lock);
    rsx_draw_engine_wcb_service();
    gcm_rsx_process_fifo_unlocked();
    ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
    if (ft_t0) {
        const uint64_t t1 = ps3_ft_now();
        if (t1 - ft_t0 >= ps3_ft_min_ns())
            ps3_ft_span(PS3_FT_DRAIN, 0, s_ft_sema_t0 ? 1u : 0u, 0, ft_t0, t1);
    }
}

/* GCM_GET_EQ_PUT=1: see where get is published at the end of a pass. */
static int gcm_get_eq_put(void)
{
    static int eq = -1;
    if (eq < 0) { const char* e = getenv("GCM_GET_EQ_PUT"); eq = e ? atoi(e) : 0; }
    return eq;
}

/* v140: FIFO method histogram, armed by F11 for a few seconds after the
 * profile (debugtools.inc): blocks and data words per (subchannel, method),
 * so the next round of decoder work aims at what a frame is made of. */
volatile long g_fifo_mhist_on = 0;
static u32 s_mh_blk[2][0x800], s_mh_dw[2][0x800], s_mh_ni[2][0x800];
static void fifo_mhist_note(u32 subch, u32 method, u32 type, u32 count)
{
    const u32 sc = subch ? 1u : 0u, mi = (method & 0x1FFCu) >> 2;
    s_mh_blk[sc][mi]++; s_mh_dw[sc][mi] += count;
    if (type == 2) s_mh_ni[sc][mi]++;
}
void fifo_mhist_reset(void)
{
    memset(s_mh_blk, 0, sizeof s_mh_blk); memset(s_mh_dw, 0, sizeof s_mh_dw); memset(s_mh_ni, 0, sizeof s_mh_ni);
}
/* Top n by data words into buf: "sc:method blocks words ni" lines. */
int fifo_mhist_report(char* buf, int cap, int n, double secs)
{
    static u32 dw[2][0x800];
    memcpy(dw, s_mh_dw, sizeof dw);
    unsigned long long tot_w = 0, tot_b = 0;
    for (int sc = 0; sc < 2; sc++) for (int k = 0; k < 0x800; k++) { tot_w += s_mh_dw[sc][k]; tot_b += s_mh_blk[sc][k]; }
    int len = snprintf(buf, cap, "  FIFO methods over %.1f s: %llu headers, %llu data words (%.0f words/s)%c"
                       "  subch:method   headers/s   words/s  non-incr%c",
                       secs, tot_b, tot_w, secs > 0 ? tot_w / secs : 0.0, 10, 10);
    for (int r = 0; r < n && len < cap - 128; r++) {
        u32 best = 0; int bs = -1, bk = -1;
        for (int sc = 0; sc < 2; sc++) for (int k = 0; k < 0x800; k++)
            if (dw[sc][k] > best) { best = dw[sc][k]; bs = sc; bk = k; }
        if (bs < 0) break;
        len += snprintf(buf + len, cap - len, "  %u:%04X  %10.0f %10.0f  %u%c", (unsigned)bs, (unsigned)(bk << 2),
                        s_mh_blk[bs][bk] / (secs > 0 ? secs : 1), s_mh_dw[bs][bk] / (secs > 0 ? secs : 1),
                        s_mh_ni[bs][bk], 10);
        dw[bs][bk] = 0;
    }
    return len;
}

/* ---------------------------------------------------------------------------
 * ZCULL reports and conditional rendering: RPCS3's ZCULL_control
 * (RSXZCULL.cpp) and conditional_render_eval, run on the FIFO walker, with
 * RPCS3's settings "Accurate ZCULL stats" on, "Relaxed ZCULL Sync" off and
 * occlusion queries enabled.
 *
 * GET_REPORT does not write when it is issued. ZPASS_PIXEL_CNT ends the
 * running occlusion query and queues the write; the queue retires in order as
 * host queries complete (gcm_zc_update, every walker pass, flushing the GPU
 * when the head is older than 300 us), and a label release forces it
 * (gcm_zc_sync), so the CPU never sees a label before the reports issued
 * ahead of it. The 16-byte report is big-endian {u64 timestamp, u32 value,
 * u32 0}, time-stamped when it retires. Host queries come from the draw
 * engine's backend (rsx_draw_engine_occlusion_*); without one every query
 * reports "visible", like RPCS3's null provider.
 *
 * PS3RECOMP_ZCULL_REPORTS=0   the old immediate write (ZPASS 0xFFFF)
 * PS3RECOMP_ZCULL_QUERIES=0   no host queries: every ZPASS query is visible
 * PS3RECOMP_ZCULL_PRECISE=0   ZPASS writes 0xFFFF for any hit (Accurate ZCULL stats off)
 * PS3RECOMP_ZCULL_RELAXED=1   labels do not wait for reports (Relaxed ZCULL Sync on)
 * PS3RECOMP_ZCULL_CRITICAL=n  2 (default) labels wait only once the guest touched a
 *                             report page (RPCS3 page guard); 1 always; 0 never
 * PS3RECOMP_COND_RENDER=0     SET_RENDER_ENABLE ignored, every draw runs
 * -------------------------------------------------------------------------*/
#define ZC_QUERIES     2048u
#define ZC_WRITES      4096u
#define ZC_STATS       8192u
#define ZC_SAFE_DEPTH  1792u
#define ZC_TICK_US     100u
#define ZC_DELAY_US    300u

typedef struct {
    u64 result, ts_us;
    u32 draws, sync_tag;
    u8  active, pending, host, have;
} gcm_zc_query;

typedef struct {
    int q;                       /* query index, -1 = none          */
    u32 sink, type, tag;         /* sink 0 = not claimed yet         */
    int forwarder;
} gcm_zc_write;

static struct {
    int  inited, on, queries, precise, relaxed, cond;
    int  zcull_en, zpass_en, stats_en, surface_active, host_active;
    int  cur;
    u32  zeta_off;
    u32  zeta_dma, surf_fmt;     /* 0x198 / 0x208, for the surface-active test */
    int  strict_active, surf_dirty;
    /* RSXGraphEngine get_framebuffer_layout inputs (depth_buffer_unused) */
    int  depth_use, fb_ctx, contested;
    u32  depth_test, depth_mask, depth_func, stencil_test, stencil_mask, two_sided;
    u32  bounds_test, bounds_min, bounds_max, zpitch, scissor_h;
    u32  surf_reg[(0x2C0u - 0x180u) >> 2];
    u32  timer, sync_tag;
    u64  next_tick_us;
    gcm_zc_query q[ZC_QUERIES];
    u32  free_q[ZC_QUERIES], nfree;
    gcm_zc_write w[ZC_WRITES];
    u32  wh, wn;
    struct { u64 result; u32 flags; } stat[ZC_STATS];
    u32  stat_tag;
    /* conditional rendering */
    int  cr_enabled, cr_failed, cr_pending, skip_clause;
    u32  cr_addr;
    volatile LONG sync_req;      /* an HLE read wants the queue flushed */
} s_zc;

static int gcm_zc_env(const char* n, int def)
{
    const char* e = getenv(n);
    return (e && *e) ? atoi(e) : def;
}

static void gcm_zcp_init(void);

static void gcm_zc_init(void)
{
    if (s_zc.inited) return;
    s_zc.inited   = 1;
    s_zc.on       = gcm_zc_env("PS3RECOMP_ZCULL_REPORTS", 1) != 0;
    s_zc.queries  = gcm_zc_env("PS3RECOMP_ZCULL_QUERIES", 1) != 0;
    s_zc.precise  = gcm_zc_env("PS3RECOMP_ZCULL_PRECISE", 1) != 0;
    s_zc.relaxed  = gcm_zc_env("PS3RECOMP_ZCULL_RELAXED", 0) != 0;
    s_zc.cond     = gcm_zc_env("PS3RECOMP_COND_RENDER", 1) != 0;
    s_zc.zcull_en = 1;           /* NV4097_SET_ZCULL_EN resets to 3 */
    s_zc.cur      = -1;
    /* GCM_ZC_STRICT_ACTIVE=0: the old offset-only surface-active test,
     * recomputed on every zpass/stats/enable change. */
    s_zc.strict_active = gcm_zc_env("GCM_ZC_STRICT_ACTIVE", 1) != 0;
    s_zc.zeta_dma = 0xFEED0000u; /* rsx_methods.cpp init: CONTEXT_DMA_ZETA */
    s_zc.surf_fmt = 0x121u;      /* rsx_methods.cpp init: SURFACE_FORMAT */
    s_zc.surf_dirty = 1;
    /* GCM_ZC_DEPTH_USE=0: zeta always counts as bound (no clear/draw context). */
    s_zc.depth_use = gcm_zc_env("GCM_ZC_DEPTH_USE", 1) != 0;
    s_zc.fb_ctx = -1;
    s_zc.depth_mask = 1; s_zc.depth_func = 0x201u; s_zc.stencil_mask = 0xFFu;
    s_zc.bounds_max = 0x3F800000u; s_zc.zpitch = 64; s_zc.scissor_h = 4096u << 16;
    s_zc.surf_reg[(0x198u - 0x180u) >> 2] = 0xFEED0000u;
    s_zc.surf_reg[(0x208u - 0x180u) >> 2] = 0x121u;
    s_zc.surf_reg[(0x22Cu - 0x180u) >> 2] = 64u;
    gcm_zcp_init();
    for (u32 i = ZC_QUERIES; i-- > 0;) s_zc.free_q[s_zc.nfree++] = i;
    fprintf(stderr, "[zcull] reports %s, host queries %s, %s, %s sync, conditional render %s%c",
            s_zc.on ? "deferred" : "immediate", s_zc.queries ? "on" : "off",
            s_zc.precise ? "precise" : "binary", s_zc.relaxed ? "relaxed" : "strict",
            s_zc.cond ? "on" : "off", 10);
}

static u64 gcm_zc_now_us(void) { return get_timestamp_ns() / 1000u; }

/* ---- critical reports: RPCS3 on_report_enqueued / on_report_completed /
 * on_access_violation / disable_optimizations. A queued report's page is
 * made inaccessible; while the guest never touches it the reports are not
 * critical and labels do not wait for them. The first guest access to a page
 * with reports in flight makes its location (local / main) critical for
 * good, unguards it and asks for a sync (pipe_flush_interrupt). The walker's
 * own report accesses open the page around the access (RPCS3 writes through
 * an unguarded mapping).
 * PS3RECOMP_ZCULL_CRITICAL=2 guard pages (RPCS3), 1 always critical, 0 never. */
#define ZCP_PAGES 256u

typedef struct { u32 page, refs; int prot; } gcm_zcp_page;

static struct {
    CRITICAL_SECTION cs;
    int  mode, cs_ready, accessed[2];
    gcm_zcp_page pg[2][ZCP_PAGES];
    u32  npg[2];
    volatile LONG critical;
    PVOID veh;
} s_zcp;

static int gcm_zcp_loc(u32 a)
{
    const int local = (a >= s_config.localAddress && a - s_config.localAddress < s_config.localSize) ||
                      (a >= GCM_REPORT_GUEST_BASE && a - GCM_REPORT_GUEST_BASE < CELL_GCM_MAX_REPORT_COUNT * 16u);
    return local ? 0 : 1;
}

static int gcm_zcp_prot(u32 page, int noaccess)
{
    /* Through the shared guard registry: a page a WCB surface also guards
     * stays guarded when the report side lifts its own. */
    return rsx_page_guard_set(page, 1u, noaccess);
}

static gcm_zcp_page* gcm_zcp_find(int loc, u32 page)
{
    for (u32 i = 0; i < s_zcp.npg[loc]; i++)
        if (s_zcp.pg[loc][i].page == page) return &s_zcp.pg[loc][i];
    return NULL;
}

static void gcm_zcp_erase(int loc, gcm_zcp_page* p)
{
    *p = s_zcp.pg[loc][--s_zcp.npg[loc]];
}

/* Under cs. */
static void gcm_zcp_disable(int loc)
{
    if (!s_zcp.accessed[loc])
        fprintf(stderr, "[zcull] reports area (%s) accessed: ZCULL optimizations disabled%c",
                loc ? "main" : "local", 10);
    s_zcp.accessed[loc] = 1;
    for (u32 i = 0; i < s_zcp.npg[loc]; i++) {
        gcm_zcp_page* p = &s_zcp.pg[loc][i];
        if (p->prot) gcm_zcp_prot(p->page, 0);
        s_zcp.critical += (LONG)p->refs;
    }
    s_zcp.npg[loc] = 0;
}

static LONG WINAPI gcm_zcp_veh(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t a = (uintptr_t)er->ExceptionInformation[1];
    if (a < (uintptr_t)vm_base || (u64)(a - (uintptr_t)vm_base) > 0xFFFFFFFFull)
        return EXCEPTION_CONTINUE_SEARCH;
    const u32 page = (u32)(a - (uintptr_t)vm_base) & ~(VM_PAGE_SIZE - 1u);
    int handled = 0, flush = 0;
    EnterCriticalSection(&s_zcp.cs);
    for (int loc = 0; loc < 2 && !handled; loc++) {
        gcm_zcp_page* p = gcm_zcp_find(loc, page);
        if (!p || !p->prot) continue;
        handled = 1;
        if (p->refs) { gcm_zcp_disable(loc); flush = 1; }       /* R/W to an active block */
        else { gcm_zcp_prot(page, 0); gcm_zcp_erase(loc, p); }  /* stale block            */
    }
    if (!handled) {
        /* Unguarded by the walker between the fault and here: retry. */
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(vm_base + page, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
            mbi.Protect == PAGE_READWRITE) {
            for (int loc = 0; loc < 2; loc++)
                if (s_zcp.accessed[loc] || s_zcp.npg[loc]) handled = 1;
        }
    }
    LeaveCriticalSection(&s_zcp.cs);
    if (!handled) return EXCEPTION_CONTINUE_SEARCH;
    if (flush) {
        InterlockedExchange(&s_zc.sync_req, 1);
        if (s_gcm_kick_ev) SetEvent(s_gcm_kick_ev);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void gcm_zcp_init(void)
{
    s_zcp.mode = gcm_zc_env("PS3RECOMP_ZCULL_CRITICAL", 2);
    InitializeCriticalSection(&s_zcp.cs);
    s_zcp.cs_ready = 1;
}

static int gcm_zc_critical(void)
{
    if (s_zcp.mode == 1) return 1;
    if (s_zcp.mode == 0) return 0;
    return s_zcp.critical > 0;
}

static void gcm_zcp_enqueued(u32 sink)
{
    if (s_zcp.mode != 2 || !s_zcp.cs_ready) return;
    const int loc = gcm_zcp_loc(sink);
    const u32 page = sink & ~(VM_PAGE_SIZE - 1u);
    EnterCriticalSection(&s_zcp.cs);
    if (s_zcp.accessed[loc]) {
        s_zcp.critical++;
    } else {
        gcm_zcp_page* p = gcm_zcp_find(loc, page);
        if (!p && s_zcp.npg[loc] < ZCP_PAGES) {
            p = &s_zcp.pg[loc][s_zcp.npg[loc]++];
            p->page = page; p->refs = 0; p->prot = 0;
        }
        if (!p) {
            gcm_zcp_disable(loc);
            s_zcp.critical++;
        } else {
            p->refs++;
            if (!p->prot) {
                if (!s_zcp.veh) s_zcp.veh = AddVectoredExceptionHandler(1, gcm_zcp_veh);
                if (s_zcp.veh && gcm_zcp_prot(page, 1)) p->prot = 1;
                else gcm_zcp_disable(loc);       /* cannot guard it: the guest may be reading */
            }
        }
    }
    LeaveCriticalSection(&s_zcp.cs);
}

/* The walker's own report access (RPCS3's unguarded mapping). With
 * `complete`, a queued report retires (on_report_completed) and the page is
 * left open once nothing is in flight on it. store: 4 words, NULL = read. */
/* The always-writable second view of guest memory (vm_sudo_base, set up by
 * the host; RPCS3 g_sudo_addr). Report traffic goes through it so the guarded
 * page never opens to the guest's threads, not even briefly. NULL (no host
 * view, or RSX_ENG_SUDO=0): lift the guards around the access instead. */
extern u8* vm_sudo_base __attribute__((weak));
static u8* gcm_zcp_sudo(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_ENG_SUDO"); on = !(e && *e == '0'); }
    return on && &vm_sudo_base && vm_base ? vm_sudo_base : NULL;
}

static u32 gcm_zcp_access(u32 addr, const u32* store, int complete)
{
    const int guard = s_zcp.mode == 2 && s_zcp.cs_ready;
    u8* const sudo = guard ? gcm_zcp_sudo() : NULL;
    gcm_zcp_page* p = NULL;
    int loc = 0;
    if (guard) {
        EnterCriticalSection(&s_zcp.cs);
        loc = gcm_zcp_loc(addr);
        p = gcm_zcp_find(loc, addr & ~(VM_PAGE_SIZE - 1u));
        if (complete) {
            if (s_zcp.accessed[loc]) s_zcp.critical--;
            else if (p && p->refs) p->refs--;
        }
        if (p && p->prot && !sudo) gcm_zcp_prot(p->page, 0);
    }
    u32 r = 0;
    if (sudo) {
        if (store) {
            u32 raw[4];
            for (u32 i = 0; i < 4u; i++) raw[i] = ps3_bswap32(store[i]);
            if (spu_coh_is_reserved(addr)) {
                spu_lockline_lock();
                memcpy(sudo + addr, raw, sizeof raw);
                spu_coh_notify_write(addr);
                spu_lockline_unlock();
            } else {
                memcpy(sudo + addr, raw, sizeof raw);
            }
            if (g_resv_store_active > 0)
                for (u32 i = 0; i < 4u; i++) ppu_resv_break_store(addr + i * 4u);
        } else {
            u32 raw;
            memcpy(&raw, sudo + addr, sizeof raw);
            r = ps3_bswap32(raw);
        }
    } else {
        if (guard) rsx_page_guard_hold(addr & ~(VM_PAGE_SIZE - 1u), 1);   /* other owners' guards too */
        if (store) { for (u32 i = 0; i < 4u; i++) vm_write32(addr + i * 4u, store[i]); }
        else r = vm_read32(addr);
        if (guard) rsx_page_guard_hold(addr & ~(VM_PAGE_SIZE - 1u), -1);
    }
    if (guard) {
        if (p && p->prot) {
            if (p->refs) { if (!sudo) gcm_zcp_prot(p->page, 1); }
            else { if (sudo) gcm_zcp_prot(p->page, 0); gcm_zcp_erase(loc, p); }
        }
        LeaveCriticalSection(&s_zcp.cs);
    }
    return r;
}

/* Where a report offset lands for the bound report DMA (RPCS3 get_address):
 * 0xBAD68000 = IO 0x0E000000 + off, 0x66626660 = the report area the
 * cellGcmGetReportData* calls hand out, 0xFEED0000 = local memory,
 * 0xFEED0001 = IO offset. 0 = nowhere the guest can read. */
static u32 gcm_zc_sink(u32 off)
{
    u32 ea;
    if (gcm_report_dma_ea(s_report_dma, off, &ea)) return ea;
    switch (s_report_dma) {
    case 0xFEED0000u: return cellGcmResolveLocated(1, off);
    case 0xFEED0001u: return gcm_io2ea(off);
    default:          return 0;
    }
}

static void gcm_zc_store(u32 sink, u64 ts, u32 value, int complete)
{
    const u32 words[4] = { (u32)(ts >> 32), (u32)ts, value, 0u };
    gcm_zcp_access(sink, words, complete);
    if (sink >= GCM_REPORT_GUEST_BASE && sink < GCM_REPORT_GUEST_BASE + CELL_GCM_MAX_REPORT_COUNT * 16u) {
        const u32 idx = (sink - GCM_REPORT_GUEST_BASE) / 16u;
        s_report_data[idx].timestamp = ts;
        s_report_data[idx].value = value;
    }
}

/* ZCULL_control::write: the value a report type carries. */
static void gcm_zc_write_mem(u32 sink, u32 type, u64 value64, int complete)
{
    u32 value = value64 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)value64;
    switch (type) {
    case 1u:                      /* ZPASS_PIXEL_CNT; resolution scale 100% */
        if (value) value = s_zc.precise ? value : 0xFFFFu;
        break;
    case 5u:                      /* ZCULL_STATS3 */
        value = (value || !s_zc.surface_active || !s_zc.stats_en) ? 0u : 0xFFFFu;
        break;
    default:                      /* ZCULL_STATS, STATS1, STATS2: not implemented */
        value = (s_zc.surface_active && s_zc.stats_en) ? 0xFFFFFFFFu : 0u;
        break;
    }
    gcm_zc_store(sink, gcm_report_timestamp(), value, complete);
}

/* ---- host queries ---- */
static void gcm_zc_q_begin(int i)
{
    s_zc.q[i].host = s_zc.queries && rsx_draw_engine_enabled() &&
                     rsx_draw_engine_occlusion_begin((u32)i);
}
static void gcm_zc_q_end(int i)
{
    if (s_zc.q[i].host) rsx_draw_engine_occlusion_end((u32)i);
}
static int gcm_zc_q_check(int i)
{
    gcm_zc_query* q = &s_zc.q[i];
    if (!q->host || q->have) return 1;
    u64 r = 0;
    if (!rsx_draw_engine_occlusion_result((u32)i, 0, &r)) return 0;
    q->result = r; q->have = 1;
    return 1;
}
static void gcm_zc_q_read(int i)
{
    gcm_zc_query* q = &s_zc.q[i];
    if (q->have) return;
    u64 r = 0xFFFFu;              /* the null provider's "visible" */
    if (q->host && !rsx_draw_engine_occlusion_result((u32)i, 1, &r)) r = 0xFFFFu;
    q->result = r; q->have = 1;
}
static void gcm_zc_q_discard(int i)
{
    if (s_zc.q[i].host && !s_zc.q[i].have) rsx_draw_engine_occlusion_discard((u32)i);
    s_zc.q[i].host = 0;
}
static void gcm_zc_q_free(int i)
{
    s_zc.q[i].pending = 0;
    s_zc.q[i].active = 0;
    s_zc.free_q[s_zc.nfree++] = (u32)i;
}

/* sync_hint(zcull_sync) for query `qi`: RPCS3's VKGSRender::on_sync_hint
 * submits the command buffer when that query is still in it (no wait), and
 * ZCULL_control::on_sync_hint raises m_sync_tag to the query's tag. */
static void gcm_zc_kick(int qi)
{
    if (qi < 0) return;
    if (s_zc.q[qi].host && rsx_draw_engine_enabled()) rsx_draw_engine_occlusion_kick((u32)qi);
    if (s_zc.q[qi].sync_tag > s_zc.sync_tag) s_zc.sync_tag = s_zc.q[qi].sync_tag;
}

static gcm_zc_write* gcm_zc_w(u32 k) { return &s_zc.w[(s_zc.wh + k) % ZC_WRITES]; }

static void gcm_zc_pop_front(u32 n)
{
    s_zc.wh = (s_zc.wh + n) % ZC_WRITES;
    s_zc.wn -= n;
}

/* ZCULL_control::retire (CPU-evaluated conditional rendering reads memory). */
static void gcm_zc_retire(const gcm_zc_write* w, u64 result)
{
    if (!w->forwarder)
        gcm_zc_write_mem(w->sink, w->type, w->type == 1u ? s_zc.stat[w->tag].result : result, 1);
}

/* ZCULL_control::update. sync_addr: retire up to and including the write to
 * that address, waiting for its queries. */
static void gcm_zc_update(u32 sync_addr, int hint)
{
    if (!s_zc.wn) return;
    const gcm_zc_write* front = gcm_zc_w(0);
    if (!front->sink) return;
    if (!sync_addr) {
        if (hint || s_zc.wn >= ZC_SAFE_DEPTH) {
            for (u32 k = s_zc.wn; k-- > 0;) {
                const gcm_zc_write* w = gcm_zc_w(k);
                if (w->q < 0) continue;
                if (s_zc.q[w->q].draws && s_zc.q[w->q].sync_tag > s_zc.sync_tag) gcm_zc_kick(w->q);
                break;
            }
        }
        const u64 now = gcm_zc_now_us();
        if (now < s_zc.next_tick_us) return;
        s_zc.next_tick_us = now + ZC_TICK_US;
        if (!s_zc.relaxed && gcm_zc_critical() && front->q >= 0 && s_zc.q[front->q].draws &&
            s_zc.q[front->q].sync_tag > s_zc.sync_tag) {
            if (now - s_zc.q[front->q].ts_us > ZC_DELAY_US) gcm_zc_kick(front->q);
            return;
        }
    }
    u32 processed = 0;
    for (u32 k = 0; k < s_zc.wn; k++) {
        gcm_zc_write* w = gcm_zc_w(k);
        if (!w->sink) break;
        const int force = sync_addr != 0;
        if (force && w->sink == sync_addr && !w->forwarder) sync_addr = 0;
        if (w->q >= 0) {
            gcm_zc_query* q = &s_zc.q[w->q];
            const int implemented = w->type == 1u || w->type == 5u;
            const int have_result = s_zc.stat[w->tag].result && !s_zc.precise;
            if (!implemented || !q->draws || have_result) gcm_zc_q_discard(w->q);
            else if (force || gcm_zc_q_check(w->q)) {
                gcm_zc_q_read(w->q);
                s_zc.stat[w->tag].result += q->result;
            } else break;
            gcm_zc_q_free(w->q);
            w->q = -1;
        }
        s_zc.stat[w->tag].flags = 0;
        gcm_zc_retire(w, s_zc.stat[w->tag].result);
        processed++;
    }
    gcm_zc_pop_front(processed);
}

/* ZCULL_control::sync: every claimed report is written now. */
static void gcm_zc_sync_x(int force)
{
    if (!s_zc.wn) return;
    if (!force && !gcm_zc_critical()) return;      /* nothing important queued up */
    if (!force && s_zc.relaxed) { gcm_zc_update(0, 1); return; }
    for (u32 k = s_zc.wn; k-- > 0;) {
        const gcm_zc_write* w = gcm_zc_w(k);
        if (w->sink && w->q >= 0 && s_zc.q[w->q].draws) {
            if (s_zc.q[w->q].sync_tag > s_zc.sync_tag) gcm_zc_kick(w->q);
            break;
        }
    }
    u32 processed = 0;
    for (u32 k = 0; k < s_zc.wn; k++) {
        gcm_zc_write* w = gcm_zc_w(k);
        if (!w->sink) break;
        if (w->q >= 0) {
            gcm_zc_query* q = &s_zc.q[w->q];
            const int implemented = w->type == 1u || w->type == 5u;
            const int have_result = s_zc.stat[w->tag].result && !s_zc.precise;
            if (implemented && !have_result && q->draws) {
                gcm_zc_q_read(w->q);
                s_zc.stat[w->tag].result += q->result;
            } else gcm_zc_q_discard(w->q);
            gcm_zc_q_free(w->q);
            w->q = -1;
        }
        gcm_zc_retire(w, s_zc.stat[w->tag].result);
        processed++;
    }
    gcm_zc_pop_front(processed);
    /* Release every statistics slot but the current one. */
    const u32 cur = s_zc.stat_tag;
    for (u32 i = (cur + ZC_STATS - 1u) % ZC_STATS; i != cur; i = (i + ZC_STATS - 1u) % ZC_STATS) {
        if (!s_zc.stat[i].flags) break;
        s_zc.stat[i].flags = 0;
    }
}

/* ZCULL_control::sync (force: a full queue drains regardless). */
static void gcm_zc_sync(void) { gcm_zc_sync_x(0); }

static gcm_zc_write* gcm_zc_push(void)
{
    if (s_zc.wn >= ZC_WRITES) {
        gcm_zc_sync_x(1);
        if (s_zc.wn >= ZC_WRITES) return NULL;
    }
    gcm_zc_write* w = gcm_zc_w(s_zc.wn++);
    w->q = -1; w->sink = 0; w->type = 0; w->tag = 0; w->forwarder = 0;
    return w;
}

static int gcm_zc_alloc(void)
{
    for (int tries = 0; tries < 2 && !s_zc.nfree; tries++) {
        s_zc.next_tick_us = 0;
        if (s_zc.wn && gcm_zc_w(0)->sink) gcm_zc_update(gcm_zc_w(0)->sink, 0);
        else gcm_zc_sync_x(1);
    }
    if (!s_zc.nfree) return -1;
    const int i = (int)s_zc.free_q[--s_zc.nfree];
    gcm_zc_query* q = &s_zc.q[i];
    memset(q, 0, sizeof *q);
    q->active = 1;
    return i;
}

/* A query that ended with draws waits in the queue for a report to claim it. */
static void gcm_zc_enqueue_current(void)
{
    const int i = s_zc.cur;
    gcm_zc_q_end(i);
    s_zc.q[i].active = 0;
    s_zc.q[i].pending = 1;
    s_zc.q[i].sync_tag = ++s_zc.timer;
    s_zc.q[i].ts_us = gcm_zc_now_us();
    gcm_zc_write* w = gcm_zc_push();
    if (w) w->q = i;
    else { gcm_zc_q_discard(i); gcm_zc_q_free(i); }
}

static void gcm_zc_set_active(int state)
{
    if (state == s_zc.host_active) return;
    s_zc.host_active = state;
    if (state) {
        s_zc.cur = gcm_zc_alloc();
        if (s_zc.cur >= 0) gcm_zc_q_begin(s_zc.cur);
        return;
    }
    if (s_zc.cur >= 0) {
        if (s_zc.q[s_zc.cur].draws) gcm_zc_enqueue_current();
        else {
            gcm_zc_q_end(s_zc.cur);
            gcm_zc_q_discard(s_zc.cur);
            gcm_zc_q_free(s_zc.cur);
        }
    }
    s_zc.cur = -1;
    gcm_zc_update(0, 0);
}

/* thread::check_zcull_status + ZCULL_control::check_state. */
static void gcm_zc_check_state(void)
{
    if (!s_zc.strict_active) {
        int active = 0;
        for (u32 i = 0; i < CELL_GCM_MAX_ZCULL_COUNT; i++)
            if (s_zcull[i].bound && s_zcull[i].offset == s_zc.zeta_off) { active = 1; break; }
        s_zc.surface_active = active;
    }
    if (s_zc.host_active && !s_zc.zpass_en) gcm_zc_set_active(0);
    else if (!s_zc.host_active && s_zc.zpass_en && s_zc.zcull_en) gcm_zc_set_active(1);
}

static int gcm_zc_stencil_on(void)
{
    return s_zc.stencil_test && ((s_zc.surf_fmt >> 5) & 7u) == 2u;
}

/* RSXGraphEngine.cpp get_framebuffer_layout: zeta_address = 0 for a colour-
 * only clear, a draw that never touches depth/stencil memory, or a zeta pitch
 * that cannot hold the clipped surface. ctx: 0 draw, 1 colour, 2 depth, 3 all. */
static int gcm_zc_depth_unused(int ctx)
{
    if (ctx == 1) return 1;
    if (ctx == 0 && !gcm_zc_stencil_on()) {
        int unused = !s_zc.depth_test ||
                     (!s_zc.depth_mask && (s_zc.depth_func == 0x200u || s_zc.depth_func == 0x207u));
        if (unused && s_zc.bounds_test) {
            float mn, mx;
            memcpy(&mn, &s_zc.bounds_min, 4); memcpy(&mx, &s_zc.bounds_max, 4);
            if (mn > 0.f || mx < 1.f) unused = 0;
        }
        if (unused) { s_zc.contested = 1; return 1; }
    }
    if (((s_zc.surf_fmt >> 8) & 0xFu) != 2u) {      /* not swizzled */
        const u32 aa = (s_zc.surf_fmt >> 12) & 0xFu;
        const u32 texel = ((((s_zc.surf_fmt >> 5) & 7u) == 1u) ? 2u : 4u) * (aa ? 2u : 1u);
        u32 lim = s_zc.surf_reg[(0x200u - 0x180u) >> 2] >> 16;   /* clip width */
        const u32 sc = s_zc.scissor_h, se = (sc & 0xFFFFu) + (sc >> 16);
        if (se < lim) lim = se;
        if (s_zc.zpitch < texel * lim && s_zc.zpitch <= 64u) return 1;
    }
    return 0;
}

/* RPCS3 RSXThread.cpp:1498-1531 check_zcull_status(framebuffer_swap), from
 * graph_engine::bind_framebuffer: re-evaluated when the framebuffer context
 * (clear / draw) changes or a surface register changed. It needs a bound
 * region whose depth format and AA mode match the surface and whose local
 * offset is the (used) zeta surface's local address. */
static void gcm_zc_fb_swap(int ctx)
{
    if (!s_zc.strict_active) return;
    if (s_zc.depth_use) {
        if (ctx == s_zc.fb_ctx && !s_zc.surf_dirty) return;
        s_zc.fb_ctx = ctx;
        s_zc.contested = 0;
    } else if (!s_zc.surf_dirty) return;
    s_zc.surf_dirty = 0;
    int active = 0;
    const u32 zfmt = (s_zc.surf_fmt >> 5) & 7u, aa = (s_zc.surf_fmt >> 12) & 0xFu;
    if (s_zc.zeta_dma != 0xFEED0001u && !(s_zc.depth_use && gcm_zc_depth_unused(ctx)))
        for (u32 i = 0; i < CELL_GCM_MAX_ZCULL_COUNT; i++)
            if (s_zcull[i].bound && s_zcull[i].zFormat == zfmt && s_zcull[i].aaFormat == aa &&
                s_zcull[i].offset == s_zc.zeta_off) { active = 1; break; }
    s_zc.surface_active = active;
    gcm_zc_check_state();
}

/* ZCULL_control::read_report. */
static void gcm_zc_read_report(u32 sink, u32 type)
{
    if (s_zc.cur >= 0 && type == 1u) {
        gcm_zc_enqueue_current();
        s_zc.cur = gcm_zc_alloc();
        if (s_zc.cur >= 0) gcm_zc_q_begin(s_zc.cur);
    } else {
        if (!s_zc.wn) {
            gcm_zc_write_mem(sink, type, s_zc.stat[s_zc.stat_tag].result, 0);
            return;
        }
        if (!gcm_zc_push()) { gcm_zc_write_mem(sink, type, s_zc.stat[s_zc.stat_tag].result, 0); return; }
    }
    if (!s_zc.wn) return;
    const u32 last = s_zc.wn - 1u;
    s_zc.stat[s_zc.stat_tag].flags |= 1u;
    int claimed = 0;
    for (u32 k = s_zc.wn; k-- > 0;) {
        gcm_zc_write* w = gcm_zc_w(k);
        if (w->sink) break;
        w->tag = s_zc.stat_tag;
        w->sink = sink;
        w->type = type;
        /* Not the last in the chain: truncated queries forward their count
         * to the last writer. */
        w->forwarder = k != last;
        if (k == last) claimed = 1;
    }
    if (claimed) gcm_zcp_enqueued(sink);
    if (s_zc.stat[s_zc.stat_tag].result) gcm_zc_update(0, 0);
}

/* ZCULL_control::clear. */
static void gcm_zc_clear(u32 type)
{
    if (!(type & 1u)) return;
    if (s_zc.cur >= 0 && s_zc.q[s_zc.cur].active) {
        gcm_zc_q_end(s_zc.cur);
        gcm_zc_q_discard(s_zc.cur);
        gcm_zc_q_free(s_zc.cur);
        s_zc.cur = gcm_zc_alloc();
        if (s_zc.cur >= 0) gcm_zc_q_begin(s_zc.cur);
    }
    while (s_zc.wn && !gcm_zc_w(s_zc.wn - 1u)->sink) {
        gcm_zc_write* w = gcm_zc_w(s_zc.wn - 1u);
        if (w->q >= 0) { gcm_zc_q_discard(w->q); gcm_zc_q_free(w->q); }
        s_zc.wn--;
    }
    if (!s_zc.wn) s_zc.stat[s_zc.stat_tag].flags = 0;
    if (s_zc.stat[s_zc.stat_tag].flags) s_zc.stat_tag = (s_zc.stat_tag + 1u) % ZC_STATS;
    s_zc.stat[s_zc.stat_tag].result = 0;
}

static void gcm_zc_on_draw(void)
{
    if (s_zc.cur >= 0) {
        s_zc.q[s_zc.cur].draws++;
        s_zc.q[s_zc.cur].sync_tag = ++s_zc.timer;
    }
}

/* thread::enable_conditional_rendering / disable_conditional_rendering. The
 * evaluation is on the CPU: a report still queued forces the queue up to it
 * (at the first draw that needs the answer); otherwise memory decides now. */
static void gcm_zc_set_render_enable(u32 arg)
{
    const u32 mode = arg >> 24;
    if (mode == 1u) { s_zc.cr_enabled = 0; s_zc.cr_failed = 0; s_zc.cr_pending = 0; return; }
    if (mode != 2u) return;
    const u32 sink = gcm_zc_sink(arg & 0xFFFFFFu);
    if (!sink) { static int n = 0; if (PS3_LOG_FIRST(n, 4)) printf("[zcull] SET_RENDER_ENABLE 0x%08X: no address%c", arg, 10); return; }
    s_zc.cr_enabled = 1;
    s_zc.cr_failed = 0;
    s_zc.cr_pending = 0;
    s_zc.cr_addr = sink;
    /* ZCULL_control::find_query */
    int found = 0;
    u32 stat_id = 0xFFFFFFFFu;
    for (u32 k = s_zc.wn; k-- > 0;) {
        const gcm_zc_write* w = gcm_zc_w(k);
        if (stat_id != 0xFFFFFFFFu) {
            if (w->tag != stat_id) {
                if (found) break;
                s_zc.cr_failed = 1;        /* stats cleared in between: the result is 0 */
                return;
            }
            if (w->q >= 0 && s_zc.q[w->q].draws) found = 1;
        } else if (w->sink == sink) {
            if (w->q >= 0 && s_zc.q[w->q].draws) found = 1;
            stat_id = w->tag;
        }
    }
    if (found) { s_zc.cr_pending = 1; return; }
    s_zc.cr_failed = gcm_zcp_access(sink + 8u, NULL, 0) == 0u;
}

static int gcm_zc_render_disabled(void)
{
    if (!s_zc.cond || !s_zc.cr_enabled) return 0;
    if (s_zc.cr_pending) {
        s_zc.cr_pending = 0;
        gcm_zc_update(s_zc.cr_addr, 0);
        s_zc.cr_failed = gcm_zcp_access(s_zc.cr_addr + 8u, NULL, 0) == 0u;
    }
    return s_zc.cr_failed;
}

/* Before a label write the CPU can see (NV406E release, back-end release):
 * RPCS3 write_gcm_label -> thread::sync -> ZCULL_control::sync. */
static void gcm_zc_label_sync(void)
{
    if (s_zc.on && s_zc.wn) gcm_zc_sync();
}

/* Walker pre-hook for an NV4097 method, ahead of the draw engine and
 * rsx_process_method. 1 = drop the method (a draw conditional rendering
 * skips). */
static void gcm_zc_option(u32* field, u32 v)
{
    if (field) *field = v;
    if (s_zc.contested && (s_zc.depth_test || gcm_zc_stencil_on())) s_zc.surf_dirty = 1;
}

static int gcm_zc_pre(u32 m, u32 v)
{
    gcm_zc_init();
    if (!s_zc.on) return 0;
    const u32 mm = m & 0x1FFCu;
    if (s_zc.depth_use) {
        if (mm >= 0x180u && mm < 0x2C0u) {
            switch (mm) {
            case 0x18Cu: case 0x194u: case 0x198u: case 0x1B4u: case 0x1B8u:
            case 0x200u: case 0x204u: case 0x208u: case 0x20Cu: case 0x210u: case 0x214u:
            case 0x218u: case 0x21Cu: case 0x220u: case 0x22Cu: case 0x280u: case 0x284u:
            case 0x288u: case 0x28Cu: case 0x2B8u: {
                u32* r = &s_zc.surf_reg[(mm - 0x180u) >> 2];
                const u32 diff = mm == 0x208u ? ((*r ^ v) & 0xFFFFu) : (*r ^ v);
                *r = v;
                if (mm == 0x198u) s_zc.zeta_dma = v;
                else if (mm == 0x208u) s_zc.surf_fmt = v;
                else if (mm == 0x214u) s_zc.zeta_off = v;
                else if (mm == 0x22Cu) s_zc.zpitch = v;
                if (diff) { s_zc.surf_dirty = 1; s_zc.contested = 0; }
                return 0;
            }
            default: break;
            }
        }
        switch (mm) {
        case 0x0328u: gcm_zc_option(&s_zc.stencil_test, v); return 0;
        case 0x032Cu: gcm_zc_option(&s_zc.stencil_mask, v); return 0;
        case 0x0348u: gcm_zc_option(&s_zc.two_sided, v); return 0;
        case 0x033Cu: case 0x0340u: case 0x0344u: case 0x034Cu:
        case 0x035Cu: case 0x0360u: case 0x0364u: gcm_zc_option(NULL, v); return 0;
        case 0x0A6Cu: gcm_zc_option(&s_zc.depth_func, v); return 0;
        case 0x0A70u: gcm_zc_option(&s_zc.depth_mask, v); return 0;
        case 0x0A74u: gcm_zc_option(&s_zc.depth_test, v); return 0;
        case 0x0380u: s_zc.bounds_test = v; return 0;
        case 0x0384u: s_zc.bounds_min = v; return 0;
        case 0x0388u: s_zc.bounds_max = v; return 0;
        case 0x08C0u: s_zc.scissor_h = v; return 0;
        case 0x1D94u: {
            /* RSXCoreRenderer clear: stencil bit dropped with a zero stencil
             * mask; no bits = no framebuffer bind. */
            u32 a = v;
            if (!s_zc.stencil_mask) a &= ~2u;
            if (!(a & 0xF3u)) return 0;
            gcm_zc_fb_swap(((a & 0xF0u) ? 1 : 0) | ((a & 3u) ? 2 : 0));
            return 0;
        }
        default: break;
        }
    }
    switch (mm) {
    case 0x0198u: s_zc.zeta_dma = v; s_zc.surf_dirty = 1; return 0;
    case 0x0208u: s_zc.surf_fmt = v; s_zc.surf_dirty = 1; return 0;
    case 0x0214u: s_zc.zeta_off = v; s_zc.surf_dirty = 1; return 0;
    case 0x1D94u: gcm_zc_fb_swap(3); return 0;
    case 0x1808u:
        if (v) {
            gcm_zc_fb_swap(0);
            s_zc.skip_clause = gcm_zc_render_disabled();
            return s_zc.skip_clause;
        }
        if (s_zc.skip_clause) { s_zc.skip_clause = 0; return 1; }
        gcm_zc_on_draw();
        return 0;
    case 0x1814u: case 0x1818u: case 0x181Cu: case 0x1820u: case 0x1824u:
        return s_zc.skip_clause;
    case 0x1D70u:
        gcm_zc_label_sync();
        return 0;
    default:
        return 0;
    }
}

/* Start of every walker pass: retire what has completed, and a flush an HLE
 * report read asked for (RPCS3's pipe_flush_interrupt -> thread::sync). */
static void gcm_zc_tick(void)
{
    if (!s_zc.inited || !s_zc.on) return;
    if (InterlockedExchange(&s_zc.sync_req, 0)) gcm_zc_sync();
    gcm_zc_update(0, 0);
}

/* A PPU thread reading a report through cellGcm. With guard pages (RPCS3)
 * the read itself faults when it matters; with PS3RECOMP_ZCULL_CRITICAL=1
 * ask the walker to retire the queue (without blocking, as RPCS3). */
static void gcm_zc_hle_read(void)
{
    if (!s_zc.inited || !s_zc.on || !s_zc.wn || s_zcp.mode != 1) return;
    InterlockedExchange(&s_zc.sync_req, 1);
    if (s_gcm_kick_ev) SetEvent(s_gcm_kick_ev);
}

/* thread::flip: zpass/stats left running with nothing reading them would
 * queue forever (RPCS3's Diablo III / Yakuza 5 workaround). */
static void gcm_zc_on_flip(void)
{
    if (s_zc.inited && s_zc.on && s_zc.wn) gcm_zc_clear(1u | 2u);
}

static int gcm_flip_reset_enabled(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_FLIP_RESET"); on = !(e && *e == '0'); }
    return on;
}

/* RPCS3 flip_command -> thread::reset: after the register reset
 * (ZPASS_PIXEL_COUNT_ENABLE 0, RENDER_ENABLE 0x01000000), check_zcull_status
 * and set_render_mode run from those values. */
static void gcm_flip_state_reset_zc(void)
{
    if (!gcm_flip_reset_enabled()) return;
    gcm_zc_init();
    if (!s_zc.on) return;
    s_zc.zpass_en = 0;
    gcm_zc_check_state();
    gcm_zc_set_render_enable(0x01000000u);
}

static int gcm_method_post_wanted(u32 m)
{
    switch (m) {
    case 0x50u: case 0x1A8u: case 0x1800u:
    case 0x17C8u: case 0x17CCu: case 0x1804u: case 0x1D84u: case 0x1E98u:
        return 1;
    default:
        return 0;
    }
}

/* v140: what the FIFO walker does after rsx_process_method for the methods
 * it also handles itself (reference fences, report DMA, reports, ZCULL and
 * conditional rendering state). Shared by the per-word walk and the quick
 * block walk. */
static void gcm_method_post(u32 m, u32 v_in)
{
    extern u32 g_rsx_last_reference;
    gcm_zc_init();
    /* NV406E_SET_REFERENCE: queue the fence value for PACED
     * publication (gcm_ref_publish below) instead of letting a
     * later fence in the same batch overwrite it. */
    if (m == 0x50) {
        /* RPCS3 nv406e.cpp:14-22 set_reference: thread::sync() before ref is
         * written -- queued ZCULL reports and pending reads land first.
         * GCM_REF_SYNC=0 skips it. The paced queue stays: see
         * gcm_ref_publish_one / the read-driven publication below, which an
         * equality-waiter needs to see every value. */
        static int rs = -1;
        if (rs < 0) { const char* e = getenv("GCM_REF_SYNC"); rs = !(e && atoi(e) == 0); }
        if (rs) {
            gcm_zc_label_sync();
            { extern void rsx_backend_flush_reads(void); rsx_backend_flush_reads(); }
        }
        gcm_ref_push_at(g_rsx_last_reference, s_fifo_getoff);
    }
    if (m == 0x1A8) s_report_dma = v_in;
    if (s_zc.on) {
        switch (m) {
        case 0x17C8u: gcm_zc_clear(v_in); return;
        case 0x17CCu: s_zc.zpass_en = v_in != 0; gcm_zc_check_state(); return;
        case 0x1804u: s_zc.stats_en = v_in != 0; gcm_zc_check_state(); return;
        case 0x1D84u: {
            const int en = v_in != 0;
            if (en != s_zc.zcull_en) { s_zc.zcull_en = en; gcm_zc_check_state(); }
            return;
        }
        case 0x1E98u: gcm_zc_set_render_enable(v_in); return;
        default: break;
        }
    }
    /* NV4097_GET_REPORT: (type << 24) | offset into the report area. */
    if (m == 0x1800) {
        const u32 v = v_in, off = v & 0xFFFFFFu, idx = off / 16u, type = v >> 24;
        { static int rn = 0; if (PS3_LOG_FIRST(rn, 6)) printf("[GET_REPORT] type=%u off=0x%X dma=0x%08X%c", type, off, s_report_dma, 10); }
        { static u32 seen[16]; static int ns = 0; const u32 key = (s_report_dma & 0xFFFFFFF0u) | type;
          int f = 0; for (int q = 0; q < ns; q++) if (seen[q] == key) f = 1;
          if (!f && ns < 16) { seen[ns++] = key;
              printf("[GET_REPORT] first type=%u with dma=0x%08X (off=0x%X)%c", type, s_report_dma, off, 10); } }
        const u32 sink = gcm_zc_sink(off);
        if (!sink) {
            { static int un = 0; if (PS3_LOG_FIRST(un, 4)) printf("[GET_REPORT] offset 0x%X (dma 0x%08X) has no address%c", off, s_report_dma, 10); }
            /* RPCS3 get_report: no address, nothing written. GCM_REPORT_NOSINK_STORE=1: old store. */
            static int nss = -1;
            if (nss < 0) { const char* e = getenv("GCM_REPORT_NOSINK_STORE"); nss = e && atoi(e) != 0; }
            if (nss && idx < CELL_GCM_MAX_REPORT_COUNT) {
                s_report_data[idx].timestamp = gcm_report_timestamp();
                s_report_data[idx].value = type == 1u ? 0xFFFFu : 0u;
            }
            return;
        }
        if (!s_zc.on) {
            /* The old immediate write: ZPASS "visible", the rest 0. */
            gcm_zc_store(sink, gcm_report_timestamp(), type == 1u ? 0xFFFFu : 0u, 0);
            return;
        }
        if (type >= 1u && type <= 5u) { gcm_zc_read_report(sink, type); return; }
        /* Bad type: RPCS3 stamps the time and padding, value untouched. */
        const u64 ts = gcm_report_timestamp();
        const u32 words[4] = { (u32)(ts >> 32), (u32)ts, gcm_zcp_access(sink + 8u, NULL, 0), 0u };
        gcm_zcp_access(sink, words, 0);
    }
}

static void gcm_rsx_process_fifo_unlocked(void)
{
    gcm_zc_tick();
    { static unsigned _n = 0; static unsigned long long _t0 = 0;
      extern unsigned long long ps3_ms_now(void);
      _n++;
      if (getenv("GCM_RATE")) {
          unsigned long long now = ps3_ms_now();
          if (_t0 == 0) _t0 = now;
          if (now - _t0 >= 2000) {
              fprintf(stderr, "[RATE] drains/sec=%.0f\n", _n * 1000.0 / (now - _t0));
              _n = 0; _t0 = now;
          }
      } }
    static rsx_state s_state;
    static int  s_inited = 0;
    extern u32 g_rsx_last_reference;

    /* A statically-linked libgcm (PS3 firmware modules -- ps1_netemu) never calls
     * cellGcmSetupContext, so there is no CellGcmContextData EA to gate on: it gets
     * its ring through sys_rsx_context_allocate instead. The walk below only needs
     * `put` and the IO table, both of which the syscall layer fills; the one part
     * that does read the context (the ring-recycle heuristic) is separately guarded
     * by s_gcm_ctx_out_ea, which stays 0 on that path. */
    if (!s_gcm_context_ea && !s_gcm_syscall_mode) return;
    if (!s_inited) { rsx_state_init(&s_state); s_inited = 1; }

    /* Walk the FIFO exactly like the RSX: chase the guest-written `put` (an IO
     * offset in the control register), decoding method headers and following
     * JUMP/CALL/RET control words. The old linear drain copied context->current
     * onward and treated a JUMP as end-of-buffer -- fine for titles that write
     * one flat ring (cellmark), but PSL1GHT/Tiny3D immediately JUMPs into its
     * own command ring, so everything after the jump (including the reference
     * writes its waits spin on) silently never executed. */
    u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
    /* The words behind `put` were stored by another thread (the title, or the
     * recycle below) before it stored `put`. On x86 the hardware keeps loads
     * in order; on arm64 it does not, so without an acquire here the ring
     * word can be fetched before `put` was and come back stale -- the walker
     * then runs off the end of the previous frame's bytes until it meets a
     * word that is not a command, and parks there forever. */
    atomic_thread_fence(memory_order_acquire);

    /* GCM_CTXDBG=1: which context the title is actually driving. Ours is written
     * once at init; if the title repoints gCellGcmCurrentContext at a buffer of
     * its own, every field below moves with it and the FIFO the runtime walks
     * stops being the one the title fills. Reported on change, not per tick. */
    if (getenv("GCM_CTXDBG") && s_gcm_ctx_out_ea) {
        static u32 last_ptr = 0xFFFFFFFFu, last_cur = 0xFFFFFFFFu;
        u32 ptr = vm_read32(s_gcm_ctx_out_ea);
        u32 cur = ptr ? vm_read32(ptr + 0x8) : 0;
        if (ptr != last_ptr || cur != last_cur) {
            last_ptr = ptr; last_cur = cur;
            fprintf(stderr, "[CTX] gCellGcmCurrentContext@0x%08X -> 0x%08X  "
                    "begin=%08X end=%08X current=%08X callback=%08X  (ours=0x%08X)\n",
                    s_gcm_ctx_out_ea, ptr,
                    ptr ? vm_read32(ptr + 0x0) : 0, ptr ? vm_read32(ptr + 0x4) : 0,
                    cur, ptr ? vm_read32(ptr + 0xC) : 0, s_gcm_context_ea);
            fflush(stderr);
        }
    }

    u32 start_off = s_fifo_getoff;
    /* GCM_DRAINDBG: log one drain pass in GCM_DRAINDBG_EVERY (default 60). */
    static int s_drain_every = -1;
    if (s_drain_every < 0) { const char* e = getenv("GCM_DRAINDBG_EVERY");
                             s_drain_every = (e && atoi(e) > 0) ? atoi(e) : 60; }
    if (getenv("GCM_DRAINDBG")) {
        static int n = 0;
        if (n++ % s_drain_every == 0)
            printf("[DRAIN] getoff=%08X put=%08X ref=%08X ctx.current=%08X\n",
                   s_fifo_getoff, put, vm_read32(GCM_CONTROL_GUEST_ADDR + 8),
                   vm_read32(s_gcm_context_ea + 0x8));
    }

    /* GCM_SCANREF=1 (one-shot): list every NV406E_SET_REFERENCE actually
     * present in 0..put. "The title waits for a fence we never published" has
     * two very different causes -- we missed the command, or it was never
     * submitted -- and only a direct scan of the ring separates them. */
    { static int scanned = 0;
      if (!scanned && getenv("GCM_SCANREF") && put >= 0x19F0) { scanned = 1;
        unsigned found = 0;
        for (u32 io = 0; io + 8 <= put; io += 4) {
            u32 ea = gcm_io2ea(io); if (!ea) continue;
            u32 w = vm_read32(ea);
            if ((w & 0x1FFCu) == 0x0050u && (w >> 29) == 0) {
                u32 dea = gcm_io2ea(io + 4);
                fprintf(stderr, "[SCANREF] io=%08X w=%08X count=%u arg=%08X\n",
                        io, w, (w >> 18) & 0x7FFu, dea ? vm_read32(dea) : 0xDEADu);
                found++;
            }
        }
        fprintf(stderr, "[SCANREF] %u SET_REFERENCE in 0..%08X\n", found, put);
      } }

    /* GCM_FIFO_SNAP=N: dump N snapshots of the raw words at both ends of the
     * ring -- what `get` is about to decode, and what the title just wrote at
     * `put`. When get and put drift apart, the two windows say immediately
     * whether get is grinding through NOPs/garbage or the title is writing
     * somewhere get can never reach. The [FIFOSTEP] trace only ever fired on a
     * bad branch, which is exactly the case where the drift is silent. */
    { static int snap = -1;
      if (snap < 0) { const char* e = getenv("GCM_FIFO_SNAP"); snap = e ? atoi(e) : 0; }
      if (snap > 0 && put) {
          snap--;
          fprintf(stderr, "[SNAP] get=%08X put=%08X\n", s_fifo_getoff, put);
          for (int k = 0; k < 16; k++) {
              u32 gio = s_fifo_getoff + (u32)(k * 4), pio = put + (u32)(k * 4);
              u32 gea = gcm_io2ea(gio), pea = gcm_io2ea(pio);
              fprintf(stderr, "[SNAP]  get+%-3d io=%08X w=%08X   put+%-3d io=%08X w=%08X\n",
                      k * 4, gio, gea ? vm_read32(gea) : 0xDEADDEADu,
                      k * 4, pio, pea ? vm_read32(pea) : 0xDEADDEADu);
          }
          fflush(stderr);
      } }

    int budget = 0x100000;                    /* words per tick cap */
    /* GCM_DRAINDBG also reports WHY each pass stopped. A drain that ends far
     * short of `put` every tick is the signature of a FIFO that can never
     * catch up, and the reason is the whole diagnosis. */
    const char* why = "caught-up";
    while (s_fifo_getoff != put && budget-- > 0) {
        u32 ea = gcm_io2ea(s_fifo_getoff);
        if (!ea) {
            /* get is sitting on an IO offset with no mapping -- the title
             * branched into a region and then unmapped it, or the offset was
             * never valid. Breaking silently (what this did) leaves get stuck
             * there FOREVER: it can never reach put, the FIFO stops draining,
             * no fences publish, and anything waiting on ctrl->ref hangs with
             * nothing logged. Resynchronise to put instead. Commands between
             * here and put are lost, which is bad, but a permanently dead FIFO
             * is worse -- and now it says so. */
            static u32 last_bad; static int n;
            if (s_fifo_getoff != last_bad || n < 4) {
                last_bad = s_fifo_getoff;
                if (PS3_LOG_FIRST(n, 16))
                    fprintf(stderr, "[cellGcmSys] FIFO get=0x%08X has no IO "
                            "mapping -- resyncing to put=0x%08X\n",
                            s_fifo_getoff, put);
            }
            s_fifo_getoff = put;
            gcm_skip_ahead_note("unmapped-get");
            /* Whatever the walker had not reached is gone, and so are the flips
             * queued in it: none of them will ever be executed, so do not let a
             * flip wait on them. */
            s_fifo_flips_reached = s_fifo_flips_queued;
            why = "no-io-mapping";
            break;
        }
        u32 w = vm_read32(ea);
        /* RSXThread.cpp:886-891: note a rollback address at a command boundary. */
        if (s_fifo_sync_req && !s_state.in_begin_end) {
            s_fifo_restore = s_fifo_getoff; s_fifo_saved_ret = s_fifo_calloff; s_fifo_sync_req = 0; }
        /* Only remember offsets BELOW put -- those are positions in the
         * ring the title is writing. A park in another region is where the
         * walker gave up, not how far it got through the ring. */
        if (s_fifo_getoff < put) s_last_ring_off = s_fifo_getoff + 4;
        s_tr_off[s_tr_i % GCM_TRACE_N] = s_fifo_getoff;
        s_tr_w[s_tr_i % GCM_TRACE_N] = w;
        s_tr_i++;
        if (w && ((w & 0xE0000003u) == 0x20000000u || (w & 3u) == 1u || (w & 3u) == 2u)) {
            s_cf_off[s_cf_i % 8] = s_fifo_getoff; s_cf_w[s_cf_i % 8] = w; s_cf_i++; }
        /* GCM_WORD_STATS=<secs>: NOP words vs headers vs jumps/calls/returns
         * walked -- a frame that is mostly NOPs is reserved space someone
         * (an SPU job) was supposed to fill. */
        { static int ws = -1; static unsigned long long nop, hdr, jmp, cal, ret, t0;
          if (ws < 0) { const char* e = getenv("GCM_WORD_STATS"); ws = e ? atoi(e) : 0; }
          if (ws > 0) {
              if (!w && s_fifo_getoff < put) { static int mm = 0;
                  const u32 alias = s_fifo_getoff + 0x11000000u;
                  if (mm < 6 && ea != alias && vm_read32(alias)) { mm++;
                      fprintf(stderr, "[io2ea-mismatch] io=%08X walker_ea=%08X (w=0) alias=%08X w=%08X\n",
                              s_fifo_getoff, ea, alias, vm_read32(alias)); } }
              if (!w) nop++;
              else if ((w & 0xE0000003u) == 0x20000000u || (w & 3u) == 1u) jmp++;
              else if ((w & 3u) == 2u) cal++;
              else if (w == 0x00020000u) ret++;
              else hdr++;
              extern unsigned long long ps3_ms_now(void);
              const unsigned long long now = ps3_ms_now();
              if (!t0) t0 = now;
              if (now - t0 >= (unsigned long long)ws * 1000u) {
                  fprintf(stderr, "[words] nop=%llu hdr=%llu jump=%llu call=%llu ret=%llu get=0x%08X put=0x%08X jumps:",
                          nop, hdr, jmp, cal, ret, s_fifo_getoff, put);
                  for (u32 t = 4; t >= 1; t--) { u32 q = (s_cf_i - t) % 8;
                      fprintf(stderr, " %08X@%08X", s_cf_w[q], s_cf_off[q]); }
                  fprintf(stderr, "\n");
                  nop = hdr = jmp = cal = ret = 0; t0 = now;
              } } }
        u32 type = w >> 29;

        if ((w & 0xFFFF0000u) == 0xFEAD0000u) {
            /* lv1 driver flip: statically-linked libgcm cellGcmSetFlip writes
             * this word into the FIFO instead of calling any import. Fire the
             * flip (status/count/handler) and STOP this drain so the ticker's
             * flip-gated present shows exactly the completed frame -- draining
             * on would mix the next frame's head into the batch (wave: every
             * frame split across 4 presents, layout flashing/zooming). */
            extern s32 cellGcmSetFlipCommand(u32 bufferId);
            s_fifo_getoff += 4;
            /* RPCS3 flip_command: thread::reset before the flip. */
            gcm_flip_state_reset_zc();
            if (rsx_draw_engine_enabled()) rsx_draw_engine_method(0xFEACu, w & 7u);
            gcm_zc_on_flip();
            /* Count the flip WORDS the title actually writes into the FIFO,
             * separately from the presents that result. When a title stops
             * appearing on screen the first question is which side stopped:
             * this counter still climbing while presents do not is a runtime
             * problem, both stopping together is the guest's. */
            { static unsigned long long flips = 0; static int dbg = -1;
              if (dbg < 0) dbg = getenv("GCM_FLIPCOUNT") ? 1 : 0;
              ++flips;
              if (dbg && (flips <= 8ull || (flips % 200ull) == 0))
                  fprintf(stderr, "[flipword] %llu FIFO flip words decoded%c",
                          flips, 10); }
            if (PS3_FT_ON()) { const uint64_t t = ps3_ft_now();
                               ps3_ft_span(PS3_FT_FLIP_FIFO, 0, w & 7u, 0, t, t); }
            /* RPCS3 rsx_methods.cpp flip_command: executing the flip clears
             * the 16-byte label 1 slot -- what cellGcmSetWaitFlip's
             * SEMAPHORE_ACQUIRE(0x10, 0) waits on. GCM_FLIP_KEEP_LABEL1=1
             * restores the old behaviour. */
            { static int keep = -1;
              if (keep < 0) keep = getenv("GCM_FLIP_KEEP_LABEL1") ? 1 : 0;
              if (!keep)
                  for (u32 q = 0; q < 16u; q += 4u)
                      vm_write32(GCM_LABEL_GUEST_BASE + GCM_LABEL_STRIDE + q, 0u); }
            if ((w & 0xFFFFFF00u) == GCM_FLIP_MARKER) {
                /* Queued by _cellGcmSetFlipCommand, which already did the
                 * guest-visible half of the request. */
                gcm_set_current_display_buffer(w & 7u);
                s_flip_pending = 1;
                s_flip_request_count++;
                /* Not past `queued`: a resync may already have written off
                 * this flip. */
                if (s_fifo_flips_reached != s_fifo_flips_queued) s_fifo_flips_reached++;
            } else {
                cellGcmSetFlipCommand(w & 0xFFu);
            }
            /* ...unless the FIFO is badly backlogged. One flip per drain is
             * right while `get` is keeping up with `put`; when it is megabytes
             * behind it is a deadlock, because the title's ring can only be
             * reclaimed as `get` advances. VF5 submits ~5x faster than one
             * flip per drain consumes, so its 4 MB ring fills, its own
             * buffer-full callback can never free space, and AMGL prints
             * "Command Buffer Overflow" forever. Past the threshold keep
             * draining -- the intermediate presents are frames we are too far
             * behind to show anyway. No-op for a title that is keeping up. */
            /* A wrapped ring has put behind get, and that is the most
             * backlogged it can be, not the least: God of War HD fills its
             * 3 MB ring at boot, wraps, and a walker stopping at every flip
             * then crawled ~1 KB a tick while the title wrote over the
             * commands ahead of it. */
            if (gcm_fifo_backlog(put, s_fifo_getoff) < GCM_FIFO_CATCHUP_BYTES)
                { why = "flip"; break; }
            continue;
        }

        /* RPCS3 RSXFIFO.cpp:487-503 / 777-780: a word with any of the
         * non-method bits (0xA0030003) set must be an old JUMP, a new JUMP
         * (0x00000001 | offset), a CALL or a RET; anything else is a desync
         * and recovers. The FEAD flip words above are this port's own. */
        int is_jump = (type == 1), is_ret = (w == 0x00020000u);
        if (gcm_fifo_recover_on()) {
            is_jump = (w & 0xE0000003u) == 0x20000000u || (w & 0xE0000003u) == 1u;
            is_ret  = (w & 0xFFFF0003u) == 0x00020000u;
            if ((w & 0xA0030003u) && !is_jump && !is_ret && (w & 3u) != 2u) {
                s_fifo_rec_why = "malformed command"; goto fifo_recover; }
        }
        if (is_jump) {                         /* JUMP: 0x20000000 | offset, or offset | 1 */
            { static int _rd = -1; if (_rd < 0) _rd = getenv("GCM_RECDBG") ? 1 : 0;
              if (_rd) fprintf(stderr, "[JMP] %08X -> %08X (put=%08X)\n",
                               s_fifo_getoff, w & 0x1FFFFFFCu, put); }
            { u32 tgt = w & 0x1FFFFFFCu;
              if (!gcm_io2ea(tgt)) { gcm_fifo_bad_branch("JUMP", tgt, w); gcm_fifo_dump_around(s_fifo_getoff); gcm_fifo_resync_why("unmapped-JUMP", &s_fifo_getoff, put); break; }
              /* A JUMP to its own address is the "park the RSX here" idiom:
               * the title leaves it at the write head so the GPU stops if it
               * catches up, and overwrites it when the next segment is
               * appended. Taking it re-reads the same word forever -- 38.8 M
               * times in one Twisted Metal run, which is the drain burning the
               * CPU the guest needs. Stop this pass instead; the next one
               * re-reads the word, so a patched jump is still followed. */
              if (tgt == s_fifo_getoff) {
                  /* Parked. If the title has meanwhile moved `put` somewhere
                   * else, it has switched to its other segment and left this
                   * park standing -- it only patches a park when it reuses that
                   * block. Waiting for a patch that will not come freezes the
                   * FIFO for the rest of the run, so follow the write head. */
                  /* ...but not at once. A title that links its segments by
                   * patching the park moves `put` first and patches second:
                   * inFAMOUS 2 ends each of its main-memory command blocks in
                   * a park, writes the next commands at the ring start, sets
                   * put, and only then rewrites the park as a JUMP to them. A
                   * drain that lands between the two saw put elsewhere, went
                   * straight to it, and dropped everything from the ring
                   * start to put -- the frame's fence with it, so the title
                   * waited on that label for the rest of the run. Give the
                   * patch time to arrive, and resync only for a park that
                   * has stood with the same put for longer than any title
                   * takes between the two stores. */
                  if (put != s_fifo_getoff) {
                      extern unsigned long long ps3_ms_now(void);
                      static u32 park_off, park_put;
                      static unsigned long long park_since;
                      const unsigned long long now = ps3_ms_now();
                      if (park_off != s_fifo_getoff || park_put != put) {
                          park_off = s_fifo_getoff; park_put = put; park_since = now;
                      } else if (now - park_since >= GCM_PARK_GRACE_MS) {
                          park_off = 0;
                          gcm_fifo_dump_around(s_fifo_getoff);
                          gcm_fifo_resync_why("jump-park", &s_fifo_getoff, put);
                      }
                  }
                  why = "parked";
                  s_park_pass = 1;
                  s_fifo_sync_req = 1;         /* RSXFIFO.cpp:798 */
                  break;
              }
              if (tgt < s_fifo_getoff) s_fifo_back_jumps++;
              s_fifo_getoff = tgt;
              /* get is the RSX's fetch pointer: a title polling it sees the
               * GPU leave a segment as soon as the JUMP out of it is taken.
               * The end of the pass, where get is otherwise published, can
               * come tens of milliseconds later. GCM_GET_EQ_PUT keeps its
               * own value. */
              if (!gcm_get_eq_put()) vm_write32(GCM_CONTROL_GUEST_ADDR + 4, tgt); }
            continue;
        }
        if ((w & 3) == 2) {                    /* CALL: offset | 2 */
            /* RSXFIFO.cpp:814-818: only one call level -- CALL inside a
             * subroutine recovers. */
            if (s_fifo_calloff != GCM_CALL_NONE && gcm_fifo_recover_on()) {
                s_fifo_rec_why = "CALL inside a subroutine"; goto fifo_recover; }
            { u32 tgt = w & 0x1FFFFFFCu;
              if (!gcm_io2ea(tgt)) { gcm_fifo_bad_branch("CALL", tgt, w); gcm_fifo_resync_why("unmapped-CALL", &s_fifo_getoff, put); break; }
              s_fifo_calloff = s_fifo_getoff + 4;
              s_fifo_getoff  = tgt; }
            continue;
        }
        if (is_ret) {                          /* RET */
            /* RSXFIFO.cpp:830-833: RET without CALL recovers. */
            if (s_fifo_calloff == GCM_CALL_NONE) {
                if (gcm_fifo_recover_on()) { s_fifo_rec_why = "RET without CALL"; goto fifo_recover; }
                s_fifo_getoff = 0;
                continue;
            }
            s_fifo_getoff = s_fifo_calloff;
            s_fifo_calloff = GCM_CALL_NONE;
            continue;
        }
        if (type == 0 || type == 2) {          /* method (incrementing / NI) */
            u32 count  = (w >> 18) & 0x7FF;
            u32 method = w & 0x1FFCu;
            u32 subch  = (w >> 13) & 7;
            /* TCONST_FIFO=1: how the title actually streams vertex constants.
             * NV4097_SET_TRANSFORM_CONSTANT is a 32-register window; if a block
             * arrives NON-INCREMENTING then every dword carries the same method
             * and any handler that derives the constant slot from the method
             * offset writes them all on top of each other. */
            { static int tf = -1;
              if (tf < 0) { const char* e = getenv("TCONST_FIFO"); tf = e ? atoi(e) : 0; }
              if (tf && subch == 0) {
                  static u32 hist[2048]; static u32 tot[2048]; static int dumped = 0;
                  u32 mi = (method >> 2) & 2047;
                  hist[mi]++; tot[mi] += count;
                  static u64 seen = 0;
                  if (++seen == 200000ull && !dumped) { dumped = 1;
                      fprintf(stderr, "[TCFIFO] method histogram (blocks, dwords):%c", 10);
                      for (u32 k = 0; k < 2048; k++) if (hist[k])
                          fprintf(stderr, "[TCFIFO]   0x%04X  %8u blocks %10u dwords%c",
                                  k << 2, hist[k], tot[k], 10);
                  }
              } }
            /* NV406E semaphore: OFFSET(0x64) / ACQUIRE(0x68) / RELEASE(0x6C).
             *
             * These were falling through to rsx_process_method as "unknown method" --
             * i.e. no-ops. A title that syncs with cellGcmSetFlipCommandWithWaitLabel and
             * cellGcmGetLabelAddress (Twisted Metal does both) then waits on a label the
             * RSX is supposed to write and never sees it move, so it never appends its
             * next FIFO segment and the GPU sits on a park forever.
             *
             * RELEASE writes the value; ACQUIRE stops this drain pass without consuming
             * the method, so the next pass re-reads it -- which is what the hardware does
             * while it waits. Semaphore offsets index the same label window
             * cellGcmGetLabelAddress hands out. */
            if (subch == 0 && (method == 0x64u || method == 0x68u || method == 0x6Cu)) {
                int sem_blocked = 0, sem_recover = 0;
                for (u32 i = 0; i < count; i++) {
                    u32 dea = gcm_io2ea(s_fifo_getoff + 4 + i * 4);
                    if (!dea) break;
                    u32 m = (type == 0) ? method + i * 4 : method;
                    u32 v = vm_read32(dea);
                    u32 la = gcm_sema406e_ea(s_sema_offset);
                    { static int sn = 0; if (getenv("GCM_RECDBG") && PS3_LOG_FIRST(sn, 12))
                        fprintf(stderr, "[SEMA] m=0x%02X v=0x%08X off=0x%X\n", m, v, s_sema_offset); }
                    /* PS3_FTRACE: an acquire this walker sat on is satisfied now. */
                    if (m == 0x68u && s_ft_sema_t0 && vm_read32(la) == v) {
                        ps3_ft_span(PS3_FT_SEMA, 0, s_sema_offset, v, s_ft_sema_t0, ps3_ft_now());
                        s_ft_sema_t0 = 0;
                    }
                    if (m == 0x68u) {   /* Resistance port: which labels does the FIFO wait on? */
                        static u32 seen_off[64]; static int nseen = 0; static unsigned long long nmiss = 0, nacq = 0;
                        int f = 0; for (int q = 0; q < nseen; q++) if (seen_off[q] == s_sema_offset) f = 1;
                        nacq++; if (vm_read32(la) != v) nmiss++;
                        if (!f && nseen < 64) { seen_off[nseen++] = s_sema_offset;
                            printf("[SEMA-ACQ] new off=0x%X want=0x%08X label=0x%08X%c", s_sema_offset, v, vm_read32(la), 10); }
                        if (nacq == 100 || nacq == 10000 || nacq == 1000000)
                            printf("[SEMA-ACQ] %llu acquires, %llu not yet satisfied when reached%c", nacq, nmiss, 10);
                    }
                    if (m == 0x64u)      s_sema_offset = v;
                    else if (m == 0x6Cu && (s_sema_offset & 3u)) {
                        /* RPCS3 nv406e.cpp:117-121 semaphore_release: an
                         * unaligned semaphore is ignored. */
                    }
                    else if (m == 0x6Cu) {
                        /* nv406e.cpp:123-147 (GCM_SEMA_ADDR=0 restores the old
                         * label-window-only release): every release but the
                         * flip semaphore's requests a sync point; an address
                         * outside the label region recovers; device+0x30 never
                         * gets 0. */
                        if (gcm_sema_addr_on()) {
                            if (!(s_sema_offset == 0x10u && s_sema406e_ctx == 0x66616661u)) s_fifo_sync_req = 1;
                            if (!la || (la >> 28) != (GCM_LABEL_GUEST_BASE >> 28)) {
                                s_fifo_rec_why = "NV406E semaphore unexpected address";
                                sem_recover = 1; break;
                            }
                            if (la == R2RSX_DEVICE_EA + 0x30u && !v) v = 1;
                        }
                        /* RPCS3 semaphore_release -> write_gcm_label -> sync
                         * (nv47_sync.hpp:14-50): queued ZCULL reports land
                         * before the label does -- except for the flip
                         * semaphores, and a write of the value already there. */
                        if (!gcm_sema_addr_on() ||
                            (la != GCM_LABEL_GUEST_BASE + 0x10u && la != R2RSX_DEVICE_EA + 0x30u && vm_read32(la) != v)) {
                            gcm_zc_label_sync();
                            { extern void rsx_backend_flush_reads(void); rsx_backend_flush_reads(); }   /* v180 */
                        }
                        /* The RSX has fetched past a RELEASE before its
                         * semaphore write lands, so a guest that sees the
                         * label must also see `get` past the command. `get`
                         * was published only at a JUMP and at the end of the
                         * pass: Ratchet & Clank: Tools of Destruction waits
                         * for a label, then reads `get` once and spins until
                         * its own `put` equals it, which a snapshot from
                         * before the RELEASE never does. GCM_GET_EQ_PUT keeps
                         * publishing put at the end of the pass instead. */
                        if (!gcm_get_eq_put())
                            vm_write32(GCM_CONTROL_GUEST_ADDR + 4, s_fifo_getoff + 4 + count * 4);
                        atomic_thread_fence(memory_order_release);
                        vm_write32(la, v);
                        { static int rn = 0; if (s_sema_offset != 0x400u && PS3_LOG_FIRST(rn, 40))   /* TEMP */
                            fprintf(stderr, "[SEMA-REL] get=0x%08X off=0x%X val=0x%08X\n", s_fifo_getoff, s_sema_offset, v); }
                    }
                    else if (m == 0x68u) {
                        /* nv406e.cpp:26: every acquire requests a sync point. */
                        if (gcm_fifo_recover_on()) s_fifo_sync_req = 1;
                        if (la && vm_read32(la) != v && gcm_sema_acquire_blocks(s_sema_offset)) {
                            /* RPCS3 nv406e.cpp:57-107: ACQUIRE blocks until the
                             * label matches, bounded by driver_recovery_timeout
                             * (1 s): then it logs and carries on. Here a blocked
                             * acquire stops the pass without consuming the
                             * method and the next pass re-reads it. inFAMOUS 2
                             * acquires a label its SPU lighting writes each
                             * frame: waiting here orders the RSX's sampling after
                             * that write. GCM_SEMA_ACQUIRE=0 never blocks;
                             * GCM_SEMA_ACQUIRE_MS=<ms> sets the bound (0 = wait
                             * for ever, as the hardware). */
                            static u32 b_off, b_val, b_get; static unsigned long long b_t0, b_last;
                            extern unsigned long long ps3_ms_now(void);
                            const unsigned long long now = ps3_ms_now();
                            const unsigned blk_ms = gcm_sema_acquire_ms();
                            if (!b_t0 || b_off != s_sema_offset || b_val != v || b_get != s_fifo_getoff) {
                                b_off = s_sema_offset; b_val = v; b_get = s_fifo_getoff; b_t0 = now; }
                            /* nv406e.cpp:78-84: a gap over 20 ms between polls
                             * (host stall) is not counted. GCM_SEMA_GAP_EXCLUDE=0 counts it. */
                            { static int gx = -1;
                              if (gx < 0) { const char* e = getenv("GCM_SEMA_GAP_EXCLUDE"); gx = !(e && atoi(e) == 0); }
                              if (gx && b_last && now - b_last > 20u && b_t0 < now) {
                                  const unsigned long long gap = now - b_last;
                                  b_t0 = b_t0 + gap > now ? now : b_t0 + gap; }
                              b_last = now; }
                            if (!blk_ms || now - b_t0 < blk_ms) {
                                if (PS3_FT_ON() && !s_ft_sema_t0) s_ft_sema_t0 = ps3_ft_now();
                                sem_blocked = 1; break;
                            }
                            { static int tn; if (PS3_LOG_FIRST(tn, 12))
                                fprintf(stderr, "[SEMA] acquire off=0x%X == 0x%08X timed out after %u ms (label 0x%08X)\n",
                                        s_sema_offset, v, blk_ms, vm_read32(la)); }
                            b_t0 = 0;
                        }
                    }
                }
                if (sem_recover) goto fifo_recover;
                if (sem_blocked) { s_sema_blocked_pass = 1; break; }
                s_fifo_getoff += 4 + count * 4;
                continue;
            }
            /* Resistance port: SET_TRANSFORM_CONSTANT blocks -- most of a York
             * frame's FIFO words -- straight into the constant file, without
             * the per-word diagnostic gates below and rsx_process_method's
             * long compare chain (~40% of the FIFO thread together). Taken
             * only when no per-word diagnostic is on, the block is incrementing
             * on subchannel 0, stays inside the 0x1F00 window and does not cross
             * an IO page. GCM_TCONST_SLOW=1 disables. */
            /* v140: the quick walk. When no per-word diagnostic is on, a 3D
             * method block (subchannel 0, or 1 bound as 3D) whose data sits in
             * one IO page is read straight from guest memory and handed to
             * rsx_process_method with nothing else per word: the per-word
             * walk below checks ~15 debug switches and re-reads each data word
             * up to five times, and on a Grimsby frame that loop was a quarter
             * of the FIFO thread on its own. Transform-constant and
             * transform-program runs go in one call each. GCM_FIFO_QUICK=0
             * turns it off (GCM_TCONST_SLOW=1 still disables just the
             * constant runs). */
            { static int fast = -1, quick = -1, s1q = -1;
              if (fast < 0) {
                  fast = !getenv("TCONST_FIFO") && !getenv("GCM_SUBCH7") &&
                         !getenv("GCM_OBJDBG") && !getenv("GCM_SUBCH_GATE") && !getenv("GCM_VAL_WATCH") &&
                         !getenv("GCM_METHOD_HIST") && !getenv("RSX_LIVE_FEED_DBG") &&
                         !rsx_live_draw_enabled() && !rsx_draw_engine_enabled() &&
                         !getenv("METHOD_SEEN") && !getenv("RSX_TRACE");
                  const char* q = getenv("GCM_FIFO_QUICK");
                  quick = fast && !(q && atoi(q) == 0);
                  const char* e = getenv("GCM_SUBCH1_2D");
                  s1q = !(e && atoi(e));
                  fprintf(stderr, "[fifo] quick walk %s%c", quick ? "on" : "off", 10);
              }
              extern volatile int g_port_trace_left;
              extern volatile long g_fifo_mhist_on;
              if (g_fifo_mhist_on) fifo_mhist_note(subch, method, type, count);
              if (fast && count && (subch == 0 || (subch == 1 && s1q && s_sub1_class == 1)) && g_port_trace_left <= 0) {
                  const u32 io0 = s_fifo_getoff + 4, io1 = s_fifo_getoff + count * 4;
                  const u32 dea = gcm_io2ea(io0);
                  if (dea && (io0 >> 20) == (io1 >> 20)) {
                      extern u8* vm_base;
                      const u8* be = vm_base + dea;
                      if (type == 0 && method >= 0x1F00u && method + count * 4u <= 0x1F80u &&
                          rsx_tconst_fast_ok()) {
                          rsx_tconst_block(&s_state, method, be, count);
                          s_fifo_getoff += 4 + count * 4;
                          continue;
                      }
                      if (quick) {
                          if (method >= 0x0B80u && (type == 0 ? method + count * 4u <= 0x0C00u : method < 0x0C00u)) {
                              rsx_tprog_block(&s_state, be, count);
                          } else {
                              static unsigned char qseen[0x800];
                              for (u32 i = 0; i < count; i++, be += 4) {
                                  const u32 m = (type == 0) ? method + i * 4 : method;
                                  const u32 v = ((u32)be[0] << 24) | ((u32)be[1] << 16) | ((u32)be[2] << 8) | be[3];
                                  const u32 mi = (m & 0x1FFCu) >> 2;
                                  if (!qseen[mi]) { qseen[mi] = 1;
                                      printf("[mfirst] subch=%u m=0x%04X = 0x%08X%c", subch, m & 0x1FFCu, v, 10); }
                                  if (gcm_method_invalid(m, v)) goto fifo_recover;
                                  if (gcm_zc_pre(m, v)) continue;
                                  if (gcm_walk_check(m, v)) goto fifo_recover;
                                  rsx_process_method(&s_state, m, v);
                                  if (gcm_method_post_wanted(m)) gcm_method_post(m, v);
                              }
                          }
                          s_fifo_getoff += 4 + count * 4;
                          continue;
                      }
                  }
              } }
            for (u32 i = 0; i < count; i++) {
                u32 dea = gcm_io2ea(s_fifo_getoff + 4 + i * 4);
                if (!dea) break;
                u32 m = (type == 0) ? method + i * 4 : method;
                /* GCM_SET_USER_COMMAND is method 0xEB00, which this decode
                 * splits into subchannel 7 / method 0x0B00 -- so it was landing
                 * in the 2D engine path and being dropped as unrecognised. It
                 * is not a 2D method at all: it tells the RSX to raise a
                 * USER_CMD interrupt, which is what ps1_netemu's flip path
                 * blocks on. */
                { static int sd = -1; static unsigned long s7 = 0;
                  if (sd < 0) sd = getenv("GCM_SUBCH7") ? 1 : 0;
                  if (sd && subch == 7 && (PS3_LOG_FIRST(s7, 16)))
                      fprintf(stderr, "[subch7] method=0x%04X data=0x%08X\n",
                              m, vm_read32(dea)); }
                if (subch == 7 && m == 0x0B00u) {
                    extern void rsx_raise_user_cmd(u32 arg);
                    rsx_raise_user_cmd(vm_read32(dea));
                    /* A title that registered its handler with the HLE
                     * cellGcmSetUserHandler has no lv2 interrupt thread
                     * reading that event; its handler runs from the pump.
                     * The `continue` skips the 0xEB00 case below, so queue
                     * it for the pump here. */
                    if (s_user_handler_opd) cellGcmQueueUserCommand(vm_read32(dea));
                    continue;
                }
                /* GCM_SUBCH1_3D=1: treat subchannel 1 as the 3D object too.
                 * The title issues NV4097 methods on it -- 0x1A80..0x1AC0 is
                 * SET_TEXTURE_OFFSET for units 4-6 -- and the 2D path silently
                 * discards whatever it does not recognise, so that state never
                 * reaches the renderer. SET_OBJECT binds are not tracked, which
                 * is what makes the subchannel-to-engine mapping a guess. */
                /* GCM_OBJDBG=1: log every SET_OBJECT (method 0). The handle a
                 * title binds to a subchannel is what says which engine that
                 * subchannel drives; comparing subchannel 1 against subchannel 0
                 * turns the routing below from a guess into a reading. */
                { static int od = -1;
                  if (od < 0) { const char* e = getenv("GCM_OBJDBG"); od = e ? 1 : 0; }
                  if (od && m == 0) {
                      static unsigned seen[8]; static int have[8];
                      const unsigned h = vm_read32(dea);
                      if (subch < 8 && (!have[subch] || seen[subch] != h)) {
                          have[subch] = 1; seen[subch] = h;
                          printf("[GCM-OBJ] subch=%u SET_OBJECT handle=0x%08X%c",
                                 subch, h, 10); fflush(stdout);
                      } } }
                /* The subchannel is a binding slot, not an engine selector: a
                 * title may bind NV4097 to something other than subchannel 0.
                 * Twisted Metal uses subchannel 1, and caner (canersaka) hit
                 * the same thing in Yakuza Dead Souls, where SPU-built command
                 * lists bind NV4097 elsewhere -- his rsx_live_draw.c masks the
                 * subchannel out of the method for exactly this reason.
                 *
                 * gcm_2d_method only ever handles subchannels 2..7 (NV3062,
                 * NV308A/NV309E, NV3089), so anything arriving on 1 was being
                 * dropped on the floor -- for this title that included
                 * SET_SHADER_PROGRAM, leaving one stale fragment program bound
                 * for every draw in the game. Treat the subchannels the 2D path
                 * does not claim as 3D. GCM_SUBCH1_2D=1 restores the old split.
                 */
                static int s1_2d = -1;
                if (s1_2d < 0) { const char* e = getenv("GCM_SUBCH1_2D"); s1_2d = e ? atoi(e) : 0; }
                /* GCM_SUBCH_GATE=<file>: while <file> exists, log every method on a
                 * non-zero subchannel -- what each binding slot actually carries. */
                { static const char* sg = (const char*)-1; static unsigned poll; static int open_g;
                  if (sg == (const char*)-1) sg = getenv("GCM_SUBCH_GATE");
                  const u32 cm3 = m & 0x1FFCu;
                  /* GCM_VAL_WATCH=<hex>: every FIFO word carrying that value, any method. */
                  { static u32 vw = 1;
                    if (vw == 1) { const char* e = getenv("GCM_VAL_WATCH"); vw = e ? (u32)strtoul(e, 0, 16) : 0; }
                    if (vw && vm_read32(dea) == vw) {
                        static int vn; if (PS3_LOG_FIRST(vn, 64))
                            printf("[valwatch] subch=%u method=0x%04X = 0x%08X get=0x%X%c", subch, m, vw, s_fifo_getoff, 10);
                    } }
                  /* ...and surface offsets (colour 0x210/0x218/0x288/0x28C, zeta 0x214) and texture 1 offset */
                  if (sg && !subch && (cm3 == 0x210 || cm3 == 0x214 || cm3 == 0x218 || cm3 == 0x288 || cm3 == 0x28C ||
                                       cm3 == 0x1A20)) {
                      if ((poll++ & 255u) == 0) { FILE* g = fopen(sg, "rb"); open_g = g != NULL; if (g) fclose(g); }
                      if (open_g) printf("[surfreg] 0x%04X = 0x%08X%c", cm3, vm_read32(dea), 10);
                  }
                  if (sg && subch) {
                      if ((poll++ & 255u) == 0) { FILE* g = fopen(sg, "rb"); open_g = g != NULL; if (g) fclose(g); }
                      if (open_g) printf("[subch] %u 0x%04X = 0x%08X%c", subch, m, vm_read32(dea), 10);
                  } }
                /* Mirror the whole method stream into the live NV4097->D3D12
                 * engine (caner / canersaka). Inert unless RSX_LIVE_DRAW is set.
                 * It wants the raw method with its subchannel bits -- it
                 * canonicalises with & 0x1FFC itself. */
                { static int live = -1;
                  if (live < 0) live = rsx_live_draw_enabled();
                  if (live) rsx_live_draw_method((subch << 13) | m, vm_read32(dea)); }
                /* Resistance port: TRACE_AT_PIC window (armed by cellVdec) --
                 * every FIFO method for a stretch of a movie's playback. */
                { extern volatile int g_port_trace_left;
                  if (g_port_trace_left > 0) { g_port_trace_left--;
                      printf("[mtrace] get=0x%08X subch=%u m=0x%04X = 0x%08X%c",
                             s_fifo_getoff, subch, m, vm_read32(dea), 10); } }
                /* Resistance port: log each (subchannel, method) the first time
                 * the FIFO carries it, with its value -- finds methods the
                 * backend never handles (inline arrays, conditional render). */
                { static unsigned char seen[8][0x800];
                  const u32 mi = (m & 0x1FFCu) >> 2;
                  if (!seen[subch & 7][mi]) { seen[subch & 7][mi] = 1;
                      printf("[mfirst] subch=%u m=0x%04X = 0x%08X%c", subch, m & 0x1FFCu, vm_read32(dea), 10); } }
                /* GCM_METHOD_HIST=<secs>: every <secs>, the most frequent
                 * (subchannel, method) pairs dispatched since the last report --
                 * what a frame is actually made of when nothing draws. */
                { static int hs = -1; static u32 cnt[8][0x800]; static unsigned long long t0;
                  if (hs < 0) { const char* e = getenv("GCM_METHOD_HIST"); hs = e ? atoi(e) : 0; }
                  if (hs > 0) {
                      cnt[subch & 7][(m & 0x1FFCu) >> 2]++;
                      extern unsigned long long ps3_ms_now(void);
                      const unsigned long long now = ps3_ms_now();
                      if (!t0) t0 = now;
                      if (now - t0 >= (unsigned long long)hs * 1000u) {
                          t0 = now;
                          fprintf(stderr, "[mhist]");
                          for (int top = 0; top < 16; top++) {
                              u32 bs = 0, bm = 0, bc = 0;
                              for (u32 sc = 0; sc < 8; sc++) for (u32 mi = 0; mi < 0x800; mi++)
                                  if (cnt[sc][mi] > bc) { bc = cnt[sc][mi]; bs = sc; bm = mi; }
                              if (!bc) break;
                              fprintf(stderr, " %u:%04X=%u", bs, bm << 2, bc);
                              cnt[bs][bm] = 0;
                          }
                          fprintf(stderr, "\n");
                          memset(cnt, 0, sizeof cnt);
                      } } }

                /* And into the platform-neutral register-file draw engine,
                 * which is the macOS path under PS3RECOMP_RSX_ENGINE=dispatch.
                 * Only NV4097 methods (and the driver methods, which it
                 * decodes with their subchannel bits) go there: the 2D
                 * objects' registers (NV0039, NV3062, NV309E, NV3089) alias
                 * NV4097 alpha/blend/colour-mask state once the subchannel
                 * is masked off. */
                const u32 mfull = (subch << 13) | m;
                if (m == 0) gcm_note_set_object(subch, vm_read32(dea));
                const int is3d = subch == 0 ||
                    (subch == 1 && !s1_2d && !gcm_sub1_is_nv0039(&s_state, m));
                if (gcm_method_invalid(is3d ? m : mfull, vm_read32(dea))) goto fifo_recover;
                /* ZCULL / conditional rendering (RPCS3): a draw whose
                 * condition failed never reaches either consumer. */
                const int zdrop = is3d && gcm_zc_pre(m, vm_read32(dea));
                if (is3d && !zdrop && gcm_walk_check(m, vm_read32(dea))) goto fifo_recover;
                if (mfull == 0xFEACu) gcm_flip_state_reset_zc();
                /* GCM_WATCH_M=<method hex>: every write of that method, with
                 * the subchannel and whether the draw engine receives it. */
                { static int wm = -2; static u32 prev = 0xDEADBEEFu, nt = 0;
                  static u32 tv[16][2]; static u32 tc[16]; static u32 ntv = 0;
                  if (wm == -2) { const char* e = getenv("GCM_WATCH_M");
                                  wm = e ? (int)strtoul(e, NULL, 16) : -1; }
                  if (wm >= 0 && (m & 0x1FFCu) == (u32)wm) {
                      const u32 v = vm_read32(dea);
                      if (v != prev) {
                          u32 k = 0;
                          for (; k < ntv; k++) if (tv[k][0] == prev && tv[k][1] == v) break;
                          if (k == ntv && ntv < 16) { tv[ntv][0] = prev; tv[ntv][1] = v; tc[ntv] = 0; ntv++; }
                          if (k < 16) tc[k]++;
                          prev = v;
                          if (++nt % 5000 == 0) {
                              fprintf(stderr, "[mwatch] after %u transitions:", nt);
                              for (u32 i = 0; i < ntv; i++)
                                  fprintf(stderr, " %08X->%08X x%u", tv[i][0], tv[i][1], tc[i]);
                              fprintf(stderr, "\n");
                              ntv = 0;
                          }
                      } } }
                { static int eng = -1;
                  if (eng < 0) eng = rsx_draw_engine_enabled();
                  /* 0xFEB0 is the dispatcher's discard-draw pseudo-method
                   * (RSX_DSP_DISCARD_DRAW): never from the guest. */
                  if (eng && !zdrop && (is3d || mfull >= 0xE000u) && mfull != RSX_DSP_DISCARD_DRAW)
                      rsx_draw_engine_method(mfull, vm_read32(dea)); }

                /* RSX_LIVE_FEED_DBG=1: what the FIFO actually carries. Counted
                 * for any run, live engine or not, so the method stream and the
                 * backend's draw callbacks can be compared in one measurement. */
                { static int fdbg = -1;
                  if (fdbg < 0) fdbg = getenv("RSX_LIVE_FEED_DBG") ? 1 : 0;
                  if (fdbg) {
                      static long long n_be = 0, n_va = 0, n_ia = 0, tick = 0;
                      const u32 cm = m & 0x1FFCu;
                      if      (cm == 0x1808u) n_be++;
                      else if (cm == 0x1814u) n_va++;
                      else if (cm == 0x1824u) n_ia++;
                      if (((++tick) % 200000) == 0)
                          printf("[feed] begin_end=%lld draw_arrays=%lld draw_index=%lld\n",
                                 n_be, n_va, n_ia), fflush(stdout);
                  } }

                /* Driver methods (0xE9xx, 0xEBxx) occupy bits 2-15 of the
                 * FIFO word; the standard 11-bit extraction (w & 0x1FFC)
                 * truncates them.  Reconstruct the full address the same way
                 * the draw-engine call above does. */
                { if (mfull == 0xEB00u || mfull == 0xEB04u) {
                    cellGcmQueueUserCommand(vm_read32(dea));
                } else if (mfull == 0xE920u || mfull == 0xE924u) {
                    gcm_zc_on_flip();
                    cellGcmSetFlipCommand(vm_read32(dea) & 7u);
                } else if (is3d) {
                    if (!zdrop) {
                        rsx_process_method(&s_state, m, vm_read32(dea));
                        if (gcm_method_post_wanted(m)) gcm_method_post(m, vm_read32(dea));
                    }
                } else if (subch == 1 && !s1_2d) {
                    gcm_sub1_nv0039(m, vm_read32(dea));
                } else {
                    s_nv3089_recover = 0;
                    gcm_2d_method(subch, m, vm_read32(dea));
                    if (s_nv3089_recover) {
                        s_nv3089_recover = 0;
                        s_fifo_rec_why = "NV3089 invalid blit";
                        goto fifo_recover;
                    }
                }
                }
            }
            nv308a_flush_writes();
            s_fifo_getoff += 4 + count * 4;
            continue;
        }
        { static int _uw = 0; if (PS3_LOG_FIRST(_uw, 16) && getenv("RTT_DUMP"))
            fprintf(stderr, "[FIFOUW] unknown word 0x%08X at getoff 0x%X\n", w, s_fifo_getoff); }
        s_fifo_getoff += 4;                    /* unknown word: skip */
        continue;
    fifo_recover:
        gcm_fifo_recover(s_fifo_rec_why, &s_state, put);
        why = "recover";
        break;
    }

    /* Walker watchdog. Everything above recovers from a *recognised* stall --
     * a branch we cannot take, a park the title abandoned. A walker that stops
     * for any other reason simply freezes `get` for the rest of the run and
     * nothing renders again, and which of those happens is a race, so the same
     * boot renders one time in five. If `get` has not moved across several
     * passes while `put` is somewhere else, there is work the walker will
     * never reach: resynchronise to the write head. Same trade as the
     * unmapped-get path -- commands are lost, the FIFO lives. */
    /* The stuck-detector above only fires when `get` stops MOVING. A walker
     * going in circles moves plenty -- it just never arrives. Twisted Metal
     * and flOw both write one ring and their JUMPs lead to `put`; VF5 does
     * not. AMGL leaves libgcm's default ring at IO 0 parked on its own JUMP
     * loop and drives the RSX from a second buffer it mapped at IO 0xA00000,
     * so the walker circulates the default ring forever, `get` never reaches
     * `put`, the title's ring is never reclaimed, and AMGL prints "Command
     * Buffer Overflow" until it runs off the end of its own mapping.
     *
     * A drain that burns its ENTIRE word budget without arriving is that
     * signature: no legitimate frame leaves a million words pending, and a
     * genuine burst clears within a tick or two. Circling does not, so
     * require several passes in a row before following the write head.
     *
     * ponytail: consecutive-budget-burn is a heuristic, not a proof. The real
     * fix is knowing which buffer the RSX is bound to -- that needs the
     * SetQueueHandler / context-switch path modelled, not a walker rule. */
    { static int burned = 0, passes = -1;
      if (passes < 0) { const char* e = getenv("GCM_FIFO_CIRCLE_PASSES");
                        passes = e ? atoi(e) : 4; }
      if (passes && budget <= 0 && s_fifo_getoff != put) {
          if (++burned >= passes) {
              static int n = 0;
              if (PS3_LOG_FIRST(n, 8))
                  fprintf(stderr, "[cellGcmSys] FIFO walker circling (get=0x%08X, "
                          "put=0x%08X, full budget burned x%d) -- following the "
                          "write head\n", s_fifo_getoff, put, burned);
              /* What the walker circles in: the words at get, and where the
               * buffer it is in was jumped/called from (GCM_CIRCLE_DUMP=1). */
              if (n <= 3 && getenv("GCM_CIRCLE_DUMP"))
                  for (u32 t = 0; t < 8; t++) {
                      u32 q = (s_cf_i + t) % 8;
                      if (s_cf_w[q]) fprintf(stderr, "[circle-jump] at io=%08X w=%08X\n", s_cf_off[q], s_cf_w[q]);
                  }
              if (n <= 3 && getenv("GCM_CIRCLE_DUMP"))
                  for (int k = -8; k <= 24; k++) {
                      u32 io = s_fifo_getoff + (u32)(k * 4), ea = gcm_io2ea(io);
                      if (ea) fprintf(stderr, "[circle] %+3d io=%08X ea=%08X w=%08X\n", k, io, ea, vm_read32(ea));
                  }
              gcm_fifo_resync_why("circling", &s_fifo_getoff, put);
              burned = 0;
          }
      } else burned = 0; }

    /* A drain stopped on a semaphore acquire (GCM_SEMA_ACQUIRE) holds `get`
     * on purpose: that is the wait, not a stuck walker, and the acquire's own
     * bound (GCM_SEMA_ACQUIRE_MS) is what ends it. So is a self-JUMP park,
     * which GCM_PARK_GRACE_MS ends. This counter counts drain passes, not
     * time: both stop every pass at the same `get`, so it fired a few
     * milliseconds in and resynced to put. For a park that is before the
     * title has patched it into a JUMP to the commands it just wrote at the
     * ring start, and the resync dropped them: inFAMOUS 2 stopped presenting
     * for good that way in about one New Marais run in five ("stuck-get" at a
     * self-jump with no "jump-park" before it). */
    { static u32 last = 0xFFFFFFFFu; static int stuck = 0;
      if (s_sema_blocked_pass || s_park_pass) {
          stuck = 0; last = s_fifo_getoff; s_sema_blocked_pass = 0; s_park_pass = 0; }
      else if (s_fifo_getoff == last && s_fifo_getoff != put) {
          if (++stuck >= 8) { gcm_fifo_dump_around(s_fifo_getoff);
                             gcm_fifo_resync_why("stuck-get", &s_fifo_getoff, put); stuck = 0; }
      } else { stuck = 0; last = s_fifo_getoff; } }

    /* Publish progress: get chases put; ref advances at most ONE queued fence
     * per tick (gcm_ref_push/gcm_ref_publish_one above) so every SET_REFERENCE
     * value stays observable to equality-waiters. The wrap recycle path polls
     * the drained EA. */
    if (getenv("GCM_DRAINDBG")) {
        static int n2 = 0;
        if (n2++ % s_drain_every == 0)
            fprintf(stderr, "[DRAINEND] %s: %08X -> %08X (%u bytes), put=%08X\n",
                    why, start_off, s_fifo_getoff, s_fifo_getoff - start_off, put);
    }
    /* Recycle the TITLE'S OWN command buffer when its callback will not.
     *
     * gCellGcmCurrentContext points at the title's context, not the one
     * cellGcmInit handed back: Virtua Fighter 5's is a 512 KB ring at EA
     * 0x4AE00000, and the buffer-full callback AMGL installs (0x006A8030) only
     * prints "[AMGL]:[ERROR] Command Buffer Overflow!" and returns 0 -- which
     * libgcm's gcmReserve reads as "retry", so the title writes straight past
     * `end` into unmapped memory and never renders again.
     *
     * Hardware never reaches that state because the ring is recycled once the
     * RSX has consumed it. Do the same here, on the title's context, and only
     * when it is provably safe: the walker has drained everything the title
     * submitted (get == put) and `current` is within one reserve of `end`.
     * A title whose own callback recycles never reaches that state, so this is
     * inert for every port that works today.
     *
     * ponytail: append the JUMP at `current` the way the SDK's own callback
     * does, rather than tracking a second copy of the ring's geometry. */
    if (s_gcm_ctx_out_ea) {
        u32 ctx = vm_read32(s_gcm_ctx_out_ea);
        u32 begin = ctx ? vm_read32(ctx + 0x0) : 0;
        u32 end   = ctx ? vm_read32(ctx + 0x4) : 0;
        u32 cur   = ctx ? vm_read32(ctx + 0x8) : 0;
        /* `get == put` -- a fully drained FIFO -- was too strict. It holds
         * while a title is only clearing, and stops holding the moment its SPU
         * work starts producing real command volume: the walker then always has
         * a backlog, the recycle never fires, and the ring overflows again.
         * That is exactly where Virtua Fighter 5 stalls once its "SPU Delegate"
         * group starts feeding the renderer.
         *
         * Hardware does not need a drained FIFO either -- only that the bytes
         * about to be overwritten have already been read. So require the walker
         * to be a margin PAST `begin` instead: the head of the ring is consumed,
         * which is the region the guest is about to write. */
        u32 io_begin_chk = begin ? gcm_ea2io(begin) : 0xFFFFFFFFu;
        int head_consumed =
            (io_begin_chk != 0xFFFFFFFFu) &&
            (s_fifo_getoff == put ||
             (s_fifo_getoff > io_begin_chk &&
              s_fifo_getoff - io_begin_chk >= GCM_RECYCLE_MARGIN));
        /* Already overrun. gcmReserve writes past `end` when its callback
         * declines to recycle (AMGL's only prints), and once the guest is out
         * there the walker follows into memory that is not the ring: `get`
         * stops advancing, head_consumed can never become true again, and the
         * title is wedged for good -- which is exactly where Virtua Fighter 5
         * stops, at the same flip every run however long it is left.
         *
         * There is nothing left to protect at that point. Recycling loses
         * whatever was written past the end; not recycling loses the rest of
         * the run. */
        /* A pass that stopped at a flip has a backlog on purpose (the next
         * frame), and teleporting `get` to `begin` would drop it -- fence
         * writes included, so the title then waits on a reference that never
         * comes (The Simpsons Arcade Game froze at its 4th recycle). The next
         * pass drains to `put` and recycles then. */
        if (!strcmp(why, "flip") && s_fifo_getoff != put) head_consumed = 0;
        /* Only once the title has been SEEN to overrun. Near `end` is also
         * where a title whose callback does recycle sits just before calling
         * it: Tornado Outbreak's steps its context through 64 KB segments of a
         * larger ring, and recycling under it from this thread rewound put/get
         * behind its callback's bookkeeping -- which then waited forever for a
         * GET that could not arrive. A callback that declines (AMGL's) always
         * overruns first, so the first overrun is recycled by the path above
         * and near-end recycling is enabled from then on. */
        static int s_title_overran = 0;
        if (s_fifo_recycle_waiting) head_consumed = 0;   /* its callback is recycling */
        else if (begin && end > begin && cur >= end) { head_consumed = 1; s_title_overran = 1; }
        if (begin && end > begin && cur >= begin && cur + GCM_RECYCLE_SLACK >= end
                && head_consumed && s_title_overran) {
            u32 io_begin = gcm_ea2io(begin);
            if (io_begin != 0xFFFFFFFFu) {
                u32 jmp_at = (cur + 4 <= end) ? cur : end - 4;
                vm_write32(jmp_at, 0x20000000u | io_begin);   /* JUMP -> begin */
                vm_write32(ctx + 0x8, begin);                 /* current = begin */
                vm_write32(GCM_CONTROL_GUEST_ADDR + 0, io_begin);  /* put */
                s_fifo_getoff = io_begin;                          /* get */
                put = io_begin;
                static int n = 0;
                if (PS3_LOG_FIRST(n, 8))
                    fprintf(stderr, "[cellGcmSys] recycled the title's own ring "
                            "(ctx=0x%08X begin=0x%08X end=0x%08X) -- its callback "
                            "does not%c", ctx, begin, end, 10);
            }
        }
    }

    g_gcm_fifo_drained_ea = gcm_io2ea(s_fifo_getoff);
    /* GCM_GET_EQ_PUT=1: publish `get` as having reached `put` rather than where
     * the walker actually is. A probe, not a fix -- it removes the back-pressure
     * a title uses to avoid overwriting commands the GPU has not read. It exists
     * to answer one question for a title that reports its ring full while the
     * walker says it is drained: is the title reading `get`, or something else?
     * If the overflow survives this, `get` is not what it consults. */
    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, gcm_get_eq_put() ? put : s_fifo_getoff);

    AcquireSRWLockExclusive(&s_ref_pub_lock);                           /* ref */
    gcm_ref_publish_one();
    ReleaseSRWLockExclusive(&s_ref_pub_lock);
}

/* FIFO command-buffer-full callback body. The title's inline gcmReserve calls
 * context->callback(context, count) (routed here via the synthetic OPD) when the
 * ring's `current` nears `end`. Real libgcm flushes, waits for the RSX `get` to
 * pass, then recycles `current` to `begin`. We mirror that: wait (bounded) for
 * the ticker drain to consume everything up to `current`, then reset current to
 * begin. Runs on the guest thread; the ticker owns all rendering, so we only
 * pointer-shuffle here (the drain's own wrap-detect handles s_get). Without this
 * the ring never recycles and the whole pipeline wedges once the FIFO wraps
 * (~200 frames at the 256KB default), which looked like a GPU/driver hang. */
static u32 gcm_ea2io(u32 ea)
{
    u32 page = ea >> 20;
    if (page >= 65536) return 0xFFFFFFFFu;
    u16 io_page = s_io_address_table[page];
    if (io_page == 0xFFFF) return 0xFFFFFFFFu;
    return ((u32)io_page << 20) | (ea & 0xFFFFFu);
}

/* RPCS3 mode, GCM_RING_SEGMENT (default on; 0 = the full drain below): the
 * runtime ring is used in 32 KB segments, as RPCS3's cellGcmCallback does.
 * Flush up to `current`, jump to the next segment and wait only while the RSX
 * is still reading that segment. The full drain jumped back to begin and waited
 * for GET to reach it: the guest thread then waited for every command queued
 * before the wrap -- label waits on SPU jobs and the flip included ([hitch]
 * "ring"). Returns 0 when the context is not the runtime ring (full drain). */
#define GCM_RING_SEG 0x8000u
static int gcm_ring_segmented(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("GCM_RING_SEGMENT"); v = !(e && *e == '0'); }
    return v;
}

static int gcm_r2rsx_recycle_segment(u32 ctx_ea, u32 cur, u32 count)
{
    const u32 base = s_ring_base_ea, nseg = s_ring_size / GCM_RING_SEG;
    if (!base || nseg < 2 || cur < base || cur > base + s_ring_size) return 0;
    if (count > (GCM_RING_SEG - 8) / 4) return 0;   /* reservation larger than a segment */
    u32 next = (cur - base) / GCM_RING_SEG + 1;
    if (next >= nseg) next = 0;
    const u32 nb = base + next * GCM_RING_SEG, ne = nb + GCM_RING_SEG - 4;   /* 4 bytes for the next jump */
    const u32 io_cur = gcm_ea2io(cur), io_nb = gcm_ea2io(nb), io_ne = gcm_ea2io(ne);
    if (io_cur == 0xFFFFFFFFu || io_nb == 0xFFFFFFFFu || io_ne == 0xFFFFFFFFu || io_ne - io_nb != GCM_RING_SEG - 4)
        return 0;
    /* put stops the RSX in front of the jump; the title's next flush lets it through */
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, io_cur);
    atomic_thread_fence(memory_order_release);
    vm_write32(cur, 0x20000000u | io_nb);
    vm_write32(ctx_ea + 0x0, nb);
    vm_write32(ctx_ea + 0x4, ne);
    vm_write32(ctx_ea + 0x8, nb);
    r2rsx_wait_get_outside(io_nb, io_ne + 4, 60000);
    return 1;
}

void cellGcm_fifo_recycle_count(u32 ctx_ea, u32 count);
void cellGcm_fifo_recycle(u32 ctx_ea)
{
    cellGcm_fifo_recycle_count(ctx_ea, 0);
}

/* count = the words the title is reserving (r4 of the SDK callback). */
void cellGcm_fifo_recycle_count(u32 ctx_ea, u32 count)
{
    if (!ctx_ea) return;
    u32 begin   = vm_read32(ctx_ea + 0x0);
    u32 current = vm_read32(ctx_ea + 0x8);
    if (current <= begin) return;                       /* nothing to recycle */

    if (getenv("CELLMARK_BLINKDBG"))
        printf("[RECYCLE] ring wrap: current=0x%08X -> begin=0x%08X\n", current, begin);

    static int s_recdbg = -1;
    if (s_recdbg < 0) s_recdbg = getenv("GCM_RECDBG") ? 1 : 0;
    if (s_recdbg)
        fprintf(stderr, "[REC>] tid=%lu begin=%08X cur=%08X put=%08X get=%08X drained=%08X\n",
                GetCurrentThreadId(), begin, current,
                vm_read32(GCM_CONTROL_GUEST_ADDR + 0),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 4),
                g_gcm_fifo_drained_ea);

    /* Do what the SDK's default command-buffer-full callback does: append a
     * JUMP-to-begin at the write head and move `put` to begin. The FIFO walker
     * consumes the tail, follows the jump, and idles at begin; then it's safe
     * for the guest to write from begin (that region was consumed long ago). */
    u32 io_begin = gcm_ea2io(begin);
    if (s_r2rsx) {
        if (gcm_ring_segmented()) {
            if (gcm_r2rsx_recycle_segment(ctx_ea, current, count))
                return;
            /* Full drain over the whole runtime ring, not the current segment. */
            if (s_ring_base_ea && begin >= s_ring_base_ea && begin < s_ring_base_ea + s_ring_size) {
                begin = s_ring_base_ea;
                io_begin = gcm_ea2io(begin);
                vm_write32(ctx_ea + 0x0, begin);
                vm_write32(ctx_ea + 0x4, s_ring_base_ea + s_ring_size);
            }
        }
        /* RPCS3 mode: flush the tail, jump back to begin and wait for the RSX
         * to get there (what RPCS3's cellGcmCallback waits for). */
        if (io_begin != 0xFFFFFFFFu) {
            vm_write32(current, 0x20000000u | io_begin);
            atomic_thread_fence(memory_order_release);
            vm_write32(GCM_CONTROL_GUEST_ADDR + 0, io_begin);
            r2rsx_wait_get(io_begin, 60000);
        }
        vm_write32(ctx_ea + 0x8, begin);
        return;
    }
    const u32 back_jumps0 = s_fifo_back_jumps;
    if (io_begin != 0xFFFFFFFFu) {
        vm_write32(current, 0x20000000u | io_begin);            /* JUMP begin  */
        /* The jump has to be in guest memory before `put` moves behind the
         * walker's `get`. Program order is not enough on arm64: the walker
         * may observe the new `put`, find `get` != `put`, and read the old
         * word at `current` before the jump lands there. */
        atomic_thread_fence(memory_order_release);
        vm_write32(GCM_CONTROL_GUEST_ADDR + 0, io_begin);        /* put = begin */
    }

    /* Wait (bounded ~2s) for the walker to consume the tail + take the jump so
     * no commands are lost; a stalled ticker degrades to dropped commands.
     * "Took the jump" is a backward JUMP counted after the one written above.
     * Comparing the drained EA with begin let the wait through at once when
     * the walker had not moved yet (drained starts at begin), and the title
     * then overwrote megabytes of commands the RSX had never read. */
    /* The bound is on the walker making no progress, not on the total wait: a
     * title that fills a 3 MB ring with hundreds of tiny frames at boot (God of
     * War HD) leaves a backlog the draw engine needs tens of seconds to execute,
     * and giving up after 2 s handed the title megabytes of commands to write
     * over. A walker that has stopped moving still ends the wait after 2 s, and
     * 60 s caps the whole thing. */
    int spins = 0, idle = 0;
    u32 last_get = s_fifo_getoff;
    s_fifo_recycle_waiting = 1;
    while (io_begin != 0xFFFFFFFFu && s_fifo_back_jumps == back_jumps0 &&
           idle < 2000 && spins < 60000) {
        Sleep(1); spins++;
        if (s_fifo_getoff != last_get) { last_get = s_fifo_getoff; idle = 0; }
        else idle++;
        if (s_recdbg && spins % 1000 == 0)
            fprintf(stderr, "[REC~] %d ms: walker get=%08X put=%08X back_jumps=%u\n",
                    spins, s_fifo_getoff, vm_read32(GCM_CONTROL_GUEST_ADDR + 0),
                    s_fifo_back_jumps);
    }
    if (s_fifo_back_jumps == back_jumps0 && io_begin != 0xFFFFFFFFu) {
        static int warned = 0;
        if (PS3_LOG_FIRST(warned, 4))
            printf("[cellGcmSys] fifo recycle: drain stalled (drained=0x%08X begin=0x%08X, %d ms)\n",
                   g_gcm_fifo_drained_ea, begin, spins);
    }

    vm_write32(ctx_ea + 0x8, begin);                    /* recycle ring to base */
    s_fifo_recycle_waiting = 0;                          /* current no longer reads end */

    if (s_recdbg)
        fprintf(stderr, "[REC<] tid=%lu cur-now=%08X put=%08X get=%08X drained=%08X spins=%d\n",
                GetCurrentThreadId(), vm_read32(ctx_ea + 0x8),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 0),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 4),
                g_gcm_fifo_drained_ea, spins);
}

/* A host that brings its backend up on another thread -- a frame clock that
 * opens the window while the title boots -- can register the draw engine after
 * the title has already called cellGcmSetDisplayBuffer. The engine then knows
 * no scanout, and every flip presents whatever surface happens to be bound:
 * Twisted Metal on Linux showed an offscreen target instead of its composited
 * display buffer. rsx_draw_engine_init calls this to catch up. */
void cellGcm_replay_display_buffers_to_engine(void)
{
    for (u32 i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; i++)
        if (s_display_buffer_set[i])
            rsx_draw_engine_set_display_buffer(i, 0, s_display_buffers[i].offset,
                                               s_display_buffers[i].pitch,
                                               s_display_buffers[i].width,
                                               s_display_buffers[i].height);
}

/* RPCS3 mode: append command words to a context (0 = the title's current one)
 * the way libgcm's inline writers do, calling its buffer-full callback first
 * when they do not fit. The title's next flush makes them visible to the RSX. */
static void gcm_r2rsx_emit(u32 ctx, const u32* words, u32 n)
{
    if (!ctx) ctx = s_gcm_ctx_out_ea ? vm_read32(s_gcm_ctx_out_ea) : s_gcm_context_ea;
    if (!ctx) return;
    if (vm_read32(ctx + 8) + 4u * n >= vm_read32(ctx + 4)) {
        const u32 cb_opd = vm_read32(ctx + 0xC);
        if (cb_opd && vm_read32(cb_opd) == GCM_FIFO_CALLBACK_SENTINEL_EA)
            cellGcm_fifo_recycle(ctx);
        else if (cb_opd && g_ps3_guest_caller)
            g_ps3_guest_caller(cb_opd, ctx, n, 0, 0, 0, 0, 0, 0);
    }
    u32 cur = vm_read32(ctx + 8);
    for (u32 i = 0; i < n; i++) vm_write32(cur + 4u * i, words[i]);
    vm_write32(ctx + 8, cur + 4u * n);
}

/* NID: 0xDC09357E */
static int gcm_hle_validate(void);

s32 cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch,
                            u32 width, u32 height)
{
    printf("[cellGcmSys] SetDisplayBuffer(id=%u, offset=0x%X, pitch=%u, %ux%u)\n",
           bufferId, offset, pitch, width, height);

    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return gcm_hle_validate() ? CELL_GCM_ERROR_FAILURE : CELL_GCM_ERROR_INVALID_VALUE;
    if (s_r2rsx) r2rsx_set_display_buffer(bufferId, offset, pitch, width, height);

    s_display_buffers[bufferId].offset = offset;
    s_display_buffers[bufferId].pitch  = pitch;
    s_display_buffers[bufferId].width  = width;
    s_display_buffers[bufferId].height = height;
    s_display_buffer_set[bufferId] = 1;

    /* Tell the live engine too, or it has no registered scanout to present and
     * falls back to whatever surface happens to be current. Display buffers are
     * always in RSX local memory (location 0). */
    if (rsx_live_draw_enabled())
        rsx_live_draw_set_display_buffer(bufferId, 0, offset, pitch, width, height);
    if (rsx_draw_engine_enabled())
        rsx_draw_engine_set_display_buffer(bufferId, 0, offset, pitch, width, height);

    return CELL_OK;
}

/* Backend query (not a guest API): is this raw RSX offset a registered
 * display buffer? Used to split display draws (-> backbuffer) from
 * offscreen render-to-texture passes. */
int cellGcmOffsetIsDisplay(u32 offset)
{
    for (int i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; i++)
        if (s_display_buffer_set[i] && s_display_buffers[i].offset == offset)
            return 1;
    return 0;
}

/* NID: 0xEAA52F23 */
/* How many display buffers the title has actually registered. cellResc needs
 * it to know how far to rotate its flip target; nothing else about the set is
 * anyone else's business, so this stays a count rather than exposing the array. */
u32 cellGcm_display_buffer_count(void)
{
    u32 n = 0;
    for (u32 i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; i++)
        if (s_display_buffer_set[i]) n++;
    return n;
}

static s32 gcm_flip_request(u32 bufferId, int in_fifo)
{
    if (s_r2rsx) {
        /* RPCS3 gcmSetPrepareFlip: GCM_FLIP_COMMAND (0xFEAC) into the stream. */
        if (bufferId > 7) return CELL_GCM_ERROR_FAILURE;
        const u32 w[2] = { 0x0004FEACu, bufferId };
        gcm_r2rsx_emit(0, w, 2);
        return CELL_OK;
    }
    /* GCM_FLIPCOUNT=1: every flip, with a timestamp. FLIP_DBG caps at 20 lines,
     * which answers "did it ever flip?" and not "is it still flipping, and how
     * fast?" -- the question that matters when a title stops appearing on
     * screen. Virtua Fighter 5 does not use the in-FIFO 0xFEADxxxx flip word at
     * all (that counter stays at zero for the whole run); it flips through
     * cellGcmSetFlip, so this is where its frame rate actually is. */
    { static int dbg = -1;
      if (dbg < 0) dbg = getenv("GCM_FLIPCOUNT") ? 1 : 0;
      if (dbg) {
          extern unsigned long long ps3_ms_now(void);
          static unsigned long long n = 0, t0 = 0;
          unsigned long long now = ps3_ms_now();
          if (!t0) t0 = now;
          ++n;
          if (n <= 400ull || (n % 10ull) == 0)
              fprintf(stderr, "[flip] %llu at %llu ms%c", n, now - t0, 10);
      } }

    /* GCM_FLIP_BT=<n>: dump the flipping thread's guest stack for the flips
     * around n. When a title renders correctly for a while and then stops
     * submitting -- no error, no unresolved import, no overflow -- the only
     * thing separating the last good frame from the first missing one is what
     * the render path was doing on each. This is the window onto that. */
    { static long long at = -2;
      if (at == -2) { const char* e = getenv("GCM_FLIP_BT"); at = e ? atoll(e) : -1; }
      if (at >= 0) {
          static unsigned long long m = 0;
          ++m;
          if ((long long)m >= at - 5 && (long long)m <= at + 5) {
              extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
              extern void ppu_dump_guest_stack(ppu_context*, const char*);
              if (g_active_ctx) {
                  char tag[48];
                  snprintf(tag, sizeof tag, "flip#%llu", m);
                  ppu_dump_guest_stack(g_active_ctx, tag);
                  { extern void ppu_dump_bctrl_ring(uint32_t, const char*);
                    ppu_dump_bctrl_ring((uint32_t)g_active_ctx->thread_id, tag); }
              }
          }
      } }

    { static int _n=0; if (getenv("FLIP_DBG") && PS3_LOG_FIRST(_n, 20))
        fprintf(stderr, "[FLIP] SetFlipCommand(buf=%u) set=%d\n",
                bufferId, bufferId < CELL_GCM_MAX_DISPLAY_BUFFER_NUM ? s_display_buffer_set[bufferId] : -1); }
    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    if (!s_display_buffer_set[bufferId])
        return CELL_GCM_ERROR_INVALID_VALUE;


    /* in_fifo: the flip sits in the command stream as a GCM_FLIP_MARKER, and
     * the drain does this half when it reaches it (see the 0xFEAD case). */
    { extern int g_drawlog_left;
      if (g_drawlog_left > 0)
          fprintf(stderr, "[DRAWLOG] PPU flip request buf=%u in_fifo=%d (walker get=%08X put=%08X, flips queued %u reached %u)%c",
                  bufferId, in_fifo, s_fifo_getoff, vm_read32(GCM_CONTROL_GUEST_ADDR + 0),
                  s_fifo_flips_queued, s_fifo_flips_reached, 10); }
    if (!in_fifo) {
        gcm_set_current_display_buffer(bufferId);
        s_flip_pending = 1;   /* ticker: present BEFORE the next drain */
        s_flip_request_count++;
        s_flip_direct_pending = 1;
    }
    /* Flip requested but not yet shown: a subsequent cellGcmSetWaitFlip blocks
     * until the present thread's cellGcmTickFlip marks it done (vsync). */
    FLIP_LOCK();
    s_flip_status = CELL_GCM_FLIP_STATUS_WAITING;
    s_flip_req_seq++;
    FLIP_UNLOCK();
    s_last_flip_time = get_timestamp_ns();
    if (PS3_FT_ON()) { const uint64_t t = ps3_ft_now();
                       ps3_ft_span(PS3_FT_FLIP_REQ, 0, bufferId, (uint32_t)in_fifo, t, t); }

    /* Invoke via OPD resolution, not a raw call into guest code.
     *
     * On hardware this callback fires when the flip COMPLETES, at vsync --
     * which is what cellGcmTickFlip does. Calling it again here, at request
     * time, is a compatibility wart: some titles need the early edge, but a
     * title that issues its first flip while still initialising then runs its
     * handler against half-built state. Tokyo Jungle faults that way, in
     * func_001F2D3C, dereferencing a singleton it has not constructed yet.
     * GCM_FLIPCB_ONTICK=1 leaves the callback to the tick alone. */
    if (s_flip_handler_opd && g_ps3_guest_caller && !gcm_flipcb_on_tick_only())
        g_ps3_guest_caller(s_flip_handler_opd, 0, 0, 0, 0, 0, 0, 0, 0);  /* head 0 = primary display */

    return CELL_OK;
}

s32 cellGcmSetFlipCommand(u32 bufferId) { return gcm_flip_request(bufferId, 0); }

/* The real _cellGcmSetFlipCommand appends the flip to the command buffer, so
 * the RSX flips once it has executed everything queued before it. Firing it
 * at call time instead presented whatever the drain had reached by then --
 * and the title has not even flushed its frame's tail yet: The Simpsons Arcade
 * Game calls it with ctx->current ~2.4 KB past put, so frames showed without
 * their last draws (a dialog, a dimming overlay) for one flip. Write a marker
 * at ctx->current instead; the drain stops at it and fires the flip there,
 * exactly as it does for the 0xFEAD words a statically-linked libgcm emits.
 * Returns 0 when there is no room or ctx is not a mapped command buffer. */
static int gcm_flip_into_fifo(u32 ctx, u32 bufferId)
{
    if (!ctx || bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM ||
        !s_display_buffer_set[bufferId]) return 0;
    const u32 end = vm_read32(ctx + 4), cur = vm_read32(ctx + 8);
    /* Never near the end: those bytes are the title's reserve before its
     * ring wraps, and the recycle above keys on them. */
    if (cur + 8 + GCM_RECYCLE_SLACK > end || gcm_ea2io(cur) == 0xFFFFFFFFu) return 0;
    vm_write32(cur, GCM_FLIP_MARKER | bufferId);
    s_fifo_flips_queued++;
    /* Wrap here, on the guest thread, once the ring is nearly used -- what the
     * SDK's buffer-full callback does. Otherwise the drain thread recycles it
     * (see "recycled the title's own ring"), rewriting ctx->current and put
     * while the guest may be between advancing current and flushing: put
     * then lands on begin, the guest's closing fence is never executed, and
     * it spins on `ref` for good. Safe when the walker has left the head of
     * the ring well behind, which is the region the guest writes next.
     *
     * Only for a ring the runtime owns, i.e. one whose buffer-full callback is
     * still the runtime's (cellGcmSetupContext installs it). A title with its
     * own callback manages its own buffers, and its context need not be a ring
     * at all: Killzone 3 flips on a 0x30000-byte draw context whose `end`
     * moves down as it allocates from the top, so it always looks nearly full.
     * Rewinding it to `begin` in mid-frame cut the frame short, dropping the
     * fence release in its tail, and the title then waited for it forever. */
    const u32 begin = vm_read32(ctx), io_begin = gcm_ea2io(begin);
    const u32 cb_opd = vm_read32(ctx + 0xC);
    const int runtime_ring = cb_opd && vm_read32(cb_opd) == GCM_FIFO_CALLBACK_SENTINEL_EA;
    if (runtime_ring && end - (cur + 4) < GCM_FLIP_WRAP_BYTES && io_begin != 0xFFFFFFFFu &&
        s_fifo_getoff >= io_begin + GCM_RECYCLE_MARGIN) {
        vm_write32(cur + 4, 0x20000000u | io_begin);   /* JUMP -> begin */
        vm_write32(ctx + 8, begin);
        return 1;
    }
    vm_write32(ctx + 8, cur + 4);
    return 1;
}

/* cellGcmSetFlip(context, buffer_id) — immediate flip request. PSL1GHT's
 * gcmSetFlip import (NID 0xDC09357E) lands here; without it vkcube's flip was
 * a no-op, GetFlipStatus never cleared, and init timed out into exit(-1). */
s32 cellGcmSetFlip(void* context, u32 bufferId)
{
    if (s_r2rsx) {
        s32 r = _cellGcmSetFlipCommand(context, bufferId);
        return r < 0 ? CELL_GCM_ERROR_FAILURE : CELL_OK;
    }
    (void)context;   /* command context — flip is immediate in HLE */
    return cellGcmSetFlipCommand(bufferId);
}

/* NID: 0xD01B570F */
s32 cellGcmSetFlipCommandWithWaitLabel(u32 bufferId, u32 labelIndex, u32 labelValue)
{
    if (labelIndex >= CELL_GCM_MAX_LABEL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    if (s_r2rsx) {   /* RPCS3: flip, then the label write (by the PPU) */
        s32 r = cellGcmSetFlipCommand(bufferId);
        vm_write32(R2RSX_LABEL_EA + labelIndex * 0x10u, labelValue);
        return r;
    }
    /* Wait until the label reaches the expected value (instant in HLE). Labels
     * live in guest memory (see cellGcmGetLabelAddress) so the game can poll. */
    vm_write32(GCM_LABEL_GUEST_BASE + labelIndex * GCM_LABEL_STRIDE, labelValue);

    return cellGcmSetFlipCommand(bufferId);
}

/* NID: 0xA2478CA3 */
s32 cellGcmSetPrepareFlip(void* ctx, u32 bufferId)
{
    (void)ctx;  /* command buffer context -- not used in HLE */

    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    if (!s_display_buffer_set[bufferId])
        return CELL_GCM_ERROR_INVALID_VALUE;

    if (s_r2rsx) {
        const u32 w[2] = { 0x0004FEACu, bufferId };
        gcm_r2rsx_emit((u32)(uintptr_t)ctx, w, 2);
        return (s32)bufferId;
    }

    printf("[cellGcmSys] SetPrepareFlip(bufferId=%u)\n", bufferId);

    gcm_set_current_display_buffer(bufferId);
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    s_flip_request_count++;
    s_last_flip_time = get_timestamp_ns();

    /* Invoke the guest flip handler via OPD resolution -- s_flip_handler holds
     * the raw guest OPD (e.g. 0x530D70); calling it as a host function pointer
     * jumps into guest code and crashes. See cellGcmSetFlipCommand for why
     * GCM_FLIPCB_ONTICK exists. */
    if (s_flip_handler_opd && g_ps3_guest_caller && !gcm_flipcb_on_tick_only())
        g_ps3_guest_caller(s_flip_handler_opd, 0, 0, 0, 0, 0, 0, 0, 0);

    return CELL_OK;
}

/* NID: 0x1BFAB6EE */
CellGcmDisplayInfo* cellGcmGetDisplayBufferByFlipIndex(u32 index)
{
    u32 id = index % CELL_GCM_MAX_DISPLAY_BUFFER_NUM;
    return &s_display_buffers[id];
}

/* NID: 0x8BADE8BE */
u32 cellGcmGetCurrentDisplayBufferId(void)
{
    if (s_r2rsx) return r2rsx_current_display_buffer();
    return s_current_display_buffer_id;
}

/* NID: 0xD9B7653E */
void cellGcmSetFlipHandler(CellGcmFlipHandler handler)
{
    /* `handler` is a guest OPD address — store as such and remember
     * the legacy host-typed value too for any callers still using
     * the old API style. Real dispatch goes through
     * cellGcmDispatchVBlank()/DispatchFlip() via g_ps3_guest_caller. */
    printf("[cellGcmSys] SetFlipHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_flip_handler_opd = (u32)(size_t)handler;
    s_flip_handler = handler;
    { u32 c = s_flip_handler_opd ? vm_read32(s_flip_handler_opd) : 0; if (c) s_flip_handler_code = c; }
    if (s_r2rsx) r2rsx_set_flip_handler(s_flip_handler_opd);
    gcm_update_handler_mask();
}

/* NID: 0xA547ADDE */
void cellGcmSetVBlankHandler(CellGcmVBlankHandler handler)
{
    printf("[cellGcmSys] SetVBlankHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_vblank_handler_opd = (u32)(size_t)handler;
    s_vblank_handler = handler;
    { u32 c = s_vblank_handler_opd ? vm_read32(s_vblank_handler_opd) : 0; if (c) s_vblank_handler_code = c; }
    if (s_r2rsx) r2rsx_set_vblank_handler(s_vblank_handler_opd);
    gcm_update_handler_mask();
}

/* NID: 0xF9BFCDA3 */
void cellGcmSetSecondVHandler(CellGcmSecondVHandler handler)
{
    printf("[cellGcmSys] SetSecondVHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_second_v_handler_opd = (u32)(size_t)handler;
    s_second_v_handler = handler;
}

/* NID: 0x0B4B62D5 */
void cellGcmSetUserHandler(CellGcmUserHandler handler)
{
    printf("[cellGcmSys] SetUserHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_user_handler_opd = (u32)(size_t)handler;
    s_user_handler = handler;
    if (s_r2rsx) r2rsx_set_user_handler(s_user_handler_opd);
    gcm_update_handler_mask();
}

/* NID: 0x21AC3697 */
u64 cellGcmGetLastFlipTime(void)
{
    if (s_r2rsx) return r2rsx_last_flip_time();
    return s_last_flip_time;
}

/* ---------------------------------------------------------------------------
 * Address translation / IO mapping
 * -----------------------------------------------------------------------*/

/* NID: 0x0E6B0DFF */
/* Mirror the host tables into the guest window the title will index. */
static void gcm_publish_offset_tables(void)
{
    for (u32 i = 0; i < GCM_OFFSET_TABLE_PAGES; i++) {
        vm_write16(GCM_OFFSET_TABLE_IO_ADDR + i * 2, s_io_address_table[i]);
        vm_write16(GCM_OFFSET_TABLE_EA_ADDR + i * 2, s_ea_address_table[i]);
    }
}

s32 cellGcmGetOffsetTable(CellGcmOffsetTable* table)
{
    u32 ea = (u32)(uintptr_t)table;
    if (!ea)
        return CELL_GCM_ERROR_INVALID_VALUE;

    gcm_publish_offset_tables();

    /* The guest struct is two 4-byte EAs, not two host pointers. */
    vm_write32(ea + 0, GCM_OFFSET_TABLE_IO_ADDR);
    vm_write32(ea + 4, GCM_OFFSET_TABLE_EA_ADDR);
    return CELL_OK;
}

/* NID: 0xDB769B32 */
static u32 gcm_io_alloc(u32 size);   /* defined below */

s32 cellGcmAddressToOffset(u32 address, u32* offset)
{
    /* `offset` is a GUEST address; write big-endian via vm_write32. */
    uint32_t off_ea = (uint32_t)(uintptr_t)offset;
    if (!off_ea)
        return CELL_GCM_ERROR_INVALID_VALUE;
    /* A2O_DBG=<N>: the EA->offset conversions a title asks for. A texture bound
     * at an offset nothing ever writes usually means the offset came from an EA
     * in a space we classified wrongly, and that is only visible here. */
    { static int cap = -1, k = 0;
      if (cap < 0) { const char* e = getenv("A2O_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && k < cap) { k++;
        u32 nz = 0;
        for (u32 i = 0; i < 0x400u; i += 13) if (vm_base && vm_base[address + i]) nz++;
        printf("[A2O] ea=0x%08X  (first 1KB nonzero=%u/79)\n", address, nz); } }

    /* Local memory: offset = address - localBase. Record the offset page as
     * LOCAL-derived so cellGcmResolveOffset can disambiguate it from an IO
     * (main-memory) offset with the same page number -- the two offset spaces
     * overlap, and a title with a large IO region (gcm/cube: 16MB) otherwise
     * gets its VRAM vertex/texture/FP reads resolved into the empty command
     * buffer. */
    if (address >= s_config.localAddress &&
        address < s_config.localAddress + s_config.localSize) {
        u32 loff = address - s_config.localAddress;
        if ((loff >> 20) < sizeof(s_local_offset_page)) s_local_offset_page[loff >> 20] = 1;
        vm_write32(off_ea, loff);
        return CELL_OK;
    }

    /* IO-mapped main memory: consult offset table */
    u32 page = address >> 20;
    if (page < 65536 && s_io_address_table[page] != 0xFFFF) {
        u32 io_page = s_io_address_table[page];
        vm_write32(off_ea, (io_page << 20) | (address & 0xFFFFF));
        return CELL_OK;
    }

    /* Legacy fallback for initial IO region */
    if (s_config.ioAddress != 0 && address >= s_config.ioAddress &&
        address < s_config.ioAddress + s_config.ioSize) {
        vm_write32(off_ea, address - s_config.ioAddress);
        return CELL_OK;
    }

    /* Auto-map unmapped main memory. On real hardware PSL1GHT pre-maps its
     * whole RSX heap in gcmInitBody, so its libraries never call MapMainMemory
     * before handing an EA to the RSX (Tiny3D's command ring lives in an
     * sys_mmapper region). Mirror that by mapping the 1MB page on first use.
     *
     * The bound used to be a flat `< 0x40000000`, which is not a description of
     * anything -- RSX local memory is already handled at the top of this
     * function against its real base, and the raw-SPU window at 0x30000000 sits
     * *below* the old bound, so it was not keeping that out either. What it did
     * do was exclude titles whose RSX-visible heap lives high: Virtua Fighter 5
     * maps 0x4A900000..0x4B400000 and then hands us 0x4B400040, which fell past
     * 0x40000000, skipped the auto-map, and failed -- 976 times a boot, leaving
     * the title with no offsets to draw with (20 draws in five minutes).
     *
     * So the test is now what it always meant: anything below RSX local memory
     * is main memory. Local is checked first and returns above, so reaching
     * here with an address under localAddress means an unmapped main-memory
     * page, which is exactly the case this fallback exists for. */
    /* NULL is not a buffer. Auto-mapping it costs an IO page and, worse, hands
     * the title a real-looking offset for guest address 0 -- so an EA the title
     * computed as NULL (a failed allocation, an out-param it never filled)
     * silently becomes a valid RSX offset instead of an error it could notice.
     * The Orange Box's chooser asks for exactly this, and the page it burned was
     * the one the following cellGcmMapMainMemory would otherwise have taken. */
    if (address < 0x100000u) {
        printf("[cellGcmSys] AddressToOffset REFUSED: ea 0x%08X is not a mappable "
               "address (page 0)\n", address);
        vm_write32(off_ea, 0);
        return CELL_GCM_ERROR_INVALID_VALUE;
    }

    u32 auto_map_limit = s_config.localAddress ? s_config.localAddress : 0x40000000u;
    if (address < auto_map_limit && s_gcm_initialized) {
        u32 ea_page = address & ~0xFFFFFu;
        u32 io      = gcm_io_alloc(0x100000u);
        if (io == 0xFFFFFFFFu) {
            printf("[cellGcmSys] AddressToOffset: no free IO page to auto-map ea 0x%08X\n", address);
            vm_write32(off_ea, 0);
            return CELL_GCM_ERROR_FAILURE;
        }
        populate_offset_table(ea_page, io, 0x100000u);
        /* Record it in the mapping list too, not just the offset table.
         *
         * These are two different structures answering two different questions,
         * and only the offset table was being filled in. The FIFO walker
         * resolves io -> ea through the MAPPING LIST (find_io_mapping_by_io),
         * so an auto-mapped range was invisible to it: a JUMP into one reported
         * "no IO mapping", the walker resynced straight to put, and every fence
         * in the skipped range was lost -- which strands a title spinning on
         * ctrl->ref with no way to see why. Both paths have to agree about what
         * is mapped. */
        IoMapping* m = find_free_mapping();
        if (m) {
            m->ea = ea_page; m->io = io; m->size = 0x100000u; m->active = 1;
            s_io_mapping_count++;
        } else {
            printf("[cellGcmSys] WARNING: auto-map has no free IO mapping slot; "
                   "the FIFO will not be able to walk io 0x%08X\n", io);
        }
        printf("[cellGcmSys] AddressToOffset: auto-mapped ea 0x%08X -> io 0x%08X\n",
               ea_page, io);
        vm_write32(off_ea, io | (address & 0xFFFFFu));
        return CELL_OK;
    }

    printf("[cellGcmSys] WARNING: AddressToOffset failed for 0x%08X\n", address);
    vm_write32(off_ea, 0);
    { static int _d = -1; if (_d < 0) _d = getenv("A2O_FAILDBG") ? 1 : 0;
      if (_d) { static int _n = 0; if (PS3_LOG_FIRST(_n, 12))
          fprintf(stderr, "[A2O] FAILED ea=0x%08X (not in local or IO space)%c",
                  address, 10); } }
    return CELL_GCM_ERROR_FAILURE;
}

/* IO-offset space allocator for main-memory mappings. It never hands out a
 * page that is mapped, the Init command-buffer region included: handing out io
 * 0 (the old s_local_mem_allocated bump, which begins at 0) remapped the FIFO's
 * own io page -- demosaic's cellGcmMapMainMemory(sys heap) got offset 0,
 * io2ea(0) then resolved into its heap (all zeros) and the RSX walker consumed
 * every frame's commands as no-ops (no draws, label fence never written, guest
 * waited forever). */
/* First fit over the IO -> EA table: the lowest run of 1MB IO pages that no
 * mapping holds. A bump pointer kept across cellGcmInit (Init wipes the tables
 * and claims io 0..ioSize itself) handed a later map IO pages inside the Init
 * window: a title that maps 2MB before Init left the pointer at 0x300000, and
 * its next map took io 0x300000 onwards, on top of the window. Command buffers
 * in that part of its IO heap then resolved into the other mapping, the FIFO
 * walker read zeros and resynced, and a frame's commands were lost. Searching
 * the table cannot hand out a page that is in use.
 * Returns 0xFFFFFFFF when no run is free. */
#define GCM_IO_PAGES 256u   /* the RSX's 256MB IO window (cellGcmGetMaxIoMapSize) */
static u32 gcm_io_alloc(u32 size)
{
    const u32 need = (size + 0xFFFFFu) >> 20;
    u32 run = 0;
    for (u32 p = 1; p < GCM_IO_PAGES && need; p++) {   /* io 0 is the FIFO's own page */
        run = (s_ea_address_table[p] == 0xFFFF) ? run + 1 : 0;
        if (run == need) return (p + 1 - need) << 20;
    }
    return 0xFFFFFFFFu;
}

/* NID: 0x2A6FBA9C */
s32 cellGcmMapMainMemory(u32 ea, u32 size, u32* offset)
{
    /* Log the REQUEST before validating it. Every rejection below used to
     * happen before the only printf in this function, so a title whose
     * mapping we refused looked -- in the log -- like a title that never asked.
     * Virtua Fighter 5 then fails cellGcmAddressToOffset ~976 times a boot for
     * addresses immediately past the region it did get, draws 20 times instead
     * of thousands, and never flips; the refusal that would explain it was
     * invisible. Same lesson as the savedata early returns. */
    printf("[cellGcmSys] MapMainMemory(ea=0x%08X, size=0x%X)\n", ea, size);

    if (!offset) {
        printf("[cellGcmSys] MapMainMemory REFUSED: offset out-param is NULL\n");
        return CELL_GCM_ERROR_INVALID_VALUE;
    }
    /* Size and EA must both be 1MB aligned (RPCS3: CELL_GCM_ERROR_FAILURE). */
    if (size == 0 || (size & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapMainMemory REFUSED: size 0x%X is not a non-zero multiple of 1MB\n", size);
        return gcm_hle_validate() ? CELL_GCM_ERROR_FAILURE : CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }
    if ((ea & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapMainMemory REFUSED: ea 0x%08X is not 1MB aligned\n", ea);
        return gcm_hle_validate() ? CELL_GCM_ERROR_FAILURE : CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }

    /* Before cellGcmInit there is no IO page table to map into (the reference
     * emulator returns NO_IO_PAGE_TABLE here too). Accepting the map only for
     * Init to wipe it left the title holding an offset that Init then gave to
     * its own window. */
    if (!s_gcm_initialized) {
        printf("[cellGcmSys] MapMainMemory REFUSED: cellGcmInit has not run (no IO page table)\n");
        return CELL_GCM_ERROR_NO_IO_PAGE_TABLE;
    }

    /* A free slot first, then the IO region: allocating the region before
     * the slot check leaked IO space on every refused call. */
    IoMapping* mapping = find_free_mapping();
    if (!mapping) {
        printf("[cellGcmSys] WARNING: no free IO mapping slots\n");
        return CELL_GCM_ERROR_FAILURE;
    }

    u32 io_offset = gcm_io_alloc(size);
    if (io_offset == 0xFFFFFFFFu) {
        printf("[cellGcmSys] MapMainMemory REFUSED: no free run of 0x%X bytes of IO space\n", size);
        return CELL_GCM_ERROR_NO_IO_PAGE_TABLE;
    }
    printf("[cellGcmSys] MapMainMemory -> io 0x%08X\n", io_offset);

    mapping->ea     = ea;
    mapping->io     = io_offset;
    mapping->size   = size;
    mapping->active = 1;
    s_io_mapping_count++;

    populate_offset_table(ea, io_offset, size);

    vm_write32((uint32_t)(uintptr_t)offset, io_offset);   /* guest out-param */
    return CELL_OK;
}

/* NID: 0x5A41C10F */
s32 cellGcmMapEaIoAddress(u32 ea, u32 io, u32 size)
{
    /* Logged before validation, and every failure path named -- same reason as
     * cellGcmMapMainMemory above. Two of these returned silently AFTER the log
     * line as well, so even a call that got that far could vanish. */
    printf("[cellGcmSys] MapEaIoAddress(ea=0x%08X, io=0x%08X, size=0x%X)\n", ea, io, size);

    if ((ea & 0xFFFFF) != 0 || (io & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: ea 0x%08X / io 0x%08X not 1MB aligned\n", ea, io);
        return gcm_hle_validate() ? CELL_GCM_ERROR_FAILURE : CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }
    if (size == 0 || (size & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: size 0x%X is not a non-zero multiple of 1MB\n", size);
        return gcm_hle_validate() ? CELL_GCM_ERROR_FAILURE : CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }
    if (find_mapping_by_ea(ea) != NULL) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: ea 0x%08X is already mapped\n", ea);
        return CELL_GCM_ERROR_ADDRESS_OVERWRAP;
    }

    IoMapping* mapping = find_free_mapping();
    if (!mapping) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: no free IO mapping slots\n");
        return CELL_GCM_ERROR_FAILURE;
    }

    mapping->ea     = ea;
    mapping->io     = io;
    mapping->size   = size;
    mapping->active = 1;
    s_io_mapping_count++;

    populate_offset_table(ea, io, size);

    return CELL_OK;
}

/* NID: 0x626E8518 */
s32 cellGcmMapEaIoAddressWithFlags(u32 ea, u32 io, u32 size, u32 flags)
{
    /* RPCS3 cellGcmSys.cpp accepts exactly STRICT_ORDERING (not an arbitrary
     * bit mask). The software FIFO already reads shared guest memory in FIFO
     * order after its acquire fence; there is no relaxed DMA mapping path to
     * select here. Preserve that path rather than pretending to enable a new
     * ordering mode by storing an unused flag. */
    if (flags != CELL_GCM_IOMAP_FLAG_STRICT_ORDERING)
        return CELL_GCM_ERROR_INVALID_VALUE;
    if ((ea | io | size) & 0xFFFFFu || !size)
        return gcm_hle_validate() ? CELL_GCM_ERROR_FAILURE : CELL_GCM_ERROR_INVALID_ALIGNMENT;
    const u64 ea_end = (u64)ea + size;
    const u64 io_end = (u64)io + size;
    /* The HLE models the standard 256 MiB RSX IO aperture. Widen sums before
     * checking bounds so wrapping inputs cannot publish aliases at page 0. */
    if (ea_end > 0x100000000ull || io_end > 0x10000000ull)
        return CELL_GCM_ERROR_INVALID_VALUE;
    /* The legacy mapper checks only an identical EA base. Preflight complete
     * intervals on both sides so a rejected flagged map changes neither the
     * mapping list nor either direction of the translation table. */
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        const IoMapping* m = &s_io_mappings[i];
        if (m->active &&
            (((u64)ea < (u64)m->ea + m->size && ea_end > m->ea) ||
             ((u64)io < (u64)m->io + m->size && io_end > m->io)))
            return CELL_GCM_ERROR_ADDRESS_OVERWRAP;
    }
    return cellGcmMapEaIoAddress(ea, io, size);
}

/* NID: 0xDB23E867 (disambiguation by context) */
s32 cellGcmUnmapEaIoAddress(u32 ea)
{
    /* RPCS3 cellGcmUnmapEaIoAddress: ea >> 20, the low 20 bits ignored. */
    IoMapping* mapping = find_mapping_by_ea(gcm_hle_validate() ? (ea & ~0xFFFFFu) : ea);
    if (!mapping)
        return CELL_GCM_ERROR_FAILURE;

    printf("[cellGcmSys] UnmapEaIoAddress(ea=0x%08X)\n", ea);

    clear_offset_table(mapping->ea, mapping->io, mapping->size);
    mapping->active = 0;
    s_io_mapping_count--;

    return CELL_OK;
}

/* NID: 0x3B9BD5BD */
s32 cellGcmUnmapIoAddress(u32 io)
{
    IoMapping* mapping = find_mapping_by_io(io);
    if (!mapping)
        return CELL_GCM_ERROR_FAILURE;

    printf("[cellGcmSys] UnmapIoAddress(io=0x%08X)\n", io);

    clear_offset_table(mapping->ea, mapping->io, mapping->size);
    mapping->active = 0;
    s_io_mapping_count--;

    return CELL_OK;
}

/* NID: 0xC47D0812 */
s32 cellGcmIoOffsetToAddress(u32 ioOffset, u32* ea)
{
    uint32_t ea_ea = (uint32_t)(uintptr_t)ea;   /* guest out-param */
    if (!ea_ea)
        return CELL_GCM_ERROR_INVALID_VALUE;

    u32 page = ioOffset >> 20;
    if (page < 65536 && s_ea_address_table[page] != 0xFFFF) {
        u32 ea_page = s_ea_address_table[page];
        vm_write32(ea_ea, (ea_page << 20) | (ioOffset & 0xFFFFF));
        return CELL_OK;
    }

    printf("[cellGcmSys] WARNING: IoOffsetToAddress failed for 0x%08X\n", ioOffset);
    vm_write32(ea_ea, 0);
    return CELL_GCM_ERROR_FAILURE;
}

/* Resolve an RSX FIFO offset (from a vertex-array / texture / surface method)
 * to a guest effective address the backend can read via vm_base + addr.
 *
 * An RSX offset refers to either IO-mapped *main* memory or *local* video
 * memory, distinguished on hardware by a per-object location bit that the
 * command stream no longer carries by the time the backend sees it. Resolve by
 * table: if the offset's 1MB page is IO-mapped, it's main memory; otherwise it
 * is local video memory (localAddress + offset). IO mappings are sparse (the
 * command/IO window is a few MB) while local objects sit high in the local
 * heap, so this disambiguates every real case — only offsets inside the small
 * IO window are treated as main. */
u32 cellGcmResolveOffset(u32 offset)
{
    u32 page = offset >> 20;
    /* A page the guest derived from a LOCAL EA resolves to VRAM even when the
     * IO table also covers that page number (overlapping offset spaces). */
    if (page < sizeof(s_local_offset_page) && s_local_offset_page[page])
        return s_config.localAddress + offset;
    if (page < 65536) {
        u16 ea_page = s_ea_address_table[page];
        if (ea_page != 0 && ea_page != 0xFFFF)
            return ((u32)ea_page << 20) | (offset & 0xFFFFF);
    }
    return s_config.localAddress + offset;
}

/* IO-table-first resolve: skip the local-page shortcut and ask the IO offset
 * table, falling back to VRAM only if the page is unmapped. cellGcmResolveOffset
 * prefers local for any page the guest ever derived from a local EA, which is
 * right for a title that really does keep the data in VRAM but wrong for one
 * that builds its textures in main memory on pages whose numbers happen to
 * collide. Returns 0 when the IO table has no mapping, so a caller can tell
 * "not IO-mapped" from "IO-mapped at 0". */
u32 cellGcmResolveIO(u32 offset)
{
    u32 page = offset >> 20;
    if (page < 65536) {
        u16 ea_page = s_ea_address_table[page];
        if (ea_page != 0 && ea_page != 0xFFFF)
            return ((u32)ea_page << 20) | (offset & 0xFFFFF);
    }
    return 0;
}

/* Location-aware resolve. RSX offsets are TWO overlapping number spaces --
 * LOCAL (VRAM, ea = localAddress + offset) and MAIN (IO-mapped, via the
 * offset table). cellGcmResolveOffset guesses table-first, which breaks when
 * a title's IO region is large enough to cover the same page numbers as its
 * local allocations (gcm/cube: ioSize 16MB, FP ucode at local 0xB90000 --
 * table hit returned the empty command-buffer EA instead of VRAM). Callers
 * that KNOW the location (texture format bits, SET_SHADER_PROGRAM bits) must
 * use this. `local` != 0 -> local memory.
 *
 * A MAIN offset asks the IO table first. cellGcmResolveOffset would send it
 * to VRAM whenever the guest once derived a local offset in the same 1 MB
 * page number, and a title with both a large IO map and a full VRAM has
 * many such pages: Twisted Metal's menu fragment programs at IO 0x08B96040
 * and 0x0F75E800 and a G-buffer texture at IO 0x02A1F380 were read out of
 * VRAM -- texture bytes that decode as a NOP program, so the menu's UI drew
 * transparent black. An offset the IO table does not map keeps the old
 * guess. */
u32 cellGcmResolveLocated(int local, u32 offset)
{
    if (local)
        return s_config.localAddress + offset;
    const u32 ea = cellGcmResolveIO(offset);
    return ea ? ea : cellGcmResolveOffset(offset);
}

/* ---------------------------------------------------------------------------
 * Label / report / timestamp
 * -----------------------------------------------------------------------*/

/* NID: 0xF80196C1 */
u32* cellGcmGetLabelAddress(u8 index)
{
    if (index >= CELL_GCM_MAX_LABEL_COUNT) {
        printf("[cellGcmSys] WARNING: GetLabelAddress index %u out of range\n", index);
        return NULL;
    }
    if (s_r2rsx) return (u32*)(uintptr_t)(R2RSX_LABEL_EA + (u32)index * 0x10u);
    { static int n = 0; if (n < 32 && ++n)   /* TEMP */
        fprintf(stderr, "[cellGcmSys] GetLabelAddress(0x%X) -> 0x%08X\n", index,
                GCM_LABEL_GUEST_BASE + (u32)index * GCM_LABEL_STRIDE); }
    return (u32*)(uintptr_t)(GCM_LABEL_GUEST_BASE + (u32)index * GCM_LABEL_STRIDE);
}

/* NID: 0x8572ADE4 */
CellGcmReportData* cellGcmGetReportDataAddress(u32 index)
{
    if (s_r2rsx) return (CellGcmReportData*)(uintptr_t)(R2RSX_REPORT_EA + (index & 2047u) * 16u);
    if (index >= CELL_GCM_MAX_REPORT_COUNT) {
        printf("[cellGcmSys] WARNING: GetReportDataAddress index %u out of range\n", index);
        return NULL;
    }
    /* Resistance port: a GUEST address (the title dereferences it); this
     * returned a host pointer, which the guest saw truncated to garbage. */
    { static int n = 0; if (n < 4 && ++n) printf("[cellGcmSys] GetReportDataAddress(%u) -> 0x%08X%c", index, GCM_REPORT_GUEST_BASE + index * 16u, 10); }
    return (CellGcmReportData*)(uintptr_t)(GCM_REPORT_GUEST_BASE + index * 16u);
}

/* NID: 0x97FC4B73 */
/* NID: 0x99D397AC. The value NV4097_GET_REPORT stored at this index (the
 * walker fills it). Unregistered, every call returned a faked 0. */
u32 cellGcmGetReport(u32 type, u32 index)
{
    if (s_r2rsx) {
        if (type < 1 || type > 5) return 0xFFFFFFFFu;
        return vm_read32(R2RSX_REPORT_EA + (index & 2047u) * 16u + 8u);
    }
    { static unsigned n = 0;   /* unsigned: the periodic line keeps counting, and wrapping is defined */
      if (n++ < 8 || (n % 2000) == 0) printf("[cellGcmSys] GetReport(type=%u, index=%u) -> %u%c", type, index,
                          index < CELL_GCM_MAX_REPORT_COUNT ? s_report_data[index].value : 0u, 10); }
    if (index >= CELL_GCM_MAX_REPORT_COUNT) return 0;
    gcm_zc_hle_read();
    return vm_read32(GCM_REPORT_GUEST_BASE + index * 16u + 8u);
}

u64 cellGcmGetTimeStamp(u32 index)
{
    if (s_r2rsx) {
        const u32 a = R2RSX_REPORT_EA + (index & 2047u) * 16u;
        return ((u64)vm_read32(a) << 32) | vm_read32(a + 4);
    }
    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;

    /*
     * On real hardware this reads the RSX timestamp for a given report index.
     * We return the host timestamp in nanoseconds as a reasonable approximation.
     */
    s_report_data[index].timestamp = get_timestamp_ns();
    return s_report_data[index].timestamp;
}

/* ---------------------------------------------------------------------------
 * Tile / Zcull configuration
 * -----------------------------------------------------------------------*/

/* RPCS3 mode: mirror a tile / zcull slot into rsx::thread (cellGcmSetTile etc.). */
static void gcm_r2rsx_tile(u8 i)
{
    rsx_draw_engine_set_tile(i, s_tiles[i].tile & 1u, s_tiles[i].offset, s_tiles[i].size,
                             s_tiles[i].pitch, s_tiles[i].format, s_tiles[i].bank, s_tiles[i].bound);
    if (!s_r2rsx) return;
    r2rsx_set_tile(i, s_tiles[i].tile & 1u, s_tiles[i].offset, s_tiles[i].size, s_tiles[i].pitch,
                   s_tiles[i].format, s_tiles[i].base, s_tiles[i].bank, s_tiles[i].bound);
}
static void gcm_r2rsx_zcull(u8 i)
{
    if (!s_r2rsx) return;
    r2rsx_set_zcull(i, s_zcull[i].offset, s_zcull[i].width, s_zcull[i].height, s_zcull[i].cullStart,
                    s_zcull[i].zFormat, s_zcull[i].aaFormat, s_zcull[i].zcullDir, s_zcull[i].zcullFormat,
                    s_zcull[i].sFunc, s_zcull[i].sRef, s_zcull[i].sMask, s_zcull[i].bound);
}

static int gcm_env_on(const char* name)
{
    const char* e = getenv(name);
    return !(e && e[0] == '0');
}

/* GCM_HLE_VALIDATE=0: the old unchecked SetTile / SetDisplayBuffer / map paths. */
static int gcm_hle_validate(void)
{
    static int v = -1;
    if (v < 0) v = gcm_env_on("GCM_HLE_VALIDATE");
    return v;
}

/* GcmTileInfo::pack (RPCS3 GCM.h) into the guest tiles array. */
static void gcm_tile_mirror(u8 i)
{
    if (!s_tiles_guest_ea || !gcm_hle_validate()) return;
    const CellGcmTileInfo* t = &s_tiles[i];
    const u32 loc = t->tile & 1u, size = t->size ? t->size : 0x10000u;
    const u32 ea = s_tiles_guest_ea + 16u * i;
    vm_write32(ea + 0, (loc + 1u) | (t->bank << 4) | ((t->offset / 0x10000u) << 16) | (loc << 31));
    vm_write32(ea + 4, ((t->offset + size - 1u) / 0x10000u) << 16 | (loc << 31));
    vm_write32(ea + 8, (t->pitch / 0x100u) << 8);
    vm_write32(ea + 12, t->base | ((t->base + ((size - 1u) / 0x10000u)) << 13) | (t->format << 26) | (1u << 30));
}

/* NID: 0x0B4B62D5 */
s32 cellGcmSetTile(u8 index, u8 location, u32 offset, u32 size,
                   u32 pitch, u8 comp, u16 base, u8 bank)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetTile(index=%u, loc=%u, offset=0x%X, size=0x%X, pitch=%u, comp=%u)\n",
           index, location, offset, size, pitch, comp);

    /* RPCS3 cellGcmSys.cpp:1376-1418. */
    if (gcm_hle_validate()) {
        if (base >= 2048 || bank >= 4) return CELL_GCM_ERROR_INVALID_VALUE;
        if ((offset & 0xFFFFu) || (size & 0xFFFFu) || (pitch & 0xFFu)) return CELL_GCM_ERROR_INVALID_ALIGNMENT;
        if (location >= 2 || (comp != 0 && (comp < 7 || comp > 12))) return CELL_GCM_ERROR_INVALID_ENUM;
    }

    s_tiles[index].offset = offset;
    s_tiles[index].size   = size;
    s_tiles[index].pitch  = pitch;
    s_tiles[index].format = comp;
    s_tiles[index].base   = base;
    s_tiles[index].bank   = bank;
    s_tiles[index].bound  = 0;

    /* Build tile register value (simplified) */
    s_tiles[index].tile  = (location << 0) | (pitch << 8);
    s_tiles[index].limit = offset + size - 1;
    /* RPCS3 cellGcmSetTile: tile.bound = (pitch > 0). */
    s_tiles[index].bound = pitch > 0;
    gcm_tile_mirror(index);
    gcm_r2rsx_tile(index);

    return CELL_OK;
}

/* GCM_ZCULL_BIND=0: the old walker-mode SetZcull (region stored unbound
 * unless r2rsx) and the one-argument BindZcull. */
static int gcm_zcull_bind_on(void)
{
    static int v = -1;
    if (v < 0) v = gcm_env_on("GCM_ZCULL_BIND");
    return v;
}

/* RPCS3 cellGcmSetZcull / cellGcmBindZcull: GcmZcullInfo::pack, then
 * sys_rsx_context_attribute(0x301) (sys_rsx.cpp:1081-1141) validates and
 * unpacks the region into rsx::thread::zculls[] with bound = (status1 != 0),
 * i.e. always bound. */
static s32 gcm_zcull_store(u8 index, u32 offset, u32 width, u32 height, u32 cullStart, u32 zFormat,
                           u32 aaFormat, u32 zcullDir, u32 zcullFormat, u32 sFunc, u32 sRef, u32 sMask)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT) return CELL_GCM_ERROR_INVALID_VALUE;
    const u32 region  = 1u | (zFormat << 4) | (aaFormat << 8);
    const u32 size    = ((width >> 6) << 22) | ((height >> 6) << 6);
    const u32 start   = cullStart & ~0xFFFu;
    const u32 status0 = (zcullDir << 1) | (zcullFormat << 2) | ((sFunc & 0xFu) << 12) | (sRef << 16) | (sMask << 24);
    const u32 status1 = 0x2000u | (0x20u << 16);
    const u32 w = (size >> 22) << 6, h = ((size & 0xFFFFu) >> 6) << 6, off = offset & 0x0FFFFFFFu;
    const u64 len = (u64)w * h;
    if (!len || (u64)start + len - 1u > 0xFFFFFFFFull || (u64)start + len - 1u >= (3ull << 20) ||
        off >= s_config.localSize) {
        printf("[cellGcmSys] zcull %u: cull range invalid (start=0x%X %ux%u offset=0x%X)%c", index, start, w, h, off, 10);
        return CELL_EINVAL;
    }
    CellGcmZcullInfo* z = &s_zcull[index];
    z->zFormat = (region >> 4) & 0xFu;
    z->aaFormat = (region >> 8) & 0xFu;
    z->width = w;
    z->height = h;
    z->cullStart = start;
    z->offset = off;
    z->zcullDir = (status0 >> 1) & 1u;
    z->zcullFormat = (status0 >> 2) & 0x3FFu;
    z->sFunc = (status0 >> 12) & 0xFu;
    z->sRef = (status0 >> 16) & 0xFFu;
    z->sMask = (status0 >> 24) & 0xFFu;
    z->bound = 1;
    s_zc.surf_dirty = 1;
    if (s_zculls_guest_ea) {
        const u32 ea = s_zculls_guest_ea + 24u * index;
        vm_write32(ea + 0, region);
        vm_write32(ea + 4, size);
        vm_write32(ea + 8, start);
        vm_write32(ea + 12, offset);
        vm_write32(ea + 16, status0);
        vm_write32(ea + 20, status1);
    }
    gcm_r2rsx_zcull(index);
    return CELL_OK;
}

/* NID: 0xD34A420D. Twelve arguments: the last four come off the guest stack
 * (hle_gcm_set_zcull). */
s32 cellGcmSetZcull(u8 index, u32 offset, u32 width, u32 height,
                    u32 cullStart, u32 zFormat, u32 aaFormat,
                    u32 zcullDir, u32 zcullFormat,
                    u32 sFunc, u32 sRef, u32 sMask)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetZcull(index=%u, offset=0x%X, %ux%u)\n",
           index, offset, width, height);

    if (gcm_zcull_bind_on())
        return gcm_zcull_store(index, offset, width, height, cullStart, zFormat, aaFormat,
                               zcullDir, zcullFormat, sFunc, sRef, sMask);

    s_zcull[index].offset      = offset;
    s_zcull[index].width       = width;
    s_zcull[index].height      = height;
    s_zcull[index].cullStart   = cullStart;
    s_zcull[index].zFormat     = zFormat;
    s_zcull[index].aaFormat    = aaFormat;
    s_zcull[index].zcullDir    = zcullDir;
    s_zcull[index].zcullFormat = zcullFormat;
    s_zcull[index].sFunc       = sFunc;
    s_zcull[index].sRef        = sRef;
    s_zcull[index].sMask       = sMask;
    s_zcull[index].bound       = 0;
    if (s_r2rsx) { s_zcull[index].bound = 1; gcm_r2rsx_zcull(index); }

    return CELL_OK;
}

/* NID: 0x2BB1F1E5 */
s32 cellGcmBindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] BindTile(index=%u)\n", index);
    s_tiles[index].bound = 1;
    gcm_r2rsx_tile(index);
    return CELL_OK;
}

/* NID: 0x9DC04436. RPCS3 cellGcmSys.cpp:328-366: the same twelve arguments
 * as SetZcull, with an error code. */
s32 cellGcmBindZcull(u8 index, u32 offset, u32 width, u32 height,
                     u32 cullStart, u32 zFormat, u32 aaFormat,
                     u32 zcullDir, u32 zcullFormat,
                     u32 sFunc, u32 sRef, u32 sMask)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] BindZcull(index=%u, offset=0x%X, %ux%u)\n", index, offset, width, height);
    if (gcm_zcull_bind_on())
        return gcm_zcull_store(index, offset, width, height, cullStart, zFormat, aaFormat,
                               zcullDir, zcullFormat, sFunc, sRef, sMask);
    s_zcull[index].bound = 1;
    s_zc.surf_dirty = 1;
    gcm_r2rsx_zcull(index);
    return CELL_OK;
}

/* NID: 0x75843042 */
s32 cellGcmUnbindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] UnbindTile(index=%u)\n", index);
    s_tiles[index].bound = 0;
    gcm_r2rsx_tile(index);
    return CELL_OK;
}

/* NID: 0xA093BE1C */
s32 cellGcmUnbindZcull(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] UnbindZcull(index=%u)\n", index);
    s_zcull[index].bound = 0;
    s_zc.surf_dirty = 1;
    gcm_r2rsx_zcull(index);
    return CELL_OK;
}

/* SetZcull / BindZcull take twelve arguments; the generic HLE adapter passes
 * r3..r10 only, so the ninth onwards (zcullFormat, sFunc, sRef, sMask) are
 * read from the guest parameter save area at sp+112. */
static u32 gcm_ctx_arg(ppu_context* ctx, u32 n)
{
    return n <= 8u ? (u32)ctx->gpr[2u + n] : vm_read32((u32)ctx->gpr[1] + 112u + 8u * (n - 9u) + 4u);
}
static void hle_gcm_set_zcull(ppu_context* ctx)
{
    ctx->gpr[3] = (u64)(s64)cellGcmSetZcull((u8)gcm_ctx_arg(ctx, 1), gcm_ctx_arg(ctx, 2), gcm_ctx_arg(ctx, 3),
        gcm_ctx_arg(ctx, 4), gcm_ctx_arg(ctx, 5), gcm_ctx_arg(ctx, 6), gcm_ctx_arg(ctx, 7), gcm_ctx_arg(ctx, 8),
        gcm_ctx_arg(ctx, 9), gcm_ctx_arg(ctx, 10), gcm_ctx_arg(ctx, 11), gcm_ctx_arg(ctx, 12));
}
static void hle_gcm_bind_zcull(ppu_context* ctx)
{
    ctx->gpr[3] = (u64)(s64)cellGcmBindZcull((u8)gcm_ctx_arg(ctx, 1), gcm_ctx_arg(ctx, 2), gcm_ctx_arg(ctx, 3),
        gcm_ctx_arg(ctx, 4), gcm_ctx_arg(ctx, 5), gcm_ctx_arg(ctx, 6), gcm_ctx_arg(ctx, 7), gcm_ctx_arg(ctx, 8),
        gcm_ctx_arg(ctx, 9), gcm_ctx_arg(ctx, 10), gcm_ctx_arg(ctx, 11), gcm_ctx_arg(ctx, 12));
}

/* ps3_hle_register_ctx lives in the PPU runtime; unit tests that link
 * cellGcmSys.c alone get the no-op fallback. */
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
void gcm_hle_register_ctx_none(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{
    (void)nid; (void)name; (void)fn;
}
#if defined(_MSC_VER)
#pragma comment(linker, "/alternatename:ps3_hle_register_ctx=gcm_hle_register_ctx_none")
#endif

static void gcm_register_ctx_handlers(void)
{
    static int done = 0;
    if (done || !gcm_zcull_bind_on()) return;
    done = 1;
    ps3_hle_register_ctx(0xD34A420Du, "cellGcmSetZcull", hle_gcm_set_zcull);
    ps3_hle_register_ctx(0x9DC04436u, "cellGcmBindZcull", hle_gcm_bind_zcull);
}

/* ---------------------------------------------------------------------------
 * Misc
 * -----------------------------------------------------------------------*/

/* NID: 0x107BF789
 * SDK prototype (cell/gcm.h): uint32_t cellGcmGetTiledPitchSize(const uint32_t size)
 * -- returns the pitch BY VALUE in r3 (no out-pointer!). The old signature here
 * took (size, u32* pitch-out) and returned a status code, so every caller got 0
 * / an error code as its "pitch" -- gcm/cube's `bne` on the result then skipped
 * its entire config+framebuffer-alloc block (GetConfiguration never ran, local
 * memory base stayed 0, AddressToOffset(0) auto-mapped a bogus IO alias, and the
 * FIFO teleported into it). Returns 0 only for size > max, like hardware. */
u32 cellGcmGetTiledPitchSize(u32 size)
{
    u32 r = 0;
    /* Smallest valid tiled pitch >= size (RSX supports only specific pitches). */
    if (size) {
        for (int i = 0; i < s_valid_pitch_count; i++) {
            if (s_valid_pitches[i] >= size) { r = s_valid_pitches[i]; break; }
        }
    }
    { static int _n=0; if (PS3_LOG_FIRST(_n, 4)) fprintf(stderr, "[cellGcmSys] GetTiledPitchSize(0x%X) -> 0x%X\n", size, r); }
    return r;
}

/* NID: 0xBC982946 */
void cellGcmSetDebugOutputLevel(u32 level)
{
    printf("[cellGcmSys] SetDebugOutputLevel(level=%u)\n", level);
    s_debug_level = level;
}

/* ---------------------------------------------------------------------------
 * Additional functions needed by Tokyo Jungle
 * -----------------------------------------------------------------------*/

/* Notify data area — similar to report data, 16 bytes per slot */

/* Notify data lives in the IO page 0xF1 (IO 0x0F100000 + index * 0x40), the
 * page NV4097_NOTIFY writes {timestamp, 0} into. This returned a HOST pointer,
 * so the guest wrote its "pending" flags through a truncated address and the
 * RSX had nothing to clear: Twisted Metal's SpuPostFX thread waits for that
 * clear, so post-processing never ran and the frame label it releases never
 * moved. NULL when the title has not mapped the page, as libgcm does. */
void* cellGcmGetNotifyDataAddress(u32 index)
{
    u32 ea = gcm_io2ea(0x0F100000u);
    if (index >= 256 || !ea) return NULL;
    return (void*)(uintptr_t)(ea + index * 0x40u);
}

/* Timestamp location — returns CELL_GCM_LOCATION_LOCAL or MAIN */
/* The Location suffix names an ARGUMENT, not an out-parameter: it is the
 * caller saying which memory the report lives in, CELL_GCM_LOCATION_LOCAL or
 * _MAIN, exactly as cellGcmGetReportDataLocation and
 * cellGcmGetReportDataAddressLocation take it three hundred lines below. This
 * one alone was declared u32* and treated the value as a guest address to
 * write the location INTO, which inverts the parameter and loses the return.
 *
 * A title passing CELL_GCM_LOCATION_MAIN therefore had a 1 taken for an
 * effective address, and vm_write32 stored four bytes at guest address 1.
 * Yakuza: Dead Souls does that during display setup and dies there.
 *
 * It returns the timestamp, like cellGcmGetTimeStamp, whose body this is with
 * a location the report table does not distinguish. */
u64 cellGcmGetTimeStampLocation(u32 index, u32 location)
{
    if (s_r2rsx) {
        const u32 a = location == 1u ? gcm_io2ea(0x0E000000u + index * 16u)
                                     : R2RSX_REPORT_EA + (index & 2047u) * 16u;
        return a ? ((u64)vm_read32(a) << 32) | vm_read32(a + 4) : 0;
    }
    (void)location;

    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;

    s_report_data[index].timestamp = get_timestamp_ns();
    return s_report_data[index].timestamp;
}

/* SetTileInfo — alternative to SetTile with same parameters */
s32 cellGcmSetTileInfo(u8 index, u8 location, u32 offset, u32 size,
                       u32 pitch, u8 comp, u16 base, u8 bank)
{
    /* Delegates to existing SetTile */
    return cellGcmSetTile(index, location, offset, size, pitch, comp, base, bank);
}

/* Default FIFO size — configures command buffer size before init */
static u32 s_default_fifo_size = 0x40000; /* 256KB default */

s32 cellGcmSetDefaultFifoSize(u32 size)
{
    printf("[cellGcmSys] SetDefaultFifoSize(0x%X)\n", size);
    s_default_fifo_size = size;
    return CELL_OK;
}

/* Internal flip commands — called directly by some games */
/* libgcm's internal exports pass the command context FIRST -- taking
 * bufferId as arg1 made the range check eat the context pointer, so the
 * flip counter never advanced (wave: presents free-ran 4x per frame,
 * layout flashing/zooming). */
/* NID 0x21397818 is gcmSetFlipCommand (PSL1GHT libgcm_sys exports.h) -- it
 * was misassigned to cellGcmGetLabelAddress (real NID 0xF80196C1), so every
 * import-based flip (LBP: cellGcmSetFlip -> stub 0x7482FC(ctx, id)) landed
 * in the label getter and no flip was EVER issued: the intro-movie pump ran,
 * drew, "flipped" into the wrong function, and the screen stayed black. */
/* NID: 0x21397818 */
s32 _cellGcmSetFlipCommand(void* ctx, u32 bufferId)
{
    if (s_r2rsx) {
        if (bufferId > 7) return CELL_GCM_ERROR_FAILURE;
        const u32 w[2] = { 0x0004FEACu, bufferId };
        gcm_r2rsx_emit((u32)(uintptr_t)ctx, w, 2);
        return (s32)bufferId;
    }
    if (gcm_flip_into_fifo((u32)(uintptr_t)ctx, bufferId))
        return gcm_flip_request(bufferId, 1);
    return cellGcmSetFlipCommand(bufferId);
}

/* NID: 0xD8F88E1A */
/* libgcm writes this into the context: NV406E SEMAPHORE_OFFSET and
 * SEMAPHORE_ACQUIRE for the label, then the flip, so the RSX flips once the
 * label holds the value. The import used to write the label itself, from the
 * PPU, and flip at call time: whoever was meant to write the label was
 * overruled, and the flip did not wait for anything. Twisted Metal flips this
 * way on its main context. Queue it the same way as _cellGcmSetFlipCommand,
 * and fall back to the immediate form when the context is not a mapped command
 * buffer or has no room. */
s32 _cellGcmSetFlipCommandWithWaitLabel(void* ctx, u32 bufferId,
                                        u32 labelIndex, u32 labelValue)
{
    const u32 c = (u32)(uintptr_t)ctx;
    if (s_r2rsx) {   /* RPCS3: queue the flip, the PPU writes the label */
        s32 r = _cellGcmSetFlipCommand(ctx, bufferId);
        if (labelIndex < CELL_GCM_MAX_LABEL_COUNT)
            vm_write32(R2RSX_LABEL_EA + labelIndex * 0x10u, labelValue);
        return r;
    }
    if (c && labelIndex < CELL_GCM_MAX_LABEL_COUNT &&
        bufferId < CELL_GCM_MAX_DISPLAY_BUFFER_NUM && s_display_buffer_set[bufferId]) {
        const u32 end = vm_read32(c + 4), cur = vm_read32(c + 8);
        if (cur + 16 + 8 + GCM_RECYCLE_SLACK <= end && gcm_ea2io(cur) != 0xFFFFFFFFu) {
            vm_write32(cur + 0,  0x00040064u);   /* SEMAPHORE_OFFSET  */
            vm_write32(cur + 4,  labelIndex * GCM_LABEL_STRIDE);
            vm_write32(cur + 8,  0x00040068u);   /* SEMAPHORE_ACQUIRE */
            vm_write32(cur + 12, labelValue);
            vm_write32(c + 8, cur + 16);
            if (gcm_flip_into_fifo(c, bufferId))
                return gcm_flip_request(bufferId, 1);
            vm_write32(c + 8, cur);              /* no room for the flip */
        }
    }
    return cellGcmSetFlipCommandWithWaitLabel(bufferId, labelIndex, labelValue);
}

/* ---------------------------------------------------------------------------
 * Additional functions (RPCS3 parity)
 * -----------------------------------------------------------------------*/

/* FIFO command buffer callback — called when put pointer wraps.
 * In recomp we don't have a real GPU consuming FIFO, so this is a no-op. */
s32 cellGcmCallback(void* context, u32 count)
{
    (void)context;
    (void)count;
    return CELL_OK;
}

/* Map RSX local memory — returns the base address and size of local VRAM */
s32 cellGcmMapLocalMemory(u32* address, u32* size)
{
    if (!address || !size)
        return CELL_GCM_ERROR_INVALID_VALUE;

    /* Guest out-params, like every other pointer parameter in this file. */
    vm_write32((uint32_t)(uintptr_t)address, s_config.localAddress);
    vm_write32((uint32_t)(uintptr_t)size,    s_config.localSize);

    printf("[cellGcmSys] MapLocalMemory(address=0x%08X, size=0x%X)\n",
           s_config.localAddress, s_config.localSize);
    return CELL_OK;
}

/* Shutdown RSX — clear all state */
void cellGcmTerminate(void)
{
    printf("[cellGcmSys] Terminate\n");

    s_gcm_initialized = 0;
    s_flip_mode   = CELL_GCM_DISPLAY_VSYNC;
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    s_debug_level = CELL_GCM_DEBUG_LEVEL0;
    s_vblank_count = 0;
    s_io_map_reserved = 0;
    s_default_fifo_mode = 0;
    GCM_USER_STORE(0);
    GCM_USER_PENDING_STORE(0);

    memset(s_display_buffers, 0, sizeof(s_display_buffers));
    memset(s_display_buffer_set, 0, sizeof(s_display_buffer_set));
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_control, 0, sizeof(s_control));
    memset(s_io_mappings, 0, sizeof(s_io_mappings));
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_zcull, 0, sizeof(s_zcull));
    memset(s_report_data, 0, sizeof(s_report_data));
    s_report_dma = gcm_report_dma_init();
    memset(s_labels, 0, sizeof(s_labels));
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));

    s_flip_handler     = NULL;
    s_vblank_handler   = NULL;
    s_user_handler     = NULL;
    s_second_v_handler = NULL;
    s_graphics_handler = NULL;
    s_queue_handler    = NULL;
    s_local_mem_allocated = 0;
    s_io_mapping_count = 0;
    /* Unmapped = 0xFFFF. The tables are static (zero-initialized), so without
     * this every unmapped EA page silently aliased io page 0 (and vice versa),
     * producing plausible-but-wrong offsets instead of a translation failure. */
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    s_current_display_buffer_id = 0;
    s_last_flip_time = 0;
}

/* Return remaining IO map space (256MB total IO space minus already mapped) */
u32 cellGcmGetMaxIoMapSize(void)
{
    /* RSX has a 256MB IO address window */
    u32 total_io = 256 * 1024 * 1024;
    u32 used = 0;

    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active)
            used += s_io_mappings[i].size;
    }

    return (used < total_io) ? (total_io - used) : 0;
}

/* Reserve IO map space (pre-allocation for future mappings) */
s32 cellGcmReserveIoMapSize(u32 size)
{
    u32 max_size = cellGcmGetMaxIoMapSize();
    if (size > max_size - s_io_map_reserved)
        return CELL_GCM_ERROR_FAILURE;

    s_io_map_reserved += size;
    return CELL_OK;
}

/* Unreserve IO map space */
s32 cellGcmUnreserveIoMapSize(u32 size)
{
    if (size > s_io_map_reserved)
        return CELL_GCM_ERROR_FAILURE;

    s_io_map_reserved -= size;
    return CELL_OK;
}

/* Return incrementing VBlank counter */
u32 cellGcmGetVBlankCount(void)
{
    if (s_r2rsx) return (u32)r2rsx_vblank_count();
    /* Approximate: increment each time queried (games use this for timing) */
    return s_vblank_count++;
}

/* RPCS3 cellGcmGetTileInfo / cellGcmGetZcullInfo take no argument and return
 * the guest arrays SetTile / SetZcull fill. */
CellGcmTileInfo* cellGcmGetTileInfo(u8 index)
{
    if (s_tiles_guest_ea && gcm_hle_validate()) return (CellGcmTileInfo*)(uintptr_t)s_tiles_guest_ea;
    if (index >= CELL_GCM_MAX_TILE_COUNT) {
        printf("[cellGcmSys] WARNING: GetTileInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_tiles[index];
}

CellGcmZcullInfo* cellGcmGetZcullInfo(u8 index)
{
    if (s_zculls_guest_ea && gcm_hle_validate()) return (CellGcmZcullInfo*)(uintptr_t)s_zculls_guest_ea;
    if (index >= CELL_GCM_MAX_ZCULL_COUNT) {
        printf("[cellGcmSys] WARNING: GetZcullInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_zcull[index];
}

/* Return display buffer info by index */
CellGcmDisplayInfo* cellGcmGetDisplayInfo(u32 index)
{
    if (index >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM) {
        printf("[cellGcmSys] WARNING: GetDisplayInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_display_buffers[index];
}

/* Set default FIFO mode (before init) */
void cellGcmInitDefaultFifoMode(s32 mode)
{
    printf("[cellGcmSys] InitDefaultFifoMode(mode=%d)\n", mode);
    s_default_fifo_mode = mode;
}

/* Set command buffer to defaults (reset put/get pointers) */
void cellGcmSetDefaultCommandBuffer(void)
{
    printf("[cellGcmSys] SetDefaultCommandBuffer\n");
    s_control.put = 0;
    s_control.get = 0;
    s_control.ref = 0;

    /* On hardware this re-points gCellGcmCurrentContext at the default context
     * cellGcmInit built. Zeroing a host-side struct is not that: a title that
     * calls it to go back to the big default buffer stays on whatever smaller
     * segment it had switched to.
     *
     * GCM_DEFAULT_CTX_REPOINT=1 does the real thing -- write our context's EA
     * into the guest variable cellGcmInit's ctx_out pointed at, and rewind that
     * context. Off by default because a title managing its own segment chain
     * may hold pointers into the old one; this exists to be measured, not
     * assumed.
     *
     * Measured once now, on Virtua Fighter 5, which is the case this was
     * written for. VF5's AMGL keeps its own command buffers (it repoints
     * gCellGcmCurrentContext at a 512 KB segment of its own inside the 4 MB it
     * maps) and calls this entry to recover when one fills. Getting a host-side
     * struct zeroed instead leaves it on the exhausted segment, so it allocates
     * another, fills that, and asks again:
     *
     *     [AMGL]:[ERROR] Command Buffer Overflow!   x91, 636 calls to here
     *
     * With the repoint: **0 overflows**, and the command packets reaching the
     * draw engine go from 12,756 to 15,097 over the same five minutes. Still
     * one title's evidence, so still opt-in -- but it is no longer unmeasured,
     * and a port whose title manages its own segments should try it. */
    if (getenv("GCM_DEFAULT_CTX_REPOINT") && s_gcm_ctx_out_ea && s_gcm_context_ea) {
        vm_write32(s_gcm_context_ea + 0x8, vm_read32(s_gcm_context_ea + 0x0));
        vm_write32(s_gcm_ctx_out_ea, s_gcm_context_ea);
        printf("[cellGcmSys] SetDefaultCommandBuffer: repointed "
               "gCellGcmCurrentContext@0x%08X -> 0x%08X\n",
               s_gcm_ctx_out_ea, s_gcm_context_ea);
    }
}

/* Debug dump — no-op in recomp */
void cellGcmDumpGraphicsError(void)
{
    printf("[cellGcmSys] DumpGraphicsError (no-op)\n");
}

/* Default command word size: 0x400 (1024) words */
u32 cellGcmGetDefaultCommandWordSize(void)
{
    return 0x400;
}

/* Default segment word size: 0x100 (256) words */
u32 cellGcmGetDefaultSegmentWordSize(void)
{
    return 0x100;
}

/* Immediate flip — perform flip right now */
s32 cellGcmSetFlipImmediate(u32 bufferId)
{
    printf("[cellGcmSys] SetFlipImmediate(bufferId=%u)\n", bufferId);
    return cellGcmSetFlipCommand(bufferId);
}

/* Set flip status directly */
void cellGcmSetFlipStatus(u32 status)
{
    if (s_r2rsx) { r2rsx_set_flip_status(status); return; }
    s_flip_status = status;
}

/* Store graphics handler callback */
void cellGcmSetGraphicsHandler(CellGcmGraphicsHandler handler)
{
    printf("[cellGcmSys] SetGraphicsHandler(%p)\n", (void*)(size_t)handler);
    s_graphics_handler = handler;
}

/* Store queue handler callback */
void cellGcmSetQueueHandler(CellGcmQueueHandler handler)
{
    printf("[cellGcmSys] SetQueueHandler(%p)\n", (void*)(size_t)handler);
    s_queue_handler = handler;
    if (s_r2rsx) r2rsx_set_queue_handler((u32)(size_t)handler);
}

/* Set second V frequency */
void cellGcmSetSecondVFrequency(u32 freq)
{
    printf("[cellGcmSys] SetSecondVFrequency(%u)\n", freq);
    s_second_v_frequency = freq;
}

/* Set VBlank frequency */
void cellGcmSetVBlankFrequency(u32 freq)
{
    printf("[cellGcmSys] SetVBlankFrequency(%u)\n", freq);
    s_vblank_frequency = freq;
}

/* Store user command value */
void cellGcmSetUserCommand(u32 cmd)
{
    GCM_USER_STORE(cmd);
}

/* Called when a FIFO consumer retires a user-interrupt method. Publishing
 * the cause and pending marker is atomic; guest code runs later in the pump.
 * Like the driver cause register, multiple pending commands coalesce. */
void cellGcmQueueUserCommand(u32 cmd)
{
    GCM_USER_STORE(cmd);
    GCM_USER_PENDING_STORE((1ULL << 32) | cmd);
}

/* Invalidate a tile region (unbind + clear) */
s32 cellGcmSetInvalidateTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetInvalidateTile(index=%u)\n", index);

    s_tiles[index].bound = 0;
    memset(&s_tiles[index], 0, sizeof(CellGcmTileInfo));
    rsx_draw_engine_set_tile(index, 0, 0, 0, 0, 0, 0, 0);
    return CELL_OK;
}

/* EA IO address remap — stub, sorting not needed in recomp */
void cellGcmSortRemapEaIoAddress(void)
{
    /* No-op: IO address remapping not required for recomp */
}

/* Report data with location parameter */
static u32 gcm_report_ea(u32 index, u32 location)
{
    /* RPCS3: MAIN reports live at IO offset 0x0E000000 + index*16; LOCAL ones
     * in the report area (guest memory). */
    if (location == 1u) return gcm_io2ea(0x0E000000u + index * 16u);
    if (s_r2rsx) return R2RSX_REPORT_EA + (index & 2047u) * 16u;
    return GCM_REPORT_GUEST_BASE + (index & 0xFFFFu) * 16u;
}
CellGcmReportData* cellGcmGetReportDataAddressLocation(u32 index, u32 location)
{
    if (index >= CELL_GCM_MAX_REPORT_COUNT) {
        printf("[cellGcmSys] WARNING: GetReportDataAddressLocation index %u out of range\n", index);
        return NULL;
    }
    { static int n = 0; if (n < 4 && ++n) printf("[cellGcmSys] GetReportDataAddressLocation(%u, %u) -> 0x%08X%c", index, location, gcm_report_ea(index, location), 10); }
    return (CellGcmReportData*)(uintptr_t)gcm_report_ea(index, location);
}

/* Get report value at index with location */
u32 cellGcmGetReportDataLocation(u32 index, u32 location)
{
    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;
    { static int n = 0; if (n < 4 && ++n) printf("[cellGcmSys] GetReportDataLocation(%u, %u)%c", index, location, 10); }
    const u32 ea = gcm_report_ea(index, location);
    gcm_zc_hle_read();
    return ea ? vm_read32(ea + 8u) : 0u;
}

/* ---------------------------------------------------------------------------
 * VRAM upload registry
 *
 * A title whose texture uploads land at an address that does not match the
 * offset it later programs into SET_TEXTURE_OFFSET leaves the sampler reading
 * untouched memory. Recording what was uploaded lets the texture path find the
 * real bytes by size instead of trusting the offset. Diagnostic bridge: it is
 * a lookup, not a fix for the offset bookkeeping itself.
 * -----------------------------------------------------------------------*/
#define RSX_UPLOAD_LOG 256
static struct { u32 dst, size; int claimed; } s_uploads[RSX_UPLOAD_LOG];
static u32 s_upload_n;

void rsx_note_vram_upload(u32 dst_ea, u32 size)
{
    if (!size) return;
    s_uploads[s_upload_n % RSX_UPLOAD_LOG].dst     = dst_ea;
    s_uploads[s_upload_n % RSX_UPLOAD_LOG].size    = size;
    s_uploads[s_upload_n % RSX_UPLOAD_LOG].claimed = 0;
    s_upload_n++;
}

/* OLDEST unclaimed upload of exactly `want` bytes, claimed on return. Matching
 * by size alone hands every same-sized surface the same image; consuming them in
 * order pairs the Nth bind of a size with the Nth upload of that size, which is
 * the order a title loads and then draws its materials in. Returns 0 if none. */
u32 rsx_find_vram_upload(u32 want)
{
    u32 n = s_upload_n < RSX_UPLOAD_LOG ? s_upload_n : RSX_UPLOAD_LOG;
    u32 first = s_upload_n > RSX_UPLOAD_LOG ? s_upload_n - RSX_UPLOAD_LOG : 0;
    for (u32 i = 0; i < n; i++) {
        u32 idx = (first + i) % RSX_UPLOAD_LOG;
        if (s_uploads[idx].size == want && !s_uploads[idx].claimed) {
            s_uploads[idx].claimed = 1;
            return s_uploads[idx].dst;
        }
    }
    return 0;
}

/* Let the same pairing repeat next frame. */
void rsx_reset_upload_claims(void)
{
    for (u32 i = 0; i < RSX_UPLOAD_LOG; i++) s_uploads[i].claimed = 0;
}

/* ---------------------------------------------------------------------------
 * sys_rsx_* bridge (libs/video/sys_rsx.c)
 *
 * A PS3 firmware module links libgcm statically and drives RSX through the lv2
 * syscalls instead of importing cellGcmSys, so none of the HLE entry points above
 * ever run. Everything the FIFO walker needs is still here -- it just has to be
 * reachable from the syscall layer. These four accessors are that surface; the
 * walker, the method decoder and rsx_live_draw are untouched.
 * -----------------------------------------------------------------------*/

/* Bring the GCM state up the way cellGcmInit would, minus the IO window (the
 * driver maps that itself with sys_rsx_context_iomap). Returns the guest base of
 * RSX local memory, which is what sys_rsx_memory_allocate hands back. */
u32 cellGcm_syscall_bringup(u32 local_size)
{
    if (!s_gcm_initialized)
        cellGcmInit(0x10000, 0, 0);
    if (local_size)
        s_config.localSize = local_size;
    s_gcm_syscall_mode = 1;
    return s_config.localAddress;
}

/* Guest EA of the put/get/ref triple. The driver gets this as
 * sys_rsx_context_allocate's lpar_dma_control + 0x40 (verified in the image:
 * `lwz r3,0x18(r9)` / `addi r3,r3,64`), so the syscall returns this minus 0x40
 * and the driver's own flushes land exactly where the walker reads. */
u32 cellGcm_control_guest_addr(void) { return GCM_CONTROL_GUEST_ADDR; }

/* sys_rsx_context_iomap / iounmap: the same 1MB-page offset table
 * cellGcmMapEaIoAddress fills, which is what gcm_io2ea() and every offset
 * resolution read. */
void cellGcm_syscall_iomap(u32 ea, u32 io, u32 size)
{
    populate_offset_table(ea, io, size);
    /* A free slot, not s_io_mappings[count]: an unmap leaves a hole, and the
     * count then indexes a live slot. */
    IoMapping* m = find_free_mapping();
    if (m) {
        m->ea = ea; m->io = io; m->size = size; m->active = 1;
        s_io_mapping_count++;
    }
    if (!s_config.ioSize) { s_config.ioAddress = ea; s_config.ioSize = size; }
}

void cellGcm_syscall_iounmap(u32 io, u32 size)
{
    u32 page = io >> 20;
    u32 ea = (page < 65536 && s_ea_address_table[page] != 0xFFFF)
             ? ((u32)s_ea_address_table[page] << 20) : 0;
    if (ea) clear_offset_table(ea, io, size);
}

/* sys_rsx_context_attribute(0x001): the driver's explicit FIFO reset. Move the
 * walker with it -- leaving s_fifo_getoff where it was makes the next drain walk
 * from a stale offset through whatever the driver has since overwritten. */
void cellGcm_syscall_set_fifo(u32 put, u32 get)
{
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, put);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, get);
    s_fifo_getoff  = get;
    s_fifo_calloff = GCM_CALL_NONE;
    s_fifo_restore = 0xFFFFFFFFu; s_fifo_saved_ret = GCM_CALL_NONE; s_fifo_sync_req = 1;
}
