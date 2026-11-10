#pragma once
// Deterministic risk checker.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dale::risk {

enum class Risk { Seguro = 0, Precaucion = 1, Peligro = 2 };

// A reason that raises the level. The UI shows it as "· <fragment>: <why>".
struct Finding {
    Risk level = Risk::Precaucion;
    std::string rule_id;   // "R1", "R14c"…, or a policy id (e.g. "PARSE", "DYN")
    std::string fragment;  // short piece of the command it refers to, e.g. "rm -rf ~"
    std::string why_es;
    std::string why_en;
};

// level = maximum of everything. findings: every reason found, in order of appearance;
// the UI shows those with level == level. Safe => empty findings.
struct Assessment {
    Risk level = Risk::Seguro;
    std::vector<Finding> findings;
};

// Context of the user and the system that refines the verdict. Everything is optional.
struct Policy {
    // Commands the user trusts (config key "allow"). An entry is a command name optionally
    // followed by leading arguments ("ffmpeg", "docker compose up -d"). It turns the "caution"
    // findings of the matching command into "safe". It never lowers a "danger" finding.
    std::vector<std::string> allow;
    // Commands the user wants blocked (config key "deny"): they are treated as danger and need
    // the typed confirmation word. Best effort: it is not a security boundary.
    std::vector<std::string> deny;
    // The target system is macOS: GNU/Linux-only commands and options are reported (rule MAC).
    bool macos = false;
    // Optional: whether a program is installed (e.g. GNU coreutils from Homebrew). Used only to
    // silence the macOS portability check.
    std::function<bool(std::string_view)> has_tool;
};

// Pure function: looks only at the string (no files, environment or PATH).
Assessment classify(std::string_view command);
// Same, refined by the user's policy. Still pure: has_tool is the only external input.
Assessment classify(std::string_view command, const Policy& policy);

// "seguro" | "precaucion" | "peligro" (no accent; used by dale_risk and --json).
std::string_view to_string(Risk level);

// Escapes control characters except \n and \t (ESC => "\x1b") so the command can be shown
// without it injecting terminal sequences.
std::string escape_control(std::string_view text);

}  // namespace dale::risk
