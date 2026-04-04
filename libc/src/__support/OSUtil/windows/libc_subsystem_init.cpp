//===-- Tier B libc subsystem initialization for c.dll -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// __libc_dll_init() — Tier B subsystem bring-up. Runs after
// __libc_bootstrap() has brought the process into a state where code can
// execute safely (PCB Zone 0 sealed, master VEH online, identity set).
//
// Replaces the former .CRT$XI* section-walked function-pointer dispatch
// with explicit, source-visible function calls. All init ordering is
// visible in this single function. Dependencies are documented by phase.
// No function-pointer arrays, no indirect dispatch, no section letter
// fragility.
//
// Tier B covers: allocator, memory fault filters, object pools, brk,
// fd table, reactor, signal dispatch, ALPC bus, stdio + console. This
// is everything a user program expects ready by the time main() runs
// that wasn't already set up in Tier A.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/libc_subsystem_init.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/lazy_init.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/OSUtil/windows/memory/va_inventory.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/veh/veh_filter_registry.h"
#include "src/__support/macros/config.h"
#include "src/stdlib/windows/posix_alloc.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

// Tri-state init gate, file-scope so __libc_dll_fini can poison it.
//   0 = UNINIT (Tier B not yet run)
//   1 = READY  (Tier B in progress or done)
//   2 = POISONED (FreeLibrary fini ran — re-attach forbidden)
namespace {
constexpr int kSubsystemsUninit = 0;
constexpr int kSubsystemsReady = 1;
constexpr int kSubsystemsPoisoned = 2;
LIBC_NAMESPACE::cpp::Atomic<int> g_subsystems_up{kSubsystemsUninit};
} // namespace

// Poison hook called from __libc_dll_fini in libc_subsystem_fini.cpp on
// FreeLibrary teardown. Once latched, any subsequent LoadLibrary("c.dll")
// + _DllMainCRTStartup → __libc_dll_init() returns failure rather than
// silently skipping Tier B over torn-down pools.
namespace LIBC_NAMESPACE_DECL {
namespace internal {
void mark_dll_init_poisoned() {
  g_subsystems_up.store(kSubsystemsPoisoned, cpp::MemoryOrder::RELEASE);
}

bool dll_init_is_poisoned() {
  return g_subsystems_up.load(cpp::MemoryOrder::ACQUIRE) == kSubsystemsPoisoned;
}
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Defined in libc_subsystem_fini.cpp. Reverse-order subsystem teardown that
// is documented no-op-safe for any subsystem whose init didn't run, so it
// can be invoked safely from a partial Tier B failure path. Also poisons the
// init gate so a retry attempt is rejected.
extern "C" void __libc_dll_fini();

namespace {
// Tear down any subsystems that completed before this failure, then return
// the failure code. Without this, a partial Tier B leaves live resources
// (reactor drain thread + IOCP, ALPC port, slab pool VA reservations,
// mlock onfault state) attached to a DLL the loader is about to unload —
// the drain thread would crash on the first instruction fetch from
// unmapped code. The reverse-order fini sequence in __libc_dll_fini
// closes those resources cleanly.
[[gnu::cold]] int fail_partial_init() {
  __libc_dll_fini();
  return 1;
}
} // namespace

extern "C" int __libc_dll_init() {
  using namespace LIBC_NAMESPACE;
  using namespace LIBC_NAMESPACE::internal;

  // Idempotent gate. Two entry points reach this:
  //   1. c.dll _DllMainCRTStartup on DLL_PROCESS_ATTACH (normal apps).
  //   2. __libc_init() fallback for freestanding exes with no c.dll
  //      (libc test-suite standalone executables).
  //
  // ACQ_REL CAS publishes the UNINIT→READY transition with a release fence
  // (so a spinning loser observes Tier B's writes when it eventually reads
  // pcb_init_state with ACQUIRE) and gives the loser an acquire fence on
  // its read of the prior winner's stores.
  //
  // POISONED is sticky: once __libc_dll_fini ran in this address space the
  // remaining pools/handles are inconsistent and re-running Tier B would
  // build on top of torn-down state. Refuse re-attach.
  int prev = kSubsystemsUninit;
  if (!g_subsystems_up.compare_exchange_strong(prev, kSubsystemsReady,
                                               cpp::MemoryOrder::ACQ_REL,
                                               cpp::MemoryOrder::ACQUIRE)) {
    if (prev == kSubsystemsPoisoned)
      return 1;
    return 0; // Already initialised by an earlier caller.
  }

  // Precondition: __libc_bootstrap() has run. It performs the OS floor
  // check, brings up PCB Zone 0, installs the master VEH, sets identity,
  // and seals Zone 0. Both EXE-entry and DllMain paths call it before
  // reaching here.

  // =====================================================================
  // Phase 3: Allocator subsystem
  //   posix_alloc_init needs page_size. Must complete before any slab
  //   pool or malloc use.
  //
  //   mmap_subsystem_init used to live here (kernel/PEB/stack stamping
  //   + bulk-VAD sweep + DLL notification registration). All of that
  //   work has moved into Tier A Phase 0c.5 — the `.libcmem` walker
  //   brings up the substrate + mapping table, then runs
  //   va_inventory_startup_discover — so the mapping table is the
  //   single source of truth from the moment it comes online; no Tier
  //   B window during which user-visible VA would be misclassified.
  //
  //   mlock_policy_startup_init and the static VEH filter sweep have
  //   moved to Tier A — the master VEH and the entire .libcveh filter
  //   table are live before _DllMainCRTStartup returns control here.
  //   Faults during Phase 3 bring-up route through the sealed dispatch
  //   table (mem_fault, mlock_policy, signal_veh_transport).
  // =====================================================================
  if (posix_alloc_init() != 0)
    return fail_partial_init();

  // =====================================================================
  // Phase 4: Object pools
  //   All slab pool inits. No inter-pool dependencies.
  //   Each reserves VA and allocates TLS slots.
  // =====================================================================
  if (lifecycle_startup_init() != 0)
    return fail_partial_init();
  if (ofd_pool_startup_init() != 0)
    return fail_partial_init();
  if (file_pool_startup_init() != 0)
    return fail_partial_init();
  if (wait_slot_startup_init() != 0)
    return fail_partial_init();
  // named_semaphore eagerly brought up pre-#30; now lazy-gated at first
  // sem_open(). If no TU ever links a named-semaphore syscall, the
  // g_sem_pool SlabPool + TLS slot + SipHash-key state all stay cold.
  if (robust_pool_startup_init() != 0)
    return fail_partial_init();
  if (thread_ring_startup_init() != 0)
    return fail_partial_init();
  if (thread_storage_startup_init() != 0)
    return fail_partial_init();
  if (brk_startup_init() != 0)
    return fail_partial_init();

  // =====================================================================
  // Phase 6: Fd table
  //   Chunk 0 commit. Depends on pools + VEH fault handlers.
  // =====================================================================
  if (fd_table_startup_init() != 0)
    return fail_partial_init();

  // =====================================================================
  // Phase 7: Services
  //   Reactor creates IOCP + drain thread.
  //   Signal / lock-table subsystems register their ALPC opcode handlers
  //   (no network state yet — just static table writes).
  //   alpc_bus_startup_init brings up the per-process ALPC port LAST so
  //   the dispatch table is already complete when the first connection
  //   request could arrive.
  //   NtCreateThreadEx uses SKIP_THREAD_ATTACH to avoid loader deadlock.
  // =====================================================================
  if (reactor_startup_init() != 0)
    return fail_partial_init();
  if (socket_io_startup_init() != 0)
    return fail_partial_init();
  if (signal_startup_init() != 0)
    return fail_partial_init();
  if (lock_table_startup_init() != 0)
    return fail_partial_init();
  if (alpc_bus_startup_init() != 0)
    return fail_partial_init();

  // =====================================================================
  // Phase 8: Stdio
  //   Bind stdin/stdout/stderr from PEB handles.
  //   Depends on signal being live (SIGHUP delivery for console hangup).
  //
  // vt_pty and console used to eagerly init here. Both are now lazy-gated:
  //   - vt_pty inherited-PTY adoption fires on first
  //     pty_tree::current_attached_pty_id / has_current_attached_pty.
  //   - console PEB cookie sync fires on first console::current_reference.
  // Both gates are swept by `.libclzr` during exec_self_hollow().
  // =====================================================================
  if (fd_table_std_fds_startup_init() != 0)
    return fail_partial_init();

  // PCB Zone 0 is already sealed PAGE_READONLY by __libc_bootstrap().
  // Any Tier B phase above that tries to write Zone 0 fields (page_size,
  // cookie, pid, parent_pid, nt_build, caps) will fault — by design.

  // Publish Tier B completion. RELEASE pairs with pcb_init_state()'s
  // ACQUIRE used by lazy-init preconditions and fork-reinit state
  // queries.
  pcb_init_state_advance(PcbInitState::TierB);

  return 0;
}

// ---------------------------------------------------------------------------
// Fork reinit for VA inventory: delegates to the subsystem's own
// fork-reinit entry point (defined in va_inventory.h). This function
// exists only to bridge the `LIBC_NAMESPACE::internal` naming
// convention used by libc_fork_reinit_impl.cpp — no cross-module field
// access.
// ---------------------------------------------------------------------------
void LIBC_NAMESPACE::internal::va_inventory_fork_reinit() {
  LIBC_NAMESPACE::windows::va_inventory_fork_reinit();
}

LIBC_REGISTER_FINI(1, va_inventory,
                   &::LIBC_NAMESPACE::windows::va_inventory_fini)

LIBC_REGISTER_FORK_REINIT(va_inventory,
                          ::LIBC_NAMESPACE::internal::kForkPrioVaInventory,
                          &::LIBC_NAMESPACE::internal::va_inventory_fork_reinit)
