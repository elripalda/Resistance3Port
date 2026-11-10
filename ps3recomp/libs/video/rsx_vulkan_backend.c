/*
 * ps3recomp - Vulkan RSX Backend. See rsx_vulkan_backend.h.
 *
 * The file, in order:
 *   loader and device      libvulkan by dlopen, Vulkan 1.0 entry points only;
 *                          device choice, BC textures when the device has them
 *   memory, images, submit helpers
 *   vtable fallback path   the null backend's fixed-function contract:
 *                          position = attribute 0, flat colour = attribute 3
 *                          of each triangle's first vertex, texcoord0 =
 *                          attribute 8, fetched and expanded through the
 *                          shared rsx_fetch_attrib(); unit 0 decoded through
 *                          rsx_texture_decode(); depth test, no blend/stencil
 *   vtable guest programs  the same path running the decompilers' HLSL, with
 *                          whole mip chains and per-unit samplers
 *   optional window        SDL2 + swapchain; a present blits the frame into it
 *   draw engine backend    rsx_draw_backend: targets, clears, present and
 *                          readback (E1), pipelines and draws (E2), textures,
 *                          surface views and samplers (E3), depth snapshots
 *                          (E4) -- the stage names in the commit history
 *   public entry points    init picks the path, shutdown undoes everything
 *
 * Submission. The vtable fallback path is still synchronous: one one-shot
 * command buffer per operation, submitted and waited on. The draw engine path
 * records a whole frame instead (see "Frame recording"): VK_FRAMES slots,
 * each with its own command pool, fences, descriptor pools, upload ring and
 * deferred-deletion list; draws share a render pass until the targets change
 * or something has to run outside one; everything is submitted at present
 * (or when the CPU needs pixels back: readback, depth_readback), and a
 * present blits the flipped surface straight into the swapchain image.
 * PS3RECOMP_VK_SYNC=1 flushes and waits after every operation again, for
 * telling a synchronisation bug from a rendering one.
 *
 * Images stay in VK_IMAGE_LAYOUT_GENERAL for their whole life: legal for
 * transfers, sampling and as attachments, so there is no layout bookkeeping;
 * ordering inside a command buffer is global memory barriers (vk_rec_*).
 *
 * Shaders: libs/video/vulkan/rsx_vk_fallback.{vert,frag}, embedded as SPIR-V.
 * Regenerate after editing them with:
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_vert_spv \
 *       -o rsx_vk_fallback_vert.spv.h rsx_vk_fallback.vert
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_frag_spv \
 *       -o rsx_vk_fallback_frag.spv.h rsx_vk_fallback.frag
 * then `expand -t 4` both outputs: glslang indents with tabs, the project
 * with spaces.
 */

#define VK_NO_PROTOTYPES
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include "rsx_vulkan_backend.h"
#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include "rsx_vp_decompiler.h"
#include "rsx_fp_decompiler.h"
#include "rsx_vk_scale_math.h"
#include "rsx_vk_present_rules.h"
#include "rsx_shader_spirv.h"
#include "rsx_vk_pipestore.h"
#include "rsx_draw_engine.h"
#include "ps3emu/log_first.h"
#include "vulkan/rsx_vk_fallback_vert.spv.h"
#include "vulkan/rsx_vk_fallback_frag.spv.h"
#include "vulkan/rsx_vk_fsr1_easu.spv.h"
#include "vulkan/rsx_vk_fsr1_rcas.spv.h"
#include "vulkan/rsx_vk_aa_prep.spv.h"
#include "vulkan/rsx_vk_aa_merge8.spv.h"
#include "vulkan/rsx_vk_aa_merge16.spv.h"

#ifdef _WIN32
/* Windows: the host owns the window (rsx_vulkan_backend_set_window), the
 * loader is vulkan-1.dll, and the two mutexes are SRW locks. */
typedef SRWLOCK pthread_mutex_t;
#define pthread_mutex_init(m, a)  InitializeSRWLock(m)
#define pthread_mutex_destroy(m)  ((void)(m))
#define pthread_mutex_lock(m)     AcquireSRWLockExclusive(m)
#define pthread_mutex_unlock(m)   ReleaseSRWLockExclusive(m)
#define dlopen(name, flags)       ((void*)LoadLibraryA(name))
#define dlsym(lib, name)          ((void*)GetProcAddress((HMODULE)(lib), name))
#define dlclose(lib)              FreeLibrary((HMODULE)(lib))
#define dlerror()                 "LoadLibrary failed"
static HWND s_vk_hwnd;
#else
/* SDL2 is already required off Windows (cellPad, cellAudio); here it only
 * opens the optional window. Same include style as those two. */
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Run-time loaded entry points (Vulkan 1.0 core only)
 * -------------------------------------------------------------------------*/
#define VK_GLOBAL_FNS(X) X(vkCreateInstance)
#define VK_INSTANCE_FNS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#define VK_DEVICE_FNS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkQueueSubmit) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) \
    X(vkCmdSetStencilReference) X(vkCmdSetDepthBounds) X(vkCmdSetBlendConstants) \
    X(vkCmdSetDepthBias) X(vkCmdSetLineWidth) X(vkCmdSetStencilWriteMask) \
    X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCmdBlitImage) \
    X(vkResetDescriptorPool) X(vkResetCommandPool) X(vkCmdClearAttachments) \
    X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkGetPipelineCacheData) \
    X(vkCreateComputePipelines) X(vkCmdDispatch)
/* Window-system entry points, loaded only when a window was requested, so a
 * headless run never depends on a driver that can present. */
#define VK_WSI_INSTANCE_FNS(X) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VK_WSI_DEVICE_FNS(X) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

#define VK_DECLARE(name) static PFN_##name p##name;
static PFN_vkGetInstanceProcAddr pvkGetInstanceProcAddr;
VK_GLOBAL_FNS(VK_DECLARE)
VK_INSTANCE_FNS(VK_DECLARE)
VK_DEVICE_FNS(VK_DECLARE)
VK_WSI_INSTANCE_FNS(VK_DECLARE)
VK_WSI_DEVICE_FNS(VK_DECLARE)

/* ---------------------------------------------------------------------------
 * Backend state
 * -------------------------------------------------------------------------*/
typedef struct vk_vertex {
    float pos[4];
    float col[4];
    float uv[2];
} vk_vertex;

typedef struct vk_buffer {
    VkBuffer       buf;
    VkDeviceMemory mem;
    void*          ptr;
    VkDeviceSize   size;
    int            coherent;
} vk_buffer;

typedef struct vk_image {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
} vk_image;

#define VK_SAMPLER_CACHE 32
#define VK_PIPELINE_SLOTS (2 * 2 * 8)   /* ztest x zwrite x compare op */

typedef struct vk_state {
    void*            lib;
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    u32              queue_family;
    VkPhysicalDeviceMemoryProperties memprops;

    u32              width, height;
    vk_image         color;           /* R8G8B8A8_UNORM                 */
    vk_image         depth;           /* D32_SFLOAT or fallback         */
    VkFormat         depth_format;
    VkImageAspectFlags depth_aspect;  /* DEPTH, plus STENCIL if it has one  */

    VkRenderPass     render_pass;
    VkFramebuffer    framebuffer;
    VkShaderModule   vs, fs;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool desc_pool;
    VkDescriptorSet  desc_set;
    VkPipelineLayout pipe_layout;
    VkPipeline       pipelines[VK_PIPELINE_SLOTS];
    VkSampler        sampler;

    vk_image         dummy;           /* 1x1 transparent black: unbound   */
    vk_image         tex[RSX_MAX_TEXTURES];   /* decoded guest textures  */
    int              tex_ready[RSX_MAX_TEXTURES];
    /* Guest path only: each unit's sampler, built from its wrap, filter and
     * LOD registers (the fallback path keeps `sampler`, the null backend's
     * nearest + repeat). Cached by register value; VK_NULL_HANDLE = use
     * `sampler`. */
    VkSampler        tex_sampler[RSX_MAX_TEXTURES];
    struct { u64 key; VkSampler s; } samp_cache[VK_SAMPLER_CACHE];
    u32              samp_count;

    vk_buffer        vertices;        /* expanded triangle list          */
    vk_vertex*       cpu_verts;
    size_t           cpu_cap, cpu_count;

    vk_buffer        readback;
    VkCommandPool    pool;
    VkCommandBuffer  cmd;
    VkFence          fence;

    /* A present records and submits on its own pool, command buffer and
     * fences, so its second half can run on a host thread of its own while
     * the FIFO walker records the next frame (rsx_vulkan_backend.h,
     * present_begin/_finish). The queue is shared, and Vulkan wants every
     * vkQueueSubmit / vkQueuePresentKHR / vkDeviceWaitIdle on it externally
     * synchronised: queue_mu. present_mu keeps the present's own objects to
     * one present at a time, which matters when a flip in the command stream
     * presents on the walker's thread while a host thread finishes the last. */
    VkCommandPool    present_pool;
    VkCommandBuffer  present_cmd;
    VkFence          present_fence;   /* the copy out of the presented surface */
    VkFence          window_fence;    /* the blit into the swapchain image     */
    int              present_inflight;/* copy submitted, not yet finished      */
    int              present_split;   /* inside rsx_vulkan_backend_present_begin */
    int              present_order;   /* the walker's next command buffer waits */
    pthread_mutex_t  queue_mu;
    pthread_mutex_t  present_mu;

    /* Optional window (PS3RECOMP_VK_WINDOW=1). Rendering stays offscreen
     * either way -- readback and the tests are unchanged -- and a present
     * additionally blits the frame into the window's swapchain. */
    int              eng_active;      /* the register-file engine drives us */
    int              bc_ok;           /* textureCompressionBC enabled       */
    int              depth_bounds_ok; /* depthBounds enabled                */
    int              logic_op_ok;     /* logicOp enabled                    */
    int              depth_clamp_ok;  /* depthClamp enabled                 */
    int              wide_lines_ok;   /* wideLines enabled                  */
    int              fill_non_solid_ok; /* fillModeNonSolid enabled         */
    int              nv_d24_bias_half;/* RPCS3: NVIDIA D24 bias is 2x RSX's */
    float            line_w_min, line_w_max; /* limits.lineWidthRange   */
    int              mirror_clamp_ok; /* VK_KHR_sampler_mirror_clamp_to_edge */
    int              provoking_last_ok; /* VK_EXT_provoking_vertex, LAST mode */
    int              depth_unrestricted; /* VK_EXT_depth_range_unrestricted */
    int              custom_border_ok;  /* VK_EXT_custom_border_color enabled */
    uint32_t         inst_api;        /* the instance's apiVersion          */
    float            aniso_max;       /* samplerAnisotropy limit, or 0 */
    float            lod_bias_max;    /* maxSamplerLodBias */
    VkPipelineCache  pcache;          /* persisted to shadercache/ */
    u32              max_dim;         /* maxImageDimension2D of the device  */
    int              windowed;
#ifdef _WIN32
    void*            window;
#else
    SDL_Window*      window;
#endif
    const char*      inst_exts[16];
    u32              inst_ext_count;
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swapchain;
    VkExtent2D       sc_extent;
    u32              sc_count;
    VkImage          sc_images[8];
    VkSemaphore      sc_done[8];      /* per image: the present waits on it */
    VkSemaphore      sem_acquire;     /* the readback path's (vk_present_window) */
    int              sc_stale;        /* recreate before the next present      */
    /* RPCS3 swapchain_WSI: VK_KHR_get_surface_capabilities2 on the
     * instance and VK_EXT_full_screen_exclusive on the device (Win32). */
    int              surf_caps2_ok;
    int              fse_ok;
    double           hold_seconds;

    const rsx_state* state;           /* live RSX state, for draws       */
    u32              clear_argb;
    u32              last_center;
    u32              presented;
    const char*      dump_path;
    u32              dump_every;       /* write every nth presented frame */
} vk_state;

static vk_state s_vk;

/* RSX takes the LAST vertex of a primitive as the provoking vertex (GL
 * convention), Vulkan the first: RPCS3 enables VK_EXT_provoking_vertex's
 * LAST mode for flat shading (VKPipelineCompiler.cpp:169-176). Chained into
 * every graphics pipeline once enabled, so no render pass mixes modes
 * (provokingVertexModePerPipeline is not required). */
static const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT s_vk_pv_last = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
    .provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT };
static const void* vk_provoking_chain(void)
{
    return s_vk.provoking_last_ok ? &s_vk_pv_last : NULL;
}
/* Hardware MSAA device features (sampleRateShading, alphaToOne). */
static int s_ms_srs_ok, s_ms_a2one_ok;

#define VK_LOG(...)  fprintf(stderr, "[RSX vulkan] " __VA_ARGS__)
#define VK_CHECK(call, what)                                               \
    do { VkResult r_ = (call);                                             \
         if (r_ != VK_SUCCESS) {                                           \
             VK_LOG("%s failed (VkResult %d)\n", (what), (int)r_);         \
             return -1; } } while (0)

/* ---------------------------------------------------------------------------
 * Loading
 * -------------------------------------------------------------------------*/
static int vk_load_library(void)
{
#ifdef _WIN32
    static const char* names[] = { "vulkan-1.dll" };
#else
    static const char* names[] = { "libvulkan.so.1", "libvulkan.so" };
#endif
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !s_vk.lib; i++)
        s_vk.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!s_vk.lib) {
        VK_LOG("no Vulkan loader found (dlopen libvulkan.so.1: %s)\n", dlerror());
        return -1;
    }
    pvkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym(s_vk.lib, "vkGetInstanceProcAddr");
    if (!pvkGetInstanceProcAddr) { VK_LOG("libvulkan has no vkGetInstanceProcAddr\n"); return -1; }
#define LOAD_G(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(VK_NULL_HANDLE, #name))) \
        { VK_LOG("missing global function " #name "\n"); return -1; }
    VK_GLOBAL_FNS(LOAD_G)
    return 0;
}

static int vk_load_instance_functions(void)
{
#define LOAD_I(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(s_vk.instance, #name))) \
        { VK_LOG("missing instance function " #name "\n"); return -1; }
    VK_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_wsi_instance_functions(void)
{
    VK_WSI_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_device_functions(void)
{
#define LOAD_D(name) \
    if (!(p##name = (PFN_##name)pvkGetDeviceProcAddr(s_vk.device, #name))) \
        { VK_LOG("missing device function " #name "\n"); return -1; }
    VK_DEVICE_FNS(LOAD_D)
    return 0;
}

static int vk_load_wsi_device_functions(void)
{
    VK_WSI_DEVICE_FNS(LOAD_D)
    return 0;
}

/* ---------------------------------------------------------------------------
 * Device selection: PS3RECOMP_VK_DEVICE if set, else the first discrete GPU,
 * else integrated, else anything that is not a CPU rasteriser, else whatever
 * is there (llvmpipe/lavapipe on a GPU-less CI runner).
 * -------------------------------------------------------------------------*/
static int vk_device_rank(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 0;
    default:                                     return 1;
    }
}

static int vk_find_graphics_queue(VkPhysicalDevice pd, u32* family)
{
    u32 n = 0;
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    if (n == 0 || n > 64) return -1;
    VkQueueFamilyProperties props[64];
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, props);
    for (u32 i = 0; i < n; i++) {
        if (!(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (s_vk.windowed) {           /* the same queue must also present */
            VkBool32 ok = VK_FALSE;
            if (pvkGetPhysicalDeviceSurfaceSupportKHR(pd, i, s_vk.surface, &ok) != VK_SUCCESS || !ok)
                continue;
        }
        *family = i; return 0;
    }
    return -1;
}

static int vk_has_device_extension(VkPhysicalDevice pd, const char* name)
{
    u32 n = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS || n == 0) return 0;
    VkExtensionProperties* e = (VkExtensionProperties*)malloc(n * sizeof *e);
    if (!e) return 0;
    int found = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) == VK_SUCCESS)
        for (u32 i = 0; i < n && !found; i++) found = strcmp(e[i].extensionName, name) == 0;
    free(e);
    return found;
}

/* Before the instance exists: through the loader's global entry point. */
static int vk_has_instance_extension(const char* name)
{
    PFN_vkEnumerateInstanceExtensionProperties en = (PFN_vkEnumerateInstanceExtensionProperties)
        pvkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    u32 n = 0;
    if (!en || en(NULL, &n, NULL) != VK_SUCCESS || n == 0) return 0;
    VkExtensionProperties* e = (VkExtensionProperties*)malloc(n * sizeof *e);
    if (!e) return 0;
    int found = 0;
    if (en(NULL, &n, e) == VK_SUCCESS)
        for (u32 i = 0; i < n && !found; i++) found = strcmp(e[i].extensionName, name) == 0;
    free(e);
    return found;
}

static int vk_pick_device(void)
{
    u32 n = 0;
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, NULL), "vkEnumeratePhysicalDevices");
    if (n == 0) { VK_LOG("no Vulkan physical devices\n"); return -1; }
    if (n > 16) n = 16;
    VkPhysicalDevice devs[16];
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, devs), "vkEnumeratePhysicalDevices");

    int best = -1, best_rank = -1;
    const char* forced = getenv("PS3RECOMP_VK_DEVICE");
    for (u32 i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        pvkGetPhysicalDeviceProperties(devs[i], &p);
        u32 fam;
        int usable = vk_find_graphics_queue(devs[i], &fam) == 0 &&
                     (!s_vk.windowed ||
                      vk_has_device_extension(devs[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        VK_LOG("device %u: %s (type %d, api %u.%u.%u)%s\n", i, p.deviceName,
               (int)p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               usable ? "" : (s_vk.windowed ? " -- cannot present to the window"
                                            : " -- no graphics queue"));
        if (!usable) continue;
        if (forced) {
            if ((u32)atoi(forced) == i) { best = (int)i; break; }
            continue;
        }
        int rank = vk_device_rank(p.deviceType);
        if (rank > best_rank) { best = (int)i; best_rank = rank; }
    }
    if (best < 0) {
        if (forced) VK_LOG("PS3RECOMP_VK_DEVICE=%s is not a usable device\n", forced);
        else        VK_LOG("no device with a graphics queue\n");
        return -1;
    }
    s_vk.phys = devs[best];
    vk_find_graphics_queue(s_vk.phys, &s_vk.queue_family);
    pvkGetPhysicalDeviceMemoryProperties(s_vk.phys, &s_vk.memprops);
    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    /* Vulkan guarantees 4096; the engine's targets and textures may use what
     * the device really has (a title's 4608-wide target on RADV). */
    s_vk.max_dim = p.limits.maxImageDimension2D >= 4096u ? p.limits.maxImageDimension2D : 4096u;
    VK_LOG("using device %d: %s\n", best, p.deviceName);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Memory, images, buffers
 * -------------------------------------------------------------------------*/
static int vk_find_memory_type(u32 type_bits, VkMemoryPropertyFlags want, u32* out)
{
    for (u32 i = 0; i < s_vk.memprops.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) &&
            (s_vk.memprops.memoryTypes[i].propertyFlags & want) == want) {
            *out = i; return 0;
        }
    return -1;
}

static int vk_create_image_levels(vk_image* im, VkFormat fmt, u32 w, u32 h, u32 levels,
                                  VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    if (!levels) levels = 1;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = { w, h, 1 }, .mipLevels = levels, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(pvkCreateImage(s_vk.device, &ici, NULL, &im->img), "vkCreateImage");
    VkMemoryRequirements req;
    pvkGetImageMemoryRequirements(s_vk.device, im->img, &req);
    u32 type;
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) {
        VK_LOG("no memory type for an image\n"); return -1;
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &im->mem), "vkAllocateMemory(image)");
    VK_CHECK(pvkBindImageMemory(s_vk.device, im->img, im->mem, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { aspect, 0, levels, 0, 1 },
    };
    VK_CHECK(pvkCreateImageView(s_vk.device, &vci, NULL, &im->view), "vkCreateImageView");
    return 0;
}

static int vk_create_image(vk_image* im, VkFormat fmt, u32 w, u32 h,
                           VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    return vk_create_image_levels(im, fmt, w, h, 1, usage, aspect);
}

static void vk_present_wait_gpu(void);

static void vk_destroy_image(vk_image* im)
{
    /* Never under a present's copy that may still read it. */
    if (im->img) vk_present_wait_gpu();
    if (im->view) pvkDestroyImageView(s_vk.device, im->view, NULL);
    if (im->img)  pvkDestroyImage(s_vk.device, im->img, NULL);
    if (im->mem)  pvkFreeMemory(s_vk.device, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

static int vk_create_host_buffer(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK(pvkCreateBuffer(s_vk.device, &bci, NULL, &b->buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    pvkGetBufferMemoryRequirements(s_vk.device, b->buf, &req);
    u32 type;
    const VkMemoryPropertyFlags vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags coh = vis | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    /* A buffer the GPU only copies into is one the CPU reads back, and the
     * first coherent type is usually write-combined and uncached (RADV's
     * GTT): there the readback memcpy was 82% of the FIFO thread's time in
     * Twisted Metal. Take a cached type for it when there is one; reads
     * invalidate, so a non-coherent one is fine. */
    const VkMemoryPropertyFlags cached = vis | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const int readback = usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (readback && vk_find_memory_type(req.memoryTypeBits, cached | coh, &type) == 0) b->coherent = 1;
    else if (readback && vk_find_memory_type(req.memoryTypeBits, cached, &type) == 0) b->coherent = 0;
    else if (vk_find_memory_type(req.memoryTypeBits, coh, &type) == 0) b->coherent = 1;
    else if (vk_find_memory_type(req.memoryTypeBits, vis, &type) == 0) b->coherent = 0;
    else { VK_LOG("no host-visible memory for a buffer\n"); return -1; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &b->mem), "vkAllocateMemory(buffer)");
    VK_CHECK(pvkBindBufferMemory(s_vk.device, b->buf, b->mem, 0), "vkBindBufferMemory");
    VK_CHECK(pvkMapMemory(s_vk.device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->ptr), "vkMapMemory");
    b->size = size;
    return 0;
}

static void vk_destroy_buffer(vk_buffer* b)
{
    if (b->ptr) pvkUnmapMemory(s_vk.device, b->mem);
    if (b->buf) pvkDestroyBuffer(s_vk.device, b->buf, NULL);
    if (b->mem) pvkFreeMemory(s_vk.device, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

/* Host writes to non-coherent memory need a flush; there is no
 * vkFlushMappedMemoryRanges loaded, so writes only ever go to coherent
 * buffers (every desktop/L4T driver exposes one). Reads invalidate. */
static void vk_invalidate(vk_buffer* b)
{
    if (b->coherent) return;
    VkMappedMemoryRange r = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                              .memory = b->mem, .offset = 0, .size = VK_WHOLE_SIZE };
    pvkInvalidateMappedMemoryRanges(s_vk.device, 1, &r);
}

/* ---------------------------------------------------------------------------
 * One-shot command submission
 *
 * The queue is shared with the present (see present_mu in vk_state), so every
 * operation on it goes through these.
 * -------------------------------------------------------------------------*/
static VkResult vk_queue_submit(const VkSubmitInfo* si, VkFence fence)
{
    pthread_mutex_lock(&s_vk.queue_mu);
    const VkResult r = pvkQueueSubmit(s_vk.queue, 1, si, fence);
    pthread_mutex_unlock(&s_vk.queue_mu);
    return r;
}

static VkResult vk_queue_present(const VkPresentInfoKHR* pi)
{
    pthread_mutex_lock(&s_vk.queue_mu);
    const VkResult r = pvkQueuePresentKHR(s_vk.queue, pi);
    pthread_mutex_unlock(&s_vk.queue_mu);
    return r;
}

static void vk_device_idle(void)
{
    if (!s_vk.device || !pvkDeviceWaitIdle) return;
    pthread_mutex_lock(&s_vk.queue_mu);
    pvkDeviceWaitIdle(s_vk.device);
    pthread_mutex_unlock(&s_vk.queue_mu);
}

/* A full execution and memory dependency on everything submitted before
 * this command buffer, the present's copy included. */
static void vk_cmd_after_everything(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* ---------------------------------------------------------------------------
 * Frame recording (the draw engine path)
 *
 * A frame is recorded into long-lived primary command buffers and submitted
 * at present, or earlier only when the CPU needs GPU results (readback). Each
 * of the VK_FRAMES slots owns a command pool, one fence per submission, a
 * chain of descriptor pools, a ring of host-coherent buffers for everything
 * the CPU hands over (vertices, indices, constants, upload staging) and a
 * list of objects released while the GPU may still use them. A present moves
 * to the next slot and first waits for that slot's fences: then its pools are
 * reset, its ring rewinds and its deferred objects are destroyed. Slots retire
 * in submission order, so a deferred object outlives every use of it.
 *
 * Two command buffers are recorded at once:
 *   main  everything, in the engine's call order;
 *   pre   only the first writes to images no command in `main` has touched
 *         yet (a new texture's layout move and level uploads, a new target's
 *         initial clear). It is submitted in front of `main`, so creating and
 *         filling a texture between two draws does not end their render pass.
 * An object's `fresh` flag says which one it still goes to.
 *
 * Ordering inside a command buffer is global memory barriers, all stages to
 * all stages (images stay in GENERAL): one before every transfer and render
 * pass that follows other work, skipped between uploads into distinct images.
 * Every command buffer starts with one ordering it after everything submitted
 * before it, so submissions need no semaphores between them.
 * -------------------------------------------------------------------------*/
#define VK_FRAMES        3
#define VK_FR_MAX_SUB    256                      /* submissions per slot   */
/* The submissions a slot takes before vk_flush drains every slot. Each one
 * is a full CPU/GPU stall, so the perf pass raised it from 64 (still the
 * value with RSX_PERF_VK_MAXSUB=0 or RSX_PERF_ALL=0). */
static u32 vk_fr_sub_limit(void)
{
    static int lim = -1;
    if (lim < 0) {
        const char* a = getenv("RSX_PERF_ALL");
        const char* e = getenv("RSX_PERF_VK_MAXSUB");
        lim = ((a && *a == '0') || (e && *e == '0')) ? 64 : VK_FR_MAX_SUB;
    }
    return (u32)lim;
}
/* Perf-pass switches: RSX_PERF_<name>=0 (or RSX_PERF_ALL=0) turns one off. */
static int vk_perf_on(const char* name, int* cache)
{
    if (*cache < 0) {
        const char* a = getenv("RSX_PERF_ALL");
        const char* e = getenv(name);
        *cache = !((a && *a == '0') || (e && *e == '0'));
    }
    return *cache;
}
#define VK_FR_MAX_CB     (2 * VK_FR_MAX_SUB + 8)
#define VK_FR_MAX_CHUNK  64
#define VK_FR_CHUNK      ((VkDeviceSize)16u * 1024u * 1024u)
#define VK_FR_SOFT_BYTES ((VkDeviceSize)128u * 1024u * 1024u)  /* ring use that starts a new slot */
#define VK_FR_SOFT_DRAWS 50000u
#define VK_FR_MAX_DPOOL  512
#define VK_FR_DPOOL_SETS 256u
#define VK_BATCH_MAX     64
#define VK_DCACHE        256

enum { VK_TR_IMAGE = 1, VK_TR_BUFFER, VK_TR_FB, VK_TR_PIPE, VK_TR_SHADER };

typedef struct vk_trash {
    int            kind;
    vk_image       im;
    vk_buffer      buf;
    VkFramebuffer  fb;
    VkPipeline     pipe;
    VkShaderModule mod;
} vk_trash;

typedef struct vk_frame {
    VkCommandPool    pool;
    VkCommandBuffer  cb[VK_FR_MAX_CB];
    u32              ncb, cb_used;
    VkFence          fence[VK_FR_MAX_SUB];
    u32              nfence, nsub;
    vk_buffer        chunk[VK_FR_MAX_CHUNK];
    u32              nchunk, cur_chunk;
    VkDeviceSize     off, used;
    VkDescriptorPool dpool[VK_FR_MAX_DPOOL];
    u32              ndpool, cur_dpool, dpool_sets;
    vk_trash*        trash;
    u32              ntrash, trash_cap;
    u32              draws;
    VkSemaphore      acquire;
} vk_frame;

/* One command buffer being recorded and what its next command must wait for. */
typedef struct vk_rec {
    VkCommandBuffer cb;
    int             open;
    int             other;                 /* work since the last barrier    */
    u32             nbatch;
    VkImage         batch[VK_BATCH_MAX];   /* images uploaded into since then */
} vk_rec;

static struct {
    int          on;
    int          sync;                    /* PS3RECOMP_VK_SYNC=1            */
    u32          cur;
    u32          epoch;                   /* bumps when caches go stale     */
    vk_frame     fr[VK_FRAMES];
    vk_rec       pre, main;
    VkDeviceSize ubo_align;
    /* The render pass open on `main`. */
    int          pass;
    VkRenderPass pass_rp;
    VkFramebuffer pass_fb;
    u32          pass_w, pass_h;
    u32          pass_rt[RSX_BE_MAX_COLOR_TARGETS], pass_nrt;
    VkImage      pass_img[RSX_BE_MAX_COLOR_TARGETS];
    VkImage      pass_depth;
    /* What is bound on `main`, so unchanged state is not re-recorded. */
    int          b_valid;
    VkPipeline   b_pipe;
    VkDescriptorSet b_set;
    u32          b_doff[2];
    VkBuffer     b_vbuf, b_ibuf;
    VkDeviceSize b_voff, b_ioff;
    VkViewport   b_vp;
    VkRect2D     b_sc;
    u32          b_sref, b_bref;
    float        b_dbias[3];          /* fill, bias, scale */
    float        b_lw;
    float        b_db[2];
    float        b_bc[4];
    u32          b_split;
    vk_buffer    rb;                      /* readback, grown on demand      */
} s_f;

static void vk_destroy_trash(vk_trash* t)
{
    switch (t->kind) {
    case VK_TR_IMAGE:
        if (t->im.view) pvkDestroyImageView(s_vk.device, t->im.view, NULL);
        if (t->im.img)  pvkDestroyImage(s_vk.device, t->im.img, NULL);
        if (t->im.mem)  pvkFreeMemory(s_vk.device, t->im.mem, NULL);
        break;
    case VK_TR_BUFFER: vk_destroy_buffer(&t->buf); break;
    case VK_TR_FB:     if (t->fb) pvkDestroyFramebuffer(s_vk.device, t->fb, NULL); break;
    case VK_TR_PIPE:   if (t->pipe) pvkDestroyPipeline(s_vk.device, t->pipe, NULL); break;
    case VK_TR_SHADER: if (t->mod) pvkDestroyShaderModule(s_vk.device, t->mod, NULL); break;
    default: break;
    }
}

/* Destroy once every command recorded so far has finished. */
static void vk_defer(const vk_trash* t)
{
    vk_trash copy = *t;
    if (!s_f.on) { vk_destroy_trash(&copy); s_f.epoch++; return; }
    vk_frame* f = &s_f.fr[s_f.cur];
    if (f->ntrash == f->trash_cap) {
        const u32 cap = f->trash_cap ? f->trash_cap * 2u : 256u;
        vk_trash* n = (vk_trash*)realloc(f->trash, (size_t)cap * sizeof *n);
        if (!n) {                          /* out of memory: leak it rather than race the GPU */
            VK_LOG("deferred-deletion list: out of memory, leaking an object\n");
            return;
        }
        f->trash = n;
        f->trash_cap = cap;
    }
    f->trash[f->ntrash++] = copy;
}

static void vk_defer_image(vk_image* im)
{
    if (im->img || im->view || im->mem) {
        vk_trash t = { .kind = VK_TR_IMAGE, .im = *im };
        vk_defer(&t);
    }
    memset(im, 0, sizeof *im);
}

static void vk_defer_fb(VkFramebuffer fb)
{
    if (!fb) return;
    vk_trash t = { .kind = VK_TR_FB, .fb = fb };
    vk_defer(&t);
}

static void vk_defer_pipe(VkPipeline p)
{
    if (!p) return;
    vk_trash t = { .kind = VK_TR_PIPE, .pipe = p };
    vk_defer(&t);
}

static void vk_defer_shader(VkShaderModule m)
{
    if (!m) return;
    vk_trash t = { .kind = VK_TR_SHADER, .mod = m };
    vk_defer(&t);
}

/* The slot's command buffers, fences, descriptor pools, ring and deferred
 * objects are free again: every submission from it has finished. */
static void vk_fr_retire(vk_frame* f)
{
    for (u32 i = 0; i < f->ntrash; i++) vk_destroy_trash(&f->trash[i]);
    f->ntrash = 0;
    for (u32 i = 0; i < f->ndpool; i++) pvkResetDescriptorPool(s_vk.device, f->dpool[i], 0);
    f->cur_dpool = 0;
    f->dpool_sets = 0;
    if (f->pool) pvkResetCommandPool(s_vk.device, f->pool, 0);
    f->cb_used = 0;
    f->cur_chunk = 0;
    f->off = 0;
    f->used = 0;
    f->draws = 0;
    f->nsub = 0;
    if (f == &s_f.fr[s_f.cur]) s_f.epoch++;
}

static void vk_fr_wait(vk_frame* f)
{
    if (f->nsub)
        pvkWaitForFences(s_vk.device, f->nsub, f->fence, VK_TRUE, UINT64_MAX);
    vk_fr_retire(f);
}

/* Nothing may be recording. */
static void vk_fr_wait_all(void)
{
    for (u32 k = 1; k <= VK_FRAMES; k++) vk_fr_wait(&s_f.fr[(s_f.cur + k) % VK_FRAMES]);
}

static VkCommandBuffer vk_fr_cb(vk_frame* f)
{
    if (f->cb_used < f->ncb) return f->cb[f->cb_used++];
    if (f->ncb >= VK_FR_MAX_CB) return VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = f->pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    if (pvkAllocateCommandBuffers(s_vk.device, &cai, &f->cb[f->ncb]) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    f->ncb++;
    return f->cb[f->cb_used++];
}

/* The fence for the slot's next submission, unsignalled. */
static VkFence vk_fr_fence(vk_frame* f)
{
    if (f->nsub >= VK_FR_MAX_SUB) return VK_NULL_HANDLE;
    if (f->nsub >= f->nfence) {
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (pvkCreateFence(s_vk.device, &fci, NULL, &f->fence[f->nfence]) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        f->nfence++;
    }
    pvkResetFences(s_vk.device, 1, &f->fence[f->nsub]);
    return f->fence[f->nsub];
}

/* `size` bytes of the slot's ring, `align`ed: the CPU writes them now, the
 * GPU reads them before the slot retires. Never flushes. */
static void* vk_ring(VkDeviceSize size, VkDeviceSize align, VkBuffer* buf, VkDeviceSize* off)
{
    vk_frame* f = &s_f.fr[s_f.cur];
    if (!align) align = 16;
    if (!size) size = 4;
    for (;;) {
        if (f->cur_chunk < f->nchunk) {
            const vk_buffer* c = &f->chunk[f->cur_chunk];
            const VkDeviceSize o = (f->off + align - 1u) / align * align;
            if (o + size <= c->size) {
                f->off = o + size;
                f->used += size;
                *buf = c->buf;
                *off = o;
                return (u8*)c->ptr + o;
            }
            if (f->cur_chunk + 1u < f->nchunk) { f->cur_chunk++; f->off = 0; continue; }
        }
        if (f->nchunk >= VK_FR_MAX_CHUNK) {
            VK_LOG("frame ring exhausted (%u chunks)\n", (unsigned)VK_FR_MAX_CHUNK);
            return NULL;
        }
        VkDeviceSize sz = VK_FR_CHUNK;
        while (sz < size) sz *= 2u;
        vk_buffer* nb = &f->chunk[f->nchunk];
        if (vk_create_host_buffer(nb, sz, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT) || !nb->coherent) {
            vk_destroy_buffer(nb);
            VK_LOG("frame ring: no host-coherent buffer of %llu bytes\n", (unsigned long long)sz);
            return NULL;
        }
        f->cur_chunk = f->nchunk++;
        f->off = 0;
    }
}

/* A descriptor set from the slot's pools; they reset when the slot retires. */
static VkDescriptorSet vk_fr_desc(VkDescriptorSetLayout layout)
{
    vk_frame* f = &s_f.fr[s_f.cur];
    for (;;) {
        if (f->cur_dpool < f->ndpool && f->dpool_sets < VK_FR_DPOOL_SETS) {
            VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                .descriptorPool = f->dpool[f->cur_dpool],
                                                .descriptorSetCount = 1, .pSetLayouts = &layout };
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (pvkAllocateDescriptorSets(s_vk.device, &dai, &set) == VK_SUCCESS) {
                f->dpool_sets++;
                return set;
            }
        }
        if (f->cur_dpool < f->ndpool) {
            f->cur_dpool++;
            f->dpool_sets = 0;
            if (f->cur_dpool < f->ndpool) continue;
        }
        if (f->ndpool >= VK_FR_MAX_DPOOL) {
            VK_LOG("frame descriptor pools exhausted\n");
            return VK_NULL_HANDLE;
        }
        VkDescriptorPoolSize ps[3] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2u * VK_FR_DPOOL_SETS },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, (RSX_MAX_TEXTURES + RSX_MAX_VERTEX_TEXTURES) * VK_FR_DPOOL_SETS },
            { VK_DESCRIPTOR_TYPE_SAMPLER, (RSX_MAX_TEXTURES + RSX_MAX_VERTEX_TEXTURES) * VK_FR_DPOOL_SETS },
        };
        VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                           .maxSets = VK_FR_DPOOL_SETS, .poolSizeCount = 3,
                                           .pPoolSizes = ps };
        if (pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &f->dpool[f->ndpool]) != VK_SUCCESS) {
            VK_LOG("vkCreateDescriptorPool (frame) failed\n");
            return VK_NULL_HANDLE;
        }
        f->cur_dpool = f->ndpool++;
        f->dpool_sets = 0;
    }
}

static void vk_rec_barrier(vk_rec* r)
{
    vk_cmd_after_everything(r->cb);
    r->other = 0;
    r->nbatch = 0;
}

/* The command buffer, begun (and ordered after all earlier submissions) on
 * first use. */
static VkCommandBuffer vk_rec_get(vk_rec* r)
{
    if (r->open) return r->cb;
    if (!s_f.on) return VK_NULL_HANDLE;
    VkCommandBuffer cb = vk_fr_cb(&s_f.fr[s_f.cur]);
    if (!cb) { VK_LOG("out of frame command buffers\n"); return VK_NULL_HANDLE; }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        VK_LOG("vkBeginCommandBuffer (frame) failed\n"); return VK_NULL_HANDLE;
    }
    r->cb = cb;
    r->open = 1;
    vk_rec_barrier(r);
    if (r == &s_f.main) s_f.b_valid = 0;
    return cb;
}

static void vkq_on_pass_end(void);                       /* occlusion queries */
static void vkq_mark_skipped(void);
static void vkq_on_pass_begin(VkCommandBuffer cb, int after);

static void vk_pass_end(void)
{
    if (!s_f.pass) return;
    vkq_on_pass_end();
    pvkCmdEndRenderPass(s_f.main.cb);
    s_f.pass = 0;
    s_f.main.other = 1;
}

/* For a command that reads or writes anything: after all earlier work. */
static VkCommandBuffer vk_rec_xfer(vk_rec* r)
{
    if (r == &s_f.main) vk_pass_end();
    VkCommandBuffer cb = vk_rec_get(r);
    if (cb && (r->other || r->nbatch)) vk_rec_barrier(r);
    r->other = 1;
    return cb;
}

/* For a command that writes only `img` and reads only the ring: unordered
 * against other such writes into other images. */
static VkCommandBuffer vk_rec_upload(vk_rec* r, VkImage img)
{
    if (r == &s_f.main) vk_pass_end();
    VkCommandBuffer cb = vk_rec_get(r);
    if (!cb) return VK_NULL_HANDLE;
    int hit = r->other || r->nbatch == VK_BATCH_MAX;
    for (u32 i = 0; i < r->nbatch && !hit; i++) hit = (r->batch[i] == img);
    if (hit) vk_rec_barrier(r);
    r->batch[r->nbatch++] = img;
    return cb;
}

/* Submit what is recorded (pre in front of main) on the slot's next fence;
 * with `wait`, wait for every slot and retire them all. */
static void vk_flush(int wait)
{
    if (!s_f.on) return;
    vk_pass_end();
    vk_frame* f = &s_f.fr[s_f.cur];
    VkCommandBuffer cbs[2];
    u32 n = 0;
    if (s_f.pre.open) {
        if (pvkEndCommandBuffer(s_f.pre.cb) == VK_SUCCESS) cbs[n++] = s_f.pre.cb;
        else VK_LOG("vkEndCommandBuffer (pre) failed\n");
        s_f.pre.open = 0;
    }
    if (s_f.main.open) {
        if (pvkEndCommandBuffer(s_f.main.cb) == VK_SUCCESS) cbs[n++] = s_f.main.cb;
        else VK_LOG("vkEndCommandBuffer (frame) failed\n");
        s_f.main.open = 0;
    }
    s_f.b_valid = 0;
    if (n) {
        VkFence fe = vk_fr_fence(f);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = n, .pCommandBuffers = cbs };
        const VkResult r = vk_queue_submit(&si, fe);
        if (r != VK_SUCCESS) VK_LOG("vkQueueSubmit (frame) failed (VkResult %d)\n", (int)r);
        else if (fe) f->nsub++;
        if (!fe) wait = 1;
    }
    if (wait || f->nsub >= vk_fr_sub_limit()) vk_fr_wait_all();
}

/* Start the next slot: wait for its last use, then recycle it. */
static void vk_fr_advance(void)
{
    if (!s_f.on) return;
    s_f.cur = (s_f.cur + 1u) % VK_FRAMES;
    vk_fr_wait(&s_f.fr[s_f.cur]);
    s_f.epoch++;
}

/* Called before an operation takes anything from the ring: a slot that has
 * grown past its budget without a present (a loading screen streaming
 * textures) is submitted and the next one started, so memory stays bounded. */
static void vk_op_begin(void)
{
    const vk_frame* f = &s_f.fr[s_f.cur];
    if (s_f.on && (f->used >= VK_FR_SOFT_BYTES || f->draws >= VK_FR_SOFT_DRAWS)) {
        vk_flush(0);
        vk_fr_advance();
    }
}

/* PS3RECOMP_VK_SYNC=1: every operation completes before the next. */
static void vk_op_done(void)
{
    if (s_f.sync) vk_flush(1);
}

static int vk_fr_init(void)
{
    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    s_f.ubo_align = p.limits.minUniformBufferOffsetAlignment ? p.limits.minUniformBufferOffsetAlignment : 256u;
    s_f.sync = 0;
    { const char* e = getenv("PS3RECOMP_VK_SYNC"); s_f.sync = e && e[0] && e[0] != '0'; }
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                    .queueFamilyIndex = s_vk.queue_family };
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (u32 i = 0; i < VK_FRAMES; i++) {
        VK_CHECK(pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_f.fr[i].pool), "vkCreateCommandPool(frame)");
        VK_CHECK(pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_f.fr[i].acquire), "vkCreateSemaphore(frame)");
    }
    s_f.on = 1;
    VK_LOG("frame recording: %u frames in flight%s\n", (unsigned)VK_FRAMES,
           s_f.sync ? ", PS3RECOMP_VK_SYNC=1 (every operation waited on)" : "");
    return 0;
}

/* Device idle. */
static void vk_fr_destroy(void)
{
    if (!s_vk.device) return;
    for (u32 i = 0; i < VK_FRAMES; i++) {
        vk_frame* f = &s_f.fr[i];
        for (u32 k = 0; k < f->ntrash; k++) vk_destroy_trash(&f->trash[k]);
        free(f->trash);
        for (u32 k = 0; k < f->nchunk; k++) vk_destroy_buffer(&f->chunk[k]);
        for (u32 k = 0; k < f->ndpool; k++) pvkDestroyDescriptorPool(s_vk.device, f->dpool[k], NULL);
        for (u32 k = 0; k < f->nfence; k++) pvkDestroyFence(s_vk.device, f->fence[k], NULL);
        if (f->pool) pvkDestroyCommandPool(s_vk.device, f->pool, NULL);
        if (f->acquire) pvkDestroySemaphore(s_vk.device, f->acquire, NULL);
    }
    vk_destroy_buffer(&s_f.rb);
    memset(&s_f, 0, sizeof s_f);
}

static int vk_begin(void)
{
    /* The one-shot path runs after whatever the frame path has recorded. */
    if (s_f.pre.open || s_f.main.open) vk_flush(1);
    VK_CHECK(pvkResetCommandBuffer(s_vk.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_CHECK(pvkBeginCommandBuffer(s_vk.cmd, &bi), "vkBeginCommandBuffer");
    /* A present's copy may still be reading the surface this frame is about
     * to draw over: order everything here after it. Set only by a present,
     * which never runs alongside the walker's recording. */
    if (s_vk.present_order) { vk_cmd_after_everything(s_vk.cmd); s_vk.present_order = 0; }
    return 0;
}

static int vk_submit_and_wait(void)
{
    VK_CHECK(pvkEndCommandBuffer(s_vk.cmd), "vkEndCommandBuffer");
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &s_vk.cmd };
    VK_CHECK(pvkResetFences(s_vk.device, 1, &s_vk.fence), "vkResetFences");
    VK_CHECK(vk_queue_submit(&si, s_vk.fence), "vkQueueSubmit");
    VK_CHECK(pvkWaitForFences(s_vk.device, 1, &s_vk.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    return 0;
}

/* Order this command buffer's use of an image after everything submitted
 * before it; from UNDEFINED it is also the one-time move to GENERAL. */
static void vk_barrier_image_on(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                                VkImageLayout old_layout)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = old_layout, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS },
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void vk_barrier_image(VkImage img, VkImageAspectFlags aspect, VkImageLayout old_layout)
{
    vk_barrier_image_on(s_vk.cmd, img, aspect, old_layout);
}

/* ---------------------------------------------------------------------------
 * Texture unit 0
 * -------------------------------------------------------------------------*/
static void vk_write_texture_descriptor(void)
{
    VkDescriptorImageInfo ii = { .sampler = s_vk.sampler,
                                 .imageView = s_vk.tex_ready[0] ? s_vk.tex[0].view
                                                                : s_vk.dummy.view,
                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = s_vk.desc_set, .dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &ii };
    pvkUpdateDescriptorSets(s_vk.device, 1, &w, 0, NULL);
}

/* Replace *dst with an RGBA8 image of `nlv` levels: level m is lw[m] x lh[m]
 * texels, tightly packed, the levels one after another in `rgba`. */
static int vk_upload_texture_levels(vk_image* dst, const u8* rgba,
                                    const u32* lw, const u32* lh, u32 nlv)
{
    VkBufferImageCopy regions[RSX_MAX_TEXTURE_LEVELS];
    if (!nlv || nlv > RSX_MAX_TEXTURE_LEVELS) return -1;
    VkDeviceSize total = 0;
    for (u32 m = 0; m < nlv; m++) {
        regions[m] = (VkBufferImageCopy){
            .bufferOffset = total,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 },
            .imageExtent = { lw[m], lh[m], 1 },
        };
        total += (VkDeviceSize)lw[m] * lh[m] * 4u;
    }
    vk_buffer staging = {0};
    if (vk_create_host_buffer(&staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !staging.coherent) {
        vk_destroy_buffer(&staging); return -1;
    }
    memcpy(staging.ptr, rgba, (size_t)total);

    vk_image fresh = {0};
    if (vk_create_image_levels(&fresh, VK_FORMAT_R8G8B8A8_UNORM, lw[0], lh[0], nlv,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&fresh); vk_destroy_buffer(&staging); return -1;
    }
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(fresh.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        pvkCmdCopyBufferToImage(s_vk.cmd, staging.buf, fresh.img, VK_IMAGE_LAYOUT_GENERAL,
                                nlv, regions);
        rc = vk_submit_and_wait();
    }
    vk_destroy_buffer(&staging);
    if (rc) { vk_destroy_image(&fresh); return -1; }

    vk_destroy_image(dst);          /* idle: every submission is waited on */
    *dst = fresh;
    return 0;
}

static int vk_upload_texture(vk_image* dst, const u8* rgba, u32 w, u32 h)
{
    return vk_upload_texture_levels(dst, rgba, &w, &h, 1);
}

/* NV4097_SET_TEXTURE_ADDRESS wrap field, one per axis -- the table the Metal
 * backend and the live draw engine use: 1 WRAP, 2 MIRROR, 3 CLAMP_TO_EDGE,
 * 4 BORDER, 5 CLAMP, 6..8 MIRROR_ONCE. Vulkan 1.0 has MIRROR_CLAMP_TO_EDGE
 * only behind an extension, so the MIRROR_ONCE family clamps to edge here --
 * exact for texcoords in [0,1], the mirrored half is what is lost. */
static VkSamplerAddressMode vk_gcm_wrap(u32 w)
{
    switch (w & 0xFu) {
    case 1:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 6: case 7: case 8:                                    /* MIRROR_ONCE_* */
        return s_vk.mirror_clamp_ok ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
                                    : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;      /* 3, 5, unset */
    }
}

/* RPCS3 vk::border_color_t(color4f): rsx_vk_border_select (core colour by
 * storage key, else FLOAT_CUSTOM_EXT with VK_EXT_custom_border_color, else
 * the closest core colour). *key: the custom colour's storage key or 0. */
static VkBorderColor vk_eng_border(const float rgba[4], u32* key)
{
    return (VkBorderColor)rsx_vk_border_select(rgba, s_vk.custom_border_ok, key);
}

/* A unit's sampler from its registers, decoded as the Metal backend's
 * samp_slot_for does: SET_TEXTURE_FILTER min at [18:16] (1 NEAREST,
 * 2 LINEAR, 3..6 the four nearest/linear x nearest/linear-mip pairs), mag at
 * [26:24]; SET_TEXTURE_CONTROL0 max LOD at [18:7], min LOD at [30:19], 4.8
 * fixed point. The LOD bias (FILTER [12:0]) is not applied, as in Metal.
 * VK_NULL_HANDLE on failure: the caller falls back to `sampler`. */
static VkSampler vk_unit_sampler(const rsx_texture_state* t)
{
    const u32 minf = (t->filter >> 16) & 7u, magf = (t->filter >> 24) & 7u;
    const u32 lod  = (t->control0 >> 7) & 0xFFFFFFu;       /* max then min LOD */
    const u64 key  = (u64)minf | ((u64)magf << 3)
                   | ((u64)(t->address & 0x000F0F0Fu) << 6) | ((u64)lod << 26);
    for (u32 i = 0; i < s_vk.samp_count; i++)
        if (s_vk.samp_cache[i].key == key) return s_vk.samp_cache[i].s;
    if (s_vk.samp_count >= VK_SAMPLER_CACHE) return VK_NULL_HANDLE;

    const int mip_present = (minf >= 3);
    const int mip_linear  = (minf == 5 || minf == 6);
    /* A min filter with no mip term samples level 0 only: pin the range. */
    const float min_lod = (float)((t->control0 >> 19) & 0xFFFu) / 256.0f;
    float max_lod = mip_present ? (float)((t->control0 >> 7) & 0xFFFu) / 256.0f : 0.0f;
    if (max_lod < min_lod) max_lod = min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = (magf == 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (minf == 2 || minf == 4 || minf == 6) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(t->address),
        .addressModeV = vk_gcm_wrap(t->address >> 8),
        .addressModeW = vk_gcm_wrap(t->address >> 16),
        .minLod = min_lod, .maxLod = max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    VkSampler smp = VK_NULL_HANDLE;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &smp) != VK_SUCCESS) return VK_NULL_HANDLE;
    s_vk.samp_cache[s_vk.samp_count].key = key;
    s_vk.samp_cache[s_vk.samp_count].s   = smp;
    s_vk.samp_count++;
    return smp;
}

/* One level of a guest texture into tightly packed RGBA8, with the null
 * backend's decode (rsx_null_backend.c, nullsw_bind_texture): formats that
 * decode to fewer than four bytes are splayed, alpha forced opaque. */
static int vk_decode_level_rgba8(u8* dst, const u8* src, u32 w, u32 h,
                                 const rsx_tex_layout* tl)
{
    const u32 pitch = w * 4u;
    if (tl->fmt == RSX_TEXFMT_R8G8B8A8) {
        rsx_texture_decode(dst, pitch, src, w, h, tl, rsx_texture_argb_is_rgba());
        return 0;
    }
    const u32 srcp = tl->row_bytes;
    u8* tmp = (u8*)malloc((size_t)srcp * h);
    if (!tmp) return -1;
    rsx_texture_decode(tmp, srcp, src, w, h, tl, 0);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const u8* sp = tmp + (size_t)y * srcp + (size_t)x * tl->bytes_per_texel;
            u8* dp = dst + (size_t)y * pitch + (size_t)x * 4u;
            dp[0] = sp[0];
            dp[1] = tl->bytes_per_texel > 1 ? sp[1] : sp[0];
            dp[2] = tl->bytes_per_texel > 2 ? sp[2] : sp[0];
            dp[3] = 255;
        }
    free(tmp);
    return 0;
}

/* Unit `unit` has nothing usable bound. The fallback path's descriptor only
 * ever shows unit 0, so only that one needs rewriting. */
static void vk_tex_off(u32 unit)
{
    s_vk.tex_ready[unit] = 0;
    s_vk.tex_sampler[unit] = VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* Same decode as the headless null backend (rsx_null_backend.c,
 * nullsw_bind_texture), so both backends sample identical texels. */
static void vk_cb_bind_texture(void* ud, u32 unit, const rsx_texture_state* t)
{
    (void)ud;
    if (unit >= RSX_MAX_TEXTURES || !t || !s_vk.device) return;
    if (!(t->control0 & 0x80000000u) || !t->offset) { vk_tex_off(unit); return; }

    u32 w = (t->image_rect >> 16) & 0xFFFFu;
    u32 h =  t->image_rect        & 0xFFFFu;
    if (!w || !h || w > 4096u || h > 4096u) { vk_tex_off(unit); return; }

    u32 ea = cellGcmResolveLocated((t->format & 3u) == 1u, t->offset);
    if (!vm_base || ea == 0xFFFFFFFFu) { vk_tex_off(unit); return; }
    const u32 fmt = (t->format >> 8) & 0xFFu;

    /* The whole mip chain, as the Metal backend reads it: SET_TEXTURE_FORMAT's
     * level count above bit 15 and SET_TEXTURE_CONTROL3's row pitch, laid out
     * by the shared rsx_texture_mip_chain() (which clamps the count to what
     * the dimensions allow). Cube maps are not handled yet: face 0 only. */
    const u32 levels    = (t->format >> 16) & 0xFFFFu;
    const u32 row_pitch = t->control3 & 0xFFFFFu;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, row_pitch, lv);
    if (!nlv || lv[0].tl.compressed) { vk_tex_off(unit); return; }   /* no BC path yet */

    size_t total = 0, off[RSX_MAX_TEXTURE_LEVELS];
    u32 lw[RSX_MAX_TEXTURE_LEVELS], lh[RSX_MAX_TEXTURE_LEVELS];
    for (u32 m = 0; m < nlv; m++) {
        off[m] = total;
        lw[m] = lv[m].w; lh[m] = lv[m].h;
        total += (size_t)lv[m].w * lv[m].h * 4u;
    }
    u8* buf = (u8*)malloc(total);
    if (!buf) { vk_tex_off(unit); return; }
    for (u32 m = 0; m < nlv; m++)
        if (vk_decode_level_rgba8(buf + off[m], vm_base + ea + lv[m].offset,
                                  lv[m].w, lv[m].h, &lv[m].tl)) {
            free(buf); vk_tex_off(unit); return;
        }
    s_vk.tex_ready[unit] = vk_upload_texture_levels(&s_vk.tex[unit], buf, lw, lh, nlv) == 0;
    free(buf);
    s_vk.tex_sampler[unit] = s_vk.tex_ready[unit] ? vk_unit_sampler(t) : VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* ---------------------------------------------------------------------------
 * Render pass, framebuffer, pipelines
 * -------------------------------------------------------------------------*/
static int vk_pick_depth_format(void)
{
    /* With stencil first -- titles use it for shadows, reflections and UI
     * masks -- and D32 first within each group: a D32 depth aspect copies out
     * as 32-bit floats, which is what depth snapshots are built from. */
    static const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM
    };
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if ((fp.optimalTilingFeatures & need) == need) {
            s_vk.depth_format = candidates[i]; goto chosen;
        }
    }
    /* TRANSFER_DST is a 1.1 format feature bit; 1.0 drivers may not report
     * it although clears work. Retry on the attachment bit alone. */
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            s_vk.depth_format = candidates[i]; goto chosen;
        }
    }
    VK_LOG("no usable depth format\n");
    return -1;
chosen:
    s_vk.depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (s_vk.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
        s_vk.depth_format == VK_FORMAT_D24_UNORM_S8_UINT)
        s_vk.depth_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return 0;
}

static int vk_create_render_pass(void)
{
    VkAttachmentDescription att[2] = {
        { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .format = s_vk.depth_format, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,     /* ignored without stencil */
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
    };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 2, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    VK_CHECK(pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_vk.render_pass), "vkCreateRenderPass");

    VkImageView views[2] = { s_vk.color.view, s_vk.depth.view };
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = s_vk.render_pass, .attachmentCount = 2,
                                    .pAttachments = views, .width = s_vk.width,
                                    .height = s_vk.height, .layers = 1 };
    VK_CHECK(pvkCreateFramebuffer(s_vk.device, &fci, NULL, &s_vk.framebuffer), "vkCreateFramebuffer");
    return 0;
}

static int vk_create_shader(const uint32_t* code, size_t bytes, VkShaderModule* out)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = bytes, .pCode = code };
    VK_CHECK(pvkCreateShaderModule(s_vk.device, &ci, NULL, out), "vkCreateShaderModule");
    return 0;
}

static int vk_create_pipeline_objects(void)
{
    if (vk_create_shader(rsx_vk_fallback_vert_spv, sizeof rsx_vk_fallback_vert_spv, &s_vk.vs) ||
        vk_create_shader(rsx_vk_fallback_frag_spv, sizeof rsx_vk_fallback_frag_spv, &s_vk.fs))
        return -1;

    VkDescriptorSetLayoutBinding bind = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &bind };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_vk.set_layout),
             "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_vk.desc_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_vk.desc_pool,
                                        .descriptorSetCount = 1, .pSetLayouts = &s_vk.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_vk.desc_set), "vkAllocateDescriptorSets");

    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int32_t) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_vk.set_layout,
                                        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_vk.pipe_layout),
             "vkCreatePipelineLayout");

    /* Nearest + repeat: the null backend's point sample with wrapping. */
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .maxLod = 0.0f };
    VK_CHECK(pvkCreateSampler(s_vk.device, &sci, NULL, &s_vk.sampler), "vkCreateSampler");

    /* A 1x1 dummy keeps the descriptor valid before any bind. Transparent
     * black, as RPCS3's vk::null_image_view (vkutils/scratch.cpp), which is
     * what a disabled or failed unit samples there. */
    static const u8 null_texel[4] = { 0x00, 0x00, 0x00, 0x00 };
    if (vk_upload_texture(&s_vk.dummy, null_texel, 1, 1)) return -1;
    vk_write_texture_descriptor();
    return 0;
}

/* NV4097 depth function is the GL enum 0x200 NEVER .. 0x207 ALWAYS, which is
 * VkCompareOp 0..7 in the same order. Anything else passes, as in the null
 * and D3D12 backends (including the 1 rsx_state_init seeds). */
static u32 vk_compare_slot(u32 gl_func)
{
    return (gl_func >= 0x200u && gl_func <= 0x207u) ? gl_func - 0x200u
                                                    : (u32)VK_COMPARE_OP_ALWAYS;
}

static VkPipeline vk_get_pipeline(int ztest, int zwrite, u32 cmp)
{
    u32 slot = ((u32)ztest * 2u + (u32)zwrite) * 8u + cmp;
    if (s_vk.pipelines[slot]) return s_vk.pipelines[slot];

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_vk.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_vk.fs, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, sizeof(vk_vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[3] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, pos) },
        { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, col) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(vk_vertex, uv)  },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* No culling: the null backend fills both windings. */
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
        .pNext = vk_provoking_chain() };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_vk.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = pvkCreateGraphicsPipelines(s_vk.device, s_vk.pcache, 1, &gci, NULL, &p);
    if (r != VK_SUCCESS) { VK_LOG("vkCreateGraphicsPipelines failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
    s_vk.pipelines[slot] = p;
    return p;
}

/* ---------------------------------------------------------------------------
 * Draw path
 * -------------------------------------------------------------------------*/
static void vk_cb_track_state(void* ud, const rsx_state* state)
{
    (void)ud; s_vk.state = state;
}

static int vk_push_vertex(const float pos[4], const float col[4], const float uv[4])
{
    if (s_vk.cpu_count == s_vk.cpu_cap) {
        size_t cap = s_vk.cpu_cap ? s_vk.cpu_cap * 2 : 1024;
        vk_vertex* n = (vk_vertex*)realloc(s_vk.cpu_verts, cap * sizeof *n);
        if (!n) return -1;
        s_vk.cpu_verts = n; s_vk.cpu_cap = cap;
    }
    vk_vertex* v = &s_vk.cpu_verts[s_vk.cpu_count++];
    memcpy(v->pos, pos, sizeof v->pos);
    memcpy(v->col, col, sizeof v->col);
    v->uv[0] = uv[0]; v->uv[1] = uv[1];
    return 0;
}

/* One triangle, colour from its first vertex (the null backend's flat shade). */
static void vk_emit_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    float p[3][4], uv[3][4], col[4];
    rsx_fetch_attrib(st, 0, i0, p[0]);
    rsx_fetch_attrib(st, 0, i1, p[1]);
    rsx_fetch_attrib(st, 0, i2, p[2]);
    rsx_fetch_attrib(st, 3, i0, col);
    rsx_fetch_attrib(st, 8, i0, uv[0]);
    rsx_fetch_attrib(st, 8, i1, uv[1]);
    rsx_fetch_attrib(st, 8, i2, uv[2]);
    for (int k = 0; k < 3; k++) vk_push_vertex(p[k], col, uv[k]);
}

/* Called once per expanded triangle: the fixed-function path records the
 * null backend's flat-shaded vertex, the guest path all sixteen attributes. */
typedef void (*vk_tri_fn)(const rsx_state* st, u32 i0, u32 i1, u32 i2);

/* Primitive expansion, identical to the null backend's draw_prim(). */
static void vk_expand(const rsx_state* st, u32 prim, u32 first, u32 count, vk_tri_fn emit)
{
    switch (prim) {
    case RSX_PRIMITIVE_TRIANGLES:
        for (u32 i = 0; i + 2 < count; i += 3) emit(st, first + i, first + i + 1, first + i + 2);
        break;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 i = 0; i + 2 < count; i++)
            emit(st, first + i, first + i + 1 + (i & 1), first + i + 2 - (i & 1));
        break;
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 i = 1; i + 1 < count; i++) emit(st, first, first + i, first + i + 1);
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 i = 0; i + 3 < count; i += 4) {
            emit(st, first + i, first + i + 1, first + i + 2);
            emit(st, first + i, first + i + 2, first + i + 3);
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 i = 0; i + 3 < count; i += 2) {
            emit(st, first + i, first + i + 1, first + i + 3);
            emit(st, first + i, first + i + 3, first + i + 2);
        }
        break;
    default:
        break;   /* points and lines: no coverage, as in the null backend */
    }
}

static int vk_ensure_vertex_buffer(VkDeviceSize bytes)
{
    if (s_vk.vertices.buf && s_vk.vertices.size >= bytes) return 0;
    VkDeviceSize size = s_vk.vertices.size ? s_vk.vertices.size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(&s_vk.vertices);   /* idle: every submission is waited on */
    if (vk_create_host_buffer(&s_vk.vertices, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return -1;
    if (!s_vk.vertices.coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Guest programs (stage V3)
 *
 * With the HLSL -> SPIR-V translator built and PS3RECOMP_VK_GUEST_PROGRAMS=1,
 * a draw runs the guest's own vertex and fragment programs, as the Metal
 * backend's guest path does and keyed the same way: decompiled to HLSL by the
 * shared decompilers, translated by rsx_shader_spirv.cpp, cached per program.
 * Constants use the Metal/D3D12 layout: VPConst is the 512 transform
 * constants plus the viewport epilogue, PSConstants the fragment program's
 * inline constants plus fp_alpha.
 *
 * Vertices are still fetched and expanded on the CPU through the shared
 * rsx_fetch_attrib(), now all sixteen attributes (a disabled one reads its
 * constant register, as on the hardware). Every RSX vertex format therefore
 * arrives as float4, and the pipeline has one fixed input layout: location n
 * is attribute n, which is what the vertex decompiler declares.
 *
 * A program that needs a cube map or a vertex texture falls back to the
 * fixed-function path, with a message. Not honoured on this path yet: the RSX
 * viewport and scissor (the whole target is used, as on the fallback path),
 * blending, stencil, culling, per-unit sampler state.
 * -------------------------------------------------------------------------*/
#define VK_VP_CACHE      256
#define VK_FP_CACHE      512
#define VK_GPIPE_CACHE   512
#define VK_FP_MAX_BYTES  4096u              /* bound on a fragment program, as Metal/D3D12 */
#define VK_HLSL_BYTES    (256u * 1024u)
#define VK_SPV_WORDS     (256u * 1024u)
#define VK_VPCONST_BYTES ((RSX_MAX_VERTEX_CONSTANTS + 2u) * 16u)
#define VK_GUEST_ATTRS   16u
#define VK_GUEST_FLOATS  (VK_GUEST_ATTRS * 4u)   /* per vertex */

typedef struct { u32 hash; VkShaderModule mod; } vk_vp_entry;
typedef struct { u64 key; VkShaderModule mod; u32 nconst; } vk_fp_entry;
typedef struct { int vs, fs; u32 depth_key; VkPipeline pipe; } vk_gpipe_entry;

static struct {
    int                   on;
    char*                 hlsl;
    u32*                  spv;
    char                  log[4096];
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout      pipe_layout;
    VkDescriptorPool      pool;
    VkDescriptorSet       set;
    vk_buffer             ubo;              /* VPConst at 0, PSConstants at ps_off */
    VkDeviceSize          ps_off;
    VkDeviceSize          max_ubo_range;
    vk_buffer             vertices;
    float*                cpu;
    size_t                cpu_cap, cpu_count;   /* in vertices */
    vk_vp_entry           vp[VK_VP_CACHE];     u32 vp_count;
    vk_fp_entry           fp[VK_FP_CACHE];     u32 fp_count;
    vk_gpipe_entry        pipes[VK_GPIPE_CACHE]; u32 pipe_count;
    rsx_fp_constant_block consts;
    u32                   draws, last_draws;
    u32                   warned;
} s_g;

int rsx_vulkan_backend_guest_programs(void)
{
    const char* v = getenv("PS3RECOMP_VK_GUEST_PROGRAMS");
    return rsx_hlsl_to_spirv_available() && v && v[0] && v[0] != '0';
}

static u32 vk_fnv1a32(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static u64 vk_fnv1a64(const void* d, size_t n, u64 h)
{
    const u8* p = (const u8*)d;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* Built from the registers, as the Metal and D3D12 backends do. */
static u32 vk_cube_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const rsx_texture_state* t = &st->textures[u];
        const u32 w = (t->image_rect >> 16) & 0xFFFFu, h = t->image_rect & 0xFFFFu;
        if ((t->control0 & 0x80000000u) && (t->format & 4u) && w && w == h) m |= 1u << u;
    }
    return m;
}

static u32 vk_vtex_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++)
        if (st->vertex_textures[u].control0 & 0x80000000u) m |= 1u << u;
    return m;
}

static VkShaderModule vk_guest_module(const char* what, int stage)
{
    u32 n = 0;
    if (rsx_hlsl_to_spirv(s_g.hlsl, stage, s_g.spv, VK_SPV_WORDS, &n,
                          s_g.log, sizeof s_g.log) != 0) {
        VK_LOG("%s: %s\n", what, s_g.log);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)n * 4u, .pCode = s_g.spv };
    VkShaderModule m = VK_NULL_HANDLE;
    if (pvkCreateShaderModule(s_vk.device, &ci, NULL, &m) != VK_SUCCESS) {
        VK_LOG("%s: vkCreateShaderModule failed\n", what);
        return VK_NULL_HANDLE;
    }
    return m;
}

/* The vertex program, keyed on its own bytes to its end bit (as Metal). */
static int vk_vp_slot(const rsx_state* st)
{
    if (st->vp_ucode_bytes < 16) return -1;
    u32 vstart = st->transform_program_start * 16u;
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u8* uc = st->vp_ucode + vstart;
    const u32 avail = st->vp_ucode_bytes - vstart;
    const u32 instrs = rsx_vp_program_size_instrs(uc, avail);
    const u32 len = instrs ? instrs * 16u : avail;
    const u32 hash = vk_fnv1a32(uc, len);
    for (u32 i = 0; i < s_g.vp_count; i++)
        if (s_g.vp[i].hash == hash) return s_g.vp[i].mod ? (int)i : -1;
    if (s_g.vp_count >= VK_VP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "vertex program %08X", hash);
    VkShaderModule m = VK_NULL_HANDLE;
    const int ni = rsx_vp_decompile_ex(uc, len, 0, s_g.hlsl, VK_HLSL_BYTES);
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SHADER_STAGE_VERTEX);
    if (s_g.vp_count < 32)
        VK_LOG("%s: %d instrs -> %s\n", what, ni, m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.vp_count++;
    s_g.vp[slot].hash = hash;
    s_g.vp[slot].mod  = m;
    return m ? slot : -1;
}

/* The fragment program, resolved and keyed as the Metal backend's
 * fp_slot_for: structure (inline constants live in PSConstants), export
 * width, and the alpha test patched into the source. No cube units here --
 * the caller has already sent those to the fixed-function path. */
static int vk_fp_slot(const rsx_state* st, const u8** uc)
{
    if (!vm_base || st->shader_program == 0) return -1;
    const u32 off = cellGcmResolveLocated((st->shader_program & 3u) == 1u,
                                          st->shader_program & ~3u);
    if (off == 0xFFFFFFFFu) return -1;
    *uc = vm_base + off;
    u64 key = rsx_fp_structural_hash(*uc, VK_FP_MAX_BYTES, 1469598103934665603ull);
    if (!key) return -1;                       /* malformed or unterminated */
    const u32 ctrl     = st->shader_control;
    const u32 ctrl_key = ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;
    const u32 alpha_en = (st->alpha_test_enable && st->alpha_func != 0x0207u) ? 1u : 0u;
    const u32 alpha_fn = alpha_en ? st->alpha_func : 0u;
    key = vk_fnv1a64(&ctrl_key, sizeof ctrl_key, key);
    key = vk_fnv1a64(&alpha_en, sizeof alpha_en, key);
    key = vk_fnv1a64(&alpha_fn, sizeof alpha_fn, key);
    for (u32 i = 0; i < s_g.fp_count; i++)
        if (s_g.fp[i].key == key) return s_g.fp[i].mod ? (int)i : -1;
    if (s_g.fp_count >= VK_FP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "fragment program %016llX", (unsigned long long)key);
    VkShaderModule m = VK_NULL_HANDLE;
    u32 nconst = 0;
    int ni = rsx_fp_decompile_buffered_ex(*uc, VK_FP_MAX_BYTES, ctrl, 0,
                                          s_g.hlsl, VK_HLSL_BYTES, &nconst);
    if (ni > 0 && alpha_en &&
        rsx_fp_apply_alpha_test_buffered(s_g.hlsl, VK_HLSL_BYTES, st->alpha_func) < 0)
        ni = -1;
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SHADER_STAGE_FRAGMENT);
    if (s_g.fp_count < 32)
        VK_LOG("%s: %d instrs, %u constants -> %s\n", what, ni, nconst,
               m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.fp_count++;
    s_g.fp[slot].key    = key;
    s_g.fp[slot].mod    = m;
    s_g.fp[slot].nconst = nconst;
    return m ? slot : -1;
}

static VkPipeline vk_guest_pipeline(int vs, int fs, int ztest, int zwrite, u32 cmp)
{
    const u32 dkey = (u32)ztest | ((u32)zwrite << 1) | (cmp << 2);
    for (u32 i = 0; i < s_g.pipe_count; i++)
        if (s_g.pipes[i].vs == vs && s_g.pipes[i].fs == fs && s_g.pipes[i].depth_key == dkey)
            return s_g.pipes[i].pipe;
    if (s_g.pipe_count >= VK_GPIPE_CACHE) return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_g.vp[vs].mod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_g.fp[fs].mod, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, VK_GUEST_FLOATS * 4u, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) {
        va[a].location = a;
        va[a].binding  = 0;
        va[a].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[a].offset   = a * 16u;
    }
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = VK_GUEST_ATTRS, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
        .pNext = vk_provoking_chain() };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_g.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = pvkCreateGraphicsPipelines(s_vk.device, s_vk.pcache, 1, &gci, NULL, &p);
    if (r != VK_SUCCESS) {
        VK_LOG("guest pipeline: vkCreateGraphicsPipelines failed (%d)\n", (int)r);
        return VK_NULL_HANDLE;
    }
    vk_gpipe_entry* e = &s_g.pipes[s_g.pipe_count++];
    e->vs = vs; e->fs = fs; e->depth_key = dkey; e->pipe = p;
    return p;
}

/* One vertex: all sixteen attributes as float4, attribute n at float 4n. */
static int vk_guest_push(const rsx_state* st, u32 idx)
{
    if (s_g.cpu_count == s_g.cpu_cap) {
        size_t cap = s_g.cpu_cap ? s_g.cpu_cap * 2 : 1024;
        float* n = (float*)realloc(s_g.cpu, cap * VK_GUEST_FLOATS * sizeof(float));
        if (!n) return -1;
        s_g.cpu = n; s_g.cpu_cap = cap;
    }
    float* v = s_g.cpu + s_g.cpu_count * VK_GUEST_FLOATS;
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) rsx_fetch_attrib(st, (int)a, idx, v + a * 4u);
    s_g.cpu_count++;
    return 0;
}

static void vk_guest_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    vk_guest_push(st, i0);
    vk_guest_push(st, i1);
    vk_guest_push(st, i2);
}

static int vk_guest_init(void)
{
    if (!rsx_vulkan_backend_guest_programs()) return 0;
    s_g.hlsl = (char*)malloc(VK_HLSL_BYTES);
    s_g.spv  = (u32*)malloc(VK_SPV_WORDS * sizeof(u32));
    if (!s_g.hlsl || !s_g.spv) { VK_LOG("guest programs: out of memory\n"); return -1; }

    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    s_g.max_ubo_range = p.limits.maxUniformBufferRange;
    VkDeviceSize align = p.limits.minUniformBufferOffsetAlignment;
    if (align == 0) align = 256;
    s_g.ps_off = (VK_VPCONST_BYTES + align - 1u) / align * align;
    if (VK_VPCONST_BYTES > s_g.max_ubo_range) {
        VK_LOG("guest programs: maxUniformBufferRange %llu < VPConst\n",
               (unsigned long long)s_g.max_ubo_range);
        return -1;
    }

    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[4] = {
        { RSX_SPIRV_VPCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL },
        { RSX_SPIRV_PSCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL },
        { RSX_SPIRV_TEXTURE_BINDING, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { RSX_SPIRV_SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4, .pBindings = b };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_g.set_layout),
             "vkCreateDescriptorSetLayout(guest)");
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES },
        { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_g.pool),
             "vkCreateDescriptorPool(guest)");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_g.pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_g.set), "vkAllocateDescriptorSets(guest)");
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_g.pipe_layout),
             "vkCreatePipelineLayout(guest)");
    if (vk_create_host_buffer(&s_g.ubo,
                              s_g.ps_off + (RSX_FP_MAX_INLINE_CONSTANTS + 1u) * 16u,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !s_g.ubo.coherent) {
        VK_LOG("guest programs: no host-coherent uniform memory\n");
        return -1;
    }
    s_g.on = 1;
    VK_LOG("guest programs: on (HLSL -> SPIR-V through glslang)\n");
    return 0;
}

static void vk_guest_shutdown(void)
{
    if (s_vk.device) {
        for (u32 i = 0; i < s_g.pipe_count; i++)
            if (s_g.pipes[i].pipe) pvkDestroyPipeline(s_vk.device, s_g.pipes[i].pipe, NULL);
        for (u32 i = 0; i < s_g.vp_count; i++)
            if (s_g.vp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.vp[i].mod, NULL);
        for (u32 i = 0; i < s_g.fp_count; i++)
            if (s_g.fp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.fp[i].mod, NULL);
        if (s_g.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_g.pipe_layout, NULL);
        if (s_g.pool)        pvkDestroyDescriptorPool(s_vk.device, s_g.pool, NULL);
        if (s_g.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_g.set_layout, NULL);
        vk_destroy_buffer(&s_g.ubo);
        vk_destroy_buffer(&s_g.vertices);
    }
    free(s_g.hlsl);
    free(s_g.spv);
    free(s_g.cpu);
    memset(&s_g, 0, sizeof s_g);
}

static int vk_ensure_buffer(vk_buffer* b, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
    if (b->buf && b->size >= bytes) return 0;
    VkDeviceSize size = b->size ? b->size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(b);                 /* idle: every submission is waited on */
    if (vk_create_host_buffer(b, size, usage)) return -1;
    if (!b->coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}


/* Returns 1 when the draw went through the guest programs (or had nothing to
 * draw), 0 when the caller must use the fixed-function path instead. */
static int vk_guest_draw(const rsx_state* st, u32 prim, u32 first, u32 count)
{
    if (!s_g.on) return 0;
    if (vk_cube_mask(st) || vk_vtex_mask(st)) {
        if (!(s_g.warned & 1u)) {
            VK_LOG("guest programs: cube maps and vertex textures are not supported "
                   "yet; those draws use the fixed-function path\n");
            s_g.warned |= 1u;
        }
        return 0;
    }
    const int vs = vk_vp_slot(st);
    if (vs < 0) return 0;
    const u8* fp_uc = NULL;
    const int fs = vk_fp_slot(st, &fp_uc);
    if (fs < 0) return 0;
    /* The constant count must be what the program was compiled against. */
    if (rsx_fp_collect_constants(fp_uc, VK_FP_MAX_BYTES, &s_g.consts) < 0 ||
        s_g.consts.count != s_g.fp[fs].nconst)
        return 0;
    const u32 nslots = s_g.consts.count ? s_g.consts.count : 1u;
    const u32 fp_len = (nslots + 1u) * 16u;
    if (fp_len > s_g.max_ubo_range) return 0;

    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_guest_pipeline(vs, fs, ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return 0;

    s_g.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_guest_tri);
    if (!s_g.cpu_count) return 1;
    const VkDeviceSize bytes = (VkDeviceSize)s_g.cpu_count * VK_GUEST_FLOATS * sizeof(float);
    if (vk_ensure_buffer(&s_g.vertices, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
    memcpy(s_g.vertices.ptr, s_g.cpu, (size_t)bytes);

    /* VPConst: the 512 transform constants, then the viewport epilogue as the
     * Metal and D3D12 backends write it -- x/y identity, and z remapped from
     * GL clip space to [0, 1] when the guest programmed a z scale. */
    u8* vpc = (u8*)s_g.ubo.ptr;
    memcpy(vpc, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16u);
    float* vpx = (float*)(vpc + RSX_MAX_VERTEX_CONSTANTS * 16u);
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (st->viewport_scale[2] != 0.0f) { vpx[2] = st->viewport_scale[2]; vpx[6] = st->viewport_offset[2]; }
    else                                { vpx[2] = 1.0f;                  vpx[6] = 0.0f; }
    /* PSConstants: inline constants as host-order bit patterns, then fp_alpha. */
    u8* fpc = (u8*)s_g.ubo.ptr + s_g.ps_off;
    memset(fpc, 0, fp_len);
    if (s_g.consts.count) memcpy(fpc, s_g.consts.values, s_g.consts.count * 16u);
    float* alpha = (float*)(fpc + nslots * 16u);
    alpha[0] = rsx_fp_alpha_ref(st->alpha_ref, st->surface_format & 0x1Fu);

    /* Descriptors: rewritten per draw, which is safe because every submission
     * is waited on before the next one is recorded. */
    VkDescriptorBufferInfo bi[2] = {
        { s_g.ubo.buf, 0, VK_VPCONST_BYTES },
        { s_g.ubo.buf, s_g.ps_off, fp_len },
    };
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE,
                                         s_vk.tex_ready[u] ? s_vk.tex[u].view : s_vk.dummy.view,
                                         VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ s_vk.tex_sampler[u] ? s_vk.tex_sampler[u] : s_vk.sampler,
                                         VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[4] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_PSCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si },
    };
    pvkUpdateDescriptorSets(s_vk.device, 4, w, 0, NULL);

    if (vk_begin()) return 0;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vpt = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vpt);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_g.pipe_layout,
                             0, 1, &s_g.set, 0, NULL);
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_g.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_g.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    if (vk_submit_and_wait()) return 0;
    s_g.draws++;
    return 1;
}

static void vk_draw(u32 prim, u32 first, u32 count)
{
    const rsx_state* st = s_vk.state;
    if (!st || !s_vk.device || count < 3) return;
    if (vk_guest_draw(st, prim, first, count)) return;

    s_vk.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_emit_tri);
    if (!s_vk.cpu_count) return;

    const VkDeviceSize bytes = (VkDeviceSize)s_vk.cpu_count * sizeof(vk_vertex);
    if (vk_ensure_vertex_buffer(bytes)) return;
    memcpy(s_vk.vertices.ptr, s_vk.cpu_verts, (size_t)bytes);

    /* Depth writes need the test enabled, as on the hardware and in both
     * reference backends. */
    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_get_pipeline(ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return;
    const int32_t textured = (s_vk.tex_ready[0] && st->vertex_attribs[8].enabled) ? 1 : 0;

    if (vk_begin()) return;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    /* Whole target, as the null backend's ndc_to_px maps it. */
    VkViewport vp = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vk.pipe_layout,
                             0, 1, &s_vk.desc_set, 0, NULL);
    pvkCmdPushConstants(s_vk.cmd, s_vk.pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof textured, &textured);
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_vk.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_vk.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    vk_submit_and_wait();
}

static void vk_cb_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    vk_draw(primitive, first, count);
}

static void vk_cb_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    /* Not read yet, as in the null backend: the harness geometry is
     * non-indexed. Real titles need this; it lands with the index fetch. */
    (void)ud; (void)primitive; (void)offset; (void)count;
}

/* ---------------------------------------------------------------------------
 * Clear and present
 * -------------------------------------------------------------------------*/
static void vk_cb_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud; (void)stencil;                       /* no stencil buffer yet */
    s_vk.clear_argb = color;
    /* CLEAR_SURFACE bits: 0x01 depth, 0x02 stencil, 0xF0 the colour channels
     * (all or nothing, as in the null backend). */
    const int do_color = (flags & 0xF0u) != 0;
    const int do_depth = (flags & 0x01u) != 0;
    if (!s_vk.device || (!do_color && !do_depth)) return;

    if (vk_begin()) return;
    if (do_color) {
        VkClearColorValue v;
        v.float32[0] = (float)((color >> 16) & 0xFFu) / 255.0f;   /* R */
        v.float32[1] = (float)((color >>  8) & 0xFFu) / 255.0f;   /* G */
        v.float32[2] = (float)( color        & 0xFFu) / 255.0f;   /* B */
        v.float32[3] = (float)((color >> 24) & 0xFFu) / 255.0f;   /* A */
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    }
    if (do_depth) {
        VkClearDepthStencilValue d = { depth, 0 };
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    }
    vk_submit_and_wait();
}

/* ---------------------------------------------------------------------------
 * Optional window: PS3RECOMP_VK_WINDOW=1
 *
 * An SDL2 window with a Vulkan swapchain. Rendering still targets the
 * offscreen image, so readback and every test are unchanged; a present then
 * also blits that image into the next swapchain image. Any failure on this
 * path drops back to headless with a message instead of failing the run.
 * -------------------------------------------------------------------------*/
#ifndef _WIN32
static int vk_env_on(const char* name)
{
    const char* v = getenv(name);
    return v && v[0] && v[0] != '0';
}
#endif

static void vk_window_drop(const char* why)
{
    if (why) VK_LOG("window disabled, continuing headless: %s\n", why);
    if (s_vk.device && pvkDeviceWaitIdle) {
        vk_device_idle();
        for (u32 i = 0; i < 8; i++)
            if (s_vk.sc_done[i]) {
                pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
                s_vk.sc_done[i] = VK_NULL_HANDLE;
            }
        if (s_vk.sem_acquire) pvkDestroySemaphore(s_vk.device, s_vk.sem_acquire, NULL);
        if (s_vk.swapchain && pvkDestroySwapchainKHR)
            pvkDestroySwapchainKHR(s_vk.device, s_vk.swapchain, NULL);
    }
    if (s_vk.surface && s_vk.instance && pvkDestroySurfaceKHR)
        pvkDestroySurfaceKHR(s_vk.instance, s_vk.surface, NULL);
    if (s_vk.window) {
#ifndef _WIN32
        SDL_DestroyWindow(s_vk.window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
#endif
    }
    s_vk.window = NULL;
    s_vk.surface = VK_NULL_HANDLE;
    s_vk.swapchain = VK_NULL_HANDLE;
    s_vk.sem_acquire = VK_NULL_HANDLE;
    s_vk.sc_count = 0;
    s_vk.windowed = 0;
}

static void vk_window_open(const char* title)
{
#ifdef _WIN32
    (void)title;
    if (!s_vk_hwnd) return;
    s_vk.window = s_vk_hwnd;
    s_vk.windowed = 1;
    s_vk.inst_exts[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    s_vk.inst_exts[1] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    s_vk.inst_ext_count = 2;
    VK_LOG("window: host HWND %p\n", (void*)s_vk_hwnd);
}
#else
    if (!vk_env_on("PS3RECOMP_VK_WINDOW")) return;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        VK_LOG("window disabled, continuing headless: SDL video init failed: %s\n", SDL_GetError());
        return;
    }
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN;
    if (vk_env_on("PS3RECOMP_VK_FULLSCREEN")) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    s_vk.window = SDL_CreateWindow(title && title[0] ? title : "ps3recomp",
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   (int)s_vk.width, (int)s_vk.height, flags);
    s_vk.windowed = 1;          /* so vk_window_drop also shuts SDL video down */
    if (!s_vk.window) {
        char why[256];
        snprintf(why, sizeof why, "SDL_CreateWindow failed: %s", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    unsigned n = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, NULL) || n == 0 || n > 16 ||
        !SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, s_vk.inst_exts)) {
        char why[256];
        snprintf(why, sizeof why, "no Vulkan surface extensions (%s)", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    s_vk.inst_ext_count = n;
    const char* hold = getenv("PS3RECOMP_VK_HOLD");
    s_vk.hold_seconds = hold ? atof(hold) : 0.0;
    VK_LOG("window: SDL video driver '%s', %u surface extension(s)\n",
           SDL_GetCurrentVideoDriver(), n);
}
#endif

static u32 s_sc_client_w, s_sc_client_h;  /* window client size at swapchain creation */

static int vk_create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    /* RPCS3 swapchain_WSI::init_surface_capabilities: with RSX_DISPLAY set
     * and VK_EXT_full_screen_exclusive, the capabilities of the window's
     * monitor say whether exclusive fullscreen can be requested at all. */
    int fse_supported = 0;
    const int excl = rsx_vk_exclusive_mode(getenv("RSX_DISPLAY"));
    int have_caps = 0;
#ifdef _WIN32
    if (excl != RSX_VK_EXCL_UNSPECIFIED && s_vk.fse_ok) {
        HMONITOR mon = MonitorFromWindow((HWND)s_vk.window, MONITOR_DEFAULTTOPRIMARY);
        PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR caps2 = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)
            pvkGetInstanceProcAddr(s_vk.instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
        if (mon && caps2) {
            VkSurfaceFullScreenExclusiveWin32InfoEXT w32 = {
                .sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT, .hmonitor = mon };
            VkPhysicalDeviceSurfaceInfo2KHR si = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, .pNext = &w32,
                .surface = s_vk.surface };
            VkSurfaceCapabilitiesFullScreenExclusiveEXT fc = {
                .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_FULL_SCREEN_EXCLUSIVE_EXT };
            VkSurfaceCapabilities2KHR c2 = {
                .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, .pNext = &fc };
            if (caps2(s_vk.phys, &si, &c2) == VK_SUCCESS) {
                caps = c2.surfaceCapabilities;
                fse_supported = fc.fullScreenExclusiveSupported ? 1 : 0;
                have_caps = 1;
            }
        } else if (!mon) {
            VK_LOG("swapchain: no monitor for the window; exclusive fullscreen not requested\n");
        }
    }
#endif
    if (!have_caps)
        VK_CHECK(pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_vk.phys, s_vk.surface, &caps),
                 "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        VK_LOG("swapchain images cannot be blit destinations\n"); return -1;
    }

    u32 nf = 0;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, NULL),
             "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (nf == 0) { VK_LOG("surface reports no formats\n"); return -1; }
    if (nf > 64) nf = 64;
    VkSurfaceFormatKHR fmts[64];
    VK_CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, fmts),
             "vkGetPhysicalDeviceSurfaceFormatsKHR");
    /* UNORM, not SRGB: the offscreen target holds the guest's values as-is. */
    VkSurfaceFormatKHR fmt = fmts[0];
    if (nf == 1 && fmts[0].format == VK_FORMAT_UNDEFINED) {
        fmt.format = VK_FORMAT_B8G8R8A8_UNORM;
    } else {
        static const VkFormat pref[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
        int found = 0;
        for (size_t k = 0; k < 2 && !found; k++)
            for (u32 i = 0; i < nf; i++)
                if (fmts[i].format == pref[k]) { fmt = fmts[i]; found = 1; break; }
    }
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, fmt.format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        VK_LOG("swapchain format %d cannot be a blit destination\n", (int)fmt.format); return -1;
    }

    /* RPCS3 swapchain_WSI::init: the surface's current extent when it has
     * one, else the window's client size raised to the minimum extent (the
     * maximum clamp is ours: RPCS3 refuses to create a swapchain then). */
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        int w = 0, hh = 0;
#ifdef _WIN32
        RECT rc;
        if (GetClientRect((HWND)s_vk.window, &rc)) { w = rc.right - rc.left; hh = rc.bottom - rc.top; }
#else
        SDL_Vulkan_GetDrawableSize(s_vk.window, &w, &hh);
#endif
        if (w <= 0 || hh <= 0) { s_vk.sc_stale = 1; return 1; }
        ext.width = (u32)w; ext.height = (u32)hh;
        if (ext.width  < caps.minImageExtent.width)  ext.width  = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.width  > caps.maxImageExtent.width)  ext.width  = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    /* Minimised: no swapchain can be made now; presents skip until one can. */
    if (!ext.width || !ext.height) { s_vk.sc_stale = 1; return 1; }

    /* RPCS3 swapchain_WSI::init (rsx_vk_pick_present_mode): RSX_VSYNC off
     * prefers IMMEDIATE > MAILBOX > FIFO_RELAXED, "adaptive" MAILBOX >
     * FIFO_RELAXED, on FIFO. */
    const int vsync = rsx_vk_vsync_mode(getenv("RSX_VSYNC"));
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    {
        u32 nm = 0;
        VkPresentModeKHR modes[16];
        uint32_t mv[16];
        if (vsync != RSX_VK_VSYNC_FULL && pvkGetPhysicalDeviceSurfacePresentModesKHR &&
            pvkGetPhysicalDeviceSurfacePresentModesKHR(s_vk.phys, s_vk.surface, &nm, NULL) >= 0) {
            if (nm > 16) nm = 16;
            if (pvkGetPhysicalDeviceSurfacePresentModesKHR(s_vk.phys, s_vk.surface, &nm, modes) < 0) nm = 0;
            for (u32 i = 0; i < nm; i++) mv[i] = (uint32_t)modes[i];
            mode = (VkPresentModeKHR)rsx_vk_pick_present_mode(vsync, mv, nm);
        }
    }

    u32 count = rsx_vk_swap_image_count(caps.minImageCount, caps.maxImageCount);
    if (count > 8) count = 8;
    if (count < caps.minImageCount) count = caps.minImageCount;

    VkSurfaceTransformFlagBitsKHR xform = caps.currentTransform;
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
        xform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    /* RPCS3 always asks for OPAQUE; a surface without it (never seen on
     * Win32) takes its first supported mode instead of failing. */
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha) && caps.supportedCompositeAlpha)
        alpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & (0u - caps.supportedCompositeAlpha));

    VkSwapchainKHR old = s_vk.swapchain;
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s_vk.surface, .minImageCount = count,
        .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
        .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = xform, .compositeAlpha = alpha,
        .presentMode = mode,
        .clipped = VK_TRUE, .oldSwapchain = old,
    };
#ifdef _WIN32
    /* RPCS3: ALLOWED for RSX_DISPLAY=exclusive, DISALLOWED for any other
     * value (borderless / windowed keep the compositor), nothing when the
     * monitor cannot do it or RSX_DISPLAY is unset. */
    VkSurfaceFullScreenExclusiveInfoEXT fsi = {
        .sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT,
        .fullScreenExclusive = excl == RSX_VK_EXCL_ENABLE ? VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT
                                                          : VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT };
    if (fse_supported) sci.pNext = &fsi;
#else
    (void)fse_supported; (void)excl;
#endif
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = pvkCreateSwapchainKHR(s_vk.device, &sci, NULL, &sc);
    if (r != VK_SUCCESS && sci.pNext) {
        VK_LOG("vkCreateSwapchainKHR with a fullscreen-exclusive request failed (VkResult %d); retrying without\n", (int)r);
        sci.pNext = NULL;
        r = pvkCreateSwapchainKHR(s_vk.device, &sci, NULL, &sc);
    }
    if (old) pvkDestroySwapchainKHR(s_vk.device, old, NULL);
    s_vk.swapchain = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) { VK_LOG("vkCreateSwapchainKHR failed (VkResult %d)\n", (int)r); return -1; }
    s_vk.swapchain = sc;

    u32 n = 0;
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, NULL), "vkGetSwapchainImagesKHR");
    if (n == 0 || n > 8) { VK_LOG("unsupported swapchain image count %u\n", n); return -1; }
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, s_vk.sc_images), "vkGetSwapchainImagesKHR");

    for (u32 i = 0; i < 8; i++)
        if (s_vk.sc_done[i]) {
            pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
            s_vk.sc_done[i] = VK_NULL_HANDLE;
        }
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (u32 i = 0; i < n; i++)
        VK_CHECK(pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sc_done[i]), "vkCreateSemaphore");

    s_vk.sc_extent = ext;
    s_vk.sc_count = n;
    s_vk.sc_stale = 0;
#ifdef _WIN32
    {
        RECT rc;
        if (GetClientRect((HWND)s_vk.window, &rc)) {
            s_sc_client_w = (u32)(rc.right - rc.left);
            s_sc_client_h = (u32)(rc.bottom - rc.top);
        }
    }
#endif
    VK_LOG("window swapchain %ux%u, %u images, format %d, present mode %d, exclusive fullscreen %s\n",
           ext.width, ext.height, n, (int)fmt.format, (int)mode,
           !sci.pNext ? "unspecified" : excl == RSX_VK_EXCL_ENABLE ? "allowed" : "disallowed");
    return 0;
}

/* Rebuild the swapchain (resized, out of date). 0 = usable, 1 = not now
 * (minimised; presents skip), -1 = the window is gone. */
static int vk_swapchain_lost(void)
{
    vk_device_idle();
    const int r = vk_create_swapchain();
    if (r < 0) vk_window_drop("the swapchain could not be recreated");
    return r;
}

/* RPCS3 VKGSRender::flip: a swapchain whose size no longer matches the
 * window's client area is rebuilt before the next acquire, not only when
 * the driver reports it out of date (Win32 often keeps presenting a
 * stretched, mismatched swapchain after a resize or fullscreen toggle). */
static void vk_sc_check_resized(void)
{
#ifdef _WIN32
    RECT rc;
    if (!s_vk.window || !s_vk.swapchain || !GetClientRect((HWND)s_vk.window, &rc)) return;
    const LONG w = rc.right - rc.left, h = rc.bottom - rc.top;
    /* Against the client size the swapchain was made for, so a driver whose
     * currentExtent differs from GetClientRect (DPI virtualisation) does not
     * rebuild on every frame. */
    if (w > 0 && h > 0 && ((u32)w != s_sc_client_w || (u32)h != s_sc_client_h))
        s_vk.sc_stale = 1;
#endif
}

/* Blit the offscreen target into the next swapchain image and present it. */
static void vk_present_window(void)
{
    if (!s_vk.windowed || !s_vk.swapchain) return;
    vk_sc_check_resized();
    if (s_vk.sc_stale && vk_swapchain_lost() != 0) return;

    u32 idx = 0;
    VkResult r = pvkAcquireNextImageKHR(s_vk.device, s_vk.swapchain, UINT64_MAX,
                                        s_vk.sem_acquire, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk_swapchain_lost(); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        VK_LOG("vkAcquireNextImageKHR failed (VkResult %d)\n", (int)r); return;
    }

    /* The present's own command buffer: free, since every caller has waited
     * for its copy (present_fence) first. */
    VkCommandBuffer cb = s_vk.present_cmd;
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS) { VK_LOG("vkResetCommandBuffer (window) failed\n"); return; }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { VK_LOG("vkBeginCommandBuffer (window) failed\n"); return; }
    vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = s_vk.sc_images[idx], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.width, (int32_t)s_vk.height, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 },
                        { (int32_t)s_vk.sc_extent.width, (int32_t)s_vk.sc_extent.height, 1 } },
    };
    pvkCmdBlitImage(cb, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL,
                    s_vk.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blit, VK_FILTER_LINEAR);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS) { VK_LOG("vkEndCommandBuffer failed\n"); return; }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sem_acquire,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &s_vk.sc_done[idx],
    };
    pvkResetFences(s_vk.device, 1, &s_vk.window_fence);
    if (vk_queue_submit(&si, s_vk.window_fence) != VK_SUCCESS) {
        VK_LOG("vkQueueSubmit (window) failed\n"); return;
    }
    pvkWaitForFences(s_vk.device, 1, &s_vk.window_fence, VK_TRUE, UINT64_MAX);

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
        .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
    };
    r = vk_queue_present(&pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk_swapchain_lost();
    else if (r != VK_SUCCESS) VK_LOG("vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
}

static void vk_dump_ppm(const unsigned char* rgba)
{
    FILE* f = fopen(s_vk.dump_path, "wb");
    if (!f) { VK_LOG("cannot open %s for the frame dump\n", s_vk.dump_path); return; }
    fprintf(f, "P6\n%u %u\n255\n", s_vk.width, s_vk.height);
    for (size_t i = 0; i < (size_t)s_vk.width * s_vk.height; i++)
        fwrite(rgba + i * 4, 1, 3, f);
    fclose(f);
}

/* ---------------------------------------------------------------------------
 * Present
 *
 * In two halves. The first records, on the present's own command buffer,
 * whatever puts the frame into the offscreen target (the engine's blit of
 * the flipped surface) and the copy of that target into `readback`, and
 * submits it without waiting. The second waits for it, reads the centre
 * pixel and the frame dump back, and blits the target into the window.
 *
 * The first half reads the frame, so it must not run while the walker is
 * recording; the second touches only the target, `readback` and the
 * present's own objects, none of which the engine path draws into, so a host
 * can run it on a thread of its own beside the walker
 * (rsx_vulkan_backend_present_begin / _finish). On the fixed-function path
 * the walker draws into the target itself, so there the second half always
 * follows the first at once.
 * -------------------------------------------------------------------------*/

/* Wait until the GPU is done with a present's copy. Safe from any thread:
 * present_fence is reset only when a present starts, which never runs
 * alongside the walker. */
static void vk_present_wait_gpu(void)
{
    if (__atomic_load_n(&s_vk.present_inflight, __ATOMIC_ACQUIRE) && s_vk.present_fence)
        pvkWaitForFences(s_vk.device, 1, &s_vk.present_fence, VK_TRUE, UINT64_MAX);
}

/* present_mu held. */
static void vk_present_complete_locked(void)
{
    if (!s_vk.present_inflight) return;
    pvkWaitForFences(s_vk.device, 1, &s_vk.present_fence, VK_TRUE, UINT64_MAX);
    __atomic_store_n(&s_vk.present_inflight, 0, __ATOMIC_RELEASE);

    vk_invalidate(&s_vk.readback);
    const unsigned char* px = (const unsigned char*)s_vk.readback.ptr;
    size_t c = ((size_t)(s_vk.height / 2) * s_vk.width + s_vk.width / 2) * 4;
    s_vk.last_center = ((u32)px[c] << 16) | ((u32)px[c + 1] << 8) | (u32)px[c + 2];
    s_vk.presented++;
    if (s_vk.dump_path && (s_vk.presented % s_vk.dump_every) == 0) vk_dump_ppm(px);
    vk_present_window();
}

/* present_mu held: a previous present finished, the present's command
 * buffer begun and ordered after everything submitted so far. */
static VkCommandBuffer vk_present_record_begin(void)
{
    vk_present_complete_locked();
    VkCommandBuffer cb = s_vk.present_cmd;
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return VK_NULL_HANDLE;
    vk_cmd_after_everything(cb);
    return cb;
}

/* present_mu held: copy the target out, submit, and finish at once unless a
 * host asked for the halves separately on the engine path. */
static void vk_present_record_end(VkCommandBuffer cb)
{
    vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { s_vk.width, s_vk.height, 1 },
    };
    pvkCmdCopyImageToBuffer(cb, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL,
                            s_vk.readback.buf, 1, &region);
    VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s_vk.readback.buf, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &host, 0, NULL);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS) { VK_LOG("vkEndCommandBuffer (present) failed\n"); return; }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cb };
    pvkResetFences(s_vk.device, 1, &s_vk.present_fence);
    if (vk_queue_submit(&si, s_vk.present_fence) != VK_SUCCESS) {
        VK_LOG("vkQueueSubmit (present) failed\n"); return;
    }
    __atomic_store_n(&s_vk.present_inflight, 1, __ATOMIC_RELEASE);
    s_vk.present_order = 1;
    if (!(s_vk.present_split && s_vk.eng_active)) vk_present_complete_locked();
}

static void vk_cb_present(void* ud, u32 buffer_id)
{
    (void)ud; (void)buffer_id;
    if (!s_vk.device) return;
    pthread_mutex_lock(&s_vk.present_mu);
    VkCommandBuffer cb = vk_present_record_begin();
    if (cb) vk_present_record_end(cb);
    pthread_mutex_unlock(&s_vk.present_mu);
    s_g.last_draws = s_g.draws;
    s_g.draws = 0;
}

/* ---------------------------------------------------------------------------
 * Register-file draw engine backend (rsx_draw_engine.h)
 *
 * The path the Metal backend takes: the shared engine walks the register
 * file, owns surfaces, texture caching, vertex compaction and pipeline keys,
 * and drives this backend through rsx_draw_backend. Selected with
 * PS3RECOMP_RSX_ENGINE=dispatch; only one of the two paths is registered at a
 * time, since the FIFO walker feeds both and would record every draw twice.
 *
 * Operations are recorded into the current frame slot's command buffers
 * (s_f: `pre` for writes into objects nothing in `main` uses yet, `main` for
 * everything else, in command order) and submitted at present, or at once
 * when the CPU reads results back (readback, depth_readback). Everything the
 * engine hands over (vertices, indices, constants, texels) is copied into the
 * slot's ring at call time, so its staging is free to reuse on return, which
 * is what submit_and_wait promises. Released objects are destroyed once the
 * slot's fences signal. PS3RECOMP_VK_SYNC=1 submits and waits after every
 * operation instead.
 *
 * Stage E1: colour and depth targets, clears, present and readback.
 * Pipelines, draws and textures are the next stages; until then
 * pipeline_create reports 0, which the engine caches as "cannot build".
 * -------------------------------------------------------------------------*/
#define VK_ENG_MAX_OBJ 2048          /* the engine's texture cache is 1024 */

enum { VK_ENG_FREE = 0, VK_ENG_COLOR, VK_ENG_DEPTH, VK_ENG_TEXTURE, VK_ENG_VIEW };

typedef struct vk_eng_obj {
    u8       kind;
    VkFormat fmt;
    u32      bpp;                    /* bytes per texel, 0 for depth     */
    u32      w, h;
    vk_image im;                     /* a VIEW owns only im.view            */
    u32      block;                  /* bytes per 4x4 block, 0 = not BC     */
    u32      surface, remap, rsx_fmt;/* VIEW: what it is a view of          */
    u8       cube;                   /* TEXTURE: 1 six layers, a CUBE view; 2 a 3D volume */
    u8       snapshot;               /* COLOR: a feedback copy, never a target */
    u8       fresh;                  /* no command in `main` uses it yet    */
    u32      mips;                   /* TEXTURE: levels (an SNORM view's too) */
    u8       dcls;                   /* DEPTH: format class (vk_dcls_*), 0 = the default */
    u32      pw, ph;                 /* image size when scaled (0 = w, h)  */
    float    rs;                     /* RSX_RES_SCALE of the image, 0/1 = none */
    u8       gz16;                   /* DEPTH: the guest zeta is fixed-point Z16 */
    vk_image sv;                     /* COLOR of a native 16/8-bit format: the view it is sampled
                                        through (vk_cv_base_map), owning sv.view only */
} vk_eng_obj;

static vk_eng_obj s_eobj[VK_ENG_MAX_OBJ];

/* Hardware MSAA twins (vk_ems_*, defined with the conversion passes). */
static void vk_ems_ss(u32 handle);
static int  vk_ems_log_left(void);
static u32  vk_ems_state_of(u32 handle);
static void vk_ems_wrote_ss(u32 handle);
static void vk_ems_free(u32 handle);
static int  vk_ems_clear(u32 handle, VkImageAspectFlags aspect, const VkClearValue* v);

/* Defined with the E2 pipeline state further down. */
static void vk_eng_drop_framebuffers_for(u32 handle);
static void vk_eng2_shutdown(void* user);

static struct {
    u32 rt[RSX_BE_MAX_COLOR_TARGETS], nrt, depth;
} s_eng_bound;

static VkFormat vk_eng_format(rsx_be_format f, u32* bpp)
{
    switch (f) {
    case RSX_BE_FMT_R8:            *bpp = 1;  return VK_FORMAT_R8_UNORM;
    case RSX_BE_FMT_R8G8:          *bpp = 2;  return VK_FORMAT_R8G8_UNORM;
    case RSX_BE_FMT_R8G8B8A8:      *bpp = 4;  return VK_FORMAT_R8G8B8A8_UNORM;
    case RSX_BE_FMT_R16:           *bpp = 2;  return VK_FORMAT_R16_UNORM;
    case RSX_BE_FMT_R16G16:        *bpp = 4;  return VK_FORMAT_R16G16_UNORM;
    case RSX_BE_FMT_R16G16F:       *bpp = 4;  return VK_FORMAT_R16G16_SFLOAT;
    case RSX_BE_FMT_R16G16B16A16F: *bpp = 8;  return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RSX_BE_FMT_R32F:          *bpp = 4;  return VK_FORMAT_R32_SFLOAT;
    case RSX_BE_FMT_R32G32B32A32F: *bpp = 16; return VK_FORMAT_R32G32B32A32_SFLOAT;
    case RSX_BE_FMT_R5G6B5:        *bpp = 2;  return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case RSX_BE_FMT_A1R5G5B5:      *bpp = 2;  return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case RSX_BE_FMT_BC1:           *bpp = 8;  return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;  /* per block */
    case RSX_BE_FMT_BC2:           *bpp = 16; return VK_FORMAT_BC2_UNORM_BLOCK;
    case RSX_BE_FMT_BC3:           *bpp = 16; return VK_FORMAT_BC3_UNORM_BLOCK;
    default:                       *bpp = 0;  return VK_FORMAT_UNDEFINED;
    }
}

/* Colour formats the E2 passes render into: RGBA8 for ordinary and display
 * targets, RGBA16F for SET_SURFACE_FORMAT 0xB HDR targets, R32F / RGBA32F for
 * the 0xD X32 / 0xC W32Z32Y32X32 float targets (RPCS3 VKFormats
 * get_compatible_surface_format). All four are required colour attachment
 * formats in Vulkan 1.0; blending is required only for the first two
 * (vk_eng_rt_blendable). The native 16/8-bit ones (RPCS3 VKGSRender.cpp:43-60:
 * r5g6b5, x1r5g5b5, b8, g8b8) only once vk_eng_color_format_supported let the
 * engine ask for them. */
#define VK_ENG_RT_FORMATS 8
static int vk_eng_rt_index(VkFormat f)
{
    static int no_f32 = -1;   /* RSX_VK_NO_F32RT=1: fp32 targets unsupported (A/B debugging) */
    if (no_f32 < 0) no_f32 = getenv("RSX_VK_NO_F32RT") != NULL;
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:      return 0;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 1;
    case VK_FORMAT_R32_SFLOAT:          return no_f32 ? -1 : 2;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return no_f32 ? -1 : 3;
    case VK_FORMAT_R5G6B5_UNORM_PACK16:   return 4;
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return 5;
    case VK_FORMAT_R8_UNORM:              return 6;
    case VK_FORMAT_R8G8_UNORM:            return 7;
    default:                            return -1;
    }
}

/* RPCS3 keeps the guest's blend enable on fp32 targets; a device that cannot
 * blend the format gets it off instead of an invalid pipeline. */
static int vk_eng_rt_blendable(VkFormat f)
{
    static int cache[VK_ENG_RT_FORMATS];       /* 0 unknown, 1 no, 2 yes */
    const int i = vk_eng_rt_index(f);
    if (i < 0) return 0;
    if (!cache[i]) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, f, &fp);
        cache[i] = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) ? 2 : 1;
    }
    return cache[i] == 2;
}

/* ---- native 16/8-bit colour targets (RPCS3 VKGSRender.cpp:43-60) ----------
 * The engine still reads, writes, seeds and samples them in the RGBA8 target
 * layout: B8 as (B,B,B,B), G8B8 as (B,G,B,G), R5G6B5 as (R,G,B,1), X1R5G5B5
 * as (R,G,B,A) with A the stored bit. The image holds the guest's precision:
 * R8 = B, R8G8 = (B,G) (RPCS3 g8b8 gb_rg / b8 no_alpha swizzles), so the
 * sampled view maps back onto the RGBA8 lanes and the transfers convert. */
static int vk_cv_native(VkFormat f)
{
    return f == VK_FORMAT_R5G6B5_UNORM_PACK16 || f == VK_FORMAT_A1R5G5B5_UNORM_PACK16 ||
           f == VK_FORMAT_R8_UNORM || f == VK_FORMAT_R8G8_UNORM;
}

static VkComponentMapping vk_cv_base_map(VkFormat f)
{
    const VkComponentSwizzle R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G,
                             B = VK_COMPONENT_SWIZZLE_B, A = VK_COMPONENT_SWIZZLE_A;
    switch (f) {
    case VK_FORMAT_R8_UNORM:            return (VkComponentMapping){ R, R, R, R };
    case VK_FORMAT_R8G8_UNORM:          return (VkComponentMapping){ R, G, R, G };
    case VK_FORMAT_R5G6B5_UNORM_PACK16: return (VkComponentMapping){ R, G, B, VK_COMPONENT_SWIZZLE_ONE };
    default:                            return (VkComponentMapping){ R, G, B, A };
    }
}

/* m (a mapping over the RGBA8 lanes) applied on top of the base map. */
static VkComponentMapping vk_cv_compose(VkComponentMapping m, VkFormat f)
{
    const VkComponentMapping b = vk_cv_base_map(f);
    const VkComponentSwizzle bl[4] = { b.r, b.g, b.b, b.a };
    VkComponentSwizzle o[4] = { m.r, m.g, m.b, m.a };
    for (u32 c = 0; c < 4u; c++) {
        if (o[c] == VK_COMPONENT_SWIZZLE_IDENTITY) o[c] = (VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_R + c);
        if (o[c] >= VK_COMPONENT_SWIZZLE_R && o[c] <= VK_COMPONENT_SWIZZLE_A)
            o[c] = bl[o[c] - VK_COMPONENT_SWIZZLE_R];
    }
    return (VkComponentMapping){ o[0], o[1], o[2], o[3] };
}

static u8 vk_cv_ex(u32 v, u32 max) { return (u8)((v * 255u + max / 2u) / max); }
static u32 vk_cv_q(u32 v8, u32 max) { return (v8 * max + 127u) / 255u; }

/* n texels of native format f -> RGBA8 lanes. */
static void vk_cv_to_rgba(VkFormat f, const u8* s, u8* d, u32 n)
{
    for (u32 i = 0; i < n; i++, d += 4) {
        switch (f) {
        case VK_FORMAT_R8_UNORM: d[0] = d[1] = d[2] = d[3] = s[i]; break;
        case VK_FORMAT_R8G8_UNORM:
            d[0] = d[2] = s[i * 2u]; d[1] = d[3] = s[i * 2u + 1u]; break;
        case VK_FORMAT_R5G6B5_UNORM_PACK16: {
            const u32 v = (u32)s[i * 2u] | ((u32)s[i * 2u + 1u] << 8);
            d[0] = vk_cv_ex(v >> 11, 31u); d[1] = vk_cv_ex((v >> 5) & 63u, 63u);
            d[2] = vk_cv_ex(v & 31u, 31u); d[3] = 255u;
            break;
        }
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16: {
            const u32 v = (u32)s[i * 2u] | ((u32)s[i * 2u + 1u] << 8);
            d[0] = vk_cv_ex((v >> 10) & 31u, 31u); d[1] = vk_cv_ex((v >> 5) & 31u, 31u);
            d[2] = vk_cv_ex(v & 31u, 31u); d[3] = (v & 0x8000u) ? 255u : 0u;
            break;
        }
        default: memcpy(d, s + (size_t)i * 4u, 4); break;
        }
    }
}

/* n RGBA8-lane texels -> native format f. */
static void vk_cv_from_rgba(VkFormat f, const u8* s, u8* d, u32 n)
{
    for (u32 i = 0; i < n; i++, s += 4) {
        u32 v;
        switch (f) {
        case VK_FORMAT_R8_UNORM: d[i] = s[2]; continue;
        case VK_FORMAT_R8G8_UNORM: d[i * 2u] = s[2]; d[i * 2u + 1u] = s[1]; continue;
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
            v = (vk_cv_q(s[0], 31u) << 11) | (vk_cv_q(s[1], 63u) << 5) | vk_cv_q(s[2], 31u);
            break;
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
            v = (s[3] >= 128u ? 0x8000u : 0u) | (vk_cv_q(s[0], 31u) << 10) |
                (vk_cv_q(s[1], 31u) << 5) | vk_cv_q(s[2], 31u);
            break;
        default: memcpy(d + (size_t)i * 4u, s, 4); continue;
        }
        d[i * 2u] = (u8)v; d[i * 2u + 1u] = (u8)(v >> 8);
    }
}

static int vk_cv_blit_ok(VkFormat f)
{
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R5G6B5_UNORM_PACK16 ||
           f == VK_FORMAT_A1R5G5B5_UNORM_PACK16;
}

/* An RGBA8-lane clear colour in the image's channels: B8 in R, G8B8 as (B,G)
 * (RPCS3 color_utils.hpp get_b8_clear_color / get_g8b8_clear_color). */
static void vk_cv_clear_value(VkFormat f, const float in[4], float out[4])
{
    memcpy(out, in, 4u * sizeof(float));
    if (f == VK_FORMAT_R8_UNORM) out[0] = in[2];
    else if (f == VK_FORMAT_R8G8_UNORM) { out[0] = in[2]; out[1] = in[1]; }
}

/* The colour write mask (bit0 R .. bit3 A over the RGBA8 lanes) in the
 * image's channels (RPCS3 get_b8_colormask / get_g8b8_r8g8_colormask); the
 * channels the format has not got are set once any is written, so a full
 * mask stays full. */
static u32 vk_cv_write_mask(VkFormat f, u32 m)
{
    u32 o = m;
    if (f == VK_FORMAT_R8_UNORM) o = ((m >> 2) & 1u) ? 0xFu : 0u;
    else if (f == VK_FORMAT_R8G8_UNORM) { o = ((m >> 2) & 1u) | (m & 2u); if (o) o |= 0xCu; }
    else if (f == VK_FORMAT_R5G6B5_UNORM_PACK16 && (m & 7u)) o = m | 8u;
    return o;
}

/* RGBA8-lane rows as tightly packed native rows (malloc'd), or NULL. */
static u8* vk_cv_rows_in(VkFormat f, u32 bpp, const void* rows, u32 row_bytes, u32 w, u32 h)
{
    u8* p = (u8*)malloc((size_t)w * bpp * h);
    if (!p) return NULL;
    for (u32 r = 0; r < h; r++)
        vk_cv_from_rgba(f, (const u8*)rows + (size_t)r * row_bytes, p + (size_t)r * w * bpp, w);
    return p;
}

/* rsx_draw_backend.color_format_supported: the format renders, blends,
 * blits both ways and samples (A1R5G5B5 is optional in Vulkan); else the
 * engine keeps RGBA8, like RPCS3's Apple branch (VKGSRender.cpp:59-68). */
static int vk_eng_color_format_supported(void* user, rsx_be_format fmt)
{
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    if (!s_vk.phys || !vk_cv_native(vf)) return 0;
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, vf, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    return (fp.optimalTilingFeatures & need) == need;
}

/* The largest image side the engine creates: the device's own limit once a
 * device is picked, the 4096 Vulkan guarantees before. */
static u32 vk_max_dim(void) { return s_vk.max_dim ? s_vk.max_dim : 4096u; }

static u32 vk_eng_alloc(void)
{
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
        if (s_eobj[i].kind == VK_ENG_FREE) return i;
    VK_LOG("engine: object table full (%d)\n", VK_ENG_MAX_OBJ);
    return 0;
}

static vk_eng_obj* vk_eng_get(u32 handle, int kind)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return NULL;
    vk_eng_obj* o = &s_eobj[handle];
    return (o->kind == kind) ? o : NULL;
}

/* ---- internal resolution scale (RSX_RES_SCALE) ----------------------------
 * RPCS3's surface_scaling_config_t / apply_resolution_scale: a colour or
 * depth target whose larger side exceeds RSX_RES_SCALE_MIN guest pixels is
 * created at guest size x scale. o->w/h stay guest sizes (the engine's
 * bookkeeping); every guest rectangle is mapped onto the image here, as the
 * D3D12 backend does. Textures never scale. RSX_VK_RES_SCALE=0 turns it off. */
static struct { float s; u32 min; int init, dyn; } s_rs;

static float vk_rs_global(void)
{
    if (!s_rs.init) {
        s_rs.init = 1;
        s_rs.s = 1.0f;
        s_rs.min = 16u;
        const char* e = getenv("RSX_RES_SCALE");
        const char* k = getenv("RSX_VK_RES_SCALE");
        const char* m = getenv("RSX_RES_SCALE_MIN");
        if (e && *e && !(k && k[0] == '0' && !k[1])) {
            double v = atof(e);
            if (!(v >= 1.0)) v = 1.0;
            if (v > 4.0) v = 4.0;
            s_rs.s = (float)v;
        }
        if (m && *m) s_rs.min = (u32)strtoul(m, NULL, 10);
        if (s_rs.s > 1.0f)
            VK_LOG("engine: internal resolution x%.3g (targets larger than %u px)\n",
                   (double)s_rs.s, s_rs.min);
    }
    return s_rs.s;
}
static float vk_rs_of(const vk_eng_obj* o) { return o->rs > 1.0f ? o->rs : 1.0f; }
static u32 vk_pw(const vk_eng_obj* o) { return o->pw ? o->pw : o->w; }
static u32 vk_ph(const vk_eng_obj* o) { return o->ph ? o->ph : o->h; }
static u32 vk_rs_dim(u32 v, float rs) { return (u32)((double)v * rs + 0.5); }
/* A guest coordinate on the image, clamped to lim. */
static u32 vk_rs_c(const vk_eng_obj* o, u32 v, u32 lim)
{
    if (!(o->rs > 1.0f)) return v;
    const u32 p = vk_rs_dim(v, o->rs);
    return p < lim ? p : lim;
}
/* A guest rectangle on the image (endpoints mapped, so neighbours tile);
 * never empty when it was not. */
static void vk_rs_rect(const vk_eng_obj* o, u32* x, u32* y, u32* w, u32* h)
{
    if (!(o->rs > 1.0f)) return;
    const u32 W = vk_pw(o), H = vk_ph(o);
    u32 x0 = vk_rs_c(o, *x, W), x1 = vk_rs_c(o, *x + *w, W);
    u32 y0 = vk_rs_c(o, *y, H), y1 = vk_rs_c(o, *y + *h, H);
    if (*w && x1 <= x0) { if (x0 >= W) x0 = W - 1u; x1 = x0 + 1u; }
    if (*h && y1 <= y0) { if (y0 >= H) y0 = H - 1u; y1 = y0 + 1u; }
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}
/* The scale a w x h target is created at: 1 at or under the minimum. */
static float vk_rs_pick(u32 w, u32 h)
{
    const float s = vk_rs_global();
    if (!(s > 1.0f) || (w > h ? w : h) <= s_rs.min) return 1.0f;
    if (vk_rs_dim(w, s) > vk_max_dim() || vk_rs_dim(h, s) > vk_max_dim()) return 1.0f;
    return s;
}
/* Nearest source index of output i of n over pn image texels. */
static u32 vk_rs_src(u32 i, u32 n, u32 pn)
{
    const u32 v = (u32)(((u64)i * 2u + 1u) * pn / (2u * (u64)n));
    return v < pn ? v : pn - 1u;
}
/* Every attachment of a draw / clear at one scale (defined with the clears). */
static void vk_rs_match(const u32* rt, u32 n, u32 depth);
/* RSX_VK_BIAS_SLOPE_RS=1: the depth-bias slope factor times the scale
 * (rsx_vk_depth_bias_slope); off, as in RPCS3. */
static int vk_bias_slope_rs(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_VK_BIAS_SLOPE_RS"); v = (e && e[0] == '1') ? 1 : 0; }
    return v;
}

/* Which command buffer a write into `o` goes to: `pre` while no command in
 * `main` has used it yet, so creating and filling it does not split a pass. */
static vk_rec* vk_eng_rec(const vk_eng_obj* o) { return o->fresh ? &s_f.pre : &s_f.main; }

/* `main` now references the object (and, for a view, its surface). */
static void vk_eng_touch(u32 handle)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return;
    vk_eng_obj* o = &s_eobj[handle];
    if (o->kind == VK_ENG_VIEW && o->surface && o->surface < VK_ENG_MAX_OBJ)
        s_eobj[o->surface].fresh = 0;
    o->fresh = 0;
}

/* A new image, UNDEFINED -> GENERAL in `pre`: nothing has used it, so it
 * waits on nothing, and every later command (in `pre` or `main`) is after it. */
static int vk_eng_new_layout(VkImage img, VkImageAspectFlags aspect)
{
    VkCommandBuffer cb = vk_rec_get(&s_f.pre);
    if (!cb) return -1;
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS },
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
    return 0;
}

/* Copy `rows` rows of `w` texels from host memory into a w x h rectangle at
 * (x, y) of one level / layer of the object's image (in GENERAL): staged in
 * the frame ring, the copy recorded in command order. */
static int vk_eng_upload_region(vk_eng_obj* o, u32 layer, u32 mip, u32 x, u32 y,
                                u32 w, u32 h, size_t tight, u32 rows,
                                const void* src, u32 row_bytes)
{
    if (tight > row_bytes) tight = row_bytes;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    u8* p = (u8*)vk_ring((VkDeviceSize)tight * rows, 16, &buf, &off);
    if (!p) return -1;
    for (u32 r = 0; r < rows; r++)
        memcpy(p + (size_t)r * tight, (const u8*)src + (size_t)r * row_bytes, tight);
    VkCommandBuffer cb = vk_rec_upload(vk_eng_rec(o), o->im.img);
    if (!cb) return -1;
    VkBufferImageCopy region = {
        .bufferOffset = off,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },
        .imageOffset = { (int32_t)x, (int32_t)y, 0 },
        .imageExtent = { w, h, 1 },
    };
    pvkCmdCopyBufferToImage(cb, buf, o->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    return 0;
}

/* The same into the whole of a level, from its origin. */
static int vk_eng_upload_rows(vk_eng_obj* o, u32 layer, u32 mip, u32 w, u32 h,
                              size_t tight, u32 rows, const void* src, u32 row_bytes)
{
    return vk_eng_upload_region(o, layer, mip, 0, 0, w, h, tight, rows, src, row_bytes);
}

static int vk_eng_upload(vk_eng_obj* o, u32 layer, u32 mip, u32 w, u32 h, u32 bpp,
                         const void* src, u32 row_bytes)
{
    return vk_eng_upload_rows(o, layer, mip, w, h, (size_t)w * bpp, h, src, row_bytes);
}

/* Host rows for a guest rectangle of a scaled colour target: copied at guest
 * size into a staging image, then blitted (nearest) over the image's
 * rectangle, in `main` (the staging images are shared). */
static struct { vk_image im; VkFormat fmt; u32 w, h; } s_rs_stage[4];

/* A guest-size staging image of format fmt holding at least w x h (one per
 * format, grown on demand), or -1. */
static int vk_rs_stage_get(VkFormat fmt, u32 w, u32 h)
{
    u32 si = 0;
    for (u32 i = 0; i < 4u; i++) {
        if (s_rs_stage[i].im.img && s_rs_stage[i].fmt == fmt) { si = i; break; }
        if (!s_rs_stage[i].im.img) si = i;
    }
    if (s_rs_stage[si].im.img && s_rs_stage[si].fmt != fmt) si = 0;
    if (!s_rs_stage[si].im.img || s_rs_stage[si].fmt != fmt ||
        s_rs_stage[si].w < w || s_rs_stage[si].h < h) {
        const int same = s_rs_stage[si].im.img && s_rs_stage[si].fmt == fmt;
        const u32 nw = same && s_rs_stage[si].w > w ? s_rs_stage[si].w : w;
        const u32 nh = same && s_rs_stage[si].h > h ? s_rs_stage[si].h : h;
        if (s_rs_stage[si].im.img) vk_defer_image(&s_rs_stage[si].im);
        memset(&s_rs_stage[si], 0, sizeof s_rs_stage[si]);
        if (vk_create_image(&s_rs_stage[si].im, fmt, nw, nh,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT) ||
            vk_eng_new_layout(s_rs_stage[si].im.img, VK_IMAGE_ASPECT_COLOR_BIT)) {
            vk_destroy_image(&s_rs_stage[si].im);
            memset(&s_rs_stage[si], 0, sizeof s_rs_stage[si]);
            return -1;
        }
        s_rs_stage[si].fmt = fmt; s_rs_stage[si].w = nw; s_rs_stage[si].h = nh;
    }
    return (int)si;
}

static int vk_rs_upload(vk_eng_obj* o, u32 x, u32 y, u32 w, u32 h,
                        const void* src, u32 row_bytes)
{
    const size_t tight = (size_t)w * o->bpp;
    if (!w || !h || !o->bpp || tight > row_bytes) return -1;
    const int sii = vk_rs_stage_get(o->fmt, w, h);
    if (sii < 0) return -1;
    const u32 si = (u32)sii;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    u8* p = (u8*)vk_ring((VkDeviceSize)tight * h, 16, &buf, &off);
    if (!p) return -1;
    for (u32 r = 0; r < h; r++)
        memcpy(p + (size_t)r * tight, (const u8*)src + (size_t)r * row_bytes, tight);
    o->fresh = 0;
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (!cb) return -1;
    const VkImage st = s_rs_stage[si].im.img;
    VkBufferImageCopy region = {
        .bufferOffset = off,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { w, h, 1 },
    };
    pvkCmdCopyBufferToImage(cb, buf, st, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    vk_rec_barrier(&s_f.main);
    s_f.main.other = 1;
    u32 dx = x, dy = y, dw = w, dh = h;
    vk_rs_rect(o, &dx, &dy, &dw, &dh);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { (int32_t)dx, (int32_t)dy, 0 },
                        { (int32_t)(dx + dw), (int32_t)(dy + dh), 1 } },
    };
    pvkCmdBlitImage(cb, st, VK_IMAGE_LAYOUT_GENERAL, o->im.img, VK_IMAGE_LAYOUT_GENERAL,
                    1, &blit, VK_FILTER_NEAREST);
    return 0;
}

/* The CPU-visible buffer readbacks land in, grown on demand. Every readback
 * waits for its copy, so it is never in use when this runs. */
static int vk_eng_readback_buffer(VkDeviceSize bytes)
{
    if (s_f.rb.buf && s_f.rb.size >= bytes) return 0;
    VkDeviceSize sz = s_f.rb.size ? s_f.rb.size : (VkDeviceSize)1u << 20;
    while (sz < bytes) sz *= 2u;
    vk_destroy_buffer(&s_f.rb);
    if (vk_create_host_buffer(&s_f.rb, sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        vk_destroy_buffer(&s_f.rb);
        return -1;
    }
    return 0;
}

/* Record "copy into s_f.rb, visible to the host", submit and wait. */
static int vk_eng_readback_finish(VkCommandBuffer cb)
{
    VkBufferMemoryBarrier hb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s_f.rb.buf, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &hb, 0, NULL);
    /* Wait on this submission's fence alone: the copy is ordered after all
     * earlier queue work by vk_rec_xfer's barrier, so it is enough, and
     * nothing is retired -- descriptor sets, constants and the ring stay
     * warm (vk_flush(1) drained every slot and bumped the epoch). */
    vk_flush(0);
    {
        vk_frame* f = &s_f.fr[s_f.cur];
        if (f->nsub)
            pvkWaitForFences(s_vk.device, 1, &f->fence[f->nsub - 1u], VK_TRUE, UINT64_MAX);
    }
    vk_invalidate(&s_f.rb);
    return 0;
}

/* NV4097 TEXTURE_CONTROL1 crossbar as a Vulkan component mapping: the shared
 * rsx_texture_component_remap() gives, in A,R,G,B order, which uploaded
 * channel (0..3 = R,G,B,A) or constant each output takes -- the table the
 * Metal backend's swizzle_sel reads. */
static VkComponentSwizzle vk_remap_sel(u8 sel)
{
    switch (sel) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case RSX_REMAP_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    default: return VK_COMPONENT_SWIZZLE_ZERO;
    }
}
static VkComponentMapping vk_remap(u32 remap, u32 rsx_fmt)
{
    u8 sel[4];
    rsx_texture_component_remap(remap, rsx_fmt & 0x9Fu, sel);
    VkComponentMapping m = { vk_remap_sel(sel[1]), vk_remap_sel(sel[2]),
                             vk_remap_sel(sel[3]), vk_remap_sel(sel[0]) };
    return m;
}
static int vk_eng_make_view(VkImage img, VkFormat fmt, u32 levels, VkComponentMapping map,
                            int cube, VkImageView* out)
{
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = cube == 2 ? VK_IMAGE_VIEW_TYPE_3D : cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt, .components = map,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels ? levels : 1, 0, cube == 1 ? 6u : 1u },
    };
    return pvkCreateImageView(s_vk.device, &vci, NULL, out) == VK_SUCCESS ? 0 : -1;
}

static int vk_eng_init(void* user, u32 width, u32 height)
{
    (void)user; (void)width; (void)height;
    memset(s_eobj, 0, sizeof s_eobj);
    memset(&s_eng_bound, 0, sizeof s_eng_bound);
    return s_vk.device ? 0 : -1;
}

static void vk_eng_snapshot_recycle(vk_eng_obj* o);
static void vk_eng_drop_framebuffers_for(u32 handle);

/* Never destroys at once: the image goes on the frame's deferred list, since
 * commands recorded or in flight may still use it. */
static void vk_eng_release(void* user, u32 handle)
{
    (void)user;
    if (!handle || handle >= VK_ENG_MAX_OBJ || s_eobj[handle].kind == VK_ENG_FREE) return;
    if (s_eobj[handle].kind == VK_ENG_COLOR || s_eobj[handle].kind == VK_ENG_TEXTURE)
        for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
            if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == handle) {
                vk_defer_image(&s_eobj[i].im);
                memset(&s_eobj[i], 0, sizeof s_eobj[i]);
            }
    if (s_eobj[handle].sv.view) vk_defer_image(&s_eobj[handle].sv);
    if (s_eobj[handle].kind == VK_ENG_DEPTH && s_eobj[handle].surface) {
        const u32 snap = s_eobj[handle].surface;
        s_eobj[handle].surface = 0;
        if (vk_eng_get(snap, VK_ENG_TEXTURE)) vk_eng_release(user, snap);
    }
    if (s_eobj[handle].kind == VK_ENG_COLOR && s_eobj[handle].snapshot) {
        /* Never an attachment, so no framebuffer names it; the image goes
         * back to the pool for the next feedback draw (its next copy is
         * recorded after every command that used it). */
        vk_eng_snapshot_recycle(&s_eobj[handle]);
        memset(&s_eobj[handle], 0, sizeof s_eobj[handle]);
        return;
    }
    if (s_eobj[handle].kind == VK_ENG_COLOR || s_eobj[handle].kind == VK_ENG_DEPTH)
        vk_ems_free(handle);
    if (s_eobj[handle].kind == VK_ENG_COLOR || s_eobj[handle].kind == VK_ENG_DEPTH)
        vk_eng_drop_framebuffers_for(handle);
    vk_defer_image(&s_eobj[handle].im);
    memset(&s_eobj[handle], 0, sizeof s_eobj[handle]);
}

static void vk_eng_snapshot_pool_drain(void);

/* RPCS3 keeps compiled pipelines on disk (its pipeline storage); Vulkan's
 * pipeline cache does the same for the driver's compile: loaded at device
 * creation, written back at shutdown. RSX_VK_PCACHE=0 disables it. */
static const char* vk_pcache_path(void)
{
    const char* e = getenv("RSX_VK_PCACHE_PATH");
    return (e && *e) ? e : "shadercache/vk_pipeline_cache.bin";
}

static void vk_pcache_load(void)
{
    const char* off = getenv("RSX_VK_PCACHE");
    if (off && *off == '0') return;
    void* data = NULL; size_t n = 0;
    FILE* f = fopen(vk_pcache_path(), "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        if (len > 0 && (data = malloc((size_t)len)) && fread(data, 1, (size_t)len, f) == (size_t)len)
            n = (size_t)len;
        fclose(f);
    }
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                     .initialDataSize = n, .pInitialData = n ? data : NULL };
    /* The driver validates the header and ignores a stale blob. */
    if (pvkCreatePipelineCache(s_vk.device, &ci, NULL, &s_vk.pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0; ci.pInitialData = NULL;
        if (pvkCreatePipelineCache(s_vk.device, &ci, NULL, &s_vk.pcache) != VK_SUCCESS)
            s_vk.pcache = VK_NULL_HANDLE;
    }
    free(data);
    VK_LOG("pipeline cache: %zu bytes loaded from %s\n", n, vk_pcache_path());
}

static void vk_pcache_save(void)
{
    if (!s_vk.pcache) return;
    size_t n = 0;
    if (pvkGetPipelineCacheData(s_vk.device, s_vk.pcache, &n, NULL) == VK_SUCCESS && n) {
        void* data = malloc(n);
        if (data && pvkGetPipelineCacheData(s_vk.device, s_vk.pcache, &n, data) == VK_SUCCESS) {
            char tmp[512];
            snprintf(tmp, sizeof tmp, "%s.tmp", vk_pcache_path());
            FILE* f = fopen(tmp, "wb");
            if (f) {
                const int ok = fwrite(data, 1, n, f) == n;
                fclose(f);
                if (ok) { remove(vk_pcache_path()); rename(tmp, vk_pcache_path()); }
            }
        }
        free(data);
    }
}

static void vk_async_stop(void);
static void vk_mod_release_all(void);
static void vk_vfile_close(void);

static void vk_eng_shutdown(void* user)
{
    vk_async_stop();                 /* no pipeline worker touches the cache or a slot now */
    vk_flush(1);
    vk_pcache_save();
    vk_eng2_shutdown(user);
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++) vk_eng_release(user, i);
    vk_eng_snapshot_pool_drain();
    vk_mod_release_all();
    vk_vfile_close();
    vk_flush(1);                     /* and destroy everything deferred */
}

/* The engine calls this before it reads guest memory the GPU may be about to
 * write (GUEST_REFERENCE; a readback always follows, and waits itself), when
 * it recycles its vertex staging (VERTEX_RING) and at shutdown. Everything it
 * hands over is copied into the frame ring at call time, so its staging is
 * free on return either way; submitting gets the GPU going early. */
static void vk_eng_submit_and_wait(void* user, u32 reason)
{
    (void)user;
    vk_flush(reason == RSX_BE_FLUSH_GUEST_REFERENCE ? 0 : 1);
}

static u32 vk_eng_color_target_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                      const void* seed, u32 seed_row_bytes)
{
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    /* Only formats the E2 passes are built for (vk_eng_rt_index). */
    if (vk_eng_rt_index(vf) < 0 || !w || !h || w > vk_max_dim() || h > vk_max_dim()) {
        VK_LOG("engine: colour target format %d %ux%u not supported yet\n", (int)fmt, w, h);
        return 0;
    }
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_op_begin();
    vk_eng_obj* o = &s_eobj[hd];
    const float rs = vk_rs_pick(w, h);
    const u32 pw = rs > 1.0f ? vk_rs_dim(w, rs) : w, ph = rs > 1.0f ? vk_rs_dim(h, rs) : h;
    if (vk_create_image(&o->im, vf, pw, ph,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&o->im); memset(o, 0, sizeof *o); return 0;
    }
    if (vk_cv_native(vf) &&
        vk_eng_make_view(o->im.img, vf, 1, vk_cv_base_map(vf), 0, &o->sv.view)) {
        vk_destroy_image(&o->im); memset(o, 0, sizeof *o); return 0;
    }
    o->kind = VK_ENG_COLOR; o->fmt = vf; o->bpp = bpp; o->w = w; o->h = h; o->fresh = 1;
    o->pw = pw; o->ph = ph; o->rs = rs;
    /* UNDEFINED -> GENERAL, then the guest's own bytes when the engine could
     * resolve them, else RPCS3 render_target::clear_memory's opaque black
     * (0,0,0,1) -- what a new erase_bkgnd surface starts as with "Read Color
     * Buffers" off. */
    if (vk_eng_new_layout(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT)) { vk_eng_release(user, hd); return 0; }
    VkCommandBuffer cb = vk_rec_upload(&s_f.pre, o->im.img);
    if (!cb) { vk_eng_release(user, hd); return 0; }
    VkClearColorValue zero = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    pvkCmdClearColorImage(cb, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
    if (seed && seed_row_bytes) {
        /* The engine's seed rows are RGBA8 lanes. */
        u8* cv = vk_cv_native(vf) ? vk_cv_rows_in(vf, bpp, seed, seed_row_bytes, w, h) : NULL;
        if (cv) { seed = cv; seed_row_bytes = w * bpp; }
        if (cv || !vk_cv_native(vf)) {
            if (rs > 1.0f) vk_rs_upload(o, 0, 0, w, h, seed, seed_row_bytes);
            else vk_eng_upload(o, 0, 0, w, h, bpp, seed, seed_row_bytes);
        }
        free(cv);
    }
    vk_op_done();
    return hd;
}

static VkImageUsageFlags vk_aa_depth_usage(void);   /* temporal upscaler */

/* ---- zeta formats (RPCS3 VKFormats get_compatible_depth_surface_format) ----
 * Z16 renders into D16_UNORM, Z24S8 into D24_UNORM_S8_UINT (D32_SFLOAT_S8_UINT
 * where the device has no D24S8), and with CONTROL0's depth_float into
 * D32_SFLOAT / D32_SFLOAT_S8_UINT ("Force High Precision Z buffer" off). The
 * engine passes are built per format class: class 0 is the Z24S8 fixed one
 * (and the scratch depth's), so the common case keeps the async pipelines.
 * RSX_VK_DEPTH_LEGACY=1 puts every target back on the device default
 * (D32_SFLOAT_S8_UINT first), as before. */
#define VK_DCLS 4
static VkFormat s_dcls_fmt[VK_DCLS];
static u8       s_dcls_sampled[VK_DCLS];
static int      s_dcls_ready, s_dcls_legacy;

static VkImageAspectFlags vk_depth_aspect_of(VkFormat f)
{
    return (f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT ||
            f == VK_FORMAT_D16_UNORM_S8_UINT)
         ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
}

static VkFormatFeatureFlags vk_depth_features(VkFormat f)
{
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, f, &fp);
    return fp.optimalTilingFeatures;
}

static void vk_dcls_setup(void)
{
    if (s_dcls_ready) return;
    const char* e = getenv("RSX_VK_DEPTH_LEGACY");
    s_dcls_legacy = (e && *e == '1') ? 1 : 0;
    const VkFormat want[VK_DCLS] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM,
                                     VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT };
    for (u32 c = 0; c < VK_DCLS; c++) {
        const VkFormat f = want[c];
        const int ok = !s_dcls_legacy &&
            (vk_depth_features(f) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
        s_dcls_fmt[c] = ok ? f : s_vk.depth_format;
        s_dcls_sampled[c] = (vk_depth_features(s_dcls_fmt[c]) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? 1 : 0;
    }
    s_dcls_ready = 1;
    VK_LOG("engine: zeta formats z16 %d, z24s8 %d, float z16 %d / z24s8 %d%s\n",
           (int)s_dcls_fmt[1], (int)s_dcls_fmt[0], (int)s_dcls_fmt[2], (int)s_dcls_fmt[3],
           s_dcls_legacy ? " (RSX_VK_DEPTH_LEGACY)" : "");
}

/* The class of SURFACE_FORMAT's zeta (1 Z16, 2 Z24S8) and depth_float: the
 * lowest class of the same host format, so equal formats share passes. */
static u32 vk_dcls_for(u32 zfmt, u32 depth_float)
{
    vk_dcls_setup();
    u32 c = 0;
    if (zfmt == 1u) c = depth_float ? 2u : 1u;
    else if (zfmt == 2u) c = depth_float ? 3u : 0u;
    for (u32 k = 0; k < c; k++) if (s_dcls_fmt[k] == s_dcls_fmt[c]) return k;
    return c;
}

static u32 vk_eng_depth_target_create_ex(void* user, u32 w, u32 h, u32 zfmt, u32 depth_float)
{
    (void)user;
    if (!w || !h || w > vk_max_dim() || h > vk_max_dim()) return 0;
    const u32 cls = vk_dcls_for(zfmt, depth_float);
    const VkFormat df = s_dcls_fmt[cls];
    const VkImageAspectFlags da = vk_depth_aspect_of(df);
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_op_begin();
    vk_eng_obj* o = &s_eobj[hd];
    const float rs = vk_rs_pick(w, h);
    const u32 pw = rs > 1.0f ? vk_rs_dim(w, rs) : w, ph = rs > 1.0f ? vk_rs_dim(h, rs) : h;
    /* Sampled as well where the format allows: MSAA resolves read it. */
    if (vk_create_image(&o->im, df, pw, ph,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | vk_aa_depth_usage() |
                        (s_dcls_sampled[cls] ? VK_IMAGE_USAGE_SAMPLED_BIT : 0u) |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        da)) {
        vk_destroy_image(&o->im); memset(o, 0, sizeof *o); return 0;
    }
    o->kind = VK_ENG_DEPTH; o->fmt = df; o->dcls = (u8)cls; o->w = w; o->h = h; o->fresh = 1;
    o->gz16 = (u8)(zfmt == 1u && !depth_float);
    o->pw = pw; o->ph = ph; o->rs = rs;
    /* Far plane, for the reason the display depth starts there. */
    if (vk_eng_new_layout(o->im.img, da)) { vk_eng_release(user, hd); return 0; }
    VkCommandBuffer cb = vk_rec_upload(&s_f.pre, o->im.img);
    if (!cb) { vk_eng_release(user, hd); return 0; }
    /* RPCS3 render_target::clear_memory: depth 1.0, stencil 255. */
    VkClearDepthStencilValue far_plane = { 1.0f, 255 };
    VkImageSubresourceRange r = { da, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(cb, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    vk_op_done();
    return hd;
}

static u32 vk_eng_depth_target_create(void* user, u32 w, u32 h)
{
    return vk_eng_depth_target_create_ex(user, w, h, 2u, 0u);
}

static void vk_eng_bind_targets(void* user, const u32* surfaces, u32 count, u32 depth)
{
    (void)user;
    if (count > RSX_BE_MAX_COLOR_TARGETS) count = RSX_BE_MAX_COLOR_TARGETS;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
        s_eng_bound.rt[i] = (i < count && surfaces) ? surfaces[i] : 0;
    s_eng_bound.nrt = count;
    s_eng_bound.depth = depth;
}

static void vk_eng_clear_color(void* user, u32 surface, const float rgba[4])
{
    (void)user;
    /* The debug hook reports the last colour asked for, as Metal's does. */
    s_vk.clear_argb = ((u32)(rgba[3] * 255.0f + 0.5f) << 24) |
                      ((u32)(rgba[0] * 255.0f + 0.5f) << 16) |
                      ((u32)(rgba[1] * 255.0f + 0.5f) <<  8) |
                       (u32)(rgba[2] * 255.0f + 0.5f);
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o) return;
    vk_op_begin();
    VkClearColorValue v;
    vk_cv_clear_value(o->fmt, rgba, v.float32);
    {   /* A hardware MSAA target clears its twin only. */
        const VkClearValue tv = { .color = v };
        if (vk_ems_clear(surface, VK_IMAGE_ASPECT_COLOR_BIT, &tv)) { vk_op_done(); return; }
    }
    /* An attachment of the open pass covering the whole image: clear it
     * inside the pass (in rasterization order with its draws). */
    if (s_f.pass && !o->fresh && vk_pw(o) == s_f.pass_w && vk_ph(o) == s_f.pass_h)
        for (u32 i = 0; i < s_f.pass_nrt; i++)
            if (s_f.pass_rt[i] == surface && s_f.pass_img[i] == o->im.img) {
                VkClearAttachment ca = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                         .colorAttachment = i, .clearValue = { .color = v } };
                VkClearRect cr = { { { 0, 0 }, { vk_pw(o), vk_ph(o) } }, 0, 1 };
                pvkCmdClearAttachments(s_f.main.cb, 1, &ca, 1, &cr);
                vk_op_done();
                return;
            }
    VkCommandBuffer cb = vk_rec_upload(vk_eng_rec(o), o->im.img);
    if (cb) {
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        pvkCmdClearColorImage(cb, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    }
    vk_op_done();
}

static void vk_eng_clear_depth_stencil(void* user, u32 depth, u32 flags,
                                       float depth_value, u8 stencil)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(depth, VK_ENG_DEPTH);
    VkImageAspectFlags aspect = 0;
    if (flags & RSX_BE_CLEAR_DEPTH) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (flags & RSX_BE_CLEAR_STENCIL) aspect |= vk_depth_aspect_of(o ? o->fmt : s_vk.depth_format) & VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!o || !aspect) return;
    vk_op_begin();
    VkClearDepthStencilValue d = { depth_value, stencil };
    {
        const VkClearValue tv = { .depthStencil = d };
        if (vk_ems_clear(depth, aspect, &tv)) { vk_op_done(); return; }
    }
    if (s_f.pass && !o->fresh && s_f.pass_depth == o->im.img &&
        vk_pw(o) == s_f.pass_w && vk_ph(o) == s_f.pass_h) {
        VkClearAttachment ca = { .aspectMask = aspect, .colorAttachment = 0,
                                 .clearValue = { .depthStencil = d } };
        VkClearRect cr = { { { 0, 0 }, { vk_pw(o), vk_ph(o) } }, 0, 1 };
        pvkCmdClearAttachments(s_f.main.cb, 1, &ca, 1, &cr);
        vk_op_done();
        return;
    }
    VkCommandBuffer cb = vk_rec_upload(vk_eng_rec(o), o->im.img);
    if (cb) {
        VkImageSubresourceRange r = { aspect, 0, 1, 0, 1 };
        pvkCmdClearDepthStencilImage(cb, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    }
    vk_op_done();
}

/* ---------------------------------------------------------------------------
 * Compute passes: RPCS3's output scaling at present (VKPresent.cpp with
 * upscalers/fsr1: nearest, bilinear, FSR1 EASU + RCAS) and the optional
 * temporal upscaler over the scene (rsx_be_scene_aa).
 * -------------------------------------------------------------------------*/
#define VK_CX_RING 8u                /* descriptor sets per pass, round robin */

typedef struct vk_cx_img {
    vk_image im;
    u32      w, h;
    VkFormat fmt;
    u8       undef;                  /* still UNDEFINED: first use moves it */
} vk_cx_img;

static VkSampler s_cx_smp;          /* nearest, clamp: every pass texelFetches */

static int vk_cx_sampler(void)
{
    if (s_cx_smp) return 0;
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                               .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                               .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    return pvkCreateSampler(s_vk.device, &si, NULL, &s_cx_smp) == VK_SUCCESS ? 0 : -1;
}

/* kinds[i]: 0 combined image sampler, 1 storage image, at binding i. */
static int vk_cx_layout(const u8* kinds, u32 n, u32 push_bytes,
                        VkDescriptorSetLayout* dsl, VkPipelineLayout* pl)
{
    VkDescriptorSetLayoutBinding b[8];
    for (u32 i = 0; i < n && i < 8; i++) {
        VkDescriptorSetLayoutBinding x = {
            .binding = i, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .descriptorType = kinds[i] ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                       : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
        b[i] = x;
    }
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = n, .pBindings = b };
    if (pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, dsl) != VK_SUCCESS) return -1;
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes };
    VkPipelineLayoutCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = dsl,
                                       .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
    return pvkCreatePipelineLayout(s_vk.device, &pci, NULL, pl) == VK_SUCCESS ? 0 : -1;
}

static int vk_cx_pipeline(const uint32_t* code, size_t bytes, VkPipelineLayout pl, VkPipeline* out)
{
    VkShaderModule m = VK_NULL_HANDLE;
    if (vk_create_shader(code, bytes, &m)) return -1;
    VkComputePipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main" },
        .layout = pl };
    const VkResult r = pvkCreateComputePipelines(s_vk.device, s_vk.pcache, 1, &ci, NULL, out);
    pvkDestroyShaderModule(s_vk.device, m, NULL);
    if (r != VK_SUCCESS) {
        VK_LOG("vkCreateComputePipelines failed (VkResult %d)\n", (int)r);
        *out = VK_NULL_HANDLE;
        return -1;
    }
    return 0;
}

/* VK_CX_RING sets of each of `nl` layouts from one pool of their own. */
static int vk_cx_sets(const VkDescriptorSetLayout* dsl, u32 nl, u32 n_sampled, u32 n_storage,
                      VkDescriptorPool* pool, VkDescriptorSet* sets)
{
    VkDescriptorPoolSize ps[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, n_sampled * VK_CX_RING },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, n_storage * VK_CX_RING } };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = nl * VK_CX_RING, .poolSizeCount = 2, .pPoolSizes = ps };
    if (pvkCreateDescriptorPool(s_vk.device, &pci, NULL, pool) != VK_SUCCESS) return -1;
    for (u32 i = 0; i < VK_CX_RING; i++) {
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                           .descriptorPool = *pool, .descriptorSetCount = nl,
                                           .pSetLayouts = dsl };
        if (pvkAllocateDescriptorSets(s_vk.device, &ai, sets + i * nl) != VK_SUCCESS) return -1;
    }
    return 0;
}

static void vk_cx_write(VkDescriptorSet set, const u8* kinds, u32 n, const VkImageView* views)
{
    VkDescriptorImageInfo ii[8];
    VkWriteDescriptorSet w[8];
    for (u32 i = 0; i < n && i < 8; i++) {
        VkDescriptorImageInfo x = { kinds[i] ? VK_NULL_HANDLE : s_cx_smp, views[i], VK_IMAGE_LAYOUT_GENERAL };
        ii[i] = x;
        VkWriteDescriptorSet y = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                   .dstBinding = i, .descriptorCount = 1,
                                   .descriptorType = kinds[i] ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                              : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   .pImageInfo = &ii[i] };
        w[i] = y;
    }
    pvkUpdateDescriptorSets(s_vk.device, n, w, 0, NULL);
}

/* 1 when (re)created (UNDEFINED until vk_cx_img_ready), 0 unchanged, -1 failed. */
static int vk_cx_img_ensure(vk_cx_img* x, VkFormat f, u32 w, u32 h, VkImageUsageFlags usage)
{
    if (x->im.img && x->w == w && x->h == h && x->fmt == f) return 0;
    vk_defer_image(&x->im);
    x->w = x->h = 0;
    if (vk_create_image(&x->im, f, w, h, usage, VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&x->im);
        return -1;
    }
    x->w = w; x->h = h; x->fmt = f; x->undef = 1;
    return 1;
}

static void vk_cx_img_ready(VkCommandBuffer cb, vk_cx_img* x)
{
    if (!x->undef) return;
    vk_barrier_image_on(cb, x->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    x->undef = 0;
}

static void vk_cx_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags sa,
                          VkPipelineStageFlags dst, VkAccessFlags da)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = sa, .dstAccessMask = da };
    pvkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void vk_cx_relayout(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
}

/* ---- output scaling ------------------------------------------------------
 * RSX_FILTER / PS3RECOMP_VK_OUTPUT_SCALING: linear (default), point|nearest,
 * fsr (RPCS3 "Output Scaling Mode"); RSX_SHARPEN 0..1 the RCAS strength
 * ("FidelityFX CAS Sharpening Intensity" / 100). As in RPCS3, FSR only runs
 * when the frame is upscaled; otherwise the blit filters. */
enum { VK_OS_NEAREST = 0, VK_OS_BILINEAR = 1, VK_OS_FSR = 2 };
static struct {
    int         read, mode, broken;
    float       sharp;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout      pl;
    VkPipeline            easu, rcas;
    VkDescriptorPool      pool;
    VkDescriptorSet       set[VK_CX_RING * 2];
    u32         ring;
    vk_cx_img   mid, out;
} s_os;
static const u8 s_os_kinds[2] = { 0, 1 };

static void vk_os_settings(void)
{
    if (s_os.read) return;
    s_os.read = 1;
    const char* e = getenv("PS3RECOMP_VK_OUTPUT_SCALING");
    if (!e || !*e) e = getenv("RSX_FILTER");
    char m[16] = { 0 };
    for (u32 i = 0; e && e[i] && i < sizeof m - 1; i++)
        m[i] = (char)((e[i] >= 'A' && e[i] <= 'Z') ? e[i] + 32 : e[i]);
    s_os.mode = VK_OS_BILINEAR;
    if (!strncmp(m, "fsr", 3)) s_os.mode = VK_OS_FSR;
    else if (!strcmp(m, "point") || !strcmp(m, "nearest")) s_os.mode = VK_OS_NEAREST;
    s_os.sharp = 0.5f;
    const char* s = getenv("RSX_SHARPEN");
    if (s && *s) {
        float v = (float)atof(s);
        if (v > 1.0f) v *= 0.01f;                /* a percentage, as RPCS3's setting */
        s_os.sharp = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    }
    VK_LOG("output scaling: %s%s\n",
           s_os.mode == VK_OS_FSR ? "FSR1 (EASU + RCAS)" : s_os.mode == VK_OS_NEAREST ? "nearest" : "bilinear",
           s_os.mode == VK_OS_FSR ? "" : "");
    if (s_os.mode == VK_OS_FSR) VK_LOG("output scaling: RCAS sharpening %.2f\n", (double)s_os.sharp);
}

static VkFilter vk_os_filter(void)
{
    vk_os_settings();
    return s_os.mode == VK_OS_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

void rsx_vulkan_backend_force_output_fsr1(void)
{
    vk_os_settings();
    if (s_os.mode == VK_OS_FSR) return;
    s_os.mode = VK_OS_FSR;
    VK_LOG("output scaling: FSR1 (EASU + RCAS), RCAS sharpening %.2f\n", (double)s_os.sharp);
}

static int vk_os_init(void)
{
    if (s_os.easu) return 0;
    if (vk_cx_sampler() || vk_cx_layout(s_os_kinds, 2, 48, &s_os.dsl, &s_os.pl) ||
        vk_cx_pipeline(rsx_vk_fsr1_easu_spv, sizeof rsx_vk_fsr1_easu_spv, s_os.pl, &s_os.easu) ||
        vk_cx_pipeline(rsx_vk_fsr1_rcas_spv, sizeof rsx_vk_fsr1_rcas_spv, s_os.pl, &s_os.rcas)) return -1;
    const VkDescriptorSetLayout l[2] = { s_os.dsl, s_os.dsl };
    return vk_cx_sets(l, 2, 1, 1, &s_os.pool, s_os.set);
}

/* FSR1 of the source rectangle into a dw x dh image (GENERAL, ready for a
 * transfer read), or VK_NULL_HANDLE to blit as before. */
static VkImage vk_os_fsr(VkCommandBuffer cb, const vk_eng_obj* o, u32 sx, u32 sy, u32 sw, u32 sh,
                         u32 dw, u32 dh)
{
    vk_os_settings();
    if (s_os.mode != VK_OS_FSR || s_os.broken || !sw || !sh) return VK_NULL_HANDLE;
    if (sw >= dw || sh >= dh || dw > vk_max_dim() || dh > vk_max_dim()) return VK_NULL_HANDLE;
    const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vk_os_init() ||
        vk_cx_img_ensure(&s_os.mid, VK_FORMAT_R8G8B8A8_UNORM, dw, dh, u) < 0 ||
        vk_cx_img_ensure(&s_os.out, VK_FORMAT_R8G8B8A8_UNORM, dw, dh, u) < 0) {
        VK_LOG("output scaling: FSR1 setup failed, bilinear instead\n");
        s_os.broken = 1;
        return VK_NULL_HANDLE;
    }
    vk_cx_img_ready(cb, &s_os.mid);
    vk_cx_img_ready(cb, &s_os.out);
    VkDescriptorSet* set = &s_os.set[(s_os.ring++ % VK_CX_RING) * 2u];
    const VkImageView v0[2] = { o->im.view, s_os.mid.im.view };
    const VkImageView v1[2] = { s_os.mid.im.view, s_os.out.im.view };
    vk_cx_write(set[0], s_os_kinds, 2, v0);
    vk_cx_write(set[1], s_os_kinds, 2, v1);
    struct { float con0[4]; int32_t rect[4]; float misc[4]; } pc;
    const float kx = (float)sw / (float)dw, ky = (float)sh / (float)dh;
    pc.con0[0] = kx; pc.con0[1] = ky;
    pc.con0[2] = 0.5f * kx - 0.5f + (float)sx; pc.con0[3] = 0.5f * ky - 0.5f + (float)sy;
    pc.rect[0] = (int32_t)sx; pc.rect[1] = (int32_t)sy;
    pc.rect[2] = (int32_t)(sx + sw - 1u); pc.rect[3] = (int32_t)(sy + sh - 1u);
    pc.misc[0] = 0.0f; pc.misc[1] = (float)dw; pc.misc[2] = (float)dh; pc.misc[3] = 0.0f;
    pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_os.easu);
    pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_os.pl, 0, 1, &set[0], 0, NULL);
    pvkCmdPushConstants(cb, s_os.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    pvkCmdDispatch(cb, (dw + 7u) / 8u, (dh + 7u) / 8u, 1);
    vk_cx_barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    pc.rect[0] = 0; pc.rect[1] = 0; pc.rect[2] = (int32_t)dw - 1; pc.rect[3] = (int32_t)dh - 1;
    pc.misc[0] = exp2f(-(2.0f - s_os.sharp * 2.0f));
    pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_os.rcas);
    pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_os.pl, 0, 1, &set[1], 0, NULL);
    pvkCmdPushConstants(cb, s_os.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    pvkCmdDispatch(cb, (dw + 7u) / 8u, (dh + 7u) / 8u, 1);
    vk_cx_barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    return s_os.out.im.img;
}

/* ---- display gamma (RPCS3 VKPresent.cpp video_out_calibration_pass) ------
 * With "Use full RGB output range" on, RPCS3 blits raw unless the avconf
 * gamma (cellVideoOutSetGamma) is not 1; then the calibration pass samples
 * the scaled frame and writes pow(rgb, gamma) with alpha 1. Here: the source
 * rectangle is blitted (scaled) into an RGBA8 image, the pass runs in place,
 * and the caller blits that 1:1. RSX_CFG_LEGACY=1 ignores the gamma. */
#include "vulkan/rsx_vk_present_gamma.spv.h"
extern float cellAvconfExt_display_gamma(void);
static struct {
    int         read, legacy, broken;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout      pl;
    VkPipeline            pipe;
    VkDescriptorPool      pool;
    VkDescriptorSet       set[VK_CX_RING];
    u32         ring;
    vk_cx_img   img;
} s_gm;
static const u8 s_gm_kinds[1] = { 1 };

static VkImage vk_gm_pass(VkCommandBuffer cb, VkImage src, u32 sx, u32 sy, u32 sw, u32 sh,
                          VkFilter filter, u32 dw, u32 dh)
{
    if (!s_gm.read) {
        s_gm.read = 1;
        const char* e = getenv("RSX_CFG_LEGACY");
        s_gm.legacy = e && e[0] && e[0] != '0';
    }
    if (s_gm.legacy || s_gm.broken || !sw || !sh || !dw || !dh) return VK_NULL_HANDLE;
    const float gamma = cellAvconfExt_display_gamma();
    const int limited = 0;                       /* Use full RGB output range: true */
    if (!rsx_vk_present_calibrate(gamma, !limited)) return VK_NULL_HANDLE;
    if (dw > vk_max_dim() || dh > vk_max_dim()) return VK_NULL_HANDLE;
    if (!s_gm.pipe) {
        if (vk_cx_layout(s_gm_kinds, 1, 16, &s_gm.dsl, &s_gm.pl) ||
            vk_cx_pipeline(rsx_vk_present_gamma_spv, sizeof rsx_vk_present_gamma_spv, s_gm.pl, &s_gm.pipe) ||
            vk_cx_sets(&s_gm.dsl, 1, 1, 1, &s_gm.pool, s_gm.set)) {
            VK_LOG("display gamma: pass setup failed, raw blit instead\n");
            s_gm.broken = 1;
            return VK_NULL_HANDLE;
        }
        VK_LOG("display gamma: calibration pass, gamma %.3f\n", (double)gamma);
    }
    if (vk_cx_img_ensure(&s_gm.img, VK_FORMAT_R8G8B8A8_UNORM, dw, dh,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT) < 0) {
        s_gm.broken = 1;
        return VK_NULL_HANDLE;
    }
    vk_cx_img_ready(cb, &s_gm.img);
    vk_cx_barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { (int32_t)sx, (int32_t)sy, 0 }, { (int32_t)(sx + sw), (int32_t)(sy + sh), 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 }, { (int32_t)dw, (int32_t)dh, 1 } },
    };
    pvkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, s_gm.img.im.img, VK_IMAGE_LAYOUT_GENERAL,
                    1, &blit, filter);
    vk_cx_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    const VkDescriptorSet set = s_gm.set[s_gm.ring++ % VK_CX_RING];
    vk_cx_write(set, s_gm_kinds, 1, &s_gm.img.im.view);
    const float pc[4] = { gamma, (float)limited, (float)dw, (float)dh };
    pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_gm.pipe);
    pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_gm.pl, 0, 1, &set, 0, NULL);
    pvkCmdPushConstants(cb, s_gm.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    pvkCmdDispatch(cb, (dw + 7u) / 8u, (dh + 7u) / 8u, 1);
    vk_cx_barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    return s_gm.img.im.img;
}

/* ---- temporal upscaler (rsx_vulkan_backend_set_temporal_upscaler) -------- */
static struct {
    const rsx_vk_upscaler_ops* ops;
    int         ready, broken, depth_checked, depth_sampled, eval_fail;
    uint32_t    api;
    VkDescriptorSetLayout dsl_prep, dsl_merge;
    VkPipelineLayout      pl_prep, pl_merge;
    VkPipeline            prep, merge8, merge16;
    VkDescriptorPool      pool;
    VkDescriptorSet       set[VK_CX_RING * 2];
    u32         ring;
    vk_cx_img   color, depth[2], mv, mask, out, fin;
    u32         di;                  /* depth[di] receives this run's depth */
    int         prev_ok, created;
    u32         cw, ch, cflags;
} s_aa;
static const u8 s_aa_prep_kinds[7] = { 0, 0, 1, 1, 1, 0, 1 };
static const u8 s_aa_merge_kinds[3] = { 0, 0, 1 };

void rsx_vulkan_backend_set_temporal_upscaler(const rsx_vk_upscaler_ops* ops)
{
    s_aa.ops = ops;
}

int rsx_vulkan_backend_temporal_upscaler_ready(void)
{
    return s_aa.ready && !s_aa.broken;
}

/* Depth targets are sampled by the prep pass when an upscaler is registered
 * and the depth format can be sampled. */
static VkImageUsageFlags vk_aa_depth_usage(void)
{
    if (!s_aa.ops || s_aa.broken) return 0;
    if (!s_aa.depth_checked && s_vk.phys) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, s_vk.depth_format, &fp);
        s_aa.depth_sampled = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? 1 : 0;
        s_aa.depth_checked = 1;
        if (!s_aa.depth_sampled) VK_LOG("temporal upscaler: depth format cannot be sampled\n");
    }
    return s_aa.depth_sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0;
}

/* Instance creation: Vulkan 1.2 when the loader has it (the upscaler's
 * device features need it). */
static uint32_t vk_aa_api_version(void)
{
    s_aa.api = VK_API_VERSION_1_0;
    if (!s_aa.ops || !pvkGetInstanceProcAddr) return s_aa.api;
    PFN_vkEnumerateInstanceVersion ev = (PFN_vkEnumerateInstanceVersion)
        pvkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    uint32_t v = VK_API_VERSION_1_0;
    if (ev && ev(&v) == VK_SUCCESS && v >= VK_API_VERSION_1_2) s_aa.api = VK_API_VERSION_1_2;
    else VK_LOG("temporal upscaler: the Vulkan loader is older than 1.2\n");
    return s_aa.api;
}

static int vk_aa_has(const char* const* list, u32 n, const char* name)
{
    for (u32 i = 0; i < n; i++) if (!strcmp(list[i], name)) return 1;
    return 0;
}

/* Adds the upscaler's supported instance extensions to ci. */
static void vk_aa_instance_exts(VkInstanceCreateInfo* ci)
{
    static const char* exts[48];
    unsigned ni = 0, nd = 0;
    const char** ie = NULL; const char** de = NULL;
    if (!s_aa.ops || !s_aa.ops->required_extensions ||
        !s_aa.ops->required_extensions(s_aa.ops->user, &ni, &ie, &nd, &de) || !ni || !ie) return;
    PFN_vkEnumerateInstanceExtensionProperties en = (PFN_vkEnumerateInstanceExtensionProperties)
        pvkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    u32 np = 0;
    if (!en || en(NULL, &np, NULL) != VK_SUCCESS || !np) return;
    VkExtensionProperties* props = (VkExtensionProperties*)malloc(np * sizeof *props);
    if (!props) return;
    if (en(NULL, &np, props) != VK_SUCCESS) np = 0;
    u32 n = 0;
    for (u32 i = 0; i < ci->enabledExtensionCount && n < 48; i++) exts[n++] = ci->ppEnabledExtensionNames[i];
    for (unsigned i = 0; i < ni && n < 48; i++) {
        int ok = 0;
        for (u32 k = 0; k < np && !ok; k++) ok = !strcmp(props[k].extensionName, ie[i]);
        if (!ok) { VK_LOG("temporal upscaler: instance extension %s is not supported\n", ie[i]); continue; }
        if (!vk_aa_has(exts, n, ie[i])) exts[n++] = ie[i];
    }
    free(props);
    ci->enabledExtensionCount = n;
    ci->ppEnabledExtensionNames = n ? exts : NULL;
}

/* Device creation: the upscaler's supported device extensions, Vulkan 1.2
 * bufferDeviceAddress, and the storage formats the prep pass writes. */
static void vk_aa_device(VkDeviceCreateInfo* dci, VkPhysicalDeviceFeatures* want,
                         const VkPhysicalDeviceFeatures* have)
{
    static const char* exts[64];
    static VkPhysicalDeviceVulkan12Features v12, h12s;
    int has12 = 0;
    if (!s_aa.ops) return;
    want->shaderStorageImageExtendedFormats = have->shaderStorageImageExtendedFormats;
    if (!have->shaderStorageImageExtendedFormats) {
        VK_LOG("temporal upscaler: no shaderStorageImageExtendedFormats, disabled\n");
        s_aa.broken = 1;
        return;
    }
    u32 n = 0;
    for (u32 i = 0; i < dci->enabledExtensionCount && n < 64; i++) exts[n++] = dci->ppEnabledExtensionNames[i];
    unsigned ni = 0, nd = 0;
    const char** ie = NULL; const char** de = NULL;
    if (s_aa.ops->required_extensions &&
        s_aa.ops->required_extensions(s_aa.ops->user, &ni, &ie, &nd, &de) && de) {
        for (unsigned i = 0; i < nd && n < 64; i++) {
            if (!strcmp(de[i], "VK_EXT_buffer_device_address")) continue;   /* core 1.2 instead */
            if (!vk_has_device_extension(s_vk.phys, de[i])) {
                VK_LOG("temporal upscaler: device extension %s is not supported\n", de[i]);
                continue;
            }
            if (!vk_aa_has(exts, n, de[i])) exts[n++] = de[i];
        }
    }
    dci->enabledExtensionCount = n;
    dci->ppEnabledExtensionNames = n ? exts : NULL;
    VkPhysicalDeviceProperties dp;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &dp);
    PFN_vkGetPhysicalDeviceFeatures2 gf2 = (PFN_vkGetPhysicalDeviceFeatures2)
        pvkGetInstanceProcAddr(s_vk.instance, "vkGetPhysicalDeviceFeatures2");
    if (s_aa.api >= VK_API_VERSION_1_2 && dp.apiVersion >= VK_API_VERSION_1_2 && gf2) {
        VkPhysicalDeviceVulkan12Features h12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &h12 };
        gf2(s_vk.phys, &f2);
        memset(&v12, 0, sizeof v12);
        v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        v12.bufferDeviceAddress = h12.bufferDeviceAddress;
        /* With the 1.2 feature struct chained, an enabled extension promoted to
         * 1.2 needs its feature bit too (VUID-VkDeviceCreateInfo-04476..04480). */
        if (vk_aa_has(exts, n, "VK_KHR_sampler_mirror_clamp_to_edge")) v12.samplerMirrorClampToEdge = h12.samplerMirrorClampToEdge;
        if (vk_aa_has(exts, n, "VK_KHR_draw_indirect_count"))          v12.drawIndirectCount = h12.drawIndirectCount;
        if (vk_aa_has(exts, n, "VK_EXT_descriptor_indexing"))          v12.descriptorIndexing = h12.descriptorIndexing;
        if (vk_aa_has(exts, n, "VK_EXT_sampler_filter_minmax"))        v12.samplerFilterMinmax = h12.samplerFilterMinmax;
        if (vk_aa_has(exts, n, "VK_EXT_shader_viewport_index_layer")) {
            v12.shaderOutputViewportIndex = h12.shaderOutputViewportIndex;
            v12.shaderOutputLayer = h12.shaderOutputLayer;
        }
        v12.pNext = (void*)dci->pNext;
        dci->pNext = &v12;
        h12s = h12;
        h12s.pNext = NULL;
        has12 = 1;
    }
    /* The upscaler's optional features (bits it may use when supported). */
    if (s_aa.ops->device_features)
        s_aa.ops->device_features(s_aa.ops->user, (void*)s_vk.phys, (void*)pvkGetInstanceProcAddr,
                                  (void*)s_vk.instance, want, have, has12 ? (void*)&v12 : NULL,
                                  has12 ? (const void*)&h12s : NULL, (unsigned)n, exts,
                                  (const void**)&dci->pNext);
}

/* After the device and frame recording exist. */
static void vk_aa_device_ready(void)
{
    if (!s_aa.ops || s_aa.broken) return;
    VkFormatProperties a, b;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, VK_FORMAT_R16G16_SFLOAT, &a);
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, VK_FORMAT_R8_UNORM, &b);
    if (!(a.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
        !(b.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || !vk_aa_depth_usage()) {
        VK_LOG("temporal upscaler: formats unsupported, disabled\n");
        s_aa.broken = 1;
        return;
    }
    if (!s_aa.ops->init ||
        !s_aa.ops->init(s_aa.ops->user, (void*)s_vk.instance, (void*)s_vk.phys, (void*)s_vk.device,
                        (void*)pvkGetInstanceProcAddr, (void*)pvkGetDeviceProcAddr)) {
        VK_LOG("temporal upscaler: not available on this device\n");
        s_aa.broken = 1;
        return;
    }
    s_aa.ready = 1;
    VK_LOG("temporal upscaler: ready\n");
}

static int vk_aa_objects(void)
{
    if (s_aa.prep) return 0;
    if (vk_cx_sampler() ||
        vk_cx_layout(s_aa_prep_kinds, 7, 112, &s_aa.dsl_prep, &s_aa.pl_prep) ||
        vk_cx_layout(s_aa_merge_kinds, 3, 32, &s_aa.dsl_merge, &s_aa.pl_merge) ||
        vk_cx_pipeline(rsx_vk_aa_merge8_spv, sizeof rsx_vk_aa_merge8_spv, s_aa.pl_merge, &s_aa.merge8) ||
        vk_cx_pipeline(rsx_vk_aa_merge16_spv, sizeof rsx_vk_aa_merge16_spv, s_aa.pl_merge, &s_aa.merge16))
        return -1;
    const VkDescriptorSetLayout l[2] = { s_aa.dsl_prep, s_aa.dsl_merge };
    if (vk_cx_sets(l, 2, 5, 5, &s_aa.pool, s_aa.set)) return -1;
    return vk_cx_pipeline(rsx_vk_aa_prep_spv, sizeof rsx_vk_aa_prep_spv, s_aa.pl_prep, &s_aa.prep);
}

static rsx_vk_upscaler_image vk_aa_desc(const vk_cx_img* x)
{
    rsx_vk_upscaler_image r = { (u64)(uintptr_t)x->im.img, (u64)(uintptr_t)x->im.view, (int)x->fmt };
    return r;
}

/* rsx_draw_backend.scene_aa: prep -> upscaler -> merge -> copy back into the
 * scene target, all in the frame's command buffer at this point. */
static int vk_eng_scene_aa(void* user, const rsx_be_scene_aa* q)
{
    (void)user;
    if (!s_aa.ready || s_aa.broken || !q || !s_vk.device || !s_f.on) return 0;
    vk_eng_obj* c = vk_eng_get(q->color, VK_ENG_COLOR);
    vk_eng_obj* d = vk_eng_get(q->depth, VK_ENG_DEPTH);
    if (!c || !d) return 0;
    const int f16 = c->fmt == VK_FORMAT_R16G16B16A16_SFLOAT;
    if (!f16 && c->fmt != VK_FORMAT_R8G8B8A8_UNORM) return 0;
    u32 w = q->w ? q->w : c->w, h = q->h ? q->h : c->h;
    if (w > c->w) w = c->w;
    if (w > d->w) w = d->w;
    if (h > c->h) h = c->h;
    if (h > d->h) h = d->h;
    if (w < 16u || h < 16u) return 0;
    /* The upscaler works on the images: render pixels at the scale. */
    if (vk_rs_of(c) != vk_rs_of(d)) vk_rs_match(&q->color, 1, q->depth);
    if (vk_rs_of(c) != vk_rs_of(d)) return 0;
    const float ars = vk_rs_of(c);
    const u32 gw = w, gh = h;
    if (ars > 1.0f) {
        w = vk_rs_c(c, w, vk_pw(c) < vk_pw(d) ? vk_pw(c) : vk_pw(d));
        h = vk_rs_c(c, h, vk_ph(c) < vk_ph(d) ? vk_ph(c) : vk_ph(d));
    }
    vk_op_begin();
    vk_ems_ss(q->color);                 /* MSAA twins: the widened images are read and written */
    vk_ems_ss(q->depth);
    vk_ems_wrote_ss(q->color);
    if (vk_aa_objects()) {
        VK_LOG("temporal upscaler: pipeline setup failed, disabled\n");
        s_aa.broken = 1;
        return 0;
    }
    const VkImageUsageFlags su = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    int e, resized = 0;
    if ((e = vk_cx_img_ensure(&s_aa.color, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, su)) < 0) goto fail;
    resized |= e;
    if ((e = vk_cx_img_ensure(&s_aa.depth[0], VK_FORMAT_R32_SFLOAT, w, h, su)) < 0) goto fail;
    resized |= e;
    if ((e = vk_cx_img_ensure(&s_aa.depth[1], VK_FORMAT_R32_SFLOAT, w, h, su)) < 0) goto fail;
    resized |= e;
    if ((e = vk_cx_img_ensure(&s_aa.mv, VK_FORMAT_R16G16_SFLOAT, w, h, su)) < 0) goto fail;
    resized |= e;
    if ((e = vk_cx_img_ensure(&s_aa.mask, VK_FORMAT_R8_UNORM, w, h, su)) < 0) goto fail;
    resized |= e;
    if ((e = vk_cx_img_ensure(&s_aa.out, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, su)) < 0) goto fail;
    resized |= e;
    if (vk_cx_img_ensure(&s_aa.fin, c->fmt, w, h, su | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) < 0) goto fail;
    if (resized) { s_aa.prev_ok = 0; s_aa.created = 0; }

    /* The depth aspect alone: a depth/stencil view cannot be sampled. */
    VkImageView dv = VK_NULL_HANDLE;
    {
        VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                      .image = d->im.img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                      .format = d->fmt,
                                      .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
        if (pvkCreateImageView(s_vk.device, &vci, NULL, &dv) != VK_SUCCESS) return 0;
    }
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (!cb) { vk_image t = { VK_NULL_HANDLE, VK_NULL_HANDLE, dv }; vk_defer_image(&t); return 0; }
    vk_eng_touch(q->color);
    vk_eng_touch(q->depth);
    vk_cx_img_ready(cb, &s_aa.color);
    vk_cx_img_ready(cb, &s_aa.depth[0]);
    vk_cx_img_ready(cb, &s_aa.depth[1]);
    vk_cx_img_ready(cb, &s_aa.mv);
    vk_cx_img_ready(cb, &s_aa.mask);
    vk_cx_img_ready(cb, &s_aa.out);
    vk_cx_img_ready(cb, &s_aa.fin);

    const u32 cur = s_aa.di & 1u, prv = cur ^ 1u;
    VkDescriptorSet* set = &s_aa.set[(s_aa.ring++ % VK_CX_RING) * 2u];
    const VkImageView pv[7] = { c->im.view, dv, s_aa.color.im.view, s_aa.depth[cur].im.view,
                                s_aa.mv.im.view, s_aa.depth[prv].im.view, s_aa.mask.im.view };
    vk_cx_write(set[0], s_aa_prep_kinds, 7, pv);
    const float vw = (float)(q->w ? q->w : gw), vh = (float)(q->h ? q->h : gh);
    const int reset = q->reset || !s_aa.prev_ok;
    float pc[28];
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) pc[col * 4 + row] = q->reproject[row * 4 + col];
    pc[16] = 2.0f * q->jitter_px[0] / vw; pc[17] = 2.0f * q->jitter_px[1] / vh;
    pc[18] = vw * ars; pc[19] = vh * ars;
    pc[20] = q->zA; pc[21] = q->zB; pc[22] = (float)w; pc[23] = (float)h;
    pc[24] = (q->use_mask && !reset) ? 1.0f : 0.0f; pc[25] = q->inverted ? 1.0f : 0.0f;
    pc[26] = 0.0f; pc[27] = 0.0f;
    pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_aa.prep);
    pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_aa.pl_prep, 0, 1, &set[0], 0, NULL);
    pvkCmdPushConstants(cb, s_aa.pl_prep, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    pvkCmdDispatch(cb, (w + 7u) / 8u, (h + 7u) / 8u, 1);

    /* The upscaler reads its inputs in SHADER_READ_ONLY_OPTIMAL. */
    VkImage ins[4] = { s_aa.color.im.img, s_aa.depth[cur].im.img, s_aa.mv.im.img, s_aa.mask.im.img };
    for (int i = 0; i < 4; i++)
        vk_cx_relayout(cb, ins[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const int flags = (q->inverted ? 1 : 0) | (f16 ? 2 : 0);
    int ok = 1;
    if (!s_aa.created || s_aa.cw != w || s_aa.ch != h || s_aa.cflags != (u32)flags) {
        ok = s_aa.ops->create &&
             s_aa.ops->create(s_aa.ops->user, (void*)cb, w, h, w, h, flags);
        s_aa.created = ok; s_aa.cw = w; s_aa.ch = h; s_aa.cflags = (u32)flags;
        if (!ok) { VK_LOG("temporal upscaler: create failed, disabled\n"); s_aa.broken = 1; }
    }
    if (ok) {
        const rsx_vk_upscaler_image ic = vk_aa_desc(&s_aa.color), id = vk_aa_desc(&s_aa.depth[cur]),
                                    im = vk_aa_desc(&s_aa.mv), io = vk_aa_desc(&s_aa.out);
        rsx_vk_upscaler_image ik = vk_aa_desc(&s_aa.mask);
        if (!q->use_mask) ik.image = 0;
        ok = s_aa.ops->evaluate &&
             s_aa.ops->evaluate(s_aa.ops->user, (void*)cb, &ic, &id, &im, &ik, &io, w, h,
                                q->eval_jitter[0] * ars, q->eval_jitter[1] * ars,
                                vw * ars, vh * ars, reset,
                                q->frame_ms > 0.0f ? q->frame_ms : 16.7f);
        if (!ok && ++s_aa.eval_fail >= 8) {
            VK_LOG("temporal upscaler: evaluation keeps failing, disabled\n");
            s_aa.broken = 1;
        }
    }
    for (int i = 0; i < 4; i++)
        vk_cx_relayout(cb, ins[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vk_cx_barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    if (ok) {
        s_aa.eval_fail = 0;
        const VkImageView mvw[3] = { s_aa.out.im.view, c->im.view, s_aa.fin.im.view };
        vk_cx_write(set[1], s_aa_merge_kinds, 3, mvw);
        const float mpc[8] = { 0.0f, (float)w, (float)h, (float)q->debug_view, 1.0f, 1.0f, 0.0f, 0.0f };
        const VkPipeline mp = f16 ? s_aa.merge16 : s_aa.merge8;
        pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, mp);
        pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_aa.pl_merge, 0, 1, &set[1], 0, NULL);
        pvkCmdPushConstants(cb, s_aa.pl_merge, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof mpc, mpc);
        pvkCmdDispatch(cb, (w + 7u) / 8u, (h + 7u) / 8u, 1);
        vk_cx_barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkImageCopy cp = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                           .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                           .extent = { w, h, 1 } };
        pvkCmdCopyImage(cb, s_aa.fin.im.img, VK_IMAGE_LAYOUT_GENERAL, c->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        s_aa.prev_ok = 1;
        s_aa.di = prv;
    } else {
        s_aa.prev_ok = 0;
    }
    vk_cmd_after_everything(cb);
    s_f.main.other = 1;
    s_f.b_valid = 0;
    { vk_image t = { VK_NULL_HANDLE, VK_NULL_HANDLE, dv }; vk_defer_image(&t); }
    vk_op_done();
    return ok;
fail:
    VK_LOG("temporal upscaler: out of memory for %ux%u, disabled\n", w, h);
    s_aa.broken = 1;
    return 0;
}

static void vk_cx_shutdown(void)
{
    if (s_aa.ops && s_aa.ready && s_aa.ops->shutdown) s_aa.ops->shutdown(s_aa.ops->user);
    s_aa.ready = 0;
    vk_cx_img* imgs[] = { &s_os.mid, &s_os.out, &s_aa.color, &s_aa.depth[0], &s_aa.depth[1],
                          &s_aa.mv, &s_aa.mask, &s_aa.out, &s_aa.fin };
    for (size_t i = 0; i < sizeof imgs / sizeof imgs[0]; i++) {
        vk_destroy_image(&imgs[i]->im);
        imgs[i]->w = imgs[i]->h = 0;
    }
    VkPipeline pipes[] = { s_os.easu, s_os.rcas, s_aa.prep, s_aa.merge8, s_aa.merge16 };
    for (size_t i = 0; i < sizeof pipes / sizeof pipes[0]; i++)
        if (pipes[i]) pvkDestroyPipeline(s_vk.device, pipes[i], NULL);
    s_os.easu = s_os.rcas = s_aa.prep = s_aa.merge8 = s_aa.merge16 = VK_NULL_HANDLE;
    if (s_os.pool) pvkDestroyDescriptorPool(s_vk.device, s_os.pool, NULL);
    if (s_aa.pool) pvkDestroyDescriptorPool(s_vk.device, s_aa.pool, NULL);
    s_os.pool = s_aa.pool = VK_NULL_HANDLE;
    vk_destroy_image(&s_gm.img.im);
    s_gm.img.w = s_gm.img.h = 0;
    if (s_gm.pipe) pvkDestroyPipeline(s_vk.device, s_gm.pipe, NULL);
    if (s_gm.pool) pvkDestroyDescriptorPool(s_vk.device, s_gm.pool, NULL);
    if (s_gm.pl) pvkDestroyPipelineLayout(s_vk.device, s_gm.pl, NULL);
    if (s_gm.dsl) pvkDestroyDescriptorSetLayout(s_vk.device, s_gm.dsl, NULL);
    s_gm.pipe = VK_NULL_HANDLE; s_gm.pool = VK_NULL_HANDLE;
    s_gm.pl = VK_NULL_HANDLE; s_gm.dsl = VK_NULL_HANDLE;
    VkPipelineLayout pls[] = { s_os.pl, s_aa.pl_prep, s_aa.pl_merge };
    for (size_t i = 0; i < 3; i++) if (pls[i]) pvkDestroyPipelineLayout(s_vk.device, pls[i], NULL);
    s_os.pl = s_aa.pl_prep = s_aa.pl_merge = VK_NULL_HANDLE;
    VkDescriptorSetLayout dls[] = { s_os.dsl, s_aa.dsl_prep, s_aa.dsl_merge };
    for (size_t i = 0; i < 3; i++) if (dls[i]) pvkDestroyDescriptorSetLayout(s_vk.device, dls[i], NULL);
    s_os.dsl = s_aa.dsl_prep = s_aa.dsl_merge = VK_NULL_HANDLE;
    if (s_cx_smp) pvkDestroySampler(s_vk.device, s_cx_smp, NULL);
    s_cx_smp = VK_NULL_HANDLE;
    s_aa.created = s_aa.prev_ok = 0;
}

/* present_mu held. The frame is submitted; then the surface is blitted
 * straight into an acquired swapchain image by one more submission that waits
 * for the acquire and signals the present. The CPU waits for nothing here but
 * the next frame slot (vk_fr_advance), and the acquire itself. */
/* The destination rectangle of a sw x sh image on the swapchain, letterboxed
 * to aw:ah (RPCS3 avconf::aspect_convert_region); aw == 0 stretches. */
static void vk_present_dst_rect(u32 aw, u32 ah, int32_t d[4])
{
    const int32_t W = (int32_t)s_vk.sc_extent.width, H = (int32_t)s_vk.sc_extent.height;
    d[0] = 0; d[1] = 0; d[2] = W; d[3] = H;
    if (!aw || !ah || !W || !H) return;
    if ((u64)W * ah > (u64)H * aw) {            /* window wider: pillarbox */
        const int32_t w = (int32_t)((u64)H * aw / ah);
        d[0] = (W - w) / 2; d[2] = d[0] + w;
    } else if ((u64)W * ah < (u64)H * aw) {     /* taller: letterbox */
        const int32_t h = (int32_t)((u64)W * ah / aw);
        d[1] = (H - h) / 2; d[3] = d[1] + h;
    }
}

static void vk_eng_present_direct(vk_eng_obj* o, u32 sx, u32 sy, u32 sw, u32 sh,
                                  u32 aw, u32 ah)
{
    o->fresh = 0;
    vk_flush(0);
    if (!s_vk.windowed) return;
    vk_sc_check_resized();
    if ((s_vk.sc_stale || !s_vk.swapchain) && vk_swapchain_lost() != 0) return;
    if (!s_vk.windowed || !s_vk.swapchain) return;
    vk_frame* f = &s_f.fr[s_f.cur];
    u32 idx = 0;
    VkResult r = pvkAcquireNextImageKHR(s_vk.device, s_vk.swapchain, UINT64_MAX,
                                        f->acquire, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        if (vk_swapchain_lost() != 0 || !s_vk.swapchain) return;
        r = pvkAcquireNextImageKHR(s_vk.device, s_vk.swapchain, UINT64_MAX,
                                   f->acquire, VK_NULL_HANDLE, &idx);
    }
    if (r == VK_SUBOPTIMAL_KHR) s_vk.sc_stale = 1;
    else if (r != VK_SUCCESS) {
        if (r == VK_ERROR_OUT_OF_DATE_KHR) s_vk.sc_stale = 1;
        else VK_LOG("vkAcquireNextImageKHR failed (VkResult %d)\n", (int)r);
        return;
    }

    /* From here the acquire semaphore is signalled: something must wait on it. */
    const VkImage sc = s_vk.sc_images[idx];
    VkCommandBuffer cb = vk_rec_get(&s_f.main);
    if (cb) {
        VkImageMemoryBarrier b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = sc, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 0, NULL, 1, &b);
        if (sx >= o->w || sy >= o->h) { sx = sy = 0; }
        if (!sw || sx + sw > o->w) sw = o->w - sx;
        if (!sh || sy + sh > o->h) sh = o->h - sy;
        vk_rs_rect(o, &sx, &sy, &sw, &sh);
        int32_t d[4];
        vk_present_dst_rect(aw, ah, d);
        if (d[0] || d[1]) {
            /* Black borders: the acquired image's contents are undefined. */
            VkClearColorValue black = { { 0.f, 0.f, 0.f, 1.f } };
            VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            pvkCmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
            VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                   .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
            pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  0, 1, &mb, 0, NULL, 0, NULL);
        }
        VkImage src = o->im.img;
        if (d[2] > d[0] && d[3] > d[1]) {
            const VkImage up = vk_os_fsr(cb, o, sx, sy, sw, sh, (u32)(d[2] - d[0]), (u32)(d[3] - d[1]));
            if (up) { src = up; sx = sy = 0; sw = (u32)(d[2] - d[0]); sh = (u32)(d[3] - d[1]); }
            const u32 dw = (u32)(d[2] - d[0]), dh = (u32)(d[3] - d[1]);
            const VkImage gm = vk_gm_pass(cb, src, sx, sy, sw, sh,
                                          sw == dw && sh == dh ? VK_FILTER_NEAREST : vk_os_filter(), dw, dh);
            if (gm) { src = gm; sx = sy = 0; sw = dw; sh = dh; }
        }
        VkImageBlit blit = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = { { (int32_t)sx, (int32_t)sy, 0 },
                            { (int32_t)(sx + sw), (int32_t)(sy + sh), 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = { { d[0], d[1], 0 }, { d[2], d[3], 1 } },
        };
        const int same = (int32_t)sw == d[2] - d[0] && (int32_t)sh == d[3] - d[1];
        pvkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1, &blit, same ? VK_FILTER_NEAREST : vk_os_filter());
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              0, 0, NULL, 0, NULL, 1, &b);
        if (pvkEndCommandBuffer(cb) != VK_SUCCESS) { VK_LOG("vkEndCommandBuffer (present) failed\n"); cb = VK_NULL_HANDLE; }
    }
    s_f.main.open = 0;
    s_f.b_valid = 0;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &f->acquire, .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = cb ? 1u : 0u, .pCommandBuffers = &cb,
        .signalSemaphoreCount = cb ? 1u : 0u, .pSignalSemaphores = &s_vk.sc_done[idx],
    };
    VkFence fe = vk_fr_fence(f);
    r = vk_queue_submit(&si, fe);
    if (r != VK_SUCCESS) { VK_LOG("vkQueueSubmit (present) failed (VkResult %d)\n", (int)r); return; }
    if (fe) f->nsub++;
    else vk_fr_wait_all();
    if (!cb) return;
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
        .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
    };
    r = vk_queue_present(&pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) s_vk.sc_stale = 1;
    else if (r != VK_SUCCESS) VK_LOG("vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
    s_vk.presented++;
}

/* The named surface becomes the frame. In a window (and without a frame
 * dump) it is blitted straight into the swapchain; otherwise onto the
 * display image, and the existing present does the readback, the PPM dump
 * and the window. Either way the frame slot advances. */
static void vk_eng_present_region(void* user, u32 surface, u32 sx, u32 sy, u32 sw, u32 sh,
                                  u32 aw, u32 ah)
{
    (void)user;
    if (!s_vk.device) return;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (o) vk_ems_ss(surface);           /* MSAA: the widened image is what shows */
    pthread_mutex_lock(&s_vk.present_mu);
    if (s_f.on && s_vk.windowed && !s_vk.dump_path) {
        vk_present_complete_locked();
        /* A handle the backend no longer has keeps the last frame on screen
         * (RPCS3 would show black; a missing surface here is a transient
         * engine state, and a black frame is a visible flash). */
        if (o) vk_eng_present_direct(o, sx, sy, sw, sh, aw, ah);
        else vk_flush(0);
    } else {
        vk_flush(0);
        VkCommandBuffer cb = vk_present_record_begin();
        if (cb) {
            if (o) {
                vk_barrier_image_on(cb, o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
                vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
                u32 rx = sx, ry = sy;
                u32 rw = sw && sx + sw <= o->w ? sw : o->w - sx;
                u32 rh = sh && sy + sh <= o->h ? sh : o->h - sy;
                vk_rs_rect(o, &rx, &ry, &rw, &rh);
                const VkFilter flt = (vk_pw(o) == s_vk.width && vk_ph(o) == s_vk.height) ? VK_FILTER_NEAREST
                                                                                       : vk_os_filter();
                const VkImage gm = vk_gm_pass(cb, o->im.img, rx, ry, rw, rh, flt, s_vk.width, s_vk.height);
                VkImageBlit blit = {
                    .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                    .srcOffsets = { { gm ? 0 : (int32_t)rx, gm ? 0 : (int32_t)ry, 0 },
                                    { gm ? (int32_t)s_vk.width : (int32_t)(rx + rw),
                                      gm ? (int32_t)s_vk.height : (int32_t)(ry + rh), 1 } },
                    .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                    .dstOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.width, (int32_t)s_vk.height, 1 } },
                };
                pvkCmdBlitImage(cb, gm ? gm : o->im.img, VK_IMAGE_LAYOUT_GENERAL,
                                s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                                gm ? VK_FILTER_NEAREST : flt);
            }
            vk_present_record_end(cb);
        }
    }
    vk_fr_advance();
    if (s_f.sync) vk_fr_wait_all();
    pthread_mutex_unlock(&s_vk.present_mu);
    s_g.last_draws = s_g.draws;
    s_g.draws = 0;
}

static void vk_eng_present(void* user, u32 surface)
{
    vk_eng_present_region(user, surface, 0, 0, 0, 0, 0, 0);
}

/* Pixels a 2D transfer put into guest memory, pushed into the colour target
 * that rectangle belongs to. */
static void vk_eng_color_target_write(void* user, u32 surface, u32 x, u32 y,
                                      u32 w, u32 h, const void* rows, u32 row_bytes)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || (o->fmt != VK_FORMAT_R8G8B8A8_UNORM && !vk_cv_native(o->fmt))) return;   /* rows are R,G,B,A */
    if (!rows || !w || !h || x + w > o->w || y + h > o->h) return;
    u8* cv = NULL;
    if (vk_cv_native(o->fmt)) {
        if (!(cv = vk_cv_rows_in(o->fmt, o->bpp, rows, row_bytes, w, h))) return;
        rows = cv; row_bytes = w * o->bpp;
    }
    vk_op_begin();
    vk_ems_ss(surface);                  /* a partial write over the samples */
    if (o->rs > 1.0f) vk_rs_upload(o, x, y, w, h, rows, row_bytes);
    else vk_eng_upload_region(o, 0, 0, x, y, w, h, (size_t)w * o->bpp, h, rows, row_bytes);
    vk_ems_wrote_ss(surface);
    vk_op_done();
    free(cv);
}

/* RSX_VK_READBACK_LINEAR=0 keeps scaled colour readbacks point-sampled; else
 * they filter where the format can be blitted with VK_FILTER_LINEAR. */
static int vk_rs_readback_linear(VkFormat f)
{
    static int env = -1;
    if (env < 0) { const char* e = getenv("RSX_VK_READBACK_LINEAR"); env = !(e && e[0] == '0' && !e[1]); }
    if (!env) return 0;
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, f, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    return (fp.optimalTilingFeatures & need) == need;
}

/* The CPU reads the pixels at once: everything recorded is submitted and
 * waited on. */
static void vk_eng_readback(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                            void* out, u32 out_pitch)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || !out || !out_pitch || !w || !h || x + w > o->w || y + h > o->h) return;
    /* A scaled image is read at its size and point-sampled down, or (RPCS3
     * VKTextureCache.h dma_transfer: a linear copy_scaled_image for colour)
     * blitted down to guest size with a linear filter first. */
    u32 px = x, py = y, pw = w, ph = h;
    vk_rs_rect(o, &px, &py, &pw, &ph);
    if ((pw != w || ph != h) && vk_rs_readback_linear(o->fmt)) {
        const int si = vk_rs_stage_get(o->fmt, w, h);
        if (si >= 0) {
            const size_t gt = (size_t)w * o->bpp;
            vk_op_begin();
            vk_ems_ss(surface);
            if (vk_eng_readback_buffer((VkDeviceSize)gt * h)) return;
            o->fresh = 0;
            VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
            if (!cb) return;
            const VkImage st = s_rs_stage[si].im.img;
            VkImageBlit blit = {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .srcOffsets = { { (int32_t)px, (int32_t)py, 0 },
                                { (int32_t)(px + pw), (int32_t)(py + ph), 1 } },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstOffsets = { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } },
            };
            pvkCmdBlitImage(cb, o->im.img, VK_IMAGE_LAYOUT_GENERAL, st, VK_IMAGE_LAYOUT_GENERAL,
                            1, &blit, VK_FILTER_LINEAR);
            vk_rec_barrier(&s_f.main);
            s_f.main.other = 1;
            VkBufferImageCopy region = {
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageExtent = { w, h, 1 },
            };
            pvkCmdCopyImageToBuffer(cb, st, VK_IMAGE_LAYOUT_GENERAL, s_f.rb.buf, 1, &region);
            vk_eng_readback_finish(cb);
            for (u32 row = 0; row < h; row++) {
                if (vk_cv_native(o->fmt))
                    vk_cv_to_rgba(o->fmt, (const u8*)s_f.rb.ptr + (size_t)row * gt,
                                  (u8*)out + (size_t)row * out_pitch, w);
                else
                    memcpy((u8*)out + (size_t)row * out_pitch, (const u8*)s_f.rb.ptr + (size_t)row * gt, gt);
            }
            return;
        }
    }
    const size_t tight = (size_t)pw * o->bpp;
    vk_op_begin();
    vk_ems_ss(surface);
    if (vk_eng_readback_buffer((VkDeviceSize)tight * ph)) return;
    o->fresh = 0;
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (!cb) return;
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { (int32_t)px, (int32_t)py, 0 }, .imageExtent = { pw, ph, 1 },
    };
    pvkCmdCopyImageToBuffer(cb, o->im.img, VK_IMAGE_LAYOUT_GENERAL, s_f.rb.buf, 1, &region);
    vk_eng_readback_finish(cb);
    /* A native 16/8-bit target is handed back as RGBA8 lanes (vk_cv_to_rgba). */
    const int cv = vk_cv_native(o->fmt);
    if (pw == w && ph == h) {
        for (u32 row = 0; row < h; row++) {
            if (cv)
                vk_cv_to_rgba(o->fmt, (const u8*)s_f.rb.ptr + (size_t)row * tight,
                              (u8*)out + (size_t)row * out_pitch, w);
            else
                memcpy((u8*)out + (size_t)row * out_pitch, (const u8*)s_f.rb.ptr + (size_t)row * tight, tight);
        }
        return;
    }
    const u32 bpp = o->bpp;
    for (u32 row = 0; row < h; row++) {
        const u8* s = (const u8*)s_f.rb.ptr + (size_t)vk_rs_src(row, h, ph) * tight;
        u8* d = (u8*)out + (size_t)row * out_pitch;
        for (u32 i = 0; i < w; i++) {
            if (cv) vk_cv_to_rgba(o->fmt, s + (size_t)vk_rs_src(i, w, pw) * bpp, d + (size_t)i * 4u, 1);
            else memcpy(d + (size_t)i * bpp, s + (size_t)vk_rs_src(i, w, pw) * bpp, bpp);
        }
    }
}

/* ---- E3: textures ---------------------------------------------------------
 * The engine decodes every level to host rows (rsx_texture_decode) and hands
 * them over one upload each; the crossbar is a property of the view, so it
 * becomes the image view's component mapping. A cube map (faces == 6) is a
 * six-layer cube-compatible image, each face uploaded into its layer, sampled
 * through a CUBE view; the pipelines that sample one use the split set layout
 * (vk_eng_layouts). A new texture's layout move and uploads go to `pre`
 * until a draw samples it. */
static VkFormat vk_eng_srgb_twin(VkFormat f);
static u32 vk_eng_tex_new(rsx_be_format fmt, u32 w, u32 h, u32 mips, u32 faces,
                          u32 remap, u32 rsx_fmt)
{
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    const int bc = (fmt == RSX_BE_FMT_BC1 || fmt == RSX_BE_FMT_BC2 || fmt == RSX_BE_FMT_BC3);
    if (vf == VK_FORMAT_UNDEFINED || !w || !h || w > vk_max_dim() || h > vk_max_dim() ||
        (bc && !s_vk.bc_ok))
        return 0;
    const int cube = (faces == 6);
    if ((faces != 1 && !cube) || (cube && w != h)) return 0;   /* cube faces are square */
    if (!mips) mips = 1;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    /* MUTABLE_FORMAT: an UNORM image with an SNORM or SRGB twin may get a
     * signed (vk_eng_texture_snorm_view) or sRGB (vk_eng_texture_srgb_view) view. */
    const int snorm_able = !rsx_texture_legacy() &&
        (vf == VK_FORMAT_R8_UNORM || vf == VK_FORMAT_R8G8_UNORM ||
         vf == VK_FORMAT_R8G8B8A8_UNORM || vf == VK_FORMAT_R16G16_UNORM ||
         vk_eng_srgb_twin(vf) != VK_FORMAT_UNDEFINED);
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0)
               | (snorm_able ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0),
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vf, .extent = { w, h, 1 }, .mipLevels = mips, .arrayLayers = cube ? 6u : 1u,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkMemoryRequirements req;
    u32 type;
    if (pvkCreateImage(s_vk.device, &ici, NULL, &o->im.img) != VK_SUCCESS) goto fail;
    pvkGetImageMemoryRequirements(s_vk.device, o->im.img, &req);
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) goto fail;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    if (pvkAllocateMemory(s_vk.device, &mai, NULL, &o->im.mem) != VK_SUCCESS ||
        pvkBindImageMemory(s_vk.device, o->im.img, o->im.mem, 0) != VK_SUCCESS ||
        vk_eng_make_view(o->im.img, vf, mips, vk_remap(remap, rsx_fmt), cube, &o->im.view))
        goto fail;
    /* UNDEFINED -> GENERAL for every level and face before the uploads arrive. */
    if (vk_eng_new_layout(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT)) goto fail;
    o->kind = VK_ENG_TEXTURE; o->fmt = vf; o->w = w; o->h = h; o->cube = (u8)cube;
    o->mips = mips; o->remap = remap; o->rsx_fmt = rsx_fmt;
    o->bpp = bc ? 0 : bpp; o->block = bc ? bpp : 0;
    o->fresh = 1;
    return hd;
fail:
    vk_destroy_image(&o->im);         /* never recorded anywhere */
    memset(o, 0, sizeof *o);
    return 0;
}

static u32 vk_eng_texture_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                 u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{
    (void)user;
    vk_op_begin();
    const u32 hd = vk_eng_tex_new(fmt, w, h, mips, faces, remap, rsx_fmt);
    vk_op_done();
    return hd;
}

static void vk_eng_tex_upload(u32 texture, u32 face, u32 mip, u32 w, u32 h,
                              const void* src, u32 row_bytes, u32 rows)
{
    vk_eng_obj* o = vk_eng_get(texture, VK_ENG_TEXTURE);
    if (!o || !src || !row_bytes || !rows || o->cube == 2 || face >= (o->cube ? 6u : 1u)) return;
    /* A BC row is a row of 4x4 blocks; the engine's `rows` already counts them. */
    const size_t tight = o->block ? (size_t)((w + 3u) / 4u) * o->block : (size_t)w * o->bpp;
    /* A texture backed by a scaled image (a surface_reinterpret output) is
     * guest-size to the engine: its rows go up point-sampled to the image. */
    if (o->rs > 1.0f && !o->block && o->bpp && !face && !mip && tight <= row_bytes) {
        if (rows < h) h = rows;
        u32 px = 0, py = 0, pw = w, ph = h;
        vk_rs_rect(o, &px, &py, &pw, &ph);
        const size_t pt = (size_t)pw * o->bpp;
        u8* up = (u8*)malloc(pt * ph);
        if (!up) return;
        rsx_vk_rs_upscale_rows(up, pw, ph, (const u8*)src, w, h, row_bytes, o->bpp);
        vk_eng_upload_rows(o, 0, 0, pw, ph, pt, ph, up, (u32)pt);
        free(up);
        return;
    }
    vk_eng_upload_rows(o, face, mip, w, h, tight, rows, src, row_bytes);
}

static void vk_eng_texture_upload(void* user, u32 texture, u32 face, u32 mip, u32 w, u32 h,
                                  const void* src, u32 row_bytes, u32 rows)
{
    (void)user;
    vk_op_begin();
    vk_eng_tex_upload(texture, face, mip, w, h, src, row_bytes, rows);
    vk_op_done();
}

/* A volume texture (RPCS3 VKTexture image_type 3D): `depth` slices per
 * level, uploaded slice by slice; a BC volume's slices arrive un-tiled
 * (rows of blocks) from the engine. */
static u32 vk_eng_tex3d_new(rsx_be_format fmt, u32 w, u32 h, u32 depth, u32 mips,
                            u32 remap, u32 rsx_fmt)
{
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    const int bc = (fmt == RSX_BE_FMT_BC1 || fmt == RSX_BE_FMT_BC2 || fmt == RSX_BE_FMT_BC3);
    if (vf == VK_FORMAT_UNDEFINED || !w || !h || !depth || (bc && !s_vk.bc_ok) ||
        w > 2048u || h > 2048u || depth > 512u)
        return 0;
    if (!mips) mips = 1;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_3D,
        .flags = (!rsx_texture_legacy() && vk_eng_srgb_twin(vf) != VK_FORMAT_UNDEFINED)
                 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
        .format = vf, .extent = { w, h, depth }, .mipLevels = mips, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkMemoryRequirements req;
    u32 type;
    if (pvkCreateImage(s_vk.device, &ici, NULL, &o->im.img) != VK_SUCCESS) goto fail;
    pvkGetImageMemoryRequirements(s_vk.device, o->im.img, &req);
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) goto fail;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    if (pvkAllocateMemory(s_vk.device, &mai, NULL, &o->im.mem) != VK_SUCCESS ||
        pvkBindImageMemory(s_vk.device, o->im.img, o->im.mem, 0) != VK_SUCCESS ||
        vk_eng_make_view(o->im.img, vf, mips, vk_remap(remap, rsx_fmt), 2, &o->im.view))
        goto fail;
    if (vk_eng_new_layout(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT)) goto fail;
    o->kind = VK_ENG_TEXTURE; o->fmt = vf; o->w = w; o->h = h; o->cube = 2;
    o->mips = mips; o->remap = remap; o->rsx_fmt = rsx_fmt;
    o->bpp = bc ? 0 : bpp; o->block = bc ? bpp : 0;
    o->fresh = 1;
    return hd;
fail:
    vk_destroy_image(&o->im);
    memset(o, 0, sizeof *o);
    return 0;
}

static u32 vk_eng_texture_create_3d(void* user, rsx_be_format fmt, u32 w, u32 h,
                                    u32 depth, u32 mips, u32 remap, u32 rsx_fmt)
{
    (void)user;
    vk_op_begin();
    const u32 hd = vk_eng_tex3d_new(fmt, w, h, depth, mips, remap, rsx_fmt);
    vk_op_done();
    return hd;
}

static void vk_eng_tex3d_upload(u32 texture, u32 z, u32 mip, u32 w, u32 h,
                                const void* src, u32 row_bytes)
{
    vk_eng_obj* o = vk_eng_get(texture, VK_ENG_TEXTURE);
    if (o && o->cube == 2 && src && row_bytes && w && h && mip < o->mips) {
        /* A BC slice is rows of 4x4 blocks; the copy extent stays in texels. */
        const u32 rows = o->block ? (h + 3u) / 4u : h;
        size_t tight = o->block ? (size_t)((w + 3u) / 4u) * o->block : (size_t)w * o->bpp;
        if (tight > row_bytes) tight = row_bytes;
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceSize off = 0;
        u8* p = (u8*)vk_ring((VkDeviceSize)tight * rows, 16, &buf, &off);
        VkCommandBuffer cb = p ? vk_rec_upload(vk_eng_rec(o), o->im.img) : VK_NULL_HANDLE;
        if (cb) {
            for (u32 r = 0; r < rows; r++)
                memcpy(p + (size_t)r * tight, (const u8*)src + (size_t)r * row_bytes, tight);
            VkBufferImageCopy region = {
                .bufferOffset = off,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1 },
                .imageOffset = { 0, 0, (int32_t)z },
                .imageExtent = { w, h, 1 },
            };
            pvkCmdCopyBufferToImage(cb, buf, o->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        }
    }
}

static void vk_eng_texture_upload_3d(void* user, u32 texture, u32 z, u32 mip,
                                     u32 w, u32 h, const void* src, u32 row_bytes)
{
    (void)user;
    vk_op_begin();
    vk_eng_tex3d_upload(texture, z, mip, w, h, src, row_bytes);
    vk_op_done();
}

/* A colour target sampled with a unit's crossbar: an extra view of the same
 * image, cached by (surface, crossbar, format) and released with the target. */
static u32 vk_eng_surface_view(void* user, u32 surface, u32 remap, u32 rsx_format)
{
    (void)user;
    vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    if (!t) return 0;
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
        if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == surface &&
            s_eobj[i].remap == remap && s_eobj[i].rsx_fmt == rsx_format)
            return i;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    VkComponentMapping map = vk_remap(remap, rsx_format);
    if (vk_cv_native(t->fmt)) map = vk_cv_compose(map, t->fmt);   /* over the RGBA8 lanes */
    if (vk_eng_make_view(t->im.img, t->fmt, 1, map, 0, &o->im.view)) {
        memset(o, 0, sizeof *o); return 0;
    }
    o->kind = VK_ENG_VIEW; o->fmt = t->fmt; o->w = t->w; o->h = t->h;
    o->pw = t->pw; o->ph = t->ph; o->rs = t->rs;
    o->surface = surface; o->remap = remap; o->rsx_fmt = rsx_format;
    return hd;
}

/* RPCS3 VKDraw.cpp:377-395 (get_compatible_snorm_format, VKFormats.cpp:268):
 * the texture's own image and crossbar seen as SNORM. The view is a VIEW
 * object of the texture (released with it), cached in the texture's
 * otherwise unused `surface` field. */
#define VK_ENG_SNORM_TAG 0xFFFFFFFFu
static u32 vk_eng_texture_snorm_view(void* user, u32 texture)
{
    (void)user;
    vk_eng_obj* t = vk_eng_get(texture, VK_ENG_TEXTURE);
    if (!t) return 0;
    const u32 c = t->surface;
    if (c && c < VK_ENG_MAX_OBJ && s_eobj[c].kind == VK_ENG_VIEW &&
        s_eobj[c].surface == texture && s_eobj[c].rsx_fmt == VK_ENG_SNORM_TAG)
        return c;
    VkFormat sf;
    switch (t->fmt) {
    case VK_FORMAT_R8_UNORM:       sf = VK_FORMAT_R8_SNORM; break;
    case VK_FORMAT_R8G8_UNORM:     sf = VK_FORMAT_R8G8_SNORM; break;
    case VK_FORMAT_R8G8B8A8_UNORM: sf = VK_FORMAT_R8G8B8A8_SNORM; break;
    case VK_FORMAT_R16G16_UNORM:   sf = VK_FORMAT_R16G16_SNORM; break;
    default: return 0;
    }
    if (rsx_texture_legacy() || t->cube == 2) return 0;   /* the image was made without MUTABLE_FORMAT */
    {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, sf, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            static u32 n_unsupported;
            if (n_unsupported++ < 4)
                VK_LOG("engine: SNORM view format %d not sampleable; signed texture reads unsigned\n", (int)sf);
            return 0;
        }
    }
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    t = &s_eobj[texture];
    if (vk_eng_make_view(t->im.img, sf, t->mips, vk_remap(t->remap, t->rsx_fmt), t->cube,
                         &o->im.view)) {
        memset(o, 0, sizeof *o); return 0;
    }
    o->kind = VK_ENG_VIEW; o->fmt = sf; o->w = t->w; o->h = t->h; o->cube = t->cube;
    o->pw = t->pw; o->ph = t->ph; o->rs = t->rs;
    o->mips = t->mips; o->surface = texture; o->remap = 0; o->rsx_fmt = VK_ENG_SNORM_TAG;
    o->fresh = t->fresh;
    t->surface = hd;
    return hd;
}

/* RPCS3 VKDraw.cpp:383-388 (get_compatible_srgb_format, VKFormats.cpp:243):
 * the texture's own image and crossbar seen as _SRGB, so the sampler decodes
 * gamma before filtering. Cached like the SNORM view. */
#define VK_ENG_SRGB_TAG 0xFFFFFFFEu
static VkFormat vk_eng_srgb_twin(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8_UNORM:             return VK_FORMAT_R8_SRGB;
    case VK_FORMAT_R8G8_UNORM:           return VK_FORMAT_R8G8_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM:       return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK:      return VK_FORMAT_BC2_SRGB_BLOCK;
    case VK_FORMAT_BC3_UNORM_BLOCK:      return VK_FORMAT_BC3_SRGB_BLOCK;
    default:                             return VK_FORMAT_UNDEFINED;
    }
}
static int vk_eng_srgb_sampleable(VkFormat sf)
{
    static VkFormat seen[8];
    static int ok[8], n;
    for (int i = 0; i < n; i++) if (seen[i] == sf) return ok[i];
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, sf, &fp);
    const int r = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    if (!r) VK_LOG("engine: sRGB view format %d not sampleable; gamma stays in the shader\n", (int)sf);
    if (n < 8) { seen[n] = sf; ok[n] = r; n++; }
    return r;
}
static u32 vk_eng_texture_srgb_view(void* user, u32 texture, rsx_be_format fmt)
{
    (void)user;
    if (rsx_texture_legacy()) return 0;   /* images are made without MUTABLE_FORMAT */
    if (!texture) {
        u32 bpp;
        const VkFormat vf = vk_eng_format(fmt, &bpp);
        const int bc = fmt == RSX_BE_FMT_BC1 || fmt == RSX_BE_FMT_BC2 || fmt == RSX_BE_FMT_BC3;
        const VkFormat sf = vk_eng_srgb_twin(vf);
        return sf != VK_FORMAT_UNDEFINED && (!bc || s_vk.bc_ok) && vk_eng_srgb_sampleable(sf);
    }
    vk_eng_obj* t = vk_eng_get(texture, VK_ENG_TEXTURE);
    if (!t) return 0;
    const VkFormat sf = vk_eng_srgb_twin(t->fmt);
    if (sf == VK_FORMAT_UNDEFINED || !vk_eng_srgb_sampleable(sf)) return 0;
    u32 c = t->surface;
    if (!(c && c < VK_ENG_MAX_OBJ && s_eobj[c].kind == VK_ENG_VIEW &&
          s_eobj[c].surface == texture && s_eobj[c].rsx_fmt == VK_ENG_SRGB_TAG)) {
        c = 0;
        for (u32 i = 1; i < VK_ENG_MAX_OBJ && !c; i++)
            if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == texture &&
                s_eobj[i].rsx_fmt == VK_ENG_SRGB_TAG)
                c = i;
    }
    if (c) { t->surface = c; return c; }
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    t = &s_eobj[texture];
    if (vk_eng_make_view(t->im.img, sf, t->mips, vk_remap(t->remap, t->rsx_fmt), t->cube,
                         &o->im.view)) {
        memset(o, 0, sizeof *o); return 0;
    }
    o->kind = VK_ENG_VIEW; o->fmt = sf; o->w = t->w; o->h = t->h; o->cube = t->cube;
    o->pw = t->pw; o->ph = t->ph; o->rs = t->rs;
    o->mips = t->mips; o->surface = texture; o->remap = 0; o->rsx_fmt = VK_ENG_SRGB_TAG;
    o->fresh = t->fresh;
    t->surface = hd;
    return hd;
}
/* ---- colour feedback snapshots ---------------------------------------------
 * A draw that samples a colour target it is also writing (in-place deferred
 * lighting: read the albedo, write the lit colour over it) gets a copy of
 * that target taken just before it. The engine asks for one per such draw
 * (rsx_draw_backend.color_snapshot) and releases the previous one; without
 * this hook it falls back to the guest-memory bytes at the target's address,
 * which hold whatever was last stored there -- for Twisted Metal the previous
 * frame's UI, lit and composited into the next frame.
 *
 * The copy is recorded in `main`, so it is in command order: after the draws
 * before it, before the draw that samples it (the barrier at that draw's
 * render pass makes it visible). It is a COLOR object marked `snapshot` so
 * the engine can take a crossbar view of it (vk_eng_surface_view); it is
 * never an attachment. Released copies keep their images in a small pool,
 * since a lit frame asks for one per light; a recycled image's next copy is
 * recorded after every command that sampled it, so reuse needs no wait. */
#define VK_SNAP_POOL 8
static struct { vk_image im; VkFormat fmt; u32 w, h; } s_snap_pool[VK_SNAP_POOL];
static u32 s_snap_pool_n;

static void vk_eng_snapshot_recycle(vk_eng_obj* o)
{
    if (s_snap_pool_n < VK_SNAP_POOL && o->im.img) {
        s_snap_pool[s_snap_pool_n].im = o->im;
        s_snap_pool[s_snap_pool_n].fmt = o->fmt;
        s_snap_pool[s_snap_pool_n].w = vk_pw(o);
        s_snap_pool[s_snap_pool_n].h = vk_ph(o);
        s_snap_pool_n++;
    } else {
        vk_defer_image(&o->im);
    }
}

static void vk_eng_snapshot_pool_drain(void)
{
    for (u32 i = 0; i < s_snap_pool_n; i++) vk_defer_image(&s_snap_pool[i].im);
    s_snap_pool_n = 0;
}

/* A w x h copy of the rectangle at (x, y) of colour target `surface`. */
static u32 vk_eng_snapshot_take(u32 surface, u32 x, u32 y, u32 w, u32 h)
{
    vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    if (!t || t->snapshot || !w || !h || x >= t->w || y >= t->h || w > t->w - x || h > t->h - y)
        return 0;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_op_begin();
    vk_eng_obj* o = &s_eobj[hd];
    u32 px = x, py = y, pw = w, ph = h;
    vk_rs_rect(t, &px, &py, &pw, &ph);
    int reused = 0;
    for (u32 i = 0; i < s_snap_pool_n; i++)
        if (s_snap_pool[i].fmt == t->fmt && s_snap_pool[i].w == pw && s_snap_pool[i].h == ph) {
            o->im = s_snap_pool[i].im;
            s_snap_pool[i] = s_snap_pool[--s_snap_pool_n];
            reused = 1;
            break;
        }
    if (!reused) {
        if (vk_create_image(&o->im, t->fmt, pw, ph,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT) ||
            vk_eng_new_layout(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT)) {
            vk_destroy_image(&o->im);
            memset(o, 0, sizeof *o);
            return 0;
        }
    }
    if (vk_cv_native(t->fmt) &&
        vk_eng_make_view(o->im.img, t->fmt, 1, vk_cv_base_map(t->fmt), 0, &o->sv.view)) {
        vk_defer_image(&o->im);
        memset(o, 0, sizeof *o);
        return 0;
    }
    o->kind = VK_ENG_COLOR; o->snapshot = 1;
    o->fmt = t->fmt; o->bpp = t->bpp; o->w = w; o->h = h;
    if (t->rs > 1.0f) { o->pw = pw; o->ph = ph; o->rs = t->rs; }
    vk_ems_ss(surface);
    o->fresh = 0;
    t->fresh = 0;
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (!cb) { vk_eng_release(NULL, hd); return 0; }
    VkImageCopy c = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffset = { (int32_t)px, (int32_t)py, 0 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { pw, ph, 1 },
    };
    pvkCmdCopyImage(cb, t->im.img, VK_IMAGE_LAYOUT_GENERAL, o->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    vk_op_done();
    return hd;
}

static u32 vk_eng_color_snapshot(void* user, u32 surface)
{
    (void)user;
    const vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    return t ? vk_eng_snapshot_take(surface, 0, 0, t->w, t->h) : 0;
}

static u32 vk_eng_color_region_snapshot(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    return vk_eng_snapshot_take(surface, x, y, w, h);
}

/* RPCS3 texture_cache.h:2341-2440 (cubemap_unwrap / _3d_unwrap expanded to
 * gathers; VKTextureCache.cpp generate_cubemap_from_images /
 * generate_3d_from_2d_images): each face / slice copied out of the target on
 * the GPU. A scaled target is point-sampled back to guest size. */
static int vk_eng_texture_copy_from_color(void* user, u32 texture, u32 layer, u32 mip,
                                          u32 surface, u32 sx, u32 sy, u32 w, u32 h)
{
    (void)user;
    vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    vk_eng_obj* o = vk_eng_get(texture, VK_ENG_TEXTURE);
    if (!t || !o || t->snapshot || !w || !h || t->fmt != o->fmt || o->block ||
        vk_cv_native(t->fmt) || mip >= o->mips || layer >= (o->cube == 1 ? 6u : o->cube == 2 ? 512u : 1u) ||
        sx >= t->w || sy >= t->h || w > t->w - sx || h > t->h - sy)
        return 0;
    const u32 mw = o->w >> mip ? o->w >> mip : 1u, mh = o->h >> mip ? o->h >> mip : 1u;
    if (w > mw || h > mh) return 0;
    vk_op_begin();
    vk_ems_ss(surface);
    o->fresh = 0;
    t->fresh = 0;
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (!cb) { vk_op_done(); return 0; }
    const VkImageSubresourceLayers dsub = { VK_IMAGE_ASPECT_COLOR_BIT, mip,
                                            o->cube == 1 ? layer : 0u, 1 };
    const int32_t dz = o->cube == 2 ? (int32_t)layer : 0;
    if (t->rs > 1.0f) {
        u32 px = sx, py = sy, pw = w, ph = h;
        vk_rs_rect(t, &px, &py, &pw, &ph);
        VkImageBlit b = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = { { (int32_t)px, (int32_t)py, 0 },
                            { (int32_t)(px + pw), (int32_t)(py + ph), 1 } },
            .dstSubresource = dsub,
            .dstOffsets = { { 0, 0, dz }, { (int32_t)w, (int32_t)h, dz + 1 } },
        };
        pvkCmdBlitImage(cb, t->im.img, VK_IMAGE_LAYOUT_GENERAL, o->im.img,
                        VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    } else {
        VkImageCopy c = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffset = { (int32_t)sx, (int32_t)sy, 0 },
            .dstSubresource = dsub,
            .dstOffset = { 0, 0, dz },
            .extent = { w, h, 1 },
        };
        pvkCmdCopyImage(cb, t->im.img, VK_IMAGE_LAYOUT_GENERAL, o->im.img,
                        VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    vk_op_done();
    return 1;
}

/* RPCS3 (R2 patch, VKTexture.cpp blit): an out-of-bounds blit rectangle is
 * clipped to its image and the other side's rectangle is cut by the same
 * proportion, so the scale is kept; clamping both independently stretched the
 * picture instead. 0 when nothing is left. */
static int vk_eng_clip_axis(u32* sp, u32* sl, u32 smax, u32* dp, u32* dl, u32 dmax)
{
    double s0 = *sp, s1 = (double)*sp + *sl, d0 = *dp, d1 = (double)*dp + *dl;
    if (s1 <= s0 || d1 <= d0) return 0;
    const double k = (s1 - s0) / (d1 - d0);
    if (d1 > dmax) { s1 -= (d1 - dmax) * k; d1 = dmax; }
    if (s1 > smax) { d1 -= (s1 - smax) / k; s1 = smax; }
    if (d0 >= d1 || s0 >= s1) return 0;
    const u32 is0 = (u32)(s0 + 0.5), is1 = (u32)(s1 + 0.5);
    const u32 id0 = (u32)(d0 + 0.5), id1 = (u32)(d1 + 0.5);
    if (is1 <= is0 || id1 <= id0) return 0;
    *sp = is0; *sl = is1 - is0; *dp = id0; *dl = id1 - id0;
    return 1;
}

static int vk_eng_clip_blit(u32* sx, u32* sy, u32* sw, u32* sh, u32 sW, u32 sH,
                            u32* dx, u32* dy, u32* dw, u32* dh, u32 dW, u32 dH)
{
    if (*sx >= sW || *sy >= sH || *dx >= dW || *dy >= dH) return 0;
    return vk_eng_clip_axis(sx, sw, sW, dx, dw, dW) && vk_eng_clip_axis(sy, sh, sH, dy, dh, dH);
}

/* RPCS3 render_target::memory_barrier for a depth surface's old contents:
 * depth and stencil of one depth target's rectangle onto another's, nearest
 * (scale_image, linear = false). Same host format only: a copy when the
 * extents agree, a blit where the format can be blitted; anything else is
 * RPCS3's typeless path, reported as not done. */
static int vk_eng_depth_target_copy(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                                    u32 dst, u32 dx, u32 dy, u32 dw, u32 dh)
{
    (void)user;
    if (src == dst) return 0;
    vk_eng_obj* s = vk_eng_get(src, VK_ENG_DEPTH);
    vk_eng_obj* d = vk_eng_get(dst, VK_ENG_DEPTH);
    if (!s || !d || s->fmt != d->fmt) return 0;
    if (!vk_eng_clip_blit(&sx, &sy, &sw, &sh, s->w, s->h, &dx, &dy, &dw, &dh, d->w, d->h))
        return 0;
    vk_rs_rect(s, &sx, &sy, &sw, &sh);
    vk_rs_rect(d, &dx, &dy, &dw, &dh);
    const int same = sw == dw && sh == dh;
    const VkFormatFeatureFlags ff = vk_depth_features(s->fmt);
    if (!same && (ff & (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) !=
                     (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return 0;
    vk_op_begin();
    vk_ems_ss(src);
    vk_ems_ss(dst);
    vk_ems_wrote_ss(dst);
    s->fresh = 0;
    d->fresh = 0;
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (!cb) { vk_op_done(); return 0; }
    const VkImageAspectFlags a = vk_depth_aspect_of(s->fmt);
    if (same) {
        VkImageCopy c = { .srcSubresource = { a, 0, 0, 1 },
                          .srcOffset = { (int32_t)sx, (int32_t)sy, 0 },
                          .dstSubresource = { a, 0, 0, 1 },
                          .dstOffset = { (int32_t)dx, (int32_t)dy, 0 },
                          .extent = { sw, sh, 1 } };
        pvkCmdCopyImage(cb, s->im.img, VK_IMAGE_LAYOUT_GENERAL, d->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    } else {
        VkImageBlit blit = {
            .srcSubresource = { a, 0, 0, 1 },
            .srcOffsets = { { (int32_t)sx, (int32_t)sy, 0 },
                            { (int32_t)(sx + sw), (int32_t)(sy + sh), 1 } },
            .dstSubresource = { a, 0, 0, 1 },
            .dstOffsets = { { (int32_t)dx, (int32_t)dy, 0 },
                            { (int32_t)(dx + dw), (int32_t)(dy + dh), 1 } },
        };
        pvkCmdBlitImage(cb, s->im.img, VK_IMAGE_LAYOUT_GENERAL, d->im.img, VK_IMAGE_LAYOUT_GENERAL,
                        1, &blit, VK_FILTER_NEAREST);
    }
    vk_op_done();
    return 1;
}

/* A scaled, linearly filtered copy of one colour target's rectangle onto
 * another's, in command order. Images stay in GENERAL, which blits accept;
 * the global barriers order it after the draws before and before the next. */
static void vk_eng_color_target_blit(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                                     u32 dst, u32 dx, u32 dy, u32 dw, u32 dh)
{
    (void)user;
    if (src == dst) return;
    vk_eng_obj* s = vk_eng_get(src, VK_ENG_COLOR);
    vk_eng_obj* d = vk_eng_get(dst, VK_ENG_COLOR);
    if (!s || !d || d->snapshot) return;
    /* R8 / R8G8 hold B8 / G8B8 in other channels than the RGBA8 lanes. */
    if (s->fmt != d->fmt && (s->fmt == VK_FORMAT_R8_UNORM || s->fmt == VK_FORMAT_R8G8_UNORM ||
                             d->fmt == VK_FORMAT_R8_UNORM || d->fmt == VK_FORMAT_R8G8_UNORM))
        return;
    if (!vk_eng_clip_blit(&sx, &sy, &sw, &sh, s->w, s->h, &dx, &dy, &dw, &dh, d->w, d->h))
        return;
    vk_rs_rect(s, &sx, &sy, &sw, &sh);
    vk_rs_rect(d, &dx, &dy, &dw, &dh);
    vk_op_begin();
    vk_ems_ss(src);
    vk_ems_ss(dst);
    vk_ems_wrote_ss(dst);
    s->fresh = 0;
    d->fresh = 0;
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (cb) {
        VkImageBlit blit = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = { { (int32_t)sx, (int32_t)sy, 0 },
                            { (int32_t)(sx + sw), (int32_t)(sy + sh), 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = { { (int32_t)dx, (int32_t)dy, 0 },
                            { (int32_t)(dx + dw), (int32_t)(dy + dh), 1 } },
        };
        pvkCmdBlitImage(cb, s->im.img, VK_IMAGE_LAYOUT_GENERAL, d->im.img, VK_IMAGE_LAYOUT_GENERAL,
                        1, &blit, VK_FILTER_LINEAR);
    }
    vk_op_done();
}

/* ---- E4: depth snapshots --------------------------------------------------
 * A depth target read as a texture, which the engine asks for only after a
 * depth-writing draw. Metal resolves it with a fullscreen pass into R32Float;
 * Vulkan can do it with two copies on the GPU, since a D32_SFLOAT depth
 * aspect copies out as 32-bit floats: depth -> buffer (a slice of the frame
 * ring) -> an R32_SFLOAT texture. Other depth formats would need a conversion
 * pass: they report 0.
 *
 * Each depth target keeps ONE snapshot texture (its handle in `surface`),
 * refilled on every request, so nothing accumulates however often the engine
 * invalidates and asks again; it goes when the depth target goes. */
static u32 vk_eng_depth_snapshot(void* user, u32 depth, u32 w, u32 h)
{
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z) return 0;
    /* D32 / D32S8 copy out as floats and D16 as unorm16 (into R32F / R16
     * textures through the ring); D24S8 has no colour twin, so it is copied
     * into a sampled D24S8 texture read through its depth aspect. */
    const int zf32 = z->fmt == VK_FORMAT_D32_SFLOAT || z->fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
    const int zf16 = z->fmt == VK_FORMAT_D16_UNORM;
    const VkFormat sfmt = zf32 ? VK_FORMAT_R32_SFLOAT : zf16 ? VK_FORMAT_R16_UNORM : z->fmt;
    if (!zf32 && !zf16 && !(vk_depth_features(z->fmt) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        static int warned_fmt;
        if (!warned_fmt) { VK_LOG("engine: depth snapshots of format %d are not sampleable here\n",
                                  (int)z->fmt); warned_fmt = 1; }
        return 0;
    }
    if (w > z->w) w = z->w;
    if (h > z->h) h = z->h;
    if (!w || !h) return 0;
    /* A scaled target's snapshot is a scaled texture (same rs). */
    const u32 gw = w, gh = h;
    w = vk_rs_c(z, w, vk_pw(z));
    h = vk_rs_c(z, h, vk_ph(z));
    vk_op_begin();
    vk_ems_ss(depth);

    u32 snap = z->surface;
    vk_eng_obj* t = vk_eng_get(snap, VK_ENG_TEXTURE);
    if (!t || t->w != gw || t->h != gh || vk_pw(t) != w || vk_ph(t) != h || t->fmt != sfmt) {
        if (t) vk_eng_release(user, snap);
        z->surface = 0;
        snap = vk_eng_alloc();
        if (!snap) return 0;
        t = &s_eobj[snap];
        const VkImageAspectFlags sa = (zf32 || zf16) ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        if (vk_create_image(&t->im, sfmt, w, h,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            sa) ||
            vk_eng_new_layout(t->im.img, (zf32 || zf16) ? sa : vk_depth_aspect_of(sfmt))) {
            vk_destroy_image(&t->im); memset(t, 0, sizeof *t); return 0;
        }
        t->kind = VK_ENG_TEXTURE; t->fmt = sfmt; t->bpp = zf16 ? 2 : 4; t->w = gw; t->h = gh;
        if (z->rs > 1.0f) { t->pw = w; t->ph = h; t->rs = z->rs; }
        z->surface = snap;
    }
    if (!zf32 && !zf16) {
        z->fresh = 0;
        t->fresh = 0;
        VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
        if (!cb) return 0;
        const VkImageAspectFlags fa = vk_depth_aspect_of(sfmt);
        VkImageCopy c = { .srcSubresource = { fa, 0, 0, 1 }, .dstSubresource = { fa, 0, 0, 1 },
                          .extent = { w, h, 1 } };
        pvkCmdCopyImage(cb, z->im.img, VK_IMAGE_LAYOUT_GENERAL, t->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        vk_op_done();
        return snap;
    }

    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    if (!vk_ring((VkDeviceSize)w * h * (zf16 ? 2u : 4u), 16, &buf, &off)) return 0;
    z->fresh = 0;
    t->fresh = 0;
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (!cb) return 0;
    VkBufferImageCopy out = { .bufferOffset = off,
                              .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
                              .imageExtent = { w, h, 1 } };
    pvkCmdCopyImageToBuffer(cb, z->im.img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &out);
    vk_rec_barrier(&s_f.main);
    s_f.main.other = 1;
    VkBufferImageCopy in = { .bufferOffset = off,
                             .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                             .imageExtent = { w, h, 1 } };
    pvkCmdCopyBufferToImage(cb, buf, t->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &in);
    vk_op_done();
    return snap;
}
/* The depth aspect of a D32_SFLOAT target copies out as 32-bit floats, which
 * is what the engine asks for; other depth formats report failure. The CPU
 * reads it at once: submitted and waited on. */
static int vk_eng_depth_readback(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h,
                                 float* out, u32 out_pitch)
{
    (void)user;
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z || !out || !w || !h || x + w > z->w || y + h > z->h || out_pitch < w * 4u)
        return -1;
    /* Depth-aspect copies: floats from D32, unorm16 from D16, and D24 in the
     * low 24 bits of a word (the top 8 undefined). */
    const int zf16 = z->fmt == VK_FORMAT_D16_UNORM;
    const int zd24 = z->fmt == VK_FORMAT_D24_UNORM_S8_UINT || z->fmt == VK_FORMAT_X8_D24_UNORM_PACK32;
    const u32 tb = zf16 ? 2u : 4u;
    u32 px = x, py = y, pw = w, ph = h;     /* scaled: read at size, point-sampled down */
    vk_rs_rect(z, &px, &py, &pw, &ph);
    const int down = pw != w || ph != h;
    vk_op_begin();
    vk_ems_ss(depth);
    if (vk_eng_readback_buffer((VkDeviceSize)pw * ph * tb)) return -1;
    z->fresh = 0;
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (!cb) return -1;
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
        .imageOffset = { (int32_t)px, (int32_t)py, 0 }, .imageExtent = { pw, ph, 1 },
    };
    pvkCmdCopyImageToBuffer(cb, z->im.img, VK_IMAGE_LAYOUT_GENERAL, s_f.rb.buf, 1, &region);
    vk_eng_readback_finish(cb);
    if (w >= 64u && vk_ems_log_left()) {
        float v0; memcpy(&v0, s_f.rb.ptr, 4);
        VK_LOG("ems: depth_readback h%u %u,%u %ux%u (phys %ux%u) fmt %d st %u first %g\n",
               depth, x, y, w, h, pw, ph, (int)z->fmt, vk_ems_state_of(depth), v0);
    }
    for (u32 row = 0; row < h; row++) {
        float* d = (float*)((u8*)out + (size_t)row * out_pitch);
        const u8* s = (const u8*)s_f.rb.ptr + (size_t)(down ? vk_rs_src(row, h, ph) : row) * pw * tb;
        if (down) {
            for (u32 i = 0; i < w; i++) {
                const u8* e = s + (size_t)vk_rs_src(i, w, pw) * tb;
                if (zf16) { u16 v; memcpy(&v, e, 2); d[i] = (float)v / 65535.0f; }
                else if (zd24) { u32 v; memcpy(&v, e, 4); d[i] = (float)((double)(v & 0xFFFFFFu) / 16777215.0); }
                else memcpy(&d[i], e, 4);
            }
        } else if (zf16) {
            for (u32 i = 0; i < w; i++) { u16 v; memcpy(&v, s + i * 2u, 2); d[i] = (float)v / 65535.0f; }
        } else if (zd24) {
            for (u32 i = 0; i < w; i++) {
                u32 v; memcpy(&v, s + i * 4u, 4);
                d[i] = (float)((double)(v & 0xFFFFFFu) / 16777215.0);
            }
        } else {
            memcpy(d, s, (size_t)w * 4u);
        }
    }
    return 0;
}

/* The stencil aspect of a D24S8 / D32S8 depth target, one byte a pixel. */
static int vk_eng_stencil_readback(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h,
                                   u8* out, u32 out_pitch)
{
    (void)user;
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z || !out || !w || !h || x + w > z->w || y + h > z->h || out_pitch < w)
        return -1;
    if (z->fmt != VK_FORMAT_D24_UNORM_S8_UINT && z->fmt != VK_FORMAT_D32_SFLOAT_S8_UINT &&
        z->fmt != VK_FORMAT_D16_UNORM_S8_UINT)
        return -1;
    u32 px = x, py = y, pw = w, ph = h;
    vk_rs_rect(z, &px, &py, &pw, &ph);
    const int down = pw != w || ph != h;
    vk_op_begin();
    vk_ems_ss(depth);
    if (vk_eng_readback_buffer((VkDeviceSize)pw * ph)) return -1;
    z->fresh = 0;
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (!cb) return -1;
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 },
        .imageOffset = { (int32_t)px, (int32_t)py, 0 }, .imageExtent = { pw, ph, 1 },
    };
    pvkCmdCopyImageToBuffer(cb, z->im.img, VK_IMAGE_LAYOUT_GENERAL, s_f.rb.buf, 1, &region);
    vk_eng_readback_finish(cb);
    for (u32 row = 0; row < h; row++) {
        u8* d = out + (size_t)row * out_pitch;
        const u8* s = (const u8*)s_f.rb.ptr + (size_t)(down ? vk_rs_src(row, h, ph) : row) * pw;
        if (down) for (u32 i = 0; i < w; i++) d[i] = s[vk_rs_src(i, w, pw)];
        else memcpy(d, s, w);
    }
    return 0;
}

/* depth_readback + stencil_readback in one submission and one wait, with
 * D16 / D24 handed back raw (the engine divides, or stores them as they are). */
static int vk_eng_zeta_readback(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h,
                                float* out_f, u32* out_raw, u32* raw_bits,
                                u8* out_stencil, int* have_stencil)
{
    (void)user;
    if (raw_bits) *raw_bits = 0;
    if (have_stencil) *have_stencil = 0;
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z || !out_f || !raw_bits || !have_stencil || !w || !h || x + w > z->w || y + h > z->h)
        return -1;
    const int zf16 = z->fmt == VK_FORMAT_D16_UNORM;
    const int zd24 = z->fmt == VK_FORMAT_D24_UNORM_S8_UINT || z->fmt == VK_FORMAT_X8_D24_UNORM_PACK32;
    const int st = out_stencil && (z->fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
                                   z->fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                                   z->fmt == VK_FORMAT_D16_UNORM_S8_UINT);
    const u32 tb = zf16 ? 2u : 4u;
    u32 px = x, py = y, pw = w, ph = h;
    vk_rs_rect(z, &px, &py, &pw, &ph);
    const int down = pw != w || ph != h;
    const VkDeviceSize zbytes = (VkDeviceSize)pw * ph * tb;
    const VkDeviceSize soff = (zbytes + 15u) & ~(VkDeviceSize)15u;
    vk_op_begin();
    vk_ems_ss(depth);
    if (vk_eng_readback_buffer(st ? soff + (VkDeviceSize)pw * ph : zbytes)) return -1;
    z->fresh = 0;
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (!cb) return -1;
    VkBufferImageCopy region[2] = {
        { .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
          .imageOffset = { (int32_t)px, (int32_t)py, 0 }, .imageExtent = { pw, ph, 1 } },
        { .bufferOffset = soff,
          .imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 },
          .imageOffset = { (int32_t)px, (int32_t)py, 0 }, .imageExtent = { pw, ph, 1 } },
    };
    pvkCmdCopyImageToBuffer(cb, z->im.img, VK_IMAGE_LAYOUT_GENERAL, s_f.rb.buf, st ? 2u : 1u, region);
    vk_eng_readback_finish(cb);
    const int raw = out_raw && (zf16 || zd24);
    if (raw) *raw_bits = zf16 ? 16u : 24u;
    const u8* base = (const u8*)s_f.rb.ptr;
    for (u32 row = 0; row < h; row++) {
        const u8* s = base + (size_t)(down ? vk_rs_src(row, h, ph) : row) * pw * tb;
        if (raw) {
            u32* d = out_raw + (size_t)row * w;
            if (zf16) {
                for (u32 i = 0; i < w; i++) {
                    u16 v; memcpy(&v, s + (size_t)(down ? vk_rs_src(i, w, pw) : i) * 2u, 2); d[i] = v;
                }
            } else if (down) {
                for (u32 i = 0; i < w; i++) {
                    memcpy(&d[i], s + (size_t)vk_rs_src(i, w, pw) * 4u, 4);
                    d[i] &= 0xFFFFFFu;
                }
            } else {
                /* A D24 depth-aspect copy leaves the top 8 bits undefined. */
                for (u32 i = 0; i < w; i++) { memcpy(&d[i], s + (size_t)i * 4u, 4); d[i] &= 0xFFFFFFu; }
            }
        } else {
            float* d = out_f + (size_t)row * w;
            for (u32 i = 0; i < w; i++) {
                const u8* e = s + (size_t)(down ? vk_rs_src(i, w, pw) : i) * tb;
                if (zf16) { u16 v; memcpy(&v, e, 2); d[i] = (float)v / 65535.0f; }
                else if (zd24) { u32 v; memcpy(&v, e, 4); d[i] = (float)((double)(v & 0xFFFFFFu) / 16777215.0); }
                else memcpy(&d[i], e, 4);
            }
        }
        if (st) {
            u8* d = out_stencil + (size_t)row * w;
            const u8* ss = base + soff + (size_t)(down ? vk_rs_src(row, h, ph) : row) * pw;
            if (down) for (u32 i = 0; i < w; i++) d[i] = ss[vk_rs_src(i, w, pw)];
            else memcpy(d, ss, w);
        }
    }
    *have_stencil = st;
    return 0;
}
/* ---- E2: pipelines and draws ---------------------------------------------
 * A pipeline handle carries the two translated modules and the render state
 * the engine keyed it on. Topology is not part of that key (Metal sets it on
 * the encoder), but Vulkan 1.0 bakes it into the pipeline, so each handle
 * builds its VkPipeline per topology on first use.
 *
 * Bindings are the translator's contract (rsx_shader_spirv.h), and the set
 * layout, pipeline layout, descriptor set and constant UBO are the ones the
 * vtable guest path already creates. Vertex inputs are the engine's compact
 * layout: slot n is a float4 at n*16, at location n, as Metal reads it.
 * Known gaps, stated rather than guessed: vertex-texture units (their
 * samplers s0..s3 would alias the pixel samplers' binding), colour targets
 * other than RGBA8 and RGBA16F, depth-only passes, and stencil (the depth format
 * has no stencil aspect yet, so the test is inert). */
/* As many as the engine keeps (ENG_MAX_PIPELINES in rsx_draw_engine.c). God of
 * War: Ascension selects ~740 distinct fragment programs in its first level on
 * top of the menu's; with 512 slots every later pipeline failed silently and the
 * level drew nothing but its clears. */
#define VK_ENG_MAX_PIPE   8192
#define VK_ENG_TOPOLOGIES 5
#define VK_ENG_FB_CACHE   128

/* Atomics for the asynchronous pipeline compiler (vk_async_*). */
#ifdef _WIN32
typedef volatile LONG vk_atomic;
#define VK_AT_LOAD(p)      InterlockedCompareExchange((p), 0, 0)
#define VK_AT_STORE(p, v)  ((void)InterlockedExchange((p), (LONG)(v)))
#define VK_AT_ADD(p, v)    InterlockedExchangeAdd((p), (LONG)(v))
#else
typedef volatile long vk_atomic;
#define VK_AT_LOAD(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define VK_AT_STORE(p, v)  __atomic_store_n((p), (long)(v), __ATOMIC_RELEASE)
#define VK_AT_ADD(p, v)    __atomic_fetch_add((p), (long)(v), __ATOMIC_ACQ_REL)
#endif

typedef struct vk_eng_pipe {
    int                 used;
    VkShaderModule      vs, fs;
    rsx_be_render_state rs;
    u32                 nslots, stride, rt_count;
    VkFormat            rt_fmt;          /* picks the render pass it is built against */
    u32                 cube_mask;       /* TextureCube units (0-15), Texture3D units (16-31): the split set layout */
    u32                 vcube_mask;      /* vertex units: TextureCube (0-3), Texture3D (16-19) */
    VkPipeline          variant[VK_ENG_TOPOLOGIES];
    /* Async compilation: VK_AS_* of the two modules and of each topology's
     * variant, the HLSL kept until a worker translated it, and the jobs
     * queued or running for this slot. All zero on the synchronous path. */
    vk_atomic           mstate;
    vk_atomic           vstate[VK_ENG_TOPOLOGIES];
    char               *vs_src, *ps_src;
    vk_atomic           inflight;
    /* Variants for the other pass classes (zeta format x MSAA samples,
     * vk_eng_variant_pc), [pc - 1][topology]; allocated on first need. */
    VkPipeline*         msv;
    vk_atomic*          msvstate;
    /* Persistence (vk_vfile_*): content key, and the variants this pipeline
     * was linked for (bit vkps_variant_bit(pc, topology)) here or in an
     * earlier run. Modules are owned by the shared module cache (vk_mod_*). */
    u64                 pkey, vmask;
} vk_eng_pipe;

static vk_eng_pipe s_epipe[VK_ENG_MAX_PIPE];

/* fp_rscale[5] (rsx_fp_decompiler): 1/scale of the target, then of units 0-15. */
#define VK_RS_PS_EXTRA 80u
/* vp_branch.w: the register point size (rsx_draw_engine's vertex constants). */
#define VK_RS_PSZ_OFF ((512u + 2u) * 16u + 12u)
/* vp_posscale.w: the multiplier for a point size the vertex program writes */
#define VK_RS_PSZ_SCALE_OFF (512u * 16u + 12u)

static struct {
    VkRenderPass rp[VK_ENG_RT_FORMATS][RSX_BE_MAX_COLOR_TARGETS + 1]; /* by format, colour count */
    struct { u32 key[RSX_BE_MAX_COLOR_TARGETS + 1]; VkRenderPass rp; VkFramebuffer fb; } fb[VK_ENG_FB_CACHE];
    u32          fb_count;
    vk_image     scratch_depth;                          /* when no zeta is bound */
    u32          scratch_w, scratch_h;
    u32          pipeline;
    u32          vs_bytes, ps_bytes;
    u32          tex[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samp[RSX_BE_MAX_TEXTURES];
    u32          tex_mask;
    float        vp[4];
    u32          sc[4];
    int          have_vp, have_sc;
    float        zrange[2];        /* set_depth_range (CLIP_MIN/MAX); unset: [0, 1] */
    int          have_zrange;
    u32          stencil_ref;
    u32          back_ref;            /* 0x100 | ref once set_raster_dynamic ran */
    float        dbias[3];            /* enable, constant bias, slope scale */
    float        line_width;
    float        depth_bounds[2];     /* min, max for the next draw */
    float        blend_const[4];      /* SET_BLEND_COLOR as RGBA floats */
    u32          warned;
    /* The set layouts and pipeline layouts (vk_eng_layouts): [0] shared,
     * [1] split, for pipelines with cube units; the 1x1 cube a cube unit
     * with nothing bound samples. */
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout      pipe_layout[2];
    u32                   dummy_cube;
    u32                   dummy_3d;
    /* Constants as last bound, and their slice of the frame ring. */
    u8           vs_data[VK_VPCONST_BYTES + 256];
    u8           ps_data[(RSX_FP_MAX_INLINE_CONSTANTS + 1) * 16 + VK_RS_PS_EXTRA];
    u32          ps_extra;               /* fp_rscale bytes past ps_bytes (s_rs.dyn) */
    int          psz_scaled;             /* vs_data's point size is multiplied */
    float        psz_orig;
    int          vs_dirty, ps_dirty;
    u32          vs_epoch, ps_epoch;
    VkBuffer     vs_buf, ps_buf;
    VkDeviceSize vs_off, ps_off;
    /* Vertex texture units (bind_vertex_textures), sampled by the VP's TXL
     * through VK_ENG_VTEX_TEX_BINDING / VK_ENG_VTEX_SAMP_BINDING. */
    u32          vtex[RSX_BE_MAX_VERTEX_TEXTURES];
    rsx_be_sampler_desc vsamp[RSX_BE_MAX_VERTEX_TEXTURES];
    u32          vtex_mask;
} s_e2;

/* The VP declares vertex unit N as register(t16+N) / register(sN). t16+N
 * already lands past the fragment textures (bindings 16..31); its sampler is
 * moved to s16+N (vk_eng_vs_rebind) so it does not alias the fragment
 * sampler array at RSX_SPIRV_SAMPLER_BINDING. */
#define VK_ENG_VTEX_TEX_BINDING  (RSX_SPIRV_TEXTURE_BINDING + 16u)
#define VK_ENG_VTEX_SAMP_BINDING (RSX_SPIRV_SAMPLER_BINDING + 16u)

#define VK_ENG_SAMP_CACHE 256
static struct { u64 key; u32 bkey; u8 aniso; VkSampler s; } s_esamp[VK_ENG_SAMP_CACHE];
static u32 s_esamp_count;

/* Forget the framebuffers that use engine handle `handle` as an attachment
 * (0: all of them); they are destroyed once nothing in flight uses them. */
static void vk_eng_drop_framebuffers_for(u32 handle)
{
    u32 k = 0;
    for (u32 i = 0; i < s_e2.fb_count; i++) {
        int hit = !handle;
        for (u32 j = 0; j <= RSX_BE_MAX_COLOR_TARGETS && !hit; j++) hit = s_e2.fb[i].key[j] == handle;
        if (!hit) { s_e2.fb[k++] = s_e2.fb[i]; continue; }
        if (s_f.pass && s_f.pass_fb == s_e2.fb[i].fb) vk_pass_end();
        vk_defer_fb(s_e2.fb[i].fb);
    }
    s_e2.fb_count = k;
}

/* The render pass for `n` colour attachments of format `cf` plus depth,
 * LOAD/STORE in GENERAL like the vtable path's: clears are separate
 * operations. An MRT set shares target A's format (engine contract). */
static int vk_eng_rp_build(VkFormat cf, u32 n, VkFormat df, VkSampleCountFlagBits ns, VkRenderPass* out);
static int vk_ems_enabled(void);
static VkRenderPass vk_eng_render_pass(VkFormat cf, u32 n)
{
    const int fi = vk_eng_rt_index(cf);
    if (fi < 0 || n < 1 || n > RSX_BE_MAX_COLOR_TARGETS) return VK_NULL_HANDLE;
    if (s_e2.rp[fi][n]) return s_e2.rp[fi][n];
    vk_dcls_setup();
    vk_ems_enabled();
    vk_eng_rp_build(cf, n, s_dcls_fmt[0], VK_SAMPLE_COUNT_1_BIT, &s_e2.rp[fi][n]);
    return s_e2.rp[fi][n];
}
/* Pass class pc = depth class * 3 + sample index (1/2/4 samples -> 0/1/2):
 * the zeta format and MSAA sample count a pass and its pipelines share.
 * Class 0 is vk_eng_render_pass's. */
#define VK_EMS_PC 12
static VkRenderPass s_ms_rp[VK_ENG_RT_FORMATS][RSX_BE_MAX_COLOR_TARGETS + 1][VK_EMS_PC];
static const VkSampleCountFlagBits s_pc_samples[3] = {
    VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_4_BIT };
static VkRenderPass vk_eng_render_pass_pc(VkFormat cf, u32 n, u32 pc)
{
    if (pc == 0 || pc >= VK_EMS_PC) return vk_eng_render_pass(cf, n);
    const int fi = vk_eng_rt_index(cf);
    if (fi < 0 || n < 1 || n > RSX_BE_MAX_COLOR_TARGETS) return VK_NULL_HANDLE;
    if (s_ms_rp[fi][n][pc]) return s_ms_rp[fi][n][pc];
    vk_dcls_setup();
    vk_eng_rp_build(cf, n, s_dcls_fmt[pc / 3], s_pc_samples[pc % 3], &s_ms_rp[fi][n][pc]);
    return s_ms_rp[fi][n][pc];
}
static int vk_eng_rp_build(VkFormat cf, u32 n, VkFormat df, VkSampleCountFlagBits ns, VkRenderPass* out)
{
    VkAttachmentDescription att[RSX_BE_MAX_COLOR_TARGETS + 1];
    VkAttachmentReference   cref[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i <= n; i++) {
        att[i] = (VkAttachmentDescription){
            .format = (i < n) ? cf : df,
            .samples = ns,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            /* Stencil lives across draws like depth does (a title writes it in
             * one pass and tests it in the next); ignored for colour and for
             * depth formats without stencil. */
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
        if (i < n) cref[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_GENERAL };
    }
    VkAttachmentReference dref = { n, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = n, .pColorAttachments = cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = n + 1, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    if (pvkCreateRenderPass(s_vk.device, &rci, NULL, out) != VK_SUCCESS) {
        *out = VK_NULL_HANDLE;
        return -1;
    }
    return 0;
}

/* Tables carried from the Metal backend, itself copied from the D3D12 live
 * draw engine a title has shipped on. */
static VkBlendFactor vk_gcm_blend_factor(u32 f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}
/* The signed equations are emulated as RPCS3 does (VKGSRender.cpp
 * get_blend_op): ADD_SIGNED and REVERSE_ADD_SIGNED as ADD,
 * REVERSE_SUBTRACT_SIGNED as REVERSE_SUBTRACT. */
static VkBlendOp vk_gcm_blend_op(u32 e)
{
    switch (e) {
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0xF005: return VK_BLEND_OP_REVERSE_SUBTRACT;     /* REVERSE_SUBTRACT_SIGNED */
    default:     return VK_BLEND_OP_ADD;                  /* FUNC_ADD 0x8006, 0xF006, 0xF007 */
    }
}
static VkCompareOp vk_gcm_compare(u32 f)
{
    f |= 0x0200u;   /* RPCS3 to_comparison_function: raw 0..7 accepted */
    return (f <= 0x0206u) ? (VkCompareOp)(f - 0x0200u) : VK_COMPARE_OP_ALWAYS;
}
static VkStencilOp vk_gcm_stencil_op(u32 op)
{
    switch (op) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;               /* GL_KEEP 0x1E00 */
    }
}
static VkStencilOpState vk_gcm_stencil_face(u32 func, u32 fail, u32 zfail, u32 zpass,
                                            u32 cmp_mask, u32 write_mask)
{
    VkStencilOpState s = { .failOp = vk_gcm_stencil_op(fail), .passOp = vk_gcm_stencil_op(zpass),
                           .depthFailOp = vk_gcm_stencil_op(zfail),
                           .compareOp = vk_gcm_compare(func),
                           .compareMask = cmp_mask & 0xFFu, .writeMask = write_mask & 0xFFu,
                           .reference = 0 };                /* dynamic */
    return s;
}

/* RPCS3's Vulkan backend always rasterises FILL (graphics_pipeline_state.hpp;
 * FRONT/BACK_POLYGON_MODE are only honoured by its GL backend). That is the
 * default here. RSX_ENG_POLYGON_MODE=1 opts into the GL rule (GLDraw.cpp):
 * one host polygon mode per draw, the back face's when front faces are
 * culled, else the front face's, when fillModeNonSolid is available. */
static VkPolygonMode vk_eng_polygon_mode(const rsx_be_render_state* rs)
{
    static int env = -1;
    if (env < 0) { const char* e = getenv("RSX_ENG_POLYGON_MODE"); env = (e && *e == '1'); }
    if (!env || !s_vk.fill_non_solid_ok) return VK_POLYGON_MODE_FILL;
    const int show_back = rs->cull_enable && rs->cull_face == 0x0404u;
    const u32 m = show_back ? rs->polygon_mode_back : rs->polygon_mode_front;
    return m == 0x1B00u ? VK_POLYGON_MODE_POINT
         : m == 0x1B01u ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
}

/* RSX_VK_FAIL_DUMP=<existing dir>: a program glslang rejects is written there
 * as failN.<vs|ps>.hlsl with its log appended in a comment. */
static void vk_eng_fail_dump(const char* hlsl, int stage, const char* log)
{
    static long n;                      /* racy across workers; debug only */
    const char* dir = getenv("RSX_VK_FAIL_DUMP");
    if (!dir || !*dir || !hlsl) return;
    const long i = ++n;
    if (i > 64) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/fail%ld.%s.hlsl", dir, i,
             stage == RSX_SHADER_STAGE_VERTEX ? "vs" : "ps");
    FILE* f = fopen(path, "w");
    if (!f) return;
    fputs(hlsl, f);
    fprintf(f, "\n/* %s */\n", log ? log : "");
    fclose(f);
}

static int vk_eng_translate(const char* hlsl, int stage, VkShaderModule* out)
{
    u32 words = 0;
    if (rsx_hlsl_to_spirv(hlsl, stage, s_g.spv, VK_SPV_WORDS, &words, s_g.log, sizeof s_g.log) || !words) {
        VK_LOG("engine: %s translation failed: %s\n",
               stage == RSX_SHADER_STAGE_VERTEX ? "vertex" : "fragment", s_g.log);
        vk_eng_fail_dump(hlsl, stage, s_g.log);
        return -1;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)words * 4u, .pCode = s_g.spv };
    return pvkCreateShaderModule(s_vk.device, &ci, NULL, out) == VK_SUCCESS ? 0 : -1;
}

/* vk_eng_translate with the caller's SPIR-V and log buffers, for a pipeline
 * worker (s_g's are the FIFO thread's). */
static int vk_eng_translate_into(const char* hlsl, int stage, VkShaderModule* out,
                                 u32* spv, char* log, u32 log_size)
{
    u32 words = 0;
    log[0] = 0;
    if (rsx_hlsl_to_spirv(hlsl, stage, spv, VK_SPV_WORDS, &words, log, log_size) || !words) {
        VK_LOG("engine: %s translation failed: %s\n",
               stage == RSX_SHADER_STAGE_VERTEX ? "vertex" : "fragment", log);
        vk_eng_fail_dump(hlsl, stage, log);
        return -1;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)words * 4u, .pCode = spv };
    return pvkCreateShaderModule(s_vk.device, &ci, NULL, out) == VK_SUCCESS ? 0 : -1;
}

/* The engine's set layouts. Constants are dynamic uniform buffers (slices
 * of the frame ring, bound by offset). [0], shared: the sixteen textures in
 * one binding. [1], split: a pixel program with a cube unit declares all
 * sixteen units one by one (TextureCube or Texture2D rsx_texN : register(tN))
 * instead of the Texture2D array, and register(tN) is binding
 * RSX_SPIRV_TEXTURE_BINDING + N (rsx_shader_spirv.h). */
static int vk_eng_layouts(void)
{
    if (s_e2.pipe_layout[0] && s_e2.pipe_layout[1]) return 0;
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (u32 split = 0; split < 2; split++) {
        if (s_e2.pipe_layout[split]) continue;
        VkDescriptorSetLayoutBinding b[3 + RSX_MAX_TEXTURES + 2u * RSX_BE_MAX_VERTEX_TEXTURES];
        u32 n = 0;
        b[n++] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_VPCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, both, NULL };
        b[n++] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_PSCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, both, NULL };
        b[n++] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER,
                                                 RSX_MAX_TEXTURES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        if (split) {
            for (u32 u = 0; u < RSX_MAX_TEXTURES; u++)
                b[n++] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_TEXTURE_BINDING + u,
                                                         VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                         VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        } else {
            b[n++] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_TEXTURE_BINDING, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                                     RSX_MAX_TEXTURES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        }
        for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
            b[n++] = (VkDescriptorSetLayoutBinding){ VK_ENG_VTEX_TEX_BINDING + u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                                     1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
            b[n++] = (VkDescriptorSetLayoutBinding){ VK_ENG_VTEX_SAMP_BINDING + u, VK_DESCRIPTOR_TYPE_SAMPLER,
                                                     1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
        }
        VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                .bindingCount = n, .pBindings = b };
        if (!s_e2.set_layout[split] &&
            pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_e2.set_layout[split]) != VK_SUCCESS) {
            s_e2.set_layout[split] = VK_NULL_HANDLE;
            return -1;
        }
        VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                            .setLayoutCount = 1, .pSetLayouts = &s_e2.set_layout[split] };
        if (pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_e2.pipe_layout[split]) != VK_SUCCESS) {
            s_e2.pipe_layout[split] = VK_NULL_HANDLE;
            return -1;
        }
    }
    return 0;
}

/* A copy of the VP HLSL with each vertex sampler "register(sN)" moved to
 * s(16+N) (VK_ENG_VTEX_SAMP_BINDING), or NULL when it samples none. */
static char* vk_eng_vs_rebind(const char* vs_hlsl)
{
    if (!strstr(vs_hlsl, "SamplerState rsx_vsamp")) return NULL;
    const size_t len = strlen(vs_hlsl);
    char* out = (char*)malloc(len + 16u * RSX_BE_MAX_VERTEX_TEXTURES + 1u);
    if (!out) return NULL;
    memcpy(out, vs_hlsl, len + 1u);
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        char from[64], to[64];
        snprintf(from, sizeof from, "SamplerState rsx_vsamp%u : register(s%u);", u, u);
        snprintf(to, sizeof to, "SamplerState rsx_vsamp%u : register(s%u);", u, 16u + u);
        char* p = strstr(out, from);
        if (!p) continue;
        const size_t fl = strlen(from), tl = strlen(to);
        memmove(p + tl, p + fl, strlen(p + fl) + 1u);
        memcpy(p, to, tl);
    }
    return out;
}

/* Which units a pixel program declares as TextureCube (bits 0-15) or
 * Texture3D (bits 16-31); either needs the split set layout. */
static u32 vk_eng_cube_units(const char* ps_hlsl)
{
    u32 mask = 0;
    char decl[40];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        snprintf(decl, sizeof decl, "TextureCube rsx_tex%u ", u);
        if (strstr(ps_hlsl, decl)) mask |= 1u << u;
        snprintf(decl, sizeof decl, "Texture3D   rsx_tex%u ", u);
        if (strstr(ps_hlsl, decl)) mask |= 1u << (u + 16u);
    }
    return mask;
}

/* The vertex program's vertex-texture declarations (rsx_vp_decompiler). */
static u32 vk_eng_vtex_units(const char* vs_hlsl)
{
    u32 mask = 0;
    char decl[40];
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        snprintf(decl, sizeof decl, "TextureCube rsx_vtex%u ", u);
        if (strstr(vs_hlsl, decl)) mask |= 1u << u;
        snprintf(decl, sizeof decl, "Texture3D rsx_vtex%u ", u);
        if (strstr(vs_hlsl, decl)) mask |= 1u << (u + 16u);
    }
    return mask;
}

/* ---- E2: asynchronous pipeline compilation --------------------------------
 * RPCS3's "Async Recompiler" (VKPipelineCompiler.cpp pipe_compiler): a draw
 * whose pipeline is not built yet queues the work on a pool of compiler
 * threads and is skipped until the result is published. Here a slot's two
 * modules (glslang + vkCreateShaderModule) are one job, queued by
 * pipeline_create, and each topology variant (vkCreateGraphicsPipelines
 * against the shared, internally synchronised VkPipelineCache) another,
 * queued by the first draw that needs it or by pipeline_prewarm. A worker
 * publishes with a release store of the slot's VK_AS_* state; the FIFO thread
 * only touches a module or a variant after an acquire load reads READY.
 * Render passes and layouts are made on the FIFO thread before a job is
 * queued. Worker count: RPCS3 decay_num_worker_threads ("Shader Compiler
 * Threads: 0"), or RSX_VK_PIPELINE_THREADS=N. RSX_VK_ASYNC_PIPELINES=0 builds
 * synchronously at the draw, as before.
 *
 * Exactly as RPCS3 (VKGSRender::load_program, ProgramStateCache, VKProgramBuffer,
 * rsx_cache.h shaders_cache) with the port's "Shader Mode: Async Recompiler":
 * - shader modules are compiled inline on the FIFO thread (recompile_*_program:
 *   Decompile + Compile) and shared by every pipeline linked from the same
 *   program (vk_mod_*); only linking is deferred. Pipelines created before the
 *   first draw (the engine's preloaded program cache) build theirs on the pool,
 *   as RPCS3's preload_programs runs on its loader workers.
 *   RSX_VK_ASYNC_MODULES=1 queues runtime modules on the pool too.
 * - every linked (pass class, topology) variant is recorded on disk
 *   (vk_vfile_*, RPCS3 stores the full pipeline_props per pipeline file) and a
 *   later run links exactly those as the pipeline is recreated;
 * - the preload is blocking: the first draw after the engine's prewarm waits
 *   for every queued link (shaders_cache::load / compile_shaders, no skip in
 *   this mode), so a cached pipeline is never skipped. RSX_VK_PRELOAD_WAIT=0
 *   leaves it in the background;
 * - VkPipelineCache is also written back when the preload finishes and as new
 *   pipelines accumulate, not only at a clean shutdown.
 * The interpreter fallback (async_with_shader_interpreter) is not this mode;
 * see scratch/parity/interp.md. */
enum { VK_AS_NONE = 0, VK_AS_PENDING, VK_AS_READY, VK_AS_FAILED };
#define VK_ASYNC_QUEUE   16384u
#define VK_ASYNC_THREADS 12u

#ifdef _WIN32
typedef CONDITION_VARIABLE vk_cond;
#define vk_cond_init(c)       InitializeConditionVariable(c)
#define vk_cond_wait(c, m)    SleepConditionVariableSRW((c), (m), INFINITE, 0)
#define vk_cond_signal(c)     WakeConditionVariable(c)
#define vk_cond_broadcast(c)  WakeAllConditionVariable(c)
#define vk_async_yield()      Sleep(1)
typedef HANDLE vk_thread_t;
#else
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>
typedef pthread_cond_t vk_cond;
#define vk_cond_init(c)       pthread_cond_init((c), NULL)
#define vk_cond_wait(c, m)    pthread_cond_wait((c), (m))
#define vk_cond_signal(c)     pthread_cond_signal(c)
#define vk_cond_broadcast(c)  pthread_cond_broadcast(c)
#define vk_async_yield()      sched_yield()
typedef pthread_t vk_thread_t;
#endif

static struct {
    int             state;            /* 0 not started, 1 running, -1 off */
    u32             nthreads;
    pthread_mutex_t mu;
    vk_cond         cv;
    u32             q[VK_ASYNC_QUEUE];  /* slot << 3 | kind: 0 modules, 1 + topology */
    u32             head, count;
    int             stop;
    vk_thread_t     th[VK_ASYNC_THREADS];
    vk_atomic       skipped, built;
    vk_atomic       pending;          /* jobs queued or running */
    int             preload;          /* the engine prewarmed its program cache: wait at the next draw */
    int             drawn;            /* a draw has asked for a variant (the preload phase is over) */
    long            saved_built;      /* `built` at the last VkPipelineCache write-back */
} s_vka;

static VkPipeline vk_eng_variant(vk_eng_pipe* P, u32 topo_idx);
static VkPipeline vk_eng_variant_pc(vk_eng_pipe* P, u32 topo_idx, u32 pc);

/* ---- shared shader modules: RPCS3 program_state_cache keeps one compiled
 * shader per distinct vertex / fragment program (search_vertex_program /
 * search_fragment_program) and links every pipeline from those; a new render
 * state over known programs only links. Keyed on the translated source and
 * stage; workers and the FIFO thread both use it, under s_vkmod.mu. The
 * modules live until vk_mod_release_all at shutdown. */
static struct { int init; pthread_mutex_t mu; vkps_map map; u32 hits, made; } s_vkmod;

static void vk_mod_init(void)
{
    if (s_vkmod.init) return;
    pthread_mutex_init(&s_vkmod.mu, NULL);
    s_vkmod.init = 1;
}

static int vk_mod_get(const char* hlsl, int stage, VkShaderModule* out, u32* spv, char* log, u32 log_size)
{
    *out = VK_NULL_HANDLE;
    if (!hlsl || !spv) return -1;
    vk_mod_init();
    const u64 k = vkps_key(vkps_fnv(VKPS_FNV_BASIS + (u64)stage, hlsl, strlen(hlsl)));
    pthread_mutex_lock(&s_vkmod.mu);
    const u64* v = vkps_map_get(&s_vkmod.map, k);
    if (v && *v) {
        *out = (VkShaderModule)(uintptr_t)*v;
        s_vkmod.hits++;
        pthread_mutex_unlock(&s_vkmod.mu);
        return 0;
    }
    pthread_mutex_unlock(&s_vkmod.mu);
    VkShaderModule m;
    if (vk_eng_translate_into(hlsl, stage, &m, spv, log, log_size)) return -1;
    pthread_mutex_lock(&s_vkmod.mu);
    u64* slot = vkps_map_put(&s_vkmod.map, k);
    if (slot && *slot) {                        /* another thread made it meanwhile */
        pvkDestroyShaderModule(s_vk.device, m, NULL);
        m = (VkShaderModule)(uintptr_t)*slot;
    } else if (slot) {
        *slot = (u64)(uintptr_t)m;
        s_vkmod.made++;
    } else {
        pvkDestroyShaderModule(s_vk.device, m, NULL);
        m = VK_NULL_HANDLE;
    }
    pthread_mutex_unlock(&s_vkmod.mu);
    *out = m;
    return m ? 0 : -1;
}

/* Shutdown, after the pool stopped: pipelines keep no reference to a module. */
static void vk_mod_release_all(void)
{
    if (!s_vkmod.init) return;
    for (u32 i = 0; i < s_vkmod.map.cap; i++)
        if (s_vkmod.map.e[i].key && s_vkmod.map.e[i].val)
            vk_defer_shader((VkShaderModule)(uintptr_t)s_vkmod.map.e[i].val);
    vkps_map_free(&s_vkmod.map);
}

/* ---- persisted variants: per pipeline content key, the (pass class,
 * topology) variants it was linked for. RPCS3 writes one file per linked
 * pipeline_props (shaders_cache::store, called when a pipeline is linked or
 * queued) and links them all again at boot. RSX_VK_VARIANT_CACHE=0 disables;
 * RSX_VK_VARIANT_CACHE_PATH moves it from shadercache/vk_pipeline_variants.bin.
 * FIFO thread only. */
static struct { int state; FILE* f; vkps_map map; } s_vkv;   /* state: 0 untried, 1 on, -1 off */

static void vk_vfile_open(void)
{
    if (s_vkv.state) return;
    s_vkv.state = -1;
    const char* e = getenv("RSX_VK_VARIANT_CACHE");
    if (e && *e == '0') return;
    const char* pe = getenv("RSX_VK_VARIANT_CACHE_PATH");
    const char* path = (pe && *pe) ? pe : "shadercache/vk_pipeline_variants.bin";
    const int n = vkps_variants_load(path, &s_vkv.map);
    s_vkv.f = vkps_variants_open(path, n >= 0);
    if (!s_vkv.f && !(pe && *pe)) {
#ifdef _WIN32
        CreateDirectoryA("shadercache", NULL);
#else
        mkdir("shadercache", 0755);
#endif
        s_vkv.f = vkps_variants_open(path, n >= 0);
    }
    s_vkv.state = 1;
    VK_LOG("pipelines: variant cache %s: %d record(s), %u pipeline(s)%s\n", path, n < 0 ? 0 : n,
           s_vkv.map.n, s_vkv.f ? "" : " (cannot write)");
}

static void vk_vfile_close(void)
{
    if (s_vkv.f) fclose(s_vkv.f);
    vkps_map_free(&s_vkv.map);
    memset(&s_vkv, 0, sizeof s_vkv);
}

/* A pipeline's identity across runs: its two programs and everything it was
 * keyed and built on, except the dynamic blend colour. */
static u64 vk_pipe_key(const char* vs, const char* ps, const rsx_be_render_state* rs,
                       u32 nslots, u32 stride, VkFormat cf, u32 rt_count)
{
    rsx_be_render_state r = *rs;
    r.blend_color = r.blend_color2 = 0;
    const u32 extra[4] = { nslots, stride, (u32)cf, rt_count };
    u64 h = vkps_fnv(VKPS_FNV_BASIS, vs, strlen(vs) + 1u);
    h = vkps_fnv(h, ps, strlen(ps) + 1u);
    h = vkps_fnv(h, &r, sizeof r);
    return vkps_key(vkps_fnv(h, extra, sizeof extra));
}

/* The mask an earlier run recorded for this key (0 = none). */
static u64 vk_vfile_mask(u64 key)
{
    vk_vfile_open();
    const u64* v = vkps_map_get(&s_vkv.map, key);
    return v ? *v : 0;
}

/* A draw links variant (pc, t) of P: record it once. */
static void vk_vfile_note(vk_eng_pipe* P, u32 t, u32 pc)
{
    const u32 b = vkps_variant_bit(pc, t);
    if (b >= 64u || !P->pkey || ((P->vmask >> b) & 1u)) return;
    P->vmask |= 1ull << b;
    if (s_vkv.state <= 0) return;
    u64* v = vkps_map_put(&s_vkv.map, P->pkey);
    if (v) *v |= 1ull << b;
    vkps_variants_append(s_vkv.f, P->pkey, 1ull << b);
}

static u32 vk_async_thread_count(void)
{
    const char* e = getenv("RSX_VK_PIPELINE_THREADS");
    int n = (e && *e) ? atoi(e) : 0;
    if (n <= 0) {
        u32 hw;
#ifdef _WIN32
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        hw = (u32)si.dwNumberOfProcessors;
#else
        const long c = sysconf(_SC_NPROCESSORS_ONLN);
        hw = c > 0 ? (u32)c : 1u;
#endif
        n = hw >= 24 ? 12 : hw >= 16 ? 8 : hw > 12 ? 6 : hw > 8 ? 4 : hw == 8 ? 2 : 1;
    }
    if (n > (int)VK_ASYNC_THREADS) n = (int)VK_ASYNC_THREADS;
    return (u32)n;
}

/* Job word: pass class << 28 | slot << 3 | kind (0 modules, 1 + topology). */
static vk_atomic* vk_vstate(vk_eng_pipe* P, u32 t, u32 pc)
{
    return pc ? &P->msvstate[(pc - 1u) * VK_ENG_TOPOLOGIES + t] : &P->vstate[t];
}

static void vk_async_job(u32 job, u32* spv, char* log, u32 log_size)
{
    vk_eng_pipe* P = &s_epipe[(job & 0x0FFFFFFFu) >> 3];
    const u32 kind = job & 7u, pc = job >> 28;
    if (!kind) {
        VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
        const int ok = spv && P->vs_src && P->ps_src &&
            !vk_mod_get(P->vs_src, RSX_SHADER_STAGE_VERTEX, &vs, spv, log, log_size) &&
            !vk_mod_get(P->ps_src, RSX_SHADER_STAGE_FRAGMENT, &fs, spv, log, log_size);
        if (ok) {
            P->vs = vs;
            P->fs = fs;
        }
        free(P->vs_src);
        free(P->ps_src);
        P->vs_src = P->ps_src = NULL;
        VK_AT_STORE(&P->mstate, ok ? VK_AS_READY : VK_AS_FAILED);
    } else {
        const u32 t = kind - 1u;
        /* The module job was queued first, so it is running already. */
        while (VK_AT_LOAD(&P->mstate) == VK_AS_PENDING) vk_async_yield();
        VkPipeline p = VK_NULL_HANDLE;
        if (VK_AT_LOAD(&P->mstate) != VK_AS_FAILED && P->vs && P->fs) p = vk_eng_variant_pc(P, t, pc);
        if (p) VK_AT_ADD(&s_vka.built, 1);
        VK_AT_STORE(vk_vstate(P, t, pc), p ? VK_AS_READY : VK_AS_FAILED);
    }
    VK_AT_ADD(&P->inflight, -1);
    VK_AT_ADD(&s_vka.pending, -1);
}

#ifdef _WIN32
static DWORD WINAPI vk_async_main(LPVOID arg)
#else
static void* vk_async_main(void* arg)
#endif
{
    (void)arg;
    u32* spv = (u32*)malloc((size_t)VK_SPV_WORDS * sizeof(u32));
    char* log = (char*)malloc(4096);
    for (;;) {
        pthread_mutex_lock(&s_vka.mu);
        while (!s_vka.count && !s_vka.stop) vk_cond_wait(&s_vka.cv, &s_vka.mu);
        if (s_vka.stop) { pthread_mutex_unlock(&s_vka.mu); break; }
        const u32 job = s_vka.q[s_vka.head];
        s_vka.head = (s_vka.head + 1u) % VK_ASYNC_QUEUE;
        s_vka.count--;
        pthread_mutex_unlock(&s_vka.mu);
        vk_async_job(job, log ? spv : NULL, log, 4096u);
    }
    free(spv);
    free(log);
    return 0;
}

static int vk_async_on(void)
{
    if (s_vka.state) return s_vka.state > 0;
    const char* e = getenv("RSX_VK_ASYNC_PIPELINES");
    if (e && *e == '0') {
        s_vka.state = -1;
        VK_LOG("pipelines: synchronous (RSX_VK_ASYNC_PIPELINES=0)\n");
        return 0;
    }
    pthread_mutex_init(&s_vka.mu, NULL);
    vk_cond_init(&s_vka.cv);
    vk_mod_init();                    /* before any worker can reach the module cache */
    s_vka.stop = 0;
    s_vka.head = s_vka.count = 0;
    s_vka.pending = 0;
    const u32 want = vk_async_thread_count();
    u32 started = 0;
    for (u32 i = 0; i < want; i++) {
        /* glslang recurses deeply on long programs: a roomy stack. */
#ifdef _WIN32
        s_vka.th[i] = CreateThread(NULL, 8u << 20, vk_async_main, NULL,
                                   STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!s_vka.th[i]) break;
#else
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 8u << 20);
        const int rc = pthread_create(&s_vka.th[i], &at, vk_async_main, NULL);
        pthread_attr_destroy(&at);
        if (rc) break;
#endif
        started++;
    }
    s_vka.nthreads = started;
    s_vka.state = started ? 1 : -1;
    VK_LOG("pipelines: async recompiler, %u compiler thread(s)\n", started);
    return s_vka.state > 0;
}

static int vk_async_push(u32 slot, u32 kind)
{
    vk_eng_pipe* P = &s_epipe[slot];
    pthread_mutex_lock(&s_vka.mu);
    if (s_vka.stop || s_vka.count >= VK_ASYNC_QUEUE) {
        pthread_mutex_unlock(&s_vka.mu);
        return 0;
    }
    VK_AT_ADD(&P->inflight, 1);
    VK_AT_ADD(&s_vka.pending, 1);
    s_vka.q[(s_vka.head + s_vka.count) % VK_ASYNC_QUEUE] = (slot << 3) | kind;
    s_vka.count++;
    vk_cond_signal(&s_vka.cv);
    pthread_mutex_unlock(&s_vka.mu);
    return 1;
}

/* Queue topology t of P unless it is queued, built or known to fail. */
static void vk_async_request_pc(vk_eng_pipe* P, u32 t, u32 pc)
{
    if (pc && !P->msvstate) return;
    vk_atomic* vs = vk_vstate(P, t, pc);
    if (VK_AT_LOAD(vs) != VK_AS_NONE || VK_AT_LOAD(&P->mstate) == VK_AS_FAILED) return;
    if (!vk_eng_render_pass_pc(P->rt_fmt, P->rt_count, pc)) return;   /* made here, read by the worker */
    VK_AT_STORE(vs, VK_AS_PENDING);
    if (!vk_async_push((u32)(P - s_epipe), (pc << 28) | (1u + t))) VK_AT_STORE(vs, VK_AS_NONE);
}
static void vk_async_request(vk_eng_pipe* P, u32 t) { vk_async_request_pc(P, t, 0); }

/* Pass classes past 0 get their variant tables on the render thread. */
static int vk_eng_msv_alloc(vk_eng_pipe* P)
{
    if (P->msv) return 0;
    P->msv = (VkPipeline*)calloc((VK_EMS_PC - 1u) * VK_ENG_TOPOLOGIES, sizeof(VkPipeline));
    P->msvstate = (vk_atomic*)calloc((VK_EMS_PC - 1u) * VK_ENG_TOPOLOGIES, sizeof(vk_atomic));
    if (!P->msv || !P->msvstate) {
        free(P->msv); free((void*)P->msvstate);
        P->msv = NULL; P->msvstate = NULL;
        return -1;
    }
    return 0;
}

static int vk_ems_pc_ok(u32 pc);

/* Queue every variant an earlier run linked for P (RPCS3 compile_shaders:
 * each stored pipeline_props is linked again). Multisampled classes only
 * where this device and configuration render them. */
static void vk_async_request_mask(vk_eng_pipe* P, u64 mask)
{
    for (u32 b = 0; mask >> b; b++) {
        if (!((mask >> b) & 1u)) continue;
        const u32 pc = b / VKPS_TOPOLOGIES, t = b % VKPS_TOPOLOGIES;
        if (pc >= VK_EMS_PC || !vk_ems_pc_ok(pc)) continue;
        if (pc && vk_eng_msv_alloc(P)) continue;
        vk_async_request_pc(P, t, pc);
    }
}

static u64 vk_async_ms(void)
{
#ifdef _WIN32
    return (u64)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000u + (u64)ts.tv_nsec / 1000000u;
#endif
}

/* RPCS3 shaders_cache::load: the cached pipelines are all linked before the
 * game draws with any of them -- in the Async Recompiler mode the loader has
 * no skip (skip_after is only armed for async_with_interpreter). Here the
 * engine preloads at its first draw, so that draw waits. */
static void vk_async_preload_wait(void)
{
    s_vka.preload = 0;
    const char* e = getenv("RSX_VK_PRELOAD_WAIT");
    if (e && *e == '0') return;
    const long total = (long)VK_AT_LOAD(&s_vka.pending);
    if (total <= 0) return;
    const long built0 = (long)VK_AT_LOAD(&s_vka.built);
    VK_LOG("pipelines: preloading cached shaders, %ld job(s) (RPCS3 shaders_cache::load)\n", total);
    const u64 t0 = vk_async_ms();
    u64 last = t0;
    long left;
    while ((left = (long)VK_AT_LOAD(&s_vka.pending)) > 0 && s_vka.state > 0) {
        vk_async_yield();
        const u64 now = vk_async_ms();
        if (now - last >= 1000u) {
            last = now;
            VK_LOG("pipelines: compiling pipeline object %ld of %ld\n", total - left, total);
        }
    }
    VK_LOG("pipelines: preload done in %llu ms, %ld pipeline(s) linked, %u module(s) compiled\n",
           (unsigned long long)(vk_async_ms() - t0), (long)VK_AT_LOAD(&s_vka.built) - built0,
           s_vkmod.made);
    vk_pcache_save();
    s_vka.saved_built = (long)VK_AT_LOAD(&s_vka.built);
}

/* The draw's pipeline: built synchronously when the pool is off, else the
 * published variant, or NULL (the draw is skipped, RPCS3 end_draw "Program
 * is not ready") while it links. */
static VkPipeline vk_async_variant_pc(vk_eng_pipe* P, u32 t, u32 pc)
{
    if (pc >= VK_EMS_PC) return VK_NULL_HANDLE;
    if (pc && vk_eng_msv_alloc(P)) return VK_NULL_HANDLE;
    if (s_vka.state <= 0) return vk_eng_variant_pc(P, t, pc);
    s_vka.drawn = 1;
    if (s_vka.preload) vk_async_preload_wait();
    vk_vfile_note(P, t, pc);
    if (VK_AT_LOAD(vk_vstate(P, t, pc)) == VK_AS_READY) {
        /* The driver cache on disk survives a run that never shuts down cleanly. */
        const long built = (long)VK_AT_LOAD(&s_vka.built);
        if (built - s_vka.saved_built >= 1024 ||
            (built - s_vka.saved_built >= 128 && !VK_AT_LOAD(&s_vka.pending))) {
            s_vka.saved_built = built;
            vk_pcache_save();
        }
        return pc ? P->msv[(pc - 1u) * VK_ENG_TOPOLOGIES + t] : P->variant[t];
    }
    vk_async_request_pc(P, t, pc);
    vkq_mark_skipped();
    const long n = VK_AT_ADD(&s_vka.skipped, 1);
    if (n < 8 || (n & 1023) == 0)
        VK_LOG("pipelines: draw skipped while its pipeline compiles (%ld so far, %ld built)\n",
               (long)n + 1, (long)VK_AT_LOAD(&s_vka.built));
    return VK_NULL_HANDLE;
}

/* Stop the pool: the running jobs finish, the queued ones are failed. */
static void vk_async_stop(void)
{
    if (s_vka.state <= 0) { s_vka.state = 0; return; }
    pthread_mutex_lock(&s_vka.mu);
    s_vka.stop = 1;
    vk_cond_broadcast(&s_vka.cv);
    pthread_mutex_unlock(&s_vka.mu);
    for (u32 i = 0; i < s_vka.nthreads; i++) {
#ifdef _WIN32
        WaitForSingleObject(s_vka.th[i], INFINITE);
        CloseHandle(s_vka.th[i]);
#else
        pthread_join(s_vka.th[i], NULL);
#endif
    }
    for (; s_vka.count; s_vka.count--, s_vka.head = (s_vka.head + 1u) % VK_ASYNC_QUEUE) {
        const u32 job = s_vka.q[s_vka.head];
        vk_eng_pipe* P = &s_epipe[(job & 0x0FFFFFFFu) >> 3];
        if (job & 7u) VK_AT_STORE(vk_vstate(P, (job & 7u) - 1u, job >> 28), VK_AS_FAILED);
        else VK_AT_STORE(&P->mstate, VK_AS_FAILED);
        VK_AT_ADD(&P->inflight, -1);
        VK_AT_ADD(&s_vka.pending, -1);
    }
    s_vka.nthreads = 0;
    s_vka.state = 0;
}

static void vk_eng_pipeline_prewarm(void* user, u32 pipeline, u32 topology_mask)
{
    (void)user;
    if (s_vka.state <= 0 || !pipeline || pipeline > VK_ENG_MAX_PIPE) return;
    vk_eng_pipe* P = &s_epipe[pipeline - 1];
    if (!P->used) return;
    /* Only the engine's program-cache preload prewarms: its first draw waits. */
    s_vka.preload = 1;
    /* The recorded variants (pass class and topology) were queued at create;
     * the engine's topology mask is the class-0 guess for older caches. */
    if (P->vmask) return;
    for (u32 t = 0; t < VK_ENG_TOPOLOGIES; t++)
        if ((topology_mask >> (t + 1u)) & 1u) vk_async_request(P, t);
}

static int vk_eng_pipeline_async(void* user)
{
    (void)user;
    return s_g.on && vk_async_on();
}

/* Point sprites: rsx_hlsl_to_spirv turns the fragment decompiler's
 * coordinate input into BuiltIn PointCoord. */
static int vk_eng_point_sprite_supported(void* user)
{
    (void)user;
    return s_g.on && rsx_hlsl_to_spirv_available();
}

static u32 vk_eng_pipeline_create(void* user, const char* vs_hlsl, const char* ps_hlsl,
                                  const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                                  u32 vertex_stride, rsx_be_format rt_fmt, u32 rt_count)
{
    (void)user;
    /* Same gate as Metal: no guest programs, no engine pipelines. */
    if (!s_g.on || !vs_hlsl || !ps_hlsl || !rs || !layout || !vertex_stride) return 0;
    if (vk_eng_layouts()) return 0;
    u32 rt_bpp;
    const VkFormat cf = vk_eng_format(rt_fmt, &rt_bpp);
    if (vk_eng_rt_index(cf) < 0) {
        if (!(s_e2.warned & 1u)) { VK_LOG("engine: colour format %d not supported yet\n", (int)rt_fmt); s_e2.warned |= 1u; }
        return 0;
    }
    if (!rt_count) rt_count = 1;
    if (rt_count > RSX_BE_MAX_COLOR_TARGETS) rt_count = RSX_BE_MAX_COLOR_TARGETS;
    u32 slot = VK_ENG_MAX_PIPE;
    for (u32 i = 0; i < VK_ENG_MAX_PIPE; i++) if (!s_epipe[i].used) { slot = i; break; }
    if (slot == VK_ENG_MAX_PIPE) {
        if (!(s_e2.warned & 128u)) {
            VK_LOG("engine: pipeline table full (%u), draws will drop\n", (unsigned)VK_ENG_MAX_PIPE);
            s_e2.warned |= 128u;
        }
        return 0;
    }
    vk_eng_pipe* P = &s_epipe[slot];
    memset(P, 0, sizeof *P);
    if (vk_async_on()) {
        /* The handle is live at once; the link follows on a worker. */
        char* vs_rebound = vk_eng_vs_rebind(vs_hlsl);
        const size_t vn = strlen(vs_hlsl) + 1u, pn = strlen(ps_hlsl) + 1u;
        P->vs_src = vs_rebound ? vs_rebound : (char*)malloc(vn);
        P->ps_src = (char*)malloc(pn);
        if (P->vs_src && P->ps_src) {
            if (!vs_rebound) memcpy(P->vs_src, vs_hlsl, vn);
            memcpy(P->ps_src, ps_hlsl, pn);
            P->rs = *rs;
            P->nslots = layout->count;
            P->stride = vertex_stride;
            P->rt_count = rt_count;
            P->rt_fmt = cf;
            P->cube_mask = vk_eng_cube_units(ps_hlsl);
            P->vcube_mask = vk_eng_vtex_units(vs_hlsl);
            P->used = 1;
            P->pkey = vk_pipe_key(vs_hlsl, ps_hlsl, rs, P->nslots, P->stride, cf, rt_count);
            P->vmask = vk_vfile_mask(P->pkey);
            static int async_mods = -1;
            if (async_mods < 0) {
                const char* e = getenv("RSX_VK_ASYNC_MODULES");
                async_mods = (e && *e == '1') ? 1 : 0;
            }
            if (s_vka.drawn && !async_mods) {
                /* RPCS3 VKTraits::recompile_*_program: Decompile + Compile
                 * inline on the RSX thread; only linking is deferred. */
                const int ok =
                    !vk_mod_get(P->vs_src, RSX_SHADER_STAGE_VERTEX, &P->vs, s_g.spv, s_g.log, (u32)sizeof s_g.log) &&
                    !vk_mod_get(P->ps_src, RSX_SHADER_STAGE_FRAGMENT, &P->fs, s_g.spv, s_g.log, (u32)sizeof s_g.log);
                free(P->vs_src);
                free(P->ps_src);
                P->vs_src = P->ps_src = NULL;
                if (!ok) { memset(P, 0, sizeof *P); return 0; }
                P->mstate = VK_AS_READY;
                vk_async_request_mask(P, P->vmask);
                return slot + 1u;
            }
            /* Before the first draw (the engine's program-cache preload):
             * modules on the pool, like RPCS3's preload_programs workers. */
            P->mstate = VK_AS_PENDING;
            if (vk_async_push(slot, 0)) {
                vk_async_request_mask(P, P->vmask);
                return slot + 1u;
            }
        }
        free(P->vs_src);
        free(P->ps_src);
        memset(P, 0, sizeof *P);         /* queue full: build it here */
    }
    char* vs_rebound = vk_eng_vs_rebind(vs_hlsl);
    const int vs_fail = vk_mod_get(vs_rebound ? vs_rebound : vs_hlsl, RSX_SHADER_STAGE_VERTEX, &P->vs,
                                   s_g.spv, s_g.log, (u32)sizeof s_g.log);
    free(vs_rebound);
    if (vs_fail) { memset(P, 0, sizeof *P); return 0; }
    if (vk_mod_get(ps_hlsl, RSX_SHADER_STAGE_FRAGMENT, &P->fs, s_g.spv, s_g.log, (u32)sizeof s_g.log)) {
        memset(P, 0, sizeof *P); return 0;
    }
    P->rs = *rs;
    P->nslots = layout->count;
    P->stride = vertex_stride;
    P->rt_count = rt_count;
    P->rt_fmt = cf;
    P->cube_mask = vk_eng_cube_units(ps_hlsl);
    P->vcube_mask = vk_eng_vtex_units(vs_hlsl);
    P->used = 1;
    return slot + 1u;
}

static void vk_eng_pipeline_release(void* user, u32 pipeline)
{
    (void)user;
    if (!pipeline || pipeline > VK_ENG_MAX_PIPE || !s_epipe[pipeline - 1].used) return;
    vk_eng_pipe* P = &s_epipe[pipeline - 1];
    /* A worker may still be compiling for this slot. Releases only come at
     * shutdown: stop the pool rather than compile the whole queue first. */
    if (VK_AT_LOAD(&P->inflight) > 0) vk_async_stop();
    while (VK_AT_LOAD(&P->inflight) > 0) vk_async_yield();
    free(P->vs_src);
    free(P->ps_src);
    /* Recorded draws may still use the variants: destroyed with the slot. */
    for (u32 t = 0; t < VK_ENG_TOPOLOGIES; t++) vk_defer_pipe(P->variant[t]);
    if (P->msv)
        for (u32 i = 0; i < (VK_EMS_PC - 1u) * VK_ENG_TOPOLOGIES; i++) vk_defer_pipe(P->msv[i]);
    free(P->msv);
    free((void*)P->msvstate);
    /* The modules belong to the shared cache (vk_mod_release_all). */
    memset(P, 0, sizeof *P);
    s_f.b_valid = 0;
    if (s_e2.pipeline == pipeline) s_e2.pipeline = 0;
}

static VkPipeline vk_eng_variant_pc(vk_eng_pipe* P, u32 topo_idx, u32 pc);
static VkPipeline vk_eng_variant(vk_eng_pipe* P, u32 topo_idx) { return vk_eng_variant_pc(P, topo_idx, 0); }
static int s_ems_on;   /* > 0: hardware MSAA / A2C (vk_ems_enabled, render thread) */
static VkPipeline vk_eng_variant_pc(vk_eng_pipe* P, u32 topo_idx, u32 pc)
{
    static const VkPrimitiveTopology topo[VK_ENG_TOPOLOGIES] = {
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    if (pc >= VK_EMS_PC || (pc && !P->msv)) return VK_NULL_HANDLE;
    VkPipeline* const out = pc ? &P->msv[(pc - 1u) * VK_ENG_TOPOLOGIES + topo_idx] : &P->variant[topo_idx];
    if (*out) return *out;
    /* Modules an async job never built (the pool was stopped with it queued):
     * build them here once the pool is down, never a pipeline from NULL. */
    if (!P->vs || !P->fs) {
        if (s_vka.state > 0 || !P->vs_src || !P->ps_src) return VK_NULL_HANDLE;
        if (!P->vs && vk_mod_get(P->vs_src, RSX_SHADER_STAGE_VERTEX, &P->vs, s_g.spv, s_g.log, (u32)sizeof s_g.log)) P->vs = VK_NULL_HANDLE;
        if (P->vs && !P->fs && vk_mod_get(P->ps_src, RSX_SHADER_STAGE_FRAGMENT, &P->fs, s_g.spv, s_g.log, (u32)sizeof s_g.log)) P->fs = VK_NULL_HANDLE;
        if (!P->vs || !P->fs) { VK_AT_STORE(&P->mstate, VK_AS_FAILED); return VK_NULL_HANDLE; }
        VK_AT_STORE(&P->mstate, VK_AS_READY);
    }
    VkRenderPass rp = vk_eng_render_pass_pc(P->rt_fmt, P->rt_count, pc);
    if (!rp || P->nslots > VK_GUEST_ATTRS) return VK_NULL_HANDLE;
    const VkPipelineLayout layout = s_e2.pipe_layout[P->cube_mask != 0];
    if (!layout) return VK_NULL_HANDLE;
    const rsx_be_render_state* rs = &P->rs;

    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = P->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = P->fs, .pName = "main" } };
    VkVertexInputBindingDescription vb = { 0, P->stride, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 s = 0; s < P->nslots; s++)
        va[s] = (VkVertexInputAttributeDescription){ s, 0, VK_FORMAT_R32G32B32A32_SFLOAT, s * 16u };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = P->nslots, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = topo[topo_idx] };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* CULL_FACE FRONT 0x0404 / BACK 0x0405 / FRONT_AND_BACK 0x0408 as RPCS3's
     * vk::get_cull_face (FRONT_AND_BACK culls every polygon, points and lines
     * still draw); FRONT_FACE CCW 0x0901 maps directly: both APIs judge
     * winding on screen, and the Y flip in the vertex stage makes this
     * screen the same one D3D's viewport produces. */
    VkPipelineRasterizationStateCreateInfo ras = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = vk_eng_polygon_mode(rs),
        .cullMode = rsx_vk_cull_mode(rs->cull_enable, rs->cull_face),
        .frontFace = (rs->front_face == 0x0901u) ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                 : VK_FRONT_FACE_CLOCKWISE,
        /* RPCS3 get_pipeline_properties: bias always on (zero when the
         * game disables it), clamp from ZMIN_MAX_CONTROL. */
        .depthClampEnable = (rs->depth_clamp && s_vk.depth_clamp_ok) ? VK_TRUE : VK_FALSE,
        .depthBiasEnable = VK_TRUE,
        .lineWidth = 1.0f,
        .pNext = vk_provoking_chain() };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = s_pc_samples[pc % 3u] };
    /* RPCS3 (VKGSRender get_pipeline_properties): with hardware A2C or a
     * multisampled pass, SET_ANTI_ALIASING_CONTROL drives the sample mask
     * (bits 16..31), alpha-to-coverage (bit 4) and alpha-to-one (bit 8), at a
     * forced shading rate of 1. A register never written reads as its reset
     * value 0xFFFF0000. The mask is only applied with real samples. */
    VkSampleMask smask = 0xFFFFFFFFu;
    if (s_ems_on > 0) {
        const u32 aac = rs->aa_control ? rs->aa_control : 0xFFFF0000u;
        if (pc % 3u) smask = (aac >> 16) & 0xFFFFu;
        ms.pSampleMask = &smask;
        ms.alphaToCoverageEnable = (aac & 0x10u) ? VK_TRUE : VK_FALSE;
        ms.alphaToOneEnable = ((aac & 0x100u) && s_ms_a2one_ok) ? VK_TRUE : VK_FALSE;
        if (s_ms_srs_ok && (pc % 3u)) { ms.sampleShadingEnable = VK_TRUE; ms.minSampleShading = 1.0f; }
    }
    VkStencilOpState front = vk_gcm_stencil_face(rs->s_func, rs->s_fail, rs->s_zfail, rs->s_zpass,
                                                 rs->s_func_mask, rs->s_write_mask);
    VkStencilOpState back = rs->stencil_two_sided
        ? vk_gcm_stencil_face(rs->bs_func, rs->bs_fail, rs->bs_zfail, rs->bs_zpass,
                              rs->bs_func_mask, rs->bs_write_mask)
        : front;
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = rs->depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = (rs->depth_test && rs->depth_write) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = vk_gcm_compare(rs->depth_func),
        .stencilTestEnable = rs->stencil_enable ? VK_TRUE : VK_FALSE,
        .front = front, .back = back,
        /* The bounds themselves are dynamic (vk_eng_set_depth_bounds). */
        .depthBoundsTestEnable = (rs->depth_bounds_test && s_vk.depth_bounds_ok) ? VK_TRUE : VK_FALSE,
        .minDepthBounds = 0.0f, .maxDepthBounds = 1.0f };
    if (rs->depth_bounds_test && !s_vk.depth_bounds_ok && !(s_e2.warned & 64u)) {
        VK_LOG("engine: the device has no depthBounds; the depth bounds test is ignored\n");
        s_e2.warned |= 64u;
    }
    /* Per target as RPCS3's decode_blend_state: target 0 takes COLOR_MASK
     * (nv40: B [0:7], G [8:15], R [16:23], A [24:31], any bit set) and
     * BLEND_ENABLE; targets 1..3 their COLOR_MASK_MRT nibble (A,R,G,B from
     * bit 4i) and BLEND_ENABLE_MRT bit. A logic op replaces blending.
     * RSX_VK_MRT_INHERIT=1 restores the old rule: every target copies
     * target 0's mask and blend. */
    static int mrt_inherit = -1;
    if (mrt_inherit < 0) {
        const char* e = getenv("RSX_VK_MRT_INHERIT");
        mrt_inherit = (e && *e == '1') ? 1 : 0;
    }
    const int logic = rs->logic_op_enable && s_vk.logic_op_ok;
    const int blendable = vk_eng_rt_blendable(P->rt_fmt);
    VkPipelineColorBlendAttachmentState cba[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 r = 0; r < P->rt_count; r++) {
        VkColorComponentFlags wm = 0;
        u32 blend_on;
        if (r == 0 || mrt_inherit) {
            if ((rs->color_mask >> 16) & 0xFFu) wm |= VK_COLOR_COMPONENT_R_BIT;
            if ((rs->color_mask >>  8) & 0xFFu) wm |= VK_COLOR_COMPONENT_G_BIT;
            if ((rs->color_mask >>  0) & 0xFFu) wm |= VK_COLOR_COMPONENT_B_BIT;
            if ((rs->color_mask >> 24) & 0xFFu) wm |= VK_COLOR_COMPONENT_A_BIT;
            blend_on = rs->blend_enable || (rs->blend_mrt & 1u);   /* RPCS3 blend_enabled_mask */
        } else {
            const u32 nib = (rs->color_mask_mrt >> (4u * r)) & 0xFu;
            if (nib & 1u) wm |= VK_COLOR_COMPONENT_A_BIT;
            if (nib & 2u) wm |= VK_COLOR_COMPONENT_R_BIT;
            if (nib & 4u) wm |= VK_COLOR_COMPONENT_G_BIT;
            if (nib & 8u) wm |= VK_COLOR_COMPONENT_B_BIT;
            blend_on = (rs->blend_mrt >> r) & 1u;
        }
        cba[r] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = (blend_on && !logic && blendable) ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = vk_gcm_blend_factor(rs->sf_rgb),
            .dstColorBlendFactor = vk_gcm_blend_factor(rs->df_rgb),
            .colorBlendOp = vk_gcm_blend_op(rs->eq_rgb),
            .srcAlphaBlendFactor = vk_gcm_blend_factor(rs->sf_a),
            .dstAlphaBlendFactor = vk_gcm_blend_factor(rs->df_a),
            .alphaBlendOp = vk_gcm_blend_op(rs->eq_a),
            .colorWriteMask = (VkColorComponentFlags)vk_cv_write_mask(P->rt_fmt, wm) };
    }
    /* GL logic-op enums 0x1500..0x150F are in VkLogicOp's order. */
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = logic ? VK_TRUE : VK_FALSE,
        .logicOp = (logic && rs->logic_op >= 0x1500u && rs->logic_op <= 0x150Fu)
                 ? (VkLogicOp)(rs->logic_op - 0x1500u) : VK_LOGIC_OP_COPY,
        .attachmentCount = P->rt_count, .pAttachments = cba };
    VkDynamicState dyn[7] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                              VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                              VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_LINE_WIDTH,
                              VK_DYNAMIC_STATE_DEPTH_BOUNDS };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = s_vk.depth_bounds_ok ? 7u : 6u, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &ras, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = layout, .renderPass = rp, .subpass = 0 };
    if (pvkCreateGraphicsPipelines(s_vk.device, s_vk.pcache, 1, &gci, NULL,
                                   out) != VK_SUCCESS) {
        VK_LOG("engine: vkCreateGraphicsPipelines failed\n");
        *out = VK_NULL_HANDLE;
    }
    return *out;
}

static void vk_eng_bind_pipeline(void* user, u32 pipeline) { (void)user; s_e2.pipeline = pipeline; }

static void vk_cv_shutdown(void);
static void vk_ems_shutdown(void);

static void vk_eng2_shutdown(void* user)
{
    vk_cv_shutdown();
    for (u32 i = 1; i <= VK_ENG_MAX_PIPE; i++) vk_eng_pipeline_release(user, i);
    vk_eng_drop_framebuffers_for(0);
    vk_flush(1);                         /* nothing in flight uses the passes below */
    for (u32 f = 0; f < VK_ENG_RT_FORMATS; f++)
        for (u32 n = 0; n <= RSX_BE_MAX_COLOR_TARGETS; n++)
            if (s_e2.rp[f][n]) pvkDestroyRenderPass(s_vk.device, s_e2.rp[f][n], NULL);
    for (u32 f = 0; f < VK_ENG_RT_FORMATS; f++)
        for (u32 n = 0; n <= RSX_BE_MAX_COLOR_TARGETS; n++)
            for (u32 c = 0; c < VK_EMS_PC; c++)
                if (s_ms_rp[f][n][c]) { pvkDestroyRenderPass(s_vk.device, s_ms_rp[f][n][c], NULL); s_ms_rp[f][n][c] = VK_NULL_HANDLE; }
    vk_ems_shutdown();
    vk_defer_image(&s_e2.scratch_depth);
    for (u32 k = 0; k < 2; k++) {
        if (s_e2.pipe_layout[k]) pvkDestroyPipelineLayout(s_vk.device, s_e2.pipe_layout[k], NULL);
        if (s_e2.set_layout[k])  pvkDestroyDescriptorSetLayout(s_vk.device, s_e2.set_layout[k], NULL);
    }
    memset(&s_e2, 0, sizeof s_e2);
    for (u32 i = 0; i < s_esamp_count; i++) pvkDestroySampler(s_vk.device, s_esamp[i].s, NULL);
    s_esamp_count = 0;
    s_f.epoch++;                         /* the descriptor cache is stale */
}

/* Constants are kept here and copied into the frame ring (vk_eng_constants)
 * when a draw finds them changed. */
static void vk_eng_bind_vs_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_g.on || !data) { s_e2.vs_bytes = 0; return; }
    u32 cap = (u32)sizeof s_e2.vs_data;   /* own ring slice: no ps_off bound */
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 2u)) { VK_LOG("engine: VS constants truncated\n"); s_e2.warned |= 2u; } }
    if (bytes != s_e2.vs_bytes || memcmp(s_e2.vs_data, data, bytes)) {
        memcpy(s_e2.vs_data, data, bytes);
        s_e2.vs_bytes = bytes;
        s_e2.vs_dirty = 1;
        s_e2.psz_scaled = 0;
    }
}
static void vk_eng_bind_ps_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_g.on || !data) { s_e2.ps_bytes = 0; return; }
    const u32 cap = (u32)sizeof s_e2.ps_data - VK_RS_PS_EXTRA;
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 4u)) { VK_LOG("engine: PS constants truncated\n"); s_e2.warned |= 4u; } }
    if (bytes != s_e2.ps_bytes || memcmp(s_e2.ps_data, data, bytes)) {
        memcpy(s_e2.ps_data, data, bytes);
        s_e2.ps_bytes = bytes;
        s_e2.ps_dirty = 1;
    }
}
static void vk_eng_bind_textures(void* user, const u32* textures,
                                 const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        const int on = (mask >> u) & 1u;
        s_e2.tex[u] = on ? textures[u] : 0;
        if (on && samplers) s_e2.samp[u] = samplers[u];
    }
    s_e2.tex_mask = mask;
}
static void vk_eng_bind_vertex_textures(void* user, const u32* textures,
                                        const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        const int on = textures && ((mask >> u) & 1u);
        s_e2.vtex[u] = on ? textures[u] : 0;
        if (on && samplers) s_e2.vsamp[u] = samplers[u];
    }
    s_e2.vtex_mask = textures ? mask : 0;
}
static void vk_eng_set_viewport(void* user, float x, float y, float w, float h)
{
    (void)user;
    s_e2.vp[0] = x; s_e2.vp[1] = y; s_e2.vp[2] = w; s_e2.vp[3] = h; s_e2.have_vp = 1;
}
static void vk_eng_set_depth_range(void* user, float min_z, float max_z)
{
    (void)user;
    s_e2.zrange[0] = min_z; s_e2.zrange[1] = max_z; s_e2.have_zrange = 1;
}
static void vk_eng_set_scissor(void* user, u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    s_e2.sc[0] = x; s_e2.sc[1] = y; s_e2.sc[2] = w; s_e2.sc[3] = h; s_e2.have_sc = 1;
}
static void vk_eng_set_stencil_ref(void* user, u32 ref) { (void)user; s_e2.stencil_ref = ref; }
static void vk_eng_set_raster_dynamic(void* user, u32 poly_offset_fill, float scale,
                                      float bias, float line_width, u32 back_ref)
{
    (void)user;
    s_e2.dbias[0] = poly_offset_fill ? 1.0f : 0.0f;
    s_e2.dbias[1] = bias;
    s_e2.dbias[2] = scale;
    s_e2.line_width = line_width;
    s_e2.back_ref = 0x100u | (back_ref & 0xFFu);
}

/* Vulkan wants the bounds inside [0, 1] without VK_EXT_depth_range_unrestricted;
 * a NaN clamps to the open end so a garbage register cannot reject a draw. */
static float vk_depth_bound(float v, float open)
{
    if (!(v == v)) return open;
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static void vk_eng_set_depth_bounds(void* user, float min, float max)
{
    (void)user;
    s_e2.depth_bounds[0] = vk_depth_bound(min, 0.0f);
    s_e2.depth_bounds[1] = vk_depth_bound(max, 1.0f);
}

static void vk_eng_set_blend_constants(void* user, const float rgba[4])
{
    (void)user;
    memcpy(s_e2.blend_const, rgba, sizeof s_e2.blend_const);
}

/* Set while vk_eng_framebuffer builds a multisampled pass's framebuffer: the
 * colour attachments are then the targets' MSAA twins (vk_ems_view). */
static int s_ems_fb_ms;
static VkImageView vk_ems_view(u32 handle);
static VkFramebuffer vk_eng_framebuffer(VkRenderPass rp, const u32* rt, u32 n, VkImageView dview,
                                        u32 dkey, u32 w, u32 h)
{
    u32 key[RSX_BE_MAX_COLOR_TARGETS + 1] = { 0 };
    for (u32 i = 0; i < n; i++) key[i] = rt[i];
    key[RSX_BE_MAX_COLOR_TARGETS] = dkey;
    for (u32 i = 0; i < s_e2.fb_count; i++)
        if (s_e2.fb[i].rp == rp && !memcmp(s_e2.fb[i].key, key, sizeof key)) return s_e2.fb[i].fb;
    if (s_e2.fb_count >= VK_ENG_FB_CACHE) vk_eng_drop_framebuffers_for(0);
    VkImageView views[RSX_BE_MAX_COLOR_TARGETS + 1];
    for (u32 i = 0; i < n; i++) views[i] = s_ems_fb_ms ? vk_ems_view(rt[i]) : s_eobj[rt[i]].im.view;
    views[n] = dview;
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = rp, .attachmentCount = n + 1, .pAttachments = views,
                                    .width = w, .height = h, .layers = 1 };
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    memcpy(s_e2.fb[s_e2.fb_count].key, key, sizeof key);
    s_e2.fb[s_e2.fb_count].rp = rp;
    s_e2.fb[s_e2.fb_count++].fb = fb;
    return fb;
}

/* Depth when the draw names no zeta: one scratch image, grown as needed and
 * cleared to the far plane when (re)created. Key 0xFFFFFFFF in the cache. */
static VkImageView vk_eng_scratch_depth(u32 w, u32 h)
{
    if (s_e2.scratch_depth.view && s_e2.scratch_w >= w && s_e2.scratch_h >= h)
        return s_e2.scratch_depth.view;
    vk_eng_drop_framebuffers_for(0xFFFFFFFFu);
    vk_defer_image(&s_e2.scratch_depth);
    s_e2.scratch_w = s_e2.scratch_h = 0;
    /* The format of depth class 0, which the class-0 passes are made for. */
    vk_dcls_setup();
    const VkImageAspectFlags sda = vk_depth_aspect_of(s_dcls_fmt[0]);
    if (vk_create_image(&s_e2.scratch_depth, s_dcls_fmt[0], w, h,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        sda) ||
        vk_eng_new_layout(s_e2.scratch_depth.img, sda)) {
        vk_destroy_image(&s_e2.scratch_depth);
        return VK_NULL_HANDLE;
    }
    VkCommandBuffer cb = vk_rec_upload(&s_f.pre, s_e2.scratch_depth.img);
    if (!cb) return VK_NULL_HANDLE;
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { sda, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(cb, s_e2.scratch_depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    s_e2.scratch_w = w; s_e2.scratch_h = h;
    return s_e2.scratch_depth.view;
}

/* rsx_be_sampler_desc is the registers decoded the D3D12 way; the wrap codes
 * stay the guest's 1..8, which vk_gcm_wrap already translates. */
static VkSampler vk_eng_sampler(const rsx_be_sampler_desc* d)
{
    u32 lo, hi;
    memcpy(&lo, &d->min_lod, 4); memcpy(&hi, &d->max_lod, 4);
    /* RPCS3 VKDraw.cpp: the bias applies only with mip sampling, rounded to
     * 0.5 for NEAREST mip mode; bounded only by the device limit. */
    float bias = d->mip_present ? d->lod_bias : 0.0f;
    if (d->mip_present && !d->mip_linear) bias = floorf(bias * 2.0f + 0.5f) * 0.5f;
    const float bmax = s_vk.lod_bias_max > 0.0f ? s_vk.lod_bias_max : 15.0f;
    if (bias > bmax) bias = bmax;
    if (bias < -bmax) bias = -bmax;
    /* The engine already resolved the colour RPCS3 would use (opaque black
     * unless is_border_clamped_texture, sign/SNORM/remap applied); the
     * border only reaches a sample through a CLAMP_TO_BORDER axis. */
    const int wraps_border = (d->wrap_s & 0xF) == 4 || (d->wrap_t & 0xF) == 4 || (d->wrap_r & 0xF) == 4;
    u32 bkey = 0;
    const VkBorderColor border = wraps_border ? vk_eng_border(d->border_rgba, &bkey)
                                              : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    const u64 bcode = border == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT ? 7u : (u64)border;
    const u64 key = (u64)d->min_linear | ((u64)d->mag_linear << 1) | ((u64)d->mip_linear << 2)
                  | ((u64)d->mip_present << 3) | ((u64)(d->wrap_s & 0xF) << 4)
                  | ((u64)(d->wrap_t & 0xF) << 8) | ((u64)(d->wrap_r & 0xF) << 12)
                  | ((u64)(lo >> 16) << 16) | ((u64)(hi >> 16) << 32)
                  | ((u64)((u32)(s32)(bias * 256.0f) & 0x1FFFu) << 48)
                  | (bcode << 61);
    /* RPCS3 VKDraw.cpp load_texture_env / vk::sampler: the unit's
     * CONTROL0 level whatever the filters, enabled from 2x when the device
     * supports anisotropy; vertex units pass 1. */
    u8 aniso = (s_vk.aniso_max > 1.0f) ? d->max_aniso : 1;
    if ((float)aniso > s_vk.aniso_max) aniso = (u8)s_vk.aniso_max;
    if (aniso < 1) aniso = 1;
    /* RSX_PERF_VK_SAMPHINT: where this key was last found. Keys are unique
     * in s_esamp (added only after a miss), so a hint whose entry holds the
     * same key is what the scan finds. */
    static int perf_sh = -1;
    static u16 s_samp_hint[256];
    const int sh = vk_perf_on("RSX_PERF_VK_SAMPHINT", &perf_sh);
    u32 hslot = 0;
    if (sh) {
        u64 hk = (key ^ ((u64)bkey << 7) ^ ((u64)aniso << 57)) * 0x9E3779B97F4A7C15ull;
        hslot = (u32)(hk >> 56);
        const u32 hi = s_samp_hint[hslot];
        if (hi && hi - 1u < s_esamp_count && s_esamp[hi - 1u].key == key &&
            s_esamp[hi - 1u].bkey == bkey && s_esamp[hi - 1u].aniso == aniso)
            return s_esamp[hi - 1u].s;
    }
    for (u32 i = 0; i < s_esamp_count; i++)
        if (s_esamp[i].key == key && s_esamp[i].bkey == bkey && s_esamp[i].aniso == aniso) {
            if (sh) s_samp_hint[hslot] = (u16)(i + 1u);
            return s_esamp[i].s;
        }
    if (s_esamp_count >= VK_ENG_SAMP_CACHE) return s_vk.sampler;
    float min_lod = d->min_lod, max_lod = d->mip_present ? d->max_lod : d->min_lod;
    if (max_lod < min_lod) max_lod = min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = d->mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = d->min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = d->mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(d->wrap_s), .addressModeV = vk_gcm_wrap(d->wrap_t),
        .addressModeW = vk_gcm_wrap(d->wrap_r),
        .minLod = min_lod, .maxLod = max_lod, .mipLodBias = bias,
        .anisotropyEnable = aniso > 1 ? VK_TRUE : VK_FALSE, .maxAnisotropy = (float)aniso,
        .borderColor = border };
    VkSamplerCustomBorderColorCreateInfoEXT cbi;
    if (border == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT) {
        memset(&cbi, 0, sizeof cbi);
        cbi.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT;
        cbi.format = VK_FORMAT_UNDEFINED;
        memcpy(cbi.customBorderColor.float32, d->border_rgba, sizeof(float) * 4);
        sci.pNext = &cbi;
    }
    VkSampler smp;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &smp) != VK_SUCCESS) return s_vk.sampler;
    s_esamp[s_esamp_count].aniso = aniso;
    s_esamp[s_esamp_count].key = key;
    s_esamp[s_esamp_count].bkey = bkey;
    s_esamp[s_esamp_count++].s = smp;
    return smp;
}

/* The view a bound engine handle samples through: an uploaded texture, a
 * colour target (bound directly when surface_view was not asked), or a view.
 * A unit gets a view of its own kind only -- a cube for a TextureCube unit, a
 * 2D one otherwise -- and NULL for anything else, which then samples the
 * dummy of the unit's kind. */
static VkImageView vk_eng_sample_view(u32 handle, int want_cube)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return VK_NULL_HANDLE;
    const vk_eng_obj* o = &s_eobj[handle];
    if (o->kind != VK_ENG_TEXTURE && o->kind != VK_ENG_COLOR && o->kind != VK_ENG_VIEW)
        return VK_NULL_HANDLE;
    if ((int)o->cube != want_cube) return VK_NULL_HANDLE;
    return (o->kind == VK_ENG_COLOR && o->sv.view) ? o->sv.view : o->im.view;
}

/* The image behind a sampled handle (a view's surface), for feedback checks. */
static VkImage vk_eng_sample_image(u32 handle)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return VK_NULL_HANDLE;
    const vk_eng_obj* o = &s_eobj[handle];
    if (o->kind == VK_ENG_VIEW)
        return (o->surface && o->surface < VK_ENG_MAX_OBJ) ? s_eobj[o->surface].im.img : VK_NULL_HANDLE;
    return o->im.img;
}

/* What a cube unit with nothing bound samples: a 1x1 cube, transparent black
 * on every face like the 2D dummy (RPCS3 null_image_view), made on first use. */
static VkImageView vk_eng_dummy_cube_view(void)
{
    if (!vk_eng_get(s_e2.dummy_cube, VK_ENG_TEXTURE)) {
        static const u8 magenta[4] = { 0x00, 0x00, 0x00, 0x00 };
        s_e2.dummy_cube = vk_eng_tex_new(RSX_BE_FMT_R8G8B8A8, 1, 1, 1, 6, 0xAAE4u, 0x85u);
        if (!s_e2.dummy_cube) return VK_NULL_HANDLE;
        for (u32 f = 0; f < 6; f++)
            vk_eng_tex_upload(s_e2.dummy_cube, f, 0, 1, 1, magenta, 4, 1);
    }
    return s_eobj[s_e2.dummy_cube].im.view;
}

/* What a Texture3D unit with nothing bound samples: a 1x1x1 transparent
 * black volume (RPCS3 null_image_view). */
static VkImageView vk_eng_dummy_3d_view(void)
{
    if (!vk_eng_get(s_e2.dummy_3d, VK_ENG_TEXTURE)) {
        static const u8 magenta[4] = { 0x00, 0x00, 0x00, 0x00 };
        s_e2.dummy_3d = vk_eng_tex3d_new(RSX_BE_FMT_R8G8B8A8, 1, 1, 1, 1, 0xAAE4u, 0x85u);
        if (!s_e2.dummy_3d) return VK_NULL_HANDLE;
        vk_eng_tex3d_upload(s_e2.dummy_3d, 0, 0, 1, 1, magenta, 4);
    }
    return s_eobj[s_e2.dummy_3d].im.view;
}

/* This draw's constants as frame-ring slices: copied once per change (and
 * once per frame slot), bound through dynamic offsets. */
static int vk_eng_constants(void)
{
    if (s_e2.vs_dirty || s_e2.vs_epoch != s_f.epoch || !s_e2.vs_buf) {
        const u32 n = s_e2.vs_bytes ? s_e2.vs_bytes : 16u;
        u8* p = (u8*)vk_ring(n, s_f.ubo_align, &s_e2.vs_buf, &s_e2.vs_off);
        if (!p) { s_e2.vs_buf = VK_NULL_HANDLE; return -1; }
        memcpy(p, s_e2.vs_data, n);
        s_e2.vs_dirty = 0;
        s_e2.vs_epoch = s_f.epoch;
    }
    if (s_e2.ps_dirty || s_e2.ps_epoch != s_f.epoch || !s_e2.ps_buf) {
        const u32 n = s_e2.ps_bytes ? s_e2.ps_bytes + s_e2.ps_extra : 16u;
        u8* p = (u8*)vk_ring(n, s_f.ubo_align, &s_e2.ps_buf, &s_e2.ps_off);
        if (!p) { s_e2.ps_buf = VK_NULL_HANDLE; return -1; }
        memcpy(p, s_e2.ps_data, n);
        s_e2.ps_dirty = 0;
        s_e2.ps_epoch = s_f.epoch;
    }
    return 0;
}

/* The set for the bound pipeline: the shared layout (sixteen textures in one
 * binding), or for a pipeline with cube units the split one (one binding per
 * unit). Sets come from the frame's pools and are reused while the same
 * buffers, views and samplers are bound in the same frame slot. */
typedef struct vk_dkey {
    VkBuffer    vs_buf, ps_buf;
    u32         vs_range, ps_range;
    u32         split, pad;
    VkImageView view[RSX_MAX_TEXTURES];
    VkSampler   samp[RSX_MAX_TEXTURES];
    VkImageView vview[RSX_BE_MAX_VERTEX_TEXTURES];
    VkSampler   vsamp[RSX_BE_MAX_VERTEX_TEXTURES];
} vk_dkey;
_Static_assert(sizeof(vk_dkey) % 8u == 0, "vk_dkey hashes as 64-bit words");

static struct { vk_dkey key; u32 epoch; VkDescriptorSet set; } s_dcache[VK_DCACHE];

/* RPCS3 VKDraw.cpp:459-466: a depth-class image is point-sampled when the
 * device cannot filter its format linearly (colour formats are not queried). */
static int vk_eng_can_filter(u32 handle)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return 1;
    const VkFormat f = s_eobj[handle].fmt;
    if (f != VK_FORMAT_D16_UNORM && f != VK_FORMAT_D32_SFLOAT && f != VK_FORMAT_X8_D24_UNORM_PACK32 &&
        f != VK_FORMAT_D24_UNORM_S8_UINT && f != VK_FORMAT_D32_SFLOAT_S8_UINT &&
        f != VK_FORMAT_D16_UNORM_S8_UINT)
        return 1;
    static struct { VkFormat f; u8 ok; } cache[16];
    static u32 n;
    for (u32 i = 0; i < n; i++) if (cache[i].f == f) return cache[i].ok;
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, f, &fp);
    const u8 ok = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? 1 : 0;
    if (n < 16) { cache[n].f = f; cache[n].ok = ok; n++; }
    return ok;
}

static VkDescriptorSet vk_eng_write_descriptors(const vk_eng_pipe* P)
{
    const int split = P->cube_mask != 0;
    const VkImageView dummy_cube = split ? vk_eng_dummy_cube_view() : VK_NULL_HANDLE;
    if (split && !dummy_cube) return VK_NULL_HANDLE;
    vk_dkey k;
    memset(&k, 0, sizeof k);
    k.vs_buf = s_e2.vs_buf; k.ps_buf = s_e2.ps_buf;
    k.vs_range = s_e2.vs_bytes ? s_e2.vs_bytes : 16u;
    k.ps_range = s_e2.ps_bytes ? s_e2.ps_bytes + s_e2.ps_extra : 16u;
    k.split = (u32)split;
    const VkImageView dummy_3d = ((P->cube_mask | P->vcube_mask) >> 16) ? vk_eng_dummy_3d_view()
                                                                         : VK_NULL_HANDLE;
    if (((P->cube_mask | P->vcube_mask) >> 16) && !dummy_3d) return VK_NULL_HANDLE;
    const VkImageView vdummy_cube = (P->vcube_mask & 0xFFFFu) ? vk_eng_dummy_cube_view()
                                                              : VK_NULL_HANDLE;
    if ((P->vcube_mask & 0xFFFFu) && !vdummy_cube) return VK_NULL_HANDLE;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const int cube = ((P->cube_mask >> u) & 1u) ? 1 : ((P->cube_mask >> (u + 16u)) & 1u) ? 2 : 0;
        VkImageView v = ((s_e2.tex_mask >> u) & 1u) ? vk_eng_sample_view(s_e2.tex[u], cube)
                                                     : VK_NULL_HANDLE;
        if (v && !vk_eng_can_filter(s_e2.tex[u])) {
            rsx_be_sampler_desc nd = s_e2.samp[u];
            nd.min_linear = nd.mag_linear = nd.mip_linear = 0;
            k.samp[u] = vk_eng_sampler(&nd);
        } else {
            k.samp[u] = v ? vk_eng_sampler(&s_e2.samp[u]) : s_vk.sampler;
        }
        k.view[u] = v ? v : (cube == 2 ? dummy_3d : cube ? dummy_cube : s_vk.dummy.view);
    }
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        const int vkind = ((P->vcube_mask >> u) & 1u) ? 1 : ((P->vcube_mask >> (u + 16u)) & 1u) ? 2 : 0;
        VkImageView v = ((s_e2.vtex_mask >> u) & 1u) ? vk_eng_sample_view(s_e2.vtex[u], vkind)
                                                      : VK_NULL_HANDLE;
        k.vsamp[u] = v ? vk_eng_sampler(&s_e2.vsamp[u]) : s_vk.sampler;
        k.vview[u] = v ? v : (vkind == 2 ? dummy_3d : vkind ? vdummy_cube : s_vk.dummy.view);
    }
    static int perf_dh = -1;
    u64 dh;
    if (vk_perf_on("RSX_PERF_VK_DHASH", &perf_dh)) {
        /* Word-wise (the key is all 8-byte fields); only picks the slot,
         * which memcmp then confirms. */
        const u64* q = (const u64*)&k;
        dh = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < sizeof k / 8u; i++) { dh ^= q[i]; dh *= 0x100000001B3ull; dh ^= dh >> 29; }
    } else {
        dh = vk_fnv1a64(&k, sizeof k, 0xcbf29ce484222325ull);
    }
    const u32 slot = (u32)(dh % VK_DCACHE);
    if (s_dcache[slot].set && s_dcache[slot].epoch == s_f.epoch &&
        !memcmp(&s_dcache[slot].key, &k, sizeof k))
        return s_dcache[slot].set;

    const VkDescriptorSet set = vk_fr_desc(s_e2.set_layout[split]);
    if (!set) return VK_NULL_HANDLE;
    VkDescriptorBufferInfo bi[2] = {
        { k.vs_buf, 0, k.vs_range },
        { k.ps_buf, 0, k.ps_range },
    };
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, k.view[u], VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ k.samp[u], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[3 + RSX_MAX_TEXTURES + 2u * RSX_BE_MAX_VERTEX_TEXTURES];
    VkDescriptorImageInfo vii[RSX_BE_MAX_VERTEX_TEXTURES], vsi[RSX_BE_MAX_VERTEX_TEXTURES];
    u32 n = 0;
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi[0] };
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_PSCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi[1] };
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si };
    if (split) {
        for (u32 u = 0; u < RSX_MAX_TEXTURES; u++)
            w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                  .dstSet = set, .dstBinding = RSX_SPIRV_TEXTURE_BINDING + u, .descriptorCount = 1,
                  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii[u] };
    } else {
        w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
              .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii };
    }
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        vii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, k.vview[u], VK_IMAGE_LAYOUT_GENERAL };
        vsi[u] = (VkDescriptorImageInfo){ k.vsamp[u], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
              .dstBinding = VK_ENG_VTEX_TEX_BINDING + u, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &vii[u] };
        w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
              .dstBinding = VK_ENG_VTEX_SAMP_BINDING + u, .descriptorCount = 1,
              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &vsi[u] };
    }
    pvkUpdateDescriptorSets(s_vk.device, n, w, 0, NULL);
    s_dcache[slot].key = k;
    s_dcache[slot].epoch = s_f.epoch;
    s_dcache[slot].set = set;
    return set;
}

/* Hardware MSAA for a draw (the vk_ems block below): whether it renders the
 * targets' multisampled twins, and the pass geometry if so. */
typedef struct {
    u32 sx, sy, sidx, pw, ph, dkey;
    VkImageView dview;
    VkImage dimg;
} vk_ems_drawinfo;
static int  vk_ems_draw_begin(const u32* rt, u32 n, u32 depth, vk_ems_drawinfo* di);
static void vk_ems_draw_end(const u32* rt, u32 n, u32 depth, int ms);
static VkImage vk_ems_image(u32 handle);
static int s_ems_pass_ms;   /* the open s_f.pass renders MSAA twins */

/* Draws share one render pass while they target the same framebuffer; it
 * ends for anything that cannot run inside one (a copy, an upload into a
 * used image, a readback), a target change, or a draw sampling one of the
 * pass's own attachments (so it sees what the draws before it wrote). */
static void vk_eng_draw(void* user, rsx_topology topology, const void* vertices, u32 vertex_count,
                        u32 stride, const u32* indices, u32 index_count)
{
    (void)user;
    if (!s_e2.pipeline || !vertices || !vertex_count || !stride) return;
    if (topology < RSX_TOPOLOGY_POINTS || topology > RSX_TOPOLOGY_TRIANGLE_STRIP) return;
    vk_eng_pipe* P = &s_epipe[s_e2.pipeline - 1];
    if (!P->used || stride != P->stride) return;

    /* Targets: every member of a set is target A's size (engine contract). */
    const u32 n = s_eng_bound.nrt;
    if (n < 1 || n != P->rt_count) {
        if (!(s_e2.warned & 16u)) { VK_LOG("engine: draw with %u colour targets for a %u-target pipeline skipped\n", n, P->rt_count); s_e2.warned |= 16u; }
        return;
    }
    vk_eng_obj* rt0 = vk_eng_get(s_eng_bound.rt[0], VK_ENG_COLOR);
    if (!rt0) return;
    for (u32 i = 1; i < n; i++)
        if (!vk_eng_get(s_eng_bound.rt[i], VK_ENG_COLOR)) return;
    if (rt0->fmt != P->rt_fmt) {
        if (!(s_e2.warned & 32u)) { VK_LOG("engine: draw into colour format %d with a pipeline built for %d skipped\n", (int)rt0->fmt, (int)P->rt_fmt); s_e2.warned |= 32u; }
        return;
    }
    vk_op_begin();
    vk_rs_match(s_eng_bound.rt, n, s_eng_bound.depth);
    u32 w = vk_pw(rt0), h = vk_ph(rt0);
    const float rs = vk_rs_of(rt0);
    vk_eng_obj* zo = vk_eng_get(s_eng_bound.depth, VK_ENG_DEPTH);
    int zfits = zo && vk_pw(zo) >= w && vk_ph(zo) >= h;
    if (zfits && vk_rs_of(zo) != rs) {      /* attachments of one pass share a scale */
        zfits = 0;
        if (!(s_e2.warned & 0x40000000u)) { VK_LOG("engine: depth and colour at different scales: scratch depth\n"); s_e2.warned |= 0x40000000u; }
    }
    /* Hardware MSAA (vk_ems_draw_begin): the pass renders the targets' MSAA
     * twins at pixel size, w / sx by h / sy, instead of the widened images. */
    vk_ems_drawinfo emi;
    const int ems = vk_ems_draw_begin(s_eng_bound.rt, n, zfits ? s_eng_bound.depth : 0, &emi);
    VkImageView dview;
    int own_depth;
    u32 dkey;
    VkImage dimg;
    if (ems) {
        w = emi.pw; h = emi.ph;
        dview = emi.dview; dkey = emi.dkey; dimg = emi.dimg;
        own_depth = emi.dkey == s_eng_bound.depth && zfits;
    } else {
        dview = zfits ? zo->im.view : vk_eng_scratch_depth(w, h);
        own_depth = zo && dview == zo->im.view;
        dkey = own_depth ? s_eng_bound.depth : 0xFFFFFFFFu;
        dimg = own_depth ? zo->im.img : s_e2.scratch_depth.img;
    }
    if (!dview) goto skip;
    const u32 pcls = (own_depth ? (u32)zo->dcls : 0u) * 3u + (ems ? emi.sidx : 0u);

    VkPipeline pipe = vk_async_variant_pc(P, (u32)topology - 1u, pcls);
    VkRenderPass rp = vk_eng_render_pass_pc(P->rt_fmt, n, pcls);
    s_ems_fb_ms = ems;
    VkFramebuffer fb = rp ? vk_eng_framebuffer(rp, s_eng_bound.rt, n, dview, dkey, w, h) : VK_NULL_HANDLE;
    s_ems_fb_ms = 0;
    if (!pipe || !rp || !fb) goto skip;

    VkBuffer vbuf = VK_NULL_HANDLE, ibuf = VK_NULL_HANDLE;
    VkDeviceSize voff = 0, ioff = 0;
    const VkDeviceSize vbytes = (VkDeviceSize)vertex_count * stride;
    void* vp_mem = vk_ring(vbytes, 16, &vbuf, &voff);
    if (!vp_mem) goto skip;
    memcpy(vp_mem, vertices, (size_t)vbytes);
    if (indices && index_count) {
        void* ip = vk_ring((VkDeviceSize)index_count * 4u, 4, &ibuf, &ioff);
        if (!ip) goto skip;
        memcpy(ip, indices, (size_t)index_count * 4u);
    }
    if (s_rs.dyn && s_e2.ps_bytes) {
        float rk[VK_RS_PS_EXTRA / 4u];
        memset(rk, 0, sizeof rk);
        rk[0] = 1.0f / rs;
        for (u32 u = 0; u < RSX_MAX_TEXTURES && u < 16u; u++) {
            const u32 th = s_e2.tex[u];
            rk[4 + u] = ((s_e2.tex_mask >> u) & 1u) && th && th < VK_ENG_MAX_OBJ
                        ? 1.0f / vk_rs_of(&s_eobj[th]) : 1.0f;
        }
        u8* dst = s_e2.ps_data + s_e2.ps_bytes;
        if (s_e2.ps_extra != VK_RS_PS_EXTRA || memcmp(dst, rk, sizeof rk)) {
            memcpy(dst, rk, sizeof rk);
            s_e2.ps_extra = VK_RS_PS_EXTRA;
            s_e2.ps_dirty = 1;
        }
    }
    {   /* the register point size is in render pixels */
        const float prs = topology == RSX_TOPOLOGY_POINTS ? rs : 1.0f;
        if ((prs > 1.0f || s_e2.psz_scaled) && s_e2.vs_bytes >= VK_RS_PSZ_OFF + 4u) {
            if (!s_e2.psz_scaled) memcpy(&s_e2.psz_orig, s_e2.vs_data + VK_RS_PSZ_OFF, 4);
            const float v = s_e2.psz_orig * prs;
            if (memcmp(s_e2.vs_data + VK_RS_PSZ_OFF, &v, 4)) {
                memcpy(s_e2.vs_data + VK_RS_PSZ_OFF, &v, 4);
                s_e2.vs_dirty = 1;
            }
            /* and the program's own point size (rsx_vp_decompiler: 0 = 1,
             * which is what the engine leaves there) */
            const float ps = prs > 1.0f ? prs : 0.0f;
            if (memcmp(s_e2.vs_data + VK_RS_PSZ_SCALE_OFF, &ps, 4)) {
                memcpy(s_e2.vs_data + VK_RS_PSZ_SCALE_OFF, &ps, 4);
                s_e2.vs_dirty = 1;
            }
            s_e2.psz_scaled = prs > 1.0f;
        }
    }
    if (vk_eng_constants()) goto skip;
    const VkDescriptorSet set = vk_eng_write_descriptors(P);
    if (!set) goto skip;

    /* Everything this draw uses is now referenced by `main`. */
    int feedback = 0;
    for (u32 i = 0; i < n; i++) vk_eng_touch(s_eng_bound.rt[i]);
    if (own_depth) zo->fresh = 0;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        if (!((s_e2.tex_mask >> u) & 1u)) continue;
        vk_eng_touch(s_e2.tex[u]);
        const VkImage si = vk_eng_sample_image(s_e2.tex[u]);
        if (s_f.pass && si)
            for (u32 i = 0; i < s_f.pass_nrt; i++) if (s_f.pass_img[i] == si) feedback = 1;
    }
    /* vertex textures: same ordering (an upload after this draw must not move
     * ahead of it into `pre`) and the same attachment-feedback rule */
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        if (!((s_e2.vtex_mask >> u) & 1u)) continue;
        vk_eng_touch(s_e2.vtex[u]);
        const VkImage si = vk_eng_sample_image(s_e2.vtex[u]);
        if (s_f.pass && si)
            for (u32 i = 0; i < s_f.pass_nrt; i++) if (s_f.pass_img[i] == si) feedback = 1;
    }

    if (!s_f.pass || feedback || s_f.pass_fb != fb || s_f.pass_rp != rp ||
        s_f.pass_w != w || s_f.pass_h != h) {
        vk_pass_end();
        VkCommandBuffer cb0 = vk_rec_get(&s_f.main);
        if (!cb0) goto skip;
        if (s_f.main.other || s_f.main.nbatch) vk_rec_barrier(&s_f.main);
        s_f.main.other = 1;
        VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                      .renderPass = rp, .framebuffer = fb,
                                      .renderArea = { { 0, 0 }, { w, h } } };
        vkq_on_pass_begin(cb0, 0);
        pvkCmdBeginRenderPass(cb0, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        s_f.pass = 1;
        vkq_on_pass_begin(cb0, 1);
        s_f.pass_rp = rp; s_f.pass_fb = fb; s_f.pass_w = w; s_f.pass_h = h;
        s_f.pass_nrt = n;
        for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++) {
            s_f.pass_rt[i] = i < n ? s_eng_bound.rt[i] : 0;
            s_f.pass_img[i] = i < n ? (ems ? vk_ems_image(s_eng_bound.rt[i]) : s_eobj[s_eng_bound.rt[i]].im.img)
                                    : VK_NULL_HANDLE;
        }
        s_f.pass_depth = dimg;
        s_ems_pass_ms = ems;
    }
    VkCommandBuffer cb = s_f.main.cb;

    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    if (s_e2.have_vp && s_e2.vp[2] > 0.0f && s_e2.vp[3] > 0.0f) {
        vp.x = s_e2.vp[0]; vp.y = s_e2.vp[1]; vp.width = s_e2.vp[2]; vp.height = s_e2.vp[3];
        if (rs > 1.0f) { vp.x *= rs; vp.y *= rs; vp.width *= rs; vp.height *= rs; }
        if (ems) {      /* the engine speaks widened texels; the pass is in pixels */
            vp.x /= (float)emi.sx; vp.width /= (float)emi.sx;
            vp.y /= (float)emi.sy; vp.height /= (float)emi.sy;
        }
    }
    if (s_e2.have_zrange && s_vk.depth_unrestricted) {
        vp.minDepth = s_e2.zrange[0]; vp.maxDepth = s_e2.zrange[1];
    }
    VkRect2D sc = { { 0, 0 }, { w, h } };
    if (s_e2.have_sc) {                       /* clamped: Vulkan rejects negative or over-size */
        u32 sx0 = s_e2.sc[0], sy0 = s_e2.sc[1], sx1 = s_e2.sc[0] + s_e2.sc[2], sy1 = s_e2.sc[1] + s_e2.sc[3];
        if (rs > 1.0f) {     /* RPCS3 apply_resolution_scale<false> */
            sx0 = vk_rs_c(rt0, sx0 < rt0->w ? sx0 : rt0->w, 0xFFFFFFFFu);
            sy0 = vk_rs_c(rt0, sy0 < rt0->h ? sy0 : rt0->h, 0xFFFFFFFFu);
            sx1 = vk_rs_c(rt0, sx1 < rt0->w ? sx1 : rt0->w, 0xFFFFFFFFu);
            sy1 = vk_rs_c(rt0, sy1 < rt0->h ? sy1 : rt0->h, 0xFFFFFFFFu);
        }
        if (ems) {
            sx0 /= emi.sx; sy0 /= emi.sy;
            sx1 = (sx1 + emi.sx - 1u) / emi.sx; sy1 = (sy1 + emi.sy - 1u) / emi.sy;
        }
        u32 x0 = sx0 < w ? sx0 : w, y0 = sy0 < h ? sy0 : h;
        u32 x1 = sx1, y1 = sy1;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        sc.offset.x = (int32_t)x0; sc.offset.y = (int32_t)y0;
        sc.extent.width = x1 > x0 ? x1 - x0 : 0; sc.extent.height = y1 > y0 ? y1 - y0 : 0;
    }
    const int valid = s_f.b_valid;
    if (!valid || s_f.b_pipe != pipe) {
        pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        s_f.b_pipe = pipe;
    }
    if (!valid || memcmp(&s_f.b_vp, &vp, sizeof vp)) { pvkCmdSetViewport(cb, 0, 1, &vp); s_f.b_vp = vp; }
    if (!valid || memcmp(&s_f.b_sc, &sc, sizeof sc)) { pvkCmdSetScissor(cb, 0, 1, &sc); s_f.b_sc = sc; }
    const u32 sref = s_e2.stencil_ref & 0xFFu;
    const u32 bref = (s_e2.back_ref & 0x100u) ? (s_e2.back_ref & 0xFFu) : sref;
    if (!valid || s_f.b_sref != sref || s_f.b_bref != bref) {
        if (bref == sref) {
            pvkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sref);
        } else {
            pvkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, sref);
            pvkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, bref);
        }
        s_f.b_sref = sref; s_f.b_bref = bref;
    }
    /* RPCS3 VKDraw update_draw_state: vkCmdSetDepthBias(bias, 0, scale) when
     * POLY_OFFSET_FILL is on, else zeros. The constant is in the zeta
     * format's units (halved on NVIDIA with D24S8, x256 for a Z16 zeta held
     * in a 24-bit / float image) and does not scale with the resolution; the
     * slope passes through (rsx_vk_scale_math.h). Line width: x scale with
     * wideLines, 1 without, clamped to lineWidthRange. */
    {
        const VkFormat zf = own_depth ? zo->fmt : s_dcls_fmt[0];
        const int zk = zf == VK_FORMAT_D16_UNORM ? RSX_VK_ZHOST_UNORM16
                     : (zf == VK_FORMAT_D24_UNORM_S8_UINT || zf == VK_FORMAT_X8_D24_UNORM_PACK32)
                     ? RSX_VK_ZHOST_UNORM24 : RSX_VK_ZHOST_FLOAT;
        const int bon = s_e2.dbias[0] != 0.0f;
        const float db[3] = { s_e2.dbias[0],
                              bon ? rsx_vk_depth_bias_constant(s_e2.dbias[1], own_depth && zo->gz16,
                                                               zk, s_vk.nv_d24_bias_half) : 0.0f,
                              bon ? rsx_vk_depth_bias_slope(s_e2.dbias[2], rs, vk_bias_slope_rs()) : 0.0f };
        if (!valid || memcmp(s_f.b_dbias, db, sizeof db)) {
            pvkCmdSetDepthBias(cb, db[1], 0.0f, db[2]);
            memcpy(s_f.b_dbias, db, sizeof db);
        }
        const float lw = rsx_vk_line_width(s_e2.line_width, rs, s_vk.wide_lines_ok,
                                           s_vk.line_w_min, s_vk.line_w_max);
        if (!valid || s_f.b_lw != lw) { pvkCmdSetLineWidth(cb, lw); s_f.b_lw = lw; }
    }
    if (s_vk.depth_bounds_ok &&
        (!valid || memcmp(s_f.b_db, s_e2.depth_bounds, sizeof s_f.b_db))) {
        pvkCmdSetDepthBounds(cb, s_e2.depth_bounds[0], s_e2.depth_bounds[1]);
        s_f.b_db[0] = s_e2.depth_bounds[0]; s_f.b_db[1] = s_e2.depth_bounds[1];
    }
    if (!valid || memcmp(s_f.b_bc, s_e2.blend_const, sizeof s_f.b_bc)) {
        pvkCmdSetBlendConstants(cb, s_e2.blend_const);
        memcpy(s_f.b_bc, s_e2.blend_const, sizeof s_f.b_bc);
    }
    const u32 doff[2] = { (u32)s_e2.vs_off, (u32)s_e2.ps_off };
    const u32 split = P->cube_mask ? 1u : 0u;
    if (!valid || s_f.b_set != set || s_f.b_split != split ||
        s_f.b_doff[0] != doff[0] || s_f.b_doff[1] != doff[1]) {
        pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_e2.pipe_layout[split],
                                 0, 1, &set, 2, doff);
        s_f.b_set = set; s_f.b_split = split;
        s_f.b_doff[0] = doff[0]; s_f.b_doff[1] = doff[1];
    }
    if (!valid || s_f.b_vbuf != vbuf || s_f.b_voff != voff) {
        pvkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &voff);
        s_f.b_vbuf = vbuf; s_f.b_voff = voff;
    }
    if (indices && index_count) {
        if (!valid || s_f.b_ibuf != ibuf || s_f.b_ioff != ioff) {
            pvkCmdBindIndexBuffer(cb, ibuf, ioff, VK_INDEX_TYPE_UINT32);
            s_f.b_ibuf = ibuf; s_f.b_ioff = ioff;
        }
        pvkCmdDrawIndexed(cb, index_count, 1, 0, 0, 0);
    } else {
        pvkCmdDraw(cb, vertex_count, 1, 0, 0);
    }
    vk_ems_draw_end(s_eng_bound.rt, n, own_depth ? s_eng_bound.depth : 0, ems);
    s_f.b_valid = 1;
    s_f.fr[s_f.cur].draws++;
    vk_op_done();
    return;
skip:   /* nothing recorded: the twins stay as vk_ems_draw_begin left them */
    vk_ems_draw_end(s_eng_bound.rt, n, own_depth ? s_eng_bound.depth : 0, -1);
    vk_op_done();
}

/* ---- surface reinterpretation (RPCS3 typeless / bitcast sampling) --------
 * A texture naming a colour or depth target's bytes in another format (depth
 * read as A8R8G8B8, an RGBA8 target read as X32_FLOAT or Y16_X16, ...) is
 * built on the GPU, as RPCS3 does with its typeless copies and conversion
 * passes, instead of storing the target to guest memory and decoding it there
 * (a submit-and-wait per use). One fullscreen pass per (source kind, texture
 * format) rebuilds each texel's guest word from the target -- exactly the
 * bytes the engine's guest-memory store writes -- and emits the host texel
 * rsx_texture_decode makes of them as raw bits into an R32_UINT / R16_UINT
 * image. A buffer copy then moves those bits into a texture of the decoded
 * host format, so the result is bit-identical to the guest-memory path's
 * upload (float formats included: no float conversion on the way). Depth is
 * read through the target's R32F snapshot (vk_eng_depth_snapshot). */
/* Masked / scissored clears (vk_eng_clear_rect): pipelines keyed by render
 * pass, channel mask, stencil mask and the clear colour baked into them. */
#define VK_CLR_PIPES 32
static struct {
    VkPipelineLayout pl;
    struct { VkRenderPass rp; u32 n, cmask, smask; float rgba[4]; VkPipeline p; } pipe[VK_CLR_PIPES];
    u32 npipe;
} s_clr;

#define VK_CV_PIPES 32
#define VK_CV_POOL  4
static struct {
    int                   failed;
    VkRenderPass          rp[2];            /* [0] R32_UINT, [1] R16_UINT  */
    VkDescriptorSetLayout dsl;
    VkPipelineLayout      pl;
    VkShaderModule        vs;
    struct { u32 key; VkPipeline p; } pipe[VK_CV_PIPES];
    u32                   npipe;
    struct { vk_image im; VkFramebuffer fb; u32 w, h; int f16; } pool[VK_CV_POOL];
    u32                   npool;
} s_cv;

static void vk_tl_shutdown(void);   /* the typeless copy's pipelines (below) */

static void vk_cv_shutdown(void)
{
    vk_tl_shutdown();
    for (u32 i = 0; i < s_cv.npipe; i++) vk_defer_pipe(s_cv.pipe[i].p);
    for (u32 i = 0; i < s_clr.npipe; i++) vk_defer_pipe(s_clr.pipe[i].p);
    s_clr.npipe = 0;
    for (u32 i = 0; i < s_cv.npool; i++) {
        vk_defer_fb(s_cv.pool[i].fb);
        vk_defer_image(&s_cv.pool[i].im);
    }
    vk_flush(1);
    for (u32 i = 0; i < 2; i++)
        if (s_cv.rp[i]) pvkDestroyRenderPass(s_vk.device, s_cv.rp[i], NULL);
    if (s_cv.pl)  pvkDestroyPipelineLayout(s_vk.device, s_cv.pl, NULL);
    if (s_clr.pl) pvkDestroyPipelineLayout(s_vk.device, s_clr.pl, NULL);
    s_clr.pl = VK_NULL_HANDLE;
    if (s_cv.dsl) pvkDestroyDescriptorSetLayout(s_vk.device, s_cv.dsl, NULL);
    if (s_cv.vs)  pvkDestroyShaderModule(s_vk.device, s_cv.vs, NULL);
    memset(&s_cv, 0, sizeof s_cv);
}

static int vk_cv_init(void)
{
    if (s_cv.failed) return -1;
    if (s_cv.pl) return 0;
    static const char* vs_hlsl =
        "struct VO { float4 pos : SV_Position; };\n"
        "VO main(uint id : SV_VertexID) {\n"
        "  VO o;\n"
        "  float2 p = float2((float)((id << 1) & 2u), (float)(id & 2u));\n"
        "  o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "  return o;\n"
        "}\n";
    for (u32 i = 0; i < 2; i++) {
        VkAttachmentDescription att = {
            .format = i ? VK_FORMAT_R16_UINT : VK_FORMAT_R32_UINT,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
        VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_GENERAL };
        VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     .colorAttachmentCount = 1, .pColorAttachments = &cref };
        VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                       .attachmentCount = 1, .pAttachments = &att,
                                       .subpassCount = 1, .pSubpasses = &sub };
        if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_cv.rp[i]) != VK_SUCCESS) goto fail;
    }
    {
        VkDescriptorSetLayoutBinding b = { RSX_SPIRV_TEXTURE_BINDING, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                           1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                .bindingCount = 1, .pBindings = &b };
        if (pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_cv.dsl) != VK_SUCCESS) goto fail;
        VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                            .setLayoutCount = 1, .pSetLayouts = &s_cv.dsl };
        if (pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_cv.pl) != VK_SUCCESS) goto fail;
    }
    if (vk_eng_translate(vs_hlsl, RSX_SHADER_STAGE_VERTEX, &s_cv.vs)) goto fail;
    return 0;
fail:
    VK_LOG("engine: surface reinterpretation pass unavailable\n");
    vk_cv_shutdown();
    s_cv.failed = 1;
    return -1;
}

/* The pass for source kind `kind` read as texture format `base`, built on
 * first use. */
static VkPipeline vk_cv_pipeline(u32 kind, u32 base, int f16)
{
    const u32 key = (kind << 8) | base;
    for (u32 i = 0; i < s_cv.npipe; i++) if (s_cv.pipe[i].key == key) return s_cv.pipe[i].p;
    if (s_cv.npipe >= VK_CV_PIPES) return VK_NULL_HANDLE;
    char fs[2048];
    snprintf(fs, sizeof fs,
        "#define KIND %u\n#define DST %u\n"
        "Texture2D<float4> src : register(t0);\n"
        "uint main(float4 pos : SV_Position) : SV_Target0 {\n"
        "  float4 c = src.Load(int3(int2(pos.xy), 0));\n"
        "  uint w;\n"
        "#if KIND == 0\n"
        "  uint4 b = (uint4)round(saturate(c) * 255.0);\n"
        "  w = (b.w << 24) | (b.x << 16) | (b.y << 8) | b.z;\n"
        "#elif KIND == 1\n"
        /* round(), not + 0.5: at depth 1 the float sum rounds to 2^24 and the
         * word would wrap to 0 (the engine's store quantizes in double). */
        "  w = min((uint)round(saturate(c.x) * 16777215.0), 16777215u) << 8;\n"
        "#else\n"
        "  w = min((uint)round(saturate(c.x) * 65535.0), 65535u);\n"
        "#endif\n"
        "#if DST == 133 || DST == 158\n"
        "  uint o = ((w >> 16) & 255u) | (((w >> 8) & 255u) << 8) | ((w & 255u) << 16) | (w & 0xFF000000u);\n"
        "#if DST == 158\n"
        "  o |= 0xFF000000u;\n"
        "#endif\n"
        "  return o;\n"
        "#elif DST == 144\n"
        "  return asuint((float)(w >> 8) / 16777215.0);\n"
        "#elif DST == 149 || DST == 159\n"
        "  return (w >> 16) | ((w & 65535u) << 16);\n"
        "#else\n"
        "  return w;\n"
        "#endif\n"
        "}\n", kind, base);
    VkShaderModule fm = VK_NULL_HANDLE;
    if (vk_eng_translate(fs, RSX_SHADER_STAGE_FRAGMENT, &fm)) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_cv.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL,
                                                  .cullMode = VK_CULL_MODE_NONE,
                                                  .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                  .lineWidth = 1.0f,
                                                  .pNext = vk_provoking_chain() };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xFu };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &cba };
    const VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .pDynamicState = &ds, .layout = s_cv.pl,
        .renderPass = s_cv.rp[f16 ? 1 : 0], .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    const VkResult r = pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &pci, NULL, &p);
    pvkDestroyShaderModule(s_vk.device, fm, NULL);
    if (r != VK_SUCCESS) { VK_LOG("engine: reinterpretation pipeline failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
    s_cv.pipe[s_cv.npipe].key = key;
    s_cv.pipe[s_cv.npipe].p = p;
    s_cv.npipe++;
    return p;
}

/* An R32_UINT / R16_UINT image of w x h with its framebuffer, kept in a
 * small pool: reused images are written after every earlier command that
 * read them (global barriers), so reuse needs no wait. */
static int vk_cv_target(u32 w, u32 h, int f16)
{
    for (u32 i = 0; i < s_cv.npool; i++)
        if (s_cv.pool[i].w == w && s_cv.pool[i].h == h && s_cv.pool[i].f16 == f16) return (int)i;
    u32 slot;
    if (s_cv.npool < VK_CV_POOL) {
        slot = s_cv.npool;
    } else {
        slot = 0;
        vk_defer_fb(s_cv.pool[0].fb);
        vk_defer_image(&s_cv.pool[0].im);
        memmove(&s_cv.pool[0], &s_cv.pool[1], sizeof s_cv.pool[0] * (VK_CV_POOL - 1));
        slot = VK_CV_POOL - 1;
        s_cv.npool--;
    }
    memset(&s_cv.pool[slot], 0, sizeof s_cv.pool[slot]);
    vk_image* im = &s_cv.pool[slot].im;
    if (vk_create_image(im, f16 ? VK_FORMAT_R16_UINT : VK_FORMAT_R32_UINT, w, h,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT) ||
        vk_eng_new_layout(im->img, VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(im);
        return -1;
    }
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = s_cv.rp[f16 ? 1 : 0], .attachmentCount = 1,
                                    .pAttachments = &im->view, .width = w, .height = h, .layers = 1 };
    if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &s_cv.pool[slot].fb) != VK_SUCCESS) {
        vk_destroy_image(im);
        return -1;
    }
    s_cv.pool[slot].w = w; s_cv.pool[slot].h = h; s_cv.pool[slot].f16 = f16;
    s_cv.npool++;
    return (int)slot;
}

/* ---- hardware MSAA (RPCS3 "MSAA: Auto", VKResolveHelper) ------------------
 * A target whose SURFACE_FORMAT antialias is not center_1_sample gets an MSAA
 * twin: a w/sx x h/sy image of sx*sy samples (diagonal_centered_2 -> 2,
 * square_centered_4 / square_rotated_4 -> 4, RPCS3 surface_utils). The
 * engine's draws render into the twin, at pixel size, with the pipelines'
 * rasterizationSamples, sample mask and alpha-to-coverage; the engine's own
 * widened single-sample image stays the "resolve surface" RPCS3 keeps: every
 * other user (texture sampling, transfers, readback, guest write-back,
 * present) reads it after a resolve pass copies sample s of pixel (x, y) to
 * texel (x * sx + s % sx, y * sy + s / sx) -- RPCS3's MSAA sample mapping, so
 * a shader sampling the widened image sees what RPCS3's sampleTexture2DMS
 * fetches -- and anything written there is unresolved back into the samples
 * before the next multisampled draw. Depth goes through SV_Depth, stencil one
 * bit per pass (RPCS3 stencil resolve). RSX_VK_MSAA=0 keeps the widened
 * single-sample rendering. */
#define VK_EMS_PIPES 96
#define VK_EMS_RPS   16
typedef struct {
    vk_image      ms;               /* the twin; its view covers every aspect */
    VkImageView   ms_view[2];       /* depth twins: depth / stencil sampled views */
    VkImageView   ss_view[2];       /* ... and the widened image's */
    VkFramebuffer fb[2];            /* [0] resolve (widened), [1] unresolve (twin) */
    u32           sx, sy, samples, pw, ph;
    u8            state;            /* 0 in step, 1 twin newer, 2 widened newer */
} vk_ems_t;
static vk_ems_t s_ems[VK_ENG_MAX_OBJ];
static u32 s_ems_counts;            /* sample counts every MSAA use supports */
static struct {
    struct { VkFormat fmt; u32 samples; VkRenderPass rp; } rp[VK_EMS_RPS];
    u32 nrp;
    struct { u32 op, bit, samples, sx, sy; VkFormat fmt; VkPipeline p; } pipe[VK_EMS_PIPES];
    u32 npipe;
    vk_image scr;                   /* multisampled depth when no zeta is bound */
    u32 scr_w, scr_h, scr_samples;
    u32 warned;
} s_emsx;

/* RSX_VK_EMS_LOG=<n>: trace the first n twin transitions. */
static int vk_ems_log_left(void)
{
    static int left = -1;
    if (left < 0) { const char* e = getenv("RSX_VK_EMS_LOG"); left = e ? atoi(e) : 0; }
    return left > 0 ? left-- : 0;
}

static int vk_ems_enabled(void)
{
    if (s_ems_on) return s_ems_on > 0;
    if (!s_vk.phys) return 0;
    const char* e = getenv("RSX_VK_MSAA");
    if (e && *e == '0') {
        s_ems_on = -1;
        VK_LOG("engine: hardware MSAA off (RSX_VK_MSAA=0), widened single-sample AA\n");
        return 0;
    }
    if (!s_ms_srs_ok) {             /* the unresolve passes shade per sample */
        s_ems_on = -1;
        VK_LOG("engine: hardware MSAA off (no sampleRateShading)\n");
        return 0;
    }
    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    s_ems_counts = p.limits.framebufferColorSampleCounts & p.limits.framebufferDepthSampleCounts &
                   p.limits.framebufferStencilSampleCounts & p.limits.sampledImageColorSampleCounts &
                   p.limits.sampledImageDepthSampleCounts & p.limits.sampledImageStencilSampleCounts &
                   (VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT);
    s_ems_on = 1;
    VK_LOG("engine: hardware MSAA on (sample counts 0x%x%s)\n", s_ems_counts,
           s_ms_a2one_ok ? ", alpha-to-one" : "");
    return 1;
}

/* Can this run render pass class pc (vk_async_request_mask)? */
static int vk_ems_pc_ok(u32 pc)
{
    if (!(pc % 3u)) return 1;
    return vk_ems_enabled() && (s_ems_counts & (u32)s_pc_samples[pc % 3u]) != 0;
}

static int vk_ems_has(u32 h) { return h && h < VK_ENG_MAX_OBJ && s_ems[h].ms.img; }
static VkImageView vk_ems_view(u32 handle) { return vk_ems_has(handle) ? s_ems[handle].ms.view : VK_NULL_HANDLE; }
static VkImage vk_ems_image(u32 handle) { return vk_ems_has(handle) ? s_ems[handle].ms.img : VK_NULL_HANDLE; }

static VkImageView vk_ems_aspect_view(VkImage img, VkFormat fmt, VkImageAspectFlags a)
{
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                  .subresourceRange = { a, 0, 1, 0, 1 } };
    VkImageView v = VK_NULL_HANDLE;
    if (pvkCreateImageView(s_vk.device, &vci, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    return v;
}

static void vk_ems_defer_view(VkImageView v)
{
    if (!v) return;
    vk_image t;
    memset(&t, 0, sizeof t);
    t.view = v;
    vk_defer_image(&t);
}

/* vk_create_image with a sample count, moved to GENERAL in `pre`. */
static int vk_ems_image_new(vk_image* im, VkFormat fmt, u32 w, u32 h, u32 samples,
                            VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    memset(im, 0, sizeof *im);
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (pvkCreateImage(s_vk.device, &ici, NULL, &im->img) != VK_SUCCESS) { im->img = VK_NULL_HANDLE; return -1; }
    VkMemoryRequirements req;
    pvkGetImageMemoryRequirements(s_vk.device, im->img, &req);
    u32 type;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size };
    if ((vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
         vk_find_memory_type(req.memoryTypeBits, 0, &type)) ||
        (mai.memoryTypeIndex = type,
         pvkAllocateMemory(s_vk.device, &mai, NULL, &im->mem) != VK_SUCCESS) ||
        pvkBindImageMemory(s_vk.device, im->img, im->mem, 0) != VK_SUCCESS ||
        !(im->view = vk_ems_aspect_view(im->img, fmt, aspect)) ||
        vk_eng_new_layout(im->img, aspect)) {
        vk_destroy_image(im);
        return -1;
    }
    return 0;
}

/* Helper passes: one attachment of `fmt` at `samples`; colour DONT_CARE,
 * depth DONT_CARE with stencil cleared to 0 (the bit passes set its ones). */
static VkRenderPass vk_ems_rp(VkFormat fmt, u32 samples)
{
    for (u32 i = 0; i < s_emsx.nrp; i++)
        if (s_emsx.rp[i].fmt == fmt && s_emsx.rp[i].samples == samples) return s_emsx.rp[i].rp;
    if (s_emsx.nrp >= VK_EMS_RPS) return VK_NULL_HANDLE;
    const VkImageAspectFlags a = vk_depth_aspect_of(fmt);
    const int depth = vk_eng_rt_index(fmt) < 0;
    VkAttachmentDescription att = {
        .format = fmt, .samples = (VkSampleCountFlagBits)samples,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = (depth && (a & VK_IMAGE_ASPECT_STENCIL_BIT)) ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                                     : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS };
    if (depth) sub.pDepthStencilAttachment = &ref;
    else { sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref; }
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 1, .pAttachments = &att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    VkRenderPass rp = VK_NULL_HANDLE;
    if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &rp) != VK_SUCCESS) return VK_NULL_HANDLE;
    s_emsx.rp[s_emsx.nrp].fmt = fmt;
    s_emsx.rp[s_emsx.nrp].samples = samples;
    s_emsx.rp[s_emsx.nrp++].rp = rp;
    return rp;
}

/* Op 0/1 colour resolve / unresolve, 2/3 depth, 4/5 stencil bit `bit`.
 * Even ops write the widened single-sample image, odd ones the twin. */
static VkPipeline vk_ems_pipe(u32 op, u32 bit, VkFormat fmt, u32 samples, u32 sx, u32 sy)
{
    for (u32 i = 0; i < s_emsx.npipe; i++)
        if (s_emsx.pipe[i].op == op && s_emsx.pipe[i].bit == bit && s_emsx.pipe[i].fmt == fmt &&
            s_emsx.pipe[i].samples == samples && s_emsx.pipe[i].sx == sx && s_emsx.pipe[i].sy == sy)
            return s_emsx.pipe[i].p;
    if (s_emsx.npipe >= VK_EMS_PIPES || vk_cv_init()) return VK_NULL_HANDLE;
    const u32 ns = (op & 1u) ? samples : 1u;
    const VkRenderPass rp = vk_ems_rp(fmt, ns);
    if (!rp) return VK_NULL_HANDLE;
    char fs[2048];
    snprintf(fs, sizeof fs,
        "#define OP %u\n#define SX %u\n#define SY %u\n#define BIT %u\n"
        "#if OP == 0 || OP == 2 || OP == 4\n"
        "#if OP == 0\n"
        "Texture2DMS<float4> src : register(t0);\n"
        "float4 main(float4 pos : SV_Position) : SV_Target0 {\n"
        "#elif OP == 2\n"
        "Texture2DMS<float> src : register(t0);\n"
        "float main(float4 pos : SV_Position) : SV_Depth {\n"
        "#else\n"
        "Texture2DMS<uint> src : register(t0);\n"
        "void main(float4 pos : SV_Position) {\n"
        "#endif\n"
        "  int2 p = int2(pos.xy);\n"
        "  int2 q = p / int2(SX, SY);\n"
        "  int s = (p.y %% SY) * SX + (p.x %% SX);\n"
        "#if OP == 4\n"
        "  if (((src.Load(q, s) >> BIT) & 1u) == 0u) discard;\n"
        "#else\n"
        "  return src.Load(q, s);\n"
        "#endif\n"
        "}\n"
        "#else\n"
        "#if OP == 1\n"
        "Texture2D<float4> src : register(t0);\n"
        "float4 main(float4 pos : SV_Position, uint si : SV_SampleIndex) : SV_Target0 {\n"
        "#elif OP == 3\n"
        "Texture2D<float> src : register(t0);\n"
        "float main(float4 pos : SV_Position, uint si : SV_SampleIndex) : SV_Depth {\n"
        "#else\n"
        "Texture2D<uint> src : register(t0);\n"
        "void main(float4 pos : SV_Position, uint si : SV_SampleIndex) {\n"
        "#endif\n"
        "  int s = (int)si;\n"
        "  int3 t = int3(int2(pos.xy) * int2(SX, SY) + int2(s %% SX, s / SX), 0);\n"
        "#if OP == 5\n"
        "  if (((src.Load(t) >> BIT) & 1u) == 0u) discard;\n"
        "#else\n"
        "  return src.Load(t);\n"
        "#endif\n"
        "}\n"
        "#endif\n", op, sx, sy, bit);
    VkShaderModule fm = VK_NULL_HANDLE;
    if (vk_eng_translate(fs, RSX_SHADER_STAGE_FRAGMENT, &fm)) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_cv.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL,
                                                  .cullMode = VK_CULL_MODE_NONE,
                                                  .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                  .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = (VkSampleCountFlagBits)ns };
    if (ns > 1u) { ms.sampleShadingEnable = VK_TRUE; ms.minSampleShading = 1.0f; }
    const VkStencilOpState so = { .failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_REPLACE,
                                  .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_ALWAYS,
                                  .compareMask = 0xFFu, .writeMask = 1u << bit, .reference = 0xFFu };
    VkPipelineDepthStencilStateCreateInfo dss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                  .depthTestEnable = op < 4u, .depthWriteEnable = op < 4u,
                                                  .depthCompareOp = VK_COMPARE_OP_ALWAYS,
                                                  .stencilTestEnable = op >= 4u, .front = so, .back = so,
                                                  .maxDepthBounds = 1.0f };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xFu };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = op < 2u ? 1u : 0u, .pAttachments = &cba };
    const VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = op >= 2u ? &dss : NULL,
        .pColorBlendState = &cb, .pDynamicState = &ds, .layout = s_cv.pl,
        .renderPass = rp, .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    const VkResult r = pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &pci, NULL, &p);
    pvkDestroyShaderModule(s_vk.device, fm, NULL);
    if (r != VK_SUCCESS) { VK_LOG("engine: MSAA resolve pipeline %u failed (%d)\n", op, (int)r); return VK_NULL_HANDLE; }
    s_emsx.pipe[s_emsx.npipe].op = op;
    s_emsx.pipe[s_emsx.npipe].bit = bit;
    s_emsx.pipe[s_emsx.npipe].fmt = fmt;
    s_emsx.pipe[s_emsx.npipe].samples = samples;
    s_emsx.pipe[s_emsx.npipe].sx = sx;
    s_emsx.pipe[s_emsx.npipe].sy = sy;
    s_emsx.pipe[s_emsx.npipe++].p = p;
    return p;
}

/* Every helper a target of `fmt` at sx x sy needs, built up front so a twin
 * only exists where its resolves work. */
static int vk_ems_prepare(VkFormat fmt, u32 sx, u32 sy)
{
    const u32 samples = sx * sy;
    const int depth = vk_eng_rt_index(fmt) < 0;
    const int stencil = depth && (vk_depth_aspect_of(fmt) & VK_IMAGE_ASPECT_STENCIL_BIT);
    for (u32 op = depth ? 2u : 0u; op < (depth ? 4u : 2u); op++)
        if (!vk_ems_pipe(op, 0, fmt, samples, sx, sy)) return -1;
    if (stencil)
        for (u32 op = 4u; op < 6u; op++)
            for (u32 b = 0; b < 8u; b++)
                if (!vk_ems_pipe(op, b, fmt, samples, sx, sy)) return -1;
    return 0;
}

/* Resolve (un = 0: twin -> widened image) or unresolve (un = 1) handle h. */
static void vk_ems_run(u32 h, int un)
{
    vk_ems_t* t = &s_ems[h];
    vk_eng_obj* o = &s_eobj[h];
    const int depth = o->kind == VK_ENG_DEPTH;
    const int stencil = depth && (vk_depth_aspect_of(o->fmt) & VK_IMAGE_ASPECT_STENCIL_BIT);
    const u32 fw = un ? t->pw : t->pw * t->sx, fh = un ? t->ph : t->ph * t->sy;
    const VkRenderPass rp = vk_ems_rp(o->fmt, un ? t->samples : 1u);
    const u32 op0 = (depth ? 2u : 0u) + (u32)un;
    if (vk_ems_log_left())
        VK_LOG("ems: run h%u %s %s %ux%u x%u (%ux%u) obj %ux%u\n", h, depth ? "Z" : "C",
               un ? "unresolve" : "resolve", t->pw, t->ph, t->samples, t->sx, t->sy, o->w, o->h);
    const VkPipeline p0 = vk_ems_pipe(op0, 0, o->fmt, t->samples, t->sx, t->sy);
    if (!rp || !p0) goto fail;
    if (!t->fb[un]) {
        const VkImageView av = un ? t->ms.view : o->im.view;
        VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                        .renderPass = rp, .attachmentCount = 1, .pAttachments = &av,
                                        .width = fw, .height = fh, .layers = 1 };
        if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &t->fb[un]) != VK_SUCCESS) {
            t->fb[un] = VK_NULL_HANDLE;
            goto fail;
        }
    }
    VkDescriptorSet set[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    for (u32 k = 0; k < (stencil ? 2u : 1u); k++) {
        const VkImageView sv = depth ? (un ? t->ss_view[k] : t->ms_view[k]) : (un ? o->im.view : t->ms.view);
        set[k] = vk_fr_desc(s_cv.dsl);
        if (!set[k] || !sv) goto fail;
        VkDescriptorImageInfo ii = { .imageView = sv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set[k],
                                    .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = 1,
                                    .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii };
        pvkUpdateDescriptorSets(s_vk.device, 1, &wr, 0, NULL);
    }
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (!cb) goto fail;
    o->fresh = 0;
    VkClearValue cv;
    memset(&cv, 0, sizeof cv);
    cv.depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = rp, .framebuffer = t->fb[un],
                                  .renderArea = { { 0, 0 }, { fw, fh } },
                                  .clearValueCount = 1, .pClearValues = &cv };
    pvkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vpt = { 0.0f, 0.0f, (float)fw, (float)fh, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { fw, fh } };
    pvkCmdSetViewport(cb, 0, 1, &vpt);
    pvkCmdSetScissor(cb, 0, 1, &sc);
    pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p0);
    pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_cv.pl, 0, 1, &set[0], 0, NULL);
    pvkCmdDraw(cb, 3, 1, 0, 0);
    if (stencil) {
        pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_cv.pl, 0, 1, &set[1], 0, NULL);
        for (u32 b = 0; b < 8u; b++) {
            const VkPipeline pb = vk_ems_pipe(4u + (u32)un, b, o->fmt, t->samples, t->sx, t->sy);
            if (!pb) continue;
            pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pb);
            pvkCmdDraw(cb, 3, 1, 0, 0);
        }
    }
    pvkCmdEndRenderPass(cb);
    s_f.b_valid = 0;                     /* the engine's bound state is gone */
    vk_rec_barrier(&s_f.main);
    s_f.main.other = 1;
    return;
fail:
    if (!(s_emsx.warned & 1u)) { VK_LOG("engine: MSAA %s pass failed; samples lost\n", un ? "unresolve" : "resolve"); s_emsx.warned |= 1u; }
}

/* The widened image is about to be read (or partly written): bring the
 * samples' last writes into it. */
static void vk_ems_ss(u32 handle)
{
    if (!vk_ems_has(handle)) return;
    /* Only a newer twin is brought in; when the widened image is newer
     * (state 2) the twin stays stale, or the next draw would skip its
     * unresolve and the resolve after it would overwrite the image. */
    if (s_ems[handle].state == 1) {
        vk_ems_run(handle, 0);
        s_ems[handle].state = 0;
    }
}

/* The widened image was written: the samples are stale. */
static void vk_ems_wrote_ss(u32 handle)
{
    if (vk_ems_has(handle)) s_ems[handle].state = 2;
}

/* 0xFF: no twin. */
static u32 vk_ems_state_of(u32 handle)
{
    return vk_ems_has(handle) ? s_ems[handle].state : 0xFFu;
}

/* The twin is about to be rendered: bring the widened image's writes in. */
static void vk_ems_ms(u32 handle)
{
    if (!vk_ems_has(handle)) return;
    if (s_ems[handle].state == 2) vk_ems_run(handle, 1);
    s_ems[handle].state = 0;
}

static void vk_ems_free(u32 handle)
{
    if (!vk_ems_has(handle)) return;
    vk_ems_t* t = &s_ems[handle];
    vk_eng_drop_framebuffers_for(handle);
    if (s_f.pass && (s_f.pass_depth == t->ms.img ||
                     (s_f.pass_nrt && s_f.pass_img[0] == t->ms.img))) vk_pass_end();
    for (u32 k = 0; k < 2; k++) {
        vk_defer_fb(t->fb[k]);
        vk_ems_defer_view(t->ms_view[k]);
        vk_ems_defer_view(t->ss_view[k]);
    }
    vk_defer_image(&t->ms);
    memset(t, 0, sizeof *t);
}

/* A clear of `aspect` lands in the twin, which is then the newer copy (a
 * clear of some aspects keeps the others: unresolved first). 1 = done. */
static int vk_ems_clear(u32 handle, VkImageAspectFlags aspect, const VkClearValue* v)
{
    if (!vk_ems_has(handle) || !v) return 0;
    vk_ems_t* t = &s_ems[handle];
    const vk_eng_obj* o = &s_eobj[handle];
    const int depth = o->kind == VK_ENG_DEPTH;
    const VkImageAspectFlags all = depth ? vk_depth_aspect_of(o->fmt) : VK_IMAGE_ASPECT_COLOR_BIT;
    if (vk_ems_log_left())
        VK_LOG("ems: clear h%u %s aspect %X state %u\n", handle, depth ? "Z" : "C", (u32)aspect, t->state);
    if ((aspect & all) != all) vk_ems_ms(handle);
    if (s_f.pass && s_ems_pass_ms && s_f.pass_w == t->pw && s_f.pass_h == t->ph) {
        VkClearAttachment ca = { .aspectMask = aspect, .clearValue = *v };
        int hit = 0;
        if (depth) hit = s_f.pass_depth == t->ms.img;
        else
            for (u32 i = 0; i < s_f.pass_nrt && !hit; i++)
                if (s_f.pass_rt[i] == handle && s_f.pass_img[i] == t->ms.img) { hit = 1; ca.colorAttachment = i; }
        if (hit) {
            VkClearRect cr = { { { 0, 0 }, { t->pw, t->ph } }, 0, 1 };
            pvkCmdClearAttachments(s_f.main.cb, 1, &ca, 1, &cr);
            t->state = 1;
            return 1;
        }
    }
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (!cb) return 0;
    VkImageSubresourceRange r = { aspect, 0, 1, 0, 1 };
    if (depth) pvkCmdClearDepthStencilImage(cb, t->ms.img, VK_IMAGE_LAYOUT_GENERAL, &v->depthStencil, 1, &r);
    else       pvkCmdClearColorImage(cb, t->ms.img, VK_IMAGE_LAYOUT_GENERAL, &v->color, 1, &r);
    t->state = 1;
    return 1;
}

/* Multisampled depth for an MSAA pass with no zeta bound (key 0xFFFFFFFE). */
static VkImageView vk_ems_scratch(u32 w, u32 h, u32 samples)
{
    if (s_emsx.scr.view && s_emsx.scr_samples == samples && s_emsx.scr_w >= w && s_emsx.scr_h >= h)
        return s_emsx.scr.view;
    if (s_emsx.scr_samples == samples) {
        if (w < s_emsx.scr_w) w = s_emsx.scr_w;
        if (h < s_emsx.scr_h) h = s_emsx.scr_h;
    }
    vk_eng_drop_framebuffers_for(0xFFFFFFFEu);
    vk_defer_image(&s_emsx.scr);
    s_emsx.scr_w = s_emsx.scr_h = s_emsx.scr_samples = 0;
    vk_dcls_setup();
    const VkImageAspectFlags a = vk_depth_aspect_of(s_dcls_fmt[0]);
    if (vk_ems_image_new(&s_emsx.scr, s_dcls_fmt[0], w, h, samples,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, a))
        return VK_NULL_HANDLE;
    VkCommandBuffer cb = vk_rec_upload(&s_f.pre, s_emsx.scr.img);
    if (!cb) return VK_NULL_HANDLE;
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { a, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(cb, s_emsx.scr.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    s_emsx.scr_w = w; s_emsx.scr_h = h; s_emsx.scr_samples = samples;
    return s_emsx.scr.view;
}

static u32 vk_ems_surface_of(u32 handle)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return 0;
    return s_eobj[handle].kind == VK_ENG_VIEW ? s_eobj[handle].surface : handle;
}

/* Before a draw / clear into rt[0..n) and depth: 1 when it renders the
 * twins (all present, one sample layout, depth twin covering the pass). */
static int vk_ems_draw_begin(const u32* rt, u32 n, u32 depth, vk_ems_drawinfo* di)
{
    memset(di, 0, sizeof *di);
    if (s_ems_on <= 0) return 0;
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++)
        if ((s_e2.tex_mask >> u) & 1u) vk_ems_ss(vk_ems_surface_of(s_e2.tex[u]));
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++)
        if ((s_e2.vtex_mask >> u) & 1u) vk_ems_ss(vk_ems_surface_of(s_e2.vtex[u]));
    int ms = n && rt && vk_ems_has(rt[0]);
    const vk_ems_t* t0 = ms ? &s_ems[rt[0]] : NULL;
    for (u32 i = 1; i < n && ms; i++) {
        const vk_ems_t* t = vk_ems_has(rt[i]) ? &s_ems[rt[i]] : NULL;
        ms = t && t->samples == t0->samples && t->sx == t0->sx && t->sy == t0->sy &&
             t->pw == t0->pw && t->ph == t0->ph;
    }
    if (ms && depth) {
        const vk_ems_t* t = vk_ems_has(depth) ? &s_ems[depth] : NULL;
        ms = t && t->samples == t0->samples && t->sx == t0->sx && t->sy == t0->sy &&
             t->pw >= t0->pw && t->ph >= t0->ph;
    }
    if (ms && !depth) {
        di->dview = vk_ems_scratch(t0->pw, t0->ph, t0->samples);
        di->dimg = s_emsx.scr.img;
        di->dkey = 0xFFFFFFFEu;
        ms = di->dview != VK_NULL_HANDLE;
    } else if (ms) {
        di->dview = s_ems[depth].ms.view;
        di->dimg = s_ems[depth].ms.img;
        di->dkey = depth;
    }
    if (!ms) {
        if (vk_ems_has(depth) && vk_ems_log_left() && n && rt)
            VK_LOG("ems: draw ss rt h%u(%d st%u) z h%u(%d st%u)\n", rt[0], vk_ems_has(rt[0]),
                   vk_ems_has(rt[0]) ? s_ems[rt[0]].state : 0u, depth, vk_ems_has(depth),
                   vk_ems_has(depth) ? s_ems[depth].state : 0u);
        for (u32 i = 0; i < n; i++) vk_ems_ss(rt[i]);
        vk_ems_ss(depth);
        memset(di, 0, sizeof *di);
        return 0;
    }
    for (u32 i = 0; i < n; i++) vk_ems_ms(rt[i]);
    vk_ems_ms(depth);
    di->sx = t0->sx; di->sy = t0->sy;
    di->sidx = t0->samples == 4u ? 2u : 1u;
    di->pw = t0->pw; di->ph = t0->ph;
    return 1;
}

static void vk_ems_draw_end(const u32* rt, u32 n, u32 depth, int ms)
{
    /* ms < 0: the draw / clear was dropped after vk_ems_draw_begin. Its
     * resolves and unresolves left every twin in step (state 0) and nothing
     * was written since, so there is nothing to mark. */
    if (ms < 0) return;
    const u8 st = ms ? 1u : 2u;
    for (u32 i = 0; i < n; i++) if (vk_ems_has(rt[i])) s_ems[rt[i]].state = st;
    if (vk_ems_has(depth)) s_ems[depth].state = st;
}

static void vk_ems_shutdown(void)
{
    for (u32 h = 1; h < VK_ENG_MAX_OBJ; h++) vk_ems_free(h);
    vk_eng_drop_framebuffers_for(0xFFFFFFFEu);
    vk_defer_image(&s_emsx.scr);
    for (u32 i = 0; i < s_emsx.npipe; i++) vk_defer_pipe(s_emsx.pipe[i].p);
    vk_flush(1);
    for (u32 i = 0; i < s_emsx.nrp; i++) pvkDestroyRenderPass(s_vk.device, s_emsx.rp[i].rp, NULL);
    memset(&s_emsx, 0, sizeof s_emsx);
}

/* rsx_draw_backend.target_set_aa: make, keep or drop handle's MSAA twin. */
static u32 vk_eng_target_set_aa(void* user, u32 handle, u32 aa_sx, u32 aa_sy)
{
    (void)user;
    if (!s_vk.device) return 0;
    if (!handle)
        return vk_ems_enabled() ? (RSX_BE_AA_HW_MSAA | (s_ms_a2one_ok ? RSX_BE_AA_HW_A2ONE : 0u)) : 0u;
    if (handle >= VK_ENG_MAX_OBJ) return 0;
    vk_eng_obj* o = &s_eobj[handle];
    if (o->kind != VK_ENG_COLOR && o->kind != VK_ENG_DEPTH) return 0;
    const u32 samples = aa_sx * aa_sy;
    int want = vk_ems_enabled() && (samples == 2u || samples == 4u) && aa_sx <= 2u && aa_sy <= 2u &&
               (s_ems_counts & samples);
    /* Native 16/8-bit targets keep the engine's widened AA (no twin passes). */
    if (want && o->kind == VK_ENG_COLOR)
        want = !o->snapshot && vk_eng_rt_index(o->fmt) >= 0 && !vk_cv_native(o->fmt);
    if (want && o->kind == VK_ENG_DEPTH)
        want = (vk_depth_features(o->fmt) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && s_dcls_sampled[o->dcls];
    const u32 pw = want ? vk_pw(o) / aa_sx : 0u, ph = want ? vk_ph(o) / aa_sy : 0u;
    if (!pw || !ph) want = 0;
    vk_ems_t* t = &s_ems[handle];
    if (vk_ems_log_left())
        VK_LOG("ems: set_aa h%u %s %ux%u want %d %ux%u (had %ux%u x%u st%u)\n", handle,
               o->kind == VK_ENG_DEPTH ? "Z" : "C", aa_sx, aa_sy, want, pw, ph, t->pw, t->ph,
               t->samples, t->state);
    if (want && t->ms.img && t->sx == aa_sx && t->sy == aa_sy && t->pw == pw && t->ph == ph)
        return RSX_BE_AA_HW_MSAA;
    if (!want && !t->ms.img) return 0;
    vk_op_begin();
    if (t->ms.img) { vk_ems_ss(handle); vk_ems_free(handle); }
    u32 r = 0;
    if (want && !vk_ems_prepare(o->fmt, aa_sx, aa_sy)) {
        const int depth = o->kind == VK_ENG_DEPTH;
        const VkImageAspectFlags a = depth ? vk_depth_aspect_of(o->fmt) : VK_IMAGE_ASPECT_COLOR_BIT;
        const VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        int ok = !vk_ems_image_new(&t->ms, o->fmt, pw, ph, samples, u, a);
        if (ok && depth) {
            const u32 nv = (a & VK_IMAGE_ASPECT_STENCIL_BIT) ? 2u : 1u;
            for (u32 k = 0; k < nv && ok; k++) {
                const VkImageAspectFlags ak = k ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
                t->ms_view[k] = vk_ems_aspect_view(t->ms.img, o->fmt, ak);
                t->ss_view[k] = vk_ems_aspect_view(o->im.img, o->fmt, ak);
                ok = t->ms_view[k] && t->ss_view[k];
            }
        }
        if (ok) {
            t->sx = aa_sx; t->sy = aa_sy; t->samples = samples; t->pw = pw; t->ph = ph;
            t->state = 2;            /* the widened image holds the contents */
            vk_eng_drop_framebuffers_for(handle);
            r = RSX_BE_AA_HW_MSAA;
            if (!(s_emsx.warned & 2u)) {
                VK_LOG("engine: MSAA twin %ux%u x%u samples (%s)\n", pw, ph, samples, depth ? "depth" : "colour");
                s_emsx.warned |= 2u;
            }
        } else if (t->ms.img) {
            vk_ems_free(handle);         /* defers the image and any views made */
        } else {
            for (u32 k = 0; k < 2; k++) { vk_ems_defer_view(t->ms_view[k]); vk_ems_defer_view(t->ss_view[k]); }
            memset(t, 0, sizeof *t);
        }
    } else if (want && !(s_emsx.warned & 4u)) {
        VK_LOG("engine: MSAA helpers for format %d unavailable; widened AA kept\n", (int)o->fmt);
        s_emsx.warned |= 4u;
    }
    vk_op_done();
    return r;
}

/* ---- masked / scissored clears (RPCS3 VKGSRender::clear_surface) ---------
 * RSX clears only the scissored region and honours the colour channel mask
 * and the stencil write mask. Whole channels go through vkCmdClearAttachments
 * with the rectangle (RPCS3's fast clear); a partial colour mask or stencil
 * write mask is a fullscreen triangle under that scissor with masked writes
 * (RPCS3 attachment_clear_pass / stencil_clear_pass). */
/* The sample count of the pass the next vk_clr_pipeline is built for (its
 * cache key, the render pass, already differs per count); 0 is one. */
static VkSampleCountFlagBits s_clr_samples;
static VkPipeline vk_clr_pipeline(VkRenderPass rp, u32 n, u32 cmask, u32 smask, const float rgba[4])
{
    for (u32 i = 0; i < s_clr.npipe; i++)
        if (s_clr.pipe[i].rp == rp && s_clr.pipe[i].n == n && s_clr.pipe[i].cmask == cmask &&
            s_clr.pipe[i].smask == smask && !memcmp(s_clr.pipe[i].rgba, rgba, sizeof s_clr.pipe[i].rgba))
            return s_clr.pipe[i].p;
    if (vk_cv_init()) return VK_NULL_HANDLE;
    if (!s_clr.pl) {
        VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        if (pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_clr.pl) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    char fs[1024];
    int at = snprintf(fs, sizeof fs, "struct PO {");
    for (u32 i = 0; i < n; i++) at += snprintf(fs + at, sizeof fs - (size_t)at, " float4 c%u : SV_Target%u;", i, i);
    at += snprintf(fs + at, sizeof fs - (size_t)at, " };\nPO main() {\n  PO o;\n");
    for (u32 i = 0; i < n; i++)
        at += snprintf(fs + at, sizeof fs - (size_t)at, "  o.c%u = float4(%.9g, %.9g, %.9g, %.9g);\n",
                       i, rgba[0], rgba[1], rgba[2], rgba[3]);
    snprintf(fs + at, sizeof fs - (size_t)at, "  return o;\n}\n");
    VkShaderModule fm = VK_NULL_HANDLE;
    if (vk_eng_translate(fs, RSX_SHADER_STAGE_FRAGMENT, &fm)) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_cv.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL,
                                                  .cullMode = VK_CULL_MODE_NONE,
                                                  .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                  .lineWidth = 1.0f,
                                                  .pNext = vk_provoking_chain() };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = s_clr_samples ? s_clr_samples
                                                                                      : VK_SAMPLE_COUNT_1_BIT };
    const VkStencilOpState so = { VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP,
                                  VK_COMPARE_OP_ALWAYS, 0xFFu, smask & 0xFFu, 0u };
    VkPipelineDepthStencilStateCreateInfo dss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                  .depthTestEnable = VK_FALSE, .depthWriteEnable = VK_FALSE,
                                                  .stencilTestEnable = smask ? VK_TRUE : VK_FALSE,
                                                  .front = so, .back = so, .maxDepthBounds = 1.0f };
    VkPipelineColorBlendAttachmentState cba[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i < n; i++) cba[i] = (VkPipelineColorBlendAttachmentState){ .colorWriteMask = cmask & 0xFu };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = n, .pAttachments = cba };
    const VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                    VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = 3, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &dss, .pColorBlendState = &cb, .pDynamicState = &ds,
        .layout = s_clr.pl, .renderPass = rp, .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    const VkResult r = pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &pci, NULL, &p);
    pvkDestroyShaderModule(s_vk.device, fm, NULL);
    if (r != VK_SUCCESS) { VK_LOG("engine: masked clear pipeline failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
    if (s_clr.npipe >= VK_CLR_PIPES) {
        vk_defer_pipe(s_clr.pipe[0].p);
        memmove(&s_clr.pipe[0], &s_clr.pipe[1], sizeof s_clr.pipe[0] * (VK_CLR_PIPES - 1));
        s_clr.npipe--;
    }
    s_clr.pipe[s_clr.npipe].rp = rp; s_clr.pipe[s_clr.npipe].n = n;
    s_clr.pipe[s_clr.npipe].cmask = cmask; s_clr.pipe[s_clr.npipe].smask = smask;
    memcpy(s_clr.pipe[s_clr.npipe].rgba, rgba, sizeof s_clr.pipe[0].rgba);
    s_clr.pipe[s_clr.npipe].p = p;
    s_clr.npipe++;
    return p;
}

/* ---- one scale per render-target binding ----------------------------------
 * RPCS3 scales the surfaces bound together alike: its surface store re-clones
 * a surface whose scaling config differs from the active one
 * (surface_store.h sync_scaling_config, VKRenderTargets.h clone_surface).
 * vk_rs_pick decides per target from its own size, so a small colour target
 * (at or under RSX_RES_SCALE_MIN) bound with a large zeta, or a target that
 * would exceed the device limit scaled, ends up at another scale than its
 * partners. Before a draw or clear the odd attachments are re-created at the
 * binding's scale -- the largest ("if either qualifies, both scale"), else,
 * where that cannot be made, the smallest -- their contents blitted over
 * (colour linear where the format filters, depth / stencil nearest), and
 * their views, framebuffers and MSAA twins following. */
static int vk_rs_rescale(u32 handle, float rs)
{
    vk_eng_obj* o = &s_eobj[handle];
    if ((o->kind != VK_ENG_COLOR && o->kind != VK_ENG_DEPTH) || o->snapshot) return -1;
    if (!(rs > 1.0f)) rs = 1.0f;
    if (vk_rs_of(o) == rs) return 0;
    const u32 npw = rs > 1.0f ? vk_rs_dim(o->w, rs) : o->w;
    const u32 nph = rs > 1.0f ? vk_rs_dim(o->h, rs) : o->h;
    if (!npw || !nph || npw > vk_max_dim() || nph > vk_max_dim()) return -1;
    const int depth = o->kind == VK_ENG_DEPTH;
    const VkImageAspectFlags a = depth ? vk_depth_aspect_of(o->fmt) : VK_IMAGE_ASPECT_COLOR_BIT;
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, o->fmt, &fp);
    const VkFormatFeatureFlags bl = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((fp.optimalTilingFeatures & bl) != bl) return -1;
    const VkFilter filter = (!depth && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
                          ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    const VkImageUsageFlags usage = depth
        ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | vk_aa_depth_usage() |
           (s_dcls_sampled[o->dcls] ? VK_IMAGE_USAGE_SAMPLED_BIT : 0u) |
           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    vk_image ni;
    memset(&ni, 0, sizeof ni);
    if (vk_create_image(&ni, o->fmt, npw, nph, usage, a) || vk_eng_new_layout(ni.img, a)) {
        vk_destroy_image(&ni);
        return -1;
    }
    u32 asx = 0, asy = 0;                /* an MSAA twin is rebuilt at the new size */
    if (vk_ems_has(handle)) {
        asx = s_ems[handle].sx; asy = s_ems[handle].sy;
        vk_ems_ss(handle);
        vk_ems_free(handle);
    }
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (!cb) {
        vk_defer_image(&ni);
        if (asx) vk_eng_target_set_aa(NULL, handle, asx, asy);
        return -1;
    }
    VkImageBlit b = {
        .srcSubresource = { a, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)vk_pw(o), (int32_t)vk_ph(o), 1 } },
        .dstSubresource = { a, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 }, { (int32_t)npw, (int32_t)nph, 1 } },
    };
    pvkCmdBlitImage(cb, o->im.img, VK_IMAGE_LAYOUT_GENERAL, ni.img, VK_IMAGE_LAYOUT_GENERAL,
                    1, &b, filter);
    s_f.main.other = 1;
    vk_eng_drop_framebuffers_for(handle);
    if (!depth)
        for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++) {
            vk_eng_obj* v = &s_eobj[i];
            if (v->kind != VK_ENG_VIEW || v->surface != handle) continue;
            vk_image old;
            memset(&old, 0, sizeof old);
            old.view = v->im.view;
            vk_defer_image(&old);
            v->im.view = VK_NULL_HANDLE;
            VkComponentMapping map = vk_remap(v->remap, v->rsx_fmt);
            if (vk_cv_native(v->fmt)) map = vk_cv_compose(map, v->fmt);
            if (vk_eng_make_view(ni.img, v->fmt, 1, map, 0, &v->im.view))
                v->im.view = VK_NULL_HANDLE;
            v->pw = npw; v->ph = nph; v->rs = rs;
        }
    if (o->sv.view) {
        vk_defer_image(&o->sv);
        memset(&o->sv, 0, sizeof o->sv);
        if (vk_eng_make_view(ni.img, o->fmt, 1, vk_cv_base_map(o->fmt), 0, &o->sv.view))
            o->sv.view = VK_NULL_HANDLE;
    }
    vk_defer_image(&o->im);
    o->im = ni;
    o->pw = npw; o->ph = nph; o->rs = rs;
    o->fresh = 0;
    if (asx) vk_eng_target_set_aa(NULL, handle, asx, asy);
    return 0;
}

static void vk_rs_match(const u32* rt, u32 n, u32 depth)
{
    if (!(vk_rs_global() > 1.0f) || !rt || n < 1 || n > RSX_BE_MAX_COLOR_TARGETS) return;
    const vk_eng_obj* r0 = vk_eng_get(rt[0], VK_ENG_COLOR);
    if (!r0) return;
    float rs[RSX_BE_MAX_COLOR_TARGETS + 1];
    u32 hs[RSX_BE_MAX_COLOR_TARGETS + 1], m = 0;
    for (u32 i = 0; i < n; i++) {
        const vk_eng_obj* o = vk_eng_get(rt[i], VK_ENG_COLOR);
        if (!o || o->snapshot) return;
        hs[m] = rt[i]; rs[m++] = vk_rs_of(o);
    }
    const vk_eng_obj* zo = depth ? vk_eng_get(depth, VK_ENG_DEPTH) : NULL;
    if (zo && zo->w >= r0->w && zo->h >= r0->h) { hs[m] = depth; rs[m++] = vk_rs_of(zo); }
    float lo = rs[0];
    int diff = 0;
    for (u32 i = 1; i < m; i++) {
        if (rs[i] != rs[0]) diff = 1;
        if (rs[i] < lo) lo = rs[i];
    }
    if (!diff) return;
    const float s = rsx_vk_binding_scale(rs, m);
    int ok = 1;
    for (u32 i = 0; i < m && ok; i++) ok = !vk_rs_rescale(hs[i], s);
    if (!ok) {
        ok = 1;
        for (u32 i = 0; i < m; i++) ok &= !vk_rs_rescale(hs[i], lo);
    }
    static u32 logged;
    if (logged < 4u) {
        logged++;
        VK_LOG("engine: attachments at different scales re-created at x%.3g%s\n",
               (double)(ok ? vk_rs_of(&s_eobj[hs[0]]) : s), ok ? "" : " (failed: scratch depth)");
    }
}

static void vk_eng_clear_rect(void* user, const u32* rts, u32 n, u32 depth,
                              const float rgba[4], u32 cmask, u32 ds_flags,
                              float depth_value, u32 stencil, u32 smask,
                              u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    if (n < 1 || n > RSX_BE_MAX_COLOR_TARGETS) return;
    vk_eng_obj* rt0 = vk_eng_get(rts[0], VK_ENG_COLOR);
    if (!rt0) return;
    vk_rs_match(rts, n, depth);
    for (u32 i = 1; i < n; i++) {
        const vk_eng_obj* o = vk_eng_get(rts[i], VK_ENG_COLOR);
        if (!o || o->fmt != rt0->fmt || o->w != rt0->w || o->h != rt0->h ||
            vk_rs_of(o) != vk_rs_of(rt0)) return;
    }
    u32 W0 = rt0->w, H0 = rt0->h;
    if (x >= W0 || y >= H0) return;
    if (w > W0 - x) w = W0 - x;
    if (h > H0 - y) h = H0 - y;
    if (!w || !h) return;
    cmask &= 0xFu;
    float crgba[4];
    vk_cv_clear_value(rt0->fmt, rgba, crgba);
    rgba = crgba;
    cmask = vk_cv_write_mask(rt0->fmt, cmask);
    vk_eng_obj* zo = depth ? vk_eng_get(depth, VK_ENG_DEPTH) : NULL;
    if (zo && (zo->w < W0 || zo->h < H0)) zo = NULL;
    if (zo && vk_rs_of(zo) != vk_rs_of(rt0)) {
        if (!(s_e2.warned & 0x80000000u)) { VK_LOG("engine: clear of depth and colour at different scales: depth skipped\n"); s_e2.warned |= 0x80000000u; }
        zo = NULL;
    }
    if (!zo) ds_flags = 0;
    if (!(vk_depth_aspect_of(zo ? zo->fmt : s_dcls_fmt[0]) & VK_IMAGE_ASPECT_STENCIL_BIT)) ds_flags &= ~RSX_BE_CLEAR_STENCIL;
    smask &= 0xFFu;
    if (!(ds_flags & RSX_BE_CLEAR_STENCIL) || !smask) { ds_flags &= ~RSX_BE_CLEAR_STENCIL; smask = 0; }
    if (!cmask && !ds_flags) return;
    if (rt0->rs > 1.0f) {
        vk_rs_rect(rt0, &x, &y, &w, &h);
        W0 = vk_pw(rt0); H0 = vk_ph(rt0);
        if (zo && (vk_pw(zo) < W0 || vk_ph(zo) < H0)) {
            zo = NULL; ds_flags = 0;
            if (!cmask) return;
        }
    }

    vk_op_begin();
    /* Under hardware MSAA the clear lands in the twins, at pixel size. */
    vk_ems_drawinfo emi;
    const u32 zd = zo ? depth : 0u;     /* the depth vk_ems_draw_begin saw */
    const int ems = vk_ems_draw_begin(rts, n, zd, &emi);
    u32 W = W0, H = H0;
    VkImageView dview;
    u32 dkey;
    VkImage dimg;
    if (ems) {
        W = emi.pw; H = emi.ph;
        const u32 x1 = (x + w + emi.sx - 1u) / emi.sx, y1 = (y + h + emi.sy - 1u) / emi.sy;
        x /= emi.sx; y /= emi.sy;
        w = (x1 < W ? x1 : W) - x; h = (y1 < H ? y1 : H) - y;
        if (x >= W || y >= H || !w || !h) goto skip;
        dview = emi.dview; dkey = emi.dkey; dimg = emi.dimg;
        if (zo && dkey != depth) zo = NULL;
    } else {
        dview = zo ? zo->im.view : vk_eng_scratch_depth(W, H);
        dkey = zo ? depth : 0xFFFFFFFFu;
        dimg = zo ? zo->im.img : s_e2.scratch_depth.img;
    }
    if (!dview) goto skip;
    const u32 pcls = (zo ? (u32)zo->dcls : 0u) * 3u + (ems ? emi.sidx : 0u);
    VkRenderPass rp = vk_eng_render_pass_pc(rt0->fmt, n, pcls);
    s_ems_fb_ms = ems;
    VkFramebuffer fb = rp ? vk_eng_framebuffer(rp, rts, n, dview, dkey, W, H) : VK_NULL_HANDLE;
    s_ems_fb_ms = 0;
    if (!fb) goto skip;
    const int partial = (cmask && cmask != 0xFu) || (smask && smask != 0xFFu);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (partial) {
        const float none[4] = { 0, 0, 0, 0 };
        s_clr_samples = s_pc_samples[pcls % 3u];
        pipe = vk_clr_pipeline(rp, n, cmask != 0xFu ? cmask : 0u,
                               smask != 0xFFu ? smask : 0u, cmask != 0xFu ? rgba : none);
        s_clr_samples = (VkSampleCountFlagBits)0;
        if (!pipe) goto skip;
    }
    for (u32 i = 0; i < n; i++) vk_eng_touch(rts[i]);
    if (zo) zo->fresh = 0;

    if (!s_f.pass || s_f.pass_fb != fb || s_f.pass_rp != rp || s_f.pass_w != W || s_f.pass_h != H) {
        vk_pass_end();
        VkCommandBuffer cb0 = vk_rec_get(&s_f.main);
        if (!cb0) goto skip;
        if (s_f.main.other || s_f.main.nbatch) vk_rec_barrier(&s_f.main);
        s_f.main.other = 1;
        VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                      .renderPass = rp, .framebuffer = fb,
                                      .renderArea = { { 0, 0 }, { W, H } } };
        vkq_on_pass_begin(cb0, 0);
        pvkCmdBeginRenderPass(cb0, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        s_f.pass = 1;
        vkq_on_pass_begin(cb0, 1);
        s_f.pass_rp = rp; s_f.pass_fb = fb; s_f.pass_w = W; s_f.pass_h = H;
        s_f.pass_nrt = n;
        for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++) {
            s_f.pass_rt[i] = i < n ? rts[i] : 0;
            s_f.pass_img[i] = i < n ? (ems ? vk_ems_image(rts[i]) : s_eobj[rts[i]].im.img) : VK_NULL_HANDLE;
        }
        s_f.pass_depth = dimg;
        s_ems_pass_ms = ems;
    }
    VkCommandBuffer cb = s_f.main.cb;
    const VkClearRect cr = { { { (int32_t)x, (int32_t)y }, { w, h } }, 0, 1 };
    VkClearAttachment ca[RSX_BE_MAX_COLOR_TARGETS + 1];
    u32 nca = 0;
    if (cmask == 0xFu) {
        VkClearColorValue v;
        memcpy(v.float32, rgba, sizeof v.float32);
        for (u32 i = 0; i < n; i++)
            ca[nca++] = (VkClearAttachment){ .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                             .colorAttachment = i, .clearValue = { .color = v } };
    }
    VkImageAspectFlags za = 0;
    if (ds_flags & RSX_BE_CLEAR_DEPTH) za |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((ds_flags & RSX_BE_CLEAR_STENCIL) && smask == 0xFFu) za |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (za)
        ca[nca++] = (VkClearAttachment){ .aspectMask = za,
                                         .clearValue = { .depthStencil = { depth_value, stencil & 0xFFu } } };
    if (nca) pvkCmdClearAttachments(cb, nca, ca, 1, &cr);
    if (pipe) {
        const VkViewport vpt = { 0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f };
        const VkRect2D sc = { { (int32_t)x, (int32_t)y }, { w, h } };
        pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        pvkCmdSetViewport(cb, 0, 1, &vpt);
        pvkCmdSetScissor(cb, 0, 1, &sc);
        pvkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil & 0xFFu);
        pvkCmdDraw(cb, 3, 1, 0, 0);
        s_f.b_valid = 0;                 /* the draw path's bound state is gone */
    }
    vk_ems_draw_end(rts, n, zd, ems);
    vk_op_done();
    return;
skip:   /* nothing recorded: the twins stay as vk_ems_draw_begin left them */
    vk_ems_draw_end(rts, n, zd, -1);
    vk_op_done();
}

static u32 vk_eng_surface_reinterpret(void* user, u32 src, u32 src_kind,
                                      u32 x, u32 y, u32 w, u32 h,
                                      u32 tex_base, u32 remap, u32 rsx_fmt)
{
    rsx_be_format bf;
    int f16 = 0;
    switch (tex_base) {
    case 0x85u: case 0x9Eu: bf = RSX_BE_FMT_R8G8B8A8; break;
    case 0x90u: case 0x9Cu: bf = RSX_BE_FMT_R32F; break;
    case 0x95u:             bf = RSX_BE_FMT_R16G16; break;
    case 0x9Fu:             bf = RSX_BE_FMT_R16G16F; break;
    case 0x92u:             bf = RSX_BE_FMT_R16; f16 = 1; break;
    default: return 0;
    }
    /* A texel must be one guest word of the source: 16-bit textures read a
     * Z16 target, 32-bit ones a Z24S8 or A8R8G8B8 one. */
    if (f16 != (src_kind == RSX_BE_REINTERPRET_DEPTH_Z16) || src_kind > 2u) return 0;
    if (x || y || !w || !h) return 0;   /* the engine names whole-target origins only */
    VkImageView sv = VK_NULL_HANDLE;
    vk_eng_obj* so = NULL;
    if (src_kind == RSX_BE_REINTERPRET_COLOR_ARGB8) {
        so = vk_eng_get(src, VK_ENG_COLOR);
        if (!so || so->fmt != VK_FORMAT_R8G8B8A8_UNORM || w > so->w || h > so->h) return 0;
        vk_op_begin();
        vk_ems_ss(src);
        vk_op_done();
        sv = so->im.view;
    } else {
        vk_eng_obj* z = vk_eng_get(src, VK_ENG_DEPTH);
        if (!z || w > z->w || h > z->h) return 0;
        const u32 snap = vk_eng_depth_snapshot(user, src, z->w, z->h);
        so = vk_eng_get(snap, VK_ENG_TEXTURE);
        if (!so) return 0;
        sv = so->im.view;
    }
    /* A scaled source converts at its scale into a scaled texture. */
    const u32 gw = w, gh = h;
    w = vk_rs_c(so, w, vk_pw(so));
    h = vk_rs_c(so, h, vk_ph(so));
    if (vk_cv_init()) return 0;
    vk_op_begin();
    const VkPipeline p = vk_cv_pipeline(src_kind, tex_base, f16);
    const int ti = p ? vk_cv_target(w, h, f16) : -1;
    if (ti < 0) { vk_op_done(); return 0; }
    const u32 out = vk_eng_tex_new(bf, w, h, 1, 1, remap, rsx_fmt);
    if (!out) { vk_op_done(); return 0; }
    vk_eng_obj* o = &s_eobj[out];
    if (so->rs > 1.0f) { o->w = gw; o->h = gh; o->pw = w; o->ph = h; o->rs = so->rs; }
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    const VkDeviceSize bytes = (VkDeviceSize)w * h * (f16 ? 2u : 4u);
    VkDescriptorSet set = vk_fr_desc(s_cv.dsl);
    if (!set || !vk_ring(bytes, 16, &buf, &off)) { vk_eng_release(user, out); vk_op_done(); return 0; }
    VkDescriptorImageInfo ii = { .imageView = sv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii };
    pvkUpdateDescriptorSets(s_vk.device, 1, &wr, 0, NULL);

    so->fresh = 0;
    o->fresh = 0;
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (!cb) { vk_eng_release(user, out); vk_op_done(); return 0; }
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_cv.rp[f16 ? 1 : 0], .framebuffer = s_cv.pool[ti].fb,
                                  .renderArea = { { 0, 0 }, { w, h } } };
    pvkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_cv.pl, 0, 1, &set, 0, NULL);
    VkViewport vpt = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    pvkCmdSetViewport(cb, 0, 1, &vpt);
    pvkCmdSetScissor(cb, 0, 1, &sc);
    pvkCmdDraw(cb, 3, 1, 0, 0);
    pvkCmdEndRenderPass(cb);
    s_f.b_valid = 0;                     /* the engine's bound state is gone */
    vk_rec_barrier(&s_f.main);
    VkBufferImageCopy c = { .bufferOffset = off,
                            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                            .imageExtent = { w, h, 1 } };
    pvkCmdCopyImageToBuffer(cb, s_cv.pool[ti].im.img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    vk_rec_barrier(&s_f.main);
    pvkCmdCopyBufferToImage(cb, buf, o->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    s_f.main.other = 1;
    vk_op_done();
    return out;
}

/* ---- typeless surface copy (RPCS3 vk::copy_image_typeless) ----------------
 * render_target::memory_barrier copies old contents across aspects (colour
 * <-> depth) and between formats that are not bitcast-compatible (Z16 <->
 * Z24S8, fixed <-> float D24) through a typeless copy: the guest bytes are
 * reinterpreted, never converted as values. One fullscreen pass per mapping
 * rebuilds each destination texel's guest word from the source's guest words
 * (the gather of VKCompute's d24x8 / d32x8 kernels for a depth source, ARGB8
 * or X32 bits for a colour one, bytes regrouped where the texel sizes
 * differ, as the byte-shuffle does) and emits it in the destination's host
 * encoding (the scatter kernels: d24 = word >> 8, d24f = (word >> 8) << 7,
 * stencil = the low byte); buffer copies then move the bits into the
 * destination's colour, depth and stencil aspects. Point sampled on the
 * resolution-scaled images. */
#define VK_TL_PIPES 32
typedef struct { u32 sk, dk, host, st, hasst, sb, db, sx, sy, dw, dh, pdw, pdh, sgw, sgh, spw, sph; } vk_tl_key;
static struct {
    int                   failed;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout      pl;
    struct { vk_tl_key k; VkPipeline p; } pipe[VK_TL_PIPES];
    u32                   npipe, next;
} s_tl;

static void vk_tl_shutdown(void)
{
    for (u32 i = 0; i < s_tl.npipe; i++) vk_defer_pipe(s_tl.pipe[i].p);
    s_tl.npipe = 0;
    if (s_tl.pl || s_tl.dsl) vk_flush(1);
    if (s_tl.pl)  pvkDestroyPipelineLayout(s_vk.device, s_tl.pl, NULL);
    if (s_tl.dsl) pvkDestroyDescriptorSetLayout(s_vk.device, s_tl.dsl, NULL);
    memset(&s_tl, 0, sizeof s_tl);
}

static int vk_tl_init(void)
{
    if (s_tl.failed || vk_cv_init()) return -1;
    if (s_tl.pl) return 0;
    VkDescriptorSetLayoutBinding b[2] = {
        { RSX_SPIRV_TEXTURE_BINDING,      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { RSX_SPIRV_TEXTURE_BINDING + 1u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 2, .pBindings = b };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1 };
    if (pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_tl.dsl) != VK_SUCCESS) goto fail;
    plci.pSetLayouts = &s_tl.dsl;
    if (pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_tl.pl) != VK_SUCCESS) goto fail;
    return 0;
fail:
    VK_LOG("engine: typeless surface copy unavailable\n");
    if (s_tl.dsl) pvkDestroyDescriptorSetLayout(s_vk.device, s_tl.dsl, NULL);
    s_tl.dsl = VK_NULL_HANDLE;
    s_tl.failed = 1;
    return -1;
}

static VkPipeline vk_tl_pipeline(const vk_tl_key* k)
{
    for (u32 i = 0; i < s_tl.npipe; i++)
        if (!memcmp(&s_tl.pipe[i].k, k, sizeof *k)) return s_tl.pipe[i].p;
    static char fs[8192];
    snprintf(fs, sizeof fs,
        "#define SK %u\n#define DK %u\n#define HOST %u\n#define ST %u\n#define HAS_ST %u\n"
        "#define SB %u\n#define DB %u\n#define SU %u\n#define DU %u\n#define NW %u\n"
        "#define SPW %u.0\n#define SGW %u.0\n#define SPWM %uu\n"
        "#define SPH %u.0\n#define SGH %u.0\n#define SPHM %uu\n"
        "#define SX %uu\n#define SY %uu\n"
        "#define DW %u.0\n#define PDW %u.0\n#define DWM %uu\n"
        "#define DH %u.0\n#define PDH %u.0\n#define DHM %uu\n#define PDWU %uu\n"
        "Texture2D<float4> src : register(t0);\n"
        "Texture2D<float4> sst : register(t1);\n"
        "uint q(float v, float m) { return (uint)round(saturate(v) * m); }\n"
        "uint sword(uint x, uint y, uint wi) {\n"
        "  int2 p = int2((int)min((uint)(((float)x + 0.5) * SPW / SGW), SPWM),\n"
        "                (int)min((uint)(((float)y + 0.5) * SPH / SGH), SPHM));\n"
        "  float4 c = src.Load(int3(p, 0));\n"
        "  uint s = 0u;\n"
        "#if HAS_ST\n"
        "  s = min((uint)round(saturate(sst.Load(int3(p, 0)).x) * 255.0), 255u);\n"
        "#endif\n"
        "#if SK == 0\n"
        "  uint4 b = (uint4)round(saturate(c) * 255.0);\n"
        "  return (b.w << 24) | (b.x << 16) | (b.y << 8) | b.z;\n"
        "#elif SK == 1\n"
        "  return asuint(c.x);\n"
        "#elif SK == 2\n"
        "  return (min((uint)round(saturate(c.x) * 16777215.0), 16777215u) << 8) | s;\n"
        "#elif SK == 3\n"
        "  return (((asuint(c.x) >> 7) & 16777215u) << 8) | s;\n"
        "#elif SK == 4\n"
        "  return min((uint)round(saturate(c.x) * 65535.0), 65535u);\n"
        "#elif SK == 5\n"
        "  return f32tof16(c.x) & 65535u;\n"
        "#elif SK == 6\n"
        "  uint4 b = (uint4)round(saturate(c) * 255.0);\n"
        "  return (b.w << 24) | (b.z << 16) | (b.y << 8) | b.x;\n"
        "#elif SK == 7\n"
        "  return q(c.z, 255.0);\n"
        "#elif SK == 8\n"
        "  return (q(c.y, 255.0) << 8) | q(c.z, 255.0);\n"
        "#elif SK == 9\n"
        "  return (q(c.x, 31.0) << 11) | (q(c.y, 63.0) << 5) | q(c.z, 31.0);\n"
        "#elif SK == 10\n"
        "  return (q(c.w, 1.0) << 15) | (q(c.x, 31.0) << 10) | (q(c.y, 31.0) << 5) | q(c.z, 31.0);\n"
        "#elif SK == 11\n"
        "  float2 h = wi == 0u ? c.xy : c.zw;\n"
        "  return ((f32tof16(h.x) & 65535u) << 16) | (f32tof16(h.y) & 65535u);\n"
        "#else\n"
        "  return asuint(wi == 0u ? c.x : wi == 1u ? c.y : wi == 2u ? c.z : c.w);\n"
        "#endif\n"
        "}\n"
        "uint sbyte(uint b, uint y) {\n"
        "  uint tx = b / SB;\n"
        "  uint o = b - tx * SB;\n"
        "  uint wi = o / SU;\n"
        "  return (sword(SX + tx, y, wi) >> (8u * (SU - 1u - (o - wi * SU)))) & 255u;\n"
        "}\n"
        "uint tlword(uint gx, uint gy, uint j) {\n"
        "  uint y = SY + gy;\n"
        "#if SB == DB && NW == 1\n"
        "  return sword(SX + gx, y, 0u);\n"
        "#else\n"
        "  uint b0 = gx * DB + j * DU;\n"
        "  uint w = 0u;\n"
        "  [unroll] for (uint i = 0u; i < DU; i++) w = (w << 8) | sbyte(b0 + i, y);\n"
        "  return w;\n"
        "#endif\n"
        "}\n"
        "uint gxof(uint px) { return min((uint)(((float)px + 0.5) * DW / PDW), DWM); }\n"
        "uint n8(uint v, uint m) { return (v * 255u) / m; }\n"
        "uint main(float4 pos : SV_Position) : SV_Target0 {\n"
        "  uint gy = min((uint)(pos.y * DH / PDH), DHM);\n"
        "#if ST\n"
        "  uint px = (uint)pos.x * 4u;\n"
        "  uint o = tlword(gxof(px), gy, 0u) & 255u;\n"
        "  if (px + 1u < PDWU) o |= (tlword(gxof(px + 1u), gy, 0u) & 255u) << 8;\n"
        "  if (px + 2u < PDWU) o |= (tlword(gxof(px + 2u), gy, 0u) & 255u) << 16;\n"
        "  if (px + 3u < PDWU) o |= (tlword(gxof(px + 3u), gy, 0u) & 255u) << 24;\n"
        "  return o;\n"
        "#else\n"
        "  uint px = (uint)pos.x / NW;\n"
        "  uint w = tlword(gxof(px), gy, (uint)pos.x - px * NW);\n"
        "#if HOST == 0\n"
        "#if DK == 6\n"
        "  return w;\n"
        "#elif DK == 7\n"
        "  return (w & 255u) << 16;\n"
        "#elif DK == 8\n"
        "  return (((w >> 8) & 255u) << 8) | ((w & 255u) << 16);\n"
        "#elif DK == 9\n"
        "  return n8((w >> 11) & 31u, 31u) | (n8((w >> 5) & 63u, 63u) << 8) | (n8(w & 31u, 31u) << 16) | 0xFF000000u;\n"
        "#elif DK == 10\n"
        "  return n8((w >> 10) & 31u, 31u) | (n8((w >> 5) & 31u, 31u) << 8) | (n8(w & 31u, 31u) << 16) |\n"
        "         ((w & 32768u) != 0u ? 0xFF000000u : 0u);\n"
        "#else\n"
        "  return ((w >> 16) & 255u) | (((w >> 8) & 255u) << 8) | ((w & 255u) << 16) | (w & 0xFF000000u);\n"
        "#endif\n"
        "#elif HOST == 1 || HOST == 6\n"
        "  return w;\n"
        "#elif HOST == 5\n"
        "  return ((w & 65535u) << 16) | (w >> 16);\n"
        "#elif HOST == 7\n"
        "  return w & 65535u;\n"
        "#else\n"
        "#if DK == 2\n"
        "  uint n = (w >> 8) & 16777215u;\n"
        "  float f = (float)n / 16777215.0;\n"
        "#elif DK == 3\n"
        "  uint n = 0u;\n"
        "  float f = asfloat(((w >> 8) & 16777215u) << 7);\n"
        "#elif DK == 4\n"
        "  uint n = w & 65535u;\n"
        "  float f = (float)n / 65535.0;\n"
        "#else\n"
        "  uint n = 0u;\n"
        "  float f = f16tof32(w & 65535u);\n"
        "#endif\n"
        "#if HOST == 2\n"
        "#if DK == 2\n"
        "  return n;\n"
        "#else\n"
        "  return min((uint)round(saturate(f) * 16777215.0), 16777215u);\n"
        "#endif\n"
        "#elif HOST == 3\n"
        "  return asuint(f);\n"
        "#else\n"
        "#if DK == 4\n"
        "  return n;\n"
        "#else\n"
        "  return min((uint)round(saturate(f) * 65535.0), 65535u);\n"
        "#endif\n"
        "#endif\n"
        "#endif\n"
        "#endif\n"
        "}\n",
        k->sk, k->dk, k->host, k->st, k->hasst, k->sb, k->db,
        k->sb < 4u ? k->sb : 4u, k->db < 4u ? k->db : 4u, k->db < 4u ? 1u : k->db / 4u,
        k->spw, k->sgw, k->spw - 1u, k->sph, k->sgh, k->sph - 1u,
        k->sx, k->sy,
        k->dw, k->pdw, k->dw - 1u,
        k->dh, k->pdh, k->dh - 1u, k->pdw);
    VkShaderModule fm = VK_NULL_HANDLE;
    if (vk_eng_translate(fs, RSX_SHADER_STAGE_FRAGMENT, &fm)) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_cv.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL,
                                                  .cullMode = VK_CULL_MODE_NONE,
                                                  .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                  .lineWidth = 1.0f,
                                                  .pNext = vk_provoking_chain() };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xFu };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &cba };
    const VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = 2, .pDynamicStates = dyn };
    const int f16 = !k->st && (k->host == 4u || k->host == 7u);
    VkGraphicsPipelineCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .pDynamicState = &ds, .layout = s_tl.pl,
        .renderPass = s_cv.rp[f16 ? 1 : 0], .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    const VkResult r = pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &pci, NULL, &p);
    pvkDestroyShaderModule(s_vk.device, fm, NULL);
    if (r != VK_SUCCESS) { VK_LOG("engine: typeless copy pipeline failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
    u32 slot;
    if (s_tl.npipe < VK_TL_PIPES) {
        slot = s_tl.npipe++;
    } else {
        slot = s_tl.next;
        s_tl.next = (s_tl.next + 1u) % VK_TL_PIPES;
        vk_defer_pipe(s_tl.pipe[slot].p);
    }
    s_tl.pipe[slot].k = *k;
    s_tl.pipe[slot].p = p;
    return p;
}

/* One pass of the copy: the pipeline for `k` over a tw x th R32_UINT /
 * R16_UINT target, moved through the ring into `img`'s `aspect` at (x, y). */
static int vk_tl_pass(const vk_tl_key* k, VkDescriptorSet set, u32 tw, u32 th,
                      VkImage img, VkImageAspectFlags aspect, u32 x, u32 y, u32 w, u32 h,
                      u32 row_texels)
{
    const int f16 = !k->st && (k->host == 4u || k->host == 7u);
    const VkPipeline p = vk_tl_pipeline(k);
    const int ti = p ? vk_cv_target(tw, th, f16) : -1;
    if (ti < 0) return 0;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    if (!vk_ring((VkDeviceSize)tw * th * (f16 ? 2u : 4u), 16, &buf, &off)) return 0;
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (!cb) return 0;
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_cv.rp[f16 ? 1 : 0], .framebuffer = s_cv.pool[ti].fb,
                                  .renderArea = { { 0, 0 }, { tw, th } } };
    pvkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_tl.pl, 0, 1, &set, 0, NULL);
    VkViewport vpt = { 0.0f, 0.0f, (float)tw, (float)th, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { tw, th } };
    pvkCmdSetViewport(cb, 0, 1, &vpt);
    pvkCmdSetScissor(cb, 0, 1, &sc);
    pvkCmdDraw(cb, 3, 1, 0, 0);
    pvkCmdEndRenderPass(cb);
    s_f.b_valid = 0;                     /* the engine's bound state is gone */
    vk_rec_barrier(&s_f.main);
    VkBufferImageCopy c = { .bufferOffset = off,
                            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                            .imageExtent = { tw, th, 1 } };
    pvkCmdCopyImageToBuffer(cb, s_cv.pool[ti].im.img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    vk_rec_barrier(&s_f.main);
    VkBufferImageCopy in = { .bufferOffset = off, .bufferRowLength = row_texels,
                             .imageSubresource = { aspect, 0, 0, 1 },
                             .imageOffset = { (int32_t)x, (int32_t)y, 0 },
                             .imageExtent = { w, h, 1 } };
    pvkCmdCopyBufferToImage(cb, buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &in);
    vk_rec_barrier(&s_f.main);
    s_f.main.other = 1;
    return 1;
}

/* The host format a colour kind's target has (RSX_BE_FMT_* of the engine's
 * eng_surface_format: RGBA8 for every 8-bit-per-channel and packed layout). */
static VkFormat vk_tl_color_format(u32 kind)
{
    switch (kind) {
    case RSX_BE_TYPELESS_X32:    return VK_FORMAT_R32_SFLOAT;
    case RSX_BE_TYPELESS_FP16X4: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RSX_BE_TYPELESS_FP32X4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    default:                     return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

/* A native 16/8-bit target holds exactly that guest layout. */
static int vk_tl_native_kind(VkFormat f, u32 kind)
{
    switch (f) {
    case VK_FORMAT_R5G6B5_UNORM_PACK16:   return kind == RSX_BE_TYPELESS_RGB565;
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return kind == RSX_BE_TYPELESS_ARGB1555;
    case VK_FORMAT_R8G8_UNORM:            return kind == RSX_BE_TYPELESS_G8B8;
    case VK_FORMAT_R8_UNORM:              return kind == RSX_BE_TYPELESS_B8;
    default:                              return 0;
    }
}

static int vk_eng_surface_typeless_copy(void* user, u32 src, u32 src_kind,
                                        u32 sx, u32 sy, u32 sw, u32 sh,
                                        u32 dst, u32 dst_kind, u32 dx, u32 dy, u32 dw, u32 dh)
{
    if (src == dst || !sw || !sh || !dw || sh != dh || src_kind >= RSX_BE_TYPELESS_KINDS ||
        dst_kind >= RSX_BE_TYPELESS_KINDS)
        return 0;
    const u32 sb = RSX_BE_TYPELESS_BPP(src_kind);
    const u32 db = RSX_BE_TYPELESS_BPP(dst_kind);
    if (sw * sb != dw * db) return 0;
    const int sdepth = RSX_BE_TYPELESS_IS_DEPTH(src_kind), ddepth = RSX_BE_TYPELESS_IS_DEPTH(dst_kind);

    /* The destination and its host encoding. */
    vk_eng_obj* d = vk_eng_get(dst, ddepth ? VK_ENG_DEPTH : VK_ENG_COLOR);
    if (!d || dx + dw > d->w || dy + dh > d->h) return 0;
    u32 host;
    VkImageAspectFlags daspect = 0;
    if (!ddepth) {
        if (d->snapshot) return 0;
        if (dst_kind == RSX_BE_TYPELESS_X32) host = 1;
        else if (dst_kind == RSX_BE_TYPELESS_FP16X4) host = 5;
        else if (dst_kind == RSX_BE_TYPELESS_FP32X4) host = 6;
        else host = 0;
        /* A native 16-bit target (vk_cv_native) takes the guest word as is:
         * R5G6B5 / A1R5G5B5 / R8G8 = G<<8|B are the RSX's own bit layouts. */
        if (vk_cv_native(d->fmt)) {
            if (!vk_tl_native_kind(d->fmt, dst_kind) || d->fmt == VK_FORMAT_R8_UNORM) return 0;
            host = 7;
        } else if (d->fmt != vk_tl_color_format(dst_kind)) return 0;
    } else {
        daspect = vk_depth_aspect_of(d->fmt);
        if (d->fmt == VK_FORMAT_D24_UNORM_S8_UINT || d->fmt == VK_FORMAT_X8_D24_UNORM_PACK32) host = 2;
        else if (d->fmt == VK_FORMAT_D32_SFLOAT || d->fmt == VK_FORMAT_D32_SFLOAT_S8_UINT) host = 3;
        else if (d->fmt == VK_FORMAT_D16_UNORM || d->fmt == VK_FORMAT_D16_UNORM_S8_UINT) host = 4;
        else return 0;
    }

    /* The source, read as floats: a colour target directly, a depth target
     * through its snapshot, its stencil through an R8 copy. */
    vk_eng_obj* so = NULL;
    vk_eng_obj* zs = NULL;
    if (!sdepth) {
        so = vk_eng_get(src, VK_ENG_COLOR);
        if (!so || sx + sw > so->w || sy + sh > so->h) return 0;
        if (so->fmt != vk_tl_color_format(src_kind) && !vk_tl_native_kind(so->fmt, src_kind)) return 0;
    } else {
        zs = vk_eng_get(src, VK_ENG_DEPTH);
        if (!zs || sx + sw > zs->w || sy + sh > zs->h) return 0;
        so = vk_eng_get(vk_eng_depth_snapshot(user, src, zs->w, zs->h), VK_ENG_TEXTURE);
        if (!so) return 0;
    }
    if (vk_tl_init()) return 0;
    const int has_st = zs && src_kind <= RSX_BE_TYPELESS_Z24S8_F &&
                       (vk_depth_aspect_of(zs->fmt) & VK_IMAGE_ASPECT_STENCIL_BIT);

    u32 pdx = dx, pdy = dy, pdw = dw, pdh = dh;
    vk_rs_rect(d, &pdx, &pdy, &pdw, &pdh);
    vk_tl_key k;
    memset(&k, 0, sizeof k);
    k.sk = src_kind; k.dk = dst_kind; k.host = host; k.hasst = has_st ? 1u : 0u;
    k.sb = sb; k.db = db; k.sx = sx; k.sy = sy; k.dw = dw; k.dh = dh; k.pdw = pdw; k.pdh = pdh;
    k.sgw = so->w; k.sgh = so->h; k.spw = vk_pw(so); k.sph = vk_ph(so);

    vk_op_begin();
    if (!sdepth) vk_ems_ss(src);
    vk_ems_ss(dst);
    VkDescriptorSet set = vk_fr_desc(s_tl.dsl);
    if (!set) { vk_op_done(); return 0; }
    vk_image stim;
    memset(&stim, 0, sizeof stim);
    VkImageView stv = so->im.view;
    if (has_st) {
        const u32 zw = vk_pw(zs), zh = vk_ph(zs);
        VkBuffer sbuf = VK_NULL_HANDLE;
        VkDeviceSize soff = 0;
        if (vk_create_image(&stim, VK_FORMAT_R8_UNORM, zw, zh,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT) ||
            vk_eng_new_layout(stim.img, VK_IMAGE_ASPECT_COLOR_BIT) ||
            !vk_ring((VkDeviceSize)zw * zh, 16, &sbuf, &soff)) {
            vk_defer_image(&stim);
            vk_op_done();
            return 0;
        }
        zs->fresh = 0;
        VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
        if (!cb) { vk_defer_image(&stim); vk_op_done(); return 0; }
        VkBufferImageCopy o = { .bufferOffset = soff,
                                .imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 },
                                .imageExtent = { zw, zh, 1 } };
        pvkCmdCopyImageToBuffer(cb, zs->im.img, VK_IMAGE_LAYOUT_GENERAL, sbuf, 1, &o);
        vk_rec_barrier(&s_f.main);
        o.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        pvkCmdCopyBufferToImage(cb, sbuf, stim.img, VK_IMAGE_LAYOUT_GENERAL, 1, &o);
        vk_rec_barrier(&s_f.main);
        s_f.main.other = 1;
        stv = stim.view;
    }
    /* A native source is read through its RGBA8-lane view. */
    VkDescriptorImageInfo ii[2] = { { .imageView = so->sv.view ? so->sv.view : so->im.view,
                                      .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
                                    { .imageView = stv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL } };
    VkWriteDescriptorSet wr[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_TEXTURE_BINDING + 1u, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii[1] } };
    pvkUpdateDescriptorSets(s_vk.device, 2, wr, 0, NULL);
    so->fresh = 0;
    d->fresh = 0;

    /* RGBA16F / RGBA32F: two / four R32 words per destination texel. */
    const u32 nw = (host == 5u) ? 2u : (host == 6u) ? 4u : 1u;
    int ok = vk_tl_pass(&k, set, pdw * nw, pdh, d->im.img,
                        ddepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                        pdx, pdy, pdw, pdh, 0);
    /* Stencil: the destination word's low byte, four per R32 texel. */
    if (ok && (daspect & VK_IMAGE_ASPECT_STENCIL_BIT) && dst_kind <= RSX_BE_TYPELESS_Z24S8_F) {
        k.st = 1;
        const u32 tw = (pdw + 3u) / 4u;
        ok = vk_tl_pass(&k, set, tw, pdh, d->im.img, VK_IMAGE_ASPECT_STENCIL_BIT,
                        pdx, pdy, pdw, pdh, tw * 4u);
    }
    if (stim.img) vk_defer_image(&stim);
    if (ok) vk_ems_wrote_ss(dst);
    vk_op_done();
    return ok;
}

/* NV3089 between two colour targets: point or bilinear as the transfer's
 * interpolator says (RPCS3 blit engine). */
static void vk_eng_color_target_blit_flip(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                                          u32 dst, u32 dx, u32 dy, u32 dw, u32 dh, int linear,
                                          u32 flip);

static void vk_eng_color_target_blit_filtered(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                                              u32 dst, u32 dx, u32 dy, u32 dw, u32 dh, int linear)
{
    vk_eng_color_target_blit_flip(user, src, sx, sy, sw, sh, dst, dx, dy, dw, dh, linear, 0u);
}

/* RPCS3 VKTexture.cpp:1610-1618 (blit): a mirrored NV3089 transfer flips the
 * source area and vkCmdBlitImage reads it with reversed offsets. `flip`
 * bit 0 mirrors X, bit 1 Y (never a plain copy then). */
static void vk_eng_color_target_blit_flip(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh,
                                          u32 dst, u32 dx, u32 dy, u32 dw, u32 dh, int linear,
                                          u32 flip)
{
    (void)user;
    if (src == dst) return;
    vk_eng_obj* s = vk_eng_get(src, VK_ENG_COLOR);
    vk_eng_obj* d = vk_eng_get(dst, VK_ENG_COLOR);
    if (!s || !d || d->snapshot) return;
    /* RGBA8 <-> R5G6B5 / A1R5G5B5 convert channel for channel in the blit;
     * R8 / R8G8 hold B8 / G8B8 in other channels than the RGBA8 lanes. */
    const int conv = s->fmt != d->fmt;
    if (conv && !(vk_cv_blit_ok(s->fmt) && vk_cv_blit_ok(d->fmt))) return;
    if (!vk_eng_clip_blit(&sx, &sy, &sw, &sh, s->w, s->h, &dx, &dy, &dw, &dh, d->w, d->h))
        return;
    vk_rs_rect(s, &sx, &sy, &sw, &sh);
    vk_rs_rect(d, &dx, &dy, &dw, &dh);
    vk_op_begin();
    vk_ems_ss(src);
    vk_ems_ss(dst);
    vk_ems_wrote_ss(dst);
    s->fresh = 0;
    d->fresh = 0;
    VkCommandBuffer cb = vk_rec_xfer(&s_f.main);
    if (cb) {
        if (sw == dw && sh == dh && !flip && !conv) {
            VkImageCopy c = {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .srcOffset = { (int32_t)sx, (int32_t)sy, 0 },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstOffset = { (int32_t)dx, (int32_t)dy, 0 },
                .extent = { sw, sh, 1 },
            };
            pvkCmdCopyImage(cb, s->im.img, VK_IMAGE_LAYOUT_GENERAL, d->im.img,
                            VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        } else {
            const int32_t x0 = (int32_t)sx, x1 = (int32_t)(sx + sw);
            const int32_t y0 = (int32_t)sy, y1 = (int32_t)(sy + sh);
            VkImageBlit blit = {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .srcOffsets = { { (flip & 1u) ? x1 : x0, (flip & 2u) ? y1 : y0, 0 },
                                { (flip & 1u) ? x0 : x1, (flip & 2u) ? y0 : y1, 1 } },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstOffsets = { { (int32_t)dx, (int32_t)dy, 0 },
                                { (int32_t)(dx + dw), (int32_t)(dy + dh), 1 } },
            };
            pvkCmdBlitImage(cb, s->im.img, VK_IMAGE_LAYOUT_GENERAL, d->im.img,
                            VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                            linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        }
    }
    vk_op_done();
}

/* ---------------------------------------------------------------------------
 * Occlusion queries: RPCS3's VKGSRender occlusion map / VKQueryPool for ZCULL
 * ZPASS reports. A logical query (the engine's id) counts every draw recorded
 * while it is active. As in RPCS3, host queries live inside render pass
 * instances and never split one: a host query is begun when a pass begins
 * (or at occlusion_begin inside an open pass) and ended at occlusion_end or
 * when the pass ends, so one host query covers all of a logical query's draws
 * in one pass instance, and the logical result is the sum of its host
 * queries. vkCmdResetQueryPool cannot run inside a pass, so a batch of slots
 * is reset ahead of use just before a pass begins.
 * -------------------------------------------------------------------------*/
#define VKQ_SLOTS   4096u
#define VKQ_LOGICAL 4096u
#define VKQ_NONE    0xFFFFFFFFu
#define VKQ_READY   32u                  /* slots kept reset ahead of use  */

static int s_vkq_precise_ok;             /* occlusionQueryPrecise enabled  */

static struct {
    int         tried, ok, used;
    VkQueryPool pool;
    PFN_vkCreateQueryPool     CreateQueryPool;
    PFN_vkDestroyQueryPool    DestroyQueryPool;
    PFN_vkCmdResetQueryPool   CmdResetQueryPool;
    PFN_vkCmdBeginQuery       CmdBeginQuery;
    PFN_vkCmdEndQuery         CmdEndQuery;
    PFN_vkGetQueryPoolResults GetQueryPoolResults;
    PFN_vkGetFenceStatus      GetFenceStatus;
    /* Per ended host slot: the frame slot and submission index whose fence
     * covers it, the command buffer it was recorded into, and the next slot
     * of the same logical query. */
    struct { u32 fi, si, next; VkCommandBuffer cb; } slot[VKQ_SLOTS];
    u32         free_stack[VKQ_SLOTS];
    u32         nfree;
    u32         ready[VKQ_READY];        /* reset, not yet begun           */
    u32         nready;
    struct { u32 head; int active, unknown; float rs; } q[VKQ_LOGICAL];
    u32         active;                  /* logical id + 1, 0 = none       */
    u32         open, open_id;           /* host slot begun in this pass   */
} s_vkq;

static int vkq_ready(void)
{
    if (s_vkq.ok) return 1;
    if (s_vkq.tried || !s_f.on || !s_vk.device) return 0;
    s_vkq.tried = 1;
#define VKQ_LOAD(n) \
    if (!(s_vkq.n = (PFN_vk##n)pvkGetDeviceProcAddr(s_vk.device, "vk" #n))) \
        { VK_LOG("occlusion: missing vk" #n "\n"); return 0; }
    VKQ_LOAD(CreateQueryPool) VKQ_LOAD(DestroyQueryPool) VKQ_LOAD(CmdResetQueryPool)
    VKQ_LOAD(CmdBeginQuery) VKQ_LOAD(CmdEndQuery) VKQ_LOAD(GetQueryPoolResults)
    VKQ_LOAD(GetFenceStatus)
#undef VKQ_LOAD
    VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                 .queryType = VK_QUERY_TYPE_OCCLUSION,
                                 .queryCount = VKQ_SLOTS };
    if (s_vkq.CreateQueryPool(s_vk.device, &qi, NULL, &s_vkq.pool) != VK_SUCCESS) {
        VK_LOG("occlusion: vkCreateQueryPool failed\n");
        return 0;
    }
    s_vkq.nfree = 0;
    for (u32 i = VKQ_SLOTS; i-- > 0;) s_vkq.free_stack[s_vkq.nfree++] = i;
    for (u32 i = 0; i < VKQ_LOGICAL; i++) s_vkq.q[i].head = VKQ_NONE;
    s_vkq.nready = 0;
    s_vkq.open = VKQ_NONE;
    s_vkq.ok = 1;
    VK_LOG("occlusion: %u host queries (%s)\n", VKQ_SLOTS, s_vkq_precise_ok ? "precise" : "binary");
    return 1;
}

/* End the open host query (inside the pass it began in) and file it under
 * its logical query. */
static void vkq_close(void)
{
    const u32 s = s_vkq.open;
    if (s == VKQ_NONE) return;
    s_vkq.open = VKQ_NONE;
    s_vkq.CmdEndQuery(s_f.main.cb, s_vkq.pool, s);
    const u32 id = s_vkq.open_id;
    s_vkq.slot[s].fi = s_f.cur;
    s_vkq.slot[s].si = s_f.fr[s_f.cur].nsub;
    s_vkq.slot[s].cb = s_f.main.cb;
    s_vkq.slot[s].next = s_vkq.q[id].head;
    s_vkq.q[id].head = s;
}

/* Inside an open pass: begin a host query for the active logical query. */
static void vkq_open(void)
{
    if (s_vkq.open != VKQ_NONE || !s_vkq.active || !s_f.pass || !s_f.main.open || !s_vkq.nready)
        return;
    const u32 s = s_vkq.ready[--s_vkq.nready];
    s_vkq.CmdBeginQuery(s_f.main.cb, s_vkq.pool, s, s_vkq_precise_ok ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    s_vkq.open = s;
    s_vkq.open_id = s_vkq.active - 1u;
}

/* Engine render pass hooks. Before vkCmdBeginRenderPass (`after` 0): reset a
 * batch of free slots, in runs of consecutive indices. After it: begin the
 * active logical query's host query in the new pass. */
static void vkq_on_pass_begin(VkCommandBuffer cb, int after)
{
    if (!s_vkq.ok || !s_vkq.used) return;
    if (!after) {
        u32 run0 = 0, runn = 0;
        while (s_vkq.nready < VKQ_READY && s_vkq.nfree) {
            const u32 s = s_vkq.free_stack[--s_vkq.nfree];
            s_vkq.ready[s_vkq.nready++] = s;
            if (runn && s == run0 + runn) { runn++; continue; }
            if (runn) s_vkq.CmdResetQueryPool(cb, s_vkq.pool, run0, runn);
            run0 = s; runn = 1;
        }
        if (runn) s_vkq.CmdResetQueryPool(cb, s_vkq.pool, run0, runn);
        return;
    }
    vkq_open();
    if (s_vkq.active && s_vkq.open == VKQ_NONE) s_vkq.q[s_vkq.active - 1u].unknown = 1;
}

static void vkq_on_pass_end(void)
{
    if (s_vkq.ok) vkq_close();
}

/* A draw skipped while its pipeline compiles would count zero samples and make
 * the game cull the object as occluded; report the query as visible instead,
 * like RPCS3's conservative ZPASS on unknown results. */
static void vkq_mark_skipped(void)
{
    if (s_vkq.ok && s_vkq.active) s_vkq.q[s_vkq.active - 1u].unknown = 1;
}

static void vkq_free_list(u32 id)
{
    u32 s = s_vkq.q[id].head;
    while (s != VKQ_NONE) {
        const u32 nx = s_vkq.slot[s].next;
        s_vkq.free_stack[s_vkq.nfree++] = s;
        s = nx;
    }
    s_vkq.q[id].head = VKQ_NONE;
    s_vkq.q[id].rs = 0.0f;
    s_vkq.q[id].unknown = 0;
}

/* Recorded into main and main not yet submitted. */
static int vkq_unsubmitted(u32 s)
{
    return s_f.main.open && s_f.main.cb == s_vkq.slot[s].cb && s_f.cur == s_vkq.slot[s].fi;
}

/* Has the submission holding slot `s` (its begin and end, and so the reset
 * recorded ahead of them) completed? A later fence of the same frame slot
 * signalling implies it; a frame slot retired since has nsub back at or
 * below `si`. Availability alone is never trusted: until the reset executes,
 * a reused slot still shows its previous result. Unsubmitted work is
 * submitted before any wait (RPCS3 flushes the CB, then waits). */
static int vkq_done(u32 s, int wait)
{
    if (vkq_unsubmitted(s)) {
        if (!wait) return 0;
        vk_flush(0);
    }
    vk_frame* f = &s_f.fr[s_vkq.slot[s].fi];
    const u32 si = s_vkq.slot[s].si;
    if (f->nsub <= si) return 1;
    if (s_vkq.GetFenceStatus(s_vk.device, f->fence[si]) == VK_SUCCESS) return 1;
    if (!wait) return 0;
    pvkWaitForFences(s_vk.device, 1, &f->fence[si], VK_TRUE, UINT64_MAX);
    return 1;
}

static void vk_eng_draw_q(void* user, rsx_topology topology, const void* vertices, u32 vertex_count,
                          u32 stride, const u32* indices, u32 index_count)
{
    if (!s_vkq.ok || !s_vkq.active) {
        vk_eng_draw(user, topology, vertices, vertex_count, stride, indices, index_count);
        return;
    }
    const u32 id = s_vkq.active - 1u;
    /* An open pass without a host query only happens when occlusion_begin
     * found no reset slot inside it: start a new pass, which resets some. */
    if (s_vkq.open == VKQ_NONE && s_f.pass) {
        if (s_vkq.nfree) vk_pass_end();
        else s_vkq.q[id].unknown = 1;
    }
    const u32 cur0 = s_f.cur, d0 = s_f.fr[s_f.cur].draws;
    vk_eng_draw(user, topology, vertices, vertex_count, stride, indices, index_count);
    /* A draw the backend dropped would have been drawn by RPCS3. */
    if (s_f.cur == cur0 && s_f.fr[s_f.cur].draws == d0) s_vkq.q[id].unknown = 1;
    if (s_rs.dyn) {
        const vk_eng_obj* rt0 = vk_eng_get(s_eng_bound.rt[0], VK_ENG_COLOR);
        if (rt0 && rt0->rs > s_vkq.q[id].rs) s_vkq.q[id].rs = rt0->rs;
    }
}

static int vk_eng_occlusion_begin(void* user, u32 id)
{
    (void)user;
    if (!vkq_ready()) return 0;
    id &= VKQ_LOGICAL - 1u;
    if (s_vkq.active && s_vkq.active != id + 1u) {
        vkq_close();
        s_vkq.q[s_vkq.active - 1u].active = 0;
    }
    vkq_free_list(id);
    s_vkq.q[id].active = 1;
    s_vkq.active = id + 1u;
    s_vkq.used = 1;
    vkq_open();
    return 1;
}

static void vk_eng_occlusion_end(void* user, u32 id)
{
    (void)user;
    if (!s_vkq.ok) return;
    id &= VKQ_LOGICAL - 1u;
    if (s_vkq.open != VKQ_NONE && s_vkq.open_id == id) vkq_close();
    s_vkq.q[id].active = 0;
    if (s_vkq.active == id + 1u) s_vkq.active = 0;
}

/* wait: 0 poll, 1 submit and block, 2 only submit what holds the query. */
static int vk_eng_occlusion_result(void* user, u32 id, int wait, u64* out)
{
    (void)user;
    if (!s_vkq.ok) return 0;
    id &= VKQ_LOGICAL - 1u;
    if (wait == 2) {
        for (u32 s = s_vkq.q[id].head; s != VKQ_NONE; s = s_vkq.slot[s].next)
            if (vkq_unsubmitted(s)) { vk_flush(0); break; }
        return 0;
    }
    if (s_vkq.open != VKQ_NONE && s_vkq.open_id == id) vkq_close();
    for (u32 s = s_vkq.q[id].head; s != VKQ_NONE; s = s_vkq.slot[s].next)
        if (!vkq_done(s, wait)) return 0;
    u64 sum = 0;
    for (u32 s = s_vkq.q[id].head; s != VKQ_NONE; s = s_vkq.slot[s].next) {
        u64 v = 0;
        const VkResult r = s_vkq.GetQueryPoolResults(s_vk.device, s_vkq.pool, s, 1, sizeof v, &v,
                                                     sizeof v, VK_QUERY_RESULT_64_BIT |
                                                     VK_QUERY_RESULT_WAIT_BIT);
        if (r != VK_SUCCESS) { s_vkq.q[id].unknown = 1; continue; }
        sum += v;
    }
    if (s_vkq.q[id].rs > 1.0f && sum) {   /* samples of a scaled target: guest pixels */
        const u64 g = (u64)((double)sum / ((double)s_vkq.q[id].rs * s_vkq.q[id].rs));
        sum = g ? g : 1u;
    }
    if (s_vkq.q[id].unknown && !sum) sum = 0xFFFFu;
    vkq_free_list(id);
    if (out) *out = sum;
    return 1;
}

static void vk_eng_occlusion_discard(void* user, u32 id)
{
    (void)user;
    if (!s_vkq.ok) return;
    id &= VKQ_LOGICAL - 1u;
    if (s_vkq.open != VKQ_NONE && s_vkq.open_id == id) vkq_close();
    vkq_free_list(id);
    s_vkq.q[id].active = 0;
    if (s_vkq.active == id + 1u) s_vkq.active = 0;
}

static void vk_eng_shutdown_q(void* user)
{
    vk_eng_shutdown(user);
    if (s_vkq.ok && s_vkq.pool) s_vkq.DestroyQueryPool(s_vk.device, s_vkq.pool, NULL);
    memset(&s_vkq, 0, sizeof s_vkq);
}

static const rsx_draw_backend s_vk_engine_backend = {
    .user                 = NULL,
    .init                 = vk_eng_init,
    .shutdown             = vk_eng_shutdown_q,
    .submit_and_wait      = vk_eng_submit_and_wait,
    .texture_create       = vk_eng_texture_create,
    .texture_upload       = vk_eng_texture_upload,
    .texture_create_3d    = vk_eng_texture_create_3d,
    .texture_upload_3d    = vk_eng_texture_upload_3d,
    .texture_release      = vk_eng_release,
    .color_target_create  = vk_eng_color_target_create,
    .color_target_release = vk_eng_release,
    .surface_view         = vk_eng_surface_view,
    .depth_target_create  = vk_eng_depth_target_create,
    .depth_target_release = vk_eng_release,
    .depth_snapshot       = vk_eng_depth_snapshot,
    .color_snapshot       = vk_eng_color_snapshot,
    .pipeline_create      = vk_eng_pipeline_create,
    .pipeline_release     = vk_eng_pipeline_release,
    .bind_targets         = vk_eng_bind_targets,
    .bind_pipeline        = vk_eng_bind_pipeline,
    .bind_vs_constants    = vk_eng_bind_vs_constants,
    .bind_ps_constants    = vk_eng_bind_ps_constants,
    .bind_textures        = vk_eng_bind_textures,
    .bind_vertex_textures = vk_eng_bind_vertex_textures,
    .set_viewport         = vk_eng_set_viewport,
    .set_scissor          = vk_eng_set_scissor,
    .set_stencil_ref      = vk_eng_set_stencil_ref,
    .set_depth_bounds     = vk_eng_set_depth_bounds,
    .set_blend_constants  = vk_eng_set_blend_constants,
    .draw                 = vk_eng_draw_q,
    .clear_color          = vk_eng_clear_color,
    .clear_depth_stencil  = vk_eng_clear_depth_stencil,
    .present              = vk_eng_present,
    .readback             = vk_eng_readback,
    .depth_readback       = vk_eng_depth_readback,
    .stencil_readback     = vk_eng_stencil_readback,
    .zeta_readback        = vk_eng_zeta_readback,
    .color_target_write   = vk_eng_color_target_write,
    .color_region_snapshot = vk_eng_color_region_snapshot,
    .texture_copy_from_color = vk_eng_texture_copy_from_color,
    .color_target_blit    = vk_eng_color_target_blit,
    .present_region       = vk_eng_present_region,
    .surface_reinterpret  = vk_eng_surface_reinterpret,
    .color_target_blit_filtered = vk_eng_color_target_blit_filtered,
    .color_target_blit_flip = vk_eng_color_target_blit_flip,
    .color_format_supported = vk_eng_color_format_supported,
    .occlusion_begin      = vk_eng_occlusion_begin,
    .occlusion_end        = vk_eng_occlusion_end,
    .occlusion_result     = vk_eng_occlusion_result,
    .occlusion_discard    = vk_eng_occlusion_discard,
    .set_raster_dynamic   = vk_eng_set_raster_dynamic,
    .texture_snorm_view   = vk_eng_texture_snorm_view,
    .texture_srgb_view    = vk_eng_texture_srgb_view,
    .set_depth_range      = vk_eng_set_depth_range,
    .clear_rect           = vk_eng_clear_rect,
    .pipeline_prewarm     = vk_eng_pipeline_prewarm,
    .pipeline_async       = vk_eng_pipeline_async,
    .scene_aa             = vk_eng_scene_aa,
    .depth_target_create_ex = vk_eng_depth_target_create_ex,
    .target_set_aa        = vk_eng_target_set_aa,
    .point_sprite_supported = vk_eng_point_sprite_supported,
    .depth_target_copy    = vk_eng_depth_target_copy,
    .surface_typeless_copy = vk_eng_surface_typeless_copy,
};

static rsx_backend s_vulkan_backend = {
    .userdata           = &s_vk,
    .set_render_target  = vk_cb_track_state,
    .set_vertex_attribs = vk_cb_track_state,
    .set_depth_stencil  = vk_cb_track_state,
    .clear              = vk_cb_clear,
    .draw_arrays        = vk_cb_draw_arrays,
    .draw_indexed       = vk_cb_draw_indexed,
    .bind_texture       = vk_cb_bind_texture,
    .present            = vk_cb_present,
};

/* ---------------------------------------------------------------------------
 * Optional validation (PS3RECOMP_VK_VALIDATION=1, or =sync to add
 * synchronization validation): VK_LAYER_KHRONOS_validation when installed,
 * with a VK_EXT_debug_utils messenger that prints and counts every warning
 * and error (rsx_vulkan_backend_validation_messages).
 * -------------------------------------------------------------------------*/
static struct {
    int on;
    VkDebugUtilsMessengerEXT messenger;
    volatile long count;
    const char* exts[48];
    const char* layer;
    VkValidationFeaturesEXT features;
    VkValidationFeatureEnableEXT enables[2];
} s_val;

static VKAPI_ATTR VkBool32 VKAPI_CALL vk_val_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
{
    (void)type; (void)user;
    if (!(sev & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))) return VK_FALSE;
#ifdef _WIN32
    InterlockedIncrement(&s_val.count);
#else
    __sync_fetch_and_add(&s_val.count, 1);
#endif
    VK_LOG("VALIDATION %s: %s\n", (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "error" : "warning",
           data && data->pMessage ? data->pMessage : "?");
    return VK_FALSE;
}

static void vk_val_instance(VkInstanceCreateInfo* ci)
{
    const char* e = getenv("PS3RECOMP_VK_VALIDATION");
    memset(&s_val, 0, sizeof s_val);
    if (!e || !*e || *e == '0') return;
    PFN_vkEnumerateInstanceLayerProperties el = (PFN_vkEnumerateInstanceLayerProperties)
        pvkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
    u32 n = 0;
    int have = 0;
    if (el && el(&n, NULL) == VK_SUCCESS && n) {
        VkLayerProperties* lp = (VkLayerProperties*)malloc(n * sizeof *lp);
        if (lp && el(&n, lp) == VK_SUCCESS)
            for (u32 i = 0; i < n && !have; i++)
                have = !strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation");
        free(lp);
    }
    if (!have) { VK_LOG("validation: VK_LAYER_KHRONOS_validation is not installed\n"); return; }
    u32 ne = 0;
    for (u32 i = 0; i < ci->enabledExtensionCount && ne < 47; i++)
        s_val.exts[ne++] = ci->ppEnabledExtensionNames[i];
    s_val.exts[ne++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    ci->enabledExtensionCount = ne;
    ci->ppEnabledExtensionNames = s_val.exts;
    s_val.layer = "VK_LAYER_KHRONOS_validation";
    ci->enabledLayerCount = 1;
    ci->ppEnabledLayerNames = &s_val.layer;
    if (!strcmp(e, "sync")) {
        s_val.enables[0] = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        s_val.features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
        s_val.features.enabledValidationFeatureCount = 1;
        s_val.features.pEnabledValidationFeatures = s_val.enables;
        s_val.features.pNext = ci->pNext;
        ci->pNext = &s_val.features;
    }
    s_val.on = 1;
}

static void vk_val_messenger(void)
{
    if (!s_val.on) return;
    PFN_vkCreateDebugUtilsMessengerEXT cm = (PFN_vkCreateDebugUtilsMessengerEXT)
        pvkGetInstanceProcAddr(s_vk.instance, "vkCreateDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerCreateInfoEXT mci = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = vk_val_callback };
    if (!cm || cm(s_vk.instance, &mci, NULL, &s_val.messenger) != VK_SUCCESS)
        VK_LOG("validation: no debug messenger\n");
    else
        VK_LOG("validation: VK_LAYER_KHRONOS_validation on%s\n",
               s_val.features.sType ? " (with synchronization validation)" : "");
}

static void vk_val_destroy(void)
{
    if (!s_val.messenger || !s_vk.instance) return;
    PFN_vkDestroyDebugUtilsMessengerEXT dm = (PFN_vkDestroyDebugUtilsMessengerEXT)
        pvkGetInstanceProcAddr(s_vk.instance, "vkDestroyDebugUtilsMessengerEXT");
    if (dm) dm(s_vk.instance, s_val.messenger, NULL);
    s_val.messenger = VK_NULL_HANDLE;
}

u32 rsx_vulkan_backend_validation_messages(void) { return (u32)s_val.count; }

const void* rsx_vulkan_backend_engine_backend(void) { return &s_vk_engine_backend; }

/* ---------------------------------------------------------------------------
 * Public entry points
 * -------------------------------------------------------------------------*/
static int vk_init_all(u32 width, u32 height, const char* title)
{
    pthread_mutex_init(&s_vk.queue_mu, NULL);
    pthread_mutex_init(&s_vk.present_mu, NULL);
    s_vk.width  = width  ? width  : 1280;
    s_vk.height = height ? height : 720;
    s_vk.dump_path = getenv("PS3RECOMP_VK_DUMP");
    /* Writing a full frame to disk on every present costs more than the
     * present itself and slows a title enough to change what it does; a
     * live title wants one now and then, not all of them. */
    { const char* e = getenv("PS3RECOMP_VK_DUMP_EVERY");
      s_vk.dump_every = (e && atoi(e) > 0) ? (u32)atoi(e) : 1; }

    vk_window_open(title);           /* no-op unless PS3RECOMP_VK_WINDOW is set */
    if (vk_load_library()) return -1;

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "ps3recomp", .pEngineName = "ps3recomp RSX",
                              .apiVersion = vk_aa_api_version() };
    /* Vulkan 1.1 when the loader has it: VK_EXT_provoking_vertex needs 1.1
     * (or VK_KHR_get_physical_device_properties2) on the instance. */
    if (app.apiVersion < VK_API_VERSION_1_1) {
        PFN_vkEnumerateInstanceVersion ev = (PFN_vkEnumerateInstanceVersion)
            pvkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
        uint32_t lv = VK_API_VERSION_1_0;
        if (ev && ev(&lv) == VK_SUCCESS && lv >= VK_API_VERSION_1_1) app.apiVersion = VK_API_VERSION_1_1;
    }
    s_vk.inst_api = app.apiVersion;
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app };
#ifdef _WIN32
    /* RPCS3 instance.cpp enables VK_KHR_get_surface_capabilities2 when the
     * loader has it; swapchain_WSI queries exclusive fullscreen through it. */
    if (s_vk.windowed && s_vk.inst_ext_count < 16 &&
        vk_has_instance_extension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
        s_vk.inst_exts[s_vk.inst_ext_count++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
        s_vk.surf_caps2_ok = 1;
    }
#endif
    if (s_vk.windowed) {
        ci.enabledExtensionCount   = s_vk.inst_ext_count;
        ci.ppEnabledExtensionNames = s_vk.inst_exts;
    }
    vk_aa_instance_exts(&ci);
    vk_val_instance(&ci);
    VkResult ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    if (ir != VK_SUCCESS && s_vk.windowed) {
        vk_window_drop("vkCreateInstance with the surface extensions failed");
        s_vk.surf_caps2_ok = 0;
        ci.enabledExtensionCount = 0;
        ci.ppEnabledExtensionNames = NULL;
        ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    }
    if (ir != VK_SUCCESS) { VK_LOG("vkCreateInstance failed (VkResult %d)\n", (int)ir); return -1; }
    vk_val_messenger();
    if (vk_load_instance_functions()) return -1;
    if (s_vk.windowed) {
        if (vk_load_wsi_instance_functions()) {
            vk_window_drop("the driver has no surface functions");
        } else {
#ifdef _WIN32
            PFN_vkCreateWin32SurfaceKHR create_surface = (PFN_vkCreateWin32SurfaceKHR)
                pvkGetInstanceProcAddr(s_vk.instance, "vkCreateWin32SurfaceKHR");
            VkWin32SurfaceCreateInfoKHR wci = { .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                                .hinstance = GetModuleHandleW(NULL),
                                                .hwnd = (HWND)s_vk.window };
            if (!create_surface || create_surface(s_vk.instance, &wci, NULL, &s_vk.surface) != VK_SUCCESS)
                vk_window_drop("vkCreateWin32SurfaceKHR failed");
#else
            if (!SDL_Vulkan_CreateSurface(s_vk.window, s_vk.instance, &s_vk.surface)) {
                char why[256];
                snprintf(why, sizeof why, "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
                vk_window_drop(why);
            }
#endif
        }
    }
    if (vk_pick_device()) {
        if (!s_vk.windowed) return -1;
        vk_window_drop("no device can present to the window");
        if (vk_pick_device()) return -1;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = s_vk.queue_family,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    const char* dev_exts[8];
    u32 n_dev_exts = 0;
    if (s_vk.windowed) dev_exts[n_dev_exts++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
#ifdef _WIN32
    /* RPCS3 device.cpp: VK_EXT_full_screen_exclusive whenever the device
     * has it (and the instance has get_surface_capabilities2). */
    s_vk.fse_ok = s_vk.windowed && s_vk.surf_caps2_ok && s_vk.inst_api >= VK_API_VERSION_1_1 &&
        vk_has_device_extension(s_vk.phys, VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
    if (s_vk.fse_ok) dev_exts[n_dev_exts++] = VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME;
#endif
    /* RPCS3's MIRROR_ONCE_* wrap modes (vk_wrap_mode). */
    s_vk.mirror_clamp_ok = vk_has_device_extension(s_vk.phys, "VK_KHR_sampler_mirror_clamp_to_edge");
    if (s_vk.mirror_clamp_ok) dev_exts[n_dev_exts++] = "VK_KHR_sampler_mirror_clamp_to_edge";
    /* DXT (BC1-3) is how PS3 titles ship most textures; Vulkan gates it
     * behind a feature, enabled only when the device reports it. */
    VkPhysicalDeviceFeatures have, want;
    memset(&want, 0, sizeof want);
    pvkGetPhysicalDeviceFeatures(s_vk.phys, &have);
    want.textureCompressionBC = have.textureCompressionBC;
    s_vk.bc_ok = have.textureCompressionBC ? 1 : 0;
    /* The depth bounds test: a deferred renderer bounds its light pass to
     * the geometry and its sky to the far plane with it. Optional in
     * Vulkan; without it the test stays off, as before. */
    want.depthBounds = have.depthBounds;
    s_vk.depth_bounds_ok = have.depthBounds ? 1 : 0;
    want.logicOp = have.logicOp;
    s_vk.logic_op_ok = have.logicOp ? 1 : 0;
    /* RPCS3 device.cpp enables both: depth clamp for ZMIN_MAX_CONTROL,
     * wide lines for LINE_WIDTH. */
    want.depthClamp = have.depthClamp;
    s_vk.depth_clamp_ok = have.depthClamp ? 1 : 0;
    want.wideLines = have.wideLines;
    s_vk.wide_lines_ok = have.wideLines ? 1 : 0;
    /* RPCS3 device.cpp tests fillModeNonSolid; POINT/LINE polygon modes
     * need it (only with RSX_ENG_POLYGON_MODE=1; FILL by default). */
    want.fillModeNonSolid = have.fillModeNonSolid;
    s_vk.fill_non_solid_ok = have.fillModeNonSolid ? 1 : 0;
    /* RPCS3 VKGSRender MSAA: per-sample shading and alpha-to-one. */
    want.sampleRateShading = have.sampleRateShading;
    s_ms_srs_ok = have.sampleRateShading ? 1 : 0;
    want.alphaToOne = have.alphaToOne;
    s_ms_a2one_ok = have.alphaToOne ? 1 : 0;
    {
        VkPhysicalDeviceProperties dp;
        pvkGetPhysicalDeviceProperties(s_vk.phys, &dp);
        s_vk.nv_d24_bias_half = dp.vendorID == 0x10DEu;
        rsx_vp_set_low_precision_compare(dp.vendorID == 0x10DEu);
        rsx_draw_engine_set_host_nvidia(dp.vendorID == 0x10DEu);
        s_vk.line_w_min = dp.limits.lineWidthRange[0];
        s_vk.line_w_max = dp.limits.lineWidthRange[1];
    }
    /* RPCS3 precise ZPASS counts (occlusion queries). */
    want.occlusionQueryPrecise = have.occlusionQueryPrecise;
    s_vkq_precise_ok = have.occlusionQueryPrecise ? 1 : 0;
    want.samplerAnisotropy = have.samplerAnisotropy;
    /* RPCS3 user clip planes (gl_ClipDistance): the VP emits them only
     * once the device has the feature. */
    want.shaderClipDistance = have.shaderClipDistance;
    rsx_vp_set_clip_distances(have.shaderClipDistance ? 1 : 0);
    {
        VkPhysicalDeviceProperties pp;
        pvkGetPhysicalDeviceProperties(s_vk.phys, &pp);
        if (have.samplerAnisotropy) s_vk.aniso_max = pp.limits.maxSamplerAnisotropy;
        s_vk.lod_bias_max = pp.limits.maxSamplerLodBias;
    }
    /* RPCS3 device.cpp:97-120,608,866-871: VK_EXT_provoking_vertex with
     * provokingVertexLast (required by the extension) for SHADE_MODE FLAT.
     * RSX_ENG_FLAT_SHADE=0 leaves it off and colours smooth. */
    static VkPhysicalDeviceProvokingVertexFeaturesEXT pv_feat;
    memset(&pv_feat, 0, sizeof pv_feat);
    /* RPCS3 device.cpp:140/573: requested whenever listed; it lets
     * set_viewport take the raw CLIP_MIN/MAX range (VKGSRender.cpp:1234). */
    s_vk.depth_unrestricted = vk_has_device_extension(s_vk.phys, "VK_EXT_depth_range_unrestricted");
    if (s_vk.depth_unrestricted) dev_exts[n_dev_exts++] = "VK_EXT_depth_range_unrestricted";
    {
        const char* e = getenv("RSX_ENG_FLAT_SHADE");
        VkPhysicalDeviceProperties dp;
        pvkGetPhysicalDeviceProperties(s_vk.phys, &dp);
        s_vk.provoking_last_ok = !(e && *e == '0') && s_vk.inst_api >= VK_API_VERSION_1_1 &&
            dp.apiVersion >= VK_API_VERSION_1_1 &&
            vk_has_device_extension(s_vk.phys, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
        if (s_vk.provoking_last_ok) {
            dev_exts[n_dev_exts++] = VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME;
            pv_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT;
            pv_feat.provokingVertexLast = VK_TRUE;
        }
    }
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .pNext = s_vk.provoking_last_ok ? &pv_feat : NULL,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = n_dev_exts,
                               .ppEnabledExtensionNames = n_dev_exts ? dev_exts : NULL,
                               .pEnabledFeatures = &want };
    /* RPCS3 device.cpp:34-152,558,791-798: VK_EXT_custom_border_color with
     * customBorderColors + customBorderColorWithoutFormat; without both, the
     * closest core colour (vkutils/sampler.cpp). VK_EXT_border_color_swizzle
     * is only queried: borderColorSwizzleFromImage means the device already
     * swizzles the border, else every vendor but AMD needs the colour
     * pre-swizzled (require_border_color_remap). RSX_VK_CUSTOM_BORDER=0 keeps
     * the closest-enum path. */
    static VkPhysicalDeviceCustomBorderColorFeaturesEXT cb_feat;
    memset(&cb_feat, 0, sizeof cb_feat);
    s_vk.custom_border_ok = 0;
    {
        int remap = 1;
        VkPhysicalDeviceProperties dp;
        pvkGetPhysicalDeviceProperties(s_vk.phys, &dp);
        PFN_vkGetPhysicalDeviceFeatures2 gf2 = s_vk.inst_api >= VK_API_VERSION_1_1 &&
            dp.apiVersion >= VK_API_VERSION_1_1 ? (PFN_vkGetPhysicalDeviceFeatures2)
            pvkGetInstanceProcAddr(s_vk.instance, "vkGetPhysicalDeviceFeatures2") : NULL;
        const int has_cb = gf2 && vk_has_device_extension(s_vk.phys, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
        const int has_sw = gf2 && vk_has_device_extension(s_vk.phys, VK_EXT_BORDER_COLOR_SWIZZLE_EXTENSION_NAME);
        if (has_cb || has_sw) {
            VkPhysicalDeviceCustomBorderColorFeaturesEXT qcb = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT };
            VkPhysicalDeviceBorderColorSwizzleFeaturesEXT qsw = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT };
            VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            void** tail = &f2.pNext;
            if (has_cb) { *tail = &qcb; tail = &qcb.pNext; }
            if (has_sw) { *tail = &qsw; }
            gf2(s_vk.phys, &f2);
            remap = rsx_vk_border_needs_remap(has_sw, qsw.borderColorSwizzleFromImage != 0, dp.vendorID);
            const char* e = getenv("RSX_VK_CUSTOM_BORDER");
            if (has_cb && qcb.customBorderColors && qcb.customBorderColorWithoutFormat &&
                !(e && *e == '0') && n_dev_exts < 8) {
                s_vk.custom_border_ok = 1;
                cb_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT;
                cb_feat.customBorderColors = VK_TRUE;
                cb_feat.customBorderColorWithoutFormat = VK_TRUE;
                cb_feat.pNext = (void*)dci.pNext;
                dci.pNext = &cb_feat;
                dev_exts[n_dev_exts++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
                dci.enabledExtensionCount = n_dev_exts;
                dci.ppEnabledExtensionNames = dev_exts;
            }
        } else {
            remap = rsx_vk_border_needs_remap(0, 0, dp.vendorID);
        }
        rsx_draw_engine_set_border_color_remap(remap);
    }
    {
        const VkDeviceCreateInfo plain = dci;
        vk_aa_device(&dci, &want, &have);
        if (s_aa.ops && pvkCreateDevice(s_vk.phys, &dci, NULL, &s_vk.device) != VK_SUCCESS) {
            VK_LOG("temporal upscaler: vkCreateDevice with its extensions failed, disabled\n");
            s_aa.broken = 1;
            s_vk.device = VK_NULL_HANDLE;
            dci = plain;
            want.shaderStorageImageExtendedFormats = VK_FALSE;
        }
    }
    if (!s_vk.device && s_vk.provoking_last_ok &&
        pvkCreateDevice(s_vk.phys, &dci, NULL, &s_vk.device) != VK_SUCCESS) {
        /* Unlink the feature and drop the extension, then retry plain. */
        VK_LOG("engine: vkCreateDevice with VK_EXT_provoking_vertex failed; flat shading off\n");
        s_vk.device = VK_NULL_HANDLE;
        s_vk.provoking_last_ok = 0;
        for (VkBaseOutStructure* b = (VkBaseOutStructure*)&dci; b && b->pNext; b = b->pNext)
            if (b->pNext == (VkBaseOutStructure*)&pv_feat) { b->pNext = pv_feat.pNext; break; }
        static const char* kept[64];
        u32 nk = 0;
        for (u32 i = 0; i < dci.enabledExtensionCount && nk < 64; i++)
            if (strcmp(dci.ppEnabledExtensionNames[i], VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME))
                kept[nk++] = dci.ppEnabledExtensionNames[i];
        dci.enabledExtensionCount = nk;
        dci.ppEnabledExtensionNames = nk ? kept : NULL;
    }
    if (!s_vk.device && s_vk.custom_border_ok &&
        pvkCreateDevice(s_vk.phys, &dci, NULL, &s_vk.device) != VK_SUCCESS) {
        VK_LOG("engine: vkCreateDevice with VK_EXT_custom_border_color failed; closest border colours\n");
        s_vk.device = VK_NULL_HANDLE;
        s_vk.custom_border_ok = 0;
        for (VkBaseOutStructure* b = (VkBaseOutStructure*)&dci; b && b->pNext; b = b->pNext)
            if (b->pNext == (VkBaseOutStructure*)&cb_feat) { b->pNext = cb_feat.pNext; break; }
        static const char* kept_cb[64];
        u32 nk = 0;
        for (u32 i = 0; i < dci.enabledExtensionCount && nk < 64; i++)
            if (strcmp(dci.ppEnabledExtensionNames[i], VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME))
                kept_cb[nk++] = dci.ppEnabledExtensionNames[i];
        dci.enabledExtensionCount = nk;
        dci.ppEnabledExtensionNames = nk ? kept_cb : NULL;
    }
    if (!s_vk.device)
    VK_CHECK(pvkCreateDevice(s_vk.phys, &dci, NULL, &s_vk.device), "vkCreateDevice");
    rsx_draw_engine_set_last_provoking_vertex(s_vk.provoking_last_ok);
    if (s_vk.provoking_last_ok) VK_LOG("engine: last provoking vertex (VK_EXT_provoking_vertex)\n");
    if (s_vk.custom_border_ok) VK_LOG("engine: custom border colours (VK_EXT_custom_border_color)\n");
    if (vk_load_device_functions()) return -1;
    if (s_vk.windowed && vk_load_wsi_device_functions())
        vk_window_drop("the driver has no swapchain functions");
    pvkGetDeviceQueue(s_vk.device, s_vk.queue_family, 0, &s_vk.queue);
    vk_pcache_load();

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = s_vk.queue_family };
    VK_CHECK(pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vk.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = s_vk.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VK_CHECK(pvkAllocateCommandBuffers(s_vk.device, &cai, &s_vk.cmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.fence), "vkCreateFence");
    /* The present's own (see vk_state). */
    VK_CHECK(pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vk.present_pool), "vkCreateCommandPool");
    cai.commandPool = s_vk.present_pool;
    VK_CHECK(pvkAllocateCommandBuffers(s_vk.device, &cai, &s_vk.present_cmd), "vkAllocateCommandBuffers");
    VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.present_fence), "vkCreateFence");
    VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.window_fence), "vkCreateFence");

    if (vk_pick_depth_format()) return -1;
    if (vk_create_image(&s_vk.color, VK_FORMAT_R8G8B8A8_UNORM, s_vk.width, s_vk.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return -1;
    if (vk_create_image(&s_vk.depth, s_vk.depth_format, s_vk.width, s_vk.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, s_vk.depth_aspect)) return -1;
    if (vk_create_host_buffer(&s_vk.readback, (VkDeviceSize)s_vk.width * s_vk.height * 4u,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return -1;
    if (vk_create_render_pass()) return -1;
    if (vk_create_pipeline_objects()) return -1;
    if (vk_guest_init()) {
        VK_LOG("guest programs: setup failed, fixed-function path only\n");
        vk_guest_shutdown();
    }

    /* Both targets UNDEFINED -> GENERAL once. Colour starts opaque black so a
     * present before the first clear reads defined memory; depth starts at
     * the FAR plane, not zero -- zero is the near plane, and a guest that
     * enables LESS before its first depth clear would draw nothing (the
     * null backend makes the same choice). */
    if (vk_begin()) return -1;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange cr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageSubresourceRange dr = { s_vk.depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &cr);
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &dr);
    if (vk_submit_and_wait()) return -1;
    if (vk_fr_init()) return -1;
    vk_aa_device_ready();

    if (s_vk.windowed) {
        VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        /* 1 is a zero-sized (minimised) window: made on the first present. */
        if (pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sem_acquire) != VK_SUCCESS ||
            vk_create_swapchain() < 0)
            vk_window_drop("the swapchain could not be created");
    }
    return 0;
}

int rsx_vulkan_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_vk, 0, sizeof s_vk);
    /* Scaling is per target (vk_rs_pick): the engine's shaders read the
     * scale per draw (fp_rscale) instead of the global factor. */
    rsx_fp_set_res_scale(1.0);
    s_rs.init = 0;
    s_rs.dyn = vk_rs_global() > 1.0f;
    rsx_fp_set_res_scale_dynamic(s_rs.dyn);
    if (vk_init_all(width, height, title)) {
        rsx_vulkan_backend_shutdown();
        return -1;
    }
    /* One path at a time: the FIFO walker feeds both the rsx_state vtable
     * and the register-file engine, so registering both would record every
     * draw twice (the rule the Metal backend's init follows).
     *
     * With guest programs on, the engine is the default -- as it became
     * Metal's once every host mode passed through it. It builds every
     * pipeline from the guest's programs, so without them it has nothing to
     * draw with, and the vtable's fixed-function path stays the default.
     * Either way PS3RECOMP_RSX_ENGINE=dispatch|vtable overrides. */
    rsx_draw_engine_set_backend(&s_vk_engine_backend);
    rsx_draw_engine_set_default(s_g.on ? 1 : 0);
    if (rsx_draw_engine_enabled() && rsx_draw_engine_init(s_vk.width, s_vk.height) == 0)
        s_vk.eng_active = 1;
    else
        rsx_set_backend(&s_vulkan_backend);
    VK_LOG("%s path\n", s_vk.eng_active ? "register-file draw engine" : "vtable");
    VK_LOG("offscreen target %ux%u ready, depth format %d (%s)\n",
           s_vk.width, s_vk.height, (int)s_vk.depth_format, title ? title : "");
    return 0;
}

void rsx_vulkan_backend_shutdown(void)
{
    rsx_vulkan_backend_present_finish();   /* a host that began one and quit */
    if (rsx_get_backend() == &s_vulkan_backend) rsx_set_backend(NULL);
    if (s_vk.eng_active) { rsx_draw_engine_shutdown(); s_vk.eng_active = 0; }
    rsx_draw_engine_set_backend(NULL);
#ifndef _WIN32
    if (s_vk.windowed && s_vk.swapchain && s_vk.presented && s_vk.hold_seconds > 0.0) {
        VK_LOG("holding the last frame on screen for %.1f s "
               "(Esc or closing the window ends it early)\n", s_vk.hold_seconds);
        const Uint32 end = SDL_GetTicks() + (Uint32)(s_vk.hold_seconds * 1000.0);
        while (s_vk.windowed && !SDL_TICKS_PASSED(SDL_GetTicks(), end)) {
            if (rsx_vulkan_backend_pump_messages() != 0) break;
            vk_present_window();         /* FIFO: paced by the display */
        }
    }
#endif
    vk_flush(1);                         /* nothing recorded or in flight */
    if (s_vk.windowed || s_vk.window) vk_window_drop(NULL);
    if (s_vk.device) {
        vk_device_idle();
        vk_cx_shutdown();
        vk_fr_destroy();
        vk_guest_shutdown();
        for (int i = 0; i < VK_PIPELINE_SLOTS; i++)
            if (s_vk.pipelines[i]) pvkDestroyPipeline(s_vk.device, s_vk.pipelines[i], NULL);
        if (s_vk.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_vk.pipe_layout, NULL);
        if (s_vk.desc_pool)   pvkDestroyDescriptorPool(s_vk.device, s_vk.desc_pool, NULL);
        if (s_vk.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_vk.set_layout, NULL);
        if (s_vk.sampler)     pvkDestroySampler(s_vk.device, s_vk.sampler, NULL);
        for (u32 i = 0; i < s_vk.samp_count; i++)
            pvkDestroySampler(s_vk.device, s_vk.samp_cache[i].s, NULL);
        if (s_vk.vs)          pvkDestroyShaderModule(s_vk.device, s_vk.vs, NULL);
        if (s_vk.fs)          pvkDestroyShaderModule(s_vk.device, s_vk.fs, NULL);
        if (s_vk.framebuffer) pvkDestroyFramebuffer(s_vk.device, s_vk.framebuffer, NULL);
        if (s_vk.render_pass) pvkDestroyRenderPass(s_vk.device, s_vk.render_pass, NULL);
        for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) vk_destroy_image(&s_vk.tex[u]);
        vk_destroy_image(&s_vk.dummy);
        vk_destroy_image(&s_vk.depth);
        vk_destroy_image(&s_vk.color);
        vk_destroy_buffer(&s_vk.vertices);
        vk_destroy_buffer(&s_vk.readback);
        if (s_vk.fence) pvkDestroyFence(s_vk.device, s_vk.fence, NULL);
        if (s_vk.pool)  pvkDestroyCommandPool(s_vk.device, s_vk.pool, NULL);
        if (s_vk.present_fence) pvkDestroyFence(s_vk.device, s_vk.present_fence, NULL);
        if (s_vk.window_fence)  pvkDestroyFence(s_vk.device, s_vk.window_fence, NULL);
        if (s_vk.present_pool)  pvkDestroyCommandPool(s_vk.device, s_vk.present_pool, NULL);
        pvkDestroyDevice(s_vk.device, NULL);
    }
    if (s_vk.instance) vk_val_destroy();
    if (s_vk.instance && pvkDestroyInstance) pvkDestroyInstance(s_vk.instance, NULL);
    if (s_vk.lib) dlclose(s_vk.lib);
    free(s_vk.cpu_verts);
    pthread_mutex_destroy(&s_vk.queue_mu);
    pthread_mutex_destroy(&s_vk.present_mu);
    memset(&s_vk, 0, sizeof s_vk);
}

int rsx_vulkan_backend_pump_messages(void)
{
#ifdef _WIN32
    return 0;                         /* the host pumps its own window */
#else
    if (!s_vk.windowed) return 0;
    int quit = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quit = 1;
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) quit = 1;
    }
    return quit ? -1 : 0;
#endif
}

void rsx_vulkan_backend_set_window(void* hwnd)
{
#ifdef _WIN32
    s_vk_hwnd = (HWND)hwnd;
#else
    (void)hwnd;
#endif
}

void rsx_vulkan_backend_present(void)
{
    rsx_vulkan_backend_present_begin();
    rsx_vulkan_backend_present_finish();
}

void rsx_vulkan_backend_present_begin(void)
{
    if (!s_vk.device) return;
    s_vk.present_split = 1;
    if (s_vk.eng_active) rsx_draw_engine_present();
    else                 vk_cb_present(&s_vk, 0);
    s_vk.present_split = 0;
}

void rsx_vulkan_backend_present_finish(void)
{
    if (!s_vk.device) return;
    pthread_mutex_lock(&s_vk.present_mu);
    vk_present_complete_locked();
    pthread_mutex_unlock(&s_vk.present_mu);
}

u32 rsx_vulkan_backend_presented_center(void)
{
    return s_vk.presented ? (0xFF000000u | s_vk.last_center) : 0u;
}

u32 rsx_vulkan_backend_debug_color(void) { return s_vk.clear_argb; }

u32 rsx_vulkan_backend_readback_center(void)
{
    if (s_vk.eng_active) return rsx_draw_engine_readback_center();
    return s_vk.presented ? (0xFF000000u | s_vk.last_center) : 0u;
}

u32 rsx_vulkan_backend_guest_draws(void)
{
    return s_vk.eng_active ? rsx_draw_engine_guest_draws() : s_g.last_draws;
}

int rsx_vulkan_backend_depth_bounds(void)
{
    return s_vk.eng_active && s_vk.depth_bounds_ok;
}

