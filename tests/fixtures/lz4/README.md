# LZ4 fixtures for the compression oracle

**Made:** 2026-10-20 10:52 UTC
**Tool:** `lz4` command line v1.9.4, 64-bit (Ubuntu package `lz4 1.9.4-1build1.1`,
`liblz4-1 1.9.4-1build1.1`)
**Machine:** x86_64, Linux 6.18.44, Ubuntu 24.04.5 LTS
**Script:** `make-fixtures.py` in this directory (`python3 tests/fixtures/lz4/make-fixtures.py`)

Every block here was made or decoded by the reference tool (F21); none was made by
LoadForge's compressor, which the oracle these fixtures test is written to check
(`src/verification/oracle/compression/`, verification.md §3).

## Files

Each fixture is two files, `<name>.in` and `<name>.lz4`, listed in `MANIFEST` with the
SHA-256 of both and the exact command that produced the one the tool made.

`<name>.lz4` is an lz4 **legacy frame** (`lz4 -l`): the magic `0x184C2102`, then one
little-endian `u32` block length and one raw LZ4 block. Every fixture holds exactly one
block; the tests refuse a frame that does not.

| Kind | `.in` is | `.lz4` is | Names |
|---|---|---|---|
| `unit` | an input of 65536 bytes, the size of a compression unit (`workloads/compression/spec.hpp`) | `lz4 -l -<level>` of it | `text-l1`, `text-l12`, `records-l9`, `mixed-l1`, `mixed-l12`, `runs-l1`, `random-l1` |
| `block` | an input of another size | `lz4 -l -<level>` of it | `far-l12` (70035 bytes; a match at offset 65535), `a13-l12` (a run whose one match meets both end-of-block margins exactly), `a64-l1` (one match, overlapping at offset 1, ending exactly five bytes before the end), `hello-l1` (shorter than any match) |
| `crafted` | what `lz4 -d` decoded the block to | a block the script wrote by hand, on one boundary of the block format | `empty`, `literals-15`, `literals-270`, `literals-525`, `match-18`, `match-19`, `match-274`, `no-literals`, `offset-1`, `overlap-3`, `margins-exact` |

The inputs of `unit` and `block` fixtures are written by the script from fixed seeds of
Python's `random.Random`: words, CSV-like records, a mix of fresh bytes with near and far
copies and short-period runs, a mostly-zero buffer, random bytes. Their compressed sizes
run from 0.9% (`runs-l1`) to 100.4% (`random-l1`) of the input, so blocks with a single
literal run, with long matches, and with every length and offset in between are all
present.

A `crafted` block is a model of the format, so its expected decoding is not ours: the
script wraps it in a legacy frame and runs `lz4 -d`, and what the tool wrote is the
fixture's `.in`. A block the tool refused would stop the script.

## Regenerating

The script is deterministic given the tool: run it, and `git diff` shows nothing unless
the tool's output changed. A change of tool version that changes a block is a fixture
change, reviewed as one, with this file's header updated to the new version and machine.
