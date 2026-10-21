# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): the T1 and T2 tests of src/telemetry/sampler and nothing else.
loadforge_test_suite(loadforge_unit_telemetry_sampler_tests
  SOURCES
    round_sampler_test.cpp
  LINK loadforge_telemetry_sampler loadforge_telemetry_timeline)
