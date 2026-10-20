# SPDX-License-Identifier: GPL-3.0-or-later
#
# The fake worker (docs/IMPLEMENTATION.md §4): a test-only executable that
# speaks the worker protocol and misbehaves on demand, for the supervision
# tier. Never installed -- there is no install() anywhere in this tree, and
# tools/check-no-test-hooks.sh refuses a shipped binary that carries its
# symbols. It links the IPC library, the protocol's provider, and not
# loadforge_worker, so it cannot reuse the real worker's loop (ADR 0003).
#
# Held to the product's warning set rather than the tests' reduced one: it is
# a second implementation of a contract, and it is tidied with src/ in CI.
add_executable(loadforge_fake_worker fake_worker.cpp)
target_link_libraries(loadforge_fake_worker PRIVATE loadforge_ipc)
loadforge_set_warnings(loadforge_fake_worker)
loadforge_set_fp_contract(loadforge_fake_worker)
loadforge_enable_sanitizers(loadforge_fake_worker)
# Linked against the coverage runtime, because the libraries it links are
# instrumented, but not compiled with instrumentation itself: its own template
# instantiations of src/ headers (core::Result over its own types) would
# otherwise enter the coverage report as src/ lines that no test of src/ is
# meant to reach. The gate measures src/ by the tests of src/.
if(LOADFORGE_COVERAGE)
  target_link_options(loadforge_fake_worker PRIVATE --coverage)
endif()
