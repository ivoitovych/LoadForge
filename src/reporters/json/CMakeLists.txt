# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/reporters/json and nothing else.
#
# run.json: the result as one JSON document, schema-versioned (F12).
loadforge_component(loadforge_reporters_json
  SOURCES
    json_text.cpp
    json_writer.cpp
  LINK loadforge_controller_results)
