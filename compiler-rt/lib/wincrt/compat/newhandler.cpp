//===-- newhandler.cpp - MSVC new handler bridge to Itanium ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Bridges MSVC's _set_new_handler to std::set_new_handler (Itanium ABI).
//
// The MSVC and Itanium new handler models differ:
//
//   MSVC:    int handler(size_t) - receives size, returns 0=fail, nonzero=retry
//   Itanium: void handler()      - no size, throws bad_alloc or returns to retry
//
// This bridge allows legacy MSVC code using _set_new_handler to work with
// libc++. The MSVC handler receives size=0 since Itanium doesn't provide it.
//
// New code should use std::set_new_handler directly.
//
//===----------------------------------------------------------------------===//

#ifdef LLVM_RUNTIME_WIN32

#include "../internal.h"
#include <new>

// MSVC new handler: int handler(size_t) - 0=fail, nonzero=retry.
using _PNH = int(__cdecl *)(size_t);

namespace {

_PNH g_msvcHandler = nullptr;
std::new_handler g_savedItaniumHandler = nullptr;
void *g_lock = nullptr; // SRWLOCK, zero-initialized

[[noreturn]] void failAllocation() {
#if __cpp_exceptions
  throw std::bad_alloc();
#else
  std::terminate();
#endif
}

void adapterHandler() {
  if (g_msvcHandler) {
    if (g_msvcHandler(0) != 0)
      return; // Retry
    failAllocation();
  }
  if (g_savedItaniumHandler) {
    g_savedItaniumHandler();
    return;
  }
  failAllocation();
}

} // namespace

extern "C" {

/// MSVC-compatible new handler. Handler receives size=0 (Itanium limitation).
_PNH __cdecl _set_new_handler(_PNH handler) {
  AcquireSRWLockExclusive(&g_lock);

  _PNH old = g_msvcHandler;
  g_msvcHandler = handler;

  if (handler && !old)
    g_savedItaniumHandler = std::set_new_handler(adapterHandler);
  else if (!handler && old)
    std::set_new_handler(g_savedItaniumHandler);

  ReleaseSRWLockExclusive(&g_lock);
  return old;
}

/// Query MSVC-style handler. Does not return std::set_new_handler handlers.
_PNH __cdecl _query_new_handler(void) {
  AcquireSRWLockShared(&g_lock);
  _PNH h = g_msvcHandler;
  ReleaseSRWLockShared(&g_lock);
  return h;
}

} // extern "C"

#endif // LLVM_RUNTIME_WIN32
