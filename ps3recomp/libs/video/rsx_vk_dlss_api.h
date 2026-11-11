/*
 * The ABI between ps3recomp's Vulkan temporal-upscaler selector
 * (rsx_vk_upscale.c) and a port's NVIDIA DLSS bridge DLL (for example
 * Resistance 2's project/r2_dlss.cpp -> r2_dlss.dll). The bridge is the only
 * binary that links the NGX SDK; the runtime loads it with LoadLibrary and asks
 * for the table below, so a build or machine without it still runs (FSR 3.1 or
 * FSR1 instead).
 *
 * The bridge exports, with C linkage:
 *     const port_dlss_vk_api* port_dlss_vk_get_api(unsigned version);
 * returning NULL when it cannot provide `version` (or newer, compatible).
 *
 * Conventions (the Vulkan backend's scene_aa inputs, rsx_vulkan_backend.h):
 *  - handles: dispatchable ones (VkInstance, VkPhysicalDevice, VkDevice,
 *    VkCommandBuffer) as void*, non-dispatchable ones (VkImage, VkImageView)
 *    as uint64_t, VkFormat as int;
 *  - inputs in SHADER_READ_ONLY_OPTIMAL, the output in GENERAL;
 *  - colour/depth/motion/mask are render size (the size `create` was given),
 *    the output is out_w x out_h (>= render size; equal = DLAA);
 *  - jitter: render pixels, +x right, +y down, the offset the scene was drawn
 *    with (already sign-adjusted by the caller);
 *  - motion vectors * mv_scale = render pixels, current -> previous
 *    (NGX's MVLowRes convention).
 * No Sony material; NVIDIA NGX types never cross this interface.
 */
#ifndef RSX_VK_DLSS_API_H
#define RSX_VK_DLSS_API_H

#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_DLSS_VK_API_VERSION 1u
#define PORT_DLSS_VK_GET_API     "port_dlss_vk_get_api"

typedef void (*port_dlss_vk_log_fn)(const char* msg);

typedef struct port_dlss_vk_image {
    uint64_t image;                  /* VkImage, 0 = none (mask only)        */
    uint64_t view;                   /* VkImageView                          */
    int      format;                 /* VkFormat                             */
} port_dlss_vk_image;

typedef struct port_dlss_vk_eval {
    port_dlss_vk_image color, depth, motion, mask, output;
    unsigned out_w, out_h;
    float    jitter_x, jitter_y;
    float    mv_scale_x, mv_scale_y;
    int      reset;
    float    frame_ms;
} port_dlss_vk_eval;

/* create flags */
#define PORT_DLSS_VK_DEPTH_INVERTED 1    /* GREATER depth test: near = 1     */
#define PORT_DLSS_VK_HDR            2    /* linear HDR colour, auto exposure */

/* optimal_render_size quality */
#define PORT_DLSS_VK_Q_PERFORMANCE        0
#define PORT_DLSS_VK_Q_BALANCED           1
#define PORT_DLSS_VK_Q_QUALITY            2
#define PORT_DLSS_VK_Q_ULTRA_PERFORMANCE  3
#define PORT_DLSS_VK_Q_DLAA               5

typedef struct port_dlss_vk_api {
    unsigned version;                /* PORT_DLSS_VK_API_VERSION             */
    unsigned size;                   /* sizeof(port_dlss_vk_api)             */
    /* Optional, before init: the NGX application identity (project id GUID
     * string and engine version). NULL keeps the bridge's defaults. */
    void (*set_project)(const char* project_id, const char* engine_version);
    /* The Vulkan extensions NGX needs (static storage). With instance and
     * physical_device NULL: NVSDK_NGX_VULKAN_RequiredExtensions, usable before
     * any Vulkan object exists. With them: that list merged with the
     * DLSS-specific GetFeature{Instance,Device}ExtensionRequirements. 1 = ok. */
    int  (*required_extensions)(void* instance, void* physical_device,
                                unsigned* inst_count, const char*** inst_exts,
                                unsigned* dev_count, const char*** dev_exts);
    /* After device creation. gipa/gdpa: PFN_vkGet{Instance,Device}ProcAddr.
     * dir: folder holding nvngx_dlss.dll (NGX also writes logs there). 1 =
     * NGX initialised and DLSS is available on this GPU/driver. */
    int  (*init)(void* instance, void* physical_device, void* device, void* gipa, void* gdpa,
                 const wchar_t* dir, port_dlss_vk_log_fn log);
    /* Record feature (re)creation into cmd for w x h -> ow x oh. flags:
     * PORT_DLSS_VK_*; preset: NVSDK_NGX_DLSS_Hint_Render_Preset (0 = NVIDIA's
     * default). 1 = ok. */
    int  (*create)(void* cmd, unsigned w, unsigned h, unsigned ow, unsigned oh, int flags,
                   unsigned preset);
    /* Record one evaluation into cmd. 1 = ok. */
    int  (*evaluate)(void* cmd, const port_dlss_vk_eval* e);
    /* NGX's recommended render size for an output and quality (after init);
     * 1 = ok. */
    int  (*optimal_render_size)(unsigned ow, unsigned oh, int quality, unsigned* w, unsigned* h);
    void (*release)(void);           /* the feature (the GPU must be idle)   */
    void (*shutdown)(void);          /* feature + NGX (the GPU must be idle) */
} port_dlss_vk_api;

typedef const port_dlss_vk_api* (*port_dlss_vk_get_api_fn)(unsigned version);

#ifdef __cplusplus
}
#endif

#endif /* RSX_VK_DLSS_API_H */
