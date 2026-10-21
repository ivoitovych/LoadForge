#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the FFT oracle's real-tool fixtures: numpy's transform of two units.

Run from the repository root:

    python3 tests/fixtures/verification/oracle/fft/generate_fixtures.py

For each (seed, index) below this builds the unit's input the way the
specification says (entry_value of the Philox draw for each of the 2 n
elements), transforms it with numpy.fft.fft, and writes the n complex results
as 2 n little-endian doubles, re_0, im_0, re_1, ... -- the layout of the
specification's output -- to unit-<index>.output.bin. The inputs are not
committed: the tests regenerate them with src/primitives/rng/philox.hpp.

The committed files were produced with Python 3.13.16 and numpy 2.5.3 on an
x86-64 Linux machine (Intel Xeon 2.10GHz, kernel 6.18.44, glibc 2.39). numpy's
transform is not the specification's (it is a different algorithm with a
different rounding), which is the point: the oracle must accept it within the
derived tolerance.
"""

import pathlib
import sys

import numpy

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from philox_reference import block_at, u64_of  # noqa: E402

POINTS = 4096
SEED = 0x4C6F616446697263
INDICES = (5, 1031)


def entry_value(bits):
    return float(bits >> 11) * 2.0**-52 - 1.0


def unit_input(seed, index):
    return numpy.array(
        [entry_value(u64_of(block_at(seed, index, e))) for e in range(2 * POINTS)],
        dtype="<f8",
    )


def main():
    here = pathlib.Path(__file__).resolve().parent
    for index in INDICES:
        x = unit_input(SEED, index)
        spectrum = numpy.fft.fft(x[0::2] + 1j * x[1::2])
        out = numpy.empty(2 * POINTS, dtype="<f8")
        out[0::2] = spectrum.real
        out[1::2] = spectrum.imag
        (here / f"unit-{index}.output.bin").write_bytes(out.tobytes())


if __name__ == "__main__":
    main()
