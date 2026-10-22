# Journal fragment 34 — hardening and 1.0 (M7)

**Written:** 2026-10-22 22:09 UTC
**Change:** `docs/architecture.md`, `docs/telemetry-sources.md`, `docs/safety.md`, `docs/hardware-validation.md`; `packaging/` (the release build, the package, the machine validation script); `.github/workflows/release.yml`; `README.md`, `CONTRIBUTING.md`. No source under `src/` and no file of another stream was changed.

## What is done, what waits for the owner

Done and shown to work in this container (a four-vCPU x86-64 VM, Ubuntu 24.04, no sensors): the documents; the release build of both artifacts; the Debian package built, installed, run and purged; the machine validation script run against a debug build and against the unpacked static artifact; the release workflow written, parsed, and its action pins checked. **Not done, and not doable here:** M7's exit criterion itself (validation on several x86-64 and ARM64 machines and on live media, with real sensors), the T11 thermal result left open by stream 33, the soak on hardware, and anything on ARM64: no ARM64 machine was available, so neither the ARM64 build, the ARM64 package nor the `ubuntu-24.04-arm` legs of the workflow have been run by me. `docs/hardware-validation.md` is what the owner executes; its results go to `tests/hardware/`.

## Established by experiment

### The temperature limit in a configuration is not enforced, and the message that says so is stale

`limit_statuses` (`src/controller/safety.cpp`) reports `max_temperature_c` as `enforced = false` with the note "not enforced until telemetry lands (M3)". Telemetry landed with M3; nothing compares a reading with the limit. Run on this commit with `[limits] max_temperature_c = 85`:

```
  limit duration: enforced (3s)
  limit memory: enforced (checked at start against a ceiling of 14005748KiB)
  limit temperature: NOT ENFORCED (not enforced until telemetry lands (M3); configured 85 C)
```

`stress --test` has no temperature option. The README's earlier safety notice said the suite "will include configurable temperature ceilings, memory limits, watchdogs, and emergency stop"; memory limit, watchdog and a deadline exist, the temperature ceiling and an emergency stop do not. The documents now say so (`docs/safety.md` §1, README). This is the one known defect I found that bears on M7's "no known defect", and it is a safety gap rather than a wrong result. It needs a decision (open threads, below).

### A controller killed by any signal leaves no worker behind, and writes no report

Run in this container, a 60 s `stress --test sort` (four workers):

| Signal to the controller | Exit status of the controller | Workers alive 1 s later (`pgrep -x loadforge`) | Report / result file |
|---|---|---|---|
| `SIGINT` after 3 s | 130 | 0 | none |
| `SIGTERM` after 3 s | 143 | 0 | none |
| `SIGKILL` after 7 s, with `--journal` | 137 | 0 | none; the journal reads `ended abnormally between records; 2 entries` and the next start keeps it as `FILE.1` |

The controller installs no handler for `SIGINT` or `SIGTERM` (`grep` over `src/controller` and `src/main` finds only `ignore_signal(SIGPIPE)` and the escalation signals it sends), so Ctrl-C is the default action. The workers die by the parent-death signal (`SIGKILL`, `src/platform/process.hpp`). My first attempt to measure this launched the controller with `setsid`, which forks, so `$!` was the dead parent of the `setsid` call and the `kill` hit nothing; the controller kept running to its 60 s. It is the shape of journal §4.12: I read "workers after: 6" and nearly believed the signal had not worked. The measurements above use the controller's own pid.

### `stress --test` and `stress --config` allow a unit ten seconds, not one or two

The workers' command line shows `--stale-after-ms 10000`; `src/main/cli.cpp` sets the staleness allowance and the stop grace of any run with a verifying worker to 10 s (`kVerifyingStaleAfterNs`, `kVerifyingStopGraceNs`), and keeps the M1 defaults (2 s, 1 s) for the null workload. A run can therefore end about 11 s after its duration (10 s grace, then 1 s from `SIGTERM` to `SIGKILL`). `docs/safety.md` §2 states it.

### The result directory that is not there is a refusal (exit 1), not a write failure (exit 4)

`loadforge stress --test sort --duration 1s --output json --output-dir /nonexistent/x` exits 1 with `cannot write the result: "/nonexistent/x" is not a directory`, before any worker starts: `FileResultSink::prepare` creates and removes `.loadforge-write-test` in the directory first. Exit 4 is a write that fails after the run. The checklist's scripted step expects 1.

### `bash`'s `command -v` reads its hash table: the first run of the package test failed for a real reason

`packaging/test-deb.sh` checked `command -v loadforge` after `dpkg --purge`, and the first run stopped with "loadforge is still on the PATH after purge" while `ls /usr/bin/loadforge` said it was gone. `command -v` answers from bash's command hash after the first lookup. `hash -r` before each lookup fixed it. The test that failed here is the evidence that the check can fail; the second run exited 0.

### The dynamic binary asks for little: glibc 2.34, libstdc++ 13.1

`dpkg-shlibdeps` on the Release build: `Depends: libc6 (>= 2.34), libgcc-s1 (>= 4.3), libstdc++6 (>= 13.1)`. `objdump -T` finds symbol versions up to `GLIBC_2.34` and `GLIBCXX_3.4.32`. The real floor of the dynamic build is therefore libstdc++ from GCC 13, not glibc 2.39; Ubuntu 24.04 is where it is built and tested, and the first thing to fail on an older userspace is `GLIBCXX_3.4.31` (Ubuntu 22.04 and Debian 12 carry libstdc++ 12). Q11's "24.04 is the minimum" stands as the supported statement; the measured floor is recorded so nobody reads the package's glibc line as the limit. **Not tested here:** that the refusal on an older system reads as predicted (no older system available); `docs/hardware-validation.md` §5.5 asks the owner to record the message.

### The self-test digest is the same across build types

`loadforge --selftest-digest` printed `e0ff849ff34906de` for the debug build, the unpacked static Release artifact and the unpacked dynamic Release artifact. ARM64 is the comparison that matters and is the owner's to make.

### The scripted validation, run here

`packaging/validate-machine.sh --workload-seconds 5` against the debug build: every step passed in 2 min 13 s (eleven workloads, `mixed`, `cycling`, journal, refused output directory, a small `explore`, an `explore` refused for an unknown metric, and a controller killed with `SIGKILL` with zero workers left of four). Against the unpacked static artifact, with the soak logic exercised for two minutes at a ten-second sample (`VALIDATE_SOAK_UNIT=m`, `VALIDATE_SOAK_POLL=10`): it passed all its steps, and the soak step (mixed workload, two minutes, a sample every ten seconds, the controller's resident memory 4976 to 5056 KiB, descriptors 14 to 14, threads 1 to 1, verdict clean) passed. A real soak of hours has not been run; the growth criteria are untested against growth, because nothing here grows.

### Build time

`packaging/build-release.sh` (static and dynamic Release builds, both artifacts, the package, the checks) took about four minutes on four vCPUs, from a clean `build/static` and `build/release-dynamic`. Sizes: static binary 5.9 MB, dynamic 3.5 MB (stripped), `.deb` 1.2 MB, tarballs 2.6 MB and 1.6 MB.

## Decisions

### D1. Packaging form: a Debian package from the release binary, plus tarballs of both flavours

**Options.** (a) CMake `install()` rules and CPack's DEB generator; (b) a script that stages the built binary and runs `dpkg-deb`; (c) both; (d) RPM, AppImage, Snap or Flatpak as well; (e) tarballs only.

**Chosen: (b), with tarballs of both flavours.** The project's declared platform is Ubuntu (`docs/platforms.md`), and the artifact that matters beyond it is the static binary, which needs no package. (a) needs `install()` rules in the top-level `CMakeLists.txt` and a CPack configuration, both outside this stream's files; it would also package whatever the configured build tree is, where the release must be a Release, tests-off build that has been read back with the project's gates. (b) touches nothing shared, builds the package from the very binary that the gates inspected and the tarball carries, and computes `Depends` from that binary with `dpkg-shlibdeps`. (d) multiplies the matrix of things the owner must validate on hardware for no stated need; each can be added from the same tarball later. (e) alone leaves Ubuntu users without `apt remove`. The tarball of the dynamic flavour is kept beside the package for a machine without `dpkg`, and so that a live-media run can use the same binary as the package without installing it.

**Smallest change to a shared file: none.** No `CMakeLists.txt`, preset, `cmake/` or `tools/` file was edited. The dynamic build uses the Release configuration by command line (`-DCMAKE_BUILD_TYPE=Release -DLOADFORGE_BUILD_TESTS=OFF`) rather than a new preset in `CMakePresets.json`.

This does not meet ADR 0001's test for an ADR (it is reversible and local to `packaging/`), so there is no proposed ADR.

### D2. One script builds the release, for CI and for the owner

`packaging/build-release.sh` is the whole of the build; `release.yml` calls it. The alternative, the steps written out in the workflow as `release-static.yml` does, would have given the owner a second, unverified copy to run by hand on a machine without CI. The script's own gates are the project's: `tools/check-no-test-hooks.sh` and `tools/check-runtime-deps.sh` run on the unstripped binary, because a stripped one would be refused rather than waved through (as `release-static.yml` notes), and then the *tarball* is unpacked and run, because the artifact is what ships, not the file it was copied from.

### D3. A new `release.yml`, and `release-static.yml` left as it is

The brief gives the stream "the release workflow". Two readings: edit the existing `release-static.yml` (builds the static artifact on every push and pull request), or add the tag-driven one. I added `release.yml` (tag push, manual rehearsal, and a pull request that touches `packaging/`) and left `release-static.yml` untouched: it is the per-change guard that the static build cannot rot (F19), changing its triggers would change what CI requires of every pull request. The cost is that a tag builds the static artifact a second time; negligible.

### D4. The release is a draft, and it is created by `gh release create`

M7 asks for validation on hardware before a release is real, so the workflow only drafts it (`--draft --verify-tag`) and the owner publishes by hand. The workflow uses the `gh` that GitHub's runners carry, not a third-party release action, so no new action enters the pin list (`licensing.yml` requires each to be a commit SHA; the three used are the existing, pinned `checkout`, `upload-artifact` and `download-artifact`). The `publish` job is the only job with `contents: write`.

### D5. The tag must equal the version the binary prints; the version is not bumped here

The tree still prints `0.0.0-m2` (`project(VERSION 0.0.0)` and the `m2` milestone suffix in the top-level `CMakeLists.txt`). Choosing 1.0.0 is the owner's release act, and the file is not this stream's; but a tag that the binary contradicts would ship an artifact that says another version than its name. The workflow refuses such a tag, and `packaging/README.md` says to set the version on the release commit. The package version maps the first `-` to `~` so a pre-release sorts before its release.

### D6. No signing of artifacts

Checksums only (`SHA256SUMS`, checked on the machine where the artifact runs). Signing needs a key the owner holds; the repository's rule is that commit signing stays off because the container's key carries the wrong identity. A detached signature or a signed checksum file can be added to the draft by the owner.

### D7. Where hardware results go, and how they are named

`tests/hardware/m7-LABEL-FLAVOUR-YYYY-MM-DD-HHMM-UTC.md`, one per machine per artifact per run, never overwritten; extra tags for the thermal, soak and power-cut results; one summary `m7-summary-<stamp>.md`. It follows the one existing file there (`m2b-thermal-2026-10-20-2212-UTC.md`) and the repository's rule for dated, recurring documents. `validate-machine.sh` writes the file, so the owner's work is the part only a person can do: the medium, the cooling, the temperatures seen, what looked odd.

### D8. What the scripted validation can and cannot say

It checks exit codes and that every verifying run ends `verdict: clean`; it does not call a machine good. Its sections for the medium and the observations are left for the owner, and the soak uses growth criteria that are fixed in the script (resident memory within 10% plus 4 MiB of the sample five minutes in, descriptors and threads not growing), so that "no growth" is not a judgement made at three in the morning. The thermal procedure is not scripted: the explore command is long, takes hours and is read, not pass/failed.

### D9. The thermal procedure on a machine with no `thermal.package`

Stream 33's T11 text names `thermal.package`. On most ARM64 machines, and on any with neither coretemp nor k10temp, the only temperature is a `thermal.zone` or `thermal.sensor` series (`src/telemetry/sources/thermal/thermal.cpp`). The checklist therefore tells the owner to read the series from the capability report and to use `thermal.zone@N` where that is what exists, and says that a machine with no temperature cannot give the result and must not be given a substitute number. `--settle` and `--objective` both accept `NAME@INDEX` (checked: the parse is accepted; the unavailable-metric refusal reads `objective metric thermal.zone (index 0) is not in the capability report`).

### D10. The documents state what the code does, including what it does not do

`docs/safety.md` opens with the missing temperature ceiling, and the README's safety notice was rewritten from "will include" to what exists and what does not. The alternative, leaving the aspiration standing until the feature is built, would have left a 1.0 README promising a protection the tool does not give.

## Contract questions

No contract header is touched. Disagreements between the repository's documents and the tree, found while writing, none of which I was able to correct:

1. **The hardware-failure issue template asks "Did `loadforge selftest` pass?"** (`.github/ISSUE_TEMPLATE/hardware-failure.yml`). There is no such command; the tree has `--selftest-digest` only. `CONTRIBUTING.md` (mine) now says so and names what to run instead; the template is not mine.
2. **`docs/PLAN.md` §4.1 and `docs/verification.md` §9 describe `benchmark` and `selftest` as commands.** Neither exists in `src/main/cli.cpp`. The README's former "planned operating modes" listed `benchmark`; the README now lists the commands that exist.
3. **`src/controller/safety.cpp` says "not enforced until telemetry lands (M3)".** Stale; see above. The text is the tool's own output and the test that pins it is not mine.
4. **`docs/PLAN.md` §4.6 says `--worker-executable PATH` "lands when a live-medium user first needs it".** The static artifact is for live media. A live medium without `/proc` refuses to run (no fallback by design). If the owner meets such a medium in the validation, that is the user the note was waiting for; the checklist records the refusal message if it happens.
5. **`ci.yml` runs ShellCheck over `tools/*.sh tools/lib/*.sh tests/tools/*.sh` only.** `packaging/*.sh` pass `shellcheck` (default severity; run here, exit 0) but no gate reads them. Adding `packaging/*.sh` to that step would close it; `ci.yml` is not mine.
6. **`docs/IMPLEMENTATION.md` §10 and the README still describe M1 as in progress.** I rewrote the README. §10 is the manager's.

## Open threads

- **The temperature ceiling (needs a decision, probably from the owner).** Build it (a limit read from the timeline each tick, a stop with its own fate and exit code, the limit in `stress --test`, tests of every path) or declare it out of 1.0 and say so in the release notes. Until then M7's "no known correctness defect" is true of wrong results and not of the safety notice's original promise.
- **A graceful stop on `SIGINT` and `SIGTERM`**, so that Ctrl-C writes the report. Today it writes nothing.
- **The version.** The release commit sets 1.0.0 and the milestone suffix; the workflow refuses a tag that disagrees.
- **ARM64.** Nothing here ran on ARM64. The scripts avoid architecture assumptions (`uname -m`, `dpkg --print-architecture`), and the ARM64 legs of `release.yml` will be their first run. Expect the first tag rehearsal (`workflow_dispatch`) to find something.
- **The hardware half of M7** (section above), and the **outside reviewer**: the project still needs one.
- `docs/ROADMAP.md` should show stream 34 as built and waiting for hardware; the file is the manager's.
