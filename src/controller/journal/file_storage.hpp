// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_JOURNAL_FILE_STORAGE_HPP
#define LOADFORGE_CONTROLLER_JOURNAL_FILE_STORAGE_HPP

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "controller/journal/file_ops.hpp"
#include "controller/journal/journal.hpp"

namespace loadforge::journal {

/// A journal's Storage over a real file.
///
/// open() creates the file if it is missing (and does not truncate one that
/// is there: open_writer then refuses it as kNotEmpty), opens it O_APPEND, and
/// fsyncs the directory that holds it, because a file's name is not durable
/// until its directory entry is -- a journal whose bytes survived a power cut
/// but whose name did not is no journal.
///
/// append() writes until every byte is in, retrying EINTR and continuing after
/// a short write; a write that makes no progress, or fails, is kStorage and may
/// have left a prefix of the bytes (the contract allows it; replay reads the
/// torn record as a torn tail, or as corruption if more is written after it).
/// sync() is fsync, retried only on EINTR: after EIO a second fsync may report
/// success for pages the kernel has already dropped, so a failed sync is
/// reported and never retried here.
///
/// read_all() opens the file afresh and reads it to its end, so it answers
/// what is on disk, not what this object wrote.
///
/// Thread-safety: one thread at a time. The destructor closes the descriptor
/// and cannot report a failure; every byte was synced or not by then.
class FileStorage final : public Storage {
 public:
  /// Consecutive EINTRs one call tolerates before it is reported (the bound
  /// platform::FileSystem uses, for the same reason).
  static constexpr int kMaxConsecutiveInterrupts = 128;
  static constexpr std::size_t kReadChunkBytes = 4096;

  [[nodiscard]] static core::Result<std::unique_ptr<FileStorage>, JournalError> open(
      FileOps& ops, std::string path);

  /// Takes ownership of `fd`, an append descriptor on `path`. Use open().
  FileStorage(FileOps& ops, std::string path, int fd);
  FileStorage(const FileStorage&) = delete;
  FileStorage(FileStorage&&) = delete;
  FileStorage& operator=(const FileStorage&) = delete;
  FileStorage& operator=(FileStorage&&) = delete;
  ~FileStorage() override;

  [[nodiscard]] core::Result<core::Ok, JournalError> append(
      std::span<const std::byte> bytes) override;
  [[nodiscard]] core::Result<core::Ok, JournalError> sync() override;
  [[nodiscard]] core::Result<std::vector<std::byte>, JournalError> read_all() const override;

 private:
  FileOps* ops_;
  std::string path_;
  int fd_;
};

}  // namespace loadforge::journal

#endif
