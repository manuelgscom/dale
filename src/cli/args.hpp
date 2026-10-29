#pragma once
// Command-line arguments.

#include <optional>
#include <string>
#include <vector>

namespace dale::cli {

struct Args {
    // Setup: --setup without a request (detect, choose and exit).
    enum class Action { Run, Help, Version, Setup, NoRequest, BadFlag, MissingValue };
    Action action = Action::NoRequest;
    std::string request;   // non-flag arguments, joined with spaces
    std::string bad_flag;  // unknown flag (BadFlag) or flag without value (MissingValue)
    std::optional<std::string> model;  // --model PATH / --model=PATH
    bool json = false;          // --json
    bool print_prompt = false;  // --print-prompt
    bool setup = false;         // --setup: detect the GPU and choose the model again
};

// args without argv[0]. "--" ends the flags. "--model" always takes the next argument.
Args parse_args(const std::vector<std::string>& args);

}  // namespace dale::cli
