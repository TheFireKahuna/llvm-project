//===-- Definition of macros from sys/shm.h -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_LINUX_SYS_SHM_MACROS_H
#define LLVM_LIBC_MACROS_LINUX_SYS_SHM_MACROS_H

// shmat flags
#define SHM_RDONLY 010000
#define SHM_RND 020000
#define SHM_REMAP 040000
#define SHM_EXEC 0100000

// shmctl commands
#define SHM_LOCK 11
#define SHM_UNLOCK 12

// Linux-specific shmctl commands
#define SHM_STAT 13
#define SHM_INFO 14
#define SHM_STAT_ANY 15

// Segment low boundary address multiple (page size on most architectures)
#define SHMLBA 4096

#endif // LLVM_LIBC_MACROS_LINUX_SYS_SHM_MACROS_H
