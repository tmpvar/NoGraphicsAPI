// Internal declarations shared by the implementation modules. Everything here is private to the GPU_IMPLEMENTATION
// translation unit.

// Every Vulkan entry point is fetched at run time from the loader library, so nothing links against vulkan-1.
#if !defined(VK_NO_PROTOTYPES)
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

typedef struct GPUDeviceState GPUDeviceState; // The implementation view of the public GPUDevice storage.

// Win32 surface declarations from vulkan_win32.h, so the implementation does not pull in windows.h. Skipped when the
// application defined VK_USE_PLATFORM_WIN32_KHR before including the implementation.
#if defined(_WIN32) && !defined(VK_KHR_win32_surface)
#define VK_KHR_WIN32_SURFACE_EXTENSION_NAME "VK_KHR_win32_surface"
typedef struct HINSTANCE__* HINSTANCE;
typedef struct HWND__* HWND;
typedef VkFlags VkWin32SurfaceCreateFlagsKHR;
typedef struct VkWin32SurfaceCreateInfoKHR {
    VkStructureType sType;
    const void* pNext;
    VkWin32SurfaceCreateFlagsKHR flags;
    HINSTANCE hinstance;
    HWND hwnd;
} VkWin32SurfaceCreateInfoKHR;
typedef VkResult(VKAPI_PTR* PFN_vkCreateWin32SurfaceKHR)(
    VkInstance instance,
    const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface
);
#endif

// Vulkan entry points, grouped by the loading phase that provides them. Optional entries stay NULL when their extension
// is not enabled.
#define GPU_VK_GLOBAL_FUNCTIONS(X)            \
    X(vkEnumerateInstanceVersion)             \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties)     \
    X(vkCreateInstance)
#define GPU_VK_INSTANCE_FUNCTIONS(X)             \
    X(vkDestroyInstance)                         \
    X(vkEnumeratePhysicalDevices)                \
    X(vkGetPhysicalDeviceFeatures2)              \
    X(vkGetPhysicalDeviceFormatProperties2)      \
    X(vkGetPhysicalDeviceImageFormatProperties2) \
    X(vkGetPhysicalDeviceMemoryProperties)       \
    X(vkGetPhysicalDeviceProperties2)            \
    X(vkGetPhysicalDeviceQueueFamilyProperties)  \
    X(vkEnumerateDeviceExtensionProperties)      \
    X(vkCreateDevice)                            \
    X(vkGetDeviceProcAddr)
#if defined(_WIN32)
#define GPU_VK_INSTANCE_PLATFORM_FUNCTIONS(X) X(vkCreateWin32SurfaceKHR)
#else
#define GPU_VK_INSTANCE_PLATFORM_FUNCTIONS(X)
#endif
#define GPU_VK_INSTANCE_OPTIONAL_FUNCTIONS(X)     \
    X(vkDestroySurfaceKHR)                        \
    X(vkGetPhysicalDeviceSurfaceCapabilities2KHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)  \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)       \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)       \
    X(vkCreateDebugUtilsMessengerEXT)             \
    X(vkDestroyDebugUtilsMessengerEXT)            \
    GPU_VK_INSTANCE_PLATFORM_FUNCTIONS(X)
#define GPU_VK_DEVICE_FUNCTIONS(X)         \
    X(vkDestroyDevice)                     \
    X(vkGetDeviceQueue)                    \
    X(vkDeviceWaitIdle)                    \
    X(vkAllocateMemory)                    \
    X(vkFreeMemory)                        \
    X(vkMapMemory)                         \
    X(vkUnmapMemory)                       \
    X(vkCreateBuffer)                      \
    X(vkDestroyBuffer)                     \
    X(vkBindBufferMemory)                  \
    X(vkGetBufferMemoryRequirements)       \
    X(vkGetBufferDeviceAddress)            \
    X(vkGetDeviceBufferMemoryRequirements) \
    X(vkGetDeviceImageMemoryRequirements)  \
    X(vkCreateImage)                       \
    X(vkDestroyImage)                      \
    X(vkBindImageMemory)                   \
    X(vkCreateImageView)                   \
    X(vkDestroyImageView)                  \
    X(vkCreateSemaphore)                   \
    X(vkDestroySemaphore)                  \
    X(vkGetSemaphoreCounterValue)          \
    X(vkWaitSemaphores)                    \
    X(vkCreateFence)                       \
    X(vkDestroyFence)                      \
    X(vkGetFenceStatus)                    \
    X(vkResetFences)                       \
    X(vkWaitForFences)                     \
    X(vkCreateCommandPool)                 \
    X(vkDestroyCommandPool)                \
    X(vkResetCommandPool)                  \
    X(vkAllocateCommandBuffers)            \
    X(vkFreeCommandBuffers)                \
    X(vkBeginCommandBuffer)                \
    X(vkEndCommandBuffer)                  \
    X(vkCreateQueryPool)                   \
    X(vkDestroyQueryPool)                  \
    X(vkResetQueryPool)                    \
    X(vkGetQueryPoolResults)               \
    X(vkCreateComputePipelines)            \
    X(vkCreateGraphicsPipelines)           \
    X(vkDestroyPipeline)                   \
    X(vkQueueSubmit2)                      \
    X(vkCmdBeginRendering)                 \
    X(vkCmdEndRendering)                   \
    X(vkCmdBindPipeline)                   \
    X(vkCmdDispatch)                       \
    X(vkCmdDraw)                           \
    X(vkCmdDrawIndexed)                    \
    X(vkCmdPipelineBarrier2)               \
    X(vkCmdSetDepthCompareOp)              \
    X(vkCmdSetDepthTestEnable)             \
    X(vkCmdSetDepthWriteEnable)            \
    X(vkCmdSetScissorWithCount)            \
    X(vkCmdSetStencilCompareMask)          \
    X(vkCmdSetStencilOp)                   \
    X(vkCmdSetStencilReference)            \
    X(vkCmdSetStencilTestEnable)           \
    X(vkCmdSetStencilWriteMask)            \
    X(vkCmdSetViewportWithCount)           \
    X(vkCmdWriteTimestamp2)                \
    X(vkWriteSamplerDescriptorsEXT)        \
    X(vkWriteResourceDescriptorsEXT)       \
    X(vkCmdBindSamplerHeapEXT)             \
    X(vkCmdBindResourceHeapEXT)            \
    X(vkCmdPushDataEXT)                    \
    X(vkCmdBindIndexBuffer3KHR)            \
    X(vkCmdDrawIndirect2KHR)               \
    X(vkCmdDrawIndexedIndirect2KHR)        \
    X(vkCmdDispatchIndirect2KHR)           \
    X(vkCmdDrawMeshTasksEXT)               \
    X(vkCmdDrawMeshTasksIndirect2EXT)      \
    X(vkCmdCopyMemoryKHR)                  \
    X(vkCmdCopyMemoryToImageKHR)           \
    X(vkCmdCopyImageToMemoryKHR)
#define GPU_VK_DEVICE_OPTIONAL_FUNCTIONS(X) \
    X(vkCreateSwapchainKHR)                 \
    X(vkDestroySwapchainKHR)                \
    X(vkGetSwapchainImagesKHR)              \
    X(vkAcquireNextImageKHR)                \
    X(vkQueuePresentKHR)

#define GPU_VK_DECLARE(name) PFN_##name name;
typedef struct GPUVulkanFunctions {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    GPU_VK_GLOBAL_FUNCTIONS(GPU_VK_DECLARE)
    GPU_VK_INSTANCE_FUNCTIONS(GPU_VK_DECLARE)
    GPU_VK_INSTANCE_OPTIONAL_FUNCTIONS(GPU_VK_DECLARE)
    GPU_VK_DEVICE_FUNCTIONS(GPU_VK_DECLARE)
    GPU_VK_DEVICE_OPTIONAL_FUNCTIONS(GPU_VK_DECLARE)
} GPUVulkanFunctions;
#undef GPU_VK_DECLARE

#include <limits.h>
#include <stdlib.h>
#include <string.h>
// GPU_DEBUG enables precondition asserts, Vulkan validation layers, and debug-only checks. It defaults on unless NDEBUG
// is defined.
#if !defined(GPU_DEBUG) && !defined(NDEBUG)
#define GPU_DEBUG 1
#endif
#if defined(GPU_DEBUG)
#include <stdio.h> // Validation messenger output.
#endif

// Precondition checks. Define GPUAssert in the implementation translation unit to supply your own. Compiled out when
// GPU_DEBUG is not defined.
#if !defined(GPUAssert)
#if !defined(GPU_DEBUG)
#define GPUAssert(condition, message) ((void)0)
#else
#include <stdio.h>
#include <stdlib.h>
#if defined(_MSC_VER)
#define GPU_DEBUG_BREAK() __debugbreak()
#elif defined(__GNUC__)
#define GPU_DEBUG_BREAK() __builtin_trap()
#else
#define GPU_DEBUG_BREAK() (*(volatile int*)0 = 0)
#endif
#define GPUAssert(condition, message)                                            \
    do {                                                                         \
        if (!(condition)) {                                                      \
            printf("ASSERT: %s:%u %s\n", __FILE__, (unsigned)__LINE__, message); \
            GPU_DEBUG_BREAK();                                                   \
            exit(1);                                                             \
        }                                                                        \
    } while (0)
#endif
#endif

// Text formatting for GPUMessageFormat. Define GPU_SNPRINTF(buffer, size, format, ...) in the implementation
// translation unit to avoid stdio.
#if !defined(GPU_SNPRINTF)
#include <stdio.h>
#define GPU_SNPRINTF snprintf
#endif

// Atomics. Only used for shared bookkeeping: object pools and the atomic bump allocator.
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static inline uint64_t GPU_AtomicAddU64(volatile uint64_t* value, uint64_t addend) {
    return (uint64_t)_InterlockedExchangeAdd64((volatile long long*)value, (long long)addend);
}
static inline uint64_t GPU_AtomicLoadU64(volatile uint64_t* value) {
    return (uint64_t)_InterlockedCompareExchange64((volatile long long*)value, 0, 0);
}
static inline void GPU_AtomicStoreU64(volatile uint64_t* value, uint64_t desired) {
    _InterlockedExchange64((volatile long long*)value, (long long)desired);
}
static inline bool GPU_AtomicCompareExchangeU64(volatile uint64_t* value, uint64_t* expected, uint64_t desired) {
    const uint64_t previous =
        (uint64_t)_InterlockedCompareExchange64((volatile long long*)value, (long long)desired, (long long)*expected);
    if (previous == *expected) return true;
    *expected = previous;
    return false;
}
static inline uint32_t GPU_AtomicExchangeU32(volatile uint32_t* value, uint32_t desired) {
    return (uint32_t)_InterlockedExchange((volatile long*)value, (long)desired);
}
static inline void GPU_AtomicStoreU32(volatile uint32_t* value, uint32_t desired) {
    _InterlockedExchange((volatile long*)value, (long)desired);
}
#else
static inline uint64_t GPU_AtomicAddU64(volatile uint64_t* value, uint64_t addend) {
    return __atomic_fetch_add(value, addend, __ATOMIC_ACQ_REL);
}
static inline uint64_t GPU_AtomicLoadU64(volatile uint64_t* value) { return __atomic_load_n(value, __ATOMIC_ACQUIRE); }
static inline void GPU_AtomicStoreU64(volatile uint64_t* value, uint64_t desired) {
    __atomic_store_n(value, desired, __ATOMIC_RELEASE);
}
static inline bool GPU_AtomicCompareExchangeU64(volatile uint64_t* value, uint64_t* expected, uint64_t desired) {
    return __atomic_compare_exchange_n(value, expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static inline uint32_t GPU_AtomicExchangeU32(volatile uint32_t* value, uint32_t desired) {
    return __atomic_exchange_n(value, desired, __ATOMIC_ACQUIRE);
}
static inline void GPU_AtomicStoreU32(volatile uint32_t* value, uint32_t desired) {
    __atomic_store_n(value, desired, __ATOMIC_RELEASE);
}
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define GPU_NORETURN __declspec(noreturn)
#else
#define GPU_NORETURN __attribute__((noreturn))
#endif

#define GPU_MAX_DEVICE_EXTENSIONS 512
#define GPU_MAX_INSTANCE_EXTENSIONS 256
#define GPU_MAX_INSTANCE_LAYERS 64
#define GPU_MAX_PHYSICAL_DEVICES 32
#define GPU_MAX_QUEUE_FAMILIES 64
#define GPU_QUEUE_TYPE_COUNT 3 // General, compute-only, copy-only.
#define GPU_MAX_COLOR_ATTACHMENTS 8
#define GPU_MAX_SWAPCHAIN_IMAGES 8
#define GPU_SWAPCHAIN_PRESENT_MODE VK_PRESENT_MODE_FIFO_KHR
#define GPU_ALLOCATION_ALIGNMENT 16

// Device block allocation. Override both before including the implementation; the defaults are an aligned calloc and
// the matching free. The block does not need to be zeroed by the allocator.
#if !defined(GPU_ALLOC)
static void* GPU_DefaultAlloc(uint64_t size, uint64_t align) {
#if defined(_WIN32)
    void* memory = _aligned_malloc((size_t)size, (size_t)align);
#else
    void* memory = aligned_alloc((size_t)align, (size_t)size);
#endif
    memset(memory, 0, (size_t)size);
    return memory;
}
static void GPU_DefaultFree(void* memory) {
#if defined(_WIN32)
    _aligned_free(memory);
#else
    free(memory);
#endif
}
#define GPU_ALLOC(user, size, align) GPU_DefaultAlloc(size, align)
#if !defined(GPU_FREE)
#define GPU_FREE(user, memory) GPU_DefaultFree(memory)
#endif
#elif !defined(GPU_FREE)
#define GPU_FREE(user, memory) ((void)(user), (void)(memory))
#endif
#define GPU_MAX_SURFACE_FORMATS 64
#define GPU_FORMAT_COUNT ((uint32_t)GPUFormat_Count)

typedef enum GPUDescriptorHeapType {
    GPUDescriptorHeapType_Texture,
    GPUDescriptorHeapType_Sampler,
} GPUDescriptorHeapType;

// Object pools. Fixed-capacity slabs carved from the device allocation. Freed slots are recycled through an intrusive
// free list. Allocation and release are synchronized with a spinlock because distinct resource creation may run
// concurrently.

typedef struct GPUPool {
    uint8_t* slots;
    uint32_t slotSize; // Multiple of 16, at least 16.
    uint32_t capacity;
    uint32_t freeHead; // Slot index, or capacity when the free list is empty. Each free slot stores the next index in
                       // its first four bytes.
    uint32_t liveCount;
    volatile uint32_t lock;
} GPUPool;

static void GPU_PoolInit(GPUPool* pool, uint8_t* storage, uint32_t slotSize, uint32_t capacity);
static void* GPU_PoolAlloc(GPUPool* pool); // Returns a zeroed slot, or NULL when the pool is exhausted.
static void GPU_PoolFree(GPUPool* pool, void* slot);

static inline uint64_t GPU_AlignUp(uint64_t value, uint64_t alignment) {
    GPUAssert(
        alignment != 0 && (alignment & (alignment - 1)) == 0,
        "alignment != 0 && (alignment & (alignment - 1)) == 0"
    );
    return (value + alignment - 1) & ~(alignment - 1);
}
static inline uint64_t GPU_DivideUp(uint64_t value, uint64_t divisor) {
    GPUAssert(divisor != 0, "divisor != 0");
    return (value + divisor - 1) / divisor;
}
static inline uint32_t GPU_PoolSlotSize(uint64_t size) { return (uint32_t)GPU_AlignUp(size < 16 ? 16 : size, 16); }

typedef struct GPUBackingBuffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapped;
    VkDeviceAddress address;
} GPUBackingBuffer;

typedef struct GPUDeferredSwapchainImage {
    uint64_t retireValue;
    VkSwapchainKHR swapchain;
    VkImageView view;
} GPUDeferredSwapchainImage;

// Fixed-capacity ring of swapchain images awaiting retirement. Capacity is GPUDeviceLimits::maxRetiredSwapchainImages.
typedef struct GPUSwapchainDeleteQueue {
    GPUDeferredSwapchainImage* entries;
    uint32_t capacity;
    uint32_t first;
    uint32_t count;
} GPUSwapchainDeleteQueue;

typedef struct GPURetiredSwapchain {
    VkSwapchainKHR handle;
    VkImageView views[GPU_MAX_SWAPCHAIN_IMAGES];
    uint32_t viewCount;
} GPURetiredSwapchain;

typedef struct GPUQueue {
    VkQueue queue;
    uint32_t familyIndex;
    bool timestamps;
    VkCommandBufferSubmitInfo* commandSubmitInfos; // maxSubmitCommandBuffers entries.
    VkSemaphoreSubmitInfo* waitSubmitInfos;        // maxSubmitWaits + 1 entries.
} GPUQueue;

typedef struct GPUPresentContext {
    VkSemaphore acquired;
    VkSemaphore rendered;
    VkFence presented;
    VkSwapchainKHR swapchain;
    bool presentPending;
} GPUPresentContext;

struct GPUGpuHeapOwner {
    GPUDeviceState* state;
    GPUBackingBuffer backing;
};

struct GPUTextureDescriptorHeap {
    GPUDeviceState* state;
    GPUGpuHeap storage;
    uint32_t capacity;
};

struct GPUSamplerDescriptorHeap {
    GPUDeviceState* state;
    GPUGpuHeap storage;
    uint32_t capacity;
};

struct GPUTextureHeapOwner {
    GPUDeviceState* state;
    VkDeviceMemory memory;
};

struct GPUTimelineSemaphore {
    GPUDeviceState* state;
    VkSemaphore semaphore;
};

typedef struct GPUSwapchain GPUSwapchain;

struct GPUCommandBuffer {
    GPUDeviceState* state;
    GPUCommandBuffer* next;
    VkCommandPool commandPool;
    VkCommandBuffer commandBuffer;
    VkCommandBuffer epilogue;
    VkQueryPool timestampPool;
    uint64_t** timestampDestinations; // timestampQueryCount entries, carved from the pool slot.
    uint64_t* timestampResults;       // timestampQueryCount entries, carved from the pool slot.
    uint32_t timestampCount;
    GPUSwapchain* swapchain;
    bool submitted;
    bool hasEpilogue;
};

// A command pool slot holds the GPUCommandPool followed by maxCommandBuffersPerPool GPUCommandBuffer structs and their
// timestamp arrays.
struct GPUCommandPool {
    GPUDeviceState* state;
    VkCommandPool commandPool;
    GPUCommandBuffer* buffers; // maxCommandBuffersPerPool entries following this struct in the slot.
    uint32_t bufferCount;      // Buffers with Vulkan command buffers allocated so far.
    GPUCommandBuffer* first;   // Buffers begun since the last reset, in order.
    GPUCommandBuffer* last;
    GPUCommandBuffer* nextBuffer; // Next allocated-but-unused buffer to reuse after a reset.
    bool timestamps;
};

struct GPUTexture {
    GPUDeviceState* state;
    VkImage image;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t layerCount;
    GPUTextureType type;
    GPUFormat format;
};

struct GPURenderView {
    GPUDeviceState* state;
    VkImageView view;
    uint32_t width;
    uint32_t height;
    bool swapchainView;
};

struct GPUSwapchain {
    GPUDeviceState* state;
    VkSwapchainKHR handle;
    VkImage images[GPU_MAX_SWAPCHAIN_IMAGES];
    GPURenderView renderViews[GPU_MAX_SWAPCHAIN_IMAGES];
    bool initialized[GPU_MAX_SWAPCHAIN_IMAGES];
    uint32_t imageCount;
    uint32_t imageIndex;
    uint32_t width;
    uint32_t height;
    GPUFormat format;
    VkColorSpaceKHR colorSpace;
    VkSurfaceTransformFlagBitsKHR transform;
    VkCompositeAlphaFlagBitsKHR compositeAlpha;
    GPUPresentContext* presentContext;
    GPUCommandBuffer* transitionCommands;
    bool acquired;
    bool recreateRequired;
};

struct GPUPSO {
    GPUDeviceState* state;
    VkPipeline pso;
    VkPipelineBindPoint bindPoint;
};

// Byte offsets of every region inside the single GPU_ALLOC block. Computed by GPU_ComputeDeviceLayout from a
// GPUDeviceDesc.
typedef struct GPUDeviceLayout {
    uint64_t queues;             // GPUQueue array, queueCapacity entries.
    uint64_t commandSubmitInfos; // queueCapacity * maxSubmitCommandBuffers VkCommandBufferSubmitInfo.
    uint64_t waitSubmitInfos;    // queueCapacity * (maxSubmitWaits + 1) VkSemaphoreSubmitInfo.
    uint64_t retiredImages;      // maxRetiredSwapchainImages GPUDeferredSwapchainImage.
    uint64_t timelineSemaphores;
    uint64_t gpuHeaps;
    uint64_t textureHeaps;
    uint64_t textures;
    uint64_t renderViews;
    uint64_t textureDescriptorHeaps;
    uint64_t samplerDescriptorHeaps;
    uint64_t psos;
    uint64_t commandPools;
    uint32_t queueCapacity;
    uint32_t commandPoolSlotSize;
    uint64_t total;
} GPUDeviceLayout;

struct GPUDeviceState {
    bool initialized; // Set by a successful GPUDeviceInit; GPUDeviceDestroy zeroes the whole state.
    void* memory;     // The GPU_ALLOC block holding every region of GPUDeviceLayout.
    void* allocUser;
    GPUErrorCallback
        errorCallback; // Set from GPUDeviceDesc before any Vulkan call so creation failures are reported too.
    void* errorUser;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    GPUQueue* queues;
    uint32_t queueCount;
    VkSurfaceKHR surface;
    uint32_t queueFamilies[GPU_QUEUE_TYPE_COUNT];
    uint32_t queueFamilyCount;
    uint32_t timestampQueryCount;
    GPUDeviceLimits limits;
    VkPhysicalDeviceMemoryProperties memoryProperties;
    VkPhysicalDeviceProperties physicalProperties;
    VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProperties;
    uint64_t maxTimelineValueDifference;
    uint64_t textureHeapAlignment;
    uint32_t textureMemoryType;
    GPUVulkanFunctions fn;
    void* loaderModule;
    GPUDeviceCaps caps;
    VkFormatFeatureFlags2 formatFeatures[GPU_FORMAT_COUNT];
    bool textureCompressionEtc2;
    bool swapchainColorSpaceEnabled;
    VkSemaphore presentationRetirement;
    uint64_t presentationRetirementValue;
    uint64_t completedPresentationRetirement;
    GPUSwapchainDeleteQueue swapchainDeleteQueue;
    GPUPresentContext presentContexts[GPU_MAX_SWAPCHAIN_IMAGES];
    GPURetiredSwapchain retiredSwapchains[GPU_MAX_SWAPCHAIN_IMAGES];
    GPUSwapchain swapchainStorage;
    GPUSwapchain* swapchain; // &swapchainStorage while a swapchain exists.
    GPUSwapchain* acquiredSwapchain;
    uint32_t presentContextCount;
    uint32_t nextPresentContext;

    GPUPool timelineSemaphorePool;
    GPUPool gpuHeapPool;
    GPUPool textureHeapPool;
    GPUPool texturePool;
    GPUPool renderViewPool;
    GPUPool textureDescriptorHeapPool;
    GPUPool samplerDescriptorHeapPool;
    GPUPool psoPool;
    GPUPool commandPoolPool;
};

GPUStaticAssert(sizeof(GPUDeviceState) <= sizeof(GPUDevice), device_state_fits_public_storage);

// Shared internal functions. Defined in gpu_common.c unless noted.

// Every report goes through the device's callback. device may be NULL only where no device block exists yet; nothing is
// reported then.
static void GPU_Report(const GPUDeviceState* device, const GPUMessage* message);
static void GPU_ReportKind(
    const GPUDeviceState* device,
    GPUMessageKind kind
);                                                     // Kinds without data fields, never fatal.
static GPUError GPU_LoaderOpen(GPUDeviceState* state); // Opens the loader library and the pre-instance entry points.
static GPUError GPU_LoaderLoadInstanceFunctions(GPUDeviceState* state); // After vkCreateInstance.
static GPUError GPU_LoaderLoadDeviceFunctions(GPUDeviceState* state);   // After vkCreateDevice.
static void GPU_LoaderClose(GPUDeviceState* state);
static void GPU_ReportLimit(const GPUDeviceState* device, GPULimit limit);
static void GPU_ReportVkFailure(
    const GPUDeviceState* device,
    VkResult result,
    const char* operation,
    bool fatal,
    uint64_t byteCount
);
static GPU_NORETURN void GPU_AbortVkFailure(
    const GPUDeviceState* device,
    VkResult result,
    const char* operation,
    uint64_t byteCount
);
static GPUError GPU_ErrorFromVk(
    const GPUDeviceState* device,
    VkResult result,
    const char* operation,
    uint64_t byteCount
);
static void GPU_RequireVk(const GPUDeviceState* device, VkResult result, const char* operation, uint64_t byteCount);
static void GPU_AssertVk(VkResult result);
static void GPU_RequireError(const GPUDeviceState* device, GPUError error, const char* operation);

static bool GPU_HasExtension(const VkExtensionProperties* values, uint32_t count, const char* name);
#if defined(GPU_DEBUG)
static bool GPU_HasLayer(const VkLayerProperties* values, uint32_t count, const char* name);
#endif
#if defined(GPU_DEBUG)
static VKAPI_ATTR VkBool32 VKAPI_CALL GPU_CommonDebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData
);
#endif

static VkFormat GPU_ToVkFormat(GPUFormat format);
static bool GPU_CompatibleViewFormats(GPUFormat imageFormat, GPUFormat viewFormat);
static bool GPU_HasDepthAspect(GPUFormat format);
static bool GPU_HasStencilAspect(GPUFormat format);
static VkImageAspectFlags GPU_ImageAspects(GPUFormat format);
static VkImageType GPU_ToVkImageType(GPUTextureType type);
static VkImageViewType GPU_ToVkImageViewType(GPUTextureType type);
static VkFormatFeatureFlags2 GPU_RequiredFormatFeatures(GPUTextureUsage usage);
#if defined(GPU_DEBUG)
static bool GPU_HasValidTextureUsageBits(GPUTextureUsage usage);
#endif

typedef enum GPUTextureCompression {
    GPUTextureCompression_None,
    GPUTextureCompression_Bc,
    GPUTextureCompression_Astc,
    GPUTextureCompression_Etc2,
} GPUTextureCompression;

static GPUTextureCompression GPU_TextureCompression(GPUFormat format);
static VkFormatFeatureFlags2 GPU_OptimalFormatFeatures(const GPUDeviceState* state, GPUFormat format);
static VkBlendFactor GPU_ToVkBlendFactor(GPUBlendFactor factor);
static VkPipelineStageFlags2 GPU_ToVkStages(GPUStage stages);
static VkAccessFlags2 GPU_ToVkAccess(GPUAccess accesses);
static VkCompareOp GPU_ToVkCompareOp(GPUCompareOp op);
static VkFilter GPU_ToVkFilter(GPUFilter filter);
static VkSamplerMipmapMode GPU_ToVkMipmapMode(GPUFilter filter);
static bool GPU_IsUsableMemoryType(const VkPhysicalDeviceMemoryProperties* properties, uint32_t index);

#define GPU_UNIVERSAL_BUFFER_USAGE                                                  \
    (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | \
     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
#define GPU_CPU_VISIBLE_MEMORY_PROPERTIES \
    (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
#define GPU_FORBIDDEN_MEMORY_PROPERTIES                                           \
    (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT | \
     VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD)
#define GPU_ADDRESS_FLAGS VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR

// Device memory helpers (gpu_device.c).
static bool GPU_FindMemoryType(
    const GPUDeviceState* device,
    uint32_t bits,
    VkMemoryPropertyFlags required,
    VkMemoryPropertyFlags preferred,
    VkDeviceSize minimumHeapSize,
    uint32_t* output,
    VkMemoryPropertyFlags avoided
);
static bool GPU_CreateBackingBuffer(
    const GPUDeviceState* device,
    GPUBackingBuffer* output,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags required,
    VkMemoryPropertyFlags preferred,
    VkMemoryPropertyFlags avoided
);
static void GPU_DestroyBackingBuffer(const GPUDeviceState* device, GPUBackingBuffer* backing);
static GPUGpuHeap GPU_AllocateDescriptorHeap(GPUDeviceState* device, VkDeviceSize size, GPUDescriptorHeapType type);
static void GPU_ComputeDeviceLayout(const GPUDeviceDesc* desc, GPUDeviceLayout* layout);

// Barriers and binding (gpu_common.c).
static void GPU_RecordImageBarriers(
    const GPUDeviceState* state,
    VkCommandBuffer commandBuffer,
    const VkImageMemoryBarrier2* barriers,
    uint32_t count
);
static void GPU_RecordBarrier(
    const GPUDeviceState* state,
    VkCommandBuffer commandBuffer,
    GPUStage before,
    GPUAccess beforeAccess,
    GPUStage after,
    GPUAccess afterAccess
);
static void GPU_MakeHeapBindInfo(
    GPUGpuRange heap,
    VkDeviceSize reservedAlignment,
    VkDeviceSize reservedSize,
    VkBindHeapInfoEXT* output
);
static VkDeviceMemoryImageCopyKHR GPU_MakeTextureCopyRegion(
    const GPUTexture* texture,
    const GPUTextureCopyDesc* copy,
    GPUGpuRange memory
);

// Presentation (gpu_swapchain.c).
static uint64_t GPU_NextPresentationRetirement(GPUDeviceState* device);
static void GPU_PollPresentationRetirement(GPUDeviceState* device);
static void GPU_WaitPresentationRetirement(GPUDeviceState* device, uint64_t value);
static GPUError GPU_CreatePresentContext(GPUDeviceState* device, GPUPresentContext* context);
static void GPU_DestroyPresentContext(GPUDeviceState* device, GPUPresentContext* context);
static void GPU_FinishPresentContext(GPUDeviceState* device, GPUPresentContext* context);
static void GPU_WaitPresentContext(GPUDeviceState* device, GPUPresentContext* context);
static void GPU_PollPresentContexts(GPUDeviceState* device);
static void GPU_QueueRetiredSwapchain(GPUDeviceState* device, GPURetiredSwapchain* retired);
static void GPU_DrainContexts(GPUDeviceState* device);
static void GPU_SwapchainDeleteQueuePush(
    GPUSwapchainDeleteQueue* queue,
    uint64_t retireValue,
    VkSwapchainKHR swapchain,
    VkImageView view
);
static void GPU_SwapchainDeleteQueueCollect(
    GPUDeviceState* state,
    GPUSwapchainDeleteQueue* queue,
    VkDevice device,
    uint64_t completedValue
);
static GPUError GPU_RecreateSwapchain(GPUSwapchain* swapchain);
static GPUError GPU_CreateSwapchain(
    GPUDeviceState* device,
    const GPUDeviceDesc* desc
); // Initializes device->swapchainStorage and sets device->swapchain.
static void GPU_DestroyOwnedSwapchain(GPUDeviceState* device);
static bool GPU_SupportsSwapchainMode(GPUDeviceState* device, GPUFormat format, VkColorSpaceKHR colorSpace);

// Textures (gpu_resources.c).
static bool GPU_SupportsImageCreateInfo(
    GPUDeviceState* device,
    const VkImageCreateInfo* imageInfo,
    VkImageFormatProperties* output
);
static VkMemoryRequirements GPU_ImageMemoryRequirements(GPUDeviceState* device, const VkImageCreateInfo* imageInfo);
static bool GPU_SelectTextureMemoryType(GPUDeviceState* device);
static bool GPU_SupportsGpuHeapMemory(GPUDeviceState* device);

// Submission (gpu_commands.c).
static void GPU_SubmitCommands(
    GPUDeviceState* device,
    const GPUSubmitDesc* desc,
    uint32_t queueIndex,
    VkSemaphore waitSemaphore,
    VkSemaphore signalSemaphore
);
