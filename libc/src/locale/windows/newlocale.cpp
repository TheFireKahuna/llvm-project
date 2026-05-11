//===-- Windows implementation of newlocale --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/locale/newlocale.h"
#include "hdr/errno_macros.h"
#include "hdr/locale_macros.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/OSUtil/windows/alloc/legacy/slab_pool.h"
#include "src/__support/OSUtil/windows/nls_locale.h"
#include "src/__support/common.h"
#include "src/__support/libc_errno.h"
#include "src/__support/macros/config.h"
#include "src/locale/locale.h"
#include "src/locale/windows/locale_data.h"

namespace LIBC_NAMESPACE_DECL {

// Combined allocation: __locale_t + inline WindowsLocaleData for each category.
// One slab slot per locale object, no secondary allocations.
struct LocaleAlloc {
  __locale_t obj;
  WindowsLocaleData categories[NUM_LOCALE_CATEGORIES];
};

static internal::SlabPool locale_pool;

static LocaleAlloc *alloc_locale() {
  // init() and init_tls() are both idempotent -- safe to call on every alloc.
  // Pool-managed TLS routes the slab through the FLS abandon callback at
  // thread exit, enabling sealed-with-live-slots Crystalline retire.
  constexpr size_t kSlotSize =
      (sizeof(LocaleAlloc) + alignof(LocaleAlloc) - 1) &
      ~(alignof(LocaleAlloc) - 1);
  locale_pool.init(kSlotSize, alignof(LocaleAlloc));
  locale_pool.init_tls(internal::kTlsCleanupPhaseAllocator);

  void *slot = locale_pool.tls_alloc();
  return static_cast<LocaleAlloc *>(slot);
}

void free_locale(locale_t loc) {
  if (!loc || loc == &c_locale)
    return;
  internal::SlabPool::free(loc);
}

LLVM_LIBC_FUNCTION(locale_t, newlocale,
                   (int category_mask, const char *locale_name, locale_t base)) {
  if (category_mask < 0) {
    libc_errno = EINVAL;
    return nullptr;
  }

  cpp::string_view name(locale_name);

  // "C" or "POSIX" -> return the static C locale (no allocation needed).
  if (name == "C" || name == "POSIX")
    return &c_locale;

  // Look up the NLS record for this locale name. `nls_find_locale` rejects
  // non-UTF-8 codeset suffixes (UTF-8-only locale profile).
  const unsigned char *record = nullptr;
  if (!name.empty()) {
    record = nls::nls_find_locale(locale_name);
    if (!record) {
      libc_errno = EINVAL;
      return nullptr;
    }
  } else {
    // "" -> system default.
    record = nls::nls_find_locale("");
    if (!record)
      return &c_locale;
  }

  // Allocate a new locale object.
  LocaleAlloc *alloc = alloc_locale();
  if (!alloc) {
    libc_errno = ENOMEM;
    return nullptr;
  }

  // Initialize: copy category data from base, or zero-fill for C locale.
  if (base && base != &c_locale && base->data[0]) {
    __builtin_memcpy(alloc->categories,
                     static_cast<const WindowsLocaleData *>(base->data[0]),
                     sizeof(alloc->categories));
  } else {
    __builtin_memset(alloc->categories, 0, sizeof(alloc->categories));
  }

  // Point obj.data[] at our inline categories (implicit upcast to base).
  for (int i = 0; i < NUM_LOCALE_CATEGORIES; i++)
    alloc->obj.data[i] = &alloc->categories[i];

  // Apply the new locale to the requested categories.
  for (int i = 0; i < NUM_LOCALE_CATEGORIES; i++) {
    if (category_mask == LC_ALL_MASK || (category_mask & (1 << i))) {
      alloc->categories[i].nls_record = record;
      nls::nls_locale_name_utf8(record, alloc->categories[i].name,
                                MAX_LOCALE_NAME_SIZE);
    }
  }

  return &alloc->obj;
}

} // namespace LIBC_NAMESPACE_DECL
