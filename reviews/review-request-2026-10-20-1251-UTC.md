# Review request

- **Date and time:** 2026-10-20 12:51 UTC.
- **Series:** review requests are dated and kept, one file each, in `reviews/`; the newest is
  the current one. The previous one is
  [review-request-2026-10-20-0352-UTC.md](review-request-2026-10-20-0352-UTC.md); its general
  introduction (what LoadForge is, how to build it, how to report) still applies and is not
  repeated here.

## Why this request

The first independent review ([review-response-2026-10-20-0352-UTC.md](review-response-2026-10-20-0352-UTC.md))
covered process supervision and found two major defects that every internal gate had passed
(a successful `kill` taken as proof of causing a death; spawn time eating the heartbeat
allowance). All three of its findings are accepted and queued
([reaction plan](reaction-plan-external-review-2026-10-20-1230-UTC.md)).

Since then the project has grown by about 30,000 lines in nine pull requests, written in
parallel against frozen contracts (ADR 0004). **Not one line of them has had an outside
reader.** They are the trust core of the tool: the code that decides whether a computation on
the machine under test was right. A defect there produces the worst outcome the project
names: a confident, wrong verdict about hardware.

## What to review, most valuable first

Each item is an open pull request on <https://github.com/ivoitovych/LoadForge/pulls>. Review in
the PR (inline comments are ideal) or against its branch. Each PR has a journal fragment
(`docs/journal/NN-*.md`) recording the stream's decisions and its open "Contract questions";
disagreeing with a recorded decision is a finding.

### 1. The verification oracles — can a wrong result be accepted?

- **#41, compression oracle** (`src/verification/oracle/compression/`): an independent, strict
  LZ4 block decoder that judges the compression workload's output. It is deliberately
  stricter than the reference decoder (journal fragment 03, decisions 1-3). Questions: can a
  block that does not decode to the input be accepted? Is any of the strictness wrong, so that
  a correct compressor would be refused? Is "another valid encoding of the same input" (a bit
  flip the oracle cannot and should not catch) argued correctly?
- **#42, bounded verification** (`src/verification/residual/`,
  `src/verification/oracle/dense_linear/`, `src/verification/invariant/`): the derived
  floating-point tolerance for a dense matrix product checked on sampled entries. Questions:
  is the error bound (Higham's dot-product bound plus underflow; the header carries the
  derivation) correct and tight? Can a wrong product pass? Can a correct one, computed by any
  legal summation order with or without FMA, fail? Is the sampling (8 x 8 tiles, one entry
  each, chosen from the unit index alone) enough to catch a corrupted panel?
- **#39, verification core** (`src/verification/`): the record codec, exact comparison,
  CRC-64/XZ input seal, golden vectors. Questions: can a corrupted record decode as a valid
  different one? Is the "first problem in byte order" rule sound?

### 2. Concurrency

- **#38, executor** (`src/worker/executor*`): a fixed thread pool; indices claimed from one
  seq_cst counter; every wait's first test fixed by the protocol. Questions: lost wake-ups,
  a run that never finishes, a task run twice or never; correctness on ARM64's weaker model.
- **#40, input integrity** (`src/verification/integrity/`): the slice runner, its bracket
  around each unit, and control state kept in triplicate with voting. Questions: can a
  corruption of the input or of the control state go unreported, or be reported as a wrong
  result of the computation?

### 3. Hardware-facing code

- **#37, numerics platform** (`src/platform/fpenv*`, `src/primitives/simd/`): MXCSR and FPCR
  access by inline assembly, the ISA probe (CPUID; `getauxval` on ARM64), path selection.
  The ARM64 path has run only on CI's ARM64 runners. Questions: is the assembly correct and
  complete (clobbers, reserved bits)? Can the probe claim an ISA the CPU or OS cannot run?
- **#44, dense-linear workload** (`src/workloads/dense_linear/`,
  `src/primitives/simd/{x86,arm}/`): scalar, AVX2 (target attribute) and NEON kernels.
  Questions: undefined behaviour in the intrinsics code (aliasing, alignment); a path that
  silently uses FMA by contraction.
- **#43, compression workload** (`src/workloads/compression/`): an LZ4-block compressor and
  its generator. Questions: can it write a block the specification forbids?
- **#45, isolation** (`src/isolation/`): the re-run ladder that tries to tell a transient
  fault from a core-specific one by re-running on the same core and on the nearest other
  physical core. Questions: is "nearest" the right choice (journal fragment 10, decision 2)?
  Can the ladder name a core as faulty on evidence that does not support it?

### 4. The gates themselves

- **#46, sharded mutation testing**: one CI job per module; mutants run only against tests
  that create no process. Its first run found 56 surviving mutants (journal §2.28), now being
  killed. Question: does excluding the process-creating tests hide anything that matters?
- **#47, commit hooks in the repository**: the identity, sign-off and trailer policy enforced
  before commit and before push, mirroring the CI gate. Question: can a commit that CI would
  refuse get past the hooks, or the reverse?

## Already known, so you need not report them

- The three findings of the first independent review (EXT-01 to EXT-03), queued as ROADMAP
  items 10b-10d.
- Each PR's own "Contract questions" section in its journal fragment: these are known
  ambiguities, to be settled in one contract-corrections change after the wave merges. A
  reading you think is *wrong*, rather than ambiguous, is still a finding.
- The 56 mutation survivors listed in journal §2.28.

## How to report

As in the previous request: a GitHub issue, inline comments on the PR, or an email to the
maintainer, Iaroslav Voitovych. For each finding, the file and line, what goes wrong, and how
to make it happen. Every finding will be answered, and each that holds will be fixed with a
test that fails without the fix. A partial review of one area is worth more than a skim of
all of them.
