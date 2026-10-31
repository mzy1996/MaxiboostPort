// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// libbbport_xess.so / bbport_xess.dll: the only part of the project that uses the Intel XeSS SDK
// (XeSS SDK 2.x, xess_vk.h / xess.h; the Vulkan-specific xessVK* entry points); the port loads it
// pointing at the SDK (see CMakeLists.txt). Unlike the DLSS bridge this one does not restrict to a
// single vendor: XeSS runs on any Vulkan GPU (Intel XMX, or its DP4a / generic path).
//
// This file targets the XeSS SDK 2.x API (xessVKCreateContext / xessVKInit / xessVKExecute). The
// bridge's C ABI (BbXessApi, BBPORT_XESS_BRIDGE_ABI) is owned by the port and is independent of the
// SDK version, so the wrapper here can change with the SDK without touching the renderer.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "bbport_xess_bridge.h" // Vulkan first: the XeSS headers expect its types

#include <xess/xess.h>
#include <xess/xess_vk.h>

namespace {

#ifdef _WIN32
constexpr char EngineVersion[] = "bbport-windows";
#else
constexpr char EngineVersion[] = "bbport-linux";
#endif

// BbXessFeature.quality is 0 (Ultra Quality) .. 4 (Ultra Performance); the renderer's comment in
// vk_xess.cpp fixes that ordering. XeSS 2.x expresses quality as an enum whose values are 100+.
constexpr xess_quality_settings_t QualityForIndex(int quality) {
    switch (std::clamp(quality, 0, 4)) {
    case 0: return XESS_QUALITY_SETTING_ULTRA_QUALITY;
    case 1: return XESS_QUALITY_SETTING_QUALITY;
    case 2: return XESS_QUALITY_SETTING_BALANCED;
    case 3: return XESS_QUALITY_SETTING_PERFORMANCE;
    case 4: return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
    }
    return XESS_QUALITY_SETTING_BALANCED;
}

struct State {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    xess_context_handle_t context{};
    BbXessFeature feature_desc{};
    bool has_desc{};
    bool device_ready{};
} state;

void Log(const char* message) {
    std::printf("XeSS: %s\n", message);
}

bool Check(const char* operation, xess_result_t result) {
    if (result != XESS_RESULT_SUCCESS) {
        std::printf("XeSS: %s failed: 0x%08x\n", operation, static_cast<unsigned>(result));
        return false;
    }
    return true;
}

xess_vk_image_view_info ToViewInfo(const BbXessImage& image) {
    xess_vk_image_view_info info{};
    info.imageView = image.view;
    info.image = image.image;
    info.subresourceRange = image.range;
    info.format = image.format;
    info.width = image.width;
    info.height = image.height;
    return info;
}

// XeSS 2.x names its required extensions as C strings; the port's contract wants VkExtensionProperties
// (so it can check them against the device). Convert on the fly and keep the result alive until the
// next call (the port consumes it synchronously inside CollectExtensions).
std::vector<VkExtensionProperties>& ExtensionStore() {
    static std::vector<VkExtensionProperties> store;
    return store;
}

int32_t InstanceExtensions(uint32_t* count, const VkExtensionProperties** extensions) {
    uint32_t n = 0;
    const char* const* names = nullptr;
    uint32_t min_api = 0;
    const xess_result_t result = xessVKGetRequiredInstanceExtensions(&n, &names, &min_api);
    if (result != XESS_RESULT_SUCCESS || n == 0) {
        if (count)
            *count = 0;
        if (extensions)
            *extensions = nullptr;
        return 0; // no extra instance extensions needed
    }
    auto& store = ExtensionStore();
    store.assign(n, VkExtensionProperties{});
    for (uint32_t i = 0; i < n; ++i) {
        std::strncpy(store[i].extensionName, names[i], VK_MAX_EXTENSION_NAME_SIZE - 1);
        store[i].extensionName[VK_MAX_EXTENSION_NAME_SIZE - 1] = '\0';
        store[i].specVersion = 0;
    }
    if (count)
        *count = n;
    if (extensions)
        *extensions = store.data();
    return 1;
}

int32_t DeviceExtensions(VkPhysicalDevice physical, uint32_t* count,
                         const VkExtensionProperties** extensions) {
    if (!state.instance || !physical) {
        if (count)
            *count = 0;
        if (extensions)
            *extensions = nullptr;
        return 0;
    }
    uint32_t n = 0;
    const char* const* names = nullptr;
    const xess_result_t result =
        xessVKGetRequiredDeviceExtensions(state.instance, physical, &n, &names);
    if (result != XESS_RESULT_SUCCESS || n == 0) {
        if (count)
            *count = 0;
        if (extensions)
            *extensions = nullptr;
        return 0; // no extra device extensions needed
    }
    auto& store = ExtensionStore();
    store.assign(n, VkExtensionProperties{});
    for (uint32_t i = 0; i < n; ++i) {
        std::strncpy(store[i].extensionName, names[i], VK_MAX_EXTENSION_NAME_SIZE - 1);
        store[i].extensionName[VK_MAX_EXTENSION_NAME_SIZE - 1] = '\0';
        store[i].specVersion = 0;
    }
    if (count)
        *count = n;
    if (extensions)
        *extensions = store.data();
    return 1;
}

int32_t Initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                   uint32_t /*queue_family*/, PFN_vkGetInstanceProcAddr /*get_instance_proc*/,
                   PFN_vkGetDeviceProcAddr /*get_device_proc*/) {
    if (state.device_ready)
        return 0;
    state.instance = instance;
    state.physical = physical;
    state.device = device;
    state.device_ready = true;
    Log("device ready (context is created lazily on first feature)");
    return 1;
}

// Build (or rebuild) the XeSS context for the resolution + quality the renderer asked for. XeSS 2.x
// binds the output resolution and quality into xessVKInit, so a feature change means a fresh context.
bool EnsureContext(const BbXessFeature& desc) {
    if (state.context) {
        xessDestroyContext(state.context);
        state.context = nullptr;
    }
    if (!Check("xessVKCreateContext",
               xessVKCreateContext(state.instance, state.physical, state.device, &state.context)))
        return false;

    xess_vk_init_params_t params{};
    params.outputResolution.x = desc.output_width;
    params.outputResolution.y = desc.output_height;
    params.qualitySetting = QualityForIndex(desc.quality);
    // Jittered motion vectors are what the renderer supplies. HDR input is linear; a tonemapped
    // (LDR) input instead asks XeSS to skip its tonemapping via XESS_INIT_FLAG_LDR_INPUT_COLOR.
    params.initFlags = XESS_INIT_FLAG_JITTERED_MV |
                      (desc.hdr ? 0u : static_cast<uint32_t>(XESS_INIT_FLAG_LDR_INPUT_COLOR));
    if (!Check("xessVKInit", xessVKInit(state.context, &params)))
        return false;

    xessSetVelocityScale(state.context, 1.0f, 1.0f);
    xessSetJitterScale(state.context, 1.0f, 1.0f);
    return true;
}

void ReleaseFeature() {
    if (state.context) {
        xessDestroyContext(state.context);
        state.context = nullptr;
    }
    state.has_desc = false;
}

int32_t CreateFeature(VkCommandBuffer, const BbXessFeature* desc) {
    if (!state.device_ready || !desc)
        return 0;
    if (state.has_desc && std::memcmp(&state.feature_desc, desc, sizeof(*desc)) == 0)
        return 1; // unchanged: keep the existing context
    if (!EnsureContext(*desc))
        return 0;
    state.feature_desc = *desc;
    state.has_desc = true;
    std::printf("XeSS: feature %ux%u -> %ux%u, quality %d%s\n", desc->input_width,
                desc->input_height, desc->output_width, desc->output_height, desc->quality,
                desc->hdr ? ", HDR input" : "");
    return 1;
}

int32_t Evaluate(VkCommandBuffer command, const BbXessEvaluate* evaluate) {
    if (!state.context || !evaluate)
        return 0;
    xess_vk_execute_params_t params{};
    params.colorTexture = ToViewInfo(evaluate->color);
    params.velocityTexture = ToViewInfo(evaluate->motion);
    params.depthTexture = ToViewInfo(evaluate->depth);
    params.outputTexture = ToViewInfo(evaluate->output);
    params.jitterOffsetX = evaluate->jitter_x;
    params.jitterOffsetY = evaluate->jitter_y;
    params.exposureScale = 1.0f;
    params.resetHistory = evaluate->reset ? 1u : 0u;
    params.inputWidth = state.feature_desc.input_width;
    params.inputHeight = state.feature_desc.input_height;
    return Check("xessVKExecute", xessVKExecute(state.context, command, &params)) ? 1 : 0;
}

void Shutdown() {
    ReleaseFeature();
    state.device_ready = false;
    state.instance = nullptr;
    state.physical = nullptr;
    state.device = nullptr;
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
