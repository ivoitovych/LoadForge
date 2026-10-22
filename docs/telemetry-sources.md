# Telemetry sources

**Written:** 2026-10-22 22:30 UTC, against `main` at 97cf70e plus the release stream.

What LoadForge reads from the machine while it runs, where each reading comes from, what
makes it absent, and what you can do about it. The code is under `src/telemetry/`; the
contract every source keeps is `src/telemetry/source.hpp` and `src/telemetry/sample.hpp`.

## 1. The rule: a reading is a value or a reason

Availability varies by kernel, platform, privilege and virtualisation. Every source is
therefore **probed once** before the workers start, and every metric it could give is
reported in the **capability report** at the top of a run, either present or absent
with three things: **where** (the path or the interface), **why** (the kernel's own
words), and a **remedy** (what you can do, or that nothing can be done). A metric is
never read as zero because it could not be read: a reading is a gauge, a counter, or an
`Unavailable`, and an `Unavailable` in a result file is `null` with its reason beside it.

The six reasons are:

| Reason | Meaning | Whose problem |
|---|---|---|
| present | the value is here and parsed | |
| absent | this kernel or platform does not offer it, and never did | a fact about the machine, not an error |
| vanished | it was here, and has gone (a CPU taken offline, a device removed) | the machine changed under the run |
| denied | it is here and you may not read it | permissions: the remedy names the path or the capability |
| malformed | read, but not in a form this reader accepts | refused, never guessed |
| unreadable | present, permitted, and the read failed anyway | the device or the kernel |

A run that has less telemetry than another is a legitimate run, and the report says so.
It is not clean to compare two runs without their capability reports: the result file
records the sample interval and what was watching the run (`docs/result-schema.md`).

## 2. What is read, where from

Names are `<family>.<quantity>`; a series is the name plus a scope (machine, package,
core, CPU, device, memory controller) and an index. All reads are of files in `/proc`
and `/sys` except the two noted. The numbers in the last column are from the machine
this was written on (a four-vCPU virtual machine), where most sources are absent.

| Source | Metrics | Kind, unit | Read from | On the development VM |
|---|---|---|---|---|
| thermal | `thermal.package`, `thermal.core` | gauge, celsius | hwmon: `coretemp` ("Package id N", "Core N") and `k10temp` ("Tdie") | absent: no hwmon chip |
| | `thermal.sensor` | gauge, celsius | every other hwmon temperature input, named by its chip | |
| | `thermal.zone` | gauge, celsius | `/sys/class/thermal/thermal_zoneN/temp` | |
| fan | `fan.speed` | gauge, rpm | `/sys/class/hwmon/hwmonN/fanM_input` | absent |
| frequency | `frequency.cpu` (per online CPU) | gauge, hertz | `cpufreq/scaling_cur_freq` (kHz in the file) | absent: no cpufreq |
| | `frequency.busy_ticks`, `frequency.total_ticks` | counter, count | the aggregate line of `/proc/stat` | present |
| memory | `memory.total`, `.free`, `.available`, `.buffers`, `.cached`, `.swap_total`, `.swap_free` | gauge, bytes | `/proc/meminfo`, one read per round | present (7 of 7) |
| power | `power.package` (per package) | counter, microjoules | powercap `intel-rapl:N/energy_uj` first, then the `power` PMU's `energy-pkg` event through `perf_event_open` | absent: no RAPL |
| counters | `counters.cycles`, `.instructions`, `.cache_misses`, `.branch_misses` | counter, count | `perf_event_open`, hardware events, user space only | absent: no PMU |
| | `counters.context_switches` | counter, count | `perf_event_open`, software event | present |
| | `counters.pmu_share` | gauge, ratio | derived from the kernel's enabled and running times | absent |
| edac | `edac.corrected`, `edac.uncorrected` (per memory controller) | counter, count | `/sys/devices/system/edac/mc/mcN/{ce,ue}_count` | absent: no controller |

The development VM reports `10 of 24 metric(s) present`. The same command on another
machine reports its own count; the figure to look for on hardware is how many of the
thermal, frequency, power, counters and edac metrics are present.

### Notes per source

**Thermal.** Hardware monitors are identified by what the driver says they are, not by a
guess: only coretemp's "Package id N" and k10temp's "Tdie" are called `thermal.package`.
Every other hwmon temperature is `thermal.sensor` with its chip as the provider, and
every thermal zone is `thermal.zone` with the zone's type as the provider. The kernel's
zone and an hwmon chip may show the same die; they are kept apart and never merged.
**On a machine with neither coretemp nor k10temp (most ARM64 boards and many laptops)
there is no `thermal.package`.** The temperature, if the machine has one, is a
`thermal.zone` or a `thermal.sensor`; use that series for anything that asks for a
temperature, including the thermal procedure in `docs/hardware-validation.md`.

**Fan.** The label file is not read: whether an input is the CPU fan, a case fan or a
pump is the board's business and is often unlabelled. A reading of 0 is a stopped fan
and is a reading.

**Frequency.** `frequency.busy_ticks` is `user + nice + system + irq + softirq + steal`
and `frequency.total_ticks` adds `idle + iowait`; guest time is already inside user and
nice. The 64-bit counters wrap at 2^64. A busy share is computed from deltas of the two,
and is not reported when the interval cannot be told (a total that did not move, or
idle accounting that stepped back by a few ticks). Ticks are in `USER_HZ` (100 Hz on
this machine, so four busy CPUs read 400 ticks per second). The ticks exist on every
Linux machine, which is why the frequency family is half present on a VM.

**Memory.** One read serves every metric of a round, so a round's figures are from one
instant. A kernel without `MemAvailable` (before Linux 3.14) reports that one absent and
the rest present; no estimate stands in for a missing line.

**Power.** `/sys/class/powercap/intel-rapl:N/energy_uj` is mode 0400 root since
CVE-2020-8694, so the remedy for a denied powercap file is file access or privilege. The
perf provider's gate is different: `CAP_PERFMON` or a lower `kernel.perf_event_paranoid`.
The report labels the remedy of each provider that failed, and gives no advice when both
simply lack the interface. The counter is in microjoules and wraps at
`max_energy_range_uj`; the sampler reads it often enough that a wrap is never mistaken
for a smaller advance. Power is a *rate* of this counter: ask for `rate`, not `mean`.
Which machines offer either interface depends on the CPU generation and the kernel;
on most ARM64 machines expect it absent, and read the report rather than assume.

**Counters.** Counts the controller's main thread and every process it forks after the
probe, which are the workers. Hardware events count user space only, because that is
what an unprivileged process may count at `perf_event_paranoid` 2, and a worker's
work is its own instructions. The context-switch count must include the kernel (that is
where a switch happens), so it needs `CAP_PERFMON` or `perf_event_paranoid` 1 or lower;
without that it is **absent and denied**, never 0. With more events than the PMU has
counters, the kernel time-shares them; the raw counts are reported unscaled (an estimate
is not a counter) and `counters.pmu_share` says how much of the time they were on the
PMU. 1 means never shared; below 1 the counts are lower bounds. A virtual machine
usually has no PMU (`ENOENT` from `perf_event_open`), and the report says so with the
`perf_event_paranoid` level it read.

**EDAC.** Corrected and uncorrected memory errors per controller. The kernel keeps the
counts in 32 bits; a step down is a reset of the controller's counters and is counted as
one, not read as a wrap. The driver for a memory controller is missing on most consumer
boards, and no setting loads it, so the remedy for absence is "nothing can be changed
from the software side". **Corrected errors during a run make the verdict `corrected
errors` and the exit code 5**; a run in which the EDAC counter was blind for part of the
time is `incomplete` (exit 6), because the errors it was to count may have happened
there. On a machine with no EDAC a run can still be `clean`: that says the computations
were right, not that the memory reported no errors.

## 3. How the readings become numbers

The controller's sampler reads every source once per **round**, every
`--sample-interval` (1 s by default), stamps the round with one reading of
`CLOCK_MONOTONIC` and appends it to a bounded timeline. The workers' heartbeats use
the same clock. The sampler's own cost is reported in the run summary:

```
telemetry: 4 round(s) every 1.000 s, 40 point(s) over 10 series; sampling took 0.001 s (0.0619 % of one CPU)
```

The timeline is sized before the run from the duration, the interval and the series
count. A run that outgrows it stops sampling and says so; it never silently drops the
oldest points. After the run each series is summarised over the load window:

- a gauge: samples, min, max, mean, standard deviation, first, last, slope per second;
- a counter: samples, delta, rate per second (a wrap is advanced across; a reset is
  counted and treated as a gap, not as an advance).

The summaries, the capability report, the machine's fingerprint (CPU model, kernel,
microcode, SMT state, governor, power source and others; each one `null` with a reason
when unreadable) and every finding are written to `run.json` (and `run.csv`) with
`--output json,csv --output-dir DIR`. The layout is `docs/result-schema.md`.

## 4. Seeing what your machine offers

The capability report is printed first in every `stress` run. The shortest run that
prints it:

```
$ loadforge stress --test sort --duration 2s 2>&1 | sed -n '/^telemetry capability/,/^loadforge /p'
telemetry capability: 10 of 24 metric(s) present
  thermal: 0 of 1 present
    thermal.package (package 0): not offered by this kernel
      where:  /sys/class/hwmon
      why:    no hwmon chip with a temperature input and no thermal zone
      remedy: this machine exposes no temperature sensor to read; in a virtual machine or a container that is the platform's choice and nothing inside it can change that, while on bare metal the platform's hwmon driver (coretemp on Intel, k10temp on AMD) may not be loaded
...
```

On a machine with sensors the same lines read `present:` with the metric names. To see
what the kernel offers without running a workload, look at the files directly:

```
ls /sys/class/hwmon/*/name /sys/class/thermal/thermal_zone*/type
cat /sys/class/hwmon/hwmon*/name
ls /sys/class/powercap/ /sys/devices/system/edac/mc/
cat /proc/sys/kernel/perf_event_paranoid
```

## 5. When a source you expect is absent

| Symptom in the report | Usual cause | What to try |
|---|---|---|
| no `thermal.package`, on a laptop or PC | `coretemp` / `k10temp` not loaded | `sudo modprobe coretemp` (Intel) or `k10temp` (AMD) |
| no thermal at all, on ARM64 | no thermal zone in the device tree, or the driver is not in the kernel | check `ls /sys/class/thermal`; nothing LoadForge can do |
| `power.package` denied | powercap files are root-only | run as root, or read access to `energy_uj` |
| `power.package` absent | no RAPL or `power` PMU (older AMD, most ARM64, a VM) | nothing can be changed |
| `counters.*` denied | `perf_event_paranoid` too high, or no `CAP_PERFMON` | `sudo sysctl kernel.perf_event_paranoid=1`, or run privileged |
| `counters.*` absent (`ENOENT`) | virtual machine without a PMU | nothing can be changed |
| `edac.*` absent | no EDAC driver for the memory controller | nothing can be changed |
| `frequency.cpu` absent | no cpufreq driver, a VM, or the CPU is offline | nothing can be changed |

The remedy in the report is the authoritative one for your machine; this table is the
common case. A changed sysctl or a loaded module is a change to the machine under
test, and a run records the governor and similar settings in its fingerprint but not the
sysctl.
