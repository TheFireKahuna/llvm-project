//===-- Windows implementation of setlocale --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/locale/setlocale.h"
#include "hdr/locale_macros.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/OSUtil/windows/nls_locale.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/locale/locale.h"
#include "src/locale/windows/locale_data.h"

namespace LIBC_NAMESPACE_DECL {

// Returned by setlocale(). Valid until the next setlocale() call (per POSIX).
static char locale_name_buf[64] = "C";

// Global locale storage. setlocale() writes here, then atomically swaps
// the global locale pointer to &global_locale_obj.
static WindowsLocaleData global_data[NUM_LOCALE_CATEGORIES] = {};
static __locale_t global_locale_obj = {};

static char *apply_locale(int category, const unsigned char *record) {
  int lo = (category == LC_ALL) ? 0 : category;
  int hi = (category == LC_ALL) ? NUM_LOCALE_CATEGORIES - 1 : category;

  for (int i = lo; i <= hi; i++) {
    global_data[i].nls_record = record;
    nls::nls_locale_name_utf8(record, global_data[i].name,
                              MAX_LOCALE_NAME_SIZE);
    global_locale_obj.data[i] = &global_data[i];
  }

  set_global_locale(&global_locale_obj);
  __builtin_memcpy(locale_name_buf, global_data[lo].name,
                   MAX_LOCALE_NAME_SIZE);
  return locale_name_buf;
}

LLVM_LIBC_FUNCTION(char *, setlocale,
                   (int category, const char *locale_name)) {
  if (category < 0 || category > LC_ALL)
    return nullptr;

  // Query: return current locale name.
  if (!locale_name) {
    locale_t current = get_current_locale();
    if (!current || current == &c_locale)
      return locale_name_buf;
    for (int i = 0; i < NUM_LOCALE_CATEGORIES; i++) {
      if (current->data[i] && current->data[i]->name[0]) {
        __builtin_memcpy(locale_name_buf, current->data[i]->name,
                         MAX_LOCALE_NAME_SIZE);
        return locale_name_buf;
      }
    }
    return locale_name_buf;
  }

  cpp::string_view name(locale_name);

  // "C" or "POSIX" -> revert to C locale.
  if (name == "C" || name == "POSIX") {
    set_global_locale(&c_locale);
    __builtin_memcpy(locale_name_buf, "C", 2);
    return locale_name_buf;
  }

  // "" -> system default, anything else -> NLS name lookup.
  const unsigned char *record = nls::nls_find_locale(locale_name);
  if (!record)
    return nullptr;

  return apply_locale(category, record);
}

} // namespace LIBC_NAMESPACE_DECL
