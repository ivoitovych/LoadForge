// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_RECORD_HPP
#define LOADFORGE_VERIFICATION_RECORD_HPP

// CONTRACT (ADR 0004). The error-record model of docs/verification.md §8, and
// its encoding on the record channel. Frozen: changed only by a contract
// change, never inside a stream.
//
//   Provider of encode() and decode(): the verification-core stream
//     (src/verification/record/).
//   Producers of records: the verified runner (verification/runner.hpp).
//   Consumers: the worker, which sends them; the controller, which receives
//     them; fault isolation, which takes the two wrong-result kinds; results
//     and every reporter, which tell the user.
//
// THREE KINDS, AND ONLY ONE HAS BITS TO COMPARE
// ---------------------------------------------
// ExactBitCorruption carries expected and actual bytes, because an exact
// comparison knows the expected bits. NumericalVerificationFailure has no bit
// fields AT ALL -- not null ones, none -- because a bounded check has no
// expected bit pattern, and a type that cannot hold a bit classification
// cannot print a fabricated one. VerificationNotPerformed is the check that
// could not be made: a failure of the run, never a pass, and never routed
// into fault isolation (isolation.hpp takes only the first two, by type).
//
// EVERY PROVENANCE FIELD IS NULLABLE, AND ABSENCE IS REPORTED AS ABSENCE
// ----------------------------------------------------------------------
// A check that failed before a buffer existed has no buffer address. Zero
// would be a fact, and a false one. So every field below is optional, and a
// producer leaves unset what it does not know.
//
// TWO STAGES OF ONE RECORD
// ------------------------
// verification.md §8 lists fourteen common fields. Eleven are known where the
// record is made, in the worker, and live in Provenance. Three are not: the
// isolation result and its confidence exist only after the controller has run
// the re-run ladder, and the telemetry at the time of the error is the
// controller's timeline. Those are attached by the controller, beside the
// record, in results::Finding (controller/results/result.hpp). Putting them
// here would put fields on the wire that the sender can only ever leave empty.
//
// EQUALITY: every value type compares with a noexcept operator== over
// std::tie, never `= default`. A defaulted comparison used from two
// translation units reads as uncovered (journal §1.22), and one that can throw
// carries a cleanup block no test can enter (journal §1.13); either would be a
// coverage failure in a header no stream may edit.
//
// DETERMINISM: everything here is a value; encode() is a pure function of the
// record, so equal records encode to equal bytes on every architecture.
// THREAD-SAFETY: values, and pure functions.

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include "core/result.hpp"
#include "primitives/simd/dispatch.hpp"

namespace loadforge::verification {

/// The two verification contracts (verification.md §5). Encoded as these
/// numbers wherever a contract is stored.
enum class Contract : std::uint8_t { kExact = 0, kBounded = 1 };

[[nodiscard]] constexpr std::string_view describe(Contract contract) noexcept {
  switch (contract) {
    case Contract::kExact:
      return "exact";
    case Contract::kBounded:
      return "bounded";
  }
  return "unrecognised contract";
}

/// Which side of a computation was found corrupt (verification.md §4a, rule 4).
/// They implicate different things, and the user is told which was seen.
enum class Side : std::uint8_t { kOutput = 0, kInput = 1 };

[[nodiscard]] constexpr std::string_view describe(Side side) noexcept {
  switch (side) {
    case Side::kOutput:
      return "output";
    case Side::kInput:
      return "input";
  }
  return "unrecognised side";
}

/// What the input-integrity bracket (verification.md §4a) found for the unit
/// a record is about.
///
///   kIntact                    both the opening and the closing check passed.
///   kCorruptBeforeConsumption  the opening check failed: the input was
///                              already wrong when the workload read it.
///   kCorruptDuringConsumption  the opening check passed and the closing one
///                              failed: the input went wrong WHILE the workload
///                              was reading it. A different finding, implicating
///                              different hardware.
///   kNotChecked                the bracket did not run for this unit -- the
///                              duty cycle skipped it. Stated, never assumed.
enum class InputIntegrity : std::uint8_t {
  kIntact = 0,
  kCorruptBeforeConsumption = 1,
  kCorruptDuringConsumption = 2,
  kNotChecked = 3,
};

[[nodiscard]] constexpr std::string_view describe(InputIntegrity integrity) noexcept {
  switch (integrity) {
    case InputIntegrity::kIntact:
      return "intact";
    case InputIntegrity::kCorruptBeforeConsumption:
      return "corrupt before consumption";
    case InputIntegrity::kCorruptDuringConsumption:
      return "corrupt during consumption";
    case InputIntegrity::kNotChecked:
      return "not checked";
  }
  return "unrecognised input integrity";
}

/// How often the expensive checks run (PLAN F17): one unit in every
/// `oracle_period` is checked by the independent oracle, one in every
/// `integrity_period` is bracketed by the input-integrity checks. 1 means
/// every unit; 0 means none. Which units, exactly, is the runner's rule
/// (runner.hpp). A field of every record and of every run, because two runs
/// that verified at different rates are not comparable.
struct DutyCycle {
  std::uint32_t oracle_period = 1;
  std::uint32_t integrity_period = 1;
};

[[nodiscard]] inline bool operator==(const DutyCycle& lhs, const DutyCycle& rhs) noexcept {
  return std::tie(lhs.oracle_period, lhs.integrity_period) ==
         std::tie(rhs.oracle_period, rhs.integrity_period);
}

/// The provenance a record carries from where it was made. Every field is
/// nullable; see the header comment.
///
///   logical_cpu, physical_core, numa_node  where the unit ran.
///   buffer_address   the address of the first byte of the compared bytes --
///                    the unit's input for Side::kInput, its output for
///                    Side::kOutput. The runner places every unit's input and
///                    output at an address aligned to the cache-line size, so
///                    an offset from here has the word and line boundaries the
///                    hardware has.
///   buffer_offset    the offset of those bytes within the mapping that holds
///                    them: which unit slot of the working set.
///   iteration        the unit's index (workloads/workload.hpp).
///   workload, workload_version   the workload's descriptor.
///   seed             the run's seed. With iteration, enough to regenerate the
///                    unit's input anywhere (primitives/rng/philox.hpp).
///   isa              the dispatch path the unit was computed on.
///   input_integrity  what the bracket found for this unit.
///   duty             the duty cycle in force when the unit was checked.
struct Provenance {
  std::optional<std::uint32_t> logical_cpu;
  std::optional<std::uint32_t> physical_core;
  std::optional<std::uint32_t> numa_node;
  std::optional<std::uint64_t> buffer_address;
  std::optional<std::uint64_t> buffer_offset;
  std::optional<std::uint64_t> iteration;
  std::optional<std::string> workload;
  std::optional<std::uint32_t> workload_version;
  std::optional<std::uint64_t> seed;
  std::optional<simd::IsaPath> isa;
  std::optional<InputIntegrity> input_integrity;
  std::optional<DutyCycle> duty;
};

[[nodiscard]] inline bool operator==(const Provenance& lhs, const Provenance& rhs) noexcept {
  return std::tie(lhs.logical_cpu, lhs.physical_core, lhs.numa_node, lhs.buffer_address,
                  lhs.buffer_offset, lhs.iteration, lhs.workload, lhs.workload_version, lhs.seed,
                  lhs.isa, lhs.input_integrity, lhs.duty) ==
         std::tie(rhs.logical_cpu, rhs.physical_core, rhs.numa_node, rhs.buffer_address,
                  rhs.buffer_offset, rhs.iteration, rhs.workload, rhs.workload_version, rhs.seed,
                  rhs.isa, rhs.input_integrity, rhs.duty);
}

/// How the differing bits of an exact mismatch are spread, which implicates
/// different hardware (verification.md §8):
///
///   kSingleBit         exactly one bit differs in the whole comparison.
///   kMultiBitInWord    more than one, all within one naturally aligned
///                      8-byte word.
///   kWithinCacheLine   more than one word, all within one aligned cache line.
///                      IMPLEMENTATION M2a's "whole-cache-line" class.
///   kAcrossCacheLines  more than one cache line.
enum class BitClass : std::uint8_t {
  kSingleBit = 0,
  kMultiBitInWord = 1,
  kWithinCacheLine = 2,
  kAcrossCacheLines = 3,
};

[[nodiscard]] constexpr std::string_view describe(BitClass bit_class) noexcept {
  switch (bit_class) {
    case BitClass::kSingleBit:
      return "single bit";
    case BitClass::kMultiBitInWord:
      return "multiple bits in one word";
    case BitClass::kWithinCacheLine:
      return "within one cache line";
    case BitClass::kAcrossCacheLines:
      return "across cache lines";
  }
  return "unrecognised bit classification";
}

inline constexpr std::uint64_t kWordBytes = 8;
/// The most bytes of expected and actual a record carries.
inline constexpr std::size_t kMaxWindowBytes = 64;
/// The longest text field (workload name, what, why, check) a record carries.
inline constexpr std::size_t kMaxTextBytes = 1024;

/// The classification, from the four facts it depends on. Offsets are of the
/// first and last differing byte, from an origin aligned to `line_bytes`.
///
/// Preconditions: popcount >= 1, first_offset <= last_offset, line_bytes a
/// power of two and at least kWordBytes. A record that breaks them does not
/// decode (decode() refuses it as kInvalid), so no consumer meets one.
// The two offsets are an ordered pair, first then last, named in that order;
// a struct for them would be one more type for a four-argument pure function.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] constexpr BitClass classify_bits(std::uint64_t popcount, std::uint64_t first_offset,
                                               std::uint64_t last_offset,
                                               std::uint64_t line_bytes) noexcept {
  if (popcount == 1) {
    return BitClass::kSingleBit;
  }
  if (first_offset / kWordBytes == last_offset / kWordBytes) {
    return BitClass::kMultiBitInWord;
  }
  if (first_offset / line_bytes == last_offset / line_bytes) {
    return BitClass::kWithinCacheLine;
  }
  return BitClass::kAcrossCacheLines;
}

/// What the bytes of a Difference are.
///
///   kStored   expected and actual are the bytes as STORED in the buffer the
///             provenance names: the input against its regeneration, or an
///             output against the specification's output. The bit
///             classification then describes bits the hardware held.
///   kDerived  actual was DERIVED from the stored buffer by the oracle -- the
///             compressed output decoded, say -- and compared with what the
///             specification says it must be. The classification describes
///             the difference in the derived data, and a report must say so:
///             one flipped bit in a compressed stream can change a thousand
///             decoded ones, and presenting that as a stored bit pattern would
///             be inventing evidence.
enum class Domain : std::uint8_t { kStored = 0, kDerived = 1 };

[[nodiscard]] constexpr std::string_view describe(Domain domain) noexcept {
  switch (domain) {
    case Domain::kStored:
      return "stored bytes";
    case Domain::kDerived:
      return "derived bytes";
  }
  return "unrecognised domain";
}

/// The bytes an exact comparison found different.
///
///   first_offset, last_offset   the first and last differing byte, from the
///                 start of the compared bytes. For kStored that start is
///                 provenance.buffer_address, which is line-aligned (see
///                 Provenance), so the offsets have the hardware's word and
///                 line boundaries.
///   popcount      differing bits over the WHOLE comparison, not the window.
///   window_offset first_offset rounded down to a multiple of kWordBytes.
///   expected, actual   the bytes from window_offset on, the same length:
///                 min(kMaxWindowBytes, compared length - window_offset).
///
/// What a record can be checked against, from its own bytes (contract change,
/// external review 2026-10-20 12:51 UTC, W1-05). The window is an excerpt, and
/// a valid difference is consistent with it:
///   * the window covers first_offset, and expected and actual DIFFER at that
///     byte -- the record's own claim of where the first difference is;
///   * when last_offset lies inside the window, they differ at that byte too.
/// What an excerpt cannot show, and so is NOT checked: that no byte before
/// first_offset differs; that last_offset is the last differing byte when it
/// lies beyond the window; that popcount is the number of differing bits over
/// the whole comparison (the excerpt gives at most a lower bound, which is not
/// enforced); that the bytes outside the window agree; that the compared length
/// is what the check says. Those claims rest on the producer, and on the
/// record channel's checksum against corruption in transit -- a checksum is
/// not a proof the producer was right.
///
/// The XOR delta and the classification are derived (below), never stored: a
/// record cannot hold a delta that disagrees with its own bytes.
struct Difference {
  Domain domain = Domain::kStored;
  std::uint64_t first_offset = 0;
  std::uint64_t last_offset = 0;
  std::uint64_t popcount = 0;
  std::uint64_t window_offset = 0;
  std::vector<std::byte> expected;
  std::vector<std::byte> actual;
};

[[nodiscard]] inline bool operator==(const Difference& lhs, const Difference& rhs) noexcept {
  return std::tie(lhs.domain, lhs.first_offset, lhs.last_offset, lhs.popcount, lhs.window_offset,
                  lhs.expected, lhs.actual) ==
         std::tie(rhs.domain, rhs.first_offset, rhs.last_offset, rhs.popcount, rhs.window_offset,
                  rhs.expected, rhs.actual);
}

/// An exact comparison failed (verification.md §8).
///
///   side        which of the unit's buffers was wrong.
///   check       what was compared, for the person reading the report: "input
///               against its regeneration", "decoded output against input",
///               "compressed stream malformed at byte 517".
///   difference  the differing bytes, when the comparison could be made byte
///               for byte. ABSENT when the output could not even be turned
///               into comparable bytes -- a compressed stream that does not
///               decode is a wrong result, and has no bits to classify; the
///               check says what was found instead. Absence is reported as
///               absence, never as a zero popcount.
///   line_bytes  the cache-line size a classification uses: the topology's,
///               never an assumed 64 (PLAN §4.5).
struct ExactBitCorruption {
  Provenance provenance;
  Side side = Side::kOutput;
  std::string check;
  std::optional<Difference> difference;
  std::uint32_t line_bytes = 64;
};

[[nodiscard]] inline bool operator==(const ExactBitCorruption& lhs,
                                     const ExactBitCorruption& rhs) noexcept {
  return std::tie(lhs.provenance, lhs.side, lhs.check, lhs.difference, lhs.line_bytes) ==
         std::tie(rhs.provenance, rhs.side, rhs.check, rhs.difference, rhs.line_bytes);
}

/// The record's bit classification, or nothing when it has no difference.
[[nodiscard]] constexpr std::optional<BitClass> classify(
    const ExactBitCorruption& record) noexcept {
  if (!record.difference.has_value()) {
    return std::nullopt;
  }
  const Difference& difference = *record.difference;
  return classify_bits(difference.popcount, difference.first_offset, difference.last_offset,
                       record.line_bytes);
}

/// expected XOR actual over the window. Empty if the two lengths differ, which
/// no valid difference has.
[[nodiscard]] inline std::vector<std::byte> xor_delta(const Difference& difference) {
  std::vector<std::byte> delta;
  if (difference.expected.size() != difference.actual.size()) {
    return delta;
  }
  delta.reserve(difference.expected.size());
  for (std::size_t index = 0; index < difference.expected.size(); ++index) {
    delta.push_back(difference.expected[index] ^ difference.actual[index]);
  }
  return delta;
}

/// Whether `offset` (from the start of the compared bytes) lies inside the
/// difference's window, judged by the expected bytes.
[[nodiscard]] inline bool in_window(const Difference& difference, std::uint64_t offset) noexcept {
  return offset >= difference.window_offset &&
         offset - difference.window_offset < difference.expected.size();
}

/// Whether the window holds `offset` and the two windows differ at it. False
/// when `offset` is outside the window. Precondition: expected and actual are
/// the same length (is_valid and broken_rule test that first).
[[nodiscard]] inline bool differs_at(const Difference& difference, std::uint64_t offset) noexcept {
  return in_window(difference, offset) && difference.expected[offset - difference.window_offset] !=
                                              difference.actual[offset - difference.window_offset];
}

/// Whether a difference keeps the rules above. Line size is the record's.
[[nodiscard]] inline bool is_valid(const Difference& difference) noexcept {
  return difference.domain <= Domain::kDerived && difference.popcount >= 1 &&
         difference.first_offset <= difference.last_offset &&
         difference.window_offset == difference.first_offset / kWordBytes * kWordBytes &&
         difference.expected.size() == difference.actual.size() && !difference.expected.empty() &&
         difference.expected.size() <= kMaxWindowBytes &&
         differs_at(difference, difference.first_offset) &&
         (!in_window(difference, difference.last_offset) ||
          differs_at(difference, difference.last_offset));
}

/// A bounded check exceeded its derived tolerance (verification.md §5, §8).
///
///   check            which invariant or norm failed: "residual 2-norm",
///                    "componentwise |C - AB|", "momentum".
///   residual         what was measured. NaN or +infinity when the corruption
///                    produced one; that is a failure, not a pass.
///   tolerance        the derived bound: finite and strictly positive. A
///                    tolerance that cannot be derived is not this record --
///                    it is VerificationNotPerformed.
///
/// A record is a FAILURE only if the residual exceeds the tolerance
/// (exceeds_tolerance below): residual > tolerance, or residual NaN. A residual
/// at or below the tolerance is a check that PASSED and is not this record. A
/// negative residual (-infinity included) is no residual at all, since a norm
/// or an absolute difference is never negative, and is not a failure either
/// (external review 2026-10-20 12:51 UTC, W1-01).
/// Signed zero: -0.0 equals 0.0, is below any positive tolerance, and so is
/// not a failure. NaN: any NaN is a failure whatever its sign bit or payload --
/// the sign of a NaN carries no meaning, and the codec keeps the bits as they
/// were.
///   condition_number where meaningful.
///   element          which output element failed, where one did.
///
/// No bit fields exist on this type, by design (header comment).
struct NumericalVerificationFailure {
  Provenance provenance;
  std::string check;
  double residual = 0.0;
  double tolerance = 0.0;
  std::optional<double> condition_number;
  std::optional<std::uint64_t> element;
};

[[nodiscard]] inline bool operator==(const NumericalVerificationFailure& lhs,
                                     const NumericalVerificationFailure& rhs) noexcept {
  return std::tie(lhs.provenance, lhs.check, lhs.residual, lhs.tolerance, lhs.condition_number,
                  lhs.element) == std::tie(rhs.provenance, rhs.check, rhs.residual, rhs.tolerance,
                                           rhs.condition_number, rhs.element);
}

/// Whether a measured residual is a failure against its tolerance: NaN, or
/// strictly greater than the tolerance (+infinity included). False for a
/// residual at or below the tolerance, and for every negative one.
[[nodiscard]] inline bool exceeds_tolerance(double residual, double tolerance) noexcept {
  return std::isnan(residual) || residual > tolerance;
}

/// residual / tolerance: > 1 for every decodable record, or NaN.
[[nodiscard]] constexpr double ratio(const NumericalVerificationFailure& record) noexcept {
  return record.residual / record.tolerance;
}

/// The check could not be made (verification.md §5, §8): the oracle could not
/// be allocated, the reference was unavailable, the tolerance could not be
/// derived, or the pass compared nothing. A failure of the run.
///
///   what             what was to be checked: "oracle over compression units".
///   why              why it could not be: "no unit was sampled".
///   blocks_compared  how many were compared before it stopped; may be 0.
struct VerificationNotPerformed {
  Provenance provenance;
  std::string what;
  std::string why;
  std::uint64_t blocks_compared = 0;
};

[[nodiscard]] inline bool operator==(const VerificationNotPerformed& lhs,
                                     const VerificationNotPerformed& rhs) noexcept {
  return std::tie(lhs.provenance, lhs.what, lhs.why, lhs.blocks_compared) ==
         std::tie(rhs.provenance, rhs.what, rhs.why, rhs.blocks_compared);
}

using ErrorRecord =
    std::variant<ExactBitCorruption, NumericalVerificationFailure, VerificationNotPerformed>;

/// The type's name as the documents and every report spell it.
[[nodiscard]] constexpr std::string_view kind_name(const ErrorRecord& record) noexcept {
  switch (record.index()) {
    case 0:
      return "ExactBitCorruption";
    case 1:
      return "NumericalVerificationFailure";
    default:
      return "VerificationNotPerformed";
  }
}

[[nodiscard]] constexpr const Provenance& provenance_of(const ErrorRecord& record) noexcept {
  if (const auto* exact = std::get_if<ExactBitCorruption>(&record)) {
    return exact->provenance;
  }
  if (const auto* numerical = std::get_if<NumericalVerificationFailure>(&record)) {
    return numerical->provenance;
  }
  return std::get_if<VerificationNotPerformed>(&record)->provenance;
}

/// Whether a record states a wrong result -- the two kinds fault isolation
/// takes -- as opposed to a check that was not made.
[[nodiscard]] constexpr bool is_wrong_result(const ErrorRecord& record) noexcept {
  return !std::holds_alternative<VerificationNotPerformed>(record);
}

// ---------------------------------------------------------------------------
// The record on the record channel (worker/ipc/record.hpp).
// ---------------------------------------------------------------------------

/// The ipc frame type a record travels as. The frame's checksum protects the
/// bytes; this encoding gives them meaning.
inline constexpr std::uint16_t kErrorRecordFrameType = 16;

/// The first two bytes of every encoding: the format version, then the kind
/// (1 ExactBitCorruption, 2 NumericalVerificationFailure, 3
/// VerificationNotPerformed). The layout after them is the provider's, because
/// both ends of the channel link the one provider -- as both link ipc's frames
/// -- and a format the contract pinned byte by byte would be one nobody could
/// fix without a contract change. Integers are little-endian.
inline constexpr std::uint8_t kRecordFormatVersion = 1;

enum class DecodeFault : std::uint8_t {
  kEmpty,      ///< no bytes at all.
  kVersion,    ///< a format version this build does not read.
  kKind,       ///< a kind byte that is none of the three.
  kTruncated,  ///< the bytes end inside the record.
  kTrailing,   ///< bytes left over after a complete record.
  kInvalid,    ///< well-formed bytes describing a record this contract forbids.
};

[[nodiscard]] constexpr std::string_view describe(DecodeFault fault) noexcept {
  switch (fault) {
    case DecodeFault::kEmpty:
      return "empty";
    case DecodeFault::kVersion:
      return "unknown format version";
    case DecodeFault::kKind:
      return "unknown record kind";
    case DecodeFault::kTruncated:
      return "truncated";
    case DecodeFault::kTrailing:
      return "trailing bytes";
    case DecodeFault::kInvalid:
      return "invalid record";
  }
  return "unrecognised decode fault";
}

// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct DecodeError {
  DecodeFault fault = DecodeFault::kEmpty;
  std::string detail;  ///< What was found, for the report of a corrupt channel.
};

[[nodiscard]] inline bool operator==(const DecodeError& lhs, const DecodeError& rhs) noexcept {
  return std::tie(lhs.fault, lhs.detail) == std::tie(rhs.fault, rhs.detail);
}

/// The record's bytes. Provided by the verification-core stream.
///
/// Total: any record encodes. Deterministic: equal records give equal bytes.
/// The first two bytes are kRecordFormatVersion and the kind.
[[nodiscard]] std::vector<std::byte> encode(const ErrorRecord& record);

/// The record the bytes describe, or why they describe none. Provided by the
/// verification-core stream.
///
/// decode(encode(r)) == r for every VALID r. A record is valid when:
///   * every enumeration it holds (side, isa, input_integrity) has one of its
///     named values;
///   * every text field (workload, check, what, why) is at most kMaxTextBytes;
///   * an ExactBitCorruption has a non-empty check, line_bytes a power of two
///     >= kWordBytes, and, when it has a difference, a valid one: a named
///     domain, popcount >= 1, first_offset <= last_offset, window_offset equal
///     to first_offset rounded down to kWordBytes, and expected and actual of
///     equal length from 1 to kMaxWindowBytes;
///   * a Difference also keeps its own bytes consistent: the window covers
///     first_offset, expected and actual differ at it, and differ at
///     last_offset when that lies inside the window (what an excerpt cannot
///     show is listed at Difference);
///   * a NumericalVerificationFailure has a finite tolerance > 0 and is a
///     failure: its residual is NaN (any sign or payload) or greater than the
///     tolerance, +infinity included. A residual at or below the tolerance, or
///     negative, is no failure and does not decode.
/// The bytes of an invalid record decode to kInvalid, never to the record.
/// A garbled byte stream decodes to an error, never to a different valid
/// record silently -- the frame's checksum is what makes that hold in
/// practice; this function's part is never to guess.
[[nodiscard]] core::Result<ErrorRecord, DecodeError> decode(std::span<const std::byte> bytes);

/// Whether a record is valid, by the rules decode() applies.
[[nodiscard]] inline bool is_valid(const ErrorRecord& record) noexcept {
  const Provenance& provenance = provenance_of(record);
  if (provenance.workload.has_value() && provenance.workload->size() > kMaxTextBytes) {
    return false;
  }
  if (provenance.isa.has_value() &&
      static_cast<std::size_t>(*provenance.isa) >= simd::kIsaPathCount) {
    return false;
  }
  if (provenance.input_integrity.has_value() &&
      *provenance.input_integrity > InputIntegrity::kNotChecked) {
    return false;
  }
  if (const auto* exact = std::get_if<ExactBitCorruption>(&record)) {
    return exact->side <= Side::kInput && !exact->check.empty() &&
           exact->check.size() <= kMaxTextBytes && exact->line_bytes >= kWordBytes &&
           std::has_single_bit(exact->line_bytes) &&
           (!exact->difference.has_value() || is_valid(*exact->difference));
  }
  if (const auto* numerical = std::get_if<NumericalVerificationFailure>(&record)) {
    return numerical->check.size() <= kMaxTextBytes && std::isfinite(numerical->tolerance) &&
           numerical->tolerance > 0.0 &&
           exceeds_tolerance(numerical->residual, numerical->tolerance);
  }
  const auto* not_performed = std::get_if<VerificationNotPerformed>(&record);
  return not_performed->what.size() <= kMaxTextBytes && not_performed->why.size() <= kMaxTextBytes;
}

}  // namespace loadforge::verification

#endif
