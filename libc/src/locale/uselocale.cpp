//===-- Implementation of uselocale ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/locale/uselocale.h"
#include "hdr/locale_macros.h"
#include "src/locale/locale.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(locale_t, uselocale, (locale_t newloc)) {
  // Return the previous per-thread locale. If none was set, return
  // LC_GLOBAL_LOCALE to indicate the thread was using the global locale.
  locale_t prev = thread_locale ? thread_locale : LC_GLOBAL_LOCALE;

  if (newloc) {
    // LC_GLOBAL_LOCALE means "revert to using the global locale".
    thread_locale = (newloc == LC_GLOBAL_LOCALE) ? nullptr : newloc;
  }

  return prev;
}

} // namespace LIBC_NAMESPACE_DECL
