# Mixed and cycling schedule fixtures

Date: 2026-10-21 22:00 UTC (the fixtures were generated on 2026-10-21, between
21:40 and 22:30 UTC)

The `*.txt` files here are the output of `generate.py`, run with Python 3.11.15
on x86-64 Linux 6.18.44 (a Firecracker virtual machine, 4 CPUs):

```
generate.py mixed 4 7 70 10 8388608 "0;1;2;3"            > mixed-w4-s7-d70-r10-moving.txt
generate.py mixed 3 1 300 10 8388608                      > mixed-w3-s1-d300-r10-fixed.txt
generate.py mixed 8 42 1800 30 16777216 "0,1;2,3;4,5;6,7" > mixed-w8-s42-d1800-r30-moving.txt
generate.py cycling load 3 100 5                          > cycling-load-w3-d100-dwell5.txt
generate.py cycling cores 8 60 5                          > cycling-cores-w8-d60-dwell5.txt
generate.py cycling cores 1 20 5                          > cycling-cores-w1-d20-dwell5.txt
```

`generate.py` is a second implementation of the schedules, written from the
accounts in `src/workloads/mixed/mixed.hpp` ("The order and the rules") and
`cycling.hpp`, and shares no code with the C++: it has its own 64-bit mixing
function, its own pools and its own state machine for the cycle. The oracle
tier (`tests/oracle/workloads/mixed`) makes the same schedules with the C++ and
compares the listing line for line; the determinism tier
(`tests/determinism/workloads/mixed`) pins the digest at the end of each file
and CI compares it on x86-64 and on ARM64. The digest is a CRC-32 of the
canonical text of everything a schedule says, which `generate.py` also builds
independently (`canonical()`).

The roster of the mixed cases is the eleven workloads the build has, each
accepting a footprint of 1 MiB or more. The seed, the worker count, the
durations and the CPU sets are in the command above; the CPU sets of the
8-worker case are shared by two workers each.

What these files are not: a measurement. Which workloads stress what is the
hypothesis in `classify()` (mixed.cpp) and in `generate.py`'s `COMPUTE` and
`MEMORY`; a hardware validation (T11) is what would test it.
