// gpu.h - C99 single-header port of NoGraphicsAPI over Vulkan 1.4.
// Ported from the C++ Vulkan backend at commit 04004140f5b3b8ec7c566fd43bfed77d586155f8 (2026-10-06, "Report graphics
// initialization and resource creation failures"). Compare against src/NoGraphicsAPI.cpp at that commit when merging
// later C++ changes.
//
// Define GPU_IMPLEMENTATION in exactly one translation unit before including this header.
// The library allocates once per device through GPU_ALLOC; see GPUDeviceInit.

#ifndef GPU_H_INCLUDED
#define GPU_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Extent types. Shared CPU/GPU vector and matrix structs belong to the application; see tests/shaders/shader_types.h
// for a C-layout set.
// ---------------------------------------------------------------------------------------------------------------------

typedef struct GPUExtent2 {
    uint32_t x, y;
} GPUExtent2;
typedef struct GPUExtent3 {
    uint32_t x, y, z;
} GPUExtent3;

#define GPUStaticAssert(condition, name) typedef char gpu_static_assert_##name[(condition) ? 1 : -1]
GPUStaticAssert(sizeof(void*) == 8, pointer_size);

// ---------------------------------------------------------------------------------------------------------------------
// Spans. A span does not own its storage. A span built from a compound literal is valid only through the enclosing full
// expression; functions never retain one.
// ---------------------------------------------------------------------------------------------------------------------

typedef struct GPUByteSpan {
    const void* data;
    uint64_t size;
} GPUByteSpan;
#define GPUBytes(value) ((GPUByteSpan){&(value), sizeof(value)})
#define GPUByteSpanOf(pointer, byteSize) ((GPUByteSpan){(pointer), (byteSize)})

#define GPUArrayCount(array) (sizeof(array) / sizeof((array)[0]))
#define GPUSpanOf(SpanType, ElementType, ...) \
    ((SpanType){(ElementType[]){__VA_ARGS__}, GPUArrayCount(((ElementType[]){__VA_ARGS__}))})
#define GPUArraySpan(SpanType, array) ((SpanType){(array), GPUArrayCount(array)})

// ---------------------------------------------------------------------------------------------------------------------
// Opaque handles.
// ---------------------------------------------------------------------------------------------------------------------

// Caller-owned device storage. GPUDeviceInit fills it in place, so it can live inside persistent application state
// and survive a code hot reload.
#define GPU_DEVICE_SIZE 8192
typedef struct GPUDevice {
    uint64_t opaque[GPU_DEVICE_SIZE / 8];
} GPUDevice;
typedef struct GPUCommandPool GPUCommandPool;
typedef struct GPUTexture GPUTexture;
typedef struct GPURenderView GPURenderView;
typedef struct GPUPSO GPUPSO;
typedef struct GPUCommandBuffer GPUCommandBuffer;
typedef struct GPUTimelineSemaphore GPUTimelineSemaphore;
typedef struct GPUGpuHeapOwner GPUGpuHeapOwner;
typedef struct GPUTextureHeapOwner GPUTextureHeapOwner;
typedef struct GPUTextureDescriptorHeap GPUTextureDescriptorHeap;
typedef struct GPUSamplerDescriptorHeap GPUSamplerDescriptorHeap;

// ---------------------------------------------------------------------------------------------------------------------
// Enumerations.
// ---------------------------------------------------------------------------------------------------------------------

typedef enum GPUError {
    GPUError_None,
    GPUError_Unsupported,
    GPUError_DeviceLost,
    GPUError_DriverError,
    GPUError_OutOfMemory,
} GPUError;

// Synchronous diagnostics on the calling thread; message is valid only during the callback. Do not call graphics APIs
// recursively. The callback is stored in the device block, so it survives reloading the library. Fatal execution errors
// still abort after the callback returns.
typedef enum GPUMessageKind {
    GPUMessageKind_VulkanFailure,      // A Vulkan call failed; see GPUMessage::vulkan.
    GPUMessageKind_GraphicsFailure,    // An internal operation returned a GPUError during frame execution; see
                                       // GPUMessage::graphics. Always fatal.
    GPUMessageKind_NoMemoryHeap,       // No GPU memory heap of the requested kind can hold the allocation; see
                                       // GPUMessage::memoryHeap.
    GPUMessageKind_LimitReached,       // A GPUDeviceLimits capacity is exhausted; see GPUMessage::limit.
    GPUMessageKind_LoaderTooOld,       // The Vulkan loader is older than 1.4.
    GPUMessageKind_NoCompatibleGpu,    // No physical device offers the required extensions, features, memory, and
                                       // presentation support.
    GPUMessageKind_DeviceUnsupported,  // Device creation failed on a required capability after a physical device was
                                       // selected.
    GPUMessageKind_MissingEntryPoints, // The driver did not provide a required Vulkan command entry point.
    GPUMessageKind_TextureUnsupported, // The GPU cannot create the requested texture format, usage, or extent.
    GPUMessageKind_SurfaceCompositeAlphaUnsupported, // The display surface exposes no supported composite alpha mode.
    GPUMessageKind_SurfaceExtentUnavailable,         // The display surface does not provide a fixed drawable extent.
    GPUMessageKind_LoaderUnavailable,                // The Vulkan loader library could not be opened.
} GPUMessageKind;

typedef enum GPULimit {
    GPULimit_TimelineSemaphores,
    GPULimit_GpuHeaps,
    GPULimit_TextureHeaps,
    GPULimit_Textures,
    GPULimit_RenderViews,
    GPULimit_TextureDescriptorHeaps,
    GPULimit_SamplerDescriptorHeaps,
    GPULimit_Psos,
    GPULimit_CommandPools,
} GPULimit;

// Diagnostic delivered to GPUErrorCallback. Strings point at static storage and remain valid for the process lifetime.
typedef struct GPUMessage {
    GPUMessageKind kind;
    bool fatal;
    union {
        struct {
            const char* operation;
            int32_t result;
            uint64_t byteCount;
        } vulkan;
        struct {
            const char* operation;
            GPUError error;
        } graphics;
        struct {
            bool cpuVisible;
            uint64_t byteCount;
        } memoryHeap;
        struct {
            GPULimit limit;
        } limit;
    } data;
} GPUMessage;

typedef void (*GPUErrorCallback)(void* user, const GPUMessage* message);
// Writes a NUL-terminated description of message into buffer, truncating to size - 1 characters. Returns the full
// length like snprintf. size must be nonzero.
int GPUMessageFormat(const GPUMessage* message, char* buffer, uint64_t size);

typedef enum GPUMemoryType {
    GPUMemoryType_CpuVisible,
    GPUMemoryType_GpuOnly,
    GPUMemoryType_Readback,
} GPUMemoryType;

typedef enum GPUFormat {
    GPUFormat_Undefined, // Zero, so a zero-initialized format means "unspecified".
    GPUFormat_R8Srgb,
    GPUFormat_Rg8Srgb,
    GPUFormat_Rgba8Srgb,
    GPUFormat_Bgra8Srgb,

    GPUFormat_Rgba4Unorm,
    GPUFormat_R5g5b5a1Unorm,
    GPUFormat_R5g6b5Unorm,

    GPUFormat_R8Unorm,
    GPUFormat_Rg8Unorm,
    GPUFormat_Rgba8Unorm,
    GPUFormat_Bgra8Unorm,
    GPUFormat_R16Unorm,
    GPUFormat_Rg16Unorm,
    GPUFormat_Rgba16Unorm,

    GPUFormat_R8Uint,
    GPUFormat_Rg8Uint,
    GPUFormat_Rgba8Uint,
    GPUFormat_Bgra8Uint,
    GPUFormat_R16Uint,
    GPUFormat_Rg16Uint,
    GPUFormat_Rgba16Uint,
    GPUFormat_R32Uint,
    GPUFormat_Rg32Uint,
    GPUFormat_Rgba32Uint,

    GPUFormat_R16Float,
    GPUFormat_Rg16Float,
    GPUFormat_Rgba16Float,
    GPUFormat_R32Float,
    GPUFormat_Rg32Float,
    GPUFormat_Rgba32Float,

    GPUFormat_Rgb10a2Unorm,
    GPUFormat_Rg11b10Float,

    GPUFormat_D16Unorm,
    GPUFormat_D24UnormS8Uint,
    GPUFormat_D32Float,
    GPUFormat_S8Uint,
    GPUFormat_D32FloatS8Uint,

    GPUFormat_EacRg,
    GPUFormat_Astc4x4Srgb,
    GPUFormat_Astc4x4Unorm,
    GPUFormat_Bc3Srgb,
    GPUFormat_Bc3Unorm,
    GPUFormat_Bc5Rg,
    GPUFormat_Bc6hUfloat,
    GPUFormat_Bc6hSfloat,
    GPUFormat_Bc7Srgb,
    GPUFormat_Bc7Unorm,

    GPUFormat_Count,
} GPUFormat;

typedef enum GPUTextureType {
    GPUTextureType_Undefined, // Resolves to 2D.
    GPUTextureType_1D,
    GPUTextureType_2D,
    GPUTextureType_3D,
    GPUTextureType_Cube,
    GPUTextureType_2DArray,
    GPUTextureType_CubeArray,
} GPUTextureType;

typedef uint32_t GPUTextureUsage;
#define GPUTextureUsage_None 0u
#define GPUTextureUsage_Sampled (1u << 0)
#define GPUTextureUsage_Storage (1u << 1)
#define GPUTextureUsage_ColorAttachment (1u << 2)
#define GPUTextureUsage_DepthStencilAttachment (1u << 3)
#define GPUTextureUsage_TransferSource (1u << 4)
#define GPUTextureUsage_TransferDestination (1u << 5)

typedef enum GPUTextureDescriptorType {
    GPUTextureDescriptorType_Sampled,
    GPUTextureDescriptorType_Storage,
} GPUTextureDescriptorType;

typedef enum GPUTextureAspect {
    GPUTextureAspect_Automatic,
    GPUTextureAspect_Color,
    GPUTextureAspect_Depth,
    GPUTextureAspect_Stencil,
} GPUTextureAspect;

typedef enum GPUFilter {
    GPUFilter_Undefined, // Resolves to Linear.
    GPUFilter_Nearest,
    GPUFilter_Linear,
} GPUFilter;

typedef enum GPUAddressMode {
    GPUAddressMode_Repeat,
    GPUAddressMode_MirroredRepeat,
    GPUAddressMode_ClampToEdge,
} GPUAddressMode;

typedef enum GPUCompareOp {
    GPUCompareOp_Undefined, // Resolves to LessEqual for samplers and depth, Always for stencil faces.
    GPUCompareOp_Never,
    GPUCompareOp_Less,
    GPUCompareOp_Equal,
    GPUCompareOp_LessEqual,
    GPUCompareOp_Greater,
    GPUCompareOp_NotEqual,
    GPUCompareOp_GreaterEqual,
    GPUCompareOp_Always,
} GPUCompareOp;

typedef enum GPUCullMode {
    GPUCullMode_None,
    GPUCullMode_Clockwise,
    GPUCullMode_CounterClockwise,
} GPUCullMode;

typedef enum GPUBlendFactor {
    GPUBlendFactor_Zero,
    GPUBlendFactor_One,
    GPUBlendFactor_SourceColor,
    GPUBlendFactor_OneMinusSourceColor,
    GPUBlendFactor_DestinationColor,
    GPUBlendFactor_OneMinusDestinationColor,
    GPUBlendFactor_SourceAlpha,
    GPUBlendFactor_OneMinusSourceAlpha,
    GPUBlendFactor_DestinationAlpha,
    GPUBlendFactor_OneMinusDestinationAlpha,
    GPUBlendFactor_SourceAlphaSaturate,
} GPUBlendFactor;

typedef enum GPUBlendOp {
    GPUBlendOp_Add,
    GPUBlendOp_Subtract,
    GPUBlendOp_ReverseSubtract,
    GPUBlendOp_Minimum,
    GPUBlendOp_Maximum,
} GPUBlendOp;

typedef enum GPUIndexType {
    GPUIndexType_U16,
    GPUIndexType_U32,
} GPUIndexType;

typedef enum GPULoadOp {
    GPULoadOp_Load,
    GPULoadOp_Clear,
    GPULoadOp_Discard,
} GPULoadOp;

typedef enum GPUStoreOp {
    GPUStoreOp_Store,
    GPUStoreOp_Discard,
} GPUStoreOp;

typedef enum GPUStencilOp {
    GPUStencilOp_Keep,
    GPUStencilOp_Zero,
    GPUStencilOp_Replace,
    GPUStencilOp_IncrementClamp,
    GPUStencilOp_DecrementClamp,
    GPUStencilOp_Invert,
    GPUStencilOp_IncrementWrap,
    GPUStencilOp_DecrementWrap,
} GPUStencilOp;

// Ordered by Vulkan's logical execution order where stages are comparable.
// A barrier's before execution scope includes the selected and logically earlier stages; its after execution scope
// includes the selected and logically later stages. Vertex and task/mesh are alternative graphics branches,
// depth_stencil_tests spans early and late tests around fragment, compute and transfer are separate pipelines, host is
// a pseudo-stage, and none/all_commands are special masks.
typedef uint64_t GPUStage;
#define GPUStage_None 0ull
#define GPUStage_Indirect (1ull << 0)
#define GPUStage_IndexInput (1ull << 1)
#define GPUStage_Vertex (1ull << 2)
#define GPUStage_Task (1ull << 3)
#define GPUStage_Mesh (1ull << 4)
#define GPUStage_DepthStencilTests (1ull << 5)
#define GPUStage_Fragment (1ull << 6)
#define GPUStage_ColorOutput (1ull << 7)
#define GPUStage_Compute (1ull << 8)
#define GPUStage_Transfer (1ull << 9)
#define GPUStage_Host (1ull << 10)        // Barrier destination only, paired with GPUAccess_HostRead.
#define GPUStage_AllCommands (1ull << 11) // All GPU command stages; excludes host.

typedef uint64_t GPUAccess;
#define GPUAccess_None 0ull
#define GPUAccess_TransferRead (1ull << 0)
#define GPUAccess_TransferWrite (1ull << 1)
#define GPUAccess_ShaderRead (1ull << 2)
#define GPUAccess_ShaderWrite (1ull << 3)
#define GPUAccess_ColorRead (1ull << 4)
#define GPUAccess_ColorWrite (1ull << 5)
#define GPUAccess_DepthStencilRead (1ull << 6)
#define GPUAccess_DepthStencilWrite (1ull << 7)
#define GPUAccess_IndirectRead (1ull << 8)
#define GPUAccess_IndexRead (1ull << 9)
#define GPUAccess_HostRead (1ull << 10)
#define GPUAccess_DescriptorRead (1ull << 11)

typedef enum GPUColorSpace {
    GPUColorSpace_Srgb,
    GPUColorSpace_ExtendedSrgbLinear,
} GPUColorSpace;

typedef uint32_t GPURenderingFlags;
#define GPURenderingFlags_None 0u
#define GPURenderingFlags_Suspending 2u
#define GPURenderingFlags_Resuming 4u

// ---------------------------------------------------------------------------------------------------------------------
// Memory ranges and heaps.
// ---------------------------------------------------------------------------------------------------------------------

typedef struct GPUSizeAlign {
    uint64_t size;
    uint64_t align;
} GPUSizeAlign;

typedef struct GPUGpuRange {
    void* gpu;
    uint64_t size;
} GPUGpuRange;

typedef struct GPUGpuCpuRange {
    uint8_t* cpu;
    uint8_t* gpu;
    uint64_t size;
} GPUGpuCpuRange;

typedef struct GPUGpuHeap {
    // Copies alias the same allocation. Pass one unchanged copy to GPUGpuHeapDestroy exactly once.
    GPUGpuCpuRange range;
    const GPUGpuHeapOwner* owner;
} GPUGpuHeap;

typedef struct GPUTextureHeap {
    // Copies alias the same allocation. Pass one unchanged copy to GPUTextureHeapDestroy exactly once.
    uint64_t size;
    const GPUTextureHeapOwner* owner;
} GPUTextureHeap;

static inline GPUGpuRange GPUGpuRangeOf(GPUGpuCpuRange range) { return (GPUGpuRange){range.gpu, range.size}; }
static inline GPUGpuRange GPUGpuRangeOfHeap(const GPUGpuHeap* heap) { return GPUGpuRangeOf(heap->range); }
static inline GPUGpuRange GPUGpuSubrange(GPUGpuRange range, uint64_t offset, uint64_t size) {
    return (GPUGpuRange){(uint8_t*)range.gpu + offset, size};
}

// ---------------------------------------------------------------------------------------------------------------------
// Timelines and submission.
// ---------------------------------------------------------------------------------------------------------------------

typedef struct GPUTimelinePoint {
    GPUTimelineSemaphore* semaphore;
    uint64_t value;
} GPUTimelinePoint;

typedef struct GPUCommandBufferSpan {
    GPUCommandBuffer* const* data;
    uint64_t count;
} GPUCommandBufferSpan;
typedef struct GPUTimelinePointSpan {
    const GPUTimelinePoint* data;
    uint64_t count;
} GPUTimelinePointSpan;
#define GPUCommandBuffers(...) GPUSpanOf(GPUCommandBufferSpan, GPUCommandBuffer*, __VA_ARGS__)
#define GPUTimelinePoints(...) GPUSpanOf(GPUTimelinePointSpan, GPUTimelinePoint, __VA_ARGS__)

typedef struct GPUSubmitDesc {
    GPUCommandBufferSpan commands;
    GPUTimelinePointSpan waits;
    GPUTimelinePoint completion;
} GPUSubmitDesc;

// ---------------------------------------------------------------------------------------------------------------------
// Formats.
// ---------------------------------------------------------------------------------------------------------------------

typedef struct GPUTextureFormatInfo {
    GPUExtent2 blockExtent;
    uint32_t bytesPerBlock;
    bool depth;
    bool stencil;
} GPUTextureFormatInfo;

static inline GPUTextureFormatInfo GPUFormatGetTextureInfo(GPUFormat format) {
    GPUTextureFormatInfo info = {{1, 1}, 0, false, false};
    switch (format) {
    case GPUFormat_R8Srgb:
    case GPUFormat_R8Unorm:
    case GPUFormat_R8Uint:
    case GPUFormat_S8Uint:
        info.bytesPerBlock = 1;
        info.stencil = format == GPUFormat_S8Uint;
        return info;
    case GPUFormat_Rg8Srgb:
    case GPUFormat_Rgba4Unorm:
    case GPUFormat_R5g5b5a1Unorm:
    case GPUFormat_R5g6b5Unorm:
    case GPUFormat_Rg8Unorm:
    case GPUFormat_R16Unorm:
    case GPUFormat_Rg8Uint:
    case GPUFormat_R16Uint:
    case GPUFormat_R16Float:
    case GPUFormat_D16Unorm:
        info.bytesPerBlock = 2;
        info.depth = format == GPUFormat_D16Unorm;
        return info;
    case GPUFormat_Rgba8Srgb:
    case GPUFormat_Bgra8Srgb:
    case GPUFormat_Rgba8Unorm:
    case GPUFormat_Bgra8Unorm:
    case GPUFormat_Rg16Unorm:
    case GPUFormat_Rgba8Uint:
    case GPUFormat_Bgra8Uint:
    case GPUFormat_Rg16Uint:
    case GPUFormat_R32Uint:
    case GPUFormat_Rg16Float:
    case GPUFormat_R32Float:
    case GPUFormat_Rgb10a2Unorm:
    case GPUFormat_Rg11b10Float:
    case GPUFormat_D24UnormS8Uint:
    case GPUFormat_D32Float:
        info.bytesPerBlock = 4;
        info.depth = format == GPUFormat_D24UnormS8Uint || format == GPUFormat_D32Float;
        info.stencil = format == GPUFormat_D24UnormS8Uint;
        return info;
    case GPUFormat_Rgba16Unorm:
    case GPUFormat_Rgba16Uint:
    case GPUFormat_Rg32Uint:
    case GPUFormat_Rgba16Float:
    case GPUFormat_Rg32Float:
    case GPUFormat_D32FloatS8Uint:
        info.bytesPerBlock = 8;
        info.depth = format == GPUFormat_D32FloatS8Uint;
        info.stencil = format == GPUFormat_D32FloatS8Uint;
        return info;
    case GPUFormat_Rgba32Uint:
    case GPUFormat_Rgba32Float: info.bytesPerBlock = 16; return info;
    case GPUFormat_EacRg:
    case GPUFormat_Astc4x4Srgb:
    case GPUFormat_Astc4x4Unorm:
    case GPUFormat_Bc3Srgb:
    case GPUFormat_Bc3Unorm:
    case GPUFormat_Bc5Rg:
    case GPUFormat_Bc6hUfloat:
    case GPUFormat_Bc6hSfloat:
    case GPUFormat_Bc7Srgb:
    case GPUFormat_Bc7Unorm:
        info.blockExtent.x = 4;
        info.blockExtent.y = 4;
        info.bytesPerBlock = 16;
        return info;
    case GPUFormat_Count:
    case GPUFormat_Undefined: break;
    }
    info.blockExtent.x = 0;
    info.blockExtent.y = 0;
    return info;
}

// ---------------------------------------------------------------------------------------------------------------------
// Device description. Zero-initialized descriptors are valid everywhere; a zero field selects the default noted on
// the field. Initialize only the fields that differ.
// ---------------------------------------------------------------------------------------------------------------------

typedef struct GPUDeviceCaps {
    const char* deviceName;
    // Flat queue indices: general first, then compute-only, then copy-only. Queue zero supports presentation.
    uint32_t queueCount;
    uint32_t generalQueueCount;
    uint32_t computeQueueCount;
    uint32_t copyQueueCount;
    // Copy-only texture offsets/extents align to these texel-block counts, except at mip edges. Zero means whole mip
    // levels only.
    GPUExtent3 copyTextureGranularity;
    uint64_t maxPushDataSize;
    // Common element size for suballocating GPUTextureHeap storage; every GPUSizeAlign::align divides this value.
    uint64_t textureHeapAlignment;
    float timestampPeriodNs;
    uint32_t subTexelPrecisionBits;
    bool textureCompressionBc;
    bool textureCompressionAstc;
    bool storageInputOutput16;
    bool indirectMeshDraw;
} GPUDeviceCaps;

// Object limits size the device's GPU_ALLOC block. Creation beyond a limit returns NULL or an empty aggregate.
// Timestamp storage is reserved up front: maxCommandPools * maxCommandBuffersPerPool * timestampQueryCount * 16 bytes
// (8 MiB with the defaults).
typedef struct GPUDeviceLimits {
    uint32_t maxTimelineSemaphores;     // 0 = 64
    uint32_t maxGpuHeaps;               // 0 = 256
    uint32_t maxTextureHeaps;           // 0 = 64
    uint32_t maxTextures;               // 0 = 4096
    uint32_t maxRenderViews;            // 0 = 4096
    uint32_t maxTextureDescriptorHeaps; // 0 = 8
    uint32_t maxSamplerDescriptorHeaps; // 0 = 8
    uint32_t maxPsos;                   // 0 = 1024
    uint32_t maxCommandPools;           // 0 = 32
    uint32_t maxCommandBuffersPerPool;  // 0 = 64
    uint32_t maxSubmitCommandBuffers;   // 0 = 64
    uint32_t maxSubmitWaits;            // 0 = 16
    uint32_t maxRetiredSwapchainImages; // 0 = 64
} GPUDeviceLimits;

// Win32 windowed device lifecycle, swapchain format/extent changes, acquire, and presentation stay on the window's
// message-pump thread. The window must outlive the device. Other calls follow the object-level threading contract
// below.
typedef struct GPUDeviceDesc {
#if defined(_WIN32)
    struct {
        struct HINSTANCE__* hinstance;
        struct HWND__* hwnd;
    } win32;
#endif
    GPUFormat swapchainFormat;
    GPUColorSpace swapchainColorSpace;
    uint32_t desiredSwapchainImageCount; // 0 = 2
    // Counts are capped to each family's capacity. A nonzero request requires that kind of queue to be available.
    uint32_t desiredQueueCount; // 0 = 1
    uint32_t desiredComputeQueueCount;
    uint32_t desiredCopyQueueCount;
    uint32_t timestampQueryCount; // 0 disables timestamp queries.
    GPUDeviceLimits limits;
    GPUErrorCallback errorCallback;
    void* errorUser;
    void* allocUser; // First argument of GPU_ALLOC and GPU_FREE.
} GPUDeviceDesc;

typedef struct GPUSwapchainFrame {
    GPURenderView* renderView;
    GPUExtent2 extent;
} GPUSwapchainFrame;

// ---------------------------------------------------------------------------------------------------------------------
// Resource descriptions.
// ---------------------------------------------------------------------------------------------------------------------

// Zero fields resolve to a 1x1x1 single-mip single-layer 2D Rgba8Unorm sampled texture.
typedef struct GPUTextureDesc {
    GPUTextureType type;
    GPUExtent3 extent;
    uint32_t mipLevels;
    uint32_t layerCount;
    GPUFormat format;
    bool mutableFormat;
    GPUTextureUsage usage;
    // Overlapping placements require activation before each period of use; contents are discarded.
    bool aliasable;
} GPUTextureDesc;

typedef struct GPURenderViewDesc {
    uint32_t mipLevel;
    uint32_t slice;
} GPURenderViewDesc;

typedef struct GPUTextureDescriptorDesc {
    GPUFormat format;
    GPUTextureAspect aspect;
    uint32_t baseMip;
    uint32_t mipCount;
    uint32_t baseLayer;
    uint32_t layerCount;
} GPUTextureDescriptorDesc;

typedef struct GPUTextureCopyDesc {
    uint32_t mipLevel;
    uint32_t baseSlice;
    uint32_t sliceCount;
    GPUExtent3 offset;
    GPUExtent3 extent;
    uint64_t rowPitchBytes;
    uint64_t slicePitchBytes;
} GPUTextureCopyDesc;

typedef struct GPUSamplerDesc {
    GPUFilter minFilter;
    GPUFilter magFilter;
    GPUFilter mipFilter;
    GPUAddressMode addressU;
    GPUAddressMode addressV;
    GPUAddressMode addressW;
    // Uses the fixed 4x profile.
    bool anisotropic;
    bool compareEnabled;
    GPUCompareOp compare;
} GPUSamplerDesc;

// ---------------------------------------------------------------------------------------------------------------------
// Pipeline state.
// ---------------------------------------------------------------------------------------------------------------------

typedef struct GPUBlendComponentState {
    GPUBlendFactor source;
    GPUBlendFactor destination;
    GPUBlendOp operation;
} GPUBlendComponentState;

typedef struct GPUBlendState {
    bool enabled;
    GPUBlendComponentState color;
    GPUBlendComponentState alpha;
} GPUBlendState;

typedef struct GPUColorTargetDesc {
    GPUFormat format;
    GPUBlendState blend;
    uint8_t writeMask; // 0 = all channels
} GPUColorTargetDesc;

typedef struct GPURasterizationState {
    GPUCullMode cull;
    float depthBiasConstant;
    float depthBiasClamp;
    float depthBiasSlope;
} GPURasterizationState;

typedef struct GPUViewport {
    float x;
    float y;
    float width;
    float height;
    float minDepth;
    float maxDepth; // 0 = 1
} GPUViewport;

typedef struct GPUScissor {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} GPUScissor;

typedef struct GPUStencilFaceState {
    GPUCompareOp compare;
    GPUStencilOp fail;
    GPUStencilOp pass;
    GPUStencilOp depthFail;
    uint8_t reference;
} GPUStencilFaceState;

typedef struct GPUDepthStencilState {
    bool depthTest;
    bool depthWrite;
    GPUCompareOp depthCompare;
    bool stencilTest; // Masks and faces apply only while set.
    uint8_t stencilReadMask;
    uint8_t stencilWriteMask;
    GPUStencilFaceState front;
    GPUStencilFaceState back;
} GPUDepthStencilState;

typedef struct GPUShaderStage {
    GPUByteSpan code;
    const char* entryPoint;     // NULL = "main"
    GPUExtent3 threadgroupSize; // Required by Metal pipelines. Vulkan takes the size from the SPIR-V module.
} GPUShaderStage;

typedef struct GPUColorTargetDescSpan {
    const GPUColorTargetDesc* data;
    uint64_t count;
} GPUColorTargetDescSpan;
#define GPUColorTargets(...) GPUSpanOf(GPUColorTargetDescSpan, GPUColorTargetDesc, __VA_ARGS__)

typedef struct GPUGraphicsPSODesc {
    GPUShaderStage vertex;
    GPUShaderStage fragment;
    GPUColorTargetDescSpan colorTargets;
    GPUFormat depthFormat;
    GPUFormat stencilFormat;
    GPURasterizationState rasterization;
} GPUGraphicsPSODesc;

typedef struct GPUMeshPSODesc {
    GPUShaderStage task;
    GPUShaderStage mesh;
    GPUShaderStage fragment;
    GPUColorTargetDescSpan colorTargets;
    GPUFormat depthFormat;
    GPUFormat stencilFormat;
    GPURasterizationState rasterization;
} GPUMeshPSODesc;

// ---------------------------------------------------------------------------------------------------------------------
// Render passes.
// ---------------------------------------------------------------------------------------------------------------------

typedef struct GPUClearColor {
    float x;
    float y;
    float z;
    float w;
} GPUClearColor;

typedef struct GPUColorAttachment {
    GPURenderView* renderView;
    GPULoadOp load;
    GPUStoreOp store;
    GPUClearColor clear;
} GPUColorAttachment;

typedef struct GPUDepthAttachment {
    GPURenderView* renderView;
    GPULoadOp load;
    GPUStoreOp store;
    float clear;
} GPUDepthAttachment;

typedef struct GPUStencilAttachment {
    GPURenderView* renderView;
    GPULoadOp load;
    GPUStoreOp store;
    uint8_t clear;
} GPUStencilAttachment;

typedef struct GPUColorAttachmentSpan {
    const GPUColorAttachment* data;
    uint64_t count;
} GPUColorAttachmentSpan;
#define GPUColorAttachments(...) GPUSpanOf(GPUColorAttachmentSpan, GPUColorAttachment, __VA_ARGS__)

typedef struct GPURenderingDesc {
    GPUColorAttachmentSpan colors;
    GPUDepthAttachment depth;
    GPUStencilAttachment stencil;
} GPURenderingDesc;

// ---------------------------------------------------------------------------------------------------------------------
// Device.
//
// Rendering and raster PSOs accept at most eight color attachments. A render pass needs at least one attachment to
// infer its area. All resource destruction is immediate. Destroy resources only when no recorded or executing GPU frame
// uses them. GPUDeleteQueue can defer destruction until a submitted frame completes. Wait for all submitted frames to
// drain before destroying the device. Distinct resource creation/destruction, immutable queries, and timeline waits may
// run concurrently on one device. Resource lifetime changes must be synchronized with every CPU/GPU use of that
// resource. Concurrent descriptor updates require disjoint destinations, and copy source slots must not be modified by
// another update. Each (device, queue_index) and command pool (including recording its buffers) is externally
// synchronized; different queues/pools may run concurrently. Device idle/destruction requires exclusive access. Destroy
// command pools before their device. There are no internal queue or pool locks. Initialization and resource creation
// failures return a GPUError, NULL, or an empty aggregate; details go to the optional error callback.
// ---------------------------------------------------------------------------------------------------------------------

// Initializes zero-initialized caller-owned storage and returns true. Storage that already holds a live device is left
// as is, except that the error callback and allocUser are refreshed, and the call returns false; reloaded code can
// call it unconditionally and recreate its resources only when the device is new. error (optional) receives
// GPUError_None or the failure; after a failure the storage is zero again.
// Object pools come from one GPU_ALLOC(desc->allocUser, size, 16) call that GPUDeviceDestroy releases with
// GPU_FREE(desc->allocUser, block). Define GPU_ALLOC and GPU_FREE before including the implementation to override the
// aligned calloc defaults. Defining only GPU_ALLOC makes GPU_FREE a no-op.
bool GPUDeviceInit(GPUDevice* device, const GPUDeviceDesc* desc, GPUError* error);
void GPUDeviceDestroy(GPUDevice* device); // NULL is ignored. Leaves the storage zero.
// Replaces the callback installed by GPUDeviceDesc. Externally synchronize with every call that may report through the
// device.
void GPUDeviceSetErrorCallback(GPUDevice* device, GPUErrorCallback callback, void* user);
const GPUDeviceCaps* GPUDeviceGetCaps(const GPUDevice* device);
bool GPUDeviceSupportsTextureFormat(const GPUDevice* device, GPUFormat format, GPUTextureUsage usage);
GPUExtent2 GPUDeviceGetDrawableExtent(GPUDevice* device);
// Call after GPUDeviceWaitIdle, outside an acquired frame. Unsupported pairs leave the current mode unchanged.
// A zero-size drawable defers swapchain recreation until acquire sees a nonzero extent.
GPUError GPUDeviceSetSwapchainFormat(GPUDevice* device, GPUFormat format, GPUColorSpace colorSpace);

GPUTimelineSemaphore* GPUTimelineSemaphoreCreate(GPUDevice* device, uint64_t initialValue);
void GPUTimelineSemaphoreDestroy(GPUTimelineSemaphore* semaphore);
uint64_t GPUTimelineSemaphoreGetCompletedValue(const GPUTimelineSemaphore* semaphore);
void GPUTimelineSemaphoreWait(GPUTimelinePoint point);
void GPUDeviceWaitIdle(GPUDevice* device);

// Acquire outside a render pass. Later buffers in the same presentation submission may also access the returned image.
// GPUDeviceSubmitAndPresent prepares the image for presentation after all submitted buffers.
// Empty while the drawable extent is zero. A nonempty acquire must be submitted with GPUDeviceSubmitAndPresent on
// queue zero.
GPUSwapchainFrame GPUCmdAcquire(GPUCommandBuffer* commands);
void GPUDeviceSubmitAndPresent(GPUDevice* device, const GPUSubmitDesc* desc);

// Every non-NULL returned pointer is 16-byte aligned. GPU heaps are raw blocks for application-side suballocation.
GPUGpuHeap GPUGpuHeapCreate(GPUDevice* device, uint64_t byteCount, GPUMemoryType memory);
void GPUGpuHeapDestroy(const GPUGpuHeap* heap);

// Texture heaps use one device-selected GPU-only memory type and must outlive every placed texture.
// Placements must satisfy GPUTextureGetSizeAlign. Overlap is allowed only between aliasable textures with disjoint
// periods of GPU use. Reusing a placement without alias activation requires completion of its last use and destruction
// of the old texture. GPUDeviceCaps::textureHeapAlignment can be used as a common allocator element size, avoiding
// per-placement leading alignment padding.
GPUTextureHeap GPUTextureHeapCreate(GPUDevice* device, uint64_t byteCount);
void GPUTextureHeapDestroy(const GPUTextureHeap* heap);
GPUSizeAlign GPUTextureGetSizeAlign(GPUDevice* device, const GPUTextureDesc* desc);
// Records ordinary texture initialization into commands, outside a render pass. Submit before use. Aliasable textures
// defer initialization to activation.
GPUTexture* GPUTextureCreate(
    GPUCommandBuffer* commands,
    const GPUTextureDesc* desc,
    const GPUTextureHeap* heap,
    uint64_t offset
);
void GPUTextureDestroy(GPUTexture* texture);
// Activate an alias outside a render pass before its first use and whenever another alias has used its storage.
// Discards contents and synchronizes prior uses on this queue; order other queues with timeline waits. Clear or
// overwrite before reading.
void GPUCmdActivateTextureAlias(GPUCommandBuffer* commands, GPUTexture* texture);
GPURenderView* GPURenderViewCreate(GPUTexture* texture, const GPURenderViewDesc* desc);
void GPURenderViewDestroy(GPURenderView* renderView);
// Descriptor slots form application-owned namespaces. Writes and copies require indices within capacity;
// do not modify slots until all commands using their previous contents have completed. Copies may overlap.
GPUTextureDescriptorHeap* GPUTextureDescriptorHeapCreate(GPUDevice* device, uint32_t capacity);
void GPUTextureDescriptorHeapDestroy(GPUTextureDescriptorHeap* heap);
void GPUTextureDescriptorHeapWrite(
    GPUTextureDescriptorHeap* heap,
    uint32_t index,
    const GPUTexture* texture,
    GPUTextureDescriptorType type,
    const GPUTextureDescriptorDesc* desc
);
void GPUTextureDescriptorHeapCopy(
    const GPUTextureDescriptorHeap* source,
    uint32_t sourceIndex,
    GPUTextureDescriptorHeap* destination,
    uint32_t destinationIndex,
    uint32_t count
);
GPUSamplerDescriptorHeap* GPUSamplerDescriptorHeapCreate(GPUDevice* device, uint32_t capacity);
void GPUSamplerDescriptorHeapDestroy(GPUSamplerDescriptorHeap* heap);
void GPUSamplerDescriptorHeapWrite(GPUSamplerDescriptorHeap* heap, uint32_t index, const GPUSamplerDesc* desc);
void GPUSamplerDescriptorHeapCopy(
    const GPUSamplerDescriptorHeap* source,
    uint32_t sourceIndex,
    GPUSamplerDescriptorHeap* destination,
    uint32_t destinationIndex,
    uint32_t count
);

GPUPSO* GPUPSOCreateGraphics(GPUDevice* device, const GPUGraphicsPSODesc* desc);
GPUPSO* GPUPSOCreateMesh(GPUDevice* device, const GPUMeshPSODesc* desc);
GPUPSO* GPUPSOCreateCompute(GPUDevice* device, const GPUShaderStage* compute);
void GPUPSODestroy(GPUPSO* pso);

// Pools retain command storage until destruction. Reset only after every submitted buffer from this pool completes;
// unsubmitted buffers are discarded. Reset invalidates all previously returned GPUCommandBuffer handles. Use one pool
// per worker and in-flight frame for independent recording/reuse. Buffers from a pool must be submitted to the selected
// queue's family. A pool holds at most GPUDeviceLimits::maxCommandBuffersPerPool buffers per reset.
GPUCommandPool* GPUCommandPoolCreate(GPUDevice* device, uint32_t queueIndex);
void GPUCommandPoolDestroy(GPUCommandPool* pool);
void GPUCommandPoolReset(GPUCommandPool* pool);
GPUCommandBuffer* GPUCmdBegin(GPUCommandPool* pool);
void GPUCmdEnd(GPUCommandBuffer* commands);
// Submit any ended subset exactly once before pool reset. Order completion semaphore signal values across queues.
// queueIndex must be less than GPUDeviceCaps::queueCount.
void GPUDeviceSubmit(GPUDevice* device, const GPUSubmitDesc* desc, uint32_t queueIndex);

// ---------------------------------------------------------------------------------------------------------------------
// Command recording.
// ---------------------------------------------------------------------------------------------------------------------

void GPUCmdSetTextureDescriptorHeap(GPUCommandBuffer* commands, GPUTextureDescriptorHeap* heap);
void GPUCmdSetSamplerDescriptorHeap(GPUCommandBuffer* commands, GPUSamplerDescriptorHeap* heap);

void GPUCmdCopyMemory(GPUCommandBuffer* commands, GPUGpuRange source, GPUGpuRange destination);
// Depth/stencil copies require a general queue. Copy-only queues also require GPUDeviceCaps::copyTextureGranularity
// alignment.
void GPUCmdCopyMemoryToTexture(
    GPUCommandBuffer* commands,
    GPUGpuRange source,
    GPUTexture* destination,
    const GPUTextureCopyDesc* copy
);
void GPUCmdCopyTextureToMemory(
    GPUCommandBuffer* commands,
    GPUTexture* source,
    GPUGpuRange destination,
    const GPUTextureCopyDesc* copy
);

void GPUCmdBarrier(
    GPUCommandBuffer* commands,
    GPUStage before,
    GPUAccess beforeAccess,
    GPUStage after,
    GPUAccess afterAccess
);

// Up to GPUDeviceDesc::timestampQueryCount markers per command buffer. stage must map to a single GPU pipeline stage.
// Ignored, leaving the destination unchanged, when timestamps are disabled or the queue lacks profiling support.
// Destinations are ordinary CPU memory and must remain valid and distinct until GPUCommandPoolReadTimestamps retrieves
// the results.
void GPUCmdWriteTimestamp(GPUCommandBuffer* commands, uint64_t* cpuDestination, GPUStage stage);
// Retrieve unread timestamps from submitted buffers after every submission from this pool completes. Does not wait.
// Call before resetting the pool; reset discards unread results. Unsubmitted buffers are ignored.
void GPUCommandPoolReadTimestamps(GPUCommandPool* pool);

// Each segment needs matching attachments, load/store operations, and clear values, and its own begin/end pair.
// Submit the complete suspend/resume chain in order in one batch. No action or synchronization commands may occur
// between segments. Resuming skips load/clear operations; suspending defers store operations. Command-buffer bindings
// are not inherited.
void GPUCmdBeginRenderPass(GPUCommandBuffer* commands, const GPURenderingDesc* desc, GPURenderingFlags flags);
void GPUCmdEndRenderPass(GPUCommandBuffer* commands);

// GPUCmdBeginRenderPass resets a full render-area viewport and scissor and disables depth/stencil; these commands
// override those defaults until the next pass.
void GPUCmdSetViewport(GPUCommandBuffer* commands, const GPUViewport* viewport);
void GPUCmdSetScissor(GPUCommandBuffer* commands, const GPUScissor* scissor);
void GPUCmdSetDepthStencil(GPUCommandBuffer* commands, const GPUDepthStencilState* state);

void GPUCmdBindPSO(GPUCommandBuffer* commands, const GPUPSO* pso);

// root is a 16-byte-aligned GPU address, or NULL for shaders without root data. Retain its storage through GPU
// completion. Finish CPU writes before submission; wait for prior GPU users before overwriting. Synchronize GPU writes
// before consuming the root. Storage is application-owned; these commands do not copy root bytes or allocate memory.
void GPUCmdDraw(
    GPUCommandBuffer* commands,
    const void* root,
    uint32_t vertexCount,
    uint32_t instanceCount,
    uint32_t firstVertex,
    uint32_t firstInstance
);
void GPUCmdDrawIndexed(
    GPUCommandBuffer* commands,
    const void* root,
    GPUGpuRange indices,
    GPUIndexType type,
    uint32_t indexCount,
    uint32_t instanceCount,
    uint32_t firstIndex,
    int32_t vertexOffset,
    uint32_t firstInstance
);
void GPUCmdDrawIndirect(
    GPUCommandBuffer* commands,
    const void* root,
    GPUGpuRange arguments,
    uint32_t drawCount,
    uint32_t stride
);
void GPUCmdDrawIndexedIndirect(
    GPUCommandBuffer* commands,
    const void* root,
    GPUGpuRange indices,
    GPUIndexType type,
    GPUGpuRange arguments,
    uint32_t drawCount,
    uint32_t stride
);
void GPUCmdDispatch(GPUCommandBuffer* commands, const void* root, GPUExtent3 groupCount);
void GPUCmdDispatchIndirect(GPUCommandBuffer* commands, const void* root, GPUGpuRange arguments);
void GPUCmdDrawMeshlets(GPUCommandBuffer* commands, const void* root, GPUExtent3 groupCount);
// Requires GPUDeviceCaps::indirectMeshDraw. Direct task/mesh draws remain available on every supported device.
void GPUCmdDrawMeshletsIndirect(
    GPUCommandBuffer* commands,
    const void* root,
    GPUGpuRange arguments,
    uint32_t drawCount,
    uint32_t stride
);

// ---------------------------------------------------------------------------------------------------------------------
// Bump allocator. Linear allocation of root arguments and transient data from a mapped GPU heap.
// ---------------------------------------------------------------------------------------------------------------------

typedef struct GPUBumpAllocator {
    GPUGpuCpuRange storage;
    uint64_t offset;
} GPUBumpAllocator;

// Storage must be nonempty, expose at least one address, and align every exposed address to 16 bytes.
GPUBumpAllocator GPUBumpAllocatorCreate(GPUGpuCpuRange storage);
// The request must be nonzero. Reservations are rounded up to 16 bytes; an empty allocation reports exhausted storage.
GPUGpuCpuRange GPUBumpAllocatorAllocate(GPUBumpAllocator* allocator, uint64_t byteSize);
// Intended for concurrent bump allocation from worker threads. Successful concurrent calls return disjoint ranges.
// Every other operation, including GPUBumpAllocatorAllocate and GPUBumpAllocatorReset, requires exclusive access and
// must not run concurrently with either allocate.
GPUGpuCpuRange GPUBumpAllocatorAllocateAtomic(GPUBumpAllocator* allocator, uint64_t byteSize);
// Reset invalidates every previous allocation.
void GPUBumpAllocatorReset(GPUBumpAllocator* allocator);

// ---------------------------------------------------------------------------------------------------------------------
// Delete queue. Defers callbacks until a timeline value completes. Entry storage is caller-owned.
// ---------------------------------------------------------------------------------------------------------------------

typedef void (*GPUDeleteCallback)(void* user);

typedef struct GPUDeleteEntry {
    uint64_t retireValue;
    GPUDeleteCallback callback;
    void* user;
} GPUDeleteEntry;

typedef struct GPUDeleteQueue {
    GPUTimelineSemaphore* timeline;
    GPUDeleteEntry* entries;
    uint32_t capacity;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    bool ticking;
} GPUDeleteQueue;

// capacity must be nonzero and entries must hold capacity elements for the queue's lifetime. The timeline must be live
// when GPUDeleteQueueTick is called.
GPUDeleteQueue GPUDeleteQueueCreate(GPUTimelineSemaphore* timeline, GPUDeleteEntry* entries, uint32_t capacity);
// Retire values must be enqueued in nondecreasing order. Asserts when the queue is full.
void GPUDeleteQueueDefer(GPUDeleteQueue* queue, uint64_t retireValue, GPUDeleteCallback callback, void* user);
// Runs every callback whose retire value has completed. Callbacks cannot recursively tick or drain the same queue.
void GPUDeleteQueueTick(GPUDeleteQueue* queue);
// Call after GPUDeviceWaitIdle and before GPUDeviceDestroy; runs every queued callback without checking the timeline.
void GPUDeleteQueueDrain(GPUDeleteQueue* queue);

#ifdef __cplusplus
}
#endif

#endif // GPU_H_INCLUDED
