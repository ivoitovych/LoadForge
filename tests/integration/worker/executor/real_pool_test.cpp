// SPDX-License-Identifier: GPL-3.0-or-later
//
// T8: the pool against the real kernel and the real floating-point registers.
//
// The unit tier forces every refusal through fakes; this tier checks each
// thing those fakes model against the thing itself (F21). That a thread
// pinned with pid 0 is pinned alone, and where it was asked. That the kernel
// refuses a CPU the machine lacks with EINVAL, as the fake does, and the pool
// reports it the same way. And that a thread inherits its creator's
// floating-point state -- the property the pool's start-up check, and the
// executor's model of FpControl, both rest on, which fpenv.hpp says a
// provider must confirm on the real platform.

#include <gtest/gtest.h>
#include <pthread.h>
#include <sched.h>

#include <cerrno>
#include <cfenv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "contracts/fpenv/fp_control_contract.hpp"
#include "contracts/worker/executor_contract.hpp"
#include "platform/affinity.hpp"
#include "platform/fpenv/fpenv.hpp"
#include "platform/syscalls.hpp"
#include "unit/worker/executor/fakes.hpp"
#include "worker/executor.hpp"
#include "worker/executor/thread_pool.hpp"

#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace loadforge::worker {
namespace {

using platform::CpuMask;

// ---- the registers themselves ---------------------------------------------
//
// An FpControl over the real registers of this architecture, small enough to
// read at a glance: rounding through <cfenv>, the flags through MXCSR or FPCR.
// It is not the numerics stream's provider and is not shipped; it exists so
// that the executor's reading of the contract -- the model in fakes.hpp -- is
// run through the same suite as the real registers, and so that the pool's
// start-up check can be made against real state.

#if defined(__x86_64__)
constexpr unsigned kMxcsrFlushToZero = 1U << 15;
constexpr unsigned kMxcsrDenormalsAreZero = 1U << 6;

unsigned read_flags() { return _mm_getcsr(); }
void write_flags(unsigned flags) { _mm_setcsr(flags); }
#elif defined(__aarch64__)
constexpr std::uint64_t kFpcrFlushToZero = std::uint64_t{1} << 24;
constexpr std::uint64_t kFpcrDefaultNan = std::uint64_t{1} << 25;

std::uint64_t read_flags() {
  std::uint64_t fpcr = 0;
  asm volatile("mrs %0, fpcr" : "=r"(fpcr));
  return fpcr;
}
void write_flags(std::uint64_t fpcr) { asm volatile("msr fpcr, %0" : : "r"(fpcr)); }
#else
#error "LoadForge targets x86-64 and AArch64 (docs/PLAN.md §4.5)"
#endif

fpenv::Rounding rounding_of(int mode) {
  switch (mode) {
    case FE_TOWARDZERO:
      return fpenv::Rounding::kTowardZero;
    case FE_UPWARD:
      return fpenv::Rounding::kUpward;
    case FE_DOWNWARD:
      return fpenv::Rounding::kDownward;
    default:
      return fpenv::Rounding::kToNearestEven;
  }
}

int mode_of(fpenv::Rounding rounding) {
  switch (rounding) {
    case fpenv::Rounding::kTowardZero:
      return FE_TOWARDZERO;
    case fpenv::Rounding::kUpward:
      return FE_UPWARD;
    case fpenv::Rounding::kDownward:
      return FE_DOWNWARD;
    default:
      return FE_TONEAREST;
  }
}

class RegisterFpControl final : public fpenv::FpControl {
 public:
#if defined(__x86_64__)
  static constexpr bool kHasDenormalsAreZero = true;
  static constexpr bool kHasDefaultNan = false;
#else
  static constexpr bool kHasDenormalsAreZero = false;
  static constexpr bool kHasDefaultNan = true;
#endif

  [[nodiscard]] fpenv::FpState read() const override {
    fpenv::FpState state;
    state.rounding = rounding_of(std::fegetround());
    const auto flags = read_flags();
#if defined(__x86_64__)
    state.flush_to_zero = (flags & kMxcsrFlushToZero) != 0;
    state.denormals_are_zero = (flags & kMxcsrDenormalsAreZero) != 0;
#else
    state.flush_to_zero = (flags & kFpcrFlushToZero) != 0;
    state.default_nan = (flags & kFpcrDefaultNan) != 0;
#endif
    return state;
  }

  void write(const fpenv::FpState& state) override {
    (void)std::fesetround(mode_of(state.rounding));
    auto flags = read_flags();
#if defined(__x86_64__)
    flags = state.flush_to_zero ? (flags | kMxcsrFlushToZero) : (flags & ~kMxcsrFlushToZero);
    if (state.denormals_are_zero.has_value()) {
      flags = *state.denormals_are_zero ? (flags | kMxcsrDenormalsAreZero)
                                        : (flags & ~kMxcsrDenormalsAreZero);
    }
#else
    flags = state.flush_to_zero ? (flags | kFpcrFlushToZero) : (flags & ~kFpcrFlushToZero);
    if (state.default_nan.has_value()) {
      flags = *state.default_nan ? (flags | kFpcrDefaultNan) : (flags & ~kFpcrDefaultNan);
    }
#endif
    write_flags(flags);
  }
};

/// Restores the calling thread's registers when it leaves scope, so a failed
/// assertion cannot leave the rest of the run rounding upward.
class RestoreRegisters {
 public:
  RestoreRegisters() : found_(control_.read()) {}
  RestoreRegisters(const RestoreRegisters&) = delete;
  RestoreRegisters(RestoreRegisters&&) = delete;
  RestoreRegisters& operator=(const RestoreRegisters&) = delete;
  RestoreRegisters& operator=(RestoreRegisters&&) = delete;
  ~RestoreRegisters() { control_.write(found_); }

 private:
  RegisterFpControl control_;
  fpenv::FpState found_;
};

// ---- the CPUs this process may use --------------------------------------------

std::vector<std::uint32_t> permitted_cpus() {
  platform::RealSyscalls syscalls;
  return platform::ids_of(syscalls.get_affinity(0).value());
}

/// Up to `most` of the CPUs this process may use, lowest first: valid under any
/// cpuset, and never more threads than a CI runner has CPUs.
CpuMask some_permitted(std::size_t most) {
  CpuMask mask;
  for (const std::uint32_t cpu : permitted_cpus()) {
    if (mask.cpus.count() == most) {
      break;
    }
    mask.cpus.set(cpu);
  }
  return mask;
}

/// The calling thread's mask, asked of the kernel directly rather than through
/// RealSyscalls: tasks call this concurrently, and the platform library's
/// coverage counters are not atomic (src/worker/executor/CMakeLists.txt).
CpuMask own_affinity() {
  cpu_set_t raw;
  CPU_ZERO(&raw);
  CpuMask mask;
  if (sched_getaffinity(0, sizeof raw, &raw) != 0) {
    return mask;  // empty: never equal to a placement, so the test says so
  }
  for (std::size_t cpu = 0; cpu < platform::kAffinityMaskBits; ++cpu) {
    if (CPU_ISSET(cpu, &raw)) {
      mask.cpus.set(cpu);
    }
  }
  return mask;
}

// ---- pinning ----------------------------------------------------------------------

TEST(RealPool, EachThreadRunsOnItsOwnCpuAndTheCallerIsNotMoved) {
  platform::RealSyscalls syscalls;
  PosixThreads threads;
  const RegisterFpControl fp;
  const CpuMask before = syscalls.get_affinity(0).value();
  const CpuMask chosen = some_permitted(4);
  const std::vector<ThreadPlacement> placements = one_thread_per_cpu(chosen);
  ASSERT_FALSE(placements.empty());

  const auto made = make_thread_pool(placements, threads, syscalls, fp);
  ASSERT_TRUE(made.has_value()) << describe(made.error());
  Executor& executor = *made.value();
  ASSERT_EQ(executor.threads(), placements.size());

  // Written by the slot's own thread only, read after run: the contract's
  // ordering is what makes these plain vectors safe.
  std::vector<std::optional<CpuMask>> affinity(placements.size());
  std::vector<int> ran_on(placements.size(), -1);
  std::vector<int> moved(placements.size(), 0);
  auto observe = [&](std::size_t, std::size_t slot) noexcept {
    affinity[slot] = own_affinity();
    const int cpu = sched_getcpu();
    if (ran_on[slot] >= 0 && ran_on[slot] != cpu) {
      moved[slot] += 1;
    }
    ran_on[slot] = cpu;
  };
  for (int round = 0; round < 20; ++round) {
    for_each_index(executor, 8 * placements.size(), observe);
  }

  for (std::size_t slot = 0; slot < placements.size(); ++slot) {
    if (!affinity[slot].has_value()) {
      continue;  // a slot that drew no index in twenty runs has nothing to show
    }
    EXPECT_EQ(affinity[slot], placements[slot]) << "slot " << slot;
    // sched_getcpu(3) is the kernel's answer through a different call: the
    // CPU the thread was on, which pinning to one CPU makes that CPU.
    EXPECT_TRUE(placements[slot]->cpus.test(static_cast<std::size_t>(ran_on[slot])))
        << "slot " << slot << " ran on CPU " << ran_on[slot];
    EXPECT_EQ(moved[slot], 0) << "slot " << slot;
  }
  EXPECT_EQ(syscalls.get_affinity(0).value(), before)
      << "pid 0 is the calling thread: placing the pool's threads moves no other";
}

// The fake refuses a mask the cpuset leaves nothing of with EINVAL; so does
// the kernel, for a CPU this machine does not have and for no CPU at all. The
// pool reports both as the unit tier asserts it reports the fake's.
TEST(RealPool, ACpuTheKernelCannotUseIsRefusedWithEinvalAsTheFakeModelsIt) {
  const std::vector<std::uint32_t> permitted = permitted_cpus();
  if (permitted.back() == platform::kAffinityMaskBits - 1) {
    GTEST_SKIP() << "this process may use the highest CPU a mask can name";
  }
  CpuMask missing;
  missing.cpus.set(platform::kAffinityMaskBits - 1);
  for (const CpuMask& refused_mask : {missing, CpuMask{}}) {
    platform::RealSyscalls syscalls;
    testing::ScriptedThreads threads;
    const RegisterFpControl fp;
    const std::vector<ThreadPlacement> placements = {some_permitted(1), refused_mask};
    const auto made = make_thread_pool(placements, threads, syscalls, fp);
    ASSERT_FALSE(made.has_value());
    const auto* refused = std::get_if<ThreadNotPlaced>(&made.error());
    ASSERT_NE(refused, nullptr) << describe(made.error());
    EXPECT_EQ(refused->thread, 1U);
    EXPECT_EQ(refused->refused.kind, platform::Placement::kNoUsableCpu);
    EXPECT_EQ(refused->refused.cause.number, EINVAL);
    EXPECT_EQ(refused->refused.requested, refused_mask);
    EXPECT_EQ(threads.started(), 2U);
    EXPECT_EQ(threads.joined(), 2U);
  }
}

// ---- the floating-point state a thread inherits -------------------------------

#if defined(__x86_64__)
constexpr fpenv::FpState kUnusual{fpenv::Rounding::kUpward, true, true, std::nullopt};
#else
constexpr fpenv::FpState kUnusual{fpenv::Rounding::kUpward, true, std::nullopt, true};
#endif

TEST(RealPool, EveryThreadInheritsTheCreatorsRegistersAndThePoolAgrees) {
  const RestoreRegisters restore;
  RegisterFpControl fp;
  // Every property away from its default, so inheriting the default would
  // not pass for inheriting the creator's.
  fp.write(kUnusual);
  ASSERT_EQ(fp.read(), kUnusual) << "the registers took what was written";

  platform::RealSyscalls syscalls;
  PosixThreads threads;
  const auto made = make_thread_pool(std::vector<ThreadPlacement>(4), threads, syscalls, fp);
  ASSERT_TRUE(made.has_value()) << describe(made.error());

  // The pool's own check passed; this reads the registers again, from inside
  // each task, through the same control and through <cfenv> directly.
  std::vector<fpenv::FpState> seen(64);
  std::vector<int> mode(64, -1);
  auto read = [&](std::size_t index, std::size_t) noexcept {
    const RegisterFpControl own;
    seen[index] = own.read();
    mode[index] = std::fegetround();
  };
  for_each_index(*made.value(), seen.size(), read);
  for (std::size_t index = 0; index < seen.size(); ++index) {
    EXPECT_EQ(seen[index], kUnusual) << index;
    EXPECT_EQ(mode[index], FE_UPWARD) << index;
  }
}

// The counterpart, with nothing modelled: a pool made in the required state
// runs its tasks in the required state.
TEST(RealPool, APoolMadeInTheRequiredStateRunsInIt) {
  const RestoreRegisters restore;
  RegisterFpControl fp;
  const fpenv::FpState required = fpenv::required_state(RegisterFpControl::kHasDenormalsAreZero,
                                                        RegisterFpControl::kHasDefaultNan);
  fp.write(required);
  platform::RealSyscalls syscalls;
  PosixThreads threads;
  const auto made = make_thread_pool(std::vector<ThreadPlacement>(3), threads, syscalls, fp);
  ASSERT_TRUE(made.has_value()) << describe(made.error());
  std::vector<fpenv::FpState> seen(30);
  auto read = [&](std::size_t index, std::size_t) noexcept {
    const RegisterFpControl own;
    seen[index] = own.read();
  };
  for_each_index(*made.value(), seen.size(), read);
  for (std::size_t index = 0; index < seen.size(); ++index) {
    EXPECT_EQ(seen[index], required) << index;
  }
}

// ---- the contract, pinned -----------------------------------------------------

struct PinnedPool {
  static std::unique_ptr<Executor> make() {
    static platform::RealSyscalls syscalls;
    static PosixThreads threads;
    static const RegisterFpControl fp;
    return make_thread_pool(one_thread_per_cpu(some_permitted(8)), threads, syscalls, fp).value();
  }
};

struct Registers {
  static constexpr bool kHasDenormalsAreZero = RegisterFpControl::kHasDenormalsAreZero;
  static constexpr bool kHasDefaultNan = RegisterFpControl::kHasDefaultNan;
  static std::unique_ptr<fpenv::FpControl> make() { return std::make_unique<RegisterFpControl>(); }
};

}  // namespace
}  // namespace loadforge::worker

// In the suites' own namespace: the instantiations name them unqualified.
namespace loadforge::contracts {
namespace {

// Every thread pinned to a CPU of its own, on the real kernel.
INSTANTIATE_TYPED_TEST_SUITE_P(RealPinnedPool, ExecutorContract, worker::PinnedPool);

// The real registers through the suite the executor's model passes too: the
// model's reading of FpControl checked against the thing it models (F21).
INSTANTIATE_TYPED_TEST_SUITE_P(RealRegisters, FpControlContract, worker::Registers);

}  // namespace

}  // namespace loadforge::contracts
