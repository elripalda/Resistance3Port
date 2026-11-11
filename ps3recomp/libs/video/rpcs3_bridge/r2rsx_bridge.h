/* r2rsx_bridge: drives RPCS3's RSX core (third_party/rpcs3, GPLv2) from the
 * recompiled runtime. RPCS3 runs in its HLE mode (rsx::thread::isHLE): our
 * cellGcmSys HLE plays the part of RPCS3's cellGcmSys module, and the RSX thread
 * reads the title's FIFO, put/get/ref and memory straight out of guest RAM.
 *
 * C API so cellGcmSys.c and the host can call it. Every function is a no-op
 * when the bridge is not enabled. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default on; R2_RSX=d3d12 (or 0) selects the old D3D12 backend. Fixed at the first call. */
int  r2rsx_enabled(void);

/* The window RPCS3 renders into (host thread owning it pumps its messages). */
void r2rsx_set_window(void* hwnd, uint32_t width, uint32_t height);

/* Bring up the RSX thread and allocate the GCM context, as RPCS3's
 * _cellGcmInitBody + sys_rsx_context_allocate do. dma_control_ea is the
 * RsxDmaControl, i.e. the CellGcmControl the title polls minus 0x40.
 * label_ea is the RsxReports block (semaphores +0, notify +0x1000,
 * reports +0x1400; 0x9400 bytes). Blocks until the renderer is up. */
int  r2rsx_context_allocate(uint32_t dma_control_ea, uint32_t label_ea,
                            uint32_t driver_info_ea, uint32_t device_ea,
                            uint32_t local_size);

/* sys_rsx_context_iomap / iounmap: 1 MB granularity. */
void r2rsx_iomap(uint32_t ea, uint32_t io, uint32_t size);
void r2rsx_iounmap(uint32_t io, uint32_t size);

void r2rsx_set_display_buffer(uint32_t id, uint32_t offset, uint32_t pitch,
                              uint32_t width, uint32_t height);
void r2rsx_set_flip_mode(uint32_t mode);            /* CELL_GCM_DISPLAY_VSYNC = 1 */
uint32_t r2rsx_get_flip_status(void);               /* 0 = DONE */
void r2rsx_set_flip_status(uint32_t status);
uint64_t r2rsx_last_flip_time(void);
uint64_t r2rsx_vblank_count(void);
uint32_t r2rsx_current_display_buffer(void);

/* Guest OPDs, 0 to clear. Called on the bridge's interrupt thread, as RPCS3
 * calls them on _gcm_intr_thread. */
void r2rsx_set_flip_handler(uint32_t opd);
void r2rsx_set_vblank_handler(uint32_t opd);
void r2rsx_set_user_handler(uint32_t opd);
void r2rsx_set_queue_handler(uint32_t opd);

void r2rsx_set_tile(uint32_t index, uint32_t location, uint32_t offset, uint32_t size,
                    uint32_t pitch, uint32_t comp, uint32_t base, uint32_t bank, int bound);
void r2rsx_set_zcull(uint32_t index, uint32_t offset, uint32_t width, uint32_t height,
                     uint32_t cullStart, uint32_t zFormat, uint32_t aaFormat,
                     uint32_t zcullDir, uint32_t zcullFormat, uint32_t sFunc,
                     uint32_t sRef, uint32_t sMask, int bound);

/* Wait (bounded) until the RSX has consumed the FIFO up to `put`. */
void r2rsx_wait_get(uint32_t put, unsigned max_ms);
/* Wait (bounded) until the RSX's GET is outside the IO range [lo, hi). */
void r2rsx_wait_get_outside(uint32_t lo, uint32_t hi, unsigned max_ms);

/* OSD layers (r2rsx_osd.cpp): port notifications and menus drawn into the swapchain image by
 * RPCS3's overlay pass, so they show in exclusive fullscreen too (layered windows do not).
 * PORT_OSD=layered keeps the old windows; default swapchain. Each slot is one BGRA image
 * (straight alpha, pitch w*4, top-down) placed at x, y in a ref_w x ref_h space (the game
 * window's client size) that is scaled to the swapchain. bgra NULL keeps the last pixels and
 * only moves / fades the layer. While nothing flips (game paused) a layer change asks the RSX
 * for a native UI flip. The last slot is the bridge's own warm-up layer. */
#define R2RSX_OSD_SLOTS 16
int  r2rsx_osd_wanted(void);   /* PORT_OSD is swapchain and the RPCS3 renderer is in use */
int  r2rsx_osd_ready(void);    /* wanted, and the swapchain has presented at least once */
void r2rsx_osd_layer(int slot, const void* bgra, int w, int h, int x, int y, int ref_w, int ref_h, float alpha);
void r2rsx_osd_hide(int slot);
int  r2rsx_osd_visible(int slot);       /* slot < 0: any port layer */
void r2rsx_osd_suppress(int on);         /* hide every layer (screenshots), keep their state */
unsigned r2rsx_osd_presents(void);       /* presents that ran the OSD overlay */
void r2rsx_osd_refresh(void);            /* ask for a present if nothing flipped for ~16 ms */

#ifdef __cplusplus
}
#endif
