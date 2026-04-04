//===-- Process-wide VA discovery (kernel / image / foreign) ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// At process startup the VA space already contains regions the libc did
// not create: PE images (ntdll, c.dll, main exe, previously-loaded DLLs),
// the PEB and main-thread TEB, the main-thread stack reservation, the NT
// process heap, and anything a debugger or an earlier-initialised
// library may have allocated.
//
// This module's job is *discovery + classification*. It hands the
// resulting address ranges to the mapping table via the four
// `register_mapping_*` entry points, with the shape choice made at
// discovery time:
//
//   * TEB / PEB / main stack           → `KERNEL_REGION`
//   * MEM_IMAGE allocations / PE load  → `IMAGE_REGION`
//   * MEM_PRIVATE owned by this libc   → `LIBC_INTERNAL`
//   * Everything else                  → `FOREIGN_SENTINEL`
//
// All of the classification bookkeeping — overlap checks, staleness,
// writer locks over sorted arrays — has moved into the mapping table.
// This header now keeps only platform-specific discovery mechanics
// (TEB field reads, PE header parsing, `NtPssCaptureVaSpaceBulk`,
// `LdrRegisterDllNotification`) and the shutdown cookie lifecycle.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VA_INVENTORY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VA_INVENTORY_H

#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/memory/mapping_table.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/common.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

//===----------------------------------------------------------------------===//
// Stack bounds — main thread TEB fields, stable ABI since NT 5.1.
//   TEB+0x08  StackBase           top of stack, high address
//   TEB+0x10  StackLimit          committed low watermark
//   TEB+0x1478 DeallocationStack  full reservation base (low address)
//===----------------------------------------------------------------------===//

struct StackBounds {
  PVOID base;
  PVOID limit;
  PVOID deallocation_stack;
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

//===----------------------------------------------------------------------===//
// Kernel-region discovery.
//===----------------------------------------------------------------------===//
//
// Registers TEB, PEB, and the main thread's stack reservation as
// `KERNEL_REGION` slots. Each range is resolved to its full NT
// allocation extent via MRI (`find_alloc_range`) rather than MBI —
// MBI only covers the contiguous sub-region with identical protection
// attributes, which would omit guard pages on multi-region TEB / PEB
// allocations and leave them vulnerable to MAP_FIXED clobber.

LIBC_INLINE void discover_kernel_regions() {
  const StackBounds sb = read_stack_bounds();
  if (sb.deallocation_stack != nullptr && sb.base != nullptr) {
    void *base = sb.deallocation_stack;
    SIZE_T size =
        static_cast<SIZE_T>(static_cast<char *>(sb.base) -
                            static_cast<char *>(sb.deallocation_stack));
    if (size != 0)
      (void)g_mapping_table.register_mapping_kernel(base, size);
  }

  PVOID teb;
#ifdef __x86_64__
  __asm__ __volatile__("movq %%gs:0x30, %0" : "=r"(teb));
#elif defined(__aarch64__)
  __asm__ __volatile__("mov %0, x18" : "=r"(teb));
#endif
  {
    char *alloc_base = nullptr;
    char *alloc_end = nullptr;
    if (find_alloc_range(teb, alloc_base, alloc_end))
      (void)g_mapping_table.register_mapping_kernel(
          alloc_base, static_cast<SIZE_T>(alloc_end - alloc_base));
  }

  PEB *peb = NtCurrentPeb();
  if (peb != nullptr) {
    char *alloc_base = nullptr;
    char *alloc_end = nullptr;
    if (find_alloc_range(peb, alloc_base, alloc_end))
      (void)g_mapping_table.register_mapping_kernel(
          alloc_base, static_cast<SIZE_T>(alloc_end - alloc_base));
  }
}

//===----------------------------------------------------------------------===//
// Loaded-module discovery.
//===----------------------------------------------------------------------===//
//
// Walks the loader's in-memory-order module list and registers each
// image's full NT allocation extent as `IMAGE_REGION`. Typical process
// state at Phase 0c.5: ntdll, c.dll, the main EXE.

LIBC_INLINE void discover_loaded_modules() {
  PEB *peb = NtCurrentPeb();
  if (peb == nullptr || peb->Ldr == nullptr)
    return;

  PEB_LDR_DATA *ldr = peb->Ldr;
  // InLoadOrderLinks is at offset 0 of LDR_DATA_TABLE_ENTRY; direct
  // reinterpret_cast from LIST_ENTRY* matches the convention used by
  // dlfcn / stack_walker / bt_format / exec_ops.
  LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
  for (LIST_ENTRY *cur = head->Flink; cur != head && cur != nullptr;
       cur = cur->Flink) {
    auto *entry = reinterpret_cast<LDR_DATA_TABLE_ENTRY *>(cur);
    if (entry->DllBase == nullptr || entry->SizeOfImage == 0)
      continue;
    (void)g_mapping_table.register_mapping_image(
        entry->DllBase, static_cast<SIZE_T>(entry->SizeOfImage));
  }
}

//===----------------------------------------------------------------------===//
// Residual foreign discovery.
//===----------------------------------------------------------------------===//
//
// After `discover_kernel_regions` and `discover_loaded_modules` have
// stamped the known OS-managed regions, a bulk VA scan sweeps up
// whatever is left — NT process heap, debugger-injected ranges,
// third-party `VirtualAllocEx` allocations — and classifies by VAD
// type.
//
//   * MEM_IMAGE    → IMAGE_REGION (catches modules the Ldr walk missed)
//   * MEM_PRIVATE  → FOREIGN_SENTINEL (NT private VA not belonging to us)
//   * MEM_MAPPED   → FOREIGN_SENTINEL (section views by other owners)
//
// The "MEM_PRIVATE owned by us" case (Tier A `_internal` allocations)
// is NOT relabelled here — it will be retro-registered by the meta
// allocator's init path, which knows precisely which allocations came
// from this libc. Touching them here would race.
//
// Register per *NT allocation*, not per MBI sub-region. MBI splits a
// single allocation into one entry per protection regime (e.g. a PE
// image yields separate entries for its header page, .text, .rdata,
// ...). The mapping table's slot is keyed on `view_base >> 16` and
// represents the full allocation — passing sub-region bases collides
// against the slot already stamped for the same allocation.
//
// Bulk MBI entries arrive in address order, and sub-regions of a
// single NT allocation are contiguous. We coalesce consecutive entries
// with the same `AllocationBase` into one registration, using the
// first entry's `Type` (stable across sub-regions of a single
// allocation). No MRI syscall required.

LIBC_INLINE void publish_pending_foreign(void *base, SIZE_T size, DWORD type) {
  if (base == nullptr || size == 0)
    return;
  // MEM_PRIVATE / MEM_MAPPED are "not ours unless already registered
  // as LIBC_INTERNAL by an earlier Tier A allocator (PCB / VEH / etc.)
  // or by the radix pools' self-cover path". `register_mapping_foreign`
  // bails on any non-FREE / non-FOREIGN slot state, so it will not
  // stomp existing LIBC_INTERNAL / KERNEL_REGION / IMAGE_REGION stamps.
  if (type == MEM_IMAGE)
    (void)g_mapping_table.register_mapping_image(base, size);
  else
    (void)g_mapping_table.register_mapping_foreign(base, size);
}

LIBC_INLINE void discover_foreign_regions() {
  // Whole-process VA discovery is a Tier A bootstrap path, so avoid the
  // thread scratch allocator here. A dedicated bulk buffer keeps the sweep
  // independent of scratch lifetime and is automatically stamped as
  // LIBC_INTERNAL once page_alloc's registration hooks are live.
  constexpr SIZE_T BULK_BYTES = 0x10000;
  void *buf = LIBC_NAMESPACE::internal::page_alloc(BULK_BYTES);
  if (buf == nullptr)
    return;

  RegionWalker walk(nullptr,
                    reinterpret_cast<NTPSS_MEMORY_BULK_INFORMATION *>(buf),
                    BULK_BYTES);

  void *pending_base = nullptr;
  SIZE_T pending_size = 0;
  DWORD pending_type = 0;

  while (walk.next()) {
    if (walk.entry->State == MEM_FREE)
      continue;

    void *alloc_base = walk.entry->AllocationBase;
    if (alloc_base == nullptr)
      continue;

    if (alloc_base == pending_base) {
      // Same NT allocation, next protection band — extend.
      pending_size += walk.entry->RegionSize;
      continue;
    }

    // New allocation — publish the previous one (if any) and start
    // accumulating the new one.
    publish_pending_foreign(pending_base, pending_size, pending_type);
    pending_base = alloc_base;
    pending_size = walk.entry->RegionSize;
    pending_type = walk.entry->Type;
  }

  publish_pending_foreign(pending_base, pending_size, pending_type);
  LIBC_NAMESPACE::internal::page_free(buf);
}

//===----------------------------------------------------------------------===//
// DLL load/unload notification callback.
//===----------------------------------------------------------------------===//
//
// Keeps IMAGE_REGION coverage current across `LdrLoadDll` /
// `LdrUnloadDll`. On load: register each module as IMAGE_REGION.
// On unload: extract. The extract path releases any sentinel
// reference (no-op for IMAGE_SENTINEL_REGION_ID — pinned refcount).

NTAPI LIBC_INLINE void dll_notification_callback(
    ULONG reason, const LDR_DLL_NOTIFICATION_DATA *data, PVOID /*context*/) {
  if (reason == LDR_DLL_NOTIFICATION_REASON_LOADED) {
    void *dll_base = data->Loaded.DllBase;
    if (dll_base == nullptr || data->Loaded.SizeOfImage == 0)
      return;
    (void)g_mapping_table.register_mapping_image(
        dll_base, static_cast<SIZE_T>(data->Loaded.SizeOfImage));
    return;
  }

  if (reason == LDR_DLL_NOTIFICATION_REASON_UNLOADED) {
    // Shutdown gate — mirrors veh_core's callback. Late UNLOADs
    // during ExitProcess → LdrpShutdownProcess happen after the
    // mapping table's fini has run; skip them.
    PEB *peb = NtCurrentPeb();
    if (peb == nullptr || (peb->Ldr && peb->Ldr->ShutdownInProgress))
      return;
    void *dll_base = data->Unloaded.DllBase;
    if (dll_base == nullptr)
      return;
    g_mapping_table.remove(dll_base);
  }
}

//===----------------------------------------------------------------------===//
// Cookie lifecycle.
//===----------------------------------------------------------------------===//

/// DLL notification cookie. Process-specific loader state; does NOT
/// survive `RtlCloneUserProcess`. Re-registered on fork via
/// `va_inventory_fork_reinit()`.
inline PVOID g_dll_notify_cookie = nullptr;

/// Register the DLL load/unload notification callback. Called once at
/// Phase 0c.5 after the initial discovery sweep, and again in the
/// fork child from `va_inventory_fork_reinit()`.
[[nodiscard]] LIBC_INLINE int register_dll_notification() {
  NTSTATUS st = LdrRegisterDllNotification(0, dll_notification_callback,
                                           nullptr, &g_dll_notify_cookie);
  return NT_SUCCESS(st) ? 0 : 1;
}

/// Full Phase 0c.5 sweep: kernel → loaded modules → residual foreign
/// → notification registration. Called from `mapping_table_startup_
/// init` after the meta allocator is live.
[[nodiscard]] LIBC_INLINE int va_inventory_startup_discover() {
  discover_kernel_regions();
  discover_loaded_modules();
  discover_foreign_regions();
  return register_dll_notification();
}

/// Fork child reinit. Called from the child-side fork_reinit chain.
/// The mapping table's radix contents survive fork via CoW (the
/// discovered kernel / image / foreign slots are still valid). Only
/// the DLL notification cookie has to be re-registered — loader
/// notification state is process-specific.
LIBC_INLINE void va_inventory_fork_reinit() {
  g_dll_notify_cookie = nullptr;
  (void)LdrRegisterDllNotification(0, dll_notification_callback, nullptr,
                                   &g_dll_notify_cookie);
}

/// Shutdown: drop the DLL notification registration so the loader
/// stops calling back into unmapped code.
LIBC_INLINE void va_inventory_fini() {
  if (g_dll_notify_cookie != nullptr) {
    LdrUnregisterDllNotification(g_dll_notify_cookie);
    g_dll_notify_cookie = nullptr;
  }
}

//===----------------------------------------------------------------------===//
// MAP_FIXED pre-validation (compat path for mremap / FixedRangeGuard).
//===----------------------------------------------------------------------===//
//
// Existing call sites (mremap_engine.cpp, fixed_range_guard.h) probe a
// MAP_FIXED target ahead of the destructive prepare step and bail with
// -EINVAL when the target lands on a shape we refuse to displace. The
// former implementation walked a parallel `g_foreign` array; the new
// implementation snapshots the mapping table directly.
//
// Returns 0 when the target may proceed, EINVAL when any slot in the
// range carries a block-MAP_FIXED shape (LIBC_INTERNAL / IMAGE_REGION
// / KERNEL_REGION / FOREIGN_SENTINEL). Slots covering user mappings
// (ANON_PLACEHOLDER / FILE_VIEW_* / etc.) are NOT rejected here —
// those are the exact VAs the caller WANTS to overwrite; Pass 1 of
// `prepare_for_fixed` handles their teardown.

[[nodiscard]] LIBC_INLINE int validate_map_fixed_target(void *addr,
                                                        SIZE_T size) {
  if (addr == nullptr || size == 0)
    return 0;

  struct Ctx {
    bool blocked;
  } ctx{false};

  char *end = static_cast<char *>(addr) + size;
  g_mapping_table.walk_range(
      addr, end,
      +[](const SlotSnapshot *snap, void *vctx) {
        auto &c = *static_cast<Ctx *>(vctx);
        if (c.blocked || snap->region == nullptr)
          return;
        if (snap->region->blocks_map_fixed())
          c.blocked = true;
      },
      &ctx);

  return ctx.blocked ? EINVAL : 0;
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VA_INVENTORY_H
