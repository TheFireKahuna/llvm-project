//===-- Implementation of atexit ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/atexit.h"
#include "hdr/types/atexithandler_t.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/stdlib/exit_handler.h"

namespace LIBC_NAMESPACE_DECL {

// Windows overrides with SEH-wrapped version in windows/dtor_call.cpp.
[[gnu::weak]] void invoke_exit_destructor(void (*callback)(void *),
                                          void *payload) {
  callback(payload);
}

constinit ExitCallbackList atexit_callbacks;
Mutex handler_list_mtx(false, false, false, false);

extern "C" {

int __cxa_atexit(AtExitCallback *callback, void *payload, void *dso) {
  return add_atexit_unit(atexit_callbacks, {callback, payload, dso});
}

// Idempotent for process exit (dso == nullptr) to prevent double-finalization
// from concurrent exit() calls.
void __cxa_finalize(void *dso) {
  if (!dso) {
    static cpp::Atomic<bool> finalized{false};
    if (finalized.exchange(true))
      return;
  }
  call_exit_callbacks(atexit_callbacks, dso);
}

} // extern "C"

LLVM_LIBC_FUNCTION(int, atexit, (__atexithandler_t callback)) {
  return add_atexit_unit(
      atexit_callbacks,
      {&stdc_at_exit_func, reinterpret_cast<void *>(callback)});
}

} // namespace LIBC_NAMESPACE_DECL
