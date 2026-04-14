//===-- Implementation header for mlock2 function ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_SYS_MMAN_MLOCK2_H
#define LLVM_LIBC_SRC_SYS_MMAN_MLOCK2_H

#include "hdr/types/size_t.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/os.h"

#ifdef LIBC_TARGET_OS_IS_LINUX
#include <sys/syscall.h>
#endif

namespace LIBC_NAMESPACE_DECL {

// mlock2 is available on Linux (via SYS_mlock2) and Windows (emulated).
#if defined(LIBC_TARGET_OS_IS_WINDOWS) || defined(SYS_mlock2)
int mlock2(const void *addr, size_t len, int flags);
#endif

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_SYS_MMAN_MLOCK2_H
