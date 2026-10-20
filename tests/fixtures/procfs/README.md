# procfs fixtures

Captured 2026-10-20 10:57 UTC on a Firecracker virtual machine (`Linux 6.18.44-fc-v139 x86_64`, 4 vCPUs, 16 GiB), by
copying the file as it was, with `cp`:

| Fixture | Command |
|---|---|
| `stat/present/proc/stat` | `cp /proc/stat tests/fixtures/procfs/stat/present/proc/stat` |
| `meminfo/present/proc/meminfo` | `cp /proc/meminfo tests/fixtures/procfs/meminfo/present/proc/meminfo` |

The trees are roots: a source built over `stat/present` reads
`stat/present/proc/stat`. The other states (malformed, wrapped, denied,
vanished) are derived from these captures by the tests that need them,
at run time, and say how; nothing in them is typed in by hand beyond the
edit the test names.
