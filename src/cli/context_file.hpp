#pragma once
// DALE_CONTEXT_FILE: fixed context line, for reproducible
// evaluation.

#include <optional>
#include <string>
#include <string_view>

namespace dale::cli {

// "os=\S+ distro=\S+ pm=\S+ tools=\S+", exactly: keys in that order, one space between them and
// non-empty values with no ASCII whitespace or control characters.
bool is_context_line(std::string_view line);

struct ContextFile {
    enum class Status { Ok, Unreadable, Invalid };
    Status status = Status::Unreadable;
    std::string line;  // first line, without "\n"
};

// Reads the first line of path and validates it with is_context_line.
ContextFile read_context_file(const std::string& path);

}  // namespace dale::cli
