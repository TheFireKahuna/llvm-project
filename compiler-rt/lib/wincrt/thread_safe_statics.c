//===-- thread_safe_statics.c - MSVC's guarded local statics --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// An MSVC object initializes a local static once: its guard is 0 before, -1
// while one thread runs the initializer and then the epoch its completion
// took, from a counter that only grows from INT_MIN. Code skips the call
// where the guard is no greater than _Init_thread_epoch, the latest epoch the
// thread has seen, since every initializer of a smaller epoch completed
// before that epoch was taken. _Init_thread_epoch is a thread-local of the
// loader's template, so this file is compiled for the MSVC environment, as
// tlsdyn.c is.
//
//===----------------------------------------------------------------------===//

#define WIN32_LEAN_AND_MEAN
#include <limits.h>
#include <windows.h>

NTSYSAPI LONG NTAPI RtlWaitOnAddress(volatile void *Address,
                                     void *CompareAddress, SIZE_T Size,
                                     LARGE_INTEGER *Timeout);
NTSYSAPI void NTAPI RtlWakeAddressAll(void *Address);

enum { Uninitialized = 0, Running = -1 };

static int GlobalEpoch = INT_MIN;
__declspec(thread) int _Init_thread_epoch = INT_MIN;

// Returns with the guard Running, claimed for the caller to initialize, or
// holding an epoch, which the thread's epoch then covers.
void __cdecl _Init_thread_header(int *Guard) {
  int Value = __atomic_load_n(Guard, __ATOMIC_ACQUIRE);
  for (;;) {
    if (Value == Uninitialized) {
      if (__atomic_compare_exchange_n(Guard, &Value, Running, 0,
                                      __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
        return;
    } else if (Value == Running) {
      RtlWaitOnAddress(Guard, &Value, sizeof(int), NULL);
      Value = __atomic_load_n(Guard, __ATOMIC_ACQUIRE);
    } else {
      _Init_thread_epoch = __atomic_load_n(&GlobalEpoch, __ATOMIC_ACQUIRE);
      return;
    }
  }
}

// The epoch is taken after the initializer completes, so a thread that reads
// a later one has its effects; the guard's store is the linearization point.
void __cdecl _Init_thread_footer(int *Guard) {
  int Epoch = __atomic_add_fetch(&GlobalEpoch, 1, __ATOMIC_ACQ_REL);
  __atomic_store_n(Guard, Epoch, __ATOMIC_RELEASE);
  _Init_thread_epoch = Epoch;
  RtlWakeAddressAll(Guard);
}

// An initializer left by an exception: the next caller initializes.
void __cdecl _Init_thread_abort(int *Guard) {
  __atomic_store_n(Guard, Uninitialized, __ATOMIC_RELEASE);
  RtlWakeAddressAll(Guard);
}
