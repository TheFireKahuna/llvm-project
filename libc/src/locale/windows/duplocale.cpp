//===-- Windows implementation of duplocale --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/locale/duplocale.h"
#include "hdr/locale_macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/locale/locale.h"
#include "src/locale/newlocale.h"
#include "src/locale/windows/locale_data.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(locale_t, duplocale, (locale_t loc)) {
  if (!loc)
    return nullptr;

  // C locale is a static singleton -- no copy needed.
  if (loc == &c_locale)
    return &c_locale;

  // LC_GLOBAL_LOCALE -> snapshot the current global locale.
  if (loc == LC_GLOBAL_LOCALE)
    loc = get_global_locale();
  if (!loc)
    return &c_locale;

  // Allocate a copy via newlocale with LC_ALL_MASK.
  // Find the first category with a record and duplicate from that.
  for (int i = 0; i < NUM_LOCALE_CATEGORIES; i++) {
    if (loc->data[i] && get_nls_record(loc->data[i])) {
      return newlocale(LC_ALL_MASK, loc->data[i]->name, nullptr);
    }
  }

  return &c_locale;
}

} // namespace LIBC_NAMESPACE_DECL
