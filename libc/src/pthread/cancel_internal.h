//===-- Internal cancellation check for pthread_cancel ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Thin compatibility header. All cancellation support is now in
// __support/threads/cancel_support.h. This header exists so that existing
// callers (pthread_exit, pthread_testcancel, etc.) continue to compile.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_PTHREAD_CANCEL_INTERNAL_H
#define LLVM_LIBC_SRC_PTHREAD_CANCEL_INTERNAL_H

#include "src/__support/macros/config.h"
#include "src/__support/threads/cancel_support.h"

namespace LIBC_NAMESPACE_DECL {

LIBC_INLINE void cancel_check() { cancel::check(); }

[[noreturn]] inline void cancel_act() { cancel::act(); }
[[noreturn]] inline void exit_act(void *retval) {
  cancel::exit_with_unwind(retval);
}

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_PTHREAD_CANCEL_INTERNAL_H
