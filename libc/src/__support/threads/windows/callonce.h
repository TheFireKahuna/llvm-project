//===-- Windows callonce fastpath -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_CALLONCE_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_CALLONCE_H

#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/futex_utils.h"

namespace LIBC_NAMESPACE_DECL {

using CallOnceFlag = Futex;

namespace callonce_impl {

static constexpr FutexValueType NOT_CALLED = 0x0;
static constexpr FutexValueType START = 0x11;
static constexpr FutexValueType WAITING = 0x22;
static constexpr FutexValueType FINISH = 0x33;

// Avoid cmpxchg operation if the function has already been called.
// The destination operand of cmpxchg may receive a write cycle without
// regard to the result of the comparison.
LIBC_INLINE bool callonce_fastpath(CallOnceFlag *flag) {
  return flag->load(cpp::MemoryOrder::ACQUIRE) == FINISH;
}

template <class CallOnceCallback>
[[gnu::noinline, gnu::cold]] int callonce_slowpath(CallOnceFlag *flag,
                                                   CallOnceCallback callback) {
  auto *futex_word = reinterpret_cast<Futex *>(flag);

  FutexValueType not_called = NOT_CALLED;

  // The call_once call can return only after the called function |func|
  // returns. So, we use futexes to synchronize calls with the same flag value.
  if (futex_word->compare_exchange_strong(not_called, START)) {
    callback();
    auto status = futex_word->exchange(FINISH);
    if (status == WAITING)
      futex_word->notify_all();
    return 0;
  }

  for (;;) {
    FutexValueType status = START;
    if (futex_word->compare_exchange_strong(status, WAITING) ||
        status == WAITING)
      futex_word->wait(WAITING);
    // ACQUIRE pairs with the executor's exchange(FINISH, SEQ_CST).
    // Also handles spurious wakeups — loop rechecks.
    if (futex_word->load(cpp::MemoryOrder::ACQUIRE) == FINISH)
      return 0;
  }
}

// RAII guard for the executor slot. Resets to NOT_CALLED on abnormal exit
// (exception, longjmp, SEH unwind). Normal exit calls commit() to finalize.
struct CallOnceGuard {
  Futex *futex_word;
  bool committed = false;

  explicit CallOnceGuard(Futex *fw) : futex_word(fw) {}

  void commit(bool success) {
    committed = true;
    auto status =
        futex_word->exchange(success ? FINISH : NOT_CALLED);
    if (status == WAITING)
      futex_word->notify_all();
  }

  ~CallOnceGuard() {
    if (!committed) {
      // Abnormal exit — reset so waiters can retry.
      auto status = futex_word->exchange(NOT_CALLED);
      if (status == WAITING)
        futex_word->notify_all();
    }
  }
};

// Failable variant: callback returns bool. On success, transitions to FINISH.
// On failure, resets to NOT_CALLED so a future caller can retry.
// On abnormal exit (exception, longjmp), the RAII guard resets to NOT_CALLED.
template <class CallOnceCallback>
[[gnu::noinline, gnu::cold]] bool
callonce_slowpath_failable(CallOnceFlag *flag, CallOnceCallback callback) {
  auto *futex_word = reinterpret_cast<Futex *>(flag);

  for (;;) {
    FutexValueType not_called = NOT_CALLED;
    if (futex_word->compare_exchange_strong(not_called, START)) {
      CallOnceGuard guard(futex_word);
      bool ok = callback();
      guard.commit(ok);
      return ok;
    }

    // CAS failed — not_called holds the current value.
    if (not_called == FINISH)
      return true;
    if (not_called == NOT_CALLED)
      continue; // Executor failed or aborted — retry immediately.
    // START or WAITING — another thread is executing. Block until done.
    FutexValueType start = START;
    if (futex_word->compare_exchange_strong(start, WAITING) ||
        start == WAITING)
      futex_word->wait(WAITING);
    // Woke up: FINISH (success) or NOT_CALLED (failure/abort) — loop.
  }
}

} // namespace callonce_impl

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_CALLONCE_H
