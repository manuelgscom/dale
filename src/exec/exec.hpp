#pragma once
// Executor, editor and clipboard. The POSIX parts live in exec_posix.cpp and clipboard.cpp.

#include <optional>
#include <string>

namespace dale::exec {

// bash -c command. Returns the exit code; 128+N if killed by signal N; 127 without bash.
// While waiting, the parent ignores SIGINT and SIGQUIT.
int run_bash(const std::string& command);

// Opens the command in $VISUAL/$EDITOR/vi/nano. nullopt = cancelled (editor exit code != 0).
// Returns the text without trailing newlines (it may end up empty).
std::optional<std::string> edit_in_editor(const std::string& command);

// wl-copy / xclip / xsel; if missing or failing, OSC 52 to stdout. true if copied or emitted.
bool copy_to_clipboard(const std::string& text);

}  // namespace dale::exec
