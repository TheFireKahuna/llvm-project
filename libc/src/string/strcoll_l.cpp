//===-- Implementation of strcoll_l ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/string/strcoll_l.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/null_check.h"

#ifdef LIBC_TARGET_OS_IS_WINDOWS
#include "src/string/strcoll.h"
#include "src/locale/locale.h"
#endif

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, strcoll_l,
                   (const char *left, const char *right, locale_t loc)) {
  LIBC_CRASH_ON_NULLPTR(left);
  LIBC_CRASH_ON_NULLPTR(right);
#ifdef LIBC_TARGET_OS_IS_WINDOWS
  // Temporarily set the thread locale so strcoll_impl picks it up.
  locale_t prev = thread_locale;
  thread_locale = loc;
  int result = LIBC_NAMESPACE::strcoll(left, right);
  thread_locale = prev;
  return result;
#else
  (void)loc;
  // C locale: byte comparison (POSIX).
  for (; *left && *left == *right; ++left, ++right)
    ;
  return static_cast<int>(static_cast<unsigned char>(*left)) -
         static_cast<int>(static_cast<unsigned char>(*right));
#endif
}

} // namespace LIBC_NAMESPACE_DECL
