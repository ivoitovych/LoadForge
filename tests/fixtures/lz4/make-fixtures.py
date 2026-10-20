# SPDX-License-Identifier: GPL-3.0-or-later
"""Make the LZ4 fixtures the compression oracle is tested against (F21).

Every block here is the reference tool's, never this project's:

  * "unit" and "block" fixtures are inputs this script writes, compressed by
    the lz4 command line into its legacy frame (lz4 -l), which holds raw LZ4
    blocks: four bytes of magic, then for each block its length (u32, little
    endian) and the block. A unit fixture's input is the size of a unit of
    the compression workload (64 KiB); a block fixture's is any other size.
  * "crafted" fixtures are blocks this script builds by hand to sit on a
    boundary of the block format (a length of exactly 15, an offset of 1, a
    match on the end-of-block margins). The block is the model; the expected
    decoding is the reference tool's: each is wrapped in a legacy frame,
    decoded by `lz4 -d`, and what the tool wrote is committed as its input.
    A block the tool refuses stops this script.

Run from anywhere: python3 tests/fixtures/lz4/make-fixtures.py. It rewrites
the fixtures and MANIFEST beside itself and prints the tool's version, which
README.md records with the machine and the time.
"""

from __future__ import annotations

import hashlib
import pathlib
import random
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
LEGACY_MAGIC = 0x184C2102
UNIT_BYTES = 64 << 10

WORDS_TEXT = (
    "the of and to in is was that for on with as by at from it an be this which or are "
    "have not had but were one all their there been has when who will more no if out so "
    "said what up its about into than them can only other new some could time these two "
    "may then do first any my now such like our over man me even most made after also did "
    "many before must through back years where much your way well down should because each "
    "just those people how too little state good very make world still own see men work "
    "long get here between both life being under never day same another know while last"
)
WORDS = WORDS_TEXT.split()


def text_input(seed: int, size: int) -> bytes:
    rng = random.Random(seed)
    out = bytearray()
    while len(out) < size:
        sentence = " ".join(rng.choice(WORDS) for _ in range(rng.randint(4, 14)))
        out += sentence.capitalize().encode() + rng.choice([b". ", b", ", b".\n", b"; "])
    return bytes(out[:size])


def records_input(seed: int, size: int) -> bytes:
    rng = random.Random(seed)
    out = bytearray()
    row = 0
    while len(out) < size:
        state = rng.choice(["OK", "OK", "OK", "WARN", "FAIL"])
        out += (
            f"id={row:06d},core={rng.randint(0, 63):02d},temp={rng.uniform(30, 95):.1f},"
            f"state={state}\n"
        ).encode()
        row += 1
    return bytes(out[:size])


def mixed_input(seed: int, size: int) -> bytes:
    """Fresh bytes, copies of earlier bytes from near and far, and short-period runs."""
    rng = random.Random(seed)
    out = bytearray(rng.randbytes(64))
    while len(out) < size:
        kind = rng.random()
        if kind < 0.35:
            out += rng.randbytes(rng.randint(8, 300))
        elif kind < 0.75:
            distance = rng.randint(1, len(out))
            length = rng.randint(4, 400)
            for _ in range(length):
                out.append(out[-distance])
        else:
            period = rng.randint(1, 4)
            length = rng.randint(20, 600)
            for _ in range(length):
                out.append(out[-period])
    return bytes(out[:size])


def runs_input(seed: int, size: int) -> bytes:
    """Mostly zero, a few bytes of noise every couple of kilobytes."""
    rng = random.Random(seed)
    out = bytearray(size)
    at = rng.randint(0, 2000)
    while at < size:
        for i in range(rng.randint(1, 6)):
            if at + i < size:
                out[at + i] = rng.randint(1, 255)
        at += rng.randint(1500, 2600)
    return bytes(out)


def random_input(seed: int, size: int) -> bytes:
    return random.Random(seed).randbytes(size)


def far_input(seed: int) -> bytes:
    """65535 fresh bytes and then their beginning again: a match at offset 65535."""
    rng = random.Random(seed)
    head = rng.randbytes(65535)
    return head + head[:4000] + rng.randbytes(500)


# (name, kind, input, lz4 level)
COMPRESSED = [
    ("text-l1", "unit", lambda: text_input(1, UNIT_BYTES), 1),
    ("text-l12", "unit", lambda: text_input(1, UNIT_BYTES), 12),
    ("records-l9", "unit", lambda: records_input(2, UNIT_BYTES), 9),
    ("mixed-l1", "unit", lambda: mixed_input(3, UNIT_BYTES), 1),
    ("mixed-l12", "unit", lambda: mixed_input(3, UNIT_BYTES), 12),
    ("runs-l1", "unit", lambda: runs_input(4, UNIT_BYTES), 1),
    ("random-l1", "unit", lambda: random_input(5, UNIT_BYTES), 1),
    ("far-l12", "block", lambda: far_input(6), 12),
    ("a13-l12", "block", lambda: b"a" * 13, 12),
    ("a64-l1", "block", lambda: b"a" * 64, 1),
    ("hello-l1", "block", lambda: b"hello", 1),
]


def token(literals: int, match: int | None) -> tuple[int, bytes, bytes]:
    """The token's two fields and the extension bytes of each, per the format."""

    def field(value: int) -> tuple[int, bytes]:
        if value < 15:
            return value, b""
        rest = value - 15
        return 15, b"\xff" * (rest // 255) + bytes([rest % 255])

    high, literal_extra = field(literals)
    low, match_extra = (0, b"") if match is None else field(match - 4)
    return (high << 4) | low, literal_extra, match_extra


def sequence(literals: bytes, offset: int | None = None, match: int | None = None) -> bytes:
    value, literal_extra, match_extra = token(len(literals), match)
    out = bytes([value]) + literal_extra + literals
    if offset is not None:
        out += struct.pack("<H", offset) + match_extra
    return out


LETTERS = bytes(range(ord("A"), ord("A") + 26)) * 40

# (name, block): each valid by the block format, each on one of its boundaries.
CRAFTED = [
    ("empty", sequence(b"")),
    ("literals-15", sequence(LETTERS[:15])),
    ("literals-270", sequence(LETTERS[:270])),
    ("literals-525", sequence(LETTERS[:525])),
    ("match-19", sequence(b"abcd", 4, 19) + sequence(LETTERS[:12])),
    ("match-274", sequence(b"abcd", 4, 274) + sequence(LETTERS[:12])),
    ("match-18", sequence(b"abcd", 4, 18) + sequence(LETTERS[:12])),
    (
        "no-literals",
        sequence(b"abcd", 4, 4) + sequence(b"", 8, 4) + sequence(LETTERS[:8]),
    ),
    ("offset-1", sequence(b"z", 1, 40) + sequence(LETTERS[:12])),
    ("overlap-3", sequence(b"xyz", 3, 30) + sequence(LETTERS[:12])),
    ("margins-exact", sequence(b"abcdefg", 7, 7) + sequence(b"VWXYZ")),
]


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(command, check=True, capture_output=True, **kwargs)


def legacy_blocks(frame: bytes) -> list[bytes]:
    if struct.unpack_from("<I", frame, 0)[0] != LEGACY_MAGIC:
        sys.exit("not a legacy frame")
    blocks, at = [], 4
    while at < len(frame):
        (length,) = struct.unpack_from("<I", frame, at)
        blocks.append(frame[at + 4 : at + 4 + length])
        at += 4 + length
    return blocks


def main() -> int:
    version = run(["lz4", "--version"]).stdout.decode().strip()
    manifest = [
        "# name kind sha256(input) sha256(frame) command",
        f"# made by {pathlib.Path(__file__).name} with: {version}",
    ]
    with tempfile.TemporaryDirectory() as scratch:
        work = pathlib.Path(scratch)
        for name, kind, make, level in COMPRESSED:
            data = make()
            (HERE / f"{name}.in").write_bytes(data)
            command = ["lz4", "-l", f"-{level}", "-f", f"{name}.in", f"{name}.lz4"]
            run(command, cwd=HERE)
            frame = (HERE / f"{name}.lz4").read_bytes()
            if len(legacy_blocks(frame)) != 1:
                sys.exit(f"{name}: expected exactly one block")
            manifest.append(_line(name, kind, " ".join(command)))
        for name, block in CRAFTED:
            frame = struct.pack("<II", LEGACY_MAGIC, len(block)) + block
            (HERE / f"{name}.lz4").write_bytes(frame)
            command = ["lz4", "-d", "-f", f"{name}.lz4", str(work / name)]
            run(command, cwd=HERE)
            (HERE / f"{name}.in").write_bytes((work / name).read_bytes())
            manifest.append(_line(name, "crafted", f"lz4 -d -f {name}.lz4 {name}.in"))
    (HERE / "MANIFEST").write_text("\n".join(manifest) + "\n")
    print(version)
    return 0


def _line(name: str, kind: str, command: str) -> str:
    digest_in = hashlib.sha256((HERE / f"{name}.in").read_bytes()).hexdigest()
    digest_frame = hashlib.sha256((HERE / f"{name}.lz4").read_bytes()).hexdigest()
    return f"{name} {kind} {digest_in} {digest_frame} {command}"


if __name__ == "__main__":
    sys.exit(main())
