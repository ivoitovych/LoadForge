# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test-only dependencies. Nothing here is linked into the shipped binary, so the
# project's "no third-party runtime dependencies" promise is unaffected.
#
# Resolution order, per docs/platforms.md:
#   1. a system-installed GTest  (offline / distro builds)
#   2. FetchContent at a pinned revision  (normal developer + CI path)
# and LOADFORGE_BUILD_TESTS=OFF skips it entirely, for live-media builds with no
# network and no need for tests.

# An immutable commit SHA, not a tag. A tag is a movable reference: pinning to
# one means the dependency can change under a build that claims to be pinned.
# For a project whose entire value proposition is trust, the supply chain gets
# the same treatment as the verification logic.
# This SHA is GoogleTest v1.15.2.
set(LOADFORGE_GTEST_SHA "b514bdc898e2951020cbdca1304b75f5950d1f59"
    CACHE STRING "Pinned GoogleTest commit SHA (v1.15.2)")

function(loadforge_provide_gtest)
  find_package(GTest QUIET)
  if(GTest_FOUND)
    message(STATUS "GoogleTest: using system installation")
    return()
  endif()

  message(STATUS "GoogleTest: fetching pinned ${LOADFORGE_GTEST_SHA}")
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        ${LOADFORGE_GTEST_SHA}
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK   ON  CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endfunction()

# Vendored, not fetched: config/ needs a TOML parser at M1, and an offline or
# live-media build must not need the network to get one. third_party/MANIFEST.toml
# records the upstream commit and a sha256 over the vendored tree, so an edit to
# the vendored code -- by a rebase, a well-meant fix, or a bad merge -- fails
# tools/check-dependencies.py. A fetched dependency is pinned by the download it
# verifies; a vendored one has no such moment, and the digest is its equivalent.
#
# SYSTEM include, deliberately. The project compiles under -Werror with an
# aggressive warning set (-Wconversion, -Wold-style-cast, -Wdouble-promotion and
# more), which third-party code has no obligation to satisfy. Marking it SYSTEM
# keeps our own code held to the full standard without demanding that a vendored
# header meet a bar it never agreed to.
function(loadforge_provide_tomlplusplus)
  add_library(loadforge_tomlplusplus INTERFACE)
  add_library(loadforge::tomlplusplus ALIAS loadforge_tomlplusplus)
  target_include_directories(loadforge_tomlplusplus SYSTEM INTERFACE
    "${CMAKE_SOURCE_DIR}/third_party/tomlplusplus")
  # Header-only: no compiled TU, no runtime dependency, nothing to link.
  target_compile_definitions(loadforge_tomlplusplus INTERFACE TOML_EXCEPTIONS=1)
endfunction()
