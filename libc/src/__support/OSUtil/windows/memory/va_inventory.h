//===- va_inventory.h - Process-startup VA discovery ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Process-startup discovery and classification of VA the libc did not
/// create.
///
/// At entry the user VA space already contains regions owned by the OS or
/// by code that ran before libc bring-up: PE images (ntdll, c.dll, main
/// exe, previously-loaded DLLs), the PEB and main-thread TEB, the
/// main-thread stack reservation, the NT process heap, and anything a
/// debugger or earlier-initialised library may have allocated. This module
/// discovers those ranges via `NtQueryVirtualMemory` and stamps them as
/// *cordons* in the pagemap.
///
/// Cordons are a pagemap-only concept; they do not participate in the
/// `va_tracker` skiplist. Three flavours are stamped (see
/// `alloc::pagemap::CordonKind`):
///
///   * Kernel  - TEB / PEB / main stack.
///   * Image   - PE module load extents.
///   * Foreign - everything else (NT process heap, debugger ranges,
///               third-party `VirtualAllocEx`, ...).
///
/// The VEH master's first-line probe (`pagemap::classify`) sees any
/// cordon-stamped chunk and routes the fault to
/// `EXCEPTION_CONTINUE_SEARCH` without consulting the tracker; only chunks
/// that decode to `Empty` reach `va_tracker::resolve` for POSIX VA. This
/// is what keeps the SIGSEGV classifier wait-free against non-POSIX VA -
/// the cordon stamp is one atomic load + cookie XOR + range check per
/// 64 KiB chunk.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VA_INVENTORY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VA_INVENTORY_H

#include "src/__support/OSUtil/windows/alloc/legacy/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/pagemap_cordon.h"
#include "src/__support/OSUtil/windows/nt_pal/nt_pal.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/common.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

namespace va_inventory_detail {
// Stamp a cordon range, no-op for empty / null inputs. Callers pass
// kernel- or loader-derived `(base, size)` tuples that may legitimately
// be zero (e.g. a module entry with `SizeOfImage == 0` mid-tear-down).
LIBC_INLINE void register_sentinel(void *base, SIZE_T size,
                                    alloc::pagemap::CordonKind kind) {
  if (base == nullptr || size == 0)
    return;
  (void)alloc::pagemap::stamp_cordon(base, static_cast<size_t>(size), kind);
}
} // namespace va_inventory_detail

//===----------------------------------------------------------------------===//
// Stack bounds
//===----------------------------------------------------------------------===//

/// Triple of main-thread stack pointers read straight from TEB fields.
///
/// Stable ABI since NT 5.1; the three offsets are documented in
/// phnt-style headers as `Tib.StackBase`, `Tib.StackLimit`,
/// `DeallocationStack`. `deallocation_stack` is the full reservation
/// base (lowest address); `base` is the top of the committed range
/// (highest address); `limit` is the current committed low watermark.
struct StackBounds {
  PVOID base;
  PVOID limit;
  PVOID deallocation_stack;
};

/// Reads the three stack-bounds TEB fields directly.
///
/// One `mov` per field on x86-64 (`gs:`-relative) or one `ldr` per field
/// on AArch64 (`x18`-relative) - no syscall, no library call.
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
// Kernel-region discovery
//===----------------------------------------------------------------------===//

/// Stamps the main-thread stack reservation, the TEB allocation, and the
/// PEB allocation as `CordonKind::Kernel`.
///
/// Each region is resolved to its full NT allocation extent via MRI
/// (`nt_pal::find_alloc_range`) rather than MBI. MBI only covers the
/// contiguous sub-region with identical protection attributes, which on
/// a multi-band allocation (TEB / PEB carry guard pages with distinct
/// protection) would omit the guard pages and leave them vulnerable to
/// MAP_FIXED clobber.
LIBC_INLINE void discover_kernel_regions() {
  const StackBounds sb = read_stack_bounds();
  if (sb.deallocation_stack != nullptr && sb.base != nullptr) {
    void *base = sb.deallocation_stack;
    SIZE_T size =
        static_cast<SIZE_T>(static_cast<char *>(sb.base) -
                            static_cast<char *>(sb.deallocation_stack));
    if (size != 0)
      va_inventory_detail::register_sentinel(
          base, size, alloc::pagemap::CordonKind::Kernel);
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
    if (nt_pal::find_alloc_range(teb, alloc_base, alloc_end))
      va_inventory_detail::register_sentinel(
          alloc_base, static_cast<SIZE_T>(alloc_end - alloc_base),
          alloc::pagemap::CordonKind::Kernel);
  }

  PEB *peb = NtCurrentPeb();
  if (peb != nullptr) {
    char *alloc_base = nullptr;
    char *alloc_end = nullptr;
    if (nt_pal::find_alloc_range(peb, alloc_base, alloc_end))
      va_inventory_detail::register_sentinel(
          alloc_base, static_cast<SIZE_T>(alloc_end - alloc_base),
          alloc::pagemap::CordonKind::Kernel);
  }
}

//===----------------------------------------------------------------------===//
// Loaded-module discovery
//===----------------------------------------------------------------------===//

/// Walks the loader's in-memory-order module list and stamps each image's
/// full NT allocation extent as `CordonKind::Image`.
///
/// `InLoadOrderLinks` is at offset 0 of `LDR_DATA_TABLE_ENTRY`, so the
/// `LIST_ENTRY*` returned by the chain walk reinterpret-casts directly
/// to the table entry - the convention used elsewhere in libc by dlfcn /
/// stack_walker / bt_format / exec_ops.
LIBC_INLINE void discover_loaded_modules() {
  PEB *peb = NtCurrentPeb();
  if (peb == nullptr || peb->Ldr == nullptr)
    return;

  PEB_LDR_DATA *ldr = peb->Ldr;
  LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
  for (LIST_ENTRY *cur = head->Flink; cur != head && cur != nullptr;
       cur = cur->Flink) {
    auto *entry = reinterpret_cast<LDR_DATA_TABLE_ENTRY *>(cur);
    if (entry->DllBase == nullptr || entry->SizeOfImage == 0)
      continue;
    va_inventory_detail::register_sentinel(
        entry->DllBase, static_cast<SIZE_T>(entry->SizeOfImage),
        alloc::pagemap::CordonKind::Image);
  }
}

//===----------------------------------------------------------------------===//
// Residual foreign discovery
//===----------------------------------------------------------------------===//

/// Publishes one accumulated foreign region. `MEM_IMAGE` is promoted to
/// `Image` (catches modules the Ldr walk missed); `MEM_PRIVATE` and
/// `MEM_MAPPED` both decode to `Foreign`.
LIBC_INLINE void publish_pending_foreign(void *base, SIZE_T size, DWORD type) {
  if (base == nullptr || size == 0)
    return;
  va_inventory_detail::register_sentinel(
      base, size,
      type == MEM_IMAGE ? alloc::pagemap::CordonKind::Image
                         : alloc::pagemap::CordonKind::Foreign);
}

/// Bulk sweep that classifies any residual NT allocation left after the
/// kernel and loaded-module passes.
///
/// Catches NT process heap, debugger-injected ranges, third-party
/// `VirtualAllocEx` allocations, and any module the in-memory-order
/// walker missed. Bulk MBI entries arrive in address order, and
/// sub-regions of one NT allocation are contiguous, so consecutive
/// entries sharing `AllocationBase` are coalesced into a single stamp
/// using the first entry's `Type` - which is stable across protection
/// bands of one allocation.
LIBC_INLINE void discover_foreign_regions() {
  // Whole-process VA discovery runs early in libc bring-up before the
  // thread scratch allocator is wired in. A dedicated bulk buffer keeps
  // the sweep independent of scratch lifetime; `page_alloc`'s
  // registration hooks stamp the buffer as libc-internal automatically.
  constexpr SIZE_T BULK_BYTES = 0x10000;
  void *buf = LIBC_NAMESPACE::internal::page_alloc(BULK_BYTES);
  if (buf == nullptr)
    return;

  auto walk = nt_pal::RegionWalker::whole_process(buf, BULK_BYTES);

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
      // Same NT allocation, next protection band - extend.
      pending_size += walk.entry->RegionSize;
      continue;
    }

    // New allocation - publish the previous one (if any) and restart.
    publish_pending_foreign(pending_base, pending_size, pending_type);
    pending_base = alloc_base;
    pending_size = walk.entry->RegionSize;
    pending_type = walk.entry->Type;
  }

  publish_pending_foreign(pending_base, pending_size, pending_type);
  LIBC_NAMESPACE::internal::page_free(buf);
}

//===----------------------------------------------------------------------===//
// DLL load/unload notification callback
//===----------------------------------------------------------------------===//

/// Loader DLL-load / DLL-unload notification trampoline.
///
/// Keeps Image cordon coverage current across `LdrLoadDll` /
/// `LdrUnloadDll`. On load: stamp the new module as `Image`. On unload:
/// retire the cordon entries covering the range.
NTAPI LIBC_INLINE void dll_notification_callback(
    ULONG reason, const LDR_DLL_NOTIFICATION_DATA *data, PVOID /*context*/) {
  if (reason == LDR_DLL_NOTIFICATION_REASON_LOADED) {
    void *dll_base = data->Loaded.DllBase;
    if (dll_base == nullptr || data->Loaded.SizeOfImage == 0)
      return;
    va_inventory_detail::register_sentinel(
        dll_base, static_cast<SIZE_T>(data->Loaded.SizeOfImage),
        alloc::pagemap::CordonKind::Image);
    return;
  }

  if (reason == LDR_DLL_NOTIFICATION_REASON_UNLOADED) {
    // Skip late UNLOADs that arrive during `ExitProcess` /
    // `LdrpShutdownProcess` after the tracker has finalised. Mirrors
    // the shutdown gate in veh_core's notification callback.
    PEB *peb = NtCurrentPeb();
    if (peb == nullptr || (peb->Ldr && peb->Ldr->ShutdownInProgress))
      return;
    void *dll_base = data->Unloaded.DllBase;
    if (dll_base == nullptr || data->Unloaded.SizeOfImage == 0)
      return;
    alloc::pagemap::retire_cordon(
        dll_base, static_cast<size_t>(data->Unloaded.SizeOfImage));
  }
}

//===----------------------------------------------------------------------===//
// Cookie lifecycle
//===----------------------------------------------------------------------===//

/// Loader notification cookie. Process-specific; does not survive
/// `RtlCloneUserProcess`, so the fork child re-registers via
/// `va_inventory_fork_reinit()`.
inline PVOID g_dll_notify_cookie = nullptr;

/// Registers the DLL load/unload notification callback with the loader.
///
/// Called once during libc bring-up after the initial discovery sweep,
/// and again in the fork child from `va_inventory_fork_reinit()`.
[[nodiscard]] LIBC_INLINE int register_dll_notification() {
  NTSTATUS st = LdrRegisterDllNotification(0, dll_notification_callback,
                                           nullptr, &g_dll_notify_cookie);
  return NT_SUCCESS(st) ? 0 : 1;
}

/// Full startup sweep: kernel regions, loaded modules, residual foreign,
/// loader notification registration.
///
/// \returns 0 on success; non-zero if the loader notification
///          registration failed.
[[nodiscard]] LIBC_INLINE int va_inventory_startup_discover() {
  discover_kernel_regions();
  discover_loaded_modules();
  discover_foreign_regions();
  return register_dll_notification();
}

/// Fork-child reinit hook.
///
/// Cordon contents survive fork via CoW, so the pagemap stamps remain
/// valid. Only the loader notification cookie is process-specific and
/// must be re-registered.
LIBC_INLINE void va_inventory_fork_reinit() {
  g_dll_notify_cookie = nullptr;
  (void)LdrRegisterDllNotification(0, dll_notification_callback, nullptr,
                                   &g_dll_notify_cookie);
}

/// Drops the loader notification registration so subsequent
/// `LdrLoadDll` / `LdrUnloadDll` calls do not enter unmapped code.
LIBC_INLINE void va_inventory_fini() {
  if (g_dll_notify_cookie != nullptr) {
    LdrUnregisterDllNotification(g_dll_notify_cookie);
    g_dll_notify_cookie = nullptr;
  }
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VA_INVENTORY_H
