//===-- POSIX byte-range lock table implementation ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/fcntl_lock_table.h"

#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/bcryptprimitives.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt/nt_file_api.h"
#include "src/__support/OSUtil/windows/nt/nt_file_types.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/spin_wait.h"

#include <errno.h>
#include <fcntl.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// =========================================================================
// Global instance
// =========================================================================

static LockTable g_lock_table;

static LIBC_INLINE pid_t current_pid() {
  return static_cast<pid_t>(NtCurrentProcessId());
}

static LIBC_INLINE bool conflicts_with_probe(int16_t lock_type, int probe_type) {
  if (probe_type == F_WRLCK)
    return lock_type == F_WRLCK || lock_type == F_RDLCK;
  if (probe_type == F_RDLCK)
    return lock_type == F_WRLCK;
  return false;
}

// =========================================================================
// Spinlock
// =========================================================================

void LockTable::acquire() {
  if (LIBC_LIKELY(!lock_.exchange(1, cpp::MemoryOrder::ACQUIRE)))
    return;
  for (;;) {
    spin_wait::spin_on_raw_u32(&lock_.val, 1u);
    if (!lock_.exchange(1, cpp::MemoryOrder::ACQUIRE))
      return;
    ::NtYieldExecution();
  }
}

void LockTable::release() {
  // Publish nonempty flag before releasing the spinlock. External callers
  // read empty() without the lock — they must see the current state.
  // The RELEASE on the lock store provides the ordering guarantee.
  nonempty_.store(size_ > 0 ? 1 : 0, cpp::MemoryOrder::RELAXED);
  lock_.store(0, cpp::MemoryOrder::RELEASE);
}

// =========================================================================
// Canary hardening
// =========================================================================

void LockTable::set_canary(LockRecord &r) const {
  r.canary = make_canary(&r);
}

void LockTable::check_canary(const LockRecord &r) const {
  if (r.canary != make_canary(&r))
    __builtin_trap(); // Corruption detected.
}

void LockTable::validate_all() const {
  for (uint32_t i = 0; i < size_; ++i)
    check_canary(data_[i]);
}

// =========================================================================
// Storage — realloc-by-copy with page_alloc/page_free
// =========================================================================

bool LockTable::ensure_space() {
  if (LIBC_LIKELY(size_ < capacity_))
    return true;

  // Seed the canary key on first allocation.
  if (canary_key_ == 0) {
    ::ProcessPrng(reinterpret_cast<unsigned char *>(&canary_key_),
                  sizeof(canary_key_));
    if (canary_key_ == 0)
      canary_key_ = 0xDEAD'BEEF'CAFE'BABE;
  }

  // Double capacity, minimum 1 page worth of records.
  // Guard against uint32_t overflow on the doubling (would require ~96 GB
  // of lock records to trigger — page_alloc would fail regardless).
  uint32_t new_cap;
  if (capacity_ == 0)
    new_cap = MIN_RECORDS;
  else if (capacity_ > UINT32_MAX / 2)
    return false; // Cannot double without overflow.
  else
    new_cap = capacity_ * 2;
  new_cap = records_to_pages_cap(new_cap);
  size_t new_bytes = static_cast<size_t>(new_cap) * sizeof(LockRecord);

  auto *new_data = static_cast<LockRecord *>(page_alloc(new_bytes));
  if (!new_data)
    return false;

  // Copy existing records and rewrite canaries (addresses changed).
  if (data_) {
    for (uint32_t i = 0; i < size_; ++i) {
      check_canary(data_[i]);
      new_data[i] = data_[i];
      new_data[i].canary = make_canary(&new_data[i]);
    }
    page_free(data_);
  }

  data_ = new_data;
  capacity_ = new_cap;
  return true;
}

void LockTable::maybe_shrink() {
  if (size_ == 0) {
    // Fully empty — release everything. nonempty_ is updated in release().
    if (data_) {
      page_free(data_);
      data_ = nullptr;
      capacity_ = 0;
    }
    return;
  }

  // Shrink when usage drops below 1/4 capacity AND we'd still have at
  // least MIN_RECORDS. Hysteresis: grow at 1x, shrink at 1/4x.
  uint32_t target = records_to_pages_cap(size_);
  if (target >= capacity_ / 2)
    return; // Not worth shrinking yet.
  uint32_t new_cap = capacity_ / 2;
  if (new_cap < MIN_RECORDS)
    new_cap = MIN_RECORDS;
  if (new_cap >= capacity_)
    return;

  size_t new_bytes = static_cast<size_t>(new_cap) * sizeof(LockRecord);
  auto *new_data = static_cast<LockRecord *>(page_alloc(new_bytes));
  if (!new_data)
    return; // Shrink is best-effort — old allocation still works.

  for (uint32_t i = 0; i < size_; ++i) {
    check_canary(data_[i]);
    new_data[i] = data_[i];
    new_data[i].canary = make_canary(&new_data[i]);
  }
  page_free(data_);

  data_ = new_data;
  capacity_ = new_cap;
}

void LockTable::remove_at(uint32_t i) {
  --size_;
  if (i != size_) {
    check_canary(data_[size_]);
    data_[i] = data_[size_];
    set_canary(data_[i]);
  }
}

// =========================================================================
// NT lock/unlock wrappers
// =========================================================================

int LockTable::nt_lock(HANDLE h, int64_t start, int64_t end, bool exclusive,
                       bool blocking) {
  LARGE_INTEGER offset, length;
  offset.QuadPart = start;
  length.QuadPart = end - start;
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS st = NtLockFile(h, nullptr, nullptr, nullptr, &iosb, &offset,
                           &length, 0, blocking ? FALSE : TRUE,
                           exclusive ? TRUE : FALSE);
  if (NT_SUCCESS(st))
    return 0;
  if (st == STATUS_LOCK_NOT_GRANTED || st == STATUS_FILE_LOCK_CONFLICT)
    return -EAGAIN;
  return -windows_util::ntstatus_to_errno(st);
}

void LockTable::nt_unlock(HANDLE h, int64_t start, int64_t end) {
  LARGE_INTEGER offset, length;
  offset.QuadPart = start;
  length.QuadPart = end - start;
  IO_STATUS_BLOCK iosb = {};
  // Best-effort: STATUS_RANGE_NOT_LOCKED is not fatal.
  NtUnlockFile(h, &iosb, &offset, &length, 0);
}

// =========================================================================
// File identity query
// =========================================================================

bool LockTable::query_identity(HANDLE h, FileIdentity &out) {
  IO_STATUS_BLOCK iosb = {};

  // File ID (inode equivalent).
  FILE_INTERNAL_INFORMATION fii;
  NTSTATUS st = NtQueryInformationFile(h, &iosb, &fii, sizeof(fii),
                                       FileInternalInformation);
  if (!NT_SUCCESS(st))
    return false;
  out.file_id = static_cast<uint64_t>(fii.IndexNumber.QuadPart);

  // Volume serial (device equivalent). Buffer sized to match
  // query_volume_serial() in nt_to_stat.h.
  alignas(8) char buf[sizeof(FILE_FS_VOLUME_INFORMATION) + 64];
  iosb = {};
  st = NtQueryVolumeInformationFile(h, &iosb, buf, sizeof(buf),
                                    FileFsVolumeInformation);
  if (!NT_SUCCESS(st))
    return false;
  auto *vol = reinterpret_cast<FILE_FS_VOLUME_INFORMATION *>(buf);
  out.vol_serial = vol->VolumeSerialNumber;

  return true;
}

// =========================================================================
// set_lock — POSIX merge/split logic
// =========================================================================

int LockTable::set_lock(HANDLE handle, const FileIdentity &fid, int64_t start,
                        int64_t end, int type, bool blocking) {
  pid_t self_pid = current_pid();
  acquire();

  // --- Snapshot: save ALL records for this file before modification. ---
  // Used for full rollback if Phase 2's NtLockFile fails. Saving every
  // record for the file (not just overlapping) lets the rollback nuke all
  // Phase 1 modifications and restore the exact pre-call state.
  // Stack buffer: 32 records × 48 bytes = 1536 bytes. Handles virtually
  // all real-world cases; overflow degrades to best-effort rollback.
  static constexpr uint32_t SNAP_CAP = 32;
  LockRecord snapshot[SNAP_CAP];
  uint32_t snap_count = 0;
  bool snap_overflow = false;

  for (uint32_t i = 0; i < size_; ++i) {
    check_canary(data_[i]);
    if (!matches_file(data_[i], fid) || data_[i].owner_pid != self_pid)
      continue;
    if (snap_count < SNAP_CAP)
      snapshot[snap_count] = data_[i];
    else
      snap_overflow = true;
    ++snap_count;
  }

  // --- Phase 1: process overlapping locks on this file. ---
  // We work in reverse so that remove_at (swap-with-last) doesn't
  // invalidate indices we haven't visited yet.

  for (uint32_t i = size_; i-- > 0;) {
    check_canary(data_[i]);
    if (!matches_file(data_[i], fid) || data_[i].owner_pid != self_pid)
      continue;
    if (!overlaps(data_[i].start, data_[i].end, start, end))
      continue;

    // This record overlaps the new lock region.

    // Release the overlapping portion in NT.
    nt_unlock(data_[i].handle, data_[i].start, data_[i].end);

    bool extends_left = data_[i].start < start;
    bool extends_right = data_[i].end > end;

    if (extends_left && extends_right) {
      // Record straddles the new range — split into two.
      // Left piece: keep in place, trim end.
      int64_t orig_end = data_[i].end;
      int16_t orig_type = data_[i].type;
      HANDLE orig_handle = data_[i].handle;

      data_[i].end = start;
      nt_lock(data_[i].handle, data_[i].start, data_[i].end,
              data_[i].type == F_WRLCK, false);
      set_canary(data_[i]);

      // Right piece: append. ensure_space() may reallocate, invalidating
      // data_[i], so we captured what we need above.
      if (ensure_space()) {
        LockRecord &right = data_[size_];
        right.start = end;
        right.end = orig_end;
        right.handle = orig_handle;
        right.file_id = fid.file_id;
        right.vol_serial = fid.vol_serial;
        right.owner_pid = self_pid;
        right.type = orig_type;
        right.pad_ = 0;
        set_canary(right);
        ++size_;
        nt_lock(right.handle, right.start, right.end, right.type == F_WRLCK,
                false);
      } else {
        // Cannot track the right piece. Undo the split: re-lock the
        // original range and restore the record to its original extent.
        // data_[i] is still valid here (ensure_space failed, no realloc).
        nt_unlock(data_[i].handle, data_[i].start, data_[i].end);
        data_[i].end = orig_end;
        nt_lock(data_[i].handle, data_[i].start, data_[i].end,
                data_[i].type == F_WRLCK, false);
        set_canary(data_[i]);
      }
    } else if (extends_left) {
      // Trim right edge.
      data_[i].end = start;
      nt_lock(data_[i].handle, data_[i].start, data_[i].end,
              data_[i].type == F_WRLCK, false);
      set_canary(data_[i]);
    } else if (extends_right) {
      // Trim left edge.
      data_[i].start = end;
      nt_lock(data_[i].handle, data_[i].start, data_[i].end,
              data_[i].type == F_WRLCK, false);
      set_canary(data_[i]);
    } else {
      // Fully contained — remove.
      remove_at(i);
    }
  }

  // --- Phase 2: acquire the new lock (if not F_UNLCK). ---
  int result = 0;
  if (type != F_UNLCK) {
    bool exclusive = (type == F_WRLCK);
    result = nt_lock(handle, start, end, exclusive, blocking);

    if (result == 0) {
      // Success — add the record.
      if (!ensure_space()) {
        // Cannot track the lock we just acquired. Undo the NT lock so we
        // don't leak it (close() wouldn't know to release it).
        nt_unlock(handle, start, end);
        result = -ENOMEM;
      } else {
        LockRecord &nr = data_[size_];
        nr.start = start;
        nr.end = end;
        nr.handle = handle;
        nr.file_id = fid.file_id;
        nr.vol_serial = fid.vol_serial;
        nr.owner_pid = self_pid;
        nr.type = static_cast<int16_t>(type);
        nr.pad_ = 0;
        set_canary(nr);
        ++size_;
      }
    } else {
      // NtLockFile failed — full rollback: undo all Phase 1 modifications
      // by nuking this file's records and restoring from snapshot.
      //
      // Inherent race: between NtUnlockFile (Phase 1) and NtLockFile
      // (restore), another process could grab the range. If the restore
      // NtLockFile fails, that individual lock is lost. This window is
      // intrinsic to NT's non-atomic lock replacement.
      uint32_t restore = snap_overflow ? SNAP_CAP : snap_count;

      // Step 1: remove all current records for this file, release NT locks.
      for (uint32_t i = size_; i-- > 0;) {
        if (matches_file(data_[i], fid) && data_[i].owner_pid == self_pid) {
          nt_unlock(data_[i].handle, data_[i].start, data_[i].end);
          remove_at(i);
        }
      }

      // Step 2: restore original records from snapshot, re-acquire NT locks.
      for (uint32_t j = 0; j < restore; ++j) {
        if (!ensure_space())
          break; // OOM during rollback — remaining locks lost.
        data_[size_] = snapshot[j];
        set_canary(data_[size_]);
        ++size_;
        nt_lock(snapshot[j].handle, snapshot[j].start, snapshot[j].end,
                snapshot[j].type == F_WRLCK, false);
      }
    }
  }

  // --- Phase 3: merge adjacent same-type records for this file. ---
  // Simple O(n^2) pass — n is records on this file, typically < 10.
  for (uint32_t i = 0; i < size_; ++i) {
    check_canary(data_[i]);
    if (!matches_file(data_[i], fid) || data_[i].owner_pid != self_pid)
      continue;
    bool merged;
    do {
      merged = false;
      for (uint32_t j = i + 1; j < size_; ++j) {
        check_canary(data_[j]);
        if (!matches_file(data_[j], fid) || data_[j].owner_pid != self_pid)
          continue;
        if (data_[i].type != data_[j].type)
          continue;
        // Merge if adjacent or overlapping.
        if (data_[i].end >= data_[j].start && data_[j].end >= data_[i].start) {
          // NT side: unlock both, lock the merged range.
          nt_unlock(data_[i].handle, data_[i].start, data_[i].end);
          nt_unlock(data_[j].handle, data_[j].start, data_[j].end);

          int64_t new_start =
              data_[i].start < data_[j].start ? data_[i].start : data_[j].start;
          int64_t new_end =
              data_[i].end > data_[j].end ? data_[i].end : data_[j].end;

          // Use the handle from whichever record we keep.
          nt_lock(data_[i].handle, new_start, new_end,
                  data_[i].type == F_WRLCK, false);

          data_[i].start = new_start;
          data_[i].end = new_end;
          set_canary(data_[i]);
          remove_at(j);
          merged = true;
          break;
        }
      }
    } while (merged);
  }

  maybe_shrink();
  release();
  return result;
}

// =========================================================================
// release_file — POSIX close() semantics
// =========================================================================

void LockTable::release_file(const FileIdentity &fid) {
  pid_t self_pid = current_pid();
  acquire();

  for (uint32_t i = size_; i-- > 0;) {
    check_canary(data_[i]);
    if (matches_file(data_[i], fid) && data_[i].owner_pid == self_pid) {
      nt_unlock(data_[i].handle, data_[i].start, data_[i].end);
      remove_at(i);
    }
  }

  maybe_shrink();
  release();
}

// =========================================================================
// getlk — F_GETLK with self-lock awareness
// =========================================================================

int LockTable::getlk(HANDLE handle, const FileIdentity &fid, int64_t start,
                     int64_t end, int probe_type, int16_t &out_type,
                     pid_t &out_pid) {
  pid_t self_pid = current_pid();
  acquire();

  // Find our own overlapping locks on this file.
  // Stack buffer for temporarily released locks.
  static constexpr uint32_t MAX_TEMP = 16;
  LockRecord temp[MAX_TEMP];
  uint32_t temp_count = 0;
  pid_t foreign_pid = -1;
  int16_t foreign_type = static_cast<int16_t>(probe_type);

  for (uint32_t i = 0; i < size_; ++i) {
    check_canary(data_[i]);
    if (!matches_file(data_[i], fid))
      continue;
    if (!overlaps(data_[i].start, data_[i].end, start, end))
      continue;
    if (data_[i].owner_pid == self_pid) {
      if (temp_count < MAX_TEMP) {
        temp[temp_count] = data_[i];
        ++temp_count;
      }
      continue;
    }
    if (foreign_pid == -1 && conflicts_with_probe(data_[i].type, probe_type)) {
      foreign_pid = data_[i].owner_pid;
      foreign_type = data_[i].type;
    }
  }

  // Temporarily release our overlapping locks so the probe can detect
  // external conflicts without being blocked by our own locks.
  for (uint32_t i = 0; i < temp_count; ++i)
    nt_unlock(temp[i].handle, temp[i].start, temp[i].end);

  // Probe: try to acquire the lock non-blocking.
  bool exclusive = (probe_type == F_WRLCK);
  int rc = nt_lock(handle, start, end, exclusive, /*blocking=*/false);

  if (rc == 0) {
    // Probe succeeded — no external conflict.
    nt_unlock(handle, start, end);
    out_type = F_UNLCK;
    out_pid = 0;
  } else if (rc == -EAGAIN) {
    // External conflict detected. If we preserved a fork-time foreign
    // snapshot, report its owning PID; otherwise the owner is unknown.
    out_type =
        foreign_pid == -1 ? static_cast<int16_t>(probe_type) : foreign_type;
    out_pid = foreign_pid;
    rc = 0; // F_GETLK itself succeeds; the flock struct reports the conflict.
  }
  // Other errors propagate as-is.

  // Re-acquire our temporarily released locks. Best-effort: if a re-acquire
  // fails (another process grabbed the range in the window), the lock is
  // lost. This race is inherent to NT's lack of an atomic F_GETLK primitive.
  for (uint32_t i = 0; i < temp_count; ++i)
    nt_lock(temp[i].handle, temp[i].start, temp[i].end,
            temp[i].type == F_WRLCK, /*blocking=*/false);

  release();
  return rc;
}

// =========================================================================
// fork_reinit
// =========================================================================

void LockTable::fork_reinit() {
  // Child is single-threaded — no lock contention.
  lock_.store(0, cpp::MemoryOrder::RELAXED);

  // Release all inherited NT locks. Preserve the records themselves as
  // foreign snapshots so F_GETLK in the child can still report the parent
  // owner PID for locks that existed at the fork boundary.
  for (uint32_t i = 0; i < size_; ++i) {
    nt_unlock(data_[i].handle, data_[i].start, data_[i].end);
    data_[i].handle = nullptr;
  }

  nonempty_.store(size_ > 0 ? 1u : 0u, cpp::MemoryOrder::RELAXED);
}

// =========================================================================
// Free functions
// =========================================================================

void lock_table_fork_reinit() { g_lock_table.fork_reinit(); }

bool lock_table_empty() { return g_lock_table.empty(); }

void lock_table_release_file(HANDLE handle) {
  if (g_lock_table.empty())
    return;

  FileIdentity fid;
  if (!LockTable::query_identity(handle, fid))
    return; // Non-disk handle or query failed — no locks possible.

  g_lock_table.release_file(fid);
}

int lock_table_set(HANDLE handle, int64_t start, int64_t end, int type,
                   bool blocking) {
  FileIdentity fid;
  if (!LockTable::query_identity(handle, fid))
    return -EIO;

  return g_lock_table.set_lock(handle, fid, start, end, type, blocking);
}

int lock_table_getlk(HANDLE handle, int64_t start, int64_t end,
                     int probe_type, int16_t &out_type, pid_t &out_pid) {
  FileIdentity fid;
  if (!LockTable::query_identity(handle, fid))
    return -EIO;

  return g_lock_table.getlk(handle, fid, start, end, probe_type, out_type,
                            out_pid);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
