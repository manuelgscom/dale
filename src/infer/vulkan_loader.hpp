#pragma once
// Run-time loading of the Vulkan loader (libvulkan.so.1). dale does not link libvulkan: see
// src/vulkan/vulkan_shim.cpp. Without a loader the binary still starts and runs on the CPU.

namespace dale::infer {

// True if this build has the Vulkan backend and the system's libvulkan.so.1 could be opened.
// Opens the library on the first call; never throws.
bool vulkan_loader_available();

// Must run before the first llama/ggml call when the GPU is not wanted or not available:
// without it, ggml would initialize Vulkan (and, without a loader, call a null function).
void disable_vulkan_backend();

}  // namespace dale::infer
