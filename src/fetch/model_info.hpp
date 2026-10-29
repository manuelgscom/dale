#pragma once
// Data of the models dale downloads. The single place that holds it: dale-0.8b (CPU) and the
// larger dale-4b and dale-9b (GPU), all from the 1.0.0 model family.

#include <cstdint>
#include <initializer_list>
#include <string_view>

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

// GPU models: same contract and prompt as dale-0.8b, larger base (Qwen3.5-4B fine-tuned; 9B with a
// merged LoRA). Q4_K_M with imatrix. Hugging Face URLs pinned to the upload commit.
inline constexpr ModelInfo kDale4B_1_0_0_Q4_K_M{
    "dale-4b-1.0.0-Q4_K_M.gguf",
    "https://huggingface.co/manuelgscom/dale-4b-GGUF/resolve/"
    "b25f568e90a583916182767292885c37857e7bca/dale-4b-1.0.0-Q4_K_M.gguf",
    2708805056ULL,
    "a1a5d5185d34dea237616d7a498aa5d9c854316d0af3c87094b73678944e862b",
    "1.0.0",
    "Q4_K_M",
};

inline constexpr ModelInfo kDale9B_1_0_0_Q4_K_M{
    "dale-9b-1.0.0-Q4_K_M.gguf",
    "https://huggingface.co/manuelgscom/dale-9b-GGUF/resolve/"
    "49fb4d649913c5e415a5bb619c20981b89eb8f31/dale-9b-1.0.0-Q4_K_M.gguf",
    5629109760ULL,
    "475e4f8ecf69dcc2a1a14f21a9c6756508748e452a3358945c1f7b1616bd9ba5",
    "1.0.0",
    "Q4_K_M",
};

// CPU model, and the fallback whenever the GPU is not used.
inline constexpr const ModelInfo& kDefaultModel = kDale08B_1_0_0_Q4_K_M;

// Model by file name (the one saved by the first-run setup); nullptr if unknown.
inline const ModelInfo* find_model(std::string_view filename) {
    for (const ModelInfo* m : {&kDale08B_1_0_0_Q4_K_M, &kDale4B_1_0_0_Q4_K_M, &kDale9B_1_0_0_Q4_K_M})
        if (filename == m->filename) return m;
    return nullptr;
}

}  // namespace dale::fetch
