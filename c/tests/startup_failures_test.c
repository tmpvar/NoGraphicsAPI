// Injects Vulkan failures into the library's own translation unit and checks error returns, diagnostics, and native
// resource cleanup.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#define TEST_THREAD_LOCAL __declspec(thread)
#else
#define TEST_THREAD_LOCAL __thread
#endif

typedef struct NativeResources {
    int instances, devices, buffers, memory, semaphores, images, views, pools, queries, pipelines;
} NativeResources;
static NativeResources live;
static TEST_THREAD_LOCAL const char* failureOperation;
static TEST_THREAD_LOCAL VkResult failureResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
static TEST_THREAD_LOCAL unsigned failureSkip;
static unsigned commandAllocations, commandFrees;
static unsigned failures, reports;
static bool reportedFatal;
static char diagnostic[1024];

static bool inject(const char* operation) {
    if (!failureOperation || strcmp(failureOperation, operation)) return false;
    if (failureSkip) {
        --failureSkip;
        return false;
    }
    failureOperation = NULL;
    return true;
}

// The loader calls the hook after each loading phase (global, instance, device); the hook swaps the wrapped entry
// points into the device's function table. Normal calls still reach the real driver through the saved originals.
struct GPUVulkanFunctions;
static void test_hookFunctions(struct GPUVulkanFunctions* functions);
#define GPU_VULKAN_FUNCTIONS_HOOK(functions) test_hookFunctions(functions)
#define GPU_IMPLEMENTATION
#include "test_support.h"

static GPUVulkanFunctions real;

#define TEST_CREATE(name, parameters, arguments, counter) \
    static VkResult VKAPI_CALL test_##name parameters {   \
        if (inject(#name)) return failureResult;          \
        const VkResult result = real.name arguments;      \
        if (result == VK_SUCCESS) ++live.counter;         \
        return result;                                    \
    }
#define TEST_DESTROY(name, parameters, arguments, handle, counter) \
    static void VKAPI_CALL test_##name parameters {                \
        if (handle) --live.counter;                                \
        real.name arguments;                                       \
    }
#define TEST_RESULT(name, parameters, arguments) \
    static VkResult VKAPI_CALL test_##name parameters { return inject(#name) ? failureResult : real.name arguments; }

TEST_CREATE(
    vkCreateInstance,
    (const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* instance),
    (info, allocator, instance),
    instances
)
TEST_CREATE(
    vkCreateDevice,
    (VkPhysicalDevice physical,
     const VkDeviceCreateInfo* info,
     const VkAllocationCallbacks* allocator,
     VkDevice* device),
    (physical, info, allocator, device),
    devices
)
TEST_CREATE(
    vkCreateBuffer,
    (VkDevice device, const VkBufferCreateInfo* info, const VkAllocationCallbacks* allocator, VkBuffer* buffer),
    (device, info, allocator, buffer),
    buffers
)
TEST_CREATE(
    vkAllocateMemory,
    (VkDevice device, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* allocator, VkDeviceMemory* memory),
    (device, info, allocator, memory),
    memory
)
TEST_CREATE(
    vkCreateSemaphore,
    (VkDevice device,
     const VkSemaphoreCreateInfo* info,
     const VkAllocationCallbacks* allocator,
     VkSemaphore* semaphore),
    (device, info, allocator, semaphore),
    semaphores
)
TEST_CREATE(
    vkCreateImage,
    (VkDevice device, const VkImageCreateInfo* info, const VkAllocationCallbacks* allocator, VkImage* image),
    (device, info, allocator, image),
    images
)
TEST_CREATE(
    vkCreateImageView,
    (VkDevice device, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* allocator, VkImageView* view),
    (device, info, allocator, view),
    views
)
TEST_CREATE(
    vkCreateCommandPool,
    (VkDevice device, const VkCommandPoolCreateInfo* info, const VkAllocationCallbacks* allocator, VkCommandPool* pool),
    (device, info, allocator, pool),
    pools
)
TEST_CREATE(
    vkCreateQueryPool,
    (VkDevice device, const VkQueryPoolCreateInfo* info, const VkAllocationCallbacks* allocator, VkQueryPool* pool),
    (device, info, allocator, pool),
    queries
)
TEST_CREATE(
    vkCreateComputePipelines,
    (VkDevice device,
     VkPipelineCache cache,
     uint32_t count,
     const VkComputePipelineCreateInfo* info,
     const VkAllocationCallbacks* allocator,
     VkPipeline* pipelines),
    (device, cache, count, info, allocator, pipelines),
    pipelines
)
TEST_CREATE(
    vkCreateGraphicsPipelines,
    (VkDevice device,
     VkPipelineCache cache,
     uint32_t count,
     const VkGraphicsPipelineCreateInfo* info,
     const VkAllocationCallbacks* allocator,
     VkPipeline* pipelines),
    (device, cache, count, info, allocator, pipelines),
    pipelines
)
TEST_RESULT(
    vkBindBufferMemory,
    (VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset),
    (device, buffer, memory, offset)
)
TEST_RESULT(
    vkBindImageMemory,
    (VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset),
    (device, image, memory, offset)
)
TEST_RESULT(
    vkMapMemory,
    (VkDevice device,
     VkDeviceMemory memory,
     VkDeviceSize offset,
     VkDeviceSize size,
     VkMemoryMapFlags flags,
     void** data),
    (device, memory, offset, size, flags, data)
)
TEST_DESTROY(
    vkDestroyInstance,
    (VkInstance instance, const VkAllocationCallbacks* allocator),
    (instance, allocator),
    instance,
    instances
)
TEST_DESTROY(
    vkDestroyDevice,
    (VkDevice device, const VkAllocationCallbacks* allocator),
    (device, allocator),
    device,
    devices
)
TEST_DESTROY(
    vkDestroyBuffer,
    (VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* allocator),
    (device, buffer, allocator),
    buffer,
    buffers
)
TEST_DESTROY(
    vkFreeMemory,
    (VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator),
    (device, memory, allocator),
    memory,
    memory
)
TEST_DESTROY(
    vkDestroySemaphore,
    (VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks* allocator),
    (device, semaphore, allocator),
    semaphore,
    semaphores
)
TEST_DESTROY(
    vkDestroyImage,
    (VkDevice device, VkImage image, const VkAllocationCallbacks* allocator),
    (device, image, allocator),
    image,
    images
)
TEST_DESTROY(
    vkDestroyImageView,
    (VkDevice device, VkImageView view, const VkAllocationCallbacks* allocator),
    (device, view, allocator),
    view,
    views
)
TEST_DESTROY(
    vkDestroyCommandPool,
    (VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* allocator),
    (device, pool, allocator),
    pool,
    pools
)
TEST_DESTROY(
    vkDestroyQueryPool,
    (VkDevice device, VkQueryPool pool, const VkAllocationCallbacks* allocator),
    (device, pool, allocator),
    pool,
    queries
)
TEST_DESTROY(
    vkDestroyPipeline,
    (VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* allocator),
    (device, pipeline, allocator),
    pipeline,
    pipelines
)

static VkResult VKAPI_CALL
test_vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* commands) {
    if (inject("vkAllocateCommandBuffers")) return failureResult;
    const VkResult result = real.vkAllocateCommandBuffers(device, info, commands);
    if (result == VK_SUCCESS) commandAllocations += info->commandBufferCount;
    return result;
}

static void VKAPI_CALL
test_vkFreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer* commands) {
    commandFrees += count;
    real.vkFreeCommandBuffers(device, pool, count, commands);
}

#undef TEST_CREATE
#undef TEST_DESTROY
#undef TEST_RESULT

#define TEST_HOOK(name)                                      \
    if (functions->name && functions->name != test_##name) { \
        real.name = functions->name;                         \
        functions->name = test_##name;                       \
    }

static void test_hookFunctions(struct GPUVulkanFunctions* functions) {
    TEST_HOOK(vkCreateInstance)
    TEST_HOOK(vkCreateDevice)
    TEST_HOOK(vkCreateBuffer)
    TEST_HOOK(vkAllocateMemory)
    TEST_HOOK(vkBindBufferMemory)
    TEST_HOOK(vkMapMemory)
    TEST_HOOK(vkCreateSemaphore)
    TEST_HOOK(vkCreateImage)
    TEST_HOOK(vkBindImageMemory)
    TEST_HOOK(vkCreateImageView)
    TEST_HOOK(vkCreateCommandPool)
    TEST_HOOK(vkAllocateCommandBuffers)
    TEST_HOOK(vkCreateQueryPool)
    TEST_HOOK(vkCreateComputePipelines)
    TEST_HOOK(vkCreateGraphicsPipelines)
    TEST_HOOK(vkDestroyInstance)
    TEST_HOOK(vkDestroyDevice)
    TEST_HOOK(vkDestroyBuffer)
    TEST_HOOK(vkFreeMemory)
    TEST_HOOK(vkDestroySemaphore)
    TEST_HOOK(vkDestroyImage)
    TEST_HOOK(vkDestroyImageView)
    TEST_HOOK(vkDestroyCommandPool)
    TEST_HOOK(vkFreeCommandBuffers)
    TEST_HOOK(vkDestroyQueryPool)
    TEST_HOOK(vkDestroyPipeline)
}
#undef TEST_HOOK

static void check(bool condition, const char* message) {
    if (condition) return;
    fprintf(stderr, "FAIL: %s [%s]\n", message, diagnostic);
    ++failures;
}

static void receiveError(void* user, const GPUMessage* message) {
    (void)user;
    ++reports;
    reportedFatal |= message->fatal;
    GPUMessageFormat(message, diagnostic, sizeof(diagnostic));
}

static void arm(const char* operation, VkResult result, unsigned skip) {
    failureOperation = operation;
    failureResult = result;
    failureSkip = skip;
    reports = 0;
    reportedFatal = false;
    diagnostic[0] = '\0';
}

static void unchanged(const NativeResources* before) {
    check(
        live.instances == before->instances && live.devices == before->devices && live.buffers == before->buffers &&
            live.memory == before->memory && live.semaphores == before->semaphores && live.images == before->images &&
            live.views == before->views && live.pools == before->pools && live.queries == before->queries &&
            live.pipelines == before->pipelines,
        "partial native resources are released"
    );
}

static void reported(const char* operation, const NativeResources* before, bool bytes) {
    char result[64];
    snprintf(result, sizeof(result), "Vulkan result %d", (int)failureResult);
    check(!failureOperation, "the selected native failure was reached");
    check(reports == 1 && !reportedFatal, "one nonfatal diagnostic accompanies the returned failure");
    check(
        strstr(diagnostic, operation) && strstr(diagnostic, result),
        "diagnostic identifies the failing native operation and result"
    );
    if (bytes)
        check(strstr(diagnostic, "MiB requested") != NULL, "allocation diagnostics include the requested memory size");
    unchanged(before);
}

static void heapFailures(GPUDevice* device) {
    const char* operations[] = {"vkCreateBuffer", "vkAllocateMemory", "vkBindBufferMemory", "vkMapMemory"};
    for (size_t index = 0; index < GPUArrayCount(operations); ++index) {
        const char* operation = operations[index];
        const NativeResources before = live;
        arm(operation,
            strcmp(operation, "vkMapMemory") == 0 ? VK_ERROR_MEMORY_MAP_FAILED : VK_ERROR_OUT_OF_DEVICE_MEMORY,
            0);
        const GPUGpuHeap failed = GPUGpuHeapCreate(device, 1024 * 1024, GPUMemoryType_CpuVisible);
        check(
            !failed.owner && !failed.range.cpu && !failed.range.gpu && !failed.range.size,
            "failed mapped heap returns an empty result"
        );
        reported(operation, &before, true);
        const GPUGpuHeap retry = GPUGpuHeapCreate(device, 1024 * 1024, GPUMemoryType_CpuVisible);
        check(
            retry.owner && retry.range.cpu && retry.range.gpu && retry.range.size == 1024 * 1024,
            "mapped heap creation succeeds after failure"
        );
        GPUGpuHeapDestroy(&retry);
        unchanged(&before);
    }
    const NativeResources before = live;
    arm("vkAllocateMemory", VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
    check(!GPUTextureDescriptorHeapCreate(device, 4), "descriptor heap propagates backing allocation failure");
    reported("vkAllocateMemory", &before, true);
    GPUTextureDescriptorHeap* textures = GPUTextureDescriptorHeapCreate(device, 4);
    check(textures != NULL, "texture descriptor heap creation recovers");
    GPUTextureDescriptorHeapDestroy(textures);
    arm("vkMapMemory", VK_ERROR_MEMORY_MAP_FAILED, 0);
    check(!GPUSamplerDescriptorHeapCreate(device, 4), "sampler heap propagates backing map failure");
    reported("vkMapMemory", &before, true);
    GPUSamplerDescriptorHeap* samplers = GPUSamplerDescriptorHeapCreate(device, 4);
    check(samplers != NULL, "sampler descriptor heap creation recovers");
    GPUSamplerDescriptorHeapDestroy(samplers);
    unchanged(&before);
}

static void textureFailures(GPUDevice* device) {
    const GPUTextureDesc desc = {
        .extent = {16, 16, 1},
        .usage = GPUTextureUsage_Sampled | GPUTextureUsage_ColorAttachment,
    };
    const GPUSizeAlign size = GPUTextureGetSizeAlign(device, &desc);
    check(size.size != 0, "ordinary texture requirements are available");
    if (!size.size) return;
    const NativeResources initial = live;
    arm("vkAllocateMemory", VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
    const GPUTextureHeap failed = GPUTextureHeapCreate(device, size.size);
    check(!failed.owner && !failed.size, "texture heap allocation failure returns an empty result");
    reported("vkAllocateMemory", &initial, true);
    const GPUTextureHeap heap = GPUTextureHeapCreate(device, size.size);
    GPUCommandPool* pool = GPUCommandPoolCreate(device, 0);
    GPUCommandBuffer* commands = pool ? GPUCmdBegin(pool) : NULL;
    check(heap.owner && commands, "texture creation resources recover");
    if (heap.owner && commands) {
        const NativeResources before = live;
        const char* operations[] = {"vkCreateImage", "vkBindImageMemory"};
        for (size_t index = 0; index < GPUArrayCount(operations); ++index) {
            arm(operations[index], VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
            check(!GPUTextureCreate(commands, &desc, &heap, 0), "image creation or binding failure returns null");
            reported(operations[index], &before, false);
            GPUTexture* retry = GPUTextureCreate(commands, &desc, &heap, 0);
            check(retry != NULL, "image creation succeeds after failure");
            GPUCmdEnd(commands);
            GPUCommandPoolReset(pool);
            GPUTextureDestroy(retry);
            commands = GPUCmdBegin(pool);
            unchanged(&before);
        }
        GPUTexture* texture = GPUTextureCreate(commands, &desc, &heap, 0);
        check(texture != NULL, "render view fixture texture is available");
        if (texture) {
            const NativeResources beforeView = live;
            const GPURenderViewDesc viewDesc = {0};
            arm("vkCreateImageView", VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
            check(!GPURenderViewCreate(texture, &viewDesc), "render view creation failure returns null");
            reported("vkCreateImageView", &beforeView, false);
            GPURenderView* view = GPURenderViewCreate(texture, &viewDesc);
            check(view != NULL, "render view creation succeeds after failure");
            GPURenderViewDestroy(view);
            GPUCmdEnd(commands);
            GPUCommandPoolReset(pool);
            GPUTextureDestroy(texture);
            commands = GPUCmdBegin(pool);
        }
        GPUCmdEnd(commands);
    }
    GPUCommandPoolDestroy(pool);
    GPUTextureHeapDestroy(&heap);
    unchanged(&initial);
}

// Minimal stand-in for the C++ upload queue: a mapped heap, a timeline, a command pool, and one begun buffer, created
// together and released together.
typedef struct Uploader {
    GPUGpuHeap heap;
    GPUTimelineSemaphore* timeline;
    GPUCommandPool* pool;
    GPUCommandBuffer* commands;
    bool valid;
} Uploader;

static void destroyUploader(Uploader* uploader) {
    if (uploader->pool) GPUCommandPoolDestroy(uploader->pool);
    if (uploader->timeline) GPUTimelineSemaphoreDestroy(uploader->timeline);
    if (uploader->heap.owner) GPUGpuHeapDestroy(&uploader->heap);
    memset(uploader, 0, sizeof(*uploader));
}

static Uploader createUploader(GPUDevice* device, uint64_t capacity) {
    Uploader uploader = {.heap = GPUGpuHeapCreate(device, capacity, GPUMemoryType_CpuVisible)};
    if (uploader.heap.owner) uploader.timeline = GPUTimelineSemaphoreCreate(device, 0);
    if (uploader.timeline) uploader.pool = GPUCommandPoolCreate(device, 0);
    if (uploader.pool) uploader.commands = GPUCmdBegin(uploader.pool);
    uploader.valid = uploader.commands != NULL;
    if (!uploader.valid) destroyUploader(&uploader);
    return uploader;
}

// Minimal stand-in for the C++ texture allocator: a heap with exactly one placement slot.
typedef struct OneSlotAllocator {
    const GPUTextureHeap* heap;
    GPUTexture* texture;
} OneSlotAllocator;

static GPUTexture* allocateSlot(OneSlotAllocator* allocator, GPUCommandBuffer* commands, const GPUTextureDesc* desc) {
    if (allocator->texture) return NULL;
    allocator->texture = GPUTextureCreate(commands, desc, allocator->heap, 0);
    return allocator->texture;
}

static void freeSlot(OneSlotAllocator* allocator) {
    GPUTextureDestroy(allocator->texture);
    allocator->texture = NULL;
}

static void utilityFailures(GPUDevice* device) {
    const NativeResources initial = live;
    const char* uploadOperations[] = {
        "vkCreateBuffer",
        "vkAllocateMemory",
        "vkMapMemory",
        "vkCreateSemaphore",
        "vkCreateCommandPool",
        "vkAllocateCommandBuffers",
        "vkCreateQueryPool"
    };
    for (unsigned index = 0; index < GPUArrayCount(uploadOperations); ++index) {
        arm(uploadOperations[index], index == 2 ? VK_ERROR_MEMORY_MAP_FAILED : VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
        Uploader failed = createUploader(device, 65536);
        check(
            !failed.valid && !failed.heap.range.size && !failed.commands,
            "failed uploader construction leaves an empty invalid uploader"
        );
        reported(uploadOperations[index], &initial, index <= 2);
        destroyUploader(&failed);
        unchanged(&initial);
        Uploader retry = createUploader(device, 65536);
        check(retry.valid && retry.heap.range.size == 65536, "uploader construction succeeds after native failure");
        destroyUploader(&retry);
        check(!retry.valid, "destroying the uploader clears validity");
        unchanged(&initial);
    }

    const GPUTextureDesc desc = {.extent = {16, 16, 1}};
    const GPUSizeAlign size = GPUTextureGetSizeAlign(device, &desc);
    const uint64_t alignment = GPUDeviceGetCaps(device)->textureHeapAlignment;
    const GPUTextureHeap heap = GPUTextureHeapCreate(device, (size.size + alignment - 1) & ~(alignment - 1));
    GPUCommandPool* pool = GPUCommandPoolCreate(device, 0);
    check(heap.owner && pool, "texture allocator failure fixture is available");
    if (heap.owner && pool) {
        OneSlotAllocator allocator = {.heap = &heap, .texture = NULL};
        const char* operations[] = {"vkCreateImage", "vkBindImageMemory"};
        for (size_t index = 0; index < GPUArrayCount(operations); ++index) {
            GPUCommandBuffer* commands = GPUCmdBegin(pool);
            check(commands != NULL, "texture allocator initialization commands are available");
            if (!commands) break;
            const NativeResources before = live;
            arm(operations[index], VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
            GPUTexture* failed = allocateSlot(&allocator, commands, &desc);
            check(!failed && !allocator.texture, "failed texture creation returns an empty allocator result");
            reported(operations[index], &before, false);
            GPUTexture* retry = allocateSlot(&allocator, commands, &desc);
            check(retry != NULL, "failed texture reservation is reusable in a one-slot heap");
            check(!allocateSlot(&allocator, commands, &desc), "successful retry consumes the only texture reservation");
            GPUCmdEnd(commands);
            GPUCommandPoolReset(pool);
            freeSlot(&allocator);
            check(!allocator.texture, "freeing the successful retry restores an empty allocation");
            unchanged(&before);
        }
    }
    GPUCommandPoolDestroy(pool);
    GPUTextureHeapDestroy(&heap);
    unchanged(&initial);
}

static void commandFailures(GPUDevice* device) {
    const NativeResources before = live;
    arm("vkCreateSemaphore", VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
    check(!GPUTimelineSemaphoreCreate(device, 0), "timeline creation failure returns null");
    reported("vkCreateSemaphore", &before, false);
    GPUTimelineSemaphore* timeline = GPUTimelineSemaphoreCreate(device, 0);
    check(timeline != NULL, "timeline creation succeeds after failure");
    GPUTimelineSemaphoreDestroy(timeline);
    arm("vkCreateCommandPool", VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
    check(!GPUCommandPoolCreate(device, 0), "command pool creation failure returns null");
    reported("vkCreateCommandPool", &before, false);
    GPUCommandPool* pool = GPUCommandPoolCreate(device, 0);
    check(pool != NULL, "command pool creation succeeds after failure");
    GPUCommandPoolDestroy(pool);
    const char* operations[] = {"vkAllocateCommandBuffers", "vkCreateQueryPool"};
    for (size_t index = 0; index < GPUArrayCount(operations); ++index) {
        pool = GPUCommandPoolCreate(device, 0);
        check(pool != NULL, "command failure fixture pool is available");
        if (!pool) continue;
        const NativeResources beforeCommands = live;
        const unsigned allocations = commandAllocations, frees = commandFrees;
        arm(operations[index], VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
        check(!GPUCmdBegin(pool), "first command recording failure returns null");
        reported(operations[index], &beforeCommands, false);
        check(
            commandAllocations - allocations == commandFrees - frees,
            "failed first command context frees its native command buffer"
        );
        GPUCommandBuffer* commands = GPUCmdBegin(pool);
        check(commands != NULL, "first command recording succeeds after failure");
        if (commands) GPUCmdEnd(commands);
        GPUCommandPoolDestroy(pool);
        unchanged(&before);
    }
}

static GPUByteSpan readShader(const char* path) {
    const GPUByteSpan empty = {NULL, 0};
    FILE* file = fopen(path, "rb");
    check(file != NULL, "compiled pipeline fixture is available");
    if (!file) return empty;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    uint8_t* data = malloc((size_t)size);
    const bool valid = size > 0 && fread(data, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    check(valid, "compiled pipeline fixture loads completely");
    if (!valid) {
        free(data);
        return empty;
    }
    return (GPUByteSpan){data, (size_t)size};
}

static void pipelineFailures(GPUDevice* device) {
    const GPUByteSpan compute = readShader(GPU_FAILURE_COMPUTE);
    const GPUByteSpan vertex = readShader(GPU_FAILURE_VERTEX);
    const GPUByteSpan fragment = readShader(GPU_FAILURE_FRAGMENT);
    const NativeResources before = live;
    if (compute.data) {
        const GPUShaderStage stage = {.code = compute, .entryPoint = "computeMain"};
        arm("vkCreateComputePipelines", VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
        check(!GPUPSOCreateCompute(device, &stage), "compute pipeline failure returns null");
        reported("vkCreateComputePipelines", &before, false);
        GPUPSO* pso = GPUPSOCreateCompute(device, &stage);
        check(pso != NULL, "compute pipeline creation succeeds after failure");
        GPUPSODestroy(pso);
    }
    if (vertex.data && fragment.data) {
        const GPUColorTargetDesc target = {.format = GPUFormat_Rgba8Unorm};
        const GPUGraphicsPSODesc desc = {
            .vertex = {.code = vertex, .entryPoint = "vertexMain"},
            .fragment = {.code = fragment, .entryPoint = "fragmentMain"},
            .colorTargets = {&target, 1},
        };
        arm("vkCreateGraphicsPipelines", VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
        check(!GPUPSOCreateGraphics(device, &desc), "graphics pipeline failure returns null");
        reported("vkCreateGraphicsPipelines", &before, false);
        GPUPSO* pso = GPUPSOCreateGraphics(device, &desc);
        check(pso != NULL, "graphics pipeline creation succeeds after failure");
        GPUPSODestroy(pso);
    }
    free((void*)compute.data);
    free((void*)vertex.data);
    free((void*)fragment.data);
    unchanged(&before);
}

#if defined(_WIN32)
typedef struct CallbackWorker {
    bool isolated;
} CallbackWorker;

static void callbackThread(void* argument) {
    CallbackWorker* worker = argument;
    arm("vkCreateInstance", VK_ERROR_OUT_OF_HOST_MEMORY, 0);
    const GPUDeviceDesc desc = {0};
    const TestDeviceInit init = TestCreateDevice(&desc);
    worker->isolated = !init.device && init.error == GPUError_OutOfMemory && !failureOperation;
}
#endif

int main(void) {
    const NativeResources none = {0};
    arm("vkCreateInstance", VK_ERROR_OUT_OF_HOST_MEMORY, 0);
    GPUDeviceDesc desc = {.errorCallback = receiveError};
    TestDeviceInit init = TestCreateDevice(&desc);
    check(
        !init.device && init.error == GPUError_OutOfMemory,
        "instance allocation failure returns an error instead of terminating"
    );
    reported("vkCreateInstance", &none, false);
    arm(NULL, VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
    desc.timestampQueryCount = 8;
    init = TestCreateDevice(&desc);
    if (init.error == GPUError_Unsupported) {
        printf("SKIP: required Vulkan device features are unavailable.\n");
        return TEST_SKIPPED;
    }
    check(init.device && init.error == GPUError_None, "device creation succeeds after instance failure");
    if (!init.device) return 1;
    GPUDevice* device = init.device;
    const NativeResources beforeDevice = live;
    GPUError liveError = GPUError_DriverError;
    check(
        !GPUDeviceInit(device, &desc, &liveError) && liveError == GPUError_None,
        "initializing live device storage again is a no-op"
    );
    unchanged(&beforeDevice);
    arm("vkCreateDevice", VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
    desc = (GPUDeviceDesc){0};
    desc.errorCallback = receiveError;
    init = TestCreateDevice(&desc);
    check(!init.device && init.error == GPUError_OutOfMemory, "logical device allocation failure returns an error");
    reported("vkCreateDevice", &beforeDevice, false);
    heapFailures(device);
    textureFailures(device);
    commandFailures(device);
    utilityFailures(device);
    pipelineFailures(device);
    arm("vkCreateBuffer", VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
    GPUDeviceSetErrorCallback(device, NULL, NULL);
    const GPUGpuHeap silent = GPUGpuHeapCreate(device, 1024, GPUMemoryType_CpuVisible);
    check(
        !silent.owner && !failureOperation && reports == 0,
        "clearing the callback suppresses diagnostics without changing failure returns"
    );
    GPUDeviceSetErrorCallback(device, receiveError, NULL);
#if defined(_WIN32)
    arm(NULL, VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
    CallbackWorker worker = {.isolated = false};
    TestThread thread = {.run = callbackThread, .argument = &worker};
    const bool started = TestThreadStart(&thread);
    check(started, "callback isolation worker starts");
    if (started) {
        TestThreadJoin(&thread);
        check(worker.isolated && reports == 0, "a device created without a callback reports nothing");
    }
    arm("vkCreateBuffer", VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
    check(
        !GPUGpuHeapCreate(device, 1024, GPUMemoryType_CpuVisible).owner,
        "main thread callback remains installed after the worker finishes"
    );
    reported("vkCreateBuffer", &beforeDevice, true);
#endif
    GPUDeviceWaitIdle(device);
    TestDestroyDevice(device);
    unchanged(&none);
    printf("Startup failure returns and cleanup: %u failures\n", failures);
    return failures ? 1 : 0;
}
