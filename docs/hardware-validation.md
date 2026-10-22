# Hardware validation checklist (M7)

**Written:** 2026-10-22 23:05 UTC
**Milestone:** M7 (`docs/IMPLEMENTATION.md` §4). **Status of the work it asks for: not yet done.** This is
the document the owner executes; the results go in `tests/hardware/` (section 7).

M7's exit criterion is: *multi-machine validation complete on x86-64 and ARM64; no known
correctness defect; both release artifacts validated on live media.* Nothing in the build
can establish it, because it needs machines with real sensors, real boot media and real
heat. The development machine of this project is a virtual machine with no sensors. What
can be prepared has been: the artifacts are built by one script, one script runs the
scripted half of this list on a machine and writes the results file, and this document is
the rest. What the owner does is in sections 3 to 6; the pass criteria are in section 8.

## 1. What is validated, and against what

| Claim | How it is checked here | Section |
|---|---|---|
| Both artifacts run on x86-64 and on ARM64 | the scripted steps, on each machine, for each artifact | 4 |
| The static artifact runs on live and rescue media whose userspace is older than Ubuntu 24.04 | the scripted steps, from a live medium | 5 |
| The dynamic artifact (package) works on Ubuntu 24.04 and later, and is the minimum (Q11) | install, run, remove | 4 |
| Every workload runs clean under real load for a real time | the scripted steps, 60 s each | 4 |
| The machine's sensors are read, and an absent one is told with its reason | the capability report, by eye | 4 |
| x86-64 and ARM64 agree where the contract says they must | the self-test digest | 4 |
| Worst-case discovery is repeatable on real hardware, with variance reported (M6, T11) | the thermal procedure | 6.1 |
| Nothing grows over hours: memory, descriptors, threads | the soak | 6.2 |
| The crash journal survives what it is for | the power-cut procedure | 6.3 |

## 2. The machines

At least **three x86-64 and two ARM64**, as different from each other as you have: Intel
and AMD; a laptop (throttles early, has a battery) and a desktop or server; a machine
with ECC memory if one is available (it is the only way to see `edac.*` present); a
single-board computer for ARM64 and, if you can, a server-class ARM64 machine. Machines
are named by a **label** you choose (`thinkpad-t14`, `rpi5-a`): letters, digits and `-`.

Fill this table first and keep it in the summary (section 7); one row per machine.

| Label | Arch | CPU | Cores/threads | RAM, ECC? | Cooling | Sensors expected (hwmon driver, RAPL, EDAC) | Live medium used |
|---|---|---|---|---|---|---|---|
| | | | | | | | |

For each machine, **both artifacts**, and at least one machine of each architecture also
from a **live medium** (section 5). A machine that runs only Ubuntu 24.04 cannot show what
the static artifact is for; use an older live image for that.

## 3. Getting the artifacts

The release workflow builds both artifacts for both architectures when a version tag is
pushed and attaches them, with checksums, to a **draft** release (`packaging/README.md`).
You can also build them yourself on the machine, the same way the workflow does:

```
git clone https://github.com/ivoitovych/LoadForge && cd LoadForge
packaging/build-release.sh --out ~/artifacts
```

Either way you end with, per architecture (`x86_64` or `aarch64`), in one directory:

```
loadforge-VERSION-linux-ARCH-static.tar.gz
loadforge-VERSION-linux-ARCH-dynamic.tar.gz
loadforge_VERSION_ARCH.deb              (ARCH amd64 or arm64)
loadforge-VERSION-linux-ARCH.SHA256SUMS
```

**Check the checksum on the machine where the artifact will run**, not where it was
downloaded; that is the point of carrying it on a stick:

```
cd /path/to/artifacts
sha256sum -c loadforge-VERSION-linux-ARCH.SHA256SUMS
```

Every line must say `OK`. Also copy `packaging/validate-machine.sh` and
`packaging/test-deb.sh` from the repository next to the artifacts
(`validate-machine.sh` is also inside both tarballs).

## 4. On each machine, with each artifact

Use a terminal as a normal user with `sudo` available. Note the time you start. Nothing else
heavy should run; a laptop must be on mains power and on a hard surface.

### 4.1 The static artifact

```
tar -xzf loadforge-VERSION-linux-ARCH-static.tar.gz
./loadforge-VERSION-linux-ARCH-static/loadforge --version
./validate-machine.sh --binary ./loadforge-VERSION-linux-ARCH-static/loadforge \
    --label LABEL --flavour static --out ./results --workload-seconds 60
echo "exit $?"
```

The script prints one `PASS` or `FAIL` line per step and ends with the path of the results
file. It takes about 25 minutes at 60 s per workload. Its steps: the version, `--help`,
the self-test digest, the two exit codes of a usage error, a configuration check and a
configuration-driven run, the capability report, each of the eleven workloads for the
given time, `mixed` and `cycling` for three times that, the crash journal, a result
directory that is not there, a small `explore`, an `explore` refused for a metric nobody
has, and a controller killed with `SIGKILL` (no worker may outlive it; the journal must
say the run ended abnormally).

`exit 0` means every scripted step gave the exit code and the verdict it should. It does
**not** mean the machine is good; section 8 does.

### 4.2 The dynamic artifact, as the package (Ubuntu 24.04 or later)

```
sudo packaging/test-deb.sh ./loadforge_VERSION_ARCH.deb
echo "exit $?"
```

This installs the package, runs the installed `loadforge` (`--version`, a two-second
stress run that must end `verdict: clean`), removes it with `dpkg --purge`, and checks no
file of the package is left. It leaves the machine as it found it. Then install it again
for the full run:

```
sudo apt install ./loadforge_VERSION_ARCH.deb     # or: sudo dpkg -i ...
loadforge --version
./validate-machine.sh --binary /usr/bin/loadforge --label LABEL --flavour deb \
    --out ./results --workload-seconds 60
sudo apt remove loadforge
```

If `apt` reports an unmet dependency, the machine is older than the declared minimum, or
a library the package needs is missing: record the exact message (section 7) and use the
static artifact there.

### 4.3 The dynamic artifact as the tarball

The tarball holds the same binary as the package. Run it once, on any machine, so that
the tarball itself is known to work (not only the package):

```
tar -xzf loadforge-VERSION-linux-ARCH-dynamic.tar.gz
./loadforge-VERSION-linux-ARCH-dynamic/loadforge stress --test sort --duration 10s
```

### 4.4 Compare the two architectures

The self-test digest (`loadforge --selftest-digest`) is a hash over a fixed corpus,
including the scalar paths of the compression and dense-linear workloads. **For one
version it must be identical on x86-64 and on ARM64**
(`docs/determinism.md`). It is in every results file; compare them across all machines. A
difference is a defect in the project: stop and file it.

Measured on the development machine, a debug build of this commit, x86-64:

```
$ loadforge --selftest-digest
e0ff849ff34906de
```

(The digest is the same for every build type; that is part of what is checked, so compare
the artifacts' digest with it only if the version string is `0.0.0-m2`.)

### 4.5 What to look at by eye while it runs

The script cannot see these. Write what you see in the results file's OWNER sections.

- The **capability report** at the top of the `capabilities` step. Does it list the
  sensors you expect on this machine (section 2) as `present`? A sensor you know exists
  and the report calls absent is a finding. A sensor the report calls present that has no
  physical meaning (a temperature that does not move under load) is a finding too.
- The **temperatures and fan** with a tool of your own (`sensors` from lm-sensors, or the
  firmware's display), because LoadForge 1.0 does not enforce a temperature limit
  (`docs/safety.md` §1). Stop the run by hand if the machine is hotter than you accept.
- Anything odd: a fan that changes pitch, a throttle noticed in the frequency, a message
  in `dmesg` (`sudo dmesg | tail -50` afterwards: machine-check, thermal throttling,
  EDAC, I/O errors).

## 5. Live media

The static artifact exists for machines you cannot install anything on, or whose
userspace is old: rescue media, live USB sticks, a customer's machine. It is validated
there, from the medium, not from an installed system.

For each architecture, at least one machine, and at least **one live or rescue medium
whose userspace is older than Ubuntu 24.04** (for example an Ubuntu 22.04 or 20.04 live
image, a Debian 12 live image, SystemRescue):

1. Boot the machine from the medium. Write down the medium's name and version and how
   you booted it.
2. Open a terminal. Mount the stick that holds the artifacts and the scripts (usually
   `sudo mount /dev/sdXN /mnt`) and copy the static tarball and the scripts to a
   directory in RAM (`/tmp` on most live images). Verify the checksum there (section 3).
3. Unpack and run section 4.1 with `--label LABEL-live --out /tmp/results`.
4. Copy `/tmp/results` back to the stick before you shut down. A live image forgets
   everything on reboot.
5. **Negative control.** On the same medium, run the *dynamic* binary:
   `./loadforge-VERSION-linux-ARCH-dynamic/loadforge --version`. On a userspace older than
   Ubuntu 24.04 it is expected to be refused by the dynamic loader with a message naming a
   missing `GLIBC_…` or `GLIBCXX_…` version. That refusal is the reason Q11 declares 24.04
   the minimum for the dynamic build; save the exact message in the results file. If it
   runs, record that instead: it is information, not a failure.
6. Also run the **dynamic artifact from a live medium that is Ubuntu 24.04 or later**
   (a current Ubuntu live image): section 4.3 from the medium. This is the dynamic half of
   "both artifacts validated on live media". A live session can install the `.deb` into
   its RAM disk with `sudo dpkg -i`, which is the more realistic use.

What is different on live media, and not a defect: no writable disk (results go to RAM or
the stick), fewer or different sensors, small RAM (the default footprint is 32 MiB per
worker, so a machine with few CPUs and 1 GiB of RAM is fine, and one with 256 cores is
not: use `--workers 8`).

## 6. The long procedures

Do these on **at least one machine of each architecture**, and the thermal procedure on
every machine that has a readable temperature.

### 6.1 T11: repeatable worst-case discovery on real hardware (the thermal procedure)

This is the half of M6's exit criterion that stream 33 could not meet on a virtual
machine (`docs/journal/33-explore-integration.md`, "T11"). The criterion: *repeatable
worst-case discovery on real hardware, with variance reported and replicates recorded; an
objective whose metric is unavailable fails loudly rather than silently scoring zero.*
The second clause is already met and tested; the first needs this.

**Prerequisites.** A machine with a readable temperature that is otherwise idle: close
everything, no updates running, a constant room. Find which series you have, from the
capability report:

```
loadforge stress --test sort --duration 3s 2>&1 | sed -n '/^  thermal:/,/^  fan:/p'
```

- `thermal.package` present (Intel `coretemp`, AMD `k10temp`): use `thermal.package`.
- Only `thermal.zone` present (most ARM64 machines): look at
  `cat /sys/class/thermal/thermal_zone*/type` and pick the zone for the CPU; its number is
  the series index. Use `thermal.zone@N`. (`thermal.sensor` is the same idea for other
  hwmon chips.)
- Neither: the machine cannot give this result. Do not use another number. Record that it
  cannot, and do the check in 6.1.3 instead.

**6.1.1 One search.** With `METRIC` the series (`thermal.package` or `thermal.zone@N`),
`N` the number of CPUs (`nproc`):

```
loadforge explore \
  --objective METRIC:max:higher \
  --settle METRIC --settle-window 30s --settle-max 300s \
  --candidate dense-1:dense_linear:1 \
  --candidate dense-all:dense_linear:N \
  --candidate dense-1-pinned:dense_linear:1:0 \
  --candidate sort-1:sort:1 \
  --candidate sort-all:sort:N \
  --candidate sort-1-pinned:sort:1:0 \
  --duration 120s --replicates 2 --final-replicates 4 \
  --reference-replicates 3 --interleave 2 \
  2>&1 | tee search-1.txt
```

That is dense linear algebra and the sort workload, each on one thread anywhere, on all
cores, and on one thread pinned to CPU 0: "with and without pinning to one core". The
settle phase is the cool-down: a trial starts only when the temperature has stopped
changing (slope within 0.05 degrees a second over 30 s) or after 300 s. A trial that
started warm is reported as such. Each trial takes up to 300 s of settling plus 120 s of
load; the line `explore: T trial(s) in R round(s)` near the top of the report says how
many trials there are, so the time is T times about seven minutes at most. Plan hours.

**6.1.2 Repeat it.** Run the same command **at least five times** in a row, saving each
report whole (`search-1.txt` to `search-5.txt`): the replicates are in the report, and a
report cut to its last line is not evidence. Do not change anything in between; do not
let the machine sit hot or cold for long between searches.

**6.1.3 Reading it.** Each report says `worst case: <candidate>: <mean> +/- <half-width>
(95% CI, n=<replicates>, sd <sd>, correction sd <…>)`, the ranking against the best
rival, every candidate's replicates, and the reference's readings with the drift. The
criterion is met on this machine **only if** the five searches name the **same worst
case** and their **intervals overlap**. Write that claim in the results file with the five
`worst case:` lines pasted in. If they do not agree, say that: it is a result, and it
says the objective is not stable on that machine; do not rerun until they agree.

Record with the searches: the machine (CPU model, cooling, fan behaviour, ambient
temperature if you know it), whether the settle phase settled (`settled`/`unsettled`
per trial in the report), and the highest temperature seen. No temperature number goes in
that was not read from the machine by the tool or by a named instrument.

**6.1.4 On a machine without a temperature.** Confirm the loud failure on hardware too:

```
loadforge explore --objective thermal.package:max:higher --candidate a:sort:1
echo "exit $?"
```

Expected: a message that says the metric is unavailable, with the source's reason and
remedy, exit code 1, and no worker started.

### 6.2 The soak (the nightly soak, on the artifacts)

Continuous integration runs a soak every night: the mixed workload rotated for two hours
on a worker, then the executor and the journal at full length (`.github/workflows/soak.yml`;
tests named `MixedSoak.*` and `*Soak*`). That checks the code. This checks the artifact on
hardware: **no growth of the resident set, of descriptors or of threads, a tally that
never runs backwards, no verification lost.**

```
./validate-machine.sh --binary ./loadforge-VERSION-linux-ARCH-static/loadforge \
    --label LABEL --flavour static --out ./results --workload-seconds 5 --soak-hours 6
```

(`--workload-seconds 5` keeps the quick steps quick; the soak is the six hours. Use
`--soak-hours 12` on the machine you can leave overnight.) The script samples the
controller every minute (`soak-monitor.csv`: resident memory, descriptors, threads,
worker processes) and compares the sixth sample with the last. It fails the soak if the
verdict is not `clean`, the exit code is not 0, the resident memory grew by more than
10% plus 4 MiB, or the descriptors or threads grew at all. Attend the machine for the
first half hour (section 4.5) and look at it again at the end; an unattended machine is
exactly the case `docs/safety.md` warns about. **Use `--journal`**: the script does.

To run the same tests the nightly job runs, on a source build on the same machine:

```
cmake --preset debug && cmake --build --preset debug -j"$(nproc)"
LOADFORGE_SOAK_SECONDS=7200 ctest --preset debug -R '^MixedSoak\.' --output-on-failure
ctest --preset debug -R 'Soak' -E '^MixedSoak\.' --output-on-failure
```

and write the two `ctest` exit codes in the results file.

### 6.3 The crash journal across a real power cut (optional, destructive)

Only on a machine that you may power off hard and that has no data you need open. The
crash journal exists for the run that ends in a power-off, and only hardware can show that
what it wrote reached the disk.

```
loadforge stress --test dense_linear --duration 10m --journal ./power.journal
```

After about two minutes **pull the power** (or hold the power button). Boot again, and:

```
loadforge journal ./power.journal
```

Expected: `ended abnormally between records` (or `with a torn last record`) and the last
entries, a few seconds old at most. A journal that is empty or missing after a power cut
is a defect. Record what it printed.

## 7. Where the results go

**One results file per machine, per artifact, per run, in `tests/hardware/`, named
`m7-LABEL-FLAVOUR-YYYY-MM-DD-HHMM-UTC.md`** (`FLAVOUR` is `static`, `deb` or `dynamic`;
the time is when the run started, UTC, from `date -u`). That is the same convention as
`tests/hardware/m2b-thermal-2026-10-20-2212-UTC.md`. A file is **never overwritten**; a
second run adds a second file. `validate-machine.sh` writes exactly this file and a
directory of the same name (without `.md`) holding every step's output. Copy both into
`tests/hardware/` and commit them; the directories can be large, so commit the `.md`
files and, from the directories, `machine.log`, `artifact.log`, `capabilities.log` and
the JSON results; keep the rest where you keep such things and name them in the file.

The thermal searches, the soak's `ctest` exit codes and the power-cut result are
recorded in **additional files** with the same naming and a different tag:
`m7-LABEL-thermal-<stamp>.md`, `m7-LABEL-soak-<stamp>.md`, `m7-LABEL-powercut-<stamp>.md`.

When all machines are done, one **summary**, `tests/hardware/m7-summary-<stamp>.md`:

```
# M7 hardware validation: summary
**Written:** YYYY-MM-DD HH:MM UTC   **Version:** …   **Commit:** …

| Label | Arch | Static | Deb | Live medium (static) | Live medium (dynamic) | Digest | Thermal (T11) | Soak | Power cut | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| … links to each results file … |

## M7 exit criterion
- [ ] Multi-machine validation complete on x86-64 (n machines) and ARM64 (n machines).
- [ ] No known correctness defect (issues open: …).
- [ ] Both artifacts validated on live media (media: …).
- [ ] The self-test digest is identical on every machine.
- [ ] T11: the same worst case in five searches, on machines: …
```

A failure is filed as an issue with the **hardware failure** template, with the results
file linked, and recorded here as a failure. The earlier file is left as it was.

## 8. When M7 is met

All of these, with the files as evidence:

1. At least three x86-64 and two ARM64 machines each have a results file for the static
   artifact and for the package, every scripted step `PASS`, no unexplained finding,
   corrected error or timed-out worker.
2. The static artifact has a results file from a live or rescue medium older than Ubuntu
   24.04 on each architecture, and the dynamic artifact from a live medium of Ubuntu 24.04
   or later on each architecture, and the negative control (5.5) is recorded.
3. The digest is the same on all machines.
4. T11 is met on at least one machine with a readable temperature, or its failure is
   recorded as a result.
5. The soak is clean on at least one machine of each architecture.
6. No open issue is a correctness defect. **One known defect is open at the time of
   writing:** the temperature limit in a configuration is not enforced
   (`docs/safety.md` §1). It is a safety gap and not a wrong result, but "no known defect"
   needs the owner's decision on it: build the enforcement, or declare it out of 1.0 and
   say so in the release notes.
7. A reviewer other than the author has looked at the project. None has yet looked at the
   whole tree; the project still needs an outside reviewer.

Then the release is published from the draft (`packaging/README.md`).

## 9. If a step fails

- A scripted step fails: read its log in the results directory. The result is the
  exit code the tool gave; do not rerun until it passes. File it, and go on with the
  remaining steps: a second failure on the same machine is as useful as the first.
- A run ends `incomplete` (exit 6) or with a worker `timed out`: on a machine under
  other load this is the supervision working; check `dmesg` and the load, then rerun once
  and keep both files.
- A run ends `findings` (exit 5): this is the tool doing what it is for, **and** it is a
  possible defect in the tool. Keep the `run.json`, note the machine, and run the same
  command on a second machine. A wrong result that follows one core, one CPU or one
  memory slot is a hardware finding; one that follows the binary or the workload on every
  machine is a defect in LoadForge.
- The machine powers off or reboots: that is the machine's protection, not a LoadForge
  result. Check the journal (6.3) and the cooling, and do not repeat the run until you
  know why.
