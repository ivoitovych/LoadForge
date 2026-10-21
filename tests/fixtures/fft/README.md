# FFT fixtures

Date: 2026-10-20 (UTC)

`twiddles_full.txt` and `src/workloads/fft/twiddle_table.hpp` are the output of
`tests/fixtures/fft/gen_twiddles.py full` and `... header`, run with
Python 3.13.16 and mpmath 1.4.1 (120 decimal digits) on x86-64 Linux 6.18. The
header (piped through `clang-format-18`, the only change it makes is layout) holds the first octant only; the file holds every W[m] of the
specification computed directly from its own angle, with no symmetry used, so
the test that compares the table built from the header against the file checks
the images, not just the octant.

The golden digests in `tests/determinism/workloads/fft/determinism_test.cpp`
are FNV-1a 64-bit digests of the generated input and of the computed output
for seed 20261020, units 0 and 1234567, both radices, scalar path, taken on
x86-64 Linux with GCC 13 at -O0 and re-taken at -O2 with GCC 13 and with Clang 18
(they agree). They pin bits already shown right by the oracle tier; they are a
tripwire, not an oracle.
