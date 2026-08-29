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
)
target_link_libraries(loadforge_platform PUBLIC loadforge_core)
loadforge_set_warnings(loadforge_platform)
loadforge_set_fp_contract(loadforge_platform)
loadforge_enable_sanitizers(loadforge_platform)
loadforge_enable_coverage(loadforge_platform)

# ---- cli --------------------------------------------------------------------
add_library(loadforge_cli STATIC src/main/cli.cpp src/main/selftest.cpp)
target_link_libraries(loadforge_cli PUBLIC loadforge_core)
loadforge_set_warnings(loadforge_cli)
loadforge_set_fp_contract(loadforge_cli)
loadforge_enable_sanitizers(loadforge_cli)
loadforge_enable_coverage(loadforge_cli)

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
