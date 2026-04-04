//===-- Implementation of locale ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/locale/locale.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

__locale_t c_locale = {};

// Atomic global locale. nullptr means C locale (default).
static cpp::Atomic<locale_t> global_locale{nullptr};

locale_t get_global_locale() {
  return global_locale.load(cpp::MemoryOrder::ACQUIRE);
}

void set_global_locale(locale_t loc) {
  global_locale.store(loc, cpp::MemoryOrder::RELEASE);
}

} // namespace LIBC_NAMESPACE_DECL
