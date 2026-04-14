//===-- Linux-compatible futex constants and declaration -------*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Provides futex(2) on platforms where LLVM libc implements it (currently
// Windows). On Linux, use <linux/futex.h> + syscall() instead.
//
// The API matches the Linux futex(2) subset used by libc++ and other
// runtimes: FUTEX_WAIT, FUTEX_WAKE, and their _PRIVATE variants.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SYS_FUTEX_H
#define LLVM_LIBC_SYS_FUTEX_H

#include "__llvm-libc-common.h"

#include <stdint.h>
#include <time.h>

// Operations (matching Linux numbering).
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

// Private flag — on Linux, skips VMA lookup for process-private futexes.
// On Windows, wait/wake are always process-private; the flag is accepted
// but has no effect.
#define FUTEX_PRIVATE_FLAG 128

#define FUTEX_WAIT_PRIVATE (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)

#ifdef __cplusplus
extern "C" {
#endif

// futex — Linux-compatible compare-and-wait / wake on a 32-bit word.
//
// FUTEX_WAIT: if *uaddr == val, sleep until woken or timeout.
//   timeout: relative timespec, or NULL for infinite.
//   Returns 0 on wake, -1 on error (errno = EAGAIN, ETIMEDOUT, EINVAL).
//
// FUTEX_WAKE: wake up to val threads waiting on uaddr.
//   Returns number of threads woken.
//
// uaddr2 and val3 are accepted for signature compatibility but unused.
long futex(volatile uint32_t *uaddr, int futex_op, uint32_t val,
           const struct timespec *timeout, volatile uint32_t *uaddr2,
           uint32_t val3);

#ifdef __cplusplus
}
#endif

#endif // LLVM_LIBC_SYS_FUTEX_H
