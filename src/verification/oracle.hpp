// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ORACLE_HPP
#define LOADFORGE_VERIFICATION_ORACLE_HPP

// CONTRACT (ADR 0004). The independent oracle of docs/verification.md §3: the
// second implementation of a workload, derived from the specification and
// never from the stress implementation. Frozen: changed only by a contract
// change, never inside a stream.
//
//   Providers of Oracle: the oracle streams, one oracle per workload, in
//     src/verification/oracle/<workload>/ -- a different stream from the one
//     that writes the workload, so that two readings of the specification
//     meet here and nowhere else.
//   Provider of OracleRegistry: the integration stream.
//   Consumer: the verified runner (runner.hpp), which turns a verdict into an
//     error record with provenance.
//
// AN ORACLE JUDGES THE OUTPUT AGAINST THE INPUT IT IS GIVEN
// --------------------------------------------------------
// Whether the input itself is right is the input-integrity bracket's
// question (verification.md §4a), answered by the runner. An oracle that
// regenerated the input and judged against that would hide which side was
// wrong; one that judged against what it was given keeps the two findings
// apart, which rule 4 of §4a requires.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include "core/result.hpp"
#include "verification/record.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification {

/// One unit, as the oracle sees it: which unit, the input the workload read,
/// and the output it wrote.
struct UnitEvidence {
  std::uint64_t index = 0;
  std::span<const std::byte> input;
  std::span<const std::byte> output;
};

/// The output is right. `blocks_compared` is at least 1: an oracle that
/// compared nothing has not agreed with anything (verification.md §5).
struct Agreement {
  std::uint64_t blocks_compared = 1;
};

/// An exact oracle found the output wrong. `check` and `difference` mean what
/// they mean in ExactBitCorruption; the difference's offsets are from the
/// start of the output for Domain::kStored, and from the start of the derived
/// bytes for Domain::kDerived. blocks_compared is at least 1.
struct ExactMismatch {
  std::string check;
  std::optional<Difference> difference;
  std::uint64_t blocks_compared = 1;
};

/// A bounded oracle found the output outside its derived tolerance. The
/// fields mean what they mean in NumericalVerificationFailure; `residual` is
/// greater than `tolerance` or NaN, and `tolerance` is finite and positive.
struct BoundedMismatch {
  std::string check;
  double residual = 0.0;
  double tolerance = 0.0;
  std::optional<double> condition_number;
  std::optional<std::uint64_t> element;
  std::uint64_t blocks_compared = 1;
};

/// The oracle could not make the check: evidence of the wrong size, a scratch
/// allocation that failed, a tolerance that could not be derived (a non-finite
/// value in the input), a sample that came out empty. `what` and `why` are
/// never empty. Never an Agreement in disguise.
struct NotChecked {
  std::string what;
  std::string why;
  std::uint64_t blocks_compared = 0;
};

using Verdict = std::variant<Agreement, ExactMismatch, BoundedMismatch, NotChecked>;

namespace detail {

// One overload per alternative, so is_valid() and blocks_of() can visit the verdict: a
// get_if dereferenced in a last arm, or a std::get, fails the static release build (GCC
// at -O3 reports a possible null dereference once inlined into a consumer) or
// bugprone-exception-escape; a visit does neither (journal fragment 02, question 9).
[[nodiscard]] inline bool keeps_rules(const Agreement& agreement, Contract /*contract*/) noexcept {
  return agreement.blocks_compared >= 1;
}

[[nodiscard]] inline bool keeps_rules(const ExactMismatch& exact, Contract contract) noexcept {
  return contract == Contract::kExact && exact.blocks_compared >= 1 && !exact.check.empty() &&
         exact.check.size() <= kMaxTextBytes &&
         (!exact.difference.has_value() || is_valid(*exact.difference));
}

[[nodiscard]] inline bool keeps_rules(const BoundedMismatch& bounded, Contract contract) noexcept {
  return contract == Contract::kBounded && bounded.blocks_compared >= 1 && !bounded.check.empty() &&
         bounded.check.size() <= kMaxTextBytes && std::isfinite(bounded.tolerance) &&
         bounded.tolerance > 0.0 && !(bounded.residual <= bounded.tolerance);
}

[[nodiscard]] inline bool keeps_rules(const NotChecked& unchecked, Contract /*contract*/) noexcept {
  return !unchecked.what.empty() && !unchecked.why.empty() &&
         unchecked.what.size() <= kMaxTextBytes && unchecked.why.size() <= kMaxTextBytes;
}

}  // namespace detail

/// Whether a verdict keeps the rules above, for an oracle of `contract`.
// visit throws only for a valueless variant, which no Verdict is (its alternatives are
// nothrow-movable) and which a caller cannot make.
// NOLINTNEXTLINE(bugprone-exception-escape)
[[nodiscard]] inline bool is_valid(const Verdict& verdict, Contract contract) noexcept {
  return std::visit(
      [contract](const auto& held) noexcept { return detail::keeps_rules(held, contract); },
      verdict);
}

/// The blocks a verdict says were compared.
// visit throws only for a valueless variant, which no Verdict is (its alternatives are
// nothrow-movable) and which a caller cannot make.
// NOLINTNEXTLINE(bugprone-exception-escape)
[[nodiscard]] inline std::uint64_t blocks_of(const Verdict& verdict) noexcept {
  return std::visit([](const auto& held) noexcept { return held.blocks_compared; }, verdict);
}

/// The independent check for one workload.
///
/// check(evidence) judges evidence.output against the specification of the
/// workload named by workload(), applied to evidence.input:
///
///   * Agreement when every compared block is right;
///   * ExactMismatch (exact oracles) or BoundedMismatch (bounded oracles) when
///     one is not -- never the other contract's kind;
///   * NotChecked when the check could not be made, including evidence whose
///     input or output is not the size the oracle's plan says.
///
/// It may compare a sample of a unit's blocks rather than all of them; which
/// blocks is a function of evidence.index alone, so a re-run checks the same
/// ones. The verdict is a pure function of the evidence and the oracle's
/// configuration, and is_valid(verdict, contract()) holds for every one.
///
/// Thread-safety: check() is const and may run concurrently for any units.
/// It reads the evidence and writes nothing outside itself.
class Oracle {
 public:
  Oracle() = default;
  Oracle(const Oracle&) = delete;
  Oracle(Oracle&&) = delete;
  Oracle& operator=(const Oracle&) = delete;
  Oracle& operator=(Oracle&&) = delete;
  virtual ~Oracle() = default;

  [[nodiscard]] virtual std::string_view workload() const noexcept = 0;
  [[nodiscard]] virtual Contract contract() const noexcept = 0;
  [[nodiscard]] virtual Verdict check(const UnitEvidence& evidence) const = 0;
};

/// Why an oracle could not be created.
///
///   kUnknownWorkload  no oracle for a workload of that name.
///   kConfiguration    the configuration or plan is outside what the
///                     specification defines: sizes that do not describe a
///                     unit of this workload, a decomposition it does not have.
enum class OracleFault : std::uint8_t { kUnknownWorkload, kConfiguration };

[[nodiscard]] constexpr std::string_view describe(OracleFault fault) noexcept {
  switch (fault) {
    case OracleFault::kUnknownWorkload:
      return "no oracle for that workload";
    case OracleFault::kConfiguration:
      return "configuration outside the specification";
  }
  return "unrecognised oracle fault";
}

// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct OracleError {
  OracleFault fault = OracleFault::kConfiguration;
  std::string detail;
};

[[nodiscard]] inline bool operator==(const OracleError& lhs, const OracleError& rhs) noexcept {
  return std::tie(lhs.fault, lhs.detail) == std::tie(rhs.fault, rhs.detail);
}

/// Every oracle a build contains, by the name of the workload it checks.
///
/// names() is sorted and without duplicates. create(name, config, plan) gives
/// an oracle whose workload() is `name`, for units of the given plan -- the
/// plan the workload resolved from the same configuration -- or the oracle's
/// own refusal, or kUnknownWorkload for a name not in names().
///
/// Thread-safety: both calls may be made from any thread.
class OracleRegistry {
 public:
  OracleRegistry() = default;
  OracleRegistry(const OracleRegistry&) = delete;
  OracleRegistry(OracleRegistry&&) = delete;
  OracleRegistry& operator=(const OracleRegistry&) = delete;
  OracleRegistry& operator=(OracleRegistry&&) = delete;
  virtual ~OracleRegistry() = default;

  [[nodiscard]] virtual std::vector<std::string> names() const = 0;
  [[nodiscard]] virtual core::Result<std::unique_ptr<Oracle>, OracleError> create(
      std::string_view name, const workloads::WorkloadConfig& config,
      const workloads::Plan& plan) const = 0;
};

}  // namespace loadforge::verification

#endif
