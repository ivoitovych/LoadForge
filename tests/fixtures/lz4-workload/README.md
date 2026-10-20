# LZ4 fixtures for the compression workload

**Made:** 2026-10-20 10:56 UTC, on `Linux 6.18.44 x86_64` (Ubuntu 24.04 container),
with the real reference tool: `*** LZ4 command line interface 64-bits v1.9.4, by Yann
Collet ***`, Debian package `lz4 1.9.4-1build1.1`. Python 3.13.16 wrote the two seeded
random inputs.

These are the F21 evidence for the compression workload's tests: answers from the real
tool, never from a model of it. Two directions, two kinds of file.

## Blocks the tool wrote, for the test decoder (`*.fast.lz4`, `*.hc.lz4`)

Each is an LZ4 *legacy frame*: the magic `02 21 4C 18` (0x184C2102, little-endian), then
for each block its compressed length as a little-endian u32 and the block. A legacy
block holds up to 8 MiB, so every input here is at most one block; the empty input is
the magic alone, with no block at all. The test decoder
(`tests/oracle/workloads/compression/lz4_decoder.hpp`) must decode every block to the
`.bin` it came from, with no breach: that is the check that the decoder is a reading of
the format the reference implementation shares.

```sh
printf 'A'             > one-byte.bin
printf 'abcd'          > four-bytes.bin
printf 'abcabcabcabc'  > twelve-bytes.bin
printf 'aaaaaaaaaaaaa' > thirteen-bytes.bin
head -c 65536 /dev/zero > zeros-65536.bin
python3 -c "import random,sys; sys.stdout.buffer.write(random.Random(5).randbytes(65536))" > random-65536.bin
python3 -c "import random,sys; r=random.Random(7); a=r.randbytes(300); sys.stdout.buffer.write(a+a+r.randbytes(300))" > long-runs-900.bin
cp docs/verification.md text.bin      # as of commit 3ad4e59
: > empty.bin
for f in *.bin; do
  lz4 -q -l -1  -f "$f" "${f%.bin}.fast.lz4"
  lz4 -q -l -12 -f "$f" "${f%.bin}.hc.lz4"
done
```

What each input exercises in a decoder: fewer than five bytes (one literal-only
sequence); twelve bytes (too short for any match); thirteen bytes (the shortest block
with a match: one literal, a match of seven at offset 1 that overlaps itself, five last
literals); 64 KiB of zeros (offset 1 and match lengths coded in many 255 bytes);
incompressible data (a literal run of 65 536, coded in 257 extra bytes); runs of exactly
300 (a literal length of 15 + 255 + 30, and a match of the same length); and prose, where
the fast and the high-compression parsers choose different matches (`text.fast.lz4` and
`text.hc.lz4` differ; for every other input the two modes wrote the same bytes).

## Blocks the workload wrote, for the real tool (`*.ours.lz4`)

The same legacy frame around a block `compress_block()` wrote: the magic, the block's
length, the block. For the `unit-*` files the input is the workload's own generator,
`generate_input(seed, index)`: seed 1 index 0, seed 1 index 1, and seed and index both
2^64 - 1. Each frame was decoded by the real tool and compared with its input:

```sh
for f in *.ours.lz4; do lz4 -d -c "$f" | cmp - "${f%.ours.lz4}.bin" && echo "ok $f"; done
```

and all eleven printed `ok`. `tests/oracle/workloads/compression/` checks that the
compressor still writes exactly these bytes, and the generator exactly these inputs: if
either changes, the tool's acceptance above no longer describes the code, and the frames
must be made and decoded again (and `kVersion` raised, spec.hpp).

## Digests (sha256)

```text
e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  empty.bin
defd720fd1eb30161f269798941ef3ab079c370f55bff045c08903d44f9f31e0  empty.fast.lz4
defd720fd1eb30161f269798941ef3ab079c370f55bff045c08903d44f9f31e0  empty.hc.lz4
88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589  four-bytes.bin
bc5c0d8f014463042112199f5622d6b57fbd44b87c052bcf779eb63ff8f95af2  four-bytes.fast.lz4
bc5c0d8f014463042112199f5622d6b57fbd44b87c052bcf779eb63ff8f95af2  four-bytes.hc.lz4
bc5c0d8f014463042112199f5622d6b57fbd44b87c052bcf779eb63ff8f95af2  four-bytes.ours.lz4
89e2c589ce5da311e7f359cb11ec59a2ac4e91df2b44b53aa477a97c86d641fd  long-runs-900.bin
c94304a1feb0210f8a0e95b92eede3c99a5f14d2ce31dc2a9a6abd215cc61319  long-runs-900.fast.lz4
c94304a1feb0210f8a0e95b92eede3c99a5f14d2ce31dc2a9a6abd215cc61319  long-runs-900.hc.lz4
c94304a1feb0210f8a0e95b92eede3c99a5f14d2ce31dc2a9a6abd215cc61319  long-runs-900.ours.lz4
559aead08264d5795d3909718cdd05abd49572e84fe55590eef31a88a08fdffd  one-byte.bin
0c9f2880724fe456ec44c1ef41385fabe73731a4e7056f2e9b4699491c3b8b5a  one-byte.fast.lz4
0c9f2880724fe456ec44c1ef41385fabe73731a4e7056f2e9b4699491c3b8b5a  one-byte.hc.lz4
0c9f2880724fe456ec44c1ef41385fabe73731a4e7056f2e9b4699491c3b8b5a  one-byte.ours.lz4
7e03742be21474137c906cc24436250b052fdff78e27404bc93a535c7cb4aa52  random-65536.bin
3cd4ed3a9944a239fcc06fab97ba1c3ac780b67b608ef0708d4211073edb816b  random-65536.fast.lz4
3cd4ed3a9944a239fcc06fab97ba1c3ac780b67b608ef0708d4211073edb816b  random-65536.hc.lz4
3cd4ed3a9944a239fcc06fab97ba1c3ac780b67b608ef0708d4211073edb816b  random-65536.ours.lz4
4ee81d36c27eaf34b4f1c86bed7e9fb035cdcedaf03e49bf031bc70c6983b1bb  text.bin
08d97da7b3405d0583559cfd4f0e1a2124b84989f17fdedcd41e7b9570eba6de  text.fast.lz4
344a36d0fdc9d06b39b5fc51d6f62664325d43d2701d092542fc8e44a1f22c47  text.hc.lz4
a3c86b3703d6d60f3ffab31e6952cdc75e7270ae307b556484a1403d23ef5519  text.ours.lz4
a689d786e81340e45511dec6c7ab2d978434e5db123362450fe10cfac70d19d0  thirteen-bytes.bin
bf2018b3bfe250170a82e52239beba2662482e0853e87de859bcd7f6095d7de9  thirteen-bytes.fast.lz4
bf2018b3bfe250170a82e52239beba2662482e0853e87de859bcd7f6095d7de9  thirteen-bytes.hc.lz4
bf2018b3bfe250170a82e52239beba2662482e0853e87de859bcd7f6095d7de9  thirteen-bytes.ours.lz4
f70acb68693868ebf40255cce7d053743c346ae1e9dbb69e79e26fa8852d04ec  twelve-bytes.bin
1361d90cd8d7478e1a37ffe115bb6d97512eb597d19bd8f7be49fbe0e2f303bd  twelve-bytes.fast.lz4
1361d90cd8d7478e1a37ffe115bb6d97512eb597d19bd8f7be49fbe0e2f303bd  twelve-bytes.hc.lz4
1361d90cd8d7478e1a37ffe115bb6d97512eb597d19bd8f7be49fbe0e2f303bd  twelve-bytes.ours.lz4
04a9dccba6ccd01c8f1cf759d34ed1e0aa2ca0481587d40874cdb1253a170b2b  unit-seed1-index0.bin
945ea87981d29ab3e39e4a33f502a693d55caa8cfd0ffcc2e21c8e1dcc8c0875  unit-seed1-index0.ours.lz4
08eb2544db773289da2b450603e9c54f8d9149fbf68307e55bc1b6c1fe523880  unit-seed1-index1.bin
242bab7f1f4ac1296ac3ee97c075dc705aa2ca451dd1ebbd1cf7c27bb8c4fc2f  unit-seed1-index1.ours.lz4
3b95a7cb3a1572b43970a375650734cd24f819a0b413f30bff3693c72ad2d11d  unit-seedmax-indexmax.bin
3e34be18ef6dad221d1dba08b5c4a0adfd877799142f6791494a774a9e511ab7  unit-seedmax-indexmax.ours.lz4
de2f256064a0af797747c2b97505dc0b9f3df0de4f489eac731c23ae9ca9cc31  zeros-65536.bin
d76bf9e3dd83ef55f7e3ed44c2bbcf056c76ba6aed9eb961589a286e07059e07  zeros-65536.fast.lz4
d76bf9e3dd83ef55f7e3ed44c2bbcf056c76ba6aed9eb961589a286e07059e07  zeros-65536.hc.lz4
d76bf9e3dd83ef55f7e3ed44c2bbcf056c76ba6aed9eb961589a286e07059e07  zeros-65536.ours.lz4
```
