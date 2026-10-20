// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_ORACLE_WORKLOADS_MONTE_CARLO_REPLAY_HPP
#define LOADFORGE_TESTS_ORACLE_WORKLOADS_MONTE_CARLO_REPLAY_HPP

// A scalar replay of Monte Carlo batches, written from spec.hpp alone and
// never from the kernels (verification.md §3): one sample at a time through
// the specification's own sample_bits() and sample_value(), summed in the
// order the specification's DECOMPOSITION and a path's lane count define,
// and, separately, summed exactly. The check the specification's THE CHECK
// section describes is written out in judge(): it stands in for the oracle
// stream's, which this module neither reads nor needs.

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "workloads/monte_carlo/spec.hpp"

namespace loadforge::workloads::monte_carlo::replay {

inline std::uint64_t load_word(std::span<const std::byte> bytes) {
  std::uint64_t value = 0;
  for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
    value |= std::uint64_t{static_cast<std::uint8_t>(bytes[byte])} << (8U * byte);
  }
  return value;
}

inline double load_double(std::span<const std::byte> bytes) {
  return std::bit_cast<double>(load_word(bytes));
}

inline std::uint64_t key_of(std::span<const std::byte> input, std::uint64_t batch) {
  return load_word(input.subspan(batch * 8, 8));
}

/// f_k, one sample at a time, from the specification's definitions.
inline double f(std::uint64_t key, std::uint64_t k) { return sample_value(sample_bits(key, k)); }

/// T_b for a path of `lanes` accumulators and chunks of `chunk` samples: the
/// order of spec.hpp's DECOMPOSITION, spelt out with one vector of
/// accumulators per chunk.
inline double ordered_sum(std::uint64_t key, unsigned lanes, std::uint32_t chunk) {
  double total = 0.0;
  for (std::uint64_t first = 0; first < kSamplesPerBatch; first += chunk) {
    std::vector<double> accumulator(lanes, 0.0);
    for (std::uint64_t offset = 0; offset < chunk; ++offset) {
      accumulator[offset % lanes] += f(key, first + offset);
    }
    double chunk_sum = accumulator[0];
    for (unsigned lane = 1; lane < lanes; ++lane) {
      chunk_sum += accumulator[lane];
    }
    total = first == 0 ? chunk_sum : total + chunk_sum;
  }
  return total;
}

/// The chunk-sum of samples [first, first + length) for `lanes` accumulators.
inline double ordered_chunk(std::uint64_t key, unsigned lanes, std::uint64_t first,
                            std::uint32_t length) {
  std::vector<double> accumulator(lanes, 0.0);
  for (std::uint64_t offset = 0; offset < length; ++offset) {
    accumulator[offset % lanes] += f(key, first + offset);
  }
  double sum = accumulator[0];
  for (unsigned lane = 1; lane < lanes; ++lane) {
    sum += accumulator[lane];
  }
  return sum;
}

/// sum f_k exactly, rounded once: every f_k is m 2^e with a 53-bit m and
/// e in [-25, 1], so f_k 2^80 is an integer below 2^81 and 2^14 of them fit
/// an unsigned 128-bit sum with room to spare. Integer addition is exact and
/// associative; the only rounding is the conversion at the end.
__extension__ using Wide = unsigned __int128;

inline double exact_sum(std::uint64_t key) {
  Wide total = 0;
  for (std::uint64_t k = 0; k < kSamplesPerBatch; ++k) {
    int exponent = 0;
    const double mantissa = std::frexp(f(key, k), &exponent);
    const auto integer = static_cast<std::uint64_t>(std::ldexp(mantissa, 53));
    total += static_cast<Wide>(integer) << (exponent + 27);
  }
  return std::ldexp(static_cast<double>(total), -80);
}

enum class Verdict : std::uint8_t { kAgreement, kBoundedMismatch, kNotChecked };

/// What the check answers. `batch` is the smallest failing batch number for
/// a mismatch; residual and tolerance are those of that batch.
struct Outcome {
  Verdict verdict = Verdict::kNotChecked;
  std::uint64_t blocks_compared = 0;
  std::uint64_t batch = 0;
  double residual = 0.0;
  double tolerance = 0.0;
};

/// The replayed sums of the batches unit `index` samples, from the input.
struct Reference {
  std::array<std::uint64_t, kSampledBatches> batch{};
  std::array<double, kSampledBatches> sum{};
};

inline Reference reference_for(std::uint64_t index, std::span<const std::byte> input) {
  Reference reference;
  for (std::size_t m = 0; m < kSampledBatches; ++m) {
    reference.batch.at(m) = sampled_batch(index, m);
    reference.sum.at(m) = exact_sum(key_of(input, reference.batch.at(m)));
  }
  return reference;
}

/// THE CHECK of spec.hpp, VERDICTS: every sampled batch is compared; the
/// smallest batch number whose residual is not within tolerance is reported.
inline Outcome judge(const Reference& reference, std::span<const std::byte> output) {
  Outcome outcome;
  outcome.verdict = Verdict::kAgreement;
  outcome.blocks_compared = kSampledBatches;
  bool failed = false;
  for (std::size_t m = 0; m < kSampledBatches; ++m) {
    const std::uint64_t batch = reference.batch.at(m);
    const double reference_sum = reference.sum.at(m);
    const double residual = std::fabs(load_double(output.subspan(batch * 8, 8)) - reference_sum);
    const double allowed = tolerance(reference_sum);
    // Written as the negation so that NaN fails, as the specification says.
    if (!(residual <= allowed) && (!failed || batch < outcome.batch)) {
      failed = true;
      outcome.verdict = Verdict::kBoundedMismatch;
      outcome.batch = batch;
      outcome.residual = residual;
      outcome.tolerance = allowed;
    }
  }
  return outcome;
}

/// The whole check: NotChecked, replaying nothing, for evidence of the wrong
/// size; otherwise replay and judge.
inline Outcome check(std::uint64_t index, std::span<const std::byte> input,
                     std::span<const std::byte> output) {
  if (input.size() != kInputBytes || output.size() != kOutputBytes) {
    return Outcome{};
  }
  return judge(reference_for(index, input), output);
}

}  // namespace loadforge::workloads::monte_carlo::replay

#endif
