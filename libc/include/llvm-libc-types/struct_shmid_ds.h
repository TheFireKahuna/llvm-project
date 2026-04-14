//===-- Definition of struct shmid_ds -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_TYPES_STRUCT_SHMID_DS_H
#define LLVM_LIBC_TYPES_STRUCT_SHMID_DS_H

#include "pid_t.h"
#include "shmatt_t.h"
#include "size_t.h"
#include "struct_ipc_perm.h"
#include "time_t.h"

struct shmid_ds {
  struct ipc_perm shm_perm;
#ifdef __linux__
  size_t shm_segsz;
  time_t shm_atime;
  unsigned long __unused1;
  time_t shm_dtime;
  unsigned long __unused2;
  time_t shm_ctime;
  unsigned long __unused3;
  pid_t shm_cpid;
  pid_t shm_lpid;
  shmatt_t shm_nattch;
  unsigned long __unused4;
  unsigned long __unused5;
#else
  size_t shm_segsz;
  time_t shm_atime;
  time_t shm_dtime;
  time_t shm_ctime;
  pid_t shm_cpid;
  pid_t shm_lpid;
  shmatt_t shm_nattch;
#endif
};

#endif // LLVM_LIBC_TYPES_STRUCT_SHMID_DS_H
