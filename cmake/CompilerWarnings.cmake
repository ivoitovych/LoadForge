# SPDX-License-Identifier: GPL-3.0-or-later
#
# Warnings-as-errors, and the bans that docs/determinism.md depends on.

function(loadforge_set_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wnull-dereference
  )
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${target} PRIVATE
      -Wduplicated-cond
      -Wduplicated-branches
      -Wlogical-op
      -Wuseless-cast
    )
  endif()
endfunction()

# Guard against a build that would silently break the verification contract.
# docs/determinism.md §4 bans these outright: they permit reassociation and
# flush-to-zero, which is precisely what the determinism classes forbid.
foreach(_flagvar
        CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
  if("${${_flagvar}}" MATCHES "-ffast-math|-Ofast|-funsafe-math|-fassociative-math|-freciprocal-math")
    message(FATAL_ERROR
      "${_flagvar} contains a banned fast-math flag.\n"
      "These break the determinism contract (docs/determinism.md §4) by permitting "
      "reassociation and flush-to-zero. Remove it.")
  endif()
endforeach()
