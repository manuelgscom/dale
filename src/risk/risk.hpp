#pragma once
// Deterministic risk checker.

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

// Pure function: looks only at the string (no files, environment or PATH).
Assessment classify(std::string_view command);

// "seguro" | "precaucion" | "peligro" (no accent; used by dale_risk and --json).
std::string_view to_string(Risk level);

// Escapes control characters except \n and \t (ESC => "\x1b") so the command can be shown
// without it injecting terminal sequences.
std::string escape_control(std::string_view text);

}  // namespace dale::risk
