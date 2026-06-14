/*
 * gpu_vk.c — GPU-1 Vulkan compute backend implementation (Android/host-only).
 *
 * Implements gpu_compute.h. Plain-float matmul on a Vulkan compute queue.
 *
 * ============================ COMPATIBILITY CORE ============================
 * The whole point of this file is that yurikago runs on EVERY Android device,
 * with or without usable Vulkan compute. So:
 *
 *   1. libvulkan.so is loaded with dlopen() AT RUNTIME — it is NOT linked, so
 *      it is NOT a DT_NEEDED of libpkernel.so. A device with no libvulkan.so
 *      (or a broken one) still loads our .so; gpu_available() just returns 0
 *      and the caller uses the CPU. We resolve every Vulkan entry point with
 *      vkGetInstanceProcAddr / vkGetDeviceProcAddr (the loader-blessed way),
 *      so we never reference a Vulkan symbol at link time.
 *
 *   2. EVERY step is checked. dlopen fail, no instance, no physical device,
 *      no compute queue, any VkResult != VK_SUCCESS, any allocation failure
 *      -> we tear down what we built and report "unavailable". gpu_matmul_f32
 *      returns <0; it never dereferences a null handle, never crashes.
 *
 *   3. A conservative enable flag (default OFF) gates everything on top of
 *      availability. The future settings toggle flips gpu_set_enabled().
 *
 * We deliberately pull in Vulkan via the headers for the STRUCT/ENUM
 * definitions only (compile-time), and NEVER call a Vulkan function directly —
 * all calls go through function pointers we resolve at runtime. To make that
 * airtight we define VK_NO_PROTOTYPES before including the header, so the
 * header declares NO function prototypes and a stray direct call won't link.
 * ===========================================================================
 */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "gpu_compute.h"

#include "matmul_f32_spv.h"   /* generated: matmul_f32_spv[] + _len (bytes) */

/* The T-Kernel placeholder <stdint.h> on this build's include path predates
 * the C99 limit macros, so UINT32_MAX may be absent. Define it defensively;
 * Bionic's real stdint would otherwise provide it. */
#ifndef UINT32_MAX
#define UINT32_MAX 0xFFFFFFFFu
#endif

/* ---- Android log (optional; harmless on host) -------------------------- */
#if defined(__ANDROID__)
#include <android/log.h>
#define GLOG(...) __android_log_print(ANDROID_LOG_INFO, "pkernel-gpu", __VA_ARGS__)
#else
#define GLOG(...) ((void)0)
#endif

/* ------------------------------------------------------------------------ */
/* Function-pointer table — everything resolved at runtime, nothing linked. */
/* ------------------------------------------------------------------------ */
static PFN_vkGetInstanceProcAddr                 pvkGetInstanceProcAddr;
static PFN_vkCreateInstance                       pvkCreateInstance;
static PFN_vkDestroyInstance                       pvkDestroyInstance;
static PFN_vkEnumeratePhysicalDevices              pvkEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties           pvkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties pvkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties     pvkGetPhysicalDeviceMemoryProperties;
static PFN_vkCreateDevice                          pvkCreateDevice;
static PFN_vkDestroyDevice                          pvkDestroyDevice;
static PFN_vkGetDeviceProcAddr                      pvkGetDeviceProcAddr;
static PFN_vkGetDeviceQueue                         pvkGetDeviceQueue;
static PFN_vkCreateBuffer                           pvkCreateBuffer;
static PFN_vkDestroyBuffer                          pvkDestroyBuffer;
static PFN_vkGetBufferMemoryRequirements            pvkGetBufferMemoryRequirements;
static PFN_vkAllocateMemory                         pvkAllocateMemory;
static PFN_vkFreeMemory                             pvkFreeMemory;
static PFN_vkBindBufferMemory                       pvkBindBufferMemory;
static PFN_vkMapMemory                              pvkMapMemory;
static PFN_vkUnmapMemory                            pvkUnmapMemory;
static PFN_vkFlushMappedMemoryRanges                pvkFlushMappedMemoryRanges;
static PFN_vkInvalidateMappedMemoryRanges           pvkInvalidateMappedMemoryRanges;
static PFN_vkCreateShaderModule                     pvkCreateShaderModule;
static PFN_vkDestroyShaderModule                    pvkDestroyShaderModule;
static PFN_vkCreateDescriptorSetLayout              pvkCreateDescriptorSetLayout;
static PFN_vkDestroyDescriptorSetLayout             pvkDestroyDescriptorSetLayout;
static PFN_vkCreatePipelineLayout                   pvkCreatePipelineLayout;
static PFN_vkDestroyPipelineLayout                  pvkDestroyPipelineLayout;
static PFN_vkCreateComputePipelines                 pvkCreateComputePipelines;
static PFN_vkDestroyPipeline                        pvkDestroyPipeline;
static PFN_vkCreateDescriptorPool                   pvkCreateDescriptorPool;
static PFN_vkDestroyDescriptorPool                  pvkDestroyDescriptorPool;
static PFN_vkAllocateDescriptorSets                 pvkAllocateDescriptorSets;
static PFN_vkUpdateDescriptorSets                   pvkUpdateDescriptorSets;
static PFN_vkCreateCommandPool                      pvkCreateCommandPool;
static PFN_vkDestroyCommandPool                     pvkDestroyCommandPool;
static PFN_vkAllocateCommandBuffers                 pvkAllocateCommandBuffers;
static PFN_vkFreeCommandBuffers                     pvkFreeCommandBuffers;
static PFN_vkBeginCommandBuffer                     pvkBeginCommandBuffer;
static PFN_vkEndCommandBuffer                       pvkEndCommandBuffer;
static PFN_vkCmdBindPipeline                        pvkCmdBindPipeline;
static PFN_vkCmdBindDescriptorSets                  pvkCmdBindDescriptorSets;
static PFN_vkCmdPushConstants                       pvkCmdPushConstants;
static PFN_vkCmdDispatch                            pvkCmdDispatch;
static PFN_vkCmdPipelineBarrier                     pvkCmdPipelineBarrier;
static PFN_vkQueueSubmit                            pvkQueueSubmit;
static PFN_vkQueueWaitIdle                          pvkQueueWaitIdle;
static PFN_vkCreateFence                            pvkCreateFence;
static PFN_vkDestroyFence                           pvkDestroyFence;
static PFN_vkWaitForFences                          pvkWaitForFences;
static PFN_vkResetCommandPool                       pvkResetCommandPool;

/* ------------------------------------------------------------------------ */
/* Backend state. All zero = nothing created.                               */
/* ------------------------------------------------------------------------ */
typedef enum {
    GPU_UNINIT = 0,   /* gpu_init not yet attempted                          */
    GPU_READY,        /* device + pipeline live                              */
    GPU_DEAD          /* init attempted and failed (cached; don't retry hot) */
} gpu_state_t;

static struct {
    gpu_state_t      state;
    int              enabled;        /* runtime flag; default OFF             */
    void            *vk_lib;         /* dlopen handle for libvulkan.so        */

    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    uint32_t         queue_family;
    VkQueue          queue;

    VkPhysicalDeviceMemoryProperties memprops;

    VkShaderModule        shader;
    VkDescriptorSetLayout dset_layout;
    VkPipelineLayout      pipe_layout;
    VkPipeline            pipeline;
    VkCommandPool         cmd_pool;

    char             desc[160];      /* gpu_describe() readout                */
    char             name[256];      /* picked physical-device name (gpu_name) */
} G;  /* zero-initialized */

/* ------------------------------------------------------------------------ */
/* Workgroup geometry — MUST stay in sync with the compute shader's         */
/* local_size_x / local_size_y (COLS_PER_ROW / ROWS_PER_WG). The shader     */
/* handles ROWS_PER_WG output rows per workgroup, so we dispatch            */
/* ceil(out / ROWS_PER_WG) groups in X.                                     */
/* ------------------------------------------------------------------------ */
#define GPU_COLS_PER_ROW 32u
#define GPU_ROWS_PER_WG   8u

/* ------------------------------------------------------------------------ */
/* Small helpers.                                                           */
/* ------------------------------------------------------------------------ */
static void set_desc(const char *s)
{
    /* libc-light: bounded copy, always NUL-terminated. */
    size_t i = 0;
    if (!s) s = "";
    while (s[i] && i + 1 < sizeof(G.desc)) { G.desc[i] = s[i]; i++; }
    G.desc[i] = '\0';
}

/* Pick a host-visible memory type satisfying `type_bits` and `want` flags.
 * Returns the index or UINT32_MAX if none (caller treats as failure). */
static uint32_t find_mem_type(uint32_t type_bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < G.memprops.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (G.memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return UINT32_MAX;
}

/* ------------------------------------------------------------------------ */
/* dlopen libvulkan + resolve entry points.  Returns 0 on success.          */
/* ------------------------------------------------------------------------ */
#define RES_INSTANCE(name)                                                  \
    do {                                                                    \
        p##name = (PFN_##name)pvkGetInstanceProcAddr(G.instance, #name);    \
        if (!p##name) { set_desc("missing " #name); return -1; }            \
    } while (0)

#define RES_DEVICE(name)                                                    \
    do {                                                                    \
        p##name = (PFN_##name)pvkGetDeviceProcAddr(G.device, #name);        \
        if (!p##name) { set_desc("missing " #name); return -1; }            \
    } while (0)

static int load_vulkan_loader(void)
{
    /* Try the canonical name first, then a versioned fallback. We never hard
     * link, so absence here just means "no GPU path" (CPU fallback). */
    const char *names[] = { "libvulkan.so", "libvulkan.so.1", NULL };
    for (int i = 0; names[i]; i++) {
        G.vk_lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (G.vk_lib) break;
    }
    if (!G.vk_lib) { set_desc("libvulkan.so not present (CPU only)"); return -1; }

    pvkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym(G.vk_lib, "vkGetInstanceProcAddr");
    if (!pvkGetInstanceProcAddr) {
        set_desc("vkGetInstanceProcAddr missing (CPU only)");
        return -1;
    }
    /* vkCreateInstance is resolvable with a NULL instance per the spec. */
    pvkCreateInstance =
        (PFN_vkCreateInstance)pvkGetInstanceProcAddr(NULL, "vkCreateInstance");
    if (!pvkCreateInstance) { set_desc("vkCreateInstance missing"); return -1; }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Bring up instance/device/pipeline. Returns 0 on success, <0 on any miss. */
/* On any failure the partial state is torn down by gpu_shutdown().          */
/* ------------------------------------------------------------------------ */
static int bringup(void)
{
    VkResult r;

    /* ---- instance --------------------------------------------------- */
    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "yurikago-gpu";
    app.applicationVersion = 1;
    app.pEngineName        = "pkernel";
    app.engineVersion      = 1;
    app.apiVersion         = VK_API_VERSION_1_0;   /* 1.0: widest support */

    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    /* No layers, no extensions: pure compute needs neither. Keeps us
     * loadable on the barest conformant driver. */
    r = pvkCreateInstance(&ici, NULL, &G.instance);
    if (r != VK_SUCCESS) { set_desc("vkCreateInstance failed"); return -1; }

    /* Resolve the instance-level entry points we need. */
    RES_INSTANCE(vkDestroyInstance);
    RES_INSTANCE(vkEnumeratePhysicalDevices);
    RES_INSTANCE(vkGetPhysicalDeviceProperties);
    RES_INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
    RES_INSTANCE(vkGetPhysicalDeviceMemoryProperties);
    RES_INSTANCE(vkCreateDevice);
    RES_INSTANCE(vkGetDeviceProcAddr);

    /* ---- pick a physical device with a COMPUTE queue ---------------- */
    uint32_t ndev = 0;
    r = pvkEnumeratePhysicalDevices(G.instance, &ndev, NULL);
    if (r != VK_SUCCESS || ndev == 0) {
        set_desc("no Vulkan physical device");
        return -1;
    }
    if (ndev > 8) ndev = 8;
    VkPhysicalDevice devs[8];
    r = pvkEnumeratePhysicalDevices(G.instance, &ndev, devs);
    if (r != VK_SUCCESS) { set_desc("enumerate physical devices failed"); return -1; }

    int found = 0;
    for (uint32_t d = 0; d < ndev && !found; d++) {
        uint32_t nqf = 0;
        pvkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nqf, NULL);
        if (nqf == 0) continue;
        if (nqf > 16) nqf = 16;
        VkQueueFamilyProperties qf[16];
        pvkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nqf, qf);
        for (uint32_t q = 0; q < nqf; q++) {
            if (qf[q].queueCount > 0 &&
                (qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                G.phys         = devs[d];
                G.queue_family = q;
                found = 1;
                break;
            }
        }
    }
    if (!found) { set_desc("no compute-capable queue family"); return -1; }

    VkPhysicalDeviceProperties props;
    pvkGetPhysicalDeviceProperties(G.phys, &props);
    pvkGetPhysicalDeviceMemoryProperties(G.phys, &G.memprops);
    /* Stash the device name for gpu_name() (the status/toggle UI). Bounded
     * copy; props.deviceName is a fixed NUL-terminated VK array. */
    {
        size_t i = 0;
        while (props.deviceName[i] && i + 1 < sizeof(G.name)) {
            G.name[i] = props.deviceName[i]; i++;
        }
        G.name[i] = '\0';
    }
    {
        char b[160];
        snprintf(b, sizeof(b), "GPU ready: %s (qf=%u)",
                 props.deviceName, G.queue_family);
        set_desc(b);
    }

    /* ---- logical device + compute queue ----------------------------- */
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = G.queue_family;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos    = &qci;
    r = pvkCreateDevice(G.phys, &dci, NULL, &G.device);
    if (r != VK_SUCCESS) { set_desc("vkCreateDevice failed"); return -1; }

    /* Resolve device-level entry points. */
    RES_DEVICE(vkDestroyDevice);
    RES_DEVICE(vkGetDeviceQueue);
    RES_DEVICE(vkCreateBuffer);
    RES_DEVICE(vkDestroyBuffer);
    RES_DEVICE(vkGetBufferMemoryRequirements);
    RES_DEVICE(vkAllocateMemory);
    RES_DEVICE(vkFreeMemory);
    RES_DEVICE(vkBindBufferMemory);
    RES_DEVICE(vkMapMemory);
    RES_DEVICE(vkUnmapMemory);
    RES_DEVICE(vkFlushMappedMemoryRanges);
    RES_DEVICE(vkInvalidateMappedMemoryRanges);
    RES_DEVICE(vkCreateShaderModule);
    RES_DEVICE(vkDestroyShaderModule);
    RES_DEVICE(vkCreateDescriptorSetLayout);
    RES_DEVICE(vkDestroyDescriptorSetLayout);
    RES_DEVICE(vkCreatePipelineLayout);
    RES_DEVICE(vkDestroyPipelineLayout);
    RES_DEVICE(vkCreateComputePipelines);
    RES_DEVICE(vkDestroyPipeline);
    RES_DEVICE(vkCreateDescriptorPool);
    RES_DEVICE(vkDestroyDescriptorPool);
    RES_DEVICE(vkAllocateDescriptorSets);
    RES_DEVICE(vkUpdateDescriptorSets);
    RES_DEVICE(vkCreateCommandPool);
    RES_DEVICE(vkDestroyCommandPool);
    RES_DEVICE(vkAllocateCommandBuffers);
    RES_DEVICE(vkFreeCommandBuffers);
    RES_DEVICE(vkBeginCommandBuffer);
    RES_DEVICE(vkEndCommandBuffer);
    RES_DEVICE(vkCmdBindPipeline);
    RES_DEVICE(vkCmdBindDescriptorSets);
    RES_DEVICE(vkCmdPushConstants);
    RES_DEVICE(vkCmdDispatch);
    RES_DEVICE(vkCmdPipelineBarrier);
    RES_DEVICE(vkQueueSubmit);
    RES_DEVICE(vkQueueWaitIdle);
    RES_DEVICE(vkCreateFence);
    RES_DEVICE(vkDestroyFence);
    RES_DEVICE(vkWaitForFences);
    RES_DEVICE(vkResetCommandPool);

    pvkGetDeviceQueue(G.device, G.queue_family, 0, &G.queue);
    if (!G.queue) { set_desc("vkGetDeviceQueue returned null"); return -1; }

    /* ---- shader module from embedded SPIR-V ------------------------- */
    VkShaderModuleCreateInfo smci;
    memset(&smci, 0, sizeof(smci));
    smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = (size_t)matmul_f32_spv_len;     /* in BYTES */
    smci.pCode    = matmul_f32_spv;                 /* uint32_t* */
    r = pvkCreateShaderModule(G.device, &smci, NULL, &G.shader);
    if (r != VK_SUCCESS) { set_desc("vkCreateShaderModule failed"); return -1; }

    /* ---- descriptor set layout: 3 storage buffers (A, x, y) --------- */
    VkDescriptorSetLayoutBinding binds[3];
    memset(binds, 0, sizeof(binds));
    for (int i = 0; i < 3; i++) {
        binds[i].binding         = (uint32_t)i;
        binds[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci;
    memset(&dslci, 0, sizeof(dslci));
    dslci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 3;
    dslci.pBindings    = binds;
    r = pvkCreateDescriptorSetLayout(G.device, &dslci, NULL, &G.dset_layout);
    if (r != VK_SUCCESS) { set_desc("descriptor set layout failed"); return -1; }

    /* ---- pipeline layout: dset + push constants (in, out) ----------- */
    VkPushConstantRange pcr;
    memset(&pcr, 0, sizeof(pcr));
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset     = 0;
    pcr.size       = 2 * sizeof(uint32_t);   /* nin, nout */

    VkPipelineLayoutCreateInfo plci;
    memset(&plci, 0, sizeof(plci));
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &G.dset_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcr;
    r = pvkCreatePipelineLayout(G.device, &plci, NULL, &G.pipe_layout);
    if (r != VK_SUCCESS) { set_desc("pipeline layout failed"); return -1; }

    /* ---- compute pipeline ------------------------------------------- */
    VkPipelineShaderStageCreateInfo stage;
    memset(&stage, 0, sizeof(stage));
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = G.shader;
    stage.pName  = "main";

    VkComputePipelineCreateInfo cpci;
    memset(&cpci, 0, sizeof(cpci));
    cpci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage  = stage;
    cpci.layout = G.pipe_layout;
    r = pvkCreateComputePipelines(G.device, VK_NULL_HANDLE, 1, &cpci,
                                  NULL, &G.pipeline);
    if (r != VK_SUCCESS) { set_desc("compute pipeline failed"); return -1; }

    /* ---- command pool ----------------------------------------------- */
    VkCommandPoolCreateInfo cpc;
    memset(&cpc, 0, sizeof(cpc));
    cpc.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpc.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpc.queueFamilyIndex = G.queue_family;
    r = pvkCreateCommandPool(G.device, &cpc, NULL, &G.cmd_pool);
    if (r != VK_SUCCESS) { set_desc("command pool failed"); return -1; }

    return 0;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */
int gpu_set_enabled(int enabled)
{
    int prev = G.enabled;
    G.enabled = enabled ? 1 : 0;
    return prev;
}

int gpu_get_enabled(void) { return G.enabled; }

int gpu_init(void)
{
    if (G.state == GPU_READY) return 0;
    if (G.state == GPU_DEAD)  return -1;   /* cached failure; don't thrash  */

    /* Mark DEAD first so any early-return path leaves a stable cached state;
     * flip to READY only on full success. */
    G.state = GPU_DEAD;

    if (load_vulkan_loader() != 0) { gpu_shutdown(); G.state = GPU_DEAD; return -1; }
    if (bringup() != 0)            { gpu_shutdown(); G.state = GPU_DEAD; return -1; }

    G.state = GPU_READY;
    GLOG("%s", G.desc);
    return 0;
}

int gpu_available(void)
{
    if (!G.enabled) return 0;          /* runtime toggle OFF -> CPU         */
    if (G.state == GPU_UNINIT) gpu_init();
    return (G.state == GPU_READY) ? 1 : 0;
}

void gpu_describe(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return;
    const char *s = G.desc[0] ? G.desc : "GPU not initialized";
    size_t i = 0;
    while (s[i] && i + 1 < buflen) { buf[i] = s[i]; i++; }
    buf[i] = '\0';
}

const char *gpu_name(void)
{
    /* Always-valid, NUL-terminated. Empty before a successful init / no GPU. */
    return G.name;
}

/* ---- one SSBO (host-visible) ------------------------------------------- */
typedef struct {
    VkBuffer       buf;
    VkDeviceMemory mem;
    void          *mapped;
    VkDeviceSize   size;
} ssbo_t;

static int ssbo_create(ssbo_t *s, VkDeviceSize size)
{
    memset(s, 0, sizeof(*s));
    if (size == 0) size = 4;          /* never request a 0-byte buffer       */
    s->size = size;

    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = size;
    bci.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (pvkCreateBuffer(G.device, &bci, NULL, &s->buf) != VK_SUCCESS) return -1;

    VkMemoryRequirements mr;
    pvkGetBufferMemoryRequirements(G.device, s->buf, &mr);

    /* Prefer coherent host-visible memory; fall back to plain host-visible.
     * We flush/invalidate unconditionally below, which is correct (a no-op
     * on coherent memory), so we don't need to track which kind we got. */
    uint32_t mt = find_mem_type(mr.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX)
        mt = find_mem_type(mr.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (mt == UINT32_MAX) return -1;

    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = mr.size;
    mai.memoryTypeIndex = mt;
    if (pvkAllocateMemory(G.device, &mai, NULL, &s->mem) != VK_SUCCESS) return -1;
    if (pvkBindBufferMemory(G.device, s->buf, s->mem, 0) != VK_SUCCESS) return -1;

    void *ptr = NULL;
    if (pvkMapMemory(G.device, s->mem, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS)
        return -1;
    s->mapped = ptr;
    return 0;
}

static void ssbo_destroy(ssbo_t *s)
{
    if (s->mem) {
        if (s->mapped) pvkUnmapMemory(G.device, s->mem);
        pvkFreeMemory(G.device, s->mem, NULL);
    }
    if (s->buf) pvkDestroyBuffer(G.device, s->buf, NULL);
    memset(s, 0, sizeof(*s));
}

static void mem_flush(VkDeviceMemory m)
{
    VkMappedMemoryRange rng;
    memset(&rng, 0, sizeof(rng));
    rng.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    rng.memory = m;
    rng.offset = 0;
    rng.size   = VK_WHOLE_SIZE;
    pvkFlushMappedMemoryRanges(G.device, 1, &rng);
}

static void mem_invalidate(VkDeviceMemory m)
{
    VkMappedMemoryRange rng;
    memset(&rng, 0, sizeof(rng));
    rng.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    rng.memory = m;
    rng.offset = 0;
    rng.size   = VK_WHOLE_SIZE;
    pvkInvalidateMappedMemoryRanges(G.device, 1, &rng);
}

/* ------------------------------------------------------------------------ */
/* Shared dispatch core: bind {bA, bX, bY}, push (in,out), dispatch, fence,  */
/* invalidate bY. Returns 0 on success. Used by BOTH the upload-each-call    */
/* path (gpu_matmul_f32) and the resident-weight path (gpu_matmul_resident); */
/* the ONLY difference between them is whether bA is rebuilt + reuploaded.   */
/* Assumes bX/bY are already filled + flushed by the caller.                 */
/* ------------------------------------------------------------------------ */
static int dispatch_matmul(const ssbo_t *bA, const ssbo_t *bX, ssbo_t *bY,
                           size_t in, size_t out)
{
    int rc = -1;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkCommandBuffer  cmd   = VK_NULL_HANDLE;
    VkFence          fence = VK_NULL_HANDLE;

    /* Descriptor pool + set. */
    VkDescriptorPoolSize ps;
    memset(&ps, 0, sizeof(ps));
    ps.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 3;
    VkDescriptorPoolCreateInfo dpci;
    memset(&dpci, 0, sizeof(dpci));
    dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets       = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = &ps;
    if (pvkCreateDescriptorPool(G.device, &dpci, NULL, &dpool) != VK_SUCCESS)
        goto done;

    VkDescriptorSetAllocateInfo dsai;
    memset(&dsai, 0, sizeof(dsai));
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = dpool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &G.dset_layout;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    if (pvkAllocateDescriptorSets(G.device, &dsai, &dset) != VK_SUCCESS)
        goto done;

    VkDescriptorBufferInfo bi[3];
    memset(bi, 0, sizeof(bi));
    bi[0].buffer = bA->buf; bi[0].range = bA->size;
    bi[1].buffer = bX->buf; bi[1].range = bX->size;
    bi[2].buffer = bY->buf; bi[2].range = bY->size;
    VkWriteDescriptorSet w[3];
    memset(w, 0, sizeof(w));
    for (int i = 0; i < 3; i++) {
        w[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet          = dset;
        w[i].dstBinding      = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo     = &bi[i];
    }
    pvkUpdateDescriptorSets(G.device, 3, w, 0, NULL);

    /* Command buffer. */
    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = G.cmd_pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (pvkAllocateCommandBuffers(G.device, &cbai, &cmd) != VK_SUCCESS)
        goto done;

    VkCommandBufferBeginInfo cbbi;
    memset(&cbbi, 0, sizeof(cbbi));
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (pvkBeginCommandBuffer(cmd, &cbbi) != VK_SUCCESS) goto done;

    pvkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.pipeline);
    pvkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                             G.pipe_layout, 0, 1, &dset, 0, NULL);
    uint32_t dims[2] = { (uint32_t)in, (uint32_t)out };
    pvkCmdPushConstants(cmd, G.pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                        0, sizeof(dims), dims);

    /* The shader handles GPU_ROWS_PER_WG output rows per workgroup (2D local
     * size COLS_PER_ROW x ROWS_PER_WG). Dispatch ceil(out / ROWS_PER_WG)
     * groups in X. */
    uint32_t groups = (uint32_t)((out + GPU_ROWS_PER_WG - 1) / GPU_ROWS_PER_WG);
    pvkCmdDispatch(cmd, groups, 1, 1);

    /* Barrier so the host read of y sees the shader writes. */
    VkMemoryBarrier mb;
    memset(&mb, 0, sizeof(mb));
    mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    pvkCmdPipelineBarrier(cmd,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_HOST_BIT,
                          0, 1, &mb, 0, NULL, 0, NULL);

    if (pvkEndCommandBuffer(cmd) != VK_SUCCESS) goto done;

    /* Submit + wait via a fence. */
    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (pvkCreateFence(G.device, &fci, NULL, &fence) != VK_SUCCESS) goto done;

    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    if (pvkQueueSubmit(G.queue, 1, &si, fence) != VK_SUCCESS) goto done;

    /* Wait up to ~10s; a hung GPU must not hang the app forever. */
    if (pvkWaitForFences(G.device, 1, &fence, VK_TRUE,
                         (uint64_t)10 * 1000 * 1000 * 1000) != VK_SUCCESS)
        goto done;

    mem_invalidate(bY->mem);
    rc = 0;

done:
    if (fence) pvkDestroyFence(G.device, fence, NULL);
    if (cmd)   pvkFreeCommandBuffers(G.device, G.cmd_pool, 1, &cmd);
    if (dpool) pvkDestroyDescriptorPool(G.device, dpool, NULL);
    return rc;
}

int gpu_matmul_f32(const float *A, size_t in, size_t out,
                   const float *x, float *y)
{
    /* Availability + enable gate. Either off => caller uses CPU. */
    if (!gpu_available())            return -1;
    if (!A || !x || !y)              return -1;
    if (in == 0 || out == 0)         return -1;
    /* Guard against absurd sizes overflowing VkDeviceSize math. */
    if (in > (size_t)1 << 28 || out > (size_t)1 << 28) return -1;

    int rc = -1;
    ssbo_t bA = {0}, bX = {0}, bY = {0};

    const VkDeviceSize szA = (VkDeviceSize)in * out * sizeof(float);
    const VkDeviceSize szX = (VkDeviceSize)in       * sizeof(float);
    const VkDeviceSize szY = (VkDeviceSize)out      * sizeof(float);

    if (ssbo_create(&bA, szA) != 0) goto done;
    if (ssbo_create(&bX, szX) != 0) goto done;
    if (ssbo_create(&bY, szY) != 0) goto done;

    /* Upload A and x (this path re-uploads A every call — the one-shot/cert
     * path; the resident path below avoids the A re-upload). */
    memcpy(bA.mapped, A, (size_t)szA);
    memcpy(bX.mapped, x, (size_t)szX);
    memset(bY.mapped, 0, (size_t)szY);
    mem_flush(bA.mem);
    mem_flush(bX.mem);
    mem_flush(bY.mem);

    if (dispatch_matmul(&bA, &bX, &bY, in, out) != 0) goto done;

    /* Read back y. */
    memcpy(y, bY.mapped, (size_t)szY);
    rc = 0;

done:
    ssbo_destroy(&bY);
    ssbo_destroy(&bX);
    ssbo_destroy(&bA);
    return rc;
}

/* ------------------------------------------------------------------------ */
/* Resident weights: A stays on the GPU across many matmuls (the inference   */
/* pattern — same weights, new x every token). bX/bY are kept resident too   */
/* so a steady-state call only memcpy's x in and y out, no per-call alloc.   */
/* ------------------------------------------------------------------------ */
struct gpu_weight {
    ssbo_t bA;       /* resident weight matrix (out x in)                    */
    ssbo_t bX;       /* resident input  vector (length in)                   */
    ssbo_t bY;       /* resident output vector (length out)                  */
    size_t in;
    size_t out;
};

gpu_weight_t gpu_upload_weight(const float *A, size_t in, size_t out)
{
    if (!gpu_available())    return NULL;
    if (!A)                  return NULL;
    if (in == 0 || out == 0) return NULL;
    if (in > (size_t)1 << 28 || out > (size_t)1 << 28) return NULL;

    struct gpu_weight *h =
        (struct gpu_weight *)malloc(sizeof(struct gpu_weight));
    if (!h) return NULL;
    memset(h, 0, sizeof(*h));
    h->in  = in;
    h->out = out;

    const VkDeviceSize szA = (VkDeviceSize)in * out * sizeof(float);
    const VkDeviceSize szX = (VkDeviceSize)in       * sizeof(float);
    const VkDeviceSize szY = (VkDeviceSize)out      * sizeof(float);

    if (ssbo_create(&h->bA, szA) != 0) goto fail;
    if (ssbo_create(&h->bX, szX) != 0) goto fail;
    if (ssbo_create(&h->bY, szY) != 0) goto fail;

    /* Upload A ONCE — the whole point of the resident path. */
    memcpy(h->bA.mapped, A, (size_t)szA);
    mem_flush(h->bA.mem);
    return h;

fail:
    ssbo_destroy(&h->bY);
    ssbo_destroy(&h->bX);
    ssbo_destroy(&h->bA);
    free(h);
    return NULL;
}

int gpu_matmul_resident(gpu_weight_t h, const float *x, float *y)
{
    if (!gpu_available()) return -1;
    if (!h || !x || !y)   return -1;
    /* If the device was torn down (gpu_shutdown) the buffers are stale; the
     * available() gate above already rules that out for a live handle. */
    if (!h->bA.buf || !h->bX.buf || !h->bY.buf) return -1;

    const size_t in  = h->in;
    const size_t out = h->out;
    const VkDeviceSize szX = (VkDeviceSize)in  * sizeof(float);
    const VkDeviceSize szY = (VkDeviceSize)out * sizeof(float);

    /* Upload only x; A is already resident. */
    memcpy(h->bX.mapped, x, (size_t)szX);
    memset(h->bY.mapped, 0, (size_t)szY);
    mem_flush(h->bX.mem);
    mem_flush(h->bY.mem);

    if (dispatch_matmul(&h->bA, &h->bX, &h->bY, in, out) != 0) return -1;

    memcpy(y, h->bY.mapped, (size_t)szY);
    return 0;
}

void gpu_free_weight(gpu_weight_t h)
{
    if (!h) return;
    /* Only touch device objects if the device is still alive. After
     * gpu_shutdown the device + fn-ptrs are gone; just free the host struct. */
    if (G.device && pvkDestroyBuffer) {
        ssbo_destroy(&h->bY);
        ssbo_destroy(&h->bX);
        ssbo_destroy(&h->bA);
    }
    free(h);
}

void gpu_shutdown(void)
{
    /* Only destroy what exists; resolved fn-ptrs are non-null iff we got far
     * enough to create the matching object. */
    if (G.device) {
        if (pvkQueueWaitIdle && G.queue) pvkQueueWaitIdle(G.queue);
        if (G.cmd_pool    && pvkDestroyCommandPool)
            pvkDestroyCommandPool(G.device, G.cmd_pool, NULL);
        if (G.pipeline    && pvkDestroyPipeline)
            pvkDestroyPipeline(G.device, G.pipeline, NULL);
        if (G.pipe_layout && pvkDestroyPipelineLayout)
            pvkDestroyPipelineLayout(G.device, G.pipe_layout, NULL);
        if (G.dset_layout && pvkDestroyDescriptorSetLayout)
            pvkDestroyDescriptorSetLayout(G.device, G.dset_layout, NULL);
        if (G.shader      && pvkDestroyShaderModule)
            pvkDestroyShaderModule(G.device, G.shader, NULL);
        if (pvkDestroyDevice) pvkDestroyDevice(G.device, NULL);
    }
    if (G.instance && pvkDestroyInstance)
        pvkDestroyInstance(G.instance, NULL);
    if (G.vk_lib) dlclose(G.vk_lib);

    /* Reset all handles/pointers; keep desc + enabled flag. */
    void *kept_desc_ok = NULL; (void)kept_desc_ok;
    int   kept_enabled = G.enabled;
    char  kept_desc[sizeof(G.desc)];
    memcpy(kept_desc, G.desc, sizeof(kept_desc));
    memset(&G, 0, sizeof(G));
    G.enabled = kept_enabled;
    memcpy(G.desc, kept_desc, sizeof(G.desc));
    /* state left UNINIT only if it was never DEAD/READY; callers that want the
     * cached-failure behavior set G.state = GPU_DEAD after calling us. */
}
