#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the stencil oracle's real-tool fixture: scipy's filter, eight sweeps.

Run from the repository root:

    python3 tests/fixtures/verification/oracle/stencil/generate_fixtures.py

For each (seed, index) below this builds the unit's input grid the way the
specification says (entry_value of the Philox draw for cell e = i * 256 + j),
applies the five-point weights with scipy.ndimage.correlate eight times, and
puts the input's ring back after every sweep (the ring is fixed). The result is
written as 256 * 256 little-endian doubles, row-major, to unit-<index>.output.bin.
The input is not committed: the tests regenerate it with
src/primitives/rng/philox.hpp.

The committed file was produced with Python 3.13.16, numpy 2.5.3 and scipy
1.18.1 on an x86-64 Linux machine (Intel Xeon 2.10GHz, kernel 6.18.44, glibc
2.39). scipy's filter is not the specification's sweep (its sum is formed in
another order, and it knows nothing of the ring), which is the point: the
oracle must accept its result within the derived tolerance.
"""

import pathlib
import sys

import numpy
from scipy import ndimage

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from philox_reference import block_at, u64_of  # noqa: E402

EDGE = 256
STEPS = 8
SEED = 0x4C6F616446697263
INDICES = (7,)
WEIGHTS = numpy.array([[0.0, 0.125, 0.0], [0.125, 0.5, 0.125], [0.0, 0.125, 0.0]])


def entry_value(bits):
    return float(bits >> 11) * 2.0**-52 - 1.0


def unit_input(seed, index):
    cells = [entry_value(u64_of(block_at(seed, index, e))) for e in range(EDGE * EDGE)]
    return numpy.array(cells, dtype="<f8").reshape(EDGE, EDGE)


def sweep_all(grid):
    ring = numpy.ones((EDGE, EDGE), dtype=bool)
    ring[1:-1, 1:-1] = False
    current = grid.copy()
    for _ in range(STEPS):
        filtered = ndimage.correlate(current, WEIGHTS, mode="constant", cval=0.0)
        current = numpy.where(ring, grid, filtered)
    return current


def main():
    here = pathlib.Path(__file__).resolve().parent
    for index in INDICES:
        out = sweep_all(unit_input(SEED, index)).astype("<f8")
        (here / f"unit-{index}.output.bin").write_bytes(out.tobytes())


if __name__ == "__main__":
    main()
