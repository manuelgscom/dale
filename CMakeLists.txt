cmake_minimum_required(VERSION 3.28)
project(dale VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Enables FakeEngine through environment variables. Release builds set it to OFF.
option(DALE_TEST_HOOKS "Fake engine driven by environment variables (DALE_FAKE_*)" ON)

# Download URL of the default model. Empty = the one in src/fetch/model_info.hpp; a release
# build can override it here.
set(DALE_MODEL_URL "" CACHE STRING "Download URL of the default model (empty = the one in model_info.hpp)")

# Strict warnings only on our own targets. Never via a global add_compile_options:
# llama.cpp does not build with -Werror.
function(dale_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion)
endfunction()

# Static, CPU-only llama.cpp. No common => no cpp-httplib and no networking.
option(DALE_NATIVE "Optimize for this CPU (development)" ON)
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP      OFF CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL        OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE   ${DALE_NATIVE} CACHE BOOL "" FORCE)
option(DALE_OPENMP "OpenMP in ggml (for benchmarking only; OFF by default)" OFF)
set(GGML_OPENMP   ${DALE_OPENMP} CACHE BOOL "" FORCE)
# Vulkan backend (needs the Vulkan headers, glslc and SPIRV-Headers to build). The binary does not
# link libvulkan: dale_vk_shim (src/vulkan/vulkan_shim.cpp) defines the few Vulkan functions
# ggml-vulkan imports and forwards them to libvulkan.so.1 opened with dlopen, so dale still
# starts on machines without a Vulkan loader. The Vulkan::Vulkan target defined here (headers +
# shim) takes the place of the one FindVulkan would create (it only creates it if missing).
option(DALE_VULKAN "Vulkan GPU backend (loaded at run time)" OFF)
set(GGML_VULKAN ${DALE_VULKAN} CACHE BOOL "" FORCE)
if(DALE_VULKAN)
  find_path(DALE_VULKAN_INCLUDE_DIR vulkan/vulkan.h HINTS "$ENV{VULKAN_SDK}/include" REQUIRED)
  add_library(dale_vk_shim STATIC src/vulkan/vulkan_shim.cpp)
  target_include_directories(dale_vk_shim PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_include_directories(dale_vk_shim SYSTEM PRIVATE "${DALE_VULKAN_INCLUDE_DIR}")
  target_link_libraries(dale_vk_shim PRIVATE ${CMAKE_DL_LIBS})
  dale_warnings(dale_vk_shim)
  add_library(Vulkan::Vulkan INTERFACE IMPORTED GLOBAL)
  set_target_properties(Vulkan::Vulkan PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${DALE_VULKAN_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES dale_vk_shim)
endif()
add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL SYSTEM)
# Unoptimized, llama.cpp exceeds the inference time limit: -O2 in Debug as well.
set(DALE_GGML_TARGETS ggml-base ggml-cpu ggml llama)
if(DALE_VULKAN)
  list(APPEND DALE_GGML_TARGETS ggml-vulkan)
endif()
foreach(t IN LISTS DALE_GGML_TARGETS)
  target_compile_options(${t} PRIVATE $<$<CONFIG:Debug>:-O2>)
endforeach()

# Library with all modules. A new file in src/<module>/ is picked up automatically,
# without touching this CMakeLists.txt. *_main.cpp files are separate programs.
set(DALE_MODULES cli config context risk exec infer fetch)
set(DALE_CORE_SOURCES)
foreach(module IN LISTS DALE_MODULES)
  file(GLOB module_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/${module}/*.cpp")
  list(FILTER module_sources EXCLUDE REGEX "_main\\.cpp$")
  list(APPEND DALE_CORE_SOURCES ${module_sources})
endforeach()

add_library(dale_core STATIC ${DALE_CORE_SOURCES})
target_include_directories(dale_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(dale_core PUBLIC llama vendor::nlohmann)
target_compile_definitions(dale_core PUBLIC
  DALE_VERSION="${PROJECT_VERSION}"
  DALE_TEST_HOOKS=$<BOOL:${DALE_TEST_HOOKS}>
  DALE_VULKAN=$<BOOL:${DALE_VULKAN}>)
if(DALE_VULKAN)
  target_link_libraries(dale_core PUBLIC dale_vk_shim)
endif()
if(NOT DALE_MODEL_URL STREQUAL "")
  target_compile_definitions(dale_core PUBLIC DALE_MODEL_URL="${DALE_MODEL_URL}")
endif()
dale_warnings(dale_core)

add_executable(dale src/main.cpp)
target_link_libraries(dale PRIVATE dale_core)
dale_warnings(dale)

add_executable(dale_risk src/risk/dale_risk_main.cpp)
target_link_libraries(dale_risk PRIVATE dale_core)
dale_warnings(dale_risk)

# Tests
add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE "${CMAKE_SOURCE_DIR}/third_party/doctest")

enable_testing()

file(GLOB DALE_UNIT_TESTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/*_test.cpp")
add_executable(dale_tests tests/test_main.cpp ${DALE_UNIT_TESTS})
target_link_libraries(dale_tests PRIVATE dale_core doctest)
target_compile_definitions(dale_tests PRIVATE
  DALE_RISK_CASES_DIR="${CMAKE_SOURCE_DIR}/tests/risk"
  DALE_TESTS_DIR="${CMAKE_SOURCE_DIR}/tests"
  DALE_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
dale_warnings(dale_tests)
# The "model" suites need a GGUF (DALE_TEST_MODEL) and run separately: ctest -L model.
add_test(NAME dale_tests COMMAND dale_tests -tse=model)
add_test(NAME dale_tests_model COMMAND dale_tests -ts=model)
set_tests_properties(dale_tests_model PROPERTIES LABELS model)

file(GLOB DALE_E2E_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/e2e/*.cpp")
add_executable(dale_e2e tests/test_main.cpp ${DALE_E2E_SOURCES})
target_link_libraries(dale_e2e PRIVATE doctest util)
target_compile_definitions(dale_e2e PRIVATE
  DALE_BIN="$<TARGET_FILE:dale>"
  DALE_E2E_DIR="${CMAKE_SOURCE_DIR}/tests/e2e"
  DALE_RISK_CASES_DIR="${CMAKE_SOURCE_DIR}/tests/risk")
add_dependencies(dale_e2e dale)
dale_warnings(dale_e2e)
add_test(NAME dale_e2e COMMAND dale_e2e)
