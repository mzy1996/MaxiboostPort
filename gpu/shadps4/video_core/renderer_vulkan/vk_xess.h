// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport: Intel XeSS (Xe Super Sampling) through the optional bridge (gpu/xess_bridge, MIT;
// libbbport_xess.so / bbport_xess.dll), loaded at run time from next to bb-probe together with
// Intel's libxess.so / xess.dll from the XeSS SDK. The port itself contains no Intel code; without
// the libraries, on other GPUs or with BB_XESS=0 nothing changes (FSR 3.1 is used instead).
//
// XeSS runs on any Vulkan GPU (it selects its Intel-XMX or DP4a / generic path internally), so this
// loader does not restrict to Intel — unlike DLSS it is not a single-vendor technology. Modeled on
// vk_dlss.h.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Xess {
public:
    /// The process-wide instance, or null when the DLL is absent or BB_XESS=0.
    static Xess* Get();
    ~Xess();

    /// Instance and device creation: adds what XeSS needs (or disables XeSS when it is missing).
    void AppendInstanceExtensions(std::vector<const char*>& enabled);
    void AppendDeviceExtensions(vk::PhysicalDevice physical, std::vector<const char*>& enabled);
    /// After the device exists.
    void Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device,
                    uint32_t queue_family);
    /// Device creation can fail despite advertised extensions.
    void Disable(std::string_view reason);
    void Shutdown();

    [[nodiscard]] bool Available() const;
    /// Why XeSS cannot run on this system (empty when it can, or before device creation).
    [[nodiscard]] std::string Problem() const;

    struct Resource {
        vk::Image image;
        vk::ImageView view;
        vk::Format format;
        vk::ImageAspectFlags aspect;
        u32 width, height;
    };
    struct FeatureDesc {
        u32 input_width, input_height, output_width, output_height;
        int quality;
        bool hdr;
        bool operator==(const FeatureDesc&) const = default;
    };
    struct Frame {
        Resource color, depth, motion, output;
        float jitter_x, jitter_y;
        bool reset;
        float frame_ms;
        float sharpness;
    };

    /// XeSS quality mode for an output/render size ratio (0 Ultra Quality .. 4 Ultra Performance).
    static int QualityForScale(float scale);
    [[nodiscard]] bool HasFeature(const FeatureDesc& desc) const;
    /// Records the feature creation. The GPU must be done with the previous one.
    bool CreateFeature(vk::CommandBuffer command, const FeatureDesc& desc);
    bool Evaluate(vk::CommandBuffer command, const Frame& frame);
    void ReleaseFeature();

private:
    Xess();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
