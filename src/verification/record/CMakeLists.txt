# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/record and nothing else.
#
# The provider of encode() and decode() declared in verification/record.hpp:
# the error record on the record channel, and the record rendered for a
# person. Both ends of the channel link this one library.
loadforge_component(loadforge_verification_record
  SOURCES
    codec.cpp
    format.cpp
    render.cpp)
