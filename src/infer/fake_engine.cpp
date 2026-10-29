#include "infer/fake_engine.hpp"

#include <cstdlib>
#include <string>
#include <utility>

#include "infer/parser.hpp"

namespace dale::infer {

namespace {

std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v != nullptr ? v : fallback;
}

// Always returns the response set by the DALE_FAKE_* variables.
class FakeEngine : public Engine {
public:
    explicit FakeEngine(bool gpu) : gpu_(gpu) {}

    EngineResult suggest(const Request&) override {
        EngineResult r;
        if (const char* e = std::getenv("DALE_FAKE_GPU_ERROR"); gpu_ && e != nullptr) {
            r.error = e;
            r.error_detail = "fallo simulado de la GPU";
            return r;
        }
        // Raw text through the real parser: end-to-end JSON errors.
        if (const char* raw = std::getenv("DALE_FAKE_RAW")) {
            r.raw = raw;
            ParseResult p = parse_response(r.raw);
            r.suggestion = std::move(p.suggestion);
            r.error = std::move(p.error);
            r.error_detail = std::move(p.detail);
            return r;
        }
        if (std::getenv("DALE_FAKE_ERROR") != nullptr) {
            r.error = env_or("DALE_FAKE_ERROR", "");
            return r;
        }
        const std::string type = env_or("DALE_FAKE_TYPE", "command");
        Suggestion s;
        if (type == "command" || type.empty()) {
            s.kind = Suggestion::Kind::Command;
            s.command = env_or("DALE_FAKE_COMMAND", "");
        } else if (type == "refuse") {
            s.kind = Suggestion::Kind::Refuse;
        } else if (type == "unsure") {
            s.kind = Suggestion::Kind::Unsure;
        } else {
            r.error = "DALE_FAKE_TYPE no válido: " + type;
            return r;
        }
        s.explanation = env_or("DALE_FAKE_EXPLANATION", "");
        r.raw = "(fake)";
        r.suggestion = s;
        return r;
    }

private:
    bool gpu_;
};

}  // namespace

std::unique_ptr<Engine> fake_engine_from_env(bool gpu) {
    if (std::getenv("DALE_FAKE_COMMAND") == nullptr && std::getenv("DALE_FAKE_TYPE") == nullptr &&
        std::getenv("DALE_FAKE_RAW") == nullptr)
        return nullptr;
    return std::make_unique<FakeEngine>(gpu);
}

}  // namespace dale::infer
