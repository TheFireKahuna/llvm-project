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
// The MLOCK_ONFAULT VEH filter is registered into the unified VEH dispatch
// table (veh/veh_core.h) at VEH_PRIORITY_MLOCK during mlock_policy_startup_init() (Phase 5).
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/memory_lock_policy.h"
#include "src/__support/OSUtil/windows/libc_subsystem_init.h"
#include "src/__support/OSUtil/windows/veh/veh_core.h"

#include "src/__support/CPP/new.h"

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

LONG WINAPI OnfaultState::onfault_veh(EXCEPTION_POINTERS *ep) {
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

static LONG NTAPI mlock_onfault_filter(EXCEPTION_POINTERS *ep) {
  // No reentry guard --- the master handler already checked it.
  return LIBC_NAMESPACE::windows::OnfaultState::onfault_veh(ep);
}

// ---------------------------------------------------------------------------
// Subsystem init / fini
// ---------------------------------------------------------------------------

int LIBC_NAMESPACE::internal::mlock_policy_startup_init() {
  LIBC_NAMESPACE::windows::init_onfault_state();

  LIBC_NAMESPACE::windows::VehFilter filter;
  filter.exception_mask = LIBC_NAMESPACE::windows::VEH_GUARD_PAGE;
  filter.handler = mlock_onfault_filter;
  filter.priority = LIBC_NAMESPACE::windows::VEH_PRIORITY_MLOCK;
  LIBC_NAMESPACE::windows::register_veh_filter(filter);

  return 0;
}

void LIBC_NAMESPACE::internal::mlock_policy_startup_fini() {
  LIBC_NAMESPACE::windows::fini_onfault_state();
}

void LIBC_NAMESPACE::internal::mlock_policy_fork_reinit() {
  // Reset the onfault RW lock: both the futex value and the Treiber wait
  // stack. Parent waiters don't exist in the child. The tracked ranges
  // themselves are inherited (address space clone) and remain valid.
  LIBC_NAMESPACE::windows::get_onfault_state().rw_state.reset_for_fork(0);
  // The VEH filter registration is handled by veh_core_fork_reinit()
  // (the filter function pointer survives the address space clone).
}
