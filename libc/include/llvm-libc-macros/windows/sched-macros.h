//===-- Definition of Windows sched macros ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_WINDOWS_SCHED_MACROS_H
#define LLVM_LIBC_MACROS_WINDOWS_SCHED_MACROS_H

// POSIX required — values match Linux for source compatibility.
#define SCHED_OTHER 0
#define SCHED_FIFO 1
#define SCHED_RR 2

// cpu_set_t support — same macro interface as Linux.
#define CPU_SETSIZE __CPU_SETSIZE
#define NCPUBITS __NCPUBITS
#define CPU_COUNT_S(setsize, set) __sched_getcpucount(setsize, set)
#define CPU_COUNT(set) CPU_COUNT_S(sizeof(cpu_set_t), set)
#define CPU_ZERO_S(setsize, set) __sched_setcpuzero(setsize, set)
#define CPU_ZERO(set) CPU_ZERO_S(sizeof(cpu_set_t), set)
#define CPU_SET_S(cpu, setsize, set) __sched_setcpuset(cpu, setsize, set)
#define CPU_SET(cpu, set) CPU_SET_S(cpu, sizeof(cpu_set_t), set)
#define CPU_ISSET_S(cpu, setsize, set) __sched_getcpuisset(cpu, setsize, set)
#define CPU_ISSET(cpu, set) CPU_ISSET_S(cpu, sizeof(cpu_set_t), set)

#endif // LLVM_LIBC_MACROS_WINDOWS_SCHED_MACROS_H
