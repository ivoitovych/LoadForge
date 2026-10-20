// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/golden/golden.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "primitives/simd/dispatch.hpp"
#include "verification/checksum/crc64.hpp"
#include "verification/checksum/seal.hpp"
#include "worker/executor.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification {

namespace {

constexpr std::string_view kMagic = "loadforge-golden 1";
constexpr std::size_t kHexDigits = 16;

std::string hex16(std::uint64_t value) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string text(kHexDigits, '0');
  for (std::size_t digit = 0; digit < kHexDigits; ++digit) {
    text[kHexDigits - 1 - digit] = kDigits[(value >> (4U * digit)) & 0x0FU];
  }
  return "0x" + text;
}

// --------------------------------------------------------------------------
// Reading. Each step reads the next line that is not a comment and either
// takes its value or records why not, with the line's number.
// --------------------------------------------------------------------------

class Parser {
 public:
  explicit Parser(std::string_view text) noexcept : rest_(text) {}

  [[nodiscard]] const GoldenError& error() const noexcept { return error_; }

  /// The next line that is not empty and not a comment, or false at the end.
  bool next(std::string_view& line) {
    while (!rest_.empty()) {
      const std::size_t end = rest_.find('\n');
      line = rest_.substr(0, end);
      rest_ = end == std::string_view::npos ? std::string_view{} : rest_.substr(end + 1);
      ++number_;
      if (!line.empty() && line.front() != '#') {
        return true;
      }
    }
    return false;
  }

  /// The next line, which must be "<key> <value>"; the value is returned.
  bool keyed(std::string_view key, std::string_view& value) {
    std::string_view line;
    if (!next(line)) {
      return fail_at_end("'" + std::string(key) + "'");
    }
    if (!line.starts_with(key) || line.size() <= key.size() || line[key.size()] != ' ') {
      return fail("expected '" + std::string(key) + " ...', found '" + std::string(line) + "'");
    }
    value = line.substr(key.size() + 1);
    return true;
  }

  /// The next line, which must be exactly `expected`.
  bool exactly(std::string_view expected) {
    std::string_view line;
    if (!next(line)) {
      return fail_at_end("'" + std::string(expected) + "'");
    }
    if (line != expected) {
      return fail("expected '" + std::string(expected) + "', found '" + std::string(line) + "'");
    }
    return true;
  }

  /// A decimal without sign or leading zeros, at most `limit`.
  bool decimal(std::string_view what, std::string_view token, std::uint64_t limit,
               std::uint64_t& out) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    const bool canonical = !token.empty() && (token.size() == 1 || token.front() != '0');
    if (error != std::errc{} || end != token.data() + token.size() || !canonical || value > limit) {
      return fail(std::string(what) + " '" + std::string(token) + "' is not a decimal from 0 to " +
                  std::to_string(limit) + " without sign or leading zeros");
    }
    out = value;
    return true;
  }

  /// "0x" and exactly sixteen lower-case hexadecimal digits.
  bool hexadecimal(std::string_view what, std::string_view token, std::uint64_t& out) {
    const bool shaped = token.size() == 2 + kHexDigits && token.starts_with("0x") &&
                        std::ranges::all_of(token.substr(2), [](char c) {
                          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                        });
    if (!shaped) {
      return fail(std::string(what) + " '" + std::string(token) +
                  "' is not 0x and sixteen lower-case hexadecimal digits");
    }
    std::from_chars(token.data() + 2, token.data() + token.size(), out, 16);
    return true;
  }

  bool fail(std::string detail) {
    error_ = GoldenError{number_, std::move(detail)};
    return false;
  }

  /// The file ended before `wanted`.
  bool fail_at_end(const std::string& wanted) {
    return fail("the file ends where " + wanted + " was expected");
  }

 private:
  std::string_view rest_;
  std::uint64_t number_ = 0;
  GoldenError error_;
};

bool parse_key(Parser& parser, GoldenKey& key) {
  std::string_view value;
  std::uint64_t number = 0;
  if (!parser.exactly(kMagic) || !parser.keyed("workload", value)) {
    return false;
  }
  if (!workloads::is_valid_name(value)) {
    return parser.fail("workload '" + std::string(value) + "' is not a valid workload name");
  }
  key.workload = std::string(value);
  constexpr std::uint64_t kU32 = std::numeric_limits<std::uint32_t>::max();
  constexpr std::uint64_t kU64 = std::numeric_limits<std::uint64_t>::max();
  if (!parser.keyed("version", value) || !parser.decimal("version", value, kU32, number)) {
    return false;
  }
  if (number == 0) {
    return parser.fail("version 0: a workload's version is at least 1");
  }
  key.version = static_cast<std::uint32_t>(number);
  if (!parser.keyed("isa", value)) {
    return false;
  }
  const std::optional<simd::IsaPath> isa = simd::parse_isa_path(value);
  if (!isa.has_value()) {
    return parser.fail("isa '" + std::string(value) + "' is not an ISA path");
  }
  key.isa = *isa;
  if (!parser.keyed("seed", value) || !parser.hexadecimal("seed", value, key.seed) ||
      !parser.keyed("footprint_bytes", value) ||
      !parser.decimal("footprint_bytes", value, kU64, key.footprint_bytes) ||
      !parser.keyed("decomposition", value) ||
      !parser.decimal("decomposition", value, kU32, number)) {
    return false;
  }
  key.decomposition = static_cast<std::uint32_t>(number);
  if (!parser.keyed("resolved_decomposition", value) ||
      !parser.decimal("resolved_decomposition", value, kU32, number)) {
    return false;
  }
  if (number == 0) {
    return parser.fail("resolved_decomposition 0: a plan's decomposition is never 0");
  }
  key.resolved_decomposition = static_cast<std::uint32_t>(number);
  if (!parser.keyed("units", value) || !parser.decimal("units", value, kU64, key.units)) {
    return false;
  }
  if (key.units == 0) {
    return parser.fail("units 0: a vector of no unit would compare nothing");
  }
  return true;
}

bool parse_unit(Parser& parser, std::uint64_t index, UnitDigest& unit) {
  std::string_view value;
  if (!parser.keyed("unit", value)) {
    return false;
  }
  const std::string prefix = std::to_string(index) + " input ";
  if (!value.starts_with(prefix)) {
    return parser.fail("expected 'unit " + prefix + "...', found 'unit " + std::string(value) +
                       "'");
  }
  value.remove_prefix(prefix.size());
  constexpr std::string_view kOutput = " output ";
  const std::string_view input = value.substr(0, 2 + kHexDigits);
  if (!parser.hexadecimal("unit input digest", input, unit.input)) {
    return false;
  }
  value.remove_prefix(input.size());
  if (!value.starts_with(kOutput)) {
    return parser.fail("expected ' output ' after unit " + std::to_string(index) +
                       "'s input digest");
  }
  value.remove_prefix(kOutput.size());
  return parser.hexadecimal("unit output digest", value, unit.output);
}

// --------------------------------------------------------------------------
// Comparing.
// --------------------------------------------------------------------------

void differ(std::vector<GoldenChange>& changes, std::string what, std::string recorded,
            std::string now) {
  if (recorded == now) {
    return;
  }
  // Grown first and filled after: a temporary pushed in would leave a cleanup
  // block, for a push that throws, whose branches no test can enter
  // (journal §1.13).
  GoldenChange& change = changes.emplace_back();
  change.what = std::move(what);
  change.recorded = std::move(recorded);
  change.now = std::move(now);
}

std::vector<GoldenChange> key_changes(const GoldenKey& recorded, const GoldenKey& fresh) {
  std::vector<GoldenChange> changes;
  differ(changes, "workload", recorded.workload, fresh.workload);
  differ(changes, "isa", std::string(simd::describe(recorded.isa)),
         std::string(simd::describe(fresh.isa)));
  differ(changes, "seed", hex16(recorded.seed), hex16(fresh.seed));
  differ(changes, "footprint_bytes", std::to_string(recorded.footprint_bytes),
         std::to_string(fresh.footprint_bytes));
  differ(changes, "decomposition", std::to_string(recorded.decomposition),
         std::to_string(fresh.decomposition));
  differ(changes, "units", std::to_string(recorded.units), std::to_string(fresh.units));
  return changes;
}

}  // namespace

bool operator==(const GoldenKey& lhs, const GoldenKey& rhs) noexcept {
  return std::tie(lhs.workload, lhs.version, lhs.isa, lhs.seed, lhs.footprint_bytes,
                  lhs.decomposition, lhs.resolved_decomposition, lhs.units) ==
         std::tie(rhs.workload, rhs.version, rhs.isa, rhs.seed, rhs.footprint_bytes,
                  rhs.decomposition, rhs.resolved_decomposition, rhs.units);
}

bool operator==(const UnitDigest& lhs, const UnitDigest& rhs) noexcept {
  return std::tie(lhs.input, lhs.output) == std::tie(rhs.input, rhs.output);
}

bool operator==(const GoldenVector& lhs, const GoldenVector& rhs) noexcept {
  return std::tie(lhs.key, lhs.units, lhs.digest) == std::tie(rhs.key, rhs.units, rhs.digest);
}

bool operator==(const GoldenError& lhs, const GoldenError& rhs) noexcept {
  return std::tie(lhs.line, lhs.detail) == std::tie(rhs.line, rhs.detail);
}

bool operator==(const GoldenChange& lhs, const GoldenChange& rhs) noexcept {
  return std::tie(lhs.what, lhs.recorded, lhs.now) == std::tie(rhs.what, rhs.recorded, rhs.now);
}

bool operator==(const GoldenReport& lhs, const GoldenReport& rhs) noexcept {
  return std::tie(lhs.verdict, lhs.changes) == std::tie(rhs.verdict, rhs.changes);
}

workloads::WorkloadConfig config_of(const GoldenKey& key) noexcept {
  return workloads::WorkloadConfig{key.seed, key.footprint_bytes, key.decomposition, key.isa};
}

std::string describe(const GoldenError& error) {
  if (!error.line.has_value()) {
    return error.detail;
  }
  return "line " + std::to_string(*error.line) + ": " + error.detail;
}

core::Result<GoldenVector, GoldenError> record_golden(const workloads::Workload& workload,
                                                      const workloads::WorkloadConfig& config,
                                                      std::uint64_t units,
                                                      worker::Executor& executor) {
  if (units == 0) {
    return GoldenError{std::nullopt, "no unit to record: a vector of nothing compares nothing"};
  }
  const workloads::Descriptor& descriptor = workload.descriptor();
  const workloads::Plan& plan = workload.plan();
  GoldenVector vector;
  vector.key =
      GoldenKey{descriptor.name,        descriptor.version,   config.isa,         config.seed,
                config.footprint_bytes, config.decomposition, plan.decomposition, units};
  std::vector<std::byte> input(plan.input_bytes);
  std::vector<std::byte> output(plan.output_bytes);
  std::vector<std::byte> scratch(plan.scratch_bytes * executor.threads());
  RunningChecksum outputs;
  for (std::uint64_t index = 0; index < units; ++index) {
    workload.generate(index, input);
    workload.compute(index, input, output, scratch, executor);
    vector.units.push_back(UnitDigest{crc64(input), crc64(output)});
    outputs.update(output);
  }
  vector.digest = outputs.seal().crc;
  return vector;
}

std::string format_golden(const GoldenVector& vector, std::string_view note) {
  std::string text =
      "# LoadForge golden vector: a regression tripwire, not an oracle (docs/verification.md "
      "§4).\n"
      "# Generated by the code it checks. Regenerate only deliberately, and say why.\n";
  while (!note.empty()) {
    const std::size_t end = note.find('\n');
    text += "# ";
    text += note.substr(0, end);
    text += '\n';
    note = end == std::string_view::npos ? std::string_view{} : note.substr(end + 1);
  }
  const GoldenKey& key = vector.key;
  text += std::string(kMagic) + "\n";
  text += "workload " + key.workload + "\n";
  text += "version " + std::to_string(key.version) + "\n";
  text += "isa " + std::string(simd::describe(key.isa)) + "\n";
  text += "seed " + hex16(key.seed) + "\n";
  text += "footprint_bytes " + std::to_string(key.footprint_bytes) + "\n";
  text += "decomposition " + std::to_string(key.decomposition) + "\n";
  text += "resolved_decomposition " + std::to_string(key.resolved_decomposition) + "\n";
  text += "units " + std::to_string(key.units) + "\n";
  for (std::size_t index = 0; index < vector.units.size(); ++index) {
    text += "unit " + std::to_string(index) + " input " + hex16(vector.units[index].input) +
            " output " + hex16(vector.units[index].output) + "\n";
  }
  text += "digest " + hex16(vector.digest) + "\n";
  return text;
}

core::Result<GoldenVector, GoldenError> parse_golden(std::string_view text) {
  Parser parser(text);
  GoldenVector vector;
  if (!parse_key(parser, vector.key)) {
    return parser.error();
  }
  for (std::uint64_t index = 0; index < vector.key.units; ++index) {
    UnitDigest unit;
    if (!parse_unit(parser, index, unit)) {
      return parser.error();
    }
    vector.units.push_back(unit);
  }
  std::string_view value;
  if (!parser.keyed("digest", value) || !parser.hexadecimal("digest", value, vector.digest)) {
    return parser.error();
  }
  std::string_view extra;
  if (parser.next(extra)) {
    parser.fail("'" + std::string(extra) + "' after the digest, which ends the vector");
    return parser.error();
  }
  return vector;
}

std::string_view describe(GoldenVerdict verdict) noexcept {
  switch (verdict) {
    case GoldenVerdict::kUnchanged:
      return "unchanged";
    case GoldenVerdict::kDeclaredChange:
      return "changed, and the workload's version says so: the vector is stale";
    case GoldenVerdict::kChanged:
      return "changed under the same version: a regression report";
    case GoldenVerdict::kNotComparable:
      return "not comparable: the two vectors are of different runs";
    case GoldenVerdict::kNothingCompared:
      return "nothing compared: the recorded vector holds no unit, which is not a pass";
  }
  return "unrecognised golden verdict";
}

GoldenReport compare_golden(const GoldenVector& recorded, const GoldenVector& fresh) {
  GoldenReport report;
  if (recorded.key.units == 0 || recorded.units.empty()) {
    report.verdict = GoldenVerdict::kNothingCompared;
    return report;
  }
  report.changes = key_changes(recorded.key, fresh.key);
  if (!report.changes.empty()) {
    report.verdict = GoldenVerdict::kNotComparable;
    return report;
  }
  differ(report.changes, "version", std::to_string(recorded.key.version),
         std::to_string(fresh.key.version));
  const bool declared = !report.changes.empty();
  differ(report.changes, "resolved_decomposition",
         std::to_string(recorded.key.resolved_decomposition),
         std::to_string(fresh.key.resolved_decomposition));
  const std::size_t units = std::min(recorded.units.size(), fresh.units.size());
  for (std::size_t index = 0; index < units; ++index) {
    const std::string unit = "unit " + std::to_string(index);
    differ(report.changes, unit + " input", hex16(recorded.units[index].input),
           hex16(fresh.units[index].input));
    differ(report.changes, unit + " output", hex16(recorded.units[index].output),
           hex16(fresh.units[index].output));
  }
  differ(report.changes, "digest", hex16(recorded.digest), hex16(fresh.digest));
  if (declared) {
    report.verdict = GoldenVerdict::kDeclaredChange;
  } else if (!report.changes.empty()) {
    report.verdict = GoldenVerdict::kChanged;
  } else {
    report.verdict = GoldenVerdict::kUnchanged;
  }
  return report;
}

std::string render(const GoldenReport& report, const GoldenKey& recorded) {
  std::string text = "golden vector for " + recorded.workload + " version " +
                     std::to_string(recorded.version) + " on " +
                     std::string(simd::describe(recorded.isa)) + ", seed " + hex16(recorded.seed) +
                     ", " + std::to_string(recorded.units) + " unit(s): ";
  text += describe(report.verdict);
  text += '\n';
  for (const GoldenChange& change : report.changes) {
    text += "  " + change.what + ": recorded " + change.recorded + ", now " + change.now + "\n";
  }
  if (report.verdict != GoldenVerdict::kUnchanged) {
    text +=
        "  A golden vector is a regression tripwire, not an oracle: this says what changed "
        "since it was recorded, which the code, the build, the platform or the machine may have "
        "caused. It is not a finding about the hardware. Regenerating the vector is a "
        "deliberate, reviewed act (docs/verification.md §4).\n";
  }
  return text;
}

}  // namespace loadforge::verification
