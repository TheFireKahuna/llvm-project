//===-- Init-time VA inventory for MAP_FIXED hardening -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// At process startup, the VA space already contains regions the libc did not
// create: PE images, the main thread stack, PEB/TEB, and the NT process heap.
// MAP_FIXED on any of these would destroy critical process state.
//
// This module builds a compact sorted table of these "foreign" regions via a
// single NtPssCaptureVaSpaceBulk call, then provides O(log N) pre-validation
// for MAP_FIXED requests. Regions are classified as:
//
//   IMAGE   — PE images (exe, DLLs). Never replaceable.
//   STACK   — Main thread stack reservation. Never replaceable.
//   PEB     — Process/thread environment blocks. Never replaceable.
//   HEAP    — NT process heap. Bounded risk (MAP_FIXED proceeds with warning).
//   PRIVATE — Other pre-existing MEM_PRIVATE regions. Bounded risk.
//
// The table is updated on DLL load/unload via LdrRegisterDllNotification.
//
// Total init cost: ~1 bulk syscall + ~5 point queries for TEB/PEB/stack.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VA_INVENTORY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VA_INVENTORY_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/common.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_utils.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

//===----------------------------------------------------------------------===//
// Foreign region types
//===----------------------------------------------------------------------===//

enum class ForeignType : ULONG {
  Image,   // MEM_IMAGE — PE executable/DLL
  Stack,   // Main thread stack reservation
  Peb,     // PEB or TEB allocation
  Heap,    // NT process heap
  Private, // Other pre-existing MEM_PRIVATE
};

struct ForeignRegion {
  uintptr_t base;
  uintptr_t end; // exclusive
  ForeignType type;
};

//===----------------------------------------------------------------------===//
// ForeignRegionTable — sorted, lock-free readable, writer-locked for updates
//===----------------------------------------------------------------------===//

inline constexpr int MAX_FOREIGN_REGIONS = 256;

struct ForeignRegionTable {
  ForeignRegion entries[MAX_FOREIGN_REGIONS];
  cpp::Atomic<int> count{0};
  Futex write_lock{0};

  /// O(log N) overlap check. Returns the type of the first foreign region
  /// overlapping [addr, addr+size), or nullopt-equivalent (false) if none.
  LIBC_INLINE bool overlaps(uintptr_t addr, uintptr_t size,
                            ForeignType &out_type) const {
    uintptr_t req_end = addr + size;
    // Atomic::load() lacks const qualifier upstream; cast is safe since
    // load is logically const (acquire fence, no mutation).
    int n = const_cast<cpp::Atomic<int> &>(count).load(
        cpp::MemoryOrder::ACQUIRE);

    // Binary search for the first entry with end > addr.
    int lo = 0, hi = n;
    while (lo < hi) {
      int mid = lo + (hi - lo) / 2;
      if (entries[mid].end <= addr)
        lo = mid + 1;
      else
        hi = mid;
    }

    if (lo < n && entries[lo].base < req_end) {
      out_type = entries[lo].type;
      return true;
    }
    return false;
  }

  /// Insert a region, maintaining sorted order. Called under write_lock.
  LIBC_INLINE bool insert(uintptr_t base, uintptr_t end, ForeignType type) {
    int n = count.load(cpp::MemoryOrder::RELAXED);
    if (n >= MAX_FOREIGN_REGIONS)
      return false;

    // Find insertion point (sorted by base).
    int pos = 0;
    while (pos < n && entries[pos].base < base)
      ++pos;

    // Shift right.
    for (int i = n; i > pos; --i)
      entries[i] = entries[i - 1];

    entries[pos] = {base, end, type};
    count.store(n + 1, cpp::MemoryOrder::RELEASE);
    return true;
  }

  /// Remove all entries with the given base. Called under write_lock.
  LIBC_INLINE void remove(uintptr_t base) {
    int n = count.load(cpp::MemoryOrder::RELAXED);
    int dst = 0;
    for (int src = 0; src < n; ++src) {
      if (entries[src].base != base) {
        if (dst != src)
          entries[dst] = entries[src];
        ++dst;
      }
    }
    count.store(dst, cpp::MemoryOrder::RELEASE);
  }

  LIBC_INLINE void lock() {
    for (;;) {
      FutexValueType expected = 0;
      if (write_lock.compare_exchange_weak(expected, 1,
                                           cpp::MemoryOrder::ACQUIRE,
                                           cpp::MemoryOrder::RELAXED))
        return;
      write_lock.wait(1);
    }
  }

  LIBC_INLINE void unlock() {
    write_lock.store_and_notify(0);
  }
};

/// Process-wide foreign region table. Zero-initialized at startup.
// NOLINTBEGIN(cert-err58-cpp)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wglobal-constructors"
inline ForeignRegionTable g_foreign;
#pragma clang diagnostic pop
// NOLINTEND(cert-err58-cpp)

//===----------------------------------------------------------------------===//
// Init-time scan
//===----------------------------------------------------------------------===//

/// Read TEB fields for stack bounds. Stable ABI since NT 5.1.
/// TEB+0x08 = StackBase (high address), TEB+0x10 = StackLimit (low, committed)
/// TEB+0x1478 = DeallocationStack (low, reserved — full reservation base)
struct StackBounds {
  PVOID base;               // TEB+0x08: top of stack (high address)
  PVOID limit;              // TEB+0x10: current committed limit
  PVOID deallocation_stack; // TEB+0x1478: full reservation base
};

LIBC_INLINE StackBounds read_stack_bounds() {
  StackBounds sb;
#ifdef __x86_64__
  __asm__ __volatile__("movq %%gs:0x08, %0" : "=r"(sb.base));
  __asm__ __volatile__("movq %%gs:0x10, %0" : "=r"(sb.limit));
  __asm__ __volatile__("movq %%gs:0x1478, %0" : "=r"(sb.deallocation_stack));
#elif defined(__aarch64__)
  __asm__ __volatile__("ldr %0, [x18, #0x08]" : "=r"(sb.base));
  __asm__ __volatile__("ldr %0, [x18, #0x10]" : "=r"(sb.limit));
  __asm__ __volatile__("ldr %0, [x18, #0x1478]" : "=r"(sb.deallocation_stack));
#endif
  return sb;
}

/// Register TEB, PEB, and main thread stack as foreign regions.
LIBC_INLINE void register_critical_addresses() {
  g_foreign.lock();

  // Main thread stack — full reservation from DeallocationStack to StackBase.
  StackBounds sb = read_stack_bounds();
  g_foreign.insert(reinterpret_cast<uintptr_t>(sb.deallocation_stack),
                   reinterpret_cast<uintptr_t>(sb.base),
                   ForeignType::Stack);

  // TEB — query its full allocation extent via MRI (not MBI).
  // MBI.RegionSize only covers the contiguous region with identical
  // protection attributes starting at the queried address. TEB/PEB
  // allocations span multiple regions (committed data + guard pages),
  // so MBI would leave guard pages unregistered and vulnerable to
  // MAP_FIXED clobber. MRI.RegionSize returns the full allocation.
  PVOID teb;
#ifdef __x86_64__
  __asm__ __volatile__("movq %%gs:0x30, %0" : "=r"(teb));
#elif defined(__aarch64__)
  __asm__ __volatile__("mov %0, x18" : "=r"(teb));
#endif
  char *teb_base, *teb_end;
  if (find_alloc_range(teb, teb_base, teb_end)) {
    g_foreign.insert(reinterpret_cast<uintptr_t>(teb_base),
                     reinterpret_cast<uintptr_t>(teb_end),
                     ForeignType::Peb);
  }

  // PEB — query its full allocation extent via MRI.
  PEB *peb = NtCurrentPeb();
  char *peb_base, *peb_end;
  if (find_alloc_range(peb, peb_base, peb_end)) {
    g_foreign.insert(reinterpret_cast<uintptr_t>(peb_base),
                     reinterpret_cast<uintptr_t>(peb_end),
                     ForeignType::Peb);
  }

  g_foreign.unlock();
}

/// Scan the entire VA space and register all pre-existing non-free regions.
/// Uses NtPssCaptureVaSpaceBulk for a kernel-consistent snapshot.
LIBC_INLINE void build_foreign_inventory() {
  g_foreign.lock();

  // 4KB scratch buffer — ~83 entries per page, paginates transparently.
  auto ws = byte_scratch(4096);
  if (!ws) {
    g_foreign.unlock();
    return;
  }
  RegionWalker walk(nullptr,
                    reinterpret_cast<NTPSS_MEMORY_BULK_INFORMATION *>(ws.data()),
                    ws.size());

  // Track the NT process heap base for classification.
  uintptr_t heap_base = reinterpret_cast<uintptr_t>(NtProcessHeap());
  MEMORY_BASIC_INFORMATION heap_mbi;
  uintptr_t heap_alloc_base = 0;
  if (query_region(reinterpret_cast<void *>(heap_base), heap_mbi))
    heap_alloc_base = reinterpret_cast<uintptr_t>(heap_mbi.AllocationBase);

  while (walk.next()) {
    if (walk.entry->State == MEM_FREE)
      continue;

    uintptr_t base = reinterpret_cast<uintptr_t>(walk.entry->BaseAddress);
    uintptr_t end = base + walk.entry->RegionSize;

    // Skip regions we already registered (stack, PEB, TEB).
    ForeignType existing;
    if (g_foreign.overlaps(base, walk.entry->RegionSize, existing))
      continue;

    ForeignType type;
    if (walk.entry->Type == MEM_IMAGE) {
      type = ForeignType::Image;
    } else if (walk.entry->AllocationBase &&
               reinterpret_cast<uintptr_t>(walk.entry->AllocationBase) ==
                   heap_alloc_base) {
      type = ForeignType::Heap;
    } else {
      type = ForeignType::Private;
    }

    g_foreign.insert(base, end, type);
  }

  g_foreign.unlock();
}

/// DLL load/unload notification callback. Keeps the table current for
/// images loaded after init (plugins, dlopen).
LIBC_INLINE void NTAPI foreign_dll_notification(
    ULONG reason, const LDR_DLL_NOTIFICATION_DATA *data, PVOID /*context*/) {
  if (reason == LDR_DLL_NOTIFICATION_REASON_LOADED) {
    uintptr_t base = reinterpret_cast<uintptr_t>(data->Loaded.DllBase);
    MEMORY_BASIC_INFORMATION mbi;
    if (!query_region(data->Loaded.DllBase, mbi))
      return;
    // Find full image extent.
    uintptr_t end = base;
    char *pos = static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
    char *alloc = static_cast<char *>(mbi.AllocationBase);
    while (query_region(pos, mbi) && mbi.AllocationBase == alloc) {
      pos = static_cast<char *>(mbi.BaseAddress) + mbi.RegionSize;
    }
    end = reinterpret_cast<uintptr_t>(pos);

    g_foreign.lock();
    g_foreign.insert(base, end, ForeignType::Image);
    g_foreign.unlock();
  } else if (reason == LDR_DLL_NOTIFICATION_REASON_UNLOADED) {
    uintptr_t base = reinterpret_cast<uintptr_t>(data->Unloaded.DllBase);
    g_foreign.lock();
    g_foreign.remove(base);
    g_foreign.unlock();
  }
}

//===----------------------------------------------------------------------===//
// MAP_FIXED pre-validation
//===----------------------------------------------------------------------===//

/// Check if [addr, addr+size) overlaps any foreign region. Returns 0 if
/// safe to proceed, EINVAL if the target would destroy a critical region.
/// Heap/Private overlaps are allowed (bounded risk, proceed to prepare_for_fixed).
LIBC_INLINE int validate_map_fixed_target(void *addr, SIZE_T size) {
  ForeignType type;
  if (!g_foreign.overlaps(reinterpret_cast<uintptr_t>(addr), size, type))
    return 0; // No overlap — safe.

  switch (type) {
  case ForeignType::Image:
  case ForeignType::Stack:
  case ForeignType::Peb:
    return EINVAL; // Never replaceable.
  case ForeignType::Heap:
  case ForeignType::Private:
    return 0; // Bounded risk — let prepare_for_fixed handle it.
  }
  return 0;
}

//===----------------------------------------------------------------------===//
// Top-level init
//===----------------------------------------------------------------------===//

/// DLL notification cookie — stored so va_inventory_fork_reinit() can
/// re-register after fork (LdrRegisterDllNotification state does not
/// survive NtCreateProcessEx).
inline PVOID g_dll_notify_cookie = nullptr;

/// Called from __libc_init, before C++ constructors. One-time cost.
LIBC_INLINE void mmap_subsystem_init() {
  register_critical_addresses();
  build_foreign_inventory();

  // Register DLL notification to keep the table current.
  LdrRegisterDllNotification(0, foreign_dll_notification, nullptr,
                             &g_dll_notify_cookie);
}

/// Re-register DLL notification in the fork child. The parent's
/// registration is process-specific loader state that doesn't survive
/// NtCreateProcessEx. Without this, dlopen/dlclose in the child would
/// silently fail to update g_foreign, allowing MAP_FIXED to clobber
/// newly-loaded images.
LIBC_INLINE void va_inventory_fork_reinit_dll_notify() {
  g_foreign.write_lock.reset_for_fork(0);
  g_dll_notify_cookie = nullptr;
  LdrRegisterDllNotification(0, foreign_dll_notification, nullptr,
                             &g_dll_notify_cookie);
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VA_INVENTORY_H
