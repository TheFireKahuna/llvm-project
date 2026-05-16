//===- va_inventory.h - Process-startup VA discovery ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Process-startup discovery of VA the libc did not create (PE images, PEB,
// TEB, main stack, NT process heap, debugger / third-party allocations) and
// classification into *cordons* on the pagemap. Cordons are pagemap-only;
// they never enter the va_tracker skiplist.
//
// Three kinds (see alloc::pagemap::CordonKind): Kernel (TEB / PEB / main
// stack), Image (PE module extents), Foreign (everything else).
//
// The VEH master's first-line probe (pagemap::classify) routes cordoned
// chunks to EXCEPTION_CONTINUE_SEARCH without consulting the tracker; only
// chunks decoding to Empty reach va_tracker::resolve. The stamp is one
// atomic load + cookie XOR + range check per 64 KiB chunk, which is what
// keeps the SIGSEGV classifier wait-free against non-POSIX VA.
//
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
// Callers pass kernel- or loader-derived (base, size) tuples that may
// legitimately be zero (e.g. a module entry observed with SizeOfImage == 0
// mid-tear-down); silently drop those instead of stamping a zero range.
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

// Main-thread stack pointers mirrored from the TEB (Tib.StackBase,
// Tib.StackLimit, DeallocationStack). `deallocation_stack` is the
// reservation base (lowest address); `base` is the top of the committed
// range (highest address); `limit` is the current committed low watermark.
struct StackBounds {
  PVOID base;
  PVOID limit;
  PVOID deallocation_stack;
};

// Direct TEB field reads via the segment / platform register: one mov per
// field on x86-64 (gs:-relative) or one ldr per field on AArch64
// (x18-relative). The libc deliberately never links Win32 (no
// GetCurrentTeb / NtQueryInformationThread) and an ntdll round-trip is
// pointless for fields the TEB already exposes directly.
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

// Each region resolves to its full NT allocation extent via MRI
// (nt_pal::find_alloc_range) rather than MBI: MBI only covers the
// contiguous sub-region with identical protection attributes, so for a
// multi-band allocation (TEB / PEB carry guard pages with distinct
// protection) it would omit the guard pages and leave them vulnerable to
// MAP_FIXED clobber.
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

  // TEB self-pointer: NT_TIB.Self at TEB+0x30 on x86-64; AArch64 keeps the
  // TEB pointer live in x18 by ABI, so the register read is the address.
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

// InLoadOrderLinks is at offset 0 of LDR_DATA_TABLE_ENTRY, so the
// LIST_ENTRY* returned by the chain walk reinterpret-casts directly to
// the table entry.
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

// MEM_IMAGE is promoted to Image to catch modules the Ldr walk missed;
// MEM_PRIVATE and MEM_MAPPED both decode to Foreign.
LIBC_INLINE void publish_pending_foreign(void *base, SIZE_T size, DWORD type) {
  if (base == nullptr || size == 0)
    return;
  va_inventory_detail::register_sentinel(
      base, size,
      type == MEM_IMAGE ? alloc::pagemap::CordonKind::Image
                         : alloc::pagemap::CordonKind::Foreign);
}

// Bulk MBI entries arrive in address order and sub-regions of one NT
// allocation are contiguous, so consecutive entries sharing AllocationBase
// coalesce into a single stamp using the first entry's Type (stable across
// protection bands of one allocation).
LIBC_INLINE void discover_foreign_regions() {
  // Runs before the thread scratch allocator is wired in; a dedicated
  // bulk buffer keeps the sweep independent of scratch lifetime.
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
      // Same NT allocation, next protection band: extend.
      pending_size += walk.entry->RegionSize;
      continue;
    }

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

// Keeps Image cordon coverage current across LdrLoadDll / LdrUnloadDll.
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
    // Drop late UNLOADs arriving during ExitProcess / LdrpShutdownProcess
    // after libc fini: retire_cordon would touch pagemap metadata that may
    // already be torn down. Same shutdown gate that veh_core's notification
    // callback uses.
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

// Process-specific; does not survive RtlCloneUserProcess, so the fork
// child re-registers via va_inventory_fork_reinit().
inline PVOID g_dll_notify_cookie = nullptr;

// Returns 0 on success, non-zero on LdrRegisterDllNotification failure.
[[nodiscard]] LIBC_INLINE int register_dll_notification() {
  NTSTATUS st = LdrRegisterDllNotification(0, dll_notification_callback,
                                           nullptr, &g_dll_notify_cookie);
  return NT_SUCCESS(st) ? 0 : 1;
}

// Full startup sweep. Returns 0 on success; non-zero only if the loader
// notification registration failed (the discovery passes themselves cannot
// fail in a way the caller can act on).
[[nodiscard]] LIBC_INLINE int va_inventory_startup_discover() {
  discover_kernel_regions();
  discover_loaded_modules();
  discover_foreign_regions();
  return register_dll_notification();
}

// Cordon stamps survive fork via CoW; only the loader notification cookie
// is process-specific and must be re-registered. Re-registration failure
// is unrecoverable here (no caller path to surface it), so it is silently
// swallowed; subsequent DLL loads in the child go uncordoned.
LIBC_INLINE void va_inventory_fork_reinit() {
  g_dll_notify_cookie = nullptr;
  (void)LdrRegisterDllNotification(0, dll_notification_callback, nullptr,
                                   &g_dll_notify_cookie);
}

// Drop the registration so post-fini LdrLoadDll / LdrUnloadDll calls do
// not re-enter through an unmapped trampoline.
LIBC_INLINE void va_inventory_fini() {
  if (g_dll_notify_cookie != nullptr) {
    LdrUnregisterDllNotification(g_dll_notify_cookie);
    g_dll_notify_cookie = nullptr;
  }
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_VA_INVENTORY_H
