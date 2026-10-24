#pragma once
// Command-line arguments.

#include <optional>
#include <string>
#include <vector>

namespace dale::cli {

struct Args {
    enum class Action { Run, Help, Version, NoRequest, BadFlag, MissingValue };
    Action action = Action::NoRequest;
    std::string request;   // non-flag arguments, joined with spaces
    std::string bad_flag;  // unknown flag (BadFlag) or flag without value (MissingValue)
    std::optional<std::string> model;  // --model PATH / --model=PATH
    bool json = false;          // --json
    bool print_prompt = false;  // --print-prompt
};

// args without argv[0]. "--" ends the flags. "--model" always takes the next argument.
Args parse_args(const std::vector<std::string>& args);

}  // namespace dale::cli
