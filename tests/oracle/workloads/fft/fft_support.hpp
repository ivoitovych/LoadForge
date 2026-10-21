// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_ORACLE_WORKLOADS_FFT_FFT_SUPPORT_HPP
#define LOADFORGE_TESTS_ORACLE_WORKLOADS_FFT_FFT_SUPPORT_HPP

// What the FFT tests share: byte and double conversions, an independent direct
// DFT in long double (twiddles from the C library, which is fine in a test:
// the point is to be unlike the workload), executors with a chosen thread
// count, and a checker that applies the specification's check (spec.hpp, "The
// check, and its bounds") to a unit's input and output. The checker is the
// shape the wave-3 oracle takes; it is this stream's own, written from
// spec.hpp alone, and used to show that the workload meets the bound it is
// held to and that corruption of each kind is classified as it should be.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "worker/executor.hpp"
#include "workloads/fft/spec.hpp"
#include "workloads/fft/twiddle.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::fft::testing {

using Bytes = std::vector<std::byte>;
using LongComplex = std::complex<long double>;

inline Bytes to_bytes(const std::vector<double>& values) {
  Bytes bytes(values.size() * sizeof(double));
  for (std::size_t i = 0; i < values.size(); ++i) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &values[i], sizeof bits);
    for (std::size_t b = 0; b < 8; ++b) {
      bytes[8 * i + b] = static_cast<std::byte>((bits >> (8U * b)) & 0xFFU);
    }
  }
  return bytes;
}

inline std::vector<double> to_doubles(const Bytes& bytes) {
  std::vector<double> values(bytes.size() / sizeof(double));
  for (std::size_t i = 0; i < values.size(); ++i) {
    std::uint64_t bits = 0;
    for (std::size_t b = 0; b < 8; ++b) {
      bits |= std::to_integer<std::uint64_t>(bytes[8 * i + b]) << (8U * b);
    }
    std::memcpy(&values[i], &bits, sizeof bits);
  }
  return values;
}

/// The input of unit `index` as the specification defines it, without the workload.
inline std::vector<double> spec_input(std::uint64_t seed, std::uint64_t index) {
  std::vector<double> x(2 * kPoints);
  for (std::size_t e = 0; e < x.size(); ++e) {
    x[e] = entry_value(rng::u64_of(rng::block_at(seed, index, e)));
  }
  return x;
}

/// X_k = sum_j x_j exp(-2 pi i j k / n) for every k, in long double, for any n.
inline std::vector<LongComplex> direct_dft(const std::vector<double>& x) {
  const std::size_t n = x.size() / 2;
  const long double pi = std::acos(-1.0L);
  std::vector<LongComplex> w(n);
  for (std::size_t m = 0; m < n; ++m) {
    const long double angle = 2.0L * pi * static_cast<long double>(m) / static_cast<long double>(n);
    w[m] = LongComplex(std::cos(angle), -std::sin(angle));
  }
  std::vector<LongComplex> out(n);
  for (std::size_t k = 0; k < n; ++k) {
    LongComplex sum(0.0L, 0.0L);
    for (std::size_t j = 0; j < n; ++j) {
      sum += LongComplex(x[2 * j], x[2 * j + 1]) * w[(j * k) % n];
    }
    out[k] = sum;
  }
  return out;
}

inline double norm2(const std::vector<double>& v) {
  double sum = 0.0;
  for (const double value : v) {
    sum += value * value;
  }
  return std::sqrt(sum);
}

class Threads final : public worker::Executor {
 public:
  explicit Threads(std::size_t threads) : threads_(threads) {}
  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    ++runs;
    tasks += count;
    if (threads_ == 1) {
      for (std::size_t index = 0; index < count; ++index) {
        task(context, index, 0);
      }
      return;
    }
    std::vector<std::thread> pool;
    for (std::size_t slot = 0; slot < threads_; ++slot) {
      pool.emplace_back([this, slot, count, task, context] {
        for (std::size_t index = slot; index < count; index += threads_) {
          task(context, index, slot);
        }
      });
    }
    for (std::thread& thread : pool) {
      thread.join();
    }
  }
  std::size_t runs = 0;
  std::size_t tasks = 0;

 private:
  std::size_t threads_;
};

inline Bytes compute_unit(const Workload& workload, std::uint64_t index, const Bytes& input,
                          worker::Executor& executor) {
  Bytes output(workload.plan().output_bytes);
  Bytes scratch(workload.plan().scratch_bytes * executor.threads());
  workload.compute(index, input, output, scratch, executor);
  return output;
}

enum class Kind : std::uint8_t { kAgreement, kBoundedMismatch, kNotChecked };

struct Verdict {
  Kind kind = Kind::kNotChecked;
  std::size_t blocks_compared = 0;
  std::string check;
  std::size_t element = 0;
  double residual = 0.0;
  double tolerance = 0.0;
};

/// The kSampledBins distinct bins checked for unit `index`, increasing, always
/// including 0 and n/2.
inline std::array<std::size_t, kSampledBins> sampled_bins(std::uint64_t index) {
  std::vector<std::size_t> bins = {0, kPoints / 2};
  for (std::uint64_t draw = 0; bins.size() < kSampledBins; ++draw) {
    const auto k =
        static_cast<std::size_t>(rng::u64_of(rng::block_at(0x5A3D, index, draw)) % kPoints);
    if (std::find(bins.begin(), bins.end(), k) == bins.end()) {
      bins.push_back(k);
    }
  }
  std::sort(bins.begin(), bins.end());
  std::array<std::size_t, kSampledBins> out{};
  std::copy(bins.begin(), bins.end(), out.begin());
  return out;
}

/// X'_k of the specification: the direct sum with the table, in plain double loops.
inline Complex direct_bin(const std::vector<double>& x, std::size_t k) {
  double re = 0.0;
  double im = 0.0;
  for (std::size_t j = 0; j < kPoints; ++j) {
    const Complex w = kTwiddles[(j * k) % kPoints];
    re += x[2 * j] * w.re - x[2 * j + 1] * w.im;
    im += x[2 * j] * w.im + x[2 * j + 1] * w.re;
  }
  return Complex{re, im};
}

/// The specification's check of `output` against `input` for a unit computed
/// with `radix`: kSampledBins direct-DFT bins in increasing k, then Parseval.
inline Verdict check_unit(std::uint32_t radix, std::uint64_t index, const Bytes& input,
                          const Bytes& output) {
  Verdict verdict;
  if (input.size() != kInputBytes || output.size() != kOutputBytes) {
    return verdict;
  }
  const std::vector<double> x = to_doubles(input);
  const std::vector<double> y = to_doubles(output);
  bool all_zero = true;
  for (const double value : x) {
    if (!std::isfinite(value) || std::fabs(value) > 1.0) {
      return verdict;
    }
    all_zero = all_zero && value == 0.0;
  }
  if (all_zero) {
    return verdict;
  }
  const double a_hat = norm2(x);
  const double slack = 1.0 + kEvaluationSlack;
  const double bin_tolerance = 64.0 * a_hat * bin_tolerance_factor(radix) * slack;
  std::size_t checks = 0;
  for (const std::size_t k : sampled_bins(index)) {
    ++checks;
    const Complex direct = direct_bin(x, k);
    const double residual =
        std::max(std::fabs(y[2 * k] - direct.re), std::fabs(y[2 * k + 1] - direct.im));
    if (!(residual <= bin_tolerance)) {
      return Verdict{Kind::kBoundedMismatch, checks, "dft-bin", k, residual, bin_tolerance};
    }
  }
  ++checks;
  const double s_hat = norm2(y);
  const double parseval_tolerance = 64.0 * a_hat * parseval_tolerance_factor(radix) * slack;
  const double residual = std::fabs(s_hat - 64.0 * a_hat);
  if (!(residual <= parseval_tolerance)) {
    return Verdict{Kind::kBoundedMismatch, checks, "parseval", 0, residual, parseval_tolerance};
  }
  return Verdict{Kind::kAgreement, checks, "", 0, residual, parseval_tolerance};
}

}  // namespace loadforge::workloads::fft::testing

#endif
