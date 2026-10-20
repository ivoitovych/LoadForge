# FFT fixtures

Date: 2026-10-20 (UTC)

`twiddles_full.txt` and `src/workloads/fft/twiddle_table.hpp` are the output of
`tests/fixtures/fft/gen_twiddles.py full` and `... header`, run with
Python 3.13.16 and mpmath 1.4.1 (120 decimal digits) on x86-64 Linux 6.18. The
header holds the first octant only; the file holds every W[m] of the
specification computed directly from its own angle, with no symmetry used, so
the test that compares the table built from the header against the file checks
the images, not just the octant.

`golden.txt` holds output digests of the workload on this machine (see the
header of that file for the command that regenerates it).
