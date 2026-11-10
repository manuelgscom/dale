#include "infer/cpu.hpp"

#include <sys/sysctl.h>

#include <algorithm>

namespace dale::infer {

namespace {

// Default thread cap (same measured limit as on Linux).
constexpr int kMaxThreads = 8;

bool sysctl_int(const char* name, int& out) {
    int value = 0;
    std::size_t size = sizeof(value);
    if (::sysctlbyname(name, &value, &size, nullptr, 0) != 0 || value <= 0) return false;
    out = value;
    return true;
}

}  // namespace

// Apple Silicon: the performance cores only (hw.perflevel0), because the efficiency cores
// slow generation down and make it erratic. Intel Macs: physical cores (no SMT threads).
int default_threads() {
    int n = 0;
    if (!sysctl_int("hw.perflevel0.physicalcpu", n) && !sysctl_int("hw.physicalcpu", n) &&
        !sysctl_int("hw.ncpu", n))
        n = 1;
    return std::clamp(n, 1, kMaxThreads);
}

}  // namespace dale::infer
