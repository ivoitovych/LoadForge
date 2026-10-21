#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the sparse oracle's real-tool fixture: scipy's CG and scipy's SpMV.

Run from the repository root:

    python3 tests/fixtures/verification/oracle/sparse/generate_fixtures.py

For each (seed, index) below this builds the unit's system the way the
specification says (a symmetric banded-sparse A with the diagonal 2.0 and the
pairs at offsets 1, 97 and 1031 drawn from Philox; b from Philox), runs
scipy.sparse.linalg.cg from x = 0 for exactly 16 iterations, and forms the
residual r = b - A x with scipy's sparse matrix-vector product. The output is
x (16384 doubles) then r (16384 doubles), little-endian, written to
unit-<index>.output.bin. The input is not committed: the tests regenerate it
with src/primitives/rng/philox.hpp.

The committed file was produced with Python 3.13.16, numpy 2.5.3 and scipy
1.18.1 on an x86-64 Linux machine (Intel Xeon 2.10GHz, kernel 6.18.44, glibc
2.39). scipy's conjugate gradient is not the specification's recurrence to the
last bit, which is the point: the oracle must accept its iterate and residual
within the derived tolerances.
"""

import pathlib
import sys

import numpy
from scipy import sparse
from scipy.sparse import linalg

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from philox_reference import block_at, u64_of  # noqa: E402

ROWS = 16384
OFFSETS = (1, 97, 1031)
ITERATIONS = 16
SEED = 0x4C6F616446697263
INDICES = (11,)


def entry_value(bits):
    return float(bits >> 11) * 2.0**-52 - 1.0


def unit_system(seed, index):
    """A (CSR, sorted columns within each row) and b, from the specification."""
    rows, cols, vals = [], [], []
    for i in range(ROWS):
        rows.append(i)
        cols.append(i)
        vals.append(2.0)
        for k, offset in enumerate(OFFSETS):
            if i + offset < ROWS:
                value = entry_value(u64_of(block_at(seed, index, 3 * i + k))) / 8
                rows += [i, i + offset]
                cols += [i + offset, i]
                vals += [value, value]
    matrix = sparse.coo_matrix((vals, (rows, cols)), shape=(ROWS, ROWS)).tocsr()
    matrix.sort_indices()
    b = numpy.array(
        [entry_value(u64_of(block_at(seed, index, 3 * ROWS + i))) for i in range(ROWS)],
        dtype="<f8",
    )
    return matrix, b


def main():
    here = pathlib.Path(__file__).resolve().parent
    for index in INDICES:
        matrix, b = unit_system(SEED, index)
        steps = []
        x, info = linalg.cg(
            matrix,
            b,
            x0=numpy.zeros(ROWS),
            rtol=0.0,
            atol=0.0,
            maxiter=ITERATIONS,
            callback=steps.append,
        )
        assert len(steps) == ITERATIONS, len(steps)
        residual = b - matrix @ x
        out = numpy.concatenate([x, residual]).astype("<f8")
        (here / f"unit-{index}.output.bin").write_bytes(out.tobytes())
        print(
            f"index {index}: info {info}, |r|/|b| = "
            f"{numpy.linalg.norm(residual) / numpy.linalg.norm(b):.3e}"
        )


if __name__ == "__main__":
    main()
