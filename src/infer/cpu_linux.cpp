#include "infer/cpu.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <string>
#include <utility>

#include <sched.h>

namespace dale::infer {

namespace {

// Default thread cap (measured): above 8 threads the E-cores kick in and generation gets
// slower and more erratic.
constexpr int kMaxThreads = 8;

bool read_int(const std::string& path, int& out) {
    std::ifstream in(path);
    return static_cast<bool>(in >> out);
}

}  // namespace

// Physical cores (unique package_id, core_id pairs) among the CPUs in the affinity mask.
// If the topology cannot be read, counts the CPUs in the mask.
int default_threads() {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(mask), &mask) != 0) return 1;
    const int in_mask = CPU_COUNT(&mask);

    std::set<std::pair<int, int>> cores;
    bool ok = true;
    for (int cpu = 0; cpu < CPU_SETSIZE && ok; ++cpu) {
        if (!CPU_ISSET(cpu, &mask)) continue;
        const std::string dir = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
        int package = 0, core = 0;
        ok = read_int(dir + "physical_package_id", package) && read_int(dir + "core_id", core);
        cores.emplace(package, core);
    }
    const int n = ok ? static_cast<int>(cores.size()) : in_mask;
    return std::clamp(n, 1, kMaxThreads);
}

}  // namespace dale::infer
