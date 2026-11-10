/*
 * AMD FidelityFX Super Resolution 3.1 upscaler for the Vulkan backend's
 * temporal-upscaler slot. See rsx_vk_fsr3.h for the conventions.
 * C++ only because ffx_api_vk.h is; the interface has C linkage.
 */
#include "rsx_vk_fsr3.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && defined(PS3RECOMP_HAVE_FFX)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vulkan/vulkan.h>
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>

static struct {
    HMODULE             dll;
    int                 tried, failed;
    rsx_vk_fsr3_log_fn  log;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxDispatch      dispatch;
    PfnFfxQuery         query;
    VkDevice            device;
    VkPhysicalDevice    phys;
    PFN_vkGetDeviceProcAddr gdpa;
    ffxContext          ctx;
    unsigned            w, h, ow, oh;
    int                 hdr;
    float               sharp;
    unsigned            messages, dispatch_errors;
} s_fx;

static void fx_log(const char* fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (s_fx.log) s_fx.log(b);
    else fprintf(stderr, "[fsr3] %s\n", b);
}

static void fx_message(uint32_t type, const wchar_t* message)
{
    if (s_fx.messages >= 32) return;     /* the runtime can repeat a warning every frame */
    if (++s_fx.messages == 32) { fx_log("FSR 3.1: further runtime messages muted"); return; }
    char b[384] = { 0 };
    WideCharToMultiByte(CP_UTF8, 0, message ? message : L"", -1, b, (int)sizeof b - 1, NULL, NULL);
    fx_log("FSR 3.1 %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", b);
}

int rsx_vk_fsr3_load(const char* dll_path, rsx_vk_fsr3_log_fn log)
{
    if (log) s_fx.log = log;
    if (s_fx.tried) return s_fx.dll != NULL;
    s_fx.tried = 1;
    char path[MAX_PATH];
    if (dll_path && *dll_path) {
        snprintf(path, sizeof path, "%s", dll_path);
    } else {
        DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) n = 0;
        while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
        snprintf(path + n, sizeof path - n, "amd_fidelityfx_vk.dll");
    }
    s_fx.dll = LoadLibraryA(path);
    if (!s_fx.dll) {
        fx_log("FSR 3.1 unavailable: %s could not be loaded (error %lu; it also needs the "
               "Visual C++ 2015-2022 runtime)", path, (unsigned long)GetLastError());
        return 0;
    }
    s_fx.create = (PfnFfxCreateContext)(void*)GetProcAddress(s_fx.dll, "ffxCreateContext");
    s_fx.destroy = (PfnFfxDestroyContext)(void*)GetProcAddress(s_fx.dll, "ffxDestroyContext");
    s_fx.dispatch = (PfnFfxDispatch)(void*)GetProcAddress(s_fx.dll, "ffxDispatch");
    s_fx.query = (PfnFfxQuery)(void*)GetProcAddress(s_fx.dll, "ffxQuery");
    if (!s_fx.create || !s_fx.destroy || !s_fx.dispatch || !s_fx.query) {
        fx_log("FSR 3.1 unavailable: %s has no FidelityFX API exports", path);
        FreeLibrary(s_fx.dll);
        s_fx.dll = NULL;
        return 0;
    }
    return 1;
}

void rsx_vk_fsr3_set_sharpness(float sharpness)
{
    s_fx.sharp = sharpness > 0.0f ? (sharpness < 1.0f ? sharpness : 1.0f) : 0.0f;
}

int rsx_vk_fsr3_failed(void) { return s_fx.failed; }

int rsx_vk_fsr3_render_size(unsigned* w, unsigned* h)
{
    if (!s_fx.ctx) return 0;
    if (w) *w = s_fx.w;
    if (h) *h = s_fx.h;
    return 1;
}

int rsx_vk_fsr3_jitter(unsigned index, unsigned phases, float* x, float* y)
{
    if (!s_fx.ctx || !x || !y) return 0;
    int32_t n = (int32_t)phases;
    if (n <= 0) {
        struct ffxQueryDescUpscaleGetJitterPhaseCount pq;
        memset(&pq, 0, sizeof pq);
        pq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
        pq.renderWidth = s_fx.w;
        pq.displayWidth = s_fx.ow;
        pq.pOutPhaseCount = &n;
        if (s_fx.query(&s_fx.ctx, &pq.header) != FFX_API_RETURN_OK || n <= 0) return 0;
    }
    struct ffxQueryDescUpscaleGetJitterOffset jq;
    memset(&jq, 0, sizeof jq);
    jq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    jq.index = (int32_t)(index % (unsigned)n);
    jq.phaseCount = n;
    jq.pOutX = x;
    jq.pOutY = y;
    if (s_fx.query(&s_fx.ctx, &jq.header) != FFX_API_RETURN_OK) return 0;
    return (int)n;
}

static void fx_destroy_context(void)
{
    if (!s_fx.ctx) return;
    /* The previous context may still be referenced by submitted frames. */
    PFN_vkDeviceWaitIdle wait = s_fx.gdpa ? (PFN_vkDeviceWaitIdle)s_fx.gdpa(s_fx.device, "vkDeviceWaitIdle") : NULL;
    if (wait) wait(s_fx.device);
    s_fx.destroy(&s_fx.ctx, NULL);
    s_fx.ctx = NULL;
}

/* ---- rsx_vk_upscaler_ops ------------------------------------------------ */
static const char* s_fx_dev_exts[] = {
    /* FSR loads the KHR-suffixed entry points, which vkGetDeviceProcAddr
     * only returns when the extension is enabled. */
    "VK_KHR_get_memory_requirements2",
    "VK_KHR_dedicated_allocation",
    /* wave32/64 shader permutations */
    "VK_EXT_subgroup_size_control",
};

static int op_required(void* user, unsigned* ni, const char*** ie, unsigned* nd, const char*** de)
{
    (void)user;
    *ni = 0; *ie = NULL;
    *nd = (unsigned)(sizeof s_fx_dev_exts / sizeof s_fx_dev_exts[0]);
    *de = s_fx_dev_exts;
    return 1;
}

/* FSR picks shader permutations from what the GPU supports, so the optional
 * features it may use are enabled when supported. */
static void op_device_features(void* user, void* physical_device, void* gipa, void* instance,
                               void* features, const void* have, void* v12, const void* have12,
                               unsigned ext_count, const char* const* exts, const void** pnext)
{
    (void)user;
    VkPhysicalDeviceFeatures* f = (VkPhysicalDeviceFeatures*)features;
    const VkPhysicalDeviceFeatures* hf = (const VkPhysicalDeviceFeatures*)have;
    f->shaderInt16 |= hf->shaderInt16;
    f->shaderImageGatherExtended |= hf->shaderImageGatherExtended;
    f->shaderStorageImageExtendedFormats |= hf->shaderStorageImageExtendedFormats;
    f->shaderStorageImageReadWithoutFormat |= hf->shaderStorageImageReadWithoutFormat;
    f->shaderStorageImageWriteWithoutFormat |= hf->shaderStorageImageWriteWithoutFormat;
    if (!v12 || !have12) return;     /* Vulkan 1.2 device only */
    VkPhysicalDeviceVulkan12Features* f12 = (VkPhysicalDeviceVulkan12Features*)v12;
    const VkPhysicalDeviceVulkan12Features* h12 = (const VkPhysicalDeviceVulkan12Features*)have12;
    f12->shaderFloat16 |= h12->shaderFloat16;
    f12->shaderInt8 |= h12->shaderInt8;
    f12->shaderSubgroupExtendedTypes |= h12->shaderSubgroupExtendedTypes;
    f12->storageBuffer8BitAccess |= h12->storageBuffer8BitAccess;

    PFN_vkGetPhysicalDeviceFeatures2 gf2 = gipa ? (PFN_vkGetPhysicalDeviceFeatures2)
        ((PFN_vkGetInstanceProcAddr)gipa)((VkInstance)instance, "vkGetPhysicalDeviceFeatures2") : NULL;
    if (!gf2 || !pnext) return;
    int ssc_on = 0;
    for (unsigned i = 0; i < ext_count; i++)
        if (!strcmp(exts[i], "VK_EXT_subgroup_size_control")) ssc_on = 1;
    static VkPhysicalDevice16BitStorageFeatures s16;
    static VkPhysicalDeviceSubgroupSizeControlFeaturesEXT ssc;
    VkPhysicalDevice16BitStorageFeatures q16 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES };
    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT qssc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT };
    if (ssc_on) q16.pNext = &qssc;
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &q16 };
    gf2((VkPhysicalDevice)physical_device, &f2);
    memset(&s16, 0, sizeof s16);
    s16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
    s16.storageBuffer16BitAccess = q16.storageBuffer16BitAccess;
    s16.uniformAndStorageBuffer16BitAccess = q16.uniformAndStorageBuffer16BitAccess;
    s16.storagePushConstant16 = q16.storagePushConstant16;
    s16.storageInputOutput16 = q16.storageInputOutput16;
    s16.pNext = (void*)*pnext;
    *pnext = &s16;
    if (ssc_on && qssc.subgroupSizeControl) {
        memset(&ssc, 0, sizeof ssc);
        ssc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT;
        ssc.subgroupSizeControl = VK_TRUE;
        ssc.computeFullSubgroups = qssc.computeFullSubgroups;
        ssc.pNext = (void*)*pnext;
        *pnext = &ssc;
    }
}

static int op_init(void* user, void* instance, void* physical_device, void* device, void* gipa, void* gdpa)
{
    (void)user; (void)instance; (void)gipa;
    if (!s_fx.dll || !device || !physical_device || !gdpa) return 0;
    s_fx.device = (VkDevice)device;
    s_fx.phys = (VkPhysicalDevice)physical_device;
    s_fx.gdpa = (PFN_vkGetDeviceProcAddr)gdpa;
    return 1;
}

static int op_create(void* user, void* cmd, unsigned w, unsigned h, unsigned ow, unsigned oh, int flags)
{
    (void)user; (void)cmd;
    if (!s_fx.dll || !s_fx.device) return 0;
    fx_destroy_context();
    struct ffxCreateBackendVKDesc be;
    memset(&be, 0, sizeof be);
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.vkDevice = s_fx.device;
    be.vkPhysicalDevice = s_fx.phys;
    be.vkDeviceProcAddr = s_fx.gdpa;
    struct ffxCreateContextDescUpscale cd;
    memset(&cd, 0, sizeof cd);
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.header.pNext = &be.header;
    s_fx.hdr = (flags & 2) != 0;
    cd.flags = (s_fx.hdr ? (FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE)
                         : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE) |
               ((flags & 1) ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0);
    cd.maxRenderSize.width = w; cd.maxRenderSize.height = h;
    cd.maxUpscaleSize.width = ow; cd.maxUpscaleSize.height = oh;
    cd.fpMessage = fx_message;
    const ffxReturnCode_t rc = s_fx.create(&s_fx.ctx, &cd.header, NULL);
    if (rc != FFX_API_RETURN_OK || !s_fx.ctx) {
        s_fx.ctx = NULL;
        s_fx.failed = 1;
        fx_log("FSR 3.1 context creation failed (code %u)", (unsigned)rc);
        return 0;
    }
    s_fx.w = w; s_fx.h = h; s_fx.ow = ow; s_fx.oh = oh;
    struct ffxQueryGetProviderVersion pv;
    memset(&pv, 0, sizeof pv);
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const int named = s_fx.query(&s_fx.ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName;
    fx_log("FSR 3.1 context %ux%u -> %ux%u (%s, %s depth, %s colour), provider %s", w, h, ow, oh,
           ow > w ? "upscaling" : "native AA", (flags & 1) ? "inverted" : "standard",
           s_fx.hdr ? "FP16 HDR" : "8-bit sRGB", named ? pv.versionName : "?");
    return 1;
}

static struct FfxApiResource fx_res(const rsx_vk_upscaler_image* im, unsigned w, unsigned h, int uav)
{
    struct FfxApiResource r;
    memset(&r, 0, sizeof r);
    r.resource = (void*)(uintptr_t)im->image;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffxApiGetSurfaceFormatVK((VkFormat)im->format);
    r.description.width = w;
    r.description.height = h;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = uav ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    /* the backend hands inputs in SHADER_READ_ONLY_OPTIMAL, the output in GENERAL */
    r.state = uav ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ;
    return r;
}

static int op_evaluate(void* user, void* cmd, const rsx_vk_upscaler_image* color,
                       const rsx_vk_upscaler_image* depth, const rsx_vk_upscaler_image* mv,
                       const rsx_vk_upscaler_image* mask, const rsx_vk_upscaler_image* out,
                       unsigned out_w, unsigned out_h, float jitter_x, float jitter_y,
                       float mv_scale_x, float mv_scale_y, int reset, float frame_ms)
{
    (void)user;
    if (!s_fx.ctx || !cmd) return 0;
    struct ffxDispatchDescUpscale d;
    memset(&d, 0, sizeof d);
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cmd;
    d.color = fx_res(color, s_fx.w, s_fx.h, 0);
    d.depth = fx_res(depth, s_fx.w, s_fx.h, 0);
    d.motionVectors = fx_res(mv, s_fx.w, s_fx.h, 0);
    if (mask && mask->image) d.reactive = fx_res(mask, s_fx.w, s_fx.h, 0);
    d.output = fx_res(out, out_w, out_h, 1);
    d.jitterOffset.x = jitter_x;
    d.jitterOffset.y = jitter_y;
    d.motionVectorScale.x = mv_scale_x;
    d.motionVectorScale.y = mv_scale_y;
    d.renderSize.width = s_fx.w;
    d.renderSize.height = s_fx.h;
    d.upscaleSize.width = out_w;
    d.upscaleSize.height = out_h;
    d.enableSharpening = s_fx.sharp > 0.0f;
    d.sharpness = s_fx.sharp;
    d.frameTimeDelta = frame_ms > 0.0f ? frame_ms : 16.7f;
    d.preExposure = 1.0f;
    d.reset = reset != 0;
    /* Unknown for a recompiled game; FSR 3.1 only uses them for its depth heuristics. */
    d.cameraNear = 0.1f;
    d.cameraFar = 1000.0f;
    d.cameraFovAngleVertical = 1.0f;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = s_fx.hdr ? 0u : (uint32_t)FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    const ffxReturnCode_t rc = s_fx.dispatch(&s_fx.ctx, &d.header);
    if (rc != FFX_API_RETURN_OK) {
        if (s_fx.dispatch_errors++ < 4) fx_log("FSR 3.1 dispatch failed (code %u)", (unsigned)rc);
        return 0;
    }
    return 1;
}

static void op_shutdown(void* user)
{
    (void)user;
    fx_destroy_context();
    s_fx.device = VK_NULL_HANDLE;
}

static const rsx_vk_upscaler_ops s_fx_ops = {
    .user = NULL,
    .required_extensions = op_required,
    .init = op_init,
    .create = op_create,
    .evaluate = op_evaluate,
    .shutdown = op_shutdown,
    .device_features = op_device_features,
};

const rsx_vk_upscaler_ops* rsx_vk_fsr3_ops(void) { return &s_fx_ops; }

#else /* no FidelityFX headers, or not Windows */

static int s_fx_logged;

int rsx_vk_fsr3_load(const char* dll_path, rsx_vk_fsr3_log_fn log)
{
    (void)dll_path;
    if (!s_fx_logged) {
        s_fx_logged = 1;
        const char* m = "FSR 3.1 unavailable: built without the FidelityFX API (PS3RECOMP_FFX_INCLUDE_DIR)";
        if (log) log(m);
        else fprintf(stderr, "[fsr3] %s\n", m);
    }
    return 0;
}
const rsx_vk_upscaler_ops* rsx_vk_fsr3_ops(void) { return NULL; }
void rsx_vk_fsr3_set_sharpness(float sharpness) { (void)sharpness; }
int  rsx_vk_fsr3_render_size(unsigned* w, unsigned* h) { (void)w; (void)h; return 0; }
int  rsx_vk_fsr3_jitter(unsigned index, unsigned phases, float* x, float* y)
{
    (void)index; (void)phases; (void)x; (void)y;
    return 0;
}
int  rsx_vk_fsr3_failed(void) { return 0; }

#endif
