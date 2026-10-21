// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_COMPRESSION_COMPRESSION_HPP
#define LOADFORGE_WORKLOADS_COMPRESSION_COMPRESSION_HPP

// The compression workload (PLAN F14): the first workload, and the pattern
// every later one copies -- a factory that resolves a configuration to a plan
// or refuses it, a generator over Philox, and a compute() that is one kernel
// call on the unit's own scratch.
//
//   generate(index)  generate_input(seed, index): generator.hpp.
//   compute(index)   one LZ4 block of the input, in the output layout
//                    spec.hpp fixes: the magic, the block's length, the
//                    block, and zeros to the end. lz4_block.hpp.
//
// THE PLAN
// --------
//   input_bytes    kInputBytes (64 KiB)
//   output_bytes   output_bytes_for(kInputBytes): the header and the bound
//   scratch_bytes  kTableBytes per executor slot: the match finder's table
//   units          footprint / (input_bytes + output_bytes)
//   decomposition  1; the workload has none, and refuses any other
//   logical_bytes  input_bytes + output_bytes: the input read once and the
//                  output written once, which is the traffic the algorithm
//                  defines. The table's traffic depends on the data and is
//                  the implementation's, not the algorithm's.
//   operations     input_bytes: compression is rated in bytes consumed, the
//                  workload's own unit, as every LZ codec's rate is quoted.
//
// THE REFUSALS, in the order they are checked: an ISA path other than scalar
// (there is one kernel, and it is integer code a compiler vectorises as it
// sees fit, so no path changes its bits); a decomposition other than 0 or 1;
// a footprint that holds no unit.
//
// DETERMINISM: BitExact. One unit is compressed by one call of the kernel, on
// whichever executor thread runs index 0 of a one-task run, in that thread's
// slot of the scratch; the block is a function of the input alone.

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

#include "core/result.hpp"
#include "workloads/compression/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::compression {

/// One unit's output from its input, in spec.hpp's layout: kMagic and the
/// block's length, little-endian, the block, and every byte after it zero.
///
/// Preconditions: output.size() == output_bytes_for(input.size()),
/// table.size() >= kTableBytes, and input.size() < 2^32.
void write_unit(std::span<const std::byte> input, std::span<std::byte> output,
                std::span<std::byte> table) noexcept;

/// The workload for `config`, or why not (above).
[[nodiscard]] core::Result<std::unique_ptr<Workload>, WorkloadError> create(
    const WorkloadConfig& config);

}  // namespace loadforge::workloads::compression

#endif
