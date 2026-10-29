#include "infer/vulkan_loader.hpp"

#include <cstdlib>

namespace dale::infer {

// With DALE_VULKAN, vulkan_loader_available() lives in src/vulkan/vulkan_shim.cpp.
#if !DALE_VULKAN
bool vulkan_loader_available() { return false; }
#endif

void disable_vulkan_backend() { ::setenv("GGML_DISABLE_VULKAN", "1", 1); }

}  // namespace dale::infer
