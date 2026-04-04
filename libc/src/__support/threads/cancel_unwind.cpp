//===-- Forced-unwind machinery for pthread_cancel and pthread_exit -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements cancel::act() and cancel::exit_with_unwind() using
// _Unwind_ForcedUnwind (Itanium C++ ABI §1.6.4). Portable across all
// Itanium-ABI targets (Linux, Windows Itanium, etc.).
//
// The stop function fires C++ destructors at each frame via _URC_NO_REASON,
// walks the pthread_cleanup_push chain at _UA_END_OF_STACK, then calls
// thread_exit.
//
// Exception class follows the libcxxabi pattern (cxa_exception.h): hex
// literal with ASCII comment, set via memcpy for ARM portability.
//
//===----------------------------------------------------------------------===//

#include "src/__support/threads/cancel_support.h"

#include "src/__support/macros/config.h"
#include "src/__support/threads/thread.h"

#include <pthread.h>
#include <string.h> // memcpy

#include <unwind.h>

// On COFF/PE targets (e.g., NT-POSIX), libunwind may not be linked when libc
// is built.  Declare _Unwind_ForcedUnwind as a weak external so the shared
// library links without it.  At runtime, if libunwind is present the strong
// symbol wins; otherwise the pointer is null and we fall through to the
// fallback path (cleanup handlers + thread_exit, no C++ destructor unwind).
//
// This mirrors libunwind's own LIBUNWIND_USE_WEAK_PTHREAD pattern for optional
// pthread dependencies on platforms where the unwinder may load before pthreads.
//
// On COFF, [[gnu::weak]] emits IMAGE_SYM_CLASS_WEAK_EXTERNAL with a null
// default — exactly the right semantics for an optional import.
#if defined(__NTPOSIX__) || defined(_WIN32_ITANIUM)
extern "C" [[gnu::weak]] _Unwind_Reason_Code
_Unwind_ForcedUnwind(_Unwind_Exception *, _Unwind_Stop_Fn, void *);
#define LIBC_UNWIND_MAY_BE_ABSENT 1
#endif

namespace LIBC_NAMESPACE_DECL {

// Vendor+language identifier per Itanium ABI. Same encoding scheme as
// libcxxabi's kOurExceptionClass (0x434C4E47432B2B00 = "CLNGC++\0").
static const uint64_t CANCEL_EXCEPTION_CLASS = 0x4C4C564D43414E00; // LLVMCAN\0
static const uint64_t EXIT_EXCEPTION_CLASS   = 0x4C4C564D45585400; // LLVMEXT\0

static void set_exception_class(_Unwind_Exception *exc, uint64_t cls) {
  ::memcpy(&exc->exception_class, &cls, sizeof(cls));
}

static void exception_cleanup(_Unwind_Reason_Code, _Unwind_Exception *) {}

// Per-thread unwind exception objects. Must remain live for the duration of
// the forced unwind. thread_local is correct — these run on the dying
// thread's own stack, once.
static thread_local _Unwind_Exception cancel_exception;
static thread_local _Unwind_Exception exit_exception;

// Thread-local storage for the pthread_exit retval. The stop function reads
// this when it reaches _UA_END_OF_STACK.
static thread_local void *exit_retval;

// Walk pthread_cleanup_push handlers in LIFO order via the platform-neutral
// cancel support interface.
static void run_cleanup_handlers() {
  while (auto *frame = cancel::get_cleanup_head()) {
    cancel::set_cleanup_head(frame->__prev);
    frame->__routine(frame->__arg);
  }
}

// Shared stop function for both cancel and exit forced unwinds. At each
// frame, returns _URC_NO_REASON so the personality runs cleanup landing
// pads (C++ destructors). At _UA_END_OF_STACK, walks the cleanup chain
// and exits the thread.
static _Unwind_Reason_Code
unwind_stop_fn(int, _Unwind_Action actions, _Unwind_Exception_Class exc_class,
               _Unwind_Exception *, struct _Unwind_Context *, void *arg) {
  if (!(actions & _UA_END_OF_STACK))
    return _URC_NO_REASON;

  (void)arg;

  run_cleanup_handlers();

  uint64_t cls;
  ::memcpy(&cls, &exc_class, sizeof(cls));
  void *retval = (cls == CANCEL_EXCEPTION_CLASS)
                     ? PTHREAD_CANCELED
                     : exit_retval;

  thread_exit(ThreadReturnValue(retval), ThreadStyle::POSIX);
  __builtin_unreachable();
}

// Fallback: if _Unwind_ForcedUnwind returns (corrupt stack, missing unwind
// info), walk cleanup manually and exit.
static void fallback_exit(void *retval) {
  run_cleanup_handlers();
  LIBC_NAMESPACE::thread_exit(ThreadReturnValue(retval), ThreadStyle::POSIX);
}

// Set the DISABLE bit in cancel_state. Prevents re-entry if a destructor
// or cleanup handler calls a cancellation point during unwind.
//
// cancel_state holds only state/type bits (no pending bit — that's in
// notify_word). The owning thread is the sole writer, so a simple
// fetch_or(RELAXED) suffices — no CAS loop needed. On x86, RELAXED
// fetch_or compiles to a single LOCK OR instruction.
static void disable_cancel() {
  auto *word = cancel::get_cancel_word();
  if (!word)
    return;
  word->fetch_or(cancel::STATE_BIT, cpp::MemoryOrder::RELAXED);
}

namespace cancel {

[[noreturn]] void act() {
  disable_cancel();

  // Platform hook: cancel in-flight I/O, etc. Must happen before the
  // unwinder destroys stack frames containing SyscallFrame objects.
  before_unwind();

  set_exception_class(&cancel_exception, CANCEL_EXCEPTION_CLASS);
  cancel_exception.exception_cleanup = exception_cleanup;

  // Use forced unwind to run C++ destructors at each frame.  On COFF targets
  // where libunwind may not be linked yet, the weak symbol resolves to null
  // and we fall through to the fallback (cleanup handlers + thread_exit).
#ifdef LIBC_UNWIND_MAY_BE_ABSENT
  if (_Unwind_ForcedUnwind)
#endif
    _Unwind_ForcedUnwind(&cancel_exception, unwind_stop_fn, nullptr);

  fallback_exit(PTHREAD_CANCELED);
  __builtin_unreachable();
}

[[noreturn]] void exit_with_unwind(void *retval) {
  // Guard against re-entry: if DISABLE bit is already set, a forced unwind
  // is in progress. Skip the second unwind — go straight to thread_exit.
  auto *word = cancel::get_cancel_word();
  if (word && (word->load(cpp::MemoryOrder::RELAXED) & cancel::STATE_BIT)) {
    fallback_exit(retval);
    __builtin_unreachable();
  }

  disable_cancel();

  exit_retval = retval;
  set_exception_class(&exit_exception, EXIT_EXCEPTION_CLASS);
  exit_exception.exception_cleanup = exception_cleanup;

#ifdef LIBC_UNWIND_MAY_BE_ABSENT
  if (_Unwind_ForcedUnwind)
#endif
    _Unwind_ForcedUnwind(&exit_exception, unwind_stop_fn, nullptr);

  fallback_exit(retval);
  __builtin_unreachable();
}

} // namespace cancel
} // namespace LIBC_NAMESPACE_DECL
