//===-- Implementation of pthread_getattr_np ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_getattr_np.h"

#include "hdr/errno_macros.h"
#include "hdr/sched_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/sched_support.h"
#include "src/__support/threads/thread.h"

#ifdef __NTPOSIX__
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/threads/windows/thread_lifecycle.h"
#include "src/__support/threads/windows/thread_registry.h"
#endif

#include <pthread.h>

namespace LIBC_NAMESPACE_DECL {

namespace {

#ifdef __NTPOSIX__
// Read TEB stack bounds for the calling thread. Used for main/foreign threads
// that lack pthread_create metadata.
//
// TEB+0x08   = StackBase (high address, top of usable stack)
// TEB+0x10   = StackLimit (current committed limit, may grow on demand)
// TEB+0x1478 = DeallocationStack (base of full stack reservation)
//
// Stack layout (addresses grow upward):
//   [DeallocationStack ... StackLimit) = guard/uncommitted pages
//   [StackLimit ......... StackBase)   = committed, usable stack
struct TebStackBounds {
  uintptr_t base;               // StackBase  (high)
  uintptr_t limit;              // StackLimit (committed low)
  uintptr_t deallocation_stack; // Full reservation low
};

LIBC_INLINE TebStackBounds read_teb_stack_bounds() {
  TebStackBounds sb;
#ifdef __x86_64__
  __asm__ __volatile__("movq %%gs:0x08, %0" : "=r"(sb.base));
  __asm__ __volatile__("movq %%gs:0x10, %0" : "=r"(sb.limit));
  __asm__ __volatile__("movq %%gs:0x1478, %0" : "=r"(sb.deallocation_stack));
#elif defined(__aarch64__)
  __asm__ __volatile__("ldr %0, [x18, #0x08]" : "=r"(sb.base));
  __asm__ __volatile__("ldr %0, [x18, #0x10]" : "=r"(sb.limit));
  __asm__ __volatile__("ldr %0, [x18, #0x1478]" : "=r"(sb.deallocation_stack));
#endif
  return sb;
}

// Populate attr from TEB stack bounds (main thread / foreign thread path).
void fill_attr_from_teb(pthread_attr_t *attr) {
  auto sb = read_teb_stack_bounds();
  attr->__stack = reinterpret_cast<void *>(sb.deallocation_stack);
  attr->__stacksize = sb.base - sb.deallocation_stack;
  // The gap between DeallocationStack and StackLimit is the guard region
  // (uncommitted demand-zero pages that trigger stack growth exceptions).
  attr->__guardsize = (sb.limit > sb.deallocation_stack)
                          ? (sb.limit - sb.deallocation_stack)
                          : EXEC_PAGESIZE;
  attr->__detachstate = PTHREAD_CREATE_JOINABLE;
}

// Populate attr from a validated ThreadLifecycle (stable, heap-allocated).
void fill_attr_from_lifecycle(pthread_attr_t *attr, ThreadLifecycle *lc) {
  attr->__stack = lc->stack_base;
  attr->__stacksize = lc->stack_size;
  attr->__guardsize = lc->guard_size;
  uint32_t ds = lc->detach_state.load(cpp::MemoryOrder::ACQUIRE);
  attr->__detachstate = (ds == uint32_t(DetachState::DETACHED))
                            ? PTHREAD_CREATE_DETACHED
                            : PTHREAD_CREATE_JOINABLE;
}

#endif // __NTPOSIX__

} // namespace

LLVM_LIBC_FUNCTION(int, pthread_getattr_np,
                   (pthread_t th, pthread_attr_t *attr)) {
  if (!attr)
    return EINVAL;

#ifdef __NTPOSIX__
  // Determine whether this is a self-query or a cross-thread query.
  // Self-queries are the dominant case (sanitizers, runtimes) and can be
  // answered entirely from thread-local state — no pointer trust required.
  bool is_self = (th == reinterpret_cast<pthread_t>(self.attrib));

  if (is_self || !th) {
    // --- Self-query (or null pthread_t, which also means "self") ---
    // All data comes from our own thread-local state or TEB. Zero risk.
    auto *lc = get_current_lifecycle();
    if (lc && lc->stack_base) {
      fill_attr_from_lifecycle(attr, lc);
    } else if (self.attrib && self.attrib->stack) {
      // Lifecycle not populated yet but ThreadAttributes exists (early in
      // thread startup before platform_data is wired).
      auto *a = self.attrib;
      attr->__stack = a->stack;
      attr->__stacksize = a->stacksize;
      attr->__guardsize = a->guardsize;
      uint32_t ds = a->detach_state.load(cpp::MemoryOrder::ACQUIRE);
      attr->__detachstate = (ds == uint32_t(DetachState::DETACHED))
                                ? PTHREAD_CREATE_DETACHED
                                : PTHREAD_CREATE_JOINABLE;
    } else {
      // Main thread or foreign thread with no lifecycle yet — TEB fallback.
      fill_attr_from_teb(attr);
    }

    // Scheduling: use NtCurrentThread() pseudo-handle (no TID lookup needed).
    int self_tid = static_cast<int>(NtCurrentThreadId());
    int err = sched_support::get_thread_sched(self_tid, &attr->__schedpolicy,
                                              &attr->__schedparam);
    if (err) {
      attr->__schedpolicy = SCHED_OTHER;
      attr->__schedparam.sched_priority = 0;
    }
  } else {
    // --- Cross-thread query ---
    // pthread_t is a stable ThreadAttributes* for live threads. Match it
    // against the registry's validated lifecycle metadata rather than
    // trusting the raw pointer directly.
    // registry_for_each auto-pins; the visitor runs under the epoch guard.
    auto *target_attrib = reinterpret_cast<ThreadAttributes *>(th);
    bool found = false;
    registry_for_each([&](ThreadLifecycle *lc) -> bool {
      if (lc->attrib != target_attrib)
        return false;
      found = true;
      fill_attr_from_lifecycle(attr, lc);
      // Scheduling: query by TID from the validated lifecycle.
      // get_thread_sched nests its own epoch pin via pin_depth.
      int err = sched_support::get_thread_sched(lc->tid, &attr->__schedpolicy,
                                                &attr->__schedparam);
      if (err) {
        attr->__schedpolicy = SCHED_OTHER;
        attr->__schedparam.sched_priority = 0;
      }
      return true; // stop after first match
    }, 0);
    if (!found)
      return ESRCH;
  }
#else
  // Non-NTPOSIX stub: best-effort from ThreadAttributes.
  auto *attrib = reinterpret_cast<ThreadAttributes *>(th);
  if (attrib && attrib->stack) {
    attr->__stack = attrib->stack;
    attr->__stacksize = attrib->stacksize;
    attr->__guardsize = attrib->guardsize;
    uint32_t ds = attrib->detach_state.load(cpp::MemoryOrder::ACQUIRE);
    attr->__detachstate = (ds == uint32_t(DetachState::DETACHED))
                              ? PTHREAD_CREATE_DETACHED
                              : PTHREAD_CREATE_JOINABLE;
  } else {
    attr->__stack = nullptr;
    attr->__stacksize = Thread::DEFAULT_STACKSIZE;
    attr->__guardsize = Thread::DEFAULT_GUARDSIZE;
    attr->__detachstate = PTHREAD_CREATE_JOINABLE;
  }
  attr->__schedpolicy = SCHED_OTHER;
  attr->__schedparam.sched_priority = 0;
#endif

  // Inherit-sched: not tracked post-creation. PTHREAD_INHERIT_SCHED is the
  // POSIX default and the honest answer for a running thread.
  attr->__inheritsched = PTHREAD_INHERIT_SCHED;

  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
