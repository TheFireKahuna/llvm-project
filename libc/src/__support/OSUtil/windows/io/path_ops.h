//===-- Forward declarations for Windows path operations --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_PATH_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_PATH_OPS_H

#include "hdr/stdint_proxy.h"
#include "hdr/types/ssize_t.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t faccessat(int dfd, const char *path, int amode, int flag);
intptr_t unlinkat(int dfd, const char *path, int flags);
intptr_t linkat(int olddfd, const char *oldpath, int newdfd, const char *newpath,
            int flags);
intptr_t symlinkat(const char *target, int newdfd, const char *linkpath);
intptr_t readlinkat(int dfd, const char *__restrict path, char *__restrict buf,
                size_t bufsiz);
intptr_t chdir(const char *path);
intptr_t fchdir(int fd);
intptr_t getcwd(char *buf, size_t size);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_IO_PATH_OPS_H
