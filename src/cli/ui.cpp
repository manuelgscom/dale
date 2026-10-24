#include "cli/ui.hpp"

#include <cstddef>

namespace dale::cli {

namespace {

constexpr std::string_view kGreen = "\x1b[32m";
constexpr std::string_view kYellow = "\x1b[33m";
constexpr std::string_view kRedBold = "\x1b[1;31m";
constexpr std::string_view kBold = "\x1b[1m";
constexpr std::string_view kDim = "\x1b[2m";
constexpr std::string_view kReset = "\x1b[0m";
constexpr std::size_t kMaxReasons = 3;
constexpr int kSeparatorWidth = 40;

// Empty text is not painted, to avoid stray escape codes.
std::string paint(std::string_view text, std::string_view color, bool enabled) {
    if (!enabled || text.empty()) return std::string(text);
    std::string out(color);
    out += text;
    out += kReset;
    return out;
}

// Escapes control characters and paints each line separately, so the indentation stays
// uncolored. The first line gets prefix first and the rest get rest.
std::string indent(std::string_view text, std::string_view first, std::string_view rest,
                   std::string_view style, bool color) {
    const std::string safe = risk::escape_control(text);
    const std::string_view view(safe);
    std::string out(first);
    for (std::size_t start = 0;;) {
        const std::size_t end = view.find('\n', start);
        out += paint(view.substr(start, end == std::string_view::npos ? end : end - start), style, color);
        if (end == std::string_view::npos) break;
        out += '\n';
        out += rest;
        start = end + 1;
    }
    return out;
}

std::string reasons(const risk::Assessment& a, UiLang lang) {
    std::string out;
    std::size_t shown = 0, total = 0;
    for (const auto& f : a.findings) {
        if (f.level != a.level) continue;
        ++total;
        if (shown == kMaxReasons) continue;
        ++shown;
        out += "    · ";
        if (!f.fragment.empty()) {
            out += risk::escape_control(f.fragment);
            out += ": ";
        }
        out += lang == UiLang::Es ? f.why_es : f.why_en;
        out += '\n';
    }
    const std::size_t more = total - shown;
    if (more == 1) {
        out += "    ";
        out += tr(lang, Msg::MoreReason);
        out += '\n';
    } else if (more > 1) {
        out += "    ";
        out += fill(tr(lang, Msg::MoreReasons), std::to_string(more));
        out += '\n';
    }
    return out;
}

}  // namespace

bool use_color(bool stdout_tty, const char* no_color, const char* term) {
    if (!stdout_tty) return false;
    if (no_color != nullptr && *no_color != '\0') return false;
    if (term != nullptr && std::string_view(term) == "dumb") return false;
    return true;
}

std::string render_suggestion(std::string_view command, std::string_view explanation, bool color) {
    std::string out = "\n" + indent(command, "  ❯ ", "    ", kBold, color) + "\n";
    if (!explanation.empty()) out += indent(explanation, "    ", "    ", kDim, color) + "\n";
    out += '\n';
    return out;
}

std::string render_risk(const risk::Assessment& a, UiLang lang, bool color, bool ask) {
    std::string out = "  ";
    switch (a.level) {
        case risk::Risk::Seguro:
            out += paint("● " + std::string(tr(lang, Msg::LevelSafe)), kGreen, color);
            if (ask) {
                out += "  ·  ";
                out += tr(lang, Msg::PromptSafe);
            } else {
                out += '\n';
            }
            break;
        case risk::Risk::Precaucion:
            out += paint("▲ " + std::string(tr(lang, Msg::LevelCaution)), kYellow, color);
            out += '\n';
            out += reasons(a, lang);
            if (ask) out += render_prompt(a.level, lang);
            break;
        case risk::Risk::Peligro:
            out += paint(tr(lang, Msg::DangerBanner), kRedBold, color);
            out += "  ";
            out += tr(lang, Msg::Irreversible);
            out += '\n';
            out += reasons(a, lang);
            if (ask) out += render_prompt(a.level, lang);
            break;
    }
    return out;
}

std::string render_prompt(risk::Risk level, UiLang lang) {
    switch (level) {
        case risk::Risk::Seguro: return "  " + std::string(tr(lang, Msg::PromptSafe));
        case risk::Risk::Precaucion: return "  " + std::string(tr(lang, Msg::PromptCaution));
        case risk::Risk::Peligro: return "  " + std::string(tr(lang, Msg::PromptDanger));
    }
    return {};
}

std::string render_refusal(bool unsure, std::string_view explanation, UiLang lang, bool color) {
    std::string text(tr(lang, unsure ? Msg::Unsure : Msg::Refuse));
    text += explanation;
    return "\n" + indent(text, "  " + paint("✗", kDim, color) + " ", "    ", {}, false) + "\n\n";
}

std::string render_status(bool ok, std::string_view text, bool color) {
    std::string out = "  ";
    out += ok ? paint("✓", kGreen, color) : paint("✗", kDim, color);
    out += ' ';
    out += text;
    out += '\n';
    return out;
}

std::string render_separator(bool color) {
    std::string line;
    for (int i = 0; i < kSeparatorWidth; ++i) line += "─";
    return "  " + paint(line, kDim, color) + "\n";
}

}  // namespace dale::cli
