//===-- POSIX sys/sem.h macros for Windows --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_WINDOWS_SYS_SEM_MACROS_H
#define LLVM_LIBC_MACROS_WINDOWS_SYS_SEM_MACROS_H

// semop flags (values match Linux for source compatibility)
#define SEM_UNDO 0x1000

// semctl command definitions
#define GETPID 11
#define GETVAL 12
#define GETALL 13
#define GETNCNT 14
#define GETZCNT 15
#define SETVAL 16
#define SETALL 17

// Linux-specific extensions retained for compatibility.
#define SEM_STAT 18
#define SEM_INFO 19
#define SEM_STAT_ANY 20

#endif // LLVM_LIBC_MACROS_WINDOWS_SYS_SEM_MACROS_H
