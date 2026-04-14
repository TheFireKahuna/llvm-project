//===-- Implementation of alphasort ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "alphasort.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/string/strcoll.h"

#include <dirent.h>

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, alphasort,
                   (const struct ::dirent **a, const struct ::dirent **b)) {
  return LIBC_NAMESPACE::strcoll((*a)->d_name, (*b)->d_name);
}

} // namespace LIBC_NAMESPACE_DECL
