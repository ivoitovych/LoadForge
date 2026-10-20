# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.28)

project(LoadForge
  VERSION 0.0.0
  DESCRIPTION "Lightweight, self-verifying hardware reliability and load-testing framework for Linux"
  LANGUAGES CXX
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "LoadForge targets Linux only (see docs/platforms.md).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# C++20 modules are not usable on the reference toolchain (docs/platforms.md §2),
# so the project uses classic headers. Turning the scan off keeps '@...modmap'
# out of compile_commands.json, which otherwise makes clang-tidy unusable
# without a prior build.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LOADFORGE_BUILD_TESTS "Build the test suite" ON)
option(LOADFORGE_COVERAGE    "Instrument for coverage" OFF)
option(LOADFORGE_STATIC      "Link the executable fully statically (F19)" OFF)
set(LOADFORGE_SANITIZER "none" CACHE STRING "none|address|thread")
set(LOADFORGE_SIMD "auto" CACHE STRING "auto|scalar -- 'scalar' proves no ISA intrinsic leaked into portable code (§4.5)")

# One authoritative version, generated into a header rather than repeated in
# source. LOADFORGE_MILESTONE is appended so a build identifies its milestone.
set(LOADFORGE_MILESTONE "m0" CACHE STRING "Milestone suffix for the version string")
set(LOADFORGE_VERSION_STRING "${PROJECT_VERSION}-${LOADFORGE_MILESTONE}")
configure_file(src/core/version.hpp.in "${CMAKE_BINARY_DIR}/generated/core/version.hpp" @ONLY)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(FloatingPoint)
include(Sanitizers)
include(Coverage)
include(Dependencies)

# ---- core -------------------------------------------------------------------
add_library(loadforge_core STATIC
  src/core/parse_error.cpp
  src/core/duration.cpp
  src/core/byte_size.cpp
  src/core/parse_number.cpp
  src/core/crc32.cpp
)
target_include_directories(loadforge_core PUBLIC src "${CMAKE_BINARY_DIR}/generated")
loadforge_set_warnings(loadforge_core)
loadforge_set_fp_contract(loadforge_core)
loadforge_enable_sanitizers(loadforge_core)
loadforge_enable_coverage(loadforge_core)
if(LOADFORGE_SIMD STREQUAL "scalar")
  target_compile_definitions(loadforge_core PUBLIC LOADFORGE_SIMD_SCALAR=1)
endif()

# ---- platform ---------------------------------------------------------------
# The seam every later module's error paths depend on. Kept a separate library
# so that a link error is the immediate answer if anything above the platform
# boundary reaches for a syscall directly (docs/PLAN.md §4.2).
add_library(loadforge_platform STATIC
  src/platform/syscall_error.cpp
  src/platform/syscalls.cpp
  src/platform/fs.cpp
  src/platform/exit_status.cpp
  src/platform/process.cpp
  src/platform/worker_pool.cpp
  src/platform/clock.cpp
  src/platform/affinity.cpp
  src/platform/memory.cpp
)
target_link_libraries(loadforge_platform PUBLIC loadforge_core)
loadforge_set_warnings(loadforge_platform)
loadforge_set_fp_contract(loadforge_platform)
loadforge_enable_sanitizers(loadforge_platform)
loadforge_enable_coverage(loadforge_platform)

# ---- topology ---------------------------------------------------------------
# What the machine is: cores, threads, caches, NUMA. Reads /sys through the
# platform layer, so it depends on platform and platform never depends on it.
add_library(loadforge_topology STATIC
  src/topology/cpu_list.cpp
  src/topology/source.cpp
  src/topology/cpu_topology.cpp
  src/topology/cache.cpp
  src/topology/numa.cpp
)
target_link_libraries(loadforge_topology PUBLIC loadforge_platform)
loadforge_set_warnings(loadforge_topology)
loadforge_set_fp_contract(loadforge_topology)
loadforge_enable_sanitizers(loadforge_topology)
loadforge_enable_coverage(loadforge_topology)

# ---- resources --------------------------------------------------------------
# What the kernel lets this process use and what it records when that runs
# out: MemAvailable, the memory cgroup's limit under either hierarchy, and the
# cgroup's OOM-kill count. Platform-level knowledge read through the seam, but
# classified with topology's capability model, so it sits above topology
# rather than inside platform, which nothing may make depend on topology.
add_library(loadforge_resources STATIC
  src/resources/decimal.cpp
  src/resources/cgroup.cpp
  src/resources/memory_budget.cpp
  src/resources/cgroup_oom_record.cpp
)
target_link_libraries(loadforge_resources PUBLIC loadforge_topology loadforge_config)
loadforge_set_warnings(loadforge_resources)
loadforge_set_fp_contract(loadforge_resources)
loadforge_enable_sanitizers(loadforge_resources)
loadforge_enable_coverage(loadforge_resources)

# ---- controller -------------------------------------------------------------
# The supervisor: spawns the workers, judges their beats and their records,
# stops them at the deadline and classifies what became of each. Depends on
# worker (the IPC and the argv it execs workers with) and config, and on the
# OOM-kill record's contract in resources; the console reporter is the one
# voice it has at M1.
add_library(loadforge_controller STATIC
  src/controller/supervision.cpp
  src/controller/safety.cpp
  src/controller/orchestration.cpp
  src/reporters/console.cpp
)
target_link_libraries(loadforge_controller PUBLIC loadforge_worker loadforge_config loadforge_resources)
loadforge_set_warnings(loadforge_controller)
loadforge_set_fp_contract(loadforge_controller)
loadforge_enable_sanitizers(loadforge_controller)
loadforge_enable_coverage(loadforge_controller)

# ---- cli --------------------------------------------------------------------
add_library(loadforge_cli STATIC src/main/cli.cpp src/main/selftest.cpp)
target_link_libraries(loadforge_cli PUBLIC loadforge_core loadforge_config loadforge_worker loadforge_controller loadforge_resources)
loadforge_set_warnings(loadforge_cli)
loadforge_set_fp_contract(loadforge_cli)
loadforge_enable_sanitizers(loadforge_cli)
loadforge_enable_coverage(loadforge_cli)

# ---- worker -----------------------------------------------------------------
# The IPC the worker shares with the controller, and what runs inside a worker
# process. Depends on platform for the seam; the controller depends on this,
# never the other way round.
#
# The IPC is a library of its own because it is the protocol's provider, and a
# second implementation of the worker -- the test-only fake worker of the
# supervision tier -- links it without the real worker's loop (ADR 0003). With
# one library, nothing but review would keep that fake from calling the loop it
# is meant to be independent of; with two, it is a link error.
add_library(loadforge_ipc STATIC
  src/worker/ipc/heartbeat.cpp
  src/worker/ipc/record.cpp
)
target_link_libraries(loadforge_ipc PUBLIC loadforge_platform)
loadforge_set_warnings(loadforge_ipc)
loadforge_set_fp_contract(loadforge_ipc)
loadforge_enable_sanitizers(loadforge_ipc)
loadforge_enable_coverage(loadforge_ipc)

add_library(loadforge_worker STATIC
  src/worker/scheduler.cpp
  src/worker/entry.cpp
)
target_link_libraries(loadforge_worker PUBLIC loadforge_ipc)
loadforge_set_warnings(loadforge_worker)
loadforge_set_fp_contract(loadforge_worker)
loadforge_enable_sanitizers(loadforge_worker)
loadforge_enable_coverage(loadforge_worker)

# ---- config -----------------------------------------------------------------
# TOML parse, schema validation, resolution. The vendored parser is a PRIVATE
# dependency: nothing that includes config.hpp compiles toml.hpp, and the
# executable still links nothing but the C and C++ runtime.
loadforge_provide_tomlplusplus()
add_library(loadforge_config STATIC src/config/config.cpp)
target_link_libraries(loadforge_config PUBLIC loadforge_core PRIVATE loadforge::tomlplusplus)
loadforge_set_warnings(loadforge_config)
loadforge_set_fp_contract(loadforge_config)
loadforge_enable_sanitizers(loadforge_config)
loadforge_enable_coverage(loadforge_config)

# ---- executable -------------------------------------------------------------
add_executable(loadforge src/main/main.cpp)
target_link_libraries(loadforge PRIVATE loadforge_cli)
loadforge_set_warnings(loadforge)
loadforge_set_fp_contract(loadforge)
loadforge_enable_sanitizers(loadforge)
loadforge_enable_coverage(loadforge)
if(LOADFORGE_STATIC)
  target_link_options(loadforge PRIVATE -static)
endif()

# ---- tests ------------------------------------------------------------------
if(LOADFORGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "LoadForge: compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
               "sanitizer=${LOADFORGE_SANITIZER} coverage=${LOADFORGE_COVERAGE} "
               "simd=${LOADFORGE_SIMD} static=${LOADFORGE_STATIC}")
