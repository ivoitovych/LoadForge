# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/reporters/csv and nothing else.
#
# The result as one CSV table, one row per fact (RFC 4180).
loadforge_component(loadforge_reporters_csv
  SOURCES
    csv_text.cpp
    csv_writer.cpp
  LINK loadforge_controller_results)
