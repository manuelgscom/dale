#include "cli/model.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>

namespace dale::cli {

namespace {

#if DALE_TEST_HOOKS
const char* env(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0' ? v : nullptr;
}
#endif

}  // namespace

bool test_model_hooks() {
#if DALE_TEST_HOOKS
    return env("DALE_TEST_MODEL_URL") != nullptr;
#else
    return false;
#endif
}

ModelSpec default_model_spec() { return model_spec(fetch::kDefaultModel); }

ModelSpec model_spec(const fetch::ModelInfo& m) {
    ModelSpec spec{m.filename, m.url, m.size, m.sha256};
#if DALE_TEST_HOOKS
    if (test_model_hooks()) {
        spec.url = env("DALE_TEST_MODEL_URL");
        const char* size = env("DALE_TEST_MODEL_SIZE");
        spec.size = size != nullptr ? std::strtoull(size, nullptr, 10) : 0;
        const char* sha = env("DALE_TEST_MODEL_SHA256");
        spec.sha256 = sha != nullptr ? sha : "";
    }
#endif
    return spec;
}

ModelResolution resolve_model(const std::string& explicit_path, const std::string& models_dir,
                              const ModelSpec& spec) {
    ModelResolution r;
    struct stat st{};
    if (!explicit_path.empty()) {
        r.path = explicit_path;
        r.status = ::stat(explicit_path.c_str(), &st) == 0 ? ModelResolution::Status::Ready
                                                             : ModelResolution::Status::Missing;
        return r;
    }
    if (models_dir.empty()) return r;  // NoDir
    r.path = models_dir + "/" + spec.filename;
    const bool ok = ::stat(r.path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                    static_cast<std::uint64_t>(st.st_size) == spec.size;
    r.status = ok ? ModelResolution::Status::Ready : ModelResolution::Status::NeedsDownload;
    return r;
}

std::uint64_t size_mb(std::uint64_t bytes) {
    constexpr std::uint64_t kMB = 1000 * 1000;
    if (bytes == 0) return 0;
    return std::max<std::uint64_t>(1, (bytes + kMB / 2) / kMB);
}

}  // namespace dale::cli
