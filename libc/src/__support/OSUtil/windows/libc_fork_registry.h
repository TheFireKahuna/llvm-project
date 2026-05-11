//===-- .libcfork registry — fork-reinit dispatch table ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Each libc subsystem with a fork-child reinit hook registers its function
// here via LIBC_REGISTER_FORK_REINIT(tag, priority, &fn). libc_fork_reinit()
// in libc_fork_reinit_impl.cpp walks the merged `.libcfork$M` section, sorts
// by priority, and invokes each entry in priority order.
//
// Why a section registry rather than explicit calls or weak callbacks:
//
//   * Trim correctness — a per-test trimmed libc.lib that doesn't include a
//     subsystem TU also won't include its `.libcfork$M` record. The walker
//     visits only what is actually linked, with zero per-test CMake
//     plumbing. This was the load-bearing failure of the old explicit-call
//     dispatcher: every fork-using hermetic test had to either (a) bloat
//     to include all 37 subsystems or (b) link-fail on subsystems it
//     genuinely didn't need.
//
//   * Defensive runtime audit — `.libcfork$M` is `[[gnu::retain]]` const
//     PE data and is checked PAGE_READONLY by the walker via
//     `enforce_section_readonly_or_fastfail` before any indirect call.
//     Tamper of the dispatch table requires a section unseal that the
//     audit catches; weak callbacks have no equivalent tripwire.
//
//   * PE/COFF native — `.libcfork` joins .libcveh / .libcfin / .libcops /
//     .libclzr / .libcmem / .libcfio. All use the same `$A`/`$M`/`$Z`
//     merge pattern, the same `[[gnu::retain]]` policy, the same audit.
//
//   * Local edits — adding a new subsystem fork hook is one line next to
//     the function definition. No dispatcher edit needed.
//
// Priority space:
//
//   uint16_t priority — lower runs earlier. The walker is a stable sort,
//   so equal-priority entries run in linker-merge (file inclusion) order.
//   Conventional bands (leaving gaps for future insertion):
//
//      0..9    VEH and immediate post-VEH (rdebug)
//     10..19   identity / process metadata
//     20..29   pre-memory subsystem state (sysv_shm)
//     30..49   memory subsystem (crystalline, va_substrate, mapping_table,
//              scratch, pkey, mmap_lock — strict intra-band ordering)
//     50..59   memory follow-up (memory_reconcile, mlock_policy,
//              va_inventory)
//     60..69   environment / allocator
//     70..89   pools, timers, process subsystems, thread infra
//     90..99   fd table and lock table
//    100..119  console / tty
//    120..149  services (reactor, cpu_limit_timer_fork_restore, alpc_bus,
//              signal — strict intra-band ordering)
//    150..199  IPC (epoll, inotify, dlfcn, pty)
//
// Use the LIBC_FORK_REINIT_PRIORITY enum below for symbolic constants
// and to make the call sequence visible in one place.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_LIBC_FORK_REGISTRY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_LIBC_FORK_REGISTRY_H

#include "src/__support/OSUtil/windows/section_registry.h"
#include "src/__support/macros/config.h"
#include "hdr/stdint_proxy.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Record stored in `.libcfork$M`. POD by design — copied to a stack array
// at fork time and sorted in place.
struct ForkReinitEntry {
  void (*fn)();
  uint16_t priority;
  const char *name; // diagnostics only — kept stable so debug builds can
                    // print the offending callback in fastfail messages.
};

// Numeric priority constants. All explicit (no implicit ordering by enum
// position) so reordering this list cannot silently change fork behaviour.
// Values intentionally have gaps for future insertion without renumbering
// the rest of the table.
enum LibcForkReinitPriority : uint16_t {
  // VEH first — every later subsystem may fault.
  kForkPrioVehCore = 0,
  kForkPrioRdebug = 5,

  // Identity / process metadata.
  kForkPrioIdentity = 10,

  // Memory subsystem prelude (independent).
  kForkPrioSysvShm = 20,

  // Memory subsystem core — strict ordering: crystalline first
  // (it zeroes per-thread Crystalline slots that va_substrate and
  // friends will rebuild on top of), then substrate, then mapping table.
  kForkPrioCrystalline = 30,
  kForkPrioVaSubstrate = 31,
  kForkPrioMappingTable = 32,
  // Scratch reclamation needs substrate pool rebuilt (above) and the
  // mapping table in clean state.
  kForkPrioScratch = 33,
  // pkey / mmap_lock are independent of each other and of scratch.
  kForkPrioPkey = 34,
  kForkPrioMmapLock = 35,
  // Layer 0 PAL — re-probes ProcessCookie + SeLockMemoryPrivilege after
  // RtlCloneUserProcess. Slotted post-VA / pre-allocator-follow-up.
  // Renumber to 32 in Phase 1.G when MappingTable is deleted.
  kForkPrioPal = 36,
  // Layer 2 pagemap — decommits all CoW-inherited pagemap pages so the
  // child's allocator (which fork-reinits at the allocator band) starts
  // with a fresh empty map. The reservation itself stays valid (CoW
  // from parent); only the per-page commit state is reset.
  kForkPrioPagemap = 37,
  // Layer 7 partition — re-rolls partition_secret in the Zone 0b unseal
  // window, then walks the reserve table to recompute every live
  // descriptor's canary against the new process_cookie + new
  // partition_secret. Coarse pagemap, reserve table, descriptor pool,
  // and per-partition VAs are CoW-inherited. PINNED state on the 12
  // core libc-internal partitions survives across fork; user-class
  // partitions inherit at LIVE state and reach IDLE/DRAINING/RETIRED
  // only on the child's own decommit_chunk_unregister calls.
  kForkPrioPartition = 38,
  // Layer 1 va_tracker — POSIX-visible VA index. Drains pending retires
  // on `g_va_tracker_art_domain` and `g_va_tracker_skiplist_domain`,
  // resets per-arena hints, and re-publishes ART root for child
  // `replay_in_child`. Runs after partition (descriptor canaries valid)
  // and before memory reconcile (downstream consumers depend on the
  // tracker's child-side empty state to drive replay).
  kForkPrioVaTracker = 39,

  // Memory follow-up — depends on mmap_lock having reset.
  kForkPrioMemoryReconcile = 50,
  kForkPrioMlockPolicy = 51,
  kForkPrioVaInventory = 52,

  // Environment + allocator (after memory subsystem is reset).
  kForkPrioEnv = 60,
  kForkPrioAlloc = 61,

  // Pools and locks (independent group — order within band irrelevant).
  kForkPrioOfdPool = 70,
  kForkPrioBrk = 71,
  kForkPrioFilePool = 72,
  kForkPrioWaitSlot = 73,
  kForkPrioFutexAddr = 74,

  // Timers (independent within band).
  kForkPrioSetitimer = 75,
  kForkPrioTimerCreate = 76,

  // Process subsystems.
  kForkPrioCpuLimitTimer = 77,
  kForkPrioChildTable = 78,
  kForkPrioRlimit = 79,

  // Thread infrastructure pools (independent within band).
  kForkPrioLifecycle = 80,
  kForkPrioThreadSelf = 81,
  kForkPrioRobustPool = 82,
  kForkPrioThreadRing = 83,
  kForkPrioThreadStorage = 84,
  kForkPrioNamedSemaphore = 85,

  // Fd table — after all pool resets.
  kForkPrioFdTable = 90,
  // Lock table — after fd_table.
  kForkPrioLockTable = 91,

  // Console / tty — after fd_table (may touch fd handles).
  kForkPrioConsoleTty = 100,

  // Services — strict ordering: reactor (new IOCP) → cpu_limit_timer
  // restore (re-associate job with new IOCP) → alpc_bus (receive port is
  // a reactor watch) → signal (needs ALPC port).
  kForkPrioReactor = 120,
  kForkPrioCpuLimitTimerRestore = 121,
  kForkPrioAlpcBus = 122,
  kForkPrioSignal = 123,

  // IPC subsystems — must follow reactor (stale WCP/AFD registrations
  // reference the parent IOCP that reactor_fork_reinit replaced).
  kForkPrioEpoll = 150,
  kForkPrioInotify = 151,
  kForkPrioDlfcn = 152,

  // PTY — after console + after fd_table.
  kForkPrioPtyTree = 160,
  kForkPrioVtPty = 161,
};

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Register a fork-child reinit function. `tag` must be unique across the
// link (it becomes part of the record's strong symbol name). `prio` is one
// of the LibcForkReinitPriority constants above — using a non-enum literal
// works but defeats the central audit point.
//
// The macro emits both the section record and an `__libc_anchor_fork_<tag>`
// extern. The strong-impl glob `LIBC_FORCE_PULL_GLOB("__libc_anchor_fork_*")`
// in fork_reinit.cpp exists for libc_fork_reinit_impl.cpp / fork_quiesce.cpp
// — the per-subsystem anchors emitted here are bystanders to it (each
// subsystem TU is pulled by its own DEPENDS edges from the test, not by
// the fork glob). The anchor is emitted nonetheless for symmetry with the
// other registries and so an audit tool can enumerate every fork-reinit
// participant via the glob.
//
// Must expand at namespace scope (file scope, outside any namespace {}
// block).
#define LIBC_REGISTER_FORK_REINIT(tag, prio, fn_ptr)                           \
  LIBC_SECTION_REGISTER(libcfork,                                              \
                        ::LIBC_NAMESPACE::internal::ForkReinitEntry,           \
                        tag,                                                   \
                        {(fn_ptr), (prio), #tag})                              \
  extern "C" [[gnu::used]] void __libc_anchor_fork_##tag(void) {}

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_LIBC_FORK_REGISTRY_H
