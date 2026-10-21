// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKER_EXECUTOR_THREAD_POOL_HPP
#define LOADFORGE_WORKER_EXECUTOR_THREAD_POOL_HPP

// The provider of worker/executor.hpp: a fixed pool of threads per worker,
// created once, each placed where the worker was placed, each asserting on
// start that it inherited the floating-point state of the thread that made it.
//
// Everything that can fail fails here, in the factory, before the pool exists:
// a thread the system will not start, a CPU the kernel will not put it on, a
// floating-point state that is not the creator's. Once make_thread_pool has
// returned a pool, run() has no failure to report, which is what the contract
// promises (executor.hpp: "There is no error result").

#include <pthread.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/result.hpp"
#include "platform/affinity.hpp"
#include "platform/fpenv/fpenv.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "worker/executor.hpp"

namespace loadforge::worker {

/// Where one of the pool's threads runs: pinned to a mask, or -- absent -- left
/// on whatever the creating thread may run on, which a new thread inherits.
using ThreadPlacement = std::optional<platform::CpuMask>;

/// One placement per CPU the mask names, each naming that CPU alone, in
/// ascending order: the pool a worker placed on `cpus` runs, one thread per
/// CPU. An empty mask gives no placements, which make_thread_pool refuses.
[[nodiscard]] std::vector<ThreadPlacement> one_thread_per_cpu(const platform::CpuMask& cpus);

/// The seam threads are started and joined through: the pool's own, because
/// platform::Syscalls carries no thread calls and is not this stream's to grow.
///
/// It is pthread_create(3) and pthread_join(3) and nothing else, for the reason
/// the system-call seam is narrow (docs/PLAN.md §4.3): every decision about
/// what a failure means lives above it, in make_thread_pool, where a fake can
/// force each one. std::thread would report a failed start by throwing, and a
/// catch arm below the seam is a branch no test on a root CI runner can reach.
class Threads {
 public:
  using Entry = void* (*)(void* argument);

  Threads() = default;
  Threads(const Threads&) = delete;
  Threads(Threads&&) = delete;
  Threads& operator=(const Threads&) = delete;
  Threads& operator=(Threads&&) = delete;
  virtual ~Threads() = default;

  /// pthread_create(3) with default attributes: starts entry(argument) and
  /// stores the thread's handle. Returns 0, or the error number pthread_create
  /// returned -- it reports failure by its result, never through errno. On
  /// failure no thread was started and `handle` means nothing.
  [[nodiscard]] virtual int start(pthread_t& handle, Entry entry, void* argument) = 0;

  /// pthread_join(3), its result dropped by contract. The call fails only for
  /// a caller's mistake -- a handle never started, one already joined, a thread
  /// joining itself (EINVAL, ESRCH, EDEADLK) -- and the pool joins exactly the
  /// threads it started, once each, from the thread that started them. A
  /// failure arm here would be a branch no test could take (IMPLEMENTATION
  /// §2.6, rung 3), as for Syscalls::start_session.
  virtual void join(pthread_t handle) = 0;
};

/// The real threads. A translation and nothing else.
class PosixThreads final : public Threads {
 public:
  [[nodiscard]] int start(pthread_t& handle, Entry entry, void* argument) override;
  void join(pthread_t handle) override;
};

/// No placements were given: a pool of no threads, which the contract forbids
/// (threads() is at least 1).
struct NoThreads {};

/// The system would not start thread `thread`. `cause` carries the number
/// pthread_create returned: EAGAIN (a thread limit, or memory for its stack),
/// EINVAL or EPERM (an attribute it refused; the pool passes none).
struct ThreadNotStarted {
  std::size_t thread = 0;
  platform::SyscallError cause;
};

/// Thread `thread` started, and the kernel would not put it where it was
/// placed: platform::Affinity's classification of the refusal, the mask asked
/// for, and the kernel's own words.
struct ThreadNotPlaced {
  std::size_t thread = 0;
  platform::PlacementRefused refused;
};

/// Thread `thread` started and found a floating-point state other than the one
/// the thread that made the pool holds. A new thread inherits its creator's
/// state, so this says something changed it between the two reads -- the
/// configuration error docs/determinism.md §3 reports at startup.
struct ThreadFpStateDiffers {
  std::size_t thread = 0;
  fpenv::FpState expected;
  fpenv::FpState found;
};

using PoolError = std::variant<NoThreads, ThreadNotStarted, ThreadNotPlaced, ThreadFpStateDiffers>;

/// Human-readable rendering, suitable for putting in front of a user.
[[nodiscard]] std::string describe(const PoolError& error);

/// A pool of placements.size() threads, thread s placed by placements[s], its
/// slot s for every task it runs; or the first refusal, with every thread
/// already started stopped and joined.
///
/// The threads are started one at a time: each is placed and checked on its
/// own thread -- affinity with pid 0 and the floating-point registers both
/// belong to the calling thread -- and reports before the next one starts. So
/// `syscalls` and `fp` are used by one thread at a time, and only while this
/// call runs; `threads` is used again when the pool is destroyed, and must
/// outlive it.
///
/// The expected floating-point state is the one the calling thread holds when
/// it calls this. The worker establishes the required state before it makes
/// the pool (fpenv.hpp), and checks it there; each thread asserts that it
/// inherited exactly that.
[[nodiscard]] core::Result<std::unique_ptr<Executor>, PoolError> make_thread_pool(
    const std::vector<ThreadPlacement>& placements, Threads& threads, platform::Syscalls& syscalls,
    const fpenv::FpControl& fp);

}  // namespace loadforge::worker

#endif
