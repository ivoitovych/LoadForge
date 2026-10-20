# Journal fragment 16 — the crash journal

**Written:** 2026-10-20 15:57 UTC
**Change:** `src/controller/journal`: record format, writer, replay, file Storage behind a file seam.

## Established by experiment

### AddressSanitizer's quarantine makes resident memory climb

The T10 soak first compared resident memory after 10% of the appends with the end. Under
ASan the end was 5.7 MB higher (25.2 vs 19.5 MB over 4320 appends) with nothing leaked:
freed blocks sit in the allocator's quarantine. The soak now skips the resident bound
when built with ASan; LeakSanitizer is the leak check there. Descriptors and file size
are still asserted in every build.

### Coverage: a retry bound needs an exhaustion test per instantiation

`retry_interrupted` is a template over four call shapes. Exhausting the bound was tested
for write only; gcovr reported three branches (the `again < kMax` false arm) in the
other instantiations. One exhaustion test each (fsync, directory fsync, read) closed them.

### Local tooling

Clang's sanitizer runtimes (`libclang_rt`) are not installable in this container, so
ASan+UBSan ran with GCC (211 journal-related tests pass). The TSan preset fails to
build with GCC in `src/worker/ipc/heartbeat.cpp` (`atomic_thread_fence` is refused under
`-fsanitize=thread`, not mine); the journal code has no threads, so it owes TSan nothing
beyond CI's run.

## Decisions

- **Format.** Record = u32 length | u32 CRC-32 over the length bytes then the payload |
  payload. Fixed-width little-endian fields, no varints. Options: the reference's
  8-byte length and CRC (wasteful), varints (a reader that cannot be checked by counting).
  A length of 0 or above 512 is corrupt at once, so a flipped high length bit is
  corruption rather than a plausible torn tail. Pinned bytes come from Python's zlib (F21).
- **Directory fsync at open.** A created file's name is not durable until its directory
  entry is. `FileStorage::open` fsyncs the parent directory.
- **fsync failure is never retried** (only EINTR is): a second fsync after EIO can succeed
  for pages already dropped.
- **A failed append is not poisoned.** The storage may hold a prefix of the record;
  later appends follow it and replay reports kCorrupt with the entries before it. Poisoning
  the writer would add a state for the controller to handle; the controller already gets
  kStorage and decides. Recorded so the controller stream can choose to stop writing.
- **close():** the terminator reaching the storage closes the writer; a sync failure
  after it is reported but the writer stays closed (nothing may follow a terminator).
- **Short write continues; zero progress is an error** ("write made no progress"), not a loop.
- **Errors reuse `platform::SyscallError`** and its text for the detail.
- **Read bound:** `read_all` has no size cap; a journal of hours is megabytes.
- **Soak pacing:** 4320 entries (six hours at 5 s) by default, run unpaced; the nightly
  job sets `LOADFORGE_SOAK_INTERVAL_MS=5000` (and `LOADFORGE_SOAK_ENTRIES`) for the real
  interval. CI workflows are not mine to edit; the manager wires that.
- **T8 scope.** SIGKILL cannot tear a page-cache write; the torn tails are the contract's
  every-byte cuts, which also run over a real file (file_journal_test.cpp).

## Contract questions

1. **JournalContract over file-backed storage.** The suite constructs its own
   `MemoryStorage` and passes it to `Subject::open_writer`, so a provider cannot hand it
   a file. Reading chosen: run the suite over memory against the real writer/replay, and
   repeat its scenarios (closed, unclosed, every-byte cut, every-byte flip, not-empty) over
   a file written by `FileStorage` and cut/corrupted on disk. Suggest the suite take a
   Storage factory in a later revision.
2. **A flipped length bit.** The header says a failed record check is kCorrupt; a flip that
   makes a length exceed the remaining bytes cannot be told from a torn tail (unless it
   exceeds the maximum payload, which is refused as kCorrupt). Replay then says kTornTail with
   the intact prefix. The contract's own tests accept this.
3. Only one suite (`JournalContract`) is registered, so no
   `GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST` was needed.
