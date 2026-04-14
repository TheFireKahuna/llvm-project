//===-- Implementation of toupper_l ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/ctype/toupper_l.h"

#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/os.h"

#ifdef LIBC_TARGET_OS_IS_WINDOWS
#include "src/__support/OSUtil/windows/ntdll.h"
#else
#include "src/__support/CPP/limits.h"
#include "src/__support/ctype_utils.h"
#endif

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, toupper_l, (int c, locale_t)) {
#ifdef LIBC_TARGET_OS_IS_WINDOWS
  if (c < 0 || c > 0xFF)
    return c;
  return static_cast<int>(
      ::RtlUpcaseUnicodeChar(static_cast<WCHAR>(c)));
#else
  if (c < cpp::numeric_limits<char>::min() ||
      c > cpp::numeric_limits<char>::max()) {
    return c;
  }
  return static_cast<int>(internal::toupper(static_cast<char>(c)));
#endif
}

} // namespace LIBC_NAMESPACE_DECL
