# Journal fragment 03 — The compression oracle

**Written:** 2026-10-20 11:10 UTC
**Stream:** 03, `feature/oracle-compression` (`docs/streams/03-oracle-compression.md`)
**Module:** `src/verification/oracle/compression` — a strict LZ4 block decoder and the
`Oracle` for the compression workload, written from `src/workloads/compression/spec.hpp`
and the LZ4 block-format document (`lz4/doc/lz4_Block_format.md`, last revised
2022-07-31, fetched from the v1.9.4 tag), never from the compressor.

## Established by experiment

### The reference decoder is more lenient than the document allows a decoder to be

Hand-built blocks, each wrapped in a legacy frame (magic `0x184C2102`, a `u32` length,
the block) and given to `lz4 -d` (lz4 1.9.4, Ubuntu 24.04, x86_64), 2026-10-20 10:53 UTC:

| Block (hex) | What it breaks | `lz4 -d` |
|---|---|---|
| `40 61626364 0000 c0 41..4c` | offset 0 | **accepted**, wrote `abcd` then four zero bytes |
| `40 61626364 0500 c0 41..4c` | offset reaching before the first byte | refused (error 54) |
| `72 61..67 0700 50 56..5a` | a match starting 11 bytes before the end of 18 | **accepted** |
| `74 61..67 0700 40 56..59` | a match covering the fifth byte from the end | refused |
| `51 68656c6c6f` | the last token has a match length of 1 | **accepted**, wrote `hello` |
| `80 61..68 0800` | the block ends after a match | refused |

The document says of its end-of-block rules that "a conformant decoder is allowed to
reject the block"; lz4's own decoder, given an output buffer with room (the legacy frame
gives it 8 MiB), rejects some and not others, and decodes offset 0 as zeros — the
"clears the match segment with 0 bytes" the document describes. So agreement with
`lz4 -d` is not the oracle's standard for a malformed block; the document is.

### Every block the reference compressor wrote keeps the stricter rules

All eleven compressed fixtures (levels 1, 9 and 12; 13 bytes to 70035) decode under the
strict rules to their inputs, which includes: the last token's match field is 0 in every
one, and every match starts at least 12 bytes and ends at least 5 bytes before the end of
the decoded data. `a13-l12` meets both margins exactly (`13 61 0100 50 6161616161`: a
match of 7 at offset 1 from byte 1 to byte 8 of 13); `a64-l1` ends its match exactly 5
before the end; `far-l12` holds a match at offset 65535. The T3 suite asserts each.

### The legacy frame, measured

`lz4 -l` of an empty file writes the four bytes of magic and no block. Of 65536 random
bytes it writes one block of 65794 bytes — one token, 257 extension bytes, 65536 literals
— under `compress_bound(65536)` = 65809. A 64 KiB input is always one block (the legacy
frame's block is 8 MiB).

### A single-bit flip can make another valid encoding of the same input

The first exhaustive flip run stopped at `text-l1`, byte 1055 of the output, bit 3: the
offset 112 of a match of 8 became 120, and the input holds `between ` at both distances.
The flipped block is a valid block that decodes to the input — by the specification a
right answer ("any valid block that decodes to the input is a right answer"), with
nothing for any oracle to catch. Of the 4728 single-bit flips of `runs-l1`'s header and
block, 2916 are malformations, 1295 decode to different bytes, and 517 are such
re-encodings: the input is mostly zeros, so many offsets have an identical copy one bit
away. The test proves each re-encoding independently of the decoder (below).

### What a check costs, and why not every flip of every unit

In the debug build a check of a unit that decodes in full takes about 1.5 ms (decoding and
comparing 64 KiB through bounds-checked spans at `-O0`). Flipping every bit of all seven
units is 1.47 million checks, over half an hour in debug and longer under the sanitizers.
Every bit of `runs-l1` (4728 flips, about 5 s in debug) and a fixed sample of the other
six (every bit of the header, of the first and last 32 bytes of the block and of every
1021st byte between: 704 to 1088 flips each, about 5 s together) run on every push.

### Two allocations in one expression, again (§1.13)

`return NotChecked{std::string(kWhat), "no scratch …", 0};` left two branches of
`oracle.cpp` uncovered (99.9%): the temporary built from `kWhat` is alive while the
second string is built from the literal, and the cleanup that destroys it carries the
small-string branch. The reason built in its own statement first, as the size refusal
above it already was, made it 100%.

### Tooling details

- clang-tidy 18 refuses `std::unique_ptr<std::byte[]>` (`modernize-avoid-c-arrays`,
  `cppcoreguidelines-avoid-c-arrays`). The scratch is a `std::array<std::byte, 65536>`
  owned by a `std::unique_ptr` instead — a fixed size the specification gives anyway.
- `oracle_contract.hpp` registers both oracle suites. A test binary that includes it for
  `OracleCase` and instantiates only `OracleContract`, or neither, fails GoogleTest's
  `UninstantiatedTypeParameterizedTestSuite` check; each such binary says
  `GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST` for the suites it does not run.
- GCC 13's `-Wdangling-reference` caught `const auto& m = std::get<ExactMismatch>(check(…))`
  in the first draft of the fault tests: the verdict is a temporary. The tests hold the
  verdict first.

## Decisions

1. **Strict where the document allows strictness.** The decoder refuses every
   end-of-block breach, offset 0, and a last token with a match length, though
   `lz4 -d` accepts each (above). A lenient oracle would agree with a compressor that
   writes what the document says cannot be there, and agreement is the one answer an
   oracle must not give falsely. *Rejected:* matching the reference decoder's behaviour —
   it is an implementation's choice, the document is the specification.
2. **"End of block" in the 12-byte rule is the end of the decoded data**, and both
   margins are applied to every match. The document's rules 2 and 3 count decoded bytes
   ("the last 5 bytes of input are always literals"), the reference compressor's limits
   are on its input, and the a13 fixture meets both exactly in decoded bytes. A match
   that keeps them keeps them as the last match too, and every earlier match starts
   earlier, so checking each match is the same rule, checked where it breaks.
3. **The last token's match field must be 0.** "The last sequence contains only
   literals"; a non-zero field would describe a match the sequence does not have. The
   reference compressor writes 0. It is also what makes a flip of that nibble a caught
   corruption rather than a silent one.
4. **"The byte" a malformation names is the byte of the unit's output holding the field
   that broke the rule**: the token for a token rule, the first literal for the literals,
   the first byte of the offset for any rule about a match, and the block's end — the byte
   that is not there — for a block that ended inside a field. The check also says how many
   bytes had been decoded. Header rules name byte 0 (magic) and 4 (length); padding names
   the first non-zero byte.
5. **The order of judgement:** sizes (NotChecked), magic, `L <= compress_bound`, the zero
   padding, scratch, the block rule by rule, then the comparison. The header and padding
   come before scratch, so what is known to be wrong there is reported even without
   memory; the block's rules are judged while decoding, which needs the scratch, so a
   malformed block with no scratch is NotChecked.
6. **The scratch is a seam of the module's own (`ScratchSource`), and its failure is
   P2.** The default is `new (std::nothrow)`; the T5b suite injects a source that has
   nothing to give. The brief's table listed P1, P3, P4; P2 is added in the ledger with
   this reason. *Rejected:* a stack buffer of 64 KiB (an executor thread's stack size is
   not this module's to assume) and a `thread_local` buffer (64 KiB of static TLS in every
   thread of the process, to remove a failure the contract names).
7. **The decoder takes any decoded size; the oracle fixes 65536.** Offsets of 65535 and
   blocks shorter than any match cannot occur in a 64 KiB unit (the largest offset a valid
   unit can hold is 65524), but they are boundaries of the format, and the decoder is
   tested on them directly, on hand-built blocks and on the tool's.
8. **`create(config, plan)` is this module's factory for the registry**, which is the
   integration stream's. It refuses, as `kConfiguration`, a plan whose sizes are not a
   compression unit's, a resolved decomposition other than 1, and a configured one other
   than 0 (the default) or 1. The workload name is `compression` (`workload.hpp` uses it
   as its example, and stream 11's `--test compression`).
9. **Fixtures:** inputs written by a script from fixed seeds, compressed by `lz4 -l`;
   hand-built boundary blocks whose expected decodings are what `lz4 -d` wrote, so even
   the hand-built ones are judged by the real tool (F21). Inputs are committed beside the
   blocks (912 KiB in all) rather than regenerated in the test, so a test reads exactly
   what the tool was given.
10. **Flips are said to be one of three things**: a malformation, a derived difference,
    or another valid encoding of the input — the last proved by the test's own walk of
    the block's fields (not the decoder under test): the flipped bit is in an offset, the
    new offset is in range, and the bytes it copies equal the bytes the old one copied.
    The counts are pinned, so a rule the decoder stopped refusing would show as flips
    moving between columns.
11. **The decoder reports a `Summary`** (sequences, matches, the largest offset, the
    longest literal run and match, overlapping matches) so the tests can show a fixture
    reaches the boundary it is kept for, rather than assume it.
12. **Three test executables, one per tier**, through `loadforge_test_suite()`, as the
    brief and ADR 0004 say. The weekly mutation gate runs `loadforge_unit_tests` only, so
    these suites are outside it until it runs every executable (stream 11's to change).

## Contract questions

1. **`spec.hpp`: "the last match starts at least twelve bytes before the end" — the end
   of what?** Read as the end of the decoded data (Decision 2). A compressor that read it
   as the end of the compressed block would write blocks this oracle refuses.
2. **`spec.hpp` is silent on the last token's match field.** Read as "must be 0"
   (Decision 3). If the compression stream's compressor writes anything else there, the
   two readings meet here, which is what they are meant to do; the specification should
   say which.
3. **The brief's exit criterion 3 says every single-bit flip of a fixture is caught.**
   Some flips make another valid encoding of the same input, which the specification
   counts as right; no oracle can catch them, and one that did would refuse right
   answers. Read as: every flip is caught or is proved to be another encoding, and the
   test says which of the three each is (517 of 4728 for `runs-l1`). The criterion
   should say so.
4. **`oracle.hpp` lists "a scratch allocation that failed" as NotChecked.** A malformed
   *block* cannot be judged without decoding it, so with no scratch it is NotChecked, not
   an ExactMismatch; a malformed header or padding is still reported. Read as intended;
   noted because a reader could expect every malformation to be found without memory.
5. **`oracle_contract.hpp` registers `OracleRegistryContract` beside `OracleContract`**,
   so every oracle stream's binaries must allow the registry suite uninstantiated. A
   separate header for each suite, or `OracleCase` outside both, would spare each oracle
   stream the line.
6. **"Any other configured value is refused" (`spec.hpp`, on the decomposition)** — other
   than what? `WorkloadConfig` says 0 asks for the default; read as: 0 and 1 are
   accepted, anything else refused (Decision 8).
