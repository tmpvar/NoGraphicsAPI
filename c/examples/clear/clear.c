// Minimal gpu.h program: opens a window and clears the swapchain to a color that cycles over time. No shaders, no
// pipelines, no depth buffer. Pass a frame count as the only argument to exit after that many frames.
#include "gpu.h"

#include <math.h>
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

// Fills desc->win32 with a new hidden window. The caller shows it after the first frame is presented, so the default
// white client area never appears. Returns false on failure.
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

static double nowSeconds(void) {
    static LARGE_INTEGER frequency;
    static LARGE_INTEGER start;
    if (!frequency.QuadPart) {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
}

int main(int argc, char** argv) {
    const uint64_t frameLimit = argc > 1 ? strtoull(argv[1], NULL, 10) : 0;

    GPUDeviceDesc deviceDesc = {.swapchainFormat = GPUFormat_Bgra8Srgb, .errorCallback = reportError};
    if (!openWindow(&deviceDesc, "gpu.h clear", 1280, 720)) {
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

    GPUTimelinePoint latestCompletion = {.semaphore = GPUTimelineSemaphoreCreate(device, 0), .value = 0};
    GPUCommandPool* commandPools[] = {GPUCommandPoolCreate(device, 0), GPUCommandPoolCreate(device, 0)};

    while (pumpWindow(window) && (!frameLimit || latestCompletion.value < frameLimit)) {
        // Two frames in flight: wait for the frame that last used this command pool before resetting it.
        if (latestCompletion.value >= 2)
            GPUTimelineSemaphoreWait(
                (GPUTimelinePoint){.semaphore = latestCompletion.semaphore, .value = latestCompletion.value - 1}
            );
        GPUCommandPool* commandPool = commandPools[latestCompletion.value % 2];
        GPUCommandPoolReset(commandPool);
        GPUCommandBuffer* commands = GPUCmdBegin(commandPool);
        const GPUSwapchainFrame frame = GPUCmdAcquire(commands);
        if (!frame.renderView) continue;

        const float t = (float)nowSeconds();
        const GPUClearColor clear = {
            0.5f,
            0.5f + 0.5f * sinf(t + 2.0f),
            0.5f + 0.5f * sinf(t + 4.0f),
            1.0f,
        };
        const GPUColorAttachment color = {.renderView = frame.renderView, .load = GPULoadOp_Clear, .clear = clear};
        const GPURenderingDesc rendering = {.colors = GPUColorAttachments(color)};
        GPUCmdBeginRenderPass(commands, &rendering, GPURenderingFlags_None);
        GPUCmdEndRenderPass(commands);

        GPUCmdEnd(commands);
        latestCompletion.value++;
        const GPUSubmitDesc submit = {.commands = GPUCommandBuffers(commands), .completion = latestCompletion};
        GPUDeviceSubmitAndPresent(device, &submit);
        if (!IsWindowVisible(window)) ShowWindow(window, SW_SHOWDEFAULT);
    }

    GPUDeviceWaitIdle(device);
    GPUCommandPoolDestroy(commandPools[1]);
    GPUCommandPoolDestroy(commandPools[0]);
    GPUTimelineSemaphoreDestroy(latestCompletion.semaphore);
    GPUDeviceDestroy(device);
    DestroyWindow(window);
    return 0;
}
