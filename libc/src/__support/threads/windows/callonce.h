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

// Predicate for callonce_slowpath: wake once the executor has
// published FINISH. Notify contract satisfied by the executor's
// exchange(FINISH, SEQ_CST) followed by notify_all when the prior
// value was WAITING.
[[clang::always_inline]] LIBC_INLINE bool
is_finish(uint32_t v, uint32_t /*arg*/) noexcept {
  return v == FINISH;
}

// Predicate for callonce_slowpath_failable: wake on either terminal
// state. FINISH (success) returns true from the waiter; NOT_CALLED
// (executor failed or threw) drops the waiter back to the outer retry
// loop, where it attempts to become the executor itself via
// CAS(NOT_CALLED → START). Both edges are notified — commit(false)
// and the RAII guard's abnormal-exit path both exchange(NOT_CALLED)
// + notify_all; commit(true) exchange(FINISH) + notify_all.
[[clang::always_inline]] LIBC_INLINE bool
is_finish_or_not_called(uint32_t v, uint32_t /*arg*/) noexcept {
  return v == FINISH || v == NOT_CALLED;
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

  // Slowpath: executor is running. Wait until FINISH. The outer loop
  // handles the CAS-fail-on-not-START case (executor completed before
  // we could publish WAITING) — on that path we fall through to the
  // FINISH check without waiting. Under wait_on_predicate, ret == 0
  // guarantees pred held, so no spurious-wake loop is needed — the
  // only iteration is the pre-park CAS retry.
  for (;;) {
    FutexValueType status = START;
    if (futex_word->compare_exchange_strong(status, WAITING) ||
        status == WAITING) {
      long ret = futex_word->wait_on_predicate(&is_finish, 0);
      if (LIBC_UNLIKELY(ret < 0))
        __builtin_trap(); // only non-zero path is -ENOMEM (fatal)
      return 0; // pred held ⇒ value == FINISH
    }
    // CAS failed without status == WAITING: executor transitioned
    // START → FINISH before our CAS landed.
    if (futex_word->load(cpp::MemoryOrder::ACQUIRE) == FINISH)
      return 0;
    // Defensive: any other state (shouldn't happen in the non-
    // failable variant — NOT_CALLED never reappears) — reload and
    // retry the CAS. Single-iteration in practice.
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
    // START or WAITING — another thread is executing. Park until
    // either terminal state is observable. wait_on_predicate returns
    // exactly when value ∈ {FINISH, NOT_CALLED}; the outer loop then
    // dispatches on the observed value via the CAS-retry path above.
    FutexValueType start = START;
    if (futex_word->compare_exchange_strong(start, WAITING) ||
        start == WAITING) {
      long ret = futex_word->wait_on_predicate(&is_finish_or_not_called, 0);
      if (LIBC_UNLIKELY(ret < 0))
        __builtin_trap(); // only non-zero path is -ENOMEM (fatal)
    }
    // Fall through — outer loop CAS(NOT_CALLED → START) handles retry
    // on abort, or the FINISH/NOT_CALLED branch above returns on the
    // terminal state we just parked for.
  }
}

} // namespace callonce_impl

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_CALLONCE_H
