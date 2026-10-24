#pragma once
// Fake engine for tests. The CLI only uses it with DALE_TEST_HOOKS=1.

#include <memory>

#include "infer/engine.hpp"

namespace dale::infer {

// Reads DALE_FAKE_TYPE, DALE_FAKE_COMMAND, DALE_FAKE_EXPLANATION and DALE_FAKE_ERROR.
// DALE_FAKE_RAW (takes precedence if set): raw text that goes through the real parse_response.
// nullptr if none of DALE_FAKE_COMMAND, DALE_FAKE_TYPE and DALE_FAKE_RAW is set.
std::unique_ptr<Engine> fake_engine_from_env();

}  // namespace dale::infer
