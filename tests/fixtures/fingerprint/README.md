# Fingerprint fixtures

Each directory is a machine root: the files a run fingerprint (F12) reads, laid out
where the kernel puts them, so that `read_fingerprint(fs, "<this directory>", ...)`
opens them as it would on the machine.

## vm-x86-64

Captured on 2026-10-20 12:46 UTC, on the virtual machine this stream was built in:
Ubuntu 24.04.5, Linux 6.18.44-fc-v139 x86_64, a four-CPU Intel Xeon guest, cgroup v1
hybrid layout (the memory hierarchy at `/sys/fs/cgroup/memory`, the unified one
beside it, no controllers). The files are byte copies:

```text
cp /proc/cpuinfo /proc/meminfo /proc/sys/kernel/osrelease
cp /proc/self/cgroup /proc/self/mountinfo
cp /sys/devices/system/cpu/smt/active
cp /sys/kernel/mm/transparent_hugepage/enabled
cp memory.stat memory.limit_in_bytes   # the process's memory cgroup, and the mount root's
```

What this machine does not have is as informative as what it has: no `power_supply`,
no `cpufreq`, no `thermal` and no `platform_profile` exist on it, so the fixture has
none, and the fingerprint reports each of those fields absent. The files for a laptop
(power source, battery, governor, energy performance preference, platform profile,
turbo, thermal zone) were not available on any machine this stream could run on; the
tests that need them build them from the formats the kernel documents
(Documentation/ABI/testing/sysfs-class-power, sysfs-devices-system-cpu,
sysfs-firmware-acpi, Documentation/admin-guide/pm/cpufreq.rst), and say so.
