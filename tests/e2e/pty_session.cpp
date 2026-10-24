#include "pty_session.hpp"

#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace dale::e2e {

namespace {

using Clock = std::chrono::steady_clock;

// Copies the strings into a nullptr-terminated array, as execve expects.
std::vector<char*> c_array(const std::vector<std::string>& v) {
    std::vector<char*> out;
    for (const auto& s : v) out.push_back(const_cast<char*>(s.c_str()));
    out.push_back(nullptr);
    return out;
}

int ms_left(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

ExitStatus decode(int status) {
    ExitStatus s;
    if (WIFEXITED(status)) {
        s.exited = true;
        s.code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        s.signaled = true;
        s.signal = WTERMSIG(status);
    }
    return s;
}

// Waits for the child until the deadline; kills it if it has not finished.
ExitStatus reap(pid_t pid, Clock::time_point deadline) {
    int status = 0;
    for (;;) {
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) return decode(status);
        if (r < 0 && errno != EINTR) return ExitStatus{};
        if (Clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::kill(pid, SIGKILL);
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    ExitStatus s = decode(status);
    s.timed_out = true;
    return s;
}

void write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
}

}  // namespace

PtySession::PtySession(const std::vector<std::string>& argv, const std::vector<std::string>& env) {
    std::vector<char*> args = c_array(argv);
    std::vector<char*> envp = c_array(env);
    winsize ws{};
    ws.ws_row = 40;
    ws.ws_col = 120;
    pid_ = ::forkpty(&master_, nullptr, nullptr, &ws);
    if (pid_ < 0) throw std::runtime_error("forkpty ha fallado");
    if (pid_ == 0) {
        ::execve(args[0], args.data(), envp.data());
        ::_exit(127);
    }
}

PtySession::~PtySession() {
    if (pid_ > 0 && !reaped_) {
        ::kill(pid_, SIGKILL);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
    }
    if (master_ >= 0) ::close(master_);
}

void PtySession::write(std::string_view input) { write_all(master_, input); }

bool PtySession::read_some(int timeout_ms) {
    if (eof_) return false;
    pollfd p{master_, POLLIN, 0};
    const int r = ::poll(&p, 1, timeout_ms);
    if (r < 0 && errno == EINTR) return true;
    if (r <= 0) return true;  // no data yet
    char buf[4096];
    const ssize_t n = ::read(master_, buf, sizeof buf);
    if (n > 0) {
        output_.append(buf, static_cast<std::size_t>(n));
        return true;
    }
    if (n < 0 && errno == EINTR) return true;
    eof_ = true;  // 0 or EIO: the other end was closed
    return false;
}

bool PtySession::wait_for(std::string_view text, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (output_.find(text) != std::string::npos) return true;
        if (eof_ || Clock::now() >= deadline) return false;
        read_some(ms_left(deadline));
    }
}

ExitStatus PtySession::wait_exit(int timeout_ms) {
    if (reaped_) return status_;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    int status = 0;
    while (!eof_ && Clock::now() < deadline) {
        read_some(std::min(50, ms_left(deadline)));
        const pid_t r = ::waitpid(pid_, &status, WNOHANG);
        if (r == pid_) {
            // Already finished: drain what is left in the pty.
            while (read_some(100) && Clock::now() < deadline) {
            }
            reaped_ = true;
            status_ = decode(status);
            return status_;
        }
    }
    status_ = reap(pid_, deadline);
    reaped_ = true;
    return status_;
}

PipeResult run_with_pipes(const std::vector<std::string>& argv, const std::vector<std::string>& env,
                          std::string_view input, int timeout_ms) {
    ::signal(SIGPIPE, SIG_IGN);
    int in[2], out[2], err[2];
    if (::pipe(in) != 0 || ::pipe(out) != 0 || ::pipe(err) != 0) throw std::runtime_error("pipe ha fallado");
    std::vector<char*> args = c_array(argv);
    std::vector<char*> envp = c_array(env);
    const pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("fork ha fallado");
    if (pid == 0) {
        ::dup2(in[0], 0);
        ::dup2(out[1], 1);
        ::dup2(err[1], 2);
        for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]}) ::close(fd);
        ::execve(args[0], args.data(), envp.data());
        ::_exit(127);
    }
    ::close(in[0]);
    ::close(out[1]);
    ::close(err[1]);
    write_all(in[1], input);
    ::close(in[1]);

    PipeResult res;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
    std::string* sinks[2] = {&res.out, &res.err};
    int open = 2;
    while (open > 0 && Clock::now() < deadline) {
        if (::poll(fds, 2, ms_left(deadline)) <= 0) continue;
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || fds[i].revents == 0) continue;
            char buf[4096];
            const ssize_t n = ::read(fds[i].fd, buf, sizeof buf);
            if (n > 0) {
                sinks[i]->append(buf, static_cast<std::size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                ::close(fds[i].fd);
                fds[i].fd = -1;
                --open;
            }
        }
    }
    for (auto& p : fds)
        if (p.fd >= 0) ::close(p.fd);
    res.status = reap(pid, deadline);
    return res;
}

}  // namespace dale::e2e
