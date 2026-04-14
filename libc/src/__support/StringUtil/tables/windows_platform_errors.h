//===-- Map of error numbers to strings for Windows ----- -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_STRINGUTIL_TABLES_WINDOWS_PLATFORM_ERRORS_H
#define LLVM_LIBC_SRC___SUPPORT_STRINGUTIL_TABLES_WINDOWS_PLATFORM_ERRORS_H

#include "src/__support/macros/config.h"
#include "posix_errors.h"
#include "stdc_errors.h"

namespace LIBC_NAMESPACE_DECL {

// NTPOSIX exposes the full POSIX errno namespace from llvm-libc's generic
// header. Reuse the canonical POSIX error tables so the Windows path stays in
// lockstep with the public errno.h surface.

LIBC_INLINE_VAR constexpr auto PLATFORM_ERRORS =
    STDC_ERRORS + POSIX_ERRORS;

LIBC_INLINE_VAR constexpr auto PLATFORM_ERRNO_NAMES =
    STDC_ERRNO_NAMES + POSIX_ERRNO_NAMES;

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_STRINGUTIL_TABLES_WINDOWS_PLATFORM_ERRORS_H
