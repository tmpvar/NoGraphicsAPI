#include "test_support.h"

#define SENTINEL (~(uint64_t)0)

static int failures = 0;

#define CHECK(expression)                                                           \
    do {                                                                            \
        if (!(expression)) {                                                        \
            fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expression); \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

static void pumpMessages(void) {
    MSG message;
    while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
}

int main(int argc, char** argv) {
    const bool queueFamilies = argc == 2 && strcmp(argv[1], "--queue-families") == 0;
    const bool scrgb = argc == 2 && strcmp(argv[1], "--scrgb") == 0;
    const bool switchColorSpace = argc == 2 && strcmp(argv[1], "--switch-color-space") == 0;
    const WNDCLASSEXA windowClass = {
        .cbSize = sizeof(WNDCLASSEXA),
        .lpfnWndProc = DefWindowProcA,
        .hInstance = GetModuleHandleA(NULL),
        .lpszClassName = "gpu_presentation_test",
    };
    if (!RegisterClassExA(&windowClass)) {
        fprintf(stderr, "RegisterClassExA failed: %lu\n", GetLastError());
        return 1;
    }
    HWND window = CreateWindowExA(
        0,
        windowClass.lpszClassName,
        "gpu.h presentation test",
        WS_POPUP,
        0,
        0,
        256,
        192,
        NULL,
        NULL,
        windowClass.hInstance,
        NULL
    );
    if (!window) {
        fprintf(stderr, "CreateWindowExA failed: %lu\n", GetLastError());
        UnregisterClassA(windowClass.lpszClassName, windowClass.hInstance);
        return 1;
    }

    GPUDeviceDesc deviceDesc = {.timestampQueryCount = 256};
    deviceDesc.win32.hwnd = window;
    deviceDesc.win32.hinstance = windowClass.hInstance;
    deviceDesc.swapchainFormat = scrgb ? GPUFormat_Rgba16Float : GPUFormat_Bgra8Srgb;
    deviceDesc.swapchainColorSpace = scrgb ? GPUColorSpace_ExtendedSrgbLinear : GPUColorSpace_Srgb;
    deviceDesc.desiredQueueCount = 2;
    deviceDesc.desiredComputeQueueCount = queueFamilies ? 1u : 0u;
    deviceDesc.desiredCopyQueueCount = queueFamilies ? 1u : 0u;
    const TestDeviceInit deviceInit = TestCreateDevice(&deviceDesc);
    if (deviceInit.error != GPUError_None) {
        if (deviceInit.device) TestDestroyDevice(deviceInit.device);
        DestroyWindow(window);
        UnregisterClassA(windowClass.lpszClassName, windowClass.hInstance);
        if (deviceInit.error == GPUError_Unsupported) return TEST_SKIPPED;
        fprintf(stderr, "TestDeviceInit failed: %u\n", (unsigned)deviceInit.error);
        return 1;
    }

    GPUDevice* device = deviceInit.device;
    GPUCommandPool* presentPool = GPUCommandPoolCreate(device, 0);
    GPUCommandPool* middlePool = GPUCommandPoolCreate(device, 0);
    GPUCommandPool* lastPool = GPUCommandPoolCreate(device, 0);
    GPUCommandPool* independentPool = GPUCommandPoolCreate(device, 0);
    uint64_t timestampCpu[3] = {0, 0, 0};
    GPUTimelinePoint completion = {.semaphore = GPUTimelineSemaphoreCreate(device, 0), .value = 0};
    uint32_t width = 256;
    uint32_t height = 192;
    bool hdrOutput = scrgb;
    bool skipped = false;

    for (uint32_t frameIndex = 0; frameIndex != 8; ++frameIndex) {
        GPUCommandPoolReset(presentPool);
        GPUCommandPoolReset(middlePool);
        GPUCommandPoolReset(lastPool);
        GPUCommandPoolReset(independentPool);
        for (uint32_t index = 0; index != 3; ++index) timestampCpu[index] = SENTINEL;

        if (switchColorSpace) {
            GPUDeviceWaitIdle(device);
            if (frameIndex == 1 || frameIndex == 5 || frameIndex == 6) {
                const bool requestedHdr = frameIndex != 6;
                const GPUError error = GPUDeviceSetSwapchainFormat(
                    device,
                    requestedHdr ? GPUFormat_Rgba16Float : GPUFormat_Bgra8Srgb,
                    requestedHdr ? GPUColorSpace_ExtendedSrgbLinear : GPUColorSpace_Srgb
                );
                if (frameIndex == 1 && error == GPUError_Unsupported) {
                    skipped = true;
                    break;
                }
                CHECK(error == GPUError_None);
                if (error != GPUError_None) break;
                hdrOutput = requestedHdr;
            }
            // Rejection must preserve the active mode and leave the next acquire usable.
            CHECK(GPUDeviceSetSwapchainFormat(device, GPUFormat_D32Float, GPUColorSpace_Srgb) == GPUError_Unsupported);
            CHECK(
                GPUDeviceSetSwapchainFormat(
                    device,
                    hdrOutput ? GPUFormat_Rgba16Float : GPUFormat_Bgra8Srgb,
                    hdrOutput ? GPUColorSpace_ExtendedSrgbLinear : GPUColorSpace_Srgb
                ) == GPUError_None
            );
        }

        if (frameIndex == 2) {
            width = 384;
            height = 256;
            CHECK(SetWindowPos(window, NULL, 0, 0, (int)width, (int)height, SWP_NOZORDER | SWP_NOACTIVATE));
        }
        if (frameIndex == 4) {
            // A zero-size hidden window exercises minimized-surface behavior without showing a native window.
            CHECK(SetWindowPos(window, NULL, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE));
            pumpMessages();
            const GPUExtent2 emptyExtent = GPUDeviceGetDrawableExtent(device);
            CHECK(emptyExtent.x == 0 && emptyExtent.y == 0);
            if (switchColorSpace) {
                CHECK(GPUDeviceSetSwapchainFormat(device, GPUFormat_Bgra8Srgb, GPUColorSpace_Srgb) == GPUError_None);
                hdrOutput = false;
                CHECK(
                    GPUDeviceSetSwapchainFormat(device, GPUFormat_D32Float, GPUColorSpace_Srgb) == GPUError_Unsupported
                );
            }
            GPUCommandBuffer* emptyCommands = GPUCmdBegin(presentPool);
            const GPUSwapchainFrame emptyFrame = GPUCmdAcquire(emptyCommands);
            CHECK(!emptyFrame.renderView && emptyFrame.extent.x == 0 && emptyFrame.extent.y == 0);
            if (emptyFrame.renderView) {
                GPUCmdEnd(emptyCommands);
                ++completion.value;
                GPUDeviceSubmitAndPresent(
                    device,
                    &(GPUSubmitDesc){.commands = GPUCommandBuffers(emptyCommands), .completion = completion}
                );
                GPUTimelineSemaphoreWait(completion);
            }
            width = 320;
            height = 240;
            CHECK(SetWindowPos(window, NULL, 0, 0, (int)width, (int)height, SWP_NOZORDER | SWP_NOACTIVATE));
        }
        pumpMessages();
        const GPUExtent2 extent = GPUDeviceGetDrawableExtent(device);
        CHECK(extent.x == width && extent.y == height);
        GPUCommandBuffer* commands = GPUCmdBegin(presentPool);
        const GPUSwapchainFrame frame = GPUCmdAcquire(commands);
        CHECK(frame.renderView && frame.extent.x == width && frame.extent.y == height);
        if (!frame.renderView) break;

        GPUCommandBuffer* first = GPUCmdBegin(independentPool);
        GPUCmdBarrier(first, GPUStage_Transfer, GPUAccess_TransferWrite, GPUStage_Transfer, GPUAccess_TransferRead);
        GPUCmdEnd(first);
        GPUCommandBuffer* second = GPUCmdBegin(independentPool);
        GPUCmdBarrier(second, GPUStage_Transfer, GPUAccess_TransferWrite, GPUStage_Transfer, GPUAccess_TransferRead);

        // Submit unrelated work while the presentation buffer and another independent buffer are still recording.
        ++completion.value;
        GPUDeviceSubmit(device, &(GPUSubmitDesc){.commands = GPUCommandBuffers(first), .completion = completion}, 0);
        GPUColorAttachment color = {.renderView = frame.renderView, .load = GPULoadOp_Clear};
        color.clear = (GPUClearColor){
            .x = (float)frameIndex / 8.0f,
            .y = hdrOutput ? 2.5f : 0.25f,
            .z = hdrOutput ? 12.5f : 0.5f,
            .w = 1.0f
        };
        const GPURenderingDesc rendering = {.colors = {&color, 1}};
        const bool splitPass = (frameIndex & 1u) != 0;
        GPUCmdBeginRenderPass(commands, &rendering, splitPass ? GPURenderingFlags_Suspending : GPURenderingFlags_None);
        GPUCmdWriteTimestamp(commands, timestampCpu, GPUStage_AllCommands);
        GPUCmdEndRenderPass(commands);
        GPUCmdEnd(commands);
        GPUCmdEnd(second);
        ++completion.value;
        if (splitPass) {
            // Record continuations in reverse order, using the same clear description in every segment.
            GPUCommandBuffer* last = GPUCmdBegin(lastPool);
            GPUCmdBeginRenderPass(last, &rendering, GPURenderingFlags_Resuming);
            GPUCmdWriteTimestamp(last, timestampCpu + 2, GPUStage_AllCommands);
            GPUCmdEndRenderPass(last);
            GPUCmdEnd(last);

            GPUCommandBuffer* middle = GPUCmdBegin(middlePool);
            GPUCmdBeginRenderPass(middle, &rendering, GPURenderingFlags_Resuming | GPURenderingFlags_Suspending);
            GPUCmdWriteTimestamp(middle, timestampCpu + 1, GPUStage_AllCommands);
            GPUCmdEndRenderPass(middle);
            GPUCmdEnd(middle);
            GPUDeviceSubmitAndPresent(
                device,
                &(
                    GPUSubmitDesc
                ){.commands = GPUCommandBuffers(second, commands, middle, last), .completion = completion}
            );
        } else {
            GPUDeviceSubmitAndPresent(
                device,
                &(GPUSubmitDesc){.commands = GPUCommandBuffers(second, commands), .completion = completion}
            );
        }
        GPUTimelineSemaphoreWait(completion);
        GPUCommandPoolReadTimestamps(presentPool);
        GPUCommandPoolReadTimestamps(middlePool);
        GPUCommandPoolReadTimestamps(lastPool);
        CHECK(timestampCpu[0] != SENTINEL);
        if (splitPass) {
            CHECK(timestampCpu[1] != SENTINEL && timestampCpu[2] != SENTINEL);
            CHECK(timestampCpu[0] <= timestampCpu[1] && timestampCpu[1] <= timestampCpu[2]);
        } else {
            CHECK(timestampCpu[1] == SENTINEL && timestampCpu[2] == SENTINEL);
        }
    }

    GPUDeviceWaitIdle(device);
    GPUCommandPoolDestroy(independentPool);
    GPUCommandPoolDestroy(lastPool);
    GPUCommandPoolDestroy(middlePool);
    GPUCommandPoolDestroy(presentPool);
    GPUTimelineSemaphoreDestroy(completion.semaphore);
    TestDestroyDevice(device);
    CHECK(DestroyWindow(window));
    CHECK(UnregisterClassA(windowClass.lpszClassName, windowClass.hInstance));
    if (skipped && !failures) {
        puts("The surface does not support FP16 scRGB; presentation color-space switch test skipped.");
        return TEST_SKIPPED;
    }
    if (!failures)
        puts(
            "Presentation, color-space selection, suspended rendering, timestamp readback, resize, zero drawable, and "
            "independent submission checks passed."
        );
    return failures ? 1 : 0;
}
