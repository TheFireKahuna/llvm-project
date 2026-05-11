//===-- CrystallineDomain registry — shared cross-instantiation state ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Non-template state shared by every CrystallineDomain<> instantiation:
//
//   - Intrusive SLL head of installed domain descriptors. Domains
//     register explicitly via `CrystallineDomain::init_registration()`
//     during the consumer's Tier A startup hook (file-scope domain
//     instances are trivially constructed; the libc build runs with
//     -Wglobal-constructors as -Werror and forbids static ctors).
//   - Dense domain_id assignment at registration (indexes into the
//     per-thread inline slots[] / batches[] arrays that live in the
//     Crystalline region of every ThreadScratch arena — see
//     concurrent/crystalline_local_state.h for the contract and
//     alloc/thread_scratch.h for the arena layout).
//
// No backing pool, no TLS allocation — Crystalline local state is inline
// in the ThreadScratch arena, eager-committed at thread creation. That's
// what lets any caller (including ThreadLifecycle during its own bring-
// up, and the mapping-table path that ThreadScratch's registration runs
// through) use CrystallineDomain::read()/retire() the moment the thread
// can reach its own ThreadScratchState — no separate initialization step
// or cycle with ThreadScratch's own setup.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/concurrent/crystalline_domain_registry.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_local_state.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"

// crystalline_domain.h deliberately NOT included — this TU implements
// only the non-template registry state that CrystallineDomain<>'s header
// uses. Keeping crystalline_domain.h out breaks a CMake cycle
// (crystalline_domain.h pulls in thread_scratch.h, and thread_scratch.cpp
// links against the registry symbols defined here).

namespace LIBC_NAMESPACE_DECL {
namespace concurrent {

// -------------------------------------------------------------------------
// Registry head + domain_id counter.
// -------------------------------------------------------------------------
//
// File-scope so the static ctors of CrystallineDomain descriptors can
// push into them without depending on static-init ordering of other
// TUs. Plain cpp::Atomic<> head + counter constant-initialize to zero.
alignas(64) static cpp::Atomic<uintptr_t> g_registry_head{0};
static cpp::Atomic<uint32_t> g_registry_count{0};

void registry_push(CrystallineDomainDescriptor *desc) {
  // Dense domain_id in [0, kMaxCrystallineDomains). Overflow is a
  // compile-time-tunable structural ceiling: more CrystallineDomains
  // than CrystallineThreadRegion's inline arrays can index. Trap
  // rather than silently corrupt someone else's slot.
  uint32_t id =
      g_registry_count.fetch_add(1, cpp::MemoryOrder::RELAXED);
  if (LIBC_UNLIKELY(id >= kMaxCrystallineDomains))
    __builtin_trap();
  desc->domain_id = id;

  uintptr_t old = g_registry_head.load(cpp::MemoryOrder::ACQUIRE);
  for (;;) {
    desc->next = reinterpret_cast<CrystallineDomainDescriptor *>(old);
    if (g_registry_head.compare_exchange_weak(
            old, reinterpret_cast<uintptr_t>(desc),
            cpp::MemoryOrder::ACQ_REL, cpp::MemoryOrder::ACQUIRE))
      break;
  }
}

void registry_fork_reinit_all() {
  uintptr_t cur = g_registry_head.load(cpp::MemoryOrder::ACQUIRE);
  while (cur != 0) {
    auto *desc = reinterpret_cast<CrystallineDomainDescriptor *>(cur);
    if (desc->fork_reinit_fn)
      desc->fork_reinit_fn(desc->context);
    cur = reinterpret_cast<uintptr_t>(desc->next);
  }
}

void registry_fini_all() {
  uintptr_t cur = g_registry_head.load(cpp::MemoryOrder::ACQUIRE);
  while (cur != 0) {
    auto *desc = reinterpret_cast<CrystallineDomainDescriptor *>(cur);
    if (desc->fini_fn)
      desc->fini_fn(desc->context);
    cur = reinterpret_cast<uintptr_t>(desc->next);
  }
}

void registry_flush_thread_all(CrystallineBatch *batches) {
  // Walks every registered domain. For each, publish the exiting
  // thread's pending retire-batch (at `batches[desc->domain_id]`) into
  // the domain's per-thread slot chains so live threads will reap the
  // nodes during their normal leave cycles. Runs single-threaded in
  // the exiting thread's cleanup path — no synchronization beyond the
  // descriptor pointer loads. A domain without a thread_flush_fn
  // (shouldn't happen — every CrystallineDomain ctor wires one) is
  // silently skipped; a zero-state batch is a no-op in the trampoline.
  if (batches == nullptr)
    return;
  uintptr_t cur = g_registry_head.load(cpp::MemoryOrder::ACQUIRE);
  while (cur != 0) {
    auto *desc = reinterpret_cast<CrystallineDomainDescriptor *>(cur);
    if (desc->thread_flush_fn != nullptr)
      desc->thread_flush_fn(desc->context, &batches[desc->domain_id]);
    cur = reinterpret_cast<uintptr_t>(desc->next);
  }
}

void registry_warm_thread_all() {
  // Walks every registered domain. For each, eagerly claim the
  // calling thread's per-domain slot so any future protect() / retire()
  // call is allocation-free. Idempotent — the trampoline calls
  // `my_slot_idx()` which short-circuits on an already-cached index.
  // Domains without a warm_thread_fn (legacy / shouldn't happen — every
  // CrystallineDomain ctor wires one) are silently skipped.
  uintptr_t cur = g_registry_head.load(cpp::MemoryOrder::ACQUIRE);
  while (cur != 0) {
    auto *desc = reinterpret_cast<CrystallineDomainDescriptor *>(cur);
    if (desc->warm_thread_fn != nullptr)
      desc->warm_thread_fn(desc->context);
    cur = reinterpret_cast<uintptr_t>(desc->next);
  }
}

void registry_release_slots_all(uint16_t *slot_indices) {
  // Walks every registered domain. For each, release the exiting
  // thread's per-domain slot (`slot_indices[desc->domain_id]`) back to
  // the domain's pool. An index of 0 means the thread never reserved in
  // that domain — skip. Runs after registry_flush_thread_all so the
  // batches' retires have already been republished into the slot chains
  // the released slot is leaving behind.
  if (slot_indices == nullptr)
    return;
  uintptr_t cur = g_registry_head.load(cpp::MemoryOrder::ACQUIRE);
  while (cur != 0) {
    auto *desc = reinterpret_cast<CrystallineDomainDescriptor *>(cur);
    uint16_t idx = slot_indices[desc->domain_id];
    if (idx != 0 && desc->release_slot_fn != nullptr)
      desc->release_slot_fn(desc->context, idx);
    cur = reinterpret_cast<uintptr_t>(desc->next);
  }
}

// -------------------------------------------------------------------------
// Phase-4 startup / fork / fini hooks.
// -------------------------------------------------------------------------

int startup_init() {
  // Per-thread Crystalline state is inline in the ThreadScratch arena
  // VA; no pool bring-up required. Descriptors self-install via static
  // ctors before this point. Nothing to do at runtime.
  return 0;
}

static void crystalline_fini() {
  registry_fini_all();
  // Nothing else to release — per-thread state is owned by each thread's
  // ThreadScratch arena and freed by scratch_thread_cleanup when the
  // thread exits (or by exec_self_hollow's sweep on exec).
}

} // namespace concurrent

namespace internal {

int crystalline_startup_init() {
  return ::LIBC_NAMESPACE::concurrent::startup_init();
}

void crystalline_fork_reinit() {
  ::LIBC_NAMESPACE::concurrent::registry_fork_reinit_all();
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Phase 4 — same bucket as wait_slot/lifecycle so fini runs in the
// window where thread registry and wait slots are still alive (any
// domain consumer will pin these to freeze the domain before fini).
LIBC_REGISTER_FINI(4, crystalline,
                   &::LIBC_NAMESPACE::concurrent::crystalline_fini)

LIBC_REGISTER_FORK_REINIT(crystalline,
                          ::LIBC_NAMESPACE::internal::kForkPrioCrystalline,
                          &::LIBC_NAMESPACE::internal::crystalline_fork_reinit)
