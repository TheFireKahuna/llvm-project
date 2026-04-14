//===-- Implementation header for get_mempolicy -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_SYS_MMAN_GET_MEMPOLICY_H
#define LLVM_LIBC_SRC_SYS_MMAN_GET_MEMPOLICY_H

#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

long get_mempolicy(int *mode, unsigned long *nodemask, unsigned long maxnode,
                   void *addr, unsigned long flags);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_SYS_MMAN_GET_MEMPOLICY_H
