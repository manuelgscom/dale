#include "exec/exec.hpp"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

#include "exec/base64.hpp"
#include "exec/posix_util.hpp"

namespace dale::exec {

namespace {

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0';
}

// Runs the tool and feeds it the text on stdin. true if it exits with 0.
bool pipe_to(const std::vector<std::string>& argv, const std::string& text) {
    int fds[2];
    if (pipe(fds) != 0) return false;
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);  // the child must not inherit the write end

    detail::ChildOptions opts;
    opts.stdin_fd = fds[0];
    opts.quiet = true;
    const pid_t pid = detail::spawn(argv, opts);
    close(fds[0]);
    if (pid < 0) {
        close(fds[1]);
        return false;
    }

    bool written = false;
    {
        detail::IgnoreSigpipe guard;  // if the tool dies early, it must not kill dale
        written = detail::write_all(fds[1], text);
        close(fds[1]);
    }
    return detail::wait_child(pid) == 0 && written;
}

}  // namespace

bool copy_to_clipboard(const std::string& text) {
    if (env_set("WAYLAND_DISPLAY") && pipe_to({"wl-copy"}, text)) return true;
    if (env_set("DISPLAY")) {
        if (pipe_to({"xclip", "-selection", "clipboard"}, text)) return true;
        if (pipe_to({"xsel", "--clipboard", "--input"}, text)) return true;
    }
    // OSC 52: the terminal copies to the clipboard.
    std::fflush(stdout);
    return detail::write_all(1, "\x1b]52;c;" + base64_encode(text) + "\x07");
}

}  // namespace dale::exec
