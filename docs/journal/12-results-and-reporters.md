# Journal fragment 12 — results, the fingerprint, the JSON and CSV reporters

**Written:** 2026-10-20 13:40 UTC
**Change:** `src/controller/results/` (all but `result.hpp`), `src/reporters/json/`,
`src/reporters/csv/`, `docs/result-schema.md`, their tests and ledger fragments.

## Established by experiment

### Per-instantiation coverage of a template that is called with several types

A private `first_of<T>` template used for string, bool and double fields reported lines and
branches uncovered in the instantiations that the tests drove with fewer paths (the
single-candidate double field never evaluates the "second candidate" clause), although the
template as a whole was covered. gcovr keeps instantiations apart. Fixed by making the
reader one non-template function over `std::variant<bool, double, std::string>`, with tiny
non-template unwrapping helpers. Measured: 3 uncovered branches before, 0 after.

### A temporary handed to `push_back`, or a ternary of two `Node` factories, leaves untakeable branches

The same trap as JOURNAL §1.13, met in four places: `opt_text` (a ternary whose arms are
`Node::text(*value)` and `Node::null()`), a `BuildInfo{...}` aggregate of five strings,
`absence_of`, and `gaps.push_back(FingerprintGap{...})`. Each went to 5/7 or 6/10 branches with
every test passing. The cures that worked: `if`/early `return` instead of a ternary; assigning
members of a local instead of building an aggregate from temporaries; `emplace_back()` and
assigning in place. `noexcept` was not used to hide them: `bugprone-exception-escape` is on.

### A `switch` over every enumerator has one branch no test can take

`switch (node.kind())` with a case for each of nine kinds reported 9/10 (the implicit
no-match edge). Making the last kind the `default:` arm removes the edge and loses nothing.

### What a machine without a laptop's files gives

The fixture machine (an x86-64 VM, `tests/fixtures/fingerprint/vm-x86-64`) has `cpuinfo`,
`osrelease`, `smt/active`, THP and cgroup v1 files and none of power_supply, cpufreq,
thermal or platform_profile. Eight fingerprint fields are therefore absent on it, each with an
`absent` gap naming the path tried. That is the real-world case for a cloud VM, and the
fingerprint reports it rather than inventing values.

### `topology::describe(Availability)` returns phrases, not codes

"not offered by this kernel", "not permitted", "gone since it was last read". The contract
suite requires `describe(kDenied)` to appear in a rendering, so the words are written; a
stable machine-readable code (`absent`, `vanished`, `denied`, `malformed`, `unreadable`) is
written beside them (`availability_code()`), because a reader should not parse prose.

## Decisions

### One document tree, rendered by each reporter

Options: each reporter walks `RunResult` itself; or one tree (`to_document()`) that both
serialise. Chosen: the tree. Both formats then contain the same facts by construction, the
contract's "nothing summarised away" is checked once, and a reporter is a serialiser of about
150 lines. Cost: the tree is a second representation. It lives in `results/` because it
decides what a result is made of.

### Absence in JSON: `null` plus `<member>_unavailable`

Options: an object in place of the value (a reader discriminates on a key); a sentinel string;
null with the reason beside it (the brief's wording). Chosen the last. In CSV the equivalent
is an empty `value` with `status`, `reason`, `source_path`, `detail`, `remediation` in their own
columns. A NaN or infinite real is the text `nan`/`inf`/`-inf`, because null would say "not
recorded" about a value that was recorded.

### CSV is long-form: one row per leaf

A RunResult is a tree with lists of different shapes; a wide table would need several files or
ragged rows. One row per fact, with the dotted path in `field`, loads into any spreadsheet and
filters by prefix. The schema version is the first column of every row, and the first data row.

### JSON text: UTF-8 validated, invalid bytes become U+FFFD

Kernel strings are whatever the driver wrote. Escaping only quotes and controls could emit an
invalid document. Each offending byte becomes one `�`; nothing is dropped.

### The fingerprint's reads and their limits

The platform seam cannot list a directory, so the mains adapter is looked for under the names
laptops use (`AC`, `ACAD`, `ADP0`, `ADP1`, `AC0`) and the battery under `BAT0`, `BAT1`; cpufreq
is cpu0's; the thermal zone is zone 0. `cooling_profile` is zone 0's `policy` (the contract names
a "cooling profile" and no kernel file by that name exists; `platform_profile` is read separately).
Each is stated in `fingerprint.hpp` and `docs/result-schema.md`. A field with several candidate
files reports, when none answers, the first failure that is not a plain absence.
`git_commit` and the build flags are fixed at configure time (`src/controller/results/CMakeLists.txt`):
a commit made after configuring is not seen. A build outside git has no commit and says so.
`cgroup_memory_limit_bytes` reuses `resources::read_memory_budget`, so an unreadable
`/proc/meminfo` makes it a gap too; "no limit" is absent with no gap.

### "Two fingerprints on one machine are equal"

Equal in every field but `temperature_at_start_c`, which describes the moment: it is the only
field `is_moment_field()` names. `battery_state` can also change between two runs but is part of
what F12 says makes runs comparable, so it is not excused. The T8 test asserts that every
differing field is a moment field, over 30 repeated runs.

### Test fixtures for a laptop are built from documented formats, not captured

No machine with a battery, cpufreq or thermal zone was available to this stream. The tests build
those files from the formats in the kernel's ABI documents and say so in the test file's builder
note and in `tests/fixtures/fingerprint/README.md`. F21 asks for the real thing at least once;
that remains open for those eight fields until someone commits a capture from a laptop.

### Hardware-error P2 and P7 classes

Results owes P2 (every errno at every read, by `FakeSyscalls`, since the tests run as root) and
P7. Of P7's six states, "disappears mid-run" and "counter wraps" do not apply to a one-shot read
made before the run starts; the ledger note says so.

## Contract questions

1. **`ResultWriterContract` includes `VerdictContract`.** `writer_contract.hpp` includes
   `results_contract.hpp`, which registers `VerdictContract`. A binary that instantiates only
   the writer suite fails `GoogleTestVerification` unless it adds
   `GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(VerdictContract)`. Done in the reporters' test
   files and in the integration tests that include the shared fixtures.
2. **`Fingerprint` carries no reason for an absent field**, while the brief and the contract header
   say absence is "null with its reason beside it". The reader returns the reasons as `gaps`; the
   renderings carry an `absent_fields` list. A contract change adding the reasons to the result
   would let the reporters write them too.
3. **`WorkloadResult::logical_bytes` is a plain u64**, so `logical_bytes()` saturates at the
   ceiling on overflow rather than reporting absence. Chosen reading: a visibly absurd number
   beats a wrapped one.
4. **The Verdict rules' "oracle_blocks or integrity_checked at zero" make a bounded-contract
   workload that has no integrity bracket incomplete.** Followed as written.
5. `-fprofile-update=atomic` was not added: no code here runs on more than one thread.

## Open

- F21 for the laptop-only fingerprint fields (above).
- clang-tidy and the sanitizer and scalar and static builds are run by CI on both architectures;
  results of the local runs are in the stream report.
