#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the particles oracle's real-tool fixture: scipy's neighbour search.

Run from the repository root:

    python3 tests/fixtures/verification/oracle/particles/generate_fixtures.py

For each (seed, index) below this builds the unit's positions the way the
specification says (position_value of the Philox draw for coordinate e = 3 p +
c, a multiple of 2^-49 in [0, 16)), finds every pair within the cutoff radius 2
with scipy.spatial.cKDTree.query_ball_point -- a neighbour search, the real
tool for what the workload's cell list does -- and sums g(d) = max(0, 1 - q/4)^2
d over each particle's neighbours with numpy. The 3 * 4096 forces are written
as little-endian doubles to unit-<index>.output.bin. The positions are not
committed: the tests regenerate them with src/primitives/rng/philox.hpp.

The neighbour search is checked against a brute-force numpy sum over all pairs
for the first 64 particles, which must agree to 1e-12 absolutely: the search
finds exactly the pairs within the cutoff, and the two sums differ only in the
order of their terms.

The committed file was produced with Python 3.13.16, numpy 2.5.3 and scipy
1.18.1 on an x86-64 Linux machine (Intel Xeon 2.10GHz, kernel 6.18.44, glibc
2.39).
"""

import pathlib
import sys

import numpy
from scipy.spatial import cKDTree

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from philox_reference import block_at, u64_of  # noqa: E402

PARTICLES = 4096
CUTOFF = 2.0
SEED = 0x4C6F616446697263
INDICES = (13,)


def position_value(bits):
    return float(bits >> 11) * 2.0**-49


def unit_positions(seed, index):
    coordinates = [position_value(u64_of(block_at(seed, index, e))) for e in range(3 * PARTICLES)]
    return numpy.array(coordinates, dtype="<f8").reshape(PARTICLES, 3)


def term(difference):
    q = (difference * difference).sum(axis=-1)
    weight = numpy.maximum(0.0, 1.0 - q / 4.0) ** 2
    return weight[..., None] * difference


def forces_by_neighbour_search(positions):
    tree = cKDTree(positions)
    out = numpy.zeros_like(positions)
    for p, neighbours in enumerate(tree.query_ball_point(positions, CUTOFF)):
        others = numpy.array([j for j in neighbours if j != p], dtype=int)
        if others.size:
            out[p] = term(positions[p] - positions[others]).sum(axis=0)
    return out


def forces_brute_force(positions, particles):
    out = numpy.zeros((len(particles), 3))
    for row, p in enumerate(particles):
        others = numpy.delete(numpy.arange(PARTICLES), p)
        out[row] = term(positions[p] - positions[others]).sum(axis=0)
    return out


def main():
    here = pathlib.Path(__file__).resolve().parent
    for index in INDICES:
        positions = unit_positions(SEED, index)
        forces = forces_by_neighbour_search(positions)
        check = forces_brute_force(positions, range(64))
        assert numpy.abs(check - forces[:64]).max() < 1e-12
        (here / f"unit-{index}.output.bin").write_bytes(forces.astype("<f8").tobytes())


if __name__ == "__main__":
    main()
