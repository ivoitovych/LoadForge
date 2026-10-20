// SPDX-License-Identifier: GPL-3.0-or-later
//
// The seam's defaults. A call added after M1 has an ENOSYS default so that
// adding it breaks no implementation. This is the smallest Syscalls that
// compiles: every call that is still pure is stubbed, and the perf-event calls
// are left alone, so what is asserted is the base class's own answer.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "platform/syscalls.hpp"

namespace loadforge::platform {
namespace {

class MinimalSyscalls final : public Syscalls {
 public:
  core::Result<int, SyscallError> open_read(const std::string&) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<std::size_t, SyscallError> read(int, char*, std::size_t) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> close(int) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<std::size_t, SyscallError> read_link(const std::string&, char*,
                                                    std::size_t) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<pid_t, SyscallError> fork_process() override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  SyscallError exec(const std::string&, const std::vector<std::string>&) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> set_parent_death_signal(int) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  pid_t parent_pid() override { return 0; }
  void start_session() override {}
  core::Result<Reaped, SyscallError> wait_any() override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> send_signal(pid_t, int) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<TimeSpec, SyscallError> read_clock(int) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<CpuMask, SyscallError> get_affinity(pid_t) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> set_affinity(pid_t, const CpuMask&) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<void*, SyscallError> map_anonymous(std::size_t, int) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> unmap(void*, std::size_t) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> lock_memory(void*, std::size_t) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> advise_memory(void*, std::size_t, int) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> bind_memory(void*, std::size_t, const NodeMask&) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<int, SyscallError> create_memory_file(const std::string&, std::size_t) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<void*, SyscallError> map_shared(int, std::size_t) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> inherit_across_exec(int) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<PipeEnds, SyscallError> create_pipe() override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<std::size_t, SyscallError> write(int, std::span<const std::byte>) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<std::vector<Readiness>, SyscallError> poll_readable(const std::vector<int>&,
                                                                   int) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> ignore_signal(int) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  pid_t process_id() override { return 0; }
  core::Result<std::optional<Reaped>, SyscallError> reap_any_nonblocking() override {
    return SyscallError{ENOSYS, "stub", ""};
  }
};

TEST(SyscallsDefaults, OpeningACounterIsNotImplemented) {
  MinimalSyscalls calls;

  const auto result = calls.open_perf_event(PerfEventSpec{});

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().number, ENOSYS);
  EXPECT_EQ(result.error().call, "perf_event_open");
}

TEST(SyscallsDefaults, ControllingACounterIsNotImplemented) {
  MinimalSyscalls calls;

  const auto result = calls.control_perf_event(3, PerfAction::kEnable);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().number, ENOSYS);
  EXPECT_EQ(result.error().call, "ioctl");
}

TEST(SyscallsDefaults, ReadingACounterIsNotImplemented) {
  MinimalSyscalls calls;

  const auto result = calls.read_perf_count(3);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().number, ENOSYS);
  EXPECT_EQ(result.error().call, "read");
}

}  // namespace
}  // namespace loadforge::platform
