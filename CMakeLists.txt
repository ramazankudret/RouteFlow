cmake_minimum_required(VERSION 3.16)
project(routeflow CXX)

# ARCHITECTURE §3: C++17, no third-party libraries. The tree must build on a
# fresh Jetson with nothing but a compiler.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

# --- common -------------------------------------------------------------------

add_library(rf_common STATIC
  common/json.cpp
  common/util.cpp
  common/http.cpp
  common/types.cpp
  common/config.cpp
)
target_include_directories(rf_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rf_common PUBLIC Threads::Threads)

# --- agent --------------------------------------------------------------------

add_executable(routeflow-agent
  agent/main.cpp
  agent/telemetry/telemetry.cpp
  agent/telemetry/nvml.cpp
  agent/telemetry/tegra.cpp
  agent/telemetry/null.cpp
  agent/engine/ollama_probe.cpp
  agent/engine/lmstudio_probe.cpp
  agent/sim/sim_node.cpp
)
target_link_libraries(routeflow-agent PRIVATE rf_common ${CMAKE_DL_LIBS})

# --- router -------------------------------------------------------------------

# The router's logic is a library so the policy and cost-model tests can link it
# without going through a process and a socket. A test that has to start a
# server to check which node a policy picks is not a unit test.
add_library(rf_router STATIC
  router/core/admission.cpp
  router/core/dispatch.cpp
  router/core/ingest.cpp
  router/core/ledger.cpp
  router/core/registry.cpp
  router/core/router_state.cpp
  router/core/trace_writer.cpp
  router/cost/static_cost_model.cpp
  router/policy/candidates.cpp
  router/policy/round_robin.cpp
  router/policy/warmth.cpp
)
target_link_libraries(rf_router PUBLIC rf_common)

add_executable(routeflow-router router/main.cpp)
target_link_libraries(routeflow-router PRIVATE rf_router)

# --- tests --------------------------------------------------------------------

enable_testing()
add_executable(rf_selftest tests/selftest.cpp)
target_link_libraries(rf_selftest PRIVATE rf_common)
add_test(NAME selftest COMMAND rf_selftest)

add_executable(rf_policytest tests/policy_test.cpp)
target_link_libraries(rf_policytest PRIVATE rf_router)
add_test(NAME policytest COMMAND rf_policytest)
