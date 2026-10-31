// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// C interface between bbport and the XeSS bridge (libbbport_xess.so / bbport_xess.dll). The port
// contains no Intel code: it loads the bridge at run time when it is present (next to bb-probe,
// with Intel's libxess.so / xess.dll from the XeSS SDK) and keeps its other upscalers when it is
// absent. XeSS is Intel's Xe Super Sampling; on non-Intel GPUs it falls back to its DP4a / generic
// path, so unlike DLSS this bridge does not restrict to one vendor.
//
// The bridge mirrors bbport_dlss_bridge.h; adjust the field names to the Intel XeSS SDK headers in
// use (xess_vk.h / xess.h, XeSS SDK 1.3+).

#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BBPORT_XESS_BRIDGE_ABI 1

typedef void (*BbXessLogFn)(int warning, const char* message);

typedef struct BbXessImage {
    VkImage image;
    VkImageView view;
    VkImageSubresourceRange range;
    VkFormat format;
    uint32_t width;
    uint32_t height;
} BbXessImage;

typedef struct BbXessFeature {
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    int32_t quality; // 0 Ultra Quality, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance
    int32_t hdr;     // 1: linear HDR colour input, 0: tonemapped colour
} BbXessFeature;

typedef struct BbXessEvaluate {
    BbXessImage color;  // sampled, render-resolution scene colour
    BbXessImage depth;  // sampled, hardware depth
    BbXessImage motion; // sampled, render-resolution motion vectors (current to previous)
    BbXessImage output; // storage, output resolution
    float jitter_x;
    float jitter_y;
    int32_t reset;
    float frame_ms;
    float sharpness; // 0..1; 0 leaves the output unsharpened
} BbXessEvaluate;

typedef struct BbXessApi {
    uint32_t abi;
    // Vulkan extensions XeSS needs. XeSS is compute-based and generally needs none beyond core,
    // but the arrays stay valid until Shutdown; a 0 count is valid.
    int32_t (*InstanceExtensions)(uint32_t* count, const VkExtensionProperties** extensions);
    int32_t (*DeviceExtensions)(VkPhysicalDevice physical, uint32_t* count,
                                const VkExtensionProperties** extensions);
    int32_t (*Initialize)(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                          uint32_t queue_family, PFN_vkGetInstanceProcAddr get_instance_proc,
                          PFN_vkGetDeviceProcAddr get_device_proc);
    int32_t (*CreateFeature)(VkCommandBuffer command, const BbXessFeature* feature);
    int32_t (*Evaluate)(VkCommandBuffer command, const BbXessEvaluate* evaluate);
    // The caller must have waited for the GPU work that used the feature.
    void (*ReleaseFeature)(void);
    void (*Shutdown)(void);
} BbXessApi;

typedef const BbXessApi* (*BbXessGetApiFn)(void);

#ifdef __cplusplus
}
#endif
