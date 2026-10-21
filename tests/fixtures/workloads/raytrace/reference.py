#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""An independent model of the ray tracing workload, written from the text of
src/workloads/raytrace/spec.hpp and src/primitives/rng/philox.hpp and from
nothing else: pure Python, binary64 floats (which are IEEE 754 doubles, with no
fused multiply-add), math.sqrt (correctly rounded), no C++ and no hierarchy.

It traces every primitive for every pixel, in index order, as the
specification defines the image, and prints one FNV-1a 64 digest of the whole
output per (seed, unit, tile edge). golden.txt is its output; the C++ tests
(tests/determinism/workloads/raytrace) compare the workload's bytes to those
digests, so a typo shared by spec.hpp and the workload still shows.

Usage: reference.py > golden.txt
"""

import math
import struct
import sys

INF = math.inf
M0, M1 = 0xD2511F53, 0xCD9E8D57
W0, W1 = 0x9E3779B9, 0xBB67AE85
MASK32 = 0xFFFFFFFF

SPHERES, TRIANGLES = 128, 128
WIDTH = HEIGHT = 64
NO_HIT = 0xFFFFFFFF
MAX_TILES = 64
CASES = [(1, 0), (0xC0FFEE, 7), (0xFFFFFFFFFFFFFFFF, (1 << 40) + 5)]
EDGES = [8, 16, 32]


def philox_u64(seed, stream, position):
    c = [position & MASK32, position >> 32, stream & MASK32, stream >> 32]
    k = [seed & MASK32, seed >> 32]
    for rnd in range(10):
        if rnd > 0:
            k = [(k[0] + W0) & MASK32, (k[1] + W1) & MASK32]
        p0 = M0 * c[0]
        p1 = M1 * c[2]
        c = [(p1 >> 32) ^ c[1] ^ k[0], p1 & MASK32, (p0 >> 32) ^ c[3] ^ k[1], p0 & MASK32]
    return c[0] | (c[1] << 32)


def lattice(bits):
    return (bits >> 44) * (1.0 / 262144.0)


def scene(seed, unit):
    def draw(e):
        return philox_u64(seed, unit, e)

    spheres = []
    for i in range(SPHERES):
        cx = lattice(draw(4 * i)) - 2.0
        cy = lattice(draw(4 * i + 1)) - 2.0
        cz = lattice(draw(4 * i + 2)) - 4.0
        r = 0.0625 + (draw(4 * i + 3) >> 44) * (1.0 / 8388608.0)
        spheres.append(((cx, cy, cz), r))
    triangles = []
    for j in range(TRIANGLES):
        first = 4 * SPHERES + 12 * j
        base = (
            lattice(draw(first)) - 2.0,
            lattice(draw(first + 1)) - 2.0,
            lattice(draw(first + 2)) - 4.0,
        )
        vertices = []
        for k in range(3):
            vertices.append(
                tuple(
                    base[c] + ((draw(first + 3 + 3 * k + c) >> 44) * (1.0 / 2097152.0) - 0.25)
                    for c in range(3)
                )
            )
        triangles.append(tuple(vertices))
    return spheres, triangles


def dot(a, b):
    return (a[0] * b[0] + a[1] * b[1]) + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def ray_sphere(origin, d, sphere):
    centre, radius = sphere
    oc = sub(origin, centre)
    a = dot(d, d)
    hb = dot(oc, d)
    cc = dot(oc, oc) - radius * radius
    disc = hb * hb - a * cc
    if not disc >= 0.0:
        return INF
    t = (-hb - math.sqrt(disc)) / a
    return t if t > 0.0 else INF


def ray_triangle(origin, d, tri):
    e1 = sub(tri[1], tri[0])
    e2 = sub(tri[2], tri[0])
    p = cross(d, e2)
    det = dot(e1, p)
    if not abs(det) > 0.0:
        return INF
    inv = 1.0 / det
    s = sub(origin, tri[0])
    u = dot(s, p) * inv
    if not u >= 0.0 or not u <= 1.0:
        return INF
    q = cross(s, e1)
    v = dot(d, q) * inv
    if not v >= 0.0 or not u + v <= 1.0:
        return INF
    t = dot(e2, q) * inv
    return t if t > 0.0 else INF


def trace(spheres, triangles):
    eye = (0.0, 0.0, 4.0)
    records = []
    for y in range(HEIGHT):
        for x in range(WIDTH):
            d = ((x * 2.0 - 63.0) * (1.0 / 256.0), (63.0 - y * 2.0) * (1.0 / 256.0), -1.0)
            best_t, best = INF, NO_HIT
            for i, sphere in enumerate(spheres):
                t = ray_sphere(eye, d, sphere)
                if t < best_t:
                    best_t, best = t, i
            for j, tri in enumerate(triangles):
                t = ray_triangle(eye, d, tri)
                if t < best_t:
                    best_t, best = t, SPHERES + j
            records.append((best_t, best))
    return records


def output_bytes(records, edge):
    out = bytearray()
    for t, primitive in records:
        out += struct.pack("<dII", t, primitive, 0)
    per_row = WIDTH // edge
    count = per_row * (HEIGHT // edge)
    for tile in range(MAX_TILES):
        total = 0.0
        if tile < count:
            fx, fy = (tile % per_row) * edge, (tile // per_row) * edge
            for y in range(fy, fy + edge):
                for x in range(fx, fx + edge):
                    t, primitive = records[y * WIDTH + x]
                    if primitive != NO_HIT:
                        total = total + t
        out += struct.pack("<d", total)
    return bytes(out)


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for byte in data:
        h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    print("# seed unit edge fnv1a64-of-the-whole-output")
    for seed, unit in CASES:
        spheres, triangles = scene(seed, unit)
        records = trace(spheres, triangles)
        for edge in EDGES:
            digest = fnv1a64(output_bytes(records, edge))
            print(f"{seed} {unit} {edge} {digest:016x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
