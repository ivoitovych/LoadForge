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
