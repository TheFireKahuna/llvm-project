//===-- Windows Time Macros Extension -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_WINDOWS_TIME_MACROS_EXT_H
#define LLVM_LIBC_MACROS_WINDOWS_TIME_MACROS_EXT_H

#define CLOCK_MONOTONIC 0
#define CLOCK_REALTIME 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_REALTIME_COARSE 5
#define CLOCK_MONOTONIC_COARSE 6
#define CLOCK_BOOTTIME 7
#define CLOCK_REALTIME_ALARM 8
#define CLOCK_BOOTTIME_ALARM 9
#define CLOCK_TAI 11

// clock_nanosleep / timer_settime flags
#define TIMER_ABSTIME 1

// Maximum overrun count returned by timer_getoverrun. Matches Linux.
#define DELAYTIMER_MAX 2147483647

#define CLOCKS_PER_SEC 1000000

#endif // LLVM_LIBC_MACROS_WINDOWS_TIME_MACROS_EXT_H
