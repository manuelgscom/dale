#pragma once
// Screen rendering. Pure functions: they return the text to write.

#include <string>
#include <string_view>

#include "cli/i18n.hpp"
#include "risk/risk.hpp"

namespace dale::cli {

// Color only if stdout is a TTY, NO_COLOR is unset or empty, and TERM is not "dumb".
bool use_color(bool stdout_tty, const char* no_color, const char* term);

// "\n  ❯ <command>\n    <explanation>\n\n", escaping control characters. With color, the
// command is bold and the explanation dimmed; continuation lines are indented by 4 spaces.
std::string render_suggestion(std::string_view command, std::string_view explanation, bool color);

// Risk line ("● seguro", "▲ precaución" or the danger banner) with its reasons.
// With ask, it ends with the prompt (no trailing newline).
std::string render_risk(const risk::Assessment& a, UiLang lang, bool color, bool ask);

// The prompt alone, to ask again after an invalid answer.
std::string render_prompt(risk::Risk level, UiLang lang);

// "\n  ✗ Esto no se resuelve con un comando: …\n\n" / "\n  ✗ No estoy seguro: …\n\n".
std::string render_refusal(bool unsure, std::string_view explanation, UiLang lang, bool color);

// "  ✓ <text>\n" (green ✓) if ok; otherwise "  ✗ <text>\n" (dimmed ✗).
std::string render_status(bool ok, std::string_view text, bool color);

// "  ────…\n" (dimmed): separates dale's screen from the command output.
std::string render_separator(bool color);

}  // namespace dale::cli
