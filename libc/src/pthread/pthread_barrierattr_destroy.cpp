//===-- Implementation of the pthread_barrierattr_destroy -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pthread_barrierattr_destroy.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, pthread_barrierattr_destroy,
                   (pthread_barrierattr_t * attr [[gnu::unused]])) {
  return 0;
}

} // namespace LIBC_NAMESPACE_DECL
