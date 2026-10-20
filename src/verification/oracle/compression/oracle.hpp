// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ORACLE_COMPRESSION_ORACLE_HPP
#define LOADFORGE_VERIFICATION_ORACLE_COMPRESSION_ORACLE_HPP

// The compression workload's independent oracle (verification.md §3): the
// unit's output, as workloads/compression/spec.hpp lays it out, judged by
// decoding its block with the strict decoder beside this file and comparing
// the result with the input the unit was given (oracle.hpp).
//
// WHAT IT ANSWERS, IN ORDER
// -------------------------
//   1. evidence that is not kInputBytes in and output_bytes_for(kInputBytes)
//      out: NotChecked, nothing compared.
//   2. the magic, then the block length L against compress_bound(kInputBytes),
//      then the zero padding after the block, then the block itself, rule by
//      rule (decoder.hpp): the first breach is an ExactMismatch with no
//      difference, its check naming the rule and the byte of the output.
//   3. no scratch to decode into: NotChecked.
//   4. the decoded bytes against the input: a difference is an ExactMismatch
//      with a Difference in Domain::kDerived, offsets from the first decoded
//      byte; none is an Agreement. One block per unit, so blocks_compared is 1
//      in every verdict that compared anything.
//
// The header and the padding are judged before scratch is asked for, so an
// output wrong there is reported as wrong even when no scratch could be had.
// The block's own rules are judged as it decodes, which needs the scratch.

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "workloads/compression/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::compression {

/// The name of the workload this oracle checks.
inline constexpr std::string_view kWorkload = "compression";

/// The bytes one check decodes a block into: one unit's input, decoded.
using Scratch = std::array<std::byte, workloads::compression::kInputBytes>;

/// Where a check gets its scratch: a fresh one, owned by the caller, or null
/// when none could be had. Called once per check, from whichever thread runs
/// the check, so a source must be safe to call concurrently. The seam exists
/// so that the failure can be injected.
using ScratchSource = std::unique_ptr<Scratch> (*)() noexcept;

/// The default source: the heap, without throwing.
[[nodiscard]] std::unique_ptr<Scratch> heap_scratch() noexcept;

class CompressionOracle final : public Oracle {
 public:
  // Defined out of line, every one: an inline member used from several
  // translation units leaves one copy's counters at zero (journal §1.22).
  explicit CompressionOracle(ScratchSource scratch = heap_scratch) noexcept;

  [[nodiscard]] std::string_view workload() const noexcept override;
  [[nodiscard]] Contract contract() const noexcept override;
  [[nodiscard]] Verdict check(const UnitEvidence& evidence) const override;

 private:
  ScratchSource scratch_;
};

/// The oracle for units of `plan`, the plan the compression workload resolved
/// from `config`; kConfiguration when the two do not describe a compression
/// unit: input or output sizes other than the specification's, a resolved
/// decomposition other than 1, or a configured one other than 0 (the
/// default) or 1. The registry's create() for "compression" calls this.
[[nodiscard]] core::Result<std::unique_ptr<Oracle>, OracleError> create(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan);

}  // namespace loadforge::verification::oracle::compression

#endif
