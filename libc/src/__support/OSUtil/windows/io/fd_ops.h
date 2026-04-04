//===-- Internal fd/file-control declarations ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Forward declarations for internal:: file-descriptor control functions
// implemented in fcntl.cpp. All follow Linux syscall semantics: return value
// on success, -errno on failure.
//
// This header breaks the include cycle that prevented fcntl.cpp's functions
// from having their own group header (fd_table -> osutil -> syscall cycle).
// Callers needing internal::open/close/lseek/dup2 should include this
// instead of relying on the forward declarations in syscall.h.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_FD_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_FD_OPS_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/mode_t.h"
#include "hdr/types/off_t.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t open(const char *path, int flags, mode_t mode);
intptr_t openat(int dirfd, const char *path, int flags, mode_t mode);
intptr_t close(int fd);
intptr_t lseek(int fd, off_t offset, int whence);
intptr_t dup(int oldfd);
intptr_t dup2(int oldfd, int newfd);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_FD_OPS_H
