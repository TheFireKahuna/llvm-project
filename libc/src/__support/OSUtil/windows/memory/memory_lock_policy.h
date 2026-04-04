//===-- Memory lock policy for mlockall/mlock2 -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Process-wide memory locking policy state shared by mlock2, mlockall, mmap,
// and the heap allocator.
//
// MCL_FUTURE: Global flag checked after every allocation. When set, newly
// committed pages are locked immediately via NtLockVirtualMemory.
//
// MLOCK_ONFAULT: Deferred locking via PAGE_GUARD. On first access, the guard
// page exception fires our VEH handler which locks the page. This mirrors
// Linux MLOCK_ONFAULT semantics: pages are locked on first fault, not at
// mlock2 call time.
//
// MCL_ONFAULT: Combination — future allocations get PAGE_GUARD treatment.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_LOCK_POLICY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_LOCK_POLICY_H

#include "hdr/errno_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/memory/working_set.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/threads/windows/futex_utils.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

//===----------------------------------------------------------------------===//
// MCL_FUTURE / MCL_ONFAULT global flags
//===----------------------------------------------------------------------===//

/// Bitmask of active mlockall flags. Zero when no policy is active.
/// Bits match MCL_CURRENT(1), MCL_FUTURE(2), MCL_ONFAULT(4).
inline cpp::Atomic<unsigned> g_mcl_flags{0};

/// True if MCL_FUTURE is active (lock all future allocations immediately).
LIBC_INLINE bool mcl_future_enabled() {
  return (g_mcl_flags.load(cpp::MemoryOrder::RELAXED) & 2 /*MCL_FUTURE*/) != 0;
}

/// True if MCL_ONFAULT is active (defer locking to first fault).
LIBC_INLINE bool mcl_onfault_enabled() {
  return (g_mcl_flags.load(cpp::MemoryOrder::RELAXED) & 4 /*MCL_ONFAULT*/) != 0;
}

/// Lock a committed range, expanding working set quota as needed.
/// Best-effort: failure is silently ignored (matches mlockall semantics).
LIBC_INLINE void lock_range(void *addr, SIZE_T size) {
  HANDLE process = NtCurrentProcess();
  expand_working_set(process, size);

  PVOID base = addr;
  SIZE_T region_size = size;
  ::NtLockVirtualMemory(process, &base, &region_size, MAP_PROCESS);
}

// lock_if_future is defined after onfault_arm_range (below).
LIBC_INLINE void lock_if_future(void *addr, SIZE_T size);

//===----------------------------------------------------------------------===//
// MLOCK_ONFAULT tracking — PAGE_GUARD-based deferred locking
//===----------------------------------------------------------------------===//

/// Maximum concurrently tracked onfault ranges. Each mlock2(MLOCK_ONFAULT)
/// call registers one range. munlock or munmap removes it.
inline constexpr int MAX_ONFAULT_RANGES = 256;

struct OnfaultRange {
  uintptr_t base;
  uintptr_t end; // exclusive
};

/// Process-wide onfault range table and guard page VEH.
/// Uses atomic reader-writer lock (no SRWLOCK) consistent with the rest
/// of the memory subsystem. Writers set WRITE_BIT; readers increment the
/// count. Contention uses our Futex (thread-ID alert, no hash table).
struct OnfaultState {
  // Low bit = writer active, upper bits = reader count.
  // Packed into a Futex so contention uses our thread-ID-based wait
  // instead of the heavier RtlWaitOnAddress hash table.
  static constexpr FutexValueType WRITE_BIT = 1;
  Futex rw_state{0};
  OnfaultRange ranges[MAX_ONFAULT_RANGES];
  int count = 0;

  void write_lock() {
    for (;;) {
      FutexValueType expected = 0;
      if (rw_state.compare_exchange_strong(expected, WRITE_BIT,
                                           cpp::MemoryOrder::ACQUIRE))
        return;
      // Yield on transient wait failure (-ENOMEM on pool exhaustion).
      long ret = rw_state.wait(expected);
      if (ret < 0 && ret != -EINTR)
        ::NtYieldExecution();
    }
  }

  void write_unlock() {
    rw_state.store_and_notify_all(0);
  }

  void read_lock() {
    for (;;) {
      FutexValueType val = rw_state.load(cpp::MemoryOrder::RELAXED);
      if (!(val & WRITE_BIT) &&
          rw_state.compare_exchange_strong(val, val + 2,
                                           cpp::MemoryOrder::ACQUIRE))
        return;
      long ret = rw_state.wait(val);
      if (ret < 0 && ret != -EINTR)
        ::NtYieldExecution();
    }
  }

  void read_unlock() {
    // Single LOCK SUB. fetch_sub returns the previous value — if it was
    // exactly 2, the reader count is now 0 and we must wake any waiter.
    FutexValueType prev = rw_state.fetch_sub(2, cpp::MemoryOrder::RELEASE);
    if (prev == 2)
      rw_state.notify_all();
  }

  /// Register a range for deferred locking. Maintains sorted order by base
  /// address for O(log N) lookup in contains().
  bool arm_range(void *addr, SIZE_T size) {
    uintptr_t base = reinterpret_cast<uintptr_t>(addr);
    uintptr_t end = base + size;

    write_lock();

    if (count >= MAX_ONFAULT_RANGES) {
      write_unlock();
      return false;
    }

    // Binary search for insertion point (sorted by base address).
    int lo = 0, hi = count;
    while (lo < hi) {
      int mid = lo + (hi - lo) / 2;
      if (ranges[mid].base < base)
        lo = mid + 1;
      else
        hi = mid;
    }

    // Shift elements right to make room.
    for (int i = count; i > lo; --i)
      ranges[i] = ranges[i - 1];

    ranges[lo].base = base;
    ranges[lo].end = end;
    ++count;

    write_unlock();

    apply_guard_pages(addr, size);
    return true;
  }

  /// Remove an onfault range (called by munlock/munmap). Maintains sorted order.
  void disarm_range(void *addr, SIZE_T size) {
    uintptr_t base = reinterpret_cast<uintptr_t>(addr);
    uintptr_t end = base + size;

    write_lock();

    // Compact in-place: copy non-overlapping entries forward.
    int dst = 0;
    for (int src = 0; src < count; ++src) {
      if (ranges[src].base >= end || ranges[src].end <= base)
        ranges[dst++] = ranges[src];
    }
    count = dst;

    write_unlock();
  }

  /// Check if an address falls within a tracked onfault range.
  /// O(log N) binary search — this runs on the VEH fault path.
  bool contains(uintptr_t addr) {
    read_lock();

    // Binary search for the first entry with end > addr.
    int lo = 0, hi = count;
    while (lo < hi) {
      int mid = lo + (hi - lo) / 2;
      if (ranges[mid].end <= addr)
        lo = mid + 1;
      else
        hi = mid;
    }

    bool found = (lo < count && ranges[lo].base <= addr);
    read_unlock();
    return found;
  }

private:
  /// Apply PAGE_GUARD modifier to all committed pages in [addr, addr+size).
  /// Protection changes on the current chunk don't affect subsequent entries'
  /// Protect values (which are from query time and correctly lack PAGE_GUARD).
  static void apply_guard_pages(void *addr, SIZE_T size) {
    HANDLE process = NtCurrentProcess();
    auto ws = byte_scratch(4096);
    if (!ws) return;
    RegionWalker walk(addr, size, ws.data(), ws.size());
    while (walk.next()) {
      if (walk.entry->State == MEM_COMMIT &&
          !(walk.entry->Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        PVOID base = walk.chunk;
        SIZE_T chunk = walk.chunk_size;
        ULONG old_prot;
        ::NtProtectVirtualMemory(process, &base, &chunk,
                                 walk.entry->Protect | PAGE_GUARD, &old_prot);
      }
    }
  }

public:
  /// VEH handler for PAGE_GUARD faults in tracked MLOCK_ONFAULT ranges.
  /// Defined in memory_lock_policy.cpp so the singleton stays privately owned
  /// by one translation unit.
  static LONG onfault_veh(EXCEPTION_POINTERS *ep);
};

// Explicitly-owned onfault state operations. The singleton itself lives in
// memory_lock_policy.cpp and is created during CRT startup, not on first use.
bool onfault_arm_range(void *addr, SIZE_T size);
void onfault_disarm_range(void *addr, SIZE_T size);
bool onfault_contains(uintptr_t addr);

/// Apply MCL_FUTURE policy after a successful allocation.
/// Called from mmap after committing pages.
LIBC_INLINE void lock_if_future(void *addr, SIZE_T size) {
  if (LIBC_LIKELY(!mcl_future_enabled()))
    return;
  if (mcl_onfault_enabled()) {
    (void)onfault_arm_range(addr, size);
    return;
  }
  lock_range(addr, size);
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_LOCK_POLICY_H
