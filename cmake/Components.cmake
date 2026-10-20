# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build fragments: how a component adds itself to the build without editing a
# file another component also edits (ADR 0004).
#
# WHY
# ---
# The work after M1 runs as parallel streams, one component each. If every
# stream added its library to the top-level CMakeLists.txt and its tests to
# tests/CMakeLists.txt, every pair of streams would conflict in those two files.
# So each component owns a CMakeLists.txt in its own directories -- one under
# src/ for its library, one under each tests/<tier>/ directory it has tests in --
# and the two shared files find them by globbing rather than by a list someone
# has to edit.
#
# A glob rather than a pre-populated list, because a list is a second record of
# what exists and the two drift: a fragment missing from the list is silently
# not built, its tests silently not run, and every gate downstream reports on
# less than the tree holds (F20). A glob builds what is there. CONFIGURE_DEPENDS
# re-runs it on every build, so a new fragment is picked up without a manual
# reconfigure.
#
# THE RULES A FRAGMENT FOLLOWS
# ----------------------------
#   * It defines targets with the functions below, which apply the project's
#     warnings, floating-point contract, sanitizers and coverage instrumentation.
#     A library built any other way would be a library the gates do not see.
#   * It never calls add_subdirectory(). Every fragment is added by the glob; a
#     fragment that also added another would add it twice, which CMake refuses
#     at configure time -- loudly, which is the right way for it to fail.
#   * It names its targets after its directory: loadforge_<dir>_<subdir> for a
#     library, loadforge_<tier>_<dir>_<subdir>_tests for a test executable, so
#     two components cannot collide on a name.

# loadforge_add_fragments(<directory> [EXCLUDE <file> ...] [EXCLUDE_TREES <dir> ...])
#
# add_subdirectory() for every CMakeLists.txt below <directory>, at any depth,
# in sorted order, except <directory>/CMakeLists.txt itself, the files named in
# EXCLUDE (paths relative to <directory>), and anything under EXCLUDE_TREES.
function(loadforge_add_fragments directory)
  cmake_parse_arguments(arg "" "" "EXCLUDE;EXCLUDE_TREES" ${ARGN})
  file(GLOB_RECURSE fragments LIST_DIRECTORIES false CONFIGURE_DEPENDS
       "${directory}/CMakeLists.txt")
  list(SORT fragments)
  set(added "")
  foreach(fragment IN LISTS fragments)
    file(RELATIVE_PATH relative "${directory}" "${fragment}")
    if(relative STREQUAL "CMakeLists.txt" OR relative IN_LIST arg_EXCLUDE)
      continue()
    endif()
    set(skip FALSE)
    foreach(tree IN LISTS arg_EXCLUDE_TREES)
      if(relative MATCHES "^${tree}/")
        set(skip TRUE)
      endif()
    endforeach()
    if(skip)
      continue()
    endif()
    cmake_path(GET fragment PARENT_PATH fragment_directory)
    add_subdirectory("${fragment_directory}")
    list(APPEND added "${relative}")
  endforeach()
  list(LENGTH added count)
  file(RELATIVE_PATH where "${PROJECT_SOURCE_DIR}" "${directory}")
  message(STATUS "LoadForge: ${count} build fragment(s) under ${where}/")
endfunction()

# loadforge_component(<target> SOURCES <file> ... [LINK <target> ...])
#
# A component's static library, held to the product's warning set and
# instrumented for coverage like every library in the tree. Links
# loadforge_core, which carries the src/ include path, so a component includes
# any contract header as "area/name.hpp".
function(loadforge_component target)
  cmake_parse_arguments(arg "" "" "SOURCES;LINK" ${ARGN})
  if(NOT arg_SOURCES)
    message(FATAL_ERROR "loadforge_component(${target}): no SOURCES. A header-only "
                        "contract needs no target; anything else has a source file.")
  endif()
  add_library(${target} STATIC ${arg_SOURCES})
  target_link_libraries(${target} PUBLIC loadforge_core ${arg_LINK})
  loadforge_set_warnings(${target})
  loadforge_set_fp_contract(${target})
  loadforge_enable_sanitizers(${target})
  loadforge_enable_coverage(${target})
endfunction()

# loadforge_test_suite(<target> SOURCES <file> ... [LINK <target> ...])
#
# A GoogleTest executable, discovered into ctest. tests/ is on its include path,
# as for the unit tests, so a suite includes a contract's provider-side suite as
# "contracts/<area>/<name>_contract.hpp". LOADFORGE_SOURCE_DIR locates committed
# fixtures by absolute path. Instrumented for coverage, because these are the
# tests the coverage gate measures src/ by.
function(loadforge_test_suite target)
  cmake_parse_arguments(arg "" "" "SOURCES;LINK" ${ARGN})
  if(NOT arg_SOURCES)
    message(FATAL_ERROR "loadforge_test_suite(${target}): no SOURCES")
  endif()
  add_executable(${target} ${arg_SOURCES})
  target_link_libraries(${target} PRIVATE loadforge_core ${arg_LINK} GTest::gtest_main GTest::gmock)
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/tests")
  target_compile_definitions(${target} PRIVATE LOADFORGE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  loadforge_set_fp_contract(${target})
  loadforge_enable_sanitizers(${target})
  loadforge_enable_coverage(${target})
  # The tests' warning set: the product's minus what the framework's own macros
  # violate, as for loadforge_unit_tests.
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  gtest_discover_tests(${target})
endfunction()
