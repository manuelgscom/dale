// Stand-in for libvulkan at link time (DALE_VULKAN builds only).
//
// ggml-vulkan calls almost everything through its own dispatcher, which starts from
// vkGetInstanceProcAddr; only the functions below are imported directly. They are defined here
// and forwarded to the same functions of libvulkan.so.1, opened with dlopen on first use, so
// the binary has no NEEDED entry for libvulkan and starts on machines without it.
// Without a loader there is no GPU: dale runs on the CPU and the first run tells the user to
// install it. The engine sets GGML_DISABLE_VULKAN on the CPU, so the forwarders never run
// without the library.

#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include <mutex>

#include "infer/vulkan_loader.hpp"

namespace {

std::once_flag g_once;
void* g_lib = nullptr;

void* loader() {
    std::call_once(g_once, [] {
        void* lib = ::dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (lib != nullptr && ::dlsym(lib, "vkGetInstanceProcAddr") == nullptr) {
            ::dlclose(lib);
            lib = nullptr;
        }
        g_lib = lib;
    });
    return g_lib;
}

// Function of libvulkan.so.1; nullptr without the library.
template <typename Fn>
Fn loader_symbol(const char* name) {
    void* lib = loader();
    return lib == nullptr ? nullptr : reinterpret_cast<Fn>(::dlsym(lib, name));
}

}  // namespace

bool dale::infer::vulkan_loader_available() { return loader() != nullptr; }

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    static const auto fn = loader_symbol<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    return fn == nullptr ? nullptr : fn(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
    static const auto fn = loader_symbol<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    return fn == nullptr ? nullptr : fn(device, name);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice device,
                                                        VkPhysicalDeviceFeatures2* features) {
    static const auto fn = loader_symbol<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
    if (fn != nullptr) fn(device, features);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer buffer, VkBuffer src, VkBuffer dst, uint32_t count,
                                           const VkBufferCopy* regions) {
    static const auto fn = loader_symbol<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    if (fn != nullptr) fn(buffer, src, dst, count, regions);
}

}  // extern "C"
