//===-- RW lock for address-space mutation serialization -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Reader-writer lock mirroring Linux mmap_lock semantics. Serializes
// address-space structural mutations to prevent TOCTOU races between
// MAP_FIXED (which tears down existing mappings via prepare_for_fixed)
// and concurrent munmap.
//
//   Exclusive: MAP_FIXED, MREMAP_FIXED (prepare_for_fixed callers)
//   Shared:    munmap (all paths)
//
// On Linux, mmap_lock (rw_semaphore) serializes all VMA mutations.
// Here, the per-slot CAS in mapping_table.h handles most concurrency.
// This lock covers the remaining gap: prepare_for_fixed's multi-step
// MBI walk + extract + unmap sequence, which can race with a concurrent
// munmap that extracts + unmaps the same region.
//
// Properties:
//   - 16 bytes (2 × Futex), zero-initialized, static-safe
//   - Exclusive: TID-based ownership with dead-owner recovery
//   - Shared: atomic reader count, writer-preference
//   - Writer-preference: WRITER_WAITING blocks new readers, prevents
//     MAP_FIXED starvation under heavy munmap load
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MMAP_LOCK_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MMAP_LOCK_H

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/nt/scoped_nt_handle.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "src/__support/threads/windows/spin_wait.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// Predicate for MmapLock::drain_readers. Receives READER_MASK via the
// `arg` slot so the function can live at namespace scope (PredicateFn
// demands a freestanding `bool(*)(uint32_t, uint32_t)` with process-
// lifetime validity).
[[clang::always_inline]] LIBC_INLINE bool
readers_drained(uint32_t s, uint32_t reader_mask) noexcept {
  return (s & reader_mask) == 0;
}

struct MmapLock {
  Futex owner{0};        // 0 = free, TID = held exclusively
  Futex shared_state{0}; // bit 31: writer waiting, bits 0-30: reader count

  static constexpr FutexValueType WRITER_WAITING = 1u << 31;
  static constexpr FutexValueType READER_MASK = ~WRITER_WAITING;

  // ---------------------------------------------------------------------------
  // Per-thread held-depth counter
  // ---------------------------------------------------------------------------
  //
  // Tracks how many MmapLock acquires (shared or exclusive) the calling
  // thread currently holds. Lets dependent subsystems (notably
  // MappingTable::snapshot, which dereferences a pool-resolved RegionDesc
  // pointer that is only safe under MmapLock) assert the invariant at
  // entry without pulling MmapLock into their type signature.
  //
  // The counter is thread-local because the underlying lock has no
  // per-thread reader bookkeeping — the shared-side reader count is a
  // single global integer. We trade a TLS access on every acquire/release
  // for a debug-only ownership check; release builds compile the assert
  // and counter use to nothing.
  LIBC_INLINE static unsigned &held_depth_tls() {
    static thread_local unsigned depth = 0;
    return depth;
  }

  /// True iff this thread currently holds MmapLock (shared or exclusive).
  /// Used by callers that read a pool-resolved RegionDesc pointer to
  /// assert the lifetime invariant: only release()'s last-ref teardown
  /// path can close the descriptor's handles, and that path requires the
  /// writer lock — so any reader holding any flavor of this lock is
  /// guaranteed the descriptor remains live for the rest of its critical
  /// section.
  LIBC_INLINE static bool is_held_by_current_thread() {
    return held_depth_tls() > 0;
  }

  /// True iff this thread currently holds MmapLock in EXCLUSIVE (writer)
  /// mode. Stricter than `is_held_by_current_thread`: a thread holding
  /// only the shared side returns false. Used by `RegionPool::release`
  /// to validate the writer-only contract on its last-ref teardown path
  /// — closing handles and freeing the chunk list while a concurrent
  /// reader is mid-dereference would be a use-after-free, and the writer
  /// lock is what the contract uses to exclude that reader.
  LIBC_INLINE bool is_writer_held_by_current_thread() {
    return owner.load(cpp::MemoryOrder::RELAXED) == NtCurrentThreadId();
  }

  // -- Exclusive (write) side ------------------------------------------------

  void acquire_exclusive() {
    const FutexValueType self = NtCurrentThreadId();
    FutexValueType expected = 0;

    // Fast path: uncontended CAS.
    // Slow path: contended or dead owner — loop with dead-owner recovery.
    if (LIBC_UNLIKELY(!owner.compare_exchange_strong(
            expected, self, cpp::MemoryOrder::ACQUIRE))) {
      for (;;) {
        expected = 0;
        if (owner.compare_exchange_strong(expected, self,
                                          cpp::MemoryOrder::ACQUIRE))
          break;

        // Dead-owner recovery: if the holder terminated, steal the lock.
        if (expected != 0 && !is_thread_alive(expected)) {
          if (owner.compare_exchange_strong(expected, self,
                                            cpp::MemoryOrder::ACQUIRE))
            break;
          spin_wait::relax_processor();
          continue;
        }

        // Yield on transient wait failure (-ENOMEM on pool exhaustion)
        // so the enclosing CAS retry loop doesn't spin-fail hot.
        long ret = owner.wait(expected);
        if (ret < 0 && ret != -EINTR)
          ::NtYieldExecution();
      }
    }

    // Block new shared acquires, then wait for existing readers to finish.
    set_writer_waiting();
    drain_readers();
    ++held_depth_tls();
  }

  void release_exclusive() {
    --held_depth_tls();
    clear_writer_waiting();
    shared_state.notify_all(); // unblock waiting readers
    owner.store_and_notify(0); // release ownership, wake exclusive waiters
  }

  // -- Shared (read) side ----------------------------------------------------

  void acquire_shared() {
    FutexValueType s = shared_state.load(cpp::MemoryOrder::RELAXED);
    for (;;) {
      if (LIBC_UNLIKELY((s & WRITER_WAITING) != 0)) {
        // Writer active or waiting — back off until it finishes.
        long ret = shared_state.wait(s);
        if (ret < 0 && ret != -EINTR)
          ::NtYieldExecution();
        s = shared_state.load(cpp::MemoryOrder::RELAXED);
        continue;
      }
      // Increment reader count. CAS fails if a writer set the flag or
      // another reader raced — both are handled by the retry loop.
      if (shared_state.compare_exchange_weak(s, s + 1,
                                              cpp::MemoryOrder::ACQUIRE,
                                              cpp::MemoryOrder::RELAXED)) {
        ++held_depth_tls();
        return;
      }
    }
  }

  void release_shared() {
    --held_depth_tls();
    // Single LOCK SUB. fetch_sub returns the previous value; if we were
    // the last reader (reader count was 1) AND a writer is draining, wake
    // it. WRITER_WAITING (bit 31) is unaffected because bits 0-30 are the
    // only ones we touch.
    FutexValueType prev =
        shared_state.fetch_sub(1, cpp::MemoryOrder::RELEASE);
    if ((prev & WRITER_WAITING) && (prev & READER_MASK) == 1)
      shared_state.notify_one();
  }

  // -- Fork child reset ------------------------------------------------------

  void fork_reinit() {
    // Non-atomic: child is single-threaded after fork. Clears both the
    // value and the Treiber wait stack (no parent waiters exist in child).
    owner.reset_for_fork(0);
    shared_state.reset_for_fork(0);
    // The cloned thread inherits parent's TLS depth but starts with no
    // lock held in the child — clear so a defensive assert doesn't false-
    // positive after the child re-acquires.
    held_depth_tls() = 0;
  }

private:
  // Set WRITER_WAITING — single LOCK OR.
  void set_writer_waiting() {
    shared_state.fetch_or(WRITER_WAITING, cpp::MemoryOrder::ACQUIRE);
  }

  // Clear WRITER_WAITING — single LOCK AND with the reader-count mask.
  void clear_writer_waiting() {
    shared_state.fetch_and(READER_MASK, cpp::MemoryOrder::RELEASE);
  }

  // Park until the reader count drops to zero (bits 0-30 of
  // shared_state). Invoked by acquire_exclusive after WRITER_WAITING
  // is set, so no new shared acquires can raise the count past the
  // already-present readers — each release_shared is monotonic toward
  // zero. release_shared's notify_one on the last-reader edge pairs
  // with this wait.
  //
  // Uses wait_on_predicate: ret == 0 guarantees (s & READER_MASK) == 0
  // held at some point during the wait. The only non-zero return is
  // -ENOMEM (not Interruptible, no timeout, futex lifetime == this
  // lock which is static-safe). Pool exhaustion at an exclusive-lock
  // boundary is catastrophic with no retry story — trap rather than
  // spin-hide.
  void drain_readers() {
    long ret =
        shared_state.wait_on_predicate(&readers_drained, READER_MASK);
    if (LIBC_UNLIKELY(ret != 0))
      __builtin_trap();
  }

  // Check if a thread is still alive by attempting to open its handle.
  static bool is_thread_alive(FutexValueType tid) {
    auto oa = internal_oa();
    CLIENT_ID cid = {};
    cid.UniqueThread = reinterpret_cast<HANDLE>(
        static_cast<ULONG_PTR>(static_cast<DWORD>(tid)));
    ScopedNtHandle h;
    NTSTATUS st =
        ::NtOpenThread(h.put(), THREAD_QUERY_LIMITED_INFORMATION, &oa, &cid);
    if (NT_ERROR(st))
      return false;
    return true;
  }
};

/// Global lock — shared across mmap.cpp, munmap.cpp, and mremap.cpp.
/// Zero-initialized (both Futex{0}), static-safe.
inline MmapLock g_mmap_lock;

/// Scope-bound writer hold on `g_mmap_lock`. Non-copyable, non-movable;
/// release happens at scope exit unless `release()` was called.
///
/// Three usage patterns:
///
///   // Unconditional acquire at scope start:
///   MmapLockWriterGuard guard;
///
///   // Mid-scope release while continuing to run:
///   MmapLockWriterGuard guard;
///   ... mutate the table ...
///   guard.release();      // lock dropped, no-op at scope exit
///   ... more work without the lock ...
///
///   // Conditional acquire (deferred):
///   MmapLockWriterGuard guard{MmapLockWriterGuard::defer_acquire};
///   if (need_lock) guard.acquire();
///   ... ~guard releases iff held() ...
class MmapLockWriterGuard {
public:
  struct defer_acquire_t {};
  static constexpr defer_acquire_t defer_acquire{};

  LIBC_INLINE MmapLockWriterGuard() {
    g_mmap_lock.acquire_exclusive();
  }

  LIBC_INLINE explicit MmapLockWriterGuard(defer_acquire_t) : held_(false) {}

  LIBC_INLINE ~MmapLockWriterGuard() {
    if (held_)
      g_mmap_lock.release_exclusive();
  }

  /// Take the lock. No-op if already held. Pairs with `defer_acquire`
  /// construction for conditional-acquisition sites; safe to call from
  /// any state.
  LIBC_INLINE void acquire() {
    if (!held_) {
      g_mmap_lock.acquire_exclusive();
      held_ = true;
    }
  }

  /// Drop the lock early. Subsequent destruction is a no-op. Useful when
  /// the writer phase ends before the surrounding scope (e.g., the
  /// publish step is done, but the rest of the function operates on the
  /// new mapping without table-mutation rights).
  LIBC_INLINE void release() {
    if (held_) {
      g_mmap_lock.release_exclusive();
      held_ = false;
    }
  }

  LIBC_INLINE bool held() const { return held_; }

  MmapLockWriterGuard(const MmapLockWriterGuard &) = delete;
  MmapLockWriterGuard &operator=(const MmapLockWriterGuard &) = delete;
  MmapLockWriterGuard(MmapLockWriterGuard &&) = delete;
  MmapLockWriterGuard &operator=(MmapLockWriterGuard &&) = delete;

private:
  bool held_ = true;
};

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MMAP_LOCK_H
