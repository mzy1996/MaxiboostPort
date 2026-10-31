// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// libbbport_xess.so / bbport_xess.dll: the only part of the project that uses the Intel XeSS SDK
// (XeSS-SDK, xess_vk.h / xess.h, version 1.3+); the port loads it at run time. Build with
// XESS_SDK_ROOT pointing at the SDK (see CMakeLists.txt). Unlike the DLSS bridge this one does not
// restrict to a single vendor: XeSS runs on any Vulkan GPU (Intel XMX, or its DP4a / generic path).
//
// Field names and version macros below follow the XeSS SDK 1.3 header layout; if you build against a
// different SDK release, align the xess_execute_params_t fields and XESS_EXECUTE_PARAMS_VERSION to
// the headers you have.

#include <array>
#include <cstdio>
#include <string>

#include "bbport_xess_bridge.h" // Vulkan first: the XeSS headers expect its types

#include <xess/xess.h>
#include <xess/xess_vk.h>

namespace {

#ifdef _WIN32
constexpr char EngineVersion[] = "bbport-windows";
#else
constexpr char EngineVersion[] = "bbport-linux";
#endif

// XeSS operating modes, indexed by BbXessFeature.quality (0 Ultra Quality .. 4 Ultra Performance).
constexpr xess_operating_mode_t QualityModes[] = {
    XESS_OPERATING_MODE_ULTRA_QUALITY,
    XESS_OPERATING_MODE_QUALITY,
    XESS_OPERATING_MODE_BALANCED,
    XESS_OPERATING_MODE_PERFORMANCE,
    XESS_OPERATING_MODE_ULTRA_PERFORMANCE,
};

struct State {
    BbXessLogFn log{};
    xess_context_handle_t context{};
    BbXessFeature feature_desc{};
    bool initialized{};
} state;

template <typename... Args>
void Log(int warning, const char* format, Args... args) {
    if (!state.log)
        return;
    if constexpr (sizeof...(Args) == 0) {
        state.log(warning, format); // a plain message (no format string without arguments)
    } else {
        std::array<char, 1024> text{};
        std::snprintf(text.data(), text.size(), format, args...);
        state.log(warning, text.data());
    }
}

bool Check(const char* operation, xess_result_t result) {
    if (result != XESS_RESULT_SUCCESS) {
        Log(1, "%s failed: 0x%08x", operation, static_cast<unsigned>(result));
        return false;
    }
    return true;
}

// XeSS is compute-based and needs no extra instance extensions; a 0 count is a valid empty list.
int32_t InstanceExtensions(uint32_t* count, const VkExtensionProperties** extensions) {
    if (count)
        *count = 0;
    if (extensions)
        *extensions = nullptr;
    return 0;
}

// XeSS needs no extra device extensions on any vendor; a 0 count is a valid empty list.
int32_t DeviceExtensions(VkPhysicalDevice, uint32_t* count,
                         const VkExtensionProperties** extensions) {
    if (count)
        *count = 0;
    if (extensions)
        *extensions = nullptr;
    return 0;
}

int32_t Initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                   uint32_t queue_family, PFN_vkGetInstanceProcAddr get_instance_proc,
                   PFN_vkGetDeviceProcAddr get_device_proc) {
    if (state.initialized)
        return 0;
    // The Vulkan init takes the graphics queue family XeSS dispatches its compute work on.
    const xess_result_t result =
        xessVulkanInit(&state.context, device, physical, instance, queue_family,
                       get_instance_proc, get_device_proc, nullptr);
    if (!Check("xessVulkanInit", result))
        return 0;
    state.initialized = true;
    // Motion vectors and jitter are supplied in the renderer's natural units.
    xessSetVelocityScale(state.context, 1.0f, 1.0f);
    xessSetJitterScale(state.context, 1.0f, 1.0f);
    Log(0, "XeSS: initialized (%s)", EngineVersion);
    return 1;
}

xess_resource_t ToResource(const BbXessImage& image, VkImageLayout layout) {
    xess_resource_t resource{};
    resource.type = XESS_RESOURCE_TYPE_VK_IMAGE_VIEW;
    resource.vk.imageView = image.view;
    resource.vk.image = image.image;
    resource.vk.format = image.format;
    resource.vk.width = image.width;
    resource.vk.height = image.height;
    resource.vk.state = layout;
    resource.vk.mipIdx = 0;
    resource.vk.subresourceIdx = 0;
    return resource;
}

void ReleaseFeature() {
    // XeSS keeps no persistent feature handle; its per-frame state lives in the execute parameters.
    // Nothing to release here beyond what Shutdown tears down.
}

int32_t CreateFeature(VkCommandBuffer, const BbXessFeature* desc) {
    if (!state.initialized || !desc)
        return 0;
    state.feature_desc = *desc;
    Log(0, "XeSS %ux%u -> %ux%u, quality %d%s", desc->input_width, desc->input_height,
        desc->output_width, desc->output_height, desc->quality, desc->hdr ? ", HDR input" : "");
    return 1;
}

int32_t Evaluate(VkCommandBuffer command, const BbXessEvaluate* evaluate) {
    if (!state.initialized || !evaluate)
        return 0;
    const BbXessFeature& desc = state.feature_desc;
    // Sharpness is a global XeSS setting; refresh it each frame from the renderer's request.
    xess_quality_settings_t quality{};
    quality.sharpness = evaluate->sharpness;
    xessSetQualitySettings(state.context, &quality);

    xess_execute_params_t parameters{};
    parameters.version = XESS_EXECUTE_PARAMS_VERSION;
    parameters.tfColor = ToResource(evaluate->color, VK_IMAGE_LAYOUT_GENERAL);
    parameters.tfVelocity = ToResource(evaluate->motion, VK_IMAGE_LAYOUT_GENERAL);
    parameters.tfDepth = ToResource(evaluate->depth, VK_IMAGE_LAYOUT_GENERAL);
    parameters.tfOutput = ToResource(evaluate->output, VK_IMAGE_LAYOUT_GENERAL);
    parameters.jitterOffsetX = evaluate->jitter_x;
    parameters.jitterOffsetY = evaluate->jitter_y;
    parameters.resetHistory = evaluate->reset;
    parameters.inputWidth = desc.input_width;
    parameters.inputHeight = desc.input_height;
    parameters.outputWidth = desc.output_width;
    parameters.outputHeight = desc.output_height;
    parameters.perfQuality = QualityModes[std::clamp(desc.quality, 0, 4)];
    parameters.initFlags = XESS_INIT_FLAG_ENABLE_JITTERED_MV |
                           (desc.hdr ? XESS_INIT_FLAG_ENABLE_HDR_COLOR : 0);
    // Build pipelines lazily on first use so initialization stays cheap.
    parameters.exeFlags = XESS_EXECUTION_FLAG_LAZY_LOAD_PIPELINES;
    parameters.dt = evaluate->frame_ms;
    return Check("xessExecute", xessExecute(state.context, &parameters)) ? 1 : 0;
}

void Shutdown() {
    ReleaseFeature();
    if (state.initialized) {
        xessShutdown(state.context);
        state.initialized = false;
        state.context = nullptr;
    }
}

const BbXessApi api{BBPORT_XESS_BRIDGE_ABI, InstanceExtensions, DeviceExtensions, Initialize,
                    CreateFeature,         Evaluate,           ReleaseFeature,  Shutdown};

} // namespace

#ifdef _WIN32
#define BBPORT_XESS_EXPORT __declspec(dllexport)
#else
#define BBPORT_XESS_EXPORT __attribute__((visibility("default")))
#endif

extern "C" BBPORT_XESS_EXPORT const BbXessApi* BbXessGetApi() {
    return &api;
}
