//===-- backtrace / backtrace_symbols for NT-POSIX -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POSIX/glibc-compatible backtrace API implemented on NT primitives:
//   backtrace()            -- RtlCaptureStackBackTrace (single NT call)
//   backtrace_symbols()    -- RtlPcToFileHeader + PE export table walk
//   backtrace_symbols_fd() -- same, but writes to fd (no malloc)
//
// backtrace() and backtrace_symbols_fd() are async-signal-safe.
// backtrace_symbols() allocates via malloc so callers can release with free().
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEBUG_BACKTRACE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEBUG_BACKTRACE_H

#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Capture return addresses from the call stack. Returns the number of
// frames captured (up to `size`). buffer[0] is the caller's frame.
int posix_backtrace(void **buffer, int size);

// Resolve addresses to "module(symbol+0xoffset) [0xaddr]" strings.
// Returns a malloc'd array (single allocation: pointer array + string data).
// Caller must free() the returned pointer. Returns nullptr on failure.
char **posix_backtrace_symbols(void *const *buffer, int size);

// Write backtrace to a file descriptor. Async-signal-safe (no malloc).
void posix_backtrace_symbols_fd(void *const *buffer, int size, int fd);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_DEBUG_BACKTRACE_H
