// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/journal/journal.hpp"

#include "controller/journal/format.hpp"

namespace loadforge::journal {
namespace {

class FormatWriter final : public Writer {
 public:
  explicit FormatWriter(Storage& storage) : storage_(&storage) {}

  core::Result<core::Ok, JournalError> append(const Entry& entry) override {
    if (closed_) {
      return JournalError{JournalFault::kClosed, "append after close"};
    }
    if (entry.phase.size() > kMaxLabelBytes || entry.workload.size() > kMaxLabelBytes) {
      return JournalError{JournalFault::kTooLarge, "a label is longer than kMaxLabelBytes"};
    }
    return storage_->append(format::frame(format::encode_entry(entry)));
  }

  core::Result<core::Ok, JournalError> sync() override {
    if (closed_) {
      return JournalError{JournalFault::kClosed, "sync after close"};
    }
    return storage_->sync();
  }

  // The terminator is written first and the writer is closed once it is in
  // the storage, so a sync that fails afterwards is reported but does not
  // reopen the journal: nothing may follow a terminator.
  core::Result<core::Ok, JournalError> close() override {
    if (closed_) {
      return JournalError{JournalFault::kClosed, "close after close"};
    }
    auto appended = storage_->append(format::frame(format::terminator_payload()));
    if (!appended.has_value()) {
      return appended;
    }
    closed_ = true;
    return storage_->sync();
  }

 private:
  Storage* storage_;
  bool closed_ = false;
};

}  // namespace

core::Result<std::unique_ptr<Writer>, JournalError> open_writer(Storage& storage) {
  auto held = storage.read_all();
  if (!held.has_value()) {
    return held.error();
  }
  if (!held.value().empty()) {
    return JournalError{JournalFault::kNotEmpty, "the storage already holds a journal"};
  }
  return std::unique_ptr<Writer>(std::make_unique<FormatWriter>(storage));
}

core::Result<Replay, JournalError> replay(const Storage& storage) {
  auto held = storage.read_all();
  if (!held.has_value()) {
    return held.error();
  }
  return format::scan(held.value());
}

}  // namespace loadforge::journal
