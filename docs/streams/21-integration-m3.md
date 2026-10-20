# Stream 21 — Integration: full telemetry and the Load Signature

**State:** not started  
**Written:** 2026-10-20 05:17 UTC  
**Wave:** 3  
**Tier:** Tier B — concurrency, supervision, verification oracles, or a pattern every later stream copies  
**Milestone:** M3 (exit)  
**Branch:** `feature/integration-m3`  
**Depends on:** streams 11-16 merged

This brief is self-contained: it is everything you need to know to build this stream without asking anyone. The repository's documents are the background, and where they and this brief disagree, write the disagreement in your journal fragment and follow the repository.

## What you build

The sampler in the controller's loop, the timeline kept through a worker's death, statistics over the run's windows, the result aggregated and written by the reporters (`--output json,csv`), per-provider remedies at the start, the verification duty cycle and the observers recorded (F17), EDAC deltas per phase (F13), and the fingerprint.

## Contracts

**You consume** — write your own mock of each, as you read its contract, and run the suite against your mock (ADR 0003):

- `SamplerContract` — `tests/contracts/telemetry/sampler_contract.hpp`
- `TimelineContract` — `tests/contracts/telemetry/telemetry_contract.hpp`
- `SummarizerContract` — `tests/contracts/statistics/signature_contract.hpp`
- `VerdictContract` — `tests/contracts/results/results_contract.hpp`
- `ResultWriterContract` — `tests/contracts/results/writer_contract.hpp`
- `SourceContract` — `tests/contracts/telemetry/telemetry_contract.hpp`

**Headers you build against** (frozen):

- every telemetry, statistics and results contract

## What you own

Directories (each `tests/*/...` entry means the directory of that name under every tier directory you need):

- `src/main/`
- `src/controller/ (top-level files)`
- `tests/unit/{main,controller}/`
- `tests/supervision/ (top level)`
- `tests/integration/m3/`
- your ledger fragments under `tests/obligations.d/`, your journal fragment `docs/journal/21-integration-m3.md`, and the `State` line of this brief

### Shared files this stream owns

No other stream in this wave edits them.

- The top-level `CMakeLists.txt` link lines and `tests/CMakeLists.txt`.
- `.github/workflows/mutation.yml` (mutation gating extended, M3 exit criterion 7).

## Exit criteria

1. M3 exit criteria 1-7, each with its evidence.
2. A worker killed mid-run leaves the telemetry before its death in the result.

## Notes

- You are the only stream that edits the M1 modules in this wave.
- Integration is the real-against-real tier of ADR 0003 (rule 4): the products of earlier streams are linked as they are, and run together and against the real kernel. Unit tests of your own glue still mock what they consume, with the suites run against those mocks.

## How your work joins the build without touching a shared file (ADR 0004)

- **Library:** `src/<your module>/CMakeLists.txt`, calling
  `loadforge_component(loadforge_<dir>_<subdir> SOURCES ... LINK ...)`. It is found by
  glob; never call `add_subdirectory()`.
- **Tests:** `tests/<tier>/<your module>/CMakeLists.txt` for each tier you owe, calling
  `loadforge_test_suite(loadforge_<tier>_<dir>_<subdir>_tests SOURCES ... LINK ...)`.
  Tier directories: `unit` (T1, T2), `oracle` (T3), `property` (T4), `fault_injection`
  (T5, T5b), `determinism` (T6), `supervision` (T7), `integration` (T8), `soak` (T10).
  Committed fixtures go under `tests/fixtures/<kind>/<your module>/`.
- **Ledger:** one fragment per module, `tests/obligations.d/<module path below src>.toml`,
  declaring exactly that module: classes, tiers, fixtures or fixture builders, `provides`,
  `consumes`, and a note that says what each class means for this module. Where this
  change already created the fragment, you take it over and rewrite it.
- **Journal:** `docs/journal/<NN>-<slug>.md`, yours alone: "Established by experiment",
  "Decisions", "Contract questions". The manager folds it into `docs/JOURNAL.md` at the
  end of the wave.
- **State:** the `State` line at the top of this brief is yours to keep current, in the
  pull request that changes it. `docs/ROADMAP.md` points here and is the manager's.

## Files you must not edit

Everything outside the directories this brief gives you, and in particular: the contract
headers (ADR 0004 lists them), `tests/contracts/`, `cmake/`, the top-level
`CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/obligations.toml`, `tools/`,
`.github/`, `docs/` other than your journal fragment, your brief's `State` line and a
proposed ADR, `src/platform/syscalls.*` and `tests/support/` — unless this brief names one
of them under "Shared files this stream owns".

## Rules every stream follows

These are the project's rules, restated so this brief stands alone. Where this list and a
document in the repository disagree, the repository wins and the disagreement goes in your
journal fragment.

1. **Coverage: 100% line, 100% branch and 100% function over `src/`, with zero
   exclusions.** `tools/coverage.sh build/coverage` after `cmake --preset coverage`,
   `cmake --build --preset coverage`, deleting the old `.gcda` files and
   `ctest --preset coverage`. The gate also requires every `src/**.cpp` to appear in the
   report. An uncoverable branch is a design defect: read `docs/JOURNAL.md` §1.9 (a forked
   child's counters are lost), §1.13 (a missing `noexcept` manufactures branches) and §1.22
   (a defaulted comparison in a header reads as uncovered from a second translation unit)
   before writing code. Every `core::Result<T, E>` your tests instantiate is measured as
   `src/core/result.hpp`: take both arms of each.
2. **Every execution path is covered to the standard of `docs/IMPLEMENTATION.md` §2:**
   executed, asserted, and shown to fail when broken. Every P2 path is injected — one test
   per errno per call site, through the substitutable seam (`platform::Syscalls`) or a seam
   of your own; never a real error you merely hope for. Every P7 source has its six states.
   Every verification path has a fault-injection test asserting *which* classification is
   produced (T5) and an unrun-check test asserting `VerificationNotPerformed` (T5b).
3. **Fixtures come from the real tool or the real kernel (F21).** A committed fixture
   carries the command and the machine that produced it. A model of an external tool or of
   the kernel is checked against the real thing at least once in a test that can fail.
4. **Contracts are frozen.** You may not edit any contract header listed in ADR 0004, any
   file under `tests/contracts/`, or `cmake/Components.cmake`. If a contract is ambiguous
   or wrong, write the question and the reading you chose in your journal fragment under
   "Contract questions", test your reading in your own tree, and carry on; the contract is
   corrected between waves.
5. **Components share contracts, never tests or mocks (ADR 0003).** Write your own mocks of
   the contracts you consume, and run the provider's suite against each of them
   (`INSTANTIATE_TYPED_TEST_SUITE_P(<YourMock>, <Suite>, ...)` in a test under
   `tests/<tier>/<your module>/`). Run the suite of every contract you provide against
   your implementation the same way. Declare both in your ledger fragment as
   `provides = [...]` and `consumes = [...]`; `tools/check-test-obligations.py` refuses a
   declaration with no instantiation behind it. Never read another stream's tests, mocks or
   implementation.
6. **Decisions are recorded where they are made.** Research a decision to the depth it
   deserves, take the most defensible option, write the options and the reason in your
   journal fragment, and keep going. A decision that meets ADR 0001's test goes in
   `docs/adr/proposed/<NN>-<slug>-<topic>.md` (`<NN>` is your stream's number); the
   manager numbers it when it lands. Facts you establish by experiment go in the journal
   fragment too, with the evidence.
7. **Commits:** `git commit -s`, author and committer
   `Iaroslav Voitovych <yaroslav.voytovych@gmail.com>`, no trailer but `Signed-off-by`, no
   signing, never `--no-verify`. Messages in the repository owner's plain voice: what
   changed and why. No assistant, agent, vendor or model names anywhere — not in commits, code,
   comments, documents or branch names.
8. **Branches:** `feature/<slug>`, `fix/<slug>`, `docs/<slug>`, `chore/<slug>` or
   `refactor/<slug>` only. Commit as you go, so work survives a restarted container.
9. **Before every push, the full local gate, and read its real exit code** (not the last
   line of its output): debug build and tests with GCC, the coverage gate, the ledger gate,
   ASan+UBSan and TSan with Clang, the scalar-only build, the static build, clang-format,
   clang-tidy over `src/`, ShellCheck, Ruff, and `tests/tools/test_tooling.sh`. The
   workflows under `.github/workflows/` are the definitive list; CI runs them all on
   x86-64 and ARM64.
10. **Dated documents carry the date and the time in UTC** ("2026-10-20 05:20 UTC"); a file
    name that carries a date uses `YYYY-MM-DD-HHMM-UTC`.
11. **Required reading before code:** `docs/verification.md` and `docs/determinism.md` in
    full, ADR 0003 and ADR 0004, `docs/IMPLEMENTATION.md` §2, and the contract headers
    this brief names. Atomics: every memory order named; anything weaker than `seq_cst`
    carries a written synchronisation argument and a test of the ordering (PLAN §4.5).
    Process-shared locks: none, by design; the ledger gate refuses one undeclared.

## When you are done

Open a pull request from your branch against `main` with the tiers your change adds to and
the exit criteria, each marked met with its evidence. The manager merges stream pull
requests. Report back with: the commit list, what each module provides and consumes, the
exit criteria with their evidence, the contract questions you recorded, and anything you
could not do and why.
