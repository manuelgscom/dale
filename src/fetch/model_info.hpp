#pragma once
// Data of the model dale downloads. The single place that holds it: the final model of release 1.0.0.

#include <cstdint>

namespace dale::fetch {

struct ModelInfo {
    const char* filename;
    const char* url;
    std::uint64_t size;
    const char* sha256;
    const char* version;  // of the model, not of dale
    const char* quant;
};

// dale-0.8b 1.0.0: fine-tuned Qwen3.5-0.8B, Q4_K_M. Q8_0 does not get below 1 s warm.
// Hugging Face URL pinned to the upload commit. -DDALE_MODEL_URL=... overrides it at build
// time (CMakeLists.txt), e.g. for a download smoke test.
inline constexpr ModelInfo kDale08B_1_0_0_Q4_K_M{
    "dale-0.8b-1.0.0-Q4_K_M.gguf",
#ifdef DALE_MODEL_URL
    DALE_MODEL_URL,
#else
    "https://huggingface.co/manuelgscom/dale-0.8b-GGUF/resolve/"
    "7b0718dca0ef3fc2d9067066823b29239de3a849/dale-0.8b-1.0.0-Q4_K_M.gguf",
#endif
    529297952ULL,
    "8738d2535680cf7b8227001013a0ba6e6b2a4c8e8f3d149fd181c17cb96e2d3d",
    "1.0.0",
    "Q4_K_M",
};

inline constexpr const ModelInfo& kDefaultModel = kDale08B_1_0_0_Q4_K_M;

}  // namespace dale::fetch
