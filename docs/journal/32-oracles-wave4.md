# Journal fragment 32 — The wave 4 oracles: ray tracing and adaptive mesh

**Written:** 2026-10-20 17:50 UTC
**Stream:** 32, `feature/oracles-wave4` (`docs/streams/32-oracles-wave4.md`)
**Modules:** `src/verification/oracle/raytrace` and `src/verification/oracle/adaptive_mesh`,
written from `src/workloads/raytrace/spec.hpp` and `src/workloads/adaptive_mesh/spec.hpp`
and from nothing of either workload (no workload source, test or mock was read).

## Established by experiment

### A single bit flip of a generated unit is mostly seen, and what is not seen is exactly what the specification does not ask

Measured by the T5 suites on the debug build, x86-64, 2026-10-20.

- Ray tracing, unit 13 of seed 77: all 8192 bits of the 64 sampled pixels and all 1024 bits
  of the 16 tile sums at edge 16 (and the sums at edges 8 and 32) are caught as a
  single-bit `ExactMismatch` of the right item at the right offset. Of the 2048 bits of
  six pixels that no sample looks at (three hits, three misses of tile 0), the tile sum
  catches the flips that change a distance enough to move the sum or that make the record
  malformed; the others — the index of a hit that stays below 256, the lowest bits of a
  distance that the rounding of the sum absorbs — leave a record the specification
  accepts and a sum that is still the sum of the stored records, and the oracle
  agrees. The test derives that expectation itself, record by record, and asserts the
  verdict matches it for every one of the 2048 flips.
- Adaptive mesh, unit 19 of seed 91: all 8192 bits of the 256 leaf counts are caught with
  the residual 2^bit; of the 2048 bits of the 32 sampled integrals, the flips below the
  cell's tau (the low mantissa bits) are agreed with and the rest caught. A single flip of
  a finite integral of this size cannot make a NaN or an infinity (that takes an exponent
  of all ones), so those two are tested by writing them directly.

### The brute-force trace and the specification's arithmetic agree with geometry done another way

There is no reference tool for a synthetic scene. T3 therefore re-solves six generated
scenes (seed 31, units 0–5, 24576 pixels) in `long double` with a different formulation —
the quadratic in `t` for a sphere; the plane's `t` and the signs of three edge functions
for a triangle — and requires the same nearest primitive at a distance within 1e-9
(relative) wherever a slightly stricter and a slightly looser hit test agree. Pixels on an
edge (where they disagree) are skipped, and the test requires they be under 2% of all.
They matched on every judged pixel. Two scenes with an answer known by hand (a triangle in
the plane z = 0 is met at t = 4 exactly where the ray falls inside it; a sphere on the axis
at its nearer root) agree too.

### The specification's bound for the leaf-order sum holds on every coarse cell of generated units

Every `x_leaf` is a multiple of 2^-75 below 2^9, so a coarse cell's exact integral E fits
an unsigned 128-bit integer scaled by 2^75. On units 0–3 of seed 21, all 1024 coarse
cells have |c' − E| ≤ γ_(m−1) E (step 2 of the specification), some with rounding, and on
the 1024-leaf cell the reverse and the pairwise order of adding are both inside
|c − c'| ≤ 2 γ_(m−1) E and inside τ, so the oracle agrees with an output built from
either. The specification's figures for a generated unit hold: 3000–4400 leaves in all, at
most 80 in a cell (the specification says "about 50"; the test's bound is looser, the
measured maximum is not recorded here because the test does not assert it).

### Resolution of the tolerance on a crowded cell

On the 1024-leaf cell (256 hotspots, one to each level-8 cell), τ spans more than 1000
ulps of c', so the oracle holds a stored integral at 99% of τ and refuses it at 101%, in
both directions; on a one-leaf cell τ is about one ulp, and the oracle holds one ulp and
refuses three.

### Runtime of the suites

On the debug build the T5 suites take about 11 s (ray tracing) and 5 s (adaptive mesh).
A check costs, at the worst, 64 traces of 256 primitives for ray tracing and a recursion
over about 5000 cells for adaptive mesh.

## Decisions

### Which pixel of tile j is sample j (ray tracing)

The specification fixes the tile of sample j (tile j of the 8 × 8 tiling) and says the pixel
is "a function of the unit index alone". Options: the tile's corner (no mixing; every unit
would check the same 64 pixels, so a hierarchy that is wrong somewhere else is never
seen), the tile's centre (the same objection), or a mixed pixel. Chosen: the pixel at
(`bits % 8`, `bits / 8 % 8`) within the tile, where `bits` is splitmix64's finaliser of
`mix(index) + j`. Different units check different pixels, a re-run checks the same ones,
and all 64 positions in a tile are reached (a test over 400 indices).

### Which cell of the eight is sample j (adaptive mesh)

Same reasoning: `8 j + mix(mix(index) + j) % 8`.

### The tile sums at and after tile_count(edge) are not looked at

The specification says those slots "are zero bytes", but its list of items stops at
`tile_count(edge) − 1`, and the oracle "evaluates the items" only. Followed: they are not
items (tested: a flipped bit there is agreed with). See Contract questions.

### What a malformed tile's check says

The specification fixes the beginning of the check (`tile sum`, a space, the index) and says
the text "after the index then names the first malformed pixel". Chosen:
`tile sum 3: the stored record of pixel 517 is not well-formed`. "First" is first in the
tile's scan order (rows from the top, left to right), as in the sum itself.

### How create() resolves the decomposition (ray tracing)

A configured 0 resolves to `kDefaultTileEdge`; any other configured value is its own
resolution and must be a tile edge. `create` refuses a plan whose decomposition is not that
resolution, and a plan whose sizes are not the specification's. The oracle is therefore
only ever made for the plan the workload resolved from the same configuration.

### The oracle's arithmetic is the specification's helpers, called directly

The specification requires both sides to call the helpers of its header (`ray_sphere`,
`ray_triangle`, `hotspot_term`, `cell_centre`, `leaf_area`, `cell_tolerance`) so that no
last bit differs. The algorithms are the oracle's own: brute force over every primitive in
index order for ray tracing; a recursion that carries only the hotspots inside the coarse
cell (the only ones that can split a cell below it) for the mesh, with the values of the
leaves summed over all hotspots in index order, as the specification fixes. The tests' model
of each output is deliberately built the other way (primitive-major with a running nearest;
occupied-cell sets and an explicit stack) so that the oracle is never judged against itself.

### No P2, no seam

Neither oracle makes a system call, and the verdict's strings are the only allocation. The
compression oracle's scratch seam has no counterpart here (nothing is decoded into
scratch: a trace needs a scene on the stack, 13 KiB, and a mesh needs 2 KiB of hotspots).
Neither module therefore declares P2, and the ledger says why.

### The classes and tiers of the ledger

The brief's starting classes (P1, P3, P4) and tiers (T1, T3, T5, T5b) are kept as written
for both modules. T3 has no committed fixture: there is no reference tool for a synthetic
scene or mesh, and a fixture made by the oracle's own code would prove nothing (F21). Its
evidence is mathematics, which the ledger note names for each module. `fixtures` is
therefore absent, and no P7 is declared.

## Contract questions

1. **Raytrace, the pixel inside each tile.** The specification does not say which pixel of
   tile j is sample j (see Decisions). If the workload's own tests, or the integration
   stream, need to know the sample, it should be named in the specification;
   `sampled_pixel(index, j)` in `verification/oracle/raytrace/oracle.hpp` is the one the
   oracle uses.
2. **Raytrace, the tile-sum slots past `tile_count(edge)`.** The specification says they are
   zero bytes and does not make them an item. A workload that wrote garbage there would be
   agreed with. Read as written; if they should be checked, an item is needed after the
   tile sums, and `blocks_compared` for an Agreement changes by one.
3. **Raytrace, the difference of a failed pixel.** The specification fixes the window as
   running "from window_offset to the end of the item". Followed: the window of a pixel is
   at most 16 bytes, one `kWordBytes` word shorter when the first differing byte is in the
   second word. Combined with the 16-byte alignment of the records, a single item lies
   within one 64-byte line, so `BitClass::kAcrossCacheLines` is unreachable from this
   oracle at the line size 64; at a line size of 8 or 16 (the topology's, never assumed)
   it is reachable.
4. **Adaptive mesh, the condition number.** The specification says "condition_number
   absent" for the integral item and is silent for the leaf count; absent for both.
5. **Adaptive mesh, `cell_tolerance` precondition.** `reference` must be finite and
   positive. For hotspots below `kLattice` that always holds (every `x_leaf` is above 2^-23),
   so the oracle calls it without a guard; a guard would be an uncoverable branch.
6. **Adaptive mesh, `blocks_compared` for the integral items** counts the 256 leaf counts
   first, so the first integral failure at sample j has `blocks_compared = 256 + j + 1`;
   that is what the specification's "items evaluated" says, and it is what the tests assert.
7. **Specification text, "about 50 leaves in a cell".** The T3 test bound is 80, because the
   measured maximum was not recorded; the specification's figure is "(measured)".

## What is left open

- The registry (`OracleRegistry`) is the integration stream's; this stream provides
  `create(config, plan)` in each module's header for it to call, and runs `OracleContract`
  only. The suite `OracleRegistryContract` is allowed uninstantiated in each test binary.
- The coverage gate, the sanitizer, scalar-only and static builds, clang-tidy and the
  repeated runs were run as listed in the commit history and the final report.
