//===-- POSIX byte-range lock table implementation ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/fcntl_lock_table.h"

#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/bcryptprimitives.h"
#include "src/__support/OSUtil/windows/ipc/alpc_bus.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/nt/nt_error.h"
#include "src/__support/OSUtil/windows/nt/nt_file_api.h"
#include "src/__support/OSUtil/windows/nt/nt_file_types.h"
#include "src/__support/macros/config.h"

#include <errno.h>
#include <fcntl.h>
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

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
// Mutex acquire/release
// =========================================================================
//
// Thin forwarders to the RawMutex inside the table. Keeping them as
// private members means the rest of this file reads like pseudocode
// (`acquire()` / `release()`) and the exact primitive is a single-point
// swap: today RawMutex, tomorrow something else if we need writer-
// preference RW semantics.

void LockTable::acquire() { mu_.lock(); }

void LockTable::release() {
  // Publish `nonempty_` BEFORE unlocking so external callers reading
  // empty() without the mutex see the current state. The RELEASE on
  // the futex unlock provides the ordering.
  nonempty_.store(size_ > 0 ? 1 : 0, cpp::MemoryOrder::RELAXED);
  mu_.unlock();
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

void LockTable::release_all_owned() {
  pid_t self_pid = current_pid();
  acquire();

  for (uint32_t i = size_; i-- > 0;) {
    check_canary(data_[i]);
    if (data_[i].owner_pid == self_pid) {
      // nt_unlock tolerates an already-closed handle (returns an error
      // we ignore). Either path leaves the kernel side lock-free.
      nt_unlock(data_[i].handle, data_[i].start, data_[i].end);
      remove_at(i);
    }
  }

  maybe_shrink();
  release();
}

// =========================================================================
// peer_query — answer OP_LOCK_QUERY from a remote libc process
// =========================================================================
//
// Only reports locks owned by *this* process. The inherited "foreign
// snapshots" (owner_pid != self_pid after fork) are never returned here —
// those belong to some ancestor whose identity the caller would learn
// from its own local table if it cared, and whose attested reply (if the
// ancestor is still alive) will come back via the caller's bus round
// trip anyway.

bool LockTable::peer_query(uint64_t file_id, uint32_t vol_serial,
                           int64_t start, int64_t end, int probe_type,
                           int16_t &out_type) {
  pid_t self_pid = current_pid();
  bool found = false;
  acquire();
  for (uint32_t i = 0; i < size_; ++i) {
    check_canary(data_[i]);
    if (data_[i].file_id != file_id || data_[i].vol_serial != vol_serial)
      continue;
    if (data_[i].owner_pid != self_pid)
      continue;
    if (!overlaps(data_[i].start, data_[i].end, start, end))
      continue;
    if (!conflicts_with_probe(data_[i].type, probe_type))
      continue;
    out_type = data_[i].type;
    found = true;
    break;
  }
  release();
  return found;
}

// =========================================================================
// F_GETLK attestation
// =========================================================================
//
// Two-phase design:
//
//   1. NT probe (authoritative conflict detection): tell the kernel whether
//      a conflict exists at all. NT owns this ground truth.
//
//   2. Peer attestation (optional, for l_pid): if the probe says a conflict
//      exists, ask the kernel which PIDs have handles open to the file
//      and query each via the ALPC bus. The holder's kernel-attested PID
//      is returned; if nobody claims it (non-libc opener, stale handle,
//      etc.) we return l_pid = 0.
//
// Neither phase relies on the local LockTable's owner_pid field for
// truth — those records can be stale across process exit / PID reuse.

namespace {

// Wire payload for OP_LOCK_QUERY.
struct LockQueryRequest {
  uint64_t file_id;
  uint32_t vol_serial;
  int32_t  probe_type;
  int64_t  start;
  int64_t  end;
};

struct LockQueryReply {
  int32_t  have_lock;  // 0 or 1
  int16_t  lock_type;  // F_RDLCK / F_WRLCK, valid only if have_lock
  uint16_t _reserved;
};

static_assert(sizeof(LockQueryRequest) == 32,
              "LockQueryRequest layout is part of the wire format");
static_assert(sizeof(LockQueryReply) == 8,
              "LockQueryReply layout is part of the wire format");

int32_t lock_query_handler(const alpc_bus::RequestView &req,
                           const alpc_bus::SenderInfo & /*sender*/,
                           const alpc_bus::ReplyBuffer &reply) {
  if (req.size < sizeof(LockQueryRequest) ||
      reply.capacity < sizeof(LockQueryReply))
    return STATUS_INVALID_PARAMETER;

  auto *q = static_cast<const LockQueryRequest *>(req.data);
  auto *r = static_cast<LockQueryReply *>(reply.data);
  r->have_lock = 0;
  r->lock_type = 0;
  r->_reserved = 0;

  int16_t lock_type = 0;
  if (g_lock_table.peer_query(q->file_id, q->vol_serial, q->start, q->end,
                               q->probe_type, lock_type)) {
    r->have_lock = 1;
    r->lock_type = lock_type;
  }

  *reply.size_out = sizeof(LockQueryReply);
  return STATUS_SUCCESS;
}

// Ask the kernel which processes currently have a handle open to the
// file behind `handle`. Buffer is sized for a reasonable upper bound
// (the typical case is 1–4 openers). Returns the count actually written
// into `out`, capped at `out_cap`.
uint32_t query_file_openers(HANDLE handle, uint32_t *out, uint32_t out_cap) {
  // 4 KB holds ~510 entries on x64; more than enough for any realistic
  // deployment. If the kernel reports more, the count is truncated —
  // getlk degrades to "couldn't determine owner" (l_pid = 0), not an
  // error.
  alignas(8) UCHAR buf[4096];
  IO_STATUS_BLOCK iosb = {};
  NTSTATUS st = NtQueryInformationFile(
      handle, &iosb, buf, static_cast<ULONG>(sizeof(buf)),
      FileProcessIdsUsingFileInformation);
  if (!NT_SUCCESS(st))
    return 0;

  auto *info = reinterpret_cast<FILE_PROCESS_IDS_USING_FILE_INFORMATION *>(buf);
  ULONG n = info->NumberOfProcessIdsInList;
  if (n > out_cap)
    n = out_cap;
  for (ULONG i = 0; i < n; ++i)
    out[i] = static_cast<uint32_t>(info->ProcessIdList[i]);
  return n;
}

// Ask `peer_pid` whether it currently holds a conflicting lock on the
// given range. Returns true (+ fills `out_type`) iff the peer answered
// "yes". Any other outcome — non-libc peer, dead peer, timeout, ACL
// denial — is reported as "don't know", which lets the caller fall
// back to l_pid = 0 without leaking wrong identity info.
bool query_peer_for_lock(pid_t peer_pid, const FileIdentity &fid,
                         int64_t start, int64_t end, int probe_type,
                         int16_t &out_type) {
  LockQueryRequest q{};
  q.file_id = fid.file_id;
  q.vol_serial = fid.vol_serial;
  q.probe_type = probe_type;
  q.start = start;
  q.end = end;

  LockQueryReply r{};
  uint32_t rlen = 0;
  constexpr int64_t TIMEOUT_100MS = -1000000LL;

  int32_t st = alpc_bus::request(peer_pid, alpc_bus::OP_LOCK_QUERY, &q,
                                  sizeof(q), &r, sizeof(r), &rlen,
                                  TIMEOUT_100MS);
  if (st != STATUS_SUCCESS || rlen < sizeof(LockQueryReply) ||
      r.have_lock == 0)
    return false;

  out_type = r.lock_type;
  return true;
}

} // anonymous namespace

int LockTable::getlk(HANDLE handle, const FileIdentity &fid, int64_t start,
                     int64_t end, int probe_type, int16_t &out_type,
                     pid_t &out_pid) {
  pid_t self_pid = current_pid();

  // =====================================================================
  // Phase A — NT probe with the table mutex held.
  //
  // Match the locking discipline used elsewhere in LockTable: every
  // table-touching operation runs start-to-finish under mu_ with no
  // mid-operation release. The critical section here is small (a scan
  // plus a handful of NtLockFile/NtUnlockFile calls) and never touches
  // an ALPC round-trip.
  // =====================================================================
  bool conflict = false;
  int probe_rc = 0;
  {
    acquire();

    // Snapshot our own overlapping locks so the probe isn't fooled by
    // them. Foreign records (owner_pid != self_pid) are inherited stale
    // hints at this layer and are never consulted for identity — all
    // l_pid answers come from ALPC attestation in Phase B.
    static constexpr uint32_t MAX_TEMP = 16;
    LockRecord temp[MAX_TEMP];
    uint32_t temp_count = 0;

    for (uint32_t i = 0; i < size_; ++i) {
      check_canary(data_[i]);
      if (!matches_file(data_[i], fid))
        continue;
      if (!overlaps(data_[i].start, data_[i].end, start, end))
        continue;
      if (data_[i].owner_pid == self_pid && temp_count < MAX_TEMP) {
        temp[temp_count] = data_[i];
        ++temp_count;
      }
    }

    for (uint32_t i = 0; i < temp_count; ++i)
      nt_unlock(temp[i].handle, temp[i].start, temp[i].end);

    bool exclusive = (probe_type == F_WRLCK);
    probe_rc = nt_lock(handle, start, end, exclusive, /*blocking=*/false);
    if (probe_rc == 0) {
      nt_unlock(handle, start, end);
    } else if (probe_rc == -EAGAIN) {
      conflict = true;
    }

    // Best-effort restore of self-locks. If the re-acquire fails because
    // another process grabbed the range in the window, that lock is lost
    // — this race is inherent to NT's non-atomic F_GETLK.
    for (uint32_t i = 0; i < temp_count; ++i)
      nt_lock(temp[i].handle, temp[i].start, temp[i].end,
              temp[i].type == F_WRLCK, /*blocking=*/false);

    release();
  }

  // Non-conflict error from the probe (EIO, EBADF etc.) propagates as-is.
  if (probe_rc != 0 && probe_rc != -EAGAIN)
    return probe_rc;

  if (!conflict) {
    out_type = F_UNLCK;
    out_pid = 0;
    return 0;
  }

  // =====================================================================
  // Phase B — ALPC attestation with NO table mutex held.
  //
  // A conflict exists; find and verify its owner. We never trust a
  // stored owner_pid for this: the kernel tells us which PIDs currently
  // have the file open, and each candidate libc peer vouches for its
  // own lock table over an ALPC round-trip (kernel-attested sender).
  //
  // If no peer claims the lock (non-libc holder, timeout, bus down on
  // either side), l_pid stays 0 — POSIX permits this and it is strictly
  // better than reporting a stale PID that may have been recycled.
  // =====================================================================
  out_type = static_cast<int16_t>(probe_type);
  out_pid = 0;

  uint32_t peers[32];
  uint32_t n = query_file_openers(handle, peers, 32);
  for (uint32_t i = 0; i < n; ++i) {
    pid_t peer = static_cast<pid_t>(peers[i]);
    if (peer == self_pid || peer == 0)
      continue;
    int16_t peer_type = 0;
    if (query_peer_for_lock(peer, fid, start, end, probe_type, peer_type)) {
      out_type = peer_type;
      out_pid = peer;
      break;
    }
  }
  return 0;
}

// =========================================================================
// fork_reinit
// =========================================================================

void LockTable::fork_reinit() {
  // Child is single-threaded; reset_for_fork puts the futex into the
  // UNLOCKED state regardless of what it was in the parent snapshot.
  mu_.reset_for_fork();

  // POSIX: file locks are not inherited across fork(). The child's
  // LockTable is empty from the child's perspective.
  //
  // Why clear outright instead of retaining inherited rows as "foreign"
  // snapshots: both LockTable::set_lock and LockTable::getlk filter by
  // `owner_pid == self_pid` (the child's PID, which no inherited record
  // matches), so inherited records are unreachable code paths — they
  // are never consulted for F_SETLK conflict resolution or F_GETLK
  // attribution. F_GETLK's Phase B attributes foreign holders through
  // `FileProcessIdsUsingFileInformation` + ALPC OP_LOCK_QUERY, so the
  // parent's identity is recovered via the same mechanism used for any
  // unrelated process — no special-casing needed.
  //
  // Do NOT call NtUnlockFile here: RtlCloneUserProcess shares FILE_OBJECTs
  // between parent and child, so unlocking through an inherited handle
  // would release the parent's kernel-level lock. If the child tries to
  // F_SETLK a range the parent still holds, NtLockFile returns
  // STATUS_FILE_LOCK_CONFLICT — POSIX-correct ("another process holds
  // the lock"), since the parent is a separate process post-fork.
  size_ = 0;
  nonempty_.store(0, cpp::MemoryOrder::RELAXED);
  // Backing storage is retained; the next set_lock will reuse it without
  // an allocation round-trip.
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

int lock_table_startup_init() {
  return alpc_bus::register_handler(alpc_bus::OP_LOCK_QUERY,
                                     lock_query_handler);
}

// Drop every byte-range lock owned by this process. fd_table_fini closes
// each fd shortly after, which also releases locks at the kernel layer via
// implicit handle teardown — doing it here keeps the table internally
// consistent before its memory unmaps.
//
// No alpc_bus handler de-registration: the dispatch table lives in c.dll's
// .bss and vanishes with the DLL unmap. A subsequent LoadLibrary maps a
// fresh image with re-zeroed handlers.
static void lock_table_fini() { g_lock_table.release_all_owned(); }

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

LIBC_REGISTER_FINI(8, lock_table,
                   &::LIBC_NAMESPACE::internal::lock_table_fini)

LIBC_REGISTER_FORK_REINIT(lock_table,
                          ::LIBC_NAMESPACE::internal::kForkPrioLockTable,
                          &::LIBC_NAMESPACE::internal::lock_table_fork_reinit)
