# sysfs fixtures

Captured 2026-10-20 10:57 UTC on the same Firecracker virtual machine as `../procfs`
(`Linux 6.18.44-fc-v139 x86_64`). That machine exposes no temperature sensor, no fan input and no
cpufreq directory (PLAN F3 measured the same of its development container), so
what is committed here is what it really offers, which is nothing:

| Fixture | What the machine really had | Command |
|---|---|---|
| `thermal/absent` | no `/sys/class/hwmon`, no thermal zone | `ls /sys/class/hwmon /sys/class/thermal` listed neither a chip nor a zone; the tree holds only `CAPTURED`, the note of that |
| `fan/absent` | no `/sys/class/hwmon` | as above |
| `frequency/absent` | online CPUs `0-3`, no `cpuN/cpufreq` for any of them | `cp /sys/devices/system/cpu/online …/online`; `cp /proc/stat …/proc/stat`; `ls /sys/devices/system/cpu/cpu0` showed no `cpufreq` |

A present hwmon, thermal-zone or cpufreq tree could not be captured: no
machine reachable from the stream had one. Those states are built by the tests
(see the `LOADFORGE P7 FIXTURE BUILDER` files), in the layout of the kernel's
`Documentation/hwmon/sysfs-interface.rst`, and are not claimed to be
captures. The journal fragment 09 records this, and what would replace them.
