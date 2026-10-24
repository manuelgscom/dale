#include "cli/context_file.hpp"

#include <array>
#include <fstream>

namespace dale::cli {

bool is_context_line(std::string_view line) {
    constexpr std::array<std::string_view, 4> kKeys = {"os=", "distro=", "pm=", "tools="};
    for (std::size_t k = 0; k < kKeys.size(); ++k) {
        if (!line.starts_with(kKeys[k])) return false;
        line.remove_prefix(kKeys[k].size());
        std::size_t n = 0;
        while (n < line.size() && line[n] != ' ') {
            const auto c = static_cast<unsigned char>(line[n]);
            if (c < 0x20 || c == 0x7f) return false;  // includes \t, \r, \v, \f
            ++n;
        }
        if (n == 0) return false;
        line.remove_prefix(n);
        if (k + 1 < kKeys.size()) {
            if (line.empty()) return false;
            line.remove_prefix(1);  // the separating space
        }
    }
    return line.empty();
}

ContextFile read_context_file(const std::string& path) {
    ContextFile out;
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    std::getline(f, out.line);
    if (f.bad()) return out;
    out.status = is_context_line(out.line) ? ContextFile::Status::Ok : ContextFile::Status::Invalid;
    return out;
}

}  // namespace dale::cli
