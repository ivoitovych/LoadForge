# Stream 02 — Input integrity: the slice runner, its bracket, and protected control state

**State:** not started  
**Written:** 2026-10-20 05:17 UTC  
**Wave:** 1  
**Tier:** Tier B — concurrency, supervision, verification oracles, or a pattern every later stream copies  
**Milestone:** M2a  
**Branch:** `feature/input-integrity`  
**Depends on:** contracts only (ADR 0004); nothing from another stream

This brief is self-contained: it is everything you need to know to build this stream without asking anyone. The repository's documents are the background, and where they and this brief disagree, write the disagreement in your journal fragment and follow the repository.

## What you build

The provider of `verification/runner.hpp`: running a workload's units in slices, each inside the input-integrity bracket of verification.md §4a (write, check, compute, oracle, check again), with the duty cycle of F17, the working set mapped through `platform/memory` with every slot line-aligned, and every finding turned into a record with full provenance. And F18 rule 3: seeds, indices, tolerances and expected checksums stored as three independently checksummed copies on different pages (and NUMA nodes where there are several), written separately, voted on every use, a three-way disagreement a hard failure.

| Module | Classes | Tiers owed | What they mean here |
|---|---|---|---|
| `src/verification/integrity` | P1, P2, P3, P4, P5 | T1, T2, T5, T5b | P2: every failure of mapping, advising, binding and locking the working set, injected through `platform::Syscalls`, each a `RunnerError` naming the step. P4: an input wrong before consumption, one that goes wrong during it, a wrong output, an oracle that cannot check — each a fault-injection test asserting the record produced; a run that checked nothing reports `VerificationNotPerformed` from `finish()`. P5 only if units run concurrently: the synchronisation argument and its test. Control state: a copy corrupted is excluded and repaired; two copies disagreeing with a third is decided by the two verified ones; three disagreeing stops the run and reports itself, never a hardware fault (verification.md §6, last bullet). |

The classes and tiers are a starting point taken from `docs/IMPLEMENTATION.md` §3; your ledger fragment records the ones the code you write actually has, with a note saying what each means for it. A class you find and this table omits is added; one it lists that your code turns out not to have is removed, with the reason in the note.

## Contracts

**You provide** — run each suite against your implementation, from your own tests:

- `SliceRunnerContract` — `tests/contracts/verification/runner_contract.hpp`

**You consume** — write your own mock of each, as you read its contract, and run the suite against your mock (ADR 0003):

- `WorkloadContract` — `tests/contracts/workloads/workload_contract.hpp`
- `OracleContract` — `tests/contracts/verification/oracle_contract.hpp`
- `ExecutorContract` — `tests/contracts/worker/executor_contract.hpp`

**Headers you build against** (frozen):

- `src/verification/runner.hpp` — running a workload in slices inside the input-integrity bracket: SliceRunner, Tally, make_slice_runner()
- `src/verification/record.hpp` — the error-record model (ExactBitCorruption, NumericalVerificationFailure, VerificationNotPerformed), provenance, bit classification, encode()/decode()
- `src/verification/oracle.hpp` — the independent oracle: Oracle, Verdict, OracleRegistry
- `src/workloads/workload.hpp` — a workload as a family of independent units: Workload, WorkloadConfig, Plan, Registry
- `src/worker/executor.hpp` — spreading a unit's work over a worker's threads: Executor, for_each_index()

## What you own

Directories (each `tests/*/...` entry means the directory of that name under every tier directory you need):

- `src/verification/integrity/`
- `tests/*/verification/integrity/`
- your ledger fragments under `tests/obligations.d/`, your journal fragment `docs/journal/02-input-integrity.md`, and the `State` line of this brief

## Exit criteria

1. SliceRunnerContract passes against `make_slice_runner()`.
2. A corrupted input is caught and reported as input, distinctly from a corrupted result, for both bracket outcomes (M2a exit criterion 3).
3. `VerificationNotPerformed` is produced for every way a check can be prevented, and the runner never routes one as a wrong result (M2a exit criterion 4, the runner's half).
4. A corrupted stored seed is repaired or rejected and never produces a hardware-fault report.
5. 100% line, branch and function; every P2 path injected; ledger fragment current.

## Notes

- Your mocks of the workload, the oracle and the executor are your own; the suites in `tests/contracts/` are how you check you read their contracts as their providers will.

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
