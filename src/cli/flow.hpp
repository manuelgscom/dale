#pragma once
// Mode state machine: show, ask, edit, copy or run.

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "cli/i18n.hpp"
#include "config/config.hpp"
#include "risk/risk.hpp"

namespace dale::cli {

// Everything that touches the outside world, injectable for tests.
struct Io {
    std::function<void(std::string_view)> write;
    std::function<std::optional<std::string>()> read_line;  // without "\n"; nullopt = EOF
    std::function<risk::Assessment(std::string_view)> classify;
    std::function<int(const std::string&)> run;
    std::function<std::optional<std::string>(const std::string&)> edit;
    std::function<bool(const std::string&)> copy;
};

struct FlowOptions {
    config::Mode mode = config::Mode::Confirm;
    UiLang lang = UiLang::Es;
    bool color = false;
    bool interactive = true;  // false (no TTY) => behaves like suggest
};

enum class Answer { Yes, No, Edit, Copy, Empty, Invalid };

// Answer to the safe/caution prompt (case-insensitive, trimmed).
Answer parse_answer(std::string_view line);

// Answer to the danger prompt: Yes only with the exact word; Edit/Copy with e/c;
// anything else, No.
Answer parse_danger_answer(std::string_view line, std::string_view word);

// Shows the command and acts according to the mode. Returns dale's exit code.
int run_flow(std::string command, std::string explanation, const FlowOptions& opt, const Io& io);

}  // namespace dale::cli
