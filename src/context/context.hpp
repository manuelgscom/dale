#pragma once
// System context collector (Linux and macOS; the OS-specific logic lives in context_posix.cpp).

#include <string>
#include <string_view>
#include <vector>

namespace dale::context {

struct SystemContext {
    std::string os;      // "linux" | "macos"
    std::string distro;  // "ubuntu-24.04", "arch", "macos-15.1", "unknown"
    std::string pm;      // "apt", "dnf", "brew", "port", …, "none"
    std::vector<std::string> tools;  // in alphabetical order
};

// Injectable sources for tests.
struct ContextSources {
    std::vector<std::string> os_release_paths = {"/etc/os-release", "/usr/lib/os-release"};
    std::string path_env;  // value of PATH
    // macOS: when true the context is built from macos_version (ProductVersion, e.g. "15.1")
    // instead of os-release. detect_context() sets it on Apple builds.
    bool macos = false;
    std::string macos_version;
};

// Real system: standard os-release and $PATH.
SystemContext detect_context();
SystemContext detect_context(const ContextSources& sources);

// True if an executable with this name is in $PATH (absolute entries only).
bool tool_installed(std::string_view name);

// "os=linux distro=ubuntu-24.04 pm=apt tools=fd,jq,rg"
std::string to_line(const SystemContext& ctx);

}  // namespace dale::context
