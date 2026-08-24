# LoadForge

**A lightweight, composable, self-verifying hardware reliability and load-testing framework for Linux.**

LoadForge builds complete, realistic computational workloads to stress the whole
CPU–cache–memory complex, continuously verifies that the machine still computes
*correctly* under that stress, and records how the physical machine reacted —
power, temperature, frequency, throttling, bandwidth — on a single common timeline.

---

## Project status

> **Pre-alpha — M0 (foundation) complete. No workloads yet.**

| | |
|---|---|
| **Phase** | M0 done; M1 (core framework + supervision) next |
| **Code** | Build system, CI, config primitives, CLI skeleton |
| **Tests** | 62, at 100% line and branch coverage |
| **Public API** | Not defined |
| **Usable** | Not yet — no workloads until M2a |

What exists is the foundation: CMake with seven presets, eight CI workflows across
{x86-64, ARM64} × {GCC 13, Clang 18}, the 100% coverage gate, and the first core module.
The workloads themselves begin at M2a. See [`docs/PLAN.md`](docs/PLAN.md) for the
milestone sequence.

| Document | What it is |
|---|---|
| [`docs/PLAN.md`](docs/PLAN.md) | The development plan and design of record: a critical review of the draft, the architecture, the directory layout, the testing strategy, milestones and risks |
| [`docs/DESIGN-DRAFT.md`](docs/DESIGN-DRAFT.md) | The original project description — a **working draft**, preserved verbatim; parts are superseded by the plan |
| [`docs/determinism.md`](docs/determinism.md) | The determinism contract — normative, required reading before touching numerical code |
| [`docs/verification.md`](docs/verification.md) | The verification doctrine — oracles vs golden vectors, fault injection, fault isolation |
| [`docs/platforms.md`](docs/platforms.md) | Supported components, the platform audit, and what may be adapted later versus what must be right now |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | How to contribute, and the three rules that are not obvious |
| [`LICENSE`](LICENSE) | GPL-3.0-or-later |

Start with `docs/PLAN.md` if you want to know where the project is going and why.

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

## Planned test suite

Twelve integrated reliability workloads:

| # | Workload | Dominant character |
|---:|---|---|
| 1 | Dense linear algebra | FP / SIMD / FMA, compute-bound |
| 2 | Sparse solver | Memory latency, irregular access |
| 3 | FFT / spectral transform | Shifting locality, transposes |
| 4 | Finite-difference / PDE stencil | Sustained RAM bandwidth |
| 5 | Particle / N-body | Mixed FP, irregular, dynamic |
| 6 | CPU ray tracing | Branch-heavy, unpredictable control flow |
| 7 | Sorting / merge / database-style | Integer, writes, TLB pressure |
| 8 | Compression / integrity pipeline | Integer, bit manipulation, exact round-trip |
| 9 | Graph / network analysis | Coherence traffic, pointer chasing |
| 10 | Monte-Carlo simulation | Parallel bursts + synchronized reductions |
| 11 | Adaptive mesh / dynamic structures | Continuously changing workload shape |
| 12 | **Dynamic mixed-system test** | Different workloads on different cores, rotating |

Beneath these sits a layer of small primitive kernels (integer, FP, SIMD, cache,
memory, branch, atomic) used for calibration, diagnosis, and automatic workload
construction — not as the user-facing reliability tests.

That table is the target suite, not the build order. **Compression is built first**,
because its exact byte-for-byte round-trip (`decompress(compress(x)) == x`) proves the
verification machinery against the simplest possible contract before any floating-point
subtlety enters. Dense linear algebra follows, and proves the bounded-tolerance contract.

---

## Planned operating modes

```bash
loadforge benchmark                        # short controlled characterization
loadforge stress --hours 5                 # long reliability verification
loadforge explore --objective temperature  # search for the worst-case workload
```

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

**Reference platform: Ubuntu 24.04 LTS.**

- **Linux on x86-64 and ARM64** — both first-class. Individual telemetry sources are
  probed at startup and degrade gracefully when unavailable.
- **GCC 13.3 or Clang 18.1** (C++20). Both first-class — CI builds with each.
- **CMake 3.28+**, Ninja.
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

LoadForge is designed to drive hardware to sustained maximum power and thermal load.
On a system with inadequate cooling, a failing fan, degraded thermal paste, or an
unstable overclock, that is exactly the condition most likely to expose the problem —
which is the point, but it carries real risk. The suite will include configurable
temperature ceilings, memory limits, watchdogs, and emergency stop, and it will never
intentionally rely on undefined behaviour or unsafe hardware register manipulation.
Use it deliberately.

---

## License

**GPL-3.0-or-later.** See [`LICENSE`](LICENSE) for the full text.

Every source file carries `SPDX-License-Identifier: GPL-3.0-or-later`, and every
vendored component is checked for GPL compatibility in CI.

---

## Contributing

**Contributions are welcome — issues, forks and pull requests.** See
[`CONTRIBUTING.md`](CONTRIBUTING.md).

The project is at the planning stage, so the most useful contribution right now is
review of [`docs/PLAN.md`](docs/PLAN.md) — particularly the open questions at the end.

Two things worth knowing before you write code, because they are not obvious and are
strictly enforced: **an implementation is never verified by re-running itself** (every
workload has an independently derived reference oracle), and **every atomic operation
states its memory order explicitly**. Both rules exist because a bug in LoadForge would
be indistinguishable from the hardware fault it claims to have found.

Sign off your commits with `git commit -s` — the project uses the
[DCO](https://developercertificate.org/), not a CLA.

If LoadForge tells you your machine computed something wrong, please file it using the
**hardware failure** issue template. That is the report this project exists to receive.
