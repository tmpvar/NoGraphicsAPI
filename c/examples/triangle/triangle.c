#include "gpu.h"

#include <stdio.h>
#include <stdlib.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

static void reportError(void* user, const GPUMessage* message) {
    (void)user;
    char text[512];
    GPUMessageFormat(message, text, sizeof(text));
    fprintf(stderr, "%s%s\n", message->fatal ? "FATAL: " : "", text);
}

// Returns malloc'd shader bytes, or NULL when the file cannot be read.
static void* readShader(const char* path, size_t* outSize) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Cannot open %s\n", path);
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    const size_t byteCount = (size_t)ftell(file);
    rewind(file);
    void* code = malloc(byteCount);
    fread(code, 1, byteCount, file);
    fclose(file);
    *outSize = byteCount;
    return code;
}

static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            ShowWindow(hwnd, SW_HIDE);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcA(hwnd, message, wparam, lparam);
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcA(hwnd, message, wparam, lparam);
    }
}

// Fills desc->win32 with the new window. Returns false on failure.
static bool openWindow(GPUDeviceDesc* desc, const char* title, uint32_t width, uint32_t height) {
    const HINSTANCE instance = GetModuleHandleA(NULL);
    const WNDCLASSEXA windowClass = {
        .cbSize = sizeof(WNDCLASSEXA),
        .style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC,
        .lpfnWndProc = windowProc,
        .hInstance = instance,
        .hCursor = LoadCursorA(NULL, IDC_ARROW),
        .lpszClassName = "GPU_example_window",
    };
    if (!RegisterClassExA(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rectangle = {.right = (LONG)width, .bottom = (LONG)height};
    if (!AdjustWindowRectEx(&rectangle, style, FALSE, 0)) return false;

    const HWND hwnd = CreateWindowExA(
        0,
        windowClass.lpszClassName,
        title,
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rectangle.right - rectangle.left,
        rectangle.bottom - rectangle.top,
        NULL,
        NULL,
        instance,
        NULL
    );
    if (!hwnd) return false;
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    desc->win32.hwnd = hwnd;
    desc->win32.hinstance = instance;
    return true;
}

// Returns false when the window has been closed. Blocks while the window is minimized.
static bool pumpWindow(HWND window) {
    for (;;) {
        MSG message = {0};
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return false;
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }

        if (!IsIconic(window)) return true;
        WaitMessage();
    }
}

int main(void) {
    const uint32_t width = 512;
    const uint32_t height = 512;

    GPUDeviceDesc deviceDesc = {.swapchainFormat = GPUFormat_Bgra8Srgb, .errorCallback = reportError};
    if (!openWindow(&deviceDesc, "gpu.h triangle", width, height)) {
        fprintf(stderr, "Window creation failed.\n");
        return 1;
    }
    HWND window = deviceDesc.win32.hwnd;
    GPUDevice deviceStorage = {0};
    GPUDevice* device = &deviceStorage;
    if (!GPUDeviceInit(device, &deviceDesc, NULL)) {
        fprintf(stderr, "No compatible GPU device.\n");
        DestroyWindow(window);
        return 1;
    }

    printf("Using %s\n", GPUDeviceGetCaps(device)->deviceName);

    size_t vertexCodeSize = 0;
    size_t fragmentCodeSize = 0;
    void* vertexCode = readShader(GPU_VERTEX_SHADER_PATH, &vertexCodeSize);
    void* fragmentCode = readShader(GPU_FRAGMENT_SHADER_PATH, &fragmentCodeSize);
    GPUPSO* trianglePso = NULL;
    if (vertexCode && fragmentCode) {
        const GPUColorTargetDesc colorTarget = {.format = GPUFormat_Bgra8Srgb};
        const GPUGraphicsPSODesc psoDesc = {
            .vertex = {.code = GPUByteSpanOf(vertexCode, vertexCodeSize), .entryPoint = "vertexMain"},
            .fragment = {.code = GPUByteSpanOf(fragmentCode, fragmentCodeSize), .entryPoint = "fragmentMain"},
            .colorTargets = GPUColorTargets(colorTarget),
        };
        trianglePso = GPUPSOCreateGraphics(device, &psoDesc);
    }
    free(fragmentCode);
    free(vertexCode);
    if (!trianglePso) {
        GPUDeviceDestroy(device);
        DestroyWindow(window);
        return 1;
    }

    GPUTimelinePoint latestCompletion = {.semaphore = GPUTimelineSemaphoreCreate(device, 0), .value = 0};
    GPUCommandPool* commandPools[] = {GPUCommandPoolCreate(device, 0), GPUCommandPoolCreate(device, 0)};

    while (pumpWindow(window)) {
        if (latestCompletion.value >= 2)
            GPUTimelineSemaphoreWait(
                (GPUTimelinePoint){.semaphore = latestCompletion.semaphore, .value = latestCompletion.value - 1}
            );
        GPUCommandPool* commandPool = commandPools[latestCompletion.value % 2];
        GPUCommandPoolReset(commandPool);
        GPUCommandBuffer* commands = GPUCmdBegin(commandPool);
        const GPUSwapchainFrame frame = GPUCmdAcquire(commands);
        if (!frame.renderView) continue;
        const GPUColorAttachment color =
            {.renderView = frame.renderView, .load = GPULoadOp_Clear, .clear = {.w = 1.0f}};
        const GPURenderingDesc rendering = {.colors = GPUColorAttachments(color)};
        GPUCmdBeginRenderPass(commands, &rendering, GPURenderingFlags_None);
        GPUCmdBindPSO(commands, trianglePso);
        GPUCmdDraw(commands, NULL, 3, 1, 0, 0);
        GPUCmdEndRenderPass(commands);
        GPUCmdEnd(commands);
        latestCompletion.value++;
        const GPUSubmitDesc submit =
            {.commands = GPUCommandBuffers(commands), .waits = {NULL, 0}, .completion = latestCompletion};
        GPUDeviceSubmitAndPresent(device, &submit);
    }

    GPUDeviceWaitIdle(device);

    GPUCommandPoolDestroy(commandPools[1]);
    GPUCommandPoolDestroy(commandPools[0]);
    GPUTimelineSemaphoreDestroy(latestCompletion.semaphore);
    GPUPSODestroy(trianglePso);

    GPUDeviceDestroy(device);
    DestroyWindow(window);
    return 0;
}
