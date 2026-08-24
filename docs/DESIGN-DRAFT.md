> **Editorial note — read this first.**
>
> This is the original project description, preserved **verbatim and unedited** as the
> historical working draft. It is not a specification, and several parts of it have been
> deliberately superseded after review. Known examples: §45's source structure (no place
> for the platform abstraction, supervision or testing), §6's suggestion to verify matrix
> multiplication by recomputing blocks with the same implementation (this is the trap
> `docs/verification.md` exists to prevent), §39's back-to-back schedule, §10 and §15's
> assumption that parallel floating-point checksums are deterministic by default, and
> §50's repository naming.
>
> **[`PLAN.md`](PLAN.md) is the current design of record.** Where the two disagree, the
> plan wins — §2 of the plan explains each divergence and why.

---

# LoadForge

**A lightweight, composable, self-verifying hardware reliability and load-testing framework for Linux**

## 1. Project Idea

LoadForge is intended to be a modern hardware reliability and stress-testing suite designed from scratch around several principles:

- lightweight implementation;
- minimal external dependencies;
- broad coverage of CPU and memory subsystems;
- multiple fundamentally different integrated reliability tests;
- runtime configurability;
- continuous correctness verification;
- integrated telemetry collection;
- objective comparison of different types of hardware load;
- ability to discover which workloads produce the highest power, temperature, memory pressure, throttling, or other forms of stress.

The primary target is the complete **CPU–cache–memory complex** of a computer.

For the initial version, SSD and discrete GPU testing are intentionally outside the scope.

The project is not intended to be merely another program that makes Linux report:

```text
CPU utilization: 100%
```

Different workloads can all report 100% CPU utilization while producing radically different physical conditions inside the system.

LoadForge should instead answer questions such as:

- Which workload causes the highest package power?
- Which workload heats the CPU fastest?
- Which workload produces the highest equilibrium temperature?
- Which workload stresses DRAM and the memory controller most heavily?
- Which workload generates the greatest cache-coherency traffic?
- Which workload produces the strongest thermal or power throttling?
- Which combination of computations is most likely to expose instability?
- Can the computer execute all of these workloads correctly for several hours?

---

# 2. Main Reliability-Testing Principle

A meaningful hardware reliability test should generally **not be a tiny isolated instruction loop**.

A loop that only performs integer additions, floating-point FMAs, random reads, or atomics can be useful for:

- characterization;
- calibration;
- diagnostics;
- understanding individual hardware mechanisms.

But such kernels should normally be regarded as **internal building blocks**, not as the principal reliability tests exposed to the user.

A major reliability test should instead represent a complete computational workload.

For example, a dense linear-algebra test naturally involves:

- multiple CPU cores;
- floating-point execution units;
- SIMD;
- FMA;
- integer address calculations;
- L1 cache;
- L2 cache;
- last-level cache;
- RAM;
- memory controller;
- inter-core synchronization;
- power delivery;
- cooling and thermal management.

Similarly, graph processing, FFT, simulation, sorting, compression, or ray tracing exercise the same overall hardware complex in very different ways.

This leads to the central design principle:

> **Each major test should exercise the whole relevant hardware subsystem, but each test should create a different distribution of pressure across that subsystem.**

It would be counterproductive to force every test to maximize every hardware component equally.

Some workloads should naturally be:

- compute dominated;
- bandwidth dominated;
- latency dominated;
- branch dominated;
- synchronization dominated;
- cache-coherency dominated;
- mixed.

The diversity is precisely what makes the test suite valuable.

---

# 3. Hardware Scope

The initial LoadForge version should primarily exercise:

## CPU

- integer execution units;
- floating-point units;
- SIMD/vector units;
- FMA units;
- division and square-root units;
- branch prediction;
- instruction frontend;
- out-of-order execution;
- instruction-level parallelism;
- all physical cores;
- SMT/logical CPUs where present.

## Cache hierarchy

- L1 data cache;
- L1 instruction cache;
- L2;
- shared LLC/L3;
- cache replacement;
- cache-line transfers;
- cache coherence.

## Memory subsystem

- DRAM;
- memory channels;
- memory controller;
- reads;
- writes;
- read-modify-write operations;
- sequential access;
- irregular/random access;
- memory latency;
- memory bandwidth.

## Virtual-memory machinery

- TLB;
- page-table walking;
- normal pages;
- optionally huge pages;
- large virtual address working sets.

## Multicore/interconnect

- core-to-core communication;
- shared-cache communication;
- atomic operations;
- barriers;
- reductions;
- cache-line migration;
- false sharing;
- coherence traffic.

## Platform-level behavior

- CPU package power;
- voltage/frequency transitions;
- boost behavior;
- thermal management;
- cooling;
- fan response;
- power limiting;
- thermal throttling;
- sustained-load behavior.

---

# 4. What Is Deliberately Outside the Initial Scope

## SSD

SSD reliability testing is excluded.

The test suite itself should avoid producing unnecessary storage traffic.

Telemetry and intermediate results can preferably remain in RAM and be written to persistent storage only at the end.

## Discrete GPU

Discrete-GPU testing is excluded from the first version.

This keeps LoadForge usable in environments where:

- proprietary GPU drivers are unavailable;
- the system is booted from removable Linux media;
- only basic graphics support is available.

## PCIe saturation

Without intentionally using a high-bandwidth PCIe endpoint such as:

- GPU;
- NVMe SSD;
- high-speed NIC;
- Thunderbolt device;

there is no particularly useful way to generate massive generic PCIe traffic.

PCIe testing can therefore be considered a future optional extension.

---

# 5. Major Reliability Tests

The initial suite should contain approximately a dozen complete workloads.

Each should be configurable in:

- runtime;
- thread count;
- CPU affinity;
- working-set size;
- memory consumption;
- numerical precision where applicable;
- intensity;
- verification frequency.

---

# 6. Test 1 — Dense Linear Algebra

Dense linear algebra is an excellent primary reliability workload.

Possible operations include:

- matrix multiplication;
- repeated GEMM-like operations;
- LU decomposition;
- QR decomposition;
- Cholesky decomposition;
- matrix inversion;
- systems of linear equations.

Example:

```text
C = A × B
```

Large matrices force substantial interaction between:

- CPU execution;
- SIMD;
- FMA;
- cache hierarchy;
- main memory;
- memory controller.

A multicore implementation additionally stresses:

- worker synchronization;
- shared cache;
- multicore scheduling.

## Reliability verification

Instead of merely performing calculations, results should be verified.

For:

```text
A × x = b
```

calculate:

```text
r = A × x - b
```

and verify that:

```text
||r||
```

remains within the expected numerical tolerance.

For matrix multiplication, selected blocks may be recomputed independently or verified by mathematical checks.

## Load character

Primarily:

- high floating-point pressure;
- high SIMD/FMA pressure;
- substantial cache traffic;
- substantial RAM activity;
- sustained multicore operation.

This is likely to be one of the strongest candidates for maximum CPU package power.

---

# 7. Test 2 — Sparse Linear Algebra / Iterative Solver

Sparse numerical algorithms behave very differently from dense matrix computation.

Possible components:

- sparse matrix-vector multiplication;
- conjugate-gradient solver;
- iterative relaxation;
- preconditioning;
- dot products;
- vector norms;
- reductions.

A large sparse matrix produces irregular access patterns.

## Hardware stressed

- CPU;
- FPU;
- SIMD where practical;
- RAM;
- memory latency;
- cache hierarchy;
- TLB;
- branches;
- multicore reductions;
- barriers.

## Load character

Dense matrix multiplication tends to exploit locality extremely well.

Sparse algorithms intentionally produce much poorer locality.

This test therefore emphasizes:

- RAM access;
- latency;
- cache misses;
- synchronization.

A system stable under dense SIMD computation may behave quite differently under sparse computation.

---

# 8. Test 3 — FFT / Spectral Transform

Perform large:

- one-dimensional FFTs;
- two-dimensional FFTs;
- optionally three-dimensional FFTs.

A possible cycle is:

```text
source data
    ↓
forward FFT
    ↓
frequency-domain transformation
    ↓
inverse FFT
    ↓
verification
```

## Hardware stressed

- floating-point units;
- SIMD;
- integer indexing;
- caches;
- RAM;
- memory controller;
- array transpositions;
- multicore synchronization.

FFT is particularly interesting because its memory-access pattern changes throughout the stages of the transform.

## Verification

Perform:

```text
FFT → inverse FFT
```

and compare the reconstructed data with the original using appropriate floating-point tolerances.

## Load character

A combination of:

- significant arithmetic;
- changing locality;
- substantial memory movement;
- multicore synchronization.

---

# 9. Test 4 — Finite-Difference / PDE Simulation

Create a large simulated two- or three-dimensional physical field.

Examples:

- heat diffusion;
- wave equation;
- simplified fluid field;
- electromagnetic potential.

Each cell is updated from neighboring values.

Conceptually:

```text
new[x,y,z] =
    f(
        center,
        left,
        right,
        front,
        back,
        above,
        below
    )
```

Use two large buffers:

```text
state A → state B
state B → state A
```

repeatedly.

## Hardware stressed

- floating point;
- SIMD;
- integer address calculation;
- caches;
- RAM bandwidth;
- memory controller;
- multicore partitioning;
- synchronization at region boundaries.

## Load character

This is particularly useful for sustained:

- RAM reads;
- RAM writes;
- cache traffic;
- memory-controller traffic.

It should make an excellent long-duration memory-intensive reliability workload.

---

# 10. Test 5 — Particle / N-Body Simulation

Maintain a large collection of particles containing values such as:

```text
position
velocity
mass
force
other attributes
```

Possible simulation techniques include:

- direct N-body interaction;
- Barnes-Hut;
- octrees;
- spatial subdivision;
- molecular-dynamics-like neighbor lists.

## Hardware stressed

- floating-point arithmetic;
- SIMD;
- square root;
- reciprocal operations;
- integer indexing;
- branches;
- large dynamic data structures;
- RAM;
- caches;
- multicore load balancing.

## Verification

Possible invariants include:

- total momentum;
- total energy within numerical tolerance;
- center of mass;
- deterministic final checksum.

## Load character

A very broad mixed workload combining:

- numerical computation;
- irregular memory access;
- branches;
- dynamically changing data.

---

# 11. Test 6 — CPU Ray Tracing

No GPU is required.

Generate a synthetic 3-D scene containing:

- triangles;
- spheres;
- bounding boxes;
- procedural geometry.

Construct a spatial hierarchy such as a BVH and trace a very large number of rays.

## Hardware stressed

- floating point;
- SIMD;
- integer address calculations;
- branches;
- unpredictable control flow;
- cache hierarchy;
- RAM;
- shared scene structures;
- multicore scheduling.

Each ray can follow a substantially different execution path.

## Verification

The final image or accumulated pixel data should be deterministic.

Verification can use:

- image hash;
- pixel checksum;
- selected pixel recomputation.

## Load character

This provides a useful combination of:

- FP;
- branch pressure;
- irregular memory access;
- shared read-mostly structures.

---

# 12. Test 7 — Large Sorting / Merge / Database-Style Processing

Generate several gigabytes of structured records.

Each record may include:

```text
64-bit key
floating-point values
checksum
payload
```

Run cycles involving:

```text
generate
→ transform
→ partition
→ sort
→ merge
→ search
→ join
→ verify
```

Potential algorithms:

- radix sort;
- parallel quicksort;
- mergesort;
- hash partitioning;
- binary search;
- hash join.

## Hardware stressed

- integer ALUs;
- comparisons;
- branches;
- caches;
- RAM;
- memory writes;
- TLB;
- synchronization;
- multicore partitioning.

## Load character

Strongly data-intensive.

The individual phases create dramatically different patterns of:

- reads;
- writes;
- locality;
- branching;
- synchronization.

---

# 13. Test 8 — Compression / Decompression / Integrity Pipeline

Generate deterministic blocks with varying statistical properties:

- highly repetitive;
- partially repetitive;
- structured numerical data;
- high-entropy pseudo-random data.

Run a pipeline such as:

```text
generate
→ transform
→ compress
→ checksum
→ decompress
→ verify
```

The implementation does not need to rely on a large compression framework.

A simple internally implemented compression algorithm is sufficient for stress testing.

## Hardware stressed

- integer arithmetic;
- bit manipulation;
- branches;
- lookup tables;
- SIMD where useful;
- caches;
- RAM;
- multicore pipelines.

## Verification

The decompressed output must be exactly identical to the source.

This makes the test naturally self-verifying.

## Load character

Primarily:

- integer;
- branch;
- cache;
- data movement.

This complements the strongly floating-point-oriented scientific workloads.

---

# 14. Test 9 — Graph / Network Analysis

Generate a very large synthetic graph.

Possible algorithms:

- breadth-first search;
- shortest path;
- connected components;
- PageRank-like iterations;
- graph coloring;
- random walks.

## Hardware stressed

- integer operations;
- atomics;
- irregular RAM reads;
- irregular RAM writes;
- pointer/index chasing;
- TLB;
- cache hierarchy;
- cache coherence;
- multicore contention.

Graph workloads are naturally hostile to caches because neighboring logical graph elements may be physically far apart in memory.

## Load character

Particularly strong for:

- latency;
- irregular memory;
- coherency;
- synchronization.

This should be one of the principal tests for the CPU-memory interconnect and cache-coherence machinery.

---

# 15. Test 10 — Monte-Carlo / Statistical Simulation

Run a very large number of pseudo-random simulations.

Each worker performs a pipeline such as:

```text
random number generation
→ floating-point calculation
→ conditional branch
→ state lookup
→ accumulation
→ periodic global reduction
```

State arrays should be sufficiently large that RAM participates meaningfully.

## Hardware stressed

- integer arithmetic;
- random-number generators;
- floating-point units;
- SIMD;
- branches;
- caches;
- RAM;
- multicore reductions.

## Verification

Use deterministic seeds.

This allows:

- repeatable execution;
- deterministic checksums;
- statistical invariant checks.

## Load character

Large amounts of parallel independent computation interrupted by highly synchronized reduction phases.

This creates a very different multicore pattern from constantly synchronized algorithms.

---

# 16. Test 11 — Adaptive Mesh / Dynamic Data Structures

Create a simulated multidimensional region.

During execution:

```text
calculate
→ detect active regions
→ subdivide
→ calculate
→ merge inactive regions
→ rebalance
→ repeat
```

This resembles adaptive mesh refinement.

## Hardware stressed

Regular regions exercise:

- SIMD;
- floating-point arithmetic;
- streaming memory.

Adaptive phases exercise:

- integer logic;
- branches;
- pointer/index manipulation;
- allocation;
- irregular memory;
- TLB;
- multicore coordination.

## Load character

The key advantage is that the workload itself changes continuously.

The hardware does not spend several hours executing the same hot loop.

---

# 17. Test 12 — Dynamic Mixed-System Reliability Test

This should probably become the flagship LoadForge reliability test.

Different groups of threads simultaneously execute different computational workloads.

For example, on a 16-thread machine:

```text
threads  0–3    dense matrix operations
threads  4–5    FFT
threads  6–7    sparse solver
threads  8–9    graph traversal
threads 10–11   compression/integrity
threads 12–13   particle simulation
threads 14–15   stencil computation
```

After a configurable interval:

```text
roles rotate
working sets change
thread assignments change
dataset sizes change
```

At other points all workers may join one large common operation.

The workload can move through phases such as:

```text
compute-heavy
memory-heavy
coherence-heavy
balanced
mixed
transition
```

## Hardware stressed

Almost the entire CPU-memory platform:

- integer execution;
- floating-point units;
- SIMD;
- FMA;
- branches;
- cache hierarchy;
- RAM;
- memory controller;
- TLB;
- atomics;
- coherence;
- synchronization;
- multicore scheduler;
- dynamic voltage/frequency behavior;
- power delivery;
- cooling;
- thermal control.

## Load character

General-system reliability under a continuously changing workload.

This is intentionally different from a traditional monotonous stress loop.

---

# 18. Internal Primitive Kernels

Although the user-visible reliability tests should be complete workloads, smaller internal kernels remain useful.

Examples:

```text
integer.add
integer.multiply
integer.divide
integer.bitwise
fp32.multiply
fp64.multiply
fp64.divide
fp64.sqrt
simd.add
simd.multiply
simd.fma
cache.sequential
cache.random
memory.stream
memory.random
memory.pointer-chase
atomic.increment
coherence.pingpong
branch.predictable
branch.random
```

These kernels can serve several purposes.

## Calibration

Determine which CPU instructions and memory patterns are supported and how they behave.

## Diagnostics

If a complete reliability scenario fails, isolated kernels can help determine the likely hardware area involved.

## Automatic workload construction

LoadForge can combine these primitives when searching for especially demanding mixed workloads.

They should therefore form an internal **workload engine**, rather than being the primary definition of the reliability suite.

---

# 19. Reliability Verification

Every important load test should perform useful, verifiable computation.

The suite should detect not only:

- crashes;
- hangs;
- kernel errors;

but also:

- incorrect arithmetic results;
- silent RAM corruption;
- invalid numerical values;
- incorrect checksums;
- lost atomic updates;
- damaged shared data structures;
- stalled worker threads.

Possible verification mechanisms include:

- exact integer checksums;
- CRC;
- cryptographic hashes where useful;
- duplicate computation;
- independent recomputation;
- mathematical invariants;
- numerical residuals;
- known deterministic results;
- sequence numbers;
- cross-thread consistency checks.

Verification should occur periodically during long tests rather than only after completion.

---

# 20. Telemetry

A major part of LoadForge should be integrated telemetry.

The goal is not just to know whether the machine survived.

The tool should record exactly how the machine reacted to each workload.

---

# 21. CPU Telemetry

Collect where available:

- overall utilization;
- utilization per logical CPU;
- utilization per physical core;
- current frequency;
- average frequency;
- minimum frequency;
- maximum frequency;
- requested frequency if exposed;
- idle-state residency;
- boost behavior.

---

# 22. Thermal Telemetry

Collect:

- CPU package temperature;
- individual core temperatures where exposed;
- other relevant motherboard/platform sensors;
- memory temperature where available;
- peak temperature;
- average temperature;
- equilibrium temperature.

One particularly interesting derived quantity is:

```text
dT/dt
```

or temperature rise rate.

For example:

```text
Test A: +5.3 °C/s
Test B: +1.8 °C/s
```

Two tests can eventually reach similar equilibrium temperatures while one heats the silicon much more aggressively.

Therefore LoadForge should distinguish:

- **initial thermal aggression**;
- **steady-state thermal load**.

---

# 23. Fan Telemetry

Where available:

- fan RPM;
- requested fan state;
- fan response delay;
- fan speed versus CPU temperature.

This makes it possible to observe the complete thermal-control loop.

---

# 24. Power and Energy Telemetry

Where supported by hardware/kernel interfaces, collect:

- CPU package power;
- core-domain power;
- DRAM-domain power where available;
- energy consumed;
- power limits;
- power-limit events;
- throttling caused by package limits.

This makes it possible to identify:

- maximum-power workloads;
- power-efficient workloads;
- throttling regimes.

---

# 25. Memory Telemetry

Collect:

- total RAM usage;
- active working set;
- page faults;
- swap activity;
- memory bandwidth where measurable;
- NUMA information on applicable systems;
- huge-page use;
- memory-pressure events.

The reliability suite should normally avoid swap.

If the configured working set approaches unsafe levels, the test should reduce its allocation or refuse to start.

---

# 26. Hardware Performance Counters

Where available through Linux performance-event interfaces, collect:

- instructions retired;
- CPU cycles;
- IPC;
- branch instructions;
- branch misses;
- cache references;
- cache misses;
- selected architecture-specific events.

Useful derived metrics include:

```text
IPC
branch miss %
cache miss %
instructions/joule
```

The telemetry subsystem should gracefully degrade when a counter is unavailable.

---

# 27. Direct Linux Integration

To keep the footprint small, LoadForge should preferably obtain information directly from Linux interfaces rather than requiring external monitoring programs.

Useful sources include interfaces under:

```text
/sys/class/hwmon/
/sys/devices/system/cpu/
/proc/stat
/proc/meminfo
```

along with:

- Linux performance-event APIs;
- architecture-specific power/energy interfaces exposed by the kernel.

Utilities such as:

```text
sensors
turbostat
perf
```

remain useful as reference tools during development and validation, but LoadForge should ideally not require them for normal operation.

---

# 28. Timestamped Telemetry

All telemetry must share a common monotonic timeline.

Example:

```text
t = 0.000
phase_start: dense_matrix

t = 0.500
temperature = 52 °C
package_power = 31 W
frequency = 4.6 GHz

t = 1.000
temperature = 57 °C
package_power = 66 W
frequency = 4.3 GHz

t = 31.250
thermal_throttling = true

t = 120.000
phase_end
```

This allows direct correlation between:

- workload transitions;
- temperature;
- power;
- frequency;
- fan response;
- throttling.

---

# 29. Load Signature

A single concept such as "CPU percentage" is inadequate.

Each test should instead produce a multidimensional **Load Signature**.

Example:

```text
LoadSignature
{
    cpu_utilization
    package_power
    energy_consumed
    average_frequency
    minimum_frequency

    peak_temperature
    equilibrium_temperature
    temperature_rise_rate

    memory_bandwidth

    ipc
    cache_miss_rate
    branch_miss_rate

    thermal_throttle_time
    power_throttle_time

    verification_errors
}
```

For example:

```text
Dense Matrix

CPU utilization       100 %
Package power           72 W
Average frequency     3.82 GHz
Peak temperature        94 °C
Temperature rise       4.8 °C/s
Memory bandwidth       18 GB/s
IPC                     3.3
Thermal throttle        2.1 %
Errors                  0
```

versus:

```text
Graph Analysis

CPU utilization       100 %
Package power           49 W
Average frequency     4.21 GHz
Peak temperature        81 °C
Temperature rise       2.2 °C/s
Memory bandwidth       41 GB/s
IPC                     0.7
Cache miss rate        very high
Errors                  0
```

Both tests report 100% CPU utilization while creating very different physical conditions.

---

# 30. Composite Load Score

LoadForge may optionally calculate a single convenient overall score.

For example:

```text
CPU pressure        92
Memory pressure     68
Thermal pressure    89
Fabric pressure     43

Overall pressure    85
```

However, the multidimensional Load Signature should remain authoritative.

A single number inevitably hides important distinctions.

A test scoring lower overall might nevertheless be the strongest:

- memory test;
- cache-coherence test;
- thermal-transition test;
- power-delivery test.

---

# 31. Ranking Workloads

The suite should be capable of ranking tests by different objectives.

Examples:

```text
Highest package power
Highest peak temperature
Fastest temperature rise
Highest equilibrium temperature
Highest RAM bandwidth
Highest cache miss rate
Highest coherence pressure
Lowest sustained CPU frequency
Most thermal throttling
Most power throttling
Highest energy consumption
```

This would make the suite useful for both reliability testing and hardware characterization.

---

# 32. Automatic Worst-Case Workload Discovery

One particularly interesting LoadForge feature would be an automatic search mode.

The suite can vary:

- workload composition;
- thread count;
- CPU affinity;
- SIMD width;
- matrix/block size;
- memory footprint;
- data layout;
- working-set size;
- workload mixture;
- duty cycle.

The objective can be selected.

For example:

```bash
loadforge explore --objective temperature
```

or:

```bash
loadforge explore --objective package-power
```

or:

```bash
loadforge explore --objective throttling
```

The suite can experimentally determine what workload stresses a particular machine most severely.

This is more meaningful than assuming in advance that one instruction sequence must be the worst case.

---

# 33. Searching Mixed Workloads

The search should not be limited to homogeneous activity.

For example:

```text
cores 0–3     SIMD/FMA
cores 4–7     memory streaming
cores 8–11    integer/branch workload
cores 12–15   cache-coherence workload
```

may create a more demanding platform condition than:

```text
all cores = SIMD/FMA
```

The suite can search the space of mixed workloads.

This may reveal unexpected interactions involving:

- shared cache;
- memory controller;
- package power;
- thermal distribution;
- boost behavior;
- scheduler;
- power limits.

---

# 34. Static Load Versus Dynamic Load

Reliability testing should include both.

## Static tests

Run essentially the same computational structure for a long period.

Useful for:

- thermal equilibrium;
- long-duration arithmetic verification;
- DRAM reliability;
- sustained power behavior.

## Dynamic tests

Continuously change:

- algorithms;
- number of active threads;
- memory footprint;
- synchronization pattern;
- working-set size;
- computational intensity.

This repeatedly forces transitions in:

- clocks;
- voltage;
- CPU boost;
- thermal state;
- fan state;
- cache contents;
- memory activity.

Dynamic transitions may reveal faults that a perfectly constant five-hour workload never triggers.

---

# 35. Thermal Cycling

A specific reliability scenario should alternate:

```text
idle
→ heavy load
→ moderate load
→ heavy load
→ idle
```

or:

```text
2 cores
→ 8 cores
→ all cores
→ 4 cores
→ all cores
```

This repeatedly changes:

- package power;
- chip temperature;
- boost state;
- voltage;
- fan speed.

Thermal and power cycling may therefore deserve its own long-duration reliability mode.

---

# 36. CPU Topology Awareness

LoadForge should understand CPU topology.

It should distinguish:

- logical CPUs;
- physical cores;
- SMT siblings;
- cache-sharing groups;
- NUMA nodes where applicable;
- heterogeneous cores such as performance/efficiency cores.

Possible placement strategies:

```text
physical cores only
all logical CPUs
one thread per core
SMT siblings
selected cache domain
all cores
alternating cores
```

This allows reliability testing of different topology-dependent conditions.

---

# 37. Runtime Configuration

The suite should be highly configurable without requiring recompilation.

A declarative configuration format such as TOML would work well.

Example:

```toml
[[phase]]
test = "dense-linear"
duration = "20m"
threads = "all"
memory = "8GiB"

[[phase]]
test = "graph"
duration = "20m"
threads = "all"
memory = "70%"

[[phase]]
test = "dynamic-mixed"
duration = "60m"
threads = "all"
memory = "70%"
```

More sophisticated mixed configuration could look like:

```toml
[[phase]]
test = "dynamic-mixed"
duration = "30m"

[[phase.worker_group]]
threads = "0-3"
workload = "dense-linear"

[[phase.worker_group]]
threads = "4-7"
workload = "fft"

[[phase.worker_group]]
threads = "8-11"
workload = "graph"

[[phase.worker_group]]
threads = "12-15"
workload = "compression"
```

---

# 38. Suggested Operating Modes

Three top-level modes would make sense.

## Benchmark

Short controlled characterization:

```bash
loadforge benchmark
```

Purpose:

- compare workloads;
- characterize the machine;
- determine bandwidth, temperatures, power, etc.

## Stress

Long reliability testing:

```bash
loadforge stress --hours 5
```

Purpose:

- sustained reliability verification;
- hardware stability;
- thermal behavior.

## Explore

Automatic workload search:

```bash
loadforge explore --objective temperature
```

Purpose:

- discover worst-case workload combinations;
- build a machine-specific stress fingerprint.

---

# 39. Five-Hour Reliability Example

A comprehensive five-hour run might eventually resemble:

```text
10 min   baseline and hardware discovery

20 min   dense linear algebra
20 min   sparse solver
20 min   FFT

25 min   PDE/stencil simulation
25 min   particle simulation
20 min   CPU ray tracing

25 min   sorting/database workload
20 min   compression/integrity
25 min   graph analysis
20 min   Monte Carlo

25 min   adaptive mesh
40 min   dynamic mixed workload

25 min   thermal/power cycling
20 min   automatically discovered worst-case workload
```

Throughout the entire run:

```text
continuous telemetry
+
continuous result verification
+
system-error monitoring
```

The exact schedule should remain configurable.

---

# 40. Safety Controls

The suite must contain protective mechanisms.

Possible controls include:

- maximum allowed CPU temperature;
- maximum test duration;
- minimum available RAM;
- maximum memory allocation;
- emergency stop;
- graceful shutdown on telemetry failure;
- configurable cool-down period;
- detection of swap pressure;
- worker watchdogs;
- detection of stalled tests.

It should never intentionally depend on:

- undefined C/C++ behavior;
- integer overflow where behavior is undefined;
- invalid instructions;
- intentionally corrupting kernel memory;
- unsafe hardware register manipulation.

Reliability testing should stress valid hardware operation, not deliberately invoke undefined software behavior.

---

# 41. RAM-Backed Logging

Because SSD wear is intentionally outside the test objective, LoadForge should minimize persistent writes.

Telemetry can be kept in:

- RAM buffers;
- tmpfs;
- `/tmp` where backed by tmpfs.

A mode such as:

```bash
loadforge stress --log-memory
```

could store all intermediate logs in RAM.

Only final results would be written to persistent storage.

Typical output sizes should remain very small compared with hardware stress datasets.

---

# 42. Results

A run might generate:

```text
results/
    run.json
    telemetry.csv
    phases.csv
    errors.log
```

## `run.json`

Contains:

- LoadForge version;
- system identification;
- CPU model;
- CPU topology;
- kernel version;
- microcode version where available;
- memory size;
- configuration;
- test parameters;
- detected sensor mappings.

## `telemetry.csv`

Time-series measurements.

## `phases.csv`

Start and end of:

- tests;
- workload transitions;
- thread-role changes;
- thermal events.

## `errors.log`

Contains:

- verification failures;
- worker errors;
- unexpected operating-system events.

---

# 43. Reproducibility

Every test should record enough metadata to reproduce the run.

For pseudo-random tests, record:

- RNG algorithm;
- seed;
- generated-data configuration.

For CPU placement:

- affinity;
- logical CPU mapping.

For memory:

- working-set size;
- allocation strategy;
- page size;
- NUMA placement where applicable.

This makes results meaningful when comparing:

- BIOS changes;
- kernel versions;
- cooling configurations;
- power profiles;
- firmware updates;
- different physical machines.

---

# 44. Implementation Philosophy

LoadForge should remain lightweight.

The core implementation could reasonably use:

- C++20;
- Linux system interfaces;
- native threads;
- intrinsics where needed;
- selected assembly only where justified.

There is no need for:

- Python runtime;
- JVM;
- Electron;
- heavyweight numerical framework;
- large dependency graph.

The project can implement the relatively small amount of infrastructure required for its own tests.

---

# 45. Possible Source Structure

A possible organization:

```text
src/
    main/
    topology/
    scheduler/
    telemetry/
    verification/
    statistics/
    config/

    workloads/
        dense_linear/
        sparse/
        fft/
        stencil/
        particles/
        raytrace/
        sort/
        compression/
        graph/
        monte_carlo/
        adaptive_mesh/
        mixed/

    primitives/
        integer/
        floating/
        simd/
        cache/
        memory/
        branch/
        atomic/

    reporters/
        console/
        csv/
        json/
```

---

# 46. Important Architectural Separation

The project should clearly separate two layers.

## Layer 1 — Primitive mechanisms

Small low-level kernels used for:

- calibration;
- diagnosis;
- workload construction;
- automatic exploration.

## Layer 2 — Integrated reliability scenarios

Large complete workloads used for:

- reliability;
- thermal testing;
- sustained load;
- machine characterization.

The normal user interacts primarily with Layer 2.

---

# 47. Why Multiple Integrated Tests Are Necessary

There is no single universal maximum-load program.

Different workloads can conflict physically.

For example:

- a memory-latency workload can leave execution units idle;
- an extremely compute-dense SIMD workload may hardly touch DRAM;
- extreme coherency contention may serialize workers and reduce total power;
- streaming memory can heavily stress the memory controller while generating moderate core power.

Therefore attempting to maximize every subsystem simultaneously is not physically meaningful.

Instead, LoadForge should provide approximately a dozen **whole-system workloads with different dominant characteristics**.

Together they cover the complete operating envelope far better than a single artificial busy loop.

---

# 48. Distinctive Features

The project becomes particularly interesting if it combines these features:

## Integrated real algorithms

Reliability tests are based on genuine computational structures rather than meaningless instruction loops.

## Broad hardware participation

Tests naturally involve:

- cores;
- caches;
- RAM;
- memory controller;
- interconnect;
- power;
- cooling.

## Self-verifying computation

A test does not merely produce heat.

It continually proves that the hardware is producing correct results.

## Load Signature

The program records a multidimensional representation of hardware pressure instead of relying on CPU utilization.

## Automatic worst-case search

LoadForge can discover experimentally which workloads create the highest:

- temperature;
- power;
- throttling;
- memory pressure.

## Dynamic mixed workload

Different computational models can coexist and rotate across cores.

## Lightweight native implementation

The suite avoids unnecessary heavyweight dependencies.

---

# 49. Initial Test Set Summary

The proposed initial reliability suite is:

1. **Dense Linear Algebra**
2. **Sparse Linear Algebra / Iterative Solver**
3. **FFT / Spectral Transform**
4. **Finite-Difference / PDE Simulation**
5. **Particle / N-Body Simulation**
6. **CPU Ray Tracing**
7. **Sorting / Merge / Database Processing**
8. **Compression / Decompression / Integrity Pipeline**
9. **Graph / Network Analysis**
10. **Monte-Carlo / Statistical Simulation**
11. **Adaptive Mesh / Dynamic Structures**
12. **Dynamic Mixed-System Reliability Test**

Together, these create a deliberately broad collection of computational regimes while still exercising the complete CPU-memory platform.

---

# 50. Repository Name

The preferred GitHub repository name is:

```text
loadforge
```

Project name:

# **LoadForge**

The name reflects the central idea:

> LoadForge does not provide one fixed stress loop. It constructs — or **forges** — deliberate, complex hardware workloads.

A concise GitHub description could be:

> **Lightweight, self-verifying hardware load, telemetry and reliability testing framework for Linux.**

Or:

> **Composable whole-system CPU and memory reliability testing with integrated telemetry and workload analysis.**

---

# 51. Overall Project Definition

LoadForge can ultimately be described as:

> **A lightweight Linux framework for generating, verifying, measuring, and comparing complete computational workloads designed to stress the CPU, cache hierarchy, memory subsystem, multicore interconnect, power delivery, and thermal system under a variety of realistic computational regimes.**

Its principal value is not simply achieving maximum utilization.

Its value is discovering:

- **what kind of load the machine is experiencing;**
- **which hardware mechanisms that load stresses;**
- **how the physical machine responds;**
- **whether it continues to calculate correctly;**
- **and which workloads represent the worst reliability conditions for that particular computer.**