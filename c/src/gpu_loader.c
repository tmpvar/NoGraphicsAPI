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
