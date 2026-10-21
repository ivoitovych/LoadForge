# powercap and power PMU fixtures

Roots to hand to PowerSource as its sysfs root (`/sys` on a machine).

| tree | what it is | provenance |
|---|---|---|
| `perf-psys-only` | the `power` PMU of a machine whose RAPL exposes only `energy-psys` | **captured**: `cp -r /sys/bus/event_source/devices/power` on `Linux vm 6.18.44-fc-v139 #1 SMP PREEMPT_DYNAMIC x86_64` (a Firecracker VM, 4 vCPUs), 2026-10-20 12:55 UTC; only the files below `events/`, `format/`, `type` and `cpumask` are kept |
| `perf-one-package`, `perf-two-packages` | the same PMU with an `energy-pkg` event (`event=0x02`) | **modelled** on the capture, changing only the event name and number, which are those of the kernel's RAPL PMU (arch/x86/events/rapl.c); no machine with the package event was available |
| `intel-two-packages` | powercap zones `intel-rapl:0`, `intel-rapl:1` and a sub-zone `intel-rapl:0:0` | **modelled** on Documentation/power/powercap/powercap.rst; `max_energy_range_uj` is the value that file's examples and Intel parts publish (262143328850); no machine with powercap was available |
| `malformed-energy`, `malformed-scale` | hostile values | constructed |
| `absent` | neither interface | the sandbox's own /sys |

The fixtures a clone cannot hold -- a node readable by root only, a node that
disappears mid-run -- are built by the tests (LOADFORGE P7 FIXTURE BUILDER).
