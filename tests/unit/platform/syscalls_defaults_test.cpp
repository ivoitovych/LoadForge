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
  core::Result<int, SyscallError> open_read(const std::string& /*path*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<std::size_t, SyscallError> read(int /*fd*/, char* /*buffer*/,
                                               std::size_t /*size*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> close(int /*fd*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<std::size_t, SyscallError> read_link(const std::string& /*path*/, char* /*buffer*/,
                                                    std::size_t /*size*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<pid_t, SyscallError> fork_process() override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  SyscallError exec(const std::string& /*path*/,
                    const std::vector<std::string>& /*argv*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> set_parent_death_signal(int /*signal*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  pid_t parent_pid() override { return 0; }
  void start_session() override {}
  core::Result<Reaped, SyscallError> wait_any() override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> send_signal(pid_t /*pid*/, int /*signal*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<TimeSpec, SyscallError> read_clock(int /*clock_id*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<CpuMask, SyscallError> get_affinity(pid_t /*pid*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> set_affinity(pid_t /*pid*/,
                                                    const CpuMask& /*mask*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<void*, SyscallError> map_anonymous(std::size_t /*length*/,
                                                  int /*extra_flags*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> unmap(void* /*address*/, std::size_t /*length*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> lock_memory(void* /*address*/,
                                                   std::size_t /*length*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> advise_memory(void* /*address*/, std::size_t /*length*/,
                                                     int /*advice*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> bind_memory(void* /*address*/, std::size_t /*length*/,
                                                   const NodeMask& /*nodes*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<int, SyscallError> create_memory_file(const std::string& /*name*/,
                                                     std::size_t /*length*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<void*, SyscallError> map_shared(int /*fd*/, std::size_t /*length*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> inherit_across_exec(int /*fd*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<PipeEnds, SyscallError> create_pipe() override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<std::size_t, SyscallError> write(int /*fd*/,
                                                std::span<const std::byte> /*bytes*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<std::vector<Readiness>, SyscallError> poll_readable(const std::vector<int>& /*fds*/,
                                                                   int /*timeout_ms*/) override {
    return SyscallError{ENOSYS, "stub", ""};
  }
  core::Result<core::Ok, SyscallError> ignore_signal(int /*signal*/) override {
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
