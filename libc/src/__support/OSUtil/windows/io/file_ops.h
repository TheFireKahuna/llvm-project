//===-- Internal file operation declarations -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: file operation functions that implement
// Linux syscall semantics (return value on success, -errno on failure).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_FILE_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_FILE_OPS_H

#include "hdr/types/off_t.h"
#include "src/__support/macros/config.h"
#include <stddef.h>
#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t dup3(int oldfd, int newfd, int flags);
intptr_t pread(int fd, void *buf, size_t count, off_t offset);
intptr_t pwrite(int fd, const void *buf, size_t count, off_t offset);
intptr_t fsync(int fd);
intptr_t fdatasync(int fd);
intptr_t ftruncate(int fd, off_t length);
intptr_t truncate(const char *path, off_t length);
intptr_t isatty(int fd);
intptr_t pipe2(int pipefd[2], int flags);

// copy_file_range — kernel-mode file-to-file copy via NtCopyFileChunk.
// Returns bytes copied on success, -errno on failure.
intptr_t copy_file_range(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
                         size_t len, unsigned int flags);

// Windows CRT compat — fd ↔ HANDLE interop.
// get_osfhandle returns the raw HANDLE as intptr_t, or -EBADF.
// open_osfhandle returns the allocated fd, or -errno.
intptr_t get_osfhandle(int fd);
intptr_t open_osfhandle(intptr_t osfhandle, int flags);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_FILE_OPS_H
