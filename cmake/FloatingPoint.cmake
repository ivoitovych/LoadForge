# SPDX-License-Identifier: GPL-3.0-or-later
#
# The floating-point contract from docs/determinism.md.
#
# -ffp-contract=off prevents *implicit*, compiler-discretionary fusion of a*b+c,
# which varies by compiler, version and optimization level and is a determinism
# hazard with no compensating benefit. It does NOT mean "no FMA": FMA is used
# deliberately, through ISA intrinsics, in the kernels that intend to stress the
# FMA units. See docs/determinism.md §4.

function(loadforge_set_fp_contract target)
  target_compile_options(${target} PRIVATE -ffp-contract=off)
endfunction()
