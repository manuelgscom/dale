#include "doctest.h"

#include "llama.h"

// Check that llama.cpp builds and links into dale_tests.
TEST_SUITE("infer") {
TEST_CASE("llama.cpp enlaza") {
    llama_backend_init();
    CHECK(llama_max_devices() >= 1);
    llama_backend_free();
}
}  // TEST_SUITE
