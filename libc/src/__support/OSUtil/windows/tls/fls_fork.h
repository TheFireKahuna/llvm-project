//===-- FLS state repair for fork child -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// fls_fork_reinit() — repairs ntdll Fiber Local Storage data structures in the
// fork child so that Win32 FLS slot values survive fork and the internal FLS
// linked list is clean.
//
// Prerequisites: RtlCompleteProcessCloning(TRUE) must have already run in
// the child.  It resets all ntdll-internal locks (including the FLS SRWLock
// in FLS_MANAGER) to their unlocked state.  This function handles the DATA
// repair that Complete does not: cleaning the stale FLS linked list and
// wiring up the child's TEB->FlsData.
//
// Problem:  NtCreateUserProcess (clone mode) gives the child a fresh TEB
// with FlsData = NULL, discarding all DLL-owned FLS slot values.  The
// process-wide FLS linked list (in ntdll's private FLS_MANAGER) is a
// stale COW copy containing entries for parent threads that don't exist
// in the child.
//
// Solution (runs single-threaded in the fork child, after Complete):
//
//   1. Walk the parent's COW FLS_DATA linked list to find the sentinel
//      (FLS_MANAGER::ListHead), identified by its address falling within
//      ntdll's image range.
//
//   2. Derive FLS_MANAGER base from ListHead offset (validated at runtime
//      via static_assert'd struct layout + range checks).
//
//   3. Reinitialize ListHead as an empty circular list (drops all stale
//      entries from parent threads without invoking callbacks or freeing
//      COW memory).
//
//   4. Reuse the parent's COW FLS_DATA block directly — writing to its
//      Link field triggers a COW page split, giving the child its own
//      writable copy.  Bucket pointers inside still reference COW-valid
//      slot arrays; future RtlFlsSetValue calls trigger independent
//      COW page splits for the affected slots.  No heap allocation needed.
//
//   5. Link the COW'd block as the sole list entry and set TEB->FlsData.
//
// Graceful degradation: if any step fails (can't find sentinel, offset
// validation fails), the function returns without modifying state.
// All subsystems tolerate FlsData = NULL — the repair is an enhancement,
// not a requirement.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TLS_FLS_FORK_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TLS_FLS_FORK_H

#include "src/__support/OSUtil/windows/nt/nt_peb.h"
#include "src/__support/OSUtil/windows/nt/nt_process_api.h"
#include "src/__support/OSUtil/windows/nt/nt_process_types.h"
#include "src/__support/macros/config.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace windows {

// ---------------------------------------------------------------------------
// ntdll image range — used to distinguish the FLS list sentinel (in ntdll's
// .data section) from heap-allocated RTL_FLS_DATA blocks.
// ---------------------------------------------------------------------------

struct NtdllRange {
  uintptr_t base;
  uintptr_t end;
};

/// Walk the PEB loader module list to find ntdll's base address and size.
/// ntdll is always the first entry in InInitializationOrderLinks (it
/// initializes before everything else), but InLoadOrderLinks is more
/// conventional — ntdll is typically the second entry (after the EXE).
/// We match by checking DllBase against any known ntdll export address.
LIBC_INLINE NtdllRange get_ntdll_range() {
  // RtlAllocateHeap is in ntdll — use its address as a probe.
  auto probe = reinterpret_cast<uintptr_t>(&::RtlAllocateHeap);

  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
  for (LIST_ENTRY *cur = head->Flink; cur != head; cur = cur->Flink) {
    auto *entry = reinterpret_cast<LDR_DATA_TABLE_ENTRY *>(cur);
    auto mod_base = reinterpret_cast<uintptr_t>(entry->DllBase);
    auto mod_end = mod_base + entry->SizeOfImage;
    if (probe >= mod_base && probe < mod_end)
      return {mod_base, mod_end};
  }
  return {0, 0}; // should never happen
}

// ---------------------------------------------------------------------------
// List-walk sentinel discovery
// ---------------------------------------------------------------------------

/// Walk the circular FLS linked list starting from a parent thread's
/// COW RTL_FLS_DATA block.  The sentinel is the LIST_ENTRY node that
/// resides within ntdll's image (FLS_MANAGER::ListHead) rather than
/// on the process heap.
///
/// Returns the sentinel address, or nullptr if not found (corruption
/// or ntdll layout changed).
LIBC_INLINE LIST_ENTRY *find_fls_list_head(RTL_FLS_DATA *parent_fls,
                                           NtdllRange ntdll) {
  LIST_ENTRY *start = &parent_fls->Link;
  LIST_ENTRY *cur = start->Flink;

  // Cap iteration to prevent infinite loops on a corrupted list.
  for (unsigned i = 0; i < 4096 && cur != start; ++i) {
    auto addr = reinterpret_cast<uintptr_t>(cur);
    if (addr >= ntdll.base && addr < ntdll.end)
      return cur; // found the sentinel — it's in ntdll .data
    cur = cur->Flink;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// FLS_MANAGER validation
// ---------------------------------------------------------------------------

/// Given the sentinel (FLS_MANAGER::ListHead), compute the FLS_MANAGER
/// base and validate that the derived layout is plausible.
///
/// Checks:
///   - Manager base address is within ntdll's image
///   - Lock value looks like an SRWLOCK (small integer, not a pointer)
///   - AllocBuckets entries are either NULL or outside ntdll (heap pointers)
LIBC_INLINE FLS_MANAGER *
validate_fls_manager(LIST_ENTRY *list_head, NtdllRange ntdll) {
  // Derive the manager base from the known offset of ListHead.
  auto *mgr = reinterpret_cast<FLS_MANAGER *>(
      reinterpret_cast<char *>(list_head) -
      FIELD_OFFSET(FLS_MANAGER, ListHead));

  // The manager must reside within ntdll's image.
  auto mgr_addr = reinterpret_cast<uintptr_t>(mgr);
  if (mgr_addr < ntdll.base || mgr_addr >= ntdll.end)
    return nullptr;

  // SRWLOCK value: 0 when unlocked, small flag bits when locked.
  // RtlCompleteProcessCloning(TRUE) should have reset it to 0.
  // If it looks like a pointer (large value), the offset is wrong.
  auto lock_val = reinterpret_cast<uintptr_t>(mgr->Lock.Ptr);
  if (lock_val > 0xFFFF)
    return nullptr;

  // AllocBuckets are per-bucket callback+allocation tables, heap-allocated.
  // Should be NULL (unused bucket) or outside ntdll.  If any points
  // inside ntdll, the layout is wrong.
  for (unsigned i = 0; i < FLS_BUCKET_COUNT; ++i) {
    if (!mgr->AllocBuckets[i])
      continue;
    auto baddr = reinterpret_cast<uintptr_t>(mgr->AllocBuckets[i]);
    if (baddr >= ntdll.base && baddr < ntdll.end)
      return nullptr;
  }

  return mgr;
}

// ---------------------------------------------------------------------------
// Core FLS fork reinit
// ---------------------------------------------------------------------------

/// Repair FLS data structures in the fork child.  Must be called single-
/// threaded, AFTER RtlCompleteProcessCloning(TRUE) has reset ntdll's
/// internal locks, and before any code that might invoke ntdll FLS
/// operations (e.g., kernel32 internals triggered by the reinit chain).
///
/// parent_teb_ptr — the parent thread's TEB address, saved before clone.
///                  Points to COW-mapped pages in the child.
LIBC_INLINE void fls_fork_reinit(void *parent_teb_ptr) {
  auto *parent_teb = static_cast<TEB *>(parent_teb_ptr);
  auto *parent_fls = static_cast<RTL_FLS_DATA *>(parent_teb->FlsData);

  // Parent had no FLS state — nothing to preserve.
  if (!parent_fls)
    return;

  // --- Step 1: Find the FLS list sentinel in ntdll ---
  NtdllRange ntdll = get_ntdll_range();
  if (!ntdll.base)
    return;

  LIST_ENTRY *list_head = find_fls_list_head(parent_fls, ntdll);
  if (!list_head)
    return;

  // --- Step 2: Validate and derive FLS_MANAGER ---
  FLS_MANAGER *mgr = validate_fls_manager(list_head, ntdll);
  if (!mgr)
    return;

  // --- Step 3: Reinitialize ListHead as empty ---
  //
  // Drops all stale RTL_FLS_DATA entries from parent threads.  Those
  // blocks are COW pages that will never be freed — they're abandoned
  // in the child's address space and reclaimed at process exit.
  //
  // The FLS SRWLock was already reset to unlocked by
  // RtlCompleteProcessCloning(TRUE) earlier in fork_child_post.
  mgr->ListHead.Flink = &mgr->ListHead;
  mgr->ListHead.Blink = &mgr->ListHead;

  // --- Step 4: Reuse parent's COW FLS_DATA ---
  //
  // The parent's RTL_FLS_DATA at parent_fls is a valid heap block in
  // COW memory.  Writing to its Link field triggers a COW page split —
  // the child gets its own writable copy of the page.  The bucket
  // pointers inside still reference COW-valid slot arrays containing
  // the parent thread's FLS slot values; future RtlFlsSetValue calls
  // trigger independent COW page splits for the affected slots.
  //
  // No RtlAllocateHeap needed.  The heap manager's COW'd metadata
  // still tracks this block as allocated, so future
  // RtlProcessFlsData(DEALLOCATE) can free it correctly.  Both parent
  // and child can independently free their respective COW copies.
  parent_fls->Link.Flink = &mgr->ListHead;
  parent_fls->Link.Blink = &mgr->ListHead;
  mgr->ListHead.Flink = &parent_fls->Link;
  mgr->ListHead.Blink = &parent_fls->Link;

  // --- Step 5: Set child's TEB->FlsData ---
  NtCurrentTeb()->FlsData = parent_fls;
}

} // namespace windows
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TLS_FLS_FORK_H
