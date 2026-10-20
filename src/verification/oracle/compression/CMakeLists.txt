#SPDX - License - Identifier : GPL - 3.0 - or -later
#Build fragment(ADR 0004) : src / verification / oracle / compression and nothing else.
#
#The compression workload's independent oracle: a strict LZ4 block decoder
#written from the block - format document, and the Oracle that judges a unit's
#output with it against the unit's input. Header-only contracts above it
#(verification / oracle.hpp, workloads / compression / spec.hpp) need no link.
loadforge_component(loadforge_verification_oracle_compression SOURCES decoder.cpp oracle.cpp)
