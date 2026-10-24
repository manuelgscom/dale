#include "cli/flow.hpp"

#include "cli/ui.hpp"

namespace dale::cli {

namespace {

constexpr int kMaxInvalid = 3;

std::string_view trim(std::string_view s) {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

// ASCII lowercase; also maps "Í" (UTF-8) to "í" so "SÍ" is accepted.
std::string lower(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c - 'A' + 'a');
        } else if (c == '\xC3' && i + 1 < s.size() && s[i + 1] == '\x8D') {
            out += "\xC3\xAD";
            ++i;
        } else {
            out += c;
        }
    }
    return out;
}

int cancel(const Io& io, const FlowOptions& opt) {
    io.write(render_status(false, tr(opt.lang, Msg::Cancelled), opt.color));
    return 0;
}

// The separator marks where dale ends and the command output begins.
int run(const std::string& command, const FlowOptions& opt, const Io& io) {
    io.write(render_separator(opt.color));
    return io.run(command);
}

}  // namespace

Answer parse_answer(std::string_view line) {
    const std::string a = lower(trim(line));
    if (a.empty()) return Answer::Empty;
    if (a == "s" || a == "si" || a == "sí" || a == "y" || a == "yes") return Answer::Yes;
    if (a == "n" || a == "no") return Answer::No;
    if (a == "e") return Answer::Edit;
    if (a == "c") return Answer::Copy;
    return Answer::Invalid;
}

Answer parse_danger_answer(std::string_view line, std::string_view word) {
    const std::string_view t = trim(line);
    if (t == word) return Answer::Yes;
    const std::string a = lower(t);
    if (a == "e") return Answer::Edit;
    if (a == "c") return Answer::Copy;
    return Answer::No;
}

int run_flow(std::string command, std::string explanation, const FlowOptions& opt, const Io& io) {
    const UiLang lang = opt.lang;
    const config::Mode mode = opt.interactive ? opt.mode : config::Mode::Suggest;

    // Each iteration handles one command (the proposed one or an edited one).
    for (;;) {
        const risk::Assessment a = io.classify(command);
        io.write(render_suggestion(command, explanation, opt.color));

        if (mode == config::Mode::Suggest) {
            io.write(render_risk(a, lang, opt.color, false));
            return 0;
        }
        if (mode == config::Mode::Auto && a.level == risk::Risk::Seguro) {
            io.write(render_risk(a, lang, opt.color, false));
            return run(command, opt, io);
        }
        io.write(render_risk(a, lang, opt.color, true));

        Answer answer = Answer::Invalid;
        if (a.level == risk::Risk::Peligro) {
            const auto line = io.read_line();
            if (!line) {
                io.write("\n");
                return cancel(io, opt);
            }
            answer = parse_danger_answer(*line, tr(lang, Msg::DangerWord));
        } else {
            for (int invalid = 0;;) {
                const auto line = io.read_line();
                if (!line) {
                    io.write("\n");
                    return cancel(io, opt);
                }
                answer = parse_answer(*line);
                if (answer == Answer::Empty)
                    answer = a.level == risk::Risk::Seguro ? Answer::Yes : Answer::No;
                if (answer != Answer::Invalid) break;
                if (++invalid >= kMaxInvalid) return cancel(io, opt);
                io.write(render_prompt(a.level, lang));
            }
        }

        switch (answer) {
            case Answer::Yes:
                return run(command, opt, io);
            case Answer::Copy:
                if (!io.copy(command)) {
                    io.write(render_status(false, tr(lang, Msg::CopyFailed), opt.color));
                    return 1;
                }
                io.write(render_status(true, tr(lang, Msg::Copied), opt.color));
                return 0;
            case Answer::Edit: {
                const auto edited = io.edit(command);
                if (!edited || trim(*edited).empty()) return cancel(io, opt);
                command = std::string(trim(*edited));
                explanation = std::string(tr(lang, Msg::EditedExplanation));
                break;  // classify and show again
            }
            default:
                return cancel(io, opt);
        }
    }
}

}  // namespace dale::cli
