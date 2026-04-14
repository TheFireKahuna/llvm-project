//===-- Implementation header of inet_pton ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_ARPA_INET_INET_PTON_H
#define LLVM_LIBC_SRC_ARPA_INET_INET_PTON_H

#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

int inet_pton(int af, const char *__restrict src, void *__restrict dst);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_ARPA_INET_INET_PTON_H
