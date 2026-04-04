//===-- Definition of signgam global variable ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// POSIX requires signgam to be set by lgamma(). Initialized to 0;
// lgamma() will set it to +1 or -1 to indicate the sign of Gamma(x).
extern "C" {
LLVM_LIBC_VARIABLE_EXPORT int signgam = 0;
}

} // namespace LIBC_NAMESPACE_DECL
