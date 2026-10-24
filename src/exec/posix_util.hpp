#pragma once
// Internal POSIX helpers for the exec module: spawn a child, wait for it and ignore Ctrl-C.

#include <signal.h>
#include <sys/types.h>

#include <string>
#include <string_view>
#include <vector>

namespace dale::exec::detail {

struct ChildOptions {
    int stdin_fd = -1;                 // if >= 0, becomes the child's standard input
    bool quiet = false;                // child's stdout and stderr to /dev/null
    char** envp = nullptr;             // child's environment; nullptr = the parent's
    const char* exec_error = nullptr;  // message to stderr if exec fails (the child exits with 127)
};

// fork + execvp(argv[0], argv). In the child, resets SIGINT, SIGQUIT and SIGPIPE to SIG_DFL.
// Returns the child's pid, or -1 if fork fails.
pid_t spawn(const std::vector<std::string>& argv, const ChildOptions& opts);

// waitpid retrying on EINTR. Returns the exit code, 128+N if killed by
// signal N, or -1 if waitpid fails.
int wait_child(pid_t pid);

// Writes the whole buffer (retries on EINTR and partial writes). false on failure.
bool write_all(int fd, std::string_view data);

// Message to stderr with a direct write.
void print_error(std::string_view msg);

// While alive, the process ignores SIGINT and SIGQUIT (like system()). Restores on destruction.
class IgnoreInterrupts {
public:
    IgnoreInterrupts();
    ~IgnoreInterrupts();
    IgnoreInterrupts(const IgnoreInterrupts&) = delete;
    IgnoreInterrupts& operator=(const IgnoreInterrupts&) = delete;

private:
    struct sigaction old_int_{};
    struct sigaction old_quit_{};
};

// While alive, the process ignores SIGPIPE. Restores on destruction.
class IgnoreSigpipe {
public:
    IgnoreSigpipe();
    ~IgnoreSigpipe();
    IgnoreSigpipe(const IgnoreSigpipe&) = delete;
    IgnoreSigpipe& operator=(const IgnoreSigpipe&) = delete;

private:
    struct sigaction old_{};
};

}  // namespace dale::exec::detail
