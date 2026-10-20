// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/sparse/sparse.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <utility>

#include "core/result.hpp"
#include "primitives/rng/philox.hpp"
#include "primitives/simd/dispatch.hpp"
#include "workloads/sparse/kernels.hpp"
#include "workloads/sparse/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::sparse {
namespace {

using KernelsAt = const Kernels& (*)() noexcept;

/// One kernel set per path (dispatch.hpp). Scalar is the only one today.
constexpr simd::KernelTable<KernelsAt> kKernelTable{.scalar = &scalar_kernels};

// The traffic and the operations of one unit, by the algorithm (PLAN F12):
// every operand of each step read or written once, wherever it lives.
constexpr std::uint64_t kN = kRows;
constexpr std::uint64_t kSpmvBytes = kNonZeros * (sizeof(double) + sizeof(std::uint32_t) +
                                                  sizeof(double)) +     // value, column, x gather
                                     (kN + 1) * sizeof(std::uint32_t);  // row_ptr
constexpr std::uint64_t kVectorBytes = kN * sizeof(double);
// init: read b, write x, r, p, and read r once for r . r.
constexpr std::uint64_t kInitBytes = 5 * kVectorBytes;
// per iteration: q = A p (written), p . q (2 reads), x and r updates (4 reads, 2 writes),
// r . r (1 read), p update (2 reads, 1 write).
constexpr std::uint64_t kIterationBytes = kSpmvBytes + 13 * kVectorBytes;
// the output: the SpMV of the residual, b read, x and the residual written.
constexpr std::uint64_t kFinishBytes = kSpmvBytes + 3 * kVectorBytes;
constexpr std::uint64_t kLogicalBytes = kInitBytes + kIterationBytes * kIterations + kFinishBytes;
// A product and a sum per stored entry for each SpMV, and per step per row:
// init 2 (r . r), p . q 2, x and r updates 4, r . r 2, p update 2.
constexpr std::uint64_t kOperations =
    2 * kN + (2 * kNonZeros + 10 * kN) * kIterations + 2 * kNonZeros;

// The scratch one unit needs: x, r, p, q, the chunk partials, and a double's
// alignment of slack, because scratch is only promised as bytes.
constexpr std::uint64_t kScratchBytes = (4 * kN + kMaxChunks) * sizeof(double) + alignof(double);

/// What one run() over the chunks does: the kernel it calls, with what.
struct Job {
  Kernel kernel = nullptr;
  const Work* work = nullptr;
  std::span<double> partials;
  std::size_t chunk_rows = 0;
  double coefficient = 0.0;
};

/// The task the executor calls for chunk `chunk`. A named function, and the
/// slot unused: the chunk alone decides what is computed and where it is
/// written, so no thread can show in the result.
void run_chunk(void* context, std::size_t chunk, std::size_t /*slot*/) noexcept {
  const Job& job = *static_cast<Job*>(context);
  const Rows rows{chunk * job.chunk_rows, (chunk + 1) * job.chunk_rows};
  job.partials[chunk] = job.kernel(*job.work, rows, job.coefficient);
}

/// One step over every chunk, and the combined partial sum.
double step(worker::Executor& executor, Job& job, Kernel kernel, double coefficient) noexcept {
  job.kernel = kernel;
  job.coefficient = coefficient;
  executor.run(job.partials.size(), run_chunk, &job);
  return reduce_tree(job.partials);
}

/// The double-aligned doubles at the start of `scratch`.
std::span<double> doubles_in(std::span<std::byte> scratch, std::size_t count) noexcept {
  const auto address = std::bit_cast<std::uintptr_t>(scratch.data());
  const std::size_t pad = (alignof(double) - address % alignof(double)) % alignof(double);
  const std::span<std::byte> aligned = scratch.subspan(pad, count * sizeof(double));
  return {std::bit_cast<double*>(aligned.data()), count};
}

double pair_value(std::uint64_t seed, std::uint64_t unit, std::size_t row,
                  std::size_t offset_index) noexcept {
  return off_diagonal_value(rng::u64_of(rng::block_at(seed, unit, 3 * row + offset_index)));
}

class SparseWorkload final : public Workload {
 public:
  SparseWorkload(const WorkloadConfig& config, const Plan& plan, KernelsAt kernels) noexcept
      : descriptor_{std::string(kName), 1, DeterminismClass::kBitExactFixedDecomposition,
                    verification::Contract::kBounded, simd::kernel_paths(kKernelTable)},
        plan_(plan),
        seed_(config.seed),
        kernels_(kernels) {}

  [[nodiscard]] const Descriptor& descriptor() const noexcept override { return descriptor_; }
  [[nodiscard]] const Plan& plan() const noexcept override { return plan_; }

  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    const std::span<std::byte> values = input.subspan(kValuesOffset, kNonZeros * sizeof(double));
    const std::span<std::byte> rhs = input.subspan(kRhsOffset, kRows * sizeof(double));
    const std::span<std::byte> columns =
        input.subspan(kColumnsOffset, kNonZeros * sizeof(std::uint32_t));
    const std::span<std::byte> row_ptr =
        input.subspan(kRowPtrOffset, (kRows + 1) * sizeof(std::uint32_t));
    std::size_t entry = 0;
    const auto put = [&](std::size_t column, double value) noexcept {
      store_f64(values, entry, value);
      store_u32(columns, entry, static_cast<std::uint32_t>(column));
      ++entry;
    };
    for (std::size_t row = 0; row < kRows; ++row) {
      store_u32(row_ptr, row, static_cast<std::uint32_t>(entry));
      // The entry left of the diagonal at distance o_k is the one pair (row - o_k, k) made.
      std::size_t k = kOffsets.size();
      for (const std::size_t offset : std::views::reverse(kOffsets)) {
        --k;
        if (row >= offset) {
          put(row - offset, pair_value(seed_, index, row - offset, k));
        }
      }
      put(row, kDiagonal);
      k = 0;
      for (const std::size_t offset : kOffsets) {
        if (row + offset < kRows) {
          put(row + offset, pair_value(seed_, index, row, k));
        }
        ++k;
      }
      store_f64(rhs, row, entry_value(rng::u64_of(rng::block_at(seed_, index, 3 * kRows + row))));
    }
    store_u32(row_ptr, kRows, static_cast<std::uint32_t>(entry));
  }

  // The parameter list is the contract's (workload.hpp): output and scratch are both spans.
  void compute(std::uint64_t /*index*/, std::span<const std::byte> input,
               std::span<std::byte> output,  // NOLINT(bugprone-easily-swappable-parameters)
               std::span<std::byte> scratch, worker::Executor& executor) const noexcept override {
    const std::size_t chunk_rows = plan_.decomposition;
    const std::size_t chunks = kRows / chunk_rows;
    const std::span<double> storage = doubles_in(scratch, 4 * kRows + kMaxChunks);
    const Work work{Matrix{input.subspan(kValuesOffset, kNonZeros * sizeof(double)),
                           input.subspan(kColumnsOffset, kNonZeros * sizeof(std::uint32_t)),
                           input.subspan(kRowPtrOffset, (kRows + 1) * sizeof(std::uint32_t))},
                    input.subspan(kRhsOffset, kRows * sizeof(double)),
                    storage.subspan(0, kRows),
                    storage.subspan(kRows, kRows),
                    storage.subspan(2 * kRows, kRows),
                    storage.subspan(3 * kRows, kRows),
                    output};
    Job job;
    job.work = &work;
    job.partials = storage.subspan(4 * kRows, chunks);
    job.chunk_rows = chunk_rows;

    const Kernels& kernels = kernels_();
    double rr = step(executor, job, kernels.init, 0.0);
    for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
      const double pq = step(executor, job, kernels.spmv_dot, 0.0);
      const double alpha = rr / pq;
      const double next_rr = step(executor, job, kernels.update_xr, alpha);
      const double beta = next_rr / rr;
      rr = next_rr;
      step(executor, job, kernels.update_p, beta);
    }
    step(executor, job, kernels.finish, 0.0);
  }

 private:
  Descriptor descriptor_;
  Plan plan_;
  std::uint64_t seed_;
  KernelsAt kernels_;
};

}  // namespace

core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& config) {
  const KernelsAt kernels = simd::kernel_at(kKernelTable, config.isa);
  if (kernels == nullptr) {
    std::string detail = "no sparse kernels for the ISA path ";
    detail += simd::describe(config.isa);
    return WorkloadError{WorkloadFault::kConfiguration, std::move(detail)};
  }
  const std::uint32_t chunk_rows =
      config.decomposition == 0 ? kDefaultChunkRows : config.decomposition;
  if (!is_chunk_rows(chunk_rows)) {
    std::string detail = "chunk rows ";
    detail += std::to_string(chunk_rows);
    detail += " is not one of 256, 512, 1024, 2048";
    return WorkloadError{WorkloadFault::kConfiguration, std::move(detail)};
  }
  const std::uint64_t unit_bytes = kInputBytes + kOutputBytes;
  const std::uint64_t units = config.footprint_bytes / unit_bytes;
  if (units == 0) {
    std::string detail = std::to_string(config.footprint_bytes);
    detail += " bytes hold no unit of ";
    detail += std::to_string(unit_bytes);
    return WorkloadError{WorkloadFault::kFootprintTooSmall, std::move(detail)};
  }
  const Plan plan{kInputBytes, kOutputBytes,  kScratchBytes, units,
                  chunk_rows,  kLogicalBytes, kOperations};
  return std::unique_ptr<Workload>(new SparseWorkload(config, plan, kernels));
}

}  // namespace loadforge::workloads::sparse
