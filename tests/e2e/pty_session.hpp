#pragma once
// Small harness to run dale in a pseudo-terminal or with pipes.

#include <sys/types.h>

#include <string>
#include <string_view>
#include <vector>

namespace dale::e2e {

struct ExitStatus {
    bool exited = false;    // ended through exit()
    int code = -1;          // exit() code
    bool signaled = false;  // killed by a signal
    int signal = 0;
    bool timed_out = false;  // did not finish in time (killed with SIGKILL)
};

class PtySession {
public:
    // argv[0] is the program (full path). env: "KEY=value"; nothing is inherited.
    PtySession(const std::vector<std::string>& argv, const std::vector<std::string>& env);
    ~PtySession();
    PtySession(const PtySession&) = delete;
    PtySession& operator=(const PtySession&) = delete;

    void write(std::string_view input);

    // Reads until the accumulated output contains text. false on timeout or EOF.
    bool wait_for(std::string_view text, int timeout_ms = 5000);

    // Reads to the end and collects the exit status. On timeout, kills the process.
    ExitStatus wait_exit(int timeout_ms = 5000);

    const std::string& output() const { return output_; }

private:
    bool read_some(int timeout_ms);  // false on EOF
    pid_t pid_ = -1;
    int master_ = -1;
    bool eof_ = false;
    bool reaped_ = false;
    ExitStatus status_;
    std::string output_;
};

struct PipeResult {
    std::string out;
    std::string err;
    ExitStatus status;
};

// Runs the program without a terminal: stdin, stdout and stderr are pipes.
PipeResult run_with_pipes(const std::vector<std::string>& argv, const std::vector<std::string>& env,
                          std::string_view input, int timeout_ms = 5000);

}  // namespace dale::e2e
