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

set(LOADFORGE_GTEST_TAG "v1.15.2" CACHE STRING "Pinned GoogleTest tag")

function(loadforge_provide_gtest)
  find_package(GTest QUIET)
  if(GTest_FOUND)
    message(STATUS "GoogleTest: using system installation")
    return()
  endif()

  message(STATUS "GoogleTest: fetching pinned ${LOADFORGE_GTEST_TAG}")
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        ${LOADFORGE_GTEST_TAG}
    GIT_SHALLOW    TRUE
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK   ON  CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endfunction()
