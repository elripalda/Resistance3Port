/*
 * AMD FidelityFX Super Resolution 3.1 (upscaler only) for the Vulkan
 * backend's temporal-upscaler slot (rsx_vulkan_backend_set_temporal_upscaler).
 *
 * The FidelityFX API runtime (amd_fidelityfx_vk.dll, FidelityFX SDK 1.1.x,
 * MIT) is loaded at run time with LoadLibrary and driven through
 * ffxCreateContext / ffxDispatch / ffxQuery / ffxDestroyContext, so a build
 * or a machine without it still runs. Built with the ffx-api headers when
 * PS3RECOMP_FFX_INCLUDE_DIR is set (CMake); otherwise every call reports
 * "unavailable". Windows only.
 *
 * Conventions (the backend's scene_aa inputs, see rsx_vulkan_backend.h):
 *  - jitter: FSR's jitterOffset, render pixels, +x right, +y down: the
 *    sub-pixel translation applied to the scene (FSR's documentation adds
 *    (2 jx / w, -2 jy / h) to y-up NDC; the draw engine adds the same to its
 *    clip -> NDC epilogue for rsx_be_scene_aa.jitter_px). Sequence and phase
 *    count come from ffxQuery (Halton(2,3), FSR's own phase count).
 *  - motion vectors: the backend's RG16F image, current -> previous in
 *    viewport units (y down), times mv_scale (viewport size in render
 *    pixels) = FSR's screen-space pixels, current -> previous; no
 *    display-resolution or jitter-cancellation flags.
 *  - depth: R32F, standard or inverted (create flag 1).
 *  - colour: 8-bit scenes are display-encoded (FSR non-linear sRGB); FP16
 *    scenes (create flag 2) are treated as HDR with auto exposure.
 *  - the backend's moving-object mask, when given, is FSR's reactive mask.
 * Frame generation is not used.
 */
#ifndef RSX_VK_FSR3_H
#define RSX_VK_FSR3_H

#include "rsx_vulkan_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*rsx_vk_fsr3_log_fn)(const char* msg);

/* Loads the FidelityFX runtime: dll_path, or amd_fidelityfx_vk.dll next to
 * the executable when NULL. 1 = loaded with its exports; otherwise the reason
 * is logged once and 0 returned. log may be NULL (stderr). */
int  rsx_vk_fsr3_load(const char* dll_path, rsx_vk_fsr3_log_fn log);

/* The upscaler ops for rsx_vulkan_backend_set_temporal_upscaler (valid after
 * a successful rsx_vk_fsr3_load; static storage). */
const rsx_vk_upscaler_ops* rsx_vk_fsr3_ops(void);

/* FSR's built-in RCAS sharpening, 0 (off) .. 1. */
void rsx_vk_fsr3_set_sharpness(float sharpness);

/* 1 while a context exists, with the render size it was created for. */
int  rsx_vk_fsr3_render_size(unsigned* w, unsigned* h);

/* FSR's jitter for frame index (ffxQuery GETJITTERPHASECOUNT /
 * GETJITTEROFFSET for the context's render -> output ratio; phases > 0
 * overrides the phase count). x, y: render pixels, +x right, +y down.
 * Returns the phase count used, 0 without a context. */
int  rsx_vk_fsr3_jitter(unsigned index, unsigned phases, float* x, float* y);

/* 1 once context creation failed (the backend then disables the slot). */
int  rsx_vk_fsr3_failed(void);

#ifdef __cplusplus
}
#endif

#endif /* RSX_VK_FSR3_H */
