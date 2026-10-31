// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <ranges>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>

#include "video_core/renderer_vulkan/vk_xess.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../../../xess_bridge/bbport_xess_bridge.h"
#elif defined(__linux__)
#include <dlfcn.h>
#include "../../../xess_bridge/bbport_xess_bridge.h"
#endif

namespace Vulkan {

int Xess::QualityForScale(float scale) {
    // Ultra Quality only for near-equal sizes; a real upscale needs at least Quality.
    return scale >= 2.9f ? 4 : scale >= 1.95f ? 3 : scale >= 1.65f ? 2 : scale > 1.01f ? 1 : 0;
}

#if defined(_WIN32) || defined(__linux__)

namespace {
#ifdef _WIN32
constexpr wchar_t BridgeName[] = L"bbport_xess.dll";
constexpr wchar_t XessLibName[] = L"xess.dll";
using Module = HMODULE;

std::filesystem::path ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
}

bool HasXessLib(const std::filesystem::path& directory) {
    return std::filesystem::is_regular_file(directory / XessLibName);
}

Module LoadModule(const std::filesystem::path& path) {
    return LoadLibraryW(path.c_str());
}

void* FindSymbol(Module module, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(module, name));
}

/// The player's XeSS folder (<user>/xess, BB_GPU_USER_DIR; beside bb-probe without it): Intel's
/// library when the player overrides the package's, and the bridge's data directory.
std::filesystem::path UserXessDirectory() {
    if (const char* user = std::getenv("BB_GPU_USER_DIR"); user && user[0]) {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(std::filesystem::path{user} / "xess", error);
        return error ? std::filesystem::path{user} / "xess" : absolute;
    }
    return ExecutableDirectory() / "xess";
}

/// Where Intel's XeSS library is: the player's own first, then beside bb-probe; empty: nowhere.
std::filesystem::path XessLibDirectory() {
    for (const auto& directory : {UserXessDirectory(), ExecutableDirectory()}) {
        if (HasXessLib(directory)) {
            return directory;
        }
    }
    return {};
}

/// Where the bridge is: beside bb-probe (a build with XESS_SDK_ROOT), else the player's XeSS folder;
/// empty: nowhere.
std::filesystem::path BridgeDirectory() {
    for (const auto& directory : {ExecutableDirectory(), UserXessDirectory()}) {
        std::error_code error;
        if (std::filesystem::is_regular_file(directory / BridgeName, error)) {
            return directory;
        }
    }
    return {};
}
#else
// bbport (Linux): libbbport_xess.so (gpu/xess_bridge) and Intel's libxess.so next to bb-probe, or
// in BB_XESS_DIR (packaged builds whose /proc/self/exe is the dynamic loader); the bridge loads the
// Intel library itself from the same directory.
constexpr char BridgeName[] = "libbbport_xess.so";
using Module = void*;

std::filesystem::path ExecutableDirectory() {
    if (const char* dir = std::getenv("BB_XESS_DIR"); dir && dir[0]) {
        return dir;
    }
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
}

bool HasXessLib(const std::filesystem::path& directory) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (entry.path().filename().string().starts_with("libxess.so")) {
            return true;
        }
    }
    return false;
}

Module LoadModule(const std::filesystem::path& path) {
    Module module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        std::printf("XeSS: %s\n", dlerror());
    }
    return module;
}

void* FindSymbol(Module module, const char* name) {
    return dlsym(module, name);
}

std::filesystem::path UserXessDirectory() {
    if (const char* user = std::getenv("BB_GPU_USER_DIR"); user && user[0]) {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(std::filesystem::path{user} / "xess", error);
        return error ? std::filesystem::path{user} / "xess" : absolute;
    }
    return ExecutableDirectory() / "xess";
}

std::filesystem::path XessLibDirectory() {
    for (const auto& directory : {UserXessDirectory(), ExecutableDirectory()}) {
        if (HasXessLib(directory)) {
            return directory;
        }
    }
    return {};
}

std::filesystem::path BridgeDirectory() {
    for (const auto& directory : {ExecutableDirectory(), UserXessDirectory()}) {
        std::error_code error;
        if (std::filesystem::is_regular_file(directory / BridgeName, error)) {
            return directory;
        }
    }
    return {};
}
#endif

void BridgeLog(int warning, const char* message) {
    std::printf("XeSS: %s%s\n", warning ? "warning: " : "", message);
}

bool CollectExtensions(const VkExtensionProperties* required, u32 count,
                       const std::vector<vk::ExtensionProperties>& available,
                       std::vector<std::string>& storage, std::vector<const char*>& enabled) {
    storage.clear();
    for (u32 i = 0; i < count; ++i) {
        const auto& request = required[i];
        const auto match = std::ranges::find_if(available, [&](const auto& extension) {
            return std::strcmp(extension.extensionName.data(), request.extensionName) == 0 &&
                   extension.specVersion >= request.specVersion;
        });
        if (match == available.end()) {
            std::printf("XeSS: missing Vulkan extension %s\n", request.extensionName);
            return false;
        }
        storage.emplace_back(request.extensionName);
    }
    for (const auto& name : storage) {
        if (std::ranges::none_of(enabled, [&](const char* current) { return name == current; })) {
            enabled.push_back(name.c_str());
        }
    }
    return true;
}
} // namespace

struct Xess::Impl {
    Module module{};
    const BbXessApi* api{};
    std::vector<std::string> instance_extensions, device_extensions;
    std::optional<FeatureDesc> feature;
    std::string problem;
    bool eligible{true}, instance_ready{}, device_ready{}, initialized{}, available{};

    void Disable(std::string_view reason) {
        eligible = available = false;
        problem = reason;
        std::printf("XeSS: unavailable (%.*s)\n", int(reason.size()), reason.data());
    }
};

Xess* Xess::Get() {
    static Xess* const xess = []() -> Xess* {
        const char* setting = std::getenv("BB_XESS");
        if (setting && setting[0] == '0') {
            return nullptr;
        }
        if (BridgeDirectory().empty()) {
            return nullptr;
        }
        auto* created = new Xess;
        std::atexit([] {
            if (Xess* live = Get()) {
                live->Shutdown();
            }
        });
        return created;
    }();
    return xess;
}

Xess::Xess() : impl{std::make_unique<Impl>()} {
    impl->module = LoadModule(BridgeDirectory() / BridgeName);
    const auto get_api =
        impl->module ? reinterpret_cast<BbXessGetApiFn>(FindSymbol(impl->module, "BbXessGetApi"))
                    : nullptr;
    impl->api = get_api ? get_api() : nullptr;
    if (!impl->api || impl->api->abi != BBPORT_XESS_BRIDGE_ABI) {
        impl->Disable("the XeSS bridge library is missing or from another version");
        return;
    }
    const auto lib = XessLibDirectory();
    if (lib.empty()) {
        impl->Disable("Intel's XeSS library is missing: place xess.dll / libxess.so next to the "
                      "bridge (the Upscaler panel)");
        return;
    }
    std::printf("XeSS: Intel's library from %s\n", lib.string().c_str());
}

Xess::~Xess() {
    Shutdown();
}

void Xess::AppendInstanceExtensions(std::vector<const char*>& enabled) {
    if (!impl->eligible) {
        return;
    }
    u32 count{};
    const VkExtensionProperties* required{};
    if (!impl->api->InstanceExtensions(&count, &required)) {
        // No extra instance extensions needed (XeSS is compute-based); an empty list is valid.
        impl->instance_ready = true;
        return;
    }
    const auto [result, extensions] = vk::enumerateInstanceExtensionProperties();
    if (result != vk::Result::eSuccess ||
        !CollectExtensions(required, count, extensions, impl->instance_extensions, enabled)) {
        return impl->Disable("required Vulkan instance extensions are missing");
    }
    impl->instance_ready = true;
}

void Xess::AppendDeviceExtensions(vk::PhysicalDevice physical, std::vector<const char*>& enabled) {
    if (!impl->eligible || !impl->instance_ready) {
        return;
    }
    u32 count{};
    const VkExtensionProperties* required{};
    if (!impl->api->DeviceExtensions(physical, &count, &required)) {
        // No extra device extensions needed; XeSS runs on standard Vulkan.
        impl->device_ready = true;
        return;
    }
    const auto [result, extensions] = physical.enumerateDeviceExtensionProperties();
    if (result != vk::Result::eSuccess ||
        !CollectExtensions(required, count, extensions, impl->device_extensions, enabled)) {
        return impl->Disable("required Vulkan device extensions are missing");
    }
    impl->device_ready = true;
}

void Xess::Disable(std::string_view reason) {
    impl->Disable(reason);
}

void Xess::Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device,
                      uint32_t queue_family) {
    if (!impl->eligible || !impl->device_ready || impl->initialized) {
        return;
    }
    impl->initialized = true;
    if (!impl->api->Initialize(instance, physical, device, queue_family,
                               VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
                               VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr)) {
        return impl->Disable("Intel XeSS initialization failed");
    }
    impl->available = true;
    std::printf("XeSS: ready\n");
}

bool Xess::Available() const {
    return impl->available;
}

std::string Xess::Problem() const {
    return impl->problem;
}

bool Xess::HasFeature(const FeatureDesc& desc) const {
    return impl->feature && *impl->feature == desc;
}

bool Xess::CreateFeature(vk::CommandBuffer command, const FeatureDesc& desc) {
    if (!impl->available) {
        return false;
    }
    impl->feature.reset();
    const BbXessFeature feature{desc.input_width,  desc.input_height, desc.output_width,
                                desc.output_height, desc.quality,      desc.hdr ? 1 : 0};
    if (!impl->api->CreateFeature(command, &feature)) {
        return false;
    }
    impl->feature = desc;
    return true;
}

bool Xess::Evaluate(vk::CommandBuffer command, const Frame& frame) {
    if (!impl->available || !impl->feature) {
        return false;
    }
    const auto image = [](const Resource& r) {
        const vk::ImageSubresourceRange range{r.aspect, 0, 1, 0, 1};
        return BbXessImage{r.image,         r.view,
                          static_cast<VkImageSubresourceRange>(range),
                          static_cast<VkFormat>(r.format), r.width, r.height};
    };
    const BbXessEvaluate parameters{image(frame.color),  image(frame.depth), image(frame.motion),
                                    image(frame.output), frame.jitter_x,     frame.jitter_y,
                                    frame.reset ? 1 : 0, frame.frame_ms,     frame.sharpness};
    return impl->api->Evaluate(command, &parameters) != 0;
}

void Xess::ReleaseFeature() {
    impl->feature.reset();
    if (impl->api) {
        impl->api->ReleaseFeature();
    }
}

void Xess::Shutdown() {
    impl->available = false;
    impl->feature.reset();
    if (impl->api && impl->initialized) {
        impl->api->Shutdown();
        impl->initialized = false;
    }
}

#else

struct Xess::Impl {};
Xess::Xess() = default;
Xess::~Xess() = default;
Xess* Xess::Get() {
    return nullptr;
}
void Xess::AppendInstanceExtensions(std::vector<const char*>&) {}
void Xess::AppendDeviceExtensions(vk::PhysicalDevice, std::vector<const char*>&) {}
void Xess::Initialize(vk::Instance, vk::PhysicalDevice, vk::Device, uint32_t) {}
void Xess::Disable(std::string_view) {}
void Xess::Shutdown() {}
bool Xess::Available() const {
    return false;
}
std::string Xess::Problem() const {
    return {};
}
bool Xess::HasFeature(const FeatureDesc&) const {
    return false;
}
bool Xess::CreateFeature(vk::CommandBuffer, const FeatureDesc&) {
    return false;
}
bool Xess::Evaluate(vk::CommandBuffer, const Frame&) {
    return false;
}
void Xess::ReleaseFeature() {}

#endif

} // namespace Vulkan
