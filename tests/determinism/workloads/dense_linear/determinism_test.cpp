// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6 for the dense-linear workload: BitExactFixedDecomposition, enforced
// (docs/determinism.md §2, §5).
//
//   * One (panel depth, path): the same bits at 1, 2, 3, 4 and 7 threads,
//     under shuffled and claiming schedules, and on every repeat. The full
//     sweep at the default depth, and 1 and 7 threads at every other.
//   * Which thread ran which tile is not part of the result (P5): two runs
//     whose tile-to-slot mappings are recorded and differ give the same bits.
//   * Across depths and across paths: within the derived bound
//     (support.hpp), entry by entry -- and NOT the same bits, which is what
//     makes the class the right one rather than one weaker than the algorithm
//     supports: a depth or a path that changed nothing would make this
//     workload BitExact.
//   * Across architectures: the scalar path's output digests, and the fused
//     paths' (AVX2 on x86-64, NEON on AArch64), against the digests committed
//     in tests/fixtures/golden/workloads/dense_linear/. One file serves both
//     architectures, so the scalar lines hold x86-64 and ARM64 to each other,
//     and the fused lines hold AVX2 and NEON to each other.
//   * The scalar kernel's whole product equals panel.hpp's model at every
//     depth (PanelKernelDeterminism; the vector kernels run it in their own
//     modules' T6).

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "primitives/simd/dispatch.hpp"
#include "unit/workloads/dense_linear/panel_kernel_suite.hpp"
#include "unit/workloads/dense_linear/support.hpp"
#include "worker/executor.hpp"
#include "workloads/dense_linear/dense_linear.hpp"
#include "workloads/dense_linear/panel.hpp"
#include "workloads/dense_linear/spec.hpp"

namespace loadforge::workloads::dense_linear::testing {
namespace {

using simd::IsaPath;

struct ScalarKernel {
  static constexpr bool kFused = false;
  static bool runs_here() { return true; }
  static void kernel(const Panel& panel) { panel_scalar(panel); }
};
INSTANTIATE_TYPED_TEST_SUITE_P(Scalar, PanelKernelDeterminism, ScalarKernel);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(PanelKernelSuite);

std::string name(IsaPath path, std::uint32_t depth) {
  return std::string(simd::describe(path)) + " at depth " + std::to_string(depth);
}

// ---- one (depth, path): any thread count, any schedule, any repeat -----------

TEST(DenseLinearDeterminism, TheThreadCountAndTheScheduleNeverChangeABit) {
  const std::vector<IsaPath> paths = runnable_paths();
  ASSERT_FALSE(paths.empty());
  for (const IsaPath path : paths) {
    for (const std::uint32_t depth : kPanelDepths) {
      const auto workload = make(config_for(path, depth, 11));
      ASSERT_NE(workload, nullptr);
      const Bytes input = generated(*workload, 3);
      Serial serial;
      const Bytes reference = computed(*workload, 3, input, serial);
      const bool full = depth == kDefaultPanelDepth;
      for (const std::size_t threads : {1U, 2U, 3U, 4U, 7U}) {
        if (!full && threads != 1 && threads != 7) {
          continue;
        }
        Shuffled shuffled(threads, threads * 1000 + depth);
        EXPECT_EQ(computed(*workload, 3, input, shuffled), reference)
            << name(path, depth) << ", " << threads << " threads, shuffled";
        if (full || threads == 7) {
          Claiming claiming(threads);
          EXPECT_EQ(computed(*workload, 3, input, claiming, std::byte{0x5A}), reference)
              << name(path, depth) << ", " << threads << " threads, claiming";
        }
      }
      EXPECT_EQ(computed(*workload, 3, input, serial), reference) << name(path, depth) << ", again";
    }
  }
}

/// Runs another executor and records, for each index, the slot that ran it.
class Recording final : public worker::Executor {
 public:
  explicit Recording(worker::Executor& inner) : inner_(inner) {}
  [[nodiscard]] std::size_t threads() const noexcept override { return inner_.threads(); }
  void run(std::size_t count, Task task, void* context) noexcept override {
    slots.assign(count, ~std::size_t{0});
    Forward forward{task, context, &slots};
    inner_.run(
        count,
        [](void* raw, std::size_t index, std::size_t slot) noexcept {
          auto* f = static_cast<Forward*>(raw);
          (*f->slots)[index] = slot;  // distinct indices: no two calls share an element
          f->task(f->context, index, slot);
        },
        &forward);
  }
  std::vector<std::size_t> slots;

 private:
  struct Forward {
    Task task;
    void* context;
    std::vector<std::size_t>* slots;
  };
  worker::Executor& inner_;
};

TEST(DenseLinearDeterminism, WhichThreadRanWhichTileIsNotPartOfTheResult) {
  for (const IsaPath path : runnable_paths()) {
    const auto workload = make(config_for(path, 32, 5));
    ASSERT_NE(workload, nullptr);
    const Bytes input = generated(*workload, 9);
    Shuffled first_schedule(3, 1);
    Shuffled second_schedule(3, 2);
    Recording first(first_schedule);
    Recording second(second_schedule);
    const Bytes one = computed(*workload, 9, input, first);
    const Bytes two = computed(*workload, 9, input, second);
    ASSERT_EQ(first.slots.size(), kTiles);
    EXPECT_NE(first.slots, second.slots)
        << "the two runs must put tiles on different slots, or this proves nothing";
    EXPECT_EQ(one, two) << simd::describe(path);
  }
}

// ---- across depths and paths: the bound, and only the bound -------------------

/// s for every entry, from the input as plain doubles.
std::vector<double> absolute_sums(const Bytes& input) {
  std::vector<double> a(kMatrix);
  std::vector<double> b(kMatrix);
  for (std::size_t e = 0; e < kMatrix; ++e) {
    a[e] = std::fabs(get(input, e));
    b[e] = std::fabs(get(input, kMatrix + e));
  }
  std::vector<double> s(kMatrix, 0.0);
  for (std::size_t i = 0; i < kN; ++i) {
    for (std::size_t k = 0; k < kN; ++k) {
      const double aik = a[i * kN + k];
      const double* row = b.data() + k * kN;
      double* sums = s.data() + i * kN;
      for (std::size_t j = 0; j < kN; ++j) {
        const double term = aik * row[j];
        sums[j] = sums[j] + term;
      }
    }
  }
  return s;
}

TEST(DenseLinearDeterminism, AcrossDepthsAndPathsTheBitsDifferWithinTheDerivedBound) {
  const auto scalar = make(config_for(IsaPath::kScalar, kDefaultPanelDepth, 21));
  ASSERT_NE(scalar, nullptr);
  const Bytes input = generated(*scalar, 0);
  const std::vector<double> s = absolute_sums(input);
  Serial serial;
  const Bytes reference = computed(*scalar, 0, input, serial);

  std::map<std::tuple<IsaPath, std::uint32_t>, Bytes> outputs;
  const auto output_of = [&outputs](IsaPath path, std::uint32_t depth) {
    return outputs.at(std::make_tuple(path, depth));
  };
  for (const IsaPath path : runnable_paths()) {
    for (const std::uint32_t depth : kPanelDepths) {
      const auto workload = make(config_for(path, depth, 21));
      ASSERT_NE(workload, nullptr);
      const Bytes output = computed(*workload, 0, input, serial);
      for (std::size_t e = 0; e < kMatrix; ++e) {
        ASSERT_TRUE(within_bound(get(output, e), get(reference, e), s[e]))
            << name(path, depth) << " against scalar at the default depth, entry " << e << ": "
            << get(output, e) << " and " << get(reference, e) << ", tolerance " << tolerance(s[e]);
      }
      outputs[{path, depth}] = output;
    }
  }
  for (const IsaPath path : runnable_paths()) {
    EXPECT_NE(output_of(path, 16), output_of(path, 256))
        << simd::describe(path) << ": the depth decides where the chains are cut";
    if (fused(path)) {
      EXPECT_NE(output_of(path, 64), output_of(IsaPath::kScalar, 64))
          << simd::describe(path) << " fuses its multiply-add and scalar does not";
    }
  }
}

// ---- across architectures: the committed digests -------------------------------

/// "scalar" or "fused", the depth, and the CRC-32 of unit 0's output at seed 1.
struct Golden {
  std::map<std::tuple<std::string, std::uint32_t>, std::uint32_t> digests;
};

Golden read_golden() {
  Golden golden;
  std::ifstream file(std::string(LOADFORGE_SOURCE_DIR) +
                     "/tests/fixtures/golden/workloads/dense_linear/output-crc32.txt");
  EXPECT_TRUE(file.is_open()) << "the committed digests are missing";
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    std::istringstream fields(line);
    std::string kind;
    std::uint32_t depth = 0;
    std::uint32_t crc = 0;
    fields >> kind >> depth >> std::hex >> crc;
    EXPECT_FALSE(fields.fail()) << "unreadable line: " << line;
    golden.digests[{kind, depth}] = crc;
  }
  return golden;
}

TEST(DenseLinearDeterminism, OutputsMatchTheDigestsCommittedForBothArchitectures) {
  // Every digest is computed and printed before any is compared, so a run on
  // a new machine shows all of its lines at once, in the fixture's format.
  std::vector<std::tuple<std::string, std::uint32_t, std::uint32_t>> computed_digests;
  for (const IsaPath path : runnable_paths()) {
    for (const std::uint32_t depth : kPanelDepths) {
      const auto workload = make(config_for(path, depth, 1));
      ASSERT_NE(workload, nullptr);
      Serial serial;
      const std::uint32_t crc = digest(computed(*workload, 0, generated(*workload, 0), serial));
      const std::string kind = fused(path) ? "fused" : "scalar";
      std::cout << "  computed: " << kind << ' ' << depth << ' ' << std::hex << std::setw(8)
                << std::setfill('0') << crc << std::dec << "  (" << simd::describe(path) << ")\n";
      computed_digests.emplace_back(kind, depth, crc);
    }
  }
  const Golden golden = read_golden();
  EXPECT_EQ(golden.digests.size(), 2 * kPanelDepths.size())
      << "a scalar and a fused digest for every depth";
  for (const auto& [kind, depth, crc] : computed_digests) {
    const auto expected = golden.digests.find({kind, depth});
    ASSERT_NE(expected, golden.digests.end()) << "no digest committed for " << kind << ' ' << depth;
    EXPECT_EQ(crc, expected->second) << kind << " at depth " << depth;
  }
  EXPECT_GE(computed_digests.size(), kPanelDepths.size())
      << "at least the scalar path, at every depth";
}

}  // namespace
}  // namespace loadforge::workloads::dense_linear::testing
