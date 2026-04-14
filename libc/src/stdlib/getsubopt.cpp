//===-- Implementation of getsubopt ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdlib/getsubopt.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(int, getsubopt,
                    (char **optionp, char *const *keylistp, char **valuep)) {
  *valuep = nullptr;

  if (*optionp == nullptr || **optionp == '\0')
    return -1;

  // Find the end of this suboption (next comma or end of string).
  char *p = *optionp;
  while (*p != '\0' && *p != ',')
    ++p;

  // Null-terminate at comma and advance past it.
  if (*p == ',')
    *p++ = '\0';

  char *sub = *optionp;
  *optionp = p;

  // Split on '=' if present.
  cpp::string_view token(sub);
  size_t eq_pos = token.find_first_of('=');

  cpp::string_view key = (eq_pos == cpp::string_view::npos)
                              ? token
                              : token.substr(0, eq_pos);

  // Match against keylist.
  for (int i = 0; keylistp[i] != nullptr; ++i) {
    if (key == cpp::string_view(keylistp[i])) {
      if (eq_pos != cpp::string_view::npos)
        *valuep = sub + eq_pos + 1;
      return i;
    }
  }

  // No match — per POSIX, *valuep points to the entire token.
  *valuep = sub;
  return -1;
}

} // namespace LIBC_NAMESPACE_DECL
