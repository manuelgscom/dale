#include "cli/args.hpp"

#include <string_view>

namespace dale::cli {

Args parse_args(const std::vector<std::string>& args) {
    Args out;
    bool help = false, version = false, flags_done = false;
    std::string missing;  // first flag without a value
    std::vector<std::string> words;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (!flags_done && a.size() > 1 && a[0] == '-') {
            if (a == "--") {
                flags_done = true;
            } else if (a == "-h" || a == "--help") {
                help = true;
            } else if (a == "--version") {
                version = true;
            } else if (a == "--json") {
                out.json = true;
            } else if (a == "--print-prompt") {
                out.print_prompt = true;
            } else if (a == "--model" || std::string_view(a).starts_with("--model=")) {
                std::string value;
                if (a == "--model") {
                    if (i + 1 < args.size()) value = args[++i];
                } else {
                    value = a.substr(8);
                }
                if (value.empty()) {
                    if (missing.empty()) missing = "--model";
                } else {
                    out.model = value;
                }
            } else if (out.bad_flag.empty()) {
                out.bad_flag = a;
            }
            continue;
        }
        if (!a.empty()) words.push_back(a);
    }
    for (const auto& w : words) {
        if (!out.request.empty()) out.request += ' ';
        out.request += w;
    }
    if (!out.bad_flag.empty()) {
        out.action = Args::Action::BadFlag;
    } else if (!missing.empty()) {
        out.bad_flag = missing;
        out.action = Args::Action::MissingValue;
    } else if (help) {
        out.action = Args::Action::Help;
    } else if (version) {
        out.action = Args::Action::Version;
    } else if (out.request.empty()) {
        out.action = Args::Action::NoRequest;
    } else {
        out.action = Args::Action::Run;
    }
    return out;
}

}  // namespace dale::cli
