// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKER_EXECUTOR_HPP
#define LOADFORGE_WORKER_EXECUTOR_HPP

// CONTRACT (ADR 0004). How work is spread over threads inside one worker
// process. Frozen: changed only by a contract change, never inside a stream.
//
//   Provider: the executor stream (src/worker/executor/), a thread pool.
//   Consumers: every workload whose unit computation is parallel, and the
//   verified runner (verification/runner.hpp), which spreads units over it.
//
// WHY A SEAM AND NOT std::thread IN EACH WORKLOAD
// -----------------------------------------------
// The concurrency is written once, by one stream, with its synchronisation
// argument and its ordering tests (P5), instead of eleven times by eleven
// workloads. And determinism (docs/determinism.md §3) falls out of the shape
// of the interface: a task is named by its index, never by the thread that
// runs it, so a workload whose results depend only on the index -- and whose
// reductions combine per-index partial results in index order -- produces the
// same bits at one thread and at seven. Which thread ran which index is not
// part of this contract and must not be part of any result.

#include <cstddef>
#include <type_traits>

namespace loadforge::worker {

/// Runs `count` tasks over a fixed set of threads.
///
/// run(count, task, context) calls task(context, index, slot) exactly once for
/// each index in [0, count), and for no other index:
///
///   * Calls may run concurrently, on up to threads() threads, in any order.
///   * `slot` is in [0, threads()), and no two calls running at the same time
///     have the same slot. It exists for per-thread scratch: a task that keeps
///     scratch in slot-indexed storage never shares it with a concurrent task.
///     Which slot a given index gets is unspecified and varies between runs.
///   * run returns only after every call has returned, and everything each
///     call wrote happens-before run returns: the caller reads the results
///     with no further synchronisation.
///   * count == 0 calls nothing and returns at once.
///   * The calls happen on the executor's own threads or on the calling
///     thread; which, is unspecified.
///
/// There is no error result. Threads are created when the executor is, by
/// the provider's factory, which reports failure there; once constructed an
/// executor cannot fail to run a task. A task must not throw (the type says
/// noexcept, so a throw terminates) and must not call run() on the executor
/// that is running it: that is a precondition, and a provider is entitled to
/// deadlock or terminate when it is broken.
///
/// threads() is constant for the executor's lifetime and at least 1.
///
/// Thread-safety: run() is called by one thread at a time. Concurrent calls
/// to run() on one executor are a precondition violation, like reentrancy.
class Executor {
 public:
  using Task = void (*)(void* context, std::size_t index, std::size_t slot) noexcept;

  Executor() = default;
  Executor(const Executor&) = delete;
  Executor(Executor&&) = delete;
  Executor& operator=(const Executor&) = delete;
  Executor& operator=(Executor&&) = delete;
  virtual ~Executor() = default;

  [[nodiscard]] virtual std::size_t threads() const noexcept = 0;
  virtual void run(std::size_t count, Task task, void* context) noexcept = 0;
};

/// run() with a callable: body(index, slot) for each index, under every rule
/// above. The callable is borrowed for the duration of the call only.
template <typename Body>
void for_each_index(Executor& executor, std::size_t count, Body& body) noexcept {
  static_assert(std::is_nothrow_invocable_v<Body&, std::size_t, std::size_t>,
                "the body is called as body(index, slot) and must be noexcept");
  executor.run(
      count,
      // The signature is the Task type's (context, index, slot), fixed by run(); the two
      // adjacent std::size_t are not a choice of this lambda, so the check cannot be
      // satisfied here and would fail every workload that instantiates the helper.
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      [](void* context, std::size_t index, std::size_t slot) noexcept {
        (*static_cast<Body*>(context))(index, slot);
      },
      &body);
}

}  // namespace loadforge::worker

#endif
