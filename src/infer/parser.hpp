#pragma once
// Strict parser for the model's JSON response.

#include <optional>
#include <string>
#include <string_view>

#include "infer/engine.hpp"

namespace dale::infer {

// Empty suggestion => error: invalid_json | invalid_schema. detail: human-readable reason.
struct ParseResult {
    std::optional<Suggestion> suggestion;
    std::string error;
    std::string detail;
};

ParseResult parse_response(std::string_view raw);

}  // namespace dale::infer
