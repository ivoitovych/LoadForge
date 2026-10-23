# Verdicts on reviews D, E and F (documents against the tree)

Written 2026-10-23 00:05 UTC. Tree: origin/main 720fdbb. Pull request #102: branch
`docs/tree-disagreements`, head 342cbb9 (read through FETCH_HEAD). Nothing in any repository
was edited, committed or pushed. The private copy used for the version check was deleted.

What #102 changes (diff against main): hardware-failure issue template (selftest question
reworded), a dated "not built" note on `loadforge selftest` at PLAN.md:282 and PLAN.md:1461
and a status block at verification.md §9, ShellCheck over packaging/ in ci.yml, a new status
paragraph at the top of IMPLEMENTATION.md §10 (the old M1 paragraphs kept "as its record"),
and ROADMAP 34c marked done. It touches none of the files cited by reviews D and E.

Severity: high = a reader would act on a false statement, or a release step fails;
medium = stale or false but unlikely to mislead; low = detail.
"Side" = which side is wrong (document or code).

---------------------------------------------------------------------------------------------

## Review D: user documents

### D1  CONFIRMED  high  side: code (document states the intent)
Evidence: CMakeLists.txt:31-33 sets `LOADFORGE_VERSION_STRING "${PROJECT_VERSION}-${LOADFORGE_MILESTONE}"`
unconditionally. A private copy of origin/main configured with `-DLOADFORGE_MILESTONE=` generated
`kVersion = "0.0.0-"` (trailing hyphen) in `generated/core/version.hpp`; `--version` prints
that constant (src/main/cli.cpp:1109-1111). release.yml:44-49 compares `v$reported` with the tag,
so tag `v1.0.0` against a binary reporting `1.0.0-` fails at the "tag is the version" step.
There is no way to print a bare X.Y.Z today. (`1.0.0-rc1` works.)
Fix (code): in CMakeLists.txt build the string as `${PROJECT_VERSION}` and append
`-${LOADFORGE_MILESTONE}` only `if(NOT LOADFORGE_MILESTONE STREQUAL "")`; add a CMake or tooling
test that an empty milestone gives a bare version. Documents (packaging/README.md:109-111,
CONTRIBUTING.md:327-329) then stay true.
Resolved by #102: no.

### D2  CONFIRMED  medium  side: document
Evidence: README.md:84-87 lists "peak and equilibrium temperature, temperature rise rate, memory
bandwidth, IPC, cache and branch miss rates, throttling time". Grep of src/ (case-insensitive)
for bandwidth, throttl, equilibrium, miss rate, instructions-per finds only unrelated comments
(a comment on a ceiling in results/aggregate.hpp, `memory.high` as a "throttle" in
resources/memory_budget.hpp, "latency, not bandwidth" in primitives/cache). Raw counters
(cycles, instructions, cache_misses, branch_misses) exist; no derived metric. docs/platforms.md:135
also lists memory bandwidth as a capability row, but none is read (see F-E8).
Fix: README.md:84-87 -> "a record of summaries (min, max, mean, standard deviation, first, last,
slope for gauges; delta and rate for counters) of power, energy, frequency, temperature, memory
and the raw cycle, instruction, cache-miss and branch-miss counters, plus verification errors";
list the derived rates (IPC, miss rates, bandwidth, throttling time, equilibrium temperature) under
"Known gaps" or drop them.
Resolved by #102: no. Touches README.md: waits for 34a.

### D3  CONFIRMED  medium  side: document
Evidence: docs/safety.md:135 quotes `killed by the kernel for memory` (fate `oom killed`).
src/controller/supervision.cpp:31 returns `killed by the OOM killer`; neither quoted string is in
src/. docs/architecture.md:158-163 paraphrases the fates ("killed by the kernel for memory",
"channel corrupt", "truncated evidence", "startup failure") against supervision.cpp:15-45
(`record channel corrupt`, `evidence truncated`, `startup failed`); it is prose, not a quote.
Fix: safety.md:135 -> "reports `killed by the OOM killer` (fate `kOomKilled`)". Optional: architecture.md
158-163, use the exact fate strings.
Resolved by #102: no. Touches safety.md (and architecture.md): waits for 34a.

### D4  CONFIRMED  medium  side: document
Evidence: src/main/outcome.cpp:53-63: `return verified && verdict == kIncomplete ? kExitNotVerified : 0;`.
A run with no verified worker (all `null` workloads) ends `incomplete` and exits 0. docs/safety.md:226
"Only `clean` exits 0" is false for that case; README.md:201 states it correctly. The wording of
exit 6 ("a run that checked nothing is not clean": README.md:215, architecture.md:215-216,
`--help`) is also untrue for that case.
Fix: safety.md:226 -> "A run with a verified workload exits 0 only when the verdict is `clean`; a run
of unverified (`null`) workloads can end `incomplete` and still exit 0". README.md:215 and
architecture.md:215-216: "a run with a verified workload that checked nothing".
Resolved by #102: no. Touches README.md, safety.md, architecture.md: waits for 34a.

### D5  CONFIRMED  medium  side: document
Evidence: src/controller/journal/journal.hpp:179 `ended abnormally between records`, :181
`ended abnormally inside a record`. docs/hardware-validation.md:342 says "(or `with a torn last record`)".
Someone checking the power-cut result by eye would look for a string that does not exist.
Fix: hardware-validation.md:342 -> "`ended abnormally between records` (or `ended abnormally inside a
record` when the last record was cut short)".
Resolved by #102: no.

### D6  CONFIRMED  low  side: document
Evidence: src/main/trial_runner.cpp:373 prints `settled` / `not settled`; hardware-validation.md:280-281
says `settled`/`unsettled`.
Fix: "`settled`/`not settled`".
Resolved by #102: no.

### D7  CONFIRMED  low  side: document
Evidence: reviews/ holds review-request-2026-10-20-0352, -1251, -1841 UTC; README.md:330 says "the
newest ... ([this one](reviews/review-request-2026-10-20-0352-UTC.md))", the oldest.
Fix: link review-request-2026-10-20-1841-UTC.md (or drop "newest"/"this one" and point at the
directory). Re-check at the time of the change; a new request makes any fixed link stale again.
Resolved by #102: no. Touches README.md: waits for 34a.

### D8  PARTLY  medium  side: document (imprecise); arguably code
Evidence: src/main/cli.cpp:738 builds `stress --test NAME` workloads with `test.footprint`, nullopt
without `--footprint`; src/controller/safety.cpp:56-79 `any_memory_configured` is then false, so no
`limit memory` status and no budget comparison. Holds in part: the check does run for a configuration
that states memory/`limits.memory` and for `--footprint 1TiB` (explicit memory). Does not hold: the
README sentence "a memory check before the workers start" and hardware-validation.md:207-209
("use `--workers 8`") imply the default footprint (32 MiB x workers, controller/plan.hpp:52) is checked;
it is not. safety.md:124 is already worded about configured memory.
Fix: README.md:301 -> "a memory check, against what the configuration or `--footprint` states, before
the workers start"; hardware-validation.md:207-209 add that the default footprint and `--workers` are
not checked against memory, so `--workers 8` is the user's own limit. (Code alternative, a separate
roadmap item: check workers x default footprint against the budget.)
Resolved by #102: no. README part waits for 34a; hardware-validation.md part does not.

### D9  CONFIRMED  low  side: document
Evidence: CMakePresets.json has configure and build preset `static-headers` (lines 59, 78) that README.md:139
and CONTRIBUTING.md:289 omit. `static` has no test preset (correctly not used in CONTRIBUTING.md:310).
Fix: add `static-headers` to both lists.
Resolved by #102: no. README part waits for 34a.

### D10  CONFIRMED  low  side: document
Evidence: validate-machine.sh runs 11 workloads x S, mixed and cycling at 3S each (lines ~158-168), plus
short steps and a 4 s explore: at S=60 about 17 minutes of load plus short steps, 18 to 20 minutes.
The "25 minutes" at hardware-validation.md:95 is high. Not timed on a machine.
Fix: "about 20 minutes".
Resolved by #102: no.

### D11  CONFIRMED  low  side: document (CONTRIBUTING, unmarked rule)
Evidence: git grep for `PTHREAD_MUTEX_ROBUST` and `PROCESS_SHARED` in src/ finds nothing; architecture.md:230-231
says none exists. CONTRIBUTING.md:97-102 states the rule without saying it currently has no subject.
Not a contradiction.
Fix: add one sentence to CONTRIBUTING.md:97: "No such lock exists today (architecture.md); the rule is for any
that is added."
Resolved by #102: no.

### D12  CONFIRMED  low  side: document
Evidence: src/workloads/mixed/mixed.cpp:95-98 sizes datasets as the whole footprint or half of it (`half ?
footprint_bytes / 2`); safety.md:139 says each worker's working set is 32 MiB by default. True for single
workloads; in `mixed` it can be 16 MiB.
Fix: safety.md:139 -> "32 MiB by default for a single workload (`--footprint`); in `mixed` a dataset may be half of it".
Resolved by #102: no. Touches safety.md: waits for 34a.

### D13  CONFIRMED  low  side: document
Evidence: packaging/README.md:47-48 lists `file` among build requirements. Grep of packaging/*.sh,
tools/check-no-test-hooks.sh, tools/check-runtime-deps.sh finds no call of `file`; only
packaging/validate-machine.sh:128 (`file "$lf"`, the validation run) and the apt install line in
release.yml:40 use it.
Fix: move `file` from the build requirements to "needed by validate-machine.sh" (or say "for validation").
Resolved by #102: no.

---------------------------------------------------------------------------------------------

## Review E: root files and ADRs

### E-F1  CONFIRMED  medium  side: document/process
Evidence: docs/ROADMAP.md:87-90 promises "proposed ADRs numbered" at the end of every wave and ADR 0004's
table says "numbered by the manager when it lands" (0004:140). docs/adr/ holds 0001-0004 and
docs/adr/proposed/ still holds six files, all with Status "proposed": 02-input-integrity-working-set-memory,
06-workload-dense-linear-vector-kernels, 21-m3-integration-contract-disagreements-as-expected-failures,
21-m3-integration-sampling-points-and-phase-windows, 27-oracles-wave3-scratch-and-sampling,
29-q10-worker-granularity-under-a-mixed-workload. Waves 1-4 and M2, M3, M5, M6 are merged
(ROADMAP.md:93). The decisions were acted on (spot checks in E-F2 to E-F5 hold).
Fix: `git mv` into docs/adr/ as 0005 to 0010 in date order (06, 02, 27, 21-sampling, 21-disagreements,
29), set Status accepted with the amendments from F2-F5, retitle, and update the path citations in
docs/JOURNAL.md and docs/journal/*. Add a note, not an edit, to ADR 0004 (ADRs are immutable). Do this as
one change: the six edits F2-F5 ride along with it.
Resolved by #102: no.

### E-F2  CONFIRMED (one detail corrected)  medium  side: document
Evidence: src/verification/runner.hpp:135-153 defines `LockPolicy { kRequire, kTry, kNone }` with the comment
"Contract change of the M2 corrections, 2: until then the runner always locked"; the CLI selects among the
three (cli.cpp:267-275). The ADR's "There is no fallback ... does not start" and "superseded when" are
overtaken. Correction to the review: the CLI default is `try` (safety.md:139, `--lock try`), not `require`;
`kRequire` is the default only of `RunnerSetup`.
Fix: on numbering, record "accepted; amended 2026-10-21 18:02 UTC: locking is a controller-chosen
`LockPolicy` (RunnerSetup default kRequire; the command line defaults to try, which falls back and says so)";
state the 6.6 MB consequence for kRequire only.
Resolved by #102: no.

### E-F3  CONFIRMED  low  side: document
Evidence: src/main/run_result.cpp:50 uses `statistics::phase_window(marks, at, end_ns)` and :191
`statistics::run_window(...)`; `phase_windows()` (line 44) is now the labelling wrapper (run_result.hpp:38).
The ADR's "is to be replaced ... (recorded follow-up)" is done. The outcome box dates "2026-10-21" with no
time (CONTRIBUTING "Dated documents").
Fix: reword the outcome as "decision 3 is stated in statistics/signature.hpp and used by src/main/run_result.cpp;
the follow-up is done (<time> UTC)", with a time from git.
Resolved by #102: no.

### E-F4  CONFIRMED  low  side: document
Evidence: the proposed ADR's outcome box (lines 6-12) says "Outcome, 2026-10-21 (branch ...)" with no time and
Status "proposed"; the tests are in tests/integration/m3/corrections_test.cpp and nothing is `WILL_FAIL`
(the box already says so). Remaining defects are the missing time and status.
Fix: add the time (the corrections journal is 2026-10-21 11:45 UTC; the M2 round 18:02 UTC), number it
(F1), state "mechanism available, currently unused".
Resolved by #102: no.

### E-F5  CONFIRMED  low  side: document
Evidence: `WorkerWork` is defined at src/controller/report.hpp:53, not in result.hpp; results/result.hpp's
`WorkerResult` has no workload field. The ADR (lines 59-60) and docs/JOURNAL.md:3872 say "frozen in
`result.hpp`".
Fix: "`WorkerWork` (src/controller/report.hpp) records the workload a worker started as; the result's
`WorkerResult` does not carry the rotation"; same in JOURNAL.md:3872; Status accepted on numbering (M5 merged, #96).
Resolved by #102: no.

### E-F6  CONFIRMED  low  side: document and code comments
Evidence: tools/ has no `gen-golden/` (listing of origin/main). Cited at ADR 0004:119, docs/verification.md:97,
PLAN.md:1624, src/verification/golden/golden.hpp:28, tests/fixtures/lz4-workload/text.bin:97 (and
journal/01 and journal/27, streams/11:56). Regeneration exists only as per-fixture scripts:
tests/fixtures/determinism/workloads/sort/make_golden.py and tests/fixtures/workloads/monte_carlo/make_golden.py.
Fix: choose. Cheapest and defensible: amend (not edit) ADR 0004 with "regeneration is per-fixture
`make_golden.py`; no `tools/gen-golden/` was built"; change verification.md:97 and golden.hpp:28 comment
to name the scripts; drop it from the PLAN.md tree. (Or build the tool and keep the path.)
Resolved by #102: no.

### E-F7  CONFIRMED  low  side: document
Evidence: most streams' `State:` lines are pre-merge ("awaiting review", "pull request not yet opened",
"pushed ... pull request not opened"): streams 01-28, 30-32 inspected; only 29, 33, 34 say merged.
ADR 0004:139 names the State line as the status record; ROADMAP.md:93 says these are merged.
Fix: one pass setting each State line to "merged, #N, <date> <time> UTC" from the roadmap's merged list
(the same pass may fold into E-F1's change).
Resolved by #102: no.

### E-F8  CONFIRMED  low  side: document (or rule)
Evidence: ADR 0002 line 8 "Between 2026-08-24 and 2026-10-19 ..." and ADR 0003 (end of line 22, "review of
2026-10-19") carry dates without UTC times; CONTRIBUTING.md:278-284 says anything that carries a date carries
the time. Period and reference dates inside accepted ADRs; low.
Fix: either append times (ADR 0003: "2026-10-19 04:49 UTC", from the review file name) or state in
CONTRIBUTING that prose date ranges are exempt. The two "Outcome, 2026-10-21" boxes ride with E-F3/E-F4.
Resolved by #102: no.

---------------------------------------------------------------------------------------------

## Review F: design documents

### A1  CONFIRMED  high  side: document
Evidence: PLAN.md:2047-2087 (§11) says M0 has landed, "What does not exist yet is any workload", "Next is M1",
"eight CI workflows", and that the mutation gate "is specified, not demonstrated ... never executed ... Mull is not
pinned". Tree: eleven workloads, explore, journal, reporters present; .github/workflows/ has ten files (ci,
coverage, licensing, mutation, portability, release, release-static, sanitizers, soak, canary-2604); Mull 0.34.2
is pinned by SHA-256 in mutation.yml:30-31 and scheduled weekly.
Fix: replace §11 by a dated status paragraph (M0 to M6 built; remaining 34a-34d; first clean mutation sweep pending)
or point it at ROADMAP.md; keep the old text as history.
Resolved by #102: no (#102 only adds the "selftest not built" note to PLAN).

### A2  CONFIRMED  high  side: document
Evidence: PLAN.md:1056-1061 "the integration test has not been run. Mull is not pinned ... specified correctly,
not demonstrated". Same evidence as A1; IMPLEMENTATION and ROADMAP item 11 record the pin and demonstration.
Fix: turn the paragraph into history ("until 2026-10-.. (item 11)").
Resolved by #102: no.

### A3  CONFIRMED  medium  side: document
Evidence: PLAN.md:1946-1954 (§8) M0 row says "eight CI workflows" and two carried items; M1-M6 rows carry no
completion mark though ROADMAP shows them delivered (M1 only awaiting its first clean sweep).
Fix: mark M1 (pending the sweep) to M6 done, correct "eight" to the real count, drop the carried-items clause.
Resolved by #102: no.

### A4  CONFIRMED  high  side: document
Evidence: IMPLEMENTATION.md §10 (now lines 758-762 on main) says "The contracts for M2a to M7 are written and the
work after M1 is planned as streams in waves"; waves 1-5 and integrations are merged (ROADMAP.md:75-82).
Resolved by #102: YES. #102 adds a dated status paragraph at the top of §10 ("M0 to M6 are built and merged ...
M1's last condition ... has not run yet ...") and labels the old paragraphs "written while M1 was finishing ...
kept as its record".
Fix: none remaining beyond #102.

### A5  PARTLY  medium  side: document
Evidence: IMPLEMENTATION.md:797-800 "five survivors are expected in src/platform/syscalls.cpp" and Mull "has not
yet swept src/". The review cites JOURNAL §2.28 for 56 survivors in six of nine shards, later killed or removed;
the "first clean weekly sweep not yet recorded" part is consistent with ROADMAP and mutation.yml cron `17 3 * * 1`.
The count/expectation is stale.
Resolved by #102: partly. The paragraph survives, but #102 now frames it as the M1-time record, so a reader is
told it is history. The numbers are still wrong as history of the present.
Fix: replace the sentence with the observed result (JOURNAL §2.28) and the current state ("scheduled; no clean
sweep recorded yet"), or delete it from the record.

### A6  CONFIRMED  high  side: document
Evidence: IMPLEMENTATION.md:246-288 §3 ledger: `controller/orchestration`, `controller/safety`,
`controller/supervision`, `reporters/console` rows (marked M1, not shipped) and every row from
`controller/results` to `explore/` (M3, M5, M6, workloads, etc.) lack the shipped mark although the directories
exist in src/. Header (line 3) says "revision 3 -- reconciled against the tree"; ROADMAP.md:64-66 says §3 and
§10 are reconciled at the end of each wave.
Fix: reconcile the table once against src/ and tests/obligations.toml (mark shipped, one-line notes).
Resolved by #102: no.

### A7  CONFIRMED  high  side: document
Evidence: IMPLEMENTATION.md:257-260 rows `platform/sysfs`, `platform/procfs`, `platform/perf_events`,
`platform/edac`: no such directories. src/platform has only `fpenv/` and `isa/` as directories; telemetry sources
are src/telemetry/sources/{counters,edac,fan,frequency,memory,power,thermal} and src/telemetry/sampler; also
missing from the table: `worker/control`, `worker/executor`, `platform/isa`. PLAN.md:1546 (same four names),
1545-1551, 1554-1561, 1591 draw flat files (`fs`, `affinity`, `memory`, `process`, `clock`; controller/worker
sub-directories `orchestration/ safety/ supervision/ scheduler/`; `reporters/console/`) as directories.
Fix: in the A6 reconciliation replace the four rows by `telemetry/sources/*` and add the three missing; in PLAN.md
§6 redraw the tree from `git ls-tree` or mark it "indicative".
Resolved by #102: no.

### A8  PARTLY  medium  side: document
Evidence: IMPLEMENTATION.md:229-232 "adding a module without adding a row here is itself a gate failure", :238-239
"the gate catches it", :741. tools/check-test-obligations.py requires every src/ directory to appear in
tests/obligations.toml / tests/obligations.d (its docstring, lines 15, 28-29); no tool or workflow reads the
IMPLEMENTATION.md tables (grep of tools/, tests/tools, .github/workflows only finds comments naming sections).
Holds: the gate exists and does catch a module with no ledger row. Does not hold: it is tests/obligations.toml
that is gated, not the §3 table, which drifted unobserved (A6/A7).
Fix: reword: "the gate reads tests/obligations.toml; this table is the prose of it and is reconciled by hand each
wave", or build the check (table rows against obligations.toml).
Resolved by #102: no.

### A9  CONFIRMED  medium  side: code/plan not met (document promises)
Evidence: IMPLEMENTATION.md:739 "`tools/capture-sysfs.sh` lands with M3"; M3 is merged (ROADMAP.md:79) and
tools/ has no such script. Still referenced as future in .github/ISSUE_TEMPLATE/unsupported-platform.yml:16 and
CONTRIBUTING.md:275 ("there will be a ..."); PLAN.md:1624 draws it without a planned mark.
Fix: either build the script (small) or say it slipped: IMPLEMENTATION.md:739 "did not land with M3; no queue
item", and the two references say "not yet available; attach `find /sys ... ` output by hand".
Resolved by #102: no.

### A10  CONFIRMED  medium  side: document
Evidence: IMPLEMENTATION.md:566-569 (M3 exit criterion 7) "extended ... to verification/, config/, safety/,
controller/supervision/"; PLAN.md:1795-1799 "run nightly ... gating on" the same four. tools/mutation-shard.py
makes every module under src/ a shard (IMPLEMENTATION.md:672-679) and the job is weekly.
Fix: "every module under src/, weekly, one job per module" in both places.
Resolved by #102: no.

### A11  CONFIRMED  low  side: document
Evidence: IMPLEMENTATION.md:263 "tests/fault_injection (T5) does not exist until M2a creates it"; the tree has
tests/fault_injection/{verification,worker,workloads}.
Fix: delete the clause.
Resolved by #102: no.

### A12  CONFIRMED  low  side: document
Evidence: PLAN.md:1501 "all eight present" (ten workflows, release.yml and soak.yml not listed, plus
canary-2604/release-static); :1517 `○ bug-report.yml M1` (absent from .github/ISSUE_TEMPLATE, which holds two
templates); :1524-1527 docs tree marks architecture.md, telemetry-sources.md, result-schema.md, safety.md as
planned though all exist; ROADMAP.md, JOURNAL.md, hardware-validation.md, history/ unlisted.
Fix: redraw the §6 tree (see A7); decide on bug-report.yml (build or drop).
Resolved by #102: no.

### A13  CONFIRMED  low  side: document
Evidence: PLAN.md:1613, 1619-1626 draws fixtures/sysfs as intel, amd, arm64, no-hwmon, eacces, edac, wrapping;
actual tests/fixtures/sysfs is edac, fan, frequency, powercap, thermal (procfs: meminfo, stat). Also drawn and
absent: root benchmarks/, tools/capture-sysfs.sh, tools/gen-golden/, config/stress-5h.toml,
config/explore-temperature.toml (only config/quick.toml exists).
Fix: part of the §6 redraw (A7).
Resolved by #102: no.

### A14  CONFIRMED  low  side: document
Evidence: PLAN.md:3 "revision 14" (Q10, §4.6); IMPLEMENTATION.md:3 "revision 3 -- reconciled against the tree"; neither
describes anything after M1.
Fix: bump the revision lines with the A1/A6 changes.
Resolved by #102: no.

### B1  PARTLY (the claim holds; #102 resolves most of it)  high  side: document
Evidence: src/main/cli.cpp:1071-1123 dispatches `--worker`, `--rerun`, `--check-config`, `stress`, `explore`,
`journal`, `--version`/`-V`, `--selftest-digest`, `--help`/`-h`. No `selftest` or `benchmark` command.
Resolved by #102: mostly. #102 adds "not built" notes at PLAN.md:282 and PLAN.md:1461 and at verification.md §9,
and rewrites the issue template. Not covered: the CLI box at PLAN.md:1101 (`selftest | benchmark | stress |
explore`); IMPLEMENTATION.md §1 (28-48) which mentions neither selftest nor benchmark and no milestone
schedules them; the code comment at src/verification/golden/golden.hpp:20-24 ("selftest (§9)"); `benchmark`
anywhere (B2).
Fix (remaining): mark the PLAN.md:1101 box "selftest, benchmark: not built"; add a line to IMPLEMENTATION §1
"Explicitly not in 1.0: `loadforge selftest`, benchmark mode" (owner decision) or a roadmap item; change the
golden.hpp comment to "the selftest digest (--selftest-digest)".

### B2  CONFIRMED  medium  side: document
Evidence: `config::Mode::kBenchmark` parses (config.cpp:29,41) but src/main/cli.cpp:575-579 refuses any mode but
stress ("mode benchmark lands with a later milestone"). Nothing implements the benchmark discipline described at
PLAN.md:783-806, 883-886, 1161-1173. Explore takes flags, not a file (mode set internally, trial_runner.cpp:266);
PLAN.md:1626 `explore-temperature.toml` is not in the tree.
Fix: add a dated "not built" note to the benchmark paragraphs, as #102 did for selftest, and drop
explore-temperature.toml from the tree (A13).
Resolved by #102: no.

### B3  CONFIRMED  low  side: document (ahead of code)
Evidence: git grep for "non-comparable" and "contextual" in src/ and docs/result-schema.md: no match; the label
promised at PLAN.md:1168, 1173 is not produced.
Fix: "not yet labelled in the result" note, or build the label.
Resolved by #102: no.

### B4  CONFIRMED  high (for the risk register entry)  side: document until 34a lands
Evidence: PLAN.md §4.1 box ("Safety: limits | watchdog | emergency stop"), F13 (candidate emergency stop), risk
register (~1989, mitigation "controller safety"), and :521 present thermal protection; docs/safety.md:15-25 and
ROADMAP.md:166 (34a): the temperature ceiling is parsed and reported "NOT ENFORCED"; no emergency stop on
uncorrected ECC either. Duration/stop escalation and the memory ceiling are enforced.
Fix: add a dated note to the PLAN risk-register row and §4.1 ("temperature ceiling and ECC emergency stop: not
built until 34a; see docs/safety.md"). PLAN.md is not one of the four files, but the wording depends on how 34a
lands, so write it after 34a merges.
Resolved by #102: no.

### C1  CONFIRMED  medium  side: document
Evidence: src/workloads/fft/spec.hpp:98-103: "A round-trip (inverse transform) is not checked"; the oracle is a direct
DFT at sampled bins plus Parseval (oracle/fft/fft_oracle.hpp). verification.md:61 and PLAN.md:579 claim
round-trip.
Fix: "Direct O(n^2) DFT at sampled bins, plus Parseval; no round trip".
Resolved by #102: no.

### C2  CONFIRMED  medium  side: document
Evidence: src/workloads/sparse/spec.hpp:62-66: "Conjugate gradients without a preconditioner".
Fix: verification.md:62 and PLAN.md:580 -> "Conjugate gradients (no preconditioner)".
Resolved by #102: no.

### C3  CONFIRMED  medium  side: document
Evidence: src/workloads/graph/spec.hpp:102-112: the oracle recomputes the whole expected output; "It does not sample
a subgraph"; the triangle inequality is only a cross-check in tests.
Fix: "Whole-output scalar BFS and PageRank recomputation (exact, pinned arithmetic)" in verification.md:68 and
PLAN.md:586; adjust verification.md:170 (exact class).
Resolved by #102: no.

### C4  CONFIRMED  medium  side: document
Evidence: src/workloads/dense_linear/spec.hpp:14-29: the unit is C = A B only; the oracle is dot products on sampled
entries of C (oracle/dense_linear/dense_linear.hpp). No factorization, no solve, no ||Ax-b|| residual.
verification.md:60, 181-183 (example derivation) and PLAN F18 table (~840, `A'x = b`) pair it with a residual of a
solve.
Fix: pairing row -> "GEMM C = AB; dot products on sampled entries, tolerance derived"; keep the solve example
labelled as an illustration of the general hazard, not of this workload.
Resolved by #102: no.

### C5  CONFIRMED  low  side: document
Evidence: verification.md pairings table (56-69) has no adaptive-mesh row (grep of verification.md for "adaptive": none)
and no mixed row; particles row says "Barnes-Hut / neighbour lists" while src/workloads/particles/spec.hpp:24-31
explains Barnes-Hut was rejected for a cutoff neighbour search.
Fix: add the adaptive mesh row (bounded residual); particles -> "neighbour search with cutoff".
Resolved by #102: no.

### C6  PARTLY  medium  side: document
Evidence: verification.md:149-152 and PLAN.md:891-895 say control state is "controller-owned", holds seeds, indices,
tolerances and expected checksums, "re-validated on every use". Holds: owner and scope are wrong. The copies live
in the runner (worker process; src/verification/integrity/control_state.hpp, "one thread at a time, like the runner
that owns it") and cover seed, index, duty-cycle periods and line size only, not tolerances or checksums.
Does not hold as stated: "every use" is once per unit (docs/journal/02-input-integrity.md:135, JOURNAL.md:2670);
the header's own words "voted on every use" mean each unit's use, so the document is imprecise rather than false
on frequency. Placement rules (separate mappings, per-copy CRC-32, exclusion before voting) match; the
"more than one page apart" requirement (PLAN.md:902) is not enforced ("the kernel's choice", header).
Fix: "worker-owned (in the runner), covering seed, index, duty periods and line size; voted at the start of each
unit"; mark the DRAM-row separation as not guaranteed.
Resolved by #102: no.

### C7  CONFIRMED  low  side: document (ahead of code)
Evidence: git grep "transient" in src/: only the isolation finding (isolation.hpp:26, 196-209) and unrelated timeline
comments; no per-run transient rate or trend. verification.md:296-297 promises counting, rate tracking and correlation.
Fix: "a non-reproducible transient is one finding with telemetry attached; no run-level rate yet".
Resolved by #102: no.

### C8  CONFIRMED  low  side: document
Evidence: src/controller/isolate.hpp:36 `kDefaultIsolationLimit = 16`; explore never isolates (JOURNAL §2.56).
verification.md:239-243 "automatically re-runs the failing unit" states neither limit.
Fix: add "up to 16 wrong results per worker; not in explore".
Resolved by #102: no.

### C9  CONFIRMED  medium  side: document (three of the four)
Evidence: src/verification/record.hpp:20 "fields AT ALL -- not null ones, none"; result-schema.md writes only listed
members. PLAN.md:91, verification.md:379-380 and IMPLEMENTATION.md:530 say the bit fields are "null and reported
as not-applicable" and owe "a T5 asserting the null fields are null". result-schema.md is right.
Fix: "absent by construction (the type has no such fields)"; replace the T5 clause with "a test that the record has no
bit fields" or delete it.
Resolved by #102: no.

### C10  CONFIRMED  low  side: document
Evidence: src/verification/record.hpp:229-234 `BitClass { kSingleBit, kMultiBitInWord, kWithinCacheLine,
kAcrossCacheLines }`; verification.md:358-367 and IMPLEMENTATION.md:504 name three classes ("whole-cache-line") and say
the record holds "expected bytes, actual bytes, XOR delta". I did not re-open the `Difference` optional/64-byte-window
detail; it rests on the review's reading of record.hpp.
Fix: list the four classes; describe the record as a window of at most 64 bytes with offsets and a popcount.
Resolved by #102: no.

### C11  CONFIRMED  low  side: document and code comment
Evidence: same as E-F6 (no `tools/gen-golden/`; verification.md:97, PLAN.md:1624, golden.hpp:28).
Fix: as E-F6; do both in one change.
Resolved by #102: no.

### C12  CONFIRMED  low  side: document
Evidence: src/verification/record.hpp:149,162 list `physical_package` with `physical_core`; verification.md:334-341
omits it (also `decomposition`, `footprint_bytes` per the review, not re-read).
Fix: add the fields to the list.
Resolved by #102: no.

### D1-F (determinism.md:190-198; named D1 in review F)  PARTLY  low  side: document
Evidence: .github/workflows/portability.yml compares `loadforge --selftest-digest` across architectures; src/main/selftest.hpp
(15-33) folds the core corpus and the scalar paths of compression and dense-linear only. The cross-architecture check exists
and is real; it does not cover sort, fft, stencil, graph and the others, so "CI asserts it" (BitExact classes identical
across architectures) and "comparing its outputs" overstate coverage.
Fix: "CI compares a digest over the core corpus and the scalar paths of compression and dense_linear; extending it to
every workload is open".
Resolved by #102: no.

### D2-F (PLAN.md:1234-1235, determinism.md:160-165)  CONFIRMED  low  side: document
Evidence: `IsaPath` has kAvx512 (primitives/simd/dispatch.hpp:57) but dense_linear.hpp:52 says "No AVX-512 kernel exists, so
that path is always refused"; no SSE2 path; git grep for `rdtsc`, `cntvct`, `__rdtsc` in src/: none.
Fix: mark AVX-512, SSE2 and the rdtsc/cntvct timing row as not built (clocks are clock_gettime).
Resolved by #102: no.

### D3-F (determinism.md class assignments)  CONFIRMED (minor)  low  side: document
Evidence: Descriptors: compression and sort kBitExact; dense_linear, fft, stencil, sparse, particles, graph, monte_carlo,
raytrace kBitExactFixedDecomposition; adaptive_mesh kBoundedResidual (graph.cpp:361 sets the weaker class for the whole
workload, where determinism.md:51-52, 66-69 reads as split by output).
Fix: one sentence under the graph paragraph: "the workload is declared at the weaker class as a whole".
Resolved by #102: no.

### E1  CONFIRMED  medium  side: document
Evidence: PLAN.md:1303-1304 "the controller *is* multithreaded"; PLAN F9 (511-516) "workers are spawned before the
controller starts any background threads". git grep for `std::thread|pthread_create|std::jthread` in src/: only
src/worker/executor/thread_pool.cpp (worker side); the controller is one poll loop.
Fix: "the controller is single-threaded today (one poll loop); the rule applies if a thread is ever added".
Resolved by #102: no.

### E2  CONFIRMED  low  side: document
Evidence: IMPLEMENTATION.md:413 "writing the stop byte to a pipe nobody reads"; src/worker/control.hpp:14-21,
controller/orchestration.cpp:659 use framed control messages (`kControlFrameType`); JOURNAL §2.49 retired the stop byte.
Fix: "writing a control frame to a pipe nobody reads".
Resolved by #102: no.

### E3  CONFIRMED  low  side: document
Evidence: src/worker/entry.hpp:56-71 `RerunExit` 0, 20, 21, 22; IMPLEMENTATION.md:420-436 "the worker's exit codes, in one
place" lists 0, 119-125 only.
Fix: add the three re-run codes (and refer to src/main/outcome.hpp for the run's codes).
Resolved by #102: no.

### E4  CONFIRMED  low  side: document (design ahead of code)
Evidence: PLAN.md:399-408 "verify no swap is in use and abort if swap-in begins mid-run"; git grep "swap" in src/ finds
only the sampler fields (memory source) and unrelated comments; nothing aborts. Huge-page choice: the runner maps ordinary
pages (ADR 02), not a recorded choice.
Fix: a "not built" note on both sentences.
Resolved by #102: no.

### E5  CONFIRMED  low  side: document
Evidence: git grep "socket" in src/: topology only; PLAN.md:744 and the risk register (~1996) list the optional remote sink
as a mitigation.
Fix: "not built" note in the risk-register row.
Resolved by #102: no.

### E6  CONFIRMED (no defect; the review reports consistency)  none  side: none
Evidence: `--worker-executable` absent, as PLAN.md:1316-1323 says. No fix.
Resolved by #102: n/a.

### E7  PARTLY  low  side: document
Evidence: IMPLEMENTATION.md:35 "Twelve integrated workloads, each with an independent oracle and a declared determinism
class"; the registry holds eleven; `mixed` and `cycling` schedule them and have no oracle or class of their own.
README.md:105 words it correctly ("Twelve integrated reliability runs. Eleven are workloads; the twelfth is made of the
others"), so "twelve" as a count of runs is deliberate; the false part is "each with an independent oracle".
Fix: IMPLEMENTATION.md:35 -> "Eleven verified workloads, and a twelfth run (mixed, with cycling) that schedules them".
Resolved by #102: no.

### E8  CONFIRMED (line cited wrongly)  low  side: document
Evidence: the row is docs/platforms.md:135 ("Memory bandwidth | uncore IMC counters | ... | capability"), not 157-158.
git grep for `uncore` and `imc` in src/: isolation text only; no source reads uncore IMC. Only logical bytes are reported.
Fix: mark the row "not built" or move it to a planned list; PLAN F12 "measured DRAM bandwidth" likewise.
Resolved by #102: no.

### E9  CONFIRMED  low  side: document
Evidence: src/controller/results/document.cpp:317-324 writes `type, samples, delta, rate_per_second, resets` for a counter
summary; docs/result-schema.md:118 gives `{ type, samples, delta, rate_per_second | null }`.
Fix: add `resets` to the schema line.
Resolved by #102: no.

---------------------------------------------------------------------------------------------

# Grouping into work items

Confirmed or partly confirmed findings only (E-F6 and C11 are one fix; C1-C5 are one table).

## Group 1: fixes that touch README.md, docs/safety.md, docs/architecture.md or docs/telemetry-sources.md (wait for 34a)

One work item, "user documents: wording against the code", started after 34a merges (34a edits all four):
- D2: README.md:84-87 (delivered metrics).
- D3: safety.md:135 (OOM string); optional architecture.md:158-163 (fate names).
- D4: safety.md:226, README.md:215, architecture.md:215-216 (exit 0 and exit 6 wording).
- D7: README.md:330 (review-request link).
- D8: README.md:301 (memory check scope); the hardware-validation.md part is in Group 2.
- D9: README.md:139 (`static-headers`); the CONTRIBUTING part is in Group 2.
- D12: safety.md:139 (mixed footprint).
telemetry-sources.md: no confirmed finding touches it.
B4 (PLAN note on the unbuilt ceiling) touches PLAN.md only, but its wording depends on 34a; do it right after 34a.

## Group 2: fixes that do not touch those four files (can start now)

Work item A, release blocker (code): D1 CMake version string for an empty milestone, with a test; then fix nothing in
the documents (they state the intent). High.

Work item B, hardware-validation and packaging text: D5, D6, D10 (hardware-validation.md), the hardware-validation.md
part of D8, D13 (packaging/README.md).

Work item C, CONTRIBUTING: D9 (CONTRIBUTING.md:289), D11.

Work item D, PLAN.md and IMPLEMENTATION.md reconciliation (largest; one change): A1, A2, A3, A5, A6, A7, A8, A9, A10,
A11, A12, A13, A14, E1, E4, E5, E7, E2, E3, B2, B3, and the remaining B1 parts (PLAN.md:1101 box,
IMPLEMENTATION §1, golden.hpp comment). A4 is already done by #102. Rebase this on #102 (it edits the same files).

Work item E, verification and determinism text: C1-C5 (pairings table, also PLAN.md rows), C6, C7, C8, C9 (three documents),
C10, C12, D1-F, D2-F, D3-F, E8, E9 (result-schema.md), platforms.md E8 row.

Work item F, ADR numbering and amendments: E-F1 with E-F2, E-F3, E-F4, E-F5 (status, numbering, amendments, citation
updates in JOURNAL and journal/*), E-F6 with C11 (regeneration route; also verification.md:97, golden.hpp:28 comment,
PLAN.md tree), E-F7 (stream State lines), E-F8 (times in ADR prose, or an exemption in CONTRIBUTING).

## Already resolved by #102 (no further work)
A4 in full; B1 for verification.md §9, PLAN.md:282, PLAN.md:1461 and the issue template. A5 only partly (framed as history).
Not resolved by #102: every finding of reviews D and E, and everything else in review F.

## Rejected
None. Corrections to the reviews' details: E-F2 (CLI lock default is `try`, `kRequire` is the RunnerSetup default); E8 (row is
platforms.md:135, not 157-158); C6 ("every use" is once per unit, so frequency is imprecise rather than false); A8 (the
gate exists and reads tests/obligations.toml; only the §3 table is ungated); E7 (README's "twelve runs" is deliberate).

The project still needs an outside reviewer.
