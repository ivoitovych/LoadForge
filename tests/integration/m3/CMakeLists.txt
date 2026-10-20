# SPDX-License-Identifier: GPL-3.0-or-later
# Test fragment (ADR 0004): tier T8 of the M3 integration.
loadforge_test_suite(loadforge_integration_m3_tests
  SOURCES full_signature_test.cpp
  LINK loadforge_cli)
