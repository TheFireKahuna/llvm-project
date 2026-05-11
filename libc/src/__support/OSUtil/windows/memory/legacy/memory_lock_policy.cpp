//===-- Memory lock policy state owner ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Owns the process-wide MLOCK_ONFAULT tracking table. The state is created at
// CRT startup when this object file is linked, avoiding both global
// constructors and lazy first-use initialization on the page-fault path.
//
// The MLOCK_ONFAULT VEH filter is declaratively registered into the unified
// VEH dispatch table via `.libcveh` at VEH_PRIORITY_MLOCK; the Tier B Phase 3
// sweep (register_all_static_veh_filters()) installs it after
// mlock_policy_startup_init() has constructed the onfault state, so the
// filter is live before any pool or DLL-load-driven fault can arrive.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/legacy/memory_lock_policy.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/veh/veh_core.h"
#include "src/__support/OSUtil/windows/veh/veh_filter_registry.h"

#include "src/__support/CPP/new.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {

// Storage and accessor are file-scope (internal linkage via static) rather
// than anonymous-namespace so that mlock_policy_fork_reinit() — defined at
// the bottom of this TU — can access them.
alignas(OnfaultState) static unsigned char
    onfault_state_storage[sizeof(OnfaultState)] = {};
static bool onfault_state_constructed = false;

static OnfaultState &get_onfault_state() {
  return *reinterpret_cast<OnfaultState *>(onfault_state_storage);
}

void init_onfault_state() {
  if (onfault_state_constructed)
    return;

  ::new (static_cast<void *>(onfault_state_storage)) OnfaultState{};
  onfault_state_constructed = true;
}

void fini_onfault_state() {
  if (!onfault_state_constructed)
    return;

  get_onfault_state().~OnfaultState();
  onfault_state_constructed = false;
}

bool onfault_arm_range(void *addr, SIZE_T size) {
  return get_onfault_state().arm_range(addr, size);
}

void onfault_disarm_range(void *addr, SIZE_T size) {
  get_onfault_state().disarm_range(addr, size);
}

bool onfault_contains(uintptr_t addr) {
  return get_onfault_state().contains(addr);
}

LONG OnfaultState::onfault_veh(EXCEPTION_POINTERS *ep) {
  if (ep->ExceptionRecord->ExceptionCode !=
      static_cast<DWORD>(STATUS_GUARD_PAGE_VIOLATION))
    return EXCEPTION_CONTINUE_SEARCH;

  uintptr_t fault_addr = reinterpret_cast<uintptr_t>(
      ep->ExceptionRecord->ExceptionInformation[1]);

  if (!onfault_contains(fault_addr))
    return EXCEPTION_CONTINUE_SEARCH;

  // Lock the faulted page. The guard bit is already cleared by the CPU,
  // so the page is accessible. NtLockVirtualMemory pins it in RAM.
  const SIZE_T page_size = get_page_size();
  uintptr_t page_base = fault_addr & ~(page_size - 1);
  PVOID base = reinterpret_cast<PVOID>(page_base);
  SIZE_T region_size = page_size;

  ::NtLockVirtualMemory(NtCurrentProcess(), &base, &region_size, MAP_PROCESS);

  return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

// ---------------------------------------------------------------------------
// VEH filter wrapper for the dispatch table
// ---------------------------------------------------------------------------

static LONG mlock_onfault_filter(EXCEPTION_POINTERS *ep) {
  // No reentry guard --- the master handler already checked it.
  //
  // Self-gate on construction state. The filter pointer lives in the
  // sealed Zone 0 VEH dispatch table and cannot be removed, so it stays
  // live for the full window [Tier B Phase 3 sweep, veh_core $P0 fini].
  // A fault arriving before mlock_policy_startup_init() (Tier B pre-Phase
  // 3) or after fini_onfault_state() ($P5 fini, before $P0 removes the
  // master VEH) would otherwise read a zero- or zombie-initialised
  // OnfaultState. Fall through to the next filter in both cases.
  if (!LIBC_NAMESPACE::windows::onfault_state_constructed)
    return EXCEPTION_CONTINUE_SEARCH;
  return LIBC_NAMESPACE::windows::OnfaultState::onfault_veh(ep);
}

// ---------------------------------------------------------------------------
// Subsystem init / fini
// ---------------------------------------------------------------------------

int LIBC_NAMESPACE::internal::mlock_policy_startup_init() {
  // Filter registration is handled declaratively via the .libcveh record
  // below; Tier B Phase 3 sweeps it after this call returns.
  LIBC_NAMESPACE::windows::init_onfault_state();
  return 0;
}

void LIBC_NAMESPACE::internal::mlock_policy_fork_reinit() {
  // POSIX: memory locks are not inherited across fork(). Clear both the
  // mlockall flag word and the MLOCK_ONFAULT range table so the child
  // starts with no active policy.
  //
  // PAGE_GUARD bits on previously-armed pages do survive the address-space
  // clone, but with the table empty OnfaultState::contains() returns false
  // and the VEH filter continues the search; the first access on each such
  // page clears PAGE_GUARD in the kernel, so the residue is self-healing.
  //
  // The RW lock reset is still required: the parent could have forked with
  // a writer holding the lock or readers active. reset_for_fork skips
  // drain_waiters() to avoid alerting parent TIDs.
  LIBC_NAMESPACE::windows::g_mcl_flags.store(0, cpp::MemoryOrder::RELAXED);
  auto &st = LIBC_NAMESPACE::windows::get_onfault_state();
  st.rw_state.reset_for_fork(0);
  st.count = 0;
  // The VEH filter registration is handled by veh_core_fork_reinit()
  // (the filter function pointer survives the address space clone).
}

LIBC_REGISTER_FINI(5, mlock_policy,
                   &::LIBC_NAMESPACE::windows::fini_onfault_state)

// ---------------------------------------------------------------------------
// Static VEH filter record --- picked up by register_all_static_veh_filters()
// ---------------------------------------------------------------------------
LIBC_REGISTER_VEH_FILTER(mlock_policy,
                         ::LIBC_NAMESPACE::windows::VEH_GUARD_PAGE,
                         &mlock_onfault_filter,
                         ::LIBC_NAMESPACE::windows::VEH_PRIORITY_MLOCK)

LIBC_REGISTER_FORK_REINIT(mlock_policy,
                          ::LIBC_NAMESPACE::internal::kForkPrioMlockPolicy,
                          &::LIBC_NAMESPACE::internal::mlock_policy_fork_reinit)
