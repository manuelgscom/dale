#pragma once
// Fake engine for tests. The CLI only uses it with DALE_TEST_HOOKS=1.

#include <memory>

#include "infer/engine.hpp"

namespace dale::infer {

// Reads DALE_FAKE_TYPE, DALE_FAKE_COMMAND, DALE_FAKE_EXPLANATION and DALE_FAKE_ERROR.
// DALE_FAKE_RAW (takes precedence if set): raw text that goes through the real parse_response.
// nullptr if none of DALE_FAKE_COMMAND, DALE_FAKE_TYPE and DALE_FAKE_RAW is set.
// gpu: the engine stands for one on the GPU; DALE_FAKE_GPU_ERROR (an error code) makes it fail
// (the CPU one does not), to test the fallback.
std::unique_ptr<Engine> fake_engine_from_env(bool gpu = false);

}  // namespace dale::infer
