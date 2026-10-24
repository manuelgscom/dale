#pragma once
// Default thread count. Platform implementation: cpu_linux.cpp.

namespace dale::infer {

// Physical cores within the process affinity mask, capped at the measured limit.
// Always >= 1.
int default_threads();

}  // namespace dale::infer
