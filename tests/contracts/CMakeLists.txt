# SPDX-License-Identifier: GPL-3.0-or-later
#
# The contracts' own tests (ADR 0003, ADR 0004). Three things live here:
#
#   * the provider-side suite of each contract, a GoogleTest typed suite in
#     <area>/<name>_contract.hpp that any implementation, real or mock, is run
#     against -- by its provider's tests, and by each consumer's tests against
#     the consumer's own mock;
#   * a trivial reference implementation of each contract, instantiating its
#     suite, so the suite is proved runnable before any stream relies on it;
#   * unit tests of the few definitions a contract header carries.
#
# Owned by the contracts, not by any stream: a stream instantiates a suite and
# never edits one. Every *_test.cpp below this directory is a source of the one
# executable, found by glob so a new contract needs no edit here.
file(GLOB_RECURSE contract_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
loadforge_test_suite(loadforge_contract_tests
  SOURCES ${contract_tests}
  LINK loadforge_topology loadforge_controller)

# Every contract header compiles on its own, under the product's warning set:
# one translation unit per header in headers/, each including its header and
# nothing else, so the header is proved self-contained.
# The units are compiled and never run; they exist because a header only the
# tests include would otherwise be held to the tests' laxer warnings, and the
# first stream to include it from src/ would meet the failures in a file it is
# not allowed to edit. CI runs clang-tidy over the same units for that reason.
file(GLOB contract_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/headers/*.cpp")
add_library(loadforge_contract_headers OBJECT ${contract_headers})
target_link_libraries(loadforge_contract_headers PRIVATE loadforge_topology)
loadforge_set_warnings(loadforge_contract_headers)
loadforge_set_fp_contract(loadforge_contract_headers)
