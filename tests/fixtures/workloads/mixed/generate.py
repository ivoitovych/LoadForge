#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A second implementation of the schedules in src/workloads/mixed/, from the
header's account of them (mixed.hpp, "The order and the rules"; cycling.hpp),
for the oracle and determinism tiers to compare the C++ against.

It prints a schedule in the listing format tests/oracle/workloads/mixed reads,
and the digest (a CRC-32 of the canonical text of everything the schedule says)
that tests/determinism/workloads/mixed pins. It shares no code with the C++ and
reads none: a disagreement is a question about both.

    generate.py mixed WORKERS SEED DURATION_S ROTATION_S FOOTPRINT [CPUSETS]
    generate.py cycling KIND WORKERS DURATION_S DWELL_S

CPUSETS is semicolon-separated sets of comma-separated CPUs ("0;1;2"). The
roster is the eleven workloads the build has, each accepting 1 MiB or more.
"""

import sys
import zlib

MASK = (1 << 64) - 1
ROSTER = [
    "adaptive_mesh",
    "compression",
    "dense_linear",
    "fft",
    "graph",
    "monte_carlo",
    "particles",
    "raytrace",
    "sort",
    "sparse",
    "stencil",
]
MIN_FOOTPRINT = 1 << 20
COMPUTE = {"dense_linear", "fft", "monte_carlo", "particles", "raytrace"}
MEMORY = {"adaptive_mesh", "graph", "sort", "sparse", "stencil"}
PLAN = ["mixed", "compute-heavy", "memory-heavy", "balanced", "all-join"]
PICK, SEED, SIZE = 1, 2, 3


def mix(value):
    value = (value + 0x9E3779B97F4A7C15) & MASK
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & MASK
    return value ^ (value >> 31)


def draw(seed, rotation, slot, purpose):
    return mix((seed + mix((rotation + mix((slot + mix(purpose)) & MASK)) & MASK)) & MASK)


def pool_for(kind, slot, usable):
    wanted = None
    if kind == "compute-heavy":
        wanted = COMPUTE
    elif kind == "memory-heavy":
        wanted = MEMORY
    elif kind == "balanced":
        wanted = COMPUTE if slot % 2 == 0 else MEMORY
    if wanted is None:
        return usable
    pool = [name for name in usable if name in wanted]
    return pool or usable


def assignment(usable, seed, footprint, rotation, slot, kind):
    pool = pool_for(kind, slot, usable)
    spread = 0 if kind == "all-join" else slot
    name = pool[(draw(seed, rotation, 0, PICK) + spread) % len(pool)]
    size = footprint // 2 if draw(seed, rotation, slot, SIZE) & 1 else footprint
    if size < MIN_FOOTPRINT:
        size = footprint
    return name, draw(seed, rotation, slot, SEED), size


def label_of(names):
    return "+".join(sorted(set(names)))[:64]


def mixed(workers, seed, duration_s, rotation_s, footprint, cpusets):
    duration, rotation = duration_s * 10**9, rotation_s * 10**9
    usable = ROSTER if footprint >= MIN_FOOTPRINT else []
    out = {"workers": workers, "start": [], "steps": []}
    first = [assignment(usable, seed, footprint, 0, slot, PLAN[0]) for slot in range(workers)]
    out["start"] = [(name, size) for name, _, size in first]
    out["phase"] = f"r0 {PLAN[0]}"
    out["workload"] = label_of([name for name, _, _ in first])
    current = out["workload"]
    for r in range(1, (duration - 1) // rotation + 1):
        moves = bool(cpusets) and r % 3 == 0
        kind = "move" if moves else PLAN[r % 5]
        commands, names = [], []
        for slot in range(workers):
            if moves:
                cpus = cpusets[(slot + r // 3) % len(cpusets)]
                commands.append((slot, f"rebind {','.join(map(str, cpus))}"))
            else:
                name, sd, size = assignment(usable, seed, footprint, r, slot, kind)
                names.append(name)
                commands.append((slot, f"switch {name} {sd} {size} 0 1 1"))
        if not moves:
            current = label_of(names)
        out["steps"].append((r * rotation, f"r{r} {kind}"[:64], current, commands))
    return out


def cycling(kind, workers, duration_s, dwell_s, period_ns=10**9):
    duration, dwell = duration_s * 10**9, dwell_s * 10**9
    out = {
        "workers": workers,
        "start": [],
        "phase": "run",
        "workload": f"cycling {kind}",
        "steps": [],
    }
    paused = [False] * workers
    permille = [1000] * workers
    for level in range((duration - 1) // dwell + 1):
        if kind == "load":
            name, target = [("idle", 0), ("heavy", 1000), ("moderate", 500), ("heavy", 1000)][
                level % 4
            ]
            wanted = [target] * workers
        else:
            share = [125, 500, 1000, 250, 1000][level % 5]
            working = max(1, workers * share // 1000)
            name = f"{working} of {workers}"
            wanted = [1000 if slot < working else 0 for slot in range(workers)]
        commands = []
        for slot in range(workers):
            mine = []
            if wanted[slot] == 0:
                if not paused[slot]:
                    mine.append("pause")
                paused[slot] = True
            else:
                if paused[slot]:
                    mine.append("resume")
                if permille[slot] != wanted[slot]:
                    mine.append(f"set duty {wanted[slot]} {period_ns}")
                paused[slot] = False
                permille[slot] = wanted[slot]
            if mine:
                commands.append((slot, mine))
        out["steps"].append((level * dwell, f"c{level} {name}"[:64], out["workload"], commands))
    return out


def canonical(schedule):
    """The text the C++ digest is a CRC-32 of (schedule.cpp, digest())."""
    text = [
        f"workers={schedule['workers']};phase={schedule['phase']};workload={schedule['workload']};"
    ]
    for name, size in schedule["start"]:
        text.append(f"start={name};footprint={size};")
    for at, phase, workload, commands in schedule["steps"]:
        text.append(f"at={at};phase={phase};workload={workload};")
        for slot, each in commands:
            text.append(f"slot={slot};")
            for command in each if isinstance(each, list) else [each]:
                text.append(command_text(command))
    return "".join(text)


def command_text(command):
    parts = command.split()
    if parts[0] == "switch":
        _, name, sd, size, decomposition, oracle, integrity = parts
        return (
            f"switch(workload={name};seed={sd};footprint={size};"
            f"decomposition={decomposition};oracle={oracle};integrity={integrity};)"
        )
    if parts[0] == "rebind":
        return "rebind(" + "".join(f"cpu={cpu};" for cpu in parts[1].split(",")) + ")"
    if parts[0] == "set":
        return f"set duty(busy={parts[2]};period={parts[3]};)"
    return f"{parts[0]}()"


def listing(schedule):
    lines = [
        f"workers {schedule['workers']}",
        f"phase {schedule['phase']}",
        f"workload {schedule['workload']}",
    ]
    lines += [f"start {name} {size}" for name, size in schedule["start"]]
    for at, phase, workload, commands in schedule["steps"]:
        lines.append(f"step {at} | {phase} | {workload}")
        for slot, each in commands:
            for command in each if isinstance(each, list) else [each]:
                lines.append(f"  worker {slot}: {command}")
    lines.append(f"digest {zlib.crc32(canonical(schedule).encode()):08x}")
    return "\n".join(lines) + "\n"


def main(argv):
    if argv[0] == "mixed":
        workers, seed, duration, rotation, footprint = (int(each) for each in argv[1:6])
        cpusets = (
            [[int(cpu) for cpu in each.split(",")] for each in argv[6].split(";")]
            if len(argv) > 6
            else []
        )
        schedule = mixed(workers, seed, duration, rotation, footprint, cpusets)
    else:
        schedule = cycling(argv[1], int(argv[2]), int(argv[3]), int(argv[4]))
    sys.stdout.write(listing(schedule))


if __name__ == "__main__":
    main(sys.argv[1:])
