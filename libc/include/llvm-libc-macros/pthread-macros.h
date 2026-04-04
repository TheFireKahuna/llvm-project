//===-- Definition of pthread macros --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_MACROS_PTHREAD_MACRO_H
#define LLVM_LIBC_MACROS_PTHREAD_MACRO_H

#define PTHREAD_NULL {0}

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_INHERIT_SCHED 0
#define PTHREAD_EXPLICIT_SCHED 1

#define PTHREAD_CANCEL_ENABLE 0
#define PTHREAD_CANCEL_DISABLE 1
#define PTHREAD_CANCEL_DEFERRED 0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_CANCELED ((void *)(long)-1)

// Cleanup handler macros. MUST be paired within the same lexical scope —
// the braces enforce this at the language level.
//
// __attribute__((cleanup)) ensures the frame is unlinked from the cleanup
// chain if the scope exits via C++ exception (not just via cancel/pop).
// Without this, a C++ exception unwinding past a cleanup_push scope would
// leave a dangling pointer in the chain. POSIX says this is UB, but
// __attribute__((cleanup)) makes it safe at zero runtime cost.
#define pthread_cleanup_push(routine, arg)                                     \
  {                                                                            \
    struct __pthread_cleanup_t __clframe                                        \
        __attribute__((__cleanup__(__pthread_cleanup_pop_noop)));               \
    __pthread_cleanup_push(&__clframe, (routine), (arg));

#define pthread_cleanup_pop(execute)                                           \
    __pthread_cleanup_pop(&__clframe, (execute));                              \
  }

#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_ERRORCHECK 1
#define PTHREAD_MUTEX_RECURSIVE 2
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL

#define PTHREAD_MUTEX_STALLED 0
#define PTHREAD_MUTEX_ROBUST 1

#define PTHREAD_BARRIER_SERIAL_THREAD -1

// pthread_once_t is an opaque futex-word wrapper, so use aggregate
// initialization just like C11's once_flag.
#define PTHREAD_ONCE_INIT {0}

#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED 1

#if defined(__NTPOSIX__)
#include "windows/pthread-macros.h"
#elif defined(__linux__) || defined(__APPLE__)
#define PTHREAD_MUTEX_INITIALIZER                                              \
  {                                                                            \
      /* .__timed = */ 0,      /* .__type = */ 0,                              \
      /* .__robust = */ 0,     /* .__owner = */ NULL,                          \
      /* .__lock_count = */ 0, /* .__ftxw = */ {0},                            \
  }
#endif

#if defined(__NTPOSIX__)
// Windows rwlock type uses opaque 56-byte storage.
#define PTHREAD_RWLOCK_INITIALIZER                                             \
  { {0} }
#else
#define PTHREAD_RWLOCK_INITIALIZER                                             \
  {                                                                            \
      /* .__raw = */ {                                                         \
          /* .__is_pshared = */ 0,                                             \
          /* .__preference = */ 0,                                             \
          /* .__state = */ 0,                                                  \
          /* .__wait_queue_mutex = */ {0},                                     \
          /* .__pending_readers = */ {0},                                      \
          /* .__pending_writers = */ {0},                                      \
          /* .__reader_serialization = */ {0},                                 \
          /* .__writer_serialization = */ {0},                                 \
      },                                                                       \
      /* .__write_tid = */ 0,                                                  \
  }
#endif

// POSIX requires at least 4 rounds of TSS destructor calls.
#define PTHREAD_DESTRUCTOR_ITERATIONS 4

// glibc extensions
#define PTHREAD_STACK_MIN (1 << 14) // 16KB
#define PTHREAD_RWLOCK_PREFER_READER_NP 0
#define PTHREAD_RWLOCK_PREFER_WRITER_NP 1
#define PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP 2

#endif // LLVM_LIBC_MACROS_PTHREAD_MACRO_H
