//===-- Definition of __pthread_cleanup_t type -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES___PTHREAD_CLEANUP_T_H
#define LLVM_LIBC_TYPES___PTHREAD_CLEANUP_T_H

#include "__llvm-libc-common.h"

struct __pthread_cleanup_t {
  void (*__routine)(void *);
  void *__arg;
  struct __pthread_cleanup_t *__prev;
};
typedef struct __pthread_cleanup_t __pthread_cleanup_t;

// Forward-declare the runtime function so the cleanup attribute can call it.
__BEGIN_C_DECLS
void __pthread_cleanup_pop(__pthread_cleanup_t *, int) __NOEXCEPT;
__END_C_DECLS

// Used by __attribute__((cleanup)) in the pthread_cleanup_push macro.
// __routine == NULL means __pthread_cleanup_pop already ran (normal path) —
// skip the call entirely. On the abnormal path (C++ exception), __routine
// is still set, so we unlink the frame from the cancel cleanup chain.
static inline void
__pthread_cleanup_pop_noop(__pthread_cleanup_t *__frame) {
  if (__frame->__routine)
    __pthread_cleanup_pop(__frame, 0);
}

#endif // LLVM_LIBC_TYPES___PTHREAD_CLEANUP_T_H
