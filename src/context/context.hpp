#pragma once
// System context collector. The Linux-specific part lives in context_linux.cpp.

#include <string>
#include <vector>

namespace dale::context {

struct SystemContext {
    std::string os;      // "linux"
    std::string distro;  // "ubuntu-24.04", "arch", "unknown"
    std::string pm;      // "apt", "dnf", …, "none"
    std::vector<std::string> tools;  // in alphabetical order
};

// Injectable sources for tests.
struct ContextSources {
    std::vector<std::string> os_release_paths = {"/etc/os-release", "/usr/lib/os-release"};
    std::string path_env;  // value of PATH
};

// Real system: standard os-release and $PATH.
SystemContext detect_context();
SystemContext detect_context(const ContextSources& sources);

// "os=linux distro=ubuntu-24.04 pm=apt tools=fd,jq,rg"
std::string to_line(const SystemContext& ctx);

}  // namespace dale::context
