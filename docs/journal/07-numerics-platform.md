# Journal fragment 07 — numerics platform

**Written:** 2026-10-20 11:05 UTC
**Change:** stream 07 — `src/platform/fpenv`, `src/platform/isa`, `select_path()` in
`src/primitives/simd`.

## Established by experiment

Machine: x86-64, Intel Xeon @ 2.10 GHz under a hypervisor, Linux 6.18.44, GCC 13.3. The
AArch64 half is written to the Arm ARM and compiled on neither this machine nor, here, run;
CI's ARM64 job is its first execution (see "Left open").

### A new thread inherits the creating thread's MXCSR

The contract says a started thread inherits the starter's floating-point state and leaves
the provider to confirm it against the real kernel (F21). `SubnormalTest.AThreadStarted…`
(`tests/determinism/platform/fpenv/`) sets flush-to-zero in the main thread, starts a
`std::thread`, and reads the state there: the flag is on. A write in the child does not
reach the parent. So "establish before starting any thread, assert on each thread's start"
is the correct reading, and both halves are tests.

### The kernel's `/proc/cpuinfo` flags and the probe agree here

`avx2`, `fma` and `avx512f` are all in the flag list of this machine, and the probe
reports `kAvx2` and `kAvx512`; `__builtin_cpu_supports` gives the same answers. Linux
removes a feature's flag when the xfeature it needs is not enabled, so the flag list is
the kernel's own verdict on the OS-support half of the question; the comparison is in
`tests/integration/platform/isa/`.

### `INSTANTIATE_TYPED_TEST_SUITE_P` cannot take a qualified suite name, and a header with two suites needs both accounted for

The macro pastes the suite name into identifiers, so `contracts::FpCheckContract` fails to
compile; the instantiation has to sit in `namespace loadforge::contracts`, where the suite
was declared. A comma inside the `Types<...>` argument is split by the preprocessor into a
fourth macro argument: give the list a name with `using` first. And a contract header that
declares two suites (`dispatch_contract.hpp` declares `SelectPathContract` and
`IsaProbeContract`) makes gtest's own `UninstantiatedTypeParameterizedTestSuite` check fail
in any test binary that includes it and instantiates only one. The two suites belong to two
modules, hence two binaries; each marks the other module's suite with
`GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST`. The ledger gate reads
`INSTANTIATE_TYPED_TEST_SUITE_P` only, so this does not satisfy a `provides` by accident.

## Decisions

### The register layouts are pure functions compiled on both architectures

*Options:* (a) `#if` the whole of `real_control.cpp` per architecture and test only the
native layout; (b) put the bit arithmetic in `registers.cpp` for both layouts and `#if`
only the instruction that moves the register. *Chose (b).* The ARM layout is then a unit
test on x86-64 and the reverse, so a transcription mistake in the architecture CI is not
running first is still caught; the real-register test then compares the native layout with
the hardware, using an instruction of its own rather than the code under test.

### Inline assembly, not builtins or `<xmmintrin.h>`

The portability workflow refuses `_mm_` names outside `src/primitives/simd/x86/`, and
`_mm_getcsr` is one. `stmxcsr`/`ldmxcsr` and `mrs`/`msr fpcr` are the instructions
themselves, with a `"memory"` clobber on the writes (the compiler may not move a
floating-point operation's memory traffic across them), and an `isb` after the FPCR write
so the new mode is in force before the next instruction. `cpuid` and `xgetbv` likewise.

### A write is read-modify-write

MXCSR carries the exception masks and the sticky flags, and FPCR the trap enables; a write
that rebuilt the register from the four properties would clear them. `encode_*` changes
only the owned bits (tested, on the real register too).

### `establish_environment` derives the architecture from `read()`

The flags a control reports are the flags it has (the contract: absent, not false), so
`establish_environment(FpControl&)` needs no architecture parameter. It writes the required
state and then asserts: a register that did not take the write is reported by the read, as
the contract directs. `assert_environment` never writes. Both live in
`platform/fpenv/environment.hpp`, a header of this module: `fpenv.hpp` is frozen.

### The ISA probe reads once

`SnapshotProbe` computes its set in the constructor and returns a constant. The contract
requires "the same answer on every call in a process" and a hypervisor can change what
CPUID says between reads; a constant makes the property true by construction and the probe
trivially thread-safe. `detect_paths()` is the pure decision and takes the architecture as
an argument, so the AArch64 rule is tested on x86-64 and the reverse, and a probe never
reports another architecture's path whatever bits its source shows.

### What `kAvx512` requires

AVX512F in leaf 7 EBX, OSXSAVE, and XCR0 bits 1, 2, 5, 6 and 7 (`0xE6`). The contract names
bits 5 to 7; bits 1 and 2 are added because ZMM state is the upper half of YMM state and a
machine that saves opmask and ZMM but not YMM is not a machine this code should believe.
AVX-512 does not require the AVX2 *CPU bits* in the probe: the two paths are independent
questions, and a combination that reports one without the other is more likely a broken
source than a real CPU, which the independent rule reports faithfully rather than hides.
`kAvx2` requires AVX2, FMA, AVX, OSXSAVE and XCR0 bits 1 and 2.

### Leaf 7 is never read on a CPU that does not have it

CPUID with a leaf above the highest returns the data of the highest basic leaf, not zeros.
The probe reads leaf 0 first and answers scalar-only below leaf 7. The fake source records
a read of a leaf above the highest, and of XGETBV without OSXSAVE (which faults on real
hardware), as faults, and the tests assert none happened.

### `select_path` with a request outside the enumeration

A value that is not one of the four paths is in no set (`IsaSet::contains`), so the request
is refused as not compiled, with the path named "unrecognised ISA path". Not an error the
contract names, but the only one of its two that is true.

## Contract questions

1. **`FpState` write and `check()` for the other architecture's flag.** `FpControlContract`'s
   `AnAbsentPropertyIsLeftAsItIs` writes a state with the *other* architecture's flag
   absent; this module reads it as "leave the register alone" and a flag that exists on the
   architecture but is absent in the state is also left alone, per the header. Both
   readings agree with the header text; stated here so a consumer that writes
   `FpState{}` expecting a reset knows it gets no reset of the flags.
2. **`IsaProbeContract` with `LOADFORGE_SIMD=scalar`.** `kArchitecture` is the
   *machine's* architecture for this module's real probe, in every build: the scalar-only
   build restricts what is *compiled* (`compiled_paths()`), not what the CPU *supports*,
   and `select_path()` takes the intersection. The reference instantiation in
   `tests/contracts/simd/` names `"other"` in the scalar build; both are consistent with
   the suite, which only forbids paths of other architectures. A consumer should not read
   `supported()` as "what this build can run".
3. **Who calls `establish_environment` after a library load.** The contract says the state
   is re-asserted "after any point where a third party could have changed it" and records it
   in the fingerprint. `assert_environment` returns the error; whether a failed re-assert
   re-establishes or reports is the integration stream's call. This module gives both.

## Left open

- The AArch64 register access (`mrs`/`msr fpcr`) and `getauxval(AT_HWCAP)` path are
  written to the Arm ARM and the layouts are tested on this machine; they have not run on
  hardware here. CI's ARM64 job runs `FpControlContract` against the real FPCR, the real
  register comparison, the T6 tests and the T8 `/proc/cpuinfo` comparison (`Features` line,
  `asimd`) for the first time.
- `src/primitives/simd` has no library consumer yet: the linking into the product is the
  integration stream's.
