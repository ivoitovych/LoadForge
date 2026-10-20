#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Golden batch sums for the Monte Carlo workload, from an implementation that
shares no code with it: Python's own IEEE 754 doubles (multiplication,
subtraction and math.sqrt are correctly rounded) and a Philox4x32-10 written
here and checked against Random123's known-answer vectors before it is used.

Usage: make_golden.py [output]   (prints to standard output by default)

For seed SEED and unit UNIT it writes, for 1, 2 and 4 accumulators (the scalar,
NEON and AVX2 orders) and every chunk length, the 64 batch sums as the hex of
their IEEE bits: 'lanes chunk batch hex'. The order is spec.hpp's: per chunk,
sample k to accumulator (k - chunk start) mod lanes, accumulators added
0, 1, ..., lanes - 1; chunk sums added in increasing chunk from chunk 0's.
"""

import math
import pathlib
import platform
import struct
import sys

SEED = 0xC0FFEE
UNIT = 3
LANES = (1, 2, 4)
CHUNKS = (64, 256, 1024, 4096, 16384)
BATCHES = 64
SAMPLES = 1 << 14
MASK = 0xFFFFFFFF
KAT = pathlib.Path(__file__).resolve().parents[2] / "rng" / "philox4x32-10.kat"


def philox(counter, key):
    c0, c1, c2, c3 = counter
    k0, k1 = key
    for rnd in range(10):
        if rnd:
            k0 = (k0 + 0x9E3779B9) & MASK
            k1 = (k1 + 0xBB67AE85) & MASK
        p0 = 0xD2511F53 * c0
        p1 = 0xCD9E8D57 * c2
        c0, c1, c2, c3 = (
            (p1 >> 32) ^ c1 ^ k0,
            p1 & MASK,
            (p0 >> 32) ^ c3 ^ k1,
            p0 & MASK,
        )
    return c0, c1, c2, c3


def block_at(seed, stream, position):
    counter = (position & MASK, position >> 32, stream & MASK, stream >> 32)
    return philox(counter, (seed & MASK, seed >> 32))


def check_known_answers():
    count = 0
    for line in KAT.read_text().splitlines():
        if not line.startswith("philox4x32 10 "):
            continue
        words = [int(w, 16) for w in line.split()[2:]]
        assert philox(tuple(words[0:4]), tuple(words[4:6])) == tuple(words[6:10]), line
        count += 1
    assert count > 0, "no known-answer vectors read"


def samples(key):
    out = []
    for j in range(SAMPLES // 2):
        w = block_at(key, 0, j)
        for bits in (w[0] | (w[1] << 32), w[2] | (w[3] << 32)):
            x = (bits >> 11) * 2.0**-53
            out.append(math.sqrt(1.0 - x * x))
    return out


def batch_sum(values, lanes, chunk):
    total = None
    for first in range(0, SAMPLES, chunk):
        acc = [0.0] * lanes
        for offset in range(chunk):
            acc[offset % lanes] += values[first + offset]
        s = acc[0]
        for lane in range(1, lanes):
            s += acc[lane]
        total = s if total is None else total + s
    return total


def main():
    check_known_answers()
    rows = []
    for batch in range(BATCHES):
        w = block_at(SEED, UNIT, batch)
        values = samples(w[0] | (w[1] << 32))
        for lanes in LANES:
            for chunk in CHUNKS:
                bits = struct.unpack("<Q", struct.pack("<d", batch_sum(values, lanes, chunk)))[0]
                rows.append((lanes, chunk, batch, f"{bits:016x}"))
    rows.sort()
    header = [
        "# Monte Carlo golden batch sums (workload version 1): regression tripwire, not an oracle.",
        f"# seed {SEED:#x} unit {UNIT}; columns: lanes chunk batch ieee-bits-hex.",
        "# Made by: python3 tests/fixtures/workloads/monte_carlo/make_golden.py "
        "> tests/fixtures/workloads/monte_carlo/golden.txt",
        f"# On: {platform.platform()}, Python {platform.python_version()}; Philox checked "
        "against tests/fixtures/rng/philox4x32-10.kat first.",
    ]
    text = "\n".join(header + [" ".join(map(str, r)) for r in rows]) + "\n"
    if len(sys.argv) > 1:
        pathlib.Path(sys.argv[1]).write_text(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
