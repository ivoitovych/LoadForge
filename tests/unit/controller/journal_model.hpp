// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_CONTROLLER_JOURNAL_MODEL_HPP
#define LOADFORGE_TESTS_UNIT_CONTROLLER_JOURNAL_MODEL_HPP

// The controller's own reading of controller/journal/journal.hpp (ADR 0003),
// written from the header alone: a record is a length, a checksum over the
// length and the payload, and the payload; an entry is a payload of its fields,
// the terminator a payload of its own. It is not the journal stream's format,
// and does not need to be: it needs to keep every promise the header makes, and
// the provider's suite is run against it (journal_model_test.cpp), so a promise
// this reading does not keep is a failing test.
//
// Two things are here. ModelJournal is the pair of functions the suite takes
// (open_writer and replay). RecordingWriter is what the controller's tests give
// a run as its journal: a Writer that keeps the entries it is given, and can be
// made to refuse the append or the sync.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "controller/journal/journal.hpp"
#include "core/crc32.hpp"
#include "core/result.hpp"

namespace loadforge::controller_testing {

class ModelJournal {
 public:
  static constexpr std::uint32_t kMaxPayload = 1024;

  static core::Result<std::unique_ptr<journal::Writer>, journal::JournalError> open_writer(
      journal::Storage& storage) {
    auto held = storage.read_all();
    if (!held.has_value()) {
      return held.error();
    }
    if (!held.value().empty()) {
      return journal::JournalError{journal::JournalFault::kNotEmpty, "the storage holds a journal"};
    }
    std::unique_ptr<journal::Writer> writer = std::make_unique<Writer>(storage);
    return writer;
  }

  static core::Result<journal::Replay, journal::JournalError> replay(
      const journal::Storage& storage) {
    auto held = storage.read_all();
    if (!held.has_value()) {
      return held.error();
    }
    const std::vector<std::byte>& bytes = held.value();
    journal::Replay out;
    std::size_t at = 0;
    while (at < bytes.size()) {
      if (bytes.size() - at < 8) {
        out.ending = journal::Ending::kTornTail;
        return out;
      }
      const std::uint32_t length = get32(bytes, at);
      if (length == 0 || length > kMaxPayload) {
        out.ending = journal::Ending::kCorrupt;
        return out;
      }
      if (bytes.size() - at - 8 < length) {
        out.ending = journal::Ending::kTornTail;
        return out;
      }
      std::vector<std::byte> checked(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(at + 4));
      checked.insert(checked.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at + 8),
                     bytes.begin() + static_cast<std::ptrdiff_t>(at + 8 + length));
      if (core::crc32(checked) != get32(bytes, at + 4)) {
        out.ending = journal::Ending::kCorrupt;
        return out;
      }
      const std::span<const std::byte> payload{bytes.data() + at + 8, length};
      at += 8 + length;
      if (payload[0] == std::byte{'T'}) {
        out.ending = at == bytes.size() ? journal::Ending::kTerminated : journal::Ending::kCorrupt;
        return out;
      }
      auto entry = unpack(payload.subspan(1));
      if (payload[0] != std::byte{'E'} || !entry.has_value()) {
        out.ending = journal::Ending::kCorrupt;
        return out;
      }
      out.entries.push_back(std::move(*entry));
    }
    out.ending = out.entries.empty() ? journal::Ending::kEmpty : journal::Ending::kUnterminated;
    return out;
  }

 private:
  class Writer final : public journal::Writer {
   public:
    explicit Writer(journal::Storage& storage) : storage_(&storage) {}

    core::Result<core::Ok, journal::JournalError> append(const journal::Entry& entry) override {
      if (closed_) {
        return closed();
      }
      if (entry.phase.size() > journal::kMaxLabelBytes ||
          entry.workload.size() > journal::kMaxLabelBytes) {
        return journal::JournalError{journal::JournalFault::kTooLarge, "a label"};
      }
      return storage_->append(record('E', pack(entry)));
    }
    core::Result<core::Ok, journal::JournalError> sync() override {
      if (closed_) {
        return closed();
      }
      return storage_->sync();
    }
    core::Result<core::Ok, journal::JournalError> close() override {
      if (closed_) {
        return closed();
      }
      closed_ = true;
      if (auto appended = storage_->append(record('T', {})); !appended.has_value()) {
        return appended;
      }
      return storage_->sync();
    }

   private:
    static journal::JournalError closed() {
      return journal::JournalError{journal::JournalFault::kClosed, "closed"};
    }
    journal::Storage* storage_;
    bool closed_ = false;
  };

  static void put32(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      out.push_back(static_cast<std::byte>(value >> shift));
    }
  }
  static void put64(std::vector<std::byte>& out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      out.push_back(static_cast<std::byte>(value >> shift));
    }
  }
  static std::uint32_t get32(const std::vector<std::byte>& bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (unsigned byte = 0; byte < 4; ++byte) {
      value |= std::to_integer<std::uint32_t>(bytes[at + byte]) << (8 * byte);
    }
    return value;
  }

  static std::vector<std::byte> record(char kind, const std::vector<std::byte>& body) {
    std::vector<std::byte> payload{static_cast<std::byte>(kind)};
    payload.insert(payload.end(), body.begin(), body.end());
    std::vector<std::byte> framed;
    put32(framed, static_cast<std::uint32_t>(payload.size()));
    std::vector<std::byte> checked = framed;
    checked.insert(checked.end(), payload.begin(), payload.end());
    put32(framed, core::crc32(checked));
    framed.insert(framed.end(), payload.begin(), payload.end());
    return framed;
  }

  static void put_label(std::vector<std::byte>& out, const std::string& label) {
    out.push_back(static_cast<std::byte>(label.size()));
    for (const char c : label) {
      out.push_back(static_cast<std::byte>(c));
    }
  }

  static std::vector<std::byte> pack(const journal::Entry& entry) {
    std::vector<std::byte> out;
    put64(out, static_cast<std::uint64_t>(entry.elapsed_ns));
    const std::optional<std::uint64_t> fields[] = {
        entry.last_verified_unit,
        entry.temperature_c ? std::optional{std::bit_cast<std::uint64_t>(*entry.temperature_c)}
                            : std::nullopt,
        entry.package_power_w ? std::optional{std::bit_cast<std::uint64_t>(*entry.package_power_w)}
                              : std::nullopt,
        entry.frequency_hz ? std::optional{std::bit_cast<std::uint64_t>(*entry.frequency_hz)}
                           : std::nullopt,
        entry.ecc_corrected,
        entry.ecc_uncorrected};
    std::uint8_t present = 0;
    for (std::size_t at = 0; at < std::size(fields); ++at) {
      present = static_cast<std::uint8_t>(present | (fields[at].has_value() ? 1U << at : 0U));
    }
    out.push_back(static_cast<std::byte>(present));
    put_label(out, entry.phase);
    put_label(out, entry.workload);
    put32(out, entry.workers_alive);
    put32(out, entry.workers_stale);
    for (const auto& field : fields) {
      if (field.has_value()) {
        put64(out, *field);
      }
    }
    return out;
  }

  static std::optional<journal::Entry> unpack(std::span<const std::byte> bytes) {
    std::size_t at = 0;
    const auto take = [&](std::size_t count) -> std::optional<std::span<const std::byte>> {
      if (bytes.size() - at < count) {
        return std::nullopt;
      }
      const auto part = bytes.subspan(at, count);
      at += count;
      return part;
    };
    const auto number = [](std::span<const std::byte> part) {
      std::uint64_t value = 0;
      for (std::size_t byte = 0; byte < part.size(); ++byte) {
        value |= std::to_integer<std::uint64_t>(part[byte]) << (8 * byte);
      }
      return value;
    };
    journal::Entry entry;
    const auto elapsed = take(8);
    const auto present = take(1);
    if (!elapsed || !present) {
      return std::nullopt;
    }
    entry.elapsed_ns = static_cast<std::int64_t>(number(*elapsed));
    for (std::string* label : {&entry.phase, &entry.workload}) {
      const auto size = take(1);
      const auto text = size ? take(std::to_integer<std::size_t>((*size)[0])) : std::nullopt;
      if (!text) {
        return std::nullopt;
      }
      for (const std::byte c : *text) {
        label->push_back(static_cast<char>(std::to_integer<unsigned char>(c)));
      }
    }
    const auto alive = take(4);
    const auto stale = take(4);
    if (!alive || !stale) {
      return std::nullopt;
    }
    entry.workers_alive = static_cast<std::uint32_t>(number(*alive));
    entry.workers_stale = static_cast<std::uint32_t>(number(*stale));
    const std::uint8_t mask = std::to_integer<std::uint8_t>((*present)[0]);
    std::array<std::optional<std::uint64_t>, 6> fields;
    for (std::size_t field = 0; field < 6; ++field) {
      if ((mask >> field & 1U) != 0U) {
        const auto value = take(8);
        if (!value) {
          return std::nullopt;
        }
        fields[field] = number(*value);
      }
    }
    entry.last_verified_unit = fields[0];
    if (fields[1]) {
      entry.temperature_c = std::bit_cast<double>(*fields[1]);
    }
    if (fields[2]) {
      entry.package_power_w = std::bit_cast<double>(*fields[2]);
    }
    if (fields[3]) {
      entry.frequency_hz = std::bit_cast<double>(*fields[3]);
    }
    entry.ecc_corrected = fields[4];
    entry.ecc_uncorrected = fields[5];
    return entry;
  }
};

/// What a run is given as its journal in the controller's tests: the entries
/// appended, in order, and an append or a sync that can be made to fail.
class RecordingWriter final : public journal::Writer {
 public:
  core::Result<core::Ok, journal::JournalError> append(const journal::Entry& entry) override {
    ++appends;
    if (fail_append_at != 0 && appends >= fail_append_at) {
      return journal::JournalError{journal::JournalFault::kStorage, "ENOSPC"};
    }
    entries.push_back(entry);
    return core::Ok{};
  }
  core::Result<core::Ok, journal::JournalError> sync() override {
    ++syncs;
    if (fail_sync_at != 0 && syncs >= fail_sync_at) {
      return journal::JournalError{journal::JournalFault::kStorage, "EIO"};
    }
    return core::Ok{};
  }
  core::Result<core::Ok, journal::JournalError> close() override {
    closed = true;
    return core::Ok{};
  }

  std::vector<journal::Entry> entries;
  int appends = 0;
  int syncs = 0;
  int fail_append_at = 0;  ///< The append with this number (from one), and every later one, fails.
  int fail_sync_at = 0;
  bool closed = false;
};

}  // namespace loadforge::controller_testing

#endif
