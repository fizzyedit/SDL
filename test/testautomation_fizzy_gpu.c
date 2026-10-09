/**
 * GPU test suite for fizzy's patches
 *
 * "GPU: let each driver decide whether it can claim a transparent window":
 * SDL_ClaimWindowForGPUDevice() used to refuse every SDL_WINDOW_TRANSPARENT
 * window, though the Metal and Vulkan drivers composite the swapchain's alpha
 * for one. These tests claim a transparent window with each driver, clear it to
 * a premultiplied 50% red and present it. The opaque claims are the control: a
 * driver that cannot claim an opaque window says nothing about the patch.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_test.h>
#include "testautomation_fizzy.h"

const char *fizzyGPURequiredDrivers = NULL;

/* Private helpers */

static bool GPUDriverIsRequired(const char *driver)
{
    const char *list = fizzyGPURequiredDrivers;
    const size_t length = SDL_strlen(driver);

    while (list && *list) {
        const char *end = SDL_strchr(list, ',');
        const size_t item_length = end ? (size_t)(end - list) : SDL_strlen(list);

        if (item_length == length && SDL_strncasecmp(list, driver, length) == 0) {
            return true;
        }
        list = end ? end + 1 : NULL;
    }
    return false;
}

/**
 * Creates a window with the given flags, claims it for a GPU device made with
 * the given driver, then clears and presents one frame.
 *
 * Skips when the driver is unavailable, unless it is required.
 */
static int gpu_claimWindowWithDriver(const char *driver, SDL_WindowFlags flags)
{
    const SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB;
    SDL_GPUDevice *device;
    SDL_Window *window;
    SDL_GPUCommandBuffer *cmdbuf;
    SDL_GPUTexture *swapchain = NULL;
    Uint32 w = 0, h = 0;
    bool result;

    device = SDL_CreateGPUDevice(formats, false, driver);
    SDLTest_AssertPass("Call to SDL_CreateGPUDevice(..., \"%s\")", driver);
    if (!device) {
        if (GPUDriverIsRequired(driver)) {
            SDLTest_AssertCheck(false, "Validate that the required GPU driver '%s' is available, got error: %s", driver, SDL_GetError());
            return TEST_COMPLETED;
        }
        SDLTest_Log("GPU driver '%s' is not available (%s), skipping test", driver, SDL_GetError());
        return TEST_SKIPPED;
    }
    SDLTest_AssertCheck(SDL_strcmp(SDL_GetGPUDeviceDriver(device), driver) == 0,
                        "Validate the device's driver, expected: %s, got: %s", driver, SDL_GetGPUDeviceDriver(device));

    window = SDL_CreateWindow("testautomation_fizzy_gpu", 320, 240, flags);
    SDLTest_AssertPass("Call to SDL_CreateWindow('Title',320,240,%" SDL_PRIu64 ")", flags);
    SDLTest_AssertCheck(window != NULL, "Validate that returned window is not NULL, got error: %s", window ? "none" : SDL_GetError());
    if (!window) {
        SDL_DestroyGPUDevice(device);
        return TEST_COMPLETED;
    }

    /* If the video driver quietly dropped the flag, the claim tests nothing */
    if (flags & SDL_WINDOW_TRANSPARENT) {
        SDLTest_AssertCheck((SDL_GetWindowFlags(window) & SDL_WINDOW_TRANSPARENT) != 0, "Validate that the window is transparent");
    }

    result = SDL_ClaimWindowForGPUDevice(device, window);
    SDLTest_AssertPass("Call to SDL_ClaimWindowForGPUDevice()");
    SDLTest_AssertCheck(result, "Validate that the window was claimed, got error: %s", result ? "none" : SDL_GetError());

    if (result) {
        cmdbuf = SDL_AcquireGPUCommandBuffer(device);
        SDLTest_AssertPass("Call to SDL_AcquireGPUCommandBuffer()");
        SDLTest_AssertCheck(cmdbuf != NULL, "Validate that returned command buffer is not NULL, got error: %s", cmdbuf ? "none" : SDL_GetError());
        if (cmdbuf) {
            result = SDL_WaitAndAcquireGPUSwapchainTexture(cmdbuf, window, &swapchain, &w, &h);
            SDLTest_AssertPass("Call to SDL_WaitAndAcquireGPUSwapchainTexture()");
            SDLTest_AssertCheck(result, "Validate that a swapchain texture was acquired, got error: %s", result ? "none" : SDL_GetError());

            if (swapchain) {
                SDL_GPUColorTargetInfo target;
                SDL_GPURenderPass *pass;

                SDLTest_Log("Swapchain texture is %" SDL_PRIu32 "x%" SDL_PRIu32, w, h);
                SDL_zero(target);
                target.texture = swapchain;
                target.clear_color.r = 0.5f;
                target.clear_color.a = 0.5f;
                target.load_op = SDL_GPU_LOADOP_CLEAR;
                target.store_op = SDL_GPU_STOREOP_STORE;
                pass = SDL_BeginGPURenderPass(cmdbuf, &target, 1, NULL);
                SDLTest_AssertCheck(pass != NULL, "Validate that returned render pass is not NULL");
                if (pass) {
                    SDL_EndGPURenderPass(pass);
                }
            } else {
                /* Allowed, for example while the window is occluded */
                SDLTest_Log("No swapchain texture this frame");
            }

            result = SDL_SubmitGPUCommandBuffer(cmdbuf);
            SDLTest_AssertPass("Call to SDL_SubmitGPUCommandBuffer()");
            SDLTest_AssertCheck(result, "Validate that the command buffer was submitted, got error: %s", result ? "none" : SDL_GetError());
        }

        SDL_WaitForGPUIdle(device);
        SDL_ReleaseWindowFromGPUDevice(device, window);
        SDLTest_AssertPass("Call to SDL_ReleaseWindowFromGPUDevice()");
    }

    SDL_DestroyWindow(window);
    SDL_DestroyGPUDevice(device);
    return TEST_COMPLETED;
}

/* Test case functions */

/**
 * Claims an opaque window with the Metal driver (the control).
 */
static int SDLCALL gpu_claimOpaqueWindowMetal(void *arg)
{
    return gpu_claimWindowWithDriver("metal", 0);
}

/**
 * Claims a transparent window with the Metal driver.
 */
static int SDLCALL gpu_claimTransparentWindowMetal(void *arg)
{
    return gpu_claimWindowWithDriver("metal", SDL_WINDOW_TRANSPARENT);
}

/**
 * Claims an opaque window with the Vulkan driver (the control).
 */
static int SDLCALL gpu_claimOpaqueWindowVulkan(void *arg)
{
    return gpu_claimWindowWithDriver("vulkan", 0);
}

/**
 * Claims a transparent window with the Vulkan driver.
 */
static int SDLCALL gpu_claimTransparentWindowVulkan(void *arg)
{
    return gpu_claimWindowWithDriver("vulkan", SDL_WINDOW_TRANSPARENT);
}

/* ================= Test References ================== */

/* GPU test cases */
static const SDLTest_TestCaseReference gpuTestClaimOpaqueWindowMetal = {
    gpu_claimOpaqueWindowMetal, "gpu_claimOpaqueWindowMetal", "Claims an opaque window with the Metal driver and presents a frame", TEST_ENABLED
};

static const SDLTest_TestCaseReference gpuTestClaimTransparentWindowMetal = {
    gpu_claimTransparentWindowMetal, "gpu_claimTransparentWindowMetal", "Claims a transparent window with the Metal driver and presents a frame", TEST_ENABLED
};

static const SDLTest_TestCaseReference gpuTestClaimOpaqueWindowVulkan = {
    gpu_claimOpaqueWindowVulkan, "gpu_claimOpaqueWindowVulkan", "Claims an opaque window with the Vulkan driver and presents a frame", TEST_ENABLED
};

static const SDLTest_TestCaseReference gpuTestClaimTransparentWindowVulkan = {
    gpu_claimTransparentWindowVulkan, "gpu_claimTransparentWindowVulkan", "Claims a transparent window with the Vulkan driver and presents a frame", TEST_ENABLED
};

/* Sequence of GPU test cases */
static const SDLTest_TestCaseReference *gpuTests[] = {
    &gpuTestClaimOpaqueWindowMetal,
    &gpuTestClaimTransparentWindowMetal,
    &gpuTestClaimOpaqueWindowVulkan,
    &gpuTestClaimTransparentWindowVulkan,
    NULL
};

/* GPU test suite (global) */
SDLTest_TestSuiteReference fizzyGPUTestSuite = {
    "FizzyGPU",
    NULL,
    gpuTests,
    NULL
};
