// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/stop_signals.hpp"

#include <array>
#include <csignal>
#include <cstddef>

namespace loadforge::controller {
namespace {

/// How many bytes one look takes at most; more are taken by the next.
constexpr std::size_t kLookBytes = 16;

}  // namespace

std::string_view signal_name(int signal) noexcept { return signal == SIGINT ? "SIGINT" : "SIGTERM"; }

core::Result<core::Ok, platform::SyscallError> StopSignals::install() {
  auto made = syscalls_->create_pipe();
  if (!made.has_value()) {
    return made.error();
  }
  read_fd_ = made.value().read_fd;
  write_fd_ = made.value().write_fd;
  for (const int signal : {SIGINT, SIGTERM}) {
    if (auto diverted = syscalls_->divert_signal(signal, write_fd_); !diverted.has_value()) {
      release();
      return diverted.error();
    }
  }
  return core::Ok{};
}

core::Result<std::size_t, platform::SyscallError> StopSignals::absorb() {
  auto ready = syscalls_->poll_readable({read_fd_}, 0);
  if (!ready.has_value()) {
    return ready.error();
  }
  if (!ready.value().front().readable) {
    return std::size_t{0};
  }
  std::array<char, kLookBytes> bytes{};
  auto taken = syscalls_->read(read_fd_, bytes.data(), bytes.size());
  if (!taken.has_value()) {
    return taken.error();
  }
  for (std::size_t at = 0; at < taken.value(); ++at) {
    received_.push_back(static_cast<unsigned char>(bytes[at]));
  }
  return taken.value();
}

std::optional<int> StopSignals::first() const noexcept {
  if (received_.empty()) {
    return std::nullopt;
  }
  return received_.front();
}

void StopSignals::release() noexcept {
  if (read_fd_ < 0) {
    return;
  }
  // Both signals first, so that no handler runs once the descriptors are closed.
  // Whatever the calls answer, there is nothing more to do about it here.
  (void)syscalls_->default_signal(SIGINT);
  (void)syscalls_->default_signal(SIGTERM);
  (void)syscalls_->close(read_fd_);
  (void)syscalls_->close(write_fd_);
  read_fd_ = -1;
  write_fd_ = -1;
}

}  // namespace loadforge::controller
