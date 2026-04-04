//===-- Windows NTPOSIX implementation of clearenv -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/clearenv.h"
#include "src/__support/OSUtil/windows/io/env_ops.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, clearenv, ()) { return internal::env_clear(); }

} // namespace LIBC_NAMESPACE_DECL
