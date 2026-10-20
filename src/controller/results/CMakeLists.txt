# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/controller/results and nothing else.
#
# What a run comes to: the verdict, the aggregation helpers and the run
# fingerprint (F12). The fingerprint's build half is fixed at configure time:
# the flags this build was compiled with and the commit it was configured from.
# A build outside a git checkout has no commit, and the fingerprint says so.
set(_lf_flags "${CMAKE_CXX_FLAGS}")
string(TOUPPER "${CMAKE_BUILD_TYPE}" _lf_build_type)
if(_lf_build_type)
  string(APPEND _lf_flags " ${CMAKE_CXX_FLAGS_${_lf_build_type}}")
endif()
string(APPEND _lf_flags " -ffp-contract=off")
string(STRIP "${_lf_flags}" _lf_flags)
string(REPLACE "\\" "" _lf_flags "${_lf_flags}")
string(REPLACE "\"" "" _lf_flags "${_lf_flags}")

set(_lf_commit "")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE _lf_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _lf_git_status)
  if(NOT _lf_git_status EQUAL 0)
    set(_lf_commit "")
  endif()
endif()

loadforge_component(loadforge_controller_results
  SOURCES
    verdict.cpp
    aggregate.cpp
    fingerprint.cpp
  LINK loadforge_resources loadforge_topology loadforge_platform)
target_compile_definitions(loadforge_controller_results PRIVATE
  LOADFORGE_BUILD_FLAGS="${_lf_flags}"
  LOADFORGE_GIT_COMMIT="${_lf_commit}")
