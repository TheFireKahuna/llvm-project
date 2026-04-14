//===-- Definition of type posix_spawn_file_actions_t ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES_POSIX_SPAWNATTR_T_H
#define LLVM_LIBC_TYPES_POSIX_SPAWNATTR_T_H

#if defined(__NTPOSIX__)
#include "pid_t.h"
#include "sigset_t.h"
#include "struct_sched_param.h"

typedef struct {
  short __flags;
  pid_t __pgroup;
  sigset_t __sigmask;
  sigset_t __sigdefault;
  int __schedpolicy;
  struct sched_param __schedparam;
} posix_spawnattr_t;
#else

typedef struct {
  // This data structure will be populated as required.
} posix_spawnattr_t;
#endif
#endif // LLVM_LIBC_TYPES_POSIX_SPAWNATTR_T_H
