# EDAC fixtures

Roots to hand to EdacSource as its sysfs root (`/sys` on a machine).

| tree | what it is | provenance |
|---|---|---|
| `two-controllers` | `mc0` with 3 corrected errors, `mc1` clean | **modelled** on Documentation/ABI/testing/sysfs-devices-edac (the attribute names and the `%u` decimal text); no machine with an EDAC driver was available |
| `malformed` | `ce_count` that is not a number | constructed |
| `absent` | no EDAC | the sandbox's own /sys |

The fixtures a clone cannot hold -- a count that wraps, a controller that
disappears, an attribute the caller may not read -- are built by the tests
(LOADFORGE P7 FIXTURE BUILDER).
