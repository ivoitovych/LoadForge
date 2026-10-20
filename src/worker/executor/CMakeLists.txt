# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/worker/executor and nothing else.
#
# The thread pool behind worker/executor.hpp. It places its threads through
# platform/affinity and reads the floating-point state through the fpenv
# contract's FpControl, which it is handed; it links no implementation of
# fpenv, whose provider is another stream. Threads::Entry is a pthread entry,
# so the library links the threads library itself rather than relying on a
# consumer to.
find_package(Threads REQUIRED)
loadforge_component(loadforge_worker_executor
  SOURCES thread_pool.cpp
  LINK loadforge_platform Threads::Threads)
