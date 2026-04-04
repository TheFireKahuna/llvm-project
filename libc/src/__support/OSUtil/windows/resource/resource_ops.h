//===-- Internal resource limit declarations -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_RESOURCE_OPS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_RESOURCE_OPS_H

#include "hdr/stdint_proxy.h"
#include "src/__support/macros/config.h"

struct rlimit;
struct rusage;

namespace LIBC_NAMESPACE_DECL {
namespace internal {

intptr_t getrlimit(int resource, struct rlimit *lim);
intptr_t setrlimit(int resource, const struct rlimit *lim);
intptr_t prlimit(int pid, int resource, const struct rlimit *new_limit,
             struct rlimit *old_limit);
intptr_t getrusage(int who, struct rusage *usage);

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_RESOURCE_RESOURCE_OPS_H
