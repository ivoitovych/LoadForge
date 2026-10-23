# SPDX-License-Identifier: GPL-3.0-or-later
#
# The version string the binary reports: the project version, with the milestone
# appended only when there is one. A release build sets LOADFORGE_MILESTONE to the
# empty string and must print exactly X.Y.Z, because the release workflow refuses
# a tag that differs from what `loadforge --version` prints.
#
# Run as a script to print the string for given inputs (the tooling tests do):
#   cmake -DVERSION=1.0.0 -DMILESTONE= -P cmake/Version.cmake

function(loadforge_version_string out version milestone)
  if(milestone STREQUAL "")
    set(${out} "${version}" PARENT_SCOPE)
  else()
    set(${out} "${version}-${milestone}" PARENT_SCOPE)
  endif()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  loadforge_version_string(text "${VERSION}" "${MILESTONE}")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${text}")
endif()
