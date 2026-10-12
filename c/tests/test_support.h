// Shared helpers for the gpu.h tests: device storage slots, device creation with skip handling, SPIR-V loading, and
// threads.
#ifndef GPU_TEST_SUPPORT_H
#define GPU_TEST_SUPPORT_H

#include "gpu.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#define TEST_SKIPPED 77

#define TEST_ASSERT(condition, message)                                                   \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "ASSERT: %s:%u %s\n", __FILE__, (unsigned)__LINE__, message); \
            exit(1);                                                                      \
        }                                                                                 \
    } while (0)
#define TEST_MAX_DEVICES 4

// Device storage for a test process. A slot is reused once its device is destroyed.
static GPUDevice testDevices[TEST_MAX_DEVICES];
static bool testDeviceLive[TEST_MAX_DEVICES];

typedef struct TestDeviceInit {
    GPUDevice* device; // NULL when error is set.
    GPUError error;
} TestDeviceInit;

static inline TestDeviceInit TestCreateDevice(const GPUDeviceDesc* desc) {
    uint32_t slot = 0;
    while (slot < TEST_MAX_DEVICES && testDeviceLive[slot]) ++slot;
    TEST_ASSERT(slot < TEST_MAX_DEVICES, "test device slots exhausted");
    GPUError error;
    if (!GPUDeviceInit(&testDevices[slot], desc, &error)) return (TestDeviceInit){NULL, error};
    testDeviceLive[slot] = true;
    return (TestDeviceInit){&testDevices[slot], GPUError_None};
}

static inline void TestDestroyDevice(GPUDevice* device) {
    GPUDeviceDestroy(device);
    testDeviceLive[device - testDevices] = false;
}

// Exits with TEST_SKIPPED when no supported device exists and with failure on any other initialization error.
static inline GPUDevice* TestRequireDevice(const GPUDeviceDesc* desc) {
    const TestDeviceInit init = TestCreateDevice(desc);
    if (init.error == GPUError_Unsupported) exit(TEST_SKIPPED);
    if (init.error != GPUError_None) exit(1);
    return init.device;
}

// Reads a SPIR-V module into malloc storage. Returns an empty span after printing the failure; release with
// free((void*)span.data).
static inline GPUByteSpan TestLoadShader(const char* path) {
    const GPUByteSpan empty = {NULL, 0};
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Cannot open test shader: %s\n", path);
        return empty;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    if (size < 20 || size > 16 * 1024 * 1024) {
        fclose(file);
        return empty;
    }
    uint8_t* data = malloc((size_t)size);
    bool valid = fread(data, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    uint32_t magic = 0;
    memcpy(&magic, data, sizeof(magic));
    valid = valid && (size & 3) == 0 && magic == 0x07230203u;
    if (!valid) {
        fprintf(stderr, "Invalid test shader: %s\n", path);
        free(data);
        return empty;
    }
    return (GPUByteSpan){data, (size_t)size};
}

// ---------------------------------------------------------------------------------------------------------------------
// Threads. A gate releases every waiting thread at once so workers start recording together.
// ---------------------------------------------------------------------------------------------------------------------

typedef struct TestGate {
#if defined(_WIN32)
    HANDLE event;
#else
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool signaled;
#endif
} TestGate;

typedef struct TestThread {
    void (*run)(void* argument);
    void* argument;
    TestGate* start; // Optional; the thread waits for the gate before running.
#if defined(_WIN32)
    HANDLE handle;
#else
    pthread_t handle;
#endif
} TestThread;

static inline bool TestGateCreate(TestGate* gate) {
#if defined(_WIN32)
    gate->event = CreateEventW(NULL, TRUE, FALSE, NULL);
    return gate->event != NULL;
#else
    gate->signaled = false;
    if (pthread_mutex_init(&gate->mutex, NULL) != 0) return false;
    if (pthread_cond_init(&gate->condition, NULL) != 0) {
        pthread_mutex_destroy(&gate->mutex);
        return false;
    }
    return true;
#endif
}

static inline void TestGateWait(TestGate* gate) {
#if defined(_WIN32)
    WaitForSingleObject(gate->event, INFINITE);
#else
    pthread_mutex_lock(&gate->mutex);
    while (!gate->signaled) pthread_cond_wait(&gate->condition, &gate->mutex);
    pthread_mutex_unlock(&gate->mutex);
#endif
}

static inline void TestGateOpen(TestGate* gate) {
#if defined(_WIN32)
    SetEvent(gate->event);
#else
    pthread_mutex_lock(&gate->mutex);
    gate->signaled = true;
    pthread_cond_broadcast(&gate->condition);
    pthread_mutex_unlock(&gate->mutex);
#endif
}

static inline void TestGateDestroy(TestGate* gate) {
#if defined(_WIN32)
    CloseHandle(gate->event);
#else
    pthread_cond_destroy(&gate->condition);
    pthread_mutex_destroy(&gate->mutex);
#endif
}

#if defined(_WIN32)
static inline DWORD WINAPI testThreadMain(void* argument)
#else
static inline void* testThreadMain(void* argument)
#endif
{
    TestThread* thread = argument;
    if (thread->start) TestGateWait(thread->start);
    thread->run(thread->argument);
    return 0;
}

static inline bool TestThreadStart(TestThread* thread) {
#if defined(_WIN32)
    thread->handle = CreateThread(NULL, 0, testThreadMain, thread, 0, NULL);
    if (!thread->handle) fprintf(stderr, "CreateThread failed: %lu.\n", GetLastError());
    return thread->handle != NULL;
#else
    return pthread_create(&thread->handle, NULL, testThreadMain, thread) == 0;
#endif
}

static inline bool TestThreadJoin(TestThread* thread) {
#if defined(_WIN32)
    const bool joined = WaitForSingleObject(thread->handle, INFINITE) == WAIT_OBJECT_0;
    if (!joined) fprintf(stderr, "Thread join failed: %lu.\n", GetLastError());
    CloseHandle(thread->handle);
    return joined;
#else
    return pthread_join(thread->handle, NULL) == 0;
#endif
}

// Starts every thread behind one gate, releases them together, and joins them all.
static inline bool TestRunThreads(TestThread* threads, uint32_t count) {
    TestGate start;
    if (!TestGateCreate(&start)) return false;
    uint32_t started = 0;
    for (; started != count; ++started) {
        threads[started].start = &start;
        if (!TestThreadStart(&threads[started])) break;
    }
    TestGateOpen(&start);
    bool valid = started == count;
    if (!valid) fprintf(stderr, "Started %u of %u worker threads.\n", started, count);
    for (uint32_t index = 0; index != started; ++index) valid = TestThreadJoin(&threads[index]) && valid;
    TestGateDestroy(&start);
    return valid;
}

#endif // GPU_TEST_SUPPORT_H
