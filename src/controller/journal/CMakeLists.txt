# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/controller/journal and nothing else.
#
# The crash journal (F16): the record format, the writer, the replay, and a
# Storage over a real file opened for append and synced with fsync, behind a
# file seam of its own (file_ops.hpp) rather than platform::Syscalls.
loadforge_component(loadforge_controller_journal
  SOURCES
    format.cpp
    journal.cpp
    file_ops.cpp
    file_storage.cpp
  LINK loadforge_platform)
