//===-- Program break (brk/sbrk) engine for NT-POSIX ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/brk_state.h"

#include "src/__support/OSUtil/windows/alloc/page_alloc.h"
#include "src/__support/OSUtil/windows/alloc/page_size.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/resource/rlimit_state.h"
#include "src/__support/macros/config.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Initial reservation: one allocation granularity (64 KB).
// Extensions double each time: 64K, 128K, 256K, ...
static constexpr size_t BRK_INITIAL_RESERVE = 65536;

// Maximum single extension. Prevents runaway doubling from reserving
// an absurdly large region in one shot.
static constexpr size_t BRK_MAX_EXTEND = 256 * 1024 * 1024; // 256 MB

// Query the effective RLIMIT_DATA cap in bytes. Returns SIZE_MAX if
// unlimited (RLIM_INFINITY).
static size_t get_rlimit_data_cap() {
  windows::ensure_rlimit_init();
  rlim_t cur = g_pcb.rlimit.limits[RLIMIT_DATA].rlim_cur;
  if (cur == RLIM_INFINITY)
    return SIZE_MAX;
  return static_cast<size_t>(cur);
}

// Try to extend the brk reservation by reserving adjacent VA immediately
// after brk_reserved_end. Returns true on success. The caller must hold
// brk_lock.
static bool extend_reservation(size_t min_needed) {
  size_t grow = g_pcb.brk.next_grow;

  // Grow must cover what's needed but also advance the doubling schedule.
  // Guard against overflow: if doubling would wrap, clamp to max.
  while (grow < min_needed) {
    size_t next = grow * 2;
    if (next <= grow) {
      grow = min_needed;
      break;
    }
    grow = next;
  }

  // Cap single extension.
  if (grow > BRK_MAX_EXTEND)
    grow = BRK_MAX_EXTEND;

  // Ensure granularity alignment.
  size_t granularity = windows::get_alloc_granularity();
  grow = (grow + granularity - 1) & ~(granularity - 1);

  // Clamp to RLIMIT_DATA.
  size_t total_after =
      static_cast<size_t>(g_pcb.brk.reserved_end - g_pcb.brk.base) + grow;
  size_t cap = get_rlimit_data_cap();
  if (total_after > cap) {
    // Trim extension to fit exactly under the cap.
    size_t current_size =
        static_cast<size_t>(g_pcb.brk.reserved_end - g_pcb.brk.base);
    if (current_size >= cap)
      return false;
    grow = cap - current_size;
    // Round down to allocation granularity — partial granules can't be
    // reserved.
    grow &= ~(static_cast<size_t>(windows::get_alloc_granularity()) - 1);
    if (grow == 0)
      return false;
  }

  // Attempt to reserve adjacent VA.
  void *ext = page_reserve_at(g_pcb.brk.reserved_end, grow);
  if (!ext)
    return false;

  g_pcb.brk.reserved_end += grow;

  // Advance doubling schedule (only if we grew by the planned amount).
  if (g_pcb.brk.next_grow < BRK_MAX_EXTEND)
    g_pcb.brk.next_grow *= 2;

  return true;
}

void brk_init() {
  void *base = page_reserve(BRK_INITIAL_RESERVE);
  if (!base)
    return; // brk_base stays null — all brk() calls return 0 (query).

  g_pcb.brk.base = static_cast<char *>(base);
  g_pcb.brk.current.store(static_cast<char *>(base), cpp::MemoryOrder::RELAXED);
  g_pcb.brk.reserved_end = static_cast<char *>(base) + BRK_INITIAL_RESERVE;
  g_pcb.brk.next_grow = BRK_INITIAL_RESERVE;
}

intptr_t sys_brk(void *addr) {
  // Subsystem not initialized — can only query.
  if (!g_pcb.brk.base)
    return 0;

  // Query: return current break (unlocked — informational snapshot).
  if (!addr)
    return reinterpret_cast<intptr_t>(
        g_pcb.brk.current.load(cpp::MemoryOrder::RELAXED));

  g_pcb.brk.lock.lock();

  char *old_brk = g_pcb.brk.current.load(cpp::MemoryOrder::RELAXED);
  auto *new_brk = static_cast<char *>(addr);

  // Cannot shrink below the base.
  if (new_brk < g_pcb.brk.base) {
    g_pcb.brk.lock.unlock();
    return reinterpret_cast<intptr_t>(old_brk);
  }

  if (new_brk > old_brk) {
    // --- Grow ---

    // RLIMIT_DATA check: total brk size after growth.
    size_t new_total = static_cast<size_t>(new_brk - g_pcb.brk.base);
    size_t cap = get_rlimit_data_cap();
    if (new_total > cap) {
      g_pcb.brk.lock.unlock();
      return reinterpret_cast<intptr_t>(old_brk);
    }

    // Extend reservation if new_brk exceeds what we've reserved.
    if (new_brk > g_pcb.brk.reserved_end) {
      size_t shortfall =
          static_cast<size_t>(new_brk - g_pcb.brk.reserved_end);
      // Round up to allocation granularity.
      size_t granularity = windows::get_alloc_granularity();
      shortfall = (shortfall + granularity - 1) & ~(granularity - 1);

      if (!extend_reservation(shortfall)) {
        g_pcb.brk.lock.unlock();
        return reinterpret_cast<intptr_t>(old_brk);
      }
    }

    // Commit pages between old and new break.
    // old_page_end: first uncommitted page boundary.
    // new_page_end: last page boundary that needs to be committed.
    uintptr_t old_page_end =
        windows::align_up_to_page(reinterpret_cast<uintptr_t>(old_brk));
    uintptr_t new_page_end =
        windows::align_up_to_page(reinterpret_cast<uintptr_t>(new_brk));

    if (new_page_end > old_page_end) {
      if (!page_commit(reinterpret_cast<void *>(old_page_end),
                       new_page_end - old_page_end)) {
        g_pcb.brk.lock.unlock();
        return reinterpret_cast<intptr_t>(old_brk);
      }
    }

    g_pcb.brk.current.store(new_brk, cpp::MemoryOrder::RELEASE);

  } else if (new_brk < old_brk) {
    // --- Shrink ---

    // Decommit whole pages that are entirely above the new break.
    uintptr_t new_page_end =
        windows::align_up_to_page(reinterpret_cast<uintptr_t>(new_brk));
    uintptr_t old_page_end =
        windows::align_up_to_page(reinterpret_cast<uintptr_t>(old_brk));

    if (old_page_end > new_page_end) {
      page_decommit(reinterpret_cast<void *>(new_page_end),
                     old_page_end - new_page_end);
    }

    g_pcb.brk.current.store(new_brk, cpp::MemoryOrder::RELEASE);
  }

  char *result = g_pcb.brk.current.load(cpp::MemoryOrder::RELAXED);
  g_pcb.brk.lock.unlock();
  return reinterpret_cast<intptr_t>(result);
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// ===========================================================================
// brk_startup_init() — Phase 4b. After pools but before VEH fault handlers.
// No ordering dependency on fd_table or mem_fault_handler — brk sits on
// page_alloc.h directly. Called by __libc_dll_init().
// ===========================================================================

// Phase 4b startup init — initialize brk subsystem.
// Called by __libc_dll_init() after pool inits.
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

int LIBC_NAMESPACE::internal::brk_startup_init() {
  LIBC_NAMESPACE::internal::brk_init();
  return 0;
}

// Reset brk_lock in fork child. The child inherits the parent's brk VA
// (CoW), but any lock held by a dead parent thread would deadlock.
// Reset is safe: child is single-threaded at this point.

void LIBC_NAMESPACE::internal::brk_fork_reinit() {
  LIBC_NAMESPACE::g_pcb.brk.lock.reset_for_fork();
}

LIBC_REGISTER_FORK_REINIT(brk,
                          ::LIBC_NAMESPACE::internal::kForkPrioBrk,
                          &::LIBC_NAMESPACE::internal::brk_fork_reinit)
