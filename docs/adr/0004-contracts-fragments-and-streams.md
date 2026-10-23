# ADR 0004 — The contract set, file fragments, and the stream plan

**Status:** accepted
**Date:** 2026-10-20 05:17 UTC

## Context

ADR 0003 decided that components share contracts and nothing else, and that the work
after M1 would run as parallel streams, one component each. Two things had to exist
before a single stream could start, because the streams' agents work without being able
to ask anyone anything:

1. **every boundary between the remaining components written as a contract**, precise
   enough that two careful readers build the same thing, with a provider-side suite that
   any implementation — real, or a consumer's mock — can be run against;
2. **the streams themselves**, with disjoint ownership — including of the files that
   every stream would otherwise edit (`CMakeLists.txt`, `tests/CMakeLists.txt`,
   `tests/obligations.toml`, the journal, the roadmap).

This ADR records the contract set, the mechanism that gives each stream its own fragment
of every shared file, and the stream plan, with the options rejected for each.

## Decision 1 — The contract set

Twenty-one headers, each where `docs/PLAN.md` §6 puts its module, each marked
`CONTRACT (ADR 0004)` and frozen:

| Header | What it fixes | Suites |
|---|---|---|
| `src/primitives/rng/philox.hpp` | Philox4x32-10 and the (seed, stream, position) addressing | definition tests |
| `src/primitives/simd/dispatch.hpp` | ISA paths, sets, `select_path()`, `IsaProbe`, `KernelTable` | SelectPathContract, IsaProbeContract |
| `src/platform/fpenv/fpenv.hpp` | the floating-point environment | FpControlContract, FpCheckContract |
| `src/worker/executor.hpp` | a worker's threads, by index never by thread | ExecutorContract |
| `src/verification/record.hpp` | the three error records, provenance, classification, the encoding | RecordCodecContract |
| `src/workloads/workload.hpp` | a workload as a family of independent units; the registry | WorkloadContract, RegistryContract |
| `src/verification/oracle.hpp` | the independent oracle and its verdicts | OracleContract, OracleRegistryContract |
| `src/verification/runner.hpp` | slices inside the input-integrity bracket | SliceRunnerContract |
| `src/verification/isolation.hpp` | the re-run ladder and its table | IsolatorContract |
| `src/telemetry/sample.hpp` | metrics, readings, absence with a remedy | definition tests |
| `src/telemetry/source.hpp` | a source; the capability report | SourceContract |
| `src/telemetry/timeline.hpp` | the monotonic timeline and phase marks | TimelineContract |
| `src/telemetry/sampler.hpp` | reading sources onto the timeline in rounds | SamplerContract |
| `src/statistics/signature.hpp` | the Load Signature | SummarizerContract |
| `src/controller/results/result.hpp` | the result schema, the fingerprint, the verdict | VerdictContract |
| `src/reporters/writer.hpp` | writing a result in one format | ResultWriterContract |
| `src/controller/journal/journal.hpp` | the crash journal | JournalContract |
| `src/worker/control.hpp` | controller-to-worker control for M5 | ControlCodecContract |
| `src/explore/trial.hpp` | one candidate as a controlled trial, for M6 | TrialRunnerContract |
| `src/workloads/compression/spec.hpp` | the compression specification | definition tests |
| `src/workloads/dense_linear/spec.hpp` | the dense-linear specification | definition tests |

Each suite is a GoogleTest **typed** suite in `tests/contracts/<area>/`, taking a
`Subject` type with static factory functions, and each runs in this change against a
trivial reference implementation, so every suite is proved runnable and passable before
anyone relies on it. A provider instantiates the suite against its implementation; a
consumer instantiates it against its own mock — that instantiation is ADR 0003's
cross-execution, and the ledger gate now checks it exists (Decision 2).

The choices inside the contracts that a later reader might undo without knowing why:

- **A workload is a family of independent units**, unit *i*'s input a pure function of
  `(seed, i)` and its output a pure function of that input. Not "run for N seconds".
  Truth is then regenerable for the F18 bracket and for fault isolation's re-runs, and
  determinism becomes a property of one unit. The runner owns memory, slicing and
  verification, so a workload allocates nothing, starts no thread and makes no system
  call. *Rejected:* a workload that runs itself and verifies itself — it would put the
  F18 bracket, the subtlest logic in the tool, into eleven implementations.
- **The bracket lives in one runner, and checks happen in the slice that computed the
  unit.** No result outlives its slice unverified, which keeps verification.md §4a's
  temporal invariant without holding results across slices. Benchmark mode keeps
  expensive checks out of its measurements by a lower duty cycle, reported with the
  signature — F17's own fallback. *Rejected:* deferred verification between benchmark
  windows; it may come back as a contract change when a measurement asks for it.
- **An oracle judges the output against the input it is given**, never a regenerated
  one, so that a wrong input and a wrong output stay distinct findings (§4a rule 4); the
  runner's bracket judges the input.
- **Oracles are written by a different stream from the workload they check**, from a
  specification header both are written from — the strongest form of verification.md
  §3's independence this project can arrange. The compression specification uses the
  published LZ4 block format, so the oracle is an independent decoder with a reference
  tool to make fixtures from (F21); the dense-linear specification fixes C = AB at
  n = 256 and the form of its tolerance.
- **An error record is made in two stages.** The worker knows eleven of verification.md
  §8's fourteen common fields; isolation result, confidence and the telemetry at the
  time exist only in the controller, which attaches them beside the record in
  `results::Finding`. *Rejected:* fourteen nullable fields on the wire, three of which
  the sender could only ever leave empty.
- **An exact corruption may have no bits.** A compressed stream that does not decode is
  a wrong result with nothing to classify, and a decoded difference is labelled as
  derived rather than stored, because one flipped compressed bit changes many decoded
  ones. Absence of bit evidence is represented, never filled with zeros.
- **Fault isolation takes a `WrongResult`**, a type that cannot hold a
  `VerificationNotPerformed`, so §7's rule is kept by the compiler.
- **The sampler is driven in rounds by the caller**, with the caller's clock reading, so
  the controller's single loop samples on its tick and there is no sampler thread to be
  wrong about (PLAN §4.5's preference). A provider that needs a thread owes the P5
  argument.
- **The control pipe becomes framed at M5, on both sides at once**; the M1 stop byte is
  retired in the same change rather than spoken alongside frames.

**Three contracts carry their definition** rather than declare it: Philox (one correct
reading, pinned by Random123's published vectors, needed by every stream from its first
day), the bit classification and the isolation ladder's table (each *is* the
specification, and every side must compute it identically), plus small pure helpers —
names, `counter_delta()`, `is_valid()` rules. Each definition is tested in
`tests/contracts/` at 100%. Everything with a design choice in it — codecs, the runner,
the probe, the verdict — is declared, and its provider is a stream.

**Value types compare with a `noexcept` `operator==` over `std::tie`**, never
`= default`: a defaulted comparison in a header reads as uncovered once a second
translation unit uses it (journal §1.22), and one that can throw carries a cleanup block
no test can enter (§1.13). In a frozen header either would be a coverage failure no
stream could fix. Each header also compiles alone under the product's warnings
(`tests/contracts/headers/`), and CI runs clang-tidy over those units, for the same
reason.

**Gaps recorded rather than hidden.** `Rerunner` has no suite: re-running a unit in a real
worker is only meaningfully tested against real processes, which the integration stream's
T7 tests do. Golden-vector regeneration (`tools/gen-golden/`) is assigned to the
integration stream, because the vectors need the workloads.

### Changing a contract

A contract changes only in a change of its own: header, suite and reference together,
every in-tree instantiation of the suite still passing, the reason in the header and in
the journal. Between waves by default; within a wave only when a stream is blocked, in
which case every stream that builds against it is told. A stream that finds an
ambiguity records it under "Contract questions" in its journal fragment, takes the most
defensible reading, tests it in its own tree, and continues.

## Decision 2 — Every shared file has per-stream fragments

| Shared file | Fragment | Mechanism |
|---|---|---|
| `CMakeLists.txt` | `src/<module>/CMakeLists.txt` | `loadforge_add_fragments()` (`cmake/Components.cmake`) globs every `CMakeLists.txt` under `src/` |
| `tests/CMakeLists.txt` | `tests/<tier>/<module>/CMakeLists.txt` | the same, under `tests/`, less fixtures, the gates' tests and the two directories already added by name |
| `tests/obligations.toml` | `tests/obligations.d/<module path below src>.toml` | `tools/check-test-obligations.py` reads both; a fragment declares exactly the module it is named after |
| `docs/JOURNAL.md` | `docs/journal/<NN>-<slug>.md` | folded in by the manager at the end of a wave |
| `docs/ROADMAP.md` | the `State` line of `docs/streams/<NN>-<slug>.md` | the roadmap links to the briefs; the manager updates it per wave |
| `docs/adr/` | `docs/adr/<NNNN>-<slug>.md` | marked accepted by the manager when it lands (see the amendment at the end) |

`loadforge_component()` and `loadforge_test_suite()` apply the warnings, the
floating-point contract, the sanitizers and coverage instrumentation, so a fragment
cannot build a target the gates do not see. The ledger gate refuses a fragment that
declares another module or two, a module declared twice, and any file in the fragment
directory that is not a `.toml` fragment. It also gained `provides` and `consumes`: each
name must be a suite declared under `tests/contracts/`, and the module's own tests —
`tests/<tier>/<module path below src>/`, less those of a more specific module, never
`tests/contracts/` — must instantiate it. Seventeen tooling tests cover the ledger's
additions, each refusal among them, and four more the glob: every fragment added, the
exclusions honoured, a fragment that adds another refused at configure time.

*Rejected:*

- **A pre-populated list of fragments in the two CMake files.** A list is a second record
  of what exists; a fragment missing from it is silently not built and its tests silently
  not run, which is F20's failure shape. A glob builds what is there.
  `CONFIGURE_DEPENDS` picks a new fragment up without a manual reconfigure, and a
  fragment that also adds another is refused by CMake at configure time.
- **One ledger with a section per stream.** Adjacent edits to one file still conflict, and
  nothing would stop two streams declaring one module.
- **One fragment per stream rather than per module.** A module outlives the stream that
  built it; naming the fragment after the module makes ownership checkable by the gate.
- **Moving every existing row into fragments now.** The rows of M1's modules stay in
  `tests/obligations.toml` and move to fragments the first time their owner edits them,
  which keeps this change from conflicting with work landing on `main` meanwhile.
- **Value-parameterised suites.** Their test bodies must be compiled once into a library
  every user links; typed suites live entirely in a header and are instantiated in
  as many translation units as want them.

## Decision 3 — The stream plan

Thirty-four streams in six waves, briefed one file each in `docs/streams/`, listed with
their dependencies in `docs/ROADMAP.md`. Wave 1 is ten streams that depend on the
contracts alone — the M2a and M2b components, with the minimal telemetry M2a needs.
Wave 2 integrates them and builds the M3 components, the crash journal, and the first
workloads of M4. Waves 3 to 6 follow the milestones' own order: M3's integration with
more workloads, M5, M6, M7.

The rules of the plan, and why:

- **One integration stream per wave.** Integration edits the M1 modules (`main`,
  `controller`, `worker`) and the link lines of the top-level `CMakeLists.txt`; two in
  one wave would conflict, so integration is sequential by design and everything else is
  parallel.
- **M4 starts in wave 2**, after M2b's workload has set the pattern (IMPLEMENTATION §5's
  constraint), and **a workload's specification header lands before its wave**, so its
  workload stream and its oracle stream start from the same text at the same time.
- **Shared files outside the fragments have one named owner per wave**: the coverage
  completeness check and workflow belong to the dense-linear stream in wave 1 (an ARM-only
  kernel is never compiled on x86-64, and must be measured on ARM64); the system-call seam
  to the performance-counter stream in wave 2; the mutation workflow to the integration
  streams.
- **Two tiers of stream**, named by what they touch rather than by who builds them:
  Tier A, a bounded component against a written contract (one source, one reporter, one
  workload of the M4 set); Tier B, anything touching concurrency, supervision or
  verification oracles, and the first two workloads, whose shape every later one copies.

*Rejected:* streams per milestone (M2a, M2b and M3 are each too large for one stream and
too interdependent inside to split by milestone alone); more than ten streams in wave 1
(the brief's ceiling, and the integration that follows grows with every stream it joins);
wave 1 including the results and reporters (M3 work, while isolation is needed for M2a's
exit and took the slot).

## Consequences

- No stream edits a file another stream in its wave edits, and the gates say so where a
  mistake is possible: an unbuilt fragment does not exist, an undeclared module fails the
  ledger, a declared contract without its instantiation fails the ledger.
- The contracts will be wrong in places. The cost of finding that out is designed to be
  small: a contract question in a journal fragment, a contract change between waves, and
  suites that two independent readers have both run.
- The manager has more to do between waves: fold journals, number ADRs, land
  specifications and contract changes, reconcile IMPLEMENTATION §3 and the roadmap.
- The project still needs an outside reviewer; the contracts are the cheapest place for
  one to start.

> **Amended, 2026-10-23 00:15 UTC (golden vectors):** `tools/gen-golden/` was not built and
> will not be. Golden vectors are regenerated by the `make_golden.py` script beside each
> fixture's `golden.txt` under `tests/fixtures/`. The assignment of regeneration to the
> integration stream stands.
>
> **Amended, 2026-10-23 00:15 UTC (numbering of ADRs):** a decision record is written
> directly as `docs/adr/<NNNN>-<slug>.md` with the next free number and Status `proposed`;
> the manager marks it accepted when it lands. The table row above, which sent proposals to
> a `proposed/` subdirectory to be numbered later, is replaced by this; ADRs 0005 to 0010 were
> numbered from that subdirectory on this date.
