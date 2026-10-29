#include "config/state.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace dale::config {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool parse_u64(std::string_view v, std::uint64_t& out) {
    if (v.empty() || v.size() > 12) return false;
    std::uint64_t n = 0;
    for (char c : v) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = n;
    return true;
}

// Creates dir and its missing parents with 0755.
bool make_dirs(const std::string& dir) {
    for (std::size_t pos = 1; pos <= dir.size(); ++pos) {
        if (pos != dir.size() && dir[pos] != '/') continue;
        const std::string part = dir.substr(0, pos);
        if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

std::string setup_dir() {
    const std::string path = setup_path();
    return path.empty() ? path : path.substr(0, path.rfind('/'));
}

constexpr std::string_view kMarkerPrefix = "gpu-run.";

// Start time of a process (field 22 of /proc/<pid>/stat, in clock ticks since boot); 0 if
// unknown. With the pid, it tells a process apart from a later one that reuses its pid (in a
// container, dale is always pid 1).
std::uint64_t start_time(pid_t pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string stat;
    if (!std::getline(in, stat)) return 0;
    const std::size_t paren = stat.rfind(')');  // the command name may contain spaces
    if (paren == std::string::npos) return 0;
    std::istringstream fields(stat.substr(paren + 1));
    std::string field;
    for (int i = 3; i <= 22; ++i)
        if (!(fields >> field)) return 0;
    std::uint64_t ticks = 0;
    return parse_u64(field, ticks) ? ticks : 0;
}

// This process's marker, kept in a fixed buffer for the signal handlers. Empty = none.
char g_marker[4096] = {};

}  // namespace

std::optional<Setup> parse_setup(std::string_view text) {
    Setup s;
    bool has_device = false;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        const std::string_view line = trim(text.substr(0, nl));
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        if (line.empty() || line.front() == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) return std::nullopt;
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        if (key == "device") {
            if (value == "cpu") s.device = Device::Cpu;
            else if (value == "gpu") s.device = Device::Gpu;
            else return std::nullopt;
            has_device = true;
        } else if (key == "model") {
            s.model = value;
        } else if (key == "gpu") {
            s.gpu = value;
        } else if (key == "vram_free_mib") {
            if (!parse_u64(value, s.vram_free_mib)) return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    if (!has_device || s.model.empty() || s.model.find('/') != std::string::npos) return std::nullopt;
    return s;
}

std::string format_setup(const Setup& s) {
    std::string out = "# Written by dale on first run. Delete it or run `dale --setup` to detect again.\n";
    out += "device = " + std::string(s.device == Device::Gpu ? "gpu" : "cpu") + "\n";
    out += "model = " + s.model + "\n";
    if (s.device == Device::Gpu) {
        out += "gpu = " + s.gpu + "\n";
        out += "vram_free_mib = " + std::to_string(s.vram_free_mib) + "\n";
    }
    return out;
}

std::string setup_path() {
    const char* xdg = std::getenv("XDG_STATE_HOME");
    if (xdg != nullptr && *xdg != '\0') return std::string(xdg) + "/dale/setup";
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return {};
    return std::string(home) + "/.local/state/dale/setup";
}

std::optional<Setup> load_setup() {
    const std::string path = setup_path();
    if (path.empty()) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream text;
    text << in.rdbuf();
    return parse_setup(text.str());
}

bool save_setup(const Setup& setup) {
    const std::string path = setup_path();
    if (path.empty()) return false;
    const std::string dir = path.substr(0, path.rfind('/'));
    if (!make_dirs(dir)) return false;
    std::string tmp = dir + "/.setup-XXXXXX";
    const int fd = ::mkstemp(tmp.data());
    if (fd < 0) return false;
    const std::string text = format_setup(setup);
    bool ok = ::fchmod(fd, 0644) == 0;
    for (std::size_t done = 0; ok && done < text.size();) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        ok = n > 0;
        if (ok) done += static_cast<std::size_t>(n);
    }
    ok = ::close(fd) == 0 && ok;
    ok = ok && ::rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) ::unlink(tmp.c_str());
    return ok;
}

bool begin_gpu_run() {
    const std::string dir = setup_dir();
    if (dir.empty() || !make_dirs(dir)) return false;
    const std::string path = dir + "/" + std::string(kMarkerPrefix) + std::to_string(::getpid()) + "-" +
                             std::to_string(start_time(::getpid()));
    if (path.size() >= sizeof(g_marker)) return false;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    ::close(fd);
    std::memcpy(g_marker, path.c_str(), path.size() + 1);
    return true;
}

void end_gpu_run() { end_gpu_run_from_signal(); }

void end_gpu_run_from_signal() {
    if (g_marker[0] == '\0') return;
    ::unlink(g_marker);
    g_marker[0] = '\0';
}

bool gpu_run_died() {
    const std::string dir = setup_dir();
    DIR* d = dir.empty() ? nullptr : ::opendir(dir.c_str());
    if (d == nullptr) return false;
    bool died = false;
    while (const dirent* e = ::readdir(d)) {
        const std::string_view name = e->d_name;
        if (!name.starts_with(kMarkerPrefix)) continue;
        // "<pid>-<start time>"; a bare "<pid>" is checked by the pid alone.
        const std::string_view id = name.substr(kMarkerPrefix.size());
        const std::size_t dash = id.find('-');
        std::uint64_t pid = 0, start = 0;
        if (!parse_u64(id.substr(0, dash), pid) || pid == 0 || pid > 0x7fffffff) continue;
        if (dash != std::string_view::npos && !parse_u64(id.substr(dash + 1), start)) continue;
        const auto p = static_cast<pid_t>(pid);
        const bool alive = (::kill(p, 0) == 0 || errno != ESRCH) &&
                           (dash == std::string_view::npos || start_time(p) == start);
        if (alive) continue;
        ::unlink((dir + "/" + std::string(name)).c_str());
        died = true;
    }
    ::closedir(d);
    return died;
}

}  // namespace dale::config
