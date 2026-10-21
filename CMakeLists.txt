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
set(LOADFORGE_MILESTONE "m2" CACHE STRING "Milestone suffix for the version string")
set(LOADFORGE_VERSION_STRING "${PROJECT_VERSION}-${LOADFORGE_MILESTONE}")
configure_file(src/core/version.hpp.in "${CMAKE_BINARY_DIR}/generated/core/version.hpp" @ONLY)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(FloatingPoint)
include(Sanitizers)
include(Coverage)
include(Dependencies)
include(Components)

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
  src/topology/snapshot.cpp
)
target_link_libraries(loadforge_topology PUBLIC loadforge_platform)
loadforge_set_warnings(loadforge_topology)
loadforge_set_fp_contract(loadforge_topology)
loadforge_enable_sanitizers(loadforge_topology)
loadforge_enable_coverage(loadforge_topology)

# ---- controller -------------------------------------------------------------
# The supervisor: spawns the workers, judges their beats and their records,
# stops them at the deadline and classifies what became of each. Depends on
# worker (the IPC and the argv it execs workers with) and config, and on the
# OOM-kill record's contract in resources (src/resources/CMakeLists.txt, below
# by glob: a link names a target, so it may precede the definition); the
# console reporter is the one voice it has at M1.
#
# From M2a it also plans what each worker runs (plan), collects the records and
# tallies they send, repeats a wrong result in a process of its own (rerun) and
# climbs the isolation ladder over a finished run's findings (isolate). The
# libraries below the first line are the contracts' providers it links; the
# registry is not among them, because the controller reads workloads through
# workloads::Registry and is handed one.
add_library(loadforge_controller STATIC
  src/controller/supervision.cpp
  src/controller/safety.cpp
  src/controller/plan.cpp
  src/controller/rerun.cpp
  src/controller/isolate.cpp
  src/controller/orchestration.cpp
  src/reporters/console.cpp
)
target_link_libraries(loadforge_controller PUBLIC loadforge_worker loadforge_config loadforge_resources
  loadforge_verification_record loadforge_verification_isolation loadforge_telemetry_capability
  loadforge_primitives_simd loadforge_platform_fpenv)
loadforge_set_warnings(loadforge_controller)
loadforge_set_fp_contract(loadforge_controller)
loadforge_enable_sanitizers(loadforge_controller)
loadforge_enable_coverage(loadforge_controller)

# ---- cli --------------------------------------------------------------------
# Everything a run is made of meets here: the supervisor, every telemetry
# source and the pipeline behind them, the statistics, the result and its
# writers, and the registry of workloads a verified run draws on. The
# libraries below are found by glob (src/*/CMakeLists.txt).
add_library(loadforge_cli STATIC
  src/main/cli.cpp
  src/main/selftest.cpp
  src/main/outcome.cpp
  src/main/telemetry_rig.cpp
  src/main/run_result.cpp
  src/main/result_files.cpp
)
target_link_libraries(loadforge_cli PUBLIC loadforge_core loadforge_config loadforge_worker loadforge_controller loadforge_resources
  loadforge_telemetry_sources_thermal loadforge_telemetry_sources_fan loadforge_telemetry_sources_frequency
  loadforge_telemetry_sources_memory loadforge_telemetry_sources_power loadforge_telemetry_sources_counters
  loadforge_telemetry_sources_edac loadforge_telemetry_sampler loadforge_telemetry_timeline
  loadforge_telemetry_capability loadforge_statistics loadforge_controller_results
  loadforge_reporters_json loadforge_reporters_csv loadforge_platform_fpenv loadforge_platform_isa
  loadforge_primitives_simd loadforge_workloads_registry)
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
  src/worker/control_channel.cpp
  src/worker/duty.cpp
  src/worker/frames.cpp
  src/worker/verified_run.cpp
  src/worker/services.cpp
  src/worker/entry.cpp
)
# From M2a the worker runs real workloads: it links the registries of
# workloads and their oracles, the input-integrity runner, the executor, the
# floating-point registers and the ISA probe, and the codec of the records it
# sends. It links no workload by name; the registries do.
target_link_libraries(loadforge_worker PUBLIC loadforge_ipc loadforge_worker_control
  loadforge_workloads_registry loadforge_verification_oracle_registry
  loadforge_verification_integrity loadforge_worker_executor
  loadforge_platform_fpenv loadforge_platform_isa loadforge_primitives_simd
  loadforge_verification_record)
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

# ---- components built in parallel streams ------------------------------------
# Every component since ADR 0004 -- src/resources the first, then each from M2a
# on -- owns a CMakeLists.txt in its own directory under src/ and is found here
# by glob, so no stream edits this file to add itself (cmake/Components.cmake).
# The modules above predate the mechanism and stay listed explicitly.
loadforge_add_fragments("${CMAKE_CURRENT_SOURCE_DIR}/src")

# ---- tests ------------------------------------------------------------------
if(LOADFORGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "LoadForge: compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
               "sanitizer=${LOADFORGE_SANITIZER} coverage=${LOADFORGE_COVERAGE} "
               "simd=${LOADFORGE_SIMD} static=${LOADFORGE_STATIC}")
