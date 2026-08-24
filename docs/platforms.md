# Supported platforms and components

**Normative.** Records what LoadForge is developed against, what it is supported on, and
how it adapts to versions it was not developed against.

---

## 1. The strategy: develop against what is practical, adapt on demand

LoadForge is developed against **current, practical versions** of every component, and
adapts to other versions **when a real need appears** — not speculatively, in advance,
for every version that might ever matter.

This is a deliberate choice and it is the right one, but only because of a property the
architecture already has. Adaptation is cheap *when the variable thing sits behind an
interface*, and expensive when it is an assumption baked into the design. The plan's
platform boundary (F3) and capability model already put every version-varying thing —
kernel interfaces, sensor layouts, telemetry availability — on the cheap side of that
line.

So the rule is not "defer everything." It is:

| Safe to defer — behind an interface, adapt on demand | **Must be right from the start** — cannot be retrofitted |
|---|---|
| Kernel version and interface differences | Memory-ordering discipline (§4.5) |
| Distribution and sensor layout variation | Cache-line and topology assumptions (§4.5) |
| Newer compilers and library versions | Floating-point environment contract (F15) |
| Telemetry source availability | Determinism scoping (F1) |
| glibc / userspace ABI (dissolved by the static build, F19) | The platform boundary itself (F3) |
| Additional architectures beyond x86-64 / ARM64 | Verification-input integrity (F18) |

Everything in the right-hand column shares one property: it is not a *version* question
but a *design* question, and discovering it late means rewriting working code. Everything
in the left-hand column is a version question, and the architecture already absorbs it.

---

## 2. Audit of the development platform

Measured directly on the machine this project is being developed on, rather than assumed:

```text
distro     Ubuntu 24.04.4 LTS
kernel     6.18.44          x86_64
glibc      2.39             symbols to GLIBC_2.39
libstdc++  GLIBCXX_3.4.33
gcc        13.3.0
clang      18.1.3
cmake      3.28.3
ninja      1.11.1
valgrind   3.22.0
cpus       4                Intel Xeon
```

C++20 library support on GCC 13 was verified feature-by-feature rather than assumed. All
of `<format>`, `jthread`, `atomic_wait`, `barrier`, `latch`, `span`, `bit_cast`, `endian`,
`hardware_interference_size`, `ranges` and `source_location` are present. **C++20 modules
are not usable** in this toolchain generation; the project uses classic headers.

**Absent and required from CI:** `gcovr`, `lcov`, `perf`, `mull`. None are needed to
build; all are needed for the gates.

**Absent telemetry — the fact that shapes the architecture:** no `hwmon`, no `cpufreq`,
no RAPL, no EDAC, and `perf_event_paranoid=2`. The development and CI environment exposes
no real sensors at all, which is why every telemetry source is fixture-driven (F3).

---

## 3. Supported components

**Legend for *Adaptation*:** how LoadForge copes with a version other than the one it was
built against.
· **capability** — probed at runtime, absence is first-class (F3)
· **interface** — behind the platform boundary; a new backend, no core change
· **build** — a build-system or CI matrix change
· **design** — cannot be adapted late; must be correct from the start

### Toolchain

| Component | Developed against | Minimum supported | Adaptation | Notes |
|---|---|---|---|---|
| C++ standard | C++20 | C++20 | design | No modules — classic headers |
| GCC | **13.3.0** | 13 | build | Both compilers first-class in CI |
| Clang | **18.1.3** | 18 | build | Required for `llvm-cov` and sanitizers |
| CMake | **3.28.3** | 3.28 | build | Matches the 24.04 baseline |
| Ninja | 1.11.1 | any | build | |
| libstdc++ | GLIBCXX_3.4.33 | 3.4.33 (dynamic) / **n/a** (static) | build | Static build removes the floor |

### Runtime platform

| Component | Developed against | Minimum supported | Adaptation | Notes |
|---|---|---|---|---|
| Architecture | **x86-64**, **ARM64** | both first-class | design | SVE and 32-bit ARM out of scope |
| Distribution userspace | Ubuntu 24.04.4 LTS | **independent**, via static build | build | Userspace only — see the note below |
| glibc (dynamic build) | **2.39** | 2.39 | build | A 24.04-linked binary cannot start on 22.04 (F19) |
| Host glibc (static build) | 2.39 | **no host requirement** | build | No NSS/DNS/`dlopen`, so static linking is clean — verified |
| Kernel | 6.18 (dev host) | **5.15** | capability | A floor for *telemetry expectations*, not a hard block |
| cgroup | v2 | v2 | capability | v1 detected and reported unsupported for limits |

> **What the static build does and does not solve.** It removes the **host glibc and
> libstdc++ ABI requirement** — nothing more. Kernel ABI, CPU ISA and architecture
> requirements still apply in full: the static x86-64 artifact will not run on ARM64, and
> a kernel too old to expose an interface still cannot expose it. "Independent of
> distribution userspace" is the precise claim; "runs on any Linux" is not.

### Test and build dependencies

| Component | Version policy | Licence | Linked into binary? |
|---|---|---|---|
| GoogleTest + GMock | Pinned revision, system fallback, `BUILD_TESTS=OFF` path | BSD-3-Clause | **No** — test only |
| toml++ | Vendored, pinned, header-only | MIT | Yes — vendored, not a third-party *dependency* |
| gcovr | CI-installed | BSD | No |
| mull | CI-installed, nightly | Apache-2.0 | No — Ubuntu AArch64 packages exist, so the ARM64 leg is covered |

All GPL-compatible (§5.1). Third-party **runtime** dependencies remain zero.

### Telemetry interfaces

Every row is capability-probed. Absence is a reported state, never an error.

| Source | Path / mechanism | Typical access | Adaptation |
|---|---|---|---|
| CPU utilisation | `/proc/stat` | unprivileged | interface |
| Memory | `/proc/meminfo`, cgroup v2 `memory.max` | unprivileged | interface |
| Frequency | `/sys/devices/system/cpu/*/cpufreq/` | unprivileged | capability |
| Temperature (x86) | `/sys/class/hwmon/` — `coretemp`, `k10temp` | usually unprivileged | capability |
| Temperature (ARM) | `/sys/class/thermal/thermal_zone*/` | usually unprivileged | capability |
| Fan | `/sys/class/hwmon/` | usually unprivileged | capability |
| Package power — sysfs | `/sys/class/powercap/intel-rapl/.../energy_uj` | **root** (`0400` since CVE-2020-8694) | capability |
| Package power — PMU | `power/energy-pkg/` via `perf_event_open` | `CAP_PERFMON` | capability |
| PMU counters | `perf_event_open` | `CAP_PERFMON`, `perf_event_paranoid` | capability |
| Memory bandwidth | uncore IMC counters | usually root; absent on many parts | capability |
| ECC / RAS | `/sys/devices/system/edac/` | unprivileged where present | capability |
| Topology | `/sys/devices/system/cpu/`, `node/` | unprivileged | interface |

Note the two independent paths to package power, with **different failure modes and
different fixes** — the reason remediation advice is per-provider (F3).

---

## 4. Forward compatibility: Ubuntu 26.04 as a canary, not a target

Ubuntu 26.04 LTS now exists, and GitHub offers `ubuntu-26.04` and `ubuntu-26.04-arm`
runners. It is a large jump:

| | 24.04 (reference) | 26.04 |
|---|---|---|
| GCC | 13.3 | **15.2** |
| glibc | 2.39 | **2.43** |
| Kernel | 6.8 | **7.0** |

**The reference platform stays 24.04**, for three reasons:

1. **The users are the point.** LoadForge diagnoses hardware, and the machines that most
   need diagnosing run older software, not newer. Targeting the newest LTS optimises for
   the wrong population.
2. **24.04 is supported to 2029** and is the broadly deployed LTS. 26.04 is months old.
3. **The static build already removes the pressure.** F19 means the runtime floor is not
   set by the build host, so there is no compatibility reason to chase the newest.

One status note that reinforces the decision: **the `ubuntu-26.04` and `ubuntu-26.04-arm`
runner images are currently Public Preview**, not stable images. Gating on a preview
runner would be unwise regardless of the toolchain jump.

**So a 26.04 job runs in CI as a non-blocking canary.** GCC 15 will produce diagnostics
GCC 13 does not, and with `-Werror` those would be build failures — so the job warns
rather than gates. This is the "adapt on demand" strategy with an early-warning signal
attached: the adaptation work is *visible* before it becomes urgent, rather than
discovered when someone finally needs to build on a newer distro.

One live operational note: GitHub's 24.04 and 22.04 runner images are currently being
migrated to internal pipelines and are frozen for updates during the transition. Worth
watching, not worth acting on.

---

## 5. Changing anything in this document

The tables above are the contract. Raising a minimum version, adding an architecture, or
adding a telemetry source is a reviewed change to this file, made together with the
change that requires it — never as a silent consequence of a build that happened to work
on someone's machine.
