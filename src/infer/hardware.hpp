#pragma once
// GPU detection through ggml (Vulkan) and the first-run choice of device and model.

#include <cstdint>
#include <string>
#include <vector>

#include "fetch/model_info.hpp"

namespace dale::infer {

struct Gpu {
    std::string name;         // ggml device name ("Vulkan0"); what LlamaOptions::gpu_device takes
    std::string description;  // "NVIDIA GeForce RTX 5070 Ti"
    bool integrated = false;  // shares the system RAM (iGPU)
    std::uint64_t free_bytes = 0;
    std::uint64_t total_bytes = 0;
};

// GPUs ggml sees through Vulkan (llvmpipe and other CPU devices are excluded by ggml).
// Initializes Vulkan: only call it with vulkan_loader_available() and the backend not disabled.
// With DALE_TEST_HOOKS, DALE_FAKE_GPU="description:free_mib:total_mib[:igpu];..." replaces the
// detection (no Vulkan at all).
std::vector<Gpu> detect_gpus();

// True if the kernel shows a GPU (/dev/nvidia0 or a /dev/dri/renderD* node), without Vulkan.
// Used to explain why there is no GPU when no Vulkan loader could be opened.
bool gpu_device_node_present();

// Free VRAM needed by each GPU model: what it takes in dale with a 2048 context (about 3.2 GiB
// for dale-4b and 5.3 GiB for dale-9b) plus a margin.
inline constexpr std::uint64_t kGiB = 1024ull * 1024 * 1024;
inline constexpr std::uint64_t kMinFree9B = 8 * kGiB;
inline constexpr std::uint64_t kMinFree4B = 4 * kGiB;

struct Choice {
    bool gpu = false;
    std::size_t gpu_index = 0;  // in the list given to choose()
    const fetch::ModelInfo* model = &fetch::kDefaultModel;
};

// Automatic choice: the dedicated GPU with most free VRAM; >= kMinFree9B => dale-9b fits (the
// CLI lets the user pick dale-4b or dale-9b), >= kMinFree4B => dale-4b; otherwise (or without
// a dedicated GPU) dale-0.8b on the CPU.
// forced (config device = gpu): integrated GPUs count too (never with dale-9b), and a GPU with
// little VRAM gets dale-0.8b on the GPU instead of the CPU.
Choice choose(const std::vector<Gpu>& gpus, bool forced);

}  // namespace dale::infer
