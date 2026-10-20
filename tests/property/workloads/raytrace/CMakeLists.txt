# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): T4 for src/workloads/raytrace and nothing else.
#
# What holds of every generated scene (the fraction of the view that is hit,
# the nearest distance, the record format) and of every scene derived from
# one in a way that must leave the image alone or change it in a known place.
loadforge_test_suite(loadforge_property_workloads_raytrace_tests
  SOURCES
    property_test.cpp
  LINK loadforge_workloads_raytrace)
