/*
 * GameCube GPU on the host's GPU (Vulkan): see gcn_vk.h.
 *
 * What the game draws is batched (texture uploads, then a render pass with the draws and clears
 * in order) and submitted every few thousand triangles without waiting, so the GPU draws while
 * the game goes on; a read of the frame buffer (the game's copies need the pixels at once, a
 * frame has a few of them) submits the rest and waits for everything. Vertices, states and
 * texture uploads go to host-visible buffers that only start over after such a wait; texture
 * slots and pipelines stay. New texture descriptors are written while earlier batches run
 * (descriptorBindingUpdateUnusedWhilePending); without that feature every batch is waited for.
 *
 * Textures get mipmaps (blitted down from the full-size texture after the upload) for the
 * Textures setting: the world's maps (mipmapped by the game, or linear-filtered power-of-two
 * ones, gcn_raster.c) are sampled trilinear or anisotropic then, the rest (nearest-filtered,
 * copies of the frame buffer) as before. The picture the game copies to the display can go through post-processing compute
 * passes (gcn_vk_present): SMAA 1x, FSR 1 EASU to the size it has on screen, FSR 1 RCAS
 * (Runtime/tools/gpu/gcn_post_*.comp and third_party/: MIT).
 */
#include "gcn_vk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(GCN_NO_VULKAN) && (defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))) && \
    !defined(GEKKO) && !defined(__3DS__) && defined(__has_include)
#if __has_include(<vulkan/vulkan.h>)
#define GCN_VK 1
#endif
#endif
#ifndef GCN_VK
#define GCN_VK 0
#endif

static char sStatus[256] = "not started";
const char *gcn_vk_status(void) { return sStatus; }

#if !GCN_VK

int gcn_vk_init(int efb_w, int efb_h)
{
    (void)efb_w;
    (void)efb_h;
    snprintf(sStatus, sizeof(sStatus), "Vulkan is not built into this platform");
    return 0;
}
int gcn_vk_active(void) { return 0; }
void gcn_vk_resize(int efb_w, int efb_h) { (void)efb_w; (void)efb_h; }
int gcn_vk_texture(const uint32_t *px, int w, int h) { (void)px; (void)w; (void)h; return -1; }
void gcn_vk_texture_release(int slot) { (void)slot; }
uint32_t gcn_vk_state(const GcnVkState *state) { (void)state; return 0; }
void gcn_vk_triangle(const GcnVkVertex v[3], const GcnVkPipe *pipe, const int scissor[4]) { (void)v; (void)pipe; (void)scissor; }
void gcn_vk_clear(int x0, int y0, int x1, int y1, uint32_t rgba, uint32_t z, int color, int depth)
{
    (void)x0; (void)y0; (void)x1; (void)y1; (void)rgba; (void)z; (void)color; (void)depth;
}
void gcn_vk_read(int x0, int y0, int x1, int y1, uint32_t *color, uint32_t *depth, int stride)
{
    (void)x0; (void)y0; (void)x1; (void)y1; (void)color; (void)depth; (void)stride;
}
void gcn_vk_sync(void) {}
void gcn_vk_reset(void) {}
uint32_t gcn_vk_xf_state(const GcnVkXf *xf) { (void)xf; return 0; }
GcnVkXfVertex *gcn_vk_xf_begin(int n, uint32_t *base) { (void)n; *base = 0; return NULL; }
void gcn_vk_xf_end(const uint32_t *idx, int count, const GcnVkPipe *pipe, const int scissor[4])
{
    (void)idx; (void)count; (void)pipe; (void)scissor;
}
void gcn_vk_read_shrunk(int x0, int y0, int x1, int y1, int shrink, uint32_t *color, uint32_t *depth, int stride)
{
    (void)x0; (void)y0; (void)x1; (void)y1; (void)shrink; (void)color; (void)depth; (void)stride;
}
void gcn_vk_read_into(int x0, int y0, int x1, int y1, uint32_t *dst, int dst_stride)
{
    (void)x0; (void)y0; (void)x1; (void)y1; (void)dst; (void)dst_stride;
}
int gcn_vk_post_supported(void) { return 0; }
int gcn_vk_present(int x0, int y0, int x1, int y1, int out_w, int out_h, const GcnVkPost *post, uint32_t *dst,
                   int dst_stride, int *w, int *h)
{
    (void)x0; (void)y0; (void)x1; (void)y1; (void)out_w; (void)out_h; (void)post; (void)dst; (void)dst_stride;
    (void)w; (void)h;
    return 0;
}
void gcn_vk_set_texture_filter(int level) { (void)level; }
int gcn_vk_texture_filter(void) { return 0; }
double gcn_vk_take_gpu_ms(uint32_t *batches) { if (batches) *batches = 0; return 0.0; }
void gcn_vk_print_stats(int frames) { (void)frames; }

#else

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "gcn_vk_spv.h"
#include "gcn_vk_post_tex.h" /* SMAA's lookup textures */
#include "gcn_gpu.h" /* GCN_EFB_W / H: the console's frame buffer, the size of shrunk reads */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <time.h>
#endif

/* ---- the Vulkan functions, loaded at run time ------------------------------------------------- */
#define VK_INSTANCE_FUNCS(X)                                                                                         \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                               \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define VK_DEVICE_FUNCS(X)                                                                                            \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkWaitForFences)                \
    X(vkResetFences) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory)        \
    X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkInvalidateMappedMemoryRanges) X(vkCreateImage)           \
    X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule)                      \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateDescriptorPool)   \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)               \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdSetViewport) X(vkCmdSetScissor)  \
    X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed)                                                                       \
    X(vkCmdSetBlendConstants) X(vkCmdDraw) X(vkCmdClearAttachments) X(vkCmdPipelineBarrier)                           \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) X(vkCmdClearColorImage)                     \
    X(vkCmdClearDepthStencilImage) X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp)          \
    X(vkGetQueryPoolResults) X(vkCreateSampler) X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkCmdPushConstants)  \
    X(vkCmdCopyImage)

#define DECLARE(f) static PFN_##f f;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr_;
static PFN_vkCreateInstance vkCreateInstance_;
static PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion_;
VK_INSTANCE_FUNCS(DECLARE)
VK_DEVICE_FUNCS(DECLARE)
#undef DECLARE

/* ---- state ---------------------------------------------------------------------------------- */
#define MAX_SLOTS 4096
#define VTX_BYTES (64u << 20)
#define STATE_BYTES (16u << 20)
#define XF_VTX_BYTES (64u << 20)
#define XF_STATE_BYTES (32u << 20)
#define XF_IDX_BYTES (32u << 20)
#define STAGE_BYTES (64u << 20)
#define MAX_PIPES 256

typedef struct
{
    VkBuffer buf;
    VkDeviceMemory mem;
    uint8_t *map;
    VkDeviceSize size;
} Buffer;

typedef struct
{
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    int w, h, levels;
} Image;

typedef struct
{
    int type; /* 0 draw, 1 clear */
    int xf;   /* draw: indices into sXfVtx (transformed on the GPU; first / count of sXfIdx), else of sVtx */
    uint32_t first, count, key;
    int sc[4];
    float bc;
    /* clear */
    int rect[4];
    float col[4], depth;
    int bits;
} Op;

typedef struct
{
    int slot;
    VkDeviceSize offset;
    int w, h;
} Upload;

static int sActive, sFailed;
static VkInstance sInst;
static VkPhysicalDevice sPhys;
static VkDevice sDev;
static VkQueue sQueue;
static uint32_t sQueueFamily;
static VkPhysicalDeviceMemoryProperties sMemProps;
static VkCommandPool sPool;
#define NCMD 8
static VkCommandBuffer sCmds[NCMD], sCmd; /* sCmd: the one being recorded */
static VkFence sFences[NCMD];
static int sPending[NCMD], sCur;          /* submitted and not waited for; the next one to record */
static int sAsync;                        /* batches run while the game goes on (see above) */
static uint32_t sKickVtx;                 /* sVtxCount at the last submit */
#define KICK_VERTS (3 * 2048)
static int sDepthClamp, sDepthBlit;
/* GPU time per command buffer (start / end timestamps), for GCN_RASTER_STATS */
static VkQueryPool sQueries;
static float sTsPeriod;   /* ns per tick, 0 = no timestamps */
static uint32_t sTsMask;
static double sGpuExecMs;

static Buffer sVtx, sStates, sStage, sRead, sXfVtx, sXfStates, sXfIdx;
static uint32_t sVtxCount, sStateCount, sXfVtxCount, sXfStateCount, sKickXf, sXfIdxCount;
static VkDeviceSize sStageUsed;

static Image sColor, sDepth;
static Image sNatColor, sNatDepth; /* shrunk reads: the console's resolution */
static int sEfbW, sEfbH;

/* a read of the frame buffer: the rectangle (pixels, x1 / y1 exclusive), `shrink` x smaller
 * (color filtered, depth the nearest sample), into color / depth at (x0, y0) / shrink with rows
 * `stride` apart, or (at_origin) with the rectangle's top left at color[0] */
typedef struct
{
    int x0, y0, x1, y1, shrink, at_origin, stride;
    uint32_t *color, *depth;
} Read;
static VkRenderPass sPass;
static VkFramebuffer sFb;

static VkDescriptorSetLayout sSetLayout;
static VkPipelineLayout sPipeLayout;
static VkDescriptorPool sDescPool;
static VkDescriptorSet sSet;
static VkShaderModule sVert, sFrag, sVertXf;
/* wrap s (clamp, repeat, mirror) + wrap t * 3 + linear * 9; then the mipmapped linear ones for the
 * Textures setting: 18 + (filter - 1) * 9 + wrap s + wrap t * 3, filter 1 trilinear, 2..5
 * anisotropic 2x..16x (gcn_vk.frag) */
#define NSAMPLERS 63
static VkSampler sSamplers[NSAMPLERS];
static float sMaxAniso = 1.0f; /* 1: no anisotropic filtering */
static int sMipOK;             /* RGBA8 blits (mipmaps are made on the GPU) */
static volatile int sTexFilter;
static struct
{
    uint32_t key;
    VkPipeline pipe;
} sPipes[MAX_PIPES];
static int sPipeCount;

static Image sTex[MAX_SLOTS];
static int sFree[MAX_SLOTS], sFreeCount;
static int sReleased[MAX_SLOTS], sReleasedCount;

static Op *sOps;
static int sOpCount, sOpCap;
static Upload *sUploads;
static int sUploadCount, sUploadCap;

static double sGpuMs;
static uint32_t sBatches;
/* GCN_RASTER_STATS: what the batches since gcn_vk_print_stats held */
static struct
{
    uint32_t reads, triangles, states, uploads, draws, clears, xf_states;
    double read_ms, wait_ms;
    /* GCN_VK_READ_LOG=1: every read (what, how big, the wait) */
    uint64_t read_bytes, upload_bytes;
} sSt;

static void fail(const char *what, VkResult r)
{
    snprintf(sStatus, sizeof(sStatus), "%s failed (VkResult %d)", what, (int)r);
    fprintf(stderr, "gcn vk: %s\n", sStatus);
}

#define CHECK(call, what)                     \
    do                                        \
    {                                         \
        VkResult r_ = (call);                 \
        if (r_ != VK_SUCCESS)                 \
        {                                     \
            fail(what, r_);                   \
            return 0;                         \
        }                                     \
    } while (0)

static double now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

/* ---- loading ----------------------------------------------------------------------------------- */
static int load_library(void)
{
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) return 0;
    vkGetInstanceProcAddr_ = (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(lib, "vkGetInstanceProcAddr");
#else
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return 0;
    vkGetInstanceProcAddr_ = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#endif
    if (!vkGetInstanceProcAddr_) return 0;
    vkCreateInstance_ = (PFN_vkCreateInstance)vkGetInstanceProcAddr_(NULL, "vkCreateInstance");
    vkEnumerateInstanceVersion_ = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr_(NULL, "vkEnumerateInstanceVersion");
    return vkCreateInstance_ != NULL;
}

static int load_instance(void)
{
#define LOAD(f)                                                   \
    f = (PFN_##f)vkGetInstanceProcAddr_(sInst, #f);               \
    if (!f) { snprintf(sStatus, sizeof(sStatus), "no %s", #f); return 0; }
    VK_INSTANCE_FUNCS(LOAD)
#undef LOAD
    return 1;
}

static int load_device(void)
{
#define LOAD(f)                                                   \
    f = (PFN_##f)vkGetDeviceProcAddr(sDev, #f);                   \
    if (!f) { snprintf(sStatus, sizeof(sStatus), "no %s", #f); return 0; }
    VK_DEVICE_FUNCS(LOAD)
#undef LOAD
    return 1;
}

/* ---- memory ----------------------------------------------------------------------------------- */
static int mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;
    for (i = 0; i < sMemProps.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (sMemProps.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

static int make_buffer(Buffer *b, VkDeviceSize size, VkBufferUsageFlags usage, int readback)
{
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    int type = -1;

    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CHECK(vkCreateBuffer(sDev, &bi, NULL, &b->buf), "vkCreateBuffer");
    vkGetBufferMemoryRequirements(sDev, b->buf, &req);
    if (readback)
        type = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type < 0) type = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0)
    {
        snprintf(sStatus, sizeof(sStatus), "no host-visible memory");
        return 0;
    }
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    CHECK(vkAllocateMemory(sDev, &ai, NULL, &b->mem), "vkAllocateMemory (buffer)");
    CHECK(vkBindBufferMemory(sDev, b->buf, b->mem, 0), "vkBindBufferMemory");
    CHECK(vkMapMemory(sDev, b->mem, 0, VK_WHOLE_SIZE, 0, (void **)&b->map), "vkMapMemory");
    b->size = size;
    return 1;
}

static void free_buffer(Buffer *b)
{
    if (b->buf) vkDestroyBuffer(sDev, b->buf, NULL);
    if (b->mem) vkFreeMemory(sDev, b->mem, NULL);
    memset(b, 0, sizeof(*b));
}

static int make_image_levels(Image *im, int w, int h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                             int levels)
{
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    int type;

    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = (uint32_t)w;
    ii.extent.height = (uint32_t)h;
    ii.extent.depth = 1;
    ii.mipLevels = (uint32_t)levels;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(sDev, &ii, NULL, &im->image), "vkCreateImage");
    vkGetImageMemoryRequirements(sDev, im->image, &req);
    type = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = mem_type(req.memoryTypeBits, 0);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    CHECK(vkAllocateMemory(sDev, &ai, NULL, &im->mem), "vkAllocateMemory (image)");
    CHECK(vkBindImageMemory(sDev, im->image, im->mem, 0), "vkBindImageMemory");
    vi.image = im->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = (uint32_t)levels;
    vi.subresourceRange.layerCount = 1;
    CHECK(vkCreateImageView(sDev, &vi, NULL, &im->view), "vkCreateImageView");
    im->w = w;
    im->h = h;
    im->levels = levels;
    return 1;
}

static int make_image(Image *im, int w, int h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    return make_image_levels(im, w, h, fmt, usage, aspect, 1);
}

/* levels down to 1x1 */
static int mip_count(int w, int h)
{
    int n = 1, m = w > h ? w : h;
    while (m > 1)
    {
        m >>= 1;
        n++;
    }
    return n;
}

static void free_image(Image *im)
{
    if (im->view) vkDestroyImageView(sDev, im->view, NULL);
    if (im->image) vkDestroyImage(sDev, im->image, NULL);
    if (im->mem) vkFreeMemory(sDev, im->mem, NULL);
    memset(im, 0, sizeof(*im));
}

static void barrier_levels(VkImage image, VkImageAspectFlags aspect, uint32_t base, uint32_t count, VkImageLayout from,
                           VkImageLayout to)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.baseMipLevel = base;
    b.subresourceRange.levelCount = count;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(sCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1,
                         &b);
}

/* every level of the image */
static void barrier(VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to)
{
    barrier_levels(image, aspect, 0, VK_REMAINING_MIP_LEVELS, from, to);
}

/* an uploaded texture's mipmaps (level 0 in TRANSFER_DST, the rest UNDEFINED / TRANSFER_DST), each
 * level the one above halved (linear); every level SHADER_READ_ONLY after */
static void gen_mips(const Image *im)
{
    int l, w = im->w, h = im->h;
    for (l = 1; l < im->levels; l++)
    {
        VkImageBlit b;
        const int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        memset(&b, 0, sizeof(b));
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.mipLevel = (uint32_t)(l - 1);
        b.srcSubresource.layerCount = 1;
        b.srcOffsets[1].x = w;
        b.srcOffsets[1].y = h;
        b.srcOffsets[1].z = 1;
        b.dstSubresource = b.srcSubresource;
        b.dstSubresource.mipLevel = (uint32_t)l;
        b.dstOffsets[1].x = nw;
        b.dstOffsets[1].y = nh;
        b.dstOffsets[1].z = 1;
        barrier_levels(im->image, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)(l - 1), 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdBlitImage(sCmd, im->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &b, VK_FILTER_LINEAR);
        w = nw;
        h = nh;
    }
    barrier_levels(im->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)(im->levels - 1), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier_levels(im->image, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)(im->levels - 1), 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static int wait_slot(int i)
{
    double t0 = now_ms();
    VkResult r = vkWaitForFences(sDev, 1, &sFences[i], VK_TRUE, UINT64_MAX);
    if (r != VK_SUCCESS)
    {
        fail("vkWaitForFences (device lost?)", r);
        sActive = 0;
        sFailed = 1;
        return 0;
    }
    vkResetFences(sDev, 1, &sFences[i]);
    sPending[i] = 0;
    if (sTsPeriod > 0.0f)
    {
        uint64_t ts[2];
        if (vkGetQueryPoolResults(sDev, sQueries, (uint32_t)(2 * i), 2, sizeof(ts), ts, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            sGpuExecMs += (double)((ts[1] - ts[0]) & sTsMask) * sTsPeriod / 1e6;
    }
    sGpuMs += now_ms() - t0;
    sSt.wait_ms += now_ms() - t0;
    return 1;
}

/* waits for every submitted batch */
static int wait_all(void)
{
    int i, k;
    for (k = 0; k < NCMD; k++)
    {
        i = (sCur + k) % NCMD; /* oldest first */
        if (sPending[i] && !wait_slot(i)) return 0;
    }
    return 1;
}

/* starts recording into the next command buffer (waiting for its previous use) */
static int begin_cmd(void)
{
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (sPending[sCur] && !wait_slot(sCur)) return 0;
    sCmd = sCmds[sCur];
    CHECK(vkResetCommandBuffer(sCmd, 0), "vkResetCommandBuffer");
    CHECK(vkBeginCommandBuffer(sCmd, &bi), "vkBeginCommandBuffer");
    if (sTsPeriod > 0.0f)
    {
        vkCmdResetQueryPool(sCmd, sQueries, (uint32_t)(2 * sCur), 2);
        vkCmdWriteTimestamp(sCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, sQueries, (uint32_t)(2 * sCur));
    }
    return 1;
}

/* submits what was recorded; wait: and everything before it, then returns */
static int submit(int wait)
{
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};

    if (sTsPeriod > 0.0f)
        vkCmdWriteTimestamp(sCmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sQueries, (uint32_t)(2 * sCur + 1));
    CHECK(vkEndCommandBuffer(sCmd), "vkEndCommandBuffer");
    si.commandBufferCount = 1;
    si.pCommandBuffers = &sCmd;
    CHECK(vkQueueSubmit(sQueue, 1, &si, sFences[sCur]), "vkQueueSubmit");
    sPending[sCur] = 1;
    sCur = (sCur + 1) % NCMD;
    sBatches++;
    return wait ? wait_all() : 1;
}

/* ---- the frame buffer ------------------------------------------------------------------------- */
static int make_efb(int w, int h)
{
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    VkImageView views[2];

    if (!make_image(&sColor, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    if (!make_image(&sDepth, w, h, VK_FORMAT_D32_SFLOAT,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT))
        return 0;
    views[0] = sColor.view;
    views[1] = sDepth.view;
    fi.renderPass = sPass;
    fi.attachmentCount = 2;
    fi.pAttachments = views;
    fi.width = (uint32_t)w;
    fi.height = (uint32_t)h;
    fi.layers = 1;
    CHECK(vkCreateFramebuffer(sDev, &fi, NULL, &sFb), "vkCreateFramebuffer");
    sEfbW = w;
    sEfbH = h;
    /* the readback buffer: a whole frame buffer of color and of depth */
    free_buffer(&sRead);
    if (!make_buffer(&sRead, (VkDeviceSize)w * h * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1)) return 0;
    return 1;
}

/* new frame buffer images (UNDEFINED) to their attachment layouts, cleared */
static void init_efb_contents(void)
{
    VkClearColorValue c;
    VkClearDepthStencilValue d;
    VkImageSubresourceRange rc = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, rd = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    memset(&c, 0, sizeof(c));
    d.depth = 0.0f;
    d.stencil = 0;
    barrier(sColor.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    barrier(sDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdClearColorImage(sCmd, sColor.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rc);
    vkCmdClearDepthStencilImage(sCmd, sDepth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &d, 1, &rd);
}

static void efb_to_attachment(VkImageLayout colorFrom, VkImageLayout depthFrom)
{
    barrier(sColor.image, VK_IMAGE_ASPECT_COLOR_BIT, colorFrom, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barrier(sDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, depthFrom, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

/* ---- pipelines -------------------------------------------------------------------------------- */
static uint32_t pipe_key(const GcnVkPipe *p)
{
    uint32_t k = 0;
    if (p->blend) k |= 1u | (uint32_t)(p->sf & 7) << 1 | (uint32_t)(p->df & 7) << 4 | (uint32_t)(p->sub & 1) << 7;
    k |= (uint32_t)(p->color_upd & 1) << 8 | (uint32_t)(p->alpha_upd & 1) << 9 | (uint32_t)(p->dst_alpha_on & 1) << 10;
    k |= (uint32_t)(p->no_alpha & 1) << 11 | (uint32_t)(p->ztest & 1) << 12 | (uint32_t)(p->zfunc & 7) << 13;
    k |= (uint32_t)(p->zupd & 1) << 16;
    return k;
}

static uint32_t pipe_key_xf(const GcnVkPipe *p)
{
    return pipe_key(p) | 1u << 17 | (uint32_t)(p->cull & 3) << 18 | (uint32_t)(p->clip_off & 1) << 20;
}

static VkBlendFactor src_factor(int f, int no_alpha)
{
    switch (f)
    {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 2: return VK_BLEND_FACTOR_DST_COLOR;
    case 3: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 4: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 6: return no_alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_DST_ALPHA;
    default: return no_alpha ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
}

static VkBlendFactor dst_factor(int f, int no_alpha)
{
    switch (f)
    {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 2: return VK_BLEND_FACTOR_SRC_COLOR;
    case 3: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 6: return no_alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_DST_ALPHA;
    default: return no_alpha ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
}

static const VkCompareOp kCompare[8] = {VK_COMPARE_OP_NEVER,   VK_COMPARE_OP_LESS,      VK_COMPARE_OP_EQUAL,
                                        VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
                                        VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};

static VkPipeline make_pipeline(uint32_t key)
{
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkVertexInputBindingDescription bind = {0, sizeof(GcnVkVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr[12];
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkPipeline pipe = VK_NULL_HANDLE;
    int blend = key & 1, sf = (key >> 1) & 7, df = (key >> 4) & 7, sub = (key >> 7) & 1;
    int cupd = (key >> 8) & 1, aupd = (key >> 9) & 1, dsta = (key >> 10) & 1, noa = (key >> 11) & 1;
    int ztest = (key >> 12) & 1, zfunc = (key >> 13) & 7, zupd = (key >> 16) & 1;
    int xf = (key >> 17) & 1, cull = (key >> 18) & 3, clipoff = (key >> 20) & 1;
    int i;
    VkResult r;

    memset(stages, 0, sizeof(stages));
    for (i = 0; i < 2; i++)
    {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = xf ? sVertXf : sVert;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = sFrag;

    attr[0].location = 0; attr[0].binding = 0; attr[0].format = VK_FORMAT_R32G32B32A32_SFLOAT; attr[0].offset = 0;
    attr[1].location = 1; attr[1].binding = 0; attr[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; attr[1].offset = 16;
    attr[2].location = 2; attr[2].binding = 0; attr[2].format = VK_FORMAT_R32G32B32A32_SFLOAT; attr[2].offset = 32;
    for (i = 0; i < 8; i++)
    {
        attr[3 + i].location = (uint32_t)(3 + i);
        attr[3 + i].binding = 0;
        attr[3 + i].format = VK_FORMAT_R32G32B32_SFLOAT;
        attr[3 + i].offset = (uint32_t)(48 + 12 * i);
    }
    attr[11].location = 11; attr[11].binding = 0; attr[11].format = VK_FORMAT_R32_UINT; attr[11].offset = 144;
    if (!xf) /* the XF shader fetches its vertices (gl_VertexIndex) */
    {
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &bind;
        vin.vertexAttributeDescriptionCount = 12;
        vin.pVertexAttributeDescriptions = attr;
    }
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    rs.depthClampEnable = sDepthClamp ? VK_TRUE : VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; /* the CPU culls */
    if (xf)
    {
        /* the GameCube's front faces are clockwise on screen (y down): Vulkan's clockwise */
        static const VkCullModeFlags kCull[4] = {VK_CULL_MODE_NONE, VK_CULL_MODE_BACK_BIT, VK_CULL_MODE_FRONT_BIT,
                                                 VK_CULL_MODE_FRONT_AND_BACK};
        rs.cullMode = kCull[cull];
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        /* near / far clipping as the console's; off (XF 0x1005): clamped */
        rs.depthClampEnable = (clipoff && sDepthClamp) ? VK_TRUE : VK_FALSE;
    }
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    /* the software rasteriser writes depth when the update is on, also with the test off */
    ds.depthTestEnable = (ztest || zupd) ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = zupd ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = ztest ? kCompare[zfunc] : VK_COMPARE_OP_ALWAYS;
    memset(&ba, 0, sizeof(ba));
    ba.blendEnable = VK_TRUE;
    ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    if (blend && sub)
    {
        /* destination - source on the color; alpha is the source's */
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    }
    else if (blend)
    {
        ba.srcColorBlendFactor = src_factor(sf, noa);
        ba.dstColorBlendFactor = dst_factor(df, noa);
        ba.srcAlphaBlendFactor = src_factor(sf, noa);
        ba.dstAlphaBlendFactor = dst_factor(df, noa);
    }
    else
    {
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    }
    if (dsta)
    {
        /* the constant destination alpha (the blend constant) replaces the alpha written */
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    ba.colorWriteMask = (cupd ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT) : 0) |
                        ((aupd || dsta) ? VK_COLOR_COMPONENT_A_BIT : 0);
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    dy.dynamicStateCount = 3;
    dy.pDynamicStates = dyn;

    gi.stageCount = 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vin;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = sPipeLayout;
    gi.renderPass = sPass;
    r = vkCreateGraphicsPipelines(sDev, VK_NULL_HANDLE, 1, &gi, NULL, &pipe);
    if (r != VK_SUCCESS)
    {
        fail("vkCreateGraphicsPipelines", r);
        return VK_NULL_HANDLE;
    }
    return pipe;
}

static VkPipeline pipeline(uint32_t key)
{
    int i;
    for (i = 0; i < sPipeCount; i++)
        if (sPipes[i].key == key) return sPipes[i].pipe;
    if (sPipeCount == MAX_PIPES) return sPipes[0].pipe; /* never in practice: 2^17 keys, few used */
    sPipes[sPipeCount].key = key;
    sPipes[sPipeCount].pipe = make_pipeline(key);
    return sPipes[sPipeCount++].pipe;
}

/* ---- setup ------------------------------------------------------------------------------------ */
static int make_device(void)
{
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkPhysicalDevice devs[16];
    uint32_t ndev = 16, ver = VK_API_VERSION_1_0, i, best = 0xFFFFFFFFu;
    int bestScore = -1;
    const char *pick = getenv("GCN_VK_DEVICE");
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features want12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 want2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPhysicalDeviceProperties props;
    VkQueueFamilyProperties qf[32];
    uint32_t nqf = 32;
    float prio = 1.0f;

    if (vkEnumerateInstanceVersion_) vkEnumerateInstanceVersion_(&ver);
    if (ver < VK_API_VERSION_1_2)
    {
        snprintf(sStatus, sizeof(sStatus), "Vulkan 1.2 is needed (the loader has %u.%u)", VK_VERSION_MAJOR(ver),
                 VK_VERSION_MINOR(ver));
        return 0;
    }
    app.pApplicationName = "com.recomp.gcn";
    app.pEngineName = "com.recomp.gcn";
    app.apiVersion = VK_API_VERSION_1_2;
    ici.pApplicationInfo = &app;
    CHECK(vkCreateInstance_(&ici, NULL, &sInst), "vkCreateInstance");
    if (!load_instance()) return 0;
    CHECK(vkEnumeratePhysicalDevices(sInst, &ndev, devs), "vkEnumeratePhysicalDevices");
    for (i = 0; i < ndev; i++)
    {
        int score;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        f2.pNext = &f12;
        vkGetPhysicalDeviceFeatures2(devs[i], &f2);
        if (props.apiVersion < VK_API_VERSION_1_2 || !f12.shaderSampledImageArrayNonUniformIndexing ||
            !f12.descriptorBindingPartiallyBound)
            continue;
        score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
              : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (pick && (uint32_t)atoi(pick) == i) score = 100;
        if (score > bestScore)
        {
            bestScore = score;
            best = i;
        }
    }
    if (best == 0xFFFFFFFFu)
    {
        snprintf(sStatus, sizeof(sStatus), "no GPU with Vulkan 1.2 and descriptor indexing");
        return 0;
    }
    sPhys = devs[best];
    vkGetPhysicalDeviceProperties(sPhys, &props);
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(sPhys, &f2);
    vkGetPhysicalDeviceMemoryProperties(sPhys, &sMemProps);
    vkGetPhysicalDeviceQueueFamilyProperties(sPhys, &nqf, qf);
    for (i = 0; i < nqf; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    if (i == nqf)
    {
        snprintf(sStatus, sizeof(sStatus), "no graphics queue");
        return 0;
    }
    sQueueFamily = i;
    qi.queueFamilyIndex = sQueueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    sTsPeriod = props.limits.timestampComputeAndGraphics ? props.limits.timestampPeriod : 0.0f;
    sTsMask = qf[sQueueFamily].timestampValidBits >= 32 ? 0xFFFFFFFFu : (1u << qf[sQueueFamily].timestampValidBits) - 1u;
    want12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    want12.descriptorBindingPartiallyBound = VK_TRUE;
    /* GCN_VK_SYNC=1: wait for every batch (debugging) */
    sAsync = f12.descriptorBindingUpdateUnusedWhilePending && !getenv("GCN_VK_SYNC");
    want12.descriptorBindingUpdateUnusedWhilePending = sAsync ? VK_TRUE : VK_FALSE;
    want2.pNext = &want12;
    sDepthClamp = f2.features.depthClamp != 0;
    want2.features.depthClamp = f2.features.depthClamp;
    /* the Textures setting: anisotropic filtering when the GPU has it (else trilinear at most) */
    want2.features.samplerAnisotropy = f2.features.samplerAnisotropy;
    sMaxAniso = f2.features.samplerAnisotropy ? props.limits.maxSamplerAnisotropy : 1.0f;
    dci.pNext = &want2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    CHECK(vkCreateDevice(sPhys, &dci, NULL, &sDev), "vkCreateDevice");
    if (!load_device()) return 0;
    vkGetDeviceQueue(sDev, sQueueFamily, 0, &sQueue);
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(sPhys, VK_FORMAT_D32_SFLOAT, &fp);
        sDepthBlit = (fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) ==
                     (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT);
        vkGetPhysicalDeviceFormatProperties(sPhys, VK_FORMAT_R8G8B8A8_UNORM, &fp);
        sMipOK = (fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) ==
                 (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    }
    snprintf(sStatus, sizeof(sStatus), "%s", props.deviceName);
    return 1;
}

static int make_objects(void)
{
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkAttachmentDescription att[2];
    VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub;
    VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    VkDescriptorSetLayoutBinding binds[5];
    VkDescriptorBindingFlags bflags[5] = {0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0, 0, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
                                     {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_SLOTS},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, NSAMPLERS}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkDescriptorBufferInfo sbi, xbi[2];
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, xwr[2];
    int i;

    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = sQueueFamily;
    CHECK(vkCreateCommandPool(sDev, &pci, NULL, &sPool), "vkCreateCommandPool");
    cai.commandPool = sPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = NCMD;
    CHECK(vkAllocateCommandBuffers(sDev, &cai, sCmds), "vkAllocateCommandBuffers");
    for (i = 0; i < NCMD; i++) CHECK(vkCreateFence(sDev, &fci, NULL, &sFences[i]), "vkCreateFence");
    if (sTsPeriod > 0.0f)
    {
        VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 2 * NCMD;
        if (vkCreateQueryPool(sDev, &qpi, NULL, &sQueries) != VK_SUCCESS) sTsPeriod = 0.0f;
    }

    if (!make_buffer(&sVtx, VTX_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0)) return 0;
    if (!make_buffer(&sStates, STATE_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0)) return 0;
    if (!make_buffer(&sStage, STAGE_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0)) return 0;
    if (!make_buffer(&sXfVtx, XF_VTX_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0)) return 0;
    if (!make_buffer(&sXfStates, XF_STATE_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0)) return 0;
    if (!make_buffer(&sXfIdx, XF_IDX_BYTES, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 0)) return 0;

    memset(att, 0, sizeof(att));
    att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    att[1].format = VK_FORMAT_D32_SFLOAT;
    for (i = 0; i < 2; i++)
    {
        att[i].samples = VK_SAMPLE_COUNT_1_BIT;
        att[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    att[0].initialLayout = att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[1].initialLayout = att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    memset(&sub, 0, sizeof(sub));
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    rpi.attachmentCount = 2;
    rpi.pAttachments = att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    CHECK(vkCreateRenderPass(sDev, &rpi, NULL, &sPass), "vkCreateRenderPass");

    for (i = 0; i < NSAMPLERS; i++)
    {
        static const VkSamplerAddressMode kWrap[3] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                                      VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
        VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        /* the game's own: level 0 only; the Textures setting's: every level, trilinear, anisotropic */
        const int wrap = i < 18 ? i % 9 : (i - 18) % 9, filter = i < 18 ? 0 : 1 + (i - 18) / 9;
        const int linear = i < 18 ? i / 9 : 1;
        sci.magFilter = sci.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.mipmapMode = filter ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = kWrap[wrap % 3];
        sci.addressModeV = kWrap[wrap / 3];
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = filter ? VK_LOD_CLAMP_NONE : 0.0f;
        if (filter >= 2 && sMaxAniso > 1.0f)
        {
            const float want = (float)(1 << (filter - 1));
            sci.anisotropyEnable = VK_TRUE;
            sci.maxAnisotropy = want < sMaxAniso ? want : sMaxAniso;
        }
        CHECK(vkCreateSampler(sDev, &sci, NULL, &sSamplers[i]), "vkCreateSampler");
    }
    memset(binds, 0, sizeof(binds));
    binds[0].binding = 0;
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[1].binding = 1;
    binds[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binds[1].descriptorCount = MAX_SLOTS;
    binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[2].binding = 2;
    binds[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    binds[2].descriptorCount = NSAMPLERS;
    binds[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[2].pImmutableSamplers = sSamplers;
    for (i = 3; i < 5; i++)
    {
        binds[i].binding = (uint32_t)i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    }
    if (sAsync) bflags[1] |= VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    bfi.bindingCount = 5;
    bfi.pBindingFlags = bflags;
    dli.pNext = &bfi;
    dli.bindingCount = 5;
    dli.pBindings = binds;
    CHECK(vkCreateDescriptorSetLayout(sDev, &dli, NULL, &sSetLayout), "vkCreateDescriptorSetLayout");
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &sSetLayout;
    CHECK(vkCreatePipelineLayout(sDev, &pli, NULL, &sPipeLayout), "vkCreatePipelineLayout");
    dpi.maxSets = 1;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = sizes;
    CHECK(vkCreateDescriptorPool(sDev, &dpi, NULL, &sDescPool), "vkCreateDescriptorPool");
    dai.descriptorPool = sDescPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &sSetLayout;
    CHECK(vkAllocateDescriptorSets(sDev, &dai, &sSet), "vkAllocateDescriptorSets");
    sbi.buffer = sStates.buf;
    sbi.offset = 0;
    sbi.range = VK_WHOLE_SIZE;
    wr.dstSet = sSet;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr.pBufferInfo = &sbi;
    vkUpdateDescriptorSets(sDev, 1, &wr, 0, NULL);
    for (i = 0; i < 2; i++)
    {
        xbi[i].buffer = i == 0 ? sXfVtx.buf : sXfStates.buf;
        xbi[i].offset = 0;
        xbi[i].range = VK_WHOLE_SIZE;
        memset(&xwr[i], 0, sizeof(xwr[i]));
        xwr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        xwr[i].dstSet = sSet;
        xwr[i].dstBinding = (uint32_t)(3 + i);
        xwr[i].descriptorCount = 1;
        xwr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        xwr[i].pBufferInfo = &xbi[i];
    }
    vkUpdateDescriptorSets(sDev, 2, xwr, 0, NULL);

    smi.codeSize = sizeof(gcn_vk_vert_spv);
    smi.pCode = gcn_vk_vert_spv;
    CHECK(vkCreateShaderModule(sDev, &smi, NULL, &sVert), "vkCreateShaderModule (vertex)");
    smi.codeSize = sizeof(gcn_vk_xf_vert_spv);
    smi.pCode = gcn_vk_xf_vert_spv;
    CHECK(vkCreateShaderModule(sDev, &smi, NULL, &sVertXf), "vkCreateShaderModule (XF vertex)");
    smi.codeSize = sizeof(gcn_vk_frag_spv);
    smi.pCode = gcn_vk_frag_spv;
    CHECK(vkCreateShaderModule(sDev, &smi, NULL, &sFrag), "vkCreateShaderModule (fragment)");

    for (i = MAX_SLOTS - 1; i >= 0; i--) sFree[sFreeCount++] = i;
    return 1;
}

int gcn_vk_init(int efb_w, int efb_h)
{
    char name[200];

    if (sActive) return 1;
    if (sFailed) return 0;
    sFailed = 1; /* until it worked */
    if (!load_library())
    {
        snprintf(sStatus, sizeof(sStatus), "no Vulkan loader (vulkan-1.dll / libvulkan.so.1)");
        return 0;
    }
    if (!make_device() || !make_objects()) return 0;
    snprintf(name, sizeof(name), "%s", sStatus);
    if (!make_image(&sNatColor, GCN_EFB_W, GCN_EFB_H, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    if (sDepthBlit && !make_image(&sNatDepth, GCN_EFB_W, GCN_EFB_H, VK_FORMAT_D32_SFLOAT,
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT))
        return 0;
    if (!make_efb(efb_w, efb_h)) return 0;
    if (!begin_cmd()) return 0;
    init_efb_contents();
    efb_to_attachment(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (!submit(1)) return 0;
    sFailed = 0;
    sActive = 1;
    snprintf(sStatus, sizeof(sStatus), "%s, frame buffer %dx%d", name, efb_w, efb_h);
    fprintf(stderr, "gcn vk: drawing on %s\n", sStatus);
    return 1;
}

int gcn_vk_active(void) { return sActive; }

/* ---- batching --------------------------------------------------------------------------------- */
static Op *new_op(void)
{
    if (sOpCount == sOpCap)
    {
        sOpCap = sOpCap ? sOpCap * 2 : 1024;
        sOps = (Op *)realloc(sOps, sizeof(Op) * (size_t)sOpCap);
    }
    memset(&sOps[sOpCount], 0, sizeof(Op));
    return &sOps[sOpCount++];
}

static void kick(int wait, const Read *rd);
/* draws everything and waits: the buffers start over */
static void flush_batch(int rx0, int ry0, int rx1, int ry1, uint32_t *color, uint32_t *depth, int stride)
{
    Read rd;
    memset(&rd, 0, sizeof(rd));
    rd.x0 = rx0;
    rd.y0 = ry0;
    rd.x1 = rx1;
    rd.y1 = ry1;
    rd.shrink = 1;
    rd.color = color;
    rd.depth = depth;
    rd.stride = stride;
    kick(1, &rd);
}

static void write_texture_descriptor(int slot)
{
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    ii.sampler = VK_NULL_HANDLE;
    ii.imageView = sTex[slot].view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wr.dstSet = sSet;
    wr.dstBinding = 1;
    wr.dstArrayElement = (uint32_t)slot;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(sDev, 1, &wr, 0, NULL);
}

int gcn_vk_texture(const uint32_t *px, int w, int h)
{
    VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    Upload *u;
    int slot;

    if (!sActive || w <= 0 || h <= 0) return -1;
    if (bytes > STAGE_BYTES) return -1;
    if (sStageUsed + bytes > STAGE_BYTES || sUploadCount >= 4096) flush_batch(0, 0, 0, 0, NULL, NULL, 0);
    if (sFreeCount == 0) flush_batch(0, 0, 0, 0, NULL, NULL, 0); /* frees released slots */
    if (sFreeCount == 0) return -1;
    slot = sFree[--sFreeCount];
    /* with mipmaps (made after the upload) for the Textures setting; the game's own sampling
     * reads level 0 only */
    if (!make_image_levels(&sTex[slot], w, h, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, sMipOK ? mip_count(w, h) : 1))
    {
        sFree[sFreeCount++] = slot;
        return -1;
    }
    memcpy(sStage.map + sStageUsed, px, (size_t)bytes);
    sSt.uploads++;
    sSt.upload_bytes += bytes;
    if (sUploadCount == sUploadCap)
    {
        sUploadCap = sUploadCap ? sUploadCap * 2 : 256;
        sUploads = (Upload *)realloc(sUploads, sizeof(Upload) * (size_t)sUploadCap);
    }
    u = &sUploads[sUploadCount++];
    u->slot = slot;
    u->offset = sStageUsed;
    u->w = w;
    u->h = h;
    sStageUsed += (bytes + 15) & ~(VkDeviceSize)15;
    write_texture_descriptor(slot);
    return slot;
}

void gcn_vk_texture_release(int slot)
{
    if (!sActive || slot < 0 || slot >= MAX_SLOTS || !sTex[slot].image) return;
    sReleased[sReleasedCount++] = slot; /* destroyed after the next sync: earlier draws use it */
    if (sReleasedCount == MAX_SLOTS) flush_batch(0, 0, 0, 0, NULL, NULL, 0);
}

uint32_t gcn_vk_state(const GcnVkState *state)
{
    if (!sActive) return 0;
    if ((sStateCount + 1) * sizeof(GcnVkState) > STATE_BYTES)
    {
        /* full: draw what uses the old ones, start over (callers ask for a state before
         * the triangles that use it) */
        flush_batch(0, 0, 0, 0, NULL, NULL, 0);
        sStateCount = 0;
    }
    memcpy(sStates.map + (size_t)sStateCount * sizeof(GcnVkState), state, sizeof(GcnVkState));
    sSt.states++;
    return sStateCount++;
}

void gcn_vk_triangle(const GcnVkVertex v[3], const GcnVkPipe *pipe, const int scissor[4])
{
    uint32_t key;
    float bc;
    Op *op;

    if (!sActive) return;
    if ((sVtxCount + 3) * sizeof(GcnVkVertex) > VTX_BYTES) flush_batch(0, 0, 0, 0, NULL, NULL, 0);
    memcpy(sVtx.map + (size_t)sVtxCount * sizeof(GcnVkVertex), v, sizeof(GcnVkVertex) * 3);
    sSt.triangles++;
    key = pipe_key(pipe);
    bc = pipe->dst_alpha_on ? (float)pipe->dst_alpha / 255.0f : 0.0f;
    op = sOpCount ? &sOps[sOpCount - 1] : NULL;
    if (op && op->type == 0 && !op->xf && op->key == key && op->bc == bc && op->first + op->count == sVtxCount &&
        memcmp(op->sc, scissor, sizeof(op->sc)) == 0)
    {
        op->count += 3;
    }
    else
    {
        op = new_op();
        op->type = 0;
        op->first = sVtxCount;
        op->count = 3;
        op->key = key;
        op->bc = bc;
        memcpy(op->sc, scissor, sizeof(op->sc));
        pipeline(key); /* created now, outside the recording */
    }
    sVtxCount += 3;
    if (sAsync && (sVtxCount - sKickVtx) + (sXfVtxCount - sKickXf) >= KICK_VERTS) kick(0, NULL);
}

uint32_t gcn_vk_xf_state(const GcnVkXf *xf)
{
    if (!sActive) return 0;
    if ((sXfStateCount + 1) * sizeof(GcnVkXf) > XF_STATE_BYTES)
    {
        flush_batch(0, 0, 0, 0, NULL, NULL, 0);
        sXfStateCount = 0;
    }
    memcpy(sXfStates.map + (size_t)sXfStateCount * sizeof(GcnVkXf), xf, sizeof(GcnVkXf));
    sSt.xf_states++;
    return sXfStateCount++;
}

GcnVkXfVertex *gcn_vk_xf_begin(int n, uint32_t *base)
{
    static GcnVkXfVertex dummy[1];
    if (!sActive || n <= 0 || (size_t)n * sizeof(GcnVkXfVertex) > XF_VTX_BYTES / 2)
    {
        *base = 0;
        return n <= 1 ? dummy : NULL;
    }
    /* room for the vertices and every index they can make: else everything is drawn first */
    if ((sXfVtxCount + (uint32_t)n) * sizeof(GcnVkXfVertex) > XF_VTX_BYTES ||
        (sXfIdxCount + 3u * (uint32_t)n) * sizeof(uint32_t) > XF_IDX_BYTES)
        flush_batch(0, 0, 0, 0, NULL, NULL, 0);
    *base = sXfVtxCount;
    sXfVtxCount += (uint32_t)n;
    return (GcnVkXfVertex *)(sXfVtx.map + (size_t)*base * sizeof(GcnVkXfVertex));
}

void gcn_vk_xf_end(const uint32_t *idx, int count, const GcnVkPipe *pipe, const int scissor[4])
{
    uint32_t key;
    float bc;
    Op *op;

    count -= count % 3;
    if (!sActive || count <= 0) return;
    memcpy(sXfIdx.map + (size_t)sXfIdxCount * sizeof(uint32_t), idx, sizeof(uint32_t) * (size_t)count);
    sSt.triangles += (uint32_t)count / 3;
    key = pipe_key_xf(pipe);
    bc = pipe->dst_alpha_on ? (float)pipe->dst_alpha / 255.0f : 0.0f;
    op = sOpCount ? &sOps[sOpCount - 1] : NULL;
    if (op && op->type == 0 && op->xf && op->key == key && op->bc == bc && op->first + op->count == sXfIdxCount &&
        memcmp(op->sc, scissor, sizeof(op->sc)) == 0)
    {
        op->count += (uint32_t)count;
    }
    else
    {
        op = new_op();
        op->type = 0;
        op->xf = 1;
        op->first = sXfIdxCount;
        op->count = (uint32_t)count;
        op->key = key;
        op->bc = bc;
        memcpy(op->sc, scissor, sizeof(op->sc));
        pipeline(key);
    }
    sXfIdxCount += (uint32_t)count;
    if (sAsync && (sVtxCount - sKickVtx) + (sXfVtxCount - sKickXf) >= KICK_VERTS) kick(0, NULL);
}

void gcn_vk_clear(int x0, int y0, int x1, int y1, uint32_t rgba, uint32_t z, int color, int depth)
{
    Op *op;

    if (!sActive) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > sEfbW) x1 = sEfbW;
    if (y1 > sEfbH) y1 = sEfbH;
    if (x1 <= x0 || y1 <= y0 || (!color && !depth)) return;
    op = new_op();
    op->type = 1;
    op->rect[0] = x0;
    op->rect[1] = y0;
    op->rect[2] = x1;
    op->rect[3] = y1;
    op->col[0] = (float)(rgba & 255) / 255.0f;
    op->col[1] = (float)((rgba >> 8) & 255) / 255.0f;
    op->col[2] = (float)((rgba >> 16) & 255) / 255.0f;
    op->col[3] = (float)(rgba >> 24) / 255.0f;
    op->depth = (float)(z & 0xFFFFFF) / 16777216.0f;
    op->bits = (color ? 1 : 0) | (depth ? 2 : 0);
}

/* Records and submits what was batched since the last submit. wait (or a read: the rectangle,
 * x1 > x0): waits for everything, copies the read pixels out, and the buffers start over. */
static void kick(int wait, const Read *rd)
{
    int rx0 = rd ? rd->x0 : 0, ry0 = rd ? rd->y0 : 0, rx1 = rd ? rd->x1 : 0, ry1 = rd ? rd->y1 : 0;
    uint32_t *color = rd ? rd->color : NULL, *depth = rd ? rd->depth : NULL;
    int stride = rd ? rd->stride : 0, s = rd && rd->shrink > 1 ? rd->shrink : 1;
    /* depth can only be shrunk by a blit when the GPU blits D32 (else read whole, sampled here) */
    int sd = sDepthBlit ? s : 1;
    int i, reading = rx1 > rx0 && ry1 > ry0 && (color || depth);

    if (!sActive) return;
    if (reading || !sAsync) wait = 1;
    if (!sOpCount && !sUploadCount && !reading)
    {
        if (wait && !wait_all()) return;
        if (wait) goto release;
        return;
    }
    if (!begin_cmd()) return;
    for (i = 0; i < sUploadCount; i++)
    {
        const Upload *u = &sUploads[i];
        VkBufferImageCopy c;
        memset(&c, 0, sizeof(c));
        c.bufferOffset = u->offset;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = (uint32_t)u->w;
        c.imageExtent.height = (uint32_t)u->h;
        c.imageExtent.depth = 1;
        barrier(sTex[u->slot].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(sCmd, sStage.buf, sTex[u->slot].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        if (sTex[u->slot].levels > 1)
            gen_mips(&sTex[u->slot]);
        else
            barrier(sTex[u->slot].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (sOpCount)
    {
        VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        VkViewport vp;
        VkDeviceSize zero = 0;
        uint32_t bound = 0xFFFFFFFFu;

        rbi.renderPass = sPass;
        rbi.framebuffer = sFb;
        rbi.renderArea.extent.width = (uint32_t)sEfbW;
        rbi.renderArea.extent.height = (uint32_t)sEfbH;
        vkCmdBeginRenderPass(sCmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindDescriptorSets(sCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sPipeLayout, 0, 1, &sSet, 0, NULL);
        vkCmdBindVertexBuffers(sCmd, 0, 1, &sVtx.buf, &zero);
        vkCmdBindIndexBuffer(sCmd, sXfIdx.buf, 0, VK_INDEX_TYPE_UINT32);
        vp.x = 0;
        vp.y = 0;
        vp.width = (float)sEfbW;
        vp.height = (float)sEfbH;
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(sCmd, 0, 1, &vp);
        for (i = 0; i < sOpCount; i++)
        {
            const Op *op = &sOps[i];
            if (op->type == 0)
            {
                VkRect2D sc;
                float bcs[4] = {0, 0, 0, op->bc};
                VkPipeline p = pipeline(op->key);
                if (!p) continue;
                if (op->key != bound)
                {
                    vkCmdBindPipeline(sCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                    bound = op->key;
                }
                sc.offset.x = op->sc[0];
                sc.offset.y = op->sc[1];
                sc.extent.width = (uint32_t)(op->sc[2] - op->sc[0] + 1);
                sc.extent.height = (uint32_t)(op->sc[3] - op->sc[1] + 1);
                if (op->sc[2] < op->sc[0] || op->sc[3] < op->sc[1]) continue;
                vkCmdSetScissor(sCmd, 0, 1, &sc);
                vkCmdSetBlendConstants(sCmd, bcs);
                if (op->xf)
                    vkCmdDrawIndexed(sCmd, op->count, 1, op->first, 0, 0);
                else
                    vkCmdDraw(sCmd, op->count, 1, op->first, 0);
            }
            else
            {
                VkClearAttachment ca[2];
                VkClearRect cr;
                uint32_t n = 0;
                memset(ca, 0, sizeof(ca));
                if (op->bits & 1)
                {
                    ca[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    ca[n].colorAttachment = 0;
                    memcpy(ca[n].clearValue.color.float32, op->col, sizeof(op->col));
                    n++;
                }
                if (op->bits & 2)
                {
                    ca[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                    ca[n].clearValue.depthStencil.depth = op->depth;
                    n++;
                }
                cr.rect.offset.x = op->rect[0];
                cr.rect.offset.y = op->rect[1];
                cr.rect.extent.width = (uint32_t)(op->rect[2] - op->rect[0]);
                cr.rect.extent.height = (uint32_t)(op->rect[3] - op->rect[1]);
                cr.baseArrayLayer = 0;
                cr.layerCount = 1;
                vkCmdClearAttachments(sCmd, n, ca, 1, &cr);
            }
        }
        vkCmdEndRenderPass(sCmd);
    }
    if (reading)
    {
        /* color, then depth: copied out whole, or blitted down to the console's resolution first */
        int pass;
        for (pass = 0; pass < 2; pass++)
        {
            int dep = pass == 1, sh = dep ? sd : s;
            Image *src = dep ? &sDepth : &sColor, *nat = dep ? &sNatDepth : &sNatColor;
            VkImageAspectFlags asp = dep ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageLayout att = dep ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkBufferImageCopy c;

            if (!(dep ? depth != NULL : color != NULL)) continue;
            memset(&c, 0, sizeof(c));
            c.bufferOffset = dep ? (VkDeviceSize)sEfbW * sEfbH * 4 : 0;
            c.imageSubresource.aspectMask = asp;
            c.imageSubresource.layerCount = 1;
            c.imageExtent.depth = 1;
            barrier(src->image, asp, att, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            if (sh > 1)
            {
                VkImageBlit b;
                memset(&b, 0, sizeof(b));
                b.srcSubresource.aspectMask = asp;
                b.srcSubresource.layerCount = 1;
                b.srcOffsets[0].x = rx0;
                b.srcOffsets[0].y = ry0;
                b.srcOffsets[1].x = rx1;
                b.srcOffsets[1].y = ry1;
                b.srcOffsets[1].z = 1;
                b.dstSubresource = b.srcSubresource;
                b.dstOffsets[1].x = (rx1 - rx0) / sh;
                b.dstOffsets[1].y = (ry1 - ry0) / sh;
                b.dstOffsets[1].z = 1;
                barrier(nat->image, asp, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                vkCmdBlitImage(sCmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, nat->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, dep ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
                barrier(nat->image, asp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                c.imageExtent.width = (uint32_t)b.dstOffsets[1].x;
                c.imageExtent.height = (uint32_t)b.dstOffsets[1].y;
                vkCmdCopyImageToBuffer(sCmd, nat->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sRead.buf, 1, &c);
            }
            else
            {
                c.imageOffset.x = rx0;
                c.imageOffset.y = ry0;
                c.imageExtent.width = (uint32_t)(rx1 - rx0);
                c.imageExtent.height = (uint32_t)(ry1 - ry0);
                vkCmdCopyImageToBuffer(sCmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sRead.buf, 1, &c);
            }
            barrier(src->image, asp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, att);
        }
    }
    for (i = 0; i < sOpCount; i++)
    {
        if (sOps[i].type == 0) sSt.draws++;
        else sSt.clears++;
    }
    {
        static int log = -1;
        double tw = now_ms();
        if (log < 0) log = getenv("GCN_VK_READ_LOG") != NULL;
        if (!submit(wait)) return;
        if (log && reading)
            fprintf(stderr, "gcn vk read: %dx%d at %d,%d shrink %d %s%s%s: waited %.2f ms\n", rx1 - rx0, ry1 - ry0, rx0, ry0, s,
                    color ? "color" : "", depth ? " depth" : "", rd->at_origin ? " (display)" : "", now_ms() - tw);
    }
    sOpCount = 0;
    sUploadCount = 0;
    sKickVtx = sVtxCount;
    sKickXf = sXfVtxCount;
    if (!wait) return; /* still running: the buffers stay */
    if (reading)
    {
        /* out: the shrunk rectangle's size; at (ox, oy) in the caller's buffer */
        uint32_t w = (uint32_t)((rx1 - rx0) / s), h = (uint32_t)((ry1 - ry0) / s), y, x;
        size_t ox = rd->at_origin ? 0 : (size_t)(rx0 / s), oy = rd->at_origin ? 0 : (size_t)(ry0 / s);
        double t0 = now_ms();
        sSt.reads++;
        if (color && rd->at_origin)
        {
            /* the display copy: opaque (the external frame buffer has no alpha), set as it is copied */
            sSt.read_bytes += (uint64_t)w * h * 4;
            for (y = 0; y < h; y++)
            {
                const uint32_t *src = (const uint32_t *)(sRead.map + (size_t)y * w * 4);
                uint32_t *dst = color + (oy + y) * (size_t)stride + ox;
                for (x = 0; x < w; x++) dst[x] = src[x] | 0xFF000000u;
            }
        }
        else if (color)
        {
            sSt.read_bytes += (uint64_t)w * h * 4;
            for (y = 0; y < h; y++)
                memcpy(color + (oy + y) * (size_t)stride + ox, sRead.map + (size_t)y * w * 4, (size_t)w * 4);
        }
        if (depth)
        {
            /* read at sd x smaller; the rest of the shrink (no depth blits) by taking the middle sample */
            const float *src = (const float *)(sRead.map + (size_t)sEfbW * sEfbH * 4);
            uint32_t rw = (uint32_t)((rx1 - rx0) / sd), k = (uint32_t)(s / sd), off = k / 2;
            sSt.read_bytes += (uint64_t)rw * ((ry1 - ry0) / sd) * 4;
            for (y = 0; y < h; y++)
            {
                uint32_t *row = depth + (oy + y) * (size_t)stride + ox;
                const float *srow = src + (size_t)(y * k + off) * rw;
                for (x = 0; x < w; x++)
                {
                    float f = srow[x * k + off] * 16777216.0f;
                    row[x] = f <= 0.0f ? 0u : f >= 16777215.0f ? 16777215u : (uint32_t)f;
                }
            }
        }
        sSt.read_ms += now_ms() - t0;
    }
release:
    sVtxCount = 0;
    sKickVtx = 0;
    sXfVtxCount = 0;
    sKickXf = 0;
    sXfIdxCount = 0;
    sStageUsed = 0;
    for (i = 0; i < sReleasedCount; i++)
    {
        free_image(&sTex[sReleased[i]]);
        sFree[sFreeCount++] = sReleased[i];
    }
    sReleasedCount = 0;
}

void gcn_vk_read(int x0, int y0, int x1, int y1, uint32_t *color, uint32_t *depth, int stride)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > sEfbW) x1 = sEfbW;
    if (y1 > sEfbH) y1 = sEfbH;
    flush_batch(x0, y0, x1, y1, color, depth, stride);
}

void gcn_vk_read_shrunk(int x0, int y0, int x1, int y1, int shrink, uint32_t *color, uint32_t *depth, int stride)
{
    Read rd;
    if (shrink < 1) shrink = 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > sEfbW) x1 = sEfbW;
    if (y1 > sEfbH) y1 = sEfbH;
    /* whole console pixels, at most the console's frame buffer */
    x0 -= x0 % shrink;
    y0 -= y0 % shrink;
    x1 -= x1 % shrink;
    y1 -= y1 % shrink;
    if ((x1 - x0) / shrink > GCN_EFB_W) x1 = x0 + GCN_EFB_W * shrink;
    if ((y1 - y0) / shrink > GCN_EFB_H) y1 = y0 + GCN_EFB_H * shrink;
    memset(&rd, 0, sizeof(rd));
    rd.x0 = x0;
    rd.y0 = y0;
    rd.x1 = x1;
    rd.y1 = y1;
    rd.shrink = shrink;
    rd.color = color;
    rd.depth = depth;
    rd.stride = stride;
    kick(1, &rd);
}

void gcn_vk_read_into(int x0, int y0, int x1, int y1, uint32_t *dst, int dst_stride)
{
    Read rd;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > sEfbW) x1 = sEfbW;
    if (y1 > sEfbH) y1 = sEfbH;
    memset(&rd, 0, sizeof(rd));
    rd.x0 = x0;
    rd.y0 = y0;
    rd.x1 = x1;
    rd.y1 = y1;
    rd.shrink = 1;
    rd.at_origin = 1;
    rd.color = dst;
    rd.stride = dst_stride;
    kick(1, &rd);
}

void gcn_vk_sync(void) { flush_batch(0, 0, 0, 0, NULL, NULL, 0); }

void gcn_vk_reset(void)
{
    /* a game stopped mid-frame (its thread ended anywhere): drop what it batched */
    if (!sActive) return;
    wait_all();
    sOpCount = 0;
    sUploadCount = 0;
    sVtxCount = 0;
    sKickVtx = 0;
    sXfVtxCount = 0;
    sKickXf = 0;
    sXfIdxCount = 0;
    sStageUsed = 0;
    sStateCount = 0;
    sXfStateCount = 0;
}

void gcn_vk_resize(int efb_w, int efb_h)
{
    Image oldColor, oldDepth;
    VkFramebuffer oldFb;
    int ow = sEfbW, oh = sEfbH;

    if (!sActive || (efb_w == sEfbW && efb_h == sEfbH)) return;
    gcn_vk_sync();
    oldColor = sColor;
    oldDepth = sDepth;
    oldFb = sFb;
    memset(&sColor, 0, sizeof(sColor));
    memset(&sDepth, 0, sizeof(sDepth));
    sFb = VK_NULL_HANDLE;
    if (!make_efb(efb_w, efb_h) || !begin_cmd())
    {
        sActive = 0;
        sFailed = 1;
        return;
    }
    init_efb_contents();
    {
        /* the old picture, scaled over (nearest): what the game left in the frame buffer */
        VkImageBlit b;
        memset(&b, 0, sizeof(b));
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.layerCount = 1;
        b.srcOffsets[1].x = ow;
        b.srcOffsets[1].y = oh;
        b.srcOffsets[1].z = 1;
        b.dstSubresource = b.srcSubresource;
        b.dstOffsets[1].x = efb_w;
        b.dstOffsets[1].y = efb_h;
        b.dstOffsets[1].z = 1;
        barrier(oldColor.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdBlitImage(sCmd, oldColor.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sColor.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
        if (sDepthBlit)
        {
            b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            barrier(oldDepth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vkCmdBlitImage(sCmd, oldDepth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sDepth.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
        }
    }
    efb_to_attachment(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (!submit(1)) return;
    if (oldFb) vkDestroyFramebuffer(sDev, oldFb, NULL);
    free_image(&oldColor);
    free_image(&oldDepth);
}

void gcn_vk_print_stats(int frames)
{
    if (frames < 1) frames = 1;
    fprintf(stderr,
            "gcn vk: per frame: %u batches, GPU busy %.2f ms, waiting %.2f ms, %.1f reads (%.2f MB, copying %.2f ms), %u triangles, %u draws, "
            "%u clears, %u states, %u XF states, %.1f uploads (%.2f MB)\n",
            sBatches / (uint32_t)frames, sGpuExecMs / frames, sSt.wait_ms / frames, (double)sSt.reads / frames,
            (double)sSt.read_bytes / frames / 1048576.0, sSt.read_ms / frames, sSt.triangles / (uint32_t)frames,
            sSt.draws / (uint32_t)frames, sSt.clears / (uint32_t)frames, sSt.states / (uint32_t)frames,
            sSt.xf_states / (uint32_t)frames,
            (double)sSt.uploads / frames, (double)sSt.upload_bytes / frames / 1048576.0);
    memset(&sSt, 0, sizeof(sSt));
    sGpuExecMs = 0;
    sBatches = 0;
    sGpuMs = 0;
}

double gcn_vk_take_gpu_ms(uint32_t *batches)
{
    double ms = sGpuMs;
    if (batches) *batches = sBatches;
    sGpuMs = 0;
    sBatches = 0;
    return ms;
}

/* ---- texture filtering (the Textures setting) ------------------------------------------------ */
void gcn_vk_set_texture_filter(int level) { sTexFilter = level < 0 ? 0 : level > 5 ? 5 : level; }

int gcn_vk_texture_filter(void)
{
    int l = sTexFilter;
    if (!sActive || !sMipOK) return 0;
    if (l > 1 && sMaxAniso <= 1.0f) l = 1; /* no anisotropic filtering here: trilinear */
    return l;
}

/* ---- display post-processing (gcn_vk_present) ------------------------------------------------ */
enum
{
    PP_EDGE,   /* SMAA edges */
    PP_WEIGHT, /* SMAA blending weights */
    PP_BLEND,  /* SMAA neighborhood blending */
    PP_EASU,   /* FSR 1 upscaling */
    PP_RCAS,   /* FSR 1 sharpening */
    PP_COUNT
};

typedef struct
{
    float rt[4];       /* 1 / w, 1 / h, w, h of the input */
    uint32_t size[4];  /* input w, h, output w, h */
    float param[4];    /* x: RCAS sharpness (stops) */
} PostPush;

static int sPostState; /* 0 not tried, 1 ready, -1 failed */
static VkDescriptorSetLayout sPostSetLayout;
static VkPipelineLayout sPostLayout;
static VkPipeline sPostPipes[PP_COUNT];
static VkDescriptorPool sPostPool;
static VkDescriptorSet sPostSets[PP_COUNT];
static VkSampler sPostLinear;
static Image sPostSrc, sPostEdges, sPostWeights, sPostAA, sPostUp, sPostSharp, sPostArea, sPostSearch;
static Buffer sPostRead;

int gcn_vk_post_supported(void) { return sActive && sPostState >= 0; }

static int post_pipeline(int pass, const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    VkShaderModule mod;
    smi.codeSize = bytes;
    smi.pCode = code;
    CHECK(vkCreateShaderModule(sDev, &smi, NULL, &mod), "vkCreateShaderModule (post)");
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mod;
    cpi.stage.pName = "main";
    cpi.layout = sPostLayout;
    CHECK(vkCreateComputePipelines(sDev, VK_NULL_HANDLE, 1, &cpi, NULL, &sPostPipes[pass]), "vkCreateComputePipelines");
    return 1;
}

/* a texture of SMAA's (R8G8 / R8), uploaded and left SHADER_READ_ONLY */
static int post_lut(Image *im, int w, int h, VkFormat fmt, const uint8_t *data, size_t bytes, Buffer *tmp, VkDeviceSize at)
{
    VkBufferImageCopy c;
    if (!make_image(im, w, h, fmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    memcpy(tmp->map + at, data, bytes);
    memset(&c, 0, sizeof(c));
    c.bufferOffset = at;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w;
    c.imageExtent.height = (uint32_t)h;
    c.imageExtent.depth = 1;
    barrier(im->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(sCmd, tmp->buf, im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(im->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return 1;
}

/* the passes' layout, pipelines, sampler, descriptor sets and SMAA's textures (once) */
static int post_init(void)
{
    VkDescriptorSetLayoutBinding b[4];
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPushConstantRange pr;
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * PP_COUNT},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, PP_COUNT}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    VkDescriptorSetLayout layouts[PP_COUNT];
    const size_t areaBytes = sizeof(gcn_smaa_area), searchBytes = sizeof(gcn_smaa_search);
    Buffer tmp;
    int i;

    if (sPostState) return sPostState > 0;
    sPostState = -1;
    memset(b, 0, sizeof(b));
    for (i = 0; i < 4; i++)
    {
        b[i].binding = (uint32_t)i;
        b[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    dli.bindingCount = 4;
    dli.pBindings = b;
    CHECK(vkCreateDescriptorSetLayout(sDev, &dli, NULL, &sPostSetLayout), "vkCreateDescriptorSetLayout (post)");
    pr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pr.offset = 0;
    pr.size = sizeof(PostPush);
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &sPostSetLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    CHECK(vkCreatePipelineLayout(sDev, &pli, NULL, &sPostLayout), "vkCreatePipelineLayout (post)");
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    CHECK(vkCreateSampler(sDev, &sci, NULL, &sPostLinear), "vkCreateSampler (post)");
    if (!post_pipeline(PP_EDGE, gcn_post_smaa_edge_spv, sizeof(gcn_post_smaa_edge_spv)) ||
        !post_pipeline(PP_WEIGHT, gcn_post_smaa_weight_spv, sizeof(gcn_post_smaa_weight_spv)) ||
        !post_pipeline(PP_BLEND, gcn_post_smaa_blend_spv, sizeof(gcn_post_smaa_blend_spv)) ||
        !post_pipeline(PP_EASU, gcn_post_easu_spv, sizeof(gcn_post_easu_spv)) ||
        !post_pipeline(PP_RCAS, gcn_post_rcas_spv, sizeof(gcn_post_rcas_spv)))
        return 0;
    dpi.maxSets = PP_COUNT;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    CHECK(vkCreateDescriptorPool(sDev, &dpi, NULL, &sPostPool), "vkCreateDescriptorPool (post)");
    for (i = 0; i < PP_COUNT; i++) layouts[i] = sPostSetLayout;
    dai.descriptorPool = sPostPool;
    dai.descriptorSetCount = PP_COUNT;
    dai.pSetLayouts = layouts;
    CHECK(vkAllocateDescriptorSets(sDev, &dai, sPostSets), "vkAllocateDescriptorSets (post)");

    memset(&tmp, 0, sizeof(tmp));
    if (!make_buffer(&tmp, areaBytes + searchBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0)) return 0;
    if (!begin_cmd()) return 0;
    if (!post_lut(&sPostArea, GCN_SMAA_AREA_W, GCN_SMAA_AREA_H, VK_FORMAT_R8G8_UNORM, gcn_smaa_area, areaBytes, &tmp, 0) ||
        !post_lut(&sPostSearch, GCN_SMAA_SEARCH_W, GCN_SMAA_SEARCH_H, VK_FORMAT_R8_UNORM, gcn_smaa_search, searchBytes, &tmp,
                  areaBytes))
        return 0;
    if (!submit(1)) return 0;
    free_buffer(&tmp);
    sPostState = 1;
    fprintf(stderr, "gcn vk: display post-processing ready (SMAA 1x, FSR 1)\n");
    return 1;
}

/* an intermediate picture (RGBA8, storage + sampled), recreated at a new size */
static int post_image(Image *im, int w, int h)
{
    if (im->image && im->w == w && im->h == h) return 1;
    if (im->image) free_image(im);
    return make_image(im, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT);
}

/* a pass's inputs (sampled, linear, clamped; GENERAL unless SMAA's textures) and output */
static void post_set(int pass, const Image *in0, const Image *in1, const Image *in2, const Image *out)
{
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet wr[4];
    const Image *in[3];
    int i;
    in[0] = in0;
    in[1] = in1 ? in1 : in0;
    in[2] = in2 ? in2 : in0;
    for (i = 0; i < 4; i++)
    {
        const Image *im = i < 3 ? in[i] : out;
        ii[i].sampler = i < 3 ? sPostLinear : VK_NULL_HANDLE;
        ii[i].imageView = im->view;
        ii[i].imageLayout = (im == &sPostArea || im == &sPostSearch) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                      : VK_IMAGE_LAYOUT_GENERAL;
        memset(&wr[i], 0, sizeof(wr[i]));
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = sPostSets[pass];
        wr[i].dstBinding = (uint32_t)i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(sDev, 4, wr, 0, NULL);
}

/* records one pass: in_w x in_h in, out_w x out_h out (its image GENERAL, written whole) */
static void post_pass(int pass, int in_w, int in_h, int out_w, int out_h, float param, const Image *out)
{
    PostPush pp;
    memset(&pp, 0, sizeof(pp));
    pp.rt[0] = 1.0f / (float)in_w;
    pp.rt[1] = 1.0f / (float)in_h;
    pp.rt[2] = (float)in_w;
    pp.rt[3] = (float)in_h;
    pp.size[0] = (uint32_t)in_w;
    pp.size[1] = (uint32_t)in_h;
    pp.size[2] = (uint32_t)out_w;
    pp.size[3] = (uint32_t)out_h;
    pp.param[0] = param;
    barrier(out->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(sCmd, VK_PIPELINE_BIND_POINT_COMPUTE, sPostPipes[pass]);
    vkCmdBindDescriptorSets(sCmd, VK_PIPELINE_BIND_POINT_COMPUTE, sPostLayout, 0, 1, &sPostSets[pass], 0, NULL);
    vkCmdPushConstants(sCmd, sPostLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pp), &pp);
    vkCmdDispatch(sCmd, (uint32_t)(out_w + 7) / 8, (uint32_t)(out_h + 7) / 8, 1);
    /* the writes, visible to the next pass and the copy out */
    barrier(out->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
}

int gcn_vk_present(int x0, int y0, int x1, int y1, int out_w, int out_h, const GcnVkPost *post, uint32_t *dst,
                   int dst_stride, int *w_out, int *h_out)
{
    /* RCAS sharpness in stops (0 is the most): Low, Medium, High */
    static const float kStops[4] = {0.0f, 1.0f, 0.5f, 0.1f};
    int w, h, cw, ch, up, sharp, y, x;
    const Image *cur;
    VkImageCopy ic;
    VkBufferImageCopy bc;

    if (!sActive || !post || sPostState < 0) return 0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > sEfbW) x1 = sEfbW;
    if (y1 > sEfbH) y1 = sEfbH;
    w = x1 - x0;
    h = y1 - y0;
    if (w <= 0 || h <= 0) return 0;
    if (out_w > 8192) out_w = 8192;
    if (out_h > 8192) out_h = 8192;
    /* upscaled only to a larger picture (both ways) */
    up = post->upscale && out_w >= w && out_h >= h && (out_w > w || out_h > h);
    sharp = post->sharpness < 0 ? 0 : post->sharpness > 3 ? 3 : post->sharpness;
    if (!post->smaa && !up && !sharp) return 0;
    gcn_vk_sync(); /* what the game drew, in order before the passes */
    if (!post_init()) return 0;
    cw = up ? out_w : w;
    ch = up ? out_h : h;
    if (!post_image(&sPostSrc, w, h)) return 0;
    if (post->smaa && (!post_image(&sPostEdges, w, h) || !post_image(&sPostWeights, w, h) || !post_image(&sPostAA, w, h)))
        return 0;
    if (up && !post_image(&sPostUp, cw, ch)) return 0;
    if (sharp && !post_image(&sPostSharp, cw, ch)) return 0;
    if (sPostRead.size < (VkDeviceSize)cw * ch * 4)
    {
        free_buffer(&sPostRead);
        if (!make_buffer(&sPostRead, (VkDeviceSize)cw * ch * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1)) return 0;
    }

    /* the passes' inputs and outputs: picture -> [SMAA] -> [EASU] -> [RCAS] */
    cur = &sPostSrc;
    if (post->smaa)
    {
        post_set(PP_EDGE, &sPostSrc, NULL, NULL, &sPostEdges);
        post_set(PP_WEIGHT, &sPostEdges, &sPostArea, &sPostSearch, &sPostWeights);
        post_set(PP_BLEND, &sPostSrc, &sPostWeights, NULL, &sPostAA);
        cur = &sPostAA;
    }
    if (up)
    {
        post_set(PP_EASU, cur, NULL, NULL, &sPostUp);
        cur = &sPostUp;
    }
    if (sharp) post_set(PP_RCAS, cur, NULL, NULL, &sPostSharp);

    if (!begin_cmd()) return 0;
    /* the frame buffer's rectangle into the first picture */
    memset(&ic, 0, sizeof(ic));
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ic.srcSubresource.layerCount = 1;
    ic.srcOffset.x = x0;
    ic.srcOffset.y = y0;
    ic.dstSubresource = ic.srcSubresource;
    ic.extent.width = (uint32_t)w;
    ic.extent.height = (uint32_t)h;
    ic.extent.depth = 1;
    barrier(sColor.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(sPostSrc.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyImage(sCmd, sColor.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sPostSrc.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &ic);
    barrier(sColor.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barrier(sPostSrc.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    cur = &sPostSrc;
    if (post->smaa)
    {
        post_pass(PP_EDGE, w, h, w, h, 0.0f, &sPostEdges);
        post_pass(PP_WEIGHT, w, h, w, h, 0.0f, &sPostWeights);
        post_pass(PP_BLEND, w, h, w, h, 0.0f, &sPostAA);
        cur = &sPostAA;
    }
    if (up)
    {
        post_pass(PP_EASU, w, h, cw, ch, 0.0f, &sPostUp);
        cur = &sPostUp;
    }
    if (sharp)
    {
        post_pass(PP_RCAS, cw, ch, cw, ch, kStops[sharp], &sPostSharp);
        cur = &sPostSharp;
    }
    /* out to the host */
    memset(&bc, 0, sizeof(bc));
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = (uint32_t)cw;
    bc.imageExtent.height = (uint32_t)ch;
    bc.imageExtent.depth = 1;
    barrier(cur->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(sCmd, cur->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sPostRead.buf, 1, &bc);
    if (!submit(1)) return 0;

    if (dst_stride <= 0) dst_stride = cw;
    for (y = 0; y < ch; y++)
    {
        const uint32_t *src = (const uint32_t *)(sPostRead.map + (size_t)y * cw * 4);
        uint32_t *row = dst + (size_t)y * dst_stride;
        for (x = 0; x < cw; x++) row[x] = src[x] | 0xFF000000u; /* opaque, as the display copy */
    }
    sSt.reads++;
    sSt.read_bytes += (uint64_t)cw * ch * 4;
    if (w_out) *w_out = cw;
    if (h_out) *h_out = ch;
    return 1;
}

#endif /* GCN_VK */
