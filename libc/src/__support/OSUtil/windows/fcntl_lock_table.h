//===-- POSIX byte-range lock tracking table for Windows ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-process userspace table tracking all fcntl byte-range locks (F_SETLK /
// F_SETLKW). Required because:
//
//   1. POSIX: file locks are NOT inherited by the child after fork().
//      NT clone-mode fork inherits the parent's NtLockFile state, so the
//      child must NtUnlockFile every inherited lock during fork_reinit.
//
//   2. POSIX: close() on ANY fd referring to a file releases ALL locks held
//      by the process on that file (regardless of which fd they were set
//      through). NT has no equivalent -- we must track and release them.
//
//   3. POSIX: overlapping locks from the same process are merged, split, or
//      replaced -- not rejected. NT byte-range locks from the same process on
//      the same file object conflict, so we must release old ranges before
//      acquiring new ones.
//
// Storage is a compact flat array backed by page_alloc/page_free (ntdll VM).
// Fully lazy: no allocation until the first lock. Grows by doubling (realloc-
// by-copy), shrinks when usage drops below 1/4 capacity, returns to nullptr
// when empty. No fixed limits. Typical usage is <1 page (102 records per 4KB).
//
// All mutating operations are serialized by a process-wide spinlock.
// Lock operations are infrequent relative to I/O -- contention is negligible.
//
// Depends only on page_alloc.h (ntdll), spin_wait.h, and NT file APIs.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FCNTL_LOCK_TABLE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FCNTL_LOCK_TABLE_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/raw_mutex.h"

#include "hdr/types/pid_t.h"
#include <stdint.h>

using HANDLE = void *;

namespace LIBC_NAMESPACE_DECL {
namespace internal {

/// File identity -- POSIX (dev_t, ino_t) equivalent on NT.
struct FileIdentity {
  uint64_t file_id;    // FileInternalInformation.IndexNumber (inode).
  uint32_t vol_serial; // FileFsVolumeInformation.VolumeSerialNumber (device).

  LIBC_INLINE bool operator==(const FileIdentity &o) const {
    return file_id == o.file_id && vol_serial == o.vol_serial;
  }
};

/// A single byte-range lock held by this process.
struct LockRecord {
  int64_t start;       // Absolute byte offset.
  int64_t end;         // Exclusive end (start + length). INT64_MAX = to-EOF.
  HANDLE handle;       // NT handle used for NtLockFile (needed for NtUnlockFile).
  uint64_t file_id;    // FileIdentity::file_id.
  uintptr_t canary;    // Integrity check: key ^ &this_record.
  uint32_t vol_serial; // FileIdentity::vol_serial.
  pid_t owner_pid;     // Owning process for POSIX F_GETLK l_pid reporting.
  int16_t type;        // F_RDLCK or F_WRLCK.
  int16_t pad_;
};

static_assert(sizeof(LockRecord) == 56, "LockRecord layout changed");

/// Compact, page-backed lock table with realloc-by-copy growth/shrink.
///
/// Hardening: each record carries a canary (XOR of slot address + per-instance
/// key seeded from ProcessPrng). Verified on every read, set on every write.
/// Detects stray overwrites and use-after-free into the page-backed array.
///
/// Synchronization: a single RawMutex (adaptive-spin-then-futex-park)
/// serializes mutations and scans. Every public entry point (set_lock,
/// release_file, getlk, peer_query) takes the mutex exclusively for its
/// entire table-touching phase; no operation holds the mutex across an
/// ALPC round-trip or any other blocking IPC. The original implementation
/// used a hand-rolled spinlock — RawMutex is a strict improvement: it
/// spins briefly under low contention (same fast path) but parks on the
/// futex once contention appears, so a slow critical section cannot burn
/// CPU on other callers.
class LockTable {
  LockRecord *data_ = nullptr;
  uint32_t size_ = 0;     // Active records (compact, no holes).
  uint32_t capacity_ = 0; // Allocated capacity in records.
  RawMutex mu_;
  cpp::Atomic<uint32_t> nonempty_{0}; // 1 when size_ > 0. Lock-free fast path.
  uintptr_t canary_key_ = 0; // Per-instance XOR key, seeded on first alloc.

  static constexpr size_t PAGE_SIZE = 4096;
  // Minimum allocation: 1 page = 102 records. Covers 99%+ of real use.
  static constexpr uint32_t MIN_RECORDS = PAGE_SIZE / sizeof(LockRecord);

  void acquire();
  void release();
  bool ensure_space();
  void maybe_shrink();
  void remove_at(uint32_t i);

  // Canary helpers — detect stray writes into the record array.
  LIBC_INLINE uintptr_t make_canary(const LockRecord *r) const {
    return canary_key_ ^ reinterpret_cast<uintptr_t>(r);
  }
  void set_canary(LockRecord &r) const;
  void check_canary(const LockRecord &r) const;
  void validate_all() const;

  /// Round record count up to fill whole pages.
  LIBC_INLINE static uint32_t records_to_pages_cap(uint32_t n) {
    size_t bytes = static_cast<size_t>(n) * sizeof(LockRecord);
    bytes = (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    return static_cast<uint32_t>(bytes / sizeof(LockRecord));
  }

  LIBC_INLINE bool matches_file(const LockRecord &r,
                                const FileIdentity &fid) const {
    return r.file_id == fid.file_id && r.vol_serial == fid.vol_serial;
  }

  LIBC_INLINE static bool overlaps(int64_t s1, int64_t e1, int64_t s2,
                                   int64_t e2) {
    return s1 < e2 && s2 < e1;
  }

  // NT lock/unlock helpers.
  static int nt_lock(HANDLE h, int64_t start, int64_t end, bool exclusive,
                     bool blocking);
  static void nt_unlock(HANDLE h, int64_t start, int64_t end);

public:
  void fork_reinit();

  /// Apply a POSIX lock (F_SETLK / F_SETLKW / F_UNLCK) with merge/split.
  /// Returns 0 on success, negative errno on failure.
  int set_lock(HANDLE handle, const FileIdentity &fid, int64_t start,
               int64_t end, int type, bool blocking);

  /// Release all locks on a file (for close()).
  void release_file(const FileIdentity &fid);

  /// Release every lock owned by this process and empty the table. Called
  /// from the lock_table .libcfin thunk during DLL_PROCESS_DETACH so the
  /// table is internally consistent before its memory unmaps. Defensive: a
  /// well-behaved caller will have closed every fd first (which already
  /// drops the kernel byte-range locks).
  void release_all_owned();

  /// F_GETLK: probe for external conflicts, accounting for our own locks.
  /// Returns 0 on success (out_type/out_pid set), negative errno on failure.
  int getlk(HANDLE handle, const FileIdentity &fid, int64_t start, int64_t end,
            int probe_type, int16_t &out_type, pid_t &out_pid);

  /// Answer a remote F_GETLK by scanning *our own* held locks. Called from
  /// the OP_LOCK_QUERY bus handler when another libc process has detected
  /// a conflict and is trying to identify the attested holder.
  ///
  /// Returns true if this process holds a lock on `(file_id, vol_serial)`
  /// that overlaps `[start, end)` and would conflict with `probe_type`.
  /// `out_type` is set to the held lock's type (F_RDLCK / F_WRLCK).
  bool peer_query(uint64_t file_id, uint32_t vol_serial, int64_t start,
                  int64_t end, int probe_type, int16_t &out_type);

  /// Lock-free check — safe to call outside the spinlock. Uses ACQUIRE
  /// so callers see the state that made the table non-empty.
  LIBC_INLINE bool empty() {
    return nonempty_.load(cpp::MemoryOrder::ACQUIRE) == 0;
  }

  /// Query (dev_t, ino_t) equivalent from an NT handle.
  static bool query_identity(HANDLE h, FileIdentity &out);
};

// --- Free functions for cross-module integration ---

void lock_table_fork_reinit();
void lock_table_release_file(HANDLE handle);
bool lock_table_empty();

int lock_table_set(HANDLE handle, int64_t start, int64_t end, int type,
                   bool blocking);

int lock_table_getlk(HANDLE handle, int64_t start, int64_t end,
                     int probe_type, int16_t &out_type, pid_t &out_pid);

/// Registers the OP_LOCK_QUERY handler with the ALPC bus. Called during
/// libc startup before the bus is brought up so the handler is live as
/// soon as the first inbound query arrives. Returns 0 on success.
int lock_table_startup_init();

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_FCNTL_LOCK_TABLE_H
