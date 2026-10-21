# Journal fragment 09 — telemetry sources A: thermal, fan, frequency, memory, and the capability probe

**Written:** 2026-10-20 13:05 UTC
**Change:** `src/telemetry/sources/{thermal,fan,frequency,memory}`, `src/telemetry/capability`,
their tests under `tests/unit/telemetry/`, the fixtures under `tests/fixtures/{sysfs,procfs}`,
and the ledger fragments under `tests/obligations.d/telemetry/`.

## Established by experiment

### This machine offers no sensor, no fan and no cpufreq, so the "present" trees are built

The machine this stream ran on (a Firecracker virtual machine, `Linux 6.18.44-fc-v139 x86_64`,
4 vCPUs) was inspected for anything the sources read:

```
$ ls /sys/class/hwmon /sys/class/thermal
ls: cannot access '/sys/class/hwmon': No such file or directory
/sys/class/thermal:                                  (empty)
$ ls /sys/devices/system/cpu/cpu0                     (no cpufreq directory)
$ ls -la /sys/devices/system/cpu/cpufreq              (an empty directory)
$ find /sys -name 'temp*_input' -o -name 'fan*_input' -o -name scaling_cur_freq   (nothing)
$ cat /sys/devices/system/cpu/online
0-3
```

PLAN F3 measured the same of the development container. So the committed trees are what the
machine really has (`tests/fixtures/sysfs/{thermal,fan,frequency}/absent`, with the command in
`tests/fixtures/sysfs/README.md` and a `CAPTURED` note in the empty ones), and
`tests/fixtures/procfs/{stat,meminfo}/present` are `cp` of the real files. A present hwmon, thermal
zone or cpufreq tree cannot be captured here: the tests build them (`hwmon_tree.hpp`, marked
`LOADFORGE P7 FIXTURE BUILDER`) in the layout of the kernel's `Documentation/hwmon/sysfs-interface.rst`
(`name` and `*_label` one line of text, every `*_input` one decimal integer and a newline) and do not
claim they are captures. What would replace them: a tree copied from a machine with `coretemp` or
`k10temp`, an `nct6775` fan chip and cpufreq (`cp -r /sys/class/hwmon/hwmon*/{name,*_input,*_label}`,
taken with the command recorded), added beside `absent`. The builders keep their value as the way to
reach denied, malformed and vanished, which no committed tree can hold.

### `/proc/stat` on this kernel, and what the counters sum to

The captured aggregate line is `cpu  4677 0 1418 192173 462 0 224 244 0 0`: user 4677, nice 0,
system 1418, idle 192173, iowait 462, irq 0, softirq 224, steal 244, guest 0, guest_nice 0. Guest
ticks are already counted inside user and nice (proc(5)), so `frequency.busy_ticks` is
user + nice + system + irq + softirq + steal (6563 here) and `frequency.total_ticks` adds idle and
iowait (199198). Adding guest again would double count on a host that runs virtual machines.

### A multiplication by 0.001 is not the nearest double; a division by 1000 is

`20016 * 0.001` is `20.016000000000002` in a double and `20016 / 1000.0` is `20.016`. Over the
millidegree values 20000 to 99999 the multiplication differs from the division for 10654 of the
80000. The first version of `Channel` multiplied. It now carries a multiplier and a divisor (a
temperature divides by 1000; a frequency multiplies by 1000, exact in a double up to the bound).

### A std::string-bearing aggregate built in one expression leaves a branch no test can take (§1.13 again)

The first full coverage run over this stream was 100% of lines and functions and 99.2% of branches:
38 branches on 12 source lines, every one of them the cleanup for a half-built object that follows a
`call N never executed` in `gcov -b -c`. The lines were all of three shapes:

- `Metric{literal, ..., hwmon_provider(chip)}` and `HwmonChip{std::move(dir), index, name.value()}`,
  an aggregate with two allocating members;
- `statuses.push_back(MetricStatus{memory_metric(field), std::move(unavailable)})`, a temporary
  aggregate pushed;
- `text += "..." + a + "..." + std::to_string(n)`, and `missing.push_back(Group{...})`, in the
  capability renderer.

§1.13's two fixes (hoist the second allocation; declare `noexcept` where it is true) do not reach
these: the first leaves one object with two members built in the same expression, and the second is a
lie for a function that allocates. What does work, measured here: a **named local, or an object
that is already complete, being assigned to**. So the sources default-construct and assign
(`make_metric` fills a `Metric` field by field), add to a vector with `emplace_back()` and fill the
reference it returns, and move into an aggregate only strings that are already named locals. The
renderer writes to a `std::ostringstream`, whose destructor is a call and not a branch. After the
change the five modules measure 100% line, 100% function and 100% branch (506 lines, 58 functions,
493 branches) with no exclusion.

The rule worth carrying, which refines §1.13: **a cleanup branch comes from a partly built
aggregate or a temporary aggregate, not from a named local string.** `std::string detail = ...;
return Unavailable{kind, path_, std::move(detail)};` (topology's `Source::text`) has none;
`Unavailable{kind, a(), b()}` has one.

### A temperature range cannot catch every plausible lie

`Channel` refuses a raw reading outside a range as malformed. For a temperature the range is
physics: absolute zero (-273150 millidegrees) to one thousand degrees. A chip that reports a fixed
`-128000` for "not connected" (some SMBus sensors do) passes it, because -128 degrees is above
absolute zero. The first version's comment said the range caught `-128000`; it did not, and the test
that tried it failed. The comment now says what the bounds are. Catching such a chip needs the chip
to be named (a per-driver sentinel list), which this stream has no machine to verify against (F21),
so it is left open (below).

### `FileSystem::read_first_line` reads one line, and an empty online list parses

A value file holding `50000\n7000` reads as 50000: the second line is not looked at. That is the
kernel's format (one value, one newline) and the sources rely on it. `CpuList::parse("")` succeeds
with an empty list, which is right for `offline` (every CPU up) and wrong for `online`, where it
would mean the process runs on no CPU. The frequency source refuses an empty online list as
malformed rather than reporting a machine with no CPUs.

### The source suite's header makes every including binary fail unless it instantiates the timeline suite too

`tests/contracts/telemetry/telemetry_contract.hpp` defines `SourceContract` and `TimelineContract`
in one header. A binary that includes it and instantiates only the first fails GoogleTest's own
`UninstantiatedTypeParameterizedTestSuite<TimelineContract>` check. Each test file here says
`GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(TimelineContract);` in `loadforge::contracts`, as the
failure message instructs. See the contract questions.

### clang-tidy needs a clang configure

`-Wduplicated-cond`, `-Wlogical-op`, `-Wuseless-cast` are GCC-only and `compile_commands.json` from a
GCC configure makes clang-tidy report them as errors on every file. CI configures with clang for the
tidy job (`ci.yml`); a local run needs `CC=clang-18 CXX=clang++-18 cmake --preset debug -B build/tidy`
and `clang-tidy-18 -p build/tidy`.

## Decisions

### The sources' tests substitute the platform seam with an in-memory /sys and /proc

*Options:* (a) trees on disk read through `RealSyscalls`, with `FakeSyscalls` aimed at one file for
denial; (b) the shared `FakeSyscalls` alone; (c) the sources' own `platform::Syscalls` that holds a
machine's files in memory.

*Chosen:* (c) (`tests/unit/telemetry/sources/thermal/memfs.hpp`). A source reads dozens of files in
one probe, and (b) serves one body for every path. (a) cannot hold a denied file (a mode-000 file
arrives from a clone readable, and the tests run as root: JOURNAL §2.7), a file that vanishes
mid-run (an event, not a state) or an errno only a driver returns (EIO, ENODEV). With (c) every state
is one call (`remove`, `fail_open`, `fail_read`, `make_directory`) on a machine loaded from the
captured trees (`load()`), and each test says what it changed. The model is as strict as the kernel
for what the sources use: ENOENT for a path that is not there, a directory opens and fails the first
read with EISDIR (§1.1), a closed descriptor is EBADF. The real kernel is covered by the same suite
instantiated over `RealSyscalls` on the machine the tests run on (`RealMachine`), so a reading the
model has wrong fails there. The shared fake is not read, in keeping with ADR 0003.

### hwmon is discovered by name, not by listing the directory

`platform::FileSystem` reads files; it does not list directories, and `src/platform/syscalls.*`
belongs to no telemetry stream. Discovery asks for `hwmon0` to `hwmon63`, for `temp1` to `temp255`
and `fan1` to `fan255` of each chip it finds, and for `thermal_zone0` to `thermal_zone63`. The
kernel hands out hwmon numbers lowest-free-first, so a machine with a chip past 63 has sixty-four
sensor chips; the inputs follow the sysfs interface document's limit of 255. A chip, input or zone
beyond a bound is not seen and the bound is a constant a later change can raise. *Rejected:*
`std::filesystem::directory_iterator`, which would work and bypass the seam, leaving the P2 paths of
discovery (a denied directory, one that vanishes while listed) untestable.

### A sensor is named only by what its driver says

`coretemp`'s `Package id N` and `Core N` and `k10temp`'s `Tdie` are `thermal.package` and
`thermal.core`; every other input is `thermal.sensor` with scope device, the chip as provider
(`hwmon:nct6775`) and a series index that encodes chip and input (`chip << 8 | input`), so two
chips' `temp1` are distinct series. A label that is not exactly the driver's (`core 0`, `Core x`,
`Core 4294967296`, `Package id`) is a sensor, not an error and not a guess. `k10temp`'s `Tctl` is not
the package: it carries an offset on some parts, and calling it one would put an offset into a column.
When two inputs claim one series (two chips with a `Package id 0`) the first keeps the name and the
second is reported as a sensor. *Rejected:* matching label text for unknown chips (`acpitz`, `nvme`,
`iwlwifi`): F3 says identification is the hard part, and an unverified rule is the kind of confident
statement the capability model exists to avoid.

### Thermal zones are `thermal.zone`, beside hwmon and not merged with it

On some machines a zone and an hwmon chip report one sensor. They are kept distinct, with the
zone's `type` in the provider, because deduplicating them would mean comparing live values, which is
a guess.

### A fan at zero rpm is a reading

`0` is a stopped fan (fan.speed 0 is a finding a reliability tool wants); absence is a state of the
file, never a value. Labels are not read: which fan is the CPU's is the board's business.

### Frequency reports counters, not a utilisation gauge

`frequency.busy_ticks` and `frequency.total_ticks` are counters of the aggregate line, with no
modulus (an unsigned long wraps at 2^64 on a 64-bit kernel; a 32-bit kernel's narrower wrap would
read as a step back, and says so). The sampler differences counters with `counter_delta` and knows
the sampling interval; a gauge computed in `sample()` would need the previous reading, which makes
the source stateful in a way the contract does not give it. `utilisation(previous, current)` is the
share, for the consumers that want one. A step back (the kernel sometimes reconciles idle ticks by a
few) is not a wrap: an advance past 2^63 is a step back, a total that did not move or a busy share
above the total is "cannot be told", and none of them is a number. Per-CPU utilisation (the
`cpuN` lines) is not read: two counters per CPU on a 256-CPU machine is the cost F17 warns about,
and nothing consumes it yet.

### An empty online list is malformed; a CPU's cpufreq is read for the CPUs in the list at the probe

A CPU that goes offline mid-run vanishes (its cpufreq file goes), which is what `kVanished` is for.
The list is not re-read: the metric set is fixed at the probe, as the contract says.

### Memory: seven fields from one read, with a parser of its own

`memory.{total,free,available,buffers,cached,swap_total,swap_free}` from one read of the file per
round, so one round's figures are from one instant (`MemAvailable` and `MemFree` read a second apart
can be out of step by the allocation in between). `src/resources` exposes
`parse_mem_available`, which returns the one key's value in bytes for the memory budget; this source
needs seven keys and a per-metric state (a kernel without `MemAvailable` reports that one absent and
the rest present), so it parses the lines itself, with the same strictness (`<Key>: <number> kB`,
digits only, a key that appears twice is malformed, the largest kB count a byte count can carry is
accepted and one more is not) and reuses the part that is public and general, `resources::parse_decimal`.
No estimate stands in for a missing line.

### The capability report groups absences by name, state and remedy

256 CPUs without cpufreq are one entry with `x256`, not 256 lines. Metrics are grouped only when name,
state and remedy are all equal, so a sensor denied on one chip and denied for a different reason on
another stay two entries. The printed `where` and `why` are the first member's. The probe is
`probe_capabilities(span<Source*>)` and calls `probe()` once on each source and no other method; the
sampler that follows uses the same objects. The sources tolerate a second `probe()` (it starts from the
machine as it is then), because the contract does not forbid one and a probe-twice bug is cheap to
prevent.

### Every source runs SourceContract once per state and once over the real machine

Present, absent, denied, malformed, vanished (and unreadable for thermal; wrapped for frequency, where
the counters wrap while the suite samples) are separate subjects, each with its own tests of which
state was reported. A `RealMachine` subject reads this machine's own /sys and /proc through
`RealSyscalls`: the real-against-real tier of ADR 0003, which found nothing in this stream but is the
one place a wrong model in `MemFs` would show.

## Contract questions

1. **Two suites in one header.** `telemetry_contract.hpp` defines `SourceContract` and
   `TimelineContract` together, so a binary that instantiates one fails GoogleTest's verification for
   the other. Reading taken: each source's tests add `GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(TimelineContract);`.
   Proposed: split into `source_contract.hpp` and `timeline_contract.hpp`, or put the allowance in
   the header.
2. **Is a metric that failed at the probe retried at `sample()`?** The contract says a metric
   "present at the probe" is read each round and "a metric that comes back after vanishing is read
   again", and says nothing of a metric that was denied or malformed at the probe and has recovered.
   Reading taken: the set is fixed at the probe and an unavailable metric is not sampled. A later
   change that wants a recovered sensor reported must re-probe.
3. **What is the series of a metric when its whole source is unavailable?** A source that finds
   nothing "reports the metrics it looked for as unavailable". For thermal and fan that is one status
   (`thermal.package`, `fan.speed`, index 0); for frequency without an online list it is
   `frequency.cpu` index 0. Reading taken: one status for the metric the user would have expected, with
   the path searched and the remedy. The contract does not say which, and a consumer that wanted
   per-CPU absences cannot get them when the CPU list itself is missing.
4. **`Unavailable.path` for an absence of a whole class.** The thermal absent status names the
   directory searched (`/sys/class/hwmon`), not a file, because no file was ever found. The contract
   says `path` is "where"; this is the reading taken.
5. **Series index for a device with no natural number.** `Scope::kDevice` with a `scope_index`
   that encodes `chip << 8 | input` is stable on one machine across runs, but is not a number a reader
   can interpret without the provider. The contract fixes only that (name, scope, index) identifies a
   series. If reports are to show "hwmon3 temp2", the index or the metric needs a label field.
6. **Counter that wraps at 2^64 has no modulus; the contract's `counter_delta` handles it, but a
   32-bit counter read on a 64-bit value does not.** `/proc/stat` on a 32-bit kernel wraps at 2^32 and
   the source cannot tell; the contract has no way to say "unknown modulus". Not a problem on the
   machines LoadForge targets (64-bit), recorded for the day it is.

## Left open

- A real hwmon, thermal-zone, fan and cpufreq tree captured from a machine that has them (see the
  first section).
- A per-driver list of the sentinel values a chip reports for "not connected" (`-128000` and its
  kin), which needs those chips to verify against.
- Per-CPU utilisation, and `intel_pstate`'s `cpuinfo_cur_freq` (needs root, so it cannot be the
  default) as a second provider for `frequency.cpu`.
