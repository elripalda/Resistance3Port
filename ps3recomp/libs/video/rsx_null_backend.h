/*
 * ps3recomp - Null RSX Backend (Win32 window + color clear)
 *
 * Minimal graphics backend that:
 * - Creates a Win32 window
 * - Clears to the RSX clear color on each frame
 * - Presents via GDI (no GPU acceleration)
 *
 * This is the first step toward rendering — it proves the RSX command
 * processor is receiving commands and the game loop is running. Replace
 * with D3D12 or Vulkan backend for actual rendering.
 */

#ifndef PS3RECOMP_RSX_NULL_BACKEND_H
#define PS3RECOMP_RSX_NULL_BACKEND_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create and register the null backend.
 * Opens a window of the specified size and starts accepting RSX commands.
 * Returns 0 on success. */
int rsx_null_backend_init(u32 width, u32 height, const char* title);

/* Shut down the null backend and close the window. */
void rsx_null_backend_shutdown(void);

/* Process Win32 messages. Call this from the game's main loop
 * or from a timer/idle callback. Returns 0 normally, -1 if the
 * window was closed. Headless builds have no event queue and always
 * return 0. */
int rsx_null_backend_pump_messages(void);

#ifndef _WIN32
/* --- headless software build only ---------------------------------------
 * Off Windows this backend has no window: it renders into a host-memory
 * framebuffer, which makes it usable on a CI runner with no display and no
 * GPU. These mirror the Metal backend's test hooks so a host can assert what
 * actually came out of the guest command stream.
 * ---------------------------------------------------------------------- */

/* Copy the framebuffer's centre pixel out as the presented frame. */
void rsx_null_backend_present(void);

/* The clear colour most recently set via NV4097_SET_COLOR_CLEAR_VALUE, in the
 * RSX's native ARGB8888. Lets a host assert the command stream arrived. */
u32 rsx_null_backend_debug_color(void);

/* Centre pixel of the last presented frame as 0xFFRRGGBB, or 0 if nothing has
 * been presented yet. */
u32 rsx_null_backend_readback_center(void);

/* Draws of the last presented frame that ran the guest's own programs:
 * always 0 here, this backend has no shader path. Mirrors the Metal hook so
 * the host harness can name one entry point per backend. */
u32 rsx_null_backend_guest_draws(void);
#endif

/* The live draw engine binds to this window and takes over presentation. */
void* rsx_null_backend_get_hwnd(void);
void  rsx_null_backend_suppress_present(int on);

/* Start-up screen while the renderer compiles its shader cache before the
 * first frame (GDI on this window; the GPU swap chain has presented nothing
 * yet). step -1: dark screen, 1..steps: "Compiling shaders done / total" with
 * a progress bar and the percentage in the caption, 0: off (any thread).
 * Returns 1 when a window shows it. Closing the window while it is up ends
 * the process at once. */
int   rsx_null_backend_splash(int step, int steps, unsigned done, unsigned total);
/* 1 once the user pressed Esc / Space / Enter or clicked during the screen. */
int   rsx_null_backend_splash_skip(void);
/* 0: the renderer presents the start-up screen into its swap chain itself; this window keeps
 * the splash state, the skip keys and the caption but paints nothing (no GDI over the swap
 * chain). 1 (default): GDI paints the screen. */
void  rsx_null_backend_splash_gdi(int on);

#ifdef __cplusplus
}
#endif
#endif
