#pragma once
// llama.cpp inference engine.

#include <ctime>
#include <memory>
#include <string>

#include "infer/engine.hpp"

namespace dale::infer {

struct LlamaOptions {
    std::string model_path;
    int threads = 4;
    // ggml device name of the GPU (Gpu::name, "Vulkan0"); the whole model goes to it.
    // Empty = CPU only, even if the Vulkan backend is registered.
    std::string gpu_device;
};

// llama.cpp/ggml logs: silent, or to stderr with DALE_DEBUG=1. Called before any llama/ggml call
// that may log (loading, GPU detection).
void set_llama_logging();

// The model is loaded lazily on the first suggest(); creating the engine does not touch disk.
// The prompt prefix state is saved in $XDG_CACHE_HOME/dale (or ~/.cache/dale) and reused in
// later runs.
// Request::text arrives already normalized and within kMaxRequestBytes (the CLI checks it).
std::unique_ptr<Engine> make_llama_engine(const LlamaOptions& options);

// Prefix cache cleanup after saving dir/keep_name: removes the other prefix-*.state files
// (other models, dale versions or prompts) and .prefix.state-* temporaries older than one hour
// (a recent one may belong to another dale saving right now). A failure only leaves extra files.
void prune_prefix_cache(const std::string& dir, const std::string& keep_name, std::time_t now);

}  // namespace dale::infer
