#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the graph oracle's fixtures: scipy's distances, and PageRank's bits.

Run from the repository root:

    python3 tests/fixtures/verification/oracle/graph/generate_fixtures.py

For each (seed, index) below this builds the unit's graph the way the
specification says (32768 vertices, eight out-edges each, the head of edge e the
Philox draw e modulo 32768, the source the draw 262144 modulo 32768) and writes
two kinds of file next to this script:

  unit-<index>.distances.bin   scipy.sparse.csgraph.dijkstra with unit weights
                               from the source: 32768 little-endian u32, the
                               unreachable ones 0xFFFFFFFF. The real tool.
  unit-<index>.ranks-a<a>.bin  the PageRank after 16 iterations for a = 1, 2, 4
                               and 8 accumulators: 32768 little-endian doubles.

Distances are a property of the graph and have a real tool. PageRank's BITS do
not: the specification pins an order of additions that no library follows, so
the ranks are computed here, in Python floats (IEEE 754 binary64, one add or
multiply at a time, no fused operations), from the specification's text. That
model is checked against the real tool where the tool can speak: a power
iteration by scipy's sparse matrix-vector product, which sums in another order,
must agree with it to 1e-13 relative, so a mistake in the model's graph, damping
or iteration count shows. The C++ tests then decide whether the oracle, written
a second time in another language, produces the same bytes.

The committed files were produced with Python 3.13.16, numpy 2.5.3 and scipy
1.18.1 on an x86-64 Linux machine (Intel Xeon 2.10GHz, kernel 6.18.44, glibc
2.39). The inputs are not committed: the tests regenerate them with
src/primitives/rng/philox.hpp.
"""

import pathlib
import sys

import numpy
from scipy import sparse
from scipy.sparse import csgraph

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from philox_reference import block_at, u64_of  # noqa: E402

VERTICES = 32768
OUT_DEGREE = 8
EDGES = VERTICES * OUT_DEGREE
ITERATIONS = 16
UNREACHABLE = 0xFFFFFFFF
DAMPING = 0.85
TELEPORT = (1.0 - DAMPING) / VERTICES
SEED = 0x4C6F616446697263
INDICES = (9,)
ACCUMULATORS = (1, 2, 4, 8)


def unit_graph(seed, index):
    heads = [u64_of(block_at(seed, index, e)) % VERTICES for e in range(EDGES)]
    source = u64_of(block_at(seed, index, EDGES)) % VERTICES
    return heads, source


def distances(heads, source):
    tails = numpy.repeat(numpy.arange(VERTICES), OUT_DEGREE)
    graph = sparse.csr_matrix(
        (numpy.ones(EDGES), (tails, numpy.array(heads))), shape=(VERTICES, VERTICES)
    )
    found = csgraph.dijkstra(graph, directed=True, indices=source, unweighted=True)
    out = numpy.full(VERTICES, UNREACHABLE, dtype="<u4")
    reachable = numpy.isfinite(found)
    out[reachable] = found[reachable].astype("<u4")
    return out


def pagerank_bits(heads, accumulators):
    """The specification's PageRank, term by term, in its order."""
    slot = []
    seen = [0] * VERTICES
    for head in heads:
        slot.append(seen[head] % accumulators)
        seen[head] += 1
    rank = [1.0 / VERTICES] * VERTICES
    for _ in range(ITERATIONS):
        parts = [0.0] * (VERTICES * accumulators)
        for e in range(EDGES):
            parts[heads[e] * accumulators + slot[e]] += rank[e // OUT_DEGREE] * 0.125
        following = []
        for v in range(VERTICES):
            total = parts[v * accumulators]
            for j in range(1, accumulators):
                total = total + parts[v * accumulators + j]
            damped = DAMPING * total
            following.append(TELEPORT + damped)
        rank = following
    return numpy.array(rank, dtype="<f8")


def power_iteration(heads):
    """PageRank by scipy's sparse product: the same fixed point, another order."""
    tails = numpy.repeat(numpy.arange(VERTICES), OUT_DEGREE)
    transition = sparse.csr_matrix(
        (numpy.full(EDGES, 0.125), (numpy.array(heads), tails)), shape=(VERTICES, VERTICES)
    )
    rank = numpy.full(VERTICES, 1.0 / VERTICES)
    for _ in range(ITERATIONS):
        rank = TELEPORT + DAMPING * (transition @ rank)
    return rank


def main():
    here = pathlib.Path(__file__).resolve().parent
    for index in INDICES:
        heads, source = unit_graph(SEED, index)
        (here / f"unit-{index}.distances.bin").write_bytes(distances(heads, source).tobytes())
        reference = power_iteration(heads)
        for accumulators in ACCUMULATORS:
            ranks = pagerank_bits(heads, accumulators)
            assert numpy.allclose(ranks, reference, rtol=1e-13, atol=0.0), accumulators
            (here / f"unit-{index}.ranks-a{accumulators}.bin").write_bytes(ranks.tobytes())


if __name__ == "__main__":
    main()
