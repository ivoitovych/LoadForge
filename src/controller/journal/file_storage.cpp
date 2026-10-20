// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/journal/file_storage.hpp"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <optional>
#include <utility>

namespace loadforge::journal {
namespace {

JournalError storage_fault(const platform::SyscallError& error) {
  return JournalError{JournalFault::kStorage, platform::describe(error)};
}

/// `call` until it is not interrupted, at most kMaxConsecutiveInterrupts
/// times over; the last answer is the answer.
template <typename Call>
auto retry_interrupted(Call call) {
  auto result = call();
  for (int again = 0; again < FileStorage::kMaxConsecutiveInterrupts && !result.has_value() &&
                      result.error().number == EINTR;
       ++again) {
    result = call();
  }
  return result;
}

core::Result<core::Ok, platform::SyscallError> sync_directory(FileOps& ops,
                                                              const std::string& path) {
  std::filesystem::path directory = std::filesystem::path(path).parent_path();
  if (directory.empty()) {
    directory = ".";
  }
  auto opened = ops.open(directory.string(), FileOps::Mode::kDirectory);
  if (!opened.has_value()) {
    return opened.error();
  }
  const int fd = opened.value();
  auto synced = retry_interrupted([&] { return ops.fsync(fd); });
  auto closed = ops.close(fd);
  if (!synced.has_value()) {
    return synced;
  }
  return closed;
}

}  // namespace

core::Result<std::unique_ptr<FileStorage>, JournalError> FileStorage::open(FileOps& ops,
                                                                           std::string path) {
  auto opened = ops.open(path, FileOps::Mode::kAppend);
  if (!opened.has_value()) {
    return storage_fault(opened.error());
  }
  const int fd = opened.value();
  auto durable = sync_directory(ops, path);
  if (!durable.has_value()) {
    static_cast<void>(ops.close(fd));
    return storage_fault(durable.error());
  }
  return std::make_unique<FileStorage>(ops, std::move(path), fd);
}

FileStorage::FileStorage(FileOps& ops, std::string path, int fd)
    : ops_(&ops), path_(std::move(path)), fd_(fd) {}

FileStorage::~FileStorage() { static_cast<void>(ops_->close(fd_)); }

core::Result<core::Ok, JournalError> FileStorage::append(std::span<const std::byte> bytes) {
  while (!bytes.empty()) {
    const auto written = retry_interrupted([&] { return ops_->write(fd_, bytes); });
    if (!written.has_value()) {
      return storage_fault(written.error());
    }
    if (written.value() == 0) {
      return JournalError{JournalFault::kStorage, "write made no progress"};
    }
    bytes = bytes.subspan(std::min(written.value(), bytes.size()));
  }
  return core::Ok{};
}

core::Result<core::Ok, JournalError> FileStorage::sync() {
  const auto synced = retry_interrupted([&] { return ops_->fsync(fd_); });
  if (!synced.has_value()) {
    return storage_fault(synced.error());
  }
  return core::Ok{};
}

core::Result<std::vector<std::byte>, JournalError> FileStorage::read_all() const {
  auto opened = ops_->open(path_, FileOps::Mode::kRead);
  if (!opened.has_value()) {
    return storage_fault(opened.error());
  }
  const int fd = opened.value();
  std::vector<std::byte> whole;
  std::vector<std::byte> chunk(kReadChunkBytes);
  std::optional<platform::SyscallError> failure;
  while (!failure.has_value()) {
    const auto got = retry_interrupted([&] { return ops_->read(fd, chunk); });
    if (!got.has_value()) {
      failure = got.error();
    } else if (got.value() == 0) {
      break;
    } else {
      whole.insert(whole.end(), chunk.begin(),
                   chunk.begin() + static_cast<std::ptrdiff_t>(got.value()));
    }
  }
  const auto closed = ops_->close(fd);
  if (failure.has_value()) {
    return storage_fault(*failure);
  }
  if (!closed.has_value()) {
    return storage_fault(closed.error());
  }
  return whole;
}

}  // namespace loadforge::journal
