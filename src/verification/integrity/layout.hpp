// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_INTEGRITY_LAYOUT_HPP
#define LOADFORGE_VERIFICATION_INTEGRITY_LAYOUT_HPP

// Where a slice runner keeps its working set: one mapping, every region in it
// aligned to the cache-line size, so that every unit's input and output start
// on a line boundary (runner.hpp, record.hpp's Provenance) and an offset into
// a compared buffer has the word and line boundaries the hardware has.
//
//   [ slack ][ inputs: units x input_stride ][ outputs: units x output_stride ]
//            [ regenerated: input_stride ][ scratch: scratch_bytes, rounded ]
//
// The slack is one line: the mapping's start is page-aligned on the real
// kernel, but a line size larger than a page is a power of two the contract
// accepts, and the system-call fake hands out heap memory. The runner aligns
// up inside the slack instead of assuming either.
//
// Computed here, as a pure function, so the arithmetic's overflow arm -- a
// plan whose working set does not fit a size_t -- is tested directly rather
// than through a mapping nobody could make (IMPLEMENTATION §2.6, rung 1).

#include <cstddef>
#include <cstdint>

#include "core/result.hpp"
#include "verification/runner.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::integrity {

static_assert(sizeof(std::size_t) == sizeof(std::uint64_t),
              "LoadForge targets 64-bit Linux: a plan's 64-bit sizes are byte counts");

/// Byte offsets, from the line-aligned base, of each region; and the sizes.
struct Layout {
  std::size_t line_bytes = 0;
  std::size_t input_bytes = 0;    ///< plan.input_bytes: one unit's input.
  std::size_t output_bytes = 0;   ///< plan.output_bytes: one unit's output.
  std::size_t units = 0;          ///< plan.units: the slots.
  std::size_t input_stride = 0;   ///< input_bytes rounded up to a line.
  std::size_t output_stride = 0;  ///< output_bytes rounded up to a line.
  std::size_t outputs = 0;        ///< Offset of slot 0's output. Inputs start at 0.
  std::size_t regenerated = 0;    ///< Offset of the bracket's regeneration buffer.
  std::size_t scratch = 0;        ///< Offset of the executor slots' scratch.
  std::size_t scratch_bytes = 0;  ///< plan.scratch_bytes times the executor's threads.
  std::size_t bytes = 0;          ///< From the aligned base to the end of scratch.
  std::size_t mapping_bytes = 0;  ///< bytes plus one line of alignment slack.
};

/// The layout for `plan` on an executor of `threads` threads, with regions
/// aligned to `line_bytes`.
///
/// Errors:
///   kConfiguration  line_bytes is not a power of two of at least 8; or the
///                   plan breaks workload.hpp's rules for one (no unit, an
///                   input or output of zero bytes), which would make a slot
///                   of nothing and a division by zero of the slot rule.
///   kResources      the working set does not fit a size_t. No mapping of that
///                   size could be made, so none is asked for.
[[nodiscard]] core::Result<Layout, RunnerError> plan_layout(const workloads::Plan& plan,
                                                            std::size_t threads,
                                                            std::uint32_t line_bytes);

}  // namespace loadforge::verification::integrity

#endif
