// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/integrity/layout.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>

namespace loadforge::verification::integrity {
namespace {

/// A size that remembers whether any step of its arithmetic overflowed.
///
/// Every operation folds the overflow flags of its operands and of its own
/// step together with a bitwise or rather than returning early, so the chain
/// of sums below has one decision -- at the end -- instead of one per step,
/// each of which would need a plan overflowing at exactly that step.
struct Checked {
  std::size_t value = 0;
  bool overflowed = false;
};

Checked add(Checked lhs, Checked rhs) noexcept {
  Checked out;
  const bool step = __builtin_add_overflow(lhs.value, rhs.value, &out.value);
  out.overflowed = (static_cast<unsigned>(step) | static_cast<unsigned>(lhs.overflowed) |
                    static_cast<unsigned>(rhs.overflowed)) != 0U;
  return out;
}

Checked multiply(Checked lhs, Checked rhs) noexcept {
  Checked out;
  const bool step = __builtin_mul_overflow(lhs.value, rhs.value, &out.value);
  out.overflowed = (static_cast<unsigned>(step) | static_cast<unsigned>(lhs.overflowed) |
                    static_cast<unsigned>(rhs.overflowed)) != 0U;
  return out;
}

/// Rounds up to a multiple of `line`, a power of two.
Checked round_up(Checked size, std::size_t line) noexcept {
  Checked out = add(size, Checked{line - 1});
  out.value &= ~(line - 1);
  return out;
}

}  // namespace

// The thread count and the line size are both sizes, each named where it is
// declared; a struct for two arguments of a pure function would be one more
// type.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
core::Result<Layout, RunnerError> plan_layout(const workloads::Plan& plan, std::size_t threads,
                                              std::uint32_t line_bytes) {
  if (line_bytes < 8 || !std::has_single_bit(line_bytes)) {
    return RunnerError{
        RunnerFault::kConfiguration,
        "line size " + std::to_string(line_bytes) + " is not a power of two of at least 8 bytes"};
  }
  if (plan.units == 0 || plan.input_bytes == 0 || plan.output_bytes == 0) {
    return RunnerError{RunnerFault::kConfiguration,
                       "the workload's plan has no unit to hold: " + std::to_string(plan.units) +
                           " units of " + std::to_string(plan.input_bytes) + " bytes in and " +
                           std::to_string(plan.output_bytes) + " out"};
  }

  Layout layout;
  layout.line_bytes = line_bytes;
  layout.input_bytes = plan.input_bytes;
  layout.output_bytes = plan.output_bytes;
  layout.units = plan.units;

  const Checked units{plan.units};
  const Checked input_stride = round_up(Checked{plan.input_bytes}, line_bytes);
  const Checked output_stride = round_up(Checked{plan.output_bytes}, line_bytes);
  const Checked scratch_bytes = multiply(Checked{plan.scratch_bytes}, Checked{threads});
  const Checked outputs = multiply(input_stride, units);
  const Checked regenerated = add(outputs, multiply(output_stride, units));
  const Checked scratch = add(regenerated, input_stride);
  const Checked bytes = add(scratch, round_up(scratch_bytes, line_bytes));
  const Checked mapping = add(bytes, Checked{line_bytes});

  if (mapping.overflowed) {
    return RunnerError{RunnerFault::kResources,
                       "working set: " + std::to_string(plan.units) + " units of " +
                           std::to_string(plan.input_bytes) + " + " +
                           std::to_string(plan.output_bytes) + " bytes, with " +
                           std::to_string(plan.scratch_bytes) + " bytes of scratch for each of " +
                           std::to_string(threads) + " threads, does not fit in the address space"};
  }

  layout.input_stride = input_stride.value;
  layout.output_stride = output_stride.value;
  layout.outputs = outputs.value;
  layout.regenerated = regenerated.value;
  layout.scratch = scratch.value;
  layout.scratch_bytes = scratch_bytes.value;
  layout.bytes = bytes.value;
  layout.mapping_bytes = mapping.value;
  return layout;
}

}  // namespace loadforge::verification::integrity
