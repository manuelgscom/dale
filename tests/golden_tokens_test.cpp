#include "doctest.h"

#include "llama.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

// llama.cpp tokens against the transformers tokenizer ones (tests/golden/*.tokens).
// Needs DALE_TEST_MODEL (a GGUF); skipped when the variable is unset.
namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "no se puede leer " << path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void quiet_log(ggml_log_level, const char*, void*) {}

std::vector<llama_token> tokenize(const llama_vocab* vocab, const std::string& text) {
    const auto len = static_cast<int32_t>(text.size());
    int32_t n = -llama_tokenize(vocab, text.data(), len, nullptr, 0, false, true);
    std::vector<llama_token> out(static_cast<std::size_t>(n));
    n = llama_tokenize(vocab, text.data(), len, out.data(), n, false, true);
    REQUIRE(n == static_cast<int32_t>(out.size()));
    return out;
}

}  // namespace

TEST_SUITE("model") {

TEST_CASE("dorado: tokens de llama.cpp = tokens de transformers") {
    const char* model_path = std::getenv("DALE_TEST_MODEL");
    if (model_path == nullptr || *model_path == '\0') {
        MESSAGE("DALE_TEST_MODEL sin definir: se salta");
        return;
    }
    if (std::getenv("DALE_DEBUG") == nullptr) llama_log_set(quiet_log, nullptr);
    llama_backend_init();
    llama_model_params params = llama_model_default_params();
    params.vocab_only = true;
    llama_model* model = llama_model_load_from_file(model_path, params);
    REQUIRE_MESSAGE(model != nullptr, "no se pudo cargar " << model_path);
    const llama_vocab* vocab = llama_model_get_vocab(model);
    MESSAGE("add_bos del vocabulario: " << llama_vocab_get_add_bos(vocab));

    const auto cases = nlohmann::json::parse(read_file(DALE_GOLDEN_DIR "/cases.json"));
    for (const auto& c : cases) {
        const std::string id = c.at("id").get<std::string>();
        CAPTURE(id);
        const std::string text = read_file(DALE_GOLDEN_DIR "/prompt_" + id + ".txt");
        std::vector<llama_token> expected;
        std::istringstream in(read_file(DALE_GOLDEN_DIR "/prompt_" + id + ".tokens"));
        for (llama_token t; in >> t;) expected.push_back(t);
        REQUIRE(!expected.empty());
        CHECK(tokenize(vocab, text) == expected);
    }
    llama_model_free(model);
    llama_backend_free();
}

}  // TEST_SUITE
