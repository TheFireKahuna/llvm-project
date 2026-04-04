//===-- Definition of ucontext_t type -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES_UCONTEXT_T_H
#define LLVM_LIBC_TYPES_UCONTEXT_T_H

#include "mcontext_t.h"
#include "sigset_t.h"
#include "stack_t.h"

typedef struct __ucontext_t {
  unsigned long uc_flags;
  struct __ucontext_t *uc_link;
  stack_t uc_stack;
  mcontext_t uc_mcontext;
  sigset_t uc_sigmask;
} ucontext_t;

#endif // LLVM_LIBC_TYPES_UCONTEXT_T_H
