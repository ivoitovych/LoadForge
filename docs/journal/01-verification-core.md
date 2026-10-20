# Journal fragment 01 — verification core

**Written:** 2026-10-20 11:25 UTC
**Stream:** 01, verification core (`feature/verification-core`): the record codec
(`src/verification/record`), exact comparison and the input seal
(`src/verification/checksum`), golden vectors (`src/verification/golden`).

## Established by experiment

### xz reports CRC-64/XZ for a file's bytes, and is the reference tool for it

`xz --check=crc64` stores, per block, the CRC-64 of the uncompressed bytes, and
`xz --robot --list -vv FILE.xz` prints it as column 11 of the `block` line. For
`printf '123456789'` it printed `995dc9bbdf1939fa`, the catalogue's check value for
CRC-64/XZ, so the tool and the definition agree. Every file up to the 1,000,003-byte one
was a single block (xz 5.4.5's default block size is far larger), so the block's value is
the whole file's. An empty file has no block and no value: the empty input's CRC (0) is
pinned by the definition, not by the fixture. The fixture
(`tests/fixtures/xz/verification/checksum/crc64.txt`) carries the program that wrote the
Philox inputs, both commands, and the machine; every value matched the implementation
on the first run, slicing-by-8 included.

### The CRC-64/XZ polynomial has an even number of terms

`0x42F0E1EBA9EA3693` has 33 set bits, plus the x^64 term: 34. A polynomial with an even
number of terms is divisible by x + 1, so every odd number of bit errors is detected. The
claim is in `crc64.hpp` because it was counted, and the property tier checks it (with
every single-bit error in 1 KiB, every pair in a 64-byte line, and bursts up to 64 bits)
rather than trusting the count.

### Three more shapes of journal §1.13, met in one stream

Each was found by the coverage gate as branches on a line with no `if`, each in a cleanup
block for an exception no test can raise, and each was removed rather than excused:

- **A conditional expression whose arms build strings** (`has_value() ? to_string(v) :
  std::string("not recorded")`): 20 branches across the renderer. Rewritten as one
  statement per arm.
- **An aggregate built in one expression from a copied `Provenance` and two strings**
  (`return VerificationNotPerformed{subject.provenance, subject.check, "...", 0};`): 8
  branches. Built one member per statement in `not_performed()`.
- **A temporary aggregate passed to `push_back`**: 4 branches. `emplace_back()` first,
  members moved in after.

Chains of `operator+` building one string, and a single-allocation aggregate such as
`GoldenError{std::nullopt, "..."}`, measured clean; the trap is two or more allocating
constructions whose order of destruction depends on how far the expression got.

### At -O3, GCC cannot see that the last alternative of a variant is held

`return f(*std::get_if<VerificationNotPerformed>(&record));`, after `get_if` tests for
the other two alternatives, compiles clean in the debug and coverage presets and fails
the static preset (GCC 13, -O3, `-Wnull-dereference -Werror`): "potential null pointer
dereference", reported inside `basic_string::size()` inlined from the rule check. The
three dispatches on a record's kind are `std::visit` over overloads instead, which has no
pointer to be null. `record.hpp`'s own `provenance_of()` uses the same `get_if` shape and
did not warn in these units; a stream that inlines it where this did may meet it.

### A test binary that includes a contract suite must instantiate it, or say why not

Including `record_contract.hpp` for its sample records, without instantiating
`RecordCodecContract` in that binary, fails the run:
`GoogleTestVerification.UninstantiatedTypeParameterizedTestSuite<RecordCodecContract>`.
The fault-injection binary uses the samples only; it says so with
`GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(RecordCodecContract)`, as the golden unit
binary does for `RegistryContract`, which arrives with `workload_contract.hpp`.

### How much of a record a corrupted byte can change unseen

Over a stream of eleven encoded records, every bit of every byte flipped alone and one
random replacement per byte: 4,441 corruptions were refused, as one of the five faults a
corrupted record can produce (each seen), and 46,401 decoded -- every one to a valid
record whose own encoding is exactly the corrupted bytes. Over 5,000 random valid records
with one random byte overwritten: 1,556 refused, 3,444 decoded, all canonical. The
decoded ones are corruptions of value bytes -- a character of a check, a bit of a seed --
which carry no redundancy to break. That is the measured size of what the frame's CRC-32
(worker/ipc) exists for, and why this codec does not pretend to be it.

### Tooling this container lacked

`libclang-rt-18-dev` (journal §1.6) and ShellCheck were missing; an `apt-get install`
naming both failed as a whole because ShellCheck was not in the package lists until
`apt-get update`, so the sanitizer presets first failed at link time for want of
`libclang_rt.asan-x86_64.a`. Under `gtest_discover_tests`, building the coverage preset
runs each test binary to list its tests and so writes `.gcda` files; libgcov's
"overwriting an existing profile data with a different checksum" during a rebuild is
that, and the gate's order (delete `.gcda`, then `ctest`) is what makes it harmless.

## Decisions

### The record encoding is canonical: fixed widths, flags of 0 or 1, u64 lengths

Format version 1 is written down in `codec.hpp`. Every field has one width, every presence
flag is 0 or 1, and every length is a u64, so each record has exactly one encoding and
whatever `decode()` accepts, `encode()` gives back byte for byte. That is the strongest
reading of the contract's "this function's part is never to guess" available to a decoder
without a checksum of its own, and it is a property a test can assert over every
corruption, which the fault-injection tier does.

*Rejected:* variable-length integers (several encodings of one value unless policed, and
more branches to cover); a presence bitmap (unused bits to police); u32 lengths (the
contract says any record encodes, and a text over 4 GiB would not); a CRC inside the
record (the frame carries one, the contract assigns that job to it, and `DecodeFault` has
no member for it); and the reference's reading of a u32 from eight bytes with the top half
dropped, which decodes many byte strings to one record.

### decode() reports the first problem in byte order, and refuses a length before trusting it

A length above `kMaxTextBytes` or `kMaxWindowBytes` is `kInvalid` where it is read, so a
corrupted length never sizes an allocation (2^64 - 1 is tested). Rules over the whole
record are judged where the record ends, which is before any trailing bytes. A single
byte of a wrong version is `kVersion`, not `kTruncated`: an unknown version says nothing
about lengths.

### broken_rule() is a second reading of is_valid()

The validity rules are read again from the contract's words into a function that names
the rule broken, which `decode()` puts in its detail, and a test holds the two readings
to agree on every sample. A disagreement would be a contract question found by
execution, which is ADR 0003's point.

### Rendered text escapes everything that is not printable ASCII

A record's texts crossed a channel built to carry corruption. Control characters, DEL
and every byte above 0x7E print as `\xNN`, a backslash doubled, so a corrupted `check`
cannot move a terminal's cursor, and an escape cannot be forged. Non-ASCII UTF-8 is
escaped too: a C1 control arrives as UTF-8 bytes, and a report legible on every terminal
is worth more than a pretty `κ`.

### The input seal is CRC-64/XZ, slicing-by-8

For input that cannot be regenerated (verification.md §4a rule 2), the checksum's job is
to catch what memory does: a bit, a few bits in a word, a burst along a line, in buffers
of megabytes. A 64-bit CRC guarantees every single-bit error, every odd count, and every
burst up to 64 bits at any length, and misses random corruption one time in 2^64.
*Rejected:* the CRC-32 in `core` (its burst and Hamming-distance guarantees are for
messages far shorter than a working set, and 2^-32 is the frame's budget, not a
working set's); a 64-bit non-cryptographic hash such as xxHash64 or FNV-1a (no detection
guarantee for any error pattern, only a probability); and other CRC-64 polynomials (no
reference tool to take fixtures from, F21).

### A seal has no value that means "skip"

A corrupted checksum must raise a false alarm rather than hide a fault (§4a rule 2). A
`Seal` is a length and a CRC, both compared; no zero CRC reads as "not computed", no
length as "unknown". A seal never taken is `std::nullopt`, a different answer that
becomes `VerificationNotPerformed`. Empty bytes are "nothing compared" whatever the seal
says. A mismatch is an `ExactBitCorruption` with no difference whose check says "the
bytes or the checksum changed since; a checksum cannot say which bits" -- what was
observed, no more. Protecting the stored seal is control state (rule 3), stream 02's.

### Four comparison outcomes, and a length mismatch carries no bits

`compare_exact()` answers match, mismatch, length mismatch or nothing compared. Two
sides of different lengths are a wrong result with no byte-for-byte difference: comparing
a prefix would invent bits, so the record has no difference and its check names both
lengths. Nothing compared is `VerificationNotPerformed`. A `Subject` carries what the
bytes do not say; its check is bounded by `kMaxCheckBytes` (768), leaving room for the
longest detail this module appends, so every record made from a valid Subject is valid,
which the tests assert at the extremes.

### A golden key records more than the brief listed, and per-unit digests

The brief names (workload, version, ISA path, seed, units, digest). The key also records
the footprint, the configured decomposition and the decomposition it resolved to: the
contract makes a unit's output a function of its input and "the configuration", a
BitExactFixedDecomposition workload's bits depend on the decomposition, and a changed
default decomposition is a change the vector should name. Each unit's input and output
digest is recorded besides the overall one, so a report can say "unit 2's output changed
and its input did not" -- naming what changed, as the brief asks.

### Golden verdicts separate a declared change from a regression

The workload contract requires the version to move whenever the bits do. A moved version
is `kDeclaredChange` (the vector is stale; regenerate it deliberately); bits changed under
the same version are `kChanged` (a regression, or a forgotten version); vectors whose keys
differ are `kNotComparable` and nothing past the key is looked at; a vector of no unit is
`kNothingCompared`, never a pass. `GoldenReport` cannot become an `ErrorRecord`, which a
`static_assert` states, and the rendered report says that a mismatch is not a finding
about the hardware -- and, per verification.md §9, that the build, the platform or the
machine may have caused it.

### The golden module does no I/O

It formats and parses text and runs a workload; reading and writing files is its caller's
(the tests', and `tools/gen-golden/` when the integration stream adds it). So it has no
P2 path to inject, and no system call outside the seam. The committed fixture is
recorded by a disabled test run on purpose with two environment variables, the command
and the machine in its header: golden vectors are generated by the code they check by
definition (verification.md §4), so F21's "from the real tool" is met by recording how.

### Fixture locations

The brief gives this stream `tests/fixtures/golden/` and says committed fixtures go under
`tests/fixtures/<kind>/<module>/`. The golden vector is at
`tests/fixtures/golden/verification/golden/`, and the CRC-64 values, whose kind is xz, at
`tests/fixtures/xz/verification/checksum/` -- the reading of the brief's own convention
for a module whose fixtures are not golden vectors.

### Classes and tiers as the brief listed them

record: P1, P3, P4 at T1 and T5. checksum: P1, P3, P4 at T1, T4, T5 and T5b. golden: P1
and P3 at T1 and T6; no P4, because a golden mismatch is a regression report, not a
verification outcome, and the module makes no record. No module has P2 (no system call)
or P5 (the only threads are the tests' executors; `record_golden()` computes units in
order on the caller's executor).

## Contract questions

1. **`DecodeFault` has no member for a malformed field.** A presence flag of 2 is neither
   truncation nor trailing bytes; this codec reports it as `kInvalid`, reading "bytes
   describing a record this contract forbids" to include an encoding of no record at
   all. A `kMalformed` would separate a corrupt channel from a producer that sent a record
   the contract forbids, which are different bugs. Tested as `kInvalid`.
2. **Which fault, when several apply, is not fixed.** The reading: the first problem met
   in byte order; rules over the whole record before trailing bytes; a wrong version
   wins even over a one-byte input. The suite constrains only valid records' prefixes and
   suffixes, which every reading passes.
3. **"Total: any record encodes" and text length.** Taken literally: lengths are u64, so
   a record holding a text longer than 4 GiB still encodes, and decodes as `kInvalid`.
4. **Offsets of a kStored Difference are from a line-aligned origin**, which only the
   caller of an exact comparison can guarantee (`compare_exact` sees no address).
   Recorded as a precondition in `compare.hpp`; the runner (stream 02) places buffers on
   line boundaries, per `Provenance`.
5. **A record's check is bounded by `kMaxTextBytes`, and the contract does not say what a
   producer whose description is longer should do.** This stream bounds its own input
   (`kMaxCheckBytes`) rather than truncating silently; a contract rule (truncate with a
   marker, say) would let every producer behave alike.
6. **Golden vectors and units beyond `plan().units`.** A vector may record more units than
   the footprint holds at once; the reading is the contract's "every index is a unit"
   (the workload suite generates index 2^64 - 1), and recording runs units one at a time
   in buffers it owns.
7. **Disagreement with the brief, recorded rather than followed:** the brief's closing
   section says to open a pull request; the instructions this stream runs under say the
   manager opens it. No pull request was opened.
