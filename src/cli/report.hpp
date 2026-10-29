#pragma once
// --json output: a single line following the model output schema.

#include <string>

#include "cli/i18n.hpp"
#include "infer/engine.hpp"

namespace dale::cli {

struct ReportInput {
    std::string model;    // model file name (no directory)
    int threads = 0;
    std::string request;  // already normalized
    std::string context;  // context line
    std::string gpu;      // GPU description; empty = CPU
};

// JSON object on one line, no trailing "\n". Keys in schema order. response and risk
// are null on error; risk also when type is not command. Times in whole, rounded
// ms. Risk reasons are in language lang. Invalid UTF-8 => U+FFFD.
std::string json_report(const ReportInput& in, const infer::EngineResult& result, UiLang lang);

}  // namespace dale::cli
