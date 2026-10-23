# Journal fragment 34e — `loadforge selftest`

**Written:** 2026-10-23 12:50 UTC
**Change:** `src/main/selftest_command.{hpp,cpp}` (new), `src/main/selftest.{hpp,cpp}` (`corpus_digest`, the expected digests), `src/main/{cli.cpp,outcome.hpp}`, `CMakeLists.txt`; `tests/unit/main/selftest_command_test.cpp`, `tests/integration/selftest/`, `tests/obligations.d/main.toml`; `README.md`, `CONTRIBUTING.md`, `docs/{verification,PLAN,IMPLEMENTATION,hardware-validation,ROADMAP}.md`, `.github/ISSUE_TEMPLATE/hardware-failure.yml`, `packaging/validate-machine.sh`.

## What is done

`loadforge selftest [--isa PATH] [--footprint SIZE] [--seed N] [--verbose]` runs the golden digest and, for every workload in the registry, three units at 3 MiB on the path this CPU would use for that workload and on the scalar path too when that is another. One line per check, then a verdict. Exit 0 every check passed, 8 a check failed, 1 an `--isa` this CPU or build cannot run, 2 usage. On this container (x86-64, AVX-512 reported) a debug build runs 19 checks in about ten seconds; a stress run's own debug unit times are what set that (graph, sort, sparse and dense-linear take one to two seconds each).

## Decisions

### Where the expected digest lives: committed, in two halves

Options: (a) computed at build time, (b) a committed constant that a test regenerates. Chosen (b), `kExpectedCorpusDigest` and `kExpectedDigest` in `src/main/selftest.hpp`. A value computed by the build under test agrees with a broken build by construction, so it can only detect a broken run-time, never a broken build; a committed one is independent of the code that computes it. It cannot drift silently: `SelftestExpected.TheCommittedDigestsAreWhatThisTreeComputes` recomputes both and fails with the new values in its message when the corpus or a scalar kernel changes on purpose. CI's cross-architecture comparison (`portability.yml`) is unchanged and still the check that the two architectures agree.

The digest is checked in two parts so that a mismatch names what differed. `core_digest()` is now `workloads_digest(corpus_digest())`. The second check computes `workloads_digest(kExpectedCorpusDigest)` and compares it with `kExpectedDigest`; fed the expected first half, it is independent of the corpus check (a corpus mismatch is not also reported as a kernel mismatch). Both passing proves `core_digest() == kExpectedDigest`, which is what `--selftest-digest` prints; the test asserts that directly too.

`tests/fixtures/golden` (the golden vector files) is not what selftest compares with: the fixtures are not shipped with the binary, and the selftest has to run from an installed package.

### The exit code: 8

The table in `src/main/outcome.hpp` and the README has 0, 1, 2, 3, 4, 5, 6, 7, 130, 143. A failed selftest is a verdict about the environment and no stress code says that: 5 is a finding about the hardware, which is exactly what selftest must not claim; 6 is a run that checked nothing; 3, 4 and 7 are a run's workers, its result file and its heat. So 8, `kExitSelftestFailed`, next after 7, returned by no other command, which also means no precedence among the codes is needed. 1 for refusals and 2 for usage are kept as the other commands use them.

### Which units, which footprint

- The verified run is `worker::prepare` and the input-integrity runner through `worker::Services`, the same call a stress worker and `--rerun` make, one thread, no pinning, lock policy `none` (a small test must not fail on an `RLIMIT_MEMLOCK` of 8 MiB), oracle and input bracket on every unit. A record of any kind, including the `VerificationNotPerformed` that `finish()` makes when nothing was compared, fails the check: a check that compared nothing is not a pass (verification.md §5).
- Footprint 3 MiB, and `--footprint` is refused below it: sort's unit is 2.6 MB, graph's 1.4 MB, dense-linear's 1.5 MB, and a workload that refuses a footprint says `footprint too small for one unit`. Left as a failed check that would read as a failed environment (the verdict says "not trustworthy for a hardware verdict"), when it is the user's typo. Measured with the debug build: at 1 MiB six checks fail to set up, at 2 MiB sort does, at 3 MiB none. The floor is a constant in `selftest_command.cpp`; a workload that grows a larger unit moves it, and the unit test that runs every workload will say so.
- Three units, not four or ten: the time is dominated by graph (2.5 s per path for three units), sort (1.8 s), sparse and dense-linear (1.4 s). Per-check times in a debug build, 3 MiB, three units: adaptive_mesh 0.24 s, compression 0.07, dense_linear 0.7 (avx2) and 1.4 (scalar), fft 0.09, graph 2.7 and 2.3, monte_carlo 0.6, particles 0.08, raytrace 0.06, sort 1.8, sparse 1.4, stencil 0.4 and 0.6.
- A path a workload has no kernel for: with `--isa avx2`, a workload with no AVX2 kernel is checked on scalar alone, and its line says so (`controller::choose_isa`'s own words). A path the CPU or the build cannot run at all is a refusal (1) before any check, with `select_path`'s words: used or refused, never replaced.
- Cache-line size: the check uses 64 and does not read the topology. Selftest reports no classification as a verdict (a finding's first lines are kept on the check's line, the full record is under `--verbose`), and reading `/sys` would make the command fail on a container where stress itself would refuse. If the full record is ever to be filed, the machine's line size matters and the filer has the stress run's. Open if the owner wants it read.

### What a pass and a failure say

Pass: the build on this machine reproduces what it is expected to; it is a precondition of a hardware verdict, not a finding that the hardware is sound. Failure: the environment is not trustworthy for a hardware verdict; the machine may be faulty, the build or the tool may be broken, or the platform may be unsupported, and selftest does not say which (verification.md §9, unchanged in substance).

## Tests

Unit tests over the worker's services seam (`worker::Services`): the real registries, oracles, registers and threads, with one part replaced where a test wants it to misbehave. Digest mismatch of each half; a workload that computes a wrong byte in every unit, which the real compression oracle finds, exit 8; a registry that will not describe a name; a workload that cannot be set up (footprint of 1 byte); a runner that refuses a slice and one that refuses to finish; an `--isa` the probe does not support; the paths over synthetic sets (requested, unsupported, not offered by the workload), independent of the machine the test runs on; every usage error. No new seam was invented: the corrupting workload is the same shape as `tests/supervision/faulty_worker.cpp`'s and `rotation_faults_test.cpp`'s. Integration: the real binary passes, prints its paths under `--verbose`, refuses an `--isa` it cannot run, rejects a bad argument, and lists itself in `--help`. None asserts a time.

## Questions for the contract owners

None: no contract header and nothing under `tests/contracts/` was changed. `controller::choose_isa`, `worker::prepare` and `worker::Services` are used as they are.

## Open

- The outside reviewer the project still needs.
- `packaging/README.md` and `docs/hardware-validation.md` §4.4 quote `e0ff849ff34906de` for a `0.0.0-m2` build; it is the same value as `kExpectedDigest`, now committed.
- `ControlOverRealWorkers.ADutyCycleMakesOneWorkerDoLessThanItsNeighbour` failed once in the full coverage run under `-j3` and passed alone and on the debug run; it is not touched by this change and was not investigated here.
