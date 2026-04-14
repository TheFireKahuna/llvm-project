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

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/nt/handle_attributes.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/futex_utils.h"
#include "src/__support/threads/windows/spin_wait.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

struct MmapLock {
  Futex owner{0};        // 0 = free, TID = held exclusively
  Futex shared_state{0}; // bit 31: writer waiting, bits 0-30: reader count

  static constexpr FutexValueType WRITER_WAITING = 1u << 31;
  static constexpr FutexValueType READER_MASK = ~WRITER_WAITING;

  // -- Exclusive (write) side ------------------------------------------------

  void acquire_exclusive() {
    FutexValueType self = NtCurrentThreadId();
    FutexValueType expected = 0;

    // Fast path: uncontended.
    if (LIBC_LIKELY(
            owner.compare_exchange_strong(expected, self,
                                          cpp::MemoryOrder::ACQUIRE)))
      goto drain;

    // Slow path: contended or dead owner.
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

      owner.wait(expected);
    }

  drain:
    // Block new shared acquires, then wait for existing readers to finish.
    set_writer_waiting();
    drain_readers();
  }

  void release_exclusive() {
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
        shared_state.wait(s);
        s = shared_state.load(cpp::MemoryOrder::RELAXED);
        continue;
      }
      // Increment reader count. CAS fails if a writer set the flag or
      // another reader raced — both are handled by the retry loop.
      if (shared_state.compare_exchange_weak(s, s + 1,
                                              cpp::MemoryOrder::ACQUIRE,
                                              cpp::MemoryOrder::RELAXED))
        return;
    }
  }

  void release_shared() {
    // Decrement reader count. s - 1 is safe: WRITER_WAITING (bit 31) is
    // unaffected because reader count (bits 0-30) is always >= 1 here.
    FutexValueType s = shared_state.load(cpp::MemoryOrder::RELAXED);
    while (!shared_state.compare_exchange_weak(
        s, s - 1, cpp::MemoryOrder::RELEASE, cpp::MemoryOrder::RELAXED))
      spin_wait::relax_processor();

    // Last reader out with a writer draining — wake it.
    if ((s & WRITER_WAITING) && (s & READER_MASK) == 1)
      shared_state.notify_one();
  }

  // -- Fork child reset ------------------------------------------------------

  void fork_reinit() {
    // Non-atomic: child is single-threaded after fork. Clears both the
    // value and the Treiber wait stack (no parent waiters exist in child).
    owner.reset_for_fork(0);
    shared_state.reset_for_fork(0);
  }

private:
  // CAS loop to set WRITER_WAITING.
  void set_writer_waiting() {
    FutexValueType s = shared_state.load(cpp::MemoryOrder::RELAXED);
    while (!shared_state.compare_exchange_weak(
        s, s | WRITER_WAITING, cpp::MemoryOrder::ACQUIRE,
        cpp::MemoryOrder::RELAXED))
      spin_wait::relax_processor();
  }

  // CAS loop to clear WRITER_WAITING.
  void clear_writer_waiting() {
    FutexValueType s = shared_state.load(cpp::MemoryOrder::RELAXED);
    while (!shared_state.compare_exchange_weak(
        s, s & READER_MASK, cpp::MemoryOrder::RELEASE,
        cpp::MemoryOrder::RELAXED))
      spin_wait::relax_processor();
  }

  // Spin-wait until all readers drain. The last reader to release calls
  // notify_one, waking us from the futex wait.
  void drain_readers() {
    for (;;) {
      FutexValueType s = shared_state.load(cpp::MemoryOrder::ACQUIRE);
      if ((s & READER_MASK) == 0)
        return;
      shared_state.wait(s);
    }
  }

  // Check if a thread is still alive by attempting to open its handle.
  static bool is_thread_alive(FutexValueType tid) {
    auto oa = internal_oa();
    CLIENT_ID cid = {};
    cid.UniqueThread = reinterpret_cast<HANDLE>(
        static_cast<ULONG_PTR>(static_cast<DWORD>(tid)));
    HANDLE h = nullptr;
    NTSTATUS st =
        ::NtOpenThread(&h, THREAD_QUERY_LIMITED_INFORMATION, &oa, &cid);
    if (NT_ERROR(st))
      return false;
    ::NtClose(h);
    return true;
  }
};

/// Global lock — shared across mmap.cpp, munmap.cpp, and mremap.cpp.
/// Zero-initialized (both Futex{0}), static-safe.
inline MmapLock g_mmap_lock;

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MMAP_LOCK_H
