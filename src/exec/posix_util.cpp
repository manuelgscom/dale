#include "exec/posix_util.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace dale::exec::detail {

namespace {

void set_default(int sig) {
    struct sigaction sa {};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
}

void set_ignore(int sig, struct sigaction* old) {
    struct sigaction sa {};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, old);
}

}  // namespace

pid_t spawn(const std::vector<std::string>& argv, const ChildOptions& opts) {
    // Everything that allocates memory is prepared before fork.
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    const pid_t pid = fork();
    if (pid != 0) return pid;  // parent (or -1 on failure)

    // Child: only fork-safe functions. SIG_IGN would be inherited across exec.
    set_default(SIGINT);
    set_default(SIGQUIT);
    set_default(SIGPIPE);
    if (opts.stdin_fd >= 0 && opts.stdin_fd != 0) {
        dup2(opts.stdin_fd, 0);
        close(opts.stdin_fd);
    }
    if (opts.quiet) {
        const int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, 1);
            dup2(null_fd, 2);
            if (null_fd > 2) close(null_fd);
        }
    }
    if (opts.envp != nullptr) environ = opts.envp;
    execvp(args[0], args.data());
    if (opts.exec_error != nullptr) print_error(opts.exec_error);
    _exit(127);
}

int wait_child(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

void print_error(std::string_view msg) {
    write_all(2, msg);
}

IgnoreInterrupts::IgnoreInterrupts() {
    set_ignore(SIGINT, &old_int_);
    set_ignore(SIGQUIT, &old_quit_);
}

IgnoreInterrupts::~IgnoreInterrupts() {
    sigaction(SIGINT, &old_int_, nullptr);
    sigaction(SIGQUIT, &old_quit_, nullptr);
}

IgnoreSigpipe::IgnoreSigpipe() { set_ignore(SIGPIPE, &old_); }

IgnoreSigpipe::~IgnoreSigpipe() { sigaction(SIGPIPE, &old_, nullptr); }

}  // namespace dale::exec::detail
