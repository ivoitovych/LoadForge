# The result schema

**Written:** 2026-10-20 12:58 UTC
**Schema version:** 3
**Provided by:** `src/controller/results/` (the tree), `src/reporters/json/` (`run.json`),
`src/reporters/csv/` (the CSV table). Specified by `docs/PLAN.md` F12 and
`docs/IMPLEMENTATION.md` M3.

A finished run is a `results::RunResult` (`src/controller/results/result.hpp`). Every
format renders it through one tree of plain values (`results::to_document()`), so what
a result contains is decided once and no format can leave a part of it out. This document
is that tree's shape, and then how each format writes it.

## Versioning

`schema_version` is the first member of every rendering and a decimal integer. It is
incremented, in the contract change that makes it, by any change to the result types that
changes what a rendering means. A reader of a result **refuses a version it does not know**
rather than misreading it: a field that moved or changed its unit under an unchanged
version would compare two runs that are not alike.

Version 1 is the first. Version 2 is the M3 corrections. Version 3 (M2 contract corrections):
a provenance carries `physical_package`, `decomposition` and `footprint_bytes`;
`physical_core` is the core's id within its package; and a workload carries `memory_lock`
(below).

## Absence

Nothing in a result is ever "zero because it was not there".

| What | In the tree | JSON | CSV |
|---|---|---|---|
| A value the result leaves empty (an optional) | null | `null` | an empty cell, `status` `absent`, `reason` `not recorded` |
| A reading that was unavailable | an absence: kind, words, path, detail, remediation | `null`, and the reason as the member `<name>_unavailable` beside it | an empty cell; the kind's code in `status`, its words in `reason`, and the path, detail and remediation each in their own column |
| An empty list or object | empty | `[]`, `{}` | one row, `status` `empty` |
| A real that is not a number (a residual may be NaN or infinite) | the text `nan`, `inf` or `-inf` | a string, never `null` and never a number | the same text |

The kind's code is one of `absent`, `vanished`, `denied`, `malformed`, `unreadable` (the six
capability states of `topology::Availability`, minus `present`); its words are
`topology::describe()` ("not permitted", "not offered by this kernel", ...); the path,
detail and remediation are the source's own, verbatim.

## The tree

Members appear in this order. Names are `snake_case`. Unsigned 64-bit values are written in
full decimal; a reader that holds numbers as doubles loses them above 2^53, and the ones
that can be that large (a buffer address, a seed, a counter) should be read as integers.

```text
schema_version            unsigned
mode                      "benchmark" | "stress" | "explore"
elapsed_ns                signed
verdict                   { code, words }
                            code:  "clean" | "corrected_errors" | "incomplete" | "findings"
                            words: "clean" | "corrected errors" | "incomplete" | "findings"
fingerprint               (below)
capabilities              [ source ]
signature                 { window, metrics }
workloads                 [ workload ]
workers                   [ worker ]
observers                 { sample_interval_ns, journal_flush_interval_ns | null,
                            controller_overhead | null }
```

### fingerprint (F12)

```text
loadforge_version  git_commit | null  compiler  flags
cpu_model | null  microcode | null  kernel | null  smt_enabled | null
power_source | null  battery_state | null  cpufreq_governor | null
energy_performance_preference | null  platform_profile | null  turbo_enabled | null
cooling_profile | null  cgroup_memory_limit_bytes | null  huge_page_policy | null
temperature_at_start_c | null
fp_environment   null | { rounding, flush_to_zero, denormals_are_zero | null, default_nan | null }
isa_path         null | "scalar" | "avx2" | "avx512" | "neon"
absent_fields    [ names of every member above that is null ]
```

`absent_fields` is there because the contract's fingerprint carries no reason for an absence;
the reason a field could not be read (which file, which errno's meaning) is in the
`FingerprintReading::gaps` the reader returns, and is for the run's own report. The
fingerprint reader (`read_fingerprint()`) takes each field from the machine as follows:

| Field | Source |
|---|---|
| `cpu_model`, `microcode` | `/proc/cpuinfo`, first CPU's `model name` and `microcode` lines. arm64's `cpuinfo` prints neither: absent. |
| `kernel` | `/proc/sys/kernel/osrelease` |
| `smt_enabled` | `/sys/devices/system/cpu/smt/active` (`1`, `0`) |
| `power_source` | `online` of the mains adapter, `/sys/class/power_supply/{AC,ACAD,ADP0,ADP1,AC0}`: `1` is `ac`, `0` is `battery` |
| `battery_state` | `status` of `BAT0`, else `BAT1`, in the kernel's words |
| `cpufreq_governor`, `energy_performance_preference` | cpu0's `cpufreq` directory |
| `platform_profile` | `/sys/firmware/acpi/platform_profile` |
| `turbo_enabled` | `intel_pstate/no_turbo` (`0` is on), else `cpufreq/boost` (`1` is on) |
| `cooling_profile` | `/sys/class/thermal/thermal_zone0/policy` |
| `cgroup_memory_limit_bytes` | the limit that binds the memory cgroup (`resources::read_memory_budget`); absent when no limit applies, and also when it could not be read (the gaps tell which) |
| `huge_page_policy` | the bracketed word of `/sys/kernel/mm/transparent_hugepage/enabled` |
| `temperature_at_start_c` | `/sys/class/thermal/thermal_zone0/temp`, millidegrees, divided by 1000 |
| `fp_environment`, `isa_path` | not on the machine's files: the caller read them |

Two fingerprints from one machine agree in every field but `temperature_at_start_c`, which
describes the moment (`is_moment_field()`); `differing_fields()` lists the rest.

### capabilities

```text
source   { source, metrics: [ { metric, availability } ] }
metric   { name, unit, kind: "gauge"|"counter", scope, scope_index, modulus | null, provider }
availability   "present"  |  an absence (null in JSON with availability_unavailable beside it)
```

### signature

```text
window    { start_ns, end_ns }
metrics   [ { metric, unavailable_samples, summary } ]
summary   { type: "gauge", samples, min, max, mean, stddev | null, first, last,
            slope_per_second | null }
        | { type: "counter", samples, delta, rate_per_second | null }
        | an absence
```

### workload

```text
descriptor   { name, version, determinism, contract, paths: [ isa names ] }
isa  decomposition  duty { oracle_period, integrity_period }
tally        { units_computed, oracle_checked, oracle_blocks, oracle_failed,
               oracle_not_performed, integrity_checked, integrity_failed }
logical_bytes
findings     [ finding ]
lock_policy  "require" | "try" | "none"      what the controller chose for the working set's lock
memory_lock  null | { outcome, detail | null }
               outcome: "locked" | "not locked" | "not locked: the lock was refused"
               null when the worker never said; detail is the refusal when the lock was refused
```

### finding

```text
record      (below)
isolation   null | { steps: [ { rung, cpu, threads, outcome } ],
                     answers: { same_core, other_core, single_thread },
                     finding, confidence }
telemetry   [ { metric, timestamp_ns, reading } ]
              reading: { type: "gauge", value } | { type: "counter", raw } | an absence
```

### record

Every record has `kind` (`ExactBitCorruption`, `NumericalVerificationFailure` or
`VerificationNotPerformed`, from `kind_name()`) and `provenance`:

```text
provenance  logical_cpu | null  physical_package | null  physical_core | null  numa_node | null
            buffer_address | null  buffer_offset | null  iteration | null
            workload | null  workload_version | null  seed | null
            isa | null  input_integrity | null  duty | null { oracle_period, integrity_period }
            decomposition | null  footprint_bytes | null
```

then, by kind, and **only** these (a field that does not exist is not written as zero):

```text
ExactBitCorruption             side  check  line_bytes
                               difference   null | { domain, first_offset, last_offset, popcount,
                                                     window_offset, expected_hex, actual_hex }
                               bit_class    null | "single bit" | "multiple bits in one word" |
                                            "within one cache line" | "across cache lines"
                                            (null exactly when there is no difference)
NumericalVerificationFailure   check  residual  tolerance  ratio  condition_number | null  element | null
VerificationNotPerformed       what  why  blocks_compared
```

### worker

`{ slot, fate, cpu | null, termination }`; `fate` is `controller::describe(WorkerFate)`. `termination` is the controller's account of how the worker ended. When the temperature ceiling ended the run, every worker's ends with "; the run was ended by the temperature ceiling: <sensor> read <x> C, at or above the ceiling of <n> C, <t> s into the run": the schema has no run-level field for why a run ended (`docs/journal/34a-temperature-ceiling.md` D8).

## JSON (`run.json`)

RFC 8259, UTF-8, two-space indentation, members in the order above, `\n` line ends and one
closing newline. Equal results give byte-identical text. A control character in a string
is escaped; a byte that is not UTF-8 (a kernel string is whatever the driver wrote) becomes
U+FFFD, one per offending byte, so the document stays valid.

A reader that needs the reason an absent member was absent reads `<name>_unavailable`:
`{ "kind", "words", "path", "detail", "remediation" }`.

## CSV

RFC 4180: `CRLF` record ends including the last, a header row, a field quoted when it holds a
comma, a quote, a CR or an LF, quotes doubled. One row per leaf value:

| Column | Holds |
|---|---|
| `schema_version` | the schema version, on every row |
| `field` | the leaf's path: members joined by `.`, array items as `[index]` (`workloads[0].tally.oracle_blocks`) |
| `value` | the value; an empty cell when there is none |
| `status` | `present`, `absent`, `empty`, or an unavailable reading's kind code |
| `reason` | `status` in words; empty for `present` |
| `source_path`, `detail`, `remediation` | an unavailable reading's own, verbatim |

The first data row is `schema_version`. A spreadsheet that opens a cell beginning with `=`,
`+`, `-` or `@` as a formula will do so for a workload or check name that begins with one;
names a workload registers are lower-case identifiers, and the free text a check carries is
the one place it could happen.
