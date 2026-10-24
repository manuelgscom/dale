#include "exec/exec.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

#include "exec/posix_util.hpp"

extern char** environ;

namespace dale::exec {

namespace {

constexpr const char* kNoBash = "dale: no se encuentra bash / bash not found\n";

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? v : "";
}

bool in_path(const std::string& prog) {
    std::string path = env_or_empty("PATH");
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        const std::string dir = path.substr(start, end - start);
        if (!dir.empty() && access((dir + "/" + prog).c_str(), X_OK) == 0) return true;
        start = end + 1;
    }
    return false;
}

std::string choose_editor() {
    for (const char* var : {"VISUAL", "EDITOR"}) {
        std::string v = env_or_empty(var);
        if (!v.empty()) return v;
    }
    return in_path("vi") ? "vi" : "nano";
}

// Runs argv ignoring SIGINT/SIGQUIT while waiting. -1 if fork or waitpid fails.
int run_waiting(const std::vector<std::string>& argv, char** envp) {
    std::fflush(nullptr);
    detail::IgnoreInterrupts guard;  // before fork: a Ctrl-C right after it does not kill dale
    detail::ChildOptions opts;
    opts.envp = envp;
    opts.exec_error = kNoBash;
    const pid_t pid = detail::spawn(argv, opts);
    if (pid < 0) return -1;
    return detail::wait_child(pid);
}

}  // namespace

int run_bash(const std::string& command) {
    const int code = run_waiting({"bash", "-c", command}, nullptr);
    if (code < 0) {
        detail::print_error("dale: no se pudo lanzar bash / could not launch bash\n");
        return 1;
    }
    return code;
}

std::optional<std::string> edit_in_editor(const std::string& command) {
    std::string dir = env_or_empty("TMPDIR");
    if (dir.empty()) dir = "/tmp";
    std::string tmpl = dir + "/dale-XXXXXX.sh";
    std::vector<char> path(tmpl.begin(), tmpl.end());
    path.push_back('\0');

    const int fd = mkstemps(path.data(), 3);
    if (fd < 0) {
        detail::print_error("dale: no se pudo crear el fichero temporal / could not create temp file\n");
        return std::nullopt;
    }
    const std::string file(path.data());
    fchmod(fd, 0600);
    const bool written = detail::write_all(fd, command + "\n");
    close(fd);
    if (!written) {
        unlink(file.c_str());
        return std::nullopt;
    }

    // Child environment: the current one without DALE_EDITOR, plus DALE_EDITOR=<editor>.
    const std::string editor_var = "DALE_EDITOR=" + choose_editor();
    std::vector<char*> envp;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        if (std::strncmp(*e, "DALE_EDITOR=", 12) != 0) envp.push_back(*e);
    }
    envp.push_back(const_cast<char*>(editor_var.c_str()));
    envp.push_back(nullptr);

    const int code =
        run_waiting({"bash", "-c", "exec $DALE_EDITOR \"$1\"", "dale-edit", file}, envp.data());
    if (code != 0) {
        unlink(file.c_str());
        return std::nullopt;
    }

    // The editor may have replaced the file: reopen it by name.
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        unlink(file.c_str());
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    unlink(file.c_str());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

}  // namespace dale::exec
