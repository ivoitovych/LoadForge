# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T5 and T5b for src/workloads/raytrace and nothing else.
#
# Faults injected where the workload can be hurt (an executor that drops a
# task, a hierarchy whose boxes are too small, an output with a flipped bit)
# and the classification the comparison with brute force gives each; and the
# comparison that cannot be made, which is never a pass.
loadforge_test_suite(loadforge_fault_injection_workloads_raytrace_tests
  SOURCES
    fault_test.cpp
  LINK loadforge_workloads_raytrace)
