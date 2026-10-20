// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ORACLE_SORT_SORT_ORACLE_HPP
#define LOADFORGE_VERIFICATION_ORACLE_SORT_SORT_ORACLE_HPP

// The independent oracle for the sort workload, written from
// src/workloads/sort/spec.hpp and from nothing else: a permutation check and
// an order check over the output's (key, input position) pairs. It never sorts
// anything (verification.md §3). The three steps, their order and the wording
// of every verdict are the specification's; the comments in sort_oracle.cpp
// say where each is.
//
// The permutation check needs one bit per input position. That scratch comes
// from a ScratchAllocator, so a test can make it fail: the check that cannot
// obtain its scratch reports NotChecked, never an Agreement
// (verification.md §5).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle {

/// Gives `words` zero-initialised 64-bit words, or nothing when it cannot.
using ScratchAllocator = std::function<std::unique_ptr<std::uint64_t[]>(std::size_t words)>;

/// The allocator make_sort_oracle(config, plan) uses: operator new, which
/// reports failure as nothing rather than by throwing.
[[nodiscard]] std::unique_ptr<std::uint64_t[]> allocate_scratch(std::size_t words);

/// The sort oracle for units of `plan`, or kConfiguration when the
/// configuration's decomposition is neither 0 nor a chunk count, or the plan's
/// is not a chunk count or its sizes are not a sort unit's.
[[nodiscard]] core::Result<std::unique_ptr<Oracle>, OracleError> make_sort_oracle(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan);

/// The same, taking scratch from `allocator`.
[[nodiscard]] core::Result<std::unique_ptr<Oracle>, OracleError> make_sort_oracle(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan,
    ScratchAllocator allocator);

}  // namespace loadforge::verification::oracle

#endif
