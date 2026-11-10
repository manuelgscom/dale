#pragma once
// Default thread count. Platform implementations: cpu_linux.cpp, cpu_macos.cpp.

namespace dale::infer {

// Physical cores within the process affinity mask, capped at the measured limit.
// Always >= 1.
int default_threads();

}  // namespace dale::infer
