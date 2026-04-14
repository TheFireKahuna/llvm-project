//===-- Windows implementation of freelocale -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/locale/freelocale.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/locale/locale.h"

namespace LIBC_NAMESPACE_DECL {

// free_locale() is defined in windows/newlocale.cpp (shares the slab pool).
void free_locale(locale_t loc);

LLVM_LIBC_FUNCTION(void, freelocale, (locale_t loc)) { free_locale(loc); }

} // namespace LIBC_NAMESPACE_DECL
