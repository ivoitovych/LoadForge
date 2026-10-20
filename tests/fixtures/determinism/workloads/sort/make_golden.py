#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes golden.txt: digests of the sort workload's input and output for fixed
(seed, unit) pairs, produced without any code from the workload.

The generator follows src/workloads/sort/spec.hpp and Random123's Philox4x32-10
(the C++ side checks that function against Random123's published vectors, and
this one is checked against the same three below before anything is written).
The sort is Python's `sorted`, which is stable, applied to the pair
(key, input position): the specification's order.

    python3 make_golden.py > golden.txt
"""

import platform
import sys

KEYS = 1 << 17
M0, M1 = 0xD2511F53, 0xCD9E8D57
W0, W1 = 0x9E3779B9, 0xBB67AE85
MASK = 0xFFFFFFFF

CASES = [
    (0, 0),
    (1, 0),
    (1, 1),
    (0xDEADBEEFCAFEF00D, 5),
    (0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF),
]

# Random123 tests/kat_vectors, philox4x32 10 (counter, key, expected).
KNOWN = [
    ((0, 0, 0, 0), (0, 0), (0x6627E8D5, 0xE169C58D, 0xBC57AC4C, 0x9B00DBD8)),
    (
        (MASK, MASK, MASK, MASK),
        (MASK, MASK),
        (0x408F276D, 0x41C83B0E, 0xA20BC7C6, 0x6D5451FD),
    ),
    (
        (0x243F6A88, 0x85A308D3, 0x13198A2E, 0x03707344),
        (0xA4093822, 0x299F31D0),
        (0xD16CFE09, 0x94FDCCEB, 0x5001E420, 0x24126EA1),
    ),
]


def philox(counter, key):
    c0, c1, c2, c3 = counter
    k0, k1 = key
    for _ in range(10):
        p0 = M0 * c0
        p1 = M1 * c2
        c0, c1, c2, c3 = (p1 >> 32) ^ c1 ^ k0, p1 & MASK, (p0 >> 32) ^ c3 ^ k1, p0 & MASK
        k0 = (k0 + W0) & MASK
        k1 = (k1 + W1) & MASK
    return (c0, c1, c2, c3)


def key_at(seed, unit, position):
    counter = (position & MASK, position >> 32, unit & MASK, unit >> 32)
    block = philox(counter, (seed & MASK, seed >> 32))
    return block[0] | (block[1] << 32)


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for byte in data:
        h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    for counter, key, expected in KNOWN:
        # The reference computes round 0 with the unbumped key; the loop above
        # bumps after use, which is the same thing.
        if philox(counter, key) != expected:
            sys.exit("philox does not reproduce Random123's known answers")
    print("# Digests of the sort workload's input and output (FNV-1a, 64 bit), made")
    print("# without any code from the workload: tests/fixtures/determinism/workloads/sort/")
    print("# make_golden.py, a Philox4x32-10 written from Random123 and Python's stable")
    print("# sorted() on (key, input position).")
    print(f"# Python {platform.python_version()} on {platform.machine()}, {platform.system()}.")
    print("# This pins the bits (determinism.md §2); it is a tripwire, not the oracle.")
    print("#")
    print("# Columns: seed unit input_fnv1a64 output_fnv1a64")
    for seed, unit in CASES:
        keys = [key_at(seed, unit, e) for e in range(KEYS)]
        order = sorted(range(KEYS), key=lambda e, keys=keys: (keys[e], e))
        data_in = b"".join(k.to_bytes(8, "little") for k in keys)
        data_out = b"".join(keys[e].to_bytes(8, "little") for e in order) + b"".join(
            e.to_bytes(4, "little") for e in order
        )
        print(f"{seed:016x} {unit:016x} {fnv1a64(data_in):016x} {fnv1a64(data_out):016x}")


main()
