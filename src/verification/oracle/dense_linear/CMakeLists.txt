# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/verification/oracle/dense_linear and nothing else.
#
# The independent oracle for the dense-linear workload, written from its
# specification and judged with the residual module's derived tolerance.
loadforge_component(loadforge_verification_oracle_dense_linear
  SOURCES
    dense_linear.cpp
  LINK loadforge_verification_residual)
