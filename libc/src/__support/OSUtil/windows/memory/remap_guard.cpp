//===-- RemapGuard implementation --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "remap_guard.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/memory/region_snapshot.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// Diagnostic counter: rollback_from_unmapped() re-remap failures.
static cpp::Atomic<uint32_t> g_remap_rollback_failures{0};

uint32_t get_remap_rollback_failures() {
  return g_remap_rollback_failures.load(cpp::MemoryOrder::RELAXED);
}

// =====================================================================
// Construction / Destruction
// =====================================================================

RemapGuard::RemapGuard(void *view_base, SIZE_T guarded_size)
    : view_base_(reinterpret_cast<uintptr_t>(view_base)),
      guarded_size_(guarded_size) {}

RemapGuard::~RemapGuard() {
  if (phase_ != Phase::COMMITTED && phase_ != Phase::DISCARDED)
    rollback();
  release_resources();
}

// =====================================================================
// Phase 0: Prepare
// =====================================================================

bool RemapGuard::prepare() {
  HANDLE process = NtCurrentProcess();
  void *vb = reinterpret_cast<void *>(view_base_);

  // Lock the mapping table entry and arm the VEH remap guard.
  if (!g_mapping_table.begin_remap(vb, guarded_size_, &entry_))
    return false;

  // Duplicate section handle for rollback independence.
  if (entry_.spec.section) {
    NTSTATUS st = ::NtDuplicateObject(
        process, entry_.spec.section, process, &owned_section_,
        0, 0, DUPLICATE_SAME_ACCESS);
    if (NT_ERROR(st)) {
      g_mapping_table.abort_remap(vb);
      return false;
    }
  }

  // Snapshot protections + COW for the full guarded range.
  rec_count_ = snapshot_regions(view_base_, view_base_ + guarded_size_,
                                records_, MAX_RECORDS);
  if (LIBC_UNLIKELY(rec_count_ < 0)) {
    g_mapping_table.abort_remap(vb);
    return false;
  }

  if (!cow_.prepare(view_base_, records_, rec_count_)) {
    g_mapping_table.abort_remap(vb);
    return false;
  }

  phase_ = Phase::PREPARED;
  return true;
}

// =====================================================================
// Phase 1: Unmap
// =====================================================================

bool RemapGuard::unmap() {
  HANDLE process = NtCurrentProcess();
  void *vb = reinterpret_cast<void *>(view_base_);

  NTSTATUS st = ::NtUnmapViewOfSectionEx(
      process, vb,
      MEM_UNMAP_WITH_TRANSIENT_BOOST | MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
  if (NT_ERROR(st))
    return false; // ~RemapGuard handles rollback from PREPARED.

  phase_ = Phase::UNMAPPED;
  return true;
}

// =====================================================================
// Terminal: commit
// =====================================================================

void RemapGuard::commit(void *new_base, SIZE_T size, ViewSpec spec) {
  void *vb = reinterpret_cast<void *>(view_base_);
  g_mapping_table.commit_remap(vb, new_base, size, spec);
  phase_ = Phase::COMMITTED;
}

// =====================================================================
// Terminal: discard
// =====================================================================

void RemapGuard::discard() {
  void *vb = reinterpret_cast<void *>(view_base_);
  g_mapping_table.discard_remap(vb);
  phase_ = Phase::DISCARDED;
}

// =====================================================================
// Rollback
// =====================================================================

void RemapGuard::rollback() {
  switch (phase_) {
  case Phase::INIT:
    return;
  case Phase::PREPARED:
    rollback_from_prepared();
    return;
  case Phase::UNMAPPED:
    rollback_from_unmapped();
    return;
  case Phase::COMMITTED:
  case Phase::DISCARDED:
    return;
  }
}

void RemapGuard::rollback_from_prepared() {
  g_mapping_table.abort_remap(reinterpret_cast<void *>(view_base_));
}

void RemapGuard::rollback_from_unmapped() {
  HANDLE process = NtCurrentProcess();
  HANDLE section = owned_section_ ? owned_section_ : entry_.spec.section;

  // Unmap everything the caller remapped back to placeholders.
  // Walk the guarded range via MBI instead of relying on a tracked list —
  // this handles any number of sub-remaps without a fixed-size limit.
  {
    uintptr_t scan = view_base_;
    uintptr_t end = view_base_ + guarded_size_;
    while (scan < end) {
      MEMORY_BASIC_INFORMATION mbi;
      if (!query_region(reinterpret_cast<void *>(scan), mbi))
        break;
      uintptr_t region_end =
          reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
      if (region_end > end)
        region_end = end;
      // MEM_MAPPED = section view the caller remapped into the placeholder.
      // MEM_PRIVATE + MEM_COMMIT = committed region (e.g. COW dirty pages).
      // MEM_RESERVE with MEM_RESERVE_PLACEHOLDER = already a placeholder.
      if (mbi.Type == MEM_MAPPED) {
        ::NtUnmapViewOfSectionEx(process, mbi.BaseAddress,
                                 MEM_PRESERVE_PLACEHOLDER_ON_UNMAP);
      } else if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) {
        // Decommit + preserve as placeholder.
        PVOID base = mbi.BaseAddress;
        SIZE_T sz = region_end - reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        ::NtFreeVirtualMemory(process, &base, &sz,
                              MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
      }
      scan = region_end;
    }
  }

  // Coalesce all placeholders back into one covering the full guarded range.
  // Always needed after unmap — even a single placeholder must be coalesced
  // if splits occurred, and coalesce on a single unsplit placeholder is a
  // harmless no-op.
  PVOID coal_base = reinterpret_cast<void *>(view_base_);
  SIZE_T coal_size = guarded_size_;
  ::NtFreeVirtualMemory(process, &coal_base, &coal_size,
                        MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);

  // Re-remap the original view from the owned section handle.
  if (section) {
    PVOID base = reinterpret_cast<void *>(view_base_);
    SIZE_T size = guarded_size_;
    LARGE_INTEGER offset = entry_.spec.offset;

    NTSTATUS st = ::NtMapViewOfSectionEx(
        section, process, &base, &offset, &size,
        MEM_REPLACE_PLACEHOLDER, entry_.spec.prot, nullptr, 0);

    if (NT_SUCCESS(st)) {
      // Restore per-page protections.
      if (rec_count_ > 0)
        replay_protections(process, view_base_, entry_.spec.prot,
                           records_, rec_count_);
      // Restore COW page content.
      cow_.restore(view_base_, records_, rec_count_);

      g_mapping_table.abort_remap(reinterpret_cast<void *>(view_base_));
      return;
    }
  }

  // Re-remap failed or no section — mapping is permanently lost.
  // The VA range remains a placeholder with no backing. Record the
  // failure for crash-dump diagnostics.
  g_remap_rollback_failures.fetch_add(1, cpp::MemoryOrder::RELAXED);
  g_mapping_table.discard_remap(reinterpret_cast<void *>(view_base_));
}

// =====================================================================
// Resource cleanup
// =====================================================================

void RemapGuard::release_resources() {
  // cow_ is CowContext with RAII — no explicit cleanup needed.

  if (owned_section_) {
    ::NtClose(owned_section_);
    owned_section_ = nullptr;
  }
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
