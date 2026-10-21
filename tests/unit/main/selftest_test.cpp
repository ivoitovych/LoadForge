// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/selftest.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "worker/executor.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::selftest {
namespace {

TEST(CoreDigest, IsStableAcrossRepeatedCalls) { EXPECT_EQ(core_digest(), core_digest()); }

TEST(CoreDigest, HexIsFixedWidthAndLowercase) {
  const std::string hex = core_digest_hex();
  EXPECT_EQ(hex.size(), 16U);
  for (const char c : hex) {
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << "non-hex digit: " << c;
  }
}

TEST(CoreDigest, HexMatchesTheNumericDigest) {
  const std::uint64_t value = core_digest();
  const std::string hex = core_digest_hex();
  std::uint64_t reconstructed = 0;
  for (const char c : hex) {
    reconstructed =
        (reconstructed << 4) | static_cast<std::uint64_t>(c <= '9' ? c - '0' : (c - 'a') + 10);
  }
  EXPECT_EQ(reconstructed, value);
}

TEST(Fnv1a, MatchesPublishedTestVectors) {
  // INDEPENDENT known-answer vectors, not self-consistency. The first version
  // of selftest.cpp used an offset basis of 1469598103934665603 -- nineteen
  // digits instead of twenty -- and every test still passed, because they all
  // compared the implementation against itself and one of them embedded the
  // same wrong constant.
  //
  // These values come from the FNV specification, not from this code.
  EXPECT_EQ(kFnvOffsetBasis, 0xcbf29ce484222325ULL);
  EXPECT_EQ(kFnvPrime, 0x100000001b3ULL);
  EXPECT_EQ(fnv1a("", kFnvOffsetBasis), 0xcbf29ce484222325ULL);
  EXPECT_EQ(fnv1a("a", kFnvOffsetBasis), 0xaf63dc4c8601ec8cULL);
  EXPECT_EQ(fnv1a("foobar", kFnvOffsetBasis), 0x85944171f73967e8ULL);
}

TEST(Fnv1a, ChainsAcrossCalls) {
  // core_digest folds the corpus by threading the hash through successive
  // calls, so chaining must equal hashing the concatenation.
  EXPECT_EQ(fnv1a("bar", fnv1a("foo", kFnvOffsetBasis)), fnv1a("foobar", kFnvOffsetBasis));
}

TEST(CoreDigest, IsNotTrivial) {
  // A digest of zero or of the bare offset basis would mean the corpus never
  // reached the hash.
  EXPECT_NE(core_digest(), 0U);
  EXPECT_NE(core_digest(), kFnvOffsetBasis);
}

}  // namespace
}  // namespace loadforge::selftest

// --- the workloads' scalar paths ----------------------------------------------

namespace loadforge::selftest {
namespace {

/// Three threads, each taking the indices of its residue: a different
/// schedule from the digest's own serial one, to show the digest is not a
/// function of it.
class ThreeThreads final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 3; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    std::vector<std::thread> pool;
    for (std::size_t slot = 0; slot < 3; ++slot) {
      pool.emplace_back([slot, count, task, context] {
        for (std::size_t index = slot; index < count; index += 3) {
          task(context, index, slot);
        }
      });
    }
    for (std::thread& thread : pool) {
      thread.join();
    }
  }
};

std::uint64_t digest_with(worker::Executor& executor, std::uint64_t basis) {
  const workloads::BuiltinRegistry registry;
  std::uint64_t hash = basis;
  const workloads::WorkloadConfig config{0x5E1F7E57ULL, std::uint64_t{8} << 20U, 0,
                                         simd::IsaPath::kScalar};
  for (const std::string_view name : {"compression", "dense_linear"}) {
    const auto created = registry.create(name, config);
    const workloads::Workload& workload = *created.value();
    const workloads::Plan& plan = workload.plan();
    std::vector<std::byte> input(plan.input_bytes);
    std::vector<std::byte> output(plan.output_bytes);
    std::vector<std::byte> scratch(plan.scratch_bytes * executor.threads());
    hash = fnv1a(name, hash);
    for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{4097}}) {
      workload.generate(index, input);
      workload.compute(index, input, output, scratch, executor);
      hash = fnv1a({reinterpret_cast<const char*>(output.data()), output.size()}, hash);
    }
  }
  return hash;
}

TEST(WorkloadsDigest, FoldsTheScalarOutputsOfBothWorkloadsAndNothingElse) {
  ThreeThreads threads;
  EXPECT_EQ(workloads_digest(kFnvOffsetBasis), digest_with(threads, kFnvOffsetBasis))
      << "the digest is the outputs' hash, whatever schedule computed them";
  EXPECT_EQ(workloads_digest(7), digest_with(threads, 7))
      << "and it continues the basis it is given";
}

TEST(WorkloadsDigest, DependsOnItsBasisAndIsStable) {
  EXPECT_EQ(workloads_digest(1), workloads_digest(1));
  EXPECT_NE(workloads_digest(1), workloads_digest(2));
  EXPECT_NE(workloads_digest(kFnvOffsetBasis), kFnvOffsetBasis);
}

}  // namespace
}  // namespace loadforge::selftest
