//===-- Implementation header for exit_handler ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_STDLIB_EXIT_HANDLER_H
#define LLVM_LIBC_SRC_STDLIB_EXIT_HANDLER_H

#include "src/__support/CPP/mutex.h" // lock_guard
#include "src/__support/common.h"
#include "src/__support/fixedvector.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/os.h"
#include "src/__support/threads/mutex.h"
#if defined(LIBC_TARGET_OS_IS_WINDOWS)
#include "src/__support/OSUtil/windows/process/exit_callbacks.h"
#else
#include "src/__support/blockstore.h"
#endif

namespace LIBC_NAMESPACE_DECL {

using AtExitCallback = void(void *);
using StdCAtExitCallback = void(void);
constexpr size_t CALLBACK_LIST_SIZE_FOR_TESTS = 1024;

struct AtExitUnit {
  AtExitCallback *callback = nullptr;
  void *payload = nullptr;
  void *dso = nullptr;
  LIBC_INLINE constexpr AtExitUnit() = default;
  LIBC_INLINE constexpr AtExitUnit(AtExitCallback *c, void *p, void *d = nullptr)
      : callback(c), payload(p), dso(d) {}
};

#if defined(LIBC_TARGET_ARCH_IS_GPU)
using ExitCallbackList = FixedVector<AtExitUnit, 64>;
#elif defined(LIBC_TARGET_OS_IS_WINDOWS)
// Reserve-and-commit — no malloc dependency, avoids atexit → malloc cycle.
using ExitCallbackList = CommitVector<AtExitUnit>;
#elif defined(LIBC_COPT_PUBLIC_PACKAGING)
using ExitCallbackList = ReverseOrderBlockStore<AtExitUnit, 32>;
#else
using ExitCallbackList = FixedVector<AtExitUnit, CALLBACK_LIST_SIZE_FOR_TESTS>;
#endif

// This is handled by the 'atexit' implementation and shared by 'at_quick_exit'.
extern Mutex handler_list_mtx;

LIBC_INLINE void stdc_at_exit_func(void *payload) {
  reinterpret_cast<StdCAtExitCallback *>(payload)();
}

// Per Itanium ABI 3.3.5, a throwing destructor during finalization must call
// terminate. Windows overrides this with SEH in dtor_call.cpp.
void invoke_exit_destructor(void (*callback)(void *), void *payload);

constexpr size_t DSO_FINALIZE_BATCH_SIZE = 32;

LIBC_INLINE void call_exit_callbacks(ExitCallbackList &callbacks,
                                     void *dso = nullptr) {
  handler_list_mtx.lock();
  if (!dso) {
    while (!callbacks.empty()) {
      AtExitUnit unit = callbacks.back();
      callbacks.pop_back();
      if (!unit.callback) // Tombstoned by prior per-DSO finalization.
        continue;
      handler_list_mtx.unlock();
      invoke_exit_destructor(unit.callback, unit.payload);
      handler_list_mtx.lock();
    }
    ExitCallbackList::destroy(&callbacks);
  } else {
    // Collect-then-call: iterating with the lock released would risk
    // invalidation if a callback calls __cxa_atexit (BlockStore may
    // allocate a new block). Re-scan catches callbacks registered during
    // invocation.
    bool overflow;
    do {
      AtExitUnit batch[DSO_FINALIZE_BATCH_SIZE];
      size_t count = 0;
      overflow = false;

      for (auto it = callbacks.begin(), e = callbacks.end(); it != e; ++it) {
        AtExitUnit &unit = *it;
        if (unit.callback && unit.dso == dso) {
          if (count < DSO_FINALIZE_BATCH_SIZE) {
            batch[count++] = unit;
            unit.callback = nullptr;
          } else {
            overflow = true;
            break;
          }
        }
      }

      handler_list_mtx.unlock();
      for (size_t i = 0; i < count; ++i)
        invoke_exit_destructor(batch[i].callback, batch[i].payload);
      handler_list_mtx.lock();
    } while (overflow);
  }
  handler_list_mtx.unlock();
}

LIBC_INLINE int add_atexit_unit(ExitCallbackList &callbacks,
                                const AtExitUnit &unit) {
  cpp::lock_guard lock(handler_list_mtx);
  if (callbacks.push_back(unit))
    return 0;
  return -1;
}

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_STDLIB_EXIT_HANDLER_H
