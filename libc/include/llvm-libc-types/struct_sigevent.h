//===-- Definition of struct sigevent -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES_STRUCT_SIGEVENT_H
#define LLVM_LIBC_TYPES_STRUCT_SIGEVENT_H

#include "pthread_attr_t.h"
#include "union_sigval.h"

struct sigevent {
  int sigev_notify;
  int sigev_signo;
  union sigval sigev_value;
  void (*sigev_notify_function)(union sigval);
  pthread_attr_t *sigev_notify_attributes;
};

#endif // LLVM_LIBC_TYPES_STRUCT_SIGEVENT_H
