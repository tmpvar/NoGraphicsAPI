// ---- gpu_api.h ------------------------------------------------------------------------------------------------------
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


#if defined(GPU_IMPLEMENTATION)

// ---- gpu_internal.h -------------------------------------------------------------------------------------------------
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

// ---- gpu_common.c ---------------------------------------------------------------------------------------------------



static void GPU_Report(const GPUDeviceState* device, const GPUMessage* message) {
    if (device && device->errorCallback) device->errorCallback(device->errorUser, message);
}

static void GPU_ReportKind(const GPUDeviceState* device, GPUMessageKind kind) {
    const GPUMessage message = {.kind = kind, .fatal = false};
    GPU_Report(device, &message);
}

static void GPU_ReportLimit(const GPUDeviceState* device, GPULimit limit) {
    const GPUMessage message = {.kind = GPUMessageKind_LimitReached, .fatal = false, .data.limit = {.limit = limit}};
    GPU_Report(device, &message);
}

static void GPU_ReportVkFailure(
    const GPUDeviceState* device,
    VkResult result,
    const char* operation,
    bool fatal,
    uint64_t byteCount
) {
    const GPUMessage message = {
        .kind = GPUMessageKind_VulkanFailure,
        .fatal = fatal,
        .data.vulkan = {.operation = operation, .result = (int32_t)result, .byteCount = byteCount},
    };
    GPU_Report(device, &message);
}

static GPU_NORETURN void GPU_AbortVkFailure(
    const GPUDeviceState* device,
    VkResult result,
    const char* operation,
    uint64_t byteCount
) {
    GPU_ReportVkFailure(device, result, operation, true, byteCount);
    GPUAssert(result == VK_SUCCESS, "unexpected Vulkan failure");
    abort();
}

static GPUError GPU_ErrorFromVk(
    const GPUDeviceState* device,
    VkResult result,
    const char* operation,
    uint64_t byteCount
) {
    if (result != VK_SUCCESS) GPU_ReportVkFailure(device, result, operation, false, byteCount);
    switch (result) {
    case VK_SUCCESS: return GPUError_None;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    case VK_ERROR_TOO_MANY_OBJECTS: return GPUError_OutOfMemory;
    case VK_ERROR_DEVICE_LOST: return GPUError_DeviceLost;
    case VK_ERROR_LAYER_NOT_PRESENT:
    case VK_ERROR_EXTENSION_NOT_PRESENT:
    case VK_ERROR_FEATURE_NOT_PRESENT:
    case VK_ERROR_INCOMPATIBLE_DRIVER:
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return GPUError_Unsupported;
    default: return GPUError_DriverError;
    }
}

static void GPU_RequireVk(const GPUDeviceState* device, VkResult result, const char* operation, uint64_t byteCount) {
    if (result != VK_SUCCESS) GPU_AbortVkFailure(device, result, operation, byteCount);
}

static void GPU_AssertVk(VkResult result) {
    GPUAssert(result == VK_SUCCESS, "unexpected Vulkan failure");
    (void)result;
}

static void GPU_RequireError(const GPUDeviceState* device, GPUError error, const char* operation) {
    if (error != GPUError_None) {
        const GPUMessage message = {
            .kind = GPUMessageKind_GraphicsFailure,
            .fatal = true,
            .data.graphics = {.operation = operation, .error = error}
        };
        GPU_Report(device, &message);
        GPUAssert(false, "unexpected graphics API failure");
        abort();
    }
}

static const char* GPU_CommonVkResultDescription(int32_t result) {
    switch (result) {
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED (graphics initialization failed)";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY (not enough system memory)";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY (not enough GPU memory or CPU-visible GPU memory)";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS (graphics resource limit reached)";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST (graphics device lost)";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED (GPU memory could not be mapped for CPU access)";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT (required Vulkan layer unavailable)";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT (required Vulkan extension unavailable)";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT (required graphics feature unavailable)";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER (incompatible graphics driver)";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED (required texture or display format unsupported)";
    case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV (the graphics driver rejected the shader)";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN (unspecified graphics driver error)";
    default: return "graphics driver error";
    }
}

static const char* GPU_CommonLimitName(GPULimit limit) {
    switch (limit) {
    case GPULimit_TimelineSemaphores: return "maxTimelineSemaphores";
    case GPULimit_GpuHeaps: return "maxGpuHeaps";
    case GPULimit_TextureHeaps: return "maxTextureHeaps";
    case GPULimit_Textures: return "maxTextures";
    case GPULimit_RenderViews: return "maxRenderViews";
    case GPULimit_TextureDescriptorHeaps: return "maxTextureDescriptorHeaps";
    case GPULimit_SamplerDescriptorHeaps: return "maxSamplerDescriptorHeaps";
    case GPULimit_Psos: return "maxPsos";
    case GPULimit_CommandPools: return "maxCommandPools";
    }
    return "unknown";
}

int GPUMessageFormat(const GPUMessage* message, char* buffer, uint64_t size) {
    GPUAssert(message && buffer && size != 0, "GPUMessageFormat requires a message and a nonempty buffer");
    switch (message->kind) {
    case GPUMessageKind_VulkanFailure:
        if (message->data.vulkan.byteCount)
            return GPU_SNPRINTF(
                buffer,
                size,
                "%s failed: %s (Vulkan result %d, %.2f MiB requested).",
                message->data.vulkan.operation,
                GPU_CommonVkResultDescription(message->data.vulkan.result),
                (int)message->data.vulkan.result,
                (double)message->data.vulkan.byteCount / (1024.0 * 1024.0)
            );
        return GPU_SNPRINTF(
            buffer,
            size,
            "%s failed: %s (Vulkan result %d).",
            message->data.vulkan.operation,
            GPU_CommonVkResultDescription(message->data.vulkan.result),
            (int)message->data.vulkan.result
        );
    case GPUMessageKind_GraphicsFailure:
        return GPU_SNPRINTF(
            buffer,
            size,
            "%s failed (graphics error %u).",
            message->data.graphics.operation,
            (uint32_t)message->data.graphics.error
        );
    case GPUMessageKind_NoMemoryHeap:
        return GPU_SNPRINTF(
            buffer,
            size,
            "No compatible %s GPU memory heap can hold %.2f MiB.",
            message->data.memoryHeap.cpuVisible ? "CPU-visible" : "device-local",
            (double)message->data.memoryHeap.byteCount / (1024.0 * 1024.0)
        );
    case GPUMessageKind_LimitReached:
        return GPU_SNPRINTF(
            buffer,
            size,
            "GPUDeviceLimits::%s exceeded.",
            GPU_CommonLimitName(message->data.limit.limit)
        );
    case GPUMessageKind_LoaderTooOld:
        return GPU_SNPRINTF(buffer, size, "Vulkan 1.4 or newer is required. Update the graphics driver.");
    case GPUMessageKind_LoaderUnavailable:
        return GPU_SNPRINTF(
            buffer,
            size,
            "The Vulkan loader library could not be opened. Install a graphics driver with Vulkan support."
        );
    case GPUMessageKind_NoCompatibleGpu:
        return GPU_SNPRINTF(
            buffer,
            size,
            "No compatible GPU found. Vulkan 1.4, VK_EXT_descriptor_heap, VK_KHR_device_address_commands, "
            "VK_EXT_mesh_shader, "
            "VK_KHR_shader_untyped_pointers, and the required memory, shader, and presentation features are needed."
        );
    case GPUMessageKind_DeviceUnsupported:
        return GPU_SNPRINTF(buffer, size, "Required Vulkan device, memory, or display capabilities are unavailable.");
    case GPUMessageKind_MissingEntryPoints:
        return GPU_SNPRINTF(
            buffer,
            size,
            "The graphics driver did not provide the required Vulkan command entry points."
        );
    case GPUMessageKind_TextureUnsupported:
        return GPU_SNPRINTF(buffer, size, "The GPU cannot create the requested texture format, usage, or extent.");
    case GPUMessageKind_SurfaceCompositeAlphaUnsupported:
        return GPU_SNPRINTF(buffer, size, "The display surface exposes no supported composite alpha mode.");
    case GPUMessageKind_SurfaceExtentUnavailable:
        return GPU_SNPRINTF(buffer, size, "The display surface does not provide a fixed drawable extent.");
    }
    return GPU_SNPRINTF(buffer, size, "Unknown graphics message %d.", (int)message->kind);
}

static bool GPU_HasExtension(const VkExtensionProperties* values, uint32_t count, const char* name) {
    for (uint32_t index = 0; index < count; ++index) {
        if (strcmp(values[index].extensionName, name) == 0) return true;
    }
    return false;
}

#if defined(GPU_DEBUG)
static bool GPU_HasLayer(const VkLayerProperties* values, uint32_t count, const char* name) {
    for (uint32_t index = 0; index < count; ++index) {
        if (strcmp(values[index].layerName, name) == 0) return true;
    }
    return false;
}
#endif

#if defined(GPU_DEBUG)
static VKAPI_ATTR VkBool32 VKAPI_CALL GPU_CommonDebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData
) {
    (void)types;
    (void)userData;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && callbackData && callbackData->pMessage) {
        // Keep the library callback dependency-free. Applications can still install
        // their own messenger; this one makes validation failures debugger-visible.
        fputs("NoGraphicsAPI validation: ", stderr);
        fputs(callbackData->pMessage, stderr);
        fputc('\n', stderr);
    }
    return VK_FALSE;
}
#endif

static void GPU_PoolInit(GPUPool* pool, uint8_t* storage, uint32_t slotSize, uint32_t capacity) {
    GPUAssert(
        pool && storage && slotSize >= 16 && (slotSize % 16) == 0,
        "pool && storage && slotSize >= 16 && (slotSize % 16) == 0"
    );
    pool->slots = storage;
    pool->slotSize = slotSize;
    pool->capacity = capacity;
    pool->freeHead = 0;
    pool->liveCount = 0;
    pool->lock = 0;
    for (uint32_t index = 0; index < capacity; ++index) {
        const uint32_t next = index + 1;
        memcpy(storage + (uint64_t)index * slotSize, &next, sizeof(next));
    }
}

static void GPU_CommonPoolLock(GPUPool* pool) {
    while (GPU_AtomicExchangeU32(&pool->lock, 1) == 1) {}
}

static void* GPU_PoolAlloc(GPUPool* pool) {
    GPU_CommonPoolLock(pool);
    if (pool->freeHead == pool->capacity) {
        GPU_AtomicStoreU32(&pool->lock, 0);
        return NULL;
    }
    uint8_t* slot = pool->slots + (uint64_t)pool->freeHead * pool->slotSize;
    memcpy(&pool->freeHead, slot, sizeof(uint32_t));
    ++pool->liveCount;
    GPU_AtomicStoreU32(&pool->lock, 0);
    memset(slot, 0, pool->slotSize);
    return slot;
}

static void GPU_PoolFree(GPUPool* pool, void* slot) {
    const uint64_t offset = (uint64_t)((uint8_t*)slot - pool->slots);
    GPUAssert(
        (uint8_t*)slot >= pool->slots && offset < (uint64_t)pool->capacity * pool->slotSize &&
            (offset % pool->slotSize) == 0,
        "slot does not belong to this pool"
    );
    GPU_CommonPoolLock(pool);
    memcpy(slot, &pool->freeHead, sizeof(uint32_t));
    pool->freeHead = (uint32_t)(offset / pool->slotSize);
    --pool->liveCount;
    GPU_AtomicStoreU32(&pool->lock, 0);
}

static VkFormat GPU_ToVkFormat(GPUFormat format) {
    switch (format) {
    case GPUFormat_R8Srgb: return VK_FORMAT_R8_SRGB;
    case GPUFormat_Rg8Srgb: return VK_FORMAT_R8G8_SRGB;
    case GPUFormat_Rgba8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case GPUFormat_Bgra8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
    case GPUFormat_Rgba4Unorm: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case GPUFormat_R5g5b5a1Unorm: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case GPUFormat_R5g6b5Unorm: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case GPUFormat_R8Unorm: return VK_FORMAT_R8_UNORM;
    case GPUFormat_Rg8Unorm: return VK_FORMAT_R8G8_UNORM;
    case GPUFormat_Rgba8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case GPUFormat_Bgra8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case GPUFormat_R16Unorm: return VK_FORMAT_R16_UNORM;
    case GPUFormat_Rg16Unorm: return VK_FORMAT_R16G16_UNORM;
    case GPUFormat_Rgba16Unorm: return VK_FORMAT_R16G16B16A16_UNORM;
    case GPUFormat_R8Uint: return VK_FORMAT_R8_UINT;
    case GPUFormat_Rg8Uint: return VK_FORMAT_R8G8_UINT;
    case GPUFormat_Rgba8Uint: return VK_FORMAT_R8G8B8A8_UINT;
    case GPUFormat_Bgra8Uint: return VK_FORMAT_B8G8R8A8_UINT;
    case GPUFormat_R16Uint: return VK_FORMAT_R16_UINT;
    case GPUFormat_Rg16Uint: return VK_FORMAT_R16G16_UINT;
    case GPUFormat_Rgba16Uint: return VK_FORMAT_R16G16B16A16_UINT;
    case GPUFormat_R32Uint: return VK_FORMAT_R32_UINT;
    case GPUFormat_Rg32Uint: return VK_FORMAT_R32G32_UINT;
    case GPUFormat_Rgba32Uint: return VK_FORMAT_R32G32B32A32_UINT;
    case GPUFormat_R16Float: return VK_FORMAT_R16_SFLOAT;
    case GPUFormat_Rg16Float: return VK_FORMAT_R16G16_SFLOAT;
    case GPUFormat_Rgba16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case GPUFormat_R32Float: return VK_FORMAT_R32_SFLOAT;
    case GPUFormat_Rg32Float: return VK_FORMAT_R32G32_SFLOAT;
    case GPUFormat_Rgba32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case GPUFormat_Rgb10a2Unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case GPUFormat_Rg11b10Float: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case GPUFormat_D16Unorm: return VK_FORMAT_D16_UNORM;
    case GPUFormat_D24UnormS8Uint: return VK_FORMAT_D24_UNORM_S8_UINT;
    case GPUFormat_D32Float: return VK_FORMAT_D32_SFLOAT;
    case GPUFormat_S8Uint: return VK_FORMAT_S8_UINT;
    case GPUFormat_D32FloatS8Uint: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case GPUFormat_EacRg: return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
    case GPUFormat_Astc4x4Srgb: return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
    case GPUFormat_Astc4x4Unorm: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case GPUFormat_Bc3Srgb: return VK_FORMAT_BC3_SRGB_BLOCK;
    case GPUFormat_Bc3Unorm: return VK_FORMAT_BC3_UNORM_BLOCK;
    case GPUFormat_Bc5Rg: return VK_FORMAT_BC5_UNORM_BLOCK;
    case GPUFormat_Bc6hUfloat: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case GPUFormat_Bc6hSfloat: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case GPUFormat_Bc7Srgb: return VK_FORMAT_BC7_SRGB_BLOCK;
    case GPUFormat_Bc7Unorm: return VK_FORMAT_BC7_UNORM_BLOCK;
    case GPUFormat_Count:
    case GPUFormat_Undefined: return VK_FORMAT_UNDEFINED;
    }
    return VK_FORMAT_UNDEFINED;
}

static bool GPU_CompatibleViewFormats(GPUFormat imageFormat, GPUFormat viewFormat) {
    if (imageFormat == viewFormat) return true;
    const GPUTextureFormatInfo image = GPUFormatGetTextureInfo(imageFormat);
    const GPUTextureFormatInfo view = GPUFormatGetTextureInfo(viewFormat);
    if (image.depth || image.stencil || view.depth || view.stencil) return false;
    if (image.blockExtent.x == 1 && view.blockExtent.x == 1)
        return image.bytesPerBlock != 0 && image.bytesPerBlock == view.bytesPerBlock;
    return (imageFormat == GPUFormat_Astc4x4Unorm && viewFormat == GPUFormat_Astc4x4Srgb) ||
           (imageFormat == GPUFormat_Astc4x4Srgb && viewFormat == GPUFormat_Astc4x4Unorm) ||
           (imageFormat == GPUFormat_Bc3Unorm && viewFormat == GPUFormat_Bc3Srgb) ||
           (imageFormat == GPUFormat_Bc3Srgb && viewFormat == GPUFormat_Bc3Unorm) ||
           (imageFormat == GPUFormat_Bc6hUfloat && viewFormat == GPUFormat_Bc6hSfloat) ||
           (imageFormat == GPUFormat_Bc6hSfloat && viewFormat == GPUFormat_Bc6hUfloat) ||
           (imageFormat == GPUFormat_Bc7Unorm && viewFormat == GPUFormat_Bc7Srgb) ||
           (imageFormat == GPUFormat_Bc7Srgb && viewFormat == GPUFormat_Bc7Unorm);
}

static bool GPU_HasDepthAspect(GPUFormat format) { return GPUFormatGetTextureInfo(format).depth; }

static bool GPU_HasStencilAspect(GPUFormat format) { return GPUFormatGetTextureInfo(format).stencil; }

static VkImageAspectFlags GPU_ImageAspects(GPUFormat format) {
    VkImageAspectFlags result = 0;
    if (GPU_HasDepthAspect(format)) result |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (GPU_HasStencilAspect(format)) result |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (result == 0) result = VK_IMAGE_ASPECT_COLOR_BIT;
    return result;
}

static VkImageType GPU_ToVkImageType(GPUTextureType type) {
    switch (type) {
    case GPUTextureType_1D: return VK_IMAGE_TYPE_1D;
    case GPUTextureType_3D: return VK_IMAGE_TYPE_3D;
    case GPUTextureType_Undefined:
    case GPUTextureType_2D:
    case GPUTextureType_Cube:
    case GPUTextureType_2DArray:
    case GPUTextureType_CubeArray: return VK_IMAGE_TYPE_2D;
    }
    return VK_IMAGE_TYPE_MAX_ENUM;
}

static VkImageViewType GPU_ToVkImageViewType(GPUTextureType type) {
    switch (type) {
    case GPUTextureType_1D: return VK_IMAGE_VIEW_TYPE_1D;
    case GPUTextureType_Undefined:
    case GPUTextureType_2D: return VK_IMAGE_VIEW_TYPE_2D;
    case GPUTextureType_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case GPUTextureType_Cube: return VK_IMAGE_VIEW_TYPE_CUBE;
    case GPUTextureType_2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case GPUTextureType_CubeArray: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    }
    return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
}

static VkFormatFeatureFlags2 GPU_RequiredFormatFeatures(GPUTextureUsage usage) {
    VkFormatFeatureFlags2 result = 0;
    if (usage & GPUTextureUsage_Sampled) result |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
    if (usage & GPUTextureUsage_Storage)
        result |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
    if (usage & GPUTextureUsage_ColorAttachment) result |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;
    if (usage & GPUTextureUsage_DepthStencilAttachment) result |= VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (usage & GPUTextureUsage_TransferSource) result |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
    if (usage & GPUTextureUsage_TransferDestination) result |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
    return result;
}

#define GPU_COMMON_KNOWN_TEXTURE_USAGE_BITS                                                \
    (GPUTextureUsage_Sampled | GPUTextureUsage_Storage | GPUTextureUsage_ColorAttachment | \
     GPUTextureUsage_DepthStencilAttachment | GPUTextureUsage_TransferSource | GPUTextureUsage_TransferDestination)

#if defined(GPU_DEBUG)
static bool GPU_HasValidTextureUsageBits(GPUTextureUsage usage) {
    return usage != 0 && (usage & ~GPU_COMMON_KNOWN_TEXTURE_USAGE_BITS) == 0;
}
#endif

static GPUTextureCompression GPU_TextureCompression(GPUFormat format) {
    switch (format) {
    case GPUFormat_Bc3Srgb:
    case GPUFormat_Bc3Unorm:
    case GPUFormat_Bc5Rg:
    case GPUFormat_Bc6hUfloat:
    case GPUFormat_Bc6hSfloat:
    case GPUFormat_Bc7Srgb:
    case GPUFormat_Bc7Unorm: return GPUTextureCompression_Bc;
    case GPUFormat_Astc4x4Srgb:
    case GPUFormat_Astc4x4Unorm: return GPUTextureCompression_Astc;
    case GPUFormat_EacRg: return GPUTextureCompression_Etc2;
    default: return GPUTextureCompression_None;
    }
}

static VkFormatFeatureFlags2 GPU_OptimalFormatFeatures(const GPUDeviceState* state, GPUFormat format) {
    VkFormatProperties3 properties3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 properties2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &properties3};
    state->fn.vkGetPhysicalDeviceFormatProperties2(state->physicalDevice, GPU_ToVkFormat(format), &properties2);
    return properties3.optimalTilingFeatures;
}

static VkBlendFactor GPU_ToVkBlendFactor(GPUBlendFactor factor) {
    switch (factor) {
    case GPUBlendFactor_Zero: return VK_BLEND_FACTOR_ZERO;
    case GPUBlendFactor_One: return VK_BLEND_FACTOR_ONE;
    case GPUBlendFactor_SourceColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case GPUBlendFactor_OneMinusSourceColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GPUBlendFactor_DestinationColor: return VK_BLEND_FACTOR_DST_COLOR;
    case GPUBlendFactor_OneMinusDestinationColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case GPUBlendFactor_SourceAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case GPUBlendFactor_OneMinusSourceAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GPUBlendFactor_DestinationAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case GPUBlendFactor_OneMinusDestinationAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case GPUBlendFactor_SourceAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    }
    return VK_BLEND_FACTOR_MAX_ENUM;
}

static VkPipelineStageFlags2 GPU_ToVkStages(GPUStage stages) {
    VkPipelineStageFlags2 result = 0;
    if (stages & GPUStage_Indirect) result |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    if (stages & GPUStage_IndexInput) result |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    if (stages & GPUStage_Vertex) result |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    if (stages & GPUStage_Task) result |= VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT;
    if (stages & GPUStage_Mesh) result |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
    if (stages & GPUStage_DepthStencilTests)
        result |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    if (stages & GPUStage_Fragment) result |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (stages & GPUStage_ColorOutput) result |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (stages & GPUStage_Compute) result |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (stages & GPUStage_Transfer) result |= VK_PIPELINE_STAGE_2_COPY_BIT;
    if (stages & GPUStage_Host) result |= VK_PIPELINE_STAGE_2_HOST_BIT;
    if (stages & GPUStage_AllCommands) result |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    return result;
}

static VkAccessFlags2 GPU_ToVkAccess(GPUAccess accesses) {
    VkAccessFlags2 result = 0;
    if (accesses & GPUAccess_TransferRead) result |= VK_ACCESS_2_TRANSFER_READ_BIT;
    if (accesses & GPUAccess_TransferWrite) result |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (accesses & GPUAccess_ShaderRead)
        result |=
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;
    if (accesses & GPUAccess_ShaderWrite) result |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if (accesses & GPUAccess_ColorRead) result |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    if (accesses & GPUAccess_ColorWrite) result |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if (accesses & GPUAccess_DepthStencilRead) result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    if (accesses & GPUAccess_DepthStencilWrite) result |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (accesses & GPUAccess_IndirectRead) result |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    if (accesses & GPUAccess_IndexRead) result |= VK_ACCESS_2_INDEX_READ_BIT;
    if (accesses & GPUAccess_HostRead) result |= VK_ACCESS_2_HOST_READ_BIT;
    if (accesses & GPUAccess_DescriptorRead)
        result |= VK_ACCESS_2_SAMPLER_HEAP_READ_BIT_EXT | VK_ACCESS_2_RESOURCE_HEAP_READ_BIT_EXT;
    return result;
}

// GPU_ToVkCompareOp subtracts the Undefined slot.
GPUStaticAssert(
    GPUCompareOp_Never - 1 == (int)VK_COMPARE_OP_NEVER && GPUCompareOp_Less - 1 == (int)VK_COMPARE_OP_LESS &&
        GPUCompareOp_Equal - 1 == (int)VK_COMPARE_OP_EQUAL &&
        GPUCompareOp_LessEqual - 1 == (int)VK_COMPARE_OP_LESS_OR_EQUAL &&
        GPUCompareOp_Greater - 1 == (int)VK_COMPARE_OP_GREATER &&
        GPUCompareOp_NotEqual - 1 == (int)VK_COMPARE_OP_NOT_EQUAL &&
        GPUCompareOp_GreaterEqual - 1 == (int)VK_COMPARE_OP_GREATER_OR_EQUAL &&
        GPUCompareOp_Always - 1 == (int)VK_COMPARE_OP_ALWAYS,
    compare_op_order
);

static VkCompareOp GPU_ToVkCompareOp(GPUCompareOp op) {
    GPUAssert(op != GPUCompareOp_Undefined, "compare op must be resolved before conversion");
    return (VkCompareOp)(op - 1);
}

static VkFilter GPU_ToVkFilter(GPUFilter filter) {
    return filter == GPUFilter_Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

static VkSamplerMipmapMode GPU_ToVkMipmapMode(GPUFilter filter) {
    return filter == GPUFilter_Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

static bool GPU_IsUsableMemoryType(const VkPhysicalDeviceMemoryProperties* properties, uint32_t index) {
    const VkMemoryType* type = &properties->memoryTypes[index];
    if ((type->propertyFlags & GPU_FORBIDDEN_MEMORY_PROPERTIES) != 0) return false;
    return (properties->memoryHeaps[type->heapIndex].flags & VK_MEMORY_HEAP_TILE_MEMORY_BIT_QCOM) == 0;
}

static void GPU_RecordImageBarriers(
    const GPUDeviceState* state,
    VkCommandBuffer commandBuffer,
    const VkImageMemoryBarrier2* barriers,
    uint32_t count
) {
    GPUAssert(commandBuffer && barriers && count != 0, "commandBuffer && barriers && count != 0");
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = count,
        .pImageMemoryBarriers = barriers
    };
    state->fn.vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

static void GPU_RecordBarrier(
    const GPUDeviceState* state,
    VkCommandBuffer commandBuffer,
    GPUStage before,
    GPUAccess beforeAccess,
    GPUStage after,
    GPUAccess afterAccess
) {
    const VkMemoryBarrier2 memoryBarrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = GPU_ToVkStages(before),
        .srcAccessMask = GPU_ToVkAccess(beforeAccess),
        .dstStageMask = GPU_ToVkStages(after),
        .dstAccessMask = GPU_ToVkAccess(afterAccess),
    };
    const VkDependencyInfo dependency =
        {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &memoryBarrier};
    state->fn.vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

static void GPU_MakeHeapBindInfo(
    GPUGpuRange heap,
    VkDeviceSize reservedAlignment,
    VkDeviceSize reservedSize,
    VkBindHeapInfoEXT* output
) {
    const VkDeviceSize reservedOffset = GPU_AlignUp(heap.size, reservedAlignment);
    *output = (VkBindHeapInfoEXT){
        .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .heapRange = {.address = (VkDeviceAddress)(uintptr_t)heap.gpu, .size = reservedOffset + reservedSize},
        .reservedRangeOffset = reservedOffset,
        .reservedRangeSize = reservedSize,
    };
}

static VkDeviceMemoryImageCopyKHR GPU_MakeTextureCopyRegion(
    const GPUTexture* texture,
    const GPUTextureCopyDesc* copy,
    GPUGpuRange memory
) {
    uint32_t mipWidth = texture->width >> copy->mipLevel;
    uint32_t mipHeight = texture->height >> copy->mipLevel;
    uint32_t mipDepth = texture->depth >> copy->mipLevel;
    if (mipWidth == 0) mipWidth = 1;
    if (mipHeight == 0) mipHeight = 1;
    if (mipDepth == 0) mipDepth = 1;

    const uint32_t width = copy->extent.x == 0 ? mipWidth - copy->offset.x : copy->extent.x;
    const GPUTextureFormatInfo formatInfo = GPUFormatGetTextureInfo(texture->format);
    const uint64_t rowPitch = copy->rowPitchBytes == 0
                                  ? GPU_DivideUp(width, formatInfo.blockExtent.x) * formatInfo.bytesPerBlock
                                  : copy->rowPitchBytes;
    GPUAssert(copy->slicePitchBytes == 0 || rowPitch != 0, "slice pitch conversion requires a non-zero row pitch");
    return (VkDeviceMemoryImageCopyKHR){
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_IMAGE_COPY_KHR,
        .addressRange = {.address = (VkDeviceAddress)(uintptr_t)memory.gpu, .size = memory.size},
        .addressFlags = GPU_ADDRESS_FLAGS,
        .addressRowLength = (uint32_t)(copy->rowPitchBytes / formatInfo.bytesPerBlock * formatInfo.blockExtent.x),
        .addressImageHeight =
            copy->slicePitchBytes == 0 ? 0u : (uint32_t)(copy->slicePitchBytes / rowPitch * formatInfo.blockExtent.y),
        .imageSubresource =
            {
                .aspectMask = GPU_ImageAspects(texture->format),
                .mipLevel = copy->mipLevel,
                .baseArrayLayer = copy->baseSlice,
                .layerCount = copy->sliceCount == 0 ? texture->layerCount - copy->baseSlice : copy->sliceCount,
            },
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .imageOffset = {.x = (int32_t)copy->offset.x, .y = (int32_t)copy->offset.y, .z = (int32_t)copy->offset.z},
        .imageExtent = {
            .width = width,
            .height = copy->extent.y == 0 ? mipHeight - copy->offset.y : copy->extent.y,
            .depth = copy->extent.z == 0 ? mipDepth - copy->offset.z : copy->extent.z,
        },
    };
}

// ---- gpu_loader.c ---------------------------------------------------------------------------------------------------
// Opens the Vulkan loader library at run time and fills the device's entry point table, so the library and its users
// link against nothing but the C runtime. The module handle and every pointer live in the device state, never in
// statics.

#if defined(_WIN32)
// Declared with the SDK's exact prototypes so this coexists with windows.h in any order.
#if defined(_WIN64)
typedef long long(__stdcall* GPU_Win32FarProc)(void);
#else
typedef int(__stdcall* GPU_Win32FarProc)(void);
#endif
__declspec(dllimport) struct HINSTANCE__* __stdcall LoadLibraryA(const char* name);
__declspec(dllimport) GPU_Win32FarProc __stdcall GetProcAddress(struct HINSTANCE__* module, const char* name);
__declspec(dllimport) int __stdcall FreeLibrary(struct HINSTANCE__* module);
#if defined(_MSC_VER)
#pragma comment(lib, "kernel32.lib")
#endif

static void* GPU_LoaderOpenModule(void) { return (void*)LoadLibraryA("vulkan-1.dll"); }
static void GPU_LoaderCloseModule(void* module) { FreeLibrary((struct HINSTANCE__*)module); }
static void* GPU_LoaderSymbol(void* module, const char* name) {
    return (void*)GetProcAddress((struct HINSTANCE__*)module, name);
}
#else
#include <dlfcn.h>

static void* GPU_LoaderOpenModule(void) {
#if defined(__APPLE__)
    const char* const names[] = {"libvulkan.1.dylib", "libvulkan.dylib", "libMoltenVK.dylib"};
#else
    const char* const names[] = {"libvulkan.so.1", "libvulkan.so"};
#endif
    for (uint32_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        void* module = dlopen(names[index], RTLD_NOW | RTLD_LOCAL);
        if (module) return module;
    }
    return NULL;
}
static void GPU_LoaderCloseModule(void* module) { dlclose(module); }
static void* GPU_LoaderSymbol(void* module, const char* name) { return dlsym(module, name); }
#endif

// Tests define GPU_VULKAN_FUNCTIONS_HOOK(functions) to wrap entry points after each loading phase.
#if !defined(GPU_VULKAN_FUNCTIONS_HOOK)
#define GPU_VULKAN_FUNCTIONS_HOOK(functions) ((void)0)
#endif

// Opens the loader and fetches the entry points that exist before an instance.
static GPUError GPU_LoaderOpen(GPUDeviceState* state) {
    state->loaderModule = GPU_LoaderOpenModule();
    if (state->loaderModule) {
        state->fn.vkGetInstanceProcAddr =
            (PFN_vkGetInstanceProcAddr)GPU_LoaderSymbol(state->loaderModule, "vkGetInstanceProcAddr");
    }
    if (!state->fn.vkGetInstanceProcAddr) {
        GPU_ReportKind(state, GPUMessageKind_LoaderUnavailable);
        return GPUError_Unsupported;
    }
    bool complete = true;
#define GPU_VK_LOAD(name)                                                                \
    state->fn.name = (PFN_##name)state->fn.vkGetInstanceProcAddr(VK_NULL_HANDLE, #name); \
    complete = complete && state->fn.name != NULL;
    GPU_VK_GLOBAL_FUNCTIONS(GPU_VK_LOAD)
#undef GPU_VK_LOAD
    if (!complete) {
        GPU_ReportKind(state, GPUMessageKind_MissingEntryPoints);
        return GPUError_DriverError;
    }
    GPU_VULKAN_FUNCTIONS_HOOK(&state->fn);
    return GPUError_None;
}

// Fetches the instance-level entry points after vkCreateInstance.
static GPUError GPU_LoaderLoadInstanceFunctions(GPUDeviceState* state) {
    bool complete = true;
#define GPU_VK_LOAD(name)                                                                 \
    state->fn.name = (PFN_##name)state->fn.vkGetInstanceProcAddr(state->instance, #name); \
    complete = complete && state->fn.name != NULL;
#define GPU_VK_LOAD_OPTIONAL(name) state->fn.name = (PFN_##name)state->fn.vkGetInstanceProcAddr(state->instance, #name);
    GPU_VK_INSTANCE_FUNCTIONS(GPU_VK_LOAD)
    GPU_VK_INSTANCE_OPTIONAL_FUNCTIONS(GPU_VK_LOAD_OPTIONAL)
#undef GPU_VK_LOAD
#undef GPU_VK_LOAD_OPTIONAL
    if (!complete) {
        GPU_ReportKind(state, GPUMessageKind_MissingEntryPoints);
        return GPUError_DriverError;
    }
    GPU_VULKAN_FUNCTIONS_HOOK(&state->fn);
    return GPUError_None;
}

// Fetches the device-level entry points after vkCreateDevice, skipping the loader's dispatch trampolines.
static GPUError GPU_LoaderLoadDeviceFunctions(GPUDeviceState* state) {
    bool complete = true;
#define GPU_VK_LOAD(name)                                                             \
    state->fn.name = (PFN_##name)state->fn.vkGetDeviceProcAddr(state->device, #name); \
    complete = complete && state->fn.name != NULL;
#define GPU_VK_LOAD_OPTIONAL(name) state->fn.name = (PFN_##name)state->fn.vkGetDeviceProcAddr(state->device, #name);
    GPU_VK_DEVICE_FUNCTIONS(GPU_VK_LOAD)
    GPU_VK_DEVICE_OPTIONAL_FUNCTIONS(GPU_VK_LOAD_OPTIONAL)
#undef GPU_VK_LOAD
#undef GPU_VK_LOAD_OPTIONAL
    if (!complete) {
        GPU_ReportKind(state, GPUMessageKind_MissingEntryPoints);
        return GPUError_DriverError;
    }
    GPU_VULKAN_FUNCTIONS_HOOK(&state->fn);
    return GPUError_None;
}

static void GPU_LoaderClose(GPUDeviceState* state) {
    if (state->loaderModule) GPU_LoaderCloseModule(state->loaderModule);
    state->loaderModule = NULL;
}

// ---- gpu_device.c ---------------------------------------------------------------------------------------------------



static uint32_t GPU_DevicePopcount(uint32_t value) {
    // Keep the core library usable on x86-64 CPUs without POPCNT.
    value -= (value >> 1) & 0x55555555u;
    value = (value & 0x33333333u) + ((value >> 2) & 0x33333333u);
    value = (value + (value >> 4)) & 0x0f0f0f0fu;
    return (value * 0x01010101u) >> 24;
}

static uint32_t GPU_DeviceCountLeadingZeros(uint32_t value) {
    if (value == 0) return 32;
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long index;
    _BitScanReverse(&index, value);
    return 31u - index;
#else
    return (uint32_t)__builtin_clz(value);
#endif
}

static bool GPU_FindMemoryType(
    const GPUDeviceState* device,
    uint32_t bits,
    VkMemoryPropertyFlags required,
    VkMemoryPropertyFlags preferred,
    VkDeviceSize minimumHeapSize,
    uint32_t* output,
    VkMemoryPropertyFlags avoided
) {
    const VkPhysicalDeviceMemoryProperties* properties = &device->memoryProperties;
    bool hasBest = false;
    bool bestIsAvoided = false;
    uint32_t best = 0;
    uint32_t bestScore = 0;
    VkDeviceSize bestHeapSize = 0;
    for (uint32_t i = 0; i < properties->memoryTypeCount; ++i) {
        if ((bits & (1u << i)) == 0) continue;
        const VkMemoryPropertyFlags flags = properties->memoryTypes[i].propertyFlags;
        if ((flags & required) != required) continue;
        if (!GPU_IsUsableMemoryType(properties, i)) continue;
        const VkMemoryHeap* heap = &properties->memoryHeaps[properties->memoryTypes[i].heapIndex];
        if (heap->size < minimumHeapSize) {
            continue;
        }
        const bool isAvoided = (flags & avoided) != 0;
        const uint32_t score = GPU_DevicePopcount(flags & preferred);
        if (!hasBest || (bestIsAvoided && !isAvoided) ||
            (bestIsAvoided == isAvoided && (score > bestScore || (score == bestScore && heap->size > bestHeapSize)))) {
            best = i;
            hasBest = true;
            bestIsAvoided = isAvoided;
            bestScore = score;
            bestHeapSize = heap->size;
        }
    }
    if (!hasBest) return false;
    *output = best;
    return true;
}

static bool GPU_CreateBackingBuffer(
    const GPUDeviceState* device,
    GPUBackingBuffer* output,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags required,
    VkMemoryPropertyFlags preferred,
    VkMemoryPropertyFlags avoided
) {
    memset(output, 0, sizeof(*output));
    GPUBackingBuffer result = {0};
    const VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = device->queueFamilyCount > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = device->queueFamilyCount,
        .pQueueFamilyIndices = device->queueFamilies,
    };
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkCreateBuffer(device->device, &bufferInfo, NULL, &result.buffer),
            "vkCreateBuffer",
            size
        ) != GPUError_None)
        return false;

    VkMemoryRequirements requirements = {0};
    device->fn.vkGetBufferMemoryRequirements(device->device, result.buffer, &requirements);
    uint32_t memoryType = 0;
    if (!GPU_FindMemoryType(
            device,
            requirements.memoryTypeBits,
            required,
            preferred,
            requirements.size,
            &memoryType,
            avoided
        )) {
        const GPUMessage message = {
            .kind = GPUMessageKind_NoMemoryHeap,
            .fatal = false,
            .data.memoryHeap =
                {.cpuVisible = (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0, .byteCount = requirements.size},
        };
        GPU_Report(device, &message);
        device->fn.vkDestroyBuffer(device->device, result.buffer, NULL);
        return false;
    }

    const VkMemoryAllocateFlagsInfo flagsInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    const VkMemoryAllocateInfo allocateInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flagsInfo,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memoryType,
    };
    const char* operation = (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "vkAllocateMemory (CPU-visible GPU heap)"
                                                                             : "vkAllocateMemory (GPU-only heap)";
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkAllocateMemory(device->device, &allocateInfo, NULL, &result.memory),
            operation,
            requirements.size
        ) != GPUError_None) {
        device->fn.vkDestroyBuffer(device->device, result.buffer, NULL);
        return false;
    }
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkBindBufferMemory(device->device, result.buffer, result.memory, 0),
            "vkBindBufferMemory",
            requirements.size
        ) != GPUError_None) {
        device->fn.vkDestroyBuffer(device->device, result.buffer, NULL);
        device->fn.vkFreeMemory(device->device, result.memory, NULL);
        return false;
    }

    if ((required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        if (GPU_ErrorFromVk(
                device,
                device->fn.vkMapMemory(device->device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped),
                "vkMapMemory",
                requirements.size
            ) != GPUError_None) {
            device->fn.vkDestroyBuffer(device->device, result.buffer, NULL);
            device->fn.vkFreeMemory(device->device, result.memory, NULL);
            return false;
        }
    }

    const VkBufferDeviceAddressInfo addressInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = result.buffer,
    };
    result.address = device->fn.vkGetBufferDeviceAddress(device->device, &addressInfo);
    *output = result;
    return true;
}

static void GPU_DestroyBackingBuffer(const GPUDeviceState* device, GPUBackingBuffer* backing) {
    if (backing->mapped) device->fn.vkUnmapMemory(device->device, backing->memory);
    device->fn.vkDestroyBuffer(device->device, backing->buffer, NULL);
    device->fn.vkFreeMemory(device->device, backing->memory, NULL);
}

static GPUGpuHeap GPU_AllocateDescriptorHeap(GPUDeviceState* device, VkDeviceSize size, GPUDescriptorHeapType type) {
    const VkPhysicalDeviceDescriptorHeapPropertiesEXT* heapProperties = &device->heapProperties;
    const VkDeviceSize resourceAlignment =
        heapProperties->imageDescriptorAlignment > heapProperties->bufferDescriptorAlignment
            ? heapProperties->imageDescriptorAlignment
            : heapProperties->bufferDescriptorAlignment;
    const VkDeviceSize reservedAlignment =
        type == GPUDescriptorHeapType_Texture ? resourceAlignment : heapProperties->samplerDescriptorAlignment;
    const VkDeviceSize heapAlignment = type == GPUDescriptorHeapType_Texture ? heapProperties->resourceHeapAlignment
                                                                             : heapProperties->samplerHeapAlignment;
    const VkDeviceSize reservedSize = type == GPUDescriptorHeapType_Texture
                                          ? heapProperties->minResourceHeapReservedRange
                                          : heapProperties->minSamplerHeapReservedRange;

    const VkDeviceSize reservedOffset = GPU_AlignUp(size, reservedAlignment);
    const VkDeviceSize bindSize = reservedOffset + reservedSize;
    const VkDeviceSize allocationAlignment =
        heapAlignment > GPU_ALLOCATION_ALIGNMENT ? heapAlignment : GPU_ALLOCATION_ALIGNMENT;
    const VkDeviceSize alignmentPadding = allocationAlignment - 1;
    const VkDeviceSize backingSize = bindSize + alignmentPadding;

    GPUGpuHeapOwner* heap = (GPUGpuHeapOwner*)GPU_PoolAlloc(&device->gpuHeapPool);
    if (!heap) {
        GPU_ReportLimit(device, GPULimit_GpuHeaps);
        return (GPUGpuHeap){0};
    }
    heap->state = device;
    if (!GPU_CreateBackingBuffer(
            device,
            &heap->backing,
            backingSize,
            GPU_UNIVERSAL_BUFFER_USAGE | VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT,
            GPU_CPU_VISIBLE_MEMORY_PROPERTIES,
            0,
            0
        )) {
        GPU_PoolFree(&device->gpuHeapPool, heap);
        return (GPUGpuHeap){0};
    }

    const VkDeviceAddress gpuAddress = GPU_AlignUp(heap->backing.address, allocationAlignment);
    const VkDeviceSize allocationOffset = gpuAddress - heap->backing.address;
    return (GPUGpuHeap){
        .range =
            {
                .cpu = (uint8_t*)heap->backing.mapped + allocationOffset,
                .gpu = (uint8_t*)(uintptr_t)gpuAddress,
                .size = size,
            },
        .owner = heap,
    };
}

static VkMemoryRequirements GPU_DeviceBufferMemoryRequirements(GPUDeviceState* device, VkBufferUsageFlags usage) {
    const VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 1,
        .usage = usage,
        .sharingMode = device->queueFamilyCount > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = device->queueFamilyCount,
        .pQueueFamilyIndices = device->queueFamilies,
    };
    const VkDeviceBufferMemoryRequirements requirementsInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
        .pCreateInfo = &bufferInfo,
    };
    VkMemoryRequirements2 requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };
    device->fn.vkGetDeviceBufferMemoryRequirements(device->device, &requirementsInfo, &requirements);
    return requirements.memoryRequirements;
}

static bool GPU_SupportsGpuHeapMemory(GPUDeviceState* device) {
    const VkMemoryRequirements ordinary = GPU_DeviceBufferMemoryRequirements(device, GPU_UNIVERSAL_BUFFER_USAGE);
    uint32_t memoryType = 0;
    if (!GPU_FindMemoryType(
            device,
            ordinary.memoryTypeBits,
            GPU_CPU_VISIBLE_MEMORY_PROPERTIES,
            0,
            ordinary.size,
            &memoryType,
            0
        ) ||
        !GPU_FindMemoryType(
            device,
            ordinary.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            0,
            ordinary.size,
            &memoryType,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
        )) {
        return false;
    }
    const VkMemoryRequirements descriptor = GPU_DeviceBufferMemoryRequirements(
        device,
        GPU_UNIVERSAL_BUFFER_USAGE | VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT
    );
    return GPU_FindMemoryType(
        device,
        descriptor.memoryTypeBits,
        GPU_CPU_VISIBLE_MEMORY_PROPERTIES,
        0,
        descriptor.size,
        &memoryType,
        0
    );
}

static bool GPU_DeviceFitsImageFormatProperties(
    const VkImageCreateInfo* imageInfo,
    const VkImageFormatProperties* properties
) {
    return imageInfo->extent.width <= properties->maxExtent.width &&
           imageInfo->extent.height <= properties->maxExtent.height &&
           imageInfo->extent.depth <= properties->maxExtent.depth && imageInfo->mipLevels <= properties->maxMipLevels &&
           imageInfo->arrayLayers <= properties->maxArrayLayers;
}

static bool GPU_SupportsImageCreateInfo(
    GPUDeviceState* device,
    const VkImageCreateInfo* imageInfo,
    VkImageFormatProperties* output
) {
    const VkPhysicalDeviceImageFormatInfo2 formatInfo = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .format = imageInfo->format,
        .type = imageInfo->imageType,
        .tiling = imageInfo->tiling,
        .usage = imageInfo->usage,
        .flags = imageInfo->flags,
    };
    VkImageFormatProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
    };
    const VkResult result =
        device->fn.vkGetPhysicalDeviceImageFormatProperties2(device->physicalDevice, &formatInfo, &properties);
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) return false;
    if (GPU_ErrorFromVk(device, result, "vkGetPhysicalDeviceImageFormatProperties2", 0) != GPUError_None) return false;
    if (output) *output = properties.imageFormatProperties;
    return GPU_DeviceFitsImageFormatProperties(imageInfo, &properties.imageFormatProperties);
}

static VkMemoryRequirements GPU_ImageMemoryRequirements(GPUDeviceState* device, const VkImageCreateInfo* imageInfo) {
    const VkDeviceImageMemoryRequirements requirementsInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
        .pCreateInfo = imageInfo,
    };
    VkMemoryRequirements2 requirements = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };
    device->fn.vkGetDeviceImageMemoryRequirements(device->device, &requirementsInfo, &requirements);
    return requirements.memoryRequirements;
}

static void GPU_DeviceIncludeTextureHeapAlignment(GPUDeviceState* device, const VkMemoryRequirements* requirements) {
    if (requirements->alignment > device->textureHeapAlignment) device->textureHeapAlignment = requirements->alignment;
}

static bool GPU_SelectTextureMemoryType(GPUDeviceState* device) {
    const VkFormatFeatureFlags2 colorFeatures = device->formatFeatures[GPUFormat_Rgba8Unorm];
    if ((colorFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) == 0) return false;
    // Probes cover DCC-capable color, broad 3D, and sampled depth layouts.
    // Resource Memory Association makes the color mask common to ordinary optimal-tiled images. Intersect every public
    // depth/stencil format below.
    const uint32_t probe2dSize = device->physicalProperties.limits.maxImageDimension2D < 2048
                                     ? device->physicalProperties.limits.maxImageDimension2D
                                     : 2048;
    VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((colorFeatures & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT) != 0)
        colorUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {.width = probe2dSize, .height = probe2dSize, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = colorUsage,
        .sharingMode = device->queueFamilyCount > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = device->queueFamilyCount,
        .pQueueFamilyIndices = device->queueFamilies,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (!GPU_SupportsImageCreateInfo(device, &imageInfo, NULL)) {
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!GPU_SupportsImageCreateInfo(device, &imageInfo, NULL)) return false;
    }
    const VkMemoryRequirements colorRequirements = GPU_ImageMemoryRequirements(device, &imageInfo);
    uint32_t memoryTypeBits = colorRequirements.memoryTypeBits;
    GPU_DeviceIncludeTextureHeapAlignment(device, &colorRequirements);

    const VkFormatFeatureFlags2 broadFeatures = device->formatFeatures[GPUFormat_Rgba32Float];
    const VkFormatFeatureFlags2 broadRequiredFeatures = GPU_RequiredFormatFeatures(
        GPUTextureUsage_Sampled | GPUTextureUsage_Storage | GPUTextureUsage_TransferDestination
    );
    if ((broadFeatures & broadRequiredFeatures) == broadRequiredFeatures) {
        const uint32_t probe3dSize = device->physicalProperties.limits.maxImageDimension3D < 2048
                                         ? device->physicalProperties.limits.maxImageDimension3D
                                         : 2048;
        imageInfo.imageType = VK_IMAGE_TYPE_3D;
        imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageInfo.extent =
            (VkExtent3D){.width = probe3dSize, .height = probe3dSize, .depth = probe3dSize < 4 ? probe3dSize : 4};
        imageInfo.mipLevels = 32u - GPU_DeviceCountLeadingZeros(probe3dSize);
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (GPU_SupportsImageCreateInfo(device, &imageInfo, NULL)) {
            const VkMemoryRequirements requirements = GPU_ImageMemoryRequirements(device, &imageInfo);
            GPU_DeviceIncludeTextureHeapAlignment(device, &requirements);
        }
    }

    static const GPUFormat depthStencilFormats[] = {
        GPUFormat_D16Unorm,
        GPUFormat_D24UnormS8Uint,
        GPUFormat_D32Float,
        GPUFormat_S8Uint,
        GPUFormat_D32FloatS8Uint,
    };
    const VkFormatFeatureFlags2 storageFeatures = GPU_RequiredFormatFeatures(GPUTextureUsage_Storage);
    for (uint32_t index = 0; index < GPUArrayCount(depthStencilFormats); ++index) {
        const GPUFormat format = depthStencilFormats[index];
        const VkFormatFeatureFlags2 features = device->formatFeatures[format];
        const bool combined = GPU_HasDepthAspect(format) && GPU_HasStencilAspect(format);
        VkImageUsageFlags compatibilityUsage = 0;
        if ((features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) != 0)
            compatibilityUsage = VK_IMAGE_USAGE_SAMPLED_BIT;
        else if ((features & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
            compatibilityUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        else if ((features & storageFeatures) == storageFeatures)
            compatibilityUsage = VK_IMAGE_USAGE_STORAGE_BIT;
        else if (!combined && (features & VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT) != 0)
            compatibilityUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        else if (!combined && (features & VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT) != 0)
            compatibilityUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (compatibilityUsage != 0) {
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = GPU_ToVkFormat(format);
            imageInfo.extent = (VkExtent3D){.width = 1, .height = 1, .depth = 1};
            imageInfo.mipLevels = 1;
            imageInfo.usage = compatibilityUsage;
            VkImageFormatProperties compatibilityProperties = {0};
            if (GPU_SupportsImageCreateInfo(device, &imageInfo, &compatibilityProperties)) {
                imageInfo.extent = (VkExtent3D){.width = 512, .height = 512, .depth = 1};
                if ((features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) != 0 &&
                    (features & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
                    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

                bool supported = imageInfo.usage == compatibilityUsage
                                     ? GPU_DeviceFitsImageFormatProperties(&imageInfo, &compatibilityProperties)
                                     : GPU_SupportsImageCreateInfo(device, &imageInfo, NULL);
                if (!supported && imageInfo.usage != compatibilityUsage) {
                    imageInfo.usage = compatibilityUsage;
                    supported = GPU_DeviceFitsImageFormatProperties(&imageInfo, &compatibilityProperties);
                }
                if (!supported) {
                    imageInfo.extent = (VkExtent3D){.width = 1, .height = 1, .depth = 1};
                    imageInfo.usage = compatibilityUsage;
                }

                const VkMemoryRequirements requirements = GPU_ImageMemoryRequirements(device, &imageInfo);
                memoryTypeBits &= requirements.memoryTypeBits;
                GPU_DeviceIncludeTextureHeapAlignment(device, &requirements);
            }
        }
    }
    return GPU_FindMemoryType(
        device,
        memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        0,
        1,
        &device->textureMemoryType,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    );
}

static uint64_t GPU_DeviceLayoutRegion(uint64_t* offset, uint64_t size) {
    const uint64_t start = GPU_AlignUp(*offset, GPU_ALLOCATION_ALIGNMENT);
    *offset = start + size;
    return start;
}

static GPUDeviceDesc GPU_ResolveDeviceDesc(const GPUDeviceDesc* desc) {
    GPUDeviceDesc resolved = *desc;
    GPUDeviceLimits* limits = &resolved.limits;
    if (!resolved.desiredSwapchainImageCount) resolved.desiredSwapchainImageCount = 2;
    if (!resolved.desiredQueueCount) resolved.desiredQueueCount = 1;
    if (!limits->maxTimelineSemaphores) limits->maxTimelineSemaphores = 64;
    if (!limits->maxGpuHeaps) limits->maxGpuHeaps = 256;
    if (!limits->maxTextureHeaps) limits->maxTextureHeaps = 64;
    if (!limits->maxTextures) limits->maxTextures = 4096;
    if (!limits->maxRenderViews) limits->maxRenderViews = 4096;
    if (!limits->maxTextureDescriptorHeaps) limits->maxTextureDescriptorHeaps = 8;
    if (!limits->maxSamplerDescriptorHeaps) limits->maxSamplerDescriptorHeaps = 8;
    if (!limits->maxPsos) limits->maxPsos = 1024;
    if (!limits->maxCommandPools) limits->maxCommandPools = 32;
    if (!limits->maxCommandBuffersPerPool) limits->maxCommandBuffersPerPool = 64;
    if (!limits->maxSubmitCommandBuffers) limits->maxSubmitCommandBuffers = 64;
    if (!limits->maxSubmitWaits) limits->maxSubmitWaits = 16;
    if (!limits->maxRetiredSwapchainImages) limits->maxRetiredSwapchainImages = 64;
    return resolved;
}

static void GPU_ComputeDeviceLayout(const GPUDeviceDesc* desc, GPUDeviceLayout* layout) {
    const GPUDeviceLimits* limits = &desc->limits;
    uint64_t offset = 0;
    layout->queueCapacity = desc->desiredQueueCount + desc->desiredComputeQueueCount + desc->desiredCopyQueueCount;
    const uint64_t commandBufferSize =
        sizeof(GPUCommandBuffer) + (uint64_t)desc->timestampQueryCount * (sizeof(uint64_t*) + sizeof(uint64_t));
    layout->commandPoolSlotSize =
        GPU_PoolSlotSize(sizeof(GPUCommandPool) + (uint64_t)limits->maxCommandBuffersPerPool * commandBufferSize);
    layout->queues = GPU_DeviceLayoutRegion(&offset, (uint64_t)layout->queueCapacity * sizeof(GPUQueue));
    layout->commandSubmitInfos = GPU_DeviceLayoutRegion(
        &offset,
        (uint64_t)layout->queueCapacity * limits->maxSubmitCommandBuffers * sizeof(VkCommandBufferSubmitInfo)
    );
    layout->waitSubmitInfos = GPU_DeviceLayoutRegion(
        &offset,
        (uint64_t)layout->queueCapacity * (limits->maxSubmitWaits + 1) * sizeof(VkSemaphoreSubmitInfo)
    );
    layout->retiredImages = GPU_DeviceLayoutRegion(
        &offset,
        (uint64_t)limits->maxRetiredSwapchainImages * sizeof(GPUDeferredSwapchainImage)
    );
    layout->timelineSemaphores = GPU_DeviceLayoutRegion(
        &offset,
        (uint64_t)GPU_PoolSlotSize(sizeof(GPUTimelineSemaphore)) * limits->maxTimelineSemaphores
    );
    layout->gpuHeaps =
        GPU_DeviceLayoutRegion(&offset, (uint64_t)GPU_PoolSlotSize(sizeof(GPUGpuHeapOwner)) * limits->maxGpuHeaps);
    layout->textureHeaps = GPU_DeviceLayoutRegion(
        &offset,
        (uint64_t)GPU_PoolSlotSize(sizeof(GPUTextureHeapOwner)) * limits->maxTextureHeaps
    );
    layout->textures =
        GPU_DeviceLayoutRegion(&offset, (uint64_t)GPU_PoolSlotSize(sizeof(GPUTexture)) * limits->maxTextures);
    layout->renderViews =
        GPU_DeviceLayoutRegion(&offset, (uint64_t)GPU_PoolSlotSize(sizeof(GPURenderView)) * limits->maxRenderViews);
    layout->textureDescriptorHeaps = GPU_DeviceLayoutRegion(
        &offset,
        (uint64_t)GPU_PoolSlotSize(sizeof(GPUTextureDescriptorHeap)) * limits->maxTextureDescriptorHeaps
    );
    layout->samplerDescriptorHeaps = GPU_DeviceLayoutRegion(
        &offset,
        (uint64_t)GPU_PoolSlotSize(sizeof(GPUSamplerDescriptorHeap)) * limits->maxSamplerDescriptorHeaps
    );
    layout->psos = GPU_DeviceLayoutRegion(&offset, (uint64_t)GPU_PoolSlotSize(sizeof(GPUPSO)) * limits->maxPsos);
    layout->commandPools =
        GPU_DeviceLayoutRegion(&offset, (uint64_t)layout->commandPoolSlotSize * limits->maxCommandPools);
    layout->total = GPU_AlignUp(offset, GPU_ALLOCATION_ALIGNMENT);
}

static GPUError GPU_DeviceEnumerateDeviceExtensions(
    const GPUDeviceState* device,
    VkPhysicalDevice physicalDevice,
    VkExtensionProperties* values,
    uint32_t capacity,
    uint32_t* count
) {
    *count = capacity;
    const VkResult result = device->fn.vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, count, values);
    return result == VK_INCOMPLETE ? GPUError_Unsupported
                                   : GPU_ErrorFromVk(device, result, "vkEnumerateDeviceExtensionProperties", 0);
}

static GPUError GPU_DeviceEnumerateInstanceExtensions(
    const GPUDeviceState* device,
    VkExtensionProperties* values,
    uint32_t capacity,
    uint32_t* count
) {
    *count = capacity;
    const VkResult result = device->fn.vkEnumerateInstanceExtensionProperties(NULL, count, values);
    return result == VK_INCOMPLETE ? GPUError_Unsupported
                                   : GPU_ErrorFromVk(device, result, "vkEnumerateInstanceExtensionProperties", 0);
}

#if defined(GPU_DEBUG)
static GPUError GPU_DeviceEnumerateInstanceLayers(
    const GPUDeviceState* device,
    VkLayerProperties* values,
    uint32_t capacity,
    uint32_t* count
) {
    *count = capacity;
    const VkResult result = device->fn.vkEnumerateInstanceLayerProperties(count, values);
    return result == VK_INCOMPLETE ? GPUError_Unsupported
                                   : GPU_ErrorFromVk(device, result, "vkEnumerateInstanceLayerProperties", 0);
}
#endif

static GPUError GPU_DeviceEnumeratePhysicalDevices(
    const GPUDeviceState* device,
    VkPhysicalDevice* values,
    uint32_t capacity,
    uint32_t* count
) {
    *count = capacity;
    const VkResult result = device->fn.vkEnumeratePhysicalDevices(device->instance, count, values);
    return result == VK_INCOMPLETE ? GPUError_Unsupported
                                   : GPU_ErrorFromVk(device, result, "vkEnumeratePhysicalDevices", 0);
}

typedef struct GPUDeviceQueriedFeatures {
    VkPhysicalDeviceFeatures2 core;
    VkPhysicalDeviceVulkan11Features vulkan11;
    VkPhysicalDeviceVulkan12Features vulkan12;
    VkPhysicalDeviceVulkan13Features vulkan13;
    VkPhysicalDeviceVulkan14Features vulkan14;
    VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptorHeap;
    VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR addressCommands;
    VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untypedPointers;
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unifiedImageLayouts;
    VkPhysicalDeviceMeshShaderFeaturesEXT meshShader;
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchainMaintenance1;
} GPUDeviceQueriedFeatures;

static void GPU_DeviceInitQueriedFeatures(
    GPUDeviceQueriedFeatures* features,
    bool presentation,
    bool includeUnifiedImageLayouts
) {
    memset(features, 0, sizeof(*features));
    features->core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features->vulkan11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features->vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features->vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features->vulkan14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
    features->descriptorHeap.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT;
    features->addressCommands.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR;
    features->untypedPointers.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR;
    features->unifiedImageLayouts.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR;
    features->meshShader.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    features->swapchainMaintenance1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
    features->core.pNext = &features->vulkan11;
    features->vulkan11.pNext = &features->vulkan12;
    features->vulkan12.pNext = &features->vulkan13;
    features->vulkan13.pNext = &features->vulkan14;
    features->vulkan14.pNext = &features->descriptorHeap;
    features->descriptorHeap.pNext = &features->addressCommands;
    features->addressCommands.pNext = &features->untypedPointers;
    features->untypedPointers.pNext =
        includeUnifiedImageLayouts ? (void*)&features->unifiedImageLayouts : (void*)&features->meshShader;
    features->unifiedImageLayouts.pNext = &features->meshShader;
    features->meshShader.pNext = presentation ? &features->swapchainMaintenance1 : NULL;
}

typedef struct GPUDeviceCandidate {
    VkPhysicalDevice physicalDevice;
    uint32_t queueFamilies[GPU_QUEUE_TYPE_COUNT];
    uint32_t queueCounts[GPU_QUEUE_TYPE_COUNT];
    bool timestamps[GPU_QUEUE_TYPE_COUNT];
    GPUExtent3 copyTextureGranularity;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memoryProperties;
    bool unifiedImageLayouts;
    bool imageCubeArray;
    bool textureCompressionBc;
    bool textureCompressionAstc;
    bool textureCompressionEtc2;
    bool storageInputOutput16;
    bool khrSwapchainMaintenance1;
    VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProperties;
    VkPhysicalDeviceVulkan12Properties vulkan12Properties;
} GPUDeviceCandidate;

static GPUError GPU_DeviceInspectCandidate(
    const GPUDeviceState* device,
    VkPhysicalDevice physicalDevice,
    bool khrSurfaceMaintenance1,
    bool extSurfaceMaintenance1,
    const GPUDeviceDesc* desc,
    GPUDeviceCandidate* output
) {
    const VkSurfaceKHR surface = device->surface;
    VkExtensionProperties extensions[GPU_MAX_DEVICE_EXTENSIONS];
    uint32_t extensionCount = 0;
    const GPUError extensionError = GPU_DeviceEnumerateDeviceExtensions(
        device,
        physicalDevice,
        extensions,
        GPU_MAX_DEVICE_EXTENSIONS,
        &extensionCount
    );
    if (extensionError != GPUError_None) return extensionError;
    const bool unifiedImageLayoutsExtension =
        GPU_HasExtension(extensions, extensionCount, VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME);
    static const char* const requiredExtensions[] = {
        VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
        VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME,
        VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
        VK_EXT_MESH_SHADER_EXTENSION_NAME,
    };
    for (uint32_t index = 0; index < GPUArrayCount(requiredExtensions); ++index) {
        if (!GPU_HasExtension(extensions, extensionCount, requiredExtensions[index])) return GPUError_Unsupported;
    }
    const bool khrSwapchainMaintenance1 =
        khrSurfaceMaintenance1 &&
        GPU_HasExtension(extensions, extensionCount, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    const bool extSwapchainMaintenance1 =
        extSurfaceMaintenance1 &&
        GPU_HasExtension(extensions, extensionCount, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    if (surface && (!GPU_HasExtension(extensions, extensionCount, VK_KHR_SWAPCHAIN_EXTENSION_NAME) ||
                    (!khrSwapchainMaintenance1 && !extSwapchainMaintenance1))) {
        return GPUError_Unsupported;
    }

    GPUDeviceCandidate* result = output;
    memset(result, 0, sizeof(*result));
    result->physicalDevice = physicalDevice;
    result->copyTextureGranularity = (GPUExtent3){.x = 1, .y = 1, .z = 1};
    result->heapProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;
    result->vulkan12Properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    result->heapProperties.pNext = &result->vulkan12Properties;
    VkPhysicalDeviceProperties2 properties2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &result->heapProperties,
    };
    device->fn.vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);
    result->properties = properties2.properties;
    result->heapProperties.pNext = NULL;
    result->vulkan12Properties.pNext = NULL;
    if (result->properties.apiVersion < VK_API_VERSION_1_4) return GPUError_Unsupported;

    device->fn.vkGetPhysicalDeviceMemoryProperties(physicalDevice, &result->memoryProperties);
    bool cpuVisibleMemory = false;
    for (uint32_t index = 0; index < result->memoryProperties.memoryTypeCount; ++index) {
        if (GPU_IsUsableMemoryType(&result->memoryProperties, index) &&
            (result->memoryProperties.memoryTypes[index].propertyFlags & GPU_CPU_VISIBLE_MEMORY_PROPERTIES) ==
                GPU_CPU_VISIBLE_MEMORY_PROPERTIES) {
            cpuVisibleMemory = true;
            break;
        }
    }
    if (!cpuVisibleMemory) return GPUError_Unsupported;

    GPUDeviceQueriedFeatures features;
    GPU_DeviceInitQueriedFeatures(&features, surface != VK_NULL_HANDLE, unifiedImageLayoutsExtension);
    device->fn.vkGetPhysicalDeviceFeatures2(physicalDevice, &features.core);
    const bool requiredFeatures =
        features.core.features.shaderInt16 == VK_TRUE && features.core.features.samplerAnisotropy == VK_TRUE &&
        features.core.features.depthBiasClamp == VK_TRUE && features.core.features.independentBlend == VK_TRUE &&
        features.core.features.fragmentStoresAndAtomics == VK_TRUE &&
        features.core.features.vertexPipelineStoresAndAtomics == VK_TRUE &&
        features.core.features.shaderStorageImageReadWithoutFormat == VK_TRUE &&
        features.core.features.shaderStorageImageWriteWithoutFormat == VK_TRUE &&
        features.core.features.multiDrawIndirect == VK_TRUE &&
        features.core.features.drawIndirectFirstInstance == VK_TRUE &&
        features.vulkan11.storageBuffer16BitAccess == VK_TRUE && features.vulkan11.storagePushConstant16 == VK_TRUE &&
        features.vulkan11.shaderDrawParameters == VK_TRUE && features.vulkan12.shaderFloat16 == VK_TRUE &&
        features.vulkan12.scalarBlockLayout == VK_TRUE && features.vulkan12.bufferDeviceAddress == VK_TRUE &&
        features.vulkan12.timelineSemaphore == VK_TRUE && features.vulkan13.synchronization2 == VK_TRUE &&
        features.vulkan13.dynamicRendering == VK_TRUE && features.vulkan13.maintenance4 == VK_TRUE &&
        features.vulkan14.maintenance5 == VK_TRUE && features.descriptorHeap.descriptorHeap == VK_TRUE &&
        features.addressCommands.deviceAddressCommands == VK_TRUE &&
        features.untypedPointers.shaderUntypedPointers == VK_TRUE && features.meshShader.meshShader == VK_TRUE &&
        (features.core.features.textureCompressionBC == VK_TRUE ||
         features.core.features.textureCompressionASTC_LDR == VK_TRUE) &&
        (!surface || features.swapchainMaintenance1.swapchainMaintenance1 == VK_TRUE);
    if (!requiredFeatures) return GPUError_Unsupported;
    result->unifiedImageLayouts =
        unifiedImageLayoutsExtension && features.unifiedImageLayouts.unifiedImageLayouts == VK_TRUE;
    result->imageCubeArray = features.core.features.imageCubeArray == VK_TRUE;
    result->textureCompressionBc = features.core.features.textureCompressionBC == VK_TRUE;
    result->textureCompressionAstc = features.core.features.textureCompressionASTC_LDR == VK_TRUE;
    result->textureCompressionEtc2 = features.core.features.textureCompressionETC2 == VK_TRUE;
    result->storageInputOutput16 = features.vulkan11.storageInputOutput16 == VK_TRUE;
    result->khrSwapchainMaintenance1 = khrSwapchainMaintenance1;

    uint32_t availableQueueCount = 0;
    device->fn.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &availableQueueCount, NULL);
    if (availableQueueCount > GPU_MAX_QUEUE_FAMILIES) return GPUError_Unsupported;
    VkQueueFamilyProperties queues[GPU_MAX_QUEUE_FAMILIES] = {0};
    uint32_t queueCount = availableQueueCount;
    device->fn.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, queues);
    for (uint32_t index = 0; index < queueCount; ++index) {
        if (queues[index].queueCount == 0) continue;
        const VkQueueFlags flags = queues[index].queueFlags;
        uint32_t type = 0;
        if (flags & VK_QUEUE_GRAPHICS_BIT) {
            if (!(flags & VK_QUEUE_COMPUTE_BIT)) continue;
        } else if (flags & VK_QUEUE_COMPUTE_BIT) {
            type = 1;
        } else if (
            (flags & VK_QUEUE_TRANSFER_BIT) &&
            !(flags & ~(VkQueueFlags)(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT | VK_QUEUE_PROTECTED_BIT))
        ) {
            type = 2;
        } else {
            continue;
        }
        if (result->queueCounts[type] != 0) continue;
        VkBool32 presentationSupported = VK_TRUE;
        if (surface && type == 0) {
            const GPUError presentationError = GPU_ErrorFromVk(
                device,
                device->fn.vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, index, surface, &presentationSupported),
                "vkGetPhysicalDeviceSurfaceSupportKHR",
                0
            );
            if (presentationError != GPUError_None) return presentationError;
        }
        if (presentationSupported == VK_TRUE) {
            result->queueFamilies[type] = index;
            result->queueCounts[type] = queues[index].queueCount;
            result->timestamps[type] = desc->timestampQueryCount != 0 && queues[index].timestampValidBits == 64 &&
                                       features.vulkan12.hostQueryReset;
            if (type == 2) {
                result->copyTextureGranularity = (GPUExtent3){
                    .x = queues[index].minImageTransferGranularity.width,
                    .y = queues[index].minImageTransferGranularity.height,
                    .z = queues[index].minImageTransferGranularity.depth,
                };
            }
        }
    }
    if (result->queueCounts[0] == 0 || (desc->desiredComputeQueueCount && result->queueCounts[1] == 0) ||
        (desc->desiredCopyQueueCount && result->queueCounts[2] == 0))
        return GPUError_Unsupported;

    return GPUError_None;
}

#define GPU_DEVICE_NO_MESSAGE ((GPUMessageKind) - 1)

static bool GPU_DeviceFailCreation(GPUDeviceState* device, GPUError error, GPUMessageKind kind, GPUError* outError) {
    if (kind == GPU_DEVICE_NO_MESSAGE && error == GPUError_Unsupported) kind = GPUMessageKind_DeviceUnsupported;
    if (kind != GPU_DEVICE_NO_MESSAGE) GPU_ReportKind(device, kind);
    GPUDeviceDestroy((GPUDevice*)device);
    if (outError) *outError = error;
    return false;
}

bool GPUDeviceInit(GPUDevice* deviceHandle, const GPUDeviceDesc* userDesc, GPUError* outError) {
    GPUAssert(deviceHandle && userDesc, "GPUDeviceInit requires device storage and a description");
    if (outError) *outError = GPUError_None;
    GPUDeviceState* state = (GPUDeviceState*)deviceHandle;
    if (state->initialized) {
        state->errorCallback = userDesc->errorCallback;
        state->errorUser = userDesc->errorUser;
        state->allocUser = userDesc->allocUser;
        return false;
    }
    const GPUDeviceDesc resolved = GPU_ResolveDeviceDesc(userDesc);
    const GPUDeviceDesc* desc = &resolved;
#if defined(_WIN32)
    const bool presentation = desc->win32.hwnd != NULL;
#else
    const bool presentation = false;
#endif
    GPUAssert(
        desc->desiredSwapchainImageCount <= GPU_MAX_SWAPCHAIN_IMAGES,
        "swapchain image count must fit the wrapper's presentation context array"
    );
    const GPUDeviceLimits* limits = &desc->limits;
    memset(state, 0, sizeof(*state));
    state->errorCallback = desc->errorCallback;
    state->errorUser = desc->errorUser;
    state->allocUser = desc->allocUser;

    GPUError error = GPU_LoaderOpen(state);
    if (error != GPUError_None) {
        GPUDeviceDestroy(deviceHandle);
        if (outError) *outError = error;
        return false;
    }
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    error =
        GPU_ErrorFromVk(state, state->fn.vkEnumerateInstanceVersion(&loaderVersion), "vkEnumerateInstanceVersion", 0);
    if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);
    if (loaderVersion < VK_API_VERSION_1_4) {
        GPU_ReportKind(state, GPUMessageKind_LoaderTooOld);
        GPUDeviceDestroy(deviceHandle);
        if (outError) *outError = GPUError_Unsupported;
        return false;
    }

    GPUDeviceLayout layout;
    GPU_ComputeDeviceLayout(desc, &layout);
    uint8_t* block = GPU_ALLOC(state->allocUser, layout.total, GPU_ALLOCATION_ALIGNMENT);
    GPUAssert(((uintptr_t)block % GPU_ALLOCATION_ALIGNMENT) == 0, "GPU_ALLOC must honor the requested alignment");
    memset(block, 0, layout.total);
    state->memory = block;
    state->queues = (GPUQueue*)(block + layout.queues);
    for (uint32_t index = 0; index < layout.queueCapacity; ++index) {
        state->queues[index].commandSubmitInfos = (VkCommandBufferSubmitInfo*)(block + layout.commandSubmitInfos) +
                                                  (uint64_t)index * limits->maxSubmitCommandBuffers;
        state->queues[index].waitSubmitInfos =
            (VkSemaphoreSubmitInfo*)(block + layout.waitSubmitInfos) + (uint64_t)index * (limits->maxSubmitWaits + 1);
    }
    state->swapchainDeleteQueue.entries = (GPUDeferredSwapchainImage*)(block + layout.retiredImages);
    state->swapchainDeleteQueue.capacity = limits->maxRetiredSwapchainImages;
    state->limits = *limits;
    GPU_PoolInit(
        &state->timelineSemaphorePool,
        block + layout.timelineSemaphores,
        GPU_PoolSlotSize(sizeof(GPUTimelineSemaphore)),
        limits->maxTimelineSemaphores
    );
    GPU_PoolInit(
        &state->gpuHeapPool,
        block + layout.gpuHeaps,
        GPU_PoolSlotSize(sizeof(GPUGpuHeapOwner)),
        limits->maxGpuHeaps
    );
    GPU_PoolInit(
        &state->textureHeapPool,
        block + layout.textureHeaps,
        GPU_PoolSlotSize(sizeof(GPUTextureHeapOwner)),
        limits->maxTextureHeaps
    );
    GPU_PoolInit(
        &state->texturePool,
        block + layout.textures,
        GPU_PoolSlotSize(sizeof(GPUTexture)),
        limits->maxTextures
    );
    GPU_PoolInit(
        &state->renderViewPool,
        block + layout.renderViews,
        GPU_PoolSlotSize(sizeof(GPURenderView)),
        limits->maxRenderViews
    );
    GPU_PoolInit(
        &state->textureDescriptorHeapPool,
        block + layout.textureDescriptorHeaps,
        GPU_PoolSlotSize(sizeof(GPUTextureDescriptorHeap)),
        limits->maxTextureDescriptorHeaps
    );
    GPU_PoolInit(
        &state->samplerDescriptorHeapPool,
        block + layout.samplerDescriptorHeaps,
        GPU_PoolSlotSize(sizeof(GPUSamplerDescriptorHeap)),
        limits->maxSamplerDescriptorHeaps
    );
    GPU_PoolInit(&state->psoPool, block + layout.psos, GPU_PoolSlotSize(sizeof(GPUPSO)), limits->maxPsos);
    GPU_PoolInit(
        &state->commandPoolPool,
        block + layout.commandPools,
        layout.commandPoolSlotSize,
        limits->maxCommandPools
    );
    state->heapProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;
    state->textureHeapAlignment = 16;
    state->textureMemoryType = VK_MAX_MEMORY_TYPES;
    state->timestampQueryCount = desc->timestampQueryCount;
    state->presentContextCount = presentation ? desc->desiredSwapchainImageCount : 0;

    VkExtensionProperties instanceExtensions[GPU_MAX_INSTANCE_EXTENSIONS];
    uint32_t instanceExtensionCount = 0;
    error = GPU_DeviceEnumerateInstanceExtensions(
        state,
        instanceExtensions,
        GPU_MAX_INSTANCE_EXTENSIONS,
        &instanceExtensionCount
    );
    if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);
#if defined(_WIN32)
    state->swapchainColorSpaceEnabled =
        presentation &&
        GPU_HasExtension(instanceExtensions, instanceExtensionCount, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    if (presentation && desc->swapchainColorSpace != GPUColorSpace_Srgb && !state->swapchainColorSpaceEnabled)
        return GPU_DeviceFailCreation(state, GPUError_Unsupported, GPU_DEVICE_NO_MESSAGE, outError);
    const bool khrSurfaceMaintenance1 =
        presentation &&
        GPU_HasExtension(instanceExtensions, instanceExtensionCount, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    const bool extSurfaceMaintenance1 =
        presentation &&
        GPU_HasExtension(instanceExtensions, instanceExtensionCount, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    if (presentation &&
        (!GPU_HasExtension(instanceExtensions, instanceExtensionCount, VK_KHR_SURFACE_EXTENSION_NAME) ||
         !GPU_HasExtension(instanceExtensions, instanceExtensionCount, VK_KHR_WIN32_SURFACE_EXTENSION_NAME) ||
         !GPU_HasExtension(
             instanceExtensions,
             instanceExtensionCount,
             VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME
         ) ||
         (!khrSurfaceMaintenance1 && !extSurfaceMaintenance1))) {
        return GPU_DeviceFailCreation(state, GPUError_Unsupported, GPU_DEVICE_NO_MESSAGE, outError);
    }
#else
    const bool khrSurfaceMaintenance1 = false;
    const bool extSurfaceMaintenance1 = false;
#endif
#if defined(GPU_DEBUG)
    VkLayerProperties layers[GPU_MAX_INSTANCE_LAYERS];
    uint32_t layerCount = 0;
    error = GPU_DeviceEnumerateInstanceLayers(state, layers, GPU_MAX_INSTANCE_LAYERS, &layerCount);
    if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);
    const bool debugUtilsAvailable =
        GPU_HasExtension(instanceExtensions, instanceExtensionCount, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const bool validationAvailable = GPU_HasLayer(layers, layerCount, "VK_LAYER_KHRONOS_validation");
#endif

    const char* enabledInstanceExtensions[7] = {0};
    uint32_t enabledInstanceExtensionCount = 0;
    const char* enabledLayers[1] = {0};
    uint32_t enabledLayerCount = 0;
#if defined(GPU_DEBUG)
    if (debugUtilsAvailable)
        enabledInstanceExtensions[enabledInstanceExtensionCount++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (validationAvailable) enabledLayers[enabledLayerCount++] = "VK_LAYER_KHRONOS_validation";
#endif
#if defined(_WIN32)
    if (presentation) {
        if (state->swapchainColorSpaceEnabled)
            enabledInstanceExtensions[enabledInstanceExtensionCount++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
        enabledInstanceExtensions[enabledInstanceExtensionCount++] = VK_KHR_SURFACE_EXTENSION_NAME;
        enabledInstanceExtensions[enabledInstanceExtensionCount++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
        enabledInstanceExtensions[enabledInstanceExtensionCount++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
        if (khrSurfaceMaintenance1)
            enabledInstanceExtensions[enabledInstanceExtensionCount++] = VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
        if (extSurfaceMaintenance1)
            enabledInstanceExtensions[enabledInstanceExtensionCount++] = VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
    }
#endif

    const VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "NoGraphicsAPI application",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .pEngineName = "NoGraphicsAPI",
        .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .apiVersion = VK_API_VERSION_1_4,
    };
    const VkInstanceCreateInfo instanceInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = enabledLayerCount,
        .ppEnabledLayerNames = enabledLayerCount ? enabledLayers : NULL,
        .enabledExtensionCount = enabledInstanceExtensionCount,
        .ppEnabledExtensionNames = enabledInstanceExtensionCount ? enabledInstanceExtensions : NULL,
    };
    error = GPU_ErrorFromVk(
        state,
        state->fn.vkCreateInstance(&instanceInfo, NULL, &state->instance),
        "vkCreateInstance",
        0
    );
    if (error == GPUError_None) error = GPU_LoaderLoadInstanceFunctions(state);
    if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);

#if defined(GPU_DEBUG)
    if (debugUtilsAvailable) {
        if (!state->fn.vkCreateDebugUtilsMessengerEXT || !state->fn.vkDestroyDebugUtilsMessengerEXT)
            return GPU_DeviceFailCreation(state, GPUError_DriverError, GPU_DEVICE_NO_MESSAGE, outError);
        const VkDebugUtilsMessengerCreateInfoEXT debugInfo = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = GPU_CommonDebugCallback,
        };
        error = GPU_ErrorFromVk(
            state,
            state->fn.vkCreateDebugUtilsMessengerEXT(state->instance, &debugInfo, NULL, &state->debugMessenger),
            "vkCreateDebugUtilsMessengerEXT",
            0
        );
        if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);
    }
#endif

#if defined(_WIN32)
    if (presentation) {
        const VkWin32SurfaceCreateInfoKHR surfaceInfo = {
            .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
            .hinstance = desc->win32.hinstance,
            .hwnd = desc->win32.hwnd,
        };
        error = GPU_ErrorFromVk(
            state,
            state->fn.vkCreateWin32SurfaceKHR(state->instance, &surfaceInfo, NULL, &state->surface),
            "vkCreateWin32SurfaceKHR",
            0
        );
        if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);
    }
#endif

    VkPhysicalDevice physicalDevices[GPU_MAX_PHYSICAL_DEVICES] = {0};
    uint32_t physicalDeviceCount = 0;
    error = GPU_DeviceEnumeratePhysicalDevices(state, physicalDevices, GPU_MAX_PHYSICAL_DEVICES, &physicalDeviceCount);
    if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);

    GPUDeviceCandidate selected = {0};
    bool hasSelected = false;
    for (uint32_t index = 0; index < physicalDeviceCount; ++index) {
        GPUDeviceCandidate candidate;
        error = GPU_DeviceInspectCandidate(
            state,
            physicalDevices[index],
            khrSurfaceMaintenance1,
            extSurfaceMaintenance1,
            desc,
            &candidate
        );
        if (error == GPUError_Unsupported) continue;
        if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);
        if (!hasSelected || candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            selected = candidate;
            hasSelected = true;
        }
        if (candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) break;
    }
    if (!hasSelected)
        return GPU_DeviceFailCreation(state, GPUError_Unsupported, GPUMessageKind_NoCompatibleGpu, outError);

    state->physicalDevice = selected.physicalDevice;
    state->physicalProperties = selected.properties;
    state->heapProperties = selected.heapProperties;
    state->maxTimelineValueDifference = selected.vulkan12Properties.maxTimelineSemaphoreValueDifference;
    state->heapProperties.pNext = NULL;
    state->memoryProperties = selected.memoryProperties;
    for (uint32_t value = GPUFormat_Undefined + 1; value < GPU_FORMAT_COUNT; ++value) {
        state->formatFeatures[value] = GPU_OptimalFormatFeatures(state, (GPUFormat)value);
    }

    GPUDeviceQueriedFeatures enabledFeatures;
    GPU_DeviceInitQueriedFeatures(&enabledFeatures, presentation, selected.unifiedImageLayouts);
    state->textureCompressionEtc2 = selected.textureCompressionEtc2;
    enabledFeatures.core.features.imageCubeArray = selected.imageCubeArray;
    enabledFeatures.core.features.samplerAnisotropy = VK_TRUE;
    enabledFeatures.core.features.shaderInt16 = VK_TRUE;
    enabledFeatures.core.features.depthBiasClamp = VK_TRUE;
    enabledFeatures.core.features.independentBlend = VK_TRUE;
    enabledFeatures.core.features.textureCompressionBC = selected.textureCompressionBc;
    enabledFeatures.core.features.textureCompressionASTC_LDR = selected.textureCompressionAstc;
    enabledFeatures.core.features.textureCompressionETC2 = selected.textureCompressionEtc2;
    enabledFeatures.core.features.fragmentStoresAndAtomics = VK_TRUE;
    enabledFeatures.core.features.vertexPipelineStoresAndAtomics = VK_TRUE;
    enabledFeatures.core.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    enabledFeatures.core.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    enabledFeatures.core.features.multiDrawIndirect = VK_TRUE;
    enabledFeatures.core.features.drawIndirectFirstInstance = VK_TRUE;
    enabledFeatures.vulkan11.storageBuffer16BitAccess = VK_TRUE;
    enabledFeatures.vulkan11.storagePushConstant16 = VK_TRUE;
    enabledFeatures.vulkan11.storageInputOutput16 = selected.storageInputOutput16;
    enabledFeatures.vulkan11.shaderDrawParameters = VK_TRUE;
    enabledFeatures.vulkan12.shaderFloat16 = VK_TRUE;
    enabledFeatures.vulkan12.scalarBlockLayout = VK_TRUE;
    enabledFeatures.vulkan12.timelineSemaphore = VK_TRUE;
    enabledFeatures.vulkan12.hostQueryReset =
        selected.timestamps[0] || selected.timestamps[1] || selected.timestamps[2];
    enabledFeatures.vulkan12.bufferDeviceAddress = VK_TRUE;
    enabledFeatures.vulkan13.synchronization2 = VK_TRUE;
    enabledFeatures.vulkan13.dynamicRendering = VK_TRUE;
    enabledFeatures.vulkan13.maintenance4 = VK_TRUE;
    enabledFeatures.vulkan14.maintenance5 = VK_TRUE;
    enabledFeatures.descriptorHeap.descriptorHeap = VK_TRUE;
    enabledFeatures.addressCommands.deviceAddressCommands = VK_TRUE;
    enabledFeatures.untypedPointers.shaderUntypedPointers = VK_TRUE;
    enabledFeatures.unifiedImageLayouts.unifiedImageLayouts = selected.unifiedImageLayouts ? VK_TRUE : VK_FALSE;
    enabledFeatures.meshShader.taskShader = VK_TRUE;
    enabledFeatures.meshShader.meshShader = VK_TRUE;
    enabledFeatures.swapchainMaintenance1.swapchainMaintenance1 = VK_TRUE;

    const uint32_t requestedCounts[GPU_QUEUE_TYPE_COUNT] =
        {desc->desiredQueueCount, desc->desiredComputeQueueCount, desc->desiredCopyQueueCount};
    uint32_t queueCounts[GPU_QUEUE_TYPE_COUNT] = {0};
    for (uint32_t type = 0; type < GPU_QUEUE_TYPE_COUNT; ++type) {
        queueCounts[type] =
            requestedCounts[type] < selected.queueCounts[type] ? requestedCounts[type] : selected.queueCounts[type];
        state->queueCount += queueCounts[type];
    }
    // Every priority is 1.0. The command submit info region is unused until the first submission, so it serves as
    // scratch and is re-zeroed below.
    float* queuePriorities = (float*)(block + layout.commandSubmitInfos);
    for (uint32_t index = 0; index < state->queueCount; ++index) queuePriorities[index] = 1.0f;
    VkDeviceQueueCreateInfo queueInfos[GPU_QUEUE_TYPE_COUNT] = {0};
    uint32_t firstQueue = 0;
    for (uint32_t type = 0; type < GPU_QUEUE_TYPE_COUNT; ++type) {
        if (queueCounts[type] == 0) continue;
        state->queueFamilies[state->queueFamilyCount] = selected.queueFamilies[type];
        queueInfos[state->queueFamilyCount++] = (VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = selected.queueFamilies[type],
            .queueCount = queueCounts[type],
            .pQueuePriorities = queuePriorities + firstQueue,
        };
        firstQueue += queueCounts[type];
    }
    const char* enabledDeviceExtensions[7] = {0};
    uint32_t enabledDeviceExtensionCount = 0;
    enabledDeviceExtensions[enabledDeviceExtensionCount++] = VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME;
    enabledDeviceExtensions[enabledDeviceExtensionCount++] = VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME;
    enabledDeviceExtensions[enabledDeviceExtensionCount++] = VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME;
    if (selected.unifiedImageLayouts) {
        enabledDeviceExtensions[enabledDeviceExtensionCount++] = VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME;
    }
    enabledDeviceExtensions[enabledDeviceExtensionCount++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
#if defined(_WIN32)
    if (presentation) {
        enabledDeviceExtensions[enabledDeviceExtensionCount++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        enabledDeviceExtensions[enabledDeviceExtensionCount++] = selected.khrSwapchainMaintenance1
                                                                     ? VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME
                                                                     : VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
    }
#endif
    const VkDeviceCreateInfo deviceInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enabledFeatures.core,
        .queueCreateInfoCount = state->queueFamilyCount,
        .pQueueCreateInfos = queueInfos,
        .enabledExtensionCount = enabledDeviceExtensionCount,
        .ppEnabledExtensionNames = enabledDeviceExtensions,
    };
    error = GPU_ErrorFromVk(
        state,
        state->fn.vkCreateDevice(state->physicalDevice, &deviceInfo, NULL, &state->device),
        "vkCreateDevice",
        0
    );
    if (error == GPUError_None) error = GPU_LoaderLoadDeviceFunctions(state);
    memset(
        block + layout.commandSubmitInfos,
        0,
        (uint64_t)layout.queueCapacity * limits->maxSubmitCommandBuffers * sizeof(VkCommandBufferSubmitInfo)
    );
    if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);
    firstQueue = 0;
    for (uint32_t type = 0; type < GPU_QUEUE_TYPE_COUNT; ++type) {
        for (uint32_t index = 0; index < queueCounts[type]; ++index) {
            GPUQueue* queue = &state->queues[firstQueue++];
            queue->familyIndex = selected.queueFamilies[type];
            queue->timestamps = selected.timestamps[type];
            state->fn.vkGetDeviceQueue(state->device, queue->familyIndex, index, &queue->queue);
        }
    }
    if (!GPU_SupportsGpuHeapMemory(state) || !GPU_SelectTextureMemoryType(state))
        return GPU_DeviceFailCreation(state, GPUError_Unsupported, GPU_DEVICE_NO_MESSAGE, outError);

    state->caps = (GPUDeviceCaps){
        .deviceName = state->physicalProperties.deviceName,
        .queueCount = state->queueCount,
        .generalQueueCount = queueCounts[0],
        .computeQueueCount = queueCounts[1],
        .copyQueueCount = queueCounts[2],
        .copyTextureGranularity = selected.copyTextureGranularity,
        .maxPushDataSize = state->heapProperties.maxPushDataSize,
        .textureHeapAlignment = state->textureHeapAlignment,
        .timestampPeriodNs = selected.properties.limits.timestampPeriod,
        .subTexelPrecisionBits = selected.properties.limits.subTexelPrecisionBits,
        .textureCompressionBc = selected.textureCompressionBc,
        .textureCompressionAstc = selected.textureCompressionAstc,
        .storageInputOutput16 = selected.storageInputOutput16,
        .indirectMeshDraw = true,
    };
    if (presentation) {
        error = GPU_CreateSwapchain(state, desc);
        if (error != GPUError_None) return GPU_DeviceFailCreation(state, error, GPU_DEVICE_NO_MESSAGE, outError);
    }
    state->initialized = true;
    return true;
}

void GPUDeviceDestroy(GPUDevice* deviceHandle) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    if (!device) return;
    GPUAssert(!device->acquiredSwapchain, "device destroyed while a swapchain image is acquired");
    if (device->device) {
        GPU_DrainContexts(device);
        GPU_DestroyOwnedSwapchain(device);
        GPU_SwapchainDeleteQueueCollect(
            device,
            &device->swapchainDeleteQueue,
            device->device,
            device->completedPresentationRetirement
        );
        GPUAssert(device->swapchainDeleteQueue.count == 0, "device->swapchainDeleteQueue.count == 0");
    }
    if (device->device && device->presentationRetirement)
        device->fn.vkDestroySemaphore(device->device, device->presentationRetirement, NULL);
    for (uint32_t index = 0; index < device->presentContextCount; ++index) {
        GPU_DestroyPresentContext(device, &device->presentContexts[index]);
    }
    if (device->device) device->fn.vkDestroyDevice(device->device, NULL);
    if (device->instance && device->surface) device->fn.vkDestroySurfaceKHR(device->instance, device->surface, NULL);
    if (device->instance && device->debugMessenger && device->fn.vkDestroyDebugUtilsMessengerEXT)
        device->fn.vkDestroyDebugUtilsMessengerEXT(device->instance, device->debugMessenger, NULL);
    if (device->instance) device->fn.vkDestroyInstance(device->instance, NULL);
    GPU_LoaderClose(device);
    if (device->memory) GPU_FREE(device->allocUser, device->memory);
    memset(device, 0, sizeof(*device));
}

void GPUDeviceSetErrorCallback(GPUDevice* deviceHandle, GPUErrorCallback callback, void* user) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device, "GPUDeviceSetErrorCallback called with a null device");
    device->errorCallback = callback;
    device->errorUser = user;
}

const GPUDeviceCaps* GPUDeviceGetCaps(const GPUDevice* deviceHandle) {
    const GPUDeviceState* device = (const GPUDeviceState*)deviceHandle;
    GPUAssert(device, "GPUDeviceGetCaps called with a null device");
    return &device->caps;
}

bool GPUDeviceSupportsTextureFormat(const GPUDevice* deviceHandle, GPUFormat format, GPUTextureUsage usage) {
    const GPUDeviceState* device = (const GPUDeviceState*)deviceHandle;
    GPUAssert(
        device && format != GPUFormat_Undefined && (uint32_t)format < GPU_FORMAT_COUNT &&
            GPU_HasValidTextureUsageBits(usage),
        "device && format != GPUFormat_Undefined && (uint32_t)format < GPU_FORMAT_COUNT && "
        "GPU_HasValidTextureUsageBits(usage)"
    );

    const GPUTextureFormatInfo info = GPUFormatGetTextureInfo(format);
    if ((info.depth || info.stencil) && (usage & GPUTextureUsage_ColorAttachment)) {
        return false;
    }
    if (!info.depth && !info.stencil && (usage & GPUTextureUsage_DepthStencilAttachment)) {
        return false;
    }
    if (info.depth && info.stencil &&
        (usage & (GPUTextureUsage_TransferSource | GPUTextureUsage_TransferDestination))) {
        return false;
    }
    switch (GPU_TextureCompression(format)) {
    case GPUTextureCompression_None: break;
    case GPUTextureCompression_Etc2:
        if (!device->textureCompressionEtc2) return false;
        break;
    case GPUTextureCompression_Astc:
        if (!device->caps.textureCompressionAstc) return false;
        break;
    case GPUTextureCompression_Bc:
        if (!device->caps.textureCompressionBc) return false;
        break;
    }

    const VkFormatFeatureFlags2 available = device->formatFeatures[format];
    const VkFormatFeatureFlags2 required = GPU_RequiredFormatFeatures(usage);
    return (available & required) == required;
}

GPUTimelineSemaphore* GPUTimelineSemaphoreCreate(GPUDevice* deviceHandle, uint64_t initialValue) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device, "GPUTimelineSemaphoreCreate called with a null device");

    const VkSemaphoreTypeCreateInfo timelineInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = initialValue,
    };
    const VkSemaphoreCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timelineInfo,
    };
    GPUTimelineSemaphore* result = (GPUTimelineSemaphore*)GPU_PoolAlloc(&device->timelineSemaphorePool);
    if (!result) {
        GPU_ReportLimit(device, GPULimit_TimelineSemaphores);
        return NULL;
    }
    result->state = device;
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkCreateSemaphore(device->device, &createInfo, NULL, &result->semaphore),
            "vkCreateSemaphore (timeline)",
            0
        ) != GPUError_None) {
        GPU_PoolFree(&device->timelineSemaphorePool, result);
        return NULL;
    }
    return result;
}

void GPUTimelineSemaphoreDestroy(GPUTimelineSemaphore* semaphore) {
    if (!semaphore) return;
    semaphore->state->fn.vkDestroySemaphore(semaphore->state->device, semaphore->semaphore, NULL);
    GPU_PoolFree(&semaphore->state->timelineSemaphorePool, semaphore);
}

uint64_t GPUTimelineSemaphoreGetCompletedValue(const GPUTimelineSemaphore* semaphore) {
    GPUAssert(
        semaphore && semaphore->state && semaphore->semaphore,
        "GPUTimelineSemaphoreGetCompletedValue received an invalid semaphore"
    );
    uint64_t value = 0;
    GPU_AssertVk(
        semaphore->state->fn.vkGetSemaphoreCounterValue(semaphore->state->device, semaphore->semaphore, &value)
    );
    return value;
}

void GPUTimelineSemaphoreWait(GPUTimelinePoint point) {
    GPUTimelineSemaphore* semaphore = point.semaphore;
    GPUAssert(
        semaphore && semaphore->state && semaphore->semaphore,
        "GPUTimelineSemaphoreWait requires a live timeline semaphore"
    );

    const VkSemaphore handle = semaphore->semaphore;
    const VkSemaphoreWaitInfo waitInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &handle,
        .pValues = &point.value,
    };
    GPU_AssertVk(semaphore->state->fn.vkWaitSemaphores(semaphore->state->device, &waitInfo, ~(uint64_t)0));
}

void GPUDeviceWaitIdle(GPUDevice* deviceHandle) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device, "GPUDeviceWaitIdle called with a null device");
    GPUAssert(!device->acquiredSwapchain, "GPUDeviceWaitIdle is not allowed while a swapchain image is acquired");
    GPU_AssertVk(device->fn.vkDeviceWaitIdle(device->device));
    GPU_DrainContexts(device);
    device->nextPresentContext = 0;
}

GPUGpuHeap GPUGpuHeapCreate(GPUDevice* deviceHandle, uint64_t byteCount, GPUMemoryType memory) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device, "device");
    VkMemoryPropertyFlags required = 0;
    VkMemoryPropertyFlags preferred = 0;
    VkMemoryPropertyFlags avoided = 0;
    switch (memory) {
    case GPUMemoryType_CpuVisible: required = GPU_CPU_VISIBLE_MEMORY_PROPERTIES; break;
    case GPUMemoryType_GpuOnly:
        required = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        avoided = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        break;
    case GPUMemoryType_Readback:
        required = GPU_CPU_VISIBLE_MEMORY_PROPERTIES;
        preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        break;
    default: GPUAssert(false, "GPUGpuHeapCreate received an invalid memory type"); return (GPUGpuHeap){0};
    }

    GPUGpuHeapOwner* heap = (GPUGpuHeapOwner*)GPU_PoolAlloc(&device->gpuHeapPool);
    if (!heap) {
        GPU_ReportLimit(device, GPULimit_GpuHeaps);
        return (GPUGpuHeap){0};
    }
    heap->state = device;
    if (!GPU_CreateBackingBuffer(
            device,
            &heap->backing,
            byteCount,
            GPU_UNIVERSAL_BUFFER_USAGE,
            required,
            preferred,
            avoided
        )) {
        GPU_PoolFree(&device->gpuHeapPool, heap);
        return (GPUGpuHeap){0};
    }
    return (GPUGpuHeap){
        .range =
            {
                .cpu = (uint8_t*)heap->backing.mapped,
                .gpu = (uint8_t*)(uintptr_t)heap->backing.address,
                .size = byteCount,
            },
        .owner = heap,
    };
}

void GPUGpuHeapDestroy(const GPUGpuHeap* heap) {
    if (!heap->owner) return;
    GPUAssert(heap->owner->state, "heap->owner->state");
    GPUGpuHeapOwner* owner = (GPUGpuHeapOwner*)heap->owner;
    GPU_DestroyBackingBuffer(owner->state, &owner->backing);
    GPU_PoolFree(&owner->state->gpuHeapPool, owner);
}

// ---- gpu_swapchain.c ------------------------------------------------------------------------------------------------



// Swapchain delete queue. Fixed-capacity ring; capacity is GPUDeviceLimits::maxRetiredSwapchainImages.

static void GPU_SwapchainDeleteQueuePush(
    GPUSwapchainDeleteQueue* queue,
    uint64_t retireValue,
    VkSwapchainKHR swapchain,
    VkImageView view
) {
    GPUAssert(swapchain && view, "swapchain && view");
    GPUAssert(
        queue->count < queue->capacity,
        "retired swapchain images exceed GPUDeviceLimits::maxRetiredSwapchainImages"
    );
    if (queue->count != 0) {
        GPUAssert(
            queue->entries[(queue->first + queue->count - 1) % queue->capacity].retireValue <= retireValue,
            "swapchain deletion retire values must be monotonic"
        );
    }
    queue->entries[(queue->first + queue->count) % queue->capacity] = (GPUDeferredSwapchainImage){
        .retireValue = retireValue,
        .swapchain = swapchain,
        .view = view,
    };
    ++queue->count;
}

static void GPU_SwapchainDeleteQueueCollect(
    GPUDeviceState* state,
    GPUSwapchainDeleteQueue* queue,
    VkDevice device,
    uint64_t completedValue
) {
    while (queue->count != 0 && queue->entries[queue->first].retireValue <= completedValue) {
        const GPUDeferredSwapchainImage entry = queue->entries[queue->first];
        const bool finalImage =
            queue->count == 1 || queue->entries[(queue->first + 1) % queue->capacity].swapchain != entry.swapchain;
        state->fn.vkDestroyImageView(device, entry.view, NULL);
        if (finalImage) state->fn.vkDestroySwapchainKHR(device, entry.swapchain, NULL);
        memset(&queue->entries[queue->first], 0, sizeof(GPUDeferredSwapchainImage));
        queue->first = (queue->first + 1) % queue->capacity;
        --queue->count;
    }
    if (queue->count == 0) queue->first = 0;
}

static void GPU_PollPresentationRetirement(GPUDeviceState* device) {
    if (!device->presentationRetirement ||
        device->completedPresentationRetirement == device->presentationRetirementValue)
        return;
    uint64_t completed = 0;
    GPU_AssertVk(device->fn.vkGetSemaphoreCounterValue(device->device, device->presentationRetirement, &completed));
    GPUAssert(
        completed >= device->completedPresentationRetirement && completed <= device->presentationRetirementValue,
        "completed >= device->completedPresentationRetirement && completed <= device->presentationRetirementValue"
    );
    if (completed == device->completedPresentationRetirement) return;
    device->completedPresentationRetirement = completed;
    GPU_SwapchainDeleteQueueCollect(
        device,
        &device->swapchainDeleteQueue,
        device->device,
        device->completedPresentationRetirement
    );
}

static void GPU_WaitPresentationRetirement(GPUDeviceState* device, uint64_t value) {
    GPUAssert(value <= device->presentationRetirementValue, "value <= device->presentationRetirementValue");
    if (value <= device->completedPresentationRetirement) {
        GPU_SwapchainDeleteQueueCollect(
            device,
            &device->swapchainDeleteQueue,
            device->device,
            device->completedPresentationRetirement
        );
        return;
    }
    const VkSemaphoreWaitInfo waitInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &device->presentationRetirement,
        .pValues = &value,
    };
    GPU_AssertVk(device->fn.vkWaitSemaphores(device->device, &waitInfo, ~(uint64_t)0));
    device->completedPresentationRetirement = value;
    GPU_SwapchainDeleteQueueCollect(
        device,
        &device->swapchainDeleteQueue,
        device->device,
        device->completedPresentationRetirement
    );
}

static uint64_t GPU_NextPresentationRetirement(GPUDeviceState* device) {
    const uint64_t next = device->presentationRetirementValue + 1;
    if (next - device->completedPresentationRetirement > device->maxTimelineValueDifference) {
        GPU_PollPresentationRetirement(device);
        if (next - device->completedPresentationRetirement > device->maxTimelineValueDifference)
            GPU_WaitPresentationRetirement(device, next - device->maxTimelineValueDifference);
        GPUAssert(
            next - device->completedPresentationRetirement <= device->maxTimelineValueDifference,
            "next - device->completedPresentationRetirement <= device->maxTimelineValueDifference"
        );
    }
    device->presentationRetirementValue = next;
    return next;
}

static GPUError GPU_CreatePresentContext(GPUDeviceState* device, GPUPresentContext* context) {
    GPUAssert(
        !context->acquired && !context->rendered && !context->presented && !context->swapchain &&
            !context->presentPending,
        "!context->acquired && !context->rendered && !context->presented && !context->swapchain && "
        "!context->presentPending"
    );
    const VkSemaphoreCreateInfo semaphoreInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    GPUError error = GPU_ErrorFromVk(
        device,
        device->fn.vkCreateSemaphore(device->device, &semaphoreInfo, NULL, &context->acquired),
        "vkCreateSemaphore (acquire)",
        0
    );
    if (error != GPUError_None) return error;
    error = GPU_ErrorFromVk(
        device,
        device->fn.vkCreateSemaphore(device->device, &semaphoreInfo, NULL, &context->rendered),
        "vkCreateSemaphore (render)",
        0
    );
    if (error != GPUError_None) {
        GPU_DestroyPresentContext(device, context);
        return error;
    }
    const VkFenceCreateInfo fenceInfo = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    error = GPU_ErrorFromVk(
        device,
        device->fn.vkCreateFence(device->device, &fenceInfo, NULL, &context->presented),
        "vkCreateFence",
        0
    );
    if (error != GPUError_None) GPU_DestroyPresentContext(device, context);
    return error;
}

static void GPU_DestroyPresentContext(GPUDeviceState* device, GPUPresentContext* context) {
    GPUAssert(!context->presentPending, "!context->presentPending");
    if (device->device && context->acquired) device->fn.vkDestroySemaphore(device->device, context->acquired, NULL);
    if (device->device && context->rendered) device->fn.vkDestroySemaphore(device->device, context->rendered, NULL);
    if (device->device && context->presented) device->fn.vkDestroyFence(device->device, context->presented, NULL);
    memset(context, 0, sizeof(GPUPresentContext));
}

static void GPU_FinishPresentContext(GPUDeviceState* device, GPUPresentContext* context) {
    GPUAssert(!context->presentPending && context->swapchain, "!context->presentPending && context->swapchain");
    const VkSwapchainKHR completedSwapchain = context->swapchain;
    context->swapchain = VK_NULL_HANDLE;
    for (uint32_t index = 0; index < GPU_MAX_SWAPCHAIN_IMAGES; ++index) {
        GPURetiredSwapchain* retired = &device->retiredSwapchains[index];
        if (retired->handle != completedSwapchain) continue;
        for (uint32_t pending = 0; pending < GPU_MAX_SWAPCHAIN_IMAGES; ++pending) {
            if (device->presentContexts[pending].presentPending &&
                device->presentContexts[pending].swapchain == completedSwapchain)
                return;
        }
        GPU_QueueRetiredSwapchain(device, retired);
        return;
    }
}

static void GPU_WaitPresentContext(GPUDeviceState* device, GPUPresentContext* context) {
    if (!context->presentPending) return;
    GPU_AssertVk(device->fn.vkWaitForFences(device->device, 1, &context->presented, VK_TRUE, ~(uint64_t)0));
    context->presentPending = false;
    GPU_FinishPresentContext(device, context);
}

static void GPU_PollPresentContexts(GPUDeviceState* device) {
    for (uint32_t index = 0; index < device->presentContextCount; ++index) {
        GPUPresentContext* context = &device->presentContexts[index];
        if (!context->presentPending) continue;
        const VkResult result = device->fn.vkGetFenceStatus(device->device, context->presented);
        if (result == VK_NOT_READY) continue;
        GPU_AssertVk(result);
        context->presentPending = false;
        GPU_FinishPresentContext(device, context);
    }
}

static void GPU_QueueRetiredSwapchain(GPUDeviceState* device, GPURetiredSwapchain* retired) {
    GPUAssert(retired->handle && retired->viewCount != 0, "retired->handle && retired->viewCount != 0");
    for (uint32_t index = 0; index < retired->viewCount; ++index) {
        GPUAssert(retired->views[index], "retired->views[index]");
        GPU_SwapchainDeleteQueuePush(
            &device->swapchainDeleteQueue,
            device->presentationRetirementValue,
            retired->handle,
            retired->views[index]
        );
    }
    memset(retired, 0, sizeof(GPURetiredSwapchain));
    GPU_SwapchainDeleteQueueCollect(
        device,
        &device->swapchainDeleteQueue,
        device->device,
        device->completedPresentationRetirement
    );
}

static void GPU_DrainContexts(GPUDeviceState* device) {
    GPU_WaitPresentationRetirement(device, device->presentationRetirementValue);
    for (uint32_t index = 0; index < device->presentContextCount; ++index) {
        GPU_WaitPresentContext(device, &device->presentContexts[index]);
    }
    for (uint32_t index = 0; index < GPU_MAX_SWAPCHAIN_IMAGES; ++index) {
        GPUAssert(!device->retiredSwapchains[index].handle, "!device->retiredSwapchains[index].handle");
    }
    GPU_SwapchainDeleteQueueCollect(
        device,
        &device->swapchainDeleteQueue,
        device->device,
        device->completedPresentationRetirement
    );
}

static void GPU_SwapRetireHandle(GPUSwapchain* swapchain) {
    GPUDeviceState* device = swapchain->state;
    if (!swapchain->handle) {
        GPUAssert(swapchain->imageCount == 0, "swapchain->imageCount == 0");
        swapchain->width = 0;
        swapchain->height = 0;
        return;
    }

    GPU_PollPresentContexts(device);
    GPURetiredSwapchain retired = {
        .handle = swapchain->handle,
    };
    for (uint32_t index = 0; index < swapchain->imageCount; ++index) {
        retired.views[retired.viewCount++] = swapchain->renderViews[index].view;
        memset(&swapchain->renderViews[index], 0, sizeof(GPURenderView));
        swapchain->images[index] = VK_NULL_HANDLE;
        swapchain->initialized[index] = false;
    }
    swapchain->imageCount = 0;
    swapchain->handle = VK_NULL_HANDLE;
    swapchain->width = 0;
    swapchain->height = 0;

    bool presentPending = false;
    for (uint32_t index = 0; index < device->presentContextCount; ++index) {
        if (device->presentContexts[index].presentPending &&
            device->presentContexts[index].swapchain == retired.handle) {
            presentPending = true;
            break;
        }
    }
    if (!presentPending) {
        GPU_QueueRetiredSwapchain(device, &retired);
        return;
    }
    for (uint32_t index = 0; index < GPU_MAX_SWAPCHAIN_IMAGES; ++index) {
        if (!device->retiredSwapchains[index].handle) {
            device->retiredSwapchains[index] = retired;
            return;
        }
    }
    for (uint32_t index = 0; index < device->presentContextCount; ++index) {
        GPUPresentContext* context = &device->presentContexts[index];
        if (context->presentPending && context->swapchain == retired.handle) GPU_WaitPresentContext(device, context);
    }
    GPU_QueueRetiredSwapchain(device, &retired);
}

static void GPU_DestroyOwnedSwapchain(GPUDeviceState* device) {
    if (!device->swapchain) return;
    GPUSwapchain* swapchain = device->swapchain;
    GPUAssert(
        !swapchain->acquired && device->acquiredSwapchain != swapchain,
        "!swapchain->acquired && device->acquiredSwapchain != swapchain"
    );
    GPU_SwapRetireHandle(swapchain);
    memset(swapchain, 0, sizeof(GPUSwapchain));
    device->swapchain = NULL;
}

static VkCompositeAlphaFlagBitsKHR GPU_SwapChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
    static const VkCompositeAlphaFlagBitsKHR choices[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (uint32_t index = 0; index < GPUArrayCount(choices); ++index) {
        if ((supported & (VkCompositeAlphaFlagsKHR)choices[index]) != 0) return choices[index];
    }
    return (VkCompositeAlphaFlagBitsKHR)0;
}

static bool GPU_SwapSurfaceConfigurationChanged(const GPUSwapchain* swapchain) {
    VkSurfaceCapabilitiesKHR capabilities = {0};
    GPU_RequireError(
        swapchain->state,
        GPU_ErrorFromVk(
            swapchain->state,
            swapchain->state->fn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                swapchain->state->physicalDevice,
                swapchain->state->surface,
                &capabilities
            ),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
            0
        ),
        "Display surface query"
    );

    const VkExtent2D extent = capabilities.currentExtent;
    return extent.width == UINT_MAX || extent.height == UINT_MAX || extent.width != swapchain->width ||
           extent.height != swapchain->height || capabilities.currentTransform != swapchain->transform ||
           GPU_SwapChooseCompositeAlpha(capabilities.supportedCompositeAlpha) != swapchain->compositeAlpha;
}

static VkColorSpaceKHR GPU_SwapToVkColorSpace(GPUColorSpace colorSpace) {
    return colorSpace == GPUColorSpace_ExtendedSrgbLinear ? VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT
                                                          : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
}

// Non-sRGB color spaces require VK_EXT_swapchain_colorspace. Format support is only known from the surface query inside
// GPU_RecreateSwapchain.
static bool GPU_SupportsSwapchainMode(GPUDeviceState* device, GPUFormat format, VkColorSpaceKHR colorSpace) {
    (void)format;
    return colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR || device->swapchainColorSpaceEnabled;
}

static GPUError GPU_RecreateSwapchain(GPUSwapchain* swapchain) {
    GPUDeviceState* device = swapchain->state;
    GPUAssert(!swapchain->acquired && !device->acquiredSwapchain, "!swapchain->acquired && !device->acquiredSwapchain");
    const VkPresentModeKHR presentMode = GPU_SWAPCHAIN_PRESENT_MODE;
    const VkSurfacePresentModeKHR presentModeInfo = {
        .sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR,
        .presentMode = presentMode,
    };
    const VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
        .pNext = &presentModeInfo,
        .surface = device->surface,
    };
    VkSurfaceCapabilities2KHR capabilitiesInfo = {
        .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
    };
    GPUError error = GPU_ErrorFromVk(
        device,
        device->fn.vkGetPhysicalDeviceSurfaceCapabilities2KHR(device->physicalDevice, &surfaceInfo, &capabilitiesInfo),
        "vkGetPhysicalDeviceSurfaceCapabilities2KHR",
        0
    );
    if (error != GPUError_None) return error;
    const VkSurfaceCapabilitiesKHR* capabilities = &capabilitiesInfo.surfaceCapabilities;

    const VkExtent2D extent = capabilities->currentExtent;
    if (extent.width == UINT_MAX) return GPUError_Unsupported;
    if ((capabilities->supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) return GPUError_Unsupported;

    VkSurfaceFormatKHR formats[GPU_MAX_SURFACE_FORMATS] = {0};
    uint32_t surfaceFormatCount = 0;
    error = GPU_ErrorFromVk(
        device,
        device->fn
            .vkGetPhysicalDeviceSurfaceFormatsKHR(device->physicalDevice, device->surface, &surfaceFormatCount, NULL),
        "vkGetPhysicalDeviceSurfaceFormatsKHR",
        0
    );
    if (error != GPUError_None) return error;
    if (surfaceFormatCount == 0 || surfaceFormatCount > GPU_MAX_SURFACE_FORMATS) return GPUError_Unsupported;
    error = GPU_ErrorFromVk(
        device,
        device->fn.vkGetPhysicalDeviceSurfaceFormatsKHR(
            device->physicalDevice,
            device->surface,
            &surfaceFormatCount,
            formats
        ),
        "vkGetPhysicalDeviceSurfaceFormatsKHR",
        0
    );
    if (error != GPUError_None) return error;

    const VkFormat requestedFormat = GPU_ToVkFormat(swapchain->format);
    bool formatSupported = false;
    for (uint32_t index = 0; index < surfaceFormatCount; ++index) {
        if ((formats[index].format == requestedFormat || formats[index].format == VK_FORMAT_UNDEFINED) &&
            formats[index].colorSpace == swapchain->colorSpace) {
            formatSupported = true;
            break;
        }
    }
    if (!formatSupported) return GPUError_Unsupported;

    if (extent.width == 0 || extent.height == 0) {
        swapchain->width = 0;
        swapchain->height = 0;
        swapchain->recreateRequired = true;
        return GPUError_None;
    }

    uint32_t requestedImageCount = device->presentContextCount;
    if (requestedImageCount < capabilities->minImageCount) requestedImageCount = capabilities->minImageCount;
    if (capabilities->maxImageCount != 0 && requestedImageCount > capabilities->maxImageCount)
        requestedImageCount = capabilities->maxImageCount;
    if (requestedImageCount == 0 || requestedImageCount > GPU_MAX_SWAPCHAIN_IMAGES) return GPUError_Unsupported;

    const VkCompositeAlphaFlagBitsKHR compositeAlpha =
        GPU_SwapChooseCompositeAlpha(capabilities->supportedCompositeAlpha);
    if (!compositeAlpha) {
        GPU_ReportKind(device, GPUMessageKind_SurfaceCompositeAlphaUnsupported);
        return GPUError_Unsupported;
    }
    const VkSwapchainPresentModesCreateInfoKHR presentModesInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_KHR,
        .presentModeCount = 1,
        .pPresentModes = &presentMode,
    };
    const VkSwapchainCreateInfoKHR createInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = &presentModesInfo,
        .surface = device->surface,
        .minImageCount = requestedImageCount,
        .imageFormat = requestedFormat,
        .imageColorSpace = swapchain->colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = device->queueFamilyCount > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = device->queueFamilyCount,
        .pQueueFamilyIndices = device->queueFamilies,
        .preTransform = capabilities->currentTransform,
        .compositeAlpha = compositeAlpha,
        .presentMode = presentMode,
        .clipped = VK_TRUE,
        .oldSwapchain = swapchain->handle,
    };
    VkSwapchainKHR newHandle = VK_NULL_HANDLE;
    error = GPU_ErrorFromVk(
        device,
        device->fn.vkCreateSwapchainKHR(device->device, &createInfo, NULL, &newHandle),
        "vkCreateSwapchainKHR",
        0
    );
    if (error != GPUError_None) {
        GPU_SwapRetireHandle(swapchain);
        return error;
    }

    VkImage images[GPU_MAX_SWAPCHAIN_IMAGES] = {0};
    uint32_t imageCount = 0;
    VkResult result = device->fn.vkGetSwapchainImagesKHR(device->device, newHandle, &imageCount, NULL);
    if (result != VK_SUCCESS || imageCount == 0 || imageCount > GPU_MAX_SWAPCHAIN_IMAGES) {
        device->fn.vkDestroySwapchainKHR(device->device, newHandle, NULL);
        GPU_SwapRetireHandle(swapchain);
        return result == VK_SUCCESS ? GPUError_Unsupported
                                    : GPU_ErrorFromVk(device, result, "vkGetSwapchainImagesKHR", 0);
    }
    result = device->fn.vkGetSwapchainImagesKHR(device->device, newHandle, &imageCount, images);
    if (result != VK_SUCCESS) {
        device->fn.vkDestroySwapchainKHR(device->device, newHandle, NULL);
        GPU_SwapRetireHandle(swapchain);
        return GPU_ErrorFromVk(device, result, "vkGetSwapchainImagesKHR", 0);
    }

    VkImageView views[GPU_MAX_SWAPCHAIN_IMAGES] = {0};
    for (uint32_t index = 0; index < imageCount; ++index) {
        const VkImageViewCreateInfo viewInfo = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[index],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = requestedFormat,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        result = device->fn.vkCreateImageView(device->device, &viewInfo, NULL, &views[index]);
        if (result != VK_SUCCESS) {
            for (uint32_t created = 0; created < index; ++created) {
                device->fn.vkDestroyImageView(device->device, views[created], NULL);
            }
            device->fn.vkDestroySwapchainKHR(device->device, newHandle, NULL);
            GPU_SwapRetireHandle(swapchain);
            return GPU_ErrorFromVk(device, result, "vkCreateImageView (swapchain)", 0);
        }
    }

    GPU_SwapRetireHandle(swapchain);
    swapchain->handle = newHandle;
    swapchain->imageCount = imageCount;
    swapchain->width = extent.width;
    swapchain->height = extent.height;
    swapchain->transform = capabilities->currentTransform;
    swapchain->compositeAlpha = compositeAlpha;
    swapchain->recreateRequired = false;
    for (uint32_t index = 0; index < imageCount; ++index) {
        swapchain->images[index] = images[index];
        swapchain->renderViews[index] = (GPURenderView){
            .state = device,
            .view = views[index],
            .width = extent.width,
            .height = extent.height,
            .swapchainView = true,
        };
    }
    return GPUError_None;
}

// Creates the presentation retirement timeline, the present contexts, and the initial swapchain. The caller destroys
// everything on failure.
static GPUError GPU_CreateSwapchain(GPUDeviceState* device, const GPUDeviceDesc* desc) {
    const VkColorSpaceKHR colorSpace = GPU_SwapToVkColorSpace(desc->swapchainColorSpace);
    if (!GPU_SupportsSwapchainMode(device, desc->swapchainFormat, colorSpace)) return GPUError_Unsupported;

    const VkSemaphoreTypeCreateInfo typeInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    };
    const VkSemaphoreCreateInfo semaphoreInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &typeInfo,
    };
    GPUError error = GPU_ErrorFromVk(
        device,
        device->fn.vkCreateSemaphore(device->device, &semaphoreInfo, NULL, &device->presentationRetirement),
        "vkCreateSemaphore (presentation)",
        0
    );
    if (error != GPUError_None) return error;
    for (uint32_t index = 0; index < device->presentContextCount; ++index) {
        error = GPU_CreatePresentContext(device, &device->presentContexts[index]);
        if (error != GPUError_None) return error;
    }

    GPUSwapchain* swapchain = &device->swapchainStorage;
    memset(swapchain, 0, sizeof(GPUSwapchain));
    swapchain->state = device;
    swapchain->format = desc->swapchainFormat == GPUFormat_Undefined ? GPUFormat_Bgra8Srgb : desc->swapchainFormat;
    swapchain->colorSpace = colorSpace;
    swapchain->transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchain->compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    device->swapchain = swapchain;
    error = GPU_RecreateSwapchain(swapchain);
    if (error != GPUError_None) GPU_DestroyOwnedSwapchain(device);
    return error;
}

GPUError GPUDeviceSetSwapchainFormat(GPUDevice* deviceHandle, GPUFormat format, GPUColorSpace colorSpace) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device && !device->acquiredSwapchain, "device && !device->acquiredSwapchain");
    const VkColorSpaceKHR requestedColorSpace = GPU_SwapToVkColorSpace(colorSpace);
    if (!device->swapchain || !GPU_SupportsSwapchainMode(device, format, requestedColorSpace))
        return GPUError_Unsupported;

    GPUSwapchain* swapchain = device->swapchain;
    if (swapchain->format == format && swapchain->colorSpace == requestedColorSpace) return GPUError_None;

    const GPUFormat oldFormat = swapchain->format;
    const VkColorSpaceKHR oldColorSpace = swapchain->colorSpace;
    swapchain->format = format;
    swapchain->colorSpace = requestedColorSpace;
    const GPUError error = GPU_RecreateSwapchain(swapchain);
    if (error != GPUError_None) {
        swapchain->format = oldFormat;
        swapchain->colorSpace = oldColorSpace;
        // vkCreateSwapchainKHR retires oldSwapchain even when creation fails.
        if (!swapchain->handle) GPU_RequireError(device, GPU_RecreateSwapchain(swapchain), "Swapchain recreation");
    }
    return error;
}

GPUExtent2 GPUDeviceGetDrawableExtent(GPUDevice* deviceHandle) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device, "GPUDeviceGetDrawableExtent called with a null device");
    GPUAssert(!device->acquiredSwapchain, "GPUDeviceGetDrawableExtent must be called outside presentation");
    const GPUExtent2 none = {0};
    if (!device->swapchain) return none;

    VkSurfaceCapabilitiesKHR capabilities = {0};
    if (GPU_ErrorFromVk(
            device,
            device->fn
                .vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physicalDevice, device->surface, &capabilities),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
            0
        ) != GPUError_None)
        return none;
    const VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT_MAX || extent.height == UINT_MAX) {
        GPU_ReportKind(device, GPUMessageKind_SurfaceExtentUnavailable);
        return none;
    }

    GPUSwapchain* swapchain = device->swapchain;
    if (swapchain->handle && (swapchain->width != extent.width || swapchain->height != extent.height))
        swapchain->recreateRequired = true;
    return (GPUExtent2){
        .x = extent.width,
        .y = extent.height,
    };
}

GPUSwapchainFrame GPUCmdAcquire(GPUCommandBuffer* commands) {
    GPUAssert(
        commands && commands->state && !commands->swapchain,
        "commands && commands->state && !commands->swapchain"
    );
    GPUDeviceState* device = commands->state;
    GPUAssert(
        device->swapchain && !device->swapchain->acquired && !device->acquiredSwapchain,
        "device->swapchain && !device->swapchain->acquired && !device->acquiredSwapchain"
    );
    GPU_PollPresentationRetirement(device);

    GPUSwapchain* swapchain = device->swapchain;
    GPUPresentContext* presentContext = NULL;
    const GPUSwapchainFrame none = {0};

    for (;;) {
        if (!swapchain->handle || swapchain->recreateRequired) {
            GPU_RequireError(device, GPU_RecreateSwapchain(swapchain), "Swapchain recreation");
            if (!swapchain->handle || swapchain->width == 0 || swapchain->height == 0) return none;
        }

        if (!presentContext) {
            presentContext = &device->presentContexts[device->nextPresentContext];
            GPU_WaitPresentContext(device, presentContext);
            GPUAssert(
                !presentContext->presentPending && !presentContext->swapchain,
                "!presentContext->presentPending && !presentContext->swapchain"
            );
        }

        uint32_t imageIndex = 0;
        const VkResult result = device->fn.vkAcquireNextImageKHR(
            device->device,
            swapchain->handle,
            ~(uint64_t)0,
            presentContext->acquired,
            VK_NULL_HANDLE,
            &imageIndex
        );
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchain->recreateRequired = true;
            continue;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            GPU_AbortVkFailure(device, result, "vkAcquireNextImageKHR", 0);
        GPUAssert(imageIndex < swapchain->imageCount, "swapchain returned an invalid image index");

        swapchain->imageIndex = imageIndex;
        swapchain->presentContext = presentContext;
        swapchain->acquired = true;
        swapchain->recreateRequired = result == VK_SUBOPTIMAL_KHR && GPU_SwapSurfaceConfigurationChanged(swapchain);
        device->acquiredSwapchain = swapchain;
        const VkImageMemoryBarrier2 barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .oldLayout =
                swapchain->initialized[imageIndex] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain->images[imageIndex],
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        GPU_RecordImageBarriers(commands->state, commands->commandBuffer, &barrier, 1);
        commands->swapchain = swapchain;
        swapchain->transitionCommands = commands;
        return (GPUSwapchainFrame){
            .renderView = &swapchain->renderViews[imageIndex],
            .extent = {.x = swapchain->width, .y = swapchain->height},
        };
    }
}

void GPUDeviceSubmitAndPresent(GPUDevice* deviceHandle, const GPUSubmitDesc* desc) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device, "device");
    GPUAssert(desc->commands.data || desc->commands.count == 0, "desc->commands.data || desc->commands.count == 0");
    GPUSwapchain* swapchain = device->swapchain;
    GPUAssert(
        swapchain && swapchain->acquired && swapchain->transitionCommands && swapchain->presentContext,
        "swapchain && swapchain->acquired && swapchain->transitionCommands && swapchain->presentContext"
    );
#if defined(GPU_DEBUG)
    bool containsSwapchain = false;
    for (uint64_t index = 0; index < desc->commands.count; ++index) {
        GPUAssert(
            desc->commands.data[index] &&
                (!desc->commands.data[index]->swapchain || desc->commands.data[index] == swapchain->transitionCommands),
            "desc->commands.data[index] && (!desc->commands.data[index]->swapchain || desc->commands.data[index] == "
            "swapchain->transitionCommands)"
        );
        if (desc->commands.data[index] == swapchain->transitionCommands) containsSwapchain = true;
    }
    GPUAssert(containsSwapchain, "presentation submission must include the acquired command buffer");
#endif
    GPUPresentContext* presentContext = swapchain->presentContext;
    GPUAssert(
        !presentContext->presentPending && !presentContext->swapchain,
        "!presentContext->presentPending && !presentContext->swapchain"
    );
    GPU_AssertVk(device->fn.vkResetFences(device->device, 1, &presentContext->presented));
    GPU_SubmitCommands(device, desc, 0, presentContext->acquired, presentContext->rendered);
    swapchain->transitionCommands->swapchain = NULL;
    swapchain->transitionCommands = NULL;
    swapchain->initialized[swapchain->imageIndex] = true;

    const VkSwapchainPresentFenceInfoKHR fenceInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
        .swapchainCount = 1,
        .pFences = &presentContext->presented,
    };
    const VkPresentInfoKHR presentInfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = &fenceInfo,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &presentContext->rendered,
        .swapchainCount = 1,
        .pSwapchains = &swapchain->handle,
        .pImageIndices = &swapchain->imageIndex,
    };
    const VkResult result = device->fn.vkQueuePresentKHR(device->queues[0].queue, &presentInfo);
    presentContext->presentPending = true;
    presentContext->swapchain = swapchain->handle;
    swapchain->presentContext = NULL;
    swapchain->acquired = false;
    device->acquiredSwapchain = NULL;
    device->nextPresentContext = (device->nextPresentContext + 1) % device->presentContextCount;
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
        swapchain->recreateRequired = true;
    else if (result == VK_SUBOPTIMAL_KHR)
        swapchain->recreateRequired = swapchain->recreateRequired || GPU_SwapSurfaceConfigurationChanged(swapchain);
    else if (result != VK_SUCCESS)
        GPU_AbortVkFailure(device, result, "vkQueuePresentKHR", 0);
}

// ---- gpu_resources.c ------------------------------------------------------------------------------------------------



typedef struct GPUResPreparedTexture {
    VkFormat viewFormats[GPU_FORMAT_COUNT];
    VkImageFormatListCreateInfo formatList;
    VkImageCreateInfo imageInfo;
} GPUResPreparedTexture;

static GPUTextureDesc GPU_ResolveTextureDesc(const GPUTextureDesc* desc) {
    GPUTextureDesc resolved = *desc;
    if (resolved.type == GPUTextureType_Undefined) resolved.type = GPUTextureType_2D;
    if (!resolved.extent.x) resolved.extent.x = 1;
    if (!resolved.extent.y) resolved.extent.y = 1;
    if (!resolved.extent.z) resolved.extent.z = 1;
    if (!resolved.mipLevels) resolved.mipLevels = 1;
    if (!resolved.layerCount) resolved.layerCount = 1;
    if (resolved.format == GPUFormat_Undefined) resolved.format = GPUFormat_Rgba8Unorm;
    if (!resolved.usage) resolved.usage = GPUTextureUsage_Sampled;
    return resolved;
}

static void GPU_ResPrepareTexture(GPUDeviceState* device, const GPUTextureDesc* desc, GPUResPreparedTexture* output) {
    memset(output, 0, sizeof(*output));
    output->formatList.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
    output->imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;

    VkImageUsageFlags usage = 0;
    if (desc->usage & GPUTextureUsage_Sampled) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc->usage & GPUTextureUsage_Storage) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (desc->usage & GPUTextureUsage_ColorAttachment) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc->usage & GPUTextureUsage_DepthStencilAttachment) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (desc->usage & GPUTextureUsage_TransferSource) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (desc->usage & GPUTextureUsage_TransferDestination) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    const VkImageType imageType = GPU_ToVkImageType(desc->type);
    const VkFormat format = GPU_ToVkFormat(desc->format);
    const VkFormatFeatureFlags2 sampledFeatures = GPU_RequiredFormatFeatures(GPUTextureUsage_Sampled);
    const VkFormatFeatureFlags2 storageFeatures = GPU_RequiredFormatFeatures(GPUTextureUsage_Storage);
    uint32_t viewFormatCount = 1;
    output->viewFormats[0] = format;
    for (uint32_t value = GPUFormat_Undefined + 1; desc->mutableFormat && value < GPU_FORMAT_COUNT; ++value) {
        const GPUFormat viewFormat = (GPUFormat)value;
        if (viewFormat == desc->format || !GPU_CompatibleViewFormats(desc->format, viewFormat)) continue;
        const VkFormatFeatureFlags2 viewFeatures = device->formatFeatures[value];
        const bool sampled =
            (desc->usage & GPUTextureUsage_Sampled) && (viewFeatures & sampledFeatures) == sampledFeatures;
        const bool storage =
            (desc->usage & GPUTextureUsage_Storage) && (viewFeatures & storageFeatures) == storageFeatures;
        if (!sampled && !storage) continue;
        GPUAssert(viewFormatCount < GPU_FORMAT_COUNT, "viewFormatCount < GPU_FORMAT_COUNT");
        output->viewFormats[viewFormatCount++] = GPU_ToVkFormat(viewFormat);
    }

    VkImageCreateFlags imageFlags = 0;
    if (desc->aliasable) imageFlags |= VK_IMAGE_CREATE_ALIAS_BIT;
    if (desc->type == GPUTextureType_Cube || desc->type == GPUTextureType_CubeArray)
        imageFlags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (viewFormatCount > 1) imageFlags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

    output->formatList.viewFormatCount = viewFormatCount;
    output->formatList.pViewFormats = output->viewFormats;
    output->imageInfo = (VkImageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = viewFormatCount > 1 ? &output->formatList : NULL,
        .flags = imageFlags,
        .imageType = imageType,
        .format = format,
        .extent =
            {
                .width = desc->extent.x,
                .height = desc->extent.y,
                .depth = desc->extent.z,
            },
        .mipLevels = desc->mipLevels,
        .arrayLayers = desc->layerCount,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = device->queueFamilyCount > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = device->queueFamilyCount,
        .pQueueFamilyIndices = device->queueFamilies,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
}

GPUTextureHeap GPUTextureHeapCreate(GPUDevice* deviceHandle, uint64_t byteCount) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device, "GPUTextureHeapCreate called with a null device");
    GPUTextureHeapOwner* owner = (GPUTextureHeapOwner*)GPU_PoolAlloc(&device->textureHeapPool);
    if (!owner) {
        GPU_ReportLimit(device, GPULimit_TextureHeaps);
        return (GPUTextureHeap){0};
    }
    owner->state = device;
    const VkMemoryAllocateInfo allocateInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = byteCount,
        .memoryTypeIndex = device->textureMemoryType,
    };
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkAllocateMemory(device->device, &allocateInfo, NULL, &owner->memory),
            "vkAllocateMemory (texture heap)",
            byteCount
        ) != GPUError_None) {
        GPU_PoolFree(&device->textureHeapPool, owner);
        return (GPUTextureHeap){0};
    }
    return (GPUTextureHeap){
        .size = byteCount,
        .owner = owner,
    };
}

void GPUTextureHeapDestroy(const GPUTextureHeap* heap) {
    if (!heap->owner) return;
    GPUDeviceState* device = heap->owner->state;
    device->fn.vkFreeMemory(device->device, heap->owner->memory, NULL);
    GPU_PoolFree(&device->textureHeapPool, (void*)heap->owner);
}

GPUSizeAlign GPUTextureGetSizeAlign(GPUDevice* deviceHandle, const GPUTextureDesc* userDesc) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device && userDesc, "GPUTextureGetSizeAlign called with a null device");
    const GPUTextureDesc resolved = GPU_ResolveTextureDesc(userDesc);
    const GPUTextureDesc* desc = &resolved;
    GPUResPreparedTexture texture;
    GPU_ResPrepareTexture(device, desc, &texture);
    VkImageFormatProperties properties;
    if (!GPU_SupportsImageCreateInfo(device, &texture.imageInfo, &properties)) {
        GPU_ReportKind(device, GPUMessageKind_TextureUnsupported);
        return (GPUSizeAlign){0};
    }
    const VkMemoryRequirements requirements = GPU_ImageMemoryRequirements(device, &texture.imageInfo);
    return (GPUSizeAlign){
        .size = requirements.size,
        .align = requirements.alignment,
    };
}

GPUTexture* GPUTextureCreate(
    GPUCommandBuffer* commands,
    const GPUTextureDesc* userDesc,
    const GPUTextureHeap* heap,
    uint64_t offset
) {
    GPUAssert(
        commands && commands->state && userDesc && heap && heap->owner,
        "commands && commands->state && userDesc && heap && heap->owner"
    );
    const GPUTextureDesc resolved = GPU_ResolveTextureDesc(userDesc);
    const GPUTextureDesc* desc = &resolved;
    GPUDeviceState* device = commands->state;
    GPUResPreparedTexture texture;
    GPU_ResPrepareTexture(device, desc, &texture);

    GPUTexture* result = (GPUTexture*)GPU_PoolAlloc(&device->texturePool);
    if (!result) {
        GPU_ReportLimit(device, GPULimit_Textures);
        return NULL;
    }
    result->state = device;
    result->width = desc->extent.x;
    result->height = desc->extent.y;
    result->depth = desc->extent.z;
    result->layerCount = desc->layerCount;
    result->type = desc->type;
    result->format = desc->format;
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkCreateImage(device->device, &texture.imageInfo, NULL, &result->image),
            "vkCreateImage",
            0
        ) != GPUError_None) {
        GPU_PoolFree(&device->texturePool, result);
        return NULL;
    }
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkBindImageMemory(device->device, result->image, heap->owner->memory, offset),
            "vkBindImageMemory",
            0
        ) != GPUError_None) {
        device->fn.vkDestroyImage(device->device, result->image, NULL);
        GPU_PoolFree(&device->texturePool, result);
        return NULL;
    }
    if (desc->aliasable) return result;

    const VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = result->image,
        .subresourceRange = {
            .aspectMask = GPU_ImageAspects(desc->format),
            .levelCount = desc->mipLevels,
            .layerCount = desc->layerCount,
        },
    };
    GPU_RecordImageBarriers(commands->state, commands->commandBuffer, &barrier, 1);
    return result;
}

void GPUTextureDestroy(GPUTexture* texture) {
    if (!texture) return;
    texture->state->fn.vkDestroyImage(texture->state->device, texture->image, NULL);
    GPU_PoolFree(&texture->state->texturePool, texture);
}

void GPUCmdActivateTextureAlias(GPUCommandBuffer* commands, GPUTexture* texture) {
    GPUAssert(
        commands && texture && commands->state == texture->state,
        "commands && texture && commands->state == texture->state"
    );
    // The outgoing image can have a different format and size, so its writes need a global memory dependency.
    const VkMemoryBarrier2 memoryBarrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &memoryBarrier,
    };
    commands->state->fn.vkCmdPipelineBarrier2(commands->commandBuffer, &dependency);
    const VkImageMemoryBarrier2 imageBarrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture->image,
        .subresourceRange = {
            .aspectMask = GPU_ImageAspects(texture->format),
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    };
    GPU_RecordImageBarriers(commands->state, commands->commandBuffer, &imageBarrier, 1);
}

GPURenderView* GPURenderViewCreate(GPUTexture* texture, const GPURenderViewDesc* desc) {
    GPUAssert(texture && texture->state && desc, "texture && texture->state && desc");
    GPUDeviceState* device = texture->state;

    uint32_t width = texture->width >> desc->mipLevel;
    uint32_t height = texture->height >> desc->mipLevel;
    if (width == 0) width = 1;
    if (height == 0) height = 1;
    GPURenderView* result = (GPURenderView*)GPU_PoolAlloc(&device->renderViewPool);
    if (!result) {
        GPU_ReportLimit(device, GPULimit_RenderViews);
        return NULL;
    }
    result->state = device;
    result->width = width;
    result->height = height;
    const VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = GPU_ToVkFormat(texture->format),
        .subresourceRange = {
            .aspectMask = GPU_ImageAspects(texture->format),
            .baseMipLevel = desc->mipLevel,
            .levelCount = 1,
            .baseArrayLayer = desc->slice,
            .layerCount = 1,
        },
    };
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkCreateImageView(device->device, &viewInfo, NULL, &result->view),
            "vkCreateImageView",
            0
        ) != GPUError_None) {
        GPU_PoolFree(&device->renderViewPool, result);
        return NULL;
    }
    return result;
}

void GPURenderViewDestroy(GPURenderView* renderView) {
    if (!renderView) return;
    GPUAssert(!renderView->swapchainView, "swapchain render views are owned by their swapchain");
    renderView->state->fn.vkDestroyImageView(renderView->state->device, renderView->view, NULL);
    GPU_PoolFree(&renderView->state->renderViewPool, renderView);
}

GPUTextureDescriptorHeap* GPUTextureDescriptorHeapCreate(GPUDevice* deviceHandle, uint32_t capacity) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device && capacity, "device && capacity");
    GPUTextureDescriptorHeap* heap = (GPUTextureDescriptorHeap*)GPU_PoolAlloc(&device->textureDescriptorHeapPool);
    if (!heap) {
        GPU_ReportLimit(device, GPULimit_TextureDescriptorHeaps);
        return NULL;
    }
    heap->state = device;
    heap->storage = GPU_AllocateDescriptorHeap(
        device,
        (uint64_t)capacity * device->heapProperties.imageDescriptorSize,
        GPUDescriptorHeapType_Texture
    );
    heap->capacity = capacity;
    if (!heap->storage.owner) {
        GPU_PoolFree(&device->textureDescriptorHeapPool, heap);
        return NULL;
    }
    return heap;
}

void GPUTextureDescriptorHeapDestroy(GPUTextureDescriptorHeap* heap) {
    if (!heap) return;
    GPUGpuHeapDestroy(&heap->storage);
    GPU_PoolFree(&heap->state->textureDescriptorHeapPool, heap);
}

GPUSamplerDescriptorHeap* GPUSamplerDescriptorHeapCreate(GPUDevice* deviceHandle, uint32_t capacity) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device && capacity, "device && capacity");
    GPUSamplerDescriptorHeap* heap = (GPUSamplerDescriptorHeap*)GPU_PoolAlloc(&device->samplerDescriptorHeapPool);
    if (!heap) {
        GPU_ReportLimit(device, GPULimit_SamplerDescriptorHeaps);
        return NULL;
    }
    heap->state = device;
    heap->storage = GPU_AllocateDescriptorHeap(
        device,
        (uint64_t)capacity * device->heapProperties.samplerDescriptorSize,
        GPUDescriptorHeapType_Sampler
    );
    heap->capacity = capacity;
    if (!heap->storage.owner) {
        GPU_PoolFree(&device->samplerDescriptorHeapPool, heap);
        return NULL;
    }
    return heap;
}

void GPUSamplerDescriptorHeapDestroy(GPUSamplerDescriptorHeap* heap) {
    if (!heap) return;
    GPUGpuHeapDestroy(&heap->storage);
    GPU_PoolFree(&heap->state->samplerDescriptorHeapPool, heap);
}

void GPUTextureDescriptorHeapCopy(
    const GPUTextureDescriptorHeap* source,
    uint32_t sourceIndex,
    GPUTextureDescriptorHeap* destination,
    uint32_t destinationIndex,
    uint32_t count
) {
    GPUAssert(
        source && destination && source->state == destination->state,
        "source && destination && source->state == destination->state"
    );
    GPUAssert(
        sourceIndex <= source->capacity && count <= source->capacity - sourceIndex,
        "sourceIndex <= source->capacity && count <= source->capacity - sourceIndex"
    );
    GPUAssert(
        destinationIndex <= destination->capacity && count <= destination->capacity - destinationIndex,
        "destinationIndex <= destination->capacity && count <= destination->capacity - destinationIndex"
    );
    const uint64_t stride = source->state->heapProperties.imageDescriptorSize;
    memmove(
        destination->storage.range.cpu + (uint64_t)destinationIndex * stride,
        source->storage.range.cpu + (uint64_t)sourceIndex * stride,
        (size_t)(count * stride)
    );
}

void GPUSamplerDescriptorHeapCopy(
    const GPUSamplerDescriptorHeap* source,
    uint32_t sourceIndex,
    GPUSamplerDescriptorHeap* destination,
    uint32_t destinationIndex,
    uint32_t count
) {
    GPUAssert(
        source && destination && source->state == destination->state,
        "source && destination && source->state == destination->state"
    );
    GPUAssert(
        sourceIndex <= source->capacity && count <= source->capacity - sourceIndex,
        "sourceIndex <= source->capacity && count <= source->capacity - sourceIndex"
    );
    GPUAssert(
        destinationIndex <= destination->capacity && count <= destination->capacity - destinationIndex,
        "destinationIndex <= destination->capacity && count <= destination->capacity - destinationIndex"
    );
    const uint64_t stride = source->state->heapProperties.samplerDescriptorSize;
    memmove(
        destination->storage.range.cpu + (uint64_t)destinationIndex * stride,
        source->storage.range.cpu + (uint64_t)sourceIndex * stride,
        (size_t)(count * stride)
    );
}

void GPUTextureDescriptorHeapWrite(
    GPUTextureDescriptorHeap* heap,
    uint32_t index,
    const GPUTexture* texture,
    GPUTextureDescriptorType type,
    const GPUTextureDescriptorDesc* desc
) {
    GPUAssert(
        heap && texture && desc && index < heap->capacity && heap->state == texture->state,
        "heap && texture && desc && index < heap->capacity && heap->state == texture->state"
    );
    GPUDeviceState* device = heap->state;

    const VkImageUsageFlags descriptorUsage =
        (VkImageUsageFlags)(type == GPUTextureDescriptorType_Sampled ? VK_IMAGE_USAGE_SAMPLED_BIT
                                                                     : VK_IMAGE_USAGE_STORAGE_BIT);

    VkImageAspectFlags descriptorAspect = 0;
    switch (desc->aspect) {
    case GPUTextureAspect_Automatic:
        descriptorAspect = GPU_HasDepthAspect(texture->format)     ? VK_IMAGE_ASPECT_DEPTH_BIT
                           : GPU_HasStencilAspect(texture->format) ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                                   : VK_IMAGE_ASPECT_COLOR_BIT;
        break;
    case GPUTextureAspect_Color: descriptorAspect = VK_IMAGE_ASPECT_COLOR_BIT; break;
    case GPUTextureAspect_Depth: descriptorAspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
    case GPUTextureAspect_Stencil: descriptorAspect = VK_IMAGE_ASPECT_STENCIL_BIT; break;
    }
    const VkImageViewUsageCreateInfo viewUsage = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = descriptorUsage,
    };
    const VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &viewUsage,
        .image = texture->image,
        .viewType = GPU_ToVkImageViewType(texture->type),
        .format = GPU_ToVkFormat(desc->format == GPUFormat_Undefined ? texture->format : desc->format),
        .subresourceRange = {
            .aspectMask = descriptorAspect,
            .baseMipLevel = desc->baseMip,
            .levelCount = desc->mipCount == 0 ? VK_REMAINING_MIP_LEVELS : desc->mipCount,
            .baseArrayLayer = desc->baseLayer,
            .layerCount = desc->layerCount == 0 ? VK_REMAINING_ARRAY_LAYERS : desc->layerCount,
        },
    };
    const VkImageDescriptorInfoEXT imageDescriptor = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
        .pView = &viewInfo,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkResourceDescriptorInfoEXT descriptorInfo = {
        .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
        .type = type == GPUTextureDescriptorType_Sampled ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                         : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .data = {.pImage = &imageDescriptor},
    };
    const VkHostAddressRangeEXT destination = {
        .address = heap->storage.range.cpu + (uint64_t)index * device->heapProperties.imageDescriptorSize,
        .size = device->heapProperties.imageDescriptorSize,
    };
    GPU_AssertVk(device->fn.vkWriteResourceDescriptorsEXT(device->device, 1, &descriptorInfo, &destination));
}

void GPUSamplerDescriptorHeapWrite(GPUSamplerDescriptorHeap* heap, uint32_t index, const GPUSamplerDesc* desc) {
    GPUAssert(heap && desc && index < heap->capacity, "heap && desc && index < heap->capacity");
    GPUDeviceState* device = heap->state;
    const VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = GPU_ToVkFilter(desc->magFilter),
        .minFilter = GPU_ToVkFilter(desc->minFilter),
        .mipmapMode = GPU_ToVkMipmapMode(desc->mipFilter),
        .addressModeU = (VkSamplerAddressMode)desc->addressU,
        .addressModeV = (VkSamplerAddressMode)desc->addressV,
        .addressModeW = (VkSamplerAddressMode)desc->addressW,
        .anisotropyEnable = desc->anisotropic ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = desc->anisotropic ? 4.0f : 1.0f,
        .compareEnable = desc->compareEnabled ? VK_TRUE : VK_FALSE,
        .compareOp =
            GPU_ToVkCompareOp(desc->compare == GPUCompareOp_Undefined ? GPUCompareOp_LessEqual : desc->compare),
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    const VkHostAddressRangeEXT destination = {
        .address = heap->storage.range.cpu + (uint64_t)index * device->heapProperties.samplerDescriptorSize,
        .size = device->heapProperties.samplerDescriptorSize,
    };
    GPU_AssertVk(device->fn.vkWriteSamplerDescriptorsEXT(device->device, 1, &samplerInfo, &destination));
}

// ---- gpu_pso.c ------------------------------------------------------------------------------------------------------



static void GPU_PsoAssertSpirv(GPUByteSpan code) {
    GPUAssert(
        code.data && code.size >= 5 * sizeof(uint32_t) && code.size % sizeof(uint32_t) == 0 &&
            (size_t)code.data % sizeof(uint32_t) == 0,
        "PSO stages require aligned SPIR-V binaries"
    );
    GPUAssert(*(const uint32_t*)code.data == 0x07230203u, "PSO stage does not contain SPIR-V");
    (void)code;
}

static const VkDescriptorSetAndBindingMappingEXT GPU_PsoRootMapping = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
    .bindingCount = 1,
    .resourceMask =
        VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT | VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT,
    .source = VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT,
    .sourceData = {.pushAddressOffset = 0},
};

static GPUPSO* GPU_PsoCreateRaster(
    GPUDeviceState* device,
    const GPUShaderStage* firstStage,
    const GPUShaderStage* fragment,
    GPUColorTargetDescSpan colorTargets,
    GPUFormat depthFormat,
    GPUFormat stencilFormat,
    const GPURasterizationState* rasterizationState,
    bool mesh,
    const GPUShaderStage* task
) {
    GPUAssert(device, "PSO creation called with a null device");
    GPU_PsoAssertSpirv(firstStage->code);
    if (task->code.size) GPU_PsoAssertSpirv(task->code);
    if (fragment->code.size) GPU_PsoAssertSpirv(fragment->code);
    GPUAssert(
        (colorTargets.count == 0 || colorTargets.data) && colorTargets.count <= GPU_MAX_COLOR_ATTACHMENTS,
        "color targets must fit the wrapper's attachment array"
    );

    GPUPSO* result = (GPUPSO*)GPU_PoolAlloc(&device->psoPool);
    if (!result) {
        GPU_ReportLimit(device, GPULimit_Psos);
        return NULL;
    }

    const VkShaderModuleCreateInfo taskModuleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = task->code.size,
        .pCode = (const uint32_t*)task->code.data,
    };
    const VkShaderModuleCreateInfo firstStageModuleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = firstStage->code.size,
        .pCode = (const uint32_t*)firstStage->code.data,
    };
    const VkShaderModuleCreateInfo fragmentModuleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fragment->code.size,
        .pCode = (const uint32_t*)fragment->code.data,
    };
    const VkShaderDescriptorSetAndBindingMappingInfoEXT mappings[] = {
        {.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
         .pNext = &taskModuleInfo,
         .mappingCount = 1,
         .pMappings = &GPU_PsoRootMapping},
        {.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
         .pNext = &firstStageModuleInfo,
         .mappingCount = 1,
         .pMappings = &GPU_PsoRootMapping},
        {.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
         .pNext = &fragmentModuleInfo,
         .mappingCount = 1,
         .pMappings = &GPU_PsoRootMapping},
    };
    const VkPipelineShaderStageCreateInfo stages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &mappings[0],
            .stage = VK_SHADER_STAGE_TASK_BIT_EXT,
            .pName = task->entryPoint ? task->entryPoint : "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &mappings[1],
            .stage = mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT,
            .pName = firstStage->entryPoint ? firstStage->entryPoint : "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = &mappings[2],
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pName = fragment->entryPoint ? fragment->entryPoint : "main",
        },
    };
    const VkPipelineVertexInputStateCreateInfo vertexInput = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    const VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode =
            (VkCullModeFlags)(rasterizationState->cull == GPUCullMode_None ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT),
        .frontFace = rasterizationState->cull == GPUCullMode_CounterClockwise ? VK_FRONT_FACE_CLOCKWISE
                                                                              : VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = rasterizationState->depthBiasConstant != 0.0f ||
                           rasterizationState->depthBiasClamp != 0.0f || rasterizationState->depthBiasSlope != 0.0f,
        .depthBiasConstantFactor = rasterizationState->depthBiasConstant,
        .depthBiasClamp = rasterizationState->depthBiasClamp,
        .depthBiasSlopeFactor = rasterizationState->depthBiasSlope,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineDepthStencilStateCreateInfo depthStencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };
    VkPipelineColorBlendAttachmentState colorAttachments[GPU_MAX_COLOR_ATTACHMENTS] = {0};
    for (uint64_t index = 0; index < colorTargets.count; ++index) {
        const GPUColorTargetDesc* target = &colorTargets.data[index];
        colorAttachments[index] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = target->blend.enabled,
            .srcColorBlendFactor = GPU_ToVkBlendFactor(target->blend.color.source),
            .dstColorBlendFactor = GPU_ToVkBlendFactor(target->blend.color.destination),
            .colorBlendOp = (VkBlendOp)target->blend.color.operation,
            .srcAlphaBlendFactor = GPU_ToVkBlendFactor(target->blend.alpha.source),
            .dstAlphaBlendFactor = GPU_ToVkBlendFactor(target->blend.alpha.destination),
            .alphaBlendOp = (VkBlendOp)target->blend.alpha.operation,
            .colorWriteMask = target->writeMask ? target->writeMask : 0xf,
        };
    }
    const VkPipelineColorBlendStateCreateInfo colorBlend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = (uint32_t)colorTargets.count,
        .pAttachments = colorTargets.count ? colorAttachments : NULL,
    };
    static const VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    };
    const VkPipelineDynamicStateCreateInfo dynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = (uint32_t)GPUArrayCount(dynamicStates),
        .pDynamicStates = dynamicStates,
    };
    VkFormat colorFormats[GPU_MAX_COLOR_ATTACHMENTS] = {0};
    for (uint64_t index = 0; index < colorTargets.count; ++index) {
        colorFormats[index] = GPU_ToVkFormat(colorTargets.data[index].format);
    }
    const VkPipelineRenderingCreateInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = (uint32_t)colorTargets.count,
        .pColorAttachmentFormats = colorTargets.count ? colorFormats : NULL,
        .depthAttachmentFormat = GPU_ToVkFormat(depthFormat),
        .stencilAttachmentFormat = GPU_ToVkFormat(stencilFormat),
    };
    const VkPipelineCreateFlags2CreateInfo flagsInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .pNext = &renderingInfo,
        .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
    };
    const VkGraphicsPipelineCreateInfo psoInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &flagsInfo,
        .stageCount = 1u + (task->code.size ? 1u : 0u) + (fragment->code.size ? 1u : 0u),
        .pStages = stages + (task->code.size ? 0u : 1u),
        .pVertexInputState = mesh ? NULL : &vertexInput,
        .pInputAssemblyState = mesh ? NULL : &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &colorBlend,
        .pDynamicState = &dynamicState,
        .basePipelineIndex = -1,
    };
    result->state = device;
    result->bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkCreateGraphicsPipelines(device->device, VK_NULL_HANDLE, 1, &psoInfo, NULL, &result->pso),
            "vkCreateGraphicsPipelines",
            0
        ) != GPUError_None) {
        GPU_PoolFree(&device->psoPool, result);
        return NULL;
    }
    return result;
}

GPUPSO* GPUPSOCreateGraphics(GPUDevice* deviceHandle, const GPUGraphicsPSODesc* desc) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    const GPUShaderStage noTask = {0};
    return GPU_PsoCreateRaster(
        device,
        &desc->vertex,
        &desc->fragment,
        desc->colorTargets,
        desc->depthFormat,
        desc->stencilFormat,
        &desc->rasterization,
        false,
        &noTask
    );
}

GPUPSO* GPUPSOCreateMesh(GPUDevice* deviceHandle, const GPUMeshPSODesc* desc) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    return GPU_PsoCreateRaster(
        device,
        &desc->mesh,
        &desc->fragment,
        desc->colorTargets,
        desc->depthFormat,
        desc->stencilFormat,
        &desc->rasterization,
        true,
        &desc->task
    );
}

GPUPSO* GPUPSOCreateCompute(GPUDevice* deviceHandle, const GPUShaderStage* compute) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device, "GPUPSOCreateCompute called with a null device");
    GPU_PsoAssertSpirv(compute->code);

    GPUPSO* result = (GPUPSO*)GPU_PoolAlloc(&device->psoPool);
    if (!result) {
        GPU_ReportLimit(device, GPULimit_Psos);
        return NULL;
    }

    const VkShaderModuleCreateInfo moduleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = compute->code.size,
        .pCode = (const uint32_t*)compute->code.data,
    };
    const VkShaderDescriptorSetAndBindingMappingInfoEXT mapping = {
        .sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
        .pNext = &moduleInfo,
        .mappingCount = 1,
        .pMappings = &GPU_PsoRootMapping,
    };
    const VkPipelineCreateFlags2CreateInfo flagsInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
    };
    const VkComputePipelineCreateInfo psoInfo = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = &flagsInfo,
        .stage =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = &mapping,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .pName = compute->entryPoint ? compute->entryPoint : "main",
            },
        .basePipelineIndex = -1,
    };
    result->state = device;
    result->bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkCreateComputePipelines(device->device, VK_NULL_HANDLE, 1, &psoInfo, NULL, &result->pso),
            "vkCreateComputePipelines",
            0
        ) != GPUError_None) {
        GPU_PoolFree(&device->psoPool, result);
        return NULL;
    }
    return result;
}

void GPUPSODestroy(GPUPSO* pso) {
    if (!pso) return;
    pso->state->fn.vkDestroyPipeline(pso->state->device, pso->pso, NULL);
    GPU_PoolFree(&pso->state->psoPool, pso);
}

// ---- gpu_commands.c -------------------------------------------------------------------------------------------------



GPUCommandPool* GPUCommandPoolCreate(GPUDevice* deviceHandle, uint32_t queueIndex) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device && queueIndex < device->queueCount, "GPUCommandPoolCreate requires an available queue index");
    GPUCommandPool* pool = (GPUCommandPool*)GPU_PoolAlloc(&device->commandPoolPool);
    if (!pool) {
        GPU_ReportLimit(device, GPULimit_CommandPools);
        return NULL;
    }
    pool->state = device;
    pool->timestamps = device->queues[queueIndex].timestamps;
    pool->buffers = (GPUCommandBuffer*)(pool + 1);
    uint64_t** destinations = (uint64_t**)(pool->buffers + device->limits.maxCommandBuffersPerPool);
    uint64_t* results =
        (uint64_t*)(destinations + (uint64_t)device->limits.maxCommandBuffersPerPool * device->timestampQueryCount);
    for (uint32_t index = 0; index < device->limits.maxCommandBuffersPerPool; ++index) {
        pool->buffers[index].timestampDestinations = destinations + (uint64_t)index * device->timestampQueryCount;
        pool->buffers[index].timestampResults = results + (uint64_t)index * device->timestampQueryCount;
    }
    const VkCommandPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = device->queues[queueIndex].familyIndex,
    };
    if (GPU_ErrorFromVk(
            device,
            device->fn.vkCreateCommandPool(device->device, &poolInfo, NULL, &pool->commandPool),
            "vkCreateCommandPool",
            0
        ) != GPUError_None) {
        GPU_PoolFree(&device->commandPoolPool, pool);
        return NULL;
    }
    return pool;
}

void GPUCommandPoolDestroy(GPUCommandPool* pool) {
    if (!pool) return;
    for (uint32_t index = 0; index < pool->bufferCount; ++index) {
        GPUCommandBuffer* commands = &pool->buffers[index];
        GPUAssert(
            !commands->swapchain,
            "an acquired swapchain image must be presented before destroying its command pool"
        );
        if (commands->timestampPool)
            pool->state->fn.vkDestroyQueryPool(pool->state->device, commands->timestampPool, NULL);
    }
    pool->state->fn.vkDestroyCommandPool(pool->state->device, pool->commandPool, NULL);
    GPU_PoolFree(&pool->state->commandPoolPool, pool);
}

void GPUCommandPoolReset(GPUCommandPool* pool) {
    GPUAssert(pool && pool->state && pool->commandPool, "pool && pool->state && pool->commandPool");
    for (GPUCommandBuffer* commands = pool->first; commands; commands = commands->next) {
        GPUAssert(
            !commands->swapchain,
            "an acquired swapchain image must be presented before resetting its command pool"
        );
        commands->submitted = false;
        commands->timestampCount = 0;
    }
    GPU_AssertVk(pool->state->fn.vkResetCommandPool(pool->state->device, pool->commandPool, 0));
    pool->nextBuffer = pool->first;
}

void GPUCommandPoolReadTimestamps(GPUCommandPool* pool) {
    GPUAssert(pool && pool->state, "pool && pool->state");
    for (GPUCommandBuffer* commands = pool->first; commands; commands = commands->next) {
        if (!commands->submitted || commands->timestampCount == 0) continue;
        GPU_AssertVk(pool->state->fn.vkGetQueryPoolResults(
            pool->state->device,
            commands->timestampPool,
            0,
            commands->timestampCount,
            sizeof(uint64_t) * commands->timestampCount,
            commands->timestampResults,
            sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT
        ));
        for (uint32_t index = 0; index < commands->timestampCount; ++index)
            *commands->timestampDestinations[index] = commands->timestampResults[index];
        commands->timestampCount = 0;
    }
}

GPUCommandBuffer* GPUCmdBegin(GPUCommandPool* pool) {
    GPUAssert(pool && pool->state && pool->commandPool, "pool && pool->state && pool->commandPool");
    GPUCommandBuffer* commands = pool->nextBuffer;
    if (commands) {
        pool->nextBuffer = commands->next;
    } else {
        GPUAssert(
            pool->bufferCount < pool->state->limits.maxCommandBuffersPerPool,
            "GPUCmdBegin exceeds maxCommandBuffersPerPool"
        );
        commands = &pool->buffers[pool->bufferCount];
        commands->state = pool->state;
        commands->commandPool = pool->commandPool;
        const VkCommandBufferAllocateInfo allocateInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool->commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        const VkResult allocated =
            pool->state->fn.vkAllocateCommandBuffers(pool->state->device, &allocateInfo, &commands->commandBuffer);
        if (GPU_ErrorFromVk(pool->state, allocated, "vkAllocateCommandBuffers", 0) != GPUError_None) {
            commands->commandBuffer = VK_NULL_HANDLE;
            return NULL;
        }
        if (pool->timestamps) {
            const VkQueryPoolCreateInfo queryInfo = {
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = pool->state->timestampQueryCount,
            };
            if (GPU_ErrorFromVk(
                    pool->state,
                    pool->state->fn.vkCreateQueryPool(pool->state->device, &queryInfo, NULL, &commands->timestampPool),
                    "vkCreateQueryPool",
                    0
                ) != GPUError_None) {
                pool->state->fn
                    .vkFreeCommandBuffers(pool->state->device, pool->commandPool, 1, &commands->commandBuffer);
                commands->commandBuffer = VK_NULL_HANDLE;
                commands->timestampPool = VK_NULL_HANDLE;
                return NULL;
            }
        }
        ++pool->bufferCount;
        if (pool->last)
            pool->last->next = commands;
        else
            pool->first = commands;
        pool->last = commands;
    }
    GPUAssert(!commands->swapchain, "!commands->swapchain");
    const VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    GPU_AssertVk(pool->state->fn.vkBeginCommandBuffer(commands->commandBuffer, &beginInfo));
    commands->timestampCount = 0;
    commands->submitted = false;
    commands->hasEpilogue = false;
    if (commands->timestampPool)
        pool->state->fn
            .vkResetQueryPool(pool->state->device, commands->timestampPool, 0, pool->state->timestampQueryCount);
    return commands;
}

void GPUCmdEnd(GPUCommandBuffer* commands) {
    GPUAssert(commands, "commands");
    VkCommandBuffer commandBuffer = commands->commandBuffer;
    if (commands->swapchain) {
        GPU_AssertVk(commands->state->fn.vkEndCommandBuffer(commandBuffer));
        if (!commands->epilogue) {
            const VkCommandBufferAllocateInfo allocateInfo = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = commands->commandPool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            GPU_RequireVk(
                commands->state,
                commands->state->fn
                    .vkAllocateCommandBuffers(commands->state->device, &allocateInfo, &commands->epilogue),
                "vkAllocateCommandBuffers (epilogue)",
                0
            );
        }
        const VkCommandBufferBeginInfo beginInfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        commandBuffer = commands->epilogue;
        GPU_AssertVk(commands->state->fn.vkBeginCommandBuffer(commandBuffer, &beginInfo));
        commands->hasEpilogue = true;
        GPUSwapchain* swapchain = commands->swapchain;
        GPUAssert(
            swapchain->acquired && swapchain->transitionCommands == commands,
            "swapchain->acquired && swapchain->transitionCommands == commands"
        );
        const VkImageMemoryBarrier2 barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain->images[swapchain->imageIndex],
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
        };
        GPU_RecordImageBarriers(commands->state, commandBuffer, &barrier, 1);
    }
    GPU_AssertVk(commands->state->fn.vkEndCommandBuffer(commandBuffer));
}

void GPUCmdWriteTimestamp(GPUCommandBuffer* commands, uint64_t* cpuDestination, GPUStage stage) {
    GPUAssert(commands && commands->state, "commands && commands->state");
    if (!commands->timestampPool) return;
    GPUAssert(
        commands->timestampCount < commands->state->timestampQueryCount,
        "commands->timestampCount < commands->state->timestampQueryCount"
    );
    GPUAssert(cpuDestination, "cpuDestination");
    commands->timestampDestinations[commands->timestampCount] = cpuDestination;
    commands->state->fn.vkCmdWriteTimestamp2(
        commands->commandBuffer,
        GPU_ToVkStages(stage),
        commands->timestampPool,
        commands->timestampCount++
    );
}

static void GPU_SubmitCommands(
    GPUDeviceState* device,
    const GPUSubmitDesc* desc,
    uint32_t queueIndex,
    VkSemaphore waitSemaphore,
    VkSemaphore signalSemaphore
) {
    GPUQueue* queue = &device->queues[queueIndex];
    GPUTimelineSemaphore* completion = desc->completion.semaphore;
    GPUAssert(completion, "completion");
    GPUAssert(desc->waits.data || desc->waits.count == 0, "desc->waits.data || desc->waits.count == 0");
    uint64_t commandCount = desc->commands.count;
    for (uint64_t index = 0; index < desc->commands.count; ++index) {
        GPUAssert(desc->commands.data[index], "desc->commands.data[index]");
        if (desc->commands.data[index]->hasEpilogue) ++commandCount;
    }
    GPUAssert(commandCount <= device->limits.maxSubmitCommandBuffers, "submit exceeds maxSubmitCommandBuffers");
    for (uint64_t index = 0; index < desc->commands.count; ++index) {
        queue->commandSubmitInfos[index] = (VkCommandBufferSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = desc->commands.data[index]->commandBuffer,
            .deviceMask = 1,
        };
    }
    // Presentation transitions must follow every suspended/resumed segment.
    uint64_t epilogueIndex = desc->commands.count;
    for (uint64_t index = 0; index < desc->commands.count; ++index) {
        GPUCommandBuffer* commands = desc->commands.data[index];
        if (commands->hasEpilogue) {
            queue->commandSubmitInfos[epilogueIndex++] = (VkCommandBufferSubmitInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .commandBuffer = commands->epilogue,
                .deviceMask = 1,
            };
        }
    }
    const uint64_t waitCount = desc->waits.count + (waitSemaphore ? 1 : 0);
    GPUAssert(waitCount <= (size_t)device->limits.maxSubmitWaits + 1, "GPUDeviceSubmit exceeds maxSubmitWaits");
    for (uint64_t index = 0; index < desc->waits.count; ++index) {
        const GPUTimelinePoint point = desc->waits.data[index];
        GPUAssert(point.semaphore, "point.semaphore");
        queue->waitSubmitInfos[index] = (VkSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = point.semaphore->semaphore,
            .value = point.value,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
    }
    if (waitSemaphore) {
        queue->waitSubmitInfos[desc->waits.count] = (VkSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = waitSemaphore,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
    }
    VkSemaphoreSubmitInfo signalInfos[3] = {
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = completion->semaphore,
            .value = desc->completion.value,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        },
    };
    uint32_t signalCount = 1;
    if (signalSemaphore) {
        signalInfos[signalCount++] = (VkSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = signalSemaphore,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
        signalInfos[signalCount++] = (VkSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = device->presentationRetirement,
            .value = GPU_NextPresentationRetirement(device),
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
    }
    const VkSubmitInfo2 submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = (uint32_t)waitCount,
        .pWaitSemaphoreInfos = queue->waitSubmitInfos,
        .commandBufferInfoCount = (uint32_t)commandCount,
        .pCommandBufferInfos = queue->commandSubmitInfos,
        .signalSemaphoreInfoCount = signalCount,
        .pSignalSemaphoreInfos = signalInfos,
    };
    GPU_AssertVk(device->fn.vkQueueSubmit2(queue->queue, 1, &submitInfo, VK_NULL_HANDLE));
    for (uint64_t index = 0; index < desc->commands.count; ++index) desc->commands.data[index]->submitted = true;
}

void GPUDeviceSubmit(GPUDevice* deviceHandle, const GPUSubmitDesc* desc, uint32_t queueIndex) {
    GPUDeviceState* device = (GPUDeviceState*)deviceHandle;
    GPUAssert(device && desc && queueIndex < device->queueCount, "GPUDeviceSubmit requires an available queue index");
    GPUAssert(desc->commands.data || desc->commands.count == 0, "desc->commands.data || desc->commands.count == 0");
#if defined(GPU_DEBUG)
    for (uint64_t index = 0; index < desc->commands.count; ++index)
        GPUAssert(
            desc->commands.data[index] && !desc->commands.data[index]->swapchain,
            "swapchain commands require GPUDeviceSubmitAndPresent"
        );
#endif
    GPU_SubmitCommands(device, desc, queueIndex, VK_NULL_HANDLE, VK_NULL_HANDLE);
}

void GPUCmdBindPSO(GPUCommandBuffer* commands, const GPUPSO* pso) {
    GPUAssert(commands && pso, "commands && pso");
    commands->state->fn.vkCmdBindPipeline(commands->commandBuffer, pso->bindPoint, pso->pso);
}

static void GPU_CmdEmitRootPointer(GPUCommandBuffer* commands, const void* root) {
    if (!root) return;
    GPUAssert(((uintptr_t)root & 15u) == 0, "((uintptr_t)root & 15u) == 0");
    const VkPushDataInfoEXT info = {
        .sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
        .data = {.address = &root, .size = sizeof(root)},
    };
    commands->state->fn.vkCmdPushDataEXT(commands->commandBuffer, &info);
}

void GPUCmdSetTextureDescriptorHeap(GPUCommandBuffer* commands, GPUTextureDescriptorHeap* heap) {
    GPUAssert(commands && heap && commands->state == heap->state, "commands && heap && commands->state == heap->state");
    const VkPhysicalDeviceDescriptorHeapPropertiesEXT* properties = &commands->state->heapProperties;
    VkBindHeapInfoEXT bindInfo = {0};
    GPU_MakeHeapBindInfo(
        GPUGpuRangeOfHeap(&heap->storage),
        properties->imageDescriptorAlignment > properties->bufferDescriptorAlignment
            ? properties->imageDescriptorAlignment
            : properties->bufferDescriptorAlignment,
        properties->minResourceHeapReservedRange,
        &bindInfo
    );
    commands->state->fn.vkCmdBindResourceHeapEXT(commands->commandBuffer, &bindInfo);
}

void GPUCmdSetSamplerDescriptorHeap(GPUCommandBuffer* commands, GPUSamplerDescriptorHeap* heap) {
    GPUAssert(commands && heap && commands->state == heap->state, "commands && heap && commands->state == heap->state");
    const VkPhysicalDeviceDescriptorHeapPropertiesEXT* properties = &commands->state->heapProperties;
    VkBindHeapInfoEXT bindInfo = {0};
    GPU_MakeHeapBindInfo(
        GPUGpuRangeOfHeap(&heap->storage),
        properties->samplerDescriptorAlignment,
        properties->minSamplerHeapReservedRange,
        &bindInfo
    );
    commands->state->fn.vkCmdBindSamplerHeapEXT(commands->commandBuffer, &bindInfo);
}

void GPUCmdSetViewport(GPUCommandBuffer* commands, const GPUViewport* viewport) {
    GPUAssert(commands && viewport, "commands && viewport");
    const VkViewport vkViewport = {
        .x = viewport->x,
        .y = viewport->y,
        .width = viewport->width,
        .height = viewport->height,
        .minDepth = viewport->minDepth,
        .maxDepth = viewport->maxDepth == 0.0f ? 1.0f : viewport->maxDepth,
    };
    commands->state->fn.vkCmdSetViewportWithCount(commands->commandBuffer, 1, &vkViewport);
}

void GPUCmdSetScissor(GPUCommandBuffer* commands, const GPUScissor* scissor) {
    GPUAssert(commands && scissor, "commands && scissor");
    const VkRect2D vkScissor = {
        .offset = {.x = scissor->x, .y = scissor->y},
        .extent = {.width = scissor->width, .height = scissor->height},
    };
    commands->state->fn.vkCmdSetScissorWithCount(commands->commandBuffer, 1, &vkScissor);
}

void GPUCmdSetDepthStencil(GPUCommandBuffer* commands, const GPUDepthStencilState* state) {
    GPUAssert(commands && state, "commands && state");

    commands->state->fn.vkCmdSetDepthTestEnable(commands->commandBuffer, state->depthTest);
    if (state->depthTest) {
        commands->state->fn.vkCmdSetDepthWriteEnable(commands->commandBuffer, state->depthWrite);
        commands->state->fn.vkCmdSetDepthCompareOp(
            commands->commandBuffer,
            GPU_ToVkCompareOp(
                state->depthCompare == GPUCompareOp_Undefined ? GPUCompareOp_LessEqual : state->depthCompare
            )
        );
    }
    commands->state->fn.vkCmdSetStencilTestEnable(commands->commandBuffer, state->stencilTest);
    if (!state->stencilTest) return;

    commands->state->fn.vkCmdSetStencilOp(
        commands->commandBuffer,
        VK_STENCIL_FACE_FRONT_BIT,
        (VkStencilOp)state->front.fail,
        (VkStencilOp)state->front.pass,
        (VkStencilOp)state->front.depthFail,
        GPU_ToVkCompareOp(state->front.compare == GPUCompareOp_Undefined ? GPUCompareOp_Always : state->front.compare)
    );
    commands->state->fn.vkCmdSetStencilOp(
        commands->commandBuffer,
        VK_STENCIL_FACE_BACK_BIT,
        (VkStencilOp)state->back.fail,
        (VkStencilOp)state->back.pass,
        (VkStencilOp)state->back.depthFail,
        GPU_ToVkCompareOp(state->back.compare == GPUCompareOp_Undefined ? GPUCompareOp_Always : state->back.compare)
    );
    commands->state->fn
        .vkCmdSetStencilCompareMask(commands->commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, state->stencilReadMask);
    commands->state->fn
        .vkCmdSetStencilWriteMask(commands->commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, state->stencilWriteMask);
    commands->state->fn
        .vkCmdSetStencilReference(commands->commandBuffer, VK_STENCIL_FACE_FRONT_BIT, state->front.reference);
    commands->state->fn
        .vkCmdSetStencilReference(commands->commandBuffer, VK_STENCIL_FACE_BACK_BIT, state->back.reference);
}

void GPUCmdBeginRenderPass(GPUCommandBuffer* commands, const GPURenderingDesc* desc, GPURenderingFlags flags) {
    GPUAssert(
        commands && desc && (desc->colors.count == 0 || desc->colors.data) &&
            desc->colors.count <= GPU_MAX_COLOR_ATTACHMENTS,
        "commands && desc && (desc->colors.count == 0 || desc->colors.data) && desc->colors.count <= "
        "GPU_MAX_COLOR_ATTACHMENTS"
    );
    const GPURenderView* areaView = desc->colors.count
                                        ? desc->colors.data[0].renderView
                                        : (desc->depth.renderView ? desc->depth.renderView : desc->stencil.renderView);
    GPUAssert(areaView, "GPUCmdBeginRenderPass requires an attachment to determine the render area");

    VkRenderingAttachmentInfo colorAttachments[GPU_MAX_COLOR_ATTACHMENTS] = {0};
    for (uint64_t index = 0; index < desc->colors.count; ++index) {
        const GPUColorAttachment* attachment = &desc->colors.data[index];
        GPUAssert(attachment->renderView, "attachment->renderView");
        colorAttachments[index] = (VkRenderingAttachmentInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = attachment->renderView->view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = (VkAttachmentLoadOp)attachment->load,
            .storeOp = (VkAttachmentStoreOp)attachment->store,
            .clearValue = {
                .color = {
                    .float32 = {attachment->clear.x, attachment->clear.y, attachment->clear.z, attachment->clear.w}
                }
            },
        };
    }

    const VkRenderingAttachmentInfo depthAttachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = desc->depth.renderView ? desc->depth.renderView->view : VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = (VkAttachmentLoadOp)desc->depth.load,
        .storeOp = (VkAttachmentStoreOp)desc->depth.store,
        .clearValue = {.depthStencil = {.depth = desc->depth.clear}},
    };
    const VkRenderingAttachmentInfo stencilAttachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = desc->stencil.renderView ? desc->stencil.renderView->view : VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = (VkAttachmentLoadOp)desc->stencil.load,
        .storeOp = (VkAttachmentStoreOp)desc->stencil.store,
        .clearValue = {.depthStencil = {.stencil = desc->stencil.clear}},
    };
    const VkRenderingInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .flags = (VkRenderingFlags)flags,
        .renderArea = {.extent = {.width = areaView->width, .height = areaView->height}},
        .layerCount = 1,
        .colorAttachmentCount = (uint32_t)desc->colors.count,
        .pColorAttachments = colorAttachments,
        .pDepthAttachment = desc->depth.renderView ? &depthAttachment : NULL,
        .pStencilAttachment = desc->stencil.renderView ? &stencilAttachment : NULL,
    };
    commands->state->fn.vkCmdBeginRendering(commands->commandBuffer, &renderingInfo);

    const GPUViewport viewport = {.width = (float)areaView->width, .height = (float)areaView->height, .maxDepth = 1.0f};
    const GPUScissor scissor = {.width = areaView->width, .height = areaView->height};
    const GPUDepthStencilState depthStencil = {0};
    GPUCmdSetViewport(commands, &viewport);
    GPUCmdSetScissor(commands, &scissor);
    GPUCmdSetDepthStencil(commands, &depthStencil);
}

void GPUCmdEndRenderPass(GPUCommandBuffer* commands) {
    GPUAssert(commands, "commands");
    commands->state->fn.vkCmdEndRendering(commands->commandBuffer);
}

void GPUCmdDraw(
    GPUCommandBuffer* commands,
    const void* root,
    uint32_t vertexCount,
    uint32_t instanceCount,
    uint32_t firstVertex,
    uint32_t firstInstance
) {
    GPUAssert(commands, "commands");
    GPU_CmdEmitRootPointer(commands, root);
    commands->state->fn.vkCmdDraw(commands->commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

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
) {
    GPUAssert(commands && commands->state, "commands && commands->state");
    GPU_CmdEmitRootPointer(commands, root);
    const VkBindIndexBuffer3InfoKHR bindInfo = {
        .sType = VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        .addressRange = {.address = (VkDeviceAddress)(uintptr_t)indices.gpu, .size = indices.size},
        .addressFlags = GPU_ADDRESS_FLAGS,
        .indexType = (VkIndexType)type,
    };
    commands->state->fn.vkCmdBindIndexBuffer3KHR(commands->commandBuffer, &bindInfo);
    commands->state->fn
        .vkCmdDrawIndexed(commands->commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void GPUCmdDrawIndirect(
    GPUCommandBuffer* commands,
    const void* root,
    GPUGpuRange arguments,
    uint32_t drawCount,
    uint32_t stride
) {
    GPUAssert(commands && commands->state, "commands && commands->state");
    GPU_CmdEmitRootPointer(commands, root);
    const VkDrawIndirect2InfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .addressRange =
            {.address = (VkDeviceAddress)(uintptr_t)arguments.gpu,
             .size = arguments.size,
             .stride = stride == 0 ? sizeof(VkDrawIndirectCommand) : stride},
        .addressFlags = GPU_ADDRESS_FLAGS,
        .drawCount = drawCount,
    };
    commands->state->fn.vkCmdDrawIndirect2KHR(commands->commandBuffer, &info);
}

void GPUCmdDrawIndexedIndirect(
    GPUCommandBuffer* commands,
    const void* root,
    GPUGpuRange indices,
    GPUIndexType type,
    GPUGpuRange arguments,
    uint32_t drawCount,
    uint32_t stride
) {
    GPUAssert(commands && commands->state, "commands && commands->state");
    GPU_CmdEmitRootPointer(commands, root);
    const VkBindIndexBuffer3InfoKHR bindInfo = {
        .sType = VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        .addressRange = {.address = (VkDeviceAddress)(uintptr_t)indices.gpu, .size = indices.size},
        .addressFlags = GPU_ADDRESS_FLAGS,
        .indexType = (VkIndexType)type,
    };
    commands->state->fn.vkCmdBindIndexBuffer3KHR(commands->commandBuffer, &bindInfo);
    const VkDrawIndirect2InfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .addressRange =
            {.address = (VkDeviceAddress)(uintptr_t)arguments.gpu,
             .size = arguments.size,
             .stride = stride == 0 ? sizeof(VkDrawIndexedIndirectCommand) : stride},
        .addressFlags = GPU_ADDRESS_FLAGS,
        .drawCount = drawCount,
    };
    commands->state->fn.vkCmdDrawIndexedIndirect2KHR(commands->commandBuffer, &info);
}

void GPUCmdDispatch(GPUCommandBuffer* commands, const void* root, GPUExtent3 groupCount) {
    GPUAssert(commands, "commands");
    GPU_CmdEmitRootPointer(commands, root);
    commands->state->fn.vkCmdDispatch(commands->commandBuffer, groupCount.x, groupCount.y, groupCount.z);
}

void GPUCmdDispatchIndirect(GPUCommandBuffer* commands, const void* root, GPUGpuRange arguments) {
    GPUAssert(commands && commands->state, "commands && commands->state");
    GPU_CmdEmitRootPointer(commands, root);
    const VkDispatchIndirect2InfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_DISPATCH_INDIRECT_2_INFO_KHR,
        .addressRange = {.address = (VkDeviceAddress)(uintptr_t)arguments.gpu, .size = arguments.size},
        .addressFlags = GPU_ADDRESS_FLAGS,
    };
    commands->state->fn.vkCmdDispatchIndirect2KHR(commands->commandBuffer, &info);
}

void GPUCmdDrawMeshlets(GPUCommandBuffer* commands, const void* root, GPUExtent3 groupCount) {
    GPUAssert(commands && commands->state, "commands && commands->state");
    GPU_CmdEmitRootPointer(commands, root);
    commands->state->fn.vkCmdDrawMeshTasksEXT(commands->commandBuffer, groupCount.x, groupCount.y, groupCount.z);
}

void GPUCmdDrawMeshletsIndirect(
    GPUCommandBuffer* commands,
    const void* root,
    GPUGpuRange arguments,
    uint32_t drawCount,
    uint32_t stride
) {
    GPUAssert(commands && commands->state, "commands && commands->state");
    GPUAssert(commands->state->caps.indirectMeshDraw, "commands->state->caps.indirectMeshDraw");
    GPU_CmdEmitRootPointer(commands, root);
    const VkDrawIndirect2InfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,
        .addressRange =
            {.address = (VkDeviceAddress)(uintptr_t)arguments.gpu,
             .size = arguments.size,
             .stride = stride == 0 ? sizeof(VkDrawMeshTasksIndirectCommandEXT) : stride},
        .addressFlags = GPU_ADDRESS_FLAGS,
        .drawCount = drawCount,
    };
    commands->state->fn.vkCmdDrawMeshTasksIndirect2EXT(commands->commandBuffer, &info);
}

void GPUCmdCopyMemory(GPUCommandBuffer* commands, GPUGpuRange source, GPUGpuRange destination) {
    GPUAssert(commands && commands->state, "commands && commands->state");
    const VkDeviceMemoryCopyKHR region = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_COPY_KHR,
        .srcRange = {.address = (VkDeviceAddress)(uintptr_t)source.gpu, .size = source.size},
        .srcFlags = GPU_ADDRESS_FLAGS,
        .dstRange = {.address = (VkDeviceAddress)(uintptr_t)destination.gpu, .size = destination.size},
        .dstFlags = GPU_ADDRESS_FLAGS,
    };
    const VkCopyDeviceMemoryInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_INFO_KHR,
        .regionCount = 1,
        .pRegions = &region,
    };
    commands->state->fn.vkCmdCopyMemoryKHR(commands->commandBuffer, &info);
}

void GPUCmdCopyMemoryToTexture(
    GPUCommandBuffer* commands,
    GPUGpuRange source,
    GPUTexture* destination,
    const GPUTextureCopyDesc* copy
) {
    GPUAssert(commands && commands->state && destination && copy, "commands && commands->state && destination && copy");
    const VkDeviceMemoryImageCopyKHR region = GPU_MakeTextureCopyRegion(destination, copy, source);
    const VkCopyDeviceMemoryImageInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR,
        .image = destination->image,
        .regionCount = 1,
        .pRegions = &region,
    };
    commands->state->fn.vkCmdCopyMemoryToImageKHR(commands->commandBuffer, &info);
}

void GPUCmdCopyTextureToMemory(
    GPUCommandBuffer* commands,
    GPUTexture* source,
    GPUGpuRange destination,
    const GPUTextureCopyDesc* copy
) {
    GPUAssert(commands && commands->state && source && copy, "commands && commands->state && source && copy");
    const VkDeviceMemoryImageCopyKHR region = GPU_MakeTextureCopyRegion(source, copy, destination);
    const VkCopyDeviceMemoryImageInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR,
        .image = source->image,
        .regionCount = 1,
        .pRegions = &region,
    };
    commands->state->fn.vkCmdCopyImageToMemoryKHR(commands->commandBuffer, &info);
}

void GPUCmdBarrier(
    GPUCommandBuffer* commands,
    GPUStage before,
    GPUAccess beforeAccess,
    GPUStage after,
    GPUAccess afterAccess
) {
    GPUAssert(commands, "commands");
    GPU_RecordBarrier(commands->state, commands->commandBuffer, before, beforeAccess, after, afterAccess);
}

// ---- gpu_utility.c --------------------------------------------------------------------------------------------------



static uint8_t* GPU_UtilOffsetPointer(uint8_t* pointer, uint64_t offset) { return pointer ? pointer + offset : NULL; }

GPUBumpAllocator GPUBumpAllocatorCreate(GPUGpuCpuRange storage) {
    GPUAssert(storage.size != 0, "storage.size != 0");
    GPUAssert(storage.cpu || storage.gpu, "storage.cpu || storage.gpu");
    GPUAssert(
        (!storage.cpu || (size_t)storage.cpu % GPU_ALLOCATION_ALIGNMENT == 0) &&
            (!storage.gpu || (size_t)storage.gpu % GPU_ALLOCATION_ALIGNMENT == 0),
        "(!storage.cpu || (size_t)storage.cpu % GPU_ALLOCATION_ALIGNMENT == 0) && (!storage.gpu || (size_t)storage.gpu "
        "% GPU_ALLOCATION_ALIGNMENT == 0)"
    );
    return (GPUBumpAllocator){.storage = storage, .offset = 0};
}

GPUGpuCpuRange GPUBumpAllocatorAllocate(GPUBumpAllocator* allocator, uint64_t byteSize) {
    GPUAssert(byteSize != 0, "byteSize != 0");
    if (byteSize > allocator->storage.size - allocator->offset) return (GPUGpuCpuRange){0};

    const GPUGpuCpuRange allocation = {
        .cpu = GPU_UtilOffsetPointer(allocator->storage.cpu, allocator->offset),
        .gpu = GPU_UtilOffsetPointer(allocator->storage.gpu, allocator->offset),
        .size = byteSize,
    };
    const uint64_t remaining = allocator->storage.size - allocator->offset;
    const uint64_t alignedSize = GPU_AlignUp(byteSize, GPU_ALLOCATION_ALIGNMENT);
    allocator->offset += alignedSize < remaining ? alignedSize : remaining;
    return allocation;
}

GPUGpuCpuRange GPUBumpAllocatorAllocateAtomic(GPUBumpAllocator* allocator, uint64_t byteSize) {
    GPUAssert(byteSize != 0, "byteSize != 0");
    uint64_t allocationOffset = GPU_AtomicLoadU64(&allocator->offset);
    for (;;) {
        if (byteSize > allocator->storage.size - allocationOffset) return (GPUGpuCpuRange){0};

        const uint64_t remaining = allocator->storage.size - allocationOffset;
        const uint64_t alignedSize = GPU_AlignUp(byteSize, GPU_ALLOCATION_ALIGNMENT);
        const uint64_t nextOffset = allocationOffset + (alignedSize < remaining ? alignedSize : remaining);
        if (GPU_AtomicCompareExchangeU64(&allocator->offset, &allocationOffset, nextOffset)) {
            return (GPUGpuCpuRange){
                .cpu = GPU_UtilOffsetPointer(allocator->storage.cpu, allocationOffset),
                .gpu = GPU_UtilOffsetPointer(allocator->storage.gpu, allocationOffset),
                .size = byteSize,
            };
        }
    }
}

void GPUBumpAllocatorReset(GPUBumpAllocator* allocator) { allocator->offset = 0; }

GPUDeleteQueue GPUDeleteQueueCreate(GPUTimelineSemaphore* timeline, GPUDeleteEntry* entries, uint32_t capacity) {
    GPUAssert(timeline, "timeline");
    GPUAssert(entries, "entries");
    GPUAssert(capacity != 0, "capacity != 0");
    return (GPUDeleteQueue){
        .timeline = timeline,
        .entries = entries,
        .capacity = capacity,
        .head = 0,
        .tail = 0,
        .count = 0,
        .ticking = false
    };
}

void GPUDeleteQueueDefer(GPUDeleteQueue* queue, uint64_t retireValue, GPUDeleteCallback callback, void* user) {
    GPUAssert(callback, "callback");
    GPUAssert(queue->count < queue->capacity, "delete queue capacity exhausted");
    GPUAssert(
        queue->count == 0 ||
            queue->entries[queue->tail == 0 ? queue->capacity - 1 : queue->tail - 1].retireValue <= retireValue,
        "delete queue retire values must be nondecreasing"
    );

    queue->entries[queue->tail] = (GPUDeleteEntry){.retireValue = retireValue, .callback = callback, .user = user};
    queue->tail = queue->tail + 1 == queue->capacity ? 0 : queue->tail + 1;
    ++queue->count;
}

// Callbacks cannot recursively collect the same queue.
static void GPU_UtilDeleteQueueCollect(GPUDeleteQueue* queue, uint64_t completedValue) {
    GPUAssert(!queue->ticking, "delete queue callbacks cannot recursively collect the same queue");
    queue->ticking = true;
    while (queue->count != 0) {
        GPUDeleteEntry* entry = &queue->entries[queue->head];
        if (entry->retireValue > completedValue) break;

        entry->callback(entry->user);
        entry->callback = NULL;
        entry->user = NULL;
        queue->head = queue->head + 1 == queue->capacity ? 0 : queue->head + 1;
        --queue->count;
    }
    queue->ticking = false;
}

void GPUDeleteQueueTick(GPUDeleteQueue* queue) {
    if (queue->count == 0) return;

    GPUAssert(queue->timeline, "queue->timeline");
    GPU_UtilDeleteQueueCollect(queue, GPUTimelineSemaphoreGetCompletedValue(queue->timeline));
}

void GPUDeleteQueueDrain(GPUDeleteQueue* queue) { GPU_UtilDeleteQueueCollect(queue, ~0ull); }


#endif // GPU_IMPLEMENTATION
