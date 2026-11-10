/*
 * ps3recomp - Vulkan RSX Backend
 *
 * The Linux renderer the README calls "the obvious target". Same host-facing
 * entry points and test hooks as the Metal and headless null backends, so
 * runtime/host/host_posix.c drives it unchanged.
 *
 * Two draw paths, one registered at a time (the FIFO walker feeds both):
 *
 *   - the shared register-file draw engine (rsx_draw_engine.h), the path the
 *     Metal backend takes. The engine owns surfaces, render-to-texture, MRT,
 *     depth textures, vertex compaction and pipeline keys; this backend builds
 *     Vulkan pipelines from the guest's programs (HLSL -> SPIR-V through
 *     glslang, rsx_shader_spirv.h) and executes. Every ps3recomp_host scene
 *     passes on it. Default when guest programs are on.
 *   - the rsx_state vtable: the headless null backend's fixed-function
 *     contract (flat colour, depth test, texture unit 0, point sampling), so
 *     the two agree pixel for pixel. Default otherwise; with guest programs it
 *     also runs them, but has no surfaces (--rtt, --depthtex, --mrt, --mrt-a).
 *
 * Rendering is offscreen; a present reads the frame back (the host's pixel
 * checks), optionally dumps it, and optionally shows it in a window.
 *
 * Vulkan is loaded at run time (dlopen of libvulkan.so.1), not linked, so:
 *   - building needs only the Vulkan headers, not a target-arch libvulkan
 *     (which is what makes cross-compiling for aarch64 straightforward);
 *   - a machine with no Vulkan loader fails init() cleanly instead of failing
 *     to start at all.
 * Only Vulkan 1.0 core entry points are used, so it runs on 1.2-level drivers
 * such as NVIDIA's L4T driver for Tegra X1.
 *
 * Environment:
 *   PS3RECOMP_VK_DEVICE=<n>   pick physical device n (vkEnumeratePhysicalDevices
 *                             order) instead of the first non-CPU device.
 *   PS3RECOMP_VK_DUMP=<path>  write every presented frame to <path> as a
 *                             binary PPM (overwritten each present).
 *   PS3RECOMP_VK_WINDOW=1     also show each presented frame in an SDL2
 *                             window. Rendering and readback are unchanged, so
 *                             every test gives the same result with or without
 *                             it; if the window cannot be opened the run
 *                             continues headless and says why.
 *   PS3RECOMP_VK_FULLSCREEN=1 with a window: borderless full screen.
 *   PS3RECOMP_VK_HOLD=<sec>   with a window: keep the last frame on screen
 *                             this long before shutdown closes it.
 *   PS3RECOMP_VK_GUEST_PROGRAMS=1
 *                             run the guest's own vertex/fragment programs
 *                             (needs the translator: glslang at build time),
 *                             which also makes the draw engine the default.
 *   PS3RECOMP_RSX_ENGINE=dispatch|vtable
 *                             pick the path explicitly (the engine's own
 *                             switch, shared with the Metal backend).
 *   PS3RECOMP_VK_SYNC=1       engine path: submit and wait after every
 *                             operation (the old fully synchronous model),
 *                             to bisect frame-recording regressions.
 *   RSX_VSYNC=0|1|adaptive    present mode as RPCS3 swapchain_WSI: a number
 *                             > 0 is FIFO; "adaptive" MAILBOX, else
 *                             FIFO_RELAXED; otherwise (off, the default)
 *                             IMMEDIATE, else MAILBOX, else FIFO_RELAXED;
 *                             FIFO when none of them is available.
 *   RSX_DISPLAY=windowed|borderless|exclusive
 *                             the host window (rsx_null_backend.c) starts
 *                             as a popup over the monitor for borderless /
 *                             exclusive. With VK_EXT_full_screen_exclusive
 *                             the swapchain requests exclusive fullscreen
 *                             ALLOWED for "exclusive", DISALLOWED for any
 *                             other value; unset requests nothing (RPCS3
 *                             vk_exclusive_fs_mode). RSX_WINDOW=<w>x<h> sets
 *                             the windowed client size.
 *   RSX_FILTER=nearest|bilinear|fsr
 *                             output scaling of the engine path's present
 *                             (RPCS3 "Output Scaling Mode"); fsr = FSR1
 *                             EASU + RCAS when the frame is smaller than
 *                             the window, else bilinear. Default bilinear.
 *   PS3RECOMP_VK_OUTPUT_SCALING=...  the same, overriding RSX_FILTER.
 *   RSX_SHARPEN=<0..1>        RCAS strength (RPCS3 "FidelityFX CAS
 *                             Sharpening Intensity" / 100). Default 0.5.
 */

#ifndef PS3RECOMP_RSX_VULKAN_BACKEND_H
#define PS3RECOMP_RSX_VULKAN_BACKEND_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load Vulkan, create the device and the offscreen target, and register as
 * the RSX backend. `title` is only logged (no window at V0).
 * Returns 0 on success, -1 on any failure (reason printed to stderr). */
int  rsx_vulkan_backend_init(u32 width, u32 height, const char* title);

/* Windows: the host's own window (HWND) to present into. Call before init;
 * with one set the backend presents to it, without one it stays headless. */
void rsx_vulkan_backend_set_window(void* hwnd);

/* Unregister and destroy every Vulkan object, then unload the library. */
void rsx_vulkan_backend_shutdown(void);

/* Headless: always 0. Windowed: drains SDL's event queue; -1 once the
 * window has been closed or Esc pressed. */
int  rsx_vulkan_backend_pump_messages(void);

/* Read the frame back to host memory (and to PS3RECOMP_VK_DUMP if set), and
 * show it in the window if there is one. Same as _present_begin() followed
 * by _present_finish(). */
void rsx_vulkan_backend_present(void);

/* The same present in two halves, for a host that presents on a thread of its
 * own beside the FIFO walker.
 *
 * _present_begin() reads the flipped surface: it must not run while the
 * walker is draining (call it under the same lock as
 * cellGcm_rsx_process_fifo). On the draw engine path it only records and
 * submits the copy out of that surface, and returns at once.
 *
 * _present_finish() waits for that copy, reads the frame back, writes the
 * dump and presents to the window. It may run while the walker drains the
 * next frame: the next frame's commands are ordered after the copy, so they
 * never reach the presented image. On the fixed-function path, where the
 * walker draws into the presented image itself, _present_begin() does all of
 * it and _present_finish() has nothing left to do. */
void rsx_vulkan_backend_present_begin(void);
void rsx_vulkan_backend_present_finish(void);

/* Centre pixel of the last frame a present copied out, as 0xFFRRGGBB, or 0
 * before the first. Unlike _readback_center(), which on the draw engine path
 * reads the surface as it is now, this is what was shown. */
u32  rsx_vulkan_backend_presented_center(void);

/* The clear colour most recently set through the command stream, ARGB8888. */
u32  rsx_vulkan_backend_debug_color(void);

/* Centre pixel of the last presented frame as 0xFFRRGGBB, or 0 if nothing
 * has been presented yet. */
u32  rsx_vulkan_backend_readback_center(void);

/* Draws of the last presented frame that ran the guest's own programs. */
u32  rsx_vulkan_backend_guest_draws(void);

/* 1 when draws honour the depth bounds test: the draw engine drives the
 * backend and the device has the depthBounds feature (lavapipe in Mesa 25
 * does not). Valid after init. */
int  rsx_vulkan_backend_depth_bounds(void);

/* 1 when draws run the guest's own programs: the HLSL -> SPIR-V translator
 * was built (glslang found) and PS3RECOMP_VK_GUEST_PROGRAMS=1 opts in. Then
 * the shared register-file draw engine (rsx_draw_engine.h) is the default
 * path, as it is Metal's; PS3RECOMP_RSX_ENGINE=vtable selects the older
 * rsx_state path instead. Opt-in while the path grows; needs no initialised
 * backend. */
int  rsx_vulkan_backend_guest_programs(void);

/* ---- optional temporal upscaler (DLSS-class), engine path only ----------
 * The draw engine's scene_aa request (rsx_draw_engine_scene_aa) is executed
 * here: a compute pass turns the scene colour/depth into RGBA16F colour, R32F
 * depth, RG16F motion vectors (render pixels, current -> previous) and an R8
 * reactive mask; the registered upscaler evaluates them into an RGBA16F
 * output, which is merged back into the scene target. Dispatchable Vulkan
 * handles are passed as void*, non-dispatchable ones as u64. */
typedef struct rsx_vk_upscaler_image {
    u64 image;                       /* VkImage                         */
    u64 view;                        /* VkImageView                     */
    int format;                      /* VkFormat                        */
} rsx_vk_upscaler_image;

typedef struct rsx_vk_upscaler_ops {
    void* user;
    /* Before instance/device creation: extensions to enable (the supported
     * ones are). Optional. */
    int  (*required_extensions)(void* user, unsigned* inst_count, const char*** inst_exts,
                                unsigned* dev_count, const char*** dev_exts);
    /* After device creation; gipa/gdpa are PFN_vkGet{Instance,Device}ProcAddr.
     * 1 = usable; 0 disables the upscaler for the session. */
    int  (*init)(void* user, void* instance, void* physical_device, void* device,
                 void* gipa, void* gdpa);
    /* Record (re)creation for w x h -> ow x oh into cmd (VkCommandBuffer).
     * flags: 1 depth inverted, 2 the scene is FP16 (linear/HDR; otherwise
     * 8-bit display-encoded). 1 = ok. */
    int  (*create)(void* user, void* cmd, unsigned w, unsigned h, unsigned ow, unsigned oh,
                   int flags);
    /* Record one evaluation. Inputs are in SHADER_READ_ONLY_OPTIMAL, out in
     * GENERAL; mask.image == 0 means none. 1 = ok. */
    int  (*evaluate)(void* user, void* cmd, const rsx_vk_upscaler_image* color,
                     const rsx_vk_upscaler_image* depth, const rsx_vk_upscaler_image* mv,
                     const rsx_vk_upscaler_image* mask, const rsx_vk_upscaler_image* out,
                     unsigned out_w, unsigned out_h, float jitter_x, float jitter_y,
                     float mv_scale_x, float mv_scale_y, int reset, float frame_ms);
    void (*shutdown)(void* user);    /* optional */
    /* Optional, at device creation: enable the optional features the
     * upscaler uses. features/have are VkPhysicalDeviceFeatures (wanted /
     * supported); v12/have12 the chained VkPhysicalDeviceVulkan12Features
     * (NULL below Vulkan 1.2); exts the device extensions being enabled;
     * *pnext the VkDeviceCreateInfo chain, to prepend feature structs to
     * (static storage). gipa: PFN_vkGetInstanceProcAddr. */
    void (*device_features)(void* user, void* physical_device, void* gipa, void* instance,
                            void* features, const void* have, void* v12, const void* have12,
                            unsigned ext_count, const char* const* exts, const void** pnext);
} rsx_vk_upscaler_ops;

/* Register before rsx_vulkan_backend_init (the device is created with what
 * the upscaler needs: its extensions, Vulkan 1.2, bufferDeviceAddress,
 * shaderStorageImageExtendedFormats, sampled depth targets). NULL clears.
 * The ops struct must outlive the backend. */
void rsx_vulkan_backend_set_temporal_upscaler(const rsx_vk_upscaler_ops* ops);

/* 1 once a registered upscaler initialised on the device. */
int  rsx_vulkan_backend_temporal_upscaler_ready(void);

/* Validation (tests, debugging): with PS3RECOMP_VK_VALIDATION=1 set before
 * init the instance enables VK_LAYER_KHRONOS_validation when it is installed
 * (=sync adds synchronization validation) and prints every warning / error
 * it reports; this counts them. Survives shutdown; reset by the next init. */
u32  rsx_vulkan_backend_validation_messages(void);

/* The draw-engine backend vtable (const rsx_draw_backend*, rsx_draw_engine.h)
 * this backend registers, for tests that drive it directly. */
const void* rsx_vulkan_backend_engine_backend(void);

/* Switch output scaling to FSR1 (EASU + RCAS, RSX_SHARPEN), as
 * RSX_FILTER=fsr does: the fallback when a temporal upscaler is unavailable.
 * Safe before init. */
void rsx_vulkan_backend_force_output_fsr1(void);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_VULKAN_BACKEND_H */
