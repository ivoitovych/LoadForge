// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_MAIN_MEMORY_FILES_HPP
#define LOADFORGE_TESTS_UNIT_MAIN_MEMORY_FILES_HPP

// The command line's own double for the files a journal lives in: a small
// filesystem in memory behind both the journal's file operations and the
// housekeeping around them, with a failure that can be made at the Nth call of
// each. Not the journal stream's double (ADR 0003): this one is what the
// command line's tests need, which is files that are moved and removed.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "controller/journal/file_ops.hpp"
#include "main/journal_rig.hpp"

namespace loadforge::cli_testing {

class MemoryFiles final : public journal::FileOps, public cli::JournalFiles {
 public:
  using Fault = std::pair<int, int>;  ///< The call to fail (from one) and the errno.

  // --- the file operations ---------------------------------------------------------------

  core::Result<int, platform::SyscallError> open(const std::string& path, Mode mode) override {
    if (fails(opens_, open_fault_)) {
      return platform::SyscallError{open_fault_.second, "open", path};
    }
    if (mode == Mode::kDirectory) {
      return handout("", path);
    }
    if (mode == Mode::kRead && !files.contains(path)) {
      return platform::SyscallError{ENOENT, "open", path};
    }
    files.try_emplace(path);
    ids_.try_emplace(path, ++ids_made_);
    return handout(path, path);
  }

  core::Result<std::size_t, platform::SyscallError> write(
      int fd, std::span<const std::byte> bytes) override {
    if (fails(writes_, write_fault_)) {
      return platform::SyscallError{write_fault_.second, "write", std::to_string(fd)};
    }
    std::vector<std::byte>& file = files[open_.at(fd)];
    file.insert(file.end(), bytes.begin(), bytes.end());
    return bytes.size();
  }

  core::Result<std::size_t, platform::SyscallError> read(int fd,
                                                         std::span<std::byte> into) override {
    if (fails(reads_, read_fault_)) {
      return platform::SyscallError{read_fault_.second, "read", std::to_string(fd)};
    }
    const std::vector<std::byte>& file = files[open_.at(fd)];
    std::size_t& at = offset_[fd];
    const std::size_t count = std::min(into.size(), file.size() - std::min(at, file.size()));
    std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(at), count, into.begin());
    at += count;
    return count;
  }

  core::Result<core::Ok, platform::SyscallError> fsync(int /*fd*/) override { return core::Ok{}; }

  core::Result<core::Ok, platform::SyscallError> close(int fd) override {
    open_.erase(fd);
    offset_.erase(fd);
    return core::Ok{};
  }

  /// flock as the kernel has it: the lock belongs to the file (its id follows a rename) and is
  /// held while the descriptor that took it is open. A second descriptor on the same file
  /// finds EWOULDBLOCK. `fail_lock` forces an errno instead.
  core::Result<core::Ok, platform::SyscallError> lock(int fd) override {
    ++locks_;
    if (lock_error_ != 0 && locks_ >= lock_from_) {
      return platform::SyscallError{lock_error_, "flock", std::to_string(fd)};
    }
    const int id = id_.at(fd);
    const auto held = holder_.find(id);
    if (held != holder_.end() && held->second != fd && open_.contains(held->second)) {
      return platform::SyscallError{EWOULDBLOCK, "flock", std::to_string(fd)};
    }
    holder_[id] = fd;
    return core::Ok{};
  }

  /// Fail the Nth lock from now (from one) and every later one with this errno.
  void fail_lock(int error, int from = 1) {
    lock_error_ = error;
    lock_from_ = locks_ + from;
  }
  [[nodiscard]] int lock_calls() const { return locks_; }

  // --- the housekeeping ------------------------------------------------------------------

  bool exists(const std::string& path) override { return always_exists || files.contains(path); }

  core::Result<core::Ok, std::string> rename(const std::string& from,
                                             const std::string& to) override {
    if (rename_fails) {
      return "cannot rename " + from + " to " + to + ": scripted";
    }
    files[to] = files.at(from);
    files.erase(from);
    ids_[to] = ids_.contains(from) ? ids_.at(from) : ++ids_made_;
    ids_.erase(from);
    renamed.emplace_back(from, to);
    return core::Ok{};
  }

  core::Result<core::Ok, std::string> remove(const std::string& path) override {
    if (remove_fails) {
      return "cannot remove " + path + ": scripted";
    }
    files.erase(path);
    ids_.erase(path);
    removed.push_back(path);
    return core::Ok{};
  }

  /// Fail the Nth open from now (from one) and every later one with this errno; likewise read and
  /// write.
  void fail_open_from(int nth, int error) { open_fault_ = {opens_ + nth, error}; }
  void fail_read_from(int nth, int error) { read_fault_ = {reads_ + nth, error}; }
  void fail_write_from(int nth, int error) { write_fault_ = {writes_ + nth, error}; }

  std::map<std::string, std::vector<std::byte>> files;
  std::vector<std::pair<std::string, std::string>> renamed;
  std::vector<std::string> removed;
  bool rename_fails = false;
  bool remove_fails = false;
  bool always_exists = false;

 private:
  static bool fails(int& counter, const Fault& fault) {
    ++counter;
    return fault.first != 0 && counter >= fault.first;
  }
  int handout(const std::string& path, const std::string& /*shown*/) {
    const int fd = next_++;
    open_[fd] = path;
    id_[fd] = path.empty() ? 0 : ids_.at(path);
    return fd;
  }

  std::map<int, std::string> open_;
  std::map<int, int> id_;           ///< fd -> the file it was opened on
  std::map<std::string, int> ids_;  ///< name -> file
  std::map<int, int> holder_;       ///< file -> the fd that took its lock
  int ids_made_ = 0;
  int locks_ = 0;
  int lock_error_ = 0;
  int lock_from_ = 0;
  std::map<int, std::size_t> offset_;
  int next_ = 50;
  int opens_ = 0;
  int reads_ = 0;
  int writes_ = 0;
  Fault open_fault_{0, 0};
  Fault read_fault_{0, 0};
  Fault write_fault_{0, 0};
};

}  // namespace loadforge::cli_testing

#endif
