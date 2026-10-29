#include "infer/hardware.hpp"

#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <system_error>

#include "infer/llama_engine.hpp"
#include "llama.h"

namespace dale::infer {

namespace {

#if DALE_TEST_HOOKS
// "description:free_mib:total_mib[:igpu]" entries separated by ';'. Malformed entries are skipped.
std::vector<Gpu> fake_gpus(std::string_view spec) {
    std::vector<Gpu> out;
    while (!spec.empty()) {
        const std::size_t semi = spec.find(';');
        const std::string entry(spec.substr(0, semi));
        spec.remove_prefix(semi == std::string_view::npos ? spec.size() : semi + 1);
        const std::size_t a = entry.find(':');
        const std::size_t b = a == std::string::npos ? a : entry.find(':', a + 1);
        if (b == std::string::npos) continue;
        const std::size_t c = entry.find(':', b + 1);
        Gpu g;
        g.name = "Fake" + std::to_string(out.size());
        g.description = entry.substr(0, a);
        g.free_bytes = std::strtoull(entry.c_str() + a + 1, nullptr, 10) * 1024 * 1024;
        g.total_bytes = std::strtoull(entry.c_str() + b + 1, nullptr, 10) * 1024 * 1024;
        g.integrated = c != std::string::npos && entry.substr(c + 1) == "igpu";
        out.push_back(g);
    }
    return out;
}
#endif

}  // namespace

std::vector<Gpu> detect_gpus() {
#if DALE_TEST_HOOKS
    if (const char* fake = std::getenv("DALE_FAKE_GPU"); fake != nullptr) return fake_gpus(fake);
#endif
    set_llama_logging();
    std::vector<Gpu> out;
    for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        const enum ggml_backend_dev_type type = ggml_backend_dev_type(dev);
        if (type != GGML_BACKEND_DEVICE_TYPE_GPU && type != GGML_BACKEND_DEVICE_TYPE_IGPU) continue;
        Gpu g;
        g.name = ggml_backend_dev_name(dev);
        g.description = ggml_backend_dev_description(dev);
        g.integrated = type == GGML_BACKEND_DEVICE_TYPE_IGPU;
        std::size_t free = 0, total = 0;
        ggml_backend_dev_memory(dev, &free, &total);
        g.free_bytes = free;
        g.total_bytes = total;
        out.push_back(g);
    }
    return out;
}

bool gpu_device_node_present() {
    std::error_code ec;
    if (std::filesystem::exists("/dev/nvidia0", ec)) return true;
    for (const auto& e : std::filesystem::directory_iterator("/dev/dri", ec))
        if (e.path().filename().string().starts_with("renderD")) return true;
    return false;
}

Choice choose(const std::vector<Gpu>& gpus, bool forced) {
    // Best GPU: dedicated before integrated, then the most free VRAM.
    const Gpu* best = nullptr;
    std::size_t best_index = 0;
    for (std::size_t i = 0; i < gpus.size(); ++i) {
        const Gpu& g = gpus[i];
        if (g.integrated && !forced) continue;
        const bool better = best == nullptr || (best->integrated && !g.integrated) ||
                            (best->integrated == g.integrated && g.free_bytes > best->free_bytes);
        if (better) {
            best = &g;
            best_index = i;
        }
    }
    Choice c;
    if (best == nullptr) return c;
    if (best->free_bytes >= kMinFree9B && !best->integrated) {
        c.model = &fetch::kDale9B_1_0_0_Q4_K_M;
    } else if (best->free_bytes >= kMinFree4B) {
        c.model = &fetch::kDale4B_1_0_0_Q4_K_M;
    } else if (!forced) {
        return c;
    }
    c.gpu = true;
    c.gpu_index = best_index;
    return c;
}

}  // namespace dale::infer
