# Journal fragment 05 — workload: compression

**Written:** 2026-10-20 11:32 UTC
**Change:** the compression workload (F14), `src/workloads/compression/`: the LZ4-block
compressor, the input generator, the factory, and its T1, T3, T4, T5, T5b and T6 tests.

## Established by experiment

### Threads in an instrumented build make gcov's counts negative, and gcovr then refuses

The first coverage run of this module failed with no test failing: gcovr stopped on
`lz4_block.cpp:76 Got negative hit value in: branch 1 taken -50933 (fallthrough)` and
wrote no report. `--coverage` counters are plain increments; when the T6 tests compress
units on 2 to 7 threads at once, increments of one arc's counter are lost, and gcov,
which derives some counts from others, computes a negative one. It is not `ctest -j`:
with one test process running only the threaded tests, three runs out of three failed
the same way (-18362, -11284, -15957 on the same branch). Building the library and the
two suites that run tasks on several threads with `-fprofile-update=atomic` (their own
fragments, under `LOADFORGE_COVERAGE`) removed it; the gate then measured 100% line,
branch and function over all 39 source files. Any stream whose tests run instrumented
code on several threads meets this — the executor stream first (contract question 4).

### What the real lz4 1.9.4 tool writes, and how this compressor compares

- A legacy frame (`lz4 -l`) of an empty file is the four magic bytes and no block at
  all: the tool never writes the format's one-zero-byte empty block, so that block is
  tested from the format document's sentence instead.
- `-1` and `-12` wrote the same bytes for every fixture input except prose: single bytes,
  short inputs, zeros, incompressible data and two 300-byte runs leave a high-compression
  parser nothing to choose differently.
- This compressor's block is byte-for-byte the tool's `-1` block for seven of the eight
  fixture inputs (all but the prose), and the tool decoded all eleven of its frames —
  the eight inputs and three generated units — back to their inputs
  (`tests/fixtures/lz4-workload/README.md`).

### Where the generator lands

Over 256 units (seeds 0, 1, 42 and 2^64 − 1, 64 indices each) the compressed block was
0.568 to 0.607 of the input: inside the fifth to four fifths `spec.hpp` asks for, with
about 0.2 of margin to the upper edge. The T4 test asserts the specification's range and
also that every unit lands between 0.5 and 0.67, so a change that moves the generator
towards an edge fails before it crosses one.

### How a corrupted output is caught, flip by flip

Every single-bit flip of one unit's output (seed 1, index 0: the header, the
38 296-byte block and 64 padding bytes, 306 944 flips) was judged by the test decoder:
278 238 as decoded bytes that differ, 17 490 offsets before the start, 6 988 decodes
past the capacity, 3 609 decoded sizes, 519 non-zero padding, 32 magics, 22 truncated
literals, 16 lengths over the bound, 14 zero offsets, 5 truncated offsets, 1 block ending
with a match — and **10 agreements**. Each of the ten is a different valid block of the
same input — an offset moved to an identical earlier copy is the obvious way to get
one — which the specification counts as a right answer. So "every flip is
caught" is false for LZ4 and the T5 sweep does not assert it: it asserts that a flip is
either a mismatch or decodes to exactly the input.

### The bound's slack for a literal-only block is 14 bytes, not 15

The first property test claimed a block with no match leaves at least 15 of
`compress_bound()`'s 16 bytes unused. It leaves 14 at n = 15 (the length needs its zero
byte while n / 255 is still 0), at 269 and at 1000: wherever the length's extra bytes and
n / 255 round to the same count. The test now asserts the derived 14.

### Two test-harness facts

- `workload_contract.hpp` registers both `WorkloadContract` and `RegistryContract`, and a
  test binary that instantiates only the first fails GoogleTest's own check,
  `GoogleTestVerification.UninstantiatedTypeParameterizedTestSuite<RegistryContract>`.
  `GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(RegistryContract)` in the namespace of
  the suite is the fix (contract question 2).
- clang-tidy's `bugprone-easily-swappable-parameters` fires inside `executor.hpp`, on
  `for_each_index()`'s lambda, as soon as `for_each_index()` is instantiated in a file
  under `src/`; the header is frozen, so no `NOLINT` can be placed there (contract
  question 1).

## Decisions

### The compressor: greedy, one candidate per hash, nothing that depends on more than the next bytes

*Options:* the reference implementation's fast mode with its acceleration (it skips
ahead faster on incompressible data); its high-compression chain search; an optimal
parse; this one — greedy, a single-entry table of 4 096 positions, a candidate taken only
if its four bytes really match, extended forwards only. *Chosen:* the last. Any valid
block is a right answer (`spec.hpp`), so the choice is about load and about how much
there is to get wrong. The acceleration and backward extension change which positions
are tried in ways that depend on earlier misses, which makes the end-of-block limits
harder to keep by construction, and buys speed this workload does not need to prove a
CPU. The end-of-block rules are kept by the loop bounds rather than checked afterwards,
and the bound is argued in `lz4_block.hpp` and tested on the inputs closest to it.

- **Positions are 32-bit, the table 16 KiB of scratch.** The reference implementation
  uses 16-bit positions below 64 KiB; with 32-bit ones the function is correct for any
  input below 4 GiB, which the tests use (the 65 535-byte offset is taken and 65 536 is
  not, at inputs above 64 KiB).
- **The hash is taken from little-endian loads** (`spec.hpp`'s `load_le32`) so the
  candidates, and so the block, are the same on any byte order; the table is accessed
  through `memcpy`, so scratch of any alignment serves.
- **The table is cleared on every call.** What scratch held must not reach the output;
  a cleared table offers position 0 everywhere, which the candidate test excludes.

### The generator: Philox bytes, overwritten by copy runs drawn from far up the same stream

*Options:* a restricted alphabet (LZ4 has no entropy coder, so it barely compresses);
a Markov source (compressibility hard to bound); Philox bytes with LZ77-style copies laid
over them. *Chosen:* the last, because the proportion of copied bytes sets the
compressibility directly and both literals and matches are exercised by construction.
The draws come from positions from 2^62 up in the unit's own stream, so the input is
still a function of (seed, index) and stream `index` alone, and the bytes and the draws
never overlap. Literal runs of 1 to 40 bytes, copies of 4 to 64 from up to 32 768 back:
measured to land at 0.57–0.61. Every run starts with a literal, so a copy always has
something behind it.

### The plan's two rates

`logical_bytes` is the input read once plus the output written once: the traffic the
algorithm defines. The table's traffic depends on the data and is the implementation's.
`operations` is input bytes consumed, the unit every codec's rate is quoted in.

### `compute()` is one task on the executor, in the slot's piece of scratch

*Options:* compress on the calling thread with slot 0's scratch; one task on the
executor. *Chosen:* one task, using the slot the executor gives it, so the T6 tests run
the unit on threads and slots the workload did not choose (`LastSlotExecutor` proves the
other slots' scratch is untouched). The task is a function of this module's own rather
than `for_each_index()`, because of the clang-tidy finding above; it is exactly what
`for_each_index()` would have generated.

### The refusals, and their order

ISA path first (one scalar kernel; integer code the compiler vectorises as it likes,
so no path would change its bits), then decomposition (0 and 1 accepted, 1 reported),
then the footprint. A configuration wrong in two ways is refused for the first, and a
test pins the order.

### Path classes: P4 kept with its meaning stated, P2 and P5 absent

The module classifies nothing itself; the oracle that does is stream 03's. P4 is kept
because the workload owes T5 and T5b, and what they exercise is the set of verdicts its
output can receive under `spec.hpp`, judged by its own test decoder. P2 is absent: no
system call, and the only allocation is `create()`'s, where `std::bad_alloc` propagates
as it does from every factory in the tree. P5 is absent: a unit is one task, the
workload holds no mutable state, and verification concurrent with compute is the
runner's. `IMPLEMENTATION.md` §3 lists P5 for this row; the brief says it does not
arise, and the code agrees.

### The test decoder is strict, and returns its own judgement rather than `oracle.hpp`'s

*Strict:* the format allows a decoder to reject a block that breaks the end-of-block
rules, so this one does, with eleven breaches each placed at a byte, so that a
compressor that broke a rule is caught here rather than by some decoder in the field.
*Its own judgement type:* `oracle.hpp`'s `Verdict` and `is_valid()` are inline code in a
`src/` header, measured as `src/` once a test calls them, and §1.22 is the warning about
that; and the oracle is another stream's to implement. The judgement mirrors the
specification's list ("WHAT THE ORACLE ANSWERS") with an enum per check and a domain for
each offset, which is what the T5 tests assert.

### Fixtures in both directions, and the code held to them

The tool's blocks test the decoder; this compressor's blocks were given to the tool. The
second set is only evidence while the compressor still writes those bytes, so T3 and T6
assert it does, and that the generator still writes their inputs; the fixtures' README
says what to redo if either changes. The inputs are committed rather than regenerated in
the test, so the generator's bytes are pinned on every architecture CI runs.

### The workload's own executors, run against the executor contract

Three mocks (`tests/unit/workloads/compression/executors.hpp`): serial; *n* threads
taking indices from a shared counter, so the slot an index gets changes between runs;
and one that claims *n* threads and runs everything in the last slot. All three pass
`ExecutorContract`, which is the cross-execution ADR 0003 asks of a consumer.

## Contract questions

1. **`executor.hpp`: `for_each_index()` cannot be used from `src/` under the project's
   clang-tidy.** Its lambda's two adjacent `std::size_t` parameters trigger
   `bugprone-easily-swappable-parameters` at a line in the frozen header once it is
   instantiated in a `src/` file. *Reading taken:* call `Executor::run()` with a task
   function of the workload's own, with the `NOLINT` at its definition. *Proposed fix:*
   a `NOLINTNEXTLINE(bugprone-easily-swappable-parameters)` on the lambda in
   `executor.hpp`, so every workload can use the helper the contract offers.
2. **`workload_contract.hpp` registers two suites in every file that includes it.** A
   workload's test binary instantiates `WorkloadContract` only, and GoogleTest then fails
   on the uninstantiated `RegistryContract`. *Reading taken:*
   `GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(RegistryContract)` in the workload's
   test. *Proposed fix:* the macro for both suites in the header, or one header per suite;
   every workload stream will meet this.
3. **`spec.hpp`'s `kVersion` is the workload's `Descriptor::version`, and it is frozen.**
   `workload.hpp` says the version is raised whenever the bits a configuration produces
   change; `spec.hpp` fixes it at 1 in a header no stream may edit. *Reading taken:* the
   descriptor reports `kVersion`, and a change of the compressor's or the generator's
   bits is a contract change (the fixtures' README says so). *Question:* whether the
   workload's version should live with the workload, with the specification's own
   version separate — a compressor may change its matches without the format changing.
4. **`cmake/Coverage.cmake` has no `-fprofile-update=atomic`.** Code run on several
   threads in an instrumented test makes gcovr refuse the report (above). *Reading
   taken:* this module's fragments add the flag to their own targets. *Proposed fix:*
   the flag in `loadforge_enable_coverage()`, for every target, before the executor
   stream's tests meet it.
5. **"A block that decodes to anything but kInputBytes bytes".** A decoder given the
   input's size as its capacity stops at it, so a block that would decode to more is a
   format breach (past the capacity) where the specification names a size. *Reading
   taken:* both are an ExactMismatch with no difference; the test decoder reports more
   as a block breach and fewer as the decoded size. The oracle stream may read it the
   same way or decode without a capacity; either agrees with the specification.
6. **The fixture directory.** The brief names `tests/fixtures/lz4-workload/`, and the
   general rule in the same brief says `tests/fixtures/<kind>/<module>/`. *Reading
   taken:* the directory the brief gives this stream to own.
