//===--- Definition of Windows stderr --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// On Windows (NT-POSIX), standard streams are not backed by static File
// objects.  The fd_table startup code constructs WindowsFile instances in a
// pool and assigns them to these pointers during init_std_fds().  We define
// the storage here as nullptr; it is populated before main() runs.
//
//===----------------------------------------------------------------------===//

#include "src/stdio/stderr.h"

#include "hdr/types/FILE.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_VARIABLE(FILE *, stderr) = nullptr;

} // namespace LIBC_NAMESPACE_DECL
