#pragma once
// Inference engine interface: LlamaEngine and FakeEngine (tests).

#include <optional>
#include <string>

namespace dale::infer {

struct Suggestion {
    enum class Kind { Command, Refuse, Unsure };
    Kind kind = Kind::Command;
    std::string command;      // empty if kind != Command
    std::string explanation;  // in the language of the request
};

struct Timings {
    double load_ms = 0, prefill_ms = 0, generate_ms = 0, total_ms = 0;
    int prompt_tokens = 0, gen_tokens = 0;
};

// Empty suggestion => error. raw: raw model output.
// error is a code: invalid_json | invalid_schema | truncated | timeout | too_long | load_failed |
// decode_failed.
// error_detail: human-readable reason (for --json and DALE_DEBUG), untranslated.
struct EngineResult {
    std::optional<Suggestion> suggestion;
    std::string error;
    std::string error_detail;
    std::string raw;
    Timings timings;
};

struct Request {
    std::string context_line;  // context::to_line()
    std::string text;          // user request
};

class Engine {
public:
    virtual ~Engine() = default;
    virtual EngineResult suggest(const Request& request) = 0;
};

}  // namespace dale::infer
