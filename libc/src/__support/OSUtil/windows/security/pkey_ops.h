//===-- Internal pkey operation declarations ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECURITY_PKEY_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECURITY_PKEY_OPS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t pkey_alloc(unsigned int flags, unsigned int access_rights);
intptr_t pkey_free(int pkey);
intptr_t pkey_get(int pkey);
intptr_t pkey_set(int pkey, unsigned int access_rights);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SECURITY_PKEY_OPS_H
