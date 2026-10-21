# SPDX-License-Identifier: GPL-3.0-or-later
"""Philox4x32-10 and the project's (seed, stream, position) addressing, in Python.

The fixture generators of the five wave-3 oracles need a unit's input to feed
the real tools (numpy, scipy). The input is a pure function of (seed, index)
through src/primitives/rng/philox.hpp; this is that function written a second
time, in another language, and checked against Random123's published vectors
(tests/fixtures/rng/philox4x32-10.kat) every time it is imported. The C++
tests then regenerate each unit's input with the contract's own header and
compare the oracle's verdict on the tool's output, so a disagreement between
this file and the header would fail there.
"""

import pathlib

M0 = 0xD2511F53
M1 = 0xCD9E8D57
W0 = 0x9E3779B9
W1 = 0xBB67AE85
MASK = 0xFFFFFFFF

KAT = pathlib.Path(__file__).resolve().parents[2] / "rng" / "philox4x32-10.kat"


def philox4x32_10(counter, key):
    c0, c1, c2, c3 = counter
    k0, k1 = key
    for rnd in range(10):
        if rnd > 0:
            k0 = (k0 + W0) & MASK
            k1 = (k1 + W1) & MASK
        p0 = M0 * c0
        p1 = M1 * c2
        c0, c1, c2, c3 = (
            ((p1 >> 32) ^ c1 ^ k0) & MASK,
            p1 & MASK,
            ((p0 >> 32) ^ c3 ^ k1) & MASK,
            p0 & MASK,
        )
    return (c0, c1, c2, c3)


def block_at(seed, stream, position):
    counter = (position & MASK, position >> 32, stream & MASK, stream >> 32)
    return philox4x32_10(counter, (seed & MASK, seed >> 32))


def u64_of(block):
    return block[0] | (block[1] << 32)


def check_known_answers():
    checked = 0
    for line in KAT.read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        fields = line.split()
        assert fields[0] == "philox4x32" and fields[1] == "10", line
        words = [int(f, 16) for f in fields[2:]]
        assert philox4x32_10(tuple(words[0:4]), tuple(words[4:6])) == tuple(words[6:10]), line
        checked += 1
    assert checked >= 3


check_known_answers()
