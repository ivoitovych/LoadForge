// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_WORKER_EXECUTOR_FAKES_HPP
#define LOADFORGE_TESTS_UNIT_WORKER_EXECUTOR_FAKES_HPP

// The executor's own stand-ins (ADR 0003): a Threads that refuses a chosen
// start, and an FpControl read as this stream reads the fpenv contract. Shared
// by this module's tests in every tier, and by nothing else.

#include <pthread.h>

#include <cstddef>
#include <mutex>
#include <optional>

#include "platform/fpenv/fpenv.hpp"
#include "worker/executor/thread_pool.hpp"

namespace loadforge::worker::testing {

/// Real threads, except that start number `fail_at` (counting from zero)
/// returns `error` and starts nothing -- pthread_create's documented shape of
/// a failure. Counts the starts made and the joins asked for, so a test can
/// assert that every thread started was joined, and no other.
///
/// Used from the thread that makes and destroys the pool only, as the pool
/// itself uses it.
class ScriptedThreads final : public Threads {
 public:
  ScriptedThreads() = default;
  ScriptedThreads(std::size_t fail_at, int error) : fail_at_(fail_at), error_(error) {}

  [[nodiscard]] int start(pthread_t& handle, Entry entry, void* argument) override {
    const std::size_t attempt = attempts_++;
    if (fail_at_.has_value() && attempt == *fail_at_) {
      return error_;
    }
    const int number = real_.start(handle, entry, argument);
    if (number == 0) {
      ++started_;
    }
    return number;
  }

  void join(pthread_t handle) override {
    ++joined_;
    real_.join(handle);
  }

  [[nodiscard]] std::size_t attempts() const { return attempts_; }
  [[nodiscard]] std::size_t started() const { return started_; }
  [[nodiscard]] std::size_t joined() const { return joined_; }

 private:
  PosixThreads real_;
  std::optional<std::size_t> fail_at_;
  int error_ = 0;
  std::size_t attempts_ = 0;
  std::size_t started_ = 0;
  std::size_t joined_ = 0;
};

/// The floating-point registers, held in memory, shaped like one architecture
/// or the other. This is how this stream reads FpControl: write() sets what
/// the state names and leaves an absent property alone; read() reports exactly
/// the flags the architecture has.
///
/// A model of one thing more: the registers a thread started after something
/// changed them would read. Every thread here reads one register file, so a
/// state written once is the state every thread inherits -- the property
/// fpenv.hpp says threads have, and the integration tier confirms against the
/// real registers. dirty_from(n, state) makes read number n onwards (counting
/// from zero, across all threads) report `state` instead: the pool reads once
/// on the creating thread and once on each thread it starts, in order, so
/// dirty_from(k + 1, ...) is "thread k found its registers changed".
template <bool kDaz, bool kDn>
class ModelFpControl final : public fpenv::FpControl {
 public:
  static constexpr bool kHasDenormalsAreZero = kDaz;
  static constexpr bool kHasDefaultNan = kDn;

  ModelFpControl() : state_(fpenv::required_state(kDaz, kDn)) {}

  [[nodiscard]] fpenv::FpState read() const override {
    const std::lock_guard lock(mutex_);
    const std::size_t number = reads_++;
    if (dirty_.has_value() && number >= dirty_from_) {
      return *dirty_;
    }
    return state_;
  }

  void write(const fpenv::FpState& state) override {
    const std::lock_guard lock(mutex_);
    state_.rounding = state.rounding;
    state_.flush_to_zero = state.flush_to_zero;
    if (kDaz && state.denormals_are_zero.has_value()) {
      state_.denormals_are_zero = state.denormals_are_zero;
    }
    if (kDn && state.default_nan.has_value()) {
      state_.default_nan = state.default_nan;
    }
  }

  void dirty_from(std::size_t read_number, const fpenv::FpState& state) {
    const std::lock_guard lock(mutex_);
    dirty_from_ = read_number;
    dirty_ = state;
  }

  [[nodiscard]] std::size_t reads() const {
    const std::lock_guard lock(mutex_);
    return reads_;
  }

 private:
  mutable std::mutex mutex_;
  fpenv::FpState state_;
  mutable std::size_t reads_ = 0;
  std::size_t dirty_from_ = 0;
  std::optional<fpenv::FpState> dirty_;
};

using X86FpControl = ModelFpControl<true, false>;
using ArmFpControl = ModelFpControl<false, true>;

}  // namespace loadforge::worker::testing

#endif
