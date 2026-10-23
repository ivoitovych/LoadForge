# Documentation against code, LoadForge main (720fdbb)

Method: every command, option, exit code, path, metric name, default and behavioural claim
in the seven documents was read against src/, config/, packaging/, tools/ and
.github/workflows/. A private copy of HEAD was built (debug, target `loadforge`) and the
commands were run: every README example, the safety.md signal/journal/refusal experiments,
the telemetry capability report, an explore run, mixed and cycling, and
`packaging/validate-machine.sh --workload-seconds 5` (all 26 steps PASS, exit 0). Known
items (temperature ceiling, hardware-failure template, PLAN/verification.md
benchmark/selftest, ShellCheck vs packaging/, IMPLEMENTATION section 10) are not
repeated. The copy is deleted.

## Disagreements

Ordered by importance. "Doc wrong" = fix the document; "code gap" = the document states the
intended behaviour and the code does not do it.

1. packaging/README.md:109-111 and CONTRIBUTING.md:327-329: say that setting
   `project(VERSION X.Y.Z)` and `LOADFORGE_MILESTONE` makes `loadforge --version` print
   exactly `1.0.0` (or `1.0.0-rc1`). CMakeLists.txt:32-33 always builds
   `"${PROJECT_VERSION}-${LOADFORGE_MILESTONE}"`, so an empty milestone gives `1.0.0-`
   (trailing hyphen); there is no way to print a bare `X.Y.Z`. The release workflow
   (release.yml, "The tag is the version the binary reports") compares `v$reported` with
   the tag, so a `v1.0.0` tag would fail. Found by reading the CMake; the string was not
   produced by a build. Code gap (or doc wrong: the plain release needs a CMake change
   before it can be tagged).

2. README.md:84-87 ("Every test emits a multidimensional record ... peak and equilibrium
   temperature, temperature rise rate, memory bandwidth, IPC, cache and branch miss rates,
   throttling time"): the result holds per-series summaries (min, max, mean, stddev, first,
   last, slope for gauges; delta and rate for counters) of the metrics in
   docs/telemetry-sources.md. There is no memory-bandwidth metric, no throttling time, no
   equilibrium temperature, and no derived IPC or miss-rate (the raw counters
   `counters.cycles`, `.instructions`, `.cache_misses`, `.branch_misses` exist, and are
   absent on a machine without a PMU). `grep -ri "ipc\|bandwidth\|throttl\|equilibrium"`
   over the telemetry and statistics code finds none. Doc wrong (it lists intended
   dimensions as delivered; README "Known gaps" does not mention it).

3. docs/safety.md:135 and 161 of docs/architecture.md: safety.md quotes the message
   `killed by the kernel for memory` and the fate `oom killed`. Code:
   src/controller/supervision.cpp:31 describes the fate as `killed by the OOM killer`; no
   string `kernel for memory` or `oom killed` exists in src/. architecture.md:158-163 also
   paraphrases fate names that differ from supervision.cpp:15-45 (`startup failed`,
   `fault`, `record channel corrupt`, `evidence truncated`, `unrecognised termination`);
   it is prose, not quoted, so only safety.md:135 is a literal mismatch. Doc wrong.

4. docs/safety.md:226 ("Only `clean` exits 0"): a run whose workloads are all the `null`
   workload ends with verdict `incomplete` and exit 0. Run: `loadforge stress --config`
   with config/quick.toml at 2 s printed `verdict: incomplete`, exit 0. Code:
   src/main/outcome.cpp:61 returns 6 only when `verified` is true. README.md:201 states this
   correctly; safety.md:226 contradicts README.md:201. The wording of code 6 ("a run that
   checked nothing is not clean", README.md:215, architecture.md:215-216, `--help`) is also
   untrue for that case. Doc wrong (qualify "for a run with a verified workload").

5. docs/hardware-validation.md:342 ("`ended abnormally between records` (or `with a torn
   last record`)"): the torn case prints `ended abnormally inside a record`
   (src/controller/journal/journal.hpp:182-183). The first phrase is right (journal.hpp:179,
   and seen in the output of a killed run). Doc wrong, and a person checking the power-cut
   result by eye would be looking for a string that does not exist.

6. docs/hardware-validation.md:281 (`settled`/`unsettled` per trial in the report): the
   explore trial log prints `settled` or `not settled`
   (src/main/trial_runner.cpp:373). Doc wrong.

7. README.md:330: "the newest `reviews/review-request-*.md` ([this one](...0352-UTC.md))".
   The directory holds review-request-2026-10-20-0352, -1251 and -1841; the link points at
   the oldest. Doc wrong (link or wording).

8. README.md:301 ("a memory check before the workers start"), with docs/safety.md:124-142
   and docs/hardware-validation.md:207-209: the check runs only on a memory a configuration
   or `--footprint` states. `stress --test NAME` without `--footprint` builds its workloads
   with no memory (src/main/cli.cpp:738), `any_memory_configured` is false
   (src/controller/safety.cpp:56,79), and the run prints no `limit memory` line (seen).
   So the documented default (one worker per CPU up to 64, 32 MiB each, about 2 GiB) and a
   user's `--workers N` are never compared with the budget; only `--footprint 1TiB`
   is refused (exit 1, seen). safety.md:124 is worded correctly about configured memory;
   the README sentence and hardware-validation.md:207-209 ("use --workers 8") imply a
   protection the default run does not have. Doc imprecise; arguably a code gap.

9. README.md:139 and CONTRIBUTING.md:289: the preset lists omit `static-headers`, a visible
   configure and build preset (CMakePresets.json). `static` has no test preset, which
   CONTRIBUTING.md:310 correctly does not use. Doc incomplete (low).

10. docs/hardware-validation.md:95 ("about 25 minutes at 60 s per workload"): the script's
    own arithmetic is 11 x 60 s + 2 x 180 s + the short steps and explore, about 19 to 20
    minutes. Not timed at 60 s; the 5 s run ran correctly. Doc estimate high (low).

11. CONTRIBUTING.md:97-102 ("every process-shared lock is `PTHREAD_MUTEX_ROBUST` ... each
    such lock needs a test that kills the holder") against docs/architecture.md:230-231
    ("there is no process-shared lock anywhere"): src/ has no pthread mutex or
    PROCESS_SHARED use (grep). The two documents agree only vacuously; CONTRIBUTING's rule
    has no subject in the tree. Not wrong, but unmarked as a rule for future code (low).

12. docs/safety.md:139 ("Each worker's working set is 32 MiB by default"): true for every
    single-workload run (all eleven checked). In `--test mixed` the banner shows 16MiB for
    stencil and adaptive_mesh workers. Doc imprecise (low).

13. packaging/README.md:47-48 lists `file` as a build requirement. No script in the build
    path (build-release.sh, build-deb.sh, check-no-test-hooks.sh, check-runtime-deps.sh)
    calls `file`; release.yml installs it and validate-machine.sh uses it. Doc imprecise
    (low).

## Checked fully and found accurate

- docs/telemetry-sources.md: every metric name and count (24 metrics, `10 of 24` on this
  machine, 7 of 7 memory, frequency 2 of 6 with 4 CPUs), source paths, thermal naming
  (coretemp "Package id N"/"Core N", k10temp "Tdie"), busy/total tick formulas, the six
  reasons, 1 s default sample interval, the sed ranges and the sample output,
  summary fields, EDAC 32-bit reset rule, verdict and exit code for corrected errors, the
  telemetry line format. No disagreement found.
- docs/architecture.md except the fate-name paraphrase in item 3 and the exit-6 wording in
  item 4: internal switches `--worker`/`--rerun` (not in `--help`), exit 119, /proc/self/exe,
  setsid and group kill, half-heartbeat slice, 10 s verifying stop grace, `k mod period`
  rule, ISA paths (`--isa avx512` is refused: "no avx512 kernel"), seven sources, CRC-32 per
  journal record, seed stride, explore reference/drift/95% interval, exit codes and their
  precedence 5, 4, 3, 6, journal exit codes, directory and ADR references.
- docs/safety.md except items 3, 4 and 12: stop/SIGTERM/SIGKILL timings (1 s default, 10 s
  verifying, 5 s scheduled raised to 10 s), 100 ms beat, staleness 10 s/2 s, PDEATHSIG
  SIGKILL, SIGINT 130 and SIGTERM 143 with no report and no stray workers (run), SIGKILL of
  the controller leaves no worker (run), journal text, `j.1` rotation (run), refusal
  messages and exit 1 and 4 paths, `.loadforge-write-test` and `.partial`, default lock
  `try`, 64-worker cap, 1 to 150 range for `max_temperature_c`. Section 1 otherwise skipped
  as known.
- packaging/README.md except items 1 and 13: tarball and package contents, file names,
  `--only`, `--out`, the build steps and their order, the Debian version rule, test-deb
  behaviour (refuses if installed, purges on exit), release workflow (draft, tag check,
  merged SHA256SUMS, rehearsal, pull-request paths, SHA-pinned actions). The sample output
  matches the scripts' messages; the four-minute build time was not timed.
- docs/hardware-validation.md except items 5, 6, 10: validate-machine.sh options, defaults
  and step list (run end to end), the sixth-sample soak comparison and its thresholds,
  results file naming, explore flags and defaults (settle slope 0.05, window 30 s, max
  300 s), `worst case:` and `explore: T trial(s) in R round(s)` formats, the exit 1 refusal
  for an unavailable metric, the digest `e0ff849ff34906de`, `GLIBC_2.34` and
  `GLIBCXX_3.4.32` (objdump on the debug build), soak.yml and its test names.
- CONTRIBUTING.md except items 1, 9, 11: determinism class names, branch-name rule,
  identity file, hooks installer behaviour, gate commands and their files (coverage.sh
  argument, test_tooling.sh, check-test-obligations.py, mutation-shard.py
  `check-processes`, ruff 0.15.8, exclusion count zero, issue-template names), toolchain
  versions, `-ffp-contract=off` and the fast-math ban.
- README.md except items 2, 7, 8, 9: command list and every example (run), option syntax,
  banner, config example and refusal of an unknown key by name, exit code table and
  precedence, workload names and count, version `0.0.0-m2`, `--selftest-digest`, CI matrix,
  weekly mutation, Ctrl-C behaviour. Neither "no `selftest` command" nor the temperature
  gap was re-examined (known).
