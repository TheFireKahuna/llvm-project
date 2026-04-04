//===-- RemapGuard implementation --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "remap_guard.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/CPP/utility.h"
#include "src/__support/OSUtil/windows/alloc/thread_scratch.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives.h"
#include "src/__support/OSUtil/windows/memory/region_pool.h"
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
}

RemapGuard::RemapGuard(RemapGuard &&other)
    : view_base_(other.view_base_), guarded_size_(other.guarded_size_),
      entry_(other.entry_), region_(other.region_),
      records_(cpp::move(other.records_)), rec_count_(other.rec_count_),
      cow_(cpp::move(other.cow_)), phase_(other.phase_) {
  other.region_ = nullptr;
  other.phase_ = Phase::DISCARDED;
}

RemapGuard &RemapGuard::operator=(RemapGuard &&other) {
  if (this != &other) {
    if (phase_ != Phase::COMMITTED && phase_ != Phase::DISCARDED)
      rollback();
    view_base_ = other.view_base_;
    guarded_size_ = other.guarded_size_;
    entry_ = other.entry_;
    region_ = other.region_;
    records_ = cpp::move(other.records_);
    rec_count_ = other.rec_count_;
    cow_ = cpp::move(other.cow_);
    phase_ = other.phase_;
    other.region_ = nullptr;
    other.phase_ = Phase::DISCARDED;
  }
  return *this;
}

// =====================================================================
// Phase 0: Prepare
// =====================================================================

bool RemapGuard::prepare(bool *out_stale) {
  if (out_stale)
    *out_stale = false;
  void *vb = reinterpret_cast<void *>(view_base_);

  // Lock the mapping table entry and arm the VEH remap guard.
  if (!g_mapping_table.begin_remap(vb, guarded_size_, &entry_, out_stale))
    return false;

  // Resolve the region descriptor. The slot is REMAPPING and still owns
  // its +1 reference, so the region is pinned until commit / abort /
  // discard — no add_ref needed here. resolve() validates alloc_id.
  region_ = memory::g_region_pool.resolve(entry_.region_id, entry_.alloc_id);
  if (region_ == nullptr && entry_.region_id != memory::RegionPool::NONE) {
    // Concurrent pool reuse — should not be possible under REMAPPING,
    // but fail closed rather than dereference a null region.
    g_mapping_table.abort_remap(vb, entry_);
    return false;
  }

  // Allocate snapshot buffer from thread-local scratch arena.
  records_ = internal::ScratchAlloc<RegionRecord>(MAX_REGION_RECORDS);
  if (!records_) {
    g_mapping_table.abort_remap(vb, entry_);
    return false;
  }

  // Snapshot protections for the full guarded range. Needed for replay
  // after remap regardless of CoW — mprotect differences must round-trip.
  rec_count_ = snapshot_regions(view_base_, view_base_ + guarded_size_,
                                records_.data(), MAX_REGION_RECORDS);
  if (LIBC_UNLIKELY(rec_count_ < 0)) {
    g_mapping_table.abort_remap(vb, entry_);
    return false;
  }

  // CoW save: required when the region's section is PAGE_WRITECOPY, i.e.
  // dirty private pages would be lost on unmap. The region flag is the
  // source of truth — it captures the section's allocation protect at
  // acquire time. As a safety net (in case an unported caller forgets to
  // set the flag), fall back to the per-page MBI scan via has_cow(); the
  // records are already in hand, so this is cheap. Skip only when both
  // signals say "no CoW pages here".
  const bool region_says_cow =
      region_ != nullptr && region_->has_flag(memory::region_flag::COW);
  const bool records_say_cow = has_cow(records_.data(), rec_count_);
  if (region_says_cow || records_say_cow) {
    if (!cow_.prepare(view_base_, records_.data(), rec_count_)) {
      g_mapping_table.abort_remap(vb, entry_);
      return false;
    }
  }

  phase_ = Phase::PREPARED;
  return true;
}

// =====================================================================
// Phase 1: Unmap
// =====================================================================

bool RemapGuard::unmap() {
  void *vb = reinterpret_cast<void *>(view_base_);

  NTSTATUS st = unmap_view_preserve_transient(vb);
  if (NT_ERROR(st))
    return false; // ~RemapGuard handles rollback from PREPARED.

  phase_ = Phase::UNMAPPED;
  return true;
}

// =====================================================================
// Terminal: commit
// =====================================================================

bool RemapGuard::commit(void *new_base, SIZE_T size, uint32_t region_id,
                        uint8_t alloc_id, DWORD prot, DWORD flags) {
  void *vb = reinterpret_cast<void *>(view_base_);
  if (!g_mapping_table.commit_remap(vb, new_base, size, region_id, alloc_id,
                                    prot, flags))
    return false; // Stay non-terminal so ~RemapGuard rolls back.
  phase_ = Phase::COMMITTED;
  return true;
}

// =====================================================================
// Terminal: discard
// =====================================================================

void RemapGuard::discard() {
  void *vb = reinterpret_cast<void *>(view_base_);
  g_mapping_table.discard_remap(vb);
  phase_ = Phase::DISCARDED;
  // discard_remap released the slot's region ref; drop our handle-read
  // alias to match.
  region_ = nullptr;
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
  // Region ref is still held by the slot; abort_remap restores the
  // original MappingEntry fields. No add_ref / release dance.
  g_mapping_table.abort_remap(reinterpret_cast<void *>(view_base_), entry_);
}

void RemapGuard::rollback_from_unmapped() {
  HANDLE process = NtCurrentProcess();
  // Section handle comes from the resolved region. Safe to read because
  // the slot still holds its +1 ref on the region (REMAPPING state).
  HANDLE section =
      region_ != nullptr ? region_->section_handle : static_cast<HANDLE>(nullptr);

  // Undo whatever the caller laid over the guarded range.
  //
  // The caller (typically RemapTransaction::remap_with_cow_splits) may have
  // laid down an unbounded number of MEM_MAPPED sub-views interleaved with
  // MEM_PRIVATE committed sub-ranges — one pair per dirty/clean transition
  // in the CoW bitmap. The mapping table never sees these (slot is REMAPPING,
  // still holds the original entry), so we must discover them from NT.
  //
  // RegionWalker drives a paginated NtPssCaptureVaSpaceBulk over the bound,
  // which returns ~80 MBI entries per 4 KB fetch — one syscall covers the
  // common case (3-5 fragments) and pathological CoW fan-out scales as
  // O(fragments / 80) syscalls instead of O(fragments). Cold path either
  // way; the bulk form keeps a uniform query primitive across the memory
  // subsystem (the only remaining iterative MBI consumer was here).
  //
  // Scratch failure means we cannot discover sub-mappings — without that,
  // the coalesce + remap below would fail too. Skip straight to discard.
  bool walk_ok = false;
  {
    auto ws = byte_scratch(4096);
    if (ws) {
      void *bound = reinterpret_cast<void *>(view_base_);
      RegionWalker walk(bound, guarded_size_, ws.data(), ws.size());
      while (walk.next()) {
        if (walk.entry->Type == MEM_MAPPED) {
          unmap_view_preserve(walk.entry->BaseAddress);
        } else if (walk.entry->State == MEM_COMMIT &&
                   walk.entry->Type == MEM_PRIVATE) {
          preserve_to_placeholder(walk.chunk, walk.chunk_size);
        }
      }
      walk_ok = true;
    }
  }

  if (walk_ok) {
    // Coalesce all placeholders back into one covering the full guarded
    // range. Harmless when no split actually happened (single placeholder
    // coalesce is a no-op).
    PVOID coal_base = reinterpret_cast<void *>(view_base_);
    SIZE_T coal_size = guarded_size_;
    ::NtFreeVirtualMemory(process, &coal_base, &coal_size,
                          MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);

    // Re-remap the original view from the region's section handle. For
    // anonymous regions (no section) there is nothing to re-map — the
    // placeholder is the original state and rollback becomes discard.
    if (section != nullptr && region_ != nullptr) {
      PVOID base = reinterpret_cast<void *>(view_base_);
      SIZE_T size = guarded_size_;
      LARGE_INTEGER offset = region_->section_offset;

      NTSTATUS st = ::NtMapViewOfSectionEx(
          section, process, &base, &offset, &size, MEM_REPLACE_PLACEHOLDER,
          entry_.view_prot, nullptr, 0);

      if (NT_SUCCESS(st)) {
        if (rec_count_ > 0)
          replay_protections(process, view_base_, entry_.view_prot,
                             records_.data(), rec_count_);
        cow_.restore(view_base_, records_.data(), rec_count_);
        g_mapping_table.abort_remap(reinterpret_cast<void *>(view_base_),
                                    entry_);
        return;
      }
    }
  }

  // Re-remap failed (or no section to remap from, or scratch alloc failed) —
  // mapping is permanently lost. Record for crash-dump diagnostics.
  g_remap_rollback_failures.fetch_add(1, cpp::MemoryOrder::RELAXED);
  g_mapping_table.discard_remap(reinterpret_cast<void *>(view_base_));
  region_ = nullptr;
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
