//===-- Internal stdio file lifecycle declarations --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: stdio file lifecycle functions that
// implement Linux syscall semantics (return 0 on success, -errno on failure).
// Functions that produce a FILE* use an output parameter.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_STDIO_FILE_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_STDIO_FILE_OPS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

// Forward declare FILE to avoid pulling in stdio.h.
struct FILE;

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Returns 0 on success, -errno on failure.
intptr_t fclose_impl(::FILE *stream);

// On success, writes *out = new FILE* and returns 0. On failure, returns -errno.
intptr_t fdopen_impl(int fd, const char *mode, ::FILE **out);

// On success, writes *out = reopened FILE* and returns 0. On failure, returns
// -errno. Note: freopen may return a different pointer than stream.
intptr_t freopen_impl(const char *path, const char *mode, ::FILE *stream,
                  ::FILE **out);

// On success, writes *out = new FILE* and returns 0. On failure, returns -errno.
intptr_t tmpfile_impl(::FILE **out);

// On success, writes *out = pipe FILE* and returns 0. On failure, returns
// -errno. The child process is spawned via posix_spawn and tracked internally
// so that pclose_impl can waitpid on it.
intptr_t popen_impl(const char *command, const char *mode, ::FILE **out);

// Closes the pipe stream and waits for the child process to exit.
// Returns the child's wait status on success, or -errno on failure.
intptr_t pclose_impl(::FILE *stream);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_STDIO_FILE_OPS_H
