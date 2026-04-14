//===-- Implementation of nl_langinfo_l -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/locale/nl_langinfo_l.h"
#include "include/llvm-libc-macros/langinfo-macros.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/os.h"
#include "src/locale/locale.h"
#include "src/locale/nl_langinfo.h"

#ifdef LIBC_TARGET_OS_IS_WINDOWS
#include "src/__support/OSUtil/windows/nls_locale.h"
#include "src/locale/windows/locale_data.h"
#endif

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(char *, nl_langinfo_l, (nl_item item, locale_t loc)) {
  // nl_langinfo.cpp defines c_langinfo (static). For the _l variant, we
  // replicate the logic with the explicit locale.
  // TODO: Refactor to share the implementation with nl_langinfo().
#ifdef LIBC_TARGET_OS_IS_WINDOWS
  if (loc && loc != &c_locale) {
    const unsigned char *record = nullptr;
    for (int i = 0; i < NUM_LOCALE_CATEGORIES; i++) {
      if (loc->data[i] && get_nls_record(loc->data[i])) {
        record = get_nls_record(loc->data[i]);
        break;
      }
    }
    if (record) {
      // Delegate to nl_langinfo with the thread locale temporarily set.
      // This is safe because nl_langinfo reads the current locale.
      locale_t prev = thread_locale;
      thread_locale = loc;
      char *result = LIBC_NAMESPACE::nl_langinfo(item);
      thread_locale = prev;
      return result;
    }
  }
#else
  (void)loc;
#endif
  return LIBC_NAMESPACE::nl_langinfo(item);
}

} // namespace LIBC_NAMESPACE_DECL
