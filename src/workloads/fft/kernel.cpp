// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/fft/kernel.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "workloads/fft/spec.hpp"
#include "workloads/fft/twiddle.hpp"

namespace loadforge::workloads::fft {

double load_double(std::span<const std::byte> bytes, std::size_t index) noexcept {
  std::uint64_t bits = 0;
  for (std::size_t byte = 0; byte < sizeof(double); ++byte) {
    bits |= std::to_integer<std::uint64_t>(bytes[index * sizeof(double) + byte]) << (8U * byte);
  }
  return std::bit_cast<double>(bits);
}

// An index and a value; the call sites name both.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void store_double(std::span<std::byte> bytes, std::size_t index, double value) noexcept {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  for (std::size_t byte = 0; byte < sizeof(double); ++byte) {
    bytes[index * sizeof(double) + byte] = static_cast<std::byte>((bits >> (8U * byte)) & 0xFFU);
  }
}

namespace {

[[nodiscard]] Complex load(std::span<const std::byte> bytes, std::size_t element) noexcept {
  return Complex{load_double(bytes, 2 * element), load_double(bytes, 2 * element + 1)};
}

void store(std::span<std::byte> bytes, std::size_t element, Complex value) noexcept {
  store_double(bytes, 2 * element, value.re);
  store_double(bytes, 2 * element + 1, value.im);
}

// No fused multiply-add anywhere in the scalar path: the project builds with
// -ffp-contract=off, so each operator below is one IEEE operation and the
// scalar path gives the same bits on every architecture (determinism.md §3).
[[nodiscard]] Complex times(Complex x, Complex w) noexcept {
  return Complex{x.re * w.re - x.im * w.im, x.re * w.im + x.im * w.re};
}
[[nodiscard]] Complex plus(Complex x, Complex y) noexcept {
  return Complex{x.re + y.re, x.im + y.im};
}
[[nodiscard]] Complex minus(Complex x, Complex y) noexcept {
  return Complex{x.re - y.re, x.im - y.im};
}
/// -i x: a swap of parts and a sign change.
[[nodiscard]] Complex times_minus_i(Complex x) noexcept { return Complex{x.im, -x.re}; }

[[nodiscard]] Complex twiddled(const StageGeometry& g, std::size_t a, std::size_t v,
                               Complex x) noexcept {
  const std::span<const Complex> table{kTwiddles};
  return times(x, table[a * v * g.tail * g.stride]);
}

void radix2_scalar(const StageGeometry& g, std::span<const std::byte> in, std::span<std::byte> out,
                   std::size_t first, std::size_t last) noexcept {
  for (std::size_t butterfly = first; butterfly < last; ++butterfly) {
    const std::size_t v = butterfly / g.tail;
    const std::size_t c = butterfly % g.tail;
    const std::size_t read = 2 * v * g.tail + c;
    const Complex z0 = load(in, read);
    const Complex z1 = twiddled(g, 1, v, load(in, read + g.tail));
    const std::size_t write = v * g.tail + c;
    store(out, write, plus(z0, z1));
    store(out, write + g.length * g.tail, minus(z0, z1));
  }
}

void radix4_scalar(const StageGeometry& g, std::span<const std::byte> in, std::span<std::byte> out,
                   std::size_t first, std::size_t last) noexcept {
  for (std::size_t butterfly = first; butterfly < last; ++butterfly) {
    const std::size_t v = butterfly / g.tail;
    const std::size_t c = butterfly % g.tail;
    const std::size_t read = 4 * v * g.tail + c;
    const Complex z0 = load(in, read);
    const Complex z1 = twiddled(g, 1, v, load(in, read + g.tail));
    const Complex z2 = twiddled(g, 2, v, load(in, read + 2 * g.tail));
    const Complex z3 = twiddled(g, 3, v, load(in, read + 3 * g.tail));
    const Complex sum02 = plus(z0, z2);
    const Complex diff02 = minus(z0, z2);
    const Complex sum13 = plus(z1, z3);
    const Complex rotated = times_minus_i(minus(z1, z3));
    const std::size_t write = v * g.tail + c;
    const std::size_t step = g.length * g.tail;
    store(out, write, plus(sum02, sum13));
    store(out, write + step, plus(diff02, rotated));
    store(out, write + 2 * step, minus(sum02, sum13));
    store(out, write + 3 * step, minus(diff02, rotated));
  }
}

struct StageJob {
  StageKernel kernel;
  StageGeometry geometry;
  std::span<const std::byte> in;
  std::span<std::byte> out;
  std::size_t butterflies;
};

void run_task(void* context, std::size_t index, std::size_t /*slot*/) noexcept {
  const auto& job = *static_cast<const StageJob*>(context);
  const std::size_t first = index * kButterfliesPerTask;
  const std::size_t end = first + kButterfliesPerTask;
  job.kernel(job.geometry, job.in, job.out, first, end < job.butterflies ? end : job.butterflies);
}

}  // namespace

simd::KernelTable<StageKernel> stage_kernels(std::uint32_t radix) noexcept {
  simd::KernelTable<StageKernel> table;
  table.scalar = radix == 2 ? &radix2_scalar : &radix4_scalar;
  return table;
}

void transform(StageKernel kernel, std::uint32_t radix, std::size_t n,
               std::span<const std::byte> in, std::span<std::byte> out,
               std::span<std::byte> scratch, worker::Executor& executor) noexcept {
  std::size_t stages = 0;
  for (std::size_t length = 1; length < n; length *= radix) {
    ++stages;
  }
  std::size_t length = 1;
  std::span<const std::byte> source = in;
  for (std::size_t stage = 0; stage < stages; ++stage) {
    // The last stage writes `out`; the one before it `scratch`; and so back.
    const std::span<std::byte> target = (stages - 1 - stage) % 2 == 0 ? out : scratch;
    StageJob job{kernel, StageGeometry{length, n / (radix * length), kPoints / n}, source, target,
                 n / radix};
    const std::size_t tasks = (job.butterflies + kButterfliesPerTask - 1) / kButterfliesPerTask;
    executor.run(tasks, &run_task, &job);
    source = target;
    length *= radix;
  }
}

}  // namespace loadforge::workloads::fft
