// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/record/render.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "primitives/simd/dispatch.hpp"
#include "verification/record.hpp"
#include "verification/record/format.hpp"

namespace loadforge::verification {

namespace {

constexpr std::string_view kNotRecorded = "not recorded";

void field(std::string& out, std::string_view label, std::string_view value) {
  out += "    ";
  out += label;
  out += ": ";
  out += value;
  out += '\n';
}

// Each of these is a statement per arm rather than a conditional expression:
// a conditional whose arms build strings leaves a cleanup block for the arm
// not taken, with branches no test can enter (journal §1.13).
std::string decimal_or_absent(const std::optional<std::uint64_t>& value) {
  if (!value.has_value()) {
    return std::string(kNotRecorded);
  }
  return std::to_string(*value);
}

std::string widened(const std::optional<std::uint32_t>& value) {
  if (!value.has_value()) {
    return std::string(kNotRecorded);
  }
  return std::to_string(*value);
}

std::string hex_or_absent(const std::optional<std::uint64_t>& value) {
  if (!value.has_value()) {
    return std::string(kNotRecorded);
  }
  return hex(*value);
}

std::string text_or_absent(const std::optional<std::string>& value) {
  if (!value.has_value()) {
    return std::string(kNotRecorded);
  }
  return escaped(*value);
}

std::string shortest_or_absent(const std::optional<double>& value) {
  if (!value.has_value()) {
    return std::string(kNotRecorded);
  }
  return shortest(*value);
}

std::string period(std::uint32_t every) {
  if (every == 0) {
    return "no unit";
  }
  if (every == 1) {
    return "every unit";
  }
  return "1 unit in " + std::to_string(every);
}

std::string duty_words(const std::optional<DutyCycle>& duty) {
  if (!duty.has_value()) {
    return std::string(kNotRecorded);
  }
  std::string text = "oracle on ";
  text += period(duty->oracle_period);
  text += ", input integrity on ";
  text += period(duty->integrity_period);
  return text;
}

void put_provenance(std::string& out, const Provenance& provenance) {
  out += "  provenance:\n";
  field(out, "logical CPU", widened(provenance.logical_cpu));
  field(out, "physical core", widened(provenance.physical_core));
  field(out, "NUMA node", widened(provenance.numa_node));
  field(out, "buffer address", hex_or_absent(provenance.buffer_address));
  field(out, "buffer offset", hex_or_absent(provenance.buffer_offset));
  field(out, "iteration", decimal_or_absent(provenance.iteration));
  field(out, "workload", text_or_absent(provenance.workload));
  field(out, "workload version", widened(provenance.workload_version));
  field(out, "seed", hex_or_absent(provenance.seed));
  field(out, "ISA path",
        provenance.isa.has_value() ? simd::describe(*provenance.isa) : kNotRecorded);
  field(out, "input integrity",
        provenance.input_integrity.has_value() ? describe(*provenance.input_integrity)
                                               : kNotRecorded);
  field(out, "duty cycle", duty_words(provenance.duty));
}

std::string bytes_line(std::span<const std::byte> bytes) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string text;
  for (const std::byte byte : bytes) {
    const auto value = std::to_integer<unsigned>(byte);
    text += ' ';
    text += kDigits[value >> 4U];
    text += kDigits[value & 0x0FU];
  }
  return text;
}

void put_kind(std::string& out, const ExactBitCorruption& record) {
  out += "ExactBitCorruption: the ";
  out += describe(record.side);
  out += " was found corrupt\n";
  out += "  check: " + escaped(record.check) + "\n";
  if (!record.difference.has_value()) {
    out +=
        "  difference: none -- the bytes could not be compared one for one, so there is no "
        "bit evidence to classify\n";
    return;
  }
  const Difference& difference = *record.difference;
  out += "  difference in ";
  out += describe(difference.domain);
  out += ": " + std::to_string(difference.popcount) + " bit(s) differ, from byte " +
         std::to_string(difference.first_offset) + " to byte " +
         std::to_string(difference.last_offset) + " of the compared bytes\n";
  out += "  classification: ";
  out += describe(classify_bits(difference.popcount, difference.first_offset,
                                difference.last_offset, record.line_bytes));
  out += ", with " + std::to_string(record.line_bytes) + "-byte cache lines";
  if (difference.domain == Domain::kDerived) {
    out += " -- of bytes the oracle derived, not bits the hardware stored";
  }
  out += '\n';
  out += "  window from byte " + std::to_string(difference.window_offset) + ":\n";
  out += "    expected" + bytes_line(difference.expected) + "\n";
  out += "    actual  " + bytes_line(difference.actual) + "\n";
  out += "    xor     " + bytes_line(xor_delta(difference)) + "\n";
}

void put_kind(std::string& out, const NumericalVerificationFailure& record) {
  out += "NumericalVerificationFailure: a bounded check exceeded its derived tolerance\n";
  out += "  check: " + escaped(record.check) + "\n";
  out += "  residual: " + shortest(record.residual) + "\n";
  out += "  tolerance: " + shortest(record.tolerance) + "\n";
  out += "  ratio: " + shortest(ratio(record)) + " (residual over tolerance)\n";
  out += "  condition number: " + shortest_or_absent(record.condition_number) + "\n";
  out += "  element: " + decimal_or_absent(record.element) + "\n";
  out += "  bit fields: not applicable -- a bounded check has no expected bit pattern\n";
}

void put_kind(std::string& out, const VerificationNotPerformed& record) {
  out +=
      "VerificationNotPerformed: the check could not be made -- a failure of the run, not a "
      "pass, and no finding about the hardware\n";
  out += "  what: " + escaped(record.what) + "\n";
  out += "  why: ";
  if (record.why.empty()) {
    out += "not given";
  } else {
    out += escaped(record.why);
  }
  out += '\n';
  out += "  blocks compared: " + std::to_string(record.blocks_compared) + "\n";
}

}  // namespace

std::string render(const ErrorRecord& record) {
  std::string out;
  std::visit([&out](const auto& kind) { put_kind(out, kind); }, record);
  put_provenance(out, provenance_of(record));
  return out;
}

}  // namespace loadforge::verification
