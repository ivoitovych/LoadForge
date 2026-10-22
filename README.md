# LoadForge

**A lightweight, composable, self-verifying hardware reliability and load-testing framework for Linux.**

LoadForge builds complete, realistic computational workloads to stress the whole
CPU–cache–memory complex, continuously verifies that the machine still computes
*correctly* under that stress, and records how the physical machine reacted —
power, temperature, frequency, throttling, bandwidth — on a single common timeline.

---

## Project status

> **1.0 candidate. M0 to M6 are merged; M7, hardware validation, waits for real machines.**

| | |
|---|---|
| **Phase** | Everything through M6 is on `main`. M7 (this document set, packaging, the release workflow and the hardware-validation checklist) is built; its exit criterion, validation on real x86-64 and ARM64 machines and on live media, needs the owner's hardware and is not yet met. The queue is [`docs/ROADMAP.md`](docs/ROADMAP.md) |
| **Version** | The binary reports `0.0.0-m2` until the release commit sets the version; a release tag must equal what `loadforge --version` prints |
| **Commands** | `stress` (eleven verified workloads and two scheduled runs), `explore` (worst-case search), `journal`, `--check-config`, `--version`, `--help`, `--selftest-digest` |
| **Tests** | 100% line, branch and function coverage with zero exclusions, gated in CI; a per-module test-obligation ledger, also gated; mutation testing, weekly |
| **Known gaps** | No temperature limit is enforced ([`docs/safety.md`](docs/safety.md) §1). No `benchmark` mode and no `selftest` command. No graceful stop on Ctrl-C. No outside reviewer has seen the whole tree |

| Document | What it is |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | How it is put together: controller and workers, a run in order, verification, results |
| [`docs/telemetry-sources.md`](docs/telemetry-sources.md) | What is read from the machine, from where, and what makes it absent |
| [`docs/safety.md`](docs/safety.md) | What the tool protects, what it does not, and what to do before a run |
| [`docs/hardware-validation.md`](docs/hardware-validation.md) | The checklist for validating a release on real machines and live media |
| [`packaging/README.md`](packaging/README.md) | Building the release artifacts and the Debian package |
| [`docs/PLAN.md`](docs/PLAN.md) | The development plan and design of record: a critical review of the draft, the architecture, the directory layout, the testing strategy, milestones and risks |
| [`docs/IMPLEMENTATION.md`](docs/IMPLEMENTATION.md) | The build order to 1.0: what each milestone delivers, the module ledger, and the doctrine that makes "every execution path is tested" decidable rather than aspirational |
| [`docs/JOURNAL.md`](docs/JOURNAL.md) | The engineering journal: facts established by experiment (several contradict the obvious reading of the manual pages), decisions and the alternatives rejected, and the mistakes that earned a structural fix |
| [`docs/result-schema.md`](docs/result-schema.md) | What `run.json` and `run.csv` hold |
| [`docs/determinism.md`](docs/determinism.md) | The determinism contract: normative, required reading before touching numerical code |
| [`docs/verification.md`](docs/verification.md) | The verification doctrine: oracles vs golden vectors, fault injection, fault isolation |
| [`docs/platforms.md`](docs/platforms.md) | Supported components, the platform audit, and what may be adapted later versus what must be right now |
| [`docs/DESIGN-DRAFT.md`](docs/DESIGN-DRAFT.md) | The original project description: a **working draft**, preserved verbatim; parts are superseded by the plan |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | How to contribute, and the three rules that are not obvious |
| [`LICENSE`](LICENSE) | GPL-3.0-or-later |

Read `docs/safety.md` before your first run. Read `docs/JOURNAL.md` before writing code
that touches the kernel, the toolchain, or a CI gate; it records the things that were
wrong until an experiment corrected them.

---

## The idea

Many stress tools report the same thing:

```text
CPU utilization: 100%
```

That number says almost nothing. Two workloads can both pin every core at 100%
while producing radically different physical conditions inside the machine — one
drawing 72 W and heating the die at 4.8 °C/s, the other drawing 49 W but
saturating DRAM and the cache-coherence fabric.

LoadForge is built to answer the questions that actually matter:

- Which workload causes the highest package power?
- Which heats the silicon fastest, and which reaches the highest equilibrium temperature?
- Which stresses DRAM and the memory controller hardest?
- Which generates the most cache-coherency traffic?
- Which triggers the most thermal or power throttling?
- **And can this machine execute all of them correctly, for hours, without a single wrong bit?**

---

## What makes it different

**Real algorithms, not busy loops.** Reliability tests are complete computational
workloads — dense linear algebra, FFT, PDE stencils, graph traversal, ray tracing —
each exercising the whole hardware complex but with a *different distribution of
pressure* across it.

**Self-verifying computation.** A test does not merely produce heat. It continually
proves the hardware is producing correct results, using residuals, mathematical
invariants, exact checksums, and round-trip identities. The goal is to catch silent
data corruption, not just crashes and hangs.

**Load Signature, not a percentage.** Every test emits a multidimensional record of
hardware pressure — power, energy, frequency, peak and equilibrium temperature,
temperature rise rate, memory bandwidth, IPC, cache and branch miss rates,
throttling time, verification errors.

**Deliberate workload diversity.** There is no single universal maximum-load program.
A memory-latency workload leaves execution units idle; a compute-dense SIMD workload
barely touches DRAM. LoadForge ships roughly a dozen whole-system workloads with
different dominant characteristics.

**Automatic worst-case search.** Rather than assuming in advance which instruction
sequence is worst, LoadForge can search the space of workload mixtures to find what
stresses *your particular machine* hardest.

**Lightweight and native.** C++20 and Linux interfaces. No Python runtime, no JVM,
no heavyweight numerical framework, no large dependency graph.

---

## The workloads

Twelve integrated reliability runs. Eleven are workloads; the twelfth is made of the others.

| # | `--test` name | Dominant character |
|---:|---|---|
| 1 | `dense_linear` | Dense linear algebra: FP / SIMD / FMA, compute-bound |
| 2 | `sparse` | Sparse solver: memory latency, irregular access |
| 3 | `fft` | FFT / spectral transform: shifting locality, transposes |
| 4 | `stencil` | Finite-difference / PDE stencil: sustained RAM bandwidth |
| 5 | `particles` | Particle / N-body: mixed FP, irregular, dynamic |
| 6 | `raytrace` | CPU ray tracing: branch-heavy, unpredictable control flow |
| 7 | `sort` | Sorting / merge / database-style: integer, writes, TLB pressure |
| 8 | `compression` | Compression / integrity pipeline: integer, bit manipulation, exact round-trip |
| 9 | `graph` | Graph / network analysis: coherence traffic, pointer chasing |
| 10 | `monte_carlo` | Monte-Carlo simulation: parallel bursts + synchronized reductions |
| 11 | `adaptive_mesh` | Adaptive mesh / dynamic structures: continuously changing workload shape |
| 12 | `mixed` | **The dynamic mixed-system run**: workloads rotate across the workers and the workers across the CPUs, every unit still verified |

`cycling` is a thirteenth name, not a workload: thermal and power cycling of one workload
by level of duty or by number of working cores.

Every workload has an independent reference oracle, a declared determinism class, and
fault-injection tests. Beneath them sits a layer of small primitive kernels (integer,
floating point, SIMD, cache, memory, branch, atomic) used by the workloads.

---

## Build, test, install, run

```bash
cmake --preset debug && cmake --build --preset debug -j"$(nproc)"
ctest --preset debug -j"$(nproc)"
build/debug/loadforge --version
```

Presets: `debug`, `release`, `asan`, `tsan`, `coverage`, `simd-scalar`, `static`.

**Install.** Release artifacts are built by `packaging/build-release.sh` (and, for a tag,
by the `release` workflow) and come in two forms, with checksums, for x86-64 and ARM64:

- the **static** build, a single file that needs nothing from the system, for live and
  rescue media and for any Linux older than Ubuntu 24.04;
- the **dynamic** build, as a tarball and as a Debian package, for **Ubuntu 24.04 or later**.

```bash
sudo apt install ./loadforge_VERSION_ARCH.deb          # dynamic, Ubuntu 24.04+
tar -xzf loadforge-VERSION-linux-ARCH-static.tar.gz    # static, anywhere
```

See [`packaging/README.md`](packaging/README.md).

**Run.**

```bash
loadforge stress --test sort --duration 2s             # one verified workload, a worker per CPU
loadforge stress --test mixed --duration 1h --journal run.journal --output json --output-dir out
loadforge stress --config config/quick.toml            # a run from a configuration file
loadforge --check-config config/quick.toml             # check a configuration without running it
loadforge explore --objective frequency.busy_ticks:rate:higher \
    --candidate one:sort:1 --candidate two:sort:2 --duration 4s \
    --replicates 1 --final-replicates 2 --reference-replicates 1
loadforge journal run.journal                          # how a previous run ended
```

A run prints, first, which telemetry this machine offers and why anything is absent; then
a banner of what each worker will do; then the supervision of the run; then the verdict:

```text
loadforge 0.0.0-m2: stress for 2s with 4 worker(s)
  workload sort: 1 thread(s) on cpu 0, footprint 32MiB, 12 unit(s) resident, scalar path
  ...
[2.017 s] deadline reached: stop requested
run ended after 2.238 s: 4 of 4 worker(s) completed
  worker 0 (pid 7452): completed; exited with code 0; progress 5; stop requested
    verification: 5 unit(s) computed; the oracle checked 5 of them over 655360 sampled block(s) and found 0 wrong, 0 not checked; the input bracket covered 5 unit(s) and found 0 wrong
    findings: 0 wrong result(s), 0 check(s) not performed
verdict: clean
```

**Exit codes** (`loadforge --help` has the same list):

| Code | Meaning |
|---:|---|
| 0 | clean: everything was checked and nothing was wrong |
| 1 | refused: the configuration was refused, or the run could not start |
| 2 | usage: the command line is wrong |
| 3 | the run ended with a worker that did not complete |
| 4 | a result file could not be written |
| 5 | a fault: a wrong result, or an error the hardware reported, corrected or not |
| 6 | a check was not performed: a run that checked nothing is not clean |

When several apply, the first of 5, 4, 3, 6 is the code. `explore` uses 0, 1, 2, 3, 5 and 6
for the same things, per candidate. The verdict is one of `clean`, `corrected errors`,
`incomplete`, `findings`.

`--selftest-digest` prints a hash over a fixed corpus; for one version it must be the same
on x86-64 and ARM64 (`docs/determinism.md`).

---

## Scope

**In scope for the first version:** CPU execution units, the full cache hierarchy,
the memory subsystem and controller, virtual-memory machinery, the multicore
interconnect and coherence fabric, and platform-level behaviour (power, frequency,
thermal, cooling).

**Deliberately out of scope for now:** SSD testing (the suite actively minimizes
persistent writes), discrete GPU testing (keeps LoadForge usable on live media and
systems without proprietary drivers), and generic PCIe saturation.

---

## Testing and quality commitment

This is a tool whose entire purpose is to tell you the truth about your hardware.
A false "verification error" destroys its value as surely as a missed one. The
project therefore treats its own test suite as a first-class deliverable:

- **Independent reference oracles.** Every workload has a second, deliberately simple
  implementation derived from the mathematical definition. A blocked, vectorized GEMM is
  never verified by calling a blocked, vectorized GEMM again — that proves determinism,
  not correctness.
- **Fault injection** — every verification path has a negative test that deliberately
  corrupts data and asserts the corruption is caught *and classified correctly*.
- **Determinism tests** — each workload declares a determinism class, and the tests
  enforce exactly what was declared.
- **Out-of-process supervision.** A reliability tester should expect its workers to die.
  A controller process survives worker crashes and reports the telemetry leading up to
  them — which is the most valuable output the tool can produce.
- **Fixture-driven platform tests** — synthetic `/sys` and `/proc` trees, so telemetry
  parsing is testable on machines (and CI runners) that expose no real sensors.
- **Sanitizers**, including ThreadSanitizer, on a heavily multithreaded codebase.
- **100% line and branch coverage**, gated in CI from the first commit. Code and its
  tests land in the same change — there is no later campaign to lift coverage.
- **Mutation testing** as the primary quality metric, because at 100% coverage the
  coverage number can no longer tell a thorough test from an empty one.

The full strategy, including coverage targets and the test tier model, is in
[`docs/PLAN.md`](docs/PLAN.md).

---

## Requirements

**Reference platform: Ubuntu 24.04 LTS.** Ubuntu 24.04 is the declared minimum for the
dynamically linked build and the Debian package; the static build runs on older systems
and on live media.

- **Linux on x86-64 and ARM64** — both first-class. Individual telemetry sources are
  probed at startup and degrade gracefully when unavailable.
- **To build:** GCC 13.3 or Clang 18.1 (C++20), both first-class and CI builds with each;
  CMake 3.28+; Ninja. To run a release artifact you need none of these.
- **No third-party runtime dependencies.** The small amount of third-party code used is
  vendored and header-only; test-only dependencies are pinned and never linked into the
  shipped binary.

CI runs the full matrix — **{x86-64, ARM64} × {GCC, Clang}** — plus sanitizer jobs, a
scalar-only portability build, a cross-architecture determinism check, licence and DCO
checks, and a static release artifact. Running on ARM matters for more than
portability: ARM64 is weakly ordered where x86-64 is total-store-ordered, so a
memory-ordering bug that x86 would hide shows up there. In a tool like this that is
worth a great deal, because such a bug is indistinguishable from the hardware fault the
tool claims to have found.

---

## Safety notice

LoadForge drives hardware to sustained maximum power and thermal load. On a system with
inadequate cooling, a failing fan, degraded thermal paste, or an unstable overclock, that
is exactly the condition most likely to expose the problem, which is the point, but it
carries real risk.

What it does: a hard deadline for every run; a memory check before the workers start; a
heartbeat watchdog that stops a hung worker; workers that die with the controller; a crash
journal that survives a power cut; and no writes to the machine's configuration (it reads
`/proc` and `/sys`, and writes only the files you ask for).

What it does **not** do: **it does not stop a run because the machine is too hot.** A
configured `max_temperature_c` is read and reported as not enforced. Ctrl-C ends the run
at once and writes no report. Watch the temperature yourself, start short, and do not run
it unattended on a machine whose cooling you have not seen work. The full account is
[`docs/safety.md`](docs/safety.md).

---

## License

**GPL-3.0-or-later.** See [`LICENSE`](LICENSE) for the full text.

Every source file carries `SPDX-License-Identifier: GPL-3.0-or-later`, and every
vendored component is checked for GPL compatibility in CI.

---

## Contributing

**Contributions are welcome — issues, forks and pull requests.** See
[`CONTRIBUTING.md`](CONTRIBUTING.md).

The most useful contribution right now is **review**: nothing here has had an outside
reviewer yet. The current review request, the newest
`reviews/review-request-*.md` ([this one](reviews/review-request-2026-10-20-0352-UTC.md)), lists what to look at,
area by area, with the files and the questions, and how to report what you find.

Two things worth knowing before you write code, because they are not obvious and are
strictly enforced: **an implementation is never verified by re-running itself** (every
workload has an independently derived reference oracle), and **every atomic operation
states its memory order explicitly**. Both rules exist because a bug in LoadForge would
be indistinguishable from the hardware fault it claims to have found.

Sign off your commits with `git commit -s` — the project uses the
[DCO](https://developercertificate.org/), not a CLA.

If LoadForge tells you your machine computed something wrong, please file it using the
**hardware failure** issue template. That is the report this project exists to receive.
