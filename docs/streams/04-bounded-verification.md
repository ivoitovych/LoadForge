# Stream 04 — Bounded verification: derived tolerances, invariants, and the dense-linear oracle

**State:** merged, #42, 2026-10-20 18:03 UTC  
**Written:** 2026-10-20 05:17 UTC  
**Wave:** 1  
**Tier:** Tier B — concurrency, supervision, verification oracles, or a pattern every later stream copies  
**Milestone:** M2b  
**Branch:** `feature/bounded-verification`  
**Depends on:** contracts only (ADR 0004); nothing from another stream

This brief is self-contained: it is everything you need to know to build this stream without asking anyone. The repository's documents are the background, and where they and this brief disagree, write the disagreement in your journal fragment and follow the repository.

## What you build

The bounded half of verification.md §5: tolerances derived from the problem, never chosen; invariant checks with their own derived bounds; and the independent oracle for the dense-linear workload (naive scalar dot products on sampled entries), written from `src/workloads/dense_linear/spec.hpp` and never from the kernels.

| Module | Classes | Tiers owed | What they mean here |
|---|---|---|---|
| `src/verification/residual` | P1, P3, P4 | T1, T3, T5, T5b | The derivation of a dot product's bound from gamma_n, written in full in the header, and the comparison at the bound. P3: subnormal, zero, NaN and infinite inputs; the bound at (1 - epsilon) held and at (1 + epsilon) breached (M2b exit criterion 3). |
| `src/verification/invariant` | P1, P3, P4 | T1, T4, T5 | Invariant and conservation checks later workloads use (sums, norms, permutation and order checks), each with its bound derived; each fails under the corruption it exists for. |
| `src/verification/oracle/dense_linear` | P1, P3, P4 | T1, T3, T5, T5b | Sampled entries chosen by the unit index alone; a NaN or out-of-tolerance entry is a `BoundedMismatch` naming the element; a non-finite or out-of-range input entry is `NotChecked`, because the tolerance cannot be derived. |

The classes and tiers are a starting point taken from `docs/IMPLEMENTATION.md` §3; your ledger fragment records the ones the code you write actually has, with a note saying what each means for it. A class you find and this table omits is added; one it lists that your code turns out not to have is removed, with the reason in the note.

## Contracts

**You provide** — run each suite against your implementation, from your own tests:

- `OracleContract` — `tests/contracts/verification/oracle_contract.hpp`

**Headers you build against** (frozen):

- `src/verification/oracle.hpp` — the independent oracle: Oracle, Verdict, OracleRegistry
- `src/verification/record.hpp` — the error-record model (ExactBitCorruption, NumericalVerificationFailure, VerificationNotPerformed), provenance, bit classification, encode()/decode()
- `src/workloads/dense_linear/spec.hpp` — the dense-linear specification both the workload and its oracle are written from
- `src/primitives/rng/philox.hpp` — counter-based generator Philox4x32-10 and the (seed, stream, position) addressing; carries its definition

## What you own

Directories (each `tests/*/...` entry means the directory of that name under every tier directory you need):

- `src/verification/residual/`
- `src/verification/invariant/`
- `src/verification/oracle/dense_linear/`
- `tests/*/verification/residual/`
- `tests/*/verification/invariant/`
- `tests/*/verification/oracle/dense_linear/`
- your ledger fragments under `tests/obligations.d/`, your journal fragment `docs/journal/04-bounded-verification.md`, and the `State` line of this brief

## Exit criteria

1. OracleContract passes for the dense-linear oracle; its good units are products of matrices whose exact product is known (integer-valued or dyadic entries), never kernel output.
2. The tolerance's derivation is written down, and a test breaches it at 1 + epsilon and holds it at 1 - epsilon (M2b exit criterion 3).
3. A `NumericalVerificationFailure` built from these verdicts has no bit fields, asserted (M2b P4 row).
4. 100% line, branch and function; ledger fragments current.

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
   `docs/adr/<NNNN>-<slug>.md` (the next free number, Status `proposed`); the
   manager marks it accepted when it lands. Facts you establish by experiment go in the journal
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
