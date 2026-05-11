//===-- Crystalline domain registry state -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Registry-level types for CrystallineDomain<> instantiations. One static
// descriptor per CrystallineDomain<> template instantiation, self-installed
// into a process-wide intrusive SLL at static init, walked at fork-reinit
// and process-fini time.
//
// Keeping this non-template file separate from crystalline_domain.h avoids
// a CMake cycle: crystalline_domain.h depends on thread_scratch.h (for
// get_thread_scratch()), and thread_scratch.cpp must depend on the registry
// symbols defined here (from scratch_thread_cleanup → registry_flush_thread_all).
// Without the split the registry symbols would only exist inside the
// template header, and thread_scratch would pull the whole template
// machinery in transitively.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_DOMAIN_REGISTRY_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_DOMAIN_REGISTRY_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_local_state.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace concurrent {

struct CrystallineDomainDescriptor;

// Trampoline signatures. The walker has no knowledge of the domain's
// template parameters — it invokes these with the descriptor's opaque
// context pointer. Implementations live in the per-instantiation
// template and cast context back to the concrete domain type.
using CrystallineForkReinitFn = void (*)(void *context);
using CrystallineFiniFn = void (*)(void *context);

// Called from the exiting thread's cleanup path to flush any pending
// retire-batch state into the domain's slot chains before the per-thread
// arena (which owns the batch storage) is released. The batch argument
// points at the exiting thread's CrystallineBatch for this specific
// domain; the trampoline must not retain it beyond the call.
using CrystallineThreadFlushFn =
    void (*)(void *context, CrystallineBatch *batch);

// Called from the exiting thread's cleanup path to release the per-
// thread per-domain slot back to the domain's pool. Tombstones the slot
// fields and pushes the index onto the pool's freelist. Invoked after
// CrystallineThreadFlushFn so the batch's retires are already
// republished into the slot chains the released slot is leaving behind.
using CrystallineReleaseSlotFn = void (*)(void *context, uint16_t slot_idx);

// Called from a freshly-created thread's bring-up path to eagerly
// claim this thread's per-domain slot index — moves the slot pool's
// demand-commit (NtAllocateVirtualMemory) off any future fault path.
// Idempotent: subsequent calls on the same thread short-circuit on the
// cached index in ThreadScratchState::crystalline_slot_idx[].
using CrystallineWarmThreadFn = void (*)(void *context);

// Intrusive SLL node. Defined inside each CrystallineDomain<>
// instantiation as a `static` member; the ctor pushes it onto the
// registry head at static init time. Never freed — the static lives
// for the process lifetime.
struct CrystallineDomainDescriptor {
  // Zero-init is valid: filled in by the self-install ctor.
  CrystallineDomainDescriptor *next;
  void *context;
  CrystallineForkReinitFn fork_reinit_fn;
  CrystallineFiniFn fini_fn;
  CrystallineThreadFlushFn thread_flush_fn;
  CrystallineReleaseSlotFn release_slot_fn;
  CrystallineWarmThreadFn warm_thread_fn;
  // Stable identifier for diagnostics — pointer to a compile-time string.
  const char *name;
  // Dense index in [0, kMaxCrystallineDomains). Assigned at registry_push
  // time via a monotonic counter. Used by every thread to index its
  // ThreadScratchState::crystalline_slot_idx[] and crystalline_batches[].
  // Trapping registry_push guarantees this stays in range.
  uint32_t domain_id;
  uint32_t _pad;
};

// Lock-free push. Called by every CrystallineDomain<>'s descriptor ctor
// at static-init time. Assigns a dense `domain_id` in
// [0, kMaxCrystallineDomains) from a monotonic counter and stores it on
// `desc`. Overflow (more domains than CrystallineThreadRegion's inline
// arrays can index) traps at registration time — a compile-time-tunable
// structural ceiling, not a runtime failure mode.
//
// Safe against concurrent pushes; a concurrent walker is not expected
// during static init (the walker only runs at fork or fini, well after
// all ctors have completed).
void registry_push(CrystallineDomainDescriptor *desc);

// Walks every registered domain and invokes its fork_reinit trampoline.
// Called from libc_fork_reinit() in the child. The child is single-
// threaded here, so no synchronization beyond the descriptor pointer
// loads is needed.
void registry_fork_reinit_all();

// Walks every registered domain and invokes its fini trampoline.
// Called from the Phase-4 LIBC_REGISTER_FINI hook.
void registry_fini_all();

// Walks every registered domain and drains the calling thread's retire
// batches (now living on ThreadScratchState directly as a
// kMaxCrystallineDomains-sized array) into each domain's slot chains.
// Called from scratch_thread_cleanup BEFORE the arena VA is released.
// `batches` points at the exiting thread's
// `ThreadScratchState::crystalline_batches[0]`; the walker indexes each
// descriptor's domain_id into the array.
void registry_flush_thread_all(CrystallineBatch *batches);

// Walks every registered domain and releases the calling thread's
// per-domain Crystalline slot (from the cross-thread CrystallineSlotPool)
// back to the freelist. Called from scratch_thread_cleanup AFTER the
// batch flush. `slot_indices` points at the exiting thread's
// `ThreadScratchState::crystalline_slot_idx[0]`; an entry of 0 means
// the thread never reserved in that domain (no slot to release).
void registry_release_slots_all(uint16_t *slot_indices);

// Walks every registered domain and eagerly claims the calling
// thread's per-domain slot. Drives the slot pool's potentially-VA-
// committing claim_slot path off the fault path so any later
// `protect()` / `retire()` call on this thread is allocation-free.
// Idempotent across re-entry; safe to call multiple times.
//
// Wired into `thread_entry_impl` for libc-created threads, and into
// the late-Tier-A bring-up of any subsystem that wants its consumer
// threads warm. Domains registered AFTER this call won't be warmed
// for the calling thread until either another `registry_warm_thread_all`
// call or the lazy claim on first `protect()`.
void registry_warm_thread_all();

// Phase-4 startup hook. Asserts registry invariants. Per-thread state
// lives inline in the ThreadScratch arena VA, so nothing pool-like
// needs bring-up here.
int startup_init();

} // namespace concurrent
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_CONCURRENT_CRYSTALLINE_DOMAIN_REGISTRY_H
